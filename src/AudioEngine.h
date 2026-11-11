#pragma once

#include <algorithm>
#include <string>
#include <vector>
#include <functional>
#include <mutex>
#include <memory>
#include <atomic>
#include <condition_variable>
#include <thread>
#include <queue>
#include <cstdint>

#include "CenterSenseDsp.h"
#include "platform/audio_data_source.h"
#include "platform/audio_playback.h"

// Pitch control mode
enum class PitchMode {
    Semitone = 0,  // Ratio-based pitch shift via miniaudio resampling (semitones, ±48 st)
    FreqShift = 1  // Linear frequency shift in Hz via SSB + Hilbert (±5000 Hz, changes harmonic structure)
};

using MagicEQMode = CenterSenseDsp::MagicEQMode;
using MagicEQSettings = CenterSenseDsp::MagicEQSettings;

// A hidden transport effect retains its requested parameters but contributes
// nothing to audition or processed export. Existing installations show all five.
struct TransportEffectVisibility {
    bool mono = true;
    bool reverse = true;
    bool pan = true;
    bool soundstage = true; // CenterSense and Bass Mono share one control.
    bool eq = true;

    bool operator==(const TransportEffectVisibility& other) const {
        return mono == other.mono && reverse == other.reverse && pan == other.pan &&
               soundstage == other.soundstage && eq == other.eq;
    }
    bool operator!=(const TransportEffectVisibility& other) const { return !(*this == other); }
};

// ma_audio_buffer is a miniaudio typedef and cannot be forward-declared.
class CenterSenseDataSource;
struct PcmAudioBufferState;

// Output device info for device selection UI
struct OutputDeviceInfo {
    std::string name;
    bool isDefault = false;
    std::vector<uint8_t> deviceIdBlob;  // serialized ma_device_id (opaque, backend-specific)
};

// Audio file information
struct AudioFileInfo {
    std::string filePath;
    std::string fileName;
    uint32_t sampleRate = 0;
    uint32_t channels = 0;
    uint64_t totalFrames = 0;
    float durationSeconds = 0.0f;
    std::string format;
};

// Waveform data for visualization
struct WaveformData {
    std::vector<float> peaks;      // Peak values for display (max absolute value per block)
    std::vector<float> minPeaks;   // Negative-side peak magnitude per block (for asymmetric DAW-style rendering)
    std::vector<float> rms;        // RMS values for display
    int samplesPerPeak = 0;
    bool isReady = false;
    bool isFallbackPlaceholder = false;
    int targetPeaks = 0;
    int completedPeaks = 0;
    bool isProgressiveDetail = false;
    
    // Track which file this waveform belongs to (for consistency checking)
    std::string filePath;

    // Multi-channel support:
    // peaks/rms are interleaved: [ch0_f0, ch1_f0, ..., chN_f0, ch0_f1, ...]
    // numChannels=1 → mono, numChannels=2 → stereo (L/R), >2 → surround
    int numChannels = 1;

    // Backward compat: isStereo == (numChannels == 2)
    bool isStereo = false;
    
    // ITU-R BS.1770-4 Loudness measurements
    float integratedLUFS = -70.0f;   // Integrated Loudness (with gating) - overall program loudness
    float momentaryLUFS  = -70.0f;   // Max Momentary Loudness (400ms, no gating) - loudest moment
    float shortTermLUFS  = -70.0f;   // Max Short-Term Loudness (3s, no gating)
    float truePeakDBTP   = -70.0f;   // True Peak (4x oversampled) in dBTP
    bool hasLoudnessData = false;    // true if properly computed

    // 真实 RMS dBFS：在全文件解码时从原始 PCM 样本累加 sum-of-squares 计算
    // 公式：20 * log10(sqrt(sum(x²) / N))，匹配 ffmpeg astats 输出
    float trueRmsDbfs = -100.0f;
    bool  hasTrueRms = false;
};

struct ExportRegionInfo {
    uint64_t regionStartFrame = 0;
    uint64_t framesWritten = 0;
    uint64_t totalFrames = 0;
    uint32_t sampleRate = 0;
    uint32_t channels = 0;
};

// 响度标准预设 (F-018)
struct LoudnessStandard {
    const char* name;
    const char* name_zh;
    float targetLUFS;      // 目标积分响度
    float maxTruePeakDBTP; // 最大真峰值
};

