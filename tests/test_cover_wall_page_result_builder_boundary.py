from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
MAIN = ROOT / "src/main.cpp"
HEADER = ROOT / "src/CoverWallPageResultBuilder.h"
SOURCE = ROOT / "src/CoverWallPageResultBuilder.cpp"


def test_cover_wall_page_mapping_has_independent_owner():
    assert HEADER.exists()
    assert SOURCE.exists()
    source = SOURCE.read_text(encoding="utf-8")
    for contract in (
        "LoadGlobalCoverWallPage",
        "BuildCoverWallPageFromCurrentFileList",
        "BuildCoverWallItemsFromCachedKeys",
        "BuildCoverWallPageLoadResult",
        "BuildCoverWallLoadResultFromEntries",
        "ConvertCoverWallEntryToItem",
        "BuildCoverWallVisualDedupeToken",
        "AppendUniqueCoverWallPageItems",
    ):
        assert contract in source
    assert '"main.cpp"' not in source
    assert '"WindowsMain.h"' not in source
    assert '"platform/' not in source


def test_main_consumes_cover_wall_page_mapping_builder():
    main = MAIN.read_text(encoding="utf-8")
    assert '#include "CoverWallPageResultBuilder.h"' in main
    for old_owner in (
        "static CoverWallPageLoadResult LoadGlobalCoverWallPage(",
        "static CoverModule::CoverItem ConvertCoverWallEntryToItem(",
        "static std::string BuildCoverWallVisualDedupeToken(",
        "static CoverWallPageLoadResult BuildCoverWallPageLoadResult(",
        "static CoverWallPageLoadResult BuildCoverWallLoadResultFromEntries(",
    ):
        assert old_owner not in main
    for symbol in (
        "LoadGlobalCoverWallPage",
        "BuildCoverWallPageFromCurrentFileList",
        "BuildCoverWallPageLoadResult",
        "BuildCoverWallLoadResultFromEntries",
        "AppendUniqueCoverWallPageItems",
    ):
        assert f"using aura::cover_wall::{symbol};" in main

    load_start = main.index("if (needStartLoad && !cw.entriesInFlight && !cw.foldersInFlight)")
    load_scope = main[load_start : load_start + 8000]
    # A nonempty visible list can be only the first 500 rows, or still belong
    # to a previous filter. It cannot declare global discovery complete. The
    # database page loader preserves file-list ordering across all rows.
    assert "BuildCoverWallPageFromCurrentFileList(state.searchResults)" not in load_scope
    registry_fallback = load_scope[
        load_scope.index("} else {") :
    ]
    assert "return LoadGlobalCoverWallPage(dbPath, {}, kGlobalCoverWallPageSize);" in registry_fallback


def test_global_wall_cannot_be_completed_or_reordered_by_filtered_facets():
    main = MAIN.read_text(encoding="utf-8")
    start = main.index("// Global mode reads deduplicated covers from the database, then discovers more.")
    end = main.index("// Refresh the cover-wall load-state text.", start)
    global_mode = main[start:end]
    # Facets may finish before the first DB page, or between later pages.
    # They are count/key ordered and capped, so they cannot replace the global
    # items, clear a live cursor, or declare complete discovery at any DB size.
    assert "BuildCoverWallItemsFromCoverFacets" not in global_mode
    assert "state.currentCoverFacets" not in global_mode
    assert "currentTotal <= 50000" not in global_mode
    assert "CanReuseGlobalCoverWallCache" in global_mode
    assert "ReconstructGlobalCoverWallPageState" in global_mode
    assert "return LoadGlobalCoverWallPage(dbPath, {}, kGlobalCoverWallPageSize);" in global_mode
    assert "cw.globalPageNextCursor = page.nextCursor;" in global_mode
    assert "cw.globalPageHasMore = page.hasMore;" in global_mode
    assert "cw.discoveryDone = !cw.globalPageHasMore;" in global_mode
