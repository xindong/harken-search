
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]

SRC_SUFFIXES = {".cpp", ".inl", ".h", ".hpp"}
RAW_POPUP_CALLS = (
    "BeginPopupModal(",
    "BeginTransientScrollbarPopupModal(",
    "BeginDialog(",
)

# Dialogs that already draw their own header inside the panel and pass
# ImGuiWindowFlags_NoTitleBar through a shared flags constant.
CUSTOM_CHROME_CALLS = (
    "if (BeginTransientScrollbarPopupModal(id, nullptr, ImGuiWindowFlags_NoTitleBar)) {",
    "BeginTransientScrollbarPopupModal(progressId, nullptr, flags)",
    "BeginTransientScrollbarPopupModal(resultId, nullptr, flags)",
    "BeginTransientScrollbarPopupModal(L(Dlg_ScanFolder), nullptr, scanDialogFlags)",
    "##ScanStopChoice",
    "BeginTransientScrollbarPopupModal(scanResultPopup, nullptr, scanDialogFlags)",
    "##ScanFailureDetails",
    "##ScanDropBusy",
    "BeginTransientScrollbarPopupModal(L(Dlg_NewDB), nullptr, newDbFlags)",
)


def read(path):
    assert path.is_file(), f"missing: {path}"
    return path.read_text(encoding="utf-8")


def dialog_block(text, start):
    begin = text.index(start)
    end = text.index("aura::ui::EndAlertDialog();", begin)
    return text[begin:end]


def iter_sources():
    src = ROOT / "src"
    for path in sorted(src.rglob("*")):
        if path.suffix not in SRC_SUFFIXES or not path.is_file():
            continue
        if path.name in {"TransientScrollbarHost.cpp", "TransientScrollbarHost.h"}:
            continue  # declares the wrapper itself
        if "generated" in path.parts:
            continue
        yield path, path.read_text(encoding="utf-8", errors="replace")


def test_alert_helper_draws_title_inside_a_rounded_surface():
    header = read(ROOT / "src" / "HarkenSearchWidgets.h")
    source = read(ROOT / "src" / "HarkenSearchWidgets.cpp")
    metrics = read(ROOT / "src" / "HarkenSearchMetrics.h")

    assert "bool BeginAlertDialog(" in header
    assert "void EndAlertDialog();" in header
    assert "kRadiusDialog" in metrics

    helper = source[source.index("bool BeginAlertDialog("):]
    helper = helper[:helper.index("void EndAlertDialog()")]
    assert "ImGuiWindowFlags_NoTitleBar" in helper
    # 圆角随界面倍率放大，与按钮共用同一个缩放因子。
    assert "ImGuiStyleVar_WindowRounding, aura::ui::dpi::Px(kRadiusDialog)" in helper
    assert "ImGuiStyleVar_PopupRounding, aura::ui::dpi::Px(kRadiusDialog)" in helper
    assert "ImGui::Separator" not in helper
    # macOS-style caption bar: one filled strip inside the outline, title on
    # the left, close on the right, and the strip itself is the drag zone.
    assert "ImDrawFlags_RoundCornersTop" in helper
    assert "DrawStandaloneCaptionButton" in helper
    assert '"##DialogCaptionDrag"' in helper
    assert "ImGui::SetWindowPos(" in helper
    assert '"##DialogClose"' in helper
    assert "ImGui::CloseCurrentPopup();" in helper
    assert "bool showClose" in helper
    # The caption X runs the same dialog-local cleanup as the cancel button.
    assert "onCaptionClose" in header
    assert "if (onCaptionClose && *onCaptionClose) (*onCaptionClose)();" in helper

    end = source[source.index("void EndAlertDialog()"):]
    end = end[:end.index("}") + 1]
    assert "ImGui::EndPopup();" in end
    assert "ImGui::PopStyleVar(5);" in end
    assert "ImGui::PopStyleColor(5);" in end