inline const LoudnessStandard kLoudnessStandards[] = {
    {"EBU R128 (Broadcast)",    "EBU R128（广播）",     -23.0f, -1.0f},
    {"ATSC A/85 (US TV)",       "ATSC A/85（美国电视）", -24.0f, -2.0f},
    {"Podcast / Audiobook",     "播客/有声书",           -16.0f, -1.0f},
    {"Streaming (Spotify/YT)",  "流媒体 (Spotify/YT)",  -14.0f, -1.0f},
    {"Cinema (SMPTE RP 200)",   "电影 (SMPTE RP 200)",  -20.0f, -3.0f},
    {"Game Audio",              "游戏音频",              -18.0f, -1.0f},
};
inline constexpr int kLoudnessStandardCount = sizeof(kLoudnessStandards) / sizeof(kLoudnessStandards[0]);

/// 检查文件响度是否符合指定标准（容差 ±1 LUFS）
inline bool IsLoudnessCompliant(float integratedLUFS, float truePeakDBTP,
                                 const LoudnessStandard& std, float toleranceLUFS = 1.0f) {
    return (integratedLUFS >= std.targetLUFS - toleranceLUFS) &&
           (integratedLUFS <= std.targetLUFS + toleranceLUFS) &&
           (truePeakDBTP <= std.maxTruePeakDBTP);
}

// Region loop restart should happen only near the tail of the current region.
// We keep a small slack window so frame-quantized cursor updates still wrap reliably.
inline bool ShouldRestartLoopedRegionFrames(uint64_t cursorFrame, uint64_t inFrame, uint64_t outFrame) {
    if (outFrame <= inFrame + 1) return false;
    const uint64_t regionFrames = outFrame - inFrame;
    const uint64_t tailSlackFrames = std::max<uint64_t>(1, std::min<uint64_t>(64, regionFrames / 8));
    const uint64_t thresholdFrame = (outFrame > tailSlackFrames) ? (outFrame - tailSlackFrames) : outFrame;
    return cursorFrame >= thresholdFrame;
}

// Unselected full-sound looping. Some formats report a longer metadata length
// than the decoder can actually play; when a real decoded length is known it
// caps the wrap so playback does not stall past EOF. When the decoded length is
// unknown (sync/streaming load that never fully decoded the buffer), the
// reported length is the only usable loop endpoint — refusing to loop there
// would disable the main feature, so we keep looping at the reported tail.
inline uint64_t ResolveLoopedFullSoundOutFrame(uint64_t reportedTotalFrames,
                                               uint64_t decodedLengthFrames) {
    if (decodedLengthFrames == 0) return reportedTotalFrames;
    if (reportedTotalFrames == 0) return decodedLengthFrames;
    return (std::min)(reportedTotalFrames, decodedLengthFrames);
}

// End-to-end wrap decision for an unselected full-sound loop as performed by
// pumpRegionPlayback: returns the frame the sound must seek back to (0) for the
// loop to restart, and whether that restart is due.
// - While playing: wrap near the usable tail (real decoded length when known,
//   otherwise the reported length), or once the cursor passes a known decoded
//   length (truncated-metadata formats).
// - While stopped: a live user play intent plus a pass observed playing means
//   the source exhausted. That holds even when the backend reset the cursor to
//   0 (OGG push-mode) or the length is unknown, so no cursor guard applies.
//   A user pause (playIntent=false) or a not-yet-started pass must never
//   auto-restart.
struct FullSoundLoopWrap {
    uint64_t seekFrame = 0;
    bool wrap = false;
};
inline FullSoundLoopWrap DecideUnselectedFullSoundLoopWrap(
    uint64_t cursorFrame, uint64_t reportedTotalFrames, uint64_t decodedLengthFrames,
    bool playing, bool playIntent, bool sawPlaying) {
    FullSoundLoopWrap decision;
    // A disarmed loop (loop switch off / pause / stop) must never wrap.
    if (!playIntent) return decision;
    const uint64_t outFrame = ResolveLoopedFullSoundOutFrame(reportedTotalFrames, decodedLengthFrames);
    if (playing) {
        if (outFrame <= 1) return decision;
        const bool atTail =
            ShouldRestartLoopedRegionFrames(cursorFrame, 0, outFrame) ||
            (decodedLengthFrames > 0 && cursorFrame >= decodedLengthFrames);
        if (atTail) { decision.seekFrame = 0; decision.wrap = true; }
        return decision;
    }
    // Stopped with the loop armed: exhausted iff this pass actually played.
    // Length-agnostic: OGG push-mode keeps outFrame 0 until the waveform
    // worker resolves the real length, and the backend may reset the cursor.
    if (sawPlaying) { decision.seekFrame = 0; decision.wrap = true; }
    return decision;
}

