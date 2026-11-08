import hashlib
import json
import subprocess
import sys
from pathlib import Path

from tools.verify_release_assembly import (
    REQUIRED_DIRECTORIES,
    REQUIRED_FILES,
    verify_release_assembly,
)


ROOT = Path(__file__).resolve().parents[1]


def test_missing_release_output_fails_closed(tmp_path: Path):
    issues = verify_release_assembly(tmp_path)
    assert "missing required release file: HarkenSearch.exe" in issues
    result = subprocess.run(
        [sys.executable, str(ROOT / "tools/verify_release_assembly.py"), str(tmp_path)],
        capture_output=True,
        text=True,
        check=False,
    )
    assert result.returncode == 1


def test_complete_release_output_passes(tmp_path: Path):
    for relative in REQUIRED_FILES:
        target = tmp_path / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(b"release")
    aris_hash = hashlib.sha256(b"release").hexdigest()
    (tmp_path / "harken-runtime-pair.json").write_text(
        json.dumps(
            {
                "arisSha256": aris_hash,
                "arisSize": len(b"release"),
                "bootstrapSha256": aris_hash,
                "bootstrapSize": len(b"release"),
                "buildId": "test",
                "channel": "stable",
                "config": "Release",
                "packageKeyId": "test",
                "publicKeyDigest": "0" * 64,
                "schemaVersion": 1,
                "sourceCommit": "a" * 40,
            }
        ),
        encoding="utf-8",
    )
    (tmp_path / "HarkenSearch.exe.sig").write_bytes(
        hashlib.sha512(b"release").digest()[:32]
    )
    (tmp_path / "HarkenSearchBootstrap.exe.sig").write_bytes(
        (aris_hash + "\n").encode("ascii")
    )
    for relative in REQUIRED_DIRECTORIES:
        directory = tmp_path / relative
        directory.mkdir(parents=True)
        (directory / "present.txt").write_text("present", encoding="utf-8")

    assert verify_release_assembly(tmp_path, "a" * 40) == []


def test_configured_similarity_dependencies_must_be_staged(tmp_path: Path):
    release = _complete_release(tmp_path)
    (release / "THIRD_PARTY_COMPONENTS.json").write_text(
        json.dumps({"components": [
            {"name": "ONNX Runtime", "distributed": True},
            {"name": "LAION CLAP HTSAT unfused audio encoder", "distributed": True},
        ]}),
        encoding="utf-8",
    )
    issues = verify_release_assembly(release)
    assert "missing configured similarity file: onnxruntime.dll" in issues
    assert "missing configured similarity file: models/clap/clap-audio.onnx" in issues
    assert "missing configured similarity file: models/clap/clap-mel-filters.f64" in issues
    assert "missing configured similarity file: models/clap/clap-window.f64" in issues

    for relative in (
        "onnxruntime.dll", "models/clap/clap-audio.onnx",
        "models/clap/clap-mel-filters.f64", "models/clap/clap-window.f64",
    ):
        target = release / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(b"configured similarity resource")
    assert verify_release_assembly(release) == []


def test_installer_collects_optional_similarity_runtime_and_models():
    script = (ROOT / "installer/HarkenSearch_Setup.iss").read_text(encoding="utf-8")
    assert 'Source: "{#SourceDir}\\onnxruntime.dll"; DestDir: "{app}";' in script
    assert 'Source: "{#SourceDir}\\models\\*"; DestDir: "{app}\\models";' in script


