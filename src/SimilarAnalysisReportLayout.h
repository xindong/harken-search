#pragma once
// Geometry for the Similar Sounds "analysis report" modal.
//
// The dialog used to open at 660x380 with no persistence, so the failure list
// lived in a ~200 px strip and any resize the user made was lost on the next
// open. These helpers keep the persisted size usable on the current window and
// keep the whole dialog inside the main window's work area.

#include <algorithm>
#include <cmath>

namespace HarkenSearch::similar_analysis_report {

inline constexpr float kDefaultWidth = 1180.0f;
inline constexpr float kDefaultHeight = 780.0f;
inline constexpr float kMinimumWidth = 560.0f;
inline constexpr float kMinimumHeight = 320.0f;
inline constexpr float kWorkAreaMargin = 8.0f;

// Dialog chrome outside the failure list. The values mirror the caption strip
// (kHeightLg), the modality's own vertical padding (kGap16), the gap between
// the caption and the body (kGap12) and the footer strip holding the close
// button. Keeping them here lets the QA receipt report a real list height.
inline constexpr float kCaptionStripHeight = 36.0f;
inline constexpr float kWindowPaddingY = 16.0f;
inline constexpr float kCaptionToBodyGap = 12.0f;
inline constexpr float kFooterStripHeight = 42.0f;

struct Rect {
    float x = 0.0f;
    float y = 0.0f;
    float w = 0.0f;
    float h = 0.0f;
};

inline float StoredOrDefault(float value, float fallback) {
    return std::isfinite(value) && value > 0.0f ? value : fallback;
}

inline float ClampSizeAxis(float value, float fallback, float minimum, float workExtent) {
    const float usable = workExtent - kWorkAreaMargin * 2.0f;
    const float maximum = usable > minimum ? usable : minimum;
    const float lower = minimum < maximum ? minimum : maximum;
    return std::clamp(StoredOrDefault(value, fallback), lower, maximum);
}

// A size remembered on a larger window must not overflow the current one.
inline Rect ClampSize(const Rect& rect, const Rect& work) {
    Rect out = rect;
    out.w = ClampSizeAxis(rect.w, kDefaultWidth, kMinimumWidth, work.w);
    out.h = ClampSizeAxis(rect.h, kDefaultHeight, kMinimumHeight, work.h);
    return out;
}

// ImGui only keeps a strip of the title bar reachable, so a dialog dragged
// past the edge comes back mostly invisible. A dialog that already sits inside
// the work area keeps its position, so the caption bar never fights a drag.
inline float ClampPositionAxis(float position, float size, float workStart, float workExtent) {
    if (!std::isfinite(position)) return workStart + kWorkAreaMargin;
    const float lower = workStart + kWorkAreaMargin;
    const float upper = workStart + workExtent - kWorkAreaMargin - size;
    if (upper <= lower) return lower;
    return std::clamp(position, lower, upper);
}

inline Rect ClampPosition(const Rect& rect, const Rect& work) {
    Rect out = rect;
    out.x = ClampPositionAxis(rect.x, rect.w, work.x, work.w);
    out.y = ClampPositionAxis(rect.y, rect.h, work.y, work.h);
    return out;
}

inline Rect ClampToWorkArea(const Rect& rect, const Rect& work) {
    return ClampPosition(ClampSize(rect, work), work);
}

// One failure reads as one row: a single text line plus the row gap.
inline float FailureRowHeight(float textLineHeight, float itemSpacingY) {
    const float line = StoredOrDefault(textLineHeight, 0.0f);
    const float gap = itemSpacingY > 0.0f ? itemSpacingY : 0.0f;
    return line + gap;
}

#if ARIS_PREVIEW_ISOLATION
// QA-only: the row pitch the dialog actually used. The geometry receipt runs
// outside the dialog's pushed ItemSpacing, so it cannot recompute this.
inline float g_qaFailureRowHeight = 0.0f;
#endif

inline float FailureListHeight(float dialogHeight) {
    const float height = StoredOrDefault(dialogHeight, kDefaultHeight) - kCaptionStripHeight -
        kCaptionToBodyGap - kWindowPaddingY * 2.0f - kFooterStripHeight;
    return height > 0.0f ? height : 0.0f;
}

} // namespace HarkenSearch::similar_analysis_report
