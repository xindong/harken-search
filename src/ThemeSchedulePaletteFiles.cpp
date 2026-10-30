#include "ThemeSchedulePaletteFiles.h"

#include <atomic>
#include <chrono>
#include <fstream>
#include <iterator>
#include <system_error>

#ifdef _WIN32
#include <Windows.h>
#endif

namespace harken::theme_schedule {
namespace fs = std::filesystem;

fs::path PaletteFilePath(const fs::path& settingsFile, Period period) {
    if (settingsFile.empty() || settingsFile.parent_path().empty()) return {};
    return settingsFile.parent_path() /
        (period == Period::Day ? "theme_schedule_day.json" : "theme_schedule_night.json");
}

bool SavePaletteJson(const fs::path& settingsFile, Period period,
                     const std::string& json, std::string& error) {
    error.clear();
    const fs::path target = PaletteFilePath(settingsFile, period);
    if (target.empty() || json.empty()) {
        error = "settings path or palette JSON is empty";
        return false;
    }
    std::error_code ec;
    if (!fs::is_directory(target.parent_path(), ec)) {
        error = "settings directory is unavailable";
        return false;
    }
    static std::atomic<unsigned long long> sequence{0};
    fs::path pending = target;
    pending += ".pending-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
        "-" + std::to_string(++sequence);
    {
        std::ofstream out(pending, std::ios::binary | std::ios::trunc);
        if (!out) { error = "cannot open palette staging file"; return false; }
        out.write(json.data(), static_cast<std::streamsize>(json.size()));
        out.flush();
        if (!out) {
            error = "cannot write palette staging file";
            out.close();
            fs::remove(pending, ec);
            return false;
        }
        out.close();
        if (!out) {
            error = "cannot close palette staging file";
            fs::remove(pending, ec);
            return false;
        }
    }
#ifdef _WIN32
    if (!MoveFileExW(pending.c_str(), target.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        error = "cannot replace palette file, Windows error " + std::to_string(GetLastError());
        fs::remove(pending, ec);
        return false;
    }
#else
    fs::rename(pending, target, ec);
    if (ec) {
        error = "cannot replace palette file: " + ec.message();
        fs::remove(pending, ec);
        return false;
    }
#endif
    return true;
}

bool ReadPaletteJson(const fs::path& settingsFile, Period period,
                     std::string& json, std::string& error) {
    error.clear();
    json.clear();
    const fs::path source = PaletteFilePath(settingsFile, period);
    if (source.empty()) { error = "settings path is empty"; return false; }
    std::ifstream in(source, std::ios::binary);
    if (!in) { error = "cannot open palette file"; return false; }
    json.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    if (in.bad() || json.empty()) {
        error = "cannot read palette file";
        json.clear();
        return false;
    }
    return true;
}

} // namespace harken::theme_schedule
