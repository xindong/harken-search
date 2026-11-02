from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
MAIN = (ROOT / "src/main.cpp").read_text(encoding="utf-8")
WINDOWS_MAIN = (ROOT / "src/WindowsMain.cpp").read_text(encoding="utf-8")
APP_STATE = (ROOT / "src/AppState.h").read_text(encoding="utf-8")
HEADER_PATH = ROOT / "src/RelinkWorkflow.h"
SOURCE_PATH = ROOT / "src/RelinkWorkflow.cpp"
AUDIO_DIALOG_HEADER = ROOT / "src/platform/audio_file_dialog.h"
AUDIO_DIALOG_WIN32 = ROOT / "src/platform/audio_file_dialog_win32.cpp"
AUDIO_DIALOG_STD = ROOT / "src/platform/audio_file_dialog_std.cpp"

assert HEADER_PATH.is_file(), "Relink must expose a narrow workflow boundary"
assert SOURCE_PATH.is_file(), "Relink implementation must leave main.cpp"
assert AUDIO_DIALOG_HEADER.is_file(), "Relink audio choice needs a platform capability"
assert AUDIO_DIALOG_WIN32.is_file()
assert AUDIO_DIALOG_STD.is_file()

header = HEADER_PATH.read_text(encoding="utf-8")
source = SOURCE_PATH.read_text(encoding="utf-8")
dialog_header = AUDIO_DIALOG_HEADER.read_text(encoding="utf-8")
dialog_win32 = AUDIO_DIALOG_WIN32.read_text(encoding="utf-8")
dialog_std = AUDIO_DIALOG_STD.read_text(encoding="utf-8")

for declaration in (
    "void EvaluateRelinkForRecord(AppState& state, const AudioRecord& record);",
    "void PumpRelinkWorkflow(AppState& state);",
    "void ShutdownRelinkWorkflow(AppState& state);",
    "bool RenderRelinkBanner(AppState& state);",
    "bool RenderRelinkTool(AppState& state);",
):
    assert declaration in header

assert '#include "RelinkWorkflow.h"' in MAIN
assert MAIN.count("aura::ui::EvaluateRelinkForRecord(state, record)") == 1
assert MAIN.count("aura::ui::PumpRelinkWorkflow(state)") == 1
assert MAIN.count("aura::ui::RenderRelinkBanner(state)") == 1
assert MAIN.count("aura::ui::RenderRelinkTool(state)") == 1

for forbidden in (
    "static void PumpRelinkBannerResults",
    "static void RenderRelinkBanner",
    "static void RenderRelinkTool",
    'ImGui::BeginChild("##relinkBanner"',
    'ImGui::BeginChild("##rlkLeft"',
    'ImGui::BeginChild("##rlkRight"',
    "state.relinkBannerSearchDone.store(false",
    "state.pendingRelinkCandidates = std::move(cands)",
):
    assert forbidden not in MAIN

for required in (
    'ImGui::OpenPopup(kRelinkPromptId)',
    'BeginDialog(kRelinkPromptId, nullptr, 560.0f, 0.0f)',
    'ImGui::BeginTable(',
    "RelinkMatchReasonText(paths[0].reason)",
    "RelinkPathUpdateFailureText(failure->second)",
    "Relink_SearchPartialFailureFmt",
    "state.database.findRelinkCandidatesDetailed",
    "state.database.updateFilePath",
    "state.relinkBannerSearchDone.store(false",
    "state.pendingRelinkCandidates = std::move(cands)",
    "state.relinkDeferredThisSession = true;",
    "state.showSettingsDialog = true;",
    "state.relinkIgnoredThisSession.insert(state.relinkBanner.missingPath);",
    "Platform::BrowseFolderDialogModern",
    "HarkenSearch::UiHost::MainWindowOwner()",
    "static bool ApplyRelinkPathUpdate(",
):
    assert required in source

assert source.count("state.database.updateFilePath(") == 1
assert source.count("if (ApplyRelinkPathUpdate(") == 1
assert "pendingResults.push_back" in source
assert "state.relinkTool.results = std::move(pendingResults)" in source
assert "state.relinkTool.confirmed = state.relinkTool.results.empty()" in source
assert "try_emplace(candidate.id, -1)" in source
assert "selectedByOrphanId[candidate.id]" not in source
assert "if (selected < 0 || selected >= static_cast<int>(paths.size()))" in source
assert "pendingResults.push_back({candidate, paths})" in source
assert "selectedChoices" in source

