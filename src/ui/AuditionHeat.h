#pragma once

#include <algorithm>
#include <cstdint>

namespace harken::results {

struct AuditionHeatRailLayout {
    float tableWidth;
    float rightX;
};

inline constexpr AuditionHeatRailLayout LayoutAuditionHeatRail(
    float viewportLeft, float viewportWidth, float preferredWidth) noexcept {
    const float width = (std::max)(0.0f, viewportWidth);
    const float railWidth = (std::min)(preferredWidth, width * 0.5f);
    return {width - railWidth, viewportLeft + width};
}

inline constexpr std::uint32_t AuditionHeatRgb(int playCount, bool lightBackground = false) noexcept {
    if (playCount >= 1000) return lightBackground ? 0x9A4E12 : 0xEBA56B;
    if (playCount >= 200) return lightBackground ? 0x6743A9 : 0xAA93E8;
    if (playCount >= 100) return lightBackground ? 0x176D72 : 0x52C2C5;
    if (playCount >= 50) return lightBackground ? 0x245EAE : 0x679AEB;
    if (playCount >= 10) return lightBackground ? 0x536981 : 0x8395AA;
    return 0;
}

}
