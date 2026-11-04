#pragma once
#include "FeatureExtractor.h"
#include <array>
#include <memory>
#include <string>
#include <vector>

namespace HarkenSearch {
using AudioEmbedding = std::array<float, 512>;
inline constexpr const char* kSimilarityModelRevision = "clap-htsat-unfused:8fa0f1c6d0433df6e97c127f64b2a1d6c0dcda8a:repeatpad-slaney-peak095-stream-v2";
struct AudioEmbeddingSegment {
    float startSeconds = 0, endSeconds = 0;
    AudioEmbedding embedding{};
};
struct AudioEmbeddingResult {
    bool ok = false;
    AudioEmbedding pooled{};
    std::vector<AudioEmbeddingSegment> segments;
    std::string error;
};
// One background owner. No global model state, task state, or Python runtime.
class AudioEmbeddingService final {
public:
    AudioEmbeddingService();
    ~AudioEmbeddingService();
    AudioEmbeddingService(const AudioEmbeddingService&) = delete;
    AudioEmbeddingService& operator=(const AudioEmbeddingService&) = delete;
    bool Load(const std::string& directory, std::string& error);
    bool IsReady() const;
    bool MakeMel(const float* pcm, size_t samples, std::vector<float>& mel, std::string& error,
                 const FeatureExtractionCancelCheck& cancel = {}) const;
    bool EncodeWindow(const float* pcm, size_t samples, AudioEmbedding& result, std::string& error,
                      const FeatureExtractionCancelCheck& cancel = {});
    AudioEmbeddingResult EncodeFile(const std::string& path, float activeStart, float activeSeconds,
                                    const FeatureExtractionCancelCheck& cancel = {});
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
float AudioEmbeddingCosine(const AudioEmbedding& a, const AudioEmbedding& b);
bool IsValidAudioEmbedding(const AudioEmbedding& value);
} // namespace HarkenSearch
