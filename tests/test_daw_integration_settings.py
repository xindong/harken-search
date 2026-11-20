import json
import re
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def read(path):
    return (ROOT / path).read_text(encoding="utf-8")


def test_native_settings_sidebar_and_search_share_the_existing_dialog():
    dialog = read("src/SettingsDialog.inl")
    catalog = read("src/SettingsCatalog.h")
    assert "su::Page::Audio, su::Page::DawIntegration, su::Page::Search" in dialog
    assert dialog.count('ImGui::OpenPopup("###HarkenSettings")') == 1
    for control in (
        "daw-reaper", "daw-pro-tools", "daw-cubase", "daw-nuendo", "daw-reaper-install"
    ):
        assert f'"{control}",Page::DawIntegration' in catalog
    labels = {row["id"]: row for row in json.loads(read("locale/harken.ui.strings.json"))["entries"]}
    assert labels["SetUX_DawIntegration"]["zh"] == "DAW 集成"
    assert all(labels["SetUX_DawIntegration"][lang] for lang in ("en", "ja"))


def test_daw_draft_commits_in_existing_transaction_and_survives_save_failure():
    dialog = read("src/SettingsDialog.inl")
    editor = read("src/SettingsDialogState.h")
    assert "dawTargets.Begin(state.settings.dawTargets)" in editor
    assert "dawTargets.ChangedCount()" in editor
    assert dialog.index("editor.dawTargets.MergeInto(next.dawTargets)") < dialog.index("TrySaveAppSettings")
    assert "next.dawTargets=oldSettings.dawTargets;" in dialog
    assert "state.settings=oldSettings" in dialog


def test_reaper_install_is_an_explicit_opt_in_on_the_daw_page():
    fields = read("src/SettingsDawFields.inl")
    assert 'su::Observe("daw-reaper-install-consent")' in fields
    assert 'su::Observe("daw-reaper-install-button")' in fields
    assert "CanInstallMissingReaperBridge" in fields
    assert "StartReaperBridgeUpdateAfterExit(options, false, true)" in fields
    assert "bool reaperInstallConsent=false;" in read("src/SettingsDialogState.h")
    doctor = read("src/ReaperBridgeDoctor.cpp")
    assert "request.installMissingOnly" in doctor
    assert "target.u8string(), !installMissingOnly" in doctor


def test_startup_uses_snapshot_probe_and_existing_settings_persistence():
    main = read("src/main.cpp")
    dialog = read("src/SettingsDialog.inl")
    assert 'kv["reaperSetupNoticeDismissed"]' in main
    assert '{"reaperSetupNoticeDismissed",' in main
    assert "next.reaperSetupNoticeDismissed=oldSettings.reaperSetupNoticeDismissed;" in dialog
    assert "ProbeReaperSetupSnapshot(" in main.split("static void UpdateDAWRunningState", 1)[0]
    assert "DrawReaperSetupNotice(state);" in main
    notice = read("src/ReaperSetupNotice.inl")
    assert "ImGuiPopupFlags_AnyPopupLevel" in notice
    assert "ImGui::OpenPopup" not in notice
    assert "MarkSettingsDirty(state)" in notice
    assert "state.openDawIntegrationSettings" in notice
    assert "StartReaperBridge" not in notice


def test_legacy_daw_dialog_keeps_its_install_action():
    old = read("src/main.cpp").split("// DAW Target Configuration Dialog", 1)[1].split(
        '#include "SettingsDialog.inl"', 1
    )[0]
    assert "BeginTransientScrollbarPopupModal(L(DAW_SettingsTitle)" in old
    assert "BuildReaperBridgeProbeOptions(selected), true" in old


def test_startup_notice_uses_the_acknowledgment_copy():
    notice = read("src/ReaperSetupNotice.inl")
    assert "L(DawPrefs_NoticeAcknowledge)" in notice
    labels = {row["id"]: row for row in json.loads(read("locale/harken.ui.strings.json"))["entries"]}
    assert labels["DawPrefs_NoticeAcknowledge"]["zh"] == "知道了"


def test_running_reaper_copy_does_not_promise_an_automatic_install():
    fields = read("src/SettingsDawFields.inl")
    assert "L(DBflow_CloseReaper)" not in fields
    assert "L(DawPrefs_CloseReaperToInstall)" in fields


