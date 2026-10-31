import hashlib
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

import pytest


REPO_ROOT = Path(__file__).resolve().parents[1]
SCRIPT = REPO_ROOT / "scripts" / "package_qa_exe.ps1"
WORKFLOW = REPO_ROOT / ".github" / "workflows" / "qa-exe.yml"
GENERATOR = REPO_ROOT / "tools" / "generate_runtime_pair.py"


def test_qa_packaging_script_is_exe_only_and_fixed_to_qa_current():
    source = SCRIPT.read_text(encoding="utf-8")

    assert 'ExpectedBranch = "qa/current"' in source
    assert 'BuildDir = "build_qa"' in source
    assert 'DistDir = "dist_qa"' in source
    assert 'git rev-parse --path-format=absolute --git-common-dir' in source
    assert 'Join-Path $sharedRoot $DistDir' in source
    assert 'Join-Path $distPath "Airs\\HarkenSearch.exe"' in source
    assert "Refusing to deploy outside the exact QA target directory" in source
    assert "Compress-Archive" not in source
    assert "dist_latest" not in source
    assert "setup.exe" not in source.lower()
    for required in (
        "HarkenSearchBootstrap.exe",
        "HarkenSearchBootstrap.exe.sig",
        "harken-runtime-pair.json",
        "generate_runtime_pair.py",
        "runtime-pair.publish",
        "runtime-pair.previous",
        "Move-Item",
        '"docs\\legal\\EULA.md"',
        '"docs\\legal\\PRIVACY_POLICY.md"',
    ):
        assert required in source
    assert "Get-ChildItem -LiteralPath $binDir -File | Where-Object" not in source
    assert 'foreach ($subdir in @("docs"' not in source


def test_qa_workflow_builds_only_qa_current_and_uploads_exe_artifact():
    source = WORKFLOW.read_text(encoding="utf-8")

    assert "branches:" in source
    assert "- qa/current" in source
    assert "package_qa_exe.ps1" in source
    assert "qa-exe" in source
    assert "dist_qa/Airs" in source or "dist_qa\\Airs" in source
    assert "dist_latest" not in source
    assert "setup.exe" not in source.lower()
    assert "Compress-Archive" not in source


def test_qa_build_has_explicit_feedback_disabled_workflow_and_safe_enabled_mode():
    source = SCRIPT.read_text(encoding="utf-8")
    workflow = WORKFLOW.read_text(encoding="utf-8")
    for variable in (
        "AIRS_CUEVI_FEEDBACK_ENDPOINT",
        "AIRS_CUEVI_FEEDBACK_KEY",
        "AIRS_CUEVI_PROJECT_ID",
        "AIRS_CUEVI_BOARD_ID",
        "AIRS_CUEVI_MILESTONE_ID",
    ):
        assert variable in source
    assert "FeedbackMode" in source
    assert '[ValidateSet("enabled", "disabled")]' in source
    assert "feedbackCachePath" in source
    assert '"-C", $feedbackCachePath' in source
    assert "-DAIRS_CUEVI_FEEDBACK_KEY=" not in source
    output_lines = [
        line for line in source.splitlines()
        if "Write-Host" in line or "Write-Output" in line or "Out-File" in line
    ]
    assert "AIRS_CUEVI_FEEDBACK_KEY" not in "\n".join(output_lines)
    assert "run: .\\scripts\\package_qa_exe.ps1 -FeedbackMode disabled" in workflow
    job_env = workflow.split("steps:", 1)[0]
    build_step = workflow.split("- name: Build QA EXE snapshot", 1)[1].split("- name:", 1)[0]
    assert "AIRS_CUEVI_FEEDBACK_KEY" not in workflow
    assert "AIRS_CUEVI_FEEDBACK_KEY" not in job_env
    assert "AIRS_CUEVI_FEEDBACK_KEY" not in build_step


def test_qa_workflow_provisions_runtime_pair_variables():
    source = SCRIPT.read_text(encoding="utf-8")
    workflow = WORKFLOW.read_text(encoding="utf-8")
    assert "STABLE_UPDATE_PACKAGE_KEY_ID" in source
    assert "UPDATE_PUBLIC_KEY_B64" in workflow
    assert "ARIS_BOOTSTRAP_PACKAGE_KEY_ID" in source
    assert "ARIS_BOOTSTRAP_PACKAGE_PUBLIC_KEY_HEX" in workflow


