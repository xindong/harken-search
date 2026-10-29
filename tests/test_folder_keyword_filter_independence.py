"""Guard the actual search and folder handlers against cross-resetting filters."""

from pathlib import Path


source = (Path(__file__).resolve().parents[1] / "src/main.cpp").read_text(encoding="utf-8")
builder = (Path(__file__).resolve().parents[1] / "src/DatabaseSearchFilterBuilder.cpp").read_text(encoding="utf-8")
qa_source = (Path(__file__).resolve().parents[1] / "src/WindowsMain.cpp").read_text(encoding="utf-8")
qa_test = (Path(__file__).resolve().parents[1] / "tools/qa_folder_keyword_search.py").read_text(encoding="utf-8")


def section(start: str, end: str) -> str:
    assert start in source and end in source
    return source.split(start, 1)[1].split(end, 1)[0]


def test_search_commit_keeps_folder_scope():
    handler = section("static void ClearCoverFilterForNewSearch", "static void RequestRemoveFolderFromDatabase")
    assert "state.folderFilterActive = false" not in handler
    assert "state.selectedFolderPath.clear()" not in handler
    search_ui = section("if (!rejectInvalidSlashSearch(textToSearch))", "// 防抖定时器触发")
    assert "ClearCoverFilterForNewSearch(state)" in search_ui
    realtime = section("if (!rejectInvalidSlashSearch(debouncedSearch))", "state.syncToTab();")
    assert "ClearCoverFilterForNewSearch(state)" in realtime


def test_folder_switch_keeps_keyword_and_query_combines_both():
    folder_switch = section("// Apply a debounced pending folder switch.", "// 4a：LRU")
    assert "state.searchBuffer[0] =" not in folder_switch
    assert "state.selectedFolderPath =" in folder_switch
    assert "filter.keyword = CompileSearchBufferKeywordForFilter(state.searchBuffer)" in builder
    assert "filter.folderPath = state.folderFilterActive" in builder


def test_native_regression_uses_render_ui_transitions():
    assert 'laneKind == "functional"' in qa_source
    assert 'laneKind != "folder-search"' in qa_source
    assert 'cmd == "search_enter_key"' in qa_source
    assert 'cmd == "queue_folder_tree_selection"' in qa_source
    assert 'AURA_HARNESS_LANE_KIND="folder-search"' in qa_test
    assert '"cmd": "queue_folder_tree_selection"' in qa_test
    assert '"cmd": "search_enter_key"' in qa_test
    assert '"cmd": "set_folder_filter"' not in qa_test
