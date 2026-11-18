"""菜单栏前的官方品牌字标：与启动页同源，深色/浅色各一图，读不到时退回文字。"""

from pathlib import Path
import hashlib
import unittest


ROOT = Path(__file__).resolve().parents[1]
ICONS = (ROOT / "src" / "HarkenSearchIcons.cpp").read_text(encoding="utf-8")
ICONS_H = (ROOT / "src" / "HarkenSearchIcons.h").read_text(encoding="utf-8")
MAIN = (ROOT / "src" / "main.cpp").read_text(encoding="utf-8")
CMAKE = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
WHITE = ROOT / "assets" / "logos" / "brand" / "harken-search-menu-wordmark-white.png"
BLACK = ROOT / "assets" / "logos" / "brand" / "harken-search-menu-wordmark-black.png"
# 2026-10-16 品牌包 HARKEN Search white/black 原字节，与官网同源。
OFFICIAL_WHITE_SHA256 = "f64332d4e34ed6c7bc1a315c8f4706a86c5556e2887aa7831b1715072e41fb80"
OFFICIAL_BLACK_SHA256 = "994d01ec83c5c5540ca3a28b0d882e4446ac3118eaa9d446d4b3dc241155874a"


class MenuBarWordmarkContract(unittest.TestCase):
    def test_official_artwork_is_stored_unchanged(self) -> None:
        self.assertEqual(OFFICIAL_WHITE_SHA256, hashlib.sha256(WHITE.read_bytes()).hexdigest())
        self.assertEqual(OFFICIAL_BLACK_SHA256, hashlib.sha256(BLACK.read_bytes()).hexdigest())

    def test_artwork_is_packaged_beside_the_exe(self) -> None:
        self.assertIn("assets/logos/brand", CMAKE)
        self.assertIn("copy_directory", CMAKE)

    def test_loader_reads_both_variants_from_the_logo_folder(self) -> None:
        self.assertIn("harken-search-menu-wordmark-white.png", ICONS)
        self.assertIn("harken-search-menu-wordmark-black.png", ICONS)
        self.assertIn("assets\\\\logos\\\\brand", ICONS)  # EXE 同级安装布局
        self.assertIn("assets/logos/brand", ICONS)        # 仓库相对布局
        self.assertIn("menuBarFillBG", ICONS)
        self.assertIn("float DrawMenuBarBrandWordmark(float x0, float y0, float x1, float y1,", ICONS_H)

    def test_main_menu_bar_uses_the_wordmark_before_the_menus(self) -> None:
        bar = MAIN.split("if (ImGui::BeginMainMenuBar()) {", 1)[1]
        bar = bar.split("ImGui::BeginMenu(L(Menu_File))", 1)[0]
        self.assertIn("aura::ui::DrawMenuBarBrandWordmark(", bar)
        self.assertIn("L(Main_AppNameBrand)", bar)
        self.assertIn("menuBarRect", bar)
        # 字标右边界是屏幕坐标，游标要换成窗口内坐标，否则多视口下菜单会双倍偏移。
        self.assertIn("(brandRightX - menuWindow->Pos.x)", bar)

    def test_fallback_keeps_visible_text_brand(self) -> None:
        helper = ICONS.split("float DrawMenuBarBrandWordmark(", 1)[1]
        self.assertIn("fallbackText", helper)
        self.assertIn("AddText", helper)


if __name__ == "__main__":
    unittest.main()
