#ifdef __APPLE__

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

namespace CoverModule {
// The playlist importer does not exercise cover hashing; satisfy the shared
// Database link boundary without bringing image decoding into this test.
std::uint64_t ComputePerceptualHash(const unsigned char* data, std::size_t size) {
    if (data == nullptr || size == 0) return 0;
    std::uint64_t hash = 1469598103934665603ull;
    for (std::size_t i = 0; i < size; ++i) {
        hash ^= data[i];
        hash *= 1099511628211ull;
    }
    return hash == 0 ? 1 : hash;
}
}

namespace Platform::HarkenUserData {

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
