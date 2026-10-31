from pathlib import Path
import sys
import tempfile
import unittest
from PIL import Image, ImageDraw
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from qa_info_family_themes import audit_captures, digest


class TextPixelIdentity(unittest.TestCase):
    def audit(self, color, count):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            crops = {}
            for module, positions in {
                "similar-sounds": [(10, 8), (25, 52), (80, 52), (545, 52)],
                "info": [(25, 54)],
            }.items():
                picture = Image.new("RGB", (640, 620), (17, 23, 31))
                valid_inks = [(193,201,209), (139,150,165), (230,236,243), (0,0,0)] if module == "similar-sounds" else [(153,153,153)]
                for index, (x, y) in enumerate(positions):
                    if color is None and module == "similar-sounds" and index == 3:
                        ImageDraw.Draw(picture).rectangle((537,45,619,66), fill=(111,152,193))
                    for pixel in range(count):
                        point = (x + pixel % 8, y + pixel // 8) if color is None else (x + pixel, y)
                        picture.putpixel(point, valid_inks[index] if color is None else color)
                path = root / (module + ".png")
                picture.save(path)
                crops[module] = {"path": str(path), "sha256": digest(path)}
            return audit_captures(root, {"results": [{"theme": 0, "scenario": "results", "crops": crops}]})

    def test_bright_non_text_line_does_not_make_missing_labels_pass(self):
        with self.assertRaises(AssertionError):
            self.audit((255, 255, 255), 32)

    def test_one_high_contrast_pixel_cannot_pass_a_label(self):
        with self.assertRaises(AssertionError):
            self.audit((193, 201, 209), 1)

    def test_blank_label_fails(self):
        with self.assertRaises(AssertionError):
            self.audit((17, 23, 31), 0)

    def test_expected_label_ink_with_multiple_strokes_passes(self):
        self.assertFalse(self.audit(None, 24)["failed"])


if __name__ == "__main__":
    unittest.main()
