#ifndef NOMINMAX
#define NOMINMAX
#endif
#define STB_VORBIS_HEADER_ONLY
#include "../thirdparty/whisper.cpp/examples/stb_vorbis.c"
#define MINIAUDIO_IMPLEMENTATION
#include "../thirdparty/miniaudio.h"
#undef STB_VORBIS_HEADER_ONLY
#include "../thirdparty/whisper.cpp/examples/stb_vorbis.c"
#ifdef L
#undef L
#endif
#ifdef C
#undef C
#endif
#ifdef R
#undef R
#endif

#include "AudioEngine.h"
// i18n-anchor (F1): m_lastExportError -> getLastExportError() ->
// main.cpp export toasts (3683/3773/3783); errBuf -> m_lastLoadError ->
// load-failure dialog/toast consumers (reverse map in ticket).
#include "AudioEnvironmentPolicy.h"
#include "AppState.h"
#include "CenterSenseDataSource.h"
#include "CenterSenseDsp.h"
#include "DiagnosticsLogger.h"
#include "MediaFoundationAudioDecoder.h"
#include "Utils.h"
#include "platform/file_prefix_probe.h"
#include "platform/system.h"
#include "platform/text_encoding.h"
#include "WaveformBucketMath.h"
#include "WaveformCachePolicy.h"
#include "WaveformExtractor.h"
#include "WaveformPresentationPolicy.h"
#include "platform/audio_data_source.h"
#include "platform/audio_playback.h"
#include "platform/audio_file.h"
#include "WaveformRegionEditor.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <atomic>
#include <chrono>
#include <limits>
#include <memory>
#include <thread>
#include <unordered_map>

struct PcmAudioBufferState {
    std::vector<float> samples;
    Platform::AudioDataSource::PcmBuffer buffer;
    bool initialized = false;
};

static void DestroyPcmAudioBufferState(PcmAudioBufferState* state) {
    if (!state) return;
    if (state->initialized) {
        Platform::AudioDataSource::UninitializePcmBuffer(state->buffer);
    }
    delete state;
}

// LUFS/TruePeak 检测（ITU-R BS.1770-4）
static constexpr bool kEnableLufsDetection = true;

// AOT primary path: prefer DB cache, but fall back to JIT decode if miss.
static constexpr bool kAotWaveformCacheOnly = false;
static constexpr bool kEnableJitWaveformFallback = true;

// Main waveform policy:
// 1) 不允许 seek-based LOD 长期停留为主波形最终结果；
// 2) 允许从左到右加载，但首发要 warmup，不能第 1 个 peak 就闪出来。
// 主波形策略：禁止 seek LOD 作为用户可见主波形，但允许长音频在最终精度下左到右渐进发布。
static constexpr bool kAllowMainWaveSeekLod = false;
static constexpr bool kEnableProgressiveMainWavePublish = true;
// PRODUCTION: must be true — drives the mid-decode truncation repair at the
// stream-EOF path (see TruncatedAudioMetadata site). Disabling causes truncated
// waveform display for BWF files with FACT chunk duration mismatch.
static constexpr bool kEnableWaveformVisualStretchRepair = true;
// PRODUCTION: must be false — guards two HEURISTIC stretch sites that try to
// distinguish "BWF FACT zero-padded tail" from "legit tail silence" using only
// the peak-value pattern (signalPeaks < 85%). The heuristic can't tell them
// apart: any SFX with a transient + tail silence (door close, single hiss,
// gunshot decay) trips the threshold and gets nearest-neighbor stretched, so
// the user sees the audio "duplicated / staircased" across the timeline.
// The cache-hit site (loadedFromDb branch) is the worst offender because the
// stretched data lives in the cache and rendering errors persist across loads.
// The post-decode "SilencePadding" site has the same flaw and writes the bad
// data INTO the cache. Both stay off until we tag truncation at extract time
// (e.g. a `decoded_short` column) so the load path can stretch deterministically
// instead of guessing. Why: 2026-05-20 user-reported "main waveform doubled"
// for BoomLibrary SFX with silence tails (CacheHitStretch hit 19% of files).
// How to apply: keep this false; if BWF FACT truncation regressions return,
// fix at extract time, not by re-enabling this heuristic.
static constexpr bool kEnableHeuristicSilenceStretch = false;

static int ClampWaveformTargetPeaks(int targetPeaks) {
    if (targetPeaks < 200) targetPeaks = 200;
    if (targetPeaks > 50000) targetPeaks = 50000;
    return targetPeaks;
}

static std::unordered_map<std::string, std::chrono::steady_clock::time_point> g_dirtyDetailPaths;
static uint64_t g_dirtyDetailSweep = 0;
static constexpr auto kDirtyDetailRetention = std::chrono::minutes(10);
static constexpr int64_t kPlaybackStartWorkGuardMs = 300;
static std::atomic<int64_t> g_playbackStartWorkGuardUntilMs{0};

static int64_t SteadyNowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

static void MarkPlaybackStartWorkGuardActive() {
    g_playbackStartWorkGuardUntilMs.store(
        SteadyNowMs() + kPlaybackStartWorkGuardMs,
        std::memory_order_release);
}

static bool IsPlaybackStartWorkGuardActive() {
    return SteadyNowMs() < g_playbackStartWorkGuardUntilMs.load(std::memory_order_acquire);
}

static bool ShouldDeferAudioEngineWaveformDbWritesForBulkWork() {
    return g_appState &&
        (g_appState->isScanning.load(std::memory_order_acquire) ||
         g_appState->database.isReconcileScanRunning());
}

static bool ShouldDeferAudioEngineWaveformDbWrites() {
    return ShouldDeferAudioEngineWaveformDbWritesForBulkWork() ||
        IsPlaybackStartWorkGuardActive();
}

#ifdef _WIN32
// Convert UTF-8 string to wide string for Windows file operations
static std::wstring Utf8ToWide(const std::string& str) {
    return Platform::TextEncoding::Utf8ToWidePermissive(str);
}
#endif

#ifdef _WIN32
static std::wstring ToWinLongPathIfNeeded(const std::wstring& path) {
    if (path.empty()) return path;
    // Already in extended-length path format.
    if (path.rfind(L"\\\\?\\", 0) == 0) return path;
    // UNC path: \\server\share\... => \\?\UNC\server\share\...
    if (path.rfind(L"\\\\", 0) == 0) {
        return L"\\\\?\\UNC\\" + path.substr(2);
    }
    // Drive path: C:\... => \\?\C:\...
    if (path.size() >= 3 && path[1] == L':' && (path[2] == L'\\' || path[2] == L'/')) {
        return L"\\\\?\\" + path;
    }
    return path;
}
#endif

#ifdef _WIN32
static bool IsEnvironmentFlagEnabled(const char* name) {
    return HarkenSearch::AudioEnvironment::IsEnabled(
        Platform::ReadEnvironmentVariable(name));
}
#endif

static bool IsAsyncLoadDiagEnabled() {
#ifdef _WIN32
    static int cached = -1;
    if (cached < 0) {
        cached = IsEnvironmentFlagEnabled("AURA_LOAD_DIAG") ? 1 : 0;
    }
    return cached == 1;
#else
    return false;
#endif
}

static bool IsHarnessAudioMuteEnabled() {
#ifdef _WIN32
    static int cached = -1;
    if (cached < 0) {
        const bool multiInstance =
            IsEnvironmentFlagEnabled("AURA_HARNESS_MULTI_INSTANCE");
        const bool muteAudio =
            IsEnvironmentFlagEnabled("AURA_HARNESS_MUTE_AUDIO");
        cached = (multiInstance && muteAudio) ? 1 : 0;
    }
    return cached == 1;
#else
    return false;
#endif
}

static void ApplyHarnessAudioMuteIfNeeded(Platform::AudioPlayback::BorrowedEngine engine) {
    if (engine.IsValid() && IsHarnessAudioMuteEnabled()) {
        Platform::AudioPlayback::SetEngineVolume(engine, 0.0f);
        DIAG_ACTION("Audio", "HarnessMuteAudioEnabled");
    }
}

static constexpr int kMaxAuditionChannelSolo = 8;

static bool ShouldEnableProgressiveMainWaveForFile(uint64_t totalFrames,
                                                   uint32_t sampleRate,
                                                   int targetPeaks,
                                                   bool hasUsableMainDetailCache) {
    if (!kEnableProgressiveMainWavePublish) {
        return false;
    }
    if (sampleRate == 0 || totalFrames == 0 || targetPeaks <= 0) {
        return false;
    }

    return ShouldUseProgressiveMainWaveDecode(
        hasUsableMainDetailCache,
        (float)((double)totalFrames / (double)sampleRate),
        targetPeaks,
        AppState::WAVEFORM_DEFAULT_DETAIL_PEAKS);
}

WaveformData AudioEngine::BuildDisplayWaveformFromSource(const WaveformData& source,
                                                         bool forceMono,
                                                         int channelSolo,
                                                         const std::string& filePath) {
    WaveformData out = source;
    out.filePath = filePath;

    if (!source.isReady || source.peaks.empty() || source.numChannels <= 1) {
        out.numChannels = (std::max)(1, source.numChannels);
        out.isStereo = (out.numChannels == 2);
        if (!out.minPeaks.empty() && out.minPeaks.size() != out.peaks.size()) {
            out.minPeaks.clear();
        }
        return out;
    }

    const int srcChannels = (std::max)(1, source.numChannels);
    const int frameCount = (int)source.peaks.size() / srcChannels;
    const bool hasRms = !source.rms.empty() && source.rms.size() == source.peaks.size();
    const bool hasMinPeaks = !source.minPeaks.empty() && source.minPeaks.size() == source.peaks.size();

    if (frameCount <= 0) {
        out.numChannels = 1;
        out.isStereo = false;
        out.minPeaks.clear();
        return out;
    }

    // Solo：直接抽取指定通道
    if (channelSolo > 0 && channelSolo <= srcChannels) {
        const int soloCh = channelSolo - 1;

        out.peaks.assign((size_t)frameCount, 0.0f);
        if (hasMinPeaks) out.minPeaks.assign((size_t)frameCount, 0.0f);
        else out.minPeaks.clear();
        if (hasRms) out.rms.assign((size_t)frameCount, 0.0f);
        else out.rms.clear();

        for (int frame = 0; frame < frameCount; ++frame) {
            const int base = frame * srcChannels;
            out.peaks[(size_t)frame] = source.peaks[(size_t)(base + soloCh)];
            if (hasMinPeaks) {
                out.minPeaks[(size_t)frame] = source.minPeaks[(size_t)(base + soloCh)];
            }
            if (hasRms) {
                out.rms[(size_t)frame] = source.rms[(size_t)(base + soloCh)];
            }
        }

        out.numChannels = 1;
        out.isStereo = false;
        out.isReady = true;
        return out;
    }

    // Mono：从多通道 detail 即时合成显示波形
    if (forceMono && srcChannels > 1) {
        out.peaks.assign((size_t)frameCount, 0.0f);
        if (hasMinPeaks) out.minPeaks.assign((size_t)frameCount, 0.0f);
        else out.minPeaks.clear();
        if (hasRms) out.rms.assign((size_t)frameCount, 0.0f);
        else out.rms.clear();

        for (int frame = 0; frame < frameCount; ++frame) {
            const int base = frame * srcChannels;

            float peak = 0.0f;
            float minPeak = 0.0f;
            double rmsSqSum = 0.0;

            for (int ch = 0; ch < srcChannels; ++ch) {
                const float p = source.peaks[(size_t)(base + ch)];
                if (p > peak) peak = p;
                if (hasMinPeaks) {
                    const float mn = source.minPeaks[(size_t)(base + ch)];
                    if (mn < minPeak) minPeak = mn;
                }

                if (hasRms) {
                    const float r = source.rms[(size_t)(base + ch)];
                    rmsSqSum += (double)r * (double)r;
                }
            }

            out.peaks[(size_t)frame] = peak;
            if (hasMinPeaks) {
                out.minPeaks[(size_t)frame] = minPeak;
            }
            if (hasRms) {
                out.rms[(size_t)frame] = (float)std::sqrt(rmsSqSum / (double)srcChannels);
            }
        }

        out.numChannels = 1;
        out.isStereo = false;
        out.isReady = true;
        return out;
    }

    out.numChannels = srcChannels;
    out.isStereo = (srcChannels == 2);
    out.isReady = true;
    return out;
}

bool AudioEngine::tryApplySourceWaveformForCurrentView(const std::shared_ptr<const WaveformData>& sourceSnapshot,
                                                       const std::string& expectedPath) {
    if (!sourceSnapshot || !sourceSnapshot->isReady || sourceSnapshot->peaks.empty()) {
        return false;
    }
    if (sourceSnapshot->filePath != expectedPath) {
        return false;
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_isLoaded || m_fileInfo.filePath != expectedPath) {
        return false;
    }

    WaveformData display = BuildDisplayWaveformFromSource(
        *sourceSnapshot,
        m_forceMono,
        m_channelSolo,
        expectedPath
    );

    if (!display.isReady || display.peaks.empty()) {
        return false;
    }

    std::atomic_store(&m_waveformSource, sourceSnapshot);
    std::shared_ptr<const WaveformData> displayPtr =
        std::make_shared<WaveformData>(std::move(display));
    std::atomic_store(&m_waveform, displayPtr);

    const int generatedPeaks =
        (sourceSnapshot->numChannels > 0)
            ? (int)sourceSnapshot->peaks.size() / sourceSnapshot->numChannels
            : (int)sourceSnapshot->peaks.size();

    m_waveformGeneratedPeaks.store(generatedPeaks);
    if (m_waveformRequestedPeaks.load() < generatedPeaks) {
        m_waveformRequestedPeaks.store(generatedPeaks);
    }

    return true;
}

AudioEngine::AudioEngine() {}

AudioEngine::~AudioEngine() {
    shutdown();
}

bool AudioEngine::initialize(int periodSizeMs) {
    std::lock_guard<std::mutex> lock(m_mutex);

    if (m_isInitialized) return true;

    m_engine = std::make_unique<Platform::AudioPlayback::Engine>();

    Platform::AudioPlayback::EngineInitOptions engineOptions;
    engineOptions.channels = 2;
    engineOptions.sampleRate = 48000;
    int period = (periodSizeMs >= 1 && periodSizeMs <= 100) ? periodSizeMs : 10;
    engineOptions.periodSizeMs = static_cast<std::uint32_t>(period);

    const auto engineInitResult = m_engine->Initialize(engineOptions);
    if (!engineInitResult.Succeeded()) {
        m_engine = nullptr;
        return false;
    }
    ApplyHarnessAudioMuteIfNeeded(m_engine->Borrow());

    m_isInitialized = true;

    {
        const auto metrics =
            Platform::AudioPlayback::QueryPlaybackDeviceMetrics(
                m_engine->Borrow());
        if (metrics.IsValid()) {
            DIAG_ACTION_FMT("Audio",
                "EngineInit: engineSR=%u deviceSR=%u periodFrames=%u periods=%u latencyMs=%.1f",
                Platform::AudioPlayback::QueryEngineSampleRate(m_engine->Borrow()),
                metrics.sampleRate,
                metrics.periodFrames,
                metrics.periods,
                (float)metrics.periodFrames *
                    (float)metrics.periods /
                    (float)(metrics.sampleRate > 0 ? metrics.sampleRate : 48000) * 1000.0f);
        }
    }

    return true;
}

// ---------------------------------------------------------------------------
// Deferred sound cleanup — replaces detached threads.
// Old sounds are queued with a deadline (fade duration + margin); the main
// thread drains expired entries each frame via pumpDeferredCleanup().
// ---------------------------------------------------------------------------
void AudioEngine::pumpDeferredCleanup() {
    if (m_cleanupQueue.empty()) return;
    auto now = std::chrono::steady_clock::now();
    auto it = m_cleanupQueue.begin();
    while (it != m_cleanupQueue.end()) {
        if (now >= it->deadline) {
            (void)Platform::AudioPlayback::Stop(it->sound->Borrow());
            it->sound.reset();
            if (it->csds) { it->csds->uninit(); delete it->csds; }
            it->decoder.reset();
            if (it->pcm) {
                PcmAudioBufferState* pcm = it->pcm;
                // Disconnect the data source from the audio graph on the main thread first.
                if (pcm->initialized) {
                    Platform::AudioDataSource::UninitializePcmBuffer(pcm->buffer);
                    pcm->initialized = false;
                }
                // Freeing the PCM samples vector on the main thread can stall for
                // 100-200 ms in Debug builds because MSVC _free_dbg fills freed
                // memory with 0xDD (a 200 MB WMA buffer = ~200 ms). Move the
                // actual delete to a detached background thread.
                std::thread([pcm]() noexcept { delete pcm; }).detach();
            }
            it = m_cleanupQueue.erase(it);
        } else {
            ++it;
        }
    }
}

void AudioEngine::flushDeferredCleanup() {
    for (auto& c : m_cleanupQueue) {
        (void)Platform::AudioPlayback::Stop(c.sound->Borrow());
        c.sound.reset();
        if (c.csds) { c.csds->uninit(); delete c.csds; }
        c.decoder.reset();
        DestroyPcmAudioBufferState(c.pcm);
    }
    m_cleanupQueue.clear();
}

void AudioEngine::shutdown() {
    DIAG_ACTION_FMT("Audio", "ShutdownBegin: hasEngine=%d loaded=%d deferred=%d",
        m_engine ? 1 : 0, m_isLoaded ? 1 : 0, (int)m_cleanupQueue.size());
    stopSeekWorker();
    stopWaveformWorker();
    stopLoadFileWorker();

    std::unique_ptr<Platform::AudioPlayback::Engine> engineToUninit;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_engine) {
            // 退出路径不能复用交互态的异步 fade stop。
            // 这里需要同步切断当前 sound 与 deferred cleanup 队列里的旧 sound，
            // 然后再停设备，避免音频回调继续访问即将释放的节点。
            if (m_sound) {
                Platform::AudioPlayback::SetStopTimeInPcmFrames(m_sound->Borrow(), static_cast<std::uint64_t>(-1));
                Platform::AudioPlayback::SetFadeInMilliseconds(m_sound->Borrow(), 1.0f, 1.0f, 0);
                (void)Platform::AudioPlayback::Stop(m_sound->Borrow());
            }
            for (auto& c : m_cleanupQueue) {
                if (c.sound) {
                    Platform::AudioPlayback::SetStopTimeInPcmFrames(c.sound->Borrow(), static_cast<std::uint64_t>(-1));
                    Platform::AudioPlayback::SetFadeInMilliseconds(c.sound->Borrow(), 0.0f, 0.0f, 0);
                    (void)Platform::AudioPlayback::Stop(c.sound->Borrow());
                }
            }

            // 必须先停止设备，确保音频回调不再被调用，再销毁 sound；否则回调/混音可能访问已释放的 sound → 撕裂/爆音
            (void)m_engine->StopDevice();
            // 给驱动一个极短的稳定窗口，避免 USB/ASIO 等设备在 stop 返回后仍有残余缓冲切换。
            std::this_thread::sleep_for(std::chrono::milliseconds(50));

            // 设备停稳后，再释放当前 sound 和 deferred cleanup 队列，避免退出边界 UAF。
            flushDeferredCleanup();
            unload();
            engineToUninit = std::move(m_engine);
            m_isInitialized = false;
        }
    }

    if (engineToUninit) engineToUninit->Uninitialize();
    DIAG_ACTION_FMT("Audio", "ShutdownEnd: deferred=%d", (int)m_cleanupQueue.size());
}

void AudioEngine::setFadeInOutEnabled(bool enabled) {
    m_fadeEnabled.store(enabled);
}

void AudioEngine::setFadeInOutMs(int ms) {
    // Keep it very short; too long makes scrubbing feel laggy.
    if (ms < 0) ms = 0;
    if (ms > 60) ms = 60;
    m_seekFadeMs.store(ms);
}

void AudioEngine::resetState(bool clearVisualWaveform) {
    m_fileInfo = AudioFileInfo{};
    m_playbackChannels = 0;
    if (m_magicEQUnsupportedChannels) {
        m_magicEQ.enabled = m_magicEQUserEnabled;
        m_magicEQ.invalidInput = false;
        m_magicEQUnsupportedChannels = false;
    }

    if (clearVisualWaveform) {
        std::shared_ptr<const WaveformData> p = std::make_shared<WaveformData>();
        std::atomic_store(&m_waveform, p);
    }

    m_waveformRequestedPeaks.store(0);
    m_waveformGeneratedPeaks.store(0);
    m_waveformRequestToken.fetch_add(1, std::memory_order_relaxed);
    m_isLoaded = false;
    m_playIntent = false;
    m_loopSawPlaying = false;
    m_decodedPlaybackTotalFrames = 0;
    m_inPoint = 0.0f;
    m_outPoint = 1.0f;
    m_regionPlayback = false;
    m_regionLoopActive = false;
    m_regionFadeInNorm = 0.0f;
    m_regionFadeOutNorm = 0.0f;
    m_reverseBuffer.clear();
    m_reverseBufferValid = false;
    m_reverse = false;
}

std::shared_ptr<const WaveformData> AudioEngine::getWaveformData() const {
    auto p = std::atomic_load(&m_waveform);
    if (!p) return std::make_shared<WaveformData>();
    return p;
}

void AudioEngine::updateWaveformDataManually(const std::vector<float>& newPeaks) {
    if (newPeaks.empty()) return;
    WaveformData wfData;
    wfData.peaks = newPeaks;
    wfData.rms.resize(newPeaks.size());
    for (size_t i = 0; i < newPeaks.size(); ++i)
        wfData.rms[i] = std::abs(newPeaks[i]) * 0.35f;
    wfData.isReady = true;
    wfData.isFallbackPlaceholder = false;
    wfData.targetPeaks = (int)newPeaks.size();
    wfData.completedPeaks = (int)newPeaks.size();
    wfData.isProgressiveDetail = false;
    wfData.numChannels = 1;
    wfData.isStereo = false;
    wfData.hasLoudnessData = false;
    std::shared_ptr<const WaveformData> p = std::make_shared<WaveformData>(std::move(wfData));
    // MSVC needs explicit template arg; libc++ rejects _Atomic on const-qualified types
#ifdef _MSC_VER
    std::atomic_store<const WaveformData>(&m_waveform, p);
#else
    std::atomic_store(&m_waveform, p);
#endif

}

void AudioEngine::clearCurrentWaveformVisual() {
    std::string path;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        path = m_fileInfo.filePath;
    }

    auto empty = std::make_shared<WaveformData>();
    empty->filePath = path;
    empty->isReady = false;
    empty->isFallbackPlaceholder = true;
    std::shared_ptr<const WaveformData> display = empty;
    std::shared_ptr<const WaveformData> source;
    std::atomic_store(&m_waveform, display);
    std::atomic_store(&m_waveformSource, source);
    DIAG_ACTION_FMT("Waveform", "ClearCurrentWaveformVisual path=%s", path.c_str());
}

void AudioEngine::ensureWaveformWorkerStarted() {
    std::lock_guard<std::mutex> lk(m_waveformWorkerMutex);
    if (m_waveformWorker.joinable()) return;
    m_waveformWorkerExit.store(false);
    m_waveformWorker = std::thread([this]() { waveformWorkerLoop(); });
}

void AudioEngine::stopWaveformWorker() {
    {
        std::lock_guard<std::mutex> lk(m_waveformWorkerMutex);
        m_waveformWorkerExit.store(true);
    }
    m_waveformWorkerCv.notify_all();
    if (m_waveformWorker.joinable()) {
        m_waveformWorker.join();
    }
}

bool AudioEngine::buildWaveform(const std::string& filePath, Platform::AudioPlayback::BorrowedEngine engine, bool forceMono, int channelSolo, WaveformData& out, int targetPeaks) {
    DIAG_SCOPED_DETAIL("Waveform", "buildWaveform");
    if (!engine.IsValid() || filePath.empty() || targetPeaks <= 0) return false;

    const bool isFlacPath = HarkenSearch::MediaFoundationAudio::HasFlacExtension(filePath);

    auto buildFromMediaFoundationDecoded = [&](const char* diagPrefix) -> bool {
        HarkenSearch::MediaFoundationAudio::DecodedAudioBuffer decoded;
        std::string mfError;
        if (!HarkenSearch::MediaFoundationAudio::Decode(filePath, decoded, &mfError)) {
            DIAG_ACTION_FMT("Waveform",
                "%sDecodeFailed: path=%s error=%s",
                diagPrefix,
                filePath.c_str(),
                HarkenSearch::MediaFoundationAudio::ClassifyFailure(mfError).c_str());
            return false;
        }
        WaveformExtractResult extracted;
        const bool perChannel = !(forceMono || channelSolo != 0);
        if (!HarkenSearch::MediaFoundationAudio::BuildPeaksFromDecoded(
                decoded, targetPeaks, true, perChannel, extracted, nullptr)) {
            DIAG_ACTION_FMT("Waveform",
                "%sPeaksFailed: path=%s error=%s",
                diagPrefix,
                filePath.c_str(),
                extracted.errorCode.c_str());
            return false;
        }
        out.peaks = std::move(extracted.peaks);
        out.minPeaks = std::move(extracted.minPeaks);
        out.rms = std::move(extracted.rms);
        out.samplesPerPeak = extracted.samplesPerPeak;
        out.numChannels = extracted.channels > 0 ? extracted.channels : 1;
        out.isStereo = (out.numChannels == 2);
        out.trueRmsDbfs = extracted.trueRmsDbfs;
        out.hasTrueRms = extracted.hasTrueRms;
        out.filePath = filePath;
        out.targetPeaks = targetPeaks;
        out.completedPeaks = out.numChannels > 0 ? (int)(out.peaks.size() / (size_t)out.numChannels) : 0;
        out.isReady = true;
        DIAG_ACTION_FMT("Waveform",
            "MediaFoundationBuildWaveformOK: mode=%s path=%s format=%s ch=%u frames=%llu peaks=%zu",
            diagPrefix,
            filePath.c_str(),
            HarkenSearch::MediaFoundationAudio::FormatNameFromPath(filePath),
            (unsigned)decoded.channels,
            (unsigned long long)decoded.totalFrames,
            out.peaks.size());
        return true;
    };

    if (HarkenSearch::MediaFoundationAudio::CanDecodeExtension(filePath) && !isFlacPath) {
        return buildFromMediaFoundationDecoded("MediaFoundationBuildWaveform");
    }

    Platform::AudioDataSource::Decoder decoder;
    const std::uint32_t engineSR = Platform::AudioPlayback::QueryEngineSampleRate(engine);

    // Preserve the source channel count while extracting detail peaks.
    // An implicit decoder layout may downmix 5.1 or 7.1 sources to stereo.
    std::uint32_t wfChannels = (forceMono || channelSolo != 0) ? 1 : 0;
    if (wfChannels == 0) {
        Platform::AudioDataSource::Decoder probe;
        const auto probeResult = Platform::AudioDataSource::InitializeDecoderFromFile(
            probe, filePath);
        if (probeResult.Succeeded()) {
            const auto probeFormat = Platform::AudioDataSource::QueryDataFormat(
                Platform::AudioDataSource::BorrowDataSource(probe));
            if (probeFormat.channels > 2) {
                wfChannels = probeFormat.channels;
            }
            Platform::AudioDataSource::UninitializeDecoder(probe);
        }
    }
    const auto decoderInit = Platform::AudioDataSource::InitializeDecoderFromFileWithFormatAndChannelMap(
        decoder, filePath, wfChannels, engineSR, channelSolo);
    int result = decoderInit.backendCode;
    if (result != MA_SUCCESS) {
        DIAG_ACTION_FMT("Waveform", "buildWaveform DecoderInitFailed: path=%s, ma_result=%d", filePath.c_str(), (int)result);
        if (isFlacPath && buildFromMediaFoundationDecoded("MediaFoundationFlacBuildWaveformFallback")) {
            return true;
        }
        return false;
    }

    const auto decoderFormat = Platform::AudioDataSource::QueryDataFormat(
        Platform::AudioDataSource::BorrowDataSource(decoder));
    const uint32_t actualChannels = decoderFormat.channels;
    if (actualChannels == 0) { Platform::AudioDataSource::UninitializeDecoder(decoder); return false; }

    uint64_t totalFrames =
        Platform::AudioDataSource::QueryLength(
            Platform::AudioDataSource::BorrowDataSource(decoder)).frames;
    if (totalFrames == 0) {
        Platform::AudioDataSource::UninitializeDecoder(decoder);
        return false;
    }

    int samplesPerPeak = (int)(totalFrames / (uint64_t)targetPeaks);
    if (samplesPerPeak < 1) samplesPerPeak = 1;

    out.peaks.clear();
    out.rms.clear();
    out.samplesPerPeak = samplesPerPeak;
    out.numChannels = (int)actualChannels;
    out.isStereo = (actualChannels == 2);

    static constexpr uint64_t kWaveChunkFrames = 8192;
    std::vector<float> buffer((size_t)kWaveChunkFrames * (size_t)actualChannels);
    std::vector<float> blockPeaks((size_t)actualChannels, 0.0f);
    std::vector<double> blockSumSq((size_t)actualChannels, 0.0);
    uint64_t blockFrames = 0;
    while (true) {
        const auto readResult = Platform::AudioDataSource::ReadPcmFrames(
            Platform::AudioDataSource::BorrowDataSource(decoder),
            buffer.data(),
            kWaveChunkFrames);
        uint64_t framesRead = readResult.framesRead;
        if (framesRead == 0) break;

        for (uint64_t i = 0; i < framesRead; i++) {
            for (uint32_t ch = 0; ch < actualChannels; ch++) {
                float s = buffer[(size_t)(i * actualChannels + ch)];
                float a = std::abs(s);
                if (a > blockPeaks[(size_t)ch]) blockPeaks[(size_t)ch] = a;
                blockSumSq[(size_t)ch] += (double)s * (double)s;
            }
            blockFrames++;
            if (blockFrames >= (uint64_t)samplesPerPeak) {
                for (uint32_t ch = 0; ch < actualChannels; ch++) {
                    out.peaks.push_back(blockPeaks[(size_t)ch]);
                    out.rms.push_back((float)std::sqrt(blockSumSq[(size_t)ch] / (double)blockFrames));
                    blockPeaks[(size_t)ch] = 0.0f;
                    blockSumSq[(size_t)ch] = 0.0;
                }
                blockFrames = 0;
            }
        }
    }

    if (blockFrames > 0) {
        for (uint32_t ch = 0; ch < actualChannels; ch++) {
            out.peaks.push_back(blockPeaks[(size_t)ch]);
            out.rms.push_back((float)std::sqrt(blockSumSq[(size_t)ch] / (double)blockFrames));
        }
    }

    Platform::AudioDataSource::UninitializeDecoder(decoder);
    out.isReady = true;
    return true;
}

// Seek-based quick waveform: reads only small blocks at evenly spaced positions.
// Produces a rough preview in ~1/100th the time of a full buildWaveform.
bool AudioEngine::buildQuickWaveform(const std::string& filePath, Platform::AudioPlayback::BorrowedEngine engine, bool forceMono, int channelSolo, WaveformData& out, int targetPeaks) {
    DIAG_SCOPED_DETAIL("Waveform", "buildQuickWaveform");
    if (!engine.IsValid() || filePath.empty() || targetPeaks <= 0) return false;

    Platform::AudioDataSource::Decoder decoder;
    const std::uint32_t engineSR = Platform::AudioPlayback::QueryEngineSampleRate(engine);
    std::uint32_t wfChannels = (forceMono || channelSolo != 0) ? 1 : 0;
    // 多声道探测：同 buildWaveform
    if (wfChannels == 0) {
        Platform::AudioDataSource::Decoder probe;
        const auto probeResult = Platform::AudioDataSource::InitializeDecoderFromFile(
            probe, filePath);
        if (probeResult.Succeeded()) {
            const auto probeFormat = Platform::AudioDataSource::QueryDataFormat(
                Platform::AudioDataSource::BorrowDataSource(probe));
            if (probeFormat.channels > 2) {
                wfChannels = probeFormat.channels;
            }
            Platform::AudioDataSource::UninitializeDecoder(probe);
        }
    }
    const auto decoderInit = Platform::AudioDataSource::InitializeDecoderFromFileWithFormatAndChannelMap(
        decoder, filePath, wfChannels, engineSR, channelSolo);
    int result = decoderInit.backendCode;
    if (result != MA_SUCCESS) {
        DIAG_ACTION_FMT("Waveform", "buildQuickWaveform DecoderInitFailed: path=%s, ma_result=%d", filePath.c_str(), (int)result);
        return false;
    }

    const auto decoderFormat = Platform::AudioDataSource::QueryDataFormat(
        Platform::AudioDataSource::BorrowDataSource(decoder));
    const uint32_t actualChannels = decoderFormat.channels;
    if (actualChannels == 0) { Platform::AudioDataSource::UninitializeDecoder(decoder); return false; }

    uint64_t totalFrames =
        Platform::AudioDataSource::QueryLength(
            Platform::AudioDataSource::BorrowDataSource(decoder)).frames;
    if (totalFrames == 0) {
        Platform::AudioDataSource::UninitializeDecoder(decoder);
        return false;
    }

    // Read a small block (512 frames) at evenly spaced seek positions
    const int blockSize = 512;
    uint64_t segmentSize = totalFrames / (uint64_t)targetPeaks;
    if (segmentSize < 1) segmentSize = 1;

    out.peaks.clear();
    out.rms.clear();
    out.peaks.reserve((size_t)targetPeaks * actualChannels);
    out.rms.reserve((size_t)targetPeaks * actualChannels);
    out.samplesPerPeak = (int)segmentSize;
    out.numChannels = (int)actualChannels;
    out.isStereo = (actualChannels == 2);

    std::vector<float> buffer((size_t)blockSize * (size_t)actualChannels);

    for (int seg = 0; seg < targetPeaks; seg++) {
        uint64_t seekPos = (uint64_t)seg * segmentSize;
        // Seek to segment center for better representation
        uint64_t seekCenter = seekPos + segmentSize / 2;
        if (seekCenter + blockSize > totalFrames) seekCenter = (totalFrames > (uint64_t)blockSize) ? totalFrames - blockSize : 0;

        if (!Platform::AudioDataSource::SeekPcmFrame(
                Platform::AudioDataSource::BorrowDataSource(decoder),
                seekCenter).Succeeded()) {
            // Seek failed — push silence for this segment
            for (uint32_t ch = 0; ch < actualChannels; ch++) {
                out.peaks.push_back(0.0f);
                out.rms.push_back(0.0f);
            }
            continue;
        }

        const auto readResult = Platform::AudioDataSource::ReadPcmFrames(
            Platform::AudioDataSource::BorrowDataSource(decoder),
            buffer.data(),
            blockSize);
        uint64_t framesRead = readResult.framesRead;
        if (framesRead == 0) {
            for (uint32_t ch = 0; ch < actualChannels; ch++) {
                out.peaks.push_back(0.0f);
                out.rms.push_back(0.0f);
            }
            continue;
        }

        for (uint32_t ch = 0; ch < actualChannels; ch++) {
            float peak = 0.0f;
            float sumSq = 0.0f;
            for (uint64_t i = 0; i < framesRead; i++) {
                float s = buffer[(size_t)(i * actualChannels + ch)];
                float a = std::abs(s);
                peak = std::max(peak, a);
                sumSq += s * s;
            }
            float rms = std::sqrt(sumSq / (float)framesRead);
            out.peaks.push_back(peak);
            out.rms.push_back(rms);
        }
    }

    Platform::AudioDataSource::UninitializeDecoder(decoder);
    out.isReady = true;
    return true;
}

// 请确保在 AudioEngine.cpp 顶部加了 #include "AppState.h"

