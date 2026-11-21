"""Confirm the isolated build keeps its data root when started without env overrides."""
from pathlib import Path
import argparse, hashlib, json, time
from qa_required_library_setup import PrivateDesktop, wait_state
from qa_fresh_result_driver import FreshResultDriver as Driver

def run(args):
    import win32con, win32event
    exe = args.exe.resolve(); root = args.isolation_root.resolve(); out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    (root/'profile/runtime').mkdir(parents=True, exist_ok=True)
    driver = Driver(root/'profile/runtime', out); desktop = PrivateDesktop()
    process = None
    report = {}
    try:
        process, pid = desktop.start(exe)  # no AURA_RUNTIME_ROOT
        identity = driver.command({'cmd': 'magic_eq_identity'}, timeout=60)
        report['identity'] = identity
        state = wait_state(driver, lambda s: s.get('main_ui_ready'), 'main ui', 90)
        report['db_path'] = state.get('db_path')
        report['settings_profile'] = str(root)
        report['uses_isolation_root'] = str(Path(state.get('db_path','')).resolve()).lower().startswith(str(root).lower())
        driver.command({'cmd': 'info_edit_input', 'action': 'tray_exit'})
        assert win32event.WaitForSingleObject(process, 15000) == win32con.WAIT_OBJECT_0
        process.Close(); process = None
        report['exit'] = 'clean'
    finally:
        if process is not None:
            try:
                if win32event.WaitForSingleObject(process, 3000) == win32con.WAIT_TIMEOUT:
                    import win32process; win32process.TerminateProcess(process, 1)
            except Exception: pass
        desktop.close()
        (out/'report.json').write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
        print(json.dumps({k: report.get(k) for k in ('uses_isolation_root','db_path','exit')}, ensure_ascii=False))

if __name__ == '__main__':
    p = argparse.ArgumentParser(); p.add_argument('--exe', type=Path, required=True)
    p.add_argument('--isolation-root', type=Path, required=True); p.add_argument('--output', type=Path, required=True)
    run(p.parse_args())

