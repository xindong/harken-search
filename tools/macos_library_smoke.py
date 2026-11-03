#!/usr/bin/env python3
"""Drive the real isolated Mac application without system mouse/keyboard input."""
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import shutil
import sqlite3
import struct
import subprocess
import time
import wave
import zlib


def save(path, value):
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')


class App:
    def __init__(self, binary, root, output):
        self.root, self.output = root, output
        self.runtime = root / 'runtime'
        self.runtime.mkdir(parents=True, exist_ok=True)
        self.sequence = 0
        if (self.runtime / 'command.json').exists():
            self.sequence = json.loads((self.runtime / 'command.json').read_text())['id']
        self.log = []
        self.log_offsets = {name: (self.runtime / name).stat().st_size
                            if (self.runtime / name).exists() else 0
                            for name in ['events.jsonl', 'startup.txt']}
        self.stdout = (output / 'stdout.log').open('wb')
        self.stderr = (output / 'stderr.log').open('wb')
        self.process = subprocess.Popen([str(binary), '--mac-qa-control'], stdout=self.stdout, stderr=self.stderr)
        self.started = time.monotonic()
        save(output / 'process.json', {'pid': self.process.pid, 'binary': str(binary),
             'sha256': hashlib.sha256(binary.read_bytes()).hexdigest()})

    def command(self, action='snapshot', **fields):
        self.sequence += 1
        request = dict(id=self.sequence, action=action, **fields)
        temporary = self.runtime / 'command.tmp'
        save(temporary, request)
        temporary.replace(self.runtime / 'command.json')
        deadline = time.monotonic() + 60
        while time.monotonic() < deadline:
            response = self.runtime / 'response.json'
            if response.exists():
                try:
                    value = json.loads(response.read_text())
                except (ValueError, OSError):
                    value = {}
                if value.get('id') == self.sequence:
                    self.log.append({'time': round(time.monotonic() - self.started, 3),
                                     'request': request, 'response': value})
                    if value['error']:
                        raise AssertionError(value['error'])
                    if fields.get('capture'):
                        assert value['captured'], 'framebuffer capture failed'
                        shutil.copy2(self.runtime / 'main-window.png', self.output / f'{self.sequence:04d}.png')
                    return value
            if self.process.poll() is not None:
                raise AssertionError(f'application exited {self.process.returncode}')
            time.sleep(0.04)
        raise AssertionError(f'command timed out: {request}')

    def wait(self, predicate, label, timeout=45):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            value = self.command()
            if predicate(value):
                return value
            time.sleep(0.1)
        raise AssertionError(f'{label}: {value}')

    def close(self):
        if self.process.poll() is None:
            self.command('close')
        code = self.process.wait(timeout=20)
        assert code == 0, f'normal exit returned {code}'
        return code

    def retain(self):
        save(self.output / 'commands.json', self.log)
        for name in ['events.jsonl', 'startup.txt']:
            source = self.runtime / name
            if source.exists():
                with source.open('rb') as stream:
                    stream.seek(self.log_offsets[name])
                    (self.output / name).write_bytes(stream.read())
        self.stdout.close()
        self.stderr.close()


def fixtures(root):
    root.mkdir(parents=True, exist_ok=False)
    names = ['A/stone-impact.wav', 'A/stone-soft.wav', 'A/bird.wav',
             'B/stone-impact.wav', 'B/bird.wav', 'B/风声.wav']
    for index, name in enumerate(names):
        path = root / name
        path.parent.mkdir(exist_ok=True)
        with wave.open(str(path), 'wb') as stream:
            stream.setparams((1, 2, 48000, 0, 'NONE', 'not compressed'))
            data = b''.join(struct.pack('<h', round(4000 * math.sin(2 * math.pi * (220 + index * 70) * n / 48000)))
                            for n in range(48000 * 3))
            stream.writeframes(data)
    # Independently generated cover, no external media or daily library inputs.
    def chunk(kind, data):
        return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data))
    pixels = b''.join(b'\x00' + bytes([y * 4, 100, 160]) * 64 for y in range(64))
    png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', 64, 64, 8, 2, 0, 0, 0))
    png += chunk(b'IDAT', zlib.compress(pixels)) + chunk(b'IEND', b'')
    (root / 'A/folder.png').write_bytes(png)
    (root / 'B/damaged.wav').write_bytes(b'this is not a RIFF audio file')
    return names


