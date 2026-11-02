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
    def test_wrong_channel_identity_never_receives_exit(self):
        for mismatch in ('pid', 'exe_path', 'isolation_root'):
            with self.subTest(mismatch=mismatch), tempfile.TemporaryDirectory() as tmp:
                root = Path(tmp)
                exe = root / 'fixture.exe'
                exe.write_bytes(b'not launched: mocked process')
                process, desktop, driver = Mock(), Mock(), Mock()
                desktop.start.return_value = (process, 42)
                args = SimpleNamespace(exe=exe, isolation_root=root / 'profile', output=root / 'out')
                identity = {'pid': 42, 'exe_path': str(exe), 'isolation_root': str(args.isolation_root)}
                identity[mismatch] = 99 if mismatch == 'pid' else str(root / 'unrelated')
                driver.command.return_value = identity
                modules = {
                    'win32con': SimpleNamespace(WAIT_TIMEOUT=258, WAIT_OBJECT_0=0),
                    'win32event': SimpleNamespace(WaitForSingleObject=Mock(return_value=258)),
                    'win32gui': SimpleNamespace(),
                    'win32process': SimpleNamespace(GetExitCodeProcess=Mock(return_value=259),
                                                    TerminateProcess=Mock()),
                }
                with patch.dict(sys.modules, modules), patch.object(qa, 'PrivateDesktop', return_value=desktop), \
                        patch.object(qa, 'Driver', return_value=driver):
                    with self.assertRaises(AssertionError):
                        qa.run(args)
                self.assertFalse(any(c.args[0].get('action') == 'exit'
                                     for c in driver.command.call_args_list))
                modules['win32process'].TerminateProcess.assert_called_once_with(process, 1)
                desktop.close.assert_called_once()
                self.assertEqual(json.loads((args.output / 'report.json').read_text(encoding='utf-8'))['status'], 'failed')

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
                modules = {
                    'win32con': SimpleNamespace(WAIT_TIMEOUT=258, WAIT_OBJECT_0=0),
                    'win32event': SimpleNamespace(WaitForSingleObject=Mock(return_value=258)),
                    'win32gui': SimpleNamespace(),
                    'win32process': SimpleNamespace(GetExitCodeProcess=Mock(return_value=259),
                                                    TerminateProcess=Mock()),
                }
                args = SimpleNamespace(exe=exe, isolation_root=root / 'profile', output=root / 'out')
                identity = {'pid': 42, 'exe_path': str(exe), 'isolation_root': str(args.isolation_root)}
                driver.command.side_effect = [identity, identity, TimeoutError('exit command timed out')
                                              if exit_command_fails else {}]
                with patch.dict(sys.modules, modules), patch.object(qa, 'PrivateDesktop', return_value=desktop), \
                        patch.object(qa, 'Driver', return_value=driver), \
                        patch.object(qa, 'wait_state', side_effect=original_error), patch.object(qa.time, 'sleep'):
                    with self.assertRaisesRegex(RuntimeError, 'original identity failure') as caught:
                        qa.run(args)
                self.assertIs(caught.exception, original_error)
                self.assertEqual(sum(c.args[0].get('action') == 'exit'
                                     for c in driver.command.call_args_list), 1)
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
