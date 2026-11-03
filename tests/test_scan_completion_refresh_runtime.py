"""Execute the actual two refresh functions with deterministic ledger snapshots.

Only the database and UI clock are stand-ins. Refresh, commit, AppState methods
and completion gates are compiled verbatim from production sources. Generated
probe files stay in build/, without editing or instrumenting the application.
Set HARKEN_SCAN_REFRESH_BASELINE to a Git revision to demonstrate the old bug.
"""
from pathlib import Path
import os
import re
import subprocess
import tempfile

import pytest

ROOT = Path(__file__).resolve().parents[1]


def definition(source, signature):
    start = source.index(signature)
    opening = source.index("{", start)
    # Skip strings/comments when counting braces in the selected definition.
    tokens = re.finditer(r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"|[{}]',
                         source[opening:], re.S)
    depth = 0
    for token in tokens:
        if token.group() == "{":
            depth += 1
        elif token.group() == "}":
            depth -= 1
            if depth == 0:
                return source[start:opening + token.end()]
    raise AssertionError("Unclosed production definition: " + signature)


def source_file(relative):
    baseline = os.environ.get("HARKEN_SCAN_REFRESH_BASELINE")
    if baseline:
        return subprocess.check_output(
            ["git", "show", f"{baseline}:{relative}"], cwd=ROOT, encoding="utf-8")
    return (ROOT / relative).read_text(encoding="utf-8")


@pytest.fixture(scope="module")
def refresh_probe():
    parent = ROOT / "build" / "scan-refresh-tests"
    parent.mkdir(parents=True, exist_ok=True)
    folder = Path(tempfile.mkdtemp(prefix="probe-", dir=parent))
    app = source_file("src/AppState.h")
    members = [definition(app, signature) for signature in (
        "static int64_t nowSteadyMs()", "void markScanCompletionReadyNow()",
        "HarkenSearch::ScanWorkflowSnapshot getScanWorkflowSnapshot() const",
        "void setScanMiniWaveformProgress(int completed, int total)",
        "void resolveScanWorkflowTerminalStage(bool cancelled, bool fatalFailure)",
    )]
    functions = []
    for relative, suffix in (("src/main.cpp", ""), ("src/WindowsMain.cpp", "ForQa")):
        source = source_file(relative)
        for name, result, argument in (
            ("ScanCompletionPendingAny", "bool", "const AppState& state"),
            ("CommitScanCompletionState", "void", "AppState& state"),
            ("RefreshScanCompletionState", "void", "AppState& state"),
        ):
            functions.append(definition(source, f"static {result} {name}{suffix}({argument})"))
    counts = definition(source_file("src/Database.h"), "struct ScanForegroundCompletionCounts")
    (folder / "ledger_counts.inc").write_text(counts + ";\n", encoding="utf-8")
    (folder / "app_members.inc").write_text("\n".join(members), encoding="utf-8")
    (folder / "refresh_functions.inc").write_text("\n".join(functions), encoding="utf-8")
    (folder / "CMakeLists.txt").write_text(
        'cmake_minimum_required(VERSION 3.20)\nproject(ScanRefreshProbe LANGUAGES CXX)\n'
        f'add_executable(ScanRefreshProbe "{ROOT.as_posix()}/tests/scan_completion_refresh_probe.cpp")\n'
        'target_compile_features(ScanRefreshProbe PRIVATE cxx_std_17)\n'
        f'target_include_directories(ScanRefreshProbe PRIVATE "{ROOT.as_posix()}/src" "{folder.as_posix()}")\n',
        encoding="utf-8",
    )
    binary = folder / "bin"
    for command in (["cmake", "-S", str(folder), "-B", str(binary)],
                    ["cmake", "--build", str(binary), "--config", "Release"]):
        result = subprocess.run(command, text=True, capture_output=True, timeout=120)
        assert result.returncode == 0, result.stdout + result.stderr
    name = "ScanRefreshProbe.exe" if os.name == "nt" else "ScanRefreshProbe"
    executable = binary / "Release" / name
    if not executable.exists():
        executable = binary / name
    return executable


@pytest.mark.parametrize("case", [
    "last-mini", "still-pending", "cover-pending", "metadata-pending",
    "progress-without-flag-change", "fast-mode", "pause", "rollback",
])
def test_real_refresh_sequence(refresh_probe, case):
    result = subprocess.run([str(refresh_probe), case], text=True, capture_output=True, timeout=10)
    assert result.returncode == 0, result.stdout + result.stderr
