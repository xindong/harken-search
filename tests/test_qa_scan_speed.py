"""Logic tests only: these do not launch or claim to validate a Harken EXE."""
import importlib
import json
import sqlite3
import sys
import wave
from pathlib import Path
from types import SimpleNamespace

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))


def tool():
    assert importlib.util.find_spec('qa_scan_speed') is not None, 'scan speed tool is not implemented'
    return importlib.import_module('qa_scan_speed')


def completed(**changes):
    status = dict(ok=True, scanning=False, scan_workflow_stage='completed',
        scan_phase='completed', scan_started_at_ms=100, scan_strategy='fast',
        scan_completion_pending=False, scan_completion_waveforms_pending=False,
        scan_completion_covers_pending=False, scan_completion_pending_files=0,
        scan_completion_mini_unresolved=0, scan_completion_cover_unresolved=0,
        scan_completion_metadata_unresolved=0, errors=0, failed_files=0,
        processed=3, discovered=3, succeeded=3, added=3, updated=0, skipped=0,
        removed=0, total=3, mini_waveforms_total=0, mini_waveforms_completed=0,
        scan_completion_duration_ms=275, error_details=[])
    status.update(changes)
    return status


def test_cli_defaults_and_full_are_explicit():
    qa = tool()
    args = qa.parse_args(['--exe', 'a.exe', '--isolation-root', 'root', '--output', 'out'])
    assert (args.count, args.repeats, args.full) == (2000, 3, False)
    assert args.timeout >= 60
    assert qa.parse_args(['--exe', 'a.exe', '--isolation-root', 'root',
                          '--output', 'out', '--full']).full


@pytest.mark.parametrize('option,value', [('--count', '0'), ('--count', '-1'),
    ('--repeats', '0'), ('--timeout', 'nan'), ('--timeout', 'inf'), ('--timeout', '0')])
def test_cli_rejects_unbounded_or_empty_measurements(option, value):
    with pytest.raises(SystemExit):
        tool().parse_args(['--exe', 'a.exe', '--isolation-root', 'r', '--output', 'o', option, value])


def paths(tmp_path):
    qa_root = tmp_path / 'qa'
    qa_root.mkdir()
    exe = qa_root / 'app.exe'
    exe.write_bytes(b'MZ test placeholder, never executed')
    return qa_root, exe, qa_root / 'isolated', qa_root / 'evidence'


def test_path_preflight_is_read_only_and_accepts_a_new_output(tmp_path):
    qa_root, exe, root, out = paths(tmp_path)
    assert tool().validate_paths(exe, root, out, qa_root=qa_root) == (exe, root, out)
    assert not root.exists() and not out.exists()


@pytest.mark.parametrize('case', ['relative-exe', 'relative-root', 'relative-out',
    'qa-root', 'outside-root', 'outside-out', 'existing-out', 'profile-output',
    'output-contains-root', 'same-root-output', 'missing-exe'])
def test_rejects_unsafe_paths_without_changing_anything(tmp_path, case):
    qa_root, exe, root, out = paths(tmp_path)
    if case == 'relative-exe': exe = Path('app.exe')
    if case == 'relative-root': root = Path('isolated')
    if case == 'relative-out': out = Path('evidence')
    if case == 'qa-root': root = qa_root
    if case == 'outside-root': root = tmp_path / 'personal'
    if case == 'outside-out': out = tmp_path / 'personal'
    if case == 'existing-out': out.mkdir(); (out / 'keep').write_text('untouched')
    if case == 'profile-output': out = root / 'profile' / 'results'
    if case == 'output-contains-root': root = out / 'root'
    if case == 'same-root-output': out = root
    if case == 'missing-exe': exe = qa_root / 'absent.exe'
    before = sorted(str(p) for p in tmp_path.rglob('*'))
    with pytest.raises((RuntimeError, ValueError)):
        tool().validate_paths(exe, root, out, qa_root=qa_root)
    assert sorted(str(p) for p in tmp_path.rglob('*')) == before


