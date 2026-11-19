
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def read(path):
    assert path.is_file(), f"missing: {path}"
    return path.read_text(encoding="utf-8")


def dialog_block(text, start):
    begin = text.index(start)
    end = text.index("aura::ui::EndAlertDialog();", begin)
    return text[begin:end]


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
    assert "ImGuiStyleVar_WindowRounding, kRadiusDialog" in helper
    assert "ImGuiStyleVar_PopupRounding, kRadiusDialog" in helper
    assert "ImGui::Separator" not in helper
    assert "ImGui::TextUnformatted(title, end)" in helper

    end = source[source.index("void EndAlertDialog()"):]
    end = end[:end.index("}") + 1]
    assert "ImGui::EndPopup();" in end
    assert "ImGui::PopStyleVar(5);" in end
    assert "ImGui::PopStyleColor(5);" in end


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


if __name__ == "__main__":
    test_alert_helper_draws_title_inside_a_rounded_surface()
    test_small_confirm_and_input_dialogs_share_the_alert_surface()
    test_playlist_folder_dialogs_use_the_alert_surface()
    test_close_dialog_does_not_repeat_the_title_as_a_question()
    print("alert dialog contract ok")
