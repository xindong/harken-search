"""RED/GREEN contract tests for the add-only release catalog foundation."""

from __future__ import annotations

import copy
import json
from pathlib import Path
import shutil
import subprocess
import sys

import pytest


ROOT = Path(__file__).resolve().parents[2]
BUILDER = ROOT / "tools" / "build_release_catalog.py"
SOURCE_FIXTURE = Path(__file__).parent / "fixtures" / "release_catalog_source.json"
NODE_PROJECTOR = ROOT / "website" / "src" / "release" / "project.mjs"


def _strict_json(path: Path) -> dict:
    def unique_pairs(pairs: list[tuple[str, object]]) -> dict[str, object]:
        result: dict[str, object] = {}
        for key, value in pairs:
            if key in result:
                raise ValueError(f"duplicate JSON key: {key}")
            result[key] = value
        return result

    value = json.loads(path.read_text(encoding="utf-8"), object_pairs_hook=unique_pairs)
    assert isinstance(value, dict)
    return value


def _run_builder(source: Path, output: Path) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [sys.executable, "-B", str(BUILDER), "--source", str(source), "--output", str(output)],
        cwd=ROOT,
        text=True,
        encoding="utf-8",
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )


def _node_projection(catalog: bytes) -> dict:
    script = """
import fs from 'node:fs';
import { projectRelease } from './website/src/release/project.mjs';
const catalog = JSON.parse(fs.readFileSync(0, 'utf8'));
process.stdout.write(JSON.stringify(projectRelease(catalog)));
"""
    result = subprocess.run(
        ["node", "--input-type=module", "-e", script],
        cwd=ROOT,
        input=catalog,
        text=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    assert result.returncode == 0, result.stderr.decode("utf-8", errors="replace")
    value = json.loads(result.stdout.decode("utf-8"))
    assert isinstance(value, dict)
    return value


def test_same_source_is_deterministic_and_client_website_projections_match(
    tmp_path: Path,
) -> None:
    first_path = tmp_path / "first" / "release-catalog.json"
    second_path = tmp_path / "second" / "release-catalog.json"
    first = _run_builder(SOURCE_FIXTURE, first_path)
    assert first.returncode == 0, first.stderr
    second = _run_builder(SOURCE_FIXTURE, second_path)
    assert second.returncode == 0, second.stderr

    first_bytes = first_path.read_bytes()
    second_bytes = second_path.read_bytes()
    assert first_bytes == second_bytes
    first_bytes.decode("utf-8")

    catalog = _strict_json(first_path)
    client_projection = catalog["projections"]["client"]
    website_projection = catalog["projections"]["website"]
    assert client_projection == website_projection
    # The website source is maintained in its own repository. This checkout
    # still verifies both published projections from the shared catalog.
    if NODE_PROJECTOR.is_file():
        assert _node_projection(first_bytes) == client_projection

    source = _strict_json(SOURCE_FIXTURE)
    assert client_projection == {
        "channel": source["channel"],
        "release_revision": source["release_revision"],
        "version": source["version"],
        "asset": {
            "url": source["primary_asset"]["url"],
            "size": source["primary_asset"]["size"],
            "sha256": source["primary_asset"]["sha256"],
        },
    }


def test_builder_cli_is_available_without_external_workflow_writes() -> None:
    result = subprocess.run(
        [sys.executable, "-B", str(BUILDER), "--help"],
        cwd=ROOT,
        text=True,
        encoding="utf-8",
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    assert result.returncode == 0, result.stderr
    assert "--source" in result.stdout
    assert "--output" in result.stdout


def test_release_payload_fixture_is_repository_portable_binary() -> None:
    fixture = "tests/release_contract/fixtures/release_update_payload.bin"
    result = subprocess.run(
        [
            "git",
            "-c",
            "core.attributesFile=NUL",
            "check-attr",
            "binary",
            "--",
            fixture,
        ],
        cwd=ROOT,
        text=True,
        encoding="utf-8",
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    assert result.returncode == 0, result.stderr
    assert result.stdout.strip() == f"{fixture}: binary: set"


@pytest.mark.parametrize(
    "fault",
    [
        "missing_asset",
        "missing_asset_file",
        "not_ready",
        "mutable_url",
        "zero_size",
        "bad_sha256",
        "bad_signature_metadata",
    ],
)
def test_invalid_source_fails_closed_without_catalog(
    tmp_path: Path, fault: str
) -> None:
    source = _strict_json(SOURCE_FIXTURE)
    if fault == "missing_asset":
        source.pop("primary_asset")
    elif fault == "missing_asset_file":
        source["primary_asset"]["file"] = "missing-release.zip"
    elif fault == "not_ready":
        source["ready"] = False
    elif fault == "bad_schema_version":
        source["schema_version"] = True
    elif fault == "mutable_url":
        source["primary_asset"]["url"] = (
            "https://github.com/xindong/aris/releases/latest/download/"
            "Aris-1.2.3-win-x64-update.zip"
        )
    elif fault == "zero_size":
        source["primary_asset"]["size"] = 0
    elif fault == "bad_sha256":
        source["primary_asset"]["sha256"] = "not-a-sha256"
    elif fault == "bad_signature_metadata":
        source["signature"]["encoding"] = "base64"

    source_path = tmp_path / "source.json"
    source_path.write_text(json.dumps(source, indent=2) + "\n", encoding="utf-8")
    output = tmp_path / "catalog.json"
    result = _run_builder(source_path, output)
    assert result.returncode != 0
    assert not output.exists()


def test_schema_version_rejects_boolean_value(tmp_path: Path) -> None:
    source = _strict_json(SOURCE_FIXTURE)
    source["schema_version"] = True
    source_path = tmp_path / "source.json"
    source_path.write_text(json.dumps(source), encoding="utf-8")
    shutil.copyfile(
        SOURCE_FIXTURE.parent / "release_update_payload.bin",
        tmp_path / "release_update_payload.bin",
    )
    output = tmp_path / "catalog.json"

    result = _run_builder(source_path, output)
    assert result.returncode != 0
    assert "schema_version" in result.stdout
    assert not output.exists()


@pytest.mark.parametrize(
    "replacement",
    [
        "https://attacker.example/xindong/aris/",
        "https://github.com/another/repository/",
        "https://github.com:8443/xindong/aris/",
    ],
)
def test_builder_rejects_noncanonical_release_source(
    tmp_path: Path, replacement: str
) -> None:
    source = _strict_json(SOURCE_FIXTURE)
    original = "https://github.com/xindong/aris/"
    source["primary_asset"]["url"] = source["primary_asset"]["url"].replace(
        original, replacement
    )
    source_path = tmp_path / "source.json"
    source_path.write_text(json.dumps(source), encoding="utf-8")
    shutil.copyfile(
        SOURCE_FIXTURE.parent / "release_update_payload.bin",
        tmp_path / "release_update_payload.bin",
    )
    output = tmp_path / "catalog.json"

    result = _run_builder(source_path, output)

    assert result.returncode != 0
    assert "canonical GitHub release source" in result.stdout
    assert not output.exists()


def test_failed_rebuild_does_not_replace_existing_catalog(tmp_path: Path) -> None:
    output = tmp_path / "catalog.json"
    first = _run_builder(SOURCE_FIXTURE, output)
    assert first.returncode == 0, first.stderr
    previous = output.read_bytes()

    source = _strict_json(SOURCE_FIXTURE)
    source["ready"] = False
    invalid_source = tmp_path / "invalid.json"
    invalid_source.write_text(json.dumps(source), encoding="utf-8")
    failed = _run_builder(invalid_source, output)
    assert failed.returncode != 0
    assert output.read_bytes() == previous


def test_website_projection_source_contains_no_fixture_version_or_download_url() -> None:
    if not NODE_PROJECTOR.is_file():
        pytest.skip("website projector is maintained in the separate website repository")
    source = _strict_json(SOURCE_FIXTURE)
    text = NODE_PROJECTOR.read_text(encoding="utf-8")
    assert source["version"] not in text
    assert source["primary_asset"]["url"] not in text
