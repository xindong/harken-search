// AIEngine.cpp
// AI 音频处理引擎实现

#include "AIEngine.h"
// i18n-anchor (F2 -> H1b): task/result errorMessage -> AI panel task rows &
// transcription result toasts (main.cpp AI render sites). H1b (constraint 4):
// ExecuteTask/TranscribeAudio run on m_workerThread / detached threads, so
// these messages are carried as LedgerText key+args and L() is resolved on the
// UI thread at the render sites. Cancel/transient semantics untouched.

#include "Localization.h"   // L(id) for task type/status names (knife-B fix)
#include "platform/audio_file.h"
#include "platform/executable_path.h"
#include "platform/filesystem.h"
#include "platform/system.h"
#include "platform/text_encoding.h"

// Whisper.cpp
#include "whisper.h"
// 确保包含 AppState.h，因为我们需要访问 g_appState->database

#include <filesystem>
#include <fstream>
#include <cmath>
#include <algorithm>
#include <chrono>

namespace fs = std::filesystem;

// 辅助函数

const char* GetAITaskTypeName(AITaskType type) {
    switch (type) {
        case AITaskType::SpeechRecognition: return L(AITaskType_Asr);
        case AITaskType::SpeechEnhancement: return L(AITaskType_Enhance);
        case AITaskType::MonoToStereo:      return L(AITaskType_ToStereo);
        case AITaskType::AudioNormalize:    return L(AI_VolumeNormalize);
        case AITaskType::CoverGeneration:   return L(AICover_Title);
        default:                            return L(RenMode_Unknown);
    }
}

const char* GetAITaskStatusName(AITaskStatus status) {
    switch (status) {
        case AITaskStatus::Pending:    return L(AITaskStatus_Waiting);
        case AITaskStatus::Processing: return L(AITaskStatus_Processing);
        case AITaskStatus::Completed:  return L(AITaskStatus_Done);
        case AITaskStatus::Failed:     return L(Export_Failed);
        case AITaskStatus::Cancelled:  return L(Export_Cancelled);
        default:                       return L(RenMode_Unknown);
    }
}

// IsAudioFileSupported is defined in Utils.cpp (included via Utils.h) to allow
// lightweight test targets to use it without linking the full AIEngine.

std::string GetDefaultModelDir() {
#ifdef _WIN32
    const auto executable = Platform::GetExecutablePath();
    if (executable.status == Platform::ExecutablePathStatus::Success) {
        return (fs::u8path(executable.pathUtf8).parent_path() / "models").u8string();
    }
#endif
    return "./models";
}

static std::string ConvertTraditionalChineseToSimplified(const std::string& text) {
    return Platform::TextEncoding::ConvertTraditionalChineseToSimplified(text);
}

// Extract "message" from API error JSON like {"error":{"message":"..."}}
// Handles escaped quotes within the message value.
static std::string ExtractApiErrorMessage(const std::string& response) {
    size_t msgPos = response.find("\"message\"");
    if (msgPos == std::string::npos) return "";
    size_t colonPos = response.find(":", msgPos + 9);
    if (colonPos == std::string::npos) return "";
    size_t startQuote = response.find("\"", colonPos);
    if (startQuote == std::string::npos) return "";
    // Walk forward, skipping escaped quotes
    std::string result;
    for (size_t i = startQuote + 1; i < response.size(); i++) {
        if (response[i] == '\\' && i + 1 < response.size()) {
            result += response[i + 1]; // unescape
            i++;
        } else if (response[i] == '"') {
            break; // end of string value
        } else {
            result += response[i];
        }
    }
    return result;
}

// HTTPS POST 请求工具 (WinINet 实现，零第三方依赖)

