import pathlib
import re
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]


def _read(rel: str) -> str:
    return (ROOT / rel).read_text(encoding="utf-8")


def _settings_toggle_row(widgets: str) -> str:
    start = widgets.index("bool SettingsToggleRow(")
    end = widgets.index("bool SettingsSliderRow(", start)
    return widgets[start:end]


def _settings_button_row(widgets: str) -> str:
    start = widgets.index("bool SettingsButtonRow(")
    end = widgets.index("void TooltipRich(", start)
    return widgets[start:end]


class SettingsSearchHitHighlightSourceContractTests(unittest.TestCase):







    # Search now presents a result list and opens the destination page. The
    # old previous/next cursor and hand-maintained body-scroll are retired.
    # Catalog behavior is exercised by TestPreferenceDraft; actual scrolling
    # and popup input are exercised by qa_settings_redesign.py.
    def test_search_target_keeps_label_highlight_and_opens_advanced_group(self):
        dialog = _read("src/SettingsDialog.inl")
        self.assertIn("editor.highlightId=entry->id", dialog)
        self.assertIn("editor.highlightQuery=editor.query", dialog)
        self.assertIn("BeginSettingsSearchHighlight", dialog)
        self.assertIn("DrawSettingsSearchableLabel(entryLabel)", dialog)
        self.assertIn("EndSettingsSearchHighlight", dialog)
        self.assertIn("ImGui::SetNextItemOpen(true)", dialog)
        self.assertIn("ImGui::SetScrollHereY", dialog)

    def test_shared_highlight_helpers_still_accept_keywords(self):
        widgets = _read("src/HarkenSearchWidgets.cpp")
        self.assertIn("BuildSettingsLabelHighlightSpans", widgets)
        self.assertIn("NotifySettingsSearchLabeledItem", _settings_toggle_row(widgets))
        self.assertIn("NotifySettingsSearchLabeledItem", _settings_button_row(widgets))
        self.assertIn("EnsureActiveSettingsLabelHighlight", widgets)

    def test_radio_keeps_caller_text_color(self):
        widgets = _read("src/HarkenSearchWidgets.cpp")
        radio_fn = widgets[widgets.index("bool Radio(") : widgets.index("bool Toggle(")]
        self.assertIn("GetStyleColorVec4(ImGuiCol_Text)", radio_fn)
        self.assertNotIn("th.interfaceText", radio_fn)

    def test_empty_query_does_not_force_label_highlight(self):
        widgets = _read("src/HarkenSearchWidgets.cpp")
        notify = widgets[widgets.index("void NotifySettingsSearchLabeledItem") :]
        notify = notify[: notify.index("\nvoid ", 1) if "\nvoid " in notify[1:] else 2500]
        self.assertIn("g_settingsSearchQuery", notify)
        self.assertTrue(
            "empty()" in notify or "empty" in notify,
            "highlight path must bail out when the settings query is empty",
        )

    def test_overlay_pulses_hit_glyphs_then_settles_steady(self):
        widgets = _read("src/HarkenSearchWidgets.cpp")
        overlay = widgets[widgets.index("void OverlayLabelHighlight(") : widgets.index("void BeginSettingsSearchHighlight(")]
        self.assertIn("ImGui::GetTime()", overlay)
        executable_overlay = re.sub(r"/\*.*?\*/", "", overlay, flags=re.S)
        executable_overlay = re.sub(r"//.*?$", "", executable_overlay, flags=re.M)
        self.assertIn("const float alpha = SettingsSearchHighlightAlpha(elapsed);", executable_overlay)
        self.assertNotIn("0.15f", overlay)
        self.assertNotIn("0.55f", overlay)
        self.assertNotIn("0.38f", overlay)
        self.assertIn("spans.empty()", overlay)
        self.assertIn("AddRectFilled", overlay)
        # Pulse only the matching glyphs, not the whole control.
        self.assertIn("hiSize", overlay)
        self.assertNotIn("itemMax.x, itemMax.y", overlay)

        begin = widgets[widgets.index("void BeginSettingsSearchHighlight(") : widgets.index("void EndSettingsSearchHighlight(")]
        self.assertIn("ImGui::GetTime()", begin)
        self.assertIn("g_settingsSearchLastOverlayId = 0", begin)
        self.assertIn("g_settingsSearchQuery", begin)
        self.assertIn("g_settingsSearchActiveLabel", begin)
        self.assertIn("g_settingsSearchKeywords", begin)


if __name__ == "__main__":
    unittest.main()
