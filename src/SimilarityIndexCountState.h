#pragma once

// The first count is unknown, not an empty database. Keep this state tied to
// the current database generation and never enable analysis on stale counts.
class SimilarityIndexCountState final {
public:
    enum class Phase { Loading, Ready, Failed };

    void Reset() noexcept { Retry(); }
    void Retry() noexcept {
        phase_ = Phase::Loading;
        indexed_ = semanticIndexed_ = total_ = pending_ = 0;
    }
    void Complete(bool success, int indexed, int semanticIndexed, int total, int pending) noexcept {
        if (!success || indexed < 0 || semanticIndexed < 0 || total < 0 || pending < 0 ||
            indexed > total || semanticIndexed > total || pending > total) {
            phase_ = Phase::Failed;
            return;
        }
        indexed_ = indexed;
        semanticIndexed_ = semanticIndexed;
        total_ = total;
        pending_ = pending;
        phase_ = Phase::Ready;
    }
    Phase phase() const noexcept { return phase_; }
    int indexed() const noexcept { return indexed_; }
    int semanticIndexed() const noexcept { return semanticIndexed_; }
    int total() const noexcept { return total_; }
    int pending() const noexcept { return pending_; }
    bool CanStart(bool switching, bool databaseOpen) const noexcept {
        return phase_ == Phase::Ready && total_ > 0 && !switching && databaseOpen;
    }

private:
    Phase phase_ = Phase::Loading;
    int indexed_ = 0;
    int semanticIndexed_ = 0;
    int total_ = 0;
    int pending_ = 0;
};
