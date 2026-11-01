"""Real filesystem lock tests; mock only the private-app/Win32 boundary.

No Harken process, desktop, mouse input, or existing QA profile is used.
"""
import json
from pathlib import Path
import subprocess
import sys
from types import SimpleNamespace

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import qa_scan_dialog_outcomes as qa


LOCK_NAME = 'qa_scan_dialog_simple.lock'


def contender(lock):
    """A separate interpreter performs the same atomic create as smoke QA."""
    code = '''
from pathlib import Path
import sys
try:
    with Path(sys.argv[1]).open('x', encoding='utf-8') as handle:
        handle.write('contender acquired')
except FileExistsError:
    sys.exit(23)
'''
    return subprocess.run(
        [sys.executable, '-B', '-c', code, str(lock)], capture_output=True,
        text=True, timeout=10,
        creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0),
    )


@pytest.fixture
def case_env(tmp_path, monkeypatch):
    exe = tmp_path / 'qa.exe'
    exe.write_bytes(b'not an executable; launch is mocked')
    root = tmp_path / 'isolated'
    runtime = root / 'profile/runtime'
    lock = runtime / LOCK_NAME
    state = SimpleNamespace(
        lock=lock, args=SimpleNamespace(exe=exe, isolation_root=root, output=tmp_path / 'evidence'),
        started=0, desktop_closed=0, process_closed=0, terminated=0,
        commands=[], waits=[], wait_results=[0], exit_code=0, exit_reads=0,
        driver_hook=lambda: None, desktop_hook=lambda: None, start_hook=lambda: None,
        identity_hook=lambda: None, smoke_hook=lambda: None,
        close_error=False, desktop_error=False, terminate_error=False, exit_error=False,
    )

    class Process:
        def Close(self):
            state.process_closed += 1
            if state.close_error:
                raise RuntimeError('process handle close failed')

    process = Process()

    class Desktop:
        def __init__(self):
            state.desktop_hook()

        def start(self, path):
            assert path == exe
            state.started += 1
            state.start_hook()
            return process, 123

        def close(self):
            state.desktop_closed += 1
            if state.desktop_error:
                raise RuntimeError('desktop close failed')

    class Driver:
        def __init__(self, actual_runtime, output):
            assert actual_runtime == runtime and output == state.args.output
            state.driver_hook()

        def command(self, payload, **kwargs):
            state.commands.append(payload)
            if payload['cmd'] == 'magic_eq_identity':
                state.identity_hook()
                return {'pid': 123, 'exe_path': str(exe), 'isolation_root': str(root)}
            assert payload == {'cmd': 'info_edit_input', 'action': 'exit'}
            if state.exit_error:
                raise RuntimeError('exit command failed')

    class Smoke:
        last = {}

        def __init__(self, *args):
            state.smoke_hook()

        def wait(self, *args, **kwargs):
            # Stop at the first post-identity readiness wait, before UI/data work.
            raise RuntimeError('test workflow stopped before UI')

    def wait(handle, timeout):
        assert handle is process
        state.waits.append(timeout)
        result = state.wait_results.pop(0)
        if isinstance(result, BaseException):
            raise result
        return result

    def terminate(handle, code):
        assert handle is process and code == 1
        state.terminated += 1
        if state.terminate_error:
            raise RuntimeError('terminate failed')

    def exit_code(handle):
        assert handle is process
        state.exit_reads += 1
        return state.exit_code

    monkeypatch.setitem(sys.modules, 'win32con', SimpleNamespace(WAIT_OBJECT_0=0, WAIT_TIMEOUT=258))
    monkeypatch.setitem(sys.modules, 'win32event', SimpleNamespace(WaitForSingleObject=wait))
    monkeypatch.setitem(sys.modules, 'win32process', SimpleNamespace(
        GetExitCodeProcess=exit_code, TerminateProcess=terminate))
    monkeypatch.setitem(sys.modules, 'win32gui', SimpleNamespace())
    monkeypatch.setattr(qa, 'PrivateDesktop', Desktop)
    monkeypatch.setattr(qa, 'Driver', Driver)
    monkeypatch.setattr(qa, 'Smoke', Smoke)
    monkeypatch.setattr(qa, 'capture', lambda *args: None)
    real_inside = qa.inside
    # Redirect only the fixed QA-root guard into this test's unique temp tree.
    monkeypatch.setattr(qa, 'inside', lambda path, parent: real_inside(
        path, tmp_path if parent == Path('D:/Harken-QA') else parent))
    return state


