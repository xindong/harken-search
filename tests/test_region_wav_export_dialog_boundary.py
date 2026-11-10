from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
HEADER_PATH = ROOT / "src/platform/region_wav_export_dialog.h"
WIN32_PATH = ROOT / "src/platform/region_wav_export_dialog_win32.cpp"
PORTABLE_PATH = ROOT / "src/platform/region_wav_export_dialog_std.cpp"

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

assert "std::string SaveRegionWavFileDialogUtf8(void* ownerWindow);" in HEADER
for token in ("windows.h", "commdlg.h", "HWND", "OPENFILENAMEW"):
    assert token not in HEADER
for forbidden in (
    '#include "main.cpp"',
    '#include "WindowsMain.h"',
    '#include "AppState.h"',
    '#include "AudioEngine.h"',
    '#include "UIRenderer.h"',
):
    assert forbidden not in WIN32
    assert forbidden not in PORTABLE

for required in (
    "OPENFILENAMEW ofn = {};",
    'wchar_t szFileName[MAX_PATH] = L"region_export.wav";',
    "ofn.hwndOwner = static_cast<HWND>(ownerWindow);",
    'ofn.lpstrFilter = filterBuf.c_str();',
    'Lw(Dlg_FilterWav)',
    "ofn.lpstrFile = szFileName;",
    "ofn.nMaxFile = MAX_PATH;",
    "ofn.Flags = OFN_OVERWRITEPROMPT;",
    'ofn.lpstrDefExt = L"wav";',
    "GetSaveFileNameW(&ofn)",
    "TextEncoding::WideToUtf8(std::wstring(szFileName))",
):
    assert required in WIN32
assert "lpstrTitle" in WIN32
assert 'return "";' in PORTABLE

qualified_call = "Platform::RegionDialogs::SaveRegionWavFileDialogUtf8(MainUiOwnerWindow())"
assert MAIN.count(qualified_call) == 1
dialog_start = MAIN.index("//  MODULE 4: Loop")
dialog_end = MAIN.index("//  MODULE 5:", dialog_start)
body = MAIN[dialog_start:dialog_end]
assert "OPENFILENAMEW" not in body
assert "GetSaveFileNameW" not in body

clicked = MAIN.index("if (exportRegionClicked)", dialog_start, dialog_end)
call = MAIN.index(qualified_call, clicked, dialog_end)
export = MAIN.index("state.audioEngine.exportRegion(outputPath", call, dialog_end)
fade_in = MAIN.index("state.uiState.regionFadeInNorm", export, dialog_end)
fade_out = MAIN.index("state.uiState.regionFadeOutNorm", fade_in, dialog_end)
error = MAIN.index("state.audioEngine.getLastExportError()", export, dialog_end)
message = MAIN.index("Platform::ShowMessageBox", error, dialog_end)
assert clicked < call < export < fade_in < fade_out < error < message

assert "AURO_REGION_WAV_EXPORT_DIALOG_SOURCE" in CMAKE
assert CMAKE.count("${AURO_REGION_WAV_EXPORT_DIALOG_SOURCE}") == 1
assert "NAME TestRegionWavExportDialogBoundary" in BEHAVIOR
assert "tests/test_region_wav_export_dialog_boundary.py" in BEHAVIOR
assert '"src/platform/region_wav_export_dialog.h"' in RULES
assert (
    '"src/platform/region_wav_export_dialog.h": '
    '"tests/test_region_wav_export_dialog_boundary.py"'
    in STAGE7
)
