"""Capture one theme with a scrolled result list so scrollbars are visible.

Used to judge a proposed scrollbar unification. Runs on the private desktop of a
pinned preview isolation root; nothing touches the user's desktop or library.
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
    parser.add_argument('--database', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--theme', default=2, type=int)
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

    def cmd(**payload):
        return driver.command(payload)

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
        state = wait_state(driver, lambda s: s['main_ui_ready'] and bool(s.get('db_path'))
                           and not s['db_switch_in_progress'], 'library', 120)
        target = args.database.resolve()
        if Path(str(state['db_path'])).resolve() != target:
            cmd(cmd='switch_db', path=str(target))
            state = wait_state(driver, lambda s: Path(str(s.get('db_path', ''))).resolve() == target
                               and not s['db_switch_in_progress'], 'switch', 120)
        time.sleep(4.0)

        cmd(cmd='set_theme', theme=args.theme)
        time.sleep(1.0)
        cmd(cmd='scroll_results', top_row=40)
        time.sleep(1.0)
        first = cmd(cmd='theme_schedule')['frame']
        deadline = time.monotonic() + 15
        while time.monotonic() < deadline:
            if cmd(cmd='theme_schedule')['frame'] > first + 20:
                break
            time.sleep(.05)
        capture(driver, out / 'scrolled.png')
        cmd(cmd='scroll_results', top_row=0)
        time.sleep(1.0)
        cmd(cmd='info_edit_input', action='exit')
        win32event.WaitForSingleObject(process, 20000)
        assert win32process.GetExitCodeProcess(process) == 0
        process.Close()
        process = None
        (out / 'result.json').write_text(json.dumps({'ok': True}), encoding='utf-8')
        print('captured', out / 'scrolled.png')
    finally:
        if process is not None:
            try:
                win32process.TerminateProcess(process, 1)
            except Exception:
                pass
            process.Close()
        desktop.close()


if __name__ == '__main__':
    main()
