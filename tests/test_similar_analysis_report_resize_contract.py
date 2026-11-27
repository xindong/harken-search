"""Contract for the Similar Sounds analysis report dialog.

The card defect: the dialog could not be resized, never remembered a size, and
squeezed 44 failures into a narrow scrolling strip. These assertions pin the
three answers in the source, so a later refactor cannot quietly drop one.
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
window = (ROOT / "src" / "SimilarSoundsWindow.cpp").read_text(encoding="utf-8")
app_state = (ROOT / "src" / "AppState.h").read_text(encoding="utf-8")
main = (ROOT / "src" / "main.cpp").read_text(encoding="utf-8")
layout = (ROOT / "src" / "SimilarAnalysisReportLayout.h").read_text(encoding="utf-8")
qa_host = (ROOT / "src" / "WindowsMain.cpp").read_text(encoding="utf-8")
cmake = (ROOT / "cmake" / "harken_search_behavior_tests.cmake").read_text(encoding="utf-8")

start = window.index("static void DrawSimilarAnalysisReport")
end = window.index("static void DrawSimilarSoundsBody", start)
dialog = window[start:end]

# The dialog owns its geometry through one header, not inline magic numbers.
assert '#include "SimilarAnalysisReportLayout.h"' in window
assert "namespace HarkenSearch::similar_analysis_report" in layout
for constant in (
    "kDefaultWidth = 1180.0f",
    "kDefaultHeight = 780.0f",
    "kMinimumWidth",
    "kMinimumHeight",
    "kWorkAreaMargin = 8.0f",
    "kFooterStripHeight = 42.0f",
):
    assert constant in layout, constant

# Size: read the remembered size, clamp it to the work area, apply it on the
# frame the dialog opens, and allow a real edge drag afterwards.
assert "state.settings.similarAnalysisReportWidth" in dialog
assert "state.settings.similarAnalysisReportHeight" in dialog
assert "report_layout::ClampSize(" in dialog
assert "ImGui::SetNextWindowSize(" in dialog
assert "ImGuiCond_Always : ImGuiCond_FirstUseEver" in dialog, (
    "the remembered size must be applied when the dialog opens and left alone "
    "afterwards, so an edge drag is not overwritten every frame")
assert "ImGui::SetNextWindowSizeConstraints(" in dialog
# BeginAlertDialog defaults to ImGuiWindowFlags_AlwaysAutoResize, which fits the
# modal to its content and makes edge resizing impossible. This dialog must opt
# out explicitly or the whole fix is void.
assert "aura::ui::BeginAlertDialog(L(Similar_AnalysisReport), L(Similar_AnalysisReport),\n" in dialog
assert "ImGuiWindowFlags_None)) {" in dialog
assert "ImGuiWindowFlags_NoResize" not in dialog

# Position: an appearing dialog is clamped into the work area with the margin,
# while a visible one keeps the position the caption drag gave it.
assert "ImGui::IsWindowAppearing()" in dialog
assert "report_layout::ClampToWorkArea(" in dialog
assert "ImGui::SetWindowPos(ImVec2(clamped.x, clamped.y))" in dialog
assert "ImGui::SetWindowPos(" in dialog[dialog.index("ImGui::IsWindowAppearing()"):]

# Persistence: the settled size is written back and saved with the settings.
assert "state.settings.similarAnalysisReportWidth = settled.x" in dialog
assert "state.settings.similarAnalysisReportHeight = settled.y" in dialog
assert "state.settingsDirty = true" in dialog
assert "float similarAnalysisReportWidth = 1180.0f;" in app_state
assert "float similarAnalysisReportHeight = 780.0f;" in app_state
assert 'ParseFloat(kv["similarAnalysisReportWidth"]' in main
assert 'ParseFloat(kv["similarAnalysisReportHeight"]' in main
assert '{"similarAnalysisReportWidth",' in main
assert '{"similarAnalysisReportHeight",' in main

# Failure list: one row per failure, one text line tall, and the old
# reason-line plus path-line pair must not come back.
assert "report_layout::FailureRowHeight(" in dialog
assert "ImGuiListClipper clipper;" in dialog
assert "clipper.Begin(static_cast<int>(report.issues.size()), rowHeight)" in dialog
assert "ImGui::GetTextLineHeightWithSpacing() * 2" not in dialog
assert "ImVec2(0, -report_layout::kFooterStripHeight)" in dialog
rows = dialog[dialog.index("clipper.Begin("):dialog.index("ImGui::EndChild()")]
assert rows.count("ImGui::Text(") == 1, "one plain text item per failure row"
assert rows.count("ImGui::SameLine()") == 1, "the path trails the reason on the same line"
assert "ImGui::TextUnformatted(issue.file.path.c_str())" in rows
assert "ImGui::SetTooltip(\"%s\", issue.file.path.c_str())" in rows
assert "ImGui::TextWrapped(issue.file.path.c_str())" not in dialog

# The list keeps a readable height at the default size instead of a strip.
assert "FailureListHeight" in layout
assert "FailureRowHeight" in layout

# QA receipt: the dialog can be opened with a 44-failure fixture and the export
# reports the geometry and resize state, so the size claim is measurable.
assert '"similar-analysis-report"' in qa_host
assert "for (int index = 0; index < 44; ++index)" in qa_host
assert 'action == "geometry" || action == "resize" || action == "resize_drag"' in qa_host
assert '\\"resizable\\":%s' in qa_host
assert '\\"viewport_owned\\":%s' in qa_host
assert "failure_list_height" in qa_host
assert "s_qaDialogResizeDrag" in qa_host
assert "io.AddMouseButtonEvent" in qa_host

# Both tests are wired into the build and the suite.
assert "tests/test_similar_analysis_report_layout.cpp" in cmake
assert "TestSimilarAnalysisReportLayout" in cmake
assert "add_test(NAME TestSimilarAnalysisReportLayout" in cmake
assert "tests/test_similar_analysis_report_resize_contract.py" in cmake
assert "NAME TestSimilarAnalysisReportResizeContract" in cmake

print("similar analysis report resize contract: ok")
