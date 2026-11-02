#include "AsyncSearchResultApplicationAdapter.h"

#include "AppState.h"
#include "AsyncSearchAppendResultStateAdapter.h"
#include "AsyncSearchFacetResultStateAdapter.h"
#include "AsyncSearchLoadAllResultStateAdapter.h"
#include "AsyncSearchPlaylistContextStateAdapter.h"
#include "AsyncSearchPlaylistResultPreparationStateAdapter.h"
#include "AsyncSearchQueryTimingRegistry.h"
#include "AsyncSearchRefreshResultCompositionAdapter.h"
#include "AsyncSearchResultDispatchWorkflow.h"
#include "AsyncSearchResultLifecycleStateAdapter.h"
#include "AsyncWorker.h"
#include "CoverReplacementFoundation.h"
#include "LoadAllBatchRuntime.h"

#include <utility>

namespace aura::search {

AsyncSearchResultApplicationAdapter::AsyncSearchResultApplicationAdapter(
    AppState& state,
    AsyncSearchResult& result,
    aura::load_all::LoadAllBatchRuntime& loadAllRuntime,
    AsyncSearchQueryTimingRegistry& queryTiming,
    int loadAllThreshold,
    bool miniWaveformColumnVisible,
    AsyncSearchResultApplicationEffects effects)
    : state_(state),
      result_(result),
      loadAllRuntime_(loadAllRuntime),
      queryTiming_(queryTiming),
      loadAllThreshold_(loadAllThreshold),
      miniWaveformColumnVisible_(miniWaveformColumnVisible),
      effects_(std::move(effects)) {}

void AsyncSearchResultApplicationAdapter::apply() {
    const double queryDurationMs =
        queryTiming_.finishMilliseconds(result_.requestId);
    const auto dispatch = ClassifyAsyncSearchResultDispatch(
        {result_.kind == AsyncSearchKind::FacetsOnly,
         result_.kind == AsyncSearchKind::CountAndFacetsOnly,
         result_.kind == AsyncSearchKind::Refresh,
         result_.kind == AsyncSearchKind::LoadAll});
    const bool facetRelay =
        dispatch == AsyncSearchResultDispatch::FacetsOnly ||
        dispatch == AsyncSearchResultDispatch::CountAndFacetsOnly;
    const bool loadAll = dispatch == AsyncSearchResultDispatch::LoadAll;

    AsyncSearchLoadAllResultStateAdapter loadAllResultAdapter(
        state_, loadAllRuntime_);
    AsyncSearchResultLifecycleStateAdapter lifecycleAdapter(
        state_,
        {
            facetRelay,
            result_.success,
            loadAll,
            !loadAll || loadAllResultAdapter.acceptsResult(),
            queryDurationMs,
        });
    if (lifecycleAdapter.apply()) {
        return;
    }

    AsyncSearchPlaylistContextStateAdapter playlistContextAdapter(
        state_, result_);
    playlistContextAdapter.apply();

    if (facetRelay) {
        const auto kind =
            dispatch == AsyncSearchResultDispatch::FacetsOnly
            ? AsyncSearchFacetResultKind::FacetsOnly
            : AsyncSearchFacetResultKind::CountAndFacetsOnly;
        AsyncSearchFacetResultStateAdapter facetResultAdapter(
            state_,
            result_,
            kind,
            aura::cover_replacement::HasPersistentFacetFilterSemantics,
            [this] {
                return effects_.inconsistentFacetWarmStart(
                    state_, result_);
            },
            effects_.requestCoverWallRefresh);
        facetResultAdapter.apply();
        return;
    }

    AsyncSearchPlaylistResultPreparationStateAdapter
        playlistPreparationAdapter(
            state_,
            result_,
            dispatch,
            loadAllThreshold_,
            effects_.applyPlaylistFilters,
            effects_.applyLibraryFilters);
    playlistPreparationAdapter.apply();

    if (dispatch == AsyncSearchResultDispatch::Refresh) {
        AsyncSearchRefreshResultCompositionAdapter
            refreshCompositionAdapter(
                state_,
                result_,
                miniWaveformColumnVisible_,
                effects_.refreshEffects);
        refreshCompositionAdapter.apply();
    } else {
        const auto appendKind =
            dispatch == AsyncSearchResultDispatch::LoadAll
            ? AsyncSearchAppendKind::LoadAll
            : AsyncSearchAppendKind::LoadMore;
        AsyncSearchAppendResultStateAdapter appendResultAdapter(
            state_,
            result_,
            appendKind,
            effects_.applyCoverKeyFilter,
            [&loadAllResultAdapter](
                int loadedCount,
                bool hasMore,
                bool emptyPage) {
                loadAllResultAdapter.observeResult(
                    loadedCount, hasMore, emptyPage);
            });
        appendResultAdapter.apply();
    }

    state_.syncToTab();
}

}  // namespace aura::search
