#include "AudioDragCard.h"

#ifdef _WIN32

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <objidl.h>  // GDI+ 需要 IStream（工程开启了 WIN32_LEAN_AND_MEAN）
#include <gdiplus.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace HarkenSearch::AudioDragCard {
namespace {

using namespace Gdiplus;

// 卡片尺寸与动效参数来自本卡已确认的预览（168×59 小卡，210ms 缩卡，
// 松手后 125ms 内下沉 8px，160–245ms 淡出）。
constexpr wchar_t kWindowClass[] = L"HarkenSearchAudioDragCard";
constexpr UINT_PTR kTickTimerId = 1;
constexpr UINT_PTR kHideMainTimerId = 2;
constexpr UINT kTickIntervalMs = 8;

constexpr float kCardWidth = 168.0f;
constexpr float kCardHeight = 59.0f;
constexpr float kPaddingX = 8.0f;
constexpr float kPaddingTop = 7.0f;
constexpr float kPaddingBottom = 6.0f;
constexpr float kWaveHeight = 31.0f;
constexpr float kCaptionGap = 3.0f;
constexpr float kCardRadius = 6.0f;
constexpr float kStackFirstX = 4.0f;
constexpr float kStackFirstY = 3.0f;
constexpr float kStackSecondX = 8.0f;
constexpr float kStackSecondY = 6.0f;
constexpr float kCursorGap = 14.0f;
constexpr int kWaveBars = 56;

constexpr int kMorphMs = 210;
constexpr int kSettleMs = 125;
constexpr int kFadeStartMs = 160;
constexpr int kLandingEndMs = 245;
constexpr float kLandingDip = 8.0f;

constexpr ARGB kSurfaceFill = 0xFF192737;
constexpr ARGB kSurfaceBorder = 0xFF6F98C1;
constexpr ARGB kStackFirstFill = 0xFF1B2938;
constexpr ARGB kStackFirstBorder = 0xFF627C99;
constexpr ARGB kStackSecondFill = 0xFF172332;
constexpr ARGB kStackSecondBorder = 0xFF4F6780;
constexpr ARGB kWaveBar = 0xFFB7D4F0;
constexpr ARGB kWaveCenter = 0xFF59718A;
constexpr ARGB kNameText = 0xFFE2EDF8;
constexpr ARGB kCountText = 0xFFB7D4F0;

enum class Phase { Morph, Follow, Landing };

struct CardState {
    HWND window = nullptr;
    bool active = false;

    CardSpec spec;
    Phase phase = Phase::Follow;
    ULONGLONG phaseStart = 0;

    // 源波形与目标位置（屏幕像素）
    float morphWide = 1.0f;
    float morphTall = 1.0f;
    float sourceOriginX = 0.0f;
    float sourceOriginY = 0.0f;

    // 落地动效的起点
    float landingX = 0.0f;
    float landingY = 0.0f;

    // 鼠标释放检测：目标处理拖放较慢时也先落地，不让卡片一直挂着
    ULONGLONG buttonReleasedAt = 0;
    bool sawButtonDown = false;

    // 最近一次呈现的位置（落地动效从这里开始）
    float presentedX = 0.0f;
    float presentedY = 0.0f;

    // 离屏画布
    HDC memoryDC = nullptr;
    HBITMAP bitmap = nullptr;
    HGDIOBJ previousBitmap = nullptr;
    void* bits = nullptr;
    int bitmapWidth = 0;
    int bitmapHeight = 0;

