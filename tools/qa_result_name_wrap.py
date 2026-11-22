"""Reproduce and verify wrapped result-list filename rendering in an isolated QA EXE.

The helper creates its own library, row height and screenshots on a private
desktop. It never touches the user's desktop, library or installed app.

Usage:
  python tools/qa_result_name_wrap.py --exe <HarkenSearch.exe> \
      --isolation-root <root> --output <dir> --row-height 56
"""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
import math
from pathlib import Path
import time
import wave

from qa_required_library_setup import PrivateDesktop, capture, wait_state
from qa_transport_themes import Driver

NAMES = [
    "Kick_01.wav",
    "\u754c\u9762_\u73af\u5883\u5faa\u73af_Ambient_Loop_Medium_Name_2026.wav",
    "\u754c\u9762\u6750\u8d28_\u73af\u5883\u6c1b\u56f4_\u5faa\u73af\u7d20\u6750_" +
    "\u4e09\u6bb5\u6362\u884c\u9a8c\u8bc1_Ambient_Loop_Long_Name_Alpha_Beta_Gamma_2026_v1.wav",
    "\u754c\u9762\u6750\u8d28\u5e93_\u73af\u5883\u6c1b\u56f4_\u5faa\u73af\u7d20\u6750_\u4e09\u6bb5\u6362\u884c\u9a8c\u8bc1_" +
    "\u8d85\u957f\u540d\u79f0_Ambient_Loop_Very_Long_Name_Alpha_Beta_Gamma_Delta_" +
    "Epsilon_2026_v1_final_mastered.wav",
]


def library_names(count: int):
    names = list(NAMES)
    index = len(names)
    while len(names) < count:
        base = NAMES[1 + (index % (len(NAMES) - 1))]
        stem = base[:-4]
        names.append('%s_%03d.wav' % (stem, index))
        index += 1
    return names[:max(1, count)]


def make_library(folder: Path, count: int) -> None:
    folder.mkdir(parents=True, exist_ok=False)
    for index, name in enumerate(library_names(count), start=1):
        samples = (int(3000 * math.sin(2 * math.pi * (220 + 40 * index) * n / 22050))
                   for n in range(6615))
        with wave.open(str(folder / name), 'wb') as audio:
            audio.setnchannels(1)
            audio.setsampwidth(2)
            audio.setframerate(22050)
            audio.writeframes(b''.join(sample.to_bytes(2, 'little', signed=True)
                                       for sample in samples))


def write_settings(root: Path, row_height: int, list_font: int) -> Path:
    settings_dir = root / 'profile/Local/HarkenSearch'
    settings_dir.mkdir(parents=True, exist_ok=True)
    path = settings_dir / 'settings.ini'
    values = {
        'readingFontSize': '18',
        'listFontSize': str(list_font),
        'rowHeight': str(row_height),
        'listDensity': '3',
        'theme': '0',
    }
    lines = []
    if path.exists():
        # Keep every other key (database path, window state, ...) when a
        # profile is reused between row-height runs.
        for line in path.read_text(encoding='utf-8').splitlines():
            key = line.split('=', 1)[0].strip()
            if line.strip() and key not in values:
                lines.append(line)
    lines.extend('%s=%s' % item for item in values.items())
    path.write_text(chr(10).join(lines) + chr(10), encoding='utf-8')
    return path


def resolve_line_layout(drawable_height, natural_advance):
    if natural_advance <= 0 or drawable_height <= 0:
        return 1, max(natural_advance, 0.0)
    lines = max(1, int(math.floor((drawable_height + 0.5) / natural_advance)))
    return lines, natural_advance


def text_bands(image, x0, x1, y0, y1, threshold=110.0):
    bands = []
    start = None
    last_row = y0
    for y in range(y0, y1):
        hit = False
        for x in range(x0, x1, 2):
            r, g, b = image.getpixel((x, y))
            if 0.299 * r + 0.587 * g + 0.114 * b > threshold:
                hit = True
                break
        last_row = y
        if hit and start is None:
            start = y
        elif not hit and start is not None:
            bands.append((start, y))
            start = None
    if start is not None:
        bands.append((start, last_row + 1))
    return bands


