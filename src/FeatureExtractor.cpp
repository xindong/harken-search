// FeatureExtractor.cpp — Audio feature extraction for similarity search (F-028)
// See docs/plans/2026-03-20-F-028-查找相似音效-design.md for full design.

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "FeatureExtractor.h"
#include "platform/audio_file.h"
#include <complex>
#include <algorithm>
#include <cctype>
#include <numeric>
#include <cmath>
#include <string>
#include <vector>

// Constants

static constexpr int   kTargetSampleRate = 22050;
static constexpr int   kFrameSize        = 2048;
static constexpr int   kHopSize          = 1024;
static constexpr int   kNumMelFilters    = 40;
static constexpr int   kNumMFCC          = 13;
static constexpr float kPI               = 3.14159265358979f;

static bool isKnownUnsupportedFeatureExtension(const std::string& filePath) {
    const size_t dot = filePath.find_last_of('.');
    if (dot == std::string::npos) return false;

    std::string extension = filePath.substr(dot);
    for (char& c : extension)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    // The bundled miniaudio decoder used by similarity analysis supports
    // WAV/MP3/FLAC. These common library formats need another decoder path.
    return extension == ".m4a" || extension == ".wma" ||
           extension == ".ogg" || extension == ".aac" ||
           extension == ".mp4";
}

// FFT (Radix-2 Cooley-Tukey, matches main.cpp implementation)

static void computeFFT(std::vector<std::complex<float>>& x) {
    size_t N = x.size();
    if (N <= 1) return;

    // Bit-reversal permutation
    for (size_t i = 1, j = 0; i < N; i++) {
        size_t bit = N >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(x[i], x[j]);
    }

    // Cooley-Tukey iterative
    for (size_t len = 2; len <= N; len <<= 1) {
        float angle = -2.0f * kPI / (float)len;
        std::complex<float> wlen(cosf(angle), sinf(angle));
        for (size_t i = 0; i < N; i += len) {
            std::complex<float> w(1.0f, 0.0f);
            for (size_t j = 0; j < len / 2; j++) {
                std::complex<float> u = x[i + j];
                std::complex<float> t = w * x[i + j + len / 2];
                x[i + j] = u + t;
                x[i + j + len / 2] = u - t;
                w *= wlen;
            }
        }
    }
}

// IFFT (used for FFT-based autocorrelation in YIN pitch)

static void computeIFFT(std::vector<std::complex<float>>& x) {
    // IFFT via conjugate symmetry: IFFT(X) = (1/N) * conj(FFT(conj(X)))
    for (auto& v : x) v = std::conj(v);
    computeFFT(x);
    const float n = (float)x.size();
    for (auto& v : x) v = std::conj(v) / n;
}

// Mel Filter Bank

static float hzToMel(float hz) { return 2595.0f * std::log10(1.0f + hz / 700.0f); }
static float melToHz(float mel) { return 700.0f * (std::pow(10.0f, mel / 2595.0f) - 1.0f); }

struct MelFilterBank {
    int numFilters;
    int fftSize;
    // filterWeights[f][bin] — sparse, but for 40 filters × 1025 bins it's only ~160 KB
    std::vector<std::vector<float>> weights;

    void build(int nFilters, int fftSz, int sampleRate, float fLow = 20.0f, float fHigh = 0.0f) {
        numFilters = nFilters;
        fftSize = fftSz;
        int numBins = fftSz / 2 + 1;
        if (fHigh <= 0.0f) fHigh = (float)(sampleRate / 2);
        fHigh = std::min(fHigh, (float)(sampleRate / 2));

        float melLow = hzToMel(fLow);
        float melHigh = hzToMel(fHigh);
        float melSpacing = (melHigh - melLow) / (nFilters + 1);

        // Center frequencies in Hz
        std::vector<float> centers(nFilters + 2);
        for (int i = 0; i < nFilters + 2; ++i)
            centers[i] = melToHz(melLow + i * melSpacing);

        // Convert to FFT bin indices
        std::vector<int> binIdx(nFilters + 2);
        for (int i = 0; i < nFilters + 2; ++i)
            binIdx[i] = (int)std::round(centers[i] * fftSz / sampleRate);

        // Build triangular filters
        weights.assign(nFilters, std::vector<float>(numBins, 0.0f));
        for (int f = 0; f < nFilters; ++f) {
            int lo = binIdx[f], mid = binIdx[f + 1], hi = binIdx[f + 2];
            for (int b = lo; b <= mid && b < numBins; ++b) {
                if (mid > lo)
                    weights[f][b] = (float)(b - lo) / (mid - lo);
            }
            for (int b = mid; b <= hi && b < numBins; ++b) {
                if (hi > mid)
                    weights[f][b] = (float)(hi - b) / (hi - mid);
            }
        }
    }