void AudioEngine::waveformWorkerLoop() {
    // Keep waveform decode in background: avoid stealing cycles from UI/audio threads.
    Platform::SetThreadBackgroundPriority();
    while (!m_waveformWorkerExit.load()) {
        // 1. 获取当前时刻的请求快照（使用强一致性确保所有 CPU 核心立即感知）
        const uint64_t workerToken = m_waveformRequestToken.load(std::memory_order_seq_cst);
        int req = m_waveformRequestedPeaks.load();
        int gen = m_waveformGeneratedPeaks.load();

        // 1. 如果没有新请求，就睡觉
        if (req <= gen) {
            std::unique_lock<std::mutex> lk(m_waveformWorkerMutex);
            m_waveformWorkerCv.wait_for(lk, std::chrono::milliseconds(100), [&]() {
                return m_waveformWorkerExit.load() || m_waveformRequestedPeaks.load() > m_waveformGeneratedPeaks.load();
            });
            continue;
        }

        // 2. 获取任务信息
        std::string path;
        bool mono = false;
        int solo = 0;
        Platform::AudioPlayback::BorrowedEngine engine = Platform::AudioPlayback::BorrowEngine(nullptr);
        uint64_t fileTotalFramesSnapshot = 0;
        int fileChannels = 1; // 文件原始通道数
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (!m_isLoaded || m_fileInfo.filePath.empty() || !m_engine) {
                m_waveformGeneratedPeaks.store(req);
                continue;
            }
            path = m_fileInfo.filePath;
            mono = m_forceMono;
            solo = m_channelSolo;
            engine = m_engine->Borrow();
            fileChannels = (int)m_fileInfo.channels;
            fileTotalFramesSnapshot = m_fileInfo.totalFrames;
        }
        // Defensive: when channel probe fails, default to mono (1 channel),
        // not stereo. Defaulting to 2 caused the waveform placeholder to
        // publish 2-channel data with duplicated peaks for mono sources —
        // user-visible symptom was mono SFX rendering as 2 lanes ("Track 1 - L"
        // / "Track 2 - R") in the main waveform area. Mini waveform path uses
        // WaveformExtractor::ExtractMini directly which correctly reports
        // chan=1; only the main waveform fallback was wrong.
        if (fileChannels < 1) fileChannels = 1;

        auto expectedDetailFloats = [&](int channels) -> int {
            channels = (std::max)(1, channels);
            int perChannelExpected = req;
            if (fileTotalFramesSnapshot > 0 &&
                fileTotalFramesSnapshot < (uint64_t)perChannelExpected) {
                perChannelExpected = (int)fileTotalFramesSnapshot;
            }
            return perChannelExpected * channels;
        };

        WaveformData wf;
        std::shared_ptr<const WaveformData> sourceWaveformToStore;
        bool loadedFromDb = false;
        bool finalPersistAttempted = false;
        bool finalPersisted = false;
        const char* finalPersistReason = "not_applicable";
        extern AppState* g_appState;

        DIAG_SYSTEM_FMT("Waveform",
            "MainDetailWorkerStart: path=%s req=%d token=%llu channels=%d totalFrames=%llu",
            path.c_str(),
            req,
            (unsigned long long)workerToken,
            fileChannels,
            (unsigned long long)fileTotalFramesSnapshot);

        auto normalizeWaveformTailPeaksForStableDisplay = [&](const char* stage) -> bool {
            const int ch = std::max(1, wf.numChannels);
            if (req <= 0 || wf.peaks.empty()) return false;
            const int beforePeaks = (int)wf.peaks.size() / ch;
            if (!RebucketWaveformPeaksToTarget(wf.peaks, wf.minPeaks, wf.rms, ch, req)) {
                return false;
            }

            wf.targetPeaks = req;
            wf.completedPeaks = req;
            wf.isProgressiveDetail = false;
            const int afterPeaks = (int)wf.peaks.size() / ch;
            DIAG_ACTION_FMT("Waveform",
                "WaveformRebucketedToTarget: path=%s stage=%s beforePeaks=%d targetPeaks=%d afterPeaks=%d channels=%d",
                path.c_str(), stage ? stage : "unknown", beforePeaks, req, afterPeaks, ch);
            return true;
        };

        // Reuse persisted detail before decoding. Channel-solo views bypass this
        // cache because stored peaks retain the source channel layout.
        if (g_appState && g_appState->database.isOpen()) {
            const auto now = std::chrono::steady_clock::now();
            const int64_t appFileId = g_appState ? g_appState->currentFileId : -2;
            if (appFileId > 0) {
                const AudioRecord rec = g_appState->database.getFileById(appFileId);
                if (rec.channels > 0) {
                    fileChannels = rec.channels;
                }
            }
            auto dirtyIt = g_dirtyDetailPaths.find(path);
            if (dirtyIt != g_dirtyDetailPaths.end() &&
                (now - dirtyIt->second) >= kDirtyDetailRetention) {
                DIAG_ACTION_FMT("Waveform",
                    "DirtyDetailCacheExpired: path=%s, ageMs=%lld",
                    path.c_str(),
                    (long long)std::chrono::duration_cast<std::chrono::milliseconds>(now - dirtyIt->second).count());
                g_dirtyDetailPaths.erase(dirtyIt);
                dirtyIt = g_dirtyDetailPaths.end();
            }

            auto markSuspiciousDetailDirty = [&](const char* reason, float detailMax, float miniMax, int peaksSize) {
                const auto now = std::chrono::steady_clock::now();
                const bool alreadyDirty = g_dirtyDetailPaths.find(path) != g_dirtyDetailPaths.end();
                g_dirtyDetailPaths[path] = now;
                if ((++g_dirtyDetailSweep & 63ULL) == 0) {
                    const auto expireBefore = now - kDirtyDetailRetention;
                    for (auto mapIt = g_dirtyDetailPaths.begin(); mapIt != g_dirtyDetailPaths.end();) {
                        if (mapIt->second < expireBefore) mapIt = g_dirtyDetailPaths.erase(mapIt);
                        else ++mapIt;
                    }
                }

                if (alreadyDirty) {
                    DIAG_ACTION_FMT("Waveform",
                        "InvalidDetailCacheAlreadyDirty: path=%s, reason=%s, detailMax=%.6f, miniMax=%.6f, peaksSize=%d",
                        path.c_str(), reason, detailMax, miniMax, peaksSize);
                } else {
                    DIAG_ACTION_FMT("Waveform",
                    "InvalidDetailCacheMarkedDirty: path=%s, detailMax=%.6f, miniMax=%.6f, peaksSize=%d, reason=%s",
                    path.c_str(), detailMax, miniMax, peaksSize, reason);
                }
            };

            // 尝试读取波形
            const bool dirtyDetailShadow = g_dirtyDetailPaths.find(path) != g_dirtyDetailPaths.end();
            const bool allowPersistentMainDetailCache =
                g_appState->database.getWaveformCacheMode() != WaveformCachePolicy::Mode::MiniOnly;
            std::vector<float> cachedPeaks;
            std::vector<float> cachedMinPeaks;
            if (!allowPersistentMainDetailCache) {
                DIAG_ACTION_FMT("Waveform",
                    "MainDetailCacheBypassPolicyMiniOnly: path=%s",
                    path.c_str());
            } else if (dirtyDetailShadow) {
                DIAG_ACTION_FMT("Waveform", "DirtyDetailCacheBypass: path=%s", path.c_str());
            } else {
                cachedPeaks = g_appState->database.getWaveformDetailByPath(path);
            }
            int cachedDetailChannelsForHit = 0;
            // 防御：过小 detail 缓存直接判无效并清理，避免“缓存命中但空白”
            if (!cachedPeaks.empty()) {
                const int cachedDetailChannels =
                    g_appState->database.getWaveformDetailChannelsByPath(path);
                if (cachedDetailChannels > 0 && cachedDetailChannels <= 16) {
                    if (!IsUsableWaveformDetailCacheShape(
                            cachedPeaks.size(), cachedDetailChannels, fileChannels)) {
                        const char* reason =
                            (cachedDetailChannels != fileChannels)
                                ? "detail_channel_mismatch"
                                : "bad_detail_channel_stride";
                        markSuspiciousDetailDirty(
                            reason, -1.0f, -1.0f, (int)cachedPeaks.size());
                        cachedPeaks.clear();
                    } else {
                        cachedDetailChannelsForHit = cachedDetailChannels;
                    }
                } else if (cachedDetailChannels <= 0 || cachedDetailChannels > 16) {
                    markSuspiciousDetailDirty("missing_detail_channels", -1.0f, -1.0f, (int)cachedPeaks.size());
                    cachedPeaks.clear();
                }
            }
            if (!cachedPeaks.empty() && (int)cachedPeaks.size() < expectedDetailFloats(fileChannels)) {
                markSuspiciousDetailDirty("too_small", -1.0f, -1.0f, (int)cachedPeaks.size());
                cachedPeaks.clear();
            }

            // 仅在存在矛盾证据时清理近零 detail 缓存，避免误伤真正的静音文件。
            if (!cachedPeaks.empty()) {
                cachedMinPeaks = g_appState->database.getWaveformMinDetailByPath(path);
                if (cachedMinPeaks.size() != cachedPeaks.size()) {
                    markSuspiciousDetailDirty(
                        cachedMinPeaks.empty() ? "missing_min_detail" : "bad_min_detail_size",
                        -1.0f, -1.0f, (int)cachedPeaks.size());
                    cachedPeaks.clear();
                    cachedMinPeaks.clear();
                }
            }

            auto sampleMaxAbs = [](const std::vector<float>& values, size_t stride) -> float {
                float maxAbs = 0.0f;
                if (stride == 0) stride = 1;
                for (size_t i = 0; i < values.size(); i += stride) {
                    maxAbs = (std::max)(maxAbs, std::abs(values[i]));
                }
                return maxAbs;
            };
            if (!cachedPeaks.empty()) {
                int miniChannels = 1;
                std::vector<float> cachedMiniPeaks =
                    g_appState->database.getWaveformByPath(path, nullptr, &miniChannels);
                const int detailChannelsForValidation =
                    (cachedDetailChannelsForHit > 0) ? cachedDetailChannelsForHit : fileChannels;
                const bool tailMismatch = IsMainDetailCacheContradictedByMiniEnvelope(
                    cachedPeaks,
                    detailChannelsForValidation,
                    cachedMiniPeaks,
                    miniChannels);
                const bool shapeMismatch =
                    !tailMismatch && IsMainDetailCacheShapeContradictedByMiniEnvelope(
                        cachedPeaks,
                        cachedMinPeaks,
                        detailChannelsForValidation,
                        cachedMiniPeaks,
                        miniChannels);
                const bool repeatedCopyMismatch =
                    !tailMismatch && !shapeMismatch &&
                    IsMainDetailCacheRepeatedCopyOfMiniEnvelope(
                        cachedPeaks,
                        cachedMinPeaks,
                        detailChannelsForValidation,
                        cachedMiniPeaks,
                        miniChannels);
                if (tailMismatch || shapeMismatch || repeatedCopyMismatch) {
                    const float cachedPeakMax = sampleMaxAbs(cachedPeaks, 8);
                    const float miniPeakMax = sampleMaxAbs(cachedMiniPeaks, 4);
                    markSuspiciousDetailDirty(
                        tailMismatch
                            ? "mini_detail_tail_mismatch"
                            : (shapeMismatch
                                ? "mini_detail_shape_mismatch"
                                : "mini_detail_repeated_copy"),
                        cachedPeakMax,
                        miniPeakMax,
                        (int)cachedPeaks.size());
                    cachedPeaks.clear();
                    cachedMinPeaks.clear();
                }
            }
            if (!cachedPeaks.empty()) {
                constexpr float kZeroCachePeakEpsilon = 1e-6f;
                constexpr float kSuspiciousDetailPeakCeiling = 0.12f;
                const float cachedPeakMax = sampleMaxAbs(cachedPeaks, 8);
                float miniPeakMax = 0.0f;
                bool hasContradictingMini = false;
                // Mini/detail comparison is disabled because waveformCache belongs
                // to the UI thread; the near-zero detail check remains lock-free.
                (void)miniPeakMax;
                (void)hasContradictingMini;

                const bool isNearZeroDetail = (cachedPeakMax <= kZeroCachePeakEpsilon);
                const bool isSeverelyAttenuatedDetail =
                    hasContradictingMini && cachedPeakMax <= kSuspiciousDetailPeakCeiling;

                if (isNearZeroDetail || isSeverelyAttenuatedDetail) {
                    markSuspiciousDetailDirty(
                        isNearZeroDetail ? "near_zero" : "attenuated_mismatch",
                        cachedPeakMax, miniPeakMax, (int)cachedPeaks.size());
                    cachedPeaks.clear();
                } else if (isNearZeroDetail || hasContradictingMini) {
                    DIAG_ACTION_FMT("Waveform",
                        "DetailCacheObserved: path=%s, detailMax=%.6f, miniMax=%.6f, contradictMini=%d",
                        path.c_str(), cachedPeakMax, miniPeakMax, hasContradictingMini ? 1 : 0);
                }
            }
            
            // Cached detail must contain req peaks per source channel; shorter
            // legacy payloads are discarded.
            int totalRequired = expectedDetailFloats(fileChannels);
            if (!cachedPeaks.empty() && (int)cachedPeaks.size() >= totalRequired) {
                // 防御：过小的 detail 缓存视为无效，避免主波形空白
                // 例如 peaksSize=2 或明显低于 req 的情况
                if ((int)cachedPeaks.size() < totalRequired) {
                    DIAG_ACTION_FMT("Waveform",
                        "CacheInvalidTooSmall: path=%s, peaksSize=%d, req=%d, fileCh(loadFile)=%d",
                        path.c_str(), (int)cachedPeaks.size(), req, fileChannels);
                } else {
                // 检查 RMS 模式是否需要真实 RMS 数据
                bool needRmsData = g_appState->settings.waveformStyle == 2;
                bool rmsOk = true;
                std::vector<float> cachedRms;
                if (needRmsData) {
                    cachedRms = g_appState->database.getRmsDataByPath(path);
                    if (cachedRms.empty()) {
                        rmsOk = false; // RMS 模式但无缓存 → 需要重新解码
                    }
                }

                if (needRmsData && cachedRms.empty()) {
                    needRmsData = false;
                    rmsOk = true;
                    DIAG_ACTION_FMT("Waveform",
                        "MainDetailCacheHitWithoutRms: path=%s peaksSize=%d",
                        path.c_str(), (int)cachedPeaks.size());
                }

                if (rmsOk) {
                    wf.peaks = std::move(cachedPeaks);
                    wf.minPeaks = std::move(cachedMinPeaks);
                    if (needRmsData) {
                        wf.rms = std::move(cachedRms);
                    } else {
                        wf.rms.clear(); // 非 RMS 模式不加载 RMS 数据
                    }

                    // 尝试读取响度（当前关闭，避免多余开销）
                    if (kEnableLufsDetection) {
                        float i=0, m=0, s=0, tp=0;
                        if (g_appState->database.getLoudnessByPath(path, i, m, s, tp)) {
                            wf.integratedLUFS = i;
                            wf.momentaryLUFS = m;
                            wf.shortTermLUFS = s;
                            wf.truePeakDBTP = tp;
                            wf.hasLoudnessData = true;
                        }
                    } else {
                        wf.hasLoudnessData = false;
                    }

                    wf.isReady = true;
                    wf.filePath = path;
                    wf.isProgressiveDetail = false;

                    // Derive samplesPerPeak from cached peaks + current totalFrames.
                    // Database only stores the peaks blob, not samplesPerPeak.
                    {
                        const int cachedPeakCount = (int)wf.peaks.size() / (std::max)(1, fileChannels);
                        if (cachedPeakCount > 0) {
                            std::lock_guard<std::mutex> lk(m_mutex);
                            const uint64_t tf = m_fileInfo.totalFrames;
                            wf.samplesPerPeak = (int)(tf / static_cast<uint64_t>(cachedPeakCount));
                            if (wf.samplesPerPeak < 1) wf.samplesPerPeak = 1;
                        }
                    }

                    // DB detail 缓存按 source/native 通道理解
                    {
                        int fileCh = 1;
                        {
                            std::lock_guard<std::mutex> lock(m_mutex);
                            fileCh = (int)m_fileInfo.channels;
                        }
                        if (fileCh < 1) fileCh = 1;
                        if (cachedDetailChannelsForHit > 0) {
                            fileCh = cachedDetailChannelsForHit;
                        }

                        if (!wf.peaks.empty() && wf.peaks.size() % fileCh == 0) {
                            wf.numChannels = fileCh;
                        } else {
                            wf.numChannels = (fileCh > 1) ? fileCh : 1;
                        }
                        wf.isStereo = (wf.numChannels == 2);
                    }
                    NormalizeFinalWaveformFrameMetadata(wf);

                    // 保存原始 source detail，随后按 mono/solo 即时变换显示
                    {
                        WaveformData source = wf;
                        source.filePath = path;
                        source.isReady = true;
                        sourceWaveformToStore = std::make_shared<WaveformData>(std::move(source));
                    }

                    wf = BuildDisplayWaveformFromSource(
                        *sourceWaveformToStore,
                        mono,
                        solo,
                        path
                    );
                    NormalizeFinalWaveformFrameMetadata(wf);

                    loadedFromDb = true;
                    finalPersistReason = "cache_hit";

                    // Inspect cached detail for zero-filled tails caused by an
                    // overstated frame count.
                    {
                        const int ch = wf.numChannels > 0 ? wf.numChannels : 1;
                        const int totalPeaks = (int)wf.peaks.size() / ch;
                        int lastNonZeroPeak = totalPeaks - 1;
                        for (; lastNonZeroPeak >= 0; --lastNonZeroPeak) {
                            bool hasSignal = false;
                            for (int c = 0; c < ch; ++c) {
                                if (std::abs(wf.peaks[(size_t)(lastNonZeroPeak * ch + c)]) > 1e-9f) {
                                    hasSignal = true;
                                    break;
                                }
                            }
                            if (hasSignal) break;
                        }
                        const int signalPeaks = lastNonZeroPeak + 1;
                        // Silence can be valid content, so heuristic stretching is
                        // disabled in production. Deterministic truncation repair uses
                        // decoder EOF evidence instead.
                        if (kEnableHeuristicSilenceStretch &&
                            signalPeaks > 0 && signalPeaks < (int)(totalPeaks * 0.85)) {
                            // 拉伸：把 signalPeaks 个有效 peak 用最近邻插值铺满 totalPeaks
                            std::vector<float> stretched((size_t)(totalPeaks * ch));
                            std::vector<float> stretchedMin;
                            std::vector<float> stretchedRms;
                            const bool hasMinPeaks = (wf.minPeaks.size() == wf.peaks.size());
                            const bool hasRms = (wf.rms.size() == wf.peaks.size());
                            if (hasMinPeaks) stretchedMin.resize(stretched.size());
                            if (hasRms) stretchedRms.resize(stretched.size());
                            for (int dst = 0; dst < totalPeaks; ++dst) {
                                int src = (int)((int64_t)dst * (int64_t)signalPeaks / (int64_t)totalPeaks);
                                if (src >= signalPeaks) src = signalPeaks - 1;
                                for (int c = 0; c < ch; ++c) {
                                    const size_t dstIdx = (size_t)(dst * ch + c);
                                    const size_t srcIdx = (size_t)(src * ch + c);
                                    stretched[dstIdx] = wf.peaks[srcIdx];
                                    if (hasMinPeaks) stretchedMin[dstIdx] = wf.minPeaks[srcIdx];
                                    if (hasRms) stretchedRms[dstIdx] = wf.rms[srcIdx];
                                }
                            }
                            wf.peaks = std::move(stretched);
                            if (hasMinPeaks) wf.minPeaks = std::move(stretchedMin);
                            if (hasRms) wf.rms = std::move(stretchedRms);
                            // Recompute samplesPerPeak from the detected signal extent.
                            const uint64_t realFrames = static_cast<uint64_t>(wf.samplesPerPeak) * static_cast<uint64_t>(signalPeaks);
                            wf.samplesPerPeak = (int)(realFrames / static_cast<uint64_t>(totalPeaks));
                            if (wf.samplesPerPeak < 1) wf.samplesPerPeak = 1;
                            // Decoder frame counts remain authoritative for ruler and
                            // playhead mapping; cache peak counts may not align exactly.
                            DIAG_ACTION_FMT("Waveform",
                                "CacheHitStretch: path=%s signalPeaks=%d totalPeaks=%d realFrames=%llu",
                                path.c_str(), signalPeaks, totalPeaks, (unsigned long long)realFrames);
                        }
                    }

                    // Detect all-zero cached peaks before publication.
                    normalizeWaveformTailPeaksForStableDisplay("cache_hit");

                    {
                        float maxPeakInCache = 0.0f;
                        for (size_t pi = 0; pi < wf.peaks.size(); ++pi) {
                            float a = std::abs(wf.peaks[pi]);
                            if (a > maxPeakInCache) maxPeakInCache = a;
                            if (maxPeakInCache > 1e-9f) break; // 找到非零值，提前退出
                        }
                        const char* cacheScope =
                            (appFileId > 0) ? "library" :
                            (g_appState && g_appState->filePlayerFastPathActive) ? "external" : "path";
                        DIAG_ACTION_FMT("Waveform",
                            "CacheHitDiag: path=%s, scope=%s, currentFileId=%lld, peaksSize=%d, numCh=%d, fileCh(loadFile)=%d, maxPeak=%.6f, isReady=%d",
                            path.c_str(), cacheScope, (long long)appFileId,
                            (int)wf.peaks.size(), wf.numChannels, fileChannels, maxPeakInCache, (int)wf.isReady);
                    }

                    DIAG_ACTION_FMT("WaveformLane",
                        "lane=cache_hit req=%d persist=1 loadedFromDb=1 path=%s",
                        req, path.c_str());
                    DIAG_METRIC("Waveform", "MainDetailCacheHit", 1.0);
                }
                }
                // else: rmsOk==false → loadedFromDb 保持 false，走真解码路径
            }
        }
        if (!loadedFromDb) {
            const int64_t appFileId = g_appState ? g_appState->currentFileId : -2;
            DIAG_ACTION_FMT("Waveform",
                "MainDetailCacheMissDetail: path=%s, currentFileId=%lld, dbOpen=%d, fastPath=%d, req=%d",
                path.c_str(),
                (long long)appFileId,
                (g_appState && g_appState->database.isOpen()) ? 1 : 0,
                (g_appState && g_appState->filePlayerFastPathActive) ? 1 : 0,
                req);
            DIAG_ACTION_FMT("WaveformLane",
                "lane=cache_miss req=%d persist=0 loadedFromDb=0 path=%s",
                req, path.c_str());
            DIAG_METRIC("Waveform", "MainDetailCacheMiss", 1.0);
            DIAG_SYSTEM_FMT("Waveform",
                "MainDetailDecodeStart: path=%s req=%d token=%llu channels=%d totalFrames=%llu",
                path.c_str(),
                req,
                (unsigned long long)workerToken,
                fileChannels,
                (unsigned long long)fileTotalFramesSnapshot);
        }

        // 4. 只有数据库没货时，直接进入最终高精度解码。
        // 不再先发 quick preview 粗图，避免"先粗后细"的停留感。
        if (!loadedFromDb) {
            // Click-time detail decode remains enabled while scan pre-caching is active.
            // Files missed during scanning can still produce high-resolution waveforms.
            bool cacheOnlyMode = false; 
            
            if (cacheOnlyMode && !kEnableJitWaveformFallback) {
                // In strict cache-only mode, retain any visible waveform instead of
                // replacing it with an empty placeholder on a cache miss.
                bool hasUsableCurrentWaveform = false;
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    auto current = std::atomic_load(&m_waveform);
                    hasUsableCurrentWaveform =
                        current &&
                        current->isReady &&
                        !current->peaks.empty() &&
                        (current->filePath.empty() || current->filePath == path);
                }

                if (!hasUsableCurrentWaveform) {
                    // If strict req misses, still try to use any cached detail to avoid blank waveform.
                    bool publishedAnyCachedDetail = false;
                    if (g_appState && g_appState->database.isOpen()) {
                        std::vector<float> anyCachedPeaks;
                        if (g_dirtyDetailPaths.find(path) != g_dirtyDetailPaths.end()) {
                            DIAG_ACTION_FMT("Waveform", "DirtyDetailCacheBypassFallback: path=%s", path.c_str());
                        } else {
                            anyCachedPeaks = g_appState->database.getWaveformDetailByPath(path);
                        }
                        if (!anyCachedPeaks.empty()) {
                            const int storedDetailChannels =
                                g_appState->database.getWaveformDetailChannelsByPath(path);
                            WaveformData fallbackCached;
                            fallbackCached.peaks = std::move(anyCachedPeaks);
                            fallbackCached.rms.clear();
                            fallbackCached.filePath = path;
                            fallbackCached.isReady = true;
                            fallbackCached.numChannels = (std::max)(1, storedDetailChannels > 0 ? storedDetailChannels : 1);
                            if (fallbackCached.numChannels > 1 &&
                                (int)(fallbackCached.peaks.size() % (size_t)fallbackCached.numChannels) != 0) {
                                fallbackCached.numChannels = 1;
                            }
                            fallbackCached.isStereo = (fallbackCached.numChannels == 2);
                            const int frames = (int)fallbackCached.peaks.size() / (std::max)(1, fallbackCached.numChannels);
                            fallbackCached.samplesPerPeak = (frames > 0) ? (int)((std::max)(1.0f, (float)frames / 1000.0f)) : 1;

                            {
                                std::lock_guard<std::mutex> lock(m_mutex);
                                if (m_isLoaded && m_fileInfo.filePath == path &&
                                    m_waveformRequestToken.load(std::memory_order_relaxed) == workerToken) {
                                    std::shared_ptr<const WaveformData> p =
                                        std::make_shared<WaveformData>(std::move(fallbackCached));
                                    std::atomic_store(&m_waveform, p);
                                    publishedAnyCachedDetail = true;
                                }
                            }
                        }
                    }

                    if (!publishedAnyCachedDetail) {
                        WaveformData placeholder;
                        const int ch = (std::max)(1, fileChannels);
                        placeholder.peaks.assign((size_t)req * (size_t)ch, 0.0f);
                        placeholder.rms.clear();
                        placeholder.samplesPerPeak = 1;
                        placeholder.numChannels = ch;
                        placeholder.isStereo = (ch == 2);
                        placeholder.filePath = path;
                        placeholder.isReady = true;
                        {
                            std::lock_guard<std::mutex> lock(m_mutex);
                            if (m_isLoaded && m_fileInfo.filePath == path &&
                                m_waveformRequestToken.load(std::memory_order_relaxed) == workerToken) {
                                std::shared_ptr<const WaveformData> p =
                                    std::make_shared<WaveformData>(std::move(placeholder));
                                std::atomic_store(&m_waveform, p);
                            }
                        }
                    }
                } else {
                    DIAG_ACTION_FMT("Waveform",
                                    "AOTCacheOnlyMissKeepCurrent: %s (req=%d)",
                                    path.c_str(), req);
                }

                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    if (m_isLoaded && m_fileInfo.filePath == path &&
                        m_waveformRequestToken.load(std::memory_order_relaxed) == workerToken) {
                        m_waveformGeneratedPeaks.store(req);
                    }
                }
                if (g_appState &&
                    g_appState->database.isOpen() &&
                    !ShouldDeferAudioEngineWaveformDbWrites()) {
                    g_appState->database.upsertWaveformCacheStateByPath(
                        path, "pending", "cache_miss_pending_build", 0, "");
                }
                DIAG_ACTION_FMT("Waveform", "AOTCacheOnlyMiss: %s", path.c_str());
                DIAG_ACTION_FMT("WaveformLane",
                    "lane=fallback req=%d persist=0 reason=aot_cache_only_miss path=%s",
                    req, path.c_str());
                continue;
            }
            // 检查文件是否仍然匹配
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                if (!m_isLoaded || m_fileInfo.filePath != path) {
                    m_waveformGeneratedPeaks.store(req);
                    continue;
                }
            }

            auto publishFallbackWaveform = [&](const char* reason) {
                // Publish an incomplete marker without zero-filled peaks.
                WaveformData failed;
                failed.filePath = path;
                failed.isReady = false;
                // This marker remains eligible for a later decode attempt.
                failed.isFallbackPlaceholder = true; 
                failed.numChannels = (std::max)(1, fileChannels);
                failed.isStereo = (failed.numChannels == 2);
                failed.hasLoudnessData = false;
                failed.samplesPerPeak = 0;
                failed.peaks.clear();
                failed.rms.clear();

                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    if (m_isLoaded &&
                        m_fileInfo.filePath == path &&
                        m_waveformRequestToken.load(std::memory_order_relaxed) == workerToken) {
                        std::shared_ptr<const WaveformData> p =
                            std::make_shared<WaveformData>(std::move(failed));
                        std::atomic_store(&m_waveform, p);
                    }
                }

                DIAG_ACTION_FMT("Waveform",
                    "MainWaveformFallbackNoPlaceholder: path=%s, reason=%s",
                    path.c_str(), reason ? reason : "unknown");
                DIAG_ACTION_FMT("WaveformLane",
                    "lane=fallback req=%d persist=0 reason=%s path=%s",
                    req, reason ? reason : "unknown", path.c_str());
            };

            // 4b. 精确解码 (ProgressiveMainWaveDecode) — 读完写入 DB
            const bool needRms = g_appState && g_appState->settings.waveformStyle == 2;
            auto extractNativeDetailToWaveform = [&]() -> bool {
                WaveformExtractResult extracted;
                if (!WaveformExtractor::ExtractDetail(path, req, needRms, extracted) ||
                    extracted.peaks.empty()) {
                    DIAG_ACTION_FMT("Waveform",
                        "NativeDetailExtractFailed: path=%s error=%s",
                        path.c_str(), extracted.errorCode.c_str());
                    return false;
                }

                wf.peaks = std::move(extracted.peaks);
                wf.minPeaks = std::move(extracted.minPeaks);
                wf.rms = needRms ? std::move(extracted.rms) : std::vector<float>{};
                wf.samplesPerPeak = extracted.samplesPerPeak;
                wf.numChannels = (std::max)(1, extracted.channels);
                wf.isStereo = (wf.numChannels == 2);
                wf.filePath = path;
                wf.trueRmsDbfs = extracted.trueRmsDbfs;
                wf.hasTrueRms = extracted.hasTrueRms;
                wf.isReady = true;
                wf.isFallbackPlaceholder = false;

                const int completed = (int)wf.peaks.size() / (std::max)(1, wf.numChannels);
                if (extracted.decodedFrames > 0 && extracted.sampleRate > 0) {
                    std::lock_guard<std::mutex> lk(m_mutex);
                    if (m_isLoaded && m_fileInfo.filePath == path &&
                        m_waveformRequestToken.load(std::memory_order_relaxed) == workerToken) {
                        const double decodedDurationSec =
                            (double)extracted.decodedFrames / (double)extracted.sampleRate;
                        const uint32_t playbackSampleRate =
                            m_fileInfo.sampleRate > 0 ? m_fileInfo.sampleRate : extracted.sampleRate;
                        const uint64_t playbackFrames = ConvertFrameCountToSampleRateBasis(
                            extracted.decodedFrames,
                            extracted.sampleRate,
                            playbackSampleRate);
                        if (playbackFrames > 0) {
                            m_fileInfo.totalFrames = playbackFrames;
                            if (completed > 0) {
                                const uint64_t spp64 = playbackFrames / static_cast<uint64_t>(completed);
                                wf.samplesPerPeak = (spp64 > static_cast<uint64_t>(INT_MAX))
                                    ? INT_MAX
                                    : (int)(std::max<uint64_t>(1, spp64));
                            }
                            // Push-mode decoders may reveal frame count only after detail
                            // extraction. Apply the seek deferred during load.
                            const float deferredSeek =
                                m_deferredSeekNorm.exchange(-1.0f, std::memory_order_relaxed);
                            if (deferredSeek >= 0.0f && m_sound) {
                                const double norm = std::clamp((double)deferredSeek, 0.0, 1.0);
                                const uint64_t maxFrame = playbackFrames > 0 ? playbackFrames - 1 : 0;
                                const uint64_t targetFrame =
                                    (uint64_t)std::llround(norm * (double)maxFrame);
                                Platform::AudioPlayback::SetStopTimeInPcmFrames(m_sound->Borrow(), static_cast<std::uint64_t>(-1));
                                (void)Platform::AudioPlayback::SeekToFrame(m_sound->Borrow(), targetFrame);
                                DIAG_ACTION_FMT("Audio",
                                    "DeferredSeekApplied: path=%s norm=%.4f frame=%llu totalFrames=%llu",
                                    path.c_str(), deferredSeek,
                                    (unsigned long long)targetFrame,
                                    (unsigned long long)playbackFrames);
                            }
                        }
                        m_fileInfo.durationSeconds = (float)decodedDurationSec;
                    }
                }

                DIAG_ACTION_FMT("Waveform",
                    "NativeDetailExtracted: path=%s peaks=%d channels=%d samplesPerPeak=%d decodedFrames=%llu",
                    path.c_str(), completed, wf.numChannels, wf.samplesPerPeak,
                    (unsigned long long)extracted.decodedFrames);
                return completed > 0;
            };

            if (HarkenSearch::MediaFoundationAudio::CanDecodeExtension(path)) {
                if (!extractNativeDetailToWaveform()) {
                    publishFallbackWaveform("media_foundation_native_detail_failed");
                    m_waveformGeneratedPeaks.store(req);
                    continue;
                }

                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    const uint64_t currentToken = m_waveformRequestToken.load(std::memory_order_seq_cst);
                    if (m_isLoaded && m_fileInfo.filePath == path && currentToken == workerToken) {
                        const int finalTargetPeaks =
                            (wf.numChannels > 0)
                                ? (int)wf.peaks.size() / wf.numChannels
                                : (int)wf.peaks.size();
                        wf.targetPeaks = finalTargetPeaks;
                        wf.completedPeaks = finalTargetPeaks;
                        wf.isProgressiveDetail = false;
                        std::shared_ptr<const WaveformData> p =
                            std::make_shared<WaveformData>(wf);
                        std::atomic_store(&m_waveformSource, p);
                        std::atomic_store(&m_waveform, p);
                        m_waveformGeneratedPeaks.store(req);
                        DIAG_ACTION_FMT("Waveform",
                            "MediaFoundationMainDetailExtracted: path=%s format=%s req=%d peaks=%d channels=%d",
                            path.c_str(),
                            HarkenSearch::MediaFoundationAudio::FormatNameFromPath(path),
                            req,
                            (int)wf.peaks.size(),
                            wf.numChannels);
                    }
                }
                continue;
            }

            DIAG_SCOPED_DETAIL("Waveform", "ProgressiveMainWaveDecode");
            DIAG_ACTION_FMT("WaveformLane",
                "lane=progressive req=%d persist=%d path=%s",
                req, ShouldDeferAudioEngineWaveformDbWrites() ? 0 : 1, path.c_str());

            Platform::AudioDataSource::Decoder decoder;
            const std::uint32_t engineSR = Platform::AudioPlayback::QueryEngineSampleRate(engine);
            const std::uint32_t wfChannels = (mono || solo != 0) ? 1 : 0;
            const auto decoderInit = Platform::AudioDataSource::InitializeDecoderFromFileWithFormatAndChannelMap(
                decoder, path, wfChannels, engineSR, solo);
            int result = decoderInit.backendCode;
            if (result != MA_SUCCESS) {
                DIAG_ACTION_FMT("Waveform",
                    "WaveformDecoderInitFailed: path=%s, ma_result=%d",
                    path.c_str(), (int)result);
                publishFallbackWaveform("decoder_init_failed");
                m_waveformGeneratedPeaks.store(req);
                continue;
            }

            const auto decoderFormat = Platform::AudioDataSource::QueryDataFormat(
                Platform::AudioDataSource::BorrowDataSource(decoder));
            const uint32_t actualChannels = decoderFormat.channels;
            const uint32_t decoderSampleRate = decoderFormat.sampleRate;
            if (actualChannels == 0) {
                DIAG_ACTION_FMT("Waveform",
                    "WaveformDecoderChannelsZero: path=%s", path.c_str());
                Platform::AudioDataSource::UninitializeDecoder(decoder);
                publishFallbackWaveform("decoder_output_channels_zero");
                m_waveformGeneratedPeaks.store(req);
                continue;
            }

            uint64_t totalFrames =
                Platform::AudioDataSource::QueryLength(
                    Platform::AudioDataSource::BorrowDataSource(decoder)).frames;
            if (totalFrames == 0) {
                // OGG/Vorbis via stb_vorbis push mode (Windows wide path) always returns 0
                // for ma_decoder_get_length_in_pcm_frames. Fall back to the sequential
                // sample-accurate extractor which does not require a frame count up front.
                DIAG_ACTION_FMT("Waveform",
                    "WaveformDecoderTotalFramesZero_ExtractorFallback: path=%s channels=%u sr=%u",
                    path.c_str(), actualChannels, decoderSampleRate);
                Platform::AudioDataSource::UninitializeDecoder(decoder);
                if (extractNativeDetailToWaveform()) {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    const uint64_t currentToken =
                        m_waveformRequestToken.load(std::memory_order_seq_cst);
                    if (m_isLoaded && m_fileInfo.filePath == path && currentToken == workerToken) {
                        const int finalTargetPeaks = (wf.numChannels > 0)
                            ? (int)wf.peaks.size() / wf.numChannels
                            : (int)wf.peaks.size();
                        wf.targetPeaks = finalTargetPeaks;
                        wf.completedPeaks = finalTargetPeaks;
                        wf.isProgressiveDetail = false;
                        std::shared_ptr<const WaveformData> p =
                            std::make_shared<WaveformData>(wf);
                        std::atomic_store(&m_waveformSource, p);
                        std::atomic_store(&m_waveform, p);
                        m_waveformGeneratedPeaks.store(req);
                    }
                } else {
                    publishFallbackWaveform("decoder_total_frames_zero");
                    m_waveformGeneratedPeaks.store(req);
                }
                continue;
            }

            const int samplesPerPeak = ComputeWaveformSamplesPerPeak(totalFrames, req);
            const int coveredPeakCount = ComputeWaveformCoveredPeakCount(totalFrames, samplesPerPeak);

            DIAG_ACTION_FMT("Waveform",
                "DecodeSetup: path=%s totalFrames=%llu engineSR=%u fileCh=%u samplesPerPeak=%d req=%d durationEst=%.2fs",
                path.c_str(), (unsigned long long)totalFrames, engineSR,
                actualChannels, samplesPerPeak, req,
                (double)totalFrames / (double)(engineSR > 0 ? engineSR : 48000));

            // RMS is only needed for style 2; other waveform styles skip this pass.
            // 安全 LOD：仅在超长总览时启用 seek 近似读取；RMS 模式始终保持精确。
            static constexpr uint64_t kLodReadWindowFrames = 2048;
            const bool allowSeekLod =
                kAllowMainWaveSeekLod &&
                !needRms &&
                req <= AppState::WAVEFORM_DEFAULT_DETAIL_PEAKS &&
                (uint64_t)samplesPerPeak > (kLodReadWindowFrames * 8);

            wf.peaks.assign((size_t)req * (size_t)actualChannels, 0.0f);
            wf.peaks.reserve((size_t)(std::max)(req, coveredPeakCount) * (size_t)actualChannels);
            wf.minPeaks.assign((size_t)req * (size_t)actualChannels, 0.0f);
            wf.minPeaks.reserve((size_t)(std::max)(req, coveredPeakCount) * (size_t)actualChannels);
            if (needRms)
                wf.rms.assign((size_t)req * (size_t)actualChannels, 0.0f);
            else
                wf.rms.clear();
            if (needRms) {
                wf.rms.reserve((size_t)(std::max)(req, coveredPeakCount) * (size_t)actualChannels);
            }
            wf.samplesPerPeak = samplesPerPeak;
            wf.numChannels = (int)actualChannels;
            wf.isStereo = (actualChannels == 2);
            wf.filePath = path;

            static constexpr uint64_t kWaveChunkFrames = 32768;  // balanced I/O and cancellation latency
            std::vector<float> buffer((size_t)kWaveChunkFrames * (size_t)actualChannels);
            std::vector<float> peakByCh((size_t)actualChannels, 0.0f);
            std::vector<float> minPeakByCh((size_t)actualChannels, 0.0f);
            std::vector<double> sumSqByCh;
            if (needRms) sumSqByCh.assign((size_t)actualChannels, 0.0);
            // 全文件 RMS dBFS 累加器（零额外解码开销，顺便算）
            const bool needTrueRmsForMainWave = needRms;
            double totalSigmaX2 = 0.0;
            uint64_t totalDecodedSamples = 0;
            int completedPeaks = 0;
            bool aborted = false;
            bool fallbackNeeded = false;
            const char* fallbackReason = nullptr;
            bool useSeekLod = allowSeekLod;
            auto decodeStartTime = std::chrono::steady_clock::now();
            auto lastProgressPublish = decodeStartTime;
            bool usedNativeDetailExtraction = false;

            const bool enableProgressiveForThisFile =
                ShouldEnableProgressiveMainWaveForFile(
                    totalFrames,
                    decoderSampleRate,
                    req,
                    loadedFromDb);

            // 首发再保守一点：
            // 1) 更大的 warmup，避免第一眼轮廓太粗
            // 2) 增加最小首发延迟，避免"极快闪一下"
            bool hasPublishedPreview = false;
            const int requestedPeaksSnapshot = req;
            const int warmupPeaks = MainWaveformMinProgressivePreviewPeaks(requestedPeaksSnapshot);
            const auto minFirstPublishDelay = std::chrono::milliseconds(40);
            const auto progressivePublishInterval = std::chrono::milliseconds(120);

            if (!useSeekLod) {
                bool extractedByNativeDetail = false;
                const bool preferStreamingProgressiveDecode = enableProgressiveForThisFile;
                if (preferStreamingProgressiveDecode) {
                    DIAG_ACTION_FMT("Waveform",
                        "NativeDetailSkippedForProgressive: path=%s durationSec=%.2f req=%d",
                        path.c_str(),
                        (double)totalFrames / (double)(decoderSampleRate > 0 ? decoderSampleRate : 48000),
                        req);
                } else {
                    WaveformExtractResult extracted;
                    if (WaveformExtractor::ExtractDetail(path, req, needRms, extracted) &&
                        !extracted.peaks.empty()) {
                        wf.peaks = std::move(extracted.peaks);
                        wf.minPeaks = std::move(extracted.minPeaks);
                        wf.rms = needRms ? std::move(extracted.rms) : std::vector<float>{};
                        wf.samplesPerPeak = extracted.samplesPerPeak;
                        wf.numChannels = (std::max)(1, extracted.channels);
                        wf.isStereo = (wf.numChannels == 2);
                        wf.filePath = path;
                        wf.trueRmsDbfs = extracted.trueRmsDbfs;
                        wf.hasTrueRms = extracted.hasTrueRms;
                        completedPeaks = (int)wf.peaks.size() / (std::max)(1, wf.numChannels);
                        extractedByNativeDetail = completedPeaks > 0;
                        usedNativeDetailExtraction = extractedByNativeDetail;
                        if (extracted.decodedFrames > 0 && extracted.sampleRate > 0) {
                            std::lock_guard<std::mutex> lk(m_mutex);
                            if (m_isLoaded && m_fileInfo.filePath == path &&
                                m_waveformRequestToken.load(std::memory_order_relaxed) == workerToken) {
                                const double decodedDurationSec =
                                    (double)extracted.decodedFrames / (double)extracted.sampleRate;
                                const uint32_t playbackSampleRate =
                                    m_fileInfo.sampleRate > 0 ? m_fileInfo.sampleRate : extracted.sampleRate;
                                const uint64_t playbackFrames = ConvertFrameCountToSampleRateBasis(
                                    extracted.decodedFrames,
                                    extracted.sampleRate,
                                    playbackSampleRate);
                                if (playbackFrames > 0) {
                                    m_fileInfo.totalFrames = playbackFrames;
                                    if (completedPeaks > 0) {
                                        const uint64_t spp64 =
                                            playbackFrames / static_cast<uint64_t>(completedPeaks);
                                        wf.samplesPerPeak = (spp64 > static_cast<uint64_t>(INT_MAX))
                                            ? INT_MAX
                                            : (int)(std::max<uint64_t>(1, spp64));
                                    }
                                }
                                m_fileInfo.durationSeconds = (float)decodedDurationSec;
                                if (playbackFrames > 0 && playbackFrames != extracted.decodedFrames) {
                                    DIAG_ACTION_FMT("AudioAlign",
                                        "NativeDetailFrameBasisConverted: path=%s decodedFrames=%llu decodedSR=%u playbackFrames=%llu playbackSR=%u",
                                        path.c_str(),
                                        (unsigned long long)extracted.decodedFrames,
                                        extracted.sampleRate,
                                        (unsigned long long)playbackFrames,
                                        playbackSampleRate);
                                }
                            }
                        }
                        DIAG_ACTION_FMT("Waveform",
                            "NativeDetailExtracted: path=%s peaks=%d channels=%d samplesPerPeak=%d decodedFrames=%llu",
                            path.c_str(), completedPeaks, wf.numChannels, wf.samplesPerPeak,
                            (unsigned long long)extracted.decodedFrames);
                    } else {
                        DIAG_ACTION_FMT("Waveform",
                            "NativeDetailExtractFailed: path=%s error=%s",
                            path.c_str(), extracted.errorCode.c_str());
                    }
                }
                if (!extractedByNativeDetail) {
                auto publishStreamingProgress = [&](bool forceFinal) {
                    if (!enableProgressiveForThisFile) return;
                    const auto now = std::chrono::steady_clock::now();
                    const bool enoughForFirstPublish = (completedPeaks >= warmupPeaks);
                    const bool delayedEnoughForFirstPublish =
                        (now - decodeStartTime) >= minFirstPublishDelay;
                    const bool timeDrivenPublish =
                        (now - lastProgressPublish) >= progressivePublishInterval;
                    if ((!hasPublishedPreview && enoughForFirstPublish && delayedEnoughForFirstPublish) ||
                        (hasPublishedPreview && timeDrivenPublish) ||
                        forceFinal) {
                        WaveformData partial = wf;
                        partial.isReady = true;
                        partial.targetPeaks = requestedPeaksSnapshot;
                        partial.completedPeaks = forceFinal
                            ? requestedPeaksSnapshot
                            : (std::min)(completedPeaks, requestedPeaksSnapshot);
                        partial.isProgressiveDetail = !forceFinal;

                        std::lock_guard<std::mutex> lock(m_mutex);
                        if (m_isLoaded &&
                            m_fileInfo.filePath == path &&
                            m_waveformRequestToken.load(std::memory_order_relaxed) == workerToken) {
                            std::shared_ptr<const WaveformData> p =
                                std::make_shared<WaveformData>(std::move(partial));
                            std::atomic_store(&m_waveform, p);
                            DIAG_ACTION_FMT("Waveform",
                                "MainDetailProgressivePublish: path=%s completed=%d target=%d final=%d first=%d",
                                path.c_str(),
                                forceFinal ? requestedPeaksSnapshot : (std::min)(completedPeaks, requestedPeaksSnapshot),
                                requestedPeaksSnapshot,
                                forceFinal ? 1 : 0,
                                hasPublishedPreview ? 0 : 1);
                            lastProgressPublish = now;
                            hasPublishedPreview = true;
                        }
                    }
                };

                auto commitStreamingPeak = [&](uint64_t usedFrames) {
                    if (usedFrames == 0) return;
                    const bool append = completedPeaks >= requestedPeaksSnapshot;
                    const int base = completedPeaks * (int)actualChannels;
                    for (uint32_t ch = 0; ch < actualChannels; ++ch) {
                        if (append) {
                            wf.peaks.push_back(peakByCh[(size_t)ch]);
                            wf.minPeaks.push_back(minPeakByCh[(size_t)ch]);
                            if (needRms) {
                                wf.rms.push_back((float)std::sqrt(sumSqByCh[(size_t)ch] / (double)usedFrames));
                            }
                        } else {
                            wf.peaks[(size_t)(base + (int)ch)] = peakByCh[(size_t)ch];
                            wf.minPeaks[(size_t)(base + (int)ch)] = minPeakByCh[(size_t)ch];
                            if (needRms) {
                                wf.rms[(size_t)(base + (int)ch)] =
                                    (float)std::sqrt(sumSqByCh[(size_t)ch] / (double)usedFrames);
                            }
                        }
                        peakByCh[(size_t)ch] = 0.0f;
                        minPeakByCh[(size_t)ch] = 0.0f;
                        if (needRms) {
                            sumSqByCh[(size_t)ch] = 0.0;
                        }
                    }
                    ++completedPeaks;
                    if (completedPeaks < requestedPeaksSnapshot) {
                        publishStreamingProgress(false);
                    }
                };

                uint64_t blockFrames = 0;
                while (true) {
                    if (m_waveformRequestToken.load(std::memory_order_seq_cst) != workerToken ||
                        m_waveformRequestedPeaks.load() > req) {
                        aborted = true;
                        break;
                    }

                    const auto readResult = Platform::AudioDataSource::ReadPcmFrames(
                        Platform::AudioDataSource::BorrowDataSource(decoder),
                        buffer.data(),
                        kWaveChunkFrames);
                    uint64_t framesRead = readResult.framesRead;
                    if (framesRead == 0) break;

                    for (uint64_t i = 0; i < framesRead; ++i) {
                        for (uint32_t ch = 0; ch < actualChannels; ++ch) {
                            const float s = buffer[(size_t)(i * actualChannels + ch)];
                            if (s > peakByCh[(size_t)ch]) peakByCh[(size_t)ch] = s;
                            if (s < minPeakByCh[(size_t)ch]) minPeakByCh[(size_t)ch] = s;
                            if (needRms) {
                                sumSqByCh[(size_t)ch] += (double)s * (double)s;
                            }
                            if (needTrueRmsForMainWave) {
                                const double sd = (double)s;
                                totalSigmaX2 += sd * sd;
                            }
                        }
                        if (needTrueRmsForMainWave) {
                            totalDecodedSamples += actualChannels;
                        }
                        ++blockFrames;
                        if (blockFrames >= (uint64_t)samplesPerPeak) {
                            commitStreamingPeak(blockFrames);
                            blockFrames = 0;
                        }
                    }
                }

                if (!aborted && blockFrames > 0) {
                    commitStreamingPeak(blockFrames);
                }
                if (!aborted) {
                    if (completedPeaks > 0 && completedPeaks < requestedPeaksSnapshot) {
                        const size_t actualValues = (size_t)completedPeaks * (size_t)actualChannels;
                        wf.peaks.resize(actualValues);
                        wf.minPeaks.resize(actualValues);
                        if (needRms && wf.rms.size() >= actualValues) {
                            wf.rms.resize(actualValues);
                        }
                    }
                    normalizeWaveformTailPeaksForStableDisplay("streaming_final");
                    publishStreamingProgress(true);
                }
                }
            }

            if (useSeekLod) {
            for (int peakIdx = 0; peakIdx < req; ++peakIdx) {
                // 用户切换文件或出现新精度请求时，尽快中止当前任务
                // 每 8 个 peak 检查一次（约 ~8ms），比之前每 32 个更快响应切歌
                if ((peakIdx & 7) == 0) {
                    if (m_waveformRequestedPeaks.load() > req) {
                        aborted = true;
                    }
                    if (m_waveformRequestToken.load(std::memory_order_seq_cst) != workerToken) {
                        aborted = true;
                    }
                    if (aborted) break;
                }

                const int base = peakIdx * (int)actualChannels;
                std::fill(peakByCh.begin(), peakByCh.end(), 0.0f);
                std::fill(minPeakByCh.begin(), minPeakByCh.end(), 0.0f);
                if (needRms) {
                    std::fill(sumSqByCh.begin(), sumSqByCh.end(), 0.0);
                }
                uint64_t framesRemaining = (uint64_t)samplesPerPeak;
                uint64_t actualFramesForPeak = 0;
                if (useSeekLod) {
                    const uint64_t segStart = (uint64_t)peakIdx * (uint64_t)samplesPerPeak;
                    const uint64_t center = segStart + ((uint64_t)samplesPerPeak / 2);
                    const uint64_t halfWindow = kLodReadWindowFrames / 2;
                    const uint64_t maxSeekStart =
                        (totalFrames > kLodReadWindowFrames) ? (totalFrames - kLodReadWindowFrames) : 0;
                    uint64_t seekStart = (center > halfWindow) ? (center - halfWindow) : 0;
                    if (seekStart > maxSeekStart) seekStart = maxSeekStart;
                    if (Platform::AudioDataSource::SeekPcmFrame(
                            Platform::AudioDataSource::BorrowDataSource(decoder),
                            seekStart).Succeeded()) {
                        framesRemaining = (totalFrames >= kLodReadWindowFrames) ? kLodReadWindowFrames : totalFrames;
                    } else {
                        useSeekLod = false;
                        uint64_t preciseStart = segStart;
                        if (preciseStart > totalFrames) preciseStart = totalFrames;
                        if (!Platform::AudioDataSource::SeekPcmFrame(
                                Platform::AudioDataSource::BorrowDataSource(decoder),
                                preciseStart).Succeeded()) {
                            fallbackNeeded = true;
                            fallbackReason = "decoder_seek_failed";
                            aborted = true;
                            break;
                        }
                        framesRemaining = (uint64_t)samplesPerPeak;
                    }
                }

                while (framesRemaining > 0) {
                    if (m_waveformRequestToken.load(std::memory_order_seq_cst) != workerToken) {
                        aborted = true;
                        break;
                    }
                    if (m_waveformRequestedPeaks.load() > req) {
                        aborted = true;
                        break;
                    }
                    if (aborted) break;

                    uint64_t toRead = (std::min)(framesRemaining, kWaveChunkFrames);
                    const auto readResult = Platform::AudioDataSource::ReadPcmFrames(
                        Platform::AudioDataSource::BorrowDataSource(decoder),
                        buffer.data(),
                        toRead);
                    uint64_t framesRead = readResult.framesRead;
                    if (framesRead == 0) {
                        const auto currentCursor =
                            Platform::AudioDataSource::QueryCursor(
                                Platform::AudioDataSource::BorrowDataSource(decoder));
                        if (currentCursor.Succeeded() && currentCursor.frame < totalFrames) {
                            DIAG_ACTION_FMT(
                                "Waveform",
                                "PrematureEOF_MainWave: path=%s cursor=%llu totalFrames=%llu peakIdx=%d",
                                path.c_str(),
                                (unsigned long long)currentCursor.frame,
                                (unsigned long long)totalFrames,
                                peakIdx);
                        }
                        break;
                    }

                    for (uint64_t i = 0; i < framesRead; ++i) {
                        for (uint32_t ch = 0; ch < actualChannels; ++ch) {
                            float s = buffer[(size_t)(i * actualChannels + ch)];
                            if (s > peakByCh[(size_t)ch]) peakByCh[(size_t)ch] = s;
                            if (s < minPeakByCh[(size_t)ch]) minPeakByCh[(size_t)ch] = s;
                            if (needRms) {
                                sumSqByCh[(size_t)ch] += (double)s * (double)s;
                            }
                            if (needTrueRmsForMainWave) {
                                double sd = (double)s;
                                totalSigmaX2 += sd * sd;
                            }
                        }
                        if (needTrueRmsForMainWave) {
                            totalDecodedSamples += actualChannels;
                        }
                    }

                    actualFramesForPeak += framesRead;
                    if (framesRead >= framesRemaining) {
                        framesRemaining = 0;
                    } else {
                        framesRemaining -= framesRead;
                    }
                }

                if (aborted) break;
                if (actualFramesForPeak == 0) {
                    if (peakIdx > 0) {
                        uint64_t realTotalFrames = (uint64_t)peakIdx * (uint64_t)samplesPerPeak;
                        const auto cursorFrames =
                            Platform::AudioDataSource::QueryCursor(
                                Platform::AudioDataSource::BorrowDataSource(decoder));
                        if (cursorFrames.Succeeded() && cursorFrames.frame > 0) {
                            realTotalFrames = cursorFrames.frame;
                        }
                        DIAG_ACTION_FMT(
                            "Waveform",
                            "TruncatedAudioMetadata: path=%s metaFrames=%llu physicalFrames=%llu peaksKept=%d reqWas=%d",
                            path.c_str(),
                            (unsigned long long)totalFrames,
                            (unsigned long long)realTotalFrames,
                            peakIdx,
                            requestedPeaksSnapshot);

                        const size_t srcPeaks = (size_t)peakIdx;
                        const size_t srcSamples = srcPeaks * (size_t)actualChannels;
                        const size_t dstPeaks = (size_t)requestedPeaksSnapshot;

                        // When enabled, nearest-neighbor expansion maps decoded peaks
                        // across the requested width without changing duration selection.
                        if (kEnableWaveformVisualStretchRepair && srcPeaks > 0 && srcPeaks < dstPeaks) {
                            std::vector<float> stretched(dstPeaks * (size_t)actualChannels, 0.0f);
                            std::vector<float> stretchedMin(dstPeaks * (size_t)actualChannels, 0.0f);
                            std::vector<float> stretchedRms;
                            if (needRms) stretchedRms.assign(dstPeaks * (size_t)actualChannels, 0.0f);

                            const bool hasMinPeaks = (wf.minPeaks.size() == wf.peaks.size());
                            for (size_t dst = 0; dst < dstPeaks; ++dst) {
                                const size_t src = dst * srcPeaks / dstPeaks;
                                for (uint32_t ch = 0; ch < actualChannels; ++ch) {
                                    stretched[dst * actualChannels + ch] =
                                        wf.peaks[src * actualChannels + ch];
                                    if (hasMinPeaks)
                                        stretchedMin[dst * actualChannels + ch] =
                                            wf.minPeaks[src * actualChannels + ch];
                                    if (needRms) {
                                        stretchedRms[dst * actualChannels + ch] =
                                            wf.rms[src * actualChannels + ch];
                                    }
                                }
                            }
                            wf.peaks = std::move(stretched);
                            wf.minPeaks = std::move(stretchedMin);
                            if (needRms) wf.rms = std::move(stretchedRms);
                            req = requestedPeaksSnapshot;
                        } else {
                            req = peakIdx;
                            wf.peaks.resize(srcSamples);
                            wf.minPeaks.resize(srcSamples);
                            if (needRms) wf.rms.resize(srcSamples);
                        }

                        {
                            const uint64_t spp64 = realTotalFrames / static_cast<uint64_t>((std::max)(1, req));
                            wf.samplesPerPeak = (spp64 > static_cast<uint64_t>(INT_MAX))
                                ? INT_MAX
                                : (int)(std::max<uint64_t>(1, spp64));
                        }

                        const uint32_t sr =
                            (decoderSampleRate > 0) ? decoderSampleRate
                            : [&]() -> uint32_t {
                                  std::lock_guard<std::mutex> lk(m_mutex);
                                  return (m_fileInfo.sampleRate > 0) ? m_fileInfo.sampleRate : 48000u;
                              }();
                        {
                            std::lock_guard<std::mutex> lk(m_mutex);
                            if (m_isLoaded && m_fileInfo.filePath == path &&
                                m_waveformRequestToken.load(std::memory_order_relaxed) == workerToken) {
                                m_fileInfo.totalFrames = realTotalFrames;
                                m_fileInfo.durationSeconds = (float)((double)realTotalFrames / (double)sr);
                            }
                        }
                    }
                    break;
                }

                for (uint32_t ch = 0; ch < actualChannels; ++ch) {
                    wf.peaks[(size_t)(base + (int)ch)] = peakByCh[(size_t)ch];
                    wf.minPeaks[(size_t)(base + (int)ch)] = minPeakByCh[(size_t)ch];

                    if (needRms) {
                        wf.rms[(size_t)(base + (int)ch)] =
                            (float)std::sqrt(sumSqByCh[(size_t)ch] / (double)actualFramesForPeak);
                    }
                }
                completedPeaks = peakIdx + 1;

                const auto now = std::chrono::steady_clock::now();
                if (enableProgressiveForThisFile) {
                    const bool isLast = (completedPeaks == req);
                    const bool enoughForFirstPublish = (completedPeaks >= warmupPeaks);
                    const bool delayedEnoughForFirstPublish =
                        (now - decodeStartTime) >= minFirstPublishDelay;
                    const bool timeDrivenPublish =
                        (now - lastProgressPublish) >= progressivePublishInterval;

                    if ((!hasPublishedPreview && enoughForFirstPublish && delayedEnoughForFirstPublish) ||
                        (hasPublishedPreview && timeDrivenPublish) ||
                        isLast) {
                        WaveformData partial = wf;
                        partial.isReady = true;
                        partial.targetPeaks = requestedPeaksSnapshot;
                        partial.completedPeaks = isLast ? requestedPeaksSnapshot : completedPeaks;
                        partial.isProgressiveDetail = !isLast;

                        std::lock_guard<std::mutex> lock(m_mutex);
                        if (m_isLoaded &&
                            m_fileInfo.filePath == path &&
                            m_waveformRequestToken.load(std::memory_order_relaxed) == workerToken) {
                            std::shared_ptr<const WaveformData> p =
                                std::make_shared<WaveformData>(std::move(partial));
                            std::atomic_store(&m_waveform, p);
                            DIAG_ACTION_FMT("Waveform",
                                "MainDetailProgressivePublish: path=%s completed=%d target=%d final=%d first=%d",
                                path.c_str(),
                                isLast ? requestedPeaksSnapshot : completedPeaks,
                                requestedPeaksSnapshot,
                                isLast ? 1 : 0,
                                hasPublishedPreview ? 0 : 1);
                            lastProgressPublish = now;
                            hasPublishedPreview = true;
                        }
                    }
                }
            }
            }

            if (!aborted && !fallbackNeeded && !useSeekLod &&
                !usedNativeDetailExtraction &&
                completedPeaks == requestedPeaksSnapshot &&
                coveredPeakCount > requestedPeaksSnapshot) {
                std::fill(peakByCh.begin(), peakByCh.end(), 0.0f);
                std::fill(minPeakByCh.begin(), minPeakByCh.end(), 0.0f);
                if (needRms) {
                    std::fill(sumSqByCh.begin(), sumSqByCh.end(), 0.0);
                }

                uint64_t blockFrames = 0;
                auto appendTailPeak = [&](uint64_t usedFrames) {
                    if (usedFrames == 0) return;
                    for (uint32_t ch = 0; ch < actualChannels; ++ch) {
                        wf.peaks.push_back(peakByCh[(size_t)ch]);
                        wf.minPeaks.push_back(minPeakByCh[(size_t)ch]);
                        if (needRms) {
                            wf.rms.push_back((float)std::sqrt(sumSqByCh[(size_t)ch] / (double)usedFrames));
                        }
                        peakByCh[(size_t)ch] = 0.0f;
                        minPeakByCh[(size_t)ch] = 0.0f;
                        if (needRms) {
                            sumSqByCh[(size_t)ch] = 0.0;
                        }
                    }
                    ++completedPeaks;
                };

                while (true) {
                    const auto readResult = Platform::AudioDataSource::ReadPcmFrames(
                        Platform::AudioDataSource::BorrowDataSource(decoder),
                        buffer.data(),
                        kWaveChunkFrames);
                    uint64_t framesRead = readResult.framesRead;
                    if (framesRead == 0) break;

                    for (uint64_t i = 0; i < framesRead; ++i) {
                        for (uint32_t ch = 0; ch < actualChannels; ++ch) {
                            const float s = buffer[(size_t)(i * actualChannels + ch)];
                            if (s > peakByCh[(size_t)ch]) peakByCh[(size_t)ch] = s;
                            if (s < minPeakByCh[(size_t)ch]) minPeakByCh[(size_t)ch] = s;
                            if (needRms) {
                                sumSqByCh[(size_t)ch] += (double)s * (double)s;
                            }
                            if (needTrueRmsForMainWave) {
                                const double sd = (double)s;
                                totalSigmaX2 += sd * sd;
                            }
                        }
                        if (needTrueRmsForMainWave) {
                            totalDecodedSamples += actualChannels;
                        }
                        ++blockFrames;
                        if (blockFrames >= (uint64_t)samplesPerPeak) {
                            appendTailPeak(blockFrames);
                            blockFrames = 0;
                        }
                    }
                }

                if (blockFrames > 0) {
                    appendTailPeak(blockFrames);
                }
            }

            Platform::AudioDataSource::UninitializeDecoder(decoder);

            // Some decoders emit low-level padding instead of EOF. Scan backward
            // with a 0.002 threshold to locate the last meaningful peak.
            if (!aborted && !fallbackNeeded && completedPeaks > 0 &&
                completedPeaks == requestedPeaksSnapshot) {
                // 先算前 10% 的平均峰值作为参照
                const int headCount = (std::max)(1, completedPeaks / 10);
                double headSum = 0.0;
                for (int i = 0; i < headCount; ++i) {
                    for (uint32_t ch = 0; ch < actualChannels; ++ch) {
                        headSum += (double)std::abs(wf.peaks[(size_t)(i * (int)actualChannels + (int)ch)]);
                    }
                }
                const float headAvg = (float)(headSum / ((double)headCount * (double)actualChannels));

                // 自适应阈值：取前段平均的 1%，至少 0.002
                const float kSilenceThreshold = (std::max)(0.002f, headAvg * 0.01f);

                int lastSignalPeak = completedPeaks - 1;
                for (int i = completedPeaks - 1; i >= 0; --i) {
                    bool hasSignal = false;
                    for (uint32_t ch = 0; ch < actualChannels; ++ch) {
                        if (std::abs(wf.peaks[(size_t)(i * (int)actualChannels + (int)ch)]) > kSilenceThreshold) {
                            hasSignal = true;
                            break;
                        }
                    }
                    if (hasSignal) { lastSignalPeak = i; break; }
                    if (i == 0) lastSignalPeak = 0;
                }

                const int signalPeaks = lastSignalPeak + 1;
                // 旧实现：信号只占不到 80% 且 headAvg > silence 阈值 → 拉伸尾部静音段。
                // 对带 tail silence 的合法 SFX 同样误判，且拉伸结果会被写进 cache 长期污染。
                // 改由 kEnableHeuristicSilenceStretch 控制（生产值 = false）。BWF FACT 截断
                // 走 line 1740 的 TruncatedAudioMetadata 路径（确定性 EOF，无误伤风险）。
                if (kEnableHeuristicSilenceStretch &&
                    signalPeaks > 0 && signalPeaks < (int)(completedPeaks * 0.8) && headAvg > kSilenceThreshold) {
                    DIAG_ACTION_FMT("Waveform",
                        "SilencePaddingDetected: path=%s signalPeaks=%d totalPeaks=%d, stretching",
                        path.c_str(), signalPeaks, completedPeaks);

                    const size_t srcPeaks = (size_t)signalPeaks;
                    const size_t dstPeaks = (size_t)requestedPeaksSnapshot;
                    std::vector<float> stretched(dstPeaks * (size_t)actualChannels, 0.0f);
                    std::vector<float> stretchedMin(dstPeaks * (size_t)actualChannels, 0.0f);
                    std::vector<float> stretchedRms;
                    if (needRms) stretchedRms.assign(dstPeaks * (size_t)actualChannels, 0.0f);

                    const bool hasMinPeaks = (wf.minPeaks.size() == wf.peaks.size());
                    for (size_t dst = 0; dst < dstPeaks; ++dst) {
                        const size_t src = dst * srcPeaks / dstPeaks;
                        for (uint32_t ch = 0; ch < actualChannels; ++ch) {
                            stretched[dst * actualChannels + ch] =
                                wf.peaks[src * actualChannels + ch];
                            if (hasMinPeaks)
                                stretchedMin[dst * actualChannels + ch] =
                                    wf.minPeaks[src * actualChannels + ch];
                            if (needRms) {
                                stretchedRms[dst * actualChannels + ch] =
                                    wf.rms[src * actualChannels + ch];
                            }
                        }
                    }
                    wf.peaks = std::move(stretched);
                    wf.minPeaks = std::move(stretchedMin);
                    if (needRms) wf.rms = std::move(stretchedRms);

                    // 修正 duration 和 samplesPerPeak
                    const uint64_t realFrames = (uint64_t)signalPeaks * (uint64_t)samplesPerPeak;
                    {
                        const uint64_t spp64 = realFrames / (uint64_t)requestedPeaksSnapshot;
                        wf.samplesPerPeak = (spp64 > (uint64_t)INT_MAX)
                            ? INT_MAX : (int)(std::max)((uint64_t)1, spp64);
                    }
                    const uint32_t sr =
                        (decoderSampleRate > 0) ? decoderSampleRate
                        : [&]() -> uint32_t {
                              std::lock_guard<std::mutex> lk(m_mutex);
                              return (m_fileInfo.sampleRate > 0) ? m_fileInfo.sampleRate : 48000u;
                          }();
                    {
                        std::lock_guard<std::mutex> lk(m_mutex);
                        if (m_isLoaded && m_fileInfo.filePath == path &&
                            m_waveformRequestToken.load(std::memory_order_relaxed) == workerToken) {
                            m_fileInfo.totalFrames = realFrames;
                            m_fileInfo.durationSeconds = (float)((double)realFrames / (double)sr);
                        }
                    }
                }
            }

            if (fallbackNeeded) {
                publishFallbackWaveform(fallbackReason ? fallbackReason : "seek_failed_unknown");
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    if (m_isLoaded && m_fileInfo.filePath == path) {
                        m_waveformGeneratedPeaks.store(req);
                    }
                }
                continue;
            }

            if (aborted) {
                continue;
            }

            if (completedPeaks <= 0) {
                publishFallbackWaveform("decode_no_frames");
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    if (m_isLoaded && m_fileInfo.filePath == path) {
                        m_waveformGeneratedPeaks.store(req);
                    }
                }
                continue;
            }

            // 计算真实 RMS dBFS（全文件原始 PCM，匹配 ffmpeg astats）
            if (totalDecodedSamples > 0) {
                double rmsLinear = std::sqrt(totalSigmaX2 / (double)totalDecodedSamples);
                wf.trueRmsDbfs = (rmsLinear > 1e-10)
                    ? std::clamp((float)(20.0 * std::log10(rmsLinear)), -100.0f, 0.0f)
                    : -100.0f;
                wf.hasTrueRms = true;
            }

            normalizeWaveformTailPeaksForStableDisplay("decode_final");

            wf.isReady = true;
            wf.isFallbackPlaceholder = false;
            const bool deferDetailPersist = ShouldDeferAudioEngineWaveformDbWritesForBulkWork();
            const bool deferLowPriorityDbWrites = ShouldDeferAudioEngineWaveformDbWrites();
            if (deferDetailPersist) {
                finalPersistReason = "db_write_deferred";
            } else if (useSeekLod) {
                finalPersistReason = "seek_lod";
            } else if (!g_appState || !g_appState->database.isOpen()) {
                finalPersistReason = "db_not_open";
            }

            // 5. 完整结果入库（仅最终态，避免把渐进中间态写进 DB）
            if (g_appState && g_appState->database.isOpen()) {
                // LOD 近似结果仅用于首屏显示，不写入 detail 缓存，避免污染长期精度。
                if (!deferDetailPersist && !useSeekLod) {
                    float decodedMaxPeak = 0.0f;
                    for (float p : wf.peaks) {
                        decodedMaxPeak = (std::max)(decodedMaxPeak, std::abs(p));
                    }
                    DIAG_ACTION_FMT("Waveform",
                        "DecodedDetailDiag: path=%s, decodedMaxPeak=%.6f, peaksSize=%d, channels=%u, req=%d",
                        path.c_str(), decodedMaxPeak, (int)wf.peaks.size(), actualChannels, req);
                    finalPersistReason = "playback_detail_session_only";
                    DIAG_ACTION_FMT("Waveform",
                        "PlaybackDetailPersistSkipped: path=%s peaksSize=%d channels=%u",
                        path.c_str(), (int)wf.peaks.size(), actualChannels);
                }

                if (!deferLowPriorityDbWrites && wf.hasLoudnessData) {
                    g_appState->database.saveLoudnessByPath(path,
                        wf.integratedLUFS, wf.momentaryLUFS, wf.shortTermLUFS, wf.truePeakDBTP);
                }
            }
        }

        // 6. 提交结果给界面显示
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            // Reject results superseded while waiting for m_mutex.
            const uint64_t currentToken = m_waveformRequestToken.load(std::memory_order_seq_cst);
            if (m_isLoaded &&
                m_fileInfo.filePath == path &&
                currentToken == workerToken) {

                // 先写回 source waveform：
                // 1) DB 命中路径：sourceWaveformToStore 已经准备好了
                // 2) fresh decode 且当前是正常立体声视图：直接把最终 detail 结果当作 source 保存
                if (sourceWaveformToStore &&
                    sourceWaveformToStore->isReady &&
                    !sourceWaveformToStore->peaks.empty()) {
                    std::atomic_store(&m_waveformSource, sourceWaveformToStore);
                } else if (!mono && solo == 0 && wf.isReady && !wf.peaks.empty()) {
                    const int finalTargetPeaks =
                        (wf.numChannels > 0)
                            ? (int)wf.peaks.size() / wf.numChannels
                            : (int)wf.peaks.size();
                    wf.targetPeaks = finalTargetPeaks;
                    wf.completedPeaks = finalTargetPeaks;
                    wf.isProgressiveDetail = false;
                    std::shared_ptr<const WaveformData> src =
                        std::make_shared<WaveformData>(wf);
                    std::atomic_store(&m_waveformSource, src);
                }

                const int finalTargetPeaks =
                    (wf.numChannels > 0)
                        ? (int)wf.peaks.size() / wf.numChannels
                        : (int)wf.peaks.size();
                wf.targetPeaks = finalTargetPeaks;
                wf.completedPeaks = finalTargetPeaks;
                wf.isProgressiveDetail = false;
                // 最小验证诊断：确认是“日志时机”还是“真的发布空波形”（不改逻辑）
                const size_t peaksBeforeMove = wf.peaks.size();
                std::shared_ptr<const WaveformData> p =
                    std::make_shared<WaveformData>(std::move(wf));
                const size_t peaksInShared = p->peaks.size();
                const size_t peaksBeforeStore = p->peaks.size();
                DIAG_ACTION_FMT("Waveform",
                    "PublishDiag: pathSuffix=%s, workerToken=%llu, currentToken=%llu, peaksBeforeMove=%zu, peaksInShared=%zu, peaksBeforeStore=%zu",
                    path.size() > 60 ? path.c_str() + path.size() - 60 : path.c_str(),
                    (unsigned long long)workerToken,
                    (unsigned long long)currentToken,
                    peaksBeforeMove,
                    peaksInShared,
                    peaksBeforeStore);
                // Alignment diagnostic: compare waveform basis vs playback basis
                {
                    const int wfCh = p->numChannels > 0 ? p->numChannels : 1;
                    const int wfPeaks = (int)p->peaks.size() / wfCh;
                    const uint64_t wfTotalFrames = static_cast<uint64_t>(p->samplesPerPeak) * static_cast<uint64_t>(wfPeaks);
                    const bool hasMinPeaks = !p->minPeaks.empty() && p->minPeaks.size() == p->peaks.size();
                    DIAG_ACTION_FMT("AudioAlign",
                        "WaveformPublish: path=%s spp=%d peaks=%d wfTotalFrames=%llu fileInfoTotalFrames=%llu fileInfoDur=%.4fs fileInfoSR=%u loadedFromDb=%d hasMinPeaks=%d minPeaksSize=%d mono=%d solo=%d",
                        (path.size() > 60 ? path.c_str() + path.size() - 60 : path.c_str()),
                        p->samplesPerPeak, wfPeaks,
                        (unsigned long long)wfTotalFrames,
                        (unsigned long long)m_fileInfo.totalFrames,
                        m_fileInfo.durationSeconds,
                        m_fileInfo.sampleRate,
                        (int)loadedFromDb,
                        (int)hasMinPeaks,
                        (int)p->minPeaks.size(),
                        (int)mono,
                        solo);
                }
                const int finalGeneratedPeaks =
                    (p->numChannels > 0)
                        ? (int)p->peaks.size() / p->numChannels
                        : (int)p->peaks.size();
                std::atomic_store(&m_waveform, p);
                m_waveformGeneratedPeaks.store(finalGeneratedPeaks);
                DIAG_ACTION_FMT("Waveform",
                    "MainDetailReady path=%s peaks=%d loadedFromDb=%d",
                    path.c_str(),
                    finalGeneratedPeaks,
                    loadedFromDb ? 1 : 0);
                DIAG_SYSTEM_FMT("Waveform",
                    "MainDetailFinalPublish: path=%s peaks=%d loadedFromDb=%d persistAttempted=%d persisted=%d persistReason=%s",
                    path.c_str(),
                    finalGeneratedPeaks,
                    loadedFromDb ? 1 : 0,
                    finalPersistAttempted ? 1 : 0,
                    finalPersisted ? 1 : 0,
                    finalPersistReason);
                DIAG_ACTION_FMT("WaveformLane",
                    "lane=publish req=%d persist=%d loadedFromDb=%d peaks=%d path=%s",
                    req,
                    ShouldDeferAudioEngineWaveformDbWrites() ? 0 : 1,
                    loadedFromDb ? 1 : 0,
                    finalGeneratedPeaks,
                    path.c_str());
                DIAG_METRIC("Waveform", "MainDetailReady", 1.0);
            } else {
                // 🔍 诊断：波形结果被丢弃的原因
                DIAG_ACTION_FMT("Waveform",
                    "WaveformResultDiscarded: path=%s, loaded=%d, pathMatch=%d, tokenMatch=%d (worker=%llu, current=%llu), peaksSize=%d",
                    path.c_str(),
                    (int)m_isLoaded,
                    (int)(m_fileInfo.filePath == path),
                    (int)(currentToken == workerToken),
                    (unsigned long long)workerToken,
                    (unsigned long long)currentToken,
                    (int)wf.peaks.size());
            }
        }
    }
}

