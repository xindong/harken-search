"""Check the 18/24/32 px system font against rendered Harken menus on a private desktop.

Requires pywin32, Pillow, and an EXE built with ARIS_PREVIEW_ISOLATION=ON,
AURA_BUILD_DIAGNOSTICS=ON and AURA_INTERNAL_BETA=OFF. Pass the CMake cache
for that EXE and its exact compile-time isolation root. A fresh root gets
synthetic WAVs. For an existing QA root, explicitly pass its synthetic WAV
directory: the test creates a new fixture database and writes QA settings
inside that root. Do not share the root with a running preview or other QA.
The monitor work area must fit the 1600 x 1200 wide QA client plus its frame.
"""
from __future__ import annotations

import argparse
import ctypes
import hashlib
import json
import math
import os
import re
import shutil
import stat
import subprocess
import sys
import time
import traceback
import uuid
import wave
from ctypes import wintypes
from datetime import datetime, timezone
from pathlib import Path

from qa_required_library_setup import PrivateDesktop, capture
from qa_transport_themes import Driver, window_api

FONT_OPTIONS = (10, 11, 12, 13, 14, 16, 18, 20, 22, 24, 26, 28, 30, 32)
LIST_OPTIONS = FONT_OPTIONS[:10]
MENU_SIZES = {18: (16, 16), 24: (21, 21), 32: (28, 28)}
MENU_IDS = ('File', 'Edit', 'View', 'Database', 'Playlist', 'Tools', 'Settings', 'Help')
STOCK_THEMES = ((0, 'dark'), (10, 'rose-cedar'), (2, 'lapis-blue'), (1, 'ocean'), (3, 'jade-mist'))
WIDE_SIZE = (1600, 1200)
FONT_TOLERANCE = 0.5
EDGE_TOLERANCE = 2


def expected_menu_px(size: int) -> int:
    return MENU_SIZES[size][0]


def expected_popup_px(size: int) -> int:
    return MENU_SIZES[size][1]


def absolute_path(path: Path) -> Path:
    return Path(os.path.abspath(path))


def reject_reparse(path: Path) -> None:
    for candidate in (path, *path.parents):
        try:
            attributes = candidate.lstat().st_file_attributes
        except FileNotFoundError:
            continue
        if attributes & stat.FILE_ATTRIBUTE_REPARSE_POINT:
            raise ValueError(f'Reparse points are not allowed in the QA paths: {candidate}')


def isolated_path(path: Path, root: Path) -> Path:
    if not path.is_absolute() or not absolute_path(path).is_relative_to(root):
        raise ValueError(f'Expected an absolute path inside the isolation root: {path}')
    path = absolute_path(path)
    reject_reparse(path)
    return path


def exe_has_root(image: bytes, root: Path) -> bool:
    return b'\0' + root.as_posix().lower().encode('utf-8') + b'\0' in image.lower().replace(b'\\', b'/')


def read_process_image(pid: int) -> Path:
    import win32api
    import win32con

    query = ctypes.WinDLL('kernel32', use_last_error=True).QueryFullProcessImageNameW
    query.argtypes = (wintypes.HANDLE, wintypes.DWORD, wintypes.LPWSTR,
                      ctypes.POINTER(wintypes.DWORD))
    query.restype = wintypes.BOOL
    handle = win32api.OpenProcess(win32con.PROCESS_QUERY_LIMITED_INFORMATION, False, pid)
    try:
        image = ctypes.create_unicode_buffer(32768)
        length = wintypes.DWORD(len(image))
        if not query(int(handle), 0, image, ctypes.byref(length)) or not length.value:
            raise ctypes.WinError(ctypes.get_last_error())
        return Path(image.value)
    finally:
        handle.Close()


def probe_idle_root(root: Path) -> None:
    import pywintypes
    from win32com.client import GetObject

    query = "SELECT ProcessId, ExecutablePath FROM Win32_Process WHERE Name = 'HarkenSearch.exe'"
    for process in GetObject('winmgmts:').ExecQuery(query):
        try:
            reported = process.ExecutablePath
            path = Path(reported) if reported else read_process_image(process.ProcessId)
            if not path.is_absolute():
                raise ValueError('Invalid executable path')
            image = path.read_bytes()
            if not image.startswith(b'MZ'):
                raise ValueError('Not a Windows executable')
        except (OSError, ValueError, pywintypes.error) as error:
            raise RuntimeError(f'Cannot verify the QA root of PID {process.ProcessId}; '
                               f'no process was stopped: {error}') from error
        if exe_has_root(image, root):
            raise RuntimeError(f'Isolation root is in use by PID {process.ProcessId} ({path}); no process was stopped')


def require_idle_root(root: Path) -> None:
    """Keep WMI/COM windows off the thread that will bind the private desktop."""
    try:
        probe = subprocess.run(
            [sys.executable, '-B', '-X', 'utf8', str(Path(__file__).resolve()),
             '--probe-idle-root', str(root)],
            capture_output=True, encoding='utf-8', timeout=30,
            creationflags=subprocess.CREATE_NO_WINDOW,
        )
    except (OSError, subprocess.TimeoutExpired) as error:
        raise RuntimeError(f'Cannot verify that the QA root is idle: {error}') from error
    if probe.returncode:
        reason = probe.stderr.strip() or f'idle-root probe exited with code {probe.returncode}'
        raise RuntimeError(reason)


