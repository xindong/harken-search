"""RQ-20260910-09: public CI subset and leftover delivery-name contracts."""

from __future__ import annotations

import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]

PUBLIC_GITHUB_PATHS = (
    ".github/SECURITY.md",
    ".github/pull_request_template.md",
    ".github/workflows/public-checks.yml",
    ".github/workflows/auto-merge.yml",
    ".github/workflows/qa-exe.yml",
    ".github/workflows/package-update.yml",
    ".github/workflows/settings-scroll-regression.yml",
    ".github/workflows/pr-merge-notify.yml",
)

INTERNAL_WORKFLOWS = (
    ".github/workflows/inner-source-gates.yml",
    ".github/workflows/claude-review.yml",
    ".github/workflows/release-update.yml",
)


FORBIDDEN_SECRET_NAMES = (
    "RELEASES_REPO_PAT",
    "SLACK_BOT_TOKEN",
    "ANTHROPIC_API_KEY",
    "ANTHROPIC_BASE_URL",
    "ANTHROPIC_AUTH_TOKEN",
)

FORBIDDEN_PRIVATE_URLS = (
    "xindong/aris-releases",
    "zhongziyu-cmd/aris-releases",
)

SECRETS_EXPR = re.compile(r"secrets\.([A-Za-z0-9_]+)")
ARIS_ZIP_PREFIX = re.compile(r"Aris_v")
ARIS_SETUP_PREFIX = re.compile(
    r"Aris-\{0\}-win-x64-setup\.exe|Aris-\{version\}-win-x64-setup\.exe|Aris-\*-win-x64-setup\.exe"
)


def _read(rel: str) -> str:
    return (ROOT / rel).read_text(encoding="utf-8")


def _optional_manifest_github_paths():
    manifest_py = ROOT / "tools" / "public_repo_manifest.py"
    if not manifest_py.is_file():
        return None
    import sys

    if str(ROOT) not in sys.path:
        sys.path.insert(0, str(ROOT))
    from tools.public_repo_manifest import PUBLIC_GITHUB_PATHS as live

    return tuple(live)


class PublicCiOpensrcTests(unittest.TestCase):
    def test_public_github_subset_keeps_standalone_delivery_and_excludes_private_integrations(self):
        live = _optional_manifest_github_paths()
        if live is not None:
            self.assertEqual(live, PUBLIC_GITHUB_PATHS)
        for rel in PUBLIC_GITHUB_PATHS:
            self.assertTrue((ROOT / rel).is_file(), rel)
        for rel in INTERNAL_WORKFLOWS:
            self.assertNotIn(rel, PUBLIC_GITHUB_PATHS)
            if live is not None:
                self.assertNotIn(rel, live)
            if (ROOT / rel).is_file() and live is not None:
                from tools.public_repo_manifest import is_public_path

                self.assertFalse(is_public_path(rel), rel)

    def test_public_subset_workflows_have_no_internal_secrets_or_private_release_urls(self):
        hits = []
        for rel in PUBLIC_GITHUB_PATHS:
            text = _read(rel)
            # Only the optional notifier may reference these repository secrets.
            allowed_secrets = {"GITHUB_TOKEN"}
            if rel == ".github/workflows/pr-merge-notify.yml":
                allowed_secrets.update({"SLACK_BOT_TOKEN", "ANTHROPIC_API_KEY", "ANTHROPIC_BASE_URL"})
            for name in FORBIDDEN_SECRET_NAMES:
                if name in text and name not in allowed_secrets:
                    hits.append("%s:secret:%s" % (rel, name))
            for snippet in FORBIDDEN_PRIVATE_URLS:
                if snippet in text:
                    hits.append("%s:url:%s" % (rel, snippet))
            for match in SECRETS_EXPR.finditer(text):
                secret_name = match.group(1)
                if secret_name not in allowed_secrets:
                    hits.append("%s:secrets.%s" % (rel, secret_name))
        self.assertEqual(hits, [])

    def test_pack_engine_zip_and_history_use_harken_search_names(self):
        import sys

        if str(ROOT / "tools") not in sys.path:
            sys.path.insert(0, str(ROOT / "tools"))
        import pack_engine

        source = _read("tools/pack_engine.py")
        self.assertRegex(
            source,
            r'zip_name = f"HarkenSearch_v\{version\}_\{edition_label\}_\{date_str\}\.zip"',
        )
        self.assertIsNone(ARIS_ZIP_PREFIX.search(source))
        self.assertEqual(pack_engine.RELEASE_HISTORY_JSON_PATH.name, "harken_release_history.json")
        self.assertTrue((ROOT / "installer" / "harken_release_history.json").is_file())
        self.assertFalse((ROOT / "installer" / "aris_release_history.json").exists())

    def test_package_update_handoff_setup_name_has_no_aris_prefix(self):
        source = _read(".github/workflows/package-update.yml")
        self.assertIn("handoff/installer/HarkenSearch-{0}-win-x64-setup.exe", source)
        self.assertIn("handoff/installer/HarkenSearch-*-win-x64-setup.exe", source)
        self.assertIsNone(ARIS_SETUP_PREFIX.search(source))
        self.assertNotIn("Aris-{0}-win-x64-setup.exe", source)


if __name__ == "__main__":
    unittest.main()
