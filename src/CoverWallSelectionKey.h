#pragma once

#include <string>
#include <string_view>
#include <vector>
#include <limits>

namespace aura::cover_wall {

inline constexpr std::string_view kGroupedSelectionPrefix = "CWGROUP|";
inline constexpr std::string_view kExactSelectionPrefix = "CWEXACT|";

inline std::string EncodeGroupedCoverWallKeys(const std::vector<std::string>& keys) {
    std::string encoded(kGroupedSelectionPrefix);
    for (const auto& key : keys) {
        encoded += std::to_string(key.size());
        encoded += ':';
        encoded += key;
    }
    return encoded;
}

inline std::string EncodeExactCoverWallKey(const std::string& key) {
    return std::string(kExactSelectionPrefix) + key;
}

inline bool DecodeCoverWallSelectionKey(const std::string& value,
                                        std::vector<std::string>& keys) {
    keys.clear();
    if (value.rfind(kExactSelectionPrefix, 0) == 0) {
        const std::string key = value.substr(kExactSelectionPrefix.size());
        if (key.empty()) return false;
        keys.push_back(key);
        return true;
    }
    if (value.rfind(kGroupedSelectionPrefix, 0) != 0) return false;
    size_t at = kGroupedSelectionPrefix.size();
    while (at < value.size()) {
        const size_t colon = value.find(':', at);
        if (colon == std::string::npos || colon == at) break;
        size_t length = 0;
        for (size_t i = at; i < colon; ++i) {
            if (value[i] < '0' || value[i] > '9') { keys.clear(); return false; }
            const size_t digit = static_cast<size_t>(value[i] - '0');
            if (length > ((std::numeric_limits<size_t>::max)() - digit) / 10) {
                keys.clear(); return false;
            }
            length = length * 10 + digit;
        }
        at = colon + 1;
        if (length == 0 || length > value.size() - at) { keys.clear(); return false; }
        keys.push_back(value.substr(at, length));
        at += length;
    }
    if (at != value.size() || keys.empty()) { keys.clear(); return false; }
    return true;
}

}  // namespace aura::cover_wall
