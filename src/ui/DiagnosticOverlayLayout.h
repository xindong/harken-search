#pragma once

#include <algorithm>
#include <cmath>

namespace harken::diagnostic_overlay::layout {

struct Size { float width, height; };
struct Inset { float right, top; };
struct Placement { Size size; Inset inset; };
struct Bounds { float scale; Size minimum, maximum; };

inline constexpr Placement kDefaultPlacement{{420.0f, 304.0f}, {10.0f, 10.0f}};
inline constexpr Size kMinimumSize{352.0f, 270.0f};

inline Bounds WindowBounds(Size work, float dpiScale) {
    const float scale = std::min({dpiScale, work.width / kMinimumSize.width, work.height / kMinimumSize.height});
    return {scale,
            {std::max(1.0f, std::floor(kMinimumSize.width * scale)),
             std::max(1.0f, std::floor(kMinimumSize.height * scale))},
            work};
}

inline Placement FitPlacement(Placement preferred, const Bounds& bounds) {
    const float scale = bounds.scale;
    const Size size{std::clamp(std::round(preferred.size.width * scale), bounds.minimum.width, bounds.maximum.width),
                    std::clamp(std::round(preferred.size.height * scale), bounds.minimum.height, bounds.maximum.height)};
    return {size, {std::clamp(std::round(preferred.inset.right * scale), 0.0f, bounds.maximum.width - size.width),
                   std::clamp(std::round(preferred.inset.top * scale), 0.0f, bounds.maximum.height - size.height)}};
}

inline Size ResizeMaximum(const Bounds& bounds, Placement placed, bool fromLeft, bool fromTop) {
    const float width = fromLeft ? bounds.maximum.width - placed.inset.right : placed.size.width + placed.inset.right;
    const float height = fromTop ? placed.size.height + placed.inset.top : bounds.maximum.height - placed.inset.top;
    return {width, height};
}

inline void RememberResize(Placement& preferred, Placement fitted, Placement actual, float scale) {
    if (actual.size.width != fitted.size.width) preferred.size.width = actual.size.width / scale;
    if (actual.size.height != fitted.size.height) preferred.size.height = actual.size.height / scale;
    if (actual.inset.right != fitted.inset.right) preferred.inset.right = actual.inset.right / scale;
    if (actual.inset.top != fitted.inset.top) preferred.inset.top = actual.inset.top / scale;
}

inline float FontScale(Size size) {
    if (size.width >= 640.0f && size.height >= 448.0f) return 1.25f;
    if (size.width >= 500.0f && size.height >= 352.0f) return 1.125f;
    return 1.0f;
}

}
