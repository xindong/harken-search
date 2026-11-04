#include "SearchPresetFilterBuilders.h"

#include "AppState.h"
#include "AsyncWorker.h"
#include "SearchFilterFileTypeAdapter.h"
#include "SearchKeywordCompiler.h"

#include <ctime>

namespace aura::search {

SearchFilter BuildCurrentPlaylistSearchFilter(const AppState& state) {
    SearchFilter filter;
    filter.keyword = CompileSearchBufferKeywordForFilter(state.searchBuffer);
    if (state.folderFilterActive) filter.folderPath = state.selectedFolderPath;
    filter.coverFilter = state.coverFilterIndex;
    if (state.coverFilterActive) filter.specificCoverKey = state.coverFilterPath;
    filter.exactMode = state.searchExactMode;
    ApplyFileTypePolicyToFilter(state, filter);
    return filter;
}

SearchFilter BuildCurrentSmartCollectionFilter(const AppState& state) {
    SearchFilter filter;
    filter.keyword = CompileSearchBufferKeywordForFilter(state.searchBuffer);
    filter.exactMode = state.searchExactMode;
    ApplyFileTypePolicyToFilter(state, filter);
    const std::int64_t now = static_cast<std::int64_t>(std::time(nullptr));
    switch (state.activeSmartCollection) {
        case SmartCollection::RecentlyAdded:
            filter.addedAfter = now - 30LL * 24 * 3600;
            filter.sortColumn = static_cast<int>(SortColumn::DateAdded);
            filter.sortAscending = false;
            break;
        case SmartCollection::RecentlyPlayed:
            filter.playedOnlyAny = true;
            filter.sortColumn = static_cast<int>(SortColumn::History);
            filter.sortAscending = false;
            break;
        case SmartCollection::Unplayed:
            filter.unplayedOnly = true;
            filter.sortColumn = static_cast<int>(SortColumn::DateAdded);
            filter.sortAscending = false;
            break;
        case SmartCollection::HighlyRated:
            filter.highlyRatedOnly = true;
            filter.sortColumn = static_cast<int>(SortColumn::FileName);
            filter.sortAscending = true;
            break;
        default:
            break;
    }
    return filter;
}

}  // namespace aura::search
