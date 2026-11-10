#include "AudioEmbeddingService.h"
#include "AcousticSimilarity.h"
#include "platform/audio_file.h"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
using namespace HarkenSearch;
std::vector<float> Read(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) return {};
    const auto bytes = input.tellg(); input.seekg(0);
    std::vector<float> data(static_cast<size_t>(bytes) / sizeof(float));
    input.read(reinterpret_cast<char*>(data.data()), bytes);
    return input ? data : std::vector<float>{};
}
int main(int argc, char** argv) {
    if (argc != 2) { std::cerr << "A pinned exported-model fixture directory is required\n"; return 2; }
    int failures = 0;
    auto check = [&](bool good, const char* message) { std::cout << (good ? "PASS: " : "FAIL: ") << message << '\n'; if (!good) ++failures; };
    const auto root = std::filesystem::u8path(argv[1]);
    const auto pcm = Read(root / "parity-audio.f32");
    const auto expectedMel = Read(root / "parity-mel.f32");
    const auto expectedEmbedding = Read(root / "parity-embedding.f32");
    if (pcm.size() != 480000 || expectedMel.size() != 1001 * 64 || expectedEmbedding.size() != 512) {
        std::cerr << "Official inference fixture is incomplete\n"; return 2;
    }
    AudioEmbeddingService service; std::string error;
    check(service.Load(root.u8string(), error) && service.IsReady(), "pinned local audio model loads");
    std::vector<float> actualMel;
    const bool melOk = service.MakeMel(pcm.data(), pcm.size(), actualMel, error);
    float maxError = std::numeric_limits<float>::infinity();
    if (melOk && actualMel.size() == expectedMel.size()) {
        maxError = 0; for (size_t i = 0; i < actualMel.size(); ++i) maxError = std::max(maxError, std::fabs(actualMel[i] - expectedMel[i]));
    }
    std::cout << "mel_max_absolute_error=" << maxError << '\n';
    check(melOk && maxError < 0.005f, "native preprocessing matches the official fixed fixture");
    AudioEmbedding actual{}, expected{}; std::copy(expectedEmbedding.begin(), expectedEmbedding.end(), expected.begin());
    const bool encoded = service.EncodeWindow(pcm.data(), pcm.size(), actual, error);
    const float cosine = AudioEmbeddingCosine(actual, expected);
    std::cout << "embedding_cosine=" << cosine << " error=" << error << '\n';
    check(encoded && cosine > 0.9999f, "native CPU inference matches the official audio encoder");
    if (std::filesystem::exists(root / "real-parity-audio.f32")) {
        const auto realPcm = Read(root / "real-parity-audio.f32");
        const auto realMel = Read(root / "real-parity-mel.f32");
        const auto realEmbedding = Read(root / "real-parity-embedding.f32");
        bool realOk = !realPcm.empty() && realPcm.size() <= 480000 && realEmbedding.size() == 512 &&
            service.MakeMel(realPcm.data(), realPcm.size(), actualMel, error) && realMel.size() == actualMel.size();
        float difference = 0;
        if (realOk) for (size_t i = 0; i < realMel.size(); ++i) difference = std::max(difference, std::fabs(realMel[i] - actualMel[i]));
        check(realOk && difference < 0.005f, "native preprocessing matches official inference on a real sound");
        if (realEmbedding.size() == 512) std::copy(realEmbedding.begin(), realEmbedding.end(), expected.begin());
        realOk = realOk && service.EncodeWindow(realPcm.data(), realPcm.size(), actual, error);
        std::cout << "real_embedding_cosine=" << AudioEmbeddingCosine(actual, expected) << '\n';
        check(realOk && AudioEmbeddingCosine(actual, expected) > 0.9999f, "native real-sound embedding matches the official audio encoder");
    }
    const auto filePath = (root / "native-long-window.wav").u8string();
    std::vector<float> longer(504000);
    for (size_t i = 0; i < longer.size(); ++i) longer[i] = pcm[i % pcm.size()];
    auto write = [&] {
        Platform::AudioFile::FloatWavWriter writer;
        return writer.Open(filePath, 48000, 1) && writer.Write(longer.data(), longer.size());
    };
    check(write(), "native model fixture writes");
    const auto fileEncoded = service.EncodeFile(filePath, 0, 10.5f);
    check(fileEncoded.ok && fileEncoded.segments.size() == 2 && std::fabs(fileEncoded.segments.back().endSeconds - 10.5f) < 0.001f,
        "native file analysis covers the final partial window without counting padding as duration");
    for (float& value : longer) value *= 0.1f;
    check(write(), "quieter native model fixture writes");
    const auto quieter = service.EncodeFile(filePath, 0, 10.5f);
    check(quieter.ok && fileEncoded.ok && AudioEmbeddingCosine(quieter.pooled, fileEncoded.pooled) > 0.9999f,
        "semantic analysis is invariant to overall recording gain");
    for (int rate : {44100, 96000, 192000}) {
        const auto convertedPath = (root / ("model-resampling-" + std::to_string(rate) + ".wav")).u8string();
        std::vector<float> source(rate * 2 + 1);
        for (size_t i = 0; i < source.size(); ++i) source[i] = 0.4f + 0.1f * float(std::sin(6.28318530718 * 440 * i / rate));
        { Platform::AudioFile::FloatWavWriter writer; writer.Open(convertedPath, rate, 1); writer.Write(source.data(), source.size()); }
        const auto acoustic = AnalyzeSimilarityFile(convertedPath);
        const auto semantic = service.EncodeFile(convertedPath, acoustic.analysis.activeStart, acoustic.analysis.activeSeconds);
        std::cout << "resampling=" << rate << " semantic_error=" << semantic.error << '\n';
        check(acoustic.ok && semantic.ok, "acoustic and semantic sample grids agree at a fractional file ending");
    }
    error.clear();
    check(!service.EncodeWindow(pcm.data(), pcm.size(), actual, error, [] { return true; }) && error == "cancelled", "model inference observes cancellation");
    check(!service.MakeMel(nullptr, 0, actualMel, error), "empty audio is rejected");
    auto corrupt = pcm; corrupt[100] = std::numeric_limits<float>::quiet_NaN();
    check(!service.MakeMel(corrupt.data(), corrupt.size(), actualMel, error), "non-finite PCM is rejected");
    AudioEmbedding invalid{};
    check(!IsValidAudioEmbedding(invalid), "zero embeddings cannot enter the index");
    check(!service.Load((root / "missing").u8string(), error) && !service.IsReady(), "failed model replacement does not retain the previous model");
    return failures ? 1 : 0;
}
