from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
MAIN = (ROOT / "src/main.cpp").read_text(encoding="utf-8")
CMAKE = (ROOT / "cmake/harken_search_behavior_tests.cmake").read_text(encoding="utf-8")


def _file_table_surface_block() -> str:
    start = MAIN.index("// File table with sortable columns")
    end = MAIN.index("// F-034 Relink", start)
    return MAIN[start:end]


def test_empty_list_inner_chrome_contract_is_wired() -> None:
    assert "TestEmptyListInnerChromeContract" in CMAKE


def test_zero_result_lists_reserve_no_standalone_empty_surface() -> None:
    # TASK-20261028-NO-EMPTY-SURFACE: the user removed the framed
    # "no results" panel. The results table keeps its full height and no
    # bounded empty-state child is drawn anywhere in the list column.
    surface = _file_table_surface_block()
    for symbol in (
        "showEmptyResultsSurface",
        "showStandaloneEmptyResultsSurface",
        "kEmptyResultsSurfaceHeight",
        "kStandaloneEmptyResultsSurfaceHeight",
        '"##EmptyResultsSurface"',
    ):
        assert symbol not in MAIN


def test_zero_result_table_keeps_full_height() -> None:
    surface = _file_table_surface_block()
    assert "ImVec2 adjustedFileTableOuterSize = ImVec2(0.0f, 0.0f);" in surface


def test_truly_empty_list_does_not_keep_table_outer_border() -> None:
    surface = _file_table_surface_block()
    flags_start = surface.index("ImGuiTableFlags tableFlags")
    table_start = surface.index('BeginTransientScrollbarTable("FileTableV6"')
    flags = surface[flags_start:table_start]
    assert "ImGuiTableFlags_BordersOuter" in flags
    assert "totalFileCount == 0" in flags


def test_zero_result_layer_renders_no_guidance_messages() -> None:
    surface = _file_table_surface_block()
    for message in (
        "Empty_FilterAndSearch",
        "Empty_FilterOnly",
        "Empty_SearchOnly",
        "Empty_NoResults",
        "Empty_NoFiles",
    ):
        assert message not in surface


if __name__ == "__main__":
    test_empty_list_inner_chrome_contract_is_wired()
    test_zero_result_lists_reserve_no_standalone_empty_surface()
    test_zero_result_table_keeps_full_height()
    test_truly_empty_list_does_not_keep_table_outer_border()
    test_zero_result_layer_renders_no_guidance_messages()
    print("PASS")
