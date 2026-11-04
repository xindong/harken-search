#!/usr/bin/env python3
"""Exercise the actual Cocoa drop handlers using a private file-URL pasteboard."""
import argparse
import hashlib
from pathlib import Path
import sqlite3
import struct
import wave

from macos_library_smoke import App, save, cancellation_fixtures


def wav(path, sample):
    path.parent.mkdir(parents=True, exist_ok=True)
    with wave.open(str(path), 'wb') as stream:
        stream.setparams((1, 2, 48000, 0, 'NONE', 'not compressed'))
        stream.writeframes(struct.pack('<h', sample) * 48000)


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(binary, root, output):
    output.mkdir(parents=True, exist_ok=False)
    app = App(binary, root, output)
    report = {'status': 'FAIL', 'scope': 'Cocoa folder URL drop, confirmation, scan, cancel, restart'}
    source = root / 'fixtures' / output.name
    first = source / 'A'
    second = source / '中文目录'
    paths = [first / 'impact.wav', second / '风声.wav']
    loose = source / 'loose.wav'

    def snapshot():
        return app.command()

    def click(key):
        rect = snapshot()['scan_dialog']['buttons'][key]
        x, y = (rect[0] + rect[2]) / 2, (rect[1] + rect[3]) / 2
        for phase in ('hover', 'down', 'up'):
            before = snapshot()['scan_dialog']['frame']
            app.command('pointer', x=x, y=y, phase=phase)
            app.wait(lambda s: s['scan_dialog']['frame'] > before,
                     'pointer frame advanced', 8)

    try:
        app.wait(lambda s: not s['switch_pending'], 'startup mount', 60)
        app.command('create', name='Drop-' + output.name)
        created = app.wait(lambda s: s['database_open'] and not s['switch_pending'], 'library created')
        report['library'] = created['database']
        for index, path in enumerate(paths):
            wav(path, 1200 + index * 500)
        wav(loose, 700)
        hashes = {str(path): sha(path) for path in (*paths, loose)}
        assert snapshot()['count'] == 0

        rejected = app.command('native_folder_drop', path=str(loose))
        assert rejected['native_drop'] == {'attempted': True, 'accepted': False}
        assert rejected['scan_dialog']['pending_folders'] == []
        mixed = app.command('native_folder_drop', paths=[str(first), str(loose)])
        assert not mixed['native_drop']['accepted'] and mixed['scan_dialog']['pending_folders'] == []
        missing = app.command('native_folder_drop', path=str(source / 'absent'))
        assert not missing['native_drop']['accepted']
        report['rejected_file_and_mixed'] = True

        drop = app.command('native_folder_drop', paths=[str(first), str(first), str(second)])
        assert drop['native_drop'] == {'attempted': True, 'accepted': True}, drop
        assert drop['scan_dialog']['open'] and not drop['scan_dialog']['auto_start'], drop['scan_dialog']
        assert drop['scan_dialog']['pending_folders'] == [str(first), str(second)], drop['scan_dialog']
        assert drop['count'] == 0 and not drop['scan']['active']
        app.command()
        shot = app.command(capture=True)
        report['confirm_screenshot'] = f'{shot["id"]:04d}.png'
        click('scan.dialog.cancel')
        cancelled = app.wait(lambda s: not s['scan_dialog']['open'] and
                             not s['scan_dialog']['pending_folders'], 'cancel closes dialog')
        assert cancelled['count'] == 0 and not cancelled['scan']['active']
        report['cancel_kept_library_empty'] = True

        drop = app.command('native_folder_drop', paths=[str(first), str(second)])
        assert drop['native_drop']['accepted'] and drop['scan_dialog']['open']
        app.command()
        shot = app.command(capture=True)
        report['start_screenshot'] = f'{shot["id"]:04d}.png'
        click('scan.dialog.start')
        result = app.wait(lambda s: not s['scan']['active'] and not s['scan']['pending'] and
                          s['scan']['succeeded'] == 2,
                          'two files scanned after explicit confirmation', 90)
        assert result['scan']['succeeded'] == 2 and result['scan']['failed'] == 0, result['scan']
        assert result['scan']['duration_ms'] > 0, result['scan']
        report['scan'] = result['scan']
        with sqlite3.connect(created['database']) as database:
            assert database.execute('SELECT COUNT(*) FROM audio_files').fetchone()[0] == 2
        # A completed scan intentionally focuses the first scanned folder.
        # Clear only that view filter before asserting both imported rows.
        app.command('filter', folder='')
        result = app.wait(lambda s: not s['search_pending'] and s['count'] == 2,
                          'both imported files visible without folder filter', 30)
        assert {row['name'] for row in result['rows']} == {'impact.wav', '风声.wav'}

        busy = source / 'busy'
        cancellation_fixtures(busy)
        app.command('scan', path=str(busy), worker_threads=1)
        app.wait(lambda s: s['scan']['active'], 'busy scan active', 20)
        busy_drop = app.command('native_folder_drop', path=str(first))
        assert busy_drop['native_drop']['accepted'] and busy_drop['scan']['active']
        assert busy_drop['scan_dialog']['ignored_count'] == 1
        assert not busy_drop['scan_dialog']['pending_folders']
        report['busy_drop_not_queued'] = True
        app.command()
        shot = app.command(capture=True)
        report['busy_screenshot'] = f'{shot["id"]:04d}.png'
        if busy_drop['scan']['active']:
            app.command('cancel_scan')
        app.wait(lambda s: not s['scan']['active'] and not s['scan']['pending'], 'busy scan settled', 120)
        assert all(sha(Path(path)) == digest for path, digest in hashes.items())
        report['source_hashes_unchanged'] = hashes
        app.close()

        reopened_output = output / 'reopen'
        reopened_output.mkdir()
        reopened = App(binary, root, reopened_output)
        try:
            reopened.wait(lambda s: s['database_open'] and not s['switch_pending'], 'reopened library', 60)
            resumed = reopened.wait(lambda s: not s['search_pending'] and
                                    {'impact.wav', '风声.wav'} <= {r['name'] for r in s['rows']},
                                    'dropped files remain after restart', 30)
            report['reopened_count'] = resumed['count']
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
