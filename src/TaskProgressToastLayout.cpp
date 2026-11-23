#include "TaskProgressToastLayout.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace {
constexpr float kCloseSize = 16.0f;
constexpr float kCloseInsetRight = 10.0f;
} // namespace

TaskProgressToastRect ToastCloseButtonRect(const TaskProgressToastStack& stack,
                                           const TaskProgressToastRect& row) {
    (void)stack;
    TaskProgressToastRect rect;
    rect.width = kCloseSize;
    rect.height = kCloseSize;
    rect.x = row.x + row.width - kCloseInsetRight - kCloseSize;
    rect.y = row.y + (row.height - kCloseSize) * 0.5f;
    return rect;
}

bool ToastPointInRect(TaskProgressToastPoint point,
                      const TaskProgressToastRect& rect) {
    if (rect.width <= 0.0f || rect.height <= 0.0f) return false;
    return point.x >= rect.x && point.x < rect.x + rect.width &&
           point.y >= rect.y && point.y < rect.y + rect.height;
}

TaskProgressToastHit HitTestTaskProgressToast(const TaskProgressToastStack& stack,
                                              const TaskProgressToastRect& panel,
                                              TaskProgressToastPoint mouse,
                                              std::size_t rowCount,
                                              double now) {
    TaskProgressToastHit hit;
    if (rowCount == 0) return hit;
    if (!ToastPointInRect(mouse, panel)) return hit;

    hit.panel = true;
    const auto rows = stack.VisibleRows(now);
    for (std::size_t index = 0; index < rowCount && index < rows.size(); ++index) {
        const TaskProgressToastRect row = stack.RowRect(panel, index);
        if (!ToastPointInRect(mouse, row)) continue;

        hit.row = true;
        hit.rowIndex = index;
        hit.id = rows[index].id;

        const TaskProgressToastRect close = ToastCloseButtonRect(stack, row);
        if (ToastPointInRect(mouse, close)) {
            hit.close = true;
            hit.closeId = rows[index].id;
        }
        break;
    }
    return hit;
}

bool FormatTaskProgressPercent(float fraction, char* out, std::size_t capacity) {
    if (!out || capacity == 0) return false;
    if (!(fraction >= 0.0f)) return false;
    const float clamped = std::min(1.0f, std::max(0.0f, fraction));
    const int percent = static_cast<int>(std::lround(clamped * 100.0f));
    std::snprintf(out, capacity, "%d%%", percent);
    return true;
}

float TaskProgressIndeterminateSweep(double now) {
    const double period = 2.0;
    if (!(now > 0.0)) return 0.0;
    const double phase = std::fmod(now, period);
    return static_cast<float>(phase < 0.0 ? 0.0 : phase / period);
}