"""Check that a dock split grip stays behind the Settings modal in an isolated app.

Requires the preview-isolated Debug build, pywin32, and Pillow. The app runs on
a private Windows desktop; all input goes through its isolated QA command queue.
Exit 0 means the visual regression passed, 1 means the grip leaked through the
modal, and 2 means the fixture or app could not be observed.
"""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import sys
import time

from PIL import Image

from qa_required_library_setup import PrivateDesktop, capture, wait_state
from qa_transport_themes import Driver


def visible_grip_rows(path: Path, info: dict, settings_bounds=None) -> list[int]:
    """Find two bright dot centers on the otherwise plain right-hand margin."""
    center_x = round(info['x'] + info['w'] / 2)
    gap = round(min(9, max(5, info['w'] * .018)))
    seam_y = round(info['y'] + info['h'])
    with Image.open(path) as source:
        image = source.convert('RGB')
        rows = []
        for y in range(max(0, seam_y - 50), min(image.height, seam_y + 10)):
            if settings_bounds and not (settings_bounds[0] + 20 < center_x and
                center_x + gap + 3 < settings_bounds[2] - 3 and
                settings_bounds[1] + 20 < y < settings_bounds[3] - 20):
                continue
            if center_x + gap + 5 >= image.width:
                continue
            bright = [image.getpixel((x, y)) for x in (center_x, center_x + gap)]
            plain = [image.getpixel((x, y)) for x in (center_x + 4, center_x + gap + 4)]
            if all(max(dot) - max(back) >= 40 for dot, back in zip(bright, plain)):
                rows.append(y)
        return rows


