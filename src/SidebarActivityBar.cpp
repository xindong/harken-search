#include "SidebarActivityBar.h"

#include "AppState.h"
#include "Localization.h"
#include "ProductEdition.h"
#include "Theme.h"
#include "TransientScrollbarHost.h"

#include "imgui.h"
#include "imgui_internal.h"

#include <algorithm>
#include <cstdio>

void MarkSettingsDirty(AppState& state);
void MaybePausePlaybackOnInScopeDragStart();

// Sidebar Activity Bar (VS Code-style icon rail)

namespace {
inline float LerpSidebarValue(float a, float b, float t) { return a + (b - a) * t; }
} // namespace

// Draws a simple vector icon for the given sectionId into draw_list.
// sectionId: 0=ViewMode, 1=FolderTree, 2=MetadataFilter, 3=Playlists, 4=TagCloud, 5=FolderShortcuts
void DrawSidebarIcon(ImDrawList* dl, ImVec2 center, float size, int sectionId, ImU32 color) {
    const float s = size * 0.5f;
    const float thick = 1.5f;
    switch (sectionId) {
    case 0: { // ViewMode — blocks/grid
        dl->AddRect(ImVec2(center.x - s, center.y - s), ImVec2(center.x + s, center.y + s), color, 2.0f, 0, thick);
        dl->AddLine(ImVec2(center.x - s, center.y - s*0.2f), ImVec2(center.x + s, center.y - s*0.2f), color, thick);
        dl->AddLine(ImVec2(center.x - s*0.2f, center.y - s*0.2f), ImVec2(center.x - s*0.2f, center.y + s), color, thick);
    } break;
    case 1: { // FolderTree — branching nodes
        const float r = thick * 1.5f;
        dl->AddLine(ImVec2(center.x - s*0.5f, center.y - s*0.8f), ImVec2(center.x - s*0.5f, center.y + s*0.8f), color, thick);
        dl->AddLine(ImVec2(center.x - s*0.5f, center.y - s*0.1f), ImVec2(center.x + s*0.3f, center.y - s*0.1f), color, thick);
        dl->AddLine(ImVec2(center.x - s*0.5f, center.y + s*0.8f), ImVec2(center.x + s*0.3f, center.y + s*0.8f), color, thick);
        dl->AddCircleFilled(ImVec2(center.x - s*0.5f, center.y - s*0.8f), r, color);
        dl->AddCircleFilled(ImVec2(center.x + s*0.5f, center.y - s*0.1f), r, color);
        dl->AddCircleFilled(ImVec2(center.x + s*0.5f, center.y + s*0.8f), r, color);
    } break;
    case 2: { // MetadataFilter — funnel
        dl->PathClear();
        dl->PathLineTo(ImVec2(center.x - s, center.y - s));
        dl->PathLineTo(ImVec2(center.x + s, center.y - s));
        dl->PathLineTo(ImVec2(center.x + s*0.2f, center.y));
        dl->PathLineTo(ImVec2(center.x + s*0.2f, center.y + s));
        dl->PathLineTo(ImVec2(center.x - s*0.2f, center.y + s*0.5f));
        dl->PathLineTo(ImVec2(center.x - s*0.2f, center.y));
        dl->PathStroke(color, ImDrawFlags_Closed, thick);
    } break;
    case 3: { // Playlists — list lines
        for (int i = 0; i < 3; ++i) {
            float y = center.y - s*0.6f + i * (s*0.6f);
            dl->AddLine(ImVec2(center.x - s*0.8f, y), ImVec2(center.x - s*0.6f, y), color, thick);
            dl->AddLine(ImVec2(center.x - s*0.2f, y), ImVec2(center.x + s*0.8f, y), color, thick);
        }
    } break;
    case 4: { // TagCloud — tag
        dl->PathClear();
        dl->PathLineTo(ImVec2(center.x - s, center.y - s));
        dl->PathLineTo(ImVec2(center.x, center.y - s));
        dl->PathLineTo(ImVec2(center.x + s, center.y));
        dl->PathLineTo(ImVec2(center.x, center.y + s));
        dl->PathLineTo(ImVec2(center.x - s, center.y));
        dl->PathStroke(color, ImDrawFlags_Closed, thick);
        dl->AddCircleFilled(ImVec2(center.x - s*0.4f, center.y - s*0.4f), 1.5f, color);
    } break;
    case 5: { // FolderShortcuts — lightning
        dl->PathClear();
        dl->PathLineTo(ImVec2(center.x + s*0.1f, center.y - s*1.0f));
        dl->PathLineTo(ImVec2(center.x - s*0.9f, center.y + s*0.2f));
        dl->PathLineTo(ImVec2(center.x,          center.y + s*0.2f));
        dl->PathLineTo(ImVec2(center.x - s*0.1f, center.y + s*1.0f));
        dl->PathLineTo(ImVec2(center.x + s*0.9f, center.y - s*0.2f));
        dl->PathLineTo(ImVec2(center.x,          center.y - s*0.2f));
        dl->PathStroke(color, ImDrawFlags_Closed, thick);
    } break;
    default: break;
    }
}