static std::string SendHttpPost(const std::string& url, const std::string& headers, const std::string& postData, int* outHttpStatus = nullptr) {
#ifdef _WIN32
    std::string response;
    if (outHttpStatus) *outHttpStatus = 0;

    // 解析 URL (极其简易的解析，仅适用于 https://host/path 格式)
    std::string host, path;
    size_t hostStart = url.find("https://");
    if (hostStart != std::string::npos) {
        hostStart += 8;
        size_t pathStart = url.find("/", hostStart);
        if (pathStart != std::string::npos) {
            host = url.substr(hostStart, pathStart - hostStart);
            path = url.substr(pathStart);
        } else {
            host = url.substr(hostStart);
            path = "/";
        }
    } else {
        return ""; // 只支持 HTTPS
    }

    void* hInternet = Platform::OpenInternetSession(
        L"HarkenSearch/2.0", Platform::Internet::kOpenTypePreconfigured);
    if (!hInternet) return "";

    // Set timeouts: 30s connect, 60s send/receive
    uint32_t connectTimeout = 30000, sendRecvTimeout = 60000;
    Platform::SetInternetOption(hInternet, Platform::Internet::kOptionConnectTimeout, connectTimeout);
    Platform::SetInternetOption(hInternet, Platform::Internet::kOptionSendTimeout, sendRecvTimeout);
    Platform::SetInternetOption(hInternet, Platform::Internet::kOptionReceiveTimeout, sendRecvTimeout);

    void* hConnect = Platform::ConnectInternetHttp(
        hInternet, Platform::TextEncoding::Utf8ToWidePermissive(host), Platform::Internet::kDefaultHttpsPort);
    if (hConnect) {
        void* hRequest = Platform::OpenHttpRequest(
            hConnect, L"POST", Platform::TextEncoding::Utf8ToWidePermissive(path),
            Platform::Internet::kFlagSecure | Platform::Internet::kFlagReload);
        if (hRequest) {
            // 发送请求
            std::wstring wHeaders = Platform::TextEncoding::Utf8ToWidePermissive(headers);
            bool bSend = Platform::SendHttpRequest(
                hRequest, wHeaders, postData.data(), static_cast<uint32_t>(postData.length()));

            if (bSend) {
                // Retrieve HTTP status code
                if (outHttpStatus) {
                    uint32_t statusCode = 0;
                    if (Platform::QueryHttpInfoNumber(
                            hRequest,
                            Platform::Internet::kQueryStatusCode | Platform::Internet::kQueryFlagNumber,
                            statusCode)) {
                        *outHttpStatus = (int)statusCode;
                    }
                }
                // 读取响应
                char buffer[4096];
                uint32_t bytesRead = 0;
                while (Platform::ReadInternetFile(hRequest, buffer, sizeof(buffer) - 1, bytesRead) && bytesRead > 0) {
                    buffer[bytesRead] = '\0';
                    response += buffer;
                }
            }
            Platform::CloseInternetHandle(hRequest);
        }
        Platform::CloseInternetHandle(hConnect);
    }
    Platform::CloseInternetHandle(hInternet);
    return response;
#else
    if (outHttpStatus) *outHttpStatus = 0;
    // Linux/macOS 占位，之后可以用 libcurl 实现
    return "";
#endif
}

static bool DownloadImageToDisk(const std::string& url, const std::string& savePath) {
#ifdef _WIN32
    if (Platform::FileSystem::ProbePath(savePath) != Platform::FileSystem::PathStatus::Missing) {
        return false;
    }

    void* hInternet = Platform::OpenInternetSession(
        L"HarkenSearch/2.0", Platform::Internet::kOpenTypePreconfigured);
    if (!hInternet) return false;
    void* hUrl = Platform::OpenInternetUrl(
        hInternet, Platform::TextEncoding::Utf8ToWidePermissive(url), Platform::Internet::kFlagReload | Platform::Internet::kFlagSecure);
    if (!hUrl) { Platform::CloseInternetHandle(hInternet); return false; }

    const std::string partPath = savePath + ".part";
    Platform::FileSystem::StreamWriter writer;
    if (!writer.Open(partPath, false, true)) {
        Platform::CloseInternetHandle(hUrl);
        Platform::CloseInternetHandle(hInternet);
        Platform::FileSystem::RemoveFile(partPath);
        return false;
    }

    char buffer[8192];
    uint32_t bytesRead = 0;
    while (Platform::ReadInternetFile(hUrl, buffer, sizeof(buffer), bytesRead) && bytesRead > 0) {
        writer.Write(std::string_view(buffer, bytesRead));
    }
    writer.Close();
    Platform::CloseInternetHandle(hUrl);
    Platform::CloseInternetHandle(hInternet);

    if (!Platform::FileSystem::CopyFile(partPath, savePath, false)) {
        Platform::FileSystem::RemoveFile(partPath);
        return false;
    }
    Platform::FileSystem::RemoveFile(partPath);
    return true;
#else
    return false;
#endif
}

// 模型下载 URL 和大小

std::string AIEngine::GetModelUrl(const std::string& modelName) {
    // Hugging Face 模型仓库
    std::string base = "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/";
    
    if (modelName == "tiny" || modelName == "tiny.en") {
        return base + "ggml-" + modelName + ".bin";
    } else if (modelName == "base" || modelName == "base.en") {
        return base + "ggml-" + modelName + ".bin";
    } else if (modelName == "small" || modelName == "small.en") {
        return base + "ggml-" + modelName + ".bin";
    } else if (modelName == "medium" || modelName == "medium.en") {
        return base + "ggml-" + modelName + ".bin";
    } else if (modelName == "large" || modelName == "large-v1" || modelName == "large-v2" || modelName == "large-v3") {
        return base + "ggml-" + modelName + ".bin";
    }
    
    return base + "ggml-base.bin";  // 默认
}

int64_t AIEngine::GetModelSize(const std::string& modelName) {
    // 近似文件大小 (字节)
    if (modelName == "tiny" || modelName == "tiny.en") return 75LL * 1024 * 1024;      // ~75 MB
    if (modelName == "base" || modelName == "base.en") return 142LL * 1024 * 1024;     // ~142 MB
    if (modelName == "small" || modelName == "small.en") return 466LL * 1024 * 1024;   // ~466 MB
    if (modelName == "medium" || modelName == "medium.en") return 1500LL * 1024 * 1024; // ~1.5 GB
    if (modelName.find("large") != std::string::npos) return 3000LL * 1024 * 1024;      // ~3 GB
    return 142LL * 1024 * 1024;  // 默认 base
}

