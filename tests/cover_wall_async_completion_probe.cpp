#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <future>
#include <iostream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

[[noreturn]] void UnexpectedSideEffect() {
    std::cerr << "Obsolete work attempted to publish or query a database\n";
    std::exit(2);
}

namespace CoverModule {
#include "cover_item.inc"

bool GetFolderCoverImagePathBackground(const std::string&, std::string&, bool, bool) {
    UnexpectedSideEffect();
}

std::string NormalizeCoverKeyByVisualIdentity(const std::string&) {
    UnexpectedSideEffect();
}
}

#include "cover_state.inc"

struct Database {
#include "database_entry.inc"

    std::string getMainDbPath() const { return "same-library.db"; }
    bool openReadOnly(const std::string&) { UnexpectedSideEffect(); }
    std::vector<std::string> getDistinctFolderPaths() const { UnexpectedSideEffect(); }
};

struct AppState {
    Database database;
    CoverWallState coverWall;
    bool coverWallDatabaseInvalidationRequested = false;
    bool coverWallDisplayRefreshRequested = false;
    bool needsCoverWallReload = false;
    bool preloadedCoverWallFullyDiscovered = false;
    std::vector<Database::CoverWallEntry> preloadedCoverWallEntries;
    std::atomic<bool> scanDataCommitted{false};
    std::atomic<bool> isScanning{false};
    bool needsDatabaseRefresh = false;

#include "app_invalidation.inc"
#include "app_scan_cover_refresh.inc"
};

#include "force_reload.inc"
namespace aura::cover_wall {
#include "request_invalidation.inc"
}

void DevTools_BeginFramePhase(const char*) {}
void DevTools_EndFramePhase() {}
void DIAG_ACTION_FMT(const char*, const char*, ...) {}
void SanitizeCoverWallItemsForCache(std::vector<CoverModule::CoverItem>&) { UnexpectedSideEffect(); }
void AppendUniqueCoverWallPageItems(std::vector<CoverModule::CoverItem>&,
                                   std::vector<CoverModule::CoverItem>&) { UnexpectedSideEffect(); }
void StoreGlobalCoverWallCache(CoverWallState&, const std::string&, int64_t) { UnexpectedSideEffect(); }
std::string BaseNameFromUtf8Path(const std::string&) { UnexpectedSideEffect(); }

void ApplyReadyCompletions(AppState& state) {
    auto& cw = state.coverWall;
    const auto mainDbPath = state.database.getMainDbPath();
    const auto& currentDbPath = mainDbPath;
    constexpr int64_t kCoverWallFolderDiscoveryMaxFiles = 50000;
    bool coverWallPreloadPhaseActive = false;
#include "completions.inc"
}

CoverModule::CoverItem CoverWithKey(const char* key) {
    CoverModule::CoverItem item;
    item.key = key;
    return item;
}