def test_rejects_symlink_or_junction_before_resolving(tmp_path):
    qa_root, exe, root, out = paths(tmp_path)
    target = qa_root / 'real'
    target.mkdir()
    if sys.platform == 'win32':
        import _winapi
        _winapi.CreateJunction(str(target), str(root))
    else:
        root.symlink_to(target, target_is_directory=True)
    with pytest.raises(RuntimeError, match='(?i)link|reparse'):
        tool().validate_paths(exe, root, out, qa_root=qa_root)


def test_prelaunch_literal_guard_rejects_another_root_or_ordinary_binary(tmp_path):
    qa = tool()
    exe, root = tmp_path / 'app.exe', tmp_path / 'isolated'
    exe.write_bytes(b'MZ ordinary binary')
    assert not qa.contains_root_literal(exe, root)
    exe.write_bytes(b'MZ\0' + (root.as_posix() + '-different').encode() + b'\0')
    assert not qa.contains_root_literal(exe, root)
    exe.write_bytes(b'MZ\0' + root.as_posix().encode() + b'\0')
    assert qa.contains_root_literal(exe, root)  # a string guard, NOT proof of a QA build


def test_deterministic_fixture_is_new_and_has_path_independent_identity(tmp_path):
    qa = tool()
    root = tmp_path / 'isolated'
    root.mkdir()
    first, second = root / 'first', root / 'second'
    a, b = qa.create_fixture(root, first, 3), qa.create_fixture(root, second, 3)
    assert a['content_sha256'] == b['content_sha256']
    assert a['parameters'] == b['parameters']
    assert len(a['files']) == 3
    for entry in a['files']:
        with wave.open(str(first / entry['name']), 'rb') as sound:
            assert (sound.getnchannels(), sound.getsampwidth(), sound.getframerate(), sound.getnframes()) == (1, 2, 16000, 16000)
    before = (first / a['files'][0]['name']).read_bytes()
    with pytest.raises((RuntimeError, FileExistsError)):
        qa.create_fixture(root, first, 3)
    assert (first / a['files'][0]['name']).read_bytes() == before
    with pytest.raises(RuntimeError):
        qa.create_fixture(root, tmp_path / 'outside', 3)
    assert not (tmp_path / 'outside').exists()


def test_shared_lock_rejects_contender_and_releases_its_owner(tmp_path):
    qa = tool()
    lock = qa.SharedLock(tmp_path, tmp_path / 'evidence')
    lock.acquire()
    assert lock.path.name == 'qa_scan_dialog_simple.lock'
    before = lock.path.read_bytes()
    with pytest.raises(FileExistsError):
        qa.SharedLock(tmp_path, tmp_path / 'other').acquire()
    assert lock.path.read_bytes() == before
    assert lock.release(process_exited=True)
    assert not lock.path.exists()


@pytest.mark.parametrize('replacement', [False, True])
def test_lock_never_removes_changed_owner_or_replaced_file(tmp_path, replacement):
    lock = tool().SharedLock(tmp_path, tmp_path / 'evidence')
    lock.acquire()
    if replacement:
        lock.path.rename(tmp_path / 'retained-first-owner')
    lock.path.write_text('{"owner":"somebody-else"}', encoding='utf-8')
    with pytest.raises(RuntimeError, match='owner'):
        lock.release(process_exited=True)
    assert json.loads(lock.path.read_text())['owner'] == 'somebody-else'


def test_unconfirmed_process_exit_keeps_shared_lock(tmp_path):
    lock = tool().SharedLock(tmp_path, tmp_path / 'evidence')
    lock.acquire()
    assert not lock.release(process_exited=False)
    assert lock.path.exists()


def test_stale_completed_snapshot_cannot_finish_new_run():
    assert not tool().scan_terminal(completed(), previous_start=100)
    assert tool().scan_terminal(completed(), previous_start=0)


@pytest.mark.parametrize('changes', [dict(scanning=True), dict(scan_completion_pending=True),
    dict(scan_completion_waveforms_pending=True), dict(scan_completion_covers_pending=True),
    dict(scan_completion_pending_files=1), dict(scan_completion_mini_unresolved=1),
    dict(scan_completion_cover_unresolved=1), dict(scan_completion_metadata_unresolved=1),
    dict(scan_workflow_stage='finalizing'), dict(scan_workflow_stage='idle')])
