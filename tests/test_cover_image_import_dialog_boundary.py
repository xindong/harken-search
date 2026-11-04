from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
HEADER_PATH = ROOT / "src/platform/cover_image_import_dialog.h"
WIN32_PATH = ROOT / "src/platform/cover_image_import_dialog_win32.cpp"
PORTABLE_PATH = ROOT / "src/platform/cover_image_import_dialog_std.cpp"

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

assert "std::string OpenCoverImageImportFileDialogUtf8(void* ownerWindow);" in HEADER
for token in ("windows.h", "commdlg.h", "HWND", "OPENFILENAMEW"):
    assert token not in HEADER
for forbidden in (
    '#include "main.cpp"',
    '#include "WindowsMain.h"',
    '#include "AppState.h"',
    '#include "MetadataWriter.h"',
    '#include "CoverModule.h"',
    '#include "Database.h"',
):
    assert forbidden not in WIN32
    assert forbidden not in PORTABLE

for required in (
    "OPENFILENAMEW ofn = {0};",
    "wchar_t szFile[260] = {0};",
    "ofn.hwndOwner = static_cast<HWND>(ownerWindow);",
    "ofn.lpstrFile = szFile;",
    "ofn.nMaxFile = sizeof(szFile) / sizeof(szFile[0]);",
    "ofn.nFilterIndex = 1;",
    "ofn.lpstrFilter = filterBuf.c_str();",
    "ofn.lpstrTitle = titleBuf.c_str();",
    "OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST",
    "GetOpenFileNameW(&ofn)",
    "TextEncoding::WideToUtf8(std::wstring(szFile))",
):
    assert required in WIN32
for forbidden_policy in ("OFN_NOCHANGEDIR", "lpstrDefExt", "lpstrInitialDir"):
    assert forbidden_policy not in WIN32
assert 'return "";' in PORTABLE

qualified_call = "Platform::CoverDialogs::OpenCoverImageImportFileDialogUtf8(MainUiOwnerWindow())"
assert MAIN.count(qualified_call) == 1
dialog_start = MAIN.index("// COVER IMPORT DIALOG")
dialog_end = MAIN.index("// AI 封面生成弹窗", dialog_start)
body = MAIN[dialog_start:dialog_end]
assert "OPENFILENAMEW" not in body
assert "GetOpenFileNameW" not in body

show = MAIN.index("if (state.showCoverImportDialog)", dialog_start, dialog_end)
call = MAIN.index(qualified_call, show, dialog_end)
empty_check = MAIN.index("if (!imagePath.empty())", call, dialog_end)
deferred = MAIN.index("if (state.coverImportDeferredApply)", empty_check, dialog_end)
store = MAIN.index("state.coverImportImagePath = imagePath;", deferred, dialog_end)
load_all = MAIN.index("StartLoadAllInBatches(state);", store, dialog_end)
targets = MAIN.index("std::vector<std::string> importTargets", load_all, dialog_end)
decode = MAIN.index("MetadataWriter::loadImageFile(imagePath", targets, dialog_end)
kind = MAIN.index("pendingOriginalAudioWriteKind", decode, dialog_end)
paths = MAIN.index("pendingOriginalAudioWritePaths", kind, dialog_end)
request = MAIN.index("RequestOriginalAudioWriteConfirm(state)", paths, dialog_end)
hide = MAIN.index("state.showCoverImportDialog = false;", request, dialog_end)
cleanup_guard = MAIN.index("if (!state.coverImportDeferredApply &&", hide, dialog_end)
clear_ids = MAIN.index("state.coverImportIds.clear();", cleanup_guard, dialog_end)
clear_paths = MAIN.index("state.coverImportPaths.clear();", clear_ids, dialog_end)
assert show < call < empty_check < deferred < store < load_all < targets < decode
assert decode < kind < paths < request < hide < cleanup_guard < clear_ids < clear_paths
assert "MetadataWriter::writeAlbumArt" not in body
assert "ImportCover" in body

assert "AURO_COVER_IMAGE_IMPORT_DIALOG_SOURCE" in CMAKE
assert CMAKE.count("${AURO_COVER_IMAGE_IMPORT_DIALOG_SOURCE}") == 1
assert "NAME TestCoverImageImportDialogBoundary" in BEHAVIOR
assert "tests/test_cover_image_import_dialog_boundary.py" in BEHAVIOR
assert '"src/platform/cover_image_import_dialog.h"' in RULES
assert (
    '"src/platform/cover_image_import_dialog.h": '
    '"tests/test_cover_image_import_dialog_boundary.py"'
    in STAGE7
)
