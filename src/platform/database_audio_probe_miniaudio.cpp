#include "platform/database_audio_probe_backend.h"
#include "platform/wavpack_decoding_backend.h"

#include "../../thirdparty/miniaudio.h"
#ifdef AURO_DATABASE_AUDIO_PROBE_TESTING
#include "database_audio_probe_miniaudio_test_seam.h"
#endif

#include <algorithm>
#include <cwctype>
#include <string>
#include <vector>

#if defined(AURO_DATABASE_AUDIO_PROBE_TEST_WIN32) && defined(AURO_DATABASE_AUDIO_PROBE_TEST_STD)
#error Database audio probe test policies are mutually exclusive
#elif defined(AURO_DATABASE_AUDIO_PROBE_TEST_WIN32)
#define AURO_DATABASE_AUDIO_PROBE_MINIAUDIO_WIN32_POLICY 1
#elif defined(AURO_DATABASE_AUDIO_PROBE_TEST_STD)
#define AURO_DATABASE_AUDIO_PROBE_MINIAUDIO_WIN32_POLICY 0
#elif defined(_WIN32)
#define AURO_DATABASE_AUDIO_PROBE_MINIAUDIO_WIN32_POLICY 1
#else
#define AURO_DATABASE_AUDIO_PROBE_MINIAUDIO_WIN32_POLICY 0
#endif

#if defined(_WIN32) && !defined(AURO_DATABASE_AUDIO_PROBE_TESTING) && \
    !defined(AURO_DATABASE_AUDIO_PROBE_TEST_WIN32) && \
    !defined(AURO_DATABASE_AUDIO_PROBE_TEST_STD)
#define AURO_DATABASE_AUDIO_PROBE_REAL_WIN32 1
#include <windows.h>
#else
#define AURO_DATABASE_AUDIO_PROBE_REAL_WIN32 0
#endif

namespace Platform::DatabaseAudioProbe::Detail {
namespace {
ma_result GetLength(ma_decoder* decoder, ma_uint64* length) {
#ifdef AURO_DATABASE_AUDIO_PROBE_TESTING
    return TestSeam::GetLength(decoder, length);
#else
    return ma_decoder_get_length_in_pcm_frames(decoder, length);
#endif
}

ma_result ReadFrames(ma_decoder* decoder, void* destination,
                     ma_uint64 requested, ma_uint64* framesRead) {
#ifdef AURO_DATABASE_AUDIO_PROBE_TESTING
    return TestSeam::Read(decoder, destination, requested, framesRead);
#else
    return ma_decoder_read_pcm_frames(decoder, destination, requested, framesRead);
#endif
}

void Uninit(ma_decoder* decoder) {
#ifdef AURO_DATABASE_AUDIO_PROBE_TESTING
    TestSeam::Uninit(decoder);
#else
    ma_decoder_uninit(decoder);
#endif
}

#if AURO_DATABASE_AUDIO_PROBE_MINIAUDIO_WIN32_POLICY
std::wstring ExtendedPath(std::wstring path) {
    if (path.empty() || path.rfind(L"\\\\?\\", 0) == 0) {
        return path;
    }
    if (path.rfind(L"\\\\", 0) == 0) {
        return L"\\\\?\\UNC\\" + path.substr(2);
    }
    if (path.size() >= 2 && std::iswalpha(path[0]) && path[1] == L':') {
        return L"\\\\?\\" + path;
    }
    return path;
}

ma_result Init(const std::filesystem::path& path,
               const ma_decoder_config* config, ma_decoder* decoder) {
    const std::wstring candidate = ExtendedPath(path.wstring());
#ifdef AURO_DATABASE_AUDIO_PROBE_TESTING
    return TestSeam::InitWide(candidate.c_str(), config, decoder);
#else
    return ma_decoder_init_file_w(candidate.c_str(), config, decoder);
#endif
}
#else
ma_result Init(const std::filesystem::path& path,
               const ma_decoder_config* config, ma_decoder* decoder) {
    const std::string candidate = path.string();
#ifdef AURO_DATABASE_AUDIO_PROBE_TESTING
    return TestSeam::InitNarrow(candidate.c_str(), config, decoder);
#else
    return ma_decoder_init_file(candidate.c_str(), config, decoder);
#endif
}
#endif

bool ProbeMiniaudioCpp(const std::filesystem::path& path, Info& out) {
    ma_decoder decoder;
    ma_decoder_config config = ma_decoder_config_init(ma_format_f32, 0, 0);
    Platform::WavPack::ApplyDecodingBackend(config);
    if (Init(path, &config, &decoder) != MA_SUCCESS) {
        return false;
    }

    out.sampleRate = decoder.outputSampleRate;
    out.channels = decoder.outputChannels;

    ma_uint64 length = 0;
    (void)GetLength(&decoder, &length);
#if AURO_DATABASE_AUDIO_PROBE_MINIAUDIO_WIN32_POLICY
    if (length == 0) {
        constexpr ma_uint64 kChunkFrames = 8192;
        const ma_uint32 decodeChannels = decoder.outputChannels > 0
            ? decoder.outputChannels
            : 1;
        std::vector<float> buffer(
            static_cast<size_t>(kChunkFrames) * static_cast<size_t>(decodeChannels));
        ma_uint64 decodedFrames = 0;
        while (true) {
            ma_uint64 framesRead = 0;
            const ma_result readResult = ReadFrames(
                &decoder, buffer.data(), kChunkFrames, &framesRead);
            decodedFrames += framesRead;
            if (readResult != MA_SUCCESS && readResult != MA_AT_END) {
                break;
            }
            if (framesRead == 0 || readResult == MA_AT_END) {
                break;
            }
        }
        length = decodedFrames;
    }
#endif
    out.totalFrames = length;
    if (out.sampleRate > 0 && length > 0) {
        out.durationSeconds = static_cast<float>(length) /
                              static_cast<float>(out.sampleRate);
    } else {
        out.durationSeconds = 0.0f;
    }

    Uninit(&decoder);
    return true;
}

#if AURO_DATABASE_AUDIO_PROBE_REAL_WIN32
bool ProbeMiniaudioSeh(const std::filesystem::path* path, Info* out) {
    __try {
        return ProbeMiniaudioCpp(*path, *out);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *out = {};
        return false;
    }
}
#endif
} // namespace

bool ProbeMiniaudio(const std::filesystem::path& path, Info& out) {
#if AURO_DATABASE_AUDIO_PROBE_REAL_WIN32
    return ProbeMiniaudioSeh(&path, &out);
#else
    return ProbeMiniaudioCpp(path, out);
#endif
}
} // namespace Platform::DatabaseAudioProbe::Detail
