#include "AsyncSearchRefreshResultCompositionAdapter.h"

#include "AppState.h"
#include "AsyncSearchBrowsePreviewRefreshStateAdapter.h"
#include "AsyncSearchCoverPreloadStateAdapter.h"
#include "AsyncSearchEmptyFallbackStateAdapter.h"
#include "AsyncSearchRefreshCompletionStateAdapter.h"
#include "AsyncSearchRefreshCountRepairAdapter.h"
#include "AsyncSearchRefreshDiagnosticsStateAdapter.h"
#include "AsyncSearchRefreshFacetsAdapter.h"
#include "AsyncSearchRefreshLoadingStateAdapter.h"
#include "AsyncSearchRefreshMemoryStateAdapter.h"
#include "AsyncSearchRefreshResultsStateAdapter.h"
#include "AsyncSearchWaveformPreloadStateAdapter.h"
#include "AsyncWorker.h"
#include "CoverReplacementFoundation.h"
#include "RandomShufflePlaybackStateAdapter.h"

#include <utility>

namespace aura::search {

AsyncSearchRefreshResultCompositionAdapter::
    AsyncSearchRefreshResultCompositionAdapter(
        AppState& state,
        AsyncSearchResult& result,
        bool miniWaveformColumnVisible,
        AsyncSearchRefreshResultCompositionEffects effects)
    : state_(state),
      result_(result),
      miniWaveformColumnVisible_(miniWaveformColumnVisible),
      effects_(std::move(effects)) {}

void AsyncSearchRefreshResultCompositionAdapter::apply() {
    AsyncSearchBrowsePreviewRefreshStateAdapter browsePreviewAdapter(
        state_,
        effects_.startDeferredCoverPriming,
        effects_.applyCoverKeyFilter,
        effects_.readIntegratedMode,
        effects_.readActiveDatabaseFilter,
        effects_.canonicalizeFolderPath);
    const auto browseSurfaceSelection = browsePreviewAdapter.capture();

    AsyncSearchRefreshResultsStateAdapter refreshResultsAdapter(
        state_, result_);
    refreshResultsAdapter.apply();

    AsyncSearchRefreshCountRepairAdapter countRepairAdapter(
        state_,
        result_,
        effects_.hasActiveDatabaseFilter,
        effects_.submitCountRepair);
    countRepairAdapter.apply();

    AsyncSearchRefreshFacetsAdapter facetsAdapter(
        state_,
        result_,
        effects_.hasActiveDatabaseFilter,
        aura::cover_replacement::HasPersistentFacetFilterSemantics,
        effects_.requestCoverWallRefresh);
    facetsAdapter.apply();

    AsyncSearchEmptyFallbackStateAdapter emptyFallbackAdapter(state_);
    emptyFallbackAdapter.apply();

    browsePreviewAdapter.restore(browseSurfaceSelection, !result_.scanCoverRefresh);
    if (result_.scanCoverRefresh) {
        state_.coverWall.filteredLastSelSize = 0;
        state_.requestCoverWallScanBackfillRefresh();
    }

    AsyncSearchCoverPreloadStateAdapter coverPreloadAdapter(
        state_,
        effects_.hasAdvancedFilters,
        effects_.isCoverWallPriorityMode);
    coverPreloadAdapter.apply();

    AsyncSearchWaveformPreloadStateAdapter waveformPreloadAdapter(
        state_, miniWaveformColumnVisible_);
    waveformPreloadAdapter.apply();

    AsyncSearchRefreshCompletionStateAdapter refreshCompletionAdapter(
        state_,
        effects_.hasAdvancedFilters,
        effects_.requestCoverWallRefresh);
    refreshCompletionAdapter.apply();

    AsyncSearchRandomShufflePlaybackStateAdapter randomShuffleAdapter(
        state_,
        result_,
        effects_.scheduleWaveformDetail,
        effects_.resetWaveformZoom,
        effects_.markFilePlayed);
    randomShuffleAdapter.apply();

    AsyncSearchRefreshDiagnosticsStateAdapter refreshDiagnosticsAdapter(
        state_);
    refreshDiagnosticsAdapter.apply();

    AsyncSearchRefreshLoadingStateAdapter refreshLoadingAdapter(state_);
    refreshLoadingAdapter.apply();

    AsyncSearchRefreshMemoryStateAdapter memoryAdapter(
        state_, effects_.releaseWaveformEvicted);
    memoryAdapter.apply();
}

}  // namespace aura::search
