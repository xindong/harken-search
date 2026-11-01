#include "SimilarityWorkflow.h"

#include <utility>

namespace HarkenSearch {

bool SimilarityReference::operator==(const SimilarityReference& rhs) const {
    return fileId == rhs.fileId && filePath == rhs.filePath &&
        displayName == rhs.displayName;
}

SimilarityWorkflow::SimilarityWorkflow(Handler handler)
    : handler_(std::move(handler)), worker_([this]() { Run(); }) {}

SimilarityWorkflow::~SimilarityWorkflow() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_.store(true, std::memory_order_release);
        pending_.reset();
        completion_.reset();
    }
    wake_.notify_one();
    if (worker_.joinable()) worker_.join();
}

void SimilarityWorkflow::InvalidateLocked() {
    latestGeneration_.fetch_add(1, std::memory_order_acq_rel);
    pending_.reset();
    completion_.reset();
    snapshot_.rows.clear();
    historyToken_.clear(); shownSounds_.clear(); minimumScore_ = 0.0f;
    snapshot_.batchNumber = 0; snapshot_.stats = {};
    snapshot_.indexed = snapshot_.totalFiles = snapshot_.staleFiles = 0;
    snapshot_.error = SimilarityWorkflowError::None;
    if (reference_) {
        snapshot_.reference = *reference_;
        snapshot_.status = SimilarityWorkflowStatus::Ready;
    } else {
        snapshot_.reference = {};
        snapshot_.status = SimilarityWorkflowStatus::NoSelection;
    }
}

void SimilarityWorkflow::SetDatabasePath(std::string path) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (databasePath_ == path) return;
    databasePath_ = std::move(path);
    InvalidateLocked();
}

void SimilarityWorkflow::SetReference(
    std::optional<SimilarityReference> reference) {
    std::lock_guard<std::mutex> lock(mutex_);
    const bool unchanged = reference_.has_value() == reference.has_value() &&
        (!reference || *reference_ == *reference);
    if (unchanged) return;

    reference_ = std::move(reference);
    InvalidateLocked();
}

bool SimilarityWorkflow::RequestSearch(SimilarityPreset preset, int topK) {
    SimilarityOptions options; options.preset = preset;
    return RequestSearchImpl(options, topK, 1);
}
bool SimilarityWorkflow::RequestSearch(const SimilarityOptions& options, int topK) {
    return RequestSearchImpl(options, topK, SimilarityOptions::VERSION);
}
bool SimilarityWorkflow::RequestSearchImpl(const SimilarityOptions& options, int topK, int version) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_.load(std::memory_order_acquire) || !reference_) {
            return false;
        }

        SimilarityRequest request;
        if (version == SimilarityOptions::VERSION &&
            (snapshot_.options.mode != options.mode || snapshot_.options.deep != options.deep || snapshot_.options.preset != options.preset)) {
            historyToken_.clear(); shownSounds_.clear(); minimumScore_ = 0.0f; snapshot_.batchNumber = 0;
        }
        request.generation =
            latestGeneration_.fetch_add(1, std::memory_order_acq_rel) + 1;
        request.databasePath = databasePath_;
        request.reference = *reference_;
        request.preset = options.preset;
        request.options = options;
        request.analysisVersion = version;
        request.topK = topK;
        if (version == SimilarityOptions::VERSION) {
            request.historyToken = historyToken_; request.shownSounds = shownSounds_; request.minimumScore = minimumScore_;
        }
        pending_ = std::move(request);
        completion_.reset();
        snapshot_.status = SimilarityWorkflowStatus::Searching;
        snapshot_.options = options;
        snapshot_.indexed = snapshot_.totalFiles = snapshot_.staleFiles = 0;
        snapshot_.reference = *reference_;
        snapshot_.rows.clear();
        snapshot_.error = SimilarityWorkflowError::None;
    }
    wake_.notify_one();
    return true;
}

void SimilarityWorkflow::Poll() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!completion_) return;

    SimilarityCompletion completion = std::move(*completion_);
    completion_.reset();
    if (!reference_ ||
        completion.generation !=
            latestGeneration_.load(std::memory_order_acquire) ||
        !(completion.reference == *reference_)) {
        return;
    }

    snapshot_.reference = *reference_;
    snapshot_.rows = std::move(completion.rows);
    snapshot_.error = completion.error;
    snapshot_.options = completion.options;
    snapshot_.indexed = completion.indexed;
    snapshot_.totalFiles = completion.totalFiles;
    snapshot_.staleFiles = completion.staleFiles;
    snapshot_.stats = completion.stats;
    if (completion.error == SimilarityWorkflowError::None && !completion.historyToken.empty()) {
        if (historyToken_ != completion.historyToken) { shownSounds_.clear(); snapshot_.batchNumber = 0; }
        historyToken_ = std::move(completion.historyToken); minimumScore_ = completion.minimumScore;
        for (const auto& row : snapshot_.rows) shownSounds_.push_back({row.fileId, row.filePath, row.contentFingerprint});
        if (!snapshot_.rows.empty()) ++snapshot_.batchNumber;
    }
    snapshot_.status = completion.error == SimilarityWorkflowError::None
        ? SimilarityWorkflowStatus::ResultsReady
        : SimilarityWorkflowStatus::Error;
}

void SimilarityWorkflow::Invalidate() {
    std::lock_guard<std::mutex> lock(mutex_);
    InvalidateLocked();
}

void SimilarityWorkflow::CancelSearch() {
    std::lock_guard<std::mutex> lock(mutex_);
    latestGeneration_.fetch_add(1, std::memory_order_acq_rel);
    pending_.reset(); completion_.reset(); snapshot_.rows.clear(); snapshot_.error = SimilarityWorkflowError::None;
    snapshot_.status = reference_ ? SimilarityWorkflowStatus::Ready : SimilarityWorkflowStatus::NoSelection;
}

SimilarityWorkflowSnapshot SimilarityWorkflow::Snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return snapshot_;
}

void SimilarityWorkflow::Run() {
    for (;;) {
        std::optional<SimilarityRequest> request;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            wake_.wait(lock, [this]() {
                return stopping_.load(std::memory_order_acquire) || pending_;
            });
            if (stopping_.load(std::memory_order_acquire)) return;
            request = std::move(pending_);
            pending_.reset();
        }

        const uint64_t requestGeneration = request->generation;
        CancelCheck cancelled = [this, requestGeneration]() {
            return stopping_.load(std::memory_order_acquire) ||
                latestGeneration_.load(std::memory_order_acquire) !=
                    requestGeneration;
        };

        SimilarityCompletion completion;
        try {
            completion = handler_(*request, cancelled);
        } catch (...) {
            completion.generation = request->generation;
            completion.reference = request->reference;
            completion.options = request->options;
            completion.error = SimilarityWorkflowError::SearchFailed;
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stopping_.load(std::memory_order_acquire)) return;
            completion_ = std::move(completion);
        }
    }
}

} // namespace HarkenSearch
