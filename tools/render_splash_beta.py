"""Regenerate the Windows beta splash from the recorded source artwork.

The splash is the fallback startup facade shown when WebView2 cannot present
the loading page (``kUseBetaSplashLoading``) — it keeps the original dark-blue
composition but draws the current brand: the orange Search bot tile and the
official orange wordmark.

Source artwork: ``assets/splash/splash_beta-source.png`` (the 2026-10-14 splash,
preserved unchanged). The old lettermark and white wordmark are removed by
fitting a smooth background model on the untouched pixels, then the brand
assets are composited back in. No recolouring of the brand artwork.

Run from the repository root. Both dependencies are listed in
``tools/requirements-manual.txt`` (Pillow for the image work, numpy for the
background model):

    python tools/render_splash_beta.py
    python tools/render_splash_beta.py --check
"""
from __future__ import annotations

import argparse
import importlib.util
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFilter

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "assets" / "splash" / "splash_beta-source.png"
OUTPUT = ROOT / "resources" / "splash_beta.png"
WORDMARK = ROOT / "assets" / "logos" / "brand" / "harken-search-menu-wordmark-orange.png"
ICON_GENERATOR = ROOT / "tools" / "generate_harken_app_icons.py"

# Pixel boxes measured on the source artwork.
TILE = (147, 197, 337, 390)
WORDMARK_BOX = (395, 236, 770, 292)
WORDMARK_WIDTH = 470
BOT_ARTWORK_SIZE = 384
CORNER_RADIUS = 0.22


def bot_tile(size: int) -> Image.Image:
    """Reuse the shared app-icon generator so the splash icon cannot drift."""
    spec = importlib.util.spec_from_file_location("harken_app_icons", ICON_GENERATOR)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module.bot_tile(size)


def render() -> Image.Image:
    base = Image.open(SOURCE).convert("RGBA")
    width, height = base.size
    pixels = np.asarray(base.convert("RGB")).astype(np.float64)

    keep = np.zeros((height, width), bool)
    keep[TILE[1] - 10:TILE[3] + 10, TILE[0] - 10:TILE[2] + 10] = True
    # The old wordmark carried a soft bloom, so the masked area is larger than
    # the glyphs themselves.
    keep[WORDMARK_BOX[1] - 24:WORDMARK_BOX[3] + 48, WORDMARK_BOX[0] - 20:WORDMARK_BOX[2] + 26] = True

    ys, xs = np.mgrid[0:height, 0:width]
    nx, ny = xs / width, ys / height
    terms = [np.ones_like(nx), nx, ny, nx * nx, ny * ny, nx * ny]
    sample = (~keep)[::3, ::3]
    design = np.stack([term[::3, ::3][sample] for term in terms], axis=1)
    fit = np.zeros_like(pixels)
    for channel in range(3):
        values = pixels[..., channel][::3, ::3][sample]
        coefficients, *_ = np.linalg.lstsq(design, values, rcond=None)
        fit[..., channel] = np.clip(sum(coefficients[i] * terms[i] for i in range(6)), 0, 255)

    residual = pixels - fit
    residual[keep] = 0
    blurred = Image.fromarray(np.clip(residual + 128, 0, 255).astype(np.uint8)).filter(
        ImageFilter.GaussianBlur(45))
    residual = np.asarray(blurred).astype(np.float64) - 128
    background = Image.fromarray(np.clip(fit + residual, 0, 255).astype(np.uint8)).convert("RGBA")

    soft = Image.fromarray((keep * 255).astype(np.uint8)).filter(ImageFilter.GaussianBlur(12))
    canvas = Image.composite(background, base, soft)

    tile_width = TILE[2] - TILE[0]
    tile_height = TILE[3] - TILE[1]
    shadow = Image.new("RGBA", base.size, (0, 0, 0, 0))
    ImageDraw.Draw(shadow).rounded_rectangle(
        (TILE[0] - 4, TILE[1] - 2, TILE[2] + 4, TILE[3] + 14),
        radius=int(tile_width * CORNER_RADIUS), fill=(0, 0, 0, 90))
    canvas.alpha_composite(shadow.filter(ImageFilter.GaussianBlur(14)))
    canvas.alpha_composite(bot_tile(BOT_ARTWORK_SIZE).resize((tile_width, tile_height), Image.LANCZOS),
                           (TILE[0], TILE[1]))

    wordmark = Image.open(WORDMARK).convert("RGBA")
    wordmark_height = round(WORDMARK_WIDTH * wordmark.size[1] / wordmark.size[0])
    wordmark = wordmark.resize((WORDMARK_WIDTH, wordmark_height), Image.LANCZOS)
    top = (WORDMARK_BOX[1] + WORDMARK_BOX[3]) // 2 - wordmark_height // 2 + 8
    canvas.alpha_composite(wordmark, (WORDMARK_BOX[0], top))
    return canvas


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true", help="Verify without writing")
    args = parser.parse_args()
    expected = render()
    if args.check:
        if not OUTPUT.is_file():
            raise SystemExit(f"missing {OUTPUT}")
        with Image.open(OUTPUT) as current:
            if current.convert("RGBA").tobytes() != expected.convert("RGBA").tobytes():
                raise SystemExit(f"splash differs from its source: {OUTPUT}")
        print(f"PASS: {OUTPUT.relative_to(ROOT)} matches the recorded source artwork")
        return
    OUTPUT.parent.mkdir(parents=True, exist_ok=True)
    expected.save(OUTPUT)
    print(f"Wrote {OUTPUT.relative_to(ROOT)}")


if __name__ == "__main__":
    main()
