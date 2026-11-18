"""Regression tests for the browser filter QA helper's evidence and ownership."""
from pathlib import Path
from types import SimpleNamespace
import sys

from PIL import Image, ImageDraw
import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import qa_browser_filter_white as qa
from qa_scan_speed import SharedLock


def test_samples_inside_observed_search_bounds_when_layout_moves():
    image = Image.new("RGB", (300, 200), (1, 2, 3))
    bounds = [120, 70, 240, 100]
    ImageDraw.Draw(image).rectangle((120, 70, 239, 99), fill=(230, 240, 250))
    color, point = qa.sample_search_field(image, bounds)
    assert color == (230, 240, 250)
    assert bounds[0] < point[0] < bounds[2] and bounds[1] < point[1] < bounds[3]


@pytest.mark.parametrize("bounds", [None, [], [0, 0, 0, 10], [-1, 0, 20, 10], [0, 0, 301, 10]])
def test_rejects_missing_or_invalid_search_bounds(bounds):
    with pytest.raises(ValueError):
        qa.sample_search_field(Image.new("RGB", (300, 200)), bounds)


def test_existing_lock_prevents_settings_mutation(tmp_path, monkeypatch):
    root = tmp_path / "card" / "profile"
    runtime = root / "profile/runtime"
    runtime.mkdir(parents=True)
    exe = tmp_path / "card" / "build" / "HarkenSearch.exe"
    exe.parent.mkdir(parents=True)
    exe.write_bytes(b"qa")
    settings = root / "profile/Local/HarkenSearch/settings.ini"
    settings.parent.mkdir(parents=True)
    settings.write_text("currentDbPath=untouched", encoding="utf-8")
    monkeypatch.setattr(qa, "CARD_ROOT", tmp_path / "card")
    monkeypatch.setattr(qa, "contains_root_literal", lambda *_: True)
    lock = SharedLock(runtime, tmp_path / "owner")
    lock.acquire()
    try:
        with pytest.raises(FileExistsError):
            qa.run(SimpleNamespace(exe=exe, isolation_root=root, output=tmp_path / "card" / "output"))
        assert settings.read_text(encoding="utf-8") == "currentDbPath=untouched"
    finally:
        lock.release(process_exited=True)


def test_existing_app_prevents_settings_mutation(tmp_path, monkeypatch):
    card = tmp_path / "card"
    root = card / "profile"
    settings = root / "profile/Local/HarkenSearch/settings.ini"
    settings.parent.mkdir(parents=True)
    settings.write_text("currentDbPath=untouched", encoding="utf-8")
    exe = card / "build/HarkenSearch.exe"
    exe.parent.mkdir(parents=True)
    exe.write_bytes(b"qa")
    monkeypatch.setattr(qa, "CARD_ROOT", card)
    monkeypatch.setattr(qa, "contains_root_literal", lambda *_: True)
    def reject_running_app(*_):
        raise RuntimeError("isolation root already in use")
    monkeypatch.setattr(qa, "ensure_root_idle", reject_running_app)
    with pytest.raises(RuntimeError, match="already in use"):
        qa.run(SimpleNamespace(exe=exe, isolation_root=root, output=card / "output"))
    assert settings.read_text(encoding="utf-8") == "currentDbPath=untouched"
    assert not (root / "profile/runtime/qa_scan_dialog_simple.lock").exists()
