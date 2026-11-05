#!/usr/bin/env python3
"""Exercise a small audio URL batch in the real isolated Cocoa drop entry."""
import argparse
import hashlib
from pathlib import Path
import sqlite3

from macos_library_smoke import App, save
from macos_single_audio_drop_smoke import make_wav


def rows(database):
    with sqlite3.connect(database) as connection:
        return connection.execute(
            'SELECT file_path FROM audio_files ORDER BY file_path').fetchall()


def run(binary, root, output):
    output.mkdir(parents=True, exist_ok=False)
    fixture = root / 'fixtures' / output.name
    fixture.mkdir(parents=True)
    broken = fixture / 'broken.wav'
    first = fixture / 'first.wav'
    second = fixture / '第二.WAV'
    plain = fixture / 'plain.txt'
    make_wav(first)
    make_wav(second)
    broken.write_bytes(b'not a WAV stream')
    plain.write_text('not audio', encoding='utf-8')
    source_hashes = {str(path): hashlib.sha256(path.read_bytes()).hexdigest()
                     for path in (broken, first, second)}
    report = {'status': 'FAIL', 'scope': 'Cocoa multi-audio import and restart'}
    app = App(binary, root, output)
    try:
        app.wait(lambda state: not state['switch_pending'], 'startup mount', 60)
        app.command('create', name='MultiDrop-' + output.name)
        state = app.wait(lambda value: value['database_open'] and
                         not value['switch_pending'] and
                         'MultiDrop-' + output.name in value['database'],
                         'isolated library', 60)
        database = state['database']
        report['library'] = database
        dropped = app.command('native_audio_drop', paths=[str(broken), str(first), str(second)])
        report['first_drop'] = dropped['native_drop']
        assert dropped['native_drop']['accepted'], dropped
        assert (dropped['drop_notice']['imported'] == 2 and
                dropped['drop_notice']['failed'] == 1), dropped
        state = app.wait(lambda value: not value['search_pending'] and
                         value['count'] == 2 and len(value['rows']) == 2 and
                         value['audio']['loaded'] and value['audio']['wave_ready'],
                         'two rows and first valid audio ready', 60)
        assert state['audio']['path'] == str(first), state['audio']
        assert len(rows(database)) == 2
        report['count_after_drop'] = state['count']
        report['selected_audio'] = state['audio']['path']
        shot = app.command(capture=True)
        report['drop_screenshot'] = f'{shot["id"]:04d}.png'

        repeated = app.command('native_audio_drop', paths=[str(first), str(first)])
        assert (repeated['native_drop']['accepted'] and
                repeated['drop_notice']['existing'] == 1 and
                repeated['drop_notice']['imported'] == 0 and
                len(rows(database)) == 2), repeated
        report['duplicate_batch_kept_one_row'] = True
        for paths in ([str(first), str(plain)], [str(first), str(fixture)],
                      [str(first), str(database)], [str(first), str(fixture / 'missing.wav')]):
            rejected = app.command('native_audio_drop', paths=paths)
            assert not rejected['native_drop']['accepted'] and len(rows(database)) == 2, rejected
        report['mixed_and_missing_rejected'] = True

        app.close()
        reopen_dir = output / 'reopen'
        reopen_dir.mkdir()
        reopened = App(binary, root, reopen_dir)
        try:
            state = reopened.wait(lambda value: value['database_open'] and
                                  value['database'] == database and not value['search_pending'],
                                  'reopen same library', 60)
            assert state['count'] == 2 and len(rows(database)) == 2, state
            report['count_after_restart'] = state['count']
            report['rows_after_restart'] = [row['name'] for row in state['rows']]
            shot = reopened.command(capture=True)
            report['restart_screenshot'] = f'{shot["id"]:04d}.png'
            reopened.close()
        finally:
            if reopened.process.poll() is None:
                reopened.close()
            reopened.retain()

        report['source_hashes_unchanged'] = all(
            hashlib.sha256(Path(path).read_bytes()).hexdigest() == digest
            for path, digest in source_hashes.items())
        assert report['source_hashes_unchanged'], report
        report['status'] = 'PASS'
    finally:
        if app.process.poll() is None:
            app.close()
        app.retain()
        save(output / 'report.json', report)
    return report


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--app', type=Path, required=True)
    parser.add_argument('--qa-root', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    print(run(args.app, args.qa_root, args.out))
