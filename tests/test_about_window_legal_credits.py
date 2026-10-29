"""关于窗口：法务信息、品牌与 Cindy 标识、明文仓库/下载链接（REQ-20261029-ABOUT-LEGAL-CREDITS）。"""

from pathlib import Path
import json
import re

ROOT = Path(__file__).resolve().parents[1]
MAIN = ROOT / "src/main.cpp"
LEDGER = ROOT / "locale/harken.ui.strings.json"

GITHUB_URL = "https://github.com/xindong/harken-search"
DOWNLOAD_URL = "https://harkensearch.xd.com"
CINDY_GITHUB_URL = "https://github.com/makecindy/cindy"
CINDY_DOWNLOAD_URL = "https://cindy.app"


def _main() -> str:
    return MAIN.read_text(encoding="utf-8")


def _about_block() -> str:
    text = _main()
    start = text.index('"##AboutAuro"')
    end = text.index("ImGui::EndPopup();", start)
    return text[start:end]


def _ledger_entries() -> dict[str, dict]:
    data = json.loads(LEDGER.read_text(encoding="utf-8"))
    return {entry["id"]: entry for entry in data["entries"]}


def test_about_no_longer_points_to_update_center():
    block = _about_block()
    assert "Main_UpdateCenterMovedNote" not in block
    assert "Main_OpenUpdateCenterBtn" not in block
    assert "showUpdateCenter" not in block


def test_about_carries_legal_and_credit_copy():
    block = _about_block()
    for key in (
        "Main_AboutLicenseNote",
        "Main_AboutLegalDocsNote",
        "Main_AboutCindyCredit",
        "Main_AboutRepoLine",
        "Main_AboutDownloadLine",
        "Main_AboutCindyRepoLine",
        "Main_AboutCindyDownloadLine",
    ):
        assert key in block, key


def test_about_shows_all_four_plain_text_urls():
    block = _about_block()
    for url in (GITHUB_URL, DOWNLOAD_URL, CINDY_GITHUB_URL, CINDY_DOWNLOAD_URL):
        assert url in block, url


def test_about_opens_each_url_through_platform_openurl():
    block = _about_block()
    assert block.count("Platform::OpenUrl") == 1  # shared click helper, four call sites
    for url in (GITHUB_URL, DOWNLOAD_URL, CINDY_GITHUB_URL, CINDY_DOWNLOAD_URL):
        assert f'urlLine(L(Main_About' in block


def test_about_shows_brand_and_cindy_artwork():
    block = _about_block()
    assert "aura::ui::LoadPackagedAsset" in block
    assert "assets/icons/brand/harken.png" in block
    assert "harken-search-wordmark" in block
    assert "assets/icons/contributors/cindy.png" in block


def test_contributor_and_brand_assets_exist_and_ship():
    assert (ROOT / "assets/icons/contributors/cindy.png").is_file()
    assert (ROOT / "assets/icons/brand/harken.png").is_file()
    assert (ROOT / "assets/logos/brand/harken-search-wordmark-white.png").is_file()
    assert (ROOT / "assets/logos/brand/harken-search-wordmark-black.png").is_file()
    cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
    assert "assets/icons/contributors" in cmake


def test_ledger_entries_complete_for_about():
    entries = _ledger_entries()
    for key in (
        "Main_AboutLicenseNote",
        "Main_AboutLegalDocsNote",
        "Main_AboutCindyCredit",
        "Main_AboutRepoLine",
        "Main_AboutDownloadLine",
        "Main_AboutCindyRepoLine",
        "Main_AboutCindyDownloadLine",
    ):
        entry = entries[key]
        for lang in ("zh", "en", "ja"):
            assert entry[lang].strip(), (key, lang)
    assert "Cindy" in entries["Main_AboutCindyCredit"]["zh"]
    assert "Apache" in entries["Main_AboutLicenseNote"]["zh"]
    for retired in ("Main_UpdateCenterMovedNote", "Main_OpenUpdateCenterBtn",
                    "Main_AboutGithubAction", "Main_AboutDownloadAction"):
        assert retired not in entries


def test_retired_update_center_strings_have_no_remaining_reference():
    text = _main()
    for retired in ("Main_UpdateCenterMovedNote", "Main_OpenUpdateCenterBtn"):
        assert not re.search(r"\b" + retired + r"\b", text), retired
