#include "audio_file.h"
#include "text_encoding.h"
#include "text_encoding.inl"

#include "../../thirdparty/miniaudio.h"
#include "wavpack_decoding_backend.h"
#ifdef AURO_AUDIO_FILE_TESTING
#include "audio_file_miniaudio_test_seam.h"
#endif

#include <utility>
#include <algorithm>
#include <cwctype>

namespace Platform::AudioFile {

void ConvertFloatToPackedS24(
    std::uint8_t* destination,
    const float* source,
    std::uint64_t sampleCount) noexcept {
    ma_pcm_f32_to_s24(
        destination, source, static_cast<ma_uint64>(sampleCount), ma_dither_mode_none);
}

struct LinearFloatResampler::Impl {
    ma_resampler resampler{};
    bool initialized = false;
    ~Impl() {
        if (initialized) ma_resampler_uninit(&resampler, nullptr);
    }
};

LinearFloatResampler::LinearFloatResampler() noexcept = default;
LinearFloatResampler::~LinearFloatResampler() = default;
LinearFloatResampler::LinearFloatResampler(LinearFloatResampler&&) noexcept = default;
LinearFloatResampler& LinearFloatResampler::operator=(LinearFloatResampler&&) noexcept = default;

bool LinearFloatResampler::Open(
    std::uint32_t channels,
    std::uint32_t inputRate,
    std::uint32_t outputRate) {
    Close();
    if (channels == 0 || inputRate == 0 || outputRate == 0) return false;
    auto next = std::make_unique<Impl>();
    auto config = ma_resampler_config_init(
        ma_format_f32, static_cast<ma_uint32>(channels),
        static_cast<ma_uint32>(inputRate), static_cast<ma_uint32>(outputRate),
        ma_resample_algorithm_linear);
    config.linear.lpfOrder = MA_MAX_FILTER_ORDER;
    if (ma_resampler_init(&config, nullptr, &next->resampler) != MA_SUCCESS) return false;
    next->initialized = true;
    impl_ = std::move(next);
    return true;
}

ResampleProgress LinearFloatResampler::Process(
    const float* input,
    FrameCount inputAvailableFrames,
    float* output,
    FrameCount outputCapacityFrames) noexcept {
    if (!impl_ || !impl_->initialized || !output || outputCapacityFrames == 0 ||
        (!input && inputAvailableFrames != 0)) {
        return {};
    }
    ma_uint64 inputFrames = static_cast<ma_uint64>(inputAvailableFrames);
    ma_uint64 outputFrames = static_cast<ma_uint64>(outputCapacityFrames);
    const ma_result result = ma_resampler_process_pcm_frames(
        &impl_->resampler, input, &inputFrames, output, &outputFrames);
    return {
        result == MA_SUCCESS,
        static_cast<FrameCount>(inputFrames),
        static_cast<FrameCount>(outputFrames),
    };
}

ResampleProgress LinearFloatResampler::Drain(
    float* output,
    FrameCount outputCapacityFrames) noexcept {
    return Process(nullptr, 0, output, outputCapacityFrames);
}

bool LinearFloatResampler::IsOpen() const noexcept {
    return impl_ != nullptr && impl_->initialized;
}

void LinearFloatResampler::Close() noexcept { impl_.reset(); }

struct MonoFloatReader::Impl {
    ma_decoder decoder{};
    bool initialized = false;

