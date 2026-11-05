// Runtime queue for durable cover, waveform, and metadata backfill jobs.
//
// Jobs are isolated by database path and generation, then keyed by file ID
// and kind. The queue owns deduplication, priority, retry state, recovery
// cursors, and shutdown behavior. Durable rows remain in backfill_pending;
// BackfillWorkerPool owns worker database connections.

#include "BackfillQueue.h"

#include "Database.h"

#include <algorithm>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <queue>
#include <unordered_map>
#include <vector>

namespace aura {

std::mutex& BackfillDatabaseMutationMutex() {
    static std::mutex mutex;
    return mutex;
}

namespace {

constexpr int kMaxTransientRetries = 3;
constexpr int kScanRuntimeQueueCapacity = 4096;

inline uint64_t makeKey(int64_t fileId, BackfillJobKind kind) {
    // file_id is an SQLite rowid (positive 64-bit). Reserve one byte for the
    // job kind so cover/waveform/metadata jobs never collide for one file.
    return (static_cast<uint64_t>(fileId) << 8) |
           (static_cast<uint64_t>(kind) & 0xFFULL);
}

inline int64_t nowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(
               steady_clock::now().time_since_epoch())
        .count();
}

inline BackfillPriority priorityForDurablePendingRow(int kind, int64_t scanSessionId) {
    return scanSessionId > 0 ||
           static_cast<BackfillJobKind>(kind) == BackfillJobKind::WaveformL1Overview
        ? BackfillPriority::RecentScan
        : BackfillPriority::IdleBackfill;
}

struct QueueItem {
    BackfillJob job;
    uint64_t    sequence;  // FIFO tiebreaker within the same priority
};

struct QueueItemCompare {
    // priority_queue is a max-heap; we want lowest priority enum (SelectedFile=0)
    // to come out first, and within the same priority, oldest sequence first.
    // So "left has lower precedence" returns true when left should NOT come
    // before right.
    bool operator()(const QueueItem& l, const QueueItem& r) const {
        if (l.job.priority != r.job.priority) {
            return static_cast<int>(l.job.priority) >
                   static_cast<int>(r.job.priority);
        }
        return l.sequence > r.sequence;
    }
};

struct EntryInfo {
    BackfillItemState state    = BackfillItemState::Unknown;
    BackfillPriority  priority = BackfillPriority::IdleBackfill;
    int               retries  = 0;  // count of TransientFailure observations
    int64_t           inFlightSinceMs = 0;
    int64_t           scanSessionId = 0;
};

}  // namespace

struct BackfillQueue::Impl {
    mutable std::mutex                                          mu;
    std::condition_variable                                     cv;
    std::priority_queue<QueueItem, std::vector<QueueItem>,
                        QueueItemCompare>                       pq;
    std::unordered_map<uint64_t, EntryInfo>                     index;

    uint64_t nextSequence = 1;

    bool        shutdownRequested = false;
    std::string dbPath;
    int64_t     dbGeneration = 0;

    // Snapshot counters
    int totalEnqueued     = 0;
    int done              = 0;
    int inFlight          = 0;
    int scanStaleRejected = 0;
    int byKind[kBackfillJobKindCount]        = {};
    int byPriority[kBackfillPriorityCount]   = {};
    int pendingTableRows  = -1;

    int64_t cursorEnqueuedAt = 0;
    int64_t cursorFileId = 0;
    int cursorKind = -1;
};

BackfillQueue::BackfillQueue() : m_impl(std::make_unique<Impl>()) {}
BackfillQueue::~BackfillQueue() = default;

// Runtime state is isolated by database identity.
//
// File IDs are local to each database. Opening or closing a database clears
// queue entries so identical row IDs cannot cross database boundaries.
// Durable backfill_pending rows remain with their database file.