// The pump's hoisted restart predicate: an armed unselected loop whose sound
// stopped after a pass that actually played must start the next pass from
// frame 0, independent of any length knowledge. Boxed selections keep their
// own explicit endpoint stop path.
inline bool ShouldRestartExhaustedUnselectedLoop(bool unselectedFullSoundLoop,
                                                 bool regionLoopActive,
                                                 bool playing,
                                                 bool playIntent,
                                                 bool sawPlaying) {
    return unselectedFullSoundLoop && regionLoopActive &&
           playIntent && sawPlaying && !playing;
}

// Start-path arming rule for an unselected full-sound loop. OGG/Vorbis
// push-mode reports QueryLength 0, so totalFrames stays 0 until the waveform
// worker resolves the real length. Requiring a known length at the play /
// playRegion entry made the first pass a no-op (nothing armed, nothing
// started), which is exactly why the user had to press play a second time.
// A loop request over the whole sound may arm region playback with zero known
// length; the pump's length-agnostic exhaustion restart then completes the
// first loop. Boxed selections still need their explicit endpoint and must
// not use this path.
inline bool ShouldArmRegionLoopWithoutLength(float inPoint, float outPoint,
                                             bool loopEnabled) {
    if (!loopEnabled) return false;
    return inPoint <= 0.0f && (outPoint >= 1.0f || outPoint <= 0.0f);
}

// Playing-state convenience kept for callers/tests that only reason about a
// moving cursor.
inline bool ShouldRestartUnselectedFullSoundLoop(uint64_t cursorFrame,
                                                 uint64_t reportedTotalFrames,
                                                 uint64_t decodedLengthFrames) {
    return DecideUnselectedFullSoundLoopWrap(
        cursorFrame, reportedTotalFrames, decodedLengthFrames, true, true, true).wrap;
}

// Sync decoder load may keep QueryLength as file metadata, but that value is
// not decoded PCM. Only an independently counted PCM length may be published
// as decodedPlaybackTotalFrames; QueryLength-only falls back to 0.
inline uint64_t ResolveDecodedPlaybackTotalFramesFromSyncLoad(uint64_t queryLengthFrames,
                                                              uint64_t actualPcmFrames) {
    (void)queryLengthFrames;
    return actualPcmFrames;
}

// Loop wrap may seek only while the sound is already playing. A paused
// unselected loop must not auto-Start itself from the pump.
inline bool ShouldSeekLoopedRegionWhilePlaying(bool playing, bool shouldRestart) {
    return playing && shouldRestart;
}

// Turning the loop switch off disarms looping in the same tick. Playback is
// never paused or stopped: a boxed region keeps its region playback for the
// remainder of the current pass, while an armed full-sound (unselected) loop
// pass de-arms region mode entirely and simply finishes the pass.
struct LoopDisarmState {
    bool regionPlayback = false;
    bool regionLoopActive = false;
};
inline LoopDisarmState DecideLoopDisarmKeepCurrentPass(bool boxedPlayback) {
    LoopDisarmState state;
    state.regionPlayback = boxedPlayback;
    state.regionLoopActive = false;
    return state;
}

// @stage7-contract
// API contract: AudioEngine owns playback and asynchronous load state. Callers
// must initialize before playback operations, serialize access to one instance,
// and pump pending asynchronous work from the owning/UI thread as documented
// by the load methods. File paths are UTF-8; failed loads preserve the prior
// loaded state and expose diagnostics through the error accessors.
class AudioEngine {
public:
    static constexpr float kFreqShiftRangeHz = 5000.0f;

    struct TransferProcessingSnapshot {
        float pitchSemitones = 0.0f;
        float volume = 1.0f;
        float replayGain = 1.0f;
        float pan = 0.0f;
        float centerSense = 0.0f;
        float bassMonoFreqHz = 0.0f;
        bool reverse = false;
        bool mono = false;
        int channelSolo = 0;
        float inPoint = 0.0f;
        float outPoint = 1.0f;
        uint64_t totalFrames = 0;
        MagicEQSettings magicEQ{};
    };

    friend TransferProcessingSnapshot ApplyTransportEffectVisibility(
        TransferProcessingSnapshot requested, const TransportEffectVisibility& visibility) {
        if (!visibility.mono) requested.mono = false;
        if (!visibility.reverse) requested.reverse = false;
        if (!visibility.pan) requested.pan = 0.0f;
        if (!visibility.soundstage) {
            requested.centerSense = 0.0f;
            requested.bassMonoFreqHz = 0.0f;
        }
        if (!visibility.eq) requested.magicEQ.enabled = false;
        return requested;
    }

