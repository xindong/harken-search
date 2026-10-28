#include "FileInfoPanel.h"

#include "AppState.h"
#include "Localization.h"
#include "ThemeInspector.h"
#include <cmath>
#include "Theme.h"
#include "TransientScrollbarHost.h"
#include "ui/CoreWorkspaceStyle.h"
#include "ui/TransportContrast.h"

#include "imgui.h"

FileInfoPanelChrome MakeFileInfoPanelChrome() {
    ImGuiStyle& style = ImGui::GetStyle();
    ImVec4* themeColors = style.Colors;
    auto withAlpha = [](ImVec4 c, float a) -> ImVec4 { c.w = a; return c; };
    auto mix = [](const ImVec4& a, const ImVec4& b, float t) -> ImVec4 {
        return ImVec4(
            a.x + (b.x - a.x) * t,
            a.y + (b.y - a.y) * t,
            a.z + (b.z - a.z) * t,
            a.w + (b.w - a.w) * t);
    };

    const auto& infoTheme = ThemeColors::current();
    const ImVec4 kAccent = withAlpha(ThemeColors::infoPanelAccent(infoTheme), 1.0f);
    const ImVec4 kSelected = withAlpha(COLOR_SELECTED, 1.0f);
    const ImVec4 kPanelBase = themeColors[ImGuiCol_ChildBg];
    FileInfoPanelChrome chrome;
    chrome.kAccent = kAccent;
    chrome.kSelected = kSelected;
    chrome.kPanelBase = kPanelBase;
    chrome.kShellSurface = withAlpha(mix(kPanelBase, themeColors[ImGuiCol_WindowBg], 0.18f), 0.98f);
    chrome.kShellSectionBg = withAlpha(mix(chrome.kShellSurface, kAccent, 0.08f), 0.96f);
    chrome.kShellSectionBgStrong = withAlpha(mix(chrome.kShellSurface, kAccent, 0.14f), 0.98f);
    chrome.kShellSectionEdge = withAlpha(mix(ThemeColors::infoPanelDockEdge(infoTheme), kAccent, 0.16f), 1.0f);
    chrome.kShellSectionAccent = withAlpha(mix(kAccent, kSelected, 0.16f), 1.0f);
    chrome.kPanelTab = withAlpha(mix(kPanelBase, kAccent, 0.10f), 0.82f);
    chrome.kPanelTabHover = withAlpha(mix(kPanelBase, kAccent, 0.20f), 0.90f);
    chrome.kPanelTabActive = withAlpha(mix(kPanelBase, kAccent, 0.28f), 0.96f);
    chrome.kPanelTabUnfocused = withAlpha(mix(kPanelBase, themeColors[ImGuiCol_TabUnfocused], 0.46f), 0.84f);
    chrome.kPanelTabUnfocusedActive = withAlpha(mix(kPanelBase, themeColors[ImGuiCol_TabUnfocusedActive], 0.50f), 0.88f);
    chrome.kMutedButton = withAlpha(mix(kPanelBase, themeColors[ImGuiCol_Button], 0.55f), 1.0f);
    chrome.kPathChipBase = withAlpha(mix(kPanelBase, COLOR_ACCENT, 0.35f), 0.86f);
    chrome.kPathChipHover = withAlpha(mix(themeColors[ImGuiCol_ButtonHovered], COLOR_ACCENT, 0.55f), 1.0f);
    chrome.kPathChipActive = withAlpha(mix(themeColors[ImGuiCol_ButtonActive], COLOR_SELECTED, 0.45f), 1.0f);
    chrome.kPathChipText = withAlpha(mix(themeColors[ImGuiCol_Text], COLOR_ACCENT, 0.35f), 1.0f);
    chrome.kCurrentItemText = withAlpha(mix(themeColors[ImGuiCol_Text], COLOR_ACCENT, 0.45f), 1.0f);
    chrome.kInfoLabelText = withAlpha(mix(kAccent, themeColors[ImGuiCol_Text], 0.20f), 0.99f);
    chrome.kInfoMutedText = withAlpha(mix(themeColors[ImGuiCol_TextDisabled], themeColors[ImGuiCol_Text], 0.28f), 1.0f);
    return chrome;
}

