#pragma once
#include "imgui.h"
#include "imgui_internal.h"
#include <cstdio>
#include "TransportValueText.h"

namespace TransportBlendSlider {
inline bool Draw(const char* label, float* percent, ImU32 ink, ImU32 background) {
    auto* draw = ImGui::GetWindowDrawList();
    const int firstVertex=draw->VtxBuffer.Size;
    ImGui::PushStyleColor(ImGuiCol_Text, ink);
    const auto nativeInk = ImGui::GetColorU32(ImGuiCol_Text);
    const bool changed = ImGui::SliderFloat(label, percent, 0.f, 100.f, "%.0f%%");
    ImGui::PopStyleColor();
    // Preserve the native editor, including its caret and selection.
    if (ImGui::TempInputIsActive(ImGui::GetItemID())) return changed;
    const ImVec2 minimum = ImGui::GetItemRectMin(), maximum = ImGui::GetItemRectMax();
    char text[16]; std::snprintf(text, sizeof(text), "%.0f%%", *percent);
    const ImVec2 size = ImGui::CalcTextSize(text);
    const ImVec2 position((minimum.x + maximum.x - size.x) * .5f,
        (minimum.y + maximum.y - size.y) * .5f);
    const auto white=ImGui::GetFontTexUvWhitePixel();
    const auto grabColor=ImGui::GetColorU32(ImGui::IsItemActive() ? ImGuiCol_SliderGrabActive : ImGuiCol_SliderGrab);
    float grabMin=maximum.x,grabMax=minimum.x;
    for(int i=firstVertex;i<draw->VtxBuffer.Size;++i) {
        auto& v=draw->VtxBuffer[i];
        const bool glyph=(v.uv.x!=white.x || v.uv.y!=white.y) && v.col==nativeInk &&
            v.pos.x>=std::floor(position.x)-1 && v.pos.x<=position.x+size.x+1 &&
            v.pos.y>=std::floor(position.y)-1 && v.pos.y<=position.y+size.y+1;
        if(glyph) v.col &= ~IM_COL32_A_MASK; // Replace native value ink, never its frame.
        else if(v.col==grabColor) { grabMin=std::min(grabMin,v.pos.x);grabMax=std::max(grabMax,v.pos.x); }
    }
    const auto frameColor=ImGui::GetColorU32(ImGui::IsItemActive() ? ImGuiCol_FrameBgActive :
        ImGui::IsItemHovered() ? ImGuiCol_FrameBgHovered : ImGuiCol_FrameBg);
    const auto frame=TransportContrast::Over(ImGui::ColorConvertU32ToFloat4(frameColor),ImGui::ColorConvertU32ToFloat4(background));
    const auto grab=TransportContrast::Over(ImGui::ColorConvertU32ToFloat4(grabColor),frame);
    TransportValueText::Draw(draw,position,text,ImGui::ColorConvertU32ToFloat4(ink),
        [&](float x) { return x>=grabMin && x<grabMax ? grab : frame; },"spectrum.blend");
    return changed;
}
}
