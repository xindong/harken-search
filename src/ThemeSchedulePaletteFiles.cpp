#include "ThemeSchedulePaletteFiles.h"
#include "platform/filesystem.h"

#include <atomic>
#include <chrono>

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
    if (!Platform::FileSystem::IsDirectory(target.parent_path().u8string())) {
        error = "settings directory is unavailable";
        return false;
    }
    static std::atomic<unsigned long long> sequence{0};
    fs::path pending = target;
    pending += ".pending-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
        "-" + std::to_string(++sequence);
    {
        Platform::FileSystem::StreamWriter out;
        if (!out.Open(pending.u8string(), false, true)) {
            error = "cannot open palette staging file"; return false;
        }
        if (!out.Write(json) || !out.Flush()) {
            error = "cannot write palette staging file";
            out.Close();
            Platform::FileSystem::RemoveFile(pending.u8string());
            return false;
        }
        if (!out.CloseChecked()) {
            error = "cannot close palette staging file";
            Platform::FileSystem::RemoveFile(pending.u8string());
            return false;
        }
    }
    if (!Platform::FileSystem::AtomicFileReplace(target.u8string(), pending.u8string())) {
        error = "cannot replace palette file";
        Platform::FileSystem::RemoveFile(pending.u8string());
        return false;
    }
    return true;
}

bool ReadPaletteJson(const fs::path& settingsFile, Period period,
                     std::string& json, std::string& error) {
    error.clear();
    json.clear();
    const fs::path source = PaletteFilePath(settingsFile, period);
    if (source.empty()) { error = "settings path is empty"; return false; }
    const auto bytes = Platform::FileSystem::ReadFileBytes(source.u8string());
    if (bytes.empty()) {
        error = "cannot read palette file";
        json.clear();
        return false;
    }
    json.assign(bytes.begin(), bytes.end());
    return true;
}

} // namespace harken::theme_schedule
