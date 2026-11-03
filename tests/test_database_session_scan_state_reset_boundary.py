from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
HEADER_PATH = ROOT / "src/DatabaseSessionScanStateReset.h"
SOURCE_PATH = ROOT / "src/DatabaseSessionScanStateReset.cpp"

assert HEADER_PATH.is_file(), f"missing header: {HEADER_PATH}"
assert SOURCE_PATH.is_file(), f"missing source: {SOURCE_PATH}"

HEADER = HEADER_PATH.read_text(encoding="utf-8")
SOURCE = SOURCE_PATH.read_text(encoding="utf-8")
MAIN = (ROOT / "src/main.cpp").read_text(encoding="utf-8")
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


assert "struct AppState;" in HEADER
assert "namespace aura::database_session" in HEADER
assert "void ResetDatabaseSessionScanState(AppState& state);" in HEADER
assert '#include "AppState.h"' in SOURCE
assert MAIN.count("aura::database_session::ResetDatabaseSessionScanState(state);") == 1
assert CMAKE.count("src/DatabaseSessionScanStateReset.cpp") == 1
assert BEHAVIOR.count("src/DatabaseSessionScanStateReset.cpp") == 1
assert "NAME TestDatabaseSessionScanStateResetBoundary" in BEHAVIOR

for forbidden in (
    "Database.h",
    "AsyncWorker.h",
    "ScanWorkflow.h",
    "CoverModule.h",
    "glDeleteTextures",
    "Platform::",
    "resetScanReadyToWorkState",
    "main.cpp",
    "WindowsMain",
):
    assert forbidden not in HEADER
    assert forbidden not in SOURCE

reset_body = function_body(MAIN, "void ResetDatabaseState(AppState& state)")
for moved in (
    "state.isScanning = false",
    "state.scanPhase.store(AppState::ScanPhase::Idle)",
    "state.clearScanTiming()",
    "state.clearScanWorkflowState()",
    'state.setScanFile("")',
    "state.clearScanErrors()",
    "state.autoFilterToScannedPath = false",
):
    assert moved not in reset_body, moved

for retained in (
    "aura::database_session::ResetDatabaseSessionMediaCaches(state)",
    "glDeleteTextures",
    "g_miniWaveTextureCache.clear()",
    "StartDeferredSidebarBootstrap(state, true, true)",
):
    assert retained in reset_body, retained

print("PASS: database session scan state reset boundary contracts")

# The switch worker joins old analysis before ResetDatabaseState is called.
# Both user-facing report surfaces must therefore be cleared at this boundary.
for cleared in (
    "state.batchFeatureResults.Clear()",
    "state.batchFeatureResult = {}",
    "state.showSimilarAnalysisReport = false",
    "state.showScanSimilarityDialog = false",
    "state.batchFeatureCompletionNotice.store(false)",
):
    assert cleared in SOURCE, f"old-library analysis report survives switch: {cleared}"
assert "state.batchFeatureResultMutex" in SOURCE
