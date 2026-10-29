#pragma once
// FeatureExtractor.h — Audio feature extraction for similarity search (F-028)
// Computes a 48-dimensional feature vector per audio file:
//   MFCC[13] + MFCC_Delta[13] + Spectral[5] + Pitch[3] + Energy[10] + Global[4]

#include <vector>
#include <string>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <functional>

// @stage7-contract
// API contract: feature extraction returns owned fixed-version vectors and does
// not retain PCM/file input. VERSION/DIMS and segment offsets are compatibility
// values; invalid or unsupported audio reports failure through existing results.

// Feature Vector

struct AudioFeatureVector {
    static constexpr int VERSION = 1;
    static constexpr int DIMS = 48;

    // Segment offsets
    static constexpr int MFCC_OFFSET      = 0;   // [0..12]   13 dims — timbre
    static constexpr int MFCC_D_OFFSET    = 13;  // [13..25]  13 dims — timbre delta
    static constexpr int SPECTRAL_OFFSET  = 26;  // [26..30]  5 dims  — spectral shape
    static constexpr int PITCH_OFFSET     = 31;  // [31..33]  3 dims  — F0, F0 std, voiced ratio
    static constexpr int ENERGY_OFFSET    = 34;  // [34..43]  10 dims — 8-seg RMS + attack + ZCR
    static constexpr int GLOBAL_OFFSET    = 44;  // [44..47]  4 dims  — LUFS, RMS, duration, crest

    float features[DIMS] = {};

    // Convenience accessors
    float  spectralCentroid()  const { return features[SPECTRAL_OFFSET + 0]; }
    float  spectralBandwidth() const { return features[SPECTRAL_OFFSET + 1]; }
    float  spectralRolloff()   const { return features[SPECTRAL_OFFSET + 2]; }
    float  spectralFlux()      const { return features[SPECTRAL_OFFSET + 3]; }
    float  spectralFlatness()  const { return features[SPECTRAL_OFFSET + 4]; }
    float  medianF0()          const { return features[PITCH_OFFSET + 0]; }
    float  f0Std()             const { return features[PITCH_OFFSET + 1]; }
    float  voicedRatio()       const { return features[PITCH_OFFSET + 2]; }
    float  attackTime()        const { return features[ENERGY_OFFSET + 8]; }
    float  zeroCrossingRate()  const { return features[ENERGY_OFFSET + 9]; }

    // Serialization (raw float array, little-endian)
    std::vector<uint8_t> serialize() const {
        std::vector<uint8_t> buf(sizeof(int) + sizeof(features));
        int ver = VERSION;
        std::memcpy(buf.data(), &ver, sizeof(int));
        std::memcpy(buf.data() + sizeof(int), features, sizeof(features));
        return buf;
    }

    static bool deserialize(const void* blob, int blobSize, AudioFeatureVector& out) {
        const int expected = sizeof(int) + DIMS * sizeof(float);
        if (!blob || blobSize < expected) return false;
        int ver = 0;
        std::memcpy(&ver, blob, sizeof(int));
        if (ver != VERSION) return false;
        std::memcpy(out.features, (const uint8_t*)blob + sizeof(int), sizeof(out.features));
        return true;
    }

    bool isValid() const {
        for (int i = 0; i < DIMS; ++i)
            if (std::isnan(features[i]) || std::isinf(features[i])) return false;
        return true;
    }
};

enum class FeatureExtractionError {
    None,
    UnsupportedFormat,
    DecodeFailed,
    NoSamples,
    Cancelled,
};

struct FeatureExtractionResult {
    bool ok = false;
    AudioFeatureVector features;
    FeatureExtractionError error = FeatureExtractionError::None;
};

using FeatureExtractionCancelCheck = std::function<bool()>;

// Feature Extractor

class FeatureExtractor {
public:
    // Extract features from mono PCM float data.
    // sampleRate: original sample rate of the audio.
    // Returns a 48-dim feature vector (global features left at 0, caller fills them).
    static AudioFeatureVector extract(const float* samples, size_t numSamples, int sampleRate);

    // Extract features from file (decodes internally via miniaudio).
    // Downsamples to 22050 Hz mono for consistency.
    static AudioFeatureVector extractFromFile(const std::string& filePath);

    static FeatureExtractionResult extractFromFileChecked(
        const std::string& filePath,
        const FeatureExtractionCancelCheck& cancelCheck = {});

    // Fill the 4 global features from pre-computed DB values.
    static void fillGlobalFeatures(AudioFeatureVector& vec,
                                   float integratedLufs, float trueRmsDbfs,
                                   float durationSec, float peakDb);
};