def cancellation_fixtures(root):
    root.mkdir(parents=True, exist_ok=False)
    # Short, valid generated WAVs; one worker leaves a measurable cancellation
    # window without sleeping in or altering the product scanner.
    payload = struct.pack('<h', 1000) * 4800
    for index in range(5000):
        with wave.open(str(root / f'cancel-{index:04d}.wav'), 'wb') as stream:
            stream.setparams((1, 2, 48000, 0, 'NONE', 'not compressed'))
            stream.writeframes(payload)


def run(binary, root, output, create_only=False):
    output.mkdir(parents=True, exist_ok=False)
    app = App(binary, root, output)
    report = {'status': 'FAIL', 'scope': 'real library creation, scan, filters and restart'}
    try:
        initial = app.wait(lambda s: not s['switch_pending'], 'startup mount', 60)
        changed_font = 22 if initial['reading_font'] != 22 else 18
        name = 'Mac-' + output.name
        app.command('create', name=name)
        value = app.wait(lambda s: s['database_open'] and not s['switch_pending'] and name in s['database'], 'create library')
        app.command(capture=True)
        startup = dict(line.split('=', 1) for line in (app.runtime / 'startup.txt').read_text().splitlines() if '=' in line)
        expected = Path(startup['default_library_directory']) / name / 'main.db'
        assert Path(value['database']) == expected, f'library path: {value["database"]}; expected {expected}'
        assert expected.is_file(), 'library file missing'
        report['library'] = str(expected)
        if not create_only:
            source = root / 'fixtures' / output.name
            fixtures(source)
            app.command('scan', path=str(source))
            value = app.wait(lambda s: not s['scan']['active'] and not s['scan']['pending'] and s['scan']['discovered'] > 0, 'scan completion', 90)
            report['scan'] = value['scan']
            # The shared scanner deliberately persists terminal failure rows
            # for its completion ledger (Database::scanDirectoryEx). A failed
            # row must not be mistaken for successfully decoded audio.
            value = app.wait(lambda s: not s['search_pending'] and s['count'] == 7 and len(s['rows']) == 7, 'visible rows after scan')
            assert value['scan']['failed'] == 1, value['scan']
            assert value['scan']['errors'], 'damaged audio failure reason missing'
            assert value['scan']['duration_ms'] > 0, 'scan duration missing'
            good = [row for row in value['rows'] if row['name'] != 'damaged.wav']
            failed = [row for row in value['rows'] if row['name'] == 'damaged.wav']
            assert len(good) == 6 and len(failed) == 1
            assert all(row['duration'] > 2.9 and row['sample_rate'] == 48000 and row['channels'] == 1 for row in good), good
            assert failed[0]['duration'] == 0 and failed[0]['sample_rate'] == 0
            report['visible_rows'] = {'valid': 6, 'failed_terminal_records': 1}
            app.command(capture=True)
            app.command('filter', text='stone', folder='')
            app.wait(lambda s: not s['search_pending'] and s['count'] == 3, 'keyword filter')
            app.command('filter', folder=str(source / 'A'))
            value = app.wait(lambda s: not s['search_pending'] and s['count'] == 2, 'folder plus keyword')
            assert value['filters']['text'] == 'stone'
            app.command('tag', file_name='stone-impact.wav', tags=['impact'])
            app.command('filter', text='stone tag:impact')
            value = app.wait(lambda s: not s['search_pending'] and s['count'] == 1, 'tag plus folder plus keyword')
            assert value['filters']['folder'] == str(source / 'A')
            app.command('filter', cover_mode=1)
            value = app.wait(lambda s: not s['search_pending'] and s['count'] == 1, 'cover plus all other filters')
            assert value['filters']['text'] == 'stone tag:impact' and value['filters']['folder_active']
            app.command(capture=True)
            app.command('filter', text='bird tag:impact')
            value = app.wait(lambda s: not s['search_pending'] and s['count'] == 0, 'change keyword preserves tag, folder and cover')
            assert value['filters']['cover_mode'] == 1 and value['filters']['folder_active']
            app.command('filter', text='bird')
            app.wait(lambda s: not s['search_pending'] and s['count'] == 1, 'remove tag preserves keyword, folder and cover')
            app.command('filter', text='', folder='', cover_mode=0)
            app.wait(lambda s: not s['search_pending'] and s['count'] == 7, 'clear explicit filters')
            app.command('scan', path=str(source))
            value = app.wait(lambda s: not s['scan']['active'] and not s['scan']['pending'] and s['scan']['discovered'] > 0, 'repeat scan', 90)
            app.wait(lambda s: not s['search_pending'] and s['count'] == 7, 'repeat scan does not duplicate rows')
            report['repeat_scan'] = value['scan']
            app.command('filter', text='stone tag:impact', folder=str(source / 'A'), cover_mode=1)
            app.wait(lambda s: not s['search_pending'] and s['count'] == 1, 'selection before restart')
            app.command('settings', reading_font=changed_font)
        report['exit_code'] = app.close()
        app.retain()
        if not create_only:
            reopen = output / 'reopen'
            reopen.mkdir()
            app = App(binary, root, reopen)
            value = app.wait(lambda s: s['database'] == str(expected) and not s['switch_pending'] and not s['search_pending'] and s['count'] == 7, 'reopen restores library', 60)
            assert value['reading_font'] == changed_font, value
            # Match the shared Windows startup contract: mount resets transient
            # result filters; library identity and persistent settings survive.
            assert value['filters'] == {'text': '', 'folder': '', 'folder_active': False, 'cover_mode': 0}
            app.command(capture=True)
            report['restart'] = {'library': value['database'], 'reading_font': value['reading_font'], 'filters': value['filters'], 'count': value['count']}
            app.command('filter', text='', folder='', cover_mode=0)
            app.wait(lambda s: not s['search_pending'] and s['count'] == 7, 'clear restored filters')
            empty_name = name + '-empty'
            app.command('create', name=empty_name)
            app.wait(lambda s: empty_name in s['database'] and not s['switch_pending'] and not s['search_pending'] and s['count'] == 0, 'switch to new empty library')
            app.command('open', path=str(expected))
            app.wait(lambda s: s['database'] == str(expected) and not s['switch_pending'] and not s['search_pending'] and s['count'] == 7, 'open original library')
            cancel_source = root / 'fixtures' / (output.name + '-cancel')
            cancellation_fixtures(cancel_source)
            def committed_scan(snapshot):
                if not snapshot['scan']['active']:
                    return False
                # Read the real isolated WAL through a separate read-only
                # connection, proving a batch was committed before stopping.
                with sqlite3.connect(expected.as_uri() + '?mode=ro', uri=True) as connection:
                    snapshot['committed_count'] = connection.execute(
                        'SELECT COUNT(*) FROM audio_files').fetchone()[0]
                return snapshot['committed_count'] > 7
            app.command('scan', path=str(cancel_source), worker_threads=1)
            before = app.wait(committed_scan, 'scan commits a batch before cancellation', 30)
            app.command('cancel_scan')
            value = app.wait(lambda s: not s['scan']['active'] and not s['scan']['pending'] and s['scan']['stage'] == 7, 'scan cancellation completes', 60)
            app.command('filter', text='', folder='', cover_mode=0)
            app.wait(lambda s: not s['search_pending'] and s['count'] == 7, 'cancel restores original library count')
            report['cancel'] = {'observed_before': before['scan'],
                                'committed_before': before['committed_count'], 'after': value['scan']}
            app.command('scan', path=str(cancel_source), worker_threads=1)
            before_close = app.wait(committed_scan, 'scan commits a batch before exit', 30)
            report['exit_during_scan'] = {'scan': before_close['scan'],
                                        'committed_before': before_close['committed_count'], 'exit_code': app.close()}
            app.retain()
            recovered = output / 'reopen-after-scan-exit'
            recovered.mkdir()
            app = App(binary, root, recovered)
            value = app.wait(lambda s: s['database'] == str(expected) and not s['switch_pending'] and not s['search_pending'] and s['count'] == 7, 'reopen after interrupted scan', 60)
            app.command(capture=True)
            report['recovered_count'] = value['count']
            report['final_exit_code'] = app.close()
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
    parser.add_argument('--create-only', action='store_true')
    args = parser.parse_args()
    for path in [args.app, args.root, args.out]:
        if not path.is_absolute() or path.resolve() != path:
            parser.error('absolute non-symlink paths required')
    if 'qa' not in args.root.parts or 'qa' not in args.out.parts:
        parser.error('dedicated qa roots required')
    raise SystemExit(0 if run(args.app, args.root, args.out, args.create_only) else 1)