bool AudioEngine::loadFile(const std::string& filePath) {
    DIAG_SCOPED_DETAIL("Audio", "loadFile");
    DIAG_ACTION_FMT("Audio", "LoadFile: %s", filePath.c_str());
    std::unique_lock<std::mutex> lock(m_mutex);

    if (!m_isInitialized) return false;

    if (m_sound) {
        // Retire old sound + decoder to deferred cleanup queue.
        // If fading, schedule cleanup after fade completes; otherwise clean immediately.
        // This avoids blocking the UI thread and eliminates the UAF risk from detached threads.
        const bool fadeEnabled = m_fadeEnabled.load();
        int fadeMs = m_seekFadeMs.load();
        if (fadeMs < 0) fadeMs = 0;
        if (fadeMs > 60) fadeMs = 60;

        auto oldSound = std::move(m_sound);
        auto oldDecoder = std::move(m_decoder);
        PcmAudioBufferState* oldPcm = m_pcmState;
        CenterSenseDataSource* oldCSDS = m_centerSenseDataSource;
        m_pcmState = nullptr;
        m_centerSenseDataSource = nullptr;

        if (fadeEnabled && fadeMs > 0 && Platform::AudioPlayback::IsPlaying(oldSound->Borrow())) {
            Platform::AudioPlayback::SetFadeInMilliseconds(oldSound->Borrow(), -1.0f, 0.0f, static_cast<std::uint64_t>(fadeMs));
            DeferredCleanup dc;
            dc.sound = std::move(oldSound);
            dc.decoder = std::move(oldDecoder);
            dc.pcm = oldPcm;
            dc.csds = oldCSDS;
            dc.deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(fadeMs + 20);
            try {
                m_cleanupQueue.push_back(std::move(dc));
            } catch (...) {
                if (dc.sound) {
                    (void)Platform::AudioPlayback::Stop(dc.sound->Borrow());
                    dc.sound.reset();
                }
                if (oldCSDS) { oldCSDS->uninit(); delete oldCSDS; }
                dc.decoder.reset();
                DestroyPcmAudioBufferState(oldPcm);
            }
        } else {
            (void)Platform::AudioPlayback::Stop(oldSound->Borrow());
            oldSound.reset();
            if (oldCSDS) { oldCSDS->uninit(); delete oldCSDS; }
            oldDecoder.reset();
            DestroyPcmAudioBufferState(oldPcm);
        }
    } else if (m_decoder) {
        if (m_centerSenseDataSource) {
            m_centerSenseDataSource->uninit();
            delete m_centerSenseDataSource;
            m_centerSenseDataSource = nullptr;
        }
        m_decoder.reset();
    }
    if (m_pcmState) {
        DestroyPcmAudioBufferState(m_pcmState);
        m_pcmState = nullptr;
    }
    // 框选拖入 DAW 只导出选区：同文件重载时保留 in/out，避免调整 Pitch 等触发重载后误导出整轨（Bug: 只有框选处正确、其余削波）
    const std::string oldPath = m_fileInfo.filePath;
    const float savedIn = m_inPoint;
    const float savedOut = m_outPoint;
    constexpr float kRegionPreserveEpsilon = 0.001f;
    const bool preserveRegion = (filePath == oldPath && (savedIn > kRegionPreserveEpsilon || savedOut < (1.0f - kRegionPreserveEpsilon)));

    resetState(false);

    m_decoder = std::make_unique<Platform::AudioDataSource::Decoder>();
    std::uint32_t engineSR = Platform::AudioPlayback::QueryEngineSampleRate(m_engine->Borrow());
    if (filePath != oldPath) {
        m_channelSolo = 0;
    }

    // Solo modes output as mono (centered), forceMono also outputs mono
    uint32_t outChannels = (m_forceMono || m_channelSolo != 0) ? 1 : 2;
    const auto decoderInit = Platform::AudioDataSource::InitializeDecoderFromFileWithFormatAndChannelMap(
        *m_decoder, filePath, outChannels, engineSR, m_channelSolo);
    int result = decoderInit.backendCode;
    if (result != MA_SUCCESS) {
        m_decoder.reset();
        if (HarkenSearch::MediaFoundationAudio::CanDecodeExtension(filePath)) {
            HarkenSearch::MediaFoundationAudio::DecodedAudioBuffer decoded;
            std::string mfError;
            if (HarkenSearch::MediaFoundationAudio::Decode(filePath, decoded, &mfError)) {
                AudioFileInfo info;
                info.filePath = filePath;
#ifdef _WIN32
                std::filesystem::path fsPath(Utf8ToWide(filePath));
                std::wstring wfilename = fsPath.filename().wstring();
                info.fileName = Platform::TextEncoding::WideToUtf8Permissive(wfilename);
#else
                std::filesystem::path fsPath(filePath);
                info.fileName = fsPath.filename().string();
#endif
                info.sampleRate = decoded.sampleRate;
                info.channels = decoded.channels;
                info.totalFrames = decoded.totalFrames;
                info.durationSeconds = decoded.durationSeconds;
                info.format = HarkenSearch::MediaFoundationAudio::FormatNameFromPath(filePath);
                if (installDecodedPcmSourceLocked(info, std::move(decoded.interleaved))) {
                    if (preserveRegion) {
                        m_inPoint = savedIn;
                        m_outPoint = savedOut;
                    }
                    DIAG_SYSTEM_FMT("Audio",
                        "MediaFoundationLoadDecodedPCMReady: format=%s durationSec=%.4f channels=%u path=%s",
                        info.format.c_str(),
                        info.durationSeconds,
                        info.channels,
                        filePath.c_str());
                    return true;
                }
            }
            DIAG_SYSTEM_FMT("Audio",
                "MediaFoundationLoadDecodedPCMFailed: format=%s error=%s path=%s",
                HarkenSearch::MediaFoundationAudio::FormatNameFromPath(filePath),
                HarkenSearch::MediaFoundationAudio::ClassifyFailure(mfError).c_str(),
                filePath.c_str());
        }
        return false;
    }

    const auto decoderFormat =
        Platform::AudioDataSource::QueryDataFormat(
            Platform::AudioDataSource::BorrowDecoder(*m_decoder));
// 立体声一律走中心感链路，旋钮后续调节才生效；k=0 时 DSP 内部 no-op.
    const bool useCenterSense = decoderFormat.channels == 1 ||
        (outChannels == 2 && decoderFormat.channels == 2);

    if (useCenterSense) {
        m_centerSenseDataSource = new CenterSenseDataSource();
        if (!m_centerSenseDataSource->init(Platform::AudioDataSource::BorrowDataSource(*m_decoder), engineSR, &m_centerSense, &m_bassMonoFreq, &m_freqShiftHz, &m_pitchMode, &m_magicEQRealtime)) {
            delete m_centerSenseDataSource;
            m_centerSenseDataSource = nullptr;
            m_decoder.reset();
            return false;
        }
    }

    m_sound = std::make_unique<Platform::AudioPlayback::Sound>();
    const auto soundInitResult = m_sound->Initialize(
        m_engine->Borrow(),
        Platform::AudioPlayback::BorrowDataSource(
            useCenterSense ? m_centerSenseDataSource->getDataSource()
                           : m_decoder->NativeHandle()),
        Platform::AudioPlayback::SoundInitFlags::NoSpatialization);

    if (!soundInitResult.Succeeded()) {
        if (m_centerSenseDataSource) {
            m_centerSenseDataSource->uninit();
            delete m_centerSenseDataSource;
            m_centerSenseDataSource = nullptr;
        }
        m_decoder.reset();
        m_sound.reset();
        return false;
    }

    m_fileInfo.filePath = filePath;

#ifdef _WIN32
    std::wstring wpath = Utf8ToWide(filePath);
    std::filesystem::path fsPath(wpath);
    std::wstring wfilename = fsPath.filename().wstring();
    std::wstring wext = fsPath.extension().wstring();
    m_fileInfo.fileName = Platform::TextEncoding::WideToUtf8Permissive(wfilename);
    std::string ext = Platform::TextEncoding::WideToUtf8Permissive(wext);
#else
    std::filesystem::path fsPath(filePath);
    m_fileInfo.fileName = fsPath.filename().string();
    std::string ext = fsPath.extension().string();
#endif
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

    uint32_t sampleRate = engineSR;
    m_fileInfo.sampleRate = sampleRate;
    m_playbackChannels = decoderFormat.channels;
    enforceMagicEQAvailabilityLocked();

    uint64_t length =
        Platform::AudioDataSource::QueryLength(
            Platform::AudioDataSource::BorrowDecoder(*m_decoder)).frames;
    m_fileInfo.totalFrames = length;
    m_decodedPlaybackTotalFrames =
        ResolveDecodedPlaybackTotalFramesFromSyncLoad(length, 0);

    // Probe source channels separately because the playback decoder outputs stereo.
    // The source count is required to interpret multichannel waveform caches.
    {
        Platform::AudioDataSource::Decoder probe;
        const auto probeResult = Platform::AudioDataSource::InitializeDecoderFromFile(
            probe, filePath);
        if (probeResult.Succeeded()) {
            const auto probeFormat = Platform::AudioDataSource::QueryDataFormat(
                Platform::AudioDataSource::BorrowDataSource(probe));
            m_fileInfo.channels = probeFormat.channels;
            Platform::AudioDataSource::UninitializeDecoder(probe);
        } else {
            // 探测失败时回退到播放解码器的声道数
            const auto format =
                Platform::AudioDataSource::QueryDataFormat(
                    Platform::AudioDataSource::BorrowDecoder(*m_decoder));
            m_fileInfo.channels = format.channels;
        }
    }

    if (sampleRate > 0) {
        m_fileInfo.durationSeconds = (float)length / (float)sampleRate;
    }
    if (ext == ".wav") m_fileInfo.format = "WAV";
    else if (ext == ".mp3") m_fileInfo.format = "MP3";
    else if (ext == ".flac") m_fileInfo.format = "FLAC";
    else if (ext == ".ogg") m_fileInfo.format = "OGG";
    else if (ext == ".m4a") m_fileInfo.format = "M4A";
    else if (ext == ".aac") m_fileInfo.format = "AAC";
    else if (ext == ".wma") m_fileInfo.format = "WMA";
    else m_fileInfo.format = "Unknown";

    Platform::AudioPlayback::SetVolume(m_sound->Borrow(), std::max(0.0f, std::min(1.2f, m_volume * m_replayGain)));
    // Solo 或 Mono 模式输出单声道，pan 强制居中
    Platform::AudioPlayback::SetPan(
        m_sound->Borrow(),
        (m_forceMono || m_channelSolo != 0) ? 0.0f : m_pan);

    {
        float pitchMultiplier = (m_pitchMode == PitchMode::FreqShift) ? 1.0f : std::pow(2.0f, m_pitch / 12.0f);
        Platform::AudioPlayback::SetPitch(
            m_sound->Borrow(), pitchMultiplier);
    }

    if (preserveRegion) {
        m_inPoint = savedIn;
        m_outPoint = savedOut;
    }

    m_isLoaded = true;


    // Diagnostic: log key playback alignment metrics per file
    {
        const auto format =
            Platform::AudioDataSource::QueryDataFormat(
                Platform::AudioDataSource::BorrowDecoder(*m_decoder));
        const std::uint32_t internalSR = format.sampleRate;
        DIAG_ACTION_FMT("AudioAlign",
            "LoadFile: path=%s engineSR=%u internalSR=%u totalFrames=%llu dur=%.4fs ch=%u fmt=%s",
            filePath.c_str(), engineSR, internalSR,
            (unsigned long long)m_fileInfo.totalFrames,
            m_fileInfo.durationSeconds,
            m_fileInfo.channels,
            m_fileInfo.format.c_str());
    }

    return true;
}

