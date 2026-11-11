"""Structural contract for the executable update-release boundary."""

from __future__ import annotations

import ast
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys

import pytest
import yaml


ROOT = Path(__file__).resolve().parents[2]
LEGACY_RELEASE_WORKFLOW = ROOT / ".github" / "workflows" / "release-update.yml"
PACKAGE_WORKFLOW = ROOT / ".github" / "workflows" / "package-update.yml"
COORDINATOR = ROOT / "tools" / "release_update.py"
BUILDER = ROOT / "tools" / "build_update_manifest.py"
FIXTURE = Path(__file__).parent / "fixtures" / "expected_update_release_contract.json"
PLAN = ROOT / "docs" / "plans" / "2026-10-31-release-1.0.43.md"
CLIENT_REPOSITORY_SOURCES = (
    ROOT / "CMakeLists.txt",
    ROOT / "src" / "update" / "UpdateReleaseConfig.h",
)

ARTIFACTS = {
    "bootstrap": "HarkenSearchBootstrap.exe",
    "update_zip": "Aris-{version}-win-x64-update.zip",
    "installer": "HarkenSearch-{version}-win-x64-setup.exe",
    "testing_manifest": "manifest-win-x64-testing.json",
    "stable_manifest": "manifest-win-x64-stable.json",
    "detached_signature_suffix": ".sig",
}
EXTERNAL_BLOCKERS = {
    "bootstrap_executable_target_and_delivery_layout",
    "bootstrap_restart_recovery_readiness",
    "packaging_profile_and_legacy_hmac_governance",
    "data_compatibility_authority",
    "installer_silent_target_elevation_arguments_exit_cleanup",
    "installer_authenticity_policy",
    "signed_release_note_policy",
    "github_public_anonymous_visibility",
    "github_environment_reviewers_branch_self_review_administration",
    "channel_secret_public_key_provisioning",
    "runtime_channel_public_key_pinning",
    "tag_immutability_retention_governance",
    "packaging_cmake_serial_prerequisite",
}


def _strict_json(path: Path) -> dict:
    def unique(pairs):
        result = {}
        for key, value in pairs:
            if key in result:
                raise ValueError(f"duplicate key: {key}")
            result[key] = value
        return result

    value = json.loads(path.read_text(encoding="utf-8"), object_pairs_hook=unique)
    assert isinstance(value, dict)
    return value


def _package_workflow() -> dict:
    value = yaml.load(PACKAGE_WORKFLOW.read_text(encoding="utf-8"), Loader=yaml.BaseLoader)
    assert isinstance(value, dict) and isinstance(value.get("jobs"), dict)
    return value


def _run_lines(job: dict) -> list[str]:
    result = []
    for step in job.get("steps", []):
        if isinstance(step, dict) and isinstance(step.get("run"), str):
            for raw in step["run"].splitlines():
                command = raw.split("#", 1)[0].strip()
                if command:
                    result.append(command)
    return result


def test_fixture_contains_only_frozen_release_facts_and_explicit_external_blockers() -> None:
    fixture = _strict_json(FIXTURE)
    assert fixture["artifacts"] == ARTIFACTS
    assert fixture["canonical_manifest"] == {
        "schema_version": 1,
        "builder": "tools/build_update_manifest.py",
        "signed_payload": "raw manifest bytes",
        "signature_encoding": "lowercase hexadecimal",
        "asset_role": "immutable update ZIP only",
    }
    assert set(fixture["external_fail_closed_dependencies"]) == EXTERNAL_BLOCKERS
    forbidden = {
        "workflow_files", "implementation_files", "release_inventory",
        "promotion_state", "tag_policy", "secret_names", "job_topology",
    }
    assert forbidden.isdisjoint(fixture)


def test_only_canonical_builder_handles_private_signing() -> None:
    coordinator_tree = ast.parse(COORDINATOR.read_text(encoding="utf-8"))
    imports = {
        alias.name
        for node in ast.walk(coordinator_tree)
        if isinstance(node, ast.ImportFrom) and node.module == "tools.build_update_manifest"
        for alias in node.names
    }
    assert {"build_manifest", "build_manifest_with_ephemeral_key"} <= imports
    forbidden = {"Ed25519PrivateKey", "SigningKey"}
    assert not any(isinstance(node, ast.Name) and node.id in forbidden for node in ast.walk(coordinator_tree))
    assert not any(isinstance(node, ast.Attribute) and node.attr == "sign" for node in ast.walk(coordinator_tree))
    builder_tree = ast.parse(BUILDER.read_text(encoding="utf-8"))
    assert any(isinstance(node, ast.Attribute) and node.attr == "sign" for node in ast.walk(builder_tree))