def _complete_release(tmp_path: Path) -> Path:
    release = tmp_path / "Release"
    release.mkdir()
    payload = b"present"
    for relative in REQUIRED_FILES:
        target = release / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(payload)
    digest = hashlib.sha256(payload).hexdigest()
    (release / "harken-runtime-pair.json").write_text(
        json.dumps(
            {
                "arisSha256": digest,
                "arisSize": len(payload),
                "bootstrapSha256": digest,
                "bootstrapSize": len(payload),
                "buildId": "test",
                "channel": "stable",
                "config": "Release",
                "packageKeyId": "test",
                "publicKeyDigest": "0" * 64,
                "schemaVersion": 1,
                "sourceCommit": "a" * 40,
            }
        ),
        encoding="utf-8",
    )
    (release / "HarkenSearch.exe.sig").write_bytes(hashlib.sha512(payload).digest()[:32])
    (release / "HarkenSearchBootstrap.exe.sig").write_bytes((digest + "\n").encode("ascii"))
    (release / "aris_release_notes.json").write_text(
        json.dumps(
            {
                "schema_version": 2,
                "releases": [
                    {
                        "version": "1.0.32",
                        "release_date": "2026-09-26",
                        "headline": "installer test",
                        "sections": [
                            {"items": [{"list": ["test"]}]}
                        ],
                        "contributors": ["test"],
                    }
                ],
            }
        ),
        encoding="utf-8",
    )
    for relative in REQUIRED_DIRECTORIES:
        directory = release / relative
        directory.mkdir(parents=True, exist_ok=True)
        (directory / "present.txt").write_text("present", encoding="utf-8")
    for filename in ("HarkenSearchProToolsBridge.exe", "reaper_harken.dll"):
        (release / filename).write_bytes(payload)
    return release


def test_release_build_compiles_installer_with_signature_verifier(tmp_path: Path):
    """正式构建必须把 signtool 和证书主体传给安装器，否则旧版升级清理不会生效。"""
    from tools.pack_engine import compile_installer

    release = _complete_release(tmp_path)
    template = tmp_path / "HarkenSearch_Setup.iss"
    generated = tmp_path / "HarkenSearch_Setup_generated.iss"
    template.write_text(
        '#define MyAppVersion "0.0.0"\n'
        '#define SourceDir "..\\build_final\\Release"\n',
        encoding="utf-8",
    )
    tool = tmp_path / "signtool.exe"
    tool.write_bytes(b"MZ")
    seen = {}

    def fake_compiler():
        return tmp_path / "ISCC.exe"

    def fake_run(command, **kwargs):
        seen["command"] = command
        seen["cwd"] = kwargs.get("cwd")

        class Result:
            returncode = 0
            stdout = ""
            stderr = ""

        return Result()

    import tools.pack_engine as pack_engine

    original_find = pack_engine.find_inno_setup_compiler
    original_run = pack_engine.subprocess.run
    pack_engine.find_inno_setup_compiler = fake_compiler
    pack_engine.subprocess.run = fake_run
    try:
        compile_installer(
            "1.0.32",
            release,
            template,
            generated,
            sign_tool_path=tool,
            trusted_signer_subject="CN=Harken Search",
        )
    finally:
        pack_engine.find_inno_setup_compiler = original_find
        pack_engine.subprocess.run = original_run

    command = [str(part) for part in seen["command"]]
    assert any(part.replace("\\", "/").endswith("ISCC.exe") for part in command)
    assert f"/DSignToolPath={tool.resolve()}" in command
    assert "/DTrustedSignerSubject=CN=Harken Search" in command
    assert str(generated.resolve()) in command


def test_release_build_rejects_a_signer_subject_that_can_break_the_define(tmp_path: Path):
    from tools.pack_engine import compile_installer

    release = _complete_release(tmp_path)
    template = tmp_path / "HarkenSearch_Setup.iss"
    generated = tmp_path / "generated.iss"
    template.write_text('#define MyAppVersion "0.0.0"\n', encoding="utf-8")
    tool = tmp_path / "signtool.exe"
    tool.write_bytes(b"MZ")
    try:
        compile_installer(
            "1.0.32",
            release,
            template,
            generated,
            sign_tool_path=tool,
            trusted_signer_subject="CN=O'Brien",
        )
    except ValueError as error:
        assert "trusted signer subject" in str(error)
    else:
        raise AssertionError("an apostrophe in the signer subject must be rejected")


def test_installer_script_binds_the_exact_release_directory(tmp_path: Path):
    from tools.pack_engine import build_installer_script

    template = tmp_path / "HarkenSearch_Setup.iss"
    generated = tmp_path / "HarkenSearch_Setup_generated.iss"
    release = tmp_path / "clean release"
    release.mkdir()
    template.write_text(
        '#define MyAppVersion "0.0.0"\n'
        '#define SourceDir "..\\build_final\\Release"\n',
        encoding="utf-8",
    )

    build_installer_script("1.0.24", release, template, generated)
    text = generated.read_text(encoding="utf-8")
    assert f'#define SourceDir "{release.resolve()}"' in text
