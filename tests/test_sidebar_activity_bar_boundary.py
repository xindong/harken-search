from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
MAIN = (ROOT / "src" / "main.cpp").read_text(encoding="utf-8")
HEADER_PATH = ROOT / "src" / "SidebarActivityBar.h"
SOURCE_PATH = ROOT / "src" / "SidebarActivityBar.cpp"
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
# The public checkout carries the executable dependency rules above, not the
# historical architecture narrative from the private repository.


def test_sidebar_activity_bar_files_own_drawing_entry():
    assert HEADER_PATH.exists()
    assert SOURCE_PATH.exists()

    header = HEADER_PATH.read_text(encoding="utf-8")
    source = SOURCE_PATH.read_text(encoding="utf-8")

    assert "void DrawSidebarActivityBar(AppState& state);" in header
    assert "float SidebarRailWidth(const AppState& state);" in header
    assert "void DrawSidebarActivityBar(AppState& state)" in source
    assert "float SidebarRailWidth(const AppState& state)" in source
    assert "GetSidebarSectionVisibleFlag" in source
    assert "SidebarSizeFactor" in source
    assert "BeginTransientScrollbarPopup" in source

    for forbidden in ("main.cpp", "WindowsMain", "platform/"):
        assert forbidden not in header
        assert forbidden not in source


def test_root_delegates_sidebar_activity_bar_without_owning_drawing():
    assert '#include "SidebarActivityBar.h"' in MAIN
    assert "static void DrawSidebarActivityBar(AppState& state)" not in MAIN
    assert "static float SidebarRailWidth(const AppState& state)" not in MAIN
    assert "static float SidebarSizeFactor(" not in MAIN
    assert "static bool* GetSidebarSectionVisibleFlag(" not in MAIN
    assert "DrawSidebarActivityBar(state);" in MAIN
    assert "const float railWidth   = SidebarRailWidth(state);" in MAIN
    assert "void DrawSidebarActivityBar(AppState& state)" not in MAIN
    assert "static void DrawSidebarContentHeader(AppState& state, int sectionId)" in MAIN
    assert "static void RenderSidebarPanelContent(AppState& state, const char* idScope)" in MAIN


def test_sidebar_activity_bar_is_built_tested_and_governed():
    assert CMAKE.count("src/SidebarActivityBar.cpp") == 1
    assert BEHAVIOR.count("tests/test_sidebar_activity_bar_boundary.py") == 1
    assert RULES.count("src/SidebarActivityBar.cpp") >= 2
    assert RULES.count("src/SidebarActivityBar.h") >= 2
    assert (
        '"src/SidebarActivityBar.h": '
        '"tests/test_sidebar_activity_bar_boundary.py"'
        in STAGE7
    )
