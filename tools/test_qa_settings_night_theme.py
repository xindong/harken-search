import tempfile
import unittest
from pathlib import Path
from unittest.mock import Mock,patch
from qa_settings_night_theme import require_saved_palette,stop_owned_process,require_loaded_palette,require_window_size

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
            with self.assertRaises(AssertionError):require_saved_palette(root,'day')
            (root/'theme_schedule_day.json').write_text('{}',encoding='utf-8')
            with self.assertRaises(AssertionError):require_saved_palette(root,'day')
            (root/'theme_schedule_day.json').write_text('{"colors":{"mainWindowBG":[0,0,0,1]}}',encoding='utf-8')
            self.assertTrue(require_saved_palette(root,'day'))

    def test_app_warning_rejects_fallback_palette(self):
        with self.assertRaises(AssertionError):require_loaded_palette({'day_warning':'invalid palette'},'day')
        require_loaded_palette({'day_warning':''},'day')

    def test_shrunk_window_is_not_requested_size(self):
        with self.assertRaises(AssertionError):require_window_size([0,0,900,700],1120,800)
        require_window_size([720,307,1840,1107],1120,800)

if __name__=='__main__':unittest.main()
