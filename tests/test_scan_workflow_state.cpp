#include "ScanWorkflow.h"

#include <cmath>
#include <iostream>
#include <string>

namespace {

int g_failures = 0;

void check(bool condition, const char* label) {
    std::cout << (condition ? "PASS: " : "FAIL: ") << label << '\n';
    if (!condition) ++g_failures;
}

HarkenSearch::ScanWorkflowSnapshot completedSnapshot() {
    HarkenSearch::ScanWorkflowSnapshot snapshot;
    snapshot.discovered = 3206;
    snapshot.processed = 3206;
    snapshot.succeeded = 3019;
    snapshot.skipped = 187;
    snapshot.failed = 0;
    snapshot.miniWaveformsCompleted = 3206;
    snapshot.miniWaveformsTotal = 3206;
    snapshot.miniWaveformsRequired = true;
    snapshot.foregroundTasksDrained = true;
    return snapshot;
}

void testSuccessfulCountCanBeLowerThanProcessedTotal() {
    auto snapshot = completedSnapshot();
    snapshot.stage = HarkenSearch::ResolveTerminalScanWorkflowStage(
        snapshot, HarkenSearch::ScanStopDisposition::None, false);
    const auto progress = HarkenSearch::BuildScanProcessingProgress(snapshot);

    check(snapshot.stage == HarkenSearch::ScanWorkflowStage::Completed,
          "success count below total still completes after every file is processed");
    check(progress.current == 3206 && progress.total == 3206,
          "completed processing progress uses processed/discovered, not successful ingest count");
    check(std::fabs(progress.fraction - 1.0f) < 0.0001f,
          "completed UI progress is exactly 100 percent");
}

void testSkippedFilesRemainACompletePartition() {
    const auto counts = HarkenSearch::BuildScanOutcomeCounts(10, 10, 7, 3, 0);
    check(counts.succeeded == 7 && counts.skipped == 3 && counts.failed == 0,
          "skipped files are separate from successful ingestion");
    check(counts.succeeded + counts.skipped + counts.failed == counts.processed,
          "result buckets sum to processed files");
}

void testFailedFilesDoNotPreventNonFatalCompletion() {
    auto snapshot = completedSnapshot();
    snapshot.succeeded = 3009;
    snapshot.skipped = 187;
    snapshot.failed = 10;
    snapshot.stage = HarkenSearch::ResolveTerminalScanWorkflowStage(
        snapshot, HarkenSearch::ScanStopDisposition::None, false);

    check(snapshot.stage == HarkenSearch::ScanWorkflowStage::Completed,
          "individual failed files are summarized after all work drains");
    check(HarkenSearch::BuildScanProcessingProgress(snapshot).current == snapshot.discovered,
          "completed scan with failures still shows all files processed");
    check(HarkenSearch::ResolveTerminalScanWorkflowStage(
              snapshot, HarkenSearch::ScanStopDisposition::None, true) ==
              HarkenSearch::ScanWorkflowStage::Failed,
          "fatal workflow failure remains distinct from per-file failures");
}

void testMiniWaveformDebtBlocksCompleted() {
    auto snapshot = completedSnapshot();
    snapshot.miniWaveformsCompleted = 3019;
    snapshot.foregroundTasksDrained = false;

    check(!HarkenSearch::CanEnterCompleted(snapshot),
          "required mini waveform debt blocks completed");
    check(HarkenSearch::ResolveTerminalScanWorkflowStage(
              snapshot, HarkenSearch::ScanStopDisposition::None, false) ==
              HarkenSearch::ScanWorkflowStage::CachingMiniWaveforms,
          "required mini waveform debt keeps caching stage active");
    check(std::string(HarkenSearch::ScanWorkflowStageName(
              HarkenSearch::ScanWorkflowStage::CachingMiniWaveforms)) ==
              "cachingMiniWaveforms",
          "public workflow stage uses the required cachingMiniWaveforms name");

    snapshot.miniWaveformsRequired = false;
    snapshot.miniWaveformsContinueInBackground = true;
    snapshot.foregroundTasksDrained = true;
    check(HarkenSearch::ResolveTerminalScanWorkflowStage(
              snapshot, HarkenSearch::ScanStopDisposition::None, false) ==
              HarkenSearch::ScanWorkflowStage::Completed,
          "fast scan may complete only when background mini continuation is explicit");
}

void testFastModeReleasesOptionalCompletionDebtImmediately() {
    check(HarkenSearch::ShouldReleaseFastScanCompletionGate(
              /*discovered=*/2258,
              /*processed=*/2258,
              /*fastMode=*/true),
          "fast scan releases the foreground completion gate once indexing reaches the discovered total");
    check(!HarkenSearch::ShouldReleaseFastScanCompletionGate(
              /*discovered=*/2258,
              /*processed=*/2257,
              /*fastMode=*/true),
          "fast scan keeps the completion gate while indexing still has files to process");
    check(!HarkenSearch::ShouldReleaseFastScanCompletionGate(
              /*discovered=*/2258,
              /*processed=*/2258,
              /*fastMode=*/false),
          "full scan keeps the foreground completion contract for required waveform work");
}

void testFastCompletionDoesNotWaitForOptionalCovers() {
    check(HarkenSearch::AreScanCompletionCountersReady(
              /*fastMode=*/true, /*miniUnresolved=*/0,
              /*coverUnresolved=*/7093, /*metadataUnresolved=*/0),
          "fast scan summary is ready while covers continue in background");
    check(!HarkenSearch::AreScanCompletionCountersReady(
              /*fastMode=*/false, /*miniUnresolved=*/0,
              /*coverUnresolved=*/7093, /*metadataUnresolved=*/0),
          "full scan still waits for required covers");
    check(!HarkenSearch::AreScanCompletionCountersReady(
              /*fastMode=*/true, /*miniUnresolved=*/1,
              /*coverUnresolved=*/0, /*metadataUnresolved=*/0),
          "required mini work cannot be hidden by fast completion");
}

void testFastCompletionPreservesPopulatedBackgroundQueue() {
    check(!HarkenSearch::ShouldRebuildScanBackfillQueue(true, 7093, 0),
          "fast completion reuses queued cover work without a blocking rebuild");
    check(HarkenSearch::ShouldRebuildScanBackfillQueue(true, 0, 0),
          "empty fast queue recovers durable background work");
    check(HarkenSearch::ShouldRebuildScanBackfillQueue(false, 7093, 0),
          "full scan retains its existing queue recovery path");
}

void testPauseIsASeparateTerminalStage() {
    auto snapshot = completedSnapshot();
    snapshot.processed = 120;
    snapshot.foregroundTasksDrained = false;

    check(HarkenSearch::ResolveTerminalScanWorkflowStage(
              snapshot,
              HarkenSearch::ScanStopDisposition::PauseKeepCommitted,
              false) == HarkenSearch::ScanWorkflowStage::Paused,
          "pause is distinct from completed and cancelled");
    check(HarkenSearch::ResolveTerminalScanWorkflowStage(
              snapshot,
              HarkenSearch::ScanStopDisposition::PauseKeepCommitted,
              true) == HarkenSearch::ScanWorkflowStage::Failed,
          "fatal failure wins over a pause request");
    check(std::string(HarkenSearch::ScanWorkflowStageName(
              HarkenSearch::ScanWorkflowStage::Paused)) == "paused",
          "paused has a stable public diagnostic name");
}

void testPostScanSimilarityIndexStartsOnlyOnSuccessfulCompletedScan() {
    auto snapshot = completedSnapshot();
    snapshot.stage = HarkenSearch::ScanWorkflowStage::Completed;

    check(HarkenSearch::ShouldStartPostScanSimilarityIndex(
              true, snapshot, HarkenSearch::ScanStopDisposition::None, false),
          "checked option starts similarity index after a successful completed scan");
    check(!HarkenSearch::ShouldStartPostScanSimilarityIndex(
              false, snapshot, HarkenSearch::ScanStopDisposition::None, false),
          "unchecked option never starts similarity index");
    check(HarkenSearch::ShouldStartPostScanSimilarityIndex(
              true, snapshot, HarkenSearch::ScanStopDisposition::None, false),
          "successful completed scans still queue even if an index job is already running");
    check(!HarkenSearch::ShouldConsumePendingPostScanSimilarityIndex(true, false, true),
          "queued request waits while an existing similarity index job is running");
    check(HarkenSearch::ShouldConsumePendingPostScanSimilarityIndex(true, false, false),
          "queued request starts once the existing similarity index job finishes");
    check(!HarkenSearch::ShouldConsumePendingPostScanSimilarityIndex(true, true, false),
          "queued request waits while a database switch is in progress");
}

void testPendingPostScanSimilarityIndexDoesNotCrossDatabaseSwitch() {
    check(!HarkenSearch::ShouldConsumePendingPostScanSimilarityIndex(false, false, false),
          "no pending request means nothing to consume");
}

void testPostScanSimilarityIndexDoesNotStartOnCancelFailOrPartialFailure() {
    auto snapshot = completedSnapshot();
    snapshot.stage = HarkenSearch::ScanWorkflowStage::Completed;

    check(!HarkenSearch::ShouldStartPostScanSimilarityIndex(
              true, snapshot, HarkenSearch::ScanStopDisposition::CancelRollback, false),
          "cancelled scans do not start similarity index");
    check(!HarkenSearch::ShouldStartPostScanSimilarityIndex(
              true, snapshot, HarkenSearch::ScanStopDisposition::PauseKeepCommitted, false),
          "paused scans do not start similarity index");
    check(!HarkenSearch::ShouldStartPostScanSimilarityIndex(
              true, snapshot, HarkenSearch::ScanStopDisposition::None, true),
          "fatal scan failure does not start similarity index");

    snapshot.failed = 10;
    snapshot.succeeded = 3009;
    check(!HarkenSearch::ShouldStartPostScanSimilarityIndex(
              true, snapshot, HarkenSearch::ScanStopDisposition::None, false),
          "partial file failures do not start a full similarity index");

    snapshot = completedSnapshot();
    snapshot.discovered = 0;
    snapshot.processed = 0;
    snapshot.succeeded = 0;
    snapshot.skipped = 0;
    snapshot.miniWaveformsCompleted = 0;
    snapshot.miniWaveformsTotal = 0;
    check(!HarkenSearch::ShouldStartPostScanSimilarityIndex(
              true, snapshot, HarkenSearch::ScanStopDisposition::None, false),
          "empty scans do not start similarity index");
}

void testStoppedScanDoesNotPublishCompletionToast() {
    check(!HarkenSearch::ShouldPublishScanCompletionToast(
              true, false, 71, false,
              HarkenSearch::ScanStopDisposition::CancelRollback),
          "manually cancelled scan never publishes a completed status");
    check(!HarkenSearch::ShouldPublishScanCompletionToast(
              true, false, 71, false,
              HarkenSearch::ScanStopDisposition::PauseKeepCommitted),
          "paused scan never publishes a completed status");
    check(HarkenSearch::ShouldPublishScanCompletionToast(
              true, false, 71, false,
              HarkenSearch::ScanStopDisposition::None),
          "ordinary completed scan still publishes its status");
    check(!HarkenSearch::ShouldPublishScanCompletionToast(
              true, false, 71, true,
              HarkenSearch::ScanStopDisposition::None),
          "unfinished foreground work does not publish completion");
    check(!HarkenSearch::ShouldPublishScanCompletionToast(
              true, false, 0, false,
              HarkenSearch::ScanStopDisposition::None),
          "empty scan does not publish completion");
}

} // namespace

int main() {
    testSuccessfulCountCanBeLowerThanProcessedTotal();
    testSkippedFilesRemainACompletePartition();
    testFailedFilesDoNotPreventNonFatalCompletion();
    testMiniWaveformDebtBlocksCompleted();
    testFastModeReleasesOptionalCompletionDebtImmediately();
    testFastCompletionDoesNotWaitForOptionalCovers();
    testFastCompletionPreservesPopulatedBackgroundQueue();
    testPauseIsASeparateTerminalStage();
    testPostScanSimilarityIndexStartsOnlyOnSuccessfulCompletedScan();
    testPendingPostScanSimilarityIndexDoesNotCrossDatabaseSwitch();
    testPostScanSimilarityIndexDoesNotStartOnCancelFailOrPartialFailure();
    testStoppedScanDoesNotPublishCompletionToast();

    if (g_failures == 0) {
        std::cout << "All scan workflow state tests passed.\n";
        return 0;
    }
    std::cerr << g_failures << " scan workflow state test(s) failed.\n";
    return 1;
}
