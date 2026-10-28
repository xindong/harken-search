from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
MAIN = (ROOT / "src" / "main.cpp").read_text(encoding="utf-8")
HEADER_PATH = ROOT / "src" / "BuiltinThemeScheme.h"
SOURCE_PATH = ROOT / "src" / "BuiltinThemeScheme.cpp"
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


def test_builtin_theme_scheme_files_own_preset_table():
    assert HEADER_PATH.exists()
    assert SOURCE_PATH.exists()

    header = HEADER_PATH.read_text(encoding="utf-8")
    source = SOURCE_PATH.read_text(encoding="utf-8")

    assert "ThemeColorScheme MakeBuiltinScheme(AppTheme t);" in header
    assert "ThemeColorScheme MakeBuiltinScheme(AppTheme t)" in source
    assert "case AppTheme::Dark:" in source
    assert "case AppTheme::JadeMist:" in source
    assert "return s;" in source

    for forbidden in ("main.cpp", "WindowsMain", "platform/"):
        assert forbidden not in header
        assert forbidden not in source

    assert "void ApplyTheme(" not in source
    assert "loadAndApplyThemeFile" not in source
    assert "useCustomColors" not in source


def test_root_delegates_builtin_scheme_without_owning_table():
    assert '#include "BuiltinThemeScheme.h"' in MAIN
    assert "static ThemeColorScheme MakeBuiltinScheme(AppTheme t)" not in MAIN
    assert "ThemeColors::current() = MakeBuiltinScheme(theme);" in MAIN
    assert "void ApplyTheme(AppTheme theme)" in MAIN
    assert "s.mainWindowBG           = H(0xff0f141b);" not in MAIN
    assert "s.wwiseLeafBadge         = H(0xff2e7c68);" not in MAIN


def test_builtin_theme_scheme_is_built_tested_and_governed():
    assert CMAKE.count("src/BuiltinThemeScheme.cpp") == 1
    assert BEHAVIOR.count("tests/test_builtin_theme_scheme_boundary.py") == 1
    assert RULES.count("src/BuiltinThemeScheme.cpp") >= 2
    assert RULES.count("src/BuiltinThemeScheme.h") >= 2
    assert (
        '"src/BuiltinThemeScheme.h": '
        '"tests/test_builtin_theme_scheme_boundary.py"'
        in STAGE7
    )
