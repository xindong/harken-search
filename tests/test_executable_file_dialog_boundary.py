from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
HEADER_PATH = ROOT / "src/platform/executable_file_dialog.h"
WIN32_PATH = ROOT / "src/platform/executable_file_dialog_win32.cpp"
PORTABLE_PATH = ROOT / "src/platform/executable_file_dialog_std.cpp"

assert HEADER_PATH.is_file()
assert WIN32_PATH.is_file()
assert PORTABLE_PATH.is_file()

MAIN = (ROOT / "src/main.cpp").read_text(encoding="utf-8")
HEADER = HEADER_PATH.read_text(encoding="utf-8")
WIN32 = WIN32_PATH.read_text(encoding="utf-8")
PORTABLE = PORTABLE_PATH.read_text(encoding="utf-8")
CMAKE = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
BEHAVIOR = (ROOT / "cmake/harken_search_behavior_tests.cmake").read_text(encoding="utf-8")
RULES = (ROOT / "docs/architecture/stage-4-dependency-rules.json").read_text(encoding="utf-8")
STAGE7 = (ROOT / "tools/stage7_api_contract_audit.py").read_text(encoding="utf-8")

declaration = (
    "std::string OpenExecutableFileDialogUtf8(\n"
    "    void* ownerWindow,\n"
    "    const std::wstring& title,\n"
    "    const std::wstring& initialPath);"
)
assert declaration in HEADER
for native_token in ("windows.h", "commdlg.h", "HWND", "OPENFILENAMEW"):
    assert native_token not in HEADER

for forbidden_include in ('#include "main.cpp"', '#include "WindowsMain.h"'):
    assert forbidden_include not in WIN32
    assert forbidden_include not in PORTABLE

for required in (
    "OPENFILENAMEW ofn = {};",
    'wchar_t szFileName[MAX_PATH] = L"";',
    "if (!initialPath.empty())",
    'normalized.find_last_of(L"\\\\/")',
    "initialDir = normalized.substr(0, slashPos);",
    "wcsncpy_s(szFileName, initialPath.c_str(), _TRUNCATE);",
    "ofn.hwndOwner = static_cast<HWND>(ownerWindow);",
    'ofn.lpstrFilter = filterBuf.c_str();',
    'Lw(Dlg_FilterExecutable)',
    'Lw(Dlg_FilterAllFilesPat)',
    "ofn.lpstrInitialDir = initialDir.empty() ? nullptr : initialDir.c_str();",
    "OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST",
    'ofn.lpstrDefExt = L"exe";',
    "ofn.lpstrTitle = title.c_str();",
    "GetOpenFileNameW(&ofn)",
    "TextEncoding::WideToUtf8(std::wstring(szFileName))",
):
    assert required in WIN32

assert 'return "";' in PORTABLE
assert "OpenExecutableFileDialogUtf8" in PORTABLE

qualified_call = "Platform::ExecutableDialogs::OpenExecutableFileDialogUtf8("
assert MAIN.count(qualified_call) == 1
assert "static std::string OpenExecutableFileDialogUtf8(HWND owner" not in MAIN
assert MAIN.count("OpenExecutableFileDialogUtf8(") == 1

browse = MAIN.index('aura::ui::ActionButton(L(Main_DawBrowseBtn)')
title = MAIN.index('std::wstring title = L"Select "', browse)
call = MAIN.index(qualified_call, title)
empty_check = MAIN.index("if (!picked.empty())", call)
assign = MAIN.index("selected.exePath = picked;", empty_check)
buffer_copy = MAIN.index("strncpy_s(s_dawExePathBuf", assign)
clear = MAIN.index('aura::ui::ActionButton(L(Main_DawClearBtn)', buffer_copy)
assert browse < title < call < empty_check < assign < buffer_copy < clear
assert "Utf8ToWide(selected.exePath)" in MAIN[title:empty_check]
assert "L(Daw_PickExeHint)" in MAIN

assert "AURO_EXECUTABLE_FILE_DIALOG_SOURCE" in CMAKE
assert CMAKE.count("${AURO_EXECUTABLE_FILE_DIALOG_SOURCE}") == 1
assert "NAME TestExecutableFileDialogBoundary" in BEHAVIOR
assert "tests/test_executable_file_dialog_boundary.py" in BEHAVIOR
assert '"src/platform/executable_file_dialog.h"' in RULES
assert (
    '"src/platform/executable_file_dialog.h": '
    '"tests/test_executable_file_dialog_boundary.py"'
    in STAGE7
)
