#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace HarkenSearch {
struct LibraryStatisticsResult {
    bool success = false;
    int indexed = 0, total = 0;
    int64_t missingWaveforms = 0, pendingAssets = 0;
    double startedWallMs = 0, queryMilliseconds = 0;
    unsigned long threadId = 0;
};

// One worker owns its read-only SQLite connection. Public calls only exchange
// small values; no UI database or AppState reference crosses the thread boundary.
// SetDatabase/Cancel invalidate queued and running work, including same-path
// database reopen. The SQL progress/busy handlers observe cancellation at exit.
class LibraryStatisticsWorker final {
public:
    using CancelCheck = std::function<bool()>;
    using Handler = std::function<LibraryStatisticsResult(const std::string&, const CancelCheck&)>;
    explicit LibraryStatisticsWorker(Handler handler);
    ~LibraryStatisticsWorker();
    LibraryStatisticsWorker(const LibraryStatisticsWorker&) = delete;
    LibraryStatisticsWorker& operator=(const LibraryStatisticsWorker&) = delete;
    void SetDatabase(const std::string& path, int64_t generation);
    bool Request();
    std::optional<LibraryStatisticsResult> Poll();
    void Cancel();
    // Blocking maintenance-thread boundary: rejects new requests and waits for
    // the handler to return, after it has released its read-only connection.
    // Never call from a UI frame or from this worker's own handler.
    void CancelAndWait();
private:
    struct RequestValue { std::string path; uint64_t epoch; };
    void InvalidateLocked();
    void Run();
    Handler handler_;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::condition_variable idle_;
    std::string path_;
    int64_t databaseGeneration_ = -1;
    std::atomic<uint64_t> epoch_{1};
    std::atomic<bool> stopping_{false};
    std::optional<RequestValue> pending_;
    std::optional<LibraryStatisticsResult> completion_;
    uint64_t activeEpoch_ = 0;
    std::thread worker_;
};
LibraryStatisticsWorker::Handler MakeFeatureIndexCountHandler();
} // namespace HarkenSearch
