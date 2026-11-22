#pragma once

#include "UiScale.h"

namespace aura::ui::transport {

inline float ResolveEqualizerWidth(float preferredWidth, float availableWidth, float scale) noexcept {
    const float minimum = dpi::ScalePx(120.0f, scale);
    const float preferred = dpi::ScalePx(std::clamp(preferredWidth, 120.0f, 1600.0f), scale);
    return std::clamp(availableWidth, minimum, preferred);
}

inline float ReserveEqualizerSlope(float railStart, float railEnd, float textWidth, float scale) noexcept {
    const float fullSlopeEnd = dpi::ScalePx(80.0f, scale) + textWidth;
    return railEnd - fullSlopeEnd >= dpi::ScalePx(12.0f, scale)
        ? (std::max)(railStart, fullSlopeEnd)
        : railStart;
}

} // namespace aura::ui::transport
