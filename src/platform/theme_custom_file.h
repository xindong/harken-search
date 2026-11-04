#pragma once

#include <string>

#ifdef __APPLE__
#include "harken_user_data_layout.h"
#include <filesystem>
#else
#include "Utils.h"
#endif

namespace Platform::ThemeFiles {

inline std::string CustomThemePathUtf8() {
#ifdef __APPLE__
    const auto layout = HarkenUserData::EnsureLayout();
    if (layout.readOnly || layout.activeSettingsDir.empty()) return {};
    const auto settingsDir = HarkenUserData::WideToUtf8Path(layout.activeSettingsDir);
    if (settingsDir.empty()) return {};
    const auto path = (std::filesystem::u8path(settingsDir) / "theme_custom.json").u8string();
    return HarkenUserData::PreviewIsolationAllowsDataPathUtf8(path) ? path : std::string{};
#else
    // Preserve the Windows location for existing installations.
    return GetExeDirUtf8() + "\\theme_custom.json";
#endif
}

}  // namespace Platform::ThemeFiles
