#ifdef __APPLE__

#include "reaper_bridge_endpoint_macos.h"

#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <cstdlib>

namespace HarkenSearch::ReaperBridge {

std::string MacPrivateSocketPath(std::string_view rawPath) {
    if (rawPath.empty() || rawPath.front() != '/' ||
        rawPath.find(char(0)) != std::string_view::npos ||
        rawPath.find(char(10)) != std::string_view::npos ||
        rawPath.find(char(13)) != std::string_view::npos) return {};
    sockaddr_un address{};
    if (rawPath.size() >= sizeof(address.sun_path)) return {};
    const auto lastSlash = rawPath.find_last_of('/');
    if (lastSlash == std::string_view::npos || lastSlash == 0 ||
        lastSlash == rawPath.size() - 1) return {};
    const std::string parent(rawPath.substr(0, lastSlash));
    struct stat directory{};
    if (lstat(parent.c_str(), &directory) != 0 ||
        !S_ISDIR(directory.st_mode) || directory.st_uid != geteuid() ||
        (directory.st_mode & 0077) != 0) return {};
    return std::string(rawPath);
}

std::string MacSocketPathInPrivateDirectory(std::string_view rawDirectory) {
    while (rawDirectory.size() > 1 && rawDirectory.back() == '/')
        rawDirectory.remove_suffix(1);
    return MacPrivateSocketPath(
        std::string(rawDirectory) + "/" + kMacDefaultSocketName);
}

std::string MacDefaultSocketPath() {
    const std::size_t required = confstr(_CS_DARWIN_USER_TEMP_DIR, nullptr, 0);
    if (required < 2 || required > 4096) return {};
    std::string directory(required, char(0));
    if (confstr(_CS_DARWIN_USER_TEMP_DIR, directory.data(), required) != required ||
        directory.back() != char(0)) return {};
    directory.pop_back();
    return MacSocketPathInPrivateDirectory(directory);
}

std::string MacSelectedSocketPath() {
    if (const char* qaPath = std::getenv(kMacQaSocketVariable))
        return MacPrivateSocketPath(qaPath);
    return MacDefaultSocketPath();
}

} // namespace HarkenSearch::ReaperBridge

#endif // __APPLE__
