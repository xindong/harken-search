#!/usr/bin/env python3
"""Exercise REAPER FileList import in the isolated real Mac product."""

import argparse
import json
import math
from pathlib import Path
import struct
import wave

from macos_library_smoke import App, save


def make_wav(path, frequency):
    with wave.open(str(path), 'wb') as stream:
        stream.setparams((1, 2, 48000, 0, 'NONE', 'not compressed'))
        stream.writeframes(b''.join(
            struct.pack('<h', round(3000 * math.sin(2 * math.pi * frequency * n / 48000)))
            for n in range(12000)
        ))


def run(binary, root, output):
    output.mkdir(parents=True, exist_ok=False)
    fixtures = root / ('filelist-fixtures-' + output.name)
    fixtures.mkdir(parents=True, exist_ok=False)
    make_wav(fixtures / 'impact.wav', 220)
    make_wav(fixtures / '风声.wav', 330)
    file_list = fixtures / 'QA.ReaperFileList'
    file_list.write_text(
        f'PATH "{fixtures}"\n'
        'FILE "impact.wav" 100 0 1 0\n'
        'FILE "风声.wav" 100 0 1 0\n'
        'FILE "impact.wav" 100 0 1 0\n'
        'FILE "missing.wav" 100 0 1 0\n', encoding='utf-8'
    )
    report = {'status': 'FAIL', 'scope': 'real isolated Mac app FileList workflow and restart',
              'file_list': str(file_list), 'runs': []}
    library_name = 'FileList-QA-' + output.name
    playlist_name = 'QA REAPER Import ' + output.name
    try:
        for ordinal, name in enumerate(('first', 'reopen')):
            run_dir = output / name
            run_dir.mkdir()
            app = App(binary, root, run_dir)
            try:
                app.wait(lambda s: not s['switch_pending'], 'startup mount')
                if ordinal == 0:
                    app.command('create', name=library_name)
                    app.wait(lambda s: s['database_open'] and library_name in s['database']
                             and not s['switch_pending'], 'create isolated library')
                    app.command('import_reaper_file_list', path=str(file_list), playlist_name=playlist_name)
                state = app.wait(
                    lambda s: s['database_open'] and any(
                        p['name'] == playlist_name and p['file_count'] == 2
                        for p in s['playlists']),
                    'playlist import and reopen' if ordinal else 'playlist import', timeout=60)
                if ordinal:
                    playlist = next(p for p in state['playlists'] if p['name'] == playlist_name)
                    app.command('select_playlist', playlist_id=playlist['id'])
                state = app.wait(lambda s: not s['search_pending'] and len(s['rows']) == 2,
                                 'playlist rows visible', timeout=30)
                assert state['count'] == 2, 'visible total did not follow imported playlist'
                if ordinal == 0:
                    scan = state['scan']
                    assert (scan['stage'], scan['discovered'], scan['processed'],
                            scan['succeeded'], scan['skipped'], scan['failed']) == (5, 3, 3, 2, 1, 0), scan
                screenshot = app.command(capture=True)
                assert screenshot['captured']
                screenshot_path = run_dir / f'{app.sequence:04d}.png'
                report['runs'].append({'name': name, 'database': state['database'],
                                       'playlists': state['playlists'], 'count': state['count'],
                                       'visible_rows': [row['name'] for row in state['rows']],
                                       'exit_code': app.close(), 'screenshot': str(screenshot_path)})
            finally:
                if app.process.poll() is None:
                    app.process.terminate()
                    app.process.wait(timeout=5)
                app.retain()
        report['status'] = 'PASS'
    except Exception as error:
        report['error'] = str(error)
        raise
    finally:
        save(output / 'report.json', report)
    print(json.dumps(report, ensure_ascii=False))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--app', type=Path, required=True)
    parser.add_argument('--qa-root', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    run(args.app.resolve(strict=True), args.qa_root.resolve(strict=True), args.out.resolve())
