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
        signature = "ImportThemeFromSettings(AppState& state, std::string& result)"
        self.assertIn(signature, editor)
        body = editor.split(signature, 1)[1].split(
            "enum class ThemeEditorRelationAction", 1
        )[0]
        steps = (
            "OpenThemeJsonFileDialogUtf8",
            "if (filePath.empty())",
            "LoadEditorThemeFile",
            "InstallImportedTheme",
            "TrySaveImportedAppearance",
            "TrySaveAppSettings",
            "installed != library::ImportResult::Saved",
            "ActivateInstalledTheme",
        )
        offsets = [body.index(step) for step in steps]
        self.assertEqual(offsets, sorted(offsets))
        self.assertNotIn("g_ed.draft", body)
        self.assertNotIn("loadAndApplyThemeFile", body)

    def test_unrelated_apply_does_not_cycle_schedule_or_replace_effective_appearance(self):
        dialog = source("src/SettingsDialog.inl")
        apply = dialog.split("auto apply=[&]()", 1)[1].split("auto requestClose", 1)[0]
        self.assertIn("if (themePreferencesChanged) schedule::SetMode(state,schedule::Mode::Off);", apply)
        self.assertIn("next.theme=state.settings.theme;", apply)
        self.assertIn("next.useCustomColors=state.settings.useCustomColors;", apply)
        schedule = source("src/ThemeSchedule.cpp")
        set_mode = schedule.split("void SetMode", 1)[1].split("void OnManualThemeSelected", 1)[0]
        self.assertLess(set_mode.index("if (state.settings.themeScheduleMode == mode) return;"),
                        set_mode.index("Invalidate();"))

    def test_import_holds_current_period_without_changing_automatic_mode(self):
        schedule = source("src/ThemeSchedule.cpp")
        callback = schedule.split("bool TrySaveImportedAppearance", 1)[1].split("void Invalidate", 1)[0]
        self.assertIn("ModeAfterManualTheme(previousMode)", callback)
        self.assertIn("Mode::Automatic", callback)
        self.assertIn("importOverridePeriod = SelectedPeriod(state);", callback)
        self.assertLess(callback.index("if (!persistSettings())"), callback.index("Invalidate();"))
        tick = schedule.split("void Tick", 1)[1].split("bool SavePalette", 1)[0]
        self.assertLess(tick.index("importOverridePeriod == wanted"), tick.index("ShouldApply"))
        self.assertIn("importOverridePeriod.reset();", tick)


if __name__ == "__main__":
    unittest.main()
