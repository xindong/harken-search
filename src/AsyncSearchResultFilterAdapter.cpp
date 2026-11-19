#include "AsyncSearchResultFilterAdapter.h"

#include "AppState.h"
#include "AsyncWorker.h"
#include "DiagnosticsLogger.h"
#include "FileTypeVisibilityPolicy.h"
#include "PlaylistFolderTree.h"
#include "PlaylistResultFilterWorkflow.h"

#include <algorithm>
#include <utility>

namespace aura::search {

void FilterResultsByFileTypePolicy(
    const AppState& state,
    std::vector<AudioRecord>& batch) {
    const aura::file_types::FileTypeVisibility visibility{
        state.showWAV, state.showMP3, state.showFLAC, state.showOGG,
        state.showAIFF, state.showM4A, state.showWMA, state.showOther, state.showLosslessWavPack};
    const auto policy = aura::file_types::BuildPolicy(visibility);
    if (policy.allVisible) return;
    batch.erase(
        std::remove_if(
            batch.begin(),
            batch.end(),
            [&](const AudioRecord& record) {
                return !aura::file_types::isVisible(
                    visibility, record.extension, record.wavPackLossless.value_or(false));
            }),
        batch.end());
}

void ApplyPlaylistScopedFilters(
    AppState& state,
    std::vector<AudioRecord>& batch,
    bool rebuildTree) {
    const size_t rawCount = batch.size();
    class Effects final : public aura::playlist::PlaylistResultFilterEffects {
    public:
        Effects(AppState& state, const std::vector<AudioRecord>& records)
            : state_(state), records_(records) {}

        void rebuildFolderTree() override {
            state_.setPlaylistFolderHierarchy(
                BuildPlaylistFolderTreeFromRecords(records_),
                state_.selectedPlaylistId);
            state_.folderSearchHits.clear();
            state_.folderSearchDirectHits.clear();
            state_.folderSearchExpandPaths.clear();
            state_.lastFolderSearch.clear();
        }

    private:
        AppState& state_;
        const std::vector<AudioRecord>& records_;
    } effects(state, batch);

    aura::playlist::PlaylistResultFilterInput input;
    input.rebuildFolderTree = rebuildTree;
    input.folderFilterActive = state.folderFilterActive;
    input.selectedFolderPath = state.selectedFolderPath;
    input.visibility = {
        state.showWAV, state.showMP3, state.showFLAC, state.showOGG,
        state.showAIFF, state.showM4A, state.showWMA, state.showOther, state.showLosslessWavPack};
    input.records.reserve(batch.size());
    for (const auto& record : batch) {
        input.records.push_back({record.folderPath, record.extension, record.wavPackLossless.value_or(false)});
    }

    const auto kept = aura::playlist::HandlePlaylistResultFilter(
        input, effects);
    if (kept.size() != batch.size()) {
        std::vector<AudioRecord> filtered;
        filtered.reserve(kept.size());
        for (const auto index : kept) {
            filtered.push_back(std::move(batch[index]));
        }
        batch = std::move(filtered);
    }
    DIAG_ACTION_FMT(
        "Playlist",
        "ScopedFilters playlist=%lld raw=%zu filtered=%zu folderFilter=%d selectedFolder=%s",
        static_cast<long long>(state.selectedPlaylistId),
        rawCount,
        batch.size(),
        state.folderFilterActive ? 1 : 0,
        state.selectedFolderPath.empty()
            ? "<none>"
            : state.selectedFolderPath.c_str());
}

}  // namespace aura::search