void DecorateFileInfoTab(const FileInfoPanelChrome& chrome, bool selected) {
    auto withAlpha = [](ImVec4 c, float a) -> ImVec4 { c.w = a; return c; };
    auto mix = [](const ImVec4& a, const ImVec4& b, float t) -> ImVec4 {
        return ImVec4(
            a.x + (b.x - a.x) * t,
            a.y + (b.y - a.y) * t,
            a.z + (b.z - a.z) * t,
            a.w + (b.w - a.w) * t);
    };

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 mn = ImGui::GetItemRectMin();
    ImVec2 mx = ImGui::GetItemRectMax();
    if (mx.x <= mn.x || mx.y <= mn.y) return;
    const ImVec4 cue = withAlpha(mix(chrome.kAccent, chrome.kSelected, 0.18f), 1.0f);
    const bool isLightInfoTabTheme =
        (chrome.kPanelTab.x + chrome.kPanelTab.y + chrome.kPanelTab.z) > 2.10f;
    const bool hovered = ImGui::IsItemHovered();
    const float fillAlpha = selected ? 0.14f : (hovered ? 0.07f : 0.0f);
    if (fillAlpha > 0.0f) {
        dl->AddRectFilled(
            mn, mx,
            ImGui::GetColorU32(withAlpha(cue, fillAlpha)),
            4.0f);
    }
    // F-044 /typeset: light-theme active emphasis was too faint vs
    // dark (0.14/0.24 vs 0.30/0.72). Bump the light side so the
    // active tab is clearly the selected one on RoseCedar/JadeMist.
    if (selected || hovered) {
        dl->AddRect(
            ImVec2(mn.x + 0.5f, mn.y + 0.5f),
            ImVec2(mx.x - 0.5f, mx.y - 0.5f),
            ImGui::GetColorU32(withAlpha(cue, selected ? (isLightInfoTabTheme ? 0.26f : 0.30f) : (isLightInfoTabTheme ? 0.10f : 0.12f))),
            4.0f, 0, 1.0f);
    }
    if (selected) {
        dl->AddLine(
            ImVec2(mn.x + 6.0f, mx.y - 1.0f),
            ImVec2(mx.x - 6.0f, mx.y - 1.0f),
            ImGui::GetColorU32(withAlpha(cue, isLightInfoTabTheme ? 0.48f : 0.72f)),
            2.0f);
    } else if (hovered) {
        dl->AddLine(
            ImVec2(mn.x + 8.0f, mx.y - 1.0f),
            ImVec2(mx.x - 8.0f, mx.y - 1.0f),
            ImGui::GetColorU32(withAlpha(cue, isLightInfoTabTheme ? 0.18f : 0.24f)),
            1.0f);
    }
}

void RenderFileInfoFieldVisibilityMenu(AppState& state) {
    if (ImGui::IsWindowHovered() && ImGui::IsMouseClicked(1) && !ImGui::IsAnyItemHovered()) {
        ImGui::OpenPopup("##FileInfoVisibilityMenu");
    }
    if (BeginTransientScrollbarPopup("##FileInfoVisibilityMenu")) {
        ImGui::TextDisabled("%s", L(Info_ShowHideFields));
    ImGui::Separator();
        ImGui::MenuItem(L(Info_AudioType), nullptr, &state.settings.infoShowAudioType);
        ImGui::MenuItem(L(Info_BitDepth), nullptr, &state.settings.infoShowBitDepth);
        ImGui::MenuItem(L(Info_Channels), nullptr, &state.settings.infoShowChannels);
        ImGui::MenuItem(L(Info_SampleRate), nullptr, &state.settings.infoShowSampleRate);
        ImGui::MenuItem(L(Info_TotalFrames), nullptr, &state.settings.infoShowTotalFrames);
        ImGui::MenuItem(L(Info_Duration), nullptr, &state.settings.infoShowDuration);
        ImGui::Separator();
        ImGui::MenuItem(L(Info_FileName), nullptr, &state.settings.infoShowFilename);
        ImGui::MenuItem(L(Info_Path), nullptr, &state.settings.infoShowPath);
        ImGui::Separator();
        ImGui::MenuItem(L(Info_CreateDate), nullptr, &state.settings.infoShowCreateDate);
        ImGui::MenuItem(L(Info_ModifyDate), nullptr, &state.settings.infoShowModifyDate);
        ImGui::MenuItem(L(Info_EntryDate), nullptr, &state.settings.infoShowEntryDate);
        ImGui::MenuItem(L(Info_ScanDate), nullptr, &state.settings.infoShowScannedDate);
        ImGui::Separator();
        ImGui::MenuItem(L(Info_Description), nullptr, &state.settings.infoShowDescription);
        ImGui::MenuItem(L(Info_Keywords), nullptr, &state.settings.infoShowKeywords);
        ImGui::MenuItem(L(Info_Category), nullptr, &state.settings.infoShowCategory);
        ImGui::MenuItem(L(Info_BWFields), nullptr, &state.settings.infoShowBWFields);
        ImGui::MenuItem(L(Info_Rating), nullptr, &state.settings.infoShowRating);
        ImGui::MenuItem(L(Info_Cover), nullptr, &state.settings.infoShowCover);
        ImGui::EndPopup();
    }
}

