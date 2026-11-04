#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
namespace Platform::AudioFile {

// @stage7-contract
// API contract: UTF-8 paths are accepted by file readers and writers. Decoder
// and writer objects own their backend state, are non-copyable, and may be
// moved only while not concurrently accessed. Open establishes a usable
// session; failed Open/Process/Read operations leave the object valid for
// Close and report failure through Status or the result structure.
using FrameCount = std::uint64_t;
void ConvertFloatToPackedS24(
    std::uint8_t* destination,
    const float* source,
    std::uint64_t sampleCount) noexcept;
struct ResampleProgress {
    bool backendSucceeded = false;
    FrameCount inputConsumed = 0;
    FrameCount outputProduced = 0;
};
class LinearFloatResampler final {
public:
    LinearFloatResampler() noexcept;
    ~LinearFloatResampler();
    LinearFloatResampler(LinearFloatResampler&&) noexcept;
    LinearFloatResampler& operator=(LinearFloatResampler&&) noexcept;
    LinearFloatResampler(const LinearFloatResampler&) = delete;
    LinearFloatResampler& operator=(const LinearFloatResampler&) = delete;
    bool Open(std::uint32_t channels, std::uint32_t inputRate, std::uint32_t outputRate);
    ResampleProgress Process(
        const float* input,
        FrameCount inputAvailableFrames,
        float* output,
        FrameCount outputCapacityFrames) noexcept;
    ResampleProgress Drain(float* output, FrameCount outputCapacityFrames) noexcept;
    bool IsOpen() const noexcept;
    void Close() noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
enum class Status {
    Ok, EndOfFile, Cancelled, InvalidArgument,
    OpenFailed, SeekFailed, DecodeFailed,
};
enum class LegacyWindowsPathPolicy {
    FeatureLegacyUtf8VerbatimPrefix, KeyPermissiveUtf8CanonicalPrefix,
};
enum class MonoSampleRate : std::uint32_t {
    Hz16000 = 16000,
    Hz22050 = 22050,
    Hz44100 = 44100,
    Hz48000 = 48000,
};
struct ReadResult { Status status; FrameCount framesRead; };
class MonoFloatReader final {
public:
    MonoFloatReader() noexcept;
    ~MonoFloatReader();
    MonoFloatReader(MonoFloatReader&&) noexcept;
    MonoFloatReader& operator=(MonoFloatReader&&) noexcept;
    MonoFloatReader(const MonoFloatReader&) = delete;
    MonoFloatReader& operator=(const MonoFloatReader&) = delete;
    Status Open(const std::string& utf8Path, LegacyWindowsPathPolicy pathPolicy,
                MonoSampleRate sampleRate = MonoSampleRate::Hz22050);
    ReadResult Read(float* destination, FrameCount requestedFrames,
                    const std::atomic<bool>* cancel = nullptr);
    Status Seek(FrameCount absoluteOutputFrame);
    bool IsOpen() const noexcept;
    void Close() noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

class FloatWavWriter final {
public:
    FloatWavWriter() noexcept;
    ~FloatWavWriter();
    FloatWavWriter(FloatWavWriter&&) noexcept;
    FloatWavWriter& operator=(FloatWavWriter&&) noexcept;
    FloatWavWriter(const FloatWavWriter&) = delete;
    FloatWavWriter& operator=(const FloatWavWriter&) = delete;
    bool Open(const std::string& utf8Path, int sampleRate, int channels);
    bool Write(const float* source, FrameCount frames, FrameCount* framesWritten = nullptr);
    bool IsOpen() const noexcept;
    void Close() noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

class S24WavWriter final {
public:
    S24WavWriter() noexcept;
    ~S24WavWriter();
    S24WavWriter(S24WavWriter&&) noexcept;
    S24WavWriter& operator=(S24WavWriter&&) noexcept;
    S24WavWriter(const S24WavWriter&) = delete;
    S24WavWriter& operator=(const S24WavWriter&) = delete;
    bool Open(const std::string& utf8Path, int sampleRate, int channels);
    bool Write(const std::uint8_t* source, FrameCount frames, FrameCount* framesWritten = nullptr);
    bool IsOpen() const noexcept;
    void Close() noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace Platform::AudioFile