void BackfillQueue::onDbOpened(std::string dbPath, int64_t newGeneration) {
    std::lock_guard<std::mutex> lk(m_impl->mu);
    // Clear previous database state before installing the new generation.
    // This also covers recovery paths that skipped onDbClosing().

    if (!m_impl->dbPath.empty() || m_impl->dbGeneration != 0) {
        std::priority_queue<QueueItem, std::vector<QueueItem>, QueueItemCompare>
            empty;
        m_impl->pq.swap(empty);
        m_impl->index.clear();
        for (int i = 0; i < kBackfillJobKindCount; ++i)     m_impl->byKind[i]     = 0;
        for (int i = 0; i < kBackfillPriorityCount; ++i)    m_impl->byPriority[i] = 0;
        m_impl->inFlight = 0;
    }
    m_impl->dbPath           = std::move(dbPath);
    m_impl->dbGeneration     = newGeneration;
    m_impl->pendingTableRows = -1;
    m_impl->cursorEnqueuedAt = 0;
    m_impl->cursorFileId     = 0;
    m_impl->cursorKind       = -1;
    // The caller rebuilds durable rows and owns the worker-pool connection.

}

void BackfillQueue::onDbClosing(int64_t /*closingGeneration*/) {
    // The caller stops and joins the worker pool before closing the database.
    // Clear every runtime entry associated with that database.

    std::lock_guard<std::mutex> lk(m_impl->mu);
    std::priority_queue<QueueItem, std::vector<QueueItem>, QueueItemCompare>
        empty;
    m_impl->pq.swap(empty);
    m_impl->index.clear();
    for (int i = 0; i < kBackfillJobKindCount; ++i)     m_impl->byKind[i]     = 0;
    for (int i = 0; i < kBackfillPriorityCount; ++i)    m_impl->byPriority[i] = 0;
    m_impl->inFlight        = 0;
    m_impl->pendingTableRows = -1;
    m_impl->cursorEnqueuedAt = 0;
    m_impl->cursorFileId     = 0;
    m_impl->cursorKind       = -1;
    // Clear the path sentinel so stale scan snapshots fail path and generation
    // validation until another database is opened.

    m_impl->dbPath.clear();
    // Retain the generation for diagnostics; an empty path rejects stale
    // snapshots because enqueueFromScan checks both values. Lifetime counters
    // remain intact across database switches.

}

void BackfillQueue::rebuildFromDatabase(Database* db, int windowSize) {
    if (!db || windowSize <= 0) return;
    const int boundedWindow = std::min(windowSize, kScanRuntimeQueueCapacity);
    const int pendingRows = db->countBackfillPending();
    const std::vector<BackfillPendingRow> rows = db->getBackfillPendingRows(boundedWindow);

    std::lock_guard<std::mutex> lk(m_impl->mu);
    if (m_impl->shutdownRequested) return;

    std::priority_queue<QueueItem, std::vector<QueueItem>, QueueItemCompare> empty;
    m_impl->pq.swap(empty);
    m_impl->index.clear();
    for (int i = 0; i < kBackfillJobKindCount; ++i)  m_impl->byKind[i] = 0;
    for (int i = 0; i < kBackfillPriorityCount; ++i) m_impl->byPriority[i] = 0;
    m_impl->inFlight = 0;
    m_impl->pendingTableRows = pendingRows;
    m_impl->cursorEnqueuedAt = 0;
    m_impl->cursorFileId = 0;
    m_impl->cursorKind = -1;

    for (const auto& row : rows) {
        if (row.fileId <= 0 || row.kind < 0 || row.kind >= kBackfillJobKindCount) continue;
        enqueueLocked(row.fileId,
                      static_cast<BackfillJobKind>(row.kind),
                      priorityForDurablePendingRow(row.kind, row.scanSessionId),
                      row.scanSessionId,
                      true);
        m_impl->cursorEnqueuedAt = row.enqueuedAt;
        m_impl->cursorFileId = row.fileId;
        m_impl->cursorKind = row.kind;
    }
}

