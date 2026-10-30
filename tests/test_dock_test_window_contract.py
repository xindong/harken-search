"""The diagnostic Dock window exposes the bounded panel-capacity test."""

from __future__ import annotations

import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MAIN = ROOT / "src" / "main.cpp"


def _read() -> str:
    return MAIN.read_text(encoding="utf-8")


class DockTestWindowContract(unittest.TestCase):
    def test_menu_entry_and_reset_exist(self) -> None:
        main = _read()
        self.assertIn("Tools_DockTestWindow", main)
        self.assertIn("Tools_ResetDockTestLayout", main)

    def test_diagnostic_panels_have_a_bounded_capacity_and_count(self) -> None:
        main = _read()
        registration = main.split('static const char* const kDockTestModuleIds', 1)[1].split('};', 1)[0]
        self.assertIn('mod-docktest-filter', registration)
        self.assertIn('mod-docktest-info', registration)
        self.assertIn('mod-docktest-12', registration)
        self.assertIn('kDockTestPanelCapacity = 12', main)
        self.assertIn('g_dockTestPanels[kDockTestPanelCapacity]', main)
        self.assertIn('DockTestActiveCount', main)
        self.assertNotIn('DockTestVisibleCount()', main)
        self.assertIn('kDockFrameModuleCapacity', main)

    def test_control_window_cannot_be_captured_by_native_dockspace(self) -> None:
        main = _read()
        start = main.index("if (g_dockTestWindowOpen) {")
        end = main.index("// STATUS BAR with Panel Toggle Buttons", start)
        controls = main[start:end]
        self.assertTrue("beginSharedDockWindowHost(" in controls, "test controls use the shared ImGui host")
        self.assertTrue("ImGuiWindowFlags_NoDocking" in controls, "test controls are excluded from native DockSpace")
        self.assertTrue("ImGuiWindowFlags_NoSavedSettings" in controls, "test controls do not persist native layout")
        self.assertIn("g_dockLayout.InsertModule", main)

    def test_reset_mirrors_real_modules_and_keeps_waveform_out_of_dock(self) -> None:
        main = _read()
        start = main.index("static void ResetDockTestLayout()")
        end = main.index("static bool OpenNextDockTestModule()", start)
        reset = main[start:end]
        self.assertTrue("ResetDockTestLayoutLeaves(g_dockLayout)" in reset,
                        "Reset delegates to the production mirror layout")
        self.assertIn('PurgeObsoleteDockTestModules(g_dockLayout);', reset)
        bridge = (ROOT / 'src' / 'ui' / 'dock' / 'DockPanelStateBridge.h').read_text(encoding='utf-8')
        purge = bridge.split('inline void PurgeObsoleteDockTestModules', 1)[1].split('inline void ResetDockTestLayoutLeaves', 1)[0]
        self.assertIn('mod-docktest-similar', purge, 'old Similar test leaves are removed')
        self.assertIn('mod-docktest-band', purge, 'old Band test leaves are removed')
        self.assertFalse("DockSide::Bottom" in reset, "Reset leaves no unused band below results")
        controls = main[main.index("if (g_dockTestWindowOpen) {"):]
        self.assertIn("g_dockTestBandHintsVisible = !g_dockTestBandHintsVisible", controls,
                      "the band button toggles a read-only target overlay")
        self.assertTrue("search-results" in reset, "Search Results remains a live module, not a duplicate test panel")
        self.assertTrue("not a separate reset mirror" in reset, "Search Results exclusion is explicit")
        self.assertFalse("mod-docktest-waveform" in main, "no Dock test claims to represent the fixed waveform")

    def test_floating_test_panels_have_a_render_and_drag_dock_path(self) -> None:
        main = _read()
        self.assertTrue("drawFloatingDockTestPanel" in main, "floating test panels have a render helper")
        self.assertTrue("if (!panel.visible || !panel.floating) return;" in main, "floating state selects the test panel render pass")
        self.assertTrue("driveFloatingDockDrag(moduleId" in main, "floating test panels share dock hit and commit handling")

    def test_control_buttons_do_not_open_empty_similar_or_band_panels(self) -> None:
        main = _read()
        controls = main[main.index("if (g_dockTestWindowOpen) {"):main.index("// STATUS BAR with Panel Toggle Buttons")]
        self.assertNotIn("OpenDockTestModule(kDockTestModuleIds[2]", controls)
        self.assertNotIn("OpenDockTestModule(kDockTestModuleIds[3]", controls)
        self.assertIn("CollectLegalDockHints", main)
        self.assertIn("g_dockTestBandHintsVisible", main)
        self.assertIn("OpenNextDockTestModule", controls)
        self.assertIn("OpenAllDockTestModules", controls)
        self.assertIn("DockTest_AddNext", controls)
        self.assertIn("DockTest_AddAll", controls)
        self.assertIn("DockTest_Capacity", controls)
        self.assertIn("DockTestActiveCount()", controls)
        reset = main[main.index("static void ResetDockTestLayout()"):main.index("static bool OpenNextDockTestModule()")]
        self.assertIn("ResetDockTestLayoutLeaves(g_dockLayout)", reset)

    def test_standalone_right_dock_leaf_keeps_width_splitter(self) -> None:
        main = _read()
        right_column = main[main.index("// Right Column - File Info with title bar"):main.index("if (g_dockTestBandHintsVisible)")]
        self.assertIn("rightDockSlotRequired && !showRightColumn", right_column)
        self.assertIn('left ? "##LeftDockLeafSplitter" : "##RightDockLeafSplitter"', right_column)
        self.assertIn("MirrorDockRootExtent(state, side, resizedRect)", right_column)
        splitter = right_column.split('for (int column = 0; column < 2; ++column) {', 1)[1]
        self.assertIn('ImGuiStyleVar_WindowBorderSize', splitter)
        self.assertIn('ImGui::IsMouseHoveringRect(', splitter,
                      'the painted seam and pointer hit must use the same rect')


if __name__ == "__main__":
    unittest.main()