def test_successful_install_invalidates_inflight_missing_component_snapshots():
    main = read("src/main.cpp")
    harvest = main.split("if (harvested.ok)", 1)[1].split("} else if", 1)[0]
    assert "g_reaperSetupPageStatus.reset()" in harvest
    assert "++g_reaperSetupProbeGeneration" in harvest
    assert "g_reaperSetupStartupSubmitted = true" in harvest
    assert "g_reaperSetupNoticePending = false" in harvest

    poll = main.split("if (auto completed = s_probeWorker.Poll())", 1)[1]
    assert poll.index("result.setupGeneration != g_reaperSetupProbeGeneration") < poll.index(
        "DawProbePurpose::ReaperSetupStartup"
    )


def test_reaper_row_keeps_probing_and_keeps_editors_out_of_the_collapsed_row():
    dialog = read("src/SettingsDialog.inl")
    fields = read("src/SettingsDawFields.inl")
    assert 'if (id=="daw-reaper-install") { drawReaperInstallAction(); return; }' in dialog
    assert "su::FindSettingsDawPresetIndex(id)" in dialog
    assert "drawDawCard(entry,dawIndex)" in dialog
    assert "editor.reaperConfigOpen=false;" in dialog
    row = fields.split("auto drawDawCard", 1)[-1].split("auto drawReaperInstallAction", 1)[0]
    assert "g_reaperSetupPageProbePath=target.exePath;" in row
    disclosure = row.index("if (sendReady && editor.reaperConfigOpen)")
    assert row.index("g_reaperSetupPageProbePath=target.exePath;") < disclosure
    assert row.index("drawDawDisclosure(L(DawPrefs_Configure)") < disclosure
    assert row.index('su::Observe("daw-reaper-status-icon")') < disclosure
    for item in ("daw-reaper-path-selector", "daw-reaper-path", "daw-reaper-enabled", "daw-reaper-diagnostic"):
        assert row.index(f'su::Observe("{item}")') > disclosure
    assert "OpenPopup" not in row[:disclosure]


def test_other_daws_share_the_card_and_cannot_open_configuration():
    fields = read("src/SettingsDawFields.inl")
    header = fields.split("auto drawDawCard", 1)[1].split("if (sendReady && editor.reaperConfigOpen)", 1)[0]
    assert "const bool sendReady=index==0;" in header
    assert header.count("drawDawDisclosure(L(DawPrefs_Configure)") == 1
    assert 'sendReady ? "daw-reaper-config" : entry.id+"-config-disabled"' in header
    assert "editor.reaperConfigOpen,sendReady,false,configObservation.c_str()" in header
    disclosure = fields.split("auto drawDawDisclosure", 1)[1].split("auto drawDawCard", 1)[0]
    interaction = disclosure.split("ImGui::BeginDisabled(!enabled);", 1)[1].split("ImGui::EndDisabled();", 1)[0]
    button = "ImGui::InvisibleButton(label,size,ImGuiButtonFlags_EnableNav)"
    assert button in interaction
    assert "su::Observe(observationId);" in interaction.split(button, 1)[1], (
        "Observe the submitted button before restoring the enabled flags"
    )
    assert "return pressed;" in disclosure


def test_unsupported_daw_cards_show_only_name_and_disabled_configuration():
    fields = read("src/SettingsDawFields.inl")
    header = fields.split("auto drawDawCard", 1)[1].split("if (sendReady && editor.reaperConfigOpen)", 1)[0]
    assert re.findall(r"ImGui::Text\w*\([^;]+;", header) == ["ImGui::TextUnformatted(name);"]
    assert "DawPrefs_SendUnavailable" not in fields
    assert '"-send-unavailable"' not in header
    disclosure = fields.split("auto drawDawDisclosure", 1)[1].split("auto drawDawCard", 1)[0]
    assert "Tooltip" not in disclosure
    assert re.findall(r"ImGui::\w*Tooltip\([^\n]+", header) == [
        'ImGui::SetTooltip("%s",L(DawPrefs_Consent));',
        'ImGui::SetTooltip("%s",pathState);',
    ]
    labels = {row["id"]: row for row in json.loads(read("locale/harken.ui.strings.json"))["entries"]}
    assert "DawPrefs_SendUnavailable" not in labels


def test_reaper_install_controls_only_exist_when_install_is_available():
    fields = read("src/SettingsDawFields.inl")
    row = fields.split("auto drawDawCard", 1)[-1].split("auto drawReaperInstallAction", 1)[0]
    action = fields.split("auto drawReaperInstallAction", 1)[-1].split("auto drawDawFields", 1)[0]
    install_control = row.split("if (installable) {", 1)[1].split('su::Observe("daw-reaper-install-consent")', 1)[0]
    assert "ImGui::Checkbox(checkboxLabel.c_str(), &editor.reaperInstallConsent)" in install_control
    assert "if (!installable) return;" in action
    assert "ImGui::BeginDisabled(!editor.reaperInstallConsent)" in action
    assert "L(DawPrefs_InstallSelected)" in action
    assert fields.count("StartReaperBridgeUpdateAfterExit(options, false, true)") == 1


