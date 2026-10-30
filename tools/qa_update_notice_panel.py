"""Capture the real update notice panel without touching the desktop pointer.

Launches the pinned-isolation QA package, opens the update center through the
functional harness seam, verifies the rendered local-version record, and saves
a full-window screenshot. No mouse movement, key injection, or foreground
activation is used beyond ShowWindow(SW_SHOW).
"""
from __future__ import annotations

import argparse
import ctypes
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import time
from datetime import datetime, timezone

sys.path.insert(0, str(Path(__file__).resolve().parent))
from qa_transport_themes import Driver, window_api  # noqa: E402


def find_window(user32, pid):
    from ctypes import wintypes
    found = []
    cb = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)

    def each(hwnd, _):
        owner = wintypes.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
        if owner.value == pid and user32.IsWindowVisible(hwnd):
            found.append(hwnd)
        return True

    user32.EnumWindows(cb(None), 0)
    return found[0] if found else None


def capture_window(user32, hwnd, pid, destination):
    from ctypes import wintypes
    from PIL import ImageGrab
    actual = wintypes.DWORD()
    user32.GetWindowThreadProcessId(hwnd, ctypes.byref(actual))
    if actual.value != pid:
        raise RuntimeError('window identity changed before capture')
    image = ImageGrab.grab(window=hwnd)
    image.save(destination)
    return hashlib.sha256(destination.read_bytes()).hexdigest()


def run(args):
    exe = args.exe.resolve()
    isolation = args.isolation_root.resolve()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    runtime = isolation / 'profile' / 'runtime'
    runtime.mkdir(parents=True, exist_ok=True)
    if (runtime / 'qa_cmd.json').exists():
        raise RuntimeError('pending command in isolation root')
    notes = exe.parent / 'aris_release_notes.json'
    if not notes.is_file():
        raise FileNotFoundError(f'release notes missing next to the exe: {notes}')
    notes_data = json.loads(notes.read_bytes().decode('utf-8'))
    notes_release = (notes_data.get('releases') or [{}])[0]
    expected_version = str(notes_release.get('version', ''))
    expected_contributors = {item.get('name', '')
        for section in notes_release.get('sections', [])
        for item in section.get('items', [])}
    if not expected_version:
        raise RuntimeError('release notes have no current version')

    startup = subprocess.STARTUPINFO()
    startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
    startup.wShowWindow = 4
    process = subprocess.Popen(
        [str(exe)], cwd=exe.parent, startupinfo=startup,
        env=dict(os.environ,
                 LOCALAPPDATA=str(isolation / 'profile' / 'Local'),
                 APPDATA=str(isolation / 'profile' / 'Roaming'),
                 AURA_RUNTIME_ROOT=str(runtime),
                 TEMP=str(isolation / 'temp'), TMP=str(isolation / 'temp'),
                 AURA_HARNESS_MULTI_INSTANCE='1',
                 AURA_HARNESS_LANE_KIND='functional',
                 AURA_DISABLE_SINGLE_INSTANCE='1'))
    driver = Driver(runtime, output)
    user32 = window_api()
    results = {}
    hwnd = None
    try:
        identity = driver.command({'cmd': 'magic_eq_identity'}, timeout=60)
        if identity['pid'] != process.pid:
            raise RuntimeError('identity pid mismatch')
        if Path(identity['isolation_root']).resolve() != isolation:
            raise RuntimeError('pinned isolation root mismatch')
        hwnd = identity['hwnd']
        user32.ShowWindow(hwnd, 4)
        deadline = time.monotonic() + 90
        while time.monotonic() < deadline:
            if driver.command({'cmd': 'get_state'}).get('main_ui_ready'):
                break
            time.sleep(0.5)
        else:
            raise TimeoutError('main UI never became ready')
        if args.library:
            library = Path(args.library).resolve()
            if not library.is_file():
                raise FileNotFoundError(library)
            driver.command({'cmd': 'switch_db', 'path': str(library)})
            time.sleep(2.0)

        state = driver.command({'cmd': 'update_notice', 'action': 'open'})
        results['open'] = state
        if not state.get('visible'):
            raise RuntimeError('update notice did not open')
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            state = driver.command({'cmd': 'update_notice', 'action': 'state'})
            if state.get('rendered_version') and state.get('items', 0) > 0:
                break
            time.sleep(0.2)
        results['final'] = state
        if not state.get('rendered_version') or state.get('items', 0) < 1:
            raise RuntimeError(f'local version panel rendered no content: {state}')
        if state.get('rendered_version') != expected_version:
            raise RuntimeError(f'panel showed {state.get("rendered_version")!r}, expected {expected_version!r}')
        if 'Cindy' not in expected_contributors:
            raise RuntimeError(f'release notes contributors lack Cindy: {sorted(expected_contributors)}')
        time.sleep(0.6)
        png = output / 'update-notice-local-version.png'
        digest = capture_window(user32, hwnd, process.pid, png)
        results['screenshot'] = {'path': str(png), 'sha256': digest}
        # Scroll the body to the bottom so the contributors line is visible,
        # then capture a second frame. Wheel events are injected into ImGui
        # only; the desktop pointer is never moved.
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            state = driver.command({'cmd': 'update_notice', 'action': 'state'})
            if state.get('scroll_y', 0) + 1 >= state.get('scroll_max', 0):
                break
            driver.command({'cmd': 'update_notice_pointer', 'control': 'body',
                            'phase': 'wheel', 'amount': -8})
            time.sleep(0.15)
        state = driver.command({'cmd': 'update_notice', 'action': 'state'})
        results['scrolled'] = state
        if state.get('scroll_y', 0) + 1 < state.get('scroll_max', 0):
            raise RuntimeError(f'body did not reach the bottom: {state}')
        time.sleep(0.6)
        bottom = output / 'update-notice-contributors.png'
        bottom_digest = capture_window(user32, hwnd, process.pid, bottom)
        results['contributors_screenshot'] = {'path': str(bottom), 'sha256': bottom_digest}
        results['identity'] = {
            'exe': str(exe),
            'exe_sha256': hashlib.sha256(exe.read_bytes()).hexdigest(),
            'notes_sha256': hashlib.sha256(notes.read_bytes()).hexdigest(),
            'captured_at': datetime.now(timezone.utc).isoformat(),
        }
        (output / 'report.json').write_text(
            json.dumps(results, ensure_ascii=False, indent=2), encoding='utf-8')
        print(json.dumps(results['final'], ensure_ascii=False))
        return 0
    finally:
        if process.poll() is None:
            if hwnd:
                user32.PostMessageW(hwnd, 0x0010, 0, 0)
                try:
                    process.wait(timeout=15)
                except subprocess.TimeoutExpired:
                    process.terminate()
            else:
                process.terminate()
            try:
                process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                process.kill()


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exe', type=Path, required=True)
    parser.add_argument('--isolation-root', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--library', type=Path, default=None,
                        help='optional main.db to switch into before opening the notice')
    raise SystemExit(run(parser.parse_args()))
