#pragma once

#include "CoreWorkspaceStyle.h"

#include <algorithm>
#include <cmath>

namespace harken::results {

inline constexpr float kContentInsetX = aura::ui::workspace::kCoreWorkspaceMetrics.space8;

inline constexpr const char* TableName(bool browser) noexcept {
    return browser ? "BrowserFileTableV1" : "FileTableV6";
}

inline constexpr ImGuiTableFlags TableOuterFlags(bool emptyLibrary) noexcept {
    return emptyLibrary ? ImGuiTableFlags_PadOuterX : ImGuiTableFlags_BordersOuter;
}

inline void AlignBrowserPathLabel(bool emptyLibrary) {
    const float tableBorder = emptyLibrary ? 0.0f : 1.0f;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetStyle().CellPadding.x + tableBorder);
    ImGui::AlignTextToFramePadding();
}

struct BrowserSearchRowLayout {
    bool searchBesideControls;
    bool sourceBesideSearch;
    float searchWidth;
    float sourceWidth;
};

inline BrowserSearchRowLayout LayoutBrowserSearchRow(
    float fullWidth, float inlineWidth, float sourceTextWidth, float spacing) noexcept {
    constexpr float minSearchWidth = 110.0f;
    const float minSourceWidth = (std::min)(36.0f, sourceTextWidth);
    const float inlineBudget = minSearchWidth + (minSourceWidth > 0.0f ? spacing + minSourceWidth : 0.0f);
    const bool searchBesideControls = inlineWidth >= inlineBudget;
    const float rowWidth = searchBesideControls ? inlineWidth : fullWidth;
    const float sourceRoom = std::floor(rowWidth - minSearchWidth - spacing);
    const bool sourceBesideSearch = minSourceWidth > 0.0f && sourceRoom >= minSourceWidth;
    const float sourceWidth = (std::min)(sourceTextWidth,
        sourceBesideSearch ? sourceRoom : (std::max)(0.0f, fullWidth));
    const float searchWidth = (std::max)(1.0f, std::floor(
        rowWidth - (sourceBesideSearch ? spacing + sourceWidth : 0.0f)));
    return {searchBesideControls, sourceBesideSearch, searchWidth, sourceWidth};
}

struct BrowserColumnDefaults {
    float fileName;
    float waveform;
};

inline BrowserColumnDefaults ResolveBrowserColumnDefaults(
    float contentWidth, float fixedVisibleWidth) noexcept {
    constexpr float minFileName = 240.0f;
    constexpr float minWaveform = 160.0f;
    constexpr float maxWaveform = 300.0f;
    const float flexibleWidth = (std::max)(minFileName + minWaveform,
        std::floor(contentWidth - fixedVisibleWidth));
    const float waveform = std::clamp(std::floor(flexibleWidth * 0.38f), minWaveform, maxWaveform);
    return {flexibleWidth - waveform, waveform};
}

}