def test_reaper_path_selector_offers_only_confirmed_detection_and_edits_the_draft():
    fields = read("src/SettingsDawFields.inl")
    assert "g_reaperSetupStatusPath.empty()" in fields
    assert 'if (!editor.reaperDetectedPath.empty())' in fields
    assert "target.exePath=editor.reaperDetectedPath;" in fields
    assert 'su::Observe("daw-reaper-use-detected-path")' in fields
    assert "if (target.exePath!=previousPath)" in fields
    assert "!target.exePath.empty() && status && status->hostInstalled" in fields
    assert "state.settings.dawTargets" not in fields


def test_reaper_search_opens_the_inline_controls_and_scrolls_to_the_blocker():
    dialog = read("src/SettingsDialog.inl")
    fields = read("src/SettingsDawFields.inl")
    assert "su::AppendReaperFieldSearchResults(results,editor.query)" in dialog
    for destination in ("focusPath", "focusSending", "focusDiagnostic", "focusInstallReason"):
        assert destination in fields
    assert "editor.reaperDiagnosticsOpen=false;" in dialog
    assert "if (focusDiagnostic) editor.reaperDiagnosticsOpen=true;" in fields
    assert "finishReaperFocus(focusDiagnostic);" in fields
    assert 'editor.focusId=="daw-reaper-install" && !installable' in fields
    assert 'su::Observe("daw-reaper-install-reason")' in fields
    assert "ImGui::SetKeyboardFocusHere()" in fields
    assert "ImGui::SetScrollHereY(.25f)" in fields


def test_reaper_disclosure_keeps_path_actions_and_diagnostics_secondary():
    fields = read("src/SettingsDawFields.inl")
    row = fields.split("auto drawDawCard", 1)[1].split("auto drawReaperInstallAction", 1)[0]
    choices = row.split("ImGui::BeginCombo", 1)[1].split("ImGui::EndCombo()", 1)[0]
    assert "L(DawPrefs_Browse)" in choices
    assert "L(DawPrefs_Clear)" in choices
    assert "drawDawDisclosure(L(DawPrefs_ReaperDiagnostics)" in row
    diagnostic = row.split('su::Observe("daw-reaper-diagnostic-disclosure")', 1)[1]
    assert diagnostic.index("if (editor.reaperDiagnosticsOpen)") < diagnostic.index('su::Observe("daw-reaper-diagnostic")')
    name = row.index('su::Observe((entry.id+"-name").c_str())')
    status = row.index('su::Observe("daw-reaper-status-icon")')
    config = row.index("drawDawDisclosure(L(DawPrefs_Configure)")
    assert name < status < config


def test_reaper_card_background_uses_the_settings_theme_and_ends_after_inline_content():
    fields = read("src/SettingsDawFields.inl")
    row = fields.split("auto drawDawCard", 1)[1].split("auto drawReaperInstallAction", 1)[0]
    card = 'su::Observe((entry.id+"-card").c_str())'
    assert card in row
    assert "BeginChild" not in row
    assert row.index("cardLayers.SetCurrentChannel(draw,1)") < row.index('su::Observe((entry.id+"-name").c_str())')
    assert row.index('su::Observe("daw-reaper-diagnostic")') < row.index(card)
    assert row[:row.index(card)].rstrip().endswith("ImGui::EndGroup();")
    assert row.index(card) < row.index("cardLayers.SetCurrentChannel(draw,0)")
    assert "dawCardFill=ImLerp(preview.dialogBG,preview.interfaceText" in fields
    assert "draw->AddRectFilled(cardMin,cardMax,ImGui::GetColorU32(dawCardFill)" in row
    assert "draw->AddRect(cardMin,cardMax,ImGui::GetColorU32(preview.chromeBorder)" in row
    assert "cardLayers.Merge(draw)" in row
    assert row.count("drawCardDivider();") == 3


def test_reaper_path_menu_spans_the_input_and_arrow():
    fields = read("src/SettingsDawFields.inl")
    menu = fields.split('ImGui::InputTextWithHint("##reaper-path"', 1)[1].split("ImGui::EndCombo()", 1)[0]
    assert "ImGui::BeginComboPopup(pathMenu,pathAnchor,ImGuiComboFlags_PopupAlignLeft)" in menu
    assert menu.index("pathMin=ImGui::GetItemRectMin()") < menu.index('ImGui::ArrowButton("##reaper-path-selector"')
    assert "ImRect pathAnchor(pathMin,ImGui::GetItemRectMax())" in menu
    assert "ImGui::SetNextWindowPos(pathAnchor.GetBL(),ImGuiCond_Appearing)" in menu
    assert "menuWidth=pathAnchor.GetWidth()" in menu
    detected = menu.split("const std::string detectedLabel=", 1)[1].split(
        'su::Observe("daw-reaper-use-detected-path")', 1
    )[0]
    assert "ImGui::GetContentRegionAvail().x" in detected


