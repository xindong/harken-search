from pathlib import Path


root = Path(__file__).resolve().parents[1]
editor = (root / "src/ThemeEditor.cpp").read_text(encoding="utf-8")
cmake = (root / "CMakeLists.txt").read_text(encoding="utf-8")
mac_source = root / "src/platform/theme_json_dialogs_macos.mm"

assert mac_source.is_file(), "macOS theme import needs a native file chooser"
mac = mac_source.read_text(encoding="utf-8")
assert "NSOpenPanel" in mac and "runModal" in mac
assert "NSModalResponseOK" in mac and "isFileURL" in mac
assert "src/platform/theme_json_dialogs_macos.mm" in cmake

for first, last in (
    ("ThemeEditor_ImportJson", "ThemeEditor_ImportCreateTheme"),
    ("ThemeEditor_ImportCreateTheme", "ThemeEditor_ExportJson"),
):
    section = editor[editor.index("ActionButton(L(" + first):editor.index("ActionButton(L(" + last)]
    assert "#if defined(_WIN32) || defined(__APPLE__)" in section
    assert "OpenThemeJsonFileDialogUtf8(MainUiOwnerWindow(), suggestedPath)" in section
    assert "loadAndApplyThemeFile" in section
    assert "if (!filePath.empty())" in section

assert "SaveThemeJsonFileDialogUtf8(g_hwnd, suggestedPath)" in editor