bool AudioEngine::installDecodedPcmSourceLocked(const AudioFileInfo& fileInfo, std::vector<float>&& pcm) {
    if (!m_engine || fileInfo.channels == 0 || fileInfo.totalFrames == 0 || pcm.empty()) {
        return false;
    }

    PcmAudioBufferState* state = new PcmAudioBufferState();
    state->samples = std::move(pcm);
    const auto bufferInitResult = Platform::AudioDataSource::InitializePcmBuffer(
        state->buffer,
        fileInfo.channels,
        fileInfo.totalFrames,
        state->samples.data());
    if (!bufferInitResult.Succeeded()) {
        delete state;
        return false;
    }
    state->initialized = true;

    std::uint32_t sr = Platform::AudioPlayback::QueryEngineSampleRate(m_engine->Borrow());
    if (sr == 0) sr = fileInfo.sampleRate > 0 ? fileInfo.sampleRate : 48000;
    m_centerSenseDataSource = new CenterSenseDataSource();
    if (!m_centerSenseDataSource->init(
            Platform::AudioDataSource::BorrowDataSource(state->buffer), sr,
            &m_centerSense, &m_bassMonoFreq, &m_freqShiftHz, &m_pitchMode,
            &m_magicEQRealtime)) {
        delete m_centerSenseDataSource;
        m_centerSenseDataSource = nullptr;
        Platform::AudioDataSource::UninitializePcmBuffer(state->buffer);
        delete state;
        return false;
    }

    auto sound = std::make_unique<Platform::AudioPlayback::Sound>();
    const auto soundInitResult = sound->Initialize(
        m_engine->Borrow(),
        Platform::AudioPlayback::BorrowDataSource(m_centerSenseDataSource->getDataSource()),
        Platform::AudioPlayback::SoundInitFlags::NoSpatialization);
    if (!soundInitResult.Succeeded()) {
        m_centerSenseDataSource->uninit();
        delete m_centerSenseDataSource;
        m_centerSenseDataSource = nullptr;
        Platform::AudioDataSource::UninitializePcmBuffer(state->buffer);
        delete state;
        return false;
    }

    m_pcmState = state;
    m_sound = std::move(sound);
    m_decoder = nullptr;
    m_fileInfo = fileInfo;
    m_playbackChannels = fileInfo.channels;
    enforceMagicEQAvailabilityLocked();
    const uint64_t pcmFrames =
        (fileInfo.channels > 0) ? (state->samples.size() / fileInfo.channels) : 0;
    m_decodedPlaybackTotalFrames =
        (pcmFrames > 0) ? pcmFrames : fileInfo.totalFrames;

    Platform::AudioPlayback::SetStopTimeInPcmFrames(m_sound->Borrow(), static_cast<std::uint64_t>(-1));
    Platform::AudioPlayback::SetVolume(m_sound->Borrow(), std::max(0.0f, std::min(1.2f, m_volume * m_replayGain)));
    Platform::AudioPlayback::SetPan(
        m_sound->Borrow(),
        (m_forceMono || m_channelSolo != 0) ? 0.0f : m_pan);
    {
        float pitchMultiplier = (m_pitchMode == PitchMode::FreqShift) ? 1.0f : std::pow(2.0f, m_pitch / 12.0f);
        Platform::AudioPlayback::SetPitch(
            m_sound->Borrow(), pitchMultiplier);
    }
    Platform::AudioPlayback::SetFadeInMilliseconds(m_sound->Borrow(), 1.0f, 1.0f, 0);
    m_isLoaded = true;

    DIAG_ACTION_FMT("AudioAlign",
        "LoadDecodedPCM: path=%s sr=%u ch=%u totalFrames=%llu dur=%.4fs fmt=%s",
        fileInfo.filePath.c_str(),
        fileInfo.sampleRate,
        fileInfo.channels,
        (unsigned long long)fileInfo.totalFrames,
        fileInfo.durationSeconds,
        fileInfo.format.c_str());
    return true;
}

void AudioEngine::unload() {
    stopSeekWorker();
    stopWaveformWorker();
    if (m_sound) {
        (void)Platform::AudioPlayback::Stop(m_sound->Borrow());
        m_sound.reset();
    }
    if (m_centerSenseDataSource) {
        m_centerSenseDataSource->uninit();
        delete m_centerSenseDataSource;
        m_centerSenseDataSource = nullptr;
    }
    if (m_decoder) {
        m_decoder.reset();
    }
    if (m_pcmState) {
        DestroyPcmAudioBufferState(m_pcmState);
        m_pcmState = nullptr;
    }
    resetState(true);
}

void AudioEngine::discardLoadedFileForDatabaseBoundary() {
    stop();

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        resetState(true);
        std::shared_ptr<const WaveformData> emptyWaveform = std::make_shared<WaveformData>();
        std::atomic_store(&m_waveformSource, emptyWaveform);
        m_pendingNormalizedSeek.store(-1.0f, std::memory_order_relaxed);
        m_seekPending.store(false, std::memory_order_relaxed);
        m_seekToken.fetch_add(1, std::memory_order_seq_cst);
        m_loadAsyncCurrentToken.fetch_add(1, std::memory_order_seq_cst);
        m_loadAsyncPlayWhenReady = false;
    }

    {
        std::lock_guard<std::mutex> reqLock(m_loadAsyncRequestMutex);
        m_loadAsyncRequestPending = false;
        m_loadAsyncRequestPath.clear();
    }
    m_loadAsyncRequestCv.notify_all();
    m_loadAsyncPending.store(false, std::memory_order_release);

    std::queue<LoadFileAsyncResult> staleResults;
    {
        std::lock_guard<std::mutex> resultLock(m_loadAsyncResultMutex);
        std::swap(staleResults, m_loadAsyncResultQueue);
    }
    while (!staleResults.empty()) {
        staleResults.pop();
    }
}

void AudioEngine::ensureLoadFileWorkerStarted() {
    m_loadAsyncExit.store(false);
    if (!m_loadAsyncWorker.joinable()) {
        m_loadAsyncWorker = std::thread(&AudioEngine::loadFileWorkerLoop, this);
    }
    if (!m_loadAsyncSpareWorker.joinable()) {
        m_loadAsyncSpareWorker = std::thread(&AudioEngine::loadFileWorkerLoop, this);
    }
}

void AudioEngine::stopLoadFileWorker() {
    m_loadAsyncExit.store(true);
    {
        std::lock_guard<std::mutex> lock(m_loadAsyncRequestMutex);
        m_loadAsyncRequestPending = false;
    }
    m_loadAsyncRequestCv.notify_all();
    if (m_loadAsyncWorker.joinable()) {
        m_loadAsyncWorker.join();
    }
    if (m_loadAsyncSpareWorker.joinable()) {
        m_loadAsyncSpareWorker.join();
    }
    // 清空结果队列并释放未应用的 decoder，避免泄漏
    std::lock_guard<std::mutex> lock(m_loadAsyncResultMutex);
    while (!m_loadAsyncResultQueue.empty()) {
        m_loadAsyncResultQueue.pop();
    }
}

void AudioEngine::loadFileWorkerLoop() {
    for (;;) {
        std::string path;
        uint64_t token = 0;
        uint32_t sampleRate = 48000;
        bool forceMono = false;
        int channelSolo = 0;
        std::chrono::steady_clock::time_point requestStartedAt{};
        {
            std::unique_lock<std::mutex> lock(m_loadAsyncRequestMutex);
            while (!m_loadAsyncExit.load() && !m_loadAsyncRequestPending)
                m_loadAsyncRequestCv.wait(lock);
            if (m_loadAsyncExit.load()) break;
            path = m_loadAsyncRequestPath;
            token = m_loadAsyncRequestToken;
            sampleRate = m_loadAsyncRequestSampleRate;
            forceMono = m_loadAsyncRequestForceMono;
            channelSolo = m_loadAsyncRequestChannelSolo;
            requestStartedAt = m_loadAsyncRequestStartedAt;
            m_loadAsyncRequestPending = false;
        }

        DIAG_SYSTEM_FMT("Audio",
            "PlaybackLoadDecodeBegin: token=%llu path=%s",
            (unsigned long long)token,
            path.c_str());

        LoadFileAsyncResult result;
        result.token = token;
        result.filePath = path;
        result.requestStartedAt = requestStartedAt;
        DiagScopedTimer loadDecodeTimer("Audio", "loadFileAsyncDecode");
        auto dec = std::make_unique<Platform::AudioDataSource::Decoder>();
        uint32_t outChannels = (forceMono || channelSolo != 0) ? 1 : 2;
        std::string lowerPath = path;
        std::transform(lowerPath.begin(), lowerPath.end(), lowerPath.begin(), ::tolower);
        const bool isOggPath = lowerPath.size() >= 4 &&
            lowerPath.compare(lowerPath.size() - 4, 4, ".ogg") == 0;

#ifdef _WIN32
        DiagnosticsLogger::instance().logPerformance("Audio", "loadFileAsyncFastWavHeader", 0.0);
        // 🔍 诊断：检查文件是否存在及大小
        if (IsAsyncLoadDiagEnabled()) {
            const auto fileProbe = Platform::FileSystem::Internal::ProbeFilePrefix(path);
            if (!fileProbe.opened) {
                char dbuf[512];
                snprintf(dbuf, sizeof(dbuf), "LOAD_DIAG: file NOT FOUND/OPEN err=%lu path=%s",
                         static_cast<unsigned long>(fileProbe.nativeOpenError), path.c_str());
                DIAG_ACTION("Audio", dbuf);
            } else {
                char dbuf[512];
                snprintf(dbuf, sizeof(dbuf),
                    "LOAD_DIAG: file OK size=%lld hdr=[%02X %02X %02X %02X %02X %02X %02X %02X] path=%s",
                    static_cast<long long>(fileProbe.sizeBytes),
                    fileProbe.prefix[0], fileProbe.prefix[1], fileProbe.prefix[2], fileProbe.prefix[3],
                    fileProbe.prefix[4], fileProbe.prefix[5], fileProbe.prefix[6], fileProbe.prefix[7],
                    path.c_str());
                DIAG_ACTION("Audio", dbuf);
            }
        }
        const auto initStartedAt = std::chrono::steady_clock::now();
        const auto decoderInit = Platform::AudioDataSource::InitializeDecoderFromFileWithFormatAndChannelMap(
            *dec, path, outChannels, sampleRate, channelSolo);
        int maRes = decoderInit.backendCode;
        DiagnosticsLogger::instance().logPerformance(
            "Audio",
            "loadFileAsyncInitDecoder",
            std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - initStartedAt).count());
#else
        const auto initStartedAt = std::chrono::steady_clock::now();
        const auto decoderInit = Platform::AudioDataSource::InitializeDecoderFromFileWithFormatAndChannelMap(
            *dec, path, outChannels, sampleRate, channelSolo);
        int maRes = decoderInit.backendCode;
        DiagnosticsLogger::instance().logPerformance(
            "Audio",
            "loadFileAsyncInitDecoder",
            std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - initStartedAt).count());
#endif
        if (maRes != MA_SUCCESS) {
            bool handledByMediaFoundation = false;
            if (HarkenSearch::MediaFoundationAudio::CanDecodeExtension(path)) {
                HarkenSearch::MediaFoundationAudio::DecodedAudioBuffer decoded;
                std::string mfError;
                if (HarkenSearch::MediaFoundationAudio::Decode(path, decoded, &mfError)) {
                    handledByMediaFoundation = true;
                    dec.reset();
                    result.success = true;
                    result.decodedPcmSource = true;
                    result.decodedPcmSampleRate = decoded.sampleRate;
                    result.decodedPcmChannels = decoded.channels;
                    result.decodedPcmTotalFrames = decoded.totalFrames;
                    result.decodedPcm = std::move(decoded.interleaved);
                    result.fileInfo.filePath = path;
#ifdef _WIN32
                    std::filesystem::path fsPath(Utf8ToWide(path));
                    std::wstring wfilename = fsPath.filename().wstring();
                    result.fileInfo.fileName = Platform::TextEncoding::WideToUtf8Permissive(wfilename);
#else
                    std::filesystem::path fsPath(path);
                    result.fileInfo.fileName = fsPath.filename().string();
#endif
                    result.fileInfo.sampleRate = decoded.sampleRate;
                    result.fileInfo.channels = decoded.channels;
                    result.fileInfo.totalFrames = decoded.totalFrames;
                    result.fileInfo.durationSeconds = decoded.durationSeconds;
                    result.fileInfo.format = HarkenSearch::MediaFoundationAudio::FormatNameFromPath(path);
                    result.decodeCompletedAt = std::chrono::steady_clock::now();
                    DIAG_SYSTEM_FMT("Audio",
                        "MediaFoundationLoadAsyncDecodedPCMReady: token=%llu format=%s durationSec=%.4f channels=%u path=%s",
                        (unsigned long long)token,
                        result.fileInfo.format.c_str(),
                        result.fileInfo.durationSeconds,
                        result.fileInfo.channels,
                        path.c_str());
                } else {
                    DIAG_SYSTEM_FMT("Audio",
                        "MediaFoundationLoadAsyncDecodedPCMFailed: token=%llu format=%s error=%s path=%s",
                        (unsigned long long)token,
                        HarkenSearch::MediaFoundationAudio::FormatNameFromPath(path),
                        HarkenSearch::MediaFoundationAudio::ClassifyFailure(mfError).c_str(),
                        path.c_str());
                }
            }
            if (!handledByMediaFoundation) {
            char diagBuf[512];
            snprintf(diagBuf, sizeof(diagBuf),
                "LOAD_FAIL: ma_result=%d ch=%u sr=%u path=%s",
                (int)maRes, (unsigned)outChannels, (unsigned)sampleRate, path.c_str());
            DIAG_ACTION("Audio", diagBuf);
            DIAG_SYSTEM_FMT("Audio",
                "PlaybackLoadDecodeFailed: token=%llu error=%d path=%s",
                (unsigned long long)token,
                (int)maRes,
                path.c_str());
            if (isOggPath) {
                DIAG_SYSTEM_FMT("Audio",
                    "OGGLoadDecodeFailed: token=%llu error=%d path=%s",
                    (unsigned long long)token,
                    (int)maRes,
                    path.c_str());
            }
            dec.reset();
            result.success = false;
            result.errorCode = (int)maRes;
            }
        } else {
            result.decoder = std::move(dec);
            result.success = true;
            result.fileInfo.filePath = path;
#ifdef _WIN32
            std::wstring wpath = Utf8ToWide(path);
            std::filesystem::path fsPath(wpath);
            std::wstring wfilename = fsPath.filename().wstring();
            std::wstring wext = fsPath.extension().wstring();
            result.fileInfo.fileName = Platform::TextEncoding::WideToUtf8Permissive(wfilename);
            std::string ext = Platform::TextEncoding::WideToUtf8Permissive(wext);
#else
            std::filesystem::path fsPath(path);
            result.fileInfo.fileName = fsPath.filename().string();
            std::string ext = fsPath.extension().string();
#endif
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
            result.fileInfo.sampleRate = sampleRate;
            uint64_t length = 0;
            {
                const auto format =
                    Platform::AudioDataSource::QueryDataFormat(
                        Platform::AudioDataSource::BorrowDecoder(*result.decoder));
                if (format.sampleRate > 0) {
                    result.fileInfo.sampleRate = format.sampleRate;
                }
                if (result.fileInfo.channels <= 0) {
                    result.fileInfo.channels = format.channels;
                }
            }
            const auto lengthProbeStartedAt = std::chrono::steady_clock::now();
            const auto lengthResult =
                Platform::AudioDataSource::QueryLength(
                    Platform::AudioDataSource::BorrowDecoder(*result.decoder));
            length = lengthResult.frames;
            DiagnosticsLogger::instance().logPerformance(
                "Audio",
                "loadFileAsyncLengthProbe",
                std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - lengthProbeStartedAt).count());
            if (!lengthResult.Succeeded() || length == 0) {
                DIAG_ACTION_FMT("Audio",
                    "AsyncLengthProbeFailed: path=%s ma_result=%d totalFrames=%llu",
                    path.c_str(), lengthResult.backendCode, (unsigned long long)length);
            }
            result.fileInfo.totalFrames = length;
            if (result.fileInfo.sampleRate > 0)
                result.fileInfo.durationSeconds = (float)length / (float)result.fileInfo.sampleRate;
            DIAG_ACTION_FMT("AudioAlign",
                "AsyncFileInfo: path=%s totalFrames=%llu dur=%.4fs sr=%u ch=%u",
                path.c_str(),
                (unsigned long long)result.fileInfo.totalFrames,
                result.fileInfo.durationSeconds,
                result.fileInfo.sampleRate,
                result.fileInfo.channels);
            if (ext == ".wav") result.fileInfo.format = "WAV";
            else if (ext == ".mp3") result.fileInfo.format = "MP3";
            else if (ext == ".flac") result.fileInfo.format = "FLAC";
            else if (ext == ".ogg") result.fileInfo.format = "OGG";
            else if (ext == ".m4a") result.fileInfo.format = "M4A";
            else if (ext == ".aac") result.fileInfo.format = "AAC";
            else if (ext == ".wma") result.fileInfo.format = "WMA";
            else result.fileInfo.format = "Unknown";
            result.decodeCompletedAt = std::chrono::steady_clock::now();
            DIAG_SYSTEM_FMT("Audio",
                "PlaybackLoadDecodeReady: token=%llu durationSec=%.4f channels=%u path=%s",
                (unsigned long long)token,
                result.fileInfo.durationSeconds,
                result.fileInfo.channels,
                path.c_str());
            if (isOggPath) {
                DIAG_SYSTEM_FMT("Audio",
                    "OGGLoadDecodeReady: token=%llu durationSec=%.4f channels=%u path=%s",
                    (unsigned long long)token,
                    result.fileInfo.durationSeconds,
                    result.fileInfo.channels,
                    path.c_str());
            }
        }

        {
            std::lock_guard<std::mutex> lock(m_loadAsyncResultMutex);
            m_loadAsyncResultQueue.push(std::move(result));
        }
    }
}

bool AudioEngine::applyLoadFileResult(
    std::unique_ptr<Platform::AudioDataSource::Decoder> decoder,
    const std::string& filePath,
    const AudioFileInfo& fileInfo) {
    std::unique_lock<std::mutex> lock(m_mutex);
    if (!m_isInitialized || !decoder) return false;

    if (m_sound) {
        const bool fadeEnabled = m_fadeEnabled.load();
        int fadeMs = m_seekFadeMs.load();
        if (fadeMs < 0) fadeMs = 0;
        if (fadeMs > 60) fadeMs = 60;
        auto oldSound = std::move(m_sound);
        auto oldDecoder = std::move(m_decoder);
        PcmAudioBufferState* oldPcm = m_pcmState;
        CenterSenseDataSource* oldCSDS = m_centerSenseDataSource;
        m_pcmState = nullptr;
        m_centerSenseDataSource = nullptr;
        if (fadeEnabled && fadeMs > 0 && Platform::AudioPlayback::IsPlaying(oldSound->Borrow())) {
            Platform::AudioPlayback::SetFadeInMilliseconds(oldSound->Borrow(), -1.0f, 0.0f, static_cast<std::uint64_t>(fadeMs));
            // Ensure the old sound stops after fade-out so it doesn't keep
            // overlapping the new sound ("beginning plays twice" artifact).
            {
                std::uint32_t sr = Platform::AudioPlayback::QueryEngineSampleRate(m_engine->Borrow());
                if (sr == 0) sr = 48000;
                std::uint64_t fadeSamples = (std::uint64_t)fadeMs * sr / 1000;
                std::uint64_t now = Platform::AudioPlayback::QueryEngineTimeInPcmFrames(m_engine->Borrow());
                Platform::AudioPlayback::SetStopTimeInPcmFrames(oldSound->Borrow(), now + fadeSamples);
            }
            DeferredCleanup dc;
            dc.sound = std::move(oldSound);
            dc.decoder = std::move(oldDecoder);
            dc.pcm = oldPcm;
            dc.csds = oldCSDS;
            dc.deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(fadeMs + 20);
            try {
                m_cleanupQueue.push_back(std::move(dc));
            } catch (...) {
                if (dc.sound) {
                    (void)Platform::AudioPlayback::Stop(dc.sound->Borrow());
                    dc.sound.reset();
                }
                if (oldCSDS) { oldCSDS->uninit(); delete oldCSDS; }
                dc.decoder.reset();
                DestroyPcmAudioBufferState(oldPcm);
            }
        } else {
            (void)Platform::AudioPlayback::Stop(oldSound->Borrow());
            oldSound.reset();
            if (oldCSDS) { oldCSDS->uninit(); delete oldCSDS; }
            oldDecoder.reset();
            DestroyPcmAudioBufferState(oldPcm);
        }
    } else if (m_decoder) {
        if (m_centerSenseDataSource) {
            m_centerSenseDataSource->uninit();
            delete m_centerSenseDataSource;
            m_centerSenseDataSource = nullptr;
        }
        m_decoder.reset();
    }
    if (m_pcmState) {
        DestroyPcmAudioBufferState(m_pcmState);
        m_pcmState = nullptr;
    }
    const std::string oldPathForPreserve = m_fileInfo.filePath;
    const float savedInApply = m_inPoint;
    const float savedOutApply = m_outPoint;
    constexpr float kRegionPreserveEpsilonApply = 0.001f;
    const bool preserveRegionApply = (fileInfo.filePath == oldPathForPreserve && (savedInApply > kRegionPreserveEpsilonApply || savedOutApply < (1.0f - kRegionPreserveEpsilonApply)));

    resetState(false);

    m_decoder = std::move(decoder);

    // 用解码器实际输出声道判断（播放 decoder 总是 1 或 2），而非文件原始声道
    const auto decoderFormat =
        Platform::AudioDataSource::QueryDataFormat(
            Platform::AudioDataSource::BorrowDecoder(*m_decoder));
    const bool useCenterSense = decoderFormat.channels == 1 || decoderFormat.channels == 2;

    if (useCenterSense) {
        m_centerSenseDataSource = new CenterSenseDataSource();
        if (!m_centerSenseDataSource->init(Platform::AudioDataSource::BorrowDataSource(*m_decoder), fileInfo.sampleRate > 0 ? (unsigned int)fileInfo.sampleRate : 48000u, &m_centerSense, &m_bassMonoFreq, &m_freqShiftHz, &m_pitchMode, &m_magicEQRealtime)) {
            delete m_centerSenseDataSource;
            m_centerSenseDataSource = nullptr;
            m_decoder.reset();
            return false;
        }
    }

    m_sound = std::make_unique<Platform::AudioPlayback::Sound>();
    const auto soundInitResult = m_sound->Initialize(
        m_engine->Borrow(),
        Platform::AudioPlayback::BorrowDataSource(
            useCenterSense ? m_centerSenseDataSource->getDataSource()
                           : m_decoder->NativeHandle()),
        Platform::AudioPlayback::SoundInitFlags::NoSpatialization);
    if (!soundInitResult.Succeeded()) {
        if (m_centerSenseDataSource) {
            m_centerSenseDataSource->uninit();
            delete m_centerSenseDataSource;
            m_centerSenseDataSource = nullptr;
        }
        m_decoder.reset();
        m_sound.reset();
        return false;
    }
    m_fileInfo = fileInfo;
    m_playbackChannels = decoderFormat.channels;
    enforceMagicEQAvailabilityLocked();
    m_decodedPlaybackTotalFrames =
        ResolveDecodedPlaybackTotalFramesFromSyncLoad(fileInfo.totalFrames, 0);
    if (preserveRegionApply) {
        m_inPoint = savedInApply;
        m_outPoint = savedOutApply;
    }
    // Defensive: clear any inherited/stale stop_time on the freshly created sound.
    // Without this, the engine's global clock may already exceed a leftover stop_time
    // from a previous sound, causing the new sound to stop immediately after start —
    // resulting in "audio plays but playhead stuck at 0" intermittently.
    Platform::AudioPlayback::SetStopTimeInPcmFrames(m_sound->Borrow(), static_cast<std::uint64_t>(-1));
    Platform::AudioPlayback::SetVolume(m_sound->Borrow(), std::max(0.0f, std::min(1.2f, m_volume * m_replayGain)));
    Platform::AudioPlayback::SetPan(
        m_sound->Borrow(),
        (m_forceMono || m_channelSolo != 0) ? 0.0f : m_pan);
    {
        float pitchMultiplier = (m_pitchMode == PitchMode::FreqShift) ? 1.0f : std::pow(2.0f, m_pitch / 12.0f);
        Platform::AudioPlayback::SetPitch(
            m_sound->Borrow(), pitchMultiplier);
    }
    // Clear any residual fade state so the fader is at passthrough (1.0).
    // Base volume is already set via set_volume above; fader should be neutral.
    Platform::AudioPlayback::SetFadeInMilliseconds(m_sound->Borrow(), 1.0f, 1.0f, 0);
    m_isLoaded = true;
    return true;
}

bool AudioEngine::applyDecodedPcmLoadFileResult(LoadFileAsyncResult& result) {
    std::unique_lock<std::mutex> lock(m_mutex);
    if (!m_isInitialized || !result.decodedPcmSource || result.decodedPcm.empty()) return false;

    if (m_sound) {
        const bool fadeEnabled = m_fadeEnabled.load();
        int fadeMs = m_seekFadeMs.load();
        if (fadeMs < 0) fadeMs = 0;
        if (fadeMs > 60) fadeMs = 60;
        auto oldSound = std::move(m_sound);
        auto oldDecoder = std::move(m_decoder);
        PcmAudioBufferState* oldPcm = m_pcmState;
        CenterSenseDataSource* oldCSDS = m_centerSenseDataSource;
        m_pcmState = nullptr;
        m_centerSenseDataSource = nullptr;
        if (fadeEnabled && fadeMs > 0 && Platform::AudioPlayback::IsPlaying(oldSound->Borrow())) {
            Platform::AudioPlayback::SetFadeInMilliseconds(oldSound->Borrow(), -1.0f, 0.0f, static_cast<std::uint64_t>(fadeMs));
            {
                std::uint32_t sr = Platform::AudioPlayback::QueryEngineSampleRate(m_engine->Borrow());
                if (sr == 0) sr = 48000;
                std::uint64_t fadeSamples = (std::uint64_t)fadeMs * sr / 1000;
                std::uint64_t now = Platform::AudioPlayback::QueryEngineTimeInPcmFrames(m_engine->Borrow());
                Platform::AudioPlayback::SetStopTimeInPcmFrames(oldSound->Borrow(), now + fadeSamples);
            }
            DeferredCleanup dc;
            dc.sound = std::move(oldSound);
            dc.decoder = std::move(oldDecoder);
            dc.pcm = oldPcm;
            dc.csds = oldCSDS;
            dc.deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(fadeMs + 20);
            try {
                m_cleanupQueue.push_back(std::move(dc));
            } catch (...) {
                if (dc.sound) {
                    (void)Platform::AudioPlayback::Stop(dc.sound->Borrow());
                    dc.sound.reset();
                }
                if (oldCSDS) { oldCSDS->uninit(); delete oldCSDS; }
                dc.decoder.reset();
                DestroyPcmAudioBufferState(oldPcm);
            }
        } else {
            (void)Platform::AudioPlayback::Stop(oldSound->Borrow());
            oldSound.reset();
            if (oldCSDS) { oldCSDS->uninit(); delete oldCSDS; }
            oldDecoder.reset();
            DestroyPcmAudioBufferState(oldPcm);
        }
    } else if (m_decoder) {
        if (m_centerSenseDataSource) {
            m_centerSenseDataSource->uninit();
            delete m_centerSenseDataSource;
            m_centerSenseDataSource = nullptr;
        }
        m_decoder.reset();
    }
    if (m_pcmState) {
        DestroyPcmAudioBufferState(m_pcmState);
        m_pcmState = nullptr;
    }

    const std::string oldPathForPreserve = m_fileInfo.filePath;
    const float savedInApply = m_inPoint;
    const float savedOutApply = m_outPoint;
    constexpr float kRegionPreserveEpsilonApply = 0.001f;
    const bool preserveRegionApply =
        (result.fileInfo.filePath == oldPathForPreserve &&
         (savedInApply > kRegionPreserveEpsilonApply ||
          savedOutApply < (1.0f - kRegionPreserveEpsilonApply)));

    resetState(false);

    result.fileInfo.sampleRate = result.decodedPcmSampleRate;
    result.fileInfo.channels = result.decodedPcmChannels;
    result.fileInfo.totalFrames = result.decodedPcmTotalFrames;
    if (result.fileInfo.sampleRate > 0 && result.fileInfo.totalFrames > 0) {
        result.fileInfo.durationSeconds =
            (float)((double)result.fileInfo.totalFrames / (double)result.fileInfo.sampleRate);
    }
    if (result.fileInfo.format.empty()) {
        result.fileInfo.format = HarkenSearch::MediaFoundationAudio::FormatNameFromPath(result.fileInfo.filePath);
    }

    if (!installDecodedPcmSourceLocked(result.fileInfo, std::move(result.decodedPcm))) {
        return false;
    }
    if (preserveRegionApply) {
        m_inPoint = savedInApply;
        m_outPoint = savedOutApply;
    }
    return true;
}

