"""Verify results home/Reset Layout in a pinned preview EXE on a private desktop.

Requires pywin32 and Pillow. Uses the production title drag and menu input;
never switches desktops or sends global mouse/keyboard input.
"""
import argparse
import ctypes
from ctypes import wintypes
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import time
import wave

from qa_transport_themes import Driver, window_api
from qa_required_library_setup import PrivateDesktop, capture, wait_state
from qa_scan_dialog_simple import Smoke


def run(args):
    import win32con
    import win32event
    import win32gui
    import win32process
    exe, root, out = args.exe.resolve(), args.isolation_root.resolve(), args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    runtime = root / 'profile/runtime'
    runtime.mkdir(parents=True, exist_ok=True)
    driver = Driver(runtime, out)
    desktop = PrivateDesktop()
    process = None
    report = {'exe': str(exe), 'exe_sha256': hashlib.sha256(exe.read_bytes()).hexdigest(),
              'started_at': datetime.now(timezone.utc).isoformat(), 'cases': []}

    def cmd(**payload):
        return driver.command(payload)

    def settled(predicate=lambda s: True):
        first = cmd(cmd='dock_state')
        deadline = time.monotonic() + 12
        last = first
        while time.monotonic() < deadline:
            last = cmd(cmd='dock_state')
            if last['frame'] >= first['frame'] + 8 and predicate(last):
                return last
            time.sleep(.08)
        raise AssertionError(last)

    def record(name, state):
        path = out / (name + '.png')
        capture(driver, path)
        report['cases'].append({'name': name, 'state': state, 'screenshot': str(path)})
        (out / 'report.json').write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
        print(json.dumps({'case': name, 'panel': state['panels']['search-results'],
                          'layout': state['layout']}, ensure_ascii=False), flush=True)

    def begin():
        for phase in ('hover', 'down'):
            cmd(cmd='dock_pointer', phase=phase, module='search-results', part='title')
            settled()
        time.sleep(.35)

    def float_results(original):
        begin()
        x, y = original['x'] + original['w'] * .5, original['y'] + original['h'] * .5
        cmd(cmd='dock_pointer', phase='move', x=x, y=y)
        settled(lambda s: s['dragging'])
        cmd(cmd='dock_pointer', phase='up', x=x, y=y)
        state = settled(lambda s: not s['dragging'])
        assert state['panels']['search-results']['floating'], 'Results must retain floating support'
        return state

    def home(state, original):
        panel = state['panels']['search-results']
        assert not panel['floating'], 'Results still floating after return/reset'
        assert 'search-results' not in json.dumps(state['layout']), 'Results became a side/top leaf instead of the home surface'
        assert 'CenterFileList' in panel['window_name'], panel
        for key in ('x', 'y', 'w', 'h'):
            assert abs(panel[key] - original[key]) < 1, (key, panel[key], original[key])

    def menu_click(x, y):
        owner = wintypes.DWORD()
        windows.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
        assert owner.value == pid, 'Test window changed owners'
        rect = win32gui.GetClientRect(hwnd)
        assert 0 <= x < rect[2] and 0 <= y < rect[3]
        sx, sy = win32gui.ClientToScreen(hwnd, (x, y))
        cmd(cmd='dock_pointer', phase='move', x=sx, y=sy)
        time.sleep(.4)
        location = (y << 16) | x
        win32gui.PostMessage(hwnd, win32con.WM_MOUSEMOVE, 0, location)
        time.sleep(.1)
        win32gui.PostMessage(hwnd, win32con.WM_LBUTTONDOWN, win32con.MK_LBUTTON, location)
        time.sleep(.1)
        win32gui.PostMessage(hwnd, win32con.WM_LBUTTONUP, 0, location)
        time.sleep(.4)

    def reset_layout(prefix):
        # The language and viewport size below are verified by the screenshot
        # of the actual menu before its Reset Layout item is selected.
        # Target the main viewport: results may now live in a separate OS window.
        cmd(cmd='dock_pointer', phase='hover', module='filter', part='title')
        settled()
        menu_click(300, 12)
        sx, sy = win32gui.ClientToScreen(hwnd, (332, 262))
        cmd(cmd='dock_pointer', phase='move', x=sx, y=sy)
        time.sleep(.8)
        record(prefix + '-reset-menu', settled())
        menu_click(460, 328)
        sx, sy = win32gui.ClientToScreen(hwnd, (460, 328))
        cmd(cmd='dock_pointer', phase='up', x=sx, y=sy)
        return settled()

    def business_state():
        state = cmd(cmd='get_state')
        return {key: state[key] for key in ('db_path', 'search_text', 'result_count',
            'selected_file_id', 'selected_path', 'play_state', 'current_file',
            'folder_filter', 'metadata_filters')}

    def populate_library():
        fixture = root / 'results-home-fixture'
        fixture.mkdir(exist_ok=True)
        for index, seconds in enumerate((120, 2, 2)):
            path = fixture / f'ReturnSample-{index + 1:02}.wav'
            if not path.exists():
                with wave.open(str(path), 'wb') as stream:
                    stream.setparams((2, 2, 48000, 0, 'NONE', 'not compressed'))
                    # Silence exercises the real playback engine without audible
                    # output on the owner's computer during unattended checks.
                    for _ in range(seconds):
                        stream.writeframes(bytes(48000 * 4))
        cmd(cmd='scan_folder', path=str(fixture), recursive=False,
            update_existing=False, remove_missing=False, cache_waveforms=False,
            build_similarity_index=False, worker_threads=1)
        deadline = time.monotonic() + 90
        while True:
            scan = cmd(cmd='scan_status')
            if not scan['scanning'] and not scan['scan_completion_pending']:
                break
            assert time.monotonic() < deadline, scan
            time.sleep(.2)
        assert scan['errors'] == 0, scan
        report['fixture_scan'] = scan
        scan_ui = Smoke(driver, root, out, 30)
        scan_ui.click('scan.result.done', '##ScanResult')
        scan_ui.wait(lambda s: not s['windows'] and not s['is_scanning'], 'Close the real scan result')
        cmd(cmd='set_folder_filter', path=str(fixture))
        cmd(cmd='set_metadata_filter', format='wav')
        cmd(cmd='search', query='ReturnSample')
        # This regression checks the visible result page and state preservation;
        # the deferred global-count scheduler has its own coverage.
        report['filtered_state'] = wait_state(driver, lambda s: s['result_count'] == 3 and
                   not s['async_search_in_progress'], 'Three scanned and filtered WAV files')
        cmd(cmd='load_path', path=str(fixture / 'ReturnSample-01.wav'))
        wait_state(driver, lambda s: s['audio_loaded'] and s['selected_file_id'] > 0,
                   'Selected real scanned audio')
        cmd(cmd='play')
        wait_state(driver, lambda s: s['play_state'] == 'playing', 'Playback started')
        cmd(cmd='pause')
        wait_state(driver, lambda s: s['play_state'] == 'paused', 'Playback paused')
        report['business_before'] = business_state()

    try:
        process, pid = desktop.start(exe)
        identity = driver.command({'cmd': 'magic_eq_identity'}, timeout=60)
        assert identity['pid'] == pid and Path(identity['exe_path']).resolve() == exe, identity
        assert Path(identity['isolation_root']).resolve() == root, identity
        report['identity'] = identity
        time.sleep(2)
        wait_state(driver, lambda s: s['main_ui_ready'], 'main UI', timeout=90)
        state = cmd(cmd='get_state')
        if not state['db_path']:
            assert state['new_db_dialog_open'], state
            cmd(cmd='info_edit_input', action='new_db_name', text='ResultsHomeFixture')
            time.sleep(.15)
            cmd(cmd='info_edit_input', action='new_db_create')
            state = wait_state(driver, lambda s: bool(s['db_path']) and
                not s['db_switch_in_progress'] and not s['new_db_dialog_open'], 'isolated library')
        assert Path(state['db_path']).resolve().is_relative_to(root), state['db_path']
        windows = window_api()
        hwnd = identity['hwnd']
        windows.SetWindowPos(hwnd, None, 40, 40, 1600, 1000, 0x0014)
        cmd(cmd='dock_pointer', phase='focus')
        if args.populated:
            populate_library()
        for theme in args.themes:
            prefix = 'theme-' + str(theme)
            cmd(cmd='set_theme', theme=theme)
            before = settled()
            original = before['panels']['search-results']
            home(before, original)
            record(prefix + '-01-before', before)
            business_before = business_state()
            if args.mode in ('all', 'return'):
                for cycle in range(args.cycles):
                    record(prefix + f'-02-floating-{cycle}', float_results(original))
                    if cycle == 0:
                        begin()
                        x, y = original['x'] + original['w'] * .5, original['y'] + 10
                        cmd(cmd='dock_pointer', phase='move', x=x, y=y)
                        preview = settled(lambda s: s['dragging'])
                        assert preview['preview_kind'] == 4, preview
                        cmd(cmd='dock_pointer', phase='escape')
                        cancelled = settled(lambda s: not s['dragging'])
                        assert cancelled['panels']['search-results']['floating'], cancelled
                        record(prefix + '-cancel-return', cancelled)
                        cmd(cmd='dock_pointer', phase='up', x=x, y=y)
                        settled()
                    begin()
                    x = original['x'] + original['w'] * .5
                    # Reproduce both the original Top-edge mistake and the
                    # new whole-center return target, without a QA fixture.
                    y = original['y'] + (10 if cycle % 2 == 0 else original['h'] * .5)
                    cmd(cmd='dock_pointer', phase='move', x=x, y=y)
                    record(prefix + f'-03-return-preview-{cycle}', settled(lambda s: s['dragging']))
                    cmd(cmd='dock_pointer', phase='up', x=x, y=y)
                    returned = settled(lambda s: not s['dragging'])
                    record(prefix + f'-04-returned-{cycle}', returned)
                    home(returned, original)
                    assert business_state() == business_before, 'Return changed library/filter/selection/playback state'
            if args.mode in ('all', 'reset'):
                record(prefix + '-05-before-reset', float_results(original))
                reset = reset_layout(prefix)
                record(prefix + '-06-reset', reset)
                home(reset, original)
                assert business_state() == business_before, 'Reset changed library/filter/selection/playback state'
            windows.SetWindowPos(hwnd, None, 40, 40, 1400, 900, 0x0014)
            resized = settled()
            assert not resized['panels']['search-results']['floating']
            assert 'CenterFileList' in resized['panels']['search-results']['window_name']
            record(prefix + '-07-resized', resized)
            windows.SetWindowPos(hwnd, None, 40, 40, 1600, 1000, 0x0014)
            restored = settled()
            record(prefix + '-08-restored-size', restored)
            home(restored, original)
        if args.populated:
            cmd(cmd='play')
            wait_state(driver, lambda s: s['play_state'] == 'playing', 'Active playback')
            playing_before = business_state()
            float_results(original)
            begin()
            x, y = original['x'] + original['w'] * .5, original['y'] + 10
            cmd(cmd='dock_pointer', phase='move', x=x, y=y)
            settled(lambda s: s['dragging'])
            cmd(cmd='dock_pointer', phase='up', x=x, y=y)
            returned = settled(lambda s: not s['dragging'])
            home(returned, original)
            report['playing_after'] = business_state()
            assert report['playing_after'] == playing_before, 'Return changed active playback/selection/filter state'
            record('playing-returned', returned)
            cmd(cmd='pause')
        if args.restart:
            cmd(cmd='info_edit_input', action='exit')
            assert win32event.WaitForSingleObject(process, 15000) == win32con.WAIT_OBJECT_0
            assert win32process.GetExitCodeProcess(process) == 0
            process.Close()
            process, pid = desktop.start(exe)
            identity = driver.command({'cmd': 'magic_eq_identity'}, timeout=60)
            assert identity['pid'] == pid and Path(identity['exe_path']).resolve() == exe, identity
            assert Path(identity['isolation_root']).resolve() == root, identity
            report['restart_identity'] = identity
            wait_state(driver, lambda s: s['main_ui_ready'] and not s['db_switch_in_progress'], 'Restart ready', timeout=90)
            hwnd = identity['hwnd']
            windows.SetWindowPos(hwnd, None, 40, 40, 1600, 1000, 0x0014)
            restarted = settled()
            home(restarted, original)
            record('restarted', restarted)
        report['status'] = 'passed'
    except Exception as error:
        report['status'] = 'failed'
        report['error'] = repr(error)
        raise
    finally:
        if process is not None:
            try:
                if win32event.WaitForSingleObject(process, 0) == win32con.WAIT_TIMEOUT:
                    cmd(cmd='info_edit_input', action='exit')
                    assert win32event.WaitForSingleObject(process, 15000) == win32con.WAIT_OBJECT_0
                report['process_exit_code'] = win32process.GetExitCodeProcess(process)
            finally:
                process.Close()
        report['finished_at'] = datetime.now(timezone.utc).isoformat()
        (out / 'report.json').write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
        desktop.close()
    print(json.dumps({'status': report['status'], 'report': str(out / 'report.json')}))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exe', type=Path, required=True)
    parser.add_argument('--isolation-root', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--mode', choices=('return', 'reset', 'all'), default='all')
    parser.add_argument('--themes', type=int, nargs='+', default=[2])
    parser.add_argument('--cycles', type=int, default=3)
    parser.add_argument('--populated', action='store_true')
    parser.add_argument('--restart', action='store_true')
    run(parser.parse_args())
