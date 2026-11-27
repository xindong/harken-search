#include "WaveformExtractor.h"
#include "NativeFloatReader.h"

#include "DiagnosticsLogger.h"
#include "MediaFoundationAudioDecoder.h"
#include "WaveformBucketMath.h"
#include "platform/filesystem.h"
#define XXH_INLINE_ALL
#include "../thirdparty/xxhash.h"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstdint>
#include <cwctype>
#include <cstring>
#include <sstream>

namespace {

static bool IsCancelled(const std::atomic<bool>* cancelFlag) {
    return cancelFlag && cancelFlag->load(std::memory_order_acquire);
}

static bool HasAnyPositivePeak(const WaveformExtractResult& result) {
    for (float peak : result.peaks) {
        if (peak > 0.0f) return true;
    }
    return false;
}

static constexpr double kMiniSequentialMinDurationSec = 1.0;

// 用顺序扫描的最低时长阈值。
// 实测：HDD 上 128 次随机 seek 即使对短 WAV 也要数百 ms，远比单次顺序读慢。
// 因此对几乎所有文件都用 sequential，仅极短文件（< 1s）保持 seek-based 以避免开销。
static bool HasWavExtension(const std::string& filePath) {
    const size_t dot = filePath.find_last_of('.');
    if (dot == std::string::npos) return false;
    std::string ext = filePath.substr(dot);
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
        return (char)std::tolower(c);
    });
    return ext == ".wav" || ext == ".wave" || ext == ".bwf";
}

static bool OpenBinaryInput(const std::string& filePath,
                            Platform::FileSystem::StreamReader& in) {
    return in.Open(filePath, true);
}

