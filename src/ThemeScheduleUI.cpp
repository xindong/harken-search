#include "ThemeSchedule.h"
#include "AppState.h"
#include "Localization.h"
#include "WindowsMain.h"
#include "imgui.h"
#include <algorithm>

namespace harken::theme_schedule {
namespace {
bool g_otherThemesExpanded = false;
ImVec4 ItemBounds() {
    const auto lo = ImGui::GetItemRectMin(), hi = ImGui::GetItemRectMax();
    return ImVec4(lo.x, lo.y, hi.x, hi.y);
}
bool AutomaticSwitch(bool& enabled) {
    const auto p = ImGui::GetCursorScreenPos();
    const ImVec2 size(40.f, 22.f);
    const bool clicked = ImGui::InvisibleButton("##DayNightAutomatic", size, ImGuiButtonFlags_EnableNav);
    if (clicked) enabled = !enabled;
    const auto& colors = ThemeColors::current();
    auto* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(p, ImVec2(p.x+size.x,p.y+size.y),
        ImGui::GetColorU32(enabled ? colors.controlAccent : colors.chromeBorder), size.y*.5f);
    draw->AddRect(p, ImVec2(p.x+size.x,p.y+size.y),
        ImGui::GetColorU32(ImGui::IsItemFocused() ? colors.controlAccent : colors.chromeBorder), size.y*.5f);
    draw->AddCircleFilled(ImVec2(p.x+(enabled ? size.x-11.f : 11.f),p.y+11.f),8.f,IM_COL32(249,248,246,255));
    if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    return clicked;
}
bool PaletteCard(Period period, bool selected, bool automatic, float width) {
    const auto p = ImGui::GetCursorScreenPos();
    const ImVec2 size(width, 128.f), end(p.x+width,p.y+128.f);
    const bool clicked = ImGui::InvisibleButton("##palette",size,
        automatic ? ImGuiButtonFlags_None : ImGuiButtonFlags_EnableNav);
    const auto& palette = Palette(period);
    const auto& surface = ThemeColors::current();
    auto* draw = ImGui::GetWindowDrawList();
    const auto bg = palette.mainWindowBG;
    const auto text = .299f*bg.x+.587f*bg.y+.114f*bg.z > .55f
        ? IM_COL32(45,33,40,255) : IM_COL32(230,238,246,255);
    const bool hovered = !automatic && ImGui::IsItemHovered();
    draw->AddRectFilled(p,end,ImGui::GetColorU32(bg),8.f);
    draw->AddTriangleFilled(ImVec2(end.x-42.f,p.y+1.f),ImVec2(end.x-1.f,p.y+1.f),
        ImVec2(end.x-1.f,p.y+42.f),ImGui::GetColorU32(palette.toolbarAccent));
    draw->AddRect(p,end,ImGui::GetColorU32(selected || hovered || ImGui::IsItemFocused()
        ? surface.controlAccent : surface.chromeBorder),8.f,0,selected || ImGui::IsItemFocused() ? 2.f : 1.f);
    if (selected) {
        draw->AddLine(ImVec2(p.x+18.f,p.y+25.f),ImVec2(p.x+23.f,p.y+30.f),text,1.6f);
        draw->AddLine(ImVec2(p.x+23.f,p.y+30.f),ImVec2(p.x+33.f,p.y+19.f),text,1.6f);
        draw->AddText(ImVec2(p.x+42.f,p.y+16.f),text,L(ThemeSchedule_Using));
    }
    draw->AddText(ImGui::GetFont(),ImGui::GetFontSize()*1.3f,ImVec2(p.x+18.f,end.y-39.f),text,
        period == Period::Day ? L(ThemeSchedule_Day) : L(ThemeSchedule_Night));
    if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    return clicked && !automatic;
}
bool EditLink() {
    ImGui::PushStyleColor(ImGuiCol_Button,ImVec4(0,0,0,0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,ImVec4(0,0,0,0));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,ImVec4(0,0,0,0));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize,0.f);
    const bool clicked = ImGui::Button(L(ThemeSchedule_EditPalette),ImVec2(0,28.f));
    if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    if (ImGui::IsItemHovered() || ImGui::IsItemFocused()) {
        const auto lo=ImGui::GetItemRectMin(), hi=ImGui::GetItemRectMax();
        ImGui::GetWindowDrawList()->AddLine(ImVec2(lo.x+8.f,hi.y-3.f),ImVec2(hi.x-8.f,hi.y-3.f),
            ImGui::GetColorU32(ImGuiCol_Text),1.f);
    }
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(3);
    return clicked;
}
}
#if AURA_BUILD_DIAGNOSTICS && ARIS_PREVIEW_ISOLATION
static ImVec4 g_autoToggleBounds;
static QaSettingsObservation g_ui;
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
    // Settings uses a clipped page inside the popup. Its visible body can be
    // narrower than GetContentRegionAvail(), which includes the scroll gutter.
    const float contentWidth = std::min(ImGui::GetContentRegionAvail().x,
        ImGui::GetWindowDrawList()->GetClipRectMax().x-ImGui::GetCursorScreenPos().x-1.f);
    const float right = ImGui::GetCursorPosX()+contentWidth;
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(L(ThemeSchedule_Automatic));
    ImGui::SameLine();
    ImGui::SetCursorPosX(right-40.f);
    if (AutomaticSwitch(automatic)) {
        SetMode(state, automatic ? Mode::Automatic
            : (CurrentPeriod() == Period::Day ? Mode::FixedDay : Mode::FixedNight));
        SaveAppSettings(state);
    }
