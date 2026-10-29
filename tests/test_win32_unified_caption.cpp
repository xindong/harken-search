#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "platform/win32_unified_caption.h"

#include <cassert>

namespace {
int closeCount = 0;
const harken::caption::Palette palette{
    RGB(15, 20, 30), RGB(220, 225, 235), RGB(135, 150, 170),
    RGB(40, 48, 60), RGB(54, 62, 76), RGB(70, 80, 95)
};

LRESULT CALLBACK TestProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_CLOSE) { ++closeCount; return 0; }
    if (const auto handled = harken::caption::HandleMessage(
            hwnd, msg, wp, lp, DefWindowProcW, palette)) return *handled;
    return DefWindowProcW(hwnd, msg, wp, lp);
}
}

int main() {
    WNDCLASSW cls{};
    cls.hInstance = GetModuleHandleW(nullptr);
    cls.lpfnWndProc = TestProc;
    cls.lpszClassName = L"HarkenCaptionTest";
    assert(RegisterClassW(&cls));
    HWND hwnd = CreateWindowExW(0, cls.lpszClassName, L"Caption test",
        WS_OVERLAPPEDWINDOW, 100, 100, 800, 600, nullptr, nullptr, cls.hInstance, nullptr);
    assert(hwnd);
    assert(harken::caption::Install(hwnd));
    assert((GetWindowLongW(hwnd, GWL_STYLE) & WS_CAPTION) != WS_CAPTION);

    RECT window{};
    assert(GetWindowRect(hwnd, &window));
    const auto layout = harken::caption::LayoutForWindow(hwnd);
    const int closeX = window.left + (layout.close.left + layout.close.right) / 2;
    const int closeY = window.top + (layout.close.top + layout.close.bottom) / 2;
    const LPARAM point = MAKELPARAM(closeX, closeY);
    assert(SendMessageW(hwnd, WM_NCHITTEST, 0, point) == HTCLOSE);
    SendMessageW(hwnd, WM_NCLBUTTONDOWN, HTCLOSE, point);
    SendMessageW(hwnd, WM_NCLBUTTONUP, HTCLOSE, point);
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    assert(closeCount == 1);
    DestroyWindow(hwnd);
    return 0;
}
