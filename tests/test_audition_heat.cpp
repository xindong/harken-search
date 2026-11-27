#include "ui/AuditionHeat.h"

#include <cstdint>
#include <iostream>
#include <limits>

struct TestCase {
    int count;
    std::uint32_t expectedRgb;
};

int main() {
    constexpr TestCase cases[] = {
        {-1, 0}, {0, 0}, {9, 0},
        {10, 0x8395AA}, {11, 0x8395AA}, {49, 0x8395AA},
        {50, 0x679AEB}, {51, 0x679AEB}, {99, 0x679AEB},
        {100, 0x52C2C5}, {101, 0x52C2C5}, {199, 0x52C2C5},
        {200, 0xAA93E8}, {201, 0xAA93E8}, {999, 0xAA93E8},
        {1000, 0xEBA56B}, {1001, 0xEBA56B},
        {std::numeric_limits<int>::max(), 0xEBA56B},
    };
    for (const auto& test : cases) {
        if (harken::results::AuditionHeatRgb(test.count) != test.expectedRgb) {
            std::cerr << "Unexpected audition heat color for " << test.count << " plays\n";
            return 1;
        }
    }
    const auto narrow = harken::results::LayoutAuditionHeatRail(262.0f, 1010.0f, 88.0f);
    const auto wide = harken::results::LayoutAuditionHeatRail(211.0f, 1587.0f, 88.0f);
    const auto browser = harken::results::LayoutAuditionHeatRail(262.0f, 1010.0f, 0.0f);
    const auto compact = harken::results::LayoutAuditionHeatRail(10.0f, 60.0f, 88.0f);
    if (narrow.tableWidth != 922.0f || narrow.rightX != 1272.0f ||
        wide.tableWidth != 1499.0f || wide.rightX != 1798.0f ||
        browser.tableWidth != 1010.0f || browser.rightX != 1272.0f ||
        compact.tableWidth != 30.0f || compact.rightX != 70.0f) {
        std::cerr << "Audition heat rail did not reserve the visible right edge\n";
        return 1;
    }
    std::cout << "Audition heat: 18 threshold and blank cases, 4 right-edge layouts passed\n";
}
