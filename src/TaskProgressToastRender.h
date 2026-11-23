#pragma once

#include "TaskProgressToastLayout.h"

#include "imgui.h"

#include <string>

// ImGui renderer for the bottom-right task progress overlay
// (card REQ-20261123-TASK-PROGRESS-TOAST). The model decides which rows exist
// and where they sit; this unit paints them and reports the row the user
// closed or activated.
//
// Hovering a row reveals its close target. Closing a succeeded or failed row
// dismisses it; closing an active row leaves the source alone and only hides
// the row until the source reports progress again.

struct TaskProgressToastPalette {
    ImU32 panelBg = 0;
    ImU32 border = 0;
    ImU32 title = 0;
    ImU32 phase = 0;
    ImU32 track = 0;
    ImU32 fill = 0;
    ImU32 success = 0;
    ImU32 failed = 0;
    ImU32 hover = 0;
    ImU32 closeGlyph = 0;
    float rounding = 10.0f;
    float titleSize = 14.0f;
    float phaseSize = 12.0f;
};

struct TaskProgressToastRenderResult {
    bool hovered = false;
    std::string dismissedId;
    std::string activatedId;
};

// Paints the overlay inside the given work area. acceptInput is false while a
// modal blocks the window, so the overlay stays visible but inert.
TaskProgressToastRenderResult RenderTaskProgressToast(
    ImDrawList* drawList,
    const TaskProgressToastStack& stack,
    const TaskProgressToastPalette& palette,
    ImVec2 workPos,
    ImVec2 workSize,
    double now,
    bool acceptInput);