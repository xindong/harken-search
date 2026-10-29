from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
MAIN = (ROOT / "src/main.cpp").read_text(encoding="utf-8")
HEADER = (ROOT / "src/platform/database_file_dialog.h").read_text(encoding="utf-8")
WIN32 = (ROOT / "src/platform/database_file_dialog_win32.cpp").read_text(encoding="utf-8")
PORTABLE = (ROOT / "src/platform/database_file_dialog_std.cpp").read_text(encoding="utf-8")
BEHAVIOR = (ROOT / "cmake/harken_search_behavior_tests.cmake").read_text(encoding="utf-8")
RULES = (ROOT / "docs/architecture/stage-4-dependency-rules.json").read_text(encoding="utf-8")
STAGE7 = (ROOT / "tools/stage7_api_contract_audit.py").read_text(encoding="utf-8")

legacy_decl = "std::string OpenDatabaseFileDialogUtf8(void* ownerWindow);"
initial_decl = (
    "std::string OpenDatabaseFileDialogWithInitialDirectoryUtf8(\n"
    "    void* ownerWindow,\n"
    "    const std::wstring& initialDirectory);"
)
assert legacy_decl in HEADER
assert initial_decl in HEADER

for native_token in ("windows.h", "commdlg.h", "HWND", "OPENFILENAMEW"):
    assert native_token not in HEADER

legacy_start = WIN32.index("std::string OpenDatabaseFileDialogUtf8(void* ownerWindow)")
initial_start = WIN32.index("std::string OpenDatabaseFileDialogWithInitialDirectoryUtf8(")
legacy_body = WIN32[legacy_start:initial_start]
initial_body = WIN32[initial_start:]

assert "lpstrInitialDir" not in legacy_body
for required in (
    "OPENFILENAMEW ofn = {};",
    "ofn.hwndOwner = static_cast<HWND>(ownerWindow);",
    "ofn.lpstrInitialDir = initialDirectory.c_str();",
    "OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST",
    'ofn.lpstrDefExt = L"db";',
    "GetOpenFileNameW(&ofn)",
    "TextEncoding::WideToUtf8(std::wstring(szFileName))",
):
    assert required in initial_body
assert 'ofn.lpstrFilter = filterBuf.c_str();' in initial_body
assert 'Lw(Dlg_FilterArisDb)' in initial_body

assert PORTABLE.count('return "";') == 2
assert "OpenDatabaseFileDialogWithInitialDirectoryUtf8" in PORTABLE

legacy_call = "Platform::DatabaseDialogs::OpenDatabaseFileDialogUtf8(g_hwnd)"
initial_call = "Platform::DatabaseDialogs::OpenDatabaseFileDialogWithInitialDirectoryUtf8("
assert MAIN.count(legacy_call) == 1
assert MAIN.count(initial_call) == 5  # Includes the required first-library setup.
assert MAIN.count("GetDefaultDbDirWide()") >= 4
assert "static std::string OpenDatabaseFileDialogUtf8(HWND owner)" not in MAIN

assert "NAME TestDatabaseOpenDialogBoundary" in BEHAVIOR
assert "tests/test_database_open_dialog_boundary.py" in BEHAVIOR
assert '"src/platform/database_file_dialog.h"' in RULES
assert (
    '"src/platform/database_file_dialog.h": "tests/test_stage7_platform_contract.py"'
    in STAGE7
)
