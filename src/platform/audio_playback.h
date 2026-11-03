#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace Platform::AudioPlayback {

// @stage7-contract
// API contract: Borrowed* capabilities never own or destroy the native object;
// callers must keep the owner alive and must treat invalid capabilities as
// no-ops/failures. Engine and Sound own their backend state, are move-only, and
// require serialized access to one instance. BackendResult preserves the
// backend status code while helpers document legacy fallback values below.

using FrameIndex = std::uint64_t;

class Engine;

namespace Detail { struct Access; }

class BorrowedEngine final {
public:
    bool IsValid() const noexcept { return native_ != nullptr; }
private:
    explicit BorrowedEngine(void* native) noexcept : native_(native) {}
    void* native_ = nullptr;
    friend struct Detail::Access;
    friend BorrowedEngine BorrowEngine(void*) noexcept;
};

class BorrowedDataSource final {
public:
    bool IsValid() const noexcept { return native_ != nullptr; }
private:
    explicit BorrowedDataSource(void* native) noexcept : native_(native) {}
    void* native_ = nullptr;
    friend struct Detail::Access;
    friend BorrowedDataSource BorrowDataSource(void*) noexcept;
};

class SoundStorage final {
public:
    bool IsValid() const noexcept { return native_ != nullptr; }
private:
    explicit SoundStorage(void* native) noexcept : native_(native) {}
    void* native_ = nullptr;
    friend struct Detail::Access;
    friend SoundStorage UseSoundStorage(void*) noexcept;
};

class BorrowedSound final {
public:
    bool IsValid() const noexcept { return native_ != nullptr; }

private:
    explicit BorrowedSound(void* native) noexcept : native_(native) {}
    void* native_ = nullptr;
    friend struct Detail::Access;
    friend BorrowedSound BorrowSound(void*) noexcept;
};

// The returned capability never owns or uninitializes the playback sound.
BorrowedSound BorrowSound(void* sound) noexcept;
BorrowedEngine BorrowEngine(void* engine) noexcept;
BorrowedDataSource BorrowDataSource(void* dataSource) noexcept;
SoundStorage UseSoundStorage(void* soundStorage) noexcept;

enum class SoundInitFlags : std::uint8_t {
    None = 0,
    NoSpatialization = 1 << 0,
    NoPitch = 1 << 1,
};

constexpr SoundInitFlags operator|(SoundInitFlags left, SoundInitFlags right) noexcept {
    return static_cast<SoundInitFlags>(
        static_cast<std::uint8_t>(left) | static_cast<std::uint8_t>(right));
}

struct BackendResult {
    int backendCode = -1;
    bool Succeeded() const noexcept { return backendCode == 0; }
};

struct PlaybackDeviceMetrics {
    std::uint32_t sampleRate = 0;
    std::uint32_t periodFrames = 0;
    std::uint32_t periods = 0;
    bool IsValid() const noexcept { return sampleRate != 0 && periodFrames != 0 && periods != 0; }
};

struct OutputDeviceDescriptor {
    std::string name;
    bool isDefault = false;
    std::vector<std::uint8_t> deviceIdBlob;
};

struct EngineInitOptions {
    std::uint32_t channels = 0;
    std::uint32_t sampleRate = 0;
    std::uint32_t periodSizeMs = 0;
    std::vector<std::uint8_t> playbackDeviceIdBlob;
};

#if AURA_BUILD_DIAGNOSTICS && ARIS_PREVIEW_ISOLATION && !AURA_INTERNAL_BETA && !defined(AURO_AUDIO_PLAYBACK_TESTING)
// QA-only observation of samples submitted by this engine's real device callback.
// Counts are monotonic until the engine is reinitialized. No capture or loopback.
struct OutputProbeSnapshot {
    std::uint64_t callbacks = 0;
    std::uint64_t frames = 0;
    std::uint64_t nonzeroSamples = 0;
    float maxAmplitude = 0;
    std::uint32_t channels = 0;
    std::uint32_t sampleRate = 0;
    bool deviceStarted = false;
    std::string backend;
    std::string deviceName;
};
#endif

class Engine final {
public:
    Engine() noexcept;
    ~Engine();

    Engine(Engine&&) noexcept;
    Engine& operator=(Engine&&) noexcept;

    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    BackendResult Initialize(const EngineInitOptions& options);
    BorrowedEngine Borrow() noexcept;
    bool IsInitialized() const noexcept;
    BackendResult StopDevice() noexcept;
#if AURA_BUILD_DIAGNOSTICS && ARIS_PREVIEW_ISOLATION && !AURA_INTERNAL_BETA && !defined(AURO_AUDIO_PLAYBACK_TESTING)
    OutputProbeSnapshot QueryOutputProbe() const;
#endif
    void Uninitialize() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

class Sound final {
public:
    Sound() noexcept;
    ~Sound();

    Sound(Sound&&) noexcept;
    Sound& operator=(Sound&&) noexcept;

    Sound(const Sound&) = delete;
    Sound& operator=(const Sound&) = delete;

    BackendResult Initialize(
        BorrowedEngine engine,
        BorrowedDataSource dataSource,
        SoundInitFlags flags);
    BorrowedSound Borrow() noexcept;
    bool IsInitialized() const noexcept;
    void Uninitialize() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

BackendResult InitializeFromDataSource(
    BorrowedEngine engine,
    BorrowedDataSource dataSource,
    SoundStorage soundStorage,
    SoundInitFlags flags) noexcept;
std::uint32_t QueryEngineSampleRate(BorrowedEngine engine) noexcept;
FrameIndex QueryEngineTimeInPcmFrames(BorrowedEngine engine) noexcept;
PlaybackDeviceMetrics QueryPlaybackDeviceMetrics(BorrowedEngine engine) noexcept;
std::vector<OutputDeviceDescriptor> EnumerateOutputDevices(BorrowedEngine engine);
BackendResult InitializeEngine(BorrowedEngine engine, const EngineInitOptions& options) noexcept;
BackendResult StopEngineDevice(BorrowedEngine engine) noexcept;
void UninitializeEngine(BorrowedEngine engine) noexcept;

// Preserves the legacy playback behavior by returning frame zero when the
// backend cannot report a cursor.
FrameIndex QueryCursor(BorrowedSound sound) noexcept;

// Invalid sound capabilities are never playing.
bool IsPlaying(BorrowedSound sound) noexcept;

// Preserves the backend value domain; policy clamping remains with callers.
void SetEngineVolume(BorrowedEngine engine, float volume) noexcept;
void SetPan(BorrowedSound sound, float pan) noexcept;
void SetPitch(BorrowedSound sound, float pitch) noexcept;
void SetVolume(BorrowedSound sound, float volume) noexcept;
void SetFadeInMilliseconds(
    BorrowedSound sound,
    float volumeBeg,
    float volumeEnd,
    std::uint64_t durationMs) noexcept;
void SetFadeInPcmFrames(
    BorrowedSound sound,
    float volumeBeg,
    float volumeEnd,
    std::uint64_t durationFrames) noexcept;
void SetStopTimeInPcmFrames(BorrowedSound sound, FrameIndex frame) noexcept;
void ResetStopTimeAndFade(BorrowedSound sound) noexcept;
void UninitializeSound(BorrowedSound sound) noexcept;
BackendResult Start(BorrowedSound sound) noexcept;
BackendResult Stop(BorrowedSound sound) noexcept;
BackendResult SeekToFrame(BorrowedSound sound, FrameIndex frame) noexcept;

} // namespace Platform::AudioPlayback
