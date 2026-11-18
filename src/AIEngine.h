#pragma once

#include <string>
#include "Localization.h"  // H1b: LedgerText + LStr (RQ-20260911-04)
#include "AITaskQueueStopGate.h"
#include <vector>
#include <functional>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <map>
#include <queue>

// Forward declaration for whisper.cpp context
struct whisper_context;

// 数据结构定义

enum class AITaskType {
    SpeechRecognition,  // 语音转文字
    SpeechEnhancement,  // 语音增强(降噪)
    MonoToStereo,       // 单声道转立体声
    AudioNormalize,     // 音量标准化
    CoverGeneration     // Remote cover generation
};

enum class AITaskStatus {
    Pending,
    Processing,
    Completed,
    Failed,
    Cancelled
};

struct TranscriptionSegment {
    int64_t startMs;
    int64_t endMs;
    std::string text;
    float confidence;
};

struct TranscriptionResult {
    bool success = false;
    std::string fullText;
    std::string language;
    std::vector<TranscriptionSegment> segments;
    LedgerText errorMessage;  // The transcription worker leaves rendering to the UI thread.
};

struct AudioProcessingResult {
    bool success = false;
    std::string outputPath;
    std::string errorMessage;
};

struct AITask {
    int id;
    AITaskType type;
    AITaskStatus status;
    std::string inputPath;
    std::string outputPath; // for audio processing
    std::string language;   // for transcription
    std::string initialPrompt; // for transcription language/domain bias
    bool forceSimplifiedChinese = false;
    std::string prompt;
    float progress = 0.0f;
    LedgerText errorMessage;  // The cover worker returns a keyed error for the UI to render.
    // H1b: UpdateTaskProgress status is stored as a ledger key (workers must not
    // call L() off-thread). No consumer renders it today (SetProgressCallback has
    // zero registrants); future render sites resolve statusKey on the UI thread.
    bool statusKeyed = false;
    LStr statusKey{};
    
    // Results
    TranscriptionResult transcription;
    AudioProcessingResult audioResult;
};

struct AIEngineConfig {
    std::string modelPath;      // 模型文件夹路径
    std::string modelName = "base"; // 默认模型名称
    std::string defaultLanguage = "auto";
    bool useGpu = true;
    int maxThreads = 4;
    std::string tempDir;        // 临时文件目录
    std::string siliconFlowApiKey; // API key for explicit cover-generation requests.
};

// 回调函数类型
using AITaskCallback = std::function<void(const AITask&)>;
using AIProgressCallback = std::function<void(int taskId, float progress, const std::string& status)>;

// Whisper 采样率常量
const int WHISPER_SAMPLE_RATE = 16000;

// AI 引擎类

// @stage7-contract
// API contract: AIEngine owns model and worker state. Initialize precedes model
// and task operations; Shutdown joins owned work before releasing state. UTF-8
// paths are copied at call boundaries, callbacks are invoked according to the
// existing worker contract, and failures remain in the returned status/results.
class AIEngine {
public:
    AIEngine();
    ~AIEngine();

    // 初始化/关闭
    bool Initialize(const AIEngineConfig& config);
    void Shutdown();
    bool IsInitialized() const { return m_initialized; }

    // 模型管理
    bool LoadModel(const std::string& modelPath);
    void UnloadModel();
    bool IsModelLoaded() const { return m_modelLoaded; }
    std::string GetModelPath() const;
    
    // 下载模型
    static std::string GetModelUrl(const std::string& modelName);
    static int64_t GetModelSize(const std::string& modelName);
    bool DownloadModel(const std::string& modelName, const std::string& savePath, 
                       std::function<void(float)> progressCallback);

    // 核心功能：直接调用（阻塞）
    TranscriptionResult Transcribe(const std::string& audioPath,
                                   const std::string& language = "auto",
                                   const std::string& initialPrompt = "",
                                   bool forceSimplifiedChinese = false);
    
