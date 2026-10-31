#pragma once
#include "imgui.h"
#include <algorithm>
#include <cmath>
#include <initializer_list>

namespace TransportContrast {
inline ImVec4 Quantize(ImVec4 c) {
    auto q = [](float v) { return std::round(std::clamp(v, 0.f, 1.f) * 255.f) / 255.f; };
    return ImVec4(q(c.x), q(c.y), q(c.z), q(c.w));
}
inline ImVec4 Over(ImVec4 foreground, ImVec4 background) {
    return ImVec4(foreground.x * foreground.w + background.x * (1 - foreground.w),
        foreground.y * foreground.w + background.y * (1 - foreground.w),
        foreground.z * foreground.w + background.z * (1 - foreground.w), 1);
}
inline double Luminance(ImVec4 c) {
    auto linear = [](double x) { return x <= .04045 ? x / 12.92 : std::pow((x + .055) / 1.055, 2.4); };
    return .2126 * linear(c.x) + .7152 * linear(c.y) + .0722 * linear(c.z);
}
inline double Ratio(ImVec4 foreground, ImVec4 background) {
    double a = Luminance(Over(foreground, background)), b = Luminance(background);
    return (std::max(a, b) + .05) / (std::min(a, b) + .05);
}
// Preserve a readable theme color. Otherwise remove opacity and move only as
// far toward light or dark as needed for the actual, quantized draw surface.
inline ImVec4 ForSurface(ImVec4 preferred, ImVec4 background, double minimum = 4.5) {
    background = Quantize(background);
    if (Ratio(Quantize(preferred), background) >= minimum) return preferred;
    const float originalAlpha = preferred.w;
    preferred.w = 1;
    if (Ratio(Quantize(preferred), background) >= minimum) {
        float low = originalAlpha, high = 1;
        for (int i = 0; i < 16; ++i) {
            preferred.w = (low + high) * .5f;
            if (Ratio(Quantize(preferred), background) >= minimum) high = preferred.w; else low = preferred.w;
        }
        preferred.w = high;
        return preferred;
    }
    const ImVec4 black(0, 0, 0, 1), white(1, 1, 1, 1);
    ImVec4 best = Ratio(black, background) > Ratio(white, background) ? black : white;
    float bestAmount = 2;
    for (float end : {0.f, 1.f}) {
        const ImVec4 target(end, end, end, 1);
        if (Ratio(target, background) < minimum) continue;
        float low = 0, high = 1;
        for (int i = 0; i < 16; ++i) {
            const float t = (low + high) * .5f;
            const ImVec4 c(preferred.x + (end - preferred.x) * t,
                preferred.y + (end - preferred.y) * t, preferred.z + (end - preferred.z) * t, 1);
            if (Ratio(Quantize(c), background) >= minimum) high = t; else low = t;
        }
        if (high < bestAmount) {
            bestAmount = high;
            best = ImVec4(preferred.x + (end - preferred.x) * high,
                preferred.y + (end - preferred.y) * high, preferred.z + (end - preferred.z) * high, 1);
        }
    }
    return best;
}
} // namespace TransportContrast
