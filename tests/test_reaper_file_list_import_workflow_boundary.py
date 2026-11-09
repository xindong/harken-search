from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
MAIN = ROOT / "src/main.cpp"
HEADER = ROOT / "src/ReaperFileListImportWorkflow.h"
SOURCE = ROOT / "src/ReaperFileListImportWorkflow.cpp"
CMODEL = ROOT / "CMakeLists.txt"
BEHAVIOR = ROOT / "cmake/harken_search_behavior_tests.cmake"


def test_reaper_import_start_owns_worker_boundary():
    main = MAIN.read_text(encoding="utf-8")
    header = HEADER.read_text(encoding="utf-8")
    source = SOURCE.read_text(encoding="utf-8")
    cmake = CMODEL.read_text(encoding="utf-8")
    behavior = BEHAVIOR.read_text(encoding="utf-8")

    assert "static void StartReaperFileListImport" not in main
    assert "reaper_import::Start" in main
    assert "void Start(AppState&" in header
    assert "void Start(AppState&" in source
    assert "ReaperFileListImportWorkflow.cpp" in cmake
    assert "ReaperFileListImportWorkflow.cpp" in behavior


def test_reaper_import_workflow_has_no_ui_or_platform_entrypoint_dependency():
    source = SOURCE.read_text(encoding="utf-8")
    assert '"main.cpp"' not in source
    assert '"WindowsMain.h"' not in source
    assert "ImGui::" not in source


def test_reaper_import_reports_its_own_completion_instead_of_resetting_view():
    source = SOURCE.read_text(encoding="utf-8")
    assert "state.beginScanWorkflow(false);" in source
    assert "state.setScanOutcome(" in source
    assert "state.resolveScanWorkflowTerminalStage(" in source
    assert "state.needsDatabaseRefresh = !importSucceeded;" in source
