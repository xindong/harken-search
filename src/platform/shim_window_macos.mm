// Cocoa implementations for the Win32-named window functions the shared UI
// calls. Included only by the macOS build; Windows keeps the real SDK.
//
// Rules for this file:
//   - Implement the real behaviour where macOS has one (placement, zoom,
//     foreground, quit).
//   - Say so explicitly where it does not (mouse capture, foreign-window hit
//     test) instead of returning success and letting the caller believe a
//     Windows-only flow worked.

#include "platform/shim_window.h"

// WINDOWPLACEMENT is owned by the shared front-end contract header; the shim
// header only forward-declares it so main.cpp can pass pointers without pulling
// the whole contract in.
#include "WindowsMain.h"

#ifndef _WIN32

#import <AppKit/AppKit.h>

#include <sys/stat.h>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <string>

struct HarkenPlatformWindow {
    void* nsWindow;  // NSWindow*, kept void* so the header stays C++-only.
};

namespace {

NSWindow* WindowOrNil(HarkenPlatformWindow* window) {
    if (!window || !window->nsWindow) return nil;
    return (__bridge NSWindow*)window->nsWindow;
}

HarkenPlatformWindow* g_mainWindow = nullptr;

HarkenPlatformWindow* MainWindow() {
    return g_mainWindow;
}

// The shared code stores the placement in client coordinates of a Win32
// window; Cocoa frames are screen coordinates with the origin at the bottom
// left. Convert on the way in and out so a saved placement restores the same
// rectangle.
NSRect ToCocoaRect(const WINDOWPLACEMENT& placement) {
    NSRect rect;
    rect.origin.x = placement.rcNormalPosition.left;
    rect.origin.y = placement.rcNormalPosition.top;
    rect.size.width = placement.rcNormalPosition.right - placement.rcNormalPosition.left;
    rect.size.height = placement.rcNormalPosition.bottom - placement.rcNormalPosition.top;
    return rect;
}

}  // namespace

namespace Platform::Shim {

void RegisterMainWindow(void* nsWindow) {
    static HarkenPlatformWindow handle;
    handle.nsWindow = nsWindow;
    g_mainWindow = &handle;
}

}  // namespace Platform::Shim

HarkenPlatformWindow* GetForegroundWindow() {
    HarkenPlatformWindow* main = MainWindow();
    NSWindow* window = WindowOrNil(main);
    if (!window) return nullptr;
    return [window isKeyWindow] ? main : nullptr;
}

int GetWindowPlacement(HarkenPlatformWindow* window, WINDOWPLACEMENT* placement) {
    NSWindow* cocoa = WindowOrNil(window);
    if (!cocoa || !placement) return 0;
    const NSRect frame = [cocoa frame];
    // Cocoa's origin is bottom-left; the shared code treats top as zero, so
    // report the rectangle in screen coordinates converted to the top-left
    // convention used by the placement struct.
    const NSRect screen = [[cocoa screen] frame];
    placement->length = sizeof(WINDOWPLACEMENT);
    placement->showCmd = [cocoa isMiniaturized] ? 7 /* SW_SHOWMINNOACTIVE */
                                                : ([cocoa isZoomed] ? 3 /* SW_SHOWMAXIMIZED */ : 1 /* SW_SHOWNORMAL */);
    placement->rcNormalPosition.left = static_cast<LONG>(frame.origin.x);
    placement->rcNormalPosition.top = static_cast<LONG>(screen.size.height - (frame.origin.y + frame.size.height));
    placement->rcNormalPosition.right = static_cast<LONG>(frame.origin.x + frame.size.width);
    placement->rcNormalPosition.bottom = static_cast<LONG>(screen.size.height - frame.origin.y);
    return 1;
}

int SetWindowPlacement(HarkenPlatformWindow* window, const WINDOWPLACEMENT* placement) {
    NSWindow* cocoa = WindowOrNil(window);
    if (!cocoa || !placement) return 0;
    const NSRect frame = ToCocoaRect(*placement);
    const NSRect screen = [[cocoa screen] frame];
    if (frame.size.width > 0 && frame.size.height > 0) {
        NSRect target;
        target.size = frame.size;
        target.origin.x = frame.origin.x;
        target.origin.y = screen.size.height - (frame.origin.y + frame.size.height);
        [cocoa setFrame:target display:YES];
    }
    if (placement->showCmd == 7 /* SW_SHOWMINNOACTIVE */) {
        [cocoa miniaturize:nil];
    } else if (placement->showCmd == 3 /* SW_SHOWMAXIMIZED */) {
        if (![cocoa isZoomed]) [cocoa zoom:nil];
    } else if ([cocoa isMiniaturized]) {
        [cocoa deminiaturize:nil];
    } else if ([cocoa isZoomed]) {
        [cocoa zoom:nil];
    }
    return 1;
}

int IsZoomed(HarkenPlatformWindow* window) {
    NSWindow* cocoa = WindowOrNil(window);
    if (!cocoa) return 0;
    return [cocoa isZoomed] ? 1 : 0;
}

HarkenPlatformWindow* WindowFromPoint(POINT point) {
    // Used only to locate the DAW window under a drag. macOS has no equivalent
    // of the Windows drag-to-DAW flow yet, so report "no window" rather than a
    // guess: callers already handle a null target.
    (void)point;
    return nullptr;
}

int ClientToScreen(HarkenPlatformWindow* window, POINT* point) {
    NSWindow* cocoa = WindowOrNil(window);
    if (!cocoa || !point) return 0;
    const NSRect content = [cocoa contentRectForFrameRect:[cocoa frame]];
    const NSRect screen = [[cocoa screen] frame];
    point->x += static_cast<LONG>(content.origin.x);
    point->y = static_cast<LONG>(screen.size.height - (content.origin.y + content.size.height)) + point->y;
    return 1;
}

void ReleaseCapture() {
    // Win32 mouse capture has no macOS counterpart; the ImGui macOS backend
    // owns pointer state for the whole app, so there is nothing to release.
}

void PostQuitMessage(int exitCode) {
    (void)exitCode;
    [NSApp terminate:nil];
}

int GetFileAttributesExW(const wchar_t* path, int infoLevel, WIN32_FILE_ATTRIBUTE_DATA* info) {
    (void)infoLevel;
    if (!path || !info) return 0;
    char narrow[4096];
    const std::size_t length = std::wcstombs(narrow, path, sizeof(narrow) - 1);
    if (length == static_cast<std::size_t>(-1)) return 0;
    narrow[length] = '\0';
    struct stat status;
    if (::stat(narrow, &status) != 0) return 0;
    info->dwFileAttributes = S_ISDIR(status.st_mode) ? 0x10u : 0x80u;
    info->nFileSizeHigh = static_cast<DWORD>(static_cast<unsigned long long>(status.st_size) >> 32);
    info->nFileSizeLow = static_cast<DWORD>(status.st_size & 0xFFFFFFFFull);
    info->ftLastWriteTime.dwLowDateTime = static_cast<DWORD>(status.st_mtime & 0xFFFFFFFFll);
    info->ftLastWriteTime.dwHighDateTime = static_cast<DWORD>(static_cast<unsigned long long>(status.st_mtime) >> 32);
    return 1;
}

#endif  // !_WIN32
