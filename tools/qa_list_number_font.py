"""Verify the result-list numeric cells and column headers follow list font size.

Runs a pinned isolated preview EXE on a private desktop, builds a small synthetic
library inside the isolation profile, then changes "list font size" and records
the actual list font, list mono (numeric cells), list emphasis (headers) and
filename draw sizes. Screenshots are captured for each size.
"""
from pathlib import Path
import argparse
import hashlib
import json
import math
import struct
import time
import uuid
import wave

from PIL import Image

from qa_required_library_setup import PrivateDesktop, capture, wait_state
from qa_transport_themes import Driver

FONTS = (10, 11, 12, 13, 14, 16, 18, 20, 22, 24)
FIXTURES = (
    ("tone-48k-stereo.wav", 48000, 2, 6.5),
    ("tone-44k-mono.wav", 44100, 1, 2.25),
    ("tone-22k-mono.wav", 22050, 1, 1.1),
)


def write_fixtures(folder: Path) -> list[Path]:
    folder.mkdir(parents=True, exist_ok=True)
    paths = []
    for name, rate, channels, seconds in FIXTURES:
        path = folder / name
        paths.append(path)
        if path.exists():
            continue
        frames = int(rate * seconds)
        with wave.open(str(path), "wb") as f:
            f.setparams((channels, 2, rate, frames, "NONE", "not compressed"))
            block = bytearray()
            for i in range(frames):
                value = int(1200 * math.sin(i * 2 * math.pi * 440 / rate))
                for _ in range(channels):
                    block += struct.pack("<h", value)
            f.writeframes(bytes(block))
    return paths


