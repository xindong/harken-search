"""Compile actual UI predicates with checked STL iterators (BUG-20261027-07)."""
from pathlib import Path
import glob
import re
import subprocess
import pytest

ROOT = Path(__file__).resolve().parents[1]

@pytest.fixture(scope='module')
def predicate_exe(tmp_path_factory):
    source = (ROOT / 'src/main.cpp').read_text(encoding='utf-8')
    start = source.index('// Keep iterator pairs and displayed details on the same locked copy.')
    block = source[start:source.index('const int scanIngested', start)]
    snapshot = re.search(r'const auto scanErrorSnapshot = state.getScanErrors\(\);', block)
    any_expr = re.search(r'const bool networkUnreachable = std::any_of\([\s\S]*?\n\s*\}\);', block).group()
    count_expr = re.search(r'const int unreachableHint = static_cast<int>\(std::count_if\([\s\S]*?\n\s*\}\)\);', block).group()
    progress_expr = re.search(r'const bool scanProgressSurfaceActive =[\s\S]*?;', source).group()
    result_expr = re.search(r'const bool scanResultSummaryActive =[\s\S]*?;', source).group()
    setup = sorted(glob.glob('C:/Program Files/Microsoft Visual Studio/*/*/VC/Auxiliary/Build/vcvars64.bat'))
    assert setup, 'VS C++ toolchain required for checked-iterator regression'
    folder = tmp_path_factory.mktemp('scan-error-snapshot')
    cpp = folder / 'predicate.cpp'
    cpp.write_text('''#include <algorithm>
#include <atomic>
#include <vector>
#include <string>
#include <cstdlib>
#include <crtdbg.h>
#include <windows.h>
struct State {
  bool isScanning = false;
  std::atomic<bool> scanCompletionPending{false};
  int scanTotal = 0, scanErrorCount = 0;
  bool scanWasPaused = false, scanWasCancelled = false;
  std::vector<std::string> messages;
  int reads = 0;
  std::vector<std::string> getScanErrors() { ++reads; return messages; }
};
int main(int argc, char** argv) {
  SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
  _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
  _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
  _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
  State state;
  int expected = std::atoi(argv[1]);
  if (expected == 1) state.messages = {"Scan_NetworkLibraryUnreachable: missing"};
  if (expected == 2) state.messages = {"decode error", "Scan_NetworkLibraryUnreachable: a", "Scan_NetworkLibraryUnreachable: b"};
''' + (snapshot.group() if snapshot else '') + '\n' + any_expr + '\n' + count_expr + '''
  if (networkUnreachable != (expected > 0) || unreachableHint != expected) return 2;
  if (state.reads != 1) return 3;
  if (argc == 7) {
    state.isScanning = std::atoi(argv[2]) != 0;
    const bool scanForegroundCompletionActive = std::atoi(argv[3]) != 0;
    const bool scanCompleteSummaryActive = std::atoi(argv[4]) != 0;
    const bool scanFailureSummaryActive = std::atoi(argv[5]) != 0;
''' + progress_expr + '''
    if (scanProgressSurfaceActive != (std::atoi(argv[6]) != 0)) return 4;
  }
  if (argc == 10) {
    state.isScanning = std::atoi(argv[2]) != 0;
    state.scanCompletionPending = std::atoi(argv[3]) != 0;
    const bool scanCompleteSummaryActive = std::atoi(argv[4]) != 0;
    state.scanTotal = std::atoi(argv[5]);
    state.scanErrorCount = std::atoi(argv[6]);
    state.scanWasPaused = std::atoi(argv[7]) != 0;
    state.scanWasCancelled = std::atoi(argv[8]) != 0;
''' + result_expr + '''
    if (scanResultSummaryActive != (std::atoi(argv[9]) != 0)) return 5;
  }
  return 0;
}
''', encoding='utf-8')
    script = folder / 'compile.cmd'
    script.write_text(f'@echo off\ncall "{setup[-1]}" >nul\nif errorlevel 1 exit /b 1\ncl /nologo /std:c++17 /EHsc /MDd /Od /D_ITERATOR_DEBUG_LEVEL=2 "{cpp}" /Fe:predicate.exe\n', encoding='utf-8')
    result = subprocess.run(['cmd', '/c', str(script)], cwd=folder, capture_output=True, text=True)
    assert result.returncode == 0, result.stdout + result.stderr
    return folder / 'predicate.exe'

@pytest.mark.parametrize('count', [0, 1, 2], ids=['empty', 'unreachable', 'mixed-errors'])
def test_scan_result_predicates_share_one_snapshot(predicate_exe, count):
    result = subprocess.run([str(predicate_exe), str(count)], capture_output=True, text=True, timeout=10)
    assert result.returncode == 0, result.stdout + result.stderr

def test_no_iterator_from_temporary_scan_errors():
    source = (ROOT / 'src/main.cpp').read_text(encoding='utf-8')
    assert not re.search(r'getScanErrors\(\)\.(?:begin|end)\(', source)

