#pragma once
#include "Theme.h"
#include "TransportContrast.h"
#include <array>
#include <vector>
namespace EmptyStatePalette {
// A single text color must remain readable across all gradient corners and
// normal/hover/active button fills. Leave a small margin for rasterization.
inline ImVec4 Readable(ImVec4 preferred, const std::vector<ImVec4>& surfaces,
                       double target=4.8, double minimum=4.5) {
    using namespace TransportContrast;
    auto passes = [&](ImVec4 c, double floor) {
        for (auto bg : surfaces)
            if (Ratio(Quantize(c), Quantize(bg)) < floor) return false;
        return true;
    };
    if (passes(preferred, target)) return preferred;
    preferred.w = 1;
    if (passes(preferred, target)) return preferred;
    for (double floor : {target, minimum}) {
        ImVec4 best = preferred;
        float least = 2;
        for (float end : {0.f, 1.f}) {
            if (!passes(ImVec4(end,end,end,1),floor)) continue;
            float low = 0, high = 1;
            for (int i=0; i<20; ++i) {
                const float t=(low+high)*.5f;
                ImVec4 c(preferred.x+(end-preferred.x)*t,
                    preferred.y+(end-preferred.y)*t,preferred.z+(end-preferred.z)*t,1);
                if (passes(c,floor)) high=t; else low=t;
            }
            if (high<least) {
                least=high;
                best=ImVec4(preferred.x+(end-preferred.x)*high,
                    preferred.y+(end-preferred.y)*high,preferred.z+(end-preferred.z)*high,1);
            }
        }
        if (least<=1) return best;
    }
    return ForSurface(preferred,surfaces.front());
}
struct Colors {
    ImVec4 panelBg;
    ImVec4 panelGlow;
    ImVec4 panelBorder;
    ImVec4 panelAccentLine;
    ImVec4 titleColor;
    ImVec4 bodyColor;
    ImVec4 hintColor;
    ImVec4 separatorColor;
    ImVec4 primaryButtonBg;
    ImVec4 primaryButtonHover;
    ImVec4 primaryButtonActive;
    ImVec4 primaryButtonText;
    ImVec4 secondaryButtonBg;
    ImVec4 secondaryButtonHover;
    ImVec4 secondaryButtonActive;
    ImVec4 secondaryButtonText;
    std::array<ImVec4,4> surfaces;
};
inline Colors Make(const ThemeColorScheme& tcs, ImVec4 hostBackground) {
    auto mixColor = [](const ImVec4& a, const ImVec4& b, float t) -> ImVec4 {
        return ImVec4(
            a.x + (b.x - a.x) * t,
            a.y + (b.y - a.y) * t,
            a.z + (b.z - a.z) * t,
            a.w + (b.w - a.w) * t);
    };
    auto withAlpha = [](const ImVec4& color, float alpha) -> ImVec4 {
        return ImVec4(color.x, color.y, color.z, alpha);
    };
    auto luminance = [](const ImVec4& color) -> float {
        return color.x * 0.299f + color.y * 0.587f + color.z * 0.114f;
    };

    const ImVec4 panelBg =
        withAlpha(mixColor(tcs.resultsBG, tcs.dialogPanelBG, 0.34f), 0.98f);
    const ImVec4 panelGlow =
        withAlpha(mixColor(panelBg, tcs.searchButtonBG, 0.10f), 0.24f);
    const ImVec4 panelBorder =
        withAlpha(mixColor(tcs.resultsOutline, tcs.controlAccent, 0.16f), 0.52f);
    const ImVec4 panelAccentLine =
        withAlpha(mixColor(tcs.searchButtonBG, tcs.controlAccent, 0.20f), 0.70f);
    const ImVec4 titleColor =
        withAlpha(mixColor(tcs.filterBrowserHeading, tcs.controlAccent, 0.12f), 0.98f);
    const ImVec4 bodyColor =
        withAlpha(mixColor(tcs.resultsText, tcs.resultsSecondaryText, 0.12f), 0.96f);
    const ImVec4 hintColor =
        withAlpha(mixColor(tcs.resultsSecondaryText, tcs.resultsText, 0.10f), 0.88f);
    const ImVec4 separatorColor =
        withAlpha(mixColor(panelBorder, panelAccentLine, 0.12f), 0.74f);

    ImVec4 primaryButtonBg =
        withAlpha(mixColor(tcs.searchButtonBG, panelBg, 0.10f), 0.96f);
    if (TransportContrast::Ratio(primaryButtonBg,panelBg)<1.3)
        primaryButtonBg=withAlpha(mixColor(tcs.controlAccent,panelBg,.18f),1);
    const ImVec4 primaryButtonHover =
        withAlpha(mixColor(primaryButtonBg, tcs.resultsText, 0.08f), 1.0f);
    const ImVec4 primaryButtonActive =
        withAlpha(mixColor(primaryButtonBg, tcs.resultsText, 0.16f), 1.0f);
    const ImVec4 primaryButtonText =
        luminance(primaryButtonBg) >= 0.62f
            ? withAlpha(mixColor(tcs.mainWindowBG, tcs.interfaceText, 0.20f), 0.98f)
            : withAlpha(tcs.resultsText, 0.98f);

    const ImVec4 secondaryButtonBg =
        withAlpha(mixColor(panelBg, tcs.frameBgBase, 0.26f), 0.98f);
    const ImVec4 secondaryButtonHover =
        withAlpha(mixColor(secondaryButtonBg, tcs.searchButtonBG, 0.10f), 1.0f);
    const ImVec4 secondaryButtonActive =
        withAlpha(mixColor(secondaryButtonBg, tcs.searchButtonBG, 0.18f), 1.0f);
    const ImVec4 secondaryButtonText =
        withAlpha(mixColor(tcs.resultsText, tcs.resultsSecondaryText, 0.08f), 0.95f);

    Colors p;
    p.panelBg = panelBg;
    p.panelGlow = panelGlow;
    p.panelBorder = panelBorder;
    p.panelAccentLine = panelAccentLine;
    p.titleColor = titleColor;
    p.bodyColor = bodyColor;
    p.hintColor = hintColor;
    p.separatorColor = separatorColor;
    p.primaryButtonBg = primaryButtonBg;
    p.primaryButtonHover = primaryButtonHover;
    p.primaryButtonActive = primaryButtonActive;
    p.primaryButtonText = primaryButtonText;
    p.secondaryButtonBg = secondaryButtonBg;
    p.secondaryButtonHover = secondaryButtonHover;
    p.secondaryButtonActive = secondaryButtonActive;
    p.secondaryButtonText = secondaryButtonText;
    using namespace TransportContrast;
    const auto base = Over(Quantize(panelBg), Quantize(hostBackground));
    p.surfaces = {Over(Quantize(panelGlow),base),
        Over(Quantize(withAlpha(panelGlow,.12f)),base),
        Over(Quantize(withAlpha(mixColor(panelBg,tcs.mainWindowBG,.10f),.18f)),base),
        Over(Quantize(withAlpha(mixColor(panelBg,tcs.mainWindowBG,.16f),.24f)),base)};
    const std::vector<ImVec4> panelSurfaces(p.surfaces.begin(),p.surfaces.end());
    // The secondary action must read as a whole button, not loose text on
    // the card. Use a muted theme tint with a measurable surface separation.
    p.secondaryButtonBg=Readable(withAlpha(mixColor(panelBg,tcs.controlAccent,.25f),1),
        panelSurfaces,3.2,3.05);
    const float toward=TransportContrast::Luminance(p.secondaryButtonBg)>
        TransportContrast::Luminance(p.surfaces.front()) ? 1.f : 0.f;
    p.secondaryButtonHover=mixColor(p.secondaryButtonBg,ImVec4(toward,toward,toward,1),.035f);
    p.secondaryButtonActive=mixColor(p.secondaryButtonBg,ImVec4(toward,toward,toward,1),.07f);
    p.titleColor=Readable(p.titleColor,panelSurfaces);
    p.bodyColor=Readable(p.bodyColor,panelSurfaces);
    p.hintColor=Readable(p.hintColor,panelSurfaces);
    std::vector<ImVec4> primarySurfaces,secondarySurfaces;
    for (auto surface:p.surfaces) {
        for (auto bg:{p.primaryButtonBg,p.primaryButtonHover,p.primaryButtonActive})
            primarySurfaces.push_back(Over(Quantize(bg),surface));
        for (auto bg:{p.secondaryButtonBg,p.secondaryButtonHover,p.secondaryButtonActive})
            secondarySurfaces.push_back(Over(Quantize(bg),surface));
    }
    p.primaryButtonText=Readable(p.primaryButtonText,primarySurfaces);
    p.secondaryButtonText=Readable(p.secondaryButtonText,secondarySurfaces);
    return p;
}
} // namespace EmptyStatePalette
