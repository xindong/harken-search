"""Wiring checks for the approved compact scan UI; runtime tests cover projection."""
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]
MAIN = (ROOT / "src/main.cpp").read_text(encoding="utf-8")
SCAN = MAIN.split("// Scan folder dialog", 1)[1].split("// New playlist dialog", 1)[0]
SETUP = SCAN.split("auto startScanForFolders", 1)[0]


def test_scan_dialog_removes_raw_drag_diagnostics():
    assert "state.lastDropSummary.c_str()" not in SCAN
    assert "L(Main_ClearDragInfoBtn)" not in SCAN
    assert "L(Main_ScanFolderHint)" not in SCAN


def test_scan_shell_uses_content_title_without_native_title_strip():
    assert "ImGuiWindowFlags_NoTitleBar" in SCAN
    assert "DrawScanDialogHeader" in SCAN
    assert "L(Main_ScannerTitle)" not in SCAN
    assert "ImGui::SameLine(425.0f)" not in SCAN
    assert "PlaceScanDialogInMainViewport" in SCAN
    placement = MAIN.split("static void PlaceScanDialogInMainViewport",1)[1].split("static void DrawScanSimilarityDialogs",1)[0]
    assert "SetNextWindowViewport" in placement and "ImGui::GetMainViewport" in placement
    assert "ImGuiCond_Appearing" in placement and "std::clamp" in placement


def test_setup_and_terminal_results_are_separate_surfaces():
    assert "BuildScanDialogView" in SCAN
    assert "ScanDialogSurface::Setup" in SCAN
    assert "ScanDialogSurface::Result" in SCAN
    assert "ScanDialogSurface::Progress" in SCAN
    assert "BuildCleanupScanSnapshot" in SCAN
    assert "L(ScanUi_CleanupRemovedFmt)" in SCAN
    assert "if (scanIsCleanup) state.requestCancelScanTask()" in SCAN


def test_advanced_disclosure_contains_original_scan_options():
    assert "L(ScanUi_AdvancedOptions)" in SETUP
    for option in ("scanRecursive", "scanUpdateExisting", "scanRemoveMissing",
                   "scanWorkerThreads", "scanBuildSimilarityIndexAfterScan"):
        assert option in SETUP
    assert "L(ScanUi_AutoThreads)" in SETUP
    assert "L(Scan_FullModeHint)" not in SETUP
    assert "L(Scan_FastModeHint)" not in SETUP


def test_progress_does_not_duplicate_numbers_and_preserves_waiting_phase():
    assert "%.1f f/s" not in SCAN
    assert "scanView.progressKnown" in SCAN
    assert "DrawScanWaitingBar" in SCAN
    assert "L(ScanUi_Statistics)" in SCAN


def test_speed_and_elapsed_are_visible_without_opening_statistics():
    assert "state.scanUiMetrics.Update(" in SCAN
    assert "DrawScanDialogMetrics(state.scanUiMetrics.Value(), true)" in SETUP
    result = SCAN.split("if (BeginTransientScrollbarPopupModal(scanResultPopup", 1)[1]
    assert "DrawScanDialogMetrics(state.scanUiMetrics.Value(), false)" in result
    statistics = result.split("if (ImGui::TreeNode(L(ScanUi_Statistics)))", 1)[1].split("ImGui::TreePop();", 1)[0]
    assert "Main_TotalElapsedFmt" not in statistics
    metrics = MAIN.split("static void DrawScanDialogMetrics",1)[1].split("static void ResetScanDialogResult",1)[0]
    assert "L(ScanUi_SpeedFmt)" in metrics and "L(Main_TotalElapsedFmt)" in metrics
    assert '"scan.metrics.speed"' in metrics and '"scan.metrics.elapsed"' in metrics
    assert "ImGui::GetContentRegionAvail().x" in metrics


def test_terminal_timing_is_recorded_even_when_the_scan_popup_is_not_rendering():
    state = (ROOT / "src/AppState.h").read_text(encoding="utf-8")
    terminal = state.split("void resolveScanWorkflowTerminalStage",1)[1].split("void clearScanWorkflowState",1)[0]
    assert "scanFinishedAtMs.compare_exchange_strong" in terminal
    timing = state.split("void beginScanTiming",1)[1].split("void markScanCompletionReadyNow",1)[0]
    assert "scanFinishedAtMs.store(0" in timing


