#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

namespace aura::coverwall {

// @stage7-contract
// API contract: cover-browse state and presets are value-only UI data. Geometry
// and sizing constants preserve pixel units; callers own snapshots and perform
// texture/database work outside this state surface.

enum class TileSizePreset {
    Compact,
    Browse,
    Inspect,
};

inline constexpr float kMinThumbSizePx = 56.0f;
inline constexpr float kMaxThumbSizePx = 300.0f;
inline constexpr float kCompactThumbPresetPx = 80.0f;
inline constexpr float kBrowseThumbPresetPx = 132.0f;
inline constexpr float kInspectThumbPresetPx = 208.0f;

struct BrowseSurfaceInfoInput {
    std::int64_t resultCount = 0;
    std::size_t pathCount = 0;
    std::string keyword;
    std::string selectedPath;
    std::string selectedCoverKey;
    std::string selectedCoverLabel;
    std::string sourceLabel;
    bool refreshing = false;
    bool hasStableSnapshot = false;
    bool selectionMode = false;
};

struct BrowseSurfaceInfo {
    std::string topLine;
    std::string bottomLine;
    std::string statusLabel;
    std::string coverFilterLabel;
};

struct BrowseSurfacePreviewSelection {
    std::int64_t preferredFileId = -1;
    std::string preferredFilePath;
    std::string preferredFolderPath;
    std::int64_t preservedFileId = -1;
    std::string preservedFilePath;
    bool fallbackToFirstResult = false;
};

inline constexpr float ClampThumbSize(
    float requestedPx,
    float minPx = kMinThumbSizePx,
    float maxPx = kMaxThumbSizePx) {
    return requestedPx < minPx ? minPx : (requestedPx > maxPx ? maxPx : requestedPx);
}

inline constexpr float ResolveThumbSizeSliderMaxPx(
    float availableWidthPx,
    float minPx = kMinThumbSizePx,
    float baseMaxPx = kMaxThumbSizePx) {
    const float safeWidth = availableWidthPx < minPx ? minPx : availableWidthPx;
    return safeWidth > baseMaxPx ? safeWidth : baseMaxPx;
}

inline constexpr int ClampTileSizePx(
    int requestedPx,
    int minPx = static_cast<int>(kMinThumbSizePx),
    int maxPx = static_cast<int>(kMaxThumbSizePx)) {
    return requestedPx < minPx ? minPx : (requestedPx > maxPx ? maxPx : requestedPx);
}

struct CoverWallGridMetrics {
    int columns = 1;
    float thumbSizePx = kMinThumbSizePx;
    float cellHeightPx = kMinThumbSizePx;
};

inline CoverWallGridMetrics CalculateCoverWallGridMetrics(
    float availableWidthPx,
    float requestedThumbSizePx,
    float gapPx = 4.0f,
    float minPx = kMinThumbSizePx,
    float maxPx = kMaxThumbSizePx) {
    const float safeWidth = std::max(1.0f, availableWidthPx);
    const float safeGap = std::max(0.0f, gapPx);
    const float effectiveMaxPx = ResolveThumbSizeSliderMaxPx(safeWidth, minPx, maxPx);
    const float requested = ClampThumbSize(requestedThumbSizePx, minPx, effectiveMaxPx);
    int columns = static_cast<int>((safeWidth + safeGap) / (requested + safeGap));
    columns = std::max(1, columns);

    float fittedThumb = (safeWidth - safeGap * static_cast<float>(columns - 1)) /
                        static_cast<float>(columns);
    if (fittedThumb < minPx && columns > 1) {
        columns = static_cast<int>((safeWidth + safeGap) / (minPx + safeGap));
        columns = std::max(1, columns);
        fittedThumb = (safeWidth - safeGap * static_cast<float>(columns - 1)) /
                      static_cast<float>(columns);
    }

    CoverWallGridMetrics metrics;
    metrics.columns = columns;
    metrics.thumbSizePx = ClampThumbSize(fittedThumb, minPx, effectiveMaxPx);
    metrics.cellHeightPx = metrics.thumbSizePx + safeGap;
    return metrics;
}

inline constexpr float PresetThumbSizePx(TileSizePreset preset) {
    switch (preset) {
    case TileSizePreset::Compact:
        return kCompactThumbPresetPx;
    case TileSizePreset::Browse:
        return kBrowseThumbPresetPx;
    case TileSizePreset::Inspect:
        return kInspectThumbPresetPx;
    }
    return kBrowseThumbPresetPx;
}

inline constexpr int PresetTileSizePx(TileSizePreset preset) {
    return static_cast<int>(PresetThumbSizePx(preset));
}

inline float ApplyThumbPreset(TileSizePreset preset, float& thumbSizePx) {
    thumbSizePx = PresetThumbSizePx(preset);
    return thumbSizePx;
}

inline int ApplyTileSizePreset(
    TileSizePreset preset,
    int& tileSizePx,
    int minPx = static_cast<int>(kMinThumbSizePx),
    int maxPx = static_cast<int>(kMaxThumbSizePx)) {
    tileSizePx = ClampTileSizePx(PresetTileSizePx(preset), minPx, maxPx);
    return tileSizePx;
}

inline BrowseSurfaceInfo BuildBrowseSurfaceInfo(const BrowseSurfaceInfoInput& input) {
    BrowseSurfaceInfo info;
    info.coverFilterLabel = input.selectedCoverLabel.empty()
        ? (input.selectedCoverKey.empty() ? "(none)" : input.selectedCoverKey)
        : input.selectedCoverLabel;
    if (input.refreshing) {
        info.statusLabel = input.hasStableSnapshot
            ? "Refreshing (stable snapshot)"
            : "Refreshing";
    } else {
        info.statusLabel = "Ready";
    }

    std::ostringstream top;
    top << "Results: " << input.resultCount
        << "  Paths: " << input.pathCount
        << "  Cover filter: " << info.coverFilterLabel
        << "  Status: " << info.statusLabel;
    info.topLine = top.str();

    const std::string sourceLabel = input.sourceLabel.empty()
        ? (input.selectionMode ? "selection" : "browse")
        : input.sourceLabel;
    const std::string keywordLabel = input.keyword.empty() ? "(none)" : input.keyword;
    const std::string pathLabel = input.selectedPath.empty() ? "(all paths)" : input.selectedPath;

    std::ostringstream bottom;
    bottom << "Source: " << sourceLabel
           << "  Keyword: " << keywordLabel
           << "  Path: " << pathLabel;
    info.bottomLine = bottom.str();
    return info;
}

inline bool HasActiveBrowseScope(
    bool folderFilterActive,
    bool coverFilterActive,
    bool hasKeywordFilter,
    bool hasAdvancedDbScope) {
    return folderFilterActive ||
           coverFilterActive ||
           hasKeywordFilter ||
           hasAdvancedDbScope;
}

template <typename Row, typename GetId, typename GetPath, typename GetFolderPath>
int ResolveBrowseSurfacePreviewIndex(
    const std::vector<Row>& rows,
    const BrowseSurfacePreviewSelection& selection,
    GetId getId,
    GetPath getPath,
    GetFolderPath getFolderPath) {
    auto findByPath = [&](const std::string& path) -> int {
        if (path.empty()) {
            return -1;
        }
        for (std::size_t i = 0; i < rows.size(); ++i) {
            if (getPath(rows[i]) == path) {
                return static_cast<int>(i);
            }
        }
        return -1;
    };

    auto findByFolder = [&](const std::string& folderPath) -> int {
        if (folderPath.empty()) {
            return -1;
        }
        for (std::size_t i = 0; i < rows.size(); ++i) {
            if (getFolderPath(rows[i]) == folderPath) {
                return static_cast<int>(i);
            }
        }
        return -1;
    };

    auto findById = [&](std::int64_t fileId) -> int {
        if (fileId <= 0) {
            return -1;
        }
        for (std::size_t i = 0; i < rows.size(); ++i) {
            if (getId(rows[i]) == fileId) {
                return static_cast<int>(i);
            }
        }
        return -1;
    };

    if (const int preferredPathIndex = findByPath(selection.preferredFilePath);
        preferredPathIndex >= 0) {
        return preferredPathIndex;
    }
    if (const int preferredIdIndex = findById(selection.preferredFileId);
        preferredIdIndex >= 0) {
        return preferredIdIndex;
    }
    if (const int preservedPathIndex = findByPath(selection.preservedFilePath);
        preservedPathIndex >= 0) {
        return preservedPathIndex;
    }
    if (const int preservedIdIndex = findById(selection.preservedFileId);
        preservedIdIndex >= 0) {
        return preservedIdIndex;
    }
    if (const int preferredFolderIndex = findByFolder(selection.preferredFolderPath);
        preferredFolderIndex >= 0) {
        return preferredFolderIndex;
    }
    return (selection.fallbackToFirstResult && !rows.empty()) ? 0 : -1;
}

}