def fail(message):
    raise RuntimeError(message)


def report(case_env):
    return json.loads((case_env.args.output / 'report.json').read_text(encoding='utf-8'))


def test_holds_shared_atomic_lock_before_driver_and_launch(case_env):
    observations = []

    def observe():
        attempt = contender(case_env.lock)
        observations.append((attempt.returncode, attempt.stderr))

    case_env.driver_hook = case_env.start_hook = observe
    assert qa.run(case_env.args) == 1
    assert observations == [(23, ''), (23, '')]
    assert not case_env.lock.exists()
    assert case_env.process_closed == case_env.desktop_closed == 1


def test_existing_lock_is_never_overwritten_or_removed(case_env):
    case_env.lock.parent.mkdir(parents=True)
    case_env.lock.write_text('another helper owns this lock', encoding='utf-8')
    with pytest.raises((RuntimeError, FileExistsError)):
        qa.run(case_env.args)
    assert case_env.started == 0 and case_env.commands == []
    assert case_env.lock.read_text(encoding='utf-8') == 'another helper owns this lock'


def test_atomic_create_rejects_lock_arriving_after_preflight(case_env, monkeypatch):
    original_mkdir = Path.mkdir
    inserted = False

    def racing_mkdir(path, *args, **kwargs):
        nonlocal inserted
        result = original_mkdir(path, *args, **kwargs)
        if path == case_env.lock.parent and not inserted:
            inserted = True
            with case_env.lock.open('x', encoding='utf-8') as owner:
                owner.write('winner between preflight and acquire')
        return result

    monkeypatch.setattr(Path, 'mkdir', racing_mkdir)
    with pytest.raises(FileExistsError):
        qa.run(case_env.args)
    assert case_env.started == 0 and case_env.commands == []
    assert case_env.lock.read_text(encoding='utf-8') == 'winner between preflight and acquire'


@pytest.mark.parametrize('boundary', ['driver_hook', 'desktop_hook', 'start_hook', 'smoke_hook', 'identity_hook'])
def test_startup_failure_releases_only_owned_lock(case_env, boundary):
    observed = []

    def abort():
        observed.append(case_env.lock.is_file())
        fail('injected ' + boundary)

    setattr(case_env, boundary, abort)
    assert qa.run(case_env.args) == 1
    assert observed == [True]
    assert not case_env.lock.exists()
    assert report(case_env)['error'] == 'injected ' + boundary
    assert case_env.process_closed == (1 if boundary in ('smoke_hook', 'identity_hook') else 0)
    assert case_env.desktop_closed == (0 if boundary in ('driver_hook', 'desktop_hook') else 1)


@pytest.mark.parametrize('waits,exit_code,terminations', [
    ([0], 0, 0),
    ([0], 7, 0),
    ([258, 0], 0, 0),
    ([258, 258, 0], 1, 1),
])
def test_confirmed_process_exit_releases_owned_lock(case_env, waits, exit_code, terminations):
    held = []
    case_env.start_hook = lambda: held.append(case_env.lock.is_file())
    case_env.wait_results, case_env.exit_code = list(waits), exit_code
    assert qa.run(case_env.args) == 1  # Workflow intentionally stops before UI work.
    assert held == [True]
    assert not case_env.lock.exists()
    assert report(case_env)['exit_code'] == exit_code
    assert case_env.terminated == terminations
    assert case_env.process_closed == case_env.desktop_closed == 1