def preflight(args: argparse.Namespace):
    import win32con
    import win32file

    if not __debug__:
        raise RuntimeError('Run QA with assertions enabled (do not use python -O)')
    exe = args.exe.resolve(strict=True)
    root = absolute_path(args.isolation_root)
    output = absolute_path(args.output)
    for path in (root, output):
        if not path.parent.is_dir() or win32file.GetDriveType(path.anchor) != win32con.DRIVE_FIXED:
            raise ValueError(f'Expected a local QA directory with an existing parent: {path}')
        reject_reparse(path)
    if output.exists() or output.is_relative_to(root) or root.is_relative_to(output):
        raise ValueError('Use a new output directory separate from the isolation root')
    if not exe.is_file() or exe.name.casefold() != 'harkensearch.exe':
        raise ValueError(f'Expected a Harken application EXE: {exe}')

    flags = {}
    for line in args.build_cache.read_text(encoding='utf-8-sig').splitlines():
        typed_key, equals, value = line.partition('=')
        if equals:
            flags[typed_key.partition(':')[0]] = value
    expected_flags = {'ARIS_PREVIEW_ISOLATION': 'ON', 'AURA_BUILD_DIAGNOSTICS': 'ON',
                      'AURA_INTERNAL_BETA': 'OFF'}
    for name, expected in expected_flags.items():
        if flags.get(name) != expected:
            raise ValueError(f'The EXE build cache must have {name}={expected}')
    pinned = Path(flags.get('ARIS_PREVIEW_ISOLATION_ROOT', ''))
    if not pinned.is_absolute() or absolute_path(pinned) != root:
        raise ValueError(f'--isolation-root must match ARIS_PREVIEW_ISOLATION_ROOT: {pinned}')
    image = exe.read_bytes()
    channels = (b'magic_eq_identity\0', b'menu_ui\0', b'settings_ui\0', b'capture_preview_frame\0',
                b'folder_search_pointer\0')
    if not image.startswith(b'MZ') or not exe_has_root(image, root) or not all(
            channel in image for channel in channels):
        raise ValueError('EXE does not embed the requested pinned root and the required QA commands')

    fixture = isolated_path(args.fixture_dir, root) if args.fixture_dir else None
    if root.exists():
        if not root.is_dir() or fixture is None or not fixture.is_dir():
            raise ValueError('To reuse an existing QA root, supply its synthetic WAV directory with --fixture-dir')
        for name in ('profile', 'temp'):
            directory = root / name
            reject_reparse(directory)
            if fixture.is_relative_to(directory) or directory.is_relative_to(fixture):
                raise ValueError('Keep the synthetic WAV folder separate from the QA profile and temp data')
        for folder, dirs, files in os.walk(root, followlinks=False, onerror=raise_walk_error):
            for name in (*dirs, *files):
                candidate = Path(folder) / name
                if candidate.lstat().st_file_attributes & stat.FILE_ATTRIBUTE_REPARSE_POINT:
                    raise ValueError(f'Reparse points are not allowed in the QA root: {candidate}')
        settings_file = root / 'profile/Local/HarkenSearch/settings.ini'
        if settings_file.is_file():
            for line in settings_file.read_text(encoding='utf-8-sig').splitlines():
                key, _, value = line.partition('=')
                if value and (key == 'currentDbPath' or re.fullmatch(r'recentDb\d+', key)):
                    isolated_path(Path(value), root)
    elif fixture is not None:
        raise ValueError('--fixture-dir is only for reusing an existing isolated QA root')

    runtime = root / 'profile/runtime'
    for pending in (root / 'top-menu-font-qa.lock', runtime / 'qa_cmd.json'):
        if pending.exists():
            raise RuntimeError(f'Isolation root has an outstanding QA run or command: {pending}')
    require_idle_root(root)
    if fixture is not None:
        audio_files(fixture)
    return exe, root, output, fixture, hashlib.sha256(image).hexdigest()


def raise_walk_error(error):
    raise error


def audio_files(folder: Path) -> list[Path]:
    files = sorted(path for path in folder.rglob('*') if path.is_file())
    if not (1 <= len(files) <= 8) or any(path.suffix.lower() != '.wav' for path in files):
        raise ValueError('Provide a synthetic fixture directory with 1–8 WAVs and no other files')
    if len({path.name.casefold() for path in files}) != len(files):
        raise ValueError('Fixture WAV basenames must be distinct for result-list verification')
    return files


def make_library(folder: Path) -> None:
    folder.mkdir(exist_ok=False)
    for index, frequency in enumerate((330, 440, 550), start=1):
        samples = (int(4000 * math.sin(2 * math.pi * frequency * frame / 22050))
                   for frame in range(6615))
        with wave.open(str(folder / f'menu-font-{index}.wav'), 'wb') as audio:
            audio.setnchannels(1)
            audio.setsampwidth(2)
            audio.setframerate(22050)
            audio.writeframes(b''.join(sample.to_bytes(2, 'little', signed=True)
                                       for sample in samples))


def midpoint(bounds):
    left, top, right, bottom = bounds
    return (left + right) / 2, (top + bottom) / 2


def inside(bounds, container, tolerance=EDGE_TOLERANCE):
    left, top, right, bottom = bounds
    outer_left, outer_top, outer_right, outer_bottom = container
    return (outer_left - tolerance <= left < right <= outer_right + tolerance
            and outer_top - tolerance <= top < bottom <= outer_bottom + tolerance)


