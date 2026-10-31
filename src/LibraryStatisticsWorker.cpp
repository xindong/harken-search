#include "LibraryStatisticsWorker.h"
#include "platform/thread_id.h"
#include "sqlite3.h"
#include <chrono>
#include <utility>

namespace HarkenSearch {
namespace {
struct ReadOnlyCounter {
    sqlite3* db = nullptr;
    ~ReadOnlyCounter() { if (db) sqlite3_close(db); }
    LibraryStatisticsResult Count(const std::string& requestedPath,
                             const LibraryStatisticsWorker::CancelCheck& cancel) {
        LibraryStatisticsResult result;
        // Open for each request so reopening/replacing the same path cannot keep
        // an old file handle. No sidecars, migrations, schema writes, or UI lock.
        if (db) { sqlite3_close(db); db = nullptr; }
        if (cancel() || sqlite3_open_v2(requestedPath.c_str(), &db,
                SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX, nullptr) != SQLITE_OK) return result;
        auto* check = const_cast<LibraryStatisticsWorker::CancelCheck*>(&cancel);
        sqlite3_progress_handler(db, 1000, [](void* p) {
            return (*static_cast<LibraryStatisticsWorker::CancelCheck*>(p))() ? 1 : 0;
        }, check);
        sqlite3_busy_handler(db, [](void* p, int attempts) {
            if (attempts >= 50 || (*static_cast<LibraryStatisticsWorker::CancelCheck*>(p))()) return 0;
            sqlite3_sleep(10);
            return 1;
        }, check);
        sqlite3_exec(db, "PRAGMA query_only=ON; PRAGMA mmap_size=536870912; PRAGMA cache_size=-8000;", nullptr, nullptr, nullptr);
        // Same population and feature-version rule as Database::tryGetFeatureIndexCounts.
        const char* sql = "SELECT COUNT(*), COALESCE(SUM(CASE WHEN audio_features IS NOT NULL "
                          "AND features_version >= 1 THEN 1 ELSE 0 END),0) FROM audio_files;";
        sqlite3_stmt* statement = nullptr;
        if (!cancel() && sqlite3_prepare_v2(db, sql, -1, &statement, nullptr) == SQLITE_OK &&
            sqlite3_step(statement) == SQLITE_ROW) {
            result.total = sqlite3_column_int(statement, 0);
            result.indexed = sqlite3_column_int(statement, 1);
            result.success = sqlite3_step(statement) == SQLITE_DONE && !cancel();
        }
        if (statement) sqlite3_finalize(statement);
        sqlite3_progress_handler(db, 0, nullptr, nullptr);
        sqlite3_busy_handler(db, nullptr, nullptr);
        sqlite3_close(db); db = nullptr;
        return result;
    }
};
}
LibraryStatisticsWorker::Handler MakeFeatureIndexCountHandler() {
    return [](const std::string& path, const auto& cancel) {
        ReadOnlyCounter counter;
        return counter.Count(path, cancel);
    };
}
LibraryStatisticsWorker::LibraryStatisticsWorker(Handler handler)
    : handler_(std::move(handler)), worker_([this] { Run(); }) {}
LibraryStatisticsWorker::~LibraryStatisticsWorker() {
    { std::lock_guard<std::mutex> lock(mutex_); stopping_ = true; InvalidateLocked(); }
    wake_.notify_one();
    if (worker_.joinable()) worker_.join();
}
void LibraryStatisticsWorker::InvalidateLocked() { ++epoch_; pending_.reset(); completion_.reset(); }
void LibraryStatisticsWorker::SetDatabase(const std::string& path, int64_t generation) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (path_ == path && databaseGeneration_ == generation) return;
    InvalidateLocked(); path_ = path; databaseGeneration_ = generation;
}
void LibraryStatisticsWorker::Cancel() {
    std::lock_guard<std::mutex> lock(mutex_); InvalidateLocked();
}
void LibraryStatisticsWorker::CancelAndWait() {
    std::unique_lock<std::mutex> lock(mutex_);
    path_.clear(); databaseGeneration_ = -1;
    InvalidateLocked();
    idle_.wait(lock, [this] { return activeEpoch_ == 0; });
}
bool LibraryStatisticsWorker::Request() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_ || path_.empty() || pending_ || completion_ || activeEpoch_ == epoch_) return false;
    pending_ = RequestValue{path_, epoch_.load()};
    wake_.notify_one();
    return true;
}
std::optional<LibraryStatisticsResult> LibraryStatisticsWorker::Poll() {
    std::lock_guard<std::mutex> lock(mutex_);
    auto result = std::move(completion_); completion_.reset(); return result;
}
void LibraryStatisticsWorker::Run() {
    for (;;) {
        RequestValue request;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            wake_.wait(lock, [this] { return stopping_ || pending_.has_value(); });
            if (stopping_) return;
            request = std::move(*pending_); pending_.reset(); activeEpoch_ = request.epoch;
        }
        const auto start = std::chrono::steady_clock::now();
        const double wall = std::chrono::duration<double, std::milli>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        const CancelCheck cancel = [this, epoch = request.epoch] { return stopping_ || epoch_ != epoch; };
        LibraryStatisticsResult result;
        try { result = handler_(request.path, cancel); } catch (...) { result.success = false; }
        result.startedWallMs = wall;
        result.queryMilliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now()-start).count();
        result.threadId = Platform::CurrentThreadId();
        {
            std::lock_guard<std::mutex> lock(mutex_); activeEpoch_ = 0;
            if (!cancel()) completion_ = result;
        }
        idle_.notify_all();
    }
}
} // namespace HarkenSearch
