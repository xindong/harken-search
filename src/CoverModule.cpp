#include "CoverModule.h"
#include "CoverIdentityRefreshState.h"
#include "PathUtils.h"          // Canonical cover-key paths.
#include "ExternalLibraryCover.h"
#include "AppState.h"          // Application settings and database state.
#include "MetadataReader.h"
#include "Database.h"
#include "Utils.h"
#include "MemoryBudget.h"
#include "DevToolsModule.h"
#include "DiagnosticsLogger.h"

#include "imgui.h"

#include "platform/image_codec.h"
#include "platform/cover_discovery.h"
#include "platform/filesystem.h"
#include "platform/system.h"
#include "platform/text_encoding.h"

#ifdef __APPLE__
#include <OpenGL/gl.h>
#else
#ifdef _WIN32
#ifndef WINGDIAPI
#define WINGDIAPI __declspec(dllimport)
#define ARIS_DEFINED_WINGDIAPI
#endif
#ifndef APIENTRY
#define APIENTRY __stdcall
#define ARIS_DEFINED_APIENTRY
#endif
#endif
#include <GL/gl.h>
#ifdef _WIN32
#ifdef ARIS_DEFINED_APIENTRY
#undef APIENTRY
#undef ARIS_DEFINED_APIENTRY
#endif
#ifdef ARIS_DEFINED_WINGDIAPI
#undef WINGDIAPI
#undef ARIS_DEFINED_WINGDIAPI
#endif
#endif
#endif

#include <algorithm>
#include <cctype>
#include <cwctype>
#include <memory>      // std::unique_ptr owns the XXH3 state.
#include <filesystem>
#include <fstream>
#include <unordered_map>
#include <cstring>
#include <deque>
#include <vector>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <chrono>
#include <atomic>
#include <future>
#include <unordered_set>
#include <list>
#include <cmath>

// stb_image fallback: declarations only (implementation in stb_image_impl.cpp)
#include "../thirdparty/stb_image_full.h"

// xxHash (header-only)
#define XXH_INLINE_ALL
#define XXH_IMPLEMENTATION
#include "../thirdparty/xxhash.h"

