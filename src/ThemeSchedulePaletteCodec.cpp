#include "ThemeSchedulePaletteCodec.h"
#include "ThemeEditorSchema.h"
#include "nlohmann_json.hpp"
#include <cmath>

namespace harken::theme_schedule {
namespace {
template<class Fn> void ForEachColor(Fn fn) {
    size_t count = 0;
    const auto* fields = GetThemeEditorItems(count);
    for (size_t i = 0; i < count; ++i) {
        if (fields[i].member) fn(fields[i].key, fields[i].member);
    }
    // These are part of the full scheme but have no legacy editor controls.
#define COLOR(name) fn(#name, &ThemeColorScheme::name)
    COLOR(sidebarRailDivider); COLOR(sidebarIconInactive);
    COLOR(sidebarIconHover); COLOR(sidebarIconActive); COLOR(sidebarActiveStrip);
    COLOR(sidebarSubHeaderLine);
    COLOR(wwiseConnectOk); COLOR(wwiseConnectFail); COLOR(wwiseDiffNeutral);
    COLOR(wwiseDiffNew); COLOR(wwiseDiffReplaced); COLOR(wwiseDiffUnresolved);
    COLOR(wwiseLeafBadge); COLOR(wwiseParentBadge);
#undef COLOR
}
bool ValidChannel(float value) { return std::isfinite(value) && value >= 0.f && value <= 1.f; }
bool ValidColor(const ImVec4& color) {
    return ValidChannel(color.x) && ValidChannel(color.y) && ValidChannel(color.z) && ValidChannel(color.w);
}
bool ColorsEqual(const ImVec4& left, const ImVec4& right) {
    return left.x == right.x && left.y == right.y && left.z == right.z && left.w == right.w;
}
bool ReadColor(const nlohmann::json& values, ImVec4& color) {
    if (!values.is_array() || values.size() != 4) return false;
    for (const auto& value : values) if (!value.is_number()) return false;
    color = ImVec4(values[0].get<float>(), values[1].get<float>(),
                   values[2].get<float>(), values[3].get<float>());
    return true;
}
}

bool PalettesEqual(const ThemeColorScheme& a, const ThemeColorScheme& b) {
    bool equal = true;
    ForEachColor([&](const char*, auto member) {
        equal = equal && ColorsEqual(a.*member, b.*member);
    });
    for (const auto* field : kOptionalThemeColors) {
        const auto& left = a.*(field->member);
        const auto& right = b.*(field->member);
        if (left.has_value() != right.has_value() || (left && !ColorsEqual(*left, *right)))
            return false;
    }
    return equal;
}

bool ValidatePalette(const ThemeColorScheme& scheme, Period /*period*/, std::string& error) {
    error.clear();
    ForEachColor([&](const char*, auto member) {
        if (!ValidColor(scheme.*member)) error = "invalid_color";
    });
    for (const auto* field : kOptionalThemeColors) {
        if (const auto& color = scheme.*(field->member); color && !ValidColor(*color))
            error = "invalid_color";
    }
    return error.empty();
}

std::string EncodePalette(const ThemeColorScheme& scheme) {
    nlohmann::json colors = nlohmann::json::object();
    ForEachColor([&](const char* key, auto member) {
        const auto& c = scheme.*member;
        colors[key] = {c.x, c.y, c.z, c.w};
    });
    for (const auto* field : kOptionalThemeColors) {
        if (const auto& color = scheme.*(field->member))
            colors[field->key] = {color->x, color->y, color->z, color->w};
    }
    return nlohmann::json({{"version", 1}, {"colors", colors}}).dump(2) + "\n";
}

bool DecodePalette(const std::string& text, Period period, ThemeColorScheme& destination, std::string& error) {
    try {
        const auto json = nlohmann::json::parse(text);
        if (json.at("version") != 1 || !json.at("colors").is_object()) {
            error = "invalid_palette";
            return false;
        }
        const auto& colors = json.at("colors");
        ThemeColorScheme candidate;
        bool valid = true;
        ForEachColor([&](const char* key, auto member) {
            if (!ReadColor(colors.at(key), candidate.*member)) valid = false;
        });
        for (const auto* field : kOptionalThemeColors) {
            const auto found = colors.find(field->key);
            if (found == colors.end()) continue;
            ImVec4 color;
            if (!ReadColor(*found, color)) valid = false;
            else candidate.*(field->member) = color;
        }
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
