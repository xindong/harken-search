#pragma once
#include "TransportContrast.h"
#include "imgui_internal.h"

namespace aura::ui::CloseIconButton {
inline ImVec2 VisibleGlyphCenter(const ImFontGlyph& glyph, ImTextureData* texture) {
    ImVec2 center((glyph.X0 + glyph.X1) * .5f, (glyph.Y0 + glyph.Y1) * .5f);
    if (!texture || !texture->Pixels) return center;
    const int left = int(ImFloor(glyph.U0 * texture->Width + .5f));
    const int top = int(ImFloor(glyph.V0 * texture->Height + .5f));
    const int right = int(ImFloor(glyph.U1 * texture->Width + .5f));
    const int bottom = int(ImFloor(glyph.V1 * texture->Height + .5f));
    if (left < 0 || top < 0 || right > texture->Width || bottom > texture->Height ||
        left >= right || top >= bottom) return center;
    int minX = right, minY = bottom, maxX = left - 1, maxY = top - 1;
    for (int y = top; y < bottom; ++y) for (int x = left; x < right; ++x) {
        const auto* pixel = static_cast<const unsigned char*>(texture->GetPixelsAt(x, y));
        const auto alpha = texture->Format == ImTextureFormat_Alpha8 ? pixel[0] : pixel[3];
        if (alpha) {
            minX = ImMin(minX, x); minY = ImMin(minY, y);
            maxX = ImMax(maxX, x); maxY = ImMax(maxY, y);
        }
    }
    if (minX <= maxX && minY <= maxY) {
        center.x = glyph.X0 + ((minX + maxX + 1) * .5f - left) / (right - left) * (glyph.X1 - glyph.X0);
        center.y = glyph.Y0 + ((minY + maxY + 1) * .5f - top) / (bottom - top) * (glyph.Y1 - glyph.Y0);
    }
    return center;
}

inline bool Draw(const char* label, const ImVec2& size, ImVec4 preferred, ImVec4 surface) {
    // Keep the normal button's ID, hit area and input, but paint only the X.
    // Focus and pointer feedback live on the glyph, never on a backplate.
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_NavCursor, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
    const bool clicked = ImGui::Button(label, size);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(5);
    if (ImGui::IsItemVisible()) {
        // ImGui also reports the highlighted keyboard navigation item here.
        const bool hovered = ImGui::IsItemHovered();
        const float emphasis = hovered && ImGui::IsItemActive() ? .55f :
            hovered ? .30f : 0.0f;
        const ImVec4 black(0, 0, 0, 1), white(1, 1, 1, 1);
        const auto strongest = TransportContrast::Ratio(black, surface) >
            TransportContrast::Ratio(white, surface) ? black : white;
        const auto ink = TransportContrast::ForSurface(ImLerp(preferred, strongest, emphasis), surface, 3.0);
        const ImRect bounds(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
        unsigned int codepoint = 0;
        const int length = ImTextCharFromUtf8(&codepoint, label, nullptr);
        auto* baked = ImGui::GetFontBaked();
        if (const auto* glyph = baked->FindGlyph((ImWchar)codepoint)) {
            // Lucide's rasterized quad also has unequal transparent margins.
            // Center its visible pixels, not the advance or the padded quad.
            const float scale = ImGui::GetFontSize() / baked->Size;
            const auto center = bounds.GetCenter();
            const auto glyphCenter = VisibleGlyphCenter(*glyph, ImGui::GetFont()->OwnerAtlas->TexData);
            const ImVec2 origin(ImFloor(center.x - glyphCenter.x * scale + .5f),
                                ImFloor(center.y - glyphCenter.y * scale + .5f));
            const ImVec4 clip(bounds.Min.x, bounds.Min.y, bounds.Max.x, bounds.Max.y);
            ImGui::GetWindowDrawList()->AddText(ImGui::GetFont(), ImGui::GetFontSize(), origin,
                ImGui::GetColorU32(ink), label, label + length, 0.0f, &clip);
        }
    }
    return clicked;
}
}