void BackfillQueue::refillFromDatabaseIfBelowWaterMark(
    Database* db, int lowWaterMark, int refillBatch) {
    if (!db || lowWaterMark <= 0 || refillBatch <= 0) return;
    std::lock_guard<std::mutex> dbMutationLock(BackfillDatabaseMutationMutex());

    int queued = 0;
    int64_t cursorEnqueuedAt = 0;
    int64_t cursorFileId = 0;
    int cursorKind = -1;
    int refillLimit = 0;
    int knownActive = 0;
    {
        std::lock_guard<std::mutex> lk(m_impl->mu);
        if (m_impl->shutdownRequested) return;
        for (int i = 0; i < kBackfillJobKindCount; ++i) queued += m_impl->byKind[i];
        if (queued >= lowWaterMark) return;
        refillLimit = std::min(
            refillBatch,
            kScanRuntimeQueueCapacity - queued - m_impl->inFlight);
        if (refillLimit <= 0) return;
        knownActive = queued + m_impl->inFlight;
        cursorEnqueuedAt = m_impl->cursorEnqueuedAt;
        cursorFileId = m_impl->cursorFileId;
        cursorKind = m_impl->cursorKind;
    }

    std::vector<BackfillPendingRow> rows =
        db->getBackfillPendingRows(refillLimit, cursorEnqueuedAt, cursorFileId, cursorKind);
    const int pendingRows = db->countBackfillPending();
    if (rows.empty() && cursorEnqueuedAt > 0 && pendingRows > knownActive) {
        rows = db->getBackfillPendingRows(
            std::min(refillLimit, pendingRows - knownActive));
    }

    std::lock_guard<std::mutex> lk(m_impl->mu);
    if (m_impl->shutdownRequested) return;
    m_impl->pendingTableRows = pendingRows;
    for (const auto& row : rows) {
        if (row.fileId <= 0 || row.kind < 0 || row.kind >= kBackfillJobKindCount) continue;
        // The row list is a snapshot. A worker may have cleared a pending row
        // while this thread was reading it; never re-arm a terminal runtime
        // entry from that stale snapshot.
        if (!db->hasBackfillPending(row.fileId, row.kind)) continue;
        int activeJobs = m_impl->inFlight;
        for (int i = 0; i < kBackfillJobKindCount; ++i) activeJobs += m_impl->byKind[i];
        if (activeJobs >= kScanRuntimeQueueCapacity) break;
        enqueueLocked(row.fileId,
                      static_cast<BackfillJobKind>(row.kind),
                      priorityForDurablePendingRow(row.kind, row.scanSessionId),
                      row.scanSessionId,
                      true);
        m_impl->cursorEnqueuedAt = row.enqueuedAt;
        m_impl->cursorFileId = row.fileId;
        m_impl->cursorKind = row.kind;
    }
}

// Queue operations are synchronized by m_impl->mu.

