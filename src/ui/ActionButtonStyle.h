#pragma once

#include "../HarkenSearchWidgets.h"
#include "../BuildConfig.h"
#include "EmptyStatePalette.h"
#include "imgui_internal.h"

namespace aura::ui::ActionButtonStyle {

#if ARIS_PREVIEW_ISOLATION && AURA_BUILD_DIAGNOSTICS
struct Observation {
    std::string label, window;
    ImVec2 min, max;
    ImVec4 clip;
    ImU32 text, fill;
    bool disabled;
};
inline int auditUntil = -1, auditFrame = -1;
inline std::vector<Observation> observations;
inline void RequestAudit() { auditUntil = ImGui::GetFrameCount() + 300; }
#endif

inline void Record(const char* label) {
#if ARIS_PREVIEW_ISOLATION && AURA_BUILD_DIAGNOSTICS
    if (ImGui::GetFrameCount() > auditUntil || !ImGui::IsItemVisible()) return;
    if (auditFrame != ImGui::GetFrameCount()) {
        auditFrame = ImGui::GetFrameCount();
        observations.clear();
    }
    const auto* window = ImGui::GetCurrentWindow();
    const ImGuiCol fill = ImGui::IsItemActive() ? ImGuiCol_ButtonActive :
        ImGui::IsItemHovered() ? ImGuiCol_ButtonHovered : ImGuiCol_Button;
    observations.push_back({std::string(label, ImGui::FindRenderedTextEnd(label)), window->Name,
        ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), window->ClipRect.ToVec4(),
        ImGui::GetColorU32(ImGuiCol_Text), ImGui::GetColorU32(fill),
        (GImGui->LastItemData.ItemFlags & ImGuiItemFlags_Disabled) != 0});
#else
    (void)label;
#endif
}

// Shared ordinary-button paint. Layout, operations and custom graphic controls
// belong to callers. Every push is local to one button and is restored by Scope.
inline constexpr float kRounding = 6.0f;
struct Palette {
    ImVec4 fill, hover, active, text, border;
    float borderWidth = 1.0f;
};

inline Palette Make(ImVec4 surface, ImVec4 accent, ImVec4 body,
                    ImVec4 primaryText, ButtonVariant variant) {
    const bool primary = variant == ButtonVariant::PrimarySearch ||
                         variant == ButtonVariant::PrimaryTransfer ||
                         variant == ButtonVariant::Danger;
    const bool quiet = variant == ButtonVariant::Quiet;
    accent = ImLerp(TransportContrast::Over(accent, surface), ImVec4(1, 1, 1, 1),
                    TransportContrast::Luminance(surface) > .5 ? 0.0f : .10f);
    Palette p;
    p.fill = (primary || variant == ButtonVariant::Selected)
        ? accent : ImLerp(surface, accent, quiet ? .07f : .24f);
    p.fill.w = 1.0f;
    p.text = primary ? primaryText : body;
    p.text.w = 1.0f;
    p.hover = ImLerp(p.fill, p.text, .06f);
    p.active = ImLerp(p.fill, p.text, .11f);
    p.text = EmptyStatePalette::Readable(p.text, {p.fill, p.hover, p.active});
    // A middle-luminance accent can cross the black/white contrast boundary
    // when darkened on press. Keep all states on the readable side in that case.
    if (TransportContrast::Ratio(TransportContrast::Quantize(p.text), TransportContrast::Quantize(p.hover)) < 4.5 ||
        TransportContrast::Ratio(TransportContrast::Quantize(p.text), TransportContrast::Quantize(p.active)) < 4.5) {
        p.text = TransportContrast::ForSurface(p.text, p.fill, 4.8);
        const bool darkInk = TransportContrast::Luminance(p.text) < TransportContrast::Luminance(p.fill);
        const float end = darkInk ? 1.0f : 0.0f;
        const ImVec4 toward(end, end, end, 1);
        p.hover = ImLerp(p.fill, toward, .06f);
        p.active = ImLerp(p.fill, toward, .11f);
    }
    p.border = ImLerp(surface, body, .20f);
    p.border.w = 1.0f;
    p.borderWidth = primary ? 0.0f : 1.0f;
    return p;
}