def test_clicking_the_reaper_path_input_while_the_popup_is_open_restores_input_focus():
    fields = read("src/SettingsDawFields.inl")
    menu = fields.split('ImGui::InputTextWithHint("##reaper-path"', 1)[1].split("ImGui::EndCombo()", 1)[0]
    input_item = menu.split('ImGui::ArrowButton("##reaper-path-selector"', 1)[0]
    assert "ImGui::IsMouseClicked(ImGuiMouseButton_Left)" in input_item
    hover = input_item.split("ImGui::IsItemHovered(", 1)[1].split(");", 1)[0]
    assert "ImGuiHoveredFlags_AllowWhenBlockedByPopup" in hover
    assert "ImGuiHoveredFlags_NoNavOverride" in hover
    assert "AllowWhenOverlapped" not in hover
    popup = menu.split("ImGui::BeginComboPopup(pathMenu", 1)[1]
    refocus = popup.split("if (pathInputClicked) {", 1)[1].split("} else {", 1)[0]
    assert "editor.reaperFocusPath=true;" in refocus
    assert "ImGui::CloseCurrentPopup();" in refocus
    assert "ImGui::Selectable" not in refocus
    assert "if (focusPath || editor.reaperFocusPath) ImGui::SetKeyboardFocusHere();" in fields


def test_clicking_the_reaper_path_label_focuses_the_input():
    fields = read("src/SettingsDawFields.inl")
    card = fields.split("if (sendReady && editor.reaperConfigOpen)", 1)[1].split(
        "if (browseRequested)", 1
    )[0]
    assert "ImGui::TextUnformatted(pathLabel);" in card
    assert "ImGui::IsMouseClicked(ImGuiMouseButton_Left)" in card
    assert "ImGui::IsMouseHoveringRect(" in card
    assert "pathLabelClicked" in card
    assert "if (pathLabelClicked) editor.reaperFocusPath=true;" in card
    assert card.index("pathLabelClicked") < card.index('ImGui::InputTextWithHint("##reaper-path"')


def test_reaper_path_input_shows_themed_focus_ring_and_keeps_the_caret():
    fields = read("src/SettingsDawFields.inl")
    card = fields.split("if (sendReady && editor.reaperConfigOpen)", 1)[1].split(
        "if (browseRequested)", 1
    )[0]
    ring = card.split('ImGui::InputTextWithHint("##reaper-path"', 1)[1]
    assert "ImGui::IsItemActive()" in ring
    assert "ImGui::IsItemFocused()" in ring
    assert "pathActive" in ring
    assert "if (pathActive)" in ring
    assert "draw->AddRect(" in ring
    assert "preview.controlAccent" in ring
    assert "ImGuiInputTextFlags_ReadOnly" not in ring
    assert "ImGui::PushStyleColor(" not in ring


def test_reaper_path_placeholder_is_separate_from_picker_titles():
    fields = read("src/SettingsDawFields.inl")
    card = fields.split("auto drawDawCard", 1)[1].split("auto drawReaperInstallAction", 1)[0]
    assert "const char* pathHint=L(DawPrefs_PathInputHint);" in card
    assert 'ImGui::InputTextWithHint("##reaper-path",pathHint' in card
    browse = card.split("if (browseRequested)", 1)[1].split("if (target.exePath!=previousPath)", 1)[0]
    assert "Platform::BrowseFolderDialogModern(g_hwnd,L(Main_ReaperRootDir))" in browse
    assert "Utf8ToWide(pathLabel)" in browse
    assert "pathHint" not in browse
    labels = {row["id"]: row for row in json.loads(read("locale/harken.ui.strings.json"))["entries"]}
    assert labels["DawPrefs_PathInputHint"]["zh"] == "\u8f93\u5165\u6216\u7c98\u8d34\u8def\u5f84"
    assert all(labels["DawPrefs_PathInputHint"][lang] for lang in ("en", "ja"))


