#include "src/CoverModule.h"
#include "src/DiagnosticsLogger.h"
#include "src/platform/filesystem.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>

namespace fs = std::filesystem;

namespace TestCoverStubs {
void AdvanceFrame(int frames = 1, double dtSeconds = 0.1);
void PauseAlbumArt(const std::string& path);
std::thread::id WaitForAlbumArtReader();
void ResumeAlbumArt();
}

static int g_failures = 0;

static void check(bool cond, const char* desc) {
    if (cond) {
        std::cout << "PASS: " << desc << "\n";
    } else {
        std::cout << "FAIL: " << desc << "\n";
        ++g_failures;
    }
}

static fs::path makeTempDir(const char* tag) {
    fs::path p = fs::temp_directory_path() / ("auro_cover_diag_" + std::string(tag));
    std::error_code ec;
    fs::remove_all(p, ec);
    fs::create_directories(p, ec);
    return p;
}

static bool writeFakeCoverImage(const fs::path& path) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    const char bytes[] = "fake-cover-image-data-for-cover-diagnostics";
    out.write(bytes, sizeof(bytes));
    return out.good();
}

static void waitDiagDrain() {
    std::this_thread::sleep_for(std::chrono::milliseconds(650));
}

static int countOccurrences(const std::string& text, const std::string& needle) {
    if (needle.empty()) return 0;
    int count = 0;
    size_t pos = 0;
    while ((pos = text.find(needle, pos)) != std::string::npos) {
        ++count;
        pos += needle.size();
    }
    return count;
}

static void testEmbeddedCoverMtimeCachePolicy(const fs::path& root) {
    const std::string cachedKey = "E|0123456789abcdef|4|image/jpeg";
    const fs::path audioPath = root / fs::u8path(u8"缓存 😀.wav");
    std::ofstream(audioPath, std::ios::binary | std::ios::trunc) << "audio";
    const std::string audioPathUtf8 = audioPath.u8string();
    const int64_t initialMtime =
        Platform::FileSystem::LastWriteTimeUnixSeconds(audioPathUtf8);
    check(initialMtime > 0, "mtime fixture has a positive initial timestamp");

    CoverModule::PrimeFileCoverKeysFromDb(
        audioPathUtf8, cachedKey, initialMtime, "", "", 0);
    check(CoverModule::FileHasCoverKey(audioPathUtf8, cachedKey),
          "same positive mtime retains primed embedded cover identity");

    std::error_code ec;
    const auto initialFileTime = fs::last_write_time(audioPath, ec);
    fs::last_write_time(audioPath, initialFileTime + std::chrono::seconds(5), ec);
    const int64_t changedMtime =
        Platform::FileSystem::LastWriteTimeUnixSeconds(audioPathUtf8);
    check(!ec && changedMtime > 0 && changedMtime != initialMtime,
          "mtime fixture advances to a distinct positive timestamp");
    check(!CoverModule::FileHasCoverKey(audioPathUtf8, cachedKey),
          "changed positive mtime invalidates primed embedded cover identity");

    const std::string missingPath = (root / "missing.wav").u8string();
    CoverModule::PrimeFileCoverKeysFromDb(
        missingPath, cachedKey, initialMtime, "", "", 0);
    check(CoverModule::FileHasCoverKey(missingPath, cachedKey),
          "missing path retains historical cached cover identity");

    const std::string invalidUtf8Path("\xC3\x28", 2);
    CoverModule::PrimeFileCoverKeysFromDb(
        invalidUtf8Path, cachedKey, initialMtime, "", "", 0);
    check(CoverModule::FileHasCoverKey(invalidUtf8Path, cachedKey),
          "mtime query failure retains historical cached cover identity");
}

