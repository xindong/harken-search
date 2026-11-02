import json
from pathlib import Path
import sys
import zipfile

import pytest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from tools import pack_engine
from tools.release_update import build_deterministic_zip


def history(version="1.2.3", **changes):
    entry = {
        "version": version,
        "release_date": "2026-10-19",
        "headline": "修复自动更新",
        "contributors": ["Harken Search Team"],
        "items": [{"category": "fix", "text": "更新成功后正确显示状态"}],
    }
    entry.update(changes)
    return {"schema_version": 2, "releases": [entry]}


@pytest.mark.parametrize("document", [
    {"schema_version": 2, "releases": []},
    history(headline="", items=[]),
    history(contributors=[]),
    history(contributors=["  "]),
    history(release_date=""),
    history(items=[]),
])
def test_package_gate_rejects_missing_current_version_details(tmp_path, document):
    source = tmp_path / "history.json"
    source.write_text(json.dumps(document), encoding="utf-8")
    with pytest.raises(ValueError):
        pack_engine.prepare_release_notes("1.2.3", tmp_path / "release", source)
    assert not (tmp_path / "release" / "aris_release_notes.json").exists()


def test_package_gate_writes_current_version_notes_to_release_directory(tmp_path):
    source = tmp_path / "history.json"
    source.write_text(json.dumps(history()), encoding="utf-8")
    target = pack_engine.prepare_release_notes("1.2.3", tmp_path / "release", source)
    assert target.name == "aris_release_notes.json"
    assert json.loads(target.read_text(encoding="utf-8"))["releases"][0]["contributors"] == ["Harken Search Team"]


def test_package_gate_rejects_duplicate_version_before_writing(tmp_path):
    source = tmp_path / "history.json"
    document = history()
    document["releases"].append(document["releases"][0].copy())
    source.write_text(json.dumps(document), encoding="utf-8")
    with pytest.raises(ValueError, match="duplicate versions"):
        pack_engine.prepare_release_notes("1.2.3", tmp_path / "release", source)


def test_package_workflow_checks_notes_before_creating_handoff():
    workflow = (ROOT / ".github/workflows/package-update.yml").read_text(encoding="utf-8")
    assert 'python -B tools/pack_engine.py --prepare-release-notes --version "$version" --release-dir "$release"' in workflow
    assert workflow.index("--prepare-release-notes") < workflow.index("New-Item -ItemType Directory -Force handoff/payload")
    assert workflow.index("--prepare-release-notes") < workflow.index("compile_installer(")
    assert 'if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }' in workflow
    assert "($_.Extension -in @('.exe', '.dll', '.sig', '.png', '.ttf', '.otf', '.json'" in workflow


def test_package_gate_rejects_unsupported_entries_not_shown_by_update_ui(tmp_path):
    source = tmp_path / "history.json"
    document = history(items=[])
    document["releases"][0]["entries"] = [{"title": "修复", "description": "仅新格式可见"}]
    source.write_text(json.dumps(document), encoding="utf-8")
    with pytest.raises(ValueError, match="detailed update notes"):
        pack_engine.prepare_release_notes("1.2.3", tmp_path / "release", source)


def test_installer_requires_notes_in_package():
    installer = (ROOT / "installer/HarkenSearch_Setup.iss").read_text(encoding="utf-8")
    assert 'Source: "{#SourceDir}\\aris_release_notes.json"; DestDir: "{app}"; Flags: ignoreversion' in installer
    assert 'aris_release_notes.json"; DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist' not in installer


def test_portable_pack_requires_notes_before_zip_creation(tmp_path):
    release = tmp_path / "release"
    release.mkdir()
    for filename in pack_engine.ALWAYS_INCLUDE:
        (release / filename).write_bytes(b"fixture")
    output = tmp_path / "output"
    with pytest.raises((ValueError, FileNotFoundError)):
        pack_engine.pack(release, output, "base", [], "1.2.3", secret=b"0" * 32, manifest={"version": 1, "groups": [], "features": []})
    assert not list(output.glob("*.zip"))


