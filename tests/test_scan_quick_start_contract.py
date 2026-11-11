import unittest
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MAIN = (ROOT / "src/main.cpp").read_text(encoding="utf-8")
WINDOWS_MAIN = (ROOT / "src/WindowsMain.cpp").read_text(encoding="utf-8")
HELPER = (ROOT / "src/FolderTreeContextActions.h").read_text(encoding="utf-8")
APP = (ROOT / "src/AppState.h").read_text(encoding="utf-8")
REAPER_IMPORT = (ROOT / "src/ReaperFileListImportWorkflow.cpp").read_text(encoding="utf-8")
CMake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
PRESENTATION = (ROOT / "src/ScanDialogPresentation.h").read_text(encoding="utf-8")
SCAN = MAIN.split("// Scan folder dialog", 1)[1].split("// New playlist dialog", 1)[0]
RESULT = SCAN.split("if (BeginTransientScrollbarPopupModal(scanResultPopup", 1)[1]
RESET = MAIN.split("static void ResetScanDialogResult", 1)[1].split("static void PlaceScanDialogInMainViewport", 1)[0]
LOCALIZED = {
    entry["id"]: entry
    for entry in json.loads(
        (ROOT / "locale" / "harken.ui.strings.json").read_text(encoding="utf-8")
    )["entries"]
}


