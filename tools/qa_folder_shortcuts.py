"""Exercise real shortcut UI handlers with isolated data, without desktop input.

Requires a Debug ARIS_PREVIEW_ISOLATION build pinned to --isolation-root,
Windows, Pillow and pywin32. No mouse movement, click injection or focus change.
The native screenshots still require the user's visual confirmation.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import time
import uuid
import wave


class Driver:
    def __init__(self, runtime, output):
        self.runtime, self.output = runtime, output

    def command(self, payload, timeout=25):
        request = dict(payload, request_id=uuid.uuid4().hex)
        target = self.runtime / 'qa_cmd.json'
        if target.exists():
            raise RuntimeError('Another command is pending in this isolation root')
        temporary = target.with_suffix('.tmp')
        temporary.write_text(json.dumps(request), encoding='utf-8')
        temporary.replace(target)
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            try:
                result = json.loads((self.runtime / 'qa_result.json').read_text(encoding='utf-8'))
                if result.get('request_id') == request['request_id']:
                    with (self.output / 'commands.jsonl').open('a', encoding='utf-8') as log:
                        log.write(json.dumps({'request': request, 'result': result}) + chr(10))
                    if not result.get('ok'):
                        raise RuntimeError(result)
                    return result
            except (FileNotFoundError, PermissionError, json.JSONDecodeError):
                pass
            time.sleep(.04)
        raise TimeoutError(payload)


def synthetic_audio(path):
    if not path.exists():
        with wave.open(str(path), 'wb') as audio:
            audio.setparams((2, 2, 48000, 0, 'NONE', 'not compressed'))
            audio.writeframes(bytes(48000 * 4 * 8))


def run(args):
    import win32con
    import win32gui
    from PIL import ImageGrab

    exe = args.exe.resolve()
    isolation = args.isolation_root.resolve()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    runtime = isolation / 'profile/runtime'
    runtime.mkdir(parents=True, exist_ok=True)
    settings = isolation / 'profile/Local/HarkenSearch/settings.ini'
    if settings.exists():
        raise RuntimeError('Use a fresh isolation root; existing settings are not overwritten')
    settings.parent.mkdir(parents=True, exist_ok=True)
    animal, database = isolation / 'Animal', isolation / 'DatabaseFolder'
    animal.mkdir(exist_ok=True); database.mkdir(exist_ok=True)
    synthetic_audio(animal / 'Bird.wav')
    settings.write_text(chr(10).join([
        'viewMode=1', 'activeSidebarSection=5', 'showWelcomeOnStartup=0', 'closeButtonAction=2',
        'folderShortcutCount=2', 'folderShortcut0_name=Animal',
        f'folderShortcut0_path={animal}', 'folderShortcut1_name=Database',
        f'folderShortcut1_path={database}', '',
    ]), encoding='utf-8')
    driver = Driver(runtime, output)
    evidence = []
    for restart in range(2):
        startup = subprocess.STARTUPINFO()
        startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
        startup.wShowWindow = win32con.SW_SHOWNOACTIVATE
        env = dict(os.environ, AURA_HARNESS_MULTI_INSTANCE='1',
                   AURA_HARNESS_LANE_KIND='functional', AURA_DISABLE_SINGLE_INSTANCE='1')
        process = subprocess.Popen([str(exe)], cwd=exe.parent, env=env, startupinfo=startup)
        hwnd = None
        try:
            identity = driver.command({'cmd': 'magic_eq_identity'}, timeout=45)
            assert identity['pid'] == process.pid
            assert Path(identity['exe_path']).resolve() == exe
            assert Path(identity['isolation_root']).resolve() == isolation
            hwnd = identity['hwnd']
            ready_deadline = time.monotonic() + 45
            while not driver.command({'cmd': 'get_state'}).get('main_ui_ready'):
                if time.monotonic() > ready_deadline:
                    raise TimeoutError('Main UI did not become ready')
                time.sleep(.2)
            win32gui.SetWindowPos(hwnd, 0, 40, 40, 1400, 900,
                win32con.SWP_NOZORDER | win32con.SWP_NOACTIVATE)
            win32gui.ShowWindow(hwnd, win32con.SW_SHOWNOACTIVATE)
            if not restart:
                legacy = driver.command({'cmd': 'folder_shortcut', 'open_index': 0})
                assert legacy['mode'] == 'database', legacy
                upgraded = driver.command({'cmd': 'folder_shortcut', 'bookmark_path': str(animal)})
                assert upgraded['shortcut_count'] == 2, upgraded
                repeated = driver.command({'cmd': 'folder_shortcut', 'bookmark_path': str(animal).upper()})
                assert repeated['shortcut_count'] == 2, repeated
            # Same handler invoked by the actual selectable shortcut.
            driver.command({'cmd': 'set_view_mode', 'mode': 'database'})
            opened = driver.command({'cmd': 'folder_shortcut', 'open_index': 0})
            assert opened['mode'] == 'browser', opened
            assert Path(opened['browser_path']).resolve() == animal.resolve(), opened
            deadline = time.monotonic() + 20
            while True:
                settled = driver.command({'cmd': 'folder_shortcut'})
                if settled['entries'] > 0:
                    break
                if time.monotonic() > deadline:
                    raise TimeoutError('Unindexed audio file did not appear in browser')
                time.sleep(.1)
            time.sleep(1)
            shot = output / f'browser-restart-{restart}.png'
            image = ImageGrab.grab(window=hwnd)
            assert len(image.getcolors(image.width * image.height) or []) > 16, 'Blank screenshot'
            image.save(shot)
            db = driver.command({'cmd': 'folder_shortcut', 'open_index': 1})
            assert db['mode'] == 'database', db
            time.sleep(.5)
            ImageGrab.grab(window=hwnd).save(output / f'database-restart-{restart}.png')
            evidence.append({'restart': restart, 'identity': identity,
                'browser': settled, 'database': db, 'screenshot': str(shot)})
        finally:
            if hwnd:
                if win32gui.IsWindow(hwnd):
                    win32gui.PostMessage(hwnd, win32con.WM_CLOSE, 0, 0)
            try:
                process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                # This QA process may outlive its closed native window.
                # Record that fact; persistence is checked from the saved file.
                evidence.append({'shutdown_timeout': True, 'pid': process.pid})
                process.terminate()
                process.wait(timeout=10)
        saved = settings.read_text(encoding='utf-8')
        assert 'folderShortcut0_openInBrowser=1' in saved
        assert 'folderShortcut1_openInBrowser=0' in saved
    report = {'exe': str(exe), 'sha256': hashlib.sha256(exe.read_bytes()).hexdigest(),
              'evidence': evidence, 'input': 'shared UI handlers; no OS input injection'}
    (output / 'runtime-verification.json').write_text(
        json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
    print('PASS: legacy compatibility, re-bookmark, duplicate prevention, mode routing, unindexed audio, restart persistence')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exe', type=Path, required=True)
    parser.add_argument('--isolation-root', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    run(parser.parse_args())
