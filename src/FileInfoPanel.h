#pragma once

#include "imgui.h"
#include <functional>

struct AppState;
struct ThemeColorScheme;
bool RequestInfoMetadataLeave(AppState& state, std::function<void()> afterLeave = {});
void GuardInfoMetadataContext(AppState& state);
void RenderInfoMetadataLeavePrompt(AppState& state);

// @stage7-contract
// API contract: UI-thread drawing of the right file-info panel shell. AppState
// is borrowed for one frame and not retained. Cover-wall data, metadata writes,
// and path-chip ownership remain in the composition root.

struct FileInfoPanelChrome {
    ImVec4 kAccent;
    ImVec4 kSelected;
    ImVec4 kPanelBase;
    ImVec4 kShellSurface;
    ImVec4 kShellSectionBg;
    ImVec4 kShellSectionBgStrong;
    ImVec4 kShellSectionEdge;
    ImVec4 kShellSectionAccent;
    ImVec4 kPanelTab;
    ImVec4 kPanelTabHover;
    ImVec4 kPanelTabActive;
    ImVec4 kPanelTabUnfocused;
    ImVec4 kPanelTabUnfocusedActive;
    ImVec4 kMutedButton;
    ImVec4 kPathChipBase;
    ImVec4 kPathChipHover;
    ImVec4 kPathChipActive;
    ImVec4 kPathChipText;
    ImVec4 kCurrentItemText;
    ImVec4 kInfoLabelText;
    ImVec4 kInfoMutedText;
};

FileInfoPanelChrome MakeFileInfoPanelChrome();
void DecorateFileInfoTab(const FileInfoPanelChrome& chrome, bool selected);
void RenderFileInfoFieldVisibilityMenu(AppState& state);
void RenderFileInfoPanelTabContents(
    AppState& state,
    float panelWidth,
    const FileInfoPanelChrome& chrome);
float FileInfoPageActionsWidth(AppState& state, float availableWidth);
// Returns true when the Similar Sounds window shortcut is activated.
bool RenderFileInfoPageActions(AppState& state, ImVec2 position, float height, float availableWidth);
void RenderFileInfoPanelContent(AppState& state, float panelWidth);
int PushFileInfoShellStyle(const ThemeColorScheme& tcs);

inline float FileInfoArtworkContentHeight(float availableHeight, float stripHeight) {
    // EndChild advances by the active theme spacing before the fixed footer.
    const float height = availableHeight - stripHeight - ImGui::GetStyle().ItemSpacing.y;
    return height > 32.0f ? height : 32.0f;
}

inline void FinishFileInfoArtworkFooter(ImVec2 stripPos, float stripHeight) {
    ImGui::SetCursorScreenPos(ImVec2(stripPos.x, stripPos.y + stripHeight));
    // Without a tab bar, this footer must reserve its own trailing space.
    ImGui::Dummy(ImVec2(0.0f, 0.0f));
}
