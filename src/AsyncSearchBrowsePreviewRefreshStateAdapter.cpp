#include "AsyncSearchBrowsePreviewRefreshStateAdapter.h"

#include "AppState.h"

#include <utility>

namespace aura::search {

namespace {

class Effects final : public AsyncSearchBrowsePreviewRefreshEffects {
public:
    Effects(
        AppState& state,
        AsyncSearchBrowsePreviewRefreshStateAdapter::
            StartDeferredCoverPriming startDeferredCoverPriming,
        AsyncSearchBrowsePreviewRefreshStateAdapter::ApplyCoverKeyFilter
            applyCoverKeyFilter,
        AsyncSearchBrowsePreviewRefreshStateAdapter::ReadIntegratedMode
            readIntegratedMode,
        AsyncSearchBrowsePreviewRefreshStateAdapter::
            ReadActiveDatabaseFilter readActiveDatabaseFilter,
        AsyncSearchBrowsePreviewRefreshStateAdapter::CanonicalizeFolderPath
            canonicalizeFolderPath)
        : state_(state),
          startDeferredCoverPriming_(std::move(startDeferredCoverPriming)),
          applyCoverKeyFilter_(std::move(applyCoverKeyFilter)),
          readIntegratedMode_(std::move(readIntegratedMode)),
          readActiveDatabaseFilter_(std::move(readActiveDatabaseFilter)),
          canonicalizeFolderPath_(std::move(canonicalizeFolderPath)) {}

    void startDeferredCoverPriming() override {
        startDeferredCoverPriming_();
    }

    void applyCoverKeyFilter() override {
        applyCoverKeyFilter_(state_);
    }

    AsyncSearchBrowsePreviewRefreshState readState() const override {
        return {
            state_.viewMode == ViewMode::Database,
            readIntegratedMode_(state_),
            state_.coverFilterActive && !state_.coverFilterPath.empty(),
            state_.coverFilterPath,
            state_.coverWall.pinnedCoverFilterItem.key,
            state_.coverWall.pinnedCoverFilterItem.filePathUtf8,
            state_.folderFilterActive,
            state_.selectedFolderPath,
            state_.searchBuffer[0] != '\0',
            readActiveDatabaseFilter_(state_)};
    }

    bool readSelectedRecord(
        AsyncSearchBrowsePreviewRecord& record) const override {
        if (state_.selectedResultIndex < 0 ||
            state_.selectedResultIndex >=
                static_cast<int>(state_.searchResults.size())) {
            return false;
        }
        const auto& selected =
            state_.searchResults[static_cast<size_t>(state_.selectedResultIndex)];
        record = {selected.id, selected.filePath, {}};
        return true;
    }

    std::vector<AsyncSearchBrowsePreviewRecord>
    readRefreshedRecords() const override {
        std::vector<AsyncSearchBrowsePreviewRecord> records;
        records.reserve(state_.searchResults.size());
        for (const auto& record : state_.searchResults) {
            records.push_back(
                {record.id, record.filePath,
                 canonicalizeFolderPath_(record.folderPath)});
        }
        return records;
    }

    void selectResult(int index) override {
        state_.selectedResultIndex = index;
        state_.selectedIndicesSet.clear();
        state_.selectedIndicesSet.insert(index);
        state_.lastClickedIndex = index;
    }

private:
    AppState& state_;
    AsyncSearchBrowsePreviewRefreshStateAdapter::StartDeferredCoverPriming
        startDeferredCoverPriming_;
    AsyncSearchBrowsePreviewRefreshStateAdapter::ApplyCoverKeyFilter
        applyCoverKeyFilter_;
    AsyncSearchBrowsePreviewRefreshStateAdapter::ReadIntegratedMode
        readIntegratedMode_;
    AsyncSearchBrowsePreviewRefreshStateAdapter::ReadActiveDatabaseFilter
        readActiveDatabaseFilter_;
    AsyncSearchBrowsePreviewRefreshStateAdapter::CanonicalizeFolderPath
        canonicalizeFolderPath_;
};

}  // namespace

AsyncSearchBrowsePreviewRefreshStateAdapter::
    AsyncSearchBrowsePreviewRefreshStateAdapter(
        AppState& state,
        StartDeferredCoverPriming startDeferredCoverPriming,
        ApplyCoverKeyFilter applyCoverKeyFilter,
        ReadIntegratedMode readIntegratedMode,
        ReadActiveDatabaseFilter readActiveDatabaseFilter,
        CanonicalizeFolderPath canonicalizeFolderPath)
    : state_(state),
      startDeferredCoverPriming_(std::move(startDeferredCoverPriming)),
      applyCoverKeyFilter_(std::move(applyCoverKeyFilter)),
      readIntegratedMode_(std::move(readIntegratedMode)),
      readActiveDatabaseFilter_(std::move(readActiveDatabaseFilter)),
      canonicalizeFolderPath_(std::move(canonicalizeFolderPath)) {}

aura::coverwall::BrowseSurfacePreviewSelection
AsyncSearchBrowsePreviewRefreshStateAdapter::capture() const {
    Effects effects(
        state_,
        startDeferredCoverPriming_,
        applyCoverKeyFilter_,
        readIntegratedMode_,
        readActiveDatabaseFilter_,
        canonicalizeFolderPath_);
    return CaptureAsyncSearchBrowsePreviewRefresh(effects);
}

void AsyncSearchBrowsePreviewRefreshStateAdapter::restore(
    const aura::coverwall::BrowseSurfacePreviewSelection& selection,
    bool restoreSelection) {
    Effects effects(
        state_,
        startDeferredCoverPriming_,
        applyCoverKeyFilter_,
        readIntegratedMode_,
        readActiveDatabaseFilter_,
        canonicalizeFolderPath_);
    HandleAsyncSearchBrowsePreviewRefreshRestore(selection, effects, restoreSelection);
}

}  // namespace aura::search
