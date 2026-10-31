"""The documented local gate must execute the checks used by public CI."""

import subprocess
import sys
import unittest
from pathlib import Path
from unittest.mock import patch

from tools import run_inner_source_gates


ROOT = Path(__file__).resolve().parents[1]


class InnerSourceGatesEntryTests(unittest.TestCase):
    def test_runs_public_checks_and_returns_their_status(self):
        public_checks = ROOT / "tools" / "run_public_checks.py"
        for status in (0, 7):
            with self.subTest(status=status), patch.object(
                run_inner_source_gates.subprocess, "run",
                return_value=subprocess.CompletedProcess([], status),
            ) as run:
                self.assertEqual(run_inner_source_gates.main(), status)
                run.assert_called_once_with(
                    [sys.executable, str(public_checks)], cwd=ROOT, check=False
                )


if __name__ == "__main__":
    unittest.main()
