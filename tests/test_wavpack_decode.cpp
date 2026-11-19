// WV decode regression: encode a deterministic PCM signal with the vendored
// WavPack encoder, decode it through the same platform decoder the product
// uses, and compare PCM plus seek behaviour.
#include "platform/audio_data_source.h"
#include "platform/wavpack_decoding_backend.h"
#include "MetadataReader.h"

#include "../thirdparty/wavpack/include/wavpack.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <memory>
#include <random>
#include <string>
#include <utility>
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

bool EncodeWavPack(const fs::path& path, std::vector<int32_t> samples, int channels,
                  int channelMask, int floatExponent = 0, bool hybrid = false) {
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
    config.bytes_per_sample = floatExponent == 0 ? 2 : 4;
    config.bits_per_sample = config.bytes_per_sample * 8;
    config.float_norm_exp = floatExponent;
    config.num_channels = channels;
    config.channel_mask = channelMask;
    config.sample_rate = kSampleRate;
    config.block_samples = 1024;
    if (hybrid) {
        config.flags = CONFIG_HYBRID_FLAG;
        config.bitrate = 3.5f;
    }

    if (!WavpackSetConfiguration64(context, &config, kFrames, nullptr) ||
        !WavpackPackInit(context)) {
        WavpackCloseFile(context);
        std::fclose(file);
        return false;
    }

    for (int offset = 0; offset < kFrames; offset += 4096) {
        const int frames = std::min(4096, kFrames - offset);
        if (!WavpackPackSamples(context, samples.data() + static_cast<size_t>(offset) * channels,
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

std::optional<bool> ReadWavPackLosslessness(const fs::path& path) {
    ma_decoder decoder;
    auto config = ma_decoder_config_init_default();
    Platform::WavPack::ApplyDecodingBackend(config);
#ifdef _WIN32
    const auto opened = ma_decoder_init_file_w(path.c_str(), &config, &decoder);
#else
    const auto opened = ma_decoder_init_file(path.c_str(), &config, &decoder);
#endif
    if (opened != MA_SUCCESS) return std::nullopt;
    const auto result = Platform::WavPack::IsLossless(decoder);
    ma_decoder_uninit(&decoder);
    return result;
}

constexpr unsigned char kPngCover[] = {
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a,
    0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44, 0x52,
    0x00, 0x00, 0x00, 0x28, 0x00, 0x00, 0x00, 0x28,
    0x08, 0x02, 0x00, 0x00, 0x00, 0x03, 0x9c, 0x2f,
    0x3a, 0x00, 0x00, 0x00, 0x37, 0x49, 0x44, 0x41,
    0x54, 0x78, 0xda, 0xed, 0xcd, 0x31, 0x0d, 0x00,
    0x20, 0x0c, 0x00, 0xb0, 0x59, 0x99, 0x02, 0x9c,
    0xf2, 0xcc, 0x06, 0x16, 0x49, 0xe6, 0x61, 0x70,
    0x36, 0xe9, 0xdf, 0xb8, 0x7b, 0x3d, 0xca, 0x3a,
    0x03, 0x21, 0x16, 0x8b, 0xc5, 0x62, 0xb1, 0x58,
    0x2c, 0x16, 0x8b, 0xc5, 0x62, 0xb1, 0x58, 0x2c,
    0x16, 0xff, 0x8b, 0x1b, 0xd0, 0xe8, 0xcd, 0xe8,
    0x81, 0x3b, 0xf1, 0x6d, 0x00, 0x00, 0x00, 0x00,
    0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82,
};

bool WriteFixtureTags(const fs::path& path, bool complete) {
    const std::unique_ptr<WavpackContext, decltype(&WavpackCloseFile)> context(
        WavpackOpenFileInput(path.u8string().c_str(), nullptr,
                             OPEN_TAGS | OPEN_EDIT_TAGS | OPEN_FILE_UTF8, 0),
        WavpackCloseFile);
    if (!context) return false;

    auto set = [&](const char* key, const char* value) {
        return WavpackAppendTagItem(context.get(), key, value, static_cast<int>(std::strlen(value))) != 0;
    };
    if (!set("Genre", "Field recording")) return false;
    if (complete &&
        !(set("tItLe", u8"雨声 · WV") && set("Artist", "Harken QA") &&
          set("Album", "WavPack QA") && set("Album Artist", "Harken Search") &&
          set("Year", "2026") && set("Track", "2/9") && set("Disc", "3/4") &&
          set("Description", "Outdoor rain") && set("Comment", "Retains separate comment") &&
          set("Category", "Weather") && set("Tags", "rain;field") &&
          set("REPLAYGAIN_TRACK_GAIN", "-3.25 dB"))) {
        return false;
    }
    std::string cover = "cover.jpg";
    if (complete) {
        cover.push_back('\0');
        cover.append(reinterpret_cast<const char*>(kPngCover), sizeof(kPngCover));
    }
    return WavpackAppendBinaryTagItem(context.get(), "Cover Art (Front)", cover.data(),
                                      static_cast<int>(cover.size())) != 0 &&
           WavpackWriteTag(context.get()) != 0;
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

fs::path CreateUniqueFixtureDirectory() {
    const fs::path parent = fs::temp_directory_path();
    std::random_device entropy;
    for (int attempt = 0; attempt < 16; ++attempt) {
        const auto name = "harken_wv_decode_test-" + std::to_string(entropy()) + "-" +
                          std::to_string(entropy());
        const fs::path candidate = parent / name;
        std::error_code error;
        if (fs::create_directory(candidate, error)) return candidate;
        if (error && error != std::errc::file_exists) break;
    }
    return {};
}

}  // namespace

int main() {
    const fs::path temp = CreateUniqueFixtureDirectory();
    check(!temp.empty(), "fixture directory is exclusively created");
    if (temp.empty()) return 1;
    std::error_code ec;
    std::cout << "fixture directory=" << temp.u8string() << std::endl;

    const auto pcm = MakeSine();
    const fs::path wvPath = temp / "sine_441_660.wv";
    check(EncodeWavPack(wvPath, std::vector<int32_t>(pcm.begin(), pcm.end()), kChannels, 0x3),
          "encoder writes a WV fixture");
    check(fs::exists(wvPath), "WV fixture exists");
    check(ReadWavPackLosslessness(wvPath) == true, "lossless WV is selectable as lossless");
    const fs::path hybridPath = temp / "hybrid.WV";
    check(EncodeWavPack(hybridPath, std::vector<int32_t>(pcm.begin(), pcm.end()), kChannels, 0x3, 0, true),
          "encoder writes a hybrid main file without correction");
    check(ReadWavPackLosslessness(hybridPath) == false, "hybrid WV without correction stays lossy");

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
            float worst = 0.0f;
            for (const int target : {40234, 897, 24000}) {
                const auto seek = Platform::AudioDataSource::SeekPcmFrame(source, target);
                check(seek.Succeeded(), "seek across WV blocks succeeds in both directions");
                std::vector<float> window(static_cast<size_t>(256) * kChannels, 0.0f);
                const auto read = Platform::AudioDataSource::ReadPcmFrames(source, window.data(), 256);
                check(read.framesRead == 256, "read 256 frames after seek");
                for (int frame = 0; frame < 256; ++frame) {
                    for (int channel = 0; channel < kChannels; ++channel) {
                        const float expected = ExpectedSample(target + frame, channel);
                        const float actual = window[static_cast<size_t>(frame) * kChannels + channel];
                        worst = std::max(worst, std::fabs(expected - actual));
                    }
                }
            }
            check(worst < 0.001f, "samples after seek match the source signal");
            std::cout << "cross-block seek worst sample delta=" << worst << std::endl;
            Platform::AudioDataSource::UninitializeDecoder(decoder);
        }
    }

    {
        const fs::path floatPath = temp / "nonstandard_full_scale.wv";
        constexpr float originalSample = 32768.0f;
        int32_t floatBits = 0;
        std::memcpy(&floatBits, &originalSample, sizeof(floatBits));
        check(EncodeWavPack(floatPath, std::vector<int32_t>(kFrames, floatBits), 1, 0x4, 143),
              "encoder writes a float WV with 65536 full scale");
        std::vector<float> samples;
        check(DecodeAll(floatPath.u8string(), samples, channels, sampleRate, length),
              "decode WV with nonstandard floating-point full scale");
        if (!samples.empty()) {
            const float peak = *std::max_element(samples.begin(), samples.end());
            std::cout << "normalized float peak=" << peak << std::endl;
            check(std::fabs(peak - 0.5f) < 0.0001f, "float WV decodes at normalized full scale");
        }
    }

    {
        constexpr int surroundChannels = 3;
        std::vector<int32_t> surround(static_cast<size_t>(kFrames) * surroundChannels, 0);
        for (int frame = 0; frame < kFrames; ++frame) {
            surround[static_cast<size_t>(frame) * surroundChannels] = 12288;
        }
        const fs::path surroundPath = temp / "front_right_center_lfe.wv";
        check(EncodeWavPack(surroundPath, std::move(surround), surroundChannels, 0x0e),
              "encoder writes a WV with front right, center and LFE channel positions");
        Platform::AudioDataSource::Decoder stereo;
        const auto init = Platform::AudioDataSource::InitializeDecoderFromFileWithFormat(
            stereo, surroundPath.u8string(), kChannels, kSampleRate);
        check(init.Succeeded(), "open multichannel WV for stereo downmix");
        if (init.Succeeded()) {
            float firstFrame[kChannels]{};
            const auto read = Platform::AudioDataSource::ReadPcmFrames(
                Platform::AudioDataSource::BorrowDataSource(stereo), firstFrame, 1);
            std::cout << "multichannel WV downmix left=" << firstFrame[0]
                      << ", right=" << firstFrame[1] << std::endl;
            check(read.framesRead == 1 && firstFrame[1] > 0.1f &&
                      firstFrame[1] > 2.0f * std::fabs(firstFrame[0]),
                  "encoded front right stays on the right side of the stereo downmix");
            Platform::AudioDataSource::UninitializeDecoder(stereo);
        }
    }

    {
        const fs::path unicodePath = temp / fs::u8path("中文样本_音效.WV");
        fs::copy_file(wvPath, unicodePath, fs::copy_options::overwrite_existing, ec);
        std::vector<float> unicodeDecoded;
        std::uint32_t unicodeChannels = 0;
        std::uint32_t unicodeRate = 0;
        Platform::AudioDataSource::FrameCount unicodeLength = 0;
        check(DecodeAll(unicodePath.u8string(), unicodeDecoded, unicodeChannels, unicodeRate, unicodeLength),
              "decoder opens a WV file with a non-ASCII path");
        check(unicodeLength == static_cast<Platform::AudioDataSource::FrameCount>(kFrames),
              "non-ASCII path decodes the full length");
        check(WriteFixtureTags(unicodePath, true), "fixture writes real APEv2 text and PNG");
        AudioMetadata tags;
        check(MetadataReader::readMetadata(unicodePath.u8string(), tags), "read APEv2 from uppercase WV");
        check(tags.title == u8"雨声 · WV" && tags.artist == "Harken QA" &&
              tags.album == "WavPack QA" && tags.albumArtist == "Harken Search" &&
              tags.genre == "Field recording", "read Unicode title and standard APEv2 strings");
        check(tags.year == 2026 && tags.track == 2 && tags.disc == 3,
              "read APEv2 year and fractional track/disc numbers");
        check(tags.description == "Outdoor rain" && tags.comment == "Retains separate comment" &&
              tags.category == "Weather" && tags.keywords == "rain;field" &&
              tags.replayGainTrack == -3.25f, "map APEv2 extra fields and ReplayGain");
        AlbumArt cover;
        check(MetadataReader::readAlbumArt(unicodePath.u8string(), cover) &&
              cover.mimeType == "image/png" && cover.data == std::vector<uint8_t>(kPngCover, kPngCover + sizeof(kPngCover)),
              "read APEv2 front cover bytes without the embedded filename");
    }

    {
        const fs::path sparsePath = temp / "sparse_tags.wv";
        fs::copy_file(wvPath, sparsePath, fs::copy_options::overwrite_existing, ec);
        AudioMetadata existing;
        existing.title = "keep on failure";
        check(!MetadataReader::readMetadata(sparsePath.u8string(), existing) &&
              existing.title == "keep on failure", "untagged WV leaves the output unchanged");
        check(WriteFixtureTags(sparsePath, false), "fixture writes genre and a broken cover");
        AudioMetadata tags;
        check(MetadataReader::readMetadata(sparsePath.u8string(), tags) &&
              tags.genre == "Field recording", "genre without title counts as APEv2 metadata");
        AlbumArt cover;
        cover.mimeType = "unchanged";
        check(!MetadataReader::readAlbumArt(sparsePath.u8string(), cover) &&
              cover.mimeType == "unchanged" && cover.data.empty(),
              "reject APEv2 cover without filename terminator");
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
