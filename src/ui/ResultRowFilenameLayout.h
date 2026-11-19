#pragma once

#include <algorithm>
#include <cmath>

namespace harken::results {

// Complete filename lines that fit inside the row's drawable band at the
// natural line advance. Result rows are raised to three whole lines when names
// wrap, so this only bounds names that need more lines than the row can show.
inline int ResolveWrappedFilenameLineCount(float drawableHeight, float lineAdvance) noexcept {
    if (!(lineAdvance > 0.0f) || !(drawableHeight > 0.0f)) return 1;
    return (std::max)(1, static_cast<int>(std::floor((drawableHeight + 0.5f) / lineAdvance)));
}

}  // namespace harken::results