class ScanQuickStartContract(unittest.TestCase):
    def test_quick_intent_is_explicit_and_right_click_only(self):
        self.assertIn("bool autoStart = false", HELPER)
        self.assertIn("state.scanDialogAutoStart = autoStart", HELPER)
        self.assertIn("QueueBrowserFolderScan(state, folderTarget)", MAIN)
        self.assertNotIn("PrepareQuickScanDialogForPath(state, folder.path, true)", MAIN)
        self.assertNotIn("PrepareQuickScanDialogForPath(state, folderPath, true)", MAIN)
        self.assertIn("PrepareQuickScanDialogForPath(state, folderPath);", MAIN)

    def test_quick_start_closes_modal_and_starts_once(self):
        self.assertIn("bool scanDialogAutoStart = false", APP)
        self.assertIn("if (state.scanDialogAutoStart && !state.pendingScanFolders.empty())", MAIN)
        self.assertIn("startScanForFolders(state.pendingScanFolders, false)", MAIN)
        self.assertIn("state.pendingScanFolders.clear();", MAIN)
        self.assertIn("state.scanDialogAutoStart = false;", MAIN)
        self.assertIn("ImGui::CloseCurrentPopup();", MAIN)

    def test_quick_scan_updates_existing_files_without_changing_manual_paths(self):
        quick_start = MAIN.index("if (state.scanDialogAutoStart && !state.pendingScanFolders.empty())")
        quick_end = MAIN.index("if (!quickAutoStarted", quick_start)
        self.assertIn("startScanForFolders(state.pendingScanFolders, false)", MAIN[quick_start:quick_end])
        self.assertNotIn("startScanForFolders(state.pendingScanFolders, true)", MAIN[quick_start:quick_end])
        manual = SCAN.split("if (!quickAutoStarted) {", 1)[1].split("if (openScanResult)", 1)[0]
        self.assertIn("selected ? L(Scan_StartScan) : L(Scan_SelectAndScan)", manual)
        self.assertIn("startScanForFolders(state.pendingScanFolders, false)", manual)
        self.assertIn("startScanForFolders({folder}, false)", manual)
        self.assertNotIn("startScanForFolders(state.pendingScanFolders, true)", manual)
        self.assertIn("bool updateExisting = lightweightInsert ? false : state.scanUpdateExisting;", MAIN)
        self.assertIn("bool removeMissing = lightweightInsert ? false : state.scanRemoveMissing;", MAIN)

    def test_quick_completion_intent_survives_until_ready_summary(self):
        start = MAIN.index("if (state.scanDialogAutoStart && !state.pendingScanFolders.empty())")
        end = MAIN.index("if (!quickAutoStarted", start)
        self.assertNotIn("state.scanDialogAutoStart = false;", MAIN[start:end])

    def test_manual_and_drag_flows_remain_confirmed(self):
        self.assertIn("aura::ui::ActionButton(selected ? L(Scan_StartScan) : L(Scan_SelectAndScan)", SCAN)
        self.assertIn("startScanForFolders(state.pendingScanFolders, false)", MAIN)
        self.assertIn("aura::ui::ActionButton(L(Cancel)", MAIN)
        self.assertIn("state.pendingScanFolders.clear();", MAIN)
        self.assertIn("state.showScanDialog = true;", MAIN)
        self.assertIn("g_appState->scanDialogAutoStart = false;", WINDOWS_MAIN)
        import_start = REAPER_IMPORT.index("void Start(AppState&")
        import_end = REAPER_IMPORT.index("const uint64_t scanGeneration", import_start)
        self.assertIn(
            "state.scanDialogAutoStart = false;",
            REAPER_IMPORT[import_start:import_end],
        )

    def test_scan_completion_cancellation_and_autofilter_contract_remains(self):
        self.assertIn("state.scanCancelRequested = false;", MAIN)
        self.assertIn("state.scanWasPaused = paused;", MAIN)
        self.assertIn("state.scanWasCancelled = rollbackCancelled;", MAIN)
        self.assertIn("state.autoFilterToScannedPath = !rollbackCancelled;", MAIN)
        self.assertIn("state.needsDatabaseRefresh = true;", MAIN)
        self.assertIn("state.scanDataCommitted = false;", MAIN)

    def test_fast_completion_releases_optional_cover_work_to_background(self):
        self.assertIn("HarkenSearch::ShouldReleaseFastScanCompletionGate(", MAIN)
        self.assertIn("if (releaseFastCompletionGate) {", MAIN)
        self.assertIn("state.scanCompletionCoversPending.store(\n                                false,", MAIN)
        self.assertIn("state.scanReadyToWorkCoverPostProcessPending = coverCompletionPending;", MAIN)
        self.assertIn("ScanFastCompletionReleased", MAIN)
        self.assertIn("HarkenSearch::ShouldReleaseFastScanCompletionGate(", WINDOWS_MAIN)
        self.assertIn("if (releaseFastCompletionGate) {", WINDOWS_MAIN)
        self.assertIn("state.scanCompletionCoversPending.store(\n                    false,", WINDOWS_MAIN)
        self.assertIn("state.scanReadyToWorkCoverPostProcessPending = coverCompletionPending;", WINDOWS_MAIN)

    def test_ready_completion_leaves_progress_for_a_separate_dismissible_result(self):
        transition = SCAN.split("if (scanView.surface == HarkenSearch::ScanDialogSurface::Result)", 1)[1].split("} else if", 1)[0]
        self.assertIn("ImGui::CloseCurrentPopup();", transition)
        self.assertIn("openScanResult = true;", transition)
        self.assertIn("if (openScanResult) ImGui::OpenPopup(scanResultPopup);", SCAN)
        self.assertIn("DrawScanDialogHeader(title, true)", RESULT)
        self.assertIn("aura::ui::ActionButton(L(Done)", RESULT)
        self.assertIn("scanSummaryToastUntil", MAIN)
        self.assertIn("scanCompletionPending.load(std::memory_order_acquire)", MAIN)

    def test_ready_summary_requires_terminal_completion_counters(self):
        projection = PRESENTATION.split("inline ScanDialogView BuildScanDialogView", 1)[1].split("enum class ScanFailureCategory", 1)[0]
        busy = projection.split("if (input.scanning || input.completionPending)", 1)[1].split("return view;", 1)[0]
        self.assertIn("view.surface = ScanDialogSurface::Progress", busy)
        self.assertIn("view.canClose = false", busy)
        self.assertIn("snapshot.stage == ScanWorkflowStage::Completed && CanEnterCompleted(snapshot)", projection)
        self.assertIn("state.scanCompletionPending.load(std::memory_order_acquire)", SCAN)

    def test_fast_completion_skips_redundant_queue_rebuild_and_honors_pause(self):
        resume_start = MAIN.index("const bool resumeBackground =")
        resume_end = MAIN.index("// Full scans retain their foreground", resume_start)
        resume = MAIN[resume_start:resume_end]
        self.assertIn("ShouldRebuildScanBackfillQueue", resume)
        self.assertIn("state.isScanTaskCancelled(scanGeneration, scanCancelToken)", resume)
        self.assertIn("finishInterruptedScanSession(paused);", resume)
        self.assertLess(resume.index("finishInterruptedScanSession(paused);"),
                        resume.index("scanDb.discardScanRollbackSession();"))

    def test_isolated_qa_can_exercise_native_right_click_and_pause_path(self):
        self.assertIn('cmd == "qa_rightclick_scan"', WINDOWS_MAIN)
        self.assertIn('PrepareQuickScanDialogForPath(state, path, true)', WINDOWS_MAIN)
        self.assertIn('cmd == "qa_scan_pause_keep"', WINDOWS_MAIN)
        self.assertIn('state.requestPauseScanTask();', WINDOWS_MAIN)

    def test_pending_errors_and_cancelled_summaries_remain_visible(self):
        self.assertIn("BuildScanCompletionPendingLabel(state)", MAIN)
        self.assertIn("scanView.phase == Phase::Cancelled", RESULT)
        self.assertIn("L(ScanUi_Revoked)", RESULT)
        self.assertIn("const bool showFailures = scanView.failed > 0", RESULT)
        self.assertIn("L(Main_FailedFmt), scanView.failed", RESULT)
        self.assertIn("L(ScanUi_ViewFailures)", RESULT)
        self.assertIn("const char* phase = L(ScanUi_Finalizing)", SCAN)
        self.assertIn("正在整理", LOCALIZED["ScanUi_Finalizing"]["zh"])

    def test_ready_summary_does_not_duplicate_refresh_or_acknowledgement(self):
        summary_header = SCAN.split("if (scanView.surface == HarkenSearch::ScanDialogSurface::Result)", 1)[1].split("} else if", 1)[0]
        self.assertNotIn("state.resetScanReadyToWorkState();", summary_header)
        self.assertNotIn("state.needsDatabaseRefresh = true;", summary_header)
        self.assertNotIn("ResetScanDialogResult(state)", summary_header)
        self.assertNotIn("state.scanDialogAutoStart = false", summary_header)

    def test_done_acknowledgement_clears_quick_completion_intent(self):
        done = RESULT.split("if (aura::ui::ActionButton(L(Done)", 1)[1].split('ThemeInspector::RegisterLastItem("scan.result.done")', 1)[0]
        self.assertIn("ResetScanDialogResult(state);", done)
        self.assertIn("ImGui::CloseCurrentPopup();", done)
        self.assertIn("state.scanDialogAutoStart = false;", RESET)
        self.assertIn("if (state.isScanning.load() || state.scanCompletionPending.load()) return;", RESET)

    def test_progressive_refresh_keeps_existing_safe_gate(self):
        self.assertIn("state.scanDataCommitted.load()", WINDOWS_MAIN)
        self.assertIn("state.isScanning.load() && !scanPopupOpen", WINDOWS_MAIN)
        self.assertIn("RefreshSearchResults(state, false)", WINDOWS_MAIN)
        self.assertIn("state.needsDatabaseRefresh", WINDOWS_MAIN)

    def test_ctest_registration_is_source_only(self):
        self.assertIn("TestScanQuickStartContract", CMake)
        self.assertIn("test_scan_quick_start_contract.py", CMake)


if __name__ == "__main__":
    unittest.main()