// enqueueLocked: assumes caller holds m_impl->mu. The shutdown short-circuit
// is the caller's responsibility too — public entry points decide whether a
// shutdown observation should drop or proceed.
void BackfillQueue::enqueueLocked(int64_t          fileId,
                                  BackfillJobKind  kind,
                                  BackfillPriority priority,
                                  int64_t          scanSessionId,
                                  bool             durablePending) {
    const uint64_t key = makeKey(fileId, kind);
    auto it = m_impl->index.find(key);
    if (it != m_impl->index.end()) {
        const bool terminalRuntimeState =
            it->second.state == BackfillItemState::Cached ||
            it->second.state == BackfillItemState::FailedPermanent ||
            it->second.state == BackfillItemState::FailedTransient;
        const bool belongsToNewScanSession =
            scanSessionId > 0 && it->second.scanSessionId != scanSessionId;

        // A durable row is the source of truth. A previous in-memory terminal
        // state must not hide work that was persisted again, especially when
        // a later full scan revisits a file already cached in this process.
        if (terminalRuntimeState && (durablePending || belongsToNewScanSession)) {
            // done describes the current runtime terminal set. A durable
            // rearm removes the previous terminal entry before it is counted
            // again on the new completion.
            if (m_impl->done > 0) --m_impl->done;
            m_impl->index.erase(it);
            it = m_impl->index.end();
        }
    }
    if (it != m_impl->index.end()) {
        // A fast scan may leave jobs queued when the next full scan starts.
        // Deduplicate the work, but attach pending results to the new scan;
        // otherwise the worker clears the durable row while only completing
        // the old ledger, leaving the current scan waiting forever. Jobs
        // already in flight retain their existing nonzero session identity.
        // A durable refill must not override an explicit pending scan owner.
        if (scanSessionId > 0 &&
            (it->second.scanSessionId == 0 ||
             (!durablePending && it->second.state == BackfillItemState::Pending))) {
            it->second.scanSessionId = scanSessionId;
        }
        // Already known. Dedup + promote-if-higher policy:
        //   - InFlight — no re-queue.
        //   - Cached / Failed* — no re-queue unless a durable row or a new
        //     foreground scan session explicitly rearmed it above.
        //   - Pending @ equal-or-higher priority — no-op.
        //   - Pending @ lower priority — promote (push new pq entry, mark
        //     old one as stale via the index priority update).
        if (it->second.state != BackfillItemState::Pending) return;
        if (static_cast<int>(priority) >= static_cast<int>(it->second.priority)) return;

        // Promote. byPriority moves from old to new.
        --m_impl->byPriority[static_cast<int>(it->second.priority)];
        ++m_impl->byPriority[static_cast<int>(priority)];
        it->second.priority = priority;

        QueueItem promoted;
        promoted.job.fileId       = fileId;
        promoted.job.dbGeneration = m_impl->dbGeneration;
        promoted.job.enqueuedAt   = nowMs();
        promoted.job.scanSessionId = it->second.scanSessionId;
        promoted.job.kind         = kind;
        promoted.job.priority     = priority;
        std::memset(promoted.job._padding, 0, sizeof(promoted.job._padding));
        promoted.sequence         = m_impl->nextSequence++;
        m_impl->pq.push(promoted);
        m_impl->cv.notify_one();
        return;
    }

    // Fresh enqueue.
    EntryInfo info;
    info.state    = BackfillItemState::Pending;
    info.priority = priority;
    info.retries  = 0;
    info.scanSessionId = scanSessionId > 0 ? scanSessionId : 0;
    m_impl->index.emplace(key, info);

    QueueItem qi;
    qi.job.fileId       = fileId;
    qi.job.dbGeneration = m_impl->dbGeneration;
    qi.job.enqueuedAt   = nowMs();
    qi.job.scanSessionId = info.scanSessionId;
    qi.job.kind         = kind;
    qi.job.priority     = priority;
    std::memset(qi.job._padding, 0, sizeof(qi.job._padding));
    qi.sequence         = m_impl->nextSequence++;
    m_impl->pq.push(qi);

    ++m_impl->totalEnqueued;
    ++m_impl->byKind[static_cast<int>(kind)];
    ++m_impl->byPriority[static_cast<int>(priority)];
    m_impl->cv.notify_one();
}

void BackfillQueue::enqueue(int64_t          fileId,
                            BackfillJobKind  kind,
                            BackfillPriority priority) {
    std::lock_guard<std::mutex> lk(m_impl->mu);
    if (m_impl->shutdownRequested) return;
    enqueueLocked(fileId, kind, priority, 0);
}

// Validate the scan snapshot and stamp the job under the same lock so a
// database switch cannot enqueue an old file ID into the new queue.

