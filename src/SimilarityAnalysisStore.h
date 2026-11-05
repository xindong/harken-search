#pragma once
#include "AcousticSimilarity.h"
#include "AudioEmbeddingService.h"
#include <cstdint>
#include <string>
#include <vector>
struct sqlite3;
namespace HarkenSearch {
struct SimilarityFile {
    int64_t id = -1;
    std::string path, name, format;
    float seconds = 0;
};
struct SimilarityFileIdentity {
    uint64_t bytes = 0;
    int64_t modified = 0;
    std::string sha256;
    bool operator==(const SimilarityFileIdentity& rhs) const;
};
struct SimilarityAnalysisEntry {
    SimilarityFile file;
    SimilarityFileIdentity identity;
    AcousticAnalysis acoustic;
    AudioEmbedding embedding{};
    std::vector<AudioEmbeddingSegment> segments;
    bool hasSemantic = false;
};
struct SimilarityCoverage { int64_t total = 0, acoustic = 0, semantic = 0; };
// One worker owns this connection. V1 blobs and audio_files are never rewritten.
class SimilarityAnalysisStore final {
public:
    ~SimilarityAnalysisStore();
    bool Open(const std::string& databasePath, const FeatureExtractionCancelCheck& cancel = {});
    void Close();
    bool Files(std::vector<SimilarityFile>& files);
    // Select work from stored coverage without opening already indexed audio.
    bool PendingFiles(std::vector<SimilarityFile>& files, bool requireSemantic);
    bool GetFile(int64_t id, SimilarityFile& file);
    bool Publish(const SimilarityAnalysisEntry& entry);
    bool Load(int64_t id, SimilarityAnalysisEntry& entry, bool detailed = true);
    bool LoadSummaries(std::vector<SimilarityAnalysisEntry>& entries);
    int64_t SegmentCount();
    bool VisitSegments(const std::function<bool(int64_t, const AudioEmbeddingSegment&)>& visitor);
    bool Invalidate(int64_t id);
    // Removes only this database's V2 analysis and segment rows. Returns -1
    // on failure; a failed transaction leaves both tables unchanged.
    int64_t ClearAll();
    int64_t Revision();
    std::string LibraryIdentity();
    SimilarityCoverage Coverage();
    const std::string& Error() const { return error_; }
    SimilarityAnalysisStore() = default;
    SimilarityAnalysisStore(const SimilarityAnalysisStore&) = delete;
    SimilarityAnalysisStore& operator=(const SimilarityAnalysisStore&) = delete;
private:
    sqlite3* database_ = nullptr;
    FeatureExtractionCancelCheck cancel_;
    std::string error_;
};
bool ReadSimilarityFileIdentity(const std::string& path, SimilarityFileIdentity& value,
                                const FeatureExtractionCancelCheck& cancel = {});
} // namespace HarkenSearch
