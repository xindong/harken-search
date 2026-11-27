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

// Saved window placement. macOS keeps this in the same shape so the restore
// path in main.cpp stays one code path; the values are only meaningful when
// handed back to SetWindowPlacement/GetWindowPlacement.
enum : DWORD {
    WPF_SETMINPOSITION = 0x0001,
    WPF_RESTORETOMAXIMIZED = 0x0002,
    WPF_ASYNCWINDOWPLACEMENT = 0x0004,
};

struct WINDOWPLACEMENT {
    UINT length = sizeof(WINDOWPLACEMENT);
    UINT flags = 0;
    UINT showCmd = 0;
    POINT ptMinPosition{};
    POINT ptMaxPosition{};
    RECT rcNormalPosition{};
};

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
