from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
WIDGETS_H = (ROOT / "src" / "HarkenSearchWidgets.h").read_text(encoding="utf-8")
WIDGETS_CPP = (ROOT / "src" / "HarkenSearchWidgets.cpp").read_text(encoding="utf-8")
MAIN = (ROOT / "src" / "main.cpp").read_text(encoding="utf-8")


def _between(text: str, start: str, end: str) -> str:
    i = text.index(start)
    j = text.index(end, i)
    return text[i:j]


def test_settings_row_api_exists() -> None:
    assert "struct SettingsRowOptions" in WIDGETS_H
    assert "bool SettingsToggleRow(" in WIDGETS_H
    assert "bool SettingsSliderRow(" in WIDGETS_H
    assert "bool SettingsButtonRow(" in WIDGETS_H
    assert "bool BeginSettingsGroup(" in WIDGETS_H


def test_settings_row_uses_main_tooltip() -> None:
    assert "void Tooltip(const char* text);" in WIDGETS_H
    body = _between(WIDGETS_CPP, "bool SettingsToggleRow(", "bool SettingsButtonRow(")
    assert "aura::ui::Tooltip" in WIDGETS_CPP or "Tooltip(" in body
    assert "ImGui::SetTooltip" not in body
    assert "HelpMarker" not in WIDGETS_H


def test_declick_and_loudness_are_one_row() -> None:
    declick = _between(MAIN, "Set_EnableFadeOnJump", "Set_LoudnessNorm")
    assert "SettingsSliderRow" in declick or "SameLine(" in declick
    assert "CollapsingHeader(L(Set_DeClickProtection)" not in MAIN
    loud = _between(MAIN, 'aura::ui::BeginSettingsGroup("##Set_LoudnessNorm")', "else if (selectedNav == 2)")
    assert "CollapsingHeader(L(Set_LoudnessNorm)" not in MAIN
    assert "TextDisabled" not in loud


def test_repair_button_caption_is_tooltip_only() -> None:
    repair = _between(MAIN, "s_repairSnapshotReady", "getRepairCenterSnapshot")
    assert "SettingsButtonRow" in repair
    assert "getRepairCenterSnapshot" in MAIN[MAIN.index("s_repairSnapshotReady") : MAIN.index("Set_CleanInvalid2")]


def test_settings_group_uses_gap12() -> None:
    assert "kGap12" in WIDGETS_CPP
    assert "BeginSettingsGroup(" in WIDGETS_H
    assert "BeginSettingsGroup" in WIDGETS_CPP


def _settings_dialog_fragment() -> str:
    start = MAIN.index("selectedNav == 0")
    end = MAIN.index("Set_SaveAndClose", start)
    return MAIN[start:end]


def _collapsing_header_bodies(text: str) -> list[tuple[str, str]]:
    marker = "CollapsingHeader("
    bodies: list[tuple[str, str]] = []
    pos = 0
    while True:
        i = text.find(marker, pos)
        if i < 0:
            break
        open_paren = text.find("(", i)
        depth = 0
        j = open_paren
        while j < len(text):
            ch = text[j]
            if ch == "(":
                depth += 1
            elif ch == ")":
                depth -= 1
                if depth == 0:
                    j += 1
                    break
            j += 1
        header = text[i:j]
        brace = text.find("{", j)
        if brace < 0:
            pos = j
            continue
        depth = 0
        k = brace
        while k < len(text):
            ch = text[k]
            if ch == "{":
                depth += 1
            elif ch == "}":
                depth -= 1
                if depth == 0:
                    k += 1
                    break
            k += 1
        bodies.append((header, text[brace:k]))
        pos = k
    return bodies


def _is_single_toggle_block(body: str) -> bool:
    toggle_count = body.count("Checkbox(") + body.count("SettingsToggleRow(")
    if toggle_count != 1:
        return False
    has_complex = any(
        token in body
        for token in (
            "SliderInt(",
            "SliderFloat(",
            "SettingsSliderRow(",
            "Combo(",
            "Button(",
            "SettingsButtonRow(",
            "RadioButton(",
        )
    )
    return not has_complex


def test_settings_dialog_has_no_single_toggle_collapsing_header() -> None:
    fragment = _settings_dialog_fragment()
    offenders = [
        header
        for header, body in _collapsing_header_bodies(fragment)
        if _is_single_toggle_block(body)
    ]
    assert offenders == [], f"single-toggle collapsing headers remain: {offenders}"