def test_dialogs_that_must_be_answered_hide_the_close_button():
    header = read(ROOT / "src" / "HarkenSearchWidgets.h")
    assert "bool showClose = true" in header
    main = read(ROOT / "src" / "main.cpp")
    marker = "aura::ui::BeginAlertDialog(title,title,ImGuiWindowFlags_AlwaysAutoResize,false)"
    assert marker in main, "the save-before-leaving prompt must stay un-dismissable"


def test_small_confirm_and_input_dialogs_share_the_alert_surface():
    main = read(ROOT / "src" / "main.cpp")
    for title in (
        "Dlg_CloseSM",
        "Dlg_EditFilename",
        "Dlg_ConfirmRemove",
        "Dlg_ConfirmDelete",
        "Dlg_NewPlaylist",
        "Dlg_NewFolder",
        "AssetLib_DeleteConfirm",
        "PtCloseAfterSave",
        "Dlg_ConfirmDeleteCover",
        "Dlg_ConfirmWriteOriginalAudio",
        "Dlg_RenameTab",
    ):
        marker = f"aura::ui::BeginAlertDialog(L({title}), L({title})"
        block = dialog_block(main, marker)
        assert "ImGui::Separator" not in block, f"{title} still draws a rule"


def test_playlist_folder_dialogs_use_the_alert_surface():
    source = read(ROOT / "src" / "PlaylistQuickBar.cpp")
    assert source.count("aura::ui::BeginAlertDialog(") == 6
    assert source.count("aura::ui::EndAlertDialog();") == 6

    for marker in (
        '##plnewsub',
        '##plpromote',
        '##plmovesub',
        '##pldelfolder',
        '##pldelete_second',
        '##plbulk',
    ):
        begin = source.index(marker)
        begin = source.index("aura::ui::BeginAlertDialog(", begin)
        end = source.index("aura::ui::EndAlertDialog();", begin)
        assert "ImGui::Separator" not in source[begin:end], f"{marker} still draws a rule"


def test_close_dialog_does_not_repeat_the_title_as_a_question():
    main = read(ROOT / "src" / "main.cpp")
    assert "L(CloseDlg_Question)" not in dialog_block(main, "aura::ui::BeginAlertDialog(L(Dlg_CloseSM)")


def test_old_themed_dialog_helper_is_gone():
    header = read(ROOT / "src" / "HarkenSearchWidgets.h")
    source = read(ROOT / "src" / "HarkenSearchWidgets.cpp")
    assert "BeginDialog(" not in header
    assert "EndDialog()" not in header
    assert "bool BeginDialog(" not in source
    assert "void EndDialog()" not in source


def test_custom_chrome_dialogs_still_ask_for_no_title_bar():
    main = read(ROOT / "src" / "main.cpp")
    for marker in CUSTOM_CHROME_CALLS:
        assert marker in main, f"custom-chrome dialog marker drifted: {marker}"
    assert "ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoSavedSettings;" in main
    assert "newDbFlags |= ImGuiWindowFlags_NoTitleBar" in main


def test_no_modal_dialog_keeps_the_default_title_bar():
    offenders = []
    for path, text in iter_sources():
        for call in RAW_POPUP_CALLS:
            start = 0
            while True:
                index = text.find(call, start)
                if index < 0:
                    break
                start = index + len(call)
                window = text[index:index + 320]
                if "NoTitleBar" in window:
                    continue
                if any(marker in window for marker in CUSTOM_CHROME_CALLS):
                    continue
                line = text.count("\n", 0, index) + 1
                offenders.append(f"{path.name}:{line}")
    assert offenders == [], f"dialog still draws ImGui's title bar: {offenders}"


