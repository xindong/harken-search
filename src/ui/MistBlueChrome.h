#pragma once
#include "Theme.h"
#include "imgui.h"
#include "TransportContrast.h"
namespace MistBlueChrome {
struct EqStateColors { ImVec4 button, ink, signal; };
inline EqStateColors EqState(const ThemeColorScheme& s, ImVec4 surface,
                            bool enabled, bool hovered = false, bool pressed = false) {
    const auto mix = [](ImVec4 a, ImVec4 b, float t) {
        return ImVec4(a.x+(b.x-a.x)*t, a.y+(b.y-a.y)*t, a.z+(b.z-a.z)*t, 1);
    };
    const ImVec4 button = enabled
        ? (pressed ? mix(s.controlAccent, surface, .15f)
                   : mix(s.controlAccent, s.interfaceText, hovered ? .12f : 0.f))
        : mix(surface, s.chromeBorder, hovered ? .08f : 0.f);
    const ImVec4 ink = TransportContrast::ForSurface(enabled ? surface : s.resultsSecondaryText, button, 4.7);
    ImVec4 signal = enabled ? s.controlAccent : s.interfaceText;
    if (!enabled) signal.w = .30f;
    signal = TransportContrast::Over(TransportContrast::ForSurface(signal, surface, 3.2), surface);
    return {button, ink, signal};
}
inline EqStateColors EqModeState(const ThemeColorScheme& s, ImVec4 surface,
                                bool enabled, bool notch, bool hovered = false, bool pressed = false) {
    if (!enabled) return EqState(s, surface, false, hovered, pressed);
    const auto mix = [](ImVec4 a, ImVec4 b, float t) {
        return ImVec4(a.x+(b.x-a.x)*t, a.y+(b.y-a.y)*t, a.z+(b.z-a.z)*t, 1);
    };
    // Pass stays in the navy family; Notch carries the pale blue accent.
    // The mode distinction is independent of the EQ power switch.
    const ImVec4 button = notch
        ? (pressed ? mix(s.controlAccent, surface, .12f)
                   : mix(s.controlAccent, s.interfaceText, hovered ? .10f : 0.f))
        : mix(s.modeCapsuleBG, s.interfaceText, pressed ? .10f : (hovered ? .14f : 0.f));
    const ImVec4 ink = TransportContrast::ForSurface(notch ? surface : s.interfaceText, button, 4.7);
    return {button, ink, notch ? s.controlAccent : s.chromeBorder};
}
struct ControlSurfaces { ImVec4 normal, hovered, active, title, muted; };
inline ControlSurfaces Controls(const ThemeColorScheme& s) {
    return {s.frameBgBase, s.resultsTabHover, s.modeCapsuleActiveBG,
            s.toolbarBG, s.resultsSecondaryText};
}
// Applied only to the built-in LapisBlue preset. Keep the light icon accent
// separate from the navy surfaces carrying light text.
inline void Apply(ImVec4* colors,const ThemeColorScheme& s) {
    const auto c = Controls(s);
    colors[ImGuiCol_FrameBg]=c.normal;
    colors[ImGuiCol_FrameBgHovered]=c.hovered;
    colors[ImGuiCol_FrameBgActive]=c.active;
    colors[ImGuiCol_Button]=c.normal;
    colors[ImGuiCol_ButtonHovered]=c.hovered;
    // Muted labels are still information, not disabled controls. Actual disabled
    // widgets retain ImGui DisabledAlpha on top of this readable base.
    colors[ImGuiCol_TextDisabled]=s.resultsSecondaryText;
    colors[ImGuiCol_ButtonActive]=c.active;
    colors[ImGuiCol_TabActive]=s.resultsTabActive;
    colors[ImGuiCol_TabHovered]=s.resultsTabHover;
    colors[ImGuiCol_HeaderActive]=s.historyCapsuleActiveBG;
}
inline ImVec4 ResultsTabText(const ThemeColorScheme& s) { return s.resultsText; }
}
