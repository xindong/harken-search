#!/usr/bin/env python3
"""Exercise a single native audio URL drop in the real isolated Mac app."""
import argparse
import hashlib
import math
from pathlib import Path
import sqlite3
import struct
import wave

from macos_library_smoke import App, cancellation_fixtures, save


def make_wav(path):
    path.parent.mkdir(parents=True, exist_ok=True)
    samples = (struct.pack('<h', round(6000 * math.sin(2 * math.pi * 440 * index / 48000)))
               for index in range(48000 * 6))
    with wave.open(str(path), 'wb') as stream:
        stream.setparams((1, 2, 48000, 0, 'NONE', 'not compressed'))
        stream.writeframes(b''.join(samples))


def row_count(database):
    with sqlite3.connect(database) as connection:
        return connection.execute('SELECT COUNT(*) FROM audio_files').fetchone()[0]


def path_row_count(database, path):
    with sqlite3.connect(database) as connection:
        return connection.execute('SELECT COUNT(*) FROM audio_files WHERE file_path = ?',
                                  (str(path),)).fetchone()[0]


def run(binary, root, output):
    output.mkdir(parents=True, exist_ok=False)
    app = App(binary, root, output)
    report = {'status': 'FAIL', 'scope': 'native single audio URL, import, audition and restart'}
    fixture = root / 'fixtures' / output.name
    valid = fixture / '钟声.WAV'
    broken = fixture / 'broken.wav'
    plain = fixture / 'plain.txt'
    make_wav(valid)
    source_hash = hashlib.sha256(valid.read_bytes()).hexdigest()
    broken.write_bytes(b'not a WAV stream')
    plain.write_text('not audio', encoding='utf-8')
    try:
        initial = app.wait(lambda state: not state['switch_pending'], 'initial mount', 60)
        report['started_without_library'] = not initial['database_open']
        if not initial['database_open']:
            assert not app.command('native_audio_drop', path=str(valid))['native_drop']['accepted']

        app.command('create', name='AudioDrop-' + output.name)
        state = app.wait(lambda value: value['database_open'] and not value['switch_pending'] and
                         'AudioDrop-' + output.name in value['database'], 'library open', 60)
        database = state['database']
        assert row_count(database) == 0
        for paths in ([str(plain)], [str(valid), str(broken)], [str(fixture), str(valid)]):
            rejected = app.command('native_audio_drop', paths=paths)
            assert rejected['native_drop'] == {'attempted': True, 'accepted': False}, rejected
            assert row_count(database) == 0
        report['invalid_and_mixed_inputs_rejected'] = True

        dropped = app.command('native_audio_drop', path=str(valid))
        assert dropped['native_drop']['accepted'], dropped
        assert dropped['drop_notice']['imported'] == 1 and dropped['drop_notice']['failed'] == 0
        imported = app.wait(lambda value: not value['search_pending'] and
                            any(row['name'] == valid.name for row in value['rows']) and
                            value['audio']['loaded'] and value['audio']['wave_ready'],
                            'audio indexed, loaded and wave generated', 60)
        assert row_count(database) == 1
        row = next(row for row in imported['rows'] if row['name'] == valid.name)
        assert row['duration'] > 5.9 and row['sample_rate'] == 48000 and row['channels'] == 1, row
        assert imported['audio']['path'] == str(valid)
        report['imported_row'] = row
        report['count_immediately_after_drop'] = imported['count']
        report['audio_loaded_and_wave_ready'] = True
        playing = app.wait(lambda value: value['audio']['playing'] and
                           value['audio']['position'] > 0.05 and
                           value['audio']['output']['nonzero_samples'] > 0,
                           'real playback output', 30)
        app.command('toggle_playback')
        paused = app.wait(lambda value: value['audio']['paused'] and
                          not value['audio']['playing'], 'pause audio drop preview', 20)
        report['playback'] = {'position': playing['audio']['position'],
                              'nonzero_samples': playing['audio']['output']['nonzero_samples'],
                              'paused': paused['audio']['paused']}
        app.command(capture=True)

        repeat = app.command('native_audio_drop', path=str(valid))
        assert repeat['native_drop']['accepted'] and row_count(database) == 1
        assert repeat['drop_notice']['existing'] == 1 and repeat['drop_notice']['imported'] == 0
        report['repeat_kept_one_row'] = True
        damaged = app.command('native_audio_drop', path=str(broken))
        assert damaged['native_drop']['accepted'] and row_count(database) == 1, damaged
        assert damaged['drop_notice']['failed'] == 1 and damaged['drop_notice']['imported'] == 0
        report['damaged_audio_not_imported'] = True

        # Keep the busy-scan check in a second library: shared scan-root pruning
        # removes out-of-root manual imports, a separate Windows/Mac issue.
        app.command('create', name='Busy-' + output.name)
        busy_library = app.wait(lambda value: value['database_open'] and
                                not value['switch_pending'] and
                                'Busy-' + output.name in value['database'],
                                'busy-check library open', 60)['database']
        busy_folder = fixture / 'busy'
        cancellation_fixtures(busy_folder)
        app.command('scan', path=str(busy_folder), worker_threads=1)
        app.wait(lambda value: value['scan']['active'], 'scan active', 20)
        busy = app.command('native_audio_drop', path=str(valid))
        assert not busy['native_drop']['accepted'] and busy['scan']['active'], busy
        assert path_row_count(busy_library, valid) == 0
        app.command('cancel_scan')
        app.wait(lambda value: not value['scan']['active'] and not value['scan']['pending'],
                 'cancelled scan settled', 120)
        report['busy_scan_rejected_drop'] = True
        app.command('open', path=database)
        app.wait(lambda value: value['database'] == database and not value['switch_pending'],
                 'return to imported library', 60)
        assert path_row_count(database, valid) == 1
        assert hashlib.sha256(valid.read_bytes()).hexdigest() == source_hash
        app.close()

        reopened_output = output / 'reopen'
        reopened_output.mkdir()
        reopened = App(binary, root, reopened_output)
        try:
            resumed = reopened.wait(lambda value: value['database_open'] and
                                    not value['switch_pending'] and not value['search_pending'] and
                                    value['database'] == database and
                                    any(row['name'] == valid.name for row in value['rows']),
                                    'import retained after normal restart', 60)
            assert path_row_count(database, valid) == 1
            reopened.command(capture=True)
            reopened.close()
            report['reopened_rows'] = [row['name'] for row in resumed['rows']]
            report['reopened_count'] = resumed['count']
        finally:
            if reopened.process.poll() is None:
                reopened.close()
            reopened.retain()
        report['library'] = database
        report['source_sha256_unchanged'] = source_hash
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