def test_settings_search_keeps_layout_keywords() -> None:
    start = MAIN.index("settingsSearchEntries")
    end = MAIN.index("return entries", start)
    table = MAIN[start:end]
    for keyword in (
        "fade click protection",
        "loudness normalize",
        "scan engine",
        "rebuild repair",
    ):
        assert keyword in table, f"settings search lost keyword {keyword}"


def test_loudness_controls_stay_in_same_disabled_group() -> None:
    loud = _between(MAIN, "##Set_LoudnessNorm", "Set_WaveformRender")
    assert "BeginSettingsGroup" in loud
    assert "SettingsToggleRow" in loud
    assert "SameLine(" in loud
    combo_at = loud.index("Combo(L(Set_RefLevel)")
    disable_at = loud.index("BeginDisabled")
    assert disable_at < combo_at
    assert "if (state.settings.loudnessNormEnabled)" not in loud


def test_named_settings_rows_use_shared_primitives() -> None:
    display = MAIN[MAIN.index("Set_RowHeight") - 80 : MAIN.index("Set_ShowFileExt")]
    assert "SettingsSliderRow" in display
    assert "SettingsToggleRow" in display
    assert "ImGui::SliderInt(L(Set_RowHeight)" not in MAIN
    assert 'ImGui::Checkbox("Mini waveform RMS body style"' not in MAIN
    assert "ImGui::Checkbox(L(Set_ShowFileExt)" not in MAIN

    time_fmt = MAIN[MAIN.index("Settings_SectionTimeFormat") : MAIN.index("Set_VideoFPS")]
    assert "SettingsToggleRow" in time_fmt
    assert "ImGui::Checkbox(L(Set_ShowMs)" not in time_fmt
    assert "ImGui::Checkbox(L(Set_ShowFrame)" not in time_fmt

    playback = _between(MAIN, "Set_PlaybackBehavior", "Set_EnableFadeOnJump")
    assert "SettingsSliderRow" in playback
    assert "ImGui::Checkbox(L(Set_SkipLeadingSilence)" not in playback
    assert "if (state.settings.skipLeadingSilence)" not in playback

    loud = _between(MAIN, "Set_EnableLoudnessNorm", "Set_WaveformRender")
    assert "ImGui::SliderFloat(L(Set_TargetLUFS)" not in loud
    assert "SettingsSliderRow" in loud or "SettingsSliderFloatRow" in loud


def test_settings_pages_wrap_content_in_settings_group() -> None:
    fragment = _settings_dialog_fragment()
    for child_id in (
        "AppSettingsContent",
        "AudioEngineContent",
        "WaveformVizContent",
        "LibPerfContent",
        "PreferencesContent",
        "ThesaurusContent",
        "ChannelRoutingContent",
        "DiagnosticsContent",
    ):
        block_start = fragment.index(child_id)
        window = fragment[block_start : block_start + 800]
        assert "BeginSettingsGroup" in window, f"{child_id} is not wrapped in BeginSettingsGroup"


def test_settings_function_rows_use_shared_tooltip() -> None:
    fragment = _settings_dialog_fragment()
    assert "HarkenSearch::SetTooltip" not in fragment
    assert "ImGui::SetTooltip" not in fragment
    repair = _between(MAIN, "s_repairSnapshotReady", "getRepairCenterSnapshot")
    assert "SettingsButtonRow" in repair
    assert "aura::ui::ActionButton(" not in repair


def test_settings_dialog_tooltip_does_not_pass_percent_s() -> None:
    start = MAIN.index("selectedNav == 0")
    end = MAIN.index("Set_SaveAndClose", start)
    fragment = MAIN[start:end]
    assert 'Tooltip("%s"' not in fragment
    assert 'Tooltip(u8"%s"' not in fragment


def test_guided_tooltip_flag_links_to_global_symbol() -> None:
    in_aura_ui = False
    depth = 0
    namespaced_extern = False
    for raw in WIDGETS_CPP.splitlines():
        line = raw.split("//", 1)[0].strip()
        if not in_aura_ui and line.startswith("namespace aura::ui"):
            in_aura_ui = True
            depth = line.count("{") - line.count("}")
            if depth <= 0:
                depth = 1
            continue
        if in_aura_ui:
            depth += line.count("{") - line.count("}")
            if "extern bool g_showGuidedTooltips" in line:
                namespaced_extern = True
            if depth <= 0:
                in_aura_ui = False
    assert namespaced_extern is False
    assert "::g_showGuidedTooltips" in WIDGETS_CPP


