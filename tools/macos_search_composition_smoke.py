#!/usr/bin/env python3
"""Verify composed search in the real isolated Mac app, without desktop input."""
import argparse
import json
from pathlib import Path
import struct
import time
import wave
import zlib

from macos_library_smoke import App, save


def make_fixtures(root):
    root.mkdir(parents=True, exist_ok=False)
    names = [
        'Cinematic/war gun.wav',
        'Cinematic/war gun explosion.wav',
        'Cinematic/war-soft.wav',
        'Archive/war rifle.wav',
        'Archive/quiet gun.wav',
        'Archive/风声.wav',
    ]
    for index, name in enumerate(names):
        path = root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        with wave.open(str(path), 'wb') as stream:
            stream.setparams((1, 2, 48000, 0, 'NONE', 'not compressed'))
            stream.writeframes(struct.pack('<h', 1800 + index * 100) * 4800)

    def chunk(kind, data):
        return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data))

    pixels = b''.join(b'\x00' + bytes([y * 4, 100, 160]) * 64 for y in range(64))
    png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', 64, 64, 8, 2, 0, 0, 0))
    png += chunk(b'IDAT', zlib.compress(pixels)) + chunk(b'IEND', b'')
    (root / 'Cinematic/folder.png').write_bytes(png)
    return names


def names_in(snapshot):
    return sorted(row['name'] for row in snapshot['rows'])


def run(binary, root, output):
    output.mkdir(parents=True, exist_ok=False)
    app = App(binary, root, output)
    report = {'status': 'FAIL', 'scope': 'real Mac app: phrase, exclusion, path, tag, folder and cover composition', 'steps': []}
    try:
        app.wait(lambda s: not s['switch_pending'], 'startup mount', 60)
        library_name = 'Search-' + output.name
        app.command('create', name=library_name)
        created = app.wait(lambda s: s['database_open'] and library_name in s['database'] and not s['switch_pending'], 'create library')
        report['library'] = created['database']
        source = root / 'fixtures' / output.name
        make_fixtures(source)
        app.command('scan', path=str(source))
        app.wait(lambda s: not s['scan']['active'] and not s['scan']['pending'] and s['scan']['discovered'] >= 6, 'scan completion', 90)
        full = app.wait(lambda s: not s['search_pending'] and s['count'] == 6 and len(s['rows']) == 6, 'six scanned rows')
        assert full['scan']['failed'] == 0, full['scan']
        app.command(capture=True)
        # The result view is refreshed after the command poll within a frame.
        # Wait until a following poll sees the settled rows before writing a QA tag.
        time.sleep(0.3)
        app.command('tag', file_name='war gun.wav', tags=['impact'])

        def expect(label, expected, *, text=None, folder=None, cover_mode=None, screenshot=False):
            fields = {}
            if text is not None:
                fields['text'] = text
            if folder is not None:
                fields['folder'] = folder
            if cover_mode is not None:
                fields['cover_mode'] = cover_mode
            app.command('filter', **fields)
            expected_names = sorted(expected)
            snapshot = app.wait(
                lambda s: not s['search_pending'] and s['count'] == len(expected_names) and names_in(s) == expected_names,
                label, 18)
            assert snapshot['count'] == len(snapshot['rows'])
            report['steps'].append({'label': label, 'query': snapshot['filters']['text'],
                                    'folder': snapshot['filters']['folder'],
                                    'cover_mode': snapshot['filters']['cover_mode'],
                                    'names': names_in(snapshot)})
            if screenshot:
                for _ in range(12):
                    frame = app.command(capture=True)
                    if not frame['search_pending'] and names_in(frame) == expected_names:
                        report['steps'][-1]['screenshot'] = f'{frame["id"]:04d}.png'
                        break
                    time.sleep(0.08)
                else:
                    raise AssertionError(f'{label}: no settled screenshot frame')
            return snapshot

        expect('plain war', ['war gun.wav', 'war gun explosion.wav', 'war-soft.wav', 'war rifle.wav'], text='war')
        expect('quoted phrase', ['war gun.wav', 'war gun explosion.wav'], text='"war gun"')
        expect('quoted phrase plus exclusion', ['war gun.wav'], text='"war gun" -explosion', screenshot=True)
        expect('path and text', ['war gun.wav', 'war gun explosion.wav', 'war-soft.wav'], text='path:cinematic war')
        expect('negative term', ['war gun.wav', 'war-soft.wav', 'war rifle.wav'], text='war -explosion')
        expect('clear text before tag', [Path(name).name for name in make_fixture_names()], text='')
        expect('tag lane', ['war gun.wav'], text='tag:impact')
        expect('tag plus text', ['war gun.wav'], text='war tag:impact')
        expect('add folder', ['war gun.wav'], folder=str(source / 'Cinematic'))
        expect('add cover', ['war gun.wav'], cover_mode=1, screenshot=True)
        expect('change text keeps folder and cover', ['war gun.wav', 'war-soft.wav'], text='war -explosion')
        expect('change text to tag keeps folder and cover', ['war gun.wav'], text='war tag:impact')
        expect('clear folder keeps tag and cover', ['war gun.wav'], folder='')
        expect('clear text keeps cover', ['war gun.wav', 'war gun explosion.wav', 'war-soft.wav'], text='')
        expect('clear cover restores all', [Path(name).name for name in make_fixture_names()], cover_mode=0, screenshot=True)
        expect('CJK character search', ['风声.wav'], text='风', screenshot=True)
        expect('pinyin search', ['风声.wav'], text='feng', screenshot=True)

        app.command('filter', text='"unfinished')
        safe = app.wait(lambda s: not s['search_pending'] and s['filters']['text'] == '"unfinished', 'malformed query safely settles', 18)
        assert safe['count'] == len(safe['rows'])
        report['malformed_query'] = {'count': safe['count'], 'names': names_in(safe)}
        report['exit_code'] = app.close()
        app.retain()

        reopen = output / 'reopen'
        reopen.mkdir()
        app = App(binary, root, reopen)
        restored = app.wait(lambda s: s['database'] == report['library'] and not s['switch_pending'] and not s['search_pending'] and s['count'] == 6, 'reopen library', 60)
        assert restored['filters'] == {'text': '', 'folder': '', 'folder_active': False, 'cover_mode': 0}, restored['filters']
        expect_reopen = restored['database']
        app.command(capture=True)
        report['reopen'] = {'database': expect_reopen, 'names': names_in(restored), 'filters': restored['filters']}
        app.command('filter', text='tag:impact')
        tagged = app.wait(lambda s: not s['search_pending'] and s['count'] == 1 and names_in(s) == ['war gun.wav'],
                          'saved tag remains searchable after reopen', 18)
        report['tag_after_reopen'] = names_in(tagged)
        app.command(capture=True)
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


def make_fixture_names():
    return ['Cinematic/war gun.wav', 'Cinematic/war gun explosion.wav', 'Cinematic/war-soft.wav',
            'Archive/war rifle.wav', 'Archive/quiet gun.wav', 'Archive/风声.wav']


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--app', type=Path, required=True)
    parser.add_argument('--root', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    for path in [args.app, args.root, args.out]:
        if not path.is_absolute() or path.resolve() != path:
            parser.error('absolute non-symlink paths required')
    if 'qa' not in args.root.parts or 'qa' not in args.out.parts:
        parser.error('dedicated qa roots required')
    raise SystemExit(0 if run(args.app, args.root, args.out) else 1)
