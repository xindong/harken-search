#pragma once

#include "ThemeScheduleClock.h"

#include <filesystem>
#include <string>

namespace harken::theme_schedule {

std::filesystem::path PaletteFilePath(const std::filesystem::path& settingsFile, Period period);
bool SavePaletteJson(const std::filesystem::path& settingsFile, Period period,
                     const std::string& json, std::string& error);
bool ReadPaletteJson(const std::filesystem::path& settingsFile, Period period,
                     std::string& json, std::string& error);

} // namespace harken::theme_schedule
