#pragma once

#include <optional>

namespace harken::theme_schedule {

enum class Period { Day, Night };
// Persisted values: keep stable across settings versions.
enum class Mode { Off = 0, Automatic = 1, FixedDay = 2, FixedNight = 3 };

constexpr Mode ModeFromSetting(int value) {
    return value >= 0 && value <= 3 ? static_cast<Mode>(value) : Mode::Off;
}

constexpr std::optional<Period> ResolvePeriod(Mode mode, std::optional<Period> clock) {
    switch (mode) {
    case Mode::Automatic: return clock;
    case Mode::FixedDay: return Period::Day;
    case Mode::FixedNight: return Period::Night;
    default: return std::nullopt;
    }
}

constexpr Mode ModeAfterManualTheme(Mode mode) {
    return mode == Mode::Automatic ? mode : Mode::Off;
}

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
