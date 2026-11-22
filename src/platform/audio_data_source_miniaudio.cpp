#include "audio_data_source.h"

#include "../../thirdparty/miniaudio.h"
#include "text_encoding.h"
#include "wavpack_decoding_backend.h"

#include <algorithm>
#include <cwctype>
#include <string>
#include <utility>

namespace Platform::AudioDataSource {

struct Decoder::Impl {
    ma_decoder value{};
    bool initialized = false;
};

struct PcmBuffer::Impl {
    ma_audio_buffer value{};
    bool initialized = false;
};

struct TransformSource::Impl {
    ma_data_source_base base{};
    ma_data_source* borrowed = nullptr;
    TransformCallbacks callbacks{};
};

namespace Detail {
struct Access {
    static ma_data_source* Native(BorrowedSource source) {
        return static_cast<ma_data_source*>(source.native_);
    }
    static ma_decoder* Native(BorrowedDecoder decoder) {
        return static_cast<ma_decoder*>(decoder.native_);
    }
    static ma_decoder* Native(Decoder& decoder) {
        return decoder.impl_ ? &decoder.impl_->value : nullptr;
    }
    static ma_audio_buffer* Native(PcmBuffer& buffer) {
        return buffer.impl_ ? &buffer.impl_->value : nullptr;
    }
    static void SetInitialized(Decoder& decoder, bool initialized) {
        if (decoder.impl_) decoder.impl_->initialized = initialized;
    }
    static Reader MakeReader(ma_data_source* source) { return Reader(source); }
    static TransformSource::Impl* Self(ma_data_source* source) {
        return reinterpret_cast<TransformSource::Impl*>(source);
    }
};
} // namespace Detail

Decoder::Decoder() : impl_(std::make_unique<Impl>()) {}
Decoder::~Decoder() { UninitializeDecoder(*this); }
Decoder::Decoder(Decoder&&) noexcept = default;
Decoder& Decoder::operator=(Decoder&&) noexcept = default;
bool Decoder::IsInitialized() const noexcept {
    return impl_ && impl_->initialized;
}
void* Decoder::NativeHandle() noexcept {
    return impl_ ? static_cast<void*>(&impl_->value) : nullptr;
}

PcmBuffer::PcmBuffer() : impl_(std::make_unique<Impl>()) {}
PcmBuffer::~PcmBuffer() { UninitializePcmBuffer(*this); }
PcmBuffer::PcmBuffer(PcmBuffer&&) noexcept = default;
PcmBuffer& PcmBuffer::operator=(PcmBuffer&& other) noexcept {
    if (this == &other) return *this;
    UninitializePcmBuffer(*this);
    impl_ = std::move(other.impl_);
    return *this;
}
bool PcmBuffer::IsInitialized() const noexcept {
    return impl_ && impl_->initialized;
}
void* PcmBuffer::NativeHandle() noexcept {
    return impl_ ? static_cast<void*>(&impl_->value) : nullptr;
}

namespace {
#ifdef _WIN32
std::wstring ToWinLongPathIfNeeded(const std::wstring& path) {
    if (path.empty() || path.rfind(L"\\\\?\\", 0) == 0) return path;
    std::wstring normalized = path;
    std::replace(normalized.begin(), normalized.end(), L'/', L'\\');
    if (normalized.rfind(L"\\\\", 0) == 0) return L"\\\\?\\UNC\\" + normalized.substr(2);
    if (normalized.size() >= 3 && std::iswalpha(normalized[0]) && normalized[1] == L':')
        return L"\\\\?\\" + normalized;
    return normalized;
}
#endif

ma_result OnRead(ma_data_source* source, void* output, ma_uint64 requested,
                 ma_uint64* framesRead) {
    auto* self = Detail::Access::Self(source);
    const auto result = self->callbacks.read(
        self->callbacks.context, Detail::Access::MakeReader(self->borrowed),
        static_cast<float*>(output), requested);
    if (framesRead) *framesRead = result.framesRead;
    return static_cast<ma_result>(result.backendCode);
}

ma_result OnSeek(ma_data_source* source, ma_uint64 frame) {
    auto* self = Detail::Access::Self(source);
    const ma_result result = ma_data_source_seek_to_pcm_frame(self->borrowed, frame);
    if (result == MA_SUCCESS && self->callbacks.seekSucceeded)
        self->callbacks.seekSucceeded(self->callbacks.context);
    return result;
}

ma_result OnFormat(ma_data_source* source, ma_format* format, ma_uint32* channels,
                   ma_uint32* sampleRate, ma_channel* channelMap, size_t mapCapacity) {
    return ma_data_source_get_data_format(Detail::Access::Self(source)->borrowed, format, channels,
                                          sampleRate, channelMap, mapCapacity);
}

ma_result OnCursor(ma_data_source* source, ma_uint64* cursor) {
    return ma_data_source_get_cursor_in_pcm_frames(Detail::Access::Self(source)->borrowed, cursor);
}

ma_result OnLength(ma_data_source* source, ma_uint64* length) {
    return ma_data_source_get_length_in_pcm_frames(Detail::Access::Self(source)->borrowed, length);
}

ma_result OnLooping(ma_data_source* source, ma_bool32 looping) {
    return ma_data_source_set_looping(Detail::Access::Self(source)->borrowed, looping);
}

const ma_data_source_vtable kVtable{
    OnRead, OnSeek, OnFormat, OnCursor, OnLength, OnLooping, 0};
} // namespace

BorrowedSource BorrowDataSource(void* source) noexcept {
    return BorrowedSource(source);
}

BorrowedDecoder BorrowDecoder(void* decoder) noexcept {
    return BorrowedDecoder(decoder);
}

BorrowedSource BorrowDataSource(Decoder& decoder) noexcept {
    return BorrowedSource(Detail::Access::Native(decoder));
}

BorrowedSource BorrowDataSource(PcmBuffer& buffer) noexcept {
    return BorrowedSource(buffer.impl_ ? &buffer.impl_->value : nullptr);
}

BorrowedDecoder BorrowDecoder(Decoder& decoder) noexcept {
    return BorrowedDecoder(Detail::Access::Native(decoder));
}

DataFormat QueryDataFormat(BorrowedSource source) noexcept {
    if (!source.IsValid()) return {};
    ma_uint32 channels = 0;
    ma_uint32 sampleRate = 0;
    const ma_result result = ma_data_source_get_data_format(
        Detail::Access::Native(source), nullptr, &channels, &sampleRate, nullptr, 0);
    return {static_cast<int>(result), channels, sampleRate};
}

CursorPosition QueryCursor(BorrowedSource source) noexcept {
    if (!source.IsValid()) return {};
    ma_uint64 cursor = 0;
    const ma_result result = ma_data_source_get_cursor_in_pcm_frames(
        Detail::Access::Native(source), &cursor);
    return {static_cast<int>(result), static_cast<FrameCount>(cursor)};
}

#ifndef AURO_TEST_EXTERNAL_QUERY_LENGTH
LengthInFrames QueryLength(BorrowedSource source) noexcept {
    if (!source.IsValid()) return {};
    ma_uint64 length = 0;
    const ma_result result = ma_data_source_get_length_in_pcm_frames(
        Detail::Access::Native(source), &length);
    return {static_cast<int>(result), static_cast<FrameCount>(length)};
}
#endif

ReadResult ReadPcmFrames(
    BorrowedSource source,
    float* output,
    FrameCount requestedFrames) noexcept {
    if (!source.IsValid()) return {-1, 0};
    ma_uint64 framesRead = 0;
    const ma_result result = ma_data_source_read_pcm_frames(
        Detail::Access::Native(source), output, requestedFrames, &framesRead);
    return {static_cast<int>(result), static_cast<FrameCount>(framesRead)};
}

OperationResult SeekPcmFrame(BorrowedSource source, FrameCount frame) noexcept {
    if (!source.IsValid()) return {};
    const ma_result result = ma_data_source_seek_to_pcm_frame(
        Detail::Access::Native(source), frame);
    return {static_cast<int>(result)};
}

OperationResult InitializeDecoderFromFile(
    void* decoderStorage, const std::string& pathUtf8) noexcept {
    if (!decoderStorage || pathUtf8.empty()) return {};
    auto* decoder = static_cast<ma_decoder*>(decoderStorage);
    ma_decoder_config config = ma_decoder_config_init(ma_format_f32, 0, 0);
    Platform::WavPack::ApplyDecodingBackend(config);
    ma_result result = MA_ERROR;
#ifdef _WIN32
    const std::wstring widePath = Platform::TextEncoding::Utf8ToWidePermissive(pathUtf8);
    result = ma_decoder_init_file_w(widePath.c_str(), &config, decoder);
    if (result != MA_SUCCESS) {
        const std::wstring longPath = ToWinLongPathIfNeeded(widePath);
        if (longPath != widePath)
            result = ma_decoder_init_file_w(longPath.c_str(), &config, decoder);
    }
#else
    result = ma_decoder_init_file(pathUtf8.c_str(), &config, decoder);
#endif
    return {static_cast<int>(result)};
}

OperationResult InitializeDecoderFromFile(
    Decoder& decoder, const std::string& pathUtf8) noexcept {
    UninitializeDecoder(decoder);
    const auto result = InitializeDecoderFromFile(
        Detail::Access::Native(decoder), pathUtf8);
    if (result.Succeeded()) Detail::Access::SetInitialized(decoder, true);
    return result;
}

OperationResult InitializeDecoderFromFileWithFormat(
    void* decoderStorage,
    const std::string& pathUtf8,
    std::uint32_t channels,
    std::uint32_t sampleRate) noexcept {
    if (!decoderStorage || pathUtf8.empty()) return {};
    auto* decoder = static_cast<ma_decoder*>(decoderStorage);
    ma_decoder_config config = ma_decoder_config_init(
        ma_format_f32, channels, sampleRate);
    Platform::WavPack::ApplyDecodingBackend(config);
    ma_result result = MA_ERROR;
#ifdef _WIN32
    const std::wstring widePath = Platform::TextEncoding::Utf8ToWidePermissive(pathUtf8);
    result = ma_decoder_init_file_w(widePath.c_str(), &config, decoder);
    if (result != MA_SUCCESS) {
        const std::wstring longPath = ToWinLongPathIfNeeded(widePath);
        if (longPath != widePath)
            result = ma_decoder_init_file_w(longPath.c_str(), &config, decoder);
    }
#else
    result = ma_decoder_init_file(pathUtf8.c_str(), &config, decoder);
#endif
    return {static_cast<int>(result)};
}

OperationResult InitializeDecoderFromFileWithFormat(
    Decoder& decoder,
    const std::string& pathUtf8,
    std::uint32_t channels,
    std::uint32_t sampleRate) noexcept {
    UninitializeDecoder(decoder);
    const auto result = InitializeDecoderFromFileWithFormat(
        Detail::Access::Native(decoder), pathUtf8, channels, sampleRate);
    if (result.Succeeded()) Detail::Access::SetInitialized(decoder, true);
    return result;
}

OperationResult InitializeDecoderFromFileWithFormatAndChannelMap(
    void* decoderStorage,
    const std::string& pathUtf8,
    std::uint32_t channels,
    std::uint32_t sampleRate,
    int channelSoloMode) noexcept {
    if (!decoderStorage || pathUtf8.empty()) return {};
    auto* decoder = static_cast<ma_decoder*>(decoderStorage);
    ma_decoder_config config = ma_decoder_config_init(
        ma_format_f32, channels, sampleRate);
    Platform::WavPack::ApplyDecodingBackend(config);
    ma_channel channelMap[1] = {MA_CHANNEL_NONE};
    switch (channelSoloMode) {
    case 1: channelMap[0] = MA_CHANNEL_FRONT_LEFT; break;
    case 2: channelMap[0] = MA_CHANNEL_FRONT_RIGHT; break;
    case 3: channelMap[0] = MA_CHANNEL_FRONT_CENTER; break;
    case 4: channelMap[0] = MA_CHANNEL_LFE; break;
    case 5: channelMap[0] = MA_CHANNEL_SIDE_LEFT; break;
    case 6: channelMap[0] = MA_CHANNEL_SIDE_RIGHT; break;
    case 7: channelMap[0] = MA_CHANNEL_BACK_LEFT; break;
    case 8: channelMap[0] = MA_CHANNEL_BACK_RIGHT; break;
    default: break;
    }
    if (channelSoloMode != 0) {
        config.pChannelMap = channelMap;
    }
    ma_result result = MA_ERROR;
#ifdef _WIN32
    const std::wstring widePath = Platform::TextEncoding::Utf8ToWidePermissive(pathUtf8);
    result = ma_decoder_init_file_w(widePath.c_str(), &config, decoder);
    if (result != MA_SUCCESS) {
        const std::wstring longPath = ToWinLongPathIfNeeded(widePath);
        if (longPath != widePath)
            result = ma_decoder_init_file_w(longPath.c_str(), &config, decoder);
    }
#else
    result = ma_decoder_init_file(pathUtf8.c_str(), &config, decoder);
#endif
    return {static_cast<int>(result)};
}

OperationResult InitializeDecoderFromFileWithFormatAndChannelMap(
    Decoder& decoder,
    const std::string& pathUtf8,
    std::uint32_t channels,
    std::uint32_t sampleRate,
    int channelSoloMode) noexcept {
    UninitializeDecoder(decoder);
    const auto result = InitializeDecoderFromFileWithFormatAndChannelMap(
        Detail::Access::Native(decoder), pathUtf8, channels, sampleRate, channelSoloMode);
    if (result.Succeeded()) Detail::Access::SetInitialized(decoder, true);
    return result;
}

void UninitializeDecoder(BorrowedDecoder decoder) noexcept {
    if (!decoder.IsValid()) return;
    ma_decoder_uninit(Detail::Access::Native(decoder));
}

void UninitializeDecoder(Decoder& decoder) noexcept {
    if (!decoder.impl_ || !decoder.impl_->initialized) return;
    ma_decoder_uninit(&decoder.impl_->value);
    decoder.impl_->initialized = false;
}

OperationResult InitializePcmBuffer(
    PcmBuffer& buffer,
    std::uint32_t channels,
    FrameCount frameCount,
    const float* samples) noexcept {
    if (!buffer.impl_) return {-1};
    if (buffer.impl_->initialized) UninitializePcmBuffer(buffer);
    const auto result = InitializePcmBuffer(
        static_cast<void*>(&buffer.impl_->value), channels, frameCount, samples);
    buffer.impl_->initialized = result.Succeeded();
    return result;
}

OperationResult InitializePcmBuffer(
    void* bufferStorage,
    std::uint32_t channels,
    FrameCount frameCount,
    const float* samples) noexcept {
    if (!bufferStorage || channels == 0 || frameCount == 0 || !samples) return {};
    ma_audio_buffer_config bufferConfig = ma_audio_buffer_config_init(
        ma_format_f32,
        channels,
        frameCount,
        const_cast<float*>(samples),
        nullptr);
    const ma_result result = ma_audio_buffer_init(
        &bufferConfig,
        static_cast<ma_audio_buffer*>(bufferStorage));
    return {static_cast<int>(result)};
}

void UninitializePcmBuffer(void* bufferStorage) noexcept {
    if (!bufferStorage) return;
    ma_audio_buffer_uninit(static_cast<ma_audio_buffer*>(bufferStorage));
}

void UninitializePcmBuffer(PcmBuffer& buffer) noexcept {
    if (!buffer.impl_ || !buffer.impl_->initialized) return;
    UninitializePcmBuffer(static_cast<void*>(&buffer.impl_->value));
    buffer.impl_->initialized = false;
}

ReadResult Reader::Read(float* output, FrameCount requestedFrames) const {
    ma_uint64 framesRead = 0;
    const ma_result result = ma_data_source_read_pcm_frames(
        static_cast<ma_data_source*>(native_), output, requestedFrames, &framesRead);
    return {static_cast<int>(result), framesRead};
}

std::uint32_t Reader::Channels() const {
    ma_uint32 channels = 0;
    ma_data_source_get_data_format(static_cast<ma_data_source*>(native_), nullptr,
                                   &channels, nullptr, nullptr, 0);
    return channels;
}

TransformSource::TransformSource() noexcept = default;
TransformSource::~TransformSource() { Uninitialize(); }
TransformSource::TransformSource(TransformSource&&) noexcept = default;
TransformSource& TransformSource::operator=(TransformSource&& other) noexcept {
    if (this != &other) {
        Uninitialize();
        impl_ = std::move(other.impl_);
    }
    return *this;
}

bool TransformSource::Initialize(BorrowedSource source, TransformCallbacks callbacks) {
    if (impl_ || !source.IsValid() || !callbacks.read) return false;
    auto next = std::make_unique<Impl>();
    next->borrowed = Detail::Access::Native(source);
    next->callbacks = callbacks;
    auto config = ma_data_source_config_init();
    config.vtable = &kVtable;
    if (ma_data_source_init(&config, &next->base) != MA_SUCCESS) return false;
    impl_ = std::move(next);
    return true;
}

void TransformSource::Uninitialize() noexcept {
    if (!impl_) return;
    ma_data_source_uninit(&impl_->base);
    impl_.reset();
}

void* TransformSource::NativeHandle() noexcept {
    return impl_ ? &impl_->base : nullptr;
}

} // namespace Platform::AudioDataSource
