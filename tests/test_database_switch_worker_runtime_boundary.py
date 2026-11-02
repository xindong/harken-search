from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
HEADER_PATH = ROOT / "src/DatabaseSwitchWorkerRuntime.h"
SOURCE_PATH = ROOT / "src/DatabaseSwitchWorkerRuntime.cpp"
OPERATION_PATH = ROOT / "src/DatabaseSwitchBlockingOperation.cpp"

assert HEADER_PATH.is_file(), f"missing header: {HEADER_PATH}"
assert SOURCE_PATH.is_file(), f"missing runtime source: {SOURCE_PATH}"
assert OPERATION_PATH.is_file(), f"missing blocking operation source: {OPERATION_PATH}"

HEADER = HEADER_PATH.read_text(encoding="utf-8")
SOURCE = SOURCE_PATH.read_text(encoding="utf-8")
OPERATION = OPERATION_PATH.read_text(encoding="utf-8")
MAIN = (ROOT / "src/main.cpp").read_text(encoding="utf-8")
CMAKE = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
BEHAVIOR = (ROOT / "cmake/harken_search_behavior_tests.cmake").read_text(encoding="utf-8")
RULES = (ROOT / "docs/architecture/stage-4-dependency-rules.json").read_text(
    encoding="utf-8"
)
STAGE6 = (ROOT / "tests/test_stage6_platform_boundary.py").read_text(
    encoding="utf-8"
)
STAGE7 = (ROOT / "tools/stage7_api_contract_audit.py").read_text(
    encoding="utf-8"
)

for required in (
    "namespace aura::database_switch",
    "using DatabaseSwitchOperation = std::function<bool()>;",
    "class DatabaseSwitchWorkerRuntime",
    "DatabaseSwitchWorkerRuntime();",
    "~DatabaseSwitchWorkerRuntime();",
    "DatabaseSwitchWorkerRuntime(const DatabaseSwitchWorkerRuntime&) = delete;",
    "void Start(DatabaseSwitchOperation operation);",
    "bool PollFinished(bool& success);",
    "void Wait();",
):
    assert required in HEADER

for required in (
    "std::thread worker;",
    "std::atomic<bool> done{false};",
    "std::atomic<bool> success{false};",
    "impl_->worker = std::thread(",
    "std::memory_order_release",
    "std::memory_order_acquire",
    "joinable()",
    ".join()",
):
    assert required in SOURCE

for forbidden in (
    "main.cpp",
    "WindowsMain",
    "AppState",
    '#include "Database.h"',
    "g_sameFolderScanner",
    "searchWorker",
    "waveformWorker",
    "ImGui",
    "DIAG_",
    "platform/system_win32",
):
    assert forbidden not in HEADER
    assert forbidden not in SOURCE

for removed in (
    "s_dbOpenThread",
    "s_dbOpenDone",
    "s_dbOpenSuccess",
):
    assert removed not in MAIN

switch_region = MAIN.split("void WaitForDatabaseSwitchThread()", 1)[1].split(
    "bool BeginSwitchDatabase", 1
)[0]

# Windows replacement cannot rename a library while either statistics reader
# still owns a SQLite handle. Cancellation alone does not release that handle.
worker_region = switch_region.split("g_databaseSwitchWorkerRuntime.Start(", 1)[1]
for reader in ("featureStatistics", "idleStatistics"):
    drain = f"{reader}->CancelAndWait()"
    assert drain in worker_region, f"{reader} must release its connection on the switch worker before replacement"
    assert worker_region.index(drain) < worker_region.index("RunBlockingDatabaseSwitchOperation(")
assert "CancelAndWait()" not in switch_region.split("g_databaseSwitchWorkerRuntime.Start(", 1)[0], \
    "waiting for statistics must not block the UI thread"

operation_order = (
    "SwitchDatabaseWorkerStage: stage=stop_batch_features",
    "environment.JoinBatchFeatures()",
    "SwitchDatabaseWorkerStage: stage=stop_same_folder",
    "environment.StopSameFolderScan()",
    "SwitchDatabaseWorkerStage: stage=stop_search",
    "environment.StopSearch()",
    "SwitchDatabaseWorkerStage: stage=stop_waveform",
    "environment.StopWaveform()",
    "SwitchDatabaseWorkerStage: stage=stop_backfill",
    "environment.StopBackfill()",
    "SwitchDatabaseWorkerStage: stage=close_old_db",
    "environment.CloseDatabase()",
    "SwitchDatabaseWorkerStage: stage=open_new_db",
    "environment.OpenDatabase(request.newDbPathUtf8)",
    "environment.SetWaveformCacheMode(",
    "environment.EnqueueStartupReconciliation()",
    "SwitchDatabaseWorkerStage: stage=done",
    "return ok;",
)
positions = [OPERATION.index(token) for token in operation_order]
assert positions == sorted(positions)

for retained in (
    "state.pendingDbPath = effectiveDbPathUtf8;",
    "state.pendingDbOldPath = state.currentDbPathUtf8;",
    "state.dbSwitchInProgress = true;",
    "void WaitForDatabaseSwitchThread()",
    "g_databaseSwitchWorkerRuntime.Wait();",
    "g_databaseSwitchWorkerRuntime.PollFinished(success)",
    "g_databaseSwitchWorkerRuntime.Start(",
    "RunBlockingDatabaseSwitchOperation(",
    "ResetDatabaseState(state);",
    "state.database.open(oldPath, true);",
    "SwitchDatabaseComplete: success=1",
    "SwitchDatabaseComplete: success=0",
):
    assert retained in MAIN

assert '#include "DatabaseSwitchWorkerRuntime.h"' in MAIN
assert MAIN.count("aura::database_switch::DatabaseSwitchWorkerRuntime") == 1
assert CMAKE.count("src/DatabaseSwitchWorkerRuntime.cpp") == 1
assert "NAME TestDatabaseSwitchWorkerRuntime" in BEHAVIOR
assert "NAME TestDatabaseSwitchWorkerRuntimeBoundary" in BEHAVIOR

for governed in (
    '"src/DatabaseSwitchWorkerRuntime.h"',
    '"src/DatabaseSwitchWorkerRuntime.cpp"',
):
    assert governed in RULES
assert "test_database_switch_worker_runtime_owns_thread_publication" in STAGE6
assert (
    '"src/DatabaseSwitchWorkerRuntime.h": '
    '"tests/test_database_switch_worker_runtime_boundary.py"'
    in STAGE7
)
