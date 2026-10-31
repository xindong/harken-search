from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
MAIN = (ROOT / "src/main.cpp").read_text(encoding="utf-8")
CMAKE = (ROOT / "cmake/harken_search_behavior_tests.cmake").read_text(encoding="utf-8")


def _empty_results_block() -> str:
    start = MAIN.index("// UX #109: Empty state")
    end = MAIN.index("// Apply per-tab scroll restoration", start)
    return MAIN[start:end]


def _file_table_surface_block() -> str:
    start = MAIN.index("// File table with sortable columns")
    end = MAIN.index("// F-034 Relink", start)
    return MAIN[start:end]


def _search_status_block() -> str:
    start = MAIN.index("// 物理隔离：状态区固定 75px 占位")
    end = MAIN.index("// F-034: Column view preset dial", start)
    return MAIN[start:end]


def test_zero_results_do_not_render_a_result_layer_message() -> None:
    assert "TestEmptyResultsPresetOverlapContract" in CMAKE
    block = _empty_results_block()
    assert "TableNextRow(0, 64.0f)" not in block
    assert "RenderNoDatabaseSetupEmptyState(state)" not in block
    assert "Empty_NoResults" not in block


def test_zero_results_do_not_render_a_search_toolbar_message() -> None:
    block = _search_status_block()
    assert 'TextDisabled(u8"无结果")' not in block
    assert "totalFileCount == 0" not in block


def test_zero_results_do_not_render_a_toolbar_glass_tag() -> None:
    assert "drawToolbarGlassTag(L(Search_NoResult)" not in MAIN


def test_database_setup_guidance_stays_outside_the_result_layer() -> None:
    block = _empty_results_block()
    assert "Empty_NoFiles" not in block


def test_database_setup_guidance_is_not_duplicated_behind_the_modal() -> None:
    block = _file_table_surface_block()

    assert "state.startupNeedsDatabaseSetup && !state.database.isOpen()" in block
    assert "RenderNoDatabaseSetupEmptyState(state)" not in block


def test_no_dedicated_zero_result_surface_is_reserved() -> None:
    # TASK-20261028-NO-EMPTY-SURFACE: the user removed the framed
    # "no results" panel and its 180px reservation; the results table
    # occupies the full list area even with zero rows.
    surface = _file_table_surface_block()
    assert '"##EmptyResultsSurface"' not in MAIN
    assert "kEmptyResultsSurfaceHeight" not in MAIN
    assert "kStandaloneEmptyResultsSurfaceHeight" not in MAIN
    assert "BeginTransientScrollbarChild(\n            \"##EmptyResultsSurface\"" not in surface


def test_zero_result_list_still_keeps_the_player_footer_below() -> None:
    assert MAIN.index("// UX #109: Empty state") < MAIN.index(
        "// INDEPENDENT PLAYER FOOTER CONTROL STRIP"
    )


if __name__ == "__main__":
    test_zero_results_do_not_render_a_result_layer_message()
    test_zero_results_do_not_render_a_search_toolbar_message()
    test_zero_results_do_not_render_a_toolbar_glass_tag()
    test_database_setup_guidance_stays_outside_the_result_layer()
    test_database_setup_guidance_is_not_duplicated_behind_the_modal()
    test_no_dedicated_zero_result_surface_is_reserved()
    test_zero_result_list_still_keeps_the_player_footer_below()
    print("PASS")