def _make_config_gate_fixture(tmp_path: Path):
    root = tmp_path / "repo"
    (root / "scripts").mkdir(parents=True)
    shutil.copy2(SCRIPT, root / "scripts" / SCRIPT.name)
    subprocess.run(["git", "init"], cwd=root, check=True, capture_output=True)
    subprocess.run(["git", "checkout", "-b", "qa/current"], cwd=root, check=True, capture_output=True)
    subprocess.run(["git", "config", "user.email", "feedback-config@test.invalid"], cwd=root, check=True)
    subprocess.run(["git", "config", "user.name", "Feedback Config Test"], cwd=root, check=True)
    (root / "CMakeLists.txt").write_text("cmake_minimum_required(VERSION 3.16)\n", encoding="utf-8")
    subprocess.run(["git", "add", "."], cwd=root, check=True, capture_output=True)
    subprocess.run(["git", "commit", "-m", "fixture"], cwd=root, check=True, capture_output=True)
    fake_bin = tmp_path / "fake-bin"
    fake_bin.mkdir()
    marker = tmp_path / "cmake-called.txt"
    (fake_bin / "cmake.cmd").write_text(
        '@echo called>"%CMAKE_MARKER%"\n@exit /b 0\n', encoding="utf-8"
    )
    command = [
        "powershell", "-NoProfile", "-File", str(root / "scripts" / SCRIPT.name),
        "-BuildDir", str(root / "build"), "-DistDir", str(root / "dist_qa"),
        "-PackageKeyId", "aris-stable-test", "-PackagePublicKeyHex", "11" * 32,
    ]
    return root, fake_bin, marker, command


def test_qa_packager_rejects_missing_feedback_before_cmake(tmp_path: Path):
    root, fake_bin, marker, command = _make_config_gate_fixture(tmp_path)
    env = os.environ.copy()
    env.pop("AIRS_CUEVI_FEEDBACK_KEY", None)
    env.update({
        "AIRS_CUEVI_FEEDBACK_ENDPOINT": "https://cuevi.invalid/feedback",
        "AIRS_CUEVI_PROJECT_ID": "project-qa",
        "AIRS_CUEVI_BOARD_ID": "board-qa",
        "AIRS_CUEVI_MILESTONE_ID": "milestone-qa",
        "CMAKE_MARKER": str(marker),
        "PATH": str(fake_bin) + os.pathsep + env["PATH"],
    })
    result = subprocess.run(command, cwd=root, text=True, capture_output=True, check=False, env=env)
    assert result.returncode != 0
    assert "AIRS_CUEVI_FEEDBACK_KEY" in (result.stdout + result.stderr)
    assert not marker.exists()


def test_qa_packager_feedback_disabled_reaches_cmake_without_feedback_config(
    tmp_path: Path,
):
    root, fake_bin, marker, command = _make_config_gate_fixture(tmp_path)
    command.extend(["-FeedbackMode", "disabled"])
    env = os.environ.copy()
    for name in (
        "AIRS_CUEVI_FEEDBACK_ENDPOINT",
        "AIRS_CUEVI_FEEDBACK_KEY",
        "AIRS_CUEVI_PROJECT_ID",
        "AIRS_CUEVI_BOARD_ID",
        "AIRS_CUEVI_MILESTONE_ID",
    ):
        env.pop(name, None)
    env.update({
        "CMAKE_MARKER": str(marker),
        "PATH": str(fake_bin) + os.pathsep + env["PATH"],
    })

    result = subprocess.run(
        command, cwd=root, text=True, capture_output=True, check=False, env=env
    )

    assert marker.exists()
    output = result.stdout + result.stderr
    assert "is required for the QA feedback build" not in output