    bool gdiplusReady = false;
};

CardState g_card;

float Clamp01(float value) { return std::clamp(value, 0.0f, 1.0f); }

// 与已确认预览一致的收尾速度曲线。
float EaseOutQuintic(float progress) {
    const float remaining = 1.0f - progress;
    return 1.0f - remaining * remaining * remaining * remaining * (1.0f + 4.0f * progress);
}

float SmoothStep(float value) {
    const float t = Clamp01(value);
    return t * t * (3.0f - 2.0f * t);
}

float Lerp(float from, float to, float t) { return from + (to - from) * t; }

bool EnsureGdiplus() {
    if (g_card.gdiplusReady) return true;
    GdiplusStartupInput input;
    ULONG_PTR token = 0;
    if (GdiplusStartup(&token, &input, nullptr) != Ok) return false;
    g_card.gdiplusReady = true;
    return true;
}

void BuildRoundedRect(GraphicsPath& path, const RectF& rect, float radius) {
    path.Reset();
    const float diameter = radius * 2.0f;
    if (radius <= 0.0f) {
        path.AddRectangle(rect);
        return;
    }
    path.AddArc(rect.X, rect.Y, diameter, diameter, 180.0f, 90.0f);
    path.AddArc(rect.GetRight() - diameter, rect.Y, diameter, diameter, 270.0f, 90.0f);
    path.AddArc(rect.GetRight() - diameter, rect.GetBottom() - diameter, diameter, diameter, 0.0f, 90.0f);
    path.AddArc(rect.X, rect.GetBottom() - diameter, diameter, diameter, 90.0f, 90.0f);
    path.CloseFigure();
}

void FillCard(Graphics& graphics, const RectF& rect, ARGB fill, ARGB border, BYTE opacity) {
    if (opacity == 0) return;
    const ARGB fillColor = (fill & 0x00FFFFFF) | (static_cast<ARGB>(opacity) << 24);
    const ARGB borderColor = (border & 0x00FFFFFF) | (static_cast<ARGB>(opacity) << 24);
    const RectF strokeRect(rect.X + 0.5f, rect.Y + 0.5f, rect.Width - 1.0f, rect.Height - 1.0f);
    GraphicsPath path;
    BuildRoundedRect(path, strokeRect, kCardRadius);
    SolidBrush fillBrush{Color(fillColor)};
    graphics.FillPath(&fillBrush, &path);
    Pen borderPen(Color(borderColor), 1.0f);
    graphics.DrawPath(&borderPen, &path);
}

void ReleaseSurface() {
    if (g_card.memoryDC) {
        if (g_card.previousBitmap) SelectObject(g_card.memoryDC, g_card.previousBitmap);
        DeleteDC(g_card.memoryDC);
        g_card.memoryDC = nullptr;
    }
    if (g_card.bitmap) {
        DeleteObject(g_card.bitmap);
        g_card.bitmap = nullptr;
    }
    g_card.previousBitmap = nullptr;
    g_card.bits = nullptr;
    g_card.bitmapWidth = 0;
    g_card.bitmapHeight = 0;
}

bool EnsureSurface(int width, int height) {
    if (width <= 0 || height <= 0) return false;
    if (g_card.bitmap && g_card.bitmapWidth == width && g_card.bitmapHeight == height) return true;

    ReleaseSurface();

    BITMAPINFO info = {};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;  // 自上而下
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;

    HDC screenDC = GetDC(nullptr);
    void* bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(screenDC, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    HDC memoryDC = CreateCompatibleDC(screenDC);
    ReleaseDC(nullptr, screenDC);

    if (!bitmap || !memoryDC || !bits) {
        if (bitmap) DeleteObject(bitmap);
        if (memoryDC) DeleteDC(memoryDC);
        return false;
    }

    g_card.previousBitmap = SelectObject(memoryDC, bitmap);
    g_card.bitmap = bitmap;
    g_card.memoryDC = memoryDC;
    g_card.bits = bits;
    g_card.bitmapWidth = width;
    g_card.bitmapHeight = height;
    return true;
}

std::wstring BuildCountText(int itemCount) {
    if (itemCount <= 1) return std::wstring();
    return std::wstring(L"\u00D7") + std::to_wstring(itemCount);
}

void DrawCard(Graphics& graphics, const CardSpec& spec, float scaleX, float scaleY, BYTE captionOpacity, BYTE stackOpacity) {
    graphics.ScaleTransform(scaleX, scaleY);

    const bool multi = spec.itemCount > 1;
    if (multi && stackOpacity > 0) {
        FillCard(graphics, RectF(kStackSecondX, kStackSecondY, kCardWidth, kCardHeight),
                 kStackSecondFill, kStackSecondBorder, stackOpacity);
        FillCard(graphics, RectF(kStackFirstX, kStackFirstY, kCardWidth, kCardHeight),
                 kStackFirstFill, kStackFirstBorder, stackOpacity);
    }

    FillCard(graphics, RectF(0.0f, 0.0f, kCardWidth, kCardHeight), kSurfaceFill, kSurfaceBorder, 255);

    const float waveLeft = kPaddingX;
    const float waveTop = kPaddingTop;
    const float waveWidth = kCardWidth - kPaddingX * 2.0f;
    const float centerY = waveTop + kWaveHeight * 0.5f;

    GraphicsState waveState = graphics.Save();
    graphics.SetClip(RectF(waveLeft - 1.0f, waveTop - 1.0f, waveWidth + 2.0f, kWaveHeight + 2.0f));

    Pen centerPen(Color(kWaveCenter), 1.0f);
    graphics.DrawLine(&centerPen, waveLeft, centerY, waveLeft + waveWidth, centerY);

    if (!spec.envelope.empty()) {
        Pen barPen(Color(kWaveBar), 1.5f);
        barPen.SetLineCap(LineCapRound, LineCapRound, DashCapFlat);
        const float halfWave = kWaveHeight * 0.5f - 0.5f;
        const int barCount = static_cast<int>(spec.envelope.size());
        const float step = waveWidth / static_cast<float>(barCount);
        for (int i = 0; i < barCount; ++i) {
            const float amplitude = Clamp01(spec.envelope[static_cast<size_t>(i)]);
            const float half = (std::max)(amplitude * halfWave, 0.35f);
            const float x = waveLeft + (static_cast<float>(i) + 0.5f) * step;
            graphics.DrawLine(&barPen, x, centerY - half, x, centerY + half);
        }
    }
    graphics.Restore(waveState);

    if (captionOpacity > 0) {
        const wchar_t* familyName = L"Microsoft YaHei UI";
        const auto familyAvailable = [](const wchar_t* name) {
            FontFamily probe(name);
            return probe.IsAvailable() != FALSE;
        };
        if (!familyAvailable(familyName)) familyName = L"Microsoft YaHei";
        if (!familyAvailable(familyName)) familyName = L"Segoe UI";
        FontFamily family(familyName);
        Font font(&family, 10.0f, FontStyleRegular, UnitPixel);
        StringFormat format;
        format.SetAlignment(StringAlignmentNear);
        format.SetLineAlignment(StringAlignmentCenter);
        format.SetFormatFlags(StringFormatFlagsNoWrap);
        format.SetTrimming(StringTrimmingEllipsisCharacter);

        const std::wstring countText = BuildCountText(spec.itemCount);
        float countWidth = 0.0f;
        if (!countText.empty()) {
            RectF countBounds;
            graphics.MeasureString(countText.c_str(), -1, &font, PointF(0.0f, 0.0f), &countBounds);
            countWidth = std::ceil(countBounds.Width) + 2.0f;
        }
        const float captionTop = kPaddingTop + kWaveHeight + kCaptionGap;
        const float captionHeight = kCardHeight - captionTop - kPaddingBottom;
        const RectF nameRect(kPaddingX, captionTop,
                             kCardWidth - kPaddingX * 2.0f - countWidth, captionHeight);

        const ARGB nameColor = (kNameText & 0x00FFFFFF) | (static_cast<ARGB>(captionOpacity) << 24);
        SolidBrush nameBrush{Color(nameColor)};
        graphics.DrawString(spec.fileName.c_str(), -1, &font, nameRect, &format, &nameBrush);

        if (!countText.empty()) {
            format.SetAlignment(StringAlignmentFar);
            const RectF countRect(kCardWidth - kPaddingX - countWidth, captionTop,
                                  countWidth, captionHeight);
            const ARGB countColor = (kCountText & 0x00FFFFFF) | (static_cast<ARGB>(captionOpacity) << 24);
            SolidBrush countBrush{Color(countColor)};
            graphics.DrawString(countText.c_str(), -1, &font, countRect, &format, &countBrush);
        }
    }
}

void ApplyOpacityToPixels(unsigned char* pixels, size_t pixelCount, BYTE opacity);

void ApplyGlobalOpacity(int width, int height, BYTE opacity) {
    if (opacity == 255) return;
    ApplyOpacityToPixels(static_cast<unsigned char*>(g_card.bits),
                         static_cast<size_t>(width) * static_cast<size_t>(height), opacity);
}

void ApplyOpacityToPixels(unsigned char* pixels, size_t pixelCount, BYTE opacity) {
    if (opacity == 255 || pixels == nullptr) return;
    const unsigned int factor = opacity;
    for (size_t i = 0; i < pixelCount; ++i) {
        unsigned char* px = pixels + i * 4;
        px[0] = static_cast<unsigned char>((px[0] * factor) / 255);
        px[1] = static_cast<unsigned char>((px[1] * factor) / 255);
        px[2] = static_cast<unsigned char>((px[2] * factor) / 255);
        px[3] = static_cast<unsigned char>((px[3] * factor) / 255);
    }
}

bool RenderCardPixels(const CardSpec& spec, float scaleX, float scaleY, BYTE captionOpacity, BYTE stackOpacity,
                      BYTE globalOpacity, std::vector<unsigned char>& outPixels, int& outWidth, int& outHeight) {
    if (!EnsureGdiplus()) return false;
    const float stackWidth = spec.itemCount > 1 ? kStackSecondX * scaleX : 0.0f;
    const float stackHeight = spec.itemCount > 1 ? kStackSecondY * scaleY : 0.0f;
    const int width = (std::max)(1, static_cast<int>(std::lround(kCardWidth * scaleX + stackWidth)));
    const int height = (std::max)(1, static_cast<int>(std::lround(kCardHeight * scaleY + stackHeight)));

    outWidth = width;
    outHeight = height;
    outPixels.assign(static_cast<size_t>(width) * static_cast<size_t>(height) * 4, 0);

    {
        Bitmap canvas(width, height, width * 4, PixelFormat32bppPARGB, outPixels.data());
        Graphics graphics(&canvas);
        graphics.SetSmoothingMode(SmoothingModeAntiAlias);
        graphics.SetPixelOffsetMode(PixelOffsetModeHighQuality);
        graphics.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
        graphics.SetCompositingMode(CompositingModeSourceOver);
        DrawCard(graphics, spec, scaleX, scaleY, captionOpacity, stackOpacity);
    }

    ApplyOpacityToPixels(outPixels.data(), static_cast<size_t>(width) * static_cast<size_t>(height), globalOpacity);
    return true;
}

void ClampToWorkArea(int pointerX, int pointerY, int width, int height, float* outX, float* outY) {
    POINT probe = {pointerX, pointerY};
    HMONITOR monitor = MonitorFromPoint(probe, MONITOR_DEFAULTTONEAREST);
    RECT work = {0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
    MONITORINFO info = {};
    info.cbSize = sizeof(info);
    if (monitor && GetMonitorInfoW(monitor, &info)) work = info.rcWork;

    float x = static_cast<float>(pointerX) + kCursorGap;
    float y = static_cast<float>(pointerY) + kCursorGap;
    const float maxX = static_cast<float>(work.right - width) - 4.0f;
    const float maxY = static_cast<float>(work.bottom - height) - 4.0f;
    x = std::clamp(x, static_cast<float>(work.left) + 4.0f, (std::max)(static_cast<float>(work.left) + 4.0f, maxX));
    y = std::clamp(y, static_cast<float>(work.top) + 4.0f, (std::max)(static_cast<float>(work.top) + 4.0f, maxY));
    *outX = x;
    *outY = y;
}

void PresentAt(float x, float y, float scaleX, float scaleY, BYTE captionOpacity, BYTE stackOpacity, BYTE globalOpacity) {
    if (!g_card.window || !EnsureGdiplus()) return;

    const float stackWidth = g_card.spec.itemCount > 1 ? kStackSecondX * scaleX : 0.0f;
    const float stackHeight = g_card.spec.itemCount > 1 ? kStackSecondY * scaleY : 0.0f;
    const int width = (std::max)(1, static_cast<int>(std::lround(kCardWidth * scaleX + stackWidth)));
    const int height = (std::max)(1, static_cast<int>(std::lround(kCardHeight * scaleY + stackHeight)));
    if (!EnsureSurface(width, height)) return;

    std::memset(g_card.bits, 0, static_cast<size_t>(width) * static_cast<size_t>(height) * 4);

    {
        Bitmap canvas(width, height, width * 4, PixelFormat32bppPARGB, static_cast<BYTE*>(g_card.bits));
        Graphics graphics(&canvas);
        graphics.SetSmoothingMode(SmoothingModeAntiAlias);
        graphics.SetPixelOffsetMode(PixelOffsetModeHighQuality);
        graphics.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
        graphics.SetCompositingMode(CompositingModeSourceOver);
        DrawCard(graphics, g_card.spec, scaleX, scaleY, captionOpacity, stackOpacity);
    }

    ApplyGlobalOpacity(width, height, globalOpacity);

    g_card.presentedX = x;
    g_card.presentedY = y;

    POINT destination = {static_cast<LONG>(std::lround(x)), static_cast<LONG>(std::lround(y))};
    SIZE size = {width, height};
    POINT source = {0, 0};
    BLENDFUNCTION blend = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    HDC screenDC = GetDC(nullptr);
    UpdateLayeredWindow(g_card.window, screenDC, &destination, &size, g_card.memoryDC, &source, 0, &blend, ULW_ALPHA);
    ReleaseDC(nullptr, screenDC);
}

void DestroyCard() {
    if (g_card.window) {
        KillTimer(g_card.window, kTickTimerId);
        KillTimer(g_card.window, kHideMainTimerId);
        DestroyWindow(g_card.window);
        g_card.window = nullptr;
    }
    ReleaseSurface();
    g_card.active = false;
    g_card.spec = CardSpec();
    g_card.phase = Phase::Follow;
    g_card.phaseStart = 0;
}

void BeginLandingFromPresentedPosition() {
    if (g_card.phase == Phase::Landing) return;
    g_card.spec.onHideMainDelayElapsed = nullptr;
    if (g_card.window) KillTimer(g_card.window, kHideMainTimerId);
    g_card.landingX = g_card.presentedX;
    g_card.landingY = g_card.presentedY;
    g_card.phase = Phase::Landing;
    g_card.phaseStart = GetTickCount64();
}

void Tick() {
    if (!g_card.active) return;
    const ULONGLONG now = GetTickCount64();
    POINT cursor = {};
    GetCursorPos(&cursor);

    if (g_card.spec.animationsEnabled && g_card.phase != Phase::Landing) {
        const bool leftDown = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
        if (leftDown) {
            g_card.sawButtonDown = true;
            g_card.buttonReleasedAt = 0;
        } else if (!g_card.sawButtonDown) {
            g_card.buttonReleasedAt = 0;
        } else if (g_card.buttonReleasedAt == 0) {
            g_card.buttonReleasedAt = now;
        } else if (now - g_card.buttonReleasedAt >= 60) {
            // 目标已经开始处理放下（DoDragDrop 还没返回）时，卡片照旧先落地。
            BeginLandingFromPresentedPosition();
        }
    }

    if (g_card.phase == Phase::Morph) {
        const float raw = static_cast<float>(now - g_card.phaseStart) / static_cast<float>(kMorphMs);
        if (raw >= 1.0f) {
            g_card.phase = Phase::Follow;
        } else {
            const float eased = EaseOutQuintic(Clamp01(raw));
            const float scaleX = Lerp(g_card.morphWide, 1.0f, eased);
            const float scaleY = Lerp(g_card.morphTall, 1.0f, eased);
            const int stackWidth = g_card.spec.itemCount > 1 ? static_cast<int>(kStackSecondX) : 0;
            const int stackHeight = g_card.spec.itemCount > 1 ? static_cast<int>(kStackSecondY) : 0;
            float targetX = 0.0f;
            float targetY = 0.0f;
            ClampToWorkArea(cursor.x, cursor.y,
                            static_cast<int>(kCardWidth) + stackWidth,
                            static_cast<int>(kCardHeight) + stackHeight,
                            &targetX, &targetY);
            const float x = Lerp(g_card.sourceOriginX, targetX, eased);
            const float y = Lerp(g_card.sourceOriginY, targetY, eased);
            const float captionFade = g_card.spec.itemCount > 1 ? 1.0f : Clamp01((eased - 0.55f) / 0.45f);
            const BYTE captionOpacity = static_cast<BYTE>(std::lround(captionFade * 255.0f));
            const BYTE stackOpacity = g_card.spec.itemCount > 1 ? static_cast<BYTE>(std::lround(eased * 255.0f)) : 0;
            PresentAt(x, y, scaleX, scaleY, captionOpacity, stackOpacity, 255);
            return;
        }
    }

    if (g_card.phase == Phase::Landing) {
        const float elapsedMs = static_cast<float>(now - g_card.phaseStart);
        if (elapsedMs >= static_cast<float>(kLandingEndMs)) {
            DestroyCard();
            return;
        }
        const float descent = SmoothStep(elapsedMs / static_cast<float>(kSettleMs));
        const float fade = SmoothStep((elapsedMs - static_cast<float>(kFadeStartMs)) /
                                      static_cast<float>(kLandingEndMs - kFadeStartMs));
        const BYTE globalOpacity = static_cast<BYTE>(std::lround((1.0f - fade) * 255.0f));
        PresentAt(g_card.landingX, g_card.landingY + kLandingDip * descent, 1.0f, 1.0f, 255, 255, globalOpacity);
        return;
    }

    // 跟手
    const int stackWidth = g_card.spec.itemCount > 1 ? static_cast<int>(kStackSecondX) : 0;
    const int stackHeight = g_card.spec.itemCount > 1 ? static_cast<int>(kStackSecondY) : 0;
    float x = 0.0f;
    float y = 0.0f;
    ClampToWorkArea(cursor.x, cursor.y,
                    static_cast<int>(kCardWidth) + stackWidth,
                    static_cast<int>(kCardHeight) + stackHeight,
                    &x, &y);
    PresentAt(x, y, 1.0f, 1.0f, 255, 255, 255);
}

LRESULT CALLBACK CardWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_TIMER:
        if (wParam == kTickTimerId) {
            Tick();
            return 0;
        }
        if (wParam == kHideMainTimerId) {
            KillTimer(window, kHideMainTimerId);
            if (g_card.spec.onHideMainDelayElapsed) {
                auto callback = g_card.spec.onHideMainDelayElapsed;
                g_card.spec.onHideMainDelayElapsed = nullptr;
                callback();
            }
            return 0;
        }
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_NCHITTEST:
        return HTTRANSPARENT;
    default:
        return DefWindowProcW(window, message, wParam, lParam);
    }
}

bool CreateCardWindow() {
    static bool classRegistered = false;
    HINSTANCE instance = GetModuleHandleW(nullptr);
    if (!classRegistered) {
        WNDCLASSEXW windowClass = {};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.lpfnWndProc = CardWindowProc;
        windowClass.hInstance = instance;
        windowClass.lpszClassName = kWindowClass;
        if (!RegisterClassExW(&windowClass)) return false;
        classRegistered = true;
    }

    POINT cursor = {};
    GetCursorPos(&cursor);
    g_card.window = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT,
        kWindowClass, L"", WS_POPUP, cursor.x, cursor.y, 1, 1,
        nullptr, nullptr, instance, nullptr);
    if (!g_card.window) return false;

