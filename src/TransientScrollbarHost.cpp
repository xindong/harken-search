#include "TransientScrollbarHost.h"

#include "ui/CoreWorkspaceStyle.h"
#include "imgui_internal.h"

// Native scrollbar visibility is tracked per semantic ImGui region. The style
// scope only surrounds BeginChild/BeginTable, which is when ImGui renders the
// native scrollbar, so one region can wake without revealing its neighbors.
aura::ui::workspace::TransientScrollbarController transientScrollbarController;
double transientScrollbarNow = 0.0;

bool BeginTransientScrollbarChild(
    const char* strId,
    const ImVec2& size,
    ImGuiChildFlags childFlags,
    ImGuiWindowFlags flags) {
    const ImGuiID regionId = ImGui::GetID(strId);
    aura::ui::workspace::TransientScrollbarStyleScope styleScope(
        transientScrollbarController.shouldShow(
            regionId, transientScrollbarNow, true));
    const bool visible = (ImGui::BeginChild)(strId, size, childFlags, flags);
    transientScrollbarController.bindCurrentWindowToRegion(regionId);
    return visible;
}

bool BeginTransientScrollbarChild(
    ImGuiID id,
    const ImVec2& size,
    ImGuiChildFlags childFlags,
    ImGuiWindowFlags flags) {
    aura::ui::workspace::TransientScrollbarStyleScope styleScope(
        transientScrollbarController.shouldShow(id, transientScrollbarNow, true));
    const bool visible = (ImGui::BeginChild)(id, size, childFlags, flags);
    transientScrollbarController.bindCurrentWindowToRegion(id);
    return visible;
}

bool BeginTransientScrollbarTable(
    const char* strId,
    int columns,
    ImGuiTableFlags flags,
    const ImVec2& outerSize,
    float innerWidth) {
    const ImGuiID regionId = ImGui::GetID(strId);
    aura::ui::workspace::TransientScrollbarStyleScope styleScope(
        transientScrollbarController.shouldShow(
            regionId, transientScrollbarNow, true));
    const bool visible = (ImGui::BeginTable)(
        strId, columns, flags, outerSize, innerWidth);
    if (visible && (flags & (ImGuiTableFlags_ScrollX | ImGuiTableFlags_ScrollY))) {
        transientScrollbarController.bindCurrentWindowToRegion(regionId);
    }
    return visible;
}

bool BeginTransientScrollbarWindow(
    const char* name,
    bool* open,
    ImGuiWindowFlags flags) {
    const ImGuiID regionId = ImHashStr(name);
    aura::ui::workspace::TransientScrollbarStyleScope styleScope(
        transientScrollbarController.shouldShow(
            regionId, transientScrollbarNow, true));
    const bool visible = (ImGui::Begin)(name, open, flags);
    transientScrollbarController.bindCurrentWindowToRegion(regionId);
    return visible;
}

bool BeginTransientScrollbarPopup(
    const char* strId,
    ImGuiWindowFlags flags) {
    const ImGuiID regionId = ImGui::GetID(strId);
    aura::ui::workspace::TransientScrollbarStyleScope styleScope(
        transientScrollbarController.shouldShow(
            regionId, transientScrollbarNow, true));
    const bool visible = (ImGui::BeginPopup)(strId, flags);
    if (visible) transientScrollbarController.bindCurrentWindowToRegion(regionId);
    return visible;
}

bool BeginTransientScrollbarPopupModal(
    const char* name,
    bool* open,
    ImGuiWindowFlags flags) {
    const ImGuiID regionId = ImGui::GetID(name);
    aura::ui::workspace::TransientScrollbarStyleScope styleScope(
        transientScrollbarController.shouldShow(
            regionId, transientScrollbarNow, true));
    const bool visible = (ImGui::BeginPopupModal)(name, open, flags);
    if (visible) transientScrollbarController.bindCurrentWindowToRegion(regionId);
    return visible;
}

bool BeginTransientScrollbarCombo(
    const char* label,
    const char* previewValue,
    ImGuiComboFlags flags) {
    const ImGuiID regionId = ImGui::GetID(label);
    aura::ui::workspace::TransientScrollbarStyleScope styleScope(
        transientScrollbarController.shouldShow(
            regionId, transientScrollbarNow, true));
    const bool visible = (ImGui::BeginCombo)(label, previewValue, flags);
    if (visible) transientScrollbarController.bindCurrentWindowToRegion(regionId);
    return visible;
}