def test_actions_only_builds_the_unsigned_handoff_for_the_local_publisher() -> None:
    assert not LEGACY_RELEASE_WORKFLOW.exists()
    workflow = _package_workflow()
    package = workflow["jobs"]["package"]
    source = PACKAGE_WORKFLOW.read_text(encoding="utf-8")

    assert package["permissions"]["contents"] == "read"
    assert package["env"]["ARIS_UPDATE_GITHUB_REPOSITORY"] == "xindong/harken-search"
    assert "TARGET_RELEASES_REPO" not in source
    assert "gh release create" not in source
    assert "gh release edit" not in source
    assert "gh release upload" not in source

    upload_names = {
        step.get("with", {}).get("name")
        for step in package["steps"]
        if str(step.get("uses", "")).startswith("actions/upload-artifact@")
    }
    assert {
        "aris-update-payload", "aris-bootstrap", "aris-installer",
    } <= upload_names


def test_package_workflow_and_client_share_the_canonical_repository() -> None:
    workflow = PACKAGE_WORKFLOW.read_text(encoding="utf-8")
    assert "ARIS_UPDATE_GITHUB_REPOSITORY: xindong/harken-search" in workflow
    for path in CLIENT_REPOSITORY_SOURCES:
        source = path.read_text(encoding="utf-8")
        assert "xindong/harken-search" in source
        assert "xindong/aris-releases" not in source
        assert "zhongziyu-cmd/aris-releases" not in source


def test_packaging_workflow_builds_the_three_immutable_handoff_artifacts() -> None:
    workflow = _package_workflow()
    assert set(workflow["jobs"]) == {"package"}
    job = workflow["jobs"]["package"]
    assert job.get("runs-on") == "windows-2022"
    permissions = job.get("permissions", {})
    assert permissions.get("contents") == "read"
    assert set(permissions) <= {
        "contents", "pull-requests", "checks", "actions",
    }
    source = PACKAGE_WORKFLOW.read_text(encoding="utf-8")
    assert job["env"]["ARIS_UPDATE_MIRROR_BASE_URL"] == "https://harkensearch.xd.com/aris/"
    assert '"-DARIS_UPDATE_MIRROR_BASE_URL=$env:ARIS_UPDATE_MIRROR_BASE_URL"' in source
    assert "ARIS_BOOTSTRAP_PACKAGE_PUBLIC_KEY_HEX" in source
    assert "HarkenSearchBootstrap.exe" in source
    assert "HarkenSearch-{0}-win-x64-setup.exe" in source
    assert "HarkenSearch-{version}-win-x64-update.zip" in Path(ROOT, "tools/release_update.py").read_text(encoding="utf-8")
    for artifact in ("aris-update-payload", "aris-bootstrap", "aris-installer"):
        assert artifact in source


def test_package_assembly_verifies_the_runtime_receipt_matches_the_official_source_commit() -> None:
    workflow = _package_workflow()
    package = workflow["jobs"]["package"]
    assert package["env"]["BUILD_ID"] == "${{ github.sha }}"

    assembly = next(
        step["run"]
        for step in package["steps"]
        if step.get("name") == "Assemble payload Bootstrap and installer handoff"
    )
    command = 'python -B tools/verify_release_assembly.py "$release" --source-commit "$env:BUILD_ID"'
    assert command in assembly
    commands = [line.strip() for line in assembly.splitlines() if line.strip()]
    verifier_index = commands.index(command)
    assert commands[verifier_index + 1] == 'if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }'
    command_index = assembly.index(command)
    assert assembly.index("Resolve-Path 'build-release/Release'") < command_index
    for marker in (
        "New-Item",
        "compile_installer(",
    ):
        assert assembly.index(marker) > command_index
    copy_positions = [match.start() for match in re.finditer("Copy-Item", assembly)]
    assert copy_positions
    assert all(command_index < position for position in copy_positions)
    source = PACKAGE_WORKFLOW.read_text(encoding="utf-8")
    assert source.index(command) < source.index("actions/upload-artifact@")
    assert "REVIEW_HEAD_SHA" not in command
    assert "${{ inputs.head_sha }}" not in command
    assert "inputs.head_sha" not in command