def test_waits_for_real_terminal_instead_of_thread_exit_or_counts(changes):
    assert not tool().scan_terminal(completed(scan_strategy='full', **changes), previous_start=0)


@pytest.mark.parametrize('debt', [dict(scan_completion_waveforms_pending=True),
    dict(scan_completion_covers_pending=True), dict(scan_completion_pending_files=7),
    dict(scan_completion_mini_unresolved=7), dict(scan_completion_cover_unresolved=7),
    dict(scan_completion_waveforms_pending=True, scan_completion_covers_pending=True,
         scan_completion_pending_files=7, scan_completion_mini_unresolved=7,
         scan_completion_cover_unresolved=7)])
def test_fast_terminal_does_not_wait_for_optional_background_debt(debt):
    qa = tool()
    status = completed(**debt)
    assert qa.scan_terminal(status, previous_start=50)
    qa.validate_result(status, dict(count=3, fixture_matches=True), 3, False)
    # Preserve the same strict foreground gate when the backend reports full.
    assert not qa.scan_terminal(dict(status, scan_strategy='full'), previous_start=50)


@pytest.mark.parametrize('changes', [dict(scanning=True), dict(scan_completion_pending=True),
    dict(scan_completion_metadata_unresolved=1), dict(scan_workflow_stage='finalizing'),
    dict(scan_workflow_stage='failed'), dict(scan_started_at_ms=50), dict(scan_started_at_ms=0)])
def test_fast_still_requires_new_completed_task_and_required_metadata(changes):
    status = completed(scan_completion_pending_files=7, scan_completion_cover_unresolved=7, **changes)
    assert not tool().scan_terminal(status, previous_start=50)


@pytest.mark.parametrize('strategy', [None, '', 'unknown'])
def test_terminal_does_not_guess_missing_or_unknown_strategy(strategy):
    status = completed(scan_strategy=strategy)
    if strategy is None:
        del status['scan_strategy']
    with pytest.raises(RuntimeError, match='scan_strategy'):
        tool().scan_terminal(status, previous_start=50)


def test_missing_status_fields_are_not_silently_treated_as_complete():
    state = completed()
    del state['scan_completion_pending']
    with pytest.raises(RuntimeError, match='scan_completion_pending'):
        tool().scan_terminal(state, previous_start=0)


@pytest.mark.parametrize('changes', [dict(errors=1), dict(failed_files=1),
    dict(processed=2), dict(succeeded=2), dict(added=2), dict(skipped=1),
    dict(scan_workflow_stage='failed'), dict(scan_strategy='full')])
def test_result_checks_reject_errors_partial_counts_and_wrong_mode(changes):
    with pytest.raises(RuntimeError):
        tool().validate_result(completed(**changes), {'count': 3, 'fixture_matches': True}, 3, False)


def test_full_requires_all_minis_and_persisted_nonempty_blobs():
    qa = tool()
    state = completed(scan_strategy='full', mini_waveforms_total=3, mini_waveforms_completed=3)
    db = dict(count=3, fixture_matches=True, cached_minis=3, pending_minis=0)
    qa.validate_result(state, db, 3, True)
    for changed in (dict(cached_minis=2), dict(pending_minis=1), dict(fixture_matches=False), dict(count=4)):
        with pytest.raises(RuntimeError):
            qa.validate_result(state, dict(db, **changed), 3, True)
    with pytest.raises(RuntimeError):
        qa.validate_result(dict(state, mini_waveforms_completed=2), db, 3, True)


def test_fast_does_not_require_full_cache_but_still_checks_rows():
    tool().validate_result(completed(), dict(count=3, fixture_matches=True), 3, False)
    with pytest.raises(RuntimeError):
        tool().validate_result(completed(), dict(count=2, fixture_matches=True), 3, False)


