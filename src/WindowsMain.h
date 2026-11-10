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