int PushFileInfoShellStyle(const ThemeColorScheme& tcs) {
    const ImVec4 infoPanelBg = ThemeColors::infoPanelDockBG(tcs);
    const ImVec4 infoPanelBorder = ThemeColors::infoPanelDockEdge(tcs);
    const ImVec4 infoScrollbar = ThemeColors::fileInfoPanelScrollbar(tcs);
    ImGui::PushStyleColor(ImGuiCol_WindowBg,             infoPanelBg);
    ImGui::PushStyleColor(ImGuiCol_ChildBg,              infoPanelBg);
    ImGui::PushStyleColor(ImGuiCol_Text,                 tcs.metadataPanelText);
    const ImVec4 muted = ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
    ImGui::PushStyleColor(ImGuiCol_TextDisabled, ThemeColors::customized() ? muted :
        TransportContrast::ForSurface(muted, infoPanelBg, 6.0));
    ImGui::PushStyleColor(ImGuiCol_Border,               infoPanelBorder);
    aura::ui::workspace::PushTransientScrollbarColors(
        ImVec4(infoScrollbar.x, infoScrollbar.y, infoScrollbar.z, 0.40f),
        ImVec4(infoScrollbar.x, infoScrollbar.y, infoScrollbar.z, 0.60f),
        ImVec4(infoScrollbar.x, infoScrollbar.y, infoScrollbar.z, 0.80f));
    return 9;
}


namespace {
constexpr bool kDiagnostics = AURA_BUILD_DIAGNOSTICS != 0;
constexpr float kPageButtonWidth = 24.0f;
const char* PageLabel(int page) {
    switch (page) {
    case 0: return L(Info_MetaSummaryTab);
    case 1: return L(Info_ArtworkTab);
    case 2: return L(Info_TransferHistoryTab);
    default: return L(Info_PlayHistoryTab);
    }
}
const char* PageId(int page) {
    static const char* ids[] = {"info.metadataSummary", "info.artwork", "info.transferHistory", "info.playHistory"};
    return ids[page];
}
void DrawPageIcon(ImDrawList* dl, ImVec2 origin, int page, ImU32 color) {
    auto p = [&](float x, float y) { return ImVec2(origin.x + x * .75f, origin.y + y * .75f); };
    auto line = [&](float x, float y, float x2, float y2) { dl->AddLine(p(x,y),p(x2,y2),color,1.3f); };
    if (page == 0) {
        line(14,3,5,3); line(5,3,5,21); line(5,21,19,21); line(19,21,19,8);
        line(19,8,14,3); line(14,3,14,8); line(14,8,19,8);
        line(8,12,16,12); line(8,16,14,16);
    } else if (page == 1) {
        dl->AddRect(p(3,3),p(21,21),color,1.5f,0,1.3f);
        dl->AddCircle(p(8.5f,8.5f),1.1f,color,8,1.1f);
        line(4,17,10,12); line(10,12,14,16); line(14,16,17,13); line(17,13,21,17);
    } else if (page == 2) {
        line(4,6,20,6); line(20,6,16,2); line(20,6,16,10);
        line(8,14,20,14); line(8,19,17,19);
        dl->AddCircleFilled(p(4,14),.8f,color); dl->AddCircleFilled(p(4,19),.8f,color);
    } else if (page == 3) {
        dl->PathArcTo(p(12,12),6.75f,3.66519f,8.90118f,22);
        dl->PathStroke(color,0,1.3f);
        line(4,2,4,8); line(4,8,10,8);
        line(10,8,16,12); line(16,12,10,16); line(10,16,10,8);
    } else {
        for (float x : {5.0f,12.0f,19.0f}) dl->AddCircleFilled(p(x,12),1.0f,color);
    }
}
}

float FileInfoPageActionsWidth(AppState& state, float availableWidth) {
    state.settings.infoPages.Normalize(kDiagnostics);
    const int count = availableWidth < 172.0f ? 1 : state.settings.infoPages.VisibleCount(kDiagnostics);
    return (count + 1) * kPageButtonWidth + 6.0f;
}