const char* GetSidebarSectionLabel(int sectionId) {
    switch (sectionId) {
    case 0: return L(Filter_ViewMode);
    case 1: return L(Filter_FolderTree);
    case 2: return L(Filter_Metadata);
    case 3: return L(Filter_Playlists);
    case 4: return L(Filter_TagCloud);
    case 5: return L(Nav_QuickPaths);
    default: return "?";
    }
}

static bool* GetSidebarSectionVisibleFlag(AppState& state, int sectionId) {
    switch (sectionId) {
    case 0: return &state.settings.showViewMode;
    case 1: return &state.settings.showFolderTree;
    case 2: return &state.settings.showMetadataFilter;
    case 3: return &state.settings.showPlaylists;
    case 4: return &state.settings.showTagCloud;
    case 5: return &state.settings.showFolderShortcuts;
    default: return nullptr;
    }
}

static float SidebarSizeFactor(AppSettings::SidebarSize sz) {
    switch (sz) {
    case AppSettings::SidebarSize::Compact: return 0.80f;
    case AppSettings::SidebarSize::Large:   return 1.30f;
    default: return 1.0f;
    }
}

float SidebarRailWidth(const AppState& state) {
    return 52.0f * SidebarSizeFactor(state.settings.sidebarSize);
}

// Draws the VS Code-style vertical activity bar.

// SECTION 7 - Sidebar Activity Bar