    ~Impl() {
        if (!initialized) return;
#ifdef AURO_AUDIO_FILE_TESTING
        TestSeam::Uninit(&decoder);
#else
        ma_decoder_uninit(&decoder);
#endif
    }
};

MonoFloatReader::MonoFloatReader() noexcept = default;
MonoFloatReader::~MonoFloatReader() = default;
MonoFloatReader::MonoFloatReader(MonoFloatReader&&) noexcept = default;
MonoFloatReader& MonoFloatReader::operator=(MonoFloatReader&&) noexcept = default;

Status MonoFloatReader::Open(const std::string& utf8Path,
                             LegacyWindowsPathPolicy pathPolicy,
                             MonoSampleRate sampleRate) {
    Close();
    auto next = std::make_unique<Impl>();
    auto config = ma_decoder_config_init(
        ma_format_f32, 1, static_cast<ma_uint32>(sampleRate));
    Platform::WavPack::ApplyDecodingBackend(config);
    ma_result result = MA_ERROR;
#ifdef _WIN32
    std::wstring widePath;
    if (pathPolicy == LegacyWindowsPathPolicy::FeatureLegacyUtf8VerbatimPrefix) {
        widePath = Platform::TextEncoding::Utf8ToWide(utf8Path);
    } else {
        widePath = Platform::TextEncoding::Utf8ToWidePermissive(utf8Path);
    }
    auto initWide = [&](const std::wstring& candidate) {
#ifdef AURO_AUDIO_FILE_TESTING
        return TestSeam::InitFileW(candidate.c_str(), &config, &next->decoder);
#else
        return ma_decoder_init_file_w(candidate.c_str(), &config, &next->decoder);
#endif
    };
    result = initWide(widePath);
    if (result != MA_SUCCESS) {
        if (pathPolicy == LegacyWindowsPathPolicy::FeatureLegacyUtf8VerbatimPrefix) {
            result = initWide(L"\\\\?\\" + widePath);
        } else {
            std::wstring normalized = widePath;
            std::replace(normalized.begin(), normalized.end(), L'/', L'\\');
            if (normalized.rfind(L"\\\\?\\", 0) != 0) {
                if (normalized.rfind(L"\\\\", 0) == 0)
                    normalized = L"\\\\?\\UNC\\" + normalized.substr(2);
                else if (normalized.size() >= 3 && std::iswalpha(normalized[0]) && normalized[1] == L':')
                    normalized = L"\\\\?\\" + normalized;
                else
                    normalized.clear();
                if (!normalized.empty()) result = initWide(normalized);
            }
        }
    }
#else
    (void)pathPolicy;
    #ifdef AURO_AUDIO_FILE_TESTING
    result = TestSeam::InitFile(utf8Path.c_str(), &config, &next->decoder);
    #else
    result = ma_decoder_init_file(utf8Path.c_str(), &config, &next->decoder);
    #endif
#endif
    if (result != MA_SUCCESS) {
        return Status::OpenFailed;
    }
    next->initialized = true;
    impl_ = std::move(next);
    return Status::Ok;
}

ReadResult MonoFloatReader::Read(float* destination, FrameCount requestedFrames,
                                 const std::atomic<bool>* cancel) {
    if (requestedFrames == 0) return {Status::Ok, 0};
    if (!impl_ || !impl_->initialized || !destination)
        return {Status::InvalidArgument, 0};
    if (cancel && cancel->load(std::memory_order_acquire))
        return {Status::Cancelled, 0};
    ma_uint64 framesRead = 0;
#ifdef AURO_AUDIO_FILE_TESTING
    const ma_result result = TestSeam::ReadFrames(
        &impl_->decoder, destination, static_cast<ma_uint64>(requestedFrames), &framesRead);
#else
    const ma_result result = ma_decoder_read_pcm_frames(
        &impl_->decoder, destination, static_cast<ma_uint64>(requestedFrames), &framesRead);
#endif
    if (result == MA_SUCCESS)
        return {framesRead > 0 ? Status::Ok : Status::EndOfFile,
                static_cast<FrameCount>(framesRead)};
    if (result == MA_AT_END)
        return {Status::EndOfFile, static_cast<FrameCount>(framesRead)};
    return {Status::DecodeFailed, static_cast<FrameCount>(framesRead)};
}

Status MonoFloatReader::Seek(FrameCount absoluteOutputFrame) {
    if (!impl_ || !impl_->initialized) return Status::SeekFailed;
#ifdef AURO_AUDIO_FILE_TESTING
    const ma_result result = TestSeam::SeekFrame(
        &impl_->decoder, static_cast<ma_uint64>(absoluteOutputFrame));
#else
    const ma_result result = ma_decoder_seek_to_pcm_frame(
        &impl_->decoder, static_cast<ma_uint64>(absoluteOutputFrame));
#endif
    return result == MA_SUCCESS ? Status::Ok : Status::SeekFailed;
}

bool MonoFloatReader::IsOpen() const noexcept { return impl_ != nullptr; }
void MonoFloatReader::Close() noexcept { impl_.reset(); }

struct FloatWavWriter::Impl {
    ma_encoder encoder{};
    bool initialized = false;

