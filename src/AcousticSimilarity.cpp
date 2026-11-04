#include "AcousticSimilarity.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <numeric>

namespace HarkenSearch {
namespace {
constexpr size_t kMaxFrames = 262144;
constexpr uint32_t kMagic = 0x32534b48; // HKS2
bool Cancelled(const FeatureExtractionCancelCheck& cancel) { return cancel && cancel(); }
float Unit(float value) { return std::clamp(value, 0.0f, 1.0f); }
float Ratio(float a, float b, float floor = 0.01f) {
    a = std::max(a, floor); b = std::max(b, floor);
    return std::min(a, b) / std::max(a, b);
}
template<size_t N> bool Finite(const std::array<float, N>& values) {
    return std::all_of(values.begin(), values.end(), [](float v) { return std::isfinite(v); });
}
float Median(std::vector<float> values) {
    if (values.empty()) return 0.0f;
    const size_t mid = values.size() / 2;
    std::nth_element(values.begin(), values.begin() + mid, values.end());
    return values[mid];
}
void Summarize(AcousticAnalysis& out) {
    const size_t n = out.frames.size();
    float maxEnergy = 0.0f;
    std::vector<float> pitches;
    size_t peak = 0;
    for (size_t f = 0; f < n; ++f) {
        const auto& frame = out.frames[f];
        for (size_t i = 0; i < out.mean.size(); ++i) {
            out.mean[i] += frame.mfcc[i] / n;
            if (f) out.change[i] += std::fabs(frame.mfcc[i] - out.frames[f - 1].mfcc[i]) / (n - 1);
        }
        for (size_t i = 0; i < out.spectral.size(); ++i) out.spectral[i] += frame.spectral[i] / n;
        if (frame.energy > maxEnergy) { maxEnergy = frame.energy; peak = f; }
        if (frame.voiced >= 0.1f && frame.pitch > 0.0f) pitches.push_back(frame.pitch);
    }
    for (auto& frame : out.frames) {
        frame.energy = maxEnergy > 0.0f ? frame.energy / maxEnergy : 0.0f;
        for (size_t i = 0; i < out.mean.size(); ++i) {
            const float d = frame.mfcc[i] - out.mean[i];
            out.deviation[i] += d * d / n;
        }
    }
    for (auto& value : out.deviation) value = std::sqrt(value);
    for (size_t i = 0; i < out.envelope.size(); ++i) {
        const float at = static_cast<float>(i) * (n - 1) / (out.envelope.size() - 1);
        const size_t left = static_cast<size_t>(at);
        const size_t right = std::min(left + 1, n - 1);
        out.envelope[i] = out.frames[left].energy + (at - left) * (out.frames[right].energy - out.frames[left].energy);
    }
    const float hop = static_cast<float>(AcousticAnalysis::HOP_SAMPLES) / AcousticAnalysis::SAMPLE_RATE;
    out.attack = std::min(peak * hop, out.activeSeconds);
    out.decay = std::max(0.0f, out.activeSeconds - out.attack);
    for (size_t f = 0; f < n; ++f) {
        const float energy = out.frames[f].energy;
        const float before = f ? out.frames[f - 1].energy : 0.0f;
        if (energy > 0.1f && (f == 0 || energy > std::max(0.05f, before * 1.8f))) {
            const float when = f * hop;
            if (out.onsets.empty() || when - out.onsets.back() >= 0.12f) out.onsets.push_back(when);
        }
    }
    out.voiced = static_cast<float>(pitches.size()) / n;
    out.pitch = Median(pitches);
    if (out.pitch > 0.0f) {
        float squares = 0.0f;
        for (float pitch : pitches) { const float d = std::log2(pitch / out.pitch); squares += d * d; }
        out.pitchSpread = std::sqrt(squares / pitches.size());
    }
}
float SequenceScore(const AcousticAnalysis& a, const AcousticAnalysis& b,
                    const FeatureExtractionCancelCheck& cancel, bool pitchOnly = false) {
    const size_t n = std::min<size_t>(256, a.frames.size());
    const size_t m = std::min<size_t>(256, b.frames.size());
    const float infinity = std::numeric_limits<float>::infinity();
    std::vector<float> previous(m + 1, infinity), current(m + 1, infinity);
    previous[0] = 0.0f;
    for (size_t i = 1; i <= n; ++i) {
        if (Cancelled(cancel)) return -1.0f;
        std::fill(current.begin(), current.end(), infinity);
        const auto& af = a.frames[(i - 1) * (a.frames.size() - 1) / std::max<size_t>(1, n - 1)];
        for (size_t j = 1; j <= m; ++j) {
            if (std::fabs(static_cast<float>(i) / n - static_cast<float>(j) / m) > 0.2f + 1.0f / std::min(n, m)) continue;
            const auto& bf = b.frames[(j - 1) * (b.frames.size() - 1) / std::max<size_t>(1, m - 1)];
            float timbre = 0.0f;
            for (size_t k = 0; k < af.mfcc.size(); ++k) timbre += std::fabs(af.mfcc[k] - bf.mfcc[k]) / (45.0f * af.mfcc.size());
            float local = 0.6f * std::min(2.0f, timbre) + 0.4f * std::fabs(af.energy - bf.energy);
            if (pitchOnly) {
                const bool ap = af.voiced >= 0.1f && af.pitch > 0;
                const bool bp = bf.voiced >= 0.1f && bf.pitch > 0;
                local = ap && bp ? std::min(2.0f, std::fabs(std::log2(af.pitch / bf.pitch))) :
                    (ap != bp ? 1.0f : 0.0f);
            }
            // Non-diagonal moves have a small cost: stretching cannot freely
            // turn many repeated events into one. Both file ends must match.
            current[j] = local + std::min(previous[j - 1], std::min(previous[j] + 0.015f, current[j - 1] + 0.015f));
        }
        previous.swap(current);
    }
    return std::isfinite(previous[m]) ? std::exp(-4.0f * previous[m] / std::max(n, m)) : 0.0f;
}
}

bool AcousticAnalysis::IsValid(bool requireFrames) const {
    if (!std::isfinite(sourceSeconds) || !std::isfinite(activeStart) || !std::isfinite(activeSeconds) ||
        sourceSeconds <= 0 || activeStart < 0 || activeSeconds <= 0 ||
        activeStart + activeSeconds > sourceSeconds + 0.002f || (requireFrames && frames.empty()) || frames.size() > kMaxFrames ||
        !Finite(mean) || !Finite(deviation) || !Finite(change) || !Finite(spectral) || !Finite(envelope) ||
        !std::isfinite(pitch) || !std::isfinite(pitchSpread) || !std::isfinite(voiced) || voiced < 0 || voiced > 1 ||
        !std::isfinite(attack) || !std::isfinite(decay) || attack < 0 || decay < 0) return false;
    float previous = -1.0f;
    for (float onset : onsets) {
        if (!std::isfinite(onset) || onset < 0 || onset <= previous || onset > activeSeconds) return false;
        previous = onset;
    }
    for (const auto& frame : frames) {
        if (!Finite(frame.mfcc) || !Finite(frame.spectral) || !std::isfinite(frame.energy) ||
            frame.energy < 0 || frame.energy > 1.001f || !std::isfinite(frame.pitch) || frame.pitch < 0 ||
            !std::isfinite(frame.voiced) || frame.voiced < 0 || frame.voiced > 1) return false;
    }
    return true;
}

std::vector<uint8_t> AcousticAnalysis::Serialize(bool includeFrames) const {
    if (!IsValid()) return {};
    std::vector<uint8_t> data;
    auto put = [&](const auto& value) {
        const auto* bytes = reinterpret_cast<const uint8_t*>(&value);
        data.insert(data.end(), bytes, bytes + sizeof(value));
    };
    put(kMagic); const uint32_t version = VERSION; put(version);
    put(sourceSeconds); put(activeStart); put(activeSeconds);
    put(mean); put(deviation); put(change); put(spectral); put(envelope);
    put(pitch); put(pitchSpread); put(voiced); put(attack); put(decay);
    const uint32_t onsetCount = static_cast<uint32_t>(onsets.size()); put(onsetCount);
    for (float onset : onsets) put(onset);
    const uint32_t frameCount = includeFrames ? static_cast<uint32_t>(frames.size()) : 0; put(frameCount);
    if (includeFrames) for (const auto& frame : frames) { put(frame.mfcc); put(frame.spectral); put(frame.energy); put(frame.pitch); put(frame.voiced); }
    return data;
}
bool AcousticAnalysis::Deserialize(const void* raw, size_t size, AcousticAnalysis& out, bool requireFrames) {
    if (!raw || size < 8) return false;
    const auto* data = static_cast<const uint8_t*>(raw); size_t at = 0;
    auto get = [&](auto& value) {
        if (sizeof(value) > size - at) return false;
        std::memcpy(&value, data + at, sizeof(value)); at += sizeof(value); return true;
    };
    uint32_t magic = 0, version = 0, count = 0; AcousticAnalysis parsed;
    if (!get(magic) || !get(version) || magic != kMagic || version != VERSION ||
        !get(parsed.sourceSeconds) || !get(parsed.activeStart) || !get(parsed.activeSeconds) ||
        !get(parsed.mean) || !get(parsed.deviation) || !get(parsed.change) || !get(parsed.spectral) || !get(parsed.envelope) ||
        !get(parsed.pitch) || !get(parsed.pitchSpread) || !get(parsed.voiced) || !get(parsed.attack) || !get(parsed.decay) ||
        !get(count) || count > kMaxFrames || count > (size - at) / sizeof(float)) return false;
    parsed.onsets.resize(count); for (auto& v : parsed.onsets) if (!get(v)) return false;
    if (!get(count) || (requireFrames && count == 0) || count > kMaxFrames || count > (size - at) / (21 * sizeof(float))) return false;
    parsed.frames.resize(count);
    for (auto& frame : parsed.frames) if (!get(frame.mfcc) || !get(frame.spectral) || !get(frame.energy) || !get(frame.pitch) || !get(frame.voiced)) return false;
    if (at != size || !parsed.IsValid(requireFrames)) return false;
    out = std::move(parsed); return true;
}

AcousticAnalysisResult AnalyzeSimilarityStream(size_t count, const SimilaritySampleReader& read,
                                               const FeatureExtractionCancelCheck& cancel) {
    AcousticAnalysisResult result;
    auto fail = [&](FeatureExtractionError error) { result.error = error; return result; };
    if (Cancelled(cancel)) return fail(FeatureExtractionError::Cancelled);
    if (!count) return fail(FeatureExtractionError::NoSamples);
    constexpr size_t block = 8192;
    std::vector<float> buffer(block), peaks; float peak = 0.0f;
    for (size_t at = 0; at < count; at += block) {
        if (Cancelled(cancel)) return fail(FeatureExtractionError::Cancelled);
        const size_t length = std::min(block, count - at);
        if (!read(at, length, buffer.data())) return fail(FeatureExtractionError::DecodeFailed);
        float local = 0.0f;
        for (size_t i = 0; i < length; ++i) {
            if (!std::isfinite(buffer[i])) return fail(FeatureExtractionError::DecodeFailed);
            local = std::max(local, std::fabs(buffer[i]));
        }
        peak = std::max(peak, local); peaks.push_back(local);
    }
    if (peak <= 1e-8f) return fail(FeatureExtractionError::NoSamples);
    const float threshold = peak * 0.001f;
    size_t firstBlock = 0, lastBlock = peaks.size() - 1;
    while (peaks[firstBlock] < threshold) ++firstBlock;
    while (peaks[lastBlock] < threshold) --lastBlock;
    size_t first = firstBlock * block, last = std::min(count, (lastBlock + 1) * block);
    if (!read(first, std::min(block, count - first), buffer.data())) return fail(FeatureExtractionError::DecodeFailed);
    size_t i = 0; while (std::fabs(buffer[i]) < threshold) ++i; first += i;
    const size_t endBlock = lastBlock * block;
    if (!read(endBlock, last - endBlock, buffer.data())) return fail(FeatureExtractionError::DecodeFailed);
    while (last > endBlock && std::fabs(buffer[last - endBlock - 1]) < threshold) --last;
    AcousticAnalysis analysis;
    analysis.sourceSeconds = static_cast<float>(count) / AcousticAnalysis::SAMPLE_RATE;
    analysis.activeStart = static_cast<float>(first) / AcousticAnalysis::SAMPLE_RATE;
    analysis.activeSeconds = static_cast<float>(last - first) / AcousticAnalysis::SAMPLE_RATE;
    if ((last - first) / AcousticAnalysis::HOP_SAMPLES >= kMaxFrames) return fail(FeatureExtractionError::DecodeFailed);
    buffer.resize(AcousticAnalysis::FRAME_SAMPLES);
    for (size_t at = first; at < last; at += AcousticAnalysis::HOP_SAMPLES) {
        if (Cancelled(cancel)) return fail(FeatureExtractionError::Cancelled);
        const size_t length = std::min(buffer.size(), last - at);
        std::fill(buffer.begin(), buffer.end(), 0.0f);
        if (!read(at, length, buffer.data())) return fail(FeatureExtractionError::DecodeFailed);
        double sum = 0.0;
        for (size_t j = 0; j < length; ++j) { buffer[j] /= peak; sum += buffer[j] * buffer[j]; }
        const auto features = FeatureExtractor::extract(buffer.data(), buffer.size(), AcousticAnalysis::SAMPLE_RATE);
        AcousticFrame frame;
        std::copy_n(features.features, frame.mfcc.size(), frame.mfcc.begin());
        std::copy_n(features.features + AudioFeatureVector::SPECTRAL_OFFSET, frame.spectral.size(), frame.spectral.begin());
        frame.energy = static_cast<float>(std::sqrt(sum / length));
        frame.pitch = features.medianF0(); frame.voiced = features.voicedRatio();
        analysis.frames.push_back(frame);
    }
    Summarize(analysis);
    if (!analysis.IsValid()) return fail(FeatureExtractionError::DecodeFailed);
    result.ok = true; result.analysis = std::move(analysis); return result;
}
AcousticAnalysisResult AnalyzeSimilarityPcm(const float* samples, size_t count, const FeatureExtractionCancelCheck& cancel) {
    if (!samples && count) { AcousticAnalysisResult result; result.error = FeatureExtractionError::DecodeFailed; return result; }
    return AnalyzeSimilarityStream(count, [samples](size_t at, size_t size, float* out) {
        std::copy_n(samples + at, size, out); return true;
    }, cancel);
}

SimilarityScore ScoreSimilarity(const AcousticAnalysis& a, const AcousticAnalysis& b,
                               const SimilarityOptions& options, float category,
                               const FeatureExtractionCancelCheck& cancel) {
    SimilarityScore out;
    if (Cancelled(cancel) || !a.IsValid(options.deep) || !b.IsValid(options.deep)) return out;
    const bool substitute = options.mode == SimilarityMode::Substitute;
    if (substitute && (!std::isfinite(category) || category < 0 || category > 1)) return out;
    out.weights = substitute ? std::array<float, 6>{40,25,20,5,5,5} : std::array<float, 6>{0,35,30,15,10,10};
    out.parts[0] = substitute ? category : 0;
    float meanDistance = 0, spreadDistance = 0, changeDistance = 0, spectralDistance = 0;
    for (size_t i = 0; i < a.mean.size(); ++i) {
        meanDistance += std::fabs(a.mean[i] - b.mean[i]) / (45.0f * a.mean.size());
        spreadDistance += std::fabs(a.deviation[i] - b.deviation[i]) / (30.0f * a.mean.size());
        changeDistance += std::fabs(a.change[i] - b.change[i]) / (20.0f * a.mean.size());
    }
    for (size_t i = 0; i < 3; ++i) spectralDistance += std::fabs(a.spectral[i] - b.spectral[i]) / (11025.0f * 3);
    spectralDistance += std::fabs(a.spectral[4] - b.spectral[4]);
    const float cepstral = std::exp(-(meanDistance + 0.5f * spreadDistance + 0.5f * changeDistance));
    const float spectral = std::exp(-4.0f * spectralDistance);
    out.parts[1] = options.preset == SimilarityPreset::SpectralFocused ? (cepstral + 2 * spectral) / 3 : (cepstral + spectral) / 2;
    float envelopeDistance = 0;
    for (size_t i = 0; i < a.envelope.size(); ++i) envelopeDistance += std::fabs(a.envelope[i] - b.envelope[i]) / a.envelope.size();
    out.parts[2] = 0.7f * std::exp(-4.0f * envelopeDistance) + 0.15f * Ratio(a.attack, b.attack, 0.05f) + 0.15f * Ratio(a.decay, b.decay, 0.05f);
    const float countMatch = Ratio(static_cast<float>(a.onsets.size()), static_cast<float>(b.onsets.size()), 1.0f);
    const float densityMatch = Ratio(a.onsets.size() / a.activeSeconds, b.onsets.size() / b.activeSeconds, 0.01f);
    float intervalMatch = 1.0f;
    if (a.onsets.size() > 1 || b.onsets.size() > 1) {
        if (a.onsets.size() < 2 || b.onsets.size() < 2) intervalMatch = 0.0f;
        else {
            float difference = 0; const size_t n = std::min(a.onsets.size(), b.onsets.size());
            for (size_t i = 1; i < n; ++i) difference += 1 - Ratio(a.onsets[i] - a.onsets[i-1], b.onsets[i] - b.onsets[i-1]);
            intervalMatch = 1 - difference / (n - 1);
        }
    }
    out.parts[3] = (countMatch + densityMatch + intervalMatch) / 3;
    const bool pitchApplies = a.voiced >= 0.1f && a.pitch > 0.0f;
    if (pitchApplies && b.voiced >= 0.1f && b.pitch > 0.0f)
        out.parts[4] = std::exp(-2 * std::fabs(std::log2(a.pitch / b.pitch)) - std::fabs(a.pitchSpread - b.pitchSpread));
    out.parts[5] = Ratio(a.activeSeconds, b.activeSeconds);
    if (options.deep) {
        const float aligned = SequenceScore(a, b, cancel);
        if (aligned < 0.0f) return {};
        out.parts[1] = 0.5f * out.parts[1] + 0.5f * aligned;
        out.parts[2] = 0.5f * out.parts[2] + 0.5f * aligned;
        out.parts[3] *= aligned;
        if (pitchApplies && out.parts[4] > 0) {
            const float pitchAligned = SequenceScore(a, b, cancel, true);
            if (pitchAligned < 0.0f) return {};
            out.parts[4] = 0.5f * out.parts[4] + 0.5f * pitchAligned;
        }
    }
    if (!pitchApplies) out.weights[4] = 0;
    if (options.preset == SimilarityPreset::TimbreFocused) out.weights[1] *= 2;
    if (options.preset == SimilarityPreset::RhythmFocused) { out.weights[2] *= 2; out.weights[3] *= 2; }
    if (options.preset == SimilarityPreset::PitchFocused) out.weights[4] *= 3;
    const float acoustic = std::accumulate(out.weights.begin() + 1, out.weights.end(), 0.0f);
    for (size_t i = 1; i < out.weights.size(); ++i) out.weights[i] *= (substitute ? 0.6f : 1.0f) / acoustic;
    out.weights[0] = substitute ? 0.4f : 0.0f;
    for (size_t i = 0; i < out.parts.size(); ++i) { out.parts[i] = Unit(out.parts[i]); out.total += out.parts[i] * out.weights[i]; }
    out.total = Unit(out.total); out.valid = true; return out;
}
} // namespace HarkenSearch
