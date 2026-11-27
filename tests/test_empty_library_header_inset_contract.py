from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
MAIN = (ROOT / "src/main.cpp").read_text(encoding="utf-8")
CMAKE = (ROOT / "cmake/harken_search_behavior_tests.cmake").read_text(encoding="utf-8")
LAYOUT = (ROOT / "src/ui/ResultsPanelLayout.h").read_text(encoding="utf-8")


def _file_table_setup_block() -> str:
    start = MAIN.index("// File table with sortable columns")
    end = MAIN.index("BeginTransientScrollbarTable(resultsTableName,", start)
    return MAIN[start:end]


def test_empty_library_header_inset_contract_is_wired() -> None:
    assert "TestEmptyLibraryHeaderInsetContract" in CMAKE
    assert "test_empty_library_header_inset_contract.py" in CMAKE


def test_results_content_has_real_padding_and_no_unbalanced_indent() -> None:
    center = MAIN.split("// Center list: results* tokens", 1)[1].split(
        "// Right Column - File Info", 1
    )[0]
    assert "ImGuiChildFlags_AlwaysUseWindowPadding" in center
    assert "ImGui::Indent(0.0f)" not in center
    assert "ImGui::Unindent(4.0f)" not in center


def test_browser_table_has_an_independent_settings_id() -> None:
    setup = _file_table_setup_block()
    assert "browserFileTable = state.viewMode == ViewMode::Browser" in setup
    assert "harken::results::TableName(browserFileTable)" in setup
    assert "ImGui::GetID(resultsTableName)" in setup
    assert MAIN.count("BeginTransientScrollbarTable(resultsTableName,") == 1
    assert '"BrowserFileTableV1"' in LAYOUT
    assert '"FileTableV6"' in LAYOUT
    assert "if (browserFileTable) {" in setup
    assert "ResolveBrowserColumnDefaults(availWidth, reservedWidth)" in setup


def test_empty_and_populated_table_use_the_same_child_content_bounds() -> None:
    setup = _file_table_setup_block()
    assert "harken::results::TableOuterFlags(emptyLibraryFileTable)" in setup
    assert "adjustedFileTableOuterSize" not in setup
    assert "emptyLibraryFileTable = (state.totalFileCount == 0)" in MAIN
    assert "AlignBrowserPathLabel(emptyLibraryFileTable)" in MAIN
    assert "tableFlags, ImVec2(resultsTableOuterWidth, 0.0f), resultsTableInnerWidth" in MAIN


def test_empty_library_keeps_independent_frame_and_content_padding() -> None:
    center = MAIN[
        MAIN.index("// Center list: results* tokens") : MAIN.index(
            "// Right Column - File Info"
        )
    ]
    assert "ImVec2(harken::results::kContentInsetX, 0.0f)" in center
    assert "s_centerPanelMin.x + 0.5f" in center
    assert "s_centerPanelMax.x - 0.5f" in center
    # TASK-20261028-NO-EMPTY-SURFACE: the framed zero-result panel and its
    # height reservation were removed at the user's request; the table keeps
    # the full list area and only the independent frame/padding remains.
    assert '"##EmptyResultsSurface"' not in MAIN
    assert "showStandaloneEmptyResultsSurface" not in MAIN


if __name__ == "__main__":
    test_empty_library_header_inset_contract_is_wired()
    test_results_content_has_real_padding_and_no_unbalanced_indent()
    test_browser_table_has_an_independent_settings_id()
    test_empty_and_populated_table_use_the_same_child_content_bounds()
    test_empty_library_keeps_independent_frame_and_content_padding()
    print("PASS")
    
