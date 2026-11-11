"""Measure native EXE scans on a private desktop, retaining every run's evidence.

Windows + pywin32 + Pillow; use only an isolation-enabled QA build compiled for
--isolation-root. Root and NEW --output must be below D:/Harken-QA. Compare two
builds sequentially with the same root, count, repeats and --full setting, using
different outputs. Fixtures have identical names/bytes and a content digest;
each invocation creates a new fixture directory and each repeat a new database.
No existing audio, database or evidence is overwritten/deleted.

Timing starts BEFORE qa_rightclick_scan and ends at observed backend completion.
Fast completion includes required metadata but excludes optional background
cover/mini debt; full completion requires all foreground completion counters.
Stage durations are polling estimates, not profiler measurements; unseen phases
are not reported as zero. First-run and subsequent timings are separate; this
does not flush OS caches or prove cold-disk speed. No human/UI acceptance claim.
Exit 0: measured checks pass; 1: failed run; 2: rejected preflight.

Example (both EXEs must be built for D:/Harken-QA/scan-speed-isolation):
  python tools/qa_scan_speed.py --exe D:/Harken-QA/baseline/HarkenSearch.exe \
    --isolation-root D:/Harken-QA/scan-speed-isolation \
    --output D:/Harken-QA/speed-before --count 2000 --repeats 3 --full
Then repeat with the candidate EXE and a NEW --output, keeping other flags fixed.
"""
from __future__ import annotations

import argparse
from contextlib import closing
import hashlib
import json
import math
import mmap
import os
from pathlib import Path
import re
import shutil
import sqlite3
import stat
import statistics
import subprocess
import sys
import time
import traceback
import uuid

from qa_required_library_setup import PrivateDesktop, capture
from qa_transport_themes import Driver
from qa_scan_dialog_simple import Smoke, inside, make_wavs, path_key, require, save_json, sha256, utc_now

QA_ROOT = Path('D:/Harken-QA')
POLL_SECONDS = .10
PENDING_FLAGS = ('scan_completion_pending', 'scan_completion_waveforms_pending',
                 'scan_completion_covers_pending')
PENDING_COUNTS = ('scan_completion_pending_files', 'scan_completion_mini_unresolved',
                  'scan_completion_cover_unresolved', 'scan_completion_metadata_unresolved')


