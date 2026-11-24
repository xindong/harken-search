#include "AsyncSearchBrowsePreviewRefreshWorkflow.h"

namespace aura::search {

aura::coverwall::BrowseSurfacePreviewSelection
CaptureAsyncSearchBrowsePreviewRefresh(
    const AsyncSearchBrowsePreviewRefreshEffects& effects) {
    const auto state = effects.readState();
    AsyncSearchBrowsePreviewRecord selected;
    const bool hasSelected = effects.readSelectedRecord(selected);
    return CaptureAsyncSearchBrowsePreview(
        {hasSelected,
         selected.fileId,
         selected.filePath,
         state.integratedMode,
         state.coverFilterActive,
         state.coverFilterKey,
         state.pinnedCoverKey,
         state.pinnedFilePath,
         state.folderFilterActive,
         state.selectedFolderPath});
}

void HandleAsyncSearchBrowsePreviewRefreshRestore(
    const aura::coverwall::BrowseSurfacePreviewSelection& selection,
    AsyncSearchBrowsePreviewRefreshEffects& effects,
    bool restoreSelection) {
    effects.startDeferredCoverPriming();
    effects.applyCoverKeyFilter();
    if (!restoreSelection) return;
    const auto state = effects.readState();
    HandleAsyncSearchBrowsePreviewRestore(
        {state.databaseView,
         state.integratedMode,
         aura::coverwall::HasActiveBrowseScope(
             state.folderFilterActive && !state.selectedFolderPath.empty(),
             state.coverFilterActive && !state.coverFilterKey.empty(),
             state.searchTextActive,
             state.activeDatabaseFilter),
         selection,
         effects.readRefreshedRecords()},
        effects);
}

}  // namespace aura::search
