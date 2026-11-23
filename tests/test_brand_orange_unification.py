"""REQ-20261123-BRAND-ORANGE-WORDMARK: 文字 logo 一律橙色版，品牌图标一律橙色机器人。"""

from pathlib import Path
import unittest

from PIL import Image


ROOT = Path(__file__).resolve().parents[1]
MAIN = (ROOT / "src" / "main.cpp").read_text(encoding="utf-8")
PAGE = (ROOT / "src" / "bootstrap" / "BootstrapProgressPage.html").read_text(encoding="utf-8")
PAGE_GEN = (ROOT / "tools" / "generate_bootstrap_progress_page.py").read_text(encoding="utf-8")
BOOTSTRAP_CMAKE = (ROOT / "cmake" / "harken_bootstrap_progress_ui.cmake").read_text(encoding="utf-8")
ICON = ROOT / "HarkenSearch.ico"
ROBOT = ROOT / "assets" / "icons" / "brand" / "harken-bot@2x.png"


def about_block() -> str:
    start = MAIN.index('"##AboutAuro"')
    return MAIN[start:MAIN.index("s_aboutPage", start)]


class BrandOrangeUnification(unittest.TestCase):
    def test_about_window_uses_orange_wordmark_without_luma_switch(self) -> None:
        block = about_block()
        self.assertIn("harken-search-menu-wordmark-orange.png", block)
        self.assertNotIn("aboutLuma", block)
        self.assertNotIn("harken-search-menu-wordmark-black.png", block)

    def test_about_window_icon_is_the_bot_artwork(self) -> None:
        self.assertIn("assets/icons/brand/harken-bot@2x.png", about_block())

    def test_updater_page_has_one_orange_wordmark_and_the_bot_icon(self) -> None:
        self.assertIn("@WORDMARK_ORANGE@", PAGE)
        self.assertIn("@ROBOT_ICON@", PAGE)
        self.assertNotIn("@WORDMARK_BLACK@", PAGE)
        self.assertNotIn("@WORDMARK_WHITE@", PAGE)
        self.assertNotIn("wordmark-black", PAGE)
        self.assertNotIn("wordmark-white", PAGE)

    def test_updater_page_embeds_the_same_assets(self) -> None:
        self.assertIn("assets/logos/brand/harken-search-menu-wordmark-orange.png", PAGE_GEN)
        self.assertIn("assets/icons/brand/harken-bot@2x.png", PAGE_GEN)
        self.assertIn("assets/logos/brand/harken-search-menu-wordmark-orange.png", BOOTSTRAP_CMAKE)
        self.assertIn("assets/icons/brand/harken-bot@2x.png", BOOTSTRAP_CMAKE)

    def test_bot_brand_png_is_the_shared_app_icon_tile(self) -> None:
        # 界面品牌小图与应用图标同源：像素与 ICO 的 128 尺寸帧完全一致。
        self.assertTrue(ROBOT.is_file())
        with Image.open(ROBOT) as png:
            artwork = png.convert("RGBA")
        with Image.open(ICON) as icon:
            tile = icon.ico.getimage((128, 128)).convert("RGBA")
        self.assertEqual(tile.size, artwork.size)
        self.assertEqual(tile.tobytes(), artwork.tobytes())


if __name__ == "__main__":
    unittest.main()