def test_stop_choice_preserves_original_actions_and_explicit_continue():
    assert "state.requestPauseScanTask()" in SCAN
    assert "state.requestCancelScanTask()" in SCAN
    assert "L(ScanUi_ContinueScan)" in SCAN
    assert "L(ScanUi_RevokeScan)" in SCAN
    assert "L(ScanUi_RevokeReminder)" in SCAN


def test_failure_details_have_a_separate_window():
    assert "##ScanFailureDetails" in SCAN
    assert "L(ScanUi_ViewFailures)" in SCAN
    assert "state.getScanErrors()" in SCAN


def test_paused_rescan_keeps_every_selected_folder():
    assert "state.lastScannedFolders = foldersToScan" in SCAN
    assert "resume ? state.lastScannedFolders" in SCAN


def test_setup_footer_stacks_when_two_buttons_do_not_fit():
    footer = SCAN.split("if (!quickAutoStarted)", 1)[1].split("if (openScanResult)", 1)[0]
    assert "stackFooter" in footer
    assert "if (!stackFooter) ImGui::SameLine" in footer


def test_network_details_translate_identifier_with_added_context():
    details = SCAN.split('BeginTransientScrollbarPopupModal("##ScanFailureDetails"', 1)[1]
    assert 'error.find("Scan_NetworkLibraryUnreachable")' in details
    assert 'error == "Scan_NetworkLibraryUnreachable"' not in details


def test_similarity_reports_actual_saved_and_failed_results():
    batch = MAIN.split("void StartBatchFeatureExtraction(AppState& state)", 1)[1].split("void DrawSimilarityIndexPanel", 1)[0]
    assert "batchFeatureResults.RecordSuccess()" in batch
    assert "batchFeatureResults.RecordFailure" in batch
    assert "saved = state.database.saveAudioFeaturesByPath" in batch
    assert "FeatureExtractionError::Cancelled" in batch
    assert "##ScanSimilarityProgress" in MAIN
    assert "##ScanSimilarityResult" in MAIN


def test_busy_folder_drop_keeps_current_file_and_reports_ignored_count():
    windows = (ROOT / "src/WindowsMain.cpp").read_text(encoding="utf-8")
    busy = windows.split("// Process folders - add to database", 1)[1].split("// Process files", 1)[0]
    assert "setScanFile(" not in busy
    assert "scanIgnoredDropCount" in busy
    assert "showScanBusyNotice" in busy
    assert "##ScanDropBusy" in MAIN


def test_direct_file_drop_notice_counts_actual_database_writes():
    windows = (ROOT / "src/WindowsMain.cpp").read_text(encoding="utf-8")
    direct = windows.split("// Process files - add individual files", 1)[1].split("return 0;", 1)[0]
    assert "if (g_appState->database.addFile(record))" in direct
    assert "scanDropImportedCount" in direct
    assert "scanDropFailedCount" in direct
    assert "##ScanDropNotice" in MAIN


def test_scan_qa_bridge_is_isolated_and_clicks_only_observed_scan_controls():
    windows = (ROOT / "src/WindowsMain.cpp").read_text(encoding="utf-8")
    assert 'cmd == "scan_dialog_ui"' in windows
    bridge = windows.split('cmd == "scan_dialog_ui"', 1)[1].split('cmd == "qa_rightclick_scan"', 1)[0]
    for guard in ("#if ARIS_PREVIEW_ISOLATION", "MagicEqQaIsolationReady()",
                  "PreviewIsolationAllowsDataPathUtf8", "snapshot.frameNumber",
                  'zone.key.rfind("scan.", 0)', "s_magicEqQaPointer.pending"):
        assert guard in bridge
    assert "SendInput" not in bridge and "SetCursorPos" not in bridge


def test_reading_font_scopes_end_before_their_scan_windows():
    region = MAIN.split("static void DrawScanSimilarityDialogs", 1)[1].split("// New playlist dialog", 1)[0]
    for match in re.finditer(r"ReadingTextScope (readingScan|readingResult|readingBusy|readingDrop|reading);", region):
        suffix = region[match.end():]
        for end in re.finditer(r"ImGui::End(?:Popup)?\(\);", suffix):
            # Skip EndPopup for nested child windows, which inherit this font.
            middle = re.sub(r'"(?:\\.|[^"\\])*"|//[^\n]*', '', suffix[:end.start()])
            depth = middle.count('{') - middle.count('}')
            if depth > 0:
                continue
            assert depth < 0, match.group(1) + " crosses EndPopup/End and leaks its font stack"
            break
        else:
            raise AssertionError("No scoped window end")