void RenderFileInfoPageActions(AppState& state, ImVec2 position, float height, float availableWidth) {
    auto& pages = state.settings.infoPages;
    pages.Normalize(kDiagnostics);
    const bool compact = availableWidth < 172.0f;
    const ImVec2 saved = ImGui::GetCursorPos();
    const auto& theme = ThemeColors::current();
    const ImVec4 base = theme.metadataPanelText;
    const ImVec4 accent = ThemeColors::infoPanelAccent(theme);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float x = position.x;
    ImGui::PushID("InfoPageActions");
    auto button = [&](int page, bool selected) {
        ImGui::SetCursorPos(ImVec2(x, position.y));
        ImGui::PushID(page);
        const bool clicked = ImGui::InvisibleButton("page", ImVec2(kPageButtonWidth, height));
        const bool hovered = ImGui::IsItemHovered();
        const ImVec2 a = ImGui::GetItemRectMin();
        const ImVec4 color = selected || hovered ? base : ImVec4(base.x,base.y,base.z,base.w*.72f);
        DrawPageIcon(dl, ImVec2(a.x+3.0f, a.y+(height-18.0f)*.5f), page, ImGui::GetColorU32(color));
        if (selected) dl->AddLine(ImVec2(a.x+4,a.y+height-2),ImVec2(a.x+20,a.y+height-2),ImGui::GetColorU32(accent),2.0f);
        const char* label = page < 4 ? PageLabel(page) : L(Info_VisiblePages);
        ThemeInspector::RegisterLastItem(page < 4 ? PageId(page) : "info.pageVisibility", label, "src/FileInfoPanel.cpp");
        if (hovered) ImGui::SetTooltip("%s", label);
        ImGui::PopID();
        x += kPageButtonWidth;
        return clicked;
    };
    for (int i=0; i<4; ++i) {
        if (!pages.IsVisible(static_cast<InfoPage>(i), kDiagnostics) || (compact && pages.selected != i)) continue;
        if (button(i, pages.selected == i) && pages.Select(static_cast<InfoPage>(i),kDiagnostics)) state.settingsDirty = true;
    }
    if (button(4,false)) ImGui::OpenPopup("visibility");
    if (BeginTransientScrollbarPopup("visibility")) {
        if (compact) {
            for (int i=0; i<4; ++i) {
                if (!pages.IsVisible(static_cast<InfoPage>(i),kDiagnostics)) continue;
                ImGui::PushID(i);
                if (ImGui::MenuItem(PageLabel(i), nullptr, pages.selected==i)) {
                    if (pages.Select(static_cast<InfoPage>(i),kDiagnostics)) state.settingsDirty = true;
                }
                ThemeInspector::RegisterLastItem((std::string("info.select.")+std::to_string(i)).c_str(),PageLabel(i),"src/FileInfoPanel.cpp");
                ImGui::PopID();
            }
            ImGui::Separator();
        }
        ImGui::TextDisabled("%s", L(Info_VisiblePages));
        for (int i=0; i<4; ++i) {
            const auto page = static_cast<InfoPage>(i);
            if (!InfoPageNavigation::Available(page,kDiagnostics)) continue;
            const bool visible = pages.IsVisible(page,kDiagnostics);
            const bool enabled = !visible || pages.VisibleCount(kDiagnostics)>1;
            ImGui::PushID(i+10);
            if (ImGui::MenuItem(PageLabel(i),nullptr,visible,enabled)) {
                if (pages.SetVisible(page,!visible,kDiagnostics)) state.settingsDirty = true;
            }
            ThemeInspector::RegisterLastItem((std::string("info.visibility.")+std::to_string(i)).c_str(),PageLabel(i),"src/FileInfoPanel.cpp");
            if (!enabled && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s",L(Info_KeepOnePage));
            ImGui::PopID();
        }
        ImGui::EndPopup();
    }
    ImGui::PopID();
    ImGui::SetCursorPos(saved);
}

void RenderFileInfoPanelContent(AppState& state, float panelWidth) {
    GuardInfoMetadataContext(state);
    auto& pages = state.settings.infoPages;
    pages.Normalize(kDiagnostics);
    if (state.switchToCoverTabAfterRebuild) {
        // A background rebuild must not restore an explicitly hidden entry.
        if (!state.infoEditSession.active && pages.Select(InfoPage::Artwork,kDiagnostics)) state.settingsDirty = true;
        state.switchToCoverTabAfterRebuild = false;
    }
    const FileInfoPanelChrome chrome = MakeFileInfoPanelChrome();
    RenderFileInfoFieldVisibilityMenu(state);
    RenderFileInfoPanelTabContents(state, panelWidth, chrome);
}