void AudioEngine::loadFileAsync(const std::string& filePath, bool playWhenReady) {
    if (filePath.empty()) return;

    MarkPlaybackStartWorkGuardActive();

    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_isInitialized) return;
    m_regionPlayback = false;
    m_regionLoopActive = false;
    m_regionFadeInNorm = 0.0f;
    m_regionFadeOutNorm = 0.0f;
    m_decodedPlaybackTotalFrames = 0;
    m_playIntent = false;
    m_loopSawPlaying = false;
    m_inPoint = 0.0f;
    m_outPoint = 1.0f;
    if (m_sound) {
        Platform::AudioPlayback::SetVolume(m_sound->Borrow(), std::max(0.0f, std::min(1.2f, m_volume * m_replayGain)));
        // Preserve stop_time set by stop(); clearing it would let the previous
        // sound overlap the newly loaded sound during its fade-out.
    }

    // Reset channel solo when switching files so stale routing cannot flash.
    if (filePath != m_fileInfo.filePath) {
        m_channelSolo = 0;
    }

    WaveformData blankWf;
    blankWf.filePath = filePath;
    blankWf.isReady = false; 
    blankWf.isFallbackPlaceholder = true; // Incomplete waveform, not a final decode failure.
    std::shared_ptr<const WaveformData> emptyWf = std::make_shared<WaveformData>(std::move(blankWf));
    
    std::atomic_store(&m_waveform, emptyWf);
    std::atomic_store(&m_waveformSource, emptyWf);

    m_loadAsyncCurrentToken++;
    m_loadAsyncPlayWhenReady = playWhenReady;
    // 清掉旧文件残留的预约跳转，避免快速切文件时把旧 seek 落到新文件
    m_pendingNormalizedSeek.store(-1.0f, std::memory_order_relaxed);
    m_deferredSeekNorm.store(-1.0f, std::memory_order_relaxed);

    // 关键优化：
    // 一旦开始排队新文件，立刻取消旧的 seek / 旧的主波形解码，
    // 避免快速切文件时后台还在为旧文件持续抢 CPU。
    m_seekPending.store(false, std::memory_order_relaxed);
    m_seekToken.fetch_add(1, std::memory_order_seq_cst);

    m_waveformRequestToken.fetch_add(1, std::memory_order_seq_cst);
    m_waveformRequestedPeaks.store(0, std::memory_order_relaxed);
    m_waveformGeneratedPeaks.store(0, std::memory_order_relaxed);

    uint64_t token = m_loadAsyncCurrentToken;
    uint32_t sampleRate = Platform::AudioPlayback::QueryEngineSampleRate(m_engine->Borrow());
    const auto requestStartedAt = std::chrono::steady_clock::now();

    {
        std::lock_guard<std::mutex> reqLock(m_loadAsyncRequestMutex);
        m_loadAsyncRequestPath = filePath;
        m_loadAsyncRequestToken = token;
        m_loadAsyncRequestSampleRate = sampleRate;
        m_loadAsyncRequestForceMono = m_forceMono;
        m_loadAsyncRequestChannelSolo = m_channelSolo;
        m_loadAsyncRequestStartedAt = requestStartedAt;
        m_loadAsyncRequestPending = true;
    }

    DIAG_SYSTEM_FMT("Audio",
        "PlaybackLoadRequested: token=%llu playWhenReady=%d guardMs=%lld path=%s",
        (unsigned long long)token,
        playWhenReady ? 1 : 0,
        (long long)kPlaybackStartWorkGuardMs,
        filePath.c_str());

    m_loadAsyncPending.store(true);
    m_loadAsyncRequestCv.notify_all();
    ensureLoadFileWorkerStarted();
}

bool AudioEngine::pumpLoadFileIfPending() {
    // Drain any old sounds whose fade-out has completed (replaces detached threads).
    pumpDeferredCleanup();

    LoadFileAsyncResult r;
    for (;;) {
        if (!m_loadAsyncPending.load()) {
            std::queue<LoadFileAsyncResult> staleResults;
            {
                std::lock_guard<std::mutex> lock(m_loadAsyncResultMutex);
                if (m_loadAsyncResultQueue.empty()) return false;
                std::swap(staleResults, m_loadAsyncResultQueue);
            }
            while (!staleResults.empty()) {
                staleResults.pop();
            }
            DIAG_SYSTEM("Audio", "DiscardStaleLoadResultsAfterBoundary");
            return false;
        }
        {
            std::lock_guard<std::mutex> lock(m_loadAsyncResultMutex);
            if (m_loadAsyncResultQueue.empty()) return false;
            r = std::move(m_loadAsyncResultQueue.front());
            m_loadAsyncResultQueue.pop();
        }
        uint64_t currentToken = m_loadAsyncCurrentToken.load();
        if (r.token == currentToken) break;
        r.decoder.reset();
        DIAG_SYSTEM_FMT("Audio", "DiscardStaleLoadResult: token=%llu current=%llu",
            (unsigned long long)r.token, (unsigned long long)currentToken);
    }
    m_loadAsyncPending.store(false);
    if (!r.success || (!r.decoder && !r.decodedPcmSource)) {
        {
            std::lock_guard<std::mutex> lock(m_lastLoadErrorMutex);
            char errBuf[256];
            if (r.errorCode == -7) {
                snprintf(errBuf, sizeof(errBuf), L(Ex_FileGoneOnDisk));
            } else {
                snprintf(errBuf, sizeof(errBuf), L(Ex_DecodeErrCodeFmt), r.errorCode);
            }
            m_lastLoadError = errBuf;
            m_lastLoadDiag = std::string("err=") + std::to_string(r.errorCode) + " path=" + r.filePath;
        }
        return false;
    }
    auto applyFinishedAt = std::chrono::steady_clock::now();
    bool ok = false;
    {
        DIAG_SCOPED_DETAIL("Audio", "loadFileAsyncApply");
        if (r.decodedPcmSource) {
            ok = applyDecodedPcmLoadFileResult(r);
        } else {
            ok = applyLoadFileResult(std::move(r.decoder), r.filePath, r.fileInfo);
        }
    }
    applyFinishedAt = std::chrono::steady_clock::now();
    if (r.requestStartedAt.time_since_epoch().count() > 0) {
        auto queueReadyAt = (r.decodeCompletedAt.time_since_epoch().count() > 0)
            ? r.decodeCompletedAt
            : applyFinishedAt;
        double queueReadyMs =
            std::chrono::duration<double, std::milli>(queueReadyAt - r.requestStartedAt).count();
        double totalMs =
            std::chrono::duration<double, std::milli>(applyFinishedAt - r.requestStartedAt).count();
        DiagnosticsLogger::instance().logPerformance("Audio", "loadFileAsyncQueueToReady", queueReadyMs);
        DiagnosticsLogger::instance().logPerformance("Audio", "loadFileAsyncTotal", totalMs);
        DIAG_SYSTEM_FMT("Audio", "SwitchRequestComplete: token=%llu ok=%d totalMs=%.2f",
            (unsigned long long)r.token, ok ? 1 : 0, totalMs);
        DIAG_SYSTEM_FMT("Audio",
            "PlaybackReady: token=%llu ok=%d queueReadyMs=%.2f totalMs=%.2f path=%s",
            (unsigned long long)r.token,
            ok ? 1 : 0,
            queueReadyMs,
            totalMs,
            r.filePath.c_str());
    }

    if (ok) {
        // 检查是否有加载期间预约的跳转请求
        float pendingSeek = m_pendingNormalizedSeek.exchange(-1.0f);
        if (pendingSeek >= 0.0f) {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_sound && m_fileInfo.totalFrames > 0) {
                const double norm = std::clamp((double)pendingSeek, 0.0, 1.0);
                const uint64_t maxFrame =
                    (m_fileInfo.totalFrames > 0) ? (m_fileInfo.totalFrames - 1) : 0;
                const uint64_t targetFrame =
                    (uint64_t)std::llround(norm * (double)maxFrame);

                Platform::AudioPlayback::SetStopTimeInPcmFrames(m_sound->Borrow(), static_cast<std::uint64_t>(-1));
                (void)Platform::AudioPlayback::SeekToFrame(m_sound->Borrow(), targetFrame);
            } else if (m_sound && m_fileInfo.totalFrames == 0) {
                // OGG/push-mode: totalFrames not yet known. Defer seek until the
                // waveform worker resolves it via extractNativeDetailToWaveform().
                m_deferredSeekNorm.store(pendingSeek, std::memory_order_relaxed);
                DIAG_ACTION_FMT("Audio",
                    "PendingSeekDeferredZeroFrames: path=%s norm=%.4f",
                    m_fileInfo.filePath.c_str(), pendingSeek);
            }
        }

        // The caller starts playback after applying any silence-skip seek, keeping
        // sound and waveform positions aligned.
    }
    return ok;
}

void AudioEngine::playIfLoadReady() {
    if (!m_loadAsyncPlayWhenReady) return;
    m_loadAsyncPlayWhenReady = false;
    play();
    if (m_userLoopEnabled && m_isLoaded) {
        const bool hasVisualRegion =
            m_inPoint < m_outPoint && (m_inPoint > 0.0f || m_outPoint < 1.0f);
        if (!hasVisualRegion) {
            m_inPoint = 0.0f;
            m_outPoint = 1.0f;
        }
        playRegion(true, m_regionFadeInNorm, m_regionFadeOutNorm);
    }
}

void AudioEngine::setUserLoopEnabled(bool enabled) {
    m_userLoopEnabled = enabled;
    if (!enabled) {
        // Safety net: switching the loop off must disarm any armed region loop
        // in the same tick even if a caller bypasses the intent workflow.
        m_regionLoopActive = false;
    }
}

void AudioEngine::stopLoopKeepCurrentPass() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_userLoopEnabled = false;
    if (!m_sound || !m_isLoaded) {
        m_regionLoopActive = false;
        return;
    }
    const bool boxedPlayback =
        m_regionPlayback && !(m_inPoint <= 0.0f && m_outPoint >= 1.0f);
    const LoopDisarmState disarm = DecideLoopDisarmKeepCurrentPass(boxedPlayback);
    m_regionPlayback = disarm.regionPlayback;
    m_regionLoopActive = disarm.regionLoopActive;
    if (boxedPlayback) {
        // Boxed selection keeps playing the remainder of the region; the pump
        // tail stop still applies. Looping was disarmed above in this tick.
        return;
    }
    m_regionFadeInNorm = 0.0f;
    m_regionFadeOutNorm = 0.0f;
    m_playIntent = false;
    Platform::AudioPlayback::SetVolume(
        m_sound->Borrow(), std::max(0.0f, std::min(1.2f, m_volume * m_replayGain)));
    Platform::AudioPlayback::SetStopTimeInPcmFrames(
        m_sound->Borrow(), static_cast<std::uint64_t>(-1));
}

std::string AudioEngine::getLastLoadError() {
    std::lock_guard<std::mutex> lock(m_lastLoadErrorMutex);
    std::string s = std::move(m_lastLoadError);
    m_lastLoadError.clear();
    return s;
}

std::string AudioEngine::peekLastLoadError() const {
    std::lock_guard<std::mutex> lock(m_lastLoadErrorMutex);
    return m_lastLoadError.empty() ? m_lastLoadDiag : m_lastLoadError;
}

std::string AudioEngine::peekLastLoadDiag() const {
    std::lock_guard<std::mutex> lock(m_lastLoadErrorMutex);
    return m_lastLoadDiag;
}

void AudioEngine::play() {
    DIAG_SCOPED_DETAIL("Audio", "play");
    MarkPlaybackStartWorkGuardActive();
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_sound && m_isLoaded) {
        // Capture playing state BEFORE any stop_time modifications.
        // pause() uses a scheduled stop (not the immediate stop API), so after it fires,
        // the backend playing-state query correctly returns false. But once we clear the stop_time
        // below (set to UINT64_MAX), miniaudio sees stopTime=future + startTime=past and
        // incorrectly reports the sound as playing — causing the fade-in guard below to
        // skip the fade, leaving the fader stuck at 0 → silent continued playback.
        const bool soundWasPlaying = Platform::AudioPlayback::IsPlaying(m_sound->Borrow());
        float effectiveVol = std::max(0.0f, std::min(1.2f, m_volume * m_replayGain));
        float targetStartVolume = effectiveVol;
        if (m_userLoopEnabled && !m_regionPlayback &&
            ShouldArmRegionLoopWithoutLength(m_inPoint, m_outPoint, true)) {
            // Unknown length (OGG push-mode) or plain full sound: the loop
            // request still arms region playback so the pump's exhaustion
            // restart can loop the very first pass.
            m_regionPlayback = true;
            m_regionLoopActive = true;
        } else if (m_userLoopEnabled && !m_regionPlayback && m_fileInfo.totalFrames > 0) {
            m_regionPlayback = true;
            m_regionLoopActive = true;
        }
        if (m_regionPlayback && m_fileInfo.totalFrames > 0) {
            uint64_t inFrame = (uint64_t)(m_inPoint * m_fileInfo.totalFrames);
            uint64_t outFrame = (uint64_t)(m_outPoint * m_fileInfo.totalFrames);
            if (outFrame > inFrame) {
                uint64_t cursor = 0;
                cursor = Platform::AudioPlayback::QueryCursor(m_sound->Borrow());
                if (cursor < inFrame || cursor >= outFrame) {
                    cursor = inFrame;
                    (void)Platform::AudioPlayback::SeekToFrame(m_sound->Borrow(), inFrame);
                }
                const double totalFramesD = (double)((std::max)(1ULL, (unsigned long long)m_fileInfo.totalFrames));
                const float cursorNorm = std::clamp((float)((double)cursor / totalFramesD), 0.0f, 1.0f);
                targetStartVolume = effectiveVol * ComputeRegionFadeGain(
                    cursorNorm, m_inPoint, m_outPoint, m_regionFadeInNorm, m_regionFadeOutNorm);

                if (m_regionLoopActive) {
                    Platform::AudioPlayback::SetStopTimeInPcmFrames(m_sound->Borrow(), static_cast<std::uint64_t>(-1));
                } else {
                    std::uint32_t engineSR = Platform::AudioPlayback::QueryEngineSampleRate(m_engine->Borrow());
                    if (engineSR == 0) engineSR = 48000;
                    uint32_t fileSR = m_fileInfo.sampleRate > 0 ? (uint32_t)m_fileInfo.sampleRate : engineSR;
                    uint64_t remainingFrames = (outFrame > cursor) ? (outFrame - cursor) : 0;
                    uint64_t remainingEngineFrames = remainingFrames * engineSR / fileSR;
                    std::uint64_t engineNow = Platform::AudioPlayback::QueryEngineTimeInPcmFrames(m_engine->Borrow());
                    Platform::AudioPlayback::SetStopTimeInPcmFrames(m_sound->Borrow(), engineNow + remainingEngineFrames);
                }
            } else {
                m_regionPlayback = false;
                m_regionLoopActive = false;
                m_regionFadeInNorm = 0.0f;
                m_regionFadeOutNorm = 0.0f;
                Platform::AudioPlayback::SetVolume(m_sound->Borrow(), std::max(0.0f, std::min(1.2f, m_volume * m_replayGain)));
                Platform::AudioPlayback::SetStopTimeInPcmFrames(m_sound->Borrow(), static_cast<std::uint64_t>(-1));
            }
        } else {
            // Clear any stop time limit set by playRegion() to allow full playback.
            Platform::AudioPlayback::SetVolume(m_sound->Borrow(), std::max(0.0f, std::min(1.2f, m_volume * m_replayGain)));
            Platform::AudioPlayback::SetStopTimeInPcmFrames(m_sound->Borrow(), static_cast<std::uint64_t>(-1));
        }
        const bool fadeEnabled = m_fadeEnabled.load();
        int fadeMs = m_seekFadeMs.load();
        if (fadeMs < 0) fadeMs = 0;
        if (fadeMs > 60) fadeMs = 60;
        if (fadeEnabled && fadeMs > 0 && !soundWasPlaying) {
            // Fade from silence only for a fresh start. Applying a fade to a
            // playing sound resets its gain and produces a double-start artifact.
            // Capture soundWasPlaying before clearing stop_time; the cleared value
            // can make a paused sound appear active.
            // Fade to unity because set_volume above supplies the target level.
            Platform::AudioPlayback::SetFadeInMilliseconds(m_sound->Borrow(), 0.0f, 1.0f, static_cast<std::uint64_t>(fadeMs));
        }
        m_playIntent = true;
        m_loopSawPlaying = m_regionLoopActive;
        (void)Platform::AudioPlayback::Start(
            m_sound->Borrow());
#if AURA_BUILD_DIAGNOSTICS
        {
            uint64_t cursor = 0;
            cursor = Platform::AudioPlayback::QueryCursor(m_sound->Borrow());
            bool playing = Platform::AudioPlayback::IsPlaying(m_sound->Borrow());
            DIAG_ACTION_FMT("Audio", "play(): started=%d cursor=%llu totalFrames=%llu region=%d fade=%d",
                (int)playing, (unsigned long long)cursor, (unsigned long long)m_fileInfo.totalFrames, (int)m_regionPlayback, fadeMs);
            DIAG_SYSTEM_FMT("Audio",
                "PlaybackStarted: started=%d cursor=%llu totalFrames=%llu guardMs=%lld path=%s",
                (int)playing,
                (unsigned long long)cursor,
                (unsigned long long)m_fileInfo.totalFrames,
                (long long)kPlaybackStartWorkGuardMs,
                m_fileInfo.filePath.c_str());
        }
#endif
    }
}

void AudioEngine::pause() {
    std::unique_lock<std::mutex> lock(m_mutex);
    if (!m_sound || !m_isLoaded) return;
    m_playIntent = false;

    const bool fadeEnabled = m_fadeEnabled.load();
    int fadeMs = m_seekFadeMs.load();
    if (fadeMs < 0) fadeMs = 0;
    if (fadeMs > 60) fadeMs = 60;

    if (fadeEnabled && fadeMs > 0 && Platform::AudioPlayback::IsPlaying(m_sound->Borrow())) {
        // stop_time uses the engine clock; the file cursor is a different time base.
        // A cursor-relative deadline may already be past and skip the fade.
        std::uint32_t sampleRate = Platform::AudioPlayback::QueryEngineSampleRate(m_engine->Borrow());
        if (sampleRate == 0) sampleRate = 44100;
        uint64_t fadePcm = (uint64_t)fadeMs * sampleRate / 1000;
        std::uint64_t engineNow = Platform::AudioPlayback::QueryEngineTimeInPcmFrames(m_engine->Borrow());
        Platform::AudioPlayback::SetFadeInPcmFrames(m_sound->Borrow(), -1.0f, 0.0f, fadePcm);
        Platform::AudioPlayback::SetStopTimeInPcmFrames(m_sound->Borrow(), engineNow + fadePcm);
    } else {
        (void)Platform::AudioPlayback::Stop(m_sound->Borrow());
    }
}

void AudioEngine::stop() {
    std::unique_lock<std::mutex> lock(m_mutex);
    if (!m_sound || !m_isLoaded) return;
    m_playIntent = false;
    m_regionPlayback = false;
    m_regionLoopActive = false;
    m_regionFadeInNorm = 0.0f;
    m_regionFadeOutNorm = 0.0f;

    const bool fadeEnabled = m_fadeEnabled.load();
    const int fadeMs = m_seekFadeMs.load();

    if (fadeEnabled && fadeMs > 0 && Platform::AudioPlayback::IsPlaying(m_sound->Borrow())) {
        // Fade to silence before stopping. Schedule stop_time on the engine clock
        // and leave the cursor untouched until the next play or load.
        std::uint32_t sampleRate = Platform::AudioPlayback::QueryEngineSampleRate(m_engine->Borrow());
        if (sampleRate == 0) sampleRate = 48000;
        uint64_t fadeSamples = (uint64_t)fadeMs * sampleRate / 1000;
        std::uint64_t engineNow = Platform::AudioPlayback::QueryEngineTimeInPcmFrames(m_engine->Borrow());
        Platform::AudioPlayback::SetFadeInMilliseconds(m_sound->Borrow(), -1.0f, 0.0f, static_cast<std::uint64_t>(fadeMs));
        Platform::AudioPlayback::SetStopTimeInPcmFrames(m_sound->Borrow(), engineNow + fadeSamples);
        return;  // seek(0) 由后续 play() 或 loadFile 处理
    }

    (void)Platform::AudioPlayback::Stop(m_sound->Borrow());
    (void)Platform::AudioPlayback::SeekToFrame(m_sound->Borrow(), 0);
    // 重置音量（清除可能残留的 pause fade 渐变）— fader 归 1.0，volume 走 set_volume
    Platform::AudioPlayback::SetFadeInMilliseconds(m_sound->Borrow(), 1.0f, 1.0f, 0);
    Platform::AudioPlayback::SetVolume(m_sound->Borrow(), std::max(0.0f, std::min(1.2f, m_volume * m_replayGain)));
}

void AudioEngine::setVolume(float volume) {
    float newVolume = std::clamp(volume, 0.0f, 1.2f);  // Allow up to 120% (overdrive/gain boost)
    
    // 节流：如果变化太小就不更新
    if (std::abs(newVolume - m_volume) < 0.001f) return;
    
    auto sound = Platform::AudioPlayback::BorrowSound(nullptr);
    bool regionPlayback = false;
    uint64_t totalFrames = 0;
    float inPoint = 0.0f;
    float outPoint = 1.0f;
    float regionFadeInNorm = 0.0f;
    float regionFadeOutNorm = 0.0f;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_volume = newVolume;
        sound = m_sound ? m_sound->Borrow() : Platform::AudioPlayback::BorrowSound(nullptr);
        regionPlayback = m_regionPlayback;
        totalFrames = m_fileInfo.totalFrames;
        inPoint = m_inPoint;
        outPoint = m_outPoint;
        regionFadeInNorm = m_regionFadeInNorm;
        regionFadeOutNorm = m_regionFadeOutNorm;
    }
    
    if (sound.IsValid()) {
        float effectiveBase = std::max(0.0f, std::min(1.2f, newVolume * m_replayGain));
        float appliedVolume = effectiveBase;
        if (regionPlayback && totalFrames > 0) {
            uint64_t cursor = 0;
            cursor = Platform::AudioPlayback::QueryCursor(sound);
            const double totalFramesD = (double)((std::max)(1ULL, (unsigned long long)totalFrames));
            const float cursorNorm = std::clamp((float)((double)cursor / totalFramesD), 0.0f, 1.0f);
            appliedVolume *= ComputeRegionFadeGain(cursorNorm, inPoint, outPoint, regionFadeInNorm, regionFadeOutNorm);
        }
        Platform::AudioPlayback::SetVolume(sound, appliedVolume);
    }
}

void AudioEngine::applyEffectiveVolume() {
    float effective = std::max(0.0f, std::min(1.2f, m_volume * m_replayGain));
    if (m_sound) Platform::AudioPlayback::SetVolume(m_sound->Borrow(), effective);
}

void AudioEngine::applyReplayGain(float gainDb, float /*referenceLevel*/) {
    if (gainDb == 0.0f) {
        m_replayGain = 1.0f;
    } else {
        // Convert dB gain to linear: linear = 10^(dB/20)
        float linear = std::pow(10.0f, gainDb / 20.0f);
        // Clamp to reasonable range [0.01, 4.0] (avoids clipping or silence)
        m_replayGain = std::max(0.01f, std::min(4.0f, linear));
    }
    // Reapply effective volume (pre-gain on top of user volume)
    applyEffectiveVolume();
}

void AudioEngine::setPan(float pan) {
    float newPan = std::clamp(pan, -1.0f, 1.0f);

    // 节流：如果变化太小就不更新
    if (std::abs(newPan - m_pan) < 0.001f) return;
    
    auto sound = Platform::AudioPlayback::BorrowSound(nullptr);
    float effectivePan = 0.0f;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_pan = newPan;
        sound = m_sound ? m_sound->Borrow() : Platform::AudioPlayback::BorrowSound(nullptr);
        // Apply pan - solo modes use mono output so pan is effectively centered
        effectivePan = (m_forceMono || m_channelSolo != 0) ? 0.0f : newPan;
    }
    
    if (sound.IsValid()) {
        Platform::AudioPlayback::SetPan(
            sound, effectivePan);
    }
}

void AudioEngine::setCenterSense(float k) {
    float newK = std::clamp(k, -1.0f, 1.0f);
    if (std::abs(newK - m_centerSense) < 0.001f) return;
    std::lock_guard<std::mutex> lock(m_mutex);
    m_centerSense = newK;
    // Playback chain will read m_centerSense in later tasks; no backend sound call here yet.
}

void AudioEngine::setMagicEQ(const MagicEQSettings& settings) {
    MagicEQSettings next = settings;
    next.invalidInput = settings.enabled && (
        (settings.mode != MagicEQMode::Pass && settings.mode != MagicEQMode::Notch) ||
        static_cast<int>(settings.steepness) < 0 ||
        static_cast<int>(settings.steepness) > 3 ||
        !std::isfinite(settings.centerFreq) || settings.centerFreq < 20.0f || settings.centerFreq > 20000.0f ||
        !std::isfinite(settings.halfWidthPercent) || settings.halfWidthPercent < 1.0f || settings.halfWidthPercent > 50.0f ||
        !std::isfinite(settings.widgetWidth) || settings.widgetWidth < 120.0f || settings.widgetWidth > 1600.0f);
    if (next.invalidInput) next.enabled = false;
    next.mode = next.mode == MagicEQMode::Notch ? MagicEQMode::Notch : MagicEQMode::Pass;
    if (!std::isfinite(next.centerFreq)) next.centerFreq = 1000.0f;
    if (!std::isfinite(next.halfWidthPercent)) next.halfWidthPercent = 10.0f;
    if (!std::isfinite(next.widgetWidth)) next.widgetWidth = CenterSenseDsp::kDefaultMagicEQWidgetWidth;
    next.halfWidthPercent = std::clamp(next.halfWidthPercent, 1.0f, 50.0f);
    next.centerFreq = std::clamp(next.centerFreq, 20.0f, 20000.0f);
    const float centerPercent = CenterSenseDsp::magicEQFrequencyToPercent(next.centerFreq);
    next.centerFreq = CenterSenseDsp::magicEQPercentToFrequency(
        std::clamp(centerPercent, next.halfWidthPercent, 100.0f - next.halfWidthPercent));
    next.widgetWidth = std::clamp(next.widgetWidth, 120.0f, 1600.0f);
    std::lock_guard<std::mutex> lock(m_mutex);
    const bool editingBypassedSettings =
        m_magicEQUnsupportedChannels && settings.invalidInput && !settings.enabled;
    if (!editingBypassedSettings) m_magicEQUserEnabled = next.enabled;
    m_magicEQ = next;
    m_magicEQUnsupportedChannels = false;
    enforceMagicEQAvailabilityLocked();
}

void AudioEngine::enforceMagicEQAvailabilityLocked() {
    if (m_magicEQUnsupportedChannels && m_playbackChannels <= 2) {
        m_magicEQ.enabled = m_magicEQUserEnabled;
        m_magicEQ.invalidInput = false;
        m_magicEQUnsupportedChannels = false;
    }
    if (m_magicEQUserEnabled && m_playbackChannels > 2) {
        m_magicEQ.enabled = false;
        m_magicEQ.invalidInput = true; // Reuse the existing visible bypass hint.
        m_magicEQUnsupportedChannels = true;
    }
    if (m_magicEQ.enabled && m_fileInfo.sampleRate > 0) {
        CenterSenseDsp::MagicEQProcessParams params{};
        params.enabled = true;
        params.mode = m_magicEQ.mode;
        params.steepness = m_magicEQ.steepness;
        params.centerFreq = m_magicEQ.centerFreq;
        params.halfWidthPercent = m_magicEQ.halfWidthPercent;
        if (!CenterSenseDsp::estimateMagicEQTail(params, static_cast<int>(m_fileInfo.sampleRate)).available) {
            m_magicEQ.enabled = false; // The published audio state and visible toggle agree.
            m_magicEQUserEnabled = false;
        }
    }
    m_magicEQRealtime.store(m_magicEQ);
}

MagicEQSettings AudioEngine::getMagicEQ() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_magicEQ;
}

MagicEQSettings AudioEngine::getMagicEQUserSettings() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    MagicEQSettings settings = m_magicEQ;
    if (m_magicEQUnsupportedChannels) {
        settings.enabled = m_magicEQUserEnabled;
        settings.invalidInput = false;
    }
    return settings;
}

AudioEngine::TransferProcessingSnapshot AudioEngine::captureTransferProcessingSnapshot() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    TransferProcessingSnapshot snapshot;
    snapshot.pitchSemitones = m_pitch;
    snapshot.volume = m_volume;
    snapshot.replayGain = m_replayGain;
    snapshot.pan = m_pan;
    snapshot.centerSense = m_centerSense;
    snapshot.bassMonoFreqHz = m_bassMonoFreq;
    snapshot.reverse = m_reverse;
    snapshot.mono = m_forceMono;
    snapshot.channelSolo = m_channelSolo;
    snapshot.inPoint = m_inPoint;
    snapshot.outPoint = m_outPoint;
    snapshot.totalFrames = m_fileInfo.totalFrames;
    snapshot.magicEQ = m_magicEQ;
    return snapshot;
}

void AudioEngine::setBassMonoFreq(float hz) {
    float newHz = (hz <= 0.0f) ? 0.0f : std::clamp(hz, 20.0f, 500.0f);
    if (std::abs(newHz - m_bassMonoFreq) < 0.01f) return;
    std::lock_guard<std::mutex> lock(m_mutex);
    m_bassMonoFreq = newHz;
}

void AudioEngine::setChannelSolo(int mode) {
    std::string path;
    bool shouldReload = false;
    bool wasPlaying = false;
    float pos = 0.0f;
    float inPt = 0.0f;
    float outPt = 1.0f;
    std::shared_ptr<const WaveformData> sourceSnapshot;

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const bool loadedWithFile = m_isLoaded && !m_fileInfo.filePath.empty();
        const int channelLimit = (loadedWithFile && m_fileInfo.channels > 1)
            ? std::min((int)m_fileInfo.channels, kMaxAuditionChannelSolo)
            : 0;
        const int newMode = (mode <= 0 || channelLimit <= 0)
            ? 0
            : std::clamp(mode, 1, channelLimit);
        if (m_channelSolo == newMode) return;

        m_channelSolo = newMode;

        if (loadedWithFile) {
            path = m_fileInfo.filePath;
            shouldReload = true;
            wasPlaying = (m_sound != nullptr) ? Platform::AudioPlayback::IsPlaying(m_sound->Borrow()) : false;
            if (m_sound && m_fileInfo.totalFrames > 0) {
                uint64_t cursor = 0;
                cursor = Platform::AudioPlayback::QueryCursor(m_sound->Borrow());
                pos = (float)cursor / (float)m_fileInfo.totalFrames;
            }
            inPt = m_inPoint;
            outPt = m_outPoint;
            sourceSnapshot = std::atomic_load(&m_waveformSource);
        } else {
            return;
        }
    }

    if (shouldReload && !path.empty()) {
        loadFile(path);
        setInPoint(inPt);
        setOutPoint(outPt);
        if (pos > 0.0f) {
            seekToPosition(pos);
        }

        // Reuse source detail for an immediate view change.
        if (!tryApplySourceWaveformForCurrentView(sourceSnapshot, path)) {
            // If source detail is unavailable, regenerate at the current resolution
            // instead of falling back to the default detail size.
            int req = m_waveformRequestedPeaks.load();
            forceRegenerateWaveform(req > 0 ? req : AppState::WAVEFORM_DEFAULT_DETAIL_PEAKS);
        }

        if (wasPlaying) {
            play();
        }
    }
}

void AudioEngine::setMono(bool mono) {
    std::string path;
    bool shouldReload = false;
    bool wasPlaying = false;
    float pos = 0.0f;
    float inPt = 0.0f;
    float outPt = 1.0f;
    std::shared_ptr<const WaveformData> sourceSnapshot;

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_forceMono == mono) return;

        if (mono) {
            m_panStereo = m_pan;
            m_pan = 0.0f;
        } else {
            m_pan = m_panStereo;
        }

        m_forceMono = mono;
        if (m_isLoaded && !m_fileInfo.filePath.empty()) {
            path = m_fileInfo.filePath;
            shouldReload = true;
            wasPlaying = (m_sound != nullptr) ? Platform::AudioPlayback::IsPlaying(m_sound->Borrow()) : false;
            if (m_sound && m_fileInfo.totalFrames > 0) {
                uint64_t cursor = 0;
                cursor = Platform::AudioPlayback::QueryCursor(m_sound->Borrow());
                pos = (float)cursor / (float)m_fileInfo.totalFrames;
            } else {
                pos = 0.0f;
            }
            inPt = m_inPoint;
            outPt = m_outPoint;
            sourceSnapshot = std::atomic_load(&m_waveformSource);
        }
    }

    if (shouldReload) {
        loadFile(path);
        setInPoint(inPt);
        setOutPoint(outPt);
        seekToPosition(pos);

        // 优先直接复用 source detail 即时切显示
        if (!tryApplySourceWaveformForCurrentView(sourceSnapshot, path)) {
            forceRegenerateWaveform(AppState::WAVEFORM_DEFAULT_DETAIL_PEAKS);
        }

        if (wasPlaying) play();
    }
}

void AudioEngine::setPitch(float semitones) {
    // Allow up to ±48 semitones (4 octaves) for maximum flexibility
    float newPitch = std::clamp(semitones, -48.0f, 48.0f);
    
    // 节流：如果变化太小就不更新，避免频繁调用
    if (std::abs(newPitch - m_pitch) < 0.01f) return;
    
    auto sound = Platform::AudioPlayback::BorrowSound(nullptr);
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_pitch = newPitch;
        sound = m_sound ? m_sound->Borrow() : Platform::AudioPlayback::BorrowSound(nullptr);
    }
    
    // 在锁外调用 miniaudio（它是线程安全的）
    // FreqShift 模式下 miniaudio pitch 固定为 1.0（由 CenterSenseDataSource DSP 处理）
    if (sound.IsValid() && m_pitchMode == PitchMode::Semitone) {
        float pitchMultiplier = std::pow(2.0f, newPitch / 12.0f);
        Platform::AudioPlayback::SetPitch(
            sound, pitchMultiplier);
    }
}

void AudioEngine::setFreqShift(float hz) {
    // Clamp to the shared UI/engine range; below 0.1 Hz is treated as off by the DSP path.
    m_freqShiftHz = std::clamp(hz, -kFreqShiftRangeHz, kFreqShiftRangeHz);
    // No miniaudio call needed — CenterSenseDataSource reads m_freqShiftHz directly via pointer
}

void AudioEngine::setPitchMode(PitchMode mode) {
    auto sound = Platform::AudioPlayback::BorrowSound(nullptr);
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_pitchMode = mode;
        sound = m_sound ? m_sound->Borrow() : Platform::AudioPlayback::BorrowSound(nullptr);
    }
    if (sound.IsValid()) {
        if (mode == PitchMode::FreqShift) {
            // Disable miniaudio resampling; freq shift handled by CenterSenseDataSource DSP
            Platform::AudioPlayback::SetPitch(
                sound, 1.0f);
        } else {
            // Restore semitone-based pitch ratio
            float pitchMultiplier = std::pow(2.0f, m_pitch / 12.0f);
            Platform::AudioPlayback::SetPitch(
                sound, pitchMultiplier);
        }
    }
}

bool AudioEngine::setReverse(bool reverse) {
    bool needReload = false;
    float pos = 0.0f;
    float originalPos = 0.0f;
    bool wasPlaying = false;

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (reverse == m_reverse) return (m_sound != nullptr);

        m_reverse = reverse;

        if (!m_isLoaded || m_fileInfo.filePath.empty()) {
            if (reverse) m_reverse = false;   // A reverse request needs a loaded file.
            return false;
        }

        wasPlaying = (m_sound != nullptr) ? Platform::AudioPlayback::IsPlaying(m_sound->Borrow()) : false;

        if (m_sound && m_fileInfo.totalFrames > 0) {
            uint64_t cursor = 0;
            cursor = Platform::AudioPlayback::QueryCursor(m_sound->Borrow());
            originalPos = (float)cursor / (float)m_fileInfo.totalFrames;
            pos = 1.0f - originalPos;
        }
        needReload = true;
    }

    bool reloaded = false;
    if (needReload) {
        reloaded = reloadWithReverseState();
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_sound && m_fileInfo.totalFrames > 0) {
            const float target = reloaded ? pos : originalPos;
            uint64_t targetFrame = (uint64_t)(target * m_fileInfo.totalFrames);
            (void)Platform::AudioPlayback::SeekToFrame(m_sound->Borrow(), targetFrame);
            if (wasPlaying) {
                (void)Platform::AudioPlayback::Start(
                    m_sound->Borrow());
            }
        }
    }

    // A failed reload can leave no sound; clear the reverse flag in that case.
    // Report success only when a sound exists in the requested direction.
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_reverse && !m_sound) m_reverse = false;
        return (m_sound != nullptr) && (m_reverse == reverse);
    }
}

