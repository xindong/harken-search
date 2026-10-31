from __future__ import annotations

import json
import shutil
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]


def tool_path() -> Path:
    return ROOT / "tools" / "generate_third_party_sbom.py"


def run_tool(*args: str, cwd: Path = ROOT) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        ["python", str(tool_path()), *args], cwd=cwd, text=True, capture_output=True, check=False
    )


def manifest_copy(tmp_path: Path) -> Path:
    target = tmp_path / "THIRD_PARTY_COMPONENTS.json"
    shutil.copyfile(ROOT / "THIRD_PARTY_COMPONENTS.json", target)
    return target


def test_sbom_creates_missing_output_directories(tmp_path: Path):
    output = tmp_path / "missing" / "nested" / "third-party-sbom.cdx.json"
    diff_output = tmp_path / "missing" / "diff" / "third-party-sbom-diff.json"
    baseline = tmp_path / "base.json"
    assert run_tool("--output", str(baseline)).returncode == 0
    result = run_tool(
        "--output",
        str(output),
        "--compare-base",
        str(baseline),
        "--diff-output",
        str(diff_output),
    )
    assert result.returncode == 0, result.stderr
    assert output.is_file()
    assert diff_output.is_file()
    assert json.loads(output.read_text(encoding="utf-8"))["bomFormat"] == "CycloneDX"


def test_sbom_is_deterministic_and_cyclonedx(tmp_path: Path):
    first, second = tmp_path / "one.json", tmp_path / "two.json"
    assert run_tool("--output", str(first)).returncode == 0
    assert run_tool("--output", str(second)).returncode == 0
    assert first.read_bytes() == second.read_bytes()
    payload = json.loads(first.read_text(encoding="utf-8"))
    assert payload["bomFormat"] == "CycloneDX"
    assert payload["specVersion"] == "1.5"
    assert any(item["name"] == "Avid PTSL SDK" for item in payload["components"])


def test_sbom_diff_reports_license_change(tmp_path: Path):
    baseline_manifest = manifest_copy(tmp_path)
    baseline_sbom, current_sbom, diff = tmp_path / "base.json", tmp_path / "current.json", tmp_path / "diff.json"
    assert run_tool("--manifest", str(baseline_manifest), "--output", str(baseline_sbom)).returncode == 0
    payload = json.loads(baseline_manifest.read_text(encoding="utf-8"))
    payload["components"][0]["license_expression"] = "Apache-2.0"
    baseline_manifest.write_text(json.dumps(payload), encoding="utf-8")
    result = run_tool(
        "--manifest", str(baseline_manifest), "--output", str(current_sbom),
        "--compare-base", str(baseline_sbom), "--diff-output", str(diff)
    )
    assert result.returncode != 0, result.stdout + result.stderr
    assert json.loads(diff.read_text(encoding="utf-8"))["changed"][0]["licenses"]


def test_empty_baseline_reports_all_current_components_as_added(tmp_path: Path):
    output = tmp_path / "current.json"
    diff = tmp_path / "diff.json"
    result = run_tool("--output", str(output), "--empty-base", "--diff-output", str(diff))
    assert result.returncode != 0, result.stdout + result.stderr
    payload = json.loads(diff.read_text(encoding="utf-8"))
    current = json.loads(output.read_text(encoding="utf-8"))
    assert payload["removed"] == []
    assert payload["changed"] == []
    assert payload["added"] == sorted((item["name"] for item in current["components"]), key=str.casefold)


def test_sbom_diff_accepts_utf8_bom_baseline(tmp_path: Path):
    baseline, output, diff = tmp_path / "base.json", tmp_path / "current.json", tmp_path / "diff.json"
    assert run_tool("--output", str(baseline)).returncode == 0
    baseline.write_bytes(b"\xef\xbb\xbf" + baseline.read_bytes())
    result = run_tool("--output", str(output), "--compare-base", str(baseline), "--diff-output", str(diff))
    assert result.returncode == 0, result.stderr
    assert json.loads(diff.read_text(encoding="utf-8")) == {"added": [], "removed": [], "changed": []}


def test_sbom_accepts_utf8_bom_manifest(tmp_path: Path):
    manifest, output = manifest_copy(tmp_path), tmp_path / "current.json"
    manifest.write_bytes(b"\xef\xbb\xbf" + manifest.read_bytes())
    result = run_tool("--manifest", str(manifest), "--output", str(output))
    assert result.returncode == 0, result.stderr
    assert json.loads(output.read_text(encoding="utf-8"))["bomFormat"] == "CycloneDX"


def load_checker():
    import importlib.util

    spec = importlib.util.spec_from_file_location("release_checker", ROOT / "tools" / "check_release_compliance.py")
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


