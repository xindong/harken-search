from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
HEADER = ROOT / "src/SameFolderScanWorkflow.h"
SOURCE = ROOT / "src/SameFolderScanWorkflow.cpp"


def test_same_folder_scan_workflow_remains_isolated_from_product_root() -> None:
    assert HEADER.is_file(), f"missing header: {HEADER}"
    assert SOURCE.is_file(), f"missing source: {SOURCE}"
    header = HEADER.read_text(encoding="utf-8")
    source = SOURCE.read_text(encoding="utf-8")
    main = (ROOT / "src/main.cpp").read_text(encoding="utf-8")
    for token in ("class SameFolderScanWorkflow", "start(", "drain("):
        assert token in header
    for forbidden in (
        "AppState", "Database", "AudioRecord", "ImGui", "main.cpp",
        "WindowsMain", "platform/",
    ):
        assert forbidden not in header
        assert forbidden not in source
    assert "static void StartShowSameFolderScan" not in main
    assert "static void PumpShowSameFolderScan" not in main
    assert '#include "SameFolderScanWorkflow.h"' not in main
    assert "aura::same_folder::SameFolderScanWorkflow" not in main
    assert "g_sameFolderWorkflow" not in main


def test_same_folder_menu_has_no_unreachable_async_product_pipeline() -> None:
    main = (ROOT / "src/main.cpp").read_text(encoding="utf-8")

    for removed in (
        '#include "SameFolderScanDrainStateAdapter.h"',
        '#include "SameFolderScanWorkflow.h"',
        "g_sameFolderWorkflow",
        "class AppNullSameFolderScanEffects",
        "static void StartSameFolderScan(",
        "static void PumpSameFolderScan(",
        "SameFolderScanDrainStateAdapter drainAdapter",
    ):
        assert removed not in main

    menu = main.split(
        "if (!isMulti && ImGui::MenuItem(L(Ctx_ShowSameFolder)))", 1
    )[1].split("ImGui::Separator();", 1)[0]
    for preserved in (
        "std::string targetFolder = record.folderPath;",
        "state.folderFilterActive = true;",
        "state.selectedFolderPath = HarkenSearch::CanonicalFolderPath(targetFolder);",
        "state.viewMode = ViewMode::Database;",
        "RefreshSearchResults(state, true);",
        "state.syncToTab();",
        "rowContextInvalidatedResults = true;",
    ):
        assert preserved in menu
    for reset in (
        "ClearSpecificCoverKeyFilter(state);", "ClearAllFiltersForShowAll(state);",
        "state.coverFilterActive = false;", r"state.searchBuffer[0] = '\0';",
        "state.selectedTags.clear();",
    ):
        assert reset not in menu

    assert "aura::same_folder::SameFolderScanRuntime g_sameFolderScanner" in main
    assert "g_sameFolderScanner," in main
