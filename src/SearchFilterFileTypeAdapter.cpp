#include "SearchFilterFileTypeAdapter.h"

#include "AppState.h"
#include "AsyncWorker.h"
#include "FileTypeVisibilityPolicy.h"

namespace aura::search {

void ApplyFileTypePolicyToFilter(
    const AppState& state,
    SearchFilter& filter) {
    const aura::file_types::FileTypeVisibility visibility{
        state.showWAV, state.showMP3, state.showFLAC, state.showOGG,
        state.showAIFF, state.showM4A, state.showWMA, state.showOther, state.showLosslessWavPack};
    const auto policy = aura::file_types::BuildPolicy(visibility);
    filter.excludeExtensions.insert(
        filter.excludeExtensions.end(),
        policy.excludedExtensions.begin(),
        policy.excludedExtensions.end());
    filter.includeOnlyKnownExts =
        filter.includeOnlyKnownExts || policy.includeOnlyKnownExtensions;
    filter.losslessWavPackOnly = policy.losslessWavPackOnly;
}

}  // namespace aura::search
