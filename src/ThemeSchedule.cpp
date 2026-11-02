#include "ThemeSchedule.h"
#include "ThemeSchedulePaletteFiles.h"
#include "ThemeSchedulePaletteCodec.h"
#include "BuiltinThemeScheme.h"
#include "AppState.h"
#include "UIRenderer.h"
#include "platform/filesystem.h"
#include <array>
#include <ctime>
#include <cstdlib>
#include <filesystem>

namespace harken::theme_schedule {
namespace {
struct Runtime {
    bool ready = false;
    bool dirty = true;
    bool editorWasOpen = false;
    std::filesystem::path settingsFile;
    std::array<ThemeColorScheme, 2> palettes;
    std::array<std::optional<AppTheme>, 2> builtinThemes;
    std::array<std::string, 2> warnings;
    std::optional<Period> applied;
    std::optional<ManualAppearance> manualAppearance;
#if AURA_BUILD_DIAGNOSTICS && ARIS_PREVIEW_ISOLATION
    int qaMinute = -1;
#endif
} g_runtime;
size_t Index(Period period) { return period == Period::Day ? 0 : 1; }
std::optional<AppTheme> BuiltinThemeForPalette(const ThemeColorScheme& palette) {
    for (const auto theme : {AppTheme::Dark, AppTheme::RoseCedar,
            AppTheme::LapisBlue, AppTheme::Ocean, AppTheme::JadeMist}) {
        if (PalettesEqual(palette, MakeBuiltinScheme(theme))) return theme;
    }
    return std::nullopt;
}
std::optional<Period> ReadPeriod() {
#if AURA_BUILD_DIAGNOSTICS && ARIS_PREVIEW_ISOLATION
    if (g_runtime.qaMinute >= 0) return PeriodAtLocalTime(g_runtime.qaMinute / 60, g_runtime.qaMinute % 60);
#endif
    const auto now = std::time(nullptr);
    std::tm local{};
#ifdef _WIN32
    if (localtime_s(&local, &now) != 0) return std::nullopt;
#else
    if (!localtime_r(&now, &local)) return std::nullopt;
#endif
    return PeriodAtLocalTime(local.tm_hour, local.tm_min);
}
}

void Initialize(const std::string& path) {
    g_runtime = Runtime{};
#if AURA_BUILD_DIAGNOSTICS && ARIS_PREVIEW_ISOLATION
    if (const char* value = std::getenv("HARKEN_THEME_QA_MINUTE")) {
        char* end = nullptr;
        const auto minute = std::strtol(value, &end, 10);
        if (end != value && *end == '\0' && minute >= 0 && minute < 24*60)
            g_runtime.qaMinute = static_cast<int>(minute);
    }
#endif
    g_runtime.settingsFile = std::filesystem::u8path(path);
    g_runtime.palettes = {MakeBuiltinScheme(AppTheme::RoseCedar), MakeBuiltinScheme(AppTheme::Dark)};
    for (const auto period : {Period::Day, Period::Night}) {
        const auto index = Index(period);
        const auto file = PaletteFilePath(g_runtime.settingsFile, period);
        if (Platform::FileSystem::ProbePath(file.u8string()) !=
            Platform::FileSystem::PathStatus::Missing) {
            auto& warning = g_runtime.warnings[index];
            std::string json;
            if (ReadPaletteJson(g_runtime.settingsFile, period, json, warning))
                DecodePalette(json, period, g_runtime.palettes[index], warning);
        }
        g_runtime.builtinThemes[index] = BuiltinThemeForPalette(g_runtime.palettes[index]);
    }
    g_runtime.ready = true;
}
bool Ready() { return g_runtime.ready; }
bool ControlsTheme(const AppState& state) { return Ready() && state.settings.themeScheduleMode != Mode::Off; }
std::optional<ManualAppearance> ManualAppearanceForSettings(const AppState& state) {
    return ControlsTheme(state) ? g_runtime.manualAppearance : std::nullopt;
}
std::optional<Period> SelectedPeriod(const AppState& state) {
    return ResolvePeriod(state.settings.themeScheduleMode, ReadPeriod());
}
void SetMode(AppState& state, Mode mode) {
    const bool wasControlling = ControlsTheme(state);
    if (mode != Mode::Off && !wasControlling)
        g_runtime.manualAppearance = ManualAppearance{state.settings.theme, state.settings.useCustomColors};
    state.settings.themeScheduleMode = mode;
    if (mode == Mode::Off && wasControlling) {
        if (g_runtime.manualAppearance) {
            state.settings.theme = g_runtime.manualAppearance->theme;
            state.settings.useCustomColors = g_runtime.manualAppearance->useCustomColors;
        }
        g_runtime.manualAppearance.reset();
        g_runtime.applied.reset();
        ThemeColors::deactivateCustomTheme();
        ThemeColors::markPendingUiChromeResync();
    }
    Invalidate();
}
void OnManualThemeSelected(AppState& state) {
    if (ControlsTheme(state))
        g_runtime.manualAppearance = ManualAppearance{state.settings.theme, state.settings.useCustomColors};
    SetMode(state, ModeAfterManualTheme(state.settings.themeScheduleMode));
}
void Invalidate() { g_runtime.dirty = true; }
Period CurrentPeriod() { return ReadPeriod().value_or(g_runtime.applied.value_or(Period::Night)); }
const ThemeColorScheme& Palette(Period period) { return g_runtime.palettes[Index(period)]; }
const std::string& LoadWarning(Period period) { return g_runtime.warnings[Index(period)]; }

void Tick(AppState& state, bool force) {
    if (!Ready()) return;
    const bool editorClosed = g_runtime.editorWasOpen && !state.showThemeEditor;
    g_runtime.editorWasOpen = state.showThemeEditor;
    const auto wanted = SelectedPeriod(state);
    if (!wanted) {
        g_runtime.applied.reset();
        return;
    }
    if (!g_runtime.manualAppearance)
        g_runtime.manualAppearance = ManualAppearance{state.settings.theme, state.settings.useCustomColors};
    const auto builtin = g_runtime.builtinThemes[Index(*wanted)];
    const auto selectedTheme = builtin.value_or(AppTheme::Custom);
    if (!ShouldApply({*wanted, g_runtime.applied,
            state.settings.theme != selectedTheme || state.settings.useCustomColors ||
                (builtin ? ThemeColors::customized() : !ThemeColors::customized()),
            force || g_runtime.dirty || editorClosed, state.showThemeEditor})) return;
    state.settings.theme = selectedTheme;
    state.settings.useCustomColors = false;
    if (builtin) {
        ApplyTheme(*builtin);
    } else {
        ThemeColors::activateCustomScheme(Palette(*wanted));
        ThemeColors::applyToImGui();
    }
    ThemeColors::markPendingUiChromeResync();
    g_runtime.applied = wanted;
    g_runtime.dirty = false;
}

bool SavePalette(Period period, const ThemeColorScheme& scheme, std::string& error) {
    if (!ValidatePalette(scheme, period, error)) return false;
    if (!SavePaletteJson(g_runtime.settingsFile, period, EncodePalette(scheme), error)) return false;
    g_runtime.palettes[Index(period)] = scheme;
    g_runtime.builtinThemes[Index(period)] = BuiltinThemeForPalette(scheme);
    g_runtime.warnings[Index(period)].clear();
    Invalidate();
    return true;
}
#if AURA_BUILD_DIAGNOSTICS && ARIS_PREVIEW_ISOLATION
bool SetQaClock(int minuteOfDay) {
    if (minuteOfDay < -1 || minuteOfDay >= 24*60) return false;
    g_runtime.qaMinute = minuteOfDay;
    return true;
}
#endif
}