static uint16_t ReadLe16(const uint8_t* p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t ReadLe32(const uint8_t* p) {
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static constexpr uint8_t kWpk2Header[8] = {0x00, 'W', 'P', 'K', '2', 1, 0, 1};
static constexpr size_t kWpk2HeaderSize = sizeof(kWpk2Header);
static constexpr int kWpk2Scale = 32767;

static void WriteLe16(std::vector<uint8_t>& out, int16_t value) {
    const uint16_t raw = static_cast<uint16_t>(value);
    out.push_back(static_cast<uint8_t>(raw & 0xFF));
    out.push_back(static_cast<uint8_t>((raw >> 8) & 0xFF));
}

static int16_t QuantizePeakInt16(float value) {
    if (!std::isfinite(value) || value == 0.0f) {
        return 0;
    }

    const float clamped = std::max(-1.0f, std::min(1.0f, value));
    int rounded = static_cast<int>(std::lround(clamped * (float)kWpk2Scale));
    if (rounded == 0) {
        rounded = clamped > 0.0f ? 1 : -1;
    }
    rounded = std::max(-kWpk2Scale, std::min(kWpk2Scale, rounded));
    return static_cast<int16_t>(rounded);
}

static float DequantizePeakInt16(int16_t value) {
    return static_cast<float>(value) / static_cast<float>(kWpk2Scale);
}

struct PcmWavInfo {
    uint16_t audioFormat = 0;
    uint16_t channels = 0;
    uint32_t sampleRate = 0;
    uint16_t blockAlign = 0;
    uint16_t bitsPerSample = 0;
    bool isFloat = false;
    uint64_t dataOffset = 0;
    uint64_t dataSize = 0;
    uint64_t totalFrames = 0;
};

static bool ReadExact(Platform::FileSystem::StreamReader& in, void* dst, size_t bytes) {
    return in.Read(dst, bytes) == bytes;
}

static bool ParsePcmWavHeader(Platform::FileSystem::StreamReader& in, PcmWavInfo& info) {
    info = {};
    uint8_t riff[12] = {};
    if (!ReadExact(in, riff, 12)) return false;
    if (std::memcmp(riff, "RIFF", 4) != 0 || std::memcmp(riff + 8, "WAVE", 4) != 0) {
        return false;
    }

    bool haveFmt = false;
    bool haveData = false;

    while (in.Good() && (!haveFmt || !haveData)) {
        uint8_t chunkHeader[8] = {};
        if (!ReadExact(in, chunkHeader, 8)) break;

        const uint32_t chunkSize = ReadLe32(chunkHeader + 4);
        const int64_t chunkDataPos = in.Tell();
        if (chunkDataPos < 0) return false;

        if (std::memcmp(chunkHeader, "fmt ", 4) == 0) {
            if (chunkSize < 16) return false;
            std::vector<uint8_t> fmt(chunkSize);
            if (!ReadExact(in, fmt.data(), fmt.size())) return false;

            info.audioFormat = ReadLe16(fmt.data() + 0);
            info.channels = ReadLe16(fmt.data() + 2);
            info.sampleRate = ReadLe32(fmt.data() + 4);
            info.blockAlign = ReadLe16(fmt.data() + 12);
            info.bitsPerSample = ReadLe16(fmt.data() + 14);

            if (info.audioFormat == 1) {
                info.isFloat = false;
            } else if (info.audioFormat == 3) {
                info.isFloat = true;
            } else if (info.audioFormat == 0xFFFE && fmt.size() >= 40) {
                const uint16_t subFormatTag = ReadLe16(fmt.data() + 24);
                if (subFormatTag == 1) {
                    info.isFloat = false;
                } else if (subFormatTag == 3) {
                    info.isFloat = true;
                } else {
                    return false;
                }
            } else {
                return false;
            }

            haveFmt = true;
        } else if (std::memcmp(chunkHeader, "data", 4) == 0) {
            info.dataOffset = (uint64_t)chunkDataPos;
            info.dataSize = chunkSize;
            haveData = true;
            in.Seek(chunkSize + (chunkSize & 1u), Platform::FileSystem::StreamSeekOrigin::Current);
        } else {
            in.Seek(chunkSize + (chunkSize & 1u), Platform::FileSystem::StreamSeekOrigin::Current);
        }
    }

    if (!haveFmt || !haveData) return false;
    if (info.channels == 0 || info.channels > 64) return false;
    if (info.sampleRate == 0) return false;
    if (info.bitsPerSample != 8 && info.bitsPerSample != 16 &&
        info.bitsPerSample != 24 && info.bitsPerSample != 32 &&
        info.bitsPerSample != 64) {
        return false;
    }
    if (!info.isFloat && info.bitsPerSample == 64) return false;
    if (info.isFloat && info.bitsPerSample != 32 && info.bitsPerSample != 64) return false;

    const uint16_t bytesPerSample = (uint16_t)((info.bitsPerSample + 7) / 8);
    const uint16_t minBlockAlign = (uint16_t)(info.channels * bytesPerSample);
    if (info.blockAlign < minBlockAlign || info.blockAlign == 0) return false;

    info.totalFrames = info.dataSize / info.blockAlign;
    return info.totalFrames > 0;
}

static float DecodeWavSample(const uint8_t* p, uint16_t bitsPerSample, bool isFloat) {
    if (isFloat) {
        if (bitsPerSample == 32) {
            float v = 0.0f;
            std::memcpy(&v, p, sizeof(float));
            return std::isfinite(v) ? std::clamp(v, -1.0f, 1.0f) : 0.0f;
        }
        if (bitsPerSample == 64) {
            double v = 0.0;
            std::memcpy(&v, p, sizeof(double));
            return std::isfinite(v) ? std::clamp((float)v, -1.0f, 1.0f) : 0.0f;
        }
        return 0.0f;
    }

    switch (bitsPerSample) {
    case 8:
        return ((float)p[0] - 128.0f) / 128.0f;
    case 16: {
        const int16_t v = (int16_t)ReadLe16(p);
        return (float)v / 32768.0f;
    }
    case 24: {
        int32_t v = (int32_t)p[0] | ((int32_t)p[1] << 8) | ((int32_t)p[2] << 16);
        if (v & 0x00800000) v |= 0xFF000000;
        return (float)v / 8388608.0f;
    }
    case 32: {
        const int32_t v = (int32_t)ReadLe32(p);
        return (float)((double)v / 2147483648.0);
    }
    default:
        return 0.0f;
    }
}

static bool ExtractMiniSparseWav(const std::string& filePath,
                                 int targetPeaks,
                                 WaveformExtractResult& out,
                                 bool perChannel,
                                 const std::atomic<bool>* cancelFlag) {
    out = {};
    Platform::FileSystem::StreamReader in;
    if (!OpenBinaryInput(filePath, in)) {
        out.errorCode = "wav_open_failed";
        return false;
    }

    PcmWavInfo info;
    if (!ParsePcmWavHeader(in, info)) {
        out.errorCode = "wav_parse_failed";
        return false;
    }

    // targetPeaks <= 0 is the adaptive sentinel: derive density from the source
    // frame count (SM-style 1 peak / 1024 frames). Legacy callers pass a fixed
    // count and are unaffected (128 < 1024).
    if (targetPeaks <= 0) targetPeaks = ComputeAdaptiveMiniPeakCount(info.totalFrames);
    targetPeaks = std::clamp(targetPeaks, 128, 1024);
    const bool wantStereo = perChannel && info.channels >= 2;
    const int outChannels = wantStereo ? 2 : 1;
    const uint16_t bytesPerSample = (uint16_t)((info.bitsPerSample + 7) / 8);
    const uint64_t segmentFrames = (std::max<uint64_t>)(1, info.totalFrames / (uint64_t)targetPeaks);
    const uint64_t windowFrames = std::clamp<uint64_t>(segmentFrames / 32, 96, 1024);

    out.channels = outChannels;
    out.sampleRate = info.sampleRate;
    out.samplesPerPeak = (int)segmentFrames;
    out.peaks.reserve((size_t)targetPeaks * (size_t)outChannels);
    out.minPeaks.reserve((size_t)targetPeaks * (size_t)outChannels);
    out.rms.reserve((size_t)targetPeaks * (size_t)outChannels);

    std::vector<uint8_t> bytes((size_t)windowFrames * (size_t)info.blockAlign);
    const int sampleBuckets = targetPeaks;

    for (int bucket = 0; bucket < sampleBuckets; ++bucket) {
        if (IsCancelled(cancelFlag)) {
            out.errorCode = "cancelled";
            return false;
        }
        const int outBegin = bucket * targetPeaks / sampleBuckets;
        const int outEnd = (bucket + 1) * targetPeaks / sampleBuckets;
        const uint64_t segStart = (uint64_t)outBegin * segmentFrames;
        const uint64_t segEnd = (bucket == sampleBuckets - 1)
            ? info.totalFrames
            : (std::min)(info.totalFrames, (uint64_t)outEnd * segmentFrames);
        const uint64_t segFrames = (segEnd > segStart) ? (segEnd - segStart) : 0;

        float peak = 0.0f, peakL = 0.0f, peakR = 0.0f;
        float minPeak = 0.0f, minPeakL = 0.0f, minPeakR = 0.0f;
        double sumSq = 0.0, sumSqL = 0.0, sumSqR = 0.0;
        uint64_t sampleCount = 0, sampleCountLr = 0;

        if (segFrames > 0) {
            uint64_t readFrames = (std::min)(windowFrames, segFrames);
            uint64_t anchorFrame = segStart + segFrames / 2;
            uint64_t readStart = (anchorFrame > readFrames / 2) ? (anchorFrame - readFrames / 2) : segStart;
            if (readStart < segStart) readStart = segStart;
            if (readStart + readFrames > segEnd) {
                readStart = (segEnd > readFrames) ? (segEnd - readFrames) : segStart;
            }

            const uint64_t byteOffset = info.dataOffset + readStart * (uint64_t)info.blockAlign;
            in.Clear();
            in.Seek((int64_t)byteOffset, Platform::FileSystem::StreamSeekOrigin::Begin);
            const size_t bytesToRead = (size_t)readFrames * (size_t)info.blockAlign;
            if (bytes.size() < bytesToRead) bytes.resize(bytesToRead);
            const size_t got = in.Read(bytes.data(), bytesToRead);
            const uint64_t framesRead = got > 0 ? (uint64_t)got / (uint64_t)info.blockAlign : 0;

            for (uint64_t f = 0; f < framesRead; ++f) {
                const uint8_t* frame = bytes.data() + (size_t)f * (size_t)info.blockAlign;
                if (wantStereo) {
                    const float l = DecodeWavSample(frame, info.bitsPerSample, info.isFloat);
                    const float r = DecodeWavSample(frame + bytesPerSample, info.bitsPerSample, info.isFloat);
                    peakL = (std::max)(peakL, std::abs(l));
                    peakR = (std::max)(peakR, std::abs(r));
                    minPeakL = (std::min)(minPeakL, l);
                    minPeakR = (std::min)(minPeakR, r);
                    sumSqL += (double)l * (double)l;
                    sumSqR += (double)r * (double)r;
                    ++sampleCountLr;
                } else {
                    for (uint16_t ch = 0; ch < info.channels; ++ch) {
                        const uint8_t* sample = frame + (size_t)ch * (size_t)bytesPerSample;
                        const float v = DecodeWavSample(sample, info.bitsPerSample, info.isFloat);
                        peak = (std::max)(peak, std::abs(v));
                        minPeak = (std::min)(minPeak, v);
                        sumSq += (double)v * (double)v;
                        ++sampleCount;
                    }
                }
            }
        }

        for (int seg = outBegin; seg < outEnd; ++seg) {
            if (wantStereo) {
                const double denom = sampleCountLr > 0 ? (double)sampleCountLr : 1.0;
                out.peaks.push_back(peakL);
                out.peaks.push_back(peakR);
                out.minPeaks.push_back(minPeakL);
                out.minPeaks.push_back(minPeakR);
                out.rms.push_back((float)std::sqrt(sumSqL / denom));
                out.rms.push_back((float)std::sqrt(sumSqR / denom));
            } else {
                const double denom = sampleCount > 0 ? (double)sampleCount : 1.0;
                out.peaks.push_back(peak);
                out.minPeaks.push_back(minPeak);
                out.rms.push_back((float)std::sqrt(sumSq / denom));
            }
        }
    }

    out.hasTrueRms = false;
    out.trueRmsDbfs = -200.0f;
    out.ok = !out.peaks.empty();
    if (!out.ok) out.errorCode = "wav_sparse_empty";
    return out.ok;
}

static bool ExtractMiniSequentialUnknownLength(const std::string& filePath,
                                               int targetPeaks,
                                               WaveformExtractResult& out,
                                               bool perChannel,
                                               const std::atomic<bool>* cancelFlag);

static bool ExtractMiniSeekBased(const std::string& filePath,
                                 int targetPeaks,
                                 WaveformExtractResult& out,
                                 bool perChannel = false,
                                 const std::atomic<bool>* cancelFlag = nullptr) {
    out = {};
    if (filePath.empty()) {
        out.errorCode = "empty_path";
        return false;
    }

// Keep the adaptive sentinel (<=0) intact until totalFrames is known below
    if (targetPeaks > 0) targetPeaks = std::clamp(targetPeaks, 128, 1024);

    NativeFloatReader reader;
    if (!reader.open(filePath)) {
        out.errorCode = "decoder_init_failed";
        return false;
    }

    const std::uint32_t actualChannels = reader.metadata().channels > 0 ? reader.metadata().channels : 1;
    const std::uint32_t sampleRate = reader.metadata().sampleRate > 0 ? reader.metadata().sampleRate : 48000;

    uint64_t totalFrames = reader.metadata().hasReportedFrames
        ? reader.metadata().reportedFrames : 0;
    if (HasWavExtension(filePath)) {
        Platform::FileSystem::StreamReader wavIn;
        if (OpenBinaryInput(filePath, wavIn)) {
            PcmWavInfo wavInfo;
            if (ParsePcmWavHeader(wavIn, wavInfo) && wavInfo.totalFrames > 0 && wavInfo.totalFrames < totalFrames) {
                totalFrames = wavInfo.totalFrames;
            }
        }
    }
    if (totalFrames == 0) {
        reader.close();
        return ExtractMiniSequentialUnknownLength(filePath, targetPeaks, out, perChannel, cancelFlag);
    }

    if (targetPeaks <= 0)
        targetPeaks = std::clamp(ComputeAdaptiveMiniPeakCount(totalFrames), 128, 1024);

    const int blockSize = 512;
    std::uint64_t segmentSize = totalFrames / (std::uint64_t)targetPeaks;
    if (segmentSize < 1) segmentSize = 1;

    const bool wantStereo = perChannel && (actualChannels >= 2);
    const int outChannels = wantStereo ? 2 : 1;

    out.channels = outChannels;
    out.sampleRate = sampleRate;
    out.samplesPerPeak = (int)segmentSize;
    out.peaks.reserve((size_t)targetPeaks * (size_t)outChannels);
    out.minPeaks.reserve((size_t)targetPeaks * (size_t)outChannels);
    out.rms.reserve((size_t)targetPeaks * (size_t)outChannels);

    std::vector<float> buffer((size_t)blockSize * (size_t)actualChannels);

    double globalSumSq = 0.0;
    uint64_t globalSampleCount = 0;

    for (int seg = 0; seg < targetPeaks; ++seg) {
        if (cancelFlag && cancelFlag->load(std::memory_order_acquire)) {
            reader.close();
            out.errorCode = "cancelled";
            return false;
        }
        uint64_t seekCenter = (uint64_t)seg * segmentSize + segmentSize / 2;
        if (seekCenter + blockSize > totalFrames) {
            seekCenter = (totalFrames > (std::uint64_t)blockSize) ? (totalFrames - blockSize) : 0;
        }

        const auto seekResult = reader.seek(seekCenter, cancelFlag);
        if (seekResult.status == NativeFloatReader::ReadStatus::Cancelled) {
            reader.close();
            out.errorCode = "cancelled";
            return false;
        }
        if (seekResult.status == NativeFloatReader::ReadStatus::Error) {
            for (int c = 0; c < outChannels; ++c) {
                out.peaks.push_back(0.0f);
                out.minPeaks.push_back(0.0f);
                out.rms.push_back(0.0f);
            }
            continue;
        }

        const auto readResult = reader.read(buffer.data(), blockSize, cancelFlag);
        if (readResult.status == NativeFloatReader::ReadStatus::Cancelled) {
            reader.close();
            out.errorCode = "cancelled";
            return false;
        }
        const uint64_t framesRead = readResult.framesRead;
        if (framesRead == 0 || readResult.status == NativeFloatReader::ReadStatus::Error) {
            for (int c = 0; c < outChannels; ++c) {
                out.peaks.push_back(0.0f);
                out.minPeaks.push_back(0.0f);
                out.rms.push_back(0.0f);
            }
            continue;
        }

        if (wantStereo) {
            float peakL = 0.0f, peakR = 0.0f;
            float minPeakL = 0.0f, minPeakR = 0.0f;
            double sumSqL = 0.0, sumSqR = 0.0;
            for (std::uint64_t f = 0; f < framesRead; ++f) {
                const float sL = buffer[(size_t)(f * actualChannels + 0)];
                const float sR = buffer[(size_t)(f * actualChannels + 1)];
                float aL = std::abs(sL), aR = std::abs(sR);
                if (aL > peakL) peakL = aL;
                if (aR > peakR) peakR = aR;
                minPeakL = std::min(minPeakL, sL);
                minPeakR = std::min(minPeakR, sR);
                sumSqL += (double)sL * (double)sL;
                sumSqR += (double)sR * (double)sR;
            }
            globalSumSq += sumSqL + sumSqR;
            globalSampleCount += (uint64_t)framesRead * 2;

            out.peaks.push_back(peakL);
            out.peaks.push_back(peakR);
            out.minPeaks.push_back(minPeakL);
            out.minPeaks.push_back(minPeakR);
            out.rms.push_back((float)std::sqrt(sumSqL / (double)framesRead));
            out.rms.push_back((float)std::sqrt(sumSqR / (double)framesRead));
        } else {
            float peak = 0.0f;
            float minPeak = 0.0f;
            double sumSq = 0.0;
            const std::uint64_t totalSamples = framesRead * actualChannels;
            for (std::uint64_t i = 0; i < totalSamples; ++i) {
                const float s = buffer[(size_t)i];
                const float a = std::abs(s);
                if (a > peak) peak = a;
                minPeak = std::min(minPeak, s);
                sumSq += (double)s * (double)s;
            }
            globalSumSq += sumSq;
            globalSampleCount += (uint64_t)totalSamples;

            out.peaks.push_back(peak);
            out.minPeaks.push_back(minPeak);
            out.rms.push_back((float)std::sqrt(sumSq / (double)totalSamples));
        }
    }

    reader.close();

    if (globalSampleCount > 0) {
        double rmsLinear = std::sqrt(globalSumSq / (double)globalSampleCount);
        out.trueRmsDbfs = (rmsLinear > 1e-10)
            ? std::clamp((float)(20.0 * std::log10(rmsLinear)), -100.0f, 0.0f)
            : -100.0f;
        out.hasTrueRms = true;
    }

    out.ok = !out.peaks.empty();
    if (!out.ok) out.errorCode = "mini_empty";
    return out.ok;
}

// 单次顺序扫描生成 mini 波形，避免 seek-based 在压缩/长文件上反复 seek 的代价。
// 解码器从头到尾跑一次,按 samplesPerPeak 分桶累计 peak/RMS。
static bool ExtractMiniSequentialUnknownLength(const std::string& filePath,
                                               int targetPeaks,
                                               WaveformExtractResult& out,
                                               bool perChannel,
                                               const std::atomic<bool>* cancelFlag) {
    out = {};
    NativeFloatReader reader;
    if (!reader.open(filePath)) {
        out.errorCode = "decoder_init_failed";
        return false;
    }

    const std::uint32_t actualChannels = reader.metadata().channels > 0 ? reader.metadata().channels : 1;
    const std::uint32_t sampleRate = reader.metadata().sampleRate > 0 ? reader.metadata().sampleRate : 48000;
    constexpr uint64_t kChunkFrames = 4096;
    std::vector<float> buffer((size_t)kChunkFrames * (size_t)actualChannels);

    uint64_t decodedFrames = 0;
    while (true) {
        const auto result = reader.read(buffer.data(), kChunkFrames, cancelFlag);
        if (result.status == NativeFloatReader::ReadStatus::Cancelled) {
            out.errorCode = "cancelled";
            return false;
        }
        decodedFrames += result.framesRead;
        if (result.status == NativeFloatReader::ReadStatus::Error ||
            result.status == NativeFloatReader::ReadStatus::End || result.framesRead == 0)
            break;
    }
    if (decodedFrames == 0) {
        out.errorCode = "decode_no_frames";
        return false;
    }

    reader.close();
    if (!reader.open(filePath)) {
        out.errorCode = "decoder_reinit_failed";
        return false;
    }

    // A full decode supplies decodedFrames when container metadata omits length.
    if (targetPeaks <= 0)
        targetPeaks = std::clamp(ComputeAdaptiveMiniPeakCount(decodedFrames), 128, 1024);

    const bool wantStereo = perChannel && (actualChannels >= 2);
    const int outChannels = wantStereo ? 2 : 1;
    const int effectivePeaks = (int)std::min<std::uint64_t>((std::uint64_t)targetPeaks, decodedFrames);
    const int samplesPerPeak = (int)std::max<std::uint64_t>(1, decodedFrames / (std::uint64_t)effectivePeaks);

    out.channels = outChannels;
    out.sampleRate = sampleRate;
    out.samplesPerPeak = samplesPerPeak;
    out.decodedFrames = decodedFrames;
    out.peaks.assign((size_t)effectivePeaks * (size_t)outChannels, 0.0f);
    out.minPeaks.assign((size_t)effectivePeaks * (size_t)outChannels, 0.0f);
    out.rms.assign((size_t)effectivePeaks * (size_t)outChannels, 0.0f);

    double globalSumSq = 0.0;
    uint64_t globalSampleCount = 0;
    std::uint64_t globalFrame = 0;
    while (true) {
        const auto result = reader.read(buffer.data(), kChunkFrames, cancelFlag);
        if (result.status == NativeFloatReader::ReadStatus::Cancelled) {
            out.errorCode = "cancelled";
            return false;
        }
        const uint64_t framesRead = result.framesRead;
        if (framesRead == 0) break;

        for (std::uint64_t f = 0; f < framesRead; ++f, ++globalFrame) {
            int bucket = (int)(((uint64_t)globalFrame * (uint64_t)effectivePeaks) / (uint64_t)decodedFrames);
            if (bucket >= effectivePeaks) bucket = effectivePeaks - 1;
            const size_t dst = (size_t)bucket * (size_t)outChannels;
            if (wantStereo) {
                const float sL = buffer[(size_t)(f * actualChannels + 0)];
                const float sR = buffer[(size_t)(f * actualChannels + 1)];
                out.peaks[dst] = std::max(out.peaks[dst], std::abs(sL));
                out.peaks[dst + 1] = std::max(out.peaks[dst + 1], std::abs(sR));
                out.minPeaks[dst] = std::min(out.minPeaks[dst], sL);
                out.minPeaks[dst + 1] = std::min(out.minPeaks[dst + 1], sR);
                out.rms[dst] += sL * sL;
                out.rms[dst + 1] += sR * sR;
                globalSumSq += (double)sL * (double)sL + (double)sR * (double)sR;
                globalSampleCount += 2;
            } else {
                float peak = 0.0f;
                double sumSq = 0.0;
                for (std::uint32_t ch = 0; ch < actualChannels; ++ch) {
                    const float s = buffer[(size_t)(f * actualChannels + ch)];
                    peak = std::max(peak, std::abs(s));
                    out.minPeaks[dst] = std::min(out.minPeaks[dst], s);
                    sumSq += (double)s * (double)s;
                }
                out.peaks[dst] = std::max(out.peaks[dst], peak);
                out.rms[dst] += (float)sumSq;
                globalSumSq += sumSq;
                globalSampleCount += actualChannels;
            }
        }
    }
    reader.close();

    for (int bucket = 0; bucket < effectivePeaks; ++bucket) {
        const uint64_t begin = ((uint64_t)bucket * (uint64_t)decodedFrames) / (uint64_t)effectivePeaks;
        uint64_t end = (((uint64_t)bucket + 1u) * (uint64_t)decodedFrames) / (uint64_t)effectivePeaks;
        if (end <= begin) end = begin + 1;
        const float denom = (float)std::max<uint64_t>(1, end - begin);
        const size_t dst = (size_t)bucket * (size_t)outChannels;
        for (int ch = 0; ch < outChannels; ++ch) {
            out.rms[dst + (size_t)ch] = std::sqrt(out.rms[dst + (size_t)ch] / denom);
        }
    }

    if (globalSampleCount > 0) {
        const double rmsLinear = std::sqrt(globalSumSq / (double)globalSampleCount);
        out.trueRmsDbfs = (rmsLinear > 1e-10)
            ? std::clamp((float)(20.0 * std::log10(rmsLinear)), -100.0f, 0.0f)
            : -100.0f;
        out.hasTrueRms = true;
    }
    out.ok = !out.peaks.empty();
    if (!out.ok) out.errorCode = "mini_empty";
    return out.ok;
}

static bool ExtractMiniSequential(const std::string& filePath,
                                  int targetPeaks,
                                  WaveformExtractResult& out,
                                  bool perChannel = false,
                                  const std::atomic<bool>* cancelFlag = nullptr) {
    out = {};
    if (filePath.empty()) {
        out.errorCode = "empty_path";
        return false;
    }

    if (targetPeaks > 0) targetPeaks = std::clamp(targetPeaks, 128, 1024);

    NativeFloatReader reader;
    if (!reader.open(filePath)) {
        out.errorCode = "decoder_init_failed";
        return false;
    }

    const std::uint32_t actualChannels = reader.metadata().channels > 0 ? reader.metadata().channels : 1;
    const std::uint32_t sampleRate = reader.metadata().sampleRate > 0 ? reader.metadata().sampleRate : 48000;

    uint64_t totalFrames = reader.metadata().hasReportedFrames
        ? reader.metadata().reportedFrames : 0;
    if (HasWavExtension(filePath)) {
        Platform::FileSystem::StreamReader wavIn;
        if (OpenBinaryInput(filePath, wavIn)) {
            PcmWavInfo wavInfo;
            if (ParsePcmWavHeader(wavIn, wavInfo) && wavInfo.totalFrames > 0 && wavInfo.totalFrames < totalFrames) {
                totalFrames = wavInfo.totalFrames;
            }
        }
    }
    if (totalFrames == 0) {
        reader.close();
        return ExtractMiniSequentialUnknownLength(filePath, targetPeaks, out, perChannel, cancelFlag);
    }

    if (targetPeaks <= 0)
        targetPeaks = std::clamp(ComputeAdaptiveMiniPeakCount(totalFrames), 128, 1024);

    std::uint64_t segmentSize = totalFrames / (std::uint64_t)targetPeaks;
    if (segmentSize < 1) segmentSize = 1;

    const bool wantStereo = perChannel && (actualChannels >= 2);
    const int outChannels = wantStereo ? 2 : 1;

    out.channels = outChannels;
    out.sampleRate = sampleRate;
    out.samplesPerPeak = (int)segmentSize;
    out.peaks.reserve((size_t)targetPeaks * (size_t)outChannels);
    out.minPeaks.reserve((size_t)targetPeaks * (size_t)outChannels);
    out.rms.reserve((size_t)targetPeaks * (size_t)outChannels);

    const uint64_t blockFrames = 4096;
    std::vector<float> buffer((size_t)blockFrames * (size_t)actualChannels);

    double globalSumSq = 0.0;
    uint64_t globalSampleCount = 0;

    // 当前桶的累加状态（mono 或 L/R 双桶）
    float bucketPeak = 0.0f, bucketPeakL = 0.0f, bucketPeakR = 0.0f;
    float bucketMinPeak = 0.0f, bucketMinPeakL = 0.0f, bucketMinPeakR = 0.0f;
    double bucketSumSq = 0.0, bucketSumSqL = 0.0, bucketSumSqR = 0.0;
    std::uint64_t bucketFrames = 0;
    int peaksWritten = 0;

    auto flushBucket = [&]() {
        if (peaksWritten >= targetPeaks) return;
        if (wantStereo) {
            const double denom = bucketFrames > 0 ? (double)bucketFrames : 1.0;
            out.peaks.push_back(bucketPeakL);
            out.peaks.push_back(bucketPeakR);
            out.minPeaks.push_back(bucketMinPeakL);
            out.minPeaks.push_back(bucketMinPeakR);
            out.rms.push_back((float)std::sqrt(bucketSumSqL / denom));
            out.rms.push_back((float)std::sqrt(bucketSumSqR / denom));
            globalSumSq += bucketSumSqL + bucketSumSqR;
            globalSampleCount += (uint64_t)bucketFrames * 2;
        } else {
            const double denom = (bucketFrames * actualChannels) > 0
                ? (double)(bucketFrames * actualChannels) : 1.0;
            out.peaks.push_back(bucketPeak);
            out.minPeaks.push_back(bucketMinPeak);
            out.rms.push_back((float)std::sqrt(bucketSumSq / denom));
            globalSumSq += bucketSumSq;
            globalSampleCount += (uint64_t)bucketFrames * actualChannels;
        }
        ++peaksWritten;
        bucketPeak = bucketPeakL = bucketPeakR = 0.0f;
        bucketMinPeak = bucketMinPeakL = bucketMinPeakR = 0.0f;
        bucketSumSq = bucketSumSqL = bucketSumSqR = 0.0;
        bucketFrames = 0;
    };

    while (peaksWritten < targetPeaks) {
        const auto result = reader.read(buffer.data(), blockFrames, cancelFlag);
        if (result.status == NativeFloatReader::ReadStatus::Cancelled) {
            out.errorCode = "cancelled";
            return false;
        }
        const uint64_t framesRead = result.framesRead;
        if (framesRead == 0) break;

        std::uint64_t cursor = 0;
        while (cursor < framesRead && peaksWritten < targetPeaks) {
            const std::uint64_t framesNeeded = segmentSize - bucketFrames;
            const std::uint64_t framesAvail = framesRead - cursor;
            const std::uint64_t take = (framesNeeded < framesAvail) ? framesNeeded : framesAvail;

            if (wantStereo) {
                for (std::uint64_t f = 0; f < take; ++f) {
                    const float sL = buffer[(size_t)((cursor + f) * actualChannels + 0)];
                    const float sR = buffer[(size_t)((cursor + f) * actualChannels + 1)];
                    const float aL = std::abs(sL), aR = std::abs(sR);
                    if (aL > bucketPeakL) bucketPeakL = aL;
                    if (aR > bucketPeakR) bucketPeakR = aR;
                    bucketMinPeakL = std::min(bucketMinPeakL, sL);
                    bucketMinPeakR = std::min(bucketMinPeakR, sR);
                    bucketSumSqL += (double)sL * (double)sL;
                    bucketSumSqR += (double)sR * (double)sR;
                }
            } else {
                const std::uint64_t sampleStart = (cursor) * actualChannels;
                const std::uint64_t sampleEnd = (cursor + take) * actualChannels;
                for (std::uint64_t i = sampleStart; i < sampleEnd; ++i) {
                    const float s = buffer[(size_t)i];
                    const float a = std::abs(s);
                    if (a > bucketPeak) bucketPeak = a;
                    bucketMinPeak = std::min(bucketMinPeak, s);
                    bucketSumSq += (double)s * (double)s;
                }
            }

            bucketFrames += take;
            cursor += take;

            if (bucketFrames >= segmentSize) {
                flushBucket();
            }
        }

        if (result.status == NativeFloatReader::ReadStatus::Error) break;
    }

    // 若解码到尾仍有未冲洗的桶，补齐到 targetPeaks（保持形状一致以便缓存命中）
    if (bucketFrames > 0 && peaksWritten < targetPeaks) {
        flushBucket();
    }
    while (peaksWritten < targetPeaks) {
        for (int c = 0; c < outChannels; ++c) {
            out.peaks.push_back(0.0f);
            out.minPeaks.push_back(0.0f);
            out.rms.push_back(0.0f);
        }
        ++peaksWritten;
    }

    reader.close();
    if (globalSampleCount > 0) {
        const double rmsLinear = std::sqrt(globalSumSq / (double)globalSampleCount);
        out.trueRmsDbfs = (rmsLinear > 1e-10)
            ? std::clamp((float)(20.0 * std::log10(rmsLinear)), -100.0f, 0.0f)
            : -100.0f;
        out.hasTrueRms = true;
    }

    out.ok = !out.peaks.empty();
    if (!out.ok) out.errorCode = "mini_empty";
    return out.ok;
}

static bool ExtractDetailSequential(const std::string& filePath,
                                    int targetPeaks,
                                    bool withRms,
                                    WaveformExtractResult& out,
                                    const std::atomic<bool>* cancelFlag = nullptr) {
    out = {};
    if (filePath.empty()) {
        out.errorCode = "empty_path";
        return false;
    }

    targetPeaks = std::clamp(targetPeaks, 200, 50000);

    NativeFloatReader reader;
    if (!reader.open(filePath)) {
        out.errorCode = "decoder_init_failed";
        return false;
    }

    uint64_t totalFrames = reader.metadata().hasReportedFrames
        ? reader.metadata().reportedFrames : 0;
    if (totalFrames == 0) {
        reader.close();
        out.errorCode = "empty_audio";
        return false;
    }

    const std::uint32_t channels = reader.metadata().channels > 0 ? reader.metadata().channels : 1;
    const std::uint32_t sampleRate = reader.metadata().sampleRate > 0 ? reader.metadata().sampleRate : 48000;

    // Container metadata can underestimate VBR length. Stream until EOF so peaks
    // cover the whole file; samplesPerPeak remains the initial bucket estimate.
    int samplesPerPeak = (int)(totalFrames / (std::uint64_t)targetPeaks);
    if (samplesPerPeak < 1) samplesPerPeak = 1;

    out.channels = (int)channels;
    out.sampleRate = sampleRate;
    out.samplesPerPeak = samplesPerPeak;
    // 预分配避免流式 push_back 时反复 realloc（targetPeaks × channels）
    const size_t estimatedPeaks = (size_t)targetPeaks * (size_t)channels;
    out.peaks.clear();
    out.peaks.reserve(estimatedPeaks);
    out.minPeaks.clear();
    out.minPeaks.reserve(estimatedPeaks);
    if (withRms) { out.rms.clear(); out.rms.reserve(estimatedPeaks); }

    constexpr std::uint64_t kChunkFrames = 8192;
    std::vector<float> buffer((size_t)kChunkFrames * (size_t)channels);
    std::vector<float> peakByCh((size_t)channels, 0.0f);
    std::vector<float> minPeakByCh((size_t)channels, 0.0f);
    std::vector<double> sumSqByCh;
    if (withRms) {
        sumSqByCh.assign((size_t)channels, 0.0);
    }
    std::uint64_t blockFrames = 0;

    // 全文件级 sum-of-squares 累加器，用于计算真实 RMS dBFS
    // 公式：RMS = sqrt(totalSigmaX2 / (totalDecodedFrames * channels))
    const bool computeTrueRms = withRms;
    double totalSigmaX2 = 0.0;
    uint64_t totalDecodedFrames = 0;

    while (true) {
        const auto result = reader.read(buffer.data(), kChunkFrames, cancelFlag);
        if (result.status == NativeFloatReader::ReadStatus::Cancelled) {
            out.errorCode = "cancelled";
            return false;
        }
        const uint64_t framesRead = result.framesRead;
        if (framesRead == 0) break;

        for (std::uint64_t f = 0; f < framesRead; ++f) {
            for (std::uint32_t ch = 0; ch < channels; ++ch) {
                const float s = buffer[(size_t)(f * channels + ch)];
                if (s > peakByCh[(size_t)ch]) peakByCh[(size_t)ch] = s;
                if (s < minPeakByCh[(size_t)ch]) minPeakByCh[(size_t)ch] = s;
                if (withRms) {
                    double sd = (double)s;
                    sumSqByCh[(size_t)ch] += sd * sd;
                    if (computeTrueRms) {
                        totalSigmaX2 += sd * sd;
                    }
                }
            }
            totalDecodedFrames++;
            blockFrames++;
            if (blockFrames >= (std::uint64_t)samplesPerPeak) {
                for (std::uint32_t ch = 0; ch < channels; ++ch) {
                    out.peaks.push_back(peakByCh[(size_t)ch]);
                    out.minPeaks.push_back(minPeakByCh[(size_t)ch]);
                    if (withRms) {
                        out.rms.push_back((float)std::sqrt(sumSqByCh[(size_t)ch] / (double)blockFrames));
                    }
                    peakByCh[(size_t)ch] = 0.0f;
                    minPeakByCh[(size_t)ch] = 0.0f;
                    if (withRms) {
                        sumSqByCh[(size_t)ch] = 0.0;
                    }
                }
                blockFrames = 0;
            }
        }
    }

    // 最后一个不满 samplesPerPeak 的尾块
    if (blockFrames > 0) {
        for (std::uint32_t ch = 0; ch < channels; ++ch) {
            out.peaks.push_back(peakByCh[(size_t)ch]);
            out.minPeaks.push_back(minPeakByCh[(size_t)ch]);
            if (withRms) {
                out.rms.push_back((float)std::sqrt(sumSqByCh[(size_t)ch] / (double)blockFrames));
            }
        }
    }

    reader.close();

    // 🔍 诊断：对比波形解码帧数 vs totalFrames，与频谱侧对比排查对齐问题
    DIAG_ACTION_FMT("Waveform",
        "WAVE_DECODE: file=%s ch=%u totalFrames=%llu decodedFrames=%llu diff=%lld peaks=%zu samplesPerPeak=%d",
        filePath.c_str(), (unsigned)channels,
        (unsigned long long)totalFrames, (unsigned long long)totalDecodedFrames,
        (long long)totalDecodedFrames - (long long)totalFrames,
        out.peaks.size(), samplesPerPeak);

    if (out.peaks.empty()) {
        out.errorCode = "decode_no_frames";
        return false;
    }

    // 计算真实 RMS dBFS（全文件，所有声道合并，匹配 ffmpeg astats）
    if (computeTrueRms && totalDecodedFrames > 0 && channels > 0) {
        uint64_t totalSamples = totalDecodedFrames * (uint64_t)channels;
        double rmsLinear = std::sqrt(totalSigmaX2 / (double)totalSamples);
        out.trueRmsDbfs = (rmsLinear > 1e-10)
            ? std::clamp((float)(20.0 * std::log10(rmsLinear)), -100.0f, 0.0f)
            : -100.0f;
        out.hasTrueRms = true;
    }

    out.ok = true;
    return true;
}

static bool ExtractDetailTwoPassSampleAccurate(const std::string& filePath,
                                               int targetPeaks,
                                               bool withRms,
                                               WaveformExtractResult& out,
                                               const std::atomic<bool>* cancelFlag = nullptr) {
    out = {};
    if (filePath.empty()) {
        out.errorCode = "empty_path";
        return false;
    }

    targetPeaks = std::clamp(targetPeaks, 200, 50000);

    NativeFloatReader reader;
    if (!reader.open(filePath)) {
        out.errorCode = "decoder_init_failed";
        return false;
    }

    uint64_t reportedFrames = reader.metadata().hasReportedFrames
        ? reader.metadata().reportedFrames : 0;
    if (reportedFrames == 0) {
        reader.close();
        out.errorCode = "empty_audio";
        return false;
    }

    const std::uint32_t channels = reader.metadata().channels > 0 ? reader.metadata().channels : 1;
    const std::uint32_t sampleRate = reader.metadata().sampleRate > 0 ? reader.metadata().sampleRate : 48000;
    constexpr std::uint64_t kChunkFrames = 8192;
    std::vector<float> buffer((size_t)kChunkFrames * (size_t)channels);

    std::uint64_t decodedFrames = 0;
    while (true) {
        const auto result = reader.read(buffer.data(), kChunkFrames, cancelFlag);
        if (result.status == NativeFloatReader::ReadStatus::Cancelled) {
            out.errorCode = "cancelled";
            return false;
        }
        const uint64_t framesRead = result.framesRead;
        if (framesRead == 0) break;
        decodedFrames += framesRead;
    }
    reader.close();
    if (decodedFrames == 0) {
        out.errorCode = "decode_no_frames";
        return false;
    }

    reader.close();
    if (!reader.open(filePath)) {
        out.errorCode = "decoder_reinit_failed";
        return false;
    }

    const int effectivePeaks = (int)std::min<std::uint64_t>((std::uint64_t)targetPeaks, decodedFrames);
    int samplesPerPeak = (int)(decodedFrames / (std::uint64_t)effectivePeaks);
    if (samplesPerPeak < 1) samplesPerPeak = 1;

    out.channels = (int)channels;
    out.sampleRate = sampleRate;
    out.samplesPerPeak = samplesPerPeak;
    out.decodedFrames = decodedFrames;
    const size_t outValues = (size_t)effectivePeaks * (size_t)channels;
    out.peaks.assign(outValues, 0.0f);
    out.minPeaks.assign(outValues, 0.0f);
    if (withRms) out.rms.assign(outValues, 0.0f);
    else out.rms.clear();

    std::vector<float> peakByCh((size_t)channels, 0.0f);
    std::vector<float> minPeakByCh((size_t)channels, 0.0f);
    std::vector<double> sumSqByCh;
    if (withRms) sumSqByCh.assign((size_t)channels, 0.0);

    const bool computeTrueRms = withRms;
    double totalSigmaX2 = 0.0;
    uint64_t totalDecodedFrames = 0;
    std::uint64_t bucketFrames = 0;
    int currentBucket = 0;

    auto bucketEndFrame = [&](int bucket) -> std::uint64_t {
        return (std::uint64_t)((((uint64_t)bucket + 1u) * (uint64_t)decodedFrames) /
                           (uint64_t)effectivePeaks);
    };
    auto flushBucket = [&]() {
        if (currentBucket < 0 || currentBucket >= effectivePeaks || bucketFrames == 0) return;
        const int base = currentBucket * (int)channels;
        for (std::uint32_t ch = 0; ch < channels; ++ch) {
            out.peaks[(size_t)(base + (int)ch)] = peakByCh[(size_t)ch];
            out.minPeaks[(size_t)(base + (int)ch)] = minPeakByCh[(size_t)ch];
            if (withRms) {
                out.rms[(size_t)(base + (int)ch)] =
                    (float)std::sqrt(sumSqByCh[(size_t)ch] / (double)bucketFrames);
            }
            peakByCh[(size_t)ch] = 0.0f;
            minPeakByCh[(size_t)ch] = 0.0f;
            if (withRms) sumSqByCh[(size_t)ch] = 0.0;
        }
        bucketFrames = 0;
    };

    std::uint64_t globalFrame = 0;
    std::uint64_t currentBucketEnd = bucketEndFrame(0);
    while (true) {
        const auto result = reader.read(buffer.data(), kChunkFrames, cancelFlag);
        if (result.status == NativeFloatReader::ReadStatus::Cancelled) {
            out.errorCode = "cancelled";
            return false;
        }
        const uint64_t framesRead = result.framesRead;
        if (framesRead == 0) break;

        for (std::uint64_t f = 0; f < framesRead; ++f) {
            while (globalFrame >= currentBucketEnd && currentBucket + 1 < effectivePeaks) {
                flushBucket();
                ++currentBucket;
                currentBucketEnd = bucketEndFrame(currentBucket);
            }
            for (std::uint32_t ch = 0; ch < channels; ++ch) {
                const float s = buffer[(size_t)(f * channels + ch)];
                if (s > peakByCh[(size_t)ch]) peakByCh[(size_t)ch] = s;
                if (s < minPeakByCh[(size_t)ch]) minPeakByCh[(size_t)ch] = s;
                if (withRms) {
                    const double sd = (double)s;
                    sumSqByCh[(size_t)ch] += sd * sd;
                    if (computeTrueRms) totalSigmaX2 += sd * sd;
                }
            }
            totalDecodedFrames++;
            bucketFrames++;
            globalFrame++;
        }
    }
    flushBucket();
    reader.close();

    DIAG_ACTION_FMT("Waveform",
        "WAVE_DECODE_EXACT: file=%s ch=%u reportedFrames=%llu decodedFrames=%llu diff=%lld peaks=%zu samplesPerPeak=%d",
        filePath.c_str(), (unsigned)channels,
        (unsigned long long)reportedFrames, (unsigned long long)totalDecodedFrames,
        (long long)totalDecodedFrames - (long long)reportedFrames,
        out.peaks.size(), samplesPerPeak);

    if (out.peaks.empty()) {
        out.errorCode = "decode_no_frames";
        return false;
    }

    if (computeTrueRms && totalDecodedFrames > 0 && channels > 0) {
        uint64_t totalSamples = totalDecodedFrames * (uint64_t)channels;
        const double rmsLinear = std::sqrt(totalSigmaX2 / (double)totalSamples);
        out.trueRmsDbfs = (rmsLinear > 1e-10)
            ? std::clamp((float)(20.0 * std::log10(rmsLinear)), -100.0f, 0.0f)
            : -100.0f;
        out.hasTrueRms = true;
    }

    out.ok = true;
    return true;
}

static bool ExtractDetailSampleAccurate(const std::string& filePath,
                                        int targetPeaks,
                                        bool withRms,
                                        WaveformExtractResult& out,
                                        const std::atomic<bool>* cancelFlag = nullptr) {
    out = {};
    if (filePath.empty()) {
        out.errorCode = "empty_path";
        return false;
    }

    targetPeaks = std::clamp(targetPeaks, 200, 50000);

    NativeFloatReader reader;
    if (!reader.open(filePath)) {
        out.errorCode = "decoder_init_failed";
        return false;
    }

    uint64_t reportedFrames = reader.metadata().hasReportedFrames
        ? reader.metadata().reportedFrames : 0;

    const std::uint32_t channels = reader.metadata().channels > 0 ? reader.metadata().channels : 1;
    const std::uint32_t sampleRate = reader.metadata().sampleRate > 0 ? reader.metadata().sampleRate : 48000;
    constexpr std::uint64_t kChunkFrames = 8192;
    constexpr size_t kSinglePassSampleCapBytes = 384ull * 1024ull * 1024ull;
    constexpr size_t kSinglePassSampleCapFloats = kSinglePassSampleCapBytes / sizeof(float);
    std::vector<float> buffer((size_t)kChunkFrames * (size_t)channels);
    std::vector<float> samples;
    if (reportedFrames > 0) {
        const uint64_t reportedSamples = (uint64_t)reportedFrames * (uint64_t)channels;
        if (reportedSamples <= (uint64_t)kSinglePassSampleCapFloats) {
            samples.reserve((size_t)reportedSamples);
        }
    }

    uint64_t decodedFrames = 0;
    double totalSigmaX2 = 0.0;
    bool exceededSampleCap = false;
    while (true) {
        const auto result = reader.read(buffer.data(), kChunkFrames, cancelFlag);
        if (result.status == NativeFloatReader::ReadStatus::Cancelled) {
            out.errorCode = "cancelled";
            return false;
        }
        const uint64_t framesRead = result.framesRead;
        if (framesRead == 0) break;

        const size_t floatsRead = (size_t)framesRead * (size_t)channels;
        if (samples.size() + floatsRead > kSinglePassSampleCapFloats) {
            exceededSampleCap = true;
            break;
        }
        samples.insert(samples.end(), buffer.begin(), buffer.begin() + (ptrdiff_t)floatsRead);
        if (withRms) {
            for (size_t i = 0; i < floatsRead; ++i) {
                const double sd = (double)buffer[i];
                totalSigmaX2 += sd * sd;
            }
        }
        decodedFrames += framesRead;
    }
    reader.close();

    if (exceededSampleCap) {
        DIAG_ACTION_FMT("Waveform",
            "WAVE_DECODE_EXACT_FALLBACK_TWOPASS: file=%s reportedFrames=%llu decodedFramesSoFar=%llu channels=%u capMB=%zu",
            filePath.c_str(),
            (unsigned long long)reportedFrames,
            (unsigned long long)decodedFrames,
            (unsigned)channels,
            kSinglePassSampleCapBytes / (1024ull * 1024ull));
        return ExtractDetailTwoPassSampleAccurate(filePath, targetPeaks, withRms, out, cancelFlag);
    }

    if (decodedFrames == 0 || samples.empty()) {
        out.errorCode = "decode_no_frames";
        return false;
    }

    const int effectivePeaks = (int)std::min<uint64_t>((uint64_t)targetPeaks, decodedFrames);
    int samplesPerPeak = (int)(decodedFrames / (uint64_t)effectivePeaks);
    if (samplesPerPeak < 1) samplesPerPeak = 1;

    out.channels = (int)channels;
    out.sampleRate = sampleRate;
    out.samplesPerPeak = samplesPerPeak;
    out.decodedFrames = decodedFrames;
    const size_t outValues = (size_t)effectivePeaks * (size_t)channels;
    out.peaks.assign(outValues, 0.0f);
    out.minPeaks.assign(outValues, 0.0f);
    if (withRms) out.rms.assign(outValues, 0.0f);
    else out.rms.clear();

    for (int bucket = 0; bucket < effectivePeaks; ++bucket) {
        const uint64_t frameBegin = ((uint64_t)bucket * decodedFrames) / (uint64_t)effectivePeaks;
        uint64_t frameEnd = (((uint64_t)bucket + 1u) * decodedFrames) / (uint64_t)effectivePeaks;
        if (frameEnd <= frameBegin) frameEnd = frameBegin + 1;
        frameEnd = std::min<uint64_t>(frameEnd, decodedFrames);
        const uint64_t bucketFrames = frameEnd - frameBegin;
        const size_t dstBase = (size_t)bucket * (size_t)channels;
        for (std::uint32_t ch = 0; ch < channels; ++ch) {
            float maxPeak = 0.0f;
            float minPeak = 0.0f;
            double sumSq = 0.0;
            for (uint64_t frame = frameBegin; frame < frameEnd; ++frame) {
                const float s = samples[(size_t)frame * (size_t)channels + (size_t)ch];
                if (s > maxPeak) maxPeak = s;
                if (s < minPeak) minPeak = s;
                if (withRms) {
                    const double sd = (double)s;
                    sumSq += sd * sd;
                }
            }
            out.peaks[dstBase + (size_t)ch] = maxPeak;
            out.minPeaks[dstBase + (size_t)ch] = minPeak;
            if (withRms) {
                out.rms[dstBase + (size_t)ch] =
                    (float)std::sqrt(sumSq / (double)std::max<uint64_t>(1, bucketFrames));
            }
        }
    }

    if (withRms && decodedFrames > 0 && channels > 0) {
        const uint64_t totalSamples = decodedFrames * (uint64_t)channels;
        const double rmsLinear = std::sqrt(totalSigmaX2 / (double)totalSamples);
        out.trueRmsDbfs = (rmsLinear > 1e-10)
            ? std::clamp((float)(20.0 * std::log10(rmsLinear)), -100.0f, 0.0f)
            : -100.0f;
        out.hasTrueRms = true;
    }

    DIAG_ACTION_FMT("Waveform",
        "WAVE_DECODE_EXACT: file=%s ch=%u reportedFrames=%llu decodedFrames=%llu diff=%lld peaks=%zu samplesPerPeak=%d mode=single_pass",
        filePath.c_str(), (unsigned)channels,
        (unsigned long long)reportedFrames, (unsigned long long)decodedFrames,
        (long long)decodedFrames - (long long)reportedFrames,
        out.peaks.size(), samplesPerPeak);

    out.ok = true;
    return true;
}

} // namespace

bool WaveformExtractor::EncodePeakEnvelopeInt16(const std::vector<float>& peaks,
                                                const std::vector<float>& minPeaks,
                                                int channels,
                                                int bucketCount,
                                                std::vector<uint8_t>& outBlob) {
    outBlob.clear();
    if (channels <= 0 || channels > 255 || bucketCount <= 0) {
        return false;
    }
    const size_t expected = (size_t)channels * (size_t)bucketCount;
    if (peaks.size() != expected || minPeaks.size() != expected) {
        return false;
    }

    outBlob.insert(outBlob.end(), std::begin(kWpk2Header), std::end(kWpk2Header));
    outBlob[6] = static_cast<uint8_t>(channels);
    outBlob.reserve(kWpk2HeaderSize + expected * 4);
    for (size_t i = 0; i < expected; ++i) {
        WriteLe16(outBlob, QuantizePeakInt16(minPeaks[i]));
        WriteLe16(outBlob, QuantizePeakInt16(peaks[i]));
    }
    return true;
}

bool WaveformExtractor::IsPeakEnvelopeInt16Blob(const std::vector<uint8_t>& blob) {
    if (blob.size() < kWpk2HeaderSize) {
        return false;
    }
    if (blob[0] != kWpk2Header[0] ||
        blob[1] != kWpk2Header[1] ||
        blob[2] != kWpk2Header[2] ||
        blob[3] != kWpk2Header[3] ||
        blob[4] != kWpk2Header[4] ||
        blob[5] != kWpk2Header[5] ||
        blob[7] != kWpk2Header[7]) {
        return false;
    }
    const int channels = (int)blob[6];
    if (channels <= 0) {
        return false;
    }
    const size_t payloadBytes = blob.size() - kWpk2HeaderSize;
    if (payloadBytes == 0 || (payloadBytes % 4) != 0) {
        return false;
    }
    const size_t samples = payloadBytes / 4;
    return (samples % (size_t)channels) == 0;
}

bool WaveformExtractor::DecodePeakEnvelopeInt16(const std::vector<uint8_t>& blob,
                                                std::vector<float>& outPeaks,
                                                std::vector<float>& outMinPeaks,
                                                int& outChannels,
                                                int& outBucketCount) {
    outPeaks.clear();
    outMinPeaks.clear();
    outChannels = 0;
    outBucketCount = 0;
    if (!IsPeakEnvelopeInt16Blob(blob)) {
        return false;
    }

    outChannels = (int)blob[6];
    const size_t samples = (blob.size() - kWpk2HeaderSize) / 4;
    outBucketCount = (int)(samples / (size_t)outChannels);
    outPeaks.reserve(samples);
    outMinPeaks.reserve(samples);
    size_t offset = kWpk2HeaderSize;
    for (size_t i = 0; i < samples; ++i) {
        const int16_t minValue = static_cast<int16_t>(ReadLe16(blob.data() + offset));
        offset += 2;
        const int16_t maxValue = static_cast<int16_t>(ReadLe16(blob.data() + offset));
        offset += 2;
        outMinPeaks.push_back(DequantizePeakInt16(minValue));
        outPeaks.push_back(DequantizePeakInt16(maxValue));
    }
    return true;
}

bool WaveformExtractor::ExtractMiniReturningDecoded(
    const std::string& filePath,
    int targetPeaks,
    WaveformExtractResult& out,
    bool perChannel,
    const std::atomic<bool>* cancelFlag,
    HarkenSearch::MediaFoundationAudio::DecodedAudioBuffer& outDecoded) {
    if (filePath.find('\0') != std::string::npos) {
        out = {};
        outDecoded = {};
        out.errorCode = "decoder_init_failed";
        return false;
    }
    // targetPeaks <= 0 is the adaptive-density sentinel (new DBs): each decode
    // path resolves it from the source frame count once frames are known, so we
    // must NOT clamp it away here. Fixed legacy counts (128/160) clamp normally;
    // the ceiling stays bounded so mini caches remain predictable at 3M+ scale.
    if (targetPeaks > 0) targetPeaks = std::clamp(targetPeaks, 128, 1024);

    if (HasWavExtension(filePath)) {
        if (ExtractMiniSparseWav(filePath, targetPeaks, out, perChannel, cancelFlag)) {
            if (HasAnyPositivePeak(out)) {
                return true;
            }
        } else if (out.errorCode == "cancelled") {
            return false;
        }
    }

    const bool isFlacPath = HarkenSearch::MediaFoundationAudio::HasFlacExtension(filePath);
    if (HarkenSearch::MediaFoundationAudio::CanDecodeExtension(filePath) && !isFlacPath) {
        std::string mfError;
        if (!HarkenSearch::MediaFoundationAudio::Decode(filePath, outDecoded, &mfError, cancelFlag)) {
            out = {};
            out.errorCode = HarkenSearch::MediaFoundationAudio::ClassifyFailure(mfError);
            DIAG_ACTION_FMT("Waveform",
                "MediaFoundationMiniDecodeFailed: path=%s format=%s error=%s",
                filePath.c_str(),
                HarkenSearch::MediaFoundationAudio::FormatNameFromPath(filePath),
                out.errorCode.c_str());
            return false;
        }
        int mfTargetPeaks = targetPeaks;
        if (mfTargetPeaks <= 0)
            mfTargetPeaks = std::clamp(ComputeAdaptiveMiniPeakCount(outDecoded.totalFrames), 128, 1024);
        const bool ok = HarkenSearch::MediaFoundationAudio::BuildPeaksFromDecoded(
            outDecoded, mfTargetPeaks, true, perChannel, out, cancelFlag);
        if (ok) {
            DIAG_ACTION_FMT("Waveform",
                "MediaFoundationMiniDecodeOK: path=%s format=%s ch=%u frames=%llu peaks=%zu",
                filePath.c_str(),
                HarkenSearch::MediaFoundationAudio::FormatNameFromPath(filePath),
                (unsigned)outDecoded.channels,
                (unsigned long long)outDecoded.totalFrames,
                out.peaks.size());
        }
        return ok;
    }

    // For non-MF formats, fall through to the normal mini extraction paths.
    // outDecoded stays empty — callers should defer meters via Lever A.
    {
        if (IsCancelled(cancelFlag)) {
            out = {};
            out.errorCode = "cancelled";
            return false;
        }
        NativeFloatReader probeReader;
        if (probeReader.open(filePath)) {
            const uint64_t probeTotal = probeReader.metadata().hasReportedFrames
                ? probeReader.metadata().reportedFrames : 0;
            const std::uint32_t probeSr = probeReader.metadata().sampleRate > 0
                ? probeReader.metadata().sampleRate : 48000;
            probeReader.close();
            const double durSec = (double)probeTotal / (double)probeSr;
            if (!HasWavExtension(filePath) && durSec >= kMiniSequentialMinDurationSec) {
                if (ExtractMiniSequential(filePath, targetPeaks, out, perChannel, cancelFlag)) {
                    return true;
                }
                if (out.errorCode == "cancelled") return false;
            }
        }
    }

    if (!ExtractMiniSeekBased(filePath, targetPeaks, out, perChannel, cancelFlag)) {
        if (isFlacPath && out.errorCode == "decoder_init_failed") {
            std::string mfError;
            if (!HarkenSearch::MediaFoundationAudio::Decode(filePath, outDecoded, &mfError, cancelFlag)) {
                out = {};
                out.errorCode = HarkenSearch::MediaFoundationAudio::ClassifyFailure(mfError);
                DIAG_ACTION_FMT("Waveform",
                    "MediaFoundationMiniDecodeFailed: path=%s format=%s error=%s fallback=flac",
                    filePath.c_str(),
                    HarkenSearch::MediaFoundationAudio::FormatNameFromPath(filePath),
                    out.errorCode.c_str());
                return false;
            }
            int mfTargetPeaks = targetPeaks;
            if (mfTargetPeaks <= 0)
                mfTargetPeaks = std::clamp(ComputeAdaptiveMiniPeakCount(outDecoded.totalFrames), 128, 1024);
            const bool ok = HarkenSearch::MediaFoundationAudio::BuildPeaksFromDecoded(
                outDecoded, mfTargetPeaks, true, perChannel, out, cancelFlag);
            if (ok) {
                DIAG_ACTION_FMT("Waveform",
                    "MediaFoundationMiniDecodeOK: path=%s format=%s ch=%u frames=%llu peaks=%zu fallback=flac",
                    filePath.c_str(),
                    HarkenSearch::MediaFoundationAudio::FormatNameFromPath(filePath),
                    (unsigned)outDecoded.channels,
                    (unsigned long long)outDecoded.totalFrames,
                    out.peaks.size());
            }
            return ok;
        }
        return false;
    }

    bool allZero = !HasAnyPositivePeak(out);
    if (allZero && !out.peaks.empty()) {
        WaveformExtractResult seqOut;
        if (ExtractMiniSequential(filePath, targetPeaks, seqOut, perChannel, cancelFlag) && !seqOut.peaks.empty()) {
            int ch = seqOut.channels > 0 ? seqOut.channels : 1;
            if (!perChannel && ch > 1) {
                std::vector<float> mono;
                mono.reserve(seqOut.peaks.size() / (size_t)ch);
                for (size_t i = 0; i + (size_t)ch <= seqOut.peaks.size(); i += (size_t)ch) {
                    float mx = 0.0f;
                    for (int c = 0; c < ch; ++c)
                        mx = std::max(mx, seqOut.peaks[i + c]);
                    mono.push_back(mx);
                }
                seqOut.peaks = std::move(mono);
                if (seqOut.minPeaks.size() == seqOut.peaks.size() * (size_t)ch) {
                    std::vector<float> monoMin;
                    monoMin.reserve(seqOut.minPeaks.size() / (size_t)ch);
                    for (size_t i = 0; i + (size_t)ch <= seqOut.minPeaks.size(); i += (size_t)ch) {
                        float mn = 0.0f;
                        for (int c = 0; c < ch; ++c)
                            mn = std::min(mn, seqOut.minPeaks[i + c]);
                        monoMin.push_back(mn);
                    }
                    seqOut.minPeaks = std::move(monoMin);
                } else {
                    seqOut.minPeaks.clear();
                }
                if (!seqOut.rms.empty()) {
                    std::vector<float> monoRms;
                    monoRms.reserve(seqOut.rms.size() / (size_t)ch);
                    for (size_t i = 0; i + (size_t)ch <= seqOut.rms.size(); i += (size_t)ch) {
                        float mx = 0.0f;
                        for (int c = 0; c < ch; ++c)
                            mx = std::max(mx, seqOut.rms[i + c]);
                        monoRms.push_back(mx);
                    }
                    seqOut.rms = std::move(monoRms);
                }
                seqOut.channels = 1;
            }
            out = std::move(seqOut);
            out.errorCode.clear();
        }
    }
    return out.ok;
}

bool WaveformExtractor::ExtractMini(const std::string& filePath,
                                    int targetPeaks,
                                    WaveformExtractResult& out,
                                    bool perChannel,
                                    const std::atomic<bool>* cancelFlag) {
    HarkenSearch::MediaFoundationAudio::DecodedAudioBuffer discard;
    return ExtractMiniReturningDecoded(filePath, targetPeaks, out, perChannel, cancelFlag, discard);
}


bool WaveformExtractor::ExtractDetail(const std::string& filePath,
                                      int targetPeaks,
                                      bool withRms,
                                      WaveformExtractResult& out,
                                      const std::atomic<bool>* cancelFlag) {
    if (filePath.find('\0') != std::string::npos) {
        out = {};
        out.errorCode = "decoder_init_failed";
        return false;
    }
    const bool isFlacPath = HarkenSearch::MediaFoundationAudio::HasFlacExtension(filePath);
    if (HarkenSearch::MediaFoundationAudio::CanDecodeExtension(filePath) && !isFlacPath) {
        HarkenSearch::MediaFoundationAudio::DecodedAudioBuffer decoded;
        std::string mfError;
        if (!HarkenSearch::MediaFoundationAudio::Decode(filePath, decoded, &mfError, cancelFlag)) {
            out = {};
            out.errorCode = HarkenSearch::MediaFoundationAudio::ClassifyFailure(mfError);
            DIAG_ACTION_FMT("Waveform",
                "MediaFoundationDetailDecodeFailed: path=%s format=%s error=%s",
                filePath.c_str(),
                HarkenSearch::MediaFoundationAudio::FormatNameFromPath(filePath),
                out.errorCode.c_str());
            return false;
        }
        targetPeaks = std::clamp(targetPeaks, 200, 50000);
        const bool ok = HarkenSearch::MediaFoundationAudio::BuildPeaksFromDecoded(
            decoded, targetPeaks, withRms, true, out, cancelFlag);
        if (ok) {
            DIAG_ACTION_FMT("Waveform",
                "MediaFoundationDetailDecodeOK: path=%s format=%s ch=%u frames=%llu peaks=%zu",
                filePath.c_str(),
                HarkenSearch::MediaFoundationAudio::FormatNameFromPath(filePath),
                (unsigned)decoded.channels,
                (unsigned long long)decoded.totalFrames,
                out.peaks.size());
        }
        return ok;
    }
    if (ExtractDetailSampleAccurate(filePath, targetPeaks, withRms, out, cancelFlag)) {
        return true;
    }
    if (isFlacPath && out.errorCode == "decoder_init_failed") {
        HarkenSearch::MediaFoundationAudio::DecodedAudioBuffer decoded;
        std::string mfError;
        if (!HarkenSearch::MediaFoundationAudio::Decode(filePath, decoded, &mfError, cancelFlag)) {
            out = {};
            out.errorCode = HarkenSearch::MediaFoundationAudio::ClassifyFailure(mfError);
            DIAG_ACTION_FMT("Waveform",
                "MediaFoundationDetailDecodeFailed: path=%s format=%s error=%s fallback=flac",
                filePath.c_str(),
                HarkenSearch::MediaFoundationAudio::FormatNameFromPath(filePath),
                out.errorCode.c_str());
            return false;
        }
        targetPeaks = std::clamp(targetPeaks, 200, 50000);
        const bool ok = HarkenSearch::MediaFoundationAudio::BuildPeaksFromDecoded(
            decoded, targetPeaks, withRms, true, out, cancelFlag);
        if (ok) {
            DIAG_ACTION_FMT("Waveform",
                "MediaFoundationDetailDecodeOK: path=%s format=%s ch=%u frames=%llu peaks=%zu fallback=flac",
                filePath.c_str(),
                HarkenSearch::MediaFoundationAudio::FormatNameFromPath(filePath),
                (unsigned)decoded.channels,
                (unsigned long long)decoded.totalFrames,
                out.peaks.size());
        }
        return ok;
    }
    return false;
}

bool WaveformExtractor::ExtractMiniPreview(const std::string& filePath,
                                           int targetPeaks,
                                           WaveformExtractResult& out,
                                           bool perChannel,
                                           double maxDecodeSeconds) {
    out = {};
    if (filePath.empty()) {
        out.errorCode = "empty_path";
        return false;
    }

    targetPeaks = std::clamp(targetPeaks, 32, 1024);
    if (maxDecodeSeconds <= 0.0) maxDecodeSeconds = 1.0;

    NativeFloatReader reader;
    if (!reader.open(filePath)) {
        out.errorCode = "decoder_init_failed";
        return false;
    }

    const std::uint32_t actualChannels = reader.metadata().channels > 0 ? reader.metadata().channels : 1;
    const std::uint32_t sampleRate = reader.metadata().sampleRate > 0 ? reader.metadata().sampleRate : 48000;
    const bool wantStereo = perChannel && (actualChannels >= 2);
    const int outChannels = wantStereo ? 2 : 1;

    uint64_t totalFrames = reader.metadata().hasReportedFrames
        ? reader.metadata().reportedFrames : 0;
    uint64_t framesToDecode = (uint64_t)std::ceil(maxDecodeSeconds * (double)sampleRate);
    if (framesToDecode < (uint64_t)targetPeaks) framesToDecode = (uint64_t)targetPeaks;
    if (totalFrames > 0) framesToDecode = (std::min)(framesToDecode, totalFrames);

    uint64_t segmentSize = framesToDecode / (uint64_t)targetPeaks;
    if (segmentSize < 1) segmentSize = 1;

    out.channels = outChannels;
    out.sampleRate = sampleRate;
    out.samplesPerPeak = (int)segmentSize;
    out.peaks.reserve((size_t)targetPeaks * (size_t)outChannels);
    out.rms.reserve((size_t)targetPeaks * (size_t)outChannels);

    constexpr uint64_t kChunkFrames = 2048;
    std::vector<float> buffer((size_t)kChunkFrames * (size_t)actualChannels);

    double globalSumSq = 0.0;
    uint64_t globalSampleCount = 0;
    float bucketPeak = 0.0f, bucketPeakL = 0.0f, bucketPeakR = 0.0f;
    double bucketSumSq = 0.0, bucketSumSqL = 0.0, bucketSumSqR = 0.0;
    uint64_t bucketFrames = 0;
    uint64_t decodedFrames = 0;
    int peaksWritten = 0;

    auto flushBucket = [&]() {
        if (peaksWritten >= targetPeaks) return;
        if (wantStereo) {
            const double denom = bucketFrames > 0 ? (double)bucketFrames : 1.0;
            out.peaks.push_back(bucketPeakL);
            out.peaks.push_back(bucketPeakR);
            out.rms.push_back((float)std::sqrt(bucketSumSqL / denom));
            out.rms.push_back((float)std::sqrt(bucketSumSqR / denom));
            globalSumSq += bucketSumSqL + bucketSumSqR;
            globalSampleCount += (uint64_t)bucketFrames * 2;
        } else {
            const double denom = (bucketFrames * actualChannels) > 0
                ? (double)(bucketFrames * actualChannels) : 1.0;
            out.peaks.push_back(bucketPeak);
            out.rms.push_back((float)std::sqrt(bucketSumSq / denom));
            globalSumSq += bucketSumSq;
            globalSampleCount += (uint64_t)bucketFrames * actualChannels;
        }
        ++peaksWritten;
        bucketPeak = bucketPeakL = bucketPeakR = 0.0f;
        bucketSumSq = bucketSumSqL = bucketSumSqR = 0.0;
        bucketFrames = 0;
    };

    while (peaksWritten < targetPeaks && decodedFrames < framesToDecode) {
        const uint64_t requestFrames = (std::min)(kChunkFrames, framesToDecode - decodedFrames);
        const auto result = reader.read(buffer.data(), requestFrames, nullptr);
        const uint64_t framesRead = result.framesRead;
        if (framesRead == 0) break;

        std::uint64_t cursor = 0;
        while (cursor < framesRead && peaksWritten < targetPeaks) {
            const std::uint64_t framesNeeded = segmentSize - bucketFrames;
            const std::uint64_t framesAvail = framesRead - cursor;
            const std::uint64_t take = (framesNeeded < framesAvail) ? framesNeeded : framesAvail;

            if (wantStereo) {
                for (std::uint64_t f = 0; f < take; ++f) {
                    const float sL = buffer[(size_t)((cursor + f) * actualChannels + 0)];
                    const float sR = buffer[(size_t)((cursor + f) * actualChannels + 1)];
                    const float aL = std::abs(sL), aR = std::abs(sR);
                    if (aL > bucketPeakL) bucketPeakL = aL;
                    if (aR > bucketPeakR) bucketPeakR = aR;
                    bucketSumSqL += (double)sL * (double)sL;
                    bucketSumSqR += (double)sR * (double)sR;
                }
            } else {
                const std::uint64_t sampleStart = cursor * actualChannels;
                const std::uint64_t sampleEnd = (cursor + take) * actualChannels;
                for (std::uint64_t i = sampleStart; i < sampleEnd; ++i) {
                    const float s = buffer[(size_t)i];
                    const float a = std::abs(s);
                    if (a > bucketPeak) bucketPeak = a;
                    bucketSumSq += (double)s * (double)s;
                }
            }

            bucketFrames += take;
            cursor += take;
            if (bucketFrames >= segmentSize) flushBucket();
        }

        decodedFrames += framesRead;
        if (result.status == NativeFloatReader::ReadStatus::Error) {
            out.errorCode = result.error.empty() ? "decoder_read_failed" : result.error;
            break;
        }
    }

    if (bucketFrames > 0 && peaksWritten < targetPeaks) flushBucket();
    while (peaksWritten < targetPeaks) {
        for (int c = 0; c < outChannels; ++c) {
            out.peaks.push_back(0.0f);
            out.rms.push_back(0.0f);
        }
        ++peaksWritten;
    }

    reader.close();

    if (globalSampleCount > 0) {
        const double rmsLinear = std::sqrt(globalSumSq / (double)globalSampleCount);
        out.trueRmsDbfs = (rmsLinear > 1e-10)
            ? std::clamp((float)(20.0 * std::log10(rmsLinear)), -100.0f, 0.0f)
            : -100.0f;
        out.hasTrueRms = true;
    }

    out.ok = !out.peaks.empty();
    if (!out.ok) out.errorCode = "preview_empty";
    return out.ok;
}

float WaveformExtractor::ComputeTrueRmsDbfs(const std::string& filePath) {
    if (filePath.empty()) return -100.0f;

    NativeFloatReader reader;
    if (!reader.open(filePath)) return -100.0f;

    const std::uint32_t channels = reader.metadata().channels > 0 ? reader.metadata().channels : 1;

    constexpr uint64_t kChunk = 8192;
    std::vector<float> buf((size_t)kChunk * (size_t)channels);
    double sumSq = 0.0;
    uint64_t totalSamples = 0;

    while (true) {
        const auto result = reader.read(buf.data(), kChunk, nullptr);
        const uint64_t read = result.framesRead;
        if (read == 0) break;
        const uint64_t n = (uint64_t)read * (uint64_t)channels;
        for (uint64_t i = 0; i < n; ++i) {
            double s = (double)buf[(size_t)i];
            sumSq += s * s;
        }
        totalSamples += n;
        if (result.status == NativeFloatReader::ReadStatus::Error) break;
    }

    reader.close();

    if (totalSamples == 0) return -100.0f;
    double rms = std::sqrt(sumSq / (double)totalSamples);
    return (rms > 1e-10) ? std::clamp((float)(20.0 * std::log10(rms)), -100.0f, 0.0f) : -100.0f;
}

bool WaveformExtractor::ExtractExactMonoEnvelope(const std::string& filePath,
                                                 int targetPeaks,
                                                 std::vector<float>& outPeaks) {
    outPeaks.clear();
    WaveformExtractResult detail;
    int monoChannels = 1;
    return targetPeaks > 0 &&
           ExtractDetailSequential(filePath, targetPeaks, false, detail) &&
           BuildMiniFromDetail(detail.peaks, detail.minPeaks, detail.channels,
                               targetPeaks, false, outPeaks, monoChannels);
}

bool WaveformExtractor::BuildMiniFromDetail(const std::vector<float>& maxDetail,
                                            const std::vector<float>& minDetail,
                                            int detailChannels,
                                            int targetFrames,
                                            bool perChannel,
                                            std::vector<float>& outPeaks,
                                            int& outChannels) {
    outPeaks.clear();
    outChannels = 1;
    if (detailChannels <= 0 || targetFrames <= 0 || maxDetail.empty()) {
        return false;
    }
    const int sourceFrames = (int)maxDetail.size() / detailChannels;
    if (sourceFrames <= 0) return false;

    const bool hasMinDetail = minDetail.size() >= maxDetail.size();
    outChannels = perChannel ? detailChannels : 1;
    outPeaks.assign((size_t)targetFrames * (size_t)outChannels, 0.0f);

    for (int frame = 0; frame < targetFrames; ++frame) {
        int begin = (int)(((int64_t)frame * sourceFrames) / targetFrames);
        int end = (int)(((int64_t)(frame + 1) * sourceFrames) / targetFrames);
        begin = std::clamp(begin, 0, sourceFrames - 1);
        end = std::clamp(end, begin + 1, sourceFrames);

        if (perChannel) {
            for (int ch = 0; ch < detailChannels; ++ch) {
                float peak = 0.0f;
                for (int src = begin; src < end; ++src) {
                    const size_t idx = (size_t)src * (size_t)detailChannels + (size_t)ch;
                    peak = std::max(peak, std::fabs(maxDetail[idx]));
                    if (hasMinDetail) peak = std::max(peak, std::fabs(minDetail[idx]));
                }
                outPeaks[(size_t)frame * (size_t)outChannels + (size_t)ch] = peak;
            }
        } else {
            float peak = 0.0f;
            for (int src = begin; src < end; ++src) {
                for (int ch = 0; ch < detailChannels; ++ch) {
                    const size_t idx = (size_t)src * (size_t)detailChannels + (size_t)ch;
                    peak = std::max(peak, std::fabs(maxDetail[idx]));
                    if (hasMinDetail) peak = std::max(peak, std::fabs(minDetail[idx]));
                }
            }
            outPeaks[(size_t)frame] = peak;
        }
    }
    return true;
}

bool WaveformExtractor::BuildMiniEnvelopeFromDetail(const std::vector<float>& maxDetail,
                                                    const std::vector<float>& minDetail,
                                                    int detailChannels,
                                                    int targetFrames,
                                                    bool perChannel,
                                                    std::vector<float>& outMax,
                                                    std::vector<float>& outMin,
                                                    int& outChannels) {
    outMax.clear();
    outMin.clear();
    outChannels = 1;
    if (detailChannels <= 0 || targetFrames <= 0 || maxDetail.empty()) {
        return false;
    }
    if ((maxDetail.size() % (size_t)detailChannels) != 0) {
        return false;
    }

    const int sourceFrames = (int)(maxDetail.size() / (size_t)detailChannels);
    if (sourceFrames <= 0) return false;

    const bool hasMinDetail = minDetail.size() >= maxDetail.size();
    outChannels = perChannel ? detailChannels : 1;
    outMax.assign((size_t)targetFrames * (size_t)outChannels, 0.0f);
    outMin.assign((size_t)targetFrames * (size_t)outChannels, 0.0f);

    auto fallbackMin = [&](size_t idx) {
        return -std::fabs(maxDetail[idx]);
    };

    for (int frame = 0; frame < targetFrames; ++frame) {
        int begin = (int)(((int64_t)frame * sourceFrames) / targetFrames);
        int end = (int)(((int64_t)(frame + 1) * sourceFrames) / targetFrames);
        begin = std::clamp(begin, 0, sourceFrames - 1);
        end = std::clamp(end, begin + 1, sourceFrames);

        if (perChannel) {
            for (int ch = 0; ch < detailChannels; ++ch) {
                float maxPeak = 0.0f;
                float minPeak = 0.0f;
                for (int src = begin; src < end; ++src) {
                    const size_t idx = (size_t)src * (size_t)detailChannels + (size_t)ch;
                    maxPeak = std::max(maxPeak, maxDetail[idx]);
                    minPeak = std::min(minPeak, hasMinDetail ? minDetail[idx] : fallbackMin(idx));
                }
                const size_t outIdx = (size_t)frame * (size_t)outChannels + (size_t)ch;
                outMax[outIdx] = maxPeak;
                outMin[outIdx] = minPeak;
            }
        } else {
            float maxPeak = 0.0f;
            float minPeak = 0.0f;
            for (int src = begin; src < end; ++src) {
                for (int ch = 0; ch < detailChannels; ++ch) {
                    const size_t idx = (size_t)src * (size_t)detailChannels + (size_t)ch;
                    maxPeak = std::max(maxPeak, maxDetail[idx]);
                    minPeak = std::min(minPeak, hasMinDetail ? minDetail[idx] : fallbackMin(idx));
                }
            }
            outMax[(size_t)frame] = maxPeak;
            outMin[(size_t)frame] = minPeak;
        }
    }
    return true;
}

std::string WaveformExtractor::BuildCacheKey(const std::string& filePath,
                                             uint64_t fileSize,
                                             int64_t fileMtime,
                                             int schemaVersion,
                                             int extractorVersion) {
    const uint64_t h = (uint64_t)XXH3_64bits(filePath.data(), filePath.size());
    std::ostringstream oss;
    oss << "wf:" << schemaVersion << ":" << extractorVersion
        << ":" << std::hex << h
        << ":" << std::dec << fileSize
        << ":" << fileMtime;
    return oss.str();
}