    // 异步功能：非阻塞，返回任务ID
    int TranscribeAsync(const std::string& audioPath,
                        const std::string& language,
                        AITaskCallback callback = nullptr,
                        const std::string& initialPrompt = "",
                        bool forceSimplifiedChinese = false);
    int EnhanceAudio(const std::string& inputPath, const std::string& outputPath = "", AITaskCallback callback = nullptr);
    int ConvertToStereo(const std::string& inputPath, const std::string& outputPath = "", AITaskCallback callback = nullptr);
    int NormalizeAudio(const std::string& inputPath, const std::string& outputPath = "", float targetDb = -1.0f, AITaskCallback callback = nullptr);
    
    // 批量处理
    std::vector<int> BatchTranscribe(const std::vector<std::string>& audioPaths,
                                     const std::string& language,
                                     AITaskCallback callback = nullptr,
                                     const std::string& initialPrompt = "",
                                     bool forceSimplifiedChinese = false);
    
    // 异步生成封面：传入目标文件夹、提示词
    int GenerateCoverAsync(const std::string& targetFolder, const std::string& prompt, AITaskCallback callback = nullptr);

    // 任务管理
    AITask GetTask(int taskId) const;
    std::vector<AITask> GetAllTasks() const;
    std::vector<AITask> GetPendingTasks() const;
    bool CancelTask(int taskId);
    void CancelAllTasks();
    void ClearCompletedTasks();
    
    void SetProgressCallback(AIProgressCallback callback);

private:
    // 内部任务管理
   int CreateTask(AITaskType type, const std::string& inputPath);
    int CreateCoverTaskWithoutQueue(const std::string& inputPath);
    void UpdateTaskStatus(int taskId, AITaskStatus status, const std::string& error = "");
    // Legacy: takes an already-rendered string; callers on worker threads must
    // use the LStr overload instead (constraint 4, H1b).
    void UpdateTaskProgress(int taskId, float progress, const std::string& status);
    void UpdateTaskProgress(int taskId, float progress, LStr statusKey);  // H1b
    
    // 工作线程循环
    void ProcessTaskQueue();
    void ExecuteTask(AITask& task);
    
    // 内部处理函数
    TranscriptionResult TranscribeFromPCM(const std::vector<float>& pcmData,
                                          const std::string& language,
                                          const std::string& initialPrompt,
                                          bool forceSimplifiedChinese);
    
    // 音频处理辅助函数 (这些不访问成员变量，可以是 static)
    static bool LoadAudioFile(const std::string& path, std::vector<float>& pcmData);
    static bool SaveAudioFile(const std::string& path, const std::vector<float>& pcmData, int sampleRate, int channels);
    static void ApplyNoiseReduction(std::vector<float>& pcmData, int sampleRate);
    static void ConvertMonoToStereo(const std::vector<float>& mono, std::vector<float>& stereo);
    static void NormalizePCM(std::vector<float>& pcmData, float targetDb);
    
    // Uses m_config to derive the output path.
    std::string GenerateOutputPath(const std::string& inputPath, const std::string& suffix);

private:
    AIEngineConfig m_config;
    std::atomic<bool> m_initialized{false};
    
    // Whisper context
    struct whisper_context* m_whisperCtx = nullptr;
    mutable std::mutex m_whisperMutex;
    std::string m_modelPath;
    std::atomic<bool> m_modelLoaded{false};
    
    // Task queue
    mutable std::mutex m_tasksMutex;
    AITaskQueueStopGate m_taskQueue;
    std::map<int, AITask> m_tasks;
    std::atomic<int> m_nextTaskId{1};
    
    std::thread m_workerThread;
    
    // Callbacks
    AIProgressCallback m_progressCallback;
};

// 全局辅助函数
AIEngine& GetAIEngine();
const char* GetAITaskTypeName(AITaskType type);
const char* GetAITaskStatusName(AITaskStatus status);
bool IsAudioFileSupported(const std::string& path);
std::string GetDefaultModelDir();
