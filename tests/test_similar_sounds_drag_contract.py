"""Keep Similar Sounds drag-out on the existing, processing-aware transfer path."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def test_result_row_drags_its_own_path_without_changing_the_reference():
    window = (ROOT / "src/SimilarSoundsWindow.cpp").read_text(encoding="utf-8")
    row = window[window.index("const int previousSelectedRow"):]
    row = row[:row.index("ImGui::TableSetColumnIndex(1);")]
    assert "StartAudioFileDrag(state, row.filePath)" in row, "result rows lack native file drag-out"
    assert "ImGui::IsItemActive()" in row
    assert "ImGui::IsMouseDragging(ImGuiMouseButton_Left, 10.0f)" in row
    assert '"similar.result.file."' in row
    assert "beginAudioTargetFromSearchResult" not in row
    assert "selectedResultIndex =" not in row
    assert "similarSoundsLocalReference =" not in row
    assert 'ImGui::InvisibleButton("##SimilarWaveHit"' in window


def test_drag_uses_existing_processing_and_error_handling():
    main = (ROOT / "src/main.cpp").read_text(encoding="utf-8")
    header = (ROOT / "src/UIRenderer.h").read_text(encoding="utf-8")
    signature = "void StartAudioFileDrag(AppState& state, const std::string& filePath)"
    assert signature in header and signature in main, "missing renderer transfer entry point"
    helper = main[main.index(signature):]
    helper = helper[:helper.index("\nstatic std::vector<std::string> BuildBrowserTransferSourcePaths")]
    assert "BuildTransferPayloadForPaths(" in helper
    assert "{filePath}" in helper
    assert "state.uiState.regionFadeInNorm" in helper
    assert "state.uiState.regionFadeOutNorm" in helper
    assert "state.settings.keepHarkenSearchOutputProcessingSuffix" in helper
    assert "StartFileDrag(files)" in helper
    assert helper.index("BuildTransferPayloadForPaths(") < helper.index("StartFileDrag(files)")
    assert "ImGui::ClearActiveID()" in helper
    assert "state.loadErrorToast" in helper and "L(Misc_DragFailed)" in helper
    assert "state.getSelectedRecords()" not in helper
    assert "state.currentFileId" not in helper