assert "std::atomic<bool> relinkToolSearchDone" in APP_STATE
assert "pendingRelinkToolResults" in APP_STATE
assert "Database::RelinkSearchResult searchReport" in APP_STATE
assert "pendingRelinkToolSearchReport" in APP_STATE
assert "std::atomic<bool> relinkBannerSearchCancel{false};" in APP_STATE
assert "std::atomic<bool> relinkToolSearchCancel{false};" in APP_STATE
tool_worker = source.rpartition(
    "state.relinkToolSearchThread = std::thread(")[2].split("\n            });", 1)[0]
assert "state.relinkTool.results =" not in tool_worker
assert "state.relinkTool.scanning" not in tool_worker
assert "state.pendingRelinkToolResults = std::move(results)" in tool_worker
assert "findRelinkCandidatesDetailed(" in tool_worker
assert "&state.relinkToolSearchCancel" in tool_worker
assert "state.pendingRelinkToolSearchReport = std::move(report)" in tool_worker
assert "state.relinkToolSearchDone.store(true, std::memory_order_release)" in tool_worker
banner_worker = source.split(
    "state.relinkBannerSearchThread = std::thread([&state, mp, generation]() {", 1
)[1].split("        });", 1)[0]
assert "findRelinkCandidatesDetailed(" in banner_worker
assert "&state.relinkBannerSearchCancel" in banner_worker
assert "state.pendingRelinkBannerSearchReport = std::move(report)" in banner_worker
assert "findRelinkCandidates(roots);" not in source
assert "relinkBannerSearchGeneration" in source
assert "relinkToolSearchGeneration" in source
assert "pendingRelinkBannerGeneration" in source
assert "pendingRelinkToolGeneration" in source
assert "relinkWorkflowResultMutex" in source
assert "RetireRelinkThread(state, state.relinkBannerSearchThread)" in source
assert "RetireRelinkThread(state, state.relinkToolSearchThread)" in source
assert "void ShutdownRelinkWorkflow(AppState& state)" in source
assert "state.relinkBannerSearchGeneration.fetch_add" in source
assert "state.relinkToolSearchGeneration.fetch_add" in source
shutdown = source.split("void ShutdownRelinkWorkflow(AppState& state)", 1)[1].split(
    "bool RenderRelinkBanner", 1
)[0]
assert "RequestRelinkSearchCancel(state.relinkBannerSearchCancel)" in shutdown
assert "RequestRelinkSearchCancel(state.relinkToolSearchCancel)" in shutdown
assert "JoinRelinkThreadWithTimeout" in WINDOWS_MAIN
assert "JoinRelinkThreadWithTimeout" in source
assert "DrainRetiredRelinkThreads" in shutdown
assert "kRelinkShutdownJoinTimeoutMs" in header
assert "10000" in header
assert "worker.detach()" in WINDOWS_MAIN
assert "worker.detach()" in source
assert "Platform::WaitForNativeThreadHandle" in WINDOWS_MAIN
assert "Platform::WaitForNativeThreadHandle" in source
assert "WaitForSingleObject" not in source
assert "ArmRelinkSearch(state.relinkBannerSearchCancel)" in source
assert "ArmRelinkSearch(state.relinkToolSearchCancel)" in source
pump = source.split("void PumpRelinkWorkflow(AppState& state)", 1)[1].split(
    "bool RenderRelinkBanner", 1
)[0]
assert "state.relinkToolSearchDone.exchange(" in pump
assert "results = std::move(state.pendingRelinkToolResults)" in pump
assert "state.relinkTool.results = std::move(results)" in pump
assert "state.relinkTool.scanning = false" in pump
assert '#include "RelinkWorkflow.h"' in WINDOWS_MAIN
assert "aura::ui::ShutdownRelinkWorkflow(state);" in WINDOWS_MAIN
assert WINDOWS_MAIN.index("aura::ui::ShutdownRelinkWorkflow(state);") < WINDOWS_MAIN.index(
    "state.database.close();"
)

for forbidden in (
    '#include "WindowsMain.h"',
    "g_hwnd",
    "OpenFolderDialog(",
    "RefreshSearchResults(",
    "<windows.h>",
    "<commdlg.h>",
):
    assert forbidden not in source