def test_manifest_rejects_missing_distribution_obligation(tmp_path: Path):
    checker = load_checker()
    manifest = manifest_copy(tmp_path)
    payload = json.loads(manifest.read_text(encoding="utf-8"))
    payload["components"][0].pop("distribution_obligation", None)
    manifest.write_text(json.dumps(payload), encoding="utf-8")
    issues = checker.validate_component_manifest(manifest, repo_root=ROOT)
    assert any("distribution_obligation" in issue for issue in issues)


def test_optional_similarity_components_have_reviewed_licenses_and_notices():
    checker = load_checker()
    issues = checker.validate_component_manifest(ROOT / "THIRD_PARTY_COMPONENTS.json", repo_root=ROOT)
    assert not issues, issues


def test_manifest_rejects_unknown_distribution_obligation_values(tmp_path: Path):
    checker = load_checker()
    manifest = manifest_copy(tmp_path)
    payload = json.loads(manifest.read_text(encoding="utf-8"))
    payload["components"][0]["distribution_obligation"] = {
        "copyleft": False,
        "link_mode": "shared-object",
        "commercial_restriction": False,
        "source_offer_required": False,
    }
    manifest.write_text(json.dumps(payload), encoding="utf-8")
    issues = checker.validate_component_manifest(manifest, repo_root=ROOT)
    assert any("unknown link_mode" in issue for issue in issues)


def test_manifest_rejects_copyleft_when_distributed(tmp_path: Path):
    checker = load_checker()
    manifest = manifest_copy(tmp_path)
    payload = json.loads(manifest.read_text(encoding="utf-8"))
    payload["components"][0]["distributed"] = True
    payload["components"][0]["distribution_obligation"] = {
        "copyleft": True,
        "link_mode": "static",
        "commercial_restriction": False,
        "source_offer_required": False,
    }
    manifest.write_text(json.dumps(payload), encoding="utf-8")
    issues = checker.validate_component_manifest(manifest, repo_root=ROOT)
    assert any("copyleft" in issue and "distributed" in issue for issue in issues)


def test_manifest_rejects_source_offer_required_without_offer_when_distributed(tmp_path: Path):
    checker = load_checker()
    manifest = manifest_copy(tmp_path)
    payload = json.loads(manifest.read_text(encoding="utf-8"))
    payload["components"][0]["distributed"] = True
    payload["components"][0]["distribution_obligation"] = {
        "copyleft": False,
        "link_mode": "static",
        "commercial_restriction": False,
        "source_offer_required": True,
    }
    manifest.write_text(json.dumps(payload), encoding="utf-8")
    issues = checker.validate_component_manifest(manifest, repo_root=ROOT)
    assert any("source_offer" in issue for issue in issues)


def test_manifest_rejects_notice_missing_exact_component_name(tmp_path: Path):
    checker = load_checker()
    manifest = manifest_copy(tmp_path)
    payload = json.loads(manifest.read_text(encoding="utf-8"))
    for component in payload["components"]:
        for relative in component["license_files"] + component["notice_files"]:
            target = tmp_path / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            if not target.exists():
                target.write_text("example\n", encoding="utf-8")
    notices = tmp_path / "THIRD_PARTY_NOTICES.md"
    original = (ROOT / "THIRD_PARTY_NOTICES.md").read_text(encoding="utf-8")
    notices.write_text(original.replace("Dear ImGui", "Immediate Mode UI"), encoding="utf-8")
    issues = checker.validate_component_manifest(manifest, repo_root=tmp_path, cmake_text="")
    assert any("Dear ImGui" in issue and "THIRD_PARTY_NOTICES.md" in issue for issue in issues)


def test_manifest_rejects_missing_related_work_item_rq_20260803_13(tmp_path: Path):
    checker = load_checker()
    manifest = manifest_copy(tmp_path)
    payload = json.loads(manifest.read_text(encoding="utf-8"))
    payload["policy"] = {
        "public_release": payload["policy"]["public_release"],
        "review_reference": payload["policy"]["review_reference"],
    }
    manifest.write_text(json.dumps(payload), encoding="utf-8")
    issues = checker.validate_component_manifest(manifest, repo_root=ROOT)
    assert any("RQ-20260803-13" in issue for issue in issues)