def test_every_dialog_kind_uses_the_shared_surface():
    expectations = {
        "src/main.cpp": (
            "L(DAW_SettingsTitle)",
            "L(Dlg_BatchOp)",
            "L(Export_FileTitle)",
            "L(Set_ShellSetDefault)",
            "L(Misc_ClearFolderCache)",
            "L(PL_RenameTitle)",
            "L(AssetLib_Title)",
            "L(Menu_SendFeedback)",
            '"##MetaEditDlg"',
            '"##ConfirmLoadPreset"',
            "kPopupTitle",
            "aura::ui::BeginAlertDialog(title,title,ImGuiWindowFlags_AlwaysAutoResize,false)",
        ),
        "src/ModuleUI.cpp": (
            "L(ModuleUI_WorkflowPreviewTitle)",
            "L(ModuleUI_UcsWriteTitle)",
            "L(ModuleUI_SavePreset)",
        ),
        "src/SimilarSoundsWindow.cpp": ("L(Similar_AnalysisReport)",),
        "src/WwiseTransferPanel.cpp": ("presetNameModal", "presetDeleteConfirm"),
        "src/RelinkWorkflow.cpp": ("kRelinkPromptId",),
        "src/SettingsAvatarTools.inl": ("L(AvatarCropTitle)", "L(Set_AvatarPickerTitleZh)"),
    }
    for rel, markers in expectations.items():
        text = read(ROOT / rel)
        assert text.count("aura::ui::BeginAlertDialog(") == text.count("aura::ui::EndAlertDialog();"), (
            f"{rel} has unbalanced dialog surface calls")
        for marker in markers:
            assert marker in text, f"{rel}: {marker} is missing"
            begin = 0
            found = False
            while True:
                begin = text.find(marker, begin)
                if begin < 0:
                    break
                begin += 1
                if "aura::ui::BeginAlertDialog(" in text[max(0, begin - 400):begin + 220]:
                    found = True
                    break
            assert found, f"{rel}: {marker} is not opened on the shared surface"


def test_migrated_dialogs_do_not_draw_a_rule_between_title_and_body():
    checks = {
        "src/main.cpp": (
            "aura::ui::BeginAlertDialog((std::string(L(Set_ShellSetDefault))",
            "aura::ui::BeginAlertDialog(L(DAW_SettingsTitle)",
            "aura::ui::BeginAlertDialog(playlistRenamePopupLabel.c_str()",
        ),
        "src/ModuleUI.cpp": ("aura::ui::BeginAlertDialog(L(ModuleUI_WorkflowPreviewTitle)",),
        "src/SimilarSoundsWindow.cpp": ("aura::ui::BeginAlertDialog(L(Similar_AnalysisReport)",),
    }
    for rel, markers in checks.items():
        text = read(ROOT / rel)
        for marker in markers:
            block = dialog_block(text, marker)
            head = block.split("\n")[:6]
            assert "ImGui::Separator" not in "\n".join(head), (
                f"{rel}: {marker} still draws a rule under the title")


def test_folder_auto_rename_uses_the_standalone_window_shell():
    main = read(ROOT / "src" / "main.cpp")
    header = read(ROOT / "src" / "HarkenSearchWidgets.h")
    widgets = read(ROOT / "src" / "HarkenSearchWidgets.cpp")
    app_state = read(ROOT / "src" / "AppState.h")

    assert "aura::ui::StandaloneWindowState folderAutoRenameWindow;" in app_state
    assert "aura::ui::OpenStandaloneWindow(&state.folderAutoRenameWindow);" in main
    assert "aura::ui::CloseStandaloneWindow(&state.folderAutoRenameWindow);" in main
    assert "aura::ui::BeginAlertDialog(L(Dlg_FolderAutoRename)" not in main

    marker = 'aura::ui::BeginStandaloneWindow(L(Dlg_FolderAutoRename), "##FolderAutoRenameWindow"'
    assert marker in main, "folder auto-rename must open on the shared standalone shell"
    block = main[main.index(marker):main.index("// Edit Filename Dialog", main.index(marker))]
    assert "aura::ui::StandaloneWindowBodyHeight()" in block
    assert "aura::ui::BeginStandaloneWindowFooter();" in block
    assert "aura::ui::StandaloneWindowFooterButton(" in block
    assert "aura::ui::EndStandaloneWindowFooter();" in block
    assert "ImGui::CloseCurrentPopup();" not in block

    # The size memory and footer belong to the one shell, not to this window.
    assert "bool requestRestoreSize = false;" in header
    assert "ImVec2 rememberedSize" in header
    assert "bool hasRememberedSize = false;" in header
    assert "float StandaloneWindowBodyHeight();" in header
    assert "bool StandaloneWindowFooterButton(" in header
    shell = widgets[widgets.index("bool BeginStandaloneWindow"):widgets.index("void EndStandaloneWindow")]
    assert "state->requestRestoreSize" in shell
    assert "state->rememberedSize = ImGui::GetWindowSize();" in shell
    assert "ImGui::SetNextWindowSize(state->rememberedSize, ImGuiCond_Always);" in shell
    assert "g_standaloneFooter.active" in widgets[
        widgets.index("void BeginStandaloneWindowFooter()"):
        widgets.index("void EndStandaloneWindowFooter()")]


