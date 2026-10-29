"""Capture real ordinary buttons in five themes on a private Windows desktop.

Only isolated UI-open/read-only commands: no Wwise connections, update installs,
user data operations or shared desktop input. Run first-library QA beforehand.
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


def contrast(fg, bg):
    def luminance(color):
        rgb = [v / 255 for v in color[:3]]
        linear = [v / 12.92 if v <= .04045 else ((v + .055) / 1.055) ** 2.4 for v in rgb]
        return sum(v * w for v, w in zip(linear, (.2126, .7152, .0722)))
    a, b = luminance(fg), luminance(bg)
    return (max(a, b) + .05) / (min(a, b) + .05)


def audit(driver, theme):
    first = driver.command({'cmd': 'ordinary_button_audit'})
    deadline = time.monotonic() + 12
    while time.monotonic() < deadline:
        result = driver.command({'cmd': 'ordinary_button_audit'})
        if (result['frame'] > first['current_frame'] + 2 and result['theme'] == theme
                and result['current_frame'] - result['frame'] <= 2 and result['checks']):
            return result
        time.sleep(.08)
    raise AssertionError('No current real-button observations')


def run(args):
    import win32con
    import win32event
    import win32process
    import win32gui
    exe, root, out = args.exe.resolve(), args.isolation_root.resolve(), args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    desktop = PrivateDesktop()
    process = None
    driver = Driver(root / 'profile/runtime', out)
    report = {'exe': str(exe), 'sha256': hashlib.sha256(exe.read_bytes()).hexdigest(),
              'started_at': datetime.now(timezone.utc).isoformat(), 'windows': [], 'failures': []}
    try:
        process, pid = desktop.start(exe)
        identity = driver.command({'cmd': 'magic_eq_identity'}, timeout=60)
        assert identity['pid'] == pid and Path(identity['exe_path']).resolve() == exe, identity
        assert Path(identity['isolation_root']).resolve() == root, identity
        report['identity'] = identity
        state = wait_state(driver, lambda s: s['main_ui_ready'] and not s['db_switch_in_progress'], 'Ready', 60)
        assert state['db_path'] and not state['new_db_dialog_open'], 'Run first-library suite first'
        if args.narration:
            note = driver.command({'cmd': 'qa_narration', 'action': 'state'})
            assert note['open'] and not note['error'], note
            driver.command({'cmd': 'qa_narration', 'action': 'close'})
        modules = args.modules or ['settings', 'library', 'batch-rename', 'transfer', 'workflow', 'theme-editor', 'update', 'similar', 'wwise']
        for module in modules:
            if module in ('transfer', 'workflow'):
                driver.command({'cmd': 'function_window', 'module': module, 'action': 'open', 'floating': False})
            elif module == 'update':
                driver.command({'cmd': 'update_notice', 'action': 'open'})
            elif module == 'similar':
                driver.command({'cmd': 'similar_index', 'action': 'open', 'floating': False})
            elif module != 'theme-editor':
                driver.command({'cmd': 'ordinary_button_window', 'module': module, 'action': 'open'})
            time.sleep(.4)
            for theme, _ in THEMES:
                driver.command({'cmd': 'set_theme', 'theme': theme})
                time.sleep(.25)
                if module == 'theme-editor':
                    driver.command({'cmd': 'ordinary_button_window', 'module': module, 'action': 'open'})
                    time.sleep(.3)
                observed = audit(driver, theme)
                observed['module'] = module
                observed['origin'] = list(win32gui.ClientToScreen(identity['hwnd'], (0, 0)))
                observed['screenshot'] = f'{module}-theme-{theme}.png'
                for button in observed['checks']:
                    left, top, right, bottom = button['bounds']
                    cl, ct, cr, cb = button['clip']
                    if left < cl - 1 or top < ct - 1 or right > cr + 1 or bottom > cb + 1:
                        report['failures'].append({'module': module, 'theme': theme,
                            'label': button['label'], 'kind': 'clipped-button',
                            'bounds': button['bounds'], 'clip': button['clip']})
                    if not button['disabled']:
                        ratio = contrast(button['fg'], button['fill'])
                        button['contrast'] = ratio
                        if ratio < 4.5:
                            report['failures'].append({'module': module, 'theme': theme,
                                'label': button['label'], 'contrast': ratio})
                capture(driver, out / observed['screenshot'])
                report['windows'].append(observed)
                count = len(observed['checks'])
                print(f'{module} theme={theme}: {count} buttons', flush=True)
                if module == 'theme-editor':
                    driver.command({'cmd': 'ordinary_button_window', 'module': module, 'action': 'close'})
                    time.sleep(.2)
            if module in ('transfer', 'workflow'):
                driver.command({'cmd': 'function_window', 'module': module, 'action': 'close'})
            elif module == 'update':
                driver.command({'cmd': 'update_notice', 'action': 'close'})
            elif module == 'similar':
                driver.command({'cmd': 'similar_index', 'action': 'close'})
            elif module not in ('wwise', 'theme-editor'):
                driver.command({'cmd': 'info_edit_input', 'action': 'escape_down'})
                time.sleep(.15)
                driver.command({'cmd': 'info_edit_input', 'action': 'escape_up'})
            time.sleep(.2)
        assert not report['failures'], report['failures']
        report['status'] = 'passed'
    except BaseException as error:
        report['status'] = 'failed'
        report['error'] = str(error)
        raise
    finally:
        if process is not None:
            if win32event.WaitForSingleObject(process, 0) == win32con.WAIT_TIMEOUT:
                try:
                    driver.command({'cmd': 'info_edit_input', 'action': 'exit'})
                except (RuntimeError, TimeoutError) as error:
                    report['shutdown_error'] = str(error)
            stopped = win32event.WaitForSingleObject(process, 15000) == win32con.WAIT_OBJECT_0
            report['process_stopped'] = stopped
            if stopped: report['exit_code'] = win32process.GetExitCodeProcess(process)
            process.Close()
        desktop.close()
        (out / 'report.json').write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
    assert report['process_stopped'] and report['exit_code'] == 0, report


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    for key in ('exe', 'isolation-root', 'output'):
        parser.add_argument('--' + key, type=Path, required=True)
    parser.add_argument('--narration', action='store_true')
    parser.add_argument('--modules', nargs='+', choices=['settings', 'library', 'batch-rename',
                        'transfer', 'workflow', 'theme-editor', 'update', 'similar', 'wwise'])
    run(parser.parse_args())