static void testCoverCacheDirectoryPolicy(const fs::path& root) {
    const fs::path nested = root / fs::u8path(u8"缓存 😀") / "nested";
    CoverModule::SetCoverCacheDir(nested.u8string());
    check(fs::is_directory(nested),
          "cover cache setter creates a nested Unicode directory");

    CoverModule::SetCoverCacheDir(nested.u8string());
    check(fs::is_directory(nested),
          "cover cache setter accepts an existing directory");

    const fs::path occupied = root / "occupied-cache-path";
    std::ofstream(occupied, std::ios::binary | std::ios::trunc) << "occupied";
    CoverModule::SetCoverCacheDir(occupied.u8string());
    std::ifstream occupiedInput(occupied, std::ios::binary);
    std::string occupiedText;
    occupiedInput >> occupiedText;
    check(fs::is_regular_file(occupied) && occupiedText == "occupied",
          "cover cache setter silently preserves an occupied file path");

    CoverModule::SetCoverCacheDir("");
    check(fs::is_regular_file(occupied),
          "cover cache setter accepts an empty path without side effects");

    const fs::path nulPrefix = root / "nul-prefix-cache";
    std::string embeddedNulPath = nulPrefix.u8string();
    embeddedNulPath.append("\0ignored", 8);
    CoverModule::SetCoverCacheDir(embeddedNulPath);
    check(fs::is_directory(nulPrefix) &&
              !fs::exists(root / "nul-prefix-cacheignored"),
          "cover cache setter preserves embedded-NUL prefix creation behavior");

    CoverModule::SetCoverCacheDir(root.u8string());
}

static void testBrowseCoverIdentityRunsOffCaller(const fs::path& root) {
    const auto caller = std::this_thread::get_id();
    for (const bool currentCover : {false, true}) {
        const auto filename = currentCover ? "current.wav" : "prefetch.wav";
        const fs::path audioPath = root / filename;
        std::ofstream(audioPath, std::ios::binary | std::ios::trunc) << "audio";
        const auto path = audioPath.u8string();
        TestCoverStubs::PauseAlbumArt(path);
        CoverModule::TextureHandle texture;
        const auto startedAt = std::chrono::steady_clock::now();
        if (currentCover) {
            CoverModule::LoadCurrentFileCoverTexture(path, texture, nullptr);
        } else {
            CoverModule::GetEmbeddedCoverTextureByPath(path, texture);
        }
        const auto elapsed = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - startedAt).count();
        std::cout << "Browse cover caller time (" << filename << "): " << elapsed << " ms\n";
        const auto reader = TestCoverStubs::WaitForAlbumArtReader();
        TestCoverStubs::ResumeAlbumArt();
        check(reader != std::thread::id{} && reader != caller,
              currentCover ? "current cover identity reads on a worker"
                           : "prefetch and played cover identity read on a worker");
    }
    CoverModule::ShutdownCoverWallPrecache();
    CoverModule::ClearAllCoverCaches();
}