    AudioEngine();
    ~AudioEngine();

    // Initialize/shutdown（periodSizeMs：缓冲周期 ms，越小延迟越低、对 CPU 要求越高）
    bool initialize(int periodSizeMs = 10);
    void shutdown();

    // File operations
    bool loadFile(const std::string& filePath);
    /// 异步加载：后台解码，主线程仅做 sound 绑定（规范 §11 避免主线程 200–450ms 卡顿）
    /// playWhenReady：加载完成后在主线程自动 play()（默认 true，与原有 load+play 行为一致）
    void loadFileAsync(const std::string& filePath, bool playWhenReady = true);
    /// 主循环每帧调用；若有就绪结果则应用并返回 true（调用方可据此 play/更新 UI）
    bool pumpLoadFileIfPending();
    bool isLoadFilePending() const { return m_loadAsyncPending.load(); }
    /// Call after pumpLoadFileIfPending()==true and any silence-skip seek;
    /// starts playback if loadFileAsync was called with playWhenReady=true.
    void playIfLoadReady();
    /// 解码失败时返回并清除错误信息，供状态栏/提示用；未失败则返回空
    std::string getLastLoadError();
    /// 只读，不清除——给 GM Debug panel 用
    std::string peekLastLoadError() const;
    std::string peekLastLoadDiag() const;
    void unload();
    void discardLoadedFileForDatabaseBoundary();

    // Playback control
    void play();
    void pause();
    void stop();
    void setVolume(float volume);  // 0.0 - 1.0
    // Apply ReplayGain before playback. gainDb = track or album gain in dB.
    // referenceLevel is reserved for future LUFS-fallback integration (F-018).
    // A zero gain disables ReplayGain adjustment.
    void applyReplayGain(float gainDb, float referenceLevel = -18.0f);
    void setPan(float pan);        // -1.0 (left) to 1.0 (right)
    float getRequestedPan() const { return m_forceMono ? m_panStereo : m_pan; }
    void setTransportEffectVisibility(const TransportEffectVisibility& visibility);
    TransportEffectVisibility getTransportEffectVisibility() const { return m_transportEffectVisibility; }
    void setCenterSense(float k);  // -1 (tighten) to 0 (original) to +1 (widen), stereo field
    float getCenterSense() const { return m_centerSense; }
    void setMagicEQ(const MagicEQSettings& settings);
    MagicEQSettings getMagicEQ() const;
    MagicEQSettings getMagicEQUserSettings() const; // Persistence keeps the requested toggle during a channel bypass.
    void setBassMonoFreq(float hz);  // 0 = off, 20-500 Hz = low-frequency mono collapse cutoff
    float getBassMonoFreq() const { return m_bassMonoFreq; }
    void setMono(bool mono);       // true = force mono playback (downmix), false = stereo
    bool isMono() const { return m_forceMono; }
    
    // Channel solo mode: 0 = all channels, 1..8 = common layout channel solo
    void setChannelSolo(int mode); // 0=all channels, 1..8=common channel layout solo.
    int getChannelSolo() const { return m_channelSolo; }
    void setPitch(float semitones);  // Pitch shift in semitones (-12 to +12)
    void setPitchMode(PitchMode mode);  // Switch between Semitone and FreqShift modes
    PitchMode getPitchMode() const { return m_pitchMode; }
    void setFreqShift(float hz);    // Frequency shift in Hz (±5000, FreqShift mode)
    float getFreqShift() const { return m_freqShiftHz; }
    bool setReverse(bool reverse);   // Enable/disable reverse playback
    bool isReversed() const { return m_reverse; }
    void seekToPosition(float normalizedPos);  // 0.0 - 1.0

    // Click protection (fade in/out on play/seek/stop/pause)
    // When enabled, engine will apply a short fade around transitions to reduce clicks/pops.
    void setFadeInOutEnabled(bool enabled);
    bool isFadeInOutEnabled() const { return m_fadeEnabled.load(); }
    void setFadeInOutMs(int ms);  // 0-60 recommended
    int getFadeInOutMs() const { return m_seekFadeMs.load(); }

