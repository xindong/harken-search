#pragma once

#include "AsyncSearchBrowsePreviewRefreshWorkflow.h"

#include <functional>
#include <string>

struct AppState;

namespace aura::search {

class AsyncSearchBrowsePreviewRefreshStateAdapter final {
public:
    using StartDeferredCoverPriming = std::function<void()>;
    using ApplyCoverKeyFilter = std::function<void(AppState&)>;
    using ReadIntegratedMode = std::function<bool(const AppState&)>;
    using ReadActiveDatabaseFilter = std::function<bool(const AppState&)>;
    using CanonicalizeFolderPath =
        std::function<std::string(const std::string&)>;

    AsyncSearchBrowsePreviewRefreshStateAdapter(
        AppState& state,
        StartDeferredCoverPriming startDeferredCoverPriming,
        ApplyCoverKeyFilter applyCoverKeyFilter,
        ReadIntegratedMode readIntegratedMode,
        ReadActiveDatabaseFilter readActiveDatabaseFilter,
        CanonicalizeFolderPath canonicalizeFolderPath);

    aura::coverwall::BrowseSurfacePreviewSelection capture() const;
    void restore(
        const aura::coverwall::BrowseSurfacePreviewSelection& selection,
        bool restoreSelection = true);

private:
    AppState& state_;
    StartDeferredCoverPriming startDeferredCoverPriming_;
    ApplyCoverKeyFilter applyCoverKeyFilter_;
    ReadIntegratedMode readIntegratedMode_;
    ReadActiveDatabaseFilter readActiveDatabaseFilter_;
    CanonicalizeFolderPath canonicalizeFolderPath_;
};

}  // namespace aura::search
