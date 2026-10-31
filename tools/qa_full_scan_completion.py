"""Run real full scans in an isolated QA EXE, on a private Windows desktop.

No shared mouse or keyboard input. Fails if required mini caches do not finish.
"""
import argparse
from array import array
import hashlib
import json
import math
import os
from pathlib import Path
import sqlite3
import time
import uuid
import wave

from qa_folder_shortcuts import Driver
from qa_required_library_setup import PrivateDesktop, capture, wait_state


def run(args):
    import win32con
    import win32event
    import win32process

    exe, root, out = args.exe.resolve(), args.isolation_root.resolve(), args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    runtime = root / 'profile/runtime'
    runtime.mkdir(parents=True, exist_ok=True)
    fixture = root / ('scan-fixture-' + uuid.uuid4().hex[:8]) / 'zz-target'
    fixture.mkdir(parents=True)
    for index in range(49):
        rate = 48000
        frames = array('h', (int(15000 * math.sin(2 * math.pi * (220 + index) * n / rate))
                            for n in range(rate)))
        with wave.open(str(fixture / f'voice-{index:02}.wav'), 'wb') as sound:
            sound.setparams((1, 2, rate, 0, 'NONE', 'not compressed'))
            for _ in range(int(args.duration)):
                sound.writeframes(frames.tobytes())
            sound.writeframes(frames[:int(rate * (args.duration % 1))].tobytes())
    report = {'exe': str(exe), 'sha256': hashlib.sha256(exe.read_bytes()).hexdigest(),
              'fixture': str(fixture), 'cases': [], 'passed': False}
    desktop = PrivateDesktop()
    process = None
    driver = Driver(runtime, out)
    def start_scan(path, full):
        previous_start = driver.command({'cmd': 'scan_status'})['scan_started_at_ms']
        driver.command({'cmd': 'qa_rightclick_scan', 'path': str(path),
                        'cache_waveforms': full})
        until = time.monotonic() + 10
        while True:
            status = driver.command({'cmd': 'scan_status'})
            if status['scanning'] or status['scan_started_at_ms'] != previous_start:
                break
            assert time.monotonic() < until, 'Native scan dialog did not start'
            time.sleep(.03)

    def dismiss_result():
        driver.command({'cmd': 'open_scan_dialog'})
        driver.command({'cmd': 'info_page_state'})
        for phase in ('hover', 'down', 'up'):
            time.sleep(.15)
            driver.command({'cmd': 'info_page_pointer', 'key': 'info.qaScanDone', 'phase': phase})
        until = time.monotonic() + 5
        while driver.command({'cmd': 'scan_status'})['total'] != 0:
            assert time.monotonic() < until, 'Native Done button did not dismiss scan results'
            time.sleep(.05)
    try:
        process, pid = desktop.start(exe)
        identity = driver.command({'cmd': 'magic_eq_identity'}, timeout=60)
        assert identity['pid'] == pid and Path(identity['exe_path']).resolve() == exe, identity
        assert Path(identity['isolation_root']).resolve() == root, identity
        report['identity'] = identity
        state = wait_state(driver, lambda s: s['main_ui_ready'], 'Main UI ready', timeout=60)
        if not state['new_db_dialog_open']:
            driver.command({'cmd': 'info_edit_input', 'action': 'new_db_dialog'})
        wait_state(driver, lambda s: s['new_db_dialog_open'], 'New library dialog')
        driver.command({'cmd': 'info_edit_input', 'action': 'new_db_name',
                        'text': 'MiniScan-' + uuid.uuid4().hex[:8]})
        driver.command({'cmd': 'info_edit_input', 'action': 'new_db_create'})
        state = wait_state(driver, lambda s: bool(s['db_path']) and not s['db_switch_in_progress']
                           and not s['new_db_dialog_open'], 'Create isolated library')
        database = Path(state['db_path']).resolve()
        database.relative_to(root)
        report['database'] = str(database)
        expected_count = 49
        if args.interrupt_fast:
            tail = fixture.parent / 'tail'
            tail.mkdir()
            for index in range(5000):
                os.link(fixture / f'voice-{index % 49:02}.wav', tail / f'tail-{index:05}.wav')
            start_scan(fixture.parent, False)
            until = time.monotonic() + 90
            while True:
                status = driver.command({'cmd': 'scan_status'})
                with sqlite3.connect(database.as_uri() + '?mode=ro', uri=True) as db:
                    admitted = db.execute('SELECT COUNT(*) FROM audio_files').fetchone()[0]
                    target_admitted = db.execute('SELECT COUNT(*) FROM audio_files WHERE file_name LIKE ?',
                                                 ('voice-%',)).fetchone()[0]
                if status['scanning'] and 49 < admitted < 5049 and target_admitted == 49:
                    report['interrupted_fast'] = status
                    report['admitted_at_pause'] = admitted
                    driver.command({'cmd': 'qa_scan_pause_keep'})
                    break
                if not status['scanning'] and status['processed'] == 5049:
                    raise AssertionError('Fast scan finished before the target partial state was observed')
                assert time.monotonic() < until, status
                time.sleep(.04)
            while driver.command({'cmd': 'scan_status'})['scanning']:
                assert time.monotonic() < until, 'Interrupted fast scan did not stop'
                time.sleep(.04)
            with sqlite3.connect(database.as_uri() + '?mode=ro', uri=True) as db:
                expected_count = db.execute('SELECT COUNT(*) FROM audio_files').fetchone()[0]
                target_count = db.execute('SELECT COUNT(*) FROM audio_files WHERE file_name LIKE ?',
                                          ('voice-%',)).fetchone()[0]
            assert target_count == 49 and 49 < expected_count < 5049, (target_count, expected_count)
            report['kept_after_interrupt'] = expected_count
            if args.restart_after_interruption:
                driver.command({'cmd': 'info_edit_input', 'action': 'exit'})
                assert win32event.WaitForSingleObject(process, 15000) == win32con.WAIT_OBJECT_0
                assert win32process.GetExitCodeProcess(process) == 0
                process.Close()
                process = None
                process, pid = desktop.start(exe)
                identity = driver.command({'cmd': 'magic_eq_identity'}, timeout=60)
                assert identity['pid'] == pid
                wait_state(driver, lambda s: s['main_ui_ready'] and s['db_path'] and
                           not s['db_switch_in_progress'], 'Reopen interrupted library', timeout=60)
                report['restart_identity'] = identity
        if args.fast_first or args.pause_first:
            start_scan(fixture, not args.fast_first)
            until = time.monotonic() + args.timeout
            while True:
                status = driver.command({'cmd': 'scan_status'})
                if args.pause_first and status['scan_phase'] == 'waveform_cache' and status['scanning']:
                    report['before_pause'] = status
                    driver.command({'cmd': 'qa_scan_pause_keep'})
                    break
                if not status['scanning'] and status['processed'] == 49:
                    assert not args.pause_first, 'Full scan ended before pause could be exercised'
                    break
                assert time.monotonic() < until, status
                time.sleep(.04)
            while driver.command({'cmd': 'scan_status'})['scanning']:
                assert time.monotonic() < until, 'Seed scan did not stop'
                time.sleep(.04)
            if args.fast_first:
                dismiss_result()
        for case in ('initial-full', 'repeat-full'):
            observed = []
            case_report = {'case': case, 'observed': observed}
            report['cases'].append(case_report)
            start_scan(fixture, True)
            start = time.monotonic()
            while time.monotonic() - start < args.timeout:
                status = driver.command({'cmd': 'scan_status'})
                observed.append(dict(elapsed=round(time.monotonic() - start, 3), **status))
                if not status['scanning'] and status['scan_workflow_stage'] == 'failed':
                    capture(driver, out / (case + '-failed.png'))
                    raise AssertionError(f'{case}: full scan failed: {status["error_details"]}')
                if not status['scanning'] and status['scan_workflow_stage'] == 'completed':
                    break
                time.sleep(.15)
            else:
                capture(driver, out / (case + '-stalled.png'))
                raise AssertionError(f'{case}: full scan did not complete: {status}')
            assert status['mini_waveforms_total'] == 49, status
            assert status['mini_waveforms_completed'] == 49 and status['errors'] == 0, status
            assert not status['scan_completion_pending'], status
            with sqlite3.connect(database.as_uri() + '?mode=ro', uri=True) as db:
                count = db.execute('SELECT COUNT(*) FROM audio_files').fetchone()[0]
                pending = db.execute('SELECT COUNT(*) FROM scan_foreground_completion WHERE mini_state=0').fetchone()[0]
                db.execute('ATTACH DATABASE ? AS cache',
                           (database.with_name('waveforms.db').as_uri() + '?mode=ro',))
                cached = db.execute('SELECT COUNT(*) FROM audio_files af JOIN cache.waveforms w '
                                    'ON w.file_id=af.id WHERE af.file_name LIKE ? '
                                    'AND length(w.waveform_data) > 0', ('voice-%',)).fetchone()[0]
            assert count == expected_count and pending == 0, (count, pending)
            assert cached == 49, f'Only {cached}/49 real mini blobs persisted'
            case_report.update(seconds=round(time.monotonic() - start, 3), count=count,
                               pending=pending, cached=cached)
            driver.command({'cmd': 'open_scan_dialog'})
            capture(driver, out / (case + '-complete.png'))
            dismiss_result()
        report['passed'] = True
    except Exception as error:
        report['error'] = str(error)
        raise
    finally:
        cleanup_errors = []
        if process is not None:
            try:
                if win32event.WaitForSingleObject(process, 0) == win32con.WAIT_TIMEOUT:
                    status = driver.command({'cmd': 'scan_status'})
                    if status['scanning']:
                        driver.command({'cmd': 'qa_scan_pause_keep'})
                        until = time.monotonic() + 15
                        while driver.command({'cmd': 'scan_status'})['scanning']:
                            if time.monotonic() > until:
                                raise TimeoutError('Pause did not stop the QA scan')
                            time.sleep(.1)
                    driver.command({'cmd': 'info_edit_input', 'action': 'exit'})
                    if win32event.WaitForSingleObject(process, 15000) != win32con.WAIT_OBJECT_0:
                        raise TimeoutError('QA app did not exit')
                report['exit_code'] = win32process.GetExitCodeProcess(process)
            except Exception as error:
                cleanup_errors.append(str(error))
                try:
                    win32process.TerminateProcess(process, 1)
                    win32event.WaitForSingleObject(process, 5000)
                except Exception as terminate_error:
                    cleanup_errors.append(str(terminate_error))
            finally:
                try:
                    process.Close()
                except Exception as close_error:
                    cleanup_errors.append(str(close_error))
        try:
            desktop.close()
        except Exception as desktop_error:
            cleanup_errors.append(str(desktop_error))
        if cleanup_errors:
            report['passed'] = False
            report['cleanup_error'] = '; '.join(cleanup_errors)
        report['passed'] = report['passed'] and report.get('exit_code') == 0
        (out / 'result.json').write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
    assert report.get('exit_code') == 0 and 'cleanup_error' not in report, report
    print('PASS: initial and repeated full scans complete all 49 mini waveforms; clean exit')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exe', type=Path, required=True)
    parser.add_argument('--isolation-root', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--timeout', type=float, default=30)
    parser.add_argument('--duration', type=float, default=.85)
    parser.add_argument('--fast-first', action='store_true')
    parser.add_argument('--pause-first', action='store_true')
    parser.add_argument('--interrupt-fast', action='store_true')
    parser.add_argument('--restart-after-interruption', action='store_true')
    run(parser.parse_args())