    // Region selection (in/out points)
    void setInPoint(float normalizedPos);   // 0.0 - 1.0
    void setOutPoint(float normalizedPos);  // 0.0 - 1.0
    void setRegionFadeNorms(float fadeInNorm, float fadeOutNorm);  // real-time fade sync during drag
    void restoreRegionPlayback(float fadeInNorm, float fadeOutNorm); // re-enable fade after seek without restarting
    void clearInOutPoints();
    float getInPoint() const { return m_inPoint; }
    float getOutPoint() const { return m_outPoint; }
    void playRegion(bool loopEnabled = false, float regionFadeInNorm = 0.0f, float regionFadeOutNorm = 0.0f);  // Play only the selected region
    void pumpRegionPlayback();                  // Maintain active region playback mode once per frame
    void stopRegionPlaybackMode();              // Exit active region playback mode without changing UI selection
    void setUserLoopEnabled(bool enabled);
    // Cancel a running loop immediately (user switched loop off): the current
    // pass keeps playing to its natural end but must never wrap again. Does not
    // pause/stop or seek, so the sound just finishes the pass it is in.
    void stopLoopKeepCurrentPass();
    bool isUserLoopEnabled() const { return m_userLoopEnabled; }

    // Export selected region to a new file.
    // regionFadeInNorm / regionFadeOutNorm: fractions of exported region length [0..1], from UI ClampRegionFades.
    // When both are ~0: apply fixed ~10ms symmetric declick (legacy anti-pop). When either > 0: use only those fades (no extra 10ms).
    bool exportRegion(const std::string& outputPath,
                      float regionFadeInNorm = 0.0f,
                      float regionFadeOutNorm = 0.0f,
                      ExportRegionInfo* outInfo = nullptr,
                      const TransferProcessingSnapshot* transferSnapshot = nullptr);
    static uint64_t computeRegionStartFrame(float inPt, float outPt,
                                            uint64_t totalFrames, bool isReverse) {
        if (totalFrames == 0) return 0;
        double pos = isReverse ? (1.0 - (double)outPt) : (double)inPt;
        if (pos < 0.0) pos = 0.0;
        if (pos > 1.0) pos = 1.0;
        return (uint64_t)(pos * (double)totalFrames);
    }
    /// Export full input file with current DSP settings applied (pitch/volume/pan/centerSense/mono/solo).
    /// Backs drag-to-DAW of a processed version. Definition in AudioEngine.cpp.
    bool exportFileProcessed(const std::string& inputPath, const std::string& outputPath,
                             const TransferProcessingSnapshot* transferSnapshot = nullptr);
    TransferProcessingSnapshot captureTransferProcessingSnapshot() const;
    /// True when any DSP adjustment is active (pitch/volume/pan/centerSense/reverse/solo/forceMono).
    bool hasAudioAdjustments() const;
    /// 上次 exportRegion 失败时的简短原因（用于弹窗/日志，便于间歇性「拖拽失败」定位）
    const std::string& getLastExportError() const { return m_lastExportError; }

// F-019-channel: Single-channel extraction export
    // Decode inputPath, extract channel at channelIndex (0-based), write as mono WAV to outputPath.
    // channelIndex: 0=L/Front-Left, 1=R/Front-Right, 2=Center, 3=LFE, 4=SL, 5=SR, etc.
    // Returns false and sets lastExportError on failure.
    // This is a static operation (does not require a loaded file in the engine).
    static bool exportSingleChannel(const std::string& inputPath,
                                    int channelIndex,
                                    const std::string& outputPath,
                                    std::string* outError = nullptr);

    // State queries
    bool isPlaying() const;
    bool isPaused() const;
    bool isLoaded() const;
    uint32_t getEngineSampleRate() const;
    float getPlaybackPosition() const;  // 0.0 - 1.0
    float getOutputLatencyInSeconds() const;
    float getCurrentTimeSeconds() const;
    const AudioFileInfo& getFileInfo() const { return m_fileInfo; }
    std::string getLoadedFilePath() const { return m_fileInfo.filePath; }

    // Waveform generation
    // Thread-safe: waveform is stored in an atomically swapped shared_ptr
    std::shared_ptr<const WaveformData> getWaveformData() const;
    void generateWaveform(int targetPeaks = 12000);
    // Request higher-resolution waveform (can be called frequently; coalesces requests)
    void requestWaveformDetail(int targetPeaks);
    bool isWaveformDetailPending() const;
    /**
     * 🎯 手动覆盖波形数据：允许 UI 根据缩放级别瞬间替换当前显示的 Peaks
     */
    void updateWaveformDataManually(const std::vector<float>& newPeaks);
    void clearCurrentWaveformVisual();
    // Force regenerate waveform (resets counters to trigger new generation)
    void forceRegenerateWaveform(int targetPeaks = 12000);

