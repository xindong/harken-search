from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
HEADER_PATH = ROOT / "src/platform/avatar_image_file_dialog.h"
WIN32_PATH = ROOT / "src/platform/avatar_image_file_dialog_win32.cpp"
PORTABLE_PATH = ROOT / "src/platform/avatar_image_file_dialog_std.cpp"

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

assert "std::string OpenAvatarImageFileDialogUtf8(void* ownerWindow);" in HEADER
for token in ("windows.h", "commdlg.h", "HWND", "OPENFILENAMEW"):
    assert token not in HEADER
for forbidden in (
    '#include "main.cpp"',
    '#include "WindowsMain.h"',
    '#include "AvatarCropper.h"',
    '#include "AvatarCropperIO.h"',
):
    assert forbidden not in WIN32
    assert forbidden not in PORTABLE

for required in (
    "OPENFILENAMEW ofn = {};",
    'wchar_t szFileName[MAX_PATH] = L"";',
    "ofn.hwndOwner = static_cast<HWND>(ownerWindow);",
    'ofn.lpstrFilter = filterBuf.c_str();',
    'Lw(Dlg_FilterImageFiles)',
    'Lw(Dlg_FilterAllFiles)',
    "ofn.lpstrFile = szFileName;",
    "ofn.nMaxFile = MAX_PATH;",
    "OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR",
    "GetOpenFileNameW(&ofn)",
    "TextEncoding::WideToUtf8(std::wstring(szFileName))",
):
    assert required in WIN32
assert 'return "";' in PORTABLE

qualified_call = "Platform::AvatarDialogs::OpenAvatarImageFileDialogUtf8(g_hwnd)"
assert MAIN.count(qualified_call) == 1
assert "static std::string OpenAvatarImageFileDialogUtf8(HWND owner)" not in MAIN
assert MAIN.count("OpenAvatarImageFileDialogUtf8(") == 1

button = MAIN.index("aura::ui::ActionButton(L(Set_AvatarChangeLabelZh))")
call = MAIN.index(qualified_call, button)
empty_check = MAIN.index("if (!selected.empty())", call)
clear = MAIN.index("s_cropPixels.clear();", empty_check)
load = MAIN.index("LoadImageForCrop(selected", clear)
crop = MAIN.index("s_cropState = InitCropState", load)
delete_texture = MAIN.index("glDeleteTextures", crop)
generate_texture = MAIN.index("glGenTextures", delete_texture)
bind_texture = MAIN.index(
    "glBindTexture(GL_TEXTURE_2D, s_cropPreviewGLTex)", generate_texture
)
upload_texture = MAIN.index("glTexImage2D", bind_texture)
unbind_texture = MAIN.index("glBindTexture(GL_TEXTURE_2D, 0)", upload_texture)
open_modal = MAIN.index('ImGui::OpenPopup(L(AvatarCropTitle))', unbind_texture)
assert (
    button
    < call
    < empty_check
    < clear
    < load
    < crop
    < delete_texture
    < generate_texture
    < bind_texture
    < upload_texture
    < unbind_texture
    < open_modal
)

assert "AURO_AVATAR_IMAGE_FILE_DIALOG_SOURCE" in CMAKE
assert CMAKE.count("${AURO_AVATAR_IMAGE_FILE_DIALOG_SOURCE}") == 1
assert "NAME TestAvatarImageFileDialogBoundary" in BEHAVIOR
assert "tests/test_avatar_image_file_dialog_boundary.py" in BEHAVIOR
assert '"src/platform/avatar_image_file_dialog.h"' in RULES
assert (
    '"src/platform/avatar_image_file_dialog.h": '
    '"tests/test_avatar_image_file_dialog_boundary.py"'
    in STAGE7
)
