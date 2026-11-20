import json
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
HEADER = ROOT / "src" / "HarkenSearchWidgets.h"
SOURCE = ROOT / "src" / "HarkenSearchWidgets.cpp"
RELINK = ROOT / "src" / "RelinkWorkflow.cpp"
COMPONENTS = ROOT / "design-system" / "components" / "aris.components.json"


def test_workflow_feedback_controls_have_reusable_native_apis():
    header = HEADER.read_text(encoding="utf-8")
    source = SOURCE.read_text(encoding="utf-8")

    assert "enum class ScanJobStatus" in header
    assert "struct ScanJobCardOptions" in header
    assert "void ScanJobCard(" in header
    assert "enum class RecoveryBannerIntent" in header
    assert "struct RecoveryBannerOptions" in header
    assert "void RecoveryBanner(" in header

    assert "void ScanJobCard(" in source
    assert "ImGui::ProgressBar" in source
    assert "void RecoveryBanner(" in source
    assert "meterWarning" in source
    assert "volumeClipColor" in source


def test_relink_recovery_flow_uses_the_shared_dialog_and_explicit_actions():
    relink = RELINK.read_text(encoding="utf-8")

    assert '#include "HarkenSearchWidgets.h"' in relink
    assert "aura::ui::BeginAlertDialog(kRelinkPromptId" in relink
    assert "L(Relink_MissingFileHeader)" in relink
    assert "Button(L(Relink_RemindLaterBtn)" in relink
    assert "Button(L(Relink_OpenSettingsBtn)" in relink
    assert "Button(L(Relink_SkipThisBtn)" in relink


def test_component_contract_marks_feedback_controls_implemented():
    payload = json.loads(COMPONENTS.read_text(encoding="utf-8"))
    by_id = {component["id"]: component for component in payload["components"]}

    assert by_id["scan-job-card"]["status"] == "implemented"
    assert by_id["scan-job-card"]["nativeApi"] == "aura::ui::ScanJobCard"
    assert by_id["recovery-banner"]["status"] == "implemented"
    assert by_id["recovery-banner"]["nativeApi"] == "aura::ui::RecoveryBanner"
