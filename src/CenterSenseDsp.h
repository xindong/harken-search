// CenterSenseDsp — 中心感旋钮分频段系数与处理
// 收紧侧：三频段居中量 0~1；拓宽侧：三频段宽度系数 ≥1。
// 设计见 docs/plans/2026-03-17-中心感旋钮-design.md

#pragma once

#include <cstddef>
#include <atomic>
#include <cstdint>

namespace CenterSenseDsp {

enum class MagicEQMode : int { Pass = 0, Notch = 1 };
enum class MagicEQSteepness : int { Soft = 0, Steep = 1, VerySteep = 2, Brickwall = 3 };

inline constexpr float kDefaultMagicEQWidgetWidth = 520.0f;

struct MagicEQSettings {
    bool enabled = false;
    MagicEQMode mode = MagicEQMode::Pass;
    MagicEQSteepness steepness = MagicEQSteepness::Steep;
    float centerFreq = 1000.0f;
    float halfWidthPercent = 10.0f;
    float widgetWidth = kDefaultMagicEQWidgetWidth;
    // Control-thread status only; deliberately absent from the audio snapshot.
    bool invalidInput = false;
};

// UI writes occur under AudioEngine's settings mutex. The audio callback reads
// this non-blocking snapshot and keeps its previous value if a write is in
// flight, avoiding a data race or a lock on the realtime thread.
class MagicEQSettingsPublisher {
public:
    void store(const MagicEQSettings& settings) noexcept {
        sequence_.fetch_add(1, std::memory_order_acq_rel);
        enabled_.store(settings.enabled, std::memory_order_relaxed);
        mode_.store(static_cast<int>(settings.mode), std::memory_order_relaxed);
        steepness_.store(static_cast<int>(settings.steepness), std::memory_order_relaxed);
        centerFreq_.store(settings.centerFreq, std::memory_order_relaxed);
        halfWidthPercent_.store(settings.halfWidthPercent, std::memory_order_relaxed);
        sequence_.fetch_add(1, std::memory_order_release);
    }

