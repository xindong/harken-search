#include <cmath>
#include <cstdlib>
#include <iostream>

#if __has_include("ScanDialogPresentation.h")
#include "ScanDialogPresentation.h"
using namespace HarkenSearch;
static void Check(bool ok, const char* message) {
    if (!ok) { std::cerr << "FAIL: " << message << "\n"; std::exit(1); }
}
int main(int argc, char** argv) {
    if (argc == 1 || std::string(argv[1]) == "--sampling-gap") {
        ScanDialogMetrics metrics;
        ScanDialogView view; view.surface = ScanDialogSurface::Progress;
        view.phase = ScanDialogPhase::Ingesting;
        metrics.Update(1, 1000, 0, 1000, view);
        view.current = 10;
        metrics.Update(1, 1000, 0, 1250, view);
        view.current = 10000;
        auto rate = metrics.Update(1, 1000, 0, 101250, view);
        Check(!rate.speedKnown && rate.filesPerSecond == 0., "long observation gap must not present a whole-gap average as recent speed");
        Check(rate.elapsedMs == 100250, "window restoration retains the real total elapsed");
        view.current = 10010;
        rate = metrics.Update(1, 1000, 0, 101500, view);
        Check(rate.speedKnown && rate.filesPerSecond == 40., "recent sampling resumes from restoration");
        std::cout << "PASS: observation gap resets recent speed only\n";
        if (argc > 1) return 0;
    }
    if (argc == 1 || std::string(argv[1]) == "--streaming-total") {
        ScanDialogInput input;
        input.scanning = true;
        input.workflow.stage = ScanWorkflowStage::Ingesting;
        for (int discovered : {501, 1001, 19011, 38001}) {
            input.workflow.discovered = discovered;
            input.workflow.processed = discovered - 1;
            const auto view = BuildScanDialogView(input);
            Check(!view.progressKnown, "streaming discovered count must not masquerade as a final total at 99 percent");
            Check(view.current == discovered - 1, "unknown total still exposes the real processed count");
        }
        std::cout << "PASS: streaming total remains indeterminate\n";
        if (argc > 1) return 0;
    }
    if (argc == 1 || std::string(argv[1]) == "--batched-speed") {
        ScanDialogMetrics metrics;
        ScanDialogView view;
        view.surface = ScanDialogSurface::Progress;
        view.phase = ScanDialogPhase::Ingesting;
        metrics.Update(1, 1000, 0, 1000, view);
        view.current = 50;
        auto rate = metrics.Update(1, 1000, 0, 1500, view);
        Check(rate.speedKnown && rate.filesPerSecond > 0., "first completed batch produces a real measured rate");
        rate = metrics.Update(1, 1000, 0, 1750, view);
        Check(rate.speedKnown && rate.filesPerSecond > 0., "callback gap inside recent window must not flash zero");
        Check(std::abs(rate.filesPerSecond - 50. / .75) < .001, "recent rate includes the elapsed callback gap");
        for (int now = 2000; now <= 5000; now += 250)
            rate = metrics.Update(1, 1000, 0, now, view);
        Check(rate.filesPerSecond == 0., "real sustained inactivity expires the old rate");
        Check(rate.waitingForProgress && !rate.speedKnown, "sustained inactivity is explicit, not a repeated zero-rate claim");
        Check(rate.elapsedMs == 4000, "no-progress intervals still count toward total elapsed");
        view.current = 55;
        rate = metrics.Update(1, 1000, 0, 5250, view);
        Check(rate.speedKnown && rate.filesPerSecond > 0. && !rate.waitingForProgress, "new progress clears the waiting state");
        view.current = 1;
        rate = metrics.Update(1, 1000, 0, 5500, view);
        Check(!rate.speedKnown && !rate.waitingForProgress, "decreased counters reset rate history");
        view.current = 2;
        rate = metrics.Update(1, 1000, 0, 5400, view);
        Check(!rate.speedKnown && rate.filesPerSecond == 0., "backwards clock resets without negative rates");
        for (int now = 5650; now <= 100000; now += 250) {
            ++view.current;
            rate = metrics.Update(1, 1000, 0, now, view);
        }
        Check(rate.speedKnown && std::abs(rate.filesPerSecond - 4.) < .001, "long running rate remains a recent window not a task average");
        std::cout << "PASS: batched throughput and sustained inactivity\n";
        if (argc > 1) return 0;
    }
    ScanDialogMetrics metrics;
    ScanDialogView active;
    active.surface = ScanDialogSurface::Progress;
    active.phase = ScanDialogPhase::Ingesting;
    active.current = 10;
    auto timing = metrics.Update(1, 1000, 0, 1100, active);
    Check(timing.elapsedMs == 100 && !timing.speedKnown, "first frame starts elapsed time without invented speed");
    active.current = 30;
    timing = metrics.Update(1, 1000, 0, 1600, active);
    Check(timing.elapsedMs == 600 && timing.speedKnown && std::abs(timing.filesPerSecond - 40.) < .001, "speed follows actual processed delta");
    timing = metrics.Update(1, 1000, 0, 2100, active);
    Check(timing.speedKnown && timing.filesPerSecond == 20., "short batch gap uses the recent window");
    active.phase = ScanDialogPhase::Waveforms; active.current = 2;
    timing = metrics.Update(1, 1000, 0, 2200, active);
    Check(!timing.speedKnown && timing.elapsedMs == 1200, "stage change resets speed but not total elapsed");
    active.current = 12;
    timing = metrics.Update(1, 1000, 0, 2700, active);
    Check(timing.speedKnown && timing.filesPerSecond == 20., "waveform speed uses waveform counter");
    active.phase = ScanDialogPhase::Finalizing;
    timing = metrics.Update(1, 1000, 0, 2800, active);
    Check(!timing.speedKnown && timing.elapsedMs == 1800, "finalizing retains elapsed without stale speed");
    active.surface = ScanDialogSurface::Result; active.phase = ScanDialogPhase::Paused;
    timing = metrics.Update(1, 1000, -1, 3000, active);
    Check(timing.elapsedMs == 2000, "pause freezes total elapsed");
    timing = metrics.Update(1, 1000, -1, 8000, active);
    Check(timing.elapsedMs == 2000, "reading a paused result does not extend elapsed");
    active.surface = ScanDialogSurface::Progress; active.phase = ScanDialogPhase::Counting; active.current = 0;
    timing = metrics.Update(2, 10000, 0, 10000, active);
    Check(!timing.speedKnown && timing.elapsedMs == 0, "new task resets both metrics");
    active.surface = ScanDialogSurface::Result; active.phase = ScanDialogPhase::Completed;
    timing = metrics.Update(2, 10000, 1234, 13000, active);
    Check(timing.elapsedMs == 1234, "completed result uses actual recorded completion duration");
    timing = metrics.Update(3, 20000, 0, 25000, active);
    Check(timing.elapsedMs == 0, "zero-duration task does not count time until window restoration");
    Check(ScanProgressPercent(499, 500) == 99, "progress must not round unfinished work up to 100 percent");
    Check(ScanProgressPercent(500, 500) == 100 && ScanProgressPercent(0, 0) == 0, "complete and unknown progress");
    ScanDialogInput input;
    auto view = BuildScanDialogView(input);
    Check(view.surface == ScanDialogSurface::Setup && view.canClose, "idle is setup");
    input.scanning = true;
    input.workflow.stage = ScanWorkflowStage::Scanning;
    input.workflow.discovered = 3482;
    view = BuildScanDialogView(input);
    Check(view.phase == ScanDialogPhase::Counting && !view.progressKnown, "enumerating has no false percentage");
    Check(view.canBackground && view.canStop && !view.canClose, "active scan actions");
    input.workflow.stage = ScanWorkflowStage::Ingesting;
    input.workflow.discovered = 7093; input.workflow.processed = 4823;
    view = BuildScanDialogView(input);
    Check(!view.progressKnown && view.current == 4823, "dynamic total is not advertised as a final denominator");
    input.ingestionTotalKnown = true;
    view = BuildScanDialogView(input);
    Check(view.current == 4823 && view.total == 7093 && view.progressKnown, "ingest uses file counters");
    Check(std::abs(view.fraction - 4823.f / 7093.f) < .0001f, "true ingest fraction");
    input.workflow.stage = ScanWorkflowStage::CachingMiniWaveforms;
    input.workflow.processed = 7093;
    input.workflow.miniWaveformsRequired = true;
    input.workflow.miniWaveformsTotal = 49; input.workflow.miniWaveformsCompleted = 31;
    view = BuildScanDialogView(input);
    Check(view.current == 31 && view.total == 49 && view.progressKnown, "waveform uses 31/49 not 7093/7093");
    Check(!view.canBackground && view.canStop, "foreground waveform completion gate remains");
    input.workflow.stage = ScanWorkflowStage::Finalizing;
    view = BuildScanDialogView(input);
    Check(!view.progressKnown && !view.canBackground, "finalizing has no false 100 percent");
    input.stopDisposition = ScanStopDisposition::PauseKeepCommitted;
    view = BuildScanDialogView(input);
    Check(view.phase == ScanDialogPhase::Saving && !view.canStop && !view.canBackground, "saving is not already paused");
    input.stopDisposition = ScanStopDisposition::CancelRollback;
    view = BuildScanDialogView(input);
    Check(view.phase == ScanDialogPhase::Revoking && !view.canClose, "revocation is not completion");
    input.scanning = false; input.workflow.stage = ScanWorkflowStage::Paused;
    input.stopDisposition = ScanStopDisposition::PauseKeepCommitted;
    input.workflow.processed = 4823; input.workflow.succeeded = 4800;
    input.workflow.failed = 15; input.workflow.skipped = 8;
    view = BuildScanDialogView(input);
    Check(view.surface == ScanDialogSurface::Result && view.phase == ScanDialogPhase::Paused, "paused is only result not setup");
    Check(view.succeeded == 4800 && view.failed == 15 && view.skipped == 8 && view.unprocessed == 2270, "paused partition");
    input.workflow.stage = ScanWorkflowStage::Cancelled;
    input.stopDisposition = ScanStopDisposition::CancelRollback;
    view = BuildScanDialogView(input);
    Check(view.phase == ScanDialogPhase::Cancelled, "rollback terminal");
    input.fatalFailure = true;
    view = BuildScanDialogView(input);
    Check(view.phase == ScanDialogPhase::Failed, "rollback failure cannot say revoked");
    input = {}; input.workflow.stage = ScanWorkflowStage::Completed;
    input.workflow.foregroundTasksDrained = true;
    view = BuildScanDialogView(input);
    Check(view.phase == ScanDialogPhase::Empty, "zero files is an empty result");
    input.workflow.discovered = input.workflow.processed = 7093;
    input.workflow.succeeded = 7022; input.workflow.failed = 42; input.workflow.skipped = 29;
    view = BuildScanDialogView(input);
    Check(view.phase == ScanDialogPhase::Partial && view.succeeded == 7022, "failed files are not successes");
    input.workflow.foregroundTasksDrained = false; input.completionPending = true;
    view = BuildScanDialogView(input);
    Check(view.surface == ScanDialogSurface::Progress && !view.progressKnown, "pending work cannot show result");
    Check(ClassifyScanFailure("Cannot read directory entry path under: X:/audio") == ScanFailureCategory::Path, "path reason");
    Check(ClassifyScanFailure("Failed to decode audio: X:/a.wav") == ScanFailureCategory::Read, "decode reason");
    Check(ClassifyScanFailure("No audio samples: X:/a.wav") == ScanFailureCategory::Empty, "empty reason");
    Check(ClassifyScanFailure("Audio file too small to decode: X:/zero.wav") == ScanFailureCategory::Empty, "real scanner empty-file reason");
    Check(ClassifyScanFailure("Database write failed during scan") == ScanFailureCategory::Save, "save reason");
    Check(ClassifyScanFailure("Unexpected failure") == ScanFailureCategory::Other, "unclassified remains other");
    Check(ScanFailureSubject("Failed to read audio metadata: X:/音效/door.wav (bad tag)") == "door.wav", "failure row exposes filename");
    Check(ScanFailureSubject("Database write failed during scan").empty(), "no invented filename");
    Check(ScanFailureSubject("Failed to read audio metadata: X:/library (new)/door (1).wav (bad tag)") == "door (1).wav", "parentheses in names survive");
    Check(ScanFailureSubject("Failed to decode: X:/library (new)/door (1).wav") == "door (1).wav", "filename without diagnostic suffix survives");
    auto cleanup = BuildCleanupScanSnapshot(true, false, false, 7, 20);
    Check(cleanup.stage == ScanWorkflowStage::Ingesting && cleanup.processed == 7 && cleanup.discovered == 20, "legacy cleanup retains real progress");
    ScanDialogInput cleanupInput;
    cleanupInput.workflow = cleanup; cleanupInput.scanning = true; cleanupInput.ingestionTotalKnown = true;
    Check(BuildScanDialogView(cleanupInput).progressKnown, "fixed cleanup retains its percentage");
    cleanup = BuildCleanupScanSnapshot(false, false, false, 20, 20);
    Check(CanEnterCompleted(cleanup), "legacy cleanup can show its terminal result");
    cleanup = BuildCleanupScanSnapshot(false, false, false, 0, 4);
    Check(CanEnterCompleted(cleanup), "small cleanup completes even when its throttled callback omitted final progress");
    cleanup = BuildCleanupScanSnapshot(false, true, false, 7, 20);
    Check(cleanup.stage == ScanWorkflowStage::Cancelled, "cleanup stop is distinct from completion");
    Check(ScanDiagnosticTailUtf8(std::string(20, 'a') + "中文日志", 10) == "文日志", "diagnostic clipping starts on a UTF-8 boundary");
    auto failures = BuildScanFailureSummary({"Failed to decode audio: X:/a.wav", "Database write failed during scan"}, 5);
    Check(failures[1] == 1 && failures[3] == 1 && failures[4] == 3, "unrecorded reasons remain unknown not invented");
    std::cout << "PASS: scan presentation states, truthful counts and failure categories\n";
}
#else
int main() { std::cerr << "FAIL: scan dialog presentation is not implemented\n"; return 1; }
#endif
