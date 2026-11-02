#pragma once
#include "InfoPageNavigation.h"
#include "InfoEditSession.h"
// SoundManager - 应用状态模块
// AppState.h - 应用状态、设置、标签页结构
// 注意：这是权威定义，main.cpp 中不再重复定义这些类型

#include <string>
#include <cstdio>  // H2b: getScanFile snprintf
#include <vector>
#include <set>
#include <map>
#include <atomic>
#include <thread>
#include <mutex>
#include <chrono>
#include <future>
#include <algorithm>
#include <unordered_map>

// OpenGL types — use typedef to avoid pulling in GL headers (which need
// platform window headers included first on Windows)
#ifndef __gl_h_
typedef unsigned int GLuint;
#endif
#include <unordered_set>
#include <cstdint>
#include <cstdarg>
#include <ctime>
#include <memory>
#include <optional>
#include <string_view>
#include <cctype>

#include "imgui.h"
#include "Localization.h"
#include "AudioEngine.h"
#include "BackfillQueue.h"
#include "SpectrogramMath.h"
#include "WaveformPresentationPolicy.h"
#include "FolderTreePathModePolicy.h"
#include "AIEngine.h"
#include "NamingScript.h"
#include "BatchRenamePolicy.h"
#include "SendToTargetSurfaceModel.h"
#include "ScanWorkflow.h"
#include "VoiceAssetOrganizer.h"
#include "CueviFeedbackClient.h"

namespace HarkenSearch {

// @stage7-contract
// API contract: AppState and its snapshots are value/state-transfer surfaces,
// not ownership of external audio/database handles. Callers preserve the
// existing UI-thread mutation and snapshot-copy boundaries; fields retain their
// serialized/default meanings and no reference escapes the owning state object.

struct ScanProgressSnapshot {
    int current = 0;
    int total = 1;
};

class MultiFolderScanProgressAggregator {
public:
    explicit MultiFolderScanProgressAggregator(size_t folderCount)
        : folderCount_(folderCount) {}

    ScanProgressSnapshot beginFolder(size_t folderIndex) {
        currentFolderIndex_ = folderIndex;
        currentFolderTotal_ = 0;
        currentFolderCurrent_ = 0;
        return snapshotForLocalProgress(0, 1);
    }

    ScanProgressSnapshot updateCurrentFolder(int localCurrent, int localTotal) {
        currentFolderCurrent_ = std::max(0, localCurrent);
        currentFolderTotal_ = std::max({0, localTotal, currentFolderCurrent_});
        return snapshotForLocalProgress(currentFolderCurrent_, currentFolderTotal_);
    }

    void finishFolder(size_t folderIndex, int finalLocalTotal) {
        if (folderIndex != currentFolderIndex_) return;
        const int finalTotal = std::max({currentFolderTotal_, finalLocalTotal, currentFolderCurrent_, 0});
        completedBeforeCurrentFolder_ += finalTotal;
        currentFolderCurrent_ = 0;
        currentFolderTotal_ = 0;
    }

    ScanProgressSnapshot complete(int finalTotal) const {
        const int safeTotal = std::max(finalTotal, completedBeforeCurrentFolder_);
        return {safeTotal, std::max(safeTotal, 1)};
    }

private:
    ScanProgressSnapshot snapshotForLocalProgress(int localCurrent, int localTotal) const {
        const int safeLocalCurrent = std::max(0, localCurrent);
        const int safeLocalTotal = std::max({safeLocalCurrent, localTotal, 1});
        const int current = completedBeforeCurrentFolder_ + safeLocalCurrent;
        const int total = completedBeforeCurrentFolder_ + safeLocalTotal;
        return {current, std::max(total, 1)};
    }

    size_t folderCount_ = 0;
    size_t currentFolderIndex_ = 0;
    int completedBeforeCurrentFolder_ = 0;
    int currentFolderTotal_ = 0;
    int currentFolderCurrent_ = 0;
};

} // namespace HarkenSearch

// Guided tooltips mirror Settings::showGuidedMode once per frame.
// Disabling the setting suppresses explanatory and label tooltips.

namespace aura::settings_search {

using TokenList = std::vector<std::string>;

inline std::string Normalize(std::string_view text) {
    std::string normalized;
    normalized.reserve(text.size());
    bool lastWasSpace = true;
    for (unsigned char ch : text) {
        if (ch < 0x80) {
            if (std::isalnum(ch)) {
                normalized.push_back(static_cast<char>(std::tolower(ch)));
                lastWasSpace = false;
            } else if (!lastWasSpace) {
                normalized.push_back(' ');
                lastWasSpace = true;
            }
        } else {
            normalized.push_back(static_cast<char>(ch));
            lastWasSpace = false;
        }
    }
    while (!normalized.empty() && normalized.back() == ' ') {
        normalized.pop_back();
    }
    return normalized;
}

inline TokenList TokenizeNormalizedQuery(std::string_view query) {
    TokenList tokens;
    const std::string normalizedQuery = Normalize(query);
    size_t start = 0;
    while (start < normalizedQuery.size()) {
        size_t end = normalizedQuery.find(' ', start);
        if (end == std::string::npos) end = normalizedQuery.size();
        if (end > start) {
            tokens.emplace_back(normalizedQuery.substr(start, end - start));
        }
        start = end + 1;
    }
    return tokens;
}

inline bool MatchesNormalizedTokens(const TokenList& tokens, std::string_view normalizedHaystack) {
    for (const std::string& token : tokens) {
        if (normalizedHaystack.find(token) == std::string::npos) return false;
    }
    return true;
}

inline bool MatchesAllTokens(std::string_view query, std::string_view haystack) {
    const TokenList tokens = TokenizeNormalizedQuery(query);
    if (tokens.empty()) return true;
    return MatchesNormalizedTokens(tokens, Normalize(haystack));
}

}
// All tooltip entry points use the same guided-mode gate.
extern bool g_showGuidedTooltips;

// 说明性 tooltip（解释功能 / 用法 / 快捷键）。
// 使用：把 `if (ImGui::IsItemHovered())` 改为 `if (HoveredForHelp())`。
inline bool HoveredForHelp() {
    return g_showGuidedTooltips && ImGui::IsItemHovered();
}

// 标识性 tooltip（icon-only 按钮的名称）。
// Label tooltips use the same guided-mode gate.
inline bool HoveredForLabel() {
    return g_showGuidedTooltips && ImGui::IsItemHovered();
}

// Gated wrappers for ImGui tooltip entry points.
// Callers use these wrappers so the setting is applied consistently.

namespace HarkenSearch {
    inline void SetTooltip(const char* fmt, ...) IM_FMTARGS(1);
    inline void SetTooltip(const char* fmt, ...) {
        if (!g_showGuidedTooltips) return;
        va_list args;
        va_start(args, fmt);
        ImGui::SetTooltipV(fmt, args);
        va_end(args);
    }
    inline bool BeginTooltip() {
        if (!g_showGuidedTooltips) return false;
        ImGui::BeginTooltip();
        return true;
    }
    inline void EndTooltip() { ImGui::EndTooltip(); }
}
#include "FileBrowser.h"
#include "Database.h"
#include "AsyncWorker.h"
#include "SimilaritySearch.h"
#include "SimilarityWorkflow.h"
#include "HarkenSearchWidgets.h"
#include "UndoSystem.h"
#include "ProductEdition.h"
#include "CoverModule.h"
#include "BpmDetector.h"
#include "TapTempoAccumulator.h"
#include "MusicAnalyzer.h"
#include "MusicQueue.h"
#include "GaplessEngine.h"
#include "ProToolsBridge.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifdef SetWindowPos
#undef SetWindowPos
#endif
#ifdef GetWindowPos
#undef GetWindowPos
#endif
#endif

// Typography Scale

inline constexpr int kFontSteps[]      = { 10, 11, 12, 13, 14, 16, 18, 20, 22, 24 };
inline constexpr int kFontStepCount    = (int)(sizeof(kFontSteps) / sizeof(kFontSteps[0]));
inline constexpr float kTypographyBasePx = 14.0f;

// Chrome (functional UI) uses a fixed font size; == kTypographyBasePx so that
// style.ScaleAllSizes(kChromeFontPx / kTypographyBasePx) == ScaleAllSizes(1.0),
// i.e. chrome geometry is byte-for-byte identical regardless of reading font.
inline constexpr int kChromeFontPx = 14;

// Reading-list rows: extra vertical px added to the reading font height before
// comparing against the density floor. Matches the existing list cell padding.
inline constexpr int kListRowPaddingY = 4;

// Resolve the single reading font size from possibly-legacy config keys.
// Precedence: explicit readingFontSize > legacy listFontSize > legacy fontSize > default.
inline int ResolveReadingFontSize(bool hasReading, int readingVal,
                                  bool hasList, int listVal,
                                  bool hasFont, int fontVal,
                                  int defaultPx) {
    if (hasReading) return readingVal;
    if (hasList)    return listVal;
    if (hasFont)    return fontVal;
    return defaultPx;
}

// Reading-surface row height: never shorter than the density floor, grows with
// the reading font. Tie (densityPx == font+pad) resolves to the density floor.
inline int ComputeReadingRowHeight(int densityPx, int readingFontPx, int padY) {
    const int needed = readingFontPx + padY;
    return densityPx >= needed ? densityPx : needed;
}

struct TypeScale {
    float caption;   // badges, minor labels
    float body;      // body text, list items
    float subhead;   // panel titles, table headers
    float heading;   // section headings
    float display;   // timecode, large numerals
};

inline TypeScale GetTypeScale(float basePx) {
    return {
        basePx * 0.85f,   // caption  ~11 @13
        basePx,            // body     =13
        basePx * 1.15f,   // subhead  ~15 @13
        basePx * 1.38f,   // heading  ~18 @13
        basePx * 1.69f    // display  ~22 @13
    };
}

// Enumerations

// Product-level mode: SFX library vs Music library (F-033)
enum class ProductMode {
    SFX   = 0,   // Sound effects library (default, existing behavior)
    Music = 1    // Music library mode
};

enum class ViewMode {
    Browser = 0,      // File system browser
    Database = 1,     // Database search results
    Favorites = 2,    // Retired; kept as a numeric hole so old tabN_viewMode=2 does not shift Playlist
    Playlist = 3,     // Selected playlist
    PlaylistLibrary = 4 // Distinct files from all playlists
};

inline ViewMode MapLegacyViewMode(int savedViewMode) {
    switch (savedViewMode) {
        case 0: return ViewMode::Browser;
        case 1: return ViewMode::Database;
        case 2: return ViewMode::Database; // former Favorites
        case 3: return ViewMode::Playlist;
        case 4: return ViewMode::PlaylistLibrary;
        default: return ViewMode::Database;
    }
}

// F-033: built-in smart collection presets (applied within ViewMode::Database)
enum class SmartCollection {
    None = 0,
    RecentlyAdded,   // date_added in last 30 days, sorted newest first
    RecentlyPlayed,  // last_played > 0, sorted most-recent first
    Unplayed,        // play_count == 0
    HighlyRated      // rating >= 4
};

enum class PlaybackMode {
    SingleClick,   // Play on single click
    DoubleClick,   // Play on double click
    Toggle         // Toggle play/pause on click
};

// Listening workflow mode — controls autoplay + continuous behavior
enum class ListeningMode {
    Browse   = 0,  // Select=play, end=stop (default for SFX browsing)
    Audition = 1   // Select=play, end=auto-next
};

// List density — controls row height and mini waveform visibility
enum class ListDensity {
    Compact     = 0,  // 20px rows, no mini waveform
    Normal      = 1,  // 28px rows (default)
    Comfortable = 2   // 36px rows
};

enum class SortColumn {
    FileName,
    History,
    Duration,
    Format,
    Size,
    SampleRate,
    Description,
    Keywords,
    Channels,    // 声道数列
    Category,    // 分类列
    Artist,      // 艺术家列
    Album,       // 专辑列
    DateAdded,   // 入库时间列
    Loudness,    // LUFS column
    RMS,         // RMS dB column
    TruePeak,    // dBTP column
    Cover,       // Album art/cover column
    BPM,         // BPM column (F-033)
    MusicalKey,  // Musical key column (F-033)
    Mood,        // Mood column (F-033)
    Rating,      // Star rating column (F-006)
    Similarity,  // Match % column (F-028, visible only in similar mode)
    FilePath     // UX-112: folder path of source file
};

enum class DockSide {
    Left = 0,
    Right = 1,
    Top = 2,
    Bottom = 3
};

enum class AppTheme {
    Dark = 0,        // 玄夜 (经典黑)
    Ocean = 1,       // 极光 (极光绿)
    LapisBlue = 2,   // 琉璃蓝 (现代蓝)
    JadeMist = 3,    // 霁翠 (清夏绿)
    Custom = 7,      // 自定义
    RoseCedar = 10   // 玫瑰雪松 (浅粉)
};

enum class FilterQueryState {
    Idle = 0,
    Debouncing,
    Running,
    Ready,
    Error
};

// Loudness meter channel naming conventions (F-018)
// Controls how N>2 channels are labeled under the meter bars.
// SMPTE      = music/broadcast/WAV native order (default, Pro Tools Music)
// Film       = DVD-A / cinema post (C between L/R, LFE last)
// DolbyAtmos = 7.1.x/5.1.x immersive bed
// AmbiX      = spatial audio ACN+SN3D (1st/2nd/3rd order → 4/9/16 ch)
// Numeric    = plain 1..N fallback
enum class MeterChannelLayout {
    SMPTE = 0,
    Film = 1,
    DolbyAtmos = 2,
    AmbiX = 3,
    Numeric = 4
};

// Keyboard Shortcut System

enum class ShortcutAction {
    // Transport
    PlayPause, Stop, Rewind, FastForward,
    SkipToStart, SkipToEnd,
    PlayRegion, SetInPoint, SetOutPoint, ClearInOut,
    // Navigation
    NextFile, PrevFile, RandomFile, PageDown, PageUp,
    // Edit
    EditFilename, Delete, RevealInExplorer, BatchRename, SelectAll, ClearSelection,
    // View
    Refresh, ToggleFavorite,
    // Audio channel and search shortcuts
    ToggleReverse, ToggleMono, ToggleSoloL, ToggleSoloR, FocusSearch,
    // Pitch
    PitchUp, PitchDown, PitchReset,
    // Send to DAW
    SendToDAW1, SendToDAW2, SendToDAW3, SendToDAW4,
    SpotToDAW,
    // Music mode BPM detection (F-033) — only active in music mode
    DetectBpm,   // B — trigger BPM detection (region if IN/OUT set, else full file)
    TapTempo,    // T — accumulate tap for manual BPM entry
    COUNT
};

struct KeyBinding {
    ImGuiKey key = ImGuiKey_None;
    bool ctrl = false;
    bool shift = false;
    bool alt = false;
    
    bool isPressed() const;
    std::string toString() const;
};

// 7-A slice-1: shortcut table text is ledger-driven (knife-7 F6 family).
// LStr is forward-declared here so this header stays free of generated
// includes (TestAppStateHeaderNativeFree contract); every consumer that
// READS these slots already includes Localization.h.
enum class LStr;

struct ShortcutEntry {
    ShortcutAction action;
    LStr name;
    LStr description;
    KeyBinding defaultKey;
    KeyBinding customKey;
};

// Global shortcut table
extern ShortcutEntry g_shortcuts[];
extern const int SHORTCUT_COUNT;

bool IsShortcutPressed(ShortcutAction action);
std::string GetShortcutString(ShortcutAction action);

// F-027: Shortcut Presets
enum class ShortcutPreset {
    ProTools = 0,   // Default - Pro Tools / Classic Library hybrid
    ClassicLibrary, // Classic library layout
    Minimal,        // Minimal — space/arrows/escape only
    COUNT
};
const char* GetShortcutPresetName(ShortcutPreset preset);
void ApplyShortcutPreset(ShortcutPreset preset);

// View State Snapshot (for browser-like back/forward navigation)

struct ViewStateSnapshot {
    ViewMode viewMode = ViewMode::Database;
    int browserScope = (int)BrowserScope::CurrentDir;
    bool folderFilterActive = false;
    std::string selectedFolderPath;
    bool coverFilterActive = false;
    std::string coverFilterPath;
    int coverFilterIndex = 0;
    std::string searchText;
    int64_t selectedPlaylistId = -1;
    bool playlistAggregatedView = false;
    bool findSameFilenameActive = false;
    std::string findSameFilenameQuery;
    
    bool operator==(const ViewStateSnapshot& o) const {
        return viewMode == o.viewMode &&
               browserScope == o.browserScope &&
               folderFilterActive == o.folderFilterActive &&
               selectedFolderPath == o.selectedFolderPath &&
               coverFilterActive == o.coverFilterActive &&
               coverFilterPath == o.coverFilterPath &&
               coverFilterIndex == o.coverFilterIndex &&
               searchText == o.searchText &&
               selectedPlaylistId == o.selectedPlaylistId &&
               playlistAggregatedView == o.playlistAggregatedView &&
               findSameFilenameActive == o.findSameFilenameActive &&
               findSameFilenameQuery == o.findSameFilenameQuery;
    }
    bool operator!=(const ViewStateSnapshot& o) const { return !(*this == o); }
};

