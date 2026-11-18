"""Generate or verify Harken Search's shared application icon and brand PNGs.

`HarkenSearch.ico` is the one icon used by the Windows executable, tray, taskbar
and installer: the Search bot artwork as a rounded avatar tile. The small
toolbar PNGs keep coming from the preserved 2026-10-25 app icon package.
"""
from __future__ import annotations

import argparse
import hashlib
import io
import json
from pathlib import Path

from PIL import Image, ImageChops, ImageDraw

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "assets/icons/source/harken-app-icons"
BOT_SOURCE = ROOT / "assets/icons/source/harken-bot-icons"
SIZES = (16, 20, 24, 30, 32, 36, 40, 48, 60, 64, 72, 80, 96, 128, 192, 256)
BOT_SIZES = (64, 128, 256, 512, 1024)
CORNER_RADIUS = 0.22
SUPERSAMPLE = 8


def png(size: int) -> Image.Image:
    source_size = next(n for n in (16, 20, 24, 30, 32, 36, 40, 48, 60, 64,
                                  72, 80, 96, 128, 192, 256, 512, 1024)
                       if n >= size)
    with Image.open(SOURCE / "Search" / f"Search transparent {source_size}.png") as image:
        result = image.convert("RGBA")
    return result if size == source_size else result.resize((size, size), Image.Resampling.LANCZOS)


def bot_png(size: int) -> Image.Image:
    source_size = next(n for n in BOT_SIZES if n >= size)
    with Image.open(BOT_SOURCE / "Search" / f"search bot {source_size}.png") as image:
        result = image.convert("RGBA")
    return result if size == source_size else result.resize((size, size), Image.Resampling.LANCZOS)


def bot_tile(size: int) -> Image.Image:
    """Rounded avatar tile: resize first, mask with an anti-aliased rounded rect."""
    large = size * SUPERSAMPLE
    artwork = bot_png(size).resize((large, large), Image.Resampling.LANCZOS)
    mask = Image.new("L", (large, large), 0)
    ImageDraw.Draw(mask).rounded_rectangle(
        (0, 0, large - 1, large - 1), radius=round(large * CORNER_RADIUS), fill=255)
    artwork.putalpha(ImageChops.multiply(artwork.getchannel("A"), mask))
    return artwork.resize((size, size), Image.Resampling.LANCZOS)


def app_icon() -> bytes:
    buffer = io.BytesIO()
    bot_tile(256).save(buffer, format="ICO", sizes=[(n, n) for n in SIZES],
                       append_images=[bot_tile(n) for n in SIZES if n != 256])
    return buffer.getvalue()


def outputs(website: Path | None) -> dict[Path, bytes]:
    result = {ROOT / "HarkenSearch.ico": app_icon()}
    for name, size in (("auro.png", 20), ("auro@2x.png", 40),
                       ("harken.png", 20), ("harken@2x.png", 40)):
        buffer = io.BytesIO()
        png(size).save(buffer, format="PNG")
        result[ROOT / "assets/icons/brand" / name] = buffer.getvalue()
    if website is not None:
        result[website / "public/favicon.ico"] = app_icon()
        buffer = io.BytesIO()
        bot_tile(180).save(buffer, format="PNG")
        result[website / "public/apple-touch-icon.png"] = buffer.getvalue()
    return result


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true", help="Check without changing any files")
    parser.add_argument("--website-root", type=Path, help="HarkenSearch/frontend directory")
    args = parser.parse_args()
    for source, manifest_dir in ((SOURCE, "app icon"), (BOT_SOURCE, "bot icon")):
        manifest = json.loads((source / "manifest.json").read_text(encoding="utf-8"))
        for item in manifest["files"]:
            path = source / item["path"]
            if hashlib.sha256(path.read_bytes()).hexdigest() != item["sha256"]:
                raise SystemExit(f"Source checksum mismatch ({manifest_dir}): {path}")
            with Image.open(path) as image:
                image.verify()
    generated = outputs(args.website_root)
    for path, expected in generated.items():
        if args.check:
            if not path.is_file() or path.read_bytes() != expected:
                raise SystemExit(f"Generated icon mismatch: {path}")
        else:
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(expected)
    with Image.open(ROOT / "HarkenSearch.ico") as icon:
        if icon.ico.sizes() != {(n, n) for n in SIZES}:
            raise SystemExit("Missing ICO resolutions")
        for size in SIZES:
            if icon.ico.getimage((size, size)).convert("RGBA").tobytes() != bot_tile(size).tobytes():
                raise SystemExit(f"ICO pixels differ from source: {size}")
    print(f"PASS: shared ICO from Search bot, {len(SIZES)} resolutions, rounded corners "
          f"{int(CORNER_RADIUS * 100)}%; brand PNGs from the 2026-10-25 package")


if __name__ == "__main__":
    main()
