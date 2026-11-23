#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace harken::theme_library {

// One JSON file per imported theme lives in <settings dir>/themes, next to
// settings.ini. The install directory is never used for user themes.
std::filesystem::path LibraryDirectory(const std::filesystem::path& settingsFile);

// Single slot for the theme currently saved through "save as new theme".
std::filesystem::path CustomThemePath(const std::filesystem::path& settingsFile);

// File-name-safe stem for a theme name (Windows forbids <>:"/\|?* and
// trailing dots/spaces). Returns "theme" for input that sanitizes to nothing.
std::string FileStemForName(const std::string& nameUtf8);

std::filesystem::path ThemePathFor(const std::filesystem::path& libraryDir,
                                   const std::string& nameUtf8);

// Durable write: stage next to the target, flush, then replace atomically.
bool SaveThemeJson(const std::filesystem::path& target, const std::string& json,
                   std::string& error);

// Saves a copy of an imported theme; same name overwrites the earlier copy.
bool SaveThemeCopy(const std::filesystem::path& libraryDir, const std::string& nameUtf8,
                   const std::string& json, std::string& error);

// Theme files in the library, sorted by display name (case-insensitive).
std::vector<std::filesystem::path> ListThemes(const std::filesystem::path& libraryDir);

// Copies the legacy install-directory theme into the user-data slot once.
// true = the target now exists (fresh or copied); false = nothing to adopt.
bool AdoptLegacyCustomTheme(const std::filesystem::path& legacyPath,
                            const std::filesystem::path& target, std::string& error);

} // namespace harken::theme_library
