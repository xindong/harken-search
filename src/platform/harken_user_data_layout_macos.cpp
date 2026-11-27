#include "harken_user_data_layout.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>

namespace Platform::HarkenUserData {

bool PreviewIsolationActive() {
#if defined(ARIS_PREVIEW_ISOLATION) && ARIS_PREVIEW_ISOLATION
    return true;
#else
    const char* enabled = std::getenv("ARIS_PREVIEW_ISOLATION");
    return enabled && std::string(enabled) == "1";
#endif
}

std::string PreviewIsolationRootUtf8() {
#if defined(ARIS_PREVIEW_ISOLATION) && ARIS_PREVIEW_ISOLATION
    return ARIS_PREVIEW_ISOLATION_ROOT_UTF8;
#else
    return {};
#endif
}

bool PreviewIsolationAllowsDataPathUtf8(const std::string& pathUtf8) {
    if (!PreviewIsolationActive()) return true;
    namespace fs = std::filesystem;
    const fs::path root(PreviewIsolationRootUtf8());
    const fs::path path(pathUtf8);
    if (!root.is_absolute() || !path.is_absolute()) return false;

    std::error_code error;
    const auto canonicalRoot = fs::canonical(root, error);
    if (error || canonicalRoot == canonicalRoot.root_path()) return false;
    const auto canonicalPath = fs::weakly_canonical(path, error);
    if (error) return false;
    return std::mismatch(canonicalRoot.begin(), canonicalRoot.end(),
                         canonicalPath.begin(), canonicalPath.end()).first == canonicalRoot.end();
}

}
