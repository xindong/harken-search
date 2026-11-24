// Deterministic probe for the real production refresh/commit function bodies.
// The Python test extracts them at run time; do not duplicate those algorithms.
#include "ScanWorkflow.h"
#include "BackfillQueue.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <functional>
#include <iostream>
#include <optional>
#include <string>
#include "ledger_counts.inc"

#define DIAG_SYSTEM_FMT(...) ((void)0)
#define DIAG_SYSTEM(...) ((void)0)
namespace ImGui { double GetTime() { return 1.0; } }

namespace aura {
bool coverBackfillNotification = false;
bool ConsumeCoverBackfillUiRefresh(int64_t) {
    const bool updated = coverBackfillNotification;
    coverBackfillNotification = false;
    return updated;
}
}

struct ScriptedLedger {
    ScanForegroundCompletionCounts before, after;
    int reads = 0;
    std::optional<bool> pendingCovers{false};
    std::function<void()> beforeSecondRead;
    std::function<void()> onCoverPoll;
    bool isOpen() const { return true; }
    std::optional<bool> hasBackfillPendingKind(int) const {
        if (onCoverPoll) onCoverPoll();
        return pendingCovers;
    }
    ScanForegroundCompletionCounts getScanForegroundCompletionCounts(int64_t) {
        if (reads++ == 0) return before;
        if (beforeSecondRead) beforeSecondRead();
        return after;
    }
};

enum class ViewMode { Database };

struct AppState {
    ScriptedLedger database;
    static constexpr int PAGE_SIZE = 200;
    int loadedCount = 450;
    int coverWallRefreshes = 0;
    int resultRefreshes = 0;
    int resultLimit = 0;
    int64_t backfillDbGeneration = 1;
    bool needsDatabaseRefresh = false;
    bool dbSwitchInProgress = false;
    bool asyncSearchInProgress = false;
    bool loadingMore = false;
    ViewMode viewMode = ViewMode::Database;
    struct ScanCoverResultsRefreshState {
        uint64_t scanGeneration = 0;
        int64_t dbGeneration = 0;
        bool sawCoverWork = false;
        std::chrono::steady_clock::time_point nextPoll{};
    } scanCoverResultsRefresh;
    std::atomic<uint64_t> scanTaskGeneration{1};
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
    void requestCoverWallBackfillDisplayRefresh() { ++coverWallRefreshes; }
#include "app_members.inc"
};

static uint64_t RefreshSearchResultsWithRowLimit(
    AppState& state, bool resetPagination, int resultLimit, bool scanCoverRefresh) {
    if (resetPagination) state.loadedCount = 0;
    ++state.resultRefreshes;
    if (scanCoverRefresh) ++state.coverWallRefreshes;
    state.resultLimit = resultLimit;
    return state.resultRefreshes;
}

#include "refresh_functions.inc"

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    const std::string scenario = argv[1];
    AppState state;
    state.scanCoverResultsRefresh = {1, 1};
    auto& before = state.database.before;
    before.total = before.coverTerminal = before.metadataTerminal = 42681;
    before.miniTerminal = 42680;
    before.miniUnresolved = before.pendingFiles = 1;
    auto& after = state.database.after;
    after = before;
    after.miniTerminal = 42681;
    after.miniUnresolved = after.pendingFiles = 0;
    bool pending = false;
    bool expectCoverRefresh = false;
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
        expectCoverRefresh = true;
    } else if (scenario == "pause") {
        state.scanStopDisposition = HarkenSearch::ScanStopDisposition::PauseKeepCommitted;
        before.coverUnresolved = 1;
        --before.coverTerminal;
        expectCoverRefresh = true;
    } else if (scenario == "rollback") {
        state.scanStopDisposition = HarkenSearch::ScanStopDisposition::CancelRollback;
        before.coverUnresolved = 1;
        --before.coverTerminal;
    } else if (scenario == "covers-satisfied" || scenario == "superseded" ||
               scenario == "gate-moved") {
        before.coverUnresolved = 1;
        --before.coverTerminal;
        expectCoverRefresh = scenario == "covers-satisfied";
        if (scenario == "superseded") {
            state.database.beforeSecondRead = [&state] { state.scanTaskGeneration = 2; };
        } else if (scenario == "gate-moved") {
            state.database.beforeSecondRead = [&state] { state.scanCompletionSessionId = 2; };
        }
    } else if (scenario == "covers-satisfied-early") {
        state.scanCompletionCoversPending = true;
        expectCoverRefresh = true;
    } else if (scenario == "background-pause" || scenario == "background-other-db" ||
               scenario == "background-query-unavailable" ||
               scenario == "background-switching-db" || scenario == "background-late-notification") {
        state.scanStopDisposition = HarkenSearch::ScanStopDisposition::PauseKeepCommitted;
        expectCoverRefresh = scenario == "background-pause" || scenario == "background-late-notification";
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
    check(state.resultRefreshes == 0 && state.coverWallRefreshes == 0,
          "foreground state updates do not interrupt the current list");
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
    if (scenario == "pause" || scenario.rfind("background-", 0) == 0)
        expected = HarkenSearch::ScanWorkflowStage::Paused;
    if (scenario == "rollback") expected = HarkenSearch::ScanWorkflowStage::Cancelled;
    check(state.scanWorkflowStage == expected, "real workflow resolves to the expected stage");
    if (!pending) {
        RefreshScanCompletionStateForQa(state);
        RefreshScanCompletionState(state);
        check(state.database.reads == 2, "later frames stop refreshing without leaving stale counts");
    }
    if (scenario == "background-late-notification") {
        state.database.onCoverPoll = [] { aura::coverBackfillNotification = true; };
    } else if (scenario.rfind("background-", 0) == 0) {
        state.database.pendingCovers = true;
        PumpScanCoverResultsRefresh(state, true);
        check(state.resultRefreshes == 0, "don't query while durable cover work remains");
        state.scanCoverResultsRefresh.nextPoll = {};
        if (scenario == "background-other-db") ++state.backfillDbGeneration;
        if (scenario == "background-switching-db") {
            state.dbSwitchInProgress = true;
            state.database.onCoverPoll = [&] { check(false, "do not poll during library switch"); };
        }
        state.database.pendingCovers = scenario == "background-query-unavailable"
            ? std::nullopt : std::optional<bool>(false);
    }
    PumpScanCoverResultsRefresh(state, false);
    PumpScanCoverResultsRefresh(state, false);
    const int expectedRefreshes = expectCoverRefresh ? 1 : 0;
    check(state.resultRefreshes == expectedRefreshes &&
          state.coverWallRefreshes == expectedRefreshes, "one list and cover wall refresh for current committed covers");
    check(state.loadedCount == 450 &&
          state.resultLimit == (expectCoverRefresh ? 450 : 0), "keep loaded rows on refresh");
    return failures == 0 ? 0 : 1;
}
