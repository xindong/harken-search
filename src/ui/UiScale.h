#pragma once

#include "imgui.h"

#include <algorithm>
#include <cmath>

namespace aura::ui::dpi {

inline float SanitizeScale(float scale) noexcept {
    return std::isfinite(scale) && scale > 0.0f ? std::clamp(scale, 0.5f, 4.0f) : 1.0f;
}

inline float UiScale() noexcept {
    return ImGui::GetCurrentContext() ? SanitizeScale(ImGui::GetStyle().FontScaleDpi) : 1.0f;
}

inline float ScalePx(float logical, float scale) noexcept {
    return logical * SanitizeScale(scale);
}

inline float Px(float logical) noexcept {
    return ScalePx(logical, UiScale());
}

} // namespace aura::ui::dpi