inline Palette Make(const ThemeColorScheme& theme, ImVec4 surface, ButtonVariant variant) {
    surface = TransportContrast::Over(surface, theme.mainWindowBG);
    if (variant == ButtonVariant::Selected) {
        // Keep the existing warm edit marker inside the common paint scope.
        return Make(surface, ImVec4(.95f, .75f, .2f, .45f),
                    theme.interfaceText, theme.interfaceText, variant);
    }
    const ImVec4 color = variant == ButtonVariant::PrimaryTransfer ? theme.transferButtonBG :
                         variant == ButtonVariant::Danger ? theme.volumeClipColor : theme.searchButtonBG;
    ImVec4 accent = ImLerp(color, surface, .10f);
    accent.w = .96f;
    if (variant != ButtonVariant::Danger && TransportContrast::Ratio(accent, surface) < 1.3) {
        accent = ImLerp(theme.controlAccent, surface, .18f);
        accent.w = 1.0f;
    }
    return Make(surface, accent, theme.interfaceText, theme.interfaceText, variant);
}

inline ImVec4 CurrentSurface() {
    const auto& style = ImGui::GetStyle();
    const auto* window = ImGui::GetCurrentWindow();
    const auto* parent = window;
    while ((parent->Flags & ImGuiWindowFlags_ChildWindow) && parent->ParentWindow)
        parent = parent->ParentWindow;
    ImVec4 surface = style.Colors[(parent->Flags & ImGuiWindowFlags_Popup) ? ImGuiCol_PopupBg : ImGuiCol_WindowBg];
    if (window->Flags & ImGuiWindowFlags_ChildWindow)
        surface = TransportContrast::Over(style.Colors[ImGuiCol_ChildBg], surface);
    return surface;
}

class Scope {
public:
    explicit Scope(const Palette& p) {
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, kRounding);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, p.borderWidth);
        ImGui::PushStyleColor(ImGuiCol_BorderShadow, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_Border, p.border);
        ImGui::PushStyleColor(ImGuiCol_Button, p.fill);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, p.hover);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, p.active);
        ImGui::PushStyleColor(ImGuiCol_Text, p.text);
    }
    ~Scope() { ImGui::PopStyleColor(6); ImGui::PopStyleVar(2); }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
};

inline bool Draw(const char* label, const ImVec2& size, bool loading = false) {
    if (!loading) {
        const bool clicked = ImGui::Button(label, size);
        Record(label);
        return clicked;
    }
    // Submit the same real button/ID. The spinner is drawn inside that item,
    // rather than submitting another item which would disturb layout/focus.
    const auto& style = ImGui::GetStyle();
    const float spinner = ImGui::GetFontSize(), gap = 6.0f;
    const ImVec2 textSize = ImGui::CalcTextSize(label, nullptr, true);
    const ImVec2 actual = ImGui::CalcItemSize(size, textSize.x + 2 * style.FramePadding.x + spinner + gap,
                                                  textSize.y + 2 * style.FramePadding.y);
    ImGui::BeginDisabled();
    const ImU32 textColor = ImGui::GetColorU32(ImGuiCol_Text);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0, 0, 0, 0));
    ImGui::Button(label, actual);
    ImGui::PopStyleColor();
    if (ImGui::IsItemVisible()) {
        const ImVec2 min = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax();
        const float contentWidth = spinner + gap + textSize.x;
        const float x = min.x + ImMax(style.FramePadding.x, (actual.x - contentWidth) * .5f);
        const float centerY = (min.y + max.y) * .5f;
        const ImVec4 clip(min.x + style.FramePadding.x, min.y, max.x - style.FramePadding.x, max.y);
        auto* draw = ImGui::GetWindowDrawList();
        draw->PushClipRect(ImVec2(clip.x, clip.y), ImVec2(clip.z, clip.w), true);
        const float angle = static_cast<float>(ImGui::GetTime() * 6.0);
        draw->PathArcTo(ImVec2(x + spinner * .5f, centerY), spinner * .35f, angle, angle + IM_PI * 1.5f, 16);
        draw->PathStroke(textColor, 0, 1.5f);
        draw->AddText(ImGui::GetFont(), ImGui::GetFontSize(),
                      ImVec2(x + spinner + gap, centerY - textSize.y * .5f), textColor,
                      label, ImGui::FindRenderedTextEnd(label), 0, &clip);
        draw->PopClipRect();
    }
    Record(label);
    ImGui::EndDisabled();
    return false;
}

} // namespace aura::ui::ActionButtonStyle
