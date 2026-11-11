#!/usr/bin/env python3
"""Click all three display modes in the isolated real Mac settings dialog."""
import argparse
import json
import math
from pathlib import Path
import random
import struct
import time
import wave

from macos_library_smoke import App, fixtures, save


def chirp_fixture(path: Path):
    """Known changing frequencies make a visible spectrum easier to verify."""
    sample_rate, frames = 48000, 48000 * 3
    random_source = random.Random(31)
    phase = 0.0
    samples = bytearray()
    for index in range(frames):
        frequency = 180.0 + 8000.0 * index / frames
        phase += 2.0 * math.pi * frequency / sample_rate
        value = 0.55 * math.sin(phase) + 0.18 * math.sin(2.0 * phase)
        value += 0.07 * (2.0 * random_source.random() - 1.0)
        samples.extend(struct.pack('<h', round(28000 * max(-1.0, min(1.0, value)))))
    with wave.open(str(path), 'wb') as stream:
        stream.setparams((1, 2, sample_rate, 0, 'NONE', 'not compressed'))
        stream.writeframes(samples)


def run(binary: Path, root: Path, output: Path) -> bool:
    output.mkdir(parents=True, exist_ok=False)
    app = App(binary, root, output)
    report = {'status': 'FAIL', 'scope': 'real Mac settings clicks and waveform drawing', 'modes': []}
    settings = root / 'profile/Application Support/HarkenSearch/settings.ini'

    def snapshot():
        return app.command()

    def settings_state():
        return snapshot()['settings_ui']

    def item(label):
        ui = settings_state()
        matches = [entry for entry in ui['items'] if entry['id'] == label]
        assert len(matches) == 1, (label, ui['page'], [entry['id'] for entry in ui['items']])
        entry = matches[0]
        left, top, right, bottom = entry['bounds']
        clip_left, clip_top, clip_right, clip_bottom = entry['clip']
        left, top = max(left, clip_left), max(top, clip_top)
        right, bottom = min(right, clip_right), min(bottom, clip_bottom)
        assert right - left >= 5 and bottom - top >= 5, (label, entry)
        return (left + right) / 2, (top + bottom) / 2

    def click(label):
        x, y = item(label)
        for phase in ('hover', 'down', 'up'):
            before = settings_state()['frame']
            app.command('pointer', x=x, y=y, phase=phase)
            app.wait(lambda s: s['settings_ui']['frame'] > before, 'pointer frame', 8)

    def open_wave_settings():
        app.command('show_settings')
        app.wait(lambda s: s['settings_ui']['open'], 'settings opened', 15)
        app.command('settings_focus_wave')
        app.wait(lambda s: s['settings_ui']['open'] and s['settings_ui']['page'] == 1
                 and any(entry['id'] == 'waveMode' for entry in s['settings_ui']['items']),
                 'wave mode field visible', 15)

    def saved_value(name):
        text = settings.read_text(encoding='utf-8-sig')
        return next(line.split('=', 1)[1] for line in text.splitlines()
                    if line.startswith(name + '='))

    def edit_to(mode):
        open_wave_settings()
        click('waveMode')
        app.wait(lambda s: any(entry['id'] == 'option-' + str(mode)
                 for entry in s['settings_ui']['items']), 'mode options opened', 10)
        click('option-' + str(mode))
        draft = app.wait(lambda s: s['settings_ui']['draft_wave_mode'] == mode
                         and s['settings_ui']['pending'] > 0, 'mode draft updated', 10)
        assert draft['settings_ui']['draft_spectrum'] == (mode == 2), draft['settings_ui']
        return draft

    def choose(mode):
        draft = edit_to(mode)
        app.command(capture=True)
        click('apply')
        applied = app.wait(lambda s: s['settings_ui']['live_wave_mode'] == mode
                           and s['settings_ui']['pending'] == 0, 'mode applied', 15)
        assert applied['settings_ui']['live_spectrum'] == (mode == 2), applied['settings_ui']
        assert saved_value('waveformDisplayMode') == str(mode)
        if mode < 2:
            assert saved_value('waveformStyle') == str(3 if mode == 0 else 2)
        click('done')
        app.wait(lambda s: not s['settings_ui']['open'], 'settings closed', 10)
        if mode == 2:
            drawn = app.wait(lambda s: s['wave_ui']['spectrum_texture'] > 0
                             and s['wave_ui']['spectrum_file'] == s['audio']['path']
                             and s['wave_ui']['spectrum_width'] > 0
                             and s['wave_ui']['spectrum_height'] > 0
                             and s['wave_ui']['spectrum_draw_frame'] >=
                             s['wave_ui']['frame'] - 3,
                             'current file spectrum texture drawn', 60)
        else:
            drawn = app.wait(lambda s: s['wave_ui']['wave_draw_frame'] >=
                             s['wave_ui']['frame'] - 3 and
                             s['wave_ui']['wave_draw_style'] == (3 if mode == 0 else 2)
                             and s['wave_ui']['spectrum_draw_frame'] <
                             s['wave_ui']['frame'] - 3, 'real waveform style drawn', 30)
        if mode == 2:
            for attempt in range(30):
                image = app.command(capture=True)
                pixels = image['wave_ui']['spectrum_pixels']
                if (image['wave_ui']['spectrum_commands'] and pixels
                        and pixels['bright_left'] > 1000
                        and pixels['bright_right'] > pixels['bright_left'] * 0.9):
                    break
                time.sleep(0.3)
            else:
                raise AssertionError('spectrum image did not fill the full audio timeline')
        else:
            image = app.command(capture=True)
        report['modes'].append({'mode': mode, 'draft_frame': draft['settings_ui']['frame'],
                                'draw': drawn['wave_ui'],
                                'image_id': image['id'],
                                'spectrum_pixels': image['wave_ui']['spectrum_pixels'],
                                'saved_mode': saved_value('waveformDisplayMode'),
                                'saved_style': saved_value('waveformStyle')})

    try:
        app.wait(lambda s: not s['switch_pending'], 'startup library', 60)
        library = 'WaveMode-' + output.name
        app.command('create', name=library)
        app.wait(lambda s: library in s['database'] and not s['switch_pending'], 'create library')
        source = root / 'fixtures' / ('wave-mode-' + output.name)
        fixtures(source)
        chirp_fixture(source / 'A/stone-impact.wav')
        app.command('scan', path=str(source))
        state = app.wait(lambda s: not s['scan']['active'] and not s['scan']['pending']
                         and not s['search_pending'] and s['count'] == 7, 'scan', 90)
        report['library'] = state['database']
        index = next(i for i, row in enumerate(state['rows'])
                     if row['name'] == 'stone-impact.wav' and '/A/' in row['path'])
        app.command('select', index=index, play_when_ready=False)
        app.wait(lambda s: s['audio']['loaded'] and s['audio']['wave_ready']
                 and s['audio']['wave_peaks'] > 100, 'main waveform', 45)
        start_mode = app.command()['settings_ui']['live_wave_mode']
        report['starting_mode'] = start_mode
        for mode in ((2, 0, 1, 2) if start_mode != 2 else (0, 1, 2)):
            choose(mode)
        edit_to(0)
        app.command('settings_fail_save_once')
        click('apply')
        failed = app.wait(lambda s: s['settings_ui']['pending'] > 0
                          and bool(s['settings_ui']['save_error'])
                          and s['settings_ui']['live_wave_mode'] == 2,
                          'failed save rolled back live mode', 15)
        assert failed['settings_ui']['live_spectrum']
        assert saved_value('waveformDisplayMode') == '2'
        failure_image = app.command(capture=True)
        report['failed_save'] = {'live_mode': failed['settings_ui']['live_wave_mode'],
                                 'saved_mode': saved_value('waveformDisplayMode'),
                                 'pending': failed['settings_ui']['pending'],
                                 'error': failed['settings_ui']['save_error'],
                                 'image_id': failure_image['id']}
        click('revert')
        app.wait(lambda s: s['settings_ui']['pending'] == 0
                 and not s['settings_ui']['save_error'], 'revert failed draft', 10)
        click('done')
        app.wait(lambda s: not s['settings_ui']['open'], 'close after revert', 10)
        edit_to(0)
        click('close')
        app.wait(lambda s: s['settings_ui']['prompt'], 'unsaved prompt', 10)
        click('discard')
        discarded = app.wait(lambda s: not s['settings_ui']['open']
                             and s['settings_ui']['live_wave_mode'] == 2,
                             'unsaved mode discarded', 10)
        assert saved_value('waveformDisplayMode') == '2'
        report['discard'] = {'live_mode': discarded['settings_ui']['live_wave_mode'],
                             'saved_mode': saved_value('waveformDisplayMode')}
        report['exit_code'] = app.close()
        app.retain()
        reopen = output / 'reopen'
        reopen.mkdir()
        app = App(binary, root, reopen)
        restored = app.wait(lambda s: s['database'] == report['library']
                            and s['settings_ui']['live_wave_mode'] == 2
                            and not s['search_pending'] and len(s['rows']) == 7,
                            'saved mode and library rows on reopen', 45)
        report['reopen'] = {'database': restored['database'],
                            'mode': restored['settings_ui']['live_wave_mode'],
                            'saved_mode': saved_value('waveformDisplayMode')}
        reopened_index = next(i for i, row in enumerate(restored['rows'])
                              if row['name'] == 'stone-impact.wav' and '/A/' in row['path'])
        app.command('select', index=reopened_index, play_when_ready=False)
        app.wait(lambda s: s['audio']['loaded'] and s['audio']['wave_ready'], 'reopened file', 45)
        app.wait(lambda s: s['wave_ui']['spectrum_texture'] > 0
                 and s['wave_ui']['spectrum_file'] == s['audio']['path']
                 and s['wave_ui']['spectrum_draw_frame'] >= s['wave_ui']['frame'] - 3,
                 'current file spectrum after reopen', 60)
        image = app.command(capture=True)
        pixels = image['wave_ui']['spectrum_pixels']
        assert image['wave_ui']['spectrum_commands'] and pixels and pixels['bright_left'] > 1000
        assert pixels['bright_right'] > pixels['bright_left'] * 0.9, pixels
        report['reopen']['spectrum_pixels'] = pixels
        report['reopen']['image_id'] = image['id']
        report['final_exit_code'] = app.close()
        report['status'] = 'PASS'
    except Exception as error:
        detail = str(error)
        (output / 'error.txt').write_text(detail + '\n', encoding='utf-8')
        report['error'] = detail[:400]
        try:
            app.command(capture=True)
        except Exception as capture_error:
            report['capture_error'] = str(capture_error)
        try:
            report['exit_code'] = app.close()
        except Exception as close_error:
            report['shutdown_error'] = str(close_error)
            if app.process.poll() is None:
                app.process.terminate()
                app.process.wait(timeout=10)
                report['forced_exit'] = True
    finally:
        app.retain()
        save(output / 'report.json', report)
    print(json.dumps(report, ensure_ascii=False))
    return report['status'] == 'PASS'


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--app', type=Path, required=True)
    parser.add_argument('--root', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    for path in (args.app, args.root, args.out):
        if not path.is_absolute() or path.resolve() != path:
            parser.error('absolute non-symlink paths required')
    if 'qa' not in args.root.parts or 'qa' not in args.out.parts:
        parser.error('dedicated qa roots required')
    raise SystemExit(0 if run(args.app, args.root, args.out) else 1)
