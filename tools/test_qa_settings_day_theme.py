import tempfile
import unittest
from pathlib import Path
from unittest.mock import Mock,patch
from qa_settings_day_theme import require_saved_palette,stop_owned_process

class OwnedProcessTests(unittest.TestCase):
    def test_identity_mismatch_never_sends_shared_exit(self):
        handle=Mock();send=Mock()
        with patch('win32event.WaitForSingleObject',side_effect=[258,0]),patch('win32process.TerminateProcess') as terminate:
            stop_owned_process(handle,False,send)
        send.assert_not_called();terminate.assert_called_once_with(handle,1);handle.Close.assert_called_once()

    def test_verified_identity_uses_graceful_exit_and_checks_code(self):
        handle=Mock();send=Mock()
        with patch('win32event.WaitForSingleObject',return_value=0),patch('win32process.GetExitCodeProcess',return_value=0),patch('win32process.TerminateProcess') as terminate:
            stop_owned_process(handle,True,send)
        send.assert_called_once();terminate.assert_not_called();handle.Close.assert_called_once()

    def test_missing_saved_palette_is_not_evidence(self):
        with tempfile.TemporaryDirectory() as raw:
            root=Path(raw)
            with self.assertRaises(AssertionError):require_saved_palette(root,'night')
            (root/'theme_schedule_night.json').write_text('{}',encoding='utf-8')
            self.assertEqual(require_saved_palette(root,'night'),b'{}')

if __name__=='__main__':unittest.main()
