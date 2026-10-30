#pragma once
#include "Theme.h"
#include "ThemeScheduleClock.h"
#include <string>

namespace harken::theme_schedule {
// Versioned full snapshots, including colors not exposed by the legacy editor.
std::string EncodePalette(const ThemeColorScheme& scheme);
bool ValidatePalette(const ThemeColorScheme& scheme, Period period, std::string& error);
// Failure never changes destination. Error codes are localized at the UI boundary.
bool DecodePalette(const std::string& json, Period period, ThemeColorScheme& destination, std::string& error);
}
