#!/usr/bin/env python3
"""Run the same public source checks as the repository CI workflow."""

from __future__ import annotations

import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def main() -> int:
    public_checks = ROOT / "tools" / "run_public_checks.py"
    return subprocess.run(
        [sys.executable, str(public_checks)], cwd=ROOT, check=False
    ).returncode


if __name__ == "__main__":
    raise SystemExit(main())
