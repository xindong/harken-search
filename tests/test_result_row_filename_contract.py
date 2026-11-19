import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]


class ResultRowFilenameWrapContractTests(unittest.TestCase):
    def test_row_filename_painter_draws_bounded_complete_lines(self):
        main = (ROOT / "src" / "main.cpp").read_text(encoding="utf-8")
        start = main.index("static void DrawResultRowFilename(")
        end = main.index("static const char* GetSearchMatchSourceLabel", start)
        draw = main[start:end]
        self.assertIn("ResolveWrappedFilenameLineCount", draw)
        self.assertIn("drawableHeight", draw)
        self.assertIn("ellipsis", draw)
        self.assertNotIn("displayName.c_str(), nullptr, wrapW", draw)
        self.assertNotIn("filenameLayout", draw)

        header = (ROOT / "src" / "ui" / "ResultRowFilenameLayout.h").read_text(encoding="utf-8")
        self.assertIn("ResolveWrappedFilenameLineCount", header)
        self.assertIn("std::floor", header)

    def test_result_row_is_raised_to_three_whole_lines(self):
        main = (ROOT / "src" / "main.cpp").read_text(encoding="utf-8")
        start = main.index("const float wrappedNameRowHeightFloor")
        block = main[start:start + 600]
        self.assertIn("3.0f * listFontPx", block)
        self.assertIn("(std::max)(targetRowHeight, wrappedNameRowHeightFloor)", block)

    def test_row_call_site_passes_full_row_band(self):
        main = (ROOT / "src" / "main.cpp").read_text(encoding="utf-8")
        start = main.index("nameColAvailW,")
        block = main[start:start + 200]
        self.assertIn("fixedRowHeight + listCellPadY", block)


if __name__ == "__main__":
    unittest.main()