static void testBrowseCoverFallbacks(const fs::path& root) {
    for (const bool sidecar : {true, false}) {
        const auto folder = root / (sidecar ? "sidecar-browse" : "folder-browse");
        check(writeFakeCoverImage(folder / (sidecar ? "sample.jpg" : "cover.jpg")),
              "browse fallback image fixture created");
        const auto audioPath = folder / "sample.wav";
        std::ofstream(audioPath, std::ios::binary | std::ios::trunc) << "audio";
        const auto path = audioPath.u8string();
        CoverModule::TextureHandle texture;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (std::chrono::steady_clock::now() < deadline) {
            TestCoverStubs::AdvanceFrame();
            CoverModule::PumpCoverTextureUploads(8);
            if (CoverModule::LoadCurrentFileCoverTexture(path, texture, nullptr)) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        check(texture.hasTexture, sidecar ? "same-name artwork reaches the current cover"
                                          : "missing embedded artwork falls back to folder cover");

        std::vector<CoverModule::CoverKeyCacheUpdate> updates;
        CoverModule::DrainCoverKeyCacheUpdates(updates, 128);
        bool persistedIdentity = false;
        for (const auto& update : updates) {
            if (update.filePathUtf8 != path || !update.updateEmbedded) continue;
            persistedIdentity = update.embeddedCoverKey.empty() != sidecar;
        }
        check(persistedIdentity, "background identity queues the matching cache update");
    }
    CoverModule::ShutdownCoverWallPrecache();
    CoverModule::ClearAllCoverCaches();
}

static void testCoverPersistencePreservesBothSources(const fs::path& root) {
    const auto folder = root / "two-cover-sources";
    check(writeFakeCoverImage(folder / "cover.jpg") &&
              writeFakeCoverImage(folder / "two.jpg"),
          "folder and sidecar artwork fixtures created");
    const auto audioPath = folder / "two.wav";
    std::ofstream(audioPath, std::ios::binary | std::ios::trunc) << "audio";
    const auto path = audioPath.u8string();
    const std::vector<std::string> paths{path};
    CoverModule::RequestCoverWallPrecache(paths, CoverModule::ComputeBatchSignatureXXH3(paths));
    std::vector<CoverModule::CoverItem> items;
    CoverModule::CoverWallStatus status;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline) {
        CoverModule::GetCoverWallSnapshot(items, status);
        if (status.done) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    check(status.done, "cover discovery completed for both sources");
    std::vector<CoverModule::CoverKeyCacheUpdate> updates;
    CoverModule::DrainCoverKeyCacheUpdates(updates, 128);
    check(updates.size() == 1 && updates.front().filePathUtf8 == path &&
              updates.front().updateEmbedded && !updates.front().embeddedCoverKey.empty() &&
              updates.front().updateFolder && !updates.front().folderCoverKey.empty(),
          "same-path queued cover updates preserve both sources");
    CoverModule::ShutdownCoverWallPrecache();
    CoverModule::ClearAllCoverCaches();
}

int main() {
    auto& diag = DiagnosticsLogger::instance();
    diag.setEnabled(true);
    diag.setMaxEntries(256);
    diag.clear();

    const fs::path root = makeTempDir("runtime");
    const fs::path folder = root / "Library" / "Pack";
    const fs::path coverPath = folder / "cover.jpg";
    check(writeFakeCoverImage(coverPath), "fixture cover image created");

    CoverModule::ClearAllCoverCaches();
    CoverModule::CancelPendingCoverWork();
    testBrowseCoverIdentityRunsOffCaller(root);
    testBrowseCoverFallbacks(root);
    testCoverPersistencePreservesBothSources(root);
    testEmbeddedCoverMtimeCachePolicy(root);
    testCoverCacheDirectoryPolicy(root);

    CoverModule::CoverItem item;
    item.kind = CoverModule::CoverKind::FolderCover;
    item.key = "?|cover-diagnostics";
    item.folderPathUtf8 = folder.u8string();

    CoverModule::TextureHandle tex;
    const bool firstReady = CoverModule::GetCoverTextureByKey(item, tex, false);
    check(!firstReady, "first cover request queues async load before upload");

    bool loaded = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
        TestCoverStubs::AdvanceFrame();
        CoverModule::PumpCoverTextureUploads(8);
        loaded = CoverModule::GetCoverTextureByKey(item, tex, false);
        if (loaded && tex.hasTexture) {
            break;
        }
    }

    check(loaded && tex.hasTexture, "cover diagnostics fixture reaches uploaded texture state");
    waitDiagDrain();

    const std::string coverDiag = diag.getRecentJsonLines(64);
    check(coverDiag.find("\"category\":\"CoverLane\"") != std::string::npos,
          "cover request emits CoverLane diagnostics");
    check(coverDiag.find("lane=folder_temp") != std::string::npos,
          "cover diagnostics record folder_temp lane");
    check(coverDiag.find("ok=0") != std::string::npos,
          "cover diagnostics record pre-upload miss");
    check(coverDiag.find("ok=1") != std::string::npos,
          "cover diagnostics record post-upload success");
    check(coverDiag.find("TextureUploaded") != std::string::npos,
          "cover upload emits TextureUploaded diagnostics");

    diag.clear();
    TestCoverStubs::AdvanceFrame(10, 0.2);
    for (int i = 0; i < 20; ++i) {
        CoverModule::GetCoverTextureByKey(item, tex, false);
    }
    waitDiagDrain();

    const std::string summaryDiag = diag.getRecentJsonLines(64);
    const int coverLaneEvents = countOccurrences(summaryDiag, "\"category\":\"CoverLane\"");
    check(coverLaneEvents >= 1,
          "repeated cover requests still emit a throttled CoverLane diagnostic");
    check(coverLaneEvents < 20,
          "repeated cover requests do not emit one CoverLane diagnostic per request");
    check(summaryDiag.find("lane=folder_temp") != std::string::npos ||
              summaryDiag.find("lane=folder_cover") != std::string::npos,
          "CoverLane throttled diagnostic keeps the lane identity");

    CoverModule::ShutdownCoverWallPrecache();
    CoverModule::ClearAllCoverCaches();
    diag.setEnabled(false);

    std::error_code ec;
    fs::remove_all(root, ec);

    if (g_failures == 0) {
        std::cout << "PASS: Cover diagnostics\n";
        return 0;
    }

    std::cout << "FAIL: " << g_failures << " cover diagnostics checks failed\n";
    return 1;
}