// List Tab Structure

struct CoverWallTextureMirrorEntry {
    uint32_t texId = 0;
    int width = 0;
    int height = 0;
    uint64_t lastUsedFrame = 0;
    uint64_t nonCoverTextureEpoch = 0;
};

struct CoverWallFailedTextureEntry {
    uint64_t lastAttemptFrame = 0;
    double lastAttemptTimeSec = 0.0;
    int failures = 0;
};

struct CoverWallTabSnapshot {
    bool valid = false;
    uint64_t generation = 0;
    std::string dbPath;
    int source = 0;
    bool loaded = false;
    bool discoveryDone = false;
    int64_t lastTotalFileCount = 0;
    int64_t loadTotal = 0;
    bool globalItemsCacheValid = false;
    std::string globalItemsCacheDbPath;
    int64_t globalItemsCacheTotalFileCount = 0;
    uint64_t globalItemsCacheGeneration = 0;
    bool prevUseFacetMode = false;
    uint64_t lastFacetGenSnapshot = 0;
    uint64_t lastPreviewSearchGen = 0;
    uint64_t lastPreviewFacetGen = 0;
    bool lastPreviewHadVisibleItems = false;
    size_t pendingFacetCursor = 0;
    uint64_t fallbackResultsGen = 0;
    uint64_t fallbackFacetsGen = 0;
    size_t lastDisplaySnapshotCursor = 0;
    size_t preloadCursor = 0;
    std::string preloadDbPath;
    std::string globalPageNextCursor;
    bool globalPageHasMore = false;
    bool globalPageLoadingNext = false;
    std::string globalPageDbPath;
    size_t preRequestCursor = 0;
    int lastPreRequestTotalItems = -1;
    size_t lastFirstScreenStart = (size_t)-1;
    size_t lastFirstScreenEnd = 0;
    int lastTotalItems = -1;
    double warmupUntil = 0.0;
    uint64_t nonCoverTextureEpoch = 0;
    std::vector<CoverModule::CoverItem> items;
    std::vector<CoverModule::CoverItem> pendingFacetItems;
    std::vector<CoverModule::CoverItem> filteredItems;
    std::vector<CoverModule::CoverItem> fallbackItems;
    std::vector<CoverModule::CoverItem> globalItemsCache;
    std::vector<CoverModule::CoverItem> displaySnapshot;
    std::vector<CoverModule::CoverItem> preloadFullList;
    std::vector<CoverModule::CoverItem> preloadFirstPage;
    CoverModule::CoverItem pinnedCoverFilterItem;
    std::unordered_set<std::string> discoveryKnown;
    std::unordered_map<std::string, CoverWallTextureMirrorEntry> texCache;
    std::unordered_map<std::string, CoverWallFailedTextureEntry> failedTexCache;
};

struct ListTab {
    int id = 0;
    std::string name;
    ViewMode viewMode = ViewMode::Database;
    int browserScope = (int)BrowserScope::CurrentDir;
    std::vector<AudioRecord> searchResults;
    bool searchResultsSnapshotValid = false;
    uint64_t searchResultsSyncGen = 0;  // generation of last synced searchResults (避免重复深拷贝)
    // T-295: Database revision this tab's searchResults / facets were built
    // against. On activation, if database.dataRevision() has moved past this,
    // the tab re-runs its query so a tag/metadata edit made in
    // another tab is reflected here without manual refresh. 0 = never built.
    uint64_t resultsBuiltAtRevision = 0;
    int selectedResultIndex = -1;
    int64_t selectedPlaylistId = -1;
    bool playlistAggregatedView = false;
    
    // Search/filter state
    char searchBuffer[256] = "";
    bool focusSearchBar = false;
    std::string selectedFolderPath;
    bool folderFilterActive = false;
    bool coverFilterActive = false;
    std::string coverFilterPath;
    int coverFilterIndex = 0;
    std::string artistFilterText;
    std::vector<std::string> artistFilterAlternates;
    std::string albumFilterText;
    std::string categoryFilterText;
    int formatFilterIndex = 0;
    int channelFilterIndex = 0;
    float minDurationFilter = 0.0f;
    float maxDurationFilter = 0.0f;
    int sampleRateFilterIndex = 0;
    float minBpmFilter = 0.0f;
    float maxBpmFilter = 0.0f;
    std::string keyFilterText;
    int namingFilter = 0;
    SmartCollection activeSmartCollection = SmartCollection::None;
    int activeUserCollectionIdx = -1;
    std::map<std::string, int> genrePillState;
    std::map<std::string, int> instrumentPillState;
    bool showWAV = true;
    bool showMP3 = true;
    bool showFLAC = true;
    bool showOGG = true;
    bool showAIFF = true;
    bool showM4A = true;
    bool showWMA = true;
    bool showOther = true;
    
    // Pagination
    int64_t totalFileCount = 0;
    int loadedCount = 0;
    bool hasMoreResults = false;
    bool loadingMore = false;
    std::vector<Database::CoverFacetEntry> coverFacets;
    std::vector<Database::PathFacetEntry> pathFacets;
    bool facetsSnapshotValid = false;
    uint64_t facetsSyncGen = 0;  // generation of last synced facets (避免重复深拷贝)
    CoverWallTabSnapshot coverWallSnapshot;
    
    // Multi-selection
    std::set<int> selectedIndicesSet;
    int lastClickedIndex = -1;
    
    // Per-tab virtual select-all state for large result sets.
    bool allSelected = false;
    
    // Find same filename
    std::string findSameFilenameQuery;
    bool findSameFilenameActive = false;
    
    // Tab visual
    int tabColor = -1;  // -1 = no color, 0-7 = palette index

    // Scroll position persistence across tab switches
    float listScrollY = 0.0f;
    bool restoreScrollY = false;

    // 工作空间锁定：为 true 时 syncToTab 跳过，防止搜索/切目录覆盖本页结果
    bool isLocked = false;
    // 上次被激活时间（ImGui::GetTime()），用于非活动标签页内存卸载
    double lastActivatedAt = 0.0;
    // 已被 performGlobalMemoryGC 卸载数据，切回时需重跑查询
    bool dataUnloaded = false;
    
    // Helpers
    // 判定逻辑优化：全选开启时返回 true，否则查集合
    bool isIndexSelected(int idx) const { 
        return allSelected || selectedIndicesSet.count(idx) > 0; 
    }

    void toggleSelection(int idx) {
        // Clicking one row exits virtual select-all and begins ordinary selection.

        if (allSelected) {
            allSelected = false;
            selectedIndicesSet.clear();
            selectedIndicesSet.insert(idx);
        } else {
            if (selectedIndicesSet.count(idx)) selectedIndicesSet.erase(idx);
        else selectedIndicesSet.insert(idx);
        }
    }
    void selectRange(int from, int to) {
        int s = (from < to) ? from : to;
        int e = (from > to) ? from : to;
        for (int i = s; i <= e; i++) selectedIndicesSet.insert(i);
    }
    void clearSelection() { 
        selectedIndicesSet.clear(); 
        allSelected = false; // Reset virtual select-all state.
        selectedResultIndex = -1; 
    }
    std::vector<AudioRecord> getSelectedRecords() const {
        std::vector<AudioRecord> r;
        if (allSelected) {
            // 全选模式：返回所有搜索结果
            r = searchResults;
        } else {
            // 普通模式：返回集合中的记录
        for (int idx : selectedIndicesSet) {
            if (idx >= 0 && idx < (int)searchResults.size()) r.push_back(searchResults[idx]);
            }
        }
        return r;
    }
};

// Tab Preset: named snapshot of multiple tabs as a workspace

// Lightweight per-tab state: only fields that define "what this tab looks at".
// Excludes derived data (searchResults / facets) — those are rebuilt on load.
struct TabPresetEntry {
    std::string name;
    int         viewMode           = 0;   // (int)ViewMode
    int         browserScope       = (int)BrowserScope::CurrentDir;
    std::string searchBuffer;
    std::string selectedFolderPath;
    bool        folderFilterActive = false;
    bool        coverFilterActive  = false;
    std::string coverFilterPath;
    int         coverFilterIndex   = 0;
    std::string artistFilterText;
    std::vector<std::string> artistFilterAlternates;
    std::string albumFilterText;
    std::string categoryFilterText;
    int         formatFilterIndex = 0;
    int         channelFilterIndex = 0;
    float       minDurationFilter = 0.0f;
    float       maxDurationFilter = 0.0f;
    int         sampleRateFilterIndex = 0;
    float       minBpmFilter = 0.0f;
    float       maxBpmFilter = 0.0f;
    std::string keyFilterText;
    int         namingFilter = 0;
    int         activeSmartCollection = 0;
    int         activeUserCollectionIdx = -1;
    std::map<std::string, int> genrePillState;
    std::map<std::string, int> instrumentPillState;
    bool        showWAV = true;
    bool        showMP3 = true;
    bool        showFLAC = true;
    bool        showOGG = true;
    bool        showAIFF = true;
    bool        showM4A = true;
    bool        showWMA = true;
    bool        showOther = true;
    std::string findSameFilenameQuery;
    bool        findSameFilenameActive = false;
    int64_t     selectedPlaylistId = -1;
    int         tabColor           = -1;
};

struct TabPreset {
    std::string                  name;
    std::vector<TabPresetEntry>  tabs;
    int                          activeIndex = 0;
    int64_t                      createdAt   = 0;   // unix seconds
    bool                         pinned      = false;
};

struct SavedSearchEntry {
    std::string name;
    ViewStateSnapshot snapshot;
    int64_t createdAt = 0;   // unix seconds
    int64_t lastUsed = 0;    // unix seconds
    bool pinned = false;
};

// Send-to-DAW target configuration

// Send mode: unified enum replacing old sendMethod + planned protocol fields
enum class DawSendMode : int {
    Clipboard  = 0,   // Copy path to clipboard (fallback, universal)
    LaunchExe  = 1,   // Launch exe with file args (only when DAW not running)
    HttpApi    = 2,   // REAPER Web Interface HTTP API (spot to timeline)
    NamedPipe  = 3,   // Custom extension IPC (reserved)
};

struct DawTarget {
    std::string name;           // Display name, e.g. "REAPER"
    std::string exePath;        // Full path to DAW executable
    std::string processName;    // Process name for detection, e.g. "reaper.exe"
    DawSendMode mode = DawSendMode::Clipboard;
    int httpPort = 8080;              // HttpApi mode: REAPER web interface port
    std::string reaperActionId;       // REAPER action command ID for Aura_InsertMedia script
    bool enabled = true;
    bool fallbackToClipboard = true;  // Legacy field (Send no longer uses clipboard fallback)
    bool autoDetect = true;           // Auto-detect if process is running

// Pro Tools spot settings (persisted, Pro Tools targets only)
    HarkenSearch::ProTools::SpotMode ptSpotMode = HarkenSearch::ProTools::SpotMode::AtCursor;
    HarkenSearch::ProTools::TrackFallback ptTrackFallback = HarkenSearch::ProTools::TrackFallback::AutoCreateNew;

// Runtime state (not persisted)
    bool isRunning = false;
    double lastDetectTime = 0.0;
};

// Application Settings

struct AppSettings {
    // --- 产品模式 (F-033) ---
    ProductMode productMode = ProductMode::SFX;

    // --- 硬件接口层 ---
    int audioOutputDeviceIndex = -1;  // -1 = 系统默认
    std::string audioOutputDeviceName; // stable guard for persisted output-device index
    int audioPeriodSizeMs = 10;       // 缓冲周期 (ms)，5/10/20/50，重启后生效

    // --- 播放行为层 ---
    PlaybackMode playbackMode = PlaybackMode::SingleClick;
    ListeningMode listeningMode = ListeningMode::Browse;
    bool autoplayOnSelect = true;      // Both compatibility flags derive from listeningMode.
    bool continuousPlayback = false;
    bool shufflePlayback = false;

    // Sync autoplayOnSelect/continuousPlayback from listeningMode
    void syncFromListeningMode() {
        autoplayOnSelect   = true; // always auto-play on select
        continuousPlayback = (listeningMode == ListeningMode::Audition);
    }
    // Derive listeningMode from legacy bools (INI backward compat)
    void deriveListeningModeFromBools() {
        listeningMode = continuousPlayback ? ListeningMode::Audition : ListeningMode::Browse;
    }
    // Cycle to next listening mode (2-state toggle)
    void cycleListeningMode() {
        listeningMode = (listeningMode == ListeningMode::Browse) ? ListeningMode::Audition : ListeningMode::Browse;
        syncFromListeningMode();
    }
    bool skipLeadingSilence = false;   // 跳过文件开头静音
    float silenceThresholdDb = -45.0f; // 静音检测阈值 (dB)，默认稍高使跳过更明显
    float silencePrerollMs = 0.0f;     // Legacy field; leading-silence skip no longer keeps preroll

    // --- 信号保护层 (De-clicking) ---
    bool fadeInOutEnabled = true;
    int fadeInOutMs = 12;              // 消除爆音的淡入淡出 (ms)

    // --- 其余通用设置 ---
    bool showMilliseconds = true;
    bool showFrameNumber = true;
    bool clearSearchOnLaunch = false;
    bool showArtworkInSidePanel = true;
    bool coverWallIntegratedMode = true;
    bool showFileExtension = true;
    bool showGuidedMode = true;        // 引导模式：ON=显示说明性 tooltip；OFF=仅显示标识性 tooltip（icon-only 的名称）
    bool hideWindowOnDrag = true;
    int hideWindowOnDragDelayMs = 400;  // 拖拽开始后延迟多少毫秒再隐藏主窗口，降低误触灵敏度
    bool pausePlaybackOnDragStart = false;  // 拖拽开始时自动暂停正在播放的音频，默认关闭
    aura::folder_tree::PathMode folderTreePathMode = aura::folder_tree::PathMode::Absolute;  // 数据库侧栏树默认从磁盘根展开
    bool enableShowManagerHotkey = true;
    KeyBinding showManagerHotkey = { ImGuiKey_M, true, false, true }; // Ctrl+Alt+M
    bool searchRealtime = true;
    bool autoRenameTabBySearch = true;
    bool tabRandomColor = true;        // 新建标签页自动分配随机颜色
    int closeButtonAction = 0;     // 0=Ask, 1=Minimize to tray, 2=Exit
    int suggestSourceMode = 0;     // 0=All, 1=Filename only, 2=Keywords/Category only
    bool trayMinimizeHintShown = false;
    int rowHeight = 28;
    int fontSize = 13;
    int listFontSize = 13;
    int readingFontSize = 14;   // unified reading-surface font size (replaces listFontSize for reading)
    float settingsDialogWidth = 860.0f;
    float settingsDialogHeight = 560.0f;
    float waveformPanelHeight = 200.0f;
    float sidebarRelatedPathsHeight = 104.0f;
    float meterWidth = 68.0f;  // Loudness meter width (px); user-draggable, clamp [40, 300]
    MeterChannelLayout meterChannelLayout = MeterChannelLayout::SMPTE;  // Channel naming preset for N>2
    AppTheme theme = AppTheme::Dark;
    AppLanguage language = AppLanguage::Chinese;

    // Custom colors (legacy simple custom)
    bool useCustomColors = false;
    ImVec4 customAccentColor = ImVec4(0.337f, 0.612f, 0.839f, 1.0f);
    ImVec4 customWaveformColor = ImVec4(0.686f, 0.843f, 1.000f, 1.0f); // #AFD7FF
    ImVec4 customTextColor = ImVec4(0.800f, 0.800f, 0.800f, 1.0f);
    ImVec4 customBgColor = ImVec4(0.118f, 0.118f, 0.118f, 1.0f);
    ImVec4 customVolumeColor = ImVec4(0.196f, 0.784f, 0.314f, 1.0f);
    ImVec4 customSearchGlowColor = ImVec4(0.337f, 0.612f, 0.839f, 1.0f); // 搜索呼吸灯色

    // Advanced custom theme (from theme editor)
    std::string customThemeName;  // user-given name, empty = no custom theme saved

    // List density
    ListDensity listDensity = ListDensity::Normal;
    int getEffectiveRowHeight() const {
        int densityPx = 28;
        switch (listDensity) {
            case ListDensity::Compact:     densityPx = 20; break;
            case ListDensity::Normal:      densityPx = 28; break;
            case ListDensity::Comfortable: densityPx = 36; break;
        }
        return ComputeReadingRowHeight(densityPx, readingFontSize, kListRowPaddingY);
    }

    // Sorting
    SortColumn sortColumn = SortColumn::FileName;
    bool sortAscending = true;

    // Random sampling
    int randomSampleSize = 500;

    // Waveform caching
    bool autoCacheWaveforms = false;
    bool cacheWaveformsOnScan = false;
    bool idleAssetBackfillEnabled = true;
    int idleAssetBackfillDelayMinutes = 5;
    bool miniWaveformRmsBodyStyle = false;

    bool playerSelectionLink = true;

    // Scan behavior
    bool stopPlaybackOnScan = true;
    // Startup reconcile can recurse every registered library root. Keep it
    // opt-in so large professional libraries reach the UI before maintenance.
    bool autoReconcileOnStartup = false;