    void apply(const float* powerSpectrum, float* melEnergies) const {
        int numBins = fftSize / 2 + 1;
        for (int f = 0; f < numFilters; ++f) {
            float sum = 0.0f;
            for (int b = 0; b < numBins; ++b)
                sum += weights[f][b] * powerSpectrum[b];
            melEnergies[f] = sum;
        }
    }
};

// Hamming Window

static void makeHammingWindow(float* window, int size) {
    for (int i = 0; i < size; ++i)
        window[i] = 0.54f - 0.46f * cosf(2.0f * kPI * i / (size - 1));
}

// MFCC Computation

static void computeMFCC(const float* samples, size_t numSamples, int sampleRate,
                                    float* mfccMean, float* mfccDeltaMean) {
    if (numSamples < (size_t)kFrameSize) {
        std::fill(mfccMean, mfccMean + kNumMFCC, 0.0f);
        std::fill(mfccDeltaMean, mfccDeltaMean + kNumMFCC, 0.0f);
        return;
    }

    // Static precomputed data — built once at first call (thread-safe via C++11 static init)
    static const MelFilterBank s_melBank = []() {
        MelFilterBank mb;
        mb.build(kNumMelFilters, kFrameSize, kTargetSampleRate, 20.0f, 20000.0f);
        return mb;
    }();
    static const std::vector<float> s_window = []() {
        std::vector<float> w(kFrameSize);
        makeHammingWindow(w.data(), kFrameSize);
        return w;
    }();
    // DCT-II cosine table [kNumMFCC+1][kNumMelFilters] — eliminates cosf from inner loop
    static const auto s_dctTable = []() {
        std::vector<std::vector<float>> t(kNumMFCC + 1, std::vector<float>(kNumMelFilters));
        for (int k = 0; k <= kNumMFCC; ++k)
            for (int n = 0; n < kNumMelFilters; ++n)
                t[k][n] = cosf(kPI * k * (2.0f * n + 1.0f) / (2.0f * kNumMelFilters));
        return t;
    }();

    const MelFilterBank& melBank = s_melBank;
    const std::vector<float>& window = s_window;

    int numBins = kFrameSize / 2 + 1;
    std::vector<float> powerSpec(numBins);
    std::vector<float> melEnergies(kNumMelFilters);
    std::vector<float> logMel(kNumMelFilters);

    // Accumulate MFCC per frame
    size_t numFrames = 0;
    std::vector<std::vector<float>> allMfcc;
    allMfcc.reserve((numSamples - kFrameSize) / kHopSize + 1);

    std::vector<std::complex<float>> fftBuf(kFrameSize);

    for (size_t offset = 0; offset + kFrameSize <= numSamples; offset += kHopSize) {
        // Window and FFT
        for (int i = 0; i < kFrameSize; ++i)
            fftBuf[i] = std::complex<float>(samples[offset + i] * window[i], 0.0f);
        computeFFT(fftBuf);

        // Power spectrum
        for (int i = 0; i < numBins; ++i) {
            float re = fftBuf[i].real(), im = fftBuf[i].imag();
            powerSpec[i] = re * re + im * im;
        }

        // Mel filter bank
        melBank.apply(powerSpec.data(), melEnergies.data());

        // Log + DCT → MFCC (skip c0, take c1..c13) — use precomputed cosine table
        for (int i = 0; i < kNumMelFilters; ++i)
            logMel[i] = std::log(melEnergies[i] + 1e-10f);

        std::vector<float> mfcc(kNumMFCC);
        for (int k = 0; k < kNumMFCC; ++k) {
            float sum = 0.0f;
            for (int n = 0; n < kNumMelFilters; ++n)
                sum += logMel[n] * s_dctTable[k + 1][n];  // k+1: skip c0
            mfcc[k] = sum;
        }

        allMfcc.push_back(std::move(mfcc));
        numFrames++;
    }

    if (numFrames == 0) {
        std::fill(mfccMean, mfccMean + kNumMFCC, 0.0f);
        std::fill(mfccDeltaMean, mfccDeltaMean + kNumMFCC, 0.0f);
        return;
    }

    // Mean MFCC
    std::fill(mfccMean, mfccMean + kNumMFCC, 0.0f);
    for (size_t f = 0; f < numFrames; ++f)
        for (int i = 0; i < kNumMFCC; ++i)
            mfccMean[i] += allMfcc[f][i];
    for (int i = 0; i < kNumMFCC; ++i)
        mfccMean[i] /= (float)numFrames;

    // Mean MFCC Delta (frame-to-frame difference)
    std::fill(mfccDeltaMean, mfccDeltaMean + kNumMFCC, 0.0f);
    if (numFrames > 1) {
        for (size_t f = 1; f < numFrames; ++f)
            for (int i = 0; i < kNumMFCC; ++i)
                mfccDeltaMean[i] += allMfcc[f][i] - allMfcc[f - 1][i];
        for (int i = 0; i < kNumMFCC; ++i)
            mfccDeltaMean[i] /= (float)(numFrames - 1);
    }
}

