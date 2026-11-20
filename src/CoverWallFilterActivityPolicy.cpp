#include "CoverWallFilterActivityPolicy.h"

#include "AppState.h"
#include "AsyncWorker.h"
#include "DatabaseSearchFilterBuilder.h"

namespace aura::cover_wall {

bool HasActiveDbFilterForCoverWall(const AppState& state) {
    if (state.viewMode != ViewMode::Database) return false;
    const SearchFilter filter = aura::search::BuildCurrentDbFilter(state);
    if (!filter.keyword.empty()) return true;
    if (!filter.folderPath.empty()) return true;
    if (!filter.artist.empty() || !filter.artists.empty()) return true;
    if (!filter.album.empty()) return true;
    if (!filter.category.empty()) return true;
    if (!filter.format.empty()) return true;
    if (filter.channels != 0 || filter.minChannels != 0 || filter.maxChannels != 0) return true;
    if (filter.minDuration > 0.0f || filter.maxDuration > 0.0f) return true;
    if (filter.minSampleRate != 0 || filter.maxSampleRate != 0) return true;
    if (filter.coverFilter != 0 || !filter.specificCoverKey.empty()) return true;
    if (!filter.excludeExtensions.empty() || filter.includeOnlyKnownExts || filter.losslessWavPackOnly) return true;
    if (!filter.musicalKey.empty()) return true;
    return false;
}

}  // namespace aura::cover_wall
