"""正式发布回执的仓库侧契约：字段、命名、脱敏与公开投影边界。"""

from __future__ import annotations

import copy
import json
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
RECEIPTS = ROOT / "docs" / "process" / "release-receipts"
SEMVER = re.compile(r"(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)\Z")
HEX40 = re.compile(r"[0-9a-f]{40}\Z")
HEX64 = re.compile(r"[0-9a-f]{64}\Z")
ABSOLUTE_PATH = re.compile(r"(?:[A-Za-z]:[\\/]|\\\\)")

FORBIDDEN_KEYS = {
    "user", "username", "password", "secret", "token", "credential",
    "credentials", "access_key", "secret_key", "bucket", "region", "cos",
    "prefix", "storage", "ak", "sk",
}
REQUIRED_KEYS = {
    "schema_version", "version", "channel", "build_id", "source_commit",
    "run_id", "prepared_at", "released_at", "product", "github", "files",
    "manifests", "notes", "signer", "installer", "update", "published_from",
}


def receipt_files() -> list[Path]:
    return sorted(RECEIPTS.glob("*.json"))


def load(path: Path) -> dict:
    value = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(value, dict):
        raise AssertionError(f"{path.name} root must be an object")
    return value


def walk_keys(value: object) -> list[str]:
    keys: list[str] = []
    if isinstance(value, dict):
        for key, item in value.items():
            keys.append(str(key))
            keys.extend(walk_keys(item))
    elif isinstance(value, list):
        for item in value:
            keys.extend(walk_keys(item))
    return keys


def walk_strings(value: object) -> list[str]:
    found: list[str] = []
    if isinstance(value, dict):
        for item in value.values():
            found.extend(walk_strings(item))
    elif isinstance(value, list):
        for item in value:
            found.extend(walk_strings(item))
    elif isinstance(value, str):
        found.append(value)
    return found


def validate_receipt(payload: dict, *, version: str) -> None:
    """仓库回执的固定关系；不满足就抛 AssertionError。"""
    assert payload.get("schema_version") == 1, payload.get("schema_version")
    assert isinstance(version, str) and SEMVER.fullmatch(version), version
    assert payload.get("version") == version, (payload.get("version"), version)
    assert payload.get("channel") == "stable", payload.get("channel")
    assert REQUIRED_KEYS.issubset(payload), REQUIRED_KEYS - set(payload)
    assert isinstance(payload.get("product"), str) and payload["product"].strip()
    assert HEX40.fullmatch(str(payload["source_commit"])), payload["source_commit"]
    assert HEX40.fullmatch(str(payload["build_id"])), payload["build_id"]
    assert payload["build_id"] == payload["source_commit"], "构建与来源必须是同一提交"
    assert isinstance(payload["run_id"], int) and payload["run_id"] > 0

    github = payload["github"]
    assert isinstance(github, dict), github
    assert isinstance(github.get("repository"), str) and github["repository"].strip()
    assert github.get("tag") == f"v{version}", github.get("tag")


class ReleaseReceiptContractTest(unittest.TestCase):
    def test_receipt_directory_has_a_receipt_per_released_version(self) -> None:
        files = receipt_files()
        self.assertTrue(files, "docs/process/release-receipts/ 下还没有正式发布回执")
        names = [path.name for path in files]
        self.assertEqual(len(names), len(set(names)))

    def test_each_receipt_matches_the_filename_and_schema(self) -> None:
        for path in receipt_files():
            with self.subTest(receipt=path.name):
                validate_receipt(load(path), version=path.stem)

    def test_files_are_plain_names_with_hashes_and_cover_installer_and_update(self) -> None:
        for path in receipt_files():
            payload = load(path)
            version = payload["version"]
            files = payload["files"]
            self.assertIsInstance(files, list)
            self.assertTrue(files)
            names: dict[str, dict] = {}
            for item in files:
                name = item["name"]
                self.assertNotIn("/", name)
                self.assertNotIn("\\", name)
                self.assertNotIn(name, names, f"{path.name} 文件名重复：{name}")
                self.assertGreater(item["size"], 0)
                self.assertTrue(HEX64.fullmatch(item["sha256"]), name)
                names[name] = item

            installer = payload["installer"]
            update = payload["update"]
            self.assertEqual(names[installer["name"]], installer)
            self.assertEqual(names[update["name"]], update)
            self.assertIn(version, installer["name"])
            self.assertIn(version, update["name"])

            for channel in ("canonical", "legacy"):
                manifest = payload["manifests"][channel]
                self.assertEqual(names[manifest["name"]]["sha256"], manifest["sha256"])
                self.assertEqual(
                    names[f'{manifest["name"]}.sig']["sha256"],
                    manifest["signature_sha256"],
                )
            self.assertTrue(payload["notes"]["key"])
            self.assertTrue(HEX64.fullmatch(payload["notes"]["sha256"]))
            self.assertTrue(payload["signer"]["subject"])
            self.assertTrue(payload["signer"]["thumbprint"])

    def test_receipts_do_not_archive_local_users_storage_or_credentials(self) -> None:
        for path in receipt_files():
            with self.subTest(receipt=path.name):
                payload = load(path)
                lowered = {key.lower() for key in walk_keys(payload)}
                self.assertFalse(lowered & FORBIDDEN_KEYS, lowered & FORBIDDEN_KEYS)
                self.assertEqual(set(payload["published_from"]), {"machine"})
                for text in walk_strings(payload):
                    self.assertIsNone(ABSOLUTE_PATH.search(text), text)

    def test_tampered_receipts_are_rejected(self) -> None:
        sample = load(receipt_files()[0])
        version = sample["version"]
        cases = {
            "missing product": lambda item: item.pop("product"),
            "null github": lambda item: item.update(github=None),
            "tag mismatch": lambda item: item["github"].update(tag="v0.0.1"),
            "different commits": lambda item: item.update(build_id="a" * 40),
            "missing published_from": lambda item: item.pop("published_from"),
        }
        for name, mutate in cases.items():
            with self.subTest(case=name):
                tampered = copy.deepcopy(sample)
                mutate(tampered)
                with self.assertRaises(AssertionError):
                    validate_receipt(tampered, version=version)

    def test_receipt_directory_and_its_test_stay_out_of_the_public_projection(self) -> None:
        from tools import public_repo_manifest

        self.assertIn("docs/process/", public_repo_manifest.EXCLUDED_PREFIXES)
        self.assertIn("tests/test_release_receipt_contract.py", public_repo_manifest.EXCLUDED_GLOBS)


if __name__ == "__main__":
    unittest.main()
