#include "AsyncSearchRefreshResultsStateAdapter.h"

#include "AppState.h"
#include "AsyncWorker.h"

#include <unordered_set>
#include <utility>

namespace aura::search {

namespace {

class Effects final : public AsyncSearchRefreshResultsEffects {
public:
    Effects(AppState& state, AsyncSearchResult& result)
        : state_(state), result_(result) {}

    void replaceRows() override {
        if (result_.scanCoverRefresh) {
            const auto fileIdAt = [this](int index) -> int64_t {
                return index >= 0 && index < static_cast<int>(state_.searchResults.size())
                    ? state_.searchResults[index].id : 0;
            };
            const auto selectedId = fileIdAt(state_.selectedResultIndex);
            const auto anchorId = fileIdAt(state_.lastClickedIndex);
            std::unordered_set<int64_t> selectedIds;
            for (const int index : state_.selectedIndicesSet) {
                if (const auto id = fileIdAt(index); id > 0) selectedIds.insert(id);
            }
            state_.selectedResultIndex = -1;
            state_.lastClickedIndex = -1;
            state_.selectedIndicesSet.clear();
            if (selectedId > 0 || anchorId > 0 || !selectedIds.empty()) {
                for (int index = 0; index < static_cast<int>(result_.results.size()); ++index) {
                    const auto id = result_.results[index].id;
                    if (id <= 0) continue;
                    if (id == selectedId) state_.selectedResultIndex = index;
                    if (id == anchorId) state_.lastClickedIndex = index;
                    if (selectedIds.count(id)) state_.selectedIndicesSet.insert(index);
                }
            }
        }
        state_.searchResults = std::move(result_.results);
    }
    void throttleMiniWaveforms(int frames) override {
        state_.postSearchMiniWaveThrottleFrames =
            (std::max)(state_.postSearchMiniWaveThrottleFrames, frames);
    }
    void resultsChanged() override { state_.bumpSearchResultsGen(); }
    void setBuiltRevision(std::uint64_t revision) override {
        state_.setSearchResultsBuiltRevision(revision);
    }
    void setLoadedState(int loadedCount, bool hasMore) override {
        state_.loadedCount = loadedCount;
        state_.hasMoreResults = hasMore;
    }
    void clearSelection() override {
        if (result_.scanCoverRefresh) return;
        state_.selectedResultIndex = -1;
        state_.selectedIndicesSet.clear();
        state_.lastClickedIndex = -1;
    }

private:
    AppState& state_;
    AsyncSearchResult& result_;
};

}  // namespace

AsyncSearchRefreshResultsStateAdapter::
    AsyncSearchRefreshResultsStateAdapter(
        AppState& state,
        AsyncSearchResult& result)
    : state_(state), result_(result) {}

void AsyncSearchRefreshResultsStateAdapter::apply() {
    Effects effects(state_, result_);
    HandleAsyncSearchRefreshResults(
        {result_.loadedCount, result_.hasMore, result_.requestedAtDbRevision},
        effects);
}

}  // namespace aura::search