namespace CoverModule {

// 全局 Database 指针
static Database* s_database = nullptr;
static ::AppState* g_appState = nullptr; // Non-owning application state.
static std::string s_coverCacheDirUtf8; 
static ::MemoryBudget* s_memoryBudget = nullptr; 
static std::string s_libraryRootUtf8;

static bool IsWaveformForegroundRebuildActive() {
    return g_appState && g_appState->waveformRebuildActive;
}

// LRU Folder Cover Path Cache：O(1) 查询/更新，固定容量，自动淘汰最旧
struct FolderCoverCacheEntry {
    std::string folder;
    std::string coverPath;
};
static const size_t kMaxFolderCoverCache = 20000;
static std::list<FolderCoverCacheEntry> s_folderCoverCacheList;
static std::unordered_map<std::string, std::list<FolderCoverCacheEntry>::iterator> s_folderCoverCacheMap;
static std::unordered_set<std::string> s_folderCoverDeepNegativeFolders;

// Serializes metadata and cover-path caches shared by worker and UI threads.
static std::mutex g_metaCacheMutex;

static bool FolderCoverCacheGet(const std::string& folder, std::string& outPath) {
    std::lock_guard<std::mutex> lk(g_metaCacheMutex);
    auto it = s_folderCoverCacheMap.find(folder);
    if (it == s_folderCoverCacheMap.end()) return false;
    s_folderCoverCacheList.splice(s_folderCoverCacheList.begin(), s_folderCoverCacheList, it->second);
    outPath = it->second->coverPath;
    return true;
}

static void FolderCoverCachePut(const std::string& folder, const std::string& coverPath) {
    std::lock_guard<std::mutex> lk(g_metaCacheMutex);
    if (!coverPath.empty()) s_folderCoverDeepNegativeFolders.erase(folder);
    auto it = s_folderCoverCacheMap.find(folder);
    if (it != s_folderCoverCacheMap.end()) {
        it->second->coverPath = coverPath;
        s_folderCoverCacheList.splice(s_folderCoverCacheList.begin(), s_folderCoverCacheList, it->second);
        return;
    }
    s_folderCoverCacheList.push_front({folder, coverPath});
    s_folderCoverCacheMap[folder] = s_folderCoverCacheList.begin();
    if (s_folderCoverCacheList.size() > kMaxFolderCoverCache) {
        auto last = s_folderCoverCacheList.end();
        --last;
        s_folderCoverDeepNegativeFolders.erase(last->folder);
        s_folderCoverCacheMap.erase(last->folder);
        s_folderCoverCacheList.pop_back();
    }
}

static bool FolderCoverDeepNegativeGet(const std::string& folder) {
    std::lock_guard<std::mutex> lk(g_metaCacheMutex);
    return s_folderCoverDeepNegativeFolders.find(folder) != s_folderCoverDeepNegativeFolders.end();
}

static void FolderCoverDeepNegativePut(const std::string& folder) {
    std::lock_guard<std::mutex> lk(g_metaCacheMutex);
    s_folderCoverDeepNegativeFolders.insert(folder);
}

void SetDatabase(Database* db) { s_database = db; }

void SetAppState(::AppState* appState) { g_appState = appState; }

void SetCoverCacheDir(const std::string& cacheDirUtf8) {
    s_coverCacheDirUtf8 = cacheDirUtf8;
    if (!s_coverCacheDirUtf8.empty()) {
        try {
            Platform::FileSystem::EnsureDirectoryExists(s_coverCacheDirUtf8);
        } catch (...) { }
    }
}

void SetMemoryBudget(::MemoryBudget* budget) { s_memoryBudget = budget; }

void SetLibraryRoot(const std::string& libraryRootUtf8) {
    s_libraryRootUtf8 = libraryRootUtf8;
    while (!s_libraryRootUtf8.empty() && (s_libraryRootUtf8.back() == '/' || s_libraryRootUtf8.back() == '\\'))
        s_libraryRootUtf8.pop_back();
}

// Maps visual identities to shared canonical cover keys.
static std::mutex g_visualDedupMutex;
static std::unordered_map<std::string, std::string> g_visualDedupMap;

// 匿名命名空间：内部实现

namespace {

namespace fs = std::filesystem;

// -------------------------
// 结构体定义
// -------------------------
struct TextureEntry {
    GLuint textureId = 0;
    int width = 0;
    int height = 0;
    uint64_t lastUsedFrame = 0;
    bool hasTexture = false;
};

struct PendingUpload {
    CoverKind kind = CoverKind::FolderCover;
    std::string key;
    std::string folderPathUtf8; 
    std::string filePathUtf8;   
    std::vector<uint8_t> pixels; // RGBA
    int w = 0;
    int h = 0;
};

// -------------------------
// 异步加载基础设施
// -------------------------
struct AsyncLoadTask {
    std::string key;
    std::string path;
    bool isEmbedded;
    int lodLevel;
    bool highPriority = false; // Needed by the current viewport.
    /// 入队时的视口代际；落后于 g_asyncGeneration 的任务视为僵尸，解码前丢弃
    uint64_t generation = 0;
    bool discoverIdentity = false;
    uint64_t identityTicket = 0;
};

static std::mutex g_asyncMutex;
static std::deque<AsyncLoadTask> g_asyncQueue;
static size_t g_identityDiscoveryRunning = 0; // guarded by g_asyncMutex
/// 列表快速滚动 / 换搜索批次时递增；用于丢弃过时异步解码任务
static std::atomic<uint64_t> g_asyncGeneration{0};
static std::unordered_set<std::string> g_asyncPendingKeys;
// 失败缓存：TTL 制 + 重试计数，超过 kMaxRetries 次后永久标记（避免无限重试浪费 CPU）
static constexpr double kFailTtlSec = 10.0;
static constexpr int kMaxRetries = 3; // 3 次失败后永久黑名单
struct FailEntry { double failedAtSec; int retryCount; };
static std::unordered_map<std::string, FailEntry> g_asyncFailedKeys; // key → failure info
static const size_t kMaxAsyncFailedKeys = 10000;
static std::atomic<bool> g_asyncThreadRunning{false};
static std::vector<std::thread> g_asyncThreads; // Decoder worker pool.
static std::condition_variable g_asyncCv;

// 上传队列
static std::mutex g_uploadMutex;
static std::deque<PendingUpload> g_uploadQueue;
static std::unordered_set<std::string> g_uploadQueuedKeys;
// A 256x256 RGBA upload is about 256 KiB; 256 entries cap queued pixels near 64 MiB.
static const size_t kMaxUploadQueueSize = 256;

// Bound pending-key memory during large cover-wall requests.
static const size_t kMaxAsyncQueueSize = 20000;

static void PushUpload(PendingUpload&& u) {
    std::lock_guard<std::mutex> lk(g_uploadMutex);
    if (u.key.empty()) return;
    if (g_uploadQueuedKeys.find(u.key) != g_uploadQueuedKeys.end()) return;
    
    // Completed decodes must reach the UI queue; worker backpressure enforces
    // the queue capacity before this call.
    g_uploadQueuedKeys.insert(u.key);
    g_uploadQueue.push_back(std::move(u));
}

// -------------------------
// 辅助函数
// -------------------------
static std::wstring Utf8ToWide(const std::string& s) {
    return Platform::TextEncoding::Utf8ToWidePermissive(s);
}

static std::string WideToUtf8(const std::wstring& ws) {
    return Platform::TextEncoding::WideToUtf8Permissive(ws);
}

static std::string BaseNameFromPathUtf8(const std::string& pathUtf8) {
    try {
        fs::path p = fs::path(Utf8ToWide(pathUtf8));
        return WideToUtf8(p.filename().wstring());
    } catch (...) { return pathUtf8; }
}

static std::string ParentFolderUtf8(const std::string& filePathUtf8) {
    try {
        fs::path p = fs::path(Utf8ToWide(filePathUtf8));
        return WideToUtf8(p.parent_path().wstring());
    } catch (...) { return ""; }
}

static bool DecodeImageFromMemory(const uint8_t* data, size_t dataSize,
                                  std::vector<uint8_t>& outPixels, int& width, int& height) {
    auto result = Platform::ImageCodec::DecodeImageFromMemory(data, dataSize);
    if (!result.rgba.empty() && result.width > 0 && result.height > 0) {
        outPixels = std::move(result.rgba);
        width = result.width;
        height = result.height;
        return true;
    }

    // WIC rejects some non-standard embedded JPEGs that still appear in real
    // libraries. stb_image is more lenient and lets the wall render them
    // instead of leaving persistent placeholders.
    int channels = 0;
    unsigned char* stbPixels =
        stbi_load_from_memory(data, (int)dataSize, &width, &height, &channels, 4);
    if (!stbPixels || width <= 0 || height <= 0) {
        return false;
    }

    outPixels.assign(stbPixels,
                     stbPixels + (size_t)width * (size_t)height * 4);
    stbi_image_free(stbPixels);
    return true;
}

static void DownscaleRgbaToMaxDim(std::vector<uint8_t>& pixels, int& w, int& h, int maxDim) {
    if (pixels.empty() || w <= 0 || h <= 0) return;
    if (maxDim <= 0) return;
    if (w <= maxDim && h <= maxDim) return;

    float scale = std::min((float)maxDim / (float)w, (float)maxDim / (float)h);
    int nw = std::max(1, (int)std::round((float)w * scale));
    int nh = std::max(1, (int)std::round((float)h * scale));
    std::vector<uint8_t> out((size_t)nw * (size_t)nh * 4, 0);

    for (int y = 0; y < nh; ++y) {
        int sy = std::min(h - 1, (int)((float)y / (float)nh * (float)h));
        for (int x = 0; x < nw; ++x) {
            int sx = std::min(w - 1, (int)((float)x / (float)nw * (float)w));
            size_t si = ((size_t)sy * (size_t)w + (size_t)sx) * 4;
            size_t di = ((size_t)y * (size_t)nw + (size_t)x) * 4;
            out[di + 0] = pixels[si + 0];
            out[di + 1] = pixels[si + 1];
            out[di + 2] = pixels[si + 2];
            out[di + 3] = pixels[si + 3];
        }
    }
    pixels.swap(out);
    w = nw;
    h = nh;
}

// JPEG 编码：将 RGBA 像素编码为 JPEG 字节流（用于 DB 存储瘦身）
// maxDim: 编码前先缩到该尺寸；quality: JPEG 质量 0.0~1.0
static bool EncodeRgbaToJpegWIC(const std::vector<uint8_t>& rgbaPixels, int w, int h,
                                 std::vector<uint8_t>& outJpeg, int maxDim = 256, float quality = 0.80f) {
    if (rgbaPixels.empty() || w <= 0 || h <= 0) return false;

    // Downscale first
    std::vector<uint8_t> scaled = rgbaPixels;
    int sw = w, sh = h;
    DownscaleRgbaToMaxDim(scaled, sw, sh, maxDim);

    outJpeg = Platform::ImageCodec::EncodeRgbaToJpeg(scaled.data(), sw, sh, quality);
    return !outJpeg.empty();
}

// 将原始图片数据缩小为 256px JPEG 后再存 DB（减少 blobs DB 体积 10-20x）
static bool DownscaleForDbStorage(const std::vector<uint8_t>& originalFileData,
                                   std::vector<uint8_t>& outSmallJpeg, std::string& outMime) {
    if (originalFileData.size() < 16) return false;
    std::vector<uint8_t> pixels;
    int w = 0, h = 0;
    if (!DecodeImageFromMemory(originalFileData.data(), originalFileData.size(), pixels, w, h))
        return false;
    int maxDim = (g_appState ? g_appState->settings.thumbnailMaxResolution : 256);
    if (maxDim <= 0) maxDim = 256;
    if (EncodeRgbaToJpegWIC(pixels, w, h, outSmallJpeg, maxDim, 0.82f)) {
        outMime = "image/jpeg";
        return true;
    }
    return false;
}

static bool LoadImageFileUtf8(const std::string& filePathUtf8, std::vector<uint8_t>& outPixels, int& width, int& height) {
    if (filePathUtf8.empty()) return false;
    std::vector<uint8_t> fileData =
        Platform::FileSystem::ReadFileBytes(filePathUtf8, 50 * 1024 * 1024);
    if (fileData.size() < 16) return false;
    return DecodeImageFromMemory(fileData.data(), fileData.size(), outPixels, width, height);
}

// 仅本文件内使用：读文件到 buffer，供 folder cover 从文件解码成功后 saveCoverThumb 用（与 LoadImageFileUtf8 同大小约束）
static bool ReadFileToVectorUtf8(const std::string& filePathUtf8, std::vector<uint8_t>& outData) {
    if (filePathUtf8.empty()) return false;
    outData = Platform::FileSystem::ReadFileBytes(filePathUtf8, 50 * 1024 * 1024);
    return outData.size() >= 16;
}

static std::string PathToUtf8(const fs::path& p) {
#ifdef _WIN32
    return WideToUtf8(p.wstring());
#else
    return p.string();
#endif
}

// 策略 B 中排除广告/推广类文件名（公众号、扫码、免费领等），避免用垃圾图当文件夹封面
static bool IsLikelyAdvertiserImageFileName(const std::wstring& wStem) {
    if (wStem.empty()) return false;
    std::wstring lower = wStem;
    for (auto& c : lower) c = std::towlower(c);
    // 常见推广/广告关键词（中文 + 英文）
    static const std::vector<std::wstring> junkPatterns = {
        L"公众号", L"免费领", L"扫码", L"关注", L"领资源", L"添加微信", L"加微信",
        L"wechat", L"weixin", L"qrcode", L"qr code", L"follow", L"scan", L"promo"
    };
    for (const auto& p : junkPatterns)
        if (lower.find(p) != std::wstring::npos) return true;
    return false;
}

// Probe folder cover names through the dedicated discovery policy boundary.
static bool FindFolderCoverImagePathFast(const std::string& folderPathUtf8, std::string& outCoverPathUtf8, bool useDeepSearch = false, int maxDepth = 5) {
    outCoverPathUtf8.clear();
    if (folderPathUtf8.empty()) return false;

    std::string cached;
    if (FolderCoverCacheGet(folderPathUtf8, cached)) {
        if (!cached.empty()) {
            outCoverPathUtf8 = cached;
            return true;
        }
        if (!useDeepSearch || FolderCoverDeepNegativeGet(folderPathUtf8)) return false;
    }

    const auto result = Platform::CoverDiscovery::Find(
        folderPathUtf8, s_libraryRootUtf8, useDeepSearch, maxDepth);
    if (result.status == Platform::CoverDiscovery::Status::Found) {
        outCoverPathUtf8 = result.selectedPathUtf8;
        FolderCoverCachePut(folderPathUtf8, outCoverPathUtf8);
        return true;
    }
    if (result.status == Platform::CoverDiscovery::Status::ProbeError) return false;
    if (useDeepSearch) FolderCoverDeepNegativePut(folderPathUtf8);
    FolderCoverCachePut(folderPathUtf8, "");
    return false;
}

static void ResolveEmbeddedIdentityTask(Database* db, const AsyncLoadTask& task);

static void AsyncLoaderThread() {
    Platform::InitThreadCom();
    Platform::SetThreadBackgroundPriority();
    Database localDb;
    bool dbOpened = false;
    if (s_database && !s_database->getMainDbPath().empty()) {
        dbOpened = localDb.open(s_database->getMainDbPath());
    }
    Database* dbPtr = dbOpened ? &localDb : nullptr;

    while (g_asyncThreadRunning) {
        AsyncLoadTask task;
        {
            std::unique_lock<std::mutex> lk(g_asyncMutex);
            g_asyncCv.wait(lk, [] { return !g_asyncQueue.empty() || !g_asyncThreadRunning; });
            if (!g_asyncThreadRunning) break;
            task = g_asyncQueue.front();
            g_asyncQueue.pop_front();
            if (task.discoverIdentity) ++g_identityDiscoveryRunning;
        }

        const std::string originalKey = task.key;

        if (task.generation != g_asyncGeneration.load(std::memory_order_relaxed)) {
            std::lock_guard<std::mutex> lk(g_asyncMutex);
            g_asyncPendingKeys.erase(originalKey);
            if (task.discoverIdentity) --g_identityDiscoveryRunning;
            continue;
        }

        if (task.discoverIdentity) {
            ResolveEmbeddedIdentityTask(dbPtr, task);
            std::lock_guard<std::mutex> lk(g_asyncMutex);
            --g_identityDiscoveryRunning;
            continue;
        }

        std::vector<uint8_t> pixels;
        int w = 0, h = 0;
        bool success = false;
        std::string folderCoverImagePathUtf8;

        if (task.isEmbedded) {
            // Embedded tasks use their canonical key and shared 512-pixel limit.
            std::string embKey = task.key;
            std::vector<uint8_t> embeddedArtData;
            std::string embeddedArtMime;

            auto tryDecodeEmbeddedArt = [&](const std::string& candidatePathUtf8) -> bool {
                if (candidatePathUtf8.empty()) return false;

                AlbumArt art;
                if (!MetadataReader::readAlbumArt(candidatePathUtf8, art) || !art.hasData()) {
                    return false;
                }

                std::vector<uint8_t> decodedPixels;
                int decodedW = 0;
                int decodedH = 0;
                if (!DecodeImageFromMemory(art.data.data(), art.data.size(), decodedPixels, decodedW, decodedH)) {
                    return false;
                }

                int thumbSize = g_appState ? g_appState->settings.thumbnailMaxResolution : 256;
                DownscaleRgbaToMaxDim(decodedPixels, decodedW, decodedH, thumbSize);
                pixels = std::move(decodedPixels);
                w = decodedW;
                h = decodedH;
                embeddedArtData = std::move(art.data);
                embeddedArtMime = art.mimeType;
                return true;
            };

            // Prefer DB thumb cache first (no file I/O, robust against stale file paths).
            if (dbPtr && !embKey.empty()) {
                std::vector<uint8_t> thumbData;
                std::string thumbMime;
                if (dbPtr->getCoverThumb(embKey, thumbData, thumbMime) && !thumbData.empty()) {
                        if (DecodeImageFromMemory(thumbData.data(), thumbData.size(), pixels, w, h)) {
                        // 动态缩略图尺寸
                        int thumbSize = g_appState ? g_appState->settings.thumbnailMaxResolution : 256;
                        DownscaleRgbaToMaxDim(pixels, w, h, thumbSize);
                        success = true;
                    }
                }
            }

            // Fallback to reading embedded art from source file
            if (!success) {
                if (tryDecodeEmbeddedArt(task.path)) {
                    success = true;
                }
            }

            if (!success) {
                AlbumArt art;
                if (MetadataReader::readAlbumArt(task.path, art) && art.hasData()) {
                            if (DecodeImageFromMemory(art.data.data(), art.data.size(), pixels, w, h)) {
                        // 动态缩略图尺寸
                        int thumbSize = g_appState ? g_appState->settings.thumbnailMaxResolution : 256;
                        DownscaleRgbaToMaxDim(pixels, w, h, thumbSize);
                        success = true;
                    }
                }
            }

            if (success && dbPtr && !embKey.empty() && !embeddedArtData.empty()) {
                std::vector<uint8_t> smallJpeg;
                std::string smallMime;
                if (DownscaleForDbStorage(embeddedArtData, smallJpeg, smallMime)) {
                    dbPtr->saveCoverThumb(embKey, smallJpeg, smallMime);
                } else {
                    dbPtr->saveCoverThumb(
                        embKey,
                        embeddedArtData,
                        embeddedArtMime.empty() ? std::string("image/jpeg") : embeddedArtMime);
                }
            }
        } else {
            std::string coverPath;

            // 先按 key 读持久化 thumb，不再假设 key 可反解为路径
            if (dbPtr && !task.key.empty()) {
                std::vector<uint8_t> thumbData;
                std::string thumbMime;
                if (dbPtr->getCoverThumb(task.key, thumbData, thumbMime) && !thumbData.empty()) {
                    if (DecodeImageFromMemory(thumbData.data(), thumbData.size(), pixels, w, h)) {
                        int thumbSize = g_appState ? g_appState->settings.thumbnailMaxResolution : 256;
                        DownscaleRgbaToMaxDim(pixels, w, h, thumbSize);
                        success = true;
                    }
                }
            }

            // thumb 没命中时：V2 优先把 task.path 当真实图片路径直接读，再回退到按 folder 发现
            if (!success) {
                auto hasImageExt = [](const std::string& p) {
                    if (p.size() < 4) return false;
                    const size_t last = p.find_last_of(".\\/");
                    if (last == std::string::npos || p[last] != '.') return false;
                    std::string ext = p.substr(last);
                    for (char& c : ext) c = (char)std::tolower((unsigned char)c);
                    return ext == ".jpg" || ext == ".jpeg" || ext == ".png" || ext == ".webp" || ext == ".bmp" || ext == ".gif";
                };
                if (!task.path.empty() && hasImageExt(task.path)) {
                    std::vector<uint8_t> fileData;
                    if (ReadFileToVectorUtf8(task.path, fileData) &&
                        DecodeImageFromMemory(fileData.data(), fileData.size(), pixels, w, h)) {
                        int thumbSize = g_appState ? g_appState->settings.thumbnailMaxResolution : 256;
                        DownscaleRgbaToMaxDim(pixels, w, h, thumbSize);
                        success = true;
                        folderCoverImagePathUtf8 = task.path;
                        if (dbPtr && !task.key.empty() && !fileData.empty()) {
                            // DB 瘦身：存缩小后的 JPEG 而非原图
                            std::vector<uint8_t> smallJpeg;
                            std::string smallMime;
                            if (DownscaleForDbStorage(fileData, smallJpeg, smallMime)) {
                                dbPtr->saveCoverThumb(task.key, smallJpeg, smallMime);
                            } else {
                                bool isPng = (task.path.size() >= 4 &&
                                    (task.path.compare(task.path.size() - 4, 4, ".png") == 0 ||
                                     task.path.compare(task.path.size() - 4, 4, ".PNG") == 0));
                                const char* mime = isPng ? "image/png" : "image/jpeg";
                                dbPtr->saveCoverThumb(task.key, fileData, mime);
                            }
                        }
                    }
                }
                if (!success && (ExternalLibraryCover::tryGetCoverForFolder(task.path, coverPath) ||
                                FindFolderCoverImagePathFast(task.path, coverPath, true))) {
                    std::vector<uint8_t> fileData;
                    if (ReadFileToVectorUtf8(coverPath, fileData) &&
                        DecodeImageFromMemory(fileData.data(), fileData.size(), pixels, w, h)) {
                        int thumbSize = g_appState ? g_appState->settings.thumbnailMaxResolution : 256;
                        DownscaleRgbaToMaxDim(pixels, w, h, thumbSize);
                        success = true;
                        folderCoverImagePathUtf8 = coverPath;
                        if (dbPtr && !task.key.empty() && !fileData.empty()) {
                            // DB 瘦身：存缩小后的 JPEG 而非原图
                            std::vector<uint8_t> smallJpeg;
                            std::string smallMime;
                            if (DownscaleForDbStorage(fileData, smallJpeg, smallMime)) {
                                dbPtr->saveCoverThumb(task.key, smallJpeg, smallMime);
                            } else {
                                bool isPng = (coverPath.size() >= 4 &&
                                    (coverPath.compare(coverPath.size() - 4, 4, ".png") == 0 ||
                                     coverPath.compare(coverPath.size() - 4, 4, ".PNG") == 0));
                                const char* mime = isPng ? "image/png" : "image/jpeg";
                                dbPtr->saveCoverThumb(task.key, fileData, mime);
                            }
                        }
                    }
                }
            }
            if (success && !folderCoverImagePathUtf8.empty() && task.key.rfind("F|", 0) != 0) {
                task.key = NormalizeCoverKeyByVisualIdentity(folderCoverImagePathUtf8);
            }
        }

        if (success && task.generation != g_asyncGeneration.load(std::memory_order_relaxed)) {
            success = false;
        }

        if (success) {
            // Apply backpressure until the UI upload queue has capacity; a
            // successfully decoded image is not discarded.
            while (g_asyncThreadRunning) {
                size_t queueSz;
                {
                    std::lock_guard<std::mutex> ulk(g_uploadMutex);
                    queueSz = g_uploadQueue.size();
                }
                if (queueSz < kMaxUploadQueueSize) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            
            PendingUpload u;
            u.key = task.key;
            u.kind = task.isEmbedded ? CoverKind::EmbeddedCover : CoverKind::FolderCover;
            u.folderPathUtf8 = task.path;
            u.filePathUtf8 = folderCoverImagePathUtf8; // 封面墙按 "F|filePathUtf8" 查，上传时双 key 写入
            u.pixels = std::move(pixels);
            u.w = w; u.h = h;
            if (task.generation == g_asyncGeneration.load(std::memory_order_relaxed)) {
                PushUpload(std::move(u));
            }
        }

        {
            std::lock_guard<std::mutex> lk(g_asyncMutex);
            g_asyncPendingKeys.erase(originalKey);
            if (task.key != originalKey)
                g_asyncPendingKeys.erase(task.key);
            if (success) {
                g_asyncFailedKeys.erase(task.key);
                if (task.key != originalKey)
                    g_asyncFailedKeys.erase(originalKey);
            } else {
                if (g_asyncFailedKeys.size() >= kMaxAsyncFailedKeys)
                    g_asyncFailedKeys.clear();
                double nowSec = std::chrono::duration<double>(
                    std::chrono::steady_clock::now().time_since_epoch()).count();
                auto& fe = g_asyncFailedKeys[originalKey];
                fe.retryCount++;
                fe.failedAtSec = nowSec;
            }
        }
    }
    Platform::UninitThreadCom();
}

static bool StartAsyncLoader() {
    // UI-thread admission: the switch worker may be joining the old pool.
    if (g_appState && g_appState->dbSwitchInProgress) return false;
    if (g_asyncThreads.empty()) {
        g_asyncThreadRunning = true;
        int threadCount = 4;
        if (g_appState) {
            int profile = g_appState->settings.memoryProfile;
            if (profile == 0 || g_appState->settings.threadLimit == 1) {
                threadCount = 1; // Eco: 单线程，不抢占 DAW 资源
            } else if (profile == 2 || g_appState->settings.threadLimit == -1) {
                threadCount = std::max(4, (int)std::thread::hardware_concurrency() - 2); // Ultra: 跑满
            } else {
                threadCount = std::max(2, (int)std::thread::hardware_concurrency() / 2); // Balanced
            }
        }
        if (threadCount > 16) threadCount = 16;
        for (int i = 0; i < threadCount; ++i) {
            g_asyncThreads.emplace_back(AsyncLoaderThread);
        }
    }
    return true;
}

// -------------------------
// 纹理缓存管理
// -------------------------
static TextureHandle ToHandle(const TextureEntry& e) {
    TextureHandle h;
    h.textureId = (uint32_t)e.textureId;
    h.width = e.width;
    h.height = e.height;
    h.hasTexture = e.hasTexture && e.textureId != 0;
    return h;
}

static void DeleteTextureIfAny(GLuint& id) {
    if (id != 0) {
        glDeleteTextures(1, &id);
        id = 0;
    }
}

// Cover-path, identity, and texture caches.
static std::unordered_map<std::string, std::string> g_folderCoverPathCache;        // folderPath -> srcPath
static std::unordered_map<std::string, std::string> g_folderCoverIdentityKeyCache; // folderPath -> coverKey(identity)
static std::unordered_map<std::string, TextureEntry> g_embeddedCoverCache;
static std::unordered_map<std::string, std::string> g_fileEmbeddedKeyCache;
static std::unordered_map<std::string, int64_t> g_fileEmbeddedKeyMtimeCache;
static aura::cover::IdentityRefreshState g_identityRefresh;
static std::unordered_map<std::string, std::string> g_fileFolderKeyCache;          // filePath -> coverKey(identity)
static std::unordered_map<std::string, int64_t> g_fileFolderCoverSrcMtimeCache;
static std::unordered_map<std::string, TextureEntry> g_folderCoverKeyCache;        // coverKey(identity) -> texture

static const char* kNoCoverMarker = "\1";

// Texture limits follow the selected memory profile.
static size_t GetMaxEmbeddedCoverTextures() {
    if (!g_appState) return 4096;
    int profile = g_appState->settings.memoryProfile;
    if (profile == 0) return 1024;      // Eco
    if (profile == 2) return 8192;      // Ultra
    return 4096;                        // Balanced
}

static size_t GetMaxFolderCoverKeyTextures() {
    if (!g_appState) return 2048;
    int profile = g_appState->settings.memoryProfile;
    if (profile == 0) return 512;       // Eco
    if (profile == 2) return 4096;      // Ultra
    return 2048;                        // Balanced
}

static void TrimFolderCoverKeyCacheIfNeeded_Impl(); // Defined after the batch trimmer.

// Trim the least-recently-used overage with one linear-selection pass.
template<typename MapType>
static void TrimCacheBatch(MapType& cache, size_t limit) {
    if (cache.size() <= limit) return;

    size_t overage = cache.size() - limit;
    std::vector<typename MapType::iterator> its;
    its.reserve(cache.size());
    for (auto it = cache.begin(); it != cache.end(); ++it) {
        its.push_back(it);
    }

    // 使用快速选择算法 (Quickselect)，将最老（lastUsedFrame 最小）的 overage 个元素移动到数组头部
    std::nth_element(its.begin(), its.begin() + (ptrdiff_t)overage, its.end(),
        [](const auto& a, const auto& b) {
            return a->second.lastUsedFrame < b->second.lastUsedFrame;
        });

    for (size_t i = 0; i < overage; ++i) {
        DeleteTextureIfAny(its[i]->second.textureId);
        if (s_memoryBudget) {
            s_memoryBudget->unregisterEntry(MemoryBudget::CacheType::Cover, its[i]->first);
        }
        cache.erase(its[i]);
    }
}

static void TrimEmbeddedCacheIfNeeded() {
    TrimCacheBatch(g_embeddedCoverCache, GetMaxEmbeddedCoverTextures());
}

static void TrimFolderCoverKeyCacheIfNeeded_Impl() {
    TrimCacheBatch(g_folderCoverKeyCache, GetMaxFolderCoverKeyTextures());
}

static void TrimStringCachesIfNeeded() {
    std::lock_guard<std::mutex> lk(g_metaCacheMutex);
    const size_t kMaxStringCache = 20000;
    const size_t kMaxTotalStringEntries = 10000; // folder/file 为主时尽早裁剪，避免多表合计无限增长
    // 任一字符串 cache 超限即整体裁剪；原逻辑只按 g_fileEmbeddedKeyCache 触发，导致 folder/identity 表从未清理（严重内存泄漏）
    size_t total = g_fileEmbeddedKeyCache.size() + g_folderCoverPathCache.size() + g_folderCoverIdentityKeyCache.size()
                 + g_fileFolderKeyCache.size();
    if (g_fileEmbeddedKeyCache.size() > kMaxStringCache || total > kMaxTotalStringEntries) {
        g_fileEmbeddedKeyCache.clear();
        g_fileEmbeddedKeyMtimeCache.clear();
        g_identityRefresh.clear();
        g_fileFolderKeyCache.clear();
        g_fileFolderCoverSrcMtimeCache.clear();
        // 保留 kNoCoverMarker（负缓存）条目，避免重新探测已知无封面的文件夹（特别是 USB 驱动器慢 IO）。
        // 只清除正向（已找到封面路径）的条目以释放内存。
        // 若负缓存条目本身超过上限，才整体清除。
        {
            const size_t kMaxNegEntries = 20000;
            if (g_folderCoverPathCache.size() > kMaxNegEntries) {
                g_folderCoverPathCache.clear();
            } else {
                for (auto it = g_folderCoverPathCache.begin(); it != g_folderCoverPathCache.end(); ) {
                    if (it->second != kNoCoverMarker) it = g_folderCoverPathCache.erase(it);
                    else ++it;
                }
            }
        }
        g_folderCoverIdentityKeyCache.clear();
        DIAG_METRIC("Memory", "CoverStringCachesTrimmed", 1.0);
    }
}

// TTL 感知的失败检查：超过 kFailTtlSec 的条目视为已过期（需持有 g_asyncMutex）
// 超过 kMaxRetries 次的条目永久标记，不再自动过期（避免无限重试浪费 CPU）
// 带预计算时间戳的版本（批量场景避免每次 clock::now）
static bool IsKeyFailedLockedAt(const std::string& key, double nowSec) {
    auto it = g_asyncFailedKeys.find(key);
    if (it == g_asyncFailedKeys.end()) return false;
    // 永久失败：超过最大重试次数，不再过期
    if (it->second.retryCount >= kMaxRetries) return true;
    if ((nowSec - it->second.failedAtSec) >= kFailTtlSec) {
        g_asyncFailedKeys.erase(it);
        return false;
    }
    return true;
}
static bool IsKeyFailedLocked(const std::string& key) {
    double nowSec = std::chrono::duration<double>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    return IsKeyFailedLockedAt(key, nowSec);
}


// -------------------------
// 核心加载函数 (非阻塞)
// -------------------------
// 统一语义：
// - 入参可以是 folderPath（列表场景）
// - 也可以是 stable cover key（封面墙/数据库场景）
// 但 stable key 绝不再被当成物理路径反解。
// path 字段只承载"用于发现真实图片的 folderPath"。
// retryIfFailed: 为 true 时（如「当前选中文件」Info 面板）允许忽略失败集并重试一次
// hintSrcPathUtf8: V2 真实图片路径，有值时 worker 优先从此路径读图，不再反解 key。
static bool EnsureFolderCoverTextureLoaded(const std::string& keyOrFolderPathUtf8, TextureEntry& outEntry, bool retryIfFailed = false, const std::string& hintFolderPathUtf8 = "", const std::string& hintSrcPathUtf8 = "") {
    if (keyOrFolderPathUtf8.empty()) return false;

    const bool looksLikeKey =
        keyOrFolderPathUtf8.rfind("F|", 0) == 0 ||
        keyOrFolderPathUtf8.rfind("E|", 0) == 0;

    // 1) 传进来的是稳定 key：只当身份，不再反解路径
    if (looksLikeKey) {
        const std::string& stableKey = keyOrFolderPathUtf8;

        auto it = g_folderCoverKeyCache.find(stableKey);
        if (it != g_folderCoverKeyCache.end() && it->second.hasTexture) {
            it->second.lastUsedFrame = (uint64_t)ImGui::GetFrameCount();
            if (s_memoryBudget) s_memoryBudget->updateUsage(MemoryBudget::CacheType::Cover, stableKey);
            outEntry = it->second;
            return true;
        }

        {
            std::lock_guard<std::mutex> lk(g_asyncMutex);
            if (retryIfFailed) g_asyncFailedKeys.erase(stableKey);
            {
                auto fit = g_asyncFailedKeys.find(stableKey);
                if (fit != g_asyncFailedKeys.end()) {
                    // 永久失败：超过最大重试次数
                    if (fit->second.retryCount >= kMaxRetries) return false;
                    double nowSec = std::chrono::duration<double>(
                        std::chrono::steady_clock::now().time_since_epoch()).count();
                    if ((nowSec - fit->second.failedAtSec) < kFailTtlSec) return false; // 冷却中
                    g_asyncFailedKeys.erase(fit); // TTL 到期，允许重试
                }
            }
            if (g_asyncPendingKeys.count(stableKey)) return false;
        }

        // V2：优先用真实图片路径；否则 key 反解或 folder hint（仅作回退）
        std::string pathForLookup = hintSrcPathUtf8;
        if (pathForLookup.empty()) pathForLookup = NormalizeFolderPathFromKeyForLookup(stableKey);
        if (pathForLookup.empty() && !hintFolderPathUtf8.empty())
            pathForLookup = hintFolderPathUtf8;

        // 禁止无效请求：无可用路径时不入队，避免 worker 反复失败（视觉 key F|H|... 无法反解路径）
        if (pathForLookup.empty()) {
            std::lock_guard<std::mutex> lk(g_asyncMutex);
            if (g_asyncFailedKeys.size() >= kMaxAsyncFailedKeys)
                g_asyncFailedKeys.clear();
            { double ns = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
              auto& fe = g_asyncFailedKeys[stableKey];
              fe.retryCount = kMaxRetries; // 无路径 → 直接永久标记
              fe.failedAtSec = ns; }
            return false;
        }

        if (!StartAsyncLoader()) return false;
        {
            const uint64_t gen = g_asyncGeneration.load(std::memory_order_relaxed);
            std::lock_guard<std::mutex> lk(g_asyncMutex);
            if (g_asyncQueue.size() >= kMaxAsyncQueueSize) return false;
            g_asyncPendingKeys.insert(stableKey);
            g_asyncQueue.push_front({stableKey, pathForLookup, false, 0, true, gen});
            g_asyncCv.notify_one();
        }
        return false;
    }

    // 2) 传进来的是 folderPath：用于实时发现/列表预取
    const std::string& folderPath = keyOrFolderPathUtf8;

    // Resolve folders through the identity map so equivalent covers share a texture.
    {
        auto keyIt = g_folderCoverIdentityKeyCache.find(folderPath);
        if (keyIt != g_folderCoverIdentityKeyCache.end() &&
            !keyIt->second.empty() &&
            keyIt->second != kNoCoverMarker) {
            auto wallIt = g_folderCoverKeyCache.find(keyIt->second);
            if (wallIt != g_folderCoverKeyCache.end() && wallIt->second.hasTexture) {
                wallIt->second.lastUsedFrame = (uint64_t)ImGui::GetFrameCount();
                if (s_memoryBudget) s_memoryBudget->updateUsage(MemoryBudget::CacheType::Cover, keyIt->second);
                outEntry = wallIt->second;
                return true;
            }
        }
    }

    {
        std::lock_guard<std::mutex> lk(g_asyncMutex);
        if (retryIfFailed) g_asyncFailedKeys.erase(folderPath);
        if (IsKeyFailedLocked(folderPath)) return false;
        if (g_asyncPendingKeys.count(folderPath)) return false;
    }

    if (!StartAsyncLoader()) return false;
    {
        const uint64_t gen = g_asyncGeneration.load(std::memory_order_relaxed);
        std::lock_guard<std::mutex> lk(g_asyncMutex);
        if (g_asyncQueue.size() >= kMaxAsyncQueueSize) return false;
        g_asyncPendingKeys.insert(folderPath);
        g_asyncQueue.push_front({folderPath, folderPath, false, 0, true, gen});
        g_asyncCv.notify_one();
    }

    return false;
}

// retryIfFailed: 为 true 时（当前选中文件）允许忽略失败集并重试一次
static bool EnsureEmbeddedCoverTextureLoaded(const std::string& key,
    const std::string& representativeFilePathUtf8,
    TextureEntry& outEntry,
    int lodLevel = 1,
    bool retryIfFailed = false) {
    
    if (key.empty() || representativeFilePathUtf8.empty()) return false;

    // Use the source identity key at every LOD so cache lookup remains stable.
    std::string cacheKey = key; 

    // 1. 检查缓存
    auto it = g_embeddedCoverCache.find(cacheKey);
    if (it != g_embeddedCoverCache.end()) {
        if (it->second.hasTexture) {
            it->second.lastUsedFrame = (uint64_t)ImGui::GetFrameCount();
            if (s_memoryBudget) s_memoryBudget->updateUsage(MemoryBudget::CacheType::Cover, cacheKey);
            outEntry = it->second;
            return true;
        }
    }

    // 2. 检查是否正在加载中 / 已失败；用户主动点进时允许重试
    {
        std::lock_guard<std::mutex> lk(g_asyncMutex);
        if (retryIfFailed) g_asyncFailedKeys.erase(cacheKey);
        if (IsKeyFailedLocked(cacheKey)) return false;
        if (g_asyncPendingKeys.count(cacheKey)) return false;
    }

    // 3. 发起异步任务
    if (!StartAsyncLoader()) return false;
    {
        const uint64_t gen = g_asyncGeneration.load(std::memory_order_relaxed);
        std::lock_guard<std::mutex> lk(g_asyncMutex);
        if (g_asyncQueue.size() >= kMaxAsyncQueueSize) return false;
        g_asyncPendingKeys.insert(cacheKey);
        // Selected-file and viewport requests enter at the front of the queue.
        g_asyncQueue.push_front({cacheKey, representativeFilePathUtf8, true, lodLevel, true, gen});
        g_asyncCv.notify_one();
    }

    return false;
}

// Current file cover cache
struct CurrentCoverCache {
    TextureEntry tex;
    std::string cachedFolderPathUtf8;
    // Negative cache: avoid retrying synchronous USB I/O every frame when cover not found
    bool loadFailed = false;
    std::string failedFolderPath;
    std::string failedFilePath;
};
static CurrentCoverCache g_current;

static bool StartsWith(const std::string& s, const char* prefix);
static bool ComputeFolderCoverKeyForFile(const std::string& filePathUtf8, std::string& outKey);

static std::string Hex64(uint64_t v) {
    static const char* hex = "0123456789abcdef";
    std::string s(16, '0');
    for (int i = 15; i >= 0; --i) {
        s[(size_t)i] = hex[(size_t)(v & 0xF)];
        v >>= 4;
    }
    return s;
}

static bool TryGetPathMtimeUnixSecondsUtf8(const std::string& pathUtf8, int64_t& outUnixSeconds) {
    outUnixSeconds = 0;
    if (pathUtf8.empty()) return false;
    try {
        outUnixSeconds = Platform::FileSystem::LastWriteTimeUnixSeconds(pathUtf8);
        return outUnixSeconds > 0;
    } catch (...) {
        outUnixSeconds = 0;
        return false;
    }
}

static bool StartsWith(const std::string& s, const char* prefix) {
    if (!prefix) return false;
    size_t n = std::strlen(prefix);
    return s.size() >= n && std::memcmp(s.data(), prefix, n) == 0;
}

// CoverKey updates
static std::mutex g_keyUpdatesMutex;
static std::deque<CoverKeyCacheUpdate> g_keyUpdates;
static std::unordered_set<std::string> g_keyUpdatesQueuedPaths;

static void EnqueueKeyUpdate(const CoverKeyCacheUpdate& u) {
    if (u.filePathUtf8.empty()) return;
    if (!u.updateEmbedded && !u.updateFolder) return;
    std::lock_guard<std::mutex> lk(g_keyUpdatesMutex);
    if (g_keyUpdatesQueuedPaths.find(u.filePathUtf8) != g_keyUpdatesQueuedPaths.end()) return;
    g_keyUpdatesQueuedPaths.insert(u.filePathUtf8);
    g_keyUpdates.push_back(u);
}

// dbPtr: 调用方传入的数据库连接；后台线程传本地连接，主线程传 s_database，避免共享句柄锁争用
static bool ComputeEmbeddedCoverKeyForFile(Database* dbPtr, const std::string& filePathUtf8,
                                          std::string& outKey,
                                          const AsyncLoadTask* request = nullptr) {
    outKey.clear();
    if (filePathUtf8.empty()) return false;
    const auto currentRequest = [&] {
        return !request || (request->generation == g_asyncGeneration.load(std::memory_order_relaxed) &&
            g_identityRefresh.current(filePathUtf8, request->generation, request->identityTicket));
    };
    std::string cachedKey;
    int64_t cachedMtime = 0;
    {
        std::lock_guard<std::mutex> lk(g_metaCacheMutex);
        if (!currentRequest()) return false;
        const auto it = g_fileEmbeddedKeyCache.find(filePathUtf8);
        if (it != g_fileEmbeddedKeyCache.end()) cachedKey = it->second;
        const auto mt = g_fileEmbeddedKeyMtimeCache.find(filePathUtf8);
        if (mt != g_fileEmbeddedKeyMtimeCache.end()) cachedMtime = mt->second;
    }
    // This function runs on workers for ordinary preview/prefetch. Stat is
    // deliberately outside the shared lock, including positive-cache hits.
    int64_t mtime = 0;
    const bool haveMtime = TryGetPathMtimeUnixSecondsUtf8(filePathUtf8, mtime);
    if (!cachedKey.empty() && cachedKey != kNoCoverMarker &&
        (!haveMtime || (cachedMtime > 0 && cachedMtime == mtime))) {
        outKey = cachedKey;
        return true;
    }
    if (!request && cachedKey == kNoCoverMarker &&
        (!haveMtime || (cachedMtime > 0 && cachedMtime == mtime))) return false;
    // Negative entries are re-probed only on the background refresh interval;
    // a newly added sidecar image need not change the audio file's timestamp.
    AlbumArt art;
    bool hasArt = MetadataReader::readAlbumArt(filePathUtf8, art) && art.hasData();

    if (!hasArt) {
        try {
            fs::path p = fs::path(Utf8ToWide(filePathUtf8));
            std::wstring wStem = p.stem().wstring();
            fs::path parentDir = p.parent_path();

            if (!parentDir.empty() && !wStem.empty()) {
                for (const std::wstring ext : { L".jpg", L".png", L".jpeg" }) {
                    fs::path imgPath = parentDir / (wStem + ext);
                    const std::string imgPathUtf8 = PathToUtf8(imgPath);
                    if (Platform::FileSystem::Exists(imgPathUtf8)) {
                        art.data = Platform::FileSystem::ReadFileBytes(
                            imgPathUtf8, 32 * 1024 * 1024);
                        if (!art.data.empty()) {
                            art.mimeType = (ext == L".png") ? "image/png" : "image/jpeg";
                            hasArt = true;
                            break;
                        }
                    }
                }
            }
        } catch (...) {}
    }


    if (hasArt) {
        const uint64_t hash = (uint64_t)XXH3_64bits(art.data.data(), art.data.size());
        outKey = CanonicalCoverKey(std::string("E|") + Hex64(hash) + "|" +
                                  std::to_string(art.data.size()) + "|" + art.mimeType);
    }
    // Capture mtime before the read: edits during discovery must trigger a
    // later refresh rather than marking old bytes as the new file version.
    {
        std::lock_guard<std::mutex> lk(g_metaCacheMutex);
        if (!currentRequest()) { outKey.clear(); return false; }
        g_fileEmbeddedKeyCache[filePathUtf8] = outKey.empty() ? kNoCoverMarker : outKey;
        if (haveMtime) g_fileEmbeddedKeyMtimeCache[filePathUtf8] = mtime;
        else g_fileEmbeddedKeyMtimeCache.erase(filePathUtf8);
        if (haveMtime) {
            CoverKeyCacheUpdate update;
            update.filePathUtf8 = filePathUtf8;
            update.updateEmbedded = true;
            update.embeddedCoverKey = outKey;
            update.embeddedCoverKeyMtime = mtime;
            EnqueueKeyUpdate(update);
        }
    }
    if (dbPtr && !outKey.empty() && art.data.size() > 16 && !dbPtr->hasCoverThumb(outKey)) {
        std::vector<uint8_t> smallJpeg;
        std::string smallMime;
        if (DownscaleForDbStorage(art.data, smallJpeg, smallMime))
            dbPtr->saveCoverThumb(outKey, smallJpeg, smallMime);
        else
            dbPtr->saveCoverThumb(outKey, art.data, art.mimeType);
    }
    return !outKey.empty();
}

static double IdentityRefreshNow() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

static void ResolveEmbeddedIdentityTask(Database* db, const AsyncLoadTask& task) {
    DiagScopedTimer timer("CoverIdentity", "BackgroundResolve");
    try {
        std::string key;
        ComputeEmbeddedCoverKeyForFile(db, task.path, key, &task);
    } catch (...) {
        DIAG_ACTION("CoverIdentity", "BackgroundResolveFailed");
    }
    std::lock_guard<std::mutex> lk(g_metaCacheMutex);
    g_identityRefresh.finish(task.path, task.generation, task.identityTicket, IdentityRefreshNow());
}

// UI entry: memory lookup and bounded queue admission only. An existing
// texture remains usable while its source identity is checked in the worker.
static bool RequestEmbeddedCoverIdentity(const std::string& path, std::string& key) {
    key.clear();
    if (path.empty()) return false;
    bool cached = false;
    {
        std::lock_guard<std::mutex> lk(g_metaCacheMutex);
        const auto it = g_fileEmbeddedKeyCache.find(path);
        cached = it != g_fileEmbeddedKeyCache.end();
        if (cached && it->second != kNoCoverMarker) key = it->second;
    }
    if (!StartAsyncLoader()) return !key.empty();
    // Follow the existing metadata -> queue lock order used by invalidation.
    std::lock_guard<std::mutex> metaLock(g_metaCacheMutex);
    std::lock_guard<std::mutex> queueLock(g_asyncMutex);
    if (g_asyncQueue.size() >= 64) return !key.empty();
    const auto queuedDiscoveries = std::count_if(g_asyncQueue.begin(), g_asyncQueue.end(),
        [](const AsyncLoadTask& task) { return task.discoverIdentity; });
    // Avoid turning a cold library into many competing disk reads. Existing
    // thumbnail decode workers remain free to service ready identities.
    if (g_identityDiscoveryRunning + queuedDiscoveries >= 2) return !key.empty();
    const auto generation = g_asyncGeneration.load(std::memory_order_relaxed);
    const auto ticket = g_identityRefresh.request(path, generation, IdentityRefreshNow());
    if (ticket) {
        g_asyncQueue.push_back({"I|" + path, path, false, 0, false, generation, true, ticket});
        g_asyncCv.notify_one();
    }
    return !key.empty();
}

static bool ComputeFolderCoverKeyForFile(const std::string& filePathUtf8, std::string& outKey) {
    outKey.clear();
    if (filePathUtf8.empty()) return false;

    const std::string folder = ParentFolderUtf8(filePathUtf8);
    if (folder.empty()) {
        g_fileFolderKeyCache[filePathUtf8] = kNoCoverMarker;
        return false;
    }

    // 1. 先查 filePath -> identityKey
    auto it = g_fileFolderKeyCache.find(filePathUtf8);
    if (it != g_fileFolderKeyCache.end()) {
        if (it->second == kNoCoverMarker) return false;

        // V2 语义：mtime 校验只看 srcPath，不再从 key 反解路径
        auto pathIt = g_folderCoverPathCache.find(folder);
        auto mtIt = g_fileFolderCoverSrcMtimeCache.find(filePathUtf8);
        if (pathIt != g_folderCoverPathCache.end() &&
            pathIt->second != kNoCoverMarker &&
            mtIt != g_fileFolderCoverSrcMtimeCache.end() &&
            mtIt->second > 0) {
            int64_t cur = 0;
            if (TryGetPathMtimeUnixSecondsUtf8(pathIt->second, cur) && cur != mtIt->second) {
                g_fileFolderKeyCache.erase(it);
                g_fileFolderCoverSrcMtimeCache.erase(mtIt);
                g_folderCoverIdentityKeyCache.erase(folder);
            } else {
                outKey = it->second;
                g_folderCoverIdentityKeyCache[folder] = outKey;
                return true;
            }
        } else {
            outKey = it->second;
            g_folderCoverIdentityKeyCache[folder] = outKey;
            return true;
        }
    }

    // 2. 重新发现真实 srcPath
    std::string coverPath;
    if (!FindFolderCoverImagePathFast(folder, coverPath) || coverPath.empty()) {
        g_fileFolderKeyCache[filePathUtf8] = kNoCoverMarker;
        g_folderCoverPathCache[folder] = kNoCoverMarker;
        g_folderCoverIdentityKeyCache.erase(folder);
        return false;
    }

    // 3. 先把真实路径缓存下来
    g_folderCoverPathCache[folder] = coverPath;

    // 4. 使用视觉特征 key（XXH3 hash），与 CoverWall::StepDiscover 一致，避免去重失败。
    // T2.2: NormalizeCoverKeyByVisualIdentity 出口已包 CanonicalCoverKey;
    //       fallback "F|" + coverPath 也必须经 CanonicalCoverKey 升级为 F|PATH|<canonical>
    outKey = NormalizeCoverKeyByVisualIdentity(coverPath);
    if (outKey.empty()) outKey = CanonicalCoverKey(std::string("F|") + coverPath);

    g_fileFolderKeyCache[filePathUtf8] = outKey;
    g_folderCoverIdentityKeyCache[folder] = outKey;

    int64_t mt = 0;
    if (TryGetPathMtimeUnixSecondsUtf8(coverPath, mt) && mt > 0) {
        g_fileFolderCoverSrcMtimeCache[filePathUtf8] = mt;
    }

    CoverKeyCacheUpdate u;
    u.filePathUtf8 = filePathUtf8;
    u.updateFolder = true;
    u.folderCoverKey = outKey;
    u.folderCoverSrcPathUtf8 = coverPath;
    u.folderCoverSrcMtime = mt;
    EnqueueKeyUpdate(u);

    return true;
}

// -------------------------
// Cover Wall Infrastructure
// -------------------------
static std::mutex g_wallMutex;
static std::vector<CoverItem> g_wallItems;
static CoverWallStatus g_wallStatus;
static std::atomic<uint64_t> g_wallRevision{1};

static std::mutex g_wallIndexMutex;
static uint64_t g_wallIndexSig = 0;
static std::unordered_map<std::string, std::vector<std::string>> g_wallIndex;

static std::mutex g_wallCacheMutex;
static std::unordered_map<uint64_t, std::vector<CoverItem>> g_wallCache;
static std::deque<uint64_t> g_wallCacheLru;
static const size_t kMaxWallCacheEntries = 12;

static bool WallCacheGet(uint64_t sig, std::vector<CoverItem>& outItems) {
    std::lock_guard<std::mutex> lk(g_wallCacheMutex);
    auto it = g_wallCache.find(sig);
    if (it == g_wallCache.end()) return false;
    outItems = it->second;
    g_wallCacheLru.erase(std::remove(g_wallCacheLru.begin(), g_wallCacheLru.end(), sig), g_wallCacheLru.end());
    g_wallCacheLru.push_back(sig);
    return true;
}

static void WallCachePut(uint64_t sig, const std::vector<CoverItem>& items) {
    std::lock_guard<std::mutex> lk(g_wallCacheMutex);
    g_wallCache[sig] = items;
    g_wallCacheLru.erase(std::remove(g_wallCacheLru.begin(), g_wallCacheLru.end(), sig), g_wallCacheLru.end());
    g_wallCacheLru.push_back(sig);
    while (g_wallCacheLru.size() > kMaxWallCacheEntries) {
        uint64_t old = g_wallCacheLru.front();
        g_wallCacheLru.pop_front();
        g_wallCache.erase(old);
    }
}

static std::mutex g_jobMutex;
static std::condition_variable g_jobCv;
static std::vector<std::string> g_jobFiles;
static uint64_t g_jobSig = 0;
static std::atomic<bool> g_jobPending{false};
static std::atomic<bool> g_jobExit{false};
static std::atomic<bool> g_jobCancel{false};
static std::thread g_jobThread;

static void SetWallSnapshotLocked(std::vector<CoverItem>&& items, const CoverWallStatus& st) {
    std::lock_guard<std::mutex> lk(g_wallMutex);
    g_wallItems = std::move(items);
    g_wallStatus = st;
    g_wallRevision.fetch_add(1, std::memory_order_relaxed);
}

static void JobThreadMain() {
    Platform::InitThreadCom();
    Database localDb;
    bool dbOpened = false;
    if (s_database && !s_database->getMainDbPath().empty()) {
        dbOpened = localDb.open(s_database->getMainDbPath());
    }
    Database* dbPtr = dbOpened ? &localDb : nullptr;

    while (!g_jobExit.load()) {
        std::unique_lock<std::mutex> lk(g_jobMutex);
        g_jobCv.wait(lk, [] { return g_jobPending.load() || g_jobExit.load(); });
        if (g_jobExit.load()) break;

        std::vector<std::string> files = g_jobFiles;
        uint64_t sig = g_jobSig;
        g_jobPending.store(false);
        g_jobCancel.store(false);
        lk.unlock();

        CoverWallStatus st;
        st.signature = sig;
        st.totalFiles = (int)files.size();
        st.processedFiles = 0;
        st.uniqueCovers = 0;
        st.done = files.empty();

        {
            std::lock_guard<std::mutex> slk(g_wallMutex);
            g_wallItems.clear();
            g_wallStatus = st;
            g_wallRevision.fetch_add(1, std::memory_order_relaxed);
        }
        {
            std::lock_guard<std::mutex> ilk(g_wallIndexMutex);
            g_wallIndexSig = sig;
            g_wallIndex.clear();
            g_wallIndex.reserve(2048);
        }

        std::unordered_set<std::string> seen;
        seen.reserve(2048);
        std::vector<CoverItem> items;
        items.reserve(512);

        std::unordered_map<std::string, std::vector<std::string>> localIndex;
        localIndex.reserve(2048);

        const int kThumbMax = 256;

        for (size_t i = 0; i < files.size(); ++i) {
            if (g_jobExit.load() || g_jobCancel.load()) break;

            // 背压：上传队列积压时暂停解码，让主线程先消费，
            // 避免像素数据无限堆积导致 GB 级内存峰值。
            for (int bp = 0; bp < 200; ++bp) { // 最多等 ~2 秒
                size_t queueSz;
                {
                    std::lock_guard<std::mutex> ulk(g_uploadMutex);
                    queueSz = g_uploadQueue.size();
                }
                if (queueSz < kMaxUploadQueueSize / 2) break; // 队列降到半满以下再继续
                if (g_jobExit.load() || g_jobCancel.load()) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            if (g_jobExit.load() || g_jobCancel.load()) break;

            const std::string& filePath = files[i];
            st.processedFiles = (int)(i + 1);

            std::string folderKey;
            if (ComputeFolderCoverKeyForFile(filePath, folderKey) && !folderKey.empty()) {
                localIndex[folderKey].push_back(filePath);
                if (seen.insert(folderKey).second) {
                    CoverItem it;
                    it.kind = CoverKind::FolderCover;
                    it.key = folderKey;
                    it.folderPathUtf8 = ParentFolderUtf8(filePath);
                    it.label = "";
                    items.push_back(it);
                    st.uniqueCovers = (int)items.size();

                    {
                        std::lock_guard<std::mutex> slk(g_wallMutex);
                        g_wallItems.push_back(it);
                        g_wallStatus = st;
                        g_wallRevision.fetch_add(1, std::memory_order_relaxed);
                    }

                    // V2：读图只用 src_path，不从 key 反解
                    std::string coverPath;
                    {
                        std::string folder = ParentFolderUtf8(filePath);
                        std::lock_guard<std::mutex> lk(g_metaCacheMutex);
                        auto itCache = g_folderCoverPathCache.find(folder);
                        if (itCache != g_folderCoverPathCache.end() && itCache->second != kNoCoverMarker && !itCache->second.empty())
                            coverPath = itCache->second;
                    }
                    std::vector<uint8_t> pixels;
                    int w = 0, h = 0;
                    if (!coverPath.empty() && LoadImageFileUtf8(coverPath, pixels, w, h)) {
                        DownscaleRgbaToMaxDim(pixels, w, h, kThumbMax);
                        PendingUpload u;
                        u.kind = CoverKind::FolderCover;
                        u.key = folderKey;
                        u.folderPathUtf8 = it.folderPathUtf8;
                        u.pixels = std::move(pixels);
                        u.w = w; u.h = h;
                        if (!g_jobCancel.load(std::memory_order_acquire) &&
                            !g_jobExit.load(std::memory_order_acquire)) {
                            PushUpload(std::move(u));
                        }
                    }
                }
            }

            std::string embKey;
            if (ComputeEmbeddedCoverKeyForFile(dbPtr, filePath, embKey) && !embKey.empty()) {
                localIndex[embKey].push_back(filePath);
                if (seen.insert(embKey).second) {
                    CoverItem it;
                    it.kind = CoverKind::EmbeddedCover;
                    it.key = embKey;
                    it.filePathUtf8 = filePath;
                    it.label = "";
                    items.push_back(it);
                    st.uniqueCovers = (int)items.size();

                    {
                        std::lock_guard<std::mutex> slk(g_wallMutex);
                        g_wallItems.push_back(it);
                        g_wallStatus = st;
                        g_wallRevision.fetch_add(1, std::memory_order_relaxed);
                    }

                    bool thumbDecoded = false;
                    {
                        std::vector<uint8_t> thumbData;
                        std::string thumbMime;
                        if (dbPtr && dbPtr->getCoverThumb(embKey, thumbData, thumbMime) && !thumbData.empty()) {
                            std::vector<uint8_t> pixels;
                            int w = 0, h = 0;
                            if (DecodeImageFromMemory(thumbData.data(), thumbData.size(), pixels, w, h)) {
                                DownscaleRgbaToMaxDim(pixels, w, h, kThumbMax);
                                PendingUpload u;
                                u.kind = CoverKind::EmbeddedCover;
                                u.key = embKey;
                                u.filePathUtf8 = filePath;
                                u.pixels = std::move(pixels);
                                u.w = w; u.h = h;
                                if (!g_jobCancel.load(std::memory_order_acquire) &&
                                    !g_jobExit.load(std::memory_order_acquire)) {
                                    PushUpload(std::move(u));
                                }
                                thumbDecoded = true;
                            }
                        }
                    }
                    if (!thumbDecoded) {
                        AlbumArt art;
                        if (MetadataReader::readAlbumArt(filePath, art) && art.hasData()) {
                            if (dbPtr && !embKey.empty() && art.data.size() > 16) {
                                if (!dbPtr->hasCoverThumb(embKey)) {
                                    std::vector<uint8_t> smallJpeg;
                                    std::string smallMime;
                                    if (DownscaleForDbStorage(art.data, smallJpeg, smallMime)) {
                                        dbPtr->saveCoverThumb(embKey, smallJpeg, smallMime);
                                    } else {
                                        dbPtr->saveCoverThumb(embKey, art.data, art.mimeType);
                                    }
                                }
                            }
                            std::vector<uint8_t> pixels;
                            int w = 0, h = 0;
                            if (DecodeImageFromMemory(art.data.data(), art.data.size(), pixels, w, h)) {
                                DownscaleRgbaToMaxDim(pixels, w, h, kThumbMax);
                                PendingUpload u;
                                u.kind = CoverKind::EmbeddedCover;
                                u.key = embKey;
                                u.filePathUtf8 = filePath;
                                u.pixels = std::move(pixels);
                                u.w = w; u.h = h;
                                if (!g_jobCancel.load(std::memory_order_acquire) &&
                                    !g_jobExit.load(std::memory_order_acquire)) {
                                    PushUpload(std::move(u));
                                }
                            }
                        }
                    } 
                }
            }

            if ((i % 200) == 199 && !localIndex.empty()) {
                std::lock_guard<std::mutex> ilk(g_wallIndexMutex);
                if (g_wallIndexSig == sig) {
                    for (auto& kv : localIndex) {
                        auto& dst = g_wallIndex[kv.first];
                        dst.insert(dst.end(), kv.second.begin(), kv.second.end());
                    }
                }
                localIndex.clear();
            }
        }

        st.done = !g_jobCancel.load() && !g_jobExit.load();
        if (st.done) {
            WallCachePut(sig, items);
        }
        {
            std::lock_guard<std::mutex> slk(g_wallMutex);
            g_wallStatus = st;
            g_wallRevision.fetch_add(1, std::memory_order_relaxed);
        }

        if (!localIndex.empty()) {
            std::lock_guard<std::mutex> ilk(g_wallIndexMutex);
            if (g_wallIndexSig == sig) {
                for (auto& kv : localIndex) {
                    auto& dst = g_wallIndex[kv.first];
                    dst.insert(dst.end(), kv.second.begin(), kv.second.end());
                }
            }
        }
    }

    Platform::UninitThreadCom();
}

static bool EnsureJobThreadStarted() {
    if (g_appState && g_appState->dbSwitchInProgress) return false;
    if (!g_jobThread.joinable()) {
        g_jobExit.store(false);
        g_jobCancel.store(false);
        g_jobPending.store(false);
        g_jobThread = std::thread(JobThreadMain);
    }
    return true;
}

static bool SeenKey(const std::vector<std::string>& seen, const std::string& k) {
    return std::find(seen.begin(), seen.end(), k) != seen.end();
}

static TextureEntry UploadTextureFromPixels(const std::vector<uint8_t>& pixels, int w, int h) {
    TextureEntry e;
    if (pixels.empty() || w <= 0 || h <= 0) return e;
    glGenTextures(1, &e.textureId);
    glBindTexture(GL_TEXTURE_2D, e.textureId);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    e.width = w;
    e.height = h;
    e.hasTexture = true;
    e.lastUsedFrame = (uint64_t)ImGui::GetFrameCount();
    return e;
}

} // namespace

bool PrepareCoverThumbForStorage(const std::vector<uint8_t>& originalFileData,
                                 std::vector<uint8_t>& outThumbData,
                                 std::string& outMime) {
    outThumbData.clear();
    outMime.clear();
    return DownscaleForDbStorage(originalFileData, outThumbData, outMime);
}

// -------------------------
// Public API
// -------------------------

void ReleaseCoverEvictedEntry(const std::string& key) {
    std::lock_guard<std::mutex> lk(g_metaCacheMutex);
    auto itE = g_embeddedCoverCache.find(key);
    if (itE != g_embeddedCoverCache.end()) {
        DeleteTextureIfAny(itE->second.textureId);
        g_embeddedCoverCache.erase(itE);
        return;
    }
    auto itF = g_folderCoverKeyCache.find(key);
    if (itF != g_folderCoverKeyCache.end()) {
        DeleteTextureIfAny(itF->second.textureId);
        g_folderCoverKeyCache.erase(itF);
    }
}

// Normalize fallback paths so case and separator variants share a Windows key.
static std::string NormalizePathForCoverKey(const std::string& pathUtf8) {
#ifdef _WIN32
    // Resolve an existing file to its filesystem-canonical path.
    try {
        if (Platform::FileSystem::Exists(pathUtf8)) {
            const std::string canonical = Platform::FileSystem::WeaklyCanonical(pathUtf8);
            if (!canonical.empty()) {
                // Lowercase the canonical Windows path for comparison.
                std::string result = canonical;
                std::transform(result.begin(), result.end(), result.begin(), [](unsigned char c) {
                    return (char)std::tolower(c);
                });
                return result;
            }
        }
    } catch (...) {}
    // Missing paths fall back to lowercase lexical normalization.
    std::string lower = pathUtf8;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) {
        return (char)std::tolower(c);
    });
    return lower;
#else
    return pathUtf8;
#endif
}

// Canonicalize cover keys without filesystem access before map or SQL lookup.
// Folder paths use CanonicalFolderPath rather than filesystem canonicalization so this path
// remains I/O-free.
std::string CanonicalCoverKey(std::string_view rawKey) {
    if (rawKey.empty()) return std::string();
    std::string s(rawKey);

    // Validate F|H|<hex>|<size> before accepting a hash identity.
    // Missing hash or size components are rejected before cache lookup.
    if (s.rfind("F|H|", 0) == 0) {
        std::string rest = s.substr(4);  // strip "F|H|"
        if (rest.empty()) return std::string();
        size_t pipe = rest.find('|');
        if (pipe == 0) return std::string();              // 空 hex 段 (F|H||...)
        if (pipe == std::string::npos) return std::string(); // 缺 |size 段 (F|H|<hex>)
        // A non-empty hash plus size suffix is a valid identity.
        return s;
    }

    // E|...  — embedded cover, opaque, return as-is (允许 prefix-only 给上游处理)
    if (s.rfind("E|", 0) == 0) return s;

    // F|PATH|<path>[|mtime] — re-normalize the path portion (preserves
    //                         optional |mtime suffix from path-3 case)
    if (s.rfind("F|PATH|", 0) == 0) {
        std::string rest = s.substr(7);  // strip "F|PATH|"
        if (rest.empty()) return std::string();
        size_t pipe = rest.find('|');
        std::string pathPart   = (pipe != std::string::npos) ? rest.substr(0, pipe) : rest;
        std::string mtimeSuffix = (pipe != std::string::npos) ? rest.substr(pipe) : "";
        if (pathPart.empty()) return std::string();
        // Reject paths that become empty after canonicalization.
        std::string canonicalPath = HarkenSearch::CanonicalFolderPath(pathPart);
        if (canonicalPath.empty()) return std::string();
        return std::string("F|PATH|") + canonicalPath + mtimeSuffix;
    }

    // F|<legacy_path>  — old format with no H/PATH prefix → upgrade to F|PATH|
    if (s.rfind("F|", 0) == 0) {
        std::string pathPart = s.substr(2);
        if (pathPart.empty()) return std::string();
        std::string canonicalPath = HarkenSearch::CanonicalFolderPath(pathPart);
        if (canonicalPath.empty()) return std::string();
        return std::string("F|PATH|") + canonicalPath;
    }

    // Unknown prefix — reject (don't pass through; caller likely has a bug)
    return std::string();
}

// Hash image content with XXH3 so same-name, same-size files remain distinct.
// Every return path passes through CanonicalCoverKey.
std::string NormalizeCoverKeyByVisualIdentity(const std::string& absolutePath) {
    if (absolutePath.empty()) return "";

    try {
        const auto fileInfo = Platform::FileSystem::ProbeRegularFile(absolutePath);
        if (!fileInfo.isRegularFile || !fileInfo.sizeAvailable) {
            // T1.1b: defensive wrap (idempotent on already-canonical input)
            return CanonicalCoverKey(std::string("F|PATH|") + NormalizePathForCoverKey(absolutePath));
        }

        if (fileInfo.sizeBytes == 0) {
            return CanonicalCoverKey(std::string("F|PATH|") + NormalizePathForCoverKey(absolutePath));
        }

        std::vector<uint8_t> fileData =
            Platform::FileSystem::ReadFileBytes(absolutePath, 64 * 1024 * 1024);
        size_t fileSize = fileData.size();
        if (fileSize == 0) {
            int64_t mt = 0;
            if (TryGetPathMtimeUnixSecondsUtf8(absolutePath, mt) && mt > 0) {
                return CanonicalCoverKey(std::string("F|PATH|") + NormalizePathForCoverKey(absolutePath) + "|" + std::to_string(mt));
            }
            return CanonicalCoverKey(std::string("F|PATH|") + NormalizePathForCoverKey(absolutePath));
        }

        // 🚀 head 4KB + tail 4KB + size 联合指纹 (T1.3 防碰撞升级)
        // Hash the first and last 4 KiB in one XXH3 stream. Tail bytes capture
        // image and metadata differences that headers alone can miss.
        // Files above 8 KiB omit the middle by design; smaller files hash up to
        // the first 4 KiB. TestCoverHashAntiCollision locks this trade-off.
        constexpr size_t kHeadBytes = 4096;
        constexpr size_t kTailBytes = 4096;

        // RAII ownership frees the XXH3 state on allocation failure, return, or throw.
        std::unique_ptr<XXH3_state_t, decltype(&XXH3_freeState)>
            state(XXH3_createState(), XXH3_freeState);
        if (!state) {
            return CanonicalCoverKey(std::string("F|PATH|") + NormalizePathForCoverKey(absolutePath));
        }
        XXH3_64bits_reset(state.get());

        // head
        size_t headRead = std::min(fileSize, kHeadBytes);
        XXH3_64bits_update(state.get(), fileData.data(), headRead);

        // tail (only if file > head + tail; otherwise head already covered everything)
        if (fileSize > kHeadBytes + kTailBytes) {
            XXH3_64bits_update(state.get(), fileData.data() + fileSize - kTailBytes, kTailBytes);
        }

        const uint64_t h = (uint64_t)XXH3_64bits_digest(state.get());
        const std::string stableKey = std::string("F|H|") + Hex64(h) + "|" + std::to_string(fileSize);

        std::lock_guard<std::mutex> lk(g_visualDedupMutex);
        g_visualDedupMap[stableKey] = stableKey;
        // T1.1b defensive wrap: F|H| forms are already canonical, but wrapping
        // keeps the contract explicit so future return-path edits stay safe.
        return CanonicalCoverKey(stableKey);
    } catch (...) {
        return CanonicalCoverKey(std::string("F|PATH|") + NormalizePathForCoverKey(absolutePath));
    }
}

bool TryGetFolderCoverKeyFast(const std::string& folderPathUtf8, std::string& outKey,
    std::string* outCoverPathUtf8) {
    outKey.clear();
    if (outCoverPathUtf8) outCoverPathUtf8->clear();
    if (folderPathUtf8.empty()) return false;

    // 1. 先查 identity key（folderPath -> coverKey）
    auto keyIt = g_folderCoverIdentityKeyCache.find(folderPathUtf8);
    if (keyIt != g_folderCoverIdentityKeyCache.end()) {
        if (!keyIt->second.empty() && keyIt->second != kNoCoverMarker) {
            outKey = keyIt->second;

            auto pathIt = g_folderCoverPathCache.find(folderPathUtf8);
            if (outCoverPathUtf8 &&
                pathIt != g_folderCoverPathCache.end() &&
                pathIt->second != kNoCoverMarker) {
                *outCoverPathUtf8 = pathIt->second;
            }
            return true;
        }
        return false;
    }

    // 2. 再查真实路径缓存（只用于补 outCoverPathUtf8，不再现场拼 key）
    auto pathIt = g_folderCoverPathCache.find(folderPathUtf8);
    if (pathIt != g_folderCoverPathCache.end()) {
        if (outCoverPathUtf8 && pathIt->second != kNoCoverMarker) {
            *outCoverPathUtf8 = pathIt->second;
        }
        return false;
    }

    // 3. 只查内存，不读盘
    return false;
}

void PreCacheFolderCovers(const std::vector<std::string>& folderPathsUtf8) {
    if (folderPathsUtf8.empty()) return;
    std::vector<std::string> toEnqueue;
    toEnqueue.reserve(folderPathsUtf8.size());
    for (const std::string& folderPathUtf8 : folderPathsUtf8) {
        if (folderPathUtf8.empty()) continue;
        auto keyIt = g_folderCoverIdentityKeyCache.find(folderPathUtf8);
        if (keyIt != g_folderCoverIdentityKeyCache.end() && !keyIt->second.empty() && keyIt->second != kNoCoverMarker) {
            auto texIt = g_folderCoverKeyCache.find(keyIt->second);
            if (texIt != g_folderCoverKeyCache.end() && texIt->second.hasTexture) continue;
        }
        toEnqueue.push_back(folderPathUtf8);
    }
    if (toEnqueue.empty()) return;
    if (!StartAsyncLoader()) return;
    bool anyPushed = false;
    {
        const uint64_t gen = g_asyncGeneration.load(std::memory_order_relaxed);
        std::lock_guard<std::mutex> lk(g_asyncMutex);
        for (const std::string& path : toEnqueue) {
            if (g_asyncQueue.size() >= kMaxAsyncQueueSize) break;
            if (IsKeyFailedLocked(path) || g_asyncPendingKeys.count(path)) continue;
            g_asyncPendingKeys.insert(path);
            // Background preloads enter at the back of the queue.
            g_asyncQueue.push_back({path, path, false, 0, false, gen});
            anyPushed = true;
        }
        if (anyPushed) g_asyncCv.notify_one();
    }
}

void InvalidateViewportCoverTasks() {
    g_asyncGeneration.fetch_add(1, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lk(g_asyncMutex);
    for (const auto& t : g_asyncQueue) {
        g_asyncPendingKeys.erase(t.key);
    }
    g_asyncQueue.clear();
    g_asyncCv.notify_all();
}

uint64_t ComputeBatchSignatureXXH3(const std::vector<std::string>& filePathsUtf8, uint64_t seed) {
    XXH64_hash_t h = (XXH64_hash_t)seed;
    for (const auto& p : filePathsUtf8) {
        h = XXH3_64bits_withSeed(p.data(), p.size(), (XXH64_hash_t)h);
    }
    h = XXH3_64bits_withSeed(&h, sizeof(h), (XXH64_hash_t)filePathsUtf8.size());
    return (uint64_t)h;
}

bool LoadCurrentFileCoverTexture(const std::string& audioFilePathUtf8,
                                 TextureHandle& outTex,
                                 std::string* outCoverFolderUtf8) {
    outTex = TextureHandle();
    if (IsWaveformForegroundRebuildActive()) return false;
    if (audioFilePathUtf8.empty()) return false;

    // The current-file view uses the asynchronous decode queue; selection never
    // performs image I/O or decoding on the UI thread.
    std::string folder = ParentFolderUtf8(audioFilePathUtf8);
    if (outCoverFolderUtf8) *outCoverFolderUtf8 = folder;

    std::string embKey;
    if (RequestEmbeddedCoverIdentity(audioFilePathUtf8, embKey) && !embKey.empty()) {
        TextureEntry e;
        // 请求 LOD 2 (512px) 用于右侧详情面板展示；允许失败后重试一次（点进灰色项时能显示封面）
        bool ok = EnsureEmbeddedCoverTextureLoaded(embKey, audioFilePathUtf8, e, 2, true);
        if (ok) outTex = ToHandle(e);
        return ok && outTex.hasTexture;
    }

    // 回退到异步文件夹封面；允许失败后重试一次
    if (!folder.empty()) {
        TextureEntry e;
        bool ok = EnsureFolderCoverTextureLoaded(folder, e, true);
        if (ok) outTex = ToHandle(e);
        return ok && outTex.hasTexture;
    }

    return false;
}

static bool ShouldEmitCurrentFolderCoverFailureDiagnostic(const std::string& folderPathUtf8,
                                                         bool retryIfFailed) {
#if AURA_BUILD_DIAGNOSTICS
    if (retryIfFailed) {
        return true;
    }
    static std::unordered_map<std::string, double> s_lastFailureLogAt;
    static constexpr size_t kMaxFailureLogKeys = 2048;
    static constexpr double kFailureLogCooldownSec = 2.0;
    const double now = ImGui::GetTime();
    auto it = s_lastFailureLogAt.find(folderPathUtf8);
    if (it != s_lastFailureLogAt.end() &&
        (now - it->second) < kFailureLogCooldownSec) {
        return false;
    }
    if (s_lastFailureLogAt.size() >= kMaxFailureLogKeys) {
        s_lastFailureLogAt.clear();
    }
    s_lastFailureLogAt[folderPathUtf8] = now;
    return true;
#else
    (void)folderPathUtf8;
    (void)retryIfFailed;
    return true;
#endif
}

static void LogCoverLaneThrottled(const char* surface,
                                  const char* lane,
                                  const std::string& key,
                                  int ok,
                                  int retry,
                                  int lodLevel = -1) {
#if AURA_BUILD_DIAGNOSTICS
    struct CoverLaneBucketLogState {
        double windowStart = 0.0;
        int detailedCount = 0;
        int suppressedCount = 0;
        int reportedSuppressedCount = 0;
        int totalCount = 0;
    };
    static std::unordered_map<std::string, double> s_lastLaneLogAt;
    static std::unordered_map<std::string, CoverLaneBucketLogState> s_bucketStates;
    static const size_t kMaxLaneLogKeys = 4096;
    static const size_t kMaxBucketStates = 256;
    static constexpr double kExactKeyCooldownSec = 0.75;
    static constexpr double kBucketWindowSec = 1.5;
    static constexpr int kMaxDetailedPerBucketWindow = 1;
    static constexpr int kSummaryFlushSuppressedCount = 4096;
    const double now = ImGui::GetTime();
    const std::string bucketSig =
        std::string(surface) + "|" + lane + "|" + std::to_string(ok) + "|" + std::to_string(retry) + "|" + std::to_string(lodLevel);
    const std::string sig =
        std::string(surface) + "|" + lane + "|" + key + "|" + std::to_string(ok) + "|" + std::to_string(retry) + "|" + std::to_string(lodLevel);

    auto emitSummary = [&](CoverLaneBucketLogState& bucket) {
        const int pendingSuppressed = bucket.suppressedCount - bucket.reportedSuppressedCount;
        if (pendingSuppressed <= 0) return;
        const int windowMs = (int)((now - bucket.windowStart) * 1000.0);
        if (lodLevel >= 0) {
            DIAG_ACTION_FMT("CoverLane",
                "summary=1 lane=%s surface=%s ok=%d lod=%d retry=%d count=%d windowTotal=%d windowMs=%d",
                lane, surface, ok, lodLevel, retry, pendingSuppressed, bucket.totalCount, windowMs);
        } else {
            DIAG_ACTION_FMT("CoverLane",
                "summary=1 lane=%s surface=%s ok=%d retry=%d count=%d windowTotal=%d windowMs=%d",
                lane, surface, ok, retry, pendingSuppressed, bucket.totalCount, windowMs);
        }
        bucket.reportedSuppressedCount = bucket.suppressedCount;
    };

    if (s_bucketStates.size() >= kMaxBucketStates) s_bucketStates.clear();
    auto& bucket = s_bucketStates[bucketSig];
    if (bucket.windowStart <= 0.0) bucket.windowStart = now;
    if ((now - bucket.windowStart) >= kBucketWindowSec) {
        emitSummary(bucket);
        bucket = CoverLaneBucketLogState{};
        bucket.windowStart = now;
    }

    bucket.totalCount++;
    auto it = s_lastLaneLogAt.find(sig);
    const bool exactCooldownActive =
        (it != s_lastLaneLogAt.end()) && ((now - it->second) < kExactKeyCooldownSec);
    const bool allowDetailed = !exactCooldownActive &&
                               (bucket.detailedCount < kMaxDetailedPerBucketWindow);
    if (!allowDetailed) {
        bucket.suppressedCount++;
        if ((bucket.suppressedCount - bucket.reportedSuppressedCount) >=
            kSummaryFlushSuppressedCount) {
            emitSummary(bucket);
        }
        return;
    }

    if (s_lastLaneLogAt.size() >= kMaxLaneLogKeys) s_lastLaneLogAt.clear();
    s_lastLaneLogAt[sig] = now;
    bucket.detailedCount++;
    if (lodLevel >= 0) {
        DIAG_ACTION_FMT("CoverLane",
            "lane=%s surface=%s key=%s ok=%d lod=%d retry=%d",
            lane, surface, key.c_str(), ok, lodLevel, retry);
    } else {
        DIAG_ACTION_FMT("CoverLane",
            "lane=%s surface=%s key=%s ok=%d retry=%d",
            lane, surface, key.c_str(), ok, retry);
    }
#else
    (void)surface;
    (void)lane;
    (void)key;
    (void)ok;
    (void)retry;
    (void)lodLevel;
#endif
}

static bool ShouldEmitCoverWallLoadFailureDiagnostic(const char* lane) {
#if AURA_BUILD_DIAGNOSTICS
    struct FailureBucketState {
        double windowStart = 0.0;
        int emitted = 0;
        int suppressed = 0;
        int reportedSuppressed = 0;
    };
    static std::unordered_map<std::string, FailureBucketState> s_failureBuckets;
    static constexpr double kFailureBucketWindowSec = 2.0;
    static constexpr int kMaxDetailedPerBucketWindow = 1;
    static constexpr int kSuppressedSummaryStep = 4096;

    const double now = ImGui::GetTime();
    const std::string laneName(lane);
    auto emitSuppressedSummary = [&](const FailureBucketState& bucket, int pendingSuppressed) {
        if (pendingSuppressed <= 0) return;
        if (laneName == "Embedded") {
            DIAG_ACTION_FMT("CoverWall",
                "EmbeddedLoadFailSuppressed: count=%d windowTotal=%d windowMs=%d",
                pendingSuppressed,
                bucket.emitted + bucket.suppressed,
                (int)((now - bucket.windowStart) * 1000.0));
        } else {
            DIAG_ACTION_FMT("CoverWall",
                "FolderLoadFailSuppressed: count=%d windowTotal=%d windowMs=%d",
                pendingSuppressed,
                bucket.emitted + bucket.suppressed,
                (int)((now - bucket.windowStart) * 1000.0));
        }
    };

    auto& bucket = s_failureBuckets[laneName];
    if (bucket.windowStart <= 0.0 || (now - bucket.windowStart) >= kFailureBucketWindowSec) {
        const int pendingSuppressed = bucket.suppressed - bucket.reportedSuppressed;
        emitSuppressedSummary(bucket, pendingSuppressed);
        bucket = FailureBucketState{};
        bucket.windowStart = now;
    }

    if (bucket.emitted < kMaxDetailedPerBucketWindow) {
        bucket.emitted++;
        return true;
    }

    bucket.suppressed++;
    const int pendingSuppressed = bucket.suppressed - bucket.reportedSuppressed;
    if (pendingSuppressed >= kSuppressedSummaryStep) {
        emitSuppressedSummary(bucket, pendingSuppressed);
        bucket.reportedSuppressed = bucket.suppressed;
    }
    return false;
#else
    (void)lane;
    return true;
#endif
}

bool GetFolderCoverTexture(const std::string& folderPathUtf8, TextureHandle& outTex, bool retryIfFailed /*= false*/) {
    outTex = TextureHandle();
    if (IsWaveformForegroundRebuildActive()) return false;
    TextureEntry e;
    bool ok = EnsureFolderCoverTextureLoaded(folderPathUtf8, e, retryIfFailed);
    outTex = ok ? ToHandle(e) : TextureHandle();
    const bool hasTexture = ok && outTex.hasTexture;
    if (hasTexture || ShouldEmitCurrentFolderCoverFailureDiagnostic(folderPathUtf8, retryIfFailed)) {
        LogCoverLaneThrottled("current", "folder_cover", folderPathUtf8, hasTexture ? 1 : 0, retryIfFailed ? 1 : 0);
    }
    return ok && outTex.hasTexture;
}

bool GetEmbeddedCoverTexture(const std::string& embeddedKey,
                             const std::string& representativeFilePathUtf8,
                             TextureHandle& outTex,
                             int lodLevel /*= 1*/,
                             bool retryIfFailed /*= false*/) {
    outTex = TextureHandle();
    if (IsWaveformForegroundRebuildActive()) return false;
    TextureEntry e;
    bool ok = EnsureEmbeddedCoverTextureLoaded(embeddedKey, representativeFilePathUtf8, e, lodLevel, retryIfFailed);
    outTex = ok ? ToHandle(e) : TextureHandle();
    LogCoverLaneThrottled("current", "embedded", embeddedKey, (ok && outTex.hasTexture) ? 1 : 0, retryIfFailed ? 1 : 0, lodLevel);
    return ok && outTex.hasTexture;
}

bool GetCoverTextureFromImagePath(const std::string& imageFilePathUtf8, TextureHandle& outTex, bool retryIfFailed /*= false*/) {
    outTex = TextureHandle();
    if (IsWaveformForegroundRebuildActive()) return false;
    if (imageFilePathUtf8.empty()) return false;

    // 使用路径本身作为稳定 key（"F|P|path"），以触发 EnsureFolderCoverTextureLoaded 的
    // key-based 分支。async worker 看到 task.path 含图片扩展名，直接读文件，
    // 不会调用 FindFolderCoverImagePathFast，从而绕过父目录递归搜索。
    std::string key = std::string("F|P|") + imageFilePathUtf8;
    TextureEntry e;
    // hintFolderPath="", hintSrcPath=imageFilePathUtf8 → async worker pathForLookup = imageFilePath
    bool ok = EnsureFolderCoverTextureLoaded(key, e, retryIfFailed, "", imageFilePathUtf8);
    outTex = ok ? ToHandle(e) : TextureHandle();
    return ok && outTex.hasTexture;
}

void EvictCoverTextureByImagePath(const std::string& imageFilePathUtf8) {
    const std::string key = std::string("F|P|") + imageFilePathUtf8;
    std::lock_guard<std::mutex> cacheLk(g_metaCacheMutex);
    g_folderCoverKeyCache.erase(key);
    g_asyncFailedKeys.erase(key);
}

// Read-only lookup for already materialized cover identities.
bool TryGetCachedCoverTextureByKey(const CoverItem& item, TextureHandle& outTex) {
    outTex = TextureHandle();
    if (item.key.empty() && item.folderPathUtf8.empty()) return false;

    const uint64_t frame = (uint64_t)ImGui::GetFrameCount();
    std::lock_guard<std::mutex> cacheLk(g_metaCacheMutex);

    auto tryTouchFolderKey = [&](const std::string& cacheKey) -> bool {
        auto it = g_folderCoverKeyCache.find(cacheKey);
        if (it == g_folderCoverKeyCache.end() || !it->second.hasTexture) {
            return false;
        }
        it->second.lastUsedFrame = frame;
        if (s_memoryBudget) s_memoryBudget->updateUsage(MemoryBudget::CacheType::Cover, cacheKey);
        outTex = ToHandle(it->second);
        return outTex.hasTexture;
    };

    if (item.kind == CoverKind::EmbeddedCover) {
        if (item.key.empty()) return false;
        auto it = g_embeddedCoverCache.find(item.key);
        if (it == g_embeddedCoverCache.end() || !it->second.hasTexture) {
            return false;
        }
        it->second.lastUsedFrame = frame;
        if (s_memoryBudget) s_memoryBudget->updateUsage(MemoryBudget::CacheType::Cover, item.key);
        outTex = ToHandle(it->second);
        return outTex.hasTexture;
    }

    if (!item.key.empty() && tryTouchFolderKey(item.key)) {
        return true;
    }

    if (!item.folderPathUtf8.empty()) {
        auto keyIt = g_folderCoverIdentityKeyCache.find(item.folderPathUtf8);
        if (keyIt != g_folderCoverIdentityKeyCache.end() &&
            tryTouchFolderKey(keyIt->second)) {
            return true;
        }
    }

    return false;
}

bool GetCoverTextureByKey(const CoverItem& item, TextureHandle& outTex, bool forceRetry) {
    outTex = TextureHandle();
    if (IsWaveformForegroundRebuildActive()) return false;
    if (item.key.empty()) return false;

    uint64_t frame = (uint64_t)ImGui::GetFrameCount();
    if (item.kind == CoverKind::FolderCover) {
        auto it = g_folderCoverKeyCache.find(item.key);
        if (it != g_folderCoverKeyCache.end() && it->second.hasTexture) {
            it->second.lastUsedFrame = frame;
            if (s_memoryBudget) s_memoryBudget->updateUsage(MemoryBudget::CacheType::Cover, item.key);
            outTex = ToHandle(it->second);
            LogCoverLaneThrottled("wall", "folder_cache", item.key, 1, forceRetry ? 1 : 0);
            return outTex.hasTexture;
        }
        // Temporary folder keys fall back to path-based loading.
        if (item.key.rfind("?|", 0) == 0) {
            bool ok = GetFolderCoverTexture(item.folderPathUtf8, outTex, forceRetry);
            if (!ok)
                DIAG_ACTION_FMT("CoverWall", "FolderTempLoadFail: path=%.64s", item.folderPathUtf8.c_str());
            LogCoverLaneThrottled("wall", "folder_temp", item.folderPathUtf8, ok ? 1 : 0, forceRetry ? 1 : 0);
            return ok;
        }
        // Prefer the identity key; use paths only as source hints.
        std::string requestArg = (item.key.rfind("F|", 0) == 0) ? item.key : item.folderPathUtf8;
        TextureEntry e;
        bool ok = EnsureFolderCoverTextureLoaded(requestArg, e, forceRetry, item.folderPathUtf8, item.folderCoverSrcPathUtf8);
        outTex = ok ? ToHandle(e) : TextureHandle();
        LogCoverLaneThrottled("wall", "folder_key", item.key, (ok && outTex.hasTexture) ? 1 : 0, forceRetry ? 1 : 0);
        if (!ok) {
#if AURA_BUILD_DIAGNOSTICS
            static std::unordered_map<std::string, double> s_folderFailLogLast;
            static const size_t kMaxFailLogKeys = 2000;
            double t = ImGui::GetTime();
            auto it = s_folderFailLogLast.find(item.key);
            if ((it == s_folderFailLogLast.end() || (t - it->second) >= 2.0) &&
                ShouldEmitCoverWallLoadFailureDiagnostic("Folder")) {
                if (s_folderFailLogLast.size() >= kMaxFailLogKeys) s_folderFailLogLast.clear();
                s_folderFailLogLast[item.key] = t;
                DIAG_ACTION_FMT("CoverWall", "FolderLoadFail: key=%.64s", item.key.c_str());
            }
#endif
        }
        return ok && outTex.hasTexture;
    }

    auto it = g_embeddedCoverCache.find(item.key);
    if (it != g_embeddedCoverCache.end() && it->second.hasTexture) {
        it->second.lastUsedFrame = frame;
        if (s_memoryBudget) s_memoryBudget->updateUsage(MemoryBudget::CacheType::Cover, item.key);
        outTex = ToHandle(it->second);
        LogCoverLaneThrottled("wall", "embedded_cache", item.key, 1, forceRetry ? 1 : 0);
        return outTex.hasTexture;
    }
    // 调用带重试标志的加载函数，且强制使用 LOD2 (512px)
    bool ok = GetEmbeddedCoverTexture(item.key, item.filePathUtf8, outTex, 2, forceRetry);
    LogCoverLaneThrottled("wall", "embedded", item.key, ok ? 1 : 0, forceRetry ? 1 : 0);
    if (!ok) {
#if AURA_BUILD_DIAGNOSTICS
        static std::unordered_map<std::string, double> s_embeddedFailLogLast;
        static const size_t kMaxEmbeddedFailLogKeys = 2000;
        double t = ImGui::GetTime();
        auto itEmb = s_embeddedFailLogLast.find(item.key);
            if ((itEmb == s_embeddedFailLogLast.end() || (t - itEmb->second) >= 2.0) &&
                ShouldEmitCoverWallLoadFailureDiagnostic("Embedded")) {
                if (s_embeddedFailLogLast.size() >= kMaxEmbeddedFailLogKeys) s_embeddedFailLogLast.clear();
                s_embeddedFailLogLast[item.key] = t;
                DIAG_ACTION_FMT("CoverWall", "EmbeddedLoadFail: key=%.64s", item.key.c_str());
        }
#endif
    }
    return ok;
}

bool IsCoverTextureHandleCurrent(const CoverItem& item, uint32_t textureId) {
    if (item.key.empty() || textureId == 0) return false;

    std::lock_guard<std::mutex> cacheLk(g_metaCacheMutex);
    if (item.kind == CoverKind::EmbeddedCover) {
        auto it = g_embeddedCoverCache.find(item.key);
        return it != g_embeddedCoverCache.end() &&
               it->second.hasTexture &&
               it->second.textureId == textureId;
    }

    auto it = g_folderCoverKeyCache.find(item.key);
    if (it != g_folderCoverKeyCache.end() &&
        it->second.hasTexture &&
        it->second.textureId == textureId) {
        return true;
    }

    if (item.key.rfind("?|", 0) == 0 && !item.folderPathUtf8.empty()) {
        auto keyIt = g_folderCoverIdentityKeyCache.find(item.folderPathUtf8);
        if (keyIt != g_folderCoverIdentityKeyCache.end()) {
            auto resolvedIt = g_folderCoverKeyCache.find(keyIt->second);
            return resolvedIt != g_folderCoverKeyCache.end() &&
                   resolvedIt->second.hasTexture &&
                   resolvedIt->second.textureId == textureId;
        }
    }

    return false;
}

bool GetFolderCoverImagePath(const std::string& folderPathUtf8, std::string& outCoverPathUtf8, bool skipNegativeCache) {
    if (folderPathUtf8.empty()) return false;

    // 0. 外部库优先（只读）：若配置了路径则先查，命中则缓存并返回
    if (ExternalLibraryCover::tryGetCoverForFolder(folderPathUtf8, outCoverPathUtf8)) {
        std::lock_guard<std::mutex> lk(g_metaCacheMutex);
        g_folderCoverPathCache[folderPathUtf8] = outCoverPathUtf8;
        return true;
    }

    // 1. 内存一级防御
    {
        std::lock_guard<std::mutex> lk(g_metaCacheMutex);
        auto cacheIt = g_folderCoverPathCache.find(folderPathUtf8);
        if (cacheIt != g_folderCoverPathCache.end()) {
            if (cacheIt->second == kNoCoverMarker && !skipNegativeCache) return false;
            if (cacheIt->second != kNoCoverMarker) {
                try {
                    std::wstring wCached = Utf8ToWide(cacheIt->second);
                    fs::path p(wCached);
                    if (IsLikelyAdvertiserImageFileName(p.stem().wstring())) {
                        g_folderCoverPathCache[folderPathUtf8] = kNoCoverMarker;
                        return false;
                    }
                } catch (...) { /* 转换失败则继续用缓存 */ }
                outCoverPathUtf8 = cacheIt->second;
                return true;
            }
        }
    }

    // Disk probing is limited per UI frame; rebuild requests may bypass the
    // negative cache.
    static uint64_t s_lastIoFrame = 0;
    static int s_ioCountInFrame = 0;
    if (ImGui::GetFrameCount() != s_lastIoFrame) {
        s_lastIoFrame = ImGui::GetFrameCount();
        s_ioCountInFrame = 0;
    }
    
    // Defer ordinary lookups after five probes in one frame.
    if (!skipNegativeCache && s_ioCountInFrame > 5) return false;

    s_ioCountInFrame++;

    // Ordinary UI lookups are shallow and frame-limited. Explicit retry or
    // rebuild requests may enable the bounded parent-directory search.
    if (FindFolderCoverImagePathFast(folderPathUtf8, outCoverPathUtf8, skipNegativeCache)) {
        std::lock_guard<std::mutex> lk(g_metaCacheMutex);
        g_folderCoverPathCache[folderPathUtf8] = outCoverPathUtf8;
        return true;
    }
    {
        std::lock_guard<std::mutex> lk(g_metaCacheMutex);
        g_folderCoverPathCache[folderPathUtf8] = kNoCoverMarker;
    }
    return false;
}

bool GetFolderCoverImagePathBackground(const std::string& folderPathUtf8,
                                       std::string& outCoverPathUtf8,
                                       bool skipNegativeCache,
                                       bool useDeepSearch) {
    outCoverPathUtf8.clear();
    if (folderPathUtf8.empty()) return false;

    if (ExternalLibraryCover::tryGetCoverForFolder(folderPathUtf8, outCoverPathUtf8)) {
        std::lock_guard<std::mutex> lk(g_metaCacheMutex);
        g_folderCoverPathCache[folderPathUtf8] = outCoverPathUtf8;
        return true;
    }

    {
        std::lock_guard<std::mutex> lk(g_metaCacheMutex);
        auto cacheIt = g_folderCoverPathCache.find(folderPathUtf8);
        if (cacheIt != g_folderCoverPathCache.end()) {
            if (cacheIt->second == kNoCoverMarker && !skipNegativeCache) {
                return false;
            }

            if (cacheIt->second != kNoCoverMarker) {
                try {
                    std::wstring wCached = Utf8ToWide(cacheIt->second);
                    fs::path p(wCached);
                    if (IsLikelyAdvertiserImageFileName(p.stem().wstring())) {
                        g_folderCoverPathCache[folderPathUtf8] = kNoCoverMarker;
                        return false;
                    }
                } catch (...) {
                    // Fall through and keep using the cached path.
                }

                outCoverPathUtf8 = cacheIt->second;
                return true;
            }
        }
    }

    if (FindFolderCoverImagePathFast(folderPathUtf8, outCoverPathUtf8, useDeepSearch)) {
        std::lock_guard<std::mutex> lk(g_metaCacheMutex);
        g_folderCoverPathCache[folderPathUtf8] = outCoverPathUtf8;
        return true;
    }

    {
        std::lock_guard<std::mutex> lk(g_metaCacheMutex);
        g_folderCoverPathCache[folderPathUtf8] = kNoCoverMarker;
    }

    return false;
}

bool GetEmbeddedCoverTextureByPath(const std::string& audioFilePathUtf8, TextureHandle& outTex) {
    std::string key;
    if (!RequestEmbeddedCoverIdentity(audioFilePathUtf8, key) || key.empty()) return false;
    return GetEmbeddedCoverTexture(key, audioFilePathUtf8, outTex, 1);
}

void InvalidateCoverForPath(const std::string& filePathUtf8) {
    if (filePathUtf8.empty()) return;
    std::vector<std::string> paths(1, filePathUtf8);
    InvalidateFileCoverCaches(paths);
}

void InvalidateFileCoverCaches(const std::vector<std::string>& filePaths) {
    if (filePaths.empty()) return;
    std::unordered_set<std::string> affectedFolders;
    std::unordered_set<std::string> affectedFolderCoverKeys;
    std::unordered_set<std::string> affectedFolderCoverPaths;

    {
        std::lock_guard<std::mutex> lk(g_metaCacheMutex);
        for (const auto& fp : filePaths) {
            if (fp.empty()) continue;

            std::string oldEmbeddedKey;
            auto keyIt = g_fileEmbeddedKeyCache.find(fp);
            if (keyIt != g_fileEmbeddedKeyCache.end()) {
                oldEmbeddedKey = keyIt->second;
                if (keyIt->second != kNoCoverMarker) {
                    auto texIt = g_embeddedCoverCache.find(keyIt->second);
                    if (texIt != g_embeddedCoverCache.end()) {
                        DeleteTextureIfAny(texIt->second.textureId);
                        g_embeddedCoverCache.erase(texIt);
                    }
                }
                g_fileEmbeddedKeyCache.erase(keyIt);
            }
            g_fileEmbeddedKeyMtimeCache.erase(fp);
            g_identityRefresh.invalidate(fp);

            auto fkIt = g_fileFolderKeyCache.find(fp);
            if (fkIt != g_fileFolderKeyCache.end()) {
                if (!fkIt->second.empty() && fkIt->second != kNoCoverMarker) {
                    affectedFolderCoverKeys.insert(fkIt->second);
                }
                g_fileFolderKeyCache.erase(fkIt);
            }
            g_fileFolderCoverSrcMtimeCache.erase(fp);

            std::string folder = ParentFolderUtf8(fp);
            if (!folder.empty()) affectedFolders.insert(folder);

            std::lock_guard<std::mutex> asyncLk(g_asyncMutex);
            g_asyncFailedKeys.erase(fp);
            if (!oldEmbeddedKey.empty() && oldEmbeddedKey != kNoCoverMarker) {
                g_asyncFailedKeys.erase(oldEmbeddedKey);
            }
        }
    }

    {
        std::lock_guard<std::mutex> lk(g_metaCacheMutex);
        for (const auto& folder : affectedFolders) {
            auto pIt = g_folderCoverPathCache.find(folder);
            if (pIt != g_folderCoverPathCache.end()) {
                if (!pIt->second.empty() && pIt->second != kNoCoverMarker) {
                    affectedFolderCoverPaths.insert(pIt->second);
                }
                g_folderCoverPathCache.erase(pIt);
            }
        }
    }
    for (const auto& key : affectedFolderCoverKeys) {
        auto wk = g_folderCoverKeyCache.find(key);
        if (wk != g_folderCoverKeyCache.end()) {
            DeleteTextureIfAny(wk->second.textureId);
            g_folderCoverKeyCache.erase(wk);
        }
    }
    if (!g_current.cachedFolderPathUtf8.empty() &&
        affectedFolders.count(g_current.cachedFolderPathUtf8)) {
        DeleteTextureIfAny(g_current.tex.textureId);
        g_current.tex = TextureEntry();
        g_current.cachedFolderPathUtf8.clear();
    }
}

bool FileHasCoverKey(const std::string& audioFilePathUtf8, const std::string& coverKey) {
    if (audioFilePathUtf8.empty() || coverKey.empty()) return false;

    // 先信数据库缓存字段：这是点击封面筛选时真正的主数据源
    if (s_database) {
        AudioRecord r = s_database->getFileByPath(audioFilePathUtf8);
        if (r.id > 0) {
            if (!r.embeddedCoverKey.empty() && r.embeddedCoverKey == coverKey) return true;
            if (!r.folderCoverKey.empty() && r.folderCoverKey == coverKey) return true;

            // 只要数据库里已经有任一 cover key，但都不匹配，就直接 false。
            // 不再回退做昂贵且可能语义不一致的实时重算。
            if (!r.embeddedCoverKey.empty() || !r.folderCoverKey.empty()) {
                return false;
            }

            // 若数据库记录明确 suppress，也不要再回退碰磁盘。
            if (r.coverSuppressed) {
                return false;
            }
        }
    }

    // 数据库里完全没缓存时，才做保底回退
    if (StartsWith(coverKey, "E|")) {
        std::string k;
        if (!ComputeEmbeddedCoverKeyForFile(s_database, audioFilePathUtf8, k)) return false;
        return k == coverKey;
    }

    if (StartsWith(coverKey, "F|")) {
        std::string folderPathUtf8 = ParentFolderUtf8(audioFilePathUtf8);
        if (folderPathUtf8.empty()) return false;

        std::string coverPathUtf8;
        if (!GetFolderCoverImagePath(folderPathUtf8, coverPathUtf8, false) || coverPathUtf8.empty()) {
            return false;
        }

        std::string k = NormalizeCoverKeyByVisualIdentity(coverPathUtf8);
        return !k.empty() && k == coverKey;
    }

    return false;
}

void ClearFolderCoverCache() {
    std::lock_guard<std::mutex> lk(g_metaCacheMutex);
    s_folderCoverCacheList.clear();
    s_folderCoverCacheMap.clear();
    s_folderCoverDeepNegativeFolders.clear();
}

void ClearFolderCoverNegativeCache() {
    std::lock_guard<std::mutex> lk(g_metaCacheMutex);
    s_folderCoverDeepNegativeFolders.clear();
    for (auto it = g_folderCoverPathCache.begin(); it != g_folderCoverPathCache.end(); ) {
        if (it->second == kNoCoverMarker) {
            it = g_folderCoverPathCache.erase(it);
        } else {
            ++it;
        }
    }
}

// IsKeyFailedLocked moved to anonymous namespace (line ~840) for forward reference resolution

// 显示过滤：只检查是否在失败集合中（不做 TTL 过期），避免每帧抖动
// TTL 过期逻辑只在加载决策路径（IsKeyFailedLocked）使用
static bool IsKeyInFailedSetLocked(const std::string& key) {
    return g_asyncFailedKeys.count(key) > 0;
}

static bool IsKeyPendingOrFailedLocked(const std::string& key) {
    return !key.empty() &&
           (g_asyncPendingKeys.count(key) > 0 ||
            IsKeyInFailedSetLocked(key));
}

static bool IsCoverIdentityKeyPendingOrFailedLocked(const CoverItem& item) {
    if (item.key.empty()) return true;

    if (item.kind == CoverKind::EmbeddedCover) {
        return IsKeyPendingOrFailedLocked(item.key);
    }

    const bool hasStableFolderIdentity = item.key.rfind("F|", 0) == 0;
    if (hasStableFolderIdentity) {
        return IsKeyPendingOrFailedLocked(item.key);
    }

    const std::string& requestKey =
        item.folderPathUtf8.empty() ? item.key : item.folderPathUtf8;
    return IsKeyPendingOrFailedLocked(requestKey);
}

static bool IsCoverIdentityKeyFailedLocked(const CoverItem& item) {
    if (item.key.empty()) return true;

    if (item.kind == CoverKind::EmbeddedCover) {
        return IsKeyInFailedSetLocked(item.key);
    }

    const bool hasStableFolderIdentity = item.key.rfind("F|", 0) == 0;
    if (hasStableFolderIdentity) {
        return IsKeyInFailedSetLocked(item.key);
    }

    const std::string& requestKey =
        item.folderPathUtf8.empty() ? item.key : item.folderPathUtf8;
    return IsKeyInFailedSetLocked(requestKey);
}

bool IsCoverKeyFailed(const CoverItem& item) {
    if (item.key.empty()) return true;

    std::lock_guard<std::mutex> lk(g_asyncMutex);
    if (item.kind == CoverKind::EmbeddedCover) {
        return IsKeyInFailedSetLocked(item.key);
    }
    return IsKeyInFailedSetLocked(item.key) ||
           (!item.folderPathUtf8.empty() && IsKeyInFailedSetLocked(item.folderPathUtf8)) ||
           (!item.folderCoverSrcPathUtf8.empty() && IsKeyInFailedSetLocked(item.folderCoverSrcPathUtf8));
}

bool IsCoverKeyPendingOrFailed(const CoverItem& item) {
    if (item.key.empty()) return true;

    std::lock_guard<std::mutex> lk(g_asyncMutex);
    if (item.kind == CoverKind::EmbeddedCover) {
        return g_asyncPendingKeys.count(item.key) > 0 ||
               IsKeyInFailedSetLocked(item.key);
    }
    return g_asyncPendingKeys.count(item.key) > 0 ||
           IsKeyInFailedSetLocked(item.key) ||
           (!item.folderPathUtf8.empty() &&
            (g_asyncPendingKeys.count(item.folderPathUtf8) > 0 ||
             IsKeyInFailedSetLocked(item.folderPathUtf8))) ||
           (!item.folderCoverSrcPathUtf8.empty() &&
            (g_asyncPendingKeys.count(item.folderCoverSrcPathUtf8) > 0 ||
             IsKeyInFailedSetLocked(item.folderCoverSrcPathUtf8)));
}

// 单次加锁 + 空集合快速路径（显示过滤不做 TTL，视觉稳定）
bool IsCoverIdentityKeyPendingOrFailed(const CoverItem& item) {
    std::lock_guard<std::mutex> lk(g_asyncMutex);
    return IsCoverIdentityKeyPendingOrFailedLocked(item);
}

bool HasFailedCoverIdentityWork() {
    std::lock_guard<std::mutex> lk(g_asyncMutex);
    return !g_asyncFailedKeys.empty();
}

bool HasPendingOrFailedCoverIdentityWork() {
    std::lock_guard<std::mutex> lk(g_asyncMutex);
    return !g_asyncPendingKeys.empty() || !g_asyncFailedKeys.empty();
}

CoverIdentityFilterStats CopyCoverItemsExcludingIdentityPendingOrFailed(
    const std::vector<CoverItem>& items,
    std::vector<CoverItem>& outItems) {
    outItems.clear();
    CoverIdentityFilterStats stats;
    if (items.empty()) return stats;

    std::lock_guard<std::mutex> lk(g_asyncMutex);
    if (g_asyncPendingKeys.empty() && g_asyncFailedKeys.empty()) {
        return stats;
    }

    outItems.reserve(items.size());
    for (const auto& item : items) {
        if (IsCoverIdentityKeyFailedLocked(item)) {
            ++stats.hiddenFailed;
            continue;
        }
        if (IsCoverIdentityKeyPendingOrFailedLocked(item)) {
            ++stats.hiddenPending;
            continue;
        }
        outItems.push_back(item);
    }
    return stats;
}

size_t CopyCoverItemsExcludingIdentityFailed(const std::vector<CoverItem>& items,
                                             std::vector<CoverItem>& outItems) {
    outItems.clear();
    if (items.empty()) return 0;

    std::lock_guard<std::mutex> lk(g_asyncMutex);
    if (g_asyncFailedKeys.empty()) {
        return 0;
    }

    outItems.reserve(items.size());
    size_t hidden = 0;
    for (const auto& item : items) {
        if (IsCoverIdentityKeyFailedLocked(item)) {
            ++hidden;
            continue;
        }
        outItems.push_back(item);
    }
    return hidden;
}

size_t RemoveFailedCoverItems(std::vector<CoverItem>& items) {
    // Failed cover loads are throttled by request cooldowns. The display list
    // is the user's browseable cover universe, so failures must not remove tiles.
    (void)items;
    return 0;
}

void ClearAllCoverCaches() {
    {
        std::lock_guard<std::mutex> lk(g_metaCacheMutex);
        g_folderCoverPathCache.clear();
        g_folderCoverIdentityKeyCache.clear();
        g_fileEmbeddedKeyCache.clear();
        g_fileEmbeddedKeyMtimeCache.clear();
        g_identityRefresh.clear();
        g_fileFolderKeyCache.clear();
        g_fileFolderCoverSrcMtimeCache.clear();
    }
    ClearFolderCoverCache();
    DeleteTextureIfAny(g_current.tex.textureId);
    g_current.tex = TextureEntry();
    g_current.cachedFolderPathUtf8.clear();
    for (auto& kv : g_folderCoverKeyCache) DeleteTextureIfAny(kv.second.textureId);
    g_folderCoverKeyCache.clear();

    for (auto& kv : g_embeddedCoverCache) DeleteTextureIfAny(kv.second.textureId);
    g_embeddedCoverCache.clear();

    {
        std::lock_guard<std::mutex> lk(g_wallMutex);
        g_wallItems.clear();
        g_wallStatus = CoverWallStatus();
    }
    {
        std::lock_guard<std::mutex> lk(g_wallCacheMutex);
        g_wallCache.clear();
        g_wallCacheLru.clear();
    }
    {
        std::lock_guard<std::mutex> lk(g_asyncMutex);
        g_asyncGeneration.fetch_add(1, std::memory_order_relaxed);
        g_asyncPendingKeys.clear();
        g_asyncFailedKeys.clear();
        g_asyncQueue.clear();
        g_asyncCv.notify_all();
    }
    {
        std::lock_guard<std::mutex> lk(g_uploadMutex);
        g_uploadQueue.clear();
        g_uploadQueuedKeys.clear();
    }
    {
        std::lock_guard<std::mutex> lk(g_keyUpdatesMutex);
        g_keyUpdates.clear();
        g_keyUpdatesQueuedPaths.clear();
    }
}

void CancelPendingCoverWork() {
    g_jobCancel.store(true, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lk(g_wallMutex);
        g_wallItems.clear();
        g_wallStatus = CoverWallStatus();
        g_wallRevision.fetch_add(1, std::memory_order_relaxed);
    }
    {
        std::lock_guard<std::mutex> lk(g_asyncMutex);
        g_asyncGeneration.fetch_add(1, std::memory_order_relaxed);
        g_asyncPendingKeys.clear();
        g_asyncQueue.clear();
        g_asyncCv.notify_all();
    }
    {
        std::lock_guard<std::mutex> lk(g_uploadMutex);
        g_uploadQueue.clear();
        g_uploadQueuedKeys.clear();
    }
}

void PrimeFileCoverKeysFromDb(const std::string& filePathUtf8,
                              const std::string& embeddedKey,
                              int64_t embeddedKeyMtime,
                              const std::string& folderKey,
                              const std::string& folderSrcPathUtf8,
                              int64_t folderSrcMtime) {
    if (filePathUtf8.empty()) return;
    std::lock_guard<std::mutex> cacheLock(g_metaCacheMutex);

    // T2.7: embeddedKey (E|...) 与 folderKey (F|...) 都过一次 CanonicalCoverKey,
    // 防止老库的 F|<MixedCase> / 残缺 F|H| 直接污染内存缓存.
    // 失败 (返回空) 视为该 key 无效, 不写入缓存, 等待运行时重新计算.

    if (!embeddedKey.empty()) {
        std::string canonEmbedded = CanonicalCoverKey(embeddedKey);
        if (!canonEmbedded.empty()) {
            g_fileEmbeddedKeyCache[filePathUtf8] = canonEmbedded;
            if (embeddedKeyMtime > 0) {
                g_fileEmbeddedKeyMtimeCache[filePathUtf8] = embeddedKeyMtime;
            }
        }
    }
    // embeddedKey 为空时不设 kNoCoverMarker，让运行时按最新逻辑重新检测

    const std::string folderPathUtf8 = ParentFolderUtf8(filePathUtf8);
    if (folderPathUtf8.empty()) return;

    // 1. identity key：只信 DB 的 folderKey，不再从 srcPath 反拼/反推
    if (!folderKey.empty()) {
        std::string canonFolderKey = CanonicalCoverKey(folderKey);
        if (!canonFolderKey.empty()) {
            g_fileFolderKeyCache[filePathUtf8] = canonFolderKey;
            g_folderCoverIdentityKeyCache[folderPathUtf8] = canonFolderKey;
        }
    }

    // 2. 真实路径：只信 DB 的 folderSrcPathUtf8
    if (!folderSrcPathUtf8.empty()) {
        g_folderCoverPathCache[folderPathUtf8] = folderSrcPathUtf8;
    }

    // 3. 源路径 mtime
    if (folderSrcMtime > 0) {
        g_fileFolderCoverSrcMtimeCache[filePathUtf8] = folderSrcMtime;
    }

    // 4. 兼容旧库：极端情况下只有 srcPath 没有 folderKey，
    //    暂时补一个旧格式 key，保证老数据在全量重建前还能继续工作。
    //    这只是过渡兼容，下一步全量重建后会被真正的 V2 key 覆盖。
    // T2.7: legacy "F|<srcPath>" 拼接也过 CanonicalCoverKey, 自动升级为
    //       F|PATH|<canonical_lower>, 与 backfillNormalizedKey 对齐.
    if (folderKey.empty() && !folderSrcPathUtf8.empty()) {
        std::string legacyKey = CanonicalCoverKey(std::string("F|") + folderSrcPathUtf8);
        if (!legacyKey.empty()) {
            g_fileFolderKeyCache[filePathUtf8] = legacyKey;
            g_folderCoverIdentityKeyCache[folderPathUtf8] = legacyKey;
        }
    }
}

size_t DrainCoverKeyCacheUpdates(std::vector<CoverKeyCacheUpdate>& outUpdates, size_t maxItems) {
    outUpdates.clear();
    if (maxItems == 0) return 0;
    std::lock_guard<std::mutex> lk(g_keyUpdatesMutex);
    size_t n = 0;
    while (!g_keyUpdates.empty() && n < maxItems) {
        CoverKeyCacheUpdate u = std::move(g_keyUpdates.front());
        g_keyUpdates.pop_front();
        g_keyUpdatesQueuedPaths.erase(u.filePathUtf8);
        outUpdates.push_back(std::move(u));
        n++;
    }
    return n;
}

void RequestCoverWallPrecache(const std::vector<std::string>& filePathsUtf8, uint64_t signature) {
    if (!EnsureJobThreadStarted()) return;

    {
        std::vector<CoverItem> cached;
        if (WallCacheGet(signature, cached)) {
            CoverWallStatus st;
            st.signature = signature;
            st.totalFiles = (int)filePathsUtf8.size();
            st.processedFiles = (int)filePathsUtf8.size();
            st.uniqueCovers = (int)cached.size();
            st.done = true;
            SetWallSnapshotLocked(std::move(cached), st);
            return;
        }
    }

    {
        std::lock_guard<std::mutex> lk(g_jobMutex);
        if (signature == g_jobSig && filePathsUtf8.size() == g_jobFiles.size()) {
        }
        g_jobSig = signature;
        g_jobFiles = filePathsUtf8;
        g_jobPending.store(true);
        g_jobCancel.store(true); 
    }
    {
        std::lock_guard<std::mutex> lk(g_uploadMutex);
        g_uploadQueue.clear();
        g_uploadQueuedKeys.clear();
    }
    g_jobCv.notify_one();
}

void GetCoverWallSnapshot(std::vector<CoverItem>& outItems, CoverWallStatus& outStatus) {
    std::lock_guard<std::mutex> lk(g_wallMutex);
    outItems = g_wallItems;
    outStatus = g_wallStatus;
}

bool GetCoverWallSnapshotIfChanged(uint64_t* inoutRevision,
                                   std::vector<CoverItem>& outItems,
                                   CoverWallStatus& outStatus) {
    uint64_t cur = g_wallRevision.load(std::memory_order_relaxed);
    if (inoutRevision && *inoutRevision == cur) return false;
    GetCoverWallSnapshot(outItems, outStatus);
    if (inoutRevision) *inoutRevision = cur;
    return true;
}

bool GetMatchingFilesForCoverKey(uint64_t signature,
                                 const std::string& coverKey,
                                 std::vector<std::string>& outFilePaths,
                                 CoverWallStatus* outStatus) {
    outFilePaths.clear();
    if (coverKey.empty()) return false;

    if (outStatus) {
        CoverWallStatus st;
        {
            std::lock_guard<std::mutex> lk(g_wallMutex);
            st = g_wallStatus;
        }
        *outStatus = st;
    }

    std::lock_guard<std::mutex> lk(g_wallIndexMutex);
    if (signature != 0 && g_wallIndexSig != signature) return false;
    auto it = g_wallIndex.find(coverKey);
    if (it == g_wallIndex.end()) return true; 
    outFilePaths = it->second;
    return true;
}

void PumpCoverTextureUploads(int maxUploadsPerFrame, int budgetMicroseconds) {
    // 帧时间感知限流：每次 GL 上传后检查已耗时，超过预算提前停止，
    // 避免封面队列积压时一次性上传数百张导致帧尖峰。
    if (maxUploadsPerFrame <= 0) return;

    using clock = std::chrono::steady_clock;
    const auto t0 = clock::now();
    const bool hasBudget = (budgetMicroseconds > 0);
    int uploaded = 0;
    int skipped = 0;

    for (int i = 0; i < maxUploadsPerFrame; ++i) {
        PendingUpload u;
        {
            std::lock_guard<std::mutex> lk(g_uploadMutex);
            if (g_uploadQueue.empty()) break;
            u = std::move(g_uploadQueue.front());
            g_uploadQueue.pop_front();
        }

        // 无效项：直接清理队列 key 继续
        if (u.key.empty() || u.pixels.empty() || u.w <= 0 || u.h <= 0) {
            std::lock_guard<std::mutex> lk(g_uploadMutex);
            g_uploadQueuedKeys.erase(u.key);
            ++skipped;
            continue;
        }

        // 去重：如果纹理已在缓存中（其他路径已上传），跳过 GL 调用
        // 🔒 Hold g_metaCacheMutex for cache reads — background cover-wall
        // job may concurrently write identity/path caches.
        bool alreadyCached = false;
        {
            std::lock_guard<std::mutex> cacheLk(g_metaCacheMutex);
            if (u.kind == CoverKind::EmbeddedCover) {
                alreadyCached = (g_embeddedCoverCache.find(u.key) != g_embeddedCoverCache.end());
            } else {
                alreadyCached = (g_folderCoverKeyCache.find(u.key) != g_folderCoverKeyCache.end());
            }
        }
        if (alreadyCached) {
            std::lock_guard<std::mutex> lk(g_uploadMutex);
            g_uploadQueuedKeys.erase(u.key);
            ++skipped;
            continue;
        }

        // 真正的 GL 上传（这是昂贵的部分 — 不持锁）
        TextureEntry e = UploadTextureFromPixels(u.pixels, u.w, u.h);
        if (e.hasTexture) {
            size_t sizeBytes = (size_t)e.width * e.height * 4;
            if (s_memoryBudget) {
                s_memoryBudget->registerEntry(MemoryBudget::CacheType::Cover, u.key, sizeBytes);
            }
            // 🔒 Hold g_metaCacheMutex for cache writes + trim (may erase entries).
            std::lock_guard<std::mutex> cacheLk(g_metaCacheMutex);
            if (u.kind == CoverKind::EmbeddedCover) {
                g_embeddedCoverCache[u.key] = e;
                TrimEmbeddedCacheIfNeeded();
            } else {
                g_folderCoverKeyCache[u.key] = e;
                if (!u.folderPathUtf8.empty()) {
                    g_folderCoverIdentityKeyCache[u.folderPathUtf8] = u.key;
                    g_folderCoverPathCache[u.folderPathUtf8] = u.filePathUtf8;
                }
                TrimFolderCoverKeyCacheIfNeeded_Impl();
            }
        }

        {
            std::lock_guard<std::mutex> lk(g_uploadMutex);
            g_uploadQueuedKeys.erase(u.key);
        }
        ++uploaded;

        // 帧时间预算检查：超过则提前中止，剩余项下一帧继续
        if (hasBudget) {
            auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(clock::now() - t0).count();
            if (elapsed >= budgetMicroseconds) break;
        }
    }

    double elapsedMs = std::chrono::duration_cast<std::chrono::microseconds>(clock::now() - t0).count() / 1000.0;
    DevTools_LogCoverUploadMs(elapsedMs);
    if (uploaded > 0) {
        DIAG_METRIC("Cover", "TextureUploaded", (double)uploaded);
    }

    // Check cache watermarks even when no upload succeeds so all-failure
    // workloads still evict stale entries.
    static uint64_t s_lastTrimFrame = 0;
    if (ImGui::GetFrameCount() - s_lastTrimFrame > 600) { // 每隔约10秒
        TrimStringCachesIfNeeded();
        TrimEmbeddedCacheIfNeeded();
        TrimFolderCoverKeyCacheIfNeeded_Impl();
        s_lastTrimFrame = ImGui::GetFrameCount();
#if AURA_BUILD_DIAGNOSTICS
        DIAG_METRIC("Cover", "EmbeddedCacheSize", (double)g_embeddedCoverCache.size());
        DIAG_METRIC("Cover", "FolderKeyCacheSize", (double)g_folderCoverKeyCache.size());
        {
            std::lock_guard<std::mutex> lk(g_asyncMutex);
            DIAG_METRIC("Cover", "FailedKeysCount", (double)g_asyncFailedKeys.size());
            DIAG_METRIC("Cover", "PendingKeysCount", (double)g_asyncPendingKeys.size());
        }
        {
            std::lock_guard<std::mutex> lk(g_uploadMutex);
            DIAG_METRIC("Cover", "UploadQueueSize", (double)g_uploadQueue.size());
            DIAG_METRIC("Cover", "UploadQueuedKeysSize", (double)g_uploadQueuedKeys.size());
        }
#endif
    }
}

bool HasPendingCoverWork() {
    {
        std::lock_guard<std::mutex> lk(g_asyncMutex);
        if (!g_asyncPendingKeys.empty()) return true;
        if (!g_asyncQueue.empty()) return true;
    }
    {
        std::lock_guard<std::mutex> lk(g_uploadMutex);
        if (!g_uploadQueue.empty()) return true;
    }
    return false;
}

CoverWorkQueueStats GetPendingCoverWorkStats() {
    CoverWorkQueueStats stats;
    {
        std::lock_guard<std::mutex> lk(g_asyncMutex);
        stats.pendingKeys = (int)g_asyncPendingKeys.size();
        stats.asyncQueue = (int)g_asyncQueue.size();
        stats.failedKeys = (int)g_asyncFailedKeys.size();
    }
    {
        std::lock_guard<std::mutex> lk(g_uploadMutex);
        stats.uploadQueue = (int)g_uploadQueue.size();
        stats.uploadQueuedKeys = (int)g_uploadQueuedKeys.size();
    }
    return stats;
}

int GetPendingCoverWorkCount() {
    const CoverWorkQueueStats stats = GetPendingCoverWorkStats();
    return stats.pendingKeys + stats.asyncQueue + stats.uploadQueue;
}

void ShutdownCoverWallPrecache() {
    g_jobExit.store(true);
    g_jobCancel.store(true);
    g_jobPending.store(true);
    g_jobCv.notify_one();
    
    {
        std::lock_guard<std::mutex> lk(g_asyncMutex);
        g_asyncThreadRunning = false;
        g_asyncCv.notify_all();
    }
    
    try {
        if (g_jobThread.joinable()) {
            g_jobThread.join();
        }
        // Join every decoder worker before clearing the pool.
        for (auto& t : g_asyncThreads) {
            if (t.joinable()) t.join();
        }
        g_asyncThreads.clear();
    } catch (...) { }
}

static std::string ToLowerAsciiCopy(std::string s) {
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    }
    return s;
}

static std::string CoverItemStablePath(const CoverItem& it) {
    if (!it.folderPathUtf8.empty()) return it.folderPathUtf8;
    if (!it.filePathUtf8.empty()) return it.filePathUtf8;
    return "";
}

static void SortCoverItemsForWall(std::vector<CoverItem>& items) {
    std::stable_sort(items.begin(), items.end(),
        [](const CoverItem& a, const CoverItem& b) {
            // 文件夹封面在前，嵌入封面在后
            if (a.kind != b.kind) {
                return (int)a.kind < (int)b.kind;
            }

            // 先按 label 规整排序
            const std::string la = ToLowerAsciiCopy(a.label);
            const std::string lb = ToLowerAsciiCopy(b.label);
            if (la != lb) return la < lb;

            // 同 label 时按 key，让同图更容易相邻
            if (a.key != b.key) return a.key < b.key;

            // 最后按路径稳定排序
            return CoverItemStablePath(a) < CoverItemStablePath(b);
        });
}

void CoverWall::Reset() {
    // T2.3a: cancel in-flight worker, then block briefly until it exits
    if (m_cancelled) m_cancelled->store(true);
    if (m_discoveryFuture.valid()) m_discoveryFuture.get();
    m_cancelled.reset();
    m_signature = 0;
    m_filePaths.clear();
    m_cursor = 0;
    m_done = true;
    m_items.clear();
    m_seenKeys.clear();
}

void CoverWall::SetSource(const std::vector<std::string>& filePathsUtf8, uint64_t signature) {
    if (signature == m_signature && filePathsUtf8.size() == m_filePaths.size()) {
        return;
    }
    // T2.3a: cancel previous in-flight worker (it checks cancelled flag, exits early)
    if (m_cancelled) m_cancelled->store(true);
    if (m_discoveryFuture.valid()) m_discoveryFuture.get();

    m_signature = signature;
    m_filePaths = filePathsUtf8;
    m_cursor = 0;
    m_done = m_filePaths.empty();
    m_items.clear();
    m_seenKeys.clear();

    if (m_done) {
        m_cancelled.reset();
        return;
    }

    // T2.3: launch disk I/O on worker thread; T2.3a: tag with generation token
    m_discoveryGen++;
    auto cancelled = std::make_shared<std::atomic<bool>>(false);
    m_cancelled = cancelled;
    uint64_t gen = m_discoveryGen;
    std::vector<std::string> paths = filePathsUtf8;

    m_discoveryFuture = std::async(std::launch::async,
        [gen, paths = std::move(paths), cancelled]() -> std::pair<uint64_t, std::vector<CoverItem>> {
            std::vector<CoverItem> items;
            std::vector<std::string> seenKeys;
            seenKeys.reserve(256);

            for (const auto& filePath : paths) {
                if (cancelled->load(std::memory_order_relaxed)) break;
                if (filePath.empty()) continue;

                std::string folder = ParentFolderUtf8(filePath);
                if (!folder.empty()) {
                    std::string coverPath;
                    if (FindFolderCoverImagePathFast(folder, coverPath, true) && !coverPath.empty()) {
                        std::string key = CoverModule::NormalizeCoverKeyByVisualIdentity(coverPath);
                        if (!SeenKey(seenKeys, key)) {
                            seenKeys.push_back(key);
                            CoverItem it;
                            it.kind = CoverKind::FolderCover;
                            it.key = key;
                            it.folderPathUtf8 = folder;
                            it.label = BaseNameFromPathUtf8(folder);
                            items.push_back(std::move(it));
                        }
                    }
                }

                if (cancelled->load(std::memory_order_relaxed)) break;

                AlbumArt art;
                if (MetadataReader::readAlbumArt(filePath, art) && art.hasData()) {
                    uint64_t h = (uint64_t)XXH3_64bits(art.data.data(), art.data.size());
                    std::string key = CanonicalCoverKey(std::string("E|") + Hex64(h) + "|" +
                        std::to_string(art.data.size()) + "|" + art.mimeType);
                    if (!SeenKey(seenKeys, key)) {
                        seenKeys.push_back(key);
                        CoverItem it;
                        it.kind = CoverKind::EmbeddedCover;
                        it.key = key;
                        it.filePathUtf8 = filePath;
                        it.label = BaseNameFromPathUtf8(filePath);
                        items.push_back(std::move(it));
                    }
                }
            }

            if (!cancelled->load(std::memory_order_relaxed)) {
                SortCoverItemsForWall(items);
            }
            return {gen, std::move(items)};
        });
}

void CoverWall::StepDiscover(int /*maxStepsPerFrame*/) {
    // Poll the discovery future; filesystem work stays on its worker thread.
    if (m_done) return;
    if (!m_discoveryFuture.valid()) { m_done = true; return; }
    if (m_discoveryFuture.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
        return; // still in flight
    }

    auto [gen, items] = m_discoveryFuture.get();
    // Ignore results from a superseded SetSource generation.
    if (gen == m_discoveryGen) {
        m_items = std::move(items);
        m_cursor = m_filePaths.size();
        m_done = true;
    } else {
        // A generation mismatch completes this superseded request.
        m_done = true;
    }
}

void ResetFailedCache() {
    std::lock_guard<std::mutex> lk(g_asyncMutex);
    g_asyncFailedKeys.clear(); // Allow failed keys to be scheduled again.
}

// -- dHash helpers (v2: box-average 9x8 difference hash) ----------------------

// Box-average downsample to 9x8 grayscale (72 values).
// Each target cell averages the corresponding rectangular region in the RGBA32 source.
static void DownsampleToGray9x8_Box(const uint8_t* pixels, int w, int h, float outGray[72]) {
    for (int row = 0; row < 8; ++row) {
        int srcY0 = row * h / 8;
        int srcY1 = (row + 1) * h / 8;
        if (srcY1 <= srcY0) srcY1 = srcY0 + 1;
        if (srcY1 > h) srcY1 = h;
        for (int col = 0; col < 9; ++col) {
            int srcX0 = col * w / 9;
            int srcX1 = (col + 1) * w / 9;
            if (srcX1 <= srcX0) srcX1 = srcX0 + 1;
            if (srcX1 > w) srcX1 = w;
            double sum = 0.0;
            int count = 0;
            for (int y = srcY0; y < srcY1; ++y) {
                for (int x = srcX0; x < srcX1; ++x) {
                    size_t idx = ((size_t)y * (size_t)w + (size_t)x) * 4;
                    sum += 0.299 * pixels[idx] + 0.587 * pixels[idx + 1] + 0.114 * pixels[idx + 2];
                    ++count;
                }
            }
            outGray[row * 9 + col] = (count > 0) ? (float)(sum / count) : 0.0f;
        }
    }
}

// 64-bit dHash from 9x8 grayscale: each row compares 8 adjacent pairs (left > right -> bit=1).
static uint64_t ComputeDHash64(const float gray9x8[72]) {
    uint64_t hash = 0;
    for (int row = 0; row < 8; ++row) {
        for (int col = 0; col < 8; ++col) {
            if (gray9x8[row * 9 + col] > gray9x8[row * 9 + col + 1])
                hash |= (1ULL << (row * 8 + col));
        }
    }
    return hash;
}

uint64_t ComputePerceptualHash(const uint8_t* imageData, size_t dataSize) {
    if (!imageData || dataSize == 0) return 0;
    std::vector<uint8_t> pixels;
    int w = 0, h = 0;
    bool decoded = DecodeImageFromMemory(imageData, dataSize, pixels, w, h) && w > 0 && h > 0;
    // Fallback: WIC rejects non-standard JPEGs (some DAW/tagger embedded art).
    // stb_image is more lenient and handles these fine.
    if (!decoded) {
        int channels = 0;
        unsigned char* stbPx = stbi_load_from_memory(imageData, (int)dataSize, &w, &h, &channels, 4);
        if (stbPx && w > 0 && h > 0) {
            pixels.assign(stbPx, stbPx + (size_t)w * (size_t)h * 4);
            stbi_image_free(stbPx);
            decoded = true;
        }
    }
    if (!decoded) return 0;
    float gray9x8[72];
    DownsampleToGray9x8_Box(pixels.data(), w, h, gray9x8);
    return ComputeDHash64(gray9x8);
}

} // namespace CoverModule
