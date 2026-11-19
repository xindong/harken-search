// WV decode regression: encode a deterministic PCM signal with the vendored
// WavPack encoder, decode it through the same platform decoder the product
// uses, and compare PCM plus seek behaviour.
#include "platform/audio_data_source.h"

#include "../thirdparty/wavpack/include/wavpack.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

static int g_failures = 0;

static void check(bool condition, const char* label) {
    if (!condition) {
        std::cerr << "FAIL: " << label << std::endl;
        ++g_failures;
    }
}

namespace {

constexpr int kSampleRate = 48000;
constexpr int kChannels = 2;
constexpr int kFrames = 48000;

std::vector<int16_t> MakeSine() {
    std::vector<int16_t> samples(static_cast<size_t>(kFrames) * kChannels);
    for (int frame = 0; frame < kFrames; ++frame) {
        const double left = std::sin(2.0 * 3.14159265358979323846 * 440.0 * frame / kSampleRate);
        const double right = std::sin(2.0 * 3.14159265358979323846 * 660.0 * frame / kSampleRate);
        samples[static_cast<size_t>(frame) * kChannels + 0] = static_cast<int16_t>(left * 20000.0);
        samples[static_cast<size_t>(frame) * kChannels + 1] = static_cast<int16_t>(right * 12000.0);
    }
    return samples;
}

int32_t WriteBlock(void* id, void* data, int32_t byteCount) {
    auto* file = static_cast<std::FILE*>(id);
    return static_cast<int32_t>(std::fwrite(data, 1, static_cast<size_t>(byteCount), file));
}

bool EncodeWavPack(const fs::path& path, const std::vector<int16_t>& pcm) {
#ifdef _WIN32
    std::FILE* file = _wfopen(path.wstring().c_str(), L"wb");
#else
    std::FILE* file = std::fopen(path.string().c_str(), "wb");
#endif
    if (!file) return false;

    WavpackContext* context = WavpackOpenFileOutput(WriteBlock, file, nullptr);
    if (!context) {
        std::fclose(file);
        return false;
    }

    WavpackConfig config{};
    config.bytes_per_sample = 2;
    config.bits_per_sample = 16;
    config.num_channels = kChannels;
    config.channel_mask = 0x3;
    config.sample_rate = kSampleRate;

    if (!WavpackSetConfiguration64(context, &config, kFrames, nullptr) ||
        !WavpackPackInit(context)) {
        WavpackCloseFile(context);
        std::fclose(file);
        return false;
    }

    std::vector<int32_t> staging(pcm.begin(), pcm.end());
    for (int offset = 0; offset < kFrames; offset += 4096) {
        const int frames = std::min(4096, kFrames - offset);
        if (!WavpackPackSamples(context, staging.data() + static_cast<size_t>(offset) * kChannels,
                                static_cast<uint32_t>(frames))) {
            WavpackCloseFile(context);
            std::fclose(file);
            return false;
        }
    }

    const bool flushed = WavpackFlushSamples(context) != 0;
    WavpackCloseFile(context);
    std::fclose(file);
    return flushed;
}

bool DecodeAll(const std::string& pathUtf8, std::vector<float>& out, std::uint32_t& channels,
               std::uint32_t& sampleRate, Platform::AudioDataSource::FrameCount& length) {
    Platform::AudioDataSource::Decoder decoder;
    const auto init = Platform::AudioDataSource::InitializeDecoderFromFile(decoder, pathUtf8);
    if (!init.Succeeded()) return false;

    const auto source = Platform::AudioDataSource::BorrowDataSource(decoder);
    const auto format = Platform::AudioDataSource::QueryDataFormat(source);
    if (!format.Succeeded()) return false;
    channels = format.channels;
    sampleRate = format.sampleRate;
    length = Platform::AudioDataSource::QueryLength(source).frames;
    if (length == 0 || length > 10 * kFrames) return false;

    out.assign(static_cast<size_t>(length) * channels, 0.0f);
    Platform::AudioDataSource::FrameCount cursor = 0;
    while (cursor < length) {
        const auto read = Platform::AudioDataSource::ReadPcmFrames(
            source, out.data() + static_cast<size_t>(cursor) * channels, length - cursor);
        if (read.framesRead == 0) break;
        cursor += read.framesRead;
    }
    Platform::AudioDataSource::UninitializeDecoder(decoder);
    return cursor == length;
}

float ExpectedSample(int frame, int channel) {
    const double frequency = channel == 0 ? 440.0 : 660.0;
    const double amplitude = channel == 0 ? 20000.0 : 12000.0;
    const double value = std::sin(2.0 * 3.14159265358979323846 * frequency * frame / kSampleRate);
    return static_cast<float>(static_cast<int16_t>(value * amplitude) / 32768.0);
}

}  // namespace