@pytest.mark.parametrize("sentinel", ['legacy"key', r"legacy\key"])
def test_feedback_key_special_characters_survive_real_cmake_expansion(
    tmp_path: Path, sentinel: str
) -> None:
    root, fake_bin, _marker, command = _make_config_gate_fixture(tmp_path)
    args_log = tmp_path / "cmake-args.txt"
    cache_copy = tmp_path / "initial-cache.cmake"
    (fake_bin / "cmake.cmd").write_text(
        '@echo %*>"%CMAKE_ARGS_LOG%"\n@copy "%6" "%CMAKE_CACHE_COPY%" >nul\n@exit /b 1\n',
        encoding="utf-8",
    )
    real_cmake = shutil.which("cmake")
    assert real_cmake is not None
    env = os.environ.copy()
    env.update({
        "AIRS_CUEVI_FEEDBACK_ENDPOINT": "https://cuevi.invalid/feedback",
        "AIRS_CUEVI_FEEDBACK_KEY": sentinel,
        "AIRS_CUEVI_PROJECT_ID": "project-qa",
        "AIRS_CUEVI_BOARD_ID": "board-qa",
        "AIRS_CUEVI_MILESTONE_ID": "milestone-qa",
        "CMAKE_ARGS_LOG": str(args_log),
        "CMAKE_CACHE_COPY": str(cache_copy),
        "PATH": str(fake_bin) + os.pathsep + env["PATH"],
    })

    result = subprocess.run(
        command, cwd=root, text=True, capture_output=True, check=False, env=env
    )
    assert result.returncode != 0
    assert args_log.exists() and cache_copy.exists()
    output = result.stdout + result.stderr
    assert sentinel not in output
    assert sentinel not in args_log.read_text(encoding="utf-8")
    assert sentinel not in cache_copy.read_text(encoding="utf-8")

    template = tmp_path / "feedback-config.h.in"
    generated = tmp_path / "feedback-config.h"
    cmake_script = tmp_path / "expand-feedback-config.cmake"
    template.write_text(
        '#define AIRS_CUEVI_FEEDBACK_KEY "@AIRS_CUEVI_FEEDBACK_KEY@"\n',
        encoding="utf-8",
    )
    cmake_script.write_text(
        'include("${CACHE_PATH}")\n'
        'configure_file("${TEMPLATE_PATH}" "${OUTPUT_PATH}" @ONLY)\n',
        encoding="utf-8",
    )
    expanded = subprocess.run(
        [
            real_cmake,
            f"-DCACHE_PATH={cache_copy.as_posix()}",
            f"-DTEMPLATE_PATH={template.as_posix()}",
            f"-DOUTPUT_PATH={generated.as_posix()}",
            "-P", str(cmake_script),
        ],
        cwd=root, text=True, capture_output=True, check=False, env=env,
    )
    assert expanded.returncode == 0, expanded.stderr
    assert sentinel not in (expanded.stdout + expanded.stderr)
    macro = generated.read_text(encoding="utf-8").strip()
    prefix = "#define AIRS_CUEVI_FEEDBACK_KEY "
    assert macro == prefix + json.dumps(sentinel)
    assert json.loads(macro.removeprefix(prefix)) == sentinel


@pytest.mark.parametrize("bad_key", ["legacy\nkey", "legacy\rkey", "legacy\x01key"])
def test_feedback_key_control_characters_fail_before_cmake(
    tmp_path: Path, bad_key: str
) -> None:
    root, fake_bin, marker, command = _make_config_gate_fixture(tmp_path)
    env = os.environ.copy()
    env.update({
        "AIRS_CUEVI_FEEDBACK_ENDPOINT": "https://cuevi.invalid/feedback",
        "AIRS_CUEVI_FEEDBACK_KEY": bad_key,
        "AIRS_CUEVI_PROJECT_ID": "project-qa",
        "AIRS_CUEVI_BOARD_ID": "board-qa",
        "AIRS_CUEVI_MILESTONE_ID": "milestone-qa",
        "CMAKE_MARKER": str(marker),
        "PATH": str(fake_bin) + os.pathsep + env["PATH"],
    })
    result = subprocess.run(
        command, cwd=root, text=True, capture_output=True, check=False, env=env
    )
    assert result.returncode != 0
    assert "AIRS_CUEVI_FEEDBACK_KEY" in (result.stdout + result.stderr)
    assert bad_key not in (result.stdout + result.stderr)
    assert not marker.exists()