// Spectral Features

static void computeSpectralFeatures(const float* samples, size_t numSamples,
                                                int sampleRate, float* out) {
    // out[0]=centroid, out[1]=bandwidth, out[2]=rolloff, out[3]=flux, out[4]=flatness
    std::fill(out, out + 5, 0.0f);
    if (numSamples < (size_t)kFrameSize) return;

    // Reuse the same static Hamming window built in computeMFCC
    static const std::vector<float> s_window = []() {
        std::vector<float> w(kFrameSize);
        makeHammingWindow(w.data(), kFrameSize);
        return w;
    }();
    const std::vector<float>& window = s_window;

    int numBins = kFrameSize / 2 + 1;
    std::vector<float> mag(numBins);
    std::vector<float> prevMag(numBins, 0.0f);
    std::vector<std::complex<float>> fftBuf(kFrameSize);

    double sumCentroid = 0, sumBandwidth = 0, sumRolloff = 0, sumFlux = 0;
    double sumLogMag = 0, sumMag = 0;  // for flatness
    size_t numFrames = 0;

    float freqPerBin = (float)sampleRate / kFrameSize;

    for (size_t offset = 0; offset + kFrameSize <= numSamples; offset += kHopSize) {
        for (int i = 0; i < kFrameSize; ++i)
            fftBuf[i] = std::complex<float>(samples[offset + i] * window[i], 0.0f);
        computeFFT(fftBuf);

        // Magnitude spectrum
        float totalMag = 0.0f;
        float totalPow = 0.0f;
        for (int i = 0; i < numBins; ++i) {
            float re = fftBuf[i].real(), im = fftBuf[i].imag();
            mag[i] = std::sqrt(re * re + im * im);
            totalMag += mag[i];
            totalPow += mag[i] * mag[i];
        }

        if (totalMag < 1e-10f) {
            prevMag = mag;
            numFrames++;
            continue;
        }

        // Spectral centroid
        float centroid = 0.0f;
        for (int i = 0; i < numBins; ++i)
            centroid += (i * freqPerBin) * mag[i];
        centroid /= totalMag;
        sumCentroid += centroid;

        // Spectral bandwidth
        float bw = 0.0f;
        for (int i = 0; i < numBins; ++i) {
            float diff = i * freqPerBin - centroid;
            bw += diff * diff * mag[i];
        }
        bw = std::sqrt(bw / totalMag);
        sumBandwidth += bw;

        // Spectral rolloff (85%)
        float threshold = 0.85f * totalPow;
        float cumPow = 0.0f;
        float rolloff = 0.0f;
        for (int i = 0; i < numBins; ++i) {
            cumPow += mag[i] * mag[i];
            if (cumPow >= threshold) {
                rolloff = i * freqPerBin;
                break;
            }
        }
        sumRolloff += rolloff;

        // Spectral flux (half-wave rectified)
        float flux = 0.0f;
        for (int i = 0; i < numBins; ++i) {
            float diff = mag[i] - prevMag[i];
            if (diff > 0) flux += diff * diff;
        }
        sumFlux += std::sqrt(flux);

        // Spectral flatness accumulation (geometric mean / arithmetic mean)
        for (int i = 0; i < numBins; ++i) {
            sumLogMag += std::log(mag[i] + 1e-10f);
            sumMag += mag[i];
        }

        prevMag = mag;
        numFrames++;
    }

    if (numFrames > 0) {
        out[0] = (float)(sumCentroid / numFrames);
        out[1] = (float)(sumBandwidth / numFrames);
        out[2] = (float)(sumRolloff / numFrames);
        out[3] = (float)(sumFlux / numFrames);
        // Flatness: exp(mean(log(mag))) / mean(mag)
        double totalBins = (double)numFrames * (kFrameSize / 2 + 1);
        double geoMean = std::exp(sumLogMag / totalBins);
        double ariMean = sumMag / totalBins;
        out[4] = (ariMean > 1e-10) ? (float)(geoMean / ariMean) : 0.0f;
    }
}