def test_standalone_caption_buttons_share_lucide_ink():
    widgets = read(ROOT / "src" / "HarkenSearchWidgets.cpp")
    close_icon = read(ROOT / "src" / "ui" / "CloseIconButton.h")

    start = widgets.index("bool DrawStandaloneCaptionButton")
    block = widgets[start:widgets.index("void OpenStandaloneWindow", start)]
    # One icon source, one optical rule: Lucide minus/square/copy/x, centered
    # by the rasterized visible box instead of the padded glyph quad.
    assert "ICON_LC_MINUS" in block
    assert "ICON_LC_STOP" in block
    assert "ICON_LC_COPY" in block
    assert "ICON_LC_X" in block
    assert "VisibleGlyphBounds" in block
    assert "inkExtent" in block
    assert "kIconXs / inkExtent" in block
    assert "hoverExtent" in block
    # The old hand-drawn line/rect strokes must not come back.
    assert "AddLine(" not in block
    assert "drawList->AddRect(" not in block

    assert "ImRect VisibleGlyphBounds" in close_icon
    assert "VisibleGlyphCenter" in close_icon
    assert "bounds.Min.x = glyph.X0" in close_icon


def test_database_switch_closes_folder_rename_preview():
    main = read(ROOT / "src" / "main.cpp")
    windows_main = read(ROOT / "src" / "WindowsMain.cpp")

    start = main.index("static bool SwitchDatabase(AppState& state")
    switch_block = main[start:main.index("bool BeginSwitchDatabase", start)]
    assert "state.folderRenamePreview.clear();" in switch_block
    assert "state.folderRenameLog.clear();" in switch_block
    assert "aura::ui::CloseStandaloneWindow(&state.folderAutoRenameWindow);" in switch_block

    # The QA receipt must follow the visible __Maximized window, not the hidden
    # normal window that keeps the pre-maximize geometry.
    assert '"###FolderAutoRenameWindow__Maximized"' in windows_main
    assert "state.folderAutoRenameWindow.maximized" in windows_main


if __name__ == "__main__":
    test_alert_helper_draws_title_inside_a_rounded_surface()
    test_dialogs_that_must_be_answered_hide_the_close_button()
    test_small_confirm_and_input_dialogs_share_the_alert_surface()
    test_playlist_folder_dialogs_use_the_alert_surface()
    test_close_dialog_does_not_repeat_the_title_as_a_question()
    test_old_themed_dialog_helper_is_gone()
    test_custom_chrome_dialogs_still_ask_for_no_title_bar()
    test_no_modal_dialog_keeps_the_default_title_bar()
    test_every_dialog_kind_uses_the_shared_surface()
    test_migrated_dialogs_do_not_draw_a_rule_between_title_and_body()
    test_folder_auto_rename_uses_the_standalone_window_shell()
    test_standalone_caption_buttons_share_lucide_ink()
    test_database_switch_closes_folder_rename_preview()
    print("alert dialog contract ok")