    ~Impl() {
        if (initialized) ma_encoder_uninit(&encoder);
    }
};

FloatWavWriter::FloatWavWriter() noexcept = default;
FloatWavWriter::~FloatWavWriter() = default;
FloatWavWriter::FloatWavWriter(FloatWavWriter&&) noexcept = default;
FloatWavWriter& FloatWavWriter::operator=(FloatWavWriter&&) noexcept = default;

bool FloatWavWriter::Open(const std::string& utf8Path, int sampleRate, int channels) {
    Close();
    if (utf8Path.empty() || sampleRate <= 0 || channels <= 0) return false;

    auto next = std::make_unique<Impl>();
    const auto config = ma_encoder_config_init(
        ma_encoding_format_wav, ma_format_f32,
        static_cast<ma_uint32>(channels), static_cast<ma_uint32>(sampleRate));
    ma_result result = MA_ERROR;
#ifdef _WIN32
    const std::wstring widePath = Platform::TextEncoding::Utf8ToWidePermissive(utf8Path);
    result = ma_encoder_init_file_w(widePath.c_str(), &config, &next->encoder);
#else
    result = ma_encoder_init_file(utf8Path.c_str(), &config, &next->encoder);
#endif
    if (result != MA_SUCCESS) return false;

    next->initialized = true;
    impl_ = std::move(next);
    return true;
}

bool FloatWavWriter::Write(const float* source, FrameCount frames, FrameCount* framesWritten) {
    if (framesWritten) *framesWritten = 0;
    if (!impl_ || !impl_->initialized || !source || frames == 0) return false;
    ma_uint64 written = 0;
    const ma_result result = ma_encoder_write_pcm_frames(
        &impl_->encoder, source, static_cast<ma_uint64>(frames), &written);
    if (framesWritten) *framesWritten = static_cast<FrameCount>(written);
    return result == MA_SUCCESS;
}

bool FloatWavWriter::IsOpen() const noexcept {
    return impl_ != nullptr && impl_->initialized;
}

void FloatWavWriter::Close() noexcept { impl_.reset(); }

struct S24WavWriter::Impl {
    ma_encoder encoder{};
    bool initialized = false;
    ~Impl() { if (initialized) ma_encoder_uninit(&encoder); }
};

S24WavWriter::S24WavWriter() noexcept = default;
S24WavWriter::~S24WavWriter() = default;
S24WavWriter::S24WavWriter(S24WavWriter&&) noexcept = default;
S24WavWriter& S24WavWriter::operator=(S24WavWriter&&) noexcept = default;

bool S24WavWriter::Open(const std::string& utf8Path, int sampleRate, int channels) {
    Close();
    if (utf8Path.empty() || sampleRate <= 0 || channels <= 0) return false;
    auto next = std::make_unique<Impl>();
    const auto config = ma_encoder_config_init(
        ma_encoding_format_wav, ma_format_s24,
        static_cast<ma_uint32>(channels), static_cast<ma_uint32>(sampleRate));
    ma_result result = MA_ERROR;
#ifdef _WIN32
    const std::wstring widePath = Platform::TextEncoding::Utf8ToWidePermissive(utf8Path);
    result = ma_encoder_init_file_w(widePath.c_str(), &config, &next->encoder);
#else
    result = ma_encoder_init_file(utf8Path.c_str(), &config, &next->encoder);
#endif
    if (result != MA_SUCCESS) return false;
    next->initialized = true;
    impl_ = std::move(next);
    return true;
}

bool S24WavWriter::Write(const std::uint8_t* source, FrameCount frames, FrameCount* framesWritten) {
    if (framesWritten) *framesWritten = 0;
    if (!impl_ || !impl_->initialized || !source || frames == 0) return false;
    ma_uint64 written = 0;
    const ma_result result = ma_encoder_write_pcm_frames(
        &impl_->encoder, source, static_cast<ma_uint64>(frames), &written);
    if (framesWritten) *framesWritten = static_cast<FrameCount>(written);
    return result == MA_SUCCESS;
}

bool S24WavWriter::IsOpen() const noexcept {
    return impl_ != nullptr && impl_->initialized;
}

void S24WavWriter::Close() noexcept { impl_.reset(); }

} // namespace Platform::AudioFile
