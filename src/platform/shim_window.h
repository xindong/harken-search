// Window and surface types for building the shared UI on macOS.
//
// Part of the macOS platform shim (owner: line A). Windows builds never see
// this header. Everything that needs a real window lives behind
// Platform::Window*/Platform::WindowSetForeground; what main.cpp still touches
// directly are the Win32 spellings of "a rectangle" and "a saved window
// placement", so that is all this header provides.
#pragma once

#ifndef _WIN32

#include <cstddef>
#include <cstdint>

// Win32 base types the shared UI still names directly.
using LONG = std::int32_t;
using DWORD = std::uint32_t;
using UINT = unsigned int;
using WPARAM = std::uintptr_t;
using LPARAM = std::intptr_t;
using LRESULT = std::intptr_t;
using HRESULT = std::int32_t;
using UINT_PTR = std::uintptr_t;

#ifndef MAX_PATH
#define MAX_PATH 260
#endif
#ifndef WINAPI
#define WINAPI
#endif
#ifndef CALLBACK
#define CALLBACK
#endif

// HRESULT helpers. Only the two the shared UI uses are reproduced.
#ifndef S_OK
#define S_OK (static_cast<HRESULT>(0))
#endif
#ifndef FAILED
#define FAILED(result) (static_cast<HRESULT>(result) < 0)
#endif
#ifndef SUCCEEDED
#define SUCCEEDED(result) (static_cast<HRESULT>(result) >= 0)
#endif

struct POINT {
    LONG x = 0;
    LONG y = 0;
};

struct RECT {
    LONG left = 0;
    LONG top = 0;
    LONG right = 0;
    LONG bottom = 0;
};

struct SIZE {
    LONG cx = 0;
    LONG cy = 0;
};

// WIN32_FIND_DATA / WIN32_FILE_ATTRIBUTE_DATA surface: the shared UI reads
// attributes and the two timestamps, and gets them through a platform helper.
struct FILETIME {
    DWORD dwLowDateTime = 0;
    DWORD dwHighDateTime = 0;
};

struct WIN32_FILE_ATTRIBUTE_DATA {
    DWORD dwFileAttributes = 0;
    FILETIME ftCreationTime{};
    FILETIME ftLastAccessTime{};
    FILETIME ftLastWriteTime{};
    DWORD nFileSizeHigh = 0;
    DWORD nFileSizeLow = 0;
};

enum : DWORD {
    GetFileExInfoStandard = 0,
};

// Saved window placement lives in WindowsMain.h (the shared front-end contract
// header) so both front ends see one definition; see the non-Windows branch
// there. Only the flags that main.cpp ORs into it are needed here, and they are
// guarded so a future WindowsMain.h addition cannot collide.

// Window functions the shared UI calls by their Win32 names.
//
// These are declared here and implemented against Cocoa in
// shim_window_macos.mm; nothing in this header is a silent no-op. Two of them
// have no macOS equivalent and say so in their implementation instead of
// pretending to succeed: mouse capture is a Win32 concept, and hit-testing a
// foreign window is only used to find a DAW drop target, which macOS does not
// implement yet.
struct WINDOWPLACEMENT;
struct HarkenPlatformWindow;

HarkenPlatformWindow* GetForegroundWindow();
int GetWindowPlacement(HarkenPlatformWindow* window, WINDOWPLACEMENT* placement);
int SetWindowPlacement(HarkenPlatformWindow* window, const WINDOWPLACEMENT* placement);
int IsZoomed(HarkenPlatformWindow* window);
HarkenPlatformWindow* WindowFromPoint(POINT point);
int ClientToScreen(HarkenPlatformWindow* window, POINT* point);
void ReleaseCapture();
void PostQuitMessage(int exitCode);

// Reads file attributes the way the shared code expects them.
int GetFileAttributesExW(const wchar_t* path, int infoLevel, WIN32_FILE_ATTRIBUTE_DATA* info);

namespace Platform::Shim {
// The macOS front end registers its main window once the Cocoa window exists,
// so the functions above have something to act on.
void RegisterMainWindow(void* nsWindow);
}  // namespace Platform::Shim

// Show-window commands used by the tray/drag-hide path.
enum : int {
    SW_HIDE = 0,
    SW_SHOWNORMAL = 1,
    SW_SHOWMINIMIZED = 2,
    SW_SHOWMAXIMIZED = 3,
    SW_SHOWMINNOACTIVE = 7,
    SW_RESTORE = 9,
};

// Virtual-key codes the shared keybinding code names directly.
enum : int {
    VK_TAB = 0x09,
    VK_RETURN = 0x0D,
    VK_ESCAPE = 0x1B,
    VK_SPACE = 0x20,
    VK_LEFT = 0x25,
    VK_UP = 0x26,
    VK_RIGHT = 0x27,
    VK_DOWN = 0x28,
    VK_DELETE = 0x2E,
    VK_SHIFT = 0x10,
    VK_CONTROL = 0x11,
    VK_MENU = 0x12,
    VK_F1 = 0x70,
    VK_F2 = 0x71,
};

#endif  // !_WIN32