    bool tryLoad(MagicEQSettings& result) const noexcept {
        const uint32_t before = sequence_.load(std::memory_order_acquire);
        if ((before & 1u) != 0u) return false;
        MagicEQSettings next;
        next.enabled = enabled_.load(std::memory_order_relaxed);
        next.mode = mode_.load(std::memory_order_relaxed) == 1
            ? MagicEQMode::Notch : MagicEQMode::Pass;
        const int steepness = steepness_.load(std::memory_order_relaxed);
        next.steepness = steepness >= 3 ? MagicEQSteepness::Brickwall
            : (steepness == 2 ? MagicEQSteepness::VerySteep
            : (steepness == 1 ? MagicEQSteepness::Steep : MagicEQSteepness::Soft));
        next.centerFreq = centerFreq_.load(std::memory_order_relaxed);
        next.halfWidthPercent = halfWidthPercent_.load(std::memory_order_relaxed);
        const uint32_t after = sequence_.load(std::memory_order_acquire);
        if (before != after) return false;
        result = next;
        return true;
    }

private:
    mutable std::atomic<uint32_t> sequence_{0};
    std::atomic<bool> enabled_{false};
    std::atomic<int> mode_{0};
    std::atomic<int> steepness_{1};
    std::atomic<float> centerFreq_{1000.0f};
    std::atomic<float> halfWidthPercent_{10.0f};
};

float magicEQFrequencyToPercent(float frequencyHz);
float magicEQPercentToFrequency(float percent);
void magicEQSetLowEdgePercent(float lowPercent, float& centerFreq, float& halfWidthPercent);
void magicEQSetHighEdgePercent(float highPercent, float& centerFreq, float& halfWidthPercent);

struct MagicEQState {
    static constexpr int kStages = 24;
    struct Biquad {
        double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
        double z1 = 0.0, z2 = 0.0, z1R = 0.0, z2R = 0.0;
    };
    struct FilterBank {
        Biquad sections[kStages];
        int sectionCount = 0;
        int sampleRate = 0;
        MagicEQMode mode = MagicEQMode::Pass;
        MagicEQSteepness steepness = MagicEQSteepness::Steep;
        float centerFreq = 1000.0f;
        float halfWidthPercent = 10.0f;
        float lowFreq = 0.0f;
        float highFreq = 0.0f;
        double responseGain = 1.0;
        double maxPoleRadius = 0.0;
        bool enabled = false;
        bool prepared = false;
    };
    FilterBank banks[2];
    int activeBank = 0;
    int transitionBank = 1;
    int transitionFramesRemaining = 0;
    int transitionTotalFrames = 0;
    bool hasPendingParams = false;
    bool pendingEnabled = false;
    MagicEQMode pendingMode = MagicEQMode::Pass;
    MagicEQSteepness pendingSteepness = MagicEQSteepness::Steep;
    float pendingCenterFreq = 1000.0f;
    float pendingHalfWidthPercent = 10.0f;
    int sampleRate = 0;
    MagicEQMode mode = MagicEQMode::Pass;
    MagicEQSteepness steepness = MagicEQSteepness::Steep;
    float centerFreq = 1000.0f;
    float halfWidthPercent = 10.0f;
    float lowFreq = 0.0f;
    float highFreq = 0.0f;
    bool prepared = false;
    // Shared per-frame gain: an overload on either channel changes both channels equally.
    // Unity until the first overfullscale filtered sample; seek/file reset clears it.
    double overloadGain = 1.0;
};

struct MagicEQProcessParams {
    bool enabled = false;
    MagicEQMode mode = MagicEQMode::Pass;
    MagicEQSteepness steepness = MagicEQSteepness::Steep;
    float centerFreq = 1000.0f;
    float halfWidthPercent = 10.0f;
};

enum class MagicEQTailReason { None, InvalidSampleRate, FrequencyLimit, TailBudget };
struct MagicEQTailEstimate {
    bool available = false;
    float estimatedSeconds = 0.0f;
    MagicEQTailReason reason = MagicEQTailReason::None;
};
// UI/control path only. Pole-envelope estimate for a -80 dB decay relative to
// an initial unit amplitude; validate its conservative gate against measured
// product impulse responses. Never call on the audio callback.
MagicEQTailEstimate estimateMagicEQTail(const MagicEQProcessParams& params, int sampleRate);

enum class MagicEQSlopeUnavailable { None, Bypass, InvalidSampleRate, NarrowBand, FrequencyLimit };
struct MagicEQLocalSlopeSide {
    bool valid = false;
    float fromHz = 0.0f;
    float toHz = 0.0f;
    float dbPerOctave = 0.0f;
    MagicEQSlopeUnavailable reason = MagicEQSlopeUnavailable::None;
};
struct MagicEQLocalSlope {
    MagicEQLocalSlopeSide low;
    MagicEQLocalSlopeSide high;
};
// UI/diagnostic path only: exact steady-state coefficient response over a local
// interval on each edge. Never call from the audio callback.
MagicEQLocalSlope measureMagicEQLocalSlope(const MagicEQProcessParams& params, int sampleRate);

void magicEQEdges(float centerFreq, float halfWidthPercent, float& lowFreq, float& highFreq);
void resetMagicEQ(MagicEQState* state);
void prepareMagicEQ(MagicEQState* state, int sampleRate, const MagicEQProcessParams& params);
void processMagicEQ(float* interleaved, size_t numFrames, int channels, int sampleRate,
                    const MagicEQProcessParams& params, MagicEQState* state);

// @stage7-contract
// API contract: DSP helpers are pure coefficient calculations over caller-owned
// scalars/state. They preserve the documented k ranges and do not allocate,
// perform I/O, or retain references across calls.

// 旋钮 k ∈ [-1, 0] 时有效。输出三频段「居中量」0~1（0=不改，1=该段变单声道）。
// LF 20–80Hz、Mid 80–1000Hz、HF 1000–20kHz。
// 当 k > 0 时 amount* 置 0（拓宽侧不用 amount）。
void getCenterSenseAmounts(float k, float& amountLF, float& amountMid, float& amountHF);

// 旋钮 k ∈ [0, +1] 时有效。输出三频段「宽度系数」≥1（1=原样，越大越宽）。
// 当 k < 0 时 width* 置 1（收紧侧不用 width）。
void getCenterSenseWidthFactors(float k, float& widthLF, float& widthMid, float& widthHF);

// 分频 + 每段收紧/拓宽的滤波器状态（一阶 80Hz/1000Hz crossover，每通道各 2 个极点）
struct ProcessState {
    float lp80L = 0.0f, lp80R = 0.0f;
    float lp1000L = 0.0f, lp1000R = 0.0f;
};

// In-place 处理立体声交错 L,R,L,R,...；k==0 直接 return；state 非空且 k!=0 时使用并更新。
void processCenterSense(float* interleavedLR, size_t numFrames, int sampleRate, float k, ProcessState* state);

// Bass Mono（低频单声道折叠）：cutoffHz 以下频率变为 (L+R)/2，高频保持原始立体声。
// cutoffHz <= 0 时 no-op（功能关闭）。state 必须非空且跨调用持久保持。
struct BassMonoState {
    float lp_L = 0.0f;
    float lp_R = 0.0f;
};

void processBassMono(float* interleavedLR, size_t numFrames, int sampleRate, float cutoffHz, BassMonoState* state);

// Frequency Shifter (SSB + FIR Hilbert)
// Linear frequency shift: all frequencies moved by +shiftHz (breaks harmonic relationships).
// Uses a 63-tap Type-III FIR Hilbert transformer (Hann-windowed) + single-sideband AM modulation.
// shiftHz == 0 is a no-op. state must be persisted across calls and zeroed on seek/file-change.
struct FreqShiftState {
    static constexpr int kTaps = 63;
    float delayL[kTaps] = {};  // Hilbert delay line, left channel
    float delayR[kTaps] = {};  // Hilbert delay line, right channel
    float phase = 0.0f;        // SSB oscillator phase accumulator [0, 1)
    int   pos   = 0;           // Circular buffer write position
};

// In-place stereo interleaved L,R,L,R,... processing.
// shiftHz: positive = shift up, negative = shift down.
void processFreqShift(float* interleavedLR, size_t numFrames, int sampleRate,
                      float shiftHz, FreqShiftState* state);

// In-place mono processing for single-channel playback sources.
void processFreqShiftMono(float* mono, size_t numFrames, int sampleRate,
                          float shiftHz, FreqShiftState* state);

} // namespace CenterSenseDsp
