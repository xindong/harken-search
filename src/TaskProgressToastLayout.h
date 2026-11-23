#pragma once

#include "TaskProgressToastModel.h"

#include <cstddef>
#include <string>

// Geometry and hit testing for the bottom-right task progress overlay
// (card REQ-20261123-TASK-PROGRESS-TOAST). This unit stays free of ImGui so the
// layout can be verified without a window; the renderer in main.cpp converts
// these rects and points into draw-list calls.

struct TaskProgressToastPoint {
    float x = 0.0f;
    float y = 0.0f;
};

struct TaskProgressToastHit {
    bool panel = false;
    bool row = false;
    bool close = false;
    std::size_t rowIndex = 0;
    std::string id;
    std::string closeId;
};

// Close target: a 16 px square inset 10 px from the row's right edge and
// vertically centered, so a 52 px row leaves a comfortable 18 px margin.
TaskProgressToastRect ToastCloseButtonRect(const TaskProgressToastStack& stack,
                                           const TaskProgressToastRect& row);

bool ToastPointInRect(TaskProgressToastPoint point,
                      const TaskProgressToastRect& rect);

// Hit-tests the painted rows of the panel. Rows are reported oldest first, so
// rowIndex matches the order handed out by VisibleRows(now).
TaskProgressToastHit HitTestTaskProgressToast(const TaskProgressToastStack& stack,
                                              const TaskProgressToastRect& panel,
                                              TaskProgressToastPoint mouse,
                                              std::size_t rowCount,
                                              double now);

// True for determinate rows; writes "42%" (no trailing spaces) into out.
bool FormatTaskProgressPercent(float fraction, char* out, std::size_t capacity);

// 0..1 sweep for rows whose fraction is negative, so indeterminate work still
// shows motion. Loops every 2 s.
float TaskProgressIndeterminateSweep(double now);