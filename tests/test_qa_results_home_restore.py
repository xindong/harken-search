"""Failure-path coverage for the private-desktop results regression runner."""
import json
from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import Mock, patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import qa_results_home_restore as qa


class CleanupEvidenceTests(unittest.TestCase):
    def test_exit_failures_preserve_original_error_report_and_close_desktop(self):
        for exit_command_fails in (True, False):
            with self.subTest(exit_command_fails=exit_command_fails), tempfile.TemporaryDirectory() as tmp:
                root = Path(tmp)
                exe = root / 'fixture.exe'
                exe.write_bytes(b'not launched: mocked process')
                process = Mock()
                desktop = Mock()
                desktop.start.return_value = (process, 42)
                driver = Mock()
                original_error = RuntimeError('original identity failure')
                driver.command.side_effect = [original_error, TimeoutError('exit command timed out')
                                              if exit_command_fails else {}]
                modules = {
                    'win32con': SimpleNamespace(WAIT_TIMEOUT=258, WAIT_OBJECT_0=0),
                    'win32event': SimpleNamespace(WaitForSingleObject=Mock(return_value=258)),
                    'win32gui': SimpleNamespace(),
                    'win32process': SimpleNamespace(GetExitCodeProcess=Mock(return_value=259),
                                                    TerminateProcess=Mock()),
                }
                args = SimpleNamespace(exe=exe, isolation_root=root / 'profile', output=root / 'out')
                with patch.dict(sys.modules, modules), patch.object(qa, 'PrivateDesktop', return_value=desktop), \
                        patch.object(qa, 'Driver', return_value=driver):
                    with self.assertRaisesRegex(RuntimeError, 'original identity failure') as caught:
                        qa.run(args)
                self.assertIs(caught.exception, original_error)
                desktop.close.assert_called_once()
                process.Close.assert_called_once()
                modules['win32process'].TerminateProcess.assert_called_once_with(process, 1)
                report = json.loads((args.output / 'report.json').read_text(encoding='utf-8'))
                self.assertEqual(report['status'], 'failed')
                self.assertIn('original identity failure', report['error'])
                self.assertTrue(report['cleanup_errors'])
                self.assertIn('finished_at', report)


if __name__ == '__main__':
    unittest.main()
