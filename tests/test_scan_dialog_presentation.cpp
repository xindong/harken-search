#include <cmath>
#include <cstdlib>
#include <iostream>

#if __has_include("ScanDialogPresentation.h")
#include "ScanDialogPresentation.h"
using namespace HarkenSearch;
static void Check(bool ok, const char* message) {
    if (!ok) { std::cerr << "FAIL: " << message << "\n"; std::exit(1); }
}
int main() {
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
