"""Verify the top menu follows the system font size (18/24/32 px) on a private desktop.

Uses the card's own build, an isolated runtime root and a real scanned library.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import math
import sys
import time
import traceback
import wave
from datetime import datetime, timezone
from pathlib import Path

from qa_required_library_setup import PrivateDesktop, capture, wait_state
from qa_transport_themes import Driver, THEMES

FONT_OPTIONS = (10, 11, 12, 13, 14, 16, 18, 20, 22, 24, 26, 28, 30, 32)
LIST_OPTIONS = (10, 11, 12, 13, 14, 16, 18, 20, 22, 24)
MENU_MIN_PX = 14
kChromeFloor = 14.0


def make_library(folder: Path) -> None:
    folder.mkdir(parents=True, exist_ok=False)
    for index, frequency in enumerate((330, 440, 550), start=1):
        samples = (int(4000 * math.sin(2 * math.pi * frequency * n / 22050))
                   for n in range(6615))
        with wave.open(str(folder / f'menu-font-{index}.wav'), 'wb') as audio:
            audio.setnchannels(1)
            audio.setsampwidth(2)
            audio.setframerate(22050)
            audio.writeframes(b''.join(sample.to_bytes(2, 'little', signed=True)
                                       for sample in samples))


def expected_menu_px(size: int) -> float:
    """Menu keeps 14 px at the 18 px default; larger sizes scale proportionally."""
    return float(min(32, max(MENU_MIN_PX, round(14 * size / 18))))


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
        'isolation_root': str(root),
        'started_at': datetime.now(timezone.utc).isoformat(),
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

    def menu():
        return command(cmd='menu_ui')

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

    def fresh_menu():
        first = menu()
        deadline = time.monotonic() + 8
        while time.monotonic() < deadline:
            observed = menu()
            if observed['frame'] >= first['frame'] + 2:
                return observed
            time.sleep(.05)
        raise TimeoutError('Menu bar did not render a fresh frame')

    def msettle():
        first = menu()['frame']
        deadline = time.monotonic() + 8
        while time.monotonic() < deadline:
            observed = menu()
            if observed['frame'] >= first + 2:
                return observed
            time.sleep(.05)
        raise TimeoutError('Main UI did not advance')

    def differs(path_a, path_b, minimum=0.004):
        from PIL import Image, ImageChops
        with Image.open(path_a) as a, Image.open(path_b) as b:
            if a.size != b.size:
                return True
            diff = ImageChops.difference(a.convert('RGB'), b.convert('RGB'))
            changed = sum(1 for pixel in diff.getdata() if pixel != (0, 0, 0))
            return changed / float(a.size[0] * a.size[1]) >= minimum

    def click_twice(x, y):
        pointer(x, y, phases=('up',))
        for _ in range(3):
            if pointer(x, y):
                return True
            pointer(x, y, phases=('up',))
            time.sleep(.1)
        return False

    def item(item_id):
        observed = settle()
        found = [entry for entry in observed['items'] if entry['id'] == item_id]
        assert len(found) == 1, (item_id, found, observed['page'])
        return found[0]

    def pointer(x, y, phases=('hover', 'down', 'up')):
        hwnd = command(cmd='magic_eq_identity')['hwnd']
        ox, oy = win32gui.ClientToScreen(hwnd, (0, 0))
        accepted = []
        for phase in phases:
            try:
                driver.command({'cmd': 'theme_schedule', 'action': 'pointer',
                                'phase': phase, 'x': x - ox, 'y': y - oy})
                accepted.append(True)
            except RuntimeError:
                accepted.append(False)
            msettle()
        return all(accepted)

    def hover(x, y):
        return pointer(x, y, phases=('hover',))

    def click(item_id):
        a, b, c, d = item(item_id)['bounds']
        pointer((a + c) / 2, (b + d) / 2)

    def key(name):
        command(cmd='settings_ui', action='key', key=name, down=True)
        msettle()
        command(cmd='settings_ui', action='key', key=name, down=False)
        msettle()

    def query(value):
        click('search')
        command(cmd='settings_ui', action='key', key='Ctrl', down=True)
        settle()
        key('A')
        command(cmd='settings_ui', action='key', key='Ctrl', down=False)
        settle()
        command(cmd='info_edit_input', action='text', text=value)
        settle()

    def shot(name):
        path = output / (name + '.png')
        msettle()
        capture(driver, path)
        report['screenshots'].append(str(path))
        return path

    def choose_font(size):
        query('system font')
        click('result-systemFont')
        click('systemFont')
        click('option-' + str(FONT_OPTIONS.index(size)))
        settle()

    def choose_list(size):
        query('list font')
        click('result-listFont')
        click('listFont')
        click('option-' + str(LIST_OPTIONS.index(size)))
        settle()

    def choose_theme(index):
        query('theme')
        click('result-theme')
        click('stock-' + str(index))
        settle()

    def ensure_settings():
        if not settle()['open']:
            command(cmd='ordinary_button_window', module='settings', action='open')
            settle()
        command(cmd='settings_ui', action='resize', width=1120, height=800)
        settle()
        click('nav-1')
        settle()

    def apply():
        observed = settle()
        if observed['pending']:
            click('apply')
            settle()

    def resize_window(width, height):
        hwnd = command(cmd='magic_eq_identity')['hwnd']
        win32gui.SetWindowPos(hwnd, 0, 0, 0, width, height,
                              win32con.SWP_NOMOVE | win32con.SWP_NOZORDER)
        msettle()
        time.sleep(.5)
        return msettle()

    def check_menu_fits(label, max_width):
        observed = fresh_menu()
        assert observed['bar_right'] <= max_width + 2, (label, observed)
        assert observed['last_item_right'] <= observed['bar_right'] + 0.5, (label, observed)
        assert kChromeFloor <= observed['font_pixels'] <= expected_menu_px(32) + 0.6, (label, observed)
        items = observed['items']
        assert len(items) >= 8, (label, [entry['label'] for entry in items])
        for entry in items:
            x0, y0, x1, y1 = entry['bounds']
            assert x1 <= observed['bar_right'] + 0.5, (label, entry, observed)
            assert y0 >= observed['bar_top'] - 1 and y1 <= observed['bar_top'] + observed['bar_height'] + 1, (label, entry, observed)
        report['checks'].append({'case': label, 'menu_px': observed['font_pixels'],
                                 'bar_height': observed['bar_height'],
                                 'bar_right': observed['bar_right'],
                                 'last_item_right': observed['last_item_right']})
        return observed

    def check_menu(label, size, width_expect=None):
        observed = fresh_menu()
        expected = expected_menu_px(size)
        assert abs(observed['font_pixels'] - expected) < .6, (label, observed)
        assert observed['bar_height'] >= expected + 6, (label, observed)
        assert observed['workspace_top'] + 2 >= observed['bar_top'] + observed['bar_height'], (label, observed)
        assert observed['bar_right'] >= observed['last_item_right'] - 0.5, (label, observed)
        items = observed['items']
        assert len(items) >= 8, (label, [entry['label'] for entry in items])
        assert len({entry['label'] for entry in items}) >= 8, [entry['label'] for entry in items]
        assert observed['bar_right'] - observed['last_item_right'] >= -0.5, (label, observed)
        for entry in items:
            x0, y0, x1, y1 = entry['bounds']
            assert x1 <= observed['bar_right'] + 0.5, (label, entry, observed)
            assert y0 >= observed['bar_top'] - 1 and y1 <= observed['bar_top'] + observed['bar_height'] + 1, (label, entry, observed)
            assert x1 - x0 > 8
        labels = [entry['label'] for entry in items]
        assert all(labels), labels
        heights = [entry['bounds'][3] - entry['bounds'][1] for entry in items]
        assert max(heights) - min(heights) < 1.5, (label, items)
        report['checks'].append({'case': label, 'requested_px': size,
                                 'menu_px': observed['font_pixels'],
                                 'bar_top': observed['bar_top'],
                                 'bar_height': observed['bar_height'],
                                 'workspace_top': observed['workspace_top'],
                                 'work_pos': observed.get('work_pos'),
                                 'items': labels,
                                 'frame': observed['frame']})
        print('menu', size, '->', observed['font_pixels'], 'bar', observed['bar_height'])
        return observed

    def close_scan_result():
        deadline = time.monotonic() + 25
        while time.monotonic() < deadline:
            state = command(cmd='scan_dialog_ui', action='state')
            if not state['windows']:
                return
            target = next((entry for entry in state['items']
                           if entry['id'] == 'scan.result.done'), None)
            if target:
                left, top, right, bottom = target['bounds']
                for phase in ('hover', 'down', 'up'):
                    command(cmd='scan_dialog_ui', action='pointer', phase=phase,
                            x=(left + right) / 2, y=(top + bottom) / 2)
                    time.sleep(.1)
            time.sleep(.2)
        raise TimeoutError('scan result dialog did not close')

    def start():
        nonlocal process, identity
        process, pid = desktop.start(exe, environment_overrides={
            'LOCALAPPDATA': str(root / 'profile/Local'),
            'APPDATA': str(root / 'profile/Roaming'),
            'AURA_RUNTIME_ROOT': str(runtime),
            'TEMP': str(root / 'temp'), 'TMP': str(root / 'temp'),
        })
        report['startup_pid'] = pid
        if win32event.WaitForSingleObject(process, 1000) == win32con.WAIT_OBJECT_0:
            raise RuntimeError('test package exited during startup: '
                               f'{win32process.GetExitCodeProcess(process)}')
        identity = command(cmd='magic_eq_identity')
        assert identity['pid'] == pid
        assert Path(identity['exe_path']).resolve() == exe
        assert Path(identity['isolation_root']).resolve() == root
        return wait_state(driver, lambda value: value['main_ui_ready'],
                          'native startup', 60)

    def create_library():
        state = wait_state(driver, lambda value: value['new_db_dialog_open'],
                           'new library dialog', 20)
        command(cmd='info_edit_input', action='new_db_name', text='TopMenuFontQA')
        time.sleep(.15)
        command(cmd='info_edit_input', action='new_db_create')
        state = wait_state(driver, lambda value: bool(value['db_path']) and
                           not value['new_db_dialog_open'], 'created library', 30)
        command(cmd='scan_folder', path=str(library), recursive=False,
                cache_waveforms=False, build_similarity_index=False)
        state = wait_state(driver, lambda value: value['total_count'] == 3,
                           'three WAV files scanned', 120)
        assert Path(state['db_path']).resolve().is_relative_to(root), state['db_path']
        close_scan_result()
        report['library'] = {'path': state['db_path'], 'audio_count': 3}
        return state

    def stop():
        nonlocal process
        if process is None:
            return
        command(cmd='info_edit_input', action='tray_exit')
        assert win32event.WaitForSingleObject(process, 15000) == win32con.WAIT_OBJECT_0
        assert win32process.GetExitCodeProcess(process) == 0
        process.Close()
        process = None

    try:
        state = start()
        if not state['db_path']:
            state = create_library()
        command(cmd='ordinary_button_window', module='settings', action='open')
        settle()
        click('nav-1')
        if settle()['live_font'] != 18:
            choose_font(18)
            apply()
        if settle()['live_list_font'] != 18:
            choose_list(18)
            apply()
        ensure_settings()
        choose_theme(0)
        apply()
        click('nav-1')
        check_menu('menu follows 18 px system font', 18)
        shot('01-menu-18')

        choose_font(24)
        apply()
        click('nav-1')
        check_menu('menu follows 24 px system font', 24)
        shot('02-menu-24')

        choose_font(32)
        apply()
        click('nav-1')
        observed = check_menu('menu follows new 32 px system font', 32)
        shot('03-menu-32')
        assert settle()['live_font'] == 32

        items = observed['items']
        if settle()['open']:
            click('close')
            msettle()
            if settle()['prompt']:
                click('discard')
                settle()
        closed = shot('03b-menu-32-closed')
        assert not fresh_menu()['popups'], 'menus should be closed before probing'
        first = items[0]['bounds']
        opened = []
        for _ in range(3):
            click_twice((first[0] + first[2]) / 2, (first[1] + first[3]) / 2)
            opened = fresh_menu()['popups']
            if opened:
                break
            time.sleep(.3)
        report['menu_probe'] = {'popups_after_click': opened,
                                'hovered': fresh_menu().get('hovered_window'),
                                'mouse': fresh_menu().get('mouse')}
        dropdown = shot('04-menu-32-dropdown')
        assert len(opened) == 1, opened
        assert abs(opened[0]['font_pixels'] - expected_menu_px(32)) < .6, opened
        assert opened[0]['bounds'][1] >= observed['bar_top'] + observed['bar_height'] - 1, opened
        report['checks'].append({'case': 'file dropdown renders at the menu font size',
                                 'popup_px': opened[0]['font_pixels'],
                                 'popup_bounds': opened[0]['bounds']})
        pointer(1500.0, 900.0)
        msettle()
        assert not fresh_menu()['popups'], 'file menu did not close'
        view = items[2]['bounds']
        click_twice((view[0] + view[2]) / 2, (view[1] + view[3]) / 2)
        viewp = fresh_menu()['popups']
        assert len(viewp) >= 1, 'view menu did not open'
        assert abs(viewp[0]['font_pixels'] - expected_menu_px(32)) < .6, viewp
        view_shot = shot('05-menu-32-view')
        cascade = None
        row_step = observed['font_pixels'] + 6.0
        for row in range(1, 13):
            hover_y = observed['bar_top'] + observed['bar_height'] + 6.0 + row * row_step
            hover(view[0] + 40.0, hover_y)
            msettle()
            popups = fresh_menu()['popups']
            if len(popups) >= 2:
                cascade = shot('06-menu-32-cascade')
                ids = [entry['name'] for entry in popups]
                assert all(abs(entry['font_pixels'] - expected_menu_px(32)) < .6 for entry in popups), popups
                report['checks'].append({'case': 'cascading submenu renders at the menu font size',
                                         'popups': ids,
                                         'sizes': [entry['font_pixels'] for entry in popups]})
                break
        assert cascade is not None, 'no cascading submenu opened while hovering the view menu'
        pointer(1500.0, 900.0)
        msettle()
        pointer(1500.0, 900.0)
        msettle()
        assert not fresh_menu()['popups'], 'view menu did not close'
        ensure_settings()

        ensure_settings()
        for index, (value, name) in enumerate(THEMES):
            choose_theme(index)
            apply()
            click('nav-1')
            check_menu(f'menu 32 px on theme {name}', 32)
            shot('07-theme-' + str(index))
        ensure_settings()
        choose_theme(0)
        apply()
        click('nav-1')

        click('close')
        msettle()
        if settle()['prompt']:
            click('discard')
            settle()
        hwnd_now = command(cmd='magic_eq_identity')['hwnd']
        left, top, right, bottom = win32gui.GetWindowRect(hwnd_now)
        original_size = (right - left, bottom - top)
        for width in (900, 640):
            narrow = resize_window(width, 700)
            shot('09-narrow-' + str(width) + '-32')
            check_menu_fits('narrow ' + str(width) +
                            ' px window keeps every menu entry inside the bar',
                            narrow['work_size'][0])
        resize_window(original_size[0], original_size[1])
        ensure_settings()

        choose_list(24)
        apply()
        click('nav-1')
        observed = settle()
        assert observed['live_list_font'] == 24 and observed['live_font'] == 32
        report['checks'].append({'case': 'list font stays independent at its 24 px cap',
                                 'reading': observed['live_font'],
                                 'list': observed['live_list_font']})

        click('close')
        if settle()['prompt']:
            click('discard')
            settle()
        stop()
        start()
        observed = fresh_menu()
        assert abs(observed['font_pixels'] - expected_menu_px(32)) < .6, observed
        assert settle()['live_font'] == 32
        report['checks'].append({'case': 'restart keeps 32 px menu', 'frame': observed['frame']})
        shot('08-restart-32')
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
                command(cmd='info_edit_input', action='tray_exit')
                win32event.WaitForSingleObject(process, 15000)
            except Exception:
                report['cleanup_error'] = traceback.format_exc()
        try:
            desktop.close()
        except Exception:
            report['desktop_error'] = traceback.format_exc()
        report['finished_at'] = datetime.now(timezone.utc).isoformat()
        (output / 'report.json').write_text(
            json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exe', type=Path, required=True)
    parser.add_argument('--isolation-root', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    sys.exit(main(parser.parse_args()))
