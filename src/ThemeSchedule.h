#pragma once
#include "ThemeScheduleClock.h"
#include "Theme.h"
#include "BuildConfig.h"
#include <string>
#include <vector>
struct AppState;

namespace harken::theme_schedule {
// All runtime calls are UI-thread only. Initialize reads two independent files
// beside the settings file; Tick never performs disk IO.
void Initialize(const std::string& settingsFileUtf8);
bool Ready();
bool ControlsTheme(const AppState& state);
void SetMode(AppState& state, Mode mode);
void OnManualThemeSelected(AppState& state);
std::optional<Period> SelectedPeriod(const AppState& state);
void Tick(AppState& state, bool force = false);
void Invalidate();
Period CurrentPeriod();
const ThemeColorScheme& Palette(Period period);
const std::string& LoadWarning(Period period);
bool SavePalette(Period period, const ThemeColorScheme& scheme, std::string& error);
// Returns whether the caller should draw its existing ordinary-theme controls.
bool RenderSettings(AppState& state);
#if AURA_BUILD_DIAGNOSTICS && ARIS_PREVIEW_ISOLATION
bool SetQaClock(int minuteOfDay); // -1 returns to the actual local clock.
ImVec4 QaAutoToggleBounds(); // Observed screen coordinates from the actual settings checkbox.
struct QaSettingsObservation {
    int frame = 0;
    ImVec4 cards[2]{}, editButtons[2]{}, otherThemes{}, clip{};
    bool selected[2]{}, otherThemesExpanded = false;
    struct Stock { int theme; ImVec4 bounds; };
    std::vector<Stock> stock;
};
const QaSettingsObservation& QaSettingsUi();
void QaRecordStock(int theme);
#endif
}
