"""Dock rendering must consume one fresh geometry snapshot and float dimensions."""

from __future__ import annotations

import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MAIN = ROOT / "src" / "main.cpp"
BRIDGE = ROOT / "src" / "ui" / "dock" / "DockPanelStateBridge.h"
HIT_TEST = ROOT / "src" / "DockHitTest.h"


class DockRuntimeGeometryWiringContract(unittest.TestCase):
    def test_live_frame_and_grips_use_the_production_geometry_snapshot(self) -> None:
        main = MAIN.read_text(encoding="utf-8")
        bridge = BRIDGE.read_text(encoding="utf-8")
        hit_test = HIT_TEST.read_text(encoding="utf-8")
        self.assertTrue("BuildDockFrameGeometry" in hit_test, "production snapshot collects real root leaves")
        self.assertTrue("BuildDockFrameGeometry" in main, "the renderer consumes the snapshot")
        self.assertTrue("CollectAdjacentLeafGrips" in hit_test, "snapshot collects internal split grips")
        self.assertTrue("CollectLegacyColumnLeafGrips" in hit_test, "snapshot includes legacy column boundaries")
        self.assertTrue("gripSnapshot.grips" in main, "drawing and hit testing iterate one grip array")
        self.assertFalse("g_dockedModuleRects" in main, "cached rectangles do not feed live geometry")
        self.assertTrue("ResolveDockColumnBudget" in main, "floating leaves release legacy width budgets")

    def test_float_and_commit_paths_use_committed_leaf_geometry(self) -> None:
        main = MAIN.read_text(encoding="utf-8")
        bridge = BRIDGE.read_text(encoding="utf-8")
        self.assertTrue("BeginDockPanelFloat" in bridge, "float transition accepts the committed leaf rectangle")
        self.assertTrue("BeginDockPanelFloat" in main, "all floating paths share the float transition")
        self.assertTrue("UpdateFloatingDockPanelGeometry" in bridge, "float resize updates only the floating rectangle")
        self.assertTrue("UpdateFloatingDockPanelGeometry" in main, "float render passes record actual window size")
        self.assertTrue("PlanReadableDockDrop" in main, "re-dock validates the actual candidate tree")
        self.assertIn("MirrorDockRootExtent(state, landedSide, g_dockDropPlan.rootRect)", main,
                      "split leaf dimensions must not overwrite the root column extent")
        self.assertTrue("ApplyDockCommitToPanel" in main, "re-dock writes the split axis to the matching field")

    def test_committed_float_and_redock_share_actual_leaf_rectangles(self) -> None:
        main = MAIN.read_text(encoding="utf-8")
        self.assertTrue("PlanReadableDockDrop" in main, "redock and preview consume the same candidate leaf bounds")
        self.assertTrue("UpdateFloatingDockPanelGeometry" in main, "floating resize records actual bounds")
        self.assertTrue("drawFloatingDockTestPanel" in main, "Dock Test floats have a render path")
        self.assertTrue("g_dockCommittedThisFrame" in main, "a panel cannot render docked and floating in one frame")

    def test_side_commit_does_not_store_the_other_columns_extent(self) -> None:
        main = MAIN.read_text(encoding="utf-8")
        mirror = main.split("static void MirrorDockCommitToPanel(", 1)[1].split("static void MirrorDockRootExtent", 1)[0]
        self.assertIn("&state.panelSidebar", mirror, "the filter state owns the left column, even when filter is elsewhere")
        self.assertIn("&state.panelFileInfo", mirror, "the info state owns the right column, even when info is elsewhere")
        self.assertIn("panel.dockedWidth = mirror.dockedWidth", mirror)
        self.assertNotIn("DockSide::Right, g_dockDropPlan.slots.right", main)
        self.assertNotIn("DockSide::Left, g_dockDropPlan.slots.left", main)


if __name__ == "__main__":
    unittest.main()