    // Static waveform generation for a file (for caching purposes)
    static bool generateWaveformForFile(const std::string& filePath,
                                        WaveformData& outWaveform,
                                        int targetPeaks = 100,
                                        const std::atomic<bool>* cancelFlag = nullptr,
                                        bool collectPeaks = true);
    static bool generateWaveformFromOpenDecoder(
        Platform::AudioDataSource::Decoder& decoder,
        uint32_t channels,
        uint32_t sampleRate,
        uint64_t declaredTotalFrames,
        int targetPeaks,
        WaveformData& outWaveform,
        const std::atomic<bool>* cancelFlag,
        bool collectPeaks = true);
    // Decode an entire file to native-rate/channels interleaved f32.
    static bool DecodeFileToInterleavedF32(const std::string& filePath,
                                           std::vector<float>& outSamples,
                                           uint64_t& outFrames,
                                           uint32_t& outChannels,
                                           uint32_t& outSampleRate,
                                           const std::atomic<bool>* cancelFlag = nullptr,
                                           uint64_t maxFrames = 0);
    // Compute display peaks/RMS + (when enabled) BS.1770 loudness from an
    // in-memory interleaved f32 buffer. Avoids a second decode when the caller
    // already has the samples (MediaFoundation mini path).
    static bool computeWaveformAndLoudnessFromSamples(const float* interleaved,
                                                      uint64_t totalFrames,
                                                      uint32_t channels,
                                                      uint32_t sampleRate,
                                                      int targetPeaks,
                                                      WaveformData& outWaveform,
                                                      const std::atomic<bool>* cancelFlag = nullptr);
    // Build the same full-detail waveform used by the main view for DB prewarming.
    // This is not a quick/rough preview path; callers should run it only from low-priority background work.
    bool buildWaveformDetailForCache(const std::string& filePath, WaveformData& outWaveform, int targetPeaks = 12000);

    // Seek-based waveform generation samples uniform positions instead of decoding the full file.
    // 适用于 mini 缩略图（100 peaks）等不需要精确逐帧扫描的场景
    // perChannel=true 时输出每通道独立 peaks（用于主波形 detail 缓存），否则混合为单通道
    static bool generateQuickWaveformForFile(const std::string& filePath, WaveformData& outWaveform, int targetPeaks = 100, bool perChannel = false);

    /**
     * 🚀 高精度解码：将完整音频文件解码到内存，供生成金字塔波形使用
     */
    static bool decodeFullFileToMemory(const std::string& filePath,
                                       std::vector<float>& outSamples,
                                       uint32_t& outSampleRate);

    // Volume/pan/pitch getters
    float getVolume() const { return m_volume; }
    float getPan() const { return m_pan; }
    float getPitch() const { return m_pitch; }

// Output device selection
    std::vector<OutputDeviceInfo> enumerateOutputDevices();
    bool switchOutputDevice(int deviceIndex);  // -1 = system default
    int getCurrentOutputDeviceIndex() const { return m_outputDeviceIndex; }
    std::string getCurrentOutputDeviceName() const { return m_outputDeviceName; }
    std::vector<uint8_t> getCurrentOutputDeviceId() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_outputDeviceIdBlob;
    }

