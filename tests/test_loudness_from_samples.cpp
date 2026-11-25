#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <atomic>
#include <vector>

#include "../src/AudioEngine.h"
#include "../src/WaveformExtractor.h"
#include "../thirdparty/miniaudio.h"
#include "../src/platform/audio_data_source.h"
#include "../src/Database.h"

#include "../src/platform/system.h"

namespace Platform {

EnvironmentStringResult ReadEnvironmentVariable(const std::string&) {
    return {};
}

void SetThreadBackgroundPriority() {
}

}

void ApplyExportDeclickFade(float*, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t) {
}

struct AppState;
AppState* g_appState = nullptr;

WaveformCachePolicy::Mode Database::getWaveformCacheMode() {
    return WaveformCachePolicy::Mode::MiniOnly;
}
AudioRecord Database::getFileById(int64_t) {
    return AudioRecord{};
}
std::vector<float> Database::getWaveformByPath(const std::string&, float*, int*) {
    return {};
}
std::vector<float> Database::getRmsDataByPath(const std::string&) {
    return {};
}
bool Database::saveLoudnessByPath(const std::string&, float, float, float, float) {
    return false;
}
bool Database::getLoudnessByPath(const std::string&, float&, float&, float&, float&) {
    return false;
}
std::vector<float> Database::getWaveformDetailByPath(const std::string&) {
    return {};
}
std::vector<float> Database::getWaveformMinDetailByPath(const std::string&) {
    return {};
}
int Database::getWaveformDetailChannelsByPath(const std::string&) {
    return 0;
}
bool Database::upsertWaveformCacheStateByPath(const std::string&, const std::string&, const std::string&, int, const std::string&) {
    return false;
}

namespace {

bool g_reportZeroLength = false;

}  // namespace

namespace Platform::AudioDataSource {

LengthInFrames QueryLength(BorrowedSource source) noexcept {
    if (g_reportZeroLength) return LengthInFrames{0, 0};
    if (!source.IsValid()) return {};
    ma_uint64 length = 0;
    const ma_result result = ma_data_source_get_length_in_pcm_frames(
        static_cast<ma_data_source*>(source.native_), &length);
    return {static_cast<int>(result), static_cast<FrameCount>(length)};
}

}  // namespace Platform::AudioDataSource

static int failed = 0, passed = 0;
static void CHECK(bool c, const char* d) {
    if (c) { ++passed; std::printf("  PASS: %s\n", d); }
    else   { ++failed; std::printf("  FAIL: %s\n", d); }
}

template<class SampleAt>
static void WritePcmWav(const std::string& path, uint32_t frames, SampleAt sampleAt) {
    const uint32_t sr = 48000;
    auto le16 = [](std::vector<uint8_t>& o, uint16_t v){ o.push_back(v&0xFF); o.push_back((v>>8)&0xFF); };
    auto le32 = [](std::vector<uint8_t>& o, uint32_t v){ for(int i=0;i<4;++i) o.push_back((v>>(8*i))&0xFF); };
    std::vector<uint8_t> w;
    const uint32_t dataBytes = frames * 2;
    w.insert(w.end(), {'R','I','F','F'}); le32(w, 36+dataBytes);
    w.insert(w.end(), {'W','A','V','E','f','m','t',' '}); le32(w,16);
    le16(w,1); le16(w,1); le32(w,sr); le32(w,sr*2); le16(w,2); le16(w,16);
    w.insert(w.end(), {'d','a','t','a'}); le32(w,dataBytes);
    for (uint32_t i=0;i<frames;++i){
        const int16_t s = sampleAt(i);
        w.push_back((uint8_t)(s&0xFF)); w.push_back((uint8_t)((s>>8)&0xFF));
    }
    std::ofstream o(path, std::ios::binary);
    o.write((const char*)w.data(), (std::streamsize)w.size());
}

static void WriteSineWav(const std::string& path) {
    WritePcmWav(path, 48000, [](uint32_t frame) {
        const double time = static_cast<double>(frame) / 48000;
        return static_cast<int16_t>(std::lround(0.5 * 32767.0 * std::sin(2 * 3.14159265358979 * 1000.0 * time)));
    });
}