int main() {
    const fs::path temp = fs::temp_directory_path() / "harken_wv_decode_test";
    std::error_code ec;
    fs::create_directories(temp, ec);

    const auto pcm = MakeSine();
    const fs::path wvPath = temp / "sine_441_660.wv";
    check(EncodeWavPack(wvPath, pcm), "encoder writes a WV fixture");
    check(fs::exists(wvPath), "WV fixture exists");

    std::vector<float> decoded;
    std::uint32_t channels = 0;
    std::uint32_t sampleRate = 0;
    Platform::AudioDataSource::FrameCount length = 0;
    check(DecodeAll(wvPath.u8string(), decoded, channels, sampleRate, length),
          "decoder reads every WV frame");
    check(channels == kChannels, "decoded channel count matches");
    check(sampleRate == kSampleRate, "decoded sample rate matches");
    check(length == static_cast<Platform::AudioDataSource::FrameCount>(kFrames),
          "decoded frame count matches");

    if (decoded.size() == static_cast<size_t>(kFrames) * kChannels) {
        float worst = 0.0f;
        for (int frame = 0; frame < kFrames; ++frame) {
            for (int channel = 0; channel < kChannels; ++channel) {
                const float expected = ExpectedSample(frame, channel);
                const float actual = decoded[static_cast<size_t>(frame) * kChannels + channel];
                worst = std::max(worst, std::fabs(expected - actual));
            }
        }
        check(worst < 0.001f, "decoded PCM matches the source signal");
        std::cout << "worst sample delta=" << worst << std::endl;
    }

    {
        Platform::AudioDataSource::Decoder decoder;
        const auto init = Platform::AudioDataSource::InitializeDecoderFromFile(decoder, wvPath.u8string());
        check(init.Succeeded(), "decoder reopens for seek check");
        if (init.Succeeded()) {
            const auto source = Platform::AudioDataSource::BorrowDataSource(decoder);
            const auto seek = Platform::AudioDataSource::SeekPcmFrame(source, 24000);
            check(seek.Succeeded(), "seek to frame 24000 succeeds");
            std::vector<float> window(static_cast<size_t>(256) * kChannels, 0.0f);
            const auto read = Platform::AudioDataSource::ReadPcmFrames(source, window.data(), 256);
            check(read.framesRead == 256, "seeked read returns a full window");
            float worst = 0.0f;
            for (int frame = 0; frame < static_cast<int>(read.framesRead); ++frame) {
                for (int channel = 0; channel < kChannels; ++channel) {
                    const float expected = ExpectedSample(24000 + frame, channel);
                    const float actual = window[static_cast<size_t>(frame) * kChannels + channel];
                    worst = std::max(worst, std::fabs(expected - actual));
                }
            }
            check(worst < 0.001f, "samples after seek match the source signal");
            Platform::AudioDataSource::UninitializeDecoder(decoder);
        }
    }

    {
        const fs::path unicodePath = temp / fs::u8path("中文样本_音效.wv");
        fs::copy_file(wvPath, unicodePath, fs::copy_options::overwrite_existing, ec);
        std::vector<float> unicodeDecoded;
        std::uint32_t unicodeChannels = 0;
        std::uint32_t unicodeRate = 0;
        Platform::AudioDataSource::FrameCount unicodeLength = 0;
        check(DecodeAll(unicodePath.u8string(), unicodeDecoded, unicodeChannels, unicodeRate, unicodeLength),
              "decoder opens a WV file with a non-ASCII path");
        check(unicodeLength == static_cast<Platform::AudioDataSource::FrameCount>(kFrames),
              "non-ASCII path decodes the full length");
    }

    {
        const fs::path bogus = temp / "not_really.wv";
        std::FILE* file = std::fopen(bogus.string().c_str(), "wb");
        if (file) {
            const char payload[64] = "wvpk-but-not-wavpack";
            std::fwrite(payload, 1, sizeof(payload), file);
            std::fclose(file);
        }
        Platform::AudioDataSource::Decoder decoder;
        const auto init = Platform::AudioDataSource::InitializeDecoderFromFile(decoder, bogus.u8string());
        check(!init.Succeeded(), "invalid WV payload is rejected");
    }

    if (std::getenv("HARKEN_WV_KEEP") == nullptr) fs::remove_all(temp, ec);

    if (g_failures != 0) {
        std::cerr << "FAIL: " << g_failures << " assertion(s)" << std::endl;
        return 1;
    }
    std::cout << "PASS" << std::endl;
    return 0;
}