bool AudioEngine::loadReversedAudio() {
    if (m_fileInfo.filePath.empty()) return false;
    // Safety lock: block reverse full-buffer loading for very long files to avoid OOM.
    if (m_fileInfo.durationSeconds > 300.0f) {
        DIAG_ACTION_FMT("Audio", "Reverse blocked for long file (>300s): %s", m_fileInfo.filePath.c_str());
        return false;
    }
    
    Platform::AudioDataSource::Decoder decoder;
    std::uint32_t engineSR = Platform::AudioPlayback::QueryEngineSampleRate(m_engine->Borrow());
    std::uint32_t channels = m_forceMono ? 1 : 2;
    const auto initResult = Platform::AudioDataSource::InitializeDecoderFromFileWithFormat(
        decoder, m_fileInfo.filePath, channels, engineSR);
    if (!initResult.Succeeded()) return false;
    
    // OGG/Vorbis 等 push 模式解码后端无法预先报告总长度：QueryLength 恒为 0，
    // 但可顺序读出 PCM。因此反向不能依赖总长度预分配（旧实现在 totalFrames==0 时
    // 直接 return false，导致 OGG 点反转无声音）。与 DecodeFileToInterleavedF32 一致：
    // 顺序读到 framesRead==0 耗尽再反转。totalFrames 仅用于 reserve 预分配（>0 时）。
    // 防 OOM：解码帧超过 300s（与上方 durationSeconds 守卫同口径）即放弃。
    uint64_t totalFrames =
        Platform::AudioDataSource::QueryLength(
            Platform::AudioDataSource::BorrowDataSource(decoder)).frames;

    static constexpr uint64_t kReverseChunkFrames = 8192;
    const uint64_t maxReverseFrames = 300ULL * (engineSR > 0 ? engineSR : 48000ULL);
    if (totalFrames > 0 && totalFrames < maxReverseFrames) {
        const uint64_t reserveFrames =
            std::min<uint64_t>(totalFrames, kReverseChunkFrames * 256ULL);
        m_reverseBuffer.reserve((size_t)reserveFrames * (size_t)channels);
    }
    std::vector<float> chunk((size_t)kReverseChunkFrames * (size_t)channels);
    uint64_t decodedFrames = 0;
    bool decodeErrored = false;
    while (true) {
        const auto readResult = Platform::AudioDataSource::ReadPcmFrames(
            Platform::AudioDataSource::BorrowDataSource(decoder), chunk.data(), kReverseChunkFrames);
        const uint64_t framesRead = readResult.framesRead;
        if (framesRead > 0) {
            const size_t samplesRead = (size_t)framesRead * (size_t)channels;
            m_reverseBuffer.insert(m_reverseBuffer.end(), chunk.begin(), chunk.begin() + samplesRead);
            decodedFrames += framesRead;
            if (decodedFrames > maxReverseFrames) {
                Platform::AudioDataSource::UninitializeDecoder(decoder);
                m_reverseBuffer.clear();
                DIAG_ACTION_FMT("Audio", "Reverse blocked: decoded frames exceeded 300s cap: %s", m_fileInfo.filePath.c_str());
                return false;
            }
            continue;
        }
        if (readResult.backendCode == MA_AT_END || readResult.backendCode == MA_SUCCESS) break;
        decodeErrored = true;   // 解码中断/损坏：不能当 EOF 截断成功
        break;
    }
    Platform::AudioDataSource::UninitializeDecoder(decoder);
    if (decodeErrored) {
        m_reverseBuffer.clear();
        DIAG_ACTION_FMT("Audio", "Reverse aborted: decode error before EOF: %s", m_fileInfo.filePath.c_str());
        return false;
    }
    if (decodedFrames == 0) { m_reverseBuffer.clear(); return false; }

    size_t totalSamples = (size_t)decodedFrames * channels;
    m_reverseBuffer.resize(totalSamples);

    size_t frameCount = totalSamples / channels;
    for (size_t i = 0; i < frameCount / 2; i++) {
        size_t j = frameCount - 1 - i;
        for (uint32_t ch = 0; ch < channels; ch++) {
            std::swap(m_reverseBuffer[i * channels + ch], m_reverseBuffer[j * channels + ch]);
        }
    }
    
    m_reverseBufferValid = true;
    return true;
}

bool AudioEngine::reloadWithReverseState() {
    std::unique_lock<std::mutex> lock(m_mutex);
    
    if (!m_isInitialized || m_fileInfo.filePath.empty()) return false;
    
    std::string path = m_fileInfo.filePath;
    
    if (m_sound) {
        (void)Platform::AudioPlayback::Stop(m_sound->Borrow());
        m_sound.reset();
    }
    if (m_centerSenseDataSource) {
        m_centerSenseDataSource->uninit();
        delete m_centerSenseDataSource;
        m_centerSenseDataSource = nullptr;
    }
    if (m_decoder) {
        m_decoder.reset();
    }
    if (m_pcmState) {
        DestroyPcmAudioBufferState(m_pcmState);
        m_pcmState = nullptr;
    }
    
    auto installForwardSound = [&]() -> bool {
        m_reverseBuffer.clear();
        m_reverseBufferValid = false;

        m_decoder = std::make_unique<Platform::AudioDataSource::Decoder>();
        std::uint32_t engineSR = Platform::AudioPlayback::QueryEngineSampleRate(m_engine->Borrow());
        const auto initResult = Platform::AudioDataSource::InitializeDecoderFromFileWithFormat(
            *m_decoder, path, m_forceMono ? 1u : 2u, engineSR);
        if (!initResult.Succeeded()) {
            m_decoder.reset();
            return false;
        }

        const auto format = Platform::AudioDataSource::QueryDataFormat(
            Platform::AudioDataSource::BorrowDecoder(*m_decoder));
        m_centerSenseDataSource = new CenterSenseDataSource();
        if (!m_centerSenseDataSource->init(
                Platform::AudioDataSource::BorrowDataSource(*m_decoder), engineSR,
                &m_centerSense, &m_bassMonoFreq, &m_freqShiftHz, &m_pitchMode,
                &m_magicEQRealtime)) {
            delete m_centerSenseDataSource;
            m_centerSenseDataSource = nullptr;
            m_decoder.reset();
            return false;
        }

        m_sound = std::make_unique<Platform::AudioPlayback::Sound>();
        const auto soundInitResult = m_sound->Initialize(
            m_engine->Borrow(),
            Platform::AudioPlayback::BorrowDataSource(m_centerSenseDataSource->getDataSource()),
            Platform::AudioPlayback::SoundInitFlags::NoSpatialization);

        if (!soundInitResult.Succeeded()) {
            m_sound.reset();
            m_centerSenseDataSource->uninit();
            delete m_centerSenseDataSource;
            m_centerSenseDataSource = nullptr;
            m_decoder.reset();
            return false;
        }

        uint64_t length =
            Platform::AudioDataSource::QueryLength(
                Platform::AudioDataSource::BorrowDecoder(*m_decoder)).frames;
        m_fileInfo.totalFrames = length;

        m_fileInfo.channels = format.channels;

        m_fileInfo.sampleRate = engineSR;
        m_fileInfo.durationSeconds = (float)length / (float)engineSR;
        m_playbackChannels = format.channels;
        enforceMagicEQAvailabilityLocked();
        return true;
    };

    bool restoreForwardAfterReverseFailure = false;
    if (m_reverse) {
        m_reverseBufferValid = false;

        lock.unlock();
        bool loaded = loadReversedAudio();
        lock.lock();

        if (!loaded || m_reverseBuffer.empty()) {
            m_reverse = false;
            m_reverseBuffer.clear();
            m_reverseBufferValid = false;
            restoreForwardAfterReverseFailure = true;
        }
        if (m_reverse) {
            std::uint32_t channels = m_forceMono ? 1 : 2;
            std::uint32_t sampleRate = Platform::AudioPlayback::QueryEngineSampleRate(m_engine->Borrow());
            auto* audioBuffer = new PcmAudioBufferState();
            audioBuffer->samples = std::move(m_reverseBuffer);
            uint64_t frameCount = audioBuffer->samples.size() / channels;
            const auto bufferInitResult = Platform::AudioDataSource::InitializePcmBuffer(
                audioBuffer->buffer,
                channels,
                frameCount,
                audioBuffer->samples.data());
            if (!bufferInitResult.Succeeded()) {
                delete audioBuffer;
                m_reverse = false;
                m_reverseBuffer.clear();
                m_reverseBufferValid = false;
                restoreForwardAfterReverseFailure = true;
            }
            if (m_reverse) {
                m_centerSenseDataSource = new CenterSenseDataSource();
                if (!m_centerSenseDataSource->init(
                        Platform::AudioDataSource::BorrowDataSource(audioBuffer->buffer), sampleRate,
                        &m_centerSense, &m_bassMonoFreq, &m_freqShiftHz, &m_pitchMode,
                        &m_magicEQRealtime)) {
                    delete m_centerSenseDataSource;
                    m_centerSenseDataSource = nullptr;
                    Platform::AudioDataSource::UninitializePcmBuffer(audioBuffer->buffer);
                    delete audioBuffer;
                    m_reverse = false;
                    restoreForwardAfterReverseFailure = true;
                }
            }
            if (m_reverse) {
                m_sound = std::make_unique<Platform::AudioPlayback::Sound>();
                const auto soundInitResult = m_sound->Initialize(
                    m_engine->Borrow(),
                    Platform::AudioPlayback::BorrowDataSource(m_centerSenseDataSource->getDataSource()),
                    Platform::AudioPlayback::SoundInitFlags::NoSpatialization |
                        Platform::AudioPlayback::SoundInitFlags::NoPitch);

                if (!soundInitResult.Succeeded()) {
                    m_sound.reset();
                    m_centerSenseDataSource->uninit();
                    delete m_centerSenseDataSource;
                    m_centerSenseDataSource = nullptr;
                    Platform::AudioDataSource::UninitializePcmBuffer(audioBuffer->buffer);
                    delete audioBuffer;
                    m_reverse = false;
                    m_reverseBuffer.clear();
                    m_reverseBufferValid = false;
                    restoreForwardAfterReverseFailure = true;
                }
                if (m_reverse) {
                    audioBuffer->initialized = true;
                    m_pcmState = audioBuffer;

                    m_fileInfo.totalFrames = frameCount;
                    m_fileInfo.channels = channels;
                    m_fileInfo.sampleRate = sampleRate;
                    m_fileInfo.durationSeconds = (float)frameCount / (float)sampleRate;
                    m_playbackChannels = channels;
                    enforceMagicEQAvailabilityLocked();
                }
            }
        }
    } else {
        if (!installForwardSound()) {
            return false;
        }
    }

    if (restoreForwardAfterReverseFailure) {
        if (!installForwardSound()) {
            return false;
        }
    }
    
    Platform::AudioPlayback::SetVolume(m_sound->Borrow(), std::max(0.0f, std::min(1.2f, m_volume * m_replayGain)));
    // Solo 或 Mono 模式输出单声道，pan 强制居中
    Platform::AudioPlayback::SetPan(
        m_sound->Borrow(),
        (m_forceMono || m_channelSolo != 0) ? 0.0f : m_pan);
    {
        float pitchMultiplier = (m_pitchMode == PitchMode::FreqShift) ? 1.0f : std::pow(2.0f, m_pitch / 12.0f);
        Platform::AudioPlayback::SetPitch(
            m_sound->Borrow(), pitchMultiplier);
    }

    return !restoreForwardAfterReverseFailure;
}

void AudioEngine::setInPoint(float normalizedPos) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_inPoint = std::clamp(normalizedPos, 0.0f, m_outPoint - 0.01f);
}

void AudioEngine::setOutPoint(float normalizedPos) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_outPoint = std::clamp(normalizedPos, m_inPoint + 0.01f, 1.0f);
}

void AudioEngine::setRegionFadeNorms(float fadeInNorm, float fadeOutNorm) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_regionFadeInNorm  = std::clamp(fadeInNorm,  0.0f, 1.0f);
    m_regionFadeOutNorm = std::clamp(fadeOutNorm, 0.0f, 1.0f);
}

void AudioEngine::restoreRegionPlayback(float fadeInNorm, float fadeOutNorm) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_regionPlayback    = true;
    m_regionFadeInNorm  = std::clamp(fadeInNorm,  0.0f, 1.0f);
    m_regionFadeOutNorm = std::clamp(fadeOutNorm, 0.0f, 1.0f);
}

void AudioEngine::clearInOutPoints() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_inPoint = 0.0f;
    m_outPoint = 1.0f;
    m_regionFadeInNorm = 0.0f;
    m_regionFadeOutNorm = 0.0f;
    if (m_userLoopEnabled) {
        m_regionPlayback = true;
        m_regionLoopActive = true;
    } else {
        m_regionPlayback = false;
        m_regionLoopActive = false;
    }
    if (m_sound) {
        Platform::AudioPlayback::SetVolume(m_sound->Borrow(), std::max(0.0f, std::min(1.2f, m_volume * m_replayGain)));
        Platform::AudioPlayback::SetStopTimeInPcmFrames(m_sound->Borrow(), static_cast<std::uint64_t>(-1));
    }
}

// Soft-clip: tanh-based limiter to avoid hard clipping artifacts (clicks) when
// volume/DSP pushes samples beyond [-1, 1]. Only applied in export path.
static void ApplyExportSoftClip(float* samples, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        float x = samples[i];
        if (x > 1.0f || x < -1.0f) {
            samples[i] = std::tanh(x);
        }
    }
}

// Raised-cosine fades at region boundaries; separate in/out lengths (file-rate frames).
// Used when UI supplies region fade norms; symmetric 10ms path uses Utils::ApplyExportDeclickFade instead.
static void ApplyExportRegionFadeAsymmetric(float* samples, size_t numFrames, size_t channels,
                                            size_t globalStartFrame, size_t totalFrames,
                                            size_t fadeInLen, size_t fadeOutLen) {
    if (!samples || channels == 0 || totalFrames == 0) return;
    if (fadeInLen == 0 && fadeOutLen == 0) return;
    constexpr float kPi = 3.14159265358979323846f;
    for (size_t f = 0; f < numFrames; ++f) {
        size_t global = globalStartFrame + f;
        float gain = 1.0f;
        if (fadeInLen > 0 && global < fadeInLen) {
            float t = (float)global / (float)fadeInLen;
            gain = 0.5f * (1.0f - cosf(kPi * t));
        } else if (fadeOutLen > 0 && global >= totalFrames - fadeOutLen) {
            float t = (float)(totalFrames - 1 - global) / (float)fadeOutLen;
            gain = 0.5f * (1.0f - cosf(kPi * t));
        }
        for (size_t c = 0; c < channels; ++c)
            samples[f * channels + c] *= gain;
    }
}

static uint64_t ResolveExportFrameCount(uint64_t decoderFrames,
                                        uint64_t displayFrames) {
    // Export should follow the same length truth the user saw on screen. This
    // avoids false "invalid region" failures when a decoder reports 0 frames,
    // and also avoids overshooting assets whose raw metadata overstates length.
    if (displayFrames == 0) return decoderFrames;
    if (decoderFrames == 0) return displayFrames;
    return (std::min)(decoderFrames, displayFrames);
}

void AudioEngine::playRegion(bool loopEnabled, float regionFadeInNorm, float regionFadeOutNorm) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_sound && m_isLoaded && m_fileInfo.totalFrames == 0 &&
        ShouldArmRegionLoopWithoutLength(m_inPoint, m_outPoint, loopEnabled)) {
        // OGG/push-mode first pass: the real length is still unknown (0), but
        // the user asked to loop the whole sound. Arm region playback anyway
        // and start; the pump's length-agnostic exhaustion restart finishes
        // the first loop. No frame math is possible or needed here.
        m_regionPlayback = true;
        m_regionLoopActive = true;
        m_regionFadeInNorm = std::clamp(regionFadeInNorm, 0.0f, 1.0f);
        m_regionFadeOutNorm = std::clamp(regionFadeOutNorm, 0.0f, 1.0f);
        Platform::AudioPlayback::ResetStopTimeAndFade(m_sound->Borrow());
        Platform::AudioPlayback::SetStopTimeInPcmFrames(m_sound->Borrow(), static_cast<std::uint64_t>(-1));
        Platform::AudioPlayback::SetVolume(m_sound->Borrow(), std::max(0.0f, std::min(1.2f, m_volume * m_replayGain)));
        m_playIntent = true;
        m_loopSawPlaying = true;
        (void)Platform::AudioPlayback::Start(m_sound->Borrow());
        return;
    }
    if (m_sound && m_isLoaded && m_fileInfo.totalFrames > 0) {
        // Capture this before changing stop_time. pause() fades the sound to 0
        // and then stops it via stop_time; when the user pauses and immediately
        // box-selects a main-waveform region, playRegion() must explicitly
        // re-arm the fade multiplier or the new region can start silently.
        const bool soundWasPlaying = Platform::AudioPlayback::IsPlaying(m_sound->Borrow());
        uint64_t inFrame = (uint64_t)(m_inPoint * m_fileInfo.totalFrames);
        uint64_t outFrame = (uint64_t)(m_outPoint * m_fileInfo.totalFrames);
        Platform::AudioPlayback::ResetStopTimeAndFade(m_sound->Borrow());
        if (outFrame <= inFrame) {
            // Degenerate region (zero-length after integer truncation) — fall back to normal play
            // from inPoint so the user hears something instead of silence.
            m_regionPlayback = false;
            m_regionLoopActive = false;
            m_regionFadeInNorm = 0.0f;
            m_regionFadeOutNorm = 0.0f;
            Platform::AudioPlayback::SetStopTimeInPcmFrames(m_sound->Borrow(), static_cast<std::uint64_t>(-1));
            Platform::AudioPlayback::SetVolume(m_sound->Borrow(), std::max(0.0f, std::min(1.2f, m_volume * m_replayGain)));
            (void)Platform::AudioPlayback::SeekToFrame(m_sound->Borrow(), inFrame);
            const bool fadeEnabled = m_fadeEnabled.load();
            int fadeMs = m_seekFadeMs.load();
            if (fadeMs < 0) fadeMs = 0;
            if (fadeMs > 60) fadeMs = 60;
            if (fadeEnabled && fadeMs > 0 && !soundWasPlaying) {
                Platform::AudioPlayback::SetFadeInMilliseconds(m_sound->Borrow(), 0.0f, 1.0f, static_cast<std::uint64_t>(fadeMs));
            }
            m_playIntent = true;
            m_loopSawPlaying = m_regionLoopActive;
            (void)Platform::AudioPlayback::Start(
                m_sound->Borrow());
            return;
        }

        m_regionPlayback = true;
        m_regionLoopActive = loopEnabled;
        m_regionFadeInNorm = std::clamp(regionFadeInNorm, 0.0f, 1.0f);
        m_regionFadeOutNorm = std::clamp(regionFadeOutNorm, 0.0f, 1.0f);
        HarkenSearchRegionFadeNorms fades{m_regionFadeInNorm, m_regionFadeOutNorm};
        fades = ClampRegionFades(fades, HarkenSearchFadeSide::FadeIn, 1.0f);
        fades = ClampRegionFades(fades, HarkenSearchFadeSide::FadeOut, 1.0f);
        m_regionFadeInNorm = fades.fadeInNorm;
        m_regionFadeOutNorm = fades.fadeOutNorm;
        (void)Platform::AudioPlayback::SeekToFrame(m_sound->Borrow(), inFrame);
        Platform::AudioPlayback::SetVolume(m_sound->Borrow(), std::max(0.0f, std::min(1.2f, m_volume * m_replayGain)) * ComputeRegionFadeGain(m_inPoint, m_inPoint, m_outPoint, m_regionFadeInNorm, m_regionFadeOutNorm));

        if (loopEnabled) {
            Platform::AudioPlayback::SetStopTimeInPcmFrames(m_sound->Borrow(), static_cast<std::uint64_t>(-1));
        } else {
            // stop_time uses the engine clock rather than a file-frame offset.
            // Convert region length to engine frames before scheduling the stop.
            std::uint32_t engineSR = Platform::AudioPlayback::QueryEngineSampleRate(m_engine->Borrow());
            if (engineSR == 0) engineSR = 48000;
            uint32_t fileSR = m_fileInfo.sampleRate > 0 ? (uint32_t)m_fileInfo.sampleRate : engineSR;
            uint64_t regionFrames = outFrame - inFrame;
            uint64_t regionEngineFrames = regionFrames * engineSR / fileSR;
            std::uint64_t engineNow = Platform::AudioPlayback::QueryEngineTimeInPcmFrames(m_engine->Borrow());
            // Add 512-frame safety buffer (~10ms @ 48kHz) to guard against the race where
            // the audio thread advances between engineNow read and the start command, which
            // would cause the stop time to already be in the past and silence the region.
            constexpr uint64_t kStopTimeSafetyFrames = 512;
            Platform::AudioPlayback::SetStopTimeInPcmFrames(m_sound->Borrow(), engineNow + regionEngineFrames + kStopTimeSafetyFrames);
        }

        const bool fadeEnabled = m_fadeEnabled.load();
        int fadeMs = m_seekFadeMs.load();
        if (fadeMs < 0) fadeMs = 0;
        if (fadeMs > 60) fadeMs = 60;
        if (fadeEnabled && fadeMs > 0 && !soundWasPlaying) {
            Platform::AudioPlayback::SetFadeInMilliseconds(m_sound->Borrow(), 0.0f, 1.0f, static_cast<std::uint64_t>(fadeMs));
        }

        m_playIntent = true;
        m_loopSawPlaying = m_regionLoopActive;
        (void)Platform::AudioPlayback::Start(
            m_sound->Borrow());
    }
}

void AudioEngine::pumpRegionPlayback() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_sound || !m_isLoaded || !m_regionPlayback) return;

    const bool playing = Platform::AudioPlayback::IsPlaying(m_sound->Borrow());
    if (playing) m_loopSawPlaying = true;
    const bool unselectedFullSoundLoop =
        m_userLoopEnabled &&
        m_inPoint <= 0.0f &&
        m_outPoint >= 1.0f;
    // Length-agnostic exhaustion restart. OGG/Vorbis push-mode reports 0 for
    // QueryLength, so totalFrames stays 0 until the waveform worker resolves
    // the real length — the old bail below made the first pass unable to wrap
    // at all (user had to click play a second time). Stopped + armed + intent +
    // observed-playing is the reliable "finished this pass" signal for every
    // format, even when the backend already reset the cursor to 0.
    if (ShouldRestartExhaustedUnselectedLoop(
            unselectedFullSoundLoop, m_regionLoopActive, playing, m_playIntent,
            m_loopSawPlaying)) {
        (void)Platform::AudioPlayback::SeekToFrame(m_sound->Borrow(), 0);
        Platform::AudioPlayback::SetStopTimeInPcmFrames(m_sound->Borrow(), static_cast<std::uint64_t>(-1));
        Platform::AudioPlayback::SetVolume(
            m_sound->Borrow(), std::max(0.0f, std::min(1.2f, m_volume * m_replayGain)));
        (void)Platform::AudioPlayback::Start(m_sound->Borrow());
        return;
    }

    if (m_fileInfo.totalFrames == 0) return;

    const uint64_t inFrame = (uint64_t)(m_inPoint * m_fileInfo.totalFrames);
    uint64_t outFrame = (uint64_t)(m_outPoint * m_fileInfo.totalFrames);
    if (unselectedFullSoundLoop) {
        outFrame = ResolveLoopedFullSoundOutFrame(
            m_fileInfo.totalFrames, m_decodedPlaybackTotalFrames);
    }
    if (outFrame <= inFrame + 1) {
        m_regionPlayback = false;
        m_regionLoopActive = false;
        m_regionFadeInNorm = 0.0f;
        m_regionFadeOutNorm = 0.0f;
        Platform::AudioPlayback::SetVolume(m_sound->Borrow(), std::max(0.0f, std::min(1.2f, m_volume * m_replayGain)));
        Platform::AudioPlayback::SetStopTimeInPcmFrames(m_sound->Borrow(), static_cast<std::uint64_t>(-1));
        return;
    }

    uint64_t cursor = 0;
    cursor = Platform::AudioPlayback::QueryCursor(m_sound->Borrow());
    const double totalFramesD = (double)((std::max)(1ULL, (unsigned long long)m_fileInfo.totalFrames));
    const float cursorNorm = std::clamp((float)((double)cursor / totalFramesD), 0.0f, 1.0f);
    const float regionGain = ComputeRegionFadeGain(cursorNorm, m_inPoint, m_outPoint, m_regionFadeInNorm, m_regionFadeOutNorm);
    Platform::AudioPlayback::SetVolume(m_sound->Borrow(), std::max(0.0f, std::min(1.2f, m_volume * m_replayGain)) * regionGain);

    if (m_regionLoopActive) {
        FullSoundLoopWrap wrap;
        bool shouldRestart = false;
        uint64_t restartFrame = inFrame;
        if (unselectedFullSoundLoop) {
            wrap = DecideUnselectedFullSoundLoopWrap(
                cursor, m_fileInfo.totalFrames, m_decodedPlaybackTotalFrames, playing, m_playIntent,
                m_loopSawPlaying);
            shouldRestart = wrap.wrap;
            restartFrame = wrap.seekFrame;
            // When the source stopped before the reported tail, the cursor is
            // the real playable end: learn it so the next pass wraps at the
            // true length instead of the inflated metadata.
            if (shouldRestart && !playing && m_decodedPlaybackTotalFrames == 0 && cursor > 0) {
                m_decodedPlaybackTotalFrames = cursor;
            }
        } else {
            shouldRestart = ShouldSeekLoopedRegionWhilePlaying(
                playing, ShouldRestartLoopedRegionFrames(cursor, inFrame, outFrame));
        }
        if (shouldRestart) {
            (void)Platform::AudioPlayback::SeekToFrame(m_sound->Borrow(), restartFrame);
            Platform::AudioPlayback::SetStopTimeInPcmFrames(m_sound->Borrow(), static_cast<std::uint64_t>(-1));
            Platform::AudioPlayback::SetVolume(m_sound->Borrow(), std::max(0.0f, std::min(1.2f, m_volume * m_replayGain)) * ComputeRegionFadeGain(m_inPoint, m_inPoint, m_outPoint, m_regionFadeInNorm, m_regionFadeOutNorm));
            // Only the armed user-intent exhaustion path may resume a stopped
            // sound. A playing wrap needs no Start; a paused sound never gets
            // here because playIntent is false.
            if (!playing && m_playIntent) {
                (void)Platform::AudioPlayback::Start(m_sound->Borrow());
            }
        }
        return;
    }

    if (!playing && cursor >= (outFrame - 1)) {
        m_regionPlayback = false;
        m_regionLoopActive = false;
        m_regionFadeInNorm = 0.0f;
        m_regionFadeOutNorm = 0.0f;
        Platform::AudioPlayback::SetVolume(m_sound->Borrow(), std::max(0.0f, std::min(1.2f, m_volume * m_replayGain)));
    }
}

void AudioEngine::stopRegionPlaybackMode() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_regionFadeInNorm = 0.0f;
    m_regionFadeOutNorm = 0.0f;
    if (m_userLoopEnabled) {
        m_regionPlayback = true;
        m_regionLoopActive = true;
    } else {
        m_regionPlayback = false;
        m_regionLoopActive = false;
    }
    if (m_sound) {
        Platform::AudioPlayback::SetVolume(m_sound->Borrow(), std::max(0.0f, std::min(1.2f, m_volume * m_replayGain)));
        Platform::AudioPlayback::SetStopTimeInPcmFrames(m_sound->Borrow(), static_cast<std::uint64_t>(-1));
    }
}

bool AudioEngine::exportRegion(const std::string& outputPath,
                               float regionFadeInNorm,
                               float regionFadeOutNorm,
                               ExportRegionInfo* outInfo,
                               const TransferProcessingSnapshot* transferSnapshot) {
    m_lastExportError.clear();
    DIAG_ACTION_FMT("Export",
        "ExportRegionStart: in=%s out=%s fadeIn=%.4f fadeOut=%.4f",
        m_fileInfo.filePath.c_str(), outputPath.c_str(), regionFadeInNorm, regionFadeOutNorm);
    auto failExportRegion = [&](const char* reason) {
        DIAG_ACTION_FMT("Export",
            "ExportRegionFailed: reason=%s error=%s",
            reason, m_lastExportError.c_str());
        return false;
    };
    if (!m_isLoaded || m_fileInfo.filePath.empty()) {
        m_lastExportError = L(Ex_NoSource);
        return failExportRegion("not_loaded");
    }

    // Defensive: norms are 0..1 from UI; reject non-finite / negative and cap above 1 for non-UI callers.
    if (!std::isfinite(regionFadeInNorm) || regionFadeInNorm < 0.0f) regionFadeInNorm = 0.0f;
    else if (regionFadeInNorm > 1.0f) regionFadeInNorm = 1.0f;
    if (!std::isfinite(regionFadeOutNorm) || regionFadeOutNorm < 0.0f) regionFadeOutNorm = 0.0f;
    else if (regionFadeOutNorm > 1.0f) regionFadeOutNorm = 1.0f;

    // 导出用独立 decoder；中心感在渲染循环中按当前 m_centerSense 应用，使拖入 DAW 与试听一致
    // 1. 参数快照（锁内获取，锁外处理，保证性能）
    const TransferProcessingSnapshot settings = transferSnapshot
        ? *transferSnapshot : captureTransferProcessingSnapshot();
    const float pitchSemitones = settings.pitchSemitones;
    const float volumeMultiplier = std::clamp(settings.volume * settings.replayGain, 0.0f, 1.2f);
    const float panAmount = settings.pan;
    const float centerSenseK = settings.centerSense;
    const float bassMonoFreqHz = settings.bassMonoFreqHz;
    const bool isReverse = settings.reverse;
    const bool isMono = settings.mono;
    const int soloMode = settings.channelSolo;
    const float inPt = settings.inPoint;
    const float outPt = settings.outPoint;
    const uint64_t refTotalFrames = settings.totalFrames;
    CenterSenseDsp::MagicEQProcessParams magicEQParams{};
    if (transferSnapshot) {
        magicEQParams.enabled = settings.magicEQ.enabled;
        magicEQParams.mode = settings.magicEQ.mode;
        magicEQParams.steepness = settings.magicEQ.steepness;
        magicEQParams.centerFreq = settings.magicEQ.centerFreq;
        magicEQParams.halfWidthPercent = settings.magicEQ.halfWidthPercent;
    }
    if (magicEQParams.enabled) {
        const auto availability = CenterSenseDsp::estimateMagicEQTail(
            magicEQParams, static_cast<int>(m_fileInfo.sampleRate));
        if (!availability.available) {
            m_lastExportError = availability.reason == CenterSenseDsp::MagicEQTailReason::TailBudget
                ? L(MagicEQ_TailTooLong) : L(MagicEQ_FrequencyUnavailable);
            return failExportRegion("magic_eq_unavailable");
        }
    }

    // 2. 配置解码器输出声道
    uint32_t outChannels = (isMono || soloMode != 0) ? 1 : m_fileInfo.channels;
    if (magicEQParams.enabled && (outChannels < 1 || outChannels > 2)) {
        m_lastExportError = L(MagicEQ_UnsupportedChannels);
        return failExportRegion("magic_eq_unsupported_channels");
    }
    // 处理 L/R Solo 的 Channel Map 提取
    Platform::AudioDataSource::Decoder decoder;
    const auto decoderInit = Platform::AudioDataSource::InitializeDecoderFromFileWithFormatAndChannelMap(
        decoder, m_fileInfo.filePath, outChannels, m_fileInfo.sampleRate, soloMode);
#ifdef _WIN32
    if (!decoderInit.Succeeded()) {
        m_lastExportError = L(Ex_DecoderMaybe);
        return failExportRegion("decoder_init");
    }
#else
    if (!decoderInit.Succeeded()) {
        m_lastExportError = L(Ex_DecoderMaybe);
        return failExportRegion("decoder_init");
    }
#endif

    uint64_t totalFrames =
        Platform::AudioDataSource::QueryLength(
            Platform::AudioDataSource::BorrowDataSource(decoder)).frames;

    totalFrames = ResolveExportFrameCount(totalFrames, refTotalFrames);
    if (totalFrames == 0) {
        m_lastExportError = L(Ex_ZeroFrames);
        Platform::AudioDataSource::UninitializeDecoder(decoder);
        return failExportRegion("zero_frames");
    }

    // Reverse the full source before applying region bounds so selection stays
    // aligned with the displayed reversed timeline.
    uint64_t startFrame, endFrame, framesToProcess;
    std::vector<float> reverseBuffer;
    bool useReverseBuffer = isReverse;

    if (isReverse) {
        // Region bounds are evaluated against the reversed full-file buffer.
        reverseBuffer.resize(totalFrames * outChannels);
        (void)Platform::AudioDataSource::SeekPcmFrame(
            Platform::AudioDataSource::BorrowDataSource(decoder),
            0);
        uint64_t framesRead = 0;
        const auto readResult = Platform::AudioDataSource::ReadPcmFrames(
            Platform::AudioDataSource::BorrowDataSource(decoder),
            reverseBuffer.data(),
            totalFrames);
        framesRead = readResult.framesRead;
        
        // 物理翻转整个文件
        for (uint64_t i = 0; i < framesRead / 2; i++) {
            for (uint32_t ch = 0; ch < outChannels; ch++) {
                std::swap(reverseBuffer[i * outChannels + ch], reverseBuffer[(framesRead - 1 - i) * outChannels + ch]);
            }
        }
        
        // 然后再基于翻转后的完整数据，计算选区
        startFrame = (uint64_t)(inPt * framesRead);
        endFrame = (uint64_t)(outPt * framesRead);
        if (endFrame <= startFrame) { m_lastExportError = L(Ex_RegionReversed); Platform::AudioDataSource::UninitializeDecoder(decoder); return failExportRegion("invalid_region_reversed"); }
        framesToProcess = endFrame - startFrame;
    } else {
        // 正常正放模式：直接计算选区并 Seek
        startFrame = (uint64_t)(inPt * totalFrames);
        endFrame = (uint64_t)(outPt * totalFrames);
        if (endFrame <= startFrame) { m_lastExportError = L(Ex_RegionInvalid); Platform::AudioDataSource::UninitializeDecoder(decoder); return failExportRegion("invalid_region"); }
        framesToProcess = endFrame - startFrame;
        (void)Platform::AudioDataSource::SeekPcmFrame(
            Platform::AudioDataSource::BorrowDataSource(decoder),
            startFrame);
    }

    // 4. 配置编码器（s24 影视工业标准，Pro Tools 等 DAW 兼容性优于 f32）
    Platform::AudioFile::S24WavWriter encoder;
    if (!encoder.Open(outputPath, static_cast<int>(m_fileInfo.sampleRate), static_cast<int>(outChannels))) {
        m_lastExportError = L(Ex_TempFileMaybe);
        Platform::AudioDataSource::UninitializeDecoder(decoder); return failExportRegion("encoder_init");
    }

    // 5. 配置重采样器 (处理 Pitch)
    float pitchMultiplier = std::pow(2.0f, pitchSemitones / 12.0f);
    bool needsResampling = (std::abs(pitchSemitones) > 0.01f);
    Platform::AudioFile::LinearFloatResampler resampler;
    if (needsResampling) {
        if (!resampler.Open(
                outChannels,
                static_cast<std::uint32_t>(m_fileInfo.sampleRate * pitchMultiplier + 0.5f),
                m_fileInfo.sampleRate)) {
            m_lastExportError = L(Ex_ResampleInit);
            encoder.Close();
            Platform::AudioDataSource::UninitializeDecoder(decoder);
            return failExportRegion("resampler_init");
        }
    }

    const bool needsPan = std::abs(panAmount) > 0.001f && !isMono && soloMode == 0 && outChannels == 2;
    if (std::abs(panAmount) > 0.001f && !needsPan) {
        DIAG_ACTION_FMT("Export",
            "ExportRegionPanSkipped: path=%s, outChannels=%u, mono=%d, solo=%d, pan=%.3f",
            m_fileInfo.filePath.c_str(), outChannels, isMono ? 1 : 0, soloMode, panAmount);
    }

    // Export edge fades (after all DSP): either UI region fades (norms of region length) or legacy symmetric ~10ms declick.
    const bool useRegionFade = (regionFadeInNorm > 1e-6f || regionFadeOutNorm > 1e-6f);
    uint64_t exportFadeLenSymmetric = 0;
    uint64_t regionFadeInFrames = 0;
    uint64_t regionFadeOutFrames = 0;
    if (!useRegionFade) {
        const uint64_t fadeLenFrames = (uint64_t)(m_fileInfo.sampleRate * 0.010f + 0.5f);
        exportFadeLenSymmetric = (fadeLenFrames > 0 && framesToProcess > fadeLenFrames * 2)
            ? fadeLenFrames : 0;
    } else {
        regionFadeInFrames = (uint64_t)((double)regionFadeInNorm * (double)framesToProcess + 0.5);
        regionFadeOutFrames = (uint64_t)((double)regionFadeOutNorm * (double)framesToProcess + 0.5);
        if (regionFadeInFrames > framesToProcess) regionFadeInFrames = framesToProcess;
        if (regionFadeOutFrames > framesToProcess - regionFadeInFrames)
            regionFadeOutFrames = framesToProcess - regionFadeInFrames;
        // Norms were non-zero but rounded to 0 frames: still need edge protection (legacy symmetric declick).
        if (regionFadeInFrames == 0 && regionFadeOutFrames == 0) {
            const uint64_t fadeLenFrames = (uint64_t)(m_fileInfo.sampleRate * 0.010f + 0.5f);
            exportFadeLenSymmetric = (fadeLenFrames > 0 && framesToProcess > fadeLenFrames * 2)
                ? fadeLenFrames : 0;
        }
    }

    // 6. 核心渲染循环（编码器为 s24，需将 f32 转为 s24 再写入）
    const uint64_t chunkSize = 4096;
    std::vector<float> inputBuf(chunkSize * outChannels);
    std::vector<float> outputBuf;
    if (needsResampling) outputBuf.resize(chunkSize * outChannels);
    // s24 紧密打包 3 字节/采样，供 Pro Tools 等 DAW 兼容
    std::vector<uint8_t> s24Buf(static_cast<size_t>(chunkSize * outChannels * 3));

    uint64_t framesRemaining = framesToProcess;
    uint64_t decoderFramesLeft = framesToProcess; // actual remaining frames to read from source (decoder/reverseBuffer)
    uint64_t bufferOffset = startFrame; // 反转模式下从选区起点开始读内存
    uint64_t totalInputConsumed = 0;    // 已消费的输入帧数（用于淡入淡出位置与剩余计数）
    uint64_t pendingInputFrames = 0;    // 重采样时未消费完的帧数（保留在 inputBuf 开头）
    CenterSenseDsp::BassMonoState exportBassMonoState = {};   // 跨 chunk 保持低频 mono 滤波器状态
    CenterSenseDsp::ProcessState exportCenterSenseState = {}; // 跨 chunk 保持滤波器状态
    CenterSenseDsp::MagicEQState exportMagicEQState = {};

    while (framesRemaining > 0 || (needsResampling && pendingInputFrames > 0)) {
        uint64_t toRead = (needsResampling ? (chunkSize - pendingInputFrames) : chunkSize);
        // For resampling: bound by actual decoder reads left (not framesRemaining, which
        // tracks resampler consumption and can diverge due to pending frame accumulation).
        toRead = std::min(toRead, needsResampling ? decoderFramesLeft : framesRemaining);
        uint64_t framesRead = 0;

        if (useReverseBuffer) {
            memcpy(inputBuf.data() + pendingInputFrames * outChannels,
                reverseBuffer.data() + bufferOffset * outChannels, toRead * outChannels * sizeof(float));
            framesRead = toRead;
            bufferOffset += toRead;
        } else {
            const auto readResult = Platform::AudioDataSource::ReadPcmFrames(
                Platform::AudioDataSource::BorrowDataSource(decoder),
                inputBuf.data() + pendingInputFrames * outChannels,
                toRead);
            framesRead = readResult.framesRead;
        }
        if (needsResampling) decoderFramesLeft -= framesRead;
        if (framesRead == 0 && pendingInputFrames == 0) break;

        uint64_t totalInputFrames = pendingInputFrames + framesRead;
        if (totalInputFrames == 0) break;

        // DSP must only process newly read frames to avoid double-processing
        // pending (unconsumed) frames that were already processed in a previous iteration.
        float* newDataPtr = inputBuf.data() + pendingInputFrames * outChannels;
        uint64_t newFrames = framesRead;

        if (std::abs(volumeMultiplier - 1.0f) > 0.001f) {
            for (size_t i = 0; i < newFrames * outChannels; i++) newDataPtr[i] *= volumeMultiplier;
        }
        if (needsPan) {
            const float angle = (std::clamp(panAmount, -1.0f, 1.0f) + 1.0f) * 0.25f * 3.1415926535f;
            const float leftGain = std::cos(angle);
            const float rightGain = std::sin(angle);
            for (uint64_t f = 0; f < newFrames; ++f) {
                newDataPtr[(size_t)f * 2 + 0] *= leftGain;
                newDataPtr[(size_t)f * 2 + 1] *= rightGain;
            }
        }
        /* Bass mono / center sense: these are stateful IIR filters — they must process
           the full buffer sequentially to maintain filter continuity. However, pending
           frames were already filtered. Re-running the filter on them would corrupt state.
           Solution: only feed new frames; filter state carries over from previous iteration. */
        if (outChannels == 2 && bassMonoFreqHz > 0.0f) {
            CenterSenseDsp::processBassMono(
                newDataPtr, (size_t)newFrames, (int)m_fileInfo.sampleRate,
                bassMonoFreqHz, &exportBassMonoState);
        }
        if (outChannels == 2 && std::abs(centerSenseK) > 0.001f) {
            CenterSenseDsp::processCenterSense(
                newDataPtr, (size_t)newFrames, (int)m_fileInfo.sampleRate,
                centerSenseK, &exportCenterSenseState);
        }
        if (transferSnapshot) {
            CenterSenseDsp::processMagicEQ(
                newDataPtr, (size_t)newFrames, (int)outChannels,
                (int)m_fileInfo.sampleRate, magicEQParams, &exportMagicEQState);
        }
        // Fades: only apply to new frames. Pending frames were already faded in the
        // previous iteration. Adjust globalStartFrame to account for pending offset.
        const uint64_t newGlobalStart = totalInputConsumed + pendingInputFrames;
        if (useRegionFade && (regionFadeInFrames > 0 || regionFadeOutFrames > 0)) {
            ApplyExportRegionFadeAsymmetric(newDataPtr, (size_t)newFrames, (size_t)outChannels,
                (size_t)newGlobalStart, (size_t)framesToProcess,
                (size_t)regionFadeInFrames, (size_t)regionFadeOutFrames);
        } else if (exportFadeLenSymmetric > 0) {
            ApplyExportDeclickFade(newDataPtr, (size_t)newFrames, (size_t)outChannels,
                (size_t)newGlobalStart, (size_t)framesToProcess, (size_t)exportFadeLenSymmetric);
        }
        if (needsResampling) {
            const auto resampleProgress = resampler.Process(
                inputBuf.data(), totalInputFrames, outputBuf.data(), chunkSize);
            uint64_t outFrames = resampleProgress.outputProduced;
            uint64_t inConsumed = resampleProgress.inputConsumed;
            if (outFrames > 0) {
                uint64_t samples = outFrames * outChannels;
                ApplyExportSoftClip(outputBuf.data(), (size_t)samples);
                Platform::AudioFile::ConvertFloatToPackedS24(
                    s24Buf.data(), outputBuf.data(), samples);
                encoder.Write(s24Buf.data(), outFrames, nullptr);
            }
            uint64_t unconsumed = totalInputFrames - inConsumed;
            if (unconsumed > 0) {
                memmove(inputBuf.data(), inputBuf.data() + (size_t)inConsumed * outChannels, (size_t)unconsumed * outChannels * sizeof(float));
                pendingInputFrames = unconsumed;
            } else {
                pendingInputFrames = 0;
            }
            totalInputConsumed += inConsumed;
            // Guard against unsigned underflow: when upsampling (pitch down), the
            // resampler consumes fewer frames per output chunk, so pending frames
            // accumulate. Near the selection end the resampler can consume more
            // accumulated frames than framesRemaining, wrapping uint64_t to ~2^64
            // and causing the loop to read the entire file past the OUT point.
            framesRemaining = (inConsumed >= framesRemaining) ? 0 : (framesRemaining - inConsumed);
        } else {
            uint64_t samples = framesRead * outChannels;
            ApplyExportSoftClip(inputBuf.data(), (size_t)samples);
            Platform::AudioFile::ConvertFloatToPackedS24(
                s24Buf.data(), inputBuf.data(), samples);
            encoder.Write(s24Buf.data(), framesRead, nullptr);
            totalInputConsumed += framesRead;
            framesRemaining -= framesRead;
        }
    }

    // 重采样结束时排出 resampler 内剩余样本，避免截断或边界不连续（减少整段断续 click）
    // 限制 drain 迭代与总帧数，防止 resampler 内部状态过长导致导出尾部大量过响/削波（后半段 clip 根因之一）
    if (needsResampling) {
        const uint64_t maxDrainIterations = 32;
        const uint64_t maxDrainFrames = 8192;
        uint64_t totalDrainFrames = 0;
        for (uint64_t iter = 0; iter < maxDrainIterations && totalDrainFrames < maxDrainFrames; ++iter) {
            const auto drainProgress = resampler.Drain(outputBuf.data(), chunkSize);
            uint64_t outFrames = drainProgress.outputProduced;
            if (outFrames == 0) break;
            totalDrainFrames += outFrames;
            uint64_t samples = outFrames * outChannels;
            ApplyExportSoftClip(outputBuf.data(), (size_t)samples);
            Platform::AudioFile::ConvertFloatToPackedS24(
                s24Buf.data(), outputBuf.data(), samples);
            encoder.Write(s24Buf.data(), outFrames, nullptr);
        }
        resampler.Close();
    }
    encoder.Close();
    Platform::AudioDataSource::UninitializeDecoder(decoder);
    if (outInfo) {
        outInfo->regionStartFrame = AudioEngine::computeRegionStartFrame(inPt, outPt, totalFrames, isReverse);
        outInfo->framesWritten = framesToProcess;
        outInfo->totalFrames = totalFrames;
        outInfo->sampleRate = m_fileInfo.sampleRate;
        outInfo->channels = outChannels;
    }
    DIAG_ACTION_FMT("Export",
        "ExportRegionDone: path=%s, ch=%u, sr=%u, frames=%llu, vol=%.3f, mono=%d, solo=%d, fadeIn=%.4f, fadeOut=%.4f",
        outputPath.c_str(), outChannels, m_fileInfo.sampleRate,
        (unsigned long long)framesToProcess, volumeMultiplier, isMono ? 1 : 0, soloMode,
        regionFadeInNorm, regionFadeOutNorm);
    return true;
}