int TestScanBackfillRefresh() {
    AppState state;
    auto& cw = state.coverWall;
    const auto dbPath = state.database.getMainDbPath();
    const auto generation = cw.generation;
    cw.loaded = true;
    cw.lastTotalFileCount = 768;
    cw.lastScrollY = 420.0f;
    cw.items = {CoverWithKey("visible-cover")};
    cw.displaySnapshot = cw.items;
    cw.globalItemsCache = cw.items;
    cw.globalItemsCacheValid = true;
    cw.globalItemsCacheDbPath = dbPath;
    cw.globalPageDbPath = dbPath;
    cw.globalPageNextCursor = "fl2:old-page";
    cw.globalPageHasMore = true;
    cw.preloadFullList = cw.items;
    cw.preloadDbPath = dbPath;
    state.preloadedCoverWallEntries.emplace_back();
    state.preloadedCoverWallFullyDiscovered = true;

    std::promise<CoverWallPageLoadResult> oldPage;
    cw.entriesFuture = oldPage.get_future();
    cw.entriesInFlight = true;
    cw.entriesGeneration = cw.generation;
    const auto applyResult = [](AppState& state_) {
        const struct { bool scanCoverRefresh; } result_{true};
#include "scan_cover_refresh.inc"
    };
    applyResult(state);

    if (cw.loaded || cw.generation == generation || cw.globalItemsCacheValid ||
        !cw.globalPageNextCursor.empty() || !cw.preloadFullList.empty() ||
        !state.preloadedCoverWallEntries.empty() || state.preloadedCoverWallFullyDiscovered) {
        std::cerr << "The same-size global wall or its stale cover cache would be reused\n";
        return 1;
    }
    oldPage.set_value({{CoverWithKey("obsolete-cover")}, "", false});
    ApplyReadyCompletions(state);
    if (cw.items.size() != 1 || cw.items.front().key != "visible-cover" ||
        cw.displaySnapshot.size() != 1 || cw.lastScrollY != 420.0f ||
        cw.texturesPendingClear || cw.entriesInFlight || !state.needsCoverWallReload) {
        std::cerr << "The in-flight refresh did not preserve the visible wall or retire the obsolete page\n";
        return 1;
    }
    return 0;
}

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    const std::string requestKind = argv[1];
    if (requestKind == "scan-backfill") return TestScanBackfillRefresh();
    AppState state;
    auto& cw = state.coverWall;
    const auto dbPath = state.database.getMainDbPath();
    const std::string currentCursor = "fl2:current-first-page";

    std::promise<CoverWallPageLoadResult> oldPage;
    std::promise<std::vector<CoverModule::CoverItem>> oldFolders;
    const bool keepAppendPending = requestKind == "preload";
    if (requestKind == "preload") {
        cw.preloadFuture = oldPage.get_future();
        cw.preloadInFlight = true;
        cw.preloadGeneration = cw.generation;
    } else if (requestKind == "entries") {
        cw.entriesFuture = oldPage.get_future();
        cw.entriesInFlight = true;
        cw.entriesGeneration = cw.generation;
        cw.entriesViewGeneration = cw.viewGeneration;
    } else if (requestKind == "folders") {
        cw.foldersFuture = oldFolders.get_future();
        cw.foldersInFlight = true;
        cw.foldersGeneration = cw.generation;
        cw.foldersViewGeneration = cw.viewGeneration;
    } else {
        return 2;
    }

    state.invalidateCoverWallAfterDatabaseContentChange();
    cw.prepareGlobalReloadAfterFacet(dbPath);
    const auto currentGeneration = cw.generation;
    state.coverWallDatabaseInvalidationRequested = false;
    state.needsCoverWallReload = false;
    cw.items = {CoverWithKey("current-one"), CoverWithKey("current-two")};
    cw.globalPageDbPath = dbPath;
    cw.globalPageNextCursor = currentCursor;
    cw.globalPageHasMore = true;
    cw.loaded = true;
    cw.loading = keepAppendPending;
    cw.globalPageLoadingNext = keepAppendPending;

    std::promise<CoverWallPageLoadResult> currentAppend;
    if (keepAppendPending) {
        cw.entriesFuture = currentAppend.get_future();
        cw.entriesInFlight = true;
        cw.entriesGeneration = cw.generation;
        cw.entriesViewGeneration = cw.viewGeneration;
        cw.entriesRequestedAppend = true;
        cw.entriesRequestedCursor = currentCursor;
        cw.entriesRequestBaseItemCount = cw.items.size();
    }

    if (requestKind == "folders") {
        oldFolders.set_value({CoverWithKey("old-folder")});
    } else {
        oldPage.set_value({{CoverWithKey("old-page")}, "fl2:old-page", true});
    }
    ApplyReadyCompletions(state);

    bool passed = true;
    const auto expect = [&](bool condition, const char* failure) {
        if (!condition) {
            std::cerr << requestKind << ": " << failure << '\n';
            passed = false;
        }
    };
    expect(cw.items.size() == 2 && cw.items[0].key == "current-one" &&
           cw.items[1].key == "current-two", "current first-page items changed");
    expect(cw.globalPageNextCursor == currentCursor && cw.globalPageDbPath == dbPath,
           "current first-page cursor or DB identity changed");
    expect(cw.loaded && cw.globalPageHasMore, "first page lost loaded or has-more state");
    expect(cw.loading == keepAppendPending && cw.globalPageLoadingNext == keepAppendPending &&
           !cw.discoveryDone, "current loading, append, or discovery flag changed");
    expect(cw.generation == currentGeneration &&
           !state.coverWallDatabaseInvalidationRequested && !state.needsCoverWallReload,
           "obsolete completion invalidated the current generation");
    expect(!cw.preloadInFlight && !cw.preloadFuture.valid(), "old preload did not retire");
    expect(!cw.foldersInFlight && !cw.foldersFuture.valid(), "old folder request did not retire");
    if (keepAppendPending) {
        expect(cw.entriesInFlight && cw.entriesFuture.valid() &&
               cw.entriesFuture.wait_for(std::chrono::seconds(0)) == std::future_status::timeout &&
               cw.entriesRequestedAppend && cw.entriesRequestedCursor == currentCursor,
               "newer append no longer owns its request");
    } else {
        expect(!cw.entriesInFlight && !cw.entriesFuture.valid(), "old entries request did not retire");
    }
    return passed ? 0 : 1;
}