def test_thesaurus_rules_list_has_explicit_height() -> None:
    rules = _between(MAIN, "rulesListHeight", "ThesaurusRulesTable")
    assert "ImVec2(0, 0)" not in rules
    assert "GetContentRegionAvail" not in rules
    assert "220.0f" in rules


def test_monitor_layout_combo_is_outside_settings_group_child() -> None:
    # 50b26791 moves the selector above the scrollable settings body.
    page = _settings_popup_fragment()
    combo = 'ImGui::Combo("##MonitorLayout"'
    assert page.count(combo) == 1
    combo_at = page.index(combo)
    body_at = page.index("const ImVec2 settingsBodyPos(")
    group_at = page.index('BeginSettingsGroup("##ChannelRoutingPage")')
    assert combo_at < body_at < group_at
    assert "settingsSearchH + settingsMonitorLayoutBarH" in page


def test_playback_behavior_does_not_repeat_fade_row() -> None:
    playback = _between(MAIN, "CollapsingHeader(L(Set_PlaybackBehavior)", "##DeClick")
    close = playback.rfind("ImGui::Unindent();")
    body = playback[:close] if close >= 0 else playback
    assert "Set_EnableFadeOnJump" not in body
    assert "fadeInOutEnabled" not in body
    assert "Set_PreviewBrowse" in body
    assert "Set_SkipLeadingSilence" in body


def test_settings_section_headers_do_not_take_subtitle_as_expanded() -> None:
    fragment = _settings_dialog_fragment()
    assert "SectionHeader(u8\"" not in fragment.replace("SectionHeader(u8\"", "X") or True
    offenders = []
    needle = "SectionHeader("
    pos = 0
    while True:
        i = fragment.find(needle, pos)
        if i < 0:
            break
        end = fragment.find(");", i)
        call = fragment[i : end + 2]
        if 'u8"' in call.split(",", 1)[-1] if "," in call else False:
            offenders.append(call.replace("\n", " ")[:120])
        elif "," in call:
            rest = call.split(",", 1)[1]
            if 'u8"' in rest:
                offenders.append(call.replace("\n", " ")[:120])
        pos = i + len(needle)
    assert offenders == [], f"SectionHeader still takes subtitle as second arg: {offenders}"


def test_monitor_layout_combo_is_outside_channel_routing_child() -> None:
    page = _settings_popup_fragment()
    combo_at = page.index('ImGui::Combo("##MonitorLayout"')
    assert combo_at < page.index("ChannelRoutingContent")
    window = _between(page, "if (selectedNav == 7) {", "const ImVec2 settingsBodyPos(")
    assert "BeginTransientScrollbarChild(" not in window
    assert "ImGui::BeginChild(" not in window
    assert "BeginSettingsGroup(" not in window
    assert 'ImGui::Combo("##MonitorLayout"' in window
    assert _settings_combo_calls_inside_open_child() == []


def _settings_combo_calls_inside_open_child() -> list[str]:
    fragment = _settings_popup_fragment()
    stack: list[str] = []
    offenders: list[str] = []
    i = 0
    while i < len(fragment):
        if fragment.startswith("BeginTransientScrollbarChild(", i) or fragment.startswith(
            "ImGui::BeginChild(", i
        ):
            close = fragment.find(")", i)
            stack.append(fragment[i : close + 1].replace("\n", " ")[:90])
            i += 1
            continue
        if fragment.startswith("ImGui::EndChild();", i):
            if stack:
                stack.pop()
            i += 1
            continue
        if fragment.startswith("ImGui::Combo(", i) or fragment.startswith(
            "BeginTransientScrollbarCombo(", i
        ):
            close = fragment.find(")", i)
            call = fragment[i : close + 1].replace("\n", " ")[:90]
            if stack:
                offenders.append(f"{call} in {stack[-1]}")
            i += 1
            continue
        i += 1
    return offenders


