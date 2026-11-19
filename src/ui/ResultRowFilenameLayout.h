#pragma once

#include <algorithm>
#include <cmath>

namespace harken::results {

// Wrapped filename line layout for one result row.
struct WrappedFilenameLayout {
    int lines;
    float lineAdvance;
};

// Resolve how many complete filename lines fit inside the row's drawable band.
// The painter uses this layout so a wrapped filename never ends in a clipped
// glyph. When the band is a few pixels short of a whole extra line, the line
// advance is tightened by up to 10% so the line still fits whole instead of
// being cut by the row boundary; larger shortfalls show fewer lines and close
// the last one with an ellipsis.
inline WrappedFilenameLayout ResolveWrappedFilenameLayout(float drawableHeight,
                                                          float naturalAdvance) noexcept {
    const float advance = naturalAdvance > 0.0f ? naturalAdvance : 0.0f;
    if (!(advance > 0.0f) || !(drawableHeight > 0.0f)) {
        return {1, advance};
    }
    const float naturalLines = drawableHeight / advance;
    int lines = (std::max)(1, static_cast<int>(std::floor(naturalLines + 0.5f)));
    if (lines > 1 && drawableHeight / static_cast<float>(lines) < advance * 0.9f) {
        lines = (std::max)(1, static_cast<int>(std::floor(naturalLines)));
    }
    const float lineAdvance = (std::min)(advance, drawableHeight / static_cast<float>(lines));
    return {lines, lineAdvance};
}

}  // namespace harken::results