@pytest.mark.parametrize('waits,terminate_error', [
    ([258, 258, 258], False),
    ([258, 258], True),
    ([0xFFFFFFFF], False),
    ([RuntimeError('wait failed')], False),
])
def test_unconfirmed_exit_retains_lock_and_blocks_next_helper(case_env, waits, terminate_error):
    case_env.wait_results, case_env.terminate_error = list(waits), terminate_error
    assert qa.run(case_env.args) == 1
    assert case_env.lock.is_file()
    assert contender(case_env.lock).returncode == 23
    assert case_env.exit_reads == 0  # No exit-code guess substitutes for a signaled handle.
    assert report(case_env)['status'] == 'failed'
    assert case_env.process_closed == case_env.desktop_closed == 1


def test_failed_exit_command_still_waits_and_terminates_owned_process(case_env):
    case_env.exit_error = True
    case_env.wait_results, case_env.exit_code = [258, 258, 0], 1
    assert qa.run(case_env.args) == 1
    assert case_env.terminated == 1 and not case_env.lock.exists()
    assert report(case_env)['exit_command_error'] == 'exit command failed'


@pytest.mark.parametrize('boundary', ['close_error', 'desktop_error'])
def test_cleanup_error_after_confirmed_exit_does_not_leak_lock(case_env, boundary):
    setattr(case_env, boundary, True)
    assert qa.run(case_env.args) == 1
    assert not case_env.lock.exists()
    assert case_env.process_closed == case_env.desktop_closed == 1
    assert report(case_env)['status'] == 'failed'


@pytest.mark.parametrize('replacement', ['new-file', 'changed-owner'])
def test_replaced_lock_is_not_deleted(case_env, replacement):
    def replace_owner():
        if replacement == 'new-file':
            case_env.lock.rename(case_env.lock.with_suffix('.previous-owner'))
        case_env.lock.write_text('replacement owner', encoding='utf-8')

    case_env.identity_hook = replace_owner
    assert qa.run(case_env.args) == 1
    assert case_env.lock.read_text(encoding='utf-8') == 'replacement owner'
    assert contender(case_env.lock).returncode == 23


@pytest.mark.parametrize('interruption', [KeyboardInterrupt, SystemExit])
@pytest.mark.parametrize('waits,retained', [([0], False), ([258, 258, 258], True)])
def test_interrupted_helper_cleans_up_and_reports_failure(case_env, interruption, waits, retained):
    def interrupt():
        raise interruption('interrupted test launch')

    case_env.identity_hook = interrupt
    case_env.wait_results = list(waits)
    with pytest.raises(interruption, match='interrupted test launch'):
        qa.run(case_env.args)
    assert case_env.lock.exists() is retained
    assert report(case_env)['status'] == 'failed'
    assert report(case_env)['error'] == 'interrupted test launch'
    assert case_env.process_closed == case_env.desktop_closed == 1


@pytest.mark.parametrize('waits,retained', [([0], False), ([258, 258, 258], True)])
def test_report_write_failure_does_not_change_exit_based_lock_decision(case_env, monkeypatch, waits, retained):
    case_env.wait_results = list(waits)
    monkeypatch.setattr(qa, 'save_json', lambda *args: fail('report write failed'))
    with pytest.raises(RuntimeError, match='report write failed'):
        qa.run(case_env.args)
    assert case_env.lock.exists() is retained
    assert case_env.process_closed == case_env.desktop_closed == 1


def test_real_smoke_runner_cannot_launch_while_outcomes_owns_lock(case_env, monkeypatch):
    import qa_scan_dialog_simple as smoke

    monkeypatch.setattr(smoke, 'sys', SimpleNamespace(platform='win32'))
    monkeypatch.setattr(smoke, 'inside', qa.inside)
    monkeypatch.setitem(sys.modules, 'PIL', SimpleNamespace(Image=object()))
    monkeypatch.setattr(smoke, 'PrivateDesktop', lambda: fail('smoke must not launch'))
    blocked = []

    def try_smoke():
        smoke_args = SimpleNamespace(
            exe=case_env.args.exe, isolation_root=case_env.args.isolation_root,
            output=case_env.args.output.parent / 'smoke-evidence', timeout=15,
        )
        with pytest.raises(FileExistsError):
            smoke.run(smoke_args)
        blocked.append(True)

    case_env.driver_hook = try_smoke
    assert qa.run(case_env.args) == 1
    assert blocked == [True]
    assert not case_env.lock.exists()
