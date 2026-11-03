"""Check font defaults and saved values in the native isolated QA package.

The app uses a private desktop and its own input queue; user input is untouched.
"""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
import math
from pathlib import Path
import sys
import time
import traceback
import wave

from qa_required_library_setup import PrivateDesktop, capture, wait_state
from qa_transport_themes import Driver


def make_library(folder: Path) -> None:
    folder.mkdir(parents=True, exist_ok=False)
    for index, frequency in enumerate((330, 440), start=1):
        samples = (int(4000 * math.sin(2 * math.pi * frequency * n / 22050))
                   for n in range(6615))
        with wave.open(str(folder / f'font-test-{index}.wav'), 'wb') as audio:
            audio.setnchannels(1)
            audio.setsampwidth(2)
            audio.setframerate(22050)
            audio.writeframes(b''.join(sample.to_bytes(2, 'little', signed=True)
                                       for sample in samples))


def main(args: argparse.Namespace) -> int:
    import win32con
    import win32event
    import win32gui
    import win32process

    exe, root, output = (args.exe.resolve(), args.isolation_root.resolve(),
                         args.output.resolve())
    if not exe.is_file() or root.exists() or output.exists():
        raise ValueError('Expected an existing EXE and new isolation/output directories')
    root.mkdir(parents=True)
    output.mkdir(parents=True)
    runtime = root / 'profile/runtime'
    runtime.mkdir(parents=True)
    library = root / 'library'
    make_library(library)
    driver = Driver(runtime, output)
    desktop = PrivateDesktop()
    report = {
        'exe': str(exe), 'sha256': hashlib.sha256(exe.read_bytes()).hexdigest(),
        'isolation_root': str(root), 'started_at': datetime.now(timezone.utc).isoformat(),
        'checks': [], 'screenshots': [],
    }
    process = None
    identity = None

    def command(**payload):
        result = driver.command(payload)
        assert result.get('ok'), result
        return result

    def settings():
        return command(cmd='settings_ui')

    def settle():
        first = settings()['frame']
        deadline = time.monotonic() + 8
        while time.monotonic() < deadline:
            observed = settings()
            if observed['frame'] >= first + 2:
                assert not observed.get('imgui_errors'), observed.get('imgui_log')
                return observed
            time.sleep(.05)
        raise TimeoutError('Settings did not render a fresh frame')

    def find(item_id):
        observed = settle()
        found = []
        for item in observed['items']:
            if item['id'] != item_id:
                continue
            a, b, c, d = item['bounds']
            l, t, r, bottom = item['clip']
            if a >= l - 1 and b >= t - 1 and c <= r + 1 and d <= bottom + 1:
                found.append(item)
        assert len(found) == 1, (item_id, found, observed['page'], observed['tool'])
        return found[0]

    def pointer(x, y):
        ox, oy = win32gui.ClientToScreen(identity['hwnd'], (0, 0))
        for phase in ('hover', 'down', 'up'):
            command(cmd='theme_schedule', action='pointer', phase=phase,
                    x=x - ox, y=y - oy)
            settle()

    def click(item_id):
        a, b, c, d = find(item_id)['bounds']
        pointer((a + c) / 2, (b + d) / 2)

    def key(name):
        command(cmd='settings_ui', action='key', key=name, down=True)
        settle()
        command(cmd='settings_ui', action='key', key=name, down=False)
        settle()

    def query(value):
        click('search')
        command(cmd='settings_ui', action='key', key='Ctrl', down=True)
        settle()
        key('A')
        command(cmd='settings_ui', action='key', key='Ctrl', down=False)
        settle()
        command(cmd='info_edit_input', action='text', text=value)
        settle()

    def screenshot(name):
        path = output / (name + '.png')
        settle()
        capture(driver, path)
        report['screenshots'].append(str(path))

    def assert_fonts(label, reading, listing):
        observed = settle()
        actual = (observed['live_font'], observed['live_list_font'])
        assert actual == (reading, listing), (label, actual, (reading, listing))
        report['checks'].append({'case': label, 'reading': actual[0],
                                 'list': actual[1], 'frame': observed['frame']})

    def start(reading, listing, *, create=False):
        nonlocal process, identity
        process, pid = desktop.start(exe, environment_overrides={
            'LOCALAPPDATA': str(root / 'profile/Local'),
            'APPDATA': str(root / 'profile/Roaming'),
            'AURA_RUNTIME_ROOT': str(runtime),
            'TEMP': str(root / 'temp'), 'TMP': str(root / 'temp'),
        })
        report['startup_pid'] = pid
        startup_status = win32event.WaitForSingleObject(process, 1000)
        if startup_status == win32con.WAIT_OBJECT_0:
            raise RuntimeError(f'test package exited during startup: '
                               f'{win32process.GetExitCodeProcess(process)}')
        identity = command(cmd='magic_eq_identity')
        assert identity['pid'] == pid and Path(identity['exe_path']).resolve() == exe
        assert Path(identity['isolation_root']).resolve() == root
        state = wait_state(driver, lambda value: value['main_ui_ready'], 'native startup', 60)
        if create:
            assert not state['db_path'], state['db_path']
            wait_state(driver, lambda value: value['new_db_dialog_open'], 'new library dialog', 15)
            command(cmd='info_edit_input', action='new_db_name', text='DefaultFontQA')
            time.sleep(.15)
            command(cmd='info_edit_input', action='new_db_create')
            state = wait_state(driver, lambda value: bool(value['db_path']) and
                               not value['new_db_dialog_open'], 'created library', 30)
            command(cmd='scan_folder', path=str(library), recursive=False,
                    cache_waveforms=False, build_similarity_index=False)
            state = wait_state(driver, lambda value: value['total_count'] == 2,
                               'two WAV files scanned', 90)
            capture(driver, output / '01-real-library.png')
            report['screenshots'].append(str(output / '01-real-library.png'))
        assert Path(state['db_path']).resolve().is_relative_to(root), state['db_path']
        assert state['total_count'] == 2, state['total_count']
        report['library'] = {'path': state['db_path'], 'audio_count': 2}
        command(cmd='ordinary_button_window', module='settings', action='open')
        settle()
        assert settings()['open']
        assert_fonts('loaded font preferences', reading, listing)

    def cleanup_process():
        nonlocal process, identity
        if process is None:
            return
        status = win32event.WaitForSingleObject(process, 0)
        if status == win32con.WAIT_TIMEOUT:
            win32process.TerminateProcess(process, 1)
            if win32event.WaitForSingleObject(process, 5000) != win32con.WAIT_OBJECT_0:
                raise RuntimeError('test process did not terminate')
            report['forced_termination'] = True
        elif status != win32con.WAIT_OBJECT_0:
            raise RuntimeError(f'cannot determine test process state: {status}')
        process.Close()
        process = None
        identity = None

    def stop():
        if process is None:
            return
        try:
            if win32event.WaitForSingleObject(process, 0) == win32con.WAIT_TIMEOUT:
                command(cmd='info_edit_input', action='exit')
            assert win32event.WaitForSingleObject(process, 15000) == win32con.WAIT_OBJECT_0
            assert win32process.GetExitCodeProcess(process) == 0
        finally:
            cleanup_process()

    def set_font(search, item, option):
        query(search)
        click('result-' + item)
        click(item)
        click('option-' + str(option))

    try:
        start(16, 18, create=True)
        set_font('system font', 'systemFont', 5)
        screenshot('02-default-system-16')
        set_font('list font', 'listFont', 6)
        screenshot('03-default-list-18')
        set_font('system font', 'systemFont', 7)
        set_font('list font', 'listFont', 3)
        assert settings()['pending'] > 0
        click('apply')
        assert_fonts('manual choices apply', 20, 13)
        screenshot('04-manual-20-13')
        stop()

        start(20, 13)
        assert_fonts('saved choices survive restart', 20, 13)
        query('reset size')
        click('result-resetSize')
        click('resetSize')
        click('reset-size-confirm')
        assert_fonts('reset size restores new defaults', 16, 18)
        screenshot('05-reset-16-18')
        stop()

        start(16, 18)
        assert_fonts('reset choices survive restart', 16, 18)
        stop()

        settings_path = root / 'profile/Local/HarkenSearch/settings.ini'
        assert settings_path.is_file(), settings_path
        lines = settings_path.read_text(encoding='utf-8').splitlines()
        lines = [line for line in lines if line.split('=', 1)[0].strip()
                 not in {'readingFontSize', 'listFontSize'}]
        lines.append('readingFontSize=22')
        settings_path.write_text('\n'.join(lines) + '\n', encoding='utf-8')
        start(22, 18)
        assert_fonts('saved system size leaves missing list at 18', 22, 18)
        screenshot('06-saved-system-22-list-default-18')
        stop()
        report['result'] = 'passed'
        return 0
    except Exception:
        report['result'] = 'failed'
        report['error'] = traceback.format_exc()
        print(report['error'], file=sys.stderr)
        return 1
    finally:
        if process is not None:
            try:
                if identity is not None:
                    try:
                        driver.command({'cmd': 'info_edit_input', 'action': 'exit'})
                    except Exception:
                        report['exit_command_error'] = traceback.format_exc()
                win32event.WaitForSingleObject(process, 15000)
                cleanup_process()
            except Exception:
                report['cleanup_error'] = traceback.format_exc()
        try:
            desktop.close()
        except Exception:
            report['desktop_error'] = traceback.format_exc()
        report['finished_at'] = datetime.now(timezone.utc).isoformat()
        (output / 'report.json').write_text(json.dumps(report, ensure_ascii=False, indent=2),
                                            encoding='utf-8')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exe', type=Path, required=True)
    parser.add_argument('--isolation-root', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    sys.exit(main(parser.parse_args()))
