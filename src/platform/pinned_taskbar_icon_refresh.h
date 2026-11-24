#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace Platform::Detail {

// Windows taskbar pins are per-user shortcut copies under
// %APPDATA%\Microsoft\Internet Explorer\Quick Launch\User Pinned\TaskBar.
// After an in-place upgrade Explorer can keep the icon bitmap it cached for such
// a pin even though the executable now carries the new icon. Only pins that
// launch this executable and still use its default icon may be refreshed; a
// user-chosen icon must never be replaced and stays untouched.

inline wchar_t FoldPinnedIconCase(wchar_t value) noexcept {
    return (value >= L'A' && value <= L'Z')
        ? static_cast<wchar_t>(value - L'A' + L'a')
        : value;
}

inline bool PinnedIconPathsEqual(std::wstring_view left,
                                 std::wstring_view right) noexcept {
    if (left.size() != right.size()) return false;
    for (std::size_t i = 0; i < left.size(); ++i) {
        const wchar_t leftValue = left[i] == L'/' ? L'\\' : left[i];
        const wchar_t rightValue = right[i] == L'/' ? L'\\' : right[i];
        if (FoldPinnedIconCase(leftValue) != FoldPinnedIconCase(rightValue)) {
            return false;
        }
    }
    return true;
}

inline std::wstring_view TrimPinnedIconQuotes(std::wstring_view value) noexcept {
    if (value.size() >= 2 && value.front() == L'"' && value.back() == L'"') {
        value.remove_prefix(1);
        value.remove_suffix(1);
    }
    return value;
}

// A shortcut that never had an icon chosen stores either an empty icon location
// or this executable with index 0.
inline bool PinnedShortcutUsesDefaultAppIcon(std::wstring_view targetPath,
                                            std::wstring_view iconLocation,
                                            int iconIndex,
                                            std::wstring_view exePath) noexcept {
    if (targetPath.empty() || exePath.empty()) return false;
    if (!PinnedIconPathsEqual(targetPath, exePath)) return false;
    const std::wstring_view icon = TrimPinnedIconQuotes(iconLocation);
    if (icon.empty()) return true;
    return iconIndex <= 0 && PinnedIconPathsEqual(icon, exePath);
}

} // namespace Platform::Detail
