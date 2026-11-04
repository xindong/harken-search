#include "AcousticSimilarity.h"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>

using namespace HarkenSearch;
namespace {
int failures = 0;
void Check(bool ok, const char* message) {
    std::cout << (ok ? "PASS: " : "FAIL: ") << message << '\n';
    if (!ok) ++failures;
}
std::vector<float> Sound(float seconds, float gain = 0.5f, bool reverse = false) {
    std::vector<float> pcm(static_cast<size_t>(seconds * 22050));
    for (size_t i = 0; i < pcm.size(); ++i) {
        const float t = static_cast<float>(i) / 22050.0f;
        const float event = std::fmod(t, 1.0f);
        const float env = std::exp(-8.0f * event);
        const float freq = t < seconds * 0.5f ? 330.0f : 880.0f;
        pcm[i] = gain * env * std::sin(6.28318530718f * freq * t);
    }
    if (reverse) std::reverse(pcm.begin(), pcm.end());
    return pcm;
}
}
int main() {
    SimilarityOptions options;
    auto pcm = Sound(3.0f);
    const auto base = AnalyzeSimilarityPcm(pcm.data(), pcm.size());
    Check(base.ok && base.analysis.IsValid(), "real PCM produces a valid V2 analysis");
    auto louder = pcm;
    for (auto& v : louder) v *= 0.1f;
    const auto quiet = AnalyzeSimilarityPcm(louder.data(), louder.size());
    const auto gainScore = ScoreSimilarity(base.analysis, quiet.analysis, options);
    Check(gainScore.valid && gainScore.total > 0.995f, "overall gain does not alter perceptual similarity");
    auto padded = std::vector<float>(22050, 0.0f);
    padded.insert(padded.end(), pcm.begin(), pcm.end());
    padded.resize(padded.size() + 44100, 0.0f);
    const auto silence = AnalyzeSimilarityPcm(padded.data(), padded.size());
    Check(silence.ok && std::fabs(base.analysis.activeSeconds - silence.analysis.activeSeconds) < 0.001f,
          "leading and trailing silence are excluded from duration");
    Check(ScoreSimilarity(base.analysis, silence.analysis, options).total > 0.995f,
          "silence padding does not shift the comparison grid");
    const auto reversedPcm = Sound(3.0f, 0.5f, true);
    const auto reversed = AnalyzeSimilarityPcm(reversedPcm.data(), reversedPcm.size());
    options.deep = true;
    const auto identical = ScoreSimilarity(base.analysis, base.analysis, options);
    const auto backwards = ScoreSimilarity(base.analysis, reversed.analysis, options);
    Check(identical.valid && identical.total > 0.999f, "identical audio retains full deep match");
    Check(backwards.valid && backwards.total < identical.total - 0.1f, "deep comparison preserves event order and envelope direction");
    auto rising = base.analysis;
    rising.voiced = 1; rising.pitch = 440; rising.pitchSpread = 0.5f;
    for (size_t i = 0; i < rising.frames.size(); ++i) {
        rising.frames[i].voiced = 1;
        rising.frames[i].pitch = 220.0f * std::pow(4.0f, float(i) / (rising.frames.size() - 1));
    }
    auto falling = rising;
    for (size_t i = 0; i < falling.frames.size(); ++i)
        falling.frames[i].pitch = rising.frames[rising.frames.size() - 1 - i].pitch;
    Check(ScoreSimilarity(rising, falling, options).parts[4] <
          ScoreSimilarity(rising, rising, options).parts[4] - 0.2f,
          "deep pitch distinguishes rising and falling contours with the same median and range");
    auto gap = pcm;
    gap.insert(gap.begin() + gap.size() / 2, 22050, 0.0f);
    const auto paused = AnalyzeSimilarityPcm(gap.data(), gap.size());
    Check(paused.analysis.activeSeconds > base.analysis.activeSeconds + 0.9f, "internal silence remains part of the event structure");
    Check(ScoreSimilarity(base.analysis, paused.analysis, options).total < 0.98f, "internal pauses affect the match");
    std::vector<float> longPcm(35 * 22050, 0.0f);
    longPcm.insert(longPcm.end(), pcm.begin(), pcm.end());
    const auto late = AnalyzeSimilarityPcm(longPcm.data(), longPcm.size());
    Check(late.ok && late.analysis.activeStart > 34.9f, "events after thirty seconds participate");
    Check(ScoreSimilarity(base.analysis, late.analysis, options).total > 0.995f, "late event is compared using its audio");
    options.mode = SimilarityMode::Substitute;
    Check(!ScoreSimilarity(base.analysis, base.analysis, options).valid, "substitute refuses a missing semantic score");
    const auto sameType = ScoreSimilarity(base.analysis, base.analysis, options, 0.8f);
    Check(sameType.valid && std::fabs(sameType.weights[0] - 0.4f) < 0.0001f, "category retains its forty-percent share");
    auto unpitched = base.analysis;
    unpitched.voiced = 0.0f; unpitched.pitch = 0.0f;
    const auto noPitchQuery = ScoreSimilarity(unpitched, base.analysis, options, 0.8f);
    Check(noPitchQuery.valid && noPitchQuery.weights[4] == 0.0f, "pitch applicability comes from the reference");
    const auto missingPitchCandidate = ScoreSimilarity(base.analysis, unpitched, options, 0.8f);
    Check(missingPitchCandidate.valid && missingPitchCandidate.weights[4] > 0.0f && missingPitchCandidate.parts[4] == 0.0f,
          "candidate missing expected pitch cannot opt out");
    auto corrupt = base.analysis;
    corrupt.activeSeconds = std::numeric_limits<float>::quiet_NaN();
    Check(!ScoreSimilarity(base.analysis, corrupt, options, 0.8f).valid, "corrupt analysis is rejected");
    const auto blob = base.analysis.Serialize();
    AcousticAnalysis restored;
    Check(AcousticAnalysis::Deserialize(blob.data(), blob.size(), restored) && restored.IsValid(), "versioned analysis round-trips");
    Check(!AcousticAnalysis::Deserialize(blob.data(), blob.size() / 2, restored), "truncated analysis is rejected");
    Check(AnalyzeSimilarityPcm(pcm.data(), pcm.size(), [] { return true; }).error == FeatureExtractionError::Cancelled, "analysis observes cancellation");
    std::vector<float> zeros(22050, 0.0f);
    Check(!AnalyzeSimilarityPcm(zeros.data(), zeros.size()).ok, "silence does not become a perfect match");
    return failures ? 1 : 0;
}
