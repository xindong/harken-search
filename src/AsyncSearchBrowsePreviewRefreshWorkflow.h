#pragma once

#include "AsyncSearchBrowsePreviewWorkflow.h"

#include <string>
#include <vector>

namespace aura::search {

struct AsyncSearchBrowsePreviewRefreshState {
    bool databaseView = false;
    bool integratedMode = false;
    bool coverFilterActive = false;
    std::string coverFilterKey;
    std::string pinnedCoverKey;
    std::string pinnedFilePath;
    bool folderFilterActive = false;
    std::string selectedFolderPath;
    bool searchTextActive = false;
    bool activeDatabaseFilter = false;
};

class AsyncSearchBrowsePreviewRefreshEffects
    : public AsyncSearchBrowsePreviewEffects {
public:
    virtual void startDeferredCoverPriming() = 0;
    virtual void applyCoverKeyFilter() = 0;
    virtual AsyncSearchBrowsePreviewRefreshState readState() const = 0;
    virtual bool readSelectedRecord(
        AsyncSearchBrowsePreviewRecord& record) const = 0;
    virtual std::vector<AsyncSearchBrowsePreviewRecord>
    readRefreshedRecords() const = 0;
};

aura::coverwall::BrowseSurfacePreviewSelection
CaptureAsyncSearchBrowsePreviewRefresh(
    const AsyncSearchBrowsePreviewRefreshEffects& effects);

void HandleAsyncSearchBrowsePreviewRefreshRestore(
    const aura::coverwall::BrowseSurfacePreviewSelection& selection,
    AsyncSearchBrowsePreviewRefreshEffects& effects,
    bool restoreSelection = true);

}  // namespace aura::search
