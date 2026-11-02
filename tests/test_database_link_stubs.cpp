#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include <filesystem>

#include "../src/AppState.h"
#include "../src/BackfillWorkerPool.h"
#include "../src/PathUtils.h"
#include "../src/platform/database_audio_probe.h"
#include "../src/platform/filesystem.h"
#include "../src/platform/system.h"

#ifndef AURO_TEST_WAV_DATABASE_AUDIO_PROBE
namespace Platform::DatabaseAudioProbe {

bool Probe(const std::filesystem::path&, Info& out)
{
    out = {};
    return false;
}

} // namespace Platform::DatabaseAudioProbe
#endif

namespace Platform {

std::wstring LegacyHardwareFingerprint() { return {}; }
bool ProtectUserData(std::string_view, std::string&) { return false; }
bool UnprotectUserData(std::string_view, std::string&) { return false; }
bool StoreGenericCredential(const std::string&, std::string_view) { return false; }
bool LoadGenericCredential(const std::string&, std::string&) { return false; }
bool DeleteGenericCredential(const std::string&) { return false; }

#ifndef AURO_TEST_EXTERNAL_EXE_DIR
std::string GetExeDir()
{
    return std::filesystem::current_path().u8string();
}
#endif

namespace FileSystem {
#ifndef AURO_TEST_EXTERNAL_PATH_ATTRIBUTES
PathAttributes QueryPathAttributes(const std::string& pathUtf8)
{
    const auto path = std::filesystem::u8path(pathUtf8);
    std::error_code ec;
    const auto status = std::filesystem::status(path, ec);
    if (ec) return {};
    return {
        (status.permissions() & std::filesystem::perms::owner_read) == std::filesystem::perms::none,
        false,
        false,
    };
}
#endif
} // namespace FileSystem

void SetThreadBackgroundPriority()
{
}

EnvironmentStringResult ReadEnvironmentVariable(const std::string&)
{
    return {};
}


} // namespace Platform

namespace ImGui {

ImGuiIO& GetIO()
{
    alignas(ImGuiIO) static unsigned char storage[sizeof(ImGuiIO)] = {};
    return *reinterpret_cast<ImGuiIO*>(storage);
}

bool IsKeyPressed(ImGuiKey, bool)
{
    return false;
}

const char* GetKeyName(ImGuiKey)
{
    return "";
}

} // namespace ImGui

void AppState::performGlobalMemoryGC()
{
}

#ifndef AURO_TEST_REAL_ASYNC_WORKER
void WaveformWorker::cancelAllPending()
{
}
#endif

namespace CoverModule {

std::string CanonicalCoverKey(std::string_view rawKey);

bool GetFolderCoverImagePath(const std::string& folderPathUtf8, std::string& outCoverPathUtf8, bool backgroundDiscovery)
{
    outCoverPathUtf8.clear();
    if (backgroundDiscovery &&
        folderPathUtf8.find("cover_indexing") != std::string::npos) {
        outCoverPathUtf8 = folderPathUtf8 + "/../cover.jpg";
        return true;
    }
    return false;
}

bool GetFolderCoverImagePathBackground(const std::string& folderPathUtf8,
                                       std::string& outCoverPathUtf8,
                                       bool skipNegativeCache,
                                       bool useDeepSearch)
{
    outCoverPathUtf8.clear();
    if ((skipNegativeCache || useDeepSearch) &&
        folderPathUtf8.find("cover_indexing") != std::string::npos) {
        outCoverPathUtf8 = folderPathUtf8 + "/../cover.jpg";
        return true;
    }
    return false;
}

void InvalidateCoverForPath(const std::string&)
{
}

bool IsCoverTextureHandleCurrent(const CoverItem&, uint32_t textureId)
{
    return textureId != 0;
}

std::string NormalizeCoverKeyByVisualIdentity(const std::string& coverPath)
{
    if (coverPath.find("cover_indexing") != std::string::npos) {
        return "F|H|1234567890abcdef|1";
    }
    // Production falls back to a canonical F|PATH identity when the fixture
    // path does not point at a real image. Keep this lightweight test double
    // byte-compatible with that no-I/O fallback instead of leaking a raw path
    // into folder_cover_key.
    return CanonicalCoverKey(std::string("F|PATH|") + coverPath);
}

// T2.7/T2.2 stub aligned with production semantics:
// - F|<legacy_path> -> F|PATH|<canonical_path>
// - F|PATH|<path>   -> re-canonicalize path portion
// - F|H|... / E|... -> pass through
std::string CanonicalCoverKey(std::string_view rawKey)
{
    if (rawKey.empty()) return std::string();
    std::string s(rawKey);

    if (s.rfind("F|H|", 0) == 0) {
        return s;
    }
    if (s.rfind("E|", 0) == 0) {
        return s;
    }
    if (s.rfind("F|PATH|", 0) == 0) {
        std::string rest = s.substr(7);
        if (rest.empty()) return std::string();
        size_t pipe = rest.find('|');
        std::string pathPart = (pipe != std::string::npos) ? rest.substr(0, pipe) : rest;
        std::string suffix = (pipe != std::string::npos) ? rest.substr(pipe) : "";
        std::string canonicalPath = HarkenSearch::CanonicalFolderPath(pathPart);
        if (canonicalPath.empty()) return std::string();
        return std::string("F|PATH|") + canonicalPath + suffix;
    }
    if (s.rfind("F|", 0) == 0) {
        std::string pathPart = s.substr(2);
        std::string canonicalPath = HarkenSearch::CanonicalFolderPath(pathPart);
        if (canonicalPath.empty()) return std::string();
        return std::string("F|PATH|") + canonicalPath;
    }
    return std::string();
}

std::uint64_t ComputePerceptualHash(const unsigned char* data, std::uint64_t size)
{
    if (!data || size == 0) return 0;
    std::uint64_t h = 1469598103934665603ull;
    for (std::uint64_t i = 0; i < size; ++i) {
        h ^= static_cast<std::uint64_t>(data[i]);
        h *= 1099511628211ull;
    }
    return h == 0 ? 1 : h;
}

bool PrepareCoverThumbForStorage(const std::vector<std::uint8_t>&,
                                 std::vector<std::uint8_t>&,
                                 std::string&)
{
    return false;
}

} // namespace CoverModule

namespace aura {

#ifdef AURA_TEST_STUB_BACKFILL_SCAN_WRITE_GUARD
BackfillScanWriteGuard::BackfillScanWriteGuard()
    : m_lock()
{
}

BackfillScanWriteGuard::~BackfillScanWriteGuard() = default;
#endif

}
