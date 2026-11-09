#!/usr/bin/env python3
"""Verify native Mac database URL drop with two isolated product-created libraries."""
import argparse
import hashlib
from pathlib import Path
import sqlite3
import struct
import wave

from macos_library_smoke import App, cancellation_fixtures, save


def make_wav(path):
    path.parent.mkdir(parents=True, exist_ok=True)
    with wave.open(str(path), 'wb') as stream:
        stream.setparams((1, 2, 48000, 0, 'NONE', 'not compressed'))
        stream.writeframes(struct.pack('<h', 1700) * 48000)


def count_rows(path):
    with sqlite3.connect(path) as database:
        return database.execute('SELECT COUNT(*) FROM audio_files').fetchone()[0]


def run(binary, root, output):
    output.mkdir(parents=True, exist_ok=False)
    app = App(binary, root, output)
    report = {'status': 'FAIL', 'scope': 'single .db native URL drop, failure retention and restart'}
    source = root / 'fixtures' / output.name
    wav_path = source / 'sound.wav'
    bogus = source / 'not-a-library.db'
    other = source / 'notes.txt'

    def click_cancel():
        buttons = app.command()['scan_dialog']['buttons']
        left, top, right, bottom = buttons['scan.dialog.cancel']
        x, y = (left + right) / 2, (top + bottom) / 2
        for phase in ('hover', 'down', 'up'):
            frame = app.command()['scan_dialog']['frame']
            app.command('pointer', x=x, y=y, phase=phase)
            app.wait(lambda s: s['scan_dialog']['frame'] > frame,
                     'cancel pointer frame', 8)

    try:
        app.wait(lambda s: not s['switch_pending'], 'startup mount', 60)
        app.command('create', name='First-' + output.name)
        first = app.wait(lambda s: s['database_open'] and not s['switch_pending'] and
                         'First-' + output.name in s['database'], 'first library', 45)
        first_db = first['database']
        make_wav(wav_path)
        source_hash = hashlib.sha256(wav_path.read_bytes()).hexdigest()
        app.command('scan', path=str(source))
        app.wait(lambda s: not s['scan']['active'] and not s['scan']['pending'] and
                 s['scan']['succeeded'] == 1, 'first library scan', 60)
        assert count_rows(first_db) == 1

        app.command('create', name='Second-' + output.name)
        second = app.wait(lambda s: s['database_open'] and not s['switch_pending'] and
                          'Second-' + output.name in s['database'], 'second library', 45)
        second_db = second['database']
        assert count_rows(second_db) == 0
        report['first_library'] = first_db
        report['second_library'] = second_db

        bogus.write_text('not a SQLite database', encoding='utf-8')
        other.write_text('not a library', encoding='utf-8')
        for paths in ([str(other)], [first_db, second_db], [str(source), first_db]):
            response = app.command('native_database_drop', paths=paths)
            assert response['native_drop'] == {'attempted': True, 'accepted': False}, response
            assert response['database'] == second_db and not response['switch_pending']
        rejected = app.command('native_database_drop', path=str(bogus))
        if rejected['native_drop']['accepted']:
            recovered = app.wait(lambda s: not s['switch_pending'],
                                 'invalid database switch settled', 60)
            assert recovered['database_open'] and recovered['database'] == second_db, recovered
            assert recovered['switch_error'], recovered
            report['invalid_database_recovered'] = recovered['switch_error']
        else:
            assert rejected['database'] == second_db and not rejected['switch_pending']
            report['invalid_database_recovered'] = 'rejected before switch'
        report['invalid_and_mixed_inputs_keep_second_library'] = True

        folder = app.command('native_folder_drop', path=str(source))
        assert folder['native_drop']['accepted'] and folder['scan_dialog']['open']
        assert folder['scan_dialog']['pending_folders'] == [str(source)]
        click_cancel()
        app.wait(lambda s: not s['scan_dialog']['pending_folders'] and
                 not s['scan_dialog']['open'], 'folder drop cancel', 15)
        assert count_rows(second_db) == 0
        report['folder_drop_still_confirms'] = True

        switching = app.command('native_database_drop', path=first_db)
        assert switching['native_drop']['accepted'], switching
        switched = app.wait(lambda s: s['database_open'] and not s['switch_pending'] and
                            s['database'] == first_db, 'dropped database mounted', 60)
        app.command('filter', folder='')
        visible = app.wait(lambda s: not s['search_pending'] and
                           any(r['name'] == 'sound.wav' for r in s['rows']),
                           'first library audio visible', 30)
        assert visible['database'] == first_db and count_rows(first_db) == 1
        assert count_rows(second_db) == 0
        app.command()
        picture = app.command(capture=True)
        report['switched_screenshot'] = f'{picture["id"]:04d}.png'
        report['switched_rows'] = [r['name'] for r in visible['rows']]

        busy = source / 'busy'
        cancellation_fixtures(busy)
        app.command('scan', path=str(busy), worker_threads=1)
        app.wait(lambda s: s['scan']['active'], 'busy scan active', 20)
        refused = app.command('native_database_drop', path=second_db)
        assert not refused['native_drop']['accepted'] and refused['database'] == first_db
        assert refused['scan']['active'] and not refused['switch_pending']
        report['busy_scan_kept_first_library'] = True
        app.command('cancel_scan')
        app.wait(lambda s: not s['scan']['active'] and not s['scan']['pending'],
                 'scan cancellation settled', 120)
        assert hashlib.sha256(wav_path.read_bytes()).hexdigest() == source_hash
        app.close()

        reopen_output = output / 'reopen'
        reopen_output.mkdir()
        reopened = App(binary, root, reopen_output)
        try:
            resumed = reopened.wait(lambda s: s['database_open'] and not s['switch_pending'] and
                                    s['database'] == first_db and not s['search_pending'] and
                                    any(r['name'] == 'sound.wav' for r in s['rows']),
                                    'dropped library and audio retained', 60)
            report['reopened_library'] = resumed['database']
            report['reopened_rows'] = [r['name'] for r in resumed['rows']]
            reopened.command(capture=True)
            reopened.close()
        finally:
            if reopened.process.poll() is None:
                reopened.close()
            reopened.retain()
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
