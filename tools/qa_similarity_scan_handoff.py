"""Check post-scan similarity outcomes in a pinned, isolated real EXE.

Creates twelve tones and one silent WAV below the supplied QA profile. Existing
media and the original library are retained. Uses a private desktop and the
observed scan controls; never switches desktops or moves the shared mouse.
"""
from pathlib import Path
import argparse, hashlib, json, time, traceback, uuid, wave
from qa_required_library_setup import PrivateDesktop
from qa_transport_themes import Driver
from qa_scan_dialog_simple import Smoke, make_wavs
from qa_similarity_pending import snapshot
import win32api, win32con, win32event, win32gui, win32process


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--package-receipt', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    receipt = json.loads(args.package_receipt.read_text(encoding='utf-8'))
    exe, root, original = (Path(receipt[k]).resolve() for k in ('exe', 'root', 'database'))
    assert root.is_relative_to(Path('D:/Harken-QA').resolve())
    assert original.is_relative_to(root) and original.is_file()
    assert hashlib.sha256(exe.read_bytes()).hexdigest() == receipt['sha256']
    for pid in win32process.EnumProcesses():
        handle = None
        try:
            handle = win32api.OpenProcess(win32con.PROCESS_QUERY_INFORMATION | win32con.PROCESS_VM_READ, False, pid)
            running = Path(win32process.GetModuleFileNameEx(handle, 0)).resolve()
        except Exception:
            continue
        finally:
            if handle: handle.Close()
        assert running != exe, 'The pinned package already owns its command channel'
    output = args.output.resolve(); output.mkdir(parents=True, exist_ok=False)
    runtime = root / 'profile/runtime'
    assert not (runtime / 'qa_cmd.json').exists(), 'A QA command is already pending'
    lock = runtime / 'qa_similarity_scan_handoff.lock'
    token = uuid.uuid4().hex
    with lock.open('x', encoding='utf-8') as handle: handle.write(token)
    driver = Driver(runtime, output); ui = Smoke(driver, root, output, 180)
    report = {'passed': False, 'package_sha256': receipt['sha256'], 'checks': [],
              'not_covered': ['Human sound quality, full-library performance and external DAW drop targets']}
    process = desktop = None; verified = False
    def save():
        (output / 'report.json').write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
    def command(**args): return driver.command(args, timeout=60)
    try:
        before = snapshot(original); report['original_before'] = before
        desktop = PrivateDesktop(); process, pid = desktop.start(exe)
        identity = command(cmd='magic_eq_identity'); report['identity'] = identity
        assert identity['pid'] == pid and Path(identity['exe_path']).resolve() == exe
        assert Path(identity['isolation_root']).resolve() == root
        verified = True
        ui.wait(lambda s: s['main_ui_ready'] and not s['db_switch_in_progress'], 'main ready', 60,
                lambda: command(cmd='get_state'))
        win32gui.SetWindowPos(identity['hwnd'], 0, 40, 40, 1480, 940, win32con.SWP_NOACTIVATE | win32con.SWP_NOZORDER)
        database = ui.create_library(always_new=True); report['fixture_database'] = str(database)
        source = root / ('scan-handoff-' + token[:10]); make_wavs(source, 12)
        with wave.open(str(source / 'silent.wav'), 'wb') as silent:
            silent.setparams((1, 2, 16000, 0, 'NONE', 'not compressed'))
            silent.writeframes(bytes(32000))
        command(cmd='scan_dialog_ui', action='setup', folders=[str(source)], fast=False, similar=True)
        ui.click('scan.dialog.start')
        scanned = ui.wait(lambda s: s['surface'] == 'result' and
                          ui.target(s, 'scan.result.done', '##ScanResult') is not None, 'scan result')
        assert scanned['succeeded'] == 13 and len(ui.rows(database)) == 13, scanned
        report['checks'].append({'case': 'thirteen-real-fixtures-ingested', 'passed': True})
        ui.click('scan.result.done', '##ScanResult')
        terminal = ui.wait(lambda s: ui.has(s, 'scan.similar.done') and
                          s['similarity']['available'] and not s['similarity']['running'], 'similarity result')
        outcome = terminal['similarity']; report['terminal'] = terminal
        assert outcome['total'] == 13 and outcome['succeeded'] == 12, outcome
        assert outcome['failed'] == 1 and outcome['remaining'] == 0, outcome
        report['checks'].append({'case': 'saved-and-silent-outcomes-forwarded-without-double-counting', 'passed': True})
        report['screenshot'] = ui.shot('post-scan-similarity', 'scan.similar.done', '##ScanSimilarityResult')
        ui.click('scan.similar.done', '##ScanSimilarityResult')
        ui.wait(lambda s: not ui.has(s, 'scan.similar.done'), 'dismiss similarity result')
        command(cmd='switch_db', path=str(original))
        ui.wait(lambda s: not s['db_switch_in_progress'] and s['db_path'] and
                Path(s['db_path']).resolve() == original, 'restore original library', 60, lambda: command(cmd='get_state'))
        after = snapshot(original); report['original_after'] = after
        assert before == after, 'The original library changed during the isolated scan handoff check'
        report['checks'].append({'case': 'original-library-and-analysis-preserved', 'passed': True})
        report['passed'] = True
    except Exception:
        report['error'] = traceback.format_exc(); raise
    finally:
        save()
        try:
            if process is not None:
                if verified: command(cmd='info_edit_input', action='exit')
                assert win32event.WaitForSingleObject(process, 15000) == win32con.WAIT_OBJECT_0, 'Own QA process has not exited'
                report['exit_code'] = win32process.GetExitCodeProcess(process); process.Close()
                assert report['exit_code'] == 0, report['exit_code']
            if desktop is not None: desktop.close()
        except Exception:
            report['passed'] = False; report['cleanup_error'] = traceback.format_exc(); raise
        finally:
            if lock.read_text(encoding='utf-8') == token: lock.unlink()
            save()
    print(json.dumps({'passed': report['passed'], 'checks': len(report['checks']), 'exit_code': report['exit_code']}))


if __name__ == '__main__': main()
