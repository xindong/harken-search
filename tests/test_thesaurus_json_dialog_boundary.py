from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
HEADER_PATH = ROOT / "src/platform/thesaurus_json_dialog.h"
WIN32_PATH = ROOT / "src/platform/thesaurus_json_dialog_win32.cpp"
PORTABLE_PATH = ROOT / "src/platform/thesaurus_json_dialog_std.cpp"
MACOS_PATH = ROOT / "src/platform/thesaurus_json_dialog_macos.mm"

assert HEADER_PATH.is_file()
assert WIN32_PATH.is_file()
assert PORTABLE_PATH.is_file()

MAIN = (ROOT / "src/main.cpp").read_text(encoding="utf-8")
SETTINGS_DIALOG = (ROOT / "src/SettingsDialog.inl").read_text(encoding="utf-8")
SETTINGS_ACTIONS = (ROOT / "src/SettingsActions.inl").read_text(encoding="utf-8")
SETTINGS_THESAURUS = (ROOT / "src/SettingsThesaurusTools.inl").read_text(encoding="utf-8")
HEADER = HEADER_PATH.read_text(encoding="utf-8")
WIN32 = WIN32_PATH.read_text(encoding="utf-8")
PORTABLE = PORTABLE_PATH.read_text(encoding="utf-8")
CMAKE = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
BEHAVIOR = (ROOT / "cmake/harken_search_behavior_tests.cmake").read_text(encoding="utf-8")
RULES = (ROOT / "docs/architecture/stage-4-dependency-rules.json").read_text(
    encoding="utf-8"
)
STAGE7 = (ROOT / "tools/stage7_api_contract_audit.py").read_text(encoding="utf-8")

assert "std::string OpenThesaurusJsonFileDialogUtf8(void* ownerWindow);" in HEADER
for token in ("windows.h", "commdlg.h", "HWND", "OPENFILENAMEW"):
    assert token not in HEADER
for forbidden in (
    '#include "main.cpp"',
    '#include "WindowsMain.h"',
    '#include "AppState.h"',
    '#include "Localization.h"',
    '#include "SearchQuery.h"',
):
    assert forbidden not in WIN32
    assert forbidden not in PORTABLE

for required in (
    "OPENFILENAMEW ofn = {};",
    'wchar_t szFileName[MAX_PATH] = L"";',
    "ofn.hwndOwner = static_cast<HWND>(ownerWindow);",
    'ofn.lpstrFilter = L"JSON Files\\0*.json\\0All Files\\0*.*\\0";',
    "ofn.lpstrFile = szFileName;",
    "ofn.nMaxFile = MAX_PATH;",
    "OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST",
    "GetOpenFileNameW(&ofn)",
    "TextEncoding::WideToUtf8(std::wstring(szFileName))",
):
    assert required in WIN32
for forbidden_policy in (
    "OFN_NOCHANGEDIR",
    "lpstrTitle",
    "lpstrDefExt",
    "lpstrInitialDir",
):
    assert forbidden_policy not in WIN32
assert 'return "";' in PORTABLE
assert MACOS_PATH.is_file(), "macOS needs a native JSON chooser"
MACOS = MACOS_PATH.read_text(encoding="utf-8")
assert "NSOpenPanel" in MACOS
assert "runModal" in MACOS
assert "NSModalResponseOK" in MACOS
assert "src/platform/thesaurus_json_dialog_macos.mm" in CMAKE

qualified_call = (
    "Platform::ThesaurusDialogs::OpenThesaurusJsonFileDialogUtf8(MainUiOwnerWindow())"
)
assert '#include "SettingsDialog.inl"' in MAIN
assert '#include "SettingsActions.inl"' in SETTINGS_DIALOG
assert '#include "SettingsThesaurusTools.inl"' in SETTINGS_ACTIONS
assert SETTINGS_THESAURUS.count(qualified_call) == 1
merge_button = SETTINGS_THESAURUS.index("aura::ui::ActionButton(L(Set_ThesMerge)")
reset_button = SETTINGS_THESAURUS.index("aura::ui::ActionButton(L(Set_ThesResetDefault)", merge_button)
merge_body = SETTINGS_THESAURUS[merge_button:reset_button]
assert "OPENFILENAMEW" not in merge_body
assert "GetOpenFileNameW" not in merge_body

guard = SETTINGS_THESAURUS.index("#if defined(_WIN32) || defined(__APPLE__)", merge_button, reset_button)
call = SETTINGS_THESAURUS.index(qualified_call, guard, reset_button)
empty_check = SETTINGS_THESAURUS.index("if (!importPath.empty())", call, reset_button)
json_decl = SETTINGS_THESAURUS.index("std::string json;", empty_check, reset_button)
read = SETTINGS_THESAURUS.index(
    "Platform::FileSystem::ReadFileText(importPath, json)", json_decl, reset_button
)
groups = SETTINGS_THESAURUS.index("std::vector<ThesaurusGroup> imported;", read, reset_button)
parse = SETTINGS_THESAURUS.index("ParseThesaurusJson(json, imported);", groups, reset_button)
append = SETTINGS_THESAURUS.index("g_userThesaurus.insert(", parse, reset_button)
save = SETTINGS_THESAURUS.index("SaveThesaurusJson();", append, reset_button)
guard_end = SETTINGS_THESAURUS.index("#endif", save, reset_button)
assert (
    merge_button
    < guard
    < call
    < empty_check
    < json_decl
    < read
    < groups
    < parse
    < append
    < save
    < guard_end
    < reset_button
)

assert "AURO_THESAURUS_JSON_DIALOG_SOURCE" in CMAKE
assert CMAKE.count("${AURO_THESAURUS_JSON_DIALOG_SOURCE}") == 1
assert "NAME TestThesaurusJsonDialogBoundary" in BEHAVIOR
assert "tests/test_thesaurus_json_dialog_boundary.py" in BEHAVIOR
assert '"src/platform/thesaurus_json_dialog.h"' in RULES
assert (
    '"src/platform/thesaurus_json_dialog.h": '
    '"tests/test_thesaurus_json_dialog_boundary.py"'
    in STAGE7
)