void BackfillQueue::enqueueFromScan(int64_t            fileId,
                                    BackfillJobKind    kind,
                                    BackfillPriority   priority,
                                    const std::string& expectedDbPath,
                                    int64_t            expectedDbGeneration,
                                    int64_t            scanSessionId) {
    std::lock_guard<std::mutex> lk(m_impl->mu);
    if (m_impl->shutdownRequested) return;
    if (m_impl->dbGeneration != expectedDbGeneration ||
        m_impl->dbPath != expectedDbPath) {
        ++m_impl->scanStaleRejected;
        return;
    }
    const uint64_t key = makeKey(fileId, kind);
    if (m_impl->index.find(key) == m_impl->index.end()) {
        int activeJobs = m_impl->inFlight;
        for (int i = 0; i < kBackfillJobKindCount; ++i) {
            activeJobs += m_impl->byKind[i];
        }
        if (activeJobs >= kScanRuntimeQueueCapacity) {
            // The scan commits backfill_pending before calling this hot-cache
            // path. Leave overflow durable-only; worker low-water refill will
            // page it in after the bounded runtime window drains.
            return;
        }
    }
    enqueueLocked(fileId, kind, priority, scanSessionId);
}

bool BackfillQueue::dequeue(BackfillJob& out) {
    std::unique_lock<std::mutex> lk(m_impl->mu);

    for (;;) {
        // Observe shutdown before draining jobs so worker teardown does not
        // wait for the remaining backlog.

        if (m_impl->shutdownRequested) return false;

        // Skip stale heap entries (priority-promoted, demoted-to-done by an
        // earlier outcome report, or wiped by onDbClosing).
        while (!m_impl->pq.empty()) {
            const QueueItem& top = m_impl->pq.top();
            const uint64_t key = makeKey(top.job.fileId, top.job.kind);
            auto it = m_impl->index.find(key);
            const bool stale =
                it == m_impl->index.end() ||
                it->second.state != BackfillItemState::Pending ||
                it->second.priority != top.job.priority;
            if (stale) {
                m_impl->pq.pop();
                continue;
            }
            // Live entry. Pop + record InFlight + return.
            out = top.job;
            out.scanSessionId = it->second.scanSessionId;
            m_impl->pq.pop();
            it->second.state = BackfillItemState::InFlight;
            it->second.inFlightSinceMs = nowMs();
            --m_impl->byKind[static_cast<int>(out.kind)];
            --m_impl->byPriority[static_cast<int>(out.priority)];
            ++m_impl->inFlight;
            return true;
        }

        m_impl->cv.wait(lk);
    }
}

bool BackfillQueue::tryDequeue(BackfillJob& out) {
    std::lock_guard<std::mutex> lk(m_impl->mu);
    if (m_impl->shutdownRequested) return false;

    while (!m_impl->pq.empty()) {
        const QueueItem& top = m_impl->pq.top();
        const uint64_t key = makeKey(top.job.fileId, top.job.kind);
        auto it = m_impl->index.find(key);
        const bool stale =
            it == m_impl->index.end() ||
            it->second.state != BackfillItemState::Pending ||
            it->second.priority != top.job.priority;
        if (stale) {
            m_impl->pq.pop();
            continue;
        }

        out = top.job;
        out.scanSessionId = it->second.scanSessionId;
        m_impl->pq.pop();
        it->second.state = BackfillItemState::InFlight;
        it->second.inFlightSinceMs = nowMs();
        --m_impl->byKind[static_cast<int>(out.kind)];
        --m_impl->byPriority[static_cast<int>(out.priority)];
        ++m_impl->inFlight;
        return true;
    }

    return false;
}