def parse_args(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    for option in ('exe', 'isolation-root', 'output'):
        parser.add_argument('--' + option, type=Path, required=True)
    parser.add_argument('--count', type=int, default=2000)
    parser.add_argument('--repeats', type=int, default=3)
    parser.add_argument('--full', action='store_true', help='Require persisted mini waveforms as well as import')
    parser.add_argument('--timeout', type=float, default=600, help='Per scan deadline in seconds (default: 600)')
    args = parser.parse_args(argv)
    if args.count < 1 or args.repeats < 1:
        parser.error('count and repeats must be positive')
    if not math.isfinite(args.timeout) or args.timeout <= 0:
        parser.error('timeout must be finite and positive')
    return args


def no_links(path):
    """Check existing ancestors before resolve(), including Windows junctions."""
    for part in (path, *path.parents):
        try:
            info = part.lstat()
        except FileNotFoundError:
            continue
        require(not stat.S_ISLNK(info.st_mode) and
                not getattr(info, 'st_file_attributes', 0) & stat.FILE_ATTRIBUTE_REPARSE_POINT,
                f'Link/reparse path is not allowed: {part}')


def validate_paths(exe, root, output, *, qa_root=QA_ROOT):
    for value in (exe, root, output):
        require(value.is_absolute(), f'Absolute path required: {value}')
        no_links(value)
    exe, root, output, qa_root = (p.resolve() for p in (exe, root, output, qa_root))
    require(exe.is_file(), 'EXE does not exist')
    require(inside(root, qa_root) and inside(output, qa_root), 'Root/output must be below D:/Harken-QA')
    require(not output.exists(), 'Output must be a new directory; retain existing evidence')
    require(not root.is_relative_to(output) and not output.is_relative_to(root / 'profile'),
            'Output overlaps isolation root/profile')
    no_links(root / 'profile' / 'runtime')
    require(not (root / 'profile/runtime/qa_cmd.json').exists(), 'A QA command is already pending')
    return exe, root, output


def contains_root_literal(exe, root):
    """Conservative prelaunch guard, NOT a substitute for runtime identity.

    Isolation builds contain kCompileTimePreviewRootUtf8 as a NUL-terminated
    constant. Matching a prefix (root-other) must never authorize a launch.
    """
    literal = root.as_posix().rstrip('/').encode('utf-8')
    pattern = re.compile(rb'(?:^|\x00)' + re.escape(literal) + rb'/?\x00', re.IGNORECASE)
    with exe.open('rb') as source:
        if os.fstat(source.fileno()).st_size == 0:
            return False
        with mmap.mmap(source.fileno(), 0, access=mmap.ACCESS_READ) as data:
            return bool(pattern.search(data))


def create_fixture(root, folder, count):
    require(count > 0 and inside(folder, root) and not folder.is_relative_to(root / 'profile'),
            'Fixture must be a new directory inside isolation root, outside profile')
    no_links(folder)
    make_wavs(folder, count=count, seconds=1)
    entries = [dict(name=p.name, sha256=sha256(p), bytes=p.stat().st_size)
               for p in sorted(folder.glob('*.wav'))]
    parameters = dict(version=1, count=count, channels=1, sample_width_bytes=2,
                      sample_rate=16000, seconds=1, tones=12, generator='qa_scan_dialog_simple.make_wavs')
    digest = hashlib.sha256(json.dumps(dict(parameters=parameters, files=entries),
        sort_keys=True, separators=(',', ':')).encode('utf-8')).hexdigest()
    return dict(directory=str(folder), parameters=parameters, files=entries, content_sha256=digest)


class SharedLock:
    """Use the existing scan helpers' atomic filename and verify owner on release."""
    def __init__(self, runtime, output):
        self.path = runtime / 'qa_scan_dialog_simple.lock'
        self.contents = json.dumps(dict(helper_pid=os.getpid(), output=str(output), owner=uuid.uuid4().hex))
        self.identity = None

    def acquire(self):
        with self.path.open('x', encoding='utf-8') as file:
            self.identity = os.fstat(file.fileno())
            file.write(self.contents)
            file.flush()

    def check_owner(self):
        require(self.identity is not None and
                os.path.samestat(self.identity, self.path.stat(follow_symlinks=False)) and
                self.path.read_text(encoding='utf-8') == self.contents,
                'Shared lock owner changed; retaining the lock')

    def release(self, *, process_exited):
        if not process_exited:
            return False
        self.check_owner()
        self.path.unlink()  # Only our temporary lock; retain every fixture and report.
        return True


def scan_terminal(status, previous_start):
    needed = {'scanning', 'scan_started_at_ms', 'scan_workflow_stage', 'scan_strategy',
              *PENDING_FLAGS, *PENDING_COUNTS}
    require(needed <= status.keys(), 'scan_status missing fields: ' + str(sorted(needed - status.keys())))
    require(status['scan_strategy'] in ('fast', 'full'), 'Unknown scan_strategy')
    # ScanWorkflow.h releases fast foreground completion while optional cover/
    # mini work continues. pending_files includes that debt, not just metadata.
    # Keep its observed values in the journal; only full scans wait for it here.
    gates = (*PENDING_FLAGS, *PENDING_COUNTS) if status['scan_strategy'] == 'full' else (
        'scan_completion_pending', 'scan_completion_metadata_unresolved')
    fresh = status['scan_started_at_ms'] > 0 and status['scan_started_at_ms'] != previous_start
    return fresh and not status['scanning'] and status['scan_workflow_stage'] == 'completed' and \
        not any(status[k] for k in gates)


def validate_result(status, evidence, count, full):
    require(scan_terminal(status, previous_start=0), 'Scan did not reach real completed state')
    require(status.get('scan_strategy') == ('full' if full else 'fast'), 'Unexpected scan strategy')
    for key in ('errors', 'failed_files', 'skipped', 'updated', 'removed'):
        require(status.get(key) == 0, f'{key} is not zero: {status.get(key)}')
    for key in ('processed', 'discovered', 'succeeded', 'added'):
        require(status.get(key) == count, f'{key} does not equal fixture count: {status.get(key)} / {count}')
    require(evidence.get('count') == count and evidence.get('fixture_matches') is True,
            'Database rows do not match the exact fixture set')
    if full:
        require(status.get('mini_waveforms_total') == count and status.get('mini_waveforms_completed') == count,
                'Required mini waveform work is incomplete')
        require(evidence.get('cached_minis') == count and evidence.get('pending_minis') == 0,
                'Required mini waveform blobs are not persisted')


def database_evidence(database, root, expected, *, full):
    no_links(database)
    require(inside(database, root / 'profile') and database.parent.name.startswith('ScanSpeedQA-'),
            'Refusing to read an unowned database')
    require(database.is_file(), 'Owned database does not exist')
    with closing(sqlite3.connect(database.as_uri() + '?mode=ro', uri=True, timeout=2)) as db:
        db.execute('PRAGMA query_only=ON')
        rows = [row[0] for row in db.execute('SELECT file_path FROM audio_files')]
        result = dict(count=len(rows), fixture_matches={path_key(p) for p in rows} ==
                      {path_key(p) for p in expected} and len(rows) == len(expected))
        if full:
            cache = database.with_name('waveforms.db')
            no_links(cache)
            require(cache.is_file(), 'Persisted waveform database is missing')
            result['pending_minis'] = db.execute(
                'SELECT COUNT(*) FROM scan_foreground_completion WHERE mini_state=0').fetchone()[0]
            db.execute('ATTACH DATABASE ? AS cache', (cache.as_uri() + '?mode=ro',))
            result['cached_minis'] = db.execute(
                'SELECT COUNT(DISTINCT af.id) FROM audio_files af JOIN cache.waveforms w '
                'ON w.file_id=af.id WHERE length(w.waveform_data)>0').fetchone()[0]
        return result


class ScanTimeline:
    def __init__(self, started_at):
        self.started_at = started_at
        self.samples = []

    def add(self, status, observed_at):
        elapsed = observed_at - self.started_at
        previous = self.samples[-1]['elapsed_seconds'] if self.samples else 0
        require(math.isfinite(elapsed) and elapsed >= previous, 'Non-monotonic observation time')
        sample = dict(elapsed_seconds=elapsed, scan_status=dict(status))
        self.samples.append(sample)
        return sample

    def summary(self):
        require(bool(self.samples), 'No scan observations')
        durations = {}
        gaps = [self.samples[0]['elapsed_seconds']]
        for before, after in zip(self.samples, self.samples[1:]):
            status = before['scan_status']
            key = status['scan_phase'] + '/' + status['scan_workflow_stage']
            seconds = after['elapsed_seconds'] - before['elapsed_seconds']
            durations[key] = durations.get(key, 0) + seconds
            gaps.append(seconds)
        return dict(elapsed_seconds=self.samples[-1]['elapsed_seconds'],
                    first_observation_seconds=self.samples[0]['elapsed_seconds'],
                    max_poll_gap_seconds=max(gaps), observed_stage_seconds=durations,
                    sample_count=len(self.samples), stage_durations_are_polling_estimates=True)


def summarize_runs(runs, count, *, overall_status=None):
    passed = [r['elapsed_seconds'] for r in runs if r['status'] == 'passed']
    subsequent = [r['elapsed_seconds'] for r in runs[1:] if r['status'] == 'passed']
    median = statistics.median(passed) if passed else None
    return dict(status='passed' if runs and len(passed) == len(runs) and overall_status != 'failed' else 'failed',
        passed_repeats=len(passed), failed_repeats=len(runs) - len(passed),
        median_seconds=median, mean_seconds=statistics.mean(passed) if passed else None,
        min_seconds=min(passed) if passed else None, max_seconds=max(passed) if passed else None,
        median_files_per_second=count / median if median else None,
        first_run_seconds=runs[0].get('elapsed_seconds') if runs and runs[0]['status'] == 'passed' else None,
        subsequent_median_seconds=statistics.median(subsequent) if subsequent else None)


def verify_identity(identity, pid, exe, root):
    require(identity.get('pid') == pid, 'identity pid does not belong to our launch')
    for key, expected in (('exe_path', exe), ('isolation_root', root), ('runtime_root', root / 'profile/runtime')):
        require(bool(identity.get(key)) and path_key(identity[key]) == path_key(expected),
                f'identity {key} does not match the pinned package')


def conflicting_processes(rows, exe, root, *, qa_root=QA_ROOT):
    """Read image paths/bytes only; never command, attach to, or stop a process."""
    conflicts = []
    for row in rows:
        name = (row.get('Name') or '').casefold()
        likely_app = name == exe.name.casefold() or any(word in name for word in ('harken', 'aura', 'aris'))
        raw = row.get('ExecutablePath')
        if not raw:
            require(not likely_app, f'Cannot verify an existing app process: {row.get("ProcessId")}')
            continue
        image = Path(raw)
        if path_key(image) == path_key(exe):
            conflicts.append(row['ProcessId'])
        elif (likely_app or inside(image, qa_root)) and image.is_file() and contains_root_literal(image, root):
            conflicts.append(row['ProcessId'])
    return conflicts


def ensure_root_idle(exe, root):
    # Fixed read-only command, no paths or user text interpolated into shell code.
    result = subprocess.run(['powershell.exe', '-NoProfile', '-NonInteractive', '-Command',
        '[Console]::OutputEncoding = [System.Text.UTF8Encoding]::new(); '
        '$ErrorActionPreference = "Stop"; '
        '@(Get-CimInstance Win32_Process | Select-Object ProcessId,Name,ExecutablePath) | ConvertTo-Json -Compress'],
        capture_output=True, text=True, encoding='utf-8-sig', timeout=20, check=True,
        creationflags=subprocess.CREATE_NO_WINDOW)
    rows = json.loads(result.stdout)
    require(isinstance(rows, list), 'Unable to enumerate existing processes safely')
    conflicts = conflicting_processes(rows, exe, root)
    require(not conflicts, f'An existing process uses this package/root; leave it running: {conflicts}')


class EvidenceDriver(Driver):
    def __init__(self, runtime, output, lock):
        super().__init__(runtime, output)
        self.lock = lock

    def command(self, payload, timeout=25):
        self.lock.check_owner()
        started = time.monotonic()
        with (self.output / 'requests.jsonl').open('a', encoding='utf-8') as log:
            log.write(json.dumps(dict(request=payload, started_at=utc_now(),
                monotonic_seconds=started, timeout_seconds=timeout), ensure_ascii=False) + '\n')
        try:
            return super().command(payload, timeout=timeout)
        except BaseException as error:
            with (self.output / 'command_failures.jsonl').open('a', encoding='utf-8') as log:
                log.write(json.dumps(dict(request=payload, elapsed_seconds=time.monotonic() - started,
                    error=str(error), error_type=type(error).__name__), ensure_ascii=False) + '\n')
            raise


def measure_scan(driver, fixture, full, timeout, output):
    previous = driver.command({'cmd': 'scan_status'}, timeout=min(25, timeout))
    require(not previous['scanning'] and not previous['scan_completion_pending'], 'Previous scan is still active')
    previous_start = previous['scan_started_at_ms']
    save_json(output / 'scan-start.json', dict(previous=previous, fixture=str(fixture), full=full,
        started_at=utc_now(), timeout_seconds=timeout))
    with (output / 'scan_samples.jsonl').open('x', encoding='utf-8') as journal:
        started = time.monotonic()
        timeline = ScanTimeline(started)
        deadline = started + timeout

        def budget():
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError('Scan deadline expired; all observations are retained in scan_samples.jsonl')
            return min(25, remaining)

        driver.command({'cmd': 'qa_rightclick_scan', 'path': str(fixture),
                        'cache_waveforms': full}, timeout=budget())
        while True:
            status = driver.command({'cmd': 'scan_status'}, timeout=budget())
            sample = timeline.add(status, time.monotonic())
            journal.write(json.dumps(sample, ensure_ascii=False) + '\n')
            journal.flush()
            budget()  # A late reply does not bypass the deadline.
            fresh = status.get('scan_started_at_ms', 0) > 0 and status['scan_started_at_ms'] != previous_start
            if fresh and (status.get('errors', 0) or status.get('scan_workflow_stage') in ('failed', 'paused', 'cancelled')):
                raise RuntimeError('Scan failed or was interrupted: ' + json.dumps(status, ensure_ascii=False))
            if scan_terminal(status, previous_start):
                return dict(timeline.summary(), terminal=status)
            time.sleep(min(POLL_SECONDS, budget()))


def wait_ready(driver, predicate, label, timeout=60):
    deadline = time.monotonic() + timeout
    last = {}
    while time.monotonic() < deadline:
        last = driver.command({'cmd': 'get_state'}, timeout=min(10, deadline - time.monotonic()))
        if predicate(last):
            return last
        time.sleep(POLL_SECONDS)
    raise TimeoutError(f'{label}: {last}')


def new_library(driver, root, name):
    state = wait_ready(driver, lambda s: s.get('main_ui_ready') and not s.get('db_switch_in_progress'),
                       'Main UI not ready')
    if state.get('db_path'):
        no_links(Path(state['db_path']))
        require(inside(Path(state['db_path']), root / 'profile'), 'Startup selected a nonisolated database')
    if not state.get('new_db_dialog_open'):
        driver.command({'cmd': 'info_edit_input', 'action': 'new_db_dialog'})
    wait_ready(driver, lambda s: s.get('new_db_dialog_open'), 'New library dialog did not open', 15)
    driver.command({'cmd': 'info_edit_input', 'action': 'new_db_name', 'text': name})
    driver.command({'cmd': 'info_edit_input', 'action': 'new_db_create'})
    state = wait_ready(driver, lambda s: s.get('db_path') and not s.get('db_switch_in_progress') and
                       not s.get('new_db_dialog_open'), 'New isolated database not ready')
    database = Path(state['db_path'])
    require(database.is_absolute() and database.parent.name == name, 'New database identity mismatch')
    empty = database_evidence(database, root, [], full=False)
    require(empty['count'] == 0, 'New database is not empty')
    return database.resolve()


def finish_result(driver, root, output):
    # Use the real Done control: merely clearing counters leaves a native modal
    # open in some builds and can obstruct the next round's new-library dialog.
    ui = Smoke(driver, root, output, 30)
    # qa_rightclick_scan can finish in the background. A logical result surface
    # alone does not mean its window/Done control has been opened and rendered.
    driver.command({'cmd': 'open_scan_dialog'})
    final_ui = ui.wait(lambda s: s['surface'] == 'result' and
        ui.target(s, 'scan.result.done', '##ScanResult') is not None, 'Native result did not appear')
    ui.click('scan.result.done', '##ScanResult')
    ui.wait(lambda s: s['surface'] == 'setup' and not ui.has(s, 'scan.result.done')
            and not s['is_scanning'], 'Native result did not close')
    return final_ui


def preserve_diagnostics(root, output):
    source = root / 'profile/runtime/aura_diagnostics_live.jsonl'
    destination = output / source.name
    no_links(source)
    if not source.is_file():
        return dict(present=False, source=str(source))
    # Called after our process stops and BEFORE releasing the shared root lock.
    # The next launch may truncate the live file. Never edit diagnosticMode.
    with source.open('rb') as original, destination.open('xb') as retained:
        shutil.copyfileobj(original, retained, length=1024 * 1024)
    return dict(present=True, source=str(source), copy=str(destination),
                bytes=destination.stat().st_size, sha256=sha256(destination))


def stop_owned_process(process, request_exit, report):
    """Only use the CreateProcess handle; closing it is not proof of exit."""
    import win32con
    import win32event
    import win32process
    exited = False
    try:
        waited = win32event.WaitForSingleObject(process, 0)
        if waited == win32con.WAIT_TIMEOUT:
            if request_exit is not None:
                try:
                    request_exit()
                except Exception as error:
                    report.setdefault('cleanup_errors', []).append('Exit request: ' + str(error))
            waited = win32event.WaitForSingleObject(process, 15000)
            if waited == win32con.WAIT_TIMEOUT:
                win32process.TerminateProcess(process, 1)
                report['forced_private_exit'] = True
                report.setdefault('cleanup_errors', []).append('Owned process required forced exit')
                waited = win32event.WaitForSingleObject(process, 5000)
        require(waited == win32con.WAIT_OBJECT_0, 'Owned process exit unconfirmed; retain shared lock')
        exited = True
        report['process_exit_code'] = win32process.GetExitCodeProcess(process)
        require(report['process_exit_code'] == 0, 'Owned process exited unsuccessfully')
    except Exception as error:
        report.setdefault('cleanup_errors', []).append(str(error))
    finally:
        try:
            process.Close()
        except Exception as error:
            report.setdefault('cleanup_errors', []).append('Close handle: ' + str(error))
    return exited


def check_runtime_dependencies():
    require(sys.platform == 'win32', 'Real EXE measurements require Windows')
    import win32con, win32event, win32process  # noqa: F401
    from PIL import Image  # noqa: F401


def run(args):
    exe, root, output = validate_paths(args.exe, args.isolation_root, args.output)
    require(contains_root_literal(exe, root), 'EXE lacks the exact compiled isolation root; refusing launch')
    check_runtime_dependencies()
    output.mkdir(parents=True, exist_ok=False)
    runtime = root / 'profile/runtime'
    lock = SharedLock(runtime, output)
    report = dict(status='running', started_at=utc_now(), exe=str(exe), exe_sha256=sha256(exe),
        script_sha256=sha256(Path(__file__)), isolation_root=str(root), output=str(output),
        count=args.count, repeats_requested=args.repeats, full=args.full, timeout_seconds=args.timeout,
        poll_seconds=POLL_SECONDS, cpu_count=os.cpu_count(), worker_policy='native automatic (fresh process default)',
        timing_boundary='before native qa_rightclick_scan to mode-specific backend terminal; DB readback excluded',
        completion_contract=('full: all foreground pending flags/counters drained, required minis persisted'
            if args.full else 'fast: new completed task, scanning=false, foreground gate clear, metadata resolved; '
            'optional cover/mini background debt excluded'),
        human_visual_acceptance='not performed',
        limitations=['Polling can miss short stages; reported durations are estimates.',
            'OS caches are not flushed; first and subsequent rounds are separate.',
            'Synthetic PCM WAV first imports only; no real library/rescan/source-directory benchmark.',
            'UI snapshots are outside timed interval; no UI acceptance or visibility claim.'], runs=[])
    process = desktop = driver = current = None
    process_exited = True
    verified = False
    try:
        save_json(output / 'report.json', report)
        runtime.mkdir(parents=True, exist_ok=True)
        lock.acquire()
        require(not (runtime / 'qa_cmd.json').exists(), 'A QA command is pending; retaining it')
        ensure_root_idle(exe, root)
        fixture = root / ('scan-speed-fixtures-' + uuid.uuid4().hex)
        report['fixture'] = create_fixture(root, fixture, args.count)
        save_json(output / 'fixture.json', report['fixture'])
        expected = [fixture / entry['name'] for entry in report['fixture']['files']]
        driver = EvidenceDriver(runtime, output, lock)
        desktop = PrivateDesktop()
        process, pid = desktop.start(exe)
        process_exited = False
        identity = driver.command({'cmd': 'magic_eq_identity'}, timeout=60)  # first bridge command
        report['identity'] = identity
        verify_identity(identity, pid, exe, root)
        verified = True
        for index in range(args.repeats):
            current = dict(repeat=index + 1, status='running', started_at=utc_now())
            report['runs'].append(current)
            round_output = output / f'run-{index + 1:02d}'
            round_output.mkdir()
            current['evidence_directory'] = str(round_output)
            verify_identity(driver.command({'cmd': 'magic_eq_identity'}), pid, exe, root)
            database = new_library(driver, root, 'ScanSpeedQA-' + uuid.uuid4().hex)
            current['database'] = str(database)
            save_json(output / 'report.json', report)
            current.update(measure_scan(driver, fixture, args.full, args.timeout, round_output))
            current['database_evidence'] = database_evidence(database, root, expected, full=args.full)
            save_json(round_output / 'database.json', current['database_evidence'])
            validate_result(current['terminal'], current['database_evidence'], args.count, args.full)
            # UI fields may differ between builds. Retain them without affecting
            # measured timing or requiring newly introduced presentation fields.
            current['ui_final'] = finish_result(driver, root, round_output)
            save_json(round_output / 'ui-final.json', current['ui_final'])
            current.update(status='passed', finished_at=utc_now())
            save_json(round_output / 'result.json', current)
            save_json(output / 'report.json', report)
            print(f'Run {index + 1}/{args.repeats}: {current["elapsed_seconds"]:.3f}s, {args.count} rows', flush=True)
        report['status'] = 'passed'
    except BaseException as error:
        report.update(status='failed', error=str(error), traceback=traceback.format_exc())
        if current is not None and current['status'] == 'running':
            current.update(status='failed', error=str(error), finished_at=utc_now())
        if verified:
            try:
                # Identity and lock ownership must still match before diagnostic commands.
                verify_identity(driver.command({'cmd': 'magic_eq_identity'}, timeout=3), pid, exe, root)
                report['failure_scan_status'] = driver.command({'cmd': 'scan_status'}, timeout=3)
                report['failure_ui_state'] = driver.command({'cmd': 'scan_dialog_ui', 'action': 'state'}, timeout=3)
                capture(driver, output / 'failure-diagnostic.png')
            except Exception as diagnostic_error:
                report['diagnostic_error'] = str(diagnostic_error)
        if not isinstance(error, Exception):
            raise
    finally:
        def request_exit():
            verify_identity(driver.command({'cmd': 'magic_eq_identity'}, timeout=3), pid, exe, root)
            driver.command({'cmd': 'info_edit_input', 'action': 'exit'}, timeout=8)

        if process is not None:
            process_exited = stop_owned_process(process, request_exit if verified else None, report)
            if verified:
                try:
                    report['diagnostic_log'] = preserve_diagnostics(root, output)
                    report['diagnostic_log']['process_exit_confirmed'] = process_exited
                except Exception as error:
                    report.setdefault('cleanup_errors', []).append('Retain diagnostics: ' + str(error))
        if desktop is not None:
            try:
                desktop.close()
            except Exception as error:
                report.setdefault('cleanup_errors', []).append('Desktop close: ' + str(error))
        if lock.identity is not None:
            try:
                if not lock.release(process_exited=process_exited):
                    report['lock_retained'] = str(lock.path)
            except Exception as error:
                report.setdefault('cleanup_errors', []).append('Lock release: ' + str(error))
        try:
            require(sha256(exe) == report['exe_sha256'], 'EXE bytes changed during measurement')
        except Exception as error:
            report.setdefault('cleanup_errors', []).append(str(error))
        if report.get('cleanup_errors') or not process_exited:
            report['status'] = 'failed'
        report.update(finished_at=utc_now(), summary=summarize_runs(report['runs'], args.count,
                                                                 overall_status=report['status']))
        save_json(output / 'report.json', report)
    print(json.dumps(dict(status=report['status'], report=str(output / 'report.json')), ensure_ascii=False), flush=True)
    return 0 if report['status'] == 'passed' else 1


def main(argv=None):
    args = parse_args(argv)
    try:
        return run(args)
    except Exception as error:
        print(f'Scan benchmark preflight/cleanup failed: {error}', file=sys.stderr)
        return 2


if __name__ == '__main__':
    raise SystemExit(main())
