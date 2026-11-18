"""共享应用图标：圆角 Search 橙色机器人，exe / 托盘 / 任务栏 / 安装器同源。"""

from pathlib import Path
import hashlib
import json
import subprocess
import sys
import unittest

from PIL import Image


ROOT = Path(__file__).resolve().parents[1]
ICON = ROOT / "HarkenSearch.ico"
SOURCE = ROOT / "assets" / "icons" / "source" / "harken-bot-icons"
GENERATOR = ROOT / "tools" / "generate_harken_app_icons.py"
MAIN = (ROOT / "src" / "WindowsMain.cpp").read_text(encoding="utf-8")
RC = (ROOT / "resources.rc").read_text(encoding="utf-8")
INSTALLER = (ROOT / "installer" / "HarkenSearch_Setup.iss").read_text(encoding="utf-8")
SIZES = {16, 20, 24, 30, 32, 36, 40, 48, 60, 64, 72, 80, 96, 128, 192, 256}


class AppBrandIconContract(unittest.TestCase):
    def test_preserved_bot_sources_match_their_manifest(self) -> None:
        manifest = json.loads((SOURCE / "manifest.json").read_text(encoding="utf-8"))
        self.assertEqual("Search", manifest["product"])
        self.assertEqual(5, len(manifest["files"]))
        for item in manifest["files"]:
            path = SOURCE / item["path"]
            self.assertTrue(path.is_file(), path)
            self.assertEqual(item["sha256"], hashlib.sha256(path.read_bytes()).hexdigest(), path)

    def test_shared_icon_covers_windows_sizes(self) -> None:
        with Image.open(ICON) as icon:
            self.assertEqual(SIZES, {width for width, _height in icon.ico.sizes()})
        for size in SIZES:
            with Image.open(ICON) as icon:
                self.assertEqual((size, size), icon.ico.getimage((size, size)).size)

    def test_shared_icon_is_a_rounded_avatar(self) -> None:
        for size in SIZES:
            with Image.open(ICON) as icon:
                tile = icon.ico.getimage((size, size)).convert("RGBA")
            self.assertLess(tile.getpixel((0, 0))[3], 16, size)
            self.assertLess(tile.getpixel((size - 1, size - 1))[3], 16, size)
            self.assertEqual(255, tile.getpixel((size // 2, size // 2))[3], size)
            self.assertEqual(255, tile.getpixel((size // 2, 0))[3], size)

    def test_generator_reproduces_the_committed_icon(self) -> None:
        result = subprocess.run([sys.executable, str(GENERATOR), "--check"],
                                cwd=ROOT, capture_output=True, text=True, timeout=300)
        self.assertEqual(0, result.returncode, result.stderr)

    def test_every_windows_surface_reads_the_same_icon(self) -> None:
        # 资源、托盘、窗口大/小图标和安装器都只认根目录这一份 ICO。
        self.assertIn('1 ICON "HarkenSearch.ico"', RC)
        self.assertIn(r"SetupIconFile=..\HarkenSearch.ico", INSTALLER)
        self.assertIn("LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1))", MAIN)
        self.assertEqual(2, MAIN.count("MAKEINTRESOURCEW(1), IMAGE_ICON"))
        self.assertNotIn("harken-search-window.ico", MAIN)
        self.assertFalse((ROOT / "assets" / "icons" / "brand" / "harken-search-window.ico").exists())


if __name__ == "__main__":
    unittest.main()
