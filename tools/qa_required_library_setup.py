"""Exercise first-library setup in a pinned preview EXE on a private desktop.

Requires pywin32 and Pillow. No input or desktop switch reaches the user's desktop.
The isolation profile must start without a database; all fixtures stay inside it.
"""
from __future__ import annotations

import argparse
import ctypes
from ctypes import wintypes
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import subprocess
import time
import uuid

from qa_transport_themes import Driver, THEMES


class PrivateDesktop:
    def __init__(self):
        import win32con
        self.user32 = ctypes.WinDLL('user32', use_last_error=True)
        self.user32.CreateDesktopW.argtypes = [wintypes.LPCWSTR, ctypes.c_void_p,
            ctypes.c_void_p, wintypes.DWORD, wintypes.DWORD, ctypes.c_void_p]
        self.user32.CreateDesktopW.restype = wintypes.HANDLE
        self.user32.CloseDesktop.argtypes = [wintypes.HANDLE]
        self.user32.SetThreadDesktop.argtypes = [wintypes.HANDLE]
        self.user32.GetThreadDesktop.argtypes = [wintypes.DWORD]
        self.user32.GetThreadDesktop.restype = wintypes.HANDLE
        self.original = self.user32.GetThreadDesktop(ctypes.windll.kernel32.GetCurrentThreadId())
        self.name = 'HarkenLibraryQA-' + uuid.uuid4().hex
        self.handle = self.user32.CreateDesktopW(self.name, None, None, 0,
            win32con.GENERIC_ALL, None)
        if not self.handle:
            raise ctypes.WinError(ctypes.get_last_error())
        # Bind only this helper thread; never switch the user's input desktop.
        if not self.user32.SetThreadDesktop(self.handle):
            raise ctypes.WinError(ctypes.get_last_error())

    def start(self, exe, extra=()):
        import win32con
        import win32process
        startup = win32process.STARTUPINFO()
        startup.lpDesktop = 'WinSta0\\' + self.name
        startup.dwFlags = win32con.STARTF_USESHOWWINDOW
        startup.wShowWindow = win32con.SW_HIDE
        environment = dict(os.environ, AURA_HARNESS_MULTI_INSTANCE='1',
            AURA_HARNESS_LANE_KIND='functional', AURA_DISABLE_SINGLE_INSTANCE='1')
        process, thread, pid, _ = win32process.CreateProcess(str(exe),
            subprocess.list2cmdline([str(exe), *extra]), None, None, False,
            win32con.CREATE_UNICODE_ENVIRONMENT, environment, str(exe.parent), startup)
        thread.Close()
        return process, pid

    def close(self):
        self.user32.SetThreadDesktop(self.original)
        self.user32.CloseDesktop(self.handle)


def capture(driver, path):
    from PIL import Image
    raw = driver.runtime / ('frame-' + uuid.uuid4().hex + '.ppm')
    driver.command({'cmd': 'capture_preview_frame', 'path': str(raw)})
    deadline = time.monotonic() + 15
    while time.monotonic() < deadline:
        try:
            with Image.open(raw) as image:
                image.load()
                if len(image.getcolors(image.width * image.height) or []) < 16:
                    raise RuntimeError('Rendered frame is blank')
                image.save(path)
                return
        except (OSError, ValueError):
            time.sleep(.08)
    raise TimeoutError('No complete rendered frame')


def wait_state(driver, predicate, label, timeout=30):
    deadline = time.monotonic() + timeout
    last = {}
    while time.monotonic() < deadline:
        last = driver.command({'cmd': 'get_state'})
        if predicate(last):
            return last
        time.sleep(.08)
    detail = {key: last.get(key) for key in ('main_ui_ready', 'db_path',
        'startup_needs_database_setup', 'new_db_dialog_open',
        'new_db_modal_blocks_background', 'db_switch_in_progress', 'db_switch_error')}
    raise AssertionError(f'{label}: {detail}')


