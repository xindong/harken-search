"""Check real Dock gestures using only the isolated application's input seam.

No desktop pointer movement, key injection, or foreground activation. The caller
provides an isolated QA package and profile with a library and Info visible.
"""
from __future__ import annotations

import argparse
import ctypes.wintypes
import hashlib
import json
import os
from pathlib import Path
import subprocess
import time
import uuid

from PIL import Image, ImageGrab
from qa_transport_themes import Driver, window_api


def run(args):
    exe, root, output = args.exe.resolve(), args.isolation_root.resolve(), args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    runtime = root / 'profile/runtime'
    runtime.mkdir(parents=True, exist_ok=True)
    driver, user32 = Driver(runtime, output), window_api()
    startup = subprocess.STARTUPINFO()
    startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
    startup.wShowWindow = 4
    def launch():
        return subprocess.Popen([str(exe)], cwd=exe.parent, startupinfo=startup,
            env=dict(os.environ, AURA_HARNESS_MULTI_INSTANCE='1',
                     AURA_HARNESS_LANE_KIND='functional', AURA_DISABLE_SINGLE_INSTANCE='1'))

    process = launch()
    hwnd = None
    cases = []
    identity = {}

    def command(**payload):
        return driver.command(payload)

    def settled(predicate=lambda _: True):
        first = command(cmd='dock_state')
        deadline = time.monotonic() + 15
        while time.monotonic() < deadline:
            snap = command(cmd='dock_state')
            if snap['frame'] >= first['frame'] + 5 and predicate(snap):
                return snap
            time.sleep(.06)
        raise AssertionError(snap)

    def record(name, snap):
        if args.info_bounds and not snap['panels']['info']['floating']:
            from check_info_bounds import violations
            info = command(cmd='info_page_state')
            assert info['selected'] == 1, 'Info bounds sweep requires the artwork page'
            failures = violations({'cases': [dict(name=name, state=info)]})
            assert not failures, failures
            snap['info_bounds'] = info
        owner = ctypes.wintypes.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
        assert owner.value == process.pid, 'Screenshot process identity changed'
        if args.state_only:
            cases.append(dict(name=name, pid=process.pid, state=snap, screenshot=None))
            print('PASS (state only):', name, flush=True)
            return
        deadline = time.monotonic() + 5
        frame_path = None
        if args.framebuffer:
            key = uuid.uuid4().hex
            frame_path = runtime / (key + '.ppm')
            command(cmd='qa_capture', capture_key=key)
        while True:
            try:
                shot = Image.open(frame_path).convert('RGB') if frame_path else ImageGrab.grab(window=hwnd).convert('RGB')
            except OSError:
                assert time.monotonic() < deadline, 'Framebuffer capture did not arrive'
                time.sleep(.1)
                continue
            if len(shot.getcolors(shot.width * shot.height) or []) > 20:
                break
            if time.monotonic() >= deadline:
                shot.save(output / (name + '-unrendered.png'))
                cloaked = ctypes.wintypes.DWORD()
                ctypes.WinDLL('dwmapi').DwmGetWindowAttribute(ctypes.wintypes.HWND(hwnd), 14, ctypes.byref(cloaked), ctypes.sizeof(cloaked))
                raise AssertionError(f'Unrendered screenshot: visible={user32.IsWindowVisible(hwnd)}, iconic={user32.IsIconic(hwnd)}, cloaked={cloaked.value}, size={shot.size}')
            time.sleep(.2)
        path = output / (name + '.png')
        shot.save(path)
        cases.append(dict(name=name, pid=process.pid, state=snap, screenshot=str(path),
                          capture_method='OpenGL rendered framebuffer' if frame_path else 'native window',
                          screenshot_sha256=hashlib.sha256(path.read_bytes()).hexdigest()))
        print('PASS:', name, flush=True)

    def begin(module='info'):
        for phase in ('hover', 'down'):
            command(cmd='dock_pointer', phase=phase, module=module, part='title')
            settled()

    def move(x, y):
        command(cmd='dock_pointer', phase='move', x=x, y=y)
        return settled(lambda s: s['dragging'])

    def release(x, y):
        command(cmd='dock_pointer', phase='up', x=x, y=y)
        return settled(lambda s: not s['dragging'])

    try:
        identity = driver.command({'cmd': 'magic_eq_identity'}, timeout=60)
        assert identity['pid'] == process.pid and Path(identity['exe_path']).resolve() == exe, identity
        assert Path(identity['isolation_root']).resolve() == root, identity
        hwnd = identity['hwnd']
        deadline = time.monotonic() + 45
        while not command(cmd='magic_eq_state')['main_ui_ready']:
            assert time.monotonic() < deadline, 'Main UI did not become ready'
            time.sleep(.1)
        if (exe.parent / 'qa-narration.json').is_file():
            command(cmd='qa_narration', action='close')
        user32.SetWindowPos(hwnd, None, 40, 40, 1800, 1000, 0x0014)
        command(cmd='set_theme', theme=2)
        command(cmd='info_page_layout', floating=True, width=480)
        command(cmd='dock_pointer', phase='focus')
        initial = settled(lambda s: s['panels'].get('info', {}).get('floating', False))
        rect, origin = ctypes.wintypes.RECT(), ctypes.wintypes.POINT(0, 0)
        assert user32.GetClientRect(hwnd, ctypes.byref(rect))
        assert user32.ClientToScreen(hwnd, ctypes.byref(origin))
        center = (origin.x + rect.right * .52, origin.y + rect.bottom * .35)
        right = (origin.x + rect.right - 12, center[1])
        record('00-initial', initial)

        begin()
        snap = move(*center)
        assert snap['preview_kind'] == 0 and snap['panels']['info']['floating'], snap
        record('01-center-no-snap', snap)
        snap = release(*center)
        assert snap['panels']['info']['floating'] and snap['preview_kind'] == 0, snap
        record('02-center-release-floating', snap)

        begin()
        snap = move(*right)
        assert snap['preview_kind'] == 1 and snap['preview_side'] == 1, snap
        record('03-right-edge-preview', snap)
        snap = move(*center)
        assert snap['preview_kind'] == 0, snap
        record('04-leave-edge-no-snap', snap)
        snap = release(*center)
        assert snap['panels']['info']['floating'], snap

        begin()
        snap = move(*right)
        assert snap['preview_kind'] == 1 and snap['preview_side'] == 1, snap
        expected_rect = snap['preview_rect']
        snap = release(*right)
        assert not snap['panels']['info']['floating'], snap
        assert abs(snap['panels']['info']['w'] - expected_rect['w']) <= 2, snap
        record('05-right-edge-release-docked', snap)

        before = snap['layout']
        begin()
        snap = move(*center)
        assert snap['panels']['info']['floating'] and snap['preview_kind'] == 0, snap
        command(cmd='dock_pointer', phase='escape')
        snap = settled(lambda s: not s['dragging'])
        assert snap['layout'] == before and not snap['panels']['info']['floating'], snap
        record('06-escape-restores-layout', snap)
        command(cmd='dock_pointer', phase='up', x=center[0], y=center[1])
        settled()

        if args.vertical:
            for orientation, width, height in [('landscape', 1800, 1000), ('portrait', 900, 1400)]:
                user32.SetWindowPos(hwnd, None, 40, 40, width, height, 0x0014)
                for side, side_id in [('top', 2), ('bottom', 3)]:
                    command(cmd='info_page_layout', floating=True, width=480)
                    base = settled(lambda s: s['panels']['info']['floating'])
                    results = base['panels']['search-results']
                    sidebar = base['panels']['filter']
                    x = results['x'] + results['w'] * .5
                    y = results['y'] + 10 if side == 'top' else results['y'] + results['h'] - 10
                    begin()
                    snap = move(x, y)
                    assert snap['preview_kind'] == 1 and snap['preview_side'] == side_id, snap
                    expected = snap['preview_rect']
                    record(f'{orientation}-{side}-preview', snap)
                    snap = release(x, y)
                    panel, remaining = snap['panels']['info'], snap['panels']['search-results']
                    assert not panel['floating'] and snap['layout'][side_id] == 'info', snap
                    for key in ('x', 'y', 'w', 'h'):
                        assert abs(panel[key] - expected[key]) <= 2, (key, panel, expected)
                    assert abs(panel['x'] - remaining['x']) <= 2, snap
                    assert abs(panel['w'] - remaining['w']) <= 2, snap
                    assert remaining['h'] >= 199, snap
                    if side == 'top':
                        assert panel['y'] + panel['h'] <= remaining['y'] + 2, snap
                    else:
                        assert remaining['y'] + remaining['h'] <= panel['y'] + 2, snap
                    assert abs(snap['panels']['filter']['y'] - sidebar['y']) <= 2, snap
                    assert abs(snap['panels']['filter']['h'] - sidebar['h']) <= 2, snap
                    record(f'{orientation}-{side}-docked', snap)

            command(cmd='similar_index', action='open', floating=True, width=640, height=360)
            base = settled(lambda s: s['panels'].get('similar-sounds', {}).get('floating', False))
            results = base['panels']['search-results']
            x, y = results['x'] + results['w'] * .5, results['y'] + 10
            begin('similar-sounds')
            snap = move(x, y)
            assert snap['preview_kind'] == 1 and snap['preview_side'] == 2, snap
            snap = release(x, y)
            assert snap['layout'][2:] == ['similar-sounds', 'info'], snap
            assert snap['panels']['search-results']['h'] >= 199, snap
            record('portrait-both-bands', snap)
            # Similar Sounds is intentionally a transient window at startup.
            # Close it before checking persistence of the saved Info layout.
            command(cmd='similar_index', action='close')
            snap = settled()
            panel = snap['panels']['info']
            x, y = panel['x'] + panel['w'] * .5, panel['y'] - 3
            command(cmd='dock_pointer', phase='hover', module='info', part='band-splitter')
            settled()
            command(cmd='dock_pointer', phase='down', module='info', part='band-splitter')
            settled()
            command(cmd='dock_pointer', phase='move', x=x, y=y - 60)
            settled()
            command(cmd='dock_pointer', phase='up', x=x, y=y - 60)
            saved = settled()
            assert saved['panels']['info']['h'] >= panel['h'] + 55, (panel, saved)
            assert saved['panels']['search-results']['h'] >= 199, saved
            record('portrait-resize-band', saved)
            command(cmd='info_edit_input', action='exit')
            process.wait(timeout=15)
            hwnd = None
            process = launch()
            restarted = driver.command({'cmd': 'magic_eq_identity'}, timeout=60)
            assert restarted['pid'] == process.pid, restarted
            assert Path(restarted['exe_path']).resolve() == exe, restarted
            assert Path(restarted['isolation_root']).resolve() == root, restarted
            hwnd = restarted['hwnd']
            deadline = time.monotonic() + 45
            while not command(cmd='magic_eq_state')['main_ui_ready']:
                assert time.monotonic() < deadline, 'Restart did not reach the main UI'
                time.sleep(.1)
            if (exe.parent / 'qa-narration.json').is_file():
                command(cmd='qa_narration', action='close')
            user32.SetWindowPos(hwnd, None, 40, 40, 900, 1400, 0x0014)
            snap = settled()
            assert snap['layout'] == saved['layout'], (saved, snap)
            for module in ('info', 'filter', 'search-results'):
                for key in ('x', 'y', 'w', 'h'):
                    assert abs(snap['panels'][module][key] - saved['panels'][module][key]) <= 2, (module, key, saved, snap)
            record('portrait-restart-keeps-layout', snap)

            if getattr(args, 'resize_boundaries', False):
                command(cmd='similar_index', action='open', floating=True, width=640, height=360)
                snap = settled(lambda s: s['panels'].get('similar-sounds', {}).get('floating', False))
                results = snap['panels']['search-results']
                x, y = results['x'] + results['w'] * .5, results['y'] + 10
                begin('similar-sounds'); move(x, y); snap = release(x, y)
                assert snap['layout'][2:] == ['similar-sounds', 'info'], snap
                user32.SetWindowPos(hwnd, None, 40, 40, 900, 700, 0x0014)
                snap = settled(lambda s: s['panels']['info']['floating'])
                assert snap['panels']['search-results']['h'] >= 199, snap
                user32.GetClientRect(hwnd, ctypes.byref(rect))
                user32.ClientToScreen(hwnd, ctypes.byref(origin))
                for panel in snap['panels'].values():
                    if not panel['floating']:
                        assert panel['y'] + panel['h'] <= origin.y + rect.bottom, snap
                record('short-window-floats-overflow', snap)
                command(cmd='info_edit_input', action='exit'); process.wait(timeout=15)
                hwnd = None; process = launch()
                identity2 = driver.command({'cmd':'magic_eq_identity'}, timeout=60)
                assert identity2['pid'] == process.pid and Path(identity2['exe_path']).resolve() == exe
                hwnd = identity2['hwnd']
                deadline = time.monotonic() + 45
                while not command(cmd='magic_eq_state')['main_ui_ready']:
                    assert time.monotonic() < deadline
                    time.sleep(.1)
                if (exe.parent/'qa-narration.json').is_file():
                    command(cmd='qa_narration', action='close')
                snap = settled(lambda s: s['panels']['info']['floating'])
                record('short-window-restart-retains-float', snap)
                user32.SetWindowPos(hwnd, None, 40, 40, 680, 1000, 0x0014)
                command(cmd='similar_index', action='open', floating=True, width=640, height=360)
                command(cmd='dock_pointer', phase='focus')
                snap = settled(lambda s: s['panels'].get('similar-sounds', {}).get('floating', False))
                results = snap['panels']['search-results']
                assert results['w'] < 420, results
                x, y = results['x'] + results['w'] * .5, results['y'] + 10
                begin('similar-sounds'); snap = move(x, y)
                assert snap['preview_kind'] == 0 and snap['drop_too_small'], snap
                snap = release(x, y)
                assert snap['panels']['similar-sounds']['floating'], snap
                record('narrow-window-rejects-wide-band', snap)
                command(cmd='similar_index', action='close')

        # Return to the original floating setup so the package is ready for
        # the user's center-versus-edge comparison.
        command(cmd='info_page_layout', floating=True, width=480)
        settled()
        report = dict(identity=identity, exe_sha256=hashlib.sha256(exe.read_bytes()).hexdigest(),
                      cases=cases, passed=True, input_method='app-owned QA pointer; no OS input')
        (output / 'report.json').write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
    except Exception as error:
        (output / 'failure.json').write_text(json.dumps(dict(identity=identity, cases=cases,
            error=str(error)), ensure_ascii=False, indent=2), encoding='utf-8')
        raise
    finally:
        if hwnd and process.poll() is None:
            try:
                command(cmd='info_edit_input', action='exit')
            except (RuntimeError, TimeoutError):
                user32.PostMessageW(hwnd, 0x0010, 0, 0)
        try:
            process.wait(timeout=15)
        except subprocess.TimeoutExpired:
            process.terminate()
            process.wait(timeout=5)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exe', type=Path, required=True)
    parser.add_argument('--isolation-root', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--vertical', action='store_true', help='Also check results top/bottom in landscape and portrait')
    parser.add_argument('--state-only', action='store_true', help='Check geometry without screenshots; not visual evidence')
    parser.add_argument('--framebuffer', action='store_true', help='Use a local QA-only rendered-frame capture patch')
    parser.add_argument('--info-bounds', action='store_true', help='Measure Info content in each docked state')
    parser.add_argument('--resize-boundaries', action='store_true', help='Check narrow drops, shrink overflow and restart recovery')
    run(parser.parse_args())
