from pathlib import Path
import sys

import pytest

import tools.qa_sidebar_metadata_picker_floating as floating
from tools.qa_sidebar_metadata_picker_floating import run


@pytest.mark.skipif(sys.platform != "win32", reason="requires private Windows desktop")
def test_floating_picker_restores_profile_when_output_setup_fails(tmp_path: Path):
    settings = tmp_path / "profile/Local/HarkenSearch/settings.ini"
    settings.parent.mkdir(parents=True)
    original = b"panelSidebar_floating=0\nactiveSidebarSection=1\n"
    settings.write_bytes(original)
    occupied_output = tmp_path / "occupied-output"
    occupied_output.write_text("not a directory", encoding="utf-8")

    with pytest.raises(FileExistsError):
        run(tmp_path / "unused.exe", tmp_path, occupied_output, "overlap")

    assert settings.read_bytes() == original


@pytest.mark.skipif(sys.platform != "win32", reason="requires private Windows desktop")
def test_floating_picker_restores_profile_when_private_desktop_fails(tmp_path: Path, monkeypatch):
    settings = tmp_path / "profile/Local/HarkenSearch/settings.ini"
    settings.parent.mkdir(parents=True)
    original = b"panelSidebar_floating=0\n"
    settings.write_bytes(original)

    def fail_desktop():
        raise RuntimeError("private desktop unavailable")

    monkeypatch.setattr(floating, "PrivateDesktop", fail_desktop)
    with pytest.raises(RuntimeError, match="private desktop unavailable"):
        floating.run(tmp_path / "unused.exe", tmp_path, tmp_path / "output", "zorder")

    assert settings.read_bytes() == original
@pytest.mark.skipif(sys.platform != "win32", reason="requires private Windows desktop")
@pytest.mark.parametrize("scenario", ["overlap", "zorder"])
def test_floating_picker_removes_new_layout_file_after_failure(tmp_path: Path, monkeypatch, scenario: str):
    settings = tmp_path / "profile/Local/HarkenSearch/settings.ini"
    settings.parent.mkdir(parents=True)
    original = b"panelSidebar_floating=0" + bytes([10])
    settings.write_bytes(original)
    layout = settings.with_name("imgui.ini")

    def fail_after_layout_creation():
        layout.write_bytes(b"[Window][QA]" + bytes([10]))
        raise RuntimeError("desktop unavailable after layout creation")

    monkeypatch.setattr(floating, "PrivateDesktop", fail_after_layout_creation)
    with pytest.raises(RuntimeError, match="desktop unavailable after layout creation"):
        floating.run(tmp_path / "unused.exe", tmp_path, tmp_path / "output", scenario)

    assert settings.read_bytes() == original
    assert not layout.exists()
