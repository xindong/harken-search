#include "DiagnosticsLogger.h"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <new>
#include <string>
#include <thread>

static int g_failed = 0;
static bool g_countAllocations = false;
static size_t g_allocations = 0;

void* operator new(size_t size) {
    if (g_countAllocations) ++g_allocations;
    if (void* p = std::malloc(size ? size : 1)) return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, size_t) noexcept { std::free(p); }

// The writer is stopped for these deterministic retention fixtures. Friendship
// keeps the production/private ABI intact (unlike redefining private as public).
struct DiagnosticsRetentionTestAccess {
    static void seed(DiagnosticsLogger& log, size_t capacity, size_t count,
                     size_t head, int64_t firstTimestamp) {
        log.clear();
        std::lock_guard<std::mutex> lock(log.m_mutex);
        log.m_ring.assign(capacity, {});
        log.m_maxEntries = capacity;
        log.m_count = count;
        log.m_head = head;
        log.m_oldestTimestampMs = firstTimestamp;
        const size_t start = (head + capacity - count) % capacity;
        for (size_t i = 0; i < count; ++i) {
            auto& entry = log.m_ring[(start + i) % capacity];
            entry.type = DiagLogType::System;
            entry.timestampMs = firstTimestamp + static_cast<int64_t>(i);
            std::snprintf(entry.detail, sizeof(entry.detail), "seed-%zu-end", i);
        }
    }
    static void prune(DiagnosticsLogger& log, int64_t timestamp) {
        std::lock_guard<std::mutex> lock(log.m_mutex);
        log.pruneExpiredLocked(timestamp);
    }
    static int64_t physicalTimestamp(const DiagnosticsLogger& log, size_t slot) {
        return log.m_ring[slot].timestampMs;
    }
};

#define ASSERT(cond, msg) do { \
    if (!(cond)) { std::cerr << "FAIL: " << (msg) << "\n"; ++g_failed; } \
    else { std::cout << "PASS: " << (msg) << "\n"; } \
} while (0)

static int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

