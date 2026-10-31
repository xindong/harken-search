#include "ThemeSchedulePaletteFiles.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace fs = std::filesystem;
using harken::theme_schedule::PaletteFilePath;
using harken::theme_schedule::Period;
using harken::theme_schedule::ReadPaletteJson;
using harken::theme_schedule::SavePaletteJson;

namespace {
int failed = 0;
void Check(bool ok, const char* name) {
    if (!ok) { std::cerr << "FAIL " << name << '\n'; ++failed; }
}
std::string ReadBytes(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}
}

int main(int argc, char** argv) {
    if (argc != 2) { std::cerr << "usage: test_theme_schedule_palette_files <isolated-dir>\n"; return 2; }
    const fs::path root = fs::u8path(argv[1]);
    fs::create_directories(root);
    const fs::path settings = root / "settings.ini";
    const fs::path legacy = root / "theme_custom.json";
    { std::ofstream out(legacy, std::ios::binary | std::ios::trunc); out << "legacy palette"; }
    const fs::path day = PaletteFilePath(settings, Period::Day);
    const fs::path night = PaletteFilePath(settings, Period::Night);
    Check(day != night, "two separate files");
    Check(day.parent_path() == root && night.parent_path() == root, "files next to settings");
    Check(day != legacy && night != legacy, "legacy custom path untouched");
    std::string error;
    Check(SavePaletteJson(settings, Period::Day, "{\"background\":\"light\"}", error), "save day");
    Check(SavePaletteJson(settings, Period::Night, "{\"background\":\"dark\"}", error), "save night");
    Check(ReadBytes(day) == "{\"background\":\"light\"}", "day bytes");
    Check(ReadBytes(night) == "{\"background\":\"dark\"}", "night bytes");
    const auto nightWriteTime = fs::last_write_time(night);
    Check(SavePaletteJson(settings, Period::Day, "{\"background\":\"cream\"}", error), "edit day");
    Check(ReadBytes(day) == "{\"background\":\"cream\"}", "day updated");
    Check(ReadBytes(night) == "{\"background\":\"dark\"}", "night content unchanged");
    Check(fs::last_write_time(night) == nightWriteTime, "night file not rewritten");
    Check(ReadBytes(legacy) == "legacy palette", "existing custom theme untouched");
    std::string loaded;
    Check(ReadPaletteJson(settings, Period::Day, loaded, error) && loaded == ReadBytes(day), "reload edited day");
    Check(ReadPaletteJson(settings, Period::Night, loaded, error) && loaded == ReadBytes(night), "reload night");
    Check(!SavePaletteJson({}, Period::Day, "{}", error), "empty settings path rejected");
    Check(!SavePaletteJson(settings, Period::Day, "", error), "empty palette rejected");
    Check(ReadBytes(day) == "{\"background\":\"cream\"}", "failed save preserves old palette");
    const fs::path blocked = root / "blocked";
    fs::create_directories(PaletteFilePath(blocked / "settings.ini", Period::Day));
    Check(!SavePaletteJson(blocked / "settings.ini", Period::Day, "{}", error),
          "failed replacement reports an error");
    Check(!error.empty(), "replacement failure has a diagnostic");
    Check(fs::is_directory(PaletteFilePath(blocked / "settings.ini", Period::Day)),
          "failed replacement preserves the occupied target");
    for (const auto& entry : fs::directory_iterator(blocked))
        Check(entry.path().filename().u8string().find(".pending-") == std::string::npos,
              "failed replacement removes only its staging file");
    loaded = "stale";
    Check(!ReadPaletteJson(root / "missing" / "settings.ini", Period::Day, loaded, error) &&
          loaded.empty() && !error.empty(), "missing palette clears stale output and reports failure");
    std::cout << "theme schedule palette files: " << failed << " failure(s)\n";
    return failed == 0 ? 0 : 1;
}
