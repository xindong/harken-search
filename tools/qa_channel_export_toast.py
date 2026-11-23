"""Drive a real single-channel export in the pinned preview EXE.

Captures the bottom-right task-progress overlay in its running, success and
failure states. Requires Windows, pywin32 and Pillow. The EXE must be built for
the supplied isolation root; a new output folder is created per run.
Exit 0: all three overlay states were observed and captured; 1: failed.
"""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import time
import uuid

from qa_required_library_setup import PrivateDesktop, capture, wait_state
from qa_transport_themes import Driver


def sha256(path):
    digest = hashlib.sha256()
    with path.open('rb') as source:
        for block in iter(lambda: source.read(1 << 20), b''):
            digest.update(block)
    return digest.hexdigest()


def run(args):
    import win32con
    import win32event

    exe = args.exe.resolve()
    root = args.isolation_root.resolve()
    out = args.output.resolve()
    fixture = args.fixture.resolve()
    assert exe.is_file(), 'EXE missing: ' + str(exe)
    assert fixture.is_file(), 'Fixture missing: ' + str(fixture)
    out.mkdir(parents=True, exist_ok=False)
    runtime = root / 'profile/runtime'
    runtime.mkdir(parents=True, exist_ok=True)
    desktop = PrivateDesktop()
    driver = Driver(runtime, out)
    process = None
    checks = []
    report = {'exe': str(exe), 'exe_sha256': sha256(exe),
              'started_at': datetime.now(timezone.utc).isoformat(),
              'fixture': str(fixture), 'checks': checks}

    def start():
        nonlocal process
        process, pid = desktop.start(exe)
        identity = driver.command({'cmd': 'magic_eq_identity'}, timeout=90)
        assert identity['pid'] == pid, identity
        assert Path(identity['isolation_root']).resolve() == root, identity
        report['identity'] = identity
        wait_state(driver, lambda s: s['main_ui_ready'] or s['new_db_dialog_open'],
                   'Startup must reach the main UI or the first-library setup', timeout=120)

    def stop():
        nonlocal process
        if process is None:
            return
        if win32event.WaitForSingleObject(process, 0) == win32con.WAIT_TIMEOUT:
            driver.command({'cmd': 'info_edit_input', 'action': 'exit'})
        assert win32event.WaitForSingleObject(process, 30000) == win32con.WAIT_OBJECT_0, 'QA process did not exit'
        process.Close()
        process = None

    def export_folder(name):
        folder = out / name
        folder.mkdir(parents=True, exist_ok=True)
        return folder

    try:
        start()
        state = driver.command({'cmd': 'get_state'})
        if state.get('new_db_dialog_open'):
            name = 'ChannelExportQA-' + uuid.uuid4().hex[:8]
            driver.command({'cmd': 'info_edit_input', 'action': 'new_db_name', 'text': name})
            time.sleep(.25)
            driver.command({'cmd': 'info_edit_input', 'action': 'new_db_create'})
            state = wait_state(driver, lambda s: bool(s['db_path']) and not s['new_db_dialog_open'],
                               'Isolated library must open', timeout=90)
        checks.append({'case': 'isolated-library', 'db_path': state['db_path']})

        if state['total_count'] < 1:
            driver.command({'cmd': 'scan_folder', 'path': str(fixture.parent), 'recursive': True})
            wait_state(driver, lambda s: s['total_count'] >= 1 and not s['scan_in_progress'],
                       'Fixture must land in the library', timeout=300)

        results = driver.command({'cmd': 'get_results', 'offset': 0, 'limit': 100})
        row = None
        for item in results['items']:
            if Path(item['path']).resolve() == fixture:
                row = item['idx']
                break
        assert row is not None, results
        checks.append({'case': 'fixture-row', 'row': row, 'results': results['items']})

        ok_folder = export_folder('export-ok')
        driver.command({'cmd': 'set_export_options', 'dest_folder': str(ok_folder)})
        driver.command({'cmd': 'start_channel_export', 'row': row, 'channel': 0})

        running = None
        deadline = time.monotonic() + 20
        while running is None and time.monotonic() < deadline:
            state = driver.command({'cmd': 'channel_export_state'})
            if state['jobs'] > 0:
                path = out / 'overlay-running.png'
                capture(driver, path)
                running = {'state': state, 'png': path.name}
            else:
                time.sleep(.05)
        assert running is not None, 'Running overlay was never observed'
        checks.append({'case': 'overlay-running', **running})

        state = wait_state(driver, lambda s: driver.command({'cmd': 'channel_export_state'})['jobs'] == 0,
                           'Export must finish', timeout=180)
        done = driver.command({'cmd': 'channel_export_state'})
        capture(driver, out / 'overlay-succeeded.png')
        checks.append({'case': 'overlay-succeeded', 'state': done,
                       'png': 'overlay-succeeded.png',
                       'exported_files': sorted(p.name for p in ok_folder.iterdir())})
        report['export_ok_count'] = len(list(ok_folder.iterdir()))

        blocked = out / 'blocked-target'
        blocked.write_text('not a folder', encoding='utf-8')
        driver.command({'cmd': 'set_export_options', 'dest_folder': str(blocked)})
        driver.command({'cmd': 'start_channel_export', 'row': row, 'channel': 1})
        deadline = time.monotonic() + 60
        failed = None
        while time.monotonic() < deadline:
            state = driver.command({'cmd': 'channel_export_state'})
            if state['jobs'] > 0 and failed is None:
                failed = 'started'
            if state['jobs'] == 0 and state['last_error']:
                break
            time.sleep(.05)
        state = driver.command({'cmd': 'channel_export_state'})
        assert state['jobs'] == 0 and state['last_error'], state
        time.sleep(.4)
        capture(driver, out / 'overlay-failed.png')
        checks.append({'case': 'overlay-failed', 'state': state, 'png': 'overlay-failed.png'})

        report['finished_at'] = datetime.now(timezone.utc).isoformat()
        (out / 'report.json').write_text(json.dumps(report, ensure_ascii=False, indent=2),
                                         encoding='utf-8')
        return report
    finally:
        stop()
        desktop.close()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--exe', type=Path, required=True)
    parser.add_argument('--isolation-root', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--fixture', type=Path, required=True)
    run(parser.parse_args())


if __name__ == '__main__':
    main()