// AIEngine 实现

AIEngine::AIEngine() {
#ifdef _WIN32
    const std::string tempRoot = Platform::FileSystem::TemporaryDirectory();
    m_config.tempDir = tempRoot.empty()
        ? "Airs_AI\\"
        : (fs::u8path(tempRoot) / "Airs_AI").u8string() + "\\";
#else
    m_config.tempDir = "/tmp/Airs_AI/";
#endif
}

AIEngine::~AIEngine() {
    Shutdown();
}

bool AIEngine::Initialize(const AIEngineConfig& config) {
    if (m_initialized) {
        Shutdown();
    }
    
    m_config = config;
    
    // 创建临时目录
    try {
        Platform::FileSystem::CreateDirectories(m_config.tempDir);
    } catch (...) {}
    
    // 创建模型目录
    if (m_config.modelPath.empty()) {
        m_config.modelPath = GetDefaultModelDir();
    }
    try {
        Platform::FileSystem::CreateDirectories(m_config.modelPath);
    } catch (...) {}
    
    // 启动工作线程
    m_taskQueue.Reset();
    m_workerThread = std::thread([this]() {
        ProcessTaskQueue();
    });
    
   // Begin model loading without blocking initialization.
   m_initialized = true;
    
   // Load the configured model on a background thread so initialization
   // remains responsive.
   std::string defaultModelPath = m_config.modelPath + "/ggml-" + m_config.modelName + ".bin";
   
   if (Platform::FileSystem::Exists(defaultModelPath)) {
       // Run model loading outside the task-processing worker.
       std::thread([this, defaultModelPath]() {
           this->LoadModel(defaultModelPath);
           // LoadModel publishes readiness through m_modelLoaded.
       }).detach();
   }
   
   return true;
}
void AIEngine::Shutdown() {
    if (!m_initialized) return;
    
    m_taskQueue.RequestStop();
    
    if (m_workerThread.joinable()) {
        m_workerThread.join();
    }
    
    UnloadModel();
    
    m_initialized = false;
}

bool AIEngine::LoadModel(const std::string& modelPath) {
    std::lock_guard<std::mutex> lock(m_whisperMutex);
    
    // 卸载现有模型
    if (m_whisperCtx) {
        whisper_free(m_whisperCtx);
        m_whisperCtx = nullptr;
    }
    m_modelPath.clear();
    m_modelLoaded = false;
    
    // 检查文件是否存在
    if (!Platform::FileSystem::Exists(modelPath)) {
        return false;
    }
    
    // 加载模型
    whisper_context_params cparams = whisper_context_default_params();
    cparams.use_gpu = m_config.useGpu;
    
    m_whisperCtx = whisper_init_from_file_with_params(modelPath.c_str(), cparams);
    
    if (m_whisperCtx) {
        m_modelPath = modelPath;
        m_modelLoaded = true;
        return true;
    }
    
    return false;
}

std::string AIEngine::GetModelPath() const {
    std::lock_guard<std::mutex> lock(m_whisperMutex);
    return m_modelPath;
}

void AIEngine::UnloadModel() {
    std::lock_guard<std::mutex> lock(m_whisperMutex);
    
    if (m_whisperCtx) {
        whisper_free(m_whisperCtx);
        m_whisperCtx = nullptr;
    }
    
    m_modelPath.clear();
    m_modelLoaded = false;
}

bool AIEngine::DownloadModel(const std::string& modelName, const std::string& savePath,
                             std::function<void(float)> progressCallback) {
#ifdef _WIN32
    std::string url = GetModelUrl(modelName);
    std::string targetPath = savePath.empty() ? 
        (m_config.modelPath + "/ggml-" + modelName + ".bin") : savePath;
    
    // 创建目录
    fs::path parentDir = fs::path(targetPath).parent_path();
    try {
        Platform::FileSystem::CreateDirectories(parentDir.u8string());
    } catch (...) {}
    
    // 使用 WinINet 下载
    void* hInternet = Platform::OpenInternetSession(
        L"HarkenSearch/1.0", Platform::Internet::kOpenTypePreconfigured);
    if (!hInternet) return false;
    
    std::wstring wUrl = Platform::TextEncoding::Utf8ToWidePermissive(url);
    void* hUrl = Platform::OpenInternetUrl(
        hInternet, wUrl, Platform::Internet::kFlagReload | Platform::Internet::kFlagNoCacheWrite);
    if (!hUrl) {
        Platform::CloseInternetHandle(hInternet);
        return false;
    }
    
    // 获取文件大小
    uint32_t contentLength = 0;
    Platform::QueryHttpInfoNumber(
        hUrl, Platform::Internet::kQueryContentLength | Platform::Internet::kQueryFlagNumber,
        contentLength);
    
    // 打开输出文件
    void* hFile = Platform::OpenExclusiveWriteFile(targetPath);
    if (!hFile) {
        Platform::CloseInternetHandle(hUrl);
        Platform::CloseInternetHandle(hInternet);
        return false;
    }
    
    // 下载
    char buffer[8192];
    uint32_t bytesRead = 0;
    uint32_t totalRead = 0;
    
    while (Platform::ReadInternetFile(hUrl, buffer, sizeof(buffer), bytesRead) && bytesRead > 0) {
        Platform::WriteFileBytes(hFile, buffer, bytesRead);
        totalRead += bytesRead;
        
        if (progressCallback && contentLength > 0) {
            progressCallback((float)totalRead / (float)contentLength);
        }
    }
    
    Platform::CloseFileHandle(hFile);
    Platform::CloseInternetHandle(hUrl);
    Platform::CloseInternetHandle(hInternet);

    if (contentLength > 0 && totalRead != contentLength) {
        Platform::FileSystem::RemoveFile(targetPath);
        return false;
    }

    if (progressCallback && totalRead > 0) {
        progressCallback(1.0f);
    }
    return totalRead > 0;
#else
    // Linux/macOS: 使用 curl
    std::string url = GetModelUrl(modelName);
    std::string targetPath = savePath.empty() ? 
        (m_config.modelPath + "/ggml-" + modelName + ".bin") : savePath;
    
    std::string cmd = "curl -L -o \"" + targetPath + "\" \"" + url + "\"";
    return system(cmd.c_str()) == 0;
#endif
}

