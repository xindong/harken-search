// Theme library files: imported themes are kept next to settings.ini, one
// JSON file per theme, and the legacy install-directory copy is adopted once.

#include "ThemeLibraryFiles.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

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
    std::string error;
    {
        const std::string longName(200, 'x');
        Check(FileStemForName(longName).size() <= 120, "long name capped");
    }
    {
        // Same 120-byte prefix, different tails: both must stay usable names
        // and must not land in the same file.
        const fs::path extraLibrary = root / "extra";
        const std::string shared(120, 'p');
        const std::string first = FileStemForName(shared + "-alpha");
        const std::string second = FileStemForName(shared + "-beta");
        Check(first.size() <= 120 && second.size() <= 120, "truncated names stay short");
        Check(first != second, "truncated names stay distinct");
        Check(!fs::exists(extraLibrary / (first + ".json")), "distinct name has no file yet");
        Check(SaveThemeCopy(extraLibrary, shared + "-alpha", "{\"a\":\"10\"}", error), "save long name one");
        Check(SaveThemeCopy(extraLibrary, shared + "-beta", "{\"a\":\"11\"}", error), "save long name two");
        Check(ReadBytes(extraLibrary / (first + ".json")) != ReadBytes(extraLibrary / (second + ".json")),
              "long names do not overwrite each other");
    }
    Check(FileStemForName("CON") == "_CON", "reserved device name is prefixed");
    Check(FileStemForName("con") == "_con", "reserved device name check is case-insensitive");
    Check(FileStemForName("COM1") == "_COM1" && FileStemForName("lpt9") == "_lpt9",
          "numbered device names are prefixed");
    Check(FileStemForName("NUL") == "_NUL", "null device name is prefixed");
    Check(FileStemForName("CONSOLE") == "CONSOLE", "ordinary name keeps its text");
    Check(SaveThemeCopy(root / "extra", "CON", "{\"a\":\"12\"}", error), "reserved name saves");
    Check(ReadBytes(root / "extra" / "_CON.json") == "{\"a\":\"12\"}", "reserved name lands on disk");

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

    using harken::theme_library::ImportResult;
    using harken::theme_library::InstallImportedTheme;
    const auto existing = ThemePathFor(library, "Rose Cedar");
    const std::string oldLibrary = "{\r\n  \"old\": true\r\n}\r\n";
    const std::string imported = "{\"new\":true}";
    WriteText(existing, oldLibrary);
    WriteText(custom, "");
    int calls = 0;
    const auto rejectSettings = [&] {
        ++calls;
        Check(ReadBytes(custom) == imported, "active file is written before settings save");
        return false;
    };
    Check(InstallImportedTheme(settings, "Rose Cedar", imported, rejectSettings) == ImportResult::Failed,
          "settings save failure reports a rolled-back import");
    Check(calls == 1 && ReadBytes(existing) == oldLibrary &&
              fs::is_regular_file(custom) && ReadBytes(custom).empty(),
          "settings save failure restores exact previous bytes, including empty file");

    fs::remove(custom);
    const auto fresh = ThemePathFor(library, "Fresh");
    Check(InstallImportedTheme(settings, "Fresh", imported, rejectSettings) == ImportResult::Failed &&
              !fs::exists(fresh) && !fs::exists(custom),
          "settings save failure removes newly introduced files");
    fs::remove(custom);

    fs::create_directory(custom);
    const auto unreachableSettings = [&] { Check(false, "must not persist settings after a file error"); return true; };
    Check(InstallImportedTheme(settings, "Rose Cedar", imported, unreachableSettings) == ImportResult::Failed &&
              ReadBytes(existing) == oldLibrary && fs::is_directory(custom),
          "blocked active slot leaves previous library and active slot alone");
    fs::remove(custom);

#ifdef _WIN32
    WriteText(custom, "previous active theme");
    const HANDLE locked = CreateFileW(custom.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                      nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    Check(locked != INVALID_HANDLE_VALUE, "open active slot without delete sharing");
    if (locked != INVALID_HANDLE_VALUE) {
        Check(InstallImportedTheme(settings, "Rose Cedar", imported, unreachableSettings) == ImportResult::Failed &&
                  ReadBytes(existing) == oldLibrary && ReadBytes(custom) == "previous active theme",
              "active replacement failure rolls back an already written library entry");
        CloseHandle(locked);
    }
    fs::remove(custom);
#endif

    Check(InstallImportedTheme(settings, "Rose Cedar", imported, [&] {
              fs::remove(custom);
              fs::create_directory(custom);
              WriteText(custom / "blocker", "occupied");
              return false;
          }) == ImportResult::RollbackFailed && ReadBytes(existing) == oldLibrary,
          "failed active rollback is reported and does not skip library restoration");
    fs::remove(custom / "blocker");
    fs::remove(custom);

    Check(InstallImportedTheme(settings, "Rose Cedar", imported, [&] {
              return ReadBytes(existing) == imported && ReadBytes(custom) == imported;
          }) == ImportResult::Saved && ReadBytes(existing) == imported && ReadBytes(custom) == imported,
          "success retains both files and only persists settings after both writes");

    if (failed != 0) { std::cerr << failed << " check(s) failed\n"; return 1; }
    std::cout << "theme library files: all checks passed\n";
    return 0;
}