def test_reaper_path_below_label_when_inline_space_is_too_small():
    fields = read("src/SettingsDawFields.inl")
    form = fields.split("if (sendReady && editor.reaperConfigOpen)", 1)[1].split(
        'ImGui::InputTextWithHint("##reaper-path"', 1
    )[0]
    assert "if (pathFitsInline) ImGui::SameLine();" in form, "Do not force the input past the card edge"
    assert "pathFitsInline ? pathInlineWidth : configRight-nameX" in form
    assert "ImGui::SetCursorPosX(configRight-pathWidth)" in form
    assert "ImGui::SetNextItemWidth(pathWidth-selectorWidth)" in form


def test_reaper_send_switch_keeps_search_focus_and_the_settings_palette():
    fields = read("src/SettingsDawFields.inl")
    form = fields.split("if (sendReady && editor.reaperConfigOpen)", 1)[1].split('su::Observe("daw-reaper-enabled")', 1)[0]
    assert "if (focusSending) ImGui::SetKeyboardFocusHere();" in form
    assert "transportSwitch(target.enabled," in form
    switch = read("src/SettingsDialog.inl").split("auto transportSwitch=", 1)[1].split('#include "SettingsDawFields.inl"', 1)[0]
    assert 'ImGui::Button("##visibility-switch"' in switch
    assert "preview.controlAccent" in switch


def test_reaper_live_diagnostics_wrap_paths_to_the_card_edge():
    fields = read("src/SettingsDawFields.inl")
    status = fields.split("const auto* statusForCopy=", 1)[1]
    _, wrap_scope = status.split("ImGui::PushTextWrapPos(configRight);", 1)
    wrap_scope = wrap_scope.split("ImGui::PopTextWrapPos();", 1)[0]
    live = wrap_scope.split("else if (liveStatus)", 1)[1].split("else if (!status)", 1)[0]
    for label, field in (
        ("Main_ReaperStatusFmt", "hostExecutablePath"),
        ("Main_ReaperResourceFmt", "resourcePath"),
        ("Main_ReaperUserPluginsFmt", "targetDirectory"),
        ("Main_ReaperBridgeInfoFmt", "installedVersion"),
    ):
        assert re.search(rf"ImGui::TextWrapped\s*\(\s*L\({label}\)\s*,\s*liveStatus->{field}\b", live), label


def test_reaper_settings_diagnostics_only_copy_the_redacted_summary():
    fields = read("src/SettingsDawFields.inl")
    actions = fields.split("if (editor.reaperDiagnosticsOpen) {", 1)[1].split(
        "finishReaperFocus(focusDiagnostic)", 1
    )[0]
    assert actions.count("SmallActionButton(") == 1
    assert "g_reaperAutoDetectRequests" not in actions
    assert "g_reaperSetupStatusAt" not in actions
    assert "const char* copyLabel=L(DawPrefs_CopyDiagnostic);" in actions
    assert "BuildReaperBridgeDiagnosticSummary(*statusForCopy,true)" in actions
    assert "ImGui::SetClipboardText(summary.c_str())" in actions
    labels = {row["id"]: row for row in json.loads(read("locale/harken.ui.strings.json"))["entries"]}
    assert labels["DawPrefs_CopyDiagnostic"]["zh"] == "\u590d\u5236\u8bca\u65ad"
    assert all(labels["DawPrefs_CopyDiagnostic"][lang] for lang in ("en", "ja"))


def test_reaper_copy_diagnostic_can_wrap_its_label_without_losing_focus_or_its_id():
    fields = read("src/SettingsDawFields.inl")
    disclosure = fields.split("finishReaperFocus(focusInstallReason);", 1)[1].split(
        'su::Observe("daw-reaper-diagnostic-disclosure")', 1
    )[0]
    assert disclosure.index("if (focusDiagnostic && !statusForCopy) ImGui::SetKeyboardFocusHere();") < disclosure.index(
        "drawDawDisclosure(L(DawPrefs_ReaperDiagnostics)"
    )
    actions = fields.split("if (editor.reaperDiagnosticsOpen) {", 1)[1].split(
        "finishReaperFocus(focusDiagnostic)", 1
    )[0]
    assert "ImGui::FindRenderedTextEnd(copyLabel)" in actions, "Wrap only the visible text"
    assert "CalcWordWrapPosition(fontSize,lineStart,textEnd,copyTextWidth)" in actions
    assert "configRight-nameX-horizontalPadding" in actions
    assert 'copyButtonLabel+="###ReaperCopyDiagnostic";' in actions
    copy_button = actions.split("if (focusDiagnostic && statusForCopy) ImGui::SetKeyboardFocusHere();", 1)[1]
    assert copy_button.index("ImGui::BeginDisabled(!statusForCopy)") < copy_button.index(
        "SmallActionButton(copyButtonLabel.c_str())"
    ) < copy_button.index("ImGui::EndDisabled()")
