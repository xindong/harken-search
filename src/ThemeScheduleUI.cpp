#include "ThemeSchedule.h"
#include "AppState.h"
#include "Localization.h"
#include "HarkenSearchWidgets.h"
#include "WindowsMain.h"
#include "imgui.h"

namespace harken::theme_schedule {
#if AURA_BUILD_DIAGNOSTICS && ARIS_PREVIEW_ISOLATION
static ImVec4 g_autoToggleBounds;
ImVec4 QaAutoToggleBounds() { return g_autoToggleBounds; }
#endif
void RenderSettings(AppState& state) {
    if (!Ready()) return;
    ImGui::Spacing();
    ImGui::TextUnformatted(L(ThemeSchedule_Title));
    bool automatic = state.settings.themeScheduleMode == Mode::Automatic;
    if (ImGui::Checkbox(L(ThemeSchedule_Automatic), &automatic)) {
        SetMode(state, automatic ? Mode::Automatic
            : (CurrentPeriod() == Period::Day ? Mode::FixedDay : Mode::FixedNight));
        SaveAppSettings(state);
    }
#if AURA_BUILD_DIAGNOSTICS && ARIS_PREVIEW_ISOLATION
    const auto lo = ImGui::GetItemRectMin(), hi = ImGui::GetItemRectMax();
    g_autoToggleBounds = ImVec4(lo.x, lo.y, hi.x, hi.y);
#endif
    ImGui::TextWrapped("%s", automatic ? L(ThemeSchedule_Hint) : L(ThemeSchedule_ManualHint));
    if (ImGui::BeginTable("##DayNightPalettes", 5, ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("period", ImGuiTableColumnFlags_WidthFixed, 110.f);
        ImGui::TableSetupColumn("time", ImGuiTableColumnFlags_WidthFixed, 140.f);
        ImGui::TableSetupColumn("preview");
        ImGui::TableSetupColumn("use", ImGuiTableColumnFlags_WidthFixed, 116.f);
        ImGui::TableSetupColumn("edit", ImGuiTableColumnFlags_WidthFixed, 148.f);
        for (const auto period : {Period::Day, Period::Night}) {
            const bool day = period == Period::Day;
            ImGui::PushID(day ? 0 : 1);
            ImGui::TableNextRow(0, 38.f);
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(day ? L(ThemeSchedule_Day) : L(ThemeSchedule_Night));
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(day ? "08:00 - 16:00" : "16:00 - 08:00");
            ImGui::TableNextColumn();
            const auto& palette = Palette(period);
            int index = 0;
            for (const auto color : {palette.mainWindowBG, palette.toolbarAccent, palette.resultsText}) {
                if (index) ImGui::SameLine(0.f, 5.f);
                ImGui::PushID(index++);
                ImGui::ColorButton("##preview", color, ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoDragDrop, ImVec2(24,24));
                ImGui::PopID();
            }
            if (SelectedPeriod(state) == period) {
                ImGui::SameLine();
                ImGui::TextUnformatted(L(Main_CurrentTag));
            }
            ImGui::TableNextColumn();
            if (aura::ui::ActionButton(day ? L(ThemeSchedule_UseDay) : L(ThemeSchedule_UseNight), ImVec2(108,0))) {
                SetMode(state, day ? Mode::FixedDay : Mode::FixedNight);
                SaveAppSettings(state);
            }
            ImGui::TableNextColumn();
            if (aura::ui::ActionButton(day ? L(ThemeSchedule_EditDay) : L(ThemeSchedule_EditNight), ImVec2(140,0))) {
                state.themeScheduleEditorPeriod = day ? 0 : 1;
                state.showThemeEditor = true;
                ImGui::CloseCurrentPopup();
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    for (const auto period : {Period::Day, Period::Night}) {
        if (!LoadWarning(period).empty())
            ImGui::TextWrapped("%s", period == Period::Day ? L(ThemeSchedule_DayFallback) : L(ThemeSchedule_NightFallback));
    }
    ImGui::Spacing();
}
}
