import json
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
MAIN = (ROOT / "src/main.cpp").read_text(encoding="utf-8")
WINDOWS_MAIN = (ROOT / "src/WindowsMain.cpp").read_text(encoding="utf-8")
FEATURE_TEST = (ROOT / "tests/test_feature_extractor.cpp").read_text(encoding="utf-8")
APP_STATE = (ROOT / "src/AppState.h").read_text(encoding="utf-8")
CMAKE = (ROOT / "cmake/harken_search_behavior_tests.cmake").read_text(encoding="utf-8")
LEDGER_IDS = {
    entry["id"]
    for entry in json.loads((ROOT / "locale/harken.ui.strings.json").read_text(encoding="utf-8"))["entries"]
}


def _scan_setup_dialog() -> str:
    start = MAIN.index("// Scan folder dialog")
    end = MAIN.index("auto startScanForFolders", start)
    return MAIN[start:end]


def test_scan_dialog_exposes_post_scan_similarity_index_option() -> None:
    assert "TestScanDialogSimilarIndexOptionContract" in CMAKE
    dialog = _scan_setup_dialog()
    assert "Main_BuildSimilarIndexCheck" in LEDGER_IDS
    assert "L(Main_BuildSimilarIndexCheck)" in dialog
    assert "扫描完成后立即建立相似数据库索引" not in dialog
    assert "&state.scanBuildSimilarityIndexAfterScan" in dialog
    assert "bool scanBuildSimilarityIndexAfterScan = false;" in APP_STATE


def test_scan_start_captures_similarity_index_option_for_completion() -> None:
    start = MAIN.index("auto startScanForFolders")
    end = MAIN.index("state.isScanning = false;", start)
    scan_start = MAIN[start:end]
    assert "const bool buildSimilarityIndexAfterScan = state.scanBuildSimilarityIndexAfterScan;" in scan_start
    assert "ShouldStartPostScanSimilarityIndex" in MAIN
    assert "pendingPostScanSimilarityIndex" in APP_STATE


def test_begin_folder_scan_uses_same_post_scan_similarity_index_option() -> None:
    start = WINDOWS_MAIN.index("static bool BeginFolderScan(")
    end = WINDOWS_MAIN.index("state.isScanning = false;", start)
    begin = WINDOWS_MAIN[start:end]
    assert "const bool buildSimilarityIndexAfterScan = state.scanBuildSimilarityIndexAfterScan;" in begin
    assert "ShouldStartPostScanSimilarityIndex" in begin
    assert "pendingPostScanSimilarityIndex" in begin
    assert 'getBool(raw, "build_similarity_index"' in WINDOWS_MAIN


def test_batch_extraction_contract_covers_shared_start_helper() -> None:
    start = FEATURE_TEST.index("void TestBatchExtractionUsesIndependentCancellationSignal()")
    end = FEATURE_TEST.index("void Test", start + 1) if "void Test" in FEATURE_TEST[start + 1:] else len(FEATURE_TEST)
    body = FEATURE_TEST[start:start + 2500]
    assert "StartBatchFeatureExtraction" in body
    assert "s_was_extracting" not in body or "StartBatchFeatureExtraction" in FEATURE_TEST


def test_post_scan_similarity_index_waits_instead_of_dropping_busy_or_switch() -> None:
    render_ui = MAIN.index("void RenderUI(AppState& state)")
    consume = MAIN[
        render_ui : MAIN.index("ReleaseInScopeDragPauseLatchIfIdle()", render_ui)
    ]
    assert "ShouldConsumePendingPostScanSimilarityIndex" in consume
    assert "pendingPostScanSimilarityIndex.store(false, std::memory_order_release)" in consume
    switch = MAIN[
        MAIN.index("static bool SwitchDatabase(") : MAIN.index(
            "state.backfillQueue.onDbClosing(state.backfillDbGeneration);"
        )
    ]
    assert "pendingPostScanSimilarityIndex.store(false, std::memory_order_release)" in switch
    start_hook = MAIN.index("if (HarkenSearch::ShouldStartPostScanSimilarityIndex(")
    assert "batchFeatureExtracting.load(std::memory_order_acquire)" not in MAIN[
        start_hook : MAIN.index("state.pendingPostScanSimilarityIndex.store", start_hook)
    ]


def test_post_scan_similarity_has_its_own_progress_and_result_surfaces() -> None:
    start = MAIN.index("void StartBatchFeatureExtraction(AppState& state)")
    helper = MAIN[start : MAIN.index("void RenderUI(AppState& state)")]
    assert "OpenSimilarSoundsWindow" not in helper
    assert "Scan_SimIndexRunningBg" in LEDGER_IDS
    assert "L(Scan_SimIndexRunningBg)" in MAIN
    dialog = MAIN[
        MAIN.index("// Scan folder dialog") : MAIN.index("auto startScanForFolders")
    ]
    assert "正在建立相似数据库索引" not in dialog
    assert "相似音效索引后台自动运行中" not in dialog
    assert "L(Main_SimIndexBuildingFmt)" not in dialog
    assert "##ScanSimilarityProgress" in helper
    assert "##ScanSimilarityResult" in helper
    assert "L(ScanUi_ImportComplete)" in helper
    assert "result.succeeded" in helper
    assert "L(ScanUi_ViewResult)" in MAIN
    consume = MAIN[
        MAIN.index("void RenderUI(AppState& state)") : MAIN.index(
            "ReleaseInScopeDragPauseLatchIfIdle()",
            MAIN.index("void RenderUI(AppState& state)"),
        )
    ]
    assert "showPostScanSimilarityIndexNotice = true" in consume
    assert "scanSimilarityOwned = true" in consume
    assert "showScanSimilarityDialog = true" in consume
    assert "bool showPostScanSimilarityIndexNotice = false;" in APP_STATE
    render_ui = MAIN[MAIN.index("void RenderUI(AppState& state)") :]
    assert "L(Scan_SimIndexRunningBg)" in render_ui
    notice_uses = [line.strip() for line in MAIN.splitlines() if "showPostScanSimilarityIndexNotice" in line]
    display_uses = [
        line
        for line in notice_uses
        if "true" not in line and "false" not in line
    ]
    assert display_uses
    assert all("batchFeatureExtracting" not in line for line in display_uses)
