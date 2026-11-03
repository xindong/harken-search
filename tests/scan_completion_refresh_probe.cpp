// Deterministic probe for the real production refresh/commit function bodies.
// The Python test extracts them at run time; do not duplicate those algorithms.
#include "ScanWorkflow.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <iostream>
#include <string>
#include "ledger_counts.inc"

#define DIAG_SYSTEM_FMT(...) ((void)0)
#define DIAG_SYSTEM(...) ((void)0)
namespace ImGui { double GetTime() { return 1.0; } }

struct ScriptedLedger {
    ScanForegroundCompletionCounts before, after;
    int reads = 0;
    bool isOpen() const { return true; }
    ScanForegroundCompletionCounts getScanForegroundCompletionCounts(int64_t) {
        return reads++ == 0 ? before : after;
    }
};

struct AppState {
    ScriptedLedger database;
    struct { bool cacheWaveformsOnScan = true; } settings;
    std::atomic<bool> isScanning{false}, scanCompletionPending{true};
    std::atomic<bool> scanCompletionCoversPending{false}, scanCompletionWaveformsPending{true};
    std::atomic<int64_t> scanCompletionSessionId{1}, scanCompletionDurationMs{0};
    std::atomic<int64_t> scanStartedAtMs{1}, scanCompletionReadyAtMs{0}, scanFinishedAtMs{0};
    std::atomic<int> scanCompletionTotal{42681}, scanCompletionPendingFiles{1};
    std::atomic<int> scanCompletionMiniUnresolved{1}, scanCompletionCoverUnresolved{0};
    std::atomic<int> scanCompletionMetadataUnresolved{0}, scanTotal{42681};
    std::atomic<int> scanMiniWaveformsCompleted{42680}, scanMiniWaveformsTotal{42681};
    std::atomic<int> scanDiscoveredCount{42681}, scanProcessedCount{42681};
    std::atomic<int> scanIngestedCount{42537}, scanSkippedCount{144}, scanFailedFileCount{0};
    std::atomic<bool> scanMiniWaveformsRequired{true}, scanMiniWaveformsContinueInBackground{false};
    std::atomic<bool> scanForegroundTasksDrained{false}, scanFatalFailure{false};
    std::atomic<HarkenSearch::ScanWorkflowStage> scanWorkflowStage{HarkenSearch::ScanWorkflowStage::CachingMiniWaveforms};
    std::atomic<HarkenSearch::ScanStopDisposition> scanStopDisposition{HarkenSearch::ScanStopDisposition::None};
    double scanSummaryToastUntil = 0;
#include "app_members.inc"
};

#include "refresh_functions.inc"

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    const std::string scenario = argv[1];
    AppState state;
    auto& before = state.database.before;
    before.total = before.coverTerminal = before.metadataTerminal = 42681;
    before.miniTerminal = 42680;
    before.miniUnresolved = before.pendingFiles = 1;
    auto& after = state.database.after;
    after = before;
    after.miniTerminal = 42681;
    after.miniUnresolved = after.pendingFiles = 0;
    bool pending = false;
    int mini = 42681, total = 42681;
    if (scenario == "still-pending" || scenario == "progress-without-flag-change") {
        after = before;
        pending = true;
        mini = 42680;
        if (scenario == "progress-without-flag-change") {
            before.miniTerminal = 42673;
            before.miniUnresolved = before.pendingFiles = 8;
            state.scanCompletionMiniUnresolved = 8;
        }
    } else if (scenario == "cover-pending") {
        after.coverUnresolved = after.pendingFiles = 1;
        --after.coverTerminal;
        pending = true;
    } else if (scenario == "metadata-pending") {
        after.metadataUnresolved = after.pendingFiles = 1;
        --after.metadataTerminal;
        pending = true;
    } else if (scenario == "fast-mode") {
        state.settings.cacheWaveformsOnScan = false;
        state.scanMiniWaveformsRequired = false;
        state.scanMiniWaveformsContinueInBackground = true;
        // Keep the first refresh pending on a cover while mini is optional.
        before.coverUnresolved = 1;
        --before.coverTerminal;
        after.miniUnresolved = after.pendingFiles = 1;
        --after.miniTerminal;
        mini = total = 0;
    } else if (scenario == "pause") {
        state.scanStopDisposition = HarkenSearch::ScanStopDisposition::PauseKeepCommitted;
    } else if (scenario == "rollback") {
        state.scanStopDisposition = HarkenSearch::ScanStopDisposition::CancelRollback;
    } else if (scenario != "last-mini") {
        return 2;
    }

    int failures = 0;
    const auto check = [&](bool ok, const char* label) {
        std::cout << (ok ? "PASS " : "FAIL ") << scenario << ": " << label << '\n';
        if (!ok) ++failures;
    };
    check(before.pendingFiles >= std::max({before.miniUnresolved, before.coverUnresolved, before.metadataUnresolved}) &&
          after.pendingFiles >= std::max({after.miniUnresolved, after.coverUnresolved, after.metadataUnresolved}),
          "scripted ledger pending count includes each unresolved category");
    // These are the actual functions called in this order by a Windows frame.
    RefreshScanCompletionStateForQa(state);
    check(state.scanCompletionPending, "first read keeps the completion gate");
    check(!HarkenSearch::CanEnterCompleted(state.getScanWorkflowSnapshot()),
          "N-1 cannot complete");
    RefreshScanCompletionState(state);
    check(state.database.reads == 2, "both real refresh paths read the ledger");
    check(state.scanMiniWaveformsCompleted == mini && state.scanMiniWaveformsTotal == total,
          "second reader publishes its latest mini count");
    check(state.scanCompletionPending == pending, "completion gate reflects remaining required work");
    // Normal UI terminal resolution after ingestion, not a fabricated completion.
    state.scanForegroundTasksDrained = !state.scanCompletionPending;
    state.resolveScanWorkflowTerminalStage(false, false);
    auto expected = pending ? HarkenSearch::ScanWorkflowStage::Finalizing
                            : HarkenSearch::ScanWorkflowStage::Completed;
    if (scenario == "still-pending" || scenario == "progress-without-flag-change")
        expected = HarkenSearch::ScanWorkflowStage::CachingMiniWaveforms;
    if (scenario == "pause") expected = HarkenSearch::ScanWorkflowStage::Paused;
    if (scenario == "rollback") expected = HarkenSearch::ScanWorkflowStage::Cancelled;
    check(state.scanWorkflowStage == expected, "real workflow resolves to the expected stage");
    if (!pending) {
        RefreshScanCompletionStateForQa(state);
        RefreshScanCompletionState(state);
        check(state.database.reads == 2, "later frames stop refreshing without leaving stale counts");
    }
    return failures == 0 ? 0 : 1;
}
