#pragma once

// Layout of the three first-library buttons (Create / Open database… / Exit).
//
// Geometry is logical: the height never drops below the design's 32 px, and it
// follows the real line box so a larger system font or a 125%/150% interface
// scale cannot push the glyphs past the bottom edge. Widths are measured from
// the labels, and the row splits into one button per line when the three do not
// fit side by side.

#include "imgui.h"

namespace aura::ui::setup {

inline constexpr float kButtonHeight = 32.0f;
inline constexpr float kExitMinWidth = 72.0f;
inline constexpr float kCancelWidth = 120.0f;

struct ButtonRow {
    ImVec2 create{0.0f, 0.0f};
    ImVec2 open{0.0f, 0.0f};
    ImVec2 exit{0.0f, 0.0f};
    bool stacked = false;
};

inline float ButtonHeight(float scale, float frameHeight) noexcept {
    const float scaled = kButtonHeight * scale;
    return scaled > frameHeight ? scaled : frameHeight;
}

inline ImVec2 MeasuredSize(ImVec2 textSize, float framePaddingX, float height) noexcept {
    return ImVec2(textSize.x + framePaddingX * 2.0f, height);
}

inline ButtonRow ResolveRow(float availableWidth,
                            float spacing,
                            ImVec2 createText,
                            ImVec2 openText,
                            ImVec2 exitText,
                            float framePaddingX,
                            float scale,
                            float frameHeight) noexcept {
    ButtonRow row;
    const float height = ButtonHeight(scale, frameHeight);
    const float minCreate = createText.x + framePaddingX * 2.0f;
    const float minOpen = openText.x + framePaddingX * 2.0f;
    const float scaledExit = kExitMinWidth * scale;
    const float measuredExit = exitText.x + framePaddingX * 2.0f;
    const float minExit = scaledExit > measuredExit ? scaledExit : measuredExit;
    const float needed = minCreate + minOpen + minExit + spacing * 2.0f;
    if (needed <= availableWidth) {
        // Share the surplus so the row still fills the dialog width.
        const float half = (availableWidth - spacing * 2.0f - minExit) * 0.5f;
        row.create = ImVec2(minCreate > half ? minCreate : half, height);
        row.open = ImVec2(minOpen > half ? minOpen : half, height);
        row.exit = ImVec2(minExit, height);
        return row;
    }
    row.stacked = true;
    row.create = row.open = row.exit = ImVec2(availableWidth, height);
    return row;
}

} // namespace aura::ui::setup
