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
    start = source.index('ReadingTextScope readingResult;')
    block = source[start:source.index('const bool rollbackFailed', start)]
    snapshot = re.search(r'const auto scanErrorSnapshot = state.getScanErrors\(\);', block)
    any_expr = re.search(r'const bool networkUnreachable = std::any_of\([\s\S]*?\}\);', block).group()
    count_expr = "const int unreachableHint = HarkenSearch::BuildScanFailureSummary(scanErrorSnapshot, int(scanErrorSnapshot.size()))[0];"
    setup = sorted(glob.glob('C:/Program Files/Microsoft Visual Studio/*/*/VC/Auxiliary/Build/vcvars64.bat'))
    assert setup, 'VS C++ toolchain required for checked-iterator regression'
    folder = tmp_path_factory.mktemp('scan-error-snapshot')
    cpp = folder / 'predicate.cpp'
    cpp.write_text('''#include <algorithm>
#include <vector>
#include <string>
#include <cstdlib>
#include <crtdbg.h>
#define NOMINMAX
#include <windows.h>
#include "ScanDialogPresentation.h"
struct State {
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
  return 0;
}
''', encoding='utf-8')
    script = folder / 'compile.cmd'
    script.write_text(f'@echo off\ncall "{setup[-1]}" >nul\nif errorlevel 1 exit /b 1\ncl /nologo /std:c++17 /EHsc /MDd /Od /D_ITERATOR_DEBUG_LEVEL=2 /I "{ROOT / "src"}" "{cpp}" /Fe:predicate.exe\n', encoding='utf-8')
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
