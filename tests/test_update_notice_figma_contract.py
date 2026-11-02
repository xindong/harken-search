from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[1]


def _source() -> str:
    return (ROOT / "src/update/HarkenSearchUpdateNoticeUi.cpp").read_text(encoding="utf-8")


def test_seven_states_keep_viewport_bounded_inline_content() -> None:
    source = _source()
    header = (ROOT / "src/update/HarkenSearchUpdateNoticeUi.h").read_text(encoding="utf-8")
    expected = ("Checking", "Latest", "Available", "Downloading", "Restarting", "Error", "History")
    enum_body = re.search(r"enum class HarkenSearchUpdateNoticeVisibleState\s*\{([^}]*)\}", header, re.S)
    assert enum_body
    actual = tuple(part.strip().rstrip(",") for part in enum_body.group(1).splitlines() if part.strip())
    assert actual == expected
    for state in expected:
        assert f"HarkenSearchUpdateNoticeVisibleState::{state}" in source
    # 2026-10-22 user reference: full notice in this window, with a scrollable
    # body and fixed actions. The former per-state summary heights are retired.
    for literal in ("760.0f", "330.0f", "332.0f", "700.0f"):
        assert literal in source
    assert "WorkSize.y - 40.0f" in source


def test_truthful_copy_and_actions_are_present() -> None:
    source = _source()
    for copy in (
        "正在检查更新", "当前已是最新版本", "有新版本可用", "正在下载更新",
        "正在准备更新", "独立更新窗口", "立即更新", "重试", "知道了",
    ):
        assert copy in source
    assert "已更新到" not in source
    assert "50%" not in source
    # The main process cannot truthfully claim replacement while still alive.
    assert "正在替换并重启" not in source
    assert "正在替换更新文件" not in source


def test_history_shares_full_content_and_returns_to_update() -> None:
    source = _source()
    header = (ROOT / "src/update/HarkenSearchUpdateNoticeUi.h").read_text(encoding="utf-8")
    for key in ("Update_InlineHistoryEmpty", "Update_InlineHistoryReadFailed", "Update_InlineBackToCheck"):
        assert key in source
    for state in ("Empty", "LoadFailed", "Populated"):
        assert f"HarkenSearchUpdateHistoryState::{state}" in header
    assert "ClassifyHarkenSearchUpdateHistory" in header
    assert "BuildHarkenSearchReleaseNotesHistory" in source
    assert "RenderReleaseContents(*release, theme)" in source
    assert '"##UpdateHistoryContents"' in source
    assert "expandedReleaseVersion" in source
    assert "!rendersUpdateSummary && state != HarkenSearchUpdateNoticeVisibleState::History" in source


def test_busy_states_are_disabled_and_primary_action_is_once_only() -> None:
    source = _source()
    assert "HarkenSearchUpdateNoticeActionGate" in source
    assert "ImGui::BeginDisabled" in source
    assert "TryConsume" in source


def test_isolated_native_evidence_seam_is_compile_time_gated() -> None:
    cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
    main = (ROOT / "src/main.cpp").read_text(encoding="utf-8")
    assert "ARIS_UPDATE_NOTICE_QA" in cmake
    assert "#if defined(ARIS_UPDATE_NOTICE_QA)" in main
    assert "ARIS_UPDATE_NOTICE_QA_STATE" in main
