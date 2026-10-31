#pragma once



#include <array>

#include <atomic>

#include <cctype>

#include <cstdint>

#include <future>

#include <memory>

#include <string>

#include <utility>

#include <vector>



// Forward declaration

class Database;

struct AppState;  // Defined in AppState.h



// Forward declaration for MemoryBudget (global namespace)

// MemoryBudget stays in the global namespace to match its definition.

class MemoryBudget;



// Cover loading, caching, and XXH3-based deduplication.
// main.cpp owns layout; this module exposes only the state hooks needed for cover work.


namespace CoverModule {

// @stage7-contract
// API contract: cover-wall models expose value snapshots and opaque texture
// handles. Callers must perform UI texture operations on the UI thread, keep
// Database/AppState owners alive for referenced work, and treat failed loads as
// status/results without retaining internal queue storage.



enum class CoverResolutionLane {

    EmbeddedArt,

    SameStemImage,

    DbSourcePath,

    FolderDiscovery,

    ThumbByKey,

    DirectImagePath,

};



inline const char* CoverResolutionLaneName(CoverResolutionLane lane) {

    switch (lane) {

        case CoverResolutionLane::EmbeddedArt: return "embedded_art";

        case CoverResolutionLane::SameStemImage: return "same_stem";

        case CoverResolutionLane::DbSourcePath: return "src_path";

        case CoverResolutionLane::FolderDiscovery: return "folder_discovery";

        case CoverResolutionLane::ThumbByKey: return "thumb";

        case CoverResolutionLane::DirectImagePath: return "direct_image";

    }

    return "unknown";

}



inline bool LooksLikeCoverImagePath(const std::string& pathUtf8) {

    if (pathUtf8.size() < 4) return false;

    const size_t last = pathUtf8.find_last_of(".\\/");

    if (last == std::string::npos || pathUtf8[last] != '.') return false;

    std::string ext = pathUtf8.substr(last);

    for (char& c : ext) c = (char)std::tolower((unsigned char)c);

    return ext == ".jpg" || ext == ".jpeg" || ext == ".png" ||

           ext == ".webp" || ext == ".bmp" || ext == ".gif";

}



inline constexpr std::array<CoverResolutionLane, 4> GetCurrentCoverResolutionOrder() {

    return {

        CoverResolutionLane::EmbeddedArt,

        CoverResolutionLane::SameStemImage,

        CoverResolutionLane::DbSourcePath,

        CoverResolutionLane::FolderDiscovery,

    };

}



inline std::vector<CoverResolutionLane> GetFolderCoverResolutionOrder(const std::string& requestPathUtf8) {

    std::vector<CoverResolutionLane> lanes;

    lanes.reserve(3);

    lanes.push_back(CoverResolutionLane::ThumbByKey);

    if (LooksLikeCoverImagePath(requestPathUtf8)) {

        lanes.push_back(CoverResolutionLane::DirectImagePath);

    }

    lanes.push_back(CoverResolutionLane::FolderDiscovery);

    return lanes;

}



// Enables cover_thumbs lookups after database initialization.
void SetDatabase(Database* db);



// Supplies the AppState settings used by cover performance modes.
void SetAppState(::AppState* appState);



// 设置封面缓存目录（文件系统分片存储）

void SetCoverCacheDir(const std::string& cacheDirUtf8);



// Supplies the shared byte-based memory budget.

void SetMemoryBudget(MemoryBudget* budget);



// Sets the UTF-8 library root. Parent-folder lookup never climbs above it; empty means unrestricted.
void SetLibraryRoot(const std::string& libraryRootUtf8);



struct TextureHandle {

    uint32_t textureId = 0; // OpenGL GLuint

    int width = 0;

    int height = 0;

    bool hasTexture = false;

};



enum class CoverKind {

    FolderCover,   // cover.jpg, folder.png, or another folder artwork file
    EmbeddedCover  // Artwork embedded in an audio file
};



struct CoverItem {

    CoverKind kind = CoverKind::FolderCover;

    std::string key;            // Stable identity used for deduplication and caching
    std::string label;          // 展示用名称（可用于搜索过滤）

    std::string folderPathUtf8; // Folder represented by folder artwork

    std::string filePathUtf8;   // Audio file represented by embedded artwork

    // Loads folder artwork from this path without reversing the identity key.
    std::string folderCoverSrcPathUtf8;

    // Global-wall rows represented by this tile after visual deduplication.
    std::vector<std::string> wallMemberKeys;
    std::vector<std::string> wallVisualTokens;
    std::string wallSelectionKey;



