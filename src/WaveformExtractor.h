#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include "MediaFoundationAudioDecoder.h"

struct WaveformExtractResult {
    std::vector<float> peaks;
    std::vector<float> minPeaks;   // Negative-side peak (signed, <= 0) per block for DAW-style asymmetric rendering
    std::vector<float> rms;
    int channels = 0;
    int samplesPerPeak = 0;
    uint64_t decodedFrames = 0;
    uint32_t sampleRate = 0;
    bool ok = false;
    std::string errorCode;

    // 真实 RMS dBFS（全文件原始 PCM 累加，匹配 ffmpeg astats）
    float trueRmsDbfs = -100.0f;
    bool  hasTrueRms = false;
};

// @stage7-contract
// API contract: extraction returns owned value vectors and reports decode or
// cancellation failure through WaveformExtractResult. Callers provide valid
// UTF-8 paths and serialize one extractor instance; no backend handle escapes.
class WaveformExtractor {
public:
    static constexpr int kSchemaVersion = 1;
    // PRODUCTION: must be >= 4 — lowering disables decoded-frame timeline logic
    //             in detail buckets, causing truncated waveforms for BWF files
    static constexpr int kExtractorVersion = 4;

    // mini：快速 seek-based，一次成型
    static bool ExtractMini(const std::string& filePath,
                            int targetPeaks,
                            WaveformExtractResult& out,
                            bool perChannel = false,
                            const std::atomic<bool>* cancelFlag = nullptr);

    // Like ExtractMini, but for MediaFoundation formats also returns the fully-decoded
    // interleaved f32 buffer in outDecoded so callers can compute loudness without a
    // second decode. For non-MF formats outDecoded stays empty/zeroed.
    static bool ExtractMiniReturningDecoded(const std::string& filePath,
                                            int targetPeaks,
                                            WaveformExtractResult& out,
                                            bool perChannel,
                                            const std::atomic<bool>* cancelFlag,
                                            HarkenSearch::MediaFoundationAudio::DecodedAudioBuffer& outDecoded);

    // Interactive list preview: bounded decode for cold mini-wave cache misses.
    // Produces a quick approximate waveform for first paint; callers should
    // still run ExtractMini later to persist the exact full-file cache.
    static bool ExtractMiniPreview(const std::string& filePath,
                                   int targetPeaks,
                                   WaveformExtractResult& out,
                                   bool perChannel = false,
                                   double maxDecodeSeconds = 1.0);

    // detail：完整顺序解码，高精度主波形
    static bool ExtractDetail(const std::string& filePath,
                              int targetPeaks,
                              bool withRms,
                              WaveformExtractResult& out,
                              const std::atomic<bool>* cancelFlag = nullptr);

    static bool ExtractExactMonoEnvelope(const std::string& filePath,
                                         int targetPeaks,
                                         std::vector<float>& outPeaks);

    static bool BuildMiniFromDetail(const std::vector<float>& maxDetail,
                                    const std::vector<float>& minDetail,
                                    int detailChannels,
                                    int targetFrames,
                                    bool perChannel,
                                    std::vector<float>& outPeaks,
                                    int& outChannels);

    // Preserves the signed min and max sides separately for SM-style
    // filled-envelope mini rendering instead of folding to max-of-abs.
    static bool BuildMiniEnvelopeFromDetail(const std::vector<float>& maxDetail,
                                            const std::vector<float>& minDetail,
                                            int detailChannels,
                                            int targetFrames,
                                            bool perChannel,
                                            std::vector<float>& outMax,
                                            std::vector<float>& outMin,
                                            int& outChannels);

    static bool EncodePeakEnvelopeInt16(const std::vector<float>& peaks,
                                        const std::vector<float>& minPeaks,
                                        int channels,
                                        int bucketCount,
                                        std::vector<uint8_t>& outBlob);

    static bool DecodePeakEnvelopeInt16(const std::vector<uint8_t>& blob,
                                        std::vector<float>& outPeaks,
                                        std::vector<float>& outMinPeaks,
                                        int& outChannels,
                                        int& outBucketCount);

    static bool IsPeakEnvelopeInt16Blob(const std::vector<uint8_t>& blob);

    // 轻量全文件解码，仅计算 True RMS dBFS（匹配 ffmpeg astats）
    // 不生成波形数据，纯累加 sum-of-squares
    static float ComputeTrueRmsDbfs(const std::string& filePath);

    static std::string BuildCacheKey(const std::string& filePath,
                                     uint64_t fileSize,
                                     int64_t fileMtime,
                                     int schemaVersion = kSchemaVersion,
                                     int extractorVersion = kExtractorVersion);
};
