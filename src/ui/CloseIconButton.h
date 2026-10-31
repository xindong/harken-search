#pragma once
#include "TransportContrast.h"
#include "imgui_internal.h"

namespace aura::ui::CloseIconButton {
inline bool Draw(const char* label, const ImVec2& size, ImVec4 preferred, ImVec4 surface) {
    // Let the normal button submit its ID, geometry, input and background.
    // Paint the same glyph after its current-frame input state is known.
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0, 0, 0, 0));
    const bool clicked = ImGui::Button(label, size);
    ImGui::PopStyleColor();
    if (ImGui::IsItemVisible()) {
        const bool hovered = ImGui::IsItemHovered();
        const ImGuiCol fill = hovered && ImGui::IsItemActive() ? ImGuiCol_ButtonActive :
            hovered ? ImGuiCol_ButtonHovered : ImGuiCol_Button;
        const auto renderedFill = ImGui::ColorConvertU32ToFloat4(ImGui::GetColorU32(fill));
        const auto background = TransportContrast::Over(renderedFill, surface);
        ImGui::PushStyleColor(ImGuiCol_Text, TransportContrast::ForSurface(preferred, background, 3.0));
        const auto& style = ImGui::GetStyle();
        const ImRect bounds(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
        const ImVec2 textSize = ImGui::CalcTextSize(label, nullptr, true);
        ImGui::RenderTextClipped(ImVec2(bounds.Min.x + style.FramePadding.x, bounds.Min.y + style.FramePadding.y),
            ImVec2(bounds.Max.x - style.FramePadding.x, bounds.Max.y - style.FramePadding.y),
            label, nullptr, &textSize, style.ButtonTextAlign, &bounds);
        ImGui::PopStyleColor();
    }
    return clicked;
}
}