bool AudioEngine::hasAudioAdjustments() const {
    return (std::abs(m_pitch) > 0.01f ||
            std::abs(m_volume - 1.0f) > 0.01f ||
            std::abs(m_pan) > 0.001f ||
            std::abs(m_centerSense) > 0.001f ||
            m_reverse ||
            m_channelSolo != 0 ||
            m_forceMono);
}

bool AudioEngine::exportFileProcessed(const std::string& inputPath, const std::string& outputPath,
                                      const TransferProcessingSnapshot* transferSnapshot) {
    m_lastExportError.clear();
    auto failProcessedExport = [&](const char* reason) {
        DIAG_ACTION_FMT("Export",
            "ExportFileProcessedFailed: reason=%s error=%s in=%s out=%s",
            reason, m_lastExportError.c_str(), inputPath.c_str(), outputPath.c_str());
        return false;
    };
    if (inputPath.empty()) { m_lastExportError = L(Ex_InputEmpty); return failProcessedExport("empty_input"); }
    DIAG_ACTION_FMT("Export",
        "ExportFileProcessedStart: in=%s out=%s",
        inputPath.c_str(), outputPath.c_str());

    // 1. Snapshot current DSP settings (same as exportRegion but no replayGain — that's per-loaded-file)
    const TransferProcessingSnapshot settings = transferSnapshot
        ? *transferSnapshot : captureTransferProcessingSnapshot();
    const float pitchSemitones = settings.pitchSemitones;
    const float volumeMultiplier = std::clamp(settings.volume, 0.0f, 1.2f);  // no replayGain
    const float panAmount = settings.pan;
    const float centerSenseK = settings.centerSense;
    const float bassMonoFreqHz = settings.bassMonoFreqHz;
    const bool isMono = settings.mono;
    const int soloMode = settings.channelSolo;
    CenterSenseDsp::MagicEQProcessParams magicEQParams{};
    if (transferSnapshot) {
        magicEQParams.enabled = settings.magicEQ.enabled;
        magicEQParams.mode = settings.magicEQ.mode;
        magicEQParams.steepness = settings.magicEQ.steepness;
        magicEQParams.centerFreq = settings.magicEQ.centerFreq;
        magicEQParams.halfWidthPercent = settings.magicEQ.halfWidthPercent;
    }

    // 2. Probe input file for format info
    uint32_t fileChannels = 2;
    uint32_t fileSampleRate = 48000;
    {
        Platform::AudioDataSource::Decoder probe;
        const auto probeResult = Platform::AudioDataSource::InitializeDecoderFromFile(
            probe, inputPath);
        if (probeResult.Succeeded()) {
            const auto probeFormat = Platform::AudioDataSource::QueryDataFormat(
                Platform::AudioDataSource::BorrowDataSource(probe));
            if (probeFormat.Succeeded()) {
                fileChannels = probeFormat.channels;
                fileSampleRate = probeFormat.sampleRate;
            }
            Platform::AudioDataSource::UninitializeDecoder(probe);
        }
    }

    // 3. Configure decoder with desired output channels
    if (magicEQParams.enabled) {
        const auto availability = CenterSenseDsp::estimateMagicEQTail(
            magicEQParams, static_cast<int>(fileSampleRate));
        if (!availability.available) {
            m_lastExportError = availability.reason == CenterSenseDsp::MagicEQTailReason::TailBudget
                ? L(MagicEQ_TailTooLong) : L(MagicEQ_FrequencyUnavailable);
            return failProcessedExport("magic_eq_unavailable");
        }
    }
    uint32_t outChannels = (isMono || soloMode != 0) ? 1 : fileChannels;
    if (magicEQParams.enabled && (outChannels < 1 || outChannels > 2)) {
        m_lastExportError = L(MagicEQ_UnsupportedChannels);
        return failProcessedExport("magic_eq_unsupported_channels");
    }
    Platform::AudioDataSource::Decoder decoder;
    const auto decoderInit = Platform::AudioDataSource::InitializeDecoderFromFileWithFormatAndChannelMap(
        decoder, inputPath, outChannels, fileSampleRate, soloMode);
#ifdef _WIN32
    if (!decoderInit.Succeeded()) {
#else
    if (!decoderInit.Succeeded()) {
#endif
        m_lastExportError = L(Ex_DecoderInit); return failProcessedExport("decoder_init");
    }

    uint64_t totalFrames =
        Platform::AudioDataSource::QueryLength(
            Platform::AudioDataSource::BorrowDataSource(decoder)).frames;
    if (totalFrames == 0) { m_lastExportError = L(Ex_ZeroFrames); Platform::AudioDataSource::UninitializeDecoder(decoder); return failProcessedExport("zero_frames"); }

    // 4. Encoder (s24 WAV for DAW compat)
    Platform::AudioFile::S24WavWriter encoder;
    if (!encoder.Open(outputPath, static_cast<int>(fileSampleRate), static_cast<int>(outChannels))) {
        m_lastExportError = L(Ex_OutputCreateFail); Platform::AudioDataSource::UninitializeDecoder(decoder); return failProcessedExport("encoder_init");
    }

    // 5. Resampler for pitch
    float pitchMultiplier = std::pow(2.0f, pitchSemitones / 12.0f);
    bool needsResampling = (std::abs(pitchSemitones) > 0.01f);
    Platform::AudioFile::LinearFloatResampler resampler;
    if (needsResampling) {
        if (!resampler.Open(
                outChannels,
                static_cast<std::uint32_t>(fileSampleRate * pitchMultiplier + 0.5f),
                fileSampleRate)) {
            m_lastExportError = L(Ex_ResampleInit);
            encoder.Close(); Platform::AudioDataSource::UninitializeDecoder(decoder); return failProcessedExport("resampler_init");
        }
    }

    const bool needsPan = std::abs(panAmount) > 0.001f && !isMono && soloMode == 0 && outChannels == 2;

    // Edge declick (~10ms)
    const uint64_t fadeLenFrames = (uint64_t)(fileSampleRate * 0.010f + 0.5f);
    const uint64_t exportFadeLenSymmetric = (fadeLenFrames > 0 && totalFrames > fadeLenFrames * 2) ? fadeLenFrames : 0;

    // 6. Render loop (simplified from exportRegion: no region, no reverse)
    const uint64_t chunkSize = 4096;
    std::vector<float> inputBuf(chunkSize * outChannels);
    std::vector<float> outputBuf;
    if (needsResampling) outputBuf.resize(chunkSize * outChannels);
    std::vector<uint8_t> s24Buf(static_cast<size_t>(chunkSize * outChannels * 3));

    uint64_t framesRemaining = totalFrames;
    uint64_t decoderFramesLeft = totalFrames;
    uint64_t totalInputConsumed = 0;
    uint64_t pendingInputFrames = 0;
    CenterSenseDsp::BassMonoState bmState = {};
    CenterSenseDsp::ProcessState csState = {};
    CenterSenseDsp::MagicEQState magicEQState = {};

    while (framesRemaining > 0 || (needsResampling && pendingInputFrames > 0)) {
        uint64_t toRead = needsResampling ? (chunkSize - pendingInputFrames) : chunkSize;
        toRead = std::min(toRead, needsResampling ? decoderFramesLeft : framesRemaining);
        uint64_t framesRead = 0;
        const auto readResult = Platform::AudioDataSource::ReadPcmFrames(
            Platform::AudioDataSource::BorrowDataSource(decoder),
            inputBuf.data() + pendingInputFrames * outChannels,
            toRead);
        framesRead = readResult.framesRead;
        if (needsResampling) decoderFramesLeft -= framesRead;
        if (framesRead == 0 && pendingInputFrames == 0) break;

        uint64_t totalInputFrames = pendingInputFrames + framesRead;
        if (totalInputFrames == 0) break;

        float* newData = inputBuf.data() + pendingInputFrames * outChannels;
        uint64_t newFrames = framesRead;

        // Volume
        if (std::abs(volumeMultiplier - 1.0f) > 0.001f) {
            for (size_t i = 0; i < newFrames * outChannels; i++) newData[i] *= volumeMultiplier;
        }
        // Pan
        if (needsPan) {
            const float angle = (std::clamp(panAmount, -1.0f, 1.0f) + 1.0f) * 0.25f * 3.1415926535f;
            const float lG = std::cos(angle), rG = std::sin(angle);
            for (uint64_t f = 0; f < newFrames; ++f) { newData[f*2] *= lG; newData[f*2+1] *= rG; }
        }
        // Bass mono
        if (outChannels == 2 && bassMonoFreqHz > 0.0f) {
            CenterSenseDsp::processBassMono(newData, (size_t)newFrames, (int)fileSampleRate, bassMonoFreqHz, &bmState);
        }
        // Center sense
        if (outChannels == 2 && std::abs(centerSenseK) > 0.001f) {
            CenterSenseDsp::processCenterSense(newData, (size_t)newFrames, (int)fileSampleRate, centerSenseK, &csState);
        }
        if (transferSnapshot) {
            CenterSenseDsp::processMagicEQ(
                newData, (size_t)newFrames, (int)outChannels, (int)fileSampleRate,
                magicEQParams, &magicEQState);
        }
        // Edge declick
        const uint64_t newGlobalStart = totalInputConsumed + pendingInputFrames;
        if (exportFadeLenSymmetric > 0) {
            ApplyExportDeclickFade(newData, (size_t)newFrames, (size_t)outChannels,
                (size_t)newGlobalStart, (size_t)totalFrames, (size_t)exportFadeLenSymmetric);
        }

        if (needsResampling) {
            const auto resampleProgress = resampler.Process(
                inputBuf.data(), totalInputFrames, outputBuf.data(), chunkSize);
            uint64_t outFrames = resampleProgress.outputProduced;
            uint64_t inConsumed = resampleProgress.inputConsumed;
            if (outFrames > 0) {
                ApplyExportSoftClip(outputBuf.data(), (size_t)(outFrames * outChannels));
                Platform::AudioFile::ConvertFloatToPackedS24(
                    s24Buf.data(), outputBuf.data(), outFrames * outChannels);
                encoder.Write(s24Buf.data(), outFrames, nullptr);
            }
            uint64_t unconsumed = totalInputFrames - inConsumed;
            if (unconsumed > 0) memmove(inputBuf.data(), inputBuf.data() + inConsumed * outChannels, unconsumed * outChannels * sizeof(float));
            pendingInputFrames = unconsumed;
            totalInputConsumed += inConsumed;
            framesRemaining = (inConsumed >= framesRemaining) ? 0 : (framesRemaining - inConsumed);
        } else {
            ApplyExportSoftClip(inputBuf.data(), (size_t)(framesRead * outChannels));
            Platform::AudioFile::ConvertFloatToPackedS24(
                s24Buf.data(), inputBuf.data(), framesRead * outChannels);
            encoder.Write(s24Buf.data(), framesRead, nullptr);
            totalInputConsumed += framesRead;
            framesRemaining -= framesRead;
        }
    }

    // Drain resampler
    if (needsResampling) {
        for (int iter = 0; iter < 32; ++iter) {
            const auto drainProgress = resampler.Drain(outputBuf.data(), chunkSize);
            uint64_t outF = drainProgress.outputProduced;
            if (outF == 0) break;
            ApplyExportSoftClip(outputBuf.data(), (size_t)(outF * outChannels));
            Platform::AudioFile::ConvertFloatToPackedS24(
                s24Buf.data(), outputBuf.data(), outF * outChannels);
            encoder.Write(s24Buf.data(), outF, nullptr);
        }
        resampler.Close();
    }
    encoder.Close();
    Platform::AudioDataSource::UninitializeDecoder(decoder);
    DIAG_ACTION_FMT("Export",
        "ExportFileProcessed: in=%s out=%s ch=%u sr=%u frames=%llu vol=%.3f mono=%d solo=%d pitch=%.2f",
        inputPath.c_str(), outputPath.c_str(), outChannels, fileSampleRate,
        (unsigned long long)totalFrames, volumeMultiplier, isMono ? 1 : 0, soloMode, pitchSemitones);
    return true;
}

void AudioEngine::ensureSeekWorkerStarted() {
    std::lock_guard<std::mutex> lk(m_seekWorkerMutex);
    if (m_seekWorker.joinable()) return;
    m_seekWorkerExit.store(false);
    m_seekWorker = std::thread([this]() { seekWorkerLoop(); });
}

void AudioEngine::stopSeekWorker() {
    {
        std::lock_guard<std::mutex> lk(m_seekWorkerMutex);
        m_seekWorkerExit.store(true);
    }
    m_seekWorkerCv.notify_all();
    if (m_seekWorker.joinable()) {
        m_seekWorker.join();
    }
    m_seekPending.store(false);
}

void AudioEngine::seekWorkerLoop() {
    while (!m_seekWorkerExit.load()) {
        if (!m_seekPending.load()) {
            std::unique_lock<std::mutex> lk(m_seekWorkerMutex);
            m_seekWorkerCv.wait_for(lk, std::chrono::milliseconds(50), [&]() {
                return m_seekWorkerExit.load() || m_seekPending.load();
            });
            continue;
        }

        uint64_t token = m_seekToken.load();
        uint64_t target = (uint64_t)m_seekTargetFrame.load();
        int fadeMs = m_seekFadeMs.load();
        if (fadeMs < 0) fadeMs = 0;
        if (fadeMs > 60) fadeMs = 60;

        if (fadeMs > 0) std::this_thread::sleep_for(std::chrono::milliseconds(fadeMs));

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (!m_seekPending.load() || m_seekToken.load() != token) {
                continue;
            }
            if (!m_sound || !m_isLoaded || m_fileInfo.totalFrames == 0) {
                m_seekPending.store(false);
                continue;
            }

            uint64_t clamped = target;
            if (clamped > m_fileInfo.totalFrames) clamped = m_fileInfo.totalFrames;
            (void)Platform::AudioPlayback::SeekToFrame(m_sound->Borrow(), clamped);

            // Fade 0→1 as multiplier; base volume already set via set_volume.
            Platform::AudioPlayback::SetFadeInMilliseconds(m_sound->Borrow(), 0.0f, 1.0f, static_cast<std::uint64_t>(fadeMs));
            m_seekPending.store(false);
        }
    }
}

void AudioEngine::seekToPosition(float normalizedPos) {
    normalizedPos = std::clamp(normalizedPos, 0.0f, 1.0f);

    // 只要当前有"新文件正在加载"，seek 就一律预约给新文件，
    // 不要错误地先去跳旧文件。
    if (isLoadFilePending()) {
        m_pendingNormalizedSeek.store(normalizedPos, std::memory_order_relaxed);
        return;
    }

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_sound && m_isLoaded && m_fileInfo.totalFrames > 0) {
            const double norm = std::clamp((double)normalizedPos, 0.0, 1.0);

            // 用 llround + totalFrames-1，减少边界点击时的偏差感
            const uint64_t maxFrame =
                (m_fileInfo.totalFrames > 0) ? (m_fileInfo.totalFrames - 1) : 0;
            const uint64_t targetFrame =
                (uint64_t)std::llround(norm * (double)maxFrame);

            const bool playing = Platform::AudioPlayback::IsPlaying(m_sound->Borrow());
            const bool fadeEnabled = m_fadeEnabled.load();
            int fadeMs = m_seekFadeMs.load();
            if (fadeMs < 0) fadeMs = 0;
            if (fadeMs > 60) fadeMs = 60;

            // mini 波形点击更需要"点哪到哪"，所以取消旧的异步 seek，直接做同步硬跳。
            m_seekPending.store(false, std::memory_order_relaxed);
            m_seekToken.fetch_add(1, std::memory_order_seq_cst);

            // 手动 seek 视为退出本次框选播放；用户循环开关仍由上层保留。
            if (m_userLoopEnabled) {
                m_regionPlayback = true;
                m_regionLoopActive = true;
            } else {
                m_regionPlayback = false;
                m_regionLoopActive = false;
            }
            m_regionFadeInNorm = 0.0f;
            m_regionFadeOutNorm = 0.0f;

            // 清掉旧的 region stop time，避免点击跳转后仍受旧区域限制
            Platform::AudioPlayback::SetVolume(m_sound->Borrow(), std::max(0.0f, std::min(1.2f, m_volume * m_replayGain)));
            // Only clear stop-time for active playback. On a paused sound this can
            // make miniaudio report "playing" before an explicit start command, causing UI
            // click-to-seek paths to skip their explicit resume call.
            if (playing) {
                Platform::AudioPlayback::SetStopTimeInPcmFrames(m_sound->Borrow(), static_cast<std::uint64_t>(-1));
            }

            if (playing && fadeEnabled && fadeMs > 0) {
                const int clickSeekFadeMs = (std::min)(fadeMs, 8);
                (void)Platform::AudioPlayback::SeekToFrame(m_sound->Borrow(), targetFrame);
                // Fade 0→1 as multiplier; base volume already set above.
                Platform::AudioPlayback::SetFadeInMilliseconds(
                    m_sound->Borrow(), 0.0f, 1.0f, static_cast<std::uint64_t>(clickSeekFadeMs));
            } else {
                (void)Platform::AudioPlayback::SeekToFrame(m_sound->Borrow(), targetFrame);
            }
            return;
        }
    }
}

bool AudioEngine::isPlaying() const {
    // 快速路径：先检查 sound 指针，避免不必要的锁
    auto sound = Platform::AudioPlayback::BorrowSound(nullptr);
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        sound = m_sound ? m_sound->Borrow() : Platform::AudioPlayback::BorrowSound(nullptr);
    }
    if (sound.IsValid()) {
        return Platform::AudioPlayback::IsPlaying(sound);
    }
    return false;
}

bool AudioEngine::isPaused() const {
    return m_isLoaded && !isPlaying();
}

uint32_t AudioEngine::getEngineSampleRate() const {
    return m_engine ? Platform::AudioPlayback::QueryEngineSampleRate(m_engine->Borrow()) : 48000;
}

bool AudioEngine::isLoaded() const {
    return m_isLoaded;
}

float AudioEngine::getPlaybackPosition() const {
    auto sound = Platform::AudioPlayback::BorrowSound(nullptr);
    uint64_t totalFrames = 0;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_isLoaded || m_fileInfo.totalFrames == 0) return 0.0f;
        sound = m_sound ? m_sound->Borrow() : Platform::AudioPlayback::BorrowSound(nullptr);
        totalFrames = m_fileInfo.totalFrames;
    }

    if (sound.IsValid() && totalFrames > 0) {
        uint64_t cursor = 0;
        cursor = Platform::AudioPlayback::QueryCursor(sound);
        return (float)cursor / (float)totalFrames;
    }
    return 0.0f;
}

float AudioEngine::getOutputLatencyInSeconds() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_engine) return 0.0f;
    const auto metrics =
        Platform::AudioPlayback::QueryPlaybackDeviceMetrics(
            m_engine->Borrow());
    if (!metrics.IsValid()) return 0.0f;
    std::uint32_t periodFrames = metrics.periodFrames;
    std::uint32_t periods = metrics.periods;
    std::uint32_t deviceSR = metrics.sampleRate;
    if (deviceSR == 0) deviceSR = 48000;
    return (float)periodFrames * (float)periods / (float)deviceSR;
}

float AudioEngine::getCurrentTimeSeconds() const {
    return getPlaybackPosition() * m_fileInfo.durationSeconds;
}

void AudioEngine::generateWaveform(int targetPeaks) {
    if (!m_isLoaded || m_fileInfo.filePath.empty() || !m_engine) return;
    if (targetPeaks < 200) targetPeaks = 200;
    if (targetPeaks > 50000) targetPeaks = 50000;

    WaveformData wf;
    std::string path;
    bool mono = false;
    int solo = 0;
    Platform::AudioPlayback::BorrowedEngine engine = Platform::AudioPlayback::BorrowEngine(nullptr);
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        path = m_fileInfo.filePath;
        mono = m_forceMono;
        solo = m_channelSolo;
        engine = m_engine->Borrow();
    }
    if (!buildWaveform(path, engine, mono, solo, wf, targetPeaks)) return;
    wf.filePath = path;

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_isLoaded && m_fileInfo.filePath == path && m_forceMono == mono) {
            std::shared_ptr<const WaveformData> p = std::make_shared<WaveformData>(std::move(wf));
            std::atomic_store(&m_waveform, p);
            m_waveformGeneratedPeaks.store(targetPeaks);
        }
    }
}

void AudioEngine::requestWaveformDetail(int targetPeaks) {
    std::string pathSnapshot;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_isLoaded || m_fileInfo.filePath.empty()) return;
        pathSnapshot = m_fileInfo.filePath;
    }
    if (targetPeaks < 200) targetPeaks = 200;
    if (targetPeaks > 50000) targetPeaks = 50000;

    int cur = m_waveformRequestedPeaks.load();
    bool upgraded = false;
    while (cur < targetPeaks) {
        if (m_waveformRequestedPeaks.compare_exchange_weak(cur, targetPeaks)) {
            upgraded = true;
            break;
        }
    }

    if (upgraded) {
        m_waveformRequestToken.fetch_add(1, std::memory_order_relaxed);
    }

    DIAG_SYSTEM_FMT("Waveform",
        "MainDetailRequestQueued: path=%s targetPeaks=%d upgraded=%d requestedPeaks=%d generatedPeaks=%d",
        pathSnapshot.c_str(),
        targetPeaks,
        upgraded ? 1 : 0,
        m_waveformRequestedPeaks.load(std::memory_order_relaxed),
        m_waveformGeneratedPeaks.load(std::memory_order_relaxed));

    ensureWaveformWorkerStarted();
    m_waveformWorkerCv.notify_one();
}

bool AudioEngine::isWaveformDetailPending() const {
    return m_waveformRequestedPeaks.load() > m_waveformGeneratedPeaks.load();
}

void AudioEngine::forceRegenerateWaveform(int targetPeaks) {
    if (!m_isLoaded || m_fileInfo.filePath.empty()) return;
    if (targetPeaks < 200) targetPeaks = 200;
    if (targetPeaks > 50000) targetPeaks = 50000;

    m_waveformRequestToken.fetch_add(1, std::memory_order_relaxed);
    m_waveformGeneratedPeaks.store(0);
    m_waveformRequestedPeaks.store(targetPeaks);

    ensureWaveformWorkerStarted();
    m_waveformWorkerCv.notify_one();
}

bool AudioEngine::readAudioData(std::vector<float>& outSamples) {
    return false;
}

bool AudioEngine::buildWaveformDetailForCache(const std::string& filePath,
                                              WaveformData& outWaveform,
                                              int targetPeaks) {
    Platform::AudioPlayback::BorrowedEngine engine = Platform::AudioPlayback::BorrowEngine(nullptr);
    bool isCurrentFile = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        engine = m_engine->Borrow();
        isCurrentFile = (m_isLoaded && m_fileInfo.filePath == filePath);
    }
    if (!engine.IsValid() || filePath.empty()) return false;
    if (targetPeaks < 200) targetPeaks = 200;
    if (targetPeaks > 50000) targetPeaks = 50000;

    const bool playbackStartWindowActive = IsPlaybackStartWorkGuardActive();
    const PlaybackStartWaveformWork work =
        isCurrentFile
            ? PlaybackStartWaveformWork::CurrentFileMainDetail
            : PlaybackStartWaveformWork::UnrelatedPrewarm;
    if (!ShouldRunWaveformWorkDuringPlaybackStart(
            work,
            playbackStartWindowActive,
            isCurrentFile,
            false)) {
        DIAG_SYSTEM_FMT("Waveform",
            "PlaybackStartBackgroundWorkDeferred: work=%s currentFile=%d targetPeaks=%d path=%s",
            PlaybackStartWaveformWorkName(work),
            isCurrentFile ? 1 : 0,
            targetPeaks,
            filePath.c_str());
        return false;
    }

    DIAG_SCOPED_DETAIL("Waveform", "MainDetailPrewarmDecode");
    return buildWaveform(filePath, engine, false, 0, outWaveform, targetPeaks);
}

// ITU-R BS.1770-4 K-weighting Filter Implementation
// Standards-based implementation; coefficient handling was cross-checked against
// Klangfreund LUFSMeter (MIT) while keeping the public code/comments anchored to BS.1770-4.

// Second-order IIR filter (biquad) for K-weighting
// Uses Direct Form I implementation for numerical stability
class SecondOrderIIRFilter {
public:
    SecondOrderIIRFilter() : x1(0), x2(0), y1(0), y2(0),
                             b0(1), b1(0), b2(0), a1(0), a2(0) {}
    
    // Set coefficients for specific sample rate
    // Based on ITU-R BS.1770-4 exact coefficients
    void setCoefficients(double _b0, double _b1, double _b2, double _a1, double _a2) {
        b0 = _b0; b1 = _b1; b2 = _b2; a1 = _a1; a2 = _a2;
    }
    
    void reset() { x1 = x2 = y1 = y2 = 0.0; }
    
    // Process one sample - Direct Form I
    double process(double x0) {
        double y0 = b0 * x0 + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
        x2 = x1; x1 = x0;
        y2 = y1; y1 = y0;
        return y0;
    }
    
private:
    double x1, x2;  // Input delay
    double y1, y2;  // Output delay
    double b0, b1, b2, a1, a2;  // Coefficients (a0 normalized to 1.0)
};

// K-weighting filter coefficients from ITU-R BS.1770-4
// These are the EXACT coefficients specified in the standard for 48kHz
// For other sample rates, we use bilinear transform from analog prototype
static void getKWeightingCoefficients(uint32_t sampleRate,
    double& pre_b0, double& pre_b1, double& pre_b2, double& pre_a1, double& pre_a2,
    double& rlb_b0, double& rlb_b1, double& rlb_b2, double& rlb_a1, double& rlb_a2)
{
    // ITU-R BS.1770-4 specifies exact coefficients for 48kHz
    // These are from Table 1 and Table 2 of the standard
    if (sampleRate == 48000) {
        // Stage 1: Pre-filter (High Shelf) - Table 1
        pre_b0 =  1.53512485958697;
        pre_b1 = -2.69169618940638;
        pre_b2 =  1.19839281085285;
        pre_a1 = -1.69065929318241;
        pre_a2 =  0.73248077421585;
        
        // Stage 2: RLB High-pass - Table 2
        rlb_b0 =  1.0;
        rlb_b1 = -2.0;
        rlb_b2 =  1.0;
        rlb_a1 = -1.99004745483398;
        rlb_a2 =  0.99007225036621;
    }
    else if (sampleRate == 44100) {
        // Coefficients for 44.1kHz (commonly used)
        pre_b0 =  1.5308412300498355;
        pre_b1 = -2.6509799951536985;
        pre_b2 =  1.1690790799210682;
        pre_a1 = -1.6636551132560204;
        pre_a2 =  0.7125954280732254;
        
        rlb_b0 =  1.0;
        rlb_b1 = -2.0;
        rlb_b2 =  1.0;
        rlb_a1 = -1.9891696736297957;
        rlb_a2 =  0.9891990357870394;
    }
    else if (sampleRate == 96000) {
        // Coefficients for 96kHz
        pre_b0 =  1.5409659406647672;
        pre_b1 = -2.7553213076350295;
        pre_b2 =  1.2384016524498783;
        pre_a1 = -1.7415885707967508;
        pre_a2 =  0.7651970344498649;
        
        rlb_b0 =  1.0;
        rlb_b1 = -2.0;
        rlb_b2 =  1.0;
        rlb_a1 = -1.9950144828456602;
        rlb_a2 =  0.9950194047498264;
    }
    else {
        // For other sample rates, calculate using bilinear transform
        // This matches the approach in LUFSMeter
        const double pi = 3.14159265358979323846;
        double fs = (double)sampleRate;
        
        // Stage 1: Pre-filter (High Shelf)
        // Analog prototype parameters from ITU-R BS.1770-4
        double db = 3.999843853973347;  // dB boost
        double f0 = 1681.974450955533;  // Center frequency
        double Q  = 0.7071752369554196; // Q factor
        double K  = tan(pi * f0 / fs);
        double Vh = pow(10.0, db / 20.0);
        double Vb = pow(Vh, 0.4996667741545416);
        double a0_ = 1.0 + K / Q + K * K;
        pre_b0 = (Vh + Vb * K / Q + K * K) / a0_;
        pre_b1 = 2.0 * (K * K - Vh) / a0_;
        pre_b2 = (Vh - Vb * K / Q + K * K) / a0_;
        pre_a1 = 2.0 * (K * K - 1.0) / a0_;
        pre_a2 = (1.0 - K / Q + K * K) / a0_;
        
        // Stage 2: RLB High-pass (2nd order Butterworth)
        double f0_hp = 38.13547087602444;  // Cutoff frequency
        double Q_hp  = 0.5003270373238773;
        double K_hp  = tan(pi * f0_hp / fs);
        double a0_hp = 1.0 + K_hp / Q_hp + K_hp * K_hp;
        rlb_b0 = 1.0;
        rlb_b1 = -2.0;
        rlb_b2 = 1.0;
        rlb_a1 = 2.0 * (K_hp * K_hp - 1.0) / a0_hp;
        rlb_a2 = (1.0 - K_hp / Q_hp + K_hp * K_hp) / a0_hp;
        // Normalize b coefficients
        double norm = a0_hp;
        rlb_b0 /= norm;
        rlb_b1 /= norm;
        rlb_b2 /= norm;
    }
}

// K-weighting filter for multi-channel audio
class KWeightingFilter {
public:
    void init(uint32_t sampleRate, uint32_t numChannels) {
        m_numChannels = numChannels;
        m_preFilters.resize(numChannels);
        m_rlbFilters.resize(numChannels);
        
        double pre_b0, pre_b1, pre_b2, pre_a1, pre_a2;
        double rlb_b0, rlb_b1, rlb_b2, rlb_a1, rlb_a2;
        getKWeightingCoefficients(sampleRate, 
            pre_b0, pre_b1, pre_b2, pre_a1, pre_a2,
            rlb_b0, rlb_b1, rlb_b2, rlb_a1, rlb_a2);
        
        for (uint32_t ch = 0; ch < numChannels; ch++) {
            m_preFilters[ch].setCoefficients(pre_b0, pre_b1, pre_b2, pre_a1, pre_a2);
            m_rlbFilters[ch].setCoefficients(rlb_b0, rlb_b1, rlb_b2, rlb_a1, rlb_a2);
        }
        reset();
    }
    
    void reset() {
        for (auto& f : m_preFilters) f.reset();
        for (auto& f : m_rlbFilters) f.reset();
    }
    
    // Process a single sample for a specific channel
    // Applies pre-filter then RLB filter (cascaded)
    double process(double sample, uint32_t channel) {
        if (channel >= m_numChannels) return sample;
        double y = m_preFilters[channel].process(sample);
        return m_rlbFilters[channel].process(y);
    }
    
private:
    uint32_t m_numChannels = 0;
    std::vector<SecondOrderIIRFilter> m_preFilters;
    std::vector<SecondOrderIIRFilter> m_rlbFilters;
};

