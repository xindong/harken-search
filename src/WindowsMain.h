#pragma once

// @stage7-contract
// API contract: WindowsMain declarations expose application entry/lifecycle
// coordination only. Native handles remain owned by their existing platform
// implementations; callers preserve startup/shutdown ordering and must not use
// these declarations as a general UI or process-control API.
// SoundManager - Windows平台模块
// WindowsMain.h - Windows程序入口、窗口管理、托盘、热键、字体
//
// 从 main.cpp 提取的 Windows 平台相关代码

#include "imgui.h"
#include "UiFonts.h"
#include "AppMainServices.h"
#include <cstdint>
#include <string>

// Cross-platform declarations consumed by the shared application modules.
// The implementation is supplied by the platform entry/diagnostics units;
// these declarations must remain visible on macOS as well as Windows.
void MarkUiLeftClickNow();
bool IsRapidUiClickBurstActive();
void SetCoverWallPriorityMode(bool enabled);
bool IsCoverWallPriorityMode();
void AppendDiagLogLine(const wchar_t* line);
void AppendDiagLogF(const wchar_t* fmt, ...);
void SetDiagnosticFileEnabled(bool enabled);
std::uint64_t GetDiagnosticFileSizeBytes();
std::uint64_t GetDiagnosticFileMaxBytes();
bool ClearDiagnosticFile();
std::string BuildSysInfo();
extern int g_suppressImGuiDragTooltipFrames;
#ifndef _WIN32
std::string OpenFolderDialog(void* ownerWindow);
#endif

#ifdef _WIN32
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// Keep this public contract independent of the Win32 SDK.  WindowsMain.cpp
// includes windows.h after this header; consumers only need ABI-compatible
// handle and message types for the declarations below.
#ifndef _WINDOWS_
struct HWND__;
struct HDC__;
struct HGLRC__;
struct tagWINDOWPLACEMENT;
using HWND = HWND__*;
using HDC = HDC__*;
using HGLRC = HGLRC__*;
using WINDOWPLACEMENT = tagWINDOWPLACEMENT;
using UINT = unsigned int;
using WPARAM = std::uintptr_t;
using LPARAM = std::intptr_t;
using LRESULT = std::intptr_t;
#ifndef CALLBACK
#define CALLBACK __stdcall
#endif
#endif

// Forward declarations
struct AppState;
struct AppSettings;
struct KeyBinding;
class AudioEngine;
struct AppState;

// Constants

#ifndef WM_TRAYICON
#define WM_TRAYICON (0x8000L + 42)
#endif

#ifndef WM_APP_WEBUI_OPENFOLDER
#define WM_APP_WEBUI_OPENFOLDER (0x8000L + 50)
#endif

#ifndef WM_APP_WEBUI_OPENFOLDER_RESULT
#define WM_APP_WEBUI_OPENFOLDER_RESULT (0x8000L + 51)
#endif

#ifndef WM_APP_WEBUI_STARTDRAG
#define WM_APP_WEBUI_STARTDRAG (0x8000L + 52)
#endif

#ifndef AURO_IPC_OPEN_FILE
#define AURO_IPC_OPEN_FILE  1   // dwData value for WM_COPYDATA open-file IPC
#endif

#ifndef HOTKEY_ID_SHOW_MANAGER
#define HOTKEY_ID_SHOW_MANAGER 0x534D
#endif

// Extern Globals (defined in WindowsMain.cpp)

extern HWND g_hwnd;
extern bool g_isDevModeStartupBypass;  // Dev / file-player startup bypass for login and preload flow
extern bool g_hideWindowOnDrag;
extern UINT g_hideWindowOnDragDelayMs;
extern int g_suppressImGuiDragTooltipFrames;
extern WINDOWPLACEMENT g_savedPlacementForDragHide;
extern bool g_savedPlacementForDragHideValid;
extern bool g_savedWasMaximizedForDragHide;    // IsZoomed() at save time (more reliable than showCmd)
extern WINDOWPLACEMENT g_savedPlacementForTrayMinimize;
extern bool g_savedPlacementForTrayMinimizeValid;
extern bool g_savedWasMaximizedForTrayMinimize;
extern bool g_showManagerHotkeyRegisterOk;

void MarkUiLeftClickNow();
bool IsRapidUiClickBurstActive();
void SetCoverWallPriorityMode(bool enabled);
bool IsCoverWallPriorityMode();

// Window Management

void MinimizeWindowNoAnimation(HWND hwnd);
void ShowManagerWindowNow();
void EnsureWindowOnScreen(HWND hwnd);

// Tray Icon

void EnsureTrayIconAdded(HWND hwnd);
void RemoveTrayIconIfAny(HWND hwnd);
void RequestHarkenSearchExit(HWND hwnd, const wchar_t* reason);
void ShowTrayContextMenu(HWND hwnd);
void MaybeShowTrayMinimizeHint(AppState& state, HWND hwnd);

// Global Hotkey

UINT ImGuiKeyToVirtualKey(ImGuiKey key);
UINT HotkeyModsFromKeyBinding(const KeyBinding& kb);
bool IsGlobalHotkeyPressedOnce(const KeyBinding& kb);
void ApplyShowManagerGlobalHotkey(const AppSettings& settings);

// Font Loading

void LoadChineseFont(ImGuiIO& io, float chromePixelSize, float readingPixelSize, float listPixelSize);
ImFont* GetUIFontRegular();
ImFont* GetUIFontEmphasis();
ImFont* GetUIFontMono();
ImFont* GetUIListFont();
ImFont* GetUIMeterFont();  // F-018: 10px ASCII-only, for narrow loudness meter bars

// Reading-surface scope. While alive, GetUIFont*() return the reading-sized
// faces, and the reading-regular face is pushed so plain text scales too.
// Defaults are chrome; wrap only reading surfaces (tree, list, breadcrumb,
// Info panel, sidebar filters, settings, popups, menu dropdowns). A surface
// that is never wrapped renders at the pinned chrome size — there is no
// assert, so adding a new reading surface means remembering to wrap it.
struct ReadingTextScope {
    bool prev;
    ReadingTextScope();
    ~ReadingTextScope();
};

// Explicitly forces chrome faces inside an outer reading scope (e.g. an icon
// button embedded in a reading surface).
struct ChromeTextScope {
    bool prev;
    ChromeTextScope();
    ~ChromeTextScope();
};

// OpenGL Context

bool CreateOpenGLContext(HWND hwnd, HDC* hdc, HGLRC* hglrc);

// Window Procedure

LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

// Diagnostic Logging

void AppendDiagLogLine(const wchar_t* line);
void AppendDiagLogF(const wchar_t* fmt, ...);
void SetDiagnosticFileEnabled(bool enabled);
std::uint64_t GetDiagnosticFileSizeBytes();
std::uint64_t GetDiagnosticFileMaxBytes();
bool ClearDiagnosticFile();

// Functions defined in main.cpp, used by WindowsMain.cpp

// Settings
void LoadAppSettings(AppState& state);
void SaveAppSettings(const AppState& state);
bool TrySaveAppSettings(const AppState& state, std::optional<bool> monoModeOverride = std::nullopt);
// 仅加载音频相关键，供引擎初始化前调用（缓冲周期、输出设备索引）
void LoadAudioSettingsEarly(AppState& state);
void PushRecentDbPath(AppState& state, const std::string& pathUtf8);
void MarkSettingsDirty(AppState& state);
void FlushSettingsDirty(AppState& state);

// F-033 Music Mode ↔ SFX Mode toggle is deferred to frame end because
// productMode drives many Music-only rendering branches (Module 2.5
// Key+BPM, Module 3.5 BPM/TAP, multi-channel waveform). Flipping the
// flag mid-frame desyncs SetCursorPos/ItemAdd pairing and trips the
// imgui.cpp:10789 parent-boundary assert. Call once per frame after
// RenderUI returns; defined in main.cpp.
void ProcessPendingProductModeToggle(AppState& state);

// Drag & drop export (defined in main.cpp)
void StartRegionDrag(AudioEngine& engine);

// Random playback (defined in main.cpp)
void PlayRandomFile(AppState& state);

// Search/filter (defined in main.cpp; primary declaration in SearchQuery.h)
void RefreshSearchResults(AppState& state, bool resetPagination);
void ForceCoverWallReload(AppState& state);
void StartCoverRebuild(AppState& state);
// 轮询并应用一次异步搜索结果；返回 true 表示消费了一条结果（供加载页等待首包）
bool PumpOneAsyncSearchResult(AppState& state);
void PlayRandomFile(AppState& state);
void LoadAllFilesFiltered(AppState& state, int limit = 1000);
void FilterResultsByFileType(AppState& state);

// Splash pre-upload: batch upload waveform textures to GPU (bypasses per-frame budget)
void PreUploadWaveformTextures(AppState& state, int count);

// 切库后完整状态清理（defined in main.cpp）
void ResetDatabaseState(AppState& state);
// 后台刷新文件夹树/播放列表（defined in main.cpp）
void StartDeferredSidebarBootstrap(AppState& state, bool reloadFolderTree, bool reloadPlaylists);
// 轮询异步切库结果，在每帧主循环开头调用
void PumpDatabaseSwitchIfPending(AppState& state);
// Join the owned asynchronous database-switch worker before tearing down
// shared database/runtime resources.
void WaitForDatabaseSwitchThread();
void ShutdownSelectedWaveformRebuild();
// 轮询异步随机池构建结果
void PumpInitRandomSamplingPoolIfPending(AppState& state);
// 加载完成后应用 Mini 波形预设跳转点并跳过开头无声（在 pumpLoadFileIfPending 为 true 时由主循环调用）
void OnAudioLoadPumped(AppState& state);
// 每帧可调：当有波形数据时根据阈值 seek 到首个有音位置（加载完成时波形常未就绪，故需每帧尝试直到有数据）
void MaybeSkipLeadingSilence(AppState& state);
// 开始异步切库（大库不卡主线程）；返回 true 表示已开始
bool BeginSwitchDatabase(AppState& state, const std::string& newDbPathUtf8);
// Drag & Drop (defined in main.cpp)
void StartFileDrag(const std::vector<std::string>& files);

// Utility (defined in main.cpp)
std::string GetExeDirUtf8();
std::wstring Utf8ToWide(const std::string& str);
std::string WideToUtf8(const std::wstring& wstr);
bool PathExistsUtf8(const std::string& pathUtf8);
std::string GetDefaultDbPathUtf8();
std::string OpenFolderDialog(HWND hwnd);

// Shell integration — registry helpers (defined in DefaultAudioPlayerAssociation.cpp)
void RegisterOpenWith();
void SetAsDefaultPlayer();
void RemoveDefaultPlayer();
bool IsRegisteredAsDefault();

// F-035: Beta feedback / crash report helpers (defined in WindowsMain.cpp)
std::string BuildSysInfo();

void WriteBugReportJson(const std::string& type,
                        const std::string& userName,
                        const std::string& userText,
                        const std::string& email,
                        const std::string& sysInfo,
                        const std::string& recentLog);

#endif // _WIN32
