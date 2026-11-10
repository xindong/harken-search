"""Run the real QA cleanup blocks with controlled Win32 exit receipts.

No app is launched and no desktop input or production data is touched.
"""
import ast
from datetime import datetime, timezone
from pathlib import Path
from types import SimpleNamespace
import traceback

import pytest

ROOT = Path(__file__).resolve().parents[1]


def cleanup_code(name):
    source = (ROOT / "tools" / name).read_text(encoding="utf-8")
    blocks = [node.finalbody for node in ast.walk(ast.parse(source))
              if isinstance(node, ast.Try) and node.finalbody
              and "if process is not None" in ast.unparse(ast.Module(body=node.finalbody, type_ignores=[]))]
    body = max(blocks, key=lambda block: len(ast.unparse(ast.Module(body=block, type_ignores=[]))))
    return compile(ast.fix_missing_locations(ast.Module(body=body, type_ignores=[])), name, "exec")


def run_cleanup(name, tmp_path, *, exit_code=0, timeout=False, verified=True):
    calls = []
    settings = tmp_path / "settings.ini"
    settings.write_text("volume=0.0\n", encoding="utf-8")
    report = {"passed": True, "identity": {"pid": 999, "hwnd": 900}}
    def command(*args, **kwargs): calls.append(("command", args, kwargs))
    def post(hwnd, *args): calls.append(("close-window", hwnd))
    desktop = SimpleNamespace(close=lambda: calls.append(("desktop-closed",)))
    process = SimpleNamespace(Close=lambda: calls.append(("handle-closed",)))
    def wait(handle, milliseconds): return 258 if timeout or milliseconds == 0 else 0
    def emit(phase, data=None): report["phase"] = phase
    env = dict(process=process, pid=123, desktop=desktop, report=report, settings=settings, out=tmp_path,
               old_volume="volume=0.6", verified=verified, identity_verified=verified,
               save=lambda: None, emit=emit, command=command, cmd=command,
               driver=SimpleNamespace(command=command),
               release_native_capture=lambda: None, native_log=lambda: "",
               datetime=datetime, timezone=timezone, traceback=traceback,
               win32con=SimpleNamespace(WAIT_OBJECT_0=0, WAIT_TIMEOUT=258, WM_CLOSE=16),
               win32event=SimpleNamespace(WaitForSingleObject=wait),
               win32process=SimpleNamespace(GetExitCodeProcess=lambda handle: exit_code,
                   GetWindowThreadProcessId=lambda hwnd: (1, 123 if hwnd == 100 else 999)),
               win32gui=SimpleNamespace(PostMessage=post, EnumWindows=lambda callback, arg:
                   [callback(hwnd, arg) for hwnd in (100, 900)]))
    error = None
    try:
        exec(cleanup_code(name), env)
    except BaseException as caught:
        error = caught
    return report, error, calls, settings.read_text(encoding="utf-8")


@pytest.mark.parametrize("name", ["qa_similar_deep.py", "qa_similarity_drag.py", "qa_similarity_resume.py"])
def test_shutdown_crash_fails_report_and_command_after_cleanup(name, tmp_path):
    report, error, calls, _ = run_cleanup(name, tmp_path, exit_code=123)
    assert report["passed"] is False
    assert error is not None
    assert ("desktop-closed",) in calls


@pytest.mark.parametrize("name", ["qa_similar_deep.py", "qa_similarity_drag.py", "qa_similarity_resume.py"])
def test_shutdown_timeout_fails_and_restores_desktop(name, tmp_path):
    report, error, calls, _ = run_cleanup(name, tmp_path, timeout=True)
    assert report["passed"] is False
    assert error is not None
    assert ("desktop-closed",) in calls


def test_unverified_identity_never_closes_another_instance(tmp_path):
    _, _, calls, _ = run_cleanup("qa_similar_deep.py", tmp_path, verified=False)
    assert not any(call[0] == "command" for call in calls)
    assert ("close-window", 900) not in calls
    assert ("close-window", 100) in calls


def test_drag_restores_volume_even_if_application_crashed(tmp_path):
    _, _, _, settings = run_cleanup("qa_similarity_drag.py", tmp_path, exit_code=123)
    assert "volume=0.6" in settings
