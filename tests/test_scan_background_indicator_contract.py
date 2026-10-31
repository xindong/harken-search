import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
MAIN = (ROOT / "src/main.cpp").read_text(encoding="utf-8")
APP_STATE = (ROOT / "src/AppState.h").read_text(encoding="utf-8")
CMAKE = (ROOT / "cmake/harken_search_behavior_tests.cmake").read_text(encoding="utf-8")


class ScanBackgroundIndicatorContract(unittest.TestCase):
    def test_indicator_is_registered_and_observes_existing_scan_truth(self):
        self.assertIn("TestScanBackgroundIndicatorContract", CMAKE)
        self.assertIn("tests/test_scan_background_indicator_contract.py", CMAKE)
        self.assertIn("static bool ScanActivityIndicatorVisible", MAIN)
        self.assertIn("state.isScanning.load", MAIN)
        self.assertIn("state.scanCompletionPending.load", MAIN)
        self.assertIn("ScanReadyToWorkPendingAny(state)", MAIN)

    def test_fast_scan_background_queue_keeps_indicator_visible_until_drained(self):
        start = MAIN.index("static bool ScanActivityIndicatorVisible")
        end = MAIN.index("static void DrawScanActivityIndicator", start)
        visibility = MAIN[start:end]
        self.assertIn(
            "state.scanMiniWaveformsContinueInBackground.load",
            visibility,
        )
        self.assertIn("const auto snapshot = state.backfillQueue.snapshot();", visibility)
        self.assertLess(
            visibility.index("scanMiniWaveformsContinueInBackground.load"),
            visibility.index("state.backfillQueue.snapshot()"),
        )
        self.assertIn("for (const int count : snapshot.byKind)", visibility)
        self.assertIn("queuedBackfillJobs > 0", visibility)
        self.assertIn("snapshot.inFlight > 0", visibility)
        self.assertIn("snapshot.pendingTableRows > 0", visibility)
        self.assertNotIn("countBackfillPending", visibility)
        self.assertNotIn(".store(", visibility)

    def test_pause_suppresses_indicator_before_active_or_durable_checks(self):
        start = MAIN.index("static bool ScanActivityIndicatorVisible")
        end = MAIN.index("static void DrawScanActivityIndicator", start)
        visibility = MAIN[start:end]
        self.assertIn("HarkenSearch::IsScanPause", visibility)
        self.assertIn("state.scanStopDisposition.load", visibility)
        self.assertLess(
            visibility.index("HarkenSearch::IsScanPause"),
            visibility.index("state.isScanning.load"),
        )
        self.assertLess(
            visibility.index("HarkenSearch::IsScanPause"),
            visibility.index("state.backfillQueue.snapshot()"),
        )

    def test_done_acknowledgement_keeps_pause_until_next_scan(self):
        start = MAIN.index("static void ResetScanDialogResult(")
        end = MAIN.index("void RenderUI(AppState& state)", start)
        done_acknowledgement = MAIN[start:end]
        self.assertNotIn("scanStopDisposition.store", done_acknowledgement)
        self.assertIn("scanStopDisposition.store(", APP_STATE)
        self.assertIn("HarkenSearch::ScanStopDisposition::None", APP_STATE)

    def test_cancel_rollback_clears_background_mini_waveform_indicator(self):
        start = MAIN.index("} else if (rollbackCancelled) {")
        end = MAIN.index(
            "if (state.scanDataCommitted.load(std::memory_order_acquire) || interrupted)",
            start,
        )
        rollback = MAIN[start:end]
        self.assertIn("state.scanMiniWaveformsContinueInBackground.store(", rollback)
        self.assertIn("false, std::memory_order_release", rollback)
        self.assertLess(
            rollback.index("scanMiniWaveformsContinueInBackground.store"),
            rollback.index("state.resetScanReadyToWorkState()"),
        )

    def test_indicator_is_render_only_and_uses_frame_time(self):
        start = MAIN.index("static void DrawScanActivityIndicator")
        end = MAIN.index("static bool ScanCompletionPendingAny", start)
        indicator = MAIN[start:end]
        self.assertIn("ImGui::GetWindowDrawList", indicator)
        self.assertIn("ImGui::GetTime", indicator)
        self.assertIn("AddLine", indicator)
        for forbidden in (
            "ImGui::Button",
            "ImGui::BeginPopup",
            "ImGui::SetTooltip",
            ".store(",
            "requestPause",
            "requestCancel",
            "std::thread",
        ):
            self.assertNotIn(forbidden, indicator)

    def test_bottom_status_draws_one_indicator_with_compact_real_phase_and_controls(self):
        status_start = MAIN.index("const bool statusBarScanningNow")
        status_end = MAIN.index("} else if (state.scanSimilarityOwned)", status_start)
        active_status = MAIN[status_start:status_end]
        self.assertEqual(MAIN.count("DrawScanActivityIndicator(state, statusTextH)"), 1)
        self.assertIn("DrawScanActivityIndicator(state, statusTextH)", active_status)
        self.assertNotIn("phaseLabel", active_status)
        self.assertNotIn("state.scanPhase.load", active_status)
        self.assertIn("scanView.current, scanView.total", active_status)
        self.assertIn("scanView.progressKnown", active_status)
        self.assertIn("L(ScanUi_ViewProgress)", active_status)
        self.assertIn("L(Main_StopBtn)", active_status)
        self.assertNotIn("scanAdded", active_status)
        self.assertNotIn("scanUpdated", active_status)
        self.assertIn("const bool mergeBottomStatusIntoWaveFooter = false", MAIN)

    def test_docked_waveform_transport_also_draws_indicator(self):
        transport_start = MAIN.index("const float kBarH = waveTransportH;")
        transport_end = MAIN.index("ImGui::EndChild();", transport_start)
        transport = MAIN[transport_start:transport_end]
        info_start = transport.index("ImGui::PushFont(GetUIFontEmphasis());")
        info_end = transport.index("const float dbSwitchW", info_start)
        info_bar = transport[info_start:info_end]
        self.assertIn(
            "DrawScanActivityIndicator(state, ImGui::GetTextLineHeight())",
            info_bar,
        )
        self.assertNotIn(
            "DrawScanActivityIndicatorGlyph(\n                state,\n                barDl,\n                ImVec2(topBarStatusMinX",
            transport,
        )

    def test_existing_completion_and_pause_lifetimes_remain_present(self):
        self.assertIn("BuildScanCompletionPendingLabel", MAIN)
        self.assertIn("BuildScanReadyToWorkPendingLabel", MAIN)
        self.assertIn("state.scanCompletionPending.load", MAIN)
        self.assertIn("state.scanSummaryToastUntil", MAIN)
        self.assertIn("state.requestPauseScanTask()", MAIN)
        self.assertIn("state.requestCancelScanTask()", MAIN)


if __name__ == "__main__":
    unittest.main()
