// Theme library files: imported themes are kept next to settings.ini, one
// JSON file per theme, and the legacy install-directory copy is adopted once.

#include "ThemeLibraryFiles.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace fs = std::filesystem;
using harken::theme_library::AdoptLegacyCustomTheme;
using harken::theme_library::CustomThemePath;
using harken::theme_library::FileStemForName;
using harken::theme_library::LibraryDirectory;
using harken::theme_library::ListThemes;
using harken::theme_library::SaveThemeCopy;
using harken::theme_library::ThemePathFor;

namespace {
int failed = 0;
void Check(bool ok, const char* name) {
    if (!ok) { std::cerr << "FAIL " << name << '\n'; ++failed; }
}
std::string ReadBytes(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}
void WriteText(const fs::path& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
}
}

int main(int argc, char** argv) {
    if (argc != 2) { std::cerr << "usage: test_theme_library_files <isolated-dir>\n"; return 2; }
    const fs::path root = fs::u8path(argv[1]);
    fs::remove_all(root);
    fs::create_directories(root);
    const fs::path settings = root / "settings.ini";
    WriteText(settings, "theme=7\n");

    const fs::path library = LibraryDirectory(settings);
    Check(library == root / "themes", "library sits next to settings");
    Check(CustomThemePath(settings) == root / "theme_custom.json", "custom slot next to settings");
    Check(LibraryDirectory(fs::path()).empty(), "empty settings path keeps library empty");
    Check(CustomThemePath(fs::path("settings.ini")).empty(), "relative settings path is rejected");

    Check(FileStemForName("Rose Cedar") == "Rose Cedar", "plain name kept");
    Check(FileStemForName(" a/b:c*d?e\"f<g>h|i ") == "abcdefghi", "windows-forbidden chars removed");
    Check(FileStemForName("  ...  ") == "theme", "dots and spaces fall back to theme");
    Check(FileStemForName("") == "theme", "empty name falls back to theme");
    Check(FileStemForName("abc.") == "abc", "trailing dot trimmed");
    {
        const std::string longName(200, 'x');
        Check(FileStemForName(longName).size() <= 120, "long name capped");
    }

    std::string error;
    Check(SaveThemeCopy(library, "Rose Cedar", "{\"a\":\"01\"}", error), "save first theme");
    Check(SaveThemeCopy(library, "Ocean Test", "{\"a\":\"02\"}", error), "save second theme");
    Check(ReadBytes(library / "Rose Cedar.json") == "{\"a\":\"01\"}", "theme bytes on disk");

    Check(SaveThemeCopy(library, "Rose Cedar", "{\"a\":\"03\"}", error), "same name saves again");
    auto listed = ListThemes(library);
    Check(listed.size() == 2, "same name overwrites instead of duplicating");
    Check(ReadBytes(library / "Rose Cedar.json") == "{\"a\":\"03\"}", "overwrite replaced content");

    WriteText(library / "notes.txt", "ignored");
    fs::create_directories(library / "nested.json");
    listed = ListThemes(library);
    Check(listed.size() == 2, "non-json entries and directories are ignored");
    Check(listed[0].stem().u8string() == "Ocean Test" &&
          listed[1].stem().u8string() == "Rose Cedar", "themes sorted by name");

    Check(!SaveThemeCopy(library, "Empty", "", error), "empty JSON is rejected");
    Check(!fs::exists(library / "Empty.json"), "rejected save leaves no file");
    Check(ThemePathFor(fs::path(), "x").empty(), "no library directory means no target");

    // Legacy install-directory copy is adopted exactly once.
    const fs::path legacyRoot = root / "install";
    fs::create_directories(legacyRoot);
    const fs::path legacy = legacyRoot / "theme_custom.json";
    WriteText(legacy, "{\"legacy\":\"yes\"}");
    const fs::path custom = CustomThemePath(settings);
    Check(!fs::exists(custom), "user-data slot starts empty");
    Check(AdoptLegacyCustomTheme(legacy, custom, error), "legacy theme adopted");
    Check(ReadBytes(custom) == "{\"legacy\":\"yes\"}", "adopted bytes match the install copy");
    WriteText(legacy, "{\"legacy\":\"no\"}");
    Check(AdoptLegacyCustomTheme(legacy, custom, error), "adoption is idempotent");
    Check(ReadBytes(custom) == "{\"legacy\":\"yes\"}", "existing user-data theme is not overwritten");
    Check(!AdoptLegacyCustomTheme(legacyRoot / "missing.json", root / "other.json", error),
          "missing legacy file reports failure");
    Check(!fs::exists(root / "other.json"), "failed adoption creates nothing");

    if (failed != 0) { std::cerr << failed << " check(s) failed\n"; return 1; }
    std::cout << "theme library files: all checks passed\n";
    return 0;
}
