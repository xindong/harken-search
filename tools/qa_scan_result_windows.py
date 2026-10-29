"""Verify isolated scan progress/result windows without moving the OS pointer.

Uses generated audio and a disposable database under the compile-time QA root.
Leaves its own QA process showing the final result for user acceptance.
"""
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import sqlite3
import struct
import subprocess
import time
import wave

from PIL import ImageGrab
from qa_transport_themes import Driver, window_api


def run(args):
    exe, root, output = args.exe.resolve(), args.isolation_root.resolve(), args.output.resolve()
    if not exe.is_relative_to(root) or not output.is_relative_to(root):
        raise ValueError('Executable and evidence must belong to the requested isolation root')
    output.mkdir(parents=True, exist_ok=True)
    runtime = root / 'profile/runtime'
    runtime.mkdir(parents=True, exist_ok=True)
    driver = Driver(runtime, output)
    fixture = root / 'fixtures/separate-scan-results'
    fixture.mkdir(parents=True, exist_ok=True)
    for i in range(40):
        path = fixture / f'empty-audio-{i:02}.wav'
        if not path.exists():
            path.write_bytes(b'RIFF')
    second = b''.join(struct.pack('<h', int(300 * math.sin(i * 2 * math.pi * 440 / 16000)))
                      for i in range(16000))
    for i in range(64):
        path = fixture / f'tone-{i:02}.wav'
        if not path.exists():
            with wave.open(str(path), 'wb') as sound:
                sound.setparams((1, 2, 16000, 0, 'NONE', 'not compressed'))
                sound.writeframes(second * 10)
    not_folder = root / 'fixtures/separate-scan-not-a-folder'
    if not not_folder.exists():
        not_folder.write_text('Not a directory: isolated QA fixture', encoding='utf-8')
    library = root / 'profile/library/separate-scan-results.db'
    library.parent.mkdir(parents=True, exist_ok=True)
    if not library.exists():
        with sqlite3.connect(library) as db:
            db.execute('PRAGMA user_version=1')

    startup = subprocess.STARTUPINFO()
    startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
    startup.wShowWindow = 4  # Show without activating the user's desktop.
    process = subprocess.Popen([str(exe)], cwd=exe.parent, startupinfo=startup,
        env=dict(os.environ, AURA_HARNESS_MULTI_INSTANCE='1',
                 AURA_HARNESS_LANE_KIND='functional', AURA_DISABLE_SINGLE_INSTANCE='1'))
    checks = []
    identity = driver.command({'cmd': 'magic_eq_identity'}, timeout=50)
    assert identity['pid'] == process.pid and Path(identity['exe_path']).resolve() == exe, identity
    assert Path(identity['isolation_root']).resolve() == root, identity
    (output / 'identity.json').write_text(json.dumps(dict(identity,
        package_sha256=hashlib.sha256(exe.read_bytes()).hexdigest()), ensure_ascii=False, indent=2), encoding='utf-8')
    user32 = window_api()
    user32.SetWindowPos(identity['hwnd'], None, 40, 40, 1400, 900, 0x0014)

    def command(**payload):
        return driver.command(payload)

    def wait_for(getter, predicate, label, timeout=45):
        deadline = time.monotonic() + timeout
        latest = None
        while time.monotonic() < deadline:
            if process.poll() is not None:
                raise RuntimeError(f'QA process exited: {process.returncode}')
            latest = getter()
            if predicate(latest):
                return latest
            time.sleep(.06)
        raise AssertionError((label, latest))

    def ui():
        value = command(cmd='info_page_state')
        zones = value['zones']
        assert not ('info.qaScanStop' in zones and 'info.qaScanResultDone' in zones), value
        return value

    def capture(name):
        image = ImageGrab.grab(window=identity['hwnd'])
        assert len(image.convert('RGB').getcolors(image.width * image.height) or []) > 20
        image.save(output / (name + '.png'))

    def result_ready():
        snap = wait_for(ui, lambda v: 'info.qaScanResultDone' in v['zones'], 'result popup')
        return wait_for(ui, lambda v: v['frame'] >= snap['frame'] + 3, 'result render')

    def close_result():
        for phase in ('hover', 'down', 'up'):
            command(cmd='info_page_pointer', key='info.qaScanResultDone', phase=phase)
            time.sleep(.12)
        closed = wait_for(ui, lambda v: 'info.qaScanResultDone' not in v['zones'], 'Done closes result')
        later = wait_for(ui, lambda v: v['frame'] >= closed['frame'] + 12, 'result stays closed')
        assert 'info.qaScanStop' not in later['zones'] and 'info.qaScanResultDone' not in later['zones'], later
        status = command(cmd='scan_status')
        assert status['errors'] == 0 and status['total'] == 0, status
        return status

    def start(path, full=False):
        command(cmd='open_scan_dialog')
        command(cmd='scan_folder', path=str(path), recursive=True, update_existing=False,
                remove_missing=False, cache_waveforms=full, build_similarity_index=False, worker_threads=2)

    try:
        wait_for(lambda: command(cmd='magic_eq_state'), lambda v: v.get('main_ui_ready'), 'main UI')
        command(cmd='switch_db', path=str(library))
        wait_for(lambda: command(cmd='get_state'),
                 lambda v: Path(v.get('db_path', '')).resolve() == library and not v.get('db_switch_in_progress', False),
                 'test database')
        start(fixture, full=True)
        wait_for(ui, lambda v: 'info.qaScanStop' in v['zones'], 'progress popup')
        capture('01-scan-progress')
        result_ready()
        status = command(cmd='scan_status')
        assert not status['scanning'] and not status['scan_completion_pending'], status
        assert status['total'] == 104 and status['errors'] == 40 and status['failure_no_audio'] == 40, status
        counts = [status['failure_' + key] for key in ('inaccessible', 'unreadable_file', 'no_audio', 'database_write', 'other')]
        assert sum(counts) == status['errors'] and len(status['error_details']) == 32, status
        capture('02-failure-results')
        checks.append({'case': '40 categorized failures with 32 detail samples', 'status': status})
        close_result()
        capture('03-result-dismissed')

        start(not_folder)
        result_ready()
        status = command(cmd='scan_status')
        assert status['total'] == 0 and status['errors'] == 1 and status['failure_inaccessible'] == 1, status
        capture('04-inaccessible-results')
        checks.append({'case': 'inaccessible folder with zero discovered files', 'status': status})
        close_result()

        start(fixture)
        result_ready()
        status = command(cmd='scan_status')
        assert status['errors'] == 40 and status['failure_no_audio'] == 40 and status['failure_inaccessible'] == 0, status
        capture('05-repeat-results')
        checks.append({'case': 'new scan resets old failure counts', 'status': status})
        report = {'passed': True, 'pid': process.pid, 'exe': str(exe), 'checks': checks}
        (output / 'report.json').write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
        print(json.dumps({'passed': True, 'pid': process.pid, 'report': str(output / 'report.json')}, ensure_ascii=False))
    except Exception as error:
        (output / 'failure.json').write_text(json.dumps({'error': str(error), 'pid': process.pid,
            'checks': checks}, ensure_ascii=False, indent=2), encoding='utf-8')
        raise


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exe', type=Path, required=True)
    parser.add_argument('--isolation-root', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    run(parser.parse_args())
