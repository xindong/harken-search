"""The Pilot QA driver must reject ordinary or mismatched executables before launch."""
import sys
import tempfile
import unittest
import unittest.mock as mock
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import qa_pilot_nested_roots


class PilotQaPreflightTests(unittest.TestCase):
    def test_plain_executable_is_rejected_before_output_or_launch(self):
        with tempfile.TemporaryDirectory() as temp:
            base = Path(temp)
            exe = base / "HarkenSearch.exe"
            exe.write_bytes(b"ordinary executable without a preview root")
            output = base / "output"
            args = mock.Mock(exe=exe, isolation_root=base / "isolation", output=output)
            with mock.patch.object(qa_pilot_nested_roots.subprocess, "Popen",
                                   side_effect=AssertionError("launched")) as launch:
                with self.assertRaisesRegex(ValueError, "isolation root"):
                    qa_pilot_nested_roots.run(args)
            launch.assert_not_called()
            self.assertFalse(output.exists())

    def test_different_compiled_root_is_rejected_before_output_or_launch(self):
        with tempfile.TemporaryDirectory() as temp:
            base = Path(temp)
            exe = base / "HarkenSearch.exe"
            exe.write_bytes(b"prefix\0" + (base / "other").as_posix().encode() + b"\0suffix")
            output = base / "output"
            args = mock.Mock(exe=exe, isolation_root=base / "isolation", output=output)
            with mock.patch.object(qa_pilot_nested_roots.subprocess, "Popen",
                                   side_effect=AssertionError("launched")) as launch:
                with self.assertRaisesRegex(ValueError, "isolation root"):
                    qa_pilot_nested_roots.run(args)
            launch.assert_not_called()
            self.assertFalse(output.exists())


if __name__ == "__main__":
    unittest.main()
