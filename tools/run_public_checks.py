"""Run the source checks available in an independent public checkout."""
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
TESTS = (
    "tests/test_public_workflow_closure.py",
    "tests/test_inner_source_gates_entry.py",
    "tests/test_public_repository_rules.py",
    "tests/test_auto_merge_workflow.py",
    "tests/test_qa_workflow_identity.py",
    "tests/test_remove_card_directory.py",
    "tests/test_settings_body_scroll_height.py",
    "tests/test_public_manual_packaging.py",
    "tests/test_export_public_repository.py",
    "tests/test_public_repo_manifest.py",
    "tests/test_public_ci_opensrc.py",
    "tests/test_release_compliance_contract.py",
    "tests/test_third_party_sbom.py",
    "tests/test_localization_ledger.py",
    "tests/test_localization_purity.py",
    "tests/test_repository_update_migration.py",
    "tests/test_build_update_manifest.py",
    "tests/test_update_product_composition.py",
)


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="harken-public-checks-") as test_root:
        commands = (
            [sys.executable, "tools/check_release_compliance.py"],
            [sys.executable, "-m", "pytest", *TESTS, "-q", "--basetemp", str(Path(test_root) / "pytest")],
        )
        for command in commands:
            print("+ " + " ".join(command), flush=True)
            result = subprocess.run(command, cwd=ROOT, check=False)
            if result.returncode:
                return result.returncode
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
