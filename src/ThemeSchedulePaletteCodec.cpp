#include "ThemeSchedulePaletteCodec.h"
#include "ThemeEditorSchema.h"
#include "nlohmann_json.hpp"
#include <cmath>

namespace harken::theme_schedule {
namespace {
template<class Fn> void ForEachColor(Fn fn) {
    size_t count = 0;
    const auto* fields = GetThemeEditorItems(count);
    for (size_t i = 0; i < count; ++i) fn(fields[i].key, fields[i].member);
    // These are part of the full scheme but have no legacy editor controls.
#define COLOR(name) fn(#name, &ThemeColorScheme::name)
    COLOR(sidebarRailBG); COLOR(sidebarRailDivider); COLOR(sidebarIconInactive);
    COLOR(sidebarIconHover); COLOR(sidebarIconActive); COLOR(sidebarActiveStrip);
    COLOR(sidebarHeaderBG); COLOR(sidebarStatusBarBG); COLOR(sidebarSubHeaderLine);
    COLOR(wwiseConnectOk); COLOR(wwiseConnectFail); COLOR(wwiseDiffNeutral);
    COLOR(wwiseDiffNew); COLOR(wwiseDiffReplaced); COLOR(wwiseDiffUnresolved);
    COLOR(wwiseLeafBadge); COLOR(wwiseParentBadge);
#undef COLOR
}
bool ValidChannel(float value) { return std::isfinite(value) && value >= 0.f && value <= 1.f; }
float Luma(const ImVec4& color) { return .299f*color.x + .587f*color.y + .114f*color.z; }
}

bool PalettesEqual(const ThemeColorScheme& a, const ThemeColorScheme& b) {
    bool equal = true;
    ForEachColor([&](const char*, auto member) {
        const auto& x = a.*member;
        const auto& y = b.*member;
        equal = equal && x.x == y.x && x.y == y.y && x.z == y.z && x.w == y.w;
    });
    return equal;
}

bool ValidatePalette(const ThemeColorScheme& scheme, Period period, std::string& error) {
    error.clear();
    ForEachColor([&](const char*, auto member) {
        const auto& c = scheme.*member;
        if (!ValidChannel(c.x) || !ValidChannel(c.y) || !ValidChannel(c.z) || !ValidChannel(c.w))
            error = "invalid_color";
    });
    if (!error.empty()) return false;
    // Keep the four principal surfaces light by day and dark by night. Accent
    // colors remain freely editable (including dark text on a light surface).
    for (const auto c : {scheme.mainWindowBG, scheme.resultsBG, scheme.dialogBG, scheme.waveformBG}) {
        if ((Luma(c) >= .72f) != (period == Period::Day) || c.w < .9f) {
            error = period == Period::Day ? "day_requires_light" : "night_requires_dark";
            return false;
        }
    }
    return true;
}

std::string EncodePalette(const ThemeColorScheme& scheme) {
    nlohmann::json colors = nlohmann::json::object();
    ForEachColor([&](const char* key, auto member) {
        const auto& c = scheme.*member;
        colors[key] = {c.x, c.y, c.z, c.w};
    });
    return nlohmann::json({{"version", 1}, {"colors", colors}}).dump(2) + "\n";
}

bool DecodePalette(const std::string& text, Period period, ThemeColorScheme& destination, std::string& error) {
    try {
        const auto json = nlohmann::json::parse(text);
        if (json.at("version") != 1 || !json.at("colors").is_object()) {
            error = "invalid_palette";
            return false;
        }
        ThemeColorScheme candidate;
        bool valid = true;
        ForEachColor([&](const char* key, auto member) {
            const auto& values = json.at("colors").at(key);
            if (!values.is_array() || values.size() != 4) { valid = false; return; }
            for (const auto& value : values) if (!value.is_number()) { valid = false; return; }
            candidate.*member = ImVec4(values[0].get<float>(), values[1].get<float>(),
                                      values[2].get<float>(), values[3].get<float>());
        });
        if (!valid) { error = "invalid_palette"; return false; }
        if (!ValidatePalette(candidate, period, error)) return false;
        destination = candidate;
        return true;
    } catch (const nlohmann::json::exception&) {
        error = "invalid_palette";
        return false;
    }
}
}