    // Waveform behavior
    bool stopCacheOnMouseMove = true;

    // Personal preferences
    std::string userName;
    bool hasCustomAvatar = false;
    int diceBearAvatarIndex = 0;  // 0 = auto-hash by name, 1-36 = explicit pick
    std::string defaultExportPath;
    int defaultTransferFolderStrategy = 0;
    int defaultTransferFileType = 0;
    int defaultTransferExistingBehavior = 0;
    bool defaultTransferUseSourceFormat = true;
    bool defaultTransferEmbedMetadata = true;
    bool keepHarkenSearchOutputProcessingSuffix = true;
    std::string namingPattern = "<Description>_<Category>";
    std::string namingPresetName = "Default";
    std::vector<NamingPreset> namingCustomPresets;
    std::string siliconFlowApiKey = ""; // API key for explicit cover-generation requests.
    bool aiAutoSaveTranscription = false; // deprecated: transcription is confirmed before DB write
    bool rememberLastFolder = true;
    bool showWelcomeOnStartup = false;
    float defaultVolume = 1.0f;
    float defaultPitch = 0.0f;
    float pitchRange = 24.0f;
    PitchMode pitchMode = PitchMode::Semitone;   // Active pitch control mode
    float freqShiftHz = 0.0f;                    // Frequency shift amount (FreqShift mode, ±5000 Hz)

    // Loudness normalization (F-030)
    bool loudnessNormEnabled = false;        // 试听归一化模式开关
    float loudnessNormTargetLUFS = -23.0f;   // 参考电平 (LUFS)，默认 EBU R128

    // --- 多声道路由 (F-033) ---
    int monitorLayout = 0;           // 0=Auto, 1=Stereo, 2=5.1, 3=7.1, 4=7.1.2, 5=7.1.4, 6=Binaural
    int downmixMode = 0;             // 0=Auto(仅当设备不支持多声道时降混), 1=始终降混立体声, 2=直通原生声道
    bool binauralMonitor = false;    // 双耳监听：通过 HRTF 在耳机上模拟多声道空间感
    float downmixCenterDb = -3.0f;   // Center 声道折叠电平 (dB)，ITU-R BS.775 默认 -3dB
    float downmixSurroundDb = -3.0f; // 环绕声道折叠电平 (dB)

    // Waveform display preferences
    int waveformDisplayMode = 0;
    int waveformStyle = DefaultMainWaveformStyle();
    bool showLoudnessMeter = true;
    bool showRMSMeter = true;
    bool showTruePeakMeter = false;
    bool showCoverColumn = true;
    bool showKeyCamelotCol = false; // F-033: Key column displays Camelot code when true, note name when false
    bool showClickRipple = true;   // 列表点击微光点（原扩散波纹）开关
    float defaultFrameRate = 25.0f;
    
    // 精灵（吉祥物）
    bool showMascot = true;
    
    // Sidebar panel visibility
    bool showViewMode = true;
    bool showFolderTree = true;
    bool showMetadataFilter = true;
    bool showPlaylists = true;
    bool showPlaylistQuickBar = true;
    std::vector<int64_t> playlistQuickBarOrder;
    std::vector<int64_t> playlistQuickBarHiddenIds;
    bool showTagCloud = true;
    bool showFolderShortcuts = true; // Controls the folder shortcuts section.

    // Sidebar accordion: which section is expanded (0=视图模式, 1=文件夹树, 2=元数据过滤, 3=播放列表, 4=常用标签, 5=快捷路径)
    int activeSidebarSection = 1; // default: folder tree expanded
    // Per-section animated height (0.0 = fully collapsed/hidden, 1.0 = fully expanded)
    static constexpr int kSidebarSectionCount = 6;
    float sidebarSectionT[kSidebarSectionCount] = {0, 1, 0, 0, 0, 0}; // match default: section 1 open

    // Sidebar section display order: stores sectionId at each visual position.
    // Default preserves pre-reorder visual layout. User may drag-reorder icons in the Activity Bar.
    int sidebarSectionOrder[kSidebarSectionCount] = {0, 5, 1, 4, 2, 3};

    // Sidebar header size preset (VS Code-style Activity Bar Size).
    enum class SidebarSize : int { Compact = 0, Regular = 1, Large = 2 };
    SidebarSize sidebarSize = SidebarSize::Regular;
    
    // File info panel field visibility
    InfoPageNavigation infoPages;
    bool infoShowAudioType = true;
    bool infoShowBitDepth = true;
    bool infoShowChannels = true;
    bool infoShowSampleRate = true;
    bool infoShowTotalFrames = false;
    bool infoShowDuration = true;
    bool infoShowFilename = true;
    bool infoShowPath = true;
    bool infoShowCreateDate = false;
    bool infoShowModifyDate = false;
    bool infoShowEntryDate = false;
    bool infoShowScannedDate = false;
    bool infoShowDescription = true;
    bool infoShowKeywords = true;
    bool infoShowCategory = true;
    bool infoShowBWFields = false;
    bool infoShowRating = true;
    bool infoShowCover = true;

    // Diagnostics mode (AI performance analysis)
    bool diagnosticMode = false;
    bool showDiagnosticOverlay = false;
    bool showGMPanel = false;            // GM debug panel visible
    std::string diagnosticListenCategory;  // 空=全部；非空=只记录该 category，避免缓冲爆满

    // Performance scheduling settings.
    int memoryProfile = 1;            // 0=Eco(节能), 1=Balanced(均衡), 2=Ultra(性能), 3=Custom(自定义)
    int fpsLimit = 60;                // 0=不限制, 30, 60, 120, 144
    int thumbnailMaxResolution = 256; // 封面缩略图精度：128, 256, 512（默认 256 平衡清晰度与 GPU 内存）
    int threadLimit = 0;              // 后台线程策略：0=自动, 1=单线程, -1=满载

    // --- Send to DAW ---
    std::vector<DawTarget> dawTargets;  // 数据层不限数量，UI/快捷键只暴露前 3 槽位

    // --- Shell integration ---
    bool defaultPlayerPrompted = false; // F-034: one-time "set as default player?" prompt shown

    // --- Confirmation dialogs ---
    bool suppressConfirmDialogs = false; // 跳过所有确认提示窗口，操作直接执行
};

// Transient UI State (Unified)

struct UILocalState {
    // Folder click debounce
    std::string pendingFolderPath;
    double pendingFolderTime = 0.0;
    
    // Waveform interaction
    float pendingMiniWaveSeekPos = -1.0f;
    bool miniWaveSeekApplied = false;
    std::string miniWaveSeekTargetPath; // path of file the mini-wave click targeted (empty = current file)
    float lastMiniWaveSeekPos = -1.0f;
    bool isDraggingRegion = false;
    float dragStartPos = 0.0f;
    bool isDraggingToExport = false;
    ImVec2 exportDragStart = {0, 0};
    float exportStartPos = 0.0f;
    bool exportPendingSeek = false;
    bool regionDragPending = false;
    ImVec2 regionDragStartMouse = {0, 0};
    bool regionDragClearPending = false;

    // Edge-drag for adjusting In/Out region endpoints
    bool isEdgeDragging = false;
    bool edgeDragIsInPoint = true;  // true = adjusting In point, false = Out point
    bool edgeDragNoZoom = false;    // lower-zone drag: skip adaptive zoom, no scrub
    float edgeDragMouseDownX = 0.0f; // screen X at edge-drag mouse-down (trim refine / hit stability)

    // Region editor: fade handles (normalized to current region length, 0..1)
    bool isFadeDragging = false;
    bool fadeDragIsFadeIn = true;  // spec "fadeDragKind": true = fade-in, false = fade-out
    float regionFadeInNorm = 0.0f;
    float regionFadeOutNorm = 0.0f;
    bool regionLoopEnabled = false;

    // Region editor: trim-refine (auto-zoom) mode — browsing vs selection vs refine (see waveform region editor spec)
    bool waveformTrimRefineActive = false;
    bool waveformTrimAutoZoomLatched = false;
    float trimRefineZoomBeforeEnter = 1.0f;
    double trimRefineCenterBeforeEnter = 0.5;

    // Export hold timer (prevents accidental export from region interior click)
    bool exportHoldPending = false;
    double exportHoldStartTime = 0.0;
    ImVec2 exportHoldMouseStart = {0, 0};

    // Waveform scrollbar
    bool isDraggingWaveScroll = false;
    float waveScrollDragOffset = 0.0f;
    
    // Volume & Pitch Undo State
    bool volCtrlReset = false;
    bool volWasActive = false;
    float volStartValue = 1.0f;
    
    bool pwWasActive = false;
    float pwStartValue = 0.0f;
    float pwStartNorm = 0.5f;
    float pwStartMouseY = 0.0f;

    void reset() {
        *this = UILocalState();
    }
};

// Operations and Utilities

// Folder Rename Operation

struct FolderRenameOp {
    std::string oldPathUtf8;
    std::string newPathUtf8;
    std::string oldName;
    std::string newName;
    std::string status; // OK/冲突/跳过/错误
    bool willRename = false;
};


// F-012: 重命名历史记录（支持批量撤销）
struct RenameHistoryEntry {
    std::string oldPath;   // 重命名前完整路径
    std::string newPath;   // 重命名后完整路径
    int64_t     fileId = 0;    // DB 记录 ID
    int64_t     timestamp = 0; // Unix timestamp
    bool        undone = false; // 是否已撤销
};

struct RenameHistoryBatch {
    std::vector<RenameHistoryEntry> entries;
    int64_t timestamp = 0;     // 批次时间戳
    std::string templateUsed;
};

// Panel State

struct PanelState {
    bool visible = true;
    bool floating = false;
    float dockedWidth = 250.0f;
    float dockedHeight = 200.0f;
    float floatX = 100.0f;
    float floatY = 100.0f;
    float floatW = 300.0f;
    float floatH = 400.0f;
    DockSide dockSide = DockSide::Left;
    bool wasDragging = false;
    std::string monitorId;
    float monitorRelX = 0.0f;
    float monitorRelY = 0.0f;
    bool hasMonitorRelative = false;
    bool restoreClampPending = true;
};

enum class ModulePanelType {
    FolderShortcuts = 0,
    FolderTree = 1,
    MetadataFilter = 2,
    Playlists = 3,
    Metadata = 4
};

enum class ModulePanelHostMode {
    Floating = 0,
    Docked = 1
};

enum class ModulePanelDockZone {
    Left = 0,
    Right = 1,
    Top = 2,
    Bottom = 3
};

enum class ModulePanelLayoutNodeKind {
    Workspace = 0,
    Panel = 1,
    Split = 2
};

enum class ModulePanelSplitAxis {
    X = 0,
    Y = 1
};

enum class ModulePanelRestorePlacement {
    None = 0,
    EdgeLeft = 1,
    EdgeRight = 2,
    EdgeTop = 3,
    EdgeBottom = 4,
    SplitLeft = 5,
    SplitRight = 6,
    SplitTop = 7,
    SplitBottom = 8
};

struct ModulePanelInstanceState {
    int instanceId = 0;
    ModulePanelType moduleType = ModulePanelType::FolderTree;
    bool visible = true;
    ModulePanelHostMode hostMode = ModulePanelHostMode::Floating;
    ModulePanelDockZone dockZone = ModulePanelDockZone::Left;
    float dockedPrimarySize = 280.0f;
    float floatX = 120.0f;
    float floatY = 120.0f;
    float floatW = 320.0f;
    float floatH = 420.0f;
    bool wasDragging = false;
    bool requestFocus = false;
    bool activeComposition = false;
    bool metadataFollowSelection = true;
    int64_t lockedFileId = -1;
    std::string lockedFilePath;
    std::string lockedDisplayName;
    int restoreTargetNodeId = 0;
    int restoreTargetPanelInstanceId = 0;
    ModulePanelRestorePlacement restorePlacement = ModulePanelRestorePlacement::None;
    bool lastDockedRectValid = false;
    float lastDockedX = 0.0f;
    float lastDockedY = 0.0f;
    float lastDockedW = 0.0f;
    float lastDockedH = 0.0f;
};

struct ModulePanelLayoutNode {
    int nodeId = 0;
    ModulePanelLayoutNodeKind kind = ModulePanelLayoutNodeKind::Workspace;
    ModulePanelSplitAxis splitAxis = ModulePanelSplitAxis::X;
    float splitRatio = 0.5f;
    int firstChildNodeId = 0;
    int secondChildNodeId = 0;
    int panelInstanceId = 0;
};

struct ModulePanelLayoutState {
    int nextInstanceId = 1;
    int nextNodeId = 1;
    int rootNodeId = 0;
    std::vector<ModulePanelLayoutNode> nodes;
};

// Waveform Color Mode

enum class WaveformColorMode {
    Classic = 0,
    FrequencyColored = 1
};

// Folder shortcut entry.
struct FolderShortcut {
    std::string name;
    std::string path;
    bool openInBrowser = false; // Legacy shortcuts retain database navigation.
};

// Play History Item

struct PlayHistoryItem {
    int64_t fileId = -1;
    std::string filePath;
    std::string fileName;
    int64_t timestamp = 0;
};

// Cover Wall Display Source

enum class CoverWallSource {
    Global,              // 全库封面（DB 异步加载）
    Facet,               // 过滤后的 facet 封面
    SearchFallback,      // facet 未就绪时从 searchResults 兜底
    SameFolderFallback,  // 同文件夹扫描结果兜底
    Selection            // 多选模式：仅选中文件的封面
};

// Cover Wall Centralized State（从 main.cpp 46+ 散落 static 收拢而来）

struct CoverWallPageLoadResult {
    std::vector<CoverModule::CoverItem> items;
    std::string nextCursor;
    bool hasMore = false;
    bool fromRegistry = false;
    bool largeLibrary = false;
    int registryEntries = 0;
};

struct CoverWallState {
// 显示源标识
    CoverWallSource source = CoverWallSource::Global;

// 核心封面列表
    std::vector<CoverModule::CoverItem> items;              // 全局/过滤封面（原 s_globalCoverItems）
    std::vector<CoverModule::CoverItem> pendingFacetItems;  // 待渐进加载的 facet 项
    std::vector<CoverModule::CoverItem> filteredItems;      // 多选模式临时容器
    std::vector<CoverModule::CoverItem> fallbackItems;      // searchResults 兜底（原 g_searchFallbackCoverItems）
    std::vector<CoverModule::CoverItem> globalItemsCache;   // Full-library cover wall cache, reused across facet/global switches.

// 加载状态
    bool loaded = false;                    // 全局封面已加载完毕
    bool loading = false;                   // 正在后台加载
    int64_t lastTotalFileCount = 0;         // 上次加载时的文件总数
    bool globalItemsCacheValid = false;
    std::string globalItemsCacheDbPath;
    int64_t globalItemsCacheTotalFileCount = 0;
    uint64_t globalItemsCacheGeneration = 0;

// 异步 Future
    std::future<CoverWallPageLoadResult> entriesFuture;
    std::future<std::vector<CoverModule::CoverItem>> foldersFuture;
    bool entriesInFlight = false;
    bool foldersInFlight = false;
    uint64_t entriesGeneration = 0;
    uint64_t foldersGeneration = 0;
    std::string entriesRequestedCursor;
    bool entriesRequestedAppend = false;
    size_t entriesRequestBaseItemCount = 0;

// 磁盘发现状态
    std::unordered_set<std::string> discoveryKnown;
    bool discoveryDone = false;
    int64_t loadTotal = 0;
    std::vector<CoverModule::CoverItem> displaySnapshot;    // 异步期间的稳定快照
    size_t lastDisplaySnapshotCursor = 0;
    size_t qaDisplayCount = 0;          // Final render-list size after last local overrides.
    int64_t qaRegistryEntries = 0;      // Full-library registry total from the latest paged DB result.
    int64_t qaCandidateTotal = 0;       // 3-way candidate upper bound, refreshed on a throttled dev-only path.
    double qaCandidateLastQueryTime = 0.0;
    size_t qaPrefetchTileCount = 0;     // Tiles in the current viewport prefetch band.
    size_t qaVisibleTileCount = 0;      // Tiles actually rendered by the ImGui clipper this frame.
    size_t qaVisibleReadyCount = 0;     // Viewport tiles with a texture handle this frame.
    size_t qaVisibleLiveTextureCount = 0; // Viewport tiles whose texture handle is still current this frame.
    size_t qaVisibleMirrorCount = 0;    // Viewport tiles backed by a cached texture mirror this frame.
    size_t qaVisibleCurrentMirrorCount = 0; // Viewport tiles whose cached mirror still matches CoverModule state.
    size_t qaVisibleStaleMirrorCount = 0; // Viewport tiles whose cached mirror points at an evicted/replaced texture.
    size_t qaVisibleStaleDrawCount = 0; // Viewport tiles that would draw a stale texture handle this frame.
    size_t qaVisiblePlaceholderCount = 0; // Viewport tiles still showing placeholder UI.
    size_t qaHiddenFailedItemCount = 0; // Confirmed-failed wall items compacted out of the render list.
    size_t qaHiddenPendingItemCount = 0; // Pending wall items temporarily yielded from the render list.

// 预加载缓存（Info Tab 打开时预拉）
    std::vector<CoverModule::CoverItem> preloadFullList;
    std::vector<CoverModule::CoverItem> preloadFirstPage;
    std::string preloadDbPath;
    std::future<CoverWallPageLoadResult> preloadFuture;
    bool preloadInFlight = false;
    uint64_t preloadGeneration = 0;
    size_t preloadCursor = 0;
    std::string globalPageNextCursor;
    bool globalPageHasMore = false;
    bool globalPageLoadingNext = false;
    std::string globalPageDbPath;