def test_qa_packager_publishes_verified_pair_and_preserves_previous_on_failure(
    tmp_path: Path,
) -> None:
    root = tmp_path / "repo"
    (root / "scripts").mkdir(parents=True)
    (root / "tools").mkdir()
    shutil.copy2(SCRIPT, root / "scripts" / SCRIPT.name)
    shutil.copy2(GENERATOR, root / "tools" / GENERATOR.name)
    shutil.copy2(REPO_ROOT / "tools/build_offline_manual.py", root / "tools/build_offline_manual.py")
    shutil.copy2(REPO_ROOT / "tools/pack_engine.py", root / "tools/pack_engine.py")
    (root / "src").mkdir()
    shutil.copy2(REPO_ROOT / "src/Version.h", root / "src/Version.h")
    (root / "installer").mkdir()
    shutil.copy2(
        REPO_ROOT / "installer/harken_release_history.json",
        root / "installer/harken_release_history.json",
    )
    manual = root / "docs/manual"
    manual.mkdir(parents=True)
    for name in ("HarkenSearch-Manual-zh-CN.pdf", "HarkenSearch-Manual-zh-CN.json"):
        shutil.copy2(REPO_ROOT / "docs/manual" / name, manual / name)
    (root / "QA说明.md").write_text(
        "## 要查哪些部分\n打包测试\n## 怎么查\n运行测试\n## 怎样算过\n散列一致\n"
        "## 这次不查什么\n不运行产品\n## 没通过时留下什么\n测试日志\n", encoding="utf-8")
    build = root / "build" / "Release"
    build.mkdir(parents=True)
    aris = build / "HarkenSearch.exe"
    bootstrap = build / "HarkenSearchBootstrap.exe"
    aris.write_bytes(b"qa-aris")
    bootstrap.write_bytes(b"qa-bootstrap")
    (build / "HarkenSearch.exe.sig").write_bytes(b"a" * 32)
    legal_source = build / "docs" / "legal"
    legal_source.mkdir(parents=True)
    eula_source = legal_source / "EULA.md"
    privacy_source = legal_source / "PRIVACY_POLICY.md"
    eula_source.write_text("fixture EULA\n", encoding="utf-8")
    privacy_source.write_text("fixture privacy\n", encoding="utf-8")
    (legal_source / "OPEN_SOURCE_RELEASE_NOTICE.md").write_text(
        "fixture open-source release notice\n", encoding="utf-8")
    for name in ("EULA.en.md", "PRIVACY_POLICY.en.md", "OPEN_SOURCE_RELEASE_NOTICE.en.md"):
        (legal_source / name).write_text("fixture English legal document\n", encoding="utf-8")
    unrelated_doc = build / "docs" / "internal-build-note.txt"
    unrelated_doc.write_text("must not be published\n", encoding="utf-8")
    subprocess.run(["git", "init"], cwd=root, check=True, capture_output=True)
    subprocess.run(["git", "checkout", "-b", "qa/current"], cwd=root, check=True,
                   capture_output=True)
    subprocess.run(["git", "config", "user.email", "runtime-pair@test.invalid"],
                   cwd=root, check=True)
    subprocess.run(["git", "config", "user.name", "Runtime Pair Test"],
                   cwd=root, check=True)
    subprocess.run(["git", "add", "."], cwd=root, check=True)
    subprocess.run(["git", "commit", "-m", "fixture"], cwd=root, check=True,
                   capture_output=True)
    source_commit = subprocess.check_output(
        ["git", "rev-parse", "HEAD"], cwd=root, text=True
    ).strip()
    public_key = "11" * 32
    finalized = subprocess.run(
        [
            sys.executable, "-B", str(root / "tools" / GENERATOR.name), "finalize",
            "--aris", str(aris), "--bootstrap", str(bootstrap),
            "--bootstrap-sig", str(build / "HarkenSearchBootstrap.exe.sig"),
            "--receipt", str(build / "harken-runtime-pair.json"),
                "--source-commit", source_commit, "--build-id",
                source_commit[:12],
            "--channel", "qa", "--config", "Release",
            "--key-id", "aris-stable-2026-07-30",
            "--public-key-hex", public_key,
        ],
        cwd=root, text=True, capture_output=True, check=False,
    )
    assert finalized.returncode == 0, finalized.stderr
    fixed = root / "dist_qa" / "Airs"
    fixed.mkdir(parents=True)
    (fixed / "previous.sentinel").write_bytes(b"old-generation")
    command = [
        "powershell", "-NoProfile", "-File", str(root / "scripts" / SCRIPT.name),
        "-SkipBuild", "-BuildDir", str(root / "build"), "-DistDir", "dist_qa",
        "-Config", "Release", "-PackageKeyId", "aris-stable-2026-07-30",
        "-PackagePublicKeyHex", public_key,
    ]
    feedback_env = {
        "AIRS_CUEVI_FEEDBACK_ENDPOINT": "https://cuevi.invalid/v1/feedback-submissions",
        "AIRS_CUEVI_FEEDBACK_KEY": "qa-feedback-key-sentinel-do-not-print",
        "AIRS_CUEVI_PROJECT_ID": "project-qa",
        "AIRS_CUEVI_BOARD_ID": "board-qa",
        "AIRS_CUEVI_MILESTONE_ID": "milestone-qa",
    }
    process_env = os.environ.copy()
    process_env.update(feedback_env)

    fixture_script = root / "scripts" / SCRIPT.name
    original_script = fixture_script.read_text(encoding="utf-8")
    legal_copy = "            Copy-Item -LiteralPath $source -Destination $destination"
    staging_mutations = (
        (
            legal_copy + "\n            Remove-Item -LiteralPath $destination -Force",
            "Published legal document is missing",
        ),
        (
            legal_copy + '\n            Set-Content -LiteralPath $destination -Value "tampered"',
            "Published legal document differs from current build",
        ),
    )
    for mutated_copy, expected_error in staging_mutations:
        fixture_script.write_text(
            original_script.replace(legal_copy, mutated_copy, 1), encoding="utf-8"
        )
        rejected_staging = subprocess.run(
            command, cwd=root, text=True, capture_output=True, check=False,
            env=process_env,
        )
        assert rejected_staging.returncode != 0
        assert expected_error in rejected_staging.stdout + rejected_staging.stderr
        assert (fixed / "previous.sentinel").read_bytes() == b"old-generation"
    fixture_script.write_text(original_script, encoding="utf-8")

    published = subprocess.run(command, cwd=root, text=True, capture_output=True,
                               check=False, env=process_env)
    assert published.returncode == 0, published.stderr
    assert not (fixed / "previous.sentinel").exists()
    assert (fixed / "HarkenSearch.exe").read_bytes() == aris.read_bytes()
    assert (fixed / "HarkenSearchBootstrap.exe").read_bytes() == bootstrap.read_bytes()
    assert (fixed / "docs" / "legal" / "EULA.md").read_bytes() == eula_source.read_bytes()
    assert (
        fixed / "docs" / "legal" / "PRIVACY_POLICY.md"
    ).read_bytes() == privacy_source.read_bytes()
    assert not (fixed / "docs" / unrelated_doc.name).exists()
    ledger = {
        path.relative_to(fixed).as_posix(): hashlib.sha256(path.read_bytes()).hexdigest()
        for path in fixed.rglob("*") if path.is_file()
    }

    privacy_source.unlink()
    missing_legal_rejected = subprocess.run(
        command, cwd=root, text=True, capture_output=True, check=False, env=process_env
    )
    assert missing_legal_rejected.returncode != 0
    assert "Required legal document is missing" in (
        missing_legal_rejected.stdout + missing_legal_rejected.stderr
    )
    after_missing_legal = {
        path.relative_to(fixed).as_posix(): hashlib.sha256(path.read_bytes()).hexdigest()
        for path in fixed.rglob("*") if path.is_file()
    }
    assert after_missing_legal == ledger
    privacy_source.write_text("fixture privacy\n", encoding="utf-8")

    receipt = build / "harken-runtime-pair.json"
    receipt_document = json.loads(receipt.read_text(encoding="utf-8"))
    receipt_document["buildId"] = "wrong-build"
    receipt.write_text(json.dumps(receipt_document), encoding="utf-8")
    metadata_rejected = subprocess.run(command, cwd=root, text=True,
                                       capture_output=True, check=False, env=process_env)
    assert metadata_rejected.returncode != 0
    after_metadata_tamper = {
        path.relative_to(fixed).as_posix(): hashlib.sha256(path.read_bytes()).hexdigest()
        for path in fixed.rglob("*") if path.is_file()
    }
    assert after_metadata_tamper == ledger

    finalized = subprocess.run(
        [
            sys.executable, "-B", str(root / "tools" / GENERATOR.name), "finalize",
            "--aris", str(aris), "--bootstrap", str(bootstrap),
            "--bootstrap-sig", str(build / "HarkenSearchBootstrap.exe.sig"),
            "--receipt", str(receipt), "--source-commit", source_commit,
            "--build-id", source_commit[:12], "--channel", "qa",
            "--config", "Release", "--key-id", "aris-stable-2026-07-30",
            "--public-key-hex", public_key,
        ], cwd=root, text=True, capture_output=True, check=False,
    )
    assert finalized.returncode == 0, finalized.stderr

    bootstrap.write_bytes(b"swapped-stale-bootstrap")
    rejected = subprocess.run(command, cwd=root, text=True, capture_output=True,
                              check=False, env=process_env)
    assert rejected.returncode != 0
    after = {
        path.relative_to(fixed).as_posix(): hashlib.sha256(path.read_bytes()).hexdigest()
        for path in fixed.rglob("*") if path.is_file()
    }
    assert after == ledger
