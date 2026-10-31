"""Enumerate the path set that a public Harken Search clone may contain.

Private qa/current keeps the full docs/ tree. Publication is a filter, not a
delete: card 11 consumes this list when seeding the public repository.
"""

from __future__ import annotations

import argparse
import subprocess
import sys
from collections.abc import Iterable
from fnmatch import fnmatch
from pathlib import Path, PurePosixPath

EXCLUDED_PREFIXES = (
    ".agents/",
    ".cuevi/",
    ".openclaw/",
    ".vscode/",
    "agents/",
    "design-system/",
    "docs/architecture/",
    "docs/context/",
    "docs/governance/",
    "docs/plans/",
    "docs/process/",
    "docs/product/",
    "docs/superpowers/",
    "docs/templates/",
    "tests/website/",
    "website/",
)

# Internal governance / board / review / QA-packaging paths that must not ship.
EXCLUDED_GLOBS = (
    "tests/test_local_agent_docs.py",
    "tools/aris_design_review_server.py",
    "tools/start_aris_design_review.ps1",
    "tests/test_aris_design_review_server.py",
    "tools/cuevi_*",
    "tools/daily_self_review*",
    "tools/*daily_self_review*",
    "tools/qa_provenance_manifest.py",
    "tests/test_aris_skill_runtime.py",
    "tests/test_aris_speedup_tools.py",
    "tests/test_stage3_style_gate.py",
    "tools/aris_card_timer.py",
    "tools/aris_skill_runtime.py",
    "tools/aris_startup_pack.py",
    "tools/qa_dock_gestures.py",
    "tools/qa_retired_speed_runtime.py",
    "tools/qa_send_target_menu.py",
    "tools/database_switch_acceptance.py",
    "tools/database_switch_busy_acceptance.py",
    "tools/verify_cuevi_feedback_e2e.py",
    "scripts/slack_notify*",
    "scripts/tests/test_slack_notify.py",
    "tests/test_cuevi_*",
    "tests/test_daily_self_review*",
    "tests/test_qa_provenance_manifest.py",
    "tests/test_start_cuevi_airs_bat.py",
    "tests/test_aris_intake.py",
    "tests/test_aris_merge_closer.py",
    "tests/test_aris_pr_create.py",
    "tests/test_auro_gate.py",
    "tests/test_auro_process_gate.py",
    "tests/test_auro_workflow_cuevi_contract.py",
    "tests/test_qa_exe_packaging_contract.py",
    "tools/aris_bug_intake.py",
    "tools/aris_merge_closer.py",
    "tools/aris_pr_create.py",
    "tools/aris_requirement_intake.py",
    "tools/auro_gate.py",
    "tools/auro_process_gate.py",
    "tools/auro_requirement_intake.py",
    "tools/aris_gate.py",
    "tools/aris_process_gate.py",
    "tests/website/*",
)

PUBLIC_TOP_LEVEL_DIRS = (
    "assets",
    "cmake",
    "installer",
    "locale",
    "LICENSES",
    "reaper_extension",
    "resources",
    "scripts",
    "src",
    "tests",
    "thirdparty",
    "tools",
)

PUBLIC_TOP_LEVEL_FILES = (
    ".gitattributes",
    ".gitignore",
    "AI_FEATURE_DISCLOSURE.md",
    "ASSET_PROVENANCE.md",
    "BUILD_WINDOWS.md",
    "CMakeLists.txt",
    "CODE_OF_CONDUCT.md",
    "CONTRIBUTING.md",
    "LICENSE",
    "MAINTAINERS.md",
    "NOTICE",
    "PRIVACY.md",
    "README.md",
    "SECURITY.md",
    "THIRD_PARTY_COMPONENTS.json",
    "THIRD_PARTY_NOTICES.md",
    "requirements-ci.txt",
    "HarkenSearch.ico",
    "resources.rc",
    "test_channel_routing_settings.cpp",
    "test_time_stretch.cpp",
)

PUBLIC_GITHUB_PATHS = (
    ".github/SECURITY.md",
    ".github/pull_request_template.md",
    ".github/workflows/public-checks.yml",
    ".github/workflows/auto-merge.yml",
    ".github/workflows/qa-exe.yml",
    ".github/workflows/package-update.yml",
    ".github/workflows/settings-scroll-regression.yml",
)

PUBLIC_DOCS_PREFIXES = (
    "docs/behavior/",
    "docs/legal/",
    "docs/manual/",
)


def excluded_prefixes() -> tuple[str, ...]:
    return EXCLUDED_PREFIXES


def excluded_globs() -> tuple[str, ...]:
    return EXCLUDED_GLOBS


def _posix(path: str) -> str:
    return PurePosixPath(path.replace("\\", "/")).as_posix()


def is_public_path(path: str) -> bool:
    posix = _posix(path)
    if posix.startswith("./"):
        posix = posix[2:]
    if not posix or posix == ".":
        return False
    # Machine-readable product dependency rules are test data, not private
    # process history. Boundary tests require this exact file in a clean clone.
    if posix == "docs/architecture/stage-4-dependency-rules.json":
        return True
    for prefix in EXCLUDED_PREFIXES:
        name = prefix.rstrip("/")
        if posix == name or posix.startswith(prefix):
            return False
    if any(fnmatch(posix, pattern) for pattern in EXCLUDED_GLOBS):
        return False
    if posix.startswith("docs/"):
        return any(posix.startswith(prefix) for prefix in PUBLIC_DOCS_PREFIXES)
    if posix.startswith(".github/"):
        return posix in PUBLIC_GITHUB_PATHS
    if "/" not in posix:
        return posix in PUBLIC_TOP_LEVEL_FILES
    top = posix.split("/", 1)[0]
    return top in PUBLIC_TOP_LEVEL_DIRS


def _git_tracked_files(root: Path) -> list[str]:
    completed = subprocess.run(
        ["git", "-C", str(root), "ls-files", "-z"],
        check=False,
        capture_output=True,
    )
    if completed.returncode != 0:
        raise RuntimeError(completed.stderr.decode("utf-8", errors="replace") or "git ls-files failed")
    return [item.decode("utf-8") for item in completed.stdout.split(b"\0") if item]


def iter_public_paths(root: Path, paths: Iterable[str] | None = None) -> list[str]:
    tracked = list(paths) if paths is not None else _git_tracked_files(root)
    public = [path.replace("\\", "/") for path in tracked if is_public_path(path)]
    public.sort()
    return public


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="List or check the public-repo publication path set.")
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--check", action="store_true", help="Exit 1 if a listed path is not public.")
    parser.add_argument("path", nargs="*", help="Optional paths to classify instead of git ls-files.")
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    root = args.root.resolve()
    selected = iter_public_paths(root, args.path or None)
    if args.check:
        illegal = [path for path in (args.path or []) if not is_public_path(path)]
        if illegal:
            sys.stderr.write("not public:\n" + "\n".join(illegal) + "\n")
            return 1
        print(f"{len(selected)} public paths")
        return 0
    sys.stdout.write("\n".join(selected) + ("\n" if selected else ""))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
