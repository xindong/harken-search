#pragma once

#include <filesystem>
#include <string>

namespace HarkenSearch::DawBridge {

// Lexical host layout only: no file IO, host launch, plugin installation or
// readiness claim. The doctor separately checks bundle/executable existence.
// Accept the native application selection and its known internal executable.
struct MacReaperHostLayout {
    std::filesystem::path bundle;
    std::filesystem::path executable;
    std::filesystem::path portableResourceDirectory;
};

inline MacReaperHostLayout ResolveMacReaperHostLayout(const std::string& configuredPath) {
    namespace fs = std::filesystem;
    if (configuredPath.empty() || configuredPath.find('\0') != std::string::npos)
        return {};
    fs::path input = fs::u8path(configuredPath).lexically_normal();
    if (!input.is_absolute()) return {};
    while (input.filename().empty() && input != input.root_path())
        input = input.parent_path();
    fs::path bundle;
    if (input.extension() == ".app") {
        bundle = input;
    } else if (input.filename() == "REAPER" && input.parent_path().filename() == "MacOS" &&
               input.parent_path().parent_path().filename() == "Contents") {
        bundle = input.parent_path().parent_path().parent_path();
        if (bundle.extension() != ".app") return {};
    } else {
        return {};
    }
    return {bundle, bundle / "Contents" / "MacOS" / "REAPER", bundle.parent_path()};
}

} // namespace HarkenSearch::DawBridge
