#include "AcousticSimilarity.h"
#include "platform/audio_file.h"
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace fs = std::filesystem;
using namespace HarkenSearch;
int main(int argc, char** argv) {
    const fs::path root = argc > 1 ? fs::u8path(argv[1]) : fs::temp_directory_path() / "harken-acoustic-v2-test";
    fs::create_directories(root);
    const fs::path path = root / fs::u8path(u8"后半段音效.wav");
    std::vector<int16_t> samples(38 * 22050, 0);
    for (size_t i = 35 * 22050; i < samples.size(); ++i)
        samples[i] = static_cast<int16_t>(12000 * std::sin(6.28318530718 * 440 * i / 22050.0));
    {
        std::ofstream wav(path, std::ios::binary);
        auto u16 = [&](uint16_t v) { wav.write(reinterpret_cast<const char*>(&v), 2); };
        auto u32 = [&](uint32_t v) { wav.write(reinterpret_cast<const char*>(&v), 4); };
        wav.write("RIFF", 4); u32(36 + static_cast<uint32_t>(samples.size() * 2)); wav.write("WAVEfmt ", 8);
        u32(16); u16(1); u16(1); u32(22050); u32(44100); u16(2); u16(16); wav.write("data", 4); u32(static_cast<uint32_t>(samples.size() * 2));
        wav.write(reinterpret_cast<const char*>(samples.data()), samples.size() * 2);
    }
    int failures = 0;
    auto check = [&](bool good, const char* message) { std::cout << (good ? "PASS: " : "FAIL: ") << message << '\n'; if (!good) ++failures; };
    const auto file = AnalyzeSimilarityFile(path.u8string());
    check(file.ok && file.analysis.sourceSeconds > 37.9f && file.analysis.activeStart > 34.9f, "production decoder covers late events and UTF-8 paths");
    std::vector<float> pcm(samples.size());
    for (size_t i = 0; i < samples.size(); ++i) pcm[i] = samples[i] / 32768.0f;
    const auto memory = AnalyzeSimilarityPcm(pcm.data(), pcm.size());
    const auto score = ScoreSimilarity(memory.analysis, file.analysis, SimilarityOptions{});
    check(score.valid && score.total > 0.999f, "streamed file analysis agrees with PCM analysis");
    check(AnalyzeSimilarityFile(path.u8string(), [] { return true; }).error == FeatureExtractionError::Cancelled, "file extraction cancels before opening");
    check(AnalyzeSimilarityFile((root / "missing.wav").u8string()).error == FeatureExtractionError::DecodeFailed, "file extraction reports unreadable input");
    int polls = 0;
    check(AnalyzeSimilarityFile(path.u8string(), [&] { return ++polls > 5; }).error == FeatureExtractionError::Cancelled, "streaming extraction cancels during reading");
    for (int rate : {44100, 48000, 96000, 192000}) {
        const auto resampledPath = (root / ("resampled-" + std::to_string(rate) + ".wav")).u8string();
        std::vector<float> source(static_cast<size_t>(rate * 4.12345));
        for (size_t i = 0; i < source.size(); ++i) source[i] = 0.7f * std::sin(6.28318530718 * 440 * i / rate);
        { Platform::AudioFile::FloatWavWriter writer; writer.Open(resampledPath, rate, 1); writer.Write(source.data(), source.size()); }
        const auto converted = AnalyzeSimilarityFile(resampledPath);
        std::cout << "input_rate=" << rate << " ok=" << converted.ok << " error=" << static_cast<int>(converted.error) << '\n';
        check(converted.ok && converted.analysis.sourceSeconds > 4.12f, "streaming analysis supports resampling at a non-aligned end position");
    }
    return failures ? 1 : 0;
}
