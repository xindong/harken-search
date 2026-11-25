"""RQ-20261020-10 + REQ-20261123-BRAND-ORANGE-WORDMARK: both startup pages use the official packaged orange wordmark."""

from pathlib import Path
import hashlib
import unittest


ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "src" / "WindowsMain.cpp").read_text(encoding="utf-8")
CMAKE = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
ASSET = ROOT / "assets" / "logos" / "brand" / "harken-search-menu-wordmark-orange.png"
OFFICIAL_ORANGE_MENU_SHA256 = "b42ba8755cc8a5bf78ccb50a8769d03410cab1ce147ed56c68ca89f74f210d71"


class StartupOfficialWordmarkContract(unittest.TestCase):
    def test_official_asset_is_packaged_and_loaded_locally(self) -> None:
        self.assertTrue(ASSET.is_file())
        # 橙色菜单字标是官方橙色横版字标按 alpha 边界裁切的结果，不重新上色或缩放。
        self.assertEqual(OFFICIAL_ORANGE_MENU_SHA256, hashlib.sha256(ASSET.read_bytes()).hexdigest())
        self.assertIn('copy_directory', CMAKE)
        self.assertIn('assets/logos/brand', CMAKE)
        self.assertIn('harken-search-menu-wordmark-orange.png', SOURCE, 'startup must read the orange logo')
        self.assertIn('BuildStartupBrandLogoDataUri', SOURCE, 'startup logo reader is absent')

    def test_startup_never_swaps_to_black_or_white_artwork(self) -> None:
        # 品牌标识统一为橙色版：浅色壳不再用 brightness(0) 把字标压成黑色。
        self.assertNotIn('harken-search-menu-wordmark-white.png', SOURCE)
        self.assertNotIn('harken-search-menu-wordmark-black.png', SOURCE)
        self.assertNotIn('brightness(0)!important', SOURCE)

    def test_both_pages_use_image_with_text_fallback(self) -> None:
        loading = SOURCE.split('static std::wstring BuildFlashUiLoadingHtml(', 1)[1].split('// Beta Splash Screen', 1)[0]
        welcome = SOURCE.split('static std::wstring BuildFlashUiWelcomeHtml(', 2)[-1]
        self.assertTrue('.brand-word{background-image:url(' in SOURCE, 'official logo CSS is absent')
        for page in (loading, welcome):
            self.assertTrue('AppendStartupBrandLogoCss(html);' in page, 'page does not use official logo')
            self.assertTrue('<div class="brand-word">Harken Search</div>' in page, 'text fallback is absent')


if __name__ == "__main__":
    unittest.main()
