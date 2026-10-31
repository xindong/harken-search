from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
MAIN = (ROOT / "src/main.cpp").read_text(encoding="utf-8")
CMAKE = (ROOT / "cmake/harken_search_behavior_tests.cmake").read_text(encoding="utf-8")


def _file_table_setup_block() -> str:
    start = MAIN.index("// File table with sortable columns")
    end = MAIN.index('BeginTransientScrollbarTable("FileTableV6"', start)
    return MAIN[start:end]


def _assignment(source: str, needle: str) -> str:
    start = source.index(needle)
    end = source.index(";", start)
    return source[start:end]


def test_empty_library_header_inset_contract_is_wired() -> None:
    assert "TestEmptyLibraryHeaderInsetContract" in CMAKE
    assert "test_empty_library_header_inset_contract.py" in CMAKE


def test_database_browser_playlist_share_one_filetablev6_surface() -> None:
    assert MAIN.count('BeginTransientScrollbarTable("FileTableV6"') == 1
    assert MAIN.count('ImGui::GetID("FileTableV6")') == 1
    center = MAIN[
        MAIN.index('BeginTransientScrollbarChild("CenterFileList"') : MAIN.index(
            "// Right Column - File Info"
        )
    ]
    assert 'BeginTransientScrollbarTable("FileTableV6"' in center
    setup = _file_table_setup_block()
    # Database-only empty-state gates precede the shared table. The table's
    # columns, border and inset must still be identical across view modes.
    setup = setup[setup.index("ImGuiTableFlags tableFlags =") :]
    assert "ViewMode::Database" not in setup
    assert "ViewMode::Browser" not in setup
    assert "ViewMode::Playlist" not in setup


def test_empty_library_header_reuses_populated_outer_border_inset() -> None:
    setup = _file_table_setup_block()
    assert "ImGuiTableFlags_BordersOuter" in setup
    assert "tableFlags &= ~ImGuiTableFlags_BordersOuter" in setup
    assert "totalFileCount == 0" in setup

    assert "constexpr float kResultsTableOuterBorder = kResultsPerimeterWidth" in setup
    assert "emptyLibraryFileTable" in setup
    assert "ImGui::SetCursorPosX(ImGui::GetCursorPosX() + kResultsTableOuterBorder)" in setup
    assert "adjustedFileTableOuterSize.x = -(kResultsTableOuterBorder * 2.0f)" in setup

    inset_start = setup.index("if (emptyLibraryFileTable) {")
    inset = setup[inset_start:]
    assert "kResultsTableOuterBorder" in inset
    assert "adjustedFileTableOuterSize.x = -(kResultsTableOuterBorder * 2.0f)" in inset
    assert "SetCursorPosX" in inset
    assert "tableFlags &= ~ImGuiTableFlags_BordersOuter" not in inset


def test_empty_library_keeps_independent_frame_and_content_padding() -> None:
    center = MAIN[
        MAIN.index("// Center list: results* tokens") : MAIN.index(
            "// Right Column - File Info"
        )
    ]
    assert "kResultsContentInsetX = " in center
    assert "ImVec2(kResultsContentInsetX, 2.0f)" in center
    assert "s_centerPanelMin.x + 0.5f" in center
    assert "s_centerPanelMax.x - 0.5f" in center
    # TASK-20261028-NO-EMPTY-SURFACE: the framed zero-result panel and its
    # height reservation were removed at the user's request; the table keeps
    # the full list area and only the independent frame/padding remains.
    assert '"##EmptyResultsSurface"' not in MAIN
    assert "showStandaloneEmptyResultsSurface" not in MAIN


if __name__ == "__main__":
    test_empty_library_header_inset_contract_is_wired()
    test_database_browser_playlist_share_one_filetablev6_surface()
    test_empty_library_header_reuses_populated_outer_border_inset()
    test_empty_library_keeps_independent_frame_and_content_padding()
    print("PASS")
    
