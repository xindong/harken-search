#ifdef __APPLE__

#include <filesystem>
#include <string>
#include "platform/harken_user_data_layout.h"

namespace Platform::HarkenUserData {

Layout EnsureLayout() {
    Layout layout;
    layout.readOnly = true;
    return layout;
}

bool PreviewIsolationActive() { return true; }

bool PreviewIsolationAllowsDataPathUtf8(const std::string& pathUtf8) {
    if (pathUtf8.empty()) return false;
    std::error_code ec;
    const auto root = std::filesystem::weakly_canonical(
        std::filesystem::temp_directory_path() / "auro_sub_playlist_test", ec);
    if (ec) return false;
    const auto path = std::filesystem::weakly_canonical(pathUtf8, ec);
    if (ec) return false;
    auto rootPart = root.begin();
    auto pathPart = path.begin();
    while (rootPart != root.end() && pathPart != path.end()) {
        if (*rootPart != *pathPart) return false;
        ++rootPart;
        ++pathPart;
    }
    return rootPart == root.end();
}

} // namespace Platform::HarkenUserData

#endif // __APPLE__
