#!/usr/bin/env python3
"""Capture the real AI speech panel from a pinned-isolation QA build.

The panel is opened through the app's own QA seam (similar_index open with
show_speech), then captured window-by-window. Clicks are posted to the panel
HWND only; the desktop pointer is never moved. Every fixture stays inside the
compile-time pinned isolation root.

Exit 0: captures produced and identity matched. 1: assertion/state failure.
2: capture or startup failure.
"""
from __future__ import annotations

import argparse
import ctypes
from ctypes import wintypes
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import time
import uuid
from datetime import datetime, timezone

sys.path.insert(0, str(Path(__file__).resolve().parent))
from qa_transport_themes import Driver, synthetic_audio, window_api  # noqa: E402

PANEL_TITLE = "AI 语音识别"


def process_windows(user32, pid):
    found = []
    CB = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)

    def each(h, _):
        owner = wintypes.DWORD()
        user32.GetWindowThreadProcessId(h, ctypes.byref(owner))
        if owner.value == pid and user32.IsWindowVisible(h):
            r = wintypes.RECT()
            user32.GetWindowRect(h, ctypes.byref(r))
            length = user32.GetWindowTextLengthW(h)
            buf = ctypes.create_unicode_buffer(length + 1)
            user32.GetWindowTextW(h, buf, length + 1)
            found.append({'hwnd': h, 'title': buf.value, 'x': r.left, 'y': r.top,
                          'w': r.right - r.left, 'h': r.bottom - r.top})
        return True

    user32.EnumWindows(CB(each), 0)
    return found


def grab(hwnd, destination):
    from PIL import ImageGrab
    ImageGrab.grab(window=hwnd).save(destination)
    return hashlib.sha256(destination.read_bytes()).hexdigest()


def post_click(user32, hwnd, x, y):
    """Click at client coordinates with posted messages; no cursor movement."""
    lp = (int(y) << 16) | (int(x) & 0xFFFF)
    user32.PostMessageW(hwnd, 0x0200, 0, lp)   # WM_MOUSEMOVE
    time.sleep(0.08)
    user32.PostMessageW(hwnd, 0x0201, 1, lp)   # WM_LBUTTONDOWN
    time.sleep(0.08)
    user32.PostMessageW(hwnd, 0x0202, 0, lp)   # WM_LBUTTONUP
    time.sleep(0.35)


def wait_state(driver, predicate, label, timeout=60):
    deadline = time.monotonic() + timeout
    last = {}
    while time.monotonic() < deadline:
        last = driver.command({'cmd': 'get_state'})
        if predicate(last):
            return last
        time.sleep(0.2)
    raise AssertionError(f'{label}: {json.dumps({k: last.get(k) for k in ("main_ui_ready", "db_path", "startup_needs_database_setup", "new_db_dialog_open", "db_switch_in_progress", "total_count")}, ensure_ascii=False)}')