def main(args) -> int:
    import win32con
    import win32event
    import win32gui
    import win32process

    exe, root, out = args.exe.resolve(), args.isolation_root.resolve(), args.output.resolve()
    if not exe.is_file() or not (root / 'profile').is_dir():
        raise ValueError('Expected an existing preview EXE and isolated profile')
    out.mkdir(parents=True, exist_ok=False)
    runtime = root / 'profile/runtime'
    runtime.mkdir(parents=True, exist_ok=True)
    driver = Driver(runtime, out)
    desktop = PrivateDesktop()
    process = None
    report = {
        'exe': str(exe), 'exe_sha256': hashlib.sha256(exe.read_bytes()).hexdigest(),
        'isolation_root': str(root), 'started_at': datetime.now(timezone.utc).isoformat(),
        'screenshots': ['before-settings.png', 'settings-open.png', 'after-settings.png'],
    }
    result = 2
    def settled_dock():
        first = driver.command({'cmd': 'dock_state'})
        deadline = time.monotonic() + 8
        while True:
            state = driver.command({'cmd': 'dock_state'})
            if state['frame'] >= first['frame'] + 5:
                return state
            if time.monotonic() > deadline:
                raise TimeoutError('Dock state did not advance five frames')
            time.sleep(.08)

    try:
        process, pid = desktop.start(exe)
        identity = driver.command({'cmd': 'magic_eq_identity'}, timeout=60)
        if (identity['pid'] != pid or Path(identity['exe_path']).resolve() != exe or
                Path(identity['isolation_root']).resolve() != root):
            raise AssertionError(f'Wrong app or profile: {identity}')
        report['identity'] = identity
        win32gui.ShowWindow(identity['hwnd'], win32con.SW_RESTORE)
        win32gui.SetWindowPos(identity['hwnd'], 0, 40, 40, 1400, 900,
                             win32con.SWP_NOZORDER | win32con.SWP_NOACTIVATE)
        state = wait_state(driver, lambda s: s['main_ui_ready'], 'main UI', 60)
        if not state['db_path']:
            wait_state(driver, lambda s: s['new_db_dialog_open'], 'first library dialog', 15)
            driver.command({'cmd': 'info_edit_input', 'action': 'new_db_name',
                            'text': 'DockGripModalQA'})
            time.sleep(.15)
            driver.command({'cmd': 'info_edit_input', 'action': 'new_db_create'})
            state = wait_state(driver, lambda s: bool(s['db_path']) and
                               not s['new_db_dialog_open'], 'new library', 30)
        if not Path(state['db_path']).resolve().is_relative_to(root / 'profile'):
            raise AssertionError(f'Database escaped isolation: {state["db_path"]}')
        report['database'] = state['db_path']

        driver.command({'cmd': 'dock_setup'})
        first = driver.command({'cmd': 'dock_state'})
        deadline = time.monotonic() + 8
        while True:
            dock = driver.command({'cmd': 'dock_state'})
            if dock['frame'] >= first['frame'] + 4 and dock.get('panels', {}).get('info'):
                break
            if time.monotonic() > deadline:
                raise TimeoutError('Dock fixture did not settle')
            time.sleep(.08)
        info = dock['panels']['info']
        report['dock_before'] = dock
        capture(driver, out / 'before-settings.png')
        before_rows = visible_grip_rows(out / 'before-settings.png', info)
        if not before_rows:
            raise AssertionError('Fixture did not show the dock grip before opening Settings')

        # Center a 1120 px Settings window, then expand its right edge over
        # the right-hand split grip. Use only the app's isolated UI commands.
        driver.command({'cmd': 'ordinary_button_window', 'module': 'settings', 'action': 'open'})
        driver.command({'cmd': 'settings_ui', 'action': 'resize', 'width': 1120, 'height': 800})
        deadline = time.monotonic() + 8
        while True:
            settings = driver.command({'cmd': 'settings_ui'})
            if settings['open'] and settings['bounds'][2] - settings['bounds'][0] == 1120:
                break
            if time.monotonic() > deadline:
                raise TimeoutError(f'Settings did not reach 1120 px: {settings["bounds"]}')
            time.sleep(.08)
        driver.command({'cmd': 'settings_ui', 'action': 'key', 'key': 'Escape', 'down': True})
        time.sleep(.15)
        driver.command({'cmd': 'settings_ui', 'action': 'key', 'key': 'Escape', 'down': False})
        deadline = time.monotonic() + 8
        while True:
            settings = driver.command({'cmd': 'settings_ui'})
            if not settings['open']:
                break
            if time.monotonic() > deadline:
                raise TimeoutError('Settings did not close for repositioning')
            time.sleep(.08)
        driver.command({'cmd': 'ordinary_button_window', 'module': 'settings', 'action': 'open'})
        deadline = time.monotonic() + 8
        while True:
            settings = driver.command({'cmd': 'settings_ui'})
            if settings['open'] and settings['bounds'][0] == 720:
                break
            if time.monotonic() > deadline:
                raise TimeoutError(f'Settings did not center at 1120 px: {settings["bounds"]}')
            time.sleep(.08)
        driver.command({'cmd': 'settings_ui', 'action': 'resize', 'width': 2000, 'height': 800})
        deadline = time.monotonic() + 8
        while True:
            settings = driver.command({'cmd': 'settings_ui'})
            if settings['open'] and settings['bounds'][0] <= 720 and settings['bounds'][2] >= 2220:
                break
            if time.monotonic() > deadline:
                raise TimeoutError(f'Settings did not overlap dock grip: {settings["bounds"]}')
            time.sleep(.08)
        after = driver.command({'cmd': 'dock_state'})
        if after['input']['modal'] != '###HarkenSettings':
            raise AssertionError(f'Settings is not modal: {after["input"]}')
        capture(driver, out / 'settings-open.png')
        leaked_rows = visible_grip_rows(out / 'settings-open.png', info, settings['bounds'])
        report.update({'settings_bounds': settings['bounds'], 'dock_after': after,
                       'grip_rows_before': before_rows, 'grip_rows_over_modal': leaked_rows})
        original_filter_width = after['panels']['filter']['w']
        pointer = driver.command({'cmd': 'dock_pointer', 'phase': 'hover',
                                  'module': 'filter', 'part': 'column-splitter'})
        settled_dock()
        driver.command({'cmd': 'dock_pointer', 'phase': 'down',
                        'module': 'filter', 'part': 'column-splitter'})
        settled_dock()
        driver.command({'cmd': 'dock_pointer', 'phase': 'move',
                        'x': pointer['x'] + 50, 'y': pointer['y']})
        settled_dock()
        driver.command({'cmd': 'dock_pointer', 'phase': 'up',
                        'x': pointer['x'] + 50, 'y': pointer['y']})
        blocked_drag = settled_dock()
        report['filter_width_during_modal_drag'] = blocked_drag['panels']['filter']['w']

        driver.command({'cmd': 'settings_ui', 'action': 'key', 'key': 'Escape', 'down': True})
        time.sleep(.15)
        driver.command({'cmd': 'settings_ui', 'action': 'key', 'key': 'Escape', 'down': False})
        deadline = time.monotonic() + 8
        while True:
            closed = driver.command({'cmd': 'settings_ui'})
            if not closed['open']:
                break
            if time.monotonic() > deadline:
                raise TimeoutError('Settings did not close after the visual check')
            time.sleep(.08)
        capture(driver, out / 'after-settings.png')
        restored_rows = visible_grip_rows(out / 'after-settings.png', info)
        report['grip_rows_after_close'] = restored_rows

        pointer = driver.command({'cmd': 'dock_pointer', 'phase': 'hover',
                                  'module': 'filter', 'part': 'column-splitter'})
        settled_dock()
        driver.command({'cmd': 'dock_pointer', 'phase': 'down',
                        'module': 'filter', 'part': 'column-splitter'})
        settled_dock()
        driver.command({'cmd': 'dock_pointer', 'phase': 'move',
                        'x': pointer['x'] + 50, 'y': pointer['y']})
        settled_dock()
        driver.command({'cmd': 'dock_pointer', 'phase': 'up',
                        'x': pointer['x'] + 50, 'y': pointer['y']})
        unblocked_drag = settled_dock()
        report['filter_width_after_normal_drag'] = unblocked_drag['panels']['filter']['w']
        failures = []
        if leaked_rows:
            failures.append('dock grip visible over Settings')
        if blocked_drag['panels']['filter']['w'] != original_filter_width:
            failures.append('dock splitter moved behind Settings')
        if not restored_rows:
            failures.append('dock grip did not return after Settings closed')
        if unblocked_drag['panels']['filter']['w'] < original_filter_width + 40:
            failures.append('normal dock splitter drag did not work')
        result = 1 if failures else 0
        report['result'] = 'FAIL: ' + '; '.join(failures) if failures else 'PASS'
    except Exception as exc:
        report['result'] = f'ERROR: {type(exc).__name__}: {exc}'
        raise
    finally:
        if process is not None:
            try:
                if win32event.WaitForSingleObject(process, 0) == win32con.WAIT_TIMEOUT:
                    driver.command({'cmd': 'info_edit_input', 'action': 'exit'}, timeout=8)
                if win32event.WaitForSingleObject(process, 15000) != win32con.WAIT_OBJECT_0:
                    win32process.TerminateProcess(process, 1)
                    win32event.WaitForSingleObject(process, 5000)
                report['app_exit_code'] = win32process.GetExitCodeProcess(process)
            finally:
                process.Close()
        desktop.close()
        report['test_exit_code'] = result
        (out / 'report.json').write_text(json.dumps(report, ensure_ascii=False, indent=2),
                                         encoding='utf-8')
        print(json.dumps({k: report.get(k) for k in ('result', 'exe_sha256',
            'database', 'settings_bounds', 'grip_rows_before', 'grip_rows_over_modal',
            'grip_rows_after_close', 'filter_width_during_modal_drag',
            'filter_width_after_normal_drag', 'app_exit_code', 'test_exit_code')},
            ensure_ascii=False), flush=True)
    return result


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exe', type=Path, required=True)
    parser.add_argument('--isolation-root', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    try:
        sys.exit(main(parser.parse_args()))
    except Exception as error:
        print(f'QA observation failed: {error}', file=sys.stderr)
        sys.exit(2)
