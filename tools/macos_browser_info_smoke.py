#!/usr/bin/env python3
"""Exercise Browser, Info and waveform in the real isolated Mac app.

Requires a QA build with ARIS_PREVIEW_ISOLATION and AURA_BUILD_DIAGNOSTICS,
plus Pillow for the framebuffer check. No system mouse events are sent.
"""

import argparse
import hashlib
import json
from pathlib import Path
import struct
import time
import wave

from PIL import Image

from macos_library_smoke import App


def check_isolation(binary, root):
    binary = binary.resolve(strict=True)
    root = root.resolve()
    cache = binary.parents[3] / 'CMakeCache.txt'
    values = {}
    for line in cache.read_text(encoding='utf-8').splitlines():
        if ':' in line and '=' in line and not line.startswith(('#', '//')):
            key, value = line.split('=', 1)
            values[key.split(':', 1)[0]] = value
    assert values.get('ARIS_PREVIEW_ISOLATION') == 'ON', 'isolation build required'
    assert values.get('AURA_BUILD_DIAGNOSTICS') == 'ON', 'diagnostics build required'
    assert values.get('AURA_INTERNAL_BETA') == 'OFF', 'internal beta build is not QA control'
    assert Path(values['ARIS_PREVIEW_ISOLATION_ROOT']).resolve() == root, 'QA root does not match binary'
    assert any(parent.name == 'qa' for parent in root.parents), 'QA root must be under qa/'


def make_fixture(path):
    path.mkdir(parents=True, exist_ok=False)
    for name in ('ArialTest中文', 'EnglishABC', '中文ABC', '游戏音效', '真人快打SFX+VO'):
        (path / name).mkdir()
    for name in ('EnglishABC/EffectOne.wav', '中文ABC/鼓声.wav'):
        with wave.open(str(path / name), 'wb') as out:
            out.setparams((1, 2, 48000, 0, 'NONE', 'not compressed'))
            out.writeframes(struct.pack('<h', 3000) * 48000)


def info_pixels(path, display):
    image = Image.open(path).convert('RGB')
    x_scale = image.width / display['width']
    y_scale = image.height / display['height']
    box = tuple(round(v * s) for v, s in zip((1000, 105, 1130, 210),
                                            (x_scale, y_scale, x_scale, y_scale)))
    crop = image.crop(box)
    pixels = crop.load()
    return sum(max(pixels[x, y]) > 130 for y in range(crop.height)
               for x in range(crop.width)) / (x_scale * y_scale)


def capture(app, output, report, label):
    state = app.command(capture=True)
    image = output / f'{app.sequence:04d}.png'
    sample = {'image': str(image), 'view_mode': state['view_mode'],
              'info_page_selected': state['info_page_selected'],
              'info_bright_pixels_at_1x': round(info_pixels(image, state['window'])),
              'audio_loaded': state['audio']['loaded'],
              'audio_path': state['audio']['path'],
              'wave_ready': state['audio']['wave_ready']}
    report['captures'][label] = sample
    return state


def click(app, x, y):
    for phase in ('hover', 'down', 'up'):
        frame = app.command()['wave_ui']['frame']
        app.command('pointer', x=x, y=y, phase=phase)
        app.wait(lambda state: state['wave_ui']['frame'] > frame, 'pointer frame', 8)


def run(binary, root, output):
    check_isolation(binary, root)
    root.mkdir(parents=True, exist_ok=True)
    output.mkdir(parents=True, exist_ok=False)
    source = root / 'fixtures' / ('browser_info_' + output.name)
    make_fixture(source)
    report = {'status': 'FAIL', 'binary': str(binary.resolve()),
              'binary_sha256': hashlib.sha256(binary.read_bytes()).hexdigest(),
              'fixture': str(source), 'captures': {}}
    app = App(binary, root, output)
    try:
        app.wait(lambda state: not state['switch_pending'], 'startup', 60)
        name = 'BrowserInfo-' + output.name
        app.command('create', name=name)
        app.wait(lambda state: state['database_open'] and name in state['database']
                 and not state['switch_pending'], 'new isolated library')
        app.command('scan', path=str(source))
        app.wait(lambda state: not state['scan']['active'] and
                 not state['scan']['pending'] and state['scan']['discovered'] > 0,
                 'scan completion', 90)
        app.wait(lambda state: not state['search_pending'] and state['count'] == 2,
                 'two scanned files')
        capture(app, output, report, 'scanned_database')
        report['first_exit_code'] = app.close()
        app.retain()

        reopened = output / 'reopened'
        reopened.mkdir()
        app = App(binary, root, reopened)
        app.wait(lambda state: state['database_open'] and name in state['database']
                 and not state['switch_pending'], 'reopened library', 60)
        app.command('browser_folder', path=str(source))
        app.wait(lambda state: state['view_mode'] == 0, 'Browser view')
        if app.command()['info_page_selected'] != 1:
            click(app, 1180, 76)
            app.wait(lambda state: state['info_page_selected'] == 1,
                     'Info Artwork page', 8)
        capture(app, reopened, report, 'browser_immediate')
        time.sleep(3)
        settled = capture(app, reopened, report, 'browser_settled')
        assert settled['info_page_selected'] == 1, 'expected Artwork Info page'
        assert report['captures']['browser_settled']['info_bright_pixels_at_1x'] < 350, \
            'unexpected content in the empty Artwork Info panel'

        app.command('browser_folder', path=str(source / 'EnglishABC'))
        click(app, 350, 230)
        app.wait(lambda state: state['audio']['loaded'] and
                 state['audio']['path'] == str(source / 'EnglishABC/EffectOne.wav'),
                 'Browser file audition', 30)
        app.wait(lambda state: state['audio']['wave_ready'] and
                 state['audio']['wave_peaks'] > 0, 'Browser waveform', 30)
        capture(app, reopened, report, 'browser_audio_artwork')
        click(app, 1148, 76)
        app.wait(lambda state: state['info_page_selected'] == 0,
                 'Info metadata page', 8)
        capture(app, reopened, report, 'browser_audio_metadata')
        report['second_exit_code'] = app.close()
        report['status'] = 'PASS'
    except Exception as error:
        report['error'] = str(error)
        try:
            report['exit_code'] = app.close()
        except Exception as close_error:
            report['close_error'] = str(close_error)
        raise
    finally:
        app.retain()
        (output / 'report.json').write_text(json.dumps(report, ensure_ascii=False, indent=2),
                                            encoding='utf-8')
        print(json.dumps(report, ensure_ascii=False, indent=2))


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('binary', type=Path)
    parser.add_argument('root', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    run(args.binary, args.root, args.output)
