"""Capture the real navy toolbar and scan dialog without shared mouse input.

Requires an isolated diagnostics package and an empty profile. Does not scan.
Exit 0 means capture completed, not human visual acceptance; 2 means incomplete.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import time
from collections import Counter
from PIL import ImageGrab
from qa_transport_themes import Driver, window_api
from qa_empty_state_themes import fresh


def run(args):
    exe, root, out = args.exe.resolve(), args.isolation_root.resolve(), args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    runtime = root / 'profile/runtime'
    runtime.mkdir(parents=True, exist_ok=True)
    if (runtime / 'qa_cmd.json').exists():
        raise RuntimeError('Pending QA command; refusing to overwrite')
    proc = subprocess.Popen([str(exe)], cwd=exe.parent, env=dict(os.environ,
        AURA_HARNESS_MULTI_INSTANCE='1', AURA_HARNESS_LANE_KIND='functional', AURA_DISABLE_SINGLE_INSTANCE='1'))
    driver, identity = Driver(runtime, out), {}
    api = window_api()
    sha = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
    try:
        identity = driver.command({'cmd': 'magic_eq_identity'}, timeout=40)
        if identity['pid'] != proc.pid or Path(identity['exe_path']).resolve() != exe or Path(identity['isolation_root']).resolve() != root:
            raise RuntimeError('Wrong EXE, PID or pinned profile')
        if driver.command({'cmd': 'get_state'})['db_path']:
            raise RuntimeError('Profile already has a database; refusing to change it')
        api.ShowWindow(identity['hwnd'], 4)
        api.SetWindowPos(identity['hwnd'], None, 20, 30, 1600, 960, 0x0040 | 0x0010 | 0x0004)
        driver.command({'cmd': 'set_theme', 'theme': 2})
        fresh(driver, 2)
        ImageGrab.grab(window=identity['hwnd']).convert('RGB').save(out / 'toolbar-full.png')
        driver.command({'cmd': 'open_scan_dialog'})
        deadline, previous, stable = time.monotonic() + 20, None, 0
        while time.monotonic() < deadline:
            shot = ImageGrab.grab(window=identity['hwnd']).convert('RGB')
            # Opaque dialog body and controls are distinguishable from the dimmed canvas.
            counts = Counter(shot.getdata())
            body_visible = counts[(0x26, 0x39, 0x4d)] > 30000
            controls_visible = counts[(0x3b, 0x52, 0x6c)] > 1000
            digest = hashlib.sha256(shot.tobytes()).hexdigest()
            stable = stable + 1 if body_visible and controls_visible and digest == previous else 0
            previous = digest
            if stable >= 2:
                shot.save(out / 'scan-full.png')
                break
            time.sleep(.25)
        else:
            raise TimeoutError('No stable scan dialog with expected body and controls')
        source = Path(__file__).resolve().parents[1]
        files = ['src/BuiltinThemeScheme.cpp', 'src/Theme.cpp', 'src/Theme.h',
                 'src/main.cpp', 'src/ui/MistBlueChrome.h', 'tools/qa_navy_chrome.py']
        identity.update(exe_sha256=sha(exe),
            git_revision=subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=source, text=True).strip(),
            source_sha256={f: sha(source / f) for f in files},
            worktree_dirty=bool(subprocess.check_output(['git', 'status', '--porcelain', '--untracked-files=no'], cwd=source, text=True).strip()))
        report = {'identity': identity, 'status': 'captured', 'human_acceptance': 'pending',
                  'screenshots': {f: sha(out / f) for f in ['toolbar-full.png', 'scan-full.png']},
                  'not_covered': ['actual scanning', 'custom themes', 'dialog hover interaction']}
        (out / 'report.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
        print(json.dumps(report))
        return 0
    finally:
        if proc.poll() is None:
            if identity.get('pid') == proc.pid:
                api.PostMessageW(identity['hwnd'], 0x0010, 0, 0)
            try:
                proc.wait(timeout=15)
            except subprocess.TimeoutExpired:
                proc.terminate()
                proc.wait(timeout=5)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ['exe', 'isolation-root', 'output']:
        parser.add_argument('--' + name, type=Path, required=True)
    try:
        raise SystemExit(run(parser.parse_args()))
    except Exception as error:
        print(str(error))
        raise SystemExit(2)