def run(args):
    import win32event
    import win32con
    import win32process

    exe = args.exe.resolve()
    root = args.isolation_root.resolve()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    runtime = root / 'profile/runtime'
    runtime.mkdir(parents=True, exist_ok=True)
    # The pinned preview build is fail-closed: every fixture stays inside the root.
    fixture_dir = root / 'fixtures'
    fixtures = write_fixtures(fixture_dir)

    driver = Driver(runtime, out)
    desktop = PrivateDesktop()
    report = dict(exe=str(exe), sha256=hashlib.sha256(exe.read_bytes()).hexdigest(), checks=[], sizes={})
    process = None
    identity = None
    verified = False

    def cmd(**payload):
        return driver.command(payload)

    def state():
        return cmd(cmd='get_state')

    def check(label, **details):
        report['checks'].append(dict(case=label, **details))
        print(label, flush=True)

    def start():
        nonlocal process, identity, verified
        verified = False
        process, pid = desktop.start(exe)
        identity = driver.command({'cmd': 'magic_eq_identity'}, timeout=90)
        assert identity['pid'] == pid, identity
        assert Path(identity['exe_path']).resolve() == exe, identity
        assert Path(identity['isolation_root']).resolve() == root, identity
        verified = True
        return wait_state(driver, lambda s: s['main_ui_ready'], 'main UI must be ready', 60)

    def stop():
        nonlocal process
        if process is None:
            return
        if verified:
            cmd(cmd='info_edit_input', action='tray_exit')
            assert win32event.WaitForSingleObject(process, 20000) == win32con.WAIT_OBJECT_0
            assert win32process.GetExitCodeProcess(process) == 0
        elif win32event.WaitForSingleObject(process, 0) == win32con.WAIT_TIMEOUT:
            win32process.TerminateProcess(process, 1)
        process.Close()
        process = None

    def ensure_library():
        s = state()
        if s.get('db_path'):
            return s
        if s.get('new_db_dialog_open'):
            cmd(cmd='info_edit_input', action='new_db_name', text='ListNumberFont-' + uuid.uuid4().hex[:6])
            cmd(cmd='info_edit_input', action='new_db_create')
            return wait_state(driver, lambda v: bool(v.get('db_path')) and not v.get('new_db_dialog_open'),
                              'library must be created', 30)
        return s

    def scan():
        started = cmd(cmd='scan_folder', path=str(fixture_dir), recursive=True, scan_strategy='fast')
        assert started.get('started'), started
        deadline = time.monotonic() + 180
        while time.monotonic() < deadline:
            s = state()
            if s.get('total_count', 0) >= len(FIXTURES):
                status = cmd(cmd='scan_status')
                if not status.get('scanning'):
                    return s
            time.sleep(0.2)
        raise AssertionError('fixture scan did not finish')

    def settings_state():
        return cmd(cmd='settings_ui')

    def settle():
        first = settings_state()['frame']
        deadline = time.monotonic() + 8
        while time.monotonic() < deadline:
            v = settings_state()
            if v['frame'] > first + 2:
                return v
            time.sleep(0.04)
        raise AssertionError('no fresh settings frame')

    def item(item_id):
        return next(i for i in settle()['items'] if i['id'] == item_id)

    def pointer(x, y):
        import win32gui
        ox, oy = win32gui.ClientToScreen(identity['hwnd'], (0, 0))
        for phase in ('hover', 'down', 'up'):
            cmd(cmd='theme_schedule', action='pointer', phase=phase, x=x - ox, y=y - oy)
            settle()

    def click(item_id):
        a, b, c, d = item(item_id)['bounds']
        pointer((a + c) / 2, (b + d) / 2)

    def key(name):
        for down in (True, False):
            cmd(cmd='settings_ui', action='key', key=name, down=down)
            settle()

    def query(text):
        click('search')
        cmd(cmd='settings_ui', action='key', key='Ctrl', down=True)
        settle()
        key('A')
        cmd(cmd='settings_ui', action='key', key='Ctrl', down=False)
        settle()
        cmd(cmd='info_edit_input', action='text', text=text)
        settle()

    def open_settings():
        if not settings_state()['open']:
            cmd(cmd='ordinary_button_window', module='settings', action='open')
            settle()
        cmd(cmd='settings_ui', action='resize', width=1120, height=800)
        settle()
        click('nav-1')

    def choose_list(size):
        open_settings()
        query('list font')
        click('result-listFont')
        click('listFont')
        click('option-' + str(FONTS.index(size)))
        settle()

    def apply():
        if settings_state()['pending']:
            click('apply')
            settle()

    def measure(size):
        open_settings()
        choose_list(size)
        apply()
        v = settings_state()
        click('done')
        settle()
        main = state()
        metrics = dict(
            live_list_font=v['live_list_font'],
            list_font_pixels=v['list_font_pixels'],
            list_mono_font_pixels=v['list_mono_font_pixels'],
            list_emphasis_font_pixels=v['list_emphasis_font_pixels'],
            drawn_list_font_pixels=v['drawn_list_font_pixels'],
            drawn_numeric_font_pixels=main.get('drawn_list_numeric_font_pixels'),
            drawn_header_font_pixels=main.get('drawn_list_header_font_pixels'),
            drawn_numeric_sample=main.get('drawn_list_numeric_sample'),
            drawn_header_sample=main.get('drawn_list_header_sample'),
            duration_column_width=main.get('drawn_duration_column_width'),
            sample_rate_column_width=main.get('drawn_sample_rate_column_width'),
            total_count=main.get('total_count'),
        )
        capture(driver, out / f'main-listfont-{size}.png')
        return metrics

    try:
        start()
        ensure_library()
        scan()
        before = state()
        assert before['total_count'] >= len(FIXTURES), before.get('total_count')

        for size in (18, 24, 10):
            metrics = measure(size)
            report['sizes'][str(size)] = metrics
            assert metrics['live_list_font'] == size, metrics
            assert abs(metrics['list_mono_font_pixels'] - (size + 1)) <= 0.25, metrics
            assert abs(metrics['list_emphasis_font_pixels'] - size) <= 0.25, metrics
            assert abs(metrics['drawn_list_font_pixels'] - size) <= 0.25, metrics
            # Draw-time probes: what the numeric cell and the header actually rendered with.
            assert abs(metrics['drawn_numeric_font_pixels'] - (size + 1)) <= 0.25, metrics
            assert abs(metrics['drawn_header_font_pixels'] - size) <= 0.25, metrics
            # A fresh layout must still fit the widest values at this font.
            assert metrics['sample_rate_column_width'] >= 6 * (size + 1) * 0.5, metrics
            assert metrics['duration_column_width'] >= 11 * (size + 1) * 0.5, metrics
            check(f'list font {size}px: drawn numeric, header fonts and column widths follow the list size',
                  **{k: metrics[k] for k in ('drawn_numeric_font_pixels', 'drawn_header_font_pixels',
                                             'duration_column_width', 'sample_rate_column_width')})

        mono_sizes = {k: round(v['list_mono_font_pixels'], 2) for k, v in report['sizes'].items()}
        assert len(set(mono_sizes.values())) == len(mono_sizes), mono_sizes
        check('numeric font tracks the list font size across 10/18/24px', mono_sizes=mono_sizes)
        report['passed'] = True
    except Exception:
        import traceback
        report['error'] = traceback.format_exc()
        print(report['error'], flush=True)
        try:
            if verified:
                capture(driver, out / 'failure.png')
        except Exception:
            pass
        raise
    finally:
        if process is not None:
            try:
                stop()
            except Exception:
                if win32event.WaitForSingleObject(process, 0) == win32con.WAIT_TIMEOUT:
                    win32process.TerminateProcess(process, 1)
                report['forcedTermination'] = True
        desktop.close()
        (out / 'report.json').write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--exe', type=Path, required=True)
    parser.add_argument('--isolation-root', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    run(parser.parse_args())
