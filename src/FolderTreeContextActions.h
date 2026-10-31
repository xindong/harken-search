#pragma once

#include <string>
#include "PathUtils.h"

// @stage7-contract
// API contract: folder-tree context helpers synchronously mutate only the
// caller-owned state fields required by their template contract. Path/display
// strings are copied; empty paths and unchanged duplicates are no-ops.
// Re-bookmarking an existing path updates its destination mode. No state references
// are retained after return.

template <typename TState>
inline bool AddFolderShortcutIfMissing(TState& state,
                                       const std::string& displayName,
                                       const std::string& path,
                                       bool openInBrowser = false) {
    if (path.empty()) return false;
    const std::string pathKey = HarkenSearch::CanonicalFolderPath(path);
    for (auto& shortcut : state.folderShortcuts) {
        if (HarkenSearch::CanonicalFolderPath(shortcut.path) != pathKey) continue;
        if (shortcut.openInBrowser == openInBrowser) return false;
        shortcut.openInBrowser = openInBrowser;
        return true;
    }
    state.folderShortcuts.push_back({displayName, path, openInBrowser});
    return true;
}

template <typename TState>
inline void PrepareQuickScanDialogForPath(TState& state,
                                          const std::string& path,
                                          bool autoStart = false) {
    if (path.empty() || state.isScanning.load()) return;
    state.pendingScanFolders.clear();
    state.pendingScanFolders.push_back(path);
    state.scanDialogAutoStart = autoStart;
    state.showScanDialog = true;
}