    // Snapshot of the cover wall item the user clicked to activate the
    // specific-cover-key filter. Persisted here (instead of being looked up
    // each frame in cw.items / fallbackItems / preloadFullList) because those
    // lists can be cleared / overwritten between click and the next render,
    // leaving the render-time "narrow wall to active cover" override at
    // src/main.cpp:~15822 with a key-only stub that RemoveFailedCoverItems
    // then drops (because folderPathUtf8 is empty), producing "0 个封面".
    // Cleared by the click handler when the user un-pins the filter, and by
    // ClearSpecificCoverKeyFilter / cover wall reset paths.
    CoverModule::CoverItem pinnedCoverFilterItem;

// Facet 代际追踪
    bool prevUseFacetMode = false;
    uint64_t lastFacetGenSnapshot = 0;
    uint64_t lastPreviewSearchGen = 0;
    uint64_t lastPreviewFacetGen = 0;
    bool lastPreviewHadVisibleItems = false;
    size_t pendingFacetCursor = 0;

// Fallback 代际追踪
    uint64_t fallbackResultsGen = 0;
    uint64_t fallbackFacetsGen = 0;

// UI 纹理缓存（TTL/LRU）
    using TexEntry = CoverWallTextureMirrorEntry;
    using FailedTexEntry = CoverWallFailedTextureEntry;
    std::unordered_map<std::string, TexEntry> texCache;
    std::unordered_map<std::string, FailedTexEntry> failedTexCache;

// 渲染状态
    float thumbSize = 110.0f;
    double warmupUntil = 0.0;
    int lastTotalItems = -1;
    float lastScrollY = 0.0f;
    bool lastDetailPending = false;
    double lastDetailCompletedTime = 0.0;

// 预请求 / 渐进加载游标
    uint64_t lastFrameIdx = 0;
    int requestsThisFrame = 0;
    size_t preRequestCursor = 0;
    int lastPreRequestTotalItems = -1;
    size_t lastFirstScreenStart = (size_t)-1;
    size_t lastFirstScreenEnd = 0;

// 多选追踪
    size_t filteredLastSelSize = 0;
    int64_t filteredLastId0 = 0;

// 异步回包校验（防旧库 future 污染新库）
    uint64_t generation = 0;           // 每次 reset() 递增；异步任务提交时记录，回包时比对
    uint64_t viewGeneration = 0;       // 每次 tab/cover-wall display owner 切换递增
    uint64_t entriesViewGeneration = 0;
    uint64_t foldersViewGeneration = 0;
    uint64_t nonCoverTextureEpoch = 0;
    uint64_t lastVisibleMirrorAuditViewGeneration = 0;
    bool texturesPendingClear = false; // 逻辑 reset 后标记，渲染线程在 GL context 安全时执行真正清理

// 诊断
    int failCountPeriod = 0;
    // Previous-state snapshot for change-detection logging (no heartbeat)
    int diagPrevTotal  = -1;
    int diagPrevReady  = -1;
    int diagPrevCache  = -1;
    int diagPrevFail   = -1;

    // Called only when leaving facet mode without a reusable full global cache.
    // Facets share items, but cannot retain the old global pagination identity.
    void prepareGlobalReloadAfterFacet(const std::string& dbPath) {
        const bool reuseReadyPreload =
            !preloadFullList.empty() && preloadDbPath == dbPath &&
            globalPageDbPath == dbPath && preloadGeneration == generation;
        ++viewGeneration;
        items.clear();
        displaySnapshot.clear();
        lastDisplaySnapshotCursor = 0;
        loaded = false;
        loading = false;
        discoveryDone = false;
        discoveryKnown.clear();
        lastTotalFileCount = 0;
        loadTotal = 0;
        globalPageLoadingNext = false;
        if (!reuseReadyPreload) {
            globalPageNextCursor.clear();
            globalPageHasMore = false;
            globalPageDbPath.clear();
            preloadFullList.clear();
            preloadFirstPage.clear();
        }
        // The existing entry loader owns any required first-page restart. Keep
        // its DB identity so the next frame does not launch a duplicate preload.
        preloadDbPath = dbPath;
        preloadCursor = 0;
        // Keep futures and their in-flight flags until the render loop drains
        // them. Old entries/folders are rejected by viewGeneration; a pending
        // global first-page preload remains valid for this database generation.
    }

    void invalidateAfterDatabaseContentChange() {
        ++generation;
        loaded = false;
        loading = false;
        lastTotalFileCount = 0;
        discoveryKnown.clear();
        discoveryDone = false;
        loadTotal = 0;
        displaySnapshot.clear();
        lastDisplaySnapshotCursor = 0;
        entriesRequestedCursor.clear();
        entriesRequestedAppend = false;
        entriesRequestBaseItemCount = 0;
        qaDisplayCount = 0;
        qaPrefetchTileCount = 0;
        qaVisibleTileCount = 0;
        qaVisibleReadyCount = 0;
        qaVisibleLiveTextureCount = 0;
        qaVisibleMirrorCount = 0;
        qaVisibleCurrentMirrorCount = 0;
        qaVisibleStaleMirrorCount = 0;
        qaVisibleStaleDrawCount = 0;
        qaVisiblePlaceholderCount = 0;
        qaHiddenFailedItemCount = 0;
        qaHiddenPendingItemCount = 0;
        failedTexCache.clear();
        preloadFullList.clear();
        preloadFirstPage.clear();
        preloadDbPath.clear();
        preloadCursor = 0;
        globalPageNextCursor.clear();
        globalPageHasMore = false;
        globalPageLoadingNext = false;
        globalPageDbPath.clear();
        entriesRequestedCursor.clear();
        entriesRequestedAppend = false;
        entriesRequestBaseItemCount = 0;
        globalItemsCache.clear();
        globalItemsCacheValid = false;
        globalItemsCacheDbPath.clear();
        globalItemsCacheTotalFileCount = 0;
        globalItemsCacheGeneration = 0;
        lastTotalItems = -1;
        preRequestCursor = 0;
        lastPreRequestTotalItems = -1;
        // Texture ids are non-owning mirrors of CoverModule-owned GL objects.
        // DB-content invalidation may delete/reuse those objects, so clear the
        // mirrors once on the render thread instead of validating every cell.
        texturesPendingClear = true;
    }

// 切库/重置
    void reset() {
        ++generation;  // 递增代际：旧 future 回包时比对，不匹配则丢弃
        source = CoverWallSource::Global;
        items.clear();
        pendingFacetItems.clear();
        filteredItems.clear();
        fallbackItems.clear();
        loaded = false;
        loading = false;
        lastTotalFileCount = 0;
        // futures: 不 reset（可能仍在运行），只清 inFlight 标记让结果被丢弃
        discoveryKnown.clear();
        discoveryDone = false;
        loadTotal = 0;
        displaySnapshot.clear();
        lastDisplaySnapshotCursor = 0;
        qaDisplayCount = 0;
        qaPrefetchTileCount = 0;
        qaVisibleTileCount = 0;
        qaVisibleReadyCount = 0;
        qaVisibleLiveTextureCount = 0;
        qaVisibleMirrorCount = 0;
        qaVisibleCurrentMirrorCount = 0;
        qaVisibleStaleMirrorCount = 0;
        qaVisibleStaleDrawCount = 0;
        qaVisiblePlaceholderCount = 0;
        qaHiddenFailedItemCount = 0;
        qaHiddenPendingItemCount = 0;
        preloadFullList.clear();
        preloadFirstPage.clear();
        preloadDbPath.clear();
        preloadCursor = 0;
        globalPageNextCursor.clear();
        globalPageHasMore = false;
        globalPageLoadingNext = false;
        globalPageDbPath.clear();
        globalItemsCache.clear();
        globalItemsCacheValid = false;
        globalItemsCacheDbPath.clear();
        globalItemsCacheTotalFileCount = 0;
        globalItemsCacheGeneration = 0;
        prevUseFacetMode = false;
        lastFacetGenSnapshot = 0;
        lastPreviewSearchGen = 0;
        lastPreviewFacetGen = 0;
        lastPreviewHadVisibleItems = false;
        pendingFacetCursor = 0;
        fallbackResultsGen = 0;
        fallbackFacetsGen = 0;
        failedTexCache.clear();
        // texCache stores non-owning mirrors of CoverModule texture ids. The
        // renderer must drop the mirrors after CoverModule clears its caches.
        texturesPendingClear = true;
        warmupUntil = 0.0;
        lastTotalItems = -1;
        lastScrollY = 0.0f;
        lastDetailPending = false;
        lastDetailCompletedTime = 0.0;
        lastFrameIdx = 0;
        requestsThisFrame = 0;
        preRequestCursor = 0;
        lastPreRequestTotalItems = -1;
        lastFirstScreenStart = (size_t)-1;
        lastFirstScreenEnd = 0;
        filteredLastSelSize = 0;
        filteredLastId0 = 0;
        failCountPeriod = 0;
    }
};

// F-034: Column View Preset

// F-034: Column view preset — one entry per named preset
inline constexpr int kColumnViewPresetFieldCount = 15;

struct ColumnViewPreset {
    std::string name;       // display name, e.g. "SFX", "My Preset"
    bool        col[kColumnViewPresetFieldCount]  = {};    // Dur,Fmt,Size,SR,Desc,Keywords,Ch,Cat,Art,Album,BPM,Key,Mood,Date,FilePath
    bool        showMeter = false;  // enable LUFS/RMS/TruePeak columns
    bool        builtIn   = false;    // true = user cannot delete
};

// Main Application State

enum class SuggestMode { None, WordSuggest, CmdMenu, ValuePick };

struct AppState {
    AudioEngine audioEngine;
    FileBrowser fileBrowser;
    Database database;
    aura::BackfillQueue backfillQueue;
    int64_t backfillDbGeneration = 0;
    AppSettings settings;
    UILocalState uiState;  // [Unified UI State]
    CoverWallState coverWall;  // 封面墙集中状态（原 main.cpp 46+ static）

    // Deferred productMode toggle (flipping mid-frame trips imgui.cpp:10789 assert)
    bool pendingProductModeToggle = false;
    int pendingProductModeTarget = -1;

    // Nested type aliases (for AppState::PanelState etc. compatibility)
    using PanelState = ::PanelState;
    using ModulePanelType = ::ModulePanelType;
    using ModulePanelHostMode = ::ModulePanelHostMode;
    using ModulePanelDockZone = ::ModulePanelDockZone;
    using ModulePanelLayoutNodeKind = ::ModulePanelLayoutNodeKind;
    using ModulePanelSplitAxis = ::ModulePanelSplitAxis;
    using ModulePanelRestorePlacement = ::ModulePanelRestorePlacement;
    using ModulePanelInstanceState = ::ModulePanelInstanceState;
    using ModulePanelLayoutNode = ::ModulePanelLayoutNode;
    using ModulePanelLayoutState = ::ModulePanelLayoutState;
    using WaveformColorMode = ::WaveformColorMode;
    using RenameMode = ::RenameMode;
    using RenameRule = ::RenameRule;
    using RenamePreview = ::RenamePreview;
    using PlayHistoryItem = ::PlayHistoryItem;

    enum class StartupDatabaseSetupReason : int {
        None = 0,
        FirstRunNoDatabase,
        MissingConfiguredDatabase,
    };

    // Database file management
    std::string currentDbPathUtf8;
    std::vector<std::string> recentDbPathsUtf8;
    bool startupNeedsDatabaseSetup = false;
    StartupDatabaseSetupReason startupDatabaseSetupReason =
        StartupDatabaseSetupReason::None;

    // 异步切库：open 在后台线程，主线程轮询 PumpDatabaseSwitchIfPending
    bool dbSwitchInProgress = false;
    std::string dbSwitchError; // written by worker, consumed only after PollFinished
    std::string dbSwitchRecoveryPath;
    std::string dbSwitchNotice; // UI-owned failure message, independent of the hidden status bar
    double dbSwitchNoticeUntil = 0.0;
    std::string pendingDbPath;
    std::string pendingDbOldPath;
    bool dbSwitchJustCompleted = false;
    bool dbSwitchNeedsRefresh = false;  // deferred RefreshSearchResults after switch
    int dbSwitchSettleFramesRemaining = 0;  // keep post-switch frames input-responsive

    // 启动暖机帧：进入主界面后前 N 帧做轻量绘制，避免首帧 6s+ 卡顿（由 WindowsMain 写入，RenderUI 读取）
    int warmupFramesRemaining = 0;

    bool showDemoWindow = false;
    bool showAboutWindow = false;
    bool showUpdateCenter = false;
    bool updateNoticeSeenThisSession = false;
    bool showScanDialog = false;
    bool scanDialogAutoStart = false; // Folder-tree quick scan intent; manual/drag paths remain confirmed.
    bool showPlaylistDialog = false;
    bool showCreateFolderDialog = false;
    bool showSettingsDialog = false;

    // 子播放列表 dialog state (transient, F-017 v2)
    bool   pendingNewSubDialog = false;
    int64_t pendingNewSubParentId = -1;
    char   pendingNewSubName[128] = "";

    bool   pendingPromoteOrphanDialog = false;
    int64_t pendingPromoteOrphanId = -1;
    char   pendingPromoteDefaultChildName[128] = "";
    char   pendingPromoteUserChildName[128] = "";

    bool   pendingMoveSubDialog = false;
    int64_t pendingMoveSubId = -1;

    bool   pendingDeleteFolderConfirm = false;
    bool   pendingDeleteFolderSecondConfirm = false;
    int64_t pendingDeleteFolderId = -1;

    // 封面拖拽批量加入播放列表 — 超过 500 个文件时弹确认框 (transient, F-017 Task 13)
    bool                  pendingBulkAddPrompt   = false;
    int64_t               pendingBulkAddTargetId = -1;
    std::string           pendingBulkAddTargetName;
    std::vector<int64_t>  pendingBulkAddFileIds;

    bool showNewDbDialog = false;
#ifdef ARIS_PREVIEW_ISOLATION
    bool qaCreateNewDbNow = false;
    std::string qaNewDbName;
#endif
    bool showShortcutsDialog = false;

    // --- 数据库控制中心 (Database Control Center) 状态 ---
    bool showDbManagerDialog = false;        // 控制主管理弹窗显示
    std::string dbManagerSelectedPath;       // 当前在数据网格中选中的数据库路径
    bool showDbDestroyConfirmDialog = false; // 控制高危操作：销毁二次确认弹窗
    std::string dbDestroyErrorMsg;           // 销毁失败时的错误回显

    bool showCloseChoicePopup = false;
    bool closeChoiceRemember = false;
    int closeChoiceSelection = 1;
    bool settingsDirty = false;
    double settingsDirtyAt = 0.0;

    float waveformZoom = 1.0f;
    // 使用 double 存储视口中心：防止在长音频（数小时）+ 高缩放倍率下出现浮点抖动（Jittering）
    double waveformViewCenter = 0.5;
    // Display-only: lerped toward waveformZoom / waveformViewCenter for smooth view transitions (draw + hit-test).
    float waveformDisplayZoom = 1.0f;
    double waveformDisplayViewCenter = 0.5;
    // Lerp tau override: normally kWaveformViewSmoothTauSec; set to slower value for trim-refine enter/exit.
    // Resets to default automatically once display settles.
    float waveformViewAnimTauSec = 0.048f;
    bool waveformZoomAnchorSet = false;
    float waveformZoomAnchor = 0.0f;
    float waveformAmplitudeScale = 1.0f;  // 波形幅度缩放 (0.1 ~ 5.0), 1.0 = default

    // Output device selection
    int selectedOutputDeviceIndex = -1;       // -1 = system default
    std::string outputDeviceName = "Default";
    bool outputDeviceListDirty = true;        // Flag to refresh device list on next UI frame
    std::vector<OutputDeviceInfo> cachedOutputDevices;
    char searchBuffer[256] = "";
    bool focusSearchBar = false;
    bool searchExactMode = false;       // F-034: true = exact match (no prefix *), false = fuzzy (default)
    bool searchExactModeLocked = false; // F-035: user-pinned precise mode — survives keystroke resets
    int  columnViewPreset = 0;          // UX-110 compat: 0=custom, 1-4=built-in; keep in sync
    bool columnViewPresetDirty = false; // UX-110 compat alias → use columnPresetDirty
    // F-034: user-editable preset list
    std::vector<ColumnViewPreset> columnPresets;  // persisted; populated by InitDefaultColumnPresets
    int  activeColumnPreset  = 0;                 // index into columnPresets
    bool columnPresetDirty   = false;             // apply column layout on next table frame (consumed by RenderPresetDial + preset-apply block)
    char playlistNameBuffer[128] = "";
    char folderNameBuffer[128] = "";
    char browserPathBuffer[1024] = "";