def test_readback_reads_only_owned_database_and_durable_mini_blobs(tmp_path):
    qa = tool()
    root = tmp_path / 'isolated'
    library = root / 'profile' / 'libraries' / 'ScanSpeedQA-test'
    library.mkdir(parents=True)
    database = library / 'main.db'
    expected = [root / 'fixture' / f'{i}.wav' for i in range(3)]
    with sqlite3.connect(database) as db:
        db.execute('CREATE TABLE audio_files(id INTEGER PRIMARY KEY, file_path TEXT)')
        db.executemany('INSERT INTO audio_files VALUES (?, ?)', [(i + 1, str(p)) for i, p in enumerate(expected)])
        db.execute('CREATE TABLE scan_foreground_completion(file_id INTEGER, mini_state INTEGER)')
        db.executemany('INSERT INTO scan_foreground_completion VALUES (?, 1)', [(i + 1,) for i in range(3)])
    with sqlite3.connect(library / 'waveforms.db') as db:
        db.execute('CREATE TABLE waveforms(file_id INTEGER PRIMARY KEY, waveform_data BLOB)')
        db.executemany('INSERT INTO waveforms VALUES (?, ?)', [(1, b'pcm'), (2, b'pcm'), (3, b'')])
    before = database.read_bytes()
    result = qa.database_evidence(database, root, expected, full=True)
    assert result == dict(count=3, fixture_matches=True, cached_minis=2, pending_minis=0)
    assert database.read_bytes() == before
    with pytest.raises(RuntimeError):
        qa.database_evidence(tmp_path / 'real-library.db', root, expected, full=True)
    assert not (tmp_path / 'real-library.db').exists()


def test_stage_timing_keeps_reentries_and_unobserved_stages_honest():
    qa = tool()
    timeline = qa.ScanTimeline(started_at=10.0)
    timeline.add(completed(scan_phase='enumerating', scan_workflow_stage='enumerating', scanning=True), 10.25)
    timeline.add(completed(scan_phase='processing', scan_workflow_stage='processing', scanning=True), 11.0)
    timeline.add(completed(scan_phase='enumerating', scan_workflow_stage='enumerating', scanning=True), 12.0)
    timeline.add(completed(), 13.0)
    result = timeline.summary()
    assert result['elapsed_seconds'] == 3
    assert result['first_observation_seconds'] == .25
    assert result['max_poll_gap_seconds'] == 1
    assert result['observed_stage_seconds']['enumerating/enumerating'] == 1.75
    assert result['observed_stage_seconds']['processing/processing'] == 1
    assert 'waveform_cache' not in result['observed_stage_seconds']
    assert [item['scan_status']['processed'] for item in timeline.samples] == [3, 3, 3, 3]
    with pytest.raises(RuntimeError, match='monotonic'):
        timeline.add(completed(), 12.5)


def test_summary_counts_failed_rounds_without_benchmarking_them_as_success():
    result = tool().summarize_runs([
        dict(status='passed', elapsed_seconds=2),
        dict(status='failed', elapsed_seconds=1),
        dict(status='passed', elapsed_seconds=4),
        dict(status='passed', elapsed_seconds=6)], count=12)
    assert result['status'] == 'failed'
    assert (result['passed_repeats'], result['failed_repeats']) == (3, 1)
    assert result['median_seconds'] == 4
    assert result['mean_seconds'] == 4
    assert result['min_seconds'] == 2 and result['max_seconds'] == 6
    assert result['median_files_per_second'] == 3
    assert result['first_run_seconds'] == 2
    assert result['subsequent_median_seconds'] == 5


def test_no_success_is_not_a_zero_second_benchmark():
    result = tool().summarize_runs([dict(status='failed')], count=3)
    assert result['status'] == 'failed' and result['median_seconds'] is None