bool BackfillQueue::tryDequeueKind(BackfillJobKind kind, BackfillJob& out) {
    std::lock_guard<std::mutex> lk(m_impl->mu);
    if (m_impl->shutdownRequested) return false;

    std::vector<QueueItem> deferred;
    while (!m_impl->pq.empty()) {
        QueueItem top = m_impl->pq.top();
        m_impl->pq.pop();
        const uint64_t key = makeKey(top.job.fileId, top.job.kind);
        auto it = m_impl->index.find(key);
        const bool stale =
            it == m_impl->index.end() ||
            it->second.state != BackfillItemState::Pending ||
            it->second.priority != top.job.priority;
        if (stale) {
            continue;
        }
        if (top.job.kind != kind) {
            deferred.push_back(top);
            continue;
        }

        out = top.job;
        out.scanSessionId = it->second.scanSessionId;
        it->second.state = BackfillItemState::InFlight;
        it->second.inFlightSinceMs = nowMs();
        --m_impl->byKind[static_cast<int>(out.kind)];
        --m_impl->byPriority[static_cast<int>(out.priority)];
        ++m_impl->inFlight;
        for (const auto& item : deferred) {
            m_impl->pq.push(item);
        }
        return true;
    }

    for (const auto& item : deferred) {
        m_impl->pq.push(item);
    }
    return false;
}

void BackfillQueue::promote(int64_t fileId, BackfillPriority newPriority) {
    std::lock_guard<std::mutex> lk(m_impl->mu);
    if (m_impl->shutdownRequested) return;

    // Promote applies to both kinds (caller doesn't specify). The plan's
    // public surface only ever promotes by fileId, so we lift the entry for
    // every kind that has a Pending record for this fileId.
    for (int kindIdx = 0; kindIdx < kBackfillJobKindCount; ++kindIdx) {
        const BackfillJobKind kind = static_cast<BackfillJobKind>(kindIdx);
        const uint64_t key         = makeKey(fileId, kind);
        auto it = m_impl->index.find(key);
        if (it == m_impl->index.end()) continue;
        if (it->second.state != BackfillItemState::Pending) continue;
        if (static_cast<int>(newPriority) >= static_cast<int>(it->second.priority)) continue;

        --m_impl->byPriority[static_cast<int>(it->second.priority)];
        ++m_impl->byPriority[static_cast<int>(newPriority)];
        it->second.priority = newPriority;

        QueueItem promoted;
        promoted.job.fileId       = fileId;
        promoted.job.dbGeneration = m_impl->dbGeneration;
        promoted.job.enqueuedAt   = nowMs();
        promoted.job.scanSessionId = it->second.scanSessionId;
        promoted.job.kind         = kind;
        promoted.job.priority     = newPriority;
        std::memset(promoted.job._padding, 0, sizeof(promoted.job._padding));
        promoted.sequence         = m_impl->nextSequence++;
        m_impl->pq.push(promoted);
    }
    m_impl->cv.notify_all();
}

void BackfillQueue::reportOutcome(const BackfillJob&        job,
                                  BackfillCompletionOutcome outcome) {
    std::lock_guard<std::mutex> lk(m_impl->mu);
    const uint64_t key = makeKey(job.fileId, job.kind);
    auto it = m_impl->index.find(key);
    if (it == m_impl->index.end()) return;

    // If the entry is not InFlight, the outcome doesn't apply (e.g. the
    // worker's job was already superseded). Defensive: ignore.
    if (it->second.state != BackfillItemState::InFlight) return;

    switch (outcome) {
        case BackfillCompletionOutcome::Success:
            it->second.state = BackfillItemState::Cached;
            it->second.inFlightSinceMs = 0;
            --m_impl->inFlight;
            ++m_impl->done;
            return;

        case BackfillCompletionOutcome::PermanentFailure:
            it->second.state = BackfillItemState::FailedPermanent;
            it->second.inFlightSinceMs = 0;
            --m_impl->inFlight;
            ++m_impl->done;
            return;

        case BackfillCompletionOutcome::TransientFailure:
            ++it->second.retries;
            if (it->second.retries >= kMaxTransientRetries) {
                // Auto-convert to permanent failure to bound the retry loop.
                it->second.state = BackfillItemState::FailedPermanent;
                it->second.inFlightSinceMs = 0;
                --m_impl->inFlight;
                ++m_impl->done;
                return;
            }
            // Requeue at RecentScan priority. The decrement-inFlight +
            // increment-byPriority dance mirrors the enqueue path.
            it->second.state    = BackfillItemState::Pending;
            it->second.priority = BackfillPriority::RecentScan;
            it->second.inFlightSinceMs = 0;
            --m_impl->inFlight;
            ++m_impl->byPriority[static_cast<int>(BackfillPriority::RecentScan)];
            ++m_impl->byKind[static_cast<int>(job.kind)];

            QueueItem retry;
            retry.job.fileId       = job.fileId;
            retry.job.dbGeneration = m_impl->dbGeneration;
            retry.job.enqueuedAt   = nowMs();
            retry.job.scanSessionId = job.scanSessionId;
            retry.job.kind         = job.kind;
            retry.job.priority     = BackfillPriority::RecentScan;
            std::memset(retry.job._padding, 0, sizeof(retry.job._padding));
            retry.sequence         = m_impl->nextSequence++;
            m_impl->pq.push(retry);
            m_impl->cv.notify_one();
            return;
    }
}

