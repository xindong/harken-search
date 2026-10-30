import re
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "src/update/HarkenSearchUpdateNoticeUi.cpp"


def test_update_center_reads_live_semantic_theme_tokens() -> None:
    source = SOURCE.read_text(encoding="utf-8")
    assert "ThemeColors::current()" in source
    for token in (
        "resultsText", "dialogBG",
        "dialogPanelBG", "chromeBorder", "interfaceText", "resultsSecondaryText",
        "wwiseConnectFail",
    ):
        assert f"theme.{token}" in source
    assert "ActionButtonStyle::Make(" in source
    assert "ActionButtonStyle::Draw(" in source


def test_update_center_has_no_private_palette_or_orange_escape_hatch() -> None:
    source = SOURCE.read_text(encoding="utf-8")
    assert "kColorActionPrimary" not in source
    assert "kUpdateAccent" not in source
    assert not re.search(r"#[0-9A-Fa-f]{6,8}", source)
    assert not re.search(r"ImVec4\s*\(\s*(?:\d|\.)", source)
    assert "theme.controlAccent" not in source
    assert "111.0f / 255.0f" not in source
