"""Fault injection for QA restart/cleanup; never creates or kills a real process."""
from pathlib import Path
import json
import sqlite3
import sys
from types import SimpleNamespace

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import qa_full_scan_completion as qa


def prepare_run(tmp_path, monkeypatch, *, restart=False, complete=False, exit_code=0, terminate_error=False,
                close_error=False, desktop_error=False):
    root = tmp_path / 'isolated'
    root.mkdir()
    exe = tmp_path / 'qa.exe'
    exe.write_bytes(b'fake process identity; not executable')
    database = root / 'library.db'
    with sqlite3.connect(database) as db:
        db.execute('CREATE TABLE audio_files(id INTEGER PRIMARY KEY, file_name TEXT)')
        db.executemany('INSERT INTO audio_files(file_name) VALUES(?)',
                       [(f'voice-{n}.wav',) for n in range(49)] +
                       ([] if complete else [(f'tail-{n}.wav',) for n in range(51)]))
        db.execute('CREATE TABLE scan_foreground_completion(mini_state INTEGER)')
    if complete:
        with sqlite3.connect(root / 'waveforms.db') as db:
            db.execute('CREATE TABLE waveforms(file_id INTEGER, waveform_data BLOB)')
            db.executemany('INSERT INTO waveforms VALUES(?,?)', [(n, b'cached') for n in range(1, 50)])

    class Process:
        closed = False
        terminated = False
        close_calls = 0

        def Close(self):
            self.close_calls += 1
            if close_error:
                raise RuntimeError('handle close failed')
            self.closed = True

    process = Process()

    class Desktop:
        starts = 0
        closed = False

        def start(self, path):
            self.starts += 1
            if self.starts == 2:
                raise RuntimeError('restart failed')
            return process, 123

        def close(self):
            self.closed = True
            if desktop_error:
                raise RuntimeError('desktop cleanup failed')

    desktop = Desktop()

    class Driver:
        scanning = False
        started = 0
        total = 0

        def __init__(self, *args):
            pass

        def command(self, payload, **kwargs):
            if payload['cmd'] == 'magic_eq_identity':
                if not restart and not complete:
                    raise RuntimeError('identity request failed')
                return {'pid': 123, 'exe_path': str(exe), 'isolation_root': str(root)}
            if not restart and not complete:
                raise RuntimeError('cleanup transport failed')
            if payload['cmd'] == 'qa_rightclick_scan':
                self.scanning = not complete
                self.started += 1
                self.total = 49
            if payload['cmd'] == 'qa_scan_pause_keep':
                self.scanning = False
            if payload['cmd'] == 'info_page_pointer' and payload['phase'] == 'up':
                self.total = 0
            return {'scanning': self.scanning, 'scan_started_at_ms': self.started, 'processed': 100,
                    'scan_workflow_stage': 'completed', 'mini_waveforms_total': 49,
                    'mini_waveforms_completed': 49, 'errors': 0,
                    'scan_completion_pending': False, 'total': self.total}

    def wait(handle, timeout):
        if handle.closed:
            raise RuntimeError('closed process handle')
        return 0 if restart or complete or handle.terminated else 258

    def terminate(handle, code):
        if handle.closed or terminate_error:
            raise RuntimeError('terminate failed')
        handle.terminated = True

    monkeypatch.setitem(sys.modules, 'win32con', SimpleNamespace(WAIT_OBJECT_0=0, WAIT_TIMEOUT=258))
    monkeypatch.setitem(sys.modules, 'win32event', SimpleNamespace(WaitForSingleObject=wait))
    monkeypatch.setitem(sys.modules, 'win32process',
                        SimpleNamespace(GetExitCodeProcess=lambda handle: exit_code, TerminateProcess=terminate))
    monkeypatch.setattr(qa, 'PrivateDesktop', lambda: desktop)
    monkeypatch.setattr(qa, 'Driver', Driver)
    monkeypatch.setattr(qa, 'capture', lambda *args: None)
    monkeypatch.setattr(qa.os, 'link', lambda *args: None)
    monkeypatch.setattr(qa, 'wait_state', lambda *args, **kwargs: {
        'main_ui_ready': True, 'new_db_dialog_open': True,
        'db_path': str(database), 'db_switch_in_progress': False})
    args = SimpleNamespace(exe=exe, isolation_root=root, output=tmp_path / 'result',
                           duration=0, timeout=1, interrupt_fast=restart,
                           restart_after_interruption=restart, fast_first=False, pause_first=False)
    return args, process, desktop


def test_restart_failure_keeps_original_error_and_writes_report(tmp_path, monkeypatch):
    args, process, desktop = prepare_run(tmp_path, monkeypatch, restart=True)
    with pytest.raises(RuntimeError, match='^restart failed$'):
        qa.run(args)
    result = json.loads((args.output / 'result.json').read_text(encoding='utf-8'))
    assert result['error'] == 'restart failed' and result['passed'] is False
    assert 'cleanup_error' not in result
    assert desktop.closed and process.close_calls == 1


@pytest.mark.parametrize('failure,expected', [
    ('terminate_error', 'terminate failed'),
    ('close_error', 'handle close failed'),
    ('desktop_error', 'desktop cleanup failed'),
])
def test_cleanup_failure_still_closes_desktop_and_keeps_report(tmp_path, monkeypatch, failure, expected):
    args, process, desktop = prepare_run(tmp_path, monkeypatch, **{failure: True})
    with pytest.raises(RuntimeError, match='^identity request failed$'):
        qa.run(args)
    result = json.loads((args.output / 'result.json').read_text(encoding='utf-8'))
    assert result['error'] == 'identity request failed' and result['passed'] is False
    assert expected in result['cleanup_error']
    assert desktop.closed and process.close_calls == 1


@pytest.mark.parametrize('exit_code', [0, 7])
def test_report_tracks_exit_after_completed_scans(tmp_path, monkeypatch, exit_code):
    args, process, desktop = prepare_run(tmp_path, monkeypatch, complete=True, exit_code=exit_code)
    if exit_code:
        with pytest.raises(AssertionError):
            qa.run(args)
    else:
        qa.run(args)
    result = json.loads((args.output / 'result.json').read_text(encoding='utf-8'))
    assert len(result['cases']) == 2 and result['exit_code'] == exit_code
    assert result['passed'] is (exit_code == 0)
    assert desktop.closed and process.close_calls == 1