// 音频加载

bool AIEngine::LoadAudioFile(const std::string& path, std::vector<float>& pcmData) {
    Platform::AudioFile::MonoFloatReader reader;
    if (reader.Open(
            path,
            Platform::AudioFile::LegacyWindowsPathPolicy::FeatureLegacyUtf8VerbatimPrefix,
            Platform::AudioFile::MonoSampleRate::Hz16000) != Platform::AudioFile::Status::Ok) {
        return false;
    }

    pcmData.clear();
    std::vector<float> chunk(4096);
    while (true) {
        const auto read = reader.Read(chunk.data(), chunk.size());
        if (read.framesRead > 0) {
            pcmData.insert(
                pcmData.end(), chunk.begin(),
                chunk.begin() + static_cast<std::ptrdiff_t>(read.framesRead));
        }
        if (read.status == Platform::AudioFile::Status::EndOfFile ||
            read.status == Platform::AudioFile::Status::DecodeFailed ||
            read.status == Platform::AudioFile::Status::InvalidArgument ||
            read.framesRead == 0) {
            break;
        }
    }
    return !pcmData.empty();
}

bool AIEngine::SaveAudioFile(const std::string& path, const std::vector<float>& pcmData,
                              int sampleRate, int channels) {
    Platform::AudioFile::FloatWavWriter writer;
    if (!writer.Open(path, sampleRate, channels)) return false;

    Platform::AudioFile::FrameCount framesWritten = 0;
    writer.Write(
        pcmData.data(), pcmData.size() / static_cast<std::size_t>(channels),
        &framesWritten);
    writer.Close();

    return framesWritten > 0;
}

// 语音识别

TranscriptionResult AIEngine::Transcribe(const std::string& audioPath,
                                          const std::string& language,
                                          const std::string& initialPrompt,
                                          bool forceSimplifiedChinese) {
    TranscriptionResult result;

    if (!m_modelLoaded) {
        result.errorMessage = "Model not loaded";
        return result;
    }

    std::vector<float> pcmData;
    if (!LoadAudioFile(audioPath, pcmData)) {
        result.errorMessage = "Failed to load audio file";
        return result;
    }

    return TranscribeFromPCM(pcmData, language, initialPrompt, forceSimplifiedChinese);
}

TranscriptionResult AIEngine::TranscribeFromPCM(const std::vector<float>& pcmData,
                                                 const std::string& language,
                                                 const std::string& initialPrompt,
                                                 bool forceSimplifiedChinese) {
    TranscriptionResult result;

    if (!m_modelLoaded) {
        result.errorMessage = "Model not loaded";
        return result;
    }

    if (pcmData.empty()) {
        result.errorMessage = "Audio data is empty";
        return result;
    }

    std::lock_guard<std::mutex> lock(m_whisperMutex);
    if (!m_whisperCtx) {
        result.errorMessage = "Model not loaded";
        return result;
    }

    // Configure whisper parameters.
    whisper_full_params wparams = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);

    wparams.print_progress   = false;
    wparams.print_special    = false;
    wparams.print_realtime   = false;
    wparams.print_timestamps = false;
    wparams.translate        = false;
    wparams.single_segment   = false;
    wparams.max_tokens       = 0;
    wparams.n_threads        = m_config.maxThreads;
    wparams.initial_prompt   = initialPrompt.empty() ? nullptr : initialPrompt.c_str();
    wparams.carry_initial_prompt = !initialPrompt.empty();
    
    // 设置语言
    std::string lang = language.empty() ? m_config.defaultLanguage : language;
    if (!lang.empty() && lang != "auto") {
        wparams.language = lang.c_str();
    }
    
    // 执行转录
    if (whisper_full(m_whisperCtx, wparams, pcmData.data(), (int)pcmData.size()) != 0) {
        result.errorMessage.setKey(LStr::AI_TranscribeFail);
        return result;
    }
    
    // 提取结果
    int nSegments = whisper_full_n_segments(m_whisperCtx);
    
    for (int i = 0; i < nSegments; ++i) {
        const char* text = whisper_full_get_segment_text(m_whisperCtx, i);
        int64_t t0 = whisper_full_get_segment_t0(m_whisperCtx, i);
        int64_t t1 = whisper_full_get_segment_t1(m_whisperCtx, i);
        
        TranscriptionSegment seg;
        seg.text = text ? text : "";
        if (forceSimplifiedChinese) {
            seg.text = ConvertTraditionalChineseToSimplified(seg.text);
        }
        seg.startMs = t0 * 10;  // whisper 时间单位是 10ms
        seg.endMs = t1 * 10;
        seg.confidence = 0.9f;  // whisper 不直接提供置信度
        
        result.segments.push_back(seg);
        result.fullText += seg.text;
    }
    
    // 检测语言
    result.language = whisper_lang_str(whisper_full_lang_id(m_whisperCtx));
    if (forceSimplifiedChinese) {
        result.fullText = ConvertTraditionalChineseToSimplified(result.fullText);
    }
    result.success = true;
    
    return result;
}

