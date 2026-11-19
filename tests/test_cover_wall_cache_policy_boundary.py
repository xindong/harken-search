from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
MAIN = ROOT / "src/main.cpp"
HEADER = ROOT / "src/CoverWallCachePolicy.h"
SOURCE = ROOT / "src/CoverWallCachePolicy.cpp"


def test_cover_wall_cache_policy_has_independent_owner():
    assert HEADER.exists()
    assert SOURCE.exists()
    source = SOURCE.read_text(encoding="utf-8")
    for contract in (
        "CanReuseGlobalCoverWallCache",
        "StoreGlobalCoverWallCache",
        "LastCoverWallPageCursor",
        "ReconstructGlobalCoverWallPageState",
    ):
        assert contract in source
    assert '"main.cpp"' not in source
    assert '"WindowsMain.h"' not in source
    assert '"platform/' not in source


def test_main_consumes_cover_wall_cache_policy():
    main = MAIN.read_text(encoding="utf-8")
    assert '#include "CoverWallCachePolicy.h"' in main
    for old_owner in (
        "static bool CanReuseGlobalCoverWallCache(",
        "static void StoreGlobalCoverWallCache(",
        "static std::string LastCoverWallPageCursor(",
        "static void ReconstructGlobalCoverWallPageState(",
    ):
        assert old_owner not in main
    for symbol in (
        "CanReuseGlobalCoverWallCache",
        "StoreGlobalCoverWallCache",
        "LastCoverWallPageCursor",
        "ReconstructGlobalCoverWallPageState",
    ):
        assert f"using aura::cover_wall::{symbol};" in main


def test_return_from_facets_restarts_incomplete_global_pagination():
    main = MAIN.read_text(encoding="utf-8")
    start = main.index("// Global mode reads deduplicated covers from the database, then discovers more.")
    end = main.index("bool needStartLoad = false;", start)
    global_entry = main[start:end]
    assert "if (wasUsingFacetCoverWall && !canReuseGlobalCache) {" in global_entry
    assert "cw.prepareGlobalReloadAfterFacet(mainDbPath);" in global_entry
    assert global_entry.index("cw.prepareGlobalReloadAfterFacet(mainDbPath);") < global_entry.index("if (canReuseGlobalCache)")


def test_stale_preload_does_not_clear_live_pagination():
    main = MAIN.read_text(encoding="utf-8")
    start = main.index("CoverWallPageLoadResult preloaded = cw.preloadFuture.get();")
    end = main.index("// 不预热整页", start)
    retired, _, publish = main[start:end].partition("if (!stalePreload) {")
    assert "cw.preloadGeneration != cw.generation" in retired
    assert "cw.preloadInFlight = false;" in retired
    assert "cw.globalPageNextCursor = preloaded.nextCursor;" in publish
    assert "} else {" not in publish, "A stale preload must not clear the live page or cursor"


def test_stale_entries_and_folder_results_do_not_invalidate_the_current_generation():
    main = MAIN.read_text(encoding="utf-8")
    start = main.index("CoverWallPageLoadResult page = cw.entriesFuture.get();")
    end = main.index("// Keep display snapshots independent from pending cursors.", start)
    completions = main[start:end]
    assert "RequestDatabaseInvalidation" not in completions, (
        "An obsolete result must not invalidate a newer first page"
    )
    assert "if (!staleFolders && !staleFoldersView)" in completions


def test_old_entries_view_cannot_reconcile_the_current_page_or_claim_its_append():
    main = MAIN.read_text(encoding="utf-8")
    start = main.index("CoverWallPageLoadResult page = cw.entriesFuture.get();")
    end = main.index("cw.foldersFuture.get();", start)
    completion = main[start:end]
    assert "const bool currentEntriesView = !staleEntries && !staleEntriesView;" in completion
    assert "if (currentEntriesView && !requestMatchesCurrentPageState)" in completion
    assert "else if (currentEntriesView)" in completion
    assert "if (!staleEntries && cw.entriesRequestedAppend)" in completion
    assert "cw.entriesRequestedCursor == cw.globalPageNextCursor" in completion


def test_first_page_restart_prevents_a_competing_preload():
    main = MAIN.read_text(encoding="utf-8")
    start = main.index("if (needStartLoad && !cw.entriesInFlight && !cw.foldersInFlight)")
    end = main.index("CoverWallPageLoadResult page = cw.entriesFuture.get();", start)
    assert "cw.preloadDbPath = mainDbPath;" in main[start:end], (
        "The first-page restart must claim the same DB identity as the preload"
    )


def test_current_preload_can_replace_retained_items_after_content_invalidation():
    main = MAIN.read_text(encoding="utf-8")
    start = main.index("if (canReuseGlobalCache)", main.index("cw.prepareGlobalReloadAfterFacet(mainDbPath);"))
    end = main.index("bool needStartLoad = false;", start)
    ready, _, adoption = main[start:end].partition("if (preloadReadyForReuse) {")
    assert "cw.preloadGeneration == cw.generation" in ready
    assert "cw.preloadDbPath == mainDbPath" in ready
    assert "!cw.loaded &&" in ready, "Old displayed items must not block the current first page"
    assert "cw.items = cw.preloadFullList;" in adoption
