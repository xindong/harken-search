"""Verify the real library-switch background without using the user's desktop.

The target database must be a disposable copy in the preview isolation root.
The existing preview-only search delay makes the first-page wait observable;
the application itself has no artificial minimum loading time.
"""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import time

from qa_required_library_setup import PrivateDesktop, capture, wait_state
from qa_transport_themes import Driver, THEMES


def run(args):
    import win32con
    import win32event
    import win32gui
    import win32process
    from PIL import Image, ImageChops

    exe, root, out = args.exe.resolve(), args.isolation_root.resolve(), args.output.resolve()
    libraries = [args.library_a.resolve(), args.library_b.resolve()]
    for library in libraries:
        assert library.is_relative_to(root) and library.is_file(), library
    out.mkdir(parents=True, exist_ok=False)
    runtime = root / 'profile/runtime'
    runtime.mkdir(parents=True, exist_ok=True)
    driver = Driver(runtime, out)
    desktop = PrivateDesktop()
    process = None
    checks = []
    report = {'exe': str(exe), 'exe_sha256': hashlib.sha256(exe.read_bytes()).hexdigest(),
        'started_at': datetime.now(timezone.utc).isoformat(), 'checks': checks,
        'capture_note': 'Actual executable frames; existing preview-only search delay is 3000 ms for recording. Default operation has no delay.'}

    def ready(state):
        return (state['main_ui_ready'] and not state['db_switch_in_progress']
            and state['pending_search_request_id'] == 0
            and not state.get('db_switch_background_active', False))

    def stop():
        nonlocal process
        if process is None:
            return
        if win32event.WaitForSingleObject(process, 0) == win32con.WAIT_TIMEOUT:
            driver.command({'cmd': 'info_edit_input', 'action': 'exit'})
        assert win32event.WaitForSingleObject(process, 15000) == win32con.WAIT_OBJECT_0
        assert win32process.GetExitCodeProcess(process) == 0
        process.Close()
        process = None

    try:
        process, pid = desktop.start(exe, environment_overrides={'HARKEN_QA_SEARCH_DELAY_MS': '3000'})
        identity = driver.command({'cmd': 'magic_eq_identity'}, timeout=90)
        assert identity['pid'] == pid and Path(identity['exe_path']).resolve() == exe, identity
        assert Path(identity['isolation_root']).resolve() == root, identity
        report['identity'] = identity
        initial = wait_state(driver, ready, 'Initial library must be ready', timeout=90)
        assert initial['result_count'] > 0, initial
        # Startup maximizes the host; resize only after it has finished.
        win32gui.ShowWindow(identity['hwnd'], win32con.SW_RESTORE)
        win32gui.SetWindowPos(identity['hwnd'], 0, 0, 0, 1400, 900,
            win32con.SWP_NOMOVE | win32con.SWP_NOZORDER | win32con.SWP_NOACTIVATE)
        wait_state(driver, lambda s: s['window_width'] < 1500, 'Private window resize')
        region = driver.command({'cmd': 'get_ui_regions'})['list']
        origin = win32gui.ClientToScreen(identity['hwnd'], (0, 0))
        # Leave tabs, path and header above the measured results-body area.
        roi = (round(region['x'] - origin[0] + 12),
            round(region['y'] - origin[1] + 130),
            round(region['x'] - origin[0] + region['w'] - 24),
            round(region['y'] - origin[1] + region['h'] - 28))
        report['body_roi'] = roi
        themes = THEMES if args.all_themes else [THEMES[2]]
        first_animation = None
        for theme, label in themes:
            driver.command({'cmd': 'set_theme', 'theme': theme})
            wait_state(driver, lambda state: state['theme'] == theme and ready(state), 'Theme ready')
            current = Path(driver.command({'cmd': 'get_state'})['db_path']).resolve()
            target = next(library for library in libraries if library != current)
            frames, times, states = [], [], []
            started = driver.command({'cmd': 'switch_library', 'path': str(target)})
            assert started.get('started'), started
            active = wait_state(driver, lambda s: s.get('db_switch_background_active'), 'Switch background must start')
            states.append(active)
            while len(frames) < 22:
                state = driver.command({'cmd': 'get_state'})
                states.append(state)
                if not state['db_switch_background_active']:
                    break
                path = out / f'theme-{theme}-loading-{len(frames):02d}.png'
                capture(driver, path)
                frames.append(path)
                times.append(time.monotonic())
                time.sleep(.04)
            deadline = time.monotonic() + 35
            while time.monotonic() < deadline:
                state = driver.command({'cmd': 'get_state'})
                states.append(state)
                if ready(state):
                    break
                time.sleep(.03)
            assert ready(state), state
            assert Path(state['db_path']).resolve() == target and state['result_count'] > 0, state
            assert len(frames) >= 3, 'Not enough rendered loading frames'
            with Image.open(frames[0]) as first, Image.open(frames[-2]) as last:
                assert 0 <= roi[0] < roi[2] <= first.width and 0 <= roi[1] < roi[3] <= first.height, roi
                difference = ImageChops.difference(first.convert('RGB').crop(roi), last.convert('RGB').crop(roi))
                changed = sum(count for count, value in difference.convert('L').getcolors(difference.width * difference.height) if value > 0)
                fraction = changed / (difference.width * difference.height)
                assert fraction > .08, f'Background must move across a broad area: {fraction:.4f}'
            first_page_states = [s for s in states if not s['db_switch_in_progress'] and s['pending_search_request_id'] != 0]
            assert first_page_states, 'First-page wait was not observed'
            assert all(s['db_switch_background_active'] for s in first_page_states), first_page_states
            settled = out / f'theme-{theme}-ready.png'
            capture(driver, settled)
            images = [Image.open(path).convert('RGB') for path in frames]
            durations = [max(40, round((times[i + 1] - times[i]) * 1000)) for i in range(len(times) - 1)]
            durations.append(durations[-1])
            animation = out / f'theme-{theme}-actual-switch.gif'
            images[0].save(animation, save_all=True, append_images=images[1:], duration=durations, loop=0, optimize=True)
            for image in images:
                image.close()
            if first_animation is None or theme == 2:
                first_animation = animation
            checks.append({'theme': theme, 'name': label, 'status': 'passed',
                'moving_body_fraction': fraction, 'loading_frame_count': len(frames),
                'first_page_wait_observed': True, 'background_off_when_ready': True,
                'result_count': state['result_count'], 'animation': str(animation), 'ready': str(settled)})

        # A bad target must restore the actual previous library and end the effect.
        if args.check_failure:
            before = driver.command({'cmd': 'get_state'})
            invalid = root / 'fixtures/invalid/main.db'
            invalid.parent.mkdir(parents=True, exist_ok=True)
            assert not invalid.exists(), invalid
            invalid.write_bytes(b'Harken library-switch invalid-database QA fixture')
            started = driver.command({'cmd': 'switch_library', 'path': str(invalid)})
            assert started.get('started'), started
            recovered = wait_state(driver, lambda s: ready(s) and bool(s.get('db_switch_notice')), 'Failed switch must recover', timeout=60)
            assert Path(recovered['db_path']).resolve() == Path(before['db_path']).resolve(), recovered
            assert recovered['result_count'] > 0 and not recovered['db_switch_background_active'], recovered
            capture(driver, out / 'failure-recovered.png')
            checks.append({'scenario': 'failure_recovery', 'status': 'passed', 'notice': recovered['db_switch_notice']})

        driver.command({'cmd': 'search', 'query': 'harken_no_result_probe_7361'})
        searching = wait_state(driver, lambda s: s['pending_search_request_id'] != 0, 'Ordinary search must start')
        assert not searching['db_switch_background_active'], searching
        empty = wait_state(driver, ready, 'Empty search must finish')
        assert empty['result_count'] == 0 and not empty['db_switch_background_active'], empty
        driver.command({'cmd': 'search', 'query': ''})
        restored = wait_state(driver, ready, 'Restore unfiltered results')
        assert restored['result_count'] > 0, restored
        checks.append({'scenario': 'ordinary_search_and_empty_results', 'status': 'passed', 'background_stayed_off': True})

        driver.command({'cmd': 'set_theme', 'theme': 2})
        report['preview'] = str(first_animation)
        report['status'] = 'passed'
        stop()
    except Exception as exc:
        report['status'] = 'failed'
        report['error'] = str(exc)
        raise
    finally:
        try:
            stop()
        finally:
            desktop.close()
            report['finished_at'] = datetime.now(timezone.utc).isoformat()
            (out / 'report.json').write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exe', type=Path, required=True)
    parser.add_argument('--isolation-root', type=Path, required=True)
    parser.add_argument('--library-a', type=Path, required=True)
    parser.add_argument('--library-b', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--all-themes', action='store_true')
    parser.add_argument('--check-failure', action='store_true')
    run(parser.parse_args())
