#include "src/AppState.h"

#include <chrono>
#include <cstdio>
#include <vector>

int main() {
    {
        AppSettings height;
        height.rowHeight=40;height.listDensity=ListDensity::Custom;
        if(height.getEffectiveRowHeight()!=40)return 1;
        height.rowHeight=1000;if(height.getEffectiveRowHeight()!=100)return 1;
        height.rowHeight=-1;if(height.getEffectiveRowHeight()!=16)return 1;
        height.rowHeight=40;height.listDensity=ListDensity::Normal;height.readingFontSize=14;
        if(height.getEffectiveRowHeight()!=28)return 1;
        height.listDensity=ListDensity::Compact;height.readingFontSize=24;
        if(height.getEffectiveRowHeight()!=28)return 1;
    }
    if (!aura::settings_search::MatchesAllTokens("theme font", "Theme and Font controls")) {
        std::printf("FAIL: ASCII settings search should match across case and separators\n");
        return 1;
    }
    if (!aura::settings_search::MatchesAllTokens(u8"音频 设备", u8"音频 输出 设备")) {
        std::printf("FAIL: UTF-8 settings search should match repeated localized tokens\n");
        return 1;
    }
    if (aura::settings_search::MatchesAllTokens("lufs meter", "theme font")) {
        std::printf("FAIL: unrelated settings tokens should not match\n");
        return 1;
    }
    const aura::settings_search::TokenList cachedTokens =
        aura::settings_search::TokenizeNormalizedQuery(u8"音频 设备");
    const std::vector<std::string> searchIndex = {
        aura::settings_search::Normalize(u8"音频 输出 设备"),
        aura::settings_search::Normalize(u8"搜索 建议 来源"),
        aura::settings_search::Normalize(u8"诊断 日志"),
        aura::settings_search::Normalize(u8"扫描 引擎"),
        aura::settings_search::Normalize(u8"界面 元素 tooltip"),
        aura::settings_search::Normalize(u8"双耳 监听"),
    };
    size_t cachedMatchCount = 0;
    const auto perfStart = std::chrono::steady_clock::now();
    for (int repeat = 0; repeat < 4000; ++repeat) {
        for (const std::string& normalizedEntry : searchIndex) {
            if (aura::settings_search::MatchesNormalizedTokens(cachedTokens, normalizedEntry)) {
                ++cachedMatchCount;
            }
        }
    }
    const auto perfElapsedMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - perfStart).count();
    if (cachedMatchCount != 4000) {
        std::printf("FAIL: cached settings search tokens should find exactly one matching entry per pass\n");
        return 1;
    }
    if (perfElapsedMs > 250) {
        std::printf("FAIL: cached settings search smoke benchmark exceeded budget (%lld ms)\n",
                    static_cast<long long>(perfElapsedMs));
        return 1;
    }

    AppSettings settings;
    if (settings.readingFontSize != 18 || settings.listFontSize != 18) {
        std::printf("FAIL: new system/list font defaults should be 18/18, got %d/%d\n",
                    settings.readingFontSize, settings.listFontSize);
        return 1;
    }
    if (kChromeFontPx != 14 ||
        ResolveReadingFontSize(true, 22, true, 20, true, 13, 16) != 22 ||
        ResolveReadingFontSize(false, 0, true, 20, true, 13, 16) != 20 ||
        ResolveReadingFontSize(false, 0, false, 0, false, 0, 16) != 16) {
        std::printf("FAIL: saved font priority or fixed control font changed\n");
        return 1;
    }
    if (settings.themeScheduleMode != harken::theme_schedule::Mode::Off) {
        std::printf("FAIL: day/night theme mode should be off by default\n");
        return 1;
    }
    if (settings.autoReconcileOnStartup) {
        std::printf("FAIL: startup reconcile should be opt-in by default\n");
        return 1;
    }
    if (settings.cacheWaveformsOnScan) {
        std::printf("FAIL: scan should not hide waveform backfill by default\n");
        return 1;
    }
    if (!settings.showPlaylistQuickBar) {
        std::printf("FAIL: playlist quick bar should be visible by default\n");
        return 1;
    }
    if (!settings.playlistQuickBarOrder.empty()) {
        std::printf("FAIL: playlist quick bar order should start empty\n");
        return 1;
    }
    if (!settings.playlistQuickBarHiddenIds.empty()) {
        std::printf("FAIL: all playlists should be visible in the quick bar by default\n");
        return 1;
    }
    if (!settings.keepHarkenSearchOutputProcessingSuffix) {
        std::printf("FAIL: Send to DAW should preserve legacy processing suffixes by default for compatibility\n");
        return 1;
    }

    CoverWallState coverWall;
    CoverModule::CoverItem item;
    item.key = "F|old";
    item.folderPathUtf8 = "H:/Old";
    coverWall.items.push_back(item);
    coverWall.preloadFullList.push_back(item);
    coverWall.preloadFirstPage.push_back(item);
    coverWall.preloadDbPath = "old.db";
    coverWall.globalPageNextCursor = "F|next";
    coverWall.globalPageHasMore = true;
    coverWall.globalPageLoadingNext = true;
    coverWall.globalPageDbPath = "old.db";
    coverWall.loaded = true;
    coverWall.loading = true;
    coverWall.discoveryDone = true;
    coverWall.lastTotalFileCount = 123;
    coverWall.loadTotal = 123;
    coverWall.displaySnapshot.push_back(item);
    coverWall.lastDisplaySnapshotCursor = 1;
    coverWall.entriesInFlight = true;
    coverWall.foldersInFlight = true;
    coverWall.preloadInFlight = true;
    coverWall.entriesGeneration = coverWall.generation;
    coverWall.foldersGeneration = coverWall.generation;
    coverWall.preloadGeneration = coverWall.generation;

    const uint64_t oldCoverWallGeneration = coverWall.generation;
    coverWall.invalidateAfterDatabaseContentChange();
    if (coverWall.generation == oldCoverWallGeneration ||
        coverWall.entriesGeneration == coverWall.generation ||
        coverWall.foldersGeneration == coverWall.generation ||
        coverWall.preloadGeneration == coverWall.generation) {
        std::printf("FAIL: active cover wall futures should become stale after database content changes\n");
        return 1;
    }
    if (!coverWall.entriesInFlight || !coverWall.foldersInFlight || !coverWall.preloadInFlight) {
        std::printf("FAIL: in-flight cover wall futures should finish naturally so stale results can be discarded\n");
        return 1;
    }
    if (coverWall.preloadFullList.size() != 0 ||
        coverWall.preloadFirstPage.size() != 0 ||
        !coverWall.preloadDbPath.empty() ||
        !coverWall.globalPageNextCursor.empty() ||
        coverWall.globalPageHasMore ||
        coverWall.globalPageLoadingNext ||
        !coverWall.globalPageDbPath.empty()) {
        std::printf("FAIL: cover wall preload and pagination cache should be invalidated after database content changes\n");
        return 1;
    }
    if (coverWall.loaded || coverWall.loading || coverWall.discoveryDone ||
        coverWall.lastTotalFileCount != 0 || coverWall.loadTotal != 0 ||
        !coverWall.displaySnapshot.empty() || coverWall.lastDisplaySnapshotCursor != 0) {
        std::printf("FAIL: cover wall global load state should require reload after database content changes\n");
        return 1;
    }
    if (coverWall.items.empty()) {
        std::printf("FAIL: visible cover wall items should remain as a stable snapshot while reload starts\n");
        return 1;
    }

    // A partial global page is not a complete reusable cache. Filtered facets
    // replace the shared item list; clearing the filter must restart that page,
    // not append old global cursor results to the filtered list.
    {
        CoverWallState switched;
        CoverModule::CoverItem globalItem;
        globalItem.key = "E|global_first";
        switched.items = {globalItem};
        switched.loaded = true;
        switched.globalPageDbPath = "fixture.db";
        switched.globalPageNextCursor = "file-list-cursor-after-512";
        switched.globalPageHasMore = true;
        switched.globalItemsCacheValid = false;
        switched.preloadDbPath = "fixture.db";
        switched.lastTotalFileCount = 1024;
        switched.generation = 7;
        switched.viewGeneration = 11;
        switched.entriesGeneration = switched.generation;
        switched.entriesViewGeneration = switched.viewGeneration;
        switched.foldersViewGeneration = switched.viewGeneration;
        switched.entriesRequestedCursor = switched.globalPageNextCursor;
        switched.entriesRequestedAppend = true;
        // The shared items now contain the final filtered facet payload.
        CoverModule::CoverItem filteredItem;
        filteredItem.key = "E|filtered_only";
        switched.items = {filteredItem};
        switched.displaySnapshot = switched.items;
        switched.preloadFullList = {globalItem};
        switched.preloadFirstPage = {globalItem};
        switched.prevUseFacetMode = true;
        std::promise<CoverWallPageLoadResult> pendingPage;
        switched.entriesFuture = pendingPage.get_future();
        switched.entriesInFlight = true;
        switched.foldersInFlight = true;
        switched.preloadInFlight = true;
        switched.loading = true;
        switched.globalPageLoadingNext = true;
        switched.prepareGlobalReloadAfterFacet("fixture.db");
        if (switched.loaded || switched.loading || switched.discoveryDone ||
            !switched.items.empty() || !switched.displaySnapshot.empty() ||
            !switched.globalPageNextCursor.empty() || switched.globalPageHasMore ||
            switched.globalPageLoadingNext || !switched.globalPageDbPath.empty() ||
            !switched.preloadFullList.empty() || !switched.preloadFirstPage.empty() ||
            switched.preloadDbPath != "fixture.db") {
            std::printf("FAIL: returning from facets must discard mixed items and restart the global first page\n");
            return 1;
        }
        if (switched.generation != 7 || switched.viewGeneration != 12 ||
            switched.entriesViewGeneration == switched.viewGeneration ||
            switched.foldersViewGeneration == switched.viewGeneration ||
            !switched.entriesInFlight || !switched.foldersInFlight || !switched.preloadInFlight ||
            !switched.entriesFuture.valid() ||
            switched.entriesFuture.wait_for(std::chrono::seconds(0)) != std::future_status::timeout) {
            std::printf("FAIL: global reload must reject old view results without replacing or waiting on active futures\n");
            return 1;
        }
        pendingPage.set_value(CoverWallPageLoadResult{});
        switched.entriesFuture.get();
        // Normal render-loop completion drains the old result; it is now safe
        // for the existing !loaded && !loading gate to request the first page.
        switched.entriesInFlight = false;
        switched.foldersInFlight = false;
        switched.preloadInFlight = false;
        if (switched.loaded || switched.loading || switched.globalPageHasMore) {
            std::printf("FAIL: drained obsolete work must leave the global first-page load armed\n");
            return 1;
        }

        // A preload can complete during filtering and be collected immediately
        // before returning to the global branch. Reuse that same-DB first page.
        CoverWallState readyPreload;
        readyPreload.generation = 7;
        readyPreload.preloadGeneration = 7;
        readyPreload.items = {filteredItem};
        readyPreload.loaded = true;
        readyPreload.preloadFullList = {globalItem};
        readyPreload.preloadDbPath = "fixture.db";
        readyPreload.globalPageDbPath = "fixture.db";
        readyPreload.globalPageNextCursor = "fresh-first-page-cursor";
        readyPreload.globalPageHasMore = true;
        readyPreload.prepareGlobalReloadAfterFacet("fixture.db");
        if (!readyPreload.items.empty() || readyPreload.loaded ||
            readyPreload.preloadFullList.size() != 1 ||
            readyPreload.preloadFullList.front().key != globalItem.key ||
            readyPreload.preloadDbPath != "fixture.db" ||
            readyPreload.globalPageNextCursor != "fresh-first-page-cursor" ||
            !readyPreload.globalPageHasMore) {
            std::printf("FAIL: a freshly collected global preload must survive the facet-to-global transition\n");
            return 1;
        }
        for (bool wrongDatabase : {false, true}) {
            readyPreload.preloadFullList = {globalItem};
            readyPreload.preloadDbPath = "fixture.db";
            readyPreload.globalPageDbPath = "fixture.db";
            readyPreload.preloadGeneration = wrongDatabase ? 7 : 6;
            readyPreload.globalPageNextCursor = "stale-preload-cursor";
            readyPreload.globalPageHasMore = true;
            const std::string nextDatabase = wrongDatabase ? "other.db" : "fixture.db";
            readyPreload.prepareGlobalReloadAfterFacet(nextDatabase);
            if (!readyPreload.preloadFullList.empty() ||
                !readyPreload.globalPageNextCursor.empty() || readyPreload.globalPageHasMore ||
                readyPreload.preloadDbPath != nextDatabase) {
                std::printf("FAIL: wrong-database or stale-generation preloads must not survive the global transition\n");
                return 1;
            }
        }
    }

    // S9: DawTarget Pro Tools spot settings must default to safe values.
    {
        DawTarget dt;
        if (dt.ptSpotMode != HarkenSearch::ProTools::SpotMode::AtCursor) {
            std::printf("FAIL: DawTarget.ptSpotMode should default to AtCursor (safe cursor-position default)\n");
            return 1;
        }
        if (dt.ptTrackFallback != HarkenSearch::ProTools::TrackFallback::AutoCreateNew) {
            std::printf("FAIL: DawTarget.ptTrackFallback should default to AutoCreateNew (matches user-requested behavior)\n");
            return 1;
        }
    }

    std::printf("PASS: startup reconcile is opt-in, scan waveform cache is on-demand, playlist quick bar defaults are safe\n");
    return 0;
}