bool AudioEngine::DecodeFileToInterleavedF32(const std::string& filePath,
                                             std::vector<float>& outSamples,
                                             uint64_t& outFrames,
                                             uint32_t& outChannels,
                                             uint32_t& outSampleRate,
                                             const std::atomic<bool>* cancelFlag,
                                             uint64_t maxFrames) {
    outSamples.clear();
    outFrames = 0; outChannels = 0; outSampleRate = 0;
    if (filePath.empty()) return false;

    Platform::AudioDataSource::Decoder decoder;
    const auto initResult = Platform::AudioDataSource::InitializeDecoderFromFile(
        decoder, filePath);
    if (!initResult.Succeeded()) return false;

    uint64_t totalFrames =
        Platform::AudioDataSource::QueryLength(
            Platform::AudioDataSource::BorrowDataSource(decoder)).frames;

    const auto decoderFormat = Platform::AudioDataSource::QueryDataFormat(
        Platform::AudioDataSource::BorrowDataSource(decoder));
    uint32_t channels = decoderFormat.channels;
    uint32_t sampleRate = decoderFormat.sampleRate;
    if (channels == 0) channels = 1;
    if (sampleRate == 0) sampleRate = 48000;

    static constexpr uint64_t kChunk = 8192;
    std::vector<float> buffer((size_t)kChunk * (size_t)channels);
    if (totalFrames > 0 &&
        totalFrames <
            static_cast<uint64_t>(std::numeric_limits<size_t>::max()) /
                static_cast<uint64_t>(channels)) {
        const uint64_t reserveFrames = std::min<uint64_t>(
            static_cast<uint64_t>(totalFrames), static_cast<uint64_t>(kChunk) * 256);
        outSamples.reserve((size_t)reserveFrames * (size_t)channels);
    }
    uint64_t offset = 0;
    while (true) {
        if (cancelFlag && cancelFlag->load(std::memory_order_acquire)) {
            Platform::AudioDataSource::UninitializeDecoder(decoder);
            outSamples.clear();
            outFrames = 0;
            return false;
        }
        const auto readResult = Platform::AudioDataSource::ReadPcmFrames(
            Platform::AudioDataSource::BorrowDataSource(decoder),
            buffer.data(),
            kChunk);
        uint64_t framesRead = readResult.framesRead;
        if (framesRead == 0) break;
        if (maxFrames > 0 && offset + framesRead > maxFrames) {
            Platform::AudioDataSource::UninitializeDecoder(decoder);
            outSamples.clear();
            outFrames = 0;
            return false;
        }
        const size_t samplesRead = (size_t)framesRead * (size_t)channels;
        outSamples.insert(outSamples.end(), buffer.begin(), buffer.begin() + samplesRead);
        offset += framesRead;
    }
    Platform::AudioDataSource::UninitializeDecoder(decoder);
    outFrames = offset;
    outChannels = channels;
    outSampleRate = sampleRate;
    return offset > 0;
}

bool AudioEngine::computeWaveformAndLoudnessFromSamples(const float* interleaved,
                                                        uint64_t totalFrames,
                                                        uint32_t channels,
                                                        uint32_t sampleRate,
                                                        int targetPeaks,
                                                        WaveformData& outWaveform,
                                                        const std::atomic<bool>* cancelFlag) {
    if (!interleaved || totalFrames == 0 || channels == 0 || targetPeaks <= 0) return false;

    KWeightingFilter kFilter;
    kFilter.init(sampleRate, channels);

    int samplesPerPeak = (int)(totalFrames / (uint64_t)targetPeaks);
    if (samplesPerPeak < 1) samplesPerPeak = 1;

    outWaveform.peaks.clear();
    outWaveform.rms.clear();
    outWaveform.samplesPerPeak = samplesPerPeak;
    outWaveform.isStereo = false;

    if (!kEnableLufsDetection) {
        float waveformPeak = 0.0f;
        double waveformSumSq = 0.0;
        uint64_t waveformFrameCount = 0;
        const uint64_t waveformFramesPerBlock = (uint64_t)samplesPerPeak;
        double globalSumSq = 0.0;
        uint64_t globalSampleCount = 0;

        for (uint64_t frame = 0; frame < totalFrames; ++frame) {
            if ((frame & 0x3fffULL) == 0 &&
                cancelFlag && cancelFlag->load(std::memory_order_acquire)) {
                outWaveform = {};
                return false;
            }
            for (uint32_t ch = 0; ch < channels; ++ch) {
                float sample = interleaved[frame * channels + ch];
                double s = (double)sample;
                waveformPeak = std::max(waveformPeak, std::abs(sample));
                waveformSumSq += s * s;
                globalSumSq += s * s;
                globalSampleCount++;
            }
            waveformFrameCount++;
            if (waveformFrameCount >= waveformFramesPerBlock) {
                float rmsDisplay = (float)std::sqrt(waveformSumSq / (double)(waveformFrameCount * channels));
                outWaveform.peaks.push_back(waveformPeak);
                outWaveform.rms.push_back(rmsDisplay);
                waveformPeak = 0.0f;
                waveformSumSq = 0.0;
                waveformFrameCount = 0;
            }
        }
        if (waveformFrameCount > 0) {
            float rmsDisplay = (float)std::sqrt(waveformSumSq / (double)(waveformFrameCount * channels));
            outWaveform.peaks.push_back(waveformPeak);
            outWaveform.rms.push_back(rmsDisplay);
        }
        if (globalSampleCount > 0) {
            double rmsLinear = std::sqrt(globalSumSq / (double)globalSampleCount);
            outWaveform.trueRmsDbfs = (rmsLinear > 1e-10) ? (float)(20.0 * std::log10(rmsLinear)) : -100.0f;
            outWaveform.hasTrueRms = true;
        }
        outWaveform.integratedLUFS = -70.0f;
        outWaveform.momentaryLUFS = -70.0f;
        outWaveform.shortTermLUFS = -70.0f;
        outWaveform.truePeakDBTP = -70.0f;
        outWaveform.hasLoudnessData = false;
        outWaveform.isReady = true;
        return true;
    }

    // ITU-R BS.1770-4 Integrated Loudness path (same math as the streaming path)

    const int subBlockFrames = std::max(1, (int)(sampleRate / 10));
    struct LUFSSubBlock {
        std::vector<double> channelPower;
        int frameCount = 0;
        LUFSSubBlock() {}
        LUFSSubBlock(uint32_t ch) : channelPower(ch, 0.0), frameCount(0) {}
    };
    std::vector<LUFSSubBlock> lufsSubBlocks;
    LUFSSubBlock currentSub(channels);

    std::vector<double> channelWeights(channels, 1.0);
    if (channels == 6) {
        channelWeights[3] = 0.0;
        channelWeights[4] = 1.41253754462275;
        channelWeights[5] = 1.41253754462275;
    }

    float waveformPeak = 0.0f;
    double waveformSumSq = 0.0;
    uint64_t waveformFrameCount = 0;
    const uint64_t waveformFramesPerBlock = (uint64_t)samplesPerPeak;
    double globalSumSq = 0.0;
    uint64_t globalSampleCount = 0;
    float globalTruePeak = 0.0f;
    std::vector<std::array<float, 4>> tpHistory(channels);
    for (auto& h : tpHistory) h.fill(0.0f);

    for (uint64_t frame = 0; frame < totalFrames; ++frame) {
        if ((frame & 0x3fffULL) == 0 &&
            cancelFlag && cancelFlag->load(std::memory_order_acquire)) {
            outWaveform = {};
            return false;
        }
        for (uint32_t ch = 0; ch < channels; ++ch) {
            float sample = interleaved[frame * channels + ch];
            waveformPeak = std::max(waveformPeak, std::abs(sample));
            { double s = (double)sample; globalSumSq += s * s; globalSampleCount++; }
            auto& h = tpHistory[ch];
            h[0] = h[1]; h[1] = h[2]; h[2] = h[3]; h[3] = sample;
            globalTruePeak = std::max(globalTruePeak, std::abs(sample));
            if (frame >= 3) {
                for (int p = 1; p <= 3; p++) {
                    float t = p * 0.25f, t2 = t * t, t3 = t2 * t;
                    float a0 = -0.5f*h[0]+1.5f*h[1]-1.5f*h[2]+0.5f*h[3];
                    float a1 =  h[0]-2.5f*h[1]+2.0f*h[2]-0.5f*h[3];
                    float a2 = -0.5f*h[0]+0.5f*h[2];
                    float a3 =  h[1];
                    globalTruePeak = std::max(globalTruePeak, std::abs(a0*t3+a1*t2+a2*t+a3));
                }
            }
            double filteredSample = kFilter.process((double)sample, ch);
            waveformSumSq += channelWeights[ch] * filteredSample * filteredSample;
            currentSub.channelPower[ch] += filteredSample * filteredSample;
        }
        currentSub.frameCount++;
        if (currentSub.frameCount >= subBlockFrames) {
            lufsSubBlocks.push_back(currentSub);
            currentSub = LUFSSubBlock(channels);
        }
        waveformFrameCount++;
        if (waveformFrameCount >= waveformFramesPerBlock) {
            float rmsDisplay = (float)std::sqrt(waveformSumSq / (double)waveformFrameCount);
            outWaveform.peaks.push_back(waveformPeak);
            outWaveform.rms.push_back(rmsDisplay);
            waveformPeak = 0.0f;
            waveformSumSq = 0.0;
            waveformFrameCount = 0;
        }
    }
    if (waveformFrameCount > 0) {
        float rmsDisplay = (float)std::sqrt(waveformSumSq / (double)waveformFrameCount);
        outWaveform.peaks.push_back(waveformPeak);
        outWaveform.rms.push_back(rmsDisplay);
    }
    if (globalSampleCount > 0) {
        double rmsLinear = std::sqrt(globalSumSq / (double)globalSampleCount);
        outWaveform.trueRmsDbfs = (rmsLinear > 1e-10) ? (float)(20.0 * std::log10(rmsLinear)) : -100.0f;
        outWaveform.hasTrueRms = true;
    }
    if (currentSub.frameCount >= subBlockFrames) lufsSubBlocks.push_back(currentSub);

    // BS.1770-4 gating (identical to streaming path)
    std::vector<double> blockPowers;
    if (lufsSubBlocks.size() >= 4) {
        for (size_t i = 0; i + 3 < lufsSubBlocks.size(); i++) {
            double totalWeightedPower = 0.0;
            int totalFramesInBlock = 0;
            for (int k = 0; k < 4; k++) {
                const auto& sub = lufsSubBlocks[i + k];
                for (uint32_t ch = 0; ch < channels; ch++)
                    totalWeightedPower += channelWeights[ch] * sub.channelPower[ch];
                totalFramesInBlock += sub.frameCount;
            }
            blockPowers.push_back(totalWeightedPower / (double)totalFramesInBlock);
        }
    } else if (!lufsSubBlocks.empty()) {
        double totalWeightedPower = 0.0;
        int totalFramesInBlock = 0;
        for (const auto& sub : lufsSubBlocks) {
            for (uint32_t ch = 0; ch < channels; ch++)
                totalWeightedPower += channelWeights[ch] * sub.channelPower[ch];
            totalFramesInBlock += sub.frameCount;
        }
        if (totalFramesInBlock > 0)
            blockPowers.push_back(totalWeightedPower / (double)totalFramesInBlock);
    }

    const double absGateThreshold = std::pow(10.0, (-70.0 + 0.691) / 10.0);
    double sumAbsGated = 0.0; int countAbsGated = 0;
    for (double p : blockPowers) { if (p > absGateThreshold) { sumAbsGated += p; countAbsGated++; } }

    if (countAbsGated == 0) {
        outWaveform.integratedLUFS = -70.0f;
    } else {
        double absGatedMean = sumAbsGated / countAbsGated;
        double absGatedLoudness = -0.691 + 10.0 * std::log10(absGatedMean);
        double relGateThreshold = std::pow(10.0, (absGatedLoudness - 10.0 + 0.691) / 10.0);
        double sumRelGated = 0.0; int countRelGated = 0;
        for (double p : blockPowers) { if (p > relGateThreshold) { sumRelGated += p; countRelGated++; } }
        outWaveform.integratedLUFS = (countRelGated == 0) ? -70.0f
            : (float)(-0.691 + 10.0 * std::log10(sumRelGated / countRelGated));
    }
    outWaveform.integratedLUFS = std::clamp(outWaveform.integratedLUFS, -70.0f, 0.0f);

    double maxMomentaryPower = 0.0;
    for (double p : blockPowers) maxMomentaryPower = std::max(maxMomentaryPower, p);
    outWaveform.momentaryLUFS = (maxMomentaryPower > 1e-10)
        ? std::clamp((float)(-0.691 + 10.0 * std::log10(maxMomentaryPower)), -70.0f, 0.0f) : -70.0f;

    double maxShortTermPower = 0.0;
    if (lufsSubBlocks.size() >= 30) {
        for (size_t i = 0; i + 29 < lufsSubBlocks.size(); i++) {
            double totalPower = 0.0; int totalF = 0;
            for (int k = 0; k < 30; k++) {
                const auto& sub = lufsSubBlocks[i + k];
                for (uint32_t ch = 0; ch < channels; ch++)
                    totalPower += channelWeights[ch] * sub.channelPower[ch];
                totalF += sub.frameCount;
            }
            maxShortTermPower = std::max(maxShortTermPower, totalPower / (double)totalF);
        }
    } else {
        maxShortTermPower = maxMomentaryPower;
    }
    outWaveform.shortTermLUFS = (maxShortTermPower > 1e-10)
        ? std::clamp((float)(-0.691 + 10.0 * std::log10(maxShortTermPower)), -70.0f, 0.0f) : -70.0f;

    outWaveform.truePeakDBTP = std::clamp(
        (globalTruePeak > 1e-10f) ? 20.0f * std::log10(globalTruePeak) : -70.0f, -70.0f, 0.0f);
    outWaveform.hasLoudnessData = true;
    outWaveform.isReady = true;
    return true;
}

bool AudioEngine::generateWaveformForFile(const std::string& filePath,
                                          WaveformData& outWaveform,
                                          int targetPeaks,
                                          const std::atomic<bool>* cancelFlag,
                                          bool collectPeaks) {
    if (filePath.empty() || targetPeaks <= 0) return false;

    Platform::AudioDataSource::Decoder decoder;
    const auto initResult = Platform::AudioDataSource::InitializeDecoderFromFile(
        decoder, filePath);
    if (!initResult.Succeeded()) {
        DIAG_ACTION_FMT("Waveform",
                        "generateWaveformForFile DecoderInitFailed: path=%s, ma_result=%d",
                        filePath.c_str(),
                        initResult.backendCode);
        return false;
    }

    uint64_t declaredTotalFrames =
        Platform::AudioDataSource::QueryLength(
            Platform::AudioDataSource::BorrowDataSource(decoder)).frames;
    const auto decoderFormat = Platform::AudioDataSource::QueryDataFormat(
        Platform::AudioDataSource::BorrowDataSource(decoder));
    uint32_t channels = decoderFormat.channels;
    uint32_t sampleRate = decoderFormat.sampleRate;
    if (channels == 0) channels = 1;
    if (sampleRate == 0) sampleRate = 48000;

    const bool streamed = generateWaveformFromOpenDecoder(
        decoder, channels, sampleRate, declaredTotalFrames, targetPeaks, outWaveform, cancelFlag,
        collectPeaks);
    Platform::AudioDataSource::UninitializeDecoder(decoder);
    return streamed;
}

bool AudioEngine::generateWaveformFromOpenDecoder(
    Platform::AudioDataSource::Decoder& decoder,
    uint32_t channels,
    uint32_t sampleRate,
    uint64_t declaredTotalFrames,
    int targetPeaks,
    WaveformData& outWaveform,
    const std::atomic<bool>* cancelFlag,
    bool collectPeaks) {
    if (channels == 0) channels = 1;
    if (sampleRate == 0) sampleRate = 48000;
    if (targetPeaks <= 0) return false;

    KWeightingFilter kFilter;
    kFilter.init(sampleRate, channels);

    static constexpr uint64_t kUnknownLengthFramesPerPeak = 8192;
    int samplesPerPeak = declaredTotalFrames == 0
        ? (int)kUnknownLengthFramesPerPeak
        : (int)(declaredTotalFrames / (uint64_t)targetPeaks);
    if (samplesPerPeak < 1) samplesPerPeak = 1;

    outWaveform = {};
    outWaveform.samplesPerPeak = samplesPerPeak;
    outWaveform.isStereo = false;
    outWaveform.numChannels = 1;

    const uint64_t waveformFramesPerBlock = (uint64_t)samplesPerPeak;
    float waveformPeak = 0.0f;
    double waveformSumSq = 0.0;
    uint64_t waveformFrameCount = 0;
    double globalSumSq = 0.0;
    uint64_t globalSampleCount = 0;

    auto flushWaveformBlock = [&]() {
        if (waveformFrameCount == 0) return;
        if (collectPeaks) {
            const double denom = kEnableLufsDetection
                ? (double)waveformFrameCount
                : (double)(waveformFrameCount * (uint64_t)channels);
            outWaveform.peaks.push_back(waveformPeak);
            outWaveform.rms.push_back((float)std::sqrt(waveformSumSq / std::max(1.0, denom)));
        }
        waveformPeak = 0.0f;
        waveformSumSq = 0.0;
        waveformFrameCount = 0;
    };

    const int subBlockFrames = std::max(1, (int)(sampleRate / 10));
    struct LUFSSubBlock {
        std::vector<double> channelPower;
        int frameCount = 0;
        LUFSSubBlock() {}
        LUFSSubBlock(uint32_t ch) : channelPower(ch, 0.0), frameCount(0) {}
    };
    std::vector<LUFSSubBlock> lufsSubBlocks;
    LUFSSubBlock currentSub(channels);

    std::vector<double> channelWeights(channels, 1.0);
    if (channels == 6) {
        channelWeights[3] = 0.0;
        channelWeights[4] = 1.41253754462275;
        channelWeights[5] = 1.41253754462275;
    }

    float globalTruePeak = 0.0f;
    std::vector<std::array<float, 4>> tpHistory(channels);
    for (auto& h : tpHistory) h.fill(0.0f);

    static constexpr uint64_t kWaveChunkFrames = 8192;
    std::vector<float> buffer((size_t)kWaveChunkFrames * (size_t)channels);
    uint64_t totalFramesRead = 0;
    bool cancelled = false;

    while (true) {
        if (cancelFlag && cancelFlag->load(std::memory_order_acquire)) {
            cancelled = true;
            break;
        }
        const auto readResult = Platform::AudioDataSource::ReadPcmFrames(
            Platform::AudioDataSource::BorrowDataSource(decoder),
            buffer.data(),
            kWaveChunkFrames);
        uint64_t framesRead = readResult.framesRead;
        if (framesRead == 0) break;

        for (uint64_t frame = 0; frame < framesRead; ++frame) {
            for (uint32_t ch = 0; ch < channels; ++ch) {
                const size_t idx = (size_t)frame * (size_t)channels + (size_t)ch;
                const float sample = buffer[idx];
                const double s = (double)sample;

                waveformPeak = std::max(waveformPeak, std::abs(sample));
                globalSumSq += s * s;
                globalSampleCount++;

                if (kEnableLufsDetection) {
                    auto& h = tpHistory[ch];
                    h[0] = h[1];
                    h[1] = h[2];
                    h[2] = h[3];
                    h[3] = sample;
                    globalTruePeak = std::max(globalTruePeak, std::abs(sample));
                    if (totalFramesRead + frame >= 3) {
                        for (int p = 1; p <= 3; ++p) {
                            const float t = p * 0.25f;
                            const float t2 = t * t;
                            const float t3 = t2 * t;
                            const float a0 = -0.5f * h[0] + 1.5f * h[1] - 1.5f * h[2] + 0.5f * h[3];
                            const float a1 = h[0] - 2.5f * h[1] + 2.0f * h[2] - 0.5f * h[3];
                            const float a2 = -0.5f * h[0] + 0.5f * h[2];
                            const float a3 = h[1];
                            globalTruePeak = std::max(globalTruePeak, std::abs(a0 * t3 + a1 * t2 + a2 * t + a3));
                        }
                    }

                    const double filteredSample = kFilter.process(s, ch);
                    waveformSumSq += channelWeights[ch] * filteredSample * filteredSample;
                    currentSub.channelPower[ch] += filteredSample * filteredSample;
                } else {
                    waveformSumSq += s * s;
                }
            }

            if (kEnableLufsDetection) {
                currentSub.frameCount++;
                if (currentSub.frameCount >= subBlockFrames) {
                    lufsSubBlocks.push_back(currentSub);
                    currentSub = LUFSSubBlock(channels);
                }
            }

            waveformFrameCount++;
            if (waveformFrameCount >= waveformFramesPerBlock) {
                flushWaveformBlock();
            }
        }
        totalFramesRead += framesRead;
    }

    if (cancelled || totalFramesRead == 0) {
        outWaveform = {};
        return false;
    }

    flushWaveformBlock();
    if (globalSampleCount > 0) {
        const double rmsLinear = std::sqrt(globalSumSq / (double)globalSampleCount);
        outWaveform.trueRmsDbfs = (rmsLinear > 1e-10)
            ? (float)(20.0 * std::log10(rmsLinear))
            : -100.0f;
        outWaveform.hasTrueRms = true;
    }

    if (!kEnableLufsDetection) {
        outWaveform.integratedLUFS = -70.0f;
        outWaveform.momentaryLUFS = -70.0f;
        outWaveform.shortTermLUFS = -70.0f;
        outWaveform.truePeakDBTP = -70.0f;
        outWaveform.hasLoudnessData = false;
        outWaveform.isReady = true;
        return true;
    }

    std::vector<double> blockPowers;
    if (lufsSubBlocks.size() >= 4) {
        for (size_t i = 0; i + 3 < lufsSubBlocks.size(); ++i) {
            double totalWeightedPower = 0.0;
            int totalFramesInBlock = 0;
            for (int k = 0; k < 4; ++k) {
                const auto& sub = lufsSubBlocks[i + k];
                for (uint32_t ch = 0; ch < channels; ++ch) {
                    totalWeightedPower += channelWeights[ch] * sub.channelPower[ch];
                }
                totalFramesInBlock += sub.frameCount;
            }
            if (totalFramesInBlock > 0) {
                blockPowers.push_back(totalWeightedPower / (double)totalFramesInBlock);
            }
        }
    } else if (!lufsSubBlocks.empty()) {
        double totalWeightedPower = 0.0;
        int totalFramesInBlock = 0;
        for (const auto& sub : lufsSubBlocks) {
            for (uint32_t ch = 0; ch < channels; ++ch) {
                totalWeightedPower += channelWeights[ch] * sub.channelPower[ch];
            }
            totalFramesInBlock += sub.frameCount;
        }
        if (totalFramesInBlock > 0) {
            blockPowers.push_back(totalWeightedPower / (double)totalFramesInBlock);
        }
    }

    const double absGateThreshold = std::pow(10.0, (-70.0 + 0.691) / 10.0);
    double sumAbsGated = 0.0;
    int countAbsGated = 0;
    for (double p : blockPowers) {
        if (p > absGateThreshold) {
            sumAbsGated += p;
            countAbsGated++;
        }
    }

    if (countAbsGated == 0) {
        outWaveform.integratedLUFS = -70.0f;
    } else {
        const double absGatedMean = sumAbsGated / countAbsGated;
        const double absGatedLoudness = -0.691 + 10.0 * std::log10(absGatedMean);
        const double relGateThreshold = std::pow(10.0, (absGatedLoudness - 10.0 + 0.691) / 10.0);
        double sumRelGated = 0.0;
        int countRelGated = 0;
        for (double p : blockPowers) {
            if (p > relGateThreshold) {
                sumRelGated += p;
                countRelGated++;
            }
        }
        outWaveform.integratedLUFS = (countRelGated == 0)
            ? -70.0f
            : (float)(-0.691 + 10.0 * std::log10(sumRelGated / countRelGated));
    }
    outWaveform.integratedLUFS = std::clamp(outWaveform.integratedLUFS, -70.0f, 0.0f);

    double maxMomentaryPower = 0.0;
    for (double p : blockPowers) maxMomentaryPower = std::max(maxMomentaryPower, p);
    outWaveform.momentaryLUFS = (maxMomentaryPower > 1e-10)
        ? std::clamp((float)(-0.691 + 10.0 * std::log10(maxMomentaryPower)), -70.0f, 0.0f)
        : -70.0f;

    double maxShortTermPower = 0.0;
    if (lufsSubBlocks.size() >= 30) {
        for (size_t i = 0; i + 29 < lufsSubBlocks.size(); ++i) {
            double totalPower = 0.0;
            int totalF = 0;
            for (int k = 0; k < 30; ++k) {
                const auto& sub = lufsSubBlocks[i + k];
                for (uint32_t ch = 0; ch < channels; ++ch) {
                    totalPower += channelWeights[ch] * sub.channelPower[ch];
                }
                totalF += sub.frameCount;
            }
            if (totalF > 0) {
                maxShortTermPower = std::max(maxShortTermPower, totalPower / (double)totalF);
            }
        }
    } else {
        maxShortTermPower = maxMomentaryPower;
    }
    outWaveform.shortTermLUFS = (maxShortTermPower > 1e-10)
        ? std::clamp((float)(-0.691 + 10.0 * std::log10(maxShortTermPower)), -70.0f, 0.0f)
        : -70.0f;

    outWaveform.truePeakDBTP = std::clamp(
        (globalTruePeak > 1e-10f) ? 20.0f * std::log10(globalTruePeak) : -70.0f,
        -70.0f,
        0.0f);
    outWaveform.hasLoudnessData = true;
    outWaveform.isReady = true;
    return true;
}

// Seek-based waveform generation samples the decoder without a playback engine.
// 在均匀间隔位置 seek 并只读取少量采样（512 帧），比全量读取快 100~280 倍
// 适用于 mini 缩略图（100 peaks）等场景

bool AudioEngine::generateQuickWaveformForFile(const std::string& filePath, WaveformData& outWaveform, int targetPeaks, bool perChannel) {
    if (filePath.empty() || targetPeaks <= 0) return false;

    Platform::AudioDataSource::Decoder decoder;
    // 使用原生采样率和通道数，不做重采样
    const auto initResult = Platform::AudioDataSource::InitializeDecoderFromFile(
        decoder, filePath);
    if (!initResult.Succeeded()) {
        DIAG_ACTION_FMT("Waveform", "generateQuickWaveformForFile DecoderInitFailed: path=%s, ma_result=%d", filePath.c_str(), initResult.backendCode);
        return false;
    }

    const auto decoderFormat = Platform::AudioDataSource::QueryDataFormat(
        Platform::AudioDataSource::BorrowDataSource(decoder));
    const uint32_t actualChannels = decoderFormat.channels;
    if (actualChannels == 0) { Platform::AudioDataSource::UninitializeDecoder(decoder); return false; }

    uint64_t totalFrames =
        Platform::AudioDataSource::QueryLength(
            Platform::AudioDataSource::BorrowDataSource(decoder)).frames;
    if (totalFrames == 0) {
        Platform::AudioDataSource::UninitializeDecoder(decoder);
        return false;
    }

    const int blockSize = 512; // 每个采样点只读 512 帧
    uint64_t segmentSize = totalFrames / (uint64_t)targetPeaks;
    if (segmentSize < 1) segmentSize = 1;

    // perChannel=true: 输出每通道独立 peaks（用于主波形 detail 缓存）
    // perChannel=false: 混合为单通道（用于 mini 缩略图）
    const uint32_t outChannels = perChannel ? actualChannels : 1;

    outWaveform.peaks.clear();
    outWaveform.rms.clear();
    outWaveform.peaks.reserve((size_t)targetPeaks * outChannels);
    outWaveform.samplesPerPeak = (int)segmentSize;
    outWaveform.numChannels = (int)outChannels;
    outWaveform.isStereo = (outChannels == 2);
    outWaveform.hasLoudnessData = false;

    std::vector<float> buffer((size_t)blockSize * (size_t)actualChannels);

    for (int seg = 0; seg < targetPeaks; seg++) {
        // Seek 到段中心位置
        uint64_t seekCenter = (uint64_t)seg * segmentSize + segmentSize / 2;
        if (seekCenter + blockSize > totalFrames)
            seekCenter = (totalFrames > (uint64_t)blockSize) ? totalFrames - blockSize : 0;

        if (!Platform::AudioDataSource::SeekPcmFrame(
                Platform::AudioDataSource::BorrowDataSource(decoder),
                seekCenter).Succeeded()) {
            for (uint32_t ch = 0; ch < outChannels; ch++)
                outWaveform.peaks.push_back(0.0f);
            continue;
        }

        const auto readResult = Platform::AudioDataSource::ReadPcmFrames(
            Platform::AudioDataSource::BorrowDataSource(decoder),
            buffer.data(),
            blockSize);
        uint64_t framesRead = readResult.framesRead;
        if (framesRead == 0) {
            for (uint32_t ch = 0; ch < outChannels; ch++)
                outWaveform.peaks.push_back(0.0f);
            continue;
        }

        if (perChannel) {
            // 每通道独立 peak（与 buildQuickWaveform 一致）
            for (uint32_t ch = 0; ch < actualChannels; ch++) {
                float peak = 0.0f;
                for (uint64_t i = 0; i < framesRead; i++) {
                    float a = std::abs(buffer[(size_t)(i * actualChannels + ch)]);
                    if (a > peak) peak = a;
                }
                outWaveform.peaks.push_back(peak);
            }
        } else {
            // 所有通道取最大绝对值（混合为单通道 peak）
            float peak = 0.0f;
            for (uint64_t i = 0; i < framesRead * actualChannels; i++) {
                float a = std::abs(buffer[(size_t)i]);
                if (a > peak) peak = a;
            }
            outWaveform.peaks.push_back(peak);
        }
    }

    Platform::AudioDataSource::UninitializeDecoder(decoder);
    outWaveform.isReady = true;
    return true;
}

// 全量解码到内存（统一单声道 44.1k），用于波形金字塔 generateWaveformMipmap

bool AudioEngine::decodeFullFileToMemory(const std::string& filePath, std::vector<float>& outSamples, uint32_t& outSampleRate) {
    outSamples.clear();
    outSampleRate = 0;
    if (filePath.empty()) return false;

    Platform::AudioDataSource::Decoder decoder;
    // 统一转为单声道 44.1k 进行分析，便于金字塔层级一致
    const auto initResult = Platform::AudioDataSource::InitializeDecoderFromFileWithFormat(
        decoder, filePath, 1, 44100);
    if (!initResult.Succeeded()) return false;

    outSampleRate = Platform::AudioDataSource::QueryDataFormat(
        Platform::AudioDataSource::BorrowDataSource(decoder)).sampleRate;
    if (outSampleRate == 0) outSampleRate = 44100;

    uint64_t totalFrames =
        Platform::AudioDataSource::QueryLength(
            Platform::AudioDataSource::BorrowDataSource(decoder)).frames;
    if (totalFrames == 0) {
        Platform::AudioDataSource::UninitializeDecoder(decoder);
        return false;
    }

    // 预留内存，单次读取，避免频繁分配
    outSamples.resize((size_t)totalFrames);
    const auto fullReadResult = Platform::AudioDataSource::ReadPcmFrames(
        Platform::AudioDataSource::BorrowDataSource(decoder),
        outSamples.data(),
        totalFrames);
    uint64_t framesRead = fullReadResult.framesRead;

    Platform::AudioDataSource::UninitializeDecoder(decoder);
    return framesRead > 0;
}

// Output device enumeration & switching

std::vector<OutputDeviceInfo> AudioEngine::enumerateOutputDevices() {
    std::vector<OutputDeviceInfo> result;
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_engine) return result;

    const auto devices = Platform::AudioPlayback::EnumerateOutputDevices(
        m_engine->Borrow());
    result.reserve(devices.size());
    for (const auto& device : devices) {
        OutputDeviceInfo info;
        info.name = device.name;
        info.isDefault = device.isDefault;
        info.deviceIdBlob = device.deviceIdBlob;
        result.push_back(std::move(info));
    }

    m_cachedDevices = result;
    return result;
}

bool AudioEngine::switchOutputDevice(int deviceIndex) {
    // Snapshot current playback state before destroying the engine
    std::string savedFile;
    float savedPos = 0.0f;
    bool savedPlaying = false;
    float savedVol = m_volume;
    float savedPan = m_pan;
    float savedPitch = m_pitch;
    bool savedMono = m_forceMono;
    int savedSolo = m_channelSolo;
    bool savedReverse = m_reverse;
    float savedInPt = m_inPoint;
    float savedOutPt = m_outPoint;
    auto savedSound = Platform::AudioPlayback::BorrowSound(nullptr);
    uint64_t savedTotalFrames = 0;

    DIAG_ACTION_FMT("Audio", "SwitchOutputDeviceBegin: requestedIndex=%d", deviceIndex);

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_isLoaded) {
            savedFile = m_fileInfo.filePath;
            savedSound = m_sound->Borrow();
            savedTotalFrames = m_fileInfo.totalFrames;
        }
    }

    // Avoid re-locking m_mutex through getters while switching devices.
    if (savedSound.IsValid()) {
        savedPlaying = Platform::AudioPlayback::IsPlaying(savedSound);
        if (savedTotalFrames > 0) {
            uint64_t cursor = 0;
            cursor = Platform::AudioPlayback::QueryCursor(savedSound);
            savedPos = (float)cursor / (float)savedTotalFrames;
        }
    }

    // Shutdown fully (stops workers, uninits engine)
    shutdown();

    // Reinitialize with the selected device
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_engine = std::make_unique<Platform::AudioPlayback::Engine>();

        Platform::AudioPlayback::EngineInitOptions engineOptions;
        engineOptions.channels = 2;
        engineOptions.sampleRate = 48000;
        if (deviceIndex >= 0 && deviceIndex < (int)m_cachedDevices.size()) {
            engineOptions.playbackDeviceIdBlob = m_cachedDevices[deviceIndex].deviceIdBlob;
        }

        auto engineInitResult = m_engine->Initialize(engineOptions);
        if (!engineInitResult.Succeeded()) {
            // Fallback: try default device
            m_engine = std::make_unique<Platform::AudioPlayback::Engine>();
            engineOptions.playbackDeviceIdBlob.clear();
            engineInitResult = m_engine->Initialize(engineOptions);
            if (!engineInitResult.Succeeded()) {
                m_engine = nullptr;
                m_isInitialized = false;
                m_outputDeviceIndex = -1;
                m_outputDeviceIdBlob.clear();
                m_outputDeviceName = "Default";
                return false;
            }
            m_outputDeviceIndex = -1;
            m_outputDeviceIdBlob.clear();
            m_outputDeviceName = "Default";
            m_isInitialized = true;
        } else {
            m_outputDeviceIndex = deviceIndex;
            m_outputDeviceIdBlob = (deviceIndex >= 0 && deviceIndex < (int)m_cachedDevices.size())
                ? m_cachedDevices[deviceIndex].deviceIdBlob : std::vector<uint8_t>{};
            m_outputDeviceName = (deviceIndex >= 0 && deviceIndex < (int)m_cachedDevices.size())
                ? m_cachedDevices[deviceIndex].name : "Default";
            m_isInitialized = true;
        }
        ApplyHarnessAudioMuteIfNeeded(m_engine->Borrow());
    }

    // Restore playback state
    m_volume = savedVol;
    m_pan = savedPan;
    m_pitch = savedPitch;
    m_forceMono = savedMono;
    m_channelSolo = savedSolo;
    m_reverse = savedReverse;
    m_inPoint = savedInPt;
    m_outPoint = savedOutPt;

    if (!savedFile.empty()) {
        if (loadFile(savedFile)) {
            seekToPosition(savedPos);
            if (savedPlaying) play();
        }
    }

    DIAG_ACTION_FMT("Audio", "SwitchOutputDeviceEnd: activeIndex=%d restoredFile=%d restoredPlaying=%d",
        m_outputDeviceIndex, savedFile.empty() ? 0 : 1, savedPlaying ? 1 : 0);

    return true;
}

// F-019-channel: Single-channel extraction export
// Decodes inputPath with its native channel layout, extracts channelIndex (0-based),
// and writes a mono 24-bit WAV to outputPath.
bool AudioEngine::exportSingleChannel(const std::string& inputPath,
                                       int channelIndex,
                                       const std::string& outputPath,
                                       std::string* outError)
{
    auto setErr = [&](const std::string& msg) {
        if (outError) *outError = msg;
    };

    // 1. Probe the file to discover its native channel count and sample rate
    Platform::AudioDataSource::Decoder probeDecoder;
    const auto probeResult = Platform::AudioDataSource::InitializeDecoderFromFile(
        probeDecoder, inputPath);
    if (!probeResult.Succeeded()) {
        setErr("无法打开文件: " + inputPath);
        return false;
    }
    const auto probeFormat = Platform::AudioDataSource::QueryDataFormat(
        Platform::AudioDataSource::BorrowDataSource(probeDecoder));
    const uint32_t nativeChannels = probeFormat.channels;
    const uint32_t sampleRate     = probeFormat.sampleRate;
    Platform::AudioDataSource::UninitializeDecoder(probeDecoder);

#ifdef _WIN32
    const std::wstring wInput = Utf8ToWide(inputPath);
#endif

    if (nativeChannels < 2) {
        setErr("文件为单声道，无需提取声道");
        return false;
    }
    if (channelIndex < 0 || channelIndex >= (int)nativeChannels) {
        setErr("声道索引超出范围 (文件有 " + std::to_string(nativeChannels) + " 个声道)");
        return false;
    }

    // 2. Open decoder requesting native channel layout (f32 interleaved)
    Platform::AudioDataSource::Decoder decoder;
    const auto decoderResult = Platform::AudioDataSource::InitializeDecoderFromFileWithFormat(
        decoder, inputPath, nativeChannels, sampleRate);
    if (!decoderResult.Succeeded()) {
        setErr("解码器初始化失败");
        return false;
    }

    // 3. Open encoder — mono 24-bit WAV
    Platform::AudioFile::S24WavWriter encoder;
    if (!encoder.Open(outputPath, static_cast<int>(sampleRate), 1)) {
        Platform::AudioDataSource::UninitializeDecoder(decoder);
        setErr("输出文件创建失败: " + outputPath);
        return false;
    }

    // 4. Decode, extract channel, convert f32→s24, write
    const int kBlockFrames = 4096;
    std::vector<float> inBuf(kBlockFrames * nativeChannels);
    std::vector<uint8_t> s24Buf(kBlockFrames * 3); // mono s24: 3 bytes per frame

    bool ok = true;
    while (true) {
        const auto readResult = Platform::AudioDataSource::ReadPcmFrames(
            Platform::AudioDataSource::BorrowDataSource(decoder),
            inBuf.data(),
            kBlockFrames);
        uint64_t framesRead = readResult.framesRead;
        int res = static_cast<int>(readResult.backendCode);
        if (framesRead == 0) break;

        // Extract channelIndex from interleaved f32 buffer, convert to s24
        for (uint64_t f = 0; f < framesRead; ++f) {
            float sample = inBuf[f * nativeChannels + channelIndex];
            // Clamp and convert f32 → s24
            if (sample >  1.0f) sample =  1.0f;
            if (sample < -1.0f) sample = -1.0f;
            int32_t s32 = (int32_t)(sample * 8388607.0f); // 2^23 - 1
            uint8_t* dst = &s24Buf[f * 3];
            dst[0] = (uint8_t)(s32 & 0xFF);
            dst[1] = (uint8_t)((s32 >> 8) & 0xFF);
            dst[2] = (uint8_t)((s32 >> 16) & 0xFF);
        }

        uint64_t written = 0;
        if (!encoder.Write(s24Buf.data(), framesRead, &written)
            || written != framesRead) {
            ok = false;
            break;
        }

        if (res == MA_AT_END) break;
    }

    encoder.Close();
    Platform::AudioDataSource::UninitializeDecoder(decoder);

    if (!ok) {
        setErr("写入过程中发生错误");
        return false;
    }
    return true;
}