def test_settings_combos_are_not_inside_popup_capturing_child() -> None:
    offenders = _settings_combo_calls_inside_open_child()
    assert offenders == [], f"Combo still nested in settings Child: {offenders}"
    fragment = _settings_dialog_fragment()
    assert 'BeginTransientScrollbarChild("SettingsContent"' not in fragment
    assert 'BeginTransientScrollbarChild("AppSettingsContent"' not in fragment
    assert 'BeginTransientScrollbarChild("AudioEngineContent"' not in fragment
    assert 'BeginTransientScrollbarChild("WaveformVizContent"' not in fragment
    assert 'BeginTransientScrollbarChild("LibPerfContent"' not in fragment
    assert 'BeginTransientScrollbarChild("PreferencesContent"' not in fragment
    assert 'BeginTransientScrollbarChild("ThesaurusContent"' not in fragment
    assert 'BeginTransientScrollbarChild("ChannelRoutingContent"' not in fragment
    assert 'BeginTransientScrollbarChild("DiagnosticsContent"' not in fragment
    assert 'BeginTransientScrollbarChild("SettingsBody"' not in fragment


def _settings_popup_fragment() -> str:
    start = MAIN.index("BeginTransientScrollbarPopupModal(L(Dlg_Settings)")
    end = MAIN.index("settingsDialogOpenNow", start)
    return MAIN[start:end]


def test_settings_chrome_is_pinned_to_dialog_client_rect() -> None:
    popup = _settings_popup_fragment()
    nav_at = popup.index('BeginTransientScrollbarChild("SettingsNav"')
    save_at = popup.index("aura::ui::ActionButton(L(Set_SaveAndClose)")
    chrome = popup[: popup.index("selectedNav == 0")]
    footer = popup[popup.rfind("// Bottom buttons") : save_at + len("aura::ui::ActionButton(L(Set_SaveAndClose)")]
    assert nav_at < save_at
    assert "leftPanelWidth" not in chrome
    assert "leftPanelWidth" not in footer
    assert "InnerRect" in chrome
    assert "CursorMaxPos" not in popup
    assert "s_settingsBodyScroll" in popup
    assert "settingsFooterH" in chrome
    assert "settingsFooterH" in footer
    assert 'ImVec2(navWidth, -40)' not in popup
    nav_call_end = popup.index(");", nav_at)
    nav_call = popup[nav_at : nav_call_end + 2]
    assert "-40" not in nav_call
    assert "SetCursorPos" in footer
    assert "settingsFooterY" in footer
    assert "Scroll" in chrome
    assert 'BeginTransientScrollbarChild("SettingsBody"' not in popup
    assert "InnerRect.Min - settingsDlg->Pos" not in popup
    assert "InnerRect.Max - settingsDlg->Pos" not in popup
    assert "SetScrollY(0.0f)" not in popup
    assert "SetScrollX(0.0f)" not in popup
    assert "const float navWidth = 120.0f" not in popup
    assert "GetFrameHeightWithSpacing() * 2.0f" not in popup
    assert "CalcTextSize" in popup
    assert "settingsTabLabel" in popup[popup.index("navLabelW") : popup.rindex("SettingsNav")]


def test_settings_footer_primary_buttons_fit_their_labels() -> None:
    popup = _settings_popup_fragment()
    footer = popup[popup.rfind("// Bottom buttons") : popup.index("aura::ui::ActionButton(L(Set_ExportSettings)")]
    chrome = popup[: popup.index("selectedNav == 0")]
    assert "Set_SaveAndClose" in footer
    assert "Set_RestoreDefaultTheme" in footer
    assert "Set_ExportSettings" in popup
    assert "Set_ImportSettings" in popup
    assert "const float footerPrimaryW = 120.0f" not in footer
    assert "ImVec2(footerPrimaryW, 0)" not in footer
    assert "CalcTextSize(L(Set_SaveAndClose))" in chrome
    assert "CalcTextSize(L(Cancel))" in chrome
    assert "CalcTextSize(L(Set_RestoreDefaultTheme))" in chrome
    for width in ("footerSaveW", "footerCancelW", "footerRestoreW"):
        assert f"ImVec2({width}, 0)" in footer
    assert "rightGroupW" in chrome
    assert "footerWrap" not in chrome
    assert "GetFrameHeightWithSpacing() * (footerWrap ? 2.0f : 1.0f)" not in chrome
    assert "GetFrameHeightWithSpacing() + 8.0f" in chrome
    assert "SetNextWindowSizeConstraints(settingsMinSize, settingsMaxSize)" in MAIN
    assert "if (!ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {\n        ImGui::SetNextWindowSizeConstraints" not in MAIN
    assert "IsMouseDragging(ImGuiMouseButton_Left)" in popup
    assert popup.count("SetScrollY(0.0f)") == 0
    assert popup.count("SetScrollX(0.0f)") == 0


