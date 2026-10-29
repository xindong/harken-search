#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <optional>

#include "win32_unified_caption_layout.h"

namespace harken::caption {

struct Palette {
    COLORREF background;
    COLORREF title;
    COLORREF icon;
    COLORREF hover;
    COLORREF active;
    COLORREF border;
};

// Attach only to a window which already had native caption buttons.
bool Install(HWND hwnd);
bool IsInstalled(HWND hwnd);
Layout LayoutForWindow(HWND hwnd);
std::optional<LRESULT> HandleMessage(HWND hwnd, UINT message, WPARAM wParam,
                                     LPARAM lParam, WNDPROC forward,
                                     const Palette& palette);
void RefreshAll();

} // namespace harken::caption
