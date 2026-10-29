#pragma once
#include "ThemeScheduleClock.h"
#include "Theme.h"
#include "BuildConfig.h"
#include <string>
struct AppState;

namespace harken::theme_schedule {
// All runtime calls are UI-thread only. Initialize reads two independent files
// beside the settings file; Tick never performs disk IO.
void Initialize(const std::string& settingsFileUtf8);
bool Ready();
void Tick(AppState& state, bool force = false);
void Invalidate();
Period CurrentPeriod();
const ThemeColorScheme& Palette(Period period);
const std::string& LoadWarning(Period period);
bool SavePalette(Period period, const ThemeColorScheme& scheme, std::string& error);
void RenderSettings(AppState& state);
#if AURA_BUILD_DIAGNOSTICS && ARIS_PREVIEW_ISOLATION
bool SetQaClock(int minuteOfDay); // -1 returns to the actual local clock.
#endif
}
