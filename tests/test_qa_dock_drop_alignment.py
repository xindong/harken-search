import importlib
from pathlib import Path
from types import SimpleNamespace

import pytest


@pytest.fixture
def dock_qa(monkeypatch):
    monkeypatch.syspath_prepend(str(Path(__file__).resolve().parents[1] / "tools"))
    return importlib.import_module("qa_dock_drop_alignment")


def test_dock_qa_rejects_shared_run_before_editing_settings(dock_qa, tmp_path, monkeypatch):
    pytest.importorskip("win32con")
    exe = tmp_path / "HarkenSearch.exe"
    exe.write_bytes(b"MZdock_pointer\0capture_preview_frame\0magic_eq_identity\0")
    database = tmp_path / "profile/Local/HarkenSearch/Database/Dock QA/main.db"
    database.parent.mkdir(parents=True)
    database.touch()
    lock = tmp_path / "dock-qa.lock"
    lock.write_text("another run", encoding="utf-8")
    monkeypatch.setattr(dock_qa, "exe_has_root", lambda *_: True)
    monkeypatch.setattr(dock_qa, "set_scale", lambda *_: pytest.fail("settings touched while locked"))
    args = SimpleNamespace(exe=exe, isolation_root=tmp_path, output=tmp_path / "result",
                           theme=0, scale=125)
    with pytest.raises(FileExistsError):
        dock_qa.run(args)
    assert lock.read_text(encoding="utf-8") == "another run"


@pytest.mark.parametrize("app_exit,shutdown_error", [(0, None), (1, None), (0, "shutdown timed out")])
@pytest.mark.parametrize("gestures_only", [True, False])
def test_result_requires_clean_app_shutdown(dock_qa, app_exit, shutdown_error, gestures_only):
    report = {"app_exit": app_exit, "shutdown_error": shutdown_error,
              "gestures": [{"passed": True}], "cases": [{"passed": True}]}
    assert dock_qa.run_exit_code(report, gestures_only) == (1 if app_exit or shutdown_error else 0)
