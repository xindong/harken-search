"""Build the macOS app icon from the preserved official Search PNG sizes."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]
SOURCE_ROOT = ROOT / "assets/icons/source/harken-app-icons"
SEARCH = SOURCE_ROOT / "Search"
OUTPUT = ROOT / "resources/HarkenSearch.icns"
ICONSET = {
    "icon_16x16.png": 16,
    "icon_16x16@2x.png": 32,
    "icon_32x32.png": 32,
    "icon_32x32@2x.png": 64,
    "icon_128x128.png": 128,
    "icon_128x128@2x.png": 256,
    "icon_256x256.png": 256,
    "icon_256x256@2x.png": 512,
    "icon_512x512.png": 512,
    "icon_512x512@2x.png": 1024,
}


def checked_source(size: int, manifest: dict[str, str]) -> Path:
    relative = f"Search/Search transparent {size}.png"
    source = SOURCE_ROOT / relative
    data = source.read_bytes()
    if hashlib.sha256(data).hexdigest() != manifest[relative]:
        raise ValueError(f"Source checksum mismatch: {source}")
    if data[:16] != b"\x89PNG\r\n\x1a\n\x00\x00\x00\rIHDR":
        raise ValueError(f"Invalid PNG header: {source}")
    width, height = struct.unpack(">II", data[16:24])
    if (width, height) != (size, size):
        raise ValueError(f"Unexpected PNG size: {source}: {width}x{height}")
    return source


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true", help="Verify the committed icon without writing")
    args = parser.parse_args()
    manifest_data = json.loads((SOURCE_ROOT / "manifest.json").read_text(encoding="utf-8"))
    manifest = {item["path"]: item["sha256"] for item in manifest_data["files"]}
    with tempfile.TemporaryDirectory(prefix="harken-mac-icon-") as temp:
        folder = Path(temp) / "HarkenSearch.iconset"
        folder.mkdir()
        for name, size in ICONSET.items():
            shutil.copyfile(checked_source(size, manifest), folder / name)
        generated = Path(temp) / "HarkenSearch.icns"
        subprocess.run(["iconutil", "--convert", "icns", "--output", str(generated), str(folder)],
                       check=True, capture_output=True, text=True)
        if args.check:
            if not OUTPUT.is_file() or OUTPUT.read_bytes() != generated.read_bytes():
                raise SystemExit(f"Generated macOS icon differs: {OUTPUT}")
        else:
            OUTPUT.parent.mkdir(parents=True, exist_ok=True)
            OUTPUT.write_bytes(generated.read_bytes())
    print(f"PASS: macOS icon from {len(ICONSET)} official Search PNG sizes: {OUTPUT}")


if __name__ == "__main__":
    main()