int main() {
    std::printf("=== LoudnessFromSamples Tests ===\n");
    const std::string wav = "test_loudness_tmp.wav";
    WriteSineWav(wav);

    WaveformData viaFile;
    const bool okFile = AudioEngine::generateWaveformForFile(wav, viaFile, 100);
    CHECK(okFile, "generateWaveformForFile ok");

    std::vector<float> samples;
    uint64_t fr = 0; uint32_t ch = 0, sr = 0;
    const bool decoded = AudioEngine::DecodeFileToInterleavedF32(wav, samples, fr, ch, sr);
    CHECK(decoded, "DecodeFileToInterleavedF32 ok");
    CHECK(ch == 1, "DecodeFileToInterleavedF32 channel count == 1");
    CHECK(sr == 48000, "DecodeFileToInterleavedF32 sample rate == 48000");
    CHECK(fr == 48000, "DecodeFileToInterleavedF32 frame count == 48000");

    WaveformData viaSamples;
    const bool okSamples = AudioEngine::computeWaveformAndLoudnessFromSamples(
        samples.data(), fr, ch, sr, 100, viaSamples);
    CHECK(okSamples, "computeWaveformAndLoudnessFromSamples ok");

    CHECK(std::fabs(viaFile.trueRmsDbfs - viaSamples.trueRmsDbfs) < 0.5f,
          "trueRmsDbfs matches within 0.5 dB");
    CHECK(viaFile.peaks.size() == viaSamples.peaks.size(),
          "peak count matches");

    std::atomic<bool> cancelled{true};
    WaveformData cancelledWaveform;
    const bool cancelledSamples = AudioEngine::computeWaveformAndLoudnessFromSamples(
        samples.data(), fr, ch, sr, 100, cancelledWaveform, &cancelled);
    CHECK(!cancelledSamples, "computeWaveformAndLoudnessFromSamples honors pre-cancel");
    CHECK(cancelledWaveform.peaks.empty(), "cancelled sample compute leaves no peaks");

    std::vector<float> cancelledDecodeSamples;
    uint64_t cancelledFrames = 0;
    uint32_t cancelledChannels = 0;
    uint32_t cancelledSampleRate = 0;
    const bool cancelledDecode = AudioEngine::DecodeFileToInterleavedF32(
        wav, cancelledDecodeSamples, cancelledFrames, cancelledChannels, cancelledSampleRate, &cancelled);
    CHECK(!cancelledDecode, "DecodeFileToInterleavedF32 honors pre-cancel");
    CHECK(cancelledDecodeSamples.empty(), "cancelled decode leaves no samples");

    std::remove(wav.c_str());

    WriteSineWav(wav);
    g_reportZeroLength = true;
    WaveformData streamed;
    const bool streamedOk = AudioEngine::generateWaveformForFile(wav, streamed, 4);
    CHECK(streamedOk, "unknown-length decoder still produces a waveform");
    CHECK(streamed.isReady, "unknown-length waveform is ready");
    CHECK(streamed.hasTrueRms, "unknown-length waveform still reports RMS");
    CHECK(!streamed.peaks.empty(), "unknown-length waveform has peaks");
    CHECK(streamed.peaks.size() <= 8, "unknown-length peaks stay bounded for a one-second file");
    const float streamedRms = streamed.trueRmsDbfs;
    WaveformData meterOnly;
    const bool meterOk = AudioEngine::generateWaveformForFile(
        wav, meterOnly, 4, nullptr, false);
    CHECK(meterOk, "unknown-length stream succeeds with peak collection off");
    CHECK(meterOnly.peaks.empty() && meterOnly.rms.empty(),
          "collectPeaks=false emits no peak or rms arrays");
    CHECK(meterOnly.hasTrueRms && meterOnly.hasLoudnessData,
          "collectPeaks=false still reports true RMS and loudness");
    CHECK(std::fabs(meterOnly.trueRmsDbfs - streamedRms) < 0.01f,
          "meters are unchanged by the peak collection flag");
    g_reportZeroLength = false;
    WaveformData knownLength;
    const bool knownOk = AudioEngine::generateWaveformForFile(wav, knownLength, 100);
    CHECK(knownOk, "same file with a reported length still produces a waveform");
    CHECK(std::fabs(streamedRms - knownLength.trueRmsDbfs) < 0.5f,
          "unknown-length RMS matches the known-length path within 0.5 dB");
    g_reportZeroLength = true;
    std::atomic<bool> streamCancelled{true};
    WaveformData cancelledStream;
    const bool cancelledStreamOk = AudioEngine::generateWaveformForFile(
        wav, cancelledStream, 4, &streamCancelled);
    CHECK(!cancelledStreamOk, "unknown-length stream honors pre-cancel");
    CHECK(cancelledStream.peaks.empty(), "cancelled unknown-length stream leaves no peaks");
    std::vector<float> capped;
    uint64_t cappedFrames = 0;
    uint32_t cappedChannels = 0;
    uint32_t cappedRate = 0;
    const bool overCap = AudioEngine::DecodeFileToInterleavedF32(
        wav, capped, cappedFrames, cappedChannels, cappedRate, nullptr, 100);
    g_reportZeroLength = false;
    CHECK(!overCap, "decode cap rejects a file larger than the frame budget");
    CHECK(capped.empty(), "rejected capped decode retains no samples");
    std::remove(wav.c_str());

    WritePcmWav(wav, 480001, [](uint32_t frame) -> int16_t {
        if (frame == 2400) return -24576;
        if (frame == 480000) return 16384;
        return 0;
    });
    std::vector<float> dragPeaks;
    const bool dragWaveformOk = WaveformExtractor::ExtractExactMonoEnvelope(wav, 100, dragPeaks);
    CHECK(dragWaveformOk && dragPeaks.size() == 100, "drag waveform has 100 bounded peaks");
    CHECK(dragPeaks.size() == 100 && dragPeaks.front() >= 0.74f && dragPeaks[1] == 0.0f,
          "drag waveform retains a negative hit between quick scan windows");
    CHECK(dragPeaks.size() == 100 && dragPeaks.back() >= 0.49f && dragPeaks[98] == 0.0f,
          "drag waveform retains the final frame without shifting nearby silence");
    std::remove(wav.c_str());
    std::printf("\n=== LoudnessFromSamples: %d passed, %d failed ===\n", passed, failed);
    return failed ? 1 : 0;
}
