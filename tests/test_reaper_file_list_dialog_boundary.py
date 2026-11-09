from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
HEADER_PATH = ROOT / "src/platform/reaper_file_list_dialog.h"
WIN32_PATH = ROOT / "src/platform/reaper_file_list_dialog_win32.cpp"
MAC_PATH = ROOT / "src/platform/reaper_file_list_dialog_macos.mm"
PORTABLE_PATH = ROOT / "src/platform/reaper_file_list_dialog_std.cpp"

assert HEADER_PATH.is_file()
assert WIN32_PATH.is_file()
assert MAC_PATH.is_file()
assert PORTABLE_PATH.is_file()

MAIN = (ROOT / "src/main.cpp").read_text(encoding="utf-8")
HEADER = HEADER_PATH.read_text(encoding="utf-8")
WIN32 = WIN32_PATH.read_text(encoding="utf-8")
MAC = MAC_PATH.read_text(encoding="utf-8")
PORTABLE = PORTABLE_PATH.read_text(encoding="utf-8")
CMAKE = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
BEHAVIOR = (ROOT / "cmake/harken_search_behavior_tests.cmake").read_text(encoding="utf-8")
RULES = (ROOT / "docs/architecture/stage-4-dependency-rules.json").read_text(
    encoding="utf-8"
)
STAGE7 = (ROOT / "tools/stage7_api_contract_audit.py").read_text(encoding="utf-8")

assert "std::string OpenReaperFileListDialogUtf8(void* ownerWindow);" in HEADER
for token in ("windows.h", "commdlg.h", "HWND", "OPENFILENAMEW"):
    assert token not in HEADER
for forbidden in (
    '#include "main.cpp"',
    '#include "WindowsMain.h"',
    '#include "Database.h"',
    '#include "AppState.h"',
):
    assert forbidden not in WIN32
    assert forbidden not in MAC
    assert forbidden not in PORTABLE

for required in (
    "OPENFILENAMEW ofn = {};",
    'wchar_t szFileName[MAX_PATH] = L"";',
    "ofn.hwndOwner = static_cast<HWND>(ownerWindow);",
    'ofn.lpstrFilter = filterBuf.c_str();',
    'Lw(Dlg_FilterReaperList)',
    'Lw(Dlg_FilterAllFilesPat)',
    "ofn.lpstrFile = szFileName;",
    "ofn.nMaxFile = MAX_PATH;",
    "OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST",
    'ofn.lpstrDefExt = L"ReaperFileList";',
    'ofn.lpstrTitle = titleBuf.c_str();',
    'Lw(Dlg_TitleImportReaperList)',
    "GetOpenFileNameW(&ofn)",
    "TextEncoding::WideToUtf8(std::wstring(szFileName))",
):
    assert required in WIN32
assert "OFN_NOCHANGEDIR" not in WIN32
assert 'return "";' in PORTABLE
for required in ("NSOpenPanel", "[NSThread isMainThread]", "runModal", "isFileURL", "UTF8String"):
    assert required in MAC

qualified_call = "Platform::ReaperDialogs::OpenReaperFileListDialogUtf8(ownerWindow)"
assert MAIN.count(qualified_call) == 1
assert "static std::string OpenReaperFileListDialogUtf8(HWND owner)" not in MAIN
assert MAIN.count("OpenReaperFileListDialogUtf8(") == 1

menu = MAIN.index('ImGui::MenuItem(L(Main_ImportReaperList))')
guard = MAIN.rfind("#if defined(_WIN32) || defined(__APPLE__)", 0, menu)
call = MAIN.index(qualified_call, menu)
empty_check = MAIN.index("if (!importPath.empty())", call)
snapshot = MAIN.index("state.playlists = state.database.getAllPlaylists();", empty_check)
stem = MAIN.index("PathToUtf8Main(Utf8ToPathMain(importPath).stem())", snapshot)
names = MAIN.index("std::unordered_set<std::string> existingNames;", stem)
suffix = MAIN.index("existingNames.count(playlistName)", names)
start = MAIN.index(
    "aura::reaper_import::Start(state, importPath, playlistName);", suffix
)
guard_end = MAIN.index("#endif", start)
assert guard < menu < call < empty_check < snapshot < stem < names < suffix < start < guard_end

assert "AURO_REAPER_FILE_LIST_DIALOG_SOURCE" in CMAKE
assert CMAKE.count("${AURO_REAPER_FILE_LIST_DIALOG_SOURCE}") == 1
assert "src/platform/reaper_file_list_dialog_macos.mm" in CMAKE
assert "NAME TestReaperFileListDialogBoundary" in BEHAVIOR
assert "tests/test_reaper_file_list_dialog_boundary.py" in BEHAVIOR
assert '"src/platform/reaper_file_list_dialog.h"' in RULES
assert (
    '"src/platform/reaper_file_list_dialog.h": '
    '"tests/test_reaper_file_list_dialog_boundary.py"'
    in STAGE7
)
