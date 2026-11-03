"""The theme editor can save its draft as JSON on macOS."""
from pathlib import Path

root = Path(__file__).resolve().parents[1]
editor = (root / "src/ThemeEditor.cpp").read_text(encoding="utf-8")
cmake = (root / "CMakeLists.txt").read_text(encoding="utf-8")
portable = (root / "src/platform/theme_json_dialogs_std.cpp").read_text(encoding="utf-8")
native_path = root / "src/platform/theme_json_dialogs_macos.mm"

assert native_path.is_file(), "macOS theme export needs a native save panel"
native = native_path.read_text(encoding="utf-8")
assert "NSSavePanel" in native and "runModal" in native
assert "[NSThread isMainThread]" in native
assert "NSModalResponseOK" in native and "isFileURL" in native
assert "nameFieldStringValue" in native and "json" in native
assert "src/platform/theme_json_dialogs_macos.mm" in cmake
assert "#ifndef __APPLE__" in portable

suggestion = editor.split("static std::string BuildThemeJsonSuggestedPathUtf8", 1)[1].split("static std::string ThemeNameFromPathUtf8", 1)[0]
assert "#ifdef __APPLE__" in suggestion
assert 'return stem + ".json";' in suggestion
export = editor.split("ActionButton(L(ThemeEditor_ExportJson)", 1)[1].split("if (!g_ed.exportMsg.empty())", 1)[0]
assert "ImGui::SetClipboardText" in export
assert "#if defined(_WIN32) || defined(__APPLE__)" in export
assert "SaveThemeJsonFileDialogUtf8" in export
assert "ThemeColors::saveThemeToFile" in export
assert "TE_CopiedWriteFail" in export