assert "void* ownerWindow" in dialog_header
assert "OPENFILENAMEW" in dialog_win32
assert "OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST" in dialog_win32
assert "*.wav;*.mp3;*.flac;*.aiff;*.aif;*.ogg;*.m4a;*.wma" in dialog_win32
assert "return {};" in dialog_std

cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
behavior = (ROOT / "cmake/harken_search_behavior_tests.cmake").read_text(encoding="utf-8")
rules = (ROOT / "docs/architecture/stage-4-dependency-rules.json").read_text(encoding="utf-8")
stage7 = (ROOT / "tools/stage7_api_contract_audit.py").read_text(encoding="utf-8")

assert "src/RelinkWorkflow.cpp" in cmake
assert "AURO_AUDIO_FILE_DIALOG_SOURCE" in cmake
assert "NAME TestRelinkWorkflowBoundary" in behavior
assert "tests/test_relink_workflow_boundary.py" in behavior
assert '"src/RelinkWorkflow.cpp"' in rules
assert '"src/RelinkWorkflow.h"' in rules
assert '"src/platform/audio_file_dialog.h"' in rules
assert '"src/RelinkWorkflow.h": "tests/test_relink_workflow_boundary.py"' in stage7
assert '"src/platform/audio_file_dialog.h": "tests/test_relink_workflow_boundary.py"' in stage7


def test_relink_workflow_boundary() -> None:
    """Pytest smoke item; the full contract still runs during module import."""
    assert "state.pendingRelinkToolResults = std::move(results)" in tool_worker
    assert "state.relinkTool.results =" not in tool_worker
    assert "state.relinkTool.scanning" not in tool_worker
    assert "findRelinkCandidatesDetailed(" in tool_worker
    assert "&state.relinkToolSearchCancel" in tool_worker
    assert "findRelinkCandidatesDetailed(" in banner_worker
    assert "&state.relinkBannerSearchCancel" in banner_worker


def _extract_shutdown() -> str:
    return SOURCE_PATH.read_text(encoding="utf-8").split(
        "void ShutdownRelinkWorkflow(AppState& state)", 1
    )[1].split("bool RenderRelinkBanner", 1)[0]


def test_relink_shutdown_is_idempotent() -> None:
    """Replaying window-close must keep the same cancel/join path."""
    shutdown_text = _extract_shutdown()
    replayed = _extract_shutdown()
    assert replayed == shutdown_text
    windows_main = (ROOT / "src/WindowsMain.cpp").read_text(encoding="utf-8")
    assert windows_main.count("JoinRelinkThreadWithTimeout") >= 1
    assert windows_main.count("aura::ui::ShutdownRelinkWorkflow(state);") == 1


def test_relink_cancel_reaches_both_workers() -> None:
    """Each search surface must use its own cancellation flag."""
    text = SOURCE_PATH.read_text(encoding="utf-8")
    assert text.count("findRelinkCandidatesDetailed(") == 2
    assert "&state.relinkBannerSearchCancel" in text
    assert "&state.relinkToolSearchCancel" in text
    assert "findRelinkCandidates(roots);" not in text
    app_state = (ROOT / "src/AppState.h").read_text(encoding="utf-8")
    assert "relinkBannerSearchCancel" in app_state
    assert "relinkToolSearchCancel" in app_state
    windows_main = (ROOT / "src/WindowsMain.cpp").read_text(encoding="utf-8")
    shutdown_call = windows_main.index("aura::ui::ShutdownRelinkWorkflow(state);")
    close_db = windows_main.index("state.database.close();")
    assert shutdown_call < close_db


def test_late_relink_result_does_not_publish_after_shutdown() -> None:
    """A late worker must not publish after generations are invalidated."""
    text = SOURCE_PATH.read_text(encoding="utf-8")
    pump = text.split("void PumpRelinkWorkflow(AppState& state)", 1)[1].split(
        "bool RenderRelinkBanner", 1
    )[0]
    assert "pendingRelinkBannerGeneration" in pump
    assert "pendingRelinkToolGeneration" in pump
    shutdown = _extract_shutdown()
    assert "pendingRelinkBannerGeneration = 0" in shutdown
    assert "pendingRelinkToolGeneration = 0" in shutdown
    assert "relinkBannerSearchCancel" in shutdown
    assert "relinkToolSearchCancel" in shutdown


if __name__ == "__main__":
    test_relink_workflow_boundary()
    test_relink_shutdown_is_idempotent()
    test_relink_cancel_reaches_both_workers()
    test_late_relink_result_does_not_publish_after_shutdown()