private:
    void enforceMagicEQAvailabilityLocked(); // m_mutex held; publish a visible bypass on unsupported files.
    void applyEffectiveVolume();   // Set playback sound volume to m_volume * m_replayGain
    void resetState(bool clearVisualWaveform = true);
    bool readAudioData(std::vector<float>& outSamples);
    bool loadReversedAudio();  // Load audio data in reverse for reverse playback
    bool reloadWithReverseState();  // Reload file with current reverse state
    void ensureWaveformWorkerStarted();
    void stopWaveformWorker();
    void waveformWorkerLoop();

    static WaveformData BuildDisplayWaveformFromSource(const WaveformData& source,
                                                       bool forceMono,
                                                       int channelSolo,
                                                       const std::string& filePath);

    bool tryApplySourceWaveformForCurrentView(const std::shared_ptr<const WaveformData>& sourceSnapshot,
                                              const std::string& expectedPath);

    static bool buildWaveform(const std::string& filePath, Platform::AudioPlayback::BorrowedEngine engine, bool forceMono, int channelSolo, WaveformData& out, int targetPeaks);
    // Seek-based quick waveform: reads only small blocks at evenly spaced positions.
    // Much faster than buildWaveform for producing a rough preview (~200 peaks).
    static bool buildQuickWaveform(const std::string& filePath, Platform::AudioPlayback::BorrowedEngine engine, bool forceMono, int channelSolo, WaveformData& out, int targetPeaks);

    void ensureSeekWorkerStarted();
    void stopSeekWorker();
    void seekWorkerLoop();

    // 异步 loadFile：后台 init decoder，主线程绑定 sound
    void ensureLoadFileWorkerStarted();
    void stopLoadFileWorker();
    void loadFileWorkerLoop();
    bool applyLoadFileResult(std::unique_ptr<Platform::AudioDataSource::Decoder> decoder,
                             const std::string& filePath,
                             const AudioFileInfo& fileInfo);
    bool installDecodedPcmSourceLocked(const AudioFileInfo& fileInfo, std::vector<float>&& pcm);

    struct LoadFileAsyncResult {
        std::unique_ptr<Platform::AudioDataSource::Decoder> decoder;
        std::string filePath;
        AudioFileInfo fileInfo;
        std::vector<float> decodedPcm;
        uint32_t decodedPcmSampleRate = 0;
        uint32_t decodedPcmChannels = 0;
        uint64_t decodedPcmTotalFrames = 0;
        bool decodedPcmSource = false;
        bool success = false;
        uint64_t token = 0;
        int errorCode = 0;       // ma_result when decode fails
        std::chrono::steady_clock::time_point requestStartedAt{};
        std::chrono::steady_clock::time_point decodeCompletedAt{};
    };
    bool applyDecodedPcmLoadFileResult(LoadFileAsyncResult& result);
    std::mutex m_loadAsyncRequestMutex;
    std::condition_variable m_loadAsyncRequestCv;
    std::string m_loadAsyncRequestPath;
    uint64_t m_loadAsyncRequestToken = 0;
    uint32_t m_loadAsyncRequestSampleRate = 48000;
    bool m_loadAsyncRequestForceMono = false;
    int m_loadAsyncRequestChannelSolo = 0;
    std::chrono::steady_clock::time_point m_loadAsyncRequestStartedAt{};
    bool m_loadAsyncRequestPending = false;
    std::thread m_loadAsyncWorker;
    std::thread m_loadAsyncSpareWorker;
    std::atomic<bool> m_loadAsyncExit{false};
    std::atomic<uint64_t> m_loadAsyncCurrentToken{0};
    std::queue<LoadFileAsyncResult> m_loadAsyncResultQueue;
    mutable std::mutex m_loadAsyncResultMutex;
    std::atomic<bool> m_loadAsyncPending{false};
    bool m_loadAsyncPlayWhenReady = true;
    /** 加载未完成时预约的归一化跳转位置 [0,1]，-1 表示无预约 */
    std::atomic<float> m_pendingNormalizedSeek{-1.0f};
    /** 加载完成但 totalFrames==0 时（OGG push-mode）保留的 seek，等 waveform worker
        更新 totalFrames 后再应用；-1 表示无延迟预约 */
    std::atomic<float> m_deferredSeekNorm{-1.0f};

    std::unique_ptr<Platform::AudioPlayback::Engine> m_engine;
    std::unique_ptr<Platform::AudioPlayback::Sound> m_sound;
    std::unique_ptr<Platform::AudioDataSource::Decoder> m_decoder;
    PcmAudioBufferState* m_pcmState = nullptr; // Decoded PCM fallback source (WMA via Media Foundation)
    CenterSenseDataSource* m_centerSenseDataSource = nullptr; // When non-null, sound reads from this (wraps m_decoder)
    std::uint32_t m_playbackChannels = 0; // Actual source channels, which may differ from file metadata.
    bool m_magicEQUnsupportedChannels = false;
    bool m_magicEQUserEnabled = false; // The requested toggle survives temporary multichannel bypass.

    AudioFileInfo m_fileInfo;
    std::shared_ptr<const WaveformData> m_waveform;
    std::shared_ptr<const WaveformData> m_waveformSource;  // 原始 detail 波形（用于 mono/solo 即时切显示）

    // Reverse playback buffer (stores reversed audio data)
    std::vector<float> m_reverseBuffer;
    bool m_reverseBufferValid = false;

    float m_volume = 1.0f;
    float m_replayGain = 1.0f;  // ReplayGain linear multiplier; applied on top of m_volume
    float m_pan = 0.0f;
    float m_panStereo = 0.0f;     // Remember pan when switching mono on/off

    TransportEffectVisibility m_transportEffectVisibility{};
    float m_centerSense = 0.0f;   // -1 tighten, 0 original, +1 widen (stereo field)
    float m_centerSenseEffective = 0.0f;
    MagicEQSettings m_magicEQ{};
    CenterSenseDsp::MagicEQSettingsPublisher m_magicEQRealtime;
    float m_bassMonoFreq = 0.0f;  // 0 = off, >0 = cutoff Hz for low-frequency mono collapse
    float m_bassMonoFreqEffective = 0.0f;
    bool m_forceMono = false;
    int m_channelSolo = 0;
    float m_pitch = 0.0f;  // Semitones
    PitchMode m_pitchMode = PitchMode::Semitone;
    float m_freqShiftHz = 0.0f;  // Hz shift amount (FreqShift mode, ±5000)
    bool m_reverse = false;  // Reverse playback
    float m_inPoint = 0.0f;   // Normalized 0-1
    float m_outPoint = 1.0f;  // Normalized 0-1
    bool m_isInitialized = false;
    bool m_isLoaded = false;
    bool m_regionPlayback = false;  // Playing region only
    bool m_regionLoopActive = false;
    bool m_userLoopEnabled = false;
    // True between an explicit user start (play/playRegion) and a user
    // pause/stop or a new async load. Distinguishes "source exhausted" from
    // "user paused" so the loop fallback never resurrects a paused sound.
    bool m_playIntent = false;
    // Set once the pump has actually seen this pass playing, so the
    // exhausted-restart never fires on a not-yet-started (or failed-start)
    // pass; cleared on every explicit start, load, pause or reset.
    bool m_loopSawPlaying = false;
    uint64_t m_decodedPlaybackTotalFrames = 0;
    float m_regionFadeInNorm = 0.0f;
    float m_regionFadeOutNorm = 0.0f;

    mutable std::mutex m_mutex;

    // Waveform detail worker (background generation)
    std::mutex m_waveformWorkerMutex;
    std::condition_variable m_waveformWorkerCv;
    std::thread m_waveformWorker;
    std::atomic<bool> m_waveformWorkerExit{false};
    std::atomic<int> m_waveformRequestedPeaks{0};
    std::atomic<int> m_waveformGeneratedPeaks{0};
    // Monotonic token for latest-wins cancellation.
    // Incremented on file/context changes and new waveform requests.
    std::atomic<uint64_t> m_waveformRequestToken{1};

    // Seek smoothing worker (avoid audible clicks when jumping)
    std::mutex m_seekWorkerMutex;
    std::condition_variable m_seekWorkerCv;
    std::thread m_seekWorker;
    std::atomic<bool> m_seekWorkerExit{false};
    std::atomic<bool> m_seekPending{false};
    std::atomic<uint64_t> m_seekTargetFrame{0};
    std::atomic<uint64_t> m_seekToken{0};
    std::atomic<int> m_seekFadeMs{12};

    std::atomic<bool> m_fadeEnabled{true};

    // Deferred sound cleanup queue — replaces detached threads.
    // Old sounds are scheduled for cleanup after their fade-out completes,
    // then drained on the main thread to avoid UAF in the audio callback.
    struct DeferredCleanup {
        std::unique_ptr<Platform::AudioPlayback::Sound> sound;
        std::unique_ptr<Platform::AudioDataSource::Decoder> decoder;
        PcmAudioBufferState* pcm = nullptr;
        CenterSenseDataSource* csds = nullptr;
        std::chrono::steady_clock::time_point deadline;
    };
    std::vector<DeferredCleanup> m_cleanupQueue;
    void pumpDeferredCleanup();
    void flushDeferredCleanup();  // drain all immediately (for shutdown)

    // Output device selection
    int m_outputDeviceIndex = -1;           // -1 = system default
    std::string m_outputDeviceName;
    std::vector<uint8_t> m_outputDeviceIdBlob; // active identity; unaffected by re-enumeration
    std::vector<OutputDeviceInfo> m_cachedDevices;

    std::string m_lastExportError;  // 上次 exportRegion 失败原因，供弹窗/日志
    std::string m_lastLoadError;    // 异步加载解码失败原因，getLastLoadError() 取后清空
    std::string m_lastLoadDiag;     // 最后一次加载诊断信息（不清空）
    mutable std::mutex m_lastLoadErrorMutex;
};
