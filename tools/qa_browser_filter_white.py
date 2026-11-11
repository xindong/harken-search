"""Capture Browser and Database folder filters in all five built-in themes."""
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import sqlite3
import subprocess
import time

from PIL import Image
from qa_scan_speed import EvidenceDriver, SharedLock, contains_root_literal, ensure_root_idle
from qa_transport_themes import THEMES


CARD_ROOT = Path('D:/Harken-QA/BUG-20261106-BROWSER-FILTER-WHITE').resolve()


def sample_search_field(image, bounds):
    if (not isinstance(bounds, (list, tuple)) or len(bounds) != 4 or
            not all(isinstance(v, (int, float)) and math.isfinite(v) for v in bounds)):
        raise ValueError('Missing or invalid observed search field bounds')
    left, top, right, bottom = bounds
    if not (0 <= left < right <= image.width and 0 <= top < bottom <= image.height
            and right - left >= 8 and bottom - top >= 8):
        raise ValueError('Observed search field bounds are outside the captured frame')
    point = (int((left + right) / 2), int((top + bottom) / 2))
    return image.getpixel(point), point


def run(args):
    exe, root, out = (args.exe.resolve(), args.isolation_root.resolve(),
                      args.output.resolve())
    if not exe.is_file() or not exe.is_relative_to(CARD_ROOT):
        raise ValueError('Executable must be this card isolated QA build')
    if root != CARD_ROOT / 'profile' or not out.is_relative_to(CARD_ROOT):
        raise ValueError('Profile and output must be inside this card QA directory')
    if not contains_root_literal(exe, root):
        raise ValueError('Executable lacks the exact compiled isolation root; refusing launch')
    if out.exists():
        raise ValueError('Output must be a new directory')

    runtime = root / 'profile/runtime'
    runtime.mkdir(parents=True, exist_ok=True)
    lock = SharedLock(runtime, out)
    lock.acquire()
    process_state = {'exited': True}
    try:
        if (runtime / 'qa_cmd.json').exists():
            raise ValueError('Another QA command is pending')
        if out.exists():
            raise ValueError('Output must be a new directory')
        ensure_root_idle(exe, root)
        _run_locked(exe, root, out, runtime, process_state, lock)
    finally:
        lock.release(process_exited=process_state['exited'])


