#ifdef _MSC_VER
#define _CRT_SECURE_NO_WARNINGS
#endif
#include "wavpack_decoding_backend.h"

#include "../../thirdparty/wavpack/include/wavpack.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <new>
#include <string>
#include <vector>

#ifdef _WIN32
#include <io.h>
#include <windows.h>
#else
#include <sys/stat.h>
#endif

namespace Platform::WavPack {
namespace {

struct FileHandle {
    std::FILE* file = nullptr;
};

int32_t ReadBytes(void* id, void* data, int32_t bcount) {
    auto* handle = static_cast<FileHandle*>(id);
    if (!handle || !handle->file || bcount <= 0) return 0;
    return static_cast<int32_t>(
        std::fread(data, 1, static_cast<size_t>(bcount), handle->file));
}

int32_t WriteBytes(void*, void*, int32_t) { return 0; }

int64_t GetPos(void* id) {
    auto* handle = static_cast<FileHandle*>(id);
    if (!handle || !handle->file) return 0;
#ifdef _WIN32
    return _ftelli64(handle->file);
#else
    return static_cast<int64_t>(std::ftell(handle->file));
#endif
}

int SetPosAbs(void* id, int64_t pos) {
    auto* handle = static_cast<FileHandle*>(id);
    if (!handle || !handle->file) return -1;
#ifdef _WIN32
    return _fseeki64(handle->file, pos, SEEK_SET);
#else
    return std::fseek(handle->file, static_cast<long>(pos), SEEK_SET);
#endif
}

int SetPosRel(void* id, int64_t delta, int mode) {
    auto* handle = static_cast<FileHandle*>(id);
    if (!handle || !handle->file) return -1;
#ifdef _WIN32
    return _fseeki64(handle->file, delta, mode);
#else
    return std::fseek(handle->file, static_cast<long>(delta), mode);
#endif
}

int PushBackByte(void* id, int c) {
    auto* handle = static_cast<FileHandle*>(id);
    if (!handle || !handle->file) return 0;
    (void)c;
#ifdef _WIN32
    return _fseeki64(handle->file, -1, SEEK_CUR) == 0;
#else
    return std::fseek(handle->file, -1, SEEK_CUR) == 0;
#endif
}

int64_t GetLength(void* id) {
    auto* handle = static_cast<FileHandle*>(id);
    if (!handle || !handle->file) return 0;
    const int64_t saved = GetPos(id);
#ifdef _WIN32
    if (_fseeki64(handle->file, 0, SEEK_END) != 0) return 0;
    const int64_t length = _ftelli64(handle->file);
    _fseeki64(handle->file, saved, SEEK_SET);
#else
    if (std::fseek(handle->file, 0, SEEK_END) != 0) return 0;
    const int64_t length = static_cast<int64_t>(std::ftell(handle->file));
    std::fseek(handle->file, static_cast<long>(saved), SEEK_SET);
#endif
    return length;
}

int CanSeek(void*) { return 1; }
int TruncateHere(void*) { return 0; }

int CloseHandle(void* id) {
    auto* handle = static_cast<FileHandle*>(id);
    if (!handle) return 0;
    if (handle->file) {
        std::fclose(handle->file);
        handle->file = nullptr;
    }
    return 1;
}

WavpackStreamReader64 MakeReader() {
    WavpackStreamReader64 reader{};
    reader.read_bytes = ReadBytes;
    reader.write_bytes = WriteBytes;
    reader.get_pos = GetPos;
    reader.set_pos_abs = SetPosAbs;
    reader.set_pos_rel = SetPosRel;
    reader.push_back_byte = PushBackByte;
    reader.get_length = GetLength;
    reader.can_seek = CanSeek;
    reader.truncate_here = TruncateHere;
    reader.close = CloseHandle;
    return reader;
}

std::FILE* OpenUtf8(const char* path) {
#ifdef _WIN32
    if (path == nullptr) return nullptr;
    const int wideLength = MultiByteToWideChar(CP_UTF8, 0, path, -1, nullptr, 0);
    if (wideLength <= 1) return nullptr;
    std::wstring wide(static_cast<size_t>(wideLength - 1), L'\0');
    if (MultiByteToWideChar(CP_UTF8, 0, path, -1, wide.data(), wideLength) == 0) {
        return nullptr;
    }
    return _wfopen(wide.c_str(), L"rb");
#else
    return std::fopen(path, "rb");
#endif
}

constexpr int kOpenFlags = OPEN_DSD_AS_PCM | OPEN_NORMALIZE;

using ChannelMap = std::array<ma_channel, MA_MAX_CHANNELS>;

ChannelMap BuildChannelMap(WavpackContext* context, ma_uint32 channels) {
    ChannelMap map{};
    if (channels == 1) {
        map.front() = MA_CHANNEL_MONO;
        return map;
    }

    std::array<unsigned char, MA_MAX_CHANNELS + 1> identities{};
    WavpackGetChannelIdentities(context, identities.data());
    constexpr unsigned char kLastMicrosoftSpeaker = 18;
    static_assert(MA_CHANNEL_TOP_BACK_RIGHT - MA_CHANNEL_FRONT_LEFT + 1 == kLastMicrosoftSpeaker);
    for (ma_uint32 index = 0; index < channels; ++index) {
        const auto speaker = identities[index];
        if (speaker >= 1 && speaker <= kLastMicrosoftSpeaker) {
            map[index] = static_cast<ma_channel>(MA_CHANNEL_FRONT_LEFT + speaker - 1);
        }
    }

    ChannelMap fallback{};
    ma_channel_map_init_standard(ma_standard_channel_map_default, fallback.data(), fallback.size(), channels);
    const auto end = map.begin() + channels;
    for (auto entry = map.begin(); entry != end; ++entry) {
        if (*entry != MA_CHANNEL_NONE) continue;
        const auto unused = std::find_if(fallback.begin(), fallback.begin() + channels, [&](ma_channel speaker) {
            return speaker != MA_CHANNEL_NONE && std::find(map.begin(), end, speaker) == end;
        });
        if (unused != fallback.begin() + channels) *entry = *unused;
    }
    return map;
}

struct Decoder {
    ma_data_source_base base{};
    WavpackContext* context = nullptr;
    WavpackStreamReader64 reader{};
    FileHandle handle{};
    FileHandle correction{};
    ma_format format = ma_format_s16;
    ma_uint32 channels = 0;
    ma_uint32 sampleRate = 0;
    ma_uint64 lengthInFrames = 0;
    ma_uint32 bitsPerSample = 0;
    bool isFloat = false;
    ChannelMap channelMap{};
    std::string pathUtf8;
    char openError[128] = {};
    std::vector<int32_t> staging;
};

void CloseDecoderContext(Decoder& decoder) {
    if (decoder.context)
        decoder.context = WavpackCloseFile(decoder.context);
    CloseHandle(&decoder.handle);
    CloseHandle(&decoder.correction);
}

bool OpenDecoderContext(Decoder& decoder, bool allowCorrection) {
    CloseDecoderContext(decoder);
    decoder.handle.file = OpenUtf8(decoder.pathUtf8.c_str());
    if (!decoder.handle.file) return false;

    const auto size = decoder.pathUtf8.size();
    if (allowCorrection && size > 3 && decoder.pathUtf8[size - 3] == '.' &&
        (decoder.pathUtf8[size - 2] == 'w' || decoder.pathUtf8[size - 2] == 'W') &&
        (decoder.pathUtf8.back() == 'v' || decoder.pathUtf8.back() == 'V')) {
        const auto stem = decoder.pathUtf8.substr(0, size - 3);
        decoder.correction.file = OpenUtf8((stem + ".wvc").c_str());
#ifndef _WIN32
        if (!decoder.correction.file)
            decoder.correction.file = OpenUtf8((stem + ".WVC").c_str());
#endif
    }

    decoder.reader = MakeReader();
    auto* correction = decoder.correction.file ? &decoder.correction : nullptr;
    decoder.context = WavpackOpenFileInputEx64(
        &decoder.reader, &decoder.handle, correction, decoder.openError,
        kOpenFlags | (correction ? OPEN_WVC : 0), 0);
    if (!decoder.context) CloseDecoderContext(decoder);
    return decoder.context != nullptr;
}

#ifdef HARKEN_TEST_WAVPACK_VERIFICATION_COUNT
unsigned int correctionVerificationCount = 0;
#endif

bool VerifyCorrection(WavpackContext* context) {
#ifdef HARKEN_TEST_WAVPACK_VERIFICATION_COUNT
    ++correctionVerificationCount;
#endif
    const int channels = WavpackGetNumChannels(context);
    const int64_t length = WavpackGetNumSamples64(context);
    if (channels < 1 || channels > MA_MAX_CHANNELS || length <= 0) return false;

    constexpr uint32_t chunkFrames = 4096;
    std::vector<int32_t> samples(static_cast<size_t>(channels) * chunkFrames);
    for (int64_t remaining = length; remaining > 0;) {
        const auto request = static_cast<uint32_t>(std::min<int64_t>(remaining, chunkFrames));
        const auto frames = WavpackUnpackSamples(context, samples.data(), request);
        if (frames == 0 || WavpackGetNumErrors(context) != 0 || WavpackLossyBlocks(context))
            return false;
        remaining -= frames;
    }
    return (WavpackGetMode(context) & MODE_LOSSLESS) != 0;
}

using FileIdentity = std::array<uint64_t, 7>;
using CorrectionIdentity = std::array<FileIdentity, 2>;

std::optional<FileIdentity> IdentifyFile(std::FILE* file) {
    if (!file) return std::nullopt;
#ifdef _WIN32
    const auto handle = reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(file)));
    BY_HANDLE_FILE_INFORMATION info{};
    FILE_BASIC_INFO times{};
    if (handle == INVALID_HANDLE_VALUE || !GetFileInformationByHandle(handle, &info) ||
        !GetFileInformationByHandleEx(handle, FileBasicInfo, &times, sizeof(times)) ||
        (info.nFileIndexHigh == 0 && info.nFileIndexLow == 0) || times.ChangeTime.QuadPart == 0)
        return std::nullopt;
    return FileIdentity{
        info.dwVolumeSerialNumber,
        (static_cast<uint64_t>(info.nFileIndexHigh) << 32) | info.nFileIndexLow,
        (static_cast<uint64_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow,
        static_cast<uint64_t>(times.LastWriteTime.QuadPart),
        static_cast<uint64_t>(times.ChangeTime.QuadPart),
        static_cast<uint64_t>(times.CreationTime.QuadPart), 0};
#else
    struct stat info{};
    if (fstat(fileno(file), &info) != 0 || info.st_ino == 0) return std::nullopt;
#ifdef __APPLE__
    const auto modified = info.st_mtimespec;
    const auto changed = info.st_ctimespec;
#else
    const auto modified = info.st_mtim;
    const auto changed = info.st_ctim;
#endif
    return FileIdentity{
        static_cast<uint64_t>(info.st_dev), static_cast<uint64_t>(info.st_ino),
        static_cast<uint64_t>(info.st_size),
        static_cast<uint64_t>(modified.tv_sec), static_cast<uint64_t>(modified.tv_nsec),
        static_cast<uint64_t>(changed.tv_sec), static_cast<uint64_t>(changed.tv_nsec)};
#endif
}

std::optional<CorrectionIdentity> IdentifyCorrection(const Decoder& decoder) {
    const auto audio = IdentifyFile(decoder.handle.file);
    const auto correction = IdentifyFile(decoder.correction.file);
    if (!audio || !correction) return std::nullopt;
    return CorrectionIdentity{*audio, *correction};
}

bool VerifyOpenedCorrection(Decoder& decoder) {
    constexpr size_t maxCachedPairs = 64;
    static std::mutex mutex;
    static std::deque<std::pair<CorrectionIdentity, bool>> cache;
    const auto identity = IdentifyCorrection(decoder);
    if (identity) {
        std::lock_guard<std::mutex> lock(mutex);
        const auto found = std::find_if(cache.begin(), cache.end(),
            [&](const auto& entry) { return entry.first == *identity; });
        if (found != cache.end()) return found->second;
    }

    const bool valid = VerifyCorrection(decoder.context) && WavpackSeekSample64(decoder.context, 0);
    if (!identity) return valid;
    if (IdentifyCorrection(decoder) != identity) return false;

    std::lock_guard<std::mutex> lock(mutex);
    if (std::none_of(cache.begin(), cache.end(),
        [&](const auto& entry) { return entry.first == *identity; })) {
        if (cache.size() == maxCachedPairs) cache.pop_front();
        cache.emplace_back(*identity, valid);
    }
    return valid;
}

bool OpenValidatedDecoderContext(Decoder& decoder) {
    if (OpenDecoderContext(decoder, true)) {
        if ((WavpackGetMode(decoder.context) & MODE_WVC) == 0) return true;
        if (VerifyOpenedCorrection(decoder)) return true;
    }
    return OpenDecoderContext(decoder, false);
}

ma_result DecoderRead(ma_data_source* source, void* framesOut, ma_uint64 frameCount,
                      ma_uint64* framesRead) {
    auto* decoder = reinterpret_cast<Decoder*>(source);
    if (framesRead) *framesRead = 0;
    if (!decoder || !decoder->context || !framesOut || frameCount == 0) {
        return MA_INVALID_ARGS;
    }
    const size_t samples = static_cast<size_t>(frameCount) * decoder->channels;
    decoder->staging.resize(samples);
    const uint32_t decoded = WavpackUnpackSamples(
        decoder->context, decoder->staging.data(),
        static_cast<uint32_t>(frameCount));
    if (framesRead) *framesRead = decoded;
    if (decoded == 0) return MA_AT_END;
    if (decoder->isFloat) {
        std::memcpy(framesOut, decoder->staging.data(),
                    static_cast<size_t>(decoded) * decoder->channels * sizeof(float));
        return MA_SUCCESS;
    }
    auto* out = static_cast<float*>(framesOut);
    const size_t count = static_cast<size_t>(decoded) * decoder->channels;
    const float scale = 1.0f / static_cast<float>(1ULL << (decoder->bitsPerSample - 1));
    for (size_t i = 0; i < count; ++i) {
        out[i] = static_cast<float>(decoder->staging[i]) * scale;
    }
    return MA_SUCCESS;
}

ma_result DecoderSeek(ma_data_source* source, ma_uint64 frameIndex) {
    auto* decoder = reinterpret_cast<Decoder*>(source);
    if (!decoder || !decoder->context) return MA_INVALID_ARGS;
    if (WavpackSeekSample64(decoder->context, frameIndex)) return MA_SUCCESS;

    // WavPack block seeking can fail on single-block streams, so fall back to
    // reopening the file and decoding forward to the requested position.
    // Closing the old context closes the shared file handle, so reopen from
    // the stored path before decoding forward to the requested position.
    if (!OpenValidatedDecoderContext(*decoder)) return MA_ERROR;
    if (WavpackGetNumChannels(decoder->context) != static_cast<int>(decoder->channels) ||
        WavpackGetSampleRate(decoder->context) != decoder->sampleRate ||
        WavpackGetBitsPerSample(decoder->context) != static_cast<int>(decoder->bitsPerSample) ||
        ((WavpackGetMode(decoder->context) & MODE_FLOAT) != 0) != decoder->isFloat ||
        static_cast<ma_uint64>(WavpackGetNumSamples64(decoder->context)) != decoder->lengthInFrames ||
        BuildChannelMap(decoder->context, decoder->channels) != decoder->channelMap) {
        CloseDecoderContext(*decoder);
        return MA_ERROR;
    }

    while (WavpackGetSampleIndex64(decoder->context) < static_cast<int64_t>(frameIndex)) {
        const ma_uint64 remaining = frameIndex - static_cast<ma_uint64>(WavpackGetSampleIndex64(decoder->context));
        const ma_uint32 chunk = static_cast<ma_uint32>(remaining < 4096 ? remaining : 4096);
        decoder->staging.resize(static_cast<size_t>(chunk) * decoder->channels);
        const uint32_t unpacked = WavpackUnpackSamples(decoder->context, decoder->staging.data(), chunk);
        if (unpacked == 0) return MA_ERROR;
    }
    return MA_SUCCESS;
}

ma_result DecoderGetDataFormat(ma_data_source* source, ma_format* format,
                               ma_uint32* channels, ma_uint32* sampleRate,
                               ma_channel* channelMap, size_t channelMapCapacity) {
    auto* decoder = reinterpret_cast<Decoder*>(source);
    if (!decoder || !decoder->context) return MA_INVALID_ARGS;
    if (format) *format = decoder->format;
    if (channels) *channels = decoder->channels;
    if (sampleRate) *sampleRate = decoder->sampleRate;
    if (channelMap != nullptr) {
        const auto count = std::min<size_t>(decoder->channels, channelMapCapacity);
        std::copy_n(decoder->channelMap.begin(), count, channelMap);
    }
    return MA_SUCCESS;
}

ma_result DecoderGetCursor(ma_data_source* source, ma_uint64* cursor) {
    auto* decoder = reinterpret_cast<Decoder*>(source);
    if (!decoder || !decoder->context) return MA_INVALID_ARGS;
    if (cursor) *cursor = WavpackGetSampleIndex64(decoder->context);
    return MA_SUCCESS;
}

ma_result DecoderGetLength(ma_data_source* source, ma_uint64* length) {
    auto* decoder = reinterpret_cast<Decoder*>(source);
    if (!decoder || !decoder->context) return MA_INVALID_ARGS;
    if (length) *length = decoder->lengthInFrames;
    return MA_SUCCESS;
}

ma_data_source_vtable g_decoderVtable = {
    DecoderRead,
    DecoderSeek,
    DecoderGetDataFormat,
    DecoderGetCursor,
    DecoderGetLength,
    nullptr,
    0,
};

ma_result OpenDecoder(const char* utf8Path, ma_data_source** backendOut) {
    if (backendOut) *backendOut = nullptr;
    if (!utf8Path || !utf8Path[0]) return MA_INVALID_ARGS;

    auto* decoder = new (std::nothrow) Decoder();
    if (!decoder) return MA_OUT_OF_MEMORY;

    decoder->pathUtf8 = utf8Path;
    if (!OpenValidatedDecoderContext(*decoder)) {
        delete decoder;
        return MA_NO_BACKEND;
    }

    const int mode = WavpackGetMode(decoder->context);

    decoder->channels = static_cast<ma_uint32>(WavpackGetNumChannels(decoder->context));
    decoder->sampleRate = static_cast<ma_uint32>(WavpackGetSampleRate(decoder->context));
    decoder->bitsPerSample = static_cast<ma_uint32>(WavpackGetBitsPerSample(decoder->context));
    decoder->isFloat = (mode & MODE_FLOAT) != 0;
    if (decoder->channels == 0 || decoder->channels > MA_MAX_CHANNELS || decoder->sampleRate == 0 ||
        decoder->bitsPerSample == 0 || decoder->bitsPerSample > 32) {
        CloseDecoderContext(*decoder);
        delete decoder;
        return MA_NO_BACKEND;
    }
    decoder->channelMap = BuildChannelMap(decoder->context, decoder->channels);
    decoder->lengthInFrames = WavpackGetNumSamples64(decoder->context);
    // Custom miniaudio backends feed the decoder output format directly, so
    // hand out f32 like every other call site expects.
    decoder->format = ma_format_f32;

    ma_data_source_config sourceConfig = ma_data_source_config_init();
    sourceConfig.vtable = &g_decoderVtable;
    if (ma_data_source_init(&sourceConfig, &decoder->base) != MA_SUCCESS) {
        CloseDecoderContext(*decoder);
        delete decoder;
        return MA_ERROR;
    }

    if (backendOut) *backendOut = &decoder->base;
    return MA_SUCCESS;
}

void CloseDecoder(ma_data_source* backend) {
    if (!backend) return;
    auto* decoder = reinterpret_cast<Decoder*>(backend);
    ma_data_source_uninit(&decoder->base);
    CloseDecoderContext(*decoder);
    delete decoder;
}

ma_result BackendOnInit(void*, ma_read_proc, ma_seek_proc, ma_tell_proc, void*,
                        const ma_decoding_backend_config*, const ma_allocation_callbacks*,
                        ma_data_source**) {
    // Streams without a path are not supported; miniaudio falls through to the
    // built-in decoders.
    return MA_NO_BACKEND;
}

ma_result BackendOnInitFile(void*, const char* path, const ma_decoding_backend_config*,
                            const ma_allocation_callbacks*, ma_data_source** backend) {
    return OpenDecoder(path, backend);
}

ma_result BackendOnInitFileW(void*, const wchar_t* path, const ma_decoding_backend_config*,
                             const ma_allocation_callbacks*, ma_data_source** backend) {
#ifdef _WIN32
    if (path == nullptr) return MA_INVALID_ARGS;
    const int utf8Length = WideCharToMultiByte(CP_UTF8, 0, path, -1, nullptr, 0, nullptr, nullptr);
    if (utf8Length <= 1) return MA_INVALID_ARGS;
    std::string utf8(static_cast<size_t>(utf8Length - 1), '\0');
    if (WideCharToMultiByte(CP_UTF8, 0, path, -1, utf8.data(), utf8Length, nullptr, nullptr) == 0) {
        return MA_INVALID_ARGS;
    }
    return OpenDecoder(utf8.c_str(), backend);
#else
    (void)path;
    (void)backend;
    return MA_NO_BACKEND;
#endif
}

ma_result BackendOnInitMemory(void*, const void*, size_t, const ma_decoding_backend_config*,
                              const ma_allocation_callbacks*, ma_data_source**) {
    return MA_NO_BACKEND;
}

void BackendOnUninit(void*, ma_data_source* backend, const ma_allocation_callbacks*) {
    CloseDecoder(backend);
}

const ma_decoding_backend_vtable g_backendVtable = {
    BackendOnInit,
    BackendOnInitFile,
    BackendOnInitFileW,
    BackendOnInitMemory,
    BackendOnUninit,
};

const ma_decoding_backend_vtable* const g_backends[] = {&g_backendVtable};

}  // namespace

void ApplyDecodingBackend(ma_decoder_config& config) {
    config.ppCustomBackendVTables = const_cast<ma_decoding_backend_vtable**>(g_backends);
    config.customBackendCount = 1;
}

std::optional<bool> IsLossless(const ma_decoder& decoder) {
    if (decoder.pBackendVTable != &g_backendVtable || !decoder.pBackend) return std::nullopt;
    const auto* backend = reinterpret_cast<const Decoder*>(decoder.pBackend);
    return (WavpackGetMode(backend->context) & MODE_LOSSLESS) != 0;
}

#ifdef HARKEN_TEST_WAVPACK_VERIFICATION_COUNT
unsigned int CorrectionVerificationCountForTest() {
    return correctionVerificationCount;
}
#endif

}  // namespace Platform::WavPack
