#include "SearchFilterStatePolicy.h"

#include "AppState.h"

namespace aura::search {

bool HasAdvancedDbFilters(const AppState& state) {
    if (state.searchBuffer[0] != '\0') return true;
    if (!state.artistFilterBuffer.empty()) return true;
    if (!state.artistFilterAlternates.empty()) return true;
    if (!state.albumFilterBuffer.empty()) return true;
    if (!state.categoryFilterBuffer.empty()) return true;
    if (state.formatFilterIndex != 0) return true;
    if (state.channelFilterIndex != 0) return true;
    if (state.sampleRateFilterIndex != 0) return true;
    if (state.minDurationFilter > 0.0f) return true;
    if (state.maxDurationFilter > 0.0f) return true;
    if (state.namingFilter != 0) return true;
    if (state.coverFilterIndex != 0) return true;
    if (state.minBpmFilter > 0.0f) return true;
    if (state.maxBpmFilter > 0.0f) return true;
    if (state.keyFilterBuffer[0] != '\0') return true;
    return false;
}

}  // namespace aura::search