def _run_locked(exe, root, out, runtime, process_state, lock):
    import win32con
    import win32gui

    settings = root / 'profile/Local/HarkenSearch/settings.ini'
    db = root / 'profile/Local/HarkenSearch/Database/BrowserColorQA/main.db'
    db.parent.mkdir(parents=True, exist_ok=True)
    if not db.exists():
        sqlite3.connect(db).close()
    if settings.exists() and 'currentDbPath=' not in settings.read_text(encoding='utf-8'):
        raise ValueError('Unrecognized QA settings profile')
    settings.parent.mkdir(parents=True, exist_ok=True)
    settings.write_text(chr(10).join([
        'viewMode=1', 'activeSidebarSection=1', 'folderTreePathMode=1',
        'showWelcomeOnStartup=0', 'defaultPlayerPrompted=1', 'closeButtonAction=2',
        f'currentDbPath={db}', '',
    ]), encoding='utf-8')
    out.mkdir(parents=True)
    frame_dir = root / 'frames' / out.name
    frame_dir.mkdir(parents=True, exist_ok=False)

    startup = subprocess.STARTUPINFO()
    startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
    startup.wShowWindow = win32con.SW_SHOWNOACTIVATE
    env = dict(os.environ, AURA_HARNESS_MULTI_INSTANCE='1',
               AURA_HARNESS_LANE_KIND='folder-search', AURA_DISABLE_SINGLE_INSTANCE='1')
    process = subprocess.Popen([str(exe)], cwd=exe.parent, env=env,
                               startupinfo=startup)
    process_state['exited'] = False
    hwnd = None
    shots = []
    try:
        driver = EvidenceDriver(runtime, out, lock)
        identity = driver.command({'cmd': 'magic_eq_identity'}, timeout=45)
        if (identity['pid'] != process.pid or Path(identity['exe_path']).resolve() != exe
                or Path(identity['isolation_root']).resolve() != root):
            raise RuntimeError('QA process identity differs from pinned executable/profile')
        hwnd = identity['hwnd']
        deadline = time.monotonic() + 45
        while not driver.command({'cmd': 'get_state'}).get('main_ui_ready'):
            if time.monotonic() > deadline:
                raise TimeoutError('Main UI did not become ready')
            time.sleep(.2)
        win32gui.SetWindowPos(hwnd, 0, 40, 40, 1400, 900,
                              win32con.SWP_NOZORDER | win32con.SWP_NOACTIVATE)
        win32gui.ShowWindow(hwnd, win32con.SW_SHOWNOACTIVATE)
        driver.command({'cmd': 'folder_search_sidebar', 'section': 1})
        for theme, name in THEMES:
            driver.command({'cmd': 'set_theme', 'theme': theme})
            for mode in ('browser', 'database'):
                driver.command({'cmd': 'set_view_mode', 'mode': mode})
                time.sleep(.4)
                state = driver.command({'cmd': 'get_state'})
                if state['theme'] != theme or state['active_view'] != mode:
                    raise RuntimeError(f'Wrong rendered theme/view: {theme}/{mode}: {state}')
                raw = frame_dir / f'filter-{theme}-{mode}.ppm'
                driver.command({'cmd': 'capture_preview_frame', 'path': str(raw)})
                deadline = time.monotonic() + 10
                while not raw.exists() and time.monotonic() < deadline:
                    time.sleep(.1)
                if not raw.exists():
                    raise TimeoutError(f'No native frame: {theme}/{mode}')
                image = Image.open(raw).convert('RGB')
                if len(image.getcolors(image.width * image.height) or []) <= 16:
                    raise RuntimeError(f'Blank native frame: {theme}/{mode}')
                bounds = state.get('qa_filter_ui_rects', {}).get('folder_search')
                search_color, sample_point = sample_search_field(image, bounds)
                path = out / f'{theme}-{mode}.png'
                image.save(path)
                crop = image.crop((0, 55, min(480, image.width), min(500, image.height)))
                crop.save(out / f'{theme}-{mode}-filter.png')
                shots.append({'theme': theme, 'name': name, 'mode': mode,
                              'path': str(path),
                              'search_field_rgb': search_color,
                              'search_field_bounds': bounds,
                              'search_sample_xy': sample_point,
                              'sha256': hashlib.sha256(path.read_bytes()).hexdigest()})
        for theme, _ in THEMES:
            browser, database = (next(s for s in shots if s['theme'] == theme
                                      and s['mode'] == mode)
                                 for mode in ('browser', 'database'))
            if browser['search_field_rgb'] != database['search_field_rgb']:
                raise RuntimeError(f'Browser/Database search field colors differ in theme {theme}')
            if theme == 2 and min(browser['search_field_rgb']) < 220:
                raise RuntimeError('Lapis Blue folder filter search field is not light')
        comparison = Image.new('RGB', (960, 445 * len(THEMES)))
        for row, (theme, _) in enumerate(THEMES):
            for col, mode in enumerate(('browser', 'database')):
                with Image.open(out / f'{theme}-{mode}-filter.png') as crop:
                    comparison.paste(crop, (480 * col, 445 * row))
        comparison.save(out / 'five-theme-comparison.png')
        report = {'exe': str(exe),
                  'exe_sha256': hashlib.sha256(exe.read_bytes()).hexdigest(),
                  'identity': identity, 'screenshots': shots,
                  'human_visual_acceptance': 'pending',
                  'input': 'native isolated UI commands; no shared mouse input'}
        (out / 'report.json').write_text(json.dumps(report, ensure_ascii=False, indent=2),
                                         encoding='utf-8')
        print(f'PASS: captured {len(shots)} real Browser/Database theme frames in {out}')
    finally:
        try:
            if hwnd and win32gui.IsWindow(hwnd):
                win32gui.PostMessage(hwnd, win32con.WM_CLOSE, 0, 0)
        finally:
            try:
                process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                process.terminate()
                process.wait(timeout=10)
            finally:
                process_state['exited'] = process.poll() is not None


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exe', type=Path, required=True)
    parser.add_argument('--isolation-root', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    run(parser.parse_args())
