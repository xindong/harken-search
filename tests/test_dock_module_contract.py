"""Current custom Dock modules, split geometry, and right outer grip."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def read(relative: str) -> str:
    return (ROOT / relative).read_text(encoding="utf-8")


def test_four_live_ids_and_fixed_waveform_boundary():
    layout = read("src/DockLayout.h")
    main = read("src/main.cpp")
    for module_id in ('"filter"', '"search-results"', '"info"', '"similar-sounds"'):
        assert module_id in layout
    assert 'syncDockModuleVisibility("similar-sounds"' in main
    assert 'g_dockLayout.Find("waveform")' in main
    assert 'g_dockLayout.RemoveFloat("waveform")' in main
    assert 'syncDockModuleVisibility("waveform"' not in main


def test_nested_vertical_split_reaches_drag_commit():
    layout = read("src/DockLayout.cpp")
    main = read("src/main.cpp")
    assert "bool DockLayout::InsertAdjacent(" in layout
    assert "AxisForSide(side)" in layout
    assert "g_dockLayout.InsertAdjacent(hit.targetModuleId, moduleId, hit.side" in main
    assert "g_dockDrag.CommitOnMouseUp()" in main
    assert "HitDockTarget(" in main


def test_live_snapshot_drives_grip_draw_hit_and_resize():
    hit = read("src/DockHitTest.h")
    main = read("src/main.cpp")
    for collector in ("CollectLeafRects(", "CollectAdjacentLeafGrips(",
                      "CollectLegacyColumnLeafGrips(", "BuildDockFrameGeometry("):
        assert collector in hit
    assert "const aura::ui::dock::DockFrameGeometry gripSnapshot = buildDockFrameGeometry();" in main
    assert "const auto& grip = adjacentGrips[i];" in main
    assert "ImGui::InvisibleButton(gripId, ImVec2(grip.rect.width, grip.rect.height))" in main
    assert "DrawThreeDotSplitterGrip(" in main
    assert "DragAdjacentLeafGrip(g_dockLayout" in main


def test_single_similar_right_keeps_width_grip_without_fake_leaf():
    hit = read("src/DockHitTest.h")
    main = read("src/main.cpp")
    assert "PlanRightDockOuterGrip(" in hit
    assert "DragRightDockOuterGrip(" in hit
    assert 'PlanDockColumnOuterGrip(gripSnapshot, aura::ui::dock::DockSide::Right' in main
    assert '"##RightDockLeafSplitterHit"' in main
    assert "state.panelSimilarSounds.dockedWidth" in main
    assert "!showRightColumn" in main


def test_swapped_columns_keep_both_outer_width_grips():
    main = read("src/main.cpp")
    assert "leftDockSlotRequired && !showLeftColumn" in main
    assert "PlanDockColumnOuterGrip(gripSnapshot, aura::ui::dock::DockSide::Left" in main
    assert "PlanDockColumnOuterGrip(gripSnapshot, aura::ui::dock::DockSide::Right" in main
    assert "MirrorDockRootExtent(state, side, resizedRect)" in main


def test_band_hint_is_read_only_and_retired_blank_modules_are_purged():
    main = read("src/main.cpp")
    hit = read("src/DockHitTest.h")
    layout = read("src/DockLayout.h")
    assert '"mod-docktest-similar"' not in main
    assert '"mod-docktest-band"' not in main
    assert "PurgeObsoleteDockTestModules(g_dockLayout)" in main
    assert "CollectLegalDockHints(" in main
    assert "HitDockTarget(probe, work, snapDistance" in hit
    assert "for (DockSide side : {DockSide::Left, DockSide::Right})" in hit
    assert 'id.find("mod-") == 0' in layout