    // Suggestions come from FTS5 vocabulary; no in-memory prefix tree is kept.
    std::vector<std::string> searchSuggestCandidates;

    // Search suggestion UI state
    std::string searchSuggestLastQueryNorm;
    int searchSuggestSelected = -1;
    bool searchSuggestUserSelected = false;
    bool searchSuggestPopupOpen = false;
    bool searchSuggestRequestFocus = false;
    bool searchSuggestForceApply = false;
    std::string searchSuggestPendingText;
    SuggestMode searchSuggestMode = SuggestMode::None;
    std::string searchSuggestActiveCommand; // name of the active field command in ValuePick stage

    // Persisted folder shortcuts.
    std::vector<FolderShortcut> folderShortcuts;
    char folderShortcutNameBuffer[128] = "";

    // View mode
    ViewMode viewMode = ViewMode::Database;
    int64_t selectedPlaylistId = -1;

    // Sub-playlist hover popup transient state (not persisted)
    int64_t playlistHoverExpandedParentId = -1;  // currently expanded parent folder; -1 = none
    double  playlistHoverPopupOpenTime    = 0.0; // ImGui::GetTime() when popup opened (close debounce)

    // 子播放列表:当 selectedPlaylistId 是 folder 时,主结果区显示其子的去重并集
    bool playlistAggregatedView = false;

    // F-033: active built-in smart collection (works within ViewMode::Database)
    SmartCollection activeSmartCollection = SmartCollection::None;

    // F-033: Pill tag filter state — 0=neutral, 1=include, 2=exclude
    // Maps norm tag value → pill state (e.g. "electronic" → 1, "piano" → 2)
    std::map<std::string, int> genrePillState;
    std::map<std::string, int> instrumentPillState;

    // Database search results
    std::vector<AudioRecord> searchResults;
    uint64_t searchResultsGen_ = 0;  // 递增：每次 searchResults 被赋值/修改时 ++
    uint64_t facetsGen_ = 0;         // 递增：每次 coverFacets/pathFacets 被赋值时 ++
    // T-295 fix: revision the live searchResults were built against. Updated
    // by RefreshSearchResults (or whoever rebuilds the results) so that
    // syncToTab can stamp the tab with the BUILD-time revision instead of
    // the live DB revision — otherwise an in-flight write between build and
    // tab switch would falsely mark the tab as up-to-date.
    uint64_t searchResultsBuiltAtRevision_ = 0;
    void setSearchResultsBuiltRevision(uint64_t r) { searchResultsBuiltAtRevision_ = r; }
    uint64_t searchResultsBuiltRevision() const { return searchResultsBuiltAtRevision_; }
    // 标记 searchResults 已变化（供 syncToTab 跳过不必要的深拷贝）
    void bumpSearchResultsGen() { ++searchResultsGen_; }
    void bumpFacetsGen()        { ++facetsGen_; }
    std::vector<Playlist> playlists;
    bool showPlaylistRenameDialog = false;
    bool showDawTargetSettings = false;   // DAW target configuration dialog
    std::string lastSendToDAWStatus;      // Status toast after send
    bool lastSendToDAWKeyed = false;      // H1b: row renders from g_lastSendToDAWBridgeStatus (UI-thread L())
    double lastSendToDAWTime = 0.0;       // For timed status display
    std::vector<HarkenSearch::UI::SendTargetHistoryItem> sendTargetHistory;
    std::string lastProToolsConnectionStatus;
    std::string lastProToolsConnectionDetail;
    double lastProToolsConnectionTime = 0.0;
    bool lastProToolsConnectionOk = false;
    bool showProToolsCloseConfirm = false;

    enum class DawSendState { Idle, Sending, Done };
    std::atomic<DawSendState> dawSendState{DawSendState::Idle};
    std::atomic<int> dawSendSent{0};
    std::atomic<int> dawSendTotal{0};
    std::thread dawSendThread;
    int64_t editingPlaylistId = -1;
    int selectedResultIndex = -1;
    bool scrollToSelected = false;

    // Smooth scrolling state driven by target interpolation.
    float scrollTargetY = -1.0f;    // 目标滚动位置 (-1 表示未激活)
    float scrollVelocityY = 0.0f;   // 实时滚动速度
    bool isSmoothScrolling = false; // 是否处于平滑滚动阶段

    // Pagination
    static const int PAGE_SIZE = 500;
    int64_t totalFileCount = 0;
    int loadedCount = 0;
    bool hasMoreResults = false;
    bool loadingMore = false;
    
    // 列表行点击波纹（扩散动画）
    struct Ripple {
        ImVec2 pos;
        float time = -1.0f;
    };
    Ripple listRipple;

    // Tab System
    std::vector<ListTab> listTabs;
    int activeTabIndex = 0;
    int nextTabId = 1;
    int pendingTabSelectionIndex = -1;
    
    ListTab& currentTab();
    int createNewTab(const std::string& name = "");
    void closeTab(int index);
    int reopenClosedTab();  // Ctrl+Z: 恢复最近关闭的 Tab，返回新 index 或 -1
    int duplicateTab(int index);
    std::deque<ListTab> closedTabsStack;  // 最近关闭的 Tab（最多保留 10 个）
    static constexpr int kMaxClosedTabs = 10;
    void syncFromTab();
    void syncToTab();
    void parkLiveResultsInCurrentTab();
    void saveCoverWallDisplaySnapshotForTab(int tabIndex);
    bool restoreCoverWallDisplaySnapshotForTab(int tabIndex);
    void seedCoverWallSnapshotForNewTabFromCurrent(int newTabIndex);
    bool activateTab(int tabIndex, double activationTimeSeconds = -1.0, bool runMemoryGc = true);
    int createAndActivateNewDatabaseTab(double activationTimeSeconds = -1.0);

    // Tab presets — named snapshots of multiple tabs (F-033)
    std::vector<TabPreset> tabPresets;
    static constexpr int   kMaxTabPresets    = 50;
    static constexpr int   kMaxTabsPerPreset = 20;
    int  saveTabPresetFromCurrent(const std::string& name);  // returns new idx, -1 on fail
    bool loadTabPreset(int presetIdx);                       // replace all tabs
    bool deleteTabPreset(int presetIdx);
    bool renameTabPreset(int presetIdx, const std::string& newName);

    // Saved searches - named single-search snapshots.
    std::vector<SavedSearchEntry> savedSearches;
    static constexpr int kMaxSavedSearches = 100;
    int  saveSavedSearchFromCurrent(const std::string& name);
    bool applySavedSearch(int savedSearchIdx);
    bool deleteSavedSearch(int savedSearchIdx);
    bool renameSavedSearch(int savedSearchIdx, const std::string& newName);
    std::vector<int> getSavedSearchDisplayOrder() const;

    // Fixed mini waveform dimensions
    static constexpr float MINI_WAVE_WIDTH = 200.0f;
    static constexpr float MINI_WAVE_HEIGHT = 20.0f;
    int64_t currentFileId = -1;
    int64_t pendingAudioTargetFileId = -1;
    std::string pendingAudioTargetPath;
    bool beginAudioTargetFromSearchResult(int index);
    void clearPendingAudioTarget();
    int64_t activeAudioTargetFileId() const;

    // Scan progress
    // F-001 扫描状态机：与 docs/plans/2026-03-17-F-001-扫描状态机定义.md 一致
    enum class ScanPhase {
        Idle,
        WaveformCache,   // Post-index scan phase: drain requested waveform cache work.
        InitialImport,   // 首次导入
        IncrementalScan, // 增量扫描
        RemoveMissing,   // 删除（磁盘已不存在）
        RenameOrMove,    // 重命名/移动（预留）
        FailureRetry     // 失败重试（预留）
    };
    std::atomic<ScanPhase> scanPhase{ScanPhase::Idle};
    std::atomic<bool> isScanning{false};
    std::atomic<int> scanProgress{0};
    std::atomic<int> scanTotal{0};
    std::atomic<int> scanAddedCount{0};
    std::atomic<int> scanUpdatedCount{0};
    std::atomic<int> scanSkippedCount{0};
    std::atomic<int> scanRemovedCount{0};
    std::atomic<int> scanErrorCount{0};
    std::atomic<int> scanIngestedCount{0};
    std::atomic<HarkenSearch::ScanWorkflowStage> scanWorkflowStage{
        HarkenSearch::ScanWorkflowStage::Idle
    };
    std::atomic<int> scanDiscoveredCount{0};
    std::atomic<int> scanProcessedCount{0};
    std::atomic<int> scanFailedFileCount{0};
    std::atomic<int> scanMiniWaveformsCompleted{0};
    std::atomic<int> scanMiniWaveformsTotal{0};
    std::atomic<bool> scanMiniWaveformsRequired{false};
    std::atomic<bool> scanMiniWaveformsContinueInBackground{false};
    std::atomic<bool> scanForegroundTasksDrained{false};
    std::atomic<bool> scanFatalFailure{false};
    std::atomic<int64_t> scanIngestDurationMs{0};
    std::atomic<int64_t> scanStartedAtMs{0};
    std::atomic<int64_t> scanCompletionReadyAtMs{0};
    std::atomic<int64_t> scanCompletionDurationMs{0};
    std::atomic<bool> scanCancelRequested{false};
    std::atomic<HarkenSearch::ScanStopDisposition> scanStopDisposition{
        HarkenSearch::ScanStopDisposition::None
    };
    bool scanWasCancelled = false;
    bool scanWasPaused = false;
    double scanSummaryToastUntil = 0.0;
    std::atomic<int64_t> scanCompletionSessionId{0};
    std::atomic<bool> scanCompletionPending{false};
    std::atomic<bool> scanCompletionWaveformsPending{false};
    std::atomic<bool> scanCompletionCoversPending{false};
    std::atomic<int> scanCompletionTotal{0};
    std::atomic<int> scanCompletionPendingFiles{0};
    std::atomic<int> scanCompletionMiniUnresolved{0};
    std::atomic<int> scanCompletionCoverUnresolved{0};
    std::atomic<int> scanCompletionMetadataUnresolved{0};
    enum class ScanBackgroundBackfillPhase : int {
        Idle = 0,
        Deferred = 1,
        Resumed = 2,
    };
    std::atomic<int> scanBackgroundBackfillPhase{
        static_cast<int>(ScanBackgroundBackfillPhase::Idle)
    };
    std::atomic<int64_t> scanBackgroundBackfillDeferredWaitMs{0};
    std::atomic<int> scanBackgroundBackfillResumeThreads{0};
    bool scanReadyToWorkPending = false;
    bool scanReadyToWorkWaveformsPending = false;
    bool scanReadyToWorkCoversPending = false;
    bool scanReadyToWorkCoverPostProcessPending = false;
    bool scanReadyToWorkCoverFirstScreenPending = false;
    bool scanReadyToWorkMainWavePending = false;
    std::string scanReadyToWorkDbPath;
    std::string scanReadyToWorkWaveformScope;
    std::string scanReadyToWorkMainWavePath;
    size_t scanReadyToWorkCoverFirstScreenCursor = 0;
    mutable std::recursive_mutex scanReadyToWorkMutex;
    // scanCurrentFile is written from scan thread and read from UI thread.
    // Access must go through setScanFile() / getScanFile() to avoid data race.
    std::mutex scanFileMutex;
    std::string scanCurrentFile_;  // guarded by scanFileMutex
    // H2b (#200 P1-2, constraint 4): scan/worker threads must not call L() at
    // write time; they store the ledger key (+ one %s runtime arg). getScanFile()
    // resolves on the spot - its only consumers are render sites (UI thread,
    // same contract as AppState::TransferStatusText).
    bool scanKeyed_ = false;
    LStr scanKey_{};
    std::string scanArg_;
    void setScanFile(const std::string& s) {
        std::lock_guard<std::mutex> lk(scanFileMutex);
        scanCurrentFile_ = s; scanKeyed_ = false; scanArg_.clear();
    }
    void setScanFileKey(LStr k) {
        std::lock_guard<std::mutex> lk(scanFileMutex);
        scanCurrentFile_.clear(); scanKeyed_ = true; scanKey_ = k; scanArg_.clear();
    }
    void setScanFileKeyStr(LStr k, std::string arg) {
        std::lock_guard<std::mutex> lk(scanFileMutex);
        scanCurrentFile_.clear(); scanKeyed_ = true; scanKey_ = k; scanArg_ = std::move(arg);
    }
    void setScanFileKeyPrefixed(LStr k, std::string prefix) {
        std::lock_guard<std::mutex> lk(scanFileMutex);
        scanCurrentFile_ = std::move(prefix);
        scanKeyed_ = true;
        scanKey_ = k;
        scanArg_.clear();
    }
    std::string getScanFile() {
        std::lock_guard<std::mutex> lk(scanFileMutex);
        if (!scanKeyed_) return scanCurrentFile_;
        const char* fmt = GetLocalizedString(scanKey_);
        std::string body;
        if (scanArg_.empty()) {
            body = fmt ? fmt : "";
        } else {
            char buf[1280];
            std::snprintf(buf, sizeof(buf), fmt, scanArg_.c_str());
            body = buf;
        }
        if (scanCurrentFile_.empty()) return body;
        return scanCurrentFile_ + body;
    }
    std::mutex scanErrorMutex;
    std::vector<std::string> scanErrorMessages_;  // guarded by scanErrorMutex
    void clearScanErrors() {
        std::lock_guard<std::mutex> lk(scanErrorMutex);
        scanErrorMessages_.clear();
    }
    void addScanError(const std::string& s) {
        if (s.empty()) return;
        std::lock_guard<std::mutex> lk(scanErrorMutex);
        if (scanErrorMessages_.size() < 32) scanErrorMessages_.push_back(s);
    }
    void addScanErrors(const std::vector<std::string>& messages) {
        std::lock_guard<std::mutex> lk(scanErrorMutex);
        for (const auto& s : messages) {
            if (scanErrorMessages_.size() >= 32) break;
            if (!s.empty()) scanErrorMessages_.push_back(s);
        }
    }
    std::vector<std::string> getScanErrors() {
        std::lock_guard<std::mutex> lk(scanErrorMutex);
        return scanErrorMessages_;
    }
    std::string lastScannedPath;
    std::vector<std::string> pendingScanFolders; // 拖入但尚未开始扫描的文件夹
    std::string pendingOpenFilePath;             // argv/IPC 传入的音频文件路径，主循环消费后清空
    bool filePlayerFastPathActive = false;       // 双击音频/IPC 打开时跳过素材库预热，优先进入播放
    std::thread scanThread;
    std::vector<std::thread> retiredScanThreads; // 已废弃扫描任务线程，延后 join
    std::mutex scanTaskMutex;
    std::shared_ptr<std::atomic<bool>> scanTaskCancelToken;
    std::atomic<uint64_t> scanTaskGeneration{0};
    bool needsDatabaseRefresh = false;
    bool needsCoverWallReload = false;
    bool coverWallDisplayRefreshRequested = false;
    bool coverWallDatabaseInvalidationRequested = false;
    bool needsTabResultsRefresh = false;  // 切回已卸载数据的标签页后，由 main 调用 RefreshSearchResults

    // Watcher-driven incremental UI refresh (no pagination reset)
    std::atomic<bool> watcherNeedsIncrementalRefresh{false};
    std::mutex watcherDeletedMutex;
    std::vector<std::string> watcherDeletedPaths;
    double lastWatcherRefreshTime = 0.0;
    std::vector<Database::CoverWallEntry> preloadedCoverWallEntries; // Splash 阶段预取，首帧消费后清空
    bool preloadedCoverWallFullyDiscovered = false; // Splash 已完成全库文件夹封面发现，主界面可跳过渐进补扫
    bool autoFilterToScannedPath = false;
    std::atomic<bool> scanDataCommitted{false}; // 扫描过程中数据已提交，需要刷新UI
    double lastIncrementalRefreshTime = 0.0; // 上次渐进式刷新的时间（用于节流）

    static int64_t nowSteadyMs() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    void beginScanTiming() {
        scanStartedAtMs.store(nowSteadyMs(), std::memory_order_release);
        scanCompletionReadyAtMs.store(0, std::memory_order_release);
        scanCompletionDurationMs.store(0, std::memory_order_release);
    }

    void markScanCompletionReadyNow() {
        const int64_t startedAt = scanStartedAtMs.load(std::memory_order_acquire);
        if (startedAt <= 0) return;
        int64_t readyAt = scanCompletionReadyAtMs.load(std::memory_order_acquire);
        if (readyAt <= 0) {
            const int64_t now = nowSteadyMs();
            int64_t expected = 0;
            if (scanCompletionReadyAtMs.compare_exchange_strong(
                    expected, now, std::memory_order_acq_rel, std::memory_order_acquire)) {
                readyAt = now;
            } else {
                readyAt = expected;
            }
        }
        scanCompletionDurationMs.store(
            std::max<int64_t>(0, readyAt - startedAt),
            std::memory_order_release);
    }

    void clearScanTiming() {
        scanStartedAtMs.store(0, std::memory_order_release);
        scanCompletionReadyAtMs.store(0, std::memory_order_release);
        scanCompletionDurationMs.store(0, std::memory_order_release);
    }

    HarkenSearch::ScanWorkflowSnapshot getScanWorkflowSnapshot() const {
        HarkenSearch::ScanWorkflowSnapshot snapshot;
        snapshot.stage = scanWorkflowStage.load(std::memory_order_acquire);
        snapshot.discovered = scanDiscoveredCount.load(std::memory_order_acquire);
        snapshot.processed = scanProcessedCount.load(std::memory_order_acquire);
        snapshot.succeeded = scanIngestedCount.load(std::memory_order_acquire);
        snapshot.skipped = scanSkippedCount.load(std::memory_order_acquire);
        snapshot.failed = scanFailedFileCount.load(std::memory_order_acquire);
        snapshot.miniWaveformsCompleted =
            scanMiniWaveformsCompleted.load(std::memory_order_acquire);
        snapshot.miniWaveformsTotal =
            scanMiniWaveformsTotal.load(std::memory_order_acquire);
        snapshot.miniWaveformsRequired =
            scanMiniWaveformsRequired.load(std::memory_order_acquire);
        snapshot.miniWaveformsContinueInBackground =
            scanMiniWaveformsContinueInBackground.load(std::memory_order_acquire);
        snapshot.foregroundTasksDrained =
            scanForegroundTasksDrained.load(std::memory_order_acquire);
        snapshot.durationMs =
            scanCompletionDurationMs.load(std::memory_order_acquire);
        return snapshot;
    }

    void beginScanWorkflow(bool requireMiniWaveforms) {
        scanWorkflowStage.store(HarkenSearch::ScanWorkflowStage::Scanning,
                                std::memory_order_release);
        scanDiscoveredCount.store(0, std::memory_order_release);
        scanProcessedCount.store(0, std::memory_order_release);
        scanFailedFileCount.store(0, std::memory_order_release);
        scanMiniWaveformsCompleted.store(0, std::memory_order_release);
        scanMiniWaveformsTotal.store(0, std::memory_order_release);
        scanMiniWaveformsRequired.store(requireMiniWaveforms,
                                        std::memory_order_release);
        scanMiniWaveformsContinueInBackground.store(!requireMiniWaveforms,
                                                     std::memory_order_release);
        scanForegroundTasksDrained.store(false, std::memory_order_release);
        scanFatalFailure.store(false, std::memory_order_release);
    }

    void setScanOutcome(int discovered,
                        int processed,
                        int succeeded,
                        int skipped,
                        int failed) {
        const HarkenSearch::ScanOutcomeCounts counts = HarkenSearch::BuildScanOutcomeCounts(
            discovered, processed, succeeded, skipped, failed);
        scanDiscoveredCount.store(counts.discovered, std::memory_order_release);
        scanProcessedCount.store(counts.processed, std::memory_order_release);
        scanIngestedCount.store(counts.succeeded, std::memory_order_release);
        scanSkippedCount.store(counts.skipped, std::memory_order_release);
        scanFailedFileCount.store(counts.failed, std::memory_order_release);
    }

    void setScanMiniWaveformProgress(int completed, int total) {
        const int safeTotal = std::max(0, total);
        scanMiniWaveformsTotal.store(safeTotal, std::memory_order_release);
        scanMiniWaveformsCompleted.store(
            std::clamp(completed, 0, safeTotal), std::memory_order_release);
    }

    void resolveScanWorkflowTerminalStage(bool cancelled, bool fatalFailure) {
        scanFatalFailure.store(fatalFailure, std::memory_order_release);
        HarkenSearch::ScanStopDisposition stopDisposition =
            scanStopDisposition.load(std::memory_order_acquire);
        if (cancelled && stopDisposition == HarkenSearch::ScanStopDisposition::None) {
            stopDisposition = HarkenSearch::ScanStopDisposition::CancelRollback;
        }
        scanWorkflowStage.store(
            HarkenSearch::ResolveTerminalScanWorkflowStage(
                getScanWorkflowSnapshot(), stopDisposition, fatalFailure),
            std::memory_order_release);
    }

    void clearScanWorkflowState() {
        scanWorkflowStage.store(HarkenSearch::ScanWorkflowStage::Idle,
                                std::memory_order_release);
        scanDiscoveredCount.store(0, std::memory_order_release);
        scanProcessedCount.store(0, std::memory_order_release);
        scanFailedFileCount.store(0, std::memory_order_release);
        scanMiniWaveformsCompleted.store(0, std::memory_order_release);
        scanMiniWaveformsTotal.store(0, std::memory_order_release);
        scanMiniWaveformsRequired.store(false, std::memory_order_release);
        scanMiniWaveformsContinueInBackground.store(false,
                                                     std::memory_order_release);
        scanForegroundTasksDrained.store(false, std::memory_order_release);
        scanFatalFailure.store(false, std::memory_order_release);
    }

    void resetScanReadyToWorkState() {
        std::lock_guard<std::recursive_mutex> lk(scanReadyToWorkMutex);
        scanCompletionSessionId.store(0, std::memory_order_release);
        scanCompletionPending.store(false, std::memory_order_release);
        scanCompletionWaveformsPending.store(false, std::memory_order_release);
        scanCompletionCoversPending.store(false, std::memory_order_release);
        scanCompletionTotal.store(0, std::memory_order_release);
        scanCompletionPendingFiles.store(0, std::memory_order_release);
        scanCompletionMiniUnresolved.store(0, std::memory_order_release);
        scanCompletionCoverUnresolved.store(0, std::memory_order_release);
        scanCompletionMetadataUnresolved.store(0, std::memory_order_release);
        scanBackgroundBackfillPhase.store(
            static_cast<int>(ScanBackgroundBackfillPhase::Idle),
            std::memory_order_release);
        scanBackgroundBackfillDeferredWaitMs.store(0, std::memory_order_release);
        scanBackgroundBackfillResumeThreads.store(0, std::memory_order_release);
        scanReadyToWorkPending = false;
        scanReadyToWorkWaveformsPending = false;
        scanReadyToWorkCoversPending = false;
        scanReadyToWorkCoverPostProcessPending = false;
        scanReadyToWorkCoverFirstScreenPending = false;
        scanReadyToWorkMainWavePending = false;
        scanReadyToWorkDbPath.clear();
        scanReadyToWorkWaveformScope.clear();
        scanReadyToWorkMainWavePath.clear();
        scanReadyToWorkCoverFirstScreenCursor = 0;
    }

    // REAPER FileList import runs on the scan worker thread and hands final
    // playlist activation back to the UI thread.
    std::mutex reaperImportResultMutex;
    Database::PlaylistImportResult reaperImportResult_;
    std::string reaperImportPlaylistName_;
    std::atomic<bool> reaperImportResultReady{false};

    void invalidateCoverWallAfterDatabaseContentChange() {
        coverWallDatabaseInvalidationRequested = true;
        needsCoverWallReload = true;
        preloadedCoverWallEntries.clear();
        preloadedCoverWallFullyDiscovered = false;
        coverWall.invalidateAfterDatabaseContentChange();
    }

    void requestCoverWallBackfillDisplayRefresh() {
        coverWallDisplayRefreshRequested = true;
        needsCoverWallReload = true;
        // Keep the current wall stable during incremental backfill refreshes.
        coverWall.failedTexCache.clear();
    }

// Relink session state (F-034)
    // Both sets are in-memory only and therefore clear on application restart.
    std::unordered_set<std::string> relinkIgnoredThisSession;
    bool relinkDeferredThisSession = false;

    struct RelinkBannerState {
        bool        visible          = false;
        bool        promptPending    = false;  // open the modal on the next UI frame
        std::string missingPath;               // path that triggered the prompt
        bool        searching        = false;  // background search in progress
        std::vector<std::string> candidates;   // found candidate paths
        int         selectedCandidate = 0;     // index for multi-candidate combo
    };
    RelinkBannerState relinkBanner;

    // Atomics for merging background search results on main thread
    std::atomic<bool>        relinkBannerSearchDone{false};
    std::vector<std::string> pendingRelinkCandidates; // protected by relinkWorkflowResultMutex
    uint64_t                 pendingRelinkBannerGeneration = 0;
    std::atomic<uint64_t>    relinkBannerSearchGeneration{0};
    std::thread              relinkBannerSearchThread;

    struct RelinkToolState {
        bool        scanning  = false;
        std::string searchRoot;    // extra search dir (empty = watched dirs only)
        std::vector<std::pair<Database::RelinkCandidate, std::vector<std::string>>> results;
        std::unordered_map<int, int> selectedByOrphanId; // cand.id → chosen path index
        bool        confirmed = false;
    };
    RelinkToolState relinkTool;
    std::vector<std::pair<Database::RelinkCandidate, std::vector<std::string>>>
        pendingRelinkToolResults;
    uint64_t                 pendingRelinkToolGeneration = 0;
    std::atomic<uint64_t>    relinkToolSearchGeneration{0};
    std::thread              relinkToolSearchThread;
    std::atomic<bool> relinkToolSearchDone{false};
    std::atomic<bool> relinkSearchCancel{false};
    std::mutex relinkWorkflowResultMutex;
    std::vector<std::thread> retiredRelinkSearchThreads;

    // Test seams: when set, these replace the default candidate-search worker
    // body. Production code always leaves them null.
    std::function<void(AppState&, const std::string&, uint64_t)> relinkBannerSearchFn;
    std::function<void(AppState&, const std::vector<std::string>&, uint64_t)> relinkToolSearchFn;

    uint64_t beginNewScanTask() {
        std::lock_guard<std::mutex> lk(scanTaskMutex);
        // 废弃当前任务：置 cancel，并把线程句柄转入待回收列表（避免 UI 线程 join 卡顿）
        scanStopDisposition.store(
            HarkenSearch::ScanStopDisposition::CancelRollback,
            std::memory_order_release);
        scanCancelRequested.store(true, std::memory_order_release);
        if (scanTaskCancelToken) {
            scanTaskCancelToken->store(true, std::memory_order_release);
        }
        if (scanThread.joinable()) {
            retiredScanThreads.emplace_back(std::move(scanThread));
        }
        scanCancelRequested.store(false, std::memory_order_release);
        scanStopDisposition.store(
            HarkenSearch::ScanStopDisposition::None,
            std::memory_order_release);
        scanTaskCancelToken = std::make_shared<std::atomic<bool>>(false);
        beginScanTiming();
        return scanTaskGeneration.fetch_add(1, std::memory_order_acq_rel) + 1;
    }
    std::shared_ptr<std::atomic<bool>> getActiveScanCancelToken() {
        std::lock_guard<std::mutex> lk(scanTaskMutex);
        return scanTaskCancelToken;
    }
    bool isScanTaskCurrent(uint64_t generation) const {
        return scanTaskGeneration.load(std::memory_order_acquire) == generation;
    }
    bool isScanTaskCancelled(uint64_t generation, const std::shared_ptr<std::atomic<bool>>& token) const {
        if (!isScanTaskCurrent(generation)) return true;
        if (scanCancelRequested.load(std::memory_order_acquire)) return true;
        return token && token->load(std::memory_order_acquire);
    }
    void requestCancelScanTask() {
        scanStopDisposition.store(
            HarkenSearch::ScanStopDisposition::CancelRollback,
            std::memory_order_release);
        scanCancelRequested.store(true, std::memory_order_release);
        std::shared_ptr<std::atomic<bool>> token;
        {
            std::lock_guard<std::mutex> lk(scanTaskMutex);
            token = scanTaskCancelToken;
        }
        if (token) token->store(true, std::memory_order_release);
    }
    void requestPauseScanTask() {
        scanStopDisposition.store(
            HarkenSearch::ScanStopDisposition::PauseKeepCommitted,
            std::memory_order_release);
        scanCancelRequested.store(true, std::memory_order_release);
        std::shared_ptr<std::atomic<bool>> token;
        {
            std::lock_guard<std::mutex> lk(scanTaskMutex);
            token = scanTaskCancelToken;
        }
        if (token) token->store(true, std::memory_order_release);
    }
    void joinRetiredScanThreads() {
        std::vector<std::thread> local;
        {
            std::lock_guard<std::mutex> lk(scanTaskMutex);
            local.swap(retiredScanThreads);
        }
        for (auto& t : local) {
            if (t.joinable()) t.join();
        }
    }

    // Show in Same Folder (异步扫描 + 增量接力)
    std::atomic<uint64_t> sameFolderScanGeneration{0};  // 当前有效 token（新请求会覆盖旧请求）
    bool sameFolderScanActive = false;
    bool sameFolderScanDone = false;
    std::string sameFolderScanPath;
    std::string sameFolderScanError;
    int sameFolderScanAdded = 0;
    // 复用旧容量，避免在 UI 热路径频繁 reallocate
    std::vector<AudioRecord> sameFolderRecycleBuffer;
    
    // Play History
    std::vector<PlayHistoryItem> playHistory;
    static const size_t MAX_PLAY_HISTORY = 100;
    int playHistoryIndex = -1;
    
    void addToPlayHistory(int64_t fileId, const std::string& filePath, const std::string& fileName);
    bool canGoBack() const;
    bool canGoForward() const;
    PlayHistoryItem* goBack();
    PlayHistoryItem* goForward();
    
    // View State History (browser-like back/forward for window state)
    std::vector<ViewStateSnapshot> viewStateHistory;
    int viewStateIndex = -1;
    static const size_t MAX_VIEW_STATE_HISTORY = 50;
    bool viewStateNavigating = false; // true while restoring state (prevent re-push)
    
    ViewStateSnapshot captureViewState() const;
    void pushViewState();
    void restoreViewState(const ViewStateSnapshot& snap);
    bool canViewStateBack() const;
    bool canViewStateForward() const;
    bool viewStateBack();
    bool viewStateForward();
    
    // Drag&drop diagnostics
    std::string lastDropSummary;
    std::string lastDropFirstPath;
    uint64_t lastDropTick = 0;

    // Scan options
    bool scanRecursive = true;
    bool scanUpdateExisting = true;
    bool scanRemoveMissing = false;
    int scanWorkerThreads = 0;
    bool scanBuildSimilarityIndexAfterScan = false;
    std::atomic<bool> pendingPostScanSimilarityIndex{false};
    bool showPostScanSimilarityIndexNotice = false;

    // Artwork panel state
    char artworkSearchBuffer[128] = "";
    std::string artworkBaseFolder;
    std::vector<std::string> artworkAllFolders;
    std::vector<std::string> artworkCoverFolders;
    size_t artworkDiscoverCursor = 0;
    bool artworkDiscoverDone = false;

    // Drag state
    bool isDragging = false;
    int dragStartIndex = -1;

    // Multi-selection
    std::vector<int> selectedIndices;
    bool ctrlPressed = false;
    bool shiftPressed = false;

    // Folder hierarchy
    FolderInfo folderHierarchy;
    FolderInfo databaseFolderHierarchy;
    FolderInfo playlistFolderHierarchy;
    bool folderHierarchyShowingPlaylistScope = false;
    int64_t playlistFolderHierarchyPlaylistId = -1;
    int64_t expandedPlaylistFolderTreePlaylistId = -1;
    void setFolderHierarchy(FolderInfo&& tree, const std::string& /*dbPath*/) {
        databaseFolderHierarchy = tree;
        folderHierarchy = std::move(tree);
        folderHierarchyShowingPlaylistScope = false;
    }
    void setPlaylistFolderHierarchy(FolderInfo&& tree, int64_t playlistId) {
        playlistFolderHierarchy = std::move(tree);
        playlistFolderHierarchyPlaylistId = playlistId;
        folderHierarchy = playlistFolderHierarchy;
        folderHierarchyShowingPlaylistScope = true;
    }
    void restoreDatabaseFolderHierarchy() {
        if (!folderHierarchyShowingPlaylistScope) return;
        folderHierarchy = databaseFolderHierarchy;
        folderHierarchyShowingPlaylistScope = false;
    }
    void clearFolderHierarchyState() {
        folderHierarchy = FolderInfo{};
        databaseFolderHierarchy = FolderInfo{};
        playlistFolderHierarchy = FolderInfo{};
        folderHierarchyShowingPlaylistScope = false;
        playlistFolderHierarchyPlaylistId = -1;
        expandedPlaylistFolderTreePlaylistId = -1;
    }
    std::string pendingRemoveFolderDbPath;
    bool pendingRemoveFolderDbDeleteFiles = false;
    bool showRemoveFolderDbConfirm = false;
    std::string selectedFolderPath;
    bool folderFilterActive = false;
    char folderSearchBuffer[128] = "";
    std::unordered_set<std::string> folderSearchDirectHits;  // terminal matches: stop expansion here
    std::unordered_set<std::string> folderSearchExpandPaths; // ancestor path that should auto-open
    std::string folderTreeScrollTarget;
    bool folderTreeCollapseAll = false;       // 刷新时收起所有层级
    bool folderTreeExpandSearchHitsOnce = false; // 搜索词变化后仅强制展开一帧，之后允许手动折叠
    std::string folderTreeHighlightPath;   // 定位后高亮显示的文件夹路径
    int64_t     folderTreeHighlightUntilMs = 0; // 高亮截止时间 (ms)，0 表示不高亮
    // A flat folder-search cache avoids recursive traversal on every frame.
    std::unordered_set<std::string> folderSearchHits; // 存放匹配搜索词的文件夹完整路径
    std::string lastFolderSearch;                     // 上次搜索词（用于检测变化）
    std::vector<std::string> folderSearchTokens;      // 搜索词按空格拆分的 token 列表（小写）

