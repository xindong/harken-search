#pragma once

#include <optional>

namespace harken::theme_schedule {

enum class Period { Day, Night };

// The caller supplies local wall-clock time. This function never reads the clock.
constexpr std::optional<Period> PeriodAtLocalTime(int hour, int minute) {
    if (hour < 0 || hour > 23 || minute < 0 || minute > 59) {
        return std::nullopt;
    }
    return hour >= 8 && hour < 16 ? Period::Day : Period::Night;
}

struct ApplyInput {
    Period wanted;
    std::optional<Period> applied;
    bool manualThemeSelected;
    bool paletteUpdated;
    bool editorPreviewActive;
};

constexpr bool ShouldApply(const ApplyInput& input) {
    return !input.editorPreviewActive &&
        (!input.applied || *input.applied != input.wanted ||
         input.manualThemeSelected || input.paletteUpdated);
}

} // namespace harken::theme_schedule