// Click icon → switch active section (click active → collapse to -1).
// Drag icon → reorder sections.
// Right-click → hide / size submenu / reset order.
void DrawSidebarActivityBar(AppState& state) {
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    if (window->SkipItems) return;

    const float sizeFactor = SidebarSizeFactor(state.settings.sidebarSize);
    const float bar_width = 52.0f * sizeFactor;
    const float item_h    = 52.0f * sizeFactor;
    const float icon_size = 20.0f * sizeFactor;

    const ImVec2 pos = window->DC.CursorPos;
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    const float availY = ImGui::GetContentRegionAvail().y;

    // Rail is chrome-less by design: no background fill, no right-edge divider.
    // The icons float directly on the outer LeftDockColumn gradient so the rail
    // reads as "icon column" rather than a separate bolted-on UI bar. Color comes
    // exclusively from icon tint / active strip / hover animation below.
    //
    // (sidebarRailBG / sidebarRailDivider tokens are still set per-theme but used
    // only by the content header / optional future chrome, not here.)
    const ThemeColorScheme& _sbth = ThemeColors::current();

    auto& ord = state.settings.sidebarSectionOrder;
    const int n = AppSettings::kSidebarSectionCount;

    float cursorY = pos.y;
    int visibleCount = 0;  // Counts icons actually rendered, drives the 3+3 group divider.
    for (int pos_idx = 0; pos_idx < n; ++pos_idx) {
        int sectionId = ord[pos_idx];
        if (sectionId < 0 || sectionId >= n) continue;
        bool* visiblePtr = GetSidebarSectionVisibleFlag(state, sectionId);
        if (!visiblePtr || !*visiblePtr) continue; // hidden by user

        // Feature-gated sections: hide icon when edition lacks feature.
        if (sectionId == 2 && !ProductEdition::hasFeature("metadata_filter")) continue;
        if (sectionId == 4 && !ProductEdition::hasFeature("tag_cloud")) continue;

        // Separate the navigation and data groups after the third visible icon.
        // A small divider preserves the rail hierarchy.
        if (visibleCount == 3) {
            const float gap = 12.0f * sizeFactor;
            const float lineY = cursorY + gap * 0.5f;
            draw_list->AddLine(
                ImVec2(pos.x + 14.0f * sizeFactor, lineY),
                ImVec2(pos.x + bar_width - 14.0f * sizeFactor, lineY),
                ImGui::GetColorU32(_sbth.sidebarRailDivider), 1.0f);
            cursorY += gap;
        }
        ++visibleCount;

        const ImVec2 item_min = ImVec2(pos.x, cursorY);
        const ImVec2 item_max = ImVec2(pos.x + bar_width, cursorY + item_h);
        cursorY += item_h;

        ImRect bb(item_min, item_max);
        char idbuf[32]; snprintf(idbuf, sizeof(idbuf), "##SidebarIcon_%d", sectionId);
        ImGuiID id = window->GetID(idbuf);
        ImGui::ItemSize(bb);
        if (!ImGui::ItemAdd(bb, id)) continue;

        bool hovered = false, held = false;
        bool pressed = ImGui::ButtonBehavior(bb, id, &hovered, &held);

        const bool is_active = (state.settings.activeSidebarSection == sectionId);
        if (pressed) {
            state.settings.activeSidebarSection = is_active ? -1 : sectionId;
            MarkSettingsDirty(state);
        }

        // Smooth hover/active animation via per-item storage
        ImGuiStorage* storage = ImGui::GetStateStorage();
        float hover_anim  = storage->GetFloat(id + 1, 0.0f);
        float active_anim = storage->GetFloat(id + 2, is_active ? 1.0f : 0.0f);
        const float dt = ImGui::GetIO().DeltaTime;
        hover_anim  = LerpSidebarValue(hover_anim,  hovered   ? 1.0f : 0.0f, std::min(1.0f, dt * 15.0f));
        active_anim = LerpSidebarValue(active_anim, is_active ? 1.0f : 0.0f, std::min(1.0f, dt * 15.0f));
        storage->SetFloat(id + 1, hover_anim);
        storage->SetFloat(id + 2, active_anim);

        // Color blend: inactive → hover (on hover_anim) → active (on active_anim, higher priority).
        auto mixV4 = [](const ImVec4& a, const ImVec4& b, float t) {
            return ImVec4(
                LerpSidebarValue(a.x, b.x, t),
                LerpSidebarValue(a.y, b.y, t),
                LerpSidebarValue(a.z, b.z, t),
                LerpSidebarValue(a.w, b.w, t));
        };
        ImVec4 iconV4 = mixV4(_sbth.sidebarIconInactive, _sbth.sidebarIconHover, hover_anim);
        iconV4        = mixV4(iconV4,                    _sbth.sidebarIconActive, active_anim);
        const ImU32 icon_color = ImGui::GetColorU32(iconV4);

        // F-042 /clarify: in Regular/Large sizes, shift the icon up and
        // render the section label below it so users don't rely on hover
        // tooltips to distinguish the 6 rail slots. Compact stays icon-only
        // (41.6px wide can't fit 4-char CJK at 13px font).
        const bool showRailLabel =
            state.settings.showGuidedMode &&
            state.settings.sidebarSize != AppSettings::SidebarSize::Compact;
        const float iconY = showRailLabel
            ? (item_min.y + item_h * 0.38f)
            : (item_min.y + item_h * 0.5f);
        const ImVec2 center(item_min.x + bar_width * 0.5f, iconY);
        DrawSidebarIcon(draw_list, center, icon_size, sectionId, icon_color);
        if (showRailLabel) {
            const char* lbl = GetSidebarSectionLabel(sectionId);
            const ImVec2 lblSize = ImGui::CalcTextSize(lbl);
            const ImVec2 lblPos(
                item_min.x + (bar_width - lblSize.x) * 0.5f,
                item_min.y + item_h - lblSize.y - 4.0f * sizeFactor);
            draw_list->AddText(lblPos, icon_color, lbl);
        }

        // Left-edge highlight strip when active — pulls from theme token sidebarActiveStrip.
        if (active_anim > 0.01f) {
            const float strip_h = 36.0f * sizeFactor * active_anim;
            const ImVec2 s_min(item_min.x, center.y - strip_h * 0.5f);
            const ImVec2 s_max(item_min.x + 2.0f, center.y + strip_h * 0.5f);
            const ImVec4 stripV4 = _sbth.sidebarActiveStrip;
            draw_list->AddRectFilled(s_min, s_max, ImGui::GetColorU32(stripV4));
            // Glow halo: same hue, reduced alpha modulated by active_anim.
            const ImVec4 glowV4(stripV4.x, stripV4.y, stripV4.z, stripV4.w * 0.157f * active_anim);
            draw_list->AddRectFilled(ImVec2(s_min.x, s_min.y - 4.0f), ImVec2(s_max.x + 8.0f, s_max.y + 4.0f),
                                     ImGui::GetColorU32(glowV4));
        }

        if (hovered) {
            if (HarkenSearch::BeginTooltip()) {
            ImGui::TextUnformatted(GetSidebarSectionLabel(sectionId));
            ImGui::EndTooltip(); }
        }

        // Drag source: drag this icon to reorder
        if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
            MaybePausePlaybackOnInScopeDragStart();
            ImGui::SetDragDropPayload("SIDEBAR_SECTION", &sectionId, sizeof(int));
            ImGui::TextUnformatted(GetSidebarSectionLabel(sectionId));
            ImGui::EndDragDropSource();
        }
        // Drop target: receive another icon's sectionId and reorder in place
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("SIDEBAR_SECTION")) {
                int from = *(const int*)p->Data;
                int to = sectionId;
                if (from != to) {
                    int iFrom = -1, iTo = -1;
                    for (int i = 0; i < n; i++) { if (ord[i] == from) iFrom = i; if (ord[i] == to) iTo = i; }
                    if (iFrom >= 0 && iTo >= 0) {
                        int v = ord[iFrom];
                        if (iFrom < iTo) { for (int i = iFrom; i < iTo; i++) ord[i] = ord[i+1]; ord[iTo] = v; }
                        else             { for (int i = iFrom; i > iTo; i--) ord[i] = ord[i-1]; ord[iTo] = v; }
                        MarkSettingsDirty(state);
                    }
                }
            }
            ImGui::EndDragDropTarget();
        }

        // Right-click context menu: hide / size / reset order
        char popupId[40]; snprintf(popupId, sizeof(popupId), "##SidebarCtx_%d", sectionId);
        if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
            ImGui::OpenPopup(popupId);
        }
        if (BeginTransientScrollbarPopup(popupId)) {
            if (ImGui::MenuItem(L(Misc_HideModuleHint))) {
                *visiblePtr = false;
                if (is_active) state.settings.activeSidebarSection = -1;
                MarkSettingsDirty(state);
            }
            ImGui::Separator();
            if (ImGui::BeginMenu(L(Sidebar_Size))) {
                using SZ = AppSettings::SidebarSize;
                auto& sz = state.settings.sidebarSize;
                if (ImGui::MenuItem(L(Sidebar_SizeCompact), nullptr, sz == SZ::Compact)) { sz = SZ::Compact; MarkSettingsDirty(state); }
                if (ImGui::MenuItem(L(Sidebar_SizeRegular), nullptr, sz == SZ::Regular)) { sz = SZ::Regular; MarkSettingsDirty(state); }
                if (ImGui::MenuItem(L(Sidebar_SizeLarge),   nullptr, sz == SZ::Large))   { sz = SZ::Large;   MarkSettingsDirty(state); }
                ImGui::EndMenu();
            }
            if (ImGui::MenuItem(L(Sidebar_ResetOrder))) {
                int defaults[6] = {0, 5, 1, 4, 2, 3};
                for (int i = 0; i < AppSettings::kSidebarSectionCount; i++) ord[i] = defaults[i];
                MarkSettingsDirty(state);
            }
            ImGui::EndPopup();
        }
    }

