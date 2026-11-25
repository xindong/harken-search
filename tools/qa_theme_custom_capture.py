"""Capture the app as-is under a custom theme JSON, without switching themes.

Verifies that a theme JSON placed beside the executable is actually applied:
launches the pinned preview package on a private desktop and screenshots the
main window and the advanced theme editor.
"""
from __future__ import annotations

import argparse
import json
import time
from pathlib import Path

from qa_required_library_setup import PrivateDesktop, capture, wait_state
from qa_transport_themes import Driver


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--exe', required=True, type=Path)
    parser.add_argument('--isolation-root', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--label', required=True)
    parser.add_argument('--expect-theme', type=int, default=7)
    parser.add_argument('--open-editor', action='store_true')
    parser.add_argument('--play-row', type=int, default=-1)
    args = parser.parse_args()

    import win32con
    import win32event
    import win32process

    exe = args.exe.resolve()
    root = args.isolation_root.resolve()
    out = args.output.resolve()
    runtime = root / 'profile/runtime'
    out.mkdir(parents=True, exist_ok=False)
    assert not (runtime / 'qa_cmd.json').exists()

    desktop = PrivateDesktop()
    driver = Driver(runtime, out)
    process = None
    report = {'label': args.label, 'exe': str(exe)}

    def cmd(**payload):
        return driver.command(payload)

    def settle(frames=6):
        first = cmd(cmd='theme_schedule')['frame']
        deadline = time.monotonic() + 20
        while time.monotonic() < deadline:
            frame = cmd(cmd='theme_schedule')['frame']
            if frame > first + frames:
                return
            time.sleep(.05)
        raise AssertionError('no rendered frames')

    try:
        process, pid = desktop.start(exe, environment_overrides={
            'LOCALAPPDATA': str(root / 'profile/Local'),
            'APPDATA': str(root / 'profile/Roaming'),
            'AURA_RUNTIME_ROOT': str(runtime),
        'AURA_HARNESS_LANE_KIND': 'functional',
            'TEMP': str(root / 'temp'),
            'TMP': str(root / 'temp'),
        })
        identity = cmd(cmd='magic_eq_identity', timeout=60)
        assert identity['pid'] == pid
        wait_state(driver, lambda s: s['main_ui_ready'] and not s['db_switch_in_progress'],
                   'main ui', 120)
        time.sleep(4.0)
        settle(20)
        state = cmd(cmd='get_state')
        report['theme'] = int(state['theme'])
        assert int(state['theme']) == args.expect_theme, report['theme']
        capture(driver, out / 'main.png')

        if args.play_row >= 0:
            cmd(cmd='select_row', row=args.play_row)
            time.sleep(0.6)
            cmd(cmd='play')
            time.sleep(1.2)
            settle(8)
            capture(driver, out / 'playing.png')
            cmd(cmd='pause')
            cmd(cmd='stop')
            time.sleep(0.4)

        if args.open_editor:
            cmd(cmd='ordinary_button_window', module='theme-editor', action='open')
            settle(8)
            capture(driver, out / 'editor.png')
            cmd(cmd='ordinary_button_window', module='theme-editor', action='close')
            settle(4)

        cmd(cmd='info_edit_input', action='exit')
        win32event.WaitForSingleObject(process, 20000)
        report['exit_code'] = win32process.GetExitCodeProcess(process)
        process.Close()
        process = None
    finally:
        if process is not None:
            try:
                win32process.TerminateProcess(process, 1)
            except Exception:
                pass
            process.Close()
        desktop.close()

    (out / 'report.json').write_text(json.dumps(report, ensure_ascii=False, indent=2),
                                     encoding='utf-8')
    print(json.dumps(report, ensure_ascii=False))


if __name__ == '__main__':
    main()