def test_manifest_rejects_unknown_license_policy_and_missing_notice(tmp_path: Path):
    checker = load_checker()
    manifest = manifest_copy(tmp_path)
    payload = json.loads(manifest.read_text(encoding="utf-8"))
    payload["components"][0]["policy_category"] = "not-reviewed"
    payload["components"][0]["notice_files"] = ["MISSING_NOTICE.md"]
    manifest.write_text(json.dumps(payload), encoding="utf-8")
    issues = checker.validate_component_manifest(manifest, repo_root=ROOT)
    assert any("unreviewed policy category" in issue for issue in issues)
    assert any("MISSING_NOTICE.md" in issue for issue in issues)


def test_manifest_rejects_bundled_default_excluded_sdk(tmp_path: Path):
    checker = load_checker()
    manifest = manifest_copy(tmp_path)
    payload = json.loads(manifest.read_text(encoding="utf-8"))
    avid = next(component for component in payload["components"] if component["name"] == "Avid PTSL SDK")
    avid["distributed"] = True
    manifest.write_text(json.dumps(payload), encoding="utf-8")
    issues = checker.validate_component_manifest(manifest, repo_root=ROOT)
    assert any("default release must not distribute excluded component" in issue for issue in issues)


def test_untracked_generated_installer_is_ignored(tmp_path: Path):
    checker = load_checker()
    generated = ROOT / "installer" / "HarkenSearch_Setup_generated.iss"
    generated.write_text("C:\\Users\\secret\n", encoding="utf-8")
    try:
        assert not any(path.as_posix() == "installer/HarkenSearch_Setup_generated.iss" for path, _, _ in checker.find_violations(ROOT))
    finally:
        generated.unlink(missing_ok=True)


def test_untracked_source_draft_does_not_affect_real_gate(tmp_path: Path):
    checker = load_checker()
    draft = ROOT / "src" / "untracked_compliance_draft.tmp"
    try:
        draft.write_text("reverse-engineered", encoding="utf-8")
        assert draft.relative_to(ROOT) not in checker.tracked_paths(ROOT)
        assert not any(path == draft.relative_to(ROOT) for path, _, _ in checker.find_violations(ROOT))
    finally:
        draft.unlink(missing_ok=True)


def test_release_directory_requires_sbom_and_ledger(tmp_path: Path):
    checker = load_checker()
    release = tmp_path / "Release"
    release.mkdir()
    assert any("THIRD_PARTY_COMPONENTS.json" in issue for issue in checker.validate_release_dir(release))


def test_cmake_default_policy_rejects_enabled_excluded_sdk():
    checker = load_checker()
    cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
    assert 'option(AURO_ENABLE_AUBIO_BPM "Link GPL aubio for BPM detection (makes binary GPL)" OFF)' in cmake
    assert 'option(AURO_BUNDLE_AVID_PTSL "Allow bundling Avid PTSLC_CPP.dll after manual license confirmation" OFF)' in cmake
    assert checker.validate_default_build_policy(ROOT) == []


def test_unknown_license_expression_fails_closed_even_if_marked_permissive(tmp_path: Path):
    checker = load_checker()
    manifest = manifest_copy(tmp_path)
    payload = json.loads(manifest.read_text(encoding="utf-8"))
    payload["components"][0]["license_expression"] = "LicenseRef-Unreviewed-Proprietary"
    payload["components"][0]["policy_category"] = "permissive"
    manifest.write_text(json.dumps(payload), encoding="utf-8")
    issues = checker.validate_component_manifest(manifest, repo_root=ROOT)
    assert any("unknown license expression is not reviewed" in issue for issue in issues)


def test_release_layout_accepts_copied_binary_provenance(tmp_path: Path):
    checker = load_checker()
    release = tmp_path / "Release"
    release.mkdir()
    payload = json.loads((ROOT / "THIRD_PARTY_COMPONENTS.json").read_text(encoding="utf-8"))
    (release / "THIRD_PARTY_COMPONENTS.json").write_text(json.dumps(payload), encoding="utf-8")
    for component in payload["components"]:
        for relative in component["license_files"] + component["notice_files"]:
            if relative == "thirdparty/BINARY_PROVENANCE.md":
                (release / "BINARY_PROVENANCE.md").write_text("copied by CMake\n", encoding="utf-8")
                continue
            target = release / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            if not target.exists():
                target.write_text("example\n", encoding="utf-8")
    issues = checker.validate_component_manifest(
        release / "THIRD_PARTY_COMPONENTS.json",
        repo_root=release,
        cmake_text="",
    )
    assert not any("BINARY_PROVENANCE.md" in issue for issue in issues)
    assert (ROOT / "thirdparty" / "BINARY_PROVENANCE.md").is_file()
    cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
    assert "thirdparty/BINARY_PROVENANCE.md" in cmake
    assert 'copy_if_different "${_meta}" "$<TARGET_FILE_DIR:HarkenSearch>/"' in cmake