@pytest.mark.parametrize('field', ['pid', 'exe_path', 'isolation_root', 'runtime_root'])
def test_identity_rejects_wrong_pid_exe_or_fixed_roots(tmp_path, field):
    qa = tool()
    root, exe = tmp_path / 'root', tmp_path / 'a.exe'
    identity = dict(pid=42, exe_path=str(exe), isolation_root=str(root), runtime_root=str(root / 'profile/runtime'))
    qa.verify_identity(identity, 42, exe, root)
    identity[field] = 99 if field == 'pid' else str(tmp_path / 'another')
    with pytest.raises(RuntimeError, match=field):
        qa.verify_identity(identity, 42, exe, root)


def test_process_guard_detects_other_package_using_same_root_without_commands(tmp_path):
    qa = tool()
    root = tmp_path / 'root'
    first, other, unrelated = [tmp_path / name for name in ('HarkenSearch.exe', 'other.exe', 'personal.exe')]
    for path in (first, other):
        path.write_bytes(b'MZ\0' + root.as_posix().encode() + b'\0')
    unrelated.write_bytes(b'MZ ordinary application')
    rows = [dict(ProcessId=i, Name=p.name, ExecutablePath=str(p)) for i, p in enumerate((other, unrelated), 40)]
    assert qa.conflicting_processes(rows, first, root, qa_root=tmp_path) == [40]
    assert unrelated.read_bytes() == b'MZ ordinary application'


class FakeClock:
    now = 10.0

    def monotonic(self):
        return self.now

    def sleep(self, seconds):
        self.now += seconds


@pytest.mark.parametrize('full', [False, True])
def test_measurement_ends_at_fast_foreground_gate_but_full_waits_for_drain(tmp_path, monkeypatch, full):
    qa = tool()
    clock = FakeClock()
    monkeypatch.setattr(qa.time, 'monotonic', clock.monotonic)
    monkeypatch.setattr(qa.time, 'sleep', clock.sleep)
    strategy = 'full' if full else 'fast'
    debt = completed(scan_strategy=strategy, scan_completion_pending_files=3,
        scan_completion_cover_unresolved=3, scan_completion_mini_unresolved=3,
        scan_completion_covers_pending=True, scan_completion_waveforms_pending=True)
    sequence = [completed(scan_strategy=strategy, scan_started_at_ms=50),
        dict(debt, scan_completion_metadata_unresolved=1), debt, completed(scan_strategy=strategy)]
    status_reads = []

    class BridgeDouble:
        def command(self, payload, timeout=25):
            clock.now += .2
            if payload['cmd'] == 'qa_rightclick_scan':
                assert payload['cache_waveforms'] is full
                return {'ok': True}
            assert payload['cmd'] == 'scan_status'
            status = sequence.pop(0)
            status_reads.append(status)
            return status

    result = qa.measure_scan(BridgeDouble(), tmp_path / 'fixture', full, 5, tmp_path)
    assert result['elapsed_seconds'] == pytest.approx(1.0 if full else .7)
    assert len(status_reads) == (4 if full else 3)
    # A fast measurement still records deferred debt as evidence, not as zero.
    assert result['terminal']['scan_completion_cover_unresolved'] == (0 if full else 3)
    samples = [json.loads(line) for line in (tmp_path / 'scan_samples.jsonl').read_text().splitlines()]
    assert samples[-1]['scan_status'] == result['terminal']


