#pragma once

#include "ExternalAiClientLink.h"

#include <filesystem>
#include <string>
#include <system_error>

namespace HarkenSearch {

struct ExternalAiClientConfigInputs {
    std::string userHome;
    std::string appData;
    std::string codexHome;
    bool isolated = false;
    std::string isolationRoot;
};

struct ExternalAiClientConfigLocation {
    bool detected = false;
    std::string configPath;
};

namespace ExternalAiClientConfigDetail {
namespace fs = std::filesystem;

inline fs::path AbsolutePath(const std::string& utf8) {
    if (utf8.find('\0') != std::string::npos) return {};
    auto path = fs::u8path(utf8);
    return path.is_absolute() ? path.make_preferred() : fs::path{};
}

inline fs::path ExistingDirectory(const fs::path& path) {
    if (path.empty()) return {};
    std::error_code error;
    const auto resolved = fs::canonical(path, error);
    return !error && fs::is_directory(resolved, error) && !error ? resolved : fs::path{};
}

inline bool WithinDirectory(const fs::path& path, const fs::path& directory) {
    if (directory.empty()) return false;
    auto component = path.begin();
    for (const auto& expected : directory) {
        if (component == path.end() || *component != expected) return false;
        ++component;
    }
    return true;
}

inline bool PlainDirectories(const fs::path& directory) {
    auto prefix = directory.root_path();
    for (const auto& component : directory.relative_path()) {
        if (component.empty()) continue;
        if (component == "." || component == "..") return false;
        prefix /= component;
        std::error_code error;
        if (!fs::is_directory(fs::symlink_status(prefix, error)) || error) return false;
    }
    return true;
}

inline bool PlainFileOrAbsent(const fs::path& path) {
    std::error_code error;
    const auto status = fs::symlink_status(path, error);
    if (status.type() == fs::file_type::not_found) {
        return !error || error == std::errc::no_such_file_or_directory;
    }
    return !error && fs::is_regular_file(status) && fs::hard_link_count(path, error) == 1 && !error;
}

inline ExternalAiClientConfigLocation ConfigInDirectory(const fs::path& directory, const char* filename) {
    auto config = directory / filename;
    auto backup = config;
    backup += ".harken-backup";
    auto marker = config;
    marker += ".harken-link";
    if (!PlainFileOrAbsent(config) || !PlainFileOrAbsent(backup) || !PlainFileOrAbsent(marker)) return {};
    return {true, config.u8string()};
}
}

inline ExternalAiClientConfigLocation ResolveExternalAiClientConfigLocation(
    ExternalAiClient client, const ExternalAiClientConfigInputs& inputs) {
    using namespace ExternalAiClientConfigDetail;
    try {
        if (inputs.isolated) {
            const auto root = AbsolutePath(inputs.isolationRoot);
            if (root.empty() || !root.has_relative_path()) return {};
            const auto profile = root / "profile";
            const auto directory = client == ExternalAiClient::Cindy
                ? profile / "Roaming" / "Cindy" / "codex-home" : profile / "home" / ".codex";
            if (!PlainDirectories(directory)) return {};
            const auto resolved = ExistingDirectory(directory);
            if (resolved.empty() || !WithinDirectory(resolved, root)) return {};
            return ConfigInDirectory(resolved, "config.toml");
        }

        const auto appData = AbsolutePath(inputs.appData);
        const auto cindyRoot = appData.empty() ? fs::path{} : ExistingDirectory(appData / "Cindy");
        if (client == ExternalAiClient::Cindy) {
            if (cindyRoot.empty()) return {};
            const auto desktopCodex = ExistingDirectory(cindyRoot / "codex-home");
            if (!desktopCodex.empty() && WithinDirectory(desktopCodex, cindyRoot)) {
                auto result = ConfigInDirectory(desktopCodex, "config.toml");
                if (result.detected) return result;
                return {true, {}};
            }
            const bool hasDesktopPi = !cindyRoot.empty() && !ExistingDirectory(cindyRoot / "pi-agent-home").empty();
            return {hasDesktopPi, {}};
        }

        if (!inputs.codexHome.empty()) {
            const auto overridePath = ExistingDirectory(AbsolutePath(inputs.codexHome));
            if (overridePath.empty()) return {};
            if (!WithinDirectory(overridePath, cindyRoot)) return ConfigInDirectory(overridePath, "config.toml");
        }

        const auto home = AbsolutePath(inputs.userHome);
        if (home.empty()) return {};
        const auto directory = ExistingDirectory(home / ".codex");
        if (directory.empty() || WithinDirectory(directory, cindyRoot)) return {};
        return ConfigInDirectory(directory, "config.toml");
    } catch (const std::system_error&) {
        return {};
    }
}

}
