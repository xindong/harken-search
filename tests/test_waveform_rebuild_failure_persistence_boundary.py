from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
HEADER_PATH = ROOT / "src/WaveformRebuildFailurePersistence.h"
SOURCE_PATH = ROOT / "src/WaveformRebuildFailurePersistence.cpp"

assert HEADER_PATH.is_file(), f"missing header: {HEADER_PATH}"
assert SOURCE_PATH.is_file(), f"missing source: {SOURCE_PATH}"

HEADER = HEADER_PATH.read_text(encoding="utf-8")
SOURCE = SOURCE_PATH.read_text(encoding="utf-8")
WORKFLOW = (ROOT / "src/WaveformRebuildRecordWorkflow.cpp").read_text(
    encoding="utf-8"
)
MAIN = (ROOT / "src/main.cpp").read_text(encoding="utf-8")
WINDOWS_MAIN = (ROOT / "src/WindowsMain.cpp").read_text(encoding="utf-8")
WORKER = (ROOT / "src/AsyncWorker.cpp").read_text(encoding="utf-8")
CMAKE = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
BEHAVIOR = (ROOT / "cmake/harken_search_behavior_tests.cmake").read_text(encoding="utf-8")


def function_body(source: str, signature: str) -> str:
    start = source.index(signature)
    brace = source.index("{", start)
    depth = 0
    for index in range(brace, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[brace + 1 : index]
    raise AssertionError(f"unterminated function: {signature}")


for token in (
    "struct WaveformRebuildFailureRequest",
    "class WaveformRebuildFailureSink",
    "class DatabaseWaveformRebuildFailureSink final",
    "bool PersistPreviousWaveformRebuildFailure(",
):
    assert token in HEADER, token

for owned in (
    "request.fullMode",
    "request.previouslyFailed",
    '"foreground_mini_rebuild_previous_failure"',
    '"foreground_waveform_rebuild_previous_failure"',
    "sink.persistPreviousFailure(request.record, reason)",
    "WaveformExtractor::BuildCacheKey(",
    "database_.upsertWaveformCacheStateByPath(",
):
    assert owned in SOURCE, owned

pump = function_body(MAIN, "static void PumpWaveformRebuild(AppState& state)")
assert MAIN.count("PersistPreviousWaveformRebuildFailure(") == 0
assert WORKFLOW.count("PersistPreviousWaveformRebuildFailure(") == 1
assert "DatabaseWaveformRebuildFailureSink failureSink(" in pump
assert "state.database);" in pump
assert "++result.state.failed;" in WORKFLOW
for removed in (
    "WaveformExtractor::BuildCacheKey(",
    "state.database.upsertWaveformCacheStateByPath(",
    '"foreground_mini_rebuild_previous_failure"',
    '"foreground_waveform_rebuild_previous_failure"',
):
    assert removed not in pump, removed

assert CMAKE.count("src/WaveformRebuildFailurePersistence.cpp") == 1
assert BEHAVIOR.count("src/WaveformRebuildFailurePersistence.cpp") == 2
assert "TestWaveformRebuildFailurePersistence" in BEHAVIOR
assert "NAME TestWaveformRebuildFailurePersistenceBoundary" in BEHAVIOR

for forbidden in (
    "AppState.h",
    "AsyncWorker.h",
    "DiagnosticsLogger",
    "ModuleUI.h",
    "Platform::",
    "glDelete",
    "main.cpp",
    "WindowsMain",
):
    assert forbidden not in HEADER, forbidden
    assert forbidden not in SOURCE, forbidden

print("PASS: waveform rebuild failure persistence boundary contracts")

results_surface = function_body(MAIN, "auto renderResultsSurface =")
windows_consume = function_body(WINDOWS_MAIN, "for (auto& result : completedResults)")
for consumer, result, expected in (
    (results_surface, "res", 2),
    (windows_consume, "result", 1),
):
    assert "state.database.upsertWaveformCacheStateByPath(" not in consumer, (
        f"UI consumer ({result}) still writes waveform failure state synchronously"
    )
    assert consumer.count(f"state.waveformWorker.enqueueFailurePersistence({result})") == expected
    assert "state.backfillQueue.reportOutcome(" in consumer
    assert "state.database.clearBackfillPending(" in consumer

enqueue = function_body(WORKER, "void WaveformWorker::enqueueFailurePersistence(")
for skipped in (
    "result.success",
    "result.metersOnlyMerge",
    '"mini_cache_miss_no_decode"',
    '"cancelled"',
    '"deferred_scan_db_write"',
    '"decode_failed"',
):
    assert skipped in enqueue, skipped
assert "writeFailure(" not in enqueue
assert "result.failureObservedAt" in enqueue and "std::time(" not in enqueue, (
    "a delayed UI result must carry the time the worker failed, not the time the UI enqueues it"
)
producer = function_body(WORKER, "void WaveformWorker::workerThread()")
assert "result.failureObservedAt = static_cast<int64_t>(std::time(nullptr))" in producer
assert "markFailure(mini.errorCode.empty()" in producer
assert 'markFailure("mini_waveform_empty")' in producer
assert "markFailure(detail.errorCode.empty()" in producer
assert 'markFailure("detail_save_db_failed")' in producer
assert "result.failureBinding != m_failureBinding.load(" in enqueue, (
    "pre-mount and previous-library results cannot persist into the currently bound library"
)
assert "result.generation != m_generation.load(" in enqueue

start = function_body(WORKER, "void WaveformWorker::start(")
running_start = function_body(start, "if (m_running.load())")
assert "bindFailureWriter(" in running_start and "stop()" not in running_start, (
    "interactive mount must bind persistence without restarting existing decode threads"
)
assert "bindFailureWriter(" in start.split("m_cancelRequested.store(false", 1)[1]
binding = function_body(WORKER, "void WaveformWorker::bindFailureWriter(")
assert binding.index("paths.main.empty()") < binding.index("m_failureBinding.fetch_add(")
assert binding.index("m_acceptFailureWrites ||") < binding.index("m_failureBinding.fetch_add(")
submit = function_body(WORKER, "bool WaveformWorker::submitRequestInternal(")
assert "req.failureBinding = m_failureBinding.load(" in submit
assert "result.failureBinding = req.failureBinding" in producer
mount = function_body(MAIN, "static bool MountFilePlayerDatabaseForInteractiveRefresh(")
assert "state.waveformWorker.start(&state.database," in mount

stop = function_body(WORKER, "void WaveformWorker::stop(")
assert stop.index("m_cancelRequested.store(true") < stop.index("m_failureThread.join()")
assert stop.index("m_pendingFailureWrites.clear()") < stop.index("m_failureThread.join()")

writer = function_body(WORKER, "void WaveformWorker::failureWriterThread(")
assert "Database::WaveformFailureWriter" in writer
assert "writeFailure(" in writer
assert "m_db" not in writer, "failure persistence must not use the UI-owned connection"

print("PASS: waveform result failure writes leave the UI consumer")
