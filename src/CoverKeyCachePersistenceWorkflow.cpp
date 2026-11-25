#include "CoverKeyCachePersistenceWorkflow.h"

#include "CoverModule.h"
#include "Database.h"
#include "DiagnosticsLogger.h"
#include "sqlite3.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace aura::cover_cache {
namespace {

constexpr size_t kWriteBatchSize = 128;
constexpr auto kRetryDelay = std::chrono::milliseconds(100);
constexpr auto kFlushRetryBudget = std::chrono::seconds(5);

enum class StopMode { Running, Cancel, Flush };

class PersistenceWorker {
public:
    explicit PersistenceWorker(std::string databasePathUtf8)
        : databasePathUtf8_(std::move(databasePathUtf8)),
          thread_(&PersistenceWorker::Run, this) {}

    ~PersistenceWorker() {
        Stop(StopMode::Cancel);
    }

    bool Request() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            requested_ = true;
        }
        wake_.notify_one();
        return committed_.exchange(false);
    }

    void Stop(StopMode mode) {
        if (!thread_.joinable()) return;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopMode_.store(mode);
        }
        wake_.notify_one();
        thread_.join();
    }

private:
    bool WriteBatch(Database& database, const std::vector<CoverKeyCacheUpdate>& batch) {
        using Clock = std::chrono::steady_clock;
        auto flushDeadline = Clock::time_point::max();
        bool reportedFailure = false;
        while (stopMode_.load() != StopMode::Cancel) {
            if ((database.isOpen() || database.openForCoverKeyCachePersistence(databasePathUtf8_)) &&
                stopMode_.load() != StopMode::Cancel &&
                database.updateCoverKeyCacheBatch(batch)) {
                committed_.store(true);
                return true;
            }
            if (stopMode_.load() == StopMode::Cancel) return false;
            if (!reportedFailure) {
                DIAG_SYSTEM_FMT("Cover", "Cover key cache writeback retry: %s",
                                database.getLastError().c_str());
                reportedFailure = true;
            }
            database.close();

            std::unique_lock<std::mutex> lock(mutex_);
            if (stopMode_.load() == StopMode::Flush) {
                const auto now = Clock::now();
                if (flushDeadline == Clock::time_point::max()) {
                    flushDeadline = now + kFlushRetryBudget;
                } else if (now >= flushDeadline) {
                    lock.unlock();
                    DIAG_SYSTEM("Cover", "Cover key cache writeback did not finish before shutdown");
                    return false;
                }
            }
            wake_.wait_for(lock, kRetryDelay, [this] { return stopMode_.load() == StopMode::Cancel; });
        }
        return false;
    }

    void Run() {
        Database database;
        std::vector<CoverModule::CoverKeyCacheUpdate> updates;
        std::vector<CoverKeyCacheUpdate> batch;
        updates.reserve(kWriteBatchSize);
        batch.reserve(kWriteBatchSize);
        for (;;) {
            StopMode wakeMode;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                wake_.wait(lock, [this] { return requested_ || stopMode_.load() != StopMode::Running; });
                wakeMode = stopMode_.load();
                requested_ = false;
            }
            while (stopMode_.load() != StopMode::Cancel &&
                   CoverModule::DrainCoverKeyCacheUpdates(updates, kWriteBatchSize) != 0) {
                batch.clear();
                for (auto& update : updates) {
                    batch.push_back({
                        std::move(update.filePathUtf8),
                        update.updateEmbedded,
                        update.updateFolder,
                        std::move(update.embeddedCoverKey),
                        update.embeddedCoverKeyMtime,
                        std::move(update.folderCoverKey),
                        std::move(update.folderCoverSrcPathUtf8),
                        update.folderCoverSrcMtime,
                    });
                }
                if (!WriteBatch(database, batch)) return;
            }
            if (wakeMode != StopMode::Running) return;
        }
    }

    const std::string databasePathUtf8_;
    std::mutex mutex_;
    std::condition_variable wake_;
    bool requested_ = false;
    std::atomic<StopMode> stopMode_{StopMode::Running};
    std::atomic<bool> committed_{false};
    std::thread thread_;
};

std::unique_ptr<PersistenceWorker> g_worker;

}  // namespace

void BindPersistence(const Database& database) {
    StopPersistence();
    if (database.isOpen() && sqlite3_db_readonly(database.db(), "main") == 0 &&
        !database.getMainDbPath().empty()) {
        g_worker = std::make_unique<PersistenceWorker>(database.getMainDbPath());
    }
}

bool PumpPersistence() {
    return g_worker && g_worker->Request();
}

void StopPersistence() {
    g_worker.reset();
}

void FlushPersistence() {
    if (g_worker) g_worker->Stop(StopMode::Flush);
    g_worker.reset();
}

}  // namespace aura::cover_cache