    int64_t hitCount = 0;       // Weight used for sorting and performance filtering
    bool isPlaceholder = false;

};



// Incrementally discovers unique artwork identities from a path batch.
class CoverWall {

public:

    void SetSource(const std::vector<std::string>& filePathsUtf8, uint64_t signature);

    void StepDiscover(int maxStepsPerFrame);

    void Reset();



    bool IsDone() const { return m_done; }

    size_t Cursor() const { return m_cursor; }

    size_t Total() const { return m_filePaths.size(); }

    const std::vector<CoverItem>& Items() const { return m_items; }



private:

    uint64_t m_signature = 0;

    std::vector<std::string> m_filePaths;

    size_t m_cursor = 0;

    bool m_done = true;

    std::vector<CoverItem> m_items;

    std::vector<std::string> m_seenKeys; // unused in async path; kept for ABI compat

    // T2.3 + T2.3a: async discovery with generation token (latest-wins)

    uint64_t m_discoveryGen = 0;

    std::shared_ptr<std::atomic<bool>> m_cancelled;

    std::future<std::pair<uint64_t, std::vector<CoverItem>>> m_discoveryFuture;

};



// Hashes the current path batch to decide whether the cover wall needs rebuilding.
uint64_t ComputeBatchSignatureXXH3(const std::vector<std::string>& filePathsUtf8, uint64_t seed = 0);



// Loads current-file artwork, preferring embedded artwork and caching by folder.
// outCoverFolderUtf8 receives the folder scope used by cover filtering, if any.
bool LoadCurrentFileCoverTexture(const std::string& audioFilePathUtf8,

                                 TextureHandle& outTex,

                                 std::string* outCoverFolderUtf8 = nullptr);



// Loads folder artwork through the LRU cache.
// retryIfFailed permits one retry of a failed load for the selected-file panel.
bool GetFolderCoverTexture(const std::string& folderPathUtf8, TextureHandle& outTex, bool retryIfFailed = false);



// Loads embedded artwork cached by identity key.
// lodLevel: 0=source size, 1=64x64 list view, 2=512x512 detail view.

// retryIfFailed permits one retry of a failed load for the selected file.
bool GetEmbeddedCoverTexture(const std::string& embeddedKey,

                             const std::string& representativeFilePathUtf8,

                             TextureHandle& outTex,

                             int lodLevel = 1,

                             bool retryIfFailed = false);



// Artwork grid: prefer the cover-key thumbnail cache.

// forceRetry bypasses the negative cache after an explicit user retry.

bool GetCoverTextureByKey(const CoverItem& item, TextureHandle& outTex, bool forceRetry = false);

bool TryGetCachedCoverTextureByKey(const CoverItem& item, TextureHandle& outTex);

bool IsCoverTextureHandleCurrent(const CoverItem& item, uint32_t textureId);



// Album grids load the supplied image directly instead of searching parent folders.
// This prevents albums sharing a parent cover.jpg from displaying the same artwork.
bool GetCoverTextureFromImagePath(const std::string& imageFilePathUtf8, TextureHandle& outTex, bool retryIfFailed = false);



// Removes a successfully-cached image-path texture so the next call to

// GetCoverTextureFromImagePath re-loads from disk (needed after avatar.png overwrites).

void EvictCoverTextureByImagePath(const std::string& imageFilePathUtf8);



// Cover-key filter support.

// Returns whether an audio file belongs to the requested CoverItem identity.
// Folder artwork keys are stable identities and are not guaranteed to encode a path.
// - Embedded cover:"E|<xxh3_64_hex>|<size>|<mimeType>"

bool FileHasCoverKey(const std::string& audioFilePathUtf8, const std::string& coverKey);



// 获取文件夹中的封面图片路径（cover.jpg/folder.png 等）

// skipNegativeCache forces a fresh disk scan after a previous no-artwork result.
// This UI-aware overload must be called from the main thread.
bool GetFolderCoverImagePath(const std::string& folderPathUtf8, std::string& outCoverPathUtf8, bool skipNegativeCache = false);

// Background-safe overload: no ImGui frame state.
// skipNegativeCache ignores a prior no-artwork result.
// useDeepSearch allows inherited artwork from parent folders.
bool GetFolderCoverImagePathBackground(const std::string& folderPathUtf8,

                                       std::string& outCoverPathUtf8,

                                       bool skipNegativeCache = true,

                                       bool useDeepSearch = true);



// Loads embedded artwork by file path when a cached key became stale after an edit.

bool GetEmbeddedCoverTextureByPath(const std::string& audioFilePathUtf8, TextureHandle& outTex);



// Invalidates only the supplied files after artwork is pasted or removed.
void InvalidateFileCoverCaches(const std::vector<std::string>& filePaths);



// Invalidates one moved path so stale artwork does not flash.

void InvalidateCoverForPath(const std::string& filePathUtf8);



// Clears folder-artwork path caches after a database switch or rescan.
void ClearFolderCoverCache();



// Clears failed-load entries so a later filter action can retry them.
void ResetFailedCache();



// Releases the OpenGL texture selected by MemoryBudget eviction.

void ReleaseCoverEvictedEntry(const std::string& key);



// Returns whether this cover identity is in the failed-load cache.
bool IsCoverKeyFailed(const CoverItem& item);

bool IsCoverKeyPendingOrFailed(const CoverItem& item);

bool IsCoverIdentityKeyPendingOrFailed(const CoverItem& item);

bool HasFailedCoverIdentityWork();

bool HasPendingOrFailedCoverIdentityWork();

struct CoverIdentityFilterStats {

