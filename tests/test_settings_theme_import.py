import json
from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]


def source(path):
    return (ROOT / path).read_text(encoding="utf-8")


class SettingsThemeImportTests(unittest.TestCase):
    def test_appearance_uses_same_button_slot_and_one_observable_result(self):
        fields = source("src/SettingsFields.inl")
        row = fields.split('else if(id=="themeEditor")', 1)[1].split(
            'else if(id=="language")', 1
        )[0]
        self.assertNotIn("custom-colors", row)
        self.assertIn("actionButton(L(SetUX_ImportTheme)", row)
        self.assertLess(row.index("Set_EditTheme"), row.index("SetUX_ImportTheme"))
        self.assertLess(row.index("SetUX_ImportTheme"), row.index("customThemeName"))
        self.assertIn('su::Observe("import-theme")', row)
        self.assertEqual(row.count('su::Observe("import-theme-result")'), 1)
        self.assertNotIn('editor.action=="custom-colors"', source("src/SettingsActions.inl"))
        labels = {item["id"]: item for item in json.loads(source("locale/harken.ui.strings.json"))["entries"]}
        self.assertEqual(labels["SetUX_ImportTheme"]["en"], "Import Theme")
        self.assertEqual(labels["SetUX_ImportTheme"]["zh"], "\u5bfc\u5165\u4e3b\u9898")

    def test_settings_reuses_strict_loader_and_persists_before_activation(self):
        editor = source("src/ThemeEditor.cpp")
        self.assertIn("ImportThemeFromSettings(AppState& state)", editor)
        body = editor.split("ImportThemeFromSettings(AppState& state)", 1)[1].split(
            "enum class ThemeEditorRelationAction", 1
        )[0]
        steps = (
            "OpenThemeJsonFileDialogUtf8",
            "if (filePath.empty()) return {};",
            "LoadEditorThemeFile",
            "SaveCustomThemeFiles",
            "ActivateInstalledTheme",
            "SaveAppSettings",
        )
        offsets = [body.index(step) for step in steps]
        self.assertEqual(offsets, sorted(offsets))
        self.assertNotIn("g_ed.draft", body)
        self.assertNotIn("loadAndApplyThemeFile", body)

    def test_import_holds_current_period_without_changing_automatic_mode(self):
        editor = source("src/ThemeEditor.cpp")
        install = editor.split("static void ActivateInstalledTheme", 1)[1].split(
            "static bool SaveDraftAsCustomTheme", 1
        )[0]
        self.assertLess(install.index("OnThemeImported"), install.index("activateCustomScheme"))
        schedule = source("src/ThemeSchedule.cpp")
        callback = schedule.split("void OnThemeImported", 1)[1].split("void Invalidate", 1)[0]
        self.assertIn("OnManualThemeSelected(state);", callback)
        self.assertIn("Mode::Automatic", callback)
        self.assertIn("importOverridePeriod = SelectedPeriod(state);", callback)
        tick = schedule.split("void Tick", 1)[1].split("bool SavePalette", 1)[0]
        self.assertLess(tick.index("importOverridePeriod == wanted"), tick.index("ShouldApply"))
        self.assertIn("importOverridePeriod.reset();", tick)


if __name__ == "__main__":
    unittest.main()