template <typename Predicate>
static bool waitUntil(Predicate ready, int timeoutMs = 3000) {
    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::milliseconds(timeoutMs);
    do {
        if (ready()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    } while (std::chrono::steady_clock::now() < deadline);
    return ready();
}

int main() {
    auto& log = DiagnosticsLogger::instance();
    log.setEnabled(true);
    log.clear();

    const int64_t now = nowMs();
    ASSERT(log.logSystemAt("Scan", "old-open-failed", now - 61000),
           "old diagnostic entry is accepted");
    ASSERT(log.logSystemAt("Scan", "recent-open-failed", now - 1000),
           "recent diagnostic entry is accepted");
    ASSERT(waitUntil([&] {
        return log.exportJsonString().find("recent-open-failed") != std::string::npos;
    }), "writer drains the queued entries");

    const std::string exported = log.exportJsonString();
    ASSERT(exported.find("old-open-failed") == std::string::npos,
           "entry older than one minute is discarded");
    ASSERT(exported.find("recent-open-failed") != std::string::npos,
           "entry inside one minute is retained");

    log.setEnabled(false);
    log.clear();
    log.setMaxEntries(50);
    log.setEnabled(true);
    for (int i = 0; i <= 50; ++i) {
        const std::string detail = "capacity-entry-" + std::to_string(i) + "-end";
        log.logSystem("Retention", detail.c_str());
    }
    ASSERT(waitUntil([&] {
        return log.exportJsonString().find("capacity-entry-50-end") != std::string::npos;
    }), "full capacity retains the newest diagnostic entry");
    const std::string full = log.exportJsonString();
    ASSERT(full.find("capacity-entry-0-end") == std::string::npos,
           "full capacity evicts the oldest entry");
    ASSERT(log.entryCount() == 50, "retention stays within the memory limit");
    log.setEnabled(false);

    log.clear();
    log.setEnabled(true);
    const int64_t idleTimestamp = nowMs() - 58000;
    ASSERT(log.logSystemAt("Retention", "idle-entry", idleTimestamp),
           "near-expiry diagnostic entry is accepted");
    ASSERT(waitUntil([&] { return log.entryCount() == 1; }),
           "idle entry is present before expiry");
    ASSERT(waitUntil([&] { return nowMs() > idleTimestamp + 60000; }),
           "the retention window elapses without another write or export");
    ASSERT(log.entryCount() == 0, "idle entry count expires without a new write");
    ASSERT(log.exportJsonString().find("idle-entry") == std::string::npos,
           "idle export agrees with the visible count");
    log.setEnabled(false);
    log.clear();
    log.setEnabled(true);
    const int64_t unorderedNow = nowMs();
    log.logSystemAt("Retention", "recent-before-delayed-entry", unorderedNow);
    log.logSystemAt("Retention", "expired-delayed-entry", unorderedNow - 61000);
    log.logSystemAt("Retention", "unordered-drain-marker", unorderedNow + 1);
    ASSERT(waitUntil([&] {
        return log.exportJsonString().find("unordered-drain-marker") != std::string::npos;
    }), "writer drains entries whose capture times differ from enqueue order");
    ASSERT(log.entryCount() == 2, "count excludes expired entries behind a fresh entry");
    ASSERT(log.exportJsonString().find("expired-delayed-entry") == std::string::npos,
           "full export excludes out-of-order expired entries");
    ASSERT(log.getRecentJsonLines(50).find("expired-delayed-entry") == std::string::npos,
           "JSONL tail excludes out-of-order expired entries");
    ASSERT(log.getRecentFilteredLines(50).find("expired-delayed-entry") == std::string::npos,
           "filtered tail excludes out-of-order expired entries");
    log.setEnabled(false);
    log.clear();
    log.setEnabled(true);
    const int64_t fullTimestamp = nowMs();
    for (int i = 0; i < 50; ++i) {
        const std::string detail = "valid-at-capacity-" + std::to_string(i) + "-end";
        log.logSystemAt("Retention", detail.c_str(), fullTimestamp);
    }
    ASSERT(waitUntil([&] { return log.entryCount() == 50; }),
           "capacity fixture contains fifty current entries");
    const auto droppedBefore = log.droppedCount();
    log.logSystemAt("Retention", "expired-at-capacity", fullTimestamp - 61000);
    // Repeating the final current entry is a queue barrier without consuming
    // another ring slot or legitimately evicting one of the fifty entries.
    log.logSystemAt("Retention", "valid-at-capacity-49-end", fullTimestamp);
    ASSERT(waitUntil([&] {
        return log.getRecentJsonLines(1).find("\"repeat\":2") != std::string::npos;
    }), "writer processes the expired entry and the following folding barrier");
    ASSERT(log.entryCount() == 50, "expired incoming entry preserves all current entries at capacity");
    ASSERT(log.exportJsonString().find("valid-at-capacity-0-end") != std::string::npos,
           "expired incoming entry does not evict the oldest current entry");
    ASSERT(log.exportJsonString().find("expired-at-capacity") == std::string::npos,
           "expired incoming entry is absent from the exported window");
    ASSERT(log.droppedCount() == droppedBefore, "expired incoming entry is not counted as capacity loss");
    log.setEnabled(false);

    DiagnosticsRetentionTestAccess::seed(log, 50000, 30000, 30000, 1000000);
    g_allocations = 0;
    g_countAllocations = true;
    for (int i = 1; i <= 500; ++i)
        DiagnosticsRetentionTestAccess::prune(log, 1060000 + i);
    g_countAllocations = false;
    ASSERT(g_allocations == 0, "incremental expiry makes no allocations");
    ASSERT(DiagnosticsRetentionTestAccess::physicalTimestamp(log, 500) == 1000500,
           "ordered expiry does not move surviving entries");

    const int64_t wrapBase = nowMs() - 40000;
    DiagnosticsRetentionTestAccess::seed(log, 50, 50, 17, wrapBase);
    DiagnosticsRetentionTestAccess::prune(log, wrapBase + 60010);
    ASSERT(log.entryCount() == 40, "wrapped ring drops exactly the expired prefix");
    const auto wrappedFull = log.exportJsonString();
    const auto wrappedTail = log.getRecentJsonLines(50);
    const auto wrappedFiltered = log.getRecentFilteredLines(50);
    for (const auto* output : {&wrappedFull, &wrappedTail, &wrappedFiltered}) {
        ASSERT(output->find("seed-9-end") == std::string::npos &&
               output->find("seed-10-end") < output->find("seed-49-end"),
               "all exports preserve wrapped survivor order");
    }
    log.setMaxEntries(100);
    ASSERT(log.entryCount() == 40 && log.getRecentJsonLines(1).find("seed-49-end") != std::string::npos,
           "growing a partially expired ring preserves newest entry");
    DiagnosticsRetentionTestAccess::seed(log, 100, 80, 23, nowMs() - 1000);
    log.setMaxEntries(50);
    const auto resized = log.getRecentJsonLines(100);
    ASSERT(log.entryCount() == 50 && resized.find("seed-29-end") == std::string::npos &&
           resized.find("seed-30-end") < resized.find("seed-79-end"),
           "shrinking a wrapped ring retains the fifty newest entries in order");
    const int64_t mixedBase = nowMs();
    DiagnosticsRetentionTestAccess::seed(log, 50, 50, 17, mixedBase - 40000);
    log.setEnabled(true);
    log.logSystemAt("Retention", "delayed-to-expire", mixedBase - 59000);
    log.logSystemAt("Retention", "wrapped-writer-barrier", mixedBase);
    ASSERT(waitUntil([&] {
        return log.getRecentJsonLines(1).find("wrapped-writer-barrier") != std::string::npos;
    }), "writer appends out-of-order entries after ring wrap");
    log.setEnabled(false);
    g_allocations = 0;
    g_countAllocations = true;
    DiagnosticsRetentionTestAccess::prune(log, mixedBase + 2000);
    g_countAllocations = false;
    ASSERT(g_allocations == 0, "out-of-order expiry compacts without allocations");
    const auto compacted = log.getRecentJsonLines(100);
    ASSERT(log.entryCount() == 49 && compacted.find("delayed-to-expire") == std::string::npos &&
           compacted.find("seed-2-end") < compacted.find("seed-49-end") &&
           compacted.find("seed-49-end") < compacted.find("wrapped-writer-barrier"),
           "wrapped out-of-order expiry preserves all survivors and enqueue order");
    log.clear();
    return g_failed == 0 ? 0 : 1;
}
