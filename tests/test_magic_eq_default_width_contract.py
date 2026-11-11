from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
HEADER = (ROOT / "src" / "CenterSenseDsp.h").read_text(encoding="utf-8")
ENGINE = (ROOT / "src" / "AudioEngine.cpp").read_text(encoding="utf-8")
MAIN = (ROOT / "src" / "main.cpp").read_text(encoding="utf-8")


class MagicEqDefaultWidthContractTests(unittest.TestCase):
    def test_default_width_is_one_named_520_design_value(self):
        self.assertRegex(HEADER, r"kDefaultMagicEQWidgetWidth\s*=\s*520\.0f")
        self.assertIn("float widgetWidth = kDefaultMagicEQWidgetWidth;", HEADER)

    def test_invalid_engine_width_falls_back_to_the_same_default(self):
        self.assertIn("next.widgetWidth = CenterSenseDsp::kDefaultMagicEQWidgetWidth;", ENGINE)

    def test_saved_width_still_overrides_the_default(self):
        self.assertIn('magicEQ.widgetWidth = ParseFloat(kv["magicEqWidgetWidth"], magicEQ.widgetWidth);', MAIN)
        self.assertIn('items.push_back({"magicEqWidgetWidth", std::to_string(magicEQ.widgetWidth)});', MAIN)


if __name__ == "__main__":
    unittest.main()
