import json
import re
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
MAIN = (ROOT / "src/main.cpp").read_text(encoding="utf-8")
NOTICE_UI = (ROOT / "src/update/HarkenSearchUpdateNoticeUi.cpp").read_text(encoding="utf-8")


def _function_body(source: str, name: str) -> str:
    start = source.index(name + "(")
    opening = source.index("{", start)
    depth = 0
    for index in range(opening, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[opening:index + 1]
    raise AssertionError(f"unclosed function: {name}")


def test_release_notes_use_one_scrollable_native_log_stream() -> None:
    summary = _function_body(NOTICE_UI, "RenderUpdateLog")
    history = _function_body(NOTICE_UI, "RenderHistory")

    assert 'BeginChild("##UpdateReleaseContents"' in summary
    assert "RenderReleaseContents(*release, theme)" in summary
    assert 'BeginChild("##UpdateHistoryContents"' in history
    assert "RenderReleaseContents(*release, theme)" in history
    assert "ImGui::Bullet" not in summary + history
    assert "LeftCol" not in summary + history
    assert "RightCol" not in summary + history


def test_release_notes_expose_version_date_author_and_history() -> None:
    contents = _function_body(NOTICE_UI, "RenderReleaseContents")
    history = _function_body(NOTICE_UI, "RenderHistory")

    assert "DisplayVersion(release.version)" in contents
    assert "release.releaseDate" in contents
    assert "release.headline" in contents
    assert "release.items.size()" in contents
    assert "item.title" in contents and "item.text" in contents
    assert "item.author" in contents
    assert "FormatContributors(&release)" in contents
    assert "BuildHarkenSearchReleaseNotesHistory(notes, installedVersion)" in history


def test_release_notes_keep_one_quiet_dismiss_action() -> None:
    history = _function_body(NOTICE_UI, "RenderHistory")
    assert 'ButtonAt("history-dismiss"' in history
    assert history.count("FinalizeHarkenSearchUpdateNoticeClose(open, notes)") == 1
    assert "ConfirmHarkenSearchUpdateRestart" not in history


def test_release_history_is_readable_utf8_with_structured_change_items() -> None:
    history_path = ROOT / "installer/harken_release_history.json"
    raw = history_path.read_text(encoding="utf-8")
    history = json.loads(raw)

    assert "???" not in raw
    assert history["releases"]
    latest = history["releases"][0]
    assert re.fullmatch(r"\d+\.\d+\.\d+", latest["version"])
    assert latest["release_date"] and latest["headline"]
    # Counts are optional in the native parser; do not invent statistics for
    # a combined release when the verified history only records its changes.
    for count_key in ("pull_request_count", "commit_count"):
        if count_key in latest:
            assert isinstance(latest[count_key], int) and latest[count_key] > 0
    assert latest["source"] == "Harken Search"
    assert latest["contributors"]

    changes = [
        item
        for section in latest["sections"]
        for group in section["items"]
        for item in group["list"]
    ]
    assert changes
    assert all(item["title"] and item["description"] for item in changes)


def test_release_packaging_supplies_the_installed_local_notes() -> None:
    packer = (ROOT / "tools/pack_engine.py").read_text(encoding="utf-8")
    workflow = (ROOT / ".github/workflows/package-update.yml").read_text(encoding="utf-8")
    installer = (ROOT / "installer/HarkenSearch_Setup.iss").read_text(encoding="utf-8")

    assert '"harken_release_history.json"' in packer
    assert 'RELEASE_NOTES_FILENAME = "aris_release_notes.json"' in packer
    assert "prepare_release_notes" in packer
    assert "--prepare-release-notes" in workflow
    assert 'Source: "{#SourceDir}\\aris_release_notes.json"; DestDir: "{app}"' in installer



def test_help_menu_keeps_update_history_inside_update_center() -> None:
    help_start = MAIN.index("if (ImGui::BeginMenu(L(Menu_Help)))")
    help_end = MAIN.index("ImGui::EndMenu();", help_start)
    help_menu = MAIN[help_start:help_end]
    assert "L(Main_MenuUpdateCenter)" in help_menu
    assert "L(Help_About)" in help_menu
    assert "Main_MenuUpdateChangelog" not in help_menu
    assert "wantOpen = true" not in help_menu
    assert "u8\"更新历史\"" in NOTICE_UI

def test_native_surface_uses_automatic_ready_and_manual_history_flows() -> None:
    assert "readyChanged && !state.updateNoticeSeenThisSession" in MAIN
    assert "state.showUpdateCenter = true" in MAIN
    assert "g_releaseNotesUiState.wantOpen" in MAIN
    assert "g_releaseNotesUiState.showHistory = true" in MAIN
    assert "LoadReleaseNotesFromExeDir()" in MAIN
    assert "SyncHarkenSearchCandidateReleaseNotes" in MAIN
    assert "RenderHarkenSearchUpdateNoticeUi" in MAIN
    assert "Update_InlineBackToCheck" in NOTICE_UI