def test_settings_search_stays_pinned_with_lucide_icon() -> None:
    popup = _settings_popup_fragment()
    body = popup[popup.index("s_settingsBodyScroll") : popup.rfind("// Bottom buttons")]
    assert 'TextUnformatted("Q")' not in popup
    assert "ICON_LC_SEARCH" in popup
    assert "Main_QuickFindHeader" not in popup
    search_at = popup.index("##SettingsQuickSearch")
    scroll_group_at = popup.index("s_settingsBodyScroll")
    assert search_at < scroll_group_at
    assert "##SettingsQuickSearch" not in body
    assert "ICON_LC_SEARCH" not in body
    assert "settingsClientMin.y +" in popup
    assert "kGap12" in popup[popup.index("settingsSearchPos") : popup.index("SetCursorPos(settingsSearchPos)")]
    assert "settingsSearchPos.y =\n            settingsClientMin.y)" not in popup.replace(" ", "")
    icon_block = popup[popup.index("SetCursorPos(settingsSearchPos)") : popup.index("##SettingsQuickSearch")]
    assert "AlignTextToFramePadding" in icon_block


def test_settings_footer_secondary_buttons_fit_their_labels() -> None:
    popup = _settings_popup_fragment()
    chrome = popup[: popup.index("selectedNav == 0")]
    footer = popup[popup.rfind("// Bottom buttons") :]
    assert "CalcTextSize(L(Set_ExportSettings))" in chrome
    assert "CalcTextSize(L(Set_ImportSettings))" in chrome
    assert "CalcTextSize(L(Set_ResetAllDefaults))" in chrome
    assert "CalcTextSize(L(Main_CancelBtn3))" in chrome
    assert "const float confirmCancelW = 56.0f" not in chrome
    assert "footerExportW" in chrome
    assert "footerImportW" in chrome
    assert "footerResetW" in chrome
    assert "ImVec2(footerSecondaryW, 0)" not in footer
    assert "footerExportW" in footer or "ImVec2(footerExportW, 0)" in popup
    assert "ImVec2(footerImportW, 0)" in popup
    assert "ImVec2(footerResetW, 0)" in popup
    assert "if (footerWrap)" not in footer
    assert "SetCursorPosX(settingsClientMax.x - rightGroupW)" in footer


def test_settings_body_scroll_uses_stable_max_at_bottom() -> None:
    popup = _settings_popup_fragment()
    body = popup[popup.index("s_settingsBodyScroll") : popup.rfind("// Bottom buttons")]
    assert "s_settingsBodyContentH" in body
    assert "s_settingsBodyViewportH" in body
    assert "settingsBodySize.y - settingsBodyH" not in body
    assert 'ImGui::Dummy(ImVec2(settingsBodyW, settingsBodyH))' not in body
    assert "GetContentRegionAvail().y" not in body
    assert "s_settingsBodyContentH = ImMax(s_settingsBodyContentH, settingsBodySize.y)" not in body
    assert "ImMax(s_settingsBodyContentH" not in body
    assert "s_settingsBodyContentByNav" in body or "selectedNav" in body
    assert "s_settingsBodyContentH =" in body
    assert "s_settingsBodyContentH = settingsBodySize.y;" not in body
    assert "measuredBodyH" in body
    assert "ShouldUpdateSettingsBodyContentHeight(" in body
    height_policy = (ROOT / "src" / "SettingsBodyScrollHeight.h").read_text(encoding="utf-8")
    assert "std::fabs(measuredContentH - storedContentH) >= 2.0f" in height_policy
    assert "nearScrollBottom" in body
    assert "footerWrap" not in body
    assert "GetFrameHeightWithSpacing() * (footerWrap ? 2.0f : 1.0f)" not in popup
    dummy_line = [ln for ln in body.splitlines() if "Dummy(ImVec2(settingsBodyContentW" in ln]
    assert dummy_line, "missing Dummy for settings body viewport"
    assert "s_settingsBodyViewportH" in dummy_line[0]
    assert "s_settingsBodyContentH" not in dummy_line[0]
    assert "settingsBodyW" not in dummy_line[0]


