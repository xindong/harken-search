from pathlib import Path
from types import SimpleNamespace
from unittest.mock import Mock
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import qa_dock_grip_modal as qa


def fake_windows(shutdown_error=None):
    process = Mock()
    driver = Mock()
    if shutdown_error:
        driver.command.side_effect = shutdown_error
    con = SimpleNamespace(WAIT_TIMEOUT=258, WAIT_OBJECT_0=0)
    event = Mock()
    event.WaitForSingleObject.side_effect = [258, 258, 0]
    api = Mock()
    api.GetExitCodeProcess.return_value = 1
    return process, driver, con, event, api


def test_unverified_preview_never_receives_queue_shutdown():
    process, driver, con, event, api = fake_windows()
    code, error = qa.shutdown_preview(process, driver, False, con, event, api)
    assert code == 1
    assert error is None
    driver.command.assert_not_called()
    api.TerminateProcess.assert_called_once_with(process, 1)
    process.Close.assert_called_once()


def test_queue_timeout_still_terminates_spawned_preview():
    process, driver, con, event, api = fake_windows(TimeoutError('queue stalled'))
    code, error = qa.shutdown_preview(process, driver, True, con, event, api)
    assert code == 1
    assert 'queue stalled' in error
    driver.command.assert_called_once_with({'cmd': 'info_edit_input', 'action': 'exit'}, timeout=8)
    api.TerminateProcess.assert_called_once_with(process, 1)
    process.Close.assert_called_once()