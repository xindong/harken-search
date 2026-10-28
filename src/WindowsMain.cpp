// SoundManager - Windows平台模块
// Windows entry point, window management, tray icon, hotkeys, and fonts.
// Platform code extracted from main.cpp.

#include "WindowsMain.h"
#include "DefaultAudioPlayerAssociation.h"
#include "CoverBrowseSurfaceState.h"
#include "PathUtils.h"  // Canonical folder filtering.
#include "SpectrogramMath.h"
#include "Theme.h"
#include "ThemeInspector.h"
#include "ui/TransportVisualAudit.h"
#include "ui/EmptyStateVisualAudit.h"
#include "AnnotationBridge.h"
#include "BackgroundIoPolicy.h"
#include "BetaFeatureFlags.h"
#include "WelcomeGreeting.h"
#include "WaveformCachePolicy.h"
#include "platform/system.h"
#include "platform/filesystem.h"
#include "ed25519/tweetnacl.h" // crypto_hash (SHA-512) for exe integrity check

#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_opengl3.h"
#include "ImGuiMouseCaptureLoss.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#include <GL/gl.h>
#include <ole2.h>
#include "Localization.h"   // knife-6 W6-2: Lw for window titles
#define SUB_W_T L"Preparing your sound workspace..."
#include <intrin.h>  // 用于 __cpuid
#ifdef CopyFile
#undef CopyFile
#endif

#include "AppState.h"
#include "FileInfoPanel.h"
#include "Version.h"
#include "RelinkWorkflow.h"
#include "update/HarkenSearchUpdateComposition.h"
#include "update/HarkenSearchUpdateNoticeUi.h"
#include "BackfillWorkerPool.h"
#include "WorkerThreadPolicy.h"
#include "HarkenSearchWidgets.h"  // Focus tracking.
#include "HarkenSearchIcons.h"    // Lucide font merge.
#include "DockNativeCaptionBridge.h"
#include "ui/dock/DockCaptionWndProc.h"
#include "ViewportGlLifecycle.h"
#include "Utils.h"
#include "platform/harken_user_data_layout.h"
#include "UIRenderer.h"
#include "SimilarSoundsWindow.h"
#include "PlaybackHistoryStateAdapter.h"
#include "UiHostServices.h"
#include "platform/platform.h"
#include "DiagnosticFileLifecycle.h"
#include "ScanDatabaseOpenDiagnostics.h"

// Legacy compatibility identifier: Auro.AudioFile.
static constexpr const char* kLegacyAuroAudioFileAssociation = "Auro.AudioFile";

#include <stdexcept>
#include <cctype>
#include <string_view>
#include <fstream>
#include <iomanip>

#if AURA_BUILD_DIAGNOSTICS
// Forward-declare the diagnostic widget smoke window.
// Its implementation stays in DevToolsHarkenSearchSmoke.cpp.

namespace aura::ui::dev { void DrawHarkenSearchSmokeWindow(bool* open); }
namespace {
#endif
inline bool ShouldOpenHarkenSearchGalleryFromEnv() {
#if AURA_BUILD_DIAGNOSTICS && !AURA_INTERNAL_BETA
    char value[8] = {};
    return GetEnvironmentVariableA("AURO_OPEN_UI_GALLERY", value,
                                   static_cast<DWORD>(sizeof(value))) > 0 &&
           value[0] == '1';
#else
    return false;
#endif
}

#if AURA_BUILD_DIAGNOSTICS
// Per-frame: F9 toggles the smoke window; `DrawHarkenSearchSmokeWindow` is a no-op
// when `*open == false`. Safe to call after every NewFrame.
inline void TickHarkenSearchSmokeWindow() {
    static bool s_open = false;
    static bool s_envOpenApplied = false;
    if (!s_envOpenApplied) {
        s_envOpenApplied = true;
        if (ShouldOpenHarkenSearchGalleryFromEnv()) {
            s_open = true;
        }
    }
    if (ImGui::IsKeyPressed(ImGuiKey_F9, false)) s_open = !s_open;
    aura::ui::dev::DrawHarkenSearchSmokeWindow(&s_open);
}
} // namespace
#else
namespace { inline void TickHarkenSearchSmokeWindow() {} }
#endif
#include "CoverModule.h"
#include "AvatarCropper.h"
#include "AIEngine.h"
#include "MetadataReader.h"
#include "MetadataWriter.h"
#include "ModuleUI.h"
#include "MemoryBudget.h"
#include "AsyncWorker.h"
#include "TransferEngine.h"
#include "DevToolsModule.h"
#include "FrameBudgetDiagnostics.h"
#include "DiagnosticsLogger.h"
#include "UserActionLog.h"
#include "WaapiClient.h"
#include "WwiseStructureTemplate.h"
#include "WwiseTransferConfig.h"
#include "WwiseTransferExecute.h"
#include "WwiseTransferPanel.h"
#include "WwiseTransferPersist.h"
#include "WaveformPrewarmPolicy.h"
#include "platform/audio_data_source.h"
#include "platform/executable_path.h"
#include "platform/filesystem.h"
#include "platform/text_encoding.h"
#include "platform/qa_files.h"
#include "platform/system.h"
#include "platform/thread_id.h"
#include "../thirdparty/stb_image_full.h"  // stbi_load for beta splash

void FinalizeFolderSubtreeDbRemoval(AppState& state,
                                    const std::string& targetPath,
                                    int removed);

// Startup is split into two decisions so the beta can show login without
// regressing to the old WebView loading screen.
static constexpr bool kShowStartupLogin = HarkenSearch::BetaFeatureFlags::kShowStartupLogin;
static constexpr bool kUseBetaSplash = HarkenSearch::BetaFeatureFlags::kUseBetaSplashLoading;
static std::atomic<bool> s_mainUiReady{false};
static bool bootstrapHealthAcknowledged = false;

static void AcknowledgeBootstrapHealthAfterUiPresented() {
    if (bootstrapHealthAcknowledged) return;
    bootstrapHealthAcknowledged =
        aris::update::AcknowledgeHarkenSearchBootstrapHealthReady();
}

// Include miniaudio for individual file processing
#include "../thirdparty/miniaudio.h"

#include <string>
#include <vector>
#include <algorithm>
#include <filesystem>
#include <thread>
#include <mutex>
#include <atomic>
#include <fstream>
#include <cstdio>
#include <cstdarg>
#include <chrono>
#include <ctime>
#include <unordered_set>
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")
#include <random>
#include <unordered_map>
#include <map>
#include <deque>
#include <condition_variable>
#include <cmath>
#include <cstdlib>
#include <future>
#include <cctype>
#include <complex>
#include <iterator>
#include <functional>
#include <sstream>

#if defined(__has_include)
#if __has_include(<WebView2.h>)
#define SM_HAS_WEBVIEW2 1
#include <WebView2.h>
#include <wrl.h>

#else
#define SM_HAS_WEBVIEW2 0
#endif
#else
#define SM_HAS_WEBVIEW2 0
#endif

#ifndef WM_COPYGLOBALDATA
#define WM_COPYGLOBALDATA 0x0049
#endif

// Forward declaration for ImGui Win32 handler
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// Extern globals defined in main.cpp

extern std::string g_imguiIniPathUtf8;
extern std::string g_appSettingsPathUtf8;
std::string ResolveCanonicalHarkenSearchDatabasePathUtf8(const std::string& pathUtf8);

static void SyncDiagnosticFileEnabledFromSettings();

static bool ResolveStartupDatabaseSelectionForMount(
    const AppState& state,
    std::string& outDbPathUtf8,
    AppState::StartupDatabaseSetupReason& outReason) {
    outDbPathUtf8.clear();
    outReason = AppState::StartupDatabaseSetupReason::None;

    const std::string currentDbPathUtf8 =
        ResolveCanonicalHarkenSearchDatabasePathUtf8(state.currentDbPathUtf8);
    if (IsDatabaseLibrarySelectable(
            ClassifyDatabaseLibraryPath(currentDbPathUtf8))) {
        outDbPathUtf8 = currentDbPathUtf8;
        return true;
    }

    for (const auto& recentPathRaw : state.recentDbPathsUtf8) {
        const std::string recentDbPathUtf8 =
            ResolveCanonicalHarkenSearchDatabasePathUtf8(recentPathRaw);
        if (IsDatabaseLibrarySelectable(
                ClassifyDatabaseLibraryPath(recentDbPathUtf8))) {
            outDbPathUtf8 = recentDbPathUtf8;
            return true;
        }
    }

    const bool hasKnownDatabasePath =
        !currentDbPathUtf8.empty() || !state.recentDbPathsUtf8.empty();
    outReason = hasKnownDatabasePath
        ? AppState::StartupDatabaseSetupReason::MissingConfiguredDatabase
        : AppState::StartupDatabaseSetupReason::FirstRunNoDatabase;
    return false;
}

static std::string GetHarkenSearchRuntimeRootUtf8() {
    char* runtimeRoot = nullptr;
    size_t runtimeRootLen = 0;
    if (_dupenv_s(&runtimeRoot, &runtimeRootLen, "AURA_RUNTIME_ROOT") == 0 && runtimeRoot) {
        std::string value(runtimeRoot);
        std::free(runtimeRoot);
        if (!value.empty()) {
            try {
                Platform::FileSystem::CreateDirectories(value);
            } catch (...) {}
            return value;
        }
    }
    return GetExeDirUtf8();
}

static std::string GetHarkenSearchRuntimePathUtf8(const char* fileName) {
    return GetHarkenSearchRuntimeRootUtf8() + "\\" + fileName;
}

static std::string GetDevModeDefaultDbPathUtf8() {
    return BuildDatabasePackageMainPathUtf8(GetHarkenSearchRuntimeRootUtf8() + "\\database\\Aris_Dev.db");
}

static bool GetHarkenSearchEnvFlag(const char* name) {
    char* raw = nullptr;
    size_t len = 0;
    if (_dupenv_s(&raw, &len, name) != 0 || !raw) {
        return false;
    }
    std::string value(raw);
    std::free(raw);
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return (char)std::tolower(c);
    });
    return value == "1" || value == "true" || value == "yes" || value == "on";
}

static std::string GetHarkenSearchHarnessLaneKindUtf8() {
    char* raw = nullptr;
    size_t len = 0;
    if (_dupenv_s(&raw, &len, "AURA_HARNESS_LANE_KIND") != 0 || !raw) {
        return std::string();
    }
    std::string value(raw);
    std::free(raw);
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return (char)std::tolower(c);
    });
    return value;
}

static bool IsHarkenSearchHarnessMultiInstanceEnabled() {
    if (GetHarkenSearchEnvFlag("AURA_HARNESS_MULTI_INSTANCE")) {
        return true;
    }
    auto normalizePath = [](std::string value) {
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
            if (c == '/') return '\\';
            return (char)std::tolower(c);
        });
        return value;
    };
    return normalizePath(GetHarkenSearchRuntimeRootUtf8()) != normalizePath(GetExeDirUtf8());
}

static bool ShouldUseSingleInstanceBroker() {
    // AURO_* environment variables are accepted only as legacy compatibility
    // aliases; AURA_* remains the established automation protocol namespace.
    if (GetHarkenSearchEnvFlag("AURA_DISABLE_SINGLE_INSTANCE") ||
        GetHarkenSearchEnvFlag("AURO_DISABLE_SINGLE_INSTANCE")) {
        return false;
    }
    if (GetHarkenSearchEnvFlag("AURA_SINGLE_INSTANCE") || GetHarkenSearchEnvFlag("AURO_SINGLE_INSTANCE")) {
        return true;
    }
    // Product scans must be single-instance by default: two foreground scanners
    // sharing one SQLite package can starve the scan completion ledger. Harness
    // lanes keep explicit multi-instance support through AURA_RUNTIME_ROOT.
    return !IsHarkenSearchHarnessMultiInstanceEnabled();
}

static bool ShouldDisableHarnessBgThrottle() {
    return IsHarkenSearchHarnessMultiInstanceEnabled();
}

static bool ShouldUseHarnessImmediateFilterRefresh() {
    if (!IsHarkenSearchHarnessMultiInstanceEnabled()) {
        return false;
    }
    const std::string laneKind = GetHarkenSearchHarnessLaneKindUtf8();
    return laneKind.empty() || laneKind == "functional";
}

static std::string GetHarkenSearchInstanceKeyUtf8() {
    if (!IsHarkenSearchHarnessMultiInstanceEnabled()) {
        return std::string("default");
    }
    std::string runtimeRoot = GetHarkenSearchRuntimeRootUtf8();
    if (runtimeRoot.empty()) {
        return std::string("default");
    }
    std::transform(runtimeRoot.begin(), runtimeRoot.end(), runtimeRoot.begin(), [](unsigned char c) {
        if (c == '/') return '\\';
        return (char)std::tolower(c);
    });
    uint64_t hash = 1469598103934665603ULL;
    for (unsigned char c : runtimeRoot) {
        hash ^= (uint64_t)c;
        hash *= 1099511628211ULL;
    }
    char buffer[32];
    sprintf_s(buffer, "%016llx", (unsigned long long)hash);
    return std::string(buffer);
}

static std::wstring GetHarkenSearchWindowClassNameW() {
    // Retain the historical Win32 class identifier so an Aris process can
    // still broker files to an older running installation during migration.
    if (!IsHarkenSearchHarnessMultiInstanceEnabled()) {
        return L"AuroClass";
    }
    return L"AuroClass_" + Utf8ToWide(GetHarkenSearchInstanceKeyUtf8());
}

static std::wstring GetHarkenSearchMutexNameW() {
    // Same cross-version compatibility constraint as the window class above.
    if (!IsHarkenSearchHarnessMultiInstanceEnabled()) {
        return L"Global\\Auro_SingleInstance_Mutex";
    }
    return L"Global\\Auro_SingleInstance_Mutex_" + Utf8ToWide(GetHarkenSearchInstanceKeyUtf8());
}

// OpenGL VSync extension (defined in main.cpp)
typedef BOOL(WINAPI* PFNWGLSWAPINTERVALEXTPROC)(int interval);
typedef int(WINAPI* PFNWGLGETSWAPINTERVALEXTPROC)();
extern PFNWGLSWAPINTERVALEXTPROC wglSwapIntervalEXT;

// Startup entry transition (file scope for main.cpp extern / spatial interpolation)

bool startupEntryTransitionActive = false;
std::chrono::steady_clock::time_point startupEntryTransitionStart = std::chrono::steady_clock::now();
extern const float kStartupEntryTransitionMs = 760.0f;
static constexpr float kStartupTransitionTimeoutMs = 3000.0f;

// Cover prefetch (progressive, avoids empty covers on large libraries)

struct CoverPrefetchState {
    uint64_t sig = 0;
    size_t index = 0;
    size_t lastSize = 0;
};

static uint64_t HashString64(const std::string& s) {
    return std::hash<std::string>{}(s);
}

static uint64_t BuildCoverPrefetchSignature(const AppState& state) {
    uint64_t h = 1469598103934665603ULL;
    auto mix = [&](uint64_t v) {
        h ^= v + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
    };
    mix((uint64_t)state.viewMode);
    mix((uint64_t)state.selectedPlaylistId);
    mix(HashString64(state.selectedFolderPath));
    mix(HashString64(state.searchBuffer));
    mix(HashString64(state.artistFilterBuffer));
    mix(HashString64(state.albumFilterBuffer));
    mix(HashString64(state.categoryFilterBuffer));
    mix((uint64_t)state.coverFilterActive);
    mix(HashString64(state.coverFilterPath));
    mix((uint64_t)state.searchResults.size());
    return h;
}

static void PumpCoverPrefetch(AppState& state, const BackgroundIoContext& coverIo) {
    if (!state.settings.showCoverColumn) return;
    if (state.viewMode == ViewMode::Browser) return;
    if (state.searchResults.empty()) return;
    if (IsRapidUiClickBurstActive()) return;

    const auto coverBudget = ResolveCoverWallBackgroundBudget(coverIo);
    if (!coverBudget.allowBackgroundRequests || coverBudget.maxRequestsPerFrame <= 0) return;
    if (coverIo.coverWallPriorityMode) return;

    static CoverPrefetchState g_prefetch;
    uint64_t sig = BuildCoverPrefetchSignature(state);
    if (sig != g_prefetch.sig || state.searchResults.size() < g_prefetch.lastSize) {
        g_prefetch.sig = sig;
        g_prefetch.index = 0;
        g_prefetch.lastSize = state.searchResults.size();
    } else {
        g_prefetch.lastSize = state.searchResults.size();
    }

    // Keep the decode/upload queue bounded, but let cover-wall priority mode use
    // the explicit BackgroundIoPolicy budget instead of the legacy 3/20 trickle.
    const int pendingLimit = 20;
    if (CoverModule::GetPendingCoverWorkCount() > pendingLimit) return;

    int perFrame = std::min(coverBudget.maxRequestsPerFrame, 1);
    for (int n = 0; n < perFrame && !state.searchResults.empty(); ++n) {
        if (g_prefetch.index >= state.searchResults.size()) {
            g_prefetch.index = 0; // 循环预取，逐步补齐
        }
        const auto& r = state.searchResults[g_prefetch.index];
        if (!r.filePath.empty()) {
            CoverModule::TextureHandle tex;
            bool hasTex = false;
            if (!r.embeddedCoverKey.empty()) {
                hasTex = CoverModule::GetEmbeddedCoverTexture(r.embeddedCoverKey, r.filePath, tex);
            }
            if (!hasTex && r.embeddedCoverKey.empty()) {
                hasTex = CoverModule::GetEmbeddedCoverTextureByPath(r.filePath, tex);
            }
            if (!hasTex) {
                // 统一语义：
                // 列表预取一律走 folderPath。
                // folderCoverKey 只做身份，不再作为“路径型参数”传入。
                if (!r.folderPath.empty()) {
                    CoverModule::GetFolderCoverTexture(r.folderPath, tex);
                }
            }
        }
        g_prefetch.index++;
    }
}

// Cache metadata for played files (cover + main waveform detail)

struct PlayedMetaCacheState {
    int64_t lastFileId = -1;
    std::string lastFilePath;
    std::unordered_set<std::string> cachedCoverPaths;
    std::unordered_set<std::string> cachedDetailPaths;
};

static void PumpPlayedMetadataCache(AppState& state, bool isMouseIdle) {
    static PlayedMetaCacheState s;

    if (state.currentFileId <= 0) return;
    if (state.currentFileId != s.lastFileId) {
        s.lastFileId = state.currentFileId;
        AudioRecord rec = state.database.getFileById(state.currentFileId);
        s.lastFilePath = rec.filePath;
        if (!s.lastFilePath.empty()) {
            // If this file is played, request a higher-detail waveform (main view); delay 150ms
            ScheduleDeferredWaveformDetail(state, s.lastFilePath, AppState::WAVEFORM_DEFAULT_DETAIL_PEAKS);
        }
    }

    if (s.lastFilePath.empty()) return;

    // 1) Playback keeps main waveform detail in memory only. Persistent detail
    // cache is reserved for explicit maintenance/rebuild actions.
    if (s.cachedDetailPaths.find(s.lastFilePath) == s.cachedDetailPaths.end()) {
        auto wf = state.audioEngine.getWaveformData();
        if (wf &&
            wf->isReady &&
            !wf->isFallbackPlaceholder &&
            wf->filePath == s.lastFilePath &&
            (int)wf->peaks.size() >= AppState::WAVEFORM_DEFAULT_DETAIL_PEAKS) {
            DIAG_ACTION_FMT("Waveform",
                            "PlaybackDetailCacheSkipped: path=%s peaksSize=%d",
                            s.lastFilePath.c_str(), (int)wf->peaks.size());
            s.cachedDetailPaths.insert(s.lastFilePath);
        } else if (wf &&
                   wf->isReady &&
                   wf->isFallbackPlaceholder &&
                   wf->filePath == s.lastFilePath) {
            DIAG_ACTION_FMT("Waveform",
                            "SkipCachingFallbackDetail: path=%s, peaksSize=%d",
                            s.lastFilePath.c_str(), (int)wf->peaks.size());
        }
    }

    // 2) Cache cover key/texture for played file (idle-only to avoid stutter)
    if (isMouseIdle &&
        !IsCoverWallPriorityMode() &&
        !IsRapidUiClickBurstActive() &&
        s.cachedCoverPaths.find(s.lastFilePath) == s.cachedCoverPaths.end()) {
        CoverModule::TextureHandle tex;
        bool hasTex = CoverModule::GetEmbeddedCoverTextureByPath(s.lastFilePath, tex);
        if (!hasTex) {
            // Fallback to folder cover：只按 folderPath 请求，folderCoverKey 不做路径参数
            AudioRecord rec = state.database.getFileById(state.currentFileId);
            if (!rec.folderPath.empty()) {
                CoverModule::GetFolderCoverTexture(rec.folderPath, tex);
            }
        }
        s.cachedCoverPaths.insert(s.lastFilePath);
    }
}

// Idle prewarm for lightweight main waveform detail cache.
// This never upgrades an existing detail blob to full main resolution; exact
// 12000-peak detail remains a lazy on-selection/on-playback request.

struct MainDetailPrewarmResult {
    std::string path;
    std::vector<float> peaks;
    std::vector<float> minPeaks;
    std::vector<float> rms;
    int channels = 0;
    double durationSeconds = 0.0;
    bool ok = false;
    double elapsedMs = 0.0;
};

struct MainDetailPrewarmCandidate {
    std::string path;
    double durationSeconds = 0.0;
};

struct MainDetailPrewarmState {
    std::deque<MainDetailPrewarmCandidate> queue;
    std::unordered_set<std::string> queued;
    std::unordered_set<std::string> completed;
    std::future<MainDetailPrewarmResult> active;
    std::string activePath;
    uint64_t signature = 0;
};

static bool IsMainDetailPrewarmEnabled() {
    static int enabled = -1;
    if (enabled < 0) {
        char value[8] = {};
        const DWORD len = GetEnvironmentVariableA("AURA_ENABLE_MAIN_DETAIL_PREWARM", value, (DWORD)sizeof(value));
        // Product semantics: only the current played file may persist main
        // waveform detail by default. Neighbor-result AOT detail prewarm is an
        // explicit diagnostics/benchmark opt-in because it performs disk
        // reads and DB writes unrelated to the user's current sound.
        if (len > 0) {
            enabled = (value[0] == '1') ? 1 : 0;
        } else {
            enabled = 0;
            DIAG_SYSTEM("Waveform", "Main detail waveform prewarm disabled by default; current-file caching remains enabled.");
        }
    }
    return enabled == 1;
}

static void EnqueueMainDetailPrewarmCandidate(MainDetailPrewarmState& s,
                                              AppState& state,
                                              const std::string& path) {
    if (path.empty()) return;
    if (s.completed.count(path) > 0 || s.queued.count(path) > 0) return;
    if (path == state.audioEngine.getLoadedFilePath()) return;

    // Avoid decoding files that already have detail cached. This reads only
    // a bounded number of candidates per idle pump, which is cheaper than a full decode.
    std::vector<float> cached = state.database.getWaveformDetailByPath(path);
    if (HarkenSearch::WaveformPrewarm::HasUsableDetailCache(cached.size())) {
        s.completed.insert(path);
        return;
    }
    const AudioRecord rec = state.database.getFileByPath(path);
    const auto policy =
        WaveformCachePolicy::PersistentMainDetailForDuration(
            state.database.getWaveformCacheMode(),
            rec.durationSeconds);
    if (!policy.persistMainDetail) {
        s.completed.insert(path);
        return;
    }

    s.queued.insert(path);
    s.queue.push_back({path, rec.durationSeconds});
    constexpr size_t kMaxQueuedPrewarmDetails = 8;
    while (s.queue.size() > kMaxQueuedPrewarmDetails) {
        s.queued.erase(s.queue.front().path);
        s.queue.pop_front();
    }
}

static void PumpMainDetailPrewarm(AppState& state, bool isMouseIdle) {
    static MainDetailPrewarmState s;
    if (!IsMainDetailPrewarmEnabled()) return;

    if (s.active.valid() &&
        s.active.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
        MainDetailPrewarmResult r = s.active.get();
        s.activePath.clear();
        s.queued.erase(r.path);
        if (r.ok && !r.peaks.empty()) {
            if (WaveformCachePolicy::ShouldPersistGeneratedMainDetail(
                    state.database.getWaveformCacheMode(),
                    r.durationSeconds,
                    r.peaks.size(),
                    r.channels) &&
                state.database.saveWaveformDetailByPath(r.path, r.peaks, r.minPeaks, r.channels)) {
                s.completed.insert(r.path);
                DIAG_ACTION_FMT("Waveform",
                                "MainDetailPrewarmSaved: path=%s peaks=%d elapsedMs=%.2f",
                                r.path.c_str(), (int)r.peaks.size(), r.elapsedMs);
            }
        } else {
            DIAG_ACTION_FMT("Waveform",
                            "MainDetailPrewarmFailed: path=%s elapsedMs=%.2f",
                            r.path.c_str(), r.elapsedMs);
        }
    }

    if (state.waveformRebuildActive) {
        s.queue.clear();
        s.queued.clear();
        return;
    }
    if (!state.database.isOpen()) return;
    if (state.audioEngine.isLoadFilePending()) return;
    if (state.audioEngine.isWaveformDetailPending()) return;
    if (s.active.valid()) return;
    if (state.searchResults.empty()) return;
    (void)isMouseIdle;

    const uint64_t sig = BuildCoverPrefetchSignature(state);
    if (sig != s.signature) {
        s.signature = sig;
        s.queue.clear();
        s.queued.clear();
    }

    std::string loadedPath = state.audioEngine.getLoadedFilePath();
    int currentIndex = -1;
    if (!loadedPath.empty()) {
        for (int i = 0; i < (int)state.searchResults.size(); ++i) {
            if (state.searchResults[(size_t)i].filePath == loadedPath) {
                currentIndex = i;
                break;
            }
        }
    }

    int checked = 0;
    constexpr int kCandidateWindow = 10;
    constexpr int kMaxDbChecksPerPump = 3;
    if (currentIndex >= 0) {
        for (int offset = 1; offset <= kCandidateWindow && checked < kMaxDbChecksPerPump; ++offset) {
            const int idx = currentIndex + offset;
            if (idx >= (int)state.searchResults.size()) break;
            EnqueueMainDetailPrewarmCandidate(s, state, state.searchResults[(size_t)idx].filePath);
            ++checked;
        }
    } else {
        for (int i = 0; i < (int)state.searchResults.size() && checked < kMaxDbChecksPerPump; ++i) {
            EnqueueMainDetailPrewarmCandidate(s, state, state.searchResults[(size_t)i].filePath);
            ++checked;
        }
    }

    while (!s.queue.empty()) {
        MainDetailPrewarmCandidate candidate = std::move(s.queue.front());
        s.queue.pop_front();
        std::string path = candidate.path;
        if (path.empty() || s.completed.count(path) > 0) continue;
        const auto cacheMode = state.database.getWaveformCacheMode();

        s.activePath = path;
        s.active = std::async(std::launch::async, [&state, candidate, cacheMode]() {
#ifdef _WIN32
            Platform::SetThreadBackgroundMode(true);
#endif
            const auto started = std::chrono::steady_clock::now();
            const auto policy =
                WaveformCachePolicy::PersistentMainDetailForDuration(
                    cacheMode,
                    candidate.durationSeconds);
            WaveformData wf;
            const bool ok = policy.persistMainDetail &&
                state.audioEngine.buildWaveformDetailForCache(candidate.path, wf, policy.targetPeaks);
            const auto finished = std::chrono::steady_clock::now();
#ifdef _WIN32
            Platform::SetThreadBackgroundMode(false);
#endif
            MainDetailPrewarmResult r;
            r.path = candidate.path;
            r.ok = ok && wf.isReady && !wf.peaks.empty();
            r.durationSeconds = candidate.durationSeconds;
            if (r.ok) {
                r.peaks = std::move(wf.peaks);
                r.minPeaks = std::move(wf.minPeaks);
                r.rms = std::move(wf.rms);
                r.channels = wf.numChannels;
            }
            r.elapsedMs = std::chrono::duration<double, std::milli>(finished - started).count();
            return r;
        });
        DIAG_ACTION_FMT("Waveform", "MainDetailPrewarmStart: path=%s targetPeaks=dynamic",
                        path.c_str());
        break;
    }
}

// Low-priority decoder prewarm for the next likely audio selections.
// This does not change waveform quality or substitute a rough display. It only
// pays slow file/decoder initialization on a background thread before the user
// lands on the next row during rapid browse/play workflows.

struct AudioDecoderPrewarmResult {
    std::string path;
    bool ok = false;
    int code = 0;
    double elapsedMs = 0.0;
};

struct AudioDecoderPrewarmState {
    std::deque<std::string> queue;
    std::unordered_set<std::string> queued;
    std::unordered_set<std::string> completed;
    std::future<AudioDecoderPrewarmResult> active;
    std::string activePath;
    uint64_t signature = 0;
};

static AudioDecoderPrewarmState& GetAudioDecoderPrewarmState() {
    static AudioDecoderPrewarmState s;
    return s;
}

#ifdef _WIN32
static std::wstring ToWinLongPathForPrewarm(const std::wstring& path) {
    if (path.empty()) return path;
    if (path.rfind(L"\\\\?\\", 0) == 0) return path;
    if (path.rfind(L"\\\\", 0) == 0) {
        return L"\\\\?\\UNC\\" + path.substr(2);
    }
    if (path.size() >= 3 && path[1] == L':' && (path[2] == L'\\' || path[2] == L'/')) {
        return L"\\\\?\\" + path;
    }
    return path;
}
#endif

static void EnqueueAudioDecoderPrewarmCandidate(const std::string& path) {
    if (path.empty()) return;
    auto& s = GetAudioDecoderPrewarmState();
    if (s.completed.count(path) > 0 || s.queued.count(path) > 0 || s.activePath == path) return;

    s.queued.insert(path);
    s.queue.push_back(path);
    constexpr size_t kMaxQueuedDecoderPrewarms = 16;
    while (s.queue.size() > kMaxQueuedDecoderPrewarms) {
        s.queued.erase(s.queue.front());
        s.queue.pop_front();
    }
    if (s.completed.size() > 512) {
        s.completed.clear();
    }
}

static AudioDecoderPrewarmResult PrewarmAudioDecoder(const std::string& path) {
#ifdef _WIN32
    Platform::SetThreadBackgroundMode(true);
#endif
    const auto started = std::chrono::steady_clock::now();

    Platform::AudioDataSource::Decoder decoder;
    const auto initResult = Platform::AudioDataSource::InitializeDecoderFromFileWithFormat(
        decoder, path, 2, 0);
    const int result = initResult.backendCode;
    if (result == MA_SUCCESS) {
        Platform::AudioDataSource::UninitializeDecoder(decoder);
    }

    const auto finished = std::chrono::steady_clock::now();
#ifdef _WIN32
    Platform::SetThreadBackgroundMode(false);
#endif

    AudioDecoderPrewarmResult r;
    r.path = path;
    r.ok = (result == MA_SUCCESS);
    r.code = (int)result;
    r.elapsedMs = std::chrono::duration<double, std::milli>(finished - started).count();
    return r;
}

static void EnqueueAudioDecoderPrewarmWindow(AppState& state, int anchorIndex, int lookahead = 3) {
    if (state.searchResults.empty()) return;
    const int total = (int)state.searchResults.size();
    if (anchorIndex < 0 || anchorIndex >= total) return;

    lookahead = std::clamp(lookahead, 0, 3);
    for (int offset = 1; offset <= lookahead; ++offset) {
        const int idx = anchorIndex + offset;
        if (idx >= total) break;
        EnqueueAudioDecoderPrewarmCandidate(state.searchResults[(size_t)idx].filePath);
    }
}

static BackgroundIoContext BuildBackgroundIoContextForWindowsMain(AppState& state,
                                                                  bool contextMenuHeavyFreeze,
                                                                  bool coverWallPriorityMode,
                                                                  bool mouseIdle) {
    BackgroundIoContext ctx;
    const double now = ImGui::GetTime();
    ctx.contextMenuHeavyFreeze = contextMenuHeavyFreeze;
    ctx.coverWallPriorityMode = coverWallPriorityMode;
    ctx.playing = state.audioEngine.isPlaying();
    ctx.waveformRebuildActive = state.waveformRebuildActive;
    ctx.scanning = state.isScanning.load(std::memory_order_acquire);
    ctx.dbSwitchInProgress = state.dbSwitchInProgress;
    ctx.dbSwitchSettling = state.dbSwitchSettleFramesRemaining > 0;
    ctx.loadFilePending = state.audioEngine.isLoadFilePending();
    ctx.detailPending = state.audioEngine.isWaveformDetailPending();
    ctx.detailDeferring = !state.pendingDetailFilePath.empty();
    ctx.modeSwitchCoolingDown = state.lastProductModeSwitchTime > 0.0 &&
        (now - state.lastProductModeSwitchTime) < 2.5;
    ctx.mouseIdle = mouseIdle;
    ctx.userActive = ImGui::IsAnyMouseDown();
    ctx.hasBrowseAnchor = state.selectedResultIndex >= 0 || !state.audioEngine.getLoadedFilePath().empty();
    if (state.lastSelectionOrLoadTime > 0.0) {
        ctx.secondsSinceSelectionOrLoad = now - state.lastSelectionOrLoadTime;
    }
    return ctx;
}

static void PumpAudioDecoderPrewarm(AppState& state, const BackgroundIoContext& ioContext) {
    auto& s = GetAudioDecoderPrewarmState();

    if (s.active.valid() &&
        s.active.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
        AudioDecoderPrewarmResult r = s.active.get();
        s.activePath.clear();
        s.queued.erase(r.path);
        if (r.ok) {
            s.completed.insert(r.path);
        }
        DIAG_ACTION_FMT("Audio",
                        "DecoderPrewarmComplete: ok=%d code=%d elapsedMs=%.2f path=%s",
                        r.ok ? 1 : 0, r.code, r.elapsedMs, r.path.c_str());
    }

    const DecoderPrewarmDecision decision = ResolveDecoderPrewarmDecision(ioContext);
    if (!decision.allowed) {
        s.queue.clear();
        s.queued.clear();
#if AURA_BUILD_DIAGNOSTICS
        static double s_lastSkipLogAt = 0.0;
        const double now = ImGui::GetTime();
        if ((now - s_lastSkipLogAt) >= 1.0) {
            s_lastSkipLogAt = now;
            DIAG_ACTION_FMT("Audio", "DecoderPrewarmSkippedByForeground: reason=%s", decision.reason);
        }
#endif
        return;
    }
    if (!state.database.isOpen()) return;
    if (state.searchResults.empty()) return;
    if (state.audioEngine.isLoadFilePending()) return;
    if (s.active.valid()) return;

    const uint64_t sig = BuildCoverPrefetchSignature(state);
    if (sig != s.signature) {
        s.signature = sig;
        s.queue.clear();
        s.queued.clear();
    }

    int anchorIndex = state.selectedResultIndex;
    const std::string loadedPath = state.audioEngine.getLoadedFilePath();
    if (!loadedPath.empty()) {
        for (int i = 0; i < (int)state.searchResults.size(); ++i) {
            if (state.searchResults[(size_t)i].filePath == loadedPath) {
                anchorIndex = i;
                break;
            }
        }
    }
    EnqueueAudioDecoderPrewarmWindow(state, anchorIndex, decision.lookahead);

    while (!s.queue.empty()) {
        std::string path = s.queue.front();
        s.queue.pop_front();
        if (path.empty() || s.completed.count(path) > 0 || path == loadedPath) continue;

        s.activePath = path;
        s.active = std::async(std::launch::async, [path]() {
            return PrewarmAudioDecoder(path);
        });
        DIAG_ACTION_FMT("Audio", "DecoderPrewarmStart: path=%s", path.c_str());
        break;
    }
}

static bool WaitForScanWaveformBackfillCompletion(AppState& state,
                                                  Database& scanDb,
                                                  uint64_t scanGeneration,
                                                  const std::shared_ptr<std::atomic<bool>>& scanCancelToken,
                                                  int scanTotal,
                                                  const std::string& folderPathScope = {},
                                                  int64_t maxWaitMs = -1) {
    const int waveformKind = static_cast<int>(aura::BackfillJobKind::WaveformL1Overview);
    const bool scopedDrain = !folderPathScope.empty();
    int lastPendingRows = -1;
    int waveformCacheTotal = 0;
    const auto started = std::chrono::steady_clock::now();
    auto lastProgressAt = started;

    while (state.isScanTaskCurrent(scanGeneration) &&
           !state.isScanTaskCancelled(scanGeneration, scanCancelToken)) {
        const auto now = std::chrono::steady_clock::now();
        const auto snapshot = state.backfillQueue.snapshot();
        const int fixedCompletedPending = scanDb.fixCompletedPendingWaveformStates();
        if (fixedCompletedPending > 0) {
            DIAG_SYSTEM_FMT("Scan",
                            "WaveformCachePendingFixedDone: count=%d scanTotal=%d",
                            fixedCompletedPending,
                            scanTotal);
        }
        const int pendingRows = scopedDrain
            ? scanDb.countBackfillPendingKindInFolder(waveformKind, folderPathScope)
            : scanDb.countBackfillPendingKind(waveformKind);
        if (waveformCacheTotal <= 0) {
            waveformCacheTotal = std::max(1, pendingRows);
            if (state.isScanTaskCurrent(scanGeneration)) {
                state.scanPhase.store(AppState::ScanPhase::WaveformCache);
                state.scanWorkflowStage.store(
                    HarkenSearch::ScanWorkflowStage::CachingMiniWaveforms,
                    std::memory_order_release);
                state.scanProgress = 0;
                state.scanTotal = waveformCacheTotal;
            }
            DIAG_SYSTEM_FMT("Scan",
                            "WaveformCacheDrainScope: scoped=%d pending=%d scanTotal=%d cacheTotal=%d scope=%s",
                            scopedDrain ? 1 : 0,
                            pendingRows,
                            scanTotal,
                            waveformCacheTotal,
                            scopedDrain ? folderPathScope.c_str() : "<all>");
        } else if (pendingRows > waveformCacheTotal) {
            waveformCacheTotal = pendingRows;
        }
        if (pendingRows <= 0) {
            if (state.isScanTaskCurrent(scanGeneration)) {
                state.setScanMiniWaveformProgress(
                    std::max(0, waveformCacheTotal),
                    std::max(0, waveformCacheTotal));
            }
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - started).count();
            const double filesPerSec = elapsed > 0
                ? static_cast<double>(waveformCacheTotal) * 1000.0 / static_cast<double>(elapsed)
                : static_cast<double>(waveformCacheTotal);
            DIAG_SYSTEM_FMT("Scan",
                            "WaveformCacheDrainComplete: cacheTotal=%d scanTotal=%d elapsedMs=%lld filesPerSec=%.2f",
                            waveformCacheTotal,
                            scanTotal,
                            (long long)elapsed,
                            filesPerSec);
            return true;
        }
        if (maxWaitMs >= 0) {
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - started).count();
            if (elapsed >= maxWaitMs) {
                DIAG_SYSTEM_FMT("Scan",
                                "WaveformCacheDrainBudgetExceeded: scoped=%d pending=%d cacheTotal=%d scanTotal=%d waitMs=%lld scope=%s",
                                scopedDrain ? 1 : 0,
                                pendingRows,
                                waveformCacheTotal,
                                scanTotal,
                                (long long)elapsed,
                                scopedDrain ? folderPathScope.c_str() : "<all>");
                return false;
            }
        }
        if (pendingRows != lastPendingRows) {
            lastProgressAt = now;
            DIAG_SYSTEM_FMT("Scan",
                            "WaveformCacheDrainProgress: scoped=%d pending=%d cacheTotal=%d scanTotal=%d scope=%s",
                            scopedDrain ? 1 : 0,
                            pendingRows,
                            waveformCacheTotal,
                            scanTotal,
                            scopedDrain ? folderPathScope.c_str() : "<all>");
            lastPendingRows = pendingRows;
        } else if (now - lastProgressAt >=
                   std::chrono::seconds(5)) {
            constexpr int64_t kStaleInFlightMs = 30000;
            const bool workerRunning = aura::GlobalBackfillWorkerPool().running();
            const auto staleJobs = workerRunning
                ? std::vector<aura::BackfillJob>{}
                : state.backfillQueue.markStaleInFlightPermanent(kStaleInFlightMs);
            for (const auto& staleJob : staleJobs) {
                const bool terminalRecorded =
                    staleJob.scanSessionId <= 0 ||
                    staleJob.kind != aura::BackfillJobKind::WaveformL1Overview ||
                    scanDb.markScanForegroundMiniTerminal(
                        staleJob.scanSessionId,
                        staleJob.fileId,
                        ScanForegroundTerminalState::Failed,
                        "stale_in_flight");
                if (terminalRecorded) {
                    scanDb.clearBackfillPending(staleJob.fileId, static_cast<int>(staleJob.kind));
                }
            }
            int reconciledTerminal = 0;
            int requeuedMissing = 0;
            const auto pendingBackfillRows = scanDb.getBackfillPendingRows(512);
            for (const auto& row : pendingBackfillRows) {
                if (row.kind != waveformKind || row.fileId <= 0) continue;
                const auto kind = static_cast<aura::BackfillJobKind>(row.kind);
                const auto queueState = state.backfillQueue.itemState(row.fileId, kind);
                if (queueState == aura::BackfillItemState::Cached) {
                    const bool terminalRecorded =
                        scanDb.markScanForegroundMiniTerminal(
                            static_cast<int64_t>(scanGeneration),
                            row.fileId,
                            ScanForegroundTerminalState::Cached,
                            "queue_cached");
                    if (terminalRecorded &&
                        scanDb.clearBackfillPending(row.fileId, row.kind)) {
                        ++reconciledTerminal;
                    }
                } else if (queueState == aura::BackfillItemState::Unknown) {
                    state.backfillQueue.enqueueFromScan(
                        row.fileId,
                        kind,
                        aura::BackfillPriority::RecentScan,
                        state.backfillQueue.currentDbPath(),
                        state.backfillQueue.currentGeneration(),
                        static_cast<int64_t>(scanGeneration));
                    ++requeuedMissing;
                }
            }
            if (pendingRows > 0 &&
                snapshot.inFlight == 0 &&
                staleJobs.empty() &&
                reconciledTerminal == 0 &&
                requeuedMissing == 0) {
                const auto recoverRows = scanDb.getBackfillPendingRows(512, 0, 0, waveformKind);
                for (const auto& row : recoverRows) {
                    if (row.kind != waveformKind || row.fileId <= 0) continue;
                    state.backfillQueue.enqueueFromScan(
                        row.fileId,
                        static_cast<aura::BackfillJobKind>(row.kind),
                        aura::BackfillPriority::RecentScan,
                        state.backfillQueue.currentDbPath(),
                        state.backfillQueue.currentGeneration(),
                        static_cast<int64_t>(scanGeneration));
                }
                DIAG_SYSTEM_FMT("Scan",
                                "WaveformCacheDrainQueueRebuild: pending=%d cacheTotal=%d scanTotal=%d",
                                pendingRows,
                                waveformCacheTotal,
                                scanTotal);
            }
            DIAG_SYSTEM_FMT("Scan",
                            "WaveformCacheDrainStall: scoped=%d pending=%d inFlight=%d staleCleared=%d terminalCleared=%d requeued=%d workerRunning=%d cacheTotal=%d scanTotal=%d scope=%s",
                            scopedDrain ? 1 : 0,
                            pendingRows,
                            snapshot.inFlight,
                            (int)staleJobs.size(),
                            reconciledTerminal,
                            requeuedMissing,
                            workerRunning ? 1 : 0,
                            waveformCacheTotal,
                            scanTotal,
                            scopedDrain ? folderPathScope.c_str() : "<all>");
            lastProgressAt = now;
        }
        if (state.isScanTaskCurrent(scanGeneration)) {
            waveformCacheTotal = std::max(waveformCacheTotal, std::max(0, pendingRows));
            state.scanTotal = std::max(waveformCacheTotal, 1);
            state.scanProgress = std::clamp(waveformCacheTotal - std::max(0, pendingRows), 0, waveformCacheTotal);
            state.setScanMiniWaveformProgress(
                waveformCacheTotal - std::max(0, pendingRows),
                waveformCacheTotal);
            state.setScanFileKeyStr(LStr::SCAN_CachingWaveFmt, std::to_string(std::max(0, pendingRows)));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    DIAG_SYSTEM_FMT("Scan",
                    "WaveformCacheDrainCancelled: scoped=%d cacheTotal=%d scanTotal=%d scope=%s",
                    scopedDrain ? 1 : 0,
                    waveformCacheTotal,
                    scanTotal,
                    scopedDrain ? folderPathScope.c_str() : "<all>");
    return false;
}

static bool MarkScanForegroundMiniTerminalForJob(AppState& state,
                                                 const aura::BackfillJob& job,
                                                 bool success,
                                                 const std::string& reason) {
    if (job.scanSessionId <= 0 ||
        job.kind != aura::BackfillJobKind::WaveformL1Overview) {
        return true;
    }
    return state.database.markScanForegroundMiniTerminal(
        job.scanSessionId,
        job.fileId,
        success ? ScanForegroundTerminalState::Cached : ScanForegroundTerminalState::Failed,
        reason);
}

static bool CanClearBackfillPendingForJob(AppState& state,
                                          const aura::BackfillJob& job,
                                          bool success,
                                          const std::string& reason) {
    return MarkScanForegroundMiniTerminalForJob(state, job, success, reason);
}

// Globals defined here (declared extern in WindowsMain.h)

HWND g_hwnd = nullptr;
static HDC g_mainHdc = nullptr;
static HGLRC g_hRC = nullptr;

static LRESULT CALLBACK ExtraViewportCaptionProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
extern void NotifyDockNativePointerMessage(void*, bool, bool, void*);

static void ForwardDockPointerTermination(HWND hwnd, UINT msg, LPARAM lParam) {
    if (msg == WM_LBUTTONUP || msg == WM_NCLBUTTONUP)
        NotifyDockNativePointerMessage(hwnd, true, false, nullptr);
    else if (msg == WM_CANCELMODE || msg == WM_CAPTURECHANGED)
        NotifyDockNativePointerMessage(hwnd, false, true,
            msg == WM_CAPTURECHANGED ? reinterpret_cast<void*>(lParam) : nullptr);
}

static WNDPROC ExtraViewportOriginalProc(HWND hwnd) {
    return reinterpret_cast<WNDPROC>(aura::ui::dock::OriginalExtraViewportProc(hwnd));
}

static void SubclassExtraViewportCaption(HWND hwnd) {
    if (!hwnd || hwnd == g_hwnd) return;
    if (aura::ui::dock::OriginalExtraViewportProc(hwnd)) return;
    const LONG_PTR current = ::GetWindowLongPtrW(hwnd, GWLP_WNDPROC);
    if (!current) return;
    if (current == reinterpret_cast<LONG_PTR>(&ExtraViewportCaptionProc)) return;
    if (!aura::ui::dock::RememberExtraViewportProc(hwnd, reinterpret_cast<void*>(current))) return;
    ::SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&ExtraViewportCaptionProc));
}

static void SyncExtraViewportCaptionSubclasses() {
    if (!ImGui::GetCurrentContext()) return;
    ImGuiPlatformIO& io = ImGui::GetPlatformIO();
    for (int i = 1; i < io.Viewports.Size; ++i) {
        ImGuiViewport* vp = io.Viewports[i];
        if (!vp || !vp->PlatformHandle) continue;
        HWND hwnd = static_cast<HWND>(vp->PlatformHandle);
        if (hwnd == g_hwnd) continue;
        SubclassExtraViewportCaption(hwnd);
    }
}

static LRESULT CALLBACK ExtraViewportCaptionProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    ForwardDockPointerTermination(hwnd, msg, lParam);
#if AURA_BUILD_DIAGNOSTICS && ARIS_PREVIEW_ISOLATION && !AURA_INTERNAL_BETA
    if (msg == WM_NCHITTEST) {
        auto& trace = aura::ui::dock::ActiveDockFrameTrace();
        if (trace.enabled && trace.extraNcHitTests < 1000000) ++trace.extraNcHitTests;
    }
#endif
    aura::ui::dock::DockCaptionRuntime runtime{};
    runtime.hostHwnd = g_hwnd;
    runtime.originalProc = ExtraViewportOriginalProc(hwnd);
    return aura::ui::dock::DispatchDockCaptionWindowMessage(
        hwnd, msg, wParam, lParam, runtime);
}

#ifndef ARIS_ENABLE_IMGUI_VIEWPORTS
#define ARIS_ENABLE_IMGUI_VIEWPORTS 0
#endif
struct WGL_WindowData {
    HDC hDC = nullptr;
    HGLRC hRC = nullptr;
    bool presentable = false;
    bool ownsRc = false;
};

static bool HarkenSearchHasHostedPlatformViewports() {
#if ARIS_ENABLE_IMGUI_VIEWPORTS
    if (ImGui::GetCurrentContext() == nullptr) return false;
    const ImGuiIO& io = ImGui::GetIO();
    if ((io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) == 0) return false;
    return ImGui::GetPlatformIO().Viewports.Size > 1;
#else
    return false;
#endif
}

static bool CreateDeviceWGL(HWND hWnd, WGL_WindowData* data) {
    HDC hDc = ::GetDC(hWnd);
    PIXELFORMATDESCRIPTOR pfd = {};
    pfd.nSize = sizeof(pfd);
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.cDepthBits = 24;
    pfd.cStencilBits = 8;
    const int pf = ::ChoosePixelFormat(hDc, &pfd);
    if (pf == 0) {
        ::ReleaseDC(hWnd, hDc);
        return false;
    }
    if (::SetPixelFormat(hDc, pf, &pfd) == FALSE) {
        ::ReleaseDC(hWnd, hDc);
        return false;
    }
    ::ReleaseDC(hWnd, hDc);

    data->hDC = ::GetDC(hWnd);
    if (!g_hRC) {
        g_hRC = wglCreateContext(data->hDC);
        data->hRC = g_hRC;
        data->ownsRc = false;
        data->presentable = g_hRC != nullptr && data->hDC != nullptr;
        return data->presentable;
    }
    // Official Win32+GL extra viewports: one shared HGLRC, per-HWND DC.
    aura::ui::gl::ViewportGlState extra{};
    extra.hdc = data->hDC;
    extra.sharedRc = g_hRC;
    extra.hrc = nullptr;
    const aura::ui::gl::ViewportGlBindResult sharedMain =
        aura::ui::gl::ResolveExtraViewportContext(extra, true);
    if (sharedMain.presentable) {
        data->hRC = g_hRC;
        data->ownsRc = false;
        data->presentable = true;
        return true;
    }
    HGLRC created = wglCreateContext(data->hDC);
    const bool shared = created && wglShareLists(g_hRC, created);
    extra.hrc = created;
    const aura::ui::gl::ViewportGlBindResult afterShare =
        aura::ui::gl::ResolveExtraViewportContext(extra, shared);
    if (created) {
        wglDeleteContext(created);
        created = nullptr;
    }
    data->hRC = nullptr;
    data->ownsRc = false;
    data->presentable = false;
    (void)afterShare;
    return false;
}

static void CleanupDeviceWGL(HWND hWnd, WGL_WindowData* data) {
    data->presentable = false;
    aura::ui::gl::ViewportGlState extra{};
    extra.hdc = data->hDC;
    extra.hrc = data->hRC;
    extra.sharedRc = g_hRC;
    extra.ownsRc = data->ownsRc;
    extra.presentable = false;
    extra.destroyed = true;
    if (aura::ui::gl::ShouldClearCurrentGlOnExtraCleanup(extra)) {
        wglMakeCurrent(nullptr, nullptr);
    }
    if (data->ownsRc && data->hRC && data->hRC != g_hRC) {
        wglDeleteContext(data->hRC);
    }
    data->hRC = nullptr;
    data->ownsRc = false;
    if (data->hDC) {
        ::ReleaseDC(hWnd, data->hDC);
        data->hDC = nullptr;
    }
}

static void Hook_Renderer_CreateWindow(ImGuiViewport* viewport) {
    IM_ASSERT(viewport->RendererUserData == nullptr);
    WGL_WindowData* data = IM_NEW(WGL_WindowData)();
    if (!CreateDeviceWGL((HWND)viewport->PlatformHandle, data)) {
        data->presentable = false;
        data->hRC = nullptr;
    }
    viewport->RendererUserData = data;
}

static void Hook_Renderer_DestroyWindow(ImGuiViewport* viewport) {
    if (viewport->RendererUserData != nullptr) {
        WGL_WindowData* data = (WGL_WindowData*)viewport->RendererUserData;
        CleanupDeviceWGL((HWND)viewport->PlatformHandle, data);
        IM_DELETE(data);
        viewport->RendererUserData = nullptr;
    }
}

static void Hook_Platform_RenderWindow(ImGuiViewport* viewport, void*) {
    WGL_WindowData* data = (WGL_WindowData*)viewport->RendererUserData;
    if (!data) return;
    aura::ui::gl::ViewportGlState state{};
    state.hdc = data->hDC;
    state.hrc = data->hRC;
    state.presentable = data->presentable;
    state.destroyed = false;
    if (!aura::ui::gl::CanSwapViewportBuffers(state)) return;
    if (!wglMakeCurrent(data->hDC, data->hRC)) return;
}

static void Hook_Renderer_SwapBuffers(ImGuiViewport* viewport, void*) {
    WGL_WindowData* data = (WGL_WindowData*)viewport->RendererUserData;
    if (!data) return;
    aura::ui::gl::ViewportGlState state{};
    state.hdc = data->hDC;
    state.hrc = data->hRC;
    state.presentable = data->presentable;
    state.destroyed = false;
    if (!aura::ui::gl::CanSwapViewportBuffers(state)) return;
    ::SwapBuffers(data->hDC);
}

void* HarkenSearch::UiHost::MainWindowOwner() noexcept {
    return g_hwnd;
}
bool g_hideWindowOnDrag = true;
UINT g_hideWindowOnDragDelayMs = 400;
int g_suppressImGuiDragTooltipFrames = 0;
WINDOWPLACEMENT g_savedPlacementForDragHide = { sizeof(WINDOWPLACEMENT) };
bool g_savedPlacementForDragHideValid = false;
bool g_savedWasMaximizedForDragHide = false;
WINDOWPLACEMENT g_savedPlacementForTrayMinimize = { sizeof(WINDOWPLACEMENT) };
bool g_savedPlacementForTrayMinimizeValid = false;
bool g_savedWasMaximizedForTrayMinimize = false;
static std::atomic<ULONGLONG> g_lastUiLeftClickTickMs{0};
static std::atomic<bool> g_coverWallPriorityMode{false};

// 全局唤醒键 toggle 隐藏时保存的窗口状态
static WINDOWPLACEMENT g_savedPlacementForHotkeyHide = { sizeof(WINDOWPLACEMENT) };
static bool g_savedPlacementForHotkeyHideValid = false;
static bool g_savedWasMaximizedForHotkeyHide = false;

// Module-internal globals

bool g_showManagerHotkeyRegisterOk = true;
// 登录阶段：用于拦截 WM_CLOSE，避免被最小化/提示逻辑吞掉
static bool g_loginPhaseActive = false;
static std::atomic<bool> g_explicitExitRequested{ false };
#if AURA_BUILD_DIAGNOSTICS && ARIS_PREVIEW_ISOLATION && !AURA_INTERNAL_BETA
static std::atomic<bool> s_qaUpdateExitRequested{ false };
#endif
static NOTIFYICONDATAW g_trayNid = {};
static bool g_trayIconAdded = false;
static const UINT TRAY_CMD_TOGGLE_SHOW = 0x1101;
static const UINT TRAY_CMD_EXIT = 0x1102;
static const UINT AURO_WM_SHOW_MANAGER = WM_APP + 0x42;
static UINT WM_TASKBARCREATED = 0; // 动态注册的 TaskbarCreated 消息ID

void MarkUiLeftClickNow() {
    g_lastUiLeftClickTickMs.store(
        Platform::MonotonicMilliseconds(), std::memory_order_relaxed);
}

bool IsRapidUiClickBurstActive() {
    const ULONGLONG lastTick = g_lastUiLeftClickTickMs.load(std::memory_order_relaxed);
    if (lastTick == 0) return false;
    const ULONGLONG now = Platform::MonotonicMilliseconds();
    return now >= lastTick && (now - lastTick) <= 320;
}

void SetCoverWallPriorityMode(bool enabled) {
    g_coverWallPriorityMode.store(enabled, std::memory_order_relaxed);
}

bool IsCoverWallPriorityMode() {
    return g_coverWallPriorityMode.load(std::memory_order_relaxed);
}

enum class CoverPhysicalTaskType {
    AssignEmbed = 1,
    RemoveEmbed = 2,
};

struct CoverPhysicalTask {
    int64_t jobId = 0;
    CoverPhysicalTaskType type = CoverPhysicalTaskType::AssignEmbed;
    std::vector<std::string> filePaths;
    std::vector<uint8_t> imageData;
    std::string mimeType;
};

struct CoverPhysicalTaskStatus {
    int total = 0;
    int done = 0;
    int succeeded = 0;
    int failed = 0;
    bool finished = false;
    std::string label;
    std::string lastError;
};

static std::mutex g_coverTaskMutex;
static std::condition_variable g_coverTaskCv;
static std::deque<CoverPhysicalTask> g_coverTaskQueue;
static std::unordered_map<int64_t, CoverPhysicalTaskStatus> g_coverTaskStatus;
static std::atomic<int64_t> g_coverTaskNextId{ 1 };
static std::atomic<bool> g_coverTaskStop{ false };
static std::thread g_coverTaskThread;

static void EnsureCoverTaskWorkerStarted() {
    if (g_coverTaskThread.joinable()) return;
    g_coverTaskStop.store(false);
    g_coverTaskThread = std::thread([]() {
        while (!g_coverTaskStop.load()) {
            CoverPhysicalTask task;
            {
                std::unique_lock<std::mutex> lk(g_coverTaskMutex);
                g_coverTaskCv.wait(lk, []() {
                    return g_coverTaskStop.load() || !g_coverTaskQueue.empty();
                });
                if (g_coverTaskStop.load() && g_coverTaskQueue.empty()) break;
                task = std::move(g_coverTaskQueue.front());
                g_coverTaskQueue.pop_front();
            }

            for (const auto& path : task.filePaths) {
                bool ok = false;
                if (Platform::FileSystem::IsNetworkPath(path)) {
                    ok = false;
                } else if (task.type == CoverPhysicalTaskType::AssignEmbed) {
                     ok = MetadataWriter::writeAlbumArt(path, task.imageData, task.mimeType);
                 } else {
                     ok = MetadataWriter::removeAlbumArt(path);
                 }
                {
                    std::lock_guard<std::mutex> lk(g_coverTaskMutex);
                    auto it = g_coverTaskStatus.find(task.jobId);
                    if (it != g_coverTaskStatus.end()) {
                        it->second.done++;
                        if (ok) it->second.succeeded++;
                        else {
                            it->second.failed++;
                            it->second.lastError = Platform::FileSystem::IsNetworkPath(path)
                                ? std::string("Write_BlockedNetworkLibrary")
                                : path;
                         }
                    }
                }
            }
            {
                std::lock_guard<std::mutex> lk(g_coverTaskMutex);
                auto it = g_coverTaskStatus.find(task.jobId);
                if (it != g_coverTaskStatus.end()) {
                    it->second.finished = true;
                }
            }
        }
    });
}

static int64_t EnqueueCoverPhysicalTask(CoverPhysicalTask&& task) {
    if (task.filePaths.empty()) return 0;
    EnsureCoverTaskWorkerStarted();
    const int64_t jobId = g_coverTaskNextId.fetch_add(1);
    task.jobId = jobId;
    {
        std::lock_guard<std::mutex> lk(g_coverTaskMutex);
        CoverPhysicalTaskStatus st;
        st.total = (int)task.filePaths.size();
        st.label = (task.type == CoverPhysicalTaskType::AssignEmbed) ? "assign" : "remove";
        g_coverTaskStatus[task.jobId] = st;
        g_coverTaskQueue.push_back(std::move(task));
    }
    g_coverTaskCv.notify_one();
    return jobId;
}

// Crash handler - 崩溃时自动清理托盘图标，防止幽灵图标

static LPTOP_LEVEL_EXCEPTION_FILTER g_prevExceptionFilter = nullptr;

// Helper: exception code → human-readable description (crash-safe, no heap)
static const char* ExceptionCodeToString(DWORD code) {
    switch (code) {
        case EXCEPTION_ACCESS_VIOLATION:         return "ACCESS_VIOLATION";
        case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:    return "ARRAY_BOUNDS_EXCEEDED";
        case EXCEPTION_BREAKPOINT:               return "BREAKPOINT";
        case EXCEPTION_DATATYPE_MISALIGNMENT:    return "DATATYPE_MISALIGNMENT";
        case EXCEPTION_FLT_DENORMAL_OPERAND:     return "FLT_DENORMAL_OPERAND";
        case EXCEPTION_FLT_DIVIDE_BY_ZERO:       return "FLT_DIVIDE_BY_ZERO";
        case EXCEPTION_FLT_INEXACT_RESULT:       return "FLT_INEXACT_RESULT";
        case EXCEPTION_FLT_INVALID_OPERATION:    return "FLT_INVALID_OPERATION";
        case EXCEPTION_FLT_OVERFLOW:             return "FLT_OVERFLOW";
        case EXCEPTION_FLT_STACK_CHECK:          return "FLT_STACK_CHECK";
        case EXCEPTION_FLT_UNDERFLOW:            return "FLT_UNDERFLOW";
        case EXCEPTION_GUARD_PAGE:               return "GUARD_PAGE";
        case EXCEPTION_ILLEGAL_INSTRUCTION:      return "ILLEGAL_INSTRUCTION";
        case EXCEPTION_IN_PAGE_ERROR:            return "IN_PAGE_ERROR";
        case EXCEPTION_INT_DIVIDE_BY_ZERO:       return "INT_DIVIDE_BY_ZERO";
        case EXCEPTION_INT_OVERFLOW:             return "INT_OVERFLOW";
        case EXCEPTION_INVALID_DISPOSITION:      return "INVALID_DISPOSITION";
        case EXCEPTION_NONCONTINUABLE_EXCEPTION: return "NONCONTINUABLE_EXCEPTION";
        case EXCEPTION_PRIV_INSTRUCTION:         return "PRIV_INSTRUCTION";
        case EXCEPTION_SINGLE_STEP:              return "SINGLE_STEP";
        case EXCEPTION_STACK_OVERFLOW:            return "STACK_OVERFLOW";
        case 0xC0000374:                         return "HEAP_CORRUPTION";
        case 0xC0000409:                         return "STATUS_STACK_BUFFER_OVERRUN (__fastfail)";
        default:                                 return "UNKNOWN";
    }
}

// F-036: Write structured bug report JSON to local directory
void WriteBugReportJson(const std::string& type,
                        const std::string& userName,
                        const std::string& userText,
                        const std::string& email,
                        const std::string& sysInfo,
                        const std::string& recentLog) {
#ifdef _WIN32
    namespace fs = std::filesystem;

    const Platform::ExecutablePathResult executablePath = Platform::GetExecutablePath();
    if (executablePath.status != Platform::ExecutablePathStatus::Success) return;
    fs::path dir = Utf8ToPath(executablePath.pathUtf8).parent_path() / "bug_reports";
    if (!Platform::FileSystem::EnsureDirectoryExists(PathToUtf8(dir))) return;

    auto now = std::chrono::system_clock::now();
    auto epoch_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        now.time_since_epoch()).count();
    std::time_t tt = std::chrono::system_clock::to_time_t(now);
    struct tm lt = {};
    localtime_s(&lt, &tt);
    char timeBuf[64] = {};
    std::strftime(timeBuf, sizeof(timeBuf), "%Y-%m-%d %H:%M:%S", &lt);

    std::string filename = "bug_report_" + std::to_string(epoch_ms) + ".json";
    fs::path filePath = dir / filename;

    std::string hwid = GetEncryptedMachineID();
    std::string displayName = userName.empty() ? "unknown" : userName;
    std::string buildInfo = "Harken Search Beta (built " __DATE__ " " __TIME__ ")";

    auto jsonEscape = [](const std::string& s) -> std::string {
        std::string out;
        out.reserve(s.size() + 32);
        for (char c : s) {
            switch (c) {
                case '"':  out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                default:   out += c; break;
            }
        }
        return out;
    };

    // Split recentLog into individual JSONL lines
    std::string diagArray = "[";
    {
        std::istringstream stream(recentLog);
        std::string line;
        bool first = true;
        while (std::getline(stream, line)) {
            if (line.empty()) continue;
            if (!first) diagArray += ",";
            diagArray += "\n    " + line;
            first = false;
        }
        diagArray += "\n  ]";
    }

    std::ostringstream payload;
    payload << "{\n";
    payload << "  \"type\": \"" << jsonEscape(type) << "\",\n";
    payload << "  \"userName\": \"" << jsonEscape(displayName) << "\",\n";
    payload << "  \"hwid\": \"" << jsonEscape(hwid) << "\",\n";
    payload << "  \"buildInfo\": \"" << jsonEscape(buildInfo) << "\",\n";
    payload << "  \"timestamp\": \"" << timeBuf << "\",\n";
    payload << "  \"timestampMs\": " << epoch_ms << ",\n";
    payload << "  \"userText\": \"" << jsonEscape(userText) << "\",\n";
    payload << "  \"email\": \"" << jsonEscape(email) << "\",\n";
    payload << "  \"sysInfo\": \"" << jsonEscape(sysInfo) << "\",\n";
    payload << "  \"diagnosticLog\": " << diagArray << "\n";
    payload << "}\n";

    Platform::FileSystem::StreamWriter writer;
    if (!writer.Open(PathToUtf8(filePath), false, true)) return;
    const std::string payloadText = payload.str();
    writer.Write(payloadText);
    writer.Close();
#endif
}

// F-035: Collect system info for feedback / crash report payloads
std::string BuildSysInfo() {
    std::ostringstream oss;

    // OS version via RtlGetVersion (reports true version unlike GetVersionEx)
    {
        typedef LONG(WINAPI* RtlGetVersionPtr)(PRTL_OSVERSIONINFOW);
        HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        if (ntdll) {
            auto fn = (RtlGetVersionPtr)GetProcAddress(ntdll, "RtlGetVersion");
            if (fn) {
                RTL_OSVERSIONINFOW ovi = {};
                ovi.dwOSVersionInfoSize = sizeof(ovi);
                fn(&ovi);
                oss << "OS: Windows " << ovi.dwMajorVersion << "." << ovi.dwMinorVersion
                    << " (Build " << ovi.dwBuildNumber << ")\n";
            }
        }
    }

    // Physical memory
    {
        MEMORYSTATUSEX ms = {};
        ms.dwLength = sizeof(ms);
        if (GlobalMemoryStatusEx(&ms)) {
            oss << "RAM: " << (ms.ullTotalPhys / (1024 * 1024)) << " MB\n";
        }
    }

    // CPU name via registry
    {
        auto cpuName = Platform::ReadRegistryString(
            Platform::RegistryHive::LocalMachine,
            "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
            "ProcessorNameString");
        if (cpuName.status == Platform::RegistryStringStatus::Success) {
            oss << "CPU: " << cpuName.valueUtf8 << "\n";
        }
    }

    // GPU via OpenGL (renderer string is set after wglMakeCurrent)
    {
        const char* renderer = (const char*)glGetString(GL_RENDERER);
        if (renderer) oss << "GPU: " << renderer << "\n";
    }

    oss << "App: Harken Search Beta (built " << __DATE__ << " " << __TIME__ << ")\n";
    return oss.str();
}

static LONG WINAPI HarkenSearchCrashHandler(EXCEPTION_POINTERS* pExceptionInfo) {
    // Crash-time tray cleanup
    if (g_trayIconAdded) {
        Shell_NotifyIconW(NIM_DELETE, &g_trayNid);
        g_trayIconAdded = false;
    }

    // ---- Write MiniDump for post-mortem debugging ----
    wchar_t dumpPath[MAX_PATH] = {};
    {
        DWORD n = Platform::GetTempPathBuffer(dumpPath, MAX_PATH);
        if (n > 0 && n < MAX_PATH) {
            SYSTEMTIME st;
            GetLocalTime(&st);
            wchar_t name[128];
            swprintf_s(name, L"HarkenSearch_crash_%04u%02u%02u_%02u%02u%02u.dmp",
                       st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
            wcscat_s(dumpPath, name);

            void* hFile = Platform::OpenCrashFile(dumpPath, false);
            if (hFile) {
                MINIDUMP_EXCEPTION_INFORMATION mei = {};
                mei.ThreadId = static_cast<DWORD>(Platform::CurrentThreadId());
                mei.ExceptionPointers = pExceptionInfo;
                mei.ClientPointers = FALSE;

                MiniDumpWriteDump(static_cast<HANDLE>(Platform::CurrentProcessHandle()),
                                 static_cast<DWORD>(Platform::CurrentProcessId()), hFile,
                                 (MINIDUMP_TYPE)(MiniDumpNormal | MiniDumpWithDataSegs),
                                 pExceptionInfo ? &mei : nullptr, nullptr, nullptr);
                Platform::CloseNativeHandle(hFile);

                AppendDiagLogF(L"CRASH: MiniDump written to %s", dumpPath);
            }
        }
    }

    // ---- Crash-time diagnostics snapshot (JSONL, best-effort) ----
    {
        wchar_t diagPath[MAX_PATH] = {};
        DWORD n = Platform::GetTempPathBuffer(diagPath, MAX_PATH);
        if (n > 0 && n < MAX_PATH) {
            wcscat_s(diagPath, L"HarkenSearch_crash_diagnostics.jsonl");
            DiagnosticsLogger::instance().exportJsonLinesCrashSafe(diagPath);
        }
    }

    // ---- Write plain-text (UTF-8) crash report for easy inspection ----
    {
        wchar_t txtPath[MAX_PATH] = {};
        DWORD n = Platform::GetTempPathBuffer(txtPath, MAX_PATH);
        if (n > 0 && n < MAX_PATH) {
            wcscat_s(txtPath, L"HarkenSearch_crash_info.txt");

            void* hTxt = Platform::OpenCrashFile(txtPath, true);
            if (hTxt) {
                SYSTEMTIME st;
                GetLocalTime(&st);
                char buf[4096] = {};
                int pos = 0;

                // -- Header --
                pos += sprintf_s(buf + pos, sizeof(buf) - pos,
                    "========== ARIS CRASH REPORT ==========\n"
                    "Time: %04u-%02u-%02u %02u:%02u:%02u\n"
                    "PID: %u  TID: %u\n",
                    st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
                    static_cast<unsigned>(Platform::CurrentProcessId()),
                    static_cast<unsigned>(Platform::CurrentThreadId()));

                // -- Exception info --
                if (pExceptionInfo && pExceptionInfo->ExceptionRecord) {
                    DWORD code = pExceptionInfo->ExceptionRecord->ExceptionCode;
                    void* addr = pExceptionInfo->ExceptionRecord->ExceptionAddress;
                    pos += sprintf_s(buf + pos, sizeof(buf) - pos,
                        "Exception: 0x%08X (%s)\n"
                        "Address:   0x%p\n",
                        (unsigned)code, ExceptionCodeToString(code), addr);

                    // ACCESS_VIOLATION: show read/write target address
                    if (code == EXCEPTION_ACCESS_VIOLATION &&
                        pExceptionInfo->ExceptionRecord->NumberParameters >= 2) {
                        ULONG_PTR rwFlag = pExceptionInfo->ExceptionRecord->ExceptionInformation[0];
                        ULONG_PTR target = pExceptionInfo->ExceptionRecord->ExceptionInformation[1];
                        pos += sprintf_s(buf + pos, sizeof(buf) - pos,
                            "Access:    %s address 0x%p\n",
                            rwFlag == 0 ? "Reading" : (rwFlag == 1 ? "Writing" : "Executing"),
                            (void*)target);
                    }
                }

                // -- Module containing crash address --
                if (pExceptionInfo && pExceptionInfo->ExceptionRecord) {
                    HMODULE hMod = nullptr;
                    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                           (LPCWSTR)pExceptionInfo->ExceptionRecord->ExceptionAddress,
                                           &hMod) && hMod) {
                        wchar_t modPathW[MAX_PATH] = {};
                        Platform::GetModuleFileNameBuffer(static_cast<void*>(hMod), modPathW, MAX_PATH);
                        // Extract just the filename
                        wchar_t* slash = wcsrchr(modPathW, L'\\');
                        const wchar_t* modName = slash ? slash + 1 : modPathW;
                        ULONG_PTR offset = (ULONG_PTR)pExceptionInfo->ExceptionRecord->ExceptionAddress - (ULONG_PTR)hMod;
                        pos += sprintf_s(buf + pos, sizeof(buf) - pos,
                            "Module:    %ls + 0x%IX\n", modName, offset);
                    }
                }

                // -- Stack trace (using SymFromAddr for symbol names) --
                {
                    HANDLE hProcess = static_cast<HANDLE>(Platform::CurrentProcessHandle());
                    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
                    SymInitialize(hProcess, nullptr, TRUE);

                    CONTEXT ctx = {};
                    if (pExceptionInfo && pExceptionInfo->ContextRecord) {
                        ctx = *pExceptionInfo->ContextRecord;
                    }

                    STACKFRAME64 sf = {};
#ifdef _M_X64
                    DWORD machineType = IMAGE_FILE_MACHINE_AMD64;
                    sf.AddrPC.Offset    = ctx.Rip;
                    sf.AddrPC.Mode      = AddrModeFlat;
                    sf.AddrFrame.Offset = ctx.Rbp;
                    sf.AddrFrame.Mode   = AddrModeFlat;
                    sf.AddrStack.Offset = ctx.Rsp;
                    sf.AddrStack.Mode   = AddrModeFlat;
#else
                    DWORD machineType = IMAGE_FILE_MACHINE_I386;
                    sf.AddrPC.Offset    = ctx.Eip;
                    sf.AddrPC.Mode      = AddrModeFlat;
                    sf.AddrFrame.Offset = ctx.Ebp;
                    sf.AddrFrame.Mode   = AddrModeFlat;
                    sf.AddrStack.Offset = ctx.Esp;
                    sf.AddrStack.Mode   = AddrModeFlat;
#endif
                    pos += sprintf_s(buf + pos, sizeof(buf) - pos, "\nStack Trace:\n");

                    char symbolBuf[sizeof(SYMBOL_INFO) + MAX_SYM_NAME * sizeof(TCHAR)] = {};
                    SYMBOL_INFO* sym = (SYMBOL_INFO*)symbolBuf;
                    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
                    sym->MaxNameLen = MAX_SYM_NAME;

                    IMAGEHLP_LINE64 lineInfo = {};
                    lineInfo.SizeOfStruct = sizeof(IMAGEHLP_LINE64);

                    const int kMaxFrames = 32;
                    for (int frame = 0; frame < kMaxFrames; ++frame) {
                        if (!StackWalk64(machineType, hProcess, GetCurrentThread(),
                                         &sf, &ctx, nullptr,
                                         SymFunctionTableAccess64, SymGetModuleBase64, nullptr)) {
                            break;
                        }
                        if (sf.AddrPC.Offset == 0) break;

                        DWORD64 displacement64 = 0;
                        DWORD   displacement32 = 0;
                        bool hasSym = SymFromAddr(hProcess, sf.AddrPC.Offset, &displacement64, sym) != FALSE;
                        bool hasLine = SymGetLineFromAddr64(hProcess, sf.AddrPC.Offset, &displacement32, &lineInfo) != FALSE;

                        // Find module name for this frame
                        HMODULE hMod = nullptr;
                        char modShort[64] = "???";
                        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                               (LPCWSTR)(uintptr_t)sf.AddrPC.Offset, &hMod) && hMod) {
                            wchar_t mp[MAX_PATH] = {};
                            Platform::GetModuleFileNameBuffer(static_cast<void*>(hMod), mp, MAX_PATH);
                            wchar_t* s = wcsrchr(mp, L'\\');
                            const wchar_t* mn = s ? s + 1 : mp;
                            // Convert to ASCII for the report
                            Platform::WideToUtf8Buffer(mn, modShort, sizeof(modShort));
                        }

                        if (hasSym && hasLine) {
                            // Extract just the filename from the full path
                            const char* srcFile = lineInfo.FileName;
                            const char* lastSlash = strrchr(srcFile, '\\');
                            if (lastSlash) srcFile = lastSlash + 1;
                            pos += sprintf_s(buf + pos, sizeof(buf) - pos,
                                "  [%2d] %s!%s +0x%IX  (%s:%u)\n",
                                frame, modShort, sym->Name, (uintptr_t)displacement64,
                                srcFile, (unsigned)lineInfo.LineNumber);
                        } else if (hasSym) {
                            pos += sprintf_s(buf + pos, sizeof(buf) - pos,
                                "  [%2d] %s!%s +0x%IX\n",
                                frame, modShort, sym->Name, (uintptr_t)displacement64);
                        } else {
                            pos += sprintf_s(buf + pos, sizeof(buf) - pos,
                                "  [%2d] %s!0x%p\n",
                                frame, modShort, (void*)sf.AddrPC.Offset);
                        }

                        if (pos >= (int)sizeof(buf) - 200) break; // guard against overflow
                    }

                    SymCleanup(hProcess);
                }

                // -- Dump file location --
                if (dumpPath[0]) {
                    pos += sprintf_s(buf + pos, sizeof(buf) - pos,
                        "\nDump file: ");
                    // Convert wide dump path to UTF-8
                    char dumpPathUtf8[MAX_PATH * 2] = {};
                    Platform::WideToUtf8Buffer(dumpPath, dumpPathUtf8, sizeof(dumpPathUtf8));
                    pos += sprintf_s(buf + pos, sizeof(buf) - pos, "%s\n", dumpPathUtf8);
                }

                pos += sprintf_s(buf + pos, sizeof(buf) - pos,
                    "=======================================\n\n");

                // Write to file
                Platform::WriteFileBytes(hTxt, buf, static_cast<uint32_t>(pos));
                Platform::CloseNativeHandle(hTxt);
            }
        }
    }

    // Show a message box so user knows about the crash
    {
        // H1 (P1-3): exception filters must not allocate; Lw()/ledger lookup is
        // PERMANENTLY BANNED here (exemption band: crash-filter rendering keeps the
        // verbatim literals in every UI language). Rationale: allocation in a fatal
        // filter can itself fault and hide the crash.
        wchar_t msg[600] = {};
        if (dumpPath[0]) {
            swprintf_s(msg, L"Harken Search 发生了崩溃。\n\n"
                L"崩溃转储: %s\n"
                L"崩溃报告: %%TEMP%%\\HarkenSearch_crash_info.txt\n\n"
                L"请将这两个文件发送给开发者以帮助修复问题。", dumpPath);
        } else {
            swprintf_s(msg, L"Harken Search 发生了崩溃。\n\n崩溃报告已保存到: %%TEMP%%\\HarkenSearch_crash_info.txt");
        }
        Platform::ShowMessageBox(nullptr, msg, L"Harken Search 崩溃报告", MB_OK | MB_ICONERROR);

        // 自动打开崩溃文件所在文件夹，方便用户发送 .dmp 与 .txt 给开发者
        wchar_t folderPath[MAX_PATH] = {};
        if (dumpPath[0]) {
            wcscpy_s(folderPath, dumpPath);
            wchar_t* lastSlash = wcsrchr(folderPath, L'\\');
            if (lastSlash) {
                *lastSlash = L'\0';
                Platform::OpenCrashReportFolder(folderPath);
            }
        } else {
            DWORD n = Platform::GetTempPathBuffer(folderPath, MAX_PATH);
            if (n > 0 && n < MAX_PATH)
                Platform::OpenCrashReportFolder(folderPath);
        }
    }

    if (g_prevExceptionFilter) {
        return g_prevExceptionFilter(pExceptionInfo);
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

namespace fs = std::filesystem;

// Diagnostic Logging

namespace {

HarkenSearch::DiagnosticFileLifecycle& DiagnosticFile() {
    // Detached diagnostic writers may still run during process teardown.  Keep
    // the shared lifecycle object alive for the process lifetime so those
    // writers never touch a destructed mutex or string.
    static auto* file = [] {
        std::string fileName = "HarkenSearch_diag.log";
        if (IsHarkenSearchHarnessMultiInstanceEnabled()) {
            fileName = "HarkenSearch_diag_" + GetHarkenSearchInstanceKeyUtf8() + ".log";
        }
        return new HarkenSearch::DiagnosticFileLifecycle(
            Platform::GetTempDirectory() + fileName, 10 * 1024 * 1024,
            sizeof(wchar_t));
    }();
    return *file;
}

} // namespace

static void SyncDiagnosticFileEnabledFromSettings() {
    std::string settingsPath = g_appSettingsPathUtf8;
    if (settingsPath.empty()) {
        const std::string exeDirUtf8 = GetExeDirUtf8();
#if AURA_INTERNAL_BETA
        const bool devMode = false;
#else
        const bool devMode = PathExistsUtf8(exeDirUtf8 + "\\dev_mode.txt");
#endif
        if (devMode) {
            settingsPath = GetHarkenSearchRuntimeRootUtf8() + "\\settings_dev.ini";
        } else {
            const auto layout = Platform::HarkenUserData::EnsureLayout();
            if (!layout.activeSettingsDir.empty()) {
                settingsPath = Platform::HarkenUserData::WideToUtf8Path(layout.activeSettingsDir) + "\\settings.ini";
            } else {
                settingsPath = exeDirUtf8 + "\\settings.ini";
            }
        }
    }
    std::string settings;
    bool enabled = false;
    const bool devMode = [&] {
        const std::string exeDirUtf8 = GetExeDirUtf8();
#if AURA_INTERNAL_BETA
        return false;
#else
        return PathExistsUtf8(exeDirUtf8 + "\\dev_mode.txt");
#endif
    }();
    if (Platform::FileSystem::ReadFileText(settingsPath, settings)) {
        size_t start = 0;
        while (start <= settings.size()) {
            const size_t end = settings.find('\n', start);
            std::string line(settings.data() + start,
                             (end == std::string::npos ? settings.size() : end) - start);
            const auto trim = [](std::string& value) {
                const auto first = value.find_first_not_of(" \t\r");
                const auto last = value.find_last_not_of(" \t\r");
                if (first == std::string::npos) {
                    value.clear();
                } else {
                    value = value.substr(first, last - first + 1);
                }
            };
            trim(line);
            const auto equals = line.find('=');
            std::string key = equals == std::string::npos ? line : line.substr(0, equals);
            trim(key);
            if (key == "diagnosticMode") {
                std::string value = equals == std::string::npos ? std::string() : line.substr(equals + 1);
                trim(value);
                std::transform(value.begin(), value.end(), value.begin(),
                               [](unsigned char c) {
                                   return static_cast<char>(std::tolower(c));
                               });
                enabled = value == "1" || value == "true" || value == "yes" || value == "on";
            }
            if (end == std::string::npos) break;
            start = end + 1;
        }
    }
    // Development mode is an explicit startup contract: LoadAppSettings later
    // forces diagnostics on even when the INI omits or disables the key.
    SetDiagnosticFileEnabled(devMode || enabled);
}

void AppendDiagLogLine(const wchar_t* line) {
    if (!line) return;
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t buf[1400] = {};
    swprintf_s(buf, L"[%02u:%02u:%02u.%03u] %s\r\n",
               st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, line);
    // File I/O on the main thread blocks the render loop and causes periodic
    // playback-cursor stutter (CreateFileW+WriteFile+CloseHandle can take
    // 5–50 ms on HDDs or under antivirus load). Dispatch to a detached thread.
    std::wstring msg(buf);
    const std::uint64_t generation = DiagnosticFile().generation();
    std::thread([msg = std::move(msg), generation]() noexcept {
        auto& file = DiagnosticFile();
        if (!file.isEnabled()) return;
        const char* bytes = reinterpret_cast<const char*>(msg.data());
        file.appendAtGeneration(std::string_view(bytes, msg.size() * sizeof(wchar_t)), generation);
    }).detach();
}

void AppendDiagLogF(const wchar_t* fmt, ...) {
    if (!fmt) return;
    wchar_t msg[1024] = {};
    va_list args;
    va_start(args, fmt);
    _vsnwprintf_s(msg, _countof(msg), _TRUNCATE, fmt, args);
    va_end(args);
    AppendDiagLogLine(msg);
}

void SetDiagnosticFileEnabled(bool enabled) {
    DiagnosticFile().setEnabled(enabled);
}

std::uint64_t GetDiagnosticFileSizeBytes() {
    return static_cast<std::uint64_t>(DiagnosticFile().sizeBytes());
}

std::uint64_t GetDiagnosticFileMaxBytes() {
    return static_cast<std::uint64_t>(DiagnosticFile().maxBytes());
}

bool ClearDiagnosticFile() {
    return DiagnosticFile().clear();
}

// Window Management

// QA Automation — file-based command channel for Agent-driven testing

static const char* ScanPhaseToSnakeCase(AppState::ScanPhase phase) {
    switch (phase) {
        case AppState::ScanPhase::WaveformCache: return "waveform_cache";
        case AppState::ScanPhase::InitialImport: return "initial_import";
        case AppState::ScanPhase::IncrementalScan: return "incremental_scan";
        case AppState::ScanPhase::RemoveMissing: return "remove_missing";
        case AppState::ScanPhase::RenameOrMove: return "rename_or_move";
        case AppState::ScanPhase::FailureRetry: return "failure_retry";
        case AppState::ScanPhase::Idle:
        default:
            return "idle";
    }
}

static std::string NormalizeReadyToWorkPathForCompare(std::string path) {
    if (path.empty()) return path;
#ifdef _WIN32
    if (path.size() >= 8 && path.compare(0, 8, "\\\\?\\UNC\\") == 0) {
        path = "\\" + path.substr(8);
    } else if (path.size() >= 4 && path.compare(0, 4, "\\\\?\\") == 0) {
        path = path.substr(4);
    }
    for (char& c : path) {
        if (c == '/') c = '\\';
        else if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
    }
#else
    for (char& c : path) {
        if (c == '\\') c = '/';
    }
#endif
    return path;
}

static std::string BaseNameFromUtf8PathForQa(const std::string& pathUtf8) {
    if (pathUtf8.empty()) return {};
    try {
        fs::path path = Utf8ToPath(pathUtf8);
        return PathToUtf8(path.filename());
    } catch (...) {
        const size_t pos = pathUtf8.find_last_of("/\\");
        return (pos == std::string::npos) ? pathUtf8 : pathUtf8.substr(pos + 1);
    }
}

static bool IsReadyToWorkFileUnderFolder(const std::string& filePath,
                                         const std::string& folderPath) {
    const std::string fileNorm = NormalizeReadyToWorkPathForCompare(filePath);
    std::string folderNorm =
        NormalizeReadyToWorkPathForCompare(HarkenSearch::CanonicalFolderPath(folderPath));
    if (fileNorm.empty() || folderNorm.empty()) return false;
    if (fileNorm == folderNorm) return true;
#ifdef _WIN32
    if (folderNorm.back() != '\\' && folderNorm.back() != '/') {
        folderNorm.push_back('\\');
    }
#else
    if (folderNorm.back() != '/') {
        folderNorm.push_back('/');
    }
#endif
    return fileNorm.rfind(folderNorm, 0) == 0;
}

static std::string GetReadyToWorkCurrentMainWavePathForQa(const AppState& state,
                                                          const std::string& folderPathScope) {
    if (state.currentFileId <= 0) return {};
    const std::string loadedPath = state.audioEngine.getLoadedFilePath();
    if (loadedPath.empty()) return {};
    if (!folderPathScope.empty() &&
        !IsReadyToWorkFileUnderFolder(loadedPath, folderPathScope)) {
        return {};
    }
    return loadedPath;
}

static bool HasUsableReadyToWorkMainWaveformForQa(const AppState& state,
                                                  const std::string& expectedPath) {
    if (expectedPath.empty()) return true;

    auto waveform = state.audioEngine.getWaveformData();
    if (!waveform ||
        !waveform->isReady ||
        waveform->isFallbackPlaceholder ||
        waveform->peaks.empty()) {
        return false;
    }

    const std::string expectedNorm = NormalizeReadyToWorkPathForCompare(expectedPath);
    const std::string waveformNorm =
        NormalizeReadyToWorkPathForCompare(waveform->filePath);
    if (expectedNorm.empty() || waveformNorm.empty() || expectedNorm != waveformNorm) {
        return false;
    }

    const int channelCount = (std::max)(1, waveform->numChannels);
    const int completedPeaks = waveform->completedPeaks > 0
        ? waveform->completedPeaks
        : (int)(waveform->peaks.size() / (size_t)channelCount);
    constexpr int kMinUsableMainWavePeaks = 1024;
    return completedPeaks >= kMinUsableMainWavePeaks ||
           !state.audioEngine.isWaveformDetailPending();
}

static int CountReadyToWorkCoverPendingRowsForQa(Database& db,
                                                 const std::string& folderPathScope) {
    const int coverKind = static_cast<int>(aura::BackfillJobKind::CoverResolveAndThumb);
    return folderPathScope.empty()
        ? db.countBackfillPendingKind(coverKind)
        : db.countBackfillPendingKindInFolder(coverKind, folderPathScope);
}

static bool HasOutstandingReadyToWorkCoverPostProcessForQa(
    AppState& state,
    const std::string& folderPathScope) {
    if (!state.database.isOpen()) return false;
    return CountReadyToWorkCoverPendingRowsForQa(state.database, folderPathScope) > 0 ||
           state.database.isCoverPostProcessRunning() ||
           state.database.hasPendingPostProcessCoverRefresh();
}

static bool HasPendingReadyToWorkSearchRefreshForQa(const AppState& state) {
    return state.needsDatabaseRefresh ||
           state.asyncSearchInProgress ||
           state.pendingSearchRequestId != 0 ||
           state.pendingFacetRequestId != 0;
}

static bool HasPendingReadyToWorkCoverSourceRefreshForQa(const AppState& state) {
    if (HasPendingReadyToWorkSearchRefreshForQa(state)) {
        return true;
    }

    return state.coverWallDisplayRefreshRequested ||
           state.coverWallDatabaseInvalidationRequested ||
           state.needsCoverWallReload ||
           state.coverWall.loading ||
           state.coverWall.preloadInFlight ||
           state.coverWall.entriesInFlight ||
           state.coverWall.foldersInFlight ||
           (!state.coverWall.loaded &&
            (!state.coverWall.items.empty() ||
             !state.searchResults.empty() ||
             !state.coverWall.preloadFullList.empty()));
}

static bool HasOutstandingReadyToWorkMiniWavePresentationForQa(
    AppState& state,
    const std::string& folderPathScope) {
    if (HasPendingReadyToWorkSearchRefreshForQa(state)) {
        return true;
    }

    constexpr size_t kReadyToWorkMiniRows = 24;

    size_t considered = 0;
    for (const auto& record : state.searchResults) {
        if (record.filePath.empty()) continue;
        if (!folderPathScope.empty() &&
            !IsReadyToWorkFileUnderFolder(record.filePath, folderPathScope)) {
            continue;
        }

        ++considered;
        if (state.waveformFailed.count(record.filePath) > 0 ||
            state.waveformMiniCacheMiss.count(record.filePath) > 0) {
            if (considered >= kReadyToWorkMiniRows) break;
            continue;
        }

        auto cacheIt = state.waveformCache.find(record.filePath);
        if (cacheIt == state.waveformCache.end() || !cacheIt->second.isReady) {
            state.addPendingWaveform(record.filePath, true);
            return true;
        }

        if (considered >= kReadyToWorkMiniRows) break;
    }

    return false;
}

static void RecomputeScanReadyToWorkCoverPending(AppState& state) {
    std::lock_guard<std::recursive_mutex> lk(state.scanReadyToWorkMutex);
    state.scanReadyToWorkCoversPending =
        state.scanReadyToWorkCoverPostProcessPending ||
        state.scanReadyToWorkCoverFirstScreenPending;
}

static bool IsRealCoverWallItemCandidateForQa(const CoverModule::CoverItem& item) {
    if (item.key.empty() || item.isPlaceholder) return false;
    if (item.kind == CoverModule::CoverKind::EmbeddedCover) {
        return item.key.rfind("E|", 0) == 0 && !item.filePathUtf8.empty();
    }
    if (item.key.rfind("F|", 0) == 0) {
        return !item.folderPathUtf8.empty() || !item.folderCoverSrcPathUtf8.empty();
    }
    if (item.key.rfind("?|", 0) == 0) {
        return !item.folderPathUtf8.empty();
    }
    return false;
}

static void SanitizeCoverWallItemsForQa(std::vector<CoverModule::CoverItem>& items) {
    if (items.empty()) return;
    items.erase(std::remove_if(items.begin(),
                               items.end(),
                               [](const CoverModule::CoverItem& item) {
                                   return !IsRealCoverWallItemCandidateForQa(item) ||
                                          item.key == "\1" ||
                                          item.key == "E|\1" ||
                                          item.key == "F|\1";
                               }),
                items.end());
    (void)CoverModule::RemoveFailedCoverItems(items);
}

static void BuildReadyToWorkFirstScreenCoverItemsForQa(
    const std::vector<AudioRecord>& records,
    std::vector<CoverModule::CoverItem>& outItems) {
    outItems.clear();
    if (records.empty()) return;

    constexpr size_t kMaxRecordsForCoverBuildOnUiThread = 50000;
    if (records.size() > kMaxRecordsForCoverBuildOnUiThread) return;

    std::unordered_map<std::string, size_t> keyToIdx;
    keyToIdx.reserve((std::min)(records.size(), size_t(65536)));

    for (const auto& record : records) {
        std::string folderKey = record.folderCoverKey;
        std::string folderCoverPath = record.folderCoverSrcPath;

        if (folderKey.empty() && !record.folderPath.empty()) {
            CoverModule::TryGetFolderCoverKeyFast(
                record.folderPath,
                folderKey,
                &folderCoverPath);
        }
        if (!folderKey.empty()) {
            folderKey = CoverModule::CanonicalCoverKey(folderKey);
        }

        const bool allowTemporaryFolderCover =
            folderKey.empty() &&
            record.embeddedCoverKey.empty() &&
            record.hasCover &&
            !record.folderPath.empty();
        const std::string tempFolderKey = allowTemporaryFolderCover
            ? std::string("?|") + HarkenSearch::CanonicalFolderPath(record.folderPath)
            : std::string();

        if (!folderKey.empty() || !tempFolderKey.empty()) {
            const std::string& wallKey = !folderKey.empty() ? folderKey : tempFolderKey;
            auto [it, inserted] = keyToIdx.emplace(wallKey, outItems.size());
            if (inserted) {
                CoverModule::CoverItem item;
                item.kind = CoverModule::CoverKind::FolderCover;
                item.key = wallKey;
                item.folderPathUtf8 = record.folderPath;
                item.folderCoverSrcPathUtf8 = folderCoverPath;
                item.hitCount = 1;
                item.label = !record.folderPath.empty()
                    ? BaseNameFromUtf8PathForQa(record.folderPath)
                    : std::string();
                outItems.push_back(std::move(item));
            } else {
                outItems[it->second].hitCount++;
            }
        }

        if (!record.embeddedCoverKey.empty()) {
            std::string embeddedKey =
                CoverModule::CanonicalCoverKey(record.embeddedCoverKey);
            if (!embeddedKey.empty()) {
                auto [it, inserted] = keyToIdx.emplace(embeddedKey, outItems.size());
                if (inserted) {
                    CoverModule::CoverItem item;
                    item.kind = CoverModule::CoverKind::EmbeddedCover;
                    item.key = embeddedKey;
                    item.filePathUtf8 = record.filePath;
                    item.hitCount = 1;
                    item.label = BaseNameFromUtf8PathForQa(record.filePath);
                    outItems.push_back(std::move(item));
                } else {
                    outItems[it->second].hitCount++;
                }
            }
        }
    }

    SanitizeCoverWallItemsForQa(outItems);
}

static void BuildStartupFirstScreenCoverItemsFromFacetsForQa(
    const std::vector<Database::CoverFacetEntry>& facets,
    std::vector<CoverModule::CoverItem>& outItems,
    size_t maxItems) {
    outItems.clear();
    if (facets.empty() || maxItems == 0) return;

    outItems.reserve((std::min)(facets.size(), maxItems));
    std::unordered_set<std::string> seenFacetKeys;
    seenFacetKeys.reserve((std::min)(facets.size(), maxItems * 2));

    for (const auto& f : facets) {
        CoverModule::CoverItem item;
        std::string normalizedKey = f.key;
        if (!normalizedKey.empty() && normalizedKey.rfind("?|", 0) != 0) {
            normalizedKey = CoverModule::CanonicalCoverKey(normalizedKey);
            if (normalizedKey.empty()) continue;
        }
        if (!seenFacetKeys.insert(normalizedKey).second) continue;

        item.key = normalizedKey;
        item.hitCount = f.count;
        if (f.key.rfind("?|", 0) == 0) {
            item.kind = CoverModule::CoverKind::FolderCover;
            item.folderPathUtf8 = f.path;
            item.label = BaseNameFromUtf8PathForQa(f.path);
        } else {
            item.kind = f.isEmbedded ? CoverModule::CoverKind::EmbeddedCover
                                     : CoverModule::CoverKind::FolderCover;
            if (f.isEmbedded) {
                item.filePathUtf8 = f.path;
                if (f.sourcePath.rfind("PHASH|", 0) == 0) {
                    item.folderCoverSrcPathUtf8 = f.sourcePath;
                }
            } else {
                item.folderPathUtf8 = f.path;
                item.folderCoverSrcPathUtf8 = f.sourcePath;
            }
            item.label = BaseNameFromUtf8PathForQa(f.path);
        }
        outItems.push_back(std::move(item));
        if (outItems.size() >= maxItems) break;
    }

    SanitizeCoverWallItemsForQa(outItems);
}

static bool CoverWallHasCurrentTextureMirrorForQa(const CoverWallState& coverWall,
                                                  const CoverModule::CoverItem& item) {
    auto texIt = coverWall.texCache.find(item.key);
    return texIt != coverWall.texCache.end() &&
           texIt->second.texId != 0 &&
           texIt->second.nonCoverTextureEpoch == coverWall.nonCoverTextureEpoch;
}

static bool CoverWallFailureCoolingDownForQa(const CoverWallState& coverWall,
                                             const std::string& key,
                                             uint64_t frame) {
    constexpr uint64_t kCoverWallFailCooldownFrames = 600;
    constexpr double kCoverWallFailCooldownSeconds = 10.0;
    const double now = ImGui::GetTime();
    auto it = coverWall.failedTexCache.find(key);
    return it != coverWall.failedTexCache.end() &&
           ((frame - it->second.lastAttemptFrame) < kCoverWallFailCooldownFrames ||
            (now - it->second.lastAttemptTimeSec) < kCoverWallFailCooldownSeconds);
}

static void CoverWallRecordFailureForQa(CoverWallState& coverWall,
                                        const std::string& key,
                                        uint64_t frame) {
    auto& fail = coverWall.failedTexCache[key];
    fail.lastAttemptFrame = frame;
    fail.lastAttemptTimeSec = ImGui::GetTime();
    fail.failures++;
}

static void CoverWallRecordSuccessForQa(CoverWallState& coverWall,
                                        const std::string& key) {
    coverWall.failedTexCache.erase(key);
}

static bool CoverWallShouldRecordConfirmedFailureForQa(const CoverModule::CoverItem& item) {
    return CoverModule::IsCoverKeyFailed(item);
}

static bool CoverWallReadyToWorkCandidateSatisfiedForQa(
    const CoverWallState& coverWall,
    const CoverModule::CoverItem& item,
    uint64_t frame) {
    return CoverWallHasCurrentTextureMirrorForQa(coverWall, item) ||
           CoverWallFailureCoolingDownForQa(coverWall, item.key, frame) ||
           CoverWallShouldRecordConfirmedFailureForQa(item);
}

static bool StartupCoverSourceReadyForQa(const AppState& state) {
    if (!state.currentCoverFacets.empty()) return true;
    return !state.searchResults.empty() &&
           state.pendingSearchRequestId == 0 &&
           state.pendingFacetRequestId == 0 &&
           !state.asyncSearchInProgress;
}

static bool WaitForStartupCoverSourceForQa(
    AppState& state,
    const std::function<void()>& pumpUi,
    std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (StartupCoverSourceReadyForQa(state)) return true;

        (void)PumpOneAsyncSearchResult(state);

        if (StartupCoverSourceReadyForQa(state)) return true;
        if (pumpUi) pumpUi();
        Platform::SleepMilliseconds(16);
    }
    return StartupCoverSourceReadyForQa(state);
}

static void WarmStartupFirstScreenCoversForQa(
    AppState& state,
    const std::function<void()>& pumpUi) {
    constexpr size_t kStartupCoverCandidatePoolCount = 4096;
    constexpr size_t kStartupFirstScreenCoverCount = 24;
    constexpr size_t kStartupVisibleCoverTarget = 12;
    constexpr int kStartupCoverRequestsPerFrame = 8;
    constexpr int kStartupCoverUploadsPerFrame = 8;
    constexpr int kStartupCoverUploadBudgetUs = 3000;
    constexpr auto kStartupFirstScreenCoverDeadline = std::chrono::milliseconds(2800);

    std::vector<CoverModule::CoverItem> candidatePool;
    BuildStartupFirstScreenCoverItemsFromFacetsForQa(
        state.currentCoverFacets,
        candidatePool,
        kStartupCoverCandidatePoolCount);
    if (candidatePool.empty() && !state.searchResults.empty()) {
        std::vector<AudioRecord> candidateResults;
        candidateResults.reserve((std::min)(state.searchResults.size(), size_t(128)));
        for (size_t i = 0; i < state.searchResults.size() && i < 128; ++i) {
            candidateResults.push_back(state.searchResults[i]);
        }
        BuildReadyToWorkFirstScreenCoverItemsForQa(candidateResults, candidatePool);
        if (candidatePool.size() > kStartupCoverCandidatePoolCount) {
            candidatePool.resize(kStartupCoverCandidatePoolCount);
        }
    }

    const auto preloadStarted = std::chrono::steady_clock::now();
    size_t readyCount = 0;
    size_t activeCandidateCount = 0;
    bool timedOut = false;
    std::unordered_map<std::string, uint64_t> retryAfterFrame;
    if (!candidatePool.empty()) {
        while (true) {
            if (pumpUi) pumpUi();
            CoverModule::PumpCoverTextureUploads(kStartupCoverUploadsPerFrame,
                                                 kStartupCoverUploadBudgetUs);

            const uint64_t frame = (uint64_t)ImGui::GetFrameCount();
            std::vector<CoverModule::CoverItem> activeCandidates;
            activeCandidates.reserve(kStartupFirstScreenCoverCount);
            for (const auto& item : candidatePool) {
                if (CoverWallShouldRecordConfirmedFailureForQa(item)) continue;
                auto retryIt = retryAfterFrame.find(item.key);
                if (retryIt != retryAfterFrame.end() &&
                    frame < retryIt->second &&
                    !CoverWallHasCurrentTextureMirrorForQa(state.coverWall, item)) {
                    continue;
                }
                activeCandidates.push_back(item);
                if (activeCandidates.size() >= kStartupFirstScreenCoverCount) break;
            }
            activeCandidateCount = activeCandidates.size();
            if (activeCandidates.empty()) break;

            readyCount = 0;
            int requests = 0;
            for (const auto& item : activeCandidates) {
                if (CoverWallReadyToWorkCandidateSatisfiedForQa(
                        state.coverWall,
                        item,
                        frame)) {
                    ++readyCount;
                    continue;
                }
                if (requests >= kStartupCoverRequestsPerFrame) continue;

                CoverModule::TextureHandle tex;
                const bool ok = CoverModule::GetCoverTextureByKey(item, tex, false);
                ++requests;
                if (tex.textureId != 0) {
                    state.coverWall.texCache[item.key] = {
                        tex.textureId,
                        tex.width,
                        tex.height,
                        frame,
                        state.coverWall.nonCoverTextureEpoch
                    };
                    CoverWallRecordSuccessForQa(state.coverWall, item.key);
                    retryAfterFrame.erase(item.key);
                    ++readyCount;
                } else if (!ok && CoverWallShouldRecordConfirmedFailureForQa(item)) {
                    CoverWallRecordFailureForQa(state.coverWall, item.key, frame);
                    retryAfterFrame.erase(item.key);
                } else if (!ok) {
                    retryAfterFrame[item.key] = frame + 6;
                }
            }

            const size_t readyTarget = (std::min)(kStartupVisibleCoverTarget,
                                                  activeCandidates.size());
            if (readyCount >= readyTarget) break;
            if (std::chrono::steady_clock::now() - preloadStarted >=
                kStartupFirstScreenCoverDeadline) {
                timedOut = true;
                break;
            }
            Platform::SleepMilliseconds(16);
        }
    }

    if (pumpUi) pumpUi();
    CoverModule::PumpCoverTextureUploads(kStartupCoverUploadsPerFrame,
                                         kStartupCoverUploadBudgetUs);
    const auto preloadEnded = std::chrono::steady_clock::now();
    const double elapsedMs =
        std::chrono::duration<double, std::milli>(preloadEnded - preloadStarted).count();
    DIAG_SYSTEM_FMT("CoverWall",
                    "StartupFirstScreenPreloadPrimed candidates=%zu mirrored=%zu budgetElapsed=%d elapsedMs=%.2f",
                    activeCandidateCount,
                    readyCount,
                    timedOut ? 1 : 0,
                    elapsedMs);
}

static void EnsureReadyToWorkFirstScreenCoverCandidatesForQa(AppState& state) {
    std::lock_guard<std::recursive_mutex> lk(state.scanReadyToWorkMutex);
    if (!state.scanReadyToWorkCoverFirstScreenPending) return;
    if (!state.coverWall.preloadFirstPage.empty()) return;

    constexpr size_t kCandidateSourceRows = 128;
    constexpr size_t kFirstScreenCandidateCount = 24;

    if (!state.searchResults.empty()) {
        std::vector<AudioRecord> candidateResults;
        candidateResults.reserve((std::min)(state.searchResults.size(), kCandidateSourceRows));
        for (size_t i = 0; i < state.searchResults.size() && i < kCandidateSourceRows; ++i) {
            candidateResults.push_back(state.searchResults[i]);
        }
        BuildReadyToWorkFirstScreenCoverItemsForQa(
            candidateResults,
            state.coverWall.preloadFirstPage);
        if (state.coverWall.preloadFirstPage.size() > kFirstScreenCandidateCount) {
            state.coverWall.preloadFirstPage.resize(kFirstScreenCandidateCount);
        }
        return;
    }

    if (!state.coverWall.preloadFullList.empty()) {
        const size_t count = (std::min)(state.coverWall.preloadFullList.size(),
                                        kFirstScreenCandidateCount);
        state.coverWall.preloadFirstPage.assign(
            state.coverWall.preloadFullList.begin(),
            state.coverWall.preloadFullList.begin() + count);
    }
}

static void PumpScanReadyToWorkFirstScreenCoversForQa(AppState& state) {
    std::lock_guard<std::recursive_mutex> lk(state.scanReadyToWorkMutex);
    if (!state.scanReadyToWorkCoverFirstScreenPending) return;
    EnsureReadyToWorkFirstScreenCoverCandidatesForQa(state);

    auto& candidates = state.coverWall.preloadFirstPage;
    if (candidates.empty()) {
        if (HasPendingReadyToWorkCoverSourceRefreshForQa(state)) {
            return;
        }
        state.scanReadyToWorkCoverFirstScreenPending = false;
        state.scanReadyToWorkCoverFirstScreenCursor = 0;
        RecomputeScanReadyToWorkCoverPending(state);
        return;
    }

    const double now = ImGui::GetTime();
    ImGuiIO& io = ImGui::GetIO();
    BackgroundIoContext bgIo;
    bgIo.coverWallPriorityMode = false;
    bgIo.playing = state.audioEngine.isPlaying();
    bgIo.waveformRebuildActive = state.waveformRebuildActive;
    bgIo.scanning = state.isScanning.load(std::memory_order_acquire);
    bgIo.dbSwitchInProgress = state.dbSwitchInProgress;
    bgIo.dbSwitchSettling = state.dbSwitchSettleFramesRemaining > 0;
    bgIo.loadFilePending = state.audioEngine.isLoadFilePending();
    bgIo.detailPending = state.audioEngine.isWaveformDetailPending();
    bgIo.detailDeferring = !state.pendingDetailFilePath.empty();
    bgIo.modeSwitchCoolingDown = state.lastProductModeSwitchTime > 0.0 &&
        (now - state.lastProductModeSwitchTime) < 2.5;
    bgIo.mouseIdle = std::abs(io.MouseDelta.x) <= 0.0f &&
                     std::abs(io.MouseDelta.y) <= 0.0f &&
                     !ImGui::IsAnyMouseDown();
    bgIo.userActive = ImGui::IsAnyMouseDown();
    bgIo.coverWarmup = true;
    if (state.lastSelectionOrLoadTime > 0.0) {
        bgIo.secondsSinceSelectionOrLoad = now - state.lastSelectionOrLoadTime;
    }
    const auto coverBudget = ResolveCoverWallBackgroundBudget(bgIo);
    if (coverBudget.maxTextureUploads > 0) {
        CoverModule::PumpCoverTextureUploads(coverBudget.maxTextureUploads,
                                             coverBudget.budgetMicroseconds);
    }
    if (!coverBudget.allowBackgroundRequests) return;

    const uint64_t frame = (uint64_t)ImGui::GetFrameCount();
    if (state.coverWall.failedTexCache.size() > 4096) {
        state.coverWall.failedTexCache.clear();
    }

    size_t readyCount = 0;
    for (const auto& item : candidates) {
        if (CoverWallReadyToWorkCandidateSatisfiedForQa(
                state.coverWall,
                item,
                frame)) {
            ++readyCount;
        }
    }
    if (readyCount >= candidates.size()) {
        state.scanReadyToWorkCoverFirstScreenPending = false;
        state.scanReadyToWorkCoverFirstScreenCursor = 0;
        RecomputeScanReadyToWorkCoverPending(state);
        DIAG_SYSTEM_FMT("Scan",
                        "ReadyToWorkCoverFirstScreenSatisfied: db=%s ready=%zu",
                        state.scanReadyToWorkDbPath.c_str(),
                        readyCount);
        return;
    }

    if (state.scanReadyToWorkCoverFirstScreenCursor >= candidates.size()) {
        state.scanReadyToWorkCoverFirstScreenCursor = 0;
    }
    const size_t startCursor = state.scanReadyToWorkCoverFirstScreenCursor;
    const int maxRequests = (std::max)(0, coverBudget.maxRequestsPerFrame);
    int requests = 0;
    for (size_t step = 0; step < candidates.size() && requests < maxRequests; ++step) {
        const size_t idx = (startCursor + step) % candidates.size();
        const auto& item = candidates[idx];
        if (CoverWallReadyToWorkCandidateSatisfiedForQa(
                state.coverWall,
                item,
                frame)) {
            continue;
        }
        CoverModule::TextureHandle tex;
        const bool ok = CoverModule::GetCoverTextureByKey(item, tex, false);
        requests++;
        if (tex.textureId != 0) {
            state.coverWall.texCache[item.key] = {
                tex.textureId,
                tex.width,
                tex.height,
                frame,
                state.coverWall.nonCoverTextureEpoch
            };
            CoverWallRecordSuccessForQa(state.coverWall, item.key);
        } else if (!ok && CoverWallShouldRecordConfirmedFailureForQa(item)) {
            CoverWallRecordFailureForQa(state.coverWall, item.key, frame);
        }
        state.scanReadyToWorkCoverFirstScreenCursor = (idx + 1) % candidates.size();
    }
}

static bool ScanReadyToWorkPendingAnyForQa(const AppState& state) {
    std::lock_guard<std::recursive_mutex> lk(state.scanReadyToWorkMutex);
    return state.scanReadyToWorkWaveformsPending ||
           state.scanReadyToWorkCoversPending ||
           state.scanReadyToWorkMainWavePending;
}

static bool ScanCompletionPendingAnyForQa(const AppState& state) {
    return state.scanCompletionWaveformsPending.load(std::memory_order_acquire) ||
           state.scanCompletionCoversPending.load(std::memory_order_acquire) ||
           state.scanCompletionMetadataUnresolved.load(std::memory_order_acquire) > 0;
}

struct ScanReadyToWorkSnapshot {
    bool pending = false;
    bool waveformsPending = false;
    bool coversPending = false;
    bool coverPostPending = false;
    bool coverFirstScreenPending = false;
    bool mainWavePending = false;
};

static ScanReadyToWorkSnapshot GetScanReadyToWorkSnapshotForQa(const AppState& state) {
    std::lock_guard<std::recursive_mutex> lk(state.scanReadyToWorkMutex);
    ScanReadyToWorkSnapshot snapshot;
    snapshot.waveformsPending = state.scanReadyToWorkWaveformsPending;
    snapshot.coverPostPending = state.scanReadyToWorkCoverPostProcessPending;
    snapshot.coverFirstScreenPending = state.scanReadyToWorkCoverFirstScreenPending;
    snapshot.coversPending =
        snapshot.coverPostPending || snapshot.coverFirstScreenPending;
    snapshot.mainWavePending = state.scanReadyToWorkMainWavePending;
    snapshot.pending =
        snapshot.waveformsPending ||
        snapshot.coversPending ||
        snapshot.mainWavePending;
    return snapshot;
}

static void CommitScanReadyToWorkStateForQa(AppState& state) {
    std::lock_guard<std::recursive_mutex> lk(state.scanReadyToWorkMutex);
    const bool pendingBefore = state.scanReadyToWorkPending;
    state.scanReadyToWorkPending = ScanReadyToWorkPendingAnyForQa(state);
    if (pendingBefore != state.scanReadyToWorkPending &&
        !state.scanReadyToWorkPending &&
        state.scanTotal.load() > 0) {
        DIAG_SYSTEM_FMT("Scan",
                        "ReadyToWorkSatisfied: db=%s",
                        state.scanReadyToWorkDbPath.c_str());
    }
}

static void CommitScanCompletionStateForQa(AppState& state) {
    const bool pendingBefore =
        state.scanCompletionPending.load(std::memory_order_acquire);
    const bool pendingNow = ScanCompletionPendingAnyForQa(state);
    state.scanCompletionPending.store(pendingNow, std::memory_order_release);
    state.scanForegroundTasksDrained.store(!pendingNow, std::memory_order_release);
    if (pendingNow) {
        const bool miniPending =
            state.scanMiniWaveformsRequired.load(std::memory_order_acquire) &&
            state.scanMiniWaveformsCompleted.load(std::memory_order_acquire) <
                state.scanMiniWaveformsTotal.load(std::memory_order_acquire);
        state.scanWorkflowStage.store(
            miniPending
                ? HarkenSearch::ScanWorkflowStage::CachingMiniWaveforms
                : HarkenSearch::ScanWorkflowStage::Finalizing,
            std::memory_order_release);
    } else if (state.isScanning.load(std::memory_order_acquire)) {
        state.scanWorkflowStage.store(HarkenSearch::ScanWorkflowStage::Finalizing,
                                      std::memory_order_release);
    }
    if (pendingBefore != pendingNow &&
        !pendingNow &&
        state.scanTotal.load() > 0) {
        state.markScanCompletionReadyNow();
        state.scanSummaryToastUntil = ImGui::GetTime() + 10.0;
        DIAG_SYSTEM_FMT("Scan",
                        "ScanCompletionSatisfied: session=%lld durationMs=%lld",
                        (long long)state.scanCompletionSessionId.load(std::memory_order_acquire),
                        (long long)state.scanCompletionDurationMs.load(std::memory_order_acquire));
    }
}

static void MarkScanReadyToWorkCoversSatisfiedForQa(AppState& state) {
    std::lock_guard<std::recursive_mutex> lk(state.scanReadyToWorkMutex);
    if (!state.scanReadyToWorkCoverPostProcessPending) return;
    state.scanReadyToWorkCoverPostProcessPending = false;
    RecomputeScanReadyToWorkCoverPending(state);
    DIAG_SYSTEM_FMT("Scan",
                    "ReadyToWorkCoversSatisfied: db=%s firstScreenPending=%d",
                    state.scanReadyToWorkDbPath.c_str(),
                    state.scanReadyToWorkCoverFirstScreenPending ? 1 : 0);
    CommitScanReadyToWorkStateForQa(state);
}

static void RefreshScanReadyToWorkStateForQa(AppState& state) {
    std::lock_guard<std::recursive_mutex> lk(state.scanReadyToWorkMutex);
    if (state.isScanning.load(std::memory_order_acquire)) return;
    if (!state.scanReadyToWorkPending) return;
    if (!state.scanReadyToWorkDbPath.empty() &&
        !state.currentDbPathUtf8.empty() &&
        state.scanReadyToWorkDbPath != state.currentDbPathUtf8) {
        state.resetScanReadyToWorkState();
        return;
    }

    bool changed = false;
    if (state.scanReadyToWorkCoverPostProcessPending && state.database.isOpen()) {
        if (!HasOutstandingReadyToWorkCoverPostProcessForQa(
                state,
                state.scanReadyToWorkWaveformScope)) {
            MarkScanReadyToWorkCoversSatisfiedForQa(state);
        }
    }

    if (state.scanReadyToWorkWaveformsPending && state.database.isOpen()) {
        const int waveformKind =
            static_cast<int>(aura::BackfillJobKind::WaveformL1Overview);
        const int pendingRows = state.scanReadyToWorkWaveformScope.empty()
            ? state.database.countBackfillPendingKind(waveformKind)
            : state.database.countBackfillPendingKindInFolder(
                waveformKind,
                state.scanReadyToWorkWaveformScope);
        const bool presentationPending =
            HasOutstandingReadyToWorkMiniWavePresentationForQa(
                state,
                state.scanReadyToWorkWaveformScope);
        if (pendingRows <= 0 && !presentationPending) {
            state.scanReadyToWorkWaveformsPending = false;
            changed = true;
            DIAG_SYSTEM_FMT("Scan",
                            "ReadyToWorkWaveformsSatisfied: db=%s scope=%s",
                            state.scanReadyToWorkDbPath.c_str(),
                            state.scanReadyToWorkWaveformScope.empty()
                                ? "<all>"
                                : state.scanReadyToWorkWaveformScope.c_str());
        }
    }

    if (state.scanReadyToWorkMainWavePending) {
        const std::string currentMainWavePath =
            GetReadyToWorkCurrentMainWavePathForQa(
                state,
                state.scanReadyToWorkWaveformScope);
        if (currentMainWavePath.empty()) {
            state.scanReadyToWorkMainWavePending = false;
            state.scanReadyToWorkMainWavePath.clear();
            changed = true;
        } else {
            state.scanReadyToWorkMainWavePath = currentMainWavePath;
            if (HasUsableReadyToWorkMainWaveformForQa(state, currentMainWavePath)) {
                state.scanReadyToWorkMainWavePending = false;
                changed = true;
                DIAG_SYSTEM_FMT("Scan",
                                "ReadyToWorkMainWaveSatisfied: db=%s path=%s",
                                state.scanReadyToWorkDbPath.c_str(),
                                currentMainWavePath.c_str());
            } else if (state.pendingDetailFilePath.empty() &&
                       !state.audioEngine.isWaveformDetailPending()) {
                ScheduleDeferredWaveformDetail(
                    state,
                    currentMainWavePath,
                    AppState::WAVEFORM_DEFAULT_DETAIL_PEAKS);
                DIAG_ACTION_FMT("Waveform",
                                "ReadyToWorkMainWaveRearmed: path=%s",
                                currentMainWavePath.c_str());
            }
        }
    }

    if (changed) {
        CommitScanReadyToWorkStateForQa(state);
    }
}

static void RefreshScanCompletionStateForQa(AppState& state) {
    if (state.isScanning.load(std::memory_order_acquire) &&
        !state.scanCompletionPending.load(std::memory_order_acquire)) {
        return;
    }
    if (!state.scanCompletionPending.load(std::memory_order_acquire)) return;
    const int64_t scanSessionId =
        state.scanCompletionSessionId.load(std::memory_order_acquire);
    if (scanSessionId <= 0 || !state.database.isOpen()) return;

    const ScanForegroundCompletionCounts counts =
        state.database.getScanForegroundCompletionCounts(scanSessionId);
    if (counts.total <= 0) return;
    {
        static int64_t s_throughputSessionId = 0;
        static int s_lastMiniUnresolved = 0;
        static int s_lastTerminalUnits = 0;
        static auto s_lastThroughputLog = std::chrono::steady_clock::now();
        const auto now = std::chrono::steady_clock::now();
        const int totalTerminalUnits = std::max(0, counts.total * 3);
        const int terminalUnits = std::clamp(
            counts.miniTerminal + counts.coverTerminal + counts.metadataTerminal,
            0,
            totalTerminalUnits);
        if (s_throughputSessionId != scanSessionId) {
            s_throughputSessionId = scanSessionId;
            s_lastMiniUnresolved = counts.miniUnresolved;
            s_lastTerminalUnits = terminalUnits;
            s_lastThroughputLog = now;
        }
        const double elapsedSec =
            std::chrono::duration<double>(now - s_lastThroughputLog).count();
        if (elapsedSec >= 1.0 || counts.pendingFiles == 0) {
            const int terminalUnitDelta =
                std::max(0, terminalUnits - s_lastTerminalUnits);
            const double terminalFileRate = elapsedSec > 0.0
                ? (static_cast<double>(terminalUnitDelta) / 3.0) / elapsedSec
                : 0.0;
            const int miniDoneDelta =
                std::max(0, s_lastMiniUnresolved - counts.miniUnresolved);
            const double miniDoneRate = elapsedSec > 0.0
                ? static_cast<double>(miniDoneDelta) / elapsedSec
                : 0.0;
            char detail[384];
            std::snprintf(
                detail,
                sizeof(detail),
                "ScanCompletionThroughput: session=%lld total=%d pendingFiles=%d miniUnresolved=%d coverUnresolved=%d metadataUnresolved=%d terminalUnitDelta=%d terminalFileRate=%.2f miniDoneDelta=%d miniDoneRate=%.2f",
                (long long)scanSessionId,
                counts.total,
                counts.pendingFiles,
                counts.miniUnresolved,
                counts.coverUnresolved,
                counts.metadataUnresolved,
                terminalUnitDelta,
                terminalFileRate,
                miniDoneDelta,
                miniDoneRate);
            DIAG_SYSTEM("Scan", detail);
            s_lastMiniUnresolved = counts.miniUnresolved;
            s_lastTerminalUnits = terminalUnits;
            s_lastThroughputLog = now;
        }
    }

    const bool waveformCompletionRequired = state.settings.cacheWaveformsOnScan;
    const int effectiveMiniUnresolved =
        waveformCompletionRequired ? counts.miniUnresolved : 0;
    const bool coversPending = counts.coverUnresolved > 0;
    const bool waveformsPending = effectiveMiniUnresolved > 0;
    const bool coversBefore =
        state.scanCompletionCoversPending.load(std::memory_order_acquire);
    const bool waveformsBefore =
        state.scanCompletionWaveformsPending.load(std::memory_order_acquire);
    const bool metadataBefore =
        state.scanCompletionMetadataUnresolved.load(std::memory_order_acquire) > 0;
    const bool metadataPending = counts.metadataUnresolved > 0;

    state.scanCompletionTotal.store(counts.total, std::memory_order_release);
    state.scanCompletionPendingFiles.store(counts.pendingFiles, std::memory_order_release);
    state.scanCompletionMiniUnresolved.store(effectiveMiniUnresolved, std::memory_order_release);
    state.scanCompletionCoverUnresolved.store(counts.coverUnresolved, std::memory_order_release);
    state.scanCompletionMetadataUnresolved.store(counts.metadataUnresolved, std::memory_order_release);
    state.setScanMiniWaveformProgress(
        waveformCompletionRequired ? counts.miniTerminal : 0,
        waveformCompletionRequired ? counts.total : 0);
    state.scanCompletionCoversPending.store(coversPending, std::memory_order_release);
    state.scanCompletionWaveformsPending.store(waveformsPending, std::memory_order_release);

    if (coversBefore && !coversPending) {
            DIAG_SYSTEM_FMT("Scan",
                            "ScanCompletionCoversSatisfied: session=%lld total=%d",
                            (long long)scanSessionId,
                            counts.total);
    }
    if (waveformsBefore && !waveformsPending) {
            DIAG_SYSTEM_FMT("Scan",
                            "ScanCompletionWaveformsSatisfied: session=%lld total=%d",
                            (long long)scanSessionId,
                            counts.total);
    }
    if (metadataBefore && !metadataPending) {
            DIAG_SYSTEM_FMT("Scan",
                            "ScanCompletionMetadataSatisfied: session=%lld total=%d",
                            (long long)scanSessionId,
                            counts.total);
    }

    if (coversBefore != coversPending ||
        waveformsBefore != waveformsPending ||
        metadataBefore != metadataPending) {
        CommitScanCompletionStateForQa(state);
    }
}

static bool BeginFolderScan(
    AppState& state,
    const std::string& path,
    bool recursive,
    bool updateExisting,
    bool removeMissing,
    bool cacheWaveforms,
    int workerThreads
) {
    if (path.empty() || !PathExistsUtf8(path) || state.isScanning.load()) {
        return false;
    }

    if (path.size() < sizeof(state.browserPathBuffer)) {
        std::snprintf(state.browserPathBuffer, sizeof(state.browserPathBuffer), "%s", path.c_str());
    }

    if (Platform::FileSystem::IsNetworkPath(path)) {
        removeMissing = false;
    }
     state.scanRecursive = recursive;
    state.scanUpdateExisting = updateExisting;
    state.scanRemoveMissing = removeMissing;
    state.scanWorkerThreads = std::clamp(workerThreads, 0, 64);
    state.settings.cacheWaveformsOnScan = cacheWaveforms;
    state.audioEngine.stop();
    state.searchWorker.stop();
    state.waveformWorker.stop();

    std::vector<std::string> foldersToScan = { path };
    state.isScanning = true;
    state.scanProgress = 0;
    state.scanTotal = 0;
    state.setScanFile(L(Wf_CountingFiles));
    state.scanAddedCount = 0;
    state.scanUpdatedCount = 0;
    state.scanSkippedCount = 0;
    state.scanRemovedCount = 0;
    state.scanErrorCount = 0;
    state.scanIngestedCount = 0;
    state.scanIngestDurationMs = 0;
    state.clearScanErrors();
    state.scanCancelRequested = false;
    state.scanWasCancelled = false;
    state.scanWasPaused = false;
    state.lastIncrementalRefreshTime = 0.0;
    state.resetScanReadyToWorkState();
    state.autoFilterToScannedPath = true;
    state.lastScannedPath = path;
    state.needsDatabaseRefresh = false;
    state.scanDataCommitted = false;
    state.scanDialogAutoStart = false;
    state.showScanDialog = true;
    state.beginScanWorkflow(cacheWaveforms);

    const uint64_t scanGeneration = state.beginNewScanTask();
    std::shared_ptr<std::atomic<bool>> scanCancelToken = state.getActiveScanCancelToken();
    bool precomputeWaveforms = cacheWaveforms;
    const std::string waveformFolderScope =
        foldersToScan.size() == 1
            ? HarkenSearch::CanonicalFolderPath(foldersToScan.front())
            : std::string();
    const std::string scanDbPath = !state.currentDbPathUtf8.empty()
        ? state.currentDbPathUtf8
        : state.database.getMainDbPath();
    const std::string backfillDbPathSnapshot = state.backfillQueue.currentDbPath();
    const int64_t backfillDbGenerationSnapshot = state.backfillQueue.currentGeneration();
    DIAG_SYSTEM_FMT("Scan", "ScanStart: %zu folders", foldersToScan.size());
    const bool buildSimilarityIndexAfterScan = state.scanBuildSimilarityIndexAfterScan;
    state.scanThread = std::thread([&state, foldersToScan, recursive, updateExisting, removeMissing, workerThreads, precomputeWaveforms, buildSimilarityIndexAfterScan, waveformFolderScope, scanGeneration, scanCancelToken, scanDbPath, backfillDbPathSnapshot, backfillDbGenerationSnapshot]() {
      try {
        Database scanDb;
        const bool scanDbOpen = !scanDbPath.empty() && scanDb.open(scanDbPath);
        if (!scanDbOpen) {
            const std::string err = scanDb.getLastError().empty()
                ? std::string("open scan database failed")
                : scanDb.getLastError();
            DIAG_SYSTEM("Scan", aura::FormatScanDatabaseOpenFailure(scanDbPath, err, foldersToScan).c_str());
            if (state.isScanTaskCurrent(scanGeneration)) {
                const std::string shownErr = err;
                state.setScanFileKeyStr(LStr::SCAN_FailedFmt,  // H2b: keyed; arg stays literal-diagnostic
                    shownErr);
                state.addScanError(shownErr);
                state.scanErrorCount = 1;
                state.scanFatalFailure.store(true, std::memory_order_release);
            }
        } else {
        scanDb.mergeLibraryRoots(foldersToScan);
         aura::GlobalBackfillWorkerPool().stop();
         scanDb.beginScanRollbackSession();
        if (!scanDb.resetScanForegroundCompletionSession(static_cast<int64_t>(scanGeneration))) {
            const std::string err = scanDb.getLastError().empty()
                ? std::string("reset scan foreground completion session failed")
                : scanDb.getLastError();
            DIAG_SYSTEM_FMT("Scan",
                            "ScanCompletionSessionResetFailed: session=%lld error=%s",
                            (long long)scanGeneration,
                            err.c_str());
            throw std::runtime_error(err);
        }
        DIAG_SYSTEM_FMT("Scan",
                        "ScanCompletionSessionReset: session=%lld",
                        (long long)scanGeneration);
        if (removeMissing) {
            state.scanPhase.store(AppState::ScanPhase::RemoveMissing);
        } else if (updateExisting) {
            state.scanPhase.store(AppState::ScanPhase::IncrementalScan);
        } else {
            state.scanPhase.store(AppState::ScanPhase::InitialImport);
        }
        auto isCancelled = [&]() -> bool {
            return state.isScanTaskCancelled(scanGeneration, scanCancelToken);
        };
        const int backfillThreads =
            aura::ResolveScanBackfillWorkerThreads(
                workerThreads, std::thread::hardware_concurrency());
        const int backgroundBackfillThreads =
            precomputeWaveforms ? backfillThreads : (std::min)(backfillThreads, 2);
        auto ensureScanForegroundBackfillWorkersStarted = [&](const char* reason) {
            const auto snapshot = state.backfillQueue.snapshot();
            const int queuedBackfillJobs =
                snapshot.byKind[static_cast<int>(aura::BackfillJobKind::CoverResolveAndThumb)] +
                snapshot.byKind[static_cast<int>(aura::BackfillJobKind::WaveformL1Overview)] +
                snapshot.byKind[static_cast<int>(aura::BackfillJobKind::MetadataRefresh)];
            const bool preserveQueuedScanWork =
                queuedBackfillJobs > 0 || snapshot.inFlight > 0;
            if (preserveQueuedScanWork) {
                state.backfillQueue.refillFromDatabaseIfBelowWaterMark(&scanDb, 4096, 4096);
            } else {
                state.backfillQueue.rebuildFromDatabase(&scanDb, 4096);
            }
            DIAG_SYSTEM_FMT("Scan",
                            "ScanForegroundBackfillQueueSeed: session=%lld preserve=%d queued=%d inFlight=%d",
                            (long long)scanGeneration,
                            preserveQueuedScanWork ? 1 : 0,
                            queuedBackfillJobs,
                            snapshot.inFlight);
            aura::GlobalBackfillWorkerPool().start(
                state.backfillQueue.currentDbPath(),
                state.backfillQueue.currentGeneration(),
                &state.backfillQueue,
                backfillThreads);
            DIAG_SYSTEM_FMT("Scan",
                            "ScanForegroundBackfillStart: session=%lld reason=%s strategy=%s",
                            (long long)scanGeneration,
                            reason ? reason : "",
                            precomputeWaveforms ? "full" : "fast");
        };
        ScanStats combined{};
        for (size_t fi = 0; fi < foldersToScan.size(); ++fi) {
            if (isCancelled()) break;
            const std::string& folder = foldersToScan[fi];
            if (state.isScanTaskCurrent(scanGeneration)) {
                state.setScanFileKeyPrefixed(
                    LStr::Wf_CountingFiles,
                    std::string(u8"[") + std::to_string((int)fi + 1) + "/" +
                        std::to_string((int)foldersToScan.size()) + u8"] ");
                state.scanProgress = 0;
                state.scanTotal = 1;
            }

            auto runOne = [&](bool rec) -> ScanStats {
                return scanDb.scanDirectoryEx(folder, rec, updateExisting, removeMissing,
                    [&state, scanGeneration](int current, int total, const std::string& file, bool dataCommitted) {
                        if (!state.isScanTaskCurrent(scanGeneration)) return;
                        state.scanProgress = current;
                        state.scanTotal = total;
                        state.scanDiscoveredCount.store(
                            std::max(0, total), std::memory_order_release);
                        state.scanProcessedCount.store(
                            std::clamp(current, 0, std::max(0, total)),
                            std::memory_order_release);
                        if (total > 0) {
                            state.scanWorkflowStage.store(
                                HarkenSearch::ScanWorkflowStage::Ingesting,
                                std::memory_order_release);
                        }
                        if (file.find("Scan_NetworkLibraryUnreachable") != std::string::npos) {
                            state.setScanFileKey(LStr::Scan_NetworkLibraryUnreachable);
                        } else {
                            state.setScanFile(file);
                        }
                        if (dataCommitted) {
                            state.scanDataCommitted = true;
                        }
                    }, scanCancelToken.get(), workerThreads,
                       /*completeImport=*/false,
                       /*enqueueExtraction=*/precomputeWaveforms,
                       &state.backfillQueue,
                       backfillDbPathSnapshot,
                       backfillDbGenerationSnapshot,
                       static_cast<int64_t>(scanGeneration));
            };  // slice7-B/C/D -- see equivalent comment in main.cpp scan thread.
                // 2026-05-20: fixed arg-position bug from original commit; see main.cpp.

            ScanStats st;
            try {
                st = runOne(recursive);
            } catch (...) { st = {}; st.addError("Scan failed for folder: " + folder); }
            if (!recursive && st.total == 0 && !isCancelled()) {
                if (state.isScanTaskCurrent(scanGeneration)) {
                    state.setScanFileKey(LStr::SCAN_DeepRetry);
                    state.scanProgress = 0;
                    state.scanTotal = 1;
                }
                try {
                    st = runOne(true);
                } catch (...) { st = {}; st.addError("Recursive scan failed for folder: " + folder); }
            }

            combined.mergeFrom(st);
        }

        bool cancelled = isCancelled();
        auto zeroCancelledSummary = [&]() {
            combined.added = 0;
            combined.updated = 0;
            combined.removed = 0;
            combined.ingested = 0;
            combined.ingestDurationMs = 0;
            combined.waveformPendingCreated = 0;
            combined.waveformPendingFixedDone = 0;
            state.scanDataCommitted = false;
        };
        if (cancelled) {
            zeroCancelledSummary();
        }

        if (state.isScanTaskCurrent(scanGeneration)) {
            state.scanTotal = combined.total;
            if (state.scanProgress.load() > state.scanTotal.load()) {
                state.scanProgress = state.scanTotal.load();
            }
            const int processedOutcome = cancelled
                ? std::min(state.scanProcessedCount.load(std::memory_order_acquire),
                           std::max(0, combined.total))
                : std::max(0, combined.total);
            state.setScanOutcome(
                combined.total,
                processedOutcome,
                combined.ingested,
                combined.skipped,
                combined.failedFiles);
            state.scanFatalFailure.store(
                combined.fatalFailure, std::memory_order_release);
            state.scanAddedCount = combined.added;
            state.scanUpdatedCount = combined.updated;
            state.scanRemovedCount = combined.removed;
            state.scanErrorCount = combined.errors;
            state.scanIngestDurationMs = combined.ingestDurationMs;
            state.addScanErrors(combined.errorMessages);
            if (precomputeWaveforms && !cancelled) {
                state.scanWorkflowStage.store(
                    HarkenSearch::ScanWorkflowStage::CachingMiniWaveforms,
                    std::memory_order_release);
                const int waveformKind =
                    static_cast<int>(aura::BackfillJobKind::WaveformL1Overview);
                ensureScanForegroundBackfillWorkersStarted("selected_strategy_full");
                auto snapshot = state.backfillQueue.snapshot();
                const bool scopedWaveformDrain = !waveformFolderScope.empty();
                int pendingRows = scopedWaveformDrain
                    ? scanDb.countBackfillPendingKindInFolder(waveformKind, waveformFolderScope)
                    : scanDb.countBackfillPendingKind(waveformKind);
                int queuedWaveforms = scopedWaveformDrain
                    ? 0
                    : snapshot.byKind[static_cast<int>(aura::BackfillJobKind::WaveformL1Overview)];
                if (pendingRows > 0 || queuedWaveforms > 0) {
                    snapshot = state.backfillQueue.snapshot();
                    pendingRows = scopedWaveformDrain
                        ? scanDb.countBackfillPendingKindInFolder(waveformKind, waveformFolderScope)
                        : scanDb.countBackfillPendingKind(waveformKind);
                    queuedWaveforms = scopedWaveformDrain
                        ? 0
                        : snapshot.byKind[static_cast<int>(aura::BackfillJobKind::WaveformL1Overview)];
                }
                DIAG_SYSTEM_FMT("Scan",
                                "WaveformCacheDrainStart: scoped=%d pending=%d queued=%d scanTotal=%d cacheTotal=%d scope=%s",
                                scopedWaveformDrain ? 1 : 0,
                                pendingRows,
                                queuedWaveforms,
                                combined.total,
                                std::max(0, pendingRows),
                                scopedWaveformDrain ? waveformFolderScope.c_str() : "<all>");
                DIAG_SYSTEM_FMT("Scan",
                            "ScanForegroundWaveformDrain: scoped=%d pending=%d queued=%d scanTotal=%d scope=%s",
                                scopedWaveformDrain ? 1 : 0,
                                pendingRows,
                                queuedWaveforms,
                                combined.total,
                                scopedWaveformDrain ? waveformFolderScope.c_str() : "<all>");
                const int warmDrainBudgetMs =
                    pendingRows <= 64 ? 5000 :
                    pendingRows <= 256 ? 3000 :
                    1500;
                if (pendingRows > 0 || queuedWaveforms > 0) {
                    const bool warmDrainComplete =
                        WaitForScanWaveformBackfillCompletion(
                            state,
                            scanDb,
                            scanGeneration,
                            scanCancelToken,
                            combined.total,
                            waveformFolderScope,
                            warmDrainBudgetMs);
                    if (!warmDrainComplete && !cancelled) {
                    DIAG_SYSTEM_FMT("Scan",
                                    "WaveformCacheDrainDeferred: scoped=%d pending=%d queued=%d scanTotal=%d scope=%s",
                                    scopedWaveformDrain ? 1 : 0,
                                    pendingRows,
                                    queuedWaveforms,
                                    combined.total,
                                    scopedWaveformDrain ? waveformFolderScope.c_str() : "<all>");
                    }
                }
            }
            if (!cancelled && combined.total > 0) {
                state.scanProgress = combined.total;
                state.setScanFileKey(LStr::ScanDone);
            }
            DIAG_SYSTEM_FMT("Scan",
                            "ScanComplete: total=%d added=%d updated=%d skipped=%d removed=%d errors=%d cancelled=%d waveformCacheRequested=%d",
                            combined.total,
                            combined.added,
                            combined.updated,
                            combined.skipped,
                            combined.removed,
                            combined.errors,
                            cancelled ? 1 : 0,
                            precomputeWaveforms ? 1 : 0);
            const int changedRowCount =
                combined.added + combined.updated + combined.removed;
            const ScanForegroundCompletionCounts completionCounts =
                !cancelled
                    ? scanDb.getScanForegroundCompletionCounts(
                        static_cast<int64_t>(scanGeneration))
                    : ScanForegroundCompletionCounts{};
            const int effectiveMiniUnresolved =
                precomputeWaveforms ? completionCounts.miniUnresolved : 0;
            const bool waveformCompletionPending =
                !cancelled &&
                effectiveMiniUnresolved > 0;
            const bool coverCompletionPending =
                !cancelled &&
                completionCounts.coverUnresolved > 0;
            const bool releaseFastCompletionGate =
                HarkenSearch::ShouldReleaseFastScanCompletionGate(
                    combined.total,
                    state.scanProcessedCount.load(std::memory_order_acquire),
                    !precomputeWaveforms);
            const bool coverFirstScreenReadyToWorkPending =
                !cancelled && combined.total > 0;
            const bool miniPresentationReadyToWorkPending =
                !cancelled &&
                precomputeWaveforms &&
                combined.total > 0;
            const std::string currentMainWavePath =
                GetReadyToWorkCurrentMainWavePathForQa(state, waveformFolderScope);
            bool mainWaveReadyToWorkPending = false;
            if (!currentMainWavePath.empty() &&
                !HasUsableReadyToWorkMainWaveformForQa(state, currentMainWavePath)) {
                ScheduleDeferredWaveformDetail(
                    state,
                    currentMainWavePath,
                    AppState::WAVEFORM_DEFAULT_DETAIL_PEAKS);
                mainWaveReadyToWorkPending = true;
            }
            state.scanCompletionSessionId.store(
                static_cast<int64_t>(scanGeneration),
                std::memory_order_release);
            state.scanCompletionWaveformsPending.store(
                waveformCompletionPending,
                std::memory_order_release);
            state.scanCompletionCoversPending.store(
                coverCompletionPending,
                std::memory_order_release);
            if (releaseFastCompletionGate) {
                state.scanCompletionWaveformsPending.store(
                    false, std::memory_order_release);
                state.scanCompletionCoversPending.store(
                    false, std::memory_order_release);
            }
            state.scanCompletionTotal.store(
                completionCounts.total,
                std::memory_order_release);
            state.scanCompletionPendingFiles.store(
                completionCounts.pendingFiles,
                std::memory_order_release);
            state.scanCompletionMiniUnresolved.store(
                effectiveMiniUnresolved,
                std::memory_order_release);
            state.scanCompletionCoverUnresolved.store(
                completionCounts.coverUnresolved,
                std::memory_order_release);
            state.scanCompletionMetadataUnresolved.store(
                completionCounts.metadataUnresolved,
                std::memory_order_release);
            state.scanBackgroundBackfillPhase.store(
                static_cast<int>(AppState::ScanBackgroundBackfillPhase::Idle),
                std::memory_order_release);
            state.scanBackgroundBackfillDeferredWaitMs.store(
                0,
                std::memory_order_release);
            state.scanBackgroundBackfillResumeThreads.store(
                0,
                std::memory_order_release);
            state.scanCompletionPending.store(
                releaseFastCompletionGate
                    ? false
                    : ScanCompletionPendingAnyForQa(state),
                std::memory_order_release);
            if (releaseFastCompletionGate) {
                DIAG_SYSTEM_FMT(
                    "Scan",
                    "ScanFastCompletionReleased: session=%lld processed=%d discovered=%d coverUnresolved=%d",
                    (long long)scanGeneration,
                    state.scanProcessedCount.load(std::memory_order_acquire),
                    combined.total,
                    completionCounts.coverUnresolved);
            }
            state.setScanMiniWaveformProgress(
                precomputeWaveforms ? completionCounts.miniTerminal : 0,
                precomputeWaveforms ? completionCounts.total : 0);
            const bool completionPendingNow =
                state.scanCompletionPending.load(std::memory_order_acquire);
            state.scanForegroundTasksDrained.store(
                !completionPendingNow, std::memory_order_release);
            state.scanWorkflowStage.store(
                waveformCompletionPending
                    ? HarkenSearch::ScanWorkflowStage::CachingMiniWaveforms
                    : HarkenSearch::ScanWorkflowStage::Finalizing,
                std::memory_order_release);
            const bool foregroundBackfillKickNeeded =
                !cancelled &&
                state.scanCompletionPending.load(std::memory_order_acquire) &&
                !precomputeWaveforms;
            if (foregroundBackfillKickNeeded) {
                ensureScanForegroundBackfillWorkersStarted("completion_wait_fast");
            }
            if (!state.scanCompletionPending.load(std::memory_order_acquire) &&
                combined.total > 0 &&
                !cancelled) {
                state.markScanCompletionReadyNow();
            }
            DIAG_SYSTEM_FMT("Scan",
                            "ScanCompletionState: pending=%d waveform=%d covers=%d session=%lld total=%d miniUnresolved=%d coverUnresolved=%d metadataUnresolved=%d durationMs=%lld",
                            state.scanCompletionPending.load(std::memory_order_acquire) ? 1 : 0,
                            state.scanCompletionWaveformsPending.load(std::memory_order_acquire) ? 1 : 0,
                            state.scanCompletionCoversPending.load(std::memory_order_acquire) ? 1 : 0,
                            (long long)scanGeneration,
                            completionCounts.total,
                            effectiveMiniUnresolved,
                            completionCounts.coverUnresolved,
                            completionCounts.metadataUnresolved,
                            (long long)state.scanCompletionDurationMs.load(std::memory_order_acquire));
            {
                std::lock_guard<std::recursive_mutex> lk(state.scanReadyToWorkMutex);
                state.scanReadyToWorkDbPath = scanDbPath;
                state.scanReadyToWorkWaveformScope = waveformFolderScope;
                state.scanReadyToWorkWaveformsPending =
                    waveformCompletionPending ||
                    miniPresentationReadyToWorkPending;
                state.scanReadyToWorkCoverPostProcessPending = coverCompletionPending;
                state.scanReadyToWorkCoverFirstScreenPending =
                    coverFirstScreenReadyToWorkPending;
                state.scanReadyToWorkCoverFirstScreenCursor = 0;
                state.coverWall.preloadFirstPage.clear();
                RecomputeScanReadyToWorkCoverPending(state);
                state.scanReadyToWorkMainWavePending = mainWaveReadyToWorkPending;
                state.scanReadyToWorkMainWavePath = currentMainWavePath;
                state.scanReadyToWorkPending = ScanReadyToWorkPendingAnyForQa(state);
                DIAG_SYSTEM_FMT("Scan",
                                "ReadyToWorkState: pending=%d waveform=%d covers=%d coverPost=%d coverFirstScreen=%d mainWave=%d changedRows=%d scope=%s db=%s",
                                state.scanReadyToWorkPending ? 1 : 0,
                                state.scanReadyToWorkWaveformsPending ? 1 : 0,
                                state.scanReadyToWorkCoversPending ? 1 : 0,
                                state.scanReadyToWorkCoverPostProcessPending ? 1 : 0,
                                state.scanReadyToWorkCoverFirstScreenPending ? 1 : 0,
                                state.scanReadyToWorkMainWavePending ? 1 : 0,
                                changedRowCount,
                                waveformFolderScope.empty() ? "<all>" : waveformFolderScope.c_str(),
                                scanDbPath.c_str());
            }
            const bool scanMutatedCachePackage =
                combined.added > 0 ||
                combined.updated > 0 ||
                combined.removed > 0 ||
                combined.waveformPendingCreated > 0 ||
                combined.waveformPendingFixedDone > 0;
            if (!cancelled && scanMutatedCachePackage) {
                scanDb.checkpointCachePackageBestEffort("scan_complete");
            }
            if (combined.total == 0 && !cancelled) {
                if (combined.unreachable > 0) {
                    state.setScanFileKey(LStr::Scan_NetworkLibraryUnreachable);
                 } else {
                    std::string err = scanDb.getLastError();
                    if (!err.empty()) {
                        state.setScanFileKeyStr(LStr::SCAN_FailedFmt, err);
                        state.addScanError(err);
                    } else {
                        state.setScanFileKey(LStr::Scan_NoAudioFound);
                    }
                 }
             }
        }
        if (!cancelled &&
            state.scanCompletionPending.load(std::memory_order_acquire)) {
            while (state.isScanTaskCurrent(scanGeneration) &&
                   state.scanCompletionPending.load(std::memory_order_acquire) &&
                   !state.isScanTaskCancelled(scanGeneration, scanCancelToken)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            if (state.isScanTaskCancelled(scanGeneration, scanCancelToken)) {
                aura::GlobalBackfillWorkerPool().stop();
                const bool rollbackOk =
                    scanDb.rollbackActiveScanSession(&state.backfillQueue);
                if (!rollbackOk) {
                    combined.addError("Scan cancelled during foreground completion but rollback failed; partial scan changes may remain.");
                }
                cancelled = true;
                zeroCancelledSummary();
                if (state.isScanTaskCurrent(scanGeneration)) {
                    state.scanAddedCount = combined.added;
                    state.scanUpdatedCount = combined.updated;
                    state.scanRemovedCount = combined.removed;
                    state.scanIngestedCount = combined.ingested;
                    state.scanIngestDurationMs = combined.ingestDurationMs;
                    state.scanErrorCount = combined.errors;
                }
                DIAG_SYSTEM("Scan", "Foreground completion cancel detected. Rolled back scan session.");
            }
        }
        if (!cancelled) {
            scanDb.discardScanRollbackSession();
        }
        if (!cancelled &&
            !state.scanCompletionPending.load(std::memory_order_acquire)) {
            if (!precomputeWaveforms) {
                const auto backgroundResumeDeferStartedAt =
                    std::chrono::steady_clock::now();
                const auto backgroundResumeDeferDeadline =
                    backgroundResumeDeferStartedAt + std::chrono::milliseconds(1250);
                bool deferredForReadyToWork = false;
                while (state.isScanTaskCurrent(scanGeneration) &&
                       !state.isScanTaskCancelled(scanGeneration, scanCancelToken)) {
                    const bool readyToWorkPending = [&]() {
                        std::lock_guard<std::recursive_mutex> lk(state.scanReadyToWorkMutex);
                        return state.scanReadyToWorkPending;
                    }();
                    const auto now = std::chrono::steady_clock::now();
                    const double interactIdleSec =
                        std::chrono::duration<double>(now - state.lastInteractTime).count();
                    const bool protectInteractiveWarmup =
                        readyToWorkPending || interactIdleSec < 0.5;
                    if (!protectInteractiveWarmup ||
                        now >= backgroundResumeDeferDeadline) {
                        break;
                    }
                    if (!deferredForReadyToWork) {
                        state.scanBackgroundBackfillPhase.store(
                            static_cast<int>(AppState::ScanBackgroundBackfillPhase::Deferred),
                            std::memory_order_release);
                        DIAG_SYSTEM_FMT("Scan",
                                        "ScanBackgroundBackfillDeferred: session=%lld reason=ready_to_work strategy=fast ready=%d interactIdleMs=%lld",
                                        (long long)scanGeneration,
                                        readyToWorkPending ? 1 : 0,
                                        (long long)std::llround(interactIdleSec * 1000.0));
                    }
                    deferredForReadyToWork = true;
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                }
                if (deferredForReadyToWork) {
                    const auto resumeDeferElapsedMs =
                        std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - backgroundResumeDeferStartedAt)
                            .count();
                    DIAG_SYSTEM_FMT("Scan",
                                    "ScanBackgroundBackfillDeferredSatisfied: session=%lld strategy=fast waitedMs=%lld",
                                    (long long)scanGeneration,
                                    (long long)resumeDeferElapsedMs);
                    state.scanBackgroundBackfillDeferredWaitMs.store(
                        resumeDeferElapsedMs,
                        std::memory_order_release);
                }
            }
            state.backfillQueue.rebuildFromDatabase(&scanDb, 4096);
            state.scanBackgroundBackfillPhase.store(
                static_cast<int>(AppState::ScanBackgroundBackfillPhase::Resumed),
                std::memory_order_release);
            state.scanBackgroundBackfillResumeThreads.store(
                backgroundBackfillThreads,
                std::memory_order_release);
            DIAG_SYSTEM_FMT("Scan",
                            "ScanBackgroundBackfillResume: session=%lld strategy=%s threads=%d",
                            (long long)scanGeneration,
                            precomputeWaveforms ? "full" : "fast",
                            backgroundBackfillThreads);
            aura::GlobalBackfillWorkerPool().start(
                state.backfillQueue.currentDbPath(),
                state.backfillQueue.currentGeneration(),
                &state.backfillQueue,
                backgroundBackfillThreads);
        }
        }
      } catch (...) {
        if (state.isScanTaskCurrent(scanGeneration)) {
            state.setScanFileKey(LStr::SCAN_ThreadFatalWM);
            state.addScanError("Scan thread crashed before completion.");
            state.scanErrorCount = state.scanErrorCount.load() + 1;
            state.scanFatalFailure.store(true, std::memory_order_release);
        }
      }
        state.waveformWorker.start(&state.database, aura::RecommendedWaveformWorkerThreads());
        state.searchWorker.start(&state.database);
        if (state.isScanTaskCurrent(scanGeneration)) {
            const bool cancelled = state.isScanTaskCancelled(scanGeneration, scanCancelToken);
            state.scanWasCancelled = cancelled;
            state.scanPhase.store(AppState::ScanPhase::Idle);
            state.scanProgress.store(
                state.scanProcessedCount.load(std::memory_order_acquire),
                std::memory_order_release);
            state.scanTotal.store(
                state.scanDiscoveredCount.load(std::memory_order_acquire),
                std::memory_order_release);
            state.scanForegroundTasksDrained.store(
                !state.scanCompletionPending.load(std::memory_order_acquire),
                std::memory_order_release);
            state.resolveScanWorkflowTerminalStage(
                cancelled,
                state.scanFatalFailure.load(std::memory_order_acquire));
            const bool fatalFailure =
                state.scanFatalFailure.load(std::memory_order_acquire);
            if (HarkenSearch::ShouldStartPostScanSimilarityIndex(
                    buildSimilarityIndexAfterScan,
                    state.getScanWorkflowSnapshot(),
                    state.scanStopDisposition.load(std::memory_order_acquire),
                    fatalFailure)) {
                state.pendingPostScanSimilarityIndex.store(
                    true, std::memory_order_release);
            }
            state.needsDatabaseRefresh = true;
            if (cancelled) {
                state.scanDataCommitted = false;
                state.resetScanReadyToWorkState();
            }
            DiagnosticsLogger::instance().logPerformance("Scan", "refreshAfterCommit", 0.0);
            state.isScanning = false;
        }
    });

    return true;
}

#if AURA_BUILD_DIAGNOSTICS && !AURA_INTERNAL_BETA
struct FrameBudgetColdMiniArm {
    bool holding = false, armed = false, captureStarted = false, workerStarted = false, seedSubmitted = false;
    bool savedIdleBackfill = false, priorBackfillRunning = false;
    std::string runtimeRoot, dbPath, activeDbPath, mainDbPath, queueDbPath;
    int64_t generation = 0;
    int expectedFiles = 0;
};
static FrameBudgetColdMiniArm s_frameBudgetColdMini;
static std::string CanonicalColdMiniPath(std::string value) {
    try { value = Platform::FileSystem::WeaklyCanonical(value); }
    catch (...) { return {}; }
    if (value.empty()) return {};
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return c == '/' ? '\\' : (char)std::tolower(c);
    });
    while (value.size() > 3 && value.back() == '\\') value.pop_back();
    return value;
}
static bool ColdMiniArmMatches(const AppState& state) {
    return s_frameBudgetColdMini.armed && state.database.isOpen() &&
        s_frameBudgetColdMini.activeDbPath == state.currentDbPathUtf8 &&
        s_frameBudgetColdMini.mainDbPath == state.database.getMainDbPath() &&
        s_frameBudgetColdMini.expectedFiles == 1000 &&
        s_frameBudgetColdMini.generation == state.backfillDbGeneration;
}
static void RestoreFrameBudgetColdMini(AppState& state) {
    if (!s_frameBudgetColdMini.holding) return;
    const bool sameDatabase = state.database.isOpen() &&
        s_frameBudgetColdMini.mainDbPath == state.database.getMainDbPath() &&
        s_frameBudgetColdMini.queueDbPath == state.backfillQueue.currentDbPath() &&
        s_frameBudgetColdMini.generation == state.backfillQueue.currentGeneration();
    state.settings.idleAssetBackfillEnabled = s_frameBudgetColdMini.savedIdleBackfill;
    if (state.database.isOpen())
        state.waveformWorker.start(&state.database, aura::RecommendedWaveformWorkerThreads());
    if (sameDatabase && s_frameBudgetColdMini.priorBackfillRunning)
        aura::GlobalBackfillWorkerPool().start(s_frameBudgetColdMini.queueDbPath,
            s_frameBudgetColdMini.generation, &state.backfillQueue,
            aura::RecommendedBackfillWorkerThreads());
    s_frameBudgetColdMini = {};
}
static aura::diagnostics::FrameBudgetEnvironment GetFrameBudgetEnvironment(AppState& state, bool probeAudio) {
    RECT rc{}; GetClientRect(g_hwnd, &rc);
    static auto getSwapInterval = reinterpret_cast<PFNWGLGETSWAPINTERVALEXTPROC>(
        wglGetProcAddress("wglGetSwapIntervalEXT"));
    const bool defaultAudio = state.audioEngine.getCurrentOutputDeviceIndex() == -1 &&
        state.audioEngine.isLoaded() && (!probeAudio ||
        (!state.audioEngine.isPlaying() && !state.audioEngine.enumerateOutputDevices().empty()));
    return {Platform::WindowIsVisible(g_hwnd), GetForegroundWindow() == g_hwnd, IsIconic(g_hwnd) != 0,
        rc.right - rc.left, rc.bottom - rc.top, GetDpiForWindow(g_hwnd) / 96.0,
        wglGetCurrentContext() != nullptr, getSwapInterval && getSwapInterval() > 0, defaultAudio};
}
static float s_frameBudgetSavedVolume = 1.0f;
static bool s_frameBudgetMuted = false;
static bool s_frameBudgetStartedPlayback = false;
static float s_frameBudgetSavedScroll = 0.0f;
static void RestoreFrameBudgetVolume(AppState& state, bool forceColdMini = false) {
    const bool captureActive = aura::diagnostics::FrameBudgetDiagnostics::isCaptureActive();
    if (s_frameBudgetMuted && !captureActive) {
        if (s_frameBudgetStartedPlayback) state.audioEngine.stop();
        if (!state.listTabs.empty()) { state.currentTab().listScrollY = s_frameBudgetSavedScroll; state.currentTab().restoreScrollY = true; }
        state.audioEngine.setVolume(s_frameBudgetSavedVolume); s_frameBudgetMuted = false;
        s_frameBudgetStartedPlayback = false;
    }
    if (forceColdMini || (s_frameBudgetColdMini.captureStarted && !captureActive))
        RestoreFrameBudgetColdMini(state);
}
static void AbortFrameBudgetCapture(AppState& state, const char* reason, const char* stage) {
    auto& capture = aura::diagnostics::FrameBudgetDiagnostics::shared();
    if (aura::diagnostics::FrameBudgetDiagnostics::isCaptureActive()) capture.abort(reason, stage);
    RestoreFrameBudgetVolume(state, true);
}
static void PumpFrameBudgetWorkload(AppState& state, bool intentionalFrameThrottleSleep) {
    using aura::diagnostics::FrameBudgetDiagnostics;
    using aura::diagnostics::FrameCaptureState;
    auto& capture = FrameBudgetDiagnostics::shared();
    if (!FrameBudgetDiagnostics::isCaptureActive()) { RestoreFrameBudgetVolume(state); return; }
    if (!ColdMiniArmMatches(state)) {
        AbortFrameBudgetCapture(state, "cold_mini_isolation_lost", "busy_setup"); return;
    }
    if (!capture.validateEnvironment(GetFrameBudgetEnvironment(state, false))) {
        RestoreFrameBudgetVolume(state, true); return;
    }
    if (capture.state() == FrameCaptureState::AwaitingBusy) {
        state.clearPendingWaveforms(); state.waveformFailed.clear();
        state.waveformMiniCacheMiss.clear(); state.visibleFilePaths.clear();
        state.waveformWorker.start(&state.database, aura::RecommendedWaveformWorkerThreads());
        s_frameBudgetColdMini.workerStarted = true;
        s_frameBudgetSavedVolume = state.audioEngine.getVolume();
        if (!state.listTabs.empty()) s_frameBudgetSavedScroll = state.currentTab().listScrollY;
        state.audioEngine.setVolume(0.0f); s_frameBudgetMuted = true;
        state.audioEngine.play(); s_frameBudgetStartedPlayback = true;
        if (!state.audioEngine.isPlaying()) {
            AbortFrameBudgetCapture(state, "playback_start_failed", "busy_setup"); return;
        }
        capture.beginScenario(aura::diagnostics::FrameScenario::BusyScrollMiniPlayback);
    }
    if (capture.state() != FrameCaptureState::BusyWarmup &&
        capture.state() != FrameCaptureState::BusyCapture) return;
    const int visibleRows = state.qaRegions.listVisibleRows;
    const int maxTopRow = (int)state.searchResults.size() - visibleRows;
    if (state.listTabs.empty() || visibleRows <= 0 || maxTopRow < 119) {
        AbortFrameBudgetCapture(state, "busy_fixture_insufficient", "busy_setup"); return;
    }
    const int sample = (int)capture.busySampleCount();
    const int phase = sample % 238;
    const int deterministicScroll = phase <= 119 ? phase : 238 - phase;
    const int topRow = deterministicScroll * maxTopRow / 119;
    state.currentTab().listScrollY = (float)(topRow * state.qaRegions.listRowHeight);
    state.currentTab().restoreScrollY = true; state.scrollToSelected = false;
    capture.observeBusyFrame(topRow, state.audioEngine.isPlaying(),
                             intentionalFrameThrottleSleep, visibleRows);
}
extern int g_magicEqQaLastRejectReason;
extern int g_magicEqQaActiveDragPart;

struct MagicEqQaPointer {
    bool active = false;
    bool pending = false;
    bool held = false;
    float x = 0.0f;
    float y = 0.0f;
    unsigned int viewport = 0;
};
static MagicEqQaPointer s_magicEqQaPointer;
#if ARIS_PREVIEW_ISOLATION
extern std::string GetDockQaState();
extern std::string SetupDockGestureFixtureForQa(AppState&);
extern bool ResolveDockQaPointerPoint(const std::string&, const std::string&, float&, float&, unsigned int&);
static bool s_dockQaPointerOwned = false;
static int s_dockQaEscapePhase = 0;
static int s_dockQaFocusEvent = 0;
#endif
static float s_magicEqQaFrameIntervalsMs[2048]{};
static size_t s_magicEqQaFrameCount = 0;
static std::atomic<ULONGLONG> s_magicEqQaCollectUntilMs{0};

static bool MagicEqQaIsolationReady() {
    // The isolation identity is fixed for this process. Avoid filesystem and
    // environment work on every UI frame.
    static const bool ready = [] {
    const std::string fixedRoot = Platform::HarkenUserData::PreviewIsolationRootUtf8();
    if (!Platform::HarkenUserData::PreviewIsolationActive() || fixedRoot.empty() ||
        GetHarkenSearchHarnessLaneKindUtf8() != "functional") return false;
    const auto normalized = [](std::string path) {
        std::replace(path.begin(), path.end(), '/', '\\');
        std::transform(path.begin(), path.end(), path.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        while (!path.empty() && path.back() == '\\') path.pop_back();
        return path;
    };
    return normalized(GetHarkenSearchRuntimeRootUtf8()) ==
        normalized(fixedRoot + "/profile/runtime");
    }();
    return ready;
}

static void ApplyMagicEqQaPointerBeforeFrame() {
#if ARIS_PREVIEW_ISOLATION
    if (s_dockQaEscapePhase) {
        ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, s_dockQaEscapePhase == 1);
        s_dockQaEscapePhase = s_dockQaEscapePhase == 1 ? 2 : 0;
    }
    if (s_dockQaFocusEvent) {
        ImGui::GetIO().AddFocusEvent(s_dockQaFocusEvent > 0);
        s_dockQaFocusEvent = 0;
    }
#endif
    if (!s_magicEqQaPointer.active) return;
    ImGuiIO& io = ImGui::GetIO();
    if (s_magicEqQaFrameCount < 2048)
        s_magicEqQaFrameIntervalsMs[s_magicEqQaFrameCount++] = io.DeltaTime * 1000.0f;
    io.AddMousePosEvent(s_magicEqQaPointer.x, s_magicEqQaPointer.y);
    if (s_magicEqQaPointer.viewport) io.AddMouseViewportEvent(s_magicEqQaPointer.viewport);
    if (s_magicEqQaPointer.pending) {
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, s_magicEqQaPointer.held);
        s_magicEqQaPointer.pending = false;
        if (!s_magicEqQaPointer.held) s_magicEqQaPointer.active = false;
    }
}

extern std::string ControlUpdateNoticeForQa(AppState&, const std::string&);

static void PollQaCommandFile(AppState& state) {
    static int s_pollSkip = 0;
    const int pollSkipTarget = IsHarkenSearchHarnessMultiInstanceEnabled() ? 1 : 10;
    if (++s_pollSkip < pollSkipTarget) return; // harness lanes poll every frame; default stays ~0.17s at 60fps
    s_pollSkip = 0;
    // During active audio playback skip the filesystem poll to avoid command-file I/O
    // latency (1–5 ms under antivirus) contributing to playback cursor stutter.
    // Harness-mode multi-instance polling is exempt (low-frequency test control).
    if (!IsHarkenSearchHarnessMultiInstanceEnabled() && state.audioEngine.isPlaying()) return;

    std::string runtimeRoot = GetHarkenSearchRuntimeRootUtf8();
    std::string cmdPath = runtimeRoot + "\\qa_cmd.json";
    std::string resPath = runtimeRoot + "\\qa_result.json";

    std::string raw;
    if (!Platform::QaFiles::ReadCommandJson(cmdPath, raw, 8192)) return;

    // Minimal JSON parsing (no external deps) — key-value extraction
    auto jsonUnescape = [](const std::string& s) -> std::string {
        auto DecodeJsonUnicodeEscapeToUtf8 = [](unsigned int codepoint, std::string& out) {
            if (codepoint <= 0x7F) {
                out += static_cast<char>(codepoint);
            } else if (codepoint <= 0x7FF) {
                out += static_cast<char>(0xC0 | ((codepoint >> 6) & 0x1F));
                out += static_cast<char>(0x80 | (codepoint & 0x3F));
            } else if (codepoint <= 0xFFFF) {
                out += static_cast<char>(0xE0 | ((codepoint >> 12) & 0x0F));
                out += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
                out += static_cast<char>(0x80 | (codepoint & 0x3F));
            } else if (codepoint <= 0x10FFFF) {
                out += static_cast<char>(0xF0 | ((codepoint >> 18) & 0x07));
                out += static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F));
                out += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
                out += static_cast<char>(0x80 | (codepoint & 0x3F));
            }
        };
        auto hexValue = [](char ch) -> int {
            if (ch >= '0' && ch <= '9') return ch - '0';
            if (ch >= 'a' && ch <= 'f') return 10 + (ch - 'a');
            if (ch >= 'A' && ch <= 'F') return 10 + (ch - 'A');
            return -1;
        };
        std::string out;
        out.reserve(s.size());
        for (size_t i = 0; i < s.size(); ++i) {
            char c = s[i];
            if (c != '\\' || i + 1 >= s.size()) {
                out += c;
                continue;
            }

            char next = s[++i];
            switch (next) {
                case '\\': out += '\\'; break;
                case '"':  out += '"'; break;
                case 'n':  out += '\n'; break;
                case 'r':  out += '\r'; break;
                case 't':  out += '\t'; break;
                case 'u': {
                    if (i + 4 < s.size()) {
                        unsigned int codepoint = 0;
                        bool ok = true;
                        for (int n = 0; n < 4; ++n) {
                            int v = hexValue(s[i + 1 + n]);
                            if (v < 0) {
                                ok = false;
                                break;
                            }
                            codepoint = (codepoint << 4) | static_cast<unsigned int>(v);
                        }
                        if (ok) {
                            i += 4;
                            DecodeJsonUnicodeEscapeToUtf8(codepoint, out);
                        } else {
                            out += 'u';
                        }
                    } else {
                        out += 'u';
                    }
                    break;
                }
                default:
                    out += next;
                    break;
            }
        }
        return out;
    };

    auto getStr = [&](const std::string& json, const char* key) -> std::string {
        std::string needle = std::string("\"") + key + "\"";
        auto pos = json.find(needle);
        if (pos == std::string::npos) return "";
        pos = json.find(':', pos);
        if (pos == std::string::npos) return "";
        auto q1 = json.find('"', pos + 1);
        if (q1 == std::string::npos) return "";
        bool escaped = false;
        for (size_t q2 = q1 + 1; q2 < json.size(); ++q2) {
            char ch = json[q2];
            if (escaped) {
                escaped = false;
                continue;
            }
            if (ch == '\\') {
                escaped = true;
                continue;
            }
            if (ch == '"') {
                return jsonUnescape(json.substr(q1 + 1, q2 - q1 - 1));
            }
        }
        return "";
    };

    auto getInt = [&](const std::string& json, const char* key, int def = -1) -> int {
        std::string needle = std::string("\"") + key + "\"";
        auto pos = json.find(needle);
        if (pos == std::string::npos) return def;
        pos = json.find(':', pos);
        if (pos == std::string::npos) return def;
        pos++;
        while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) pos++;
        return std::atoi(json.c_str() + pos);
    };

    auto getDouble = [&](const std::string& json, const char* key, double def = 0.0) -> double {
        std::string needle = std::string("\"") + key + "\"";
        auto pos = json.find(needle);
        if (pos == std::string::npos) return def;
        pos = json.find(':', pos);
        if (pos == std::string::npos) return def;
        pos++;
        while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) pos++;
        return std::atof(json.c_str() + pos);
    };

    auto getBool = [&](const std::string& json, const char* key, bool def = false) -> bool {
        std::string needle = std::string("\"") + key + "\"";
        auto pos = json.find(needle);
        if (pos == std::string::npos) return def;
        pos = json.find(':', pos);
        if (pos == std::string::npos) return def;
        pos++;
        while (pos < json.size() && (json[pos] == ' ' || json[pos] == '	')) pos++;
        if (json.compare(pos, 4, "true") == 0) return true;
        if (json.compare(pos, 5, "false") == 0) return false;
        return std::atoi(json.c_str() + pos) != 0;
    };

    auto jsonEscape = [](const std::string& s) -> std::string {
        std::string out;
        out.reserve(s.size() + 8);
        for (char c : s) {
            switch (c) {
                case '"':  out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n";  break;
                case '\r': out += "\\r";  break;
                case '\t': out += "\\t";  break;
                default:
                    if ((unsigned char)c < 0x20) {
                        char hex[8]; snprintf(hex, sizeof(hex), "\\u%04x", (unsigned char)c);
                        out += hex;
                    } else {
                        out += c;
                    }
            }
        }
        return out;
    };
    auto buildRowJson = [&](int rowIndex) -> std::string {
        if (rowIndex < 0 || rowIndex >= (int)state.searchResults.size()) {
            return "{}";
        }
        const auto& r = state.searchResults[(size_t)rowIndex];
        std::string item = "{";
        item += "\"idx\":" + std::to_string(rowIndex);
        item += ",\"id\":" + std::to_string((long long)r.id);
        item += ",\"name\":\"" + jsonEscape(r.fileName) + "\"";
        item += ",\"path\":\"" + jsonEscape(r.filePath) + "\"";
        item += ",\"folder_path\":\"" + jsonEscape(r.folderPath) + "\"";
        item += ",\"description\":\"" + jsonEscape(r.description) + "\"";
        item += ",\"keywords\":\"" + jsonEscape(r.keywords) + "\"";
        item += ",\"category\":\"" + jsonEscape(r.category) + "\"";
        item += ",\"embedded_cover_key\":\"" + jsonEscape(r.embeddedCoverKey) + "\"";
        item += ",\"folder_cover_key\":\"" + jsonEscape(r.folderCoverKey) + "\"";
        item += ",\"cover_key\":\"" + jsonEscape(!r.embeddedCoverKey.empty() ? r.embeddedCoverKey : r.folderCoverKey) + "\"";
        item += ",\"selected\":" + std::string(state.selectedIndicesSet.count(rowIndex) ? "true" : "false");
        item += "}";
        return item;
    };
    auto viewModeToString = [&](ViewMode mode) -> const char* {
        switch (mode) {
            case ViewMode::Browser: return "browser";
            case ViewMode::Database: return "database";
            case ViewMode::Playlist: return "playlist";
            case ViewMode::PlaylistLibrary: return "playlist_library";
            default: return "unknown";
        }
    };
    auto formatFilterValue = [&](int index) -> std::string {
        const char* formats[] = {"", "WAV", "MP3", "FLAC", "OGG", "AIFF", "M4A"};
        if (index >= 0 && index < (int)(sizeof(formats) / sizeof(formats[0]))) {
            return formats[index];
        }
        return "";
    };
    auto coverFilterValue = [&](int index) -> std::string {
        const char* modes[] = {"all", "has", "no"};
        if (index >= 0 && index < (int)(sizeof(modes) / sizeof(modes[0]))) {
            return modes[index];
        }
        return "all";
    };
    auto formatFilterIndexFromValue = [&](std::string value) -> int {
        std::transform(value.begin(), value.end(), value.begin(), ::toupper);
        if (value == "WAV") return 1;
        if (value == "MP3") return 2;
        if (value == "FLAC") return 3;
        if (value == "OGG") return 4;
        if (value == "AIFF" || value == "AIF") return 5;
        if (value == "M4A") return 6;
        return 0;
    };
    auto parseRowSet = [&](const std::string& rowsRaw) {
        std::set<int> rows;
        std::string token;
        auto flushRowToken = [&]() {
            if (token.empty()) return;
            try {
                rows.insert(std::stoi(token));
            } catch (...) {
            }
            token.clear();
        };
        for (char c : rowsRaw) {
            if (c == ',' || c == ';' || c == ' ') {
                flushRowToken();
            } else {
                token += c;
            }
        }
        flushRowToken();
        return rows;
    };
    auto buildInt64ArrayJson = [&](const std::vector<int64_t>& values) {
        std::string items = "[";
        for (size_t i = 0; i < values.size(); ++i) {
            if (i > 0) items += ",";
            items += std::to_string((long long)values[i]);
        }
        items += "]";
        return items;
    };
    auto buildPlaylistIdsJson = [&]() {
        std::vector<int64_t> ids;
        ids.reserve(state.playlists.size());
        for (const auto& playlist : state.playlists) {
            ids.push_back(playlist.id);
        }
        return buildInt64ArrayJson(ids);
    };
    auto applyDefaultTransferSettingsForQa = [&]() {
        state.transferFolderStrategy = state.settings.defaultTransferFolderStrategy;
        state.transferFileType = state.settings.defaultTransferFileType;
        state.transferExistingBehavior = state.settings.defaultTransferExistingBehavior;
        state.transferUseSourceFormat = state.settings.defaultTransferUseSourceFormat;
        state.transferEmbedMetadata = state.settings.defaultTransferEmbedMetadata;
    };
    auto applyPlaylistQuickBarOrderForQa = [&]() {
        if (state.playlists.empty() || state.settings.playlistQuickBarOrder.empty()) return;
        std::vector<Playlist> ordered;
        ordered.reserve(state.playlists.size());
        std::vector<int64_t> usedIds;
        usedIds.reserve(state.playlists.size());
        for (int64_t id : state.settings.playlistQuickBarOrder) {
            auto it = std::find_if(state.playlists.begin(), state.playlists.end(),
                [&](const Playlist& playlist) { return playlist.id == id; });
            if (it != state.playlists.end() &&
                std::find(usedIds.begin(), usedIds.end(), id) == usedIds.end()) {
                ordered.push_back(*it);
                usedIds.push_back(id);
            }
        }
        for (const auto& playlist : state.playlists) {
            if (std::find(usedIds.begin(), usedIds.end(), playlist.id) == usedIds.end()) {
                ordered.push_back(playlist);
                usedIds.push_back(playlist.id);
            }
        }
        if (ordered.size() == state.playlists.size()) {
            state.playlists = std::move(ordered);
        }
    };
    auto persistPlaylistQuickBarOrderForQa = [&]() {
        state.settings.playlistQuickBarOrder.clear();
        state.settings.playlistQuickBarOrder.reserve(state.playlists.size());
        for (const auto& playlist : state.playlists) {
            if (playlist.id > 0) state.settings.playlistQuickBarOrder.push_back(playlist.id);
        }
    };
    auto pumpImmediateSearchForQa = [&]() {
        const auto waitUntil = std::chrono::steady_clock::now() + std::chrono::milliseconds(2000);
        while (std::chrono::steady_clock::now() < waitUntil) {
            if (PumpOneAsyncSearchResult(state)) {
                break;
            }
            if (state.pendingSearchRequestId == 0 && !state.asyncSearchInProgress) {
                break;
            }
            Platform::SleepMilliseconds(5);
        }
        state.syncToTab();
    };
    auto cancelPendingSearchForQaMutation = [&]() {
        state.searchWorker.cancelActiveSearch();
        state.pendingSearchRequestId = 0;
        state.pendingFacetRequestId = 0;
        state.asyncSearchInProgress = false;
    };

    std::string requestId = getStr(raw, "request_id");
    std::string cmd = getStr(raw, "cmd");
    std::string result;

    if (cmd == "prepare_frame_budget_cold_mini") {
        char* runtimeRaw = nullptr; size_t runtimeLen = 0;
        _dupenv_s(&runtimeRaw, &runtimeLen, "AURA_RUNTIME_ROOT");
        const std::string runtimeEnv = runtimeRaw ? runtimeRaw : "";
        std::free(runtimeRaw);
        const std::string runtimeRoot = CanonicalColdMiniPath(runtimeEnv);
        const std::string expectedRoot = CanonicalColdMiniPath(getStr(raw, "expected_runtime_root"));
        const std::string expectedDb = CanonicalColdMiniPath(getStr(raw, "expected_db_path"));
        const std::string activeDb = CanonicalColdMiniPath(state.currentDbPathUtf8);
        const std::string mainDb = CanonicalColdMiniPath(state.database.getMainDbPath());
        const int expectedFiles = getInt(raw, "expected_files");
        const bool dbSwitchPending = state.dbSwitchInProgress ||
            !state.pendingDbPath.empty();
        const bool hostIsolated = IsHarkenSearchHarnessMultiInstanceEnabled() &&
            GetHarkenSearchHarnessLaneKindUtf8() == "functional" && !runtimeRoot.empty() &&
            runtimeRoot != CanonicalColdMiniPath(GetExeDirUtf8()) && expectedRoot == runtimeRoot &&
            expectedDb.size() > runtimeRoot.size() &&
            expectedDb.compare(0, runtimeRoot.size(), runtimeRoot) == 0 &&
            expectedDb[runtimeRoot.size()] == '\\';
        if (!hostIsolated) {
            result = "{\"schema\":1,\"cmd\":\"prepare_frame_budget_cold_mini\",\"status\":\"environment_gate\",\"reason\":\"isolated_runtime_required\",\"stage\":\"isolation\"}";
        } else if (dbSwitchPending) {
            result = "{\"schema\":1,\"cmd\":\"prepare_frame_budget_cold_mini\",\"status\":\"failed\",\"reason\":\"cold_mini_db_switch_pending\",\"stage\":\"preflight\"}";
        } else {
            const auto queueBefore = state.backfillQueue.snapshot();
            const int miniKind = (int)aura::BackfillJobKind::WaveformL1Overview;
            const bool idle = !s_frameBudgetColdMini.holding && expectedFiles == 1000 &&
                state.database.isOpen() && activeDb == expectedDb && mainDb == expectedDb &&
                state.database.getFileCount() == 1000 &&
                !state.isScanning.load() && !state.scanCompletionPending.load() &&
                !state.asyncSearchInProgress && state.pendingSearchRequestId == 0 &&
                !state.waveformRebuildActive && !state.audioEngine.isWaveformDetailPending() &&
                state.pendingDetailFilePath.empty() &&
                !aura::diagnostics::FrameBudgetDiagnostics::isCaptureActive() &&
                CanonicalColdMiniPath(state.backfillQueue.currentDbPath()) == expectedDb &&
                state.backfillQueue.currentGeneration() == state.backfillDbGeneration &&
                queueBefore.inFlight == 0 && queueBefore.byKind[miniKind] == 0 &&
                state.database.countBackfillPending() == 0;
            if (!idle) {
                result = "{\"schema\":1,\"cmd\":\"prepare_frame_budget_cold_mini\",\"status\":\"failed\",\"reason\":\"preflight_not_idle\",\"stage\":\"preflight\"}";
            } else {
                auto& arm = s_frameBudgetColdMini;
                arm.holding = true; arm.runtimeRoot = runtimeRoot; arm.dbPath = expectedDb;
                arm.activeDbPath = state.currentDbPathUtf8;
                arm.mainDbPath = state.database.getMainDbPath();
                arm.queueDbPath = state.backfillQueue.currentDbPath(); arm.expectedFiles = expectedFiles;
                arm.generation = state.backfillDbGeneration;
                arm.savedIdleBackfill = state.settings.idleAssetBackfillEnabled;
                arm.priorBackfillRunning = aura::GlobalBackfillWorkerPool().running();
                state.settings.idleAssetBackfillEnabled = false;
                aura::GlobalBackfillWorkerPool().stop();
                state.clearPendingWaveforms(false); state.waveformWorker.cancelAllPending();
                state.waveformWorker.stop();
                const bool identityBeforePurge = state.database.isOpen() &&
                    CanonicalColdMiniPath(state.currentDbPathUtf8) == expectedDb &&
                    CanonicalColdMiniPath(state.database.getMainDbPath()) == expectedDb &&
                    CanonicalColdMiniPath(state.backfillQueue.currentDbPath()) == expectedDb &&
                    state.backfillDbGeneration == arm.generation &&
                    state.backfillQueue.currentGeneration() == arm.generation &&
                    !state.dbSwitchInProgress && state.pendingDbPath.empty();
                const int64_t fileCount = state.database.getFileCount();
                const int miniBefore = state.database.countFilesNeedingWaveformCache();
                const int detailBefore = state.database.countFilesNeedingDetailCache();
                int purged = -1;
                if (identityBeforePurge && miniBefore >= 0 && miniBefore <= expectedFiles)
                    purged = state.database.purgeAllMiniWaveform();
                const int miniAfter = state.database.countFilesNeedingWaveformCache();
                const int detailAfter = state.database.countFilesNeedingDetailCache();
                std::vector<std::string> cacheKeys; cacheKeys.reserve(state.waveformCache.size());
                if (identityBeforePurge) {
                    for (const auto& item : state.waveformCache) cacheKeys.push_back(item.first);
                    for (const auto& key : cacheKeys) {
                        if (state.memoryBudget) state.memoryBudget->unregisterEntry(MemoryBudget::CacheType::Waveform, key);
                        ReleaseWaveformEvictedEntry(state, key);
                    }
                    state.clearPendingWaveforms(false); state.waveformFailed.clear();
                    state.waveformMiniCacheMiss.clear(); state.visibleFilePaths.clear();
                    state.meterFollowupPending.clear();
                }
                const auto queueAfter = state.backfillQueue.snapshot();
                const int workerPending = state.waveformWorker.pendingCount();
                const int durablePending = state.database.countBackfillPending();
                const size_t memoryEntries = state.memoryBudget
                    ? state.memoryBudget->getStatistics().waveformCount : 0;
                const size_t uiPending = state.waveformUrgentPending.size() +
                    state.waveformNormalPending.size();
                const size_t pendingSetCount = state.waveformPendingSet.size();
                const size_t failedCount = state.waveformFailed.size();
                const size_t missCount = state.waveformMiniCacheMiss.size();
                const size_t visibleCount = state.visibleFilePaths.size();
                const size_t meterPendingCount = state.meterFollowupPending.size();
                const size_t cacheEntries = state.waveformCache.size();
                const bool dbSwitchInProgress = state.dbSwitchInProgress;
                const bool pendingDbPathPresent = !state.pendingDbPath.empty();
                const bool dbIdentityOk = identityBeforePurge && state.database.isOpen() &&
                    CanonicalColdMiniPath(state.currentDbPathUtf8) == expectedDb &&
                    CanonicalColdMiniPath(state.database.getMainDbPath()) == expectedDb &&
                    CanonicalColdMiniPath(state.backfillQueue.currentDbPath()) == expectedDb &&
                    state.backfillDbGeneration == arm.generation &&
                    state.backfillQueue.currentGeneration() == arm.generation &&
                    !dbSwitchInProgress && !pendingDbPathPresent;
                const bool accountingOk = fileCount == expectedFiles && miniBefore >= 0 &&
                    miniBefore <= expectedFiles && purged >= 0 &&
                    miniBefore + purged == expectedFiles && miniAfter == expectedFiles;
                const bool detailOk = detailAfter == detailBefore;
                const bool workerOk = workerPending == 0;
                const bool queueOk = queueAfter.inFlight == 0 &&
                    queueAfter.byKind[miniKind] == 0 && durablePending == 0;
                const bool memoryOk = memoryEntries == 0 && cacheEntries == 0;
                const bool uiOk = uiPending == 0 && pendingSetCount == 0 && failedCount == 0 &&
                    missCount == 0 && visibleCount == 0 && meterPendingCount == 0;
                const char* failureReason = !dbIdentityOk ? "cold_mini_db_identity_lost" :
                    !accountingOk ? "cold_mini_cache_accounting_mismatch" :
                    !detailOk ? "cold_mini_detail_changed" :
                    !workerOk ? "cold_mini_worker_pending" :
                    !queueOk ? "cold_mini_queue_not_idle" :
                    !memoryOk ? "cold_mini_memory_not_cleared" :
                    !uiOk ? "cold_mini_ui_not_cleared" : nullptr;
                if (failureReason) {
                    RestoreFrameBudgetVolume(state, true);
                    result = "{\"schema\":1,\"cmd\":\"prepare_frame_budget_cold_mini\",\"status\":\"failed\",\"reason\":\"" + std::string(failureReason) +
                        "\",\"stage\":\"prepare\",\"counts\":{\"file_count\":" + std::to_string(fileCount) +
                        ",\"expected_files\":" + std::to_string(expectedFiles) + ",\"mini_missing_before\":" + std::to_string(miniBefore) +
                        ",\"purged_rows\":" + std::to_string(purged) + ",\"mini_missing_after\":" + std::to_string(miniAfter) +
                        ",\"detail_missing_before\":" + std::to_string(detailBefore) + ",\"detail_missing_after\":" + std::to_string(detailAfter) +
                        ",\"waveform_worker_pending\":" + std::to_string(workerPending) + ",\"backfill_in_flight\":" + std::to_string(queueAfter.inFlight) +
                        ",\"backfill_mini_queued\":" + std::to_string(queueAfter.byKind[miniKind]) + ",\"durable_pending\":" + std::to_string(durablePending) +
                        ",\"waveform_memory_entries\":" + std::to_string(memoryEntries) + ",\"waveform_cache_entries\":" + std::to_string(cacheEntries) +
                        ",\"ui_pending\":" + std::to_string(uiPending) + ",\"pending_set\":" + std::to_string(pendingSetCount) +
                        ",\"failed_count\":" + std::to_string(failedCount) + ",\"miss_count\":" + std::to_string(missCount) +
                        ",\"visible_count\":" + std::to_string(visibleCount) + ",\"meter_pending_count\":" + std::to_string(meterPendingCount) +
                        ",\"db_switch_in_progress\":" + std::string(dbSwitchInProgress ? "true" : "false") +
                        ",\"pending_db_path_present\":" + std::string(pendingDbPathPresent ? "true" : "false") +
                        ",\"db_identity_ok\":" + std::string(dbIdentityOk ? "true" : "false") + "}}";
                } else {
                    arm.armed = true;
                    result = "{\"schema\":1,\"cmd\":\"prepare_frame_budget_cold_mini\",\"status\":\"prepared\",\"runtime_root\":\"" + jsonEscape(runtimeRoot) +
                        "\",\"db_path\":\"" + jsonEscape(expectedDb) + "\",\"db_generation\":" + std::to_string(arm.generation) +
                        ",\"isolation_verified\":true,\"file_count\":" + std::to_string(fileCount) + ",\"purged_rows\":" + std::to_string(purged) +
                        ",\"mini_missing_before\":" + std::to_string(miniBefore) + ",\"mini_missing_after\":" + std::to_string(miniAfter) + ",\"detail_missing_before\":" + std::to_string(detailBefore) +
                        ",\"detail_missing_after\":" + std::to_string(detailAfter) + ",\"durable_pending\":" + std::to_string(durablePending) +
                        ",\"backfill_in_flight\":" + std::to_string(queueAfter.inFlight) + ",\"backfill_mini_queued\":" + std::to_string(queueAfter.byKind[miniKind]) +
                        ",\"waveform_worker_pending\":" + std::to_string(workerPending) + ",\"waveform_memory_entries\":" + std::to_string(memoryEntries) +
                        ",\"ui_pending\":" + std::to_string(uiPending) + ",\"idle_backfill_suppressed\":true,\"cold_mini_armed\":true,\"worker_start_phase\":\"busy_setup\"}";
                }
            }
        }
    } else if (cmd == aura::diagnostics::FrameBudgetDiagnostics::startCommand()) {
        auto& capture = aura::diagnostics::FrameBudgetDiagnostics::shared();
        if (!ColdMiniArmMatches(state)) {
            RestoreFrameBudgetVolume(state, true);
            result = "{\"schema\":1,\"cmd\":\"start_frame_budget_capture\",\"status\":\"failed\",\"reason\":\"cold_mini_not_armed\",\"stage\":\"preflight\"}";
        } else {
            const bool started = capture.startCapture(
                aura::diagnostics::FrameBudgetDiagnostics::Clock::now(), GetFrameBudgetEnvironment(state, true)) &&
                capture.beginScenario(aura::diagnostics::FrameScenario::Idle);
            s_frameBudgetColdMini.captureStarted = started;
            result = started ? "{\"ok\":true,\"cmd\":\"start_frame_budget_capture\",\"status\":\"started\"}"
                             : capture.resultJson();
            if (!started) RestoreFrameBudgetVolume(state, true);
        }
    } else if (cmd == aura::diagnostics::FrameBudgetDiagnostics::resultCommand()) {
        result = aura::diagnostics::FrameBudgetDiagnostics::shared().resultJson();
    } else if (cmd == "empty_state_audit") {
        if (!MagicEqQaIsolationReady()) {
            result = "{\"ok\":false,\"error\":\"fixed_preview_isolation_required\"}";
        } else {
            EmptyStateVisualAudit::Request();
            result = EmptyStateVisualAudit::Json();
        }
    } else if (cmd == "transport_audit" || cmd == "transport_audit_setup") {
        if (!MagicEqQaIsolationReady()) {
            result = "{\"ok\":false,\"error\":\"fixed_preview_isolation_required\"}";
        } else if (cmd == "transport_audit") {
            TransportVisualAudit::Request();
            result = TransportVisualAudit::Json();
        } else {
            const std::string scenario = getStr(raw, "scenario");
            if (scenario != "pass" && scenario != "bypass" && scenario != "notch" &&
                scenario != "processing" && scenario != "mono" && scenario != "spectrum" &&
                scenario != "overdrive" && scenario != "fill-boundary" && scenario != "selection" && scenario != "wide") {
                result = "{\"ok\":false,\"error\":\"invalid_transport_audit_scenario\"}";
            } else {
                state.audioEngine.stop();
                state.settings.productMode = ProductMode::SFX;
                state.audioEngine.setVolume(scenario == "overdrive" ? 1.07f :
                    (scenario == "fill-boundary" ? .55f : (scenario == "processing" ? .85f : .26f)));
                state.audioEngine.setMono(scenario == "mono");
                state.audioEngine.setPan(scenario == "processing" ? .5f : 0);
                state.audioEngine.setCenterSense(scenario == "processing" ? .6f : 0);
                state.audioEngine.setBassMonoFreq(scenario == "processing" ? 80.f : 0.f);
                state.audioEngine.setReverse(scenario == "processing");
                state.audioEngine.setInPoint(scenario == "selection" ? .2f : 0.f);
                state.audioEngine.setOutPoint(scenario == "selection" ? .8f : 1.f);
                state.showSpectrogram = scenario == "spectrum";
                state.spectrogramSplit = false;
                state.spectrogramBlend = .5f;
                MagicEQSettings eq = state.audioEngine.getMagicEQ();
                eq.enabled = scenario != "bypass";
                eq.mode = scenario == "notch" ? MagicEQMode::Notch : MagicEQMode::Pass;
                eq.widgetWidth = scenario == "wide" ? 800 : 340;
                eq.centerFreq = 1000; eq.halfWidthPercent = 30;
                state.audioEngine.setMagicEQ(eq);
                s_magicEqQaPointer.active = true; s_magicEqQaPointer.pending = true;
                s_magicEqQaPointer.held = false; s_magicEqQaPointer.x = -10000; s_magicEqQaPointer.y = -10000;
                TransportVisualAudit::Request();
                result = "{\"ok\":true,\"cmd\":\"transport_audit_setup\"}";
            }
        }

#if ARIS_PREVIEW_ISOLATION
    } else if (cmd == "info_page_state" || cmd == "info_page_pointer" || cmd == "info_page_layout" || cmd == "info_edit_input") {
        // Real ImGui input, restricted to this compile-time isolated QA package.
        // Never moves the operating-system cursor or targets another process.
        if (!MagicEqQaIsolationReady() || !s_mainUiReady.load(std::memory_order_acquire)) {
            result = R"({"ok":false,"error":"fixed_preview_isolation_required"})";
        } else {
            s_magicEqQaCollectUntilMs.store(Platform::MonotonicMilliseconds()+5000,std::memory_order_release);
            const auto snapshot = ThemeInspector::CopyLatestSnapshot();
            if (cmd == "info_edit_input") {
                if (getStr(raw,"action")=="exit") { state.settings.closeButtonAction=2; PostMessageW(g_hwnd,WM_CLOSE,0,0); }
                else if (getStr(raw,"action")=="tray_exit") {
                    Platform::WindowShow(g_hwnd,SW_MINIMIZE);
                    RequestHarkenSearchExit(g_hwnd,L"qa-tray-exit");
                }
                else if (getStr(raw,"action")=="update_exit")
                    s_qaUpdateExitRequested.store(true,std::memory_order_release);
                else if (getStr(raw,"action")=="new_db_dialog") state.showNewDbDialog=true;
                else if (getStr(raw,"action")=="new_db_name") state.qaNewDbName=getStr(raw,"text");
                else if (getStr(raw,"action")=="new_db_create") state.qaCreateNewDbNow=true;
                else if (getStr(raw,"action")=="escape_down") ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape,true);
                else if (getStr(raw,"action")=="escape_up") ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape,false);
                else ImGui::GetIO().AddInputCharactersUTF8(getStr(raw,"text").c_str());
                result=R"({"ok":true})";
            } else if (cmd == "info_page_layout") {
                const int width = getInt(raw,"width",320);
                if (width < 180 || width > 800 || s_magicEqQaPointer.held) {
                    result = R"({"ok":false,"error":"invalid_info_layout"})";
                } else {
                    state.panelFileInfo.dockedWidth = static_cast<float>(width);
                    state.panelFileInfo.floatW = static_cast<float>(width);
                    state.panelFileInfo.floatH = 600;
                    state.panelFileInfo.floatX = 100;
                    state.panelFileInfo.floatY = 120;
                    state.panelFileInfo.visible = true;
                    state.panelFileInfo.floating = getBool(raw,"floating",false);
                    result = R"({"ok":true})";
                }
            } else if (cmd == "info_page_pointer") {
                static std::string heldKey;
                const std::string key = getStr(raw,"key");
                const std::string phase = getStr(raw,"phase");
                const ThemeInspector::AnnotationZoneSnapshot* target = nullptr;
                for (const auto& z : snapshot.zones) if (z.key==key && z.key.rfind("info.",0)==0) target=&z;
                if (s_magicEqQaPointer.pending || !target || snapshot.frameNumber==0 ||
                    snapshot.frameNumber+1 < static_cast<std::uint64_t>(ImGui::GetFrameCount()) ||
                    (phase!="hover" && phase!="down" && phase!="up") ||
                    ((phase=="hover" || phase=="down") && s_magicEqQaPointer.held) ||
                    (phase=="up" && (!s_magicEqQaPointer.held || heldKey!=key))) {
                    result = R"({"ok":false,"error":"invalid_info_pointer_transition"})";
                } else {
                    const float x=(target->minX+target->maxX)*.5f, y=(target->minY+target->maxY)*.5f;
                    unsigned int viewport=ImGui::GetMainViewport()->ID;
                    for (auto* v : ImGui::GetPlatformIO().Viewports)
                        if (x>=v->Pos.x && x<v->Pos.x+v->Size.x && y>=v->Pos.y && y<v->Pos.y+v->Size.y) viewport=v->ID;
                    s_magicEqQaPointer={true,phase!="hover",phase=="down",x,y,viewport};
                    if (phase=="up") { s_magicEqQaPointer.held=false; heldKey.clear(); }
                    if (phase=="down") heldKey=key;
                    result = R"({"ok":true})";
                }
            } else {
                const auto* infoWindow = ImGui::FindWindowByName("Info##FloatingFileInfo");
                const auto editedRecord=state.metadataEditFileId>0?state.database.getFileById(state.metadataEditFileId):AudioRecord{};
                result="{\"ok\":true,\"selected\":"+std::to_string(state.settings.infoPages.selected)+
                    ",\"editing\":"+(state.infoEditSession.active?"true":"false")+
                    ",\"awaiting_save\":"+(state.infoEditSession.awaitingChoice?"true":"false")+
                    ",\"edit_id\":"+std::to_string(state.metadataEditFileId)+
                    ",\"window_iconic\":"+(IsIconic(g_hwnd)?"true":"false")+
                    ",\"visible\":"+(state.panelFileInfo.visible?"true":"false")+
                    ",\"saved_description\":\""+jsonEscape(editedRecord.description)+"\""+
                    ",\"draft_description\":\""+jsonEscape(state.metadataDescBuffer)+"\""+
                    ",\"save_error\":\""+jsonEscape(state.infoEditError)+"\""+
                    ",\"visible_mask\":"+std::to_string(state.settings.infoPages.visibleMask)+
                    ",\"floating\":"+(state.panelFileInfo.floating?"true":"false")+
                    ",\"outer_scrollbar\":"+(infoWindow && infoWindow->ScrollbarY?"true":"false")+
                    ",\"outer_scroll_max\":"+std::to_string(infoWindow?infoWindow->ScrollMax.y:0.0f)+
                    ",\"frame\":"+std::to_string(snapshot.frameNumber)+",\"zones\":{";
                bool first=true;
                for (const auto& z:snapshot.zones) {
                    if (z.key.rfind("info.",0)!=0) continue;
                    if (!first) result+=",";
                    first=false;
                    result+="\""+jsonEscape(z.key)+"\":{\"x\":"+std::to_string(z.minX)+
                        ",\"y\":"+std::to_string(z.minY)+",\"w\":"+std::to_string(z.maxX-z.minX)+
                        ",\"h\":"+std::to_string(z.maxY-z.minY)+"}";
                }
                result+="},\"layout_windows\":[";
                bool firstWindow = true;
                for (const auto* window : ImGui::GetCurrentContext()->Windows) {
                    const std::string name = window->Name;
                    if (!window->WasActive || (name.find("RightDockColumn") == std::string::npos &&
                        name.find("DockedFileInfo") == std::string::npos &&
                        name.find("FloatingFileInfo") == std::string::npos)) continue;
                    if (!firstWindow) result+=",";
                    firstWindow = false;
                    result+="{\"name\":\""+jsonEscape(name)+"\",\"x\":"+std::to_string(window->Pos.x)+
                        ",\"y\":"+std::to_string(window->Pos.y)+",\"w\":"+std::to_string(window->Size.x)+
                        ",\"h\":"+std::to_string(window->Size.y)+",\"scroll_max_y\":"+std::to_string(window->ScrollMax.y)+
                        ",\"clip_bottom\":"+std::to_string(window->ClipRect.Max.y)+"}";
                }
                result+="]}";
            }
        }
#endif
    } else if (cmd == "magic_eq_identity" || cmd == "magic_eq_state" ||
               cmd == "magic_eq_set_state" || cmd == "magic_eq_pointer") {
        // This channel exists only in a compile-time pinned QA build. An env
        // flag on an ordinary installed EXE cannot unlock pointer injection.
        const bool isolationReady = MagicEqQaIsolationReady();
        // Collect zones only around a bound QA command. Ordinary preview UI
        // stops paying the full-page annotation cost after five idle seconds.
        if (isolationReady) {
            s_magicEqQaCollectUntilMs.store(Platform::MonotonicMilliseconds() + 5000,
                                            std::memory_order_release);
        }
        if (!isolationReady) {
            result = "{\"ok\":false,\"error\":\"fixed_preview_isolation_required\"}";
        } else if (cmd == "magic_eq_identity") {
            const auto executablePath = Platform::GetExecutablePath();
            result = "{\"ok\":true,\"cmd\":\"magic_eq_identity\",\"pid\":" +
                std::to_string(Platform::CurrentProcessId()) + ",\"hwnd\":" +
                std::to_string(static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(g_hwnd))) +
                ",\"exe_path\":\"" +
                jsonEscape(executablePath.pathUtf8) +
                "\",\"isolation_root\":\"" +
                jsonEscape(Platform::HarkenUserData::PreviewIsolationRootUtf8()) +
                "\",\"runtime_root\":\"" + jsonEscape(GetHarkenSearchRuntimeRootUtf8()) + "\"}";
        } else if (cmd == "magic_eq_set_state") {
            MagicEQSettings next = state.audioEngine.getMagicEQ();
            const std::string mode = getStr(raw, "mode");
            const std::string grade = getStr(raw, "grade");
            const double center = getDouble(raw, "center_hz", next.centerFreq);
            const double halfWidth = getDouble(raw, "half_width_percent", next.halfWidthPercent);
            if ((!mode.empty() && mode != "pass" && mode != "notch") ||
                (!grade.empty() && grade != "soft" && grade != "steep" &&
                 grade != "very-steep" && grade != "brickwall") ||
                !std::isfinite(center) || center < 20.0 || center > 20000.0 ||
                !std::isfinite(halfWidth) || halfWidth < 1.0 || halfWidth > 50.0) {
                result = "{\"ok\":false,\"error\":\"invalid_magic_eq_setup\"}";
            } else {
                if (!mode.empty()) next.mode = mode == "notch" ? MagicEQMode::Notch : MagicEQMode::Pass;
                if (!grade.empty()) next.steepness = grade == "soft" ? CenterSenseDsp::MagicEQSteepness::Soft
                    : grade == "very-steep" ? CenterSenseDsp::MagicEQSteepness::VerySteep
                    : grade == "brickwall" ? CenterSenseDsp::MagicEQSteepness::Brickwall
                    : CenterSenseDsp::MagicEQSteepness::Steep;
                next.centerFreq = static_cast<float>(center);
                next.halfWidthPercent = static_cast<float>(halfWidth);
                next.enabled = getBool(raw, "enabled", next.enabled);
                const auto requested = CenterSenseDsp::estimateMagicEQTail(
                    CenterSenseDsp::MagicEQProcessParams{next.enabled, next.mode,
                        next.steepness, next.centerFreq, next.halfWidthPercent},
                    static_cast<int>(state.audioEngine.getFileInfo().sampleRate));
                const char* requestedReason = requested.reason == CenterSenseDsp::MagicEQTailReason::TailBudget
                    ? "tail_budget_200ms" :
                    requested.reason == CenterSenseDsp::MagicEQTailReason::FrequencyLimit
                    ? "frequency_limit" :
                    requested.reason == CenterSenseDsp::MagicEQTailReason::InvalidSampleRate
                    ? "invalid_sample_rate" : "none";
                state.audioEngine.setMagicEQ(next);
                const auto applied = state.audioEngine.getMagicEQ();
                result = "{\"ok\":true,\"cmd\":\"magic_eq_set_state\",\"applied_enabled\":" +
                    std::string(applied.enabled ? "true" : "false") +
                    ",\"requested_tail_reason\":\"" + requestedReason +
                    "\",\"tail_gate_preserved\":true}";
            }
        } else if (cmd == "magic_eq_pointer") {
            const auto snapshot = ThemeInspector::CopyLatestSnapshot();
            const std::string phase = getStr(raw, "phase");
            const std::string part = getStr(raw, "part");
            const float x = static_cast<float>(getDouble(raw, "x", -1.0));
            const float y = static_cast<float>(getDouble(raw, "y", -1.0));
            const auto inside = [&](const char* key) {
                for (const auto& zone : snapshot.zones) {
                    if (zone.key == key && x >= zone.minX && x < zone.maxX &&
                        y >= zone.minY && y < zone.maxY) return true;
                }
                return false;
            };
            const bool dragPart = part == "left" || part == "right" || part == "center";
            const bool menuPart = part == "grade" || part == "soft" || part == "steep" ||
                part == "very-steep" || part == "brickwall";
            const std::string targetZone = part == "grade" ? "transport.magicEQ.grade" :
                part == "soft" ? "transport.magicEQ.gradeSoft" :
                part == "steep" ? "transport.magicEQ.gradeSteep" :
                part == "very-steep" ? "transport.magicEQ.gradeVerySteep" :
                part == "brickwall" ? "transport.magicEQ.gradeBrickwall" :
                "transport.magicEQ." + part + "Grip";
            const bool startValid = (dragPart || menuPart) && inside(targetZone.c_str());
            const bool endValid = dragPart ? inside("transport.magicEQ") :
                menuPart && inside(targetZone.c_str());
            const bool coordinateValid = std::isfinite(x) && std::isfinite(y) &&
                x >= 0.0f && y >= 0.0f && x < snapshot.displayWidth && y < snapshot.displayHeight;
            if (!s_mainUiReady.load(std::memory_order_acquire) ||
                !Platform::WindowIsVisible(g_hwnd) || IsIconic(g_hwnd) ||
                !coordinateValid || snapshot.frameNumber == 0 ||
                snapshot.frameNumber + 1 < static_cast<std::uint64_t>(ImGui::GetFrameCount()) ||
                s_magicEqQaPointer.pending ||
                (phase == "down" && (s_magicEqQaPointer.active || !startValid)) ||
                (phase == "move" && (!dragPart || !s_magicEqQaPointer.active ||
                                     !inside("transport.magicEQ"))) ||
                (phase == "up" && (!s_magicEqQaPointer.active || !endValid)) ||
                (phase != "down" && phase != "move" && phase != "up")) {
                result = "{\"ok\":false,\"error\":\"invalid_magic_eq_pointer_transition\"}";
            } else {
                if (phase == "down") s_magicEqQaFrameCount = 0;
                s_magicEqQaPointer.active = true;
                s_magicEqQaPointer.x = x;
                s_magicEqQaPointer.y = y;
                s_magicEqQaPointer.held = phase != "up";
                s_magicEqQaPointer.pending = phase != "move";
                result = "{\"ok\":true,\"cmd\":\"magic_eq_pointer\",\"phase\":\"" +
                    phase + "\",\"source_frame\":" + std::to_string(snapshot.frameNumber) + "}";
            }
        } else {
            const auto current = state.audioEngine.getMagicEQ();
            const auto published = state.audioEngine.captureTransferProcessingSnapshot().magicEQ;
            const auto snapshot = ThemeInspector::CopyLatestSnapshot();
            const auto memory = Platform::QueryCurrentProcessMemoryUsage();
            float intervals[2048]{};
            std::copy_n(s_magicEqQaFrameIntervalsMs, s_magicEqQaFrameCount, intervals);
            std::sort(intervals, intervals + s_magicEqQaFrameCount);
            const float dragP95 = s_magicEqQaFrameCount == 0 ? 0.0f :
                intervals[static_cast<size_t>((s_magicEqQaFrameCount - 1) * 0.95)];
            const float dragMax = s_magicEqQaFrameCount == 0 ? 0.0f :
                intervals[s_magicEqQaFrameCount - 1];
            const int sampleRate = static_cast<int>(state.audioEngine.getFileInfo().sampleRate);
            const auto estimate = CenterSenseDsp::estimateMagicEQTail(
                CenterSenseDsp::MagicEQProcessParams{current.enabled, current.mode,
                    current.steepness, current.centerFreq, current.halfWidthPercent}, sampleRate);
            const auto reasonName = [](int reason) {
                switch (static_cast<CenterSenseDsp::MagicEQTailReason>(reason)) {
                case CenterSenseDsp::MagicEQTailReason::InvalidSampleRate: return "invalid_sample_rate";
                case CenterSenseDsp::MagicEQTailReason::FrequencyLimit: return "frequency_limit";
                case CenterSenseDsp::MagicEQTailReason::TailBudget: return "tail_budget_200ms";
                default: return "none";
                }
            };
            result = "{\"ok\":true,\"cmd\":\"magic_eq_state\",\"frame\":" +
                std::to_string(snapshot.frameNumber) + ",\"frame_ms\":" +
                std::to_string(ImGui::GetIO().DeltaTime * 1000.0f) +
                ",\"snapshot_fresh\":" +
                std::string(snapshot.frameNumber + 1 >=
                    static_cast<std::uint64_t>(ImGui::GetFrameCount()) ? "true" : "false") +
                ",\"drag_frame_count\":" + std::to_string(s_magicEqQaFrameCount) +
                ",\"drag_frame_ms_p95\":" + std::to_string(dragP95) +
                ",\"drag_frame_ms_max\":" + std::to_string(dragMax) +
                ",\"private_bytes\":" + std::to_string(memory.privateUsageBytes) +
                ",\"sample_rate\":" + std::to_string(sampleRate) +
                ",\"enabled\":" + std::string(current.enabled ? "true" : "false") +
                ",\"published_enabled\":" + std::string(published.enabled ? "true" : "false") +
                ",\"published_mode\":\"" + (published.mode == MagicEQMode::Notch ? "notch" : "pass") +
                "\",\"published_grade\":" + std::to_string(static_cast<int>(published.steepness)) +
                ",\"published_center_hz\":" + std::to_string(published.centerFreq) +
                ",\"published_half_width_percent\":" + std::to_string(published.halfWidthPercent) +
                ",\"published_equals_visible\":" +
                std::string(published.enabled == current.enabled &&
                    published.mode == current.mode && published.steepness == current.steepness &&
                    published.centerFreq == current.centerFreq &&
                    published.halfWidthPercent == current.halfWidthPercent ? "true" : "false") +
                ",\"mode\":\"" + (current.mode == MagicEQMode::Notch ? "notch" : "pass") +
                "\",\"grade\":" + std::to_string(static_cast<int>(current.steepness)) +
                ",\"center_hz\":" + std::to_string(current.centerFreq) +
                ",\"half_width_percent\":" + std::to_string(current.halfWidthPercent) +
                ",\"widget_width\":" + std::to_string(current.widgetWidth) +
                ",\"tail_available\":" + std::string(estimate.available ? "true" : "false") +
                ",\"tail_reason\":\"" + reasonName(static_cast<int>(estimate.reason)) +
                "\",\"last_ui_limit_reason\":\"" + reasonName(g_magicEqQaLastRejectReason) +
                "\",\"main_ui_ready\":" +
                std::string(s_mainUiReady.load(std::memory_order_acquire) ? "true" : "false") +
                ",\"active_drag_part\":" + std::to_string(g_magicEqQaActiveDragPart) +
                ",\"zones\":{";
            bool first = true;
            for (const auto& zone : snapshot.zones) {
                if (zone.key != "transport.magicEQ" &&
                    zone.key.rfind("transport.magicEQ.", 0) != 0) continue;
                if (!first) result += ",";
                first = false;
                result += "\"" + jsonEscape(zone.key) + "\":{\"x\":" +
                    std::to_string(zone.minX) + ",\"y\":" + std::to_string(zone.minY) +
                    ",\"w\":" + std::to_string(zone.maxX - zone.minX) +
                    ",\"h\":" + std::to_string(zone.maxY - zone.minY) + "}";
            }
            result += "}}";
        }
    } else if (cmd == "update_notice" || cmd == "update_notice_pointer") {
        if (!MagicEqQaIsolationReady()) {
            result = R"({"ok":false,"error":"fixed_preview_isolation_required"})";
        } else if (cmd == "update_notice") {
            result = ControlUpdateNoticeForQa(state, getStr(raw, "action"));
        } else {
            const auto qa = aris::update::GetHarkenSearchUpdateNoticeQaSnapshot();
            const std::string control = getStr(raw, "control");
            const std::string phase = getStr(raw, "phase");
            const auto found = qa.controls.find(control);
            // Only browsing/dismiss controls are exposed. QA cannot activate
            // an update/install through this interaction channel.
            const bool allowed = control == "history" || control == "back-to-update" ||
                control == "close" || control == "history-dismiss" || control == "body";
            if (!state.showUpdateCenter || !allowed || found == qa.controls.end() ||
                !s_mainUiReady.load(std::memory_order_acquire) || IsIconic(g_hwnd)) {
                result = R"({"ok":false,"error":"notice_control_not_visible"})";
            } else {
                const auto& rect = found->second;
                const float x = (rect[0] + rect[2]) * 0.5f;
                const float y = (rect[1] + rect[3]) * 0.5f;
                if (phase == "wheel" && control == "body") {
                    ImGui::GetIO().AddMousePosEvent(x, y);
                    const float amount = static_cast<float>(std::clamp(getDouble(raw, "amount", -3), -8.0, 8.0));
                    ImGui::GetIO().AddMouseWheelEvent(0.0f, amount);
                    result = R"({"ok":true,"interaction":"wheel"})";
                } else if (control != "body" &&
                    ((phase == "down" && !s_magicEqQaPointer.held) ||
                     (phase == "up" && s_magicEqQaPointer.held))) {
                    s_magicEqQaPointer = {true, true, phase == "down", x, y};
                    result = R"({"ok":true,"interaction":"pointer"})";
                } else {
                    result = R"({"ok":false,"error":"invalid_notice_pointer_transition"})";
                }
            }
        }
    } else if (cmd == "function_window") {
#if ARIS_PREVIEW_ISOLATION
        if (!MagicEqQaIsolationReady() || !s_mainUiReady.load(std::memory_order_acquire) ||
            (!state.currentDbPathUtf8.empty() &&
             !Platform::HarkenUserData::PreviewIsolationAllowsDataPathUtf8(state.currentDbPathUtf8))) {
            result = R"({"ok":false,"error":"fixed_preview_isolation_required"})";
        } else {
            const std::string module = getStr(raw, "module");
            AppState::PanelState* panel = nullptr;
            bool* show = nullptr;
            if (module == "ucs-browser") {
                panel = &state.panelUCSBrowser;
                show = &state.showUCSBrowser;
            } else if (module == "tag-browser") {
                panel = &state.panelTagBrowser;
                show = &state.showTagBrowser;
            } else if (module == "workflow") {
                panel = &state.panelWorkflow;
                show = &state.showWorkflowDialog;
            } else if (module == "transfer") {
                panel = &state.panelTransfer;
                show = &state.showTransferDialog;
            }
            const std::string action = getStr(raw, "action");
            if (!panel || !show) {
                result = R"({"ok":false,"error":"unknown_function_window"})";
            } else if (action == "open") {
                *show = true;
                panel->visible = true;
                panel->floating = getBool(raw, "floating", false);
                const int side = std::clamp(getInt(raw, "dock_side", static_cast<int>(panel->dockSide)), 0, 3);
                panel->dockSide = static_cast<DockSide>(side);
                panel->floatX = static_cast<float>(getDouble(raw, "x", panel->floatX));
                panel->floatY = static_cast<float>(getDouble(raw, "y", panel->floatY));
                panel->floatW = static_cast<float>(getDouble(raw, "w", panel->floatW));
                panel->floatH = static_cast<float>(getDouble(raw, "h", panel->floatH));
                result = std::string(R"({"ok":true,"module":")") + module + R"(","action":"open"})";
            } else if (action == "close") {
                *show = false;
                panel->visible = false;
                panel->floating = false;
                result = std::string(R"({"ok":true,"module":")") + module + R"(","action":"close"})";
            } else if (action == "state" || action.empty()) {
                result = std::string(R"({"ok":true,"module":")") + module +
                    R"(","visible":)" + std::string(panel->visible ? "true" : "false") +
                    R"(,"floating":)" + std::string(panel->floating ? "true" : "false") +
                    R"(,"dock_side":)" + std::to_string(static_cast<int>(panel->dockSide)) +
                    R"(,"x":)" + std::to_string(panel->floatX) +
                    R"(,"y":)" + std::to_string(panel->floatY) +
                    R"(,"w":)" + std::to_string(panel->floatW) +
                    R"(,"h":)" + std::to_string(panel->floatH) + "}";
            } else {
                result = R"({"ok":false,"error":"unknown_function_window_action"})";
            }
        }
#else
        result = R"({"ok":false,"error":"preview_isolation_disabled"})";
#endif
    } else if (cmd == "dock_state" || cmd == "dock_pointer" || cmd == "dock_setup") {
#if ARIS_PREVIEW_ISOLATION
        if (!MagicEqQaIsolationReady() || !s_mainUiReady.load(std::memory_order_acquire) ||
            (!state.currentDbPathUtf8.empty() &&
             !Platform::HarkenUserData::PreviewIsolationAllowsDataPathUtf8(state.currentDbPathUtf8))) {
            result = R"({"ok":false,"error":"fixed_preview_isolation_required"})";
        } else if (cmd == "dock_state") {
            result = GetDockQaState();
        } else if (cmd == "dock_setup") {
            result = s_magicEqQaPointer.active
                ? R"({"ok":false,"error":"qa_pointer_active"})" : SetupDockGestureFixtureForQa(state);
        } else {
            const std::string phase = getStr(raw, "phase");
            float x = s_magicEqQaPointer.x;
            float y = s_magicEqQaPointer.y;
            unsigned int viewport = s_magicEqQaPointer.viewport;
            if (phase == "hover" && !s_magicEqQaPointer.held && !s_magicEqQaPointer.pending &&
                ResolveDockQaPointerPoint(getStr(raw, "module"), getStr(raw, "part"), x, y, viewport)) {
                s_magicEqQaPointer = {true, false, false, x, y, viewport};
                s_dockQaPointerOwned = true;
                result = "{\"ok\":true,\"x\":" + std::to_string(x) + ",\"y\":" + std::to_string(y) + "}";
            } else if (phase == "down" && s_dockQaPointerOwned && !s_magicEqQaPointer.held &&
                       !s_magicEqQaPointer.pending &&
                ResolveDockQaPointerPoint(getStr(raw, "module"), getStr(raw, "part"), x, y, viewport)) {
                s_magicEqQaPointer = {true, true, true, x, y, viewport};
                s_dockQaPointerOwned = true;
                result = "{\"ok\":true,\"x\":" + std::to_string(x) + ",\"y\":" + std::to_string(y) + "}";
            } else if ((phase == "move" || phase == "up") && s_dockQaPointerOwned &&
                       s_magicEqQaPointer.active && !s_magicEqQaPointer.pending) {
                x = static_cast<float>(getDouble(raw, "x", x));
                y = static_cast<float>(getDouble(raw, "y", y));
                if (!std::isfinite(x) || !std::isfinite(y) || std::fabs(x) > 100000 || std::fabs(y) > 100000) {
                    result = R"({"ok":false,"error":"invalid_dock_pointer_position"})";
                } else {
                    s_magicEqQaPointer.x = x;
                    s_magicEqQaPointer.y = y;
                    if (phase == "up") {
                        s_magicEqQaPointer.held = false;
                        s_magicEqQaPointer.pending = true;
                        s_dockQaPointerOwned = false;
                    }
                    result = R"({"ok":true})";
                }
            } else if (phase == "escape" && s_dockQaPointerOwned) {
                s_dockQaEscapePhase = 1;
                result = R"({"ok":true})";
            } else if (phase == "blur" && s_dockQaPointerOwned) {
                s_dockQaFocusEvent = -1;
                result = R"({"ok":true})";
            } else if (phase == "focus") {
                s_dockQaFocusEvent = 1;
                result = R"({"ok":true})";
            } else {
                result = R"({"ok":false,"error":"invalid_dock_pointer_transition"})";
            }
        }
#else
        result = R"({"ok":false,"error":"preview_isolation_disabled"})";
#endif
    } else if (cmd == "similar_index") {
#if ARIS_PREVIEW_ISOLATION
        // A repeatable CLI seam for the real window and its app-owned task.
        // Only compile-time pinned preview builds may control these fixtures.
        const std::string action = getStr(raw, "action");
        const bool isolated = !Platform::HarkenUserData::PreviewIsolationRootUtf8().empty() &&
            Platform::HarkenUserData::PreviewIsolationAllowsDataPathUtf8(GetHarkenSearchRuntimeRootUtf8()) &&
            Platform::HarkenUserData::PreviewIsolationAllowsDataPathUtf8(state.currentDbPathUtf8);
        const int language = getInt(raw, "language", static_cast<int>(g_currentLanguage));
        const int width = getInt(raw, "width", 720);
        const int height = getInt(raw, "height", 420);
        if (!isolated) {
            result = "{\"ok\":false,\"error\":\"fixed_preview_isolation_required\"}";
        } else if (language < 0 || language > 2 || width < 220 || width > 1600 ||
                   height < 180 || height > 1200) {
            result = "{\"ok\":false,\"error\":\"invalid_preview_layout\"}";
        } else if (action != "open" && action != "close" && action != "start" &&
                   action != "cancel" && action != "find" && action != "state") {
            result = "{\"ok\":false,\"error\":\"unknown_similar_index_action\"}";
        } else if (state.dbSwitchInProgress) {
            result = "{\"ok\":false,\"error\":\"database_switch_in_progress\"}";
        } else {
            if (action == "open") {
                g_currentLanguage = static_cast<AppLanguage>(language);
                state.panelSimilarSounds.floatW = static_cast<float>(width);
                state.panelSimilarSounds.floatH = static_cast<float>(height);
                aura::ui::OpenSimilarSoundsWindow(state);
                state.panelSimilarSounds.floating = getBool(raw, "floating", true);
                state.showAIPanel = getBool(raw, "show_speech", false);
            } else if (action == "close") {
                state.panelSimilarSounds.visible = false;
                state.similarSoundsWindow.open = false;
                aura::ui::InvalidateSimilarSoundsWindow(state);
            } else if (action == "start") {
                StartBatchFeatureExtraction(state);
            } else if (action == "cancel") {
                state.batchFeatureCancelRequested.store(true);
            } else if (action == "find" && state.similarSoundsWorkflow &&
                       !state.batchFeatureExtracting.load()) {
                state.similarSoundsWorkflow->RequestSearch(state.similarSoundsPreset, 50);
            }
            const auto snapshot = state.similarSoundsWorkflow
                ? state.similarSoundsWorkflow->Snapshot() : HarkenSearch::SimilarityWorkflowSnapshot{};
            int indexed = 0, total = 0;
            state.database.tryGetFeatureIndexCounts(indexed, total);
            const bool completionNotice = state.panelSimilarSounds.visible &&
                !state.batchFeatureExtracting.load() &&
                !state.batchFeatureCancelRequested.load() && total > 0 && indexed == total &&
                state.batchFeatureCompletionNotice.load();
            result = "{\"ok\":true,\"cmd\":\"similar_index\",\"visible\":" +
                std::string(state.panelSimilarSounds.visible ? "true" : "false") +
                ",\"running\":" + (state.batchFeatureExtracting.load() ? "true" : "false") +
                ",\"cancel_requested\":" + (state.batchFeatureCancelRequested.load() ? "true" : "false") +
                ",\"completion_notice\":" + (completionNotice ? "true" : "false") +
                ",\"indexed\":" + std::to_string(indexed) +
                ",\"total\":" + std::to_string(total) +
                ",\"progress\":" + std::to_string(state.batchFeatureProgress.load()) +
                ",\"batch_total\":" + std::to_string(state.batchFeatureTotal.load()) +
                ",\"rows\":" + std::to_string(snapshot.rows.size()) +
                ",\"database\":\"" + jsonEscape(state.currentDbPathUtf8) + "\"}";
        }
#else
        result = "{\"ok\":false,\"error\":\"fixed_preview_isolation_required\"}";
#endif

    } else if (cmd == "get_state") {
        // Return current app state snapshot
        int resultCount = (int)state.searchResults.size();
        int64_t totalCount = state.totalFileCount;
        const char* playState = state.audioEngine.isPlaying() ? "playing" :
                               (state.audioEngine.isPaused() ? "paused" : "stopped");
        std::string currentFile = state.audioEngine.getLoadedFilePath();
        const bool audioLoaded = state.audioEngine.isLoaded();
        const float playbackPosition = state.audioEngine.getPlaybackPosition();
        const AudioFileInfo& audioInfo = state.audioEngine.getFileInfo();
        const int selectedRow =
            (state.selectedResultIndex >= 0 &&
             state.selectedResultIndex < (int)state.searchResults.size())
                ? state.selectedResultIndex
                : -1;
        const AudioRecord* selectedRecord =
            (selectedRow >= 0)
                ? &state.searchResults[(size_t)selectedRow]
                : nullptr;
        const int64_t selectedFileId = selectedRecord ? selectedRecord->id : -1;
        const std::string selectedPath =
            selectedRecord ? selectedRecord->filePath : std::string{};
        const bool hasEngineInfo =
            state.audioEngine.isLoaded() && !audioInfo.filePath.empty();
        const bool previewUsesSelectedRecord =
            selectedRecord &&
            (!hasEngineInfo ||
             state.audioEngine.isLoadFilePending() ||
             audioInfo.filePath != selectedRecord->filePath);
        const int64_t effectivePreviewFileId =
            previewUsesSelectedRecord
                ? selectedRecord->id
                : ((state.currentFileId > 0) ? state.currentFileId : -1);
        const std::string effectivePreviewPath =
            previewUsesSelectedRecord
                ? selectedRecord->filePath
                : audioInfo.filePath;
        const bool waveformCachedCurrent = !currentFile.empty() && state.waveformCache.count(currentFile) > 0;
        const bool waveformFailedCurrent = !currentFile.empty() && state.waveformFailed.count(currentFile) > 0;
        const bool detailPending = state.audioEngine.isWaveformDetailPending();
        const bool bgCacheRunning = state.bgCacheRunning.load();
        const auto scanBackgroundBackfillPhase = [&state]() {
            switch (static_cast<AppState::ScanBackgroundBackfillPhase>(
                state.scanBackgroundBackfillPhase.load(std::memory_order_acquire))) {
            case AppState::ScanBackgroundBackfillPhase::Deferred:
                return "deferred";
            case AppState::ScanBackgroundBackfillPhase::Resumed:
                return "resumed";
            case AppState::ScanBackgroundBackfillPhase::Idle:
            default:
                return "idle";
            }
        }();
        const ListTab* activeTab = nullptr;
        if (!state.listTabs.empty() &&
            state.activeTabIndex >= 0 &&
            state.activeTabIndex < (int)state.listTabs.size()) {
            activeTab = &state.listTabs[(size_t)state.activeTabIndex];
        }

        std::string recentDbPathsJson = "[";
        for (size_t i = 0; i < state.recentDbPathsUtf8.size(); ++i) {
            if (i > 0) recentDbPathsJson += ",";
            recentDbPathsJson += "\"" + jsonEscape(state.recentDbPathsUtf8[i]) + "\"";
        }
        recentDbPathsJson += "]";

        result = "{";
        result += "\"ok\":true,";
        result += "\"cmd\":\"get_state\",";
        result += "\"search_text\":\"" + jsonEscape(state.searchBuffer) + "\",";
        result += "\"result_count\":" + std::to_string(resultCount) + ",";
        result += "\"total_count\":" + std::to_string((long long)totalCount) + ",";
        result += "\"play_state\":\"" + std::string(playState) + "\",";
        result += "\"current_file\":\"" + jsonEscape(currentFile) + "\",";
        result += "\"audio_loaded\":" + std::string(audioLoaded ? "true" : "false") + ",";
        result += "\"playback_position\":" + std::to_string(playbackPosition) + ",";
        result += "\"audio_total_frames\":" + std::to_string((unsigned long long)audioInfo.totalFrames) + ",";
        result += "\"audio_duration\":" + std::to_string(audioInfo.durationSeconds) + ",";
        result += "\"selected_row\":" + std::to_string(selectedRow) + ",";
        result += "\"selected_file_id\":" + std::to_string((long long)selectedFileId) + ",";
        result += "\"selected_path\":\"" + jsonEscape(selectedPath) + "\",";
        result += "\"effective_preview_file_id\":" + std::to_string((long long)effectivePreviewFileId) + ",";
        result += "\"effective_preview_path\":\"" + jsonEscape(effectivePreviewPath) + "\",";
        result += "\"preview_uses_selected_record\":" + std::string(previewUsesSelectedRecord ? "true" : "false") + ",";
        result += "\"last_clicked_index\":" + std::to_string(state.lastClickedIndex) + ",";
        result += "\"window_width\":" + std::to_string((int)ImGui::GetIO().DisplaySize.x) + ",";
        result += "\"window_height\":" + std::to_string((int)ImGui::GetIO().DisplaySize.y) + ",";
        result += "\"webui_active\":false,";
        result += "\"webui_bridge_active\":false,";
        result += "\"memory_profile\":" + std::to_string(state.settings.memoryProfile) + ",";
        result += "\"theme\":" + std::to_string((int)state.settings.theme) + ",";
        result += "\"loudness_norm_enabled\":" + std::string(state.settings.loudnessNormEnabled ? "true" : "false") + ",";
        result += "\"active_view\":\"" + std::string(viewModeToString(state.viewMode)) + "\",";
        result += "\"active_library\":\"" + jsonEscape(state.currentDbPathUtf8) + "\",";
        result += "\"db_path\":\"" + jsonEscape(state.currentDbPathUtf8) + "\",";
        result += "\"file_player_fast_path\":" + std::string(state.filePlayerFastPathActive ? "true" : "false") + ",";
        result += "\"pending_db_path\":\"" + jsonEscape(state.pendingDbPath) + "\",";
        result += "\"recent_db_paths\":" + recentDbPathsJson + ",";
        result += "\"db_switch_in_progress\":" + std::string(state.dbSwitchInProgress ? "true" : "false") + ",";
        result += "\"db_switch_just_completed\":" + std::string(state.dbSwitchJustCompleted ? "true" : "false") + ",";
        result += "\"db_switch_settle_frames\":" + std::to_string(state.dbSwitchSettleFramesRemaining) + ",";
        result += "\"pending_search_request_id\":" + std::to_string((unsigned long long)state.pendingSearchRequestId) + ",";
        result += "\"pending_facet_request_id\":" + std::to_string((unsigned long long)state.pendingFacetRequestId) + ",";
        result += "\"async_search_in_progress\":" + std::string(state.asyncSearchInProgress ? "true" : "false") + ",";
        result += "\"pending_count_follow_up\":" + std::string(state.pendingCountFollowUp ? "true" : "false") + ",";
        result += "\"main_ui_ready\":" + std::string(s_mainUiReady.load(std::memory_order_acquire) ? "true" : "false") + ",";
        const char* startupWarmupPhase = "ready";
        if (state.warmupFramesRemaining > 0) {
            startupWarmupPhase = "ui_warmup";
        } else if (state.dbSwitchSettleFramesRemaining > 0) {
            startupWarmupPhase = "db_switch_settle";
        } else if (state.coverWall.preloadInFlight) {
            startupWarmupPhase = "cover_wall_preload";
        } else if (state.coverWall.entriesInFlight) {
            startupWarmupPhase = "cover_wall_page_load";
        } else if (state.coverWall.foldersInFlight) {
            startupWarmupPhase = "cover_wall_folder_discovery";
        } else if (state.waveformWorker.pendingCount() > 0 ||
                   !state.waveformUrgentPending.empty() ||
                   !state.waveformNormalPending.empty()) {
            startupWarmupPhase = "waveform_queue";
        }
        result += "\"startup_warmup\":{";
        result += "\"phase\":\"" + std::string(startupWarmupPhase) + "\",";
        result += "\"mode\":\"startup_first_screen_preload\",";
        result += "\"ui_warmup_frames\":" + std::to_string(state.warmupFramesRemaining) + ",";
        result += "\"db_switch_settle_frames\":" + std::to_string(state.dbSwitchSettleFramesRemaining) + ",";
        result += "\"cover_wall_preload_in_flight\":" + std::string(state.coverWall.preloadInFlight ? "true" : "false") + ",";
        result += "\"cover_wall_preload_items\":" + std::to_string((long long)state.coverWall.preloadFullList.size()) + ",";
        result += "\"cover_wall_entries_in_flight\":" + std::string(state.coverWall.entriesInFlight ? "true" : "false") + ",";
        result += "\"cover_wall_folders_in_flight\":" + std::string(state.coverWall.foldersInFlight ? "true" : "false") + ",";
        result += "\"cover_wall_loaded\":" + std::string(state.coverWall.loaded ? "true" : "false") + ",";
        result += "\"cover_wall_display_count\":" + std::to_string((long long)state.coverWall.qaDisplayCount) + ",";
        result += "\"waveform_worker_pending\":" + std::to_string(state.waveformWorker.pendingCount()) + ",";
        result += "\"waveform_ui_pending\":" + std::to_string((long long)(state.waveformUrgentPending.size() + state.waveformNormalPending.size())) + "},";
        result += "\"query_state\":" + std::to_string((int)state.filterQueryState) + ",";
        result += "\"search_ready\":" + std::string(
            (state.filterQueryState == FilterQueryState::Idle ||
             state.filterQueryState == FilterQueryState::Ready ||
             state.filterQueryState == FilterQueryState::Error)
            ? "true" : "false"
        ) + ",";
        result += "\"cover_wall_priority_mode\":" + std::string(IsCoverWallPriorityMode() ? "true" : "false") + ",";
        result += "\"folder_filter\":{";
        result += "\"active\":" + std::string(state.folderFilterActive ? "true" : "false") + ",";
        result += "\"path\":\"" + jsonEscape(state.selectedFolderPath) + "\"},";
        result += "\"cover_filter\":{";
        result += "\"active\":" + std::string(state.coverFilterActive ? "true" : "false") + ",";
        result += "\"path\":\"" + jsonEscape(state.coverFilterPath) + "\",";
        result += "\"mode\":\"" + jsonEscape(coverFilterValue(state.coverFilterIndex)) + "\"},";
        result += "\"metadata_filters\":{";
        result += "\"artist\":\"" + jsonEscape(state.artistFilterBuffer) + "\",";
        result += "\"album\":\"" + jsonEscape(state.albumFilterBuffer) + "\",";
        result += "\"category\":\"" + jsonEscape(state.categoryFilterBuffer) + "\",";
        result += "\"format\":\"" + jsonEscape(formatFilterValue(state.formatFilterIndex)) + "\"},";
        result += "\"history_panel_visible\":" + std::string(state.showHistoryPanel ? "true" : "false") + ",";
        result += "\"metadata_dialog_visible\":" + std::string((state.showMetadataEditDialog || state.metadataEditMode) ? "true" : "false") + ",";
        result += "\"metadata_batch_mode\":" + std::string(state.metadataBatchMode ? "true" : "false") + ",";
        result += "\"selected_count\":" + std::to_string((int)state.selectedIndicesSet.size()) + ",";
        result += "\"transfer\":{";
        result += "\"queued_count\":" + std::to_string((int)state.transferSelectedFiles.size()) + ",";
        result += "\"dest_folder\":\"" + jsonEscape(state.transferDestFolder) + "\",";
        result += "\"in_progress\":" + std::string(state.transferInProgress ? "true" : "false") + ",";
        // H1c: transfer status may be keyed (worker sites store key+args; main-thread
        // QA snapshot consumers must render, not read the raw field).
        // Byte-equal to the pre-H1a field content at every write site.
        result += "\"status_message\":\"" + jsonEscape(state.TransferStatusText()) + "\",";
        result += "\"completed_count\":" + std::to_string(state.transferCompletedCount) + ",";
        result += "\"error_count\":" + std::to_string(state.transferErrorCount) + ",";
        result += "\"total_count\":" + std::to_string(state.transferTotalCount) + ",";
        result += "\"folder_strategy\":" + std::to_string(state.transferFolderStrategy) + ",";
        result += "\"file_type\":" + std::to_string(state.transferFileType) + ",";
        result += "\"existing_behavior\":" + std::to_string(state.transferExistingBehavior) + ",";
        result += "\"use_source_format\":" + std::string(state.transferUseSourceFormat ? "true" : "false") + ",";
        result += "\"embed_metadata\":" + std::string(state.transferEmbedMetadata ? "true" : "false") + "},";
        result += "\"playlists\":{";
        result += "\"count\":" + std::to_string((int)state.playlists.size()) + ",";
        result += "\"ids\":" + buildPlaylistIdsJson() + ",";
        result += "\"quickbar_order\":" + buildInt64ArrayJson(state.settings.playlistQuickBarOrder) + ",";
        result += "\"quickbar_hidden_ids\":" + buildInt64ArrayJson(state.settings.playlistQuickBarHiddenIds) + ",";
        result += "\"context_menu_playlist_id\":" + std::to_string((long long)state.qaOpenSidebarPlaylistContextId) + "},";
        result += "\"undo_can_undo\":" + std::string(state.undoStack.canUndo() ? "true" : "false") + ",";
        result += "\"undo_can_redo\":" + std::string(state.undoStack.canRedo() ? "true" : "false") + ",";
        result += "\"undo_count\":" + std::to_string(state.undoStack.undoCount()) + ",";
        result += "\"redo_count\":" + std::to_string(state.undoStack.redoCount()) + ",";
        result += "\"waveform_panel_visible\":" + std::string(state.panelWaveform.visible ? "true" : "false") + ",";
        result += "\"waveform_panel_height\":" + std::to_string((int)state.panelWaveform.dockedHeight) + ",";
        result += "\"waveform_cached_current\":" + std::string(waveformCachedCurrent ? "true" : "false") + ",";
        result += "\"waveform_failed_current\":" + std::string(waveformFailedCurrent ? "true" : "false") + ",";
        result += "\"waveform_detail_pending\":" + std::string(detailPending ? "true" : "false") + ",";
        const int64_t waveformRebuildDisplayProgress =
            (state.waveformRebuildTotal > 0)
                ? std::min(state.waveformRebuildProgress, state.waveformRebuildTotal)
                : state.waveformRebuildProgress;
        result += "\"waveform_rebuild_active\":" + std::string(state.waveformRebuildActive ? "true" : "false") + ",";
        result += "\"waveform_rebuild_full_mode\":" + std::string(state.waveformRebuildFullMode ? "true" : "false") + ",";
        result += "\"waveform_rebuild_mini_only\":" + std::string(state.waveformRebuildMiniOnlyMode ? "true" : "false") + ",";
        result += "\"waveform_rebuild_progress\":" + std::to_string((long long)waveformRebuildDisplayProgress) + ",";
        result += "\"waveform_rebuild_progress_raw\":" + std::to_string((long long)state.waveformRebuildProgress) + ",";
        result += "\"waveform_rebuild_total\":" + std::to_string((long long)state.waveformRebuildTotal) + ",";
        result += "\"waveform_rebuild_mini_total\":" + std::to_string((long long)state.waveformRebuildMiniTotal) + ",";
        result += "\"waveform_rebuild_detail_total\":" + std::to_string((long long)state.waveformRebuildDetailTotal) + ",";
        result += "\"waveform_rebuild_worker_pending\":" + std::to_string(state.waveformWorker.pendingCount()) + ",";
        result += "\"waveform_rebuild_ui_pending\":" + std::to_string((long long)(state.waveformUrgentPending.size() + state.waveformNormalPending.size())) + ",";
        result += "\"waveform_rebuild_batch_index\":" + std::to_string(state.waveformRebuildBatchIdx) + ",";
        result += "\"waveform_rebuild_batch_size\":" + std::to_string((long long)state.waveformRebuildBatch.size()) + ",";
        result += "\"cover_rebuild\":{";
        result += "\"active\":" + std::string(state.coverRebuildActive ? "true" : "false") + ",";
        result += "\"progress\":" + std::to_string((long long)state.coverRebuildProgress) + ",";
        result += "\"total\":" + std::to_string((long long)state.coverRebuildTotal) + ",";
        result += "\"found\":" + std::to_string(state.coverRebuildFound) + ",";
        result += "\"cancel_requested\":" + std::string(state.coverRebuildCancel ? "true" : "false") + "},";
        result += "\"show_spectrogram\":" + std::string(state.showSpectrogram ? "true" : "false") + ",";
        result += "\"spectrogram_fft_size\":" + std::to_string(state.spectrogramFFTSize) + ",";
        result += "\"spectrogram_split\":" + std::string(state.spectrogramSplit ? "true" : "false") + ",";
        result += "\"spectrogram_needs_update\":" + std::string(state.spectrogramNeedsUpdate ? "true" : "false") + ",";
        result += "\"spectrogram_texture_ready\":" + std::string(state.spectrogramTexture != 0 ? "true" : "false") + ",";
        result += "\"bg_cache_running\":" + std::string(bgCacheRunning ? "true" : "false") + ",";
        result += "\"tabs\":{";
        result += "\"active_index\":" + std::to_string(state.activeTabIndex) + ",";
        result += "\"active_id\":" + std::to_string(activeTab ? activeTab->id : -1) + ",";
        result += "\"active_name\":\"" + jsonEscape(activeTab ? activeTab->name : std::string()) + "\",";
        result += "\"count\":" + std::to_string((int)state.listTabs.size()) + ",";
        result += "\"search_results_snapshot_valid\":" + std::string((activeTab && activeTab->searchResultsSnapshotValid) ? "true" : "false") + ",";
        result += "\"facets_snapshot_valid\":" + std::string((activeTab && activeTab->facetsSnapshotValid) ? "true" : "false") + ",";
        result += "\"cover_wall_snapshot_valid\":" + std::string((activeTab && activeTab->coverWallSnapshot.valid) ? "true" : "false") + "},";
        auto coverWallKeyStatsForQa = [](const std::vector<CoverModule::CoverItem>& items) {
            struct Stats {
                size_t uniqueKeys = 0;
                size_t duplicateKeys = 0;
                size_t emptyKeys = 0;
            } stats;
            std::unordered_set<std::string> seen;
            for (const auto& item : items) {
                if (item.key.empty()) {
                    ++stats.emptyKeys;
                    continue;
                }
                if (!seen.insert(item.key).second) {
                    ++stats.duplicateKeys;
                }
            }
            stats.uniqueKeys = seen.size();
            return stats;
        };
        auto coverWallSourceNameForQa = [](CoverWallSource source) -> const char* {
            switch (source) {
                case CoverWallSource::Global: return "global";
                case CoverWallSource::Facet: return "facet";
                case CoverWallSource::SearchFallback: return "search_fallback";
                case CoverWallSource::SameFolderFallback: return "same_folder_fallback";
                case CoverWallSource::Selection: return "selection";
            }
            return "unknown";
        };
        const auto coverWallItemKeyStats = coverWallKeyStatsForQa(state.coverWall.items);
        const auto coverWallFallbackKeyStats = coverWallKeyStatsForQa(state.coverWall.fallbackItems);

        result += "\"cover_wall\":{";
        const auto coverWorkStats = CoverModule::GetPendingCoverWorkStats();
        result += "\"integrated_mode\":" + std::string(state.settings.coverWallIntegratedMode ? "true" : "false") + ",";
        result += "\"source\":" + std::to_string((int)state.coverWall.source) + ",";
        result += "\"source_name\":\"" + std::string(coverWallSourceNameForQa(state.coverWall.source)) + "\",";
        result += "\"generation\":" + std::to_string((unsigned long long)state.coverWall.generation) + ",";
        result += "\"view_generation\":" + std::to_string((unsigned long long)state.coverWall.viewGeneration) + ",";
        result += "\"loaded\":" + std::string(state.coverWall.loaded ? "true" : "false") + ",";
        result += "\"loading\":" + std::string(state.coverWall.loading ? "true" : "false") + ",";
        result += "\"thumb_size\":" + std::to_string(state.coverWall.thumbSize) + ",";
        result += "\"entries_in_flight\":" + std::string(state.coverWall.entriesInFlight ? "true" : "false") + ",";
        result += "\"folders_in_flight\":" + std::string(state.coverWall.foldersInFlight ? "true" : "false") + ",";
        result += "\"preload_in_flight\":" + std::string(state.coverWall.preloadInFlight ? "true" : "false") + ",";
        result += "\"items\":" + std::to_string((long long)state.coverWall.items.size()) + ",";
        result += "\"pending_facet_items\":" + std::to_string((long long)state.coverWall.pendingFacetItems.size()) + ",";
        result += "\"pending_facet_cursor\":" + std::to_string((long long)state.coverWall.pendingFacetCursor) + ",";
        result += "\"filtered_items\":" + std::to_string((long long)state.coverWall.filteredItems.size()) + ",";
        result += "\"fallback_items\":" + std::to_string((long long)state.coverWall.fallbackItems.size()) + ",";
        result += "\"unique_keys\":" + std::to_string((long long)coverWallItemKeyStats.uniqueKeys) + ",";
        result += "\"duplicate_keys\":" + std::to_string((long long)coverWallItemKeyStats.duplicateKeys) + ",";
        result += "\"empty_keys\":" + std::to_string((long long)coverWallItemKeyStats.emptyKeys) + ",";
        result += "\"fallback_unique_keys\":" + std::to_string((long long)coverWallFallbackKeyStats.uniqueKeys) + ",";
        result += "\"fallback_duplicate_keys\":" + std::to_string((long long)coverWallFallbackKeyStats.duplicateKeys) + ",";
        result += "\"fallback_empty_keys\":" + std::to_string((long long)coverWallFallbackKeyStats.emptyKeys) + ",";
        result += "\"display_snapshot\":" + std::to_string((long long)state.coverWall.displaySnapshot.size()) + ",";
        result += "\"display_count\":" + std::to_string((long long)state.coverWall.qaDisplayCount) + ",";
        result += "\"prefetch_tiles\":" + std::to_string((long long)state.coverWall.qaPrefetchTileCount) + ",";
        result += "\"visible_tiles\":" + std::to_string((long long)state.coverWall.qaVisibleTileCount) + ",";
        result += "\"visible_ready\":" + std::to_string((long long)state.coverWall.qaVisibleReadyCount) + ",";
        result += "\"visible_live_textures\":" + std::to_string((long long)state.coverWall.qaVisibleLiveTextureCount) + ",";
        result += "\"visible_mirrors\":" + std::to_string((long long)state.coverWall.qaVisibleMirrorCount) + ",";
        result += "\"visible_current_mirrors\":" + std::to_string((long long)state.coverWall.qaVisibleCurrentMirrorCount) + ",";
        result += "\"visible_stale_mirrors\":" + std::to_string((long long)state.coverWall.qaVisibleStaleMirrorCount) + ",";
        result += "\"visible_stale_draws\":" + std::to_string((long long)state.coverWall.qaVisibleStaleDrawCount) + ",";
        result += "\"visible_placeholders\":" + std::to_string((long long)state.coverWall.qaVisiblePlaceholderCount) + ",";
        result += "\"hidden_failed_items\":" + std::to_string((long long)state.coverWall.qaHiddenFailedItemCount) + ",";
        result += "\"hidden_pending_items\":" + std::to_string((long long)state.coverWall.qaHiddenPendingItemCount) + ",";
        result += "\"current_cover_facets\":" + std::to_string((long long)state.currentCoverFacets.size()) + ",";
        result += "\"tex_cache\":" + std::to_string((long long)state.coverWall.texCache.size()) + ",";
        result += "\"failed_tex_cache\":" + std::to_string((long long)state.coverWall.failedTexCache.size()) + ",";
        result += "\"pending_cover_work\":" + std::to_string((long long)CoverModule::GetPendingCoverWorkCount()) + ",";
        result += "\"pending_cover_keys\":" + std::to_string((long long)coverWorkStats.pendingKeys) + ",";
        result += "\"pending_cover_queue\":" + std::to_string((long long)coverWorkStats.asyncQueue) + ",";
        result += "\"pending_cover_uploads\":" + std::to_string((long long)coverWorkStats.uploadQueue) + ",";
        result += "\"pending_cover_upload_keys\":" + std::to_string((long long)coverWorkStats.uploadQueuedKeys) + ",";
        result += "\"failed_cover_keys\":" + std::to_string((long long)coverWorkStats.failedKeys) + ",";
        result += "\"non_cover_texture_epoch\":" + std::to_string((unsigned long long)state.coverWall.nonCoverTextureEpoch) + ",";
        result += "\"display_refresh_requested\":" + std::string(state.coverWallDisplayRefreshRequested ? "true" : "false") + ",";
        result += "\"database_invalidation_requested\":" + std::string(state.coverWallDatabaseInvalidationRequested ? "true" : "false") + ",";
        result += "\"needs_reload\":" + std::string(state.needsCoverWallReload ? "true" : "false") + "},";
        {
            aura::coverwall::BrowseSurfaceInfoInput browseInfoInput;
            browseInfoInput.resultCount = static_cast<std::int64_t>(state.coverWall.qaDisplayCount);
            browseInfoInput.pathCount =
                !state.currentPathFacets.empty()
                    ? state.currentPathFacets.size()
                    : ((state.folderFilterActive && !state.selectedFolderPath.empty()) ? size_t{1} : size_t{0});
            browseInfoInput.keyword = state.searchBuffer;
            browseInfoInput.selectedPath =
                (state.folderFilterActive && !state.selectedFolderPath.empty())
                    ? state.selectedFolderPath
                    : std::string{};
            browseInfoInput.selectedCoverKey =
                (state.coverFilterActive && !state.coverFilterPath.empty())
                    ? state.coverFilterPath
                    : std::string{};
            browseInfoInput.selectedCoverLabel =
                (state.coverWall.pinnedCoverFilterItem.key == state.coverFilterPath)
                    ? state.coverWall.pinnedCoverFilterItem.label
                    : std::string{};
            browseInfoInput.sourceLabel = coverWallSourceNameForQa(state.coverWall.source);
            browseInfoInput.refreshing =
                state.pendingFacetRequestId != 0 ||
                state.pendingSearchRequestId != 0 ||
                state.asyncSearchInProgress ||
                state.coverWall.loading ||
                state.coverWall.entriesInFlight ||
                state.coverWall.foldersInFlight ||
                state.coverWall.preloadInFlight;
            browseInfoInput.hasStableSnapshot = !state.coverWall.displaySnapshot.empty();
            browseInfoInput.selectionMode = false;
            const auto browseInfo = aura::coverwall::BuildBrowseSurfaceInfo(browseInfoInput);
            result += "\"browse_surface\":{";
            result += "\"selected_row\":" + std::to_string(selectedRow) + ",";
            result += "\"selected_file_id\":" + std::to_string((long long)selectedFileId) + ",";
            result += "\"selected_path\":\"" + jsonEscape(selectedPath) + "\",";
            result += "\"effective_preview_file_id\":" + std::to_string((long long)effectivePreviewFileId) + ",";
            result += "\"effective_preview_path\":\"" + jsonEscape(effectivePreviewPath) + "\",";
            result += "\"preview_uses_selected_record\":" + std::string(previewUsesSelectedRecord ? "true" : "false") + ",";
            result += "\"thumb_size\":" + std::to_string(state.coverWall.thumbSize) + ",";
            result += "\"integrated_mode\":" + std::string(state.settings.coverWallIntegratedMode ? "true" : "false") + ",";
            result += "\"pinned_cover_key\":\"" + jsonEscape(state.coverWall.pinnedCoverFilterItem.key) + "\",";
            result += "\"pinned_cover_file_path\":\"" + jsonEscape(state.coverWall.pinnedCoverFilterItem.filePathUtf8) + "\",";
            result += "\"pinned_cover_folder_path\":\"" + jsonEscape(state.coverWall.pinnedCoverFilterItem.folderPathUtf8) + "\",";
            result += "\"top_info\":\"" + jsonEscape(browseInfo.topLine) + "\",";
            result += "\"bottom_info\":\"" + jsonEscape(browseInfo.bottomLine) + "\",";
            result += "\"status_label\":\"" + jsonEscape(browseInfo.statusLabel) + "\",";
            result += "\"cover_filter_label\":\"" + jsonEscape(browseInfo.coverFilterLabel) + "\"},";
        }
        result += "\"scan_status\":{";
        result += "\"scanning\":" + std::string(state.isScanning.load() ? "true" : "false") + ",";
        result += "\"phase\":\"" + std::string(ScanPhaseToSnakeCase(state.scanPhase.load())) + "\",";
        result += "\"progress\":" + std::to_string(state.scanProgress.load()) + ",";
        result += "\"total\":" + std::to_string(state.scanTotal.load()) + "},";
        result += "\"scan_summary\":{";
        result += "\"added\":" + std::to_string(state.scanAddedCount.load()) + ",";
        result += "\"updated\":" + std::to_string(state.scanUpdatedCount.load()) + ",";
        result += "\"skipped\":" + std::to_string(state.scanSkippedCount.load()) + ",";
        result += "\"removed\":" + std::to_string(state.scanRemovedCount.load()) + ",";
        result += "\"errors\":" + std::to_string(state.scanErrorCount.load()) + ",";
        result += "\"total\":" + std::to_string(state.scanTotal.load()) + "},";
        result += "\"scan_in_progress\":" + std::string(state.isScanning.load() ? "true" : "false") + ",";
        result += "\"last_scanned_path\":\"" + jsonEscape(state.lastScannedPath) + "\",";
        result += "\"scan_phase\":\"" + std::string(ScanPhaseToSnakeCase(state.scanPhase.load())) + "\",";
        result += "\"scan_workflow_stage\":\"" + std::string(HarkenSearch::ScanWorkflowStageName(state.scanWorkflowStage.load(std::memory_order_acquire))) + "\",";
        result += "\"scan_discovered\":" + std::to_string(state.scanDiscoveredCount.load(std::memory_order_acquire)) + ",";
        result += "\"scan_processed\":" + std::to_string(state.scanProcessedCount.load(std::memory_order_acquire)) + ",";
        result += "\"scan_succeeded\":" + std::to_string(state.scanIngestedCount.load(std::memory_order_acquire)) + ",";
        result += "\"scan_failed_files\":" + std::to_string(state.scanFailedFileCount.load(std::memory_order_acquire)) + ",";
        result += "\"scan_mini_waveforms_completed\":" + std::to_string(state.scanMiniWaveformsCompleted.load(std::memory_order_acquire)) + ",";
        result += "\"scan_mini_waveforms_total\":" + std::to_string(state.scanMiniWaveformsTotal.load(std::memory_order_acquire)) + ",";
        result += "\"scan_strategy\":\"" + std::string(state.settings.cacheWaveformsOnScan ? "full" : "fast") + "\",";
        result += "\"scan_completion_pending\":" + std::string(state.scanCompletionPending.load(std::memory_order_acquire) ? "true" : "false") + ",";
        result += "\"scan_completion_waveforms_pending\":" + std::string(state.scanCompletionWaveformsPending.load(std::memory_order_acquire) ? "true" : "false") + ",";
        result += "\"scan_completion_covers_pending\":" + std::string(state.scanCompletionCoversPending.load(std::memory_order_acquire) ? "true" : "false") + ",";
        result += "\"scan_completion_total\":" + std::to_string(state.scanCompletionTotal.load(std::memory_order_acquire)) + ",";
        result += "\"scan_completion_pending_files\":" + std::to_string(state.scanCompletionPendingFiles.load(std::memory_order_acquire)) + ",";
        result += "\"scan_completion_mini_unresolved\":" + std::to_string(state.scanCompletionMiniUnresolved.load(std::memory_order_acquire)) + ",";
        result += "\"scan_completion_cover_unresolved\":" + std::to_string(state.scanCompletionCoverUnresolved.load(std::memory_order_acquire)) + ",";
        result += "\"scan_completion_metadata_unresolved\":" + std::to_string(state.scanCompletionMetadataUnresolved.load(std::memory_order_acquire)) + ",";
        result += "\"scan_started_at_ms\":" + std::to_string(state.scanStartedAtMs.load(std::memory_order_acquire)) + ",";
        result += "\"scan_completion_ready_at_ms\":" + std::to_string(state.scanCompletionReadyAtMs.load(std::memory_order_acquire)) + ",";
        result += "\"scan_completion_duration_ms\":" + std::to_string(state.scanCompletionDurationMs.load(std::memory_order_acquire)) + ",";
        result += "\"scan_background_backfill_phase\":\"" + std::string(scanBackgroundBackfillPhase) + "\",";
        result += "\"scan_background_backfill_deferred_wait_ms\":" + std::to_string(state.scanBackgroundBackfillDeferredWaitMs.load(std::memory_order_acquire)) + ",";
        result += "\"scan_background_backfill_resume_threads\":" + std::to_string(state.scanBackgroundBackfillResumeThreads.load(std::memory_order_acquire)) + ",";
        const ScanReadyToWorkSnapshot readyToWork = GetScanReadyToWorkSnapshotForQa(state);
        result += "\"ready_to_work_pending\":" + std::string(readyToWork.pending ? "true" : "false") + ",";
        result += "\"ready_to_work_waveforms_pending\":" + std::string(readyToWork.waveformsPending ? "true" : "false") + ",";
        result += "\"ready_to_work_covers_pending\":" + std::string(readyToWork.coversPending ? "true" : "false") + ",";
        result += "\"ready_to_work_cover_post_pending\":" + std::string(readyToWork.coverPostPending ? "true" : "false") + ",";
        result += "\"ready_to_work_cover_first_screen_pending\":" + std::string(readyToWork.coverFirstScreenPending ? "true" : "false") + ",";
        result += "\"ready_to_work_main_wave_pending\":" + std::string(readyToWork.mainWavePending ? "true" : "false");
        result += "}";

    } else if (cmd == "new_tab") {
        int newIdx = state.createAndActivateNewDatabaseTab(-1.0);
        result = "{\"ok\":" + std::string(newIdx >= 0 ? "true" : "false")
               + ",\"cmd\":\"new_tab\",\"tab_index\":" + std::to_string(newIdx)
               + ",\"tab_count\":" + std::to_string((int)state.listTabs.size())
               + ",\"active_index\":" + std::to_string(state.activeTabIndex) + "}";

    } else if (cmd == "activate_tab") {
        int tabIndex = getInt(raw, "tab_index", getInt(raw, "index", -1));
        if (state.activateTab(tabIndex, -1.0, false)) {
            result = "{\"ok\":true,\"cmd\":\"activate_tab\",\"tab_index\":" + std::to_string(tabIndex)
                   + ",\"tab_count\":" + std::to_string((int)state.listTabs.size())
                   + ",\"active_index\":" + std::to_string(state.activeTabIndex) + "}";
        } else {
            result = "{\"ok\":false,\"cmd\":\"activate_tab\",\"error\":\"tab index out of range\"}";
        }

    } else if (cmd == "focus_window") {
        ShowManagerWindowNow();
        result = "{\"ok\":true,\"cmd\":\"focus_window\"}";

    } else if (cmd == "search") {
        std::string query = getStr(raw, "query");
        if (query.size() < sizeof(state.searchBuffer) - 1) {
            strncpy_s(state.searchBuffer, sizeof(state.searchBuffer), query.c_str(), _TRUNCATE);
            state.focusSearchBar = !ShouldUseHarnessImmediateFilterRefresh();
            state.filterQueryGeneration++;
            if (ShouldUseHarnessImmediateFilterRefresh()) {
                state.viewMode = ViewMode::Database;
                cancelPendingSearchForQaMutation();
                // QA keyword search should follow the real async filter pipeline:
                // submit immediately, keep the main thread free, and let wait_ready
                // poll until both the result page and any deferred facet/count
                // follow-up settle.
                state.preferDeferredCountForNextRefresh = true;
                RefreshSearchResults(state, true);
                state.lastAppliedSearchText = query;
                state.suppressNextRealtimeSearchCommit = true;
                state.syncToTab();
            }
        }
        result = "{\"ok\":true,\"cmd\":\"search\",\"query\":\"" + jsonEscape(query)
               + "\",\"result_count\":" + std::to_string((int)state.searchResults.size())
               + ",\"search_ready\":" + std::string(
                    (state.filterQueryState == FilterQueryState::Idle || state.filterQueryState == FilterQueryState::Ready)
                    ? "true" : "false")
               + "}";

    } else if (cmd == "select_row") {
        int row = getInt(raw, "row", -1);
        if (row >= 0 && row < (int)state.searchResults.size()) {
            state.selectedResultIndex = row;
            state.selectedIndicesSet.clear();
            state.selectedIndicesSet.insert(row);
            state.lastClickedIndex = row;
            state.scrollToSelected = true;
            DIAG_ACTION_FMT("Browse", "SelectRow:%d", row);
            state.audioEngine.loadFileAsync(
                state.searchResults[row].filePath,
                state.settings.autoplayOnSelect
            );
            result = "{\"ok\":true,\"cmd\":\"select_row\",\"row\":" + std::to_string(row) + "}";
        } else {
            result = "{\"ok\":false,\"cmd\":\"select_row\",\"error\":\"row out of range\"}";
        }

    } else if (cmd == "load_path") {
        std::string path = getStr(raw, "path");
        if (path.empty()) {
            result = "{\"ok\":false,\"cmd\":\"load_path\",\"error\":\"path missing\"}";
        } else if (!PathExistsUtf8(path)) {
            result = "{\"ok\":false,\"cmd\":\"load_path\",\"error\":\"path missing\"}";
        } else {
            auto normalizeQaPath = [](const std::string& value) {
                std::string normalized = value;
                std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char c) {
                    if (c == '/') return '\\';
                    return (char)std::tolower(c);
                });
                return normalized;
            };
            const std::string normalizedTarget = normalizeQaPath(path);
            int matchedRow = -1;
            for (int i = 0; i < (int)state.searchResults.size(); ++i) {
                if (normalizeQaPath(state.searchResults[(size_t)i].filePath) == normalizedTarget) {
                    matchedRow = i;
                    break;
                }
            }
            if (matchedRow >= 0) {
                state.selectedResultIndex = matchedRow;
                state.selectedIndicesSet.clear();
                state.selectedIndicesSet.insert(matchedRow);
                state.lastClickedIndex = matchedRow;
                state.scrollToSelected = true;
            }
            DIAG_ACTION("Browse", "LoadPath");
            state.audioEngine.loadFileAsync(path, state.settings.autoplayOnSelect);
            result = "{\"ok\":true,\"cmd\":\"load_path\",\"matched_row\":" + std::to_string(matchedRow)
                   + ",\"path\":\"" + jsonEscape(path) + "\"}";
        }

    } else if (cmd == "select_rows") {
        std::string rowsRaw = getStr(raw, "rows");
        std::set<int> rows = parseRowSet(rowsRaw);

        bool valid = !rows.empty();
        for (int row : rows) {
            if (row < 0 || row >= (int)state.searchResults.size()) {
                valid = false;
                break;
            }
        }
        if (!valid) {
            result = "{\"ok\":false,\"cmd\":\"select_rows\",\"error\":\"rows out of range\"}";
        } else {
            state.selectedIndicesSet = rows;
            state.selectedResultIndex = *rows.begin();
            state.lastClickedIndex = *rows.rbegin();
            state.syncToTab();
            result = "{\"ok\":true,\"cmd\":\"select_rows\",\"selected_count\":" + std::to_string((int)rows.size()) + "}";
        }

    } else if (cmd == "scroll_results") {
        if (state.viewMode != ViewMode::Database) {
            result = R"({"ok":false,"cmd":"scroll_results","error":"database view required"})";
        } else if (state.listTabs.empty()) {
            result = R"({"ok":false,"cmd":"scroll_results","error":"no active list tab"})";
        } else {
            const int rowHeight = (std::max)(state.qaRegions.listRowHeight, 1);
            const int visibleRows = (std::max)(state.qaRegions.listVisibleRows, 1);
            const int resultCount = (int)state.searchResults.size();
            const int maxTopRow = (std::max)(0, resultCount - visibleRows);
            int requestedTopRow = getInt(raw, "top_row", getInt(raw, "row", 0));
            requestedTopRow = std::clamp(requestedTopRow, 0, maxTopRow);
            state.currentTab().listScrollY = (float)(requestedTopRow * rowHeight);
            state.currentTab().restoreScrollY = true;
            state.scrollToSelected = false;
            DIAG_ACTION_FMT("Browse", "ScrollTopRow:%d", requestedTopRow);
            result = R"({"ok":true,"cmd":"scroll_results","top_row":)" + std::to_string(requestedTopRow)
                   + ",\"row_height\":" + std::to_string(rowHeight)
                   + ",\"visible_rows\":" + std::to_string(visibleRows)
                   + ",\"result_count\":" + std::to_string(resultCount) + "}";
        }

    } else if (cmd == "send_target_menu") {
#if ARIS_PREVIEW_ISOLATION
        const std::string action = getStr(raw, "action");
        if (!MagicEqQaIsolationReady()) {
            result = R"({"ok":false,"error":"fixed_preview_isolation_required"})";
        } else if (action == "open") {
            const int row = getInt(raw, "row", -1);
            const int targetIndex = getInt(raw, "target_index", -1);
            if (row < 0 || row >= (int)state.searchResults.size() ||
                targetIndex < 0 || targetIndex >= (int)state.settings.dawTargets.size() ||
                !Platform::HarkenUserData::PreviewIsolationAllowsDataPathUtf8(state.currentDbPathUtf8) ||
                !Platform::HarkenUserData::PreviewIsolationAllowsDataPathUtf8(state.searchResults[row].filePath)) {
                result = R"({"ok":false,"error":"invalid_isolated_target_or_row"})";
            } else {
                state.selectedIndicesSet = {row};
                state.selectedResultIndex = row;
                state.lastClickedIndex = row;
                state.syncToTab();
                state.qaSendTargetMenuRequested = targetIndex;
                state.qaSendTargetMenuRendered = -1;
                state.qaSendTargetMenuRenderedId = 0;
                state.qaOpenRowContextMenuRow = row;
                state.settings.showDiagnosticOverlay = false;
                result = R"({"ok":true,"cmd":"send_target_menu","queued":true})";
            }
        } else if (action == "state") {
            result = "{\"ok\":true,\"cmd\":\"send_target_menu\",\"isolation_root\":\"" +
                jsonEscape(Platform::HarkenUserData::PreviewIsolationRootUtf8()) +
                "\",\"runtime_root\":\"" + jsonEscape(runtimeRoot) +
                "\",\"rendered_index\":" + std::to_string(state.qaSendTargetMenuRendered) +
                ",\"rendered_id\":" + std::to_string(state.qaSendTargetMenuRenderedId) +
                ",\"frame\":" + std::to_string(state.qaSendTargetMenuFrame) + ",\"targets\":[";
            bool first = true;
            for (const auto& target : state.qaSendTargetMenuRows) {
                if (!first) result += ",";
                first = false;
                result += "{\"index\":" + std::to_string(target.index) +
                    ",\"label\":\"" + jsonEscape(target.label) +
                    "\",\"id\":" + std::to_string(target.id) + "}";
            }
            result += "]}";
        } else {
            result = R"({"ok":false,"error":"unsupported_action"})";
        }
#else
        result = R"({"ok":false,"error":"fixed_preview_isolation_required"})";
#endif
    } else if (cmd == "open_row_context_menu") {
        int row = getInt(raw, "row", state.selectedResultIndex);
        if (row >= 0 && row < (int)state.searchResults.size()) {
            state.selectedIndicesSet.clear();
            state.selectedIndicesSet.insert(row);
            state.selectedResultIndex = row;
            state.lastClickedIndex = row;
            state.syncToTab();
            state.qaOpenRowContextMenuRow = row;
            result = "{\"ok\":true,\"cmd\":\"open_row_context_menu\",\"row\":" + std::to_string(row) + "}";
        } else {
            result = "{\"ok\":false,\"cmd\":\"open_row_context_menu\",\"error\":\"row out of range\"}";
        }

    } else if (cmd == "open_playlist_context_menu") {
        int64_t playlistId = (int64_t)getInt(raw, "playlist_id", -1);
        if (playlistId <= 0 && !state.playlists.empty()) {
            playlistId = state.playlists.front().id;
        }
        bool found = false;
        for (const auto& playlist : state.playlists) {
            if (playlist.id == playlistId) {
                found = true;
                break;
            }
        }
        if (!found) {
            result = "{\"ok\":false,\"cmd\":\"open_playlist_context_menu\",\"error\":\"playlist not found\"}";
        } else {
            state.qaOpenSidebarPlaylistContextId = playlistId;
            result = "{\"ok\":true,\"cmd\":\"open_playlist_context_menu\",\"playlist_id\":" + std::to_string((long long)playlistId) + "}";
        }

    } else if (cmd == "remove_folder_subtree") {
        const std::string path = getStr(raw, "path");
        if (path.empty()) {
            result = "{\"ok\":false,\"cmd\":\"remove_folder_subtree\",\"error\":\"path missing\"}";
        } else if (!state.database.isOpen()) {
            result = "{\"ok\":false,\"cmd\":\"remove_folder_subtree\",\"error\":\"database not open\"}";
        } else {
            const int removed = state.database.removeFolderSubtreeFromDb(path, false);
            const std::string lastError = state.database.getLastError();
            FinalizeFolderSubtreeDbRemoval(state, path, removed);
            result = "{";
            result += "\"ok\":true,";
            result += "\"cmd\":\"remove_folder_subtree\",";
            result += "\"path\":\"" + jsonEscape(path) + "\",";
            result += "\"removed\":" + std::to_string(removed) + ",";
            result += "\"result_count\":" + std::to_string((int)state.searchResults.size()) + ",";
            result += "\"total_count\":" + std::to_string((long long)state.totalFileCount);
            if (!lastError.empty()) {
                result += ",\"last_error\":\"" + jsonEscape(lastError) + "\"";
            }
            result += "}";
        }

    } else if (cmd == "reorder_playlist_quickbar") {
        long long movedId = -1;
        long long targetId = -1;
        try {
            const std::string movedRaw = getStr(raw, "moved_id");
            const std::string targetRaw = getStr(raw, "target_id");
            if (!movedRaw.empty()) movedId = std::stoll(movedRaw);
            if (!targetRaw.empty()) targetId = std::stoll(targetRaw);
        } catch (...) {
            movedId = -1;
            targetId = -1;
        }
        if (movedId <= 0 || targetId <= 0 || movedId == targetId) {
            result = "{\"ok\":false,\"cmd\":\"reorder_playlist_quickbar\",\"error\":\"invalid playlist ids\"}";
        } else {
            applyPlaylistQuickBarOrderForQa();
            int movedIdx = -1;
            int targetIdx = -1;
            for (int i = 0; i < (int)state.playlists.size(); ++i) {
                if (state.playlists[(size_t)i].id == movedId) movedIdx = i;
                if (state.playlists[(size_t)i].id == targetId) targetIdx = i;
            }
            if (movedIdx < 0 || targetIdx < 0 || movedIdx == targetIdx) {
                result = "{\"ok\":false,\"cmd\":\"reorder_playlist_quickbar\",\"error\":\"playlist not found\"}";
            } else {
                Playlist moved = state.playlists[(size_t)movedIdx];
                state.playlists.erase(state.playlists.begin() + movedIdx);
                int insertIdx = targetIdx;
                if (movedIdx < targetIdx) {
                    insertIdx = targetIdx;
                }
                insertIdx = std::clamp(insertIdx, 0, (int)state.playlists.size());
                state.playlists.insert(state.playlists.begin() + insertIdx, moved);
                persistPlaylistQuickBarOrderForQa();
                SaveAppSettings(state);
                DIAG_ACTION_FMT("Playlist", "QaQuickBarReorder moved=%lld target=%lld", movedId, targetId);
                result = "{";
                result += "\"ok\":true,";
                result += "\"cmd\":\"reorder_playlist_quickbar\",";
                result += "\"moved_id\":" + std::to_string(movedId) + ",";
                result += "\"target_id\":" + std::to_string(targetId) + ",";
                result += "\"order\":" + buildInt64ArrayJson(state.settings.playlistQuickBarOrder);
                result += "}";
            }
        }

    } else if (cmd == "export_begin") {
        std::set<int> rows;
        const std::string rowsRaw = getStr(raw, "rows");
        if (!rowsRaw.empty()) {
            rows = parseRowSet(rowsRaw);
        } else if (!state.selectedIndicesSet.empty()) {
            rows = state.selectedIndicesSet;
        } else if (state.selectedResultIndex >= 0 && state.selectedResultIndex < (int)state.searchResults.size()) {
            rows.insert(state.selectedResultIndex);
        }
        bool valid = !rows.empty();
        for (int row : rows) {
            if (row < 0 || row >= (int)state.searchResults.size()) {
                valid = false;
                break;
            }
        }
        if (!valid) {
            result = "{\"ok\":false,\"cmd\":\"export_begin\",\"error\":\"no valid selection\"}";
        } else {
            state.selectedIndicesSet = rows;
            state.selectedResultIndex = *rows.begin();
            state.lastClickedIndex = *rows.rbegin();
            state.syncToTab();
            state.transferSelectedFiles.clear();
            state.transferSelectedFiles.reserve(rows.size());
            for (int row : rows) {
                state.transferSelectedFiles.push_back(state.searchResults[(size_t)row]);
            }
            state.transferInProgress = false;
            state.setTransferStatus(std::string(), AppState::TransferStatusKind::Neutral);  // H1c: also clears keyed state (main.cpp:5198 parity)
            state.transferCompletedCount = 0;
            state.transferErrorCount = 0;
            state.transferTotalCount = (int)state.transferSelectedFiles.size();
            applyDefaultTransferSettingsForQa();
            if (state.transferDestFolder[0] == '\0' && !state.settings.defaultExportPath.empty()) {
                strncpy_s(
                    state.transferDestFolder,
                    sizeof(state.transferDestFolder),
                    state.settings.defaultExportPath.c_str(),
                    _TRUNCATE);
            }
            state.showTransferDialog = true;
            result = "{";
            result += "\"ok\":true,";
            result += "\"cmd\":\"export_begin\",";
            result += "\"selected_count\":" + std::to_string((int)state.transferSelectedFiles.size()) + ",";
            result += "\"dest_folder\":\"" + jsonEscape(state.transferDestFolder) + "\"";
            result += "}";
        }

    } else if (cmd == "set_export_options") {
        const bool hasDestFolder = raw.find("\"dest_folder\"") != std::string::npos;
        const bool hasFolderStrategy = raw.find("\"folder_strategy\"") != std::string::npos;
        const bool hasFileType = raw.find("\"file_type\"") != std::string::npos;
        const bool hasExistingBehavior = raw.find("\"existing_behavior\"") != std::string::npos;
        const bool hasUseSourceFormat = raw.find("\"use_source_format\"") != std::string::npos;
        const bool hasEmbedMetadata = raw.find("\"embed_metadata\"") != std::string::npos;
        if (!hasDestFolder && !hasFolderStrategy && !hasFileType &&
            !hasExistingBehavior && !hasUseSourceFormat && !hasEmbedMetadata) {
            result = "{\"ok\":false,\"cmd\":\"set_export_options\",\"error\":\"no fields provided\"}";
        } else {
            if (hasDestFolder) {
                std::string destFolder = getStr(raw, "dest_folder");
                strncpy_s(state.transferDestFolder, sizeof(state.transferDestFolder), destFolder.c_str(), _TRUNCATE);
                state.settings.defaultExportPath = destFolder;
            }
            if (hasFolderStrategy) {
                state.transferFolderStrategy = std::max(0, getInt(raw, "folder_strategy", state.transferFolderStrategy));
                state.settings.defaultTransferFolderStrategy = state.transferFolderStrategy;
            }
            if (hasFileType) {
                state.transferFileType = std::max(0, getInt(raw, "file_type", state.transferFileType));
                state.settings.defaultTransferFileType = state.transferFileType;
            }
            if (hasExistingBehavior) {
                state.transferExistingBehavior = std::max(0, getInt(raw, "existing_behavior", state.transferExistingBehavior));
                state.settings.defaultTransferExistingBehavior = state.transferExistingBehavior;
            }
            if (hasUseSourceFormat) {
                state.transferUseSourceFormat = getBool(raw, "use_source_format", state.transferUseSourceFormat);
                state.settings.defaultTransferUseSourceFormat = state.transferUseSourceFormat;
            }
            if (hasEmbedMetadata) {
                state.transferEmbedMetadata = getBool(raw, "embed_metadata", state.transferEmbedMetadata);
                state.settings.defaultTransferEmbedMetadata = state.transferEmbedMetadata;
            }
            MarkSettingsDirty(state);
            SaveAppSettings(state);
            result = "{";
            result += "\"ok\":true,";
            result += "\"cmd\":\"set_export_options\",";
            result += "\"dest_folder\":\"" + jsonEscape(state.transferDestFolder) + "\",";
            result += "\"folder_strategy\":" + std::to_string(state.transferFolderStrategy) + ",";
            result += "\"file_type\":" + std::to_string(state.transferFileType) + ",";
            result += "\"existing_behavior\":" + std::to_string(state.transferExistingBehavior) + ",";
            result += "\"use_source_format\":" + std::string(state.transferUseSourceFormat ? "true" : "false") + ",";
            result += "\"embed_metadata\":" + std::string(state.transferEmbedMetadata ? "true" : "false");
            result += "}";
        }

    } else if (cmd == "start_export") {
        const std::string destDir = state.transferDestFolder;
        if (state.transferInProgress) {
            result = "{\"ok\":true,\"cmd\":\"start_export\",\"already_running\":true}";
        } else if (state.transferSelectedFiles.empty() || state.transferTotalCount <= 0) {
            result = "{\"ok\":false,\"cmd\":\"start_export\",\"error\":\"no files queued\"}";
        } else if (destDir.empty()) {
            result = "{\"ok\":false,\"cmd\":\"start_export\",\"error\":\"destination missing\"}";
        } else if (Platform::FileSystem::IsNetworkPath(destDir)) {
            state.setTransferStatus(L(Write_BlockedNetworkLibrary), AppState::TransferStatusKind::Error);
            result = "{\"ok\":false,\"cmd\":\"start_export\",\"error\":\"network destination blocked\"}";
        } else {
            state.settings.defaultExportPath = destDir;
            state.settings.defaultTransferFolderStrategy = state.transferFolderStrategy;
            state.settings.defaultTransferFileType = state.transferFileType;
            state.settings.defaultTransferExistingBehavior = state.transferExistingBehavior;
            state.settings.defaultTransferUseSourceFormat = state.transferUseSourceFormat;
            state.settings.defaultTransferEmbedMetadata = state.transferEmbedMetadata;
            MarkSettingsDirty(state);
            std::string destinationProbeError;
            const auto destinationStatus =
                Platform::FileSystem::ProbePath(destDir, &destinationProbeError);
            bool destinationCreateFailed = false;
            if (destinationStatus == Platform::FileSystem::PathStatus::Missing) {
                destinationCreateFailed = !Platform::FileSystem::CreateDirectories(destDir);
            }
            if (destinationStatus == Platform::FileSystem::PathStatus::Error || destinationCreateFailed) {
                const std::string destinationError =
                    destinationStatus == Platform::FileSystem::PathStatus::Error
                        ? destinationProbeError
                        : std::string("create directories failed");
                { char wtb1[512]; std::snprintf(wtb1, sizeof(wtb1), L(TR_MkdirFailFmt), destinationError.c_str());
                state.setTransferStatus(wtb1, AppState::TransferStatusKind::Error); }
                result = "{\"ok\":false,\"cmd\":\"start_export\",\"error\":\"create destination failed\"}";
            } else if (!Platform::FileSystem::IsDirectory(destDir)) {
                state.setTransferStatus(L(TR_NotFolder), AppState::TransferStatusKind::Error);
                result = "{\"ok\":false,\"cmd\":\"start_export\",\"error\":\"destination is not a folder\"}";
            } else {
                SaveAppSettings(state);
                state.transferInProgress = true;
                state.transferCompletedCount = 0;
                state.transferErrorCount = 0;
                state.setTransferStatus("Exporting", AppState::TransferStatusKind::Neutral);
                const auto strategies = TransferEngine::getFolderStrategyList();
                const auto behaviors = TransferEngine::getExistingBehaviorChoices();
                const FolderNamingStrategy folderStrategy =
                    state.transferFolderStrategy >= 0 && state.transferFolderStrategy < (int)strategies.size()
                        ? strategies[(size_t)state.transferFolderStrategy].first
                        : FolderNamingStrategy::SingleFolder;
                const ExistingFileBehavior existingBehavior =
                    state.transferExistingBehavior >= 0 && state.transferExistingBehavior < (int)behaviors.size()
                        ? behaviors[(size_t)state.transferExistingBehavior].second
                        : ExistingFileBehavior::SkipExisting;
                std::thread([&state, destDir, folderStrategy, existingBehavior]() {
                    int completed = 0;
                    int errors = 0;
                    for (const auto& record : state.transferSelectedFiles) {
                        if (!state.transferInProgress) break;
                        std::unordered_map<std::string, std::string> metadata;
                        metadata["Category"] = record.category;
                        metadata["Library"] = record.album;
                        metadata["Designer"] = record.artist;
                        std::string errMsg;
                        std::string subFolder = TransferEngine::generateDestFolderPath(
                            folderStrategy, destDir, metadata, errMsg);
                        if (subFolder.empty()) subFolder = destDir;
                        Platform::FileSystem::CreateDirectories(subFolder);
                        TransferPrefs transferPrefs = TransferEngine::getDefaultPrefs();
                        std::string safeFileName =
                            TransferEngine::buildSafeDestinationFileName(record.fileName, transferPrefs);
                        std::string destFile = subFolder;
                        if (!destFile.empty() && destFile.back() != '/' && destFile.back() != '\\') {
                            destFile += "\\";
                        }
                        destFile += safeFileName;
                        TransferDestinationResolution resolved =
                            TransferEngine::resolveExistingDestinationPath(destFile, existingBehavior);
                        if (!resolved.errorMessage.empty()) {
                            ++errors;
                            state.setTransferStatus("Error: " + record.fileName + " - " + resolved.errorMessage, AppState::TransferStatusKind::Neutral);
                            state.transferErrorCount = errors;
                            continue;
                        }
                        if (!resolved.shouldCopy) {
                            ++completed;
                            state.transferCompletedCount = completed;
                            continue;
                        }
                        destFile = resolved.path;
                        try {
                            std::string copyError;
                            if (!Platform::FileSystem::CopyFile(
                                    record.filePath, destFile, resolved.wouldOverwrite, &copyError)) {
                                ++errors;
                                state.setTransferStatusFmt(LStr::TR_CopyErrFmt, AppState::TransferStatusKind::Error,
                                                         record.fileName, copyError);
                            } else {
                                ++completed;
                            }
                        } catch (const std::exception& ex) {
                            ++errors;
                            state.setTransferStatusFmt(LStr::TR_CopyExFmt, AppState::TransferStatusKind::Fail,
                                                     record.fileName, std::string(ex.what()));
                        }
                        state.transferCompletedCount = completed;
                        state.transferErrorCount = errors;
                    }
                    state.transferInProgress = false;
                    if (errors == 0) {
                        state.setTransferStatusCount(LStr::TR_DoneAllFmt, AppState::TransferStatusKind::Neutral, completed, -1);
                    } else {
                        state.setTransferStatusCount(LStr::TR_DoneMixedFmt, AppState::TransferStatusKind::Fail, completed, errors);  // P1-1 parity
                    }
                }).detach();
                result = "{\"ok\":true,\"cmd\":\"start_export\",\"started\":true}";
            }
        }

    } else if (cmd == "cancel_export") {
        const bool wasActive = state.transferInProgress;
        if (wasActive) {
            state.transferInProgress = false;
            state.setTransferStatus("Cancelled", AppState::TransferStatusKind::Neutral);
        }
        result = "{\"ok\":true,\"cmd\":\"cancel_export\",\"was_active\":"
               + std::string(wasActive ? "true" : "false") + "}";

    } else if (cmd == "get_row") {
        int row = getInt(raw, "row", -1);
        if (row >= 0 && row < (int)state.searchResults.size()) {
            result = "{\"ok\":true,\"cmd\":\"get_row\",\"item\":" + buildRowJson(row) + "}";
        } else {
            result = "{\"ok\":false,\"cmd\":\"get_row\",\"error\":\"row out of range\"}";
        }

    } else if (cmd == "metadata_begin") {
        std::vector<int> rows;
        if (!state.selectedIndicesSet.empty()) {
            rows.assign(state.selectedIndicesSet.begin(), state.selectedIndicesSet.end());
        } else if (state.selectedResultIndex >= 0 && state.selectedResultIndex < (int)state.searchResults.size()) {
            rows.push_back(state.selectedResultIndex);
            state.selectedIndicesSet.insert(state.selectedResultIndex);
        }
        if (rows.empty()) {
            result = "{\"ok\":false,\"cmd\":\"metadata_begin\",\"error\":\"no selection\"}";
        } else {
            state.metadataEditMode = true;
            state.showMetadataEditDialog = true;
            if (rows.size() > 1) {
                state.metadataBatchMode = true;
                state.metadataEditFileId = -1;
                state.metadataBatchIds.clear();
                state.metadataDescBuffer[0] = '\0';
                state.metadataKeywordsBuffer[0] = '\0';
                state.metadataCategoryBuffer[0] = '\0';
                for (int row : rows) {
                    state.metadataBatchIds.push_back(state.searchResults[(size_t)row].id);
                }
            } else {
                const int row = rows.front();
                const AudioRecord& rec = state.searchResults[(size_t)row];
                state.metadataBatchMode = false;
                state.metadataBatchIds.clear();
                state.metadataEditFileId = rec.id;
                strncpy_s(state.metadataDescBuffer, sizeof(state.metadataDescBuffer), rec.description.c_str(), _TRUNCATE);
                strncpy_s(state.metadataKeywordsBuffer, sizeof(state.metadataKeywordsBuffer), rec.keywords.c_str(), _TRUNCATE);
                strncpy_s(state.metadataCategoryBuffer, sizeof(state.metadataCategoryBuffer), rec.category.c_str(), _TRUNCATE);
            }
            result = "{\"ok\":true,\"cmd\":\"metadata_begin\",\"selected_count\":" + std::to_string((int)rows.size())
                + ",\"batch_mode\":" + std::string(state.metadataBatchMode ? "true" : "false")
                + ",\"dialog_visible\":true}";
        }

    } else if (cmd == "metadata_end") {
        state.metadataEditMode = false;
        state.showMetadataEditDialog = false;
        state.metadataBatchMode = false;
        state.metadataBatchIds.clear();
        result = "{\"ok\":true,\"cmd\":\"metadata_end\"}";

    } else if (cmd == "metadata_state") {
        std::vector<int> rows;
        rows.assign(state.selectedIndicesSet.begin(), state.selectedIndicesSet.end());
        if (rows.empty() && state.selectedResultIndex >= 0 && state.selectedResultIndex < (int)state.searchResults.size()) {
            rows.push_back(state.selectedResultIndex);
        }
        if (rows.empty()) {
            result = "{\"ok\":false,\"cmd\":\"metadata_state\",\"error\":\"no selection\"}";
        } else {
            std::string rowItems = "[";
            std::string idItems = "[";
            std::string description;
            std::string keywords;
            std::string category;
            bool descriptionMixed = false;
            bool keywordsMixed = false;
            bool categoryMixed = false;
            bool first = true;
            for (int row : rows) {
                const auto& rec = state.searchResults[(size_t)row];
                if (!first) {
                    rowItems += ",";
                    idItems += ",";
                }
                rowItems += std::to_string(row);
                idItems += std::to_string((long long)rec.id);
                if (first) {
                    description = rec.description;
                    keywords = rec.keywords;
                    category = rec.category;
                    first = false;
                } else {
                    descriptionMixed = descriptionMixed || rec.description != description;
                    keywordsMixed = keywordsMixed || rec.keywords != keywords;
                    categoryMixed = categoryMixed || rec.category != category;
                }
            }
            rowItems += "]";
            idItems += "]";
            if (descriptionMixed) description.clear();
            if (keywordsMixed) keywords.clear();
            if (categoryMixed) category.clear();
            result = "{";
            result += "\"ok\":true,";
            result += "\"cmd\":\"metadata_state\",";
            result += "\"dialog_visible\":" + std::string((state.showMetadataEditDialog || state.metadataEditMode) ? "true" : "false") + ",";
            result += "\"batch_mode\":" + std::string((rows.size() > 1 || state.metadataBatchMode) ? "true" : "false") + ",";
            result += "\"selected_count\":" + std::to_string((int)rows.size()) + ",";
            result += "\"rows\":" + rowItems + ",";
            result += "\"ids\":" + idItems + ",";
            result += "\"description\":\"" + jsonEscape(description) + "\",";
            result += "\"keywords\":\"" + jsonEscape(keywords) + "\",";
            result += "\"category\":\"" + jsonEscape(category) + "\",";
            result += "\"description_mixed\":" + std::string(descriptionMixed ? "true" : "false") + ",";
            result += "\"keywords_mixed\":" + std::string(keywordsMixed ? "true" : "false") + ",";
            result += "\"category_mixed\":" + std::string(categoryMixed ? "true" : "false");
            result += "}";
        }

    } else if (cmd == "metadata_apply_patch") {
        const bool hasDescription = raw.find("\"description\"") != std::string::npos;
        const bool hasKeywords = raw.find("\"keywords\"") != std::string::npos;
        const bool hasCategory = raw.find("\"category\"") != std::string::npos;
        std::string newDescription = getStr(raw, "description");
        std::string newKeywords = getStr(raw, "keywords");
        std::string newCategory = getStr(raw, "category");
        if (!hasDescription && !hasKeywords && !hasCategory) {
            result = "{\"ok\":false,\"cmd\":\"metadata_apply_patch\",\"error\":\"no fields provided\"}";
        } else {
            std::vector<int> rows;
            rows.assign(state.selectedIndicesSet.begin(), state.selectedIndicesSet.end());
            if (rows.empty() && state.selectedResultIndex >= 0 && state.selectedResultIndex < (int)state.searchResults.size()) {
                rows.push_back(state.selectedResultIndex);
            }
            if (rows.empty()) {
                result = "{\"ok\":false,\"cmd\":\"metadata_apply_patch\",\"error\":\"no selection\"}";
            } else {
                int successCount = 0;
                int skippedCount = 0;
                int failedCount = 0;
                auto undoOp = std::make_unique<MetadataPatchOp>();
                undoOp->operationName = rows.size() > 1 ? "Batch edit metadata" : "Edit metadata";

                const bool useTxn = rows.size() > 1;
                bool txnStarted = !useTxn || state.database.beginTransaction();
                bool abort = !txnStarted;
                if (txnStarted) {
                    for (int row : rows) {
                        const auto& rec = state.searchResults[(size_t)row];
                        if (!Platform::FileSystem::Exists(rec.filePath)) {
                            skippedCount++;
                            continue;
                        }

                        MetadataEditPatch patch;
                        MetadataPatchEntry entry;
                        entry.fileId = rec.id;
                        if (hasDescription) {
                            patch.setDescription(newDescription);
                            entry.oldValues["description"] = rec.description;
                            entry.newValues["description"] = newDescription;
                        }
                        if (hasKeywords) {
                            patch.setKeywords(newKeywords);
                            entry.oldValues["keywords"] = rec.keywords;
                            entry.newValues["keywords"] = newKeywords;
                        }
                        if (hasCategory) {
                            patch.setCategory(newCategory);
                            entry.oldValues["category"] = rec.category;
                            entry.newValues["category"] = newCategory;
                        }
                        undoOp->entries.push_back(std::move(entry));
                        if (state.database.applyMetadataPatch(rec.id, patch)) {
                            successCount++;
                        } else {
                            failedCount++;
                            abort = true;
                            break;
                        }
                    }
                }

                if (useTxn) {
                    if (abort || !state.database.commitTransaction()) {
                        state.database.rollbackTransaction();
                        successCount = 0;
                        failedCount = (int)rows.size();
                        undoOp->entries.clear();
                    }
                }

                if (successCount > 0) {
                    state.undoStack.push(std::move(undoOp));
                    state.database.invalidateFacetQueryCache();
                    RefreshSearchResults(state, false);
                    state.syncToTab();
                }
                state.metadataEditMode = false;
                state.showMetadataEditDialog = false;
                state.metadataBatchMode = false;
                state.metadataBatchIds.clear();
                result = "{\"ok\":true,\"cmd\":\"metadata_apply_patch\",\"success\":" + std::to_string(successCount)
                    + ",\"skipped\":" + std::to_string(skippedCount)
                    + ",\"failed\":" + std::to_string(failedCount)
                    + ",\"selected_count\":" + std::to_string((int)rows.size()) + "}";
            }
        }

    } else if (cmd == "undo") {
        const bool applied = state.undoStack.undo(state.database);
        if (applied) {
            state.database.invalidateFacetQueryCache();
            RefreshSearchResults(state, true);
            state.syncToTab();
        }
        result = "{\"ok\":true,\"cmd\":\"undo\",\"applied\":" + std::string(applied ? "true" : "false")
            + ",\"undo_count\":" + std::to_string(state.undoStack.undoCount())
            + ",\"redo_count\":" + std::to_string(state.undoStack.redoCount()) + "}";

    } else if (cmd == "redo") {
        const bool applied = state.undoStack.redo(state.database);
        if (applied) {
            state.database.invalidateFacetQueryCache();
            RefreshSearchResults(state, true);
            state.syncToTab();
        }
        result = "{\"ok\":true,\"cmd\":\"redo\",\"applied\":" + std::string(applied ? "true" : "false")
            + ",\"undo_count\":" + std::to_string(state.undoStack.undoCount())
            + ",\"redo_count\":" + std::to_string(state.undoStack.redoCount()) + "}";

    } else if (cmd == "play") {
        state.audioEngine.play();
        result = "{\"ok\":true,\"cmd\":\"play\"}";

    } else if (cmd == "stop") {
        state.audioEngine.stop();
        result = "{\"ok\":true,\"cmd\":\"stop\"}";

    } else if (cmd == "pause") {
        state.audioEngine.pause();
        result = "{\"ok\":true,\"cmd\":\"pause\"}";

    } else if (cmd == "set_spectrogram") {
        const bool enabled = getBool(raw, "enabled", state.showSpectrogram);
        state.showSpectrogram = enabled;
        state.spectrogramNeedsUpdate = enabled;
        DIAG_ACTION_FMT("Spectrogram", "Toggle:%d", enabled ? 1 : 0);
        result = std::string("{\"ok\":true,\"cmd\":\"set_spectrogram\",\"enabled\":")
               + (enabled ? "true" : "false") + "}";

    } else if (cmd == "set_spectrogram_fft") {
        int fftSize = getInt(raw, "fft_size", state.spectrogramFFTSize);
        fftSize = std::clamp(fftSize, 512, 16384);
        int normalized = 512;
        while (normalized < fftSize) normalized <<= 1;
        if (normalized > 16384) normalized = 16384;
        state.spectrogramFFTSize = normalized;
        state.spectrogramNeedsUpdate = true;
        DIAG_ACTION_FMT("Spectrogram", "FftSize:%d", normalized);
        result = "{\"ok\":true,\"cmd\":\"set_spectrogram_fft\",\"fft_size\":" + std::to_string(normalized) + "}";

    } else if (cmd == "set_memory_profile") {
        int profile = getInt(raw, "profile", -1);
        if (profile >= 0 && profile <= 3) {
            state.settings.memoryProfile = profile;
            SaveAppSettings(state);
            result = "{\"ok\":true,\"cmd\":\"set_memory_profile\",\"profile\":" + std::to_string(profile) + "}";
        } else {
            result = "{\"ok\":false,\"cmd\":\"set_memory_profile\",\"error\":\"profile out of range\"}";
        }

    } else if (cmd == "set_loudness_norm") {
        bool enabled = getBool(raw, "enabled", state.settings.loudnessNormEnabled);
        state.settings.loudnessNormEnabled = enabled;
        SaveAppSettings(state);
        result = std::string("{\"ok\":true,\"cmd\":\"set_loudness_norm\",\"enabled\":")
               + (enabled ? "true" : "false") + "}";

    } else if (cmd == "set_theme") {
        int theme = getInt(raw, "theme", -1);
        const bool validTheme =
            theme == (int)AppTheme::Dark ||
            theme == (int)AppTheme::Ocean ||
            theme == (int)AppTheme::LapisBlue ||
            theme == (int)AppTheme::JadeMist ||
            theme == (int)AppTheme::Custom ||
            theme == (int)AppTheme::RoseCedar;
        if (!validTheme) {
            result = "{\"ok\":false,\"cmd\":\"set_theme\",\"error\":\"invalid theme\"}";
        } else if (theme == (int)AppTheme::Custom &&
                   state.settings.customThemeName.empty() &&
                   !ThemeColors::customized()) {
            result = "{\"ok\":false,\"cmd\":\"set_theme\",\"error\":\"custom theme unavailable\"}";
        } else {
            state.settings.theme = (AppTheme)theme;
            MarkSettingsDirty(state);
            SaveAppSettings(state);
            result = "{\"ok\":true,\"cmd\":\"set_theme\",\"theme\":" + std::to_string(theme) + "}";
        }

    } else if (cmd == "switch_db") {
        std::string path = getStr(raw, "path");
        if (path.empty()) {
            result = "{\"ok\":false,\"cmd\":\"switch_db\",\"error\":\"path missing\"}";
        } else if (!PathExistsUtf8(path)) {
            result = "{\"ok\":false,\"cmd\":\"switch_db\",\"error\":\"path missing\"}";
        } else if (path == state.currentDbPathUtf8) {
            result = "{\"ok\":true,\"cmd\":\"switch_db\",\"started\":false,\"skipped\":true,\"path\":\"" + jsonEscape(path) + "\"}";
        } else {
            const bool started = BeginSwitchDatabase(state, path);
            result = "{\"ok\":" + std::string(started ? "true" : "false")
                   + ",\"cmd\":\"switch_db\",\"started\":" + std::string(started ? "true" : "false")
                   + ",\"path\":\"" + jsonEscape(path) + "\"}";
        }

    } else if (cmd == "switch_library") {
        std::string path = getStr(raw, "path");
        if (path.empty()) {
            result = "{\"ok\":false,\"cmd\":\"switch_library\",\"error\":\"path missing\"}";
        } else if (!PathExistsUtf8(path)) {
            result = "{\"ok\":false,\"cmd\":\"switch_library\",\"error\":\"path missing\"}";
        } else if (path == state.currentDbPathUtf8) {
            result = "{\"ok\":true,\"cmd\":\"switch_library\",\"started\":false,\"skipped\":true,\"path\":\"" + jsonEscape(path) + "\"}";
        } else {
            const bool started = BeginSwitchDatabase(state, path);
            result = "{\"ok\":" + std::string(started ? "true" : "false")
                   + ",\"cmd\":\"switch_library\",\"started\":" + std::string(started ? "true" : "false")
                   + ",\"path\":\"" + jsonEscape(path) + "\"}";
        }

    } else if (cmd == "folder_shortcut") {
#if ARIS_PREVIEW_ISOLATION
        // Exercise the same bookmark and navigation handlers as the UI,
        // without injecting input into the user's desktop.
        extern bool AddBrowserFolderToQuickPaths(AppState&, const std::string&);
        extern void OpenFolderShortcut(AppState&, const FolderShortcut&);
        const std::string path = getStr(raw, "bookmark_path");
        const int index = getInt(raw, "open_index", -1);
        if (!MagicEqQaIsolationReady() || !s_mainUiReady.load(std::memory_order_acquire) ||
            (!path.empty() && !Platform::HarkenUserData::PreviewIsolationAllowsDataPathUtf8(path)) ||
            index < -1 || index >= static_cast<int>(state.folderShortcuts.size()) ||
            (index >= 0 && !Platform::HarkenUserData::PreviewIsolationAllowsDataPathUtf8(state.folderShortcuts[index].path))) {
            result = R"({"ok":false,"error":"isolated_shortcut_required"})";
        } else {
            if (!path.empty()) AddBrowserFolderToQuickPaths(state, path);
            if (index >= 0) OpenFolderShortcut(state, state.folderShortcuts[index]);
            result = "{\"ok\":true,\"mode\":\"" + std::string(viewModeToString(state.viewMode)) +
                "\",\"browser_path\":\"" + jsonEscape(state.fileBrowser.getCurrentPath()) +
                "\",\"entries\":" + std::to_string(state.fileBrowser.activeEntries().size()) +
                ",\"shortcut_count\":" + std::to_string(state.folderShortcuts.size()) + "}";
        }
#else
        result = R"({"ok":false,"error":"preview_isolation_disabled"})";
#endif
    } else if (cmd == "set_view_mode") {
        std::string mode = getStr(raw, "mode");
        std::transform(mode.begin(), mode.end(), mode.begin(), ::tolower);
        if (mode == "browser") {
            state.viewMode = ViewMode::Browser;
            state.syncToTab();
            DIAG_ACTION("Navigation", "ViewMode:Browser");
            result = "{\"ok\":true,\"cmd\":\"set_view_mode\",\"mode\":\"browser\"}";
        } else if (mode == "database") {
            state.viewMode = ViewMode::Database;
            state.suppressFacetsForNextRefresh = true;
            state.preferDeferredCountForNextRefresh = !ShouldUseHarnessImmediateFilterRefresh();
            RefreshSearchResults(state, true);
            state.syncToTab();
            DIAG_ACTION("Navigation", "ViewMode:Database");
            result = "{\"ok\":true,\"cmd\":\"set_view_mode\",\"mode\":\"database\"}";
        } else {
            result = "{\"ok\":false,\"cmd\":\"set_view_mode\",\"error\":\"unknown mode\"}";
        }

    } else if (cmd == "open_scan_dialog") {
        state.scanDialogAutoStart = false;
        state.showScanDialog = true;
        DIAG_ACTION("Navigation", "ScanDialogOpen");
        result = "{\"ok\":true,\"cmd\":\"open_scan_dialog\"}";

    } else if (cmd == "toggle_panel") {
        std::string panel = getStr(raw, "panel");
        std::transform(panel.begin(), panel.end(), panel.begin(), ::tolower);
        if (panel == "filter") {
            state.panelSidebar.visible = !state.panelSidebar.visible;
            DIAG_ACTION_FMT("Panel", "Toggle:Filter:visible=%d", state.panelSidebar.visible ? 1 : 0);
            result = "{\"ok\":true,\"cmd\":\"toggle_panel\",\"panel\":\"filter\",\"visible\":" + std::string(state.panelSidebar.visible ? "true" : "false") + "}";
        } else if (panel == "info") {
            state.panelFileInfo.visible = !state.panelFileInfo.visible;
            DIAG_ACTION_FMT("Panel", "Toggle:Info:visible=%d", state.panelFileInfo.visible ? 1 : 0);
            result = "{\"ok\":true,\"cmd\":\"toggle_panel\",\"panel\":\"info\",\"visible\":" + std::string(state.panelFileInfo.visible ? "true" : "false") + "}";
        } else if (panel == "waveform") {
            state.panelWaveform.visible = !state.panelWaveform.visible;
            DIAG_ACTION_FMT("Panel", "Toggle:Waveform:visible=%d", state.panelWaveform.visible ? 1 : 0);
            result = "{\"ok\":true,\"cmd\":\"toggle_panel\",\"panel\":\"waveform\",\"visible\":" + std::string(state.panelWaveform.visible ? "true" : "false") + "}";
        } else if (panel == "loudness") {
            state.showLoudnessMeter = !state.showLoudnessMeter;
            DIAG_ACTION_FMT("Panel", "Toggle:Loudness:visible=%d", state.showLoudnessMeter ? 1 : 0);
            result = "{\"ok\":true,\"cmd\":\"toggle_panel\",\"panel\":\"loudness\",\"visible\":" + std::string(state.showLoudnessMeter ? "true" : "false") + "}";
        } else {
            result = "{\"ok\":false,\"cmd\":\"toggle_panel\",\"error\":\"unknown panel\"}";
        }

    } else if (cmd == "set_folder_filter") {
        DiagScopedTimer qaFolderTimer("Filter", "folderApply");
        std::string path = getStr(raw, "path");
        if (path.empty()) {
            result = "{\"ok\":false,\"cmd\":\"set_folder_filter\",\"error\":\"path missing\"}";
        } else {
            state.viewMode = ViewMode::Database;
            state.folderFilterActive = true;
            // External folder commands store a canonical filter path.
            state.selectedFolderPath = HarkenSearch::CanonicalFolderPath(path);
            state.coverFilterActive = false;
            state.coverFilterPath.clear();
            cancelPendingSearchForQaMutation();
            state.preferDeferredCountForNextRefresh = !ShouldUseHarnessImmediateFilterRefresh();
            RefreshSearchResults(state, true);
            state.syncToTab();
            DIAG_ACTION_FMT("Filter", "FolderApply:%s", path.c_str());
            result = "{\"ok\":true,\"cmd\":\"set_folder_filter\",\"path\":\"" + jsonEscape(path) + "\"}";
        }

    } else if (cmd == "clear_folder_filter") {
        state.folderFilterActive = false;
        state.selectedFolderPath.clear();
        cancelPendingSearchForQaMutation();
        state.preferDeferredCountForNextRefresh = !ShouldUseHarnessImmediateFilterRefresh();
        RefreshSearchResults(state, true);
        state.syncToTab();
        DIAG_ACTION("Filter", "FolderClear");
        result = "{\"ok\":true,\"cmd\":\"clear_folder_filter\"}";

    } else if (cmd == "set_metadata_filter") {
        DiagScopedTimer qaMetadataTimer("Filter", "metadataApply");
        const std::string artist = getStr(raw, "artist");
        const std::string album = getStr(raw, "album");
        const std::string category = getStr(raw, "category");
        const std::string format = getStr(raw, "format");
        std::string appliedKind;
        std::string appliedValue;
        if (!artist.empty()) {
            strncpy_s(state.artistFilterBuffer, sizeof(state.artistFilterBuffer), artist.c_str(), _TRUNCATE);
            state.artistFilterAlternates.clear();
            appliedKind = "artist";
            appliedValue = artist;
        }
        if (!album.empty()) {
            strncpy_s(state.albumFilterBuffer, sizeof(state.albumFilterBuffer), album.c_str(), _TRUNCATE);
            appliedKind = "album";
            appliedValue = album;
        }
        if (!category.empty()) {
            strncpy_s(state.categoryFilterBuffer, sizeof(state.categoryFilterBuffer), category.c_str(), _TRUNCATE);
            appliedKind = "category";
            appliedValue = category;
        }
        if (!format.empty()) {
            state.formatFilterIndex = formatFilterIndexFromValue(format);
            appliedKind = "format";
            appliedValue = formatFilterValue(state.formatFilterIndex);
        }
        if (appliedKind.empty()) {
            result = "{\"ok\":false,\"cmd\":\"set_metadata_filter\",\"error\":\"no supported filter provided\"}";
        } else {
            state.viewMode = ViewMode::Database;
            cancelPendingSearchForQaMutation();
            state.preferDeferredCountForNextRefresh = !ShouldUseHarnessImmediateFilterRefresh();
            RefreshSearchResults(state, true);
            state.syncToTab();
            DIAG_ACTION_FMT("Filter", "MetadataApply:%s=%s", appliedKind.c_str(), appliedValue.c_str());
            result = "{\"ok\":true,\"cmd\":\"set_metadata_filter\",\"kind\":\"" + appliedKind + "\",\"value\":\"" + jsonEscape(appliedValue) + "\"}";
        }

    } else if (cmd == "clear_metadata_filter") {
        state.artistFilterBuffer[0] = '\0';
        state.artistFilterAlternates.clear();
        state.albumFilterBuffer[0] = '\0';
        state.categoryFilterBuffer[0] = '\0';
        state.formatFilterIndex = 0;
        cancelPendingSearchForQaMutation();
        state.preferDeferredCountForNextRefresh = !ShouldUseHarnessImmediateFilterRefresh();
        RefreshSearchResults(state, true);
        state.syncToTab();
        DIAG_ACTION("Filter", "MetadataClear");
        result = "{\"ok\":true,\"cmd\":\"clear_metadata_filter\"}";

    } else if (cmd == "set_cover_filter") {
        DiagScopedTimer qaCoverTimer("Filter", "coverApply");
        std::string mode = getStr(raw, "mode");
        std::transform(mode.begin(), mode.end(), mode.begin(), ::tolower);
        int coverIndex = 0;
        if (mode == "has" || mode == "with" || mode == "covered") {
            coverIndex = 1;
            mode = "has";
        } else if (mode == "no" || mode == "without" || mode == "none") {
            coverIndex = 2;
            mode = "no";
        } else if (mode == "all" || mode.empty()) {
            coverIndex = 0;
            mode = "all";
        } else {
            result = "{\"ok\":false,\"cmd\":\"set_cover_filter\",\"error\":\"invalid mode\"}";
        }
        if (result.empty()) {
            state.viewMode = ViewMode::Database;
            state.coverFilterIndex = coverIndex;
            state.coverFilterActive = false;
            state.coverFilterPath.clear();
            state.coverFilterMatchSig = 0;
            state.coverFilterMatchPaths.clear();
            cancelPendingSearchForQaMutation();
            state.preferDeferredCountForNextRefresh = !ShouldUseHarnessImmediateFilterRefresh();
            RefreshSearchResults(state, true);
            state.syncToTab();
            DIAG_ACTION_FMT("Filter", "CoverApply:%s", mode.c_str());
            result = "{\"ok\":true,\"cmd\":\"set_cover_filter\",\"mode\":\"" + mode + "\"}";
        }

    } else if (cmd == "clear_cover_filter") {
        state.coverFilterIndex = 0;
        state.coverFilterActive = false;
        state.coverFilterPath.clear();
        state.coverFilterMatchSig = 0;
        state.coverFilterMatchPaths.clear();
        cancelPendingSearchForQaMutation();
        state.preferDeferredCountForNextRefresh = !ShouldUseHarnessImmediateFilterRefresh();
        RefreshSearchResults(state, true);
        state.syncToTab();
        DIAG_ACTION("Filter", "CoverClear");
        result = "{\"ok\":true,\"cmd\":\"clear_cover_filter\"}";

    } else if (cmd == "set_specific_cover_filter") {
        DiagScopedTimer qaSpecificCoverTimer("Filter", "specificCoverApply");
        const std::string key = getStr(raw, "key");
        if (key.empty()) {
            result = "{\"ok\":false,\"cmd\":\"set_specific_cover_filter\",\"error\":\"key missing\"}";
        } else if (!state.settings.coverWallIntegratedMode) {
            result = "{\"ok\":false,\"cmd\":\"set_specific_cover_filter\",\"error\":\"replacement integrated mode is disabled\"}";
        } else {
            const std::string filePath = getStr(raw, "file_path");
            const std::string folderPath = getStr(raw, "folder_path");
            const std::string label = getStr(raw, "label");
            state.viewMode = ViewMode::Database;
            state.coverFilterIndex = 0;
            state.coverFilterActive = true;
            state.coverFilterPath = key;
            state.coverFilterMatchSig = 0;
            state.coverFilterMatchPaths.clear();
            state.folderFilterActive = false;
            state.selectedFolderPath.clear();
            state.coverWall.pinnedCoverFilterItem = CoverModule::CoverItem{};
            state.coverWall.pinnedCoverFilterItem.key = key;
            state.coverWall.pinnedCoverFilterItem.label = label;
            state.coverWall.pinnedCoverFilterItem.filePathUtf8 = filePath;
            state.coverWall.pinnedCoverFilterItem.folderPathUtf8 = folderPath;
            state.coverWall.pinnedCoverFilterItem.kind =
                filePath.empty()
                    ? CoverModule::CoverKind::FolderCover
                    : CoverModule::CoverKind::EmbeddedCover;
            if (state.database.isOpen()) {
                (void)state.database.getFileIdsForCoverTileMembership(key);
            }
            cancelPendingSearchForQaMutation();
            state.preferDeferredCountForNextRefresh = !ShouldUseHarnessImmediateFilterRefresh();
            RefreshSearchResults(state, true);
            state.syncToTab();
            DIAG_ACTION_FMT("Filter", "SpecificCoverApply:%s", key.c_str());
            result = "{"
                     "\"ok\":true,"
                     "\"cmd\":\"set_specific_cover_filter\","
                     "\"key\":\"" + jsonEscape(key) + "\","
                     "\"file_path\":\"" + jsonEscape(filePath) + "\""
                     "}";
        }

    } else if (cmd == "set_cover_wall_integrated_mode") {
        const bool enabled = getBool(raw, "enabled", state.settings.coverWallIntegratedMode);
        state.settings.coverWallIntegratedMode = enabled;
        if (!enabled) {
            state.coverWall.pinnedCoverFilterItem = CoverModule::CoverItem{};
            if (state.coverFilterActive && !state.coverFilterPath.empty()) {
                state.coverFilterActive = false;
                state.coverFilterPath.clear();
                state.coverFilterMatchSig = 0;
                state.coverFilterMatchPaths.clear();
            }
        }
        SaveAppSettings(state);
        result = "{"
                 "\"ok\":true,"
                 "\"cmd\":\"set_cover_wall_integrated_mode\","
                 "\"enabled\":" + std::string(state.settings.coverWallIntegratedMode ? "true" : "false") +
                 "}";

    } else if (cmd == "set_cover_wall_thumb_size") {
        const float requestedSize =
            static_cast<float>(getDouble(raw, "size", state.coverWall.thumbSize));
        state.coverWall.thumbSize = aura::coverwall::ClampThumbSize(requestedSize);
        result = "{"
                 "\"ok\":true,"
                 "\"cmd\":\"set_cover_wall_thumb_size\","
                 "\"thumb_size\":" + std::to_string(state.coverWall.thumbSize) +
                 "}";

    } else if (cmd == "set_cover_wall_tile_preset") {
        std::string preset = getStr(raw, "preset");
        std::transform(preset.begin(), preset.end(), preset.begin(), ::tolower);
        aura::coverwall::TileSizePreset sizePreset = aura::coverwall::TileSizePreset::Browse;
        if (preset == "compact") {
            sizePreset = aura::coverwall::TileSizePreset::Compact;
        } else if (preset == "browse") {
            sizePreset = aura::coverwall::TileSizePreset::Browse;
        } else if (preset == "inspect") {
            sizePreset = aura::coverwall::TileSizePreset::Inspect;
        } else {
            result = "{\"ok\":false,\"cmd\":\"set_cover_wall_tile_preset\",\"error\":\"invalid preset\"}";
        }
        if (result.empty()) {
            aura::coverwall::ApplyThumbPreset(sizePreset, state.coverWall.thumbSize);
            result = "{"
                     "\"ok\":true,"
                     "\"cmd\":\"set_cover_wall_tile_preset\","
                     "\"preset\":\"" + jsonEscape(preset) + "\","
                     "\"thumb_size\":" + std::to_string(state.coverWall.thumbSize) +
                     "}";
        }

    } else if (cmd == "start_cover_rebuild") {
        state.audioEngine.stop();
        ForceCoverWallReload(state);
        StartCoverRebuild(state);
        result = "{"
                 "\"ok\":true,"
                 "\"cmd\":\"start_cover_rebuild\","
                 "\"active\":" + std::string(state.coverRebuildActive ? "true" : "false") + ","
                 "\"total\":" + std::to_string((long long)state.coverRebuildTotal) +
                 "}";

    } else if (cmd == "stop_cover_rebuild") {
        if (state.coverRebuildActive) {
            state.coverRebuildCancel = true;
        }
        result = "{"
                 "\"ok\":true,"
                 "\"cmd\":\"stop_cover_rebuild\","
                 "\"active\":" + std::string(state.coverRebuildActive ? "true" : "false") + ","
                 "\"cancel_requested\":" + std::string(state.coverRebuildCancel ? "true" : "false") +
                 "}";

    } else if (!HarkenSearch::BetaFeatureFlags::kExposeWwiseTransfer &&
               cmd.rfind("wwise_", 0) == 0) {
        DIAG_ACTION_FMT("Wwise", "DisabledForBeta:cmd=%s", cmd.c_str());
        result = "{\"ok\":false,\"cmd\":\"" + jsonEscape(cmd) + "\",\"error\":\"wwise transfer is disabled for this beta\"}";

    } else if (cmd == "wwise_get_config") {
        const auto& cfg = aura::wwise::activeTransferConfig();
        DIAG_ACTION("Wwise", "ConfigRead");
        result = "{";
        result += "\"ok\":true,\"cmd\":\"wwise_get_config\",";
        result += "\"host\":\"" + jsonEscape(cfg.waapiHost) + "\",";
        result += "\"port\":" + std::to_string(cfg.waapiPort) + ",";
        result += "\"project_path\":\"" + jsonEscape(cfg.projectPath) + "\",";
        result += "\"import_destination\":\"" + jsonEscape(cfg.importDestination) + "\",";
        result += "\"originals_subfolder\":\"" + jsonEscape(cfg.originalsSubfolder) + "\",";
        result += "\"conflict_policy\":\"" + jsonEscape(aura::wwise::toString(cfg.conflictPolicy)) + "\",";
        result += "\"template_apply\":\"" + jsonEscape(aura::wwise::toString(cfg.templateApply)) + "\",";
        result += "\"structure_count\":" + std::to_string((int)cfg.structure.size());
        result += "}";

    } else if (cmd == "wwise_stage_selected") {
        std::vector<int> rows;
        const int requestedRow = getInt(raw, "row", -1);
        int limit = getInt(raw, "limit", 16);
        if (limit < 1) limit = 1;
        if (limit > 64) limit = 64;
        if (requestedRow >= 0) {
            rows.push_back(requestedRow);
        } else if (!state.selectedIndicesSet.empty()) {
            for (int row : state.selectedIndicesSet) {
                rows.push_back(row);
                if ((int)rows.size() >= limit) break;
            }
        } else if (state.selectedResultIndex >= 0) {
            rows.push_back(state.selectedResultIndex);
        } else if (state.lastClickedIndex >= 0) {
            rows.push_back(state.lastClickedIndex);
        } else {
            rows.push_back(0);
        }

        std::vector<std::string> sources;
        std::vector<aura::wwise::MetadataContext> contexts;
        sources.reserve(rows.size());
        contexts.reserve(rows.size());
        for (int row : rows) {
            if (row < 0 || row >= (int)state.searchResults.size()) continue;
            const auto& r = state.searchResults[(size_t)row];
            if (r.filePath.empty()) continue;
            sources.push_back(r.filePath);
            aura::wwise::MetadataContext ctx;
            const auto dot = r.fileName.find_last_of('.');
            ctx.filenameBase = (dot == std::string::npos) ? r.fileName : r.fileName.substr(0, dot);
            ctx.filenameFull = r.fileName;
            ctx.ucsCategory = r.category;
            ctx.ucsSubCategory = r.getMeta("subcategory");
            ctx.ucsCatId = r.getMeta("catid");
            ctx.library = r.getMeta("library");
            ctx.designer = r.getMeta("designer");
            ctx.composer = r.getMeta("composer");
            ctx.description = r.description;
            contexts.push_back(std::move(ctx));
        }
        if (sources.empty()) {
            DIAG_ACTION("Wwise", "StageSelected:count=0");
            result = "{\"ok\":false,\"cmd\":\"wwise_stage_selected\",\"error\":\"no selected source\"}";
        } else {
            aura::wwise::OpenWwiseTransferPanel(sources, contexts);
            DIAG_ACTION_FMT("Wwise", "StageSelected:count=%d", (int)sources.size());
            result = "{";
            result += "\"ok\":true,\"cmd\":\"wwise_stage_selected\",";
            result += "\"staged_count\":" + std::to_string((int)sources.size()) + ",";
            result += "\"first_path\":\"" + jsonEscape(sources.front()) + "\"";
            result += "}";
        }

    } else if (cmd == "wwise_ping" || cmd == "wwise_connect") {
        const auto& cfg = aura::wwise::activeTransferConfig();
        aura::waapi::ClientConfig cc;
        cc.host = cfg.waapiHost.empty() ? "127.0.0.1" : cfg.waapiHost;
        cc.port = cfg.waapiPort > 0 ? cfg.waapiPort : 8080;
        aura::waapi::WaapiClient client;
        const auto started = std::chrono::steady_clock::now();
        DIAG_SYSTEM_FMT("Wwise", "ConnectStart:host=%s port=%d", cc.host.c_str(), cc.port);
        const bool connected = client.connect(cc);
        std::string version;
        std::string projectPath;
        bool pingOk = false;
        std::string error;
        if (connected) {
            pingOk = client.ping(version, projectPath);
            if (!pingOk) error = client.lastError();
            client.disconnect();
        } else {
            error = client.lastError();
        }
        const auto elapsedMs = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - started).count();
        DIAG_SYSTEM_FMT("Wwise", "ConnectComplete:success=%d ping=%d durationMs=%.2f",
                        connected ? 1 : 0, pingOk ? 1 : 0, elapsedMs);
        result = "{";
        result += "\"ok\":" + std::string(connected && pingOk ? "true" : "false") + ",";
        result += "\"cmd\":\"" + cmd + "\",";
        result += "\"connected\":" + std::string(connected ? "true" : "false") + ",";
        result += "\"ping_ok\":" + std::string(pingOk ? "true" : "false") + ",";
        result += "\"version\":\"" + jsonEscape(version) + "\",";
        result += "\"project_path\":\"" + jsonEscape(projectPath) + "\",";
        result += "\"duration_ms\":" + std::to_string(elapsedMs) + ",";
        result += "\"error\":\"" + jsonEscape(error) + "\"";
        result += "}";

    } else if (cmd == "wwise_import_selected") {
        std::vector<int> rows;
        const int requestedRow = getInt(raw, "row", -1);
        int limit = getInt(raw, "limit", 16);
        if (limit < 1) limit = 1;
        if (limit > 64) limit = 64;
        if (requestedRow >= 0) {
            rows.push_back(requestedRow);
        } else if (!state.selectedIndicesSet.empty()) {
            for (int row : state.selectedIndicesSet) {
                rows.push_back(row);
                if ((int)rows.size() >= limit) break;
            }
        } else if (state.selectedResultIndex >= 0) {
            rows.push_back(state.selectedResultIndex);
        } else if (state.lastClickedIndex >= 0) {
            rows.push_back(state.lastClickedIndex);
        } else {
            rows.push_back(0);
        }

        std::vector<std::string> sources;
        std::vector<aura::wwise::MetadataContext> contexts;
        sources.reserve(rows.size());
        contexts.reserve(rows.size());
        for (int row : rows) {
            if (row < 0 || row >= (int)state.searchResults.size()) continue;
            const auto& r = state.searchResults[(size_t)row];
            if (r.filePath.empty()) continue;
            sources.push_back(r.filePath);
            aura::wwise::MetadataContext ctx;
            const auto dot = r.fileName.find_last_of('.');
            ctx.filenameBase = (dot == std::string::npos) ? r.fileName : r.fileName.substr(0, dot);
            ctx.filenameFull = r.fileName;
            ctx.ucsCategory = r.category;
            ctx.ucsSubCategory = r.getMeta("subcategory");
            ctx.ucsCatId = r.getMeta("catid");
            ctx.library = r.getMeta("library");
            ctx.designer = r.getMeta("designer");
            ctx.composer = r.getMeta("composer");
            ctx.description = r.description;
            contexts.push_back(std::move(ctx));
        }

        if (sources.empty()) {
            result = "{\"ok\":false,\"cmd\":\"wwise_import_selected\",\"error\":\"no selected source\"}";
        } else {
            // Reuse the same staging path as the visible UI. This seeds the
            // first-run default RandomContainer -> SoundSFX structure instead
            // of letting QA exercise a different empty-config path.
            aura::wwise::OpenWwiseTransferPanel(sources, contexts);
            auto& cfg = aura::wwise::activeTransferConfig();
            if (cfg.structure.empty()) {
                result = "{\"ok\":false,\"cmd\":\"wwise_import_selected\",\"error\":\"structure is empty\"}";
            }
        }
        if (result.empty()) {
            auto& cfg = aura::wwise::activeTransferConfig();
            aura::waapi::ClientConfig cc;
            cc.host = cfg.waapiHost.empty() ? "127.0.0.1" : cfg.waapiHost;
            cc.port = cfg.waapiPort > 0 ? cfg.waapiPort : 8080;
            aura::waapi::WaapiClient client;
            DIAG_SYSTEM_FMT("Wwise", "ConnectStart:host=%s port=%d", cc.host.c_str(), cc.port);
            const bool connected = client.connect(cc);
            DIAG_SYSTEM_FMT("Wwise", "ConnectComplete:success=%d ping=0 durationMs=0.00", connected ? 1 : 0);
            if (!connected) {
                result = "{\"ok\":false,\"cmd\":\"wwise_import_selected\",\"error\":\"" + jsonEscape(client.lastError()) + "\"}";
            } else {
                if (cfg.projectPath.empty()) {
                    std::string version;
                    std::string projectPath;
                    if (client.ping(version, projectPath) && !projectPath.empty()) {
                        cfg.projectPath = projectPath;
                    }
                }
                const auto imports = aura::wwise::expandStructure(cfg, contexts, sources);
                DIAG_SYSTEM_FMT("Wwise", "ImportStart:count=%d conflict=%s",
                                (int)imports.size(), aura::wwise::toString(cfg.conflictPolicy).c_str());
                aura::wwise::TransferResult transferResult;
                {
                    DiagScopedTimer transferTimer("Wwise", "importTotal");
                    transferResult = aura::wwise::executeTransfer(client, cfg, imports, nullptr);
                }
                client.disconnect();
                DIAG_SYSTEM_FMT("Wwise", "ImportComplete:success=%d total=%d imported=%d skipped=%d failed=%d",
                                transferResult.failed == 0 && transferResult.aggregateError.empty() ? 1 : 0,
                                transferResult.total,
                                transferResult.imported,
                                transferResult.skipped,
                                transferResult.failed);
                for (int i = 0; i < (int)transferResult.items.size(); ++i) {
                    const auto& item = transferResult.items[(size_t)i];
                    DIAG_ACTION_FMT("Wwise", "ImportItem:index=%d status=%s",
                                    i, aura::wwise::toString(item.status));
                }
                result = "{";
                result += "\"ok\":" + std::string(transferResult.failed == 0 && transferResult.aggregateError.empty() ? "true" : "false") + ",";
                result += "\"cmd\":\"wwise_import_selected\",";
                result += "\"total\":" + std::to_string(transferResult.total) + ",";
                result += "\"imported\":" + std::to_string(transferResult.imported) + ",";
                result += "\"skipped\":" + std::to_string(transferResult.skipped) + ",";
                result += "\"failed\":" + std::to_string(transferResult.failed) + ",";
                result += "\"aggregate_error\":\"" + jsonEscape(transferResult.aggregateError) + "\"";
                result += "}";
            }
        }

    } else if (cmd == "stacked_filter_select") {
        const std::string folderPath = getStr(raw, "folder_path");
        const std::string query = getStr(raw, "query");
        const std::string artist = getStr(raw, "artist");
        const std::string album = getStr(raw, "album");
        const std::string category = getStr(raw, "category");
        const std::string format = getStr(raw, "format");
        std::string coverMode = getStr(raw, "cover_mode");
        std::transform(coverMode.begin(), coverMode.end(), coverMode.begin(), ::tolower);
        int coverIndex = 0;
        if (coverMode == "has" || coverMode == "with" || coverMode == "covered") {
            coverIndex = 1;
            coverMode = "has";
        } else if (coverMode == "no" || coverMode == "without" || coverMode == "none") {
            coverIndex = 2;
            coverMode = "no";
        } else {
            coverMode = "all";
        }

        state.viewMode = ViewMode::Database;
        state.preferDeferredCountForNextRefresh = false;
        {
            DiagScopedTimer qaFolderTimer("Filter", "folderApply");
            state.folderFilterActive = !folderPath.empty();
            // Filter bundles store a canonical folder path.
            state.selectedFolderPath = HarkenSearch::CanonicalFolderPath(folderPath);
            state.coverFilterPath.clear();
            state.coverFilterMatchSig = 0;
            state.coverFilterMatchPaths.clear();
            DIAG_ACTION_FMT("Filter", "FolderApply:%s", folderPath.c_str());
        }
        {
            DiagScopedTimer qaMetadataTimer("Filter", "metadataApply");
            state.artistFilterBuffer[0] = '\0';
            state.artistFilterAlternates.clear();
            state.albumFilterBuffer[0] = '\0';
            state.categoryFilterBuffer[0] = '\0';
            state.formatFilterIndex = 0;
            if (!artist.empty()) strncpy_s(state.artistFilterBuffer, sizeof(state.artistFilterBuffer), artist.c_str(), _TRUNCATE);
            if (!album.empty()) strncpy_s(state.albumFilterBuffer, sizeof(state.albumFilterBuffer), album.c_str(), _TRUNCATE);
            if (!category.empty()) strncpy_s(state.categoryFilterBuffer, sizeof(state.categoryFilterBuffer), category.c_str(), _TRUNCATE);
            if (!format.empty()) state.formatFilterIndex = formatFilterIndexFromValue(format);
            DIAG_ACTION("Filter", "MetadataApply:stacked");
        }
        {
            DiagScopedTimer qaCoverTimer("Filter", "coverApply");
            state.coverFilterIndex = coverIndex;
            state.coverFilterActive = false;
            DIAG_ACTION_FMT("Filter", "CoverApply:%s", coverMode.c_str());
        }
        if (query.size() < sizeof(state.searchBuffer) - 1) {
            strncpy_s(state.searchBuffer, sizeof(state.searchBuffer), query.c_str(), _TRUNCATE);
        }
        state.filterQueryGeneration++;
        RefreshSearchResults(state, true);
        pumpImmediateSearchForQa();

        const int total = (int)state.searchResults.size();
        int selectedRow = -1;
        std::string selectedPath;
        if (total > 0) {
            int targetIndex = getInt(raw, "target_index", 0);
            if (targetIndex < 0) targetIndex = 0;
            selectedRow = targetIndex % total;
            const std::string currentFile = state.audioEngine.getLoadedFilePath();
            if (total > 1 && !currentFile.empty() && state.searchResults[(size_t)selectedRow].filePath == currentFile) {
                selectedRow = (selectedRow + 1) % total;
            }
            selectedPath = state.searchResults[(size_t)selectedRow].filePath;
            state.selectedResultIndex = selectedRow;
            state.selectedIndicesSet.clear();
            state.selectedIndicesSet.insert(selectedRow);
            state.lastClickedIndex = selectedRow;
            state.scrollToSelected = true;
            DIAG_ACTION_FMT("Browse", "SelectRow:%d", selectedRow);
            EnqueueAudioDecoderPrewarmWindow(state, selectedRow, 1);
            state.audioEngine.loadFileAsync(selectedPath, state.settings.autoplayOnSelect);
        }

        result = "{\"ok\":" + std::string(total > 0 ? "true" : "false")
               + ",\"cmd\":\"stacked_filter_select\""
               + ",\"result_count\":" + std::to_string(total)
               + ",\"selected_row\":" + std::to_string(selectedRow)
               + ",\"selected_path\":\"" + jsonEscape(selectedPath) + "\""
               + ",\"search_ready\":" + std::string(
                    (state.filterQueryState == FilterQueryState::Idle || state.filterQueryState == FilterQueryState::Ready)
                    ? "true" : "false")
               + (total > 0 ? "" : ",\"error\":\"no results after stacked filters\"")
               + "}";

    } else if (cmd == "get_results") {
        int offset = getInt(raw, "offset", 0);
        int limit = getInt(raw, "limit", 20);
        if (limit > 100) limit = 100;
        int total = (int)state.searchResults.size();

        std::string items = "[";
        for (int i = offset; i < std::min(offset + limit, total); i++) {
            const auto& r = state.searchResults[i];
            if (i > offset) items += ",";
            items += "{\"idx\":" + std::to_string(i)
                   + ",\"name\":\"" + jsonEscape(r.fileName) + "\""
                   + ",\"path\":\"" + jsonEscape(r.filePath) + "\""
                   + "}";
        }
        items += "]";
        result = "{\"ok\":true,\"cmd\":\"get_results\",\"total\":" + std::to_string(total)
               + ",\"offset\":" + std::to_string(offset)
               + ",\"count\":" + std::to_string(std::min(limit, total - offset))
               + ",\"items\":" + items + "}";

    } else if (cmd == "wait_ready") {
        bool dbReady = state.database.isOpen();
        const bool mainUiReady = s_mainUiReady.load(std::memory_order_acquire);
        bool searchDone = state.filterQueryState == FilterQueryState::Idle
                       || state.filterQueryState == FilterQueryState::Ready
                       || state.filterQueryState == FilterQueryState::Error;
        bool ready = dbReady && mainUiReady && searchDone;
        char buf[320];
        snprintf(buf, sizeof(buf),
            "{\"ok\":true,\"cmd\":\"wait_ready\",\"db_open\":%s,\"main_ui_ready\":%s,\"search_idle\":%s,\"ready\":%s,\"query_state\":%d}",
            dbReady ? "true" : "false",
            mainUiReady ? "true" : "false",
            searchDone ? "true" : "false",
            ready ? "true" : "false",
            (int)state.filterQueryState
        );
        result = buf;

    } else if (cmd == "wait_probe_ready") {
        const bool dbReady = state.database.isOpen();
        const bool mainUiReady = s_mainUiReady.load(std::memory_order_acquire);
        const bool searchDone = state.filterQueryState == FilterQueryState::Idle
                             || state.filterQueryState == FilterQueryState::Ready
                             || state.filterQueryState == FilterQueryState::Error;
        const bool warmupDone = state.warmupFramesRemaining <= 0;
        const bool searchRequestsIdle = state.pendingSearchRequestId == 0
                                     && state.pendingFacetRequestId == 0
                                     && !state.asyncSearchInProgress
                                     && !state.pendingCountFollowUp;
        const bool scanIdle = !state.isScanning.load(std::memory_order_relaxed);
        const bool scanForegroundComplete =
            !state.scanCompletionPending.load(std::memory_order_acquire);
        const bool waveformIdle = !state.audioEngine.isWaveformDetailPending()
                               && state.pendingDetailFilePath.empty();
        const bool bgCacheIdle = !state.bgCacheRunning.load();
        const bool coverWarmupIdle = !state.coverWall.preloadInFlight
                                  && !state.coverWall.entriesInFlight
                                  && !state.coverWall.foldersInFlight
                                  && !state.coverWall.loading;
        const bool ready = dbReady
                        && mainUiReady
                        && searchDone
                        && warmupDone
                        && searchRequestsIdle
                        && scanIdle
                        && scanForegroundComplete
                        && waveformIdle
                        && bgCacheIdle
                        && coverWarmupIdle;
        char buf[768];
        snprintf(
            buf,
            sizeof(buf),
            "{\"ok\":true,\"cmd\":\"wait_probe_ready\",\"db_open\":%s,\"main_ui_ready\":%s,\"search_idle\":%s,\"warmup_done\":%s,"
            "\"search_requests_idle\":%s,\"scan_idle\":%s,\"waveform_idle\":%s,\"bg_cache_idle\":%s,"
            "\"cover_warmup_idle\":%s,\"scan_foreground_complete\":%s,\"ready\":%s,\"query_state\":%d,\"warmup_frames\":%d,"
            "\"pending_search_request_id\":%llu,\"pending_facet_request_id\":%llu,"
            "\"async_search_in_progress\":%s,\"pending_count_follow_up\":%s}",
            dbReady ? "true" : "false",
            mainUiReady ? "true" : "false",
            searchDone ? "true" : "false",
            warmupDone ? "true" : "false",
            searchRequestsIdle ? "true" : "false",
            scanIdle ? "true" : "false",
            waveformIdle ? "true" : "false",
            bgCacheIdle ? "true" : "false",
            coverWarmupIdle ? "true" : "false",
            scanForegroundComplete ? "true" : "false",
            ready ? "true" : "false",
            (int)state.filterQueryState,
            state.warmupFramesRemaining,
            (unsigned long long)state.pendingSearchRequestId,
            (unsigned long long)state.pendingFacetRequestId,
            state.asyncSearchInProgress ? "true" : "false",
            state.pendingCountFollowUp ? "true" : "false"
        );
        result = buf;

    } else if (cmd == "rescan_metadata") {
        // Queue a deferred rescan for the given row (default: row 0).
        // Processed at the top of the next RenderUI call (safe — outside table loop).
        int row = getInt(raw, "row", 0);
        if (row >= 0 && row < (int)state.searchResults.size()) {
            state.qaRescanRow = row;
            result = "{\"ok\":true,\"cmd\":\"rescan_metadata\",\"row\":" + std::to_string(row) + "}";
        } else {
            result = "{\"ok\":false,\"cmd\":\"rescan_metadata\",\"error\":\"row out of range\"}";
        }

    } else if (cmd == "scan_folder" || cmd == "start_scan") {
        std::string path = getStr(raw, "path");
        if (path.empty()) {
            result = "{\"ok\":false,\"cmd\":\"" + cmd + "\",\"error\":\"path missing\"}";
        } else if (!PathExistsUtf8(path)) {
            result = "{\"ok\":false,\"cmd\":\"" + cmd + "\",\"error\":\"path missing\"}";
        } else if (state.isScanning.load()) {
            result = "{\"ok\":false,\"cmd\":\"" + cmd + "\",\"error\":\"scan already running\"}";
        } else {
            bool recursive = getBool(raw, "recursive", state.scanRecursive);
            bool updateExisting = getBool(raw, "update_existing", state.scanUpdateExisting);
            bool removeMissing = getBool(raw, "remove_missing", state.scanRemoveMissing);
            bool cacheWaveforms = getBool(raw, "cache_waveforms", state.settings.cacheWaveformsOnScan);
            if (raw.find("build_similarity_index") != std::string::npos) {
                state.scanBuildSimilarityIndexAfterScan =
                    getBool(raw, "build_similarity_index", state.scanBuildSimilarityIndexAfterScan);
            }
            const std::string scanStrategy = getStr(raw, "scan_strategy");
            if (scanStrategy == "fast" || scanStrategy == "极速模式") {
                cacheWaveforms = false;
            } else if (scanStrategy == "full" ||
                       scanStrategy == "complete" ||
                       scanStrategy == "完整模式") {
                cacheWaveforms = true;
            }
            int workerThreads = getInt(raw, "worker_threads", state.scanWorkerThreads);
            const bool started = BeginFolderScan(state, path, recursive, updateExisting, removeMissing, cacheWaveforms, workerThreads);
            result = "{\"ok\":" + std::string(started ? "true" : "false")
                   + ",\"cmd\":\"" + cmd + "\",\"started\":" + std::string(started ? "true" : "false")
                   + ",\"path\":\"" + jsonEscape(path) + "\""
                   + ",\"scan_strategy\":\"" + std::string(cacheWaveforms ? "full" : "fast") + "\"}";
        }

    } else if (cmd == "scan_status") {
        bool scanning = state.isScanning.load();
        int progress = state.scanProgress.load();
        int total = state.scanTotal.load();
        int added = state.scanAddedCount.load();
        int updated = state.scanUpdatedCount.load();
        int skipped = state.scanSkippedCount.load();
        int removed = state.scanRemovedCount.load();
        int errors = state.scanErrorCount.load();
        std::string currentFile = state.getScanFile();
        std::vector<std::string> errorDetails = state.getScanErrors();
        const auto scanBackgroundBackfillPhase = [&state]() {
            switch (static_cast<AppState::ScanBackgroundBackfillPhase>(
                state.scanBackgroundBackfillPhase.load(std::memory_order_acquire))) {
            case AppState::ScanBackgroundBackfillPhase::Deferred:
                return "deferred";
            case AppState::ScanBackgroundBackfillPhase::Resumed:
                return "resumed";
            case AppState::ScanBackgroundBackfillPhase::Idle:
            default:
                return "idle";
            }
        }();
        result = "{";
        result += "\"ok\":true,";
        result += "\"cmd\":\"scan_status\",";
        result += "\"scanning\":" + std::string(scanning ? "true" : "false") + ",";
        result += "\"progress\":" + std::to_string(progress) + ",";
        result += "\"total\":" + std::to_string(total) + ",";
        result += "\"added\":" + std::to_string(added) + ",";
        result += "\"updated\":" + std::to_string(updated) + ",";
        result += "\"skipped\":" + std::to_string(skipped) + ",";
        result += "\"removed\":" + std::to_string(removed) + ",";
        result += "\"errors\":" + std::to_string(errors) + ",";
        {
            int unreachableHint = 0;
            for (const auto& msg : errorDetails) {
                if (msg.find("Scan_NetworkLibraryUnreachable") != std::string::npos) {
                    ++unreachableHint;
                }
            }
            result += "\"unreachable\":" + std::to_string(unreachableHint) + ",";
        }
         result += "\"error_details\":[";
        for (size_t i = 0; i < errorDetails.size(); ++i) {
            if (i > 0) result += ",";
            const std::string shown = (errorDetails[i].find("Scan_NetworkLibraryUnreachable") != std::string::npos)
                ? std::string(L(Scan_NetworkLibraryUnreachable))
                : errorDetails[i];
            result += "\"" + jsonEscape(shown) + "\"";
        }
        result += "],";
        result += "\"current_file\":\"" + jsonEscape(currentFile) + "\",";
        result += "\"last_scanned_path\":\"" + jsonEscape(state.lastScannedPath) + "\",";
        result += "\"scan_phase\":\"" + std::string(ScanPhaseToSnakeCase(state.scanPhase.load())) + "\",";
        const HarkenSearch::ScanWorkflowSnapshot workflowSnapshot = state.getScanWorkflowSnapshot();
        const HarkenSearch::ScanProgressView processingProgress =
            HarkenSearch::BuildScanProcessingProgress(workflowSnapshot);
        result += "\"scan_workflow_stage\":\"" + std::string(HarkenSearch::ScanWorkflowStageName(workflowSnapshot.stage)) + "\",";
        result += "\"processed\":" + std::to_string(processingProgress.current) + ",";
        result += "\"discovered\":" + std::to_string(processingProgress.total) + ",";
        result += "\"succeeded\":" + std::to_string(workflowSnapshot.succeeded) + ",";
        result += "\"failed_files\":" + std::to_string(workflowSnapshot.failed) + ",";
        result += "\"mini_waveforms_completed\":" + std::to_string(workflowSnapshot.miniWaveformsCompleted) + ",";
        result += "\"mini_waveforms_total\":" + std::to_string(workflowSnapshot.miniWaveformsTotal) + ",";
        result += "\"can_enter_completed\":" + std::string(HarkenSearch::CanEnterCompleted(workflowSnapshot) ? "true" : "false") + ",";
        result += "\"scan_strategy\":\"" + std::string(state.settings.cacheWaveformsOnScan ? "full" : "fast") + "\",";
        result += "\"scan_completion_pending\":" + std::string(state.scanCompletionPending.load(std::memory_order_acquire) ? "true" : "false") + ",";
        result += "\"scan_completion_waveforms_pending\":" + std::string(state.scanCompletionWaveformsPending.load(std::memory_order_acquire) ? "true" : "false") + ",";
        result += "\"scan_completion_covers_pending\":" + std::string(state.scanCompletionCoversPending.load(std::memory_order_acquire) ? "true" : "false") + ",";
        result += "\"scan_completion_total\":" + std::to_string(state.scanCompletionTotal.load(std::memory_order_acquire)) + ",";
        result += "\"scan_completion_pending_files\":" + std::to_string(state.scanCompletionPendingFiles.load(std::memory_order_acquire)) + ",";
        result += "\"scan_completion_mini_unresolved\":" + std::to_string(state.scanCompletionMiniUnresolved.load(std::memory_order_acquire)) + ",";
        result += "\"scan_completion_cover_unresolved\":" + std::to_string(state.scanCompletionCoverUnresolved.load(std::memory_order_acquire)) + ",";
        result += "\"scan_completion_metadata_unresolved\":" + std::to_string(state.scanCompletionMetadataUnresolved.load(std::memory_order_acquire)) + ",";
        result += "\"scan_started_at_ms\":" + std::to_string(state.scanStartedAtMs.load(std::memory_order_acquire)) + ",";
        result += "\"scan_completion_ready_at_ms\":" + std::to_string(state.scanCompletionReadyAtMs.load(std::memory_order_acquire)) + ",";
        result += "\"scan_completion_duration_ms\":" + std::to_string(state.scanCompletionDurationMs.load(std::memory_order_acquire)) + ",";
        result += "\"scan_background_backfill_phase\":\"" + std::string(scanBackgroundBackfillPhase) + "\",";
        result += "\"scan_background_backfill_deferred_wait_ms\":" + std::to_string(state.scanBackgroundBackfillDeferredWaitMs.load(std::memory_order_acquire)) + ",";
        result += "\"scan_background_backfill_resume_threads\":" + std::to_string(state.scanBackgroundBackfillResumeThreads.load(std::memory_order_acquire)) + ",";
        const ScanReadyToWorkSnapshot readyToWork = GetScanReadyToWorkSnapshotForQa(state);
        result += "\"ready_to_work_pending\":" + std::string(readyToWork.pending ? "true" : "false") + ",";
        result += "\"ready_to_work_waveforms_pending\":" + std::string(readyToWork.waveformsPending ? "true" : "false") + ",";
        result += "\"ready_to_work_covers_pending\":" + std::string(readyToWork.coversPending ? "true" : "false") + ",";
        result += "\"ready_to_work_cover_post_pending\":" + std::string(readyToWork.coverPostPending ? "true" : "false") + ",";
        result += "\"ready_to_work_cover_first_screen_pending\":" + std::string(readyToWork.coverFirstScreenPending ? "true" : "false") + ",";
        result += "\"ready_to_work_main_wave_pending\":" + std::string(readyToWork.mainWavePending ? "true" : "false");
        result += "}";

    } else if (cmd == "scan_stop") {
        bool wasScanning = state.isScanning.load();
        if (wasScanning) {
            state.requestCancelScanTask();
        }
        result = "{\"ok\":true,\"cmd\":\"scan_stop\",\"was_scanning\":"
               + std::string(wasScanning ? "true" : "false") + "}";

    } else if (cmd == "scan_acknowledge_complete") {
        const bool wasScanning = state.isScanning.load();
        const bool hadScanSummary =
            state.scanTotal.load() > 0 ||
            state.scanWasCancelled ||
            state.scanErrorCount.load() > 0;
        if (wasScanning) {
            result = "{\"ok\":false,\"cmd\":\"scan_acknowledge_complete\",\"error\":\"scan still running\"}";
        } else if (!hadScanSummary) {
            result = "{\"ok\":false,\"cmd\":\"scan_acknowledge_complete\",\"error\":\"no completed scan summary\"}";
        } else if (state.scanCompletionPending.load(std::memory_order_acquire)) {
            result = "{\"ok\":false,\"cmd\":\"scan_acknowledge_complete\",\"error\":\"scan foreground completion pending\"}";
        } else {
            const int64_t acknowledgedCompletionDurationMs =
                state.scanCompletionDurationMs.load(std::memory_order_acquire);
            if (state.scanTotal.load() > 0 && !state.scanWasCancelled) {
                state.dbSwitchSettleFramesRemaining =
                    (std::max)(state.dbSwitchSettleFramesRemaining, 120);
                state.needsDatabaseRefresh = true;
            }

            state.scanProgress = 0;
            state.scanTotal = 0;
            state.scanAddedCount = 0;
            state.scanUpdatedCount = 0;
            state.scanSkippedCount = 0;
            state.scanRemovedCount = 0;
            state.scanErrorCount = 0;
            state.scanIngestedCount = 0;
            state.scanIngestDurationMs = 0;
            state.clearScanTiming();
            state.clearScanWorkflowState();
            state.scanSummaryToastUntil = 0.0;
            state.resetScanReadyToWorkState();
            state.clearScanErrors();

            state.scanCancelRequested = false;
            state.scanWasCancelled = false;
            state.scanWasPaused = false;
            state.scanDataCommitted = false;
            result = "{";
            result += "\"ok\":true,";
            result += "\"cmd\":\"scan_acknowledge_complete\",";
            result += "\"scan_completion_pending\":" + std::string(state.scanCompletionPending.load(std::memory_order_acquire) ? "true" : "false") + ",";
            result += "\"scan_completion_duration_ms\":" + std::to_string(acknowledgedCompletionDurationMs) + ",";
            result += "\"ready_to_work_pending\":" + std::string(GetScanReadyToWorkSnapshotForQa(state).pending ? "true" : "false") + ",";
            result += "\"needs_database_refresh\":" + std::string(state.needsDatabaseRefresh ? "true" : "false") + ",";
            result += "\"db_switch_settle_frames\":" + std::to_string(state.dbSwitchSettleFramesRemaining);
            result += "}";
        }

    } else if (cmd == "detect_bpm") {
        DIAG_ACTION("Music", "DetectBpmShortcut");
        result = "{\"ok\":true,\"cmd\":\"detect_bpm\"}";

    } else if (cmd == "tap_tempo") {
        int count = std::max(1, getInt(raw, "count", 1));
        for (int i = 0; i < count; ++i) {
            DIAG_ACTION("Music", "TapTempoShortcut");
            state.tapTempo.tap();
            if (state.tapTempo.hasResult()) {
                int tapCount = state.tapTempo.tapCount();
                state.pendingBpm.bpm = state.tapTempo.getBpm();
                state.pendingBpm.confidence = std::min(1.0f, 0.4f + 0.06f * static_cast<float>(tapCount));
                state.pendingBpm.source = static_cast<int>(MusicFieldSource::TapTempo);
                state.pendingBpm.isReady = true;
                state.pendingBpm.isAnalyzing = false;
                state.pendingBpm.bubbleTimer = 0.0;
            }
        }
        result = "{\"ok\":true,\"cmd\":\"tap_tempo\",\"count\":" + std::to_string(count) + "}";

    } else if (cmd == "scan_bpm") {
        DIAG_ACTION("Music", "ScanBpmButton");
        state.qaRequestScanBpm = true;
        result = "{\"ok\":true,\"cmd\":\"scan_bpm\"}";

    } else if (cmd == "start_waveform_rebuild" || cmd == "start_waveform_full_rebuild") {
        const bool fullRebuild = (cmd == "start_waveform_full_rebuild") || getBool(raw, "full", false);
        if (!state.database.isOpen()) {
            result = "{\"ok\":false,\"cmd\":\"start_waveform_rebuild\",\"error\":\"db not open\"}";
        } else if (state.dbSwitchInProgress) {
            result = "{\"ok\":false,\"cmd\":\"start_waveform_rebuild\",\"error\":\"db switch in progress\"}";
        } else if (state.waveformRebuildActive) {
            result = "{\"ok\":true,\"cmd\":\"start_waveform_rebuild\",\"already_active\":true}";
        } else {
            state.qaRequestWaveformRebuildFull = fullRebuild;
            state.qaRequestWaveformRebuild = true;
            result = std::string("{\"ok\":true,\"cmd\":\"start_waveform_rebuild\",\"started\":true,\"mode\":\"")
                + (fullRebuild ? "full" : "mini_only") + "\"}";
        }

    } else if (cmd == "stop_waveform_rebuild") {
        const bool wasActive = state.waveformRebuildActive;
        state.waveformRebuildCancel = true;
        state.waveformRebuildActive = false;
        state.waveformRebuildMiniOnlyMode = false;
        state.waveformRebuildBatch.clear();
        state.waveformRebuildQueuedIds.clear();
        state.waveformRebuildBatchIdx = 0;
        state.waveformRebuildFullMode = false;
        state.waveformRebuildProgress = 0;
        state.waveformRebuildTotal = 0;
        state.waveformRebuildMiniTotal = 0;
        state.waveformRebuildDetailTotal = 0;
        state.waveformRebuildSkipped = 0;
        state.waveformRebuildFailed = 0;
        state.clearPendingWaveforms();
        state.waveformWorker.stop();
        if (state.database.isOpen()) {
            state.waveformWorker.start(&state.database, aura::RecommendedWaveformWorkerThreads());
        }
        result = "{\"ok\":true,\"cmd\":\"stop_waveform_rebuild\",\"was_active\":"
               + std::string(wasActive ? "true" : "false") + "}";

    } else if (cmd == "get_bpm") {
        int row = getInt(raw, "row", -1);
        int offset = getInt(raw, "offset", 0);
        int limit = getInt(raw, "limit", 20);
        if (limit > 200) limit = 200;
        int total = (int)state.searchResults.size();

        std::string items = "[";
        int start = (row >= 0) ? row : offset;
        int end   = (row >= 0) ? row + 1 : std::min(offset + limit, total);
        for (int i = start; i < end && i < total; i++) {
            const auto& r = state.searchResults[i];
            if (i > start) items += ",";
            char bpmStr[32];
            if (r.bpm > 0.0f)
                snprintf(bpmStr, sizeof(bpmStr), "%.1f", r.bpm);
            else if (r.bpm < 0.0f)
                snprintf(bpmStr, sizeof(bpmStr), "-1");
            else
                snprintf(bpmStr, sizeof(bpmStr), "0");
            items += "{\"idx\":" + std::to_string(i)
                   + ",\"name\":\"" + jsonEscape(r.fileName) + "\""
                   + ",\"bpm\":" + bpmStr + "}";
        }
        items += "]";
        result = "{\"ok\":true,\"cmd\":\"get_bpm\",\"total\":" + std::to_string(total)
               + ",\"batch_running\":" + (state.musicAnalyzer.batchRunning() ? "true" : "false")
               + ",\"batch_done\":" + std::to_string(state.musicAnalyzer.m_batchDone.load())
               + ",\"batch_found\":" + std::to_string(state.musicAnalyzer.m_batchFound.load())
               + ",\"items\":" + items + "}";

    } else if (cmd == "get_ui_regions") {
        auto& qr = state.qaRegions;
        char buf[512];
        snprintf(buf, sizeof(buf),
            "{\"ok\":true,\"cmd\":\"get_ui_regions\","
            "\"list\":{\"x\":%.0f,\"y\":%.0f,\"w\":%.0f,\"h\":%.0f,"
            "\"rowH\":%d,\"visibleRows\":%d,\"scrollTop\":%d,\"playingRow\":%d}}",
            qr.listX, qr.listY, qr.listW, qr.listH,
            qr.listRowHeight, qr.listVisibleRows, qr.listScrollTopRow, qr.playingRowIndex);
        result = buf;

    } else if (cmd == "ping") {
        result = "{\"ok\":true,\"cmd\":\"ping\"}";

    } else {
        result = "{\"ok\":false,\"cmd\":\"" + cmd + "\",\"error\":\"unknown command\"}";
    }

    if (!requestId.empty() && !result.empty() && result.front() == '{') {
        result.insert(1, "\"request_id\":\"" + jsonEscape(requestId) + "\",");
    }

    const std::string tmpSuffix = requestId.empty() ? "default" : requestId;
    std::string tmpPath = runtimeRoot + "\\qa_result." + tmpSuffix + ".tmp";

    (void)Platform::QaFiles::WriteResultAtomically(resPath, tmpPath, result);

    DIAG_ACTION_FMT("QA", "Command: %s", cmd.c_str());
}
#endif // AURA_BUILD_DIAGNOSTICS

#if !(AURA_BUILD_DIAGNOSTICS && !AURA_INTERNAL_BETA)
// Public/release builds keep the QA command polling call sites source-stable,
// but do not compile the diagnostics command-file implementation.
static void PollQaCommandFile(AppState&) {}
#endif

void EnsureWindowOnScreen(HWND hwnd) {
    if (!hwnd) return;
    RECT rc{};
    if (!GetWindowRect(hwnd, &rc)) return;

    int w = rc.right - rc.left;
    int h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return;

    // Use the nearest monitor (handles multi-monitor well)
    HMONITOR hMon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = { sizeof(mi) };
    if (!GetMonitorInfoW(hMon, &mi)) return;

    const RECT& work = mi.rcWork;
    int newX = rc.left;
    int newY = rc.top;

    // Ensure at least 50 px visible on screen
    const int minVisible = 50;
    if (rc.right < work.left + minVisible) newX = work.left;
    if (rc.left > work.right - minVisible) newX = work.right - w;
    if (rc.bottom < work.top + minVisible) newY = work.top;
    if (rc.top > work.bottom - minVisible) newY = work.bottom - h;

    if (newX != rc.left || newY != rc.top) {
        SetWindowPos(hwnd, nullptr, newX, newY, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
}

void MinimizeWindowNoAnimation(HWND hwnd) {
    if (!hwnd) return;

    // Keep drag-saved placement authoritative when drag-hide is active.
    WINDOWPLACEMENT wp = { sizeof(WINDOWPLACEMENT) };
    GetWindowPlacement(hwnd, &wp);

    if (!g_savedPlacementForDragHideValid) {
        bool wasMaximized = (IsZoomed(hwnd) != 0);
        g_savedPlacementForTrayMinimize = wp;
        g_savedPlacementForTrayMinimizeValid = true;
        g_savedWasMaximizedForTrayMinimize = wasMaximized;

        if (wasMaximized) {
            wp.flags |= WPF_RESTORETOMAXIMIZED;
        }
    } else {
        // Restore the maximized state recorded when the drag began.
        if (g_savedWasMaximizedForDragHide) {
            wp.flags |= WPF_RESTORETOMAXIMIZED;
        }
    }

    // Use SetWindowPlacement to minimize without animation
    wp.showCmd = SW_SHOWMINNOACTIVE;
    SetWindowPlacement(hwnd, &wp);

    // Find and activate the next visible top-level window in Z-order
    HWND nextWnd = nullptr;
    HWND candidate = hwnd;
    for (int i = 0; i < 50; ++i) {
        candidate = GetWindow(candidate, GW_HWNDNEXT);
        if (!candidate) break;
        if (!Platform::WindowIsVisible(candidate)) continue;
        if (IsIconic(candidate)) continue;
        LONG exStyle = GetWindowLong(candidate, GWL_EXSTYLE);
        if (exStyle & WS_EX_TOOLWINDOW) continue;
        if (exStyle & WS_EX_NOACTIVATE) continue;
        LONG style = GetWindowLong(candidate, GWL_STYLE);
        if (style & WS_CHILD) continue;
        wchar_t title[2] = {};
        GetWindowTextW(candidate, title, 2);
        if (title[0] == 0 && !(exStyle & WS_EX_APPWINDOW)) continue;
        nextWnd = candidate;
        break;
    }

    if (nextWnd) {
        (void)Platform::WindowSetForeground(nextWnd);
        AppendDiagLogF(L"MinimizeWindowNoAnimation: activate next window %p", (void*)nextWnd);
    }

    AppendDiagLogF(L"MinimizeWindowNoAnimation: hwnd=%p, now iconic=%d visible=%d",
                   (void*)hwnd, (int)IsIconic(hwnd), (int)Platform::WindowIsVisible(hwnd));
}

void ShowManagerWindowNow() {
    if (!g_hwnd) return;
    AppendDiagLogF(L"ShowManagerWindowNow: begin, visible=%d iconic=%d", (int)Platform::WindowIsVisible(g_hwnd), (int)IsIconic(g_hwnd));
    AllowSetForegroundWindow(ASFW_ANY);

    // Determine the correct showCmd to use when restoring.
    // Strategy: first SetWindowPlacement to restore rcNormalPosition,
    // then ShowWindow with the explicit state (SW_SHOWMAXIMIZED or SW_RESTORE)
    // to guarantee the window returns to its pre-drag size.
    int restoreCmd = SW_RESTORE;
    if (g_savedPlacementForDragHideValid) {
        WINDOWPLACEMENT wp = g_savedPlacementForDragHide;
        wp.length = sizeof(WINDOWPLACEMENT);
        // Restore position/size data via SetWindowPlacement
        wp.showCmd = SW_RESTORE;
        SetWindowPlacement(g_hwnd, &wp);
        // Use IsZoomed snapshot (saved before minimize) to decide final state
        restoreCmd = g_savedWasMaximizedForDragHide ? SW_SHOWMAXIMIZED : SW_RESTORE;
        g_savedPlacementForDragHideValid = false;
        g_savedPlacementForTrayMinimizeValid = false;
        AppendDiagLogF(L"ShowManagerWindowNow: drag restore, wasMaximized=%d", (int)g_savedWasMaximizedForDragHide);
    } else if (g_savedPlacementForHotkeyHideValid) {
        WINDOWPLACEMENT wp = g_savedPlacementForHotkeyHide;
        wp.length = sizeof(WINDOWPLACEMENT);
        wp.showCmd = SW_RESTORE;
        SetWindowPlacement(g_hwnd, &wp);
        restoreCmd = g_savedWasMaximizedForHotkeyHide ? SW_SHOWMAXIMIZED : SW_RESTORE;
        g_savedPlacementForHotkeyHideValid = false;
        AppendDiagLogF(L"ShowManagerWindowNow: hotkey restore, wasMaximized=%d", (int)g_savedWasMaximizedForHotkeyHide);
    } else if (g_savedPlacementForTrayMinimizeValid) {
        WINDOWPLACEMENT wp = g_savedPlacementForTrayMinimize;
        wp.length = sizeof(WINDOWPLACEMENT);
        wp.showCmd = SW_RESTORE;
        SetWindowPlacement(g_hwnd, &wp);
        restoreCmd = g_savedWasMaximizedForTrayMinimize ? SW_SHOWMAXIMIZED : SW_RESTORE;
        g_savedPlacementForTrayMinimizeValid = false;
        AppendDiagLogF(L"ShowManagerWindowNow: tray restore, wasMaximized=%d", (int)g_savedWasMaximizedForTrayMinimize);
    }

    (void)Platform::WindowShow(g_hwnd, SW_SHOW);
    (void)Platform::WindowShow(g_hwnd, restoreCmd);
    EnsureWindowOnScreen(g_hwnd);

    auto forceForeground = [](HWND hwnd) {
        SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
        SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
        BringWindowToTop(hwnd);
        if (Platform::WindowSetForeground(hwnd)) {
            SetActiveWindow(hwnd);
            SetFocus(hwnd);
            return;
        }

        DWORD currentThread = static_cast<DWORD>(Platform::CurrentThreadId());
        DWORD targetThread = static_cast<DWORD>(Platform::WindowThreadId(hwnd));
        DWORD foregroundThread = 0;
        HWND foreground = GetForegroundWindow();
        if (foreground) {
            foregroundThread = static_cast<DWORD>(Platform::WindowThreadId(foreground));
        }
        if (targetThread && targetThread != currentThread) {
            AttachThreadInput(currentThread, targetThread, TRUE);
        }
        if (foregroundThread && foregroundThread != currentThread) {
            AttachThreadInput(currentThread, foregroundThread, TRUE);
        }
        BringWindowToTop(hwnd);
        (void)Platform::WindowSetForeground(hwnd);
        SetActiveWindow(hwnd);
        SetFocus(hwnd);
        if (foregroundThread && foregroundThread != currentThread) {
            AttachThreadInput(currentThread, foregroundThread, FALSE);
        }
        if (targetThread && targetThread != currentThread) {
            AttachThreadInput(currentThread, targetThread, FALSE);
        }
    };
    forceForeground(g_hwnd);

    AppendDiagLogF(L"ShowManagerWindowNow: end, visible=%d iconic=%d", (int)Platform::WindowIsVisible(g_hwnd), (int)IsIconic(g_hwnd));
}

void HandleShowManagerHotkeyToggle() {
    if (!g_hwnd) return;

    // Toggle behavior:
    // - visible + foreground: save placement and hide
    // - visible + background: bring to front without changing geometry
    // - hidden/iconic: restore from saved placement path
    bool isVisible = Platform::WindowIsVisible(g_hwnd);
    bool isIconic = (IsIconic(g_hwnd) != 0);
    HWND fg = GetForegroundWindow();
    bool isForeground = (fg == g_hwnd);

    if (isVisible && !isIconic && isForeground) {
        AppendDiagLogLine(L"ShowManagerHotkey: toggle HIDE");
        WINDOWPLACEMENT wp = { sizeof(WINDOWPLACEMENT) };
        GetWindowPlacement(g_hwnd, &wp);
        g_savedWasMaximizedForHotkeyHide = (IsZoomed(g_hwnd) != 0);
        if (g_savedWasMaximizedForHotkeyHide) {
            wp.flags |= WPF_RESTORETOMAXIMIZED;
        }
        g_savedPlacementForHotkeyHide = wp;
        g_savedPlacementForHotkeyHideValid = true;
        MinimizeWindowNoAnimation(g_hwnd);
    } else if (isVisible && !isIconic) {
        AppendDiagLogLine(L"ShowManagerHotkey: bring to front (no resize)");
        AllowSetForegroundWindow(ASFW_ANY);
        SetWindowPos(g_hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
        SetWindowPos(g_hwnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
        BringWindowToTop(g_hwnd);
        (void)Platform::WindowSetForeground(g_hwnd);
        SetActiveWindow(g_hwnd);
        SetFocus(g_hwnd);
    } else {
        AppendDiagLogLine(L"ShowManagerHotkey: toggle SHOW (restore)");
        ShowManagerWindowNow();
    }
}

// Tray Icon

void EnsureTrayIconAdded(HWND hwnd) {
    (void)hwnd;
    if (!g_hwnd) return;
    if (g_trayIconAdded) return;

    AppendDiagLogF(L"EnsureTrayIconAdded: 传入hwnd=%p, 使用g_hwnd=%p", (void*)hwnd, (void*)g_hwnd);

    ZeroMemory(&g_trayNid, sizeof(g_trayNid));
    g_trayNid.cbSize = sizeof(g_trayNid);
    g_trayNid.hWnd = g_hwnd;
    g_trayNid.uID = 1;
    g_trayNid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    g_trayNid.uCallbackMessage = WM_TRAYICON;
    g_trayNid.hIcon = (HICON)LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1));
    if (!g_trayNid.hIcon) g_trayNid.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wcscpy_s(g_trayNid.szTip, Lw(Tray_Tip).c_str());
    if (Shell_NotifyIconW(NIM_ADD, &g_trayNid)) {
        g_trayIconAdded = true;
        g_trayNid.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &g_trayNid);
        AppendDiagLogF(L"托盘图标：NIM_ADD 成功，hWnd=%p uCallbackMessage=0x%X",
                       (void*)g_trayNid.hWnd, g_trayNid.uCallbackMessage);
    } else {
        AppendDiagLogF(L"托盘图标：NIM_ADD 失败，err=%lu",
                       static_cast<unsigned long>(Platform::LastSystemError()));
    }
}

void RemoveTrayIconIfAny(HWND hwnd) {
    (void)hwnd;
    if (!g_trayIconAdded) return;
    Shell_NotifyIconW(NIM_DELETE, &g_trayNid);
    g_trayIconAdded = false;
    ZeroMemory(&g_trayNid, sizeof(g_trayNid));
    AppendDiagLogLine(L"托盘图标：NIM_DELETE");
}

void RequestHarkenSearchExit(HWND hwnd, const wchar_t* reason) {
    if (g_appState && RequestInfoMetadataLeave(*g_appState,[hwnd] {
        RequestHarkenSearchExit(hwnd,L"metadata-save-confirmed");
    })) {
        // A tray exit can arrive while the main window is minimized. The
        // confirmation needs a visible frame before shutdown can continue.
        if (hwnd && Platform::WindowIsValid(hwnd) &&
            (IsIconic(hwnd) || !Platform::WindowIsVisible(hwnd)))
            ShowManagerWindowNow();
        return;
    }
    static constexpr int kExitSettingsSaveAttempts = 3;
    g_explicitExitRequested.store(true, std::memory_order_release);
    AppendDiagLogF(L"ExitRequested: reason=%s hwnd=%p", reason ? reason : L"(unknown)", (void*)hwnd);
    DIAG_SYSTEM("Shutdown", "ExitRequested");
    bool settingsSaved = !g_appState;
    for (int attempt = 0; g_appState && attempt < kExitSettingsSaveAttempts; ++attempt) {
        if (TrySaveAppSettings(*g_appState)) {
            settingsSaved = true;
            break;
        }
        if (attempt + 1 < kExitSettingsSaveAttempts) Platform::SleepMilliseconds(50);
    }
    if (!settingsSaved && g_appState) {
        // TrySaveAppSettings persists a non-secret failure notice (and, for a
        // failed clear, the durable pending-clear intent) for the next launch.
        // Shutdown and automatic-update handoff must still continue.
        g_appState->loadErrorToast = u8"设置未能完整保存；应用仍会退出。";
        g_appState->loadErrorToastUntil = ImGui::GetTime() + 6.0;
        DIAG_SYSTEM("Shutdown", "SettingsSaveFailedProceeding");
    }
    RemoveTrayIconIfAny(hwnd);
    if (hwnd && Platform::WindowIsValid(hwnd)) {
        DestroyWindow(hwnd);
    }
    PostQuitMessage(0);
}

void MaybeShowTrayMinimizeHint(AppState& state, HWND hwnd) {
    if (!hwnd) return;
    if (state.settings.trayMinimizeHintShown) return;

    EnsureTrayIconAdded(hwnd);
    if (!g_trayIconAdded) return;

    NOTIFYICONDATAW nid = g_trayNid;
    nid.uFlags = NIF_INFO;
    wcscpy_s(nid.szInfoTitle, Lw(Tray_BalloonTitle).c_str());
    wcscpy_s(nid.szInfo, Lw(Tray_BalloonHint).c_str());    nid.dwInfoFlags = NIIF_INFO;
    Shell_NotifyIconW(NIM_MODIFY, &nid);

    state.settings.trayMinimizeHintShown = true;
    SaveAppSettings(state);
}

void ShowTrayContextMenu(HWND hwnd) {
    if (!hwnd) return;

    HMENU hMenu = CreatePopupMenu();
    if (!hMenu) return;

    bool visible = Platform::WindowIsVisible(hwnd);
    AppendMenuW(hMenu, MF_STRING, TRAY_CMD_TOGGLE_SHOW, visible ? Lw(Tray_MenuHide).c_str() : Lw(Tray_MenuShow).c_str());
    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(hMenu, MF_STRING, TRAY_CMD_EXIT, Lw(Tray_MenuExit).c_str());

    POINT pt;
    GetCursorPos(&pt);

    (void)Platform::WindowSetForeground(hwnd);
    UINT cmd = TrackPopupMenu(hMenu,
                              TPM_RETURNCMD | TPM_RIGHTBUTTON,
                              pt.x, pt.y, 0, hwnd, nullptr);
    PostMessageW(hwnd, WM_NULL, 0, 0);
    DestroyMenu(hMenu);

    if (cmd == 0) return;

    if (cmd == TRAY_CMD_TOGGLE_SHOW) {
        if (visible) {
            MinimizeWindowNoAnimation(hwnd);
            EnsureTrayIconAdded(hwnd);
            if (g_appState) {
                MaybeShowTrayMinimizeHint(*g_appState, hwnd);
            }
        } else {
            ShowManagerWindowNow();
        }
        return;
    }

    if (cmd == TRAY_CMD_EXIT) {
        RequestHarkenSearchExit(hwnd, L"tray-exit");
        return;
    }
}

// Global Hotkey Helpers

UINT ImGuiKeyToVirtualKey(ImGuiKey key) {
    if (key >= ImGuiKey_A && key <= ImGuiKey_Z) {
        return (UINT)('A' + (key - ImGuiKey_A));
    }
    if (key >= ImGuiKey_0 && key <= ImGuiKey_9) {
        return (UINT)('0' + (key - ImGuiKey_0));
    }
    if (key >= ImGuiKey_F1 && key <= ImGuiKey_F24) {
        return (UINT)(VK_F1 + (key - ImGuiKey_F1));
    }
    switch (key) {
        case ImGuiKey_Space: return VK_SPACE;
        case ImGuiKey_Enter: return VK_RETURN;
        case ImGuiKey_Escape: return VK_ESCAPE;
        case ImGuiKey_Tab: return VK_TAB;
        case ImGuiKey_Backspace: return VK_BACK;
        case ImGuiKey_Delete: return VK_DELETE;
        case ImGuiKey_Insert: return VK_INSERT;
        case ImGuiKey_Home: return VK_HOME;
        case ImGuiKey_End: return VK_END;
        case ImGuiKey_PageUp: return VK_PRIOR;
        case ImGuiKey_PageDown: return VK_NEXT;
        case ImGuiKey_UpArrow: return VK_UP;
        case ImGuiKey_DownArrow: return VK_DOWN;
        case ImGuiKey_LeftArrow: return VK_LEFT;
        case ImGuiKey_RightArrow: return VK_RIGHT;
        default: break;
    }
    return 0;
}

UINT HotkeyModsFromKeyBinding(const KeyBinding& kb) {
    UINT mods = 0;
    if (kb.ctrl) mods |= MOD_CONTROL;
    if (kb.shift) mods |= MOD_SHIFT;
    if (kb.alt) mods |= MOD_ALT;
    return mods;
}

bool IsGlobalHotkeyPressedOnce(const KeyBinding& kb) {
    if (kb.key == ImGuiKey_None) return false;

    UINT vk = ImGuiKeyToVirtualKey(kb.key);
    if (vk == 0) return false;

    auto isDown = [](int vkey) -> bool {
        return (GetAsyncKeyState(vkey) & 0x8000) != 0;
    };

    if (kb.ctrl && !isDown(VK_CONTROL)) return false;
    if (kb.shift && !isDown(VK_SHIFT)) return false;
    if (kb.alt && !isDown(VK_MENU)) return false;

    static bool s_prevDown = false;
    bool down = isDown((int)vk);
    bool pressed = down && !s_prevDown;
    s_prevDown = down;
    return pressed;
}

void ApplyShowManagerGlobalHotkey(const AppSettings& settings) {
    if (!g_hwnd) return;
    UnregisterHotKey(g_hwnd, HOTKEY_ID_SHOW_MANAGER);

    g_showManagerHotkeyRegisterOk = true;
    if (!settings.enableShowManagerHotkey || settings.showManagerHotkey.key == ImGuiKey_None) return;

    UINT vk = ImGuiKeyToVirtualKey(settings.showManagerHotkey.key);
    UINT mods = HotkeyModsFromKeyBinding(settings.showManagerHotkey);
    if (vk == 0) {
        g_showManagerHotkeyRegisterOk = false;
        return;
    }

    // MOD_NOREPEAT：防止长按热键导致窗口反复闪现
    if (!RegisterHotKey(g_hwnd, HOTKEY_ID_SHOW_MANAGER, mods | MOD_NOREPEAT, vk)) {
        DWORD err = Platform::LastSystemError();
        if (err == ERROR_HOTKEY_ALREADY_REGISTERED) {
            g_showManagerHotkeyRegisterOk = false; // 记录冲突状态，供 UI 提示
            AppendDiagLogLine(L"HOTKEY CONFLICT: The selected key combo is already in use by another application (e.g., WeChat, QQ, or System).");
        } else {
            AppendDiagLogF(L"HOTKEY ERROR: System code %lu", (unsigned long)err);
        }
    }
}

// Font Loading

// Chrome group: built at the fixed kChromeFontPx. These remain the ImGui default
// font, so any call site that does not opt into a ReadingTextScope renders chrome.
static ImFont* g_uiFontRegular = nullptr;
static ImFont* g_uiFontEmphasis = nullptr;
static ImFont* g_uiFontMono = nullptr;
static ImFont* g_uiMeterFont = nullptr;  // F-018: 10px ASCII-only, for narrow meter bars

// Reading group: built at the user's readingFontSize. Returned by the GetUIFont*()
// accessors only while g_inReadingScope is true (see ReadingTextScope).
static ImFont* g_readingRegular = nullptr;
static ImFont* g_readingEmphasis = nullptr;
static ImFont* g_readingMono = nullptr;
static ImFont* g_uiListFont = nullptr;   // reading-sized; the file-list body font

// True while rendering a reading surface. Flipped by ReadingTextScope.
// File-local: every reader (accessors) and writer (the scope ctors/dtors)
// lives in this TU, so it stays static to prevent callers from bypassing
// the RAII save/restore by poking the flag directly.
static bool g_inReadingScope = false;

static ImFont* AddFontFromCandidates(ImGuiIO& io,
                                     const char* const* candidates,
                                     int candidateCount,
                                     float fontPixelSize,
                                     const ImFontConfig& config,
                                     const ImWchar* ranges) {
    for (int i = 0; i < candidateCount; ++i) {
        const char* path = candidates[i];
        if (!path || !PathExistsUtf8(path)) continue;
        ImFont* f = io.Fonts->AddFontFromFileTTF(path, fontPixelSize, &config, ranges);
        if (f) return f;
    }
    return nullptr;
}

void LoadChineseFont(ImGuiIO& io, float chromePixelSize, float readingPixelSize, float listPixelSize) {
    const char* uiRegularLatinBase[] = {
        "C:\\Windows\\Fonts\\SegoeUIVF.ttf", // Segoe UI Variable (Win11, closer to WebUI style)
        "C:\\Windows\\Fonts\\segoeui.ttf",   // Segoe UI fallback
    };
    const char* uiRegularCjkMerge[] = {
        "C:\\Windows\\Fonts\\msyh.ttc",   // Microsoft YaHei UI - rounded and modern CJK
        "C:\\Windows\\Fonts\\msyhbd.ttc", // Bold fallback
        "C:\\Windows\\Fonts\\Deng.ttf",   // DengXian fallback (modern sans)
        "C:\\Windows\\Fonts\\Dengb.ttf",  // DengXian Bold fallback
        "C:\\Windows\\Fonts\\simhei.ttf", // Legacy fallback only
        "C:\\Windows\\Fonts\\simsun.ttc", // Legacy fallback only
    };
    const char* uiEmphasisLatinBase[] = {
        "C:\\Windows\\Fonts\\SegoeUIVF.ttf", // Keep family tone consistent when semibold is unavailable
        "C:\\Windows\\Fonts\\seguisb.ttf",
        "C:\\Windows\\Fonts\\segoeuib.ttf",
    };
    const char* uiEmphasisCjkMerge[] = {
        "C:\\Windows\\Fonts\\msyhbd.ttc",
        "C:\\Windows\\Fonts\\msyh.ttc",
        "C:\\Windows\\Fonts\\Dengb.ttf",
        "C:\\Windows\\Fonts\\Deng.ttf",
        "C:\\Windows\\Fonts\\simhei.ttf", // Legacy fallback only
    };
    const char* monoPaths[] = {
        "C:\\Windows\\Fonts\\CascadiaMono.ttf",
        "C:\\Windows\\Fonts\\consola.ttf",
        "C:\\Windows\\Fonts\\consolab.ttf",
    };

    ImFontConfig uiConfig;
    uiConfig.MergeMode = false;
    uiConfig.OversampleH = 3;
    uiConfig.OversampleV = 2;
    uiConfig.PixelSnapH = true;
    // Slightly brighten rasterization to match WebUI's softer/younger text feeling on dark backgrounds.
    uiConfig.RasterizerMultiply = 1.03f;

    // Extended Latin glyph range: default + symbols used in UI (stars, arrows, checkmarks, etc.)
    // Segoe UI on Windows 10/11 contains all these glyphs natively.
    static const ImWchar kExtendedLatinRanges[] = {
        0x0020, 0x00FF,  // Basic Latin + Latin-1 Supplement (same as GetGlyphRangesDefault)
        0x2000, 0x206F,  // General Punctuation (—, –, ", etc.)
        0x2100, 0x214F,  // Letterlike Symbols
        0x2190, 0x21FF,  // Arrows (→, ←, ↔, ↕, etc.)
        0x2200, 0x22FF,  // Mathematical Operators (⊞, ±, etc.)
        0x2300, 0x23FF,  // Miscellaneous Technical
        0x2500, 0x25FF,  // Box Drawing + Block Elements + Geometric Shapes (●, ○, ▶, etc.)
        0x2600, 0x26FF,  // Miscellaneous Symbols
        0x2700, 0x27FF,  // Dingbats (✓, ✗, ⟳, etc.)
        0x2800, 0x28FF,  // Braille Patterns (progress spinners)
        0,
    };

    // Base Latin/UI glyphs first (Segoe-like modern look), then merge Chinese glyphs
    // to reduce CJK/Latin mismatch while keeping Chinese coverage complete.
    // Build a {regular, emphasis, mono} triplet at a given pixel size. Reuses the
    // candidate arrays declared above. Lucide icons are merged into the regular
    // face so icon buttons resolve glyphs at this size.
    auto buildTriplet = [&](float px,
                            ImFont** outRegular,
                            ImFont** outEmphasis,
                            ImFont** outMono) {
        // Regular: Latin base + CJK merge.
        *outRegular = AddFontFromCandidates(
            io, uiRegularLatinBase,
            (int)(sizeof(uiRegularLatinBase) / sizeof(uiRegularLatinBase[0])),
            px, uiConfig, kExtendedLatinRanges);
        if (!*outRegular) {
            *outRegular = AddFontFromCandidates(
                io, uiRegularCjkMerge,
                (int)(sizeof(uiRegularCjkMerge) / sizeof(uiRegularCjkMerge[0])),
                px, uiConfig, io.Fonts->GetGlyphRangesChineseFull());
            if (!*outRegular) *outRegular = io.Fonts->AddFontDefault();
        } else {
            ImFontConfig mergeCjk = uiConfig;
            mergeCjk.MergeMode = true;
            mergeCjk.RasterizerMultiply = 1.08f;
            (void)AddFontFromCandidates(
                io, uiRegularCjkMerge,
                (int)(sizeof(uiRegularCjkMerge) / sizeof(uiRegularCjkMerge[0])),
                px, mergeCjk, io.Fonts->GetGlyphRangesChineseFull());
        }
        // Merge Lucide icons into THIS regular face at THIS size.
        aura::ui::MergeLucideFont(io.Fonts, px);

        // Emphasis: semibold Latin base + CJK merge.
        ImFontConfig emphasisConfig = uiConfig;
        *outEmphasis = AddFontFromCandidates(
            io, uiEmphasisLatinBase,
            (int)(sizeof(uiEmphasisLatinBase) / sizeof(uiEmphasisLatinBase[0])),
            px, emphasisConfig, kExtendedLatinRanges);
        if (!*outEmphasis) {
            *outEmphasis = AddFontFromCandidates(
                io, uiEmphasisCjkMerge,
                (int)(sizeof(uiEmphasisCjkMerge) / sizeof(uiEmphasisCjkMerge[0])),
                px, emphasisConfig, io.Fonts->GetGlyphRangesChineseFull());
            if (!*outEmphasis) *outEmphasis = *outRegular;
        } else {
            ImFontConfig mergeCjkEmphasis = emphasisConfig;
            mergeCjkEmphasis.MergeMode = true;
            mergeCjkEmphasis.RasterizerMultiply = 1.08f;
            (void)AddFontFromCandidates(
                io, uiEmphasisCjkMerge,
                (int)(sizeof(uiEmphasisCjkMerge) / sizeof(uiEmphasisCjkMerge[0])),
                px, mergeCjkEmphasis, io.Fonts->GetGlyphRangesChineseFull());
        }

        // Mono: +1px optical compensation.
        ImFontConfig monoConfig;
        monoConfig.MergeMode = false;
        monoConfig.OversampleH = 2;
        monoConfig.OversampleV = 1;
        monoConfig.PixelSnapH = true;
        *outMono = AddFontFromCandidates(
            io, monoPaths,
            (int)(sizeof(monoPaths) / sizeof(monoPaths[0])),
            px + 1.0f, monoConfig, kExtendedLatinRanges);
        if (!*outMono) *outMono = *outRegular;
    };

    buildTriplet(chromePixelSize, &g_uiFontRegular, &g_uiFontEmphasis, &g_uiFontMono);
    buildTriplet(readingPixelSize, &g_readingRegular, &g_readingEmphasis, &g_readingMono);

    // Dedicated list font (independent from system font size).
    float listPx = listPixelSize;
    ImFontConfig listConfig = uiConfig;
    listConfig.MergeMode = false;
    g_uiListFont = AddFontFromCandidates(
        io,
        uiRegularLatinBase,
        (int)(sizeof(uiRegularLatinBase) / sizeof(uiRegularLatinBase[0])),
        listPx,
        listConfig,
        kExtendedLatinRanges);
    if (!g_uiListFont) {
        g_uiListFont = AddFontFromCandidates(
            io,
            uiRegularCjkMerge,
            (int)(sizeof(uiRegularCjkMerge) / sizeof(uiRegularCjkMerge[0])),
            listPx,
            listConfig,
            io.Fonts->GetGlyphRangesChineseFull());
    } else {
        ImFontConfig mergeCjkList = listConfig;
        mergeCjkList.MergeMode = true;
        mergeCjkList.RasterizerMultiply = 1.08f;
        (void)AddFontFromCandidates(
            io,
            uiRegularCjkMerge,
            (int)(sizeof(uiRegularCjkMerge) / sizeof(uiRegularCjkMerge[0])),
            listPx,
            mergeCjkList,
            io.Fonts->GetGlyphRangesChineseFull());
    }
    if (!g_uiListFont) g_uiListFont = g_uiFontRegular;

    // F-018: Small ASCII-only font for Loudness Meter numeric readouts / channel labels
    // when barW is narrow. No CJK merge needed — meter labels are L/R/C/LFE/Lss/W/Y/Z etc.
    {
        ImFontConfig meterConfig;
        meterConfig.MergeMode = false;
        meterConfig.OversampleH = 2;
        meterConfig.OversampleV = 2;
        meterConfig.PixelSnapH = true;
        g_uiMeterFont = AddFontFromCandidates(
            io,
            uiRegularLatinBase,
            (int)(sizeof(uiRegularLatinBase) / sizeof(uiRegularLatinBase[0])),
            10.0f,
            meterConfig,
            kExtendedLatinRanges);
        if (!g_uiMeterFont) g_uiMeterFont = g_uiFontRegular;
    }

    // F-045: Lucide icons are merged per-triplet inside buildTriplet (into each
    // regular face at its own size), so both chrome and reading regulars carry
    // icons; the meter face deliberately does not.

    io.FontDefault = g_uiFontRegular;
}

ImFont* GetUIFontRegular() {
    if (g_inReadingScope && g_readingRegular) return g_readingRegular;
    return g_uiFontRegular;
}

ImFont* GetUIFontEmphasis() {
    if (g_inReadingScope) return g_readingEmphasis ? g_readingEmphasis : g_readingRegular;
    return g_uiFontEmphasis ? g_uiFontEmphasis : g_uiFontRegular;
}

ImFont* GetUIFontMono() {
    if (g_inReadingScope) return g_readingMono ? g_readingMono : g_readingRegular;
    return g_uiFontMono ? g_uiFontMono : g_uiFontRegular;
}

ImFont* GetUIListFont() {
    // Reading-sized fallback (not chrome) so the list keeps reading metrics.
    return g_uiListFont ? g_uiListFont : g_readingRegular;
}

ImFont* GetUIMeterFont() {
    return g_uiMeterFont ? g_uiMeterFont : g_uiFontRegular;
}

ReadingTextScope::ReadingTextScope() : prev(g_inReadingScope) {
    g_inReadingScope = true;
    ImGui::PushFont(g_readingRegular ? g_readingRegular : g_uiFontRegular);
}
ReadingTextScope::~ReadingTextScope() {
    ImGui::PopFont();
    g_inReadingScope = prev;
}

ChromeTextScope::ChromeTextScope() : prev(g_inReadingScope) {
    g_inReadingScope = false;
    ImGui::PushFont(g_uiFontRegular);
}
ChromeTextScope::~ChromeTextScope() {
    ImGui::PopFont();
    g_inReadingScope = prev;
}

// UI Scaling (Scheme B): rebuild fonts + scale style sizes (debounced)

static void RebuildImGuiFontsForSizePx(float chromePx, float readingPx, float listPx) {
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();
    LoadChineseFont(io, chromePx, readingPx, listPx);
    io.Fonts->Build();
    // Recreate GPU resources (OpenGL backend). Fonts texture is part of device objects.
    ImGui_ImplOpenGL3_DestroyDeviceObjects();
    ImGui_ImplOpenGL3_CreateDeviceObjects();
}

static int SnapUiFontSizePx(int px) {
    int best = kFontSteps[0];
    int bestDist = abs(px - best);
    for (int i = 1; i < kFontStepCount; ++i) {
        int d = abs(px - kFontSteps[i]);
        if (d < bestDist) {
            best = kFontSteps[i];
            bestDist = d;
        }
    }
    return best;
}

static void ApplyThemeAndScaleForSettings(const AppState& state, float scale) {
    const bool customThemeSelected =
        state.settings.theme == AppTheme::Custom && !state.settings.customThemeName.empty();
    bool appliedVisualTheme = false;

    if (state.settings.theme == AppTheme::Custom && !state.settings.customThemeName.empty()) {
        if (!ThemeColors::customized()) {
            try {
                std::string exeDir = GetExeDirUtf8();
                std::string filePath = exeDir + "\\theme_custom.json";
                appliedVisualTheme = ThemeColors::loadAndApplyThemeFile(filePath);
            } catch (...) {}
        }
        if (!appliedVisualTheme && ThemeColors::customized()) {
            ThemeColors::applyToImGui();
            appliedVisualTheme = true;
        }
        if (!appliedVisualTheme) {
            ApplyTheme(AppTheme::Dark);
            appliedVisualTheme = true;
        }
    } else {
        if (!state.showThemeEditor) {
            ThemeColors::deactivateCustomTheme();
        }
        ApplyTheme(state.settings.theme);
        appliedVisualTheme = true;
        if (state.settings.useCustomColors) {
            ApplyCustomColors(state.settings);
        }
        if (!state.settings.useCustomColors &&
            ThemeColors::shouldApplyCurrentScheme(state.showThemeEditor, customThemeSelected)) {
            ThemeColors::applyToImGui();
        }
    }
    if (!appliedVisualTheme &&
        ThemeColors::shouldApplyCurrentScheme(state.showThemeEditor, customThemeSelected)) {
        ThemeColors::applyToImGui();
    }
    ImGuiStyle& style = ImGui::GetStyle();
    style.ScaleAllSizes(scale);
    ThemeColors::discardPendingUiChromeResync();
}

static ImVec4 GetThemeClearColor() {
    const ImVec4 bg = ThemeColors::current().mainWindowBG;
    if (bg.w > 0.0f) {
        return bg;
    }
    return ImGui::GetStyle().Colors[ImGuiCol_WindowBg];
}

static ImVec4 GetHybridSeamClearColor() {
    const ThemeColorScheme& scheme = ThemeColors::current();
    auto mixColor = [](const ImVec4& a, const ImVec4& b, float t) -> ImVec4 {
        return ImVec4(
            a.x + (b.x - a.x) * t,
            a.y + (b.y - a.y) * t,
            a.z + (b.z - a.z) * t,
            a.w + (b.w - a.w) * t);
    };
    ImVec4 clearBg = mixColor(
        scheme.waveformSplitterBG,
        scheme.waveformBG,
        ThemeColors::isLightChromeTheme(scheme) ? 0.34f : 0.18f);
    clearBg.w = 1.0f;
    return clearBg;
}

static void UpdateImGuiUiScaleSchemeB(const AppState& state) {
    static float s_lastRebuiltReadingPx = -1.0f;
    static float s_lastRebuiltListPx    = -1.0f;
    static int s_lastTheme = -9999;
    static bool s_lastUseCustom = false;

    const int requestedReading = SnapUiFontSizePx(std::clamp(state.settings.readingFontSize, 10, 24));
    const int requestedList    = SnapUiFontSizePx(std::clamp(state.settings.listFontSize, 10, 24));
    const int themeNow = (int)state.settings.theme;
    const bool useCustomNow = state.settings.useCustomColors;

    ImGuiIO& io = ImGui::GetIO();
    io.FontGlobalScale = 1.0f;

    const float chromeScale = (float)kChromeFontPx / kTypographyBasePx;

    const bool needFontRebuild = (s_lastRebuiltReadingPx != (float)requestedReading) ||
                                 (s_lastRebuiltListPx    != (float)requestedList);
    if (needFontRebuild) {
        RebuildImGuiFontsForSizePx((float)kChromeFontPx, (float)requestedReading, (float)requestedList);
        s_lastRebuiltReadingPx = (float)requestedReading;
        s_lastRebuiltListPx    = (float)requestedList;
    }

    const bool themeChanged = (themeNow != s_lastTheme) || (useCustomNow != s_lastUseCustom);
    const bool chromePending = ThemeColors::hasPendingUiChromeResync();

    if (themeChanged || needFontRebuild || chromePending) {
        const bool editorPreviewChromeOnly =
            chromePending && !themeChanged && !needFontRebuild && state.showThemeEditor &&
            ThemeColors::customized();
        if (editorPreviewChromeOnly) {
            ThemeColors::applyToImGui();
            ImGui::GetStyle().ScaleAllSizes(chromeScale);
            ThemeColors::discardPendingUiChromeResync();
        } else {
            ApplyThemeAndScaleForSettings(state, chromeScale);
        }
        s_lastTheme = themeNow;
        s_lastUseCustom = useCustomNow;
    }
}

// OpenGL Context Creation

bool CreateOpenGLContext(HWND hwnd, HDC* hdc, HGLRC* hglrc) {
    *hdc = GetDC(hwnd);

    PIXELFORMATDESCRIPTOR pfd = {};
    pfd.nSize = sizeof(pfd);
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.cDepthBits = 24;
    pfd.cStencilBits = 8;

    int format = ChoosePixelFormat(*hdc, &pfd);
    if (format == 0) return false;

    if (!SetPixelFormat(*hdc, format, &pfd)) return false;

    *hglrc = wglCreateContext(*hdc);
    if (!*hglrc) return false;

    if (!wglMakeCurrent(*hdc, *hglrc)) return false;
    g_mainHdc = *hdc;
    g_hRC = *hglrc;

    return true;
}

// Window Procedure

LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
#if AURA_BUILD_DIAGNOSTICS && ARIS_PREVIEW_ISOLATION && !AURA_INTERNAL_BETA
    if (msg == WM_NCHITTEST) {
        auto& trace = aura::ui::dock::ActiveDockFrameTrace();
        if (trace.enabled && trace.hostNcHitTests < 1000000) ++trace.hostNcHitTests;
    }
#endif
    // Debug: log WM_APP range messages
    if (msg >= WM_APP && msg <= WM_APP + 100) {
        AppendDiagLogF(L"WindowProc: WM_APP+%u hwnd=%p wParam=%llu lParam=0x%llx",
                       msg - WM_APP, (void*)hwnd, (unsigned long long)wParam, (unsigned long long)lParam);
    }

    // IPC from another application instance requests a file open.
    if (msg == WM_COPYDATA) {
        auto* cds = reinterpret_cast<const COPYDATASTRUCT*>(lParam);
        if (cds && cds->dwData == AURO_IPC_OPEN_FILE && g_appState) {
            const auto* wpath = reinterpret_cast<const wchar_t*>(cds->lpData);
            // Validate: cbData must be at least 2 bytes and even (wchar_t pairs).
            if (cds->cbData >= 2 && (cds->cbData % sizeof(wchar_t)) == 0) {
                std::wstring ws(wpath, cds->cbData / sizeof(wchar_t) - 1); // strip null terminator
                g_appState->pendingOpenFilePath = WideToUtf8(ws);
                g_appState->filePlayerFastPathActive = true;
                AppendDiagLogF(L"IPC: WM_COPYDATA open-file: %hs", g_appState->pendingOpenFilePath.c_str());
            }
        }
        return TRUE;
    }

    // Handle WM_DROPFILES before ImGui processes it
    if (msg == WM_DROPFILES) {
        if (!g_appState) {
            DragFinish((HDROP)wParam);
            return 0;
        }

        HDROP hDrop = (HDROP)wParam;
        bool dropHandled = false;
        AppendDiagLogLine(L"WM_DROPFILES: handler entered.");
        try {
            UINT fileCount = DragQueryFileW(hDrop, 0xFFFFFFFF, nullptr, 0);
            AppendDiagLogF(L"WM_DROPFILES: DragQueryFileW returned %u items.", (unsigned)fileCount);
            const UINT kMaxDropItems = 20000; // Guard against huge HDROP payloads causing OOM.
            bool dropTruncated = (fileCount > kMaxDropItems);
            UINT iterCount = (fileCount > kMaxDropItems) ? kMaxDropItems : fileCount;

            std::vector<std::string> folders;
            std::vector<std::string> files;
            std::vector<std::string> dbFiles;
            folders.reserve((size_t)iterCount);
            files.reserve((size_t)iterCount);
            dbFiles.reserve(8);

            for (UINT i = 0; i < iterCount; i++) {
                UINT cch = DragQueryFileW(hDrop, i, nullptr, 0);
                if (cch == 0) continue;
                std::wstring wpath;
                wpath.resize((size_t)cch + 1);
                DragQueryFileW(hDrop, i, &wpath[0], cch + 1);
                wpath.resize(wcslen(wpath.c_str()));

                std::string pathUtf8 = WideToUtf8(wpath);
                if (Platform::FileSystem::Exists(pathUtf8)) {

                    if (Platform::FileSystem::IsDirectory(pathUtf8)) {
                        folders.push_back(pathUtf8);
                    } else {
                        std::filesystem::path p(wpath);
                        std::string ext = p.extension().string();
                        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

                        if (ext == ".db") {
                            dbFiles.push_back(pathUtf8);
                        }
                        else if (ext == ".wav" || ext == ".mp3" || ext == ".flac" || ext == ".ogg" ||
                            ext == ".aiff" || ext == ".aif" || ext == ".m4a" || ext == ".wma") {
                            files.push_back(pathUtf8);
                        }
                    }
                }
            }
            DragFinish(hDrop);
            dropHandled = true;
        
            // Handle .db file drop - route through async BeginSwitchDatabase so
            // the heavy teardown + open does not block the WindowProc thread.
            if (!dbFiles.empty() && g_appState) {
                BeginSwitchDatabase(*g_appState, dbFiles[0]);
                return 0;
            }

            // For large file-only drops, switch to folder scan flow (async) instead of per-file sync import.
            if (folders.empty() && !files.empty()) {
                const size_t kDirectFileDropLimit = 200;
                if (files.size() > kDirectFileDropLimit) {
                    std::unordered_set<std::string> uniq;
                    for (const auto& fp : files) {
                        try {
                            std::filesystem::path p(Utf8ToWide(fp));
                            std::string parent = WideToUtf8(p.parent_path().wstring());
                            if (!parent.empty()) uniq.insert(parent);
                        } catch (...) {
                            // Ignore malformed path and continue.
                        }
                    }
                    folders.assign(uniq.begin(), uniq.end());
                    files.clear();
                }
            }

            // Save drop diagnostics
            if (g_appState) {
                g_appState->lastDropTick = Platform::MonotonicMilliseconds();
                g_appState->lastDropFirstPath.clear();
                if (!folders.empty()) g_appState->lastDropFirstPath = folders.front();
                else if (!files.empty()) g_appState->lastDropFirstPath = files.front();
                char summary[320];
                snprintf(summary, sizeof(summary), "Drop received: folders=%d, audioFiles=%d, recursive=%s%s",
                         (int)folders.size(), (int)files.size(), g_appState->scanRecursive ? "on" : "off",
                         dropTruncated ? " (truncated)" : "");
                g_appState->lastDropSummary = summary;
            }

            #ifdef _DEBUG
            Platform::WriteDebuggerOutput(("WM_DROPFILES: " + std::to_string(folders.size()) + " folders, " + std::to_string(files.size()) + " files\n").c_str());
            #endif

            AppendDiagLogF(L"WM_DROPFILES: classified %d folders, %d audio, %d db files.%s",
                          (int)folders.size(), (int)files.size(), (int)dbFiles.size(),
                          dropTruncated ? L" (truncated)" : L"");

            // Process folders - add to database
            if (g_appState && !folders.empty()) {
                if (g_appState->isScanning.load()) {
                    g_appState->scanDialogAutoStart = false;
                    g_appState->showScanDialog = true;
                    g_appState->setScanFile("A scan is already running. New drop ignored.");
                    return 0;
                }
                // 不立即扫描，保存待扫描路径并弹出对话框让用户确认选项
                g_appState->pendingScanFolders = folders;
                g_appState->scanDialogAutoStart = false;
                g_appState->showScanDialog = true;
            }

            // Process files - add individual files to database (small drops only)
            if (g_appState && !files.empty()) {
                for (const auto& filePath : files) {
                    try {
                        AudioRecord record;
                        record.filePath = filePath;
                        std::filesystem::path p(Utf8ToWide(filePath));
                        record.fileName = WideToUtf8(p.filename().wstring());
                        record.extension = WideToUtf8(p.extension().wstring());
                        const auto fileInfo = Platform::FileSystem::ProbeRegularFile(filePath);
                        record.fileSize = fileInfo.sizeAvailable ? fileInfo.sizeBytes : 0;

                        AudioMetadata metadata;
                        if (MetadataReader::readMetadata(filePath, metadata)) {
                            if (!metadata.title.empty()) record.title = metadata.title;
                            if (!metadata.artist.empty()) record.artist = metadata.artist;
                            if (!metadata.album.empty()) record.album = metadata.album;
                            if (!metadata.description.empty()) record.description = metadata.description;
                            if (!metadata.category.empty()) record.category = metadata.category;
                            else if (!metadata.genre.empty()) record.category = metadata.genre;
                            if (!metadata.keywords.empty()) record.keywords = metadata.keywords;
                            else if (!metadata.comment.empty()) record.keywords = metadata.comment;
                            PopulateExtendedMeta(record.meta, metadata);
                        }

                        g_appState->database.probeAudioFileInfo(filePath,
                                                                record.sampleRate,
                                                                record.channels,
                                                                record.totalFrames,
                                                                record.durationSeconds);

                        if (!g_appState->database.fileExists(filePath)) {
                            g_appState->database.addFile(record);
                        }
                    } catch (...) {
                        // Skip bad file and continue importing others.
                    }
                }

                if (!files.empty()) {
                    g_appState->audioEngine.loadFileAsync(files[0], true);
                    ScheduleDeferredWaveformDetail(*g_appState, files[0], AppState::WAVEFORM_DEFAULT_DETAIL_PEAKS);
                    ResetWaveformZoom(*g_appState);
                }
            }
        } catch (...) {
            if (!dropHandled) DragFinish(hDrop);
            if (g_appState) {
                g_appState->scanDialogAutoStart = false;
                g_appState->lastDropTick = Platform::MonotonicMilliseconds();
                g_appState->lastDropSummary = "Drop handling failed unexpectedly. Please try dropping a top-level folder.";
                g_appState->scanDialogAutoStart = false;
                g_appState->showScanDialog = true;
            }
            return 0;
        }

        return 0;
    }

    // Explorer 重启时重建托盘图标（防止 explorer.exe 崩溃后图标丢失）
    if (WM_TASKBARCREATED && msg == WM_TASKBARCREATED) {
        AppendDiagLogLine(L"TaskbarCreated: rebuild tray icon.");
        g_trayIconAdded = false;  // 重置状态，因为 Explorer 重启后旧图标已经没了
        ZeroMemory(&g_trayNid, sizeof(g_trayNid));
        EnsureTrayIconAdded(hwnd);
        return 0;
    }

    // Handle global hotkey and tray icon BEFORE ImGui handler
    if (msg == WM_HOTKEY) {
        if ((int)wParam == HOTKEY_ID_SHOW_MANAGER) {
            HandleShowManagerHotkeyToggle();
            return 0;
        }
    }
    if (msg == AURO_WM_SHOW_MANAGER) {
        AppendDiagLogLine(L"AURO_WM_SHOW_MANAGER: second instance requested show.");
        ShowManagerWindowNow();
        return 0;
    }
    if (msg == WM_TRAYICON) {
        UINT event = LOWORD(lParam);
        AppendDiagLogF(L"WM_TRAYICON: wParam=%llu lParam=0x%llx event=0x%X", 
                       (unsigned long long)wParam, (unsigned long long)lParam, event);
        if (event == WM_LBUTTONDOWN || event == WM_LBUTTONUP || event == WM_LBUTTONDBLCLK ||
            event == NIN_SELECT || event == NIN_KEYSELECT) {
            AppendDiagLogLine(L"WM_TRAYICON: left click show window.");
            ShowManagerWindowNow();
            return 0;
        }
        if (event == WM_RBUTTONDOWN || event == WM_RBUTTONUP || event == WM_CONTEXTMENU) {
            AppendDiagLogLine(L"WM_TRAYICON: right click menu.");
            ShowTrayContextMenu(hwnd);
            return 0;
        }
    }
    
    if (msg == WM_NCLBUTTONDOWN && wParam == HTCAPTION) {
        aura::ui::dock::NativeCaptionDragEvent event;
        event.kind = aura::ui::dock::NativeCaptionEventKind::NonClientLeftDown;
        event.hwnd = hwnd;
        event.hostHwnd = g_hwnd;
        event.moduleId = aura::ui::dock::ModuleIdForNativeCaptionHwnd(hwnd);
        event.hitTest = aura::ui::dock::NativeCaptionHit::Caption;
        const aura::ui::dock::NativeCaptionProcDecision hostDecision =
            aura::ui::dock::HandleExtraViewportCaptionMessage(event);
        if (!hostDecision.beginDockDrag && hwnd == g_hwnd) {
            // Host caption must not start a module dock drag.
        } else if (aura::ui::dock::ShouldBeginDockDragFromNativeCaption(event) && hwnd != g_hwnd) {
            aura::ui::dock::NativeCaptionHwnd() = hwnd;
            aura::ui::dock::NativeCaptionModuleId() = event.moduleId;
            aura::ui::dock::NativeCaptionDragging() = true;
        }
    }
    ForwardDockPointerTermination(hwnd, msg, lParam);
    if (msg == WM_NCMOUSEMOVE && aura::ui::dock::NativeCaptionDragging()) {
        aura::ui::dock::NativeCaptionDragEvent event;
        event.kind = aura::ui::dock::NativeCaptionEventKind::NonClientMouseMove;
        event.hwnd = hwnd;
        event.moduleId = aura::ui::dock::ModuleIdForNativeCaptionHwnd(hwnd);
        if (event.moduleId.empty()) event.moduleId = aura::ui::dock::NativeCaptionModuleId();
        event.dragging = hwnd == aura::ui::dock::NativeCaptionHwnd();
        (void)aura::ui::dock::ShouldUpdateDockDragFromNativeCaption(event);
    }
    if (msg == WM_CAPTURECHANGED) {
        ReleaseImGuiLeftButtonOnCaptureLoss();
    }
    if (msg == WM_NCLBUTTONUP || msg == WM_LBUTTONUP || msg == WM_CAPTURECHANGED) {
        aura::ui::dock::NativeCaptionDragging() = false;
        aura::ui::dock::NativeCaptionHwnd() = nullptr;
        aura::ui::dock::NativeCaptionModuleId().clear();
    }

    // Let ImGui handle other messages
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam))
        return true;

    switch (msg) {
    case WM_GETMINMAXINFO:
    {
        MINMAXINFO* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
        if (!mmi) return 0;

        HMONITOR hMon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi = { sizeof(mi) };
        if (GetMonitorInfoW(hMon, &mi)) {
            const RECT& work = mi.rcWork;
            const RECT& monitor = mi.rcMonitor;
            mmi->ptMaxPosition.x = work.left - monitor.left;
            mmi->ptMaxPosition.y = work.top - monitor.top;
            mmi->ptMaxSize.x = work.right - work.left;
            mmi->ptMaxSize.y = work.bottom - work.top;
        }
        return 0;
    }
    case WM_SIZE:
        return 0;
    case WM_ACTIVATE:
        if (LOWORD(wParam) != WA_INACTIVE) {

            // Defer saved-placement consumption until the window is no longer iconic.

            // 当用户点击任务栏恢复最小化窗口时，WM_ACTIVATE 可能先于
            // WM_SYSCOMMAND(SC_RESTORE) 到达。如果此时窗口仍是 iconic
            // （最小化状态），我们 **不能** 在这里消费 saved placement，
            // 否则后续 SC_RESTORE 会走 DefWindowProc 默认路径——把窗口
            // 恢复到"普通大小"，丢失最大化状态。
            //
            // 策略：
            //   iconic → 不主动恢复，让任务栏的正常恢复消息处理原窗口状态
            //   非 iconic + 有 saved state → 二次保障（ShowManagerWindowNow
            //       已还原位置但还没最大化时可能触发 WM_ACTIVATE）

            if (IsIconic(hwnd)) {
                AppendDiagLogF(L"WM_ACTIVATE: iconic, deferring restore to Windows");
            } else {
                // 非 iconic：作为二次保障应用保存状态
                if (g_savedPlacementForDragHideValid) {
                    bool wasMax = g_savedWasMaximizedForDragHide;
                    g_savedPlacementForDragHideValid = false;
                    g_savedPlacementForTrayMinimizeValid = false;
                    (void)Platform::WindowShow(hwnd, wasMax ? SW_SHOWMAXIMIZED : SW_RESTORE);
                    AppendDiagLogF(L"WM_ACTIVATE: non-iconic restore from drag wasMax=%d", (int)wasMax);
                } else if (g_savedPlacementForTrayMinimizeValid) {
                    bool wasMax = g_savedWasMaximizedForTrayMinimize;
                    g_savedPlacementForTrayMinimizeValid = false;
                    g_savedPlacementForDragHideValid = false;
                    (void)Platform::WindowShow(hwnd, wasMax ? SW_SHOWMAXIMIZED : SW_RESTORE);
                    AppendDiagLogF(L"WM_ACTIVATE: non-iconic restore from tray wasMax=%d", (int)wasMax);
                }
            }
        }
        break;
    case WM_CLOSE:
        if (g_explicitExitRequested.load(std::memory_order_acquire) || g_loginPhaseActive) {
            RequestHarkenSearchExit(hwnd, g_loginPhaseActive ? L"login-close" : L"wm-close-explicit");
            return 0;
        }
        if (g_appState) {
            int act = std::clamp(g_appState->settings.closeButtonAction, 0, 2);
            if (act == 0) {
                // Show the manager window only when it is currently hidden.
                // Do not shrink a fullscreen or maximized window with SW_SHOWNORMAL.
                if (!Platform::WindowIsVisible(hwnd) || IsIconic(hwnd)) {
                    ShowManagerWindowNow();
                }
                g_appState->showCloseChoicePopup = true;
                return 0;
            }
            if (act == 1) {
                MinimizeWindowNoAnimation(hwnd);
                EnsureTrayIconAdded(hwnd);
                MaybeShowTrayMinimizeHint(*g_appState, hwnd);
                return 0;
            }
            RequestHarkenSearchExit(hwnd, L"close-button-direct");
            return 0;
        }
        RequestHarkenSearchExit(hwnd, L"wm-close-no-state");
        return 0;
    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU)
            return 0;
        // Intercept SC_RESTORE when the tray or drag state says the window was maximized.
        // Restore saved bounds directly instead of flashing through normal size.
        if ((wParam & 0xfff0) == SC_RESTORE) {
            if (g_savedPlacementForDragHideValid) {
                bool wasMax = g_savedWasMaximizedForDragHide;
                WINDOWPLACEMENT wp = g_savedPlacementForDragHide;
                wp.length = sizeof(WINDOWPLACEMENT);
                wp.showCmd = wasMax ? SW_SHOWMAXIMIZED : SW_RESTORE;
                SetWindowPlacement(hwnd, &wp);
                g_savedPlacementForDragHideValid = false;
                g_savedPlacementForTrayMinimizeValid = false;
                AppendDiagLogF(L"WM_SYSCOMMAND SC_RESTORE: drag wasMax=%d", (int)wasMax);
                return 0; // 阻止 DefWindowProc 处理
            } else if (g_savedPlacementForTrayMinimizeValid) {
                bool wasMax = g_savedWasMaximizedForTrayMinimize;
                WINDOWPLACEMENT wp = g_savedPlacementForTrayMinimize;
                wp.length = sizeof(WINDOWPLACEMENT);
                wp.showCmd = wasMax ? SW_SHOWMAXIMIZED : SW_RESTORE;
                SetWindowPlacement(hwnd, &wp);
                g_savedPlacementForTrayMinimizeValid = false;
                g_savedPlacementForDragHideValid = false;
                AppendDiagLogF(L"WM_SYSCOMMAND SC_RESTORE: tray wasMax=%d", (int)wasMax);
                return 0; // 阻止 DefWindowProc 处理
            }
        }
        break;
    case WM_DESTROY:
        UnregisterHotKey(hwnd, HOTKEY_ID_SHOW_MANAGER);
        RemoveTrayIconIfAny(hwnd);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// Harken Search splash backdrop and logo.

struct StartupThemeAccents {
    ImVec4 accentA;
    ImVec4 accentB;
    int themedParticleChance;
};

static ImVec4 MixStartupColor(const ImVec4& a, const ImVec4& b, float t) {
    return ImVec4(
        a.x + (b.x - a.x) * t,
        a.y + (b.y - a.y) * t,
        a.z + (b.z - a.z) * t,
        1.0f);
}

static StartupThemeAccents ResolveStartupThemeAccents(const AppSettings& settings) {
    StartupThemeAccents out{
        ImVec4(0.56f, 0.83f, 0.96f, 1.0f),
        ImVec4(0.98f, 0.62f, 0.70f, 1.0f),
        32
    };

    if (settings.useCustomColors) {
        out.accentA = settings.customAccentColor;
        out.accentB = MixStartupColor(settings.customAccentColor, ImVec4(1.0f, 1.0f, 1.0f, 1.0f), 0.35f);
        out.themedParticleChance = 46;
        return out;
    }

    switch (settings.theme) {
        case AppTheme::Ocean:
            out.accentA = ImVec4(0.00f, 0.74f, 0.83f, 1.0f); // #00BCD3 — aurora teal (matches resultsSelection)
            out.accentB = ImVec4(0.17f, 0.89f, 0.95f, 1.0f); // #2BE4F1 — bright aurora cyan (resultsPlayedText)
            out.themedParticleChance = 42;
            break;
        case AppTheme::RoseCedar:
            out.accentA = ImVec4(0.84f, 0.47f, 0.59f, 1.0f);
            out.accentB = ImVec4(0.80f, 0.63f, 0.54f, 1.0f);
            out.themedParticleChance = 54;
            break;
        case AppTheme::Custom:
            out.accentA = settings.customAccentColor;
            out.accentB = MixStartupColor(settings.customAccentColor, ImVec4(1.0f, 1.0f, 1.0f, 1.0f), 0.35f);
            out.themedParticleChance = 46;
            break;
        case AppTheme::LapisBlue:
            out.accentA = ImVec4(0.41f, 0.60f, 0.78f, 1.0f); // #6898c8 — lapis steel blue
            out.accentB = ImVec4(0.54f, 0.72f, 0.88f, 1.0f); // #8ab8e0 — sky blue highlight
            out.themedParticleChance = 40;
            break;
        case AppTheme::JadeMist:
            out.accentA = ImVec4(0.14f, 0.43f, 0.41f, 1.0f); // #246e68 — jade teal
            out.accentB = ImVec4(0.79f, 0.58f, 0.52f, 1.0f); // #ca9584 — warm coral
            out.themedParticleChance = 44;
            break;
        case AppTheme::Dark:
        default:
            out.accentA = ImVec4(0.44f, 0.60f, 0.76f, 1.0f);
            out.accentB = ImVec4(0.52f, 0.68f, 0.78f, 1.0f);
            out.themedParticleChance = 54;
            break;
    }

    return out;
}

static std::wstring BuildJsRgbaPrefix(const ImVec4& c) {
    const int r = (int)std::round(std::clamp(c.x, 0.0f, 1.0f) * 255.0f);
    const int g = (int)std::round(std::clamp(c.y, 0.0f, 1.0f) * 255.0f);
    const int b = (int)std::round(std::clamp(c.z, 0.0f, 1.0f) * 255.0f);
    wchar_t buf[64] = {};
    _snwprintf_s(buf, _countof(buf), _TRUNCATE, L"rgba(%d, %d, %d, ", r, g, b);
    return std::wstring(buf);
}

static std::wstring BuildCssRgbTriplet(const ImVec4& c) {
    const int r = (int)std::round(std::clamp(c.x, 0.0f, 1.0f) * 255.0f);
    const int g = (int)std::round(std::clamp(c.y, 0.0f, 1.0f) * 255.0f);
    const int b = (int)std::round(std::clamp(c.z, 0.0f, 1.0f) * 255.0f);
    wchar_t buf[48] = {};
    _snwprintf_s(buf, _countof(buf), _TRUNCATE, L"%d, %d, %d", r, g, b);
    return std::wstring(buf);
}

static std::wstring BuildCssRgba(const ImVec4& c, float alpha) {
    const int r = (int)std::round(std::clamp(c.x, 0.0f, 1.0f) * 255.0f);
    const int g = (int)std::round(std::clamp(c.y, 0.0f, 1.0f) * 255.0f);
    const int b = (int)std::round(std::clamp(c.z, 0.0f, 1.0f) * 255.0f);
    wchar_t buf[96] = {};
    _snwprintf_s(buf, _countof(buf), _TRUNCATE, L"rgba(%d, %d, %d, %.3f)",
                 r, g, b, std::clamp(alpha, 0.0f, 1.0f));
    return std::wstring(buf);
}

namespace HarkenSearchSplash {
    struct Particle {
        float x, y, vx, vy, size, alpha, flashSpeed, phase;
        int colorIdx;
    };

    static std::vector<Particle> particles;
    static float time = 0.0f;
    static bool inited = false;
    static int themedParticleChance = 32;
    static constexpr int kBasePaletteCount = 4;
    static constexpr int kThemePaletteCount = 2;
    static constexpr int kPaletteCount = kBasePaletteCount + kThemePaletteCount;
    static ImU32 palette[kPaletteCount] = {
        IM_COL32(63, 82, 105, 255),    // slate blue
        IM_COL32(78, 111, 139, 255),   // steel blue
        IM_COL32(91, 119, 148, 255),   // muted azure
        IM_COL32(54, 87, 98, 255),     // restrained teal
        IM_COL32(112, 153, 194, 255),  // theme accent A
        IM_COL32(132, 173, 199, 255),  // theme accent B
    };

    static ImU32 ToImU32(const ImVec4& c) {
        return IM_COL32(
            (int)std::round(std::clamp(c.x, 0.0f, 1.0f) * 255.0f),
            (int)std::round(std::clamp(c.y, 0.0f, 1.0f) * 255.0f),
            (int)std::round(std::clamp(c.z, 0.0f, 1.0f) * 255.0f),
            255);
    }

    static void ConfigureThemePalette(const AppSettings& settings) {
        const StartupThemeAccents accents = ResolveStartupThemeAccents(settings);
        palette[4] = ToImU32(accents.accentA);
        palette[5] = ToImU32(accents.accentB);
        themedParticleChance = accents.themedParticleChance;
        inited = false; // Re-seed particle colors when theme changes between startup phases.
    }

    static void Init(float W, float H) {
        if (inited) return;
        inited = true;
        time = 0.0f;
        srand(static_cast<unsigned>(Platform::MonotonicMilliseconds()));
        particles.resize(120);
        for (auto& p : particles) {
            p.x = (float)(rand() % (int)fmaxf(W, 100.0f));
            p.y = (float)(rand() % (int)fmaxf(H, 100.0f));
            p.size = 0.5f + (rand() % 25) / 10.0f;
            p.vx = ((rand() % 100) - 50) / 250.0f;
            p.vy = ((rand() % 100) - 50) / 250.0f;
            p.alpha = (rand() % 100) / 100.0f;
            p.flashSpeed = 0.02f + (rand() % 50) / 1000.0f;
            p.phase = (rand() % 628) / 100.0f;
            const bool useThemeAccent = (rand() % 100) < themedParticleChance;
            p.colorIdx = useThemeAccent
                ? (kBasePaletteCount + (rand() % kThemePaletteCount))
                : (rand() % kBasePaletteCount);
            if (useThemeAccent) {
                p.size *= 1.22f;
                p.flashSpeed *= 0.82f;
            }
        }
    }

    // Draw aurora-like holographic background
    static void DrawBackground(ImDrawList* dl, float W, float H) {
        float cx = W * 0.5f, cy = H * 0.5f;
        Init(W, H);
        time += 0.016f;

// 1. 背景
        // F-016.rose-jade-splash: 浅色主题（RoseCedar/JadeMist）用主题 mainWindowBG 底色；
        //                          深色主题（Dark/Ocean/LapisBlue）保持近黑原味
        {
            const ThemeColorScheme& splashTheme = ThemeColors::current();
            const bool splashIsLight = ThemeColors::isLightChromeTheme(splashTheme);
            const ImU32 bgCol = splashIsLight
                ? ToImU32(splashTheme.mainWindowBG)
                : IM_COL32(5, 5, 8, 255);
            dl->AddRectFilled(ImVec2(0, 0), ImVec2(W, H), bgCol);
        }

// 2. 极光星云 (3个缓慢飘动的彩色光斑)
        auto drawNebula = [&](float bx, float by, float maxR, int r, int g, int b, float peak) {
            for (int i = 0; i < 6; i++) {
                float t = (float)i / 5.0f;
                float rad = maxR * (1.0f - t * 0.7f);
                int a = (int)(peak * (0.2f + t * 0.8f) * 255.0f);
                dl->AddCircleFilled(ImVec2(bx, by), rad, IM_COL32(r, g, b, a), 48);
            }
        };
        drawNebula(cx + sinf(time * 0.3f) * W * 0.08f,
                   cy + cosf(time * 0.2f) * H * 0.06f,
                   fminf(W, H) * 0.32f, 76, 29, 149, 0.07f);
        drawNebula(W * 0.3f + cosf(time * 0.25f) * W * 0.05f,
                   H * 0.7f + sinf(time * 0.35f) * H * 0.04f,
                   fminf(W, H) * 0.25f, 78, 111, 139, 0.035f);
        drawNebula(W * 0.75f + sinf(time * 0.4f) * W * 0.04f,
                   H * 0.3f + cosf(time * 0.3f) * H * 0.05f,
                   fminf(W, H) * 0.22f, 6, 182, 212, 0.05f);

        // 3. Flowing wave with a soft glow.
        struct WaveCfg { float yOff, amp, wl, spd, phase; ImU32 col; ImU32 glowCol; };
        auto withAlpha = [](ImU32 c, int alpha) -> ImU32 {
            return (c & 0x00FFFFFF) | ((ImU32)std::clamp(alpha, 0, 255) << 24);
        };
        WaveCfg waves[] = {
            { H * 0.46f, 58.0f, 0.0030f, 0.8f, 0.0f, withAlpha(IM_COL32(63, 82, 105, 255), 22), withAlpha(IM_COL32(63, 82, 105, 255), 6) },
            { H * 0.50f, 54.0f, 0.0042f, 1.2f, 1.0f, withAlpha(palette[4], 74), withAlpha(palette[4], 22) },
            { H * 0.55f, 44.0f, 0.0035f, 1.0f, 2.0f, withAlpha(palette[5], 42), withAlpha(palette[5], 12) },
            { H * 0.60f, 34.0f, 0.0025f, 0.6f, 3.5f, withAlpha(IM_COL32(34, 211, 238, 255), 18), withAlpha(IM_COL32(34, 211, 238, 255), 5) },
            { H * 0.42f, 38.0f, 0.0050f, 1.5f, 5.0f, withAlpha(IM_COL32(54, 87, 98, 255), 12), withAlpha(IM_COL32(54, 87, 98, 255), 3) },
        };
        for (auto& wv : waves) {
            // Wider translucent glow.
            ImVec2 prevG(0, wv.yOff);
            for (float x = 0; x < W; x += 8.0f) {
                float y = wv.yOff
                    + sinf(x * wv.wl + time * wv.spd + wv.phase) * wv.amp * sinf(time * 0.5f + wv.phase * 0.3f)
                    + cosf(x * 0.008f - time * 0.7f + wv.phase) * 15.0f;
                ImVec2 cur(x, y);
                if (x > 0) dl->AddLine(prevG, cur, wv.glowCol, 8.0f);
                prevG = cur;
            }
            // 主线
            ImVec2 prev(0, wv.yOff);
            for (float x = 0; x < W; x += 6.0f) {
                float y = wv.yOff
                    + sinf(x * wv.wl + time * wv.spd + wv.phase) * wv.amp * sinf(time * 0.5f + wv.phase * 0.3f)
                    + cosf(x * 0.008f - time * 0.7f + wv.phase) * 15.0f;
                ImVec2 cur(x, y);
                if (x > 0) dl->AddLine(prev, cur, wv.col, 2.0f);
                prev = cur;
            }
        }

// 4. 闪烁粒子
        for (auto& p : particles) {
            float angle = sinf(p.x * 0.002f + time * 0.5f)
                        + cosf(p.y * 0.002f + time * 0.5f);
            p.x += cosf(angle) * 1.0f + p.vx;
            p.y += sinf(angle) * 1.0f + p.vy;
            p.alpha += sinf(time * 10.0f * p.flashSpeed + p.phase) * 0.04f;
            float a = fmaxf(0.05f, fminf(1.0f, p.alpha));
            if (p.x < 0) p.x += W; if (p.x > W) p.x -= W;
            if (p.y < 0) p.y += H; if (p.y > H) p.y -= H;
            ImU32 c = (palette[p.colorIdx] & 0x00FFFFFF) | ((ImU32)(a * 200) << 24);
            dl->AddCircleFilled(ImVec2(p.x, p.y), p.size, c);
        }
    }

    // Draw the fluid Harken Search mark.
    static void DrawLogo(ImDrawList* dl, float cx, float cy, float scale = 0.5f) {
        float lw = 10.0f * scale;
        auto lp = [&](float sx, float sy) -> ImVec2 {
            return ImVec2(cx + (sx - 50.0f) * scale, cy + (sy - 50.0f) * scale);
        };

        // Layered low-alpha glow.
        for (int g = 0; g < 3; g++) {
            float gs = scale + g * 0.02f;
            float glw = lw + g * 4.0f;
            int ga = 15 - g * 4;
            auto glp = [&](float sx, float sy) -> ImVec2 {
                return ImVec2(cx + (sx - 50.0f) * gs, cy + (sy - 50.0f) * gs);
            };
            dl->AddLine(glp(30,75), glp(30,35), IM_COL32(255,154,158,ga), glw);
            dl->AddBezierQuadratic(glp(30,35), glp(30,20), glp(45,35), IM_COL32(250,208,196,ga), glw);
            dl->AddLine(glp(45,35), glp(55,65), IM_COL32(161,140,209,ga), glw);
            dl->AddBezierQuadratic(glp(55,65), glp(70,80), glp(70,65), IM_COL32(132,250,176,ga), glw);
            dl->AddLine(glp(70,65), glp(70,25), IM_COL32(143,211,244,ga), glw);
        }

        // Logo path (gradient by segments)
        dl->AddLine(lp(30,75), lp(30,35), IM_COL32(255,154,158,240), lw);
        dl->AddBezierQuadratic(lp(30,35), lp(30,20), lp(45,35), IM_COL32(250,208,196,240), lw);
        dl->AddLine(lp(45,35), lp(55,65), IM_COL32(161,140,209,240), lw);
        dl->AddBezierQuadratic(lp(55,65), lp(70,80), lp(70,65), IM_COL32(132,250,176,240), lw);
        dl->AddLine(lp(70,65), lp(70,25), IM_COL32(143,211,244,240), lw);

        // 端点圆角
        dl->AddCircleFilled(lp(30,75), lw * 0.5f, IM_COL32(255,154,158,240));
        dl->AddCircleFilled(lp(70,25), lw * 0.5f, IM_COL32(143,211,244,240));
    }

    // Draw the ARIS wordmark glow.
    static void DrawLogoText(ImDrawList* dl, float cx, float cy, ImFont* font) {
        const char* text = "ARIS";
        ImVec2 ts = ImGui::CalcTextSize(text);
        float tx = cx - ts.x * 0.5f + 18.0f; // Logo 右侧留空
        float ty = cy - ts.y * 0.5f;

        // Rainbow-ish text glow
        struct GlowColor { ImU32 col; float ox, oy; };
        GlowColor glows[] = {
            { IM_COL32(255,154,158,20), -2, 0 }, { IM_COL32(161,140,209,20), 2, 0 },
            { IM_COL32(143,211,244,15), 0, -2 }, { IM_COL32(132,250,176,15), 0, 2 },
        };
        for (auto& g : glows) {
            dl->AddText(ImVec2(tx + g.ox, ty + g.oy), g.col, text);
        }
        dl->AddText(ImVec2(tx, ty), IM_COL32(255, 255, 255, 230), text);
    }

    // Draw glass card
    static void DrawGlassCard(ImDrawList* dl, float x, float y, float w, float h, float rounding = 24.0f) {
        // 卡片背景
        dl->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + h),
                          IM_COL32(15, 15, 20, 153), rounding);
        // Inner border glow
        dl->AddRect(ImVec2(x, y), ImVec2(x + w, y + h),
                    IM_COL32(255, 255, 255, 13), rounding, 0, 1.0f);

        // Draw the gradient border as four color segments.
        // Top: pink to cyan.
        float step = 4.0f;
        for (float px = x + rounding; px < x + w - rounding; px += step) {
            float t = (px - x) / w;
            int r = (int)(236 + (34 - 236) * t);
            int g = (int)(72 + (211 - 72) * t);
            int b = (int)(153 + (238 - 153) * t);
            dl->AddLine(ImVec2(px, y), ImVec2(px + step, y), IM_COL32(r, g, b, 100), 1.5f);
        }
        // Right: cyan to lime.
        for (float py = y + rounding; py < y + h - rounding; py += step) {
            float t = (py - y) / h;
            int r = (int)(34 + (163 - 34) * t);
            int g = (int)(211 + (230 - 211) * t);
            int b = (int)(238 + (53 - 238) * t);
            dl->AddLine(ImVec2(x + w, py), ImVec2(x + w, py + step), IM_COL32(r, g, b, 80), 1.5f);
        }
        // Bottom: lime to purple.
        for (float px = x + w - rounding; px > x + rounding; px -= step) {
            float t = (x + w - px) / w;
            int r = (int)(163 + (167 - 163) * t);
            int g = (int)(230 + (139 - 230) * t);
            int b = (int)(53 + (250 - 53) * t);
            dl->AddLine(ImVec2(px, y + h), ImVec2(px - step, y + h), IM_COL32(r, g, b, 80), 1.5f);
        }
        // Left: purple to pink.
        for (float py = y + h - rounding; py > y + rounding; py -= step) {
            float t = (y + h - py) / h;
            int r = (int)(167 + (236 - 167) * t);
            int g = (int)(139 + (72 - 139) * t);
            int b = (int)(250 + (153 - 250) * t);
            dl->AddLine(ImVec2(x, py), ImVec2(x, py - step), IM_COL32(r, g, b, 80), 1.5f);
        }
    }

    // Draw spinner
    static void DrawSpinner(ImDrawList* dl, float cx, float cy, float radius, float thickness) {
        float startAngle = time * 4.0f;
        float arcLength = 3.14159f * 1.2f;
        int segments = 30;
        for (int i = 0; i < segments; i++) {
            float t1 = startAngle + arcLength * (float)i / segments;
            float t2 = startAngle + arcLength * (float)(i + 1) / segments;
            float progress = (float)i / segments;
            // Pink-to-cyan gradient.
            int r = (int)(236 + (34 - 236) * progress);
            int g = (int)(72 + (211 - 72) * progress);
            int b = (int)(153 + (238 - 153) * progress);
            int a = (int)(220 * (0.3f + 0.7f * progress));
            ImVec2 p1(cx + cosf(t1) * radius, cy + sinf(t1) * radius);
            ImVec2 p2(cx + cosf(t2) * radius, cy + sinf(t2) * radius);
            dl->AddLine(p1, p2, IM_COL32(r, g, b, a), thickness);
        }
        // 辉光尾部
        float tailAngle = startAngle + arcLength;
        ImVec2 tailPos(cx + cosf(tailAngle) * radius, cy + sinf(tailAngle) * radius);
        dl->AddCircleFilled(tailPos, thickness * 2.0f, IM_COL32(112, 153, 194, 26));
    }
} // namespace HarkenSearchSplash

#if SM_HAS_WEBVIEW2
namespace {
using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;
using CreateWebView2EnvironmentFn = HRESULT(STDAPICALLTYPE*)(
    PCWSTR, PCWSTR, ICoreWebView2EnvironmentOptions*, ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler*);
static std::wstring BuildFlashUiWelcomeHtml(const AppSettings& settings, const std::string& userNameUtf8);
static std::string JsonEscape(const std::string& s);

static std::wstring BuildFlashUiLoginHtml(const AppSettings& settings) {
    return BuildFlashUiWelcomeHtml(settings, settings.userName);
}

static uint32_t StableStartupAvatarHash(const std::string& seed) {
    uint32_t hash = 2166136261u;
    for (unsigned char c : seed) {
        hash ^= c;
        hash *= 16777619u;
    }
    return hash;
}

static std::string Base64EncodeStartupAsset(const std::string& bytes) {
    static constexpr char kTable[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((bytes.size() + 2) / 3) * 4);
    int val = 0;
    int valb = -6;
    for (unsigned char c : bytes) {
        val = (val << 8) + c;
        valb += 8;
        while (valb >= 0) {
            out.push_back(kTable[(val >> valb) & 0x3F]);
            valb -= 6;
        }
    }
    if (valb > -6) out.push_back(kTable[((val << 8) >> (valb + 8)) & 0x3F]);
    while (out.size() % 4) out.push_back('=');
    return out;
}

static bool ReadStartupAvatarSvgFile(const std::string& pathUtf8, std::string& outSvg) {
    outSvg.clear();
    const auto bytes = Platform::FileSystem::ReadFileBytes(pathUtf8);
    if (bytes.empty()) return false;
    outSvg.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    return true;
}

static bool ReadBinaryFile(const std::string& pathUtf8, std::string& outBytes) {
    outBytes.clear();
    const auto bytes = Platform::FileSystem::ReadFileBytes(pathUtf8);
    if (bytes.empty()) return false;
    outBytes.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    return true;
}

static std::string BuildStartupBrandLogoDataUri() {
    const std::string relativePath =
        "assets\\logos\\brand\\harken-search-menu-wordmark-white.png";
    const std::string candidates[] = {
        GetExeDirUtf8() + "\\" + relativePath,
        relativePath,
    };
    for (const std::string& path : candidates) {
        std::string pngBytes;
        if (ReadBinaryFile(path, pngBytes))
            return "data:image/png;base64," + Base64EncodeStartupAsset(pngBytes);
    }
    return {};
}

static void AppendStartupBrandLogoCss(std::wstring& html, bool useLightStartupChrome) {
    const std::string logoDataUri = BuildStartupBrandLogoDataUri();
    if (logoDataUri.empty()) return; // Preserve the visible Harken Search text.
    html += L".brand-word{background-image:url('" + Utf8ToWide(logoDataUri) +
        L"');background-repeat:no-repeat;background-position:left center;"
        L"background-size:290px auto;width:290px;height:44px;"
        L"color:transparent!important;font-size:0!important;text-shadow:none!important;}\n";
    html += L"@media(max-width:640px){.brand-word{background-size:220px auto;width:220px;height:36px;}}\n";
    if (useLightStartupChrome)
        html += L".brand-word{filter:brightness(0)!important;}\n";
}

static std::string BuildLocalDiceBearAvatarDataUri(const std::string& userNameUtf8,
                                                    bool hasCustomAvatar = false,
                                                    int diceBearAvatarIndex = 0) {
    if (hasCustomAvatar) {
        const std::string pngPath = GetCustomAvatarPathUtf8();
        std::string pngBytes;
        if (ReadBinaryFile(pngPath, pngBytes))
            return "data:image/png;base64," + Base64EncodeStartupAsset(pngBytes);
        // fallthrough to DiceBear if file read fails
    }
    int index;
    if (diceBearAvatarIndex >= 1 && diceBearAvatarIndex <= 36) {
        index = diceBearAvatarIndex;
    } else {
        const std::string seed = userNameUtf8.empty() ? std::string("Harken Search User") : userNameUtf8;
        index = 1 + static_cast<int>(StableStartupAvatarHash(seed) % 36u);
    }
    char fileName[32] = {};
    std::snprintf(fileName, sizeof(fileName), "avatar_%03d.svg", index);

    const std::string relPath =
        std::string("assets\\avatars\\dicebear\\lorelei-neutral\\") + fileName;
    const std::string candidates[] = {
        GetExeDirUtf8() + "\\" + relPath,
        relPath,
    };

    std::string svg;
    for (const std::string& path : candidates) {
        if (ReadStartupAvatarSvgFile(path, svg)) {
            return "data:image/svg+xml;base64," + Base64EncodeStartupAsset(svg);
        }
    }
    return {};
}

// Loading-page HTML: energy-vortex particles and progress updates.

static std::wstring BuildFlashUiLoadingHtml(const AppSettings& settings) {
    const StartupThemeAccents themeAccents = ResolveStartupThemeAccents(settings);
    const ThemeColorScheme& startupTheme = ThemeColors::current();
    const bool useLightStartupChrome = ThemeColors::isLightChromeTheme(startupTheme);
    auto mixColor = [](const ImVec4& a, const ImVec4& b, float t) -> ImVec4 {
        return ImVec4(
            a.x + (b.x - a.x) * t,
            a.y + (b.y - a.y) * t,
            a.z + (b.z - a.z) * t,
            a.w + (b.w - a.w) * t);
    };
    const bool isRoseCedarStartup = !settings.useCustomColors && settings.theme == AppTheme::RoseCedar;
    const bool isJadeMistStartup = !settings.useCustomColors && settings.theme == AppTheme::JadeMist;
    const std::wstring themeAccent = BuildJsRgbaPrefix(startupTheme.waveformColor);
    const std::wstring themeAccentSoft = BuildJsRgbaPrefix(startupTheme.waveformSecondaryColor);
    const std::wstring themeMist = BuildJsRgbaPrefix(startupTheme.resultsSecondaryText);
    const std::wstring themeChrome = BuildJsRgbaPrefix(startupTheme.chromeBorder);
    const std::wstring themeTextGlow = BuildJsRgbaPrefix(startupTheme.interfaceText);
    const std::wstring themePrimaryRgb = BuildCssRgbTriplet(startupTheme.waveformColor);
    const std::wstring themeSecondaryRgb = BuildCssRgbTriplet(startupTheme.waveformSecondaryColor);
    const std::wstring themedParticleChance = std::to_wstring(themeAccents.themedParticleChance);
    const std::wstring tokenMainBg = BuildCssRgba(startupTheme.mainWindowBG, 1.0f);
    const std::wstring tokenCardBg = BuildCssRgba(startupTheme.dialogBG, 0.58f);
    const std::wstring tokenBorder = BuildCssRgba(startupTheme.chromeBorder, 0.34f);
    const std::wstring tokenBorderSoft = BuildCssRgba(startupTheme.waveformPanelBorder, 0.22f);
    const std::wstring tokenText = BuildCssRgba(startupTheme.interfaceText, 0.96f);
    const std::wstring tokenTextMuted = BuildCssRgba(startupTheme.resultsSecondaryText, 0.72f);
    const std::wstring tokenWave = BuildCssRgba(startupTheme.waveformColor, 1.0f);
    const std::wstring tokenWaveSoft = BuildCssRgba(startupTheme.waveformSecondaryColor, 1.0f);
    const std::wstring tokenCardShadow = BuildCssRgba(
        useLightStartupChrome
            ? mixColor(startupTheme.chromeBorder, startupTheme.mainWindowBG, 0.34f)
            : ImVec4(0.0f, 0.0f, 0.0f, 1.0f),
        useLightStartupChrome ? 0.22f : 0.52f);
    const std::wstring tokenCardShadowSoft = BuildCssRgba(
        useLightStartupChrome
            ? mixColor(startupTheme.chromeBorder, startupTheme.dialogBG, 0.38f)
            : ImVec4(0.0f, 0.0f, 0.0f, 1.0f),
        useLightStartupChrome ? 0.14f : 0.24f);
    const std::wstring tokenCardSheen = BuildCssRgba(
        mixColor(startupTheme.interfaceText, startupTheme.dialogBG, 0.76f),
        useLightStartupChrome ? 0.10f : 0.04f);
    const std::wstring tokenAvatarHalo = BuildCssRgba(
        isRoseCedarStartup
            ? mixColor(startupTheme.controlAccent, startupTheme.waveformColor, 0.62f)
            : (isJadeMistStartup
                ? mixColor(startupTheme.controlAccent, startupTheme.waveformColor, 0.56f)
                : mixColor(startupTheme.controlAccent, startupTheme.waveformColor, 0.48f)),
        useLightStartupChrome ? (isJadeMistStartup ? 0.26f : 0.24f) : 0.16f);
    const std::wstring tokenStatusText = BuildCssRgba(
        isRoseCedarStartup
            ? mixColor(startupTheme.interfaceText, startupTheme.controlAccent, 0.18f)
            : (isJadeMistStartup
                ? mixColor(startupTheme.interfaceText, startupTheme.controlAccent, 0.24f)
                : mixColor(startupTheme.interfaceText, startupTheme.resultsSecondaryText, 0.52f)),
        useLightStartupChrome ? 0.88f : 0.78f);
    const std::wstring tokenDetailText = BuildCssRgba(
        isRoseCedarStartup
            ? mixColor(startupTheme.resultsSecondaryText, startupTheme.controlAccent, 0.20f)
            : (isJadeMistStartup
                ? mixColor(startupTheme.resultsSecondaryText, startupTheme.controlAccent, 0.28f)
                : mixColor(startupTheme.resultsSecondaryText, startupTheme.chromeBorder, 0.18f)),
        useLightStartupChrome ? 0.78f : 0.48f);
    const std::wstring tokenProgressTrack = BuildCssRgba(
        isRoseCedarStartup
            ? mixColor(startupTheme.chromeBorder, startupTheme.controlAccent, 0.18f)
            : (isJadeMistStartup
                ? mixColor(startupTheme.chromeBorder, startupTheme.controlAccent, 0.30f)
                : mixColor(startupTheme.chromeBorder, startupTheme.dialogPanelBG, 0.20f)),
        useLightStartupChrome ? 0.40f : 0.22f);
    const std::wstring tokenProgressCore = BuildCssRgba(
        isRoseCedarStartup
            ? mixColor(startupTheme.controlAccent, startupTheme.waveformColor, 0.56f)
            : (isJadeMistStartup
                ? mixColor(startupTheme.controlAccent, startupTheme.waveformColor, 0.68f)
                : mixColor(startupTheme.controlAccent, startupTheme.interfaceText, useLightStartupChrome ? 0.18f : 0.34f)),
        useLightStartupChrome ? 0.94f : 0.66f);
    const std::wstring tokenCanvasWash = BuildCssRgba(
        isRoseCedarStartup
            ? mixColor(startupTheme.mainWindowBG, startupTheme.controlAccent, 0.18f)
            : (isJadeMistStartup
                ? mixColor(startupTheme.mainWindowBG, startupTheme.controlAccent, 0.22f)
                : mixColor(startupTheme.mainWindowBG, startupTheme.dialogBG, 0.38f)),
        useLightStartupChrome ? 0.12f : 0.16f);
    const std::wstring tokenCardTop = BuildCssRgba(
        useLightStartupChrome
            ? (isRoseCedarStartup
                ? mixColor(startupTheme.dialogBG, startupTheme.controlAccent, 0.26f)
                : (isJadeMistStartup
                    ? mixColor(startupTheme.dialogBG, startupTheme.controlAccent, 0.22f)
                    : mixColor(startupTheme.dialogBG, startupTheme.controlAccent, 0.14f)))
            : mixColor(startupTheme.dialogBG, startupTheme.interfaceText, 0.05f),
        1.0f);
    const std::wstring tokenCardBottom = BuildCssRgba(
        useLightStartupChrome
            ? (isRoseCedarStartup
                ? mixColor(startupTheme.mainWindowBG, startupTheme.toolbarAccent, 0.24f)
                : (isJadeMistStartup
                    ? mixColor(startupTheme.mainWindowBG, startupTheme.resultsSecondaryText, 0.18f)
                    : mixColor(startupTheme.mainWindowBG, startupTheme.toolbarAccent, 0.12f)))
            : mixColor(startupTheme.mainWindowBG, startupTheme.controlAccent, 0.04f),
        1.0f);
    const std::wstring tokenBackdropCore = BuildCssRgba(
        useLightStartupChrome
            ? (isRoseCedarStartup
                ? mixColor(startupTheme.resultsBG, startupTheme.controlAccent, 0.18f)
                : (isJadeMistStartup
                    ? mixColor(startupTheme.resultsBG, startupTheme.controlAccent, 0.22f)
                    : mixColor(startupTheme.resultsBG, startupTheme.controlAccent, 0.10f)))
            : startupTheme.resultsBG,
        useLightStartupChrome ? 0.76f : 0.56f);
    const std::wstring tokenBackdropAccentA = BuildCssRgba(
        isRoseCedarStartup
            ? mixColor(startupTheme.waveformColor, startupTheme.controlAccent, 0.58f)
            : (isJadeMistStartup
                ? mixColor(startupTheme.waveformColor, startupTheme.controlAccent, 0.70f)
                : mixColor(startupTheme.waveformColor, startupTheme.controlAccent, 0.35f)),
        useLightStartupChrome ? (isJadeMistStartup ? 0.34f : 0.30f) : 0.14f);
    const std::wstring tokenBackdropAccentB = BuildCssRgba(
        isRoseCedarStartup
            ? mixColor(startupTheme.toolbarAccent, startupTheme.controlAccent, 0.54f)
            : (isJadeMistStartup
                ? mixColor(startupTheme.resultsSecondaryText, startupTheme.controlAccent, 0.46f)
                : mixColor(startupTheme.toolbarAccent, startupTheme.resultsSecondaryText, 0.42f)),
        useLightStartupChrome ? (isJadeMistStartup ? 0.26f : 0.24f) : 0.10f);
    const std::wstring tokenBrandWord = BuildCssRgba(
        useLightStartupChrome
            ? (isRoseCedarStartup
                ? mixColor(startupTheme.interfaceText, startupTheme.controlAccent, 0.26f)
                : (isJadeMistStartup
                    ? mixColor(startupTheme.interfaceText, startupTheme.controlAccent, 0.34f)
                    : mixColor(startupTheme.interfaceText, startupTheme.controlAccent, 0.18f)))
            : startupTheme.interfaceText,
        0.96f);
    const std::wstring tokenCardBloomA = BuildCssRgba(
        isRoseCedarStartup
            ? mixColor(startupTheme.controlAccent, startupTheme.waveformColor, 0.62f)
            : (isJadeMistStartup
                ? mixColor(startupTheme.controlAccent, startupTheme.waveformColor, 0.76f)
                : mixColor(startupTheme.controlAccent, startupTheme.waveformColor, 0.40f)),
        useLightStartupChrome ? (isJadeMistStartup ? 0.22f : 0.18f) : 0.08f);
    const std::wstring tokenCardBloomB = BuildCssRgba(
        isRoseCedarStartup
            ? mixColor(startupTheme.toolbarAccent, startupTheme.controlAccent, 0.56f)
            : (isJadeMistStartup
                ? mixColor(startupTheme.resultsSecondaryText, startupTheme.controlAccent, 0.50f)
                : mixColor(startupTheme.resultsSecondaryText, startupTheme.waveformColor, 0.30f)),
        useLightStartupChrome ? (isJadeMistStartup ? 0.18f : 0.14f) : 0.06f);
    const std::string displayNameUtf8 = settings.userName.empty() ? std::string(L(Splash_DefaultUser)) : settings.userName;    const std::string startupAvatarDataUri = BuildLocalDiceBearAvatarDataUri(displayNameUtf8, settings.hasCustomAvatar, settings.diceBearAvatarIndex);
    std::wstring html;

    // ---- Part 1: head + CSS ----
    html += LR"HTML_L1(
<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8" />
  <meta name="viewport" content="width=device-width, initial-scale=1.0" />
  <title>Harken Search</title>
  <style>
    :root {
      --font-sans: "Inter", "Noto Sans SC", "Noto Sans CJK SC", "Segoe UI Variable", "Segoe UI", system-ui, sans-serif;
      --bg-dark: #070c14;
      --text-primary: #ffffff;
      --text-secondary: rgba(255, 255, 255, 0.65);
      --theme-primary-rgb: 59, 130, 246;
      --theme-secondary-rgb: 148, 163, 184;
      --primary-glow: rgb(var(--theme-primary-rgb));
      --secondary-glow: rgb(var(--theme-secondary-rgb));
    }
    html, body {
      margin: 0; padding: 0; width: 100%; height: 100%;
      background: var(--bg-dark);
      font-family: var(--font-sans);
      color: var(--text-primary);
      overflow: hidden;
      -webkit-font-smoothing: antialiased;
      user-select: none; -webkit-user-select: none;
      -webkit-user-drag: none;
      cursor: default;
    }
    .holographic-container { position: absolute; inset: 0; z-index: 0; overflow: hidden; }
    .aurora-backdrop {
      position: absolute; inset: -50%; width: 200%; height: 200%;
      background:
        radial-gradient(circle at 50% 50%, rgba(15, 23, 42, 0.4), transparent 60%),
        radial-gradient(circle at 20% 80%, rgba(var(--theme-primary-rgb), 0.12), transparent 50%),
        radial-gradient(circle at 80% 20%, rgba(var(--theme-secondary-rgb), 0.08), transparent 50%);
      animation: auroraShift 30s linear infinite;
      filter: blur(80px); z-index: 0;
    }
    @keyframes auroraShift { 0% { transform: rotate(0deg); } 100% { transform: rotate(360deg); } }
    .holographic-canvas { position: absolute; inset: 0; z-index: 1; mix-blend-mode: screen; }
    .noise-overlay {
      position: absolute; inset: 0; z-index: 2; opacity: 0.4; pointer-events: none;
      background: url("data:image/svg+xml;base64,PHN2ZyB4bWxucz0iaHR0cDovL3d3dy53My5vcmcvMjAwMC9zdmciIHdpZHRoPSIyMDAiIGhlaWdodD0iMjAwIj48ZmlsdGVyIGlkPSJub2lzZSI+PGZlVHVyYnVsZW5jZSB0eXBlPSJmcmFjdGFsTm9pc2UiIGJhc2VGcmVxdWVuY3k9IjAuNjUiIG51bU9jdGF2ZXM9IjMiIHN0aXRjaFRpbGVzPSJzdGl0Y2giLz48L2ZpbHRlcj48cmVjdCB3aWR0aD0iMTAwJSIgaGVpZ2h0PSIxMDAlIiBmaWx0ZXI9InVybCgjbm9pc2UpIiBvcGFjaXR5PSIwLjAzIi8+PC9zdmc+");
    }
    body::before {
      content: "";
      position: fixed;
      inset: 0;
      z-index: 60;
      pointer-events: none;
      background:
        radial-gradient(circle at 66% 76%, rgba(18, 30, 46, 0.42), transparent 52%),
        var(--bg-dark);
      animation: entryVeilOut 620ms cubic-bezier(.16,1,.3,1) 80ms forwards;
    }
    .loading-overlay {
      position: absolute; inset: 0; z-index: 10;
      display: flex; flex-direction: column;
      justify-content: center; align-items: center;
      animation: overlayIn 220ms ease-out both;
      transition: opacity 320ms cubic-bezier(0.22, 0.0, 0.12, 1), transform 320ms cubic-bezier(0.22, 0.0, 0.12, 1);
      will-change: opacity, transform;
    }
    .holographic-container {
      transition: opacity 460ms cubic-bezier(0.22, 0.0, 0.12, 1), transform 460ms cubic-bezier(0.22, 0.0, 0.12, 1);
      will-change: opacity, transform;
    }
    .glass-card {
      position: relative;
      width: 380px;
      background: rgba(10, 16, 24, 0.78);
      backdrop-filter: blur(24px) saturate(110%);
      -webkit-backdrop-filter: blur(24px) saturate(110%);
      border-radius: 32px;
      padding: 48px 36px;
      box-shadow: 0 25px 50px -12px rgba(0, 0, 0, 0.6), inset 0 0 0 1px rgba(255, 255, 255, 0.08);
      display: flex; flex-direction: column; align-items: center; gap: 24px;
      overflow: hidden;
      animation: cardIn 420ms ease-out both;
      transition: opacity 180ms cubic-bezier(0.19, 1, 0.22, 1), transform 180ms cubic-bezier(0.19, 1, 0.22, 1);
      will-change: opacity, transform;
    }
    .glass-card::before {
      content: ""; position: absolute; inset: 0;
      border-radius: 32px; padding: 1px;
      background: linear-gradient(135deg, rgba(var(--theme-primary-rgb), 0.42), rgba(255,255,255,0.06), rgba(var(--theme-secondary-rgb), 0.28), rgba(var(--theme-primary-rgb), 0.14));
      -webkit-mask: linear-gradient(#fff 0 0) content-box, linear-gradient(#fff 0 0);
      -webkit-mask-composite: xor; mask-composite: exclude;
      pointer-events: none; opacity: 0.8;
    }
    @keyframes entryVeilOut { to { opacity: 0; } }
    @keyframes overlayIn { from { opacity: 0; transform: translateY(4px); } to { opacity: 1; transform: translateY(0); } }
    @keyframes cardIn { from { opacity: 0; transform: scale(0.965) translateY(14px); } to { opacity: 1; transform: scale(1) translateY(0); } }
    .spinner-container { width: 70px; height: 70px; position: relative; }
    .spinner-ring {
      position: absolute; inset: 0;
      border: 3px solid transparent;
      border-top-color: var(--primary-glow);
      border-right-color: var(--secondary-glow);
      border-radius: 50%;
      animation: spin 1.5s linear infinite;
      filter: drop-shadow(0 0 8px rgba(80, 120, 160, 0.32));
    }
    .spinner-ring-inner {
      position: absolute; inset: 8px;
      border: 2px solid transparent;
      border-bottom-color: rgba(82, 148, 168, 0.46);
      border-left-color: rgba(132, 148, 166, 0.24);
      border-radius: 50%;
      animation: spin 2s linear infinite reverse;
    }
    @keyframes spin { to { transform: rotate(360deg); } }
    .status-text {
      font-size: 0.95rem; font-weight: 400;
      color: rgba(255,255,255,0.85);
      text-align: center; min-height: 1.2em;
    }
    .progress-detail {
      font-size: 0.8rem; color: rgba(255,255,255,0.4);
      text-align: center;
    }
    .progress-bar-track {
      width: 240px; height: 3px;
      background: rgba(255,255,255,0.1);
      border-radius: 4px; overflow: hidden;
    }
    .progress-bar-fill {
      height: 100%; width: 0%;
      background: linear-gradient(90deg, var(--secondary-glow), var(--primary-glow), #06b6d4);
      border-radius: 4px;
      transition: width 0.3s ease-out;
      box-shadow: 0 0 10px var(--primary-glow);
    }
    .version-footer {
      position: absolute; bottom: 30px; left: 0; right: 0;
      text-align: center;
      font-size: 0.75rem; color: rgba(255,255,255,0.25);
      letter-spacing: 0.1em; z-index: 10;
      transition: opacity 220ms cubic-bezier(0.19, 1, 0.22, 1), transform 220ms cubic-bezier(0.19, 1, 0.22, 1);
      will-change: opacity, transform;
    }
    body.phase-out .loading-overlay {
      opacity: 0;
      transform: translateY(-8px) scale(0.992);
    }
    body.phase-out .glass-card {
      opacity: 0;
      transform: translateY(-18px) scale(0.978);
    }
    body.phase-out .version-footer {
      opacity: 0;
      transform: translateY(6px);
    }
    body.phase-out .holographic-container {
      opacity: 0.58;
      transform: scale(1.006);
    }
    @media (prefers-reduced-motion: reduce) {
      *, *::before, *::after {
        animation-duration: 0.01ms !important;
        animation-iteration-count: 1 !important;
        transition-duration: 0.01ms !important;
      }
    }
  </style>
</head>
)HTML_L1";
    html += L"<style>\n";
    html += L":root{--bg-dark:" + tokenMainBg +
            L";--startup-card:" + tokenCardBg +
            L";--startup-border:" + tokenBorder +
            L";--startup-border-soft:" + tokenBorderSoft +
            L";--startup-text:" + tokenText +
            L";--startup-muted:" + tokenTextMuted +
            L";--startup-wave:" + tokenWave +
            L";--startup-wave-soft:" + tokenWaveSoft + L";}\n";
    html += L"html,body{background:var(--bg-dark);color:var(--startup-text);}\n";
    html += L".aurora-backdrop{background:radial-gradient(circle at 50% 50%," + tokenBackdropCore + L",transparent 60%),radial-gradient(circle at 20% 80%," + tokenBackdropAccentA + L",transparent 50%),radial-gradient(circle at 80% 20%," + tokenBackdropAccentB + L",transparent 50%);}\n";
    html += L".glass-card{background:linear-gradient(180deg,var(--startup-card)," + BuildCssRgba(startupTheme.mainWindowBG, 0.50f) + L");box-shadow:0 24px 56px -18px " + tokenCardShadow + L",inset 0 0 0 1px var(--startup-border-soft);}\n";
    html += L".glass-card::before{background:linear-gradient(135deg," + BuildCssRgba(startupTheme.waveformColor, 0.42f) + L"," + tokenCardSheen + L",var(--startup-border)," + BuildCssRgba(startupTheme.controlAccent, 0.18f) + L");}\n";
    html += L".status-text{color:var(--startup-text);}.progress-detail,.version-footer{color:var(--startup-muted);}\n";
    html += L".progress-bar-track{background:" + BuildCssRgba(startupTheme.chromeBorder, 0.24f) + L";}.progress-bar-fill{background:linear-gradient(90deg,var(--startup-wave-soft),var(--startup-wave)," + BuildCssRgba(startupTheme.controlAccent, 1.0f) + L");box-shadow:0 0 10px " + BuildCssRgba(startupTheme.waveformColor, 0.38f) + L";}\n";
    html += L".spinner-ring{border-top-color:var(--startup-wave);border-right-color:var(--startup-wave-soft);filter:drop-shadow(0 0 8px " + BuildCssRgba(startupTheme.waveformColor, 0.32f) + L");}.spinner-ring-inner{border-bottom-color:" + BuildCssRgba(startupTheme.controlAccent, 0.46f) + L";border-left-color:" + BuildCssRgba(startupTheme.resultsSecondaryText, 0.24f) + L";}\n";
    html += L".brand-word{position:fixed;top:40px;left:48px;z-index:90;color:" + tokenBrandWord + L";font-family:\"Segoe UI Variable\",\"Segoe UI\",Arial,sans-serif;font-size:clamp(22px,1.8vw,32px);font-weight:700;line-height:1;letter-spacing:-0.01em;opacity:.82;isolation:isolate;mix-blend-mode:normal;filter:none;text-shadow:0 1px 0 rgba(255,255,255,.08),0 10px 26px rgba(0,0,0,.18);}\n";
    html += L".holographic-canvas{opacity:.22!important;}.aurora-backdrop{opacity:.74!important;filter:blur(92px)!important;}.noise-overlay{opacity:.18!important;}\n";
    html += L".loading-overlay{z-index:10;}.glass-card{width:min(312px,calc(100vw - 48px));min-height:300px;padding:30px 28px 28px;border-radius:20px;background:linear-gradient(180deg," + tokenCardTop + L"," + tokenCardBottom + L");border:1px solid " + BuildCssRgba(startupTheme.waveformPanelBorder, useLightStartupChrome ? 0.28f : 0.18f) + L";box-shadow:0 14px 38px " + tokenCardShadowSoft + L",inset 0 0 0 1px " + tokenCardSheen + L";backdrop-filter:blur(12px) saturate(108%);-webkit-backdrop-filter:blur(12px) saturate(108%);gap:0;justify-content:center;}\n";
    html += L".glass-card::before{border-radius:20px;padding:0;background:linear-gradient(135deg," + tokenCardSheen + L",transparent 46%);-webkit-mask:none;mask:none;opacity:1;}\n";
    html += L".glass-card::after{content:\"\";position:absolute;inset:auto -10% 42% -10%;height:54%;pointer-events:none;background:radial-gradient(circle at 24% 28%," + tokenCardBloomA + L",transparent 58%),radial-gradient(circle at 76% 6%," + tokenCardBloomB + L",transparent 54%);filter:blur(14px);opacity:.98;}\n";
    html += L".avatar-wrap{position:relative;width:86px;height:86px;margin:0 0 22px;display:grid;place-items:center;}.avatar{position:relative;z-index:1;width:74px;height:74px;border-radius:50%;display:grid;place-items:center;color:rgba(255,255,255,.94);font-family:var(--font-sans);font-size:23px;font-weight:680;letter-spacing:-.03em;background:radial-gradient(circle at 34% 24%,rgba(255,255,255,.18),transparent 30%),linear-gradient(145deg,hsl(var(--avatar-hue),44%,43%),hsl(calc(var(--avatar-hue) + 22),38%,30%));box-shadow:inset 0 0 0 1px rgba(255,255,255,.11),0 6px 16px " + tokenCardShadowSoft + L";animation:avatarPulse 2.8s ease-in-out infinite;}.avatar::after{content:\"\";position:absolute;inset:-7px;border-radius:50%;background:" + tokenAvatarHalo + L";z-index:-1;animation:pulseHalo 2.8s ease-in-out infinite;}.avatar-img{position:absolute;inset:0;width:100%;height:100%;border-radius:50%;object-fit:cover;image-rendering:-webkit-optimize-contrast;display:none;background:rgba(255,255,255,.04);}.avatar.has-image{background:rgba(255,255,255,.045);}.avatar.has-image .avatar-img{display:block;}.avatar.has-image #loadingAvatarInitials{display:none;}\n";
    html += L".spinner-container{display:none!important;}.status-text{position:relative;z-index:1;margin:0;color:" + tokenStatusText + L";font-size:12.5px;line-height:1.45;font-weight:420;min-height:1.45em;animation:statusPulse 2.2s ease-in-out infinite;}.progress-detail{position:relative;z-index:1;margin-top:8px;font-size:11px;color:" + tokenDetailText + L";}.progress-bar-track{position:relative;z-index:1;width:104px;height:1px;margin-top:16px;background:" + tokenProgressTrack + L";border-radius:999px;}.progress-bar-fill{height:100%;background:linear-gradient(90deg," + BuildCssRgba(startupTheme.waveformColor, 0.22f) + L"," + tokenProgressCore + L"," + BuildCssRgba(startupTheme.waveformColor, 0.22f) + L");box-shadow:none;transition:width .28s ease-out;}\n";
    html += L".version-footer{display:none!important;}@keyframes avatarPulse{0%,100%{transform:scale(1);}50%{transform:scale(1.018);}}@keyframes pulseHalo{0%,100%{opacity:.18;transform:scale(.96);}50%{opacity:.34;transform:scale(1.05);}}@keyframes statusPulse{0%,100%{opacity:.68;}50%{opacity:.92;}}\n";
    AppendStartupBrandLogoCss(html, useLightStartupChrome);
    html += L"</style>\n";

    // ---- Part 2: body HTML ----
    html += LR"HTML_L2(
<body>
  <div class="holographic-container">
    <div class="aurora-backdrop"></div>
    <canvas id="fx" class="holographic-canvas"></canvas>
    <div class="noise-overlay"></div>
  </div>

  <div class="brand-word">Harken Search</div>
  <div class="loading-overlay">
    <div class="glass-card">
      <div class="avatar-wrap" aria-hidden="true">
        <div class="avatar" id="loadingAvatarBubble">
          <img class="avatar-img" id="loadingAvatarImage" alt="" />
          <span id="loadingAvatarInitials">AR</span>
        </div>
      </div>
      <div class="status-text" id="statusText">Preparing your sound workspace...</div>
      <div class="progress-bar-track">
        <div class="progress-bar-fill" id="progressBar"></div>
      </div>
      <div class="progress-detail" id="progressDetail">0 / 0</div>
    </div>
  </div>

  <div class="version-footer">Engine v3.4 // Cobalt Build</div>

)HTML_L2";

    // Part 3: JavaScript vortex particles and progress API.
    html += L"<style>:root{--theme-primary-rgb:" + themePrimaryRgb +
        L";--theme-secondary-rgb:" + themeSecondaryRgb +
        L";--primary-glow:rgb(var(--theme-primary-rgb));--secondary-glow:rgb(var(--theme-secondary-rgb));}</style>\n";
    html += LR"HTML_L3A(
  <script>
)HTML_L3A";
    html += L"    const themeAccent = \"" + themeAccent + L"\";\n";
    html += L"    const themeAccentSoft = \"" + themeAccentSoft + L"\";\n";
    html += L"    const themeMist = \"" + themeMist + L"\";\n";
    html += L"    const themeChrome = \"" + themeChrome + L"\";\n";
    html += L"    const themeTextGlow = \"" + themeTextGlow + L"\";\n";
    html += L"    const canvasWash = \"" + tokenCanvasWash + L"\";\n";
    html += L"    const themedParticleChance = " + themedParticleChance + L";\n";
    html += L"    const loadingDisplayName = \"" + Utf8ToWide(JsonEscape(displayNameUtf8)) + L"\";\n";
    html += L"    const loadingAvatarDataUri = \"" + Utf8ToWide(JsonEscape(startupAvatarDataUri)) + L"\";\n";
    html += LR"HTML_L3(
    const canvas = document.getElementById("fx");
    const ctx = canvas.getContext("2d");
    let width = canvas.width = window.innerWidth;
    let height = canvas.height = window.innerHeight;
    const cx = () => width * 0.54;
    const cy = () => height * 0.5;

    const palettes = [
      themeTextGlow,
      themeAccent,
      themeChrome,
      themeAccentSoft,
      themeMist,
      themeChrome
    ];
    let time = 0;
    let particles = [];
    let waves = [];

    // Swirl particles for loading screen energy.
    class Particle {
      constructor() {
        this.isThemeColor = Math.random() * 100 < themedParticleChance;
        const spawnX = Math.pow(Math.random(), 0.84);
        this.x = width * (0.16 + spawnX * 0.70);
        this.y = Math.random() * height;
        this.size = Math.random() * 2.5 + 0.5 + (this.isThemeColor ? 0.7 : 0.0);
        this.speedX = (Math.random() - 0.5) * 1.0;
        this.speedY = (Math.random() - 0.5) * 1.0;
        this.color = this.isThemeColor
          ? (Math.random() < 0.68 ? themeAccent : themeAccentSoft)
          : palettes[Math.floor(Math.random() * palettes.length)];
        this.alpha = Math.random();
        this.flashSpeed = (this.isThemeColor ? 0.024 : 0.03) + Math.random() * (this.isThemeColor ? 0.05 : 0.08);
        this.phase = Math.random() * Math.PI * 2;
      }
      update(t) {
        // 流体力场运动
        const angle = Math.sin(this.x * 0.002 + t * 0.5) + Math.cos(this.y * 0.002 + t * 0.5);
        this.x += Math.cos(angle) * 1.5 + this.speedX;
        this.y += Math.sin(angle) * 1.5 + this.speedY;

        // Swirl attraction toward center.
        const dx = cx() - this.x, dy = cy() - this.y;
        const dist = Math.sqrt(dx * dx + dy * dy);
        const force = Math.min(0.3, 800 / (dist * dist + 100));
        const theta = Math.atan2(dy, dx);
        // Centripetal + tangential force.
        this.x += Math.cos(theta) * force + Math.cos(theta + 1.57) * force * 1.5;
        this.y += Math.sin(theta) * force + Math.sin(theta + 1.57) * force * 1.5;

        // Faster pulse cadence.
        this.alpha = 0.3 + 0.7 * Math.abs(Math.sin(t * 8 * this.flashSpeed + this.phase));

        // Extra glow near center.
        this.glowSize = this.size + Math.max(0, (200 - dist) / 50);

        // 边界环绕
        if (this.x < 0) this.x = width;
        if (this.x > width) this.x = 0;
        if (this.y < 0) this.y = height;
        if (this.y > height) this.y = 0;
        this.draw(dist);
      }
      draw(dist) {
        const a = Math.max(0, Math.min(1, this.alpha));
        // Increase glow near center
        if (dist < 250) {
          const glowAlpha = (1 - dist / 250) * 0.3 * a;
          ctx.beginPath();
          ctx.arc(this.x, this.y, this.glowSize * 3, 0, Math.PI * 2);
          ctx.fillStyle = this.color + glowAlpha + ")";
          ctx.fill();
        }
        ctx.beginPath();
        ctx.arc(this.x, this.y, this.glowSize, 0, Math.PI * 2);
        ctx.fillStyle = this.color + a + ")";
        ctx.fill();
      }
    }

    class Wave {
      constructor(y, color, alpha = 0.15, glow = 20, lineWidth = 2) {
        this.yOffset = y;
        this.amplitude = 50 + Math.random() * 100;
        this.wavelength = 0.002 + Math.random() * 0.003;
        this.speed = 0.01 + Math.random() * 0.02;
        this.color = color;
        this.alpha = alpha;
        this.glow = glow;
        this.lineWidth = lineWidth;
        this.phase = Math.random() * Math.PI * 2;
      }
      update(t) {
        ctx.beginPath();
        ctx.moveTo(0, height / 2);
        for (let x = 0; x < width; x += 10) {
          const y = this.yOffset + Math.sin(x * this.wavelength + t * this.speed + this.phase) * this.amplitude * Math.sin(t * 0.5);
          const noise = Math.cos(x * 0.01 - t) * 20;
          ctx.lineTo(x, y + noise);
        }
        ctx.strokeStyle = this.color + this.alpha + ")";
        ctx.lineWidth = this.lineWidth;
        ctx.shadowBlur = this.glow;
        ctx.shadowColor = this.color + "1)";
        ctx.stroke();
        ctx.shadowBlur = 0;
      }
    }

    function init() {
      particles = [];
      waves = [];
      // 比登录页更多粒子(250)
      for (let i = 0; i < 250; i++) particles.push(new Particle());
      waves.push(new Wave(height * 0.46, palettes[0], 0.12, 18, 1.9));
      waves.push(new Wave(height * 0.50, themeAccent, 0.24, 32, 2.8));
      waves.push(new Wave(height * 0.56, themeAccentSoft, 0.18, 26, 2.3));
      waves.push(new Wave(height * 0.42, palettes[5], 0.10, 14, 1.7));
    }
    function animate() {
      ctx.fillStyle = canvasWash;
      ctx.fillRect(0, 0, width, height);
      time += 0.026;
      waves.forEach(w => w.update(time));
      particles.forEach(p => p.update(time));
      requestAnimationFrame(animate);
    }
    function handleResize() {
      width = canvas.width = window.innerWidth;
      height = canvas.height = window.innerHeight;
      init();
    }
    window.addEventListener("resize", handleResize);
    init();
    animate();

    let exitStarted = false;

    const loadingNameChars = Array.from(loadingDisplayName || "Harken Search User");
    document.getElementById("loadingAvatarInitials").textContent =
      (loadingNameChars.length > 0 ? loadingNameChars.slice(0, 2).join("") : "AU").toUpperCase();
    const loadingAvatarHash = loadingNameChars.reduce((acc, ch) => ((acc * 31) + ch.codePointAt(0)) >>> 0, 2166136261);
    document.documentElement.style.setProperty("--avatar-hue", String(196 + (loadingAvatarHash % 34)));
    if (loadingAvatarDataUri) {
      const loadingAvatarImage = document.getElementById("loadingAvatarImage");
      loadingAvatarImage.src = loadingAvatarDataUri;
      document.getElementById("loadingAvatarBubble").classList.add("has-image");
    }

    // 进度更新 API —— C++ 通过 ExecuteScript 调用
    // loaded/total：进度数值，message：阶段说明
    window.updateProgress = function(loaded, total, message) {
      const pct = total > 0 ? (loaded / total * 100) : 0;
      document.getElementById("progressBar").style.width = pct + "%";
      document.getElementById("progressDetail").textContent = loaded + " / " + total;
      if (typeof message === "string" && message.length > 0) {
        document.getElementById("statusText").textContent = message;
      }
    };
    window.startExitTransition = function(message) {
      if (exitStarted) return;
      exitStarted = true;
      if (typeof message === "string" && message.length > 0) {
        document.getElementById("statusText").textContent = message;
      }
      document.body.classList.add("phase-out");
    };
  </script>
</body>
</html>
)HTML_L3";
    // Fill splash template defaults from the localization ledger.
    {
        const std::wstring subW = std::wstring(Lw(Splash_Subtitle));
        size_t pos = 0;
        while ((pos = html.find(SUB_W_T, pos)) != std::wstring::npos) {
            html.replace(pos, sizeof(SUB_W_T) / sizeof(wchar_t) - 1, subW);
            pos += subW.size();
        }
        const std::wstring langW = (g_currentLanguage == AppLanguage::Japanese)   ? L"ja"
                                     : (g_currentLanguage == AppLanguage::English) ? L"en" : L"zh-CN";
        {
            size_t lq = 0;
            const std::wstring needle = L"<html lang=\"en\">";
            const std::wstring repl = L"<html lang=\"" + langW + L"\">";
            while ((lq = html.find(needle, lq)) != std::wstring::npos) {
                html.replace(lq, needle.size(), repl);
                lq += repl.size();
            }
        }
        const std::wstring ariaW = L"aria-label=\"" + Utf8ToWide(L(Splash_AriaContinue)) + L"\"";
        pos = 0;
        while ((pos = html.find(L"aria-label=\"Continue\"", pos)) != std::wstring::npos) {
            html.replace(pos, sizeof(L"aria-label=\"Continue\"") / sizeof(wchar_t) - 1, ariaW);
            pos += ariaW.size();
        }
    }
    return html;
}

static std::string JsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 16);
    for (char c : s) {
        switch (c) {
        case '\\': out += "\\\\"; break;
        case '"':  out += "\\\""; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if ((unsigned char)c < 0x20) {
                char buf[8];
                snprintf(buf, sizeof(buf), "\\u%04x", (unsigned char)c);
                out += buf;
            } else {
                out.push_back(c);
            }
        }
    }
    return out;
}

static std::string UrlDecodeSimple(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    auto hexVal = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
        if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
        return -1;
    };
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (c == '+') {
            out.push_back(' ');
        } else if (c == '%' && i + 2 < s.size()) {
            int hi = hexVal(s[i + 1]);
            int lo = hexVal(s[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back((char)((hi << 4) | lo));
                i += 2;
            } else {
                out.push_back(c);
            }
        } else {
            out.push_back(c);
        }
    }
    return out;
}

static std::string UrlEncodeSimple(const std::string& s) {
    static const char hex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size() * 3);
    for (unsigned char c : s) {
        if ((c >= 'a' && c <= 'z') ||
            (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back((char)c);
        } else if (c == ' ') {
            out.push_back('+');
        } else {
            out.push_back('%');
            out.push_back(hex[(c >> 4) & 0xF]);
            out.push_back(hex[c & 0xF]);
        }
    }
    return out;
}

// Base64 (encode) - for sending binary blobs over JSON-RPC

static std::string Base64Encode(const uint8_t* data, size_t len) {
    static const char* kTbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    size_t i = 0;
    while (i + 2 < len) {
        uint32_t v = (uint32_t(data[i]) << 16) | (uint32_t(data[i + 1]) << 8) | uint32_t(data[i + 2]);
        out.push_back(kTbl[(v >> 18) & 63]);
        out.push_back(kTbl[(v >> 12) & 63]);
        out.push_back(kTbl[(v >> 6) & 63]);
        out.push_back(kTbl[v & 63]);
        i += 3;
    }
    if (i < len) {
        uint32_t v = uint32_t(data[i]) << 16;
        out.push_back(kTbl[(v >> 18) & 63]);
        if (i + 1 < len) {
            v |= uint32_t(data[i + 1]) << 8;
            out.push_back(kTbl[(v >> 12) & 63]);
            out.push_back(kTbl[(v >> 6) & 63]);
            out.push_back('=');
        } else {
            out.push_back(kTbl[(v >> 12) & 63]);
            out.push_back('=');
            out.push_back('=');
        }
    }
    return out;
}

// Spectrogram generation for the WebUI preview.
// CPU path: generate RGBA tiles for the current WebUI view without OpenGL.

struct SpectrogramDecodeCache {
    std::string filePathUtf8;
    uint64_t framesRead = 0;
    std::vector<float> samples; // mono 44.1k
};
static SpectrogramDecodeCache g_specDecodeCache;
static std::unordered_map<int, std::vector<float>> g_specWindowCache; // fftSize -> window

static void FFT_CooleyTukey(std::vector<std::complex<float>>& x) {
    size_t N = x.size();
    if (N <= 1) return;
    for (size_t i = 1, j = 0; i < N; i++) {
        size_t bit = N >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(x[i], x[j]);
    }
    for (size_t len = 2; len <= N; len <<= 1) {
        float angle = -2.0f * 3.14159265358979f / (float)len;
        std::complex<float> wlen(cosf(angle), sinf(angle));
        for (size_t i = 0; i < N; i += len) {
            std::complex<float> w(1.0f, 0.0f);
            for (size_t j = 0; j < len / 2; j++) {
                std::complex<float> u = x[i + j];
                std::complex<float> t = w * x[i + j + len / 2];
                x[i + j] = u + t;
                x[i + j + len / 2] = u - t;
                w *= wlen;
            }
        }
    }
}

static ImU32 SpectrogramColor_Heat(float magnitude, float freqRatio) {
    (void)freqRatio;
    float v = std::clamp(magnitude, 0.0f, 1.0f);
    struct ColorStop { float pos; float r, g, b; };
    static const ColorStop stops[] = {
        { 0.00f, 0.00f, 0.00f, 0.05f },
        { 0.05f, 0.05f, 0.02f, 0.15f },
        { 0.15f, 0.10f, 0.10f, 0.35f },
        { 0.25f, 0.15f, 0.25f, 0.55f },
        { 0.35f, 0.20f, 0.45f, 0.65f },
        { 0.45f, 0.30f, 0.60f, 0.55f },
        { 0.55f, 0.55f, 0.70f, 0.30f },
        { 0.65f, 0.80f, 0.75f, 0.15f },
        { 0.75f, 0.95f, 0.65f, 0.10f },
        { 0.85f, 1.00f, 0.45f, 0.10f },
        { 0.92f, 1.00f, 0.30f, 0.15f },
        { 1.00f, 1.00f, 0.95f, 0.90f },
    };
    const int numStops = sizeof(stops) / sizeof(stops[0]);
    int i = 0;
    for (; i < numStops - 1; i++) {
        if (v < stops[i + 1].pos) break;
    }
    if (i >= numStops - 1) i = numStops - 2;
    float t = (v - stops[i].pos) / (stops[i + 1].pos - stops[i].pos);
    t = std::clamp(t, 0.0f, 1.0f);
    t = t * t * (3.0f - 2.0f * t);
    float r = stops[i].r + t * (stops[i + 1].r - stops[i].r);
    float g = stops[i].g + t * (stops[i + 1].g - stops[i].g);
    float b = stops[i].b + t * (stops[i + 1].b - stops[i].b);
    return IM_COL32((int)(r * 255), (int)(g * 255), (int)(b * 255), 255);
}

static const std::vector<float>& GetBlackmanHarrisWindow(int fftSize) {
    auto it = g_specWindowCache.find(fftSize);
    if (it != g_specWindowCache.end()) return it->second;
    std::vector<float> window(fftSize);
    const float a0 = 0.35875f, a1 = 0.48829f, a2 = 0.14128f, a3 = 0.01168f;
    const float pi = 3.14159265358979f;
    for (int i = 0; i < fftSize; i++) {
        float n = (float)i / (fftSize - 1);
        window[i] = a0 - a1 * cosf(2.0f * pi * n) + a2 * cosf(4.0f * pi * n) - a3 * cosf(6.0f * pi * n);
    }
    auto [insIt, _] = g_specWindowCache.emplace(fftSize, std::move(window));
    return insIt->second;
}

static bool DecodeMonoSamples44k1Cached(const std::string& filePathUtf8, std::vector<float>& outSamples, uint64_t& outFramesRead) {
    if (g_specDecodeCache.filePathUtf8 == filePathUtf8 && !g_specDecodeCache.samples.empty() && g_specDecodeCache.framesRead > 0) {
        outSamples = g_specDecodeCache.samples;
        outFramesRead = g_specDecodeCache.framesRead;
        return true;
    }
    Platform::AudioDataSource::Decoder decoder;
    const auto initResult = Platform::AudioDataSource::InitializeDecoderFromFileWithFormat(
        decoder, filePathUtf8, 1, 44100);
    if (!initResult.Succeeded()) {
        return false;
    }
    const auto lengthResult = Platform::AudioDataSource::QueryLength(
        Platform::AudioDataSource::BorrowDataSource(decoder));
    const auto totalFrames = lengthResult.frames;
    const int maxFrames = 44100 * 60 * 10; // 10 minutes
    const uint64_t framesToRead = std::min<uint64_t>(static_cast<uint64_t>(maxFrames), static_cast<uint64_t>(totalFrames));
    std::vector<float> samples((size_t)framesToRead);
    const auto readResult = Platform::AudioDataSource::ReadPcmFrames(
        Platform::AudioDataSource::BorrowDataSource(decoder),
        samples.data(),
        framesToRead);
    const auto framesRead = readResult.framesRead;
    Platform::AudioDataSource::UninitializeDecoder(decoder);
    samples.resize((size_t)framesRead);
    if (framesRead == 0) return false;
    g_specDecodeCache.filePathUtf8 = filePathUtf8;
    g_specDecodeCache.framesRead = framesRead;
    g_specDecodeCache.samples = samples;
    outSamples = std::move(samples);
    outFramesRead = framesRead;
    return true;
}

static bool GenerateSpectrogramTileRGBA(const std::vector<float>& samples, uint64_t framesRead, int fftSize,
                                        float startNorm, float endNorm, int outW, int outH,
                                        std::vector<uint8_t>& outPixels) {
    if (fftSize < 512) fftSize = 512;
    if (fftSize > 16384) fftSize = 16384;
    if (framesRead < (uint64_t)fftSize) return false;
    if (outW < 8 || outH < 8) return false;

    startNorm = std::clamp(startNorm, 0.0f, 1.0f);
    endNorm = std::clamp(endNorm, 0.0f, 1.0f);
    if (endNorm < startNorm) std::swap(startNorm, endNorm);
    if ((endNorm - startNorm) < 0.00001f) {
        endNorm = std::min(1.0f, startNorm + 0.00001f);
    }

    outPixels.assign((size_t)outW * (size_t)outH * 4, 0);

    const auto& window = GetBlackmanHarrisWindow(fftSize);
    const int numBins = fftSize / 2;
    const float minFreqHz = 20.0f;
    const float maxFreqHz = 22050.0f;
    const float freqPerBin = 44100.0f / (float)fftSize;

    // Reuse FFT buffer to avoid per-column allocations.
    std::vector<std::complex<float>> fftData((size_t)fftSize);

    for (int col = 0; col < outW; col++) {
        float u = (outW <= 1) ? 0.0f : ((float)col / (float)(outW - 1));
        float globalT = startNorm + u * (endNorm - startNorm);
        globalT = std::clamp(globalT, 0.0f, 1.0f);
        int samplePos = ComputeSpectrogramWindowStartForNorm(globalT, framesRead, fftSize);

        for (int i = 0; i < fftSize; i++) {
            int idx = samplePos + i;
            float sample = (idx >= 0 && idx < (int)framesRead) ? samples[(size_t)idx] : 0.0f;
            fftData[(size_t)i] = std::complex<float>(sample * window[(size_t)i], 0.0f);
        }

        FFT_CooleyTukey(fftData);

        for (int row = 0; row < outH; row++) {
            float t = (outH <= 1) ? 0.0f : ((float)row / (float)(outH - 1));
            float freqHz = minFreqHz * powf(maxFreqHz / minFreqHz, t);
            float binF = freqHz / freqPerBin;
            int bin0 = (int)binF;
            int bin1 = bin0 + 1;
            float frac = binF - (float)bin0;
            bin0 = std::clamp(bin0, 1, numBins - 1);
            bin1 = std::clamp(bin1, 1, numBins - 1);

            float mag0 = std::abs(fftData[(size_t)bin0]);
            float mag1 = std::abs(fftData[(size_t)bin1]);
            float magnitude = (mag0 * (1.0f - frac) + mag1 * frac) / (float)fftSize;

            float db = 20.0f * log10f(magnitude + 1e-12f);
            float normalized = std::clamp((db + 90.0f) / 90.0f, 0.0f, 1.0f);
            normalized = powf(normalized, 0.6f);

            ImU32 color = SpectrogramColor_Heat(normalized, t);
            int texRow = outH - 1 - row; // low freq bottom
            size_t pixelIdx = ((size_t)texRow * (size_t)outW + (size_t)col) * 4;
            outPixels[pixelIdx + 0] = (color >> 0) & 0xFF;
            outPixels[pixelIdx + 1] = (color >> 8) & 0xFF;
            outPixels[pixelIdx + 2] = (color >> 16) & 0xFF;
            outPixels[pixelIdx + 3] = 255;
        }
    }
    return true;
}

static std::unordered_map<std::string, std::string> ParseQueryLikePayload(const std::string& payload) {
    std::unordered_map<std::string, std::string> kv;
    size_t start = 0;
    while (start <= payload.size()) {
        size_t amp = payload.find('&', start);
        std::string pair = payload.substr(start, amp == std::string::npos ? std::string::npos : (amp - start));
        if (!pair.empty()) {
            size_t eq = pair.find('=');
            if (eq == std::string::npos) {
                kv[UrlDecodeSimple(pair)] = "";
            } else {
                kv[UrlDecodeSimple(pair.substr(0, eq))] = UrlDecodeSimple(pair.substr(eq + 1));
            }
        }
        if (amp == std::string::npos) break;
        start = amp + 1;
    }
    return kv;
}

static std::vector<std::string> SplitByPipe(const std::string& s, size_t maxParts = 0) {
    std::vector<std::string> parts;
    size_t start = 0;
    while (start <= s.size()) {
        if (maxParts > 0 && parts.size() + 1 >= maxParts) {
            parts.push_back(s.substr(start));
            break;
        }
        size_t p = s.find('|', start);
        if (p == std::string::npos) {
            parts.push_back(s.substr(start));
            break;
        }
        parts.push_back(s.substr(start, p - start));
        start = p + 1;
    }
    return parts;
}

static std::string BuildSearchItemsJson(const std::vector<AudioRecord>& rows) {
    std::string out = "[";
    for (size_t i = 0; i < rows.size(); ++i) {
        const auto& r = rows[i];
        if (i) out += ",";
        out += "{";
        out += "\"id\":\"" + std::to_string(r.id) + "\",";
        out += "\"name\":\"" + JsonEscape(r.fileName) + "\",";
        out += "\"duration\":\"" + std::to_string((int)std::round(r.durationSeconds)) + "\",";
        out += "\"format\":\"" + JsonEscape(!r.format.empty() ? r.format : r.extension) + "\",";
        out += "\"size\":\"" + std::to_string((unsigned long long)r.fileSize) + "\",";
        out += "\"sampleRate\":\"" + std::to_string(r.sampleRate) + "\",";
        out += "\"category\":\"" + JsonEscape(r.category) + "\",";
        out += "\"description\":\"" + JsonEscape(r.description) + "\"";
        out += "}";
    }
    out += "]";
    return out;
}

static std::string BuildItemDetailJson(const AudioRecord& r) {
    std::string out = "{";
    out += "\"id\":\"" + std::to_string(r.id) + "\",";
    out += "\"name\":\"" + JsonEscape(r.fileName) + "\",";
    out += "\"duration\":\"" + std::to_string((int)std::round(r.durationSeconds)) + "\",";
    out += "\"format\":\"" + JsonEscape(!r.format.empty() ? r.format : r.extension) + "\",";
    out += "\"size\":\"" + std::to_string((unsigned long long)r.fileSize) + "\",";
    out += "\"sampleRate\":\"" + std::to_string(r.sampleRate) + "\",";
    out += "\"category\":\"" + JsonEscape(r.category) + "\",";
    out += "\"description\":\"" + JsonEscape(r.description) + "\",";
    out += "\"artist\":\"" + JsonEscape(r.artist) + "\",";
    out += "\"album\":\"" + JsonEscape(r.album) + "\"";
    out += "}";
    return out;
}

static bool JsonReadStringField(const std::string& json, const std::string& key, std::string& out) {
    std::string needle = "\"" + key + "\"";
    size_t pos = json.find(needle);
    if (pos == std::string::npos) return false;
    pos += needle.size();
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t' || json[pos] == '\n' || json[pos] == '\r' || json[pos] == ':')) pos++;
    if (pos >= json.size() || json[pos] != '"') return false;
    ++pos;
    out.clear();
    while (pos < json.size() && json[pos] != '"') {
        if (json[pos] == '\\' && pos + 1 < json.size()) {
            ++pos;
            out.push_back(json[pos]);
        } else {
            out.push_back(json[pos]);
        }
        ++pos;
    }
    return true;
}

static int JsonReadIntField(const std::string& json, const std::string& key, int defVal) {
    std::string needle = "\"" + key + "\"";
    size_t pos = json.find(needle);
    if (pos == std::string::npos) return defVal;
    pos += needle.size();
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t' || json[pos] == '\n' || json[pos] == '\r' || json[pos] == ':')) pos++;
    if (pos >= json.size()) return defVal;
    size_t start = pos;
    if (json[pos] == '-') pos++;
    while (pos < json.size() && json[pos] >= '0' && json[pos] <= '9') pos++;
    if (start == pos) return defVal;
    try {
        return std::stoi(json.substr(start, pos - start));
    } catch (...) {
        return defVal;
    }
}

static double JsonReadDoubleField(const std::string& json, const std::string& key, double defVal) {
    std::string needle = "\"" + key + "\"";
    size_t pos = json.find(needle);
    if (pos == std::string::npos) return defVal;
    pos += needle.size();
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t' || json[pos] == '\n' || json[pos] == '\r' || json[pos] == ':')) pos++;
    if (pos >= json.size()) return defVal;
    size_t start = pos;
    if (json[pos] == '-') pos++;
    while (pos < json.size() && (json[pos] >= '0' && json[pos] <= '9')) pos++;
    if (pos < json.size() && json[pos] == '.') { pos++; while (pos < json.size() && json[pos] >= '0' && json[pos] <= '9') pos++; }
    if (start == pos) return defVal;
    try { return std::stod(json.substr(start, pos - start)); } catch (...) { return defVal; }
}

static bool JsonReadBoolField(const std::string& json, const std::string& key, bool defVal, bool* present = nullptr) {
    if (present) *present = false;
    std::string needle = "\"" + key + "\"";
    size_t pos = json.find(needle);
    if (pos == std::string::npos) return defVal;
    pos += needle.size();
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t' || json[pos] == '\n' || json[pos] == '\r' || json[pos] == ':')) pos++;
    if (pos >= json.size()) return defVal;
    if (json.compare(pos, 4, "true") == 0) { if (present) *present = true; return true; }
    if (json.compare(pos, 5, "false") == 0) { if (present) *present = true; return false; }
    return defVal;
}

static std::string JsonReadObjectField(const std::string& json, const std::string& key) {
    std::string needle = "\"" + key + "\"";
    size_t pos = json.find(needle);
    if (pos == std::string::npos) return "{}";
    pos += needle.size();
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t' || json[pos] == '\n' || json[pos] == '\r' || json[pos] == ':')) pos++;
    if (pos >= json.size() || json[pos] != '{') return "{}";
    size_t start = pos;
    int depth = 0;
    bool inString = false;
    for (; pos < json.size(); ++pos) {
        char c = json[pos];
        if (inString) {
            if (c == '\\') ++pos;
            else if (c == '"') inString = false;
            continue;
        }
        if (c == '"') inString = true;
        else if (c == '{') depth++;
        else if (c == '}') {
            depth--;
            if (depth == 0) return json.substr(start, pos - start + 1);
        }
    }
    return "{}";
}

static std::string ToLowerAscii(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}

static std::vector<int> ParseIdsFromParams(const std::string& paramsJson) {
    std::vector<int> ids;
    int singleId = JsonReadIntField(paramsJson, "id", -1);
    if (singleId > 0) ids.push_back(singleId);
    std::string idsRaw;
    JsonReadStringField(paramsJson, "ids", idsRaw);
    if (!idsRaw.empty()) {
        std::stringstream ss(idsRaw);
        std::string tok;
        while (std::getline(ss, tok, ',')) {
            try {
                int id = std::stoi(tok);
                if (id > 0) ids.push_back(id);
            } catch (...) {}
        }
    }
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    return ids;
}

static std::vector<AudioRecord> ResolveRecordsByIds(AppState& state, const std::vector<int>& ids) {
    std::vector<AudioRecord> rows;
    rows.reserve(ids.size());
    for (int id : ids) {
        AudioRecord r = state.database.getFileById((int64_t)id);
        if (r.id > 0 && !r.filePath.empty()) {
            rows.push_back(std::move(r));
        }
    }
    return rows;
}

static int64_t FileTimeToUnixSeconds(const std::filesystem::file_time_type& ftime) {
    using namespace std::chrono;
    auto sctp = time_point_cast<seconds>(ftime - std::filesystem::file_time_type::clock::now() + system_clock::now());
    return (int64_t)sctp.time_since_epoch().count();
}

static std::string BuildWebAudioRecordJson(const AudioRecord& r) {
    std::string out = "{";
    out += "\"id\":" + std::to_string(r.id) + ",";
    out += "\"fileName\":\"" + JsonEscape(r.fileName) + "\",";
    out += "\"filePath\":\"" + JsonEscape(r.filePath) + "\",";
    out += "\"folderPath\":\"" + JsonEscape(r.folderPath) + "\",";
    out += "\"duration\":" + std::to_string(r.durationSeconds) + ",";
    out += "\"durationSeconds\":" + std::to_string(r.durationSeconds) + ",";
    out += "\"format\":\"" + JsonEscape(!r.format.empty() ? r.format : r.extension) + "\",";
    out += "\"extension\":\"" + JsonEscape(r.extension) + "\",";
    out += "\"sampleRate\":" + std::to_string(r.sampleRate) + ",";
    out += "\"channels\":" + std::to_string(r.channels) + ",";
    out += "\"bitDepth\":0,";
    out += "\"fileSize\":" + std::to_string((unsigned long long)r.fileSize) + ",";
    out += "\"description\":\"" + JsonEscape(r.description) + "\",";
    out += "\"keywords\":\"" + JsonEscape(r.keywords) + "\",";
    out += "\"category\":\"" + JsonEscape(r.category) + "\",";
    out += "\"artist\":\"" + JsonEscape(r.artist) + "\",";
    out += "\"album\":\"" + JsonEscape(r.album) + "\",";
    out += "\"loudnessLUFS\":0,";
    out += "\"rmsDb\":0,";
    out += "\"truePeakDbTP\":0,";
    out += "\"hasCover\":" + std::string(r.hasCover ? "true" : "false") + ",";
    // Expose coverKey so WebUI can reuse cached thumbnails across rows without per-row RPC.
    // Prefer embedded cover key, fallback to folder cover key.
    {
        const std::string& key = !r.embeddedCoverKey.empty() ? r.embeddedCoverKey : r.folderCoverKey;
        out += "\"coverKey\":\"" + JsonEscape(key) + "\",";
    }
    out += "\"coverRev\":\"" + JsonEscape(
        std::to_string((long long)r.embeddedCoverKeyMtime) + "|" +
        std::to_string((long long)r.folderCoverSrcMtime) + "|" +
        std::to_string((long long)r.fileMtime) + "|" +
        (r.coverSuppressed ? "1" : "0")
    ) + "\",";
    out += "\"dateAdded\":\"" + JsonEscape(FormatDateTimeLocal(r.dateAdded)) + "\"";
    out += "}";
    return out;
}

static std::string BuildWebFileInfoJson(const AudioRecord& r) {
    std::string out = "{";
    out += "\"id\":" + std::to_string(r.id) + ",";
    out += "\"fileName\":\"" + JsonEscape(r.fileName) + "\",";
    out += "\"filePath\":\"" + JsonEscape(r.filePath) + "\",";
    out += "\"folderPath\":\"" + JsonEscape(r.folderPath) + "\",";
    out += "\"duration\":" + std::to_string(r.durationSeconds) + ",";
    out += "\"format\":\"" + JsonEscape(!r.format.empty() ? r.format : r.extension) + "\",";
    out += "\"sampleRate\":" + std::to_string(r.sampleRate) + ",";
    out += "\"channels\":" + std::to_string(r.channels) + ",";
    out += "\"bitDepth\":0,";
    out += "\"fileSize\":" + std::to_string((unsigned long long)r.fileSize) + ",";
    out += "\"description\":\"" + JsonEscape(r.description) + "\",";
    out += "\"keywords\":\"" + JsonEscape(r.keywords) + "\",";
    out += "\"category\":\"" + JsonEscape(r.category) + "\",";
    out += "\"artist\":\"" + JsonEscape(r.artist) + "\",";
    out += "\"album\":\"" + JsonEscape(r.album) + "\",";
    out += "\"hasCover\":" + std::string(r.hasCover ? "true" : "false") + ",";
    out += "\"dateAdded\":\"" + JsonEscape(FormatDateTimeLocal(r.dateAdded)) + "\",";
    out += "\"fileMtime\":\"" + JsonEscape(FormatDateTimeLocal(r.fileMtime)) + "\",";
    out += "\"lastIndexed\":\"" + JsonEscape(FormatDateTimeLocal(r.lastIndexed)) + "\",";
    out += "\"totalFrames\":" + std::to_string((unsigned long long)r.totalFrames);
    out += "}";
    return out;
}

static std::string BuildWebAudioRecordArrayJson(const std::vector<AudioRecord>& rows) {
    std::string out = "[";
    for (size_t i = 0; i < rows.size(); ++i) {
        if (i) out += ",";
        out += BuildWebAudioRecordJson(rows[i]);
    }
    out += "]";
    return out;
}

static SortColumn ParseSortColumnForWeb(const std::string& sortColumn) {
    const std::string s = ToLowerAscii(sortColumn);
    if (s == "filename" || s == "name") return SortColumn::FileName;
    if (s == "duration") return SortColumn::Duration;
    if (s == "format") return SortColumn::Format;
    if (s == "size") return SortColumn::Size;
    if (s == "samplerate") return SortColumn::SampleRate;
    if (s == "description") return SortColumn::Description;
    if (s == "artist") return SortColumn::Artist;
    if (s == "album") return SortColumn::Album;
    if (s == "bpm") return SortColumn::BPM;
    if (s == "musicalkey" || s == "key") return SortColumn::MusicalKey;
    if (s == "mood") return SortColumn::Mood;
    if (s == "dateadded") return SortColumn::DateAdded;
    if (s == "loudness" || s == "loudnesslufs") return SortColumn::Loudness;
    if (s == "rms" || s == "rmsdb") return SortColumn::RMS;
    if (s == "truepeak" || s == "truepeakdbtp") return SortColumn::TruePeak;
    if (s == "cover") return SortColumn::Cover;
    return SortColumn::FileName;
}

static void SortAudioRecordsForWeb(std::vector<AudioRecord>& rows, SortColumn column, bool asc) {
    auto cmp = [column, asc](const AudioRecord& a, const AudioRecord& b) {
        auto cmpStr = [asc](const std::string& x, const std::string& y) {
            return asc ? x < y : x > y;
        };
        auto cmpNum = [asc](auto x, auto y) {
            return asc ? x < y : x > y;
        };
        switch (column) {
        case SortColumn::FileName: return cmpStr(a.fileName, b.fileName);
        case SortColumn::Duration: return cmpNum(a.durationSeconds, b.durationSeconds);
        case SortColumn::Format: return cmpStr(!a.format.empty() ? a.format : a.extension, !b.format.empty() ? b.format : b.extension);
        case SortColumn::Size: return cmpNum(a.fileSize, b.fileSize);
        case SortColumn::SampleRate: return cmpNum(a.sampleRate, b.sampleRate);
        case SortColumn::Description: return cmpStr(a.description, b.description);
        case SortColumn::Channels: return cmpNum(a.channels, b.channels);
        case SortColumn::Category: return cmpStr(a.category, b.category);
        case SortColumn::Artist: return cmpStr(a.artist, b.artist);
        case SortColumn::Album: return cmpStr(a.album, b.album);
        case SortColumn::BPM: {
            auto parseBpm = [](const std::string& s) {
                if (s.empty()) return -1.0;
                try { return std::stod(s); } catch (...) { return -1.0; }
            };
            return cmpNum(parseBpm(a.getMeta("bpm")), parseBpm(b.getMeta("bpm")));
        }
        case SortColumn::MusicalKey: return cmpStr(a.getMeta("musical_key"), b.getMeta("musical_key"));
        case SortColumn::Mood: return cmpStr(a.getMeta("mood"), b.getMeta("mood"));
        case SortColumn::DateAdded: return cmpNum(a.dateAdded, b.dateAdded);
        case SortColumn::Cover: return cmpNum((int)a.hasCover, (int)b.hasCover);
        default: return cmpStr(a.fileName, b.fileName);
        }
    };
    std::sort(rows.begin(), rows.end(), cmp);
}

static std::string BuildFolderTreeJson(const FolderInfo& n) {
    std::string out = "{";
    out += "\"path\":\"" + JsonEscape(n.path) + "\",";
    out += "\"name\":\"" + JsonEscape(n.name) + "\",";
    out += "\"fileCount\":" + std::to_string(n.fileCount) + ",";
    out += "\"subfolders\":[";
    for (size_t i = 0; i < n.subfolders.size(); ++i) {
        if (i) out += ",";
        out += BuildFolderTreeJson(n.subfolders[i]);
    }
    out += "]}";
    return out;
}

// WebView2 登录界面 —— 登录成功后保留 WebView2 实例

static void DisableWebShortcutsForStartupUi(ICoreWebView2Controller* controller, ICoreWebView2* webview);
static bool RunWebView2LoginScreen(HWND hwnd, const AppSettings& appSettings,
                                    ComPtr<ICoreWebView2Controller>* outController,
                                    ComPtr<ICoreWebView2>* outWebview,
                                    HMODULE* outWvLoader) {
    if (!hwnd) return false;
    if (!Platform::WindowIsVisible(hwnd)) {
        (void)Platform::WindowShow(hwnd, SW_SHOWMAXIMIZED);
        (void)Platform::WindowSetForeground(hwnd);
        UpdateWindow(hwnd);
    }
    HMODULE wvLoader = LoadLibraryW(L"WebView2Loader.dll");
    if (!wvLoader) return false;
    auto createEnv = (CreateWebView2EnvironmentFn)GetProcAddress(wvLoader, "CreateCoreWebView2EnvironmentWithOptions");
    if (!createEnv) {
        FreeLibrary(wvLoader);
        return false;
    }

    struct State {
        bool finished = false;
        bool accepted = false;
        bool failed = false;
        ComPtr<ICoreWebView2Controller> controller;
        ComPtr<ICoreWebView2> webview;
    } s;

    std::wstring userDataPath;
    if (Platform::HarkenUserData::PreviewIsolationActive()) {
        userDataPath = Utf8ToWide(Platform::HarkenUserData::PreviewIsolationRootUtf8()) + L"\\profile\\WebView2";
    } else {
        userDataPath = Utf8ToWide(Platform::GetTempDirectory()) + L"Aris_WebView2";
    }

    HRESULT hr = createEnv(
        nullptr, userDataPath.c_str(), nullptr,
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [&s, &appSettings, hwnd](HRESULT result, ICoreWebView2Environment* env) -> HRESULT {
                if (FAILED(result) || !env) {
                    s.failed = true;
                    s.finished = true;
                    return S_OK;
                }
                return env->CreateCoreWebView2Controller(
                    hwnd,
                    Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                        [&s, &appSettings, hwnd](HRESULT r, ICoreWebView2Controller* controller) -> HRESULT {
                            if (FAILED(r) || !controller) {
                                s.failed = true;
                                s.finished = true;
                                return S_OK;
                            }
                            s.controller = controller;
                            s.controller->get_CoreWebView2(&s.webview);
                            if (!s.webview) {
                                s.failed = true;
                                s.finished = true;
                                return S_OK;
                            }
                            // 设置 WebView2 默认背景为深色，避免页面切换白屏闪烁
                            {
                                Microsoft::WRL::ComPtr<ICoreWebView2Controller2> ctrl2;
                                if (SUCCEEDED(s.controller.As(&ctrl2)) && ctrl2) {
                                    COREWEBVIEW2_COLOR bg = {255, 5, 5, 5};
                                    ctrl2->put_DefaultBackgroundColor(bg);
                                }
                            }
                            RECT rc = {};
                            GetClientRect(hwnd, &rc);
                            s.controller->put_Bounds(rc);
                            s.controller->put_IsVisible(TRUE);

// 禁止外部拖放到 WebView2（避免文件/文件夹被当作 URL 导航）.
                            {
                                Microsoft::WRL::ComPtr<ICoreWebView2Controller4> ctrl4;
                                if (SUCCEEDED(s.controller.As(&ctrl4)) && ctrl4) {
                                    ctrl4->put_AllowExternalDrop(FALSE);
                                }
                            }

                            // 拦截所有非初始页的导航（阻止 file:// 等意外跳转）
                            s.webview->add_NavigationStarting(
                                Callback<ICoreWebView2NavigationStartingEventHandler>(
                                    [](ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs* args) -> HRESULT {
                                        if (!args) return S_OK;
                                        LPWSTR uri = nullptr;
                                        args->get_Uri(&uri);
                                        if (uri) {
                                            std::wstring u(uri);
                                            Platform::FreeComTaskMemory(uri);
// 只允许 about:blank 和 NavigateToString 生成的 blob/data URL.
                                            if (u.find(L"about:blank") == std::wstring::npos &&
                                                u.find(L"data:") != 0 &&
                                                u.find(L"blob:") != 0) {
                                                args->put_Cancel(TRUE);
                                            }
                                        }
                                        return S_OK;
                                    }).Get(),
                                nullptr);

                            // 禁用右键菜单、DevTools、状态栏等，让 WebView2 表现得像原生 App
                            ICoreWebView2Settings* settings = nullptr;
                            if (SUCCEEDED(s.webview->get_Settings(&settings)) && settings) {
                                settings->put_AreDefaultContextMenusEnabled(FALSE);
                                settings->put_AreDevToolsEnabled(FALSE);
                                settings->put_IsStatusBarEnabled(FALSE);
                                settings->put_IsZoomControlEnabled(FALSE);
                                settings->Release();
                            }
                            DisableWebShortcutsForStartupUi(s.controller.Get(), s.webview.Get());

                            s.webview->add_WebMessageReceived(
                                Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                                    [&s](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                                        if (!args) return S_OK;
                                        std::wstring wmsg;
                                        LPWSTR msg = nullptr;
                                        if (SUCCEEDED(args->TryGetWebMessageAsString(&msg)) && msg) {
                                            wmsg = msg;
                                            Platform::FreeComTaskMemory(msg);
                                        } else {
                                            LPWSTR jsonW = nullptr;
                                            if (SUCCEEDED(args->get_WebMessageAsJson(&jsonW)) && jsonW) {
                                                wmsg = jsonW;
                                                Platform::FreeComTaskMemory(jsonW);
                                            }
                                        }
                                        if (wmsg.empty()) return S_OK;

                                        std::string msgUtf8 = WideToUtf8(wmsg);
                                        if (msgUtf8 == "welcome_continue" ||
                                            msgUtf8 == "\"welcome_continue\"") {
                                            s.accepted = true;
                                            s.finished = true;
                                            return S_OK;
                                        }
                                        std::string type;
                                        std::string account;
                                        std::string password;

                                        // 兼容旧协议：enter_studio:<account>
                                        const std::string legacyPrefix = "enter_studio:";
                                        if (msgUtf8.rfind(legacyPrefix, 0) == 0) {
                                            type = "login";
                                            account = msgUtf8.substr(legacyPrefix.size());
                                        } else if (!msgUtf8.empty() && msgUtf8[0] == '{') {
                                            JsonReadStringField(msgUtf8, "type", type);
                                            JsonReadStringField(msgUtf8, "account", account);
                                            JsonReadStringField(msgUtf8, "password", password);
                                        }

                                        if (type == "login") {
                                            // 简易验证桩：账号/密码非空即可通过
                                            if (account.empty()) {
                                                // 将错误返回给前端，不结束等待
                                                if (s.webview) {
                                                    std::string err = "账号不能为空";
                                                    std::string payload = "window.onLoginResult(false, \"";
                                                    payload += JsonEscape(err);
                                                    payload += "\");";
                                                    s.webview->ExecuteScript(Utf8ToWide(payload).c_str(), nullptr);
                                                }
                                                return S_OK;
                                            }
                                            if (password.empty()) {
                                                if (s.webview) {
                                                    std::string err = "密码不能为空";
                                                    std::string payload = "window.onLoginResult(false, \"";
                                                    payload += JsonEscape(err);
                                                    payload += "\");";
                                                    s.webview->ExecuteScript(Utf8ToWide(payload).c_str(), nullptr);
                                                }
                                                return S_OK;
                                            }

                                            s.accepted = true;
                                            s.finished = true;

                                            if (s.webview) {
                                                std::string ok = "登录成功，正在加载...";
                                                std::string payload = "window.onLoginResult(true, \"";
                                                payload += JsonEscape(ok);
                                                payload += "\");";
                                                s.webview->ExecuteScript(Utf8ToWide(payload).c_str(), nullptr);
                                            }
                                        }
                                        return S_OK;
                                    }).Get(),
                                nullptr);

                            std::wstring html = BuildFlashUiLoginHtml(appSettings);
                            s.webview->NavigateToString(html.c_str());
                            return S_OK;
                        }).Get());
            }).Get());

    if (FAILED(hr)) {
        FreeLibrary(wvLoader);
        return false;
    }

    {
        while (!s.finished) {
            MSG msg = {};
            while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
                if (msg.message == WM_QUIT) {
                    s.finished = true;
                    s.accepted = false;
                    break;
                }
                TranslateMessage(&msg);
                DispatchMessage(&msg);
            }
            if (s.controller) {
                RECT rc = {};
                GetClientRect(hwnd, &rc);
                s.controller->put_Bounds(rc);
            }
            Platform::SleepMilliseconds(10);
        }
    }

    if (s.accepted && outController && outWebview && outWvLoader) {
        // Keep the WebView2 instance after login and pass it to the loading stage.
        *outController = s.controller;
        *outWebview = s.webview;
        *outWvLoader = wvLoader;
    } else {
        // Close WebView2 after a failed login; the caller does not retain it.
        if (s.controller) {
            s.controller->put_IsVisible(FALSE);
            s.controller->Close();
            s.controller.Reset();
            s.webview.Reset();
        }
        FreeLibrary(wvLoader);
        if (outWvLoader) *outWvLoader = nullptr;
    }

    return s.accepted;
}

static void DisableWebShortcutsForStartupUi(ICoreWebView2Controller* controller, ICoreWebView2* webview) {
    if (!webview) return;

    ICoreWebView2Settings* settings = nullptr;
    if (SUCCEEDED(webview->get_Settings(&settings)) && settings) {
        // Browser-level accelerators (Ctrl+R/F5/Ctrl+L/...) off.
        Microsoft::WRL::ComPtr<ICoreWebView2Settings3> settings3;
        if (SUCCEEDED(settings->QueryInterface(IID_PPV_ARGS(&settings3))) && settings3) {
            settings3->put_AreBrowserAcceleratorKeysEnabled(FALSE);
        }
        settings->Release();
    }

    if (controller) {
        controller->add_AcceleratorKeyPressed(
            Callback<ICoreWebView2AcceleratorKeyPressedEventHandler>(
                [](ICoreWebView2Controller*, ICoreWebView2AcceleratorKeyPressedEventArgs* args) -> HRESULT {
                    if (!args) return S_OK;
                    COREWEBVIEW2_KEY_EVENT_KIND kind = COREWEBVIEW2_KEY_EVENT_KIND_KEY_DOWN;
                    UINT key = 0;
                    if (FAILED(args->get_KeyEventKind(&kind)) || FAILED(args->get_VirtualKey(&key))) return S_OK;
                    if (kind != COREWEBVIEW2_KEY_EVENT_KIND_KEY_DOWN &&
                        kind != COREWEBVIEW2_KEY_EVENT_KIND_SYSTEM_KEY_DOWN) {
                        return S_OK;
                    }
                    if (key == VK_F7 || key == VK_F5 || key == VK_F12) {
                        args->put_Handled(TRUE);
                    }
                    return S_OK;
                }).Get(),
            nullptr);
    }

    webview->AddScriptToExecuteOnDocumentCreated(
        LR"(
(() => {
  const isFunctionKey = (k) => /^F\d{1,2}$/.test(k || "");
  const shouldBlock = (e) => !!(e.ctrlKey || e.metaKey || e.altKey || isFunctionKey(e.key));
  const handler = (e) => {
    if (shouldBlock(e)) {
      e.preventDefault();
      e.stopPropagation();
      if (typeof e.stopImmediatePropagation === 'function') e.stopImmediatePropagation();
      return false;
    }
  };
  window.addEventListener('keydown', handler, true);
  window.addEventListener('keypress', handler, true);
  window.addEventListener('keyup', handler, true);
})();
)",
        nullptr);
}

// Beta Splash Screen — borderless window with PNG artwork

static std::wstring BuildFlashUiWelcomeHtml(const AppSettings& settings, const std::string& userNameUtf8) {
    const StartupThemeAccents themeAccents = ResolveStartupThemeAccents(settings);
    const ThemeColorScheme& startupTheme = ThemeColors::current();
    const bool useLightStartupChrome = ThemeColors::isLightChromeTheme(startupTheme);
    auto mixColor = [](const ImVec4& a, const ImVec4& b, float t) -> ImVec4 {
        return ImVec4(
            a.x + (b.x - a.x) * t,
            a.y + (b.y - a.y) * t,
            a.z + (b.z - a.z) * t,
            a.w + (b.w - a.w) * t);
    };
    const bool isRoseCedarStartup = !settings.useCustomColors && settings.theme == AppTheme::RoseCedar;
    const bool isJadeMistStartup = !settings.useCustomColors && settings.theme == AppTheme::JadeMist;
    const std::wstring themeAccent = BuildJsRgbaPrefix(startupTheme.waveformColor);
    const std::wstring themeAccentSoft = BuildJsRgbaPrefix(startupTheme.waveformSecondaryColor);
    const std::wstring themeMist = BuildJsRgbaPrefix(startupTheme.resultsSecondaryText);
    const std::wstring themeChrome = BuildJsRgbaPrefix(startupTheme.chromeBorder);
    const std::wstring themeTextGlow = BuildJsRgbaPrefix(startupTheme.interfaceText);
    const std::wstring themedParticleChance = std::to_wstring(themeAccents.themedParticleChance);
    const std::wstring tokenMainBg = BuildCssRgba(startupTheme.mainWindowBG, 1.0f);
    const std::wstring tokenAirBg = BuildCssRgba(startupTheme.resultsBG, 0.56f);
    const std::wstring tokenCardBg = BuildCssRgba(startupTheme.dialogBG, 0.62f);
    const std::wstring tokenBorder = BuildCssRgba(startupTheme.waveformPanelBorder, 0.26f);
    const std::wstring tokenText = BuildCssRgba(startupTheme.interfaceText, 0.97f);
    const std::wstring tokenMuted = BuildCssRgba(startupTheme.resultsSecondaryText, 0.74f);
    const std::wstring tokenControl = BuildCssRgba(startupTheme.controlAccent, 1.0f);
    const std::wstring tokenNameText = BuildCssRgba(
        mixColor(startupTheme.interfaceText, startupTheme.controlAccent, useLightStartupChrome ? 0.24f : 0.38f),
        useLightStartupChrome ? 0.92f : 0.82f);
    const std::wstring tokenNameGlow = BuildCssRgba(
        isRoseCedarStartup
            ? mixColor(startupTheme.controlAccent, startupTheme.waveformColor, 0.60f)
            : (isJadeMistStartup
                ? mixColor(startupTheme.controlAccent, startupTheme.waveformColor, 0.70f)
                : mixColor(startupTheme.controlAccent, startupTheme.waveformColor, 0.46f)),
        useLightStartupChrome ? (isJadeMistStartup ? 0.24f : 0.22f) : 0.30f);
    const std::wstring tokenCardShadow = BuildCssRgba(
        useLightStartupChrome
            ? mixColor(startupTheme.chromeBorder, startupTheme.mainWindowBG, 0.34f)
            : ImVec4(0.0f, 0.0f, 0.0f, 1.0f),
        useLightStartupChrome ? 0.20f : 0.24f);
    const std::wstring tokenCardSheen = BuildCssRgba(
        mixColor(startupTheme.interfaceText, startupTheme.dialogBG, 0.76f),
        useLightStartupChrome ? 0.10f : 0.04f);
    const std::wstring tokenCanvasWash = BuildCssRgba(
        isRoseCedarStartup
            ? mixColor(startupTheme.mainWindowBG, startupTheme.controlAccent, 0.18f)
            : (isJadeMistStartup
                ? mixColor(startupTheme.mainWindowBG, startupTheme.controlAccent, 0.22f)
                : mixColor(startupTheme.mainWindowBG, startupTheme.dialogBG, 0.38f)),
        useLightStartupChrome ? 0.12f : 0.16f);
    const std::wstring tokenCardTop = BuildCssRgba(
        useLightStartupChrome
            ? (isRoseCedarStartup
                ? mixColor(startupTheme.dialogBG, startupTheme.controlAccent, 0.28f)
                : (isJadeMistStartup
                    ? mixColor(startupTheme.dialogBG, startupTheme.controlAccent, 0.20f)
                    : mixColor(startupTheme.dialogBG, startupTheme.controlAccent, 0.15f)))
            : mixColor(startupTheme.dialogBG, startupTheme.interfaceText, 0.05f),
        1.0f);
    const std::wstring tokenCardBottom = BuildCssRgba(
        useLightStartupChrome
            ? (isRoseCedarStartup
                ? mixColor(startupTheme.mainWindowBG, startupTheme.toolbarAccent, 0.26f)
                : (isJadeMistStartup
                    ? mixColor(startupTheme.mainWindowBG, startupTheme.resultsSecondaryText, 0.20f)
                    : mixColor(startupTheme.mainWindowBG, startupTheme.toolbarAccent, 0.13f)))
            : mixColor(startupTheme.mainWindowBG, startupTheme.controlAccent, 0.04f),
        1.0f);
    const std::wstring tokenBackdropCore = BuildCssRgba(
        useLightStartupChrome
            ? (isRoseCedarStartup
                ? mixColor(startupTheme.resultsBG, startupTheme.controlAccent, 0.18f)
                : (isJadeMistStartup
                    ? mixColor(startupTheme.resultsBG, startupTheme.controlAccent, 0.24f)
                    : mixColor(startupTheme.resultsBG, startupTheme.controlAccent, 0.10f)))
            : startupTheme.resultsBG,
        useLightStartupChrome ? 0.76f : 0.56f);
    const std::wstring tokenBackdropAccentA = BuildCssRgba(
        isRoseCedarStartup
            ? mixColor(startupTheme.waveformColor, startupTheme.controlAccent, 0.58f)
            : (isJadeMistStartup
                ? mixColor(startupTheme.waveformColor, startupTheme.controlAccent, 0.72f)
                : mixColor(startupTheme.waveformColor, startupTheme.controlAccent, 0.35f)),
        useLightStartupChrome ? (isJadeMistStartup ? 0.36f : 0.30f) : 0.16f);
    const std::wstring tokenBackdropAccentB = BuildCssRgba(
        isRoseCedarStartup
            ? mixColor(startupTheme.toolbarAccent, startupTheme.controlAccent, 0.54f)
            : (isJadeMistStartup
                ? mixColor(startupTheme.resultsSecondaryText, startupTheme.controlAccent, 0.50f)
                : mixColor(startupTheme.toolbarAccent, startupTheme.resultsSecondaryText, 0.42f)),
        useLightStartupChrome ? (isJadeMistStartup ? 0.28f : 0.24f) : 0.10f);
    const std::wstring tokenBrandWord = BuildCssRgba(
        useLightStartupChrome
            ? (isRoseCedarStartup
                ? mixColor(startupTheme.interfaceText, startupTheme.controlAccent, 0.26f)
                : (isJadeMistStartup
                    ? mixColor(startupTheme.interfaceText, startupTheme.controlAccent, 0.34f)
                    : mixColor(startupTheme.interfaceText, startupTheme.controlAccent, 0.18f)))
            : startupTheme.interfaceText,
        0.96f);
    const std::wstring tokenLeadText = BuildCssRgba(
        isRoseCedarStartup
            ? mixColor(startupTheme.interfaceText, startupTheme.controlAccent, 0.22f)
            : (isJadeMistStartup
                ? mixColor(startupTheme.interfaceText, startupTheme.controlAccent, 0.28f)
                : mixColor(startupTheme.interfaceText, startupTheme.controlAccent, useLightStartupChrome ? 0.10f : 0.02f)),
        useLightStartupChrome ? 0.94f : 0.90f);
    const std::wstring tokenStatusText = BuildCssRgba(
        isRoseCedarStartup
            ? mixColor(startupTheme.resultsSecondaryText, startupTheme.controlAccent, 0.22f)
            : (isJadeMistStartup
                ? mixColor(startupTheme.resultsSecondaryText, startupTheme.controlAccent, 0.32f)
                : mixColor(startupTheme.resultsSecondaryText, startupTheme.interfaceText, 0.24f)),
        useLightStartupChrome ? 0.90f : 0.76f);
    const std::wstring tokenCardBloomA = BuildCssRgba(
        isRoseCedarStartup
            ? mixColor(startupTheme.controlAccent, startupTheme.waveformColor, 0.62f)
            : (isJadeMistStartup
                ? mixColor(startupTheme.controlAccent, startupTheme.waveformColor, 0.78f)
                : mixColor(startupTheme.controlAccent, startupTheme.waveformColor, 0.42f)),
        useLightStartupChrome ? (isJadeMistStartup ? 0.24f : 0.18f) : 0.08f);
    const std::wstring tokenCardBloomB = BuildCssRgba(
        isRoseCedarStartup
            ? mixColor(startupTheme.toolbarAccent, startupTheme.controlAccent, 0.56f)
            : (isJadeMistStartup
                ? mixColor(startupTheme.resultsSecondaryText, startupTheme.controlAccent, 0.54f)
                : mixColor(startupTheme.resultsSecondaryText, startupTheme.waveformColor, 0.30f)),
        useLightStartupChrome ? (isJadeMistStartup ? 0.20f : 0.15f) : 0.06f);

    std::time_t now = std::time(nullptr);
    std::tm localTime = {};
#ifdef _WIN32
    localtime_s(&localTime, &now);
#else
    localTime = *std::localtime(&now);
#endif
    // 7-A slice-4: band-pure greeting + ledger phrase (三语随语言，
    // 旧 zh/en 二元分支从未支持日语=母病，Lead 判词入盘点)
    const WelcomeGreeting::Greeting greeting =
        WelcomeGreeting::ResolveGreeting(localTime.tm_hour, userNameUtf8);
    LStr greetKey;
    switch (greeting.band) {
        case WelcomeGreeting::Band::Morning:     greetKey = LStr::Greet_Morning; break;
        case WelcomeGreeting::Band::Noon:        greetKey = LStr::Greet_Noon; break;
        case WelcomeGreeting::Band::Afternoon:   greetKey = LStr::Greet_Afternoon; break;
        case WelcomeGreeting::Band::Evening:     greetKey = LStr::Greet_Evening; break;
        case WelcomeGreeting::Band::LateNight:   greetKey = LStr::Greet_LateNight; break;
        case WelcomeGreeting::Band::WelcomeBack: greetKey = LStr::Greet_WelcomeBack; break;
    }
    const std::string titleUtf8 = GetLocalizedString(greetKey);
    const std::string displayNameUtf8 = greeting.name.empty() ? std::string(L(Splash_DefaultUser)) : greeting.name;
    const std::string subtitleUtf8 = L(Splash_Subtitle);    const std::string startupAvatarDataUri = BuildLocalDiceBearAvatarDataUri(displayNameUtf8, settings.hasCustomAvatar, settings.diceBearAvatarIndex);
    const std::string hintUtf8 = L(Splash_HintEntering);
    const std::string buttonUtf8 = L(Splash_BtnEnter);

    std::wstring html;
    html += LR"HTML_W1(
<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8" />
  <meta name="viewport" content="width=device-width, initial-scale=1.0" />
  <title>Harken Search</title>
  <style>
    :root {
      --font-sans: "Inter", "Noto Sans SC", "Noto Sans CJK SC", "Segoe UI Variable", "Segoe UI", system-ui, sans-serif;
      --bg-dark: #09111c;
      --text-primary: #ffffff;
      --text-muted: rgba(255, 255, 255, 0.7);
      --card-border: rgba(255, 255, 255, 0.12);
      --accent: rgba(111, 152, 193, 0.92);
      --accent-soft: rgba(148, 163, 184, 0.72);
    }
    html, body {
      margin: 0; padding: 0; width: 100%; height: 100%;
      background: var(--bg-dark);
      color: var(--text-primary);
      overflow: hidden;
      font-family: var(--font-sans);
      user-select: none; -webkit-user-select: none;
      -webkit-font-smoothing: antialiased;
    }
    .backdrop, .canvas, .noise {
      position: absolute; inset: 0;
    }
    .backdrop {
      inset: -50%;
      width: 200%;
      height: 200%;
      background:
        radial-gradient(circle at 50% 48%, rgba(20, 34, 52, 0.56), transparent 62%),
        radial-gradient(circle at 18% 76%, rgba(111, 152, 193, 0.13), transparent 52%),
        radial-gradient(circle at 82% 18%, rgba(203, 213, 225, 0.08), transparent 52%);
      animation: auroraShift 30s linear infinite;
      filter: blur(80px);
    }
    @keyframes auroraShift { 0% { transform: rotate(0deg); } 100% { transform: rotate(360deg); } }
    .canvas {
      mix-blend-mode: screen;
      opacity: 0.82;
    }
    .noise {
      opacity: 0.26;
      pointer-events: none;
      background: url("data:image/svg+xml;base64,PHN2ZyB4bWxucz0iaHR0cDovL3d3dy53My5vcmcvMjAwMC9zdmciIHdpZHRoPSIyMDAiIGhlaWdodD0iMjAwIj48ZmlsdGVyIGlkPSJub2lzZSI+PGZlVHVyYnVsZW5jZSB0eXBlPSJmcmFjdGFsTm9pc2UiIGJhc2VGcmVxdWVuY3k9IjAuNjUiIG51bU9jdGF2ZXM9IjMiIHN0aXRjaFRpbGVzPSJzdGl0Y2giLz48L2ZpbHRlcj48cmVjdCB3aWR0aD0iMTAwJSIgaGVpZ2h0PSIxMDAlIiBmaWx0ZXI9InVybCgjbm9pc2UpIiBvcGFjaXR5PSIwLjAzIi8+PC9zdmc+");
    }
    .shell {
      position: relative;
      z-index: 10;
      width: 100%;
      height: 100%;
      display: flex;
      align-items: center;
      justify-content: center;
      padding: 32px;
      box-sizing: border-box;
    }
    .card {
      width: min(520px, calc(100vw - 64px));
      border-radius: 30px;
      padding: 42px 42px 34px;
      box-sizing: border-box;
      background: linear-gradient(180deg, rgba(18, 28, 41, 0.76), rgba(8, 14, 22, 0.72));
      border: 1px solid var(--card-border);
      box-shadow: 0 28px 70px rgba(0, 0, 0, 0.34), inset 0 0 0 1px rgba(255,255,255,0.04);
      backdrop-filter: blur(20px) saturate(120%);
      -webkit-backdrop-filter: blur(20px) saturate(120%);
      overflow: hidden;
      position: relative;
      animation: cardIn 360ms ease-out both;
    }
    .card::before {
      content: "";
      position: absolute;
      inset: 0;
      background:
        linear-gradient(135deg, rgba(111,152,193,0.18), transparent 45%),
        linear-gradient(225deg, rgba(148,163,184,0.12), transparent 48%);
      pointer-events: none;
    }
    @keyframes cardIn {
      from { opacity: 0; transform: translateY(18px) scale(0.98); }
      to { opacity: 1; transform: translateY(0) scale(1); }
    }
    .eyebrow {
      letter-spacing: 0.22em;
      text-transform: uppercase;
      font-size: 0.74rem;
      color: var(--accent-soft);
      margin-bottom: 18px;
    }
    .title {
      font-size: clamp(2rem, 4vw, 3.1rem);
      line-height: 1.05;
      margin: 0;
      letter-spacing: -0.04em;
    }
    .name {
      margin-top: 14px;
      font-size: clamp(1.1rem, 2.2vw, 1.45rem);
      color: rgba(255,255,255,0.9);
      min-height: 1.3em;
    }
    .subtitle {
      margin-top: 18px;
      font-size: 0.98rem;
      line-height: 1.6;
      color: var(--text-muted);
      max-width: 34ch;
    }
    .rail {
      margin-top: 30px;
      height: 4px;
      border-radius: 999px;
      overflow: hidden;
      background: rgba(255,255,255,0.08);
      position: relative;
    }
    .rail::before {
      content: "";
      position: absolute;
      inset: 0;
      width: 42%;
      background: linear-gradient(90deg, var(--accent), rgba(255,255,255,0.95), var(--accent-soft));
      filter: drop-shadow(0 0 16px rgba(111,152,193,0.38));
      animation: pulseRail 1.3s ease-in-out infinite;
      transform-origin: left center;
    }
    @keyframes pulseRail {
      0%, 100% { transform: scaleX(0.82); opacity: 0.78; }
      50% { transform: scaleX(1); opacity: 1; }
    }
    .footer {
      margin-top: 18px;
      display: flex;
      align-items: center;
      justify-content: space-between;
      gap: 12px;
      flex-wrap: wrap;
    }
    .hint {
      color: var(--text-muted);
      font-size: 0.86rem;
    }
    .enter {
      border: none;
      border-radius: 999px;
      padding: 12px 18px;
      color: var(--text-primary);
      background: linear-gradient(135deg, rgba(33, 50, 70, 0.96), rgba(16, 28, 42, 0.96));
      box-shadow: 0 8px 22px rgba(111,152,193,0.20), inset 0 0 0 1px rgba(255,255,255,0.14);
      font: inherit;
      cursor: pointer;
      transition: transform 160ms ease, box-shadow 160ms ease;
    }
    .enter:hover {
      transform: translateY(-1px);
      box-shadow: 0 12px 26px rgba(111,152,193,0.26), inset 0 0 0 1px rgba(255,255,255,0.18);
    }
    @media (max-width: 640px) {
      .card { padding: 32px 24px 26px; }
      .footer { align-items: flex-start; }
    }
  </style>
</head>
)HTML_W1";
    html += L"<style>\n";
    html += L":root{--bg-dark:" + tokenMainBg +
            L";--card-border:" + tokenBorder +
            L";--text-primary:" + tokenText +
            L";--text-muted:" + tokenMuted +
            L";--accent:" + tokenControl +
            L";--accent-soft:" + BuildCssRgba(startupTheme.waveformSecondaryColor, 0.78f) +
            L";}\n";
    // Light startup chrome reuses the splash backdrop color stack.
    // Opaque theme tokens would wash out particles across the central canvas.
    // Use the splash core color and low-alpha waveform accents for light themes.
    // Dark themes continue to use their generated theme tokens.

    if (useLightStartupChrome) {
        const std::wstring welcomePrimaryRgb   = BuildCssRgbTriplet(startupTheme.waveformColor);
        const std::wstring welcomeSecondaryRgb = BuildCssRgbTriplet(startupTheme.waveformSecondaryColor);
        html += L".backdrop{background:"
              + std::wstring(L"radial-gradient(circle at 50% 50%, rgba(15, 23, 42, 0.4), transparent 60%),")
              + L"radial-gradient(circle at 20% 80%, rgba(" + welcomePrimaryRgb + L", 0.12), transparent 50%),"
              + L"radial-gradient(circle at 80% 20%, rgba(" + welcomeSecondaryRgb + L", 0.08), transparent 50%);}\n";
    } else {
        html += L".backdrop{background:radial-gradient(circle at 50% 48%," + tokenBackdropCore + L",transparent 62%),radial-gradient(circle at 18% 76%," + tokenBackdropAccentA + L",transparent 52%),radial-gradient(circle at 82% 18%," + tokenBackdropAccentB + L",transparent 52%);}\n";
    }
    html += L".card{background:linear-gradient(180deg," + tokenCardBg + L"," + BuildCssRgba(startupTheme.mainWindowBG, 0.70f) + L");border-color:" + tokenBorder + L";}\n";
    html += L".brand-word{position:fixed;top:40px;left:48px;z-index:90;color:" + tokenBrandWord + L";font-family:\"Segoe UI Variable\",\"Segoe UI\",Arial,sans-serif;font-size:clamp(22px,1.8vw,32px);font-weight:700;line-height:1;letter-spacing:-0.01em;opacity:0;transform:translateX(-10px);animation:brandIn .7s ease-out .2s forwards;isolation:isolate;mix-blend-mode:normal;filter:none;text-shadow:0 1px 0 rgba(255,255,255,.08),0 10px 26px rgba(0,0,0,.18);}\n";
    html += L".welcome-layout{position:absolute;right:clamp(56px,8vw,150px);bottom:clamp(76px,14vh,170px);z-index:10;display:flex;flex-direction:column;align-items:flex-start;justify-content:center;width:auto;max-width:min(560px,52vw);padding:18px 8px;text-align:left;}\n";
    html += L".welcome-copy{display:flex;flex-direction:column;align-items:flex-start;min-height:84px;width:100%;margin-top:0;}\n";
    html += L".welcome-headline{margin:0 0 18px 0;display:flex;flex-direction:column;align-items:flex-start;justify-content:center;gap:9px;font-family:\"STSong\",\"Songti SC\",\"Source Han Serif SC\",\"Noto Serif SC\",\"Times New Roman\",serif;font-style:italic;font-size:clamp(30px,3.2vw,48px);line-height:1.06;letter-spacing:0;opacity:0;transform:translateX(-46px);filter:blur(5px);animation:noteIn 1.85s cubic-bezier(.16,1,.3,1) .28s forwards;}\n";
    html += L".welcome-lead{color:" + tokenLeadText + L";font-weight:300;letter-spacing:0;}\n";
    html += L".welcome-name{display:inline-block;font-family:\"Cambria\",\"Georgia\",\"Palatino Linotype\",\"Book Antiqua\",\"Times New Roman\",serif;font-style:italic;font-size:1.06em;font-weight:700;line-height:.96;color:transparent;background-image:linear-gradient(110deg," + tokenControl + L" 0%," + tokenText + L" 52%," + BuildCssRgba(startupTheme.waveformColor, 1.0f) + L" 100%);background-size:200% auto;background-clip:text;-webkit-background-clip:text;-webkit-text-fill-color:transparent;filter:drop-shadow(0 0 16px " + tokenNameGlow + L");text-shadow:0 1px 0 " + tokenCardSheen + L",0 0 2px " + BuildCssRgba(startupTheme.interfaceText, useLightStartupChrome ? 0.10f : 0.14f) + L";animation:shimmer 5s linear infinite;transform:translateY(-.025em);}\n";
    html += L".welcome-fallback{color:" + tokenText + L";font-style:normal;font-weight:600;}\n";
    html += L".accent-line{width:0;height:1px;margin-bottom:16px;background:linear-gradient(90deg," + tokenControl + L" 0%," + BuildCssRgba(startupTheme.toolbarAccent, useLightStartupChrome ? 0.56f : 0.22f) + L" 42%,transparent 100%);opacity:0;animation:lineIn .75s ease-out .95s forwards;}\n";
    html += L".continue-hitbox{position:absolute;inset:0;z-index:15;border:0;background:transparent;cursor:default;}\n";
    html += L".backdrop,.canvas,.noise{will-change:opacity,transform;transition:opacity 620ms cubic-bezier(.16,1,.3,1),transform 620ms cubic-bezier(.16,1,.3,1);}\n";
    html += L".transition-wash{display:none;}\n";
    html += L".welcome-layout{position:relative;right:auto;bottom:auto;z-index:10;display:flex;flex-direction:column;align-items:center;justify-content:center;width:min(312px,calc(100vw - 48px));min-height:300px;padding:30px 28px 28px;text-align:center;border-radius:20px;background:linear-gradient(180deg," + tokenCardTop + L"," + tokenCardBottom + L");border:1px solid " + BuildCssRgba(startupTheme.waveformPanelBorder, useLightStartupChrome ? 0.28f : 0.18f) + L";box-shadow:0 14px 38px " + tokenCardShadow + L",inset 0 0 0 1px " + tokenCardSheen + L";backdrop-filter:blur(12px) saturate(108%);-webkit-backdrop-filter:blur(12px) saturate(108%);overflow:hidden;animation:cardIn 520ms cubic-bezier(.16,1,.3,1) both;}\n";
    html += L".welcome-layout::before{content:\"\";position:absolute;inset:0;pointer-events:none;background:linear-gradient(135deg," + tokenCardSheen + L",transparent 46%);}\n";
    html += L".welcome-layout::after{content:\"\";position:absolute;inset:auto -12% 44% -12%;height:56%;pointer-events:none;background:radial-gradient(circle at 24% 28%," + tokenCardBloomA + L",transparent 58%),radial-gradient(circle at 76% 6%," + tokenCardBloomB + L",transparent 54%);filter:blur(16px);opacity:.98;}\n";
    html += L".avatar-wrap{position:relative;width:86px;height:86px;margin:0 0 22px;display:grid;place-items:center;}\n";
    html += L".avatar-ring,.avatar-orbit{display:none!important;}\n";
    html += L".avatar{position:relative;z-index:1;width:74px;height:74px;border-radius:50%;display:grid;place-items:center;color:rgba(255,255,255,.94);font-family:var(--font-sans);font-size:23px;font-weight:680;letter-spacing:-.03em;background:radial-gradient(circle at 34% 24%,rgba(255,255,255,.18),transparent 30%),linear-gradient(145deg,hsl(var(--avatar-hue),44%,43%),hsl(calc(var(--avatar-hue) + 22),38%,30%));box-shadow:inset 0 0 0 1px rgba(255,255,255,.11),0 6px 16px rgba(0,0,0,.20);animation:avatarPulse 2.8s ease-in-out infinite;}\n";
    html += L".avatar-img{position:absolute;inset:0;width:100%;height:100%;border-radius:50%;object-fit:cover;image-rendering:-webkit-optimize-contrast;display:none;background:rgba(255,255,255,.04);}.avatar.has-image{background:rgba(255,255,255,.045);}.avatar.has-image .avatar-img{display:block;}.avatar.has-image #avatarInitials{display:none;}\n";
    html += L".avatar::after{content:\"\";position:absolute;inset:-7px;border-radius:50%;background:" + tokenNameGlow + L";z-index:-1;animation:pulseHalo 2.8s ease-in-out infinite;}\n";
    html += L".startup-status{margin-top:18px;color:" + tokenStatusText + L";font-size:12.5px;line-height:1.45;font-weight:420;animation:statusPulse 2.2s ease-in-out infinite;}\n";
    html += L".startup-detail{display:none;}\n";
    html += L".loading-dots{display:none;}.progress-track{display:none;}\n";
    html += L".welcome-copy{position:relative;z-index:1;align-items:center;min-height:0;}.welcome-headline{margin:0;align-items:center;gap:0;font-family:var(--font-sans);font-style:normal;font-size:22px;line-height:1.16;font-weight:600;letter-spacing:-.02em;animation:none;opacity:1;transform:none;filter:none;}.welcome-lead{color:" + tokenLeadText + L";font-weight:600;}.welcome-name{display:block;margin-top:8px;max-width:100%;font-family:var(--font-sans);font-style:normal;font-size:14.5px;font-weight:480;line-height:1.35;color:" + tokenNameText + L";background:none;-webkit-text-fill-color:currentColor;filter:none;text-shadow:none;animation:none;overflow:hidden;text-overflow:ellipsis;white-space:nowrap;}.welcome-fallback{font-family:var(--font-sans);}.accent-line{display:block;width:92px;opacity:" + std::wstring(useLightStartupChrome ? L".78" : L".68") + L";}\n";
    html += L".welcome-headline{will-change:opacity,transform,filter;transition:opacity 220ms cubic-bezier(.22,1,.36,1),transform 220ms cubic-bezier(.22,1,.36,1),filter 220ms cubic-bezier(.22,1,.36,1);}\n";
    html += L"body.phase-out .welcome-layout,body.phase-out .brand-word{opacity:0;transform:translateX(18px);transition:opacity 360ms cubic-bezier(.22,1,.36,1),transform 360ms cubic-bezier(.22,1,.36,1);}\n";
    html += L"body.phase-out .welcome-headline{opacity:0;transform:translateX(18px);filter:blur(5px);}\n";
    html += L"body.phase-out .accent-line{opacity:0;transform:scaleX(.35);transform-origin:left center;transition:opacity 260ms ease,transform 360ms cubic-bezier(.22,1,.36,1);}\n";
    html += L"@keyframes brandIn{to{opacity:1;transform:translateX(0);}}\n";
    html += L"@keyframes noteIn{to{opacity:1;transform:translateX(0);filter:blur(0);}}\n";
    html += L"@keyframes shimmer{from{background-position:200% center;}to{background-position:0% center;}}\n";
    html += L"@keyframes lineIn{to{width:min(7vw,80px);opacity:.68;}}\n";
    html += L"@keyframes avatarPulse{0%,100%{transform:scale(1);}50%{transform:scale(1.018);}}\n";
    html += L"@keyframes pulseHalo{0%,100%{opacity:.18;transform:scale(.96);}50%{opacity:.34;transform:scale(1.05);}}\n";
    html += L"@keyframes statusPulse{0%,100%{opacity:.68;}50%{opacity:.92;}}\n";
    html += L"@media (min-width:640px){.welcome-headline{flex-direction:row;gap:14px;}}\n";
    html += L"@media (max-width:900px){.welcome-layout{right:40px;bottom:64px;max-width:calc(100vw - 88px);}.welcome-headline{font-size:clamp(28px,4.4vw,42px);line-height:1.08;}.welcome-copy{min-height:78px;}}\n";
    html += L"@media (max-width:640px){.brand-word{top:32px;left:32px;font-size:20px;}.welcome-layout{right:28px;bottom:52px;max-width:calc(100vw - 56px);padding:16px 4px;}.welcome-headline{gap:8px;}.welcome-lead{letter-spacing:0;}}\n";
    html += L".canvas{opacity:.22!important;}.backdrop{opacity:.74!important;filter:blur(92px)!important;}.brand-word{opacity:.78!important;z-index:90!important;filter:none!important;mix-blend-mode:normal!important;}.welcome-headline{flex-direction:column!important;gap:0!important;}.welcome-layout{right:auto!important;bottom:auto!important;max-width:min(312px,calc(100vw - 48px))!important;}\n";
    html += L"@media (prefers-reduced-motion:reduce){*,*::before,*::after{animation-duration:.01ms!important;animation-iteration-count:1!important;transition-duration:.01ms!important;}}\n";
    AppendStartupBrandLogoCss(html, useLightStartupChrome);
    html += L"</style>\n";
    html += LR"HTML_W2(
<body>
  <div class="backdrop"></div>
  <canvas id="fx" class="canvas"></canvas>
  <div class="noise"></div>
  <div class="shell">
    <div class="brand-word">Harken Search</div>
    <div class="welcome-layout">
      <div class="avatar-wrap" aria-hidden="true">
        <div class="avatar" id="avatarBubble">
          <img class="avatar-img" id="avatarImage" alt="" />
          <span id="avatarInitials">AR</span>
        </div>
      </div>
      <div class="welcome-copy">
        <h1 class="welcome-headline">
          <span class="welcome-lead" id="title"></span>
          <span class="welcome-name" id="nameLine"></span>
          <span class="welcome-fallback" id="fallbackLine"></span>
        </h1>
        <div class="accent-line"></div>
      </div>
      <div class="startup-status"><span id="statusText">Preparing your sound workspace...</span></div>
    </div>
    <button class="continue-hitbox" id="continueBtn" type="button" aria-label="Continue"></button>
    <div class="transition-wash"></div>
  </div>
  <script>
)HTML_W2";
    html += L"    const themeAccent = \"" + themeAccent + L"\";\n";
    html += L"    const themeAccentSoft = \"" + themeAccentSoft + L"\";\n";
    html += L"    const themeMist = \"" + themeMist + L"\";\n";
    html += L"    const themeChrome = \"" + themeChrome + L"\";\n";
    html += L"    const themeTextGlow = \"" + themeTextGlow + L"\";\n";
    html += L"    const canvasWash = \"" + tokenCanvasWash + L"\";\n";
    html += L"    const themedParticleChance = " + themedParticleChance + L";\n";
    html += L"    const welcomeTitle = \"" + Utf8ToWide(JsonEscape(titleUtf8)) + L"\";\n";
    html += L"    const welcomeName = \"" + Utf8ToWide(JsonEscape(displayNameUtf8)) + L"\";\n";
    html += L"    const welcomeSubtitle = \"" + Utf8ToWide(JsonEscape(subtitleUtf8)) + L"\";\n";
    html += L"    const startupAvatarDataUri = \"" + Utf8ToWide(JsonEscape(startupAvatarDataUri)) + L"\";\n";
    html += L"    const welcomeHint = \"" + Utf8ToWide(JsonEscape(hintUtf8)) + L"\";\n";
    html += L"    const welcomeButton = \"" + Utf8ToWide(JsonEscape(buttonUtf8)) + L"\";\n";
    html += LR"HTML_W3(
    const canvas = document.getElementById("fx");
    const ctx = canvas.getContext("2d");
    const readViewport = () => {
      const vv = window.visualViewport;
      const nextW = Math.max(1, Math.round(vv ? vv.width : (window.innerWidth || document.documentElement.clientWidth || 1)));
      const nextH = Math.max(1, Math.round(vv ? vv.height : (window.innerHeight || document.documentElement.clientHeight || 1)));
      return { nextW, nextH };
    };
    let { nextW: width, nextH: height } = readViewport();
    canvas.width = width;
    canvas.height = height;
    const stageCx = () => {
      const note = document.querySelector(".welcome-layout");
      if (note) {
        const rect = note.getBoundingClientRect();
        if (rect.width > 1) return rect.left + rect.width * 0.5;
      }
      return width * 0.62;
    };
    const stageCy = () => height * 0.52;
    const palettes = [
      themeTextGlow,
      themeAccent,
      themeChrome,
      themeAccentSoft,
      themeMist,
      themeChrome
    ];
    let time = 0;
    let particles = [];
    let waves = [];

    class Particle {
      constructor() {
        this.isThemeColor = Math.random() * 100 < themedParticleChance;
        const spawnX = Math.pow(Math.random(), 0.72);
        this.x = width * (0.24 + spawnX * 0.64);
        this.y = Math.random() * height;
        this.size = Math.random() * 2 + 0.5 + (this.isThemeColor ? 0.6 : 0.0);
        this.speedX = (Math.random() - 0.5) * 0.5;
        this.speedY = (Math.random() - 0.5) * 0.5;
        this.color = this.isThemeColor
          ? (Math.random() < 0.68 ? themeAccent : themeAccentSoft)
          : palettes[Math.floor(Math.random() * palettes.length)];
        this.alpha = 0.16 + Math.random() * 0.72;
        this.flashSpeed = (this.isThemeColor ? 0.016 : 0.02) + Math.random() * (this.isThemeColor ? 0.035 : 0.05);
        this.phase = Math.random() * Math.PI * 2;
      }
      update(t) {
        const angle = Math.sin(this.x * 0.002 + t * 0.5) + Math.cos(this.y * 0.002 + t * 0.5);
        this.x += Math.cos(angle) * 1 + this.speedX;
        this.y += Math.sin(angle) * 1 + this.speedY;
        const dx = stageCx() - this.x;
        const dy = stageCy() - this.y;
        const dist = Math.sqrt(dx * dx + dy * dy);
        const pull = Math.min(0.18, 560 / (dist * dist + 220));
        const theta = Math.atan2(dy, dx);
        this.x += Math.cos(theta) * pull + Math.cos(theta + 1.57) * pull * 0.85;
        this.y += Math.sin(theta) * pull + Math.sin(theta + 1.57) * pull * 0.85;
        this.alpha = 0.18 + 0.72 * Math.abs(Math.sin(t * 8 * this.flashSpeed + this.phase));
        this.glowSize = this.size + Math.max(0, (200 - dist) / 64);
        if (this.x < 0) this.x = width;
        if (this.x > width) this.x = 0;
        if (this.y < 0) this.y = height;
        if (this.y > height) this.y = 0;
        if (dist < 250) {
          const glowAlpha = (1 - dist / 250) * 0.24 * this.alpha;
          ctx.beginPath();
          ctx.arc(this.x, this.y, this.glowSize * 3, 0, Math.PI * 2);
          ctx.fillStyle = this.color + glowAlpha + ")";
          ctx.fill();
        }
        ctx.beginPath();
        ctx.arc(this.x, this.y, this.glowSize, 0, Math.PI * 2);
        ctx.fillStyle = this.color + Math.max(0, Math.min(0.9, this.alpha)) + ")";
        ctx.fill();
      }
    }

    class Wave {
      constructor(y, color, alpha = 0.15, glow = 20, lineWidth = 2) {
        this.yOffset = y;
        this.amplitude = 50 + Math.random() * 100;
        this.wavelength = 0.002 + Math.random() * 0.003;
        this.speed = 0.01 + Math.random() * 0.02;
        this.color = color;
        this.alpha = alpha;
        this.glow = glow;
        this.lineWidth = lineWidth;
        this.phase = Math.random() * Math.PI * 2;
      }
      update(t) {
        ctx.beginPath();
        ctx.moveTo(0, height / 2);
        for (let x = 0; x < width; x += 10) {
          const y = this.yOffset + Math.sin(x * this.wavelength + t * this.speed + this.phase) * this.amplitude * Math.sin(t * 0.5);
          const noise = Math.cos(x * 0.01 - t) * 20;
          ctx.lineTo(x, y + noise);
        }
        ctx.strokeStyle = this.color + this.alpha + ")";
        ctx.lineWidth = this.lineWidth;
        ctx.shadowBlur = this.glow;
        ctx.shadowColor = this.color + "1)";
        ctx.stroke();
        ctx.shadowBlur = 0;
      }
    }

    function init() {
      particles = [];
      waves = [];
      for (let i = 0; i < 250; i++) particles.push(new Particle());
      waves.push(new Wave(height * 0.46, palettes[0], 0.12, 18, 1.9));
      waves.push(new Wave(height * 0.50, themeAccent, 0.24, 32, 2.8));
      waves.push(new Wave(height * 0.56, themeAccentSoft, 0.18, 26, 2.3));
      waves.push(new Wave(height * 0.42, palettes[5], 0.10, 14, 1.7));
    }

    function handleResize() {
      const viewport = readViewport();
      width = viewport.nextW;
      height = viewport.nextH;
      canvas.width = width;
      canvas.height = height;
      init();
    }

    function animate() {
      ctx.fillStyle = canvasWash;
      ctx.fillRect(0, 0, width, height);
      time += 0.012;
      waves.forEach((wave) => wave.update(time));
      particles.forEach((particle) => particle.update(time));
      requestAnimationFrame(animate);
    }

    window.addEventListener("resize", handleResize);
    if (window.visualViewport) {
      window.visualViewport.addEventListener("resize", handleResize);
    }
    init();
    animate();

    const titleEl = document.getElementById("title");
    const nameLine = document.getElementById("nameLine");
    const fallbackLine = document.getElementById("fallbackLine");
    const displayName = (welcomeName || "Harken Search User").trim() || "Harken Search User";
    titleEl.textContent = welcomeTitle;
    nameLine.textContent = displayName;
    fallbackLine.style.display = "none";
    const chars = Array.from(displayName);
    document.getElementById("avatarInitials").textContent =
      (chars.length > 0 ? chars.slice(0, 2).join("") : "AU").toUpperCase();
    const hash = chars.reduce((acc, ch) => ((acc * 31) + ch.codePointAt(0)) >>> 0, 2166136261);
    document.documentElement.style.setProperty("--avatar-hue", String(196 + (hash % 34)));
    if (startupAvatarDataUri) {
      const avatarImage = document.getElementById("avatarImage");
      avatarImage.src = startupAvatarDataUri;
      document.getElementById("avatarBubble").classList.add("has-image");
    }
    document.getElementById("statusText").textContent = welcomeSubtitle;

    let sent = false;
    function continueIntoLibrary() {
      if (sent) return;
      sent = true;
      document.body.classList.add("phase-out");
      setTimeout(() => {
        if (window.chrome && window.chrome.webview) {
          window.chrome.webview.postMessage("welcome_continue");
        }
      }, 420);
    }
    document.getElementById("continueBtn").addEventListener("click", continueIntoLibrary);
    document.body.addEventListener("keydown", (event) => {
      if (event.key === "Enter" || event.key === " " || event.key === "Spacebar") {
        continueIntoLibrary();
      }
    });
    setTimeout(continueIntoLibrary, 2300);
  </script>
</body>
</html>
)HTML_W3";
    // Fill welcome template defaults from the localization ledger.
    {
        const std::wstring subW = Utf8ToWide(subtitleUtf8);
        size_t pos = 0;
        while ((pos = html.find(SUB_W_T, pos)) != std::wstring::npos) {
            html.replace(pos, sizeof(SUB_W_T) / sizeof(wchar_t) - 1, subW);
            pos += subW.size();
        }
        const std::wstring langW = (g_currentLanguage == AppLanguage::Japanese)   ? L"ja"
                                     : (g_currentLanguage == AppLanguage::English) ? L"en" : L"zh-CN";
        {
            size_t lq = 0;
            const std::wstring needle = L"<html lang=\"en\">";
            const std::wstring repl = L"<html lang=\"" + langW + L"\">";
            while ((lq = html.find(needle, lq)) != std::wstring::npos) {
                html.replace(lq, needle.size(), repl);
                lq += repl.size();
            }
        }
        const std::wstring ariaW = L"aria-label=\"" + Utf8ToWide(L(Splash_AriaContinue)) + L"\"";
        pos = 0;
        while ((pos = html.find(L"aria-label=\"Continue\"", pos)) != std::wstring::npos) {
            html.replace(pos, sizeof(L"aria-label=\"Continue\"") / sizeof(wchar_t) - 1, ariaW);
            pos += ariaW.size();
        }
    }
    return html;
}

static void RunBetaSplashScreen(
    HWND hwnd, HDC hdc, AppState& state,
    std::function<void(std::function<void(int,int,const std::string&)>)> loadFunc,
    std::function<void(std::function<void(int,int,const std::string&)>)> finalizeUiFunc)
{
    // 1. Load splash PNG
    std::string splashPath = GetExeDirUtf8() + "\\splash_beta.png";
    int imgW = 0, imgH = 0, imgCh = 0;
    unsigned char* pixels = stbi_load(splashPath.c_str(), &imgW, &imgH, &imgCh, 4);
    GLuint splashTex = 0;
    if (pixels && imgW > 0 && imgH > 0) {
        glGenTextures(1, &splashTex);
        glBindTexture(GL_TEXTURE_2D, splashTex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, imgW, imgH, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
        stbi_image_free(pixels);
    }

    // 2. Save original window style + rect, switch to borderless popup
    LONG origStyle = GetWindowLong(hwnd, GWL_STYLE);
    LONG origExStyle = GetWindowLong(hwnd, GWL_EXSTYLE);
    RECT origRect;
    GetWindowRect(hwnd, &origRect);

    // Calculate splash window size: use image aspect ratio, cap at 70% of screen
    HMONITOR hMon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi = {}; mi.cbSize = sizeof(mi);
    GetMonitorInfoW(hMon, &mi);
    int scrW = mi.rcWork.right - mi.rcWork.left;
    int scrH = mi.rcWork.bottom - mi.rcWork.top;

    int splW = imgW, splH = imgH;
    if (splW <= 0 || splH <= 0) { splW = 800; splH = 450; }
    // Scale to fit 30% of screen (small & refined splash)
    float scale = std::min((float)scrW * 0.30f / (float)splW,
                           (float)scrH * 0.30f / (float)splH);
    splW = (int)((float)splW * scale);
    splH = (int)((float)splH * scale);
    int splX = mi.rcWork.left + (scrW - splW) / 2;
    int splY = mi.rcWork.top  + (scrH - splH) / 2;

    // Strip all chrome → borderless popup, but stay HIDDEN for now
    SetWindowLong(hwnd, GWL_STYLE, WS_POPUP);
    SetWindowLong(hwnd, GWL_EXSTYLE, WS_EX_APPWINDOW);
    SetWindowPos(hwnd, HWND_TOP, splX, splY, splW, splH,
                 SWP_FRAMECHANGED | SWP_NOACTIVATE);

    // Pre-render first frame to framebuffer BEFORE showing the window
    // This eliminates the flash of stale/empty content.
    {
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        float W = (float)splW, H = (float)splH;
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2(W, H));
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 1));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::Begin("##BetaSplashPre", nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
            ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoDecoration);
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        if (splashTex) {
            dl->AddImage((ImTextureID)(intptr_t)splashTex,
                ImVec2(0, 0), ImVec2(W, H));
        }
        ImGui::End();
        ImGui::Render();
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        if (ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
            ImGui::UpdatePlatformWindows();
            ImGui::RenderPlatformWindowsDefault();
        }
        SwapBuffers(hdc);
    }

    // Show the window only after the splash framebuffer is ready.
    // Must use SW_SHOW (not SW_SHOWNA) so the window activates and comes to foreground.
    (void)Platform::WindowShow(hwnd, SW_SHOW);
    (void)Platform::WindowSetForeground(hwnd);

    // 3. Run startup phases in background thread
    std::atomic<bool> loadingDone{false};
    std::string phaseMsg;
    std::mutex phaseMutex;

    auto renderBetaSplashFrame = [&](const std::string& forcedMessage) {
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        float W = ImGui::GetIO().DisplaySize.x;
        float H = ImGui::GetIO().DisplaySize.y;

        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2(W, H));
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 1));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::Begin("##BetaSplash", nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
            ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoDecoration);
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();

        ImDrawList* dl = ImGui::GetWindowDrawList();

        // Draw splash image — fill entire window (image matches window aspect)
        if (splashTex) {
            dl->AddImage((ImTextureID)(intptr_t)splashTex,
                ImVec2(0, 0), ImVec2(W, H));
        }

        // Progress text at bottom center
        if (!ShouldDisableHarnessBgThrottle()) {
            std::string txt = forcedMessage;
            if (txt.empty()) {
                std::lock_guard<std::mutex> lk(phaseMutex);
                txt = phaseMsg;
            }
            if (!txt.empty()) {
                ImVec2 ts = ImGui::CalcTextSize(txt.c_str());
                dl->AddText(ImVec2((W - ts.x) * 0.5f, H - 32.0f),
                    IM_COL32(200, 200, 200, 140), txt.c_str());
            }
        }

        ImGui::End();
        ImGui::Render();
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        if (ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
            ImGui::UpdatePlatformWindows();
            ImGui::RenderPlatformWindowsDefault();
        }
        SwapBuffers(hdc);
    };

    std::thread worker([&]() {
        auto progressCb = [&](int, int, const std::string& msg) {
            std::lock_guard<std::mutex> lk(phaseMutex);
            phaseMsg = msg;
        };
        loadFunc(progressCb);
        loadingDone.store(true);
    });

    // 4. Render loop: splash image fills the borderless window
    while (!loadingDone.load()) {
        MSG msg;
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { loadingDone.store(true); break; }
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        PollQaCommandFile(state);
        CoverModule::PumpCoverTextureUploads(2, 500);
        renderBetaSplashFrame({});
    }

    if (worker.joinable()) worker.join();

    auto pumpBetaStartupUi = [&]() {
        MSG msg;
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) break;
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        PollQaCommandFile(state);
        CoverModule::PumpCoverTextureUploads(2, 500);
        renderBetaSplashFrame(L(Splash_WarmFirstCover));
    };

    (void)WaitForStartupCoverSourceForQa(
        state,
        pumpBetaStartupUi,
        std::chrono::seconds(15));
    WarmStartupFirstScreenCoversForQa(state, pumpBetaStartupUi);

    // 5. Finalize on UI thread
    if (finalizeUiFunc) finalizeUiFunc([](int, int, const std::string&) {});

    // 6. Restore original window style + maximize for main UI
    SetWindowLong(hwnd, GWL_STYLE, origStyle);
    SetWindowLong(hwnd, GWL_EXSTYLE, origExStyle);
    SetWindowPos(hwnd, nullptr,
                 origRect.left, origRect.top,
                 origRect.right - origRect.left, origRect.bottom - origRect.top,
                 SWP_FRAMECHANGED | SWP_NOZORDER);
    (void)Platform::WindowShow(hwnd, SW_SHOWMAXIMIZED);

    // 7. Cleanup texture
    if (splashTex) glDeleteTextures(1, &splashTex);
}

// WebView2 Loading Screen

static void RunWebView2LoadingScreen(
    HWND hwnd,
    AppState& state,
    const AppSettings& appSettings,
    ComPtr<ICoreWebView2Controller>& controller,
    ComPtr<ICoreWebView2>& webview,
    std::function<void(std::function<void(int,int,const std::string&)>)> loadFunc,
    std::function<void(std::function<void(int,int,const std::string&)>)> finalizeUiFunc)
{
    if (!controller || !webview || !hwnd) return;
    DisableWebShortcutsForStartupUi(controller.Get(), webview.Get());

    {
        Microsoft::WRL::ComPtr<ICoreWebView2Controller4> ctrl4;
        if (SUCCEEDED(controller.As(&ctrl4)) && ctrl4) {
            ctrl4->put_AllowExternalDrop(FALSE);
        }
    }

    // 拦截所有非初始页的导航（阻止 file:// 等意外跳转）
    webview->add_NavigationStarting(
        Callback<ICoreWebView2NavigationStartingEventHandler>(
            [](ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs* args) -> HRESULT {
                if (!args) return S_OK;
                LPWSTR uri = nullptr;
                args->get_Uri(&uri);
                if (uri) {
                    std::wstring u(uri);
                    Platform::FreeComTaskMemory(uri);

                    if (u.find(L"about:blank") == std::wstring::npos &&
                        u.find(L"data:") != 0 &&
                        u.find(L"blob:") != 0) {
                        args->put_Cancel(TRUE);
                    }
                }
                return S_OK;
            }).Get(),
        nullptr);

    // 导航到加载画面 HTML
    std::wstring html = BuildFlashUiLoadingHtml(appSettings);
    webview->NavigateToString(html.c_str());

    // 等待导航完成 (简单延时让 WebView2 渲染)
    for (int i = 0; i < 30; i++) {
        MSG msg = {};
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        // 由 UI 主线程负责封面纹理上传（启动阶段非封面墙，小预算降低主线程压力）
        CoverModule::PumpCoverTextureUploads(2, 500);
        if (controller) {
            RECT rc = {};
            GetClientRect(hwnd, &rc);
            controller->put_Bounds(rc);
        }
        Platform::SleepMilliseconds(16);
    }

    // 加载进度回调（后台线程只写入队列，不触碰 WebView2）
    struct ProgressEvent {
        int loaded = 0;
        int total = 0;
        std::string message;
    };
    std::mutex progressMutex;
    std::vector<ProgressEvent> progressQueue;
    std::atomic<bool> loadDone{ false };

    int lastLoaded = 0;
    int lastTotal = 0;

    auto progressCallback = [&](int loaded, int total, const std::string& message) {
        std::lock_guard<std::mutex> lock(progressMutex);
        progressQueue.push_back({ loaded, total, message });
    };

    auto pumpUi = [&]() {
        MSG msg = {};
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        PollQaCommandFile(state);
        CoverModule::PumpCoverTextureUploads(2, 500);
        if (controller) {
            RECT rc = {};
            GetClientRect(hwnd, &rc);
            controller->put_Bounds(rc);
        }
    };

    auto flushProgressQueue = [&]() {
        std::vector<ProgressEvent> local;
        {
            std::lock_guard<std::mutex> lock(progressMutex);
            local.swap(progressQueue);
        }
        for (const auto& ev : local) {
            lastLoaded = ev.loaded;
            lastTotal = ev.total;
            std::string payload = "window.updateProgress(";
            payload += std::to_string(ev.loaded);
            payload += ", ";
            payload += std::to_string(ev.total);
            payload += ", \"";
            payload += JsonEscape(ev.message);
            payload += "\");";
            webview->ExecuteScript(Utf8ToWide(payload).c_str(), nullptr);
        }
        return local;
    };

    auto presentProgress = [&](int loaded, int total, const std::string& message) {
        progressCallback(loaded, total, message);
        pumpUi();
        flushProgressQueue();
        Platform::SleepMilliseconds(16);
        pumpUi();
    };

    // 后台线程执行真实加载（避免阻塞 UI 线程）
    std::thread worker([&]() {
        loadFunc(progressCallback);
        loadDone.store(true, std::memory_order_release);
    });

    // UI 线程仅做消息泵 + 进度同步（确保 WebView2 丝滑渲染）
    while (true) {
        pumpUi();
        const std::vector<ProgressEvent> local = flushProgressQueue();

        if (loadDone.load(std::memory_order_acquire)) {
            if (local.empty()) break;
        }
        Platform::SleepMilliseconds(8);
    }

    if (worker.joinable()) worker.join();
    flushProgressQueue();

    // 等待首包搜索结果（含 facet），以便进主界面时封面墙已有列表
    presentProgress(0, 1, L(Splash_PrepareCoverList));
    flushProgressQueue();
    (void)WaitForStartupCoverSourceForQa(
        state,
        [&]() {
            pumpUi();
            flushProgressQueue();
        },
        std::chrono::seconds(15));
    presentProgress(1, 1, L(Splash_PrepareCoverList));
    flushProgressQueue();

    // Keep the startup/preheat screen visible until the first artwork band is
    // warmed. The registry/facet list is already ready here; without this
    // bounded texture warmup the main UI opens on black placeholder cells and
    // slowly fills them after the user can see the wall.
    presentProgress(0, 1, L(Splash_WarmFirstCover));
    flushProgressQueue();
    WarmStartupFirstScreenCoversForQa(
        state,
        [&]() {
            pumpUi();
            flushProgressQueue();
        });
    presentProgress(1, 1, L(Splash_WarmFirstCover));
    flushProgressQueue();

    if (finalizeUiFunc) {
        finalizeUiFunc(presentProgress);
        flushProgressQueue();
    }

    // 完成后保持 100% 状态；真正的退出过渡放到主循环里执行，
    // 这样底层主界面可以在 WebView 渐隐的同时开始渲染，避免黑闪和“断层”。
    const int finalTotal = lastTotal > 0 ? lastTotal : 1;
    const int finalLoaded = finalTotal;
    {
        std::string payload = "window.updateProgress(";
        payload += std::to_string(finalLoaded);
        payload += ", ";
        payload += std::to_string(finalTotal);
        payload += ", \"";
        payload += JsonEscape(std::string(L(Done)));
        payload += "\");";
        webview->ExecuteScript(Utf8ToWide(payload).c_str(), nullptr);
    }
}

static float GetStartupEntryTransitionT(float elapsedMs, float durationMs) {
    return std::clamp(durationMs > 0.0f ? (elapsedMs / durationMs) : 1.0f, 0.0f, 1.0f);
}

static float EaseStartupEntry(float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

static float GetStartupEntryUiAlpha(float elapsedMs, float durationMs) {
    const float t = GetStartupEntryTransitionT(elapsedMs, durationMs);
    const float eased = EaseStartupEntry(t);
    const float delayed = std::pow(eased, 1.45f);
    return std::clamp(delayed, 0.0f, 1.0f);
}

static bool DrawStartupEntryOverlay(const AppSettings& settings, float elapsedMs, float durationMs) {
    const float t = GetStartupEntryTransitionT(elapsedMs, durationMs);
    const float eased = EaseStartupEntry(t);
    const float overlay = 1.0f - eased;
    if (overlay <= 0.001f) {
        return false;
    }

    auto mix = [](const ImVec4& a, const ImVec4& b, float k) -> ImVec4 {
        return ImVec4(
            a.x + (b.x - a.x) * k,
            a.y + (b.y - a.y) * k,
            a.z + (b.z - a.z) * k,
            a.w + (b.w - a.w) * k);
    };
    auto withAlpha = [](ImVec4 c, float a) -> ImVec4 { c.w = a; return c; };

    ImGuiIO& io = ImGui::GetIO();
    const ImVec2 ds = io.DisplaySize;
    const float W = ds.x;
    const float H = ds.y;
    if (W <= 1.0f || H <= 1.0f) {
        return false;
    }

    ImVec4* colors = ImGui::GetStyle().Colors;
    ImVec4 accentA = colors[ImGuiCol_ButtonActive];
    ImVec4 accentB = colors[ImGuiCol_Text];
    if (settings.useCustomColors) {
        accentA = settings.customAccentColor;
        accentB = mix(settings.customAccentColor, ImVec4(1, 1, 1, 1), 0.35f);
    }
    const ImVec4 themedScrim = mix(colors[ImGuiCol_WindowBg], ImVec4(0.0f, 0.0f, 0.0f, 1.0f), 0.18f);

    ImDrawList* dl = ImGui::GetForegroundDrawList();
    dl->AddRectFilled(ImVec2(0, 0), ImVec2(W, H), ImGui::GetColorU32(withAlpha(themedScrim, 0.56f * overlay)));

    const float glowAlpha = 0.22f * overlay;
    dl->AddCircleFilled(ImVec2(W * 0.28f, H * 0.64f), std::min(W, H) * 0.20f,
                        ImGui::GetColorU32(withAlpha(accentA, glowAlpha)), 56);
    dl->AddCircleFilled(ImVec2(W * 0.74f, H * 0.32f), std::min(W, H) * 0.18f,
                        ImGui::GetColorU32(withAlpha(accentB, glowAlpha * 0.8f)), 56);

    const float cardW = 360.0f;
    const float cardH = 150.0f;
    const float cardX = std::floor(W * 0.5f - cardW * 0.5f);
    const float mainSettlePx = 10.0f;
    const float cardY = std::floor(H * 0.5f - cardH * 0.5f + (1.0f - eased) * mainSettlePx);
    const float radius = 24.0f;
    const ImVec4 cardBg = withAlpha(mix(colors[ImGuiCol_WindowBg], ImVec4(0, 0, 0, 1), 0.25f), 0.74f * overlay);
    const ImVec4 cardBorder = withAlpha(mix(accentA, colors[ImGuiCol_Text], 0.20f), 0.42f * overlay);
    const ImVec4 titleCol = withAlpha(colors[ImGuiCol_Text], 0.96f * overlay);
    const ImVec4 subCol = withAlpha(mix(colors[ImGuiCol_TextDisabled], colors[ImGuiCol_Text], 0.28f), 0.86f * overlay);

    dl->AddRectFilled(ImVec2(cardX, cardY), ImVec2(cardX + cardW, cardY + cardH), ImGui::GetColorU32(cardBg), radius);
    dl->AddRect(ImVec2(cardX, cardY), ImVec2(cardX + cardW, cardY + cardH), ImGui::GetColorU32(cardBorder), radius, 0, 1.0f);
    dl->AddLine(ImVec2(cardX, cardY + 0.5f), ImVec2(cardX + cardW, cardY + 0.5f),
                ImGui::GetColorU32(withAlpha(mix(colors[ImGuiCol_Text], colors[ImGuiCol_WindowBg], 0.84f), 0.18f * overlay)), 1.0f);

    const char* title = L(Splash_TransitionTitle);
    const char* subtitle = L(Splash_TransitionSub);
    ImVec2 titleSize = ImGui::CalcTextSize(title);
    ImVec2 subSize = ImGui::CalcTextSize(subtitle);
    dl->AddText(ImVec2(std::floor(W * 0.5f - titleSize.x * 0.5f), cardY + 48.0f), ImGui::GetColorU32(titleCol), title);
    dl->AddText(ImVec2(std::floor(W * 0.5f - subSize.x * 0.5f), cardY + 82.0f), ImGui::GetColorU32(subCol), subtitle);

    const float lineY = cardY + cardH - 28.0f;
    const float lineW = 180.0f + eased * 80.0f;
    const float lineX = W * 0.5f - lineW * 0.5f;
    dl->AddRectFilled(ImVec2(lineX, lineY), ImVec2(lineX + lineW, lineY + 3.0f),
                      ImGui::GetColorU32(withAlpha(mix(accentA, accentB, 0.42f), 0.78f * overlay)), 2.0f);
    return true;
}

// WebUI 专用：只画暗色遮罩淡出（平滑 WebView2→主界面过渡），不画文字卡片
static bool DrawStartupEntryScrimOnly(float elapsedMs, float durationMs) {
    const float t = GetStartupEntryTransitionT(elapsedMs, durationMs);
    const float eased = EaseStartupEntry(t);
    const float overlay = 1.0f - eased;
    if (overlay <= 0.001f) return false;

    ImGuiIO& io = ImGui::GetIO();
    const float W = io.DisplaySize.x, H = io.DisplaySize.y;
    if (W <= 1.0f || H <= 1.0f) return false;

    ImVec4 bg = ImGui::GetStyle().Colors[ImGuiCol_WindowBg];
    ImVec4 scrim = ImVec4(bg.x * 0.82f, bg.y * 0.82f, bg.z * 0.82f, overlay);
    ImGui::GetForegroundDrawList()->AddRectFilled(ImVec2(0, 0), ImVec2(W, H), ImGui::GetColorU32(scrim));
    return true;
}

// Close WebView2 instances and release resources.
static void CloseWebView2(ComPtr<ICoreWebView2Controller>& controller,
                           ComPtr<ICoreWebView2>& webview,
                           HMODULE& wvLoader) {
    if (controller) {
        controller->put_IsVisible(FALSE);
        controller->Close();
        controller.Reset();
    }
    if (webview) {
        webview.Reset();
    }
    if (wvLoader) {
        FreeLibrary(wvLoader);
        wvLoader = nullptr;
    }
}

} // namespace
#endif

// Windows Entry Point

// Dev / file-player startup bypass for login and preload flow.
bool g_isDevModeStartupBypass = false;

static std::string TryResolveStartupAudioFilePathFromCommandLine() {
    std::string cmdPath;
    const auto argument = Platform::ReadProcessArgument(1);
    if (argument.status == Platform::ProcessArgumentStatus::Success) {
        std::wstring wpath = argument.value;
        cmdPath = WideToUtf8(wpath);
        if (cmdPath.size() >= 2 && cmdPath.front() == '"' && cmdPath.back() == '"') {
            cmdPath = cmdPath.substr(1, cmdPath.size() - 2);
        }
        if (cmdPath.empty() || !PathExistsUtf8(cmdPath) || !IsAudioFileSupported(cmdPath)) {
            cmdPath.clear();
        }
    }
    return cmdPath;
}


#if (defined(ARIS_UPDATE_LOCAL_DEMO) && ARIS_UPDATE_LOCAL_DEMO) || \
    defined(ARIS_UPDATE_QA_CLI)
static const char* DemoCliPhaseName(aris::update::HarkenSearchUpdateUiPhase phase) {
    using Phase = aris::update::HarkenSearchUpdateUiPhase;
    switch (phase) {
        case Phase::Idle: return "idle";
        case Phase::Checking: return "checking";
        case Phase::Downloading: return "downloading";
        case Phase::Ready: return "ready";
        case Phase::Restarting: return "restarting";
        case Phase::Error: return "error";
        default: return "disabled";
    }
}

static void PrintDemoCliState(const char* command, const aris::update::HarkenSearchUpdateUiState& state,
                              bool accepted = false, bool confirmAccepted = false,
                              const char* appliedDisposition = "", bool gracefulObserved = false) {
    std::printf("{\"schema\":1,\"command\":\"%s\",\"status\":\"%s\",\"currentVersion\":\"%s\",\"candidateVersion\":\"%s\",\"downloadedBytes\":%llu,\"totalBytes\":%llu,\"accepted\":%s,\"confirmAccepted\":%s,\"appliedDisposition\":\"%s\",\"appliedDetail\":\"%s\",\"gracefulObserved\":%s,\"error\":\"%s\"}\n",
                command, DemoCliPhaseName(state.phase), state.currentVersion.c_str(),
                state.candidateVersion.c_str(),
                static_cast<unsigned long long>(state.downloadedBytes),
                static_cast<unsigned long long>(state.totalBytes), accepted ? "true" : "false",
                confirmAccepted ? "true" : "false", appliedDisposition, state.lastError.c_str(),
                gracefulObserved ? "true" : "false", state.lastError.c_str());
}

static int RunHarkenSearchDemoCli(bool integrityFailed) {
    const auto flag = Platform::ReadProcessArgument(1);
#if defined(ARIS_UPDATE_QA_CLI)
    constexpr const char* expectedFlag = "--aris-update-qa-cli";
#else
    constexpr const char* expectedFlag = "--harken-search-demo-cli";
#endif
    if (flag.status != Platform::ProcessArgumentStatus::Success ||
        WideToUtf8(flag.value) != expectedFlag) {
        return -1;
    }
    if (integrityFailed) {
        std::printf("{\"schema\":1,\"command\":\"preflight\",\"status\":\"integrity_rejected\"}\n");
        return 3;
    }
    const auto commandArg = Platform::ReadProcessArgument(2);
    const std::string command = commandArg.status == Platform::ProcessArgumentStatus::Success
        ? WideToUtf8(commandArg.value) : std::string();
    if (command != "check" && command != "status" && command != "health" &&
        command != "confirm" && command != "restart" && command != "run") {
        std::printf("{\"schema\":1,\"command\":\"%s\",\"status\":\"invalid_command\"}\n", command.c_str());
        return 2;
    }
    aris::update::StartHarkenSearchAutomaticUpdate();
    if (command == "check" || command == "confirm" || command == "restart" ||
        command == "run") {
        (void)aris::update::RequestHarkenSearchUpdateCheck();
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(120);
    aris::update::HarkenSearchUpdateUiState state = aris::update::GetHarkenSearchUpdateUiState();
    bool observedUpdateWork = false;
    while (std::chrono::steady_clock::now() < deadline) {
        state = aris::update::GetHarkenSearchUpdateUiState();
        observedUpdateWork = observedUpdateWork ||
            state.phase == aris::update::HarkenSearchUpdateUiPhase::Checking ||
            state.phase == aris::update::HarkenSearchUpdateUiPhase::Downloading;
        if ((command == "status" || command == "health") &&
            state.phase != aris::update::HarkenSearchUpdateUiPhase::Disabled) break;
        if (state.phase == aris::update::HarkenSearchUpdateUiPhase::Ready ||
            state.phase == aris::update::HarkenSearchUpdateUiPhase::Error ||
            (observedUpdateWork && state.phase == aris::update::HarkenSearchUpdateUiPhase::Idle)) break;
        Platform::SleepMilliseconds(100);
    }
    if (command == "confirm" || command == "restart" || command == "run") {
        if (state.phase != aris::update::HarkenSearchUpdateUiPhase::Ready) {
            PrintDemoCliState(command.c_str(), state);
            return 1;
        }
        const bool queued = aris::update::ConfirmHarkenSearchUpdateRestart();
        if (!queued) {
            PrintDemoCliState(command.c_str(), aris::update::GetHarkenSearchUpdateUiState(), false, false,
                              "ConfirmRejected", false);
            return 1;
        }
        while (std::chrono::steady_clock::now() < deadline) {
            if (aris::update::ConsumeHarkenSearchUpdateGracefulExitRequest()) {
                PrintDemoCliState(command.c_str(), aris::update::GetHarkenSearchUpdateUiState(), true, true,
                                  "BootstrapAccepted", true);
                return 0;
            }
            state = aris::update::GetHarkenSearchUpdateUiState();
            if (state.phase == aris::update::HarkenSearchUpdateUiPhase::Error) {
                PrintDemoCliState(command.c_str(), state, false, true, "BootstrapFailed", false);
                return 1;
            }
            Platform::SleepMilliseconds(100);
        }
        PrintDemoCliState(command.c_str(), aris::update::GetHarkenSearchUpdateUiState(), false, true,
                          "Timeout", false);
        return 1;
    }
    PrintDemoCliState(command.c_str(), state);
    return state.phase == aris::update::HarkenSearchUpdateUiPhase::Error ? 1 : 0;
}
#endif

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int nCmdShow) {
    Platform::HarkenUserData::ApplyPreviewIsolationProcessEnvironment();
    // Capture the one-shot bootstrap challenge before UI startup. The health
    // acknowledgement remains tied to the first presented main UI frame.
    // RunHarkenSearchBootstrapHealthAcknowledgement remains a compatibility symbol.
    const bool bootstrapHealthSuccessor =
        aris::update::CaptureHarkenSearchBootstrapHealthChallenge();

// EXE integrity check (before license validation)
    // Compare SHA-256 of exe against sidecar .sig file
    bool g_integrityCheckFailed = false;
    {
        const auto executablePath = Platform::GetExecutablePath();
        const std::wstring exePathBuf =
            executablePath.status == Platform::ExecutablePathStatus::Success
                ? Utf8ToWide(executablePath.pathUtf8)
                : std::wstring{};
        std::wstring sigPathW = exePathBuf + L".sig";

        FILE* fSig = nullptr;
        _wfopen_s(&fSig, sigPathW.c_str(), L"rb");
        if (fSig) {
            unsigned char expectedHash[32];
            if (fread(expectedHash, 1, 32, fSig) == 32) {
                fclose(fSig);
                FILE* fExe = nullptr;
                _wfopen_s(&fExe, exePathBuf.c_str(), L"rb");
                if (fExe) {
                    fseek(fExe, 0, SEEK_END);
                    long exeSize = ftell(fExe);
                    fseek(fExe, 0, SEEK_SET);
                    std::vector<unsigned char> exeData((size_t)exeSize);
                    fread(exeData.data(), 1, (size_t)exeSize, fExe);
                    fclose(fExe);
                    // SHA-512 via tweetnacl, truncate to 32 bytes for comparison
                    unsigned char fullHash[64];
                    crypto_hash(fullHash, exeData.data(), (unsigned long long)exeSize);
                    if (memcmp(fullHash, expectedHash, 32) != 0) {
                        g_integrityCheckFailed = true;
                    }
                }
            } else {
                fclose(fSig);
            }
        }
        // No .sig file → dev/debug mode, skip check silently
    }

#if defined(ARIS_UPDATE_QA_CLI)
    // QA-only entry: run the update CLI after the integrity check. The exact
    // flag is matched inside RunHarkenSearchDemoCli; wrong or missing flags fall
    // through to normal product startup.
    {
        const int earlyCliResult = RunHarkenSearchDemoCli(g_integrityCheckFailed);
        if (earlyCliResult >= 0) return earlyCliResult;
    }
#endif

#if AURA_BUILD_DIAGNOSTICS && ARIS_PREVIEW_ISOLATION && !AURA_INTERNAL_BETA
    // Preview diagnostics stay under the compile-time isolated root.
    const std::string dockTracePath = GetHarkenSearchRuntimePathUtf8("dock_drag_trace.json");
    const bool dockTraceRequested = GetHarkenSearchEnvFlag("ARIS_DOCK_TRACE") &&
        IsHarkenSearchHarnessMultiInstanceEnabled() &&
        Platform::HarkenUserData::PreviewIsolationAllowsDataPathUtf8(dockTracePath);
#endif


    // Parse command-line to detect audio file so we can pass it to existing instance.
    std::wstring ipcFilePath;
    {
        const auto argument = Platform::ReadProcessArgument(1);
        if (argument.status == Platform::ProcessArgumentStatus::Success) {
            const std::wstring& wp = argument.value;
            std::string p8 = WideToUtf8(wp);
            if (p8.size() >= 2 && p8.front() == '"' && p8.back() == '"')
                p8 = p8.substr(1, p8.size() - 2);
            if (IsAudioFileSupported(p8))
                ipcFilePath = wp;
        }
    }
    const std::string startupAudioFilePathUtf8 = ipcFilePath.empty() ? std::string() : WideToUtf8(ipcFilePath);
    const bool startupBypassForDirectFilePlayback = !startupAudioFilePathUtf8.empty();

    const std::wstring auroWindowClassName = GetHarkenSearchWindowClassNameW();
    HANDLE hMutex = nullptr;
    if (ShouldUseSingleInstanceBroker() && !bootstrapHealthSuccessor) {
        const std::wstring auroMutexName = GetHarkenSearchMutexNameW();
        hMutex = CreateMutexW(nullptr, FALSE, auroMutexName.c_str());
        if (hMutex && Platform::LastSystemError() == ERROR_ALREADY_EXISTS) {
            HWND existingWnd = FindWindowW(auroWindowClassName.c_str(), nullptr);
            if (existingWnd) {
                const DWORD existingPid = static_cast<DWORD>(Platform::WindowProcessId(existingWnd));
                if (existingPid != 0) {
                    AllowSetForegroundWindow(existingPid);
                } else {
                    AllowSetForegroundWindow(ASFW_ANY);
                }

                    if (!ipcFilePath.empty()) {
                        COPYDATASTRUCT cds{};
                        cds.dwData = AURO_IPC_OPEN_FILE;
                        cds.cbData = (DWORD)((ipcFilePath.size() + 1) * sizeof(wchar_t));
                        cds.lpData = (void*)ipcFilePath.c_str();
                        SendMessageTimeoutW(existingWnd, WM_COPYDATA, (WPARAM)existingWnd, (LPARAM)&cds,
                                            SMTO_ABORTIFHUNG | SMTO_BLOCK, 2000, nullptr);
                    } else {
                        SendMessageTimeoutW(existingWnd, AURO_WM_SHOW_MANAGER, 0, 0,
                                            SMTO_ABORTIFHUNG | SMTO_BLOCK, 2000, nullptr);
                    }
                    (void)Platform::WindowSetForeground(existingWnd);
                }
                Platform::CloseNativeHandle(hMutex);
                return 0;
            }
        }

    // ipcFilePath and hMutex remain in scope for use later in WinMain.

#if (defined(ARIS_UPDATE_LOCAL_DEMO) && ARIS_UPDATE_LOCAL_DEMO) || \
    defined(ARIS_UPDATE_QA_CLI)
    const int demoCliResult = RunHarkenSearchDemoCli(g_integrityCheckFailed);
    if (demoCliResult >= 0) return demoCliResult;
#endif

    // Open-source startup has no remote authorization gate.
    // Open-source startup: no whitelist, tester registration, or authorization gate.
    RegisterOpenWith();

    // Resolve the persisted diagnostic switch before installing the crash
    // handler.  The handler itself can emit a diagnostic line, so waiting for
    // the later AppState settings load would lose the most important record.
    SyncDiagnosticFileEnabledFromSettings();

    // Register crash cleanup for the tray icon.
    g_prevExceptionFilter = SetUnhandledExceptionFilter(HarkenSearchCrashHandler);

    // ---- 注册 TaskbarCreated 消息（Explorer 重启时恢复托盘图标）----
    WM_TASKBARCREATED = RegisterWindowMessageW(L"TaskbarCreated");

    Platform::InitThreadCom();
    OleInitialize(nullptr);

    // Register window class
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.style = CS_OWNDC;
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon = static_cast<HICON>(LoadImageW(
        hInstance, MAKEINTRESOURCEW(1), IMAGE_ICON,
        256, 256, LR_DEFAULTCOLOR | LR_SHARED));
    if (!wc.hIcon) wc.hIcon = LoadIcon(hInstance, MAKEINTRESOURCE(1));
    wc.hIconSm = static_cast<HICON>(LoadImageW(
        hInstance, MAKEINTRESOURCEW(1), IMAGE_ICON,
        GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON),
        LR_DEFAULTCOLOR | LR_SHARED));
    if (!wc.hIconSm) wc.hIconSm = LoadIcon(hInstance, MAKEINTRESOURCE(1));
    wc.lpszClassName = auroWindowClassName.c_str();
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(
        0, auroWindowClassName.c_str(), Lw(WinTitle_Initial).c_str(),        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 1600, 900,
        nullptr, nullptr, hInstance, nullptr
    );

    if (!hwnd) {
        OleUninitialize();
        Platform::UninitThreadCom();
        return 1;
    }

    g_hwnd = hwnd;

    aris::update::StartHarkenSearchAutomaticUpdate();

    // Force the final brand icon onto the actual window. Relying only on the
    // class icon can leave Windows taskbar/titlebar surfaces showing a cached
    // legacy icon cached for a pinned executable.
    if (wc.hIcon) {
        SendMessageW(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(wc.hIcon));
    }
    if (wc.hIconSm) {
        SendMessageW(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(wc.hIconSm));
    }

    // Setup OpenGL
    HDC hdc;
    HGLRC hglrc;
    if (!CreateOpenGLContext(hwnd, &hdc, &hglrc)) {
        Platform::ShowMessageBox(hwnd, Lw(MB_GlContextFail).c_str(), Lw(MB_Title_Error).c_str(), MB_OK);
        OleUninitialize();
        Platform::UninitThreadCom();
        return 1;
    }

    // Enable VSync
    wglSwapIntervalEXT = (PFNWGLSWAPINTERVALEXTPROC)wglGetProcAddress("wglSwapIntervalEXT");
    if (wglSwapIntervalEXT) {
        wglSwapIntervalEXT(1);
    }

    // Enable drag and drop
    {
        HMODULE user32 = GetModuleHandleW(L"user32.dll");
        if (user32) {
            using ChangeFilterExFn = BOOL(WINAPI*)(HWND, UINT, DWORD, PCHANGEFILTERSTRUCT);
            auto pChangeWindowMessageFilterEx =
                (ChangeFilterExFn)GetProcAddress(user32, "ChangeWindowMessageFilterEx");
            if (pChangeWindowMessageFilterEx) {
                pChangeWindowMessageFilterEx(hwnd, WM_DROPFILES, MSGFLT_ALLOW, nullptr);
                pChangeWindowMessageFilterEx(hwnd, WM_COPYGLOBALDATA, MSGFLT_ALLOW, nullptr);
            } else {
                using ChangeFilterFn = BOOL(WINAPI*)(UINT, DWORD);
                auto pChangeWindowMessageFilter =
                    (ChangeFilterFn)GetProcAddress(user32, "ChangeWindowMessageFilter");
                if (pChangeWindowMessageFilter) {
                    pChangeWindowMessageFilter(WM_DROPFILES, 1);
                    pChangeWindowMessageFilter(WM_COPYGLOBALDATA, 1);
                }
            }
        }
    }
    DragAcceptFiles(hwnd, TRUE);

    // Don't show the window yet — keep it hidden until the splash screen
    // (RunBetaSplashScreen) switches to borderless popup and shows via SetWindowPos.
    // This avoids the flash of the main window frame before the splash appears.
    // Platform::WindowShow(hwnd, nCmdShow);  // deferred to splash / loading screen
    UpdateWindow(hwnd);

    // Setup ImGui
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    // io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;  // 禁用：左右方向键不再触发焦点框移动
#if ARIS_ENABLE_IMGUI_VIEWPORTS
    io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
#endif
    // The product uses its own DockLayout. Keep ImGui multi-viewports for
    // floating windows without activating the separate native docking layer.

    static bool s_isDevMode = false;
    static bool s_isIngestMode = false;
    {
        std::string exeDirUtf8 = GetExeDirUtf8();
#if AURA_INTERNAL_BETA
        // Internal beta ignores local developer and ingest bypass inputs.
        s_isDevMode = false;
        s_isIngestMode = false;
        g_isDevModeStartupBypass = startupBypassForDirectFilePlayback;
#else
        // 检查是否存在 dev_mode.txt 标记文件
        s_isDevMode = PathExistsUtf8(exeDirUtf8 + "\\dev_mode.txt");
        char* ingestEnv = nullptr;
        size_t ingestEnvLen = 0;
        if (_dupenv_s(&ingestEnv, &ingestEnvLen, "AURA_INGEST_MODE") == 0 && ingestEnv) {
            s_isIngestMode = ingestEnv[0] != '\0' && std::string(ingestEnv) != "0";
            std::free(ingestEnv);
        } else {
            s_isIngestMode = false;
        }
        g_isDevModeStartupBypass = s_isDevMode || startupBypassForDirectFilePlayback;
#endif

        // Development mode keeps mutable state beside the executable.
        if (s_isDevMode) {
            const std::string runtimeRootUtf8 = GetHarkenSearchRuntimeRootUtf8();
            g_imguiIniPathUtf8 = runtimeRootUtf8 + "\\imgui_dev.ini";
            g_appSettingsPathUtf8 = runtimeRootUtf8 + "\\settings_dev.ini";
            // LoadAppSettings resolves the database path after this directory exists.
            if (!Platform::FileSystem::EnsureDirectoryExists(runtimeRootUtf8 + "\\database")) {
                throw std::runtime_error("Failed to create development database directory");
            }
        } else {
            const auto layout = Platform::HarkenUserData::EnsureLayout();
            if (!layout.activeSettingsDir.empty()) {
                std::string appDataRoot = Platform::HarkenUserData::WideToUtf8Path(layout.activeSettingsDir);
                if (!Platform::FileSystem::EnsureDirectoryExists(appDataRoot)) {
                    throw std::runtime_error("Failed to create user settings directory");
                }
                g_imguiIniPathUtf8 = appDataRoot + "\\imgui.ini";
                g_appSettingsPathUtf8 = appDataRoot + "\\settings.ini";
                if (layout.readOnly && !layout.noticeUtf8.empty()) {
                    const std::wstring notice = Utf8ToWide(layout.noticeUtf8);
                    Platform::ShowMessageBox(nullptr, notice.c_str(), L"Harken Search", MB_OK | MB_ICONWARNING);
                }
            } else {
                g_imguiIniPathUtf8 = exeDirUtf8 + "\\imgui.ini";
                g_appSettingsPathUtf8 = exeDirUtf8 + "\\settings.ini";
            }
        }
        // Both branches select the authoritative settings file.
        // Correct the early best-effort probe before emitting any path-dependent log.
        SyncDiagnosticFileEnabledFromSettings();
        if (s_isDevMode) {
            AppendDiagLogLine(L"SYSTEM: DevMode active. Database and Config locked to local folder.");
        }

        io.IniFilename = g_imguiIniPathUtf8.c_str();
        DiagnosticsLogger::instance().setOutputPath(GetHarkenSearchRuntimePathUtf8("aura_diagnostics_live.jsonl"));
        UserActionLog::instance().setOutputPath(GetHarkenSearchRuntimePathUtf8("user_actions.log"));
        UserActionLog::instance().setEnabled(true);
        UA_LOG(UACategory::System, "Session start");
    }

    // Load a baseline font first; actual size will be applied after settings are loaded (Scheme B).
    LoadChineseFont(io, (float)kChromeFontPx, 16.0f, 16.0f);

    ImGui::StyleColorsDark();
    ApplyTheme(AppTheme::Dark);
#if ARIS_ENABLE_IMGUI_VIEWPORTS
    if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
        ImGuiStyle& style = ImGui::GetStyle();
        style.WindowRounding = 0.0f;
        style.Colors[ImGuiCol_WindowBg].w = 1.0f;
    }
#endif

    ImGui_ImplWin32_InitForOpenGL(hwnd);
    ImGui_ImplOpenGL3_Init("#version 130");
#if ARIS_ENABLE_IMGUI_VIEWPORTS
    if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
        ImGuiPlatformIO& platform_io = ImGui::GetPlatformIO();
        IM_ASSERT(platform_io.Renderer_CreateWindow == nullptr);
        IM_ASSERT(platform_io.Renderer_DestroyWindow == nullptr);
        IM_ASSERT(platform_io.Renderer_SwapBuffers == nullptr);
        IM_ASSERT(platform_io.Platform_RenderWindow == nullptr);
        platform_io.Renderer_CreateWindow = Hook_Renderer_CreateWindow;
        platform_io.Renderer_DestroyWindow = Hook_Renderer_DestroyWindow;
        platform_io.Renderer_SwapBuffers = Hook_Renderer_SwapBuffers;
        platform_io.Platform_RenderWindow = Hook_Platform_RenderWindow;
    }
#endif

    // 立即画一帧深色背景，避免启动白屏
    {
        const ImVec4 clearBg = GetThemeClearColor();
        glClearColor(clearBg.x, clearBg.y, clearBg.z, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        SwapBuffers(hdc);
    }

    // Initialize application state
    AppState state;
    g_appState = &state;
    state.panelSimilarSounds.visible = false;
    if (startupBypassForDirectFilePlayback) {
        state.pendingOpenFilePath = startupAudioFilePathUtf8;
        state.filePlayerFastPathActive = true;
        AppendDiagLogF(L"CMDLINE: Startup direct-play audio file: %hs", startupAudioFilePathUtf8.c_str());
    }
    state.integrityCheckFailed = g_integrityCheckFailed;
    if (g_integrityCheckFailed) ProductEdition::setIntegrityCheckFailed();
    AppendDiagLogLine(L"STARTUP: before LoadAudioSettingsEarly");
    LoadAudioSettingsEarly(state);
    AppendDiagLogLine(L"STARTUP: after LoadAudioSettingsEarly");
    AppendDiagLogLine(L"STARTUP: before audioEngine.initialize");
    if (!state.audioEngine.initialize(state.settings.audioPeriodSizeMs)) {
        AppendDiagLogLine(L"STARTUP: audioEngine.initialize failed");
        Platform::ShowMessageBox(hwnd, Lw(MB_AudioInitFail).c_str(), Lw(MB_Title_Error).c_str(), MB_OK);
        OleUninitialize();
        Platform::UninitThreadCom();
        return 1;
    }
    AppendDiagLogLine(L"STARTUP: after audioEngine.initialize");
    AppendDiagLogLine(L"STARTUP: before LoadAppSettings");
    LoadAppSettings(state);
    AppendDiagLogLine(L"STARTUP: after LoadAppSettings");

// F-035: Crash sentinel — detect previous abnormal exit
    {
        std::filesystem::path settingsDir = std::filesystem::path(g_appSettingsPathUtf8).parent_path();
        std::string sentinelPath = (settingsDir / "aris_sentinel.lock").u8string();
        if (PathExistsUtf8(sentinelPath)) {
            state.crashDetectedOnStartup = true;
            DIAG_SYSTEM("Sentinel", "Previous session did not shut down cleanly.");
        }
        // Write sentinel for this session
        Platform::FileSystem::WriteFileText(sentinelPath,
                                            "pid=" + std::to_string(Platform::CurrentProcessId()) + "\n");
    }

    // Settings loading may replace the path, so resolve the development database again.
    if (s_isDevMode) {
        state.currentDbPathUtf8 = ResolveCanonicalHarkenSearchDatabasePathUtf8(state.currentDbPathUtf8);
        DIAG_SYSTEM("DevMode", "Database path resolved through canonical Harken Search storage.");
    }

    // --- 处理命令行参数：音频文件 → 文件浏览器打开+播放；文件夹 → 扫描对话框 ---
    {
        const auto argument = Platform::ReadProcessArgument(1);
        if (argument.status == Platform::ProcessArgumentStatus::Success) {
            const std::wstring& wpath = argument.value;
            std::string cmdPath = WideToUtf8(wpath);
            // Strip surrounding quotes if present (defensive; the platform parser normally removes them).
            if (cmdPath.size() >= 2 && cmdPath.front() == '"' && cmdPath.back() == '"') {
                cmdPath = cmdPath.substr(1, cmdPath.size() - 2);
            }
            if (!cmdPath.empty() && PathExistsUtf8(cmdPath)) {
                    if (IsAudioFileSupported(cmdPath)) {
                        if (state.pendingOpenFilePath.empty()) {
                            // Audio file: navigate file browser and play — no scan dialog.
                            state.pendingOpenFilePath = cmdPath;
                            state.filePlayerFastPathActive = true;
                            AppendDiagLogF(L"CMDLINE: Open audio file in browser: %hs", cmdPath.c_str());
                        }
                } else {
                    // Directory or other path: existing scan-dialog behavior.
                    state.pendingScanFolders.push_back(cmdPath);
                    state.scanDialogAutoStart = false;
                    state.showScanDialog = true;
                    AppendDiagLogF(L"CMDLINE: Received path for scanning: %hs", cmdPath.c_str());
                }
            }
        }
    }

    ApplyTheme(state.settings.theme);
    if (state.settings.useCustomColors) {
        ApplyCustomColors(state.settings);
    }
    HarkenSearchSplash::ConfigureThemePalette(state.settings);
    ApplyShowManagerGlobalHotkey(state.settings);
    
    // Try to clean stale tray icon left by previous crash.
    {
        NOTIFYICONDATAW cleanupNid = {};
        cleanupNid.cbSize = sizeof(cleanupNid);
        cleanupNid.hWnd = g_hwnd;
        cleanupNid.uID = 1;
        Shell_NotifyIconW(NIM_DELETE, &cleanupNid);
    }
    EnsureTrayIconAdded(hwnd);
    
    // These cleanup owners are shared by both the WebView2 and fallback builds.
    // Keep their declarations outside the WebView2 guard so the common shutdown
    // tail can always stop the publisher and record its stages.
    AnnotationBridge::Publisher annotationPublisher;
    auto logShutdownStage = [](const char* stage) {
        DIAG_SYSTEM_FMT("Shutdown", "Stage:%s", stage);
        const std::wstring wideStage = Platform::TextEncoding::Utf8ToWidePermissive(stage);
        AppendDiagLogF(L"Shutdown Stage:%s", wideStage.c_str());
    };

    // Keep WebView2 instances across login/loading phases.
#if SM_HAS_WEBVIEW2
    Microsoft::WRL::ComPtr<ICoreWebView2Controller> wv2Controller;
    Microsoft::WRL::ComPtr<ICoreWebView2> wv2Webview;
    HMODULE wv2Loader = nullptr;

    // Startup login runs in WebUI unless a reviewed bypass applies.
    {
        bool loginDone = false;
        const bool bypassStartupLogin =
            s_isIngestMode || IsHarkenSearchHarnessMultiInstanceEnabled();
        AppendDiagLogLine(L"STARTUP: before login phase");
        if (ShouldOpenHarkenSearchGalleryFromEnv()) {
            loginDone = true;
            g_loginPhaseActive = false;
            AppendDiagLogLine(L"STARTUP: login skipped for gallery");
            DIAG_SYSTEM("Login", "Harken Search UI Gallery dev screenshot mode skipped startup login.");
        } else if (startupBypassForDirectFilePlayback) {
            loginDone = true;
            g_loginPhaseActive = false;
            AppendDiagLogLine(L"STARTUP: login skipped for direct file playback");
            DIAG_SYSTEM("FilePlayer", "Startup login skipped for direct file playback.");
        } else if (bypassStartupLogin) {
            loginDone = true;
            g_loginPhaseActive = false;
            AppendDiagLogLine(L"STARTUP: login skipped for harness-ingest");
            DIAG_SYSTEM(s_isIngestMode ? "IngestMode" : "Harness",
                        s_isIngestMode
                            ? "Startup login skipped for dedicated scan ingestion."
                            : "Startup login skipped for harness lane.");
        } else if (!kShowStartupLogin && kUseBetaSplash) {
            loginDone = true;
            g_loginPhaseActive = false;
            AppendDiagLogLine(L"STARTUP: login skipped for beta splash");
            DIAG_SYSTEM("Splash", "Beta splash — login skipped.");
        } else {
#if SM_HAS_WEBVIEW2
            // 强制 WebUI 登录：验证通过后才进入加载阶段
            g_loginPhaseActive = true;
            AppendDiagLogLine(L"STARTUP: login webview begin");
            DIAG_SYSTEM("Startup", s_isDevMode ? "Startup welcome entry shown in dev mode." : "Startup welcome entry shown.");
            loginDone = RunWebView2LoginScreen(hwnd, state.settings, &wv2Controller, &wv2Webview, &wv2Loader);
            g_loginPhaseActive = false;
            AppendDiagLogLine(loginDone ? L"STARTUP: login webview complete" : L"STARTUP: login webview failed");
            if (!loginDone) {
                Platform::ShowMessageBox(hwnd, Lw(MB_LoginIncomplete).c_str(), Lw(MB_Title_Aris).c_str(), MB_OK | MB_ICONWARNING);
                return 0;
            }
#else
            Platform::ShowMessageBox(hwnd, Lw(MB_NoWebView2).c_str(), Lw(MB_Title_Aris).c_str(), MB_OK | MB_ICONERROR);            return 0;
#endif
        }
        AppendDiagLogLine(L"STARTUP: after login phase");

    }

    {
        // 数据库异步初始化失败标记（后台线程设置，UI 线程读取）
        std::atomic<bool> dbOpenFailed{ false };
        std::mutex dbOpenMutex;
        std::string dbOpenErrorUtf8;
        std::string dbOpenPathUtf8;
#if SM_HAS_WEBVIEW2
            // 将启动阶段重负载加载逻辑迁移到 WebUI Loading（真实进度 + 阶段文案）
            auto runStartupPhases = [&](const std::function<void(int,int,const std::string&)>& progressCb) {
                // 同步诊断开关（用于记录真实加载阶段）
                DiagnosticsLogger::instance().setEnabled(state.settings.diagnosticMode);
                SetDiagnosticFileEnabled(s_isDevMode || state.settings.diagnosticMode);
                DIAG_SYSTEM("App", "StartupLoadingStart");
                const bool filePlayerFastPath =
                    state.filePlayerFastPathActive && !state.pendingOpenFilePath.empty();

            auto report = [&](int loaded, int total, const char* msg) {
                progressCb(loaded, total, msg ? std::string(msg) : std::string());
            };

            if (filePlayerFastPath) {
                report(0, 1, L(Splash_StageOpenAudio));
                if (state.listTabs.empty()) {
                    state.createNewTab(u8"全部文件");
                }
                state.preloadedCoverWallEntries.clear();
                state.preloadedCoverWallFullyDiscovered = false;
                DIAG_SYSTEM("FilePlayer",
                            "Direct file playback startup: skipped database mount, search refresh, and library warmup.");
                report(1, 1, L(Splash_StageOpenAudio));
                return;
            }

            constexpr int kStartupPhaseTotal = 7;

// Phase 0: 挂载数据库（异步执行，避免登录后主线程阻塞）
            report(0, kStartupPhaseTotal, L(Splash_StageMountIntegrity));
            {
                std::string dbPathUtf8;
                AppState::StartupDatabaseSetupReason startupDbReason =
                    AppState::StartupDatabaseSetupReason::None;
                if (!ResolveStartupDatabaseSelectionForMount(
                        state, dbPathUtf8, startupDbReason)) {
                    state.startupNeedsDatabaseSetup = true;
                    state.startupDatabaseSetupReason = startupDbReason;
                    state.currentDbPathUtf8.clear();
                    DIAG_SYSTEM(
                        "Startup",
                        "No database available on startup; waiting for explicit user setup.");
                    report(1, kStartupPhaseTotal, L(Splash_StageWaitDb));
                    return;
                }

                state.startupNeedsDatabaseSetup = false;
                state.startupDatabaseSetupReason =
                    AppState::StartupDatabaseSetupReason::None;
                const auto dataLayout = Platform::HarkenUserData::EnsureLayout();
                bool opened = dataLayout.readOnly
                    ? state.database.openReadOnly(dbPathUtf8)
                    : state.database.open(dbPathUtf8, true);
                std::string lastErr = opened ? "" : state.database.getLastError();

                if (!opened) {
                    std::lock_guard<std::mutex> lock(dbOpenMutex);
                    dbOpenFailed.store(true, std::memory_order_release);
                    dbOpenPathUtf8 = dbPathUtf8;
                    dbOpenErrorUtf8 = lastErr;
                    return;
                }

                state.currentDbPathUtf8 = dbPathUtf8;
                state.backfillQueue.onDbOpened(dbPathUtf8, ++state.backfillDbGeneration);
                PushRecentDbPath(state, dbPathUtf8);
                CoverModule::SetDatabase(&state.database);
                CoverModule::SetAppState(&state);  // 设置 AppState 指针，用于访问性能模式设置
                state.database.setSettings(&state.settings);  // 设置性能模式访问器
                state.musicQueue.init(state.database.db());
                state.gaplessEngine.init(&state.audioEngine);

                // Configure cover cache directory (filesystem sharded storage).
                {
                    try {
                        fs::path dbPath = Utf8ToPath(dbPathUtf8);
                        fs::path cacheDir = dbPath.parent_path() / "Cache";
                        std::string cacheDirUtf8 = PathToUtf8(cacheDir);
                        CoverModule::SetCoverCacheDir(cacheDirUtf8);
                    } catch (...) {
                        // Ignore and keep default behavior.
                    }
                }

                state.backfillQueue.rebuildFromDatabase(&state.database, 4096);
                aura::GlobalBackfillWorkerPool().start(
                    dbPathUtf8, state.backfillDbGeneration, &state.backfillQueue,
                    aura::RecommendedBackfillWorkerThreads());

// 初始化内存预算管理器
                if (!state.memoryBudget) {
                    state.memoryBudget = new MemoryBudget();
                    MemoryBudget::BudgetConfig config;
                    config.metadataBudget = 500ULL * 1024 * 1024;  // 500MB
                    config.waveformBudget = 500ULL * 1024 * 1024; // 500MB
                    config.coverBudget = 1024ULL * 1024 * 1024;   // 1GB
                    config.totalBudget = 2ULL * 1024 * 1024 * 1024; // 2GB
                    state.memoryBudget->setBudgetConfig(config);

                    // 设置到CoverModule
                    CoverModule::SetMemoryBudget(state.memoryBudget);
                }

            }
            report(1, kStartupPhaseTotal, L(Splash_StageMountIntegrity));
            report(1, kStartupPhaseTotal, L(Splash_StagePrepLib));
            if (state.listTabs.empty()) {
                state.createNewTab(u8"全部文件");
            }
            ResetDatabaseState(state);
            if (s_isIngestMode) {
                DIAG_SYSTEM("IngestMode", "Skipping startup RefreshSearchResults before MainUIReady.");
            } else if (filePlayerFastPath) {
                DIAG_SYSTEM("FilePlayer", "Skipping startup search refresh for direct file playback.");
            } else {
                RefreshSearchResults(state, true);
            }
            report(2, kStartupPhaseTotal, L(Splash_StagePrepLib));

// Phase 2: 预热 — 让主界面进来就可用
            if (s_isIngestMode) {
                DIAG_SYSTEM("IngestMode", "Skipping interactive warmup for dedicated scan ingestion.");
            } else if (filePlayerFastPath) {
                report(2, kStartupPhaseTotal, L(Splash_StageOpenAudio));
                state.preloadedCoverWallEntries.clear();
                state.preloadedCoverWallFullyDiscovered = false;
                DIAG_SYSTEM("FilePlayer", "Skipping DB/cover/waveform warmup for direct file playback.");
                report(3, kStartupPhaseTotal, L(Splash_StageOpenAudio));
            } else {
                report(2, kStartupPhaseTotal, L(Splash_StageBgWorkers));
                {
                    // Startup must not block the first interactive frame on
                    // database/cover preheat. Search/cover workers can warm
                    // indexes after the QA/main UI loop is already pumping.
                    DIAG_SYSTEM("Startup", "Database and cover index preheat deferred until after MainUIReady.");

                    // T-296: 自动重扫 watched roots，捕获关闭期间发生的文件变化。
                    // 使用现有 watcher rescan worker 异步执行，不阻塞启动。
                    if (state.settings.autoReconcileOnStartup) {
                        int n = state.database.enqueueStartupReconciliation();
                        DIAG_SYSTEM_FMT("Startup",
                                        "AutoReconcile: queued %d watched roots for rescan",
                                        n);
                    } else {
                        DIAG_SYSTEM("Startup",
                                    "AutoReconcile skipped: setting is opt-in and currently disabled");
                    }

                    // 2.1 启动异步 worker（波形 + 搜索 + BPM），越早启动越早可用
                    state.waveformWorker.start(&state.database, aura::RecommendedWaveformWorkerThreads());
                    state.searchWorker.start(&state.database);
                    state.musicAnalyzer.start(2); // BPM worker threads

                    report(3, kStartupPhaseTotal, L(Splash_StageCoverWarm));

                    // 2.2 封面墙全局索引由主界面异步加载。
                    // 启动页不能执行可能被大库/锁/索引状态放大的 DB 查询，否则会卡在 splash。
                    state.preloadedCoverWallEntries.clear();
                    state.preloadedCoverWallFullyDiscovered = false;
                    DIAG_SYSTEM_FMT("CoverWall",
                                    "StartupPersistentPreload deferred_to_startup_first_screen_preload entries=%zu trusted=%d devMode=%d",
                                    state.preloadedCoverWallEntries.size(),
                                    state.preloadedCoverWallFullyDiscovered ? 1 : 0,
                                    s_isDevMode ? 1 : 0);

                    report(4, kStartupPhaseTotal, L(Splash_StageQueueWave));

                    // 2.3 预加载首屏 mini 波形请求（开发模式下缩小到轻量级）
                    {
                        const size_t preloadCount = std::min<size_t>(state.searchResults.size(), s_isDevMode ? 8 : 50);
                        for (size_t i = 0; i < preloadCount; ++i) {
                            state.addPendingWaveform(state.searchResults[i].filePath, true);
                        }
                        if (s_isDevMode) {
                            DIAG_ACTION_FMT("DevMode", "Light warmup waveform preload count=%zu", preloadCount);
                        }
                    }

                    report(5, kStartupPhaseTotal, L(Splash_StagePlaylists));

                    // 2.4 侧边栏数据（播放列表 + 文件夹树 + 标签云）
                    StartDeferredSidebarBootstrap(state, true, true);

                    // 2.5 随机采样池：依赖搜索结果，由主循环 PumpInitRandomSamplingPoolIfPending 处理
                }
                report(6, kStartupPhaseTotal, L(Splash_StageWarmAll));
            }
            };
#if SM_HAS_WEBVIEW2
            auto finalizeStartupOnUi = [&](const std::function<void(int,int,const std::string&)>& progressCb) {
                const bool filePlayerFastPath =
                    state.filePlayerFastPathActive && !state.pendingOpenFilePath.empty();
                if (filePlayerFastPath) {
                    AppendDiagLogLine(L"STARTUP: finalizeStartupOnUi filePlayerFastPath");
                    progressCb(0, 1, L(Splash_StageOpenAudio));
                    DIAG_SYSTEM("FilePlayer", "Skipping AI engine init for direct file playback.");
                } else if (s_isIngestMode) {
                    AppendDiagLogLine(L"STARTUP: finalizeStartupOnUi ingest fast-path");
                    progressCb(6, 7, L(Splash_StageScanIngest));
                    DIAG_SYSTEM("IngestMode",
                                "Skipping AI engine init and module warmup for dedicated scan ingestion.");
                    progressCb(7, 7, L(Splash_StageAlmostReady));
                    DIAG_SYSTEM("App", "StartupLoadingComplete — entering main UI (ingest mode)");
                    AppendDiagLogLine(L"STARTUP: finalizeStartupOnUi ingest fast-path done");
                    return;
                } else {
                    AppendDiagLogLine(L"STARTUP: finalizeStartupOnUi before AI init");
                    progressCb(6, 7, L(Splash_StageAiInit));
                    AIEngineConfig aiConfig;
                    aiConfig.modelPath = GetDefaultModelDir();
                    aiConfig.modelName = state.aiModelName;
                    aiConfig.defaultLanguage = "zh";
                    aiConfig.maxThreads = 4;

                    if (GetAIEngine().Initialize(aiConfig)) {
                        state.aiEngineInitialized = true;
                        state.aiModelLoaded = GetAIEngine().IsModelLoaded();
                    }
                    AppendDiagLogLine(L"STARTUP: finalizeStartupOnUi after AI init");
                }

                AppendDiagLogLine(L"STARTUP: finalizeStartupOnUi before InitializeModules");
                progressCb(6, 7, L(Splash_StageModules));
                InitializeModules();
                AppendDiagLogLine(L"STARTUP: finalizeStartupOnUi after InitializeModules");

                progressCb(7, 7, L(Splash_StageAlmostReady));
                DIAG_SYSTEM("App", "StartupLoadingComplete — entering main UI");
                AppendDiagLogLine(L"STARTUP: finalizeStartupOnUi complete");
            };
#endif
#endif
#if SM_HAS_WEBVIEW2
            const bool bypassStartupLoadingGate =
                startupBypassForDirectFilePlayback ||
                s_isIngestMode;
            const bool hasStartupWebView = (bool)wv2Controller && (bool)wv2Webview;
            const bool useBetaSplashFallback =
                !startupBypassForDirectFilePlayback &&
                (kUseBetaSplash || !hasStartupWebView);
// 启动门面：beta splash 或 WebView2 加载画面
            if (!bypassStartupLoadingGate && useBetaSplashFallback) {
                CloseWebView2(wv2Controller, wv2Webview, wv2Loader);
                RunBetaSplashScreen(hwnd, hdc, state, runStartupPhases, finalizeStartupOnUi);
                startupEntryTransitionActive = false;
                if (dbOpenFailed.load(std::memory_order_acquire)) {
                    std::lock_guard<std::mutex> lock(dbOpenMutex);
                    std::wstring wDb = Utf8ToWide(dbOpenPathUtf8);
                    std::wstring wErr(dbOpenErrorUtf8.begin(), dbOpenErrorUtf8.end());
                    Platform::ShowMessageBox(hwnd, (Lw(MB_DatabaseErrorPrefix) + wDb + L"\n" + wErr).c_str(), Lw(MB_Title_DatabaseError).c_str(), MB_OK | MB_ICONWARNING);
                    return 0;
                }
            } else if (!bypassStartupLoadingGate && hasStartupWebView) {
                RunWebView2LoadingScreen(hwnd, state, state.settings, wv2Controller, wv2Webview, runStartupPhases, finalizeStartupOnUi);
                // Loading 完成后直接关闭启动门面，避免再出现一次“Entering Library...”过场。
                CloseWebView2(wv2Controller, wv2Webview, wv2Loader);
                startupEntryTransitionActive = false;
                // 若数据库挂载失败，主线程安全弹窗并退出
                if (dbOpenFailed.load(std::memory_order_acquire)) {
                    std::string errUtf8;
                    std::string pathUtf8;
                    {
                        std::lock_guard<std::mutex> lock(dbOpenMutex);
                        errUtf8 = dbOpenErrorUtf8;
                        pathUtf8 = dbOpenPathUtf8;
                    }
                    std::wstring wDb = Utf8ToWide(pathUtf8);
                    std::wstring errorMsg = L"无法打开数据库\n路径: " + wDb + L"\n错误: ";
                    errorMsg += Platform::TextEncoding::Utf8ToWidePermissive(errUtf8);
                    Platform::ShowMessageBox(hwnd, errorMsg.c_str(), Lw(MB_Title_DatabaseError).c_str(), MB_OK | MB_ICONWARNING);
                    return 0;
                }
            } else if (bypassStartupLoadingGate) {
                const char* bypassCategory = startupBypassForDirectFilePlayback
                                                 ? "FilePlayer"
                                                 : "IngestMode";
                const char* bypassDetail = startupBypassForDirectFilePlayback
                                               ? "Startup loading gate bypassed for direct file playback."
                                               : "Startup loading gate bypassed for dedicated scan ingestion.";
                DIAG_SYSTEM(bypassCategory, bypassDetail);
                (void)Platform::WindowShow(hwnd, SW_SHOWMAXIMIZED); // deferred show (no splash for harness/ingest/file-player)
                auto silentProgress = [](int, int, const std::string&) {};
                AppendDiagLogLine(L"STARTUP: bypass branch before runStartupPhases");
                runStartupPhases(silentProgress);
                AppendDiagLogLine(L"STARTUP: bypass branch after runStartupPhases");
                finalizeStartupOnUi(silentProgress);
                AppendDiagLogLine(L"STARTUP: bypass branch after finalizeStartupOnUi");
                startupEntryTransitionActive = false;
                if (dbOpenFailed.load(std::memory_order_acquire)) {
                    std::string errUtf8;
                    std::string pathUtf8;
                    {
                        std::lock_guard<std::mutex> lock(dbOpenMutex);
                        errUtf8 = dbOpenErrorUtf8;
                        pathUtf8 = dbOpenPathUtf8;
                    }
                    std::wstring wDb = Utf8ToWide(pathUtf8);
                    std::wstring errorMsg = L"无法打开数据库\n路径: " + wDb + L"\n错误: ";
                    errorMsg += Platform::TextEncoding::Utf8ToWidePermissive(errUtf8);
                    Platform::ShowMessageBox(hwnd, errorMsg.c_str(), Lw(MB_Title_DatabaseError).c_str(), MB_OK | MB_ICONWARNING);
                    return 0;
                }
            } else
#endif
            {
                Platform::ShowMessageBox(hwnd, Lw(MB_SplashDisabled).c_str(), Lw(MB_Title_Aris).c_str(), MB_OK | MB_ICONERROR);                return 0;
            }
#if !SM_HAS_WEBVIEW2
            bool startupEntryTransitionActive = false;
            auto startupEntryTransitionStart = std::chrono::steady_clock::now();
            constexpr float kStartupEntryTransitionMs = 240.0f;
#endif
        }

        s_mainUiReady.store(false, std::memory_order_release);
// 启动数据兜底
        // 确保 state 与 tab 同步；若 splash 加载后列表为空但数据库有数据，强制刷新
        state.syncFromTab();
        const bool startupSearchIdle =
            state.pendingSearchRequestId == 0 &&
            state.pendingFacetRequestId == 0 &&
            !state.asyncSearchInProgress;
        if (!s_isIngestMode &&
            !state.filePlayerFastPathActive &&
            startupSearchIdle &&
            state.searchResults.empty() &&
            state.database.getFileCount() > 0) {
            // 启动时强制 Database 视图加载首页，确保稳定
            // Tab 的 viewMode/searchBuffer 已恢复，但首次加载必须走 Database 路径
            if (state.viewMode != ViewMode::Database) {
                state.viewMode = ViewMode::Database;
            }
            RefreshSearchResults(state, true);
            state.syncToTab();
        }


        // Splash screen: pre-cache folder covers
        {
            // 大库性能优化：不再启动时全量预缓存封面，改为按需加载
        }

        // Update window title
        std::wstring title;
        if (state.startupNeedsDatabaseSetup && !state.database.isOpen()) {
            title = Lw(WinTitle_NoDatabase);
        } else {
            int64_t fileCount = state.database.getFileCount();
            {
                const std::wstring fmt = Lw(WinTitle_DatabaseFmt);
                const std::wstring dbNameW = Utf8ToWide(GetDatabaseDisplayNameUtf8(state.currentDbPathUtf8));
                wchar_t titleBuf[1024];
                swprintf_s(titleBuf, fmt.c_str(), dbNameW.c_str(), (long long)fileCount);
                title = titleBuf;
            }        }
        SetWindowTextW(hwnd, title.c_str());

    // Setup file browser callback
    state.fileBrowser.setOnFileSelected([&state](const FileEntry& entry) {
        if (!entry.isDirectory) {
            state.audioEngine.loadFileAsync(entry.fullPath, true);
            ScheduleDeferredWaveformDetail(
                state,
                entry.fullPath,
                AppState::WAVEFORM_DEFAULT_DETAIL_PEAKS);
            state.currentFileId = -1;
        }
    });
    state.fileBrowser.setOnDirectoryChanged([&state](const std::string&) {
        state.browserFilterBuffer[0] = '\0';
    });

    // 主题在进入主界面前再应用一次，确保 WebView 启动门面退场后样式已稳定。
    ApplyTheme(state.settings.theme);
    if (state.settings.useCustomColors) {
        ApplyCustomColors(state.settings);
    }
    s_mainUiReady.store(true, std::memory_order_release);
    DIAG_SYSTEM("App", "MainUIReady");
    // Lottie 精灵功能已禁用（见主循环内注释）

    char annotationBridgeEnvironment[8] = {};
    const DWORD annotationBridgeEnvironmentLength = GetEnvironmentVariableA(
        "ARIS_ANNOTATION_BRIDGE",
        annotationBridgeEnvironment,
        static_cast<DWORD>(sizeof(annotationBridgeEnvironment)));
    bool annotationBridgeEnabled =
        annotationBridgeEnvironmentLength > 0 && annotationBridgeEnvironment[0] == '1';
    if (annotationBridgeEnabled) {
        const std::string annotationTempDirectory =
            Platform::FileSystem::TemporaryDirectory();
        annotationBridgeEnabled = !annotationTempDirectory.empty() &&
            annotationPublisher.Start(
                std::filesystem::u8path(annotationTempDirectory) / "HarkenSearchAnnotationBridge",
                10.0);
    }
    bool magicEqQaCollect = false;
#if AURA_BUILD_DIAGNOSTICS && !AURA_INTERNAL_BETA
    magicEqQaCollect = MagicEqQaIsolationReady();
#endif
    ThemeInspector::SetBridgeCollectionEnabled(annotationBridgeEnabled);

    // Main loop
    bool running = true;
    MSG msg = {};
#if AURA_BUILD_DIAGNOSTICS && ARIS_PREVIEW_ISOLATION && !AURA_INTERNAL_BETA
    struct DockTraceSample {
        double frameMs = 0.0, renderUiMs = 0.0;
        double pumpMs = 0.0, glRenderMs = 0.0, presentMs = 0.0;
        aura::ui::dock::DockFrameTrace dock{};
    };
    std::vector<DockTraceSample> dockIdleSamples, dockDragSamples;
    bool dockTraceWritten = false;
    bool dockTraceCompleteAttempted = false;
    if (dockTraceRequested) {
        dockIdleSamples.reserve(120);
        dockDragSamples.reserve(60);
    }
    aura::ui::dock::ActiveDockFrameTrace().enabled = dockTraceRequested;
    auto writeDockTrace = [&](bool complete) {
        if (!dockTraceRequested || dockTraceWritten ||
            (dockIdleSamples.empty() && dockDragSamples.empty())) return;
        std::ostringstream output;
        output << std::setprecision(9)
               << "{\"schema\":1,\"source\":\"preview_after_consent\",\"complete\":"
               << (complete ? "true" : "false") << ",\"idle\":";
        auto writeSamples = [&](const std::vector<DockTraceSample>& samples) {
            output << '[';
            for (size_t i = 0; i < samples.size(); ++i) {
                if (i) output << ',';
                const auto& s = samples[i];
                output << "{\"frameMs\":" << s.frameMs
                       << ",\"renderUiMs\":" << s.renderUiMs
                       << ",\"pumpMs\":" << s.pumpMs
                       << ",\"glRenderMs\":" << s.glRenderMs
                       << ",\"presentMs\":" << s.presentMs
                       << ",\"geometryCalls\":" << s.dock.geometryCalls
                       << ",\"geometryMs\":" << s.dock.geometryMs
                       << ",\"hitCalls\":" << s.dock.hitCalls
                       << ",\"hitMs\":" << s.dock.hitMs
                       << ",\"previewCalls\":" << s.dock.previewCalls
                       << ",\"previewMs\":" << s.dock.previewMs
                       << ",\"captionMessages\":" << s.dock.captionMessages
                       << ",\"captionMs\":" << s.dock.captionMs
                       << ",\"hostNcHitTests\":" << s.dock.hostNcHitTests
                       << ",\"extraNcHitTests\":" << s.dock.extraNcHitTests
                       << ",\"zoneKind\":" << s.dock.zoneKind
                       << ",\"zoneSide\":" << s.dock.zoneSide
                       << ",\"previewKind\":" << s.dock.previewKind
                       << ",\"previewSide\":" << s.dock.previewSide
                       << ",\"commitKind\":" << s.dock.commitKind
                       << ",\"commitSide\":" << s.dock.commitSide
                       << ",\"commitRootRegion\":" << s.dock.commitRootRegion
                       << ",\"leafCountBefore\":" << s.dock.leafCountBefore
                       << ",\"leafCountAfter\":" << s.dock.leafCountAfter << '}';
            }
            output << ']';
        };
        writeSamples(dockIdleSamples);
        output << ",\"drag\":";
        writeSamples(dockDragSamples);
        output << '}';
        if (output.good()) {
            dockTraceWritten = Platform::FileSystem::WriteFileText(dockTracePath, output.str());
        }
    };
#endif

    static ULONGLONG s_lastLoopLogTick = 0;
    AppendDiagLogLine(L"主循环：启动");

    while (running) {
#if AURA_BUILD_DIAGNOSTICS && ARIS_PREVIEW_ISOLATION && !AURA_INTERNAL_BETA
        if (dockTraceRequested) aura::ui::dock::ActiveDockFrameTrace().beginFrame();
        const auto dockTraceFrameStart = std::chrono::steady_clock::now();
#endif
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
            if (msg.message == WM_QUIT) {
                running = false;
            }
            if (msg.message == WM_MOUSEMOVE || msg.message == WM_LBUTTONDOWN ||
                msg.message == WM_RBUTTONDOWN || msg.message == WM_MBUTTONDOWN ||
                msg.message == WM_KEYDOWN || msg.message == WM_MOUSEWHEEL) {
                state.lastInputTime = std::chrono::steady_clock::now();
            }
            if (msg.message == WM_LBUTTONDOWN || msg.message == WM_RBUTTONDOWN ||
                msg.message == WM_MBUTTONDOWN || msg.message == WM_KEYDOWN ||
                msg.message == WM_MOUSEWHEEL) {
                state.lastInteractTime = std::chrono::steady_clock::now();
            }
        }

#if AURA_BUILD_DIAGNOSTICS && ARIS_PREVIEW_ISOLATION && !AURA_INTERNAL_BETA
        const double dockTracePumpMs = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - dockTraceFrameStart).count();
#endif
        bool updateExitRequested=aris::update::ConsumeHarkenSearchUpdateGracefulExitRequest();
#if AURA_BUILD_DIAGNOSTICS && ARIS_PREVIEW_ISOLATION && !AURA_INTERNAL_BETA
        updateExitRequested=s_qaUpdateExitRequested.exchange(false,std::memory_order_acq_rel) || updateExitRequested;
#endif
        if (updateExitRequested) {
            RequestHarkenSearchExit(hwnd, L"update-bootstrap-accepted");
            // RequestHarkenSearchExit may be waiting for the Info save choice.
            // Keep rendering until that choice actually destroys the window.
            if (!Platform::WindowIsValid(hwnd)) running = false;
        }

        if (!running) {
#if AURA_BUILD_DIAGNOSTICS && !AURA_INTERNAL_BETA
            AbortFrameBudgetCapture(state, "window_closed", "loop_exit");
#endif
            break;
        }

// QA Automation: file-based command channel
        aura::diagnostics::FrameBudgetDiagnostics::beginFrame();
        // Keep QA commands alive even if the lane window is minimized/hidden.
        // Functional harness lanes may intentionally run without foreground focus;
        // command processing must not depend on active rendering.
#if AURA_BUILD_DIAGNOSTICS && !AURA_INTERNAL_BETA
        if (!aura::diagnostics::FrameBudgetDiagnostics::isCaptureActive()) {
            PollQaCommandFile(state);
        }
#endif

        // 窗口最小化或隐藏时不进行主窗口 OpenGL 呈现，避免 NVIDIA 驱动在 DrvPresentBuffers 内空指针崩溃 (ACCESS_VIOLATION 0x4)
        // 已拖出的平台视口仍要继续刷新，否则副屏面板会跟着主窗口一起冻住。
        const bool mainWindowHidden = IsIconic(hwnd) || !Platform::WindowIsVisible(hwnd);
        if (mainWindowHidden && !HarkenSearchHasHostedPlatformViewports()) {
#if AURA_BUILD_DIAGNOSTICS && !AURA_INTERNAL_BETA
            AbortFrameBudgetCapture(state, "window_hidden", "visibility");
#endif
            Platform::SleepMilliseconds(50);
            continue;
        }

        // Frame pacing follows focus, recent input, and playback state.
        // 目标：空闲时保持极低 CPU / GPU 占用。
        //
        // 三级策略：
        //   1. 后台无播放  → ~4fps  (Sleep 250)   最省电
        //   2. 后台有播放  → ~15fps (Sleep 66)    走带条仍流畅
        //   3. 前台空闲    → ~15fps (Sleep 66)    无输入无播放时自动降速
        //   4. 前台活跃    → fpsLimit (默认 60)   正常交互
        bool intentionalFrameThrottleSleep = false;
        if (!ShouldDisableHarnessBgThrottle()) {
            // Any foreground window owned by this process counts as active.
            // This includes WebView2 child windows and auxiliary application windows.

            HWND fgWnd = GetForegroundWindow();
            bool windowFocused = (fgWnd == hwnd);
            if (!windowFocused && fgWnd) {
                const uint64_t fgPid = Platform::WindowProcessId(fgWnd);
                if (fgPid == Platform::CurrentProcessId())
                    windowFocused = true;
            }
            const bool audioPlaying = state.audioEngine.isPlaying();
            const auto now = std::chrono::steady_clock::now();
            const double idleSec = std::chrono::duration<double>(now - state.lastInputTime).count();
            // Meaningful interaction (click/key/wheel) only — mousemove doesn't count.
            // A recent interaction means the user just engaged with our window; we must
            // wake fast regardless of what GetForegroundWindow() currently reports.
            const double interactIdleSec = std::chrono::duration<double>(now - state.lastInteractTime).count();

            // 前台空闲判定：无鼠标/键盘输入超过 2 秒，且无音频播放，且无后台任务
            // "没输入" 不等于 "没工作"：封面墙预加载、异步查询等都算活跃
            const bool scanForegroundPending =
                state.scanCompletionPending.load(std::memory_order_acquire);
            const bool hasPendingWork = state.audioEngine.isWaveformDetailPending()
                || state.pendingSearchRequestId != 0
                || state.isScanning.load(std::memory_order_relaxed)
                || scanForegroundPending
                || state.waveformRebuildActive
                || state.dbSwitchSettleFramesRemaining > 0
                || state.coverWall.entriesInFlight
                || state.coverWall.foldersInFlight
                || state.coverWall.loading
                || state.coverWall.preloadInFlight;
            const bool foregroundIdle = windowFocused && !audioPlaying && !hasPendingWork && idleSec > 2.0;

            if (!windowFocused) {
                const bool visibleNotMinimized = Platform::WindowIsVisible(hwnd) && !IsIconic(hwnd);
                // 用户刚点过/敲过键/滚过轮（< 0.5s），说明注意力在本窗口。此时无论
                // GetForegroundWindow() 报什么，都必须快速响应，不能被 250ms 吞掉。
                // Recent input overrides transient foreground-window races while
                // settings or child windows move focus between process-owned HWNDs.
                if (interactIdleSec < 0.5) {
                    DIAG_ACTION_FMT("FrameThrottle", "BG_interact_wake sleep=16 interactIdle=%.2f", interactIdleSec);
                    intentionalFrameThrottleSleep = true;
                    Platform::SleepMilliseconds(16);
                } else if (visibleNotMinimized && idleSec < 2.0) {
                    DIAG_ACTION_FMT("FrameThrottle", "BG_visible_recent_input sleep=16 idle=%.2f", idleSec);
                    intentionalFrameThrottleSleep = true;
                    Platform::SleepMilliseconds(16);
                } else if (scanForegroundPending) {
                    DIAG_ACTION_FMT("FrameThrottle", "BG_scan_completion_pending sleep=16 scanForegroundPending=%d", (int)scanForegroundPending);
                    intentionalFrameThrottleSleep = true;
                    Platform::SleepMilliseconds(16);
                } else if (hasPendingWork) {
                    // 后台有活（封面去重/预加载/扫描）时跑 ~30fps 让准备工作尽快完成
                    DIAG_ACTION_FMT("FrameThrottle", "BG_pending sleep=33 playing=%d scanForegroundPending=%d", (int)audioPlaying, (int)scanForegroundPending);
                    intentionalFrameThrottleSleep = true;
                    Platform::SleepMilliseconds(33);
                } else if (visibleNotMinimized) {
                    DIAG_ACTION_FMT("FrameThrottle", "BG_visible_idle sleep=66 idle=%.1f", idleSec);
                    intentionalFrameThrottleSleep = true;
                    Platform::SleepMilliseconds(66);
                } else {
                    // 真正空闲：~4fps 省电
                    int ms = audioPlaying ? 66 : 250;
                    DIAG_ACTION_FMT("FrameThrottle", "BG sleep=%d playing=%d idle=%.1f", ms, (int)audioPlaying, idleSec);
                    intentionalFrameThrottleSleep = true;
                    Platform::SleepMilliseconds(ms);
                }
            } else if (foregroundIdle) {
                // 前台但完全空闲：降到 ~15fps，肉眼无感但 CPU/GPU 降 75%
                DIAG_ACTION_FMT("FrameThrottle", "FG_idle sleep=66 idle=%.1f pending=%d scanForegroundPending=%d", idleSec, (int)hasPendingWork, (int)scanForegroundPending);
                intentionalFrameThrottleSleep = true;
                Platform::SleepMilliseconds(66);
            }
            // else: 前台活跃，不额外 Sleep，由 fpsLimit 控制
        }

        aura::diagnostics::FrameBudgetDiagnostics::beginCpuWork();
#if AURA_BUILD_DIAGNOSTICS && !AURA_INTERNAL_BETA
        PumpFrameBudgetWorkload(state, intentionalFrameThrottleSleep);
#endif

        // Periodic heartbeat log
        {
            ULONGLONG now = Platform::MonotonicMilliseconds();
            if (now - s_lastLoopLogTick > 5000) {
                s_lastLoopLogTick = now;
                AppendDiagLogF(L"心跳：visible=%d iconic=%d trayAdded=%d",
                               (int)Platform::WindowIsVisible(hwnd), (int)IsIconic(hwnd), (int)g_trayIconAdded);
            }
        }

        // Fallback: poll hotkey only when RegisterHotKey fails.
        if (state.settings.enableShowManagerHotkey &&
            state.settings.showManagerHotkey.key != ImGuiKey_None &&
            !g_showManagerHotkeyRegisterOk) {
            if (IsGlobalHotkeyPressedOnce(state.settings.showManagerHotkey)) {
                AppendDiagLogLine(L"Polling hotkey triggered.");
                HandleShowManagerHotkeyToggle();
            }
        }

        // Rebuild fonts before NewFrame so the active OpenGL texture is not destroyed.
        // Scheme B: dynamic UI scale (font rebuild + style scaling, debounced)
        UpdateImGuiUiScaleSchemeB(state);

        // Start ImGui frame
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplWin32_NewFrame();
        // Inject only into this process' ImGui queue, after the Win32 backend
        // sampled the physical mouse and before DrawMagicEQBar handles input.
#if AURA_BUILD_DIAGNOSTICS && !AURA_INTERNAL_BETA
        if (MagicEqQaIsolationReady()) ApplyMagicEqQaPointerBeforeFrame();
#endif
        ImGui::NewFrame();
        aura::ui::focus::UpdateFocusTracker();
        TickHarkenSearchSmokeWindow();  // F9 opens the diagnostic widget smoke window.
        bool magicEqQaCollectionEnabled = false;
#if AURA_BUILD_DIAGNOSTICS && !AURA_INTERNAL_BETA
        magicEqQaCollectionEnabled = magicEqQaCollect &&
            Platform::MonotonicMilliseconds() <=
                s_magicEqQaCollectUntilMs.load(std::memory_order_acquire);
#endif
        ThemeInspector::SetBridgeCollectionEnabled(annotationBridgeEnabled || magicEqQaCollectionEnabled);
        ThemeInspector::BeginFrame();
        // ── DevTools：诊断心跳（上线构建 AURA_BUILD_DIAGNOSTICS=0 时为空）
        {
            DevTools_TickFrame(state.settings.diagnosticMode);
            DevTools_LogFrameSpike(ImGui::GetIO().DeltaTime, intentionalFrameThrottleSleep);
            DevTools_ResetFramePhases();
            static int s_diagFrameCounterMain = 0;
            if (state.settings.diagnosticMode && (++s_diagFrameCounterMain % 300 == 0)) {
                DevTools_LogFrameFps(DiagnosticsLogger::instance().currentFps());
                const auto processMemory = Platform::QueryCurrentProcessMemoryUsage();
                if (processMemory.status == Platform::ProcessMemoryUsageStatus::Success)
                    DevTools_LogMemoryMb(processMemory.privateUsageBytes / (1024.0 * 1024.0));
            }
        }
        
        if (g_suppressImGuiDragTooltipFrames > 0) {
            g_suppressImGuiDragTooltipFrames--;
        }
        
        {
            // Lottie 精灵功能已禁用（避免 rlottie 线程导致的 AV 崩溃）
            // 如需恢复，回滚此处并恢复 ThemeMascot.cpp 中的 Lottie 覆盖逻辑
        }

        // Deferred search refresh after DB switch — runs one frame after
        // ResetDatabaseState so the synchronous SQL query doesn't stack
        // onto the same frame. Must be checked BEFORE Pump so that the
        // flag set by Pump on frame N is consumed on frame N+1.
        DevTools_BeginFramePhase("PreRender.DbSwitchRefresh");
        if (state.dbSwitchNeedsRefresh) {
            state.dbSwitchNeedsRefresh = false;
            RefreshSearchResults(state, true);
        }
        DevTools_EndFramePhase();
        // 异步切库：轮询后台 open 结果，避免大库 open 卡死主线程
        DevTools_BeginFramePhase("PreRender.AsyncPumps");
        PumpDatabaseSwitchIfPending(state);
        PumpInitRandomSamplingPoolIfPending(state);

        // Complete pending audio loads before dependent UI updates.
        if (state.audioEngine.pumpLoadFileIfPending()) {
            state.lastSelectionOrLoadTime = ImGui::GetTime();  // 切歌完成，封面墙预算短时降级
            // 新文件加载成功，清除旧的错误提示
            state.loadErrorToast.clear();
            state.loadErrorToastUntil = 0.0;
            ScheduleDeferredWaveformDetail(state, state.audioEngine.getLoadedFilePath(), AppState::WAVEFORM_DEFAULT_DETAIL_PEAKS);
            // 如果有之前在 Mini 波形上预设的跳转点，现在执行它！
            OnAudioLoadPumped(state);
            // Start playback AFTER silence-skip so the sound begins at the
            // correct position — avoids a visible waveform cursor jump.
            state.audioEngine.playIfLoadReady();
            static aura::playback::PlayStatsGate s_playStatsGate;
            static std::uint64_t s_playStatsLoadToken = 0;
            aura::playback::RecordSuccessfulPlayStats(
                state.database,
                s_playStatsGate,
                aura::playback::PlayStatsEvent{
                    true,
                    state.audioEngine.isPlaying(),
                    state.currentFileId,
                    ++s_playStatsLoadToken});
        } else {
            std::string loadErr = state.audioEngine.getLastLoadError();
            if (!loadErr.empty()) {
                const std::string failedPath = state.pendingAudioTargetPath;
                state.clearPendingAudioTarget();
                const bool missingOnDisk = !failedPath.empty() &&
                    Platform::FileSystem::ProbePath(failedPath) ==
                        Platform::FileSystem::PathStatus::Missing;
                if (missingOnDisk) {
                    AudioRecord missingRecord;
                    missingRecord.filePath = failedPath;
                    missingRecord.fileName = PathToUtf8(Utf8ToPath(failedPath).filename());
                    for (const auto& candidate : state.searchResults) {
                        if (candidate.filePath == failedPath) {
                            missingRecord = candidate;
                            break;
                        }
                    }
                    aura::ui::EvaluateRelinkForRecord(state, missingRecord);
                } else {
                    state.loadErrorToast = std::move(loadErr);
                    state.loadErrorToastUntil = ImGui::GetTime() + 5.0;
                }
            }
        }
        // Pump detail requests outside RenderUI so WebUI mode follows the same path.
        PumpDeferredWaveformDetail(state);
        // 跳过开头无声：加载完成时波形常未就绪，每帧尝试直至主波形或 mini 缓存有数据再 seek
        MaybeSkipLeadingSilence(state);
        if (state.dbSwitchJustCompleted) {
            state.dbSwitchJustCompleted = false;
            std::string dbName = GetDatabaseDisplayNameUtf8(state.currentDbPathUtf8);
            std::wstring title;
            {
                const std::wstring fmt = Lw(WinTitle_DatabaseFmt);
                const std::wstring dbNameW = Utf8ToWide(dbName);
                wchar_t titleBuf[1024];
                swprintf_s(titleBuf, fmt.c_str(), dbNameW.c_str(), (long long)state.database.getFileCount());
                title = titleBuf;
            }            SetWindowTextW(hwnd, title.c_str());
        }
        DevTools_EndFramePhase();


        const bool scanPopupOpen = ImGui::IsPopupOpen(L(Dlg_ScanFolder));

        // Check if database needs refresh during scan (渐进式更新)
        DevTools_BeginFramePhase("PreRender.ScanRefresh");
        if (state.scanDataCommitted.load()) {
            state.scanDataCommitted = false;
            
            // During scan, refresh list incrementally to show newly committed data.
            if (state.viewMode == ViewMode::Database && state.isScanning.load() && !scanPopupOpen) {
                // 如果有活跃的搜索/筛选条件，不覆盖 totalFileCount
                // 避免扫描期间覆盖用户的搜索结果总数（由 RefreshSearchResults 正确计算）
                // 性能：getFileCount() 大库时耗时可观，节流为每 2 秒最多一次（规范 §11）
                bool hasActiveFilters = (state.searchBuffer[0] != '\0') ||
                    state.folderFilterActive || state.coverFilterActive;
                if (!hasActiveFilters) {
                    static double s_lastScanGetFileCountTime = 0.0;
                    double nowSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
                    if (s_lastScanGetFileCountTime == 0.0 || nowSeconds - s_lastScanGetFileCountTime >= 2.0) {
                        state.totalFileCount = state.database.getFileCount();
                        s_lastScanGetFileCountTime = nowSeconds;
                    }
                }
                
                // 节流阀 (Throttling) - 防止刷新过快导致闪烁
                // Heavy list refresh is throttled.
                const double REFRESH_THROTTLE_INTERVAL = 1.0;
                auto now = std::chrono::steady_clock::now();
                auto nowSeconds = std::chrono::duration<double>(now.time_since_epoch()).count();
                double timeSinceLastRefresh = (state.lastIncrementalRefreshTime == 0.0) ? 
                    REFRESH_THROTTLE_INTERVAL : (nowSeconds - state.lastIncrementalRefreshTime);
                
                // 如果当前在第一页（显示前PAGE_SIZE个结果），刷新以显示新添加的文件
                // 这样用户可以看到列表在不断填充，体验更好
                if ((state.loadedCount <= AppState::PAGE_SIZE || state.loadedCount == 0) && 
                    timeSinceLastRefresh >= REFRESH_THROTTLE_INTERVAL) {
                    // Refresh the first page so the data source includes the new file; ImGui updates visible rows itself.
                    RefreshSearchResults(state, false); // keep pagination state, refresh data source
                    state.lastIncrementalRefreshTime = nowSeconds;
                }
                // 如果用户不在第一页，只更新总数，不刷新列表（避免丢失滚动位置）
                // 用户可以通过滚动到顶部或等待扫描完成来查看新数据
                
                // 同步到当前标签页
                if (!state.listTabs.empty()) {
                    state.currentTab().searchResults = state.searchResults;
                    state.currentTab().totalFileCount = state.totalFileCount;
                    state.currentTab().loadedCount = state.loadedCount;
                    state.currentTab().hasMoreResults = state.hasMoreResults;
                }
            }
        }
        DevTools_EndFramePhase();

        // Refresh database-backed rows after scanning completes.
        DevTools_BeginFramePhase("PreRender.DatabaseRefresh");
        if (state.needsDatabaseRefresh && !scanPopupOpen) {
            state.needsDatabaseRefresh = false;
            state.invalidateCoverWallAfterDatabaseContentChange();

            if (state.scanThread.joinable()) {
                state.scanThread.detach();
            }

            // scanDirectoryEx commits before returning, so no second commit is needed here.
            // Rebuild folder_index (write op), then use a fresh read-only
            // connection via deferred bootstrap to avoid stale WAL snapshot
            // missing library roots written by the scan thread.
            state.database.rebuildFolderIndex();
            StartDeferredSidebarBootstrap(state, true, false);
            
            // Auto-filter to scanned path and load first page.
            if (state.autoFilterToScannedPath && !state.lastScannedPath.empty()) {
                state.autoFilterToScannedPath = false;
                // Scan auto-filter paths are canonical in both fallback branches.
                try {
                    fs::path p = Utf8ToPath(state.lastScannedPath);
                    state.selectedFolderPath = HarkenSearch::CanonicalFolderPath(PathToUtf8(p));
                } catch (...) {
                    state.selectedFolderPath = HarkenSearch::CanonicalFolderPath(state.lastScannedPath);
                }
                state.folderFilterActive = true;
            }
            
            // QA/UI scan completion is a database-content transition. Normalize
            // back to Database before refreshing so a scan started from another
            // view cannot leave the DB populated while the visible list reports
            // zero rows.
            state.viewMode = ViewMode::Database;

            // Reset pagination and query the database; repaint alone would retain stale rows.
            state.searchResults.clear();
            state.bumpSearchResultsGen();
            state.loadedCount = 0;
            RefreshSearchResults(state, true); // true = 重置分页并重新查询数据库
            if (state.scanTotal.load() > 0 &&
                !state.isScanning.load(std::memory_order_acquire)) {
                state.dbSwitchSettleFramesRemaining =
                    (std::max)(state.dbSwitchSettleFramesRemaining, 120);
            }
            
            // 同步到当前标签页
            if (!state.listTabs.empty()) {
                state.currentTab().searchResults = state.searchResults;
                state.currentTab().totalFileCount = state.totalFileCount;
                state.currentTab().loadedCount = state.loadedCount;
                state.currentTab().hasMoreResults = state.hasMoreResults;
                state.currentTab().selectedFolderPath = state.selectedFolderPath;
                state.currentTab().folderFilterActive = state.folderFilterActive;
            }
        }
        DevTools_EndFramePhase();

        // After post-process maintenance (syncCoverRegistry + backfillCoverPhash + backfillNormalizedKey)
        // finishes, reload the cover wall so clustering works with correct phash values.
        DevTools_BeginFramePhase("PreRender.PostProcessRefresh");
        const bool postProcessCoverRefresh =
            state.database.consumePostProcessCoverRefresh();
        const bool backfillCoverRefresh =
            aura::ConsumeCoverBackfillUiRefresh();
        if (postProcessCoverRefresh) {
            state.invalidateCoverWallAfterDatabaseContentChange();
            MarkScanReadyToWorkCoversSatisfiedForQa(state);
        }
        if (backfillCoverRefresh) {
            state.requestCoverWallBackfillDisplayRefresh();
            MarkScanReadyToWorkCoversSatisfiedForQa(state);
        }
        PumpScanReadyToWorkFirstScreenCoversForQa(state);
        RefreshScanReadyToWorkStateForQa(state);
        RefreshScanCompletionStateForQa(state);
        DevTools_EndFramePhase();

        state.visibleFilePaths.clear();

        // 启动暖机：前 3 帧轻量绘制，避免首帧 6s+ 卡顿（列表/封面墙分帧加载）
        {
            static int s_framesSinceMainUi = 0;
            s_framesSinceMainUi++;
            state.warmupFramesRemaining = (s_framesSinceMainUi <= 3) ? (4 - s_framesSinceMainUi) : 0;
            if (state.dbSwitchSettleFramesRemaining > 0) {
                --state.dbSwitchSettleFramesRemaining;
            }
        }

        const float baseStyleAlpha = ImGui::GetStyle().Alpha;
        float startupUiAlpha = 1.0f;
        if (startupEntryTransitionActive) {
            const auto nowTransitionForUi = std::chrono::steady_clock::now();
            const float elapsedMsForUi = (float)std::chrono::duration_cast<std::chrono::milliseconds>(
                nowTransitionForUi - startupEntryTransitionStart).count();
            startupUiAlpha = GetStartupEntryUiAlpha(elapsedMsForUi, kStartupEntryTransitionMs);
        }
        ImGui::GetStyle().Alpha = baseStyleAlpha * startupUiAlpha;
#if AURA_BUILD_DIAGNOSTICS && ARIS_PREVIEW_ISOLATION && !AURA_INTERNAL_BETA
        const auto dockTraceRenderStart = std::chrono::steady_clock::now();
#endif
        DevTools_BeginFramePhase("RenderUI");
        RenderUI(state);
        GuardInfoMetadataContext(state);
        RenderInfoMetadataLeavePrompt(state);
#if AURA_BUILD_DIAGNOSTICS && ARIS_PREVIEW_ISOLATION && !AURA_INTERNAL_BETA
        const double dockTraceRenderUiMs = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - dockTraceRenderStart).count();
#endif
        aura::diagnostics::FrameBudgetDiagnostics::markRenderUi();
        DevTools_EndFramePhase();
        ImGui::GetStyle().Alpha = baseStyleAlpha;

        FlushSettingsDirty(state);

        // F-033: consume the deferred SFX↔Music toggle flag. RenderUI
        // only SETS state.pendingProductModeToggle when the top-bar
        // toggle is clicked; the actual mode flip + cache clear happens
        // Consume the mode toggle after rendering so widgets keep one mode for
        // the entire frame. The flag must be cleared exactly once here.

        ProcessPendingProductModeToggle(state);
        
        // Continuous playback
        static bool wasPlayingLastFrame = false;
        static float lastPlaybackPos = 0.0f;
        static int consecutiveLoadFailures = 0;
        
        if (state.settings.continuousPlayback && state.viewMode != ViewMode::Browser) {
            bool isPlaying = state.audioEngine.isPlaying();
            float currentPos = state.audioEngine.getPlaybackPosition();
            bool isLoaded = state.audioEngine.isLoaded();
            
            bool playbackEnded = (wasPlayingLastFrame && !isPlaying && lastPlaybackPos > 0.95f);
            bool stuckAtEnd = (isLoaded && !isPlaying && currentPos > 0.99f && lastPlaybackPos > 0.99f);
            
            if ((playbackEnded || stuckAtEnd) && !state.audioEngine.isLoadFilePending()) {
                bool success = false;

                if (state.settings.shufflePlayback) {
                    PlayRandomFile(state);
                    // loadFileAsync 是异步的，不能立即检查 isPlaying
                    // 重置检测状态，防止下一帧重复触发
                    wasPlayingLastFrame = false;
                    lastPlaybackPos = 0.0f;
                    success = true;
                } else {
                    int nextIdx = state.selectedResultIndex + 1;
                    if (nextIdx >= 0 && nextIdx < (int)state.searchResults.size()) {
                        state.selectedResultIndex = nextIdx;
                        state.selectedIndicesSet.clear();
                        state.selectedIndicesSet.insert(nextIdx);
                        state.lastClickedIndex = nextIdx;
                        state.scrollToSelected = true;
                        const auto& record = state.searchResults[nextIdx];
                        if (!record.filePath.empty()) {
                            state.audioEngine.loadFileAsync(record.filePath, true);
                            ScheduleDeferredWaveformDetail(state, record.filePath, AppState::WAVEFORM_DEFAULT_DETAIL_PEAKS);
                            state.currentFileId = record.id;
                            ResetWaveformZoom(state);
                            MarkFilePlayed(state, record.filePath, record.id);
                            success = true;
                        }
                        state.syncToTab();
                    }
                }
                
                if (success) {
                    consecutiveLoadFailures = 0;
                } else {
                    consecutiveLoadFailures++;
                    if (consecutiveLoadFailures > 3 && !state.settings.shufflePlayback) {
                        state.selectedResultIndex += consecutiveLoadFailures;
                        consecutiveLoadFailures = 0;
                    }
                }
            }
            
            wasPlayingLastFrame = isPlaying;
            lastPlaybackPos = currentPos;
        } else {
            wasPlayingLastFrame = state.audioEngine.isPlaying();
            lastPlaybackPos = state.audioEngine.getPlaybackPosition();
            consecutiveLoadFailures = 0;
        }

        // Process pending waveform loading
        auto now_wf = std::chrono::steady_clock::now();
        auto mouseIdleMs = std::chrono::duration_cast<std::chrono::milliseconds>(now_wf - state.lastMouseMoveTime).count();
        bool isMouseIdle = mouseIdleMs > 500;
        
        // Async waveform loading: submit requests and consume completed items.
        {
            // Ensure waveform worker starts once.
            // Worker 可能已在 Splash Phase 4.8 中启动，start() 内部有幂等检查。
            static bool waveformWorkerStarted = false;
            static bool searchWorkerStarted = false;
            bool holdColdMiniWorker = false;
#if AURA_BUILD_DIAGNOSTICS && !AURA_INTERNAL_BETA
            holdColdMiniWorker = s_frameBudgetColdMini.holding && !s_frameBudgetColdMini.workerStarted;
#endif
            if (!waveformWorkerStarted && !holdColdMiniWorker) {
                // Browser/direct-file mode can source-decode visible mini waves
                // without an open library. The stable Database object remains
                // available for optional cache reads/writes once it is opened.
                state.waveformWorker.start(&state.database, aura::RecommendedWaveformWorkerThreads());
                waveformWorkerStarted = true;
            }
            if (!searchWorkerStarted && state.database.isOpen()) {
                state.searchWorker.start(&state.database);
                searchWorkerStarted = true;
            }
            
            // 1) Submit pending waveforms to background worker.
            //    Increased from 10→50: DB-cached reads complete in ~1ms each,
            //    so we can safely queue many more per frame for faster initial display.
            int submitted = 0;
#if AURA_BUILD_DIAGNOSTICS && !AURA_INTERNAL_BETA
            int listSubmitted = 0;
#endif
            // Keep the selected file's main waveform ahead of list mini-waveforms.
            // During rapid scroll/play, bulk list work can otherwise saturate CPU/disk
            // and make long-file main detail miss its user-visible window.
            const bool contextMenuHeavyFreezeForWaveform =
                ImGui::IsPopupOpen("RowContextMenu") ||
                ImGui::IsPopupOpen("##PlaylistSidebarContext");
            BackgroundIoContext miniIo = BuildBackgroundIoContextForWindowsMain(
                state, contextMenuHeavyFreezeForWaveform, IsCoverWallPriorityMode(), isMouseIdle);
#if AURA_BUILD_DIAGNOSTICS && !AURA_INTERNAL_BETA
            const auto miniCaptureState = aura::diagnostics::FrameBudgetDiagnostics::shared().state();
            if (s_frameBudgetColdMini.armed &&
                (miniCaptureState == aura::diagnostics::FrameCaptureState::BusyWarmup ||
                 miniCaptureState == aura::diagnostics::FrameCaptureState::BusyCapture))
                miniIo.playing = false;
#endif
            const auto miniDecision = ResolveMiniWaveformWorkerDecision(miniIo);
            static bool s_miniListWasForegroundBlocked = false;
            if (miniDecision.cancelQueuedListWork && !s_miniListWasForegroundBlocked) {
                state.waveformWorker.cancelAllPending();
                // In-flight meterOnly requests get dropped with the worker queue; clear
                // the dedup set so future visible-mini results can re-enqueue meters.
                state.meterFollowupPending.clear();
            }
            s_miniListWasForegroundBlocked = miniDecision.cancelQueuedListWork;
            const bool foregroundWaveformRebuild = state.waveformRebuildActive;
            const bool mainWaveformPending =
                !foregroundWaveformRebuild && state.audioEngine.isWaveformDetailPending();
            // Foreground scan completion owns priority-0 mini backfill. If the
            // worker stays throttled here, scan-created jobs can starve forever.
            const bool scanForegroundWaveformDrainActive =
                state.scanCompletionPending.load(std::memory_order_acquire) &&
                state.scanCompletionWaveformsPending.load(std::memory_order_acquire);
            const bool throttleWaveformWorker =
                !scanForegroundWaveformDrainActive &&
                (mainWaveformPending || miniDecision.throttleWorker);
            int submitBudget = miniDecision.submitListBudget;
            if (state.postSearchMiniWaveThrottleFrames > 0 && !scanForegroundWaveformDrainActive) {
                submitBudget = (std::min)(submitBudget, 1);
            }
            state.waveformWorker.setThrottleMode(throttleWaveformWorker);

            const std::string loadedWaveformPath = state.audioEngine.getLoadedFilePath();
#if AURA_BUILD_DIAGNOSTICS && !AURA_INTERNAL_BETA
            if (miniCaptureState == aura::diagnostics::FrameCaptureState::BusyCapture &&
                submitBudget > 0 && !s_frameBudgetColdMini.seedSubmitted) {
                for (const auto& record : state.searchResults) {
                    if (record.filePath.empty() || record.filePath == loadedWaveformPath) continue;
                    state.visibleFilePaths.insert(record.filePath);
                    if (state.waveformWorker.submitVisibleRequest(record.filePath)) {
                        s_frameBudgetColdMini.seedSubmitted = true;
                        ++submitted;
                        ++listSubmitted;
                        break;
                    }
                }
            }
#endif
            if (!foregroundWaveformRebuild &&
                miniDecision.allowCurrentFileFastLane &&
                !loadedWaveformPath.empty() &&
                state.waveformCache.count(loadedWaveformPath) == 0 &&
                state.waveformFailed.count(loadedWaveformPath) == 0) {
                state.visibleFilePaths.insert(loadedWaveformPath);
                if (state.waveformWorker.submitVisibleRequest(loadedWaveformPath)) {
                    submitted++;
                    DIAG_ACTION_FMT("Waveform", "LoadedFileWaveformFastLane: path=%s",
                                    loadedWaveformPath.c_str());
                }
            }

            // Dequeue urgent visible waveforms before normal work and recheck liveness.
            std::string filePath;
            while (!foregroundWaveformRebuild &&
                   submitted < submitBudget &&
                   state.popNextPendingWaveform(filePath)) {
                // 1. 基础过滤：已加载或确认失败的跳过
                if (state.waveformCache.count(filePath) > 0) continue;
                if (state.waveformFailed.count(filePath) > 0) continue;

                // 2. 存活校验：如果这个路径不在本帧可见列表里，说明是滚过去的陈旧任务，直接丢弃
                // 这样能瞬间清空几万个积压请求，让真正需要显示的请求直接进入硬件
                // 全库重建时绕过此检查，否则非可见文件永远不会被提交
                if (!state.waveformRebuildActive &&
                    state.visibleFilePaths.find(filePath) == state.visibleFilePaths.end()) {
                    continue;
                }
                
                if (state.waveformWorker.submitVisibleCacheRequest(filePath)) {
                    submitted++;
#if AURA_BUILD_DIAGNOSTICS && !AURA_INTERNAL_BETA
                    listSubmitted++;
#endif
                }
            }
            
            // Consume at most N completed background results per frame.
            state.waveformWorker.setThrottleMode(throttleWaveformWorker);
            std::vector<WaveformResult> completedResults;
            const auto resultConsumeStart = std::chrono::steady_clock::now();
            int consumed = state.waveformWorker.pollResults(completedResults, AppState::WAVEFORMS_PER_FRAME);
#if AURA_BUILD_DIAGNOSTICS && !AURA_INTERNAL_BETA
            int listSuccessfulConsumed = 0;
#endif
            int diagCacheOnly = 0;
            int diagBackfill = 0;
            int diagTerminal = 0;
            int diagClear = 0;
            int diagDeferred = 0;
            int diagStaleDrop = 0;
            int diagUiDrop = 0;
            int visibleDecodePromotionsThisFrame = 0;
            const int visibleDecodePromotionBudget =
                (state.postSearchMiniWaveThrottleFrames > 0 && !scanForegroundWaveformDrainActive) ? 1 : 4;
            static std::map<std::string, int> s_waveformRetryCount;
            constexpr int kWaveformMaxRetries = 3;
            constexpr size_t kMaxWaveformRetryEntries = 5000;
            if (s_waveformRetryCount.size() > kMaxWaveformRetryEntries) {
                auto half = s_waveformRetryCount.begin();
                std::advance(half, s_waveformRetryCount.size() / 2);
                s_waveformRetryCount.erase(s_waveformRetryCount.begin(), half);
            }
            
            for (auto& result : completedResults) {
                bool isStillVisiblePageItem =
                    state.visibleFilePaths.find(result.filePath) !=
                    state.visibleFilePaths.end();
                if (!isStillVisiblePageItem) {
                    // Keep the existing central-list retention rule for rows
                    // that were submitted before the current frame's
                    // visibility set was rebuilt (for example during a fast
                    // scroll or page transition).
                    for (const auto& rec : state.searchResults) {
                        if (rec.filePath == result.filePath) {
                            isStillVisiblePageItem = true;
                            break;
                        }
                    }
                }
                const bool isLoadedFileResult =
                    !loadedWaveformPath.empty() && result.filePath == loadedWaveformPath;
#if AURA_BUILD_DIAGNOSTICS && !AURA_INTERNAL_BETA
                const bool isCurrentListPath = isStillVisiblePageItem ||
                    state.visibleFilePaths.count(result.filePath) > 0;
#endif
                if (result.hasBackfillJob) {
                    ++diagBackfill;
                    const bool deferredDbWrite = result.errorCode == "deferred_scan_db_write";
                    if (deferredDbWrite) {
                        ++diagDeferred;
                    }
                    const aura::BackfillCompletionOutcome outcome = result.success
                        ? aura::BackfillCompletionOutcome::Success
                        : (deferredDbWrite
                            ? aura::BackfillCompletionOutcome::TransientFailure
                            : aura::BackfillCompletionOutcome::PermanentFailure);
                    state.backfillQueue.reportOutcome(result.backfillJob, outcome);
                    if (result.success ||
                        state.backfillQueue.itemState(result.backfillJob.fileId, result.backfillJob.kind) ==
                            aura::BackfillItemState::FailedPermanent) {
                        const std::string foregroundReason = result.success
                            ? "mini_cached"
                            : (result.errorCode.empty() ? "waveform_backfill_failed" : result.errorCode);
                        if (CanClearBackfillPendingForJob(
                                state,
                                result.backfillJob,
                                result.success,
                                foregroundReason)) {
                            ++diagTerminal;
                            state.database.clearBackfillPending(
                                result.backfillJob.fileId,
                                static_cast<int>(result.backfillJob.kind));
                            ++diagClear;
                        } else {
                            ++diagStaleDrop;
                        }
                    }
                }
                if (result.cacheOnly) {
                    ++diagCacheOnly;
                    if (result.success) {
                        state.waveformFailed.erase(result.filePath);
                    } else if (result.errorCode == "deferred_scan_db_write") {
                        ++diagDeferred;
                        continue;
                    } else if (result.errorCode == "mini_cache_miss_no_decode") {
                        state.waveformMiniCacheMiss.erase(result.filePath);
                        state.removePendingWaveform(result.filePath);
                        if (visibleDecodePromotionsThisFrame < visibleDecodePromotionBudget &&
                            state.waveformWorker.submitVisibleRequest(result.filePath)) {
                            ++visibleDecodePromotionsThisFrame;
                            DIAG_ACTION_FMT("MiniWave",
                                            "VisibleCacheMissPromotedToDecode: path=%s",
                                            result.filePath.c_str());
                        } else {
                            state.addPendingWaveform(result.filePath, true);
                        }
                        continue;
                    } else {
                        state.waveformFailed.insert(result.filePath);
                        state.database.upsertWaveformCacheStateByPath(
                            result.filePath,
                            "failed",
                            result.errorCode.empty() ? "decode_failed" : result.errorCode,
                            1,
                            "");
                    }
                    state.removePendingWaveform(result.filePath);
                    continue;
                }
                // Drop stale/off-page list waveform results to keep cache/upload work focused
                // on current view state and reduce jitter after rapid filter/page changes.
                // 全库重建时保留所有结果，否则解码成果被白白丢弃
                if (!state.waveformRebuildActive && !isStillVisiblePageItem && !isLoadedFileResult) {
                    ++diagUiDrop;
                    continue;
                }

                if (result.success) {
#if AURA_BUILD_DIAGNOSTICS && !AURA_INTERNAL_BETA
                    if (isCurrentListPath && !isLoadedFileResult && !result.hasBackfillJob)
                        ++listSuccessfulConsumed;
#endif
                    s_waveformRetryCount.erase(result.filePath);
                    state.waveformFailed.erase(result.filePath);
                    state.waveformMiniCacheMiss.erase(result.filePath);
                    // 使用字节预算管理（如果可用）
                    if (state.memoryBudget) {
                        uint64_t waveformSizeBytes = result.waveform.peaks.size() * sizeof(float) + 
                                                     result.waveform.rms.size() * sizeof(float);
                        
                        if (!state.memoryBudget->canAdd(MemoryBudget::CacheType::Waveform, waveformSizeBytes)) {
                            uint64_t currentUsage = state.memoryBudget->getCurrentUsage(MemoryBudget::CacheType::Waveform);
                            uint64_t budget = state.memoryBudget->getBudgetConfig().waveformBudget;
                            if (currentUsage + waveformSizeBytes > budget) {
                                uint64_t requiredBytes = (currentUsage + waveformSizeBytes) - budget;
                                auto evicted = state.memoryBudget->evictLRU(MemoryBudget::CacheType::Waveform, requiredBytes);
                                for (const auto& evictEntry : evicted)
                                    ReleaseWaveformEvictedEntry(state, evictEntry.key,
                                        MiniWaveformPresentation::Removal::BudgetEviction);
                            }
                        }
                        
                        state.memoryBudget->registerEntry(MemoryBudget::CacheType::Waveform, result.filePath, waveformSizeBytes);
                        state.memoryBudget->updateUsage(MemoryBudget::CacheType::Waveform, result.filePath);
                    } else {
                        if (state.waveformCache.find(result.filePath) == state.waveformCache.end() &&
                            state.waveformCache.size() >= AppState::MAX_WAVEFORM_CACHE_SIZE) {
                            DIAG_ACTION_FMT("MiniWave", "dataCacheTrim size=%zu limit=%d",
                                            state.waveformCache.size(),
                                            AppState::MAX_WAVEFORM_CACHE_SIZE);
                            state.waveformCache.erase(state.waveformCache.begin());
                        }
                    }
                    InvalidateMiniWaveTextureAfterFrame(result.filePath);
                    state.waveformCache[result.filePath] = std::move(result.waveform);
                } else {
                    int attempt = 1;
                    auto itRetry = s_waveformRetryCount.find(result.filePath);
                    if (itRetry == s_waveformRetryCount.end()) {
                        s_waveformRetryCount[result.filePath] = 1;
                    } else {
                        itRetry->second += 1;
                        attempt = itRetry->second;
                    }

                    const char* reason = result.errorCode.empty() ? "unknown" : result.errorCode.c_str();
                    if (result.errorCode == "mini_cache_miss_no_decode") {
                        state.waveformMiniCacheMiss.erase(result.filePath);
                        s_waveformRetryCount.erase(result.filePath);
                        state.removePendingWaveform(result.filePath);
                        if (visibleDecodePromotionsThisFrame < visibleDecodePromotionBudget &&
                            state.waveformWorker.submitVisibleRequest(result.filePath)) {
                            ++visibleDecodePromotionsThisFrame;
                            DIAG_ACTION_FMT("MiniWave",
                                            "VisibleCacheMissPromotedToDecode: path=%s",
                                            result.filePath.c_str());
                        } else {
                            state.addPendingWaveform(result.filePath, true);
                        }
                        continue;
                    }
                    DIAG_ACTION_FMT("Waveform", "MiniWaveformLoadFailed: path=%s attempt=%d reason=%s",
                                    result.filePath.c_str(), attempt, reason);

                    if (attempt < kWaveformMaxRetries) {
                        state.addPendingWaveform(result.filePath, true);
                        continue;
                    }

                    DIAG_ACTION_FMT("Waveform", "MiniWaveformPermanentFailure: path=%s reason=%s",
                                    result.filePath.c_str(), reason);
                    state.waveformFailed.insert(result.filePath);
                }
            }
#if AURA_BUILD_DIAGNOSTICS && !AURA_INTERNAL_BETA
            aura::diagnostics::FrameBudgetDiagnostics::shared().observeMiniActivity(
                listSubmitted, listSuccessfulConsumed);
#endif
            {
                static auto s_lastBackfillResultConsumeLog = std::chrono::steady_clock::now();
                static int s_consumed = 0;
                static int s_cacheOnly = 0;
                static int s_backfill = 0;
                static int s_terminal = 0;
                static int s_clear = 0;
                static int s_deferred = 0;
                static int s_staleDrop = 0;
                static int s_uiDrop = 0;
                static double s_durationMs = 0.0;
                static bool s_wasScanForegroundPending = false;
                const bool scanForegroundPending =
                    state.scanCompletionPending.load(std::memory_order_acquire);
                if (scanForegroundPending || s_wasScanForegroundPending) {
                    s_wasScanForegroundPending = scanForegroundPending;
                    const auto resultConsumeEnd = std::chrono::steady_clock::now();
                    s_consumed += consumed;
                    s_cacheOnly += diagCacheOnly;
                    s_backfill += diagBackfill;
                    s_terminal += diagTerminal;
                    s_clear += diagClear;
                    s_deferred += diagDeferred;
                    s_staleDrop += diagStaleDrop;
                    s_uiDrop += diagUiDrop;
                    s_durationMs += std::chrono::duration<double, std::milli>(
                        resultConsumeEnd - resultConsumeStart).count();

                    const double elapsedSec =
                        std::chrono::duration<double>(resultConsumeEnd - s_lastBackfillResultConsumeLog).count();
                    if (elapsedSec >= 1.0 || !scanForegroundPending) {
                        char detail[320];
                        std::snprintf(
                            detail,
                            sizeof(detail),
                            "BackfillResultConsume: consumed=%d cacheOnly=%d backfill=%d terminal=%d clear=%d deferred=%d staleDrop=%d uiDrop=%d durationMs=%.2f",
                            s_consumed,
                            s_cacheOnly,
                            s_backfill,
                            s_terminal,
                            s_clear,
                            s_deferred,
                            s_staleDrop,
                            s_uiDrop,
                            s_durationMs);
                        DIAG_SYSTEM("Waveform", detail);
                        s_lastBackfillResultConsumeLog = resultConsumeEnd;
                        s_consumed = 0;
                        s_cacheOnly = 0;
                        s_backfill = 0;
                        s_terminal = 0;
                        s_clear = 0;
                        s_deferred = 0;
                        s_staleDrop = 0;
                        s_uiDrop = 0;
                        s_durationMs = 0.0;
                        s_wasScanForegroundPending = scanForegroundPending;
                    }
                }
            }
            if (state.postSearchMiniWaveThrottleFrames > 0) {
                --state.postSearchMiniWaveThrottleFrames;
            }
        }

        const bool contextMenuHeavyFreeze =
            ImGui::IsPopupOpen("RowContextMenu") ||
            ImGui::IsPopupOpen("##PlaylistSidebarContext");

        // Progressive cover prefetch (large libraries).
        DevTools_BeginFramePhase("CoverPrefetch");
        if (!contextMenuHeavyFreeze && !state.waveformRebuildActive) {
            BackgroundIoContext coverPrefetchIo = BuildBackgroundIoContextForWindowsMain(
                state, contextMenuHeavyFreeze, IsCoverWallPriorityMode(), isMouseIdle);
            PumpCoverPrefetch(state, coverPrefetchIo);
        }
        DevTools_EndFramePhase();

        // Upload decoded cover textures to the GPU on the UI thread each frame.
        // 非封面墙 Tab 时强力收缩预算以降低播放/主波形加载时的主线程压力（第一刀）
        DevTools_BeginFramePhase("CoverUpload");
        {
            static bool s_lastDetailPending = false;
            static double s_lastDetailCompletedTime = 0.0;
            const bool detailPending = state.audioEngine.isWaveformDetailPending();
            if (s_lastDetailPending && !detailPending)
                s_lastDetailCompletedTime = ImGui::GetTime();
            s_lastDetailPending = detailPending;

            BackgroundIoContext bgIo = BuildBackgroundIoContextForWindowsMain(
                state, contextMenuHeavyFreeze, IsCoverWallPriorityMode(), isMouseIdle);
            bgIo.detailJustCompleted = s_lastDetailCompletedTime > 0.0 &&
                (ImGui::GetTime() - s_lastDetailCompletedTime) < 0.4;
            const auto coverBudget = ResolveCoverWallBackgroundBudget(bgIo);
#if AURA_BUILD_DIAGNOSTICS
            if (coverBudget.maxTextureUploads == 0 && coverBudget.budgetMicroseconds == 0 &&
                std::string(coverBudget.reason) == "foreground") {
                static double s_lastCoverPauseLogAt = 0.0;
                const double now = ImGui::GetTime();
                if ((now - s_lastCoverPauseLogAt) >= 1.0) {
                    s_lastCoverPauseLogAt = now;
                    DIAG_ACTION_FMT("CoverWall", "CoverWallPausedByForeground: reason=%s", coverBudget.reason);
                }
            }
#endif
            CoverModule::PumpCoverTextureUploads(coverBudget.maxTextureUploads,
                                                 coverBudget.budgetMicroseconds);
        }
        DevTools_EndFramePhase();

        // 播放过的文件：自动缓存主波形+封面
        if (!contextMenuHeavyFreeze &&
            !state.waveformRebuildActive &&
            !state.isScanning.load(std::memory_order_acquire)) {
            PumpPlayedMetadataCache(state, isMouseIdle);
            PumpMainDetailPrewarm(state, isMouseIdle);
            BackgroundIoContext bgIo = BuildBackgroundIoContextForWindowsMain(
                state, contextMenuHeavyFreeze, IsCoverWallPriorityMode(), isMouseIdle);
            PumpAudioDecoderPrewarm(state, bgIo);
        }
        
        // Track mouse movement
        static ImVec2 lastMousePos = ImGui::GetMousePos();
        ImVec2 currentMousePos = ImGui::GetMousePos();
        bool mouseMoved = (fabsf(currentMousePos.x - lastMousePos.x) > 2.0f || 
                          fabsf(currentMousePos.y - lastMousePos.y) > 2.0f);
        if (mouseMoved) {
            state.lastMouseMoveTime = std::chrono::steady_clock::now();
            lastMousePos = currentMousePos;
        }

        // 🚀 现代版：静默后台管理器 (仅保留 UI 状态，不执行全扫)

        // 交互优先：主波形正在解码时，不驱动后台 worker，把 I/O 让给大波形

        {
            bool isMainDecoding = state.audioEngine.isWaveformDetailPending();
            const bool contextMenuHeavyFreezeForBgStatus =
                ImGui::IsPopupOpen("RowContextMenu") ||
                ImGui::IsPopupOpen("##PlaylistSidebarContext");
            BackgroundIoContext bgStatusIo = BuildBackgroundIoContextForWindowsMain(
                state, contextMenuHeavyFreezeForBgStatus, IsCoverWallPriorityMode(), isMouseIdle);
            const auto miniStatus = ResolveMiniWaveformWorkerDecision(bgStatusIo);
            if (state.waveformWorker.hasPendingRequests() && !isMainDecoding && !miniStatus.throttleWorker) {
                state.bgCacheRunning = true;
                state.bgCacheProgress = state.waveformWorker.pendingCount();
                state.bgCacheTotal = (int)state.searchResults.size();
            } else {
                state.bgCacheRunning = false;
            }
        }

        if (startupEntryTransitionActive) {
            const auto nowTransition = std::chrono::steady_clock::now();
            const float elapsedMs = (float)std::chrono::duration_cast<std::chrono::milliseconds>(
                nowTransition - startupEntryTransitionStart).count();
            // 硬超时：3 秒后无论如何强制关闭过渡遮罩，防止卡住
            if (elapsedMs > kStartupTransitionTimeoutMs) {
                startupEntryTransitionActive = false;
            } else {
                startupEntryTransitionActive = DrawStartupEntryOverlay(state.settings, elapsedMs, kStartupEntryTransitionMs);
            }
        }

        // Theme inspector: hit-test + overlay
        ThemeInspector::EndFrame();
        if (annotationBridgeEnabled)
            annotationPublisher.Submit(ThemeInspector::CopyLatestSnapshot());
        ThemeInspector::RenderOverlay();

        // Render
#if AURA_BUILD_DIAGNOSTICS && ARIS_PREVIEW_ISOLATION && !AURA_INTERNAL_BETA
        const auto dockTraceGlRenderStart = std::chrono::steady_clock::now();
#endif
        DevTools_BeginFramePhase("GL_Render");
        ImGui::Render();
        BOOL framePresented = FALSE;
        if (!mainWindowHidden) {
            RECT rect;
            GetClientRect(hwnd, &rect);
            glViewport(0, 0, rect.right - rect.left, rect.bottom - rect.top);
            ImVec4 clearBg = GetThemeClearColor();
            glClearColor(clearBg.x, clearBg.y, clearBg.z, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        }
#if ARIS_ENABLE_IMGUI_VIEWPORTS
        if (ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
            ImGui::UpdatePlatformWindows();
            SyncExtraViewportCaptionSubclasses();
            ImGui::RenderPlatformWindowsDefault();
            wglMakeCurrent(g_mainHdc ? g_mainHdc : hdc, g_hRC ? g_hRC : hglrc);
        }
#endif
        FlushDeferredMiniWaveTextureDeletes();
        aura::diagnostics::FrameBudgetDiagnostics::markGlRender();
        DevTools_EndFramePhase();

#if AURA_BUILD_DIAGNOSTICS && ARIS_PREVIEW_ISOLATION && !AURA_INTERNAL_BETA
        const double dockTraceGlRenderMs = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - dockTraceGlRenderStart).count();
        const auto dockTracePresentStart = std::chrono::steady_clock::now();
#endif
        aura::diagnostics::FrameBudgetDiagnostics::endCpuWork();
        DevTools_BeginFramePhase("SwapBuffers");
        aura::diagnostics::FrameBudgetDiagnostics::beginPresent();
        if (!mainWindowHidden)
            framePresented = SwapBuffers(hdc);
        if (framePresented) {
            if (!startupEntryTransitionActive &&
                s_mainUiReady.load(std::memory_order_acquire)) {
                AcknowledgeBootstrapHealthAfterUiPresented();
            }
            aura::diagnostics::FrameBudgetDiagnostics::markPresent();
        }
#if AURA_BUILD_DIAGNOSTICS && !AURA_INTERNAL_BETA
        else AbortFrameBudgetCapture(state, "swap_buffers_failed", "present");
#endif
        aura::diagnostics::FrameBudgetDiagnostics::endPresent();
        DevTools_EndFramePhase();
        aura::diagnostics::FrameBudgetDiagnostics::endFrame();
#if AURA_BUILD_DIAGNOSTICS && ARIS_PREVIEW_ISOLATION && !AURA_INTERNAL_BETA
        const double dockTracePresentMs = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - dockTracePresentStart).count();
        if (dockTraceRequested && !dockTraceWritten) {
            const auto& trace = aura::ui::dock::ActiveDockFrameTrace();
            DockTraceSample sample;
            sample.frameMs = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - dockTraceFrameStart).count();
            sample.renderUiMs = dockTraceRenderUiMs;
            sample.pumpMs = dockTracePumpMs;
            sample.glRenderMs = dockTraceGlRenderMs;
            sample.presentMs = dockTracePresentMs;
            sample.dock = trace;
            if (trace.dragActive) {
                if (dockDragSamples.size() < 60) dockDragSamples.push_back(sample);
            } else if (dockIdleSamples.size() < 120) {
                dockIdleSamples.push_back(sample);
            }
            if (!dockTraceCompleteAttempted &&
                dockIdleSamples.size() == 120 && dockDragSamples.size() == 60) {
                dockTraceCompleteAttempted = true;
                writeDockTrace(true);
            }
        }
#endif
#if AURA_BUILD_DIAGNOSTICS && !AURA_INTERNAL_BETA
        RestoreFrameBudgetVolume(state);
#endif

        // Cap frame cadence when an FPS limit is configured.
        if (state.settings.fpsLimit > 0) {
            static auto s_lastFrameTime = std::chrono::steady_clock::now();
            auto now = std::chrono::steady_clock::now();
            
            // 目标每帧耗时 (毫秒)
            double targetFrameTimeMs = 1000.0 / state.settings.fpsLimit;
            double elapsedMs = std::chrono::duration<double, std::milli>(now - s_lastFrameTime).count();
            
            if (elapsedMs < targetFrameTimeMs) {
                // 1. 粗略睡眠：让出 CPU 时间片给 DAW
                DWORD sleepMs = (DWORD)(targetFrameTimeMs - elapsedMs);
                if (sleepMs > 1) { 
                    Platform::SleepMilliseconds(sleepMs - 1);
                }
                
                // 2. 自旋锁微调：确保毫秒级精度，防止画面撕裂/顿挫
                while (std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - s_lastFrameTime).count() < targetFrameTimeMs) {
                    std::this_thread::yield();
                }
            }
            s_lastFrameTime = std::chrono::steady_clock::now();
        }

        // 动态 VSync 控制
        static int s_currentVSync = 1;
        int targetVSync = (state.settings.fpsLimit == 0 || state.settings.fpsLimit >= 120) ? 0 : 1;
        if (s_currentVSync != targetVSync && wglSwapIntervalEXT) {
            wglSwapIntervalEXT(targetVSync);
            s_currentVSync = targetVSync;
        }

    }

#if AURA_BUILD_DIAGNOSTICS && ARIS_PREVIEW_ISOLATION && !AURA_INTERNAL_BETA
    // Normal early close retains the frames already measured. Never write per frame.
    writeDockTrace(false);
#endif
#if AURA_BUILD_DIAGNOSTICS && !AURA_INTERNAL_BETA
    RestoreFrameBudgetVolume(state, true);
#endif

#endif

    // Cleanup
    logShutdownStage("Begin");
    logShutdownStage("BatchFeatureThreadJoinBegin");
    state.batchFeatureCancelRequested.store(true);
    logShutdownStage("DatabaseSwitchThreadJoinBegin");
    WaitForDatabaseSwitchThread();
    logShutdownStage("DatabaseSwitchThreadJoinEnd");
    if (state.batchFeatureThread.joinable()) {
        state.batchFeatureThread.join();
    }
    logShutdownStage("BatchFeatureThreadJoinEnd");
    state.requestCancelScanTask();
    logShutdownStage("ScanThreadJoinBegin");
    if (state.scanThread.joinable()) {
        state.scanThread.join();
    }
    logShutdownStage("ScanThreadJoinEnd");
    logShutdownStage("RetiredScanThreadsJoinBegin");
    state.joinRetiredScanThreads();
    logShutdownStage("RetiredScanThreadsJoinEnd");
    logShutdownStage("DawSendThreadJoinBegin");
    if (state.dawSendThread.joinable()) {
        state.dawSendThread.join();
    }
    logShutdownStage("DawSendThreadJoinEnd");

    // Stop worker threads before releasing shared state.
    ShutdownSelectedWaveformRebuild();
    logShutdownStage("WaveformWorkerStopBegin");
    state.waveformWorker.stop();
    logShutdownStage("WaveformWorkerStopEnd");
    logShutdownStage("SearchWorkerStopBegin");
    state.searchWorker.stop();
    logShutdownStage("SearchWorkerStopEnd");
    logShutdownStage("BackfillWorkerPoolStopBegin");
    aura::GlobalBackfillWorkerPool().stop();
    logShutdownStage("BackfillWorkerPoolStopEnd");
    logShutdownStage("FileSystemWatcherStopBegin");
    state.database.stopFileSystemWatcher();
    logShutdownStage("FileSystemWatcherStopEnd");
    logShutdownStage("CoverTaskThreadJoinBegin");
    g_coverTaskStop.store(true);
    g_coverTaskCv.notify_all();
    if (g_coverTaskThread.joinable()) {
        g_coverTaskThread.join();
    }
    logShutdownStage("CoverTaskThreadJoinEnd");

    logShutdownStage("RelinkWorkflowShutdownBegin");
    aura::ui::ShutdownRelinkWorkflow(state);
    auto JoinRelinkThreadWithTimeout = [](std::thread& worker) {
        if (!worker.joinable()) {
            return;
        }
        void* handle = reinterpret_cast<void*>(worker.native_handle());
        if (Platform::WaitForNativeThreadHandle(
                handle, static_cast<uint32_t>(aura::ui::kRelinkShutdownJoinTimeoutMs))) {
            worker.join();
            return;
        }
        worker.detach();
    };
    JoinRelinkThreadWithTimeout(state.relinkBannerSearchThread);
    JoinRelinkThreadWithTimeout(state.relinkToolSearchThread);
    for (auto& worker : state.retiredRelinkSearchThreads) {
        JoinRelinkThreadWithTimeout(worker);
    }
    state.retiredRelinkSearchThreads.clear();
    logShutdownStage("RelinkWorkflowShutdownEnd");

    logShutdownStage("BackfillQueueClosingBegin");
    state.backfillQueue.onDbClosing(state.backfillDbGeneration);
    logShutdownStage("BackfillQueueClosingEnd");
    logShutdownStage("DatabaseCloseBegin");
    state.database.close();
    logShutdownStage("DatabaseCloseEnd");
    logShutdownStage("AudioEngineShutdownBegin");
    state.audioEngine.shutdown();
    logShutdownStage("AudioEngineShutdownEnd");

    logShutdownStage("CoverCacheShutdownBegin");
    CoverModule::ShutdownCoverWallPrecache();
    CoverModule::ClearAllCoverCaches();
    logShutdownStage("CoverCacheShutdownEnd");

    logShutdownStage("SettingsSaveBegin");
    SaveAppSettings(state);
    logShutdownStage("SettingsSaveEnd");

// F-035: Remove crash sentinel on clean exit
    {
        std::filesystem::path settingsDir = std::filesystem::path(g_appSettingsPathUtf8).parent_path();
        std::string sentinelPath = (settingsDir / "aris_sentinel.lock").u8string();
        Platform::FileSystem::RemoveFile(sentinelPath);
    }

    logShutdownStage("ImGuiIniSaveBegin");
    ImGui::SaveIniSettingsToDisk(g_imguiIniPathUtf8.c_str());
    logShutdownStage("ImGuiIniSaveEnd");

    annotationPublisher.Stop();
    ThemeInspector::SetBridgeCollectionEnabled(false);

    logShutdownStage("ImGuiShutdownBegin");
    ImGui::DestroyPlatformWindows();
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    logShutdownStage("ImGuiShutdownEnd");

    logShutdownStage("NativeWindowDestroyBegin");
    wglMakeCurrent(nullptr, nullptr);
    wglDeleteContext(hglrc);
    ReleaseDC(hwnd, hdc);
    DestroyWindow(hwnd);
    logShutdownStage("NativeWindowDestroyEnd");

    logShutdownStage("ComShutdownBegin");
    OleUninitialize();
    Platform::UninitThreadCom();
    logShutdownStage("ComShutdownEnd");

    // 释放单实例互斥体
    logShutdownStage("MutexReleaseBegin");
    if (hMutex) {
        ReleaseMutex(hMutex);
        Platform::CloseNativeHandle(hMutex);
    }
    logShutdownStage("Complete");

    return 0;
}