#if AURA_BUILD_DIAGNOSTICS && ARIS_PREVIEW_ISOLATION
    g_autoToggleBounds = ItemBounds();
#endif
    ImGui::Spacing();
    ImGui::TextWrapped("%s",automatic ? L(ThemeSchedule_Hint) : L(ThemeSchedule_ManualHint));
    ImGui::Dummy(ImVec2(0,10.f));
    const auto origin = ImGui::GetCursorPos();
    const float gap=20.f, width=std::max(100.f,(contentWidth-gap)*.5f);
    const auto selected = SelectedPeriod(state);
    for (int i=0; i<2; ++i) {
        const auto period = i==0 ? Period::Day : Period::Night;
        ImGui::PushID(i);
        ImGui::SetCursorPos(ImVec2(origin.x+i*(width+gap),origin.y));
        if (PaletteCard(period,selected==period,automatic,width)) {
            SetMode(state,i==0 ? Mode::FixedDay : Mode::FixedNight);
            SaveAppSettings(state);
        }
#if AURA_BUILD_DIAGNOSTICS && ARIS_PREVIEW_ISOLATION
        g_ui.cards[i]=ItemBounds();
        g_ui.selected[i]=selected==period;
#endif
        const float editWidth=ImGui::CalcTextSize(L(ThemeSchedule_EditPalette)).x+ImGui::GetStyle().FramePadding.x*2.f;
        ImGui::SetCursorPos(ImVec2(origin.x+i*(width+gap)+(width-editWidth)*.5f,origin.y+136.f));
        if (EditLink()) {
            state.themeScheduleEditorPeriod=i;
            state.showThemeEditor=true;
            ImGui::CloseCurrentPopup();
        }
#if AURA_BUILD_DIAGNOSTICS && ARIS_PREVIEW_ISOLATION
        g_ui.editButtons[i]=ItemBounds();
#endif
        ImGui::PopID();
    }
    ImGui::SetCursorPos(ImVec2(origin.x,origin.y+176.f));
    if (automatic) {
        g_otherThemesExpanded=false;
        ImGui::TextUnformatted(CurrentPeriod()==Period::Day ? L(ThemeSchedule_AutoDay) : L(ThemeSchedule_AutoNight));
    } else {
        ImGui::SetNextItemOpen(g_otherThemesExpanded,ImGuiCond_Always);
        g_otherThemesExpanded=ImGui::TreeNodeEx(L(ThemeSchedule_OtherThemes),
            ImGuiTreeNodeFlags_NoTreePushOnOpen | ImGuiTreeNodeFlags_SpanAvailWidth);
#if AURA_BUILD_DIAGNOSTICS && ARIS_PREVIEW_ISOLATION
        g_ui.otherThemes=ItemBounds();
        g_ui.otherThemesExpanded=g_otherThemesExpanded;
#endif
    }
    for (const auto period : {Period::Day,Period::Night}) {
        if (!LoadWarning(period).empty())
            ImGui::TextWrapped("%s",period==Period::Day ? L(ThemeSchedule_DayFallback) : L(ThemeSchedule_NightFallback));
    }
    ImGui::Spacing();
    return !automatic && g_otherThemesExpanded;
}
}