// YIN Pitch Estimation

static void computePitch(const float* samples, size_t numSamples, int sampleRate,
                                     float* pitchOut) {
    // pitchOut[0]=medianF0, pitchOut[1]=f0Std, pitchOut[2]=voicedRatio
    std::fill(pitchOut, pitchOut + 3, 0.0f);

    const int frameSize = 2048;
    const int hopSize = 1024;
    const float threshold = 0.15f;
    int tauMin = std::max(1, sampleRate / 4000);  // 4000 Hz upper bound
    int tauMax = std::min(frameSize / 2, sampleRate / 50);   // 50 Hz lower bound

    if ((int)numSamples < frameSize || tauMax <= tauMin) return;

    std::vector<float> f0Values;
    f0Values.reserve((numSamples - frameSize) / hopSize + 1);

    std::vector<float> diff(tauMax + 1);
    std::vector<float> cmndf(tauMax + 1); // cumulative mean normalized difference.

    // FFT-based autocorrelation: zero-pad first W=frameSize/2 samples to frameSize,
    // FFT → power spectrum → IFFT → r[tau].  Turns O(tauMax×W) into O(N log N).
    const int W = frameSize / 2;          // samples used in YIN difference function
    const int acFftSz = frameSize;        // W*2 = 2048, already a power of 2
    std::vector<std::complex<float>> acBuf(acFftSz);

    for (size_t offset = 0; offset + frameSize <= numSamples; offset += hopSize) {
        const float* frame = samples + offset;

        // Compute r[tau] = autocorrelation of frame[0..W-1] via FFT
        for (int i = 0; i < W; ++i)       acBuf[i] = {frame[i], 0.f};
        for (int i = W; i < acFftSz; ++i) acBuf[i] = {0.f, 0.f};
        computeFFT(acBuf);
        for (int i = 0; i < acFftSz; ++i) {
            float re = acBuf[i].real(), im = acBuf[i].imag();
            acBuf[i] = {re * re + im * im, 0.f};
        }
        computeIFFT(acBuf);
        // acBuf[tau].real() ≈ r[tau] = sum_{j=0}^{W-1} x[j]*x[j+tau]

        const float r0 = acBuf[0].real();
        diff[0] = 0.f;
        for (int tau = 1; tau <= tauMax; ++tau)
            diff[tau] = 2.f * (r0 - acBuf[tau].real());

        cmndf[0] = 1.0f;
        float cumSum = 0.0f;
        for (int tau = 1; tau <= tauMax; ++tau) {
            cumSum += diff[tau];
            cmndf[tau] = (cumSum > 0.0f) ? diff[tau] * tau / cumSum : 1.0f;
        }

        // Find first minimum below threshold
        float bestF0 = -1.0f;
        for (int tau = tauMin; tau < tauMax; ++tau) {
            if (cmndf[tau] < threshold) {
                // Find local minimum
                while (tau + 1 < tauMax && cmndf[tau + 1] < cmndf[tau])
                    ++tau;
                // Parabolic interpolation
                float tauRefined = (float)tau;
                if (tau > 0 && tau < tauMax) {
                    float a = cmndf[tau - 1], b = cmndf[tau], c = cmndf[tau + 1];
                    float denom = 2.0f * (2.0f * b - a - c);
                    if (std::fabs(denom) > 1e-10f)
                        tauRefined = tau + (a - c) / denom;
                }
                if (tauRefined > 0.5f)
                    bestF0 = (float)sampleRate / tauRefined;
                break;
            }
        }

        if (bestF0 > 0.0f)
            f0Values.push_back(bestF0);
        else
            f0Values.push_back(-1.0f);  // unvoiced marker
    }

    // Separate voiced frames
    std::vector<float> voiced;
    for (float f : f0Values)
        if (f > 0.0f) voiced.push_back(f);

    float voicedRatio = f0Values.empty() ? 0.0f : (float)voiced.size() / f0Values.size();
    pitchOut[2] = voicedRatio;

    if (voiced.empty()) return;

    // Median F0
    std::sort(voiced.begin(), voiced.end());
    pitchOut[0] = voiced[voiced.size() / 2];

    // F0 standard deviation
    if (voiced.size() > 1) {
        float mean = 0.0f;
        for (float f : voiced) mean += f;
        mean /= (float)voiced.size();
        float var = 0.0f;
        for (float f : voiced) var += (f - mean) * (f - mean);
        pitchOut[1] = std::sqrt(var / (float)(voiced.size() - 1));
    }
}

