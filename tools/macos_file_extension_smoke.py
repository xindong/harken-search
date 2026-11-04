#!/usr/bin/env python3
"""Verify the real Mac settings checkbox against actual filename draw samples."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import wave

from macos_library_smoke import App, save


def fixtures(root: Path):
    root.mkdir(parents=True, exist_ok=False)
    names = ('000-audit.multi.part.wav', '000-audit.UPPER.WAV', '000-audit.中文.wav')
    for index, name in enumerate(names):
        with wave.open(str(root / name), 'wb') as stream:
            stream.setparams((1, 2, 48000, 0, 'NONE', 'not compressed'))
            stream.writeframes(struct.pack('<h', 1300 + index) * 24000)
    converted = root / '000-audit.aiff'
    subprocess.run(['/usr/bin/afconvert', '-f', 'AIFF', '-d', 'BEI16',
                    str(root / names[0]), str(converted)], check=True, capture_output=True)
    return sorted((*names, converted.name))


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(binary: Path, root: Path, output: Path):
    output.mkdir(parents=True, exist_ok=False)
    app = App(binary, root, output)
    report = {'status': 'FAIL', 'scope': 'real Mac extension checkbox and result-list drawing', 'steps': []}
    settings = root / 'profile/Application Support/HarkenSearch/settings.ini'
    source = root / 'fixtures' / output.name

    def snapshot():
        return app.command()

    def item(label):
        state = snapshot()['settings_ui']
        matches = [entry for entry in state['items'] if entry['id'] == label]
        assert len(matches) == 1, (label, [entry['id'] for entry in state['items']])
        entry = matches[0]
        left, top, right, bottom = entry['bounds']
        clip_left, clip_top, clip_right, clip_bottom = entry['clip']
        left, top = max(left, clip_left), max(top, clip_top)
        right, bottom = min(right, clip_right), min(bottom, clip_bottom)
        assert right - left > 3 and bottom - top > 3, (label, entry)
        return (left + right) / 2, (top + bottom) / 2

    def click(label):
        x, y = item(label)
        for phase in ('hover', 'down', 'up'):
            before = snapshot()['settings_ui']['frame']
            app.command('pointer', x=x, y=y, phase=phase)
            app.wait(lambda s: s['settings_ui']['frame'] > before, 'pointer frame', 8)

    def open_settings():
        app.command('show_settings')
        app.wait(lambda s: s['settings_ui']['open'], 'settings open', 15)
        app.command('settings_focus_extension')
        return app.wait(lambda s: any(i['id'] == 'extension' for i in s['settings_ui']['items']),
                        'extension checkbox visible', 15)

    def saved():
        return next(line.split('=', 1)[1] for line in settings.read_text('utf-8-sig').splitlines()
                    if line.startswith('showFileExtension='))

    def expected(raw, shown):
        return raw if shown else raw.rsplit('.', 1)[0]

    def drawn(shown):
        state = app.wait(lambda s: s['list_ui']['draw_frame'] >= s['list_ui']['frame'] - 3
                         and set(names) <= set(s['list_ui']['raw_names']),
                         'actual list draw samples', 20)
        observed = dict(zip(state['list_ui']['raw_names'], state['list_ui']['rendered_names']))
        assert all(observed[name] == expected(name, shown) for name in names), observed
        return state, observed

    def record(label, shown):
        state, names_on_screen = drawn(shown)
        frame = app.command(capture=True)
        report['steps'].append({'label': label, 'live': state['settings_ui']['live_extension'],
                                'draft': state['settings_ui']['draft_extension'],
                                'pending': state['settings_ui']['pending'], 'saved': saved(),
                                'drawn': names_on_screen,
                                'result_ids': [row['id'] for row in state['rows']],
                                'screenshot': f'{frame["id"]:04d}.png'})
        return state

    try:
        app.wait(lambda s: not s['switch_pending'], 'startup mount', 60)
        app.command('create', name='Extension-' + output.name)
        created = app.wait(lambda s: s['database_open'] and not s['switch_pending']
                           and 'Extension-' + output.name in s['database'], 'create library', 40)
        report['library'] = created['database']
        names = fixtures(source)
        before = {name: sha(source / name) for name in names}
        app.command('scan', path=str(source))
        app.wait(lambda s: not s['scan']['active'] and not s['scan']['pending']
                 and not s['search_pending'] and s['count'] == len(names),
                 'scan all generated audio', 90)
        opened = open_settings()
        report['starting_extension'] = opened['settings_ui']['live_extension']
        if not opened['settings_ui']['live_extension']:
            click('extension')
            click('apply')
            app.wait(lambda s: s['settings_ui']['live_extension'] and
                     s['settings_ui']['pending'] == 0, 'show extension baseline', 15)
        shown = record('shown-before', True)
        original_ids = [row['id'] for row in shown['rows']]
        click('extension')
        draft = app.wait(lambda s: not s['settings_ui']['draft_extension'] and
                         s['settings_ui']['live_extension'] and s['settings_ui']['pending'] > 0,
                         'checkbox changes draft only', 10)
        assert saved() == '1'
        record('hide-pending', True)
        click('revert')
        app.wait(lambda s: s['settings_ui']['draft_extension'] and
                 s['settings_ui']['pending'] == 0, 'revert draft', 10)
        record('hide-reverted', True)

        click('extension')
        app.command('settings_fail_save_once')
        click('apply')
        failed = app.wait(lambda s: s['settings_ui']['pending'] > 0 and
                          bool(s['settings_ui']['save_error']) and
                          s['settings_ui']['live_extension'], 'failed save rolls back', 15)
        assert saved() == '1' and not failed['settings_ui']['draft_extension']
        record('failed-save-keeps-shown', True)
        click('apply')
        app.wait(lambda s: not s['settings_ui']['live_extension'] and
                 s['settings_ui']['pending'] == 0, 'hide extension applied', 15)
        assert saved() == '0'
        hidden = record('hidden-applied', False)
        assert [row['id'] for row in hidden['rows']] == original_ids
        click('done')
        app.wait(lambda s: not s['settings_ui']['open'], 'settings close', 10)
        record('hidden-main-list', False)

        open_settings()
        click('extension')
        click('close')
        prompt = app.wait(lambda s: s['settings_ui']['prompt'], 'discard prompt', 10)
        assert prompt['settings_ui']['live_extension'] is False
        click('discard')
        app.wait(lambda s: not s['settings_ui']['open'], 'discarded settings', 10)
        record('discard-keeps-hidden', False)
        after = {name: sha(source / name) for name in names}
        assert before == after, (before, after)
        report['file_hashes_unchanged'] = before
        report['exit_code'] = app.close()
        app.retain()

        reopened = output / 'reopen'
        reopened.mkdir()
        app = App(binary, root, reopened)
        restored = app.wait(lambda s: s['database'] == report['library'] and
                            not s['switch_pending'] and not s['search_pending'] and
                            s['count'] == len(names) and not s['settings_ui']['live_extension'],
                            'reopen saved setting and library', 60)
        drawn(False)
        app.command(capture=True)
        report['reopen'] = {'library': restored['database'], 'count': restored['count'],
                            'saved_extension': saved()}
        report['reopen_exit_code'] = app.close()
        report['status'] = 'PASS'
    except Exception as error:
        report['error'] = str(error)
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
            parser.error('absolute non-symlink path required')
    if 'qa' not in args.root.parts or 'qa' not in args.out.parts:
        parser.error('dedicated qa roots required')
    raise SystemExit(0 if run(args.app, args.root, args.out) else 1)
