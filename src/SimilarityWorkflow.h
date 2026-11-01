#pragma once

#include "SimilaritySearch.h"
#include "AcousticSimilarity.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace HarkenSearch {

// @stage7-contract
// API contract: value types below own their strings and result rows. The
// workflow owns one worker thread, invokes its Handler serially there, and joins
// that thread during destruction. Public state methods are synchronized;
// changing the database or reference invalidates queued and stale generations.
// RequestSearch returns false only when no reference is available or shutdown
// has begun. Handler exceptions and domain failures become snapshot error state,
// stale completions are discarded, and Snapshot returns an owned copy. A
// current handler-reported cancellation is published as Error.
enum class SimilarityWorkflowStatus {
    NoSelection,
    Ready,
    Searching,
    ResultsReady,
    Error
};

enum class SimilarityWorkflowError {
    None,
    DatabaseUnavailable,
    ReferenceUnreadable,
    NoCandidates,
    Cancelled,
    SearchFailed,
    ModelUnavailable,
    IndexRequired,
    NoMoreCandidates
};

struct SimilarityReference {
    int64_t fileId = -1;
    std::string filePath;
    std::string displayName;

    bool operator==(const SimilarityReference& rhs) const;
};

struct SimilarityDisplayRow {
    int64_t fileId = -1;
    std::string filePath;
    std::string fileName;
    float durationSeconds = 0.0f;
    std::string format;
    float similarity = 0.0f;
    SimilarityScore breakdown;
    std::string contentFingerprint;
};

struct SimilarityShownSound {
    int64_t fileId = -1;
    std::string filePath, contentFingerprint;
};
struct SimilarityQueryStats {
    uint64_t scannedFiles = 0, visitedSegments = 0, candidates = 0, detailedComparisons = 0;
};

struct SimilarityRequest {
    uint64_t generation = 0;
    std::string databasePath;
    SimilarityReference reference;
    SimilarityPreset preset = SimilarityPreset::Balanced;
    int topK = 50;
    SimilarityOptions options;
    int analysisVersion = 1;
    // History is copied only from results the workflow actually displayed.
    std::string historyToken;
    std::vector<SimilarityShownSound> shownSounds;
    float minimumScore = 0.0f;
};

struct SimilarityCompletion {
    uint64_t generation = 0;
    SimilarityReference reference;
    std::vector<SimilarityDisplayRow> rows;
    SimilarityWorkflowError error = SimilarityWorkflowError::None;
    float queryMilliseconds = 0.0f;
    int64_t indexed = 0, totalFiles = 0, staleFiles = 0;
    SimilarityOptions options;
    std::string historyToken;
    float minimumScore = 0.0f;
    SimilarityQueryStats stats;
};

struct SimilarityWorkflowSnapshot {
    SimilarityWorkflowStatus status = SimilarityWorkflowStatus::NoSelection;
    SimilarityReference reference;
    std::vector<SimilarityDisplayRow> rows;
    SimilarityWorkflowError error = SimilarityWorkflowError::None;
    int64_t indexed = 0, totalFiles = 0, staleFiles = 0;
    SimilarityOptions options;
    unsigned batchNumber = 0;
    SimilarityQueryStats stats;
};

class SimilarityWorkflow final {
public:
    using CancelCheck = std::function<bool()>;
    using Handler = std::function<SimilarityCompletion(
        const SimilarityRequest&, const CancelCheck&)>;

    explicit SimilarityWorkflow(Handler handler);
    ~SimilarityWorkflow();

    SimilarityWorkflow(const SimilarityWorkflow&) = delete;
    SimilarityWorkflow& operator=(const SimilarityWorkflow&) = delete;

    void SetDatabasePath(std::string path);
    void SetReference(std::optional<SimilarityReference> reference);
    bool RequestSearch(SimilarityPreset preset, int topK = 50);
    bool RequestSearch(const SimilarityOptions& options, int topK = 50);
    void Poll();
    void Invalidate();
    void CancelSearch();
    SimilarityWorkflowSnapshot Snapshot() const;

private:
    bool RequestSearchImpl(const SimilarityOptions& options, int topK, int analysisVersion);
    void InvalidateLocked();
    void Run();

    Handler handler_;
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::optional<SimilarityRequest> pending_;
    std::optional<SimilarityCompletion> completion_;
    std::atomic<bool> stopping_{false};
    std::atomic<uint64_t> latestGeneration_{0};
    std::string databasePath_;
    std::optional<SimilarityReference> reference_;
    SimilarityWorkflowSnapshot snapshot_;
    std::string historyToken_;
    std::vector<SimilarityShownSound> shownSounds_;
    float minimumScore_ = 0.0f;
    std::thread worker_;
};

} // namespace HarkenSearch