    size_t hiddenFailed = 0;

    size_t hiddenPending = 0;

};

CoverIdentityFilterStats CopyCoverItemsExcludingIdentityPendingOrFailed(const std::vector<CoverItem>& items,

                                                                       std::vector<CoverItem>& outItems);

size_t CopyCoverItemsExcludingIdentityFailed(const std::vector<CoverItem>& items,

                                             std::vector<CoverItem>& outItems);



// Removes failed entries from a cover list and returns the number removed.

size_t RemoveFailedCoverItems(std::vector<CoverItem>& items);



// Background cover-wall cache



struct CoverWallStatus {

    uint64_t signature = 0;

    int totalFiles = 0;

    int processedFiles = 0;

    int uniqueCovers = 0;

    bool done = false;

};



// CoverKey cache persistence helpers (DB <-> CoverModule)

struct CoverKeyCacheUpdate {

    std::string filePathUtf8;

    bool updateEmbedded = false;

    bool updateFolder = false;

    std::string embeddedCoverKey;

    int64_t embeddedCoverKeyMtime = 0; // unix seconds

    std::string folderCoverKey;

    std::string folderCoverSrcPathUtf8;

    int64_t folderCoverSrcMtime = 0;   // unix seconds

};



// Seeds the in-memory cover-key cache from persisted database values.
void PrimeFileCoverKeysFromDb(const std::string& filePathUtf8,

                              const std::string& embeddedKey,

                              int64_t embeddedKeyMtime,

                              const std::string& folderKey,

                              const std::string& folderSrcPathUtf8,

                              int64_t folderSrcMtime);



// Drains cover-key updates on the UI thread; the caller persists them.
// Returns the number of updates moved into outUpdates.
size_t DrainCoverKeyCacheUpdates(std::vector<CoverKeyCacheUpdate>& outUpdates, size_t maxItems = 256);



// Starts background cover-wall deduplication and thumbnail decoding.
// signature changes when the filter or path batch changes.
// filePathsUtf8 contains the complete current result or selection.

void RequestCoverWallPrecache(const std::vector<std::string>& filePathsUtf8, uint64_t signature);



// Copies the current cover-wall snapshot.

void GetCoverWallSnapshot(std::vector<CoverItem>& outItems, CoverWallStatus& outStatus);



// Copies only when the revision changed, avoiding a large vector copy every frame.
// inoutRevision is caller-owned and starts at zero.
// Returns true when outItems and outStatus were refreshed.

bool GetCoverWallSnapshotIfChanged(uint64_t* inoutRevision,

                                   std::vector<CoverItem>& outItems,

                                   CoverWallStatus& outStatus);



// Returns all files in this batch that match coverKey.
// The UI can filter by path membership without decoding every file artwork.
bool GetMatchingFilesForCoverKey(uint64_t signature,

                                 const std::string& coverKey,

                                 std::vector<std::string>& outFilePaths,

                                 CoverWallStatus* outStatus = nullptr);



// Fast folder-cover key:

// Reads only the in-memory CoverModule cache; never touches disk.
// Returns a stable identity key, not necessarily a reversible path.
// outCoverPathUtf8 receives the actual image path when requested.
bool TryGetFolderCoverKeyFast(const std::string& folderPathUtf8, std::string& outKey,

                              std::string* outCoverPathUtf8 = nullptr);



// Preloads folder artwork once after search results arrive.

// This avoids per-frame texture loads while rendering.
void PreCacheFolderCovers(const std::vector<std::string>& folderPathsUtf8);



/// Cancels queued viewport decode work and invalidates older generations.
void InvalidateViewportCoverTasks();



// Uploads decoded thumbnails to OpenGL from the UI thread.

// maxUploadsPerFrame is a hard cap; 2-8 is typical.
// budgetMicroseconds stops early when the frame budget is exhausted; zero disables the time limit.
void PumpCoverTextureUploads(int maxUploadsPerFrame, int budgetMicroseconds = 0);



// Reports queued decodes or textures awaiting upload.
// Used by the splash screen to wait for cover work to settle.

bool HasPendingCoverWork();

struct CoverWorkQueueStats {

