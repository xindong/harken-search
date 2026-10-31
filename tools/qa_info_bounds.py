"""Measure and capture docked Info in a compile-time isolated QA package.

Uses app-local QA input only. Does not move the desktop pointer or activate a window.
The caller prepares an isolated fixture library with no artwork.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import time

from PIL import ImageGrab
from check_info_bounds import violations
from qa_transport_themes import Driver, window_api


def run(args):
    exe, root, output = args.exe.resolve(), args.isolation_root.resolve(), args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    runtime = root / 'profile/runtime'
    runtime.mkdir(parents=True, exist_ok=True)
    driver, user32 = Driver(runtime, output), window_api()
    startup = subprocess.STARTUPINFO()
    startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
    startup.wShowWindow = 4
    process = subprocess.Popen([str(exe)], cwd=exe.parent, startupinfo=startup,
        env=dict(os.environ, AURA_HARNESS_MULTI_INSTANCE='1',
                 AURA_HARNESS_LANE_KIND='functional', AURA_DISABLE_SINGLE_INSTANCE='1'))
    hwnd = None
    def command(**kwargs):
        return driver.command(kwargs)
    def settled():
        first = command(cmd='info_page_state')
        deadline = time.monotonic() + 15
        while time.monotonic() < deadline:
            snap = command(cmd='info_page_state')
            if snap['frame'] >= first['frame'] + 4 and snap['layout_windows'] and snap['zones']:
                return snap
            time.sleep(.08)
        raise TimeoutError('Info did not produce a fresh layout')
    try:
        identity = driver.command({'cmd':'magic_eq_identity'}, timeout=60)
        assert identity['pid'] == process.pid and Path(identity['exe_path']).resolve() == exe, identity
        assert Path(identity['isolation_root']).resolve() == root, identity
        hwnd = identity['hwnd']
        deadline = time.monotonic() + 50
        while not command(cmd='magic_eq_state')['main_ui_ready']:
            if time.monotonic() >= deadline:
                raise TimeoutError('Main UI did not become ready')
            time.sleep(.15)
        user32.SetWindowPos(hwnd, None, 40, 40, 1600, 1000, 0x0014)
        command(cmd='info_page_layout', floating=False, width=600)
        snap = settled()
        if snap['selected'] != 1:
            for phase in ['hover', 'down', 'up']:
                command(cmd='info_page_pointer', key='info.artwork', phase=phase)
                time.sleep(.12)
        cases = []
        for width, height in [(1600,1000), (1200,720), (2000,1200)]:
            user32.SetWindowPos(hwnd, None, 40, 40, width, height, 0x0014)
            for theme in [0,1,2,3,10]:
                command(cmd='set_theme', theme=theme)
                snap = settled()
                assert snap['selected'] == 1 and not snap['floating'], snap
                name = f'{width}x{height}-theme-{theme}'
                image = ImageGrab.grab(window=hwnd)
                assert len(image.convert('RGB').getcolors(image.width * image.height) or []) > 20
                image.save(output / (name + '.png'))
                cases.append(dict(name=name, state=snap))
        report = dict(identity=identity, exe_sha256=hashlib.sha256(exe.read_bytes()).hexdigest(), cases=cases)
        report['violations'] = violations(report)
        (output/'report.json').write_text(json.dumps(report,ensure_ascii=False,indent=2),encoding='utf-8')
        print(f"{len(cases)} cases; {len(report['violations'])} violations")
        for failure in report['violations']:
            print(failure)
        return bool(report['violations'])
    finally:
        if hwnd:
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
    raise SystemExit(run(parser.parse_args()))
