#include "platform/database_audio_probe.h"
#include "platform/database_audio_probe_backend.h"

#include "DiagnosticsLogger.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#if defined(AURO_DATABASE_AUDIO_PROBE_TEST_WIN32) && defined(AURO_DATABASE_AUDIO_PROBE_TEST_STD)
#error Database audio probe test policies are mutually exclusive
#elif defined(AURO_DATABASE_AUDIO_PROBE_TEST_WIN32)
#define AURO_DATABASE_AUDIO_PROBE_WIN32_POLICY 1
#elif defined(AURO_DATABASE_AUDIO_PROBE_TEST_STD)
#define AURO_DATABASE_AUDIO_PROBE_WIN32_POLICY 0
#elif defined(_WIN32)
#define AURO_DATABASE_AUDIO_PROBE_WIN32_POLICY 1
#else
#define AURO_DATABASE_AUDIO_PROBE_WIN32_POLICY 0
#endif

#if AURO_DATABASE_AUDIO_PROBE_WIN32_POLICY
std::string PathToUtf8(const std::filesystem::path& path);
#endif

namespace Platform::DatabaseAudioProbe {
namespace {
uint16_t Read16(const uint8_t* data) {
    return static_cast<uint16_t>(data[0] | (data[1] << 8));
}

uint32_t Read32(const uint8_t* data) {
    return static_cast<uint32_t>(data[0]) |
           (static_cast<uint32_t>(data[1]) << 8) |
           (static_cast<uint32_t>(data[2]) << 16) |
           (static_cast<uint32_t>(data[3]) << 24);
}

uint64_t Read64(const uint8_t* data) {
    return Read32(data) |
           (static_cast<uint64_t>(Read32(data + 4)) << 32);
}

std::string PathString(const std::filesystem::path& path) {
#if AURO_DATABASE_AUDIO_PROBE_WIN32_POLICY
    return PathToUtf8(path);
#else
    return path.string();
#endif
}

const char* FormatName(const std::string& path) {
    const size_t dot = path.find_last_of('.');
    std::string extension = dot == std::string::npos
        ? std::string{}
        : path.substr(dot);
    std::transform(
        extension.begin(), extension.end(), extension.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (extension == ".wma") {
        return "WMA";
    }
    if (extension == ".flac") {
        return "FLAC";
    }
    if (extension == ".m4a") {
        return "M4A";
    }
    if (extension == ".aac") {
        return "AAC";
    }
    if (extension == ".mp4") {
        return "MP4";
    }
    if (extension == ".m4p") {
        return "M4P";
    }
    return "MediaFoundation";
}

bool TryWave(const std::filesystem::path& path, Info& out) {
    std::string extension = PathString(path.extension());
    std::transform(extension.begin(), extension.end(), extension.begin(), ::tolower);
    if (extension != ".wav" && extension != ".wave") {
        return false;
    }

    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        return false;
    }

    char header[12] = {};
    file.read(header, sizeof(header));
    if (file.gcount() != sizeof(header)) {
        return false;
    }
    const bool isRiff = std::memcmp(header, "RIFF", 4) == 0;
    const bool isRf64 = std::memcmp(header, "RF64", 4) == 0;
    if ((!isRiff && !isRf64) || std::memcmp(header + 8, "WAVE", 4) != 0) {
        return false;
    }

    bool haveFmt = false;
    bool haveData = false;
    uint16_t blockAlign = 0;
    uint64_t dataBytes = 0;
    uint64_t factFrames = 0;
    uint64_t rf64DataBytes = 0;
    constexpr int kMaxWaveInfoChunks = 256;

    for (int i = 0; i < kMaxWaveInfoChunks && file.good(); ++i) {
        char chunkId[4] = {};
        uint8_t sizeBytes[4] = {};
        file.read(chunkId, sizeof(chunkId));
        file.read(reinterpret_cast<char*>(sizeBytes), sizeof(sizeBytes));
        if (file.gcount() != sizeof(sizeBytes)) {
            break;
        }

        const uint32_t chunkSize32 = Read32(sizeBytes);
        const uint64_t chunkSize = chunkSize32;
        const bool hasPad = (chunkSize & 1u) != 0;

        if (std::memcmp(chunkId, "ds64", 4) == 0 && chunkSize >= 24) {
            uint8_t ds64[24] = {};
            file.read(reinterpret_cast<char*>(ds64), sizeof(ds64));
            if (file.gcount() != sizeof(ds64)) {
                return false;
            }
            rf64DataBytes = Read64(ds64 + 8);
            factFrames = Read64(ds64 + 16);
            const uint64_t remaining = chunkSize - sizeof(ds64);
            if (remaining > 0) {
                file.seekg(static_cast<std::streamoff>(remaining), std::ios::cur);
            }
            if (hasPad) {
                file.seekg(1, std::ios::cur);
            }
            continue;
        }

        if (std::memcmp(chunkId, "fmt ", 4) == 0 && chunkSize >= 16) {
            const uint64_t readSize = (std::min<uint64_t>)(chunkSize, 40);
            std::vector<uint8_t> fmt(static_cast<size_t>(readSize));
            file.read(
                reinterpret_cast<char*>(fmt.data()),
                static_cast<std::streamsize>(readSize));
            if (static_cast<uint64_t>(file.gcount()) != readSize) {
                return false;
            }
            out.channels = Read16(fmt.data() + 2);
            out.sampleRate = Read32(fmt.data() + 4);
            blockAlign = Read16(fmt.data() + 12);
            haveFmt = out.channels > 0 && out.sampleRate > 0 && blockAlign > 0;
            const uint64_t remaining = chunkSize - readSize;
            if (remaining > 0) {
                file.seekg(static_cast<std::streamoff>(remaining), std::ios::cur);
            }
            if (hasPad) {
                file.seekg(1, std::ios::cur);
            }
        } else if (std::memcmp(chunkId, "fact", 4) == 0 && chunkSize >= 4) {
            uint8_t fact[8] = {};
            const uint64_t readSize = (std::min<uint64_t>)(chunkSize, sizeof(fact));
            file.read(
                reinterpret_cast<char*>(fact),
                static_cast<std::streamsize>(readSize));
            if (static_cast<uint64_t>(file.gcount()) != readSize) {
                return false;
            }
            factFrames = readSize >= 8 ? Read64(fact) : Read32(fact);
            const uint64_t remaining = chunkSize - readSize;
            if (remaining > 0) {
                file.seekg(static_cast<std::streamoff>(remaining), std::ios::cur);
            }
            if (hasPad) {
                file.seekg(1, std::ios::cur);
            }
        } else if (std::memcmp(chunkId, "data", 4) == 0) {
            dataBytes = chunkSize32 == 0xffffffffu && rf64DataBytes > 0
                ? rf64DataBytes
                : chunkSize;
            haveData = true;
            if (haveFmt) {
                break;
            }
            file.seekg(
                static_cast<std::streamoff>(chunkSize + (hasPad ? 1 : 0)),
                std::ios::cur);
        } else {
            file.seekg(
                static_cast<std::streamoff>(chunkSize + (hasPad ? 1 : 0)),
                std::ios::cur);
        }
    }

    if (!haveFmt) {
        return false;
    }
    if (factFrames > 0) {
        out.totalFrames = factFrames;
    } else if (haveData && blockAlign > 0) {
        out.totalFrames = dataBytes / blockAlign;
    }
    if (out.sampleRate > 0 && out.totalFrames > 0) {
        out.durationSeconds = static_cast<float>(
            static_cast<double>(out.totalFrames) /
            static_cast<double>(out.sampleRate));
    }
    return out.totalFrames > 0;
}
} // namespace

bool Probe(const std::filesystem::path& path, Info& out) {
    out = {};
    if (TryWave(path, out)) {
        return true;
    }
#if AURO_DATABASE_AUDIO_PROBE_WIN32_POLICY || (defined(__APPLE__) && !defined(AURO_DATABASE_AUDIO_PROBE_TEST_STD))
#if AURO_DATABASE_AUDIO_PROBE_WIN32_POLICY
    const std::string utf8Path = PathToUtf8(path);
#else
    const std::string utf8Path = PathString(path);
#endif
    const bool isFlac = Detail::IsFlacExtension(utf8Path);
    if (Detail::CanDecodeExtension(utf8Path) && !isFlac) {
        std::string error;
        if (Detail::ProbeMediaFoundation(utf8Path, out, error)) {
            DIAG_ACTION_FMT("Scan",
                "MediaFoundationProbeOK: path=%s format=%s sr=%u ch=%u frames=%llu dur=%.4f",
                utf8Path.c_str(),
                FormatName(utf8Path),
                (unsigned)out.sampleRate,
                (unsigned)out.channels,
                (unsigned long long)out.totalFrames,
                out.durationSeconds);
            return true;
        }
        DIAG_ACTION_FMT("Scan",
            "MediaFoundationProbeFailed: path=%s format=%s error=%s",
            utf8Path.c_str(),
            FormatName(utf8Path),
            error.c_str());
        return false;
    }

    if (Detail::ProbeMiniaudio(path, out)) {
        return true;
    }
    if (isFlac) {
        std::string error;
        if (Detail::ProbeMediaFoundation(utf8Path, out, error)) {
            DIAG_ACTION_FMT("Scan",
                "MediaFoundationProbeOK: path=%s format=%s sr=%u ch=%u frames=%llu dur=%.4f fallback=flac",
                utf8Path.c_str(),
                FormatName(utf8Path),
                (unsigned)out.sampleRate,
                (unsigned)out.channels,
                (unsigned long long)out.totalFrames,
                out.durationSeconds);
            return true;
        }
        DIAG_ACTION_FMT("Scan",
            "MediaFoundationProbeFailed: path=%s format=%s error=%s fallback=flac",
            utf8Path.c_str(),
            FormatName(utf8Path),
            error.c_str());
    }
    return false;
#else
    return Detail::ProbeMiniaudio(path, out);
#endif
}
} // namespace Platform::DatabaseAudioProbe