    // Metadata filters
    char artistFilterBuffer[128] = "";
    std::vector<std::string> artistFilterAlternates;
    char albumFilterBuffer[128] = "";
    char categoryFilterBuffer[128] = "";
    int formatFilterIndex = 0;
    int channelFilterIndex = 0;
    float minDurationFilter = 0.0f;
    float maxDurationFilter = 0.0f;
    int sampleRateFilterIndex = 0;
    int coverFilterIndex = 0;
    float minBpmFilter = 0.0f;
    float maxBpmFilter = 0.0f;
    char keyFilterBuffer[64] = "";

    // Naming convention filter
    int namingFilter = 0;
    
    // File type visibility
    bool showWAV = true;
    bool showMP3 = true;
    bool showFLAC = true;
    bool showOGG = true;
    bool showAIFF = true;
    bool showM4A = true;
    bool showWMA = true;
    bool showOther = true;
    
    bool shouldShowFile(const std::string& extension) const;

    // Playback history tracking
    std::set<std::string> playedFiles;
    bool showHistoryPanel = false;

    // 加载/解码失败提示（状态栏显示一段时间后消失）
    std::string loadErrorToast;
    double loadErrorToastUntil = 0.0;
    std::string backgroundReconcileScanToast;
    double backgroundReconcileScanToastUntil = 0.0;
    bool backgroundReconcileScanToastActive = false;

    // Random sampling
    bool randomSampleMode = false;
    std::set<int64_t> randomShownIds;
    std::vector<int64_t> randomAvailableIds;
    int64_t randomTotalPool = 0;
    std::set<int64_t> playedFileIds;
    bool randomAllPlayed = false;
    static const int RANDOM_PAGE_SIZE = 50;
    
    void resetRandomSampling();

    // Waveform cache / scheduling
    std::map<std::string, WaveformData> waveformCache;
    std::vector<std::string> waveformUrgentPending;
    std::vector<std::string> waveformNormalPending;
    std::set<std::string> waveformPendingSet;
    std::set<std::string> waveformFailed;
    std::set<std::string> waveformMiniCacheMiss;
    std::set<std::string> visibleFilePaths;
    // Search rows should paint before visible mini-wave decode catch-up fills the new viewport.
    int postSearchMiniWaveThrottleFrames = 0;

    static const int MAX_WAVEFORM_CACHE_SIZE = 800;
    static const int WAVEFORMS_PER_FRAME = 6;

    // mini / detail 分层
    static const int WAVEFORM_MINI_PEAKS = 128;
    static const int WAVEFORM_MINI_PEAKS_MUSIC = 160; // F-033: higher precision for music mode

    // 主波形默认高精度
    // Main waveform default precision. Higher zoom-only detail is generated
    // on demand and should not be treated as the persistent default cache tier.
    static const int WAVEFORM_DEFAULT_DETAIL_PEAKS = 3000;
    
// 内存预算管理器（字节预算）
    class MemoryBudget* memoryBudget = nullptr;  // 全局内存预算管理器

// 授权/试用状态
    ProductEdition::LicenseState licenseState;  // 运行时授权 + 试用期状态
    bool integrityCheckFailed = false;          // EXE hash mismatch → tampered binary
    
// 异步预读机制（鼠标悬停200ms后预读）
    struct HoverPrefetchEntry {
        std::string filePath;
        std::chrono::steady_clock::time_point hoverStartTime;
        bool prefetchRequested = false;
    };
    std::unordered_map<std::string, HoverPrefetchEntry> hoverPrefetchMap;  // filePath -> hover entry
    std::mutex hoverPrefetchMutex;
    static const int HOVER_PREFETCH_DELAY_MS = 200;  // 200ms悬停后触发预读
    static const size_t MAX_HOVER_PREFETCH_MAP_SIZE = 1000;
    static const int HOVER_PREFETCH_EXPIRE_SEC = 30;
    
    // Background waveform caching
    std::atomic<bool> bgCacheRunning{false};
    std::atomic<int> bgCacheProgress{0};
    std::atomic<int> bgCacheTotal{0};
    std::chrono::steady_clock::time_point lastInputTime;
    std::chrono::steady_clock::time_point lastMouseMoveTime;
    // Updated only on meaningful interaction (click / key / wheel), NOT on mousemove.
    // Used by the main-loop background throttle to wake fast when the user actually
    // engages with the window, even if GetForegroundWindow() still reports elsewhere.
    std::chrono::steady_clock::time_point lastInteractTime;
    bool showCacheTooltip = true;
    
    // Cover/artwork filter
    std::string coverFilterPath;
    bool coverFilterActive = false;
    uint64_t coverFilterMatchSig = 0;
    std::unordered_set<std::string> coverFilterMatchPaths;
    
    // Cover editing
    bool showCoverImportDialog = false;
    bool coverImportBatch = false;
    std::vector<int64_t> coverImportIds;
    std::vector<std::string> coverImportPaths;
    int coverImportApplyMode = 0;
    bool coverImportDeferredApply = false;
    bool showCoverRemoveConfirm = false;
    
    bool showOriginalAudioWriteConfirm = false;
    enum class OriginalAudioWritePendingKind { None, ImportCover, EmbedBatch };
    OriginalAudioWritePendingKind pendingOriginalAudioWriteKind =
        OriginalAudioWritePendingKind::None;
    std::vector<AudioRecord> pendingOriginalAudioWriteRecords;
    std::vector<std::string> pendingOriginalAudioWritePaths;
    std::vector<uint8_t> pendingOriginalAudioWriteImageData;
    std::string pendingOriginalAudioWriteMimeType;

    // Cover-generation workspace state.
    bool showAICoverStudio = false;
    std::string aiCoverTargetFolder;
    char aiCoverPrompt[1024] = "";
    int aiCoverTaskId = -1;
    bool aiCoverGenerating = false;
    bool coverRemoveBatch = false;
    std::vector<int64_t> coverRemoveIds;
    std::vector<std::string> coverRemovePaths;
    std::string coverImportImagePath;
    
    // Cover clipboard
    std::vector<uint8_t> coverClipboardData;
    std::string coverClipboardMimeType;
    bool hasCoverClipboard() const { return !coverClipboardData.empty(); }
    
    // Cover batch operation progress
    bool coverBatchActive = false;
    int coverBatchTotal = 0;
    int coverBatchProgress = 0;
    std::string coverBatchLabel;
    std::vector<std::string> coverBatchTargets;
    std::vector<std::string> coverBatchSucceeded;
    bool coverBatchIsPaste = true;

    // --- 显式嵌入元数据 (Embed Metadata) 状态 ---
    bool embedBatchActive = false;
    std::vector<AudioRecord> embedBatchTargets;
    int embedBatchTotal = 0;
    int embedBatchProgress = 0;

    // Rebuild all covers (重建当前数据库的所有封面) - 流式分批处理
    bool coverRebuildActive = false;
    int64_t coverRebuildTotal = 0;
    int64_t coverRebuildProgress = 0;
    int coverRebuildFound = 0;
    bool coverRebuildCancel = false;
    int coverRebuildOffset = 0;              // 当前分批偏移
    std::vector<AudioRecord> coverRebuildBatch;  // 当前批次数据
    int coverRebuildBatchIdx = 0;            // 批次内索引
    std::unordered_map<std::string, std::string> coverRebuildFolderCache; // 文件夹→key 缓存
    bool switchToCoverTabAfterRebuild = false;  // 重建完成后自动切换到封面墙标签

    // Rebuild all waveforms (重建全库波形缓存) - 流式分批，每帧提交少量文件避免队列截断
    bool waveformRebuildActive = false;
    bool waveformRebuildCancel = false;
    bool waveformRebuildFullMode = false;
    bool waveformRebuildMiniOnlyMode = false;
    bool adaptiveMiniWaveformFormatRebuildPending = false;
    bool adaptiveMiniWaveformFormatStampPending = false;
    std::set<int64_t> waveformRebuildQueuedIds;
    int waveformRebuildOffset = 0;
    int64_t waveformRebuildTotal = 0;
    int64_t waveformRebuildMiniTotal = 0;
    int64_t waveformRebuildDetailTotal = 0;
    int64_t waveformRebuildProgress = 0;
    int waveformRebuildSkipped = 0;   // already cached, skipped
    int waveformRebuildFailed  = 0;   // in waveformFailed set, skipped
    std::vector<AudioRecord> waveformRebuildBatch;
    int waveformRebuildBatchIdx = 0;

    bool idleAssetBackfillActive = false;
    bool idleAssetBackfillOwnsBackfillPool = false;
    std::string idleAssetBackfillDbPath;
    int64_t idleAssetBackfillStartedMiniProgress = 0;
    int64_t idleAssetBackfillStartedMiniTotal = 0;
    int64_t idleAssetBackfillStartedCoverPending = 0;
    int64_t idleAssetBackfillCompletedMini = 0;
    int64_t idleAssetBackfillCompletedCover = 0;
    std::chrono::steady_clock::time_point idleAssetBackfillNextCheckAt;
    std::chrono::steady_clock::time_point idleAssetBackfillNextProgressPollAt;

    /// 清理无效记录（库与性能）正在后台运行，防止重复点击导致卡死
    bool cleanupInvalidRecordsActive = false;
    std::atomic<int> cleanupInvalidRecordsProgress{0};
    std::atomic<int> cleanupInvalidRecordsTotal{0};
    std::atomic<bool> cleanupInvalidRecordsCancel{false};

    void addPendingWaveform(const std::string& path, bool isVisible);
    void clearPendingWaveforms(bool cancelWorker = true);
    bool popNextPendingWaveform(std::string& out);
    void removePendingWaveform(const std::string& path);

    // Panel states
    PanelState panelSidebar;
    PanelState panelFileInfo = [] {
        PanelState panel;
        panel.dockSide = DockSide::Right;
        panel.dockedWidth = 300.0f;
        return panel;
    }();
    PanelState panelWaveform;
    PanelState panelDatabase;
    PanelState panelSimilarSounds = [] {
        PanelState panel;
        panel.floatW = 640.0f;
        return panel;
    }();
    PanelState panelUCSBrowser = [] {
        PanelState panel;
        panel.visible = false;
        panel.dockSide = DockSide::Left;
        panel.dockedWidth = 480.0f;
        panel.floatW = 720.0f;
        panel.floatH = 540.0f;
        return panel;
    }();
    PanelState panelTagBrowser = [] {
        PanelState panel;
        panel.visible = false;
        panel.dockSide = DockSide::Left;
        panel.dockedWidth = 320.0f;
        panel.floatW = 400.0f;
        panel.floatH = 560.0f;
        return panel;
    }();
    PanelState panelWorkflow = [] {
        PanelState panel;
        panel.visible = false;
        panel.dockSide = DockSide::Right;
        panel.dockedWidth = 560.0f;
        panel.floatW = 780.0f;
        panel.floatH = 600.0f;
        return panel;
    }();
    PanelState panelTransfer = [] {
        PanelState panel;
        panel.visible = false;
        panel.dockSide = DockSide::Right;
        panel.dockedWidth = 480.0f;
        panel.floatW = 600.0f;
        panel.floatH = 600.0f;
        return panel;
    }();
    std::vector<ModulePanelInstanceState> modulePanels;
    ModulePanelLayoutState modulePanelLayout;
    
    // Legacy compatibility
    bool fileInfoDetached = false;
    bool folderBrowserDetached = false;
    DockSide fileInfoDockSide = DockSide::Right;
    DockSide sidebarDockSide = DockSide::Left;

    // Folder auto-rename
    bool showFolderAutoRenameDialog = false;
    char folderRenameRoot[512] = "";
    bool folderRenameRecursiveFolders = false;
    bool folderRenameScanRecursive = true;
    int folderRenameRule = 0;
    std::vector<FolderRenameOp> folderRenamePreview;
    std::string folderRenameLog;
    
    // Meter-column runtime switch: true when any LUFS/RMS/TruePeak list column
    // is visible. Controls whether visible-row waveform requests may run the
    // expensive full-file meter pass (true RMS + LUFS + True Peak).
    bool needsMeterDataComputation = false;
    // Paths with an in-flight meterOnly follow-up request (single-threaded
    // dedup; the waveform worker does not track meterOnly in its own sets).
    std::unordered_set<std::string> meterFollowupPending;

    // Waveform visualization options
    WaveformColorMode waveformColorMode = WaveformColorMode::FrequencyColored;
    bool showLoudnessMeter = true;
    bool showSpectrogram = true;
    bool showLoudnessOverlay = false;
    
    // Drag cursor style
    int dragCursorStyle = 0;
    
    // Spectrogram settings
    float spectrogramBlend = GetReadableSpectrogramPreset().blend;
    int spectrogramFFTSize = GetReadableSpectrogramPreset().fftSize;
    int spectrogramTexWidth = GetReadableSpectrogramPreset().textureWidth;
    bool spectrogramSplit = GetReadableSpectrogramPreset().split;
    GLuint spectrogramTexture = 0;
    int spectrogramWidth = 0;
    int spectrogramHeight = 0;
    std::string spectrogramFilePath;
    uint64_t spectrogramTimelineFrames = 0;
    uint32_t spectrogramSampleRate = 0;
    bool spectrogramNeedsUpdate = true;
    int lastSpectrogramFFTSize = 0;
    
    // Loudness meter values — per-channel (supports mono / stereo / N-channel Atmos etc.)
    std::vector<float> currentPeaks;      // smoothed peak per channel
    std::vector<float> currentRmses;      // smoothed RMS per channel
    std::vector<float> peakHolds;         // peak-hold value per channel
    std::vector<float> peakHoldFreezes;   // hold-decay timer per channel (moved from main.cpp statics)

    // Legacy L/R aggregates — kept in sync each frame for main.cpp:33385 Transport energy glow
    float currentPeakL = 0.0f;
    float currentPeakR = 0.0f;
    float currentRmsL = 0.0f;
    float currentRmsR = 0.0f;
    float peakHoldL = 0.0f;
    float peakHoldR = 0.0f;
    float peakHoldDecay = 0.0f;
    
    // Context menu operations
    bool showEditFilenameDialog = false;
    char editFilenameBuffer[512] = "";
    int64_t editFilenameRecordId = -1;
    std::string editFilenameOriginalPath;
    
    // Metadata edit state
    bool showMetadataEditDialog = false;
    bool metadataEditMode = false;
    InfoEditSession infoEditSession;
    std::vector<AudioRecord> infoEditRecords;
    std::string infoEditDatabasePath;
    std::string infoEditError;
    std::function<void()> infoEditAfterLeave;
    int infoEditVisibleMask = 15;
    int64_t infoEditSelectionId = -1;
    int64_t metadataEditFileId = -1;
    bool metadataBatchMode = false;
    std::vector<int64_t> metadataBatchIds;
    char metadataDescBuffer[1024] = "";
    char metadataKeywordsBuffer[512] = "";
    char metadataCategoryBuffer[256] = "";
    
    // Delete confirmation
    bool showDeleteConfirmDialog = false;
    bool deleteFromFileSystem = false;
    std::vector<int64_t> pendingDeleteIds;
    std::vector<std::string> pendingDeletePaths;
    
    // Find same filename
    std::string findSameFilenameQuery;
    bool findSameFilenameActive = false;
    
    // Multi-selection
    std::set<int> selectedIndicesSet;
    int lastClickedIndex = -1;
    
    // Workspace virtual select-all state for large result sets.
    bool allSelected = false;

    // Browser multi-selection + sorting
    std::set<int> browserSelectedIndicesSet;
    int browserLastClickedIndex = -1;
    SortColumn browserSortColumn = SortColumn::FileName;
    bool browserSortAscending = true;
    std::string browserSortPath;
    int browserSortEntryCount = 0;
    std::vector<int> browserSortedIndices;

    // Browser scope & bridging state
    char browserFilterBuffer[256] = "";                // in-view quick filter
    std::string browserLocateAnchorPath;               // file to highlight after DB->browser jump
    std::string browserLocateSourceLabel;              // e.g. "from database result: Kick_01.wav"
    double browserLocateSourceUntilMs = 0.0;           // auto-dismiss timestamp for source label

    // Browser DB metadata cache — populated when current directory changes.
    // Reuse the complete database record so Browser follows new metadata fields
    // without maintaining a second, partial projection model.
    std::unordered_map<std::string, AudioRecord> browserMetaCache;
    enum class BrowserMetadataLoadState : uint8_t { Loading, Available, Unavailable };
    std::unordered_map<std::string, BrowserMetadataLoadState> browserMetaLoadStates;
    std::string browserMetaCacheDir;   // which directory the cache was built for
    
    // Batch rename dialog
    bool showBatchRenameDialog = false;
    int batchDialogTab = 0;
    
