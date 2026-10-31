from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
MAIN = (ROOT / "src" / "main.cpp").read_text(encoding="utf-8")
HEADER_PATH = ROOT / "src" / "FileInfoPanel.h"
SOURCE_PATH = ROOT / "src" / "FileInfoPanel.cpp"
CMAKE = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
BEHAVIOR = (ROOT / "cmake" / "harken_search_behavior_tests.cmake").read_text(
    encoding="utf-8"
)
RULES = (
    ROOT / "docs" / "architecture" / "stage-4-dependency-rules.json"
).read_text(encoding="utf-8")
STAGE7 = (ROOT / "tools" / "stage7_api_contract_audit.py").read_text(
    encoding="utf-8"
)


def test_file_info_panel_files_own_shell_entry():
    assert HEADER_PATH.exists()
    assert SOURCE_PATH.exists()

    header = HEADER_PATH.read_text(encoding="utf-8")
    source = SOURCE_PATH.read_text(encoding="utf-8")

    assert "void RenderFileInfoPanelContent(AppState& state, float panelWidth);" in header
    assert "int PushFileInfoShellStyle(const ThemeColorScheme& tcs);" in header
    assert "void RenderFileInfoPanelContent(AppState& state, float panelWidth)" in source
    assert "int PushFileInfoShellStyle(const ThemeColorScheme& tcs)" in source
    assert "##FileInfoVisibilityMenu" in source
    assert 'ImGui::BeginTabBar("##FileInfoTabs")' not in source
    assert "RenderFileInfoPageActions" in source
    assert "DecorateFileInfoTab" in source

    for forbidden in ("main.cpp", "WindowsMain", "platform/"):
        assert forbidden not in header
        assert forbidden not in source

    assert "LoadGlobalCoverWallPage" not in source
    assert "applyMetadataPatch" not in source
    assert "applyMetadataPatchBatch" not in source


def test_root_delegates_file_info_panel_shell_without_owning_drawing():
    assert '#include "FileInfoPanel.h"' in MAIN
    assert "static void RenderFileInfoPanelContent(AppState& state, float panelWidth)" not in MAIN
    assert "static int PushFileInfoShellStyle(const ThemeColorScheme& tcs)" not in MAIN
    assert "RenderFileInfoPanelContent(state, rightContentWidth);" in MAIN
    assert "void RenderFileInfoPanelContent(AppState& state, float panelWidth)" not in MAIN
    assert "LoadGlobalCoverWallPage" in MAIN
    assert "applyMetadataPatchBatch" in MAIN
    assert "##FileInfoVisibilityMenu" not in MAIN


def test_file_info_panel_is_built_tested_and_governed():
    assert CMAKE.count("src/FileInfoPanel.cpp") == 1
    assert BEHAVIOR.count("tests/test_file_info_panel_boundary.py") == 1
    assert RULES.count("src/FileInfoPanel.cpp") >= 2
    assert RULES.count("src/FileInfoPanel.h") >= 2
    # The public repository retains executable dependency rules, not the old
    # migration-only dependency-map.md. The rules and API mapping above/below
    # continue to verify this module boundary.
    assert (
        '"src/FileInfoPanel.h": '
        '"tests/test_file_info_panel_boundary.py"'
        in STAGE7
    )
