#include "ThemeSchedule.h"
#include "ThemeSchedulePaletteCodec.h"
#include "BuiltinThemeScheme.h"
#include "AppState.h"
#include "HarkenSearchWidgets.h"
#include "Localization.h"
#ifdef _WIN32
#include "WindowsMain.h"
#endif
#include "imgui.h"
#include <algorithm>
#include <array>
#include <cstdio>

namespace harken::theme_schedule {
namespace {
#if AURA_BUILD_DIAGNOSTICS && ARIS_PREVIEW_ISOLATION
ImVec4 g_autoToggleBounds;
QaSettingsObservation g_ui;
#endif
std::array<std::string, 2> g_saveErrors;
ImVec4 ItemBounds() {
    const auto lo = ImGui::GetItemRectMin(), hi = ImGui::GetItemRectMax();
    return ImVec4(lo.x, lo.y, hi.x, hi.y);
}
struct Preset {
    AppTheme theme;
    LStr label;
    ThemeColorScheme scheme;
};
const std::array<Preset, 5>& Presets() {
    static const std::array<Preset, 5> presets{{
        {AppTheme::Dark, LStr::Set_ThemeDark, MakeBuiltinScheme(AppTheme::Dark)},
        {AppTheme::RoseCedar, LStr::Set_ThemeRoseCedar, MakeBuiltinScheme(AppTheme::RoseCedar)},
        {AppTheme::LapisBlue, LStr::Set_ThemeLapisBlue, MakeBuiltinScheme(AppTheme::LapisBlue)},
        {AppTheme::Ocean, LStr::Set_ThemeOcean, MakeBuiltinScheme(AppTheme::Ocean)},
        {AppTheme::JadeMist, LStr::Set_ThemeJadeMist, MakeBuiltinScheme(AppTheme::JadeMist)}
    }};
    return presets;
}
const Preset* MatchingPreset(Period period) {
    for (const auto& preset : Presets()) {
        if (PalettesEqual(Palette(period), preset.scheme)) return &preset;
    }
    return nullptr;
}
bool PaletteSelector(Period period, float width) {
    const int index = period == Period::Day ? 0 : 1;
    const auto* preset = MatchingPreset(period);
    char preview[256];
    std::snprintf(preview, sizeof(preview), period == Period::Day
        ? L(ThemeSchedule_DayThemeFmt) : L(ThemeSchedule_NightThemeFmt),
        preset ? GetLocalizedString(preset->label) : L(Set_ThemeCustom));
    const auto& colors = ThemeColors::current();
    ImGui::PushStyleColor(ImGuiCol_FrameBg, colors.frameBgBase);
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, colors.frameBgBase);
    ImGui::PushStyleColor(ImGuiCol_Border, colors.chromeBorder);
    ImGui::PushStyleColor(ImGuiCol_PopupBg, ThemeColors::popupComboBG(colors));
    ImGui::PushStyleColor(ImGuiCol_Text, colors.menuText);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 5.f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(12.f, 8.f));
    ImGui::SetNextItemWidth(width);
#if AURA_BUILD_DIAGNOSTICS && ARIS_PREVIEW_ISOLATION
    const auto origin = ImGui::GetCursorScreenPos();
    g_ui.selectorButtons[index] = ImVec4(origin.x, origin.y, origin.x + width, origin.y + ImGui::GetFrameHeight());
    g_ui.selectorPresets[index] = static_cast<int>(preset ? preset->theme : AppTheme::Custom);
#endif
    bool edit = false;
    if (ImGui::BeginCombo("##DayNightPreset", preview)) {
        for (const auto& option : Presets()) {
            const bool selected = preset == &option;
            if (ImGui::Selectable(GetLocalizedString(option.label), selected)) {
                SavePalette(period, option.scheme, g_saveErrors[index]);
            }
#if AURA_BUILD_DIAGNOSTICS && ARIS_PREVIEW_ISOLATION
            g_ui.presetOptions.push_back({index, static_cast<int>(option.theme), ItemBounds()});
#endif
            if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::Separator();
        edit = ImGui::MenuItem(L(ThemeSchedule_EditPalette));
#if AURA_BUILD_DIAGNOSTICS && ARIS_PREVIEW_ISOLATION
        g_ui.editButtons[index] = ItemBounds();
#endif
        ImGui::EndCombo();
    }
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(5);
    return edit;
}
}
#if AURA_BUILD_DIAGNOSTICS && ARIS_PREVIEW_ISOLATION
ImVec4 QaAutoToggleBounds() { return g_autoToggleBounds; }
const QaSettingsObservation& QaSettingsUi() { return g_ui; }
void QaRecordStock(int theme) { g_ui.stock.push_back({theme,ItemBounds()}); }
#endif

bool RenderSettings(AppState& state) {
    if (!Ready()) return true;
#if AURA_BUILD_DIAGNOSTICS && ARIS_PREVIEW_ISOLATION
    g_ui = {};
    g_ui.frame = ImGui::GetFrameCount();
    const auto clipMin = ImGui::GetWindowDrawList()->GetClipRectMin();
    const auto clipMax = ImGui::GetWindowDrawList()->GetClipRectMax();
    g_ui.clip = ImVec4(clipMin.x,clipMin.y,clipMax.x,clipMax.y);
#endif
    bool automatic = state.settings.themeScheduleMode == Mode::Automatic;
    if (aura::ui::Checkbox(L(ThemeSchedule_Automatic), &automatic)) {
        // Turning off keeps the displayed palette stable until an ordinary
        // theme is selected. Existing saved fixed modes stay compatible.
        SetMode(state, automatic ? Mode::Automatic
            : (CurrentPeriod() == Period::Day ? Mode::FixedDay : Mode::FixedNight));
#ifdef _WIN32
        SaveAppSettings(state);
#endif
    }
#if AURA_BUILD_DIAGNOSTICS && ARIS_PREVIEW_ISOLATION
    g_autoToggleBounds = ItemBounds();
    g_ui.autoCheckbox = true;
#endif
    if (ImGui::IsItemHovered()) aura::ui::Tooltip(L(ThemeSchedule_Hint));
    ImGui::Dummy(ImVec2(0, 12.f));
    // Exactly one group is drawn. The caller owns the original five themes.
    if (!automatic) return true;

    const float contentWidth = std::min(ImGui::GetContentRegionAvail().x,
        ImGui::GetWindowDrawList()->GetClipRectMax().x - ImGui::GetCursorScreenPos().x - 1.f);
    const float gap = 16.f;
    const float width = std::max(80.f, (contentWidth - gap) * .5f);
    int editPeriod = -1;
    for (int i = 0; i < 2; ++i) {
        if (i) ImGui::SameLine(0.f, gap);
        ImGui::PushID(i);
        if (PaletteSelector(i == 0 ? Period::Day : Period::Night, width)) editPeriod = i;
        ImGui::PopID();
    }
    for (int i = 0; i < 2; ++i) {
        const auto period = i == 0 ? Period::Day : Period::Night;
        if (!g_saveErrors[i].empty()) ImGui::TextWrapped("%s", L(ThemeSchedule_SaveFailed));
        if (!LoadWarning(period).empty())
            ImGui::TextWrapped("%s", i == 0 ? L(ThemeSchedule_DayFallback) : L(ThemeSchedule_NightFallback));
    }
    if (editPeriod >= 0) {
        g_saveErrors[editPeriod].clear();
        state.themeScheduleEditorPeriod = editPeriod;
        state.showThemeEditor = true;
        ImGui::CloseCurrentPopup();
    }
    ImGui::Dummy(ImVec2(0, 12.f));
    return false;
}
}