def test_direct_installer_compile_rejects_missing_notes_before_compiler(tmp_path, monkeypatch):
    source = tmp_path / "release"
    source.mkdir()
    monkeypatch.setattr(pack_engine, "find_inno_setup_compiler", lambda: pytest.fail("compiler reached"))
    with pytest.raises(FileNotFoundError, match="aris_release_notes.json"):
        pack_engine.compile_installer("1.2.3", source)


def test_prepared_notes_enter_the_actual_update_zip(tmp_path):
    source = tmp_path / "history.json"
    source.write_text(json.dumps(history()), encoding="utf-8")
    payload = tmp_path / "payload"
    pack_engine.prepare_release_notes("1.2.3", payload, source)
    bootstrap = tmp_path / "HarkenSearchBootstrap.exe"
    bootstrap.write_bytes(b"bootstrap-test-fixture")
    archive = build_deterministic_zip(payload, bootstrap, tmp_path / "update.zip")
    with zipfile.ZipFile(archive) as package:
        assert "aris_release_notes.json" in package.namelist()
        included = json.loads(package.read("aris_release_notes.json"))
    assert included["releases"][0]["version"] == "1.2.3"
    assert included["releases"][0]["contributors"] == ["Harken Search Team"]


@pytest.mark.parametrize("release_date", ["2026-02-30", "2026-1-2", "not-a-date"])
def test_package_gate_rejects_invalid_dates(tmp_path, release_date):
    source = tmp_path / "history.json"
    source.write_text(json.dumps(history(release_date=release_date)), encoding="utf-8")
    with pytest.raises(ValueError, match="release_date"):
        pack_engine.prepare_release_notes("1.2.3", tmp_path / "release", source)


def test_update_payload_replaces_old_notes_and_preserves_readable_history(tmp_path):
    old = history(version="1.2.2")["releases"][0]
    document = history(
        items=[],
        contributors=["HarkenDeveloper-zhongziyu"],
        sections=[{"title": "本次更新", "items": [{
            "name": "HarkenDeveloper-zhongziyu",
            "list": [{"title": "更新说明", "description": "更新后可阅读本次改动。"}],
        }]}],
    )
    document["releases"].append(old)
    source = tmp_path / "history.json"
    source.write_text(json.dumps(document, ensure_ascii=False), encoding="utf-8")
    payload = tmp_path / "payload"
    notes = pack_engine.prepare_release_notes("1.2.3", payload, source)
    bootstrap = tmp_path / "HarkenSearchBootstrap.exe"
    bootstrap.write_bytes(b"bootstrap-test-fixture")
    archive = build_deterministic_zip(payload, bootstrap, tmp_path / "update.zip")
    installed = tmp_path / "installed"
    pack_engine.write_release_notes_file(installed, {"schema_version": 2, "releases": [old]})
    with zipfile.ZipFile(archive) as package:
        package.extractall(installed)
    delivered = installed / pack_engine.RELEASE_NOTES_FILENAME
    assert delivered.read_bytes() == notes.read_bytes()
    assert json.loads(delivered.read_text(encoding="utf-8")) == document


def test_qa_exe_package_bundles_current_release_notes():
    script = (ROOT / "scripts/package_qa_exe.ps1").read_text(encoding="utf-8")
    assert '"aris_release_notes.json",' in script
    assert "--prepare-release-notes" in script
    assert 'QA release notes preparation failed' in script
    allowlist_start = script.index("$runtimeFileAllowlist = @(")
    allowlist_end = script.index(")", allowlist_start)
    assert script.index('"aris_release_notes.json",') < allowlist_end
    assert script.index("--prepare-release-notes") < allowlist_start or \
        script.index("--prepare-release-notes") > allowlist_end