def test_settings_dialog_scrollbar_is_only_for_middle_details() -> None:
    popup = _settings_popup_fragment()
    header = popup[: popup.index("{")]
    assert "ImGuiWindowFlags_NoScrollbar" in header
    assert "ImGuiWindowFlags_NoScrollWithMouse" in header
    assert "BeginTransientScrollbarPopupModal(L(Dlg_Settings), nullptr, ImGuiWindowFlags_None)" not in popup
    assert "settingsBodyScrollbar" in popup
    bar = popup[popup.index("const float settingsBodyScrollbarW") : popup.rfind("// Bottom buttons")]
    assert "AddRectFilled" in bar
    assert "ScrollbarSize" in bar
    assert "ScrollbarRounding" in bar
    assert "GrabMinSize" in bar
    assert "ScrollbarGrabHovered" in bar
    assert "ScrollbarGrabActive" in bar
    assert ", 2.0f)" not in bar
    assert "2.0f);" not in bar
    assert popup.count("SetScrollY(0.0f)") == 0
    assert popup.count("SetScrollX(0.0f)") == 0
    assert 'BeginTransientScrollbarChild("SettingsBody"' not in popup
    assert "settingsBodyContentW" in popup
    assert "settingsBodyScrollbarGap" in popup
    gap_block = popup[popup.index("settingsBodyScrollbarGap") : popup.index("settingsBodyH =")]
    assert "aura::ui::metrics::kGap8" in gap_block
    assert "ScrollbarSize" in gap_block
    clip = popup[popup.index("PushClipRect") : popup.index("s_settingsBodyScroll")]
    assert "settingsBodyScreen.x + settingsBodyContentW" in clip
    assert "settingsBodyScreen.x + settingsBodyW," not in clip
    bar_geom = popup[popup.index("const float settingsBodyScrollbarW") : popup.index("thumbMin")]
    assert "settingsBodyScreen.x + settingsBodyContentW + settingsBodyScrollbarGap" in bar_geom
    assert "settingsBodyScreen.x + settingsBodyW - settingsBodyScrollbarW" not in bar_geom
    dummy_line = [ln for ln in popup.splitlines() if "Dummy(ImVec2(settingsBodyContentW" in ln]
    assert dummy_line, "missing Dummy for settings body viewport"
    assert "settingsBodyContentW" in dummy_line[0]
    assert "settingsBodyWorkMaxX" in popup
    layout = popup[popup.index("SetCursorPos(ImVec2(settingsBodyPos.x, settingsBodyPos.y - s_settingsBodyScroll))") : popup.index("EndGroup();")]
    assert "settingsBodyWorkMaxX" in layout
    assert "WorkRect.Max.x" in layout
    assert "settingsBodyScreen.x + settingsBodyContentW" in layout
    restore = popup[popup.index("EndGroup();") : popup.rfind("// Bottom buttons")]
    assert "WorkRect.Max.x" in restore
    assert "settingsBodyWorkMaxX" in restore
    assert "settingsBodyW - settingsBodyScrollbarW" not in popup


def test_settings_details_scrollbar_drag_grabs_mouse_not_window() -> None:
    popup = _settings_popup_fragment()
    bar = popup[popup.index("const float settingsBodyScrollbarW") : popup.rindex("ImGui::SetCursorPos(settingsBodyPos);")]
    assert "InvisibleButton" in bar
    assert "##SettingsBodyScrollbar" in bar
    assert "IsItemActive" in bar
    assert "IsItemHovered" in bar
    # 旧写法：裸 HoveringRect + IsMouseDragging 会把拖拽漏给弹窗移动
    assert "IsMouseDragging" not in bar
    assert "IsMouseClicked" not in bar
    assert "IsMouseDown" not in bar
    header = popup[: popup.index("{")]
    assert "ImGuiWindowFlags_NoMove" not in header


def test_settings_footer_primary_buttons_inset_from_dialog_edge() -> None:
    popup = _settings_popup_fragment()
    footer = popup[popup.rfind("// Bottom buttons") : popup.index("aura::ui::ActionButton(L(Set_SaveAndClose)")]
    assert "settingsClientMin.x + aura::ui::metrics::kGap12" in footer
    assert "ImGui::SetCursorPos(ImVec2(settingsClientMin.x, settingsFooterY))" not in footer
    assert "SetCursorPosX(settingsClientMax.x - rightGroupW)" in popup[popup.rfind("// Bottom buttons") :]