BackfillItemState BackfillQueue::itemState(int64_t         fileId,
                                            BackfillJobKind kind) const {
    std::lock_guard<std::mutex> lk(m_impl->mu);
    const uint64_t key = makeKey(fileId, kind);
    auto it = m_impl->index.find(key);
    if (it == m_impl->index.end()) return BackfillItemState::Unknown;
    return it->second.state;
}

std::vector<BackfillJob> BackfillQueue::markStaleInFlightPermanent(int64_t staleAfterMs) {
    std::vector<BackfillJob> staleJobs;
    if (staleAfterMs <= 0) return staleJobs;
    const int64_t cutoff = nowMs() - staleAfterMs;

    std::lock_guard<std::mutex> lk(m_impl->mu);
    for (auto& kv : m_impl->index) {
        EntryInfo& info = kv.second;
        if (info.state != BackfillItemState::InFlight ||
            info.inFlightSinceMs <= 0 ||
            info.inFlightSinceMs > cutoff) {
            continue;
        }

        info.state = BackfillItemState::FailedPermanent;
        info.inFlightSinceMs = 0;
        --m_impl->inFlight;
        ++m_impl->done;

        BackfillJob job{};
        job.fileId = static_cast<int64_t>(kv.first >> 8);
        job.kind = static_cast<BackfillJobKind>(kv.first & 0xFFULL);
        job.priority = info.priority;
        job.dbGeneration = m_impl->dbGeneration;
        job.enqueuedAt = nowMs();
        job.scanSessionId = info.scanSessionId;
        staleJobs.push_back(job);
    }
    return staleJobs;
}

BackfillQueue::Snapshot BackfillQueue::snapshot() const {
    std::lock_guard<std::mutex> lk(m_impl->mu);
    Snapshot s{};
    s.totalEnqueued     = m_impl->totalEnqueued;
    s.done              = m_impl->done;
    s.inFlight          = m_impl->inFlight;
    s.scanStaleRejected = m_impl->scanStaleRejected;
    s.pendingTableRows  = m_impl->pendingTableRows;
    for (int i = 0; i < kBackfillJobKindCount; ++i)     s.byKind[i]     = m_impl->byKind[i];
    for (int i = 0; i < kBackfillPriorityCount; ++i)    s.byPriority[i] = m_impl->byPriority[i];
    return s;
}

void BackfillQueue::shutdown() {
    {
        std::lock_guard<std::mutex> lk(m_impl->mu);
        m_impl->shutdownRequested = true;
    }
    m_impl->cv.notify_all();
}

int64_t BackfillQueue::currentGeneration() const {
    std::lock_guard<std::mutex> lk(m_impl->mu);
    return m_impl->dbGeneration;
}

std::string BackfillQueue::currentDbPath() const {
    std::lock_guard<std::mutex> lk(m_impl->mu);
    return m_impl->dbPath;
}

}