def test_scan_summary_exposes_all_failure_categories():
    source = (ROOT / 'src/main.cpp').read_text(encoding='utf-8')
    app_state = (ROOT / 'src/AppState.h').read_text(encoding='utf-8')
    locale = (ROOT / 'locale/harken.ui.strings.json').read_text(encoding='utf-8')
    for token in (
        'scanFailureInaccessibleCount',
        'scanFailureUnreadableFileCount',
        'scanFailureNoAudioCount',
        'scanFailureDatabaseWriteCount',
        'scanFailureOtherCount',
        'Main_ScanFailureInaccessibleFmt',
        'Main_ScanFailureUnreadableFmt',
        'Main_ScanFailureNoAudioFmt',
        'Main_ScanFailureWriteFmt',
        'Main_ScanFailureOtherFmt',
    ):
        assert token in source or token in app_state or token in locale

@pytest.mark.parametrize('scanning,finishing,success,failure,progress_visible', [
    (1, 0, 0, 0, 1),
    (1, 0, 0, 1, 1),
    (0, 1, 0, 1, 1),
    (0, 0, 1, 0, 1),
    (0, 0, 0, 1, 0),
    (0, 0, 0, 0, 0),
], ids=['running', 'running-with-errors', 'finishing', 'success', 'failed', 'dismissed'])
def test_scan_progress_does_not_show_finished_failures(
        predicate_exe, scanning, finishing, success, failure, progress_visible):
    args = [0, scanning, finishing, success, failure, progress_visible]
    result = subprocess.run([str(predicate_exe), *map(str, args)],
                            capture_output=True, text=True, timeout=10)
    assert result.returncode == 0, 'Scan progress visibility is wrong: ' + result.stdout + result.stderr


def test_failure_summary_has_its_own_window_after_progress_closes():
    source = (ROOT / 'src/main.cpp').read_text(encoding='utf-8')
    modal = 'BeginTransientScrollbarPopupModal(L(Dlg_ScanResults)'
    assert modal in source, 'Scan results must have a separate modal'
    transfer_start = source.index('if (scanResultSummaryActive) {')
    transfer_end = source.index('if (scanProgressSurfaceActive)', transfer_start)
    transfer = source[transfer_start:transfer_end]
    assert 'ImGui::CloseCurrentPopup();' in transfer
    assert 'openScanResultDialog = true;' in transfer
    result_start = source.index('if (openScanResultDialog) {', transfer_end)
    assert 'ImGui::EndPopup();' in source[transfer_end:result_start]
    assert 'ImGui::OpenPopup(L(Dlg_ScanResults));' in source[result_start:source.index(modal)]
    result_window = source[source.index(modal):source.index('// New playlist dialog', result_start)]
    assert 'Main_ScanFailureSummaryTitle' in result_window
    assert 'state.getScanFailureCounts()' in result_window
    assert 'state.clearScanErrors();' in result_window
    assert 'aura::ui::ActionButton(L(Done)' in result_window
    assert 'Main_StopBtn' not in result_window
    assert 'ImGui::ProgressBar' not in result_window
    assert 'ThemeInspector::RegisterLastItem("info.qaScanResultDone"' in result_window


@pytest.mark.parametrize('scanning,pending,success,total,errors,paused,cancelled,visible', [
    (1, 0, 0, 40, 40, 0, 0, 0),
    (0, 1, 0, 40, 40, 0, 0, 0),
    (0, 0, 0, 40, 40, 0, 0, 1),
    (0, 0, 0, 0, 1, 0, 0, 1),
    (0, 0, 1, 2, 0, 0, 0, 0),
    (0, 0, 0, 0, 0, 1, 0, 1),
    (0, 0, 0, 0, 0, 0, 1, 1),
    (0, 0, 0, 0, 0, 0, 0, 0),
], ids=['running-with-errors', 'finishing-with-errors', 'many-failures',
        'inaccessible-zero-files', 'success', 'paused', 'cancelled', 'acknowledged'])
def test_scan_result_waits_for_completion(
        predicate_exe, scanning, pending, success, total, errors, paused, cancelled, visible):
    args = [0, scanning, pending, success, total, errors, paused, cancelled, visible]
    result = subprocess.run([str(predicate_exe), *map(str, args)],
                            capture_output=True, text=True, timeout=10)
    assert result.returncode == 0, 'Scan result visibility is wrong: ' + result.stdout + result.stderr


def test_success_summary_still_requires_no_failures():
    source = (ROOT / 'src/main.cpp').read_text(encoding='utf-8')
    success_start = source.index('const bool scanCompleteSummaryActive =')
    success_end = source.index('const bool scanProgressSurfaceActive =', success_start)
    success_predicate = source[success_start:success_end]
    assert 'state.scanErrorCount.load() == 0' in success_predicate
