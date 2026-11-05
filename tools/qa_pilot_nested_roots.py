"""Real UI evidence for nested Pilot roots, without OS input injection."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import sqlite3
import subprocess
import time
import uuid

from PIL import Image
import win32con
import win32gui
from qa_folder_shortcuts import Driver, synthetic_audio
from qa_scan_speed import contains_root_literal


def scan(driver, folder):
    started = driver.command({'cmd': 'scan_folder', 'path': str(folder),
                              'recursive': True, 'scan_strategy': 'fast',
                              'build_similarity_index': False})
    assert started['started'], started
    deadline = time.monotonic() + 90
    while time.monotonic() < deadline:
        state = driver.command({'cmd': 'scan_status'})
        if not state['scanning'] and not state['scan_completion_pending']:
            assert state['errors'] == 0, state
            deadline = time.monotonic() + 15
            while time.monotonic() < deadline:
                ui = driver.command({'cmd': 'scan_dialog_ui', 'action': 'state'})
                if any(item['id'] == 'scan.result.done' for item in ui['items']):
                    break
                time.sleep(.1)
            else:
                raise TimeoutError('Scan result Done button did not appear')
            for phase in ('hover', 'down', 'up'):
                ui = driver.command({'cmd': 'scan_dialog_ui', 'action': 'state'})
                item = next(item for item in ui['items'] if item['id'] == 'scan.result.done')
                left, top, right, bottom = item['bounds']
                driver.command({'cmd': 'scan_dialog_ui', 'action': 'pointer',
                                'phase': phase, 'x': (left + right) / 2,
                                'y': (top + bottom) / 2})
                time.sleep(.08)
            return state
        time.sleep(.2)
    raise TimeoutError(folder)


def run(args):
    exe, isolation, output = (args.exe.resolve(), args.isolation_root.resolve(),
                              args.output.resolve())
    assert exe.is_file()
    if not contains_root_literal(exe, isolation):
        raise ValueError('Executable lacks the exact compiled isolation root; refusing launch')
    output.mkdir(parents=True, exist_ok=True)
    runtime = isolation / 'profile/runtime'
    runtime.mkdir(parents=True, exist_ok=True)
    settings = isolation / 'profile/Local/HarkenSearch/settings.ini'
    if settings.exists():
        assert 'folderTreePathMode=1' in settings.read_text(encoding='utf-8'), (
            'Refusing to replace an unrelated app profile')
    settings.parent.mkdir(parents=True, exist_ok=True)
    db = isolation / 'profile/Local/HarkenSearch/Database/PilotQA/main.db'
    db.parent.mkdir(parents=True, exist_ok=True)
    if not db.exists():
        sqlite3.connect(db).close()
    settings.write_text(chr(10).join([
        'viewMode=1', 'activeSidebarSection=1', 'folderTreePathMode=1',
        'showWelcomeOnStartup=0', 'closeButtonAction=2',
        f'currentDbPath={db}', '',
    ]), encoding='utf-8')
    parent = isolation / '综合'
    child = parent / '8082'
    child.mkdir(parents=True, exist_ok=True)
    synthetic_audio(parent / 'root.wav')
    synthetic_audio(child / 'inside.wav')

    startup = subprocess.STARTUPINFO()
    startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
    startup.wShowWindow = win32con.SW_SHOWNOACTIVATE
    env = dict(os.environ, AURA_HARNESS_MULTI_INSTANCE='1',
               AURA_HARNESS_LANE_KIND='folder-search', AURA_DISABLE_SINGLE_INSTANCE='1')
    process = subprocess.Popen([str(exe)], cwd=exe.parent, env=env,
                               startupinfo=startup)
    hwnd = None
    driver = Driver(runtime, output)
    try:
        identity = driver.command({'cmd': 'magic_eq_identity'}, timeout=45)
        assert identity['pid'] == process.pid and Path(identity['exe_path']).resolve() == exe
        assert Path(identity['isolation_root']).resolve() == isolation
        hwnd = identity['hwnd']
        deadline = time.monotonic() + 45
        while not driver.command({'cmd': 'get_state'}).get('main_ui_ready'):
            if time.monotonic() > deadline:
                raise TimeoutError('Main UI did not become ready')
            time.sleep(.2)
        driver.command({'cmd': 'set_theme', 'theme': 2})
        first = scan(driver, parent)
        second = scan(driver, child)
        driver.command({'cmd': 'set_view_mode', 'mode': 'database'})
        driver.command({'cmd': 'clear_folder_filter'})
        win32gui.SetWindowPos(hwnd, 0, 40, 40, 1400, 900,
                              win32con.SWP_NOZORDER | win32con.SWP_NOACTIVATE)
        win32gui.ShowWindow(hwnd, win32con.SW_SHOWNOACTIVATE)
        time.sleep(2)
        for phase in ('down', 'up'):
            driver.command({'cmd': 'folder_search_pointer', 'phase': phase,
                            'x': 84, 'y': 286})
            time.sleep(.1)
        for phase in ('down', 'up'):
            driver.command({'cmd': 'folder_search_pointer', 'phase': phase,
                            'x': 180, 'y': 500})
            time.sleep(.1)
        state = driver.command({'cmd': 'get_state'})
        assert state['total_count'] == 2, state
        frame = isolation / ('pilot-nested-roots-frame-' + uuid.uuid4().hex + '.ppm')
        driver.command({'cmd': 'capture_preview_frame', 'path': str(frame)})
        deadline = time.monotonic() + 10
        while not frame.exists() and time.monotonic() < deadline:
            time.sleep(.1)
        assert frame.exists(), 'App did not write its preview frame'
        image = Image.open(frame)
        assert len(image.getcolors(image.width * image.height) or []) > 16
        shot = output / 'pilot-nested-roots.png'
        image.save(shot, format='PNG')
        report = {'exe': str(exe), 'sha256': hashlib.sha256(exe.read_bytes()).hexdigest(),
                  'identity': identity, 'first_scan': first, 'second_scan': second,
                  'final_state': {'total_count': state['total_count'],
                                  'active_view': state['active_view'],
                                  'db_path': state['db_path']},
                  'screenshot': str(shot),
                  'input': 'isolated UI handlers; no OS input injection'}
        (output / 'runtime-verification.json').write_text(
            json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
        print('PASS: two nested scans and native screenshot:', shot)
    finally:
        if hwnd and win32gui.IsWindow(hwnd):
            win32gui.PostMessage(hwnd, win32con.WM_CLOSE, 0, 0)
        try:
            process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            process.terminate()
            process.wait(timeout=10)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exe', type=Path, required=True)
    parser.add_argument('--isolation-root', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    run(parser.parse_args())
