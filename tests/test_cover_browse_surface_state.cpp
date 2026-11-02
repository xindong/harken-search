#include "src/CoverBrowseSurfaceState.h"

#include <cstdio>
#include <string>

namespace {

bool Check(bool condition, const char* message) {
    if (!condition) {
        std::printf("FAIL: %s\n", message);
        return false;
    }
    return true;
}

} // namespace

int main() {
    using namespace aura::coverwall;

    if (!Check(ClampTileSizePx(-20) == static_cast<int>(kMinThumbSizePx),
               "tile clamp should respect the shared minimum")) {
        return 1;
    }
    if (!Check(ClampTileSizePx(999) == static_cast<int>(kMaxThumbSizePx),
               "tile clamp should respect the shared maximum")) {
        return 1;
    }
    if (!Check(static_cast<int>(kMinThumbSizePx) == 56,
               "minimum production cover-wall size should allow dense right-panel fit")) {
        return 1;
    }
    const auto compactGrid = CalculateCoverWallGridMetrics(452.0f, kMinThumbSizePx, 4.0f);
    if (!Check(compactGrid.columns == 7 &&
                   compactGrid.thumbSizePx >= kMinThumbSizePx &&
                   compactGrid.thumbSizePx <= 62.0f,
               "minimum cover-wall grid should use the extra right-side column when it fits")) {
        return 1;
    }
    const auto narrowGrid = CalculateCoverWallGridMetrics(120.0f, kMinThumbSizePx, 4.0f);
    if (!Check(narrowGrid.columns >= 1 && narrowGrid.thumbSizePx >= kMinThumbSizePx,
               "cover-wall grid metrics should stay valid in narrow panels")) {
        return 1;
    }
    for (const float width : {740.0f, 980.0f, 1920.0f}) {
        const auto dense = CalculateCoverWallGridMetrics(width, kMinThumbSizePx);
        if (!Check(dense.columns > 8 && dense.thumbSizePx < 62.0f,
                   "minimum-size covers must stay small in a wide top/bottom dock")) return 1;
        const float occupied = dense.columns * dense.thumbSizePx + (dense.columns - 1) * 4.0f;
        if (!Check(occupied <= width + .01f && occupied >= width - .01f,
                   "dense cover rows must still fit the panel without overflow")) return 1;
        const auto enlarged = CalculateCoverWallGridMetrics(width, 132.0f);
        if (!Check(enlarged.columns < dense.columns && enlarged.thumbSizePx > dense.thumbSizePx,
                   "enlarging covers must reduce the column count")) return 1;
    }
    if (!Check(ResolveThumbSizeSliderMaxPx(452.0f) == 452.0f,
               "wide cover-wall panels should extend the slider range to the real fill width")) {
        return 1;
    }
    const auto fillGrid = CalculateCoverWallGridMetrics(452.0f, 999.0f, 4.0f);
    if (!Check(fillGrid.columns == 1 && fillGrid.thumbSizePx == 452.0f,
               "single-column cover wall should fill the available width exactly on wide panels")) {
        return 1;
    }
    if (!Check(PresetTileSizePx(TileSizePreset::Compact) == 80 &&
                   PresetTileSizePx(TileSizePreset::Browse) == 132 &&
                   PresetTileSizePx(TileSizePreset::Inspect) == 208,
               "tile presets should keep the replacement browse sizes stable")) {
        return 1;
    }

    float thumbSize = 110.0f;
    ApplyThumbPreset(TileSizePreset::Inspect, thumbSize);
    if (!Check(thumbSize == kInspectThumbPresetPx,
               "apply preset should update the shared production thumb size")) {
        return 1;
    }

    BrowseSurfaceInfoInput input;
    input.resultCount = 72;
    input.pathCount = 6;
    input.keyword = "footstep";
    input.selectedPath = "Fixture/Foley/Footsteps";
    input.selectedCoverKey = "cover:foley-room";
    input.selectedCoverLabel = "Footstep gravel close";
    input.sourceLabel = "facet";
    input.refreshing = true;
    input.hasStableSnapshot = true;

    const auto info = BuildBrowseSurfaceInfo(input);
    if (!Check(info.statusLabel == "Refreshing (stable snapshot)",
               "stable snapshot refresh should get an explicit status label")) {
        return 1;
    }
    if (!Check(info.coverFilterLabel == "Footstep gravel close",
               "cover filter label should prefer the human-readable selected cover label")) {
        return 1;
    }
    if (!Check(info.topLine.find("Results: 72") != std::string::npos &&
                   info.topLine.find("Paths: 6") != std::string::npos &&
                   info.topLine.find("Status: Refreshing (stable snapshot)") != std::string::npos,
               "top info line should summarize result, path, and status truth")) {
        return 1;
    }
    if (!Check(info.bottomLine.find("Source: facet") != std::string::npos &&
                   info.bottomLine.find("Keyword: footstep") != std::string::npos &&
                   info.bottomLine.find("Path: Fixture/Foley/Footsteps") != std::string::npos,
               "bottom info line should summarize active scope truth")) {
        return 1;
    }

    const auto emptyInfo = BuildBrowseSurfaceInfo(BrowseSurfaceInfoInput{});
    if (!Check(emptyInfo.statusLabel == "Ready" &&
                   emptyInfo.coverFilterLabel == "(none)" &&
                   emptyInfo.bottomLine.find("Path: (all paths)") != std::string::npos,
               "empty browse state should still produce a stable replacement info model")) {
        return 1;
    }

    if (!Check(HasActiveBrowseScope(true, false, false, false) &&
                   HasActiveBrowseScope(false, true, false, false) &&
                   HasActiveBrowseScope(false, false, true, false) &&
                   HasActiveBrowseScope(false, false, false, true) &&
                   !HasActiveBrowseScope(false, false, false, false),
               "browse scope helper should treat folder, cover, keyword, and DB scope as first-class replacement scope")) {
        return 1;
    }

    struct PreviewRow {
        std::int64_t fileId = -1;
        std::string filePath;
        std::string folderPath;
    };

    const std::vector<PreviewRow> rows = {
        {10, "Fixture/Foley/Footsteps/gravel_close.wav", "Fixture/Foley/Footsteps"},
        {11, "Fixture/Foley/Footsteps/wood_porch.wav", "Fixture/Foley/Footsteps"},
        {12, "Fixture/Impacts/Metal/bright_hit.wav", "Fixture/Impacts/Metal"},
    };

    BrowseSurfacePreviewSelection preferredSelection;
    preferredSelection.preferredFilePath = "Fixture/Impacts/Metal/bright_hit.wav";
    preferredSelection.preservedFileId = 10;
    preferredSelection.fallbackToFirstResult = true;
    if (!Check(
            ResolveBrowseSurfacePreviewIndex(
                rows,
                preferredSelection,
                [](const PreviewRow& row) { return row.fileId; },
                [](const PreviewRow& row) -> const std::string& { return row.filePath; },
                [](const PreviewRow& row) -> const std::string& { return row.folderPath; }) == 2,
            "explicit wall-selected preview path should outrank preserved selection")) {
        return 1;
    }

    BrowseSurfacePreviewSelection preservedSelection;
    preservedSelection.preservedFileId = 11;
    preservedSelection.fallbackToFirstResult = true;
    if (!Check(
            ResolveBrowseSurfacePreviewIndex(
                rows,
                preservedSelection,
                [](const PreviewRow& row) { return row.fileId; },
                [](const PreviewRow& row) -> const std::string& { return row.filePath; },
                [](const PreviewRow& row) -> const std::string& { return row.folderPath; }) == 1,
            "existing in-scope preview should be preserved when possible")) {
        return 1;
    }

    BrowseSurfacePreviewSelection folderSelection;
    folderSelection.preferredFolderPath = "Fixture/Impacts/Metal";
    folderSelection.fallbackToFirstResult = true;
    if (!Check(
            ResolveBrowseSurfacePreviewIndex(
                rows,
                folderSelection,
                [](const PreviewRow& row) { return row.fileId; },
                [](const PreviewRow& row) -> const std::string& { return row.filePath; },
                [](const PreviewRow& row) -> const std::string& { return row.folderPath; }) == 2,
            "folder-scope preview should resolve inside the active subtree before falling back")) {
        return 1;
    }

    BrowseSurfacePreviewSelection fallbackSelection;
    fallbackSelection.preservedFileId = 99;
    fallbackSelection.fallbackToFirstResult = true;
    if (!Check(
            ResolveBrowseSurfacePreviewIndex(
                rows,
                fallbackSelection,
                [](const PreviewRow& row) { return row.fileId; },
                [](const PreviewRow& row) -> const std::string& { return row.filePath; },
                [](const PreviewRow& row) -> const std::string& { return row.folderPath; }) == 0,
            "active scope should fall back to the first result when no preserved preview survives")) {
        return 1;
    }

    BrowseSurfacePreviewSelection noFallbackSelection;
    if (!Check(
            ResolveBrowseSurfacePreviewIndex(
                rows,
                noFallbackSelection,
                [](const PreviewRow& row) { return row.fileId; },
                [](const PreviewRow& row) -> const std::string& { return row.filePath; },
                [](const PreviewRow& row) -> const std::string& { return row.folderPath; }) == -1,
            "preview helper should stay unset when there is no scope hint and no fallback request")) {
        return 1;
    }

    std::printf("PASS: cover browse surface helper preserves preset and scope info contracts\n");
    return 0;
}
