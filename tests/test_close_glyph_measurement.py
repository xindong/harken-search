"""The screenshot measurement must distinguish a close glyph from its surface."""
from pathlib import Path
import sys

import pytest
from PIL import Image, ImageDraw

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from qa_unified_close_x import update_close_glyph


@pytest.mark.parametrize('background', ['#0f141a', '#0f1826', '#f6edf1', '#26394d', '#fdf7ef'])
def test_blank_button_is_not_a_glyph(tmp_path, background):
    path = tmp_path / 'blank.png'
    Image.new('RGB', (48, 48), background).save(path)
    with pytest.raises(AssertionError, match='glyph'):
        update_close_glyph({'path': str(path), 'rect': [100, 200, 148, 248]},
                           [110, 210, 138, 238])


@pytest.mark.parametrize('background,ink', [('#26394d', '#d0d8e0'), ('#fdf7ef', '#35453f')])
def test_only_strokes_determine_size_and_screen_center(tmp_path, background, ink):
    path = tmp_path / 'visible.png'
    image = Image.new('RGB', (48, 48), background)
    draw = ImageDraw.Draw(image)
    draw.line((17, 17, 30, 30), fill=ink, width=2)
    draw.line((30, 17, 17, 30), fill=ink, width=2)
    image.save(path)
    result = update_close_glyph({'path': str(path), 'rect': [100, 200, 148, 248]},
                                [110, 210, 138, 238])
    assert 13 <= result['width'] <= 16
    assert 13 <= result['height'] <= 16
    assert abs(result['center_x'] - 123.5) <= 1
    assert abs(result['center_y'] - 223.5) <= 1


def test_button_outline_is_not_a_close_glyph(tmp_path):
    path = tmp_path / 'outline.png'
    image = Image.new('RGB', (48, 48), '#26394d')
    ImageDraw.Draw(image).rectangle((10, 10, 37, 37), outline='#d0d8e0', width=2)
    image.save(path)
    with pytest.raises(AssertionError, match='glyph'):
        update_close_glyph({'path': str(path), 'rect': [0, 0, 48, 48]}, [10, 10, 38, 38])


def test_tiny_glyph_cannot_inherit_the_button_dimensions(tmp_path):
    path = tmp_path / 'tiny.png'
    image = Image.new('RGB', (48, 48), '#fdf7ef')
    draw = ImageDraw.Draw(image)
    draw.line((21, 21, 26, 26), fill='#35453f', width=1)
    draw.line((26, 21, 21, 26), fill='#35453f', width=1)
    image.save(path)
    measured = update_close_glyph({'path': str(path), 'rect': [0, 0, 48, 48]},
                                 [10, 10, 38, 38])
    assert measured['width'] == measured['height'] == 6
    assert measured['width'] < 11  # The real-window acceptance must reject it.
