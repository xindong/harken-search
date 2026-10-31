import unittest
import json
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
MAIN = (ROOT / "src/main.cpp").read_text(encoding="utf-8")
WINDOWS_MAIN = (ROOT / "src/WindowsMain.cpp").read_text(encoding="utf-8")
HELPER = (ROOT / "src/FolderTreeContextActions.h").read_text(encoding="utf-8")
APP = (ROOT / "src/AppState.h").read_text(encoding="utf-8")
CMAKE = (ROOT / "cmake/harken_search_behavior_tests.cmake").read_text(encoding="utf-8")
LOCALIZED = {
    entry["id"]: entry
    for entry in json.loads(
        (ROOT / "locale" / "harken.ui.strings.json").read_text(encoding="utf-8")
    )["entries"]
}


class ScanPauseResumeContract(unittest.TestCase):
    def test_four_quick_menu_sites_converge_on_quick_intent(self):
        self.assertEqual(MAIN.count("MenuItem(L(Scan_QuickScan))"), 4)
        self.assertIn("bool autoStart = false", HELPER)
        self.assertIn("state.scanDialogAutoStart = autoStart", HELPER)
        self.assertNotIn("PrepareQuickScanDialogForPath(state, folder.path, true)", MAIN)
        self.assertNotIn("PrepareQuickScanDialogForPath(state, folderPath, true)", MAIN)

    def test_manual_and_internal_paths_do_not_inherit_quick_intent(self):
        self.assertIn("bool scanDialogAutoStart = false", APP)
        self.assertIn("state.scanDialogAutoStart = false", MAIN)
        self.assertIn("state.scanDialogAutoStart = false", WINDOWS_MAIN)

    def test_quick_start_uses_shared_update_worker(self):
        marker = "if (state.scanDialogAutoStart && !state.pendingScanFolders.empty())"
        self.assertIn(marker, MAIN)
        start = MAIN.index(marker)
        end = MAIN.index("if (!quickAutoStarted", start)
        block = MAIN[start:end]
        self.assertIn("startScanForFolders(state.pendingScanFolders, false)", block)
        self.assertIn("ImGui::CloseCurrentPopup()", block)

    def test_contract_is_registered_as_source_only_ctest(self):
        self.assertIn("TestScanPauseResumeContract", CMAKE)
        self.assertIn("tests/test_scan_pause_resume_contract.py", CMAKE)

    def test_shared_worker_passes_one_stop_policy_to_database(self):
        self.assertIn("&state.scanStopDisposition", MAIN)
        self.assertIn("state.requestPauseScanTask()", MAIN)
        self.assertIn("state.requestCancelScanTask()", MAIN)
        self.assertNotIn("&state.scanStopDisposition", WINDOWS_MAIN)

    def test_pause_and_cancel_are_distinct_summaries(self):
        self.assertIn("scanWasPaused", MAIN)
        self.assertIn("scanView.phase == Phase::Paused", MAIN)
        self.assertIn("L(Main_PauseKeepBtn)", MAIN)
        self.assertIn("L(ScanUi_RevokeScan)", MAIN)
        self.assertIn("L(ScanUi_Rescan)", MAIN)
        self.assertEqual(LOCALIZED["Main_PauseKeepBtn"]["zh"], "暂停并保留")
        self.assertEqual(LOCALIZED["ScanUi_RevokeScan"]["zh"], "撤销本次入库")
        self.assertIn("原文件不动", LOCALIZED["ScanUi_RevokeReminder"]["zh"])
        self.assertIn("state.lastScannedPath", MAIN)

    def test_stop_choice_does_not_add_a_third_primary_scan_button(self):
        active_start = MAIN.index("const bool showBackgroundContinueButton")
        active_end = MAIN.index("if (BeginTransientScrollbarPopupModal(", active_start)
        active_block = MAIN[active_start:active_end]
        self.assertIn('aura::ui::ActionButton(L(Main_StopBtn)', active_block)
        self.assertNotIn('aura::ui::ActionButton(L(Main_PauseKeepBtn)', active_block)
        self.assertIn('ImGui::OpenPopup((std::string(L(Scan_StopScan)) + "##ScanStopChoice").c_str())', active_block)
        self.assertIn('aura::ui::ActionButton(L(Main_PauseKeepBtn), ImVec2(0, 32.f)', MAIN)
        self.assertIn('ImGui::SameLine(0, gap)', MAIN)
        self.assertIn('aura::ui::ActionButton(L(ScanUi_RevokeScan), ImVec2(0, 32.f)', MAIN)
        self.assertIn("L(ScanUi_ContinueScan)", MAIN)
        self.assertEqual(LOCALIZED["Main_StopBtn"]["zh"], "停止…")
        self.assertEqual(LOCALIZED["Scan_StopScan"]["zh"], "停止扫描")

    def test_pause_keeps_counts_but_cancel_zeroes_them(self):
        self.assertIn("const bool rollbackCancelled", MAIN)
        finish_start = MAIN.index("auto finishInterruptedScanSession")
        rollback_start = MAIN.index("const bool rollbackOk =", finish_start)
        finish_end = MAIN.index("};", rollback_start)
        self.assertNotIn("zeroCancelledSummary();", MAIN[finish_start:rollback_start])
        self.assertIn("zeroCancelledSummary();", MAIN[rollback_start:finish_end])
        resume_start = MAIN.index("const bool resumeBackground =")
        resume_end = MAIN.index("// Full scans retain their foreground", resume_start)
        self.assertEqual(MAIN[:resume_start].count("finishInterruptedScanSession(paused);"), 2)
        resume = MAIN[resume_start:resume_end]
        before_queue, after_queue = resume.split("const auto queueSnapshot =", 1)
        self.assertEqual(before_queue.count("finishInterruptedScanSession(paused);"), 1)
        self.assertEqual(after_queue.count("finishInterruptedScanSession(paused);"), 1)
        self.assertLess(after_queue.index("finishInterruptedScanSession(paused);"),
                        after_queue.index("scanDb.discardScanRollbackSession();"))

    def test_full_scan_timeout_cannot_publish_false_completion(self):
        wait_start = MAIN.index("constexpr int64_t kCompletionWaitTimeoutMs")
        wait_end = MAIN.index("if (state.isScanTaskCancelled", wait_start)
        wait_block = MAIN[wait_start:wait_end]
        self.assertIn(
            "if (!precomputeWaveforms &&\n"
            "                                elapsedMs >= kCompletionWaitTimeoutMs)",
            wait_block,
        )
        self.assertIn(
            "if (waitTimedOut && !precomputeWaveforms &&",
            wait_block,
        )


if __name__ == "__main__":
    unittest.main()