def run(args):
    import win32con
    import win32event
    import win32process
    exe, root, out = args.exe.resolve(), args.isolation_root.resolve(), args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    runtime = root / 'profile/runtime'
    runtime.mkdir(parents=True, exist_ok=True)
    desktop = PrivateDesktop()
    process = None
    driver = Driver(runtime, out)
    checks = []
    report = {'exe': str(exe), 'exe_sha256': hashlib.sha256(exe.read_bytes()).hexdigest(),
        'started_at': datetime.now(timezone.utc).isoformat(), 'checks': checks}

    def start(extra=()):
        nonlocal process
        process, pid = desktop.start(exe, extra)
        identity = driver.command({'cmd': 'magic_eq_identity'}, timeout=60)
        assert identity['pid'] == pid and Path(identity['exe_path']).resolve() == exe, identity
        assert Path(identity['isolation_root']).resolve() == root, identity
        report['identity'] = identity
        wait_state(driver, lambda s: s['main_ui_ready'], 'Startup must finish before testing', timeout=60)
        return identity

    def stop():
        nonlocal process
        if process is None:
            return
        if win32event.WaitForSingleObject(process, 0) == win32con.WAIT_TIMEOUT:
            state = driver.command({'cmd': 'get_state'})
            if state['main_ui_ready']:
                driver.command({'cmd': 'info_edit_input', 'action': 'exit'})
            else:
                import win32gui
                win32gui.PostMessage(report['identity']['hwnd'], win32con.WM_CLOSE, 0, 0)
        assert win32event.WaitForSingleObject(process, 15000) == win32con.WAIT_OBJECT_0, 'QA process did not exit'
        assert win32process.GetExitCodeProcess(process) == 0, 'QA process failed during exit'
        process.Close()
        process = None

    try:
        identity = start()
        state = driver.command({'cmd': 'get_state'})
        assert not state['db_path'], 'Use an empty isolated profile, never a user library'
        state = wait_state(driver, lambda s: s['new_db_dialog_open'] and
            s['new_db_modal_blocks_background'], 'No-library startup must require setup', timeout=8)
        checks.append({'case': 'no-library-blocks-main-ui', 'state': state})
        layouts = []
        for theme, _ in THEMES:
            driver.command({'cmd': 'set_theme', 'theme': theme})
            time.sleep(.35)
            first = driver.command({'cmd': 'empty_state_audit'})
            deadline = time.monotonic() + 8
            while True:
                audit = driver.command({'cmd': 'empty_state_audit'})
                if audit['theme'] == theme and audit['frame'] > first['current_frame'] + 2:
                    break
                assert time.monotonic() < deadline, 'No current setup layout observation'
                time.sleep(.08)
            state = driver.command({'cmd': 'get_state'})
            bounds = state['new_db_dialog_bounds']
            assert all(abs(a - b) < 1 for a, b in zip(bounds, audit['panel'])), (bounds, audit)
            assert {item['id'] for item in audit['checks']} == {'title', 'hint', 'new', 'open'}, audit
            for item in audit['checks']:
                left, top, right, bottom = item['bounds']
                assert bounds[0] <= left < right <= bounds[2], item
                assert bounds[1] <= top < bottom <= bounds[3], item
            layouts.append({'theme': theme, 'modal': bounds, 'audit': audit})
            capture(driver, out / f'setup-theme-{theme}.png')
        checks.append({'case': 'single-setup-window-in-five-themes', 'layouts': layouts})
        driver.command({'cmd': 'info_edit_input', 'action': 'escape_down'})
        time.sleep(.15)
        driver.command({'cmd': 'info_edit_input', 'action': 'escape_up'})
        time.sleep(.15)
        state = driver.command({'cmd': 'get_state'})
        assert state['new_db_dialog_open'] and state['new_db_modal_blocks_background'], state
        checks.append({'case': 'escape-cannot-skip', 'state': state})

        for name in ('invalid/name',):
            driver.command({'cmd': 'info_edit_input', 'action': 'new_db_name', 'text': name})
            time.sleep(.15)
            driver.command({'cmd': 'info_edit_input', 'action': 'new_db_create'})
            time.sleep(.6)
            state = driver.command({'cmd': 'get_state'})
            assert not state['db_path'] and not state['db_switch_in_progress'], state
            assert state['new_db_modal_blocks_background'], state
            checks.append({'case': 'invalid-name-keeps-setup', 'state': state})
        capture(driver, out / 'invalid-name.png')

        name = 'FirstLibrary-' + uuid.uuid4().hex[:8]
        driver.command({'cmd': 'info_edit_input', 'action': 'new_db_name', 'text': name})
        time.sleep(.15)
        driver.command({'cmd': 'info_edit_input', 'action': 'new_db_create'})
        state = wait_state(driver, lambda s: bool(s['db_path']) and not s['db_switch_in_progress']
            and not s['new_db_dialog_open'], 'Created empty library must unlock main UI')
        assert state['total_count'] == 0 and not state['startup_needs_database_setup'], state
        checks.append({'case': 'create-empty-library-unlocks', 'state': state})
        capture(driver, out / 'created-empty-library.png')
        from PIL import Image, ImageStat
        dimming = []
        for theme, _ in THEMES:
            driver.command({'cmd': 'set_theme', 'theme': theme})
            time.sleep(.35)
            normal_path = out / f'unblocked-theme-{theme}.png'
            capture(driver, normal_path)
            # The same left filter region is behind the modal in every theme.
            region = (24, 140, 150, 240)
            with Image.open(out / f'setup-theme-{theme}.png') as modal, Image.open(normal_path) as normal:
                dim = ImageStat.Stat(modal.crop(region).convert('L')).mean[0]
                bright = ImageStat.Stat(normal.crop(region).convert('L')).mean[0]
            assert dim < bright * .75, f'Theme {theme}: background did not dim ({dim} / {bright})'
            dimming.append({'theme': theme, 'modal_luminance': dim, 'normal_luminance': bright})
        checks.append({'case': 'background-dims-and-restores-in-five-themes', 'themes': dimming})
        stop()

        identity = start()
        state = wait_state(driver, lambda s: bool(s['db_path']) and not s['db_switch_in_progress'],
            'Existing empty library must reopen')
        assert state['total_count'] == 0 and not state['new_db_dialog_open'], state
        checks.append({'case': 'existing-empty-library-not-blocked', 'state': state})
        driver.command({'cmd': 'info_edit_input', 'action': 'new_db_dialog'})
        wait_state(driver, lambda s: s['new_db_dialog_open'], 'Manual create dialog must open')
        driver.command({'cmd': 'info_edit_input', 'action': 'escape_down'})
        time.sleep(.15)
        driver.command({'cmd': 'info_edit_input', 'action': 'escape_up'})
        state = wait_state(driver, lambda s: not s['new_db_dialog_open'], 'Manual dialog remains dismissible')
        checks.append({'case': 'existing-library-manual-dialog-esc', 'state': state})
        library = Path(state['db_path']).resolve()
        stop()

        profile = (root / 'profile').resolve()
        preserved = (root / ('profile-created-' + uuid.uuid4().hex[:8])).resolve()
        assert profile.parent == root and preserved.parent == root and not preserved.exists()
        relative_library = library.relative_to(profile)
        profile.rename(preserved)
        runtime.mkdir(parents=True)
        identity = start()
        wait_state(driver, lambda s: s['new_db_modal_blocks_background'], 'Fresh profile requires setup')
        broken = root / ('broken-library-' + uuid.uuid4().hex[:8]) / 'main.db'
        broken.parent.mkdir()
        broken.write_bytes(b'not a SQLite database')
        driver.command({'cmd': 'info_edit_input', 'action': 'open_existing_db', 'text': str(broken)})
        state = wait_state(driver, lambda s: not s['db_switch_in_progress'] and bool(s['db_switch_notice']),
            'Failed open must report its error')
        assert not state['db_path'] and state['new_db_modal_blocks_background'], state
        checks.append({'case': 'failed-open-keeps-setup', 'state': state})
        capture(driver, out / 'failed-open.png')
        driver.command({'cmd': 'info_edit_input', 'action': 'open_existing_db',
            'text': str(preserved / relative_library)})
        state = wait_state(driver, lambda s: bool(s['db_path']) and not s['db_switch_in_progress']
            and not s['new_db_dialog_open'], 'Opening an existing library must unlock main UI')
        assert state['total_count'] == 0, state
        checks.append({'case': 'open-existing-empty-library-unlocks', 'state': state})
        stop()
        report['status'] = 'passed'
    except Exception as error:
        report['status'] = 'failed'
        report['error'] = str(error)
        raise
    finally:
        try:
            stop()
        finally:
            (out / 'report.json').write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
            desktop.close()
    print(json.dumps({'status': report['status'], 'checks': len(checks), 'report': str(out / 'report.json')}))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    for option in ('exe', 'isolation-root', 'output'):
        parser.add_argument('--' + option, type=Path, required=True)
    run(parser.parse_args())
