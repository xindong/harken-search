from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
HEADER = ROOT / "src/CueviFeedbackCompletionWorkflow.h"
SOURCE = ROOT / "src/CueviFeedbackCompletionWorkflow.cpp"
MAIN = ROOT / "src/main.cpp"
CMAKE = ROOT / "CMakeLists.txt"
BEHAVIOR_CMAKE = ROOT / "cmake/harken_search_behavior_tests.cmake"
RULES = ROOT / "docs/architecture/stage-4-dependency-rules.json"
STAGE7_AUDIT = ROOT / "tools/stage7_api_contract_audit.py"


def test_completion_workflow_owns_feedback_result_transition() -> None:
    assert HEADER.exists()
    assert SOURCE.exists()
    header = HEADER.read_text(encoding="utf-8")
    source = SOURCE.read_text(encoding="utf-8")
    assert "struct CueviFeedbackCompletionState" in header
    assert "ApplyCueviFeedbackCompletion(" in header
    assert "PollCueviFeedbackCompletion(" in header
    assert "state.submitting = false;" in source
    assert "state.submitOk = result.ok;" in source
    assert "state.workItemId = result.workItemId;" in source


def test_completion_workflow_has_one_way_dependencies() -> None:
    assert HEADER.exists()
    assert SOURCE.exists()
    combined = "\n".join(
        path.read_text(encoding="utf-8") for path in (HEADER, SOURCE)
    )
    for forbidden in ("main.cpp", "WindowsMain", "src/platform/", "AppState.h", "imgui"):
        assert forbidden not in combined


def test_root_only_composes_completion_workflow() -> None:
    source = MAIN.read_text(encoding="utf-8")
    assert '#include "CueviFeedbackCompletionWorkflow.h"' in source
    assert "static void PollCueviFeedbackSubmission" not in source
    assert "aura::feedback::PollCueviFeedbackCompletion(" in source
    assert source.count("feedbackSubmitTask.Poll()") == 0


def test_completion_workflow_is_built_tested_and_governed() -> None:
    cmake = CMAKE.read_text(encoding="utf-8")
    behavior = BEHAVIOR_CMAKE.read_text(encoding="utf-8")
    rules = RULES.read_text(encoding="utf-8")
    stage7 = STAGE7_AUDIT.read_text(encoding="utf-8")
    assert "src/CueviFeedbackCompletionWorkflow.cpp" in cmake
    assert "TestCueviFeedbackCompletionWorkflow" in behavior
    assert "TestCueviFeedbackCompletionWorkflowBoundary" in behavior
    assert "src/CueviFeedbackCompletionWorkflow.cpp" in rules
    assert "src/CueviFeedbackCompletionWorkflow.h" in rules
    assert (
        '"src/CueviFeedbackCompletionWorkflow.h": '
        '"tests/test_feedback_transport_boundary.py"'
    ) in stage7
    assert (
        '"src/CueviFeedbackClient.h": "tests/test_stage6_platform_boundary.py"'
    ) in stage7