int AIEngine::TranscribeAsync(const std::string& audioPath,
                               const std::string& language,
                               AITaskCallback callback,
                               const std::string& initialPrompt,
                               bool forceSimplifiedChinese) {
    int taskId = CreateTask(AITaskType::SpeechRecognition, audioPath);
    
    {
        std::lock_guard<std::mutex> lock(m_tasksMutex);
        auto it = m_tasks.find(taskId);
        if (it != m_tasks.end()) {
            it->second.language = language;
            it->second.initialPrompt = initialPrompt;
            it->second.forceSimplifiedChinese = forceSimplifiedChinese;
        }
    }
    
    // 启动回调监控
    if (callback) {
        std::thread([this, taskId, callback]() {
            while (true) {
                AITask task = GetTask(taskId);
                if (task.status == AITaskStatus::Completed ||
                    task.status == AITaskStatus::Failed ||
                    task.status == AITaskStatus::Cancelled) {
                    callback(task);
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }).detach();
    }
    
    return taskId;
}

// 音频处理

void AIEngine::ApplyNoiseReduction(std::vector<float>& pcmData, int sampleRate) {
    if (pcmData.empty()) return;
    
    // 简单的频谱减法降噪
    // 1. 估计噪声 (取前 0.5 秒的平均能量)
    int noiseFrames = std::min((int)(0.5f * sampleRate), (int)pcmData.size());
    float noiseLevel = 0.0f;
    for (int i = 0; i < noiseFrames; ++i) {
        noiseLevel += std::abs(pcmData[i]);
    }
    noiseLevel /= noiseFrames;
    
    // 2. 应用软阈值
    float threshold = noiseLevel * 1.5f;
    for (auto& sample : pcmData) {
        float abs_val = std::abs(sample);
        if (abs_val < threshold) {
            sample *= (abs_val / threshold) * 0.5f;
        }
    }
}

void AIEngine::ConvertMonoToStereo(const std::vector<float>& mono, std::vector<float>& stereo) {
    stereo.resize(mono.size() * 2);
    
    // 简单的伪立体声 - 左右声道略有时差
    int delay = 8;  // ~0.5ms @ 16kHz
    
    for (size_t i = 0; i < mono.size(); ++i) {
        stereo[i * 2] = mono[i];  // 左声道
        
        // 右声道稍微延迟
        if (i >= (size_t)delay) {
            stereo[i * 2 + 1] = mono[i - delay] * 0.95f;
        } else {
            stereo[i * 2 + 1] = mono[i] * 0.95f;
        }
    }
}

void AIEngine::NormalizePCM(std::vector<float>& pcmData, float targetDb) {
    if (pcmData.empty()) return;
    
    // 找到峰值
    float maxPeak = 0.0f;
    for (const auto& sample : pcmData) {
        maxPeak = std::max(maxPeak, std::abs(sample));
    }
    
    if (maxPeak < 0.0001f) return;
    
    // 计算增益
    float targetLinear = std::pow(10.0f, targetDb / 20.0f);
    float gain = targetLinear / maxPeak;
    
    // 应用增益
    for (auto& sample : pcmData) {
        sample *= gain;
    }
}

int AIEngine::EnhanceAudio(const std::string& inputPath,
                           const std::string& outputPath,
                           AITaskCallback callback) {
    int taskId = CreateTask(AITaskType::SpeechEnhancement, inputPath);
    
    {
        std::lock_guard<std::mutex> lock(m_tasksMutex);
        auto it = m_tasks.find(taskId);
        if (it != m_tasks.end()) {
            it->second.outputPath = outputPath.empty() ? 
                GenerateOutputPath(inputPath, "_enhanced") : outputPath;
        }
    }
    
    if (callback) {
        std::thread([this, taskId, callback]() {
            while (true) {
                AITask task = GetTask(taskId);
                if (task.status == AITaskStatus::Completed ||
                    task.status == AITaskStatus::Failed ||
                    task.status == AITaskStatus::Cancelled) {
                    callback(task);
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }).detach();
    }
    
    return taskId;
}

int AIEngine::ConvertToStereo(const std::string& inputPath,
                              const std::string& outputPath,
                              AITaskCallback callback) {
    int taskId = CreateTask(AITaskType::MonoToStereo, inputPath);
    
    {
        std::lock_guard<std::mutex> lock(m_tasksMutex);
        auto it = m_tasks.find(taskId);
        if (it != m_tasks.end()) {
            it->second.outputPath = outputPath.empty() ? 
                GenerateOutputPath(inputPath, "_stereo") : outputPath;
        }
    }
    
    if (callback) {
        std::thread([this, taskId, callback]() {
            while (true) {
                AITask task = GetTask(taskId);
                if (task.status == AITaskStatus::Completed ||
                    task.status == AITaskStatus::Failed ||
                    task.status == AITaskStatus::Cancelled) {
                    callback(task);
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }).detach();
    }
    
    return taskId;
}

int AIEngine::NormalizeAudio(const std::string& inputPath,
                             const std::string& outputPath,
                             float targetDb,
                             AITaskCallback callback) {
    int taskId = CreateTask(AITaskType::AudioNormalize, inputPath);
    
    {
        std::lock_guard<std::mutex> lock(m_tasksMutex);
        auto it = m_tasks.find(taskId);
        if (it != m_tasks.end()) {
            it->second.outputPath = outputPath.empty() ? 
                GenerateOutputPath(inputPath, "_normalized") : outputPath;
        }
    }
    
    if (callback) {
        std::thread([this, taskId, callback]() {
            while (true) {
                AITask task = GetTask(taskId);
                if (task.status == AITaskStatus::Completed ||
                    task.status == AITaskStatus::Failed ||
                    task.status == AITaskStatus::Cancelled) {
                    callback(task);
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }).detach();
    }
    
    return taskId;
}

// 任务管理

int AIEngine::CreateTask(AITaskType type, const std::string& inputPath) {
    AITask task;
    task.id = m_nextTaskId++;
    task.type = type;
    task.status = AITaskStatus::Pending;
    task.inputPath = inputPath;
    
    {
        std::lock_guard<std::mutex> lock(m_tasksMutex);
        m_tasks[task.id] = task;
    }

    m_taskQueue.Push(task.id);
    return task.id;
}

int AIEngine::CreateCoverTaskWithoutQueue(const std::string& inputPath) {
    AITask task;
    task.id = m_nextTaskId++;
    task.type = AITaskType::CoverGeneration;
    task.status = AITaskStatus::Pending;
    task.inputPath = inputPath;
    std::lock_guard<std::mutex> lock(m_tasksMutex);
    m_tasks[task.id] = task;
    return task.id;
}

void AIEngine::UpdateTaskStatus(int taskId, AITaskStatus status, const std::string& error) {
    std::lock_guard<std::mutex> lock(m_tasksMutex);
    auto it = m_tasks.find(taskId);
    if (it != m_tasks.end()) {
        it->second.status = status;
        it->second.errorMessage = error;
    }
}

// H1b (constraint 4): worker-context overload stores the ledger KEY; it must
// not render and must not fire m_progressCallback (that callback contract takes
// a pre-rendered string; it has zero registrants today).
void AIEngine::UpdateTaskProgress(int taskId, float progress, LStr statusKey) {
    std::lock_guard<std::mutex> lock(m_tasksMutex);
    auto it = m_tasks.find(taskId);
    if (it != m_tasks.end()) {
        it->second.progress = progress;
        it->second.statusKey = statusKey;
        it->second.statusKeyed = true;
    }
}

void AIEngine::UpdateTaskProgress(int taskId, float progress, const std::string& status) {
    {
        std::lock_guard<std::mutex> lock(m_tasksMutex);
        auto it = m_tasks.find(taskId);
        if (it != m_tasks.end()) {
            it->second.progress = progress;
        }
    }
    
    if (m_progressCallback) {
        m_progressCallback(taskId, progress, status);
    }
}

AITask AIEngine::GetTask(int taskId) const {
    std::lock_guard<std::mutex> lock(m_tasksMutex);
    auto it = m_tasks.find(taskId);
    return (it != m_tasks.end()) ? it->second : AITask{};
}

std::vector<AITask> AIEngine::GetAllTasks() const {
    std::lock_guard<std::mutex> lock(m_tasksMutex);
    std::vector<AITask> result;
    for (const auto& pair : m_tasks) {
        result.push_back(pair.second);
    }
    return result;
}

std::vector<AITask> AIEngine::GetPendingTasks() const {
    std::lock_guard<std::mutex> lock(m_tasksMutex);
    std::vector<AITask> result;
    for (const auto& pair : m_tasks) {
        if (pair.second.status == AITaskStatus::Pending ||
            pair.second.status == AITaskStatus::Processing) {
            result.push_back(pair.second);
        }
    }
    return result;
}

bool AIEngine::CancelTask(int taskId) {
    std::lock_guard<std::mutex> lock(m_tasksMutex);
    auto it = m_tasks.find(taskId);
    if (it != m_tasks.end() && it->second.status == AITaskStatus::Pending) {
        it->second.status = AITaskStatus::Cancelled;
        return true;
    }
    return false;
}

void AIEngine::CancelAllTasks() {
    std::lock_guard<std::mutex> lock(m_tasksMutex);
    for (auto& pair : m_tasks) {
        if (pair.second.status == AITaskStatus::Pending) {
            pair.second.status = AITaskStatus::Cancelled;
        }
    }
}

void AIEngine::ClearCompletedTasks() {
    std::lock_guard<std::mutex> lock(m_tasksMutex);
    for (auto it = m_tasks.begin(); it != m_tasks.end(); ) {
        if (it->second.status == AITaskStatus::Completed ||
            it->second.status == AITaskStatus::Failed ||
            it->second.status == AITaskStatus::Cancelled) {
            it = m_tasks.erase(it);
        } else {
            ++it;
        }
    }
}

void AIEngine::SetProgressCallback(AIProgressCallback callback) {
    m_progressCallback = callback;
}

// 工作线程

void AIEngine::ProcessTaskQueue() {
    for (;;) {
        int taskId = -1;
        if (!m_taskQueue.WaitForTask(taskId)) break;
        if (taskId >= 0) {
            AITask task;
            bool hasTask = false;
            {
                std::lock_guard<std::mutex> lock(m_tasksMutex);
                auto it = m_tasks.find(taskId);
                if (it != m_tasks.end() && it->second.status == AITaskStatus::Pending) {
                    it->second.status = AITaskStatus::Processing;
                    task = it->second;
                    hasTask = true;
                }
            }

            if (hasTask) {
                ExecuteTask(task);
                std::lock_guard<std::mutex> lock(m_tasksMutex);
                auto it = m_tasks.find(taskId);
                if (it != m_tasks.end()) {
                    it->second = task;
                }
            }
        }
    }
}

void AIEngine::ExecuteTask(AITask& task) {
    UpdateTaskProgress(task.id, 0.1f, LStr::AI_ProgStart);
    
    switch (task.type) {
        case AITaskType::SpeechRecognition: {
            UpdateTaskProgress(task.id, 0.2f, LStr::AI_ProgLoadAudio);
            task.transcription = Transcribe(task.inputPath,
                                            task.language,
                                            task.initialPrompt,
                                            task.forceSimplifiedChinese);
            task.status = task.transcription.success ? AITaskStatus::Completed : AITaskStatus::Failed;
            task.errorMessage = task.transcription.errorMessage;
            break;
        }
        
        case AITaskType::SpeechEnhancement: {
            UpdateTaskProgress(task.id, 0.2f, LStr::AI_ProgLoadAudio);
            std::vector<float> pcmData;
            if (!LoadAudioFile(task.inputPath, pcmData)) {
                task.status = AITaskStatus::Failed;
                task.errorMessage.setKey(LStr::AI_LoadAudioFail);
                break;
            }
            
            UpdateTaskProgress(task.id, 0.5f, LStr::AI_ProgDenoise);
            ApplyNoiseReduction(pcmData, WHISPER_SAMPLE_RATE);
            
            UpdateTaskProgress(task.id, 0.8f, LStr::AI_ProgSave);
            if (SaveAudioFile(task.outputPath, pcmData, WHISPER_SAMPLE_RATE, 1)) {
                task.audioResult.outputPath = task.outputPath;
                task.audioResult.success = true;
                task.status = AITaskStatus::Completed;
            } else {
                task.status = AITaskStatus::Failed;
                task.errorMessage.setKey(LStr::AI_SaveFileFail);
            }
            break;
        }
        
        case AITaskType::MonoToStereo: {
            UpdateTaskProgress(task.id, 0.2f, LStr::AI_ProgLoadAudio);
            std::vector<float> mono;
            if (!LoadAudioFile(task.inputPath, mono)) {
                task.status = AITaskStatus::Failed;
                task.errorMessage.setKey(LStr::AI_LoadAudioFail);
                break;
            }
            
            UpdateTaskProgress(task.id, 0.5f, LStr::AI_ProgToStereo);
            std::vector<float> stereo;
            ConvertMonoToStereo(mono, stereo);
            
            UpdateTaskProgress(task.id, 0.8f, LStr::AI_ProgSave);
            if (SaveAudioFile(task.outputPath, stereo, WHISPER_SAMPLE_RATE, 2)) {
                task.audioResult.outputPath = task.outputPath;
                task.audioResult.success = true;
                task.status = AITaskStatus::Completed;
            } else {
                task.status = AITaskStatus::Failed;
                task.errorMessage.setKey(LStr::AI_SaveFileFail);
            }
            break;
        }
        
        case AITaskType::AudioNormalize: {
            UpdateTaskProgress(task.id, 0.2f, LStr::AI_ProgLoadAudio);
            std::vector<float> pcmData;
            if (!LoadAudioFile(task.inputPath, pcmData)) {
                task.status = AITaskStatus::Failed;
                task.errorMessage.setKey(LStr::AI_LoadAudioFail);
                break;
            }
            
            UpdateTaskProgress(task.id, 0.5f, LStr::AI_ProgNormalize);
            NormalizePCM(pcmData, -1.0f);
            
            UpdateTaskProgress(task.id, 0.8f, LStr::AI_ProgSave);
            if (SaveAudioFile(task.outputPath, pcmData, WHISPER_SAMPLE_RATE, 1)) {
                task.audioResult.outputPath = task.outputPath;
                task.audioResult.success = true;
                task.status = AITaskStatus::Completed;
            } else {
                task.status = AITaskStatus::Failed;
                task.errorMessage.setKey(LStr::AI_SaveFileFail);
            }
            break;
        }
        
        case AITaskType::CoverGeneration: {
            if (Platform::FileSystem::IsNetworkPath(task.inputPath)) {
                task.status = AITaskStatus::Failed;
                task.errorMessage.setKey(LStr::Write_BlockedNetworkLibrary);
                break;
            }
            task.status = AITaskStatus::Failed;
            task.errorMessage.setKey(LStr::AI_CoverPathBusy);
            break;
        }
    }
    
    task.progress = 1.0f;
    UpdateTaskProgress(task.id, 1.0f, LStr::Done);  // H1b-fix: completion status keyed too (was old overload, off-thread render)
}

std::string AIEngine::GenerateOutputPath(const std::string& inputPath, const std::string& suffix) {
    fs::path input(inputPath);
    std::string stem = input.stem().string();
    std::string ext = input.extension().string();
    return m_config.tempDir + stem + suffix + ext;
}

// 批量处理

static bool ChooseUnusedCoverJpegPath(const std::string& targetFolder, std::string& outPath) {
    if (Platform::FileSystem::IsNetworkPath(targetFolder)) {
        return false; // L(Write_BlockedNetworkLibrary)
    }
     const std::string sep =
        (!targetFolder.empty() && (targetFolder.back() == '/' || targetFolder.back() == '\\'))
            ? ""
            : "\\";
    for (int n = 1; n < 10000; ++n) {
        const std::string candidate = (n == 1)
            ? (targetFolder + sep + "cover.jpg")
            : (targetFolder + sep + "cover-" + std::to_string(n) + ".jpg");
        const auto status = Platform::FileSystem::ProbePath(candidate);
        if (status == Platform::FileSystem::PathStatus::Missing) {
            outPath = candidate;
            return true;
        }
        if (status == Platform::FileSystem::PathStatus::Present) {
            continue;
        }
        if (status == Platform::FileSystem::PathStatus::Error) {
            return false;
        }
    }
    return false;
}

int AIEngine::GenerateCoverAsync(const std::string& targetFolder, const std::string& prompt, AITaskCallback callback) {
    int taskId = -1;
    if (Platform::FileSystem::IsNetworkPath(targetFolder)) {
        taskId = CreateCoverTaskWithoutQueue(targetFolder);
    } else {
        taskId = CreateTask(AITaskType::CoverGeneration, targetFolder);
    }

     {
         std::lock_guard<std::mutex> lock(m_tasksMutex);
         auto it = m_tasks.find(taskId);
         if (it != m_tasks.end()) {
             it->second.prompt = prompt;
            if (Platform::FileSystem::IsNetworkPath(targetFolder)) {
                it->second.status = AITaskStatus::Failed;
                it->second.errorMessage.setKey(LStr::Write_BlockedNetworkLibrary);
            } else {
                std::string outputPath;
                if (ChooseUnusedCoverJpegPath(targetFolder, outputPath)) {
                    it->second.outputPath = outputPath;
                }
                it->second.status = AITaskStatus::Failed;
                it->second.errorMessage.setKey(LStr::AI_CoverPathBusy);
            }
         }
     }
    
    if (callback) {
        std::thread([this, taskId, callback]() {
            while (true) {
                AITask task = GetTask(taskId);
                if (task.status == AITaskStatus::Completed ||
                    task.status == AITaskStatus::Failed ||
                    task.status == AITaskStatus::Cancelled) {
                    callback(task);
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }).detach();
    }
    
    return taskId;
}

std::vector<int> AIEngine::BatchTranscribe(const std::vector<std::string>& audioPaths,
                                            const std::string& language,
                                            AITaskCallback callback,
                                            const std::string& initialPrompt,
                                            bool forceSimplifiedChinese) {
    std::vector<int> taskIds;
    for (const auto& path : audioPaths) {
        taskIds.push_back(TranscribeAsync(path,
                                          language,
                                          callback,
                                          initialPrompt,
                                          forceSimplifiedChinese));
    }
    return taskIds;
}

// 全局单例

AIEngine& GetAIEngine() {
    static AIEngine instance;
    return instance;
}
