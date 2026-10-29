#include "ThemeScheduleClock.h"

#include <array>
#include <cstdio>
#include <optional>

using harken::theme_schedule::ApplyInput;
using harken::theme_schedule::Period;
using harken::theme_schedule::PeriodAtLocalTime;
using harken::theme_schedule::ShouldApply;

namespace {
int failures = 0;
void Check(bool ok, const char* name) {
    if (!ok) {
        ++failures;
        std::printf("FAIL %s\n", name);
    }
}
}

int main() {
    struct Case { int hour; int minute; Period expected; const char* name; };
    constexpr std::array<Case, 8> cases{{
        {0, 0, Period::Night, "midnight"},
        {7, 59, Period::Night, "one minute before day"},
        {8, 0, Period::Day, "day starts"},
        {12, 0, Period::Day, "noon"},
        {15, 59, Period::Day, "one minute before night"},
        {16, 0, Period::Night, "night starts"},
        {23, 59, Period::Night, "one minute before midnight"},
        {3, 15, Period::Night, "early morning"},
    }};
    for (const Case& c : cases) {
        Check(PeriodAtLocalTime(c.hour, c.minute) == c.expected, c.name);
    }
    Check(!PeriodAtLocalTime(-1, 0).has_value(), "negative hour rejected");
    Check(!PeriodAtLocalTime(24, 0).has_value(), "hour 24 rejected");
    Check(!PeriodAtLocalTime(8, 60).has_value(), "minute 60 rejected");
    Check(!PeriodAtLocalTime(8, -1).has_value(), "negative minute rejected");

    Check(ShouldApply({Period::Day, std::nullopt, false, false, false}), "first application");
    Check(!ShouldApply({Period::Day, Period::Day, false, false, false}), "unchanged frame");
    Check(ShouldApply({Period::Night, Period::Day, false, false, false}), "crossing to night");
    Check(ShouldApply({Period::Day, Period::Night, false, false, false}), "clock moved back to day");
    Check(ShouldApply({Period::Day, Period::Day, true, false, false}), "manual selection restores current period");
    Check(ShouldApply({Period::Night, Period::Night, false, true, false}), "edited palette reapplies");
    Check(!ShouldApply({Period::Night, Period::Day, true, true, true}), "editor preview is not overwritten");
    Check(ShouldApply({Period::Night, Period::Day, true, true, false}), "closing editor resumes rule");

    int changes = 0;
    std::optional<Period> applied;
    for (int minuteOfDay = 0; minuteOfDay < 24 * 60; ++minuteOfDay) {
        const Period wanted = *PeriodAtLocalTime(minuteOfDay / 60, minuteOfDay % 60);
        if (ShouldApply({wanted, applied, false, false, false})) {
            applied = wanted;
            ++changes;
        }
    }
    Check(changes == 3, "one initial application and exactly two changes per day");
    std::printf("theme schedule clock: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