// Energy Envelope + Attack Time + ZCR

static void computeEnergyEnvelope(const float* samples, size_t numSamples,
                                              int /*sampleRate*/, float* energyOut) {
    // energyOut[0..7] = 8-segment RMS (dBFS), energyOut[8] = attack time, energyOut[9] = ZCR
    std::fill(energyOut, energyOut + 10, 0.0f);
    if (numSamples == 0) return;

    const int numSegments = 8;
    size_t segLen = numSamples / numSegments;
    if (segLen == 0) segLen = 1;

    float peakRms = -100.0f;
    int peakSegIdx = 0;
    int firstActiveIdx = -1;

    for (int seg = 0; seg < numSegments; ++seg) {
        size_t start = seg * segLen;
        size_t end = (seg == numSegments - 1) ? numSamples : start + segLen;
        if (start >= numSamples) {
            energyOut[seg] = -96.0f;
            continue;
        }

        double sumSq = 0.0;
        for (size_t i = start; i < end; ++i)
            sumSq += (double)samples[i] * samples[i];

        float rms = (end > start) ? std::sqrt((float)(sumSq / (end - start))) : 0.0f;
        float rmsDb = (rms > 1e-10f) ? 20.0f * std::log10(rms) : -96.0f;
        energyOut[seg] = rmsDb;

        if (rmsDb > peakRms) {
            peakRms = rmsDb;
            peakSegIdx = seg;
        }
        if (firstActiveIdx < 0 && rmsDb > -40.0f)
            firstActiveIdx = seg;
    }

    // Attack time: ratio from first active segment to peak segment
    if (firstActiveIdx >= 0 && numSegments > 0)
        energyOut[8] = (float)(peakSegIdx - firstActiveIdx) / numSegments;
    else
        energyOut[8] = 0.0f;

    // Zero-crossing rate
    size_t zeroCrossings = 0;
    for (size_t i = 1; i < numSamples; ++i) {
        if ((samples[i] >= 0.0f) != (samples[i - 1] >= 0.0f))
            zeroCrossings++;
    }
    energyOut[9] = (float)zeroCrossings / (float)numSamples;
}

// Fill Global Features

void FeatureExtractor::fillGlobalFeatures(AudioFeatureVector& vec,
                                           float integratedLufs, float trueRmsDbfs,
                                           float durationSec, float peakDb) {
    vec.features[AudioFeatureVector::GLOBAL_OFFSET + 0] = integratedLufs;
    vec.features[AudioFeatureVector::GLOBAL_OFFSET + 1] = trueRmsDbfs;
    vec.features[AudioFeatureVector::GLOBAL_OFFSET + 2] = (durationSec > 0.001f) ? std::log2(durationSec) : -10.0f;
    vec.features[AudioFeatureVector::GLOBAL_OFFSET + 3] = peakDb - trueRmsDbfs;  // crest factor
}