    int pendingKeys = 0;

    int asyncQueue = 0;

    int uploadQueue = 0;

    int uploadQueuedKeys = 0;

    int failedKeys = 0;

};

CoverWorkQueueStats GetPendingCoverWorkStats();

int GetPendingCoverWorkCount();



// Stops the worker thread before process shutdown.

void ShutdownCoverWallPrecache();



// Cancels current cover work and discards stale results during a database switch.
void CancelPendingCoverWork();



// Clears artwork caches after artwork changes.

void ClearAllCoverCaches();



// Clears negative folder-artwork entries so the next lookup rescans disk.

void ClearFolderCoverNegativeCache();



// Builds a shared key from image content and file size.

//

// Performs 4 KB of disk I/O plus a file-size lookup; forbidden on the UI thread.
// Callers must guarantee:

//   - Do not call from the UI thread.
//   - Results pass through CanonicalCoverKey and may be compared directly.

std::string NormalizeCoverKeyByVisualIdentity(const std::string& absolutePath);



// Canonicalizes a key without I/O before unordered_map or SQL equality use.

// CanonicalCoverKey is required before equality or hashing.
//   - F|H|<hex>|<size>            returned unchanged

//   - F|PATH|<path>               normalizes case and separators in the path segment

//   - F|<legacy_path>             becomes F|PATH|<canonical> for older databases

//   - E|...                       returned unchanged for embedded artwork

//   - empty or invalid             returns empty
//

// 设计契约: docs/plans/2026-04-30-canonical-display-separation.md (T1.0 v2)

//

// Invariants:

//   - Idempotent: CanonicalCoverKey(CanonicalCoverKey(x)) == CanonicalCoverKey(x)

//   - Invalid input returns an empty string and never throws.
std::string CanonicalCoverKey(std::string_view rawKey);



// Returns true for legacy F|<path> keys, excluding F|H| and F|PATH|.
inline bool IsLegacyFolderCoverKey(const std::string& key) {

    return !key.empty() &&

           key.rfind("F|", 0) == 0 &&

           key.rfind("F|H|", 0) != 0 &&

           key.rfind("F|PATH|", 0) != 0;

}



// UI-safe key normalization: memory only, with no disk I/O.
// Converts F|<path> to F|PATH|<lowercase_path>; canonical keys pass through.
// Intended for UI deduplication; use the full background version for cross-machine identity.
inline std::string NormalizeCoverKeyLightweight(const std::string& key) {

    if (key.empty()) return key;

    if (key.rfind("F|H|", 0) == 0 || key.rfind("F|PATH|", 0) == 0 || key.rfind("E|", 0) == 0)

        return key; // Already canonical

    if (key.rfind("F|", 0) == 0) {

        std::string path = key.substr(2);

        // Normalize case and separators in memory.
        for (auto& c : path) {

            if (c == '/') c = '\\';

            if (c >= 'A' && c <= 'Z') c = c + ('a' - 'A');

        }

        return std::string("F|PATH|") + path;

    }

    return key;

}



// Perceptual hash: decode, resize to 8x8 grayscale, then compute a 64-bit average hash.
// Visually similar images have nearby hashes; callers apply the Hamming-distance threshold.
uint64_t ComputePerceptualHash(const uint8_t* imageData, size_t dataSize);



// Convert raw cover image bytes into the compact JPEG payload used by cover_thumbs.

bool PrepareCoverThumbForStorage(const std::vector<uint8_t>& originalFileData,

                                 std::vector<uint8_t>& outThumbData,

                                 std::string& outMime);



} // namespace CoverModule

