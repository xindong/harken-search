"""Exercise result-tab sorting and blank-shelf undocking in the actual QA app.

All input stays inside the app on a private desktop. No user pointer or focus
is changed. Requires an isolated diagnostic package, Pillow and pywin32.
"""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import time

from qa_dock_grip_modal import shutdown_preview
from qa_required_library_setup import PrivateDesktop, capture, wait_state
from qa_scan_dialog_simple import Smoke as ScanUi
from qa_transport_themes import Driver


def run(args):
    import win32con
    import win32event
    import win32process

    exe, root, out = args.exe.resolve(), args.isolation_root.resolve(), args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    runtime = root / 'profile/runtime'
    runtime.mkdir(parents=True, exist_ok=True)
    driver = Driver(runtime, out)
    desktop = PrivateDesktop()
    process = None
    verified = False
    report = dict(exe=str(exe), exe_sha256=hashlib.sha256(exe.read_bytes()).hexdigest(),
                  started_at=datetime.now(timezone.utc).isoformat(), cases=[])

    def command(**payload):
        return driver.command(payload)

    def settled(predicate=lambda s: True, timeout=10):
        first = command(cmd='dock_state')
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            snap = command(cmd='dock_state')
            if snap['frame'] >= first['frame'] + 5 and predicate(snap):
                return snap
            time.sleep(.06)
        raise AssertionError(snap)

    def record(name, snap):
        screenshot = out / (name + '.png')
        capture(driver, screenshot)
        report['cases'].append(dict(name=name, state=snap, screenshot=str(screenshot)))
        print('PASS:', name, flush=True)

    def press(part):
        before = settled()
        assert not before['input']['modal'], ('A modal blocks the gesture under test', before)
        command(cmd='dock_pointer', phase='hover', module='search-results', part=part)
        settled()
        command(cmd='dock_pointer', phase='down', module='search-results', part=part)
        time.sleep(.65)
        return settled()

    def release():
        command(cmd='dock_pointer', phase='up')
        return settled()

    try:
        process, pid = desktop.start(exe)
        identity = command(cmd='magic_eq_identity')
        assert identity['pid'] == pid and Path(identity['exe_path']).resolve() == exe, identity
        assert Path(identity['isolation_root']).resolve() == root, identity
        verified = True
        report['identity'] = identity
        state = wait_state(driver, lambda s: s['main_ui_ready'], 'main UI', 60)
        if not state['db_path']:
            wait_state(driver, lambda s: s['new_db_dialog_open'], 'first library dialog')
            command(cmd='info_edit_input', action='new_db_name', text='ResultsTabDragQA')
            command(cmd='info_edit_input', action='new_db_create')
            wait_state(driver, lambda s: bool(s['db_path']) and not s['new_db_dialog_open'],
                       'isolated library')
        if args.scan_dir:
            audio = args.scan_dir.resolve()
            assert audio.is_dir() and audio.is_relative_to(root), 'Audio must be inside the isolated root'
            command(cmd='scan_folder', path=str(audio), cache_waveforms=True,
                    build_similarity_index=False)
            deadline = time.monotonic() + 60
            while True:
                status = command(cmd='scan_status')
                if not status['scanning'] and not status.get('scan_completion_pending', False):
                    break
                assert time.monotonic() < deadline, status
                time.sleep(.1)
            assert status['total'] > 0 and status['errors'] == 0, status
            scan_ui = ScanUi(driver, root, out, 30)
            scan_ui.wait(lambda s: scan_ui.target(s, 'scan.result.done', '##ScanResult') is not None,
                         'scan result button')
            scan_ui.click('scan.result.done', '##ScanResult')
            scan_ui.wait(lambda s: s['surface'] == 'setup', 'scan result dismissed')
            report['scan'] = status
        state = command(cmd='get_state')
        for _ in range(max(0, 3 - state['tabs']['count'])):
            command(cmd='new_tab')
        command(cmd='dock_setup')
        snap = settled()
        tabs = snap.get('results_tabs', [])
        # Older builds registered the final tab itself as the panel title. This
        # fallback reproduces the original bug without adding a new input path.
        tab_part = 'tab:' + str(tabs[-1]['id']) if tabs else 'title'
        held = press(tab_part)
        assert not held['panels']['search-results']['floating'], (
            'Holding a result tab must not float the results panel', held)
        assert held['owner'] != 'search-results', held
        record('01-hold-tab-stays-docked', held)
        assert len(tabs) >= 3, 'Need rendered tab observations to verify actual sorting'
        moved_id = tabs[-1]['id']
        target = tabs[0]['rect']
        command(cmd='dock_pointer', phase='move',
                x=target['x'] + 3, y=target['y'] + target['h'] / 2)
        snap = settled()
        snap = release()
        assert [t['id'] for t in snap['results_tabs']][0] == moved_id, snap
        assert snap['active_tab_id'] == moved_id, snap
        assert not snap['panels']['search-results']['floating'], snap
        record('02-tab-reordered-selection-preserved', snap)
        order = [t['id'] for t in snap['results_tabs']]
        other_id = order[1]
        press('tab:' + str(other_id))
        snap = release()
        assert snap['active_tab_id'] == other_id, snap
        assert [t['id'] for t in snap['results_tabs']] == order, snap
        press('tab:' + str(moved_id))
        snap = release()
        assert snap['active_tab_id'] == moved_id, snap
        record('02b-switch-tabs-keeps-order', snap)
        command(cmd='dock_pointer', phase='hover', module='search-results', part='blank')
        settled()
        command(cmd='dock_pointer', phase='down', module='search-results', part='blank')
        time.sleep(.08)
        snap = release()
        assert not snap['panels']['search-results']['floating'], snap
        record('02c-short-blank-click-stays-docked', snap)
        held = press('blank')
        assert held['panels']['search-results']['floating'], held
        assert [t['id'] for t in held['results_tabs']] == order, held
        record('03-hold-blank-floats-panel', held)
        command(cmd='dock_pointer', phase='move',
                x=held['input']['x'] + 100, y=held['input']['y'] + 60)
        moved = settled()
        assert moved['panels']['search-results']['floating'], moved
        assert moved['panels']['search-results']['y'] > held['panels']['search-results']['y'] + 20, moved
        assert [t['id'] for t in moved['results_tabs']] == order, moved
        record('03b-blank-drag-moves-floating-panel', moved)
        command(cmd='dock_pointer', phase='escape')
        snap = settled(lambda s: not s['panels']['search-results']['floating'])
        release()
        assert [t['id'] for t in snap['results_tabs']] == order, snap
        record('04-escape-restores-panel-and-order', snap)
        # Persisted settings must describe the displayed order and selection.
        command(cmd='info_edit_input', action='exit')
        assert win32event.WaitForSingleObject(process, 15000) == win32con.WAIT_OBJECT_0
        assert win32process.GetExitCodeProcess(process) == 0
        process.Close()
        process = None
        process, pid = desktop.start(exe)
        identity = command(cmd='magic_eq_identity')
        assert identity['pid'] == pid and Path(identity['exe_path']).resolve() == exe, identity
        assert Path(identity['isolation_root']).resolve() == root, identity
        wait_state(driver, lambda s: s['main_ui_ready'], 'reopened main UI', 60)
        snap = settled(lambda s: len(s.get('results_tabs', [])) >= len(order))
        assert [t['id'] for t in snap['results_tabs']] == order, snap
        assert snap['active_tab_id'] == moved_id, snap
        record('05-restart-keeps-order-and-selection', snap)
        report['result'] = 'passed'
        return 0
    except Exception as error:
        report['result'] = 'failed'
        report['error'] = repr(error)
        if verified:
            try:
                report['failure_state'] = command(cmd='dock_state')
                capture(driver, out / 'failure.png')
            except Exception as capture_error:
                report['capture_error'] = repr(capture_error)
        print(type(error).__name__ + ': ' + str(error), flush=True)
        return 1
    finally:
        if process is not None:
            report['exit_code'], report['shutdown_error'] = shutdown_preview(
                process, driver, verified, win32con, win32event, win32process)
        desktop.close()
        (out / 'report.json').write_text(json.dumps(report, ensure_ascii=False, indent=2),
                                       encoding='utf-8')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exe', type=Path, required=True)
    parser.add_argument('--isolation-root', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--scan-dir', type=Path)
    raise SystemExit(run(parser.parse_args()))