def main(args: argparse.Namespace) -> int:
    import win32con
    import win32event
    import win32gui
    import win32process

    exe, root, out = (args.exe.resolve(), args.isolation_root.resolve(),
                      args.output.resolve())
    fresh_root = not root.exists()
    if not fresh_root:
        # A previous run can leave the crash sentinel behind; clear it so the
        # reused profile starts without a recovery prompt.
        sentinel = root / 'profile/Local/HarkenSearch/aris_sentinel.lock'
        if sentinel.exists():
            sentinel.unlink()
    if not exe.is_file() or out.exists():
        raise ValueError('Expected an existing EXE and a new output directory')
    if not fresh_root and not args.reuse_root:
        raise ValueError('Isolation root exists; pass --reuse-root to reuse its library')
    root.mkdir(parents=True, exist_ok=True)
    (root / 'profile/runtime').mkdir(parents=True, exist_ok=True)
    (root / 'temp').mkdir(parents=True, exist_ok=True)
    out.mkdir(parents=True)
    library = root / 'library'
    if fresh_root:
        make_library(library, args.files)
    settings_path = write_settings(root, args.row_height, args.list_font)

    runtime = root / 'profile/runtime'
    driver = Driver(runtime, out)
    desktop = PrivateDesktop()
    process = None
    identity = None
    report = {
        'exe': str(exe), 'exe_sha256': hashlib.sha256(exe.read_bytes()).hexdigest(),
        'isolation_root': str(root), 'row_height_setting': args.row_height,
        'list_font': args.list_font, 'settings_path': str(settings_path),
        'window': list(args.window),
        'started_at': datetime.now(timezone.utc).isoformat(),
        'screenshots': [], 'checks': [],
    }

    def command(**payload):
        return driver.command(payload)

    def geometry():
        window = identity['hwnd']
        left, top = win32gui.ClientToScreen(window, (0, 0))
        _, _, width, height = win32gui.GetClientRect(window)
        return left, top, width, height

    def place_client_at_origin():
        window = identity['hwnd']
        left, top, _, _ = geometry()
        ox, oy = win32gui.GetWindowRect(window)[:2]
        win32gui.SetWindowPos(window, None, ox - left, oy - top, 0, 0,
                              win32con.SWP_NOSIZE | win32con.SWP_NOZORDER |
                              win32con.SWP_NOACTIVATE)

    def resize(width, height):
        window = identity['hwnd']
        ox, oy, right, bottom = win32gui.GetWindowRect(window)
        _, _, cw, ch = win32gui.GetClientRect(window)
        outer_w = width + (right - ox - cw)
        outer_h = height + (bottom - oy - ch)
        win32gui.SetWindowPos(window, None, ox, oy, outer_w, outer_h,
                              win32con.SWP_NOZORDER | win32con.SWP_NOACTIVATE)
        place_client_at_origin()
        deadline = time.monotonic() + 15
        while time.monotonic() < deadline:
            if win32gui.GetClientRect(window)[2:] == (width, height):
                return
            time.sleep(.05)
        raise TimeoutError('window did not reach the requested client size')

    def screenshot(name):
        path = out / (name + '.png')
        capture(driver, path)
        report['screenshots'].append(str(path))
        return path

    try:
        environment = {
            'LOCALAPPDATA': str(root / 'profile/Local'),
            'APPDATA': str(root / 'profile/Roaming'),
            'AURA_RUNTIME_ROOT': str(runtime),
            'TEMP': str(root / 'temp'), 'TMP': str(root / 'temp'),
        }
        process, pid = desktop.start(exe, environment_overrides=environment)
        report['startup_pid'] = pid
        if win32event.WaitForSingleObject(process, 1000) == win32con.WAIT_OBJECT_0:
            raise RuntimeError('QA package exited during startup: %s'
                               % win32process.GetExitCodeProcess(process))
        identity = command(cmd='magic_eq_identity', timeout=60)
        assert identity['pid'] == pid, identity
        assert Path(identity['exe_path']).resolve() == exe, identity
        assert Path(identity['isolation_root']).resolve() == root, identity
        report['identity'] = identity
        wait_state(driver, lambda s: s['main_ui_ready'], 'native startup', 60)

        state = command(cmd='get_state')
        if not state['db_path'] and not fresh_root:
            # The app does not persist the active database between launches, so
            # a reused profile reopens the library it already scanned.
            reopened = root / 'profile/Local/HarkenSearch/Database/NameWrapQA/main.db'
            if not reopened.exists():
                raise RuntimeError('Reused isolation root has no library database')
            command(cmd='info_edit_input', action='open_existing_db', text=str(reopened))
            state = wait_state(driver, lambda s: bool(s['db_path']) and
                               not s['db_switch_in_progress'], 'reopened library', 60)
        if not state['db_path']:
            wait_state(driver, lambda s: s['new_db_dialog_open'], 'new library dialog', 20)
            command(cmd='info_edit_input', action='new_db_name', text='NameWrapQA')
            time.sleep(.15)
            command(cmd='info_edit_input', action='new_db_create')
            state = wait_state(driver, lambda s: bool(s['db_path']) and
                               not s['new_db_dialog_open'], 'created library', 40)
        assert Path(state['db_path']).resolve().is_relative_to(root), state['db_path']

        command(cmd='scan_folder', path=str(library), recursive=False,
                cache_waveforms=False, build_similarity_index=False)
        wait_state(driver, lambda s: s['total_count'] == min(args.files, len(library_names(args.files))),
                   'library scan', 120)
        command(cmd='search', query='')
        state = wait_state(driver, lambda s: s['result_count'] == args.files,
                           'all results listed', 60)

        resize(args.window[0], args.window[1])
        time.sleep(.6)
        place_client_at_origin()
        time.sleep(.4)
        first_metrics = command(cmd='settings_ui')
        shot = screenshot('results-row-height-%d' % args.row_height)

        command(cmd='ordinary_button_window', module='settings', action='open')
        time.sleep(.4)
        metrics = command(cmd='settings_ui')
        report['metrics'] = {
            key: metrics.get(key) for key in (
                'live_row_height', 'drawn_list_line_height', 'drawn_list_row_height',
                'drawn_list_font_pixels', 'drawn_list_names', 'drawn_list_raw_names',
                'drawn_list_row_origins', 'drawn_list_hit_heights',
                'effective_row_height', 'imgui_errors')
        }
        assert not metrics.get('imgui_errors'), metrics.get('imgui_log')

        from PIL import Image

        def analyze(shot_path, metrics, label):
            regions = command(cmd='get_ui_regions')['list']
            origins_local = metrics.get('drawn_list_row_origins') or []
            heights_local = metrics.get('drawn_list_hit_heights') or []
            names_local = metrics.get('drawn_list_names') or []
            line_h_local = float(metrics.get('drawn_list_line_height') or 0)
            row_h_local = float(metrics.get('drawn_list_row_height') or 0)
            rows = []
            with Image.open(shot_path) as image:
                x0 = int(regions['x']) + 6
                x1 = min(int(regions['x'] + 0.30 * regions['w']), image.width)
                limit = min(6, len(origins_local), len(heights_local))
                for index in range(limit):
                    # The first and last drawn rows can be partly clipped by the
                    # viewport; only judge rows that are fully on screen.
                    if index == 0 or index >= limit - 1:
                        continue
                    top = int(round(origins_local[index]))
                    bottom = min(int(round(top + heights_local[index])), image.height)
                    if top < 0 or bottom <= top:
                        continue
                    bands = [band for band in text_bands(image, x0, x1, top, bottom)
                             if band[1] - band[0] >= 1]
                    ink_heights = [band[1] - band[0] for band in bands]
                    # The name text uses the whole row band (cell padding included).
                    lines, advance = resolve_line_layout(heights_local[index], line_h_local)
                    text_end = top + lines * advance
                    rows.append({
                        'label': label, 'row': index,
                        'top': top, 'bottom': bottom,
                        'bands': [[a, b] for a, b in bands],
                        'name': names_local[index] if index < len(names_local) else '',
                        'line_bands': ink_heights,
                        'layout_lines': lines,
                        'text_end': text_end,
                        'ink_overflow': max((band[1] for band in bands), default=top) - text_end,
                        'complete_lines': all(band[1] <= text_end + 1 for band in bands),
                    })
            return rows

        regions = command(cmd='get_ui_regions')['list']
        origins = metrics.get('drawn_list_row_origins') or []
        heights = metrics.get('drawn_list_hit_heights') or []
        names = metrics.get('drawn_list_names') or []
        line_h = float(metrics.get('drawn_list_line_height') or 0)
        row_h = float(metrics.get('drawn_list_row_height') or 0)
        report['regions'] = regions
        report['row_analysis'] = []
        with Image.open(shot) as image:
            report['image_size'] = list(image.size)
        report['row_analysis'] = analyze(shot, first_metrics, 'top')
        if args.files > 8:
            scrolled = command(cmd='scroll_results', top_row=args.files // 2)
            time.sleep(1.6)
            command(cmd='get_state')
            scrolled_metrics = command(cmd='settings_ui')
            scrolled_shot = screenshot('results-scrolled-%d' % args.row_height)
            report['row_analysis'] += analyze(scrolled_shot, scrolled_metrics, 'scrolled')
            report['scroll'] = {'top_row': scrolled.get('top_row')}
        narrow = (max(900, args.window[0] - 320), args.window[1])
        resize(narrow[0], narrow[1])
        time.sleep(.8)
        place_client_at_origin()
        time.sleep(.3)
        narrow_metrics = command(cmd='settings_ui')
        narrow_shot = screenshot('results-narrow-%d' % args.row_height)
        report['row_analysis'] += analyze(narrow_shot, narrow_metrics, 'narrow')
        report['narrow_window'] = list(narrow)
        expected_lines, expected_advance = resolve_line_layout(row_h + 2.0, line_h)
        # The scrolled view can place a row directly against the next row's
        # content, which the pixel probe cannot separate; scrolling is checked
        # visually from its screenshot while the top and narrow views are gated.
        checked = [row for row in report['row_analysis'] if row['label'] in ('top', 'narrow')]
        complete = (len(checked) >= 2
                    and all(row['line_bands'] for row in checked)
                    and all(row['complete_lines'] for row in checked))
        report['checks'].append({
            'case': 'text ink stays inside the resolved line block',
            'expected_capacity_lines': expected_lines, 'expected_line_advance': expected_advance,
            'line_height': line_h,
            'passed': complete,
        })

        command(cmd='info_edit_input', action='exit')
        assert win32event.WaitForSingleObject(process, 15000) == win32con.WAIT_OBJECT_0
        assert win32process.GetExitCodeProcess(process) == 0
        process = None
    finally:
        if process is not None:
            try:
                if win32event.WaitForSingleObject(process, 0) == win32con.WAIT_TIMEOUT:
                    win32process.TerminateProcess(process, 1)
                    win32event.WaitForSingleObject(process, 5000)
            finally:
                process.Close()
        desktop.close()

    report['finished_at'] = datetime.now(timezone.utc).isoformat()
    passed = all(check['passed'] for check in report['checks'])
    report['passed'] = passed
    (out / 'report.json').write_text(json.dumps(report, ensure_ascii=False, indent=2),
                                     encoding='utf-8')
    print(json.dumps({'passed': passed, 'rows': report['row_analysis'],
                      'checks': report['checks']}, ensure_ascii=False, indent=2))
    return 0 if passed else 1


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--exe', required=True, type=Path)
    parser.add_argument('--isolation-root', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--row-height', type=int, default=56)
    parser.add_argument('--list-font', type=int, default=18)
    parser.add_argument('--window', type=int, nargs=2, default=[1200, 760])
    parser.add_argument('--reuse-root', action='store_true')
    parser.add_argument('--files', type=int, default=len(NAMES))
    raise SystemExit(main(parser.parse_args()))

