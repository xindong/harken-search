"""A public checkout must contain everything its workflows execute."""
from pathlib import Path
import re
import unittest

from tools.public_repo_manifest import is_public_path, PUBLIC_GITHUB_PATHS

ROOT = Path(__file__).resolve().parents[1]
REQUIRED = {
    ".github/workflows/public-checks.yml",
    ".github/workflows/auto-merge.yml",
    ".github/workflows/qa-exe.yml",
    ".github/workflows/package-update.yml",
    ".github/workflows/settings-scroll-regression.yml",
    ".github/workflows/pr-merge-notify.yml",
}


class PublicWorkflowClosureTests(unittest.TestCase):
    def test_development_and_delivery_workflows_are_exported(self):
        self.assertEqual(REQUIRED - set(PUBLIC_GITHUB_PATHS), set())

    def test_executed_repository_files_survive_public_export(self):
        missing = []
        for workflow in PUBLIC_GITHUB_PATHS:
            if not workflow.startswith(".github/workflows/"):
                continue
            text = (ROOT / workflow).read_text(encoding="utf-8").replace("\\", "/")
            for name in re.findall(r"(?:tools|scripts|tests)/[A-Za-z0-9_./-]+\.(?:py|ps1|mjs|json)", text):
                if not (ROOT / name).is_file() or not is_public_path(name):
                    missing.append(f"{workflow} requires absent public file {name}")
        self.assertEqual(missing, [])

    def test_public_qa_has_a_shipppable_checklist(self):
        path = "docs/manual/QA_CHECKLIST.md"
        self.assertTrue((ROOT / path).is_file())
        self.assertTrue(is_public_path(path))
        self.assertTrue(is_public_path("scripts/package_qa_exe.ps1"))


if __name__ == "__main__":
    unittest.main()