// Bottom-docked panel menu (≡) — replaces the old top-strip hamburger
    // Renders a hamburger icon at the bottom of the rail that opens Float / Hide /
    // module visibility toggles. Anchored to availY so it stays bottom-aligned.
    {
        const float bottomY = pos.y + availY - item_h;
        const ImVec2 b_min(pos.x, bottomY);
        const ImVec2 b_max(pos.x + bar_width, bottomY + item_h);
        ImRect bb(b_min, b_max);
        ImGuiID bid = window->GetID("##SidebarPanelMenuBtn");
        ImGui::ItemSize(bb);
        if (ImGui::ItemAdd(bb, bid)) {
            bool bhovered = false, bheld = false;
            bool bpressed = ImGui::ButtonBehavior(bb, bid, &bhovered, &bheld);
            if (bpressed) ImGui::OpenPopup("##SidebarPanelMenu");

            ImGuiStorage* storage = ImGui::GetStateStorage();
            float bhover = storage->GetFloat(bid + 1, 0.0f);
            const float dt = ImGui::GetIO().DeltaTime;
            bhover = LerpSidebarValue(bhover, bhovered ? 1.0f : 0.0f, std::min(1.0f, dt * 15.0f));
            storage->SetFloat(bid + 1, bhover);

            ImVec4 bIconV4(
                LerpSidebarValue(_sbth.sidebarIconInactive.x, _sbth.sidebarIconHover.x, bhover),
                LerpSidebarValue(_sbth.sidebarIconInactive.y, _sbth.sidebarIconHover.y, bhover),
                LerpSidebarValue(_sbth.sidebarIconInactive.z, _sbth.sidebarIconHover.z, bhover),
                LerpSidebarValue(_sbth.sidebarIconInactive.w, _sbth.sidebarIconHover.w, bhover));
            const ImU32 bcol = ImGui::GetColorU32(bIconV4);
            const ImVec2 bcen(b_min.x + bar_width * 0.5f, b_min.y + item_h * 0.5f);
            const float s = icon_size * 0.5f;
            // 3 horizontal lines (hamburger)
            draw_list->AddLine(ImVec2(bcen.x - s, bcen.y - s*0.5f), ImVec2(bcen.x + s, bcen.y - s*0.5f), bcol, 1.5f);
            draw_list->AddLine(ImVec2(bcen.x - s, bcen.y),           ImVec2(bcen.x + s, bcen.y),           bcol, 1.5f);
            draw_list->AddLine(ImVec2(bcen.x - s, bcen.y + s*0.5f), ImVec2(bcen.x + s, bcen.y + s*0.5f), bcol, 1.5f);

            if (bhovered) {
                if (HarkenSearch::BeginTooltip()) {
                ImGui::TextUnformatted(L(Filter_ManageModules));
                ImGui::EndTooltip(); }
            }
        }
        if (BeginTransientScrollbarPopup("##SidebarPanelMenu")) {
            ImGuiViewport* vp = ImGui::GetMainViewport();
            if (ImGui::MenuItem(L(Panel_Float))) {
                float w = state.panelSidebar.floatW > 0 ? state.panelSidebar.floatW : 320.0f;
                float h = state.panelSidebar.floatH > 0 ? state.panelSidebar.floatH : vp->WorkSize.y * 0.7f;
                state.panelSidebar.floatW = w;
                state.panelSidebar.floatH = h;
                state.panelSidebar.floatX = vp->WorkPos.x + (vp->WorkSize.x - w) * 0.3f;
                state.panelSidebar.floatY = vp->WorkPos.y + (vp->WorkSize.y - h) * 0.5f;
                state.panelSidebar.floating = true;
                state.panelSidebar.wasDragging = false;
            }
            if (ImGui::MenuItem(L(Panel_Hide))) {
                state.panelSidebar.visible = false;
            }
            ImGui::Separator();
            ImGui::TextDisabled("%s", L(Filter_ShowHideModules));
            if (ImGui::MenuItem(L(Filter_ViewMode),     nullptr, &state.settings.showViewMode))        MarkSettingsDirty(state);
            if (ImGui::MenuItem(L(Nav_QuickPaths),      nullptr, &state.settings.showFolderShortcuts)) MarkSettingsDirty(state);
            if (ImGui::MenuItem(L(Filter_FolderTree),   nullptr, &state.settings.showFolderTree))      MarkSettingsDirty(state);
            if (ImGui::MenuItem(L(Nav_RelatedPaths),   nullptr, &state.settings.showRelatedPaths))   MarkSettingsDirty(state);
            if (ImGui::MenuItem(L(Filter_TagCloud),     nullptr, &state.settings.showTagCloud))        MarkSettingsDirty(state);
            if (ImGui::MenuItem(L(Filter_Metadata),     nullptr, &state.settings.showMetadataFilter))  MarkSettingsDirty(state);
            if (ImGui::MenuItem(L(Filter_Playlists),    nullptr, &state.settings.showPlaylists))       MarkSettingsDirty(state);
            ImGui::EndPopup();
        }
    }
}