def settings_click_target(observed, item_id, client):
    found = [entry for entry in observed['items'] if entry['id'] == item_id and not entry['disabled']]
    assert len(found) == 1, (item_id, observed['page'], observed['items'])
    item = found[0]
    bounds, clip = item['bounds'], item['clip']
    visible = (max(bounds[0], clip[0], client[0]), max(bounds[1], clip[1], client[1]),
               min(bounds[2], clip[2], client[2]), min(bounds[3], clip[3], client[3]))
    assert inside(visible, client, 0), ('Settings item is outside the client', item, client)
    if not item_id.startswith('option-'):
        return midpoint(visible), None
    popups = [popup for popup in observed['color_popups']
              if popup['name'].startswith('##Combo_') and inside(visible, popup['bounds'], 0)]
    assert len(popups) == 1, ('Could not identify the settings combo for this option', item, popups)
    popup = popups[0]
    assert inside(popup['bounds'], client, 0), ('Settings combo extends outside the QA client',
                                                item, popup, client)
    return midpoint(visible), popup['name']


def check_menu_geometry(observed, client, labels, size, narrow=False):
    left, top, right, bottom = client
    bar_top = observed['bar_top']
    bar_height = observed['bar_height']
    bar_right = observed['bar_right']
    bar_bottom = bar_top + bar_height
    assert observed['workspace_frame'] == observed['frame'], observed
    assert abs(bar_top - top) <= EDGE_TOLERANCE, (client, observed)
    assert abs(bar_right - right) <= EDGE_TOLERANCE, (client, observed)
    assert abs(observed['workspace_top'] - bar_bottom) <= EDGE_TOLERANCE, observed
    work_left, work_top = observed['work_pos']
    work_width, work_height = observed['work_size']
    assert abs(work_left - left) <= EDGE_TOLERANCE, (client, observed)
    assert abs(work_left + work_width - right) <= EDGE_TOLERANCE, (client, observed)
    assert abs(work_top + work_height - bottom) <= EDGE_TOLERANCE, (client, observed)
    assert work_top - EDGE_TOLERANCE <= observed['workspace_top'] < bottom, observed

    actual = observed['font_pixels']
    expected = expected_menu_px(size)
    if narrow:
        assert 13 - FONT_TOLERANCE <= actual <= expected + FONT_TOLERANCE, observed
    else:
        assert abs(actual - expected) < FONT_TOLERANCE, observed
    assert bar_height >= actual + 6, observed

    items = observed['items']
    assert [entry['label'] for entry in items] == labels, items
    previous_right = left
    item_heights = []
    for entry in items:
        item_left, item_top, item_right, item_bottom = entry['bounds']
        assert inside(entry['bounds'], (left, bar_top, bar_right, bar_bottom), 1), entry
        assert item_left >= previous_right - 0.5 and item_right - item_left > 8, items
        previous_right = item_right
        item_heights.append(item_bottom - item_top)
    assert max(item_heights) - min(item_heights) < 1.5, items
    assert abs(observed['last_item_right'] - previous_right) <= 1, observed


def menu_popups(observed):
    opened = {}
    for entry in observed['popups']:
        label, separator, depth = entry['name'].rpartition('###Menu_')
        if separator and depth.isdigit():
            index = int(depth)
            assert index not in opened, observed['popups']
            opened[index] = dict(entry, label=label)
    return opened


def hover_points(bounds, client):
    left, top, right, bottom = bounds
    top, bottom = max(top, client[1]), min(bottom, client[3])
    point_x = left + (right - left) / 2
    steps = max(1, math.ceil((bottom - top) / 8))
    for index in range(steps):
        yield point_x, top + (index + 0.5) * (bottom - top) / steps


def context_row_target(observed, dock, client, names):
    tabs = [tab for tab in dock.get('results_tabs', []) if 'rect' in tab]
    assert tabs, ('No drawn results-tab geometry', dock)
    point_x = min(tab['rect']['x'] for tab in tabs) + 48
    rows = zip(observed['drawn_list_raw_names'], observed['drawn_list_directories'],
               observed['drawn_list_row_origins'], observed['drawn_list_hit_heights'], strict=True)
    for name, directory, top, height in rows:
        point = (point_x, top + height * .42)
        if not directory and name in names and height > 0 and (
                client[0] <= point[0] < client[2] and client[1] <= point[1] < client[3]):
            return name, point
    raise AssertionError(('No drawn isolated WAV row', names, observed))


