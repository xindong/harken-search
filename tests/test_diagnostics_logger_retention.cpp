#include "DiagnosticsLogger.h"

#include <chrono>
#include <iostream>
#include <string>
#include <thread>

static int g_failed = 0;

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
    return g_failed == 0 ? 0 : 1;
}