@pytest.mark.parametrize('failure', [None, 'timeout', 'failed', 'bridge'])
def test_measurement_counts_launch_latency_and_retains_each_observation(tmp_path, monkeypatch, failure):
    qa = tool()
    clock = FakeClock()
    monkeypatch.setattr(qa.time, 'monotonic', clock.monotonic)
    monkeypatch.setattr(qa.time, 'sleep', clock.sleep)
    sequence = [completed(scan_started_at_ms=50),
                completed(scan_started_at_ms=50),  # previous scan must not complete this one
                completed(scanning=True, scan_phase='processing', scan_workflow_stage='ingesting'),
                completed(scan_completion_pending=True, scan_workflow_stage='finalizing'),
                completed()]
    if failure == 'failed':
        sequence[-1] = completed(scan_workflow_stage='failed', errors=1)
    if failure == 'timeout':
        sequence[-1] = sequence[-2]
    calls = []

    class Driver:
        def command(self, payload, timeout=25):
            calls.append(payload)
            assert 0 < timeout <= 25
            clock.now += .2
            if payload['cmd'] == 'qa_rightclick_scan':
                assert payload['cache_waveforms'] is False
                return {'ok': True}
            assert payload['cmd'] == 'scan_status'
            if failure == 'bridge' and len(calls) == 5:
                raise TimeoutError('test bridge timeout')
            return sequence.pop(0) if len(sequence) > 1 else sequence[0]

    if failure:
        with pytest.raises((RuntimeError, TimeoutError)):
            qa.measure_scan(Driver(), tmp_path / 'fixture', False, .95 if failure == 'timeout' else 5, tmp_path)
    else:
        result = qa.measure_scan(Driver(), tmp_path / 'fixture', False, 5, tmp_path)
        assert result['elapsed_seconds'] == pytest.approx(1.3)
        assert result['terminal']['scan_workflow_stage'] == 'completed'
    samples = [json.loads(line) for line in (tmp_path / 'scan_samples.jsonl').read_text().splitlines()]
    assert samples and samples[0]['scan_status']['scan_started_at_ms'] == 50
    assert samples[0]['elapsed_seconds'] == pytest.approx(.4)
    assert (tmp_path / 'scan-start.json').exists()
    assert calls[1]['cmd'] == 'qa_rightclick_scan'


@pytest.mark.parametrize('waits,verified,expected_exit,terminations', [
    ([0], False, True, 0), ([258, 0], True, True, 0),
    ([258, 258, 0], False, True, 1), ([258, 258, 258], True, False, 1)])
def test_shutdown_only_uses_owned_handle_and_confirms_exit(monkeypatch, waits, verified, expected_exit, terminations):
    qa = tool()
    events, pending = [], list(waits)
    handle = SimpleNamespace(Close=lambda: events.append('closed'))

    def wait(actual, timeout):
        assert actual is handle
        return pending.pop(0)

    def terminate(actual, code):
        assert actual is handle and code == 1
        events.append('terminate-owned')

    monkeypatch.setitem(sys.modules, 'win32con', SimpleNamespace(WAIT_OBJECT_0=0, WAIT_TIMEOUT=258))
    monkeypatch.setitem(sys.modules, 'win32event', SimpleNamespace(WaitForSingleObject=wait))
    monkeypatch.setitem(sys.modules, 'win32process', SimpleNamespace(
        TerminateProcess=terminate, GetExitCodeProcess=lambda _: 0))
    report = {}
    assert qa.stop_owned_process(handle, (lambda: events.append('exit-owned')) if verified else None, report) == expected_exit
    assert events.count('terminate-owned') == terminations
    assert events.count('closed') == 1
    if not verified:
        assert 'exit-owned' not in events
    if not expected_exit:
        assert 'cleanup_errors' in report


def test_failed_startup_retains_report_and_never_launches_or_removes_another_lock(tmp_path, monkeypatch):
    qa = tool()
    qa_root, exe, root, out = paths(tmp_path)
    args = qa.parse_args(['--exe', str(exe), '--isolation-root', str(root), '--output', str(out)])
    runtime = root / 'profile/runtime'
    runtime.mkdir(parents=True)
    lock = runtime / 'qa_scan_dialog_simple.lock'
    lock.write_text('another-owner', encoding='utf-8')
    original = qa.validate_paths
    monkeypatch.setattr(qa, 'validate_paths', lambda *a: original(*a, qa_root=qa_root))
    monkeypatch.setattr(qa, 'contains_root_literal', lambda *a: True)
    monkeypatch.setattr(qa, 'check_runtime_dependencies', lambda: None)
    monkeypatch.setattr(qa, 'PrivateDesktop', lambda: pytest.fail('Must not launch an EXE in this test'))
    assert qa.run(args) == 1
    assert lock.read_text(encoding='utf-8') == 'another-owner'
    report = json.loads((out / 'report.json').read_text(encoding='utf-8'))
    assert report['status'] == 'failed' and report['traceback']
    assert not report['runs']