def danger_ink(path, bounds, client, *, browser):
    from PIL import Image

    left, top, right, bottom = (round(value - client[index % 2])
                                for index, value in enumerate(bounds))
    band_top = top + 4 if browser else max(top + 4, bottom - 80)
    band_bottom = min(bottom - 4, top + 110) if browser else bottom - 4
    label_right = left + round((right - left) * .72)
    with Image.open(path) as frame:
        pixels = frame.convert('RGB').crop((left + 7, band_top, label_right, band_bottom))
    points = [(index % pixels.width + left + 7, index // pixels.width + band_top)
              for index, (red, green, blue) in enumerate(pixels.getdata())
              if red >= 235 and abs(green - 102) <= 14 and abs(blue - 102) <= 14
              and abs(green - blue) <= 6]
    assert len(points) >= 12 and len({row for _, row in points}) >= 4, (path, bounds, points)
    columns, rows = zip(*points)
    return {'pixels': len(points), 'client_ink_box': [min(columns), min(rows), max(columns), max(rows)]}


def check_browser_context_source():
    source = Path(__file__).resolve().parents[1] / 'src/main.cpp'
    text = source.read_text(encoding='utf-8')
    block = text.split('ThemeMenuPopupShellScope browserMenuShell(', 1)[1].split('browserMenuShell.reset()', 1)[0]
    assert re.match(r'false,\s*GetMenuFontSizePx\(\)\s*\);', block), 'Browser audio shell does not use system menu size'
    assert 'beginContextItem("##BrowserRowCtx", !entry.isDirectory)' in block
    assert re.search(r'PushStyleColor\(ImGuiCol_Text,\s*ImVec4\(1\.0f,\s*0\.4f,\s*0\.4f,\s*1\.0f\)\);\s*'
                     r'if\s*\(ImGui::MenuItem\(L\(Ctx_DeleteFromFS\)', block)
    assert 'BeginMenu(L(Ctx_SendToDAW))' in block, 'Browser no longer has its conditional cascade'
    return 'Browser audio shell, red Delete from disk and optional Send to DAW submenu checked in source'


def main(args: argparse.Namespace) -> int:
    exe, root, output, existing_fixture, digest = preflight(args)
    if args.preflight_only:
        print(f'Preflight only: {exe} [SHA-256 {digest}] -> {root}')
        return 0

    import win32api
    import win32con
    import win32event
    import win32gui
    import win32process
    from PIL import Image

    window_api()
    output.mkdir()
    if existing_fixture is None:
        root.mkdir()
    lock = root / 'top-menu-font-qa.lock'
    with lock.open('x', encoding='utf-8') as stream:
        stream.write(str(os.getpid()))
    report = {'exe': str(exe), 'sha256': digest, 'isolation_root': str(root),
              'started_at': datetime.now(timezone.utc).isoformat(), 'checks': [],
              'screenshots': [], 'starts': []}
    report['scope'] = 'context-menus-only' if args.context_menus_only else 'full-font-and-context-regression'
    process = desktop = identity = None
    translations = None
    runtime = root / 'profile/runtime'
    driver = Driver(runtime, output)

    def command(**payload):
        if identity is None:
            raise RuntimeError('Refusing to send QA commands before validating process identity')
        return driver.command(payload)

    def settings():
        return command(cmd='settings_ui')

    def menu():
        return command(cmd='menu_ui')

    def wait_for(read, accept, label, timeout=8):
        last = None
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            last = read()
            if accept(last):
                return last
            time.sleep(.04)
        raise TimeoutError(f'{label}: {last}')

    def settle(read=menu, accept=lambda observed: True):
        first = read()['current_frame']
        return wait_for(read, lambda observed: (
            observed['frame'] >= first + 2 and
            0 <= observed['current_frame'] - observed['frame'] <= 2 and accept(observed)),
            f'No fresh {read.__name__} observation')

    def settings_frame(accept=lambda observed: True):
        observed = settle(settings, accept)
        assert not observed['imgui_errors'], observed['imgui_log']
        return observed

    def start():
        nonlocal process, identity
        require_idle_root(root)
        if (runtime / 'qa_cmd.json').exists():
            raise RuntimeError(f'Outstanding QA command in {runtime}')
        process, pid = desktop.start(exe, environment_overrides={
            'LOCALAPPDATA': str(root / 'profile/Local'),
            'APPDATA': str(root / 'profile/Roaming'),
            'AURA_RUNTIME_ROOT': str(runtime),
            'AURA_HARNESS_LANE_KIND': 'folder-search',
            'TEMP': str(root / 'temp'), 'TMP': str(root / 'temp'),
            'ARIS_PREVIEW_ISOLATION': '1',
        })
        report['starts'].append({'pid': pid})
        if win32event.WaitForSingleObject(process, 1000) == win32con.WAIT_OBJECT_0:
            raise RuntimeError(f'QA EXE exited during startup: {win32process.GetExitCodeProcess(process)}')
        reply = driver.command({'cmd': 'magic_eq_identity'}, timeout=60)
        assert (reply['pid'] == pid and Path(reply['exe_path']).resolve(strict=True) == exe
                and isolated_path(Path(reply['isolation_root']), root) == root
                and isolated_path(Path(reply['runtime_root']), root) == runtime), reply
        identity = reply
        report['starts'][-1].update(reply)
        state = wait_for(app_state, lambda value: value['main_ui_ready'], 'Native startup', 60)
        hwnd()
        return state

    def hwnd():
        window = identity['hwnd']
        assert win32process.GetWindowThreadProcessId(window)[1] == identity['pid'], identity
        return window

    def client_bounds():
        window = hwnd()
        left, top = win32gui.ClientToScreen(window, (0, 0))
        _, _, width, height = win32gui.GetClientRect(window)
        return left, top, left + width, top + height

    def resize(width, height):
        window = hwnd()
        if win32gui.GetWindowPlacement(window)[1] == win32con.SW_SHOWMAXIMIZED:
            win32gui.ShowWindow(window, win32con.SW_RESTORE)
        left, top, right, bottom = win32gui.GetWindowRect(window)
        _, _, client_width, client_height = win32gui.GetClientRect(window)
        outer_width = width + right - left - client_width
        outer_height = height + bottom - top - client_height
        monitor = win32api.MonitorFromWindow(window, win32con.MONITOR_DEFAULTTONEAREST)
        work_left, work_top, work_right, work_bottom = win32api.GetMonitorInfo(monitor)['Work']
        if outer_width > work_right - work_left or outer_height > work_bottom - work_top:
            raise RuntimeError(f'The QA client {width}x{height} plus its frame requires '
                               f'a {outer_width}x{outer_height} monitor work area')
        left = min(max(left, work_left), work_right - outer_width)
        top = min(max(top, work_top), work_bottom - outer_height)
        win32gui.SetWindowPos(window, 0, left, top, outer_width, outer_height,
                              win32con.SWP_NOZORDER | win32con.SWP_NOACTIVATE)
        wait_for(client_bounds, lambda box: (box[2] - box[0], box[3] - box[1]) == (width, height),
                 f'Native client dimensions did not reach {width}x{height}')
        settle(accept=lambda observed: abs(observed['bar_right'] - client_bounds()[2]) <= EDGE_TOLERANCE)

    def pointer_at(point, phases=('hover', 'down', 'up')):
        left, top, _, _ = client_bounds()
        x, y = point
        for phase in phases:
            command(cmd='theme_schedule', action='pointer', phase=phase, x=x - left, y=y - top)
            observed = settle()
        return observed

    def click(item_id):
        point, popup = settings_click_target(settings_frame(), item_id, client_bounds())
        if popup is None:
            pointer_at(point)
            return
        hover = pointer_at(point, phases=('hover',))
        assert hover['hovered_window'] == popup, ('Settings option hover did not reach its combo',
                                                item_id, point, popup, hover)
        pointer_at(point, phases=('down', 'up'))

    def key(name, phases=(True, False)):
        for down in phases:
            command(cmd='settings_ui', action='key', key=name, down=down)
            settle()

    def choose_setting(setting, query, option):
        click('search')
        key('Ctrl', (True,))
        key('A')
        key('Ctrl', (False,))
        command(cmd='info_edit_input', action='text', text=query)
        click('result-' + setting)
        if setting != 'theme':
            click(setting)
        click(option)
        return settings_frame()

    def apply_settings():
        if settings_frame()['pending']:
            click('apply')
        observed = settings_frame()
        assert not observed['pending'] and not observed['error'], observed
        return observed

    def open_settings():
        assert not settings_frame()['open'], 'Settings already open'
        command(cmd='ordinary_button_window', module='settings', action='open')
        settings_frame(lambda observed: observed['open'])
        command(cmd='settings_ui', action='resize', width=1120, height=800)
        click('nav-1')

    def close_settings():
        click('close')
        observed = settings_frame()
        if observed['prompt']:
            click('discard')
        settings_frame(lambda value: not value['open'])

    def shot(name):
        path = output / (name + '.png')
        capture(driver, path)
        with Image.open(path) as image:
            left, top, right, bottom = client_bounds()
            assert image.size == (right - left, bottom - top), (path, image.size, client_bounds())
        report['screenshots'].append(str(path))
        return path

    def record(case, **evidence):
        report['checks'].append(dict(case=case, **evidence))

    def check_menu(label, size, committed=None, narrow=False):
        observed = settle(accept=lambda value: (
            abs(value['font_pixels'] - expected_menu_px(size)) < FONT_TOLERANCE
            if not narrow else value['font_pixels'] <= expected_menu_px(size) + FONT_TOLERANCE))
        assert observed['reading_font'] == (size if committed is None else committed), observed
        client = client_bounds()
        check_menu_geometry(observed, client, [translations[name] for name in MENU_IDS], size, narrow)
        record(label, requested_px=size, menu_px=observed['font_pixels'], client=client,
               **{key: observed[key] for key in ('frame', 'bar_top', 'bar_height', 'bar_right',
                   'last_item_right', 'workspace_frame', 'workspace_top', 'work_pos', 'work_size', 'items')})
        return observed

    def close_menus():
        observed = settle()
        if not observed['popups']:
            return
        left, right = observed['last_item_right'], observed['bar_right']
        point_y = observed['bar_top'] + observed['bar_height'] / 2
        for point_x in ((left + right) / 2, *range(math.floor(right - 3), math.ceil(left + 2), -8)):
            if point_x > left + 1 and point_x < right - 1 and all(
                    not (popup['bounds'][0] - 1 <= point_x <= popup['bounds'][2] + 1 and
                         popup['bounds'][1] - 1 <= point_y <= popup['bounds'][3] + 1)
                    for popup in observed['popups']):
                pointer_at((point_x, point_y))
                break
        else:
            raise AssertionError(('No empty space outside the popups for a safe dismissal', observed))
        settle(accept=lambda value: not value['popups'])

    def open_menu(menu_id):
        observed = settle()
        assert not observed['popups'], observed['popups']
        item = next(entry for entry in observed['items'] if entry['label'] == translations[menu_id])
        point = midpoint(item['bounds'])
        pointer_at(point)
        return settle(accept=lambda value: menu_popups(value).get(0, {}).get('label')
                      == translations[menu_id])

    def check_popups(prefix, size, file_menu=True):
        def check(popup):
            assert inside(popup['bounds'], client_bounds()), (popup, client_bounds())
            assert abs(popup['font_pixels'] - expected_popup_px(size)) < FONT_TOLERANCE, popup

        def root_popup(observed):
            opened = menu_popups(observed)
            assert set(opened) == {0}, observed
            parent = opened[0]
            check(parent)
            gap = parent['bounds'][1] - observed['bar_top'] - observed['bar_height']
            assert -1 <= gap <= 12, (gap, observed)
            return parent, gap

        if file_menu:
            observed = open_menu('File')
            parent, gap = root_popup(observed)
            shot(prefix + '-file')
            record(prefix + ' File dropdown', gap_px=gap, popup=parent, frame=observed['frame'])
            close_menus()

        observed = open_menu('View')
        parent, gap = root_popup(observed)
        shot(prefix + '-view')
        for point in hover_points(parent['bounds'], client_bounds()):
            pointer_at(point, phases=('hover',))
            observed = settle()
            opened = menu_popups(observed)
            if opened.get(1, {}).get('label') != translations['WaveformDisplay']:
                continue
            assert set(opened) == {0, 1} and opened[0]['label'] == translations['View'], observed
            for popup in opened.values():
                check(popup)
            parent_left, _, parent_right, _ = opened[0]['bounds']
            child_left, _, child_right, _ = opened[1]['bounds']
            assert min(abs(child_left - parent_right), abs(child_right - parent_left)) <= 12, opened
            shot(prefix + '-cascade')
            record(prefix + ' View → Waveform Display', popups=opened, hover_at=point,
                   gap_px=gap, frame=observed['frame'])
            close_menus()
            return
        raise AssertionError(f'No named Waveform Display cascade inside View popup: {observed}')

    def check_context(size, paths, cascade_label, *, browser=False):
        prefix = f'{"browser" if browser else "results"}-right-{size}'
        expected_view = 'browser' if browser else 'database'
        state = app_state()
        assert state['active_view'] == expected_view, state
        names = {path.name: path for path in paths}
        rows = settings_frame(lambda value: (
            0 <= value['current_frame'] - value['drawn_list_frame'] <= 2 and
            any(name in names for name in value['drawn_list_raw_names'])))
        client = client_bounds()
        row_name, point = context_row_target(rows, command(cmd='dock_state'), client, names)
        assert not settle()['popups'], 'Right click requires a clean popup stack'

        def right_click_at(target, viewport):
            for phase in ('hover', 'down', 'up'):
                command(cmd='folder_search_pointer', phase=phase, button='right',
                        x=target[0] - viewport[0], y=target[1] - viewport[1])
                observed = settle()
            return observed

        observed = right_click_at(point, client)
        if not browser and not observed['popups']:
            selected = app_state()
            expected_path = names[row_name]
            attempt = {'case': prefix, 'target': str(expected_path), 'first_point': point,
                       'first_drawn_frame': rows['drawn_list_frame'],
                       'selected_before': state['selected_path'], 'selected_after': selected['selected_path'],
                       'retried': False}
            report.setdefault('result_right_click_rechecks', []).append(attempt)
            if (selected['active_view'] == 'database' and selected['db_path'] == state['db_path']
                    and Path(state['selected_path']) != expected_path
                    and Path(selected['selected_path']) == expected_path):
                after_click_frame = observed['frame']
                fresh = settings_frame(lambda value: (
                    0 <= value['current_frame'] - value['drawn_list_frame'] <= 2 and
                    value['drawn_list_frame'] > after_click_frame and
                    row_name in value['drawn_list_raw_names']))
                fresh_client = client_bounds()
                _, fresh_point = context_row_target(
                    fresh, command(cmd='dock_state'), fresh_client, {row_name})
                attempt.update(fresh_point=fresh_point, fresh_drawn_frame=fresh['drawn_list_frame'])
                observed = settle()
                if not observed['popups'] and abs(fresh_point[1] - point[1]) >= 6:
                    stable = app_state()
                    assert (stable['active_view'] == 'database' and stable['db_path'] == state['db_path']
                            and Path(stable['selected_path']) == expected_path), (attempt, stable)
                    attempt['retried'] = True
                    point, client = fresh_point, fresh_client
                    observed = right_click_at(point, client)
        if not observed['popups']:
            observed = settle(accept=lambda value: bool(value['popups']))
        assert len(observed['popups']) == 1, observed
        parent = observed['popups'][0]
        assert parent['name'].startswith('##Popup_'), observed
        if not browser:
            selected_path = app_state()['selected_path']
            assert Path(selected_path) == names[row_name], (row_name, selected_path)
        assert observed['reading_font'] == size, observed
        assert inside(parent['bounds'], client), (parent, client)
        parent_shot = shot(prefix + '-parent')
        assert abs(parent['font_pixels'] - expected_popup_px(size)) < FONT_TOLERANCE, (
            size, expected_popup_px(size), parent, str(parent_shot))
        danger = danger_ink(parent_shot, parent['bounds'], client, browser=browser)
        record(prefix + ' parent and red Delete from disk', row=row_name, row_point=point,
               popup=parent, danger=danger, screenshot=str(parent_shot), frame=observed['frame'])
        for point in hover_points(parent['bounds'], client):
            observed = pointer_at(point, phases=('hover',))
            child = next((popup for popup in menu_popups(observed).values()
                          if popup['label'] == cascade_label), None)
            if child is None:
                continue
            assert len(observed['popups']) == 2 and any(
                popup['name'] == parent['name'] and
                abs(popup['font_pixels'] - expected_popup_px(size)) < FONT_TOLERANCE
                for popup in observed['popups']), observed
            assert abs(child['font_pixels'] - expected_popup_px(size)) < FONT_TOLERANCE, child
            assert inside(child['bounds'], client), (child, client)
            screenshot = shot(prefix + '-cascade')
            record(prefix + ' named cascade', popup=child, hover_at=point,
                   screenshot=str(screenshot), frame=observed['frame'])
            break
        else:
            assert browser, ('No Color Mark cascade for the real result row', observed)
            record(prefix + ' cascade', status='not-covered',
                   reason='No visible Send to DAW child; it requires configured DAW targets',
                   structural=check_browser_context_source())
        close_menus()

    def app_state():
        state = command(cmd='get_state')
        paths = (state['db_path'], state['pending_db_path'], *state['recent_db_paths'])
        for path in filter(None, paths):
            isolated_path(Path(path), root)
        return state

    def create_library(library, expected_names):
        initial = app_state()
        if not initial['new_db_dialog_open']:
            command(cmd='info_edit_input', action='new_db_dialog')
        wait_for(app_state, lambda state: state['new_db_dialog_open'], 'New library dialog', 20)
        name = 'TopMenuFontQA-' + uuid.uuid4().hex[:8]
        command(cmd='info_edit_input', action='new_db_name', text=name)
        time.sleep(.15)
        command(cmd='info_edit_input', action='new_db_create')
        state = wait_for(app_state, lambda value: value['db_path'] and
                         Path(value['db_path']) != Path(initial['db_path']) and not value['new_db_dialog_open']
                         and not value['db_switch_in_progress'], 'New isolated library', 60)
        db_path = isolated_path(Path(state['db_path']), root)
        command(cmd='scan_folder', path=str(library), recursive=True,
                cache_waveforms=False, build_similarity_index=False)
        count = len(expected_names)
        wait_for(app_state, lambda value: value['total_count'] == count
                 and not value['scan_in_progress'] and not value['scan_completion_pending'],
                 'Synthetic WAV scan', 120)

        def scan_dialog():
            return command(cmd='scan_dialog_ui', action='state')

        observed = scan_dialog()
        if observed['windows']:
            observed = wait_for(scan_dialog, lambda value: not value['windows'] or any(
                item['id'] == 'scan.result.done' for item in value['items']), 'Scan result', 30)
            for item in observed['items']:
                if item['id'] == 'scan.result.done':
                    x, y = midpoint(item['bounds'])
                    for phase in ('hover', 'down', 'up'):
                        command(cmd='scan_dialog_ui', action='pointer', phase=phase, x=x, y=y)
                        settle()
                    break
            wait_for(scan_dialog, lambda value: not value['windows'], 'Closed scan result', 20)
        wait_for(app_state, lambda value: value['result_count'] == count
                 and not value['db_switch_in_progress'], 'Synthetic WAV results', 30)
        report['library'] = {'path': str(db_path), 'audio_dir': str(library), 'wav_names': expected_names}
        return db_path

    def check_fonts(label, reading, list_size, expected_names):
        observed = settings_frame(lambda value: value['drawn_list_frame'] >= value['current_frame'] - 2)
        assert observed['live_font'] == reading and observed['live_list_font'] == list_size, observed
        assert abs(observed['reading_font_pixels'] - reading) < FONT_TOLERANCE, observed
        assert abs(observed['list_font_pixels'] - list_size) < FONT_TOLERANCE, observed
        assert abs(observed['drawn_list_font_pixels'] - list_size) < FONT_TOLERANCE, observed
        assert {name.casefold() for name in expected_names} <= {
            name.casefold() for name in observed['drawn_list_raw_names']}, observed
        record(label, reading=reading, list=list_size, requested_list_px=observed['list_font_pixels'],
               drawn_list_px=observed['drawn_list_font_pixels'], drawn_names=observed['drawn_list_names'],
               list_frame=observed['drawn_list_frame'])

    def stop():
        nonlocal process, identity
        command(cmd='info_edit_input', action='tray_exit')
        assert win32event.WaitForSingleObject(process, 15000) == win32con.WAIT_OBJECT_0, 'QA exit timed out'
        assert win32process.GetExitCodeProcess(process) == 0, 'QA EXE did not shut down cleanly'
        process.Close()
        process = identity = None

    try:
        runtime.mkdir(parents=True, exist_ok=True)
        library = existing_fixture or root / 'library'
        if existing_fixture is None:
            make_library(library)
        fixture_paths = audio_files(library)
        expected_names = [path.name for path in fixture_paths]
        desktop = PrivateDesktop()
        start()
        db_path = create_library(library, expected_names)
        resize(*WIDE_SIZE)
        initial = settings_frame()
        language = ('zh', 'en', 'ja')[initial['rendered_language']]
        locale_file = Path(__file__).resolve().parents[1] / 'locale/harken.ui.strings.json'
        strings = {entry['id']: entry[language] for entry in
                   json.loads(locale_file.read_text(encoding='utf-8'))['entries'] if language in entry}
        translations = {key.removeprefix('Menu_'): value for key, value in strings.items()
                        if key.startswith('Menu_')}
        browser_dir = root / ('browser-menu-' + uuid.uuid4().hex[:8])
        browser_dir.mkdir()
        browser_wav = browser_dir / fixture_paths[0].name
        shutil.copyfile(fixture_paths[0], browser_wav)
        all_fixtures = [*fixture_paths, browser_wav]
        original_hashes = {path: hashlib.sha256(path.read_bytes()).hexdigest() for path in all_fixtures}
        bookmark = command(cmd='folder_shortcut')['shortcut_count']
        assert command(cmd='folder_shortcut', bookmark_path=str(browser_dir))['shortcut_count'] == bookmark + 1
        initial_results = command(cmd='get_results', limit=len(fixture_paths))['items']
        assert {Path(row['path']) for row in initial_results} == set(fixture_paths), initial_results

        open_settings()
        if existing_fixture is not None:
            if initial['live_font'] != 18:
                choose_setting('systemFont', 'system font', f'option-{FONT_OPTIONS.index(18)}')
            if initial['live_list_font'] != 18:
                choose_setting('listFont', 'list font', f'option-{LIST_OPTIONS.index(18)}')
        choose_setting('theme', 'theme', 'stock-0')
        assert apply_settings()['live_theme'] == 0
        close_settings()
        check_fonts('18 px baseline renders actual list rows', 18, 18, expected_names)

        previous = 18
        for size in MENU_SIZES:
            if size != previous:
                open_settings()
                draft = choose_setting('systemFont', 'system font', f'option-{FONT_OPTIONS.index(size)}')
                assert draft['draft_font'] == size and draft['live_font'] == previous and draft['pending'], draft
                check_menu(f'{size} px system font previews before Apply', size, committed=previous)
                apply_settings()
                close_settings()
            check_fonts(f'{size} px system font leaves list font at 18 px', size, 18, expected_names)
            check_context(size, fixture_paths, strings['Ctx_ColorMark'])
            browser_state = command(cmd='folder_shortcut', open_index=bookmark)
            assert browser_state['mode'] == 'browser' and Path(browser_state['browser_path']) == browser_dir, browser_state
            try:
                wait_for(lambda: command(cmd='folder_shortcut'), lambda value: value['entries'] >= 1,
                         'Isolated browser WAV did not load', 10)
                settings_frame(lambda value: 0 <= value['current_frame'] - value['drawn_list_frame'] <= 2
                               and browser_wav.name in value['drawn_list_raw_names'])
            except TimeoutError as error:
                record(f'browser-right-{size}', status='not-covered', reason=str(error),
                       structural=check_browser_context_source())
            else:
                check_context(size, [browser_wav], strings['Ctx_SendToDAW'], browser=True)
            command(cmd='set_view_mode', mode='database')
            wait_for(app_state, lambda value: value['active_view'] == 'database'
                     and value['result_count'] == len(fixture_paths), 'Restore isolated result rows', 30)
            assert command(cmd='get_results', limit=len(fixture_paths))['items'] == initial_results
            assert {path: hashlib.sha256(path.read_bytes()).hexdigest() for path in all_fixtures} == original_hashes
            record(f'Context {size} px: no delete, result rows and WAV contents unchanged')
            if args.context_menus_only:
                previous = size
                continue
            check_menu(f'{size} px system font in wide client', size)
            shot(f'wide-{size}-bar')
            check_popups(f'wide-{size}', size)
            if size > 18:
                for width in (900, 640):
                    resize(width, 700)
                    check_menu(f'{size} px system font at {width}px client width', size, narrow=True)
                    shot(f'narrow-{width}-{size}-bar')
                    check_popups(f'narrow-{width}-{size}', size, file_menu=False)
                resize(*WIDE_SIZE)
                check_menu(f'{size} px menu recovers after widening', size)
            previous = size

        for index, (theme, name) in enumerate(() if args.context_menus_only else STOCK_THEMES):
            open_settings()
            choose_setting('theme', 'theme', f'stock-{index}')
            assert apply_settings()['live_theme'] == theme, (name, theme)
            close_settings()
            observed = check_menu(f'32 px system font on {name}', 32)
            assert observed['theme'] == theme, observed
            shot(f'theme-{name}-bar')
            check_popups(f'theme-{name}', 32, file_menu=False)

        if not args.context_menus_only:
            open_settings()
            choose_setting('theme', 'theme', 'stock-0')
            apply_settings()
            choose_setting('listFont', 'list font', f'option-{LIST_OPTIONS.index(24)}')
            apply_settings()
            close_settings()
            check_fonts('Changing list font to its 24 px cap leaves system at 32 px', 32, 24, expected_names)
            check_menu('Changing list font leaves the wide menu at 25 px', 32)
            stop()

            start()
            resize(*WIDE_SIZE)
            wait_for(app_state, lambda state: Path(state['db_path']) == db_path
                     and state['result_count'] == len(expected_names), 'Same fixture after restart', 60)
            check_fonts('Real restart retains 32 px system and 24 px drawn list font', 32, 24, expected_names)
            check_menu('Real restart retains 25 px wide menu', 32)
            shot('restart-32-bar')
            check_popups('restart-32', 32, file_menu=False)
        stop()
        report['result'] = 'passed'
    except Exception:
        report['result'] = 'failed'
        report['error'] = traceback.format_exc()
        print(report['error'], file=sys.stderr)
    finally:
        if process is not None:
            try:
                if win32event.WaitForSingleObject(process, 0) == win32con.WAIT_TIMEOUT:
                    win32process.TerminateProcess(process, 1)
                    if win32event.WaitForSingleObject(process, 15000) != win32con.WAIT_OBJECT_0:
                        raise RuntimeError('Owned QA process did not exit after termination')
            except Exception:
                report['cleanup_error'] = traceback.format_exc()
            finally:
                process.Close()
        if desktop is not None:
            try:
                desktop.close()
            except Exception:
                report['result'] = 'failed'
                report['desktop_error'] = traceback.format_exc()
        lock.unlink()
        report['finished_at'] = datetime.now(timezone.utc).isoformat()
        (output / 'report.json').write_text(
            json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
    return 0 if report['result'] == 'passed' else 1


if __name__ == '__main__':
    if len(sys.argv) == 3 and sys.argv[1] == '--probe-idle-root':
        try:
            probe_idle_root(Path(sys.argv[2]))
        except Exception as error:
            sys.exit(str(error))
        sys.exit(0)
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exe', type=Path, required=True)
    parser.add_argument('--build-cache', type=Path, required=True, help='CMakeCache.txt from the matching build')
    parser.add_argument('--isolation-root', type=Path, required=True, help='Exact hard-pinned build root')
    parser.add_argument('--output', type=Path, required=True, help='New directory outside the isolation root')
    parser.add_argument('--fixture-dir', type=Path, help='Existing synthetic WAV folder under the pinned root')
    parser.add_argument('--preflight-only', action='store_true', help='Read-only checks; do not start the EXE')
    parser.add_argument('--context-menus-only', action='store_true',
                        help='Check the three settings sizes and both right-click menus; reuse prior top-menu evidence')
    try:
        sys.exit(main(parser.parse_args()))
    except (ValueError, OSError, RuntimeError) as error:
        parser.exit(1, f'{parser.prog}: {error}\n')
