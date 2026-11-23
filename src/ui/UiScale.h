#pragma once

#include "imgui.h"

#include <algorithm>
#include <cmath>

namespace aura::ui::dpi {

inline float SanitizeScale(float scale) noexcept {
    // 8x ceiling: a 300% monitor combined with a 200% interface scale must not be
    // silently clamped, which would distort the layout instead of scaling it.
    return std::isfinite(scale) && scale > 0.0f ? std::clamp(scale, 0.5f, 8.0f) : 1.0f;
}

// Application-level interface scale: the user enlarges or shrinks the whole UI
// without touching Windows scaling or the resolution. It multiplies the monitor
// DPI, so 100% on a 100% monitor and 100% on a 150% monitor stay independent.
inline constexpr int kInterfaceScalePercents[] = {80, 90, 100, 125, 150, 175, 200};

inline int ResolveInterfaceScalePercent(int requested) noexcept {
    const int clamped = std::clamp(requested, 80, 200);
    int best = 100;
    int bestDistance = clamped;
    for (int step : kInterfaceScalePercents) {
        const int distance = std::abs(clamped - step);
        if (distance < bestDistance) { bestDistance = distance; best = step; }
    }
    return best;
}

inline float InterfaceScaleFactor(int percent) noexcept {
    return static_cast<float>(ResolveInterfaceScalePercent(percent)) / 100.0f;
}

inline float& InterfaceScaleSlot() noexcept {
    static float scale = 1.0f;
    return scale;
}

inline void SetInterfaceScalePercent(int percent) noexcept {
    InterfaceScaleSlot() = InterfaceScaleFactor(percent);
}

inline float InterfaceScale() noexcept {
    return SanitizeScale(InterfaceScaleSlot());
}

inline float UiScale() noexcept {
    if (!ImGui::GetCurrentContext()) return 1.0f;
    return SanitizeScale(ImGui::GetStyle().FontScaleDpi * InterfaceScale());
}

inline float ScalePx(float logical, float scale) noexcept {
    return logical * SanitizeScale(scale);
}

inline float UnscalePx(float pixels, float scale) noexcept {
    return pixels / SanitizeScale(scale);
}

inline float Px(float logical) noexcept {
    return ScalePx(logical, UiScale());
}

inline float LogicalPx(float pixels) noexcept {
    return UnscalePx(pixels, UiScale());
}

} // namespace aura::ui::dpi
