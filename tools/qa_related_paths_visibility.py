"""Verify related-path visibility in the isolated native UI, without shared input.

Run against a QA build compiled for the given isolation root. Captures both
Browser and Database sidebars with the setting shown, hidden and restarted.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import sqlite3
import subprocess
import time

from PIL import Image
import win32con
import win32gui

from qa_folder_shortcuts import Driver


def capture(driver, frame_dir, output, mode, variant):
    driver.command({'cmd': 'set_view_mode', 'mode': mode})
    time.sleep(.7)
    state = driver.command({'cmd': 'get_state'})
    if state.get('active_view') != mode:
        raise AssertionError(f'Wrong active view: {state.get("active_view")} != {mode}')
    raw = frame_dir / f'{variant}-{mode}.ppm'
    driver.command({'cmd': 'capture_preview_frame', 'path': str(raw)})
    deadline = time.monotonic() + 12
    while not raw.exists() and time.monotonic() < deadline:
        time.sleep(.1)
    if not raw.exists():
        raise TimeoutError(f'Native frame missing: {raw}')
    image = Image.open(raw).convert('RGB')
    if len(image.getcolors(image.width * image.height) or []) < 16:
        raise AssertionError('Blank native frame')
    image.save(output / f'{variant}-{mode}.png')
    image.crop((0, 55, min(480, image.width), min(500, image.height))).save(output / f'{variant}-{mode}-filter.png')
    return image


def difference_ratio(first, second):
    if first.size != second.size:
        raise AssertionError(f'Frame sizes differ: {first.size}, {second.size}')
    # Only the folder content region: ignore playback, window chrome and cursor.
    a = first.crop((60, 95, 241, 365))
    b = second.crop((60, 95, 241, 365))
    changed = sum(1 for one, two in zip(a.get_flattened_data(), b.get_flattened_data())
                  if max(abs(x - y) for x, y in zip(one, two)) >= 20)
    return changed / (a.width * a.height)


def click_sidebar_menu(driver, x, y):
    driver.command({'cmd': 'folder_search_pointer', 'phase': 'down', 'x': x, 'y': y})
    time.sleep(.15)
    driver.command({'cmd': 'folder_search_pointer', 'phase': 'up', 'x': x, 'y': y})
    time.sleep(.35)


def run(args):
    exe, root, output = (args.exe.resolve(), args.isolation_root.resolve(),
                         args.output.resolve())
    qa_root = output.parent
    if root != qa_root / 'profile' or not exe.is_relative_to(qa_root):
        raise ValueError('Executable, isolation root and output must belong to one QA directory')
    if output.exists():
        raise ValueError('Use a new output directory for each verification run')
    runtime = root / 'profile/runtime'
    if (runtime / 'qa_cmd.json').exists():
        raise ValueError('Another QA command is pending')
    runtime.mkdir(parents=True, exist_ok=True)
    settings = root / 'profile/Local/HarkenSearch/settings.ini'
    settings.parent.mkdir(parents=True, exist_ok=True)
    db = root / 'profile/Local/HarkenSearch/Database/RelatedPathsQA/main.db'
    db.parent.mkdir(parents=True, exist_ok=True)
    if not db.exists():
        sqlite3.connect(db).close()
    output.mkdir(parents=True)
    frame_dir = root / 'frames' / output.name
    frame_dir.mkdir(parents=True, exist_ok=False)
    driver = Driver(runtime, output)
    frames = {}
    identities = []

    settings.write_text('\n'.join([
        'viewMode=1', 'activeSidebarSection=1', 'folderTreePathMode=1',
        'showWelcomeOnStartup=0', 'defaultPlayerPrompted=1',
        'closeButtonAction=2', 'sidebarRelatedPathsHeight=104',
        'showRelatedPaths=1', f'currentDbPath={db}', '',
    ]), encoding='utf-8')

    for variant in ('visible', 'hidden_restart'):
        startup = subprocess.STARTUPINFO()
        startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
        startup.wShowWindow = win32con.SW_SHOWNOACTIVATE
        env = dict(os.environ, AURA_HARNESS_MULTI_INSTANCE='1',
                   AURA_HARNESS_LANE_KIND='folder-search',
                   AURA_DISABLE_SINGLE_INSTANCE='1')
        process = subprocess.Popen([str(exe)], cwd=exe.parent, env=env,
                                   startupinfo=startup)
        hwnd = None
        try:
            identity = driver.command({'cmd': 'magic_eq_identity'}, timeout=45)
            if (identity['pid'] != process.pid or
                Path(identity['exe_path']).resolve() != exe or
                Path(identity['isolation_root']).resolve() != root):
                raise AssertionError('QA process identity differs from pinned build/root')
            identities.append(identity)
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
            for mode in ('browser', 'database'):
                frames[(variant, mode)] = capture(
                    driver, frame_dir, output, mode, variant)
            if variant == 'visible':
                driver.command({'cmd': 'set_view_mode', 'mode': 'browser'})
                time.sleep(.5)
                # Exercise the actual activity-bar menu, not a prewritten setting.
                click_sidebar_menu(driver, 34, 575)
                click_sidebar_menu(driver, 88, 707)
                for mode in ('browser', 'database'):
                    frames[('hidden', mode)] = capture(
                        driver, frame_dir, output, mode, 'hidden')
        finally:
            if hwnd and win32gui.IsWindow(hwnd):
                win32gui.PostMessage(hwnd, win32con.WM_CLOSE, 0, 0)
            try:
                process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                process.terminate()
                process.wait(timeout=10)
        if variant == 'visible' and 'showRelatedPaths=0' not in settings.read_text(encoding='utf-8'):
            raise AssertionError('Menu click did not persist hidden visibility')

    ratios = {mode: difference_ratio(frames[('visible', mode)],
                                     frames[('hidden', mode)])
              for mode in ('browser', 'database')}
    restart = {mode: difference_ratio(frames[('hidden', mode)],
                                      frames[('hidden_restart', mode)])
               for mode in ('browser', 'database')}
    report = {'exe': str(exe), 'sha256': hashlib.sha256(exe.read_bytes()).hexdigest(),
              'identity': identities, 'show_hide_pixel_ratio': ratios,
              'hidden_restart_pixel_ratio': restart,
              'frames': [str(p) for p in sorted(output.glob('*.png'))],
              'input': 'isolated app-owned ImGui menu pointer; no shared mouse input'}
    (output / 'report.json').write_text(json.dumps(report, ensure_ascii=False, indent=2),
                                        encoding='utf-8')
    print('show/hide visual differences:', ratios)
    print('hidden restart differences:', restart)
    for mode, ratio in ratios.items():
        if ratio < .025:
            raise AssertionError(f'FAIL: {mode} related-path visibility did not change layout ({ratio:.4f})')
    for mode, ratio in restart.items():
        if ratio > .02:
            raise AssertionError(f'FAIL: {mode} hidden layout did not survive restart ({ratio:.4f})')
    print('PASS: native menu hides related paths in both views and state survives restart')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exe', type=Path, required=True)
    parser.add_argument('--isolation-root', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    run(parser.parse_args())