def test_package_assembly_stops_before_handoff_when_source_commit_verification_fails(tmp_path: Path) -> None:
    package = _package_workflow()["jobs"]["package"]
    assembly = next(
        step["run"]
        for step in package["steps"]
        if step.get("name") == "Assemble payload Bootstrap and installer handoff"
    )

    release = tmp_path / "build-release" / "Release"
    release.mkdir(parents=True)
    (release / "HarkenSearch.exe").write_bytes(b"payload")
    (release / "HarkenSearchBootstrap.exe").write_bytes(b"bootstrap")
    installer = tmp_path / "fixture-installer.exe"
    installer.write_bytes(b"installer")
    (tmp_path / "python.cmd").write_text(
        "@echo off\n"
        "if \"%~1\"==\"-B\" exit /b 23\n"
        "echo %* | %SystemRoot%\\System32\\findstr.exe /C:\"compile_installer\" >nul\n"
        "if not errorlevel 1 (\n"
        "  echo %FAKE_INSTALLER%\n"
        "  exit /b 0\n"
        ")\n"
        "echo 1.2.3\n"
        "exit /b 0\n",
        encoding="utf-8",
    )
    environment = os.environ.copy()
    environment["PATH"] = f"{tmp_path}{os.pathsep}{environment['PATH']}"
    environment["FAKE_INSTALLER"] = str(installer)
    environment["BUILD_ID"] = "a" * 40
    powershell = shutil.which("pwsh") or shutil.which("powershell")
    if powershell is None:
        pytest.skip("PowerShell is unavailable")

    result = subprocess.run(
        [powershell, "-NoProfile", "-NonInteractive", "-Command", assembly],
        cwd=tmp_path,
        env=environment,
        text=True,
        encoding="utf-8",
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        check=False,
    )

    assert result.returncode == 23, result.stdout
    assert not (tmp_path / "handoff").exists(), result.stdout


def test_package_workflow_contains_no_private_release_key() -> None:
    source = PACKAGE_WORKFLOW.read_text(encoding="utf-8")
    assert "ARIS_UPDATE_SIGNING_KEY_B64" not in source
    assert "ARIS_PACKAGE_SIGNING_KEY_B64" not in source
    private_pem = re.compile(r"-----BEGIN [A-Z ]*PRIVATE KEY-----")
    for path in (PACKAGE_WORKFLOW, COORDINATOR, BUILDER):
        assert not private_pem.search(path.read_text(encoding="utf-8"))


def test_update_payload_requires_and_copies_both_legal_documents_before_handoff() -> None:
    package = _package_workflow()["jobs"]["package"]
    assembly = next(
        step["run"]
        for step in package["steps"]
        if step.get("name") == "Assemble payload Bootstrap and installer handoff"
    )
    assert "EULA.md" in assembly
    assert "PRIVACY_POLICY.md" in assembly
    assert "OPEN_SOURCE_RELEASE_NOTICE.md" in assembly
    assert "EULA.en.md" in assembly
    assert "PRIVACY_POLICY.en.md" in assembly
    assert "OPEN_SOURCE_RELEASE_NOTICE.en.md" in assembly
    assert "Test-Path -LiteralPath $source -PathType Leaf" in assembly
    assert "if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }" in assembly
    eula_copy = assembly.index("EULA.md")
    privacy_copy = assembly.index("PRIVACY_POLICY.md")
    handoff = assembly.index("New-Item -ItemType Directory -Force handoff/payload")
    assert eula_copy < handoff
    assert privacy_copy < handoff


def test_packaging_workflow_cannot_reach_the_legacy_testing_release_cli() -> None:
    source = PACKAGE_WORKFLOW.read_text(encoding="utf-8")
    assert "release_update.py" not in source
    assert "testing" not in source.lower()
    assert "-DAIRS_RELEASE_CHANNEL=stable" in source


def test_plan_documents_signed_stable_release_gates() -> None:
    text = PLAN.read_text(encoding="utf-8")
    for gate in ("签署第一方程序", "旧版实际下载、替换、重启", "GitHub 和 COS",
                 "两套兼容清单", "正式指针", "不可变版本对象"):
        assert gate in text
