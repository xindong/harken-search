import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]


class ResultRowFilenameWrapContractTests(unittest.TestCase):
    def test_row_filename_painter_draws_bounded_complete_lines(self):
        main = (ROOT / "src" / "main.cpp").read_text(encoding="utf-8")
        start = main.index("static void DrawResultRowFilename(")
        end = main.index("static const char* GetSearchMatchSourceLabel", start)
        draw = main[start:end]
        self.assertIn("ResolveWrappedFilenameLayout", draw)
        self.assertIn("drawableHeight", draw)
        self.assertIn("ellipsis", draw)
        self.assertNotIn("displayName.c_str(), nullptr, wrapW", draw)

        header = (ROOT / "src" / "ui" / "ResultRowFilenameLayout.h").read_text(encoding="utf-8")
        self.assertIn("ResolveWrappedFilenameLayout", header)
        self.assertIn("std::floor", header)

    def test_row_call_site_passes_full_row_band(self):
        main = (ROOT / "src" / "main.cpp").read_text(encoding="utf-8")
        start = main.index("DrawResultRowFilename(\n")
        block = main[start:start + 400]
        self.assertIn("fixedRowHeight + listCellPadY", block)


if __name__ == "__main__":
    unittest.main()
