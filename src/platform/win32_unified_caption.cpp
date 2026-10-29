#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "win32_unified_caption.h"

#include <objidl.h>
#include <dwmapi.h>
#include <gdiplus.h>
#include <windowsx.h>

#include <algorithm>
#include <string>
#include <unordered_map>

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "dwmapi.lib")

namespace harken::caption {
namespace {

struct WindowState {
    Hit hovered = Hit::None;
    Hit pressed = Hit::None;
    COLORREF borderColor = CLR_INVALID;
};

std::unordered_map<HWND, WindowState> g_windows;

struct GdiPlusSession {
    ULONG_PTR token = 0;
    GdiPlusSession() {
        Gdiplus::GdiplusStartupInput input;
        Gdiplus::GdiplusStartup(&token, &input, nullptr);
    }
    ~GdiPlusSession() { if (token) Gdiplus::GdiplusShutdown(token); }
};

int Dpi(HWND hwnd) {
    const UINT dpi = GetDpiForWindow(hwnd);
    return dpi ? static_cast<int>(dpi) : 96;
}

int Scale(int value, int dpi) { return MulDiv(value, dpi, 96); }

int FrameWidth(HWND hwnd) {
    const UINT dpi = static_cast<UINT>(Dpi(hwnd));
    return GetSystemMetricsForDpi(SM_CXSIZEFRAME, dpi) +
           GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
}

int CaptionHeight(HWND hwnd) {
    const UINT dpi = static_cast<UINT>(Dpi(hwnd));
    return std::max(Scale(30, static_cast<int>(dpi)),
                    GetSystemMetricsForDpi(SM_CYCAPTION, dpi));
}

bool Enabled(HWND hwnd) {
    if (!IsInstalled(hwnd)) return false;
    const LONG style = GetWindowLongW(hwnd, GWL_STYLE);
    return (style & WS_THICKFRAME) && (style & WS_SYSMENU) &&
           (style & WS_CAPTION) != WS_CAPTION;
}

LRESULT Forward(WNDPROC proc, HWND hwnd, UINT message, WPARAM wp, LPARAM lp) {
    return CallWindowProcW(proc ? proc : DefWindowProcW, hwnd, message, wp, lp);
}

bool IsButton(Hit hit) {
    return hit == Hit::Close || hit == Hit::Maximize || hit == Hit::Minimize;
}

Gdiplus::Color Color(COLORREF rgb) {
    return Gdiplus::Color(255, GetRValue(rgb), GetGValue(rgb), GetBValue(rgb));
}

void FillRoundRect(Gdiplus::Graphics& graphics, Gdiplus::SolidBrush& brush,
                   float x, float y, float width, float height, float radius) {
    Gdiplus::GraphicsPath path;
    const float d = radius * 2.0f;
    path.AddArc(x, y, d, d, 180.0f, 90.0f);
    path.AddArc(x + width - d, y, d, d, 270.0f, 90.0f);
    path.AddArc(x + width - d, y + height - d, d, d, 0.0f, 90.0f);
    path.AddArc(x, y + height - d, d, d, 90.0f, 90.0f);
    path.CloseFigure();
    graphics.FillPath(&brush, &path);
}

void DrawButton(Gdiplus::Graphics& graphics, const Rect& rect, Hit action,
                const WindowState& state, const Palette& palette, int dpi,
                bool maximized) {
    if (rect.right <= rect.left) return;
    const float cx = (rect.left + rect.right) * 0.5f;
    const float cy = (rect.top + rect.bottom) * 0.5f;
    if (state.hovered == action || state.pressed == action) {
        const float extent = static_cast<float>(Scale(26, dpi));
        Gdiplus::SolidBrush brush(Color(state.pressed == action ? palette.active : palette.hover));
        FillRoundRect(graphics, brush, cx - extent * 0.5f, cy - extent * 0.5f,
                      extent, extent, static_cast<float>(Scale(4, dpi)));
    }
    const float half = static_cast<float>(Scale(6, dpi));
    Gdiplus::Pen pen(Color(palette.icon), std::max(1.0f, 1.75f * dpi / 96.0f));
    pen.SetStartCap(Gdiplus::LineCapRound);
    pen.SetEndCap(Gdiplus::LineCapRound);
    if (action == Hit::Close) {
        graphics.DrawLine(&pen, cx - half, cy - half, cx + half, cy + half);
        graphics.DrawLine(&pen, cx + half, cy - half, cx - half, cy + half);
    } else if (action == Hit::Minimize) {
        graphics.DrawLine(&pen, cx - half, cy + half * 0.5f,
                          cx + half, cy + half * 0.5f);
    } else if (action == Hit::Maximize) {
        if (maximized) {
            graphics.DrawRectangle(&pen, cx - half + Scale(2, dpi), cy - half,
                                   half * 2.0f, half * 2.0f);
            graphics.DrawRectangle(&pen, cx - half - Scale(2, dpi), cy - half + Scale(2, dpi),
                                   half * 2.0f, half * 2.0f);
        } else {
            graphics.DrawRectangle(&pen, cx - half, cy - half,
                                   half * 2.0f, half * 2.0f);
        }
    }
}

void Paint(HWND hwnd, const Palette& palette) {
    if (!IsWindowVisible(hwnd)) return;
    static GdiPlusSession session;
    if (!session.token) return;
    WindowState& state = g_windows.at(hwnd);
    if (state.borderColor != palette.border &&
        SUCCEEDED(DwmSetWindowAttribute(hwnd, DWMWA_BORDER_COLOR,
                                        &palette.border, sizeof(palette.border)))) {
        state.borderColor = palette.border;
    }
    const Layout layout = LayoutForWindow(hwnd);
    HDC dc = GetWindowDC(hwnd);
    if (!dc) return;
    {
        Gdiplus::Graphics graphics(dc);
        graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        Gdiplus::SolidBrush background(Color(palette.background));
        // Paint the resize frame above and beside the caption as well. Windows
        // otherwise leaves a light system-colored strip around dark themes.
        graphics.FillRectangle(&background, 0, 0,
                               layout.caption.right + layout.caption.left,
                               layout.caption.bottom);
        const int dpi = Dpi(hwnd);
        DrawButton(graphics, layout.minimize, Hit::Minimize, state, palette, dpi, IsZoomed(hwnd));
        DrawButton(graphics, layout.maximize, Hit::Maximize, state, palette, dpi, IsZoomed(hwnd));
        DrawButton(graphics, layout.close, Hit::Close, state, palette, dpi, IsZoomed(hwnd));
        Gdiplus::Pen line(Color(palette.border), 1.0f);
        graphics.DrawLine(&line, static_cast<float>(layout.caption.left),
                          layout.caption.bottom - 0.5f,
                          static_cast<float>(layout.caption.right), layout.caption.bottom - 0.5f);
        graphics.Flush(Gdiplus::FlushIntentionSync);

        const int iconSize = Scale(16, dpi);
        const int iconX = layout.caption.left + Scale(9, dpi);
        const int iconY = layout.caption.top +
                          (layout.caption.bottom - layout.caption.top - iconSize) / 2;
        HICON icon = reinterpret_cast<HICON>(SendMessageW(hwnd, WM_GETICON, ICON_SMALL, 0));
        if (!icon) icon = reinterpret_cast<HICON>(GetClassLongPtrW(hwnd, GCLP_HICONSM));
        if (icon) DrawIconEx(dc, iconX, iconY, icon, iconSize, iconSize, 0, nullptr, DI_NORMAL);

        const int length = GetWindowTextLengthW(hwnd);
        std::wstring title(static_cast<size_t>(length) + 1, wchar_t{});
        GetWindowTextW(hwnd, title.data(), length + 1);
        title.resize(static_cast<size_t>(length));
        HFONT font = CreateFontW(-Scale(12, dpi), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                 DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                 CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        HGDIOBJ oldFont = SelectObject(dc, font);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, palette.title);
        const int titleRight = layout.minimize.right > 0 ? layout.minimize.left :
                               layout.maximize.right > 0 ? layout.maximize.left :
                               layout.close.left;
        RECT titleRect{iconX + iconSize + Scale(8, dpi), layout.caption.top,
                       titleRight - Scale(8, dpi), layout.caption.bottom};
        DrawTextW(dc, title.c_str(), -1, &titleRect,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        SelectObject(dc, oldFont);
        DeleteObject(font);
    }
    ReleaseDC(hwnd, dc);
}

void Repaint(HWND hwnd) {
    RedrawWindow(hwnd, nullptr, nullptr, RDW_FRAME | RDW_INVALIDATE);
}

Hit PointHit(HWND hwnd, LPARAM lp, bool clientPoint) {
    POINT point{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
    if (clientPoint) ClientToScreen(hwnd, &point);
    RECT window{};
    GetWindowRect(hwnd, &window);
    return HitTest(LayoutForWindow(hwnd), point.x - window.left, point.y - window.top);
}

void InvokeButton(HWND hwnd, Hit hit) {
    if (hit == Hit::Close) {
        PostMessageW(hwnd, WM_CLOSE, 0, 0);
    } else if (hit == Hit::Minimize) {
        PostMessageW(hwnd, WM_SYSCOMMAND, SC_MINIMIZE, 0);
    } else if (hit == Hit::Maximize) {
        PostMessageW(hwnd, WM_SYSCOMMAND, IsZoomed(hwnd) ? SC_RESTORE : SC_MAXIMIZE, 0);
    }
}

} // namespace

bool IsInstalled(HWND hwnd) { return g_windows.find(hwnd) != g_windows.end(); }

bool Install(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd)) return false;
    if (IsInstalled(hwnd)) return true;
    const LONG style = GetWindowLongW(hwnd, GWL_STYLE);
    if ((style & WS_CAPTION) != WS_CAPTION) return false;
    g_windows.emplace(hwnd, WindowState{});
    SetLastError(0);
    if (!SetWindowLongW(hwnd, GWL_STYLE, style & ~WS_CAPTION) && GetLastError() != 0) {
        g_windows.erase(hwnd);
        return false;
    }
    // DWM's default thick-frame painting leaves a light strip above our dark
    // caption. Keep the frame for resize hit testing, but paint it ourselves.
    const DWMNCRENDERINGPOLICY ncPolicy = DWMNCRP_DISABLED;
    DwmSetWindowAttribute(hwnd, DWMWA_NCRENDERING_POLICY,
                          &ncPolicy, sizeof(ncPolicy));
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    return true;
}

Layout LayoutForWindow(HWND hwnd) {
    RECT window{};
    GetWindowRect(hwnd, &window);
    const LONG style = GetWindowLongW(hwnd, GWL_STYLE);
    const int dpi = Dpi(hwnd);
    return CalculateLayout(window.right - window.left, window.bottom - window.top,
                           FrameWidth(hwnd), CaptionHeight(hwnd), Scale(46, dpi),
                           (style & WS_MINIMIZEBOX) != 0,
                           (style & WS_MAXIMIZEBOX) != 0);
}

std::optional<LRESULT> HandleMessage(HWND hwnd, UINT message, WPARAM wParam,
                                     LPARAM lParam, WNDPROC forward,
                                     const Palette& palette) {
    if (!IsInstalled(hwnd)) return std::nullopt;
    if (message == WM_NCDESTROY) {
        g_windows.erase(hwnd);
        return std::nullopt;
    }
    if (!Enabled(hwnd)) return std::nullopt;
    WindowState& state = g_windows.at(hwnd);
    switch (message) {
        case WM_NCCALCSIZE: {
            const LRESULT result = Forward(forward, hwnd, message, wParam, lParam);
            if (wParam) {
                auto* params = reinterpret_cast<NCCALCSIZE_PARAMS*>(lParam);
                if (params) params->rgrc[0].top += CaptionHeight(hwnd);
            } else {
                auto* rect = reinterpret_cast<RECT*>(lParam);
                if (rect) rect->top += CaptionHeight(hwnd);
            }
            return result;
        }
        case WM_NCPAINT:
        case WM_NCACTIVATE:
        case WM_SETTEXT: {
            const LRESULT result = Forward(forward, hwnd, message, wParam, lParam);
            Paint(hwnd, palette);
            return result;
        }
        case WM_NCHITTEST: {
            const LRESULT base = Forward(forward, hwnd, message, wParam, lParam);
            if (base == HTLEFT || base == HTRIGHT || base == HTTOP || base == HTBOTTOM ||
                base == HTTOPLEFT || base == HTTOPRIGHT ||
                base == HTBOTTOMLEFT || base == HTBOTTOMRIGHT) return base;
            switch (PointHit(hwnd, lParam, false)) {
                case Hit::Close: return HTCLOSE;
                case Hit::Maximize: return HTMAXBUTTON;
                case Hit::Minimize: return HTMINBUTTON;
                case Hit::Caption: return HTCAPTION;
                case Hit::None: return base;
            }
            return base;
        }
        case WM_NCMOUSEMOVE: {
            const Hit hit = PointHit(hwnd, lParam, false);
            if (state.hovered != hit) { state.hovered = hit; Repaint(hwnd); }
            TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE | TME_NONCLIENT, hwnd, 0};
            TrackMouseEvent(&track);
            return std::nullopt;
        }
        case WM_NCMOUSELEAVE:
            if (state.hovered != Hit::None) { state.hovered = Hit::None; Repaint(hwnd); }
            return 0;
        case WM_NCLBUTTONDOWN: {
            const Hit hit = PointHit(hwnd, lParam, false);
            if (!IsButton(hit)) return std::nullopt;
            state.pressed = hit;
            state.hovered = hit;
            SetCapture(hwnd);
            Repaint(hwnd);
            return 0;
        }
        case WM_MOUSEMOVE:
            if (state.pressed != Hit::None) {
                const Hit hit = PointHit(hwnd, lParam, true);
                if (state.hovered != hit) { state.hovered = hit; Repaint(hwnd); }
                return 0;
            }
            return std::nullopt;
        case WM_LBUTTONUP:
        case WM_NCLBUTTONUP:
            if (state.pressed != Hit::None) {
                const Hit pressed = state.pressed;
                const bool activate = pressed == PointHit(hwnd, lParam,
                                                           message == WM_LBUTTONUP);
                state.pressed = Hit::None;
                if (GetCapture() == hwnd) ReleaseCapture();
                Repaint(hwnd);
                if (activate) InvokeButton(hwnd, pressed);
                return 0;
            }
            return std::nullopt;
        case WM_CAPTURECHANGED:
        case WM_CANCELMODE:
            if (state.pressed != Hit::None) {
                state.pressed = Hit::None;
                Repaint(hwnd);
            }
            return std::nullopt;
        case WM_NCLBUTTONDBLCLK:
            if (wParam == HTCAPTION && (GetWindowLongW(hwnd, GWL_STYLE) & WS_MAXIMIZEBOX)) {
                InvokeButton(hwnd, Hit::Maximize);
                return 0;
            }
            return std::nullopt;
        default:
            return std::nullopt;
    }
}

void RefreshAll() {
    for (const auto& [hwnd, state] : g_windows) {
        if (Enabled(hwnd)) Repaint(hwnd);
    }
}

} // namespace harken::caption
