"""首次建库弹窗按钮：多档界面倍率下的真实高度、分行与截图。

在固定隔离根上以私有桌面启动 QA 构建，用 empty_state_audit 取三个按钮的真实
矩形，并把窗口截图存下来供肉眼复核。不改用户数据、不切输入桌面。

用法：
  python tools/qa_setup_button_scale.py --exe build_qa/Debug/HarkenSearch.exe \
      --isolation-root preview-root --output D:/Harken-QA/<卡号>/scale \
      --dpi 2.0 --scales 100,125,150 --narrow-scale 125 --narrow-client-width 700
"""
from __future__ import annotations

import argparse
import hashlib
import json
import shutil
import time
import uuid
from datetime import datetime, timezone
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))

from qa_required_library_setup import PrivateDesktop, capture, wait_state  # noqa: E402
from qa_transport_themes import Driver  # noqa: E402

THEME = 0
READING_FONT = 14
LIST_FONT = 13


def write_settings(runtime: Path, scale_percent: int) -> None:
    runtime.mkdir(parents=True, exist_ok=True)
    (runtime / 'settings.ini').write_text(
        '\n'.join([
            f'uiScalePercent={scale_percent}',
            f'readingFontSize={READING_FONT}',
            f'listFontSize={LIST_FONT}',
            f'theme={THEME}',
            'language=0',
            '',
        ]),
        encoding='utf-8',
    )


def reset_profile(root: Path, scale_percent: int) -> Path:
    profile = root / 'profile'
    if profile.exists():
        shutil.rmtree(profile)
    runtime = profile / 'runtime'
    runtime.mkdir(parents=True)
    (profile / 'Roaming').mkdir(parents=True, exist_ok=True)
    settings_dir = profile / 'Local' / 'HarkenSearch'
    settings_dir.mkdir(parents=True, exist_ok=True)
    (profile / 'Temp').mkdir(parents=True, exist_ok=True)
    # The app keeps settings.ini beside its canonical root under LOCALAPPDATA,
    # not in the runtime directory the QA command channel uses.
    write_settings(settings_dir, scale_percent)
    return runtime


def audit_buttons(driver: Driver) -> dict:
    first = driver.command({'cmd': 'empty_state_audit'})
    deadline = time.monotonic() + 20
    while time.monotonic() < deadline:
        audit = driver.command({'cmd': 'empty_state_audit'})
        if audit.get('checks') and audit.get('frame', 0) > first.get('current_frame', 0) + 2:
            break
        time.sleep(.08)
    else:
        raise TimeoutError('No current setup layout observation')
    buttons = {}
    for item in audit['checks']:
        left, top, right, bottom = item['bounds']
        buttons[item['id']] = {
            'left': left, 'top': top, 'right': right, 'bottom': bottom,
            'width': right - left, 'height': bottom - top,
        }
    return {'panel': audit['panel'], 'buttons': buttons, 'frame': audit['frame']}


def resize_client(hwnd: int, client_width: int) -> None:
    import win32con
    import win32gui
    left, top, right, bottom = win32gui.GetWindowRect(hwnd)
    client = win32gui.GetClientRect(hwnd)
    frame_w = (right - left) - client[2]
    frame_h = (bottom - top) - client[3]
    win32gui.SetWindowPos(hwnd, 0, left, top, client_width + frame_w,
                          bottom - top, win32con.SWP_NOZORDER | win32con.SWP_NOACTIVATE)


def run(args) -> dict:
    exe = args.exe.resolve()
    root = args.isolation_root.resolve()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    scales = [int(value) for value in args.scales.split(',') if value]
    report = {
        'exe': str(exe),
        'exe_sha256': hashlib.sha256(exe.read_bytes()).hexdigest(),
        'dpi_override': args.dpi,
        'started_at': datetime.now(timezone.utc).isoformat(),
        'runs': [],
    }

    for scale in scales:
        runtime = reset_profile(root, scale)
        desktop = PrivateDesktop()
        process = None
        try:
            process, pid = desktop.start(exe, (), {'HARKEN_QA_DPI_SCALE': args.dpi})
            driver = Driver(runtime, out)
            identity = driver.command({'cmd': 'magic_eq_identity'}, timeout=60)
            assert identity['pid'] == pid, identity
            wait_state(driver, lambda s: s['main_ui_ready'], f'scale {scale}: UI ready', timeout=90)
            state = wait_state(driver, lambda s: s['new_db_dialog_open'],
                               f'scale {scale}: setup dialog', timeout=30)
            time.sleep(.4)
            audit = audit_buttons(driver)
            shot = out / f'setup-scale-{scale}.png'
            capture(driver, shot)
            buttons = {name: audit['buttons'][name] for name in ('new', 'open', 'exit')}
            heights = {name: round(item['height'], 1) for name, item in buttons.items()}
            same_row = len({item['top'] for item in buttons.values()}) == 1
            report['runs'].append({
                'case': 'setup-dialog', 'scale': scale, 'screenshot': str(shot),
                'panel': audit['panel'], 'buttons': buttons,
                'heights': heights, 'same_row': same_row,
                'client': list(state.get('main_client_size') or []),
            })
            driver.command({'cmd': 'info_edit_input', 'action': 'exit'})
        finally:
            if process is not None:
                try:
                    import win32event
                    win32event.WaitForSingleObject(process, 15000)
                except Exception:
                    pass
            desktop.close()

    narrow = args.narrow_scale
    if narrow:
        runtime = reset_profile(root, narrow)
        desktop = PrivateDesktop()
        process = None
        try:
            process, pid = desktop.start(exe, (), {'HARKEN_QA_DPI_SCALE': args.dpi})
            driver = Driver(runtime, out)
            identity = driver.command({'cmd': 'magic_eq_identity'}, timeout=60)
            assert identity['pid'] == pid, identity
            wait_state(driver, lambda s: s['main_ui_ready'], 'narrow: UI ready', timeout=90)
            wait_state(driver, lambda s: s['new_db_dialog_open'], 'narrow: setup dialog', timeout=30)
            resize_client(identity['hwnd'], args.narrow_client_width)
            time.sleep(.6)
            audit = audit_buttons(driver)
            shot = out / f'setup-narrow-{narrow}-{args.narrow_client_width}.png'
            capture(driver, shot)
            buttons = {name: audit['buttons'][name] for name in ('new', 'open', 'exit')}
            rows = sorted({round(item['top'], 1) for item in buttons.values()})
            report['runs'].append({
                'case': 'setup-dialog-narrow', 'scale': narrow,
                'client_width': args.narrow_client_width, 'screenshot': str(shot),
                'panel': audit['panel'], 'buttons': buttons, 'rows': rows,
            })
            driver.command({'cmd': 'info_edit_input', 'action': 'exit'})
        finally:
            if process is not None:
                try:
                    import win32event
                    win32event.WaitForSingleObject(process, 15000)
                except Exception:
                    pass
            desktop.close()

    (out / 'setup-button-scale.json').write_text(
        json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
    return report


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument('--exe', type=Path, required=True)
    parser.add_argument('--isolation-root', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--dpi', default='2.0')
    parser.add_argument('--scales', default='100,125,150')
    parser.add_argument('--narrow-scale', type=int, default=0)
    parser.add_argument('--narrow-client-width', type=int, default=700)
    args = parser.parse_args()
    report = run(args)
    for item in report['runs']:
        print(json.dumps({key: item[key] for key in item
                          if key in ('case', 'scale', 'heights', 'same_row', 'rows')},
                         ensure_ascii=False))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