def test_diagnostic_jsonl_is_copied_before_next_launch_without_changing_source(tmp_path):
    qa = tool()
    root, output = tmp_path / 'isolation', tmp_path / 'evidence'
    runtime = root / 'profile/runtime'
    runtime.mkdir(parents=True)
    output.mkdir()
    log = runtime / 'aura_diagnostics_live.jsonl'
    content = b'{"event":"ScanPhase"}\n{"event":"ScanIndexComplete","ingestMs":123}\n'
    log.write_bytes(content)
    result = qa.preserve_diagnostics(root, output)
    assert result['present'] and Path(result['copy']).read_bytes() == content
    assert log.read_bytes() == content
    assert result['sha256'] == qa.sha256(log)
    log.write_bytes(b'next launch truncates only the isolated source')
    with pytest.raises(FileExistsError):
        qa.preserve_diagnostics(root, output)
    assert Path(result['copy']).read_bytes() == content


def test_missing_diagnostic_log_is_reported_not_fabricated(tmp_path):
    result = tool().preserve_diagnostics(tmp_path / 'root', tmp_path)
    assert result['present'] is False
    assert not (tmp_path / 'aura_diagnostics_live.jsonl').exists()


def test_cleanup_failure_cannot_be_reported_as_successful_summary():
    result = tool().summarize_runs([dict(status='passed', elapsed_seconds=2)], 3, overall_status='failed')
    assert result['status'] == 'failed' and result['median_seconds'] == 2


@pytest.mark.parametrize('opening', ['hidden', 'visible', 'unavailable'])
def test_finish_opens_real_result_then_uses_done_and_waits_for_setup(tmp_path, monkeypatch, opening):
    qa = tool()
    clock = FakeClock()
    monkeypatch.setattr(qa.time, 'monotonic', clock.monotonic)
    monkeypatch.setattr(qa.time, 'sleep', clock.sleep)
    events = []

    class NativeBridgeDouble:
        frame = 0
        closed = False
        closed_reads = 0
        opened = opening == 'visible'
        visible_after = 0

        def command(self, payload, **kwargs):
            if payload['cmd'] == 'open_scan_dialog':
                events.append('open')
                self.opened = opening != 'unavailable'
                self.visible_after = self.frame + 2
                return {'ok': True}
            assert payload['cmd'] == 'scan_dialog_ui'
            if payload['action'] == 'pointer':
                assert self.opened and self.frame > self.visible_after, 'Cannot click an unrendered result'
                events.append(payload['phase'])
                if payload['phase'] == 'up':
                    self.closed = True
                return {'ok': True}
            self.frame += 1
            if self.closed:
                self.closed_reads += 1
            visible = self.opened and self.frame > self.visible_after and not self.closed
            # Match the real failure: logical result exists while items/windows
            # are empty. Disappearing controls alone also do not prove Done reset.
            return dict(surface='setup' if self.closed_reads >= 3 else 'result', phase='completed', frame=self.frame,
                theme=0, is_scanning=False, completion_pending=False, pending_folders=[], last_folders=[],
                succeeded=3, failed=0, skipped=0, unprocessed=0, imgui_errors=0, imgui_debug_log='',
                items=[dict(id='scan.result.done', window='##ScanResult', bounds=[0, 0, 20, 20])] if visible else [],
                windows=[dict(name='##ScanResult', bounds=[0, 0, 100, 100])] if visible else [],
                progress=dict(known=True, current=3, total=3), metrics=dict(waiting_for_progress=False))

    driver = NativeBridgeDouble()
    if opening == 'unavailable':
        with pytest.raises(TimeoutError, match='Native result did not appear'):
            qa.finish_result(driver, tmp_path, tmp_path)
        assert events == ['open'] and not driver.closed
        return
    final_ui = qa.finish_result(driver, tmp_path, tmp_path)
    assert events == ['open', 'hover', 'down', 'up'] and driver.closed_reads >= 3
    assert final_ui['surface'] == 'result' and final_ui['items'] and final_ui['windows']
    assert final_ui['progress'] == dict(known=True, current=3, total=3)