    ShowWindow(g_card.window, SW_SHOWNOACTIVATE);
    SetTimer(g_card.window, kTickTimerId, kTickIntervalMs, nullptr);
    return true;
}

}  // namespace

bool RenderToPixels(const CardSpec& spec, float scaleX, float scaleY,
                    unsigned char captionOpacity, unsigned char stackOpacity,
                    std::vector<unsigned char>& outPixels, int& outWidth, int& outHeight) {
    return RenderCardPixels(spec, scaleX, scaleY, captionOpacity, stackOpacity, 255,
                            outPixels, outWidth, outHeight);
}

void BeginDrag(const CardSpec& spec) {
    CancelNow();
    if (!EnsureGdiplus()) return;

    g_card.spec = spec;
    g_card.active = true;
    g_card.phaseStart = GetTickCount64();
    if (!CreateCardWindow()) {
        DestroyCard();
        return;
    }

    const bool morphFromSource = spec.animationsEnabled && spec.hasSourceRect &&
                                 spec.sourceWidth > 0 && spec.sourceHeight > 0;
    g_card.phase = morphFromSource ? Phase::Morph : Phase::Follow;

    const float waveWidth = kCardWidth - kPaddingX * 2.0f;
    g_card.morphWide = morphFromSource ? (std::max)(1.0f, static_cast<float>(spec.sourceWidth) / waveWidth) : 1.0f;
    g_card.morphTall = morphFromSource ? (std::max)(1.0f, static_cast<float>(spec.sourceHeight) / kWaveHeight) : 1.0f;
    g_card.sourceOriginX = morphFromSource
                               ? static_cast<float>(spec.sourceLeft) - kPaddingX * g_card.morphWide
                               : 0.0f;
    g_card.sourceOriginY = morphFromSource
                               ? static_cast<float>(spec.sourceTop) - kPaddingTop * g_card.morphTall
                               : 0.0f;

    if (spec.hideMainDelayMs > 0 && spec.onHideMainDelayElapsed) {
        SetTimer(g_card.window, kHideMainTimerId, spec.hideMainDelayMs, nullptr);
    }

    Tick();
}

void EndDrag(bool dropped) {
    if (!g_card.active) return;

    if (!dropped || !g_card.spec.animationsEnabled) {
        DestroyCard();
        return;
    }

    if (g_card.phase == Phase::Landing) return;
    BeginLandingFromPresentedPosition();
    Tick();
}

void CancelNow() {
    if (!g_card.active && !g_card.window) return;
    DestroyCard();
}

bool IsActive() { return g_card.active; }

}  // namespace HarkenSearch::AudioDragCard

#endif  // _WIN32
