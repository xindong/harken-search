#include "DatabaseSessionScanStateReset.h"

#include "AppState.h"

namespace aura::database_session {

void ResetDatabaseSessionScanState(AppState& state) {
    state.isScanning = false;
    state.scanPhase.store(AppState::ScanPhase::Idle);
    state.scanProgress = 0;
    state.scanTotal = 0;
    state.scanAddedCount = 0;
    state.scanUpdatedCount = 0;
    state.scanSkippedCount = 0;
    state.scanRemovedCount = 0;
    state.scanErrorCount = 0;
    state.scanIngestedCount = 0;
    state.scanIngestDurationMs = 0;
    state.clearScanTiming();
    state.clearScanWorkflowState();
    state.scanCancelRequested = false;
    state.scanWasCancelled = false;
    state.scanWasPaused = false;
    state.scanSummaryToastUntil = 0.0;
    state.setScanFile("");
    state.clearScanErrors();
    state.lastScannedPath.clear();
    state.autoFilterToScannedPath = false;
    // The database switch worker has joined the old batch before this reset.
    state.batchFeatureResults.Clear();
    {
        std::lock_guard<std::mutex> lock(state.batchFeatureResultMutex);
        state.batchFeatureResult = {};
    }
    state.batchFeatureProgress.store(0);
    state.batchFeatureTotal.store(0);
    state.batchFeatureSkipped.store(0);
    state.batchFeatureSaved.store(0);
    state.batchFeatureCompletionNotice.store(false);
    state.showSimilarAnalysisReport = false;
    state.showScanSimilarityDialog = false;
    state.scanSimilarityOwned = false;
}

} // namespace aura::database_session
