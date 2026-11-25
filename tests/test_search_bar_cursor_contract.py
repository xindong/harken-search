from pathlib import Path


MAIN = Path(__file__).resolve().parents[1] / "src" / "main.cpp"
CONTROLS = MAIN.parent / "ui" / "ToolbarSearchControls.h"


def _search_bar_region() -> str:
    source = MAIN.read_text(encoding="utf-8")
    start = source.index("const float searchY")
    end = source.index("const float searchAndHistoryRightX", start)
    return source[start:end]


def test_search_bar_uses_native_caret_without_redundant_custom_overlay():
    region = _search_bar_region()
    controls = CONTROLS.read_text(encoding="utf-8")
    assert 'DrawSearchField("##SearchBar"' in region
    assert 'ImGui::InputTextEx("##in"' in controls
    assert "ImGuiCol_InputTextCursor, palette.text" in controls
    assert "focusPulse" not in region + controls


def test_search_and_history_keep_the_existing_workflow():
    main = MAIN.read_text(encoding="utf-8")
    start = main.index("const float kHistoryButtonWidth")
    end = main.index("const float searchAndHistoryRightX", start)
    toolbar = main[start:end]
    assert 'RegisterLastItem("toolbar.back"' in toolbar
    assert 'RegisterLastItem("toolbar.continue"' in toolbar
    assert "state.viewStateBack()) { RefreshSearchResults(state, true);" in toolbar
    assert "state.viewStateForward()) { RefreshSearchResults(state, true);" in toolbar
    assert "L(Main_NavBackEndTip)" in toolbar and "L(Main_NavFwdEndTip)" in toolbar
    assert "const bool searchFocusRequested = state.searchSuggestRequestFocus || state.focusSearchBar" in toolbar
    assert "g_searchBarFocusedForShortcuts = searchFocused" in toolbar
    assert "state.searchExactModeLocked = !state.searchExactModeLocked" in toolbar
    assert "forceApply = true" in toolbar
    assert "L(Main_SearchInputHint)" in toolbar and "L(Main_SearchTriggerFmt)" in toolbar
    assert "bool typedChanged = (currentSearch != lastTypedSearchText)" in main
    assert "state.searchDebounce.trigger()" in main


def test_search_pill_only_has_a_focus_outline_and_owns_the_help_region():
    controls = CONTROLS.read_text(encoding="utf-8")
    search = controls.split("inline SearchFieldResult DrawSearchField(", 1)[1]
    assert "palette.border" not in search
    assert "GetItemRectMin(), searchRectMax" in _search_bar_region()
    assert "if (HoveredForHelp())" in _search_bar_region()


def test_explicit_clear_forces_apply_even_when_realtime_is_turned_off():
    region = _search_bar_region()
    mode_change = region.index("if (lastSearchRealtime != state.settings.searchRealtime)")
    assert "forceApply = search.cleared;" in region[:mode_change]
    assert "forceApply = forceApply || state.settings.searchRealtime;" in region[mode_change:]
    assert "bool forceCommit = forceApply || state.searchSuggestForceApply;" in MAIN.read_text(encoding="utf-8")


def test_history_always_uses_the_reference_compact_icon_layout():
    main = MAIN.read_text(encoding="utf-8")
    start = main.index("const float kHistoryButtonWidth")
    history = main[start : main.index('DrawSearchField("##SearchBar"', start)]
    assert "const float kHistoryButtonWidth = workspacePx(46.0f);" in history
    assert "const float kHistoryInnerPad = workspacePx(3.0f);" in history
    assert "const float kHistoryDividerGap = workspacePx(2.0f);" in history
    assert "2.0f * (kHistoryButtonWidth + kHistoryInnerPad + kHistoryDividerGap) + workspacePx(1.0f)" in history
    assert "const float kCapsuleH = workspacePx(34.0f);" in main
    assert "ImGui::Dummy(ImVec2(kHistoryCapsuleWidth, capsuleH))" in history
    assert 'InvisibleButton("##NavBack", ImVec2(kHistoryButtonWidth, innerH))' in history
    assert 'InvisibleButton("##NavFwd", ImVec2(kHistoryButtonWidth, innerH))' in history
    assert "backMin.x + 16.0f" in history and "fwdMax.x - 18.0f" in history
    assert "dl->AddText(" not in history


def test_narrow_search_places_history_to_its_right_and_reserves_actual_width():
    main = MAIN.read_text(encoding="utf-8")
    assert "resultsHomeLeftScreenX < leadingHistoryX + kHistoryCapsuleWidth + searchHistoryGap" in main
    assert "historyAfterSearch ? kHistoryCapsuleWidth + searchHistoryGap : 0.0f" in main
    assert "? resultsHomeLeftScreenX + searchWidth + searchHistoryGap : leadingHistoryX" in main
    assert "toolbarFlowInput.navMinX = searchAndHistoryRightX + toolbarNavMinGap" in main


def test_browser_and_settings_reuse_filter_variant_without_precise_mode():
    main = MAIN.read_text(encoding="utf-8")
    settings = (MAIN.parent / "SettingsDialog.inl").read_text(encoding="utf-8")
    assert 'DrawSearchField("##BrowserSearch", L(Search_FileHint),' in main
    assert "state.browserFilterBuffer, sizeof(state.browserFilterBuffer), filterLayout.searchWidth, palette" in main
    assert 'DrawSearchField("##settings-search", L(SetUX_SearchSettingsCtrlK),' in settings
    assert "searchOptions.requestFocus = ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_K)" in settings
    assert "if (search.active) editor.recording=false;" in settings
    assert "searchOptions.allowPrecise" not in settings
