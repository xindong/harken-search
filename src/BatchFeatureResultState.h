#pragma once
#include <algorithm>
#include <array>
#include <mutex>
#include <string>

namespace HarkenSearch {
enum class BatchFeatureFailure { Unsupported, Unreadable, Empty, Save };
struct BatchFeatureResultSnapshot {
    bool available = false, running = false, stopped = false;
    int total = 0, succeeded = 0;
    std::array<int, 4> failures{};
    std::string currentFile;
    int Failed() const { return failures[0] + failures[1] + failures[2] + failures[3]; }
    int Processed() const { return succeeded + Failed(); }
    int Remaining() const { return std::max(0, total - Processed()); }
};
// Reports the outcomes of the existing workers; never owns audio or DB work.
class BatchFeatureResultState {
public:
    void Reset(int total) {
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot_ = {}; snapshot_.available = true; snapshot_.running = true;
        snapshot_.total = std::max(0, total);
    }
    void Clear() { std::lock_guard<std::mutex> lock(mutex_); snapshot_ = {}; }
    void SetTotal(int total) { std::lock_guard<std::mutex> lock(mutex_); snapshot_.total = std::max(0, total); }
    void SetCurrentFile(const std::string& file) { std::lock_guard<std::mutex> lock(mutex_); snapshot_.currentFile = file; }
    void RecordSuccess() { std::lock_guard<std::mutex> lock(mutex_); ++snapshot_.succeeded; }
    void RecordFailure(BatchFeatureFailure reason) {
        std::lock_guard<std::mutex> lock(mutex_); ++snapshot_.failures[static_cast<size_t>(reason)];
    }
    void Finish(bool stopped) {
        std::lock_guard<std::mutex> lock(mutex_); snapshot_.running = false; snapshot_.stopped = stopped;
    }
    BatchFeatureResultSnapshot Snapshot() const { std::lock_guard<std::mutex> lock(mutex_); return snapshot_; }
private:
    mutable std::mutex mutex_;
    BatchFeatureResultSnapshot snapshot_;
};
} // namespace HarkenSearch
