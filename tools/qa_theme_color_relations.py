"""Capture every built-in theme and the advanced theme editor in an isolated package.

Used to compare two builds pixel by pixel (for example before/after a color-relation
change) and to record which screen regions a single semantic color actually drives.

No shared mouse, user library or installed application is touched: the package runs
on a private desktop, inside its pinned preview isolation root, and every edit is
cancelled before exit.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import time
from datetime import datetime, timezone
from pathlib import Path

from PIL import Image

from qa_required_library_setup import PrivateDesktop, capture, wait_state
from qa_transport_themes import Driver

THEMES = [(0, 'classic-dark'), (1, 'ocean'), (2, 'lapis-blue'),
          (3, 'jade-mist'), (10, 'rose-cedar')]

# One semantic color per case; the value is deliberately far from every theme.
DRAFT_CASES = [
    ('resultsSelection', (0.85, 0.15, 0.75, 1.0)),
    ('toolbarAccent', (0.10, 0.80, 0.20, 1.0)),
    ('controlAccent', (0.15, 0.35, 0.95, 1.0)),
]


def sha256(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--exe', required=True, type=Path)
    parser.add_argument('--isolation-root', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--label', required=True)
    parser.add_argument('--skip-draft-demo', action='store_true')
    parser.add_argument('--database', type=Path,
                        help='library inside the isolation root to open before capturing')
    args = parser.parse_args()

    import win32con
    import win32event
    import win32gui
    import win32process

    exe = args.exe.resolve()
    root = args.isolation_root.resolve()
    out = args.output.resolve()
    profile = root / 'profile/Local/HarkenSearch'
    assert exe.is_file(), exe
    assert (profile / 'settings.ini').is_file(), 'isolated profile must already exist'
    assert profile.resolve() != (Path(os.environ['LOCALAPPDATA']) / 'HarkenSearch').resolve()
    out.mkdir(parents=True, exist_ok=False)

    runtime = root / 'profile/runtime'
    runtime.mkdir(parents=True, exist_ok=True)
    assert not (runtime / 'qa_cmd.json').exists(), 'Pending command in isolation root'

    desktop = PrivateDesktop()
    driver = Driver(runtime, out)
    process = None
    identity = {}
    report = {
        'label': args.label,
        'exe': str(exe),
        'exe_sha256': sha256(exe),
        'started_at': datetime.now(timezone.utc).isoformat(),
        'images': {},
        'draft_cases': [],
    }

    def cmd(**payload):
        return driver.command(payload)

    def settle(frames=4):
        first = cmd(cmd='theme_schedule')['frame']
        deadline = time.monotonic() + 15
        while time.monotonic() < deadline:
            state = cmd(cmd='theme_schedule')
            if state['frame'] > first + frames:
                return state
            time.sleep(.05)
        raise AssertionError('no rendered frames')

    def shot(name):
        path = out / (name + '.png')
        capture(driver, path)
        report['images'][name] = {'file': path.name, 'sha256': sha256(path),
                                  'size': list(Image.open(path).size)}
        return path

    def stop():
        nonlocal process
        if process is None:
            return
        try:
            if win32event.WaitForSingleObject(process, 0) == win32con.WAIT_TIMEOUT:
                cmd(cmd='info_edit_input', action='exit')
            if win32event.WaitForSingleObject(process, 20000) != win32con.WAIT_OBJECT_0:
                win32process.TerminateProcess(process, 1)
                win32event.WaitForSingleObject(process, 5000)
            report['exit_code'] = win32process.GetExitCodeProcess(process)
        finally:
            process.Close()
            process = None

    env = {
        'LOCALAPPDATA': str(root / 'profile/Local'),
        'APPDATA': str(root / 'profile/Roaming'),
        'AURA_RUNTIME_ROOT': str(runtime),
        'AURA_HARNESS_LANE_KIND': 'functional',
        'TEMP': str(root / 'temp'),
        'TMP': str(root / 'temp'),
    }

    try:
        process, pid = desktop.start(exe, environment_overrides=env)
        identity = cmd(cmd='magic_eq_identity', timeout=60)
        assert identity['pid'] == pid and Path(identity['exe_path']).resolve() == exe, identity
        assert Path(identity['isolation_root']).resolve() == root, identity
        report['identity'] = identity
        state = wait_state(driver, lambda s: s['main_ui_ready'] and bool(s.get('db_path'))
                           and not s['db_switch_in_progress'], 'library must load', 120)
        assert Path(state['db_path']).resolve().is_relative_to(root), state['db_path']
        if args.database:
            target = args.database.resolve()
            assert target.is_file() and target.is_relative_to(root), target
            cmd(cmd='switch_db', path=str(target))
            wait_state(driver, lambda s: Path(str(s.get('db_path', ''))).resolve() == target
                       and not s['db_switch_in_progress'], 'library switch', 120)
            settle(10)
            state = wait_state(driver, lambda s: s['total_count'] > 0, 'library rows', 120)
            # Waveforms and covers load asynchronously; let them settle before capture.
            time.sleep(4.0)
            settle(30)
        report['library'] = {'path': state['db_path'], 'rows': state['total_count']}

        for theme, name in THEMES:
            cmd(cmd='set_theme', theme=theme)
            settle(6)
            state = cmd(cmd='get_state')
            assert int(state['theme']) == theme, state['theme']
            shot('main-' + name)

        cmd(cmd='ordinary_button_window', module='theme-editor', action='open')
        settle(6)
        for theme, name in THEMES:
            cmd(cmd='set_theme', theme=theme)
            settle(6)
            shot('editor-' + name)
        cmd(cmd='ordinary_button_window', module='theme-editor', action='close')
        settle(4)

        if not args.skip_draft_demo:
            cmd(cmd='theme_schedule', action='clock', minute=600)
            cmd(cmd='ordinary_button_window', module='settings', action='open')
            settle(6)
            cmd(cmd='settings_ui', action='resize', width=880, height=600)

            def settings_ui():
                return cmd(cmd='settings_ui')

            def schedule():
                return cmd(cmd='theme_schedule')

            def pointer(phase, rect):
                a, b, c, d = rect
                ox, oy = win32gui.ClientToScreen(identity['hwnd'], (0, 0))
                cmd(cmd='theme_schedule', action='pointer', phase=phase,
                    x=(a + c) / 2 - ox, y=(b + d) / 2 - oy)

            def click_rect(rect):
                for phase in ('hover', 'down', 'up'):
                    pointer(phase, rect)
                    settle(2)

            def find(identifier):
                settle(2)
                items = [x for x in settings_ui()['items']
                         if x['id'] == identifier and not x['disabled']]
                assert len(items) == 1, (identifier, settings_ui()['items'])
                return items[0]['bounds']

            def click(identifier):
                click_rect(find(identifier))

            def apply():
                if settings_ui()['pending']:
                    click('apply')
                settle(2)
                assert not settings_ui()['error'], settings_ui()

            def audit_button(label):
                cmd(cmd='ordinary_button_audit')
                settle(2)
                items = [x for x in cmd(cmd='ordinary_button_audit')['checks']
                         if x['label'] == label and not x['disabled']]
                assert len(items) == 1, (label, items)
                return items[0]['bounds']

            assert settings_ui()['open'], 'settings dialog must stay open'
            if settings_ui()['schedule'] == 0:
                click('automatic')
                apply()

            def open_day_editor():
                click('day-combo')
                click('day-editor')
                deadline = time.monotonic() + 15
                while time.monotonic() < deadline:
                    state = schedule()
                    if state['editor_open'] and state['editor_period'] == 0:
                        settle(4)
                        return state
                    time.sleep(.05)
                raise AssertionError('day palette editor did not open')

            for key, rgba in DRAFT_CASES:
                open_day_editor()
                shot('draft-' + key + '-base')
                assert cmd(cmd='theme_schedule', action='draft_color', key=key,
                           rgba=list(rgba))['ok'], key
                settle(8)
                shot('draft-' + key)
                click_rect(audit_button('取消修改'))
                deadline = time.monotonic() + 15
                while time.monotonic() < deadline and schedule()['editor_open']:
                    time.sleep(.05)
                assert not schedule()['editor_open'], 'cancel must close the palette editor'
                report['draft_cases'].append({'key': key, 'rgba': list(rgba)})

            click('automatic')
            apply()
            assert schedule()['mode'] == 0, schedule()['mode']
            if settings_ui()['open']:
                click('done')
                settle(4)
            assert not settings_ui()['open'], 'settings dialog must close before the final frame'

        shot('main-final')
        stop()
    finally:
        try:
            stop()
        finally:
            desktop.close()

    (out / 'report.json').write_text(
        json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
    print(json.dumps({'label': args.label, 'exe_sha256': report['exe_sha256'],
                      'images': len(report['images']),
                      'draft_cases': len(report['draft_cases']),
                      'exit_code': report.get('exit_code')}, ensure_ascii=False))


if __name__ == '__main__':
    main()