    // Batch metadata editing
    char batchMetaDescription[512] = "";
    char batchMetaKeywords[512] = "";
    char batchMetaCategory[256] = "";
    int batchMetaAppendMode = 0;
    
    // Batch rename rules
    std::vector<RenameRule> renameRuleQueue;
    char renameSearchBuffer[256] = "";
    char renameReplaceBuffer[256] = "";
    int renameModeIndex = 0;
    int renameDeleteToEndOccurrence = 0; // 0 = first match, 1 = last match
    int renameRuleSelectedIndex = -1;
    std::vector<RenamePreview> renamePreviewList;

    // F-012: 重命名历史（最多保留 20 批次）
    std::deque<RenameHistoryBatch> renameHistory;
    static constexpr int kMaxRenameHistoryBatches = 20;

    // F-025: Global Undo/Redo Stack
    UndoStack undoStack;
    bool showUndoHistoryPanel = false;

    // Selection helpers
    std::vector<AudioRecord> getSelectedRecords() const;
    void clearSelection();
    bool isIndexSelected(int idx) const;
    void toggleSelection(int idx);
    void selectRange(int start, int end);
    
    // AI audio processing
    std::atomic<bool> aiEngineInitialized{false};
    std::atomic<bool> aiModelLoaded{false};
    bool showAIPanel = false;
    std::vector<int> aiPendingTaskIds;
    std::map<int64_t, std::string> aiTranscriptCache;
    std::map<int64_t, TranscriptionResult> aiTranscriptResultCache;
    std::map<int64_t, VoiceAssetOrganization> aiVoiceAssetOrganizationCache;
    int aiRecognitionLanguageMode = 1; // 0=auto, 1=simplified Chinese, 2=English
    int aiCurrentTranscriptionTaskId = -1;
    int64_t aiCurrentTranscriptionFileId = 0;
    int aiSelectedTranscriptSegment = -1;
    uint64_t aiTranscriptionGeneration = 0;
    std::map<int64_t, uint64_t> aiTranscriptionFileGenerations;
    std::map<int, uint64_t> aiTranscriptionTaskGenerations;
    std::map<int, int64_t> aiTranscriptionTaskFileIds;
    std::map<int, std::string> aiTranscriptionTaskFileNames;
    std::map<int, std::string> aiTranscriptionTaskKeywords;
    // fileId -> [{value, value_zh, dimension, confidence}]
    struct CachedTagSuggestion {
        std::string value;
        std::string value_zh;
        int dimension;      // TagDimension cast to int (avoid header dep)
        float confidence;
    };
    std::map<int64_t, std::vector<CachedTagSuggestion>> aiTagSuggestionCache;
    std::atomic<bool> aiModelDownloading{false};
    std::atomic<float> aiModelDownloadProgress{0.0f};
    std::string aiModelName = "base";

    // 新模块 UI 状态 (UCS / 标签 / 嵌入 / 命名 / 传输 / 工作流 / 主题)

    // UCS 分类浏览器
    bool showUCSBrowser = false;
    char ucsBrowserSearch[128] = "";
    int ucsBrowserSelectedIdx = -1;

    // 标签系统面板
    bool showTagBrowser = false;

    // 元数据嵌入对话框
    bool showEmbedDialog = false;

    // 命名脚本对话框
    bool showNamingDialog = false;
    char namingPatternBuffer[256] = "<Description>_<Category>";
    int namingPresetIndex = 0;

    // FTS 重建进度（后台线程写，主线程读）
    std::atomic<float> ftsRebuildProgress{-1.0f}; // -1 = not running
    std::atomic<bool> ftsRebuildCancel{false};

    // 传输引擎对话框
    bool showTransferDialog = false;
    char transferDestFolder[512] = "";
    int transferFolderStrategy = 0;
    int transferFileType = 0;       // index into getFileTypeChoices()
    int transferSampleRate = 0;     // index into getSampleRateChoices()
    int transferBitDepth = 0;       // index into getBitDepthChoices()
    int transferExistingBehavior = 0; // index into getExistingBehaviorChoices()
    bool transferUseSourceFormat = true;
    bool transferEmbedMetadata = true;
    std::vector<AudioRecord> transferSelectedFiles;  // files queued for export
    bool transferInProgress = false;
    std::string transferStatusMessage;
    enum class TransferStatusKind { Neutral, Error, Fail };  // Status kind drives color independently of localized text.
    TransferStatusKind transferStatusKind = TransferStatusKind::Neutral;
    // H1 (P1-2): worker threads must NOT call L()/GetLocalizedString (unsynchronized
    // g_currentLanguage read). Workers store ledger key + args; the UI render site
    // formats on the main thread via TransferStatusText().
    enum class TransferStatusArgMode { Literal, TwoStrings, OneInt, TwoInts };
    LStr transferStatusFmtKey{};
    TransferStatusArgMode transferStatusArgMode = TransferStatusArgMode::Literal;
    std::string transferStatusArg1, transferStatusArg2;
    int transferStatusN1 = 0, transferStatusN2 = 0;
    void setTransferStatus(std::string msg, TransferStatusKind kind) {
        transferStatusMessage = std::move(msg);
        transferStatusKind = kind;
        transferStatusArgMode = TransferStatusArgMode::Literal;
    }
    void setTransferStatusFmt(LStr key, TransferStatusKind kind, std::string a1, std::string a2) {
        transferStatusMessage.clear();
        transferStatusKind = kind;
        transferStatusFmtKey = key;
        transferStatusArgMode = TransferStatusArgMode::TwoStrings;
        transferStatusArg1 = std::move(a1);
        transferStatusArg2 = std::move(a2);
    }
    void setTransferStatusCount(LStr key, TransferStatusKind kind, int n1, int n2) {
        transferStatusMessage.clear();
        transferStatusKind = kind;
        transferStatusFmtKey = key;
        transferStatusArgMode = (n2 >= 0) ? TransferStatusArgMode::TwoInts : TransferStatusArgMode::OneInt;
        transferStatusN1 = n1;
        transferStatusN2 = (n2 >= 0) ? n2 : 0;
    }
    std::string TransferStatusText() const;  // UI-thread only (defined in AppState.cpp)
    int transferCompletedCount = 0;
    int transferErrorCount = 0;
    int transferTotalCount = 0;

    struct ChannelExportJobResult {
        bool ok = false;
        std::string outputPath;
        std::string errorMessage;
    };

    struct ChannelExportJob {
        std::future<ChannelExportJobResult> future;
        std::string sourceName;
        std::string channelLabel;
    };
    std::vector<ChannelExportJob> channelExportJobs;

    // 工作流引擎对话框
    bool showWorkflowDialog = false;
    int workflowSelectedNode = -1;

    // 高级主题编辑器
    bool showThemeEditor = false;
    int themeEditorPresetIdx = 0;

    // 元数据字段管理
    bool showFieldManager = false;
    int fieldTemplateIndex = 0;

    // Send-to-DAW widget state
    int selectedDAWIndex = 0;  // default: first dawTargets entry (REAPER — Named Pipe)

// 异步预读机制函数声明
    void recordHoverStart(const std::string& filePath);
    void recordHoverEnd(const std::string& filePath);
    void processHoverPrefetch();
    
// 异步工作线程
    WaveformWorker waveformWorker;
    SearchWorker searchWorker;
    DebounceTimer searchDebounce{80}; // 搜索输入 80ms 防抖（本地 FTS5 查询足够快，降低感知延迟）
    
    // 异步搜索状态
    uint64_t pendingSearchRequestId = 0;
    uint64_t pendingFacetRequestId = 0;
    bool asyncSearchInProgress = false;
    bool pendingCountFollowUp = false;  // 滚动/输入中曾用 deferCount，稳定后需补 count+facets
    std::string lastSearchSubmitted; // 上次提交的搜索关键字（用于检测变化）
    std::string lastAppliedSearchText; // 上次实际执行搜索的关键词（用于去重）
    bool suppressNextRealtimeSearchCommit = false; // Programmatic QA search already submitted this text.
    uint64_t filterQueryGeneration = 0;
    uint64_t filterQueryAppliedGeneration = 0;
    FilterQueryState filterQueryState = FilterQueryState::Idle;
    std::string filterQueryError;
    double lastFilterQueryMs = 0.0;

    // 当前筛选结果对应的 Facet（封面/路径）
    std::vector<Database::CoverFacetEntry> currentCoverFacets;
    std::vector<Database::PathFacetEntry> currentPathFacets;

    // 主波形 detail 延迟请求：停 120–200ms 后再请求，切文件则取消前一次
    std::string pendingDetailFilePath;
    double pendingDetailTime = 0.0;
    int pendingDetailPeaks = 0;
    MainWaveformDetailRequestTracker mainWaveformDetailRequestTracker;

    // 最近一次选中/loadFileAsync 时间（用于封面墙预算：切歌后短时间暂停或极低预算）
    double lastSelectionOrLoadTime = 0.0;
    double lastProductModeSwitchTime = 0.0;

    // One-shot refresh hints consumed by RefreshSearchResults().
    bool suppressFacetsForNextRefresh = false;
    bool preferDeferredCountForNextRefresh = false;
    // One-shot: view navigation can request rows-first paint without the
    // million-row deferred count follow-up competing with the next UI frames.
    bool skipDeferredCountFollowUpForNextRefresh = false;
    bool preservePathFacetsForNextRefresh = false;

    // Memory health tracking.
    size_t lastMemoryUsageMB = 0;
    double lastGCPerformanceTime = 0.0;
    
    // 主动触发垃圾回收与内存整理
    void performGlobalMemoryGC();

// 音频相似度搜索 (F-028)
    SimilaritySearch similaritySearch;
    std::unique_ptr<HarkenSearch::SimilarityWorkflow> similarSoundsWorkflow;
    aura::ui::StandaloneWindowState similarSoundsWindow;
    SimilarityPreset similarSoundsPreset = SimilarityPreset::Balanced;
    int similarSoundsSelectedRow = -1;
    int64_t similarSoundsObservedSelectionId = -1;
    std::optional<HarkenSearch::SimilarityReference> similarSoundsLocalReference;
    bool similarSoundsObservedBatchExtracting = false;
    // Batch feature extraction state
    std::atomic<bool> batchFeatureExtracting{false};
    std::atomic<bool> batchFeatureCancelRequested{false};
    std::atomic<bool> batchFeatureCompletionNotice{false}; // Dismissed on the next window opening.
    std::atomic<int>  batchFeatureProgress{0};
    std::atomic<int>  batchFeatureTotal{0};
    std::thread batchFeatureThread;

    // -----------------------------------------------------------------
    // BPM / Music Analysis State (F-033, music mode only)
    // -----------------------------------------------------------------
    MusicAnalyzer musicAnalyzer;
    MusicQueue    musicQueue;
    GaplessEngine gaplessEngine;
    TapTempoAccumulator tapTempo;

    // Pending BPM result — shown in result bubble until user confirms or dismisses
    struct PendingBpmResult {
        float bpm         = 0.0f;
        float confidence  = 0.0f;
        int   source      = 0;   // cast to MusicFieldSource when writing to DB
        bool  isReady     = false;    // true = show bubble
        bool  isAnalyzing = false;    // true = spinner (aubio running)
        bool  failed      = false;    // true = detection ran but returned no result
        double bubbleTimer = 0.0;     // seconds since result shown; dismiss at 10s
    };
    PendingBpmResult pendingBpm;
    uint64_t bpmGeneration = 0;   // latest-wins: drop results with old generation

    // Beat Grid state (T2)
    float beatGridOffset      = 0.0f;   // seconds: phase of first beat
    float beatGridBpm         = 0.0f;   // BPM of the active beat grid
    bool  beatGridVisible     = false;  // true once a BPM is confirmed
    bool  beatGridSnapEnabled = true;   // snap region drag to grid

// Music Mode Album Grid (F-033)
    struct AlbumGridState {
        std::vector<Database::AlbumEntry> albums;
        bool   needsReload   = true;
        int    selectedIdx   = -1;
        // Track list for the selected album
        std::string expandedAlbumArtist;
        std::string expandedAlbum;
        std::vector<AudioRecord> expandedTracks;
        bool   showTrackList = false;
        // 0=name↑  1=name↓  2=year desc  3=trackCount desc
        int    sortMode      = 0;
    };
    AlbumGridState albumGrid;

// F-033: User-Defined Smart Collections
    // Field constants for CollectionRule::field
    enum class CRuleField  { Description=0, Artist=1, Genre=2, Instruments=3, Key=4 };
    // Condition constants for CollectionRule::condition
    enum class CRuleCond   { Contains=0, NotContains=1, Equals=2 };
    struct CollectionRule {
        int  field     = 0;   // CRuleField cast to int
        int  condition = 0;   // CRuleCond cast to int
        char value[128] = {};
    };
    struct UserSmartCollection {
        char name[64] = {};
        std::vector<CollectionRule> rules; // AND-conjunction
    };
    std::vector<UserSmartCollection> userCollections;
    int activeUserCollectionIdx = -1;   // -1 = none active

// F-035: Beta Feedback & Crash Report
    bool showFeedbackDialog = false;       // main feedback popup visible
    bool showCrashReportDialog = false;    // post-crash recovery popup visible
    bool feedbackSubmitting = false;       // async HTTP in progress
    bool feedbackSubmitOk = false;         // true after successful submit (cleared on dialog open)
    std::string feedbackSubmitError;       // non-empty on failure; "" on success
    CueviFeedback::AsyncSubmission feedbackSubmitTask;
    std::string feedbackSubmissionId;
    std::string feedbackWorkItemId;
    bool feedbackSubmitWasCrash = false;
    bool crashDetectedOnStartup = false;   // sentinel file found → trigger dialog
    int  feedbackType = 0;                 // 0=Bug, 1=功能建议
    bool feedbackAttachLog = false;        // attach diag log to feedback only after explicit opt-in
    bool crashReportAttachLog = false;     // attach diag log to crash report only after explicit opt-in
    bool feedbackAnonymous = false;        // true = omit name/HWID from card
    char feedbackTitleBuf[256] = "";       // required title field
    char feedbackDescBuf[1024] = "";       // optional description

    // QA harness: deferred rescan row — set by PollQaCommandFile, processed at
    // the top of RenderUI (outside any table loop) to avoid dangling references.
    int qaRescanRow = -1;
    bool qaRequestScanBpm = false;
    bool qaRequestWaveformRebuild = false;
    bool qaRequestWaveformRebuildFull = false;
    int qaOpenRowContextMenuRow = -1;
    int64_t qaOpenSidebarPlaylistContextId = -1;
#if AURA_BUILD_DIAGNOSTICS && ARIS_PREVIEW_ISOLATION && !AURA_INTERNAL_BETA
    // Read/open-only probe of the real send menus; never dispatches files.
    struct QaSendTargetMenuRow {
        int index;
        std::string label;
        unsigned int id;
    };
    int qaSendTargetMenuRequested = -1;
    int qaSendTargetMenuRendered = -1;
    unsigned int qaSendTargetMenuRenderedId = 0;
    int qaSendTargetMenuFrame = -1;
    std::vector<QaSendTargetMenuRow> qaSendTargetMenuRows;
#endif

    // QA harness: UI region coordinates (client-relative, updated each frame)
    struct QaUiRegions {
        float listX = 0, listY = 0, listW = 0, listH = 0;
        float waveX = 0, waveY = 0, waveW = 0, waveH = 0;
        float transportX = 0, transportY = 0, transportW = 0, transportH = 0;
        int   listRowHeight = 0;
        int   listHeaderHeight = 0;
        int   listVisibleRows = 0;
        int   listScrollTopRow = 0;
        int   playingRowIndex = -1;
    } qaRegions;
};

// Global app state pointer
extern AppState* g_appState;

// 主波形 detail 延迟请求（2b：停 150ms 再请求，切文件则取消）
void ScheduleDeferredWaveformDetail(AppState& state, const std::string& filePath, int targetPeaks);
/// 每帧调用：当「当前已加载文件」与挂起路径一致时发出 detail 请求（须在 pumpLoadFileIfPending 之后调用，WebUI 与经典模式均需执行）
void PumpDeferredWaveformDetail(AppState& state);

// ---------------------------------------------------------------------------
// Playlist quick-bar chip plan (spec §3.1)
// ---------------------------------------------------------------------------
namespace HarkenSearch {

enum class ChipKind { Orphan, Folder, IndependentChild };

struct PlaylistChipPlanItem {
    int64_t     id               = -1;
    std::string name;
    ChipKind    kind             = ChipKind::Orphan;
    int64_t     parentId         = -1;
    std::string parentName;           // populated only when kind == IndependentChild
    int         visibleChildCount = 0; // populated only when kind == Folder; (N) badge
};

// Pure function: given full playlist list + hidden id set, compute the chip
// rendering plan for the quick bar (see spec §3.1). Order matches input.
std::vector<PlaylistChipPlanItem> ComputePlaylistChipPlan(
    const std::vector<Playlist>& all,
    const std::vector<int64_t>& hiddenIds);

} // namespace HarkenSearch

// Legacy namespace spelling remains available for serialized/plugin compatibility.
namespace Aura {
using ::AppState;
using ::g_appState;
}
