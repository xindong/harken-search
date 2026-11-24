#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace aura {

class BackfillQueue;

// @stage7-contract
// API contract: BackfillWorkerPool owns worker threads and joins them during
// shutdown. Callers establish queue/database identity before starting work and
// submit file-ID jobs; callbacks/results do not borrow worker-owned storage.
class BackfillWorkerPool {
public:
    BackfillWorkerPool() = default;
    ~BackfillWorkerPool();

    BackfillWorkerPool(const BackfillWorkerPool&) = delete;
    BackfillWorkerPool& operator=(const BackfillWorkerPool&) = delete;

    void start(const std::string& dbPath, int64_t dbGeneration,
               BackfillQueue* queue, int threadCount = 1);
    void stop();
    bool running() const { return m_running.load(std::memory_order_acquire); }

private:
    void workerMain(int workerIndex);

    std::atomic<bool> m_running{false};
    std::atomic<bool> m_cancelRequested{false};
    std::string m_dbPath;
    int64_t m_dbGeneration = 0;
    BackfillQueue* m_queue = nullptr;
    std::vector<std::thread> m_threads;
    std::mutex m_folderCoverLookupMutex;
    std::unordered_map<std::string, std::string> m_folderCoverLookupCache;
};

BackfillWorkerPool& GlobalBackfillWorkerPool();
bool ConsumeCoverBackfillUiRefresh(int64_t dbGeneration);

class BackfillScanWriteGuard {
public:
    BackfillScanWriteGuard();
    ~BackfillScanWriteGuard();

    BackfillScanWriteGuard(const BackfillScanWriteGuard&) = delete;
    BackfillScanWriteGuard& operator=(const BackfillScanWriteGuard&) = delete;

private:
    std::unique_lock<std::mutex> m_lock;
};

bool TryAcquireBackfillWorkerWriteGate(std::unique_lock<std::mutex>& lock);

}
