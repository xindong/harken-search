#pragma once

#include "FeatureExtractor.h"
#include "SimilaritySearch.h"
#include <array>
#include <string>
#include <vector>

namespace HarkenSearch {

enum class SimilarityMode { Perceptual = 0, Substitute = 1 };
struct SimilarityOptions {
    SimilarityMode mode = SimilarityMode::Perceptual;
    bool deep = false;
    SimilarityPreset preset = SimilarityPreset::Balanced;
    static constexpr int VERSION = 2;
};

// Owned, versioned analysis. Frames retain relative level and chronological
// order; no filename, library normalization, or padded model samples enter it.
struct AcousticFrame {
    std::array<float, 13> mfcc{};
    std::array<float, 5> spectral{};
    float energy = 0.0f;
    float pitch = 0.0f;
    float voiced = 0.0f;
};
struct AcousticAnalysis {
    static constexpr int VERSION = 2;
    static constexpr int SAMPLE_RATE = 22050;
    static constexpr int FRAME_SAMPLES = 4096;
    static constexpr int HOP_SAMPLES = 2048;
    float sourceSeconds = 0.0f;
    float activeStart = 0.0f;
    float activeSeconds = 0.0f;
    std::array<float, 13> mean{}, deviation{}, change{};
    std::array<float, 5> spectral{};
    std::array<float, 32> envelope{};
    float pitch = 0.0f, pitchSpread = 0.0f, voiced = 0.0f;
    float attack = 0.0f, decay = 0.0f;
    std::vector<float> onsets;
    std::vector<AcousticFrame> frames;
    bool IsValid(bool requireFrames = true) const;
    std::vector<uint8_t> Serialize(bool includeFrames = true) const;
    static bool Deserialize(const void* data, size_t size, AcousticAnalysis& out, bool requireFrames = true);
};
struct AcousticAnalysisResult {
    bool ok = false;
    AcousticAnalysis analysis;
    FeatureExtractionError error = FeatureExtractionError::None;
};
AcousticAnalysisResult AnalyzeSimilarityPcm(const float* samples, size_t count,
    const FeatureExtractionCancelCheck& cancel = {});
AcousticAnalysisResult AnalyzeSimilarityFile(const std::string& path,
    const FeatureExtractionCancelCheck& cancel = {});
// Streaming input seam: reads exactly the requested, in-range mono samples.
// Returning false aborts analysis; the callback owns decoder/PCM lifetime.
using SimilaritySampleReader = std::function<bool(size_t, size_t, float*)>;
AcousticAnalysisResult AnalyzeSimilarityStream(size_t sampleCount,
    const SimilaritySampleReader& read,
    const FeatureExtractionCancelCheck& cancel = {});

struct SimilarityScore {
    bool valid = false;
    float total = 0.0f; // 0..1; presentation multiplies by 100, never a probability.
    std::array<float, 6> parts{}; // C, T, E, R, P, D
    std::array<float, 6> weights{};
};
SimilarityScore ScoreSimilarity(const AcousticAnalysis& reference,
    const AcousticAnalysis& candidate, const SimilarityOptions& options,
    float categorySimilarity = -1.0f,
    const FeatureExtractionCancelCheck& cancel = {});

} // namespace HarkenSearch
