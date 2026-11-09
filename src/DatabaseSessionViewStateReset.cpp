#include "DatabaseSessionViewStateReset.h"

#include "AppState.h"

namespace aura::database_session {

void ResetDatabaseSessionViewState(AppState& state) {
    state.searchResults.clear();
    state.totalFileCount = 0;
    state.loadedCount = 0;
    state.hasMoreResults = false;
    state.loadingMore = false;
    state.selectedResultIndex = -1;
    state.scrollToSelected = false;
    state.currentFileId = -1;

    state.searchBuffer[0] = '\0';
    state.lastAppliedSearchText.clear();
    state.lastSearchSubmitted.clear();
    state.artistFilterBuffer.clear();
    state.artistFilterAlternates.clear();
    state.albumFilterBuffer.clear();
    state.categoryFilterBuffer.clear();
    state.formatFilterIndex = 0;
    state.channelFilterIndex = 0;
    state.minDurationFilter = 0.0f;
    state.maxDurationFilter = 0.0f;
    state.sampleRateFilterIndex = 0;
    state.coverFilterIndex = 0;
    state.minBpmFilter = 0.0f;
    state.maxBpmFilter = 0.0f;
    state.keyFilterBuffer[0] = '\0';
    state.namingFilter = 0;
    state.activeSmartCollection = SmartCollection::None;
    state.activeUserCollectionIdx = -1;
    state.genrePillState.clear();
    state.instrumentPillState.clear();
    state.currentCoverFacets.clear();
    state.currentPathFacets.clear();

    state.clearFolderHierarchyState();
    state.folderFilterActive = false;
    state.selectedFolderPath.clear();
    state.folderSearchBuffer[0] = '\0';
    state.folderTreeScrollTarget.clear();
    state.folderSearchHits.clear();
    state.folderSearchDirectHits.clear();
    state.folderSearchExpandPaths.clear();
    state.lastFolderSearch.clear();

    state.playlists.clear();
    state.selectedPlaylistId = -1;
    state.playlistAggregatedView = false;
    state.viewMode = ViewMode::Database;

    state.findSameFilenameActive = false;
    state.findSameFilenameQuery.clear();

    state.randomSampleMode = false;
    state.randomShownIds.clear();
    state.randomAvailableIds.clear();
    state.randomTotalPool = 0;
    state.playedFileIds.clear();
    state.randomAllPlayed = false;

    state.clearSelection();
    state.isDragging = false;
    state.dragStartIndex = -1;

    state.searchSuggestCandidates.clear();
    state.searchSuggestLastQueryNorm.clear();

    state.pendingSearchRequestId = 0;
    state.pendingFacetRequestId = 0;
    state.asyncSearchInProgress = false;
    state.lastSearchSubmitted.clear();
    state.lastAppliedSearchText.clear();
    state.filterQueryGeneration++;
    state.filterQueryAppliedGeneration = 0;
    state.filterQueryState = FilterQueryState::Idle;
    state.filterQueryError.clear();

    for (auto& tab : state.listTabs) {
        tab.searchResults.clear();
        tab.totalFileCount = 0;
        tab.loadedCount = 0;
        tab.hasMoreResults = false;
        tab.loadingMore = false;
        tab.selectedResultIndex = -1;
        tab.selectedPlaylistId = -1;
        tab.playlistAggregatedView = false;
        tab.selectedFolderPath.clear();
        tab.folderFilterActive = false;
        tab.coverFilterActive = false;
        tab.coverFilterPath.clear();
        tab.coverFilterIndex = 0;
        tab.searchBuffer[0] = '\0';
        tab.coverFacets.clear();
        tab.pathFacets.clear();
        tab.selectedIndicesSet.clear();
        tab.lastClickedIndex = -1;
        tab.findSameFilenameQuery.clear();
        tab.findSameFilenameActive = false;
    }

    state.coverFilterPath.clear();
    state.coverFilterActive = false;
    state.coverFilterMatchSig = 0;
    state.coverFilterMatchPaths.clear();

    state.coverRebuildActive = false;
    state.coverRebuildBatch.clear();
    state.coverRebuildFolderCache.clear();

    state.playHistory.clear();
    state.playHistoryIndex = -1;
    state.viewStateHistory.clear();
    state.viewStateIndex = -1;

    state.sameFolderRecycleBuffer.clear();
    state.sameFolderScanActive = false;
    state.sameFolderScanDone = false;
    state.sameFolderScanPath.clear();

    state.playedFiles.clear();
}

} // namespace aura::database_session
