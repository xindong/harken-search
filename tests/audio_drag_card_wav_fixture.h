#pragma once
// 测试共用：从真实 WAV 读取峰值，按与产品相同的分桶方式生成迷你波形。
// 仅用于验证工具，不进入产品。

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>

namespace audio_drag_card_test {

struct WavInfo {
    int channels = 0;
    int bitsPerSample = 0;
    int blockAlign = 0;
    int formatTag = 0;
    const unsigned char* data = nullptr;
    size_t dataBytes = 0;
    size_t sampleFrames = 0;
};

bool ParseWav(const std::filesystem::path& path, std::vector<unsigned char>& bytes, WavInfo& info) {
    info = {};
    std::ifstream stream(path, std::ios::binary);
    if (!stream) return false;
    bytes.assign(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
    if (bytes.size() < 44 || std::memcmp(bytes.data(), "RIFF", 4) != 0 ||
        std::memcmp(bytes.data() + 8, "WAVE", 4) != 0) {
        return false;
    }

    size_t offset = 12;
    while (offset <= bytes.size() && bytes.size() - offset >= 8) {
        const unsigned char* cursor = bytes.data() + offset;
        char chunkId[5] = {};
        std::memcpy(chunkId, cursor, 4);
        uint32_t chunkSize = 0;
        std::memcpy(&chunkSize, cursor + 4, 4);
        const size_t payloadOffset = offset + 8;
        if (chunkSize > bytes.size() - payloadOffset) return false;
        const size_t paddedSize = static_cast<size_t>(chunkSize) + (chunkSize % 2);
        if (paddedSize > bytes.size() - payloadOffset) return false;
        const unsigned char* payload = bytes.data() + payloadOffset;
        if (std::strcmp(chunkId, "fmt ") == 0 && chunkSize >= 16) {
            uint16_t tag = 0, channels = 0, blockAlign = 0, bits = 0;
            std::memcpy(&tag, payload, 2);
            std::memcpy(&channels, payload + 2, 2);
            std::memcpy(&blockAlign, payload + 12, 2);
            std::memcpy(&bits, payload + 14, 2);
            info.formatTag = tag;
            info.channels = channels;
            info.blockAlign = blockAlign;
            info.bitsPerSample = bits;
            if (tag == 0xFFFE && chunkSize >= 40) {
                uint16_t subFormat = 0;
                std::memcpy(&subFormat, payload + 24, 2);
                info.formatTag = subFormat;
            }
        } else if (std::strcmp(chunkId, "data") == 0) {
            info.data = payload;
            info.dataBytes = chunkSize;
        }
        offset = payloadOffset + paddedSize;
    }
    if (offset != bytes.size() || !info.data || info.channels <= 0 || info.blockAlign <= 0) return false;
    const bool pcm = info.formatTag == 1 &&
                     (info.bitsPerSample == 8 || info.bitsPerSample == 16 ||
                      info.bitsPerSample == 24 || info.bitsPerSample == 32);
    const bool float32 = info.formatTag == 3 && info.bitsPerSample == 32;
    if ((!pcm && !float32) || info.blockAlign != info.channels * (info.bitsPerSample / 8) ||
        info.dataBytes % static_cast<size_t>(info.blockAlign) != 0) return false;
    info.sampleFrames = info.dataBytes / static_cast<size_t>(info.blockAlign);
    return info.sampleFrames > 0;
}

float ReadSample(const WavInfo& info, size_t frame, int channel) {
    if (!info.data || frame >= info.sampleFrames || channel < 0 || channel >= info.channels ||
        info.blockAlign <= 0 || info.bitsPerSample <= 0 || info.bitsPerSample % 8 != 0) return 0.0f;
    const size_t offset = frame * static_cast<size_t>(info.blockAlign) +
                          static_cast<size_t>(channel) * static_cast<size_t>(info.bitsPerSample / 8);
    const size_t sampleBytes = static_cast<size_t>(info.bitsPerSample / 8);
    if (offset > info.dataBytes || sampleBytes > info.dataBytes - offset) return 0.0f;
    const unsigned char* p = info.data + offset;
    if (info.formatTag == 3 && info.bitsPerSample == 32) {
        float value = 0.0f;
        std::memcpy(&value, p, 4);
        return value;
    }
    if (info.bitsPerSample == 16) {
        int16_t value = 0;
        std::memcpy(&value, p, 2);
        return static_cast<float>(value) / 32768.0f;
    }
    if (info.bitsPerSample == 24) {
        int32_t value = (static_cast<int32_t>(p[2]) << 24) | (static_cast<int32_t>(p[1]) << 16) |
                        (static_cast<int32_t>(p[0]) << 8);
        return static_cast<float>(value >> 8) / 8388608.0f;
    }
    if (info.bitsPerSample == 32) {
        int32_t value = 0;
        std::memcpy(&value, p, 4);
        return static_cast<float>(value) / 2147483648.0f;
    }
    if (info.bitsPerSample == 8) {
        return (static_cast<float>(*p) - 128.0f) / 128.0f;
    }
    return 0.0f;
}

// 与产品相同的取样方式：按柱数分桶，取每桶所有声道最大绝对值。
std::vector<float> BuildEnvelopeFromWav(const WavInfo& info, int bars) {
    std::vector<float> envelope(static_cast<size_t>(bars), 0.0f);
    for (size_t frame = 0; frame < info.sampleFrames; ++frame) {
        float peak = 0.0f;
        for (int channel = 0; channel < info.channels; ++channel) {
            peak = (std::max)(peak, std::fabs(ReadSample(info, frame, channel)));
        }
        const int bar = static_cast<int>((static_cast<uint64_t>(frame) * static_cast<uint64_t>(bars)) /
                                         static_cast<uint64_t>(info.sampleFrames));
        if (bar >= 0 && bar < bars) envelope[static_cast<size_t>(bar)] = (std::max)(envelope[static_cast<size_t>(bar)], peak);
    }
    for (float& value : envelope) value = std::clamp(value, 0.0f, 1.0f);
    return envelope;
}

}  // namespace audio_drag_card_test
