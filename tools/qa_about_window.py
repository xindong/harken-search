"""Capture the real About window without touching the desktop pointer.

Launches the pinned-isolation QA package, opens the About popup through the
functional harness seam, and saves a full-window screenshot. No mouse
movement, key injection, or foreground activation beyond ShowWindow(SW_SHOW).
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


def capture_window(user32, hwnd, pid, destination):
    import ctypes
    from ctypes import wintypes
    from PIL import ImageGrab
    actual = wintypes.DWORD()
    user32.GetWindowThreadProcessId(hwnd, ctypes.byref(actual))
    if actual.value != pid:
        raise RuntimeError('window identity changed before capture')
    ImageGrab.grab(window=hwnd).save(destination)
    return hashlib.sha256(destination.read_bytes()).hexdigest()


def enum_process_windows(user32, pid):
    found = []
    CB = ctypes.WINFUNCTYPE(ctypes.wintypes.BOOL, ctypes.wintypes.HWND, ctypes.wintypes.LPARAM)

    def each(h, _):
        owner = ctypes.wintypes.DWORD()
        user32.GetWindowThreadProcessId(h, ctypes.byref(owner))
        if owner.value == pid and user32.IsWindowVisible(h):
            r = ctypes.wintypes.RECT()
            user32.GetWindowRect(h, ctypes.byref(r))
            found.append((h, r.left, r.top, r.right - r.left, r.bottom - r.top))
        return True

    user32.EnumWindows(CB(each), 0)
    return found


def run(args):
    exe = args.exe.resolve()
    isolation = args.isolation_root.resolve()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    runtime = isolation / 'profile' / 'runtime'
    runtime.mkdir(parents=True, exist_ok=True)
    if (runtime / 'qa_cmd.json').exists():
        raise RuntimeError('pending command in isolation root')

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
    hwnd = None
    results = {}
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
        results['open'] = driver.command({'cmd': 'about_window', 'action': 'open'})
        time.sleep(1.2)
        # With multi-viewport enabled the modal popup is its own OS window;
        # capture the new window (the one that is not the main frame).
        wins = enum_process_windows(ctypes.windll.user32, process.pid)
        popups = [w for w in wins if w[0] != hwnd and w[3] > 100 and w[4] > 50]
        if not popups:
            raise RuntimeError(f'about popup window did not appear: {wins}')
        popup = popups[0]
        png = output / 'about-window.png'
        results['screenshot'] = {
            'path': str(png), 'window': popup[0],
            'sha256': capture_window(ctypes.windll.user32, popup[0], process.pid, png)}
        results['identity'] = {
            'exe': str(exe),
            'exe_sha256': hashlib.sha256(exe.read_bytes()).hexdigest(),
            'captured_at': datetime.now(timezone.utc).isoformat(),
        }
        (output / 'report.json').write_text(
            json.dumps(results, ensure_ascii=False, indent=2), encoding='utf-8')
        print(json.dumps(results['screenshot'], ensure_ascii=False))
        return 0
    finally:
        if process.poll() is None and hwnd:
            user32.PostMessageW(hwnd, 0x0010, 0, 0)
            try:
                process.wait(timeout=15)
            except subprocess.TimeoutExpired:
                process.terminate()


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exe', type=Path, required=True)
    parser.add_argument('--isolation-root', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--library', type=Path, default=None)
    raise SystemExit(run(parser.parse_args()))
