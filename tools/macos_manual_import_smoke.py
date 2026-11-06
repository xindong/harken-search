#!/usr/bin/env python3
"""Run a dropped audio file through a later scan in the same isolated library."""
import argparse
import hashlib
from pathlib import Path
import sqlite3
import struct
import wave

from macos_library_smoke import App, save


def write_wav(path, amplitude):
    path.parent.mkdir(parents=True, exist_ok=True)
    with wave.open(str(path), 'wb') as stream:
        stream.setparams((1, 2, 48000, 0, 'NONE', 'not compressed'))
        stream.writeframes(struct.pack('<h', amplitude) * 48000)


def db_rows(path):
    with sqlite3.connect(path) as connection:
        return connection.execute(
            'SELECT file_path FROM audio_files ORDER BY file_path').fetchall()


def run(binary, root, output):
    output.mkdir(parents=True, exist_ok=False)
    fixture = root / 'fixtures' / output.name
    manual = fixture / 'outside' / '手动.wav'
    scanned = fixture / 'scan-root' / 'scanned.wav'
    write_wav(manual, 1800)
    write_wav(scanned, 2500)
    hashes = {str(path): hashlib.sha256(path.read_bytes()).hexdigest()
              for path in (manual, scanned)}
    report = {'status': 'FAIL', 'scope': 'manual file survives scan and count stays current'}
    app = App(binary, root, output)
    try:
        app.wait(lambda s: not s['switch_pending'], 'initial mount', 60)
        app.command('create', name='ManualImport-' + output.name)
        opened = app.wait(lambda s: s['database_open'] and not s['switch_pending'] and
                          'ManualImport-' + output.name in s['database'],
                          'isolated library created', 60)
        database = opened['database']
        report['library'] = database
        dropped = app.command('native_audio_drop', path=str(manual))
        assert dropped['native_drop']['accepted'] and dropped['drop_notice']['imported'] == 1
        imported = app.wait(lambda s: not s['search_pending'] and
                            any(row['name'] == manual.name for row in s['rows']),
                            'manual file visible', 40)
        report['count_after_drop'] = imported['count']
        report['db_rows_after_drop'] = db_rows(database)
        shot = app.command(capture=True)
        report['drop_screenshot'] = f'{shot["id"]:04d}.png'
        repeated = app.command('native_audio_drop', path=str(manual))
        report['duplicate_kept_one_row'] = (
            repeated['native_drop']['accepted'] and
            repeated['drop_notice']['existing'] == 1 and
            len(db_rows(database)) == 1)

        app.command('scan', path=str(scanned.parent), worker_threads=1)
        scanned_state = app.wait(lambda s: not s['scan']['active'] and
                                 not s['scan']['pending'] and
                                 s['scan']['succeeded'] == 1,
                                 'second root scan finished', 90)
        report['scan'] = scanned_state['scan']
        app.command('filter', folder='')
        after = app.wait(lambda s: not s['search_pending'], 'search after scan', 30)
        report['count_after_scan'] = after['count']
        report['rows_after_scan'] = [row['name'] for row in after['rows']]
        report['db_rows_after_scan'] = db_rows(database)
        shot = app.command(capture=True)
        report['scan_screenshot'] = f'{shot["id"]:04d}.png'
        app.close()

        reopen_dir = output / 'reopen'
        reopen_dir.mkdir()
        reopened = App(binary, root, reopen_dir)
        try:
            state = reopened.wait(lambda s: s['database_open'] and
                                  s['database'] == database and not s['switch_pending'] and
                                  not s['search_pending'], 'normal restart', 60)
            reopened.command('filter', folder='')
            state = reopened.wait(lambda s: not s['search_pending'], 'reopened search', 30)
            report['count_after_restart'] = state['count']
            report['rows_after_restart'] = [row['name'] for row in state['rows']]
            report['db_rows_after_restart'] = db_rows(database)
            shot = reopened.command(capture=True)
            report['reopen_screenshot'] = f'{shot["id"]:04d}.png'
            reopened.close()
        finally:
            if reopened.process.poll() is None:
                reopened.close()
            reopened.retain()

        report['source_hashes_unchanged'] = all(
            hashlib.sha256(Path(path).read_bytes()).hexdigest() == digest
            for path, digest in hashes.items())
        assert report['count_after_drop'] == 1, report
        assert report['duplicate_kept_one_row'], report
        assert report['count_after_scan'] == 2, report
        assert report['count_after_restart'] == 2, report
        assert {manual.name, scanned.name} <= set(report['rows_after_scan']), report
        assert {manual.name, scanned.name} <= set(report['rows_after_restart']), report
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
