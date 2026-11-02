from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
HEADER_PATH = ROOT / "src/platform/settings_import_dialog.h"
WIN32_PATH = ROOT / "src/platform/settings_import_dialog_win32.cpp"
PORTABLE_PATH = ROOT / "src/platform/settings_import_dialog_std.cpp"

assert HEADER_PATH.is_file()
assert WIN32_PATH.is_file()
assert PORTABLE_PATH.is_file()

MAIN = (ROOT / "src/main.cpp").read_text(encoding="utf-8")
HEADER = HEADER_PATH.read_text(encoding="utf-8")
WIN32 = WIN32_PATH.read_text(encoding="utf-8")
PORTABLE = PORTABLE_PATH.read_text(encoding="utf-8")
CMAKE = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
BEHAVIOR = (ROOT / "cmake/harken_search_behavior_tests.cmake").read_text(encoding="utf-8")
RULES = (ROOT / "docs/architecture/stage-4-dependency-rules.json").read_text(
    encoding="utf-8"
)
STAGE7 = (ROOT / "tools/stage7_api_contract_audit.py").read_text(encoding="utf-8")

assert "std::string OpenSettingsImportFileDialogUtf8(void* ownerWindow);" in HEADER
for token in ("windows.h", "commdlg.h", "HWND", "OPENFILENAMEW"):
    assert token not in HEADER
for forbidden in (
    '#include "main.cpp"',
    '#include "WindowsMain.h"',
    '#include "AppState.h"',
    '#include "platform/filesystem.h"',
):
    assert forbidden not in WIN32
    assert forbidden not in PORTABLE

for required in (
    "OPENFILENAMEW ofn = {};",
    "wchar_t filePath[MAX_PATH] = {};",
    "ofn.hwndOwner = static_cast<HWND>(ownerWindow);",
    'ofn.lpstrFilter = filterBuf.c_str();',
    'Lw(Dlg_FilterSettings)',
    'Lw(Dlg_FilterAllFilesPat)',
    "ofn.lpstrFile = filePath;",
    "ofn.nMaxFile = MAX_PATH;",
    "OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR",
    "GetOpenFileNameW(&ofn)",
    "TextEncoding::WideToUtf8(std::wstring(filePath))",
):
    assert required in WIN32
for forbidden_policy in (
    "OFN_PATHMUSTEXIST",
    "lpstrTitle",
    "lpstrDefExt",
    "lpstrInitialDir",
):
    assert forbidden_policy not in WIN32
assert 'return "";' in PORTABLE

qualified_call = "Platform::SettingsDialogs::OpenSettingsImportFileDialogUtf8(g_hwnd)"
assert MAIN.count(qualified_call) == 1
button = MAIN.index("aura::ui::ActionButton(L(Set_ImportSettings)")
tooltip = MAIN.index("if (HoveredForHelp()) HarkenSearch::SetTooltip", button)
body = MAIN[button:tooltip]
assert "OPENFILENAMEW" not in body
assert "GetOpenFileNameW" not in body
call = MAIN.index(qualified_call, button, tooltip)
empty_check = MAIN.index("if (!importPath.empty())", call, tooltip)
destination = MAIN.index(
    "std::string dstPath = g_appSettingsPathUtf8;", empty_check, tooltip
)
copy = MAIN.index(
    "Platform::FileSystem::CopyFile(importPath, dstPath, true)", destination, tooltip
)
reload = MAIN.index("LoadAppSettings(state);", copy, tooltip)
assert button < call < empty_check < destination < copy < reload < tooltip

assert "AURO_SETTINGS_IMPORT_DIALOG_SOURCE" in CMAKE
assert CMAKE.count("${AURO_SETTINGS_IMPORT_DIALOG_SOURCE}") == 1
assert "NAME TestSettingsImportDialogBoundary" in BEHAVIOR
assert "tests/test_settings_import_dialog_boundary.py" in BEHAVIOR
assert '"src/platform/settings_import_dialog.h"' in RULES
assert (
    '"src/platform/settings_import_dialog.h": '
    '"tests/test_settings_import_dialog_boundary.py"'
    in STAGE7
)