def run(args):
    exe = args.exe.resolve()
    isolation = args.isolation_root.resolve()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    runtime = isolation / 'profile' / 'runtime'
    runtime.mkdir(parents=True, exist_ok=True)
    for stale in ('qa_cmd.json', 'qa_result.json'):
        (runtime / stale).unlink(missing_ok=True)
    fixtures = isolation / 'fixtures' / 'audio'
    fixtures.mkdir(parents=True, exist_ok=True)
    for index in range(3):
        synthetic_audio(fixtures / f'ai-panel-qa-{index + 1}.wav')

    user32 = ctypes.WinDLL('user32', use_last_error=True)
    user32.PostMessageW.argtypes = [wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM]
    user32.GetWindowTextW.argtypes = [wintypes.HWND, wintypes.LPWSTR, ctypes.c_int]
    user32.GetWindowTextLengthW.argtypes = [wintypes.HWND]
    user32.IsWindowVisible.argtypes = [wintypes.HWND]
    window_api()  # DPI awareness so clicks use client pixels

    environment = dict(os.environ, AURA_HARNESS_MULTI_INSTANCE='1',
                       AURA_HARNESS_LANE_KIND='functional', AURA_DISABLE_SINGLE_INSTANCE='1')
    process = subprocess.Popen([str(exe)], cwd=str(exe.parent), env=environment)
    driver = Driver(runtime, output)
    report = {'exe': str(exe), 'exe_sha256': hashlib.sha256(exe.read_bytes()).hexdigest(),
              'isolation_root': str(isolation), 'started_at': datetime.now(timezone.utc).isoformat(),
              'captures': [], 'checks': []}
    try:
        identity = driver.command({'cmd': 'magic_eq_identity'}, timeout=60)
        assert identity['pid'] == process.pid, identity
        assert Path(identity['exe_path']).resolve() == exe, identity
        assert Path(identity['isolation_root']).resolve() == isolation, identity
        report['identity'] = identity
        hwnd = identity['hwnd']
        user32.ShowWindow(hwnd, 4)
        user32.SetWindowPos(hwnd, None, 20, 30, 1280, 900, 0x0040 | 0x0010 | 0x0004)
        wait_state(driver, lambda s: s['main_ui_ready'], 'Main UI must finish starting')

        db_path = ''
        state = driver.command({'cmd': 'get_state'})
        if not state['db_path'] and state['startup_needs_database_setup']:
            name = 'AI-Panel-QA-' + uuid.uuid4().hex[:8]
            driver.command({'cmd': 'info_edit_input', 'action': 'new_db_name', 'text': name})
            time.sleep(0.15)
            driver.command({'cmd': 'info_edit_input', 'action': 'new_db_create'})
            state = wait_state(driver, lambda s: bool(s['db_path']) and not s['db_switch_in_progress']
                               and not s['new_db_dialog_open'], 'Library must be created inside isolation')
        db_path = state['db_path']
        assert Path(db_path).resolve().is_relative_to(isolation), db_path
        report['library'] = {'path': db_path, 'total_count': state['total_count']}

        if state['total_count'] < 1:
            driver.command({'cmd': 'scan_folder', 'path': str(fixtures), 'recursive': True})
            state = wait_state(driver, lambda s: s['total_count'] >= 3, 'Fixture library must scan', timeout=180)
        report['library']['total_count'] = state['total_count']
        report['checks'].append({'case': 'isolated-library-ready',
                                 'db_path': db_path, 'total_count': state['total_count']})

        opened = driver.command({'cmd': 'similar_index', 'action': 'open', 'show_speech': True,
                                 'width': 720, 'height': 460})
        report['checks'].append({'case': 'open-panel', 'visible': opened.get('visible')})
        time.sleep(0.6)
        driver.command({'cmd': 'similar_index', 'action': 'close'})
        time.sleep(0.6)

        if args.load_file:
            driver.command({'cmd': 'load_path', 'path': str(args.load_file.resolve())})
            time.sleep(1.2)
            loaded = driver.command({'cmd': 'get_state'})
            report['checks'].append({'case': 'load-audio',
                                     'audio_loaded': loaded.get('audio_loaded'),
                                     'audio_file': loaded.get('current_file')})

        def capture(label):
            target = output / f'{label}.png'
            digest = grab(hwnd, target)
            report['captures'].append({'label': label, 'path': str(target), 'sha256': digest})
            return target

        capture('panel-transcribe')
        for step in args.click:
            x, y = (int(part) for part in step.split(','))
            post_click(user32, hwnd, x, y)
            capture(f'panel-click-{x}-{y}')
        return report
    finally:
        try:
            state = driver.command({'cmd': 'get_state'}, timeout=10)
            if state.get('main_ui_ready'):
                driver.command({'cmd': 'info_edit_input', 'action': 'exit'}, timeout=10)
        except Exception:
            pass
        try:
            process.terminate()
            process.wait(timeout=15)
        except Exception:
            pass


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--exe', required=True, type=Path)
    parser.add_argument('--isolation-root', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--load-file', type=Path, default=None)
    parser.add_argument('--click', action='append', default=[],
                        help='client coordinate "x,y" inside the AI panel; repeatable')
    args = parser.parse_args()
    try:
        report = run(args)
    except Exception as error:  # noqa: BLE001
        print(json.dumps({'ok': False, 'error': f'{type(error).__name__}: {error}'}, ensure_ascii=False))
        return 2
    report['ok'] = True
    (args.output / 'evidence.json').write_text(
        json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
    print(json.dumps({'ok': True, 'captures': [c['path'] for c in report['captures']]}, ensure_ascii=False))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
