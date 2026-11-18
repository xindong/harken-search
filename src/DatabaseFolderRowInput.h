#pragma once

#include "imgui.h"

namespace DatabaseFolderRowInput {
inline bool TreeNode(const char* label, ImGuiTreeNodeFlags flags, float chevronWidth, float verticalPadding) {
    const float horizontalPadding = (chevronWidth - ImGui::GetFontSize() - ImGui::GetStyle().TouchExtraPadding.x) * 0.5f;
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(horizontalPadding, verticalPadding));
    const bool open = ImGui::TreeNodeEx(label, flags);
    ImGui::PopStyleVar();
    return open;
}

inline bool SelectClicked(bool hasChildren, float chevronWidth) {
    return ImGui::IsItemClicked() && (!hasChildren ||
           ImGui::GetIO().MousePos.x >= ImGui::GetItemRectMin().x + chevronWidth);
}
}