// Main Extract (from PCM)

AudioFeatureVector FeatureExtractor::extract(const float* samples, size_t numSamples, int sampleRate) {
    AudioFeatureVector vec;

    // MFCC + Delta MFCC → [0..25]
    computeMFCC(samples, numSamples, sampleRate,
                vec.features + AudioFeatureVector::MFCC_OFFSET,
                vec.features + AudioFeatureVector::MFCC_D_OFFSET);

    // Spectral features → [26..30]
    computeSpectralFeatures(samples, numSamples, sampleRate,
                            vec.features + AudioFeatureVector::SPECTRAL_OFFSET);

    // Pitch → [31..33]
    computePitch(samples, numSamples, sampleRate,
                 vec.features + AudioFeatureVector::PITCH_OFFSET);

    // Energy envelope → [34..43]
    computeEnergyEnvelope(samples, numSamples, sampleRate,
                          vec.features + AudioFeatureVector::ENERGY_OFFSET);

    // Global features [44..47] left at 0 — caller fills from DB
    // Sanitize NaN/Inf
    for (int i = 0; i < AudioFeatureVector::DIMS; ++i) {
        if (std::isnan(vec.features[i]) || std::isinf(vec.features[i]))
            vec.features[i] = 0.0f;
    }

    return vec;
}

// Extract from File (decode + downsample to 22050 Hz mono)

AudioFeatureVector FeatureExtractor::extractFromFile(const std::string& filePath) {
    return extractFromFileChecked(filePath).features;
}

FeatureExtractionResult FeatureExtractor::extractFromFileChecked(
    const std::string& filePath,
    const FeatureExtractionCancelCheck& cancelCheck) {
    FeatureExtractionResult result;

    if (cancelCheck && cancelCheck()) {
        result.error = FeatureExtractionError::Cancelled;
        return result;
    }

    if (isKnownUnsupportedFeatureExtension(filePath)) {
        result.error = FeatureExtractionError::UnsupportedFormat;
        return result;
    }

    Platform::AudioFile::MonoFloatReader reader;
    if (reader.Open(filePath,
                    Platform::AudioFile::LegacyWindowsPathPolicy::FeatureLegacyUtf8VerbatimPrefix,
                    Platform::AudioFile::MonoSampleRate::Hz22050) != Platform::AudioFile::Status::Ok) {
        result.error = FeatureExtractionError::DecodeFailed;
        return result;
    }

    // Read all PCM data (mono, 22050 Hz)
    constexpr Platform::AudioFile::FrameCount kChunkFrames = 8192;
    std::vector<float> allSamples;
    allSamples.reserve(kTargetSampleRate * 30);  // ~30s typical

    std::vector<float> buf(kChunkFrames);
    while (true) {
        if (cancelCheck && cancelCheck()) {
            result.error = FeatureExtractionError::Cancelled;
            return result;
        }

        const auto read = reader.Read(buf.data(), kChunkFrames);
        const auto framesRead = read.framesRead;
        if (framesRead > 0)
            allSamples.insert(allSamples.end(), buf.begin(), buf.begin() + framesRead);

        if (cancelCheck && cancelCheck()) {
            result.error = FeatureExtractionError::Cancelled;
            return result;
        }

        if (read.status == Platform::AudioFile::Status::Cancelled) {
            result.error = FeatureExtractionError::Cancelled;
            return result;
        }
        if (read.status != Platform::AudioFile::Status::Ok &&
            read.status != Platform::AudioFile::Status::EndOfFile) {
            result.error = FeatureExtractionError::DecodeFailed;
            return result;
        }
        if (framesRead == 0) break;
        // Cap at 30s: MFCC features from 30s are representative enough for similarity search.
        // Decoding beyond 30s gives diminishing returns and massively slows batch extraction.
        if (allSamples.size() > (size_t)kTargetSampleRate * 30) break;
    }

    if (allSamples.empty()) {
        result.error = FeatureExtractionError::NoSamples;
        return result;
    }

    result.features = extract(allSamples.data(), allSamples.size(), kTargetSampleRate);
    result.ok = true;
    result.error = FeatureExtractionError::None;
    return result;
}
