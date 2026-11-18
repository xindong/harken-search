import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]


def _filename_row_draw_block(main: str) -> str:
    start = main.index(
        "std::string dynamicDisplayName = GetDisplayFilename(record.fileName, state.settings.showFileExtension);"
    )
    end = main.index("if (rowSelectClicked)", start)
    return main[start:end]


class SearchHitInlineHighlightSourceContractTests(unittest.TestCase):
    def test_non_wrap_filename_is_not_selectable_label(self):
        main = (ROOT / "src" / "main.cpp").read_text(encoding="utf-8")
        block = _filename_row_draw_block(main)
        self.assertNotIn('dynamicDisplayName + "##RowSelect"', block)
        self.assertIn('"##RowSelect"', block)
        self.assertIn("wrapFileName", block)

    def test_result_row_filename_is_hand_drawn_without_search_hit_spans(self):
        main = (ROOT / "src" / "main.cpp").read_text(encoding="utf-8")
        block = _filename_row_draw_block(main)

        self.assertIn("DrawResultRowFilename", block)
        self.assertNotIn("BuildResultRowFilenameHighlightSpans", block)
        self.assertNotIn("nameHitSpans", block)
        self.assertNotIn("state.lastAppliedSearchText", block)
        self.assertNotIn("record.matchSnippet", block)

        self.assertNotIn("dynamicDisplayName.c_str(), nullptr, wrapW", block)
        self.assertNotRegex(
            block,
            r"if \(wrapFileName\) \{\s*ImDrawList\* dlName",
        )

        self.assertNotIn("static std::vector<std::pair<size_t, size_t>> BuildResultRowFilenameHighlightSpans", main)
        merge_header = (ROOT / "src" / "ResultRowHighlightSpans.h").read_text(encoding="utf-8")
        self.assertIn("std::lower_bound", merge_header)
        self.assertIn("span.first < value", merge_header)

        draw_start = main.index("DrawResultRowFilename")
        draw = main[draw_start : draw_start + 3500]
        self.assertIn("AddRectFilled", draw)
        self.assertIn("0.38f", draw)
        self.assertIn("wrapFileName", block)

    def test_scope_stays_on_result_row_filename_not_settings_combo(self):
        main = (ROOT / "src" / "main.cpp").read_text(encoding="utf-8")
        block = _filename_row_draw_block(main)
        self.assertNotIn("ImGui::Combo", block)
        self.assertNotIn("showSettingsWindow", block)
        self.assertNotIn("BeginCombo", block)

    def test_result_table_does_not_scroll_or_reveal_search_hit_column(self):
        main = (ROOT / "src" / "main.cpp").read_text(encoding="utf-8")
        self.assertNotIn("ResolveSearchHitColumnIndex", main)
        self.assertNotIn("searchHitColumnIndex", main)
        self.assertNotIn("revealSearchHitColumn", main)
        self.assertNotIn("SetScrollHereX", main)
        self.assertNotIn("static void DrawResultRowFieldText", main)
        self.assertNotIn("ResultRowFieldMatchesSearchHit", main)

        table_start = main.index("const ImGuiID resultsTableId = ImGui::GetID(resultsTableName);")
        table_end = main.index("const bool isResultsWaveColumnVisible =", table_start)
        table = main[table_start:table_end]
        self.assertNotIn("ResolveSearchHitColumnIndex", table)
        self.assertNotIn("SetScrollHereX", table)
        self.assertNotIn("showSettingsWindow", table)

    def test_visible_match_fields_use_plain_text_not_hit_spans(self):
        main = (ROOT / "src" / "main.cpp").read_text(encoding="utf-8")
        self.assertNotIn("static void DrawResultRowFieldText", main)
        self.assertNotIn("DrawResultRowFieldText(", main)

        desc_start = main.index("// Description column should always reflect real description")
        desc = main[desc_start : desc_start + 900]
        self.assertIn("TextUnformatted", desc)
        self.assertIn("record.description", desc)
        self.assertIn('"-"', desc)

        kw_start = main.index("// Keywords (L2 gray)")
        kw = main[kw_start : kw_start + 500]
        self.assertIn("TextUnformatted", kw)
        self.assertIn("record.keywords", kw)


if __name__ == "__main__":
    unittest.main()
