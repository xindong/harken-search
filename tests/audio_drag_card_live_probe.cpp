// AudioDragCard 实时探针：在独立桌面上真实创建拖出小卡，核对接手窗口位置、
// 缩卡/跟手/落地的时间线和窗口销毁，并尝试抓取分层窗口的实际画面。
// 不占用用户桌面、指针或焦点；仅用于本卡验证，不进入产品。

#include "AudioDragCard.h"
#include "audio_drag_card_wav_fixture.h"

#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace {
bool simulatedLeftButtonDown = false;
}

SHORT WINAPI ProbeGetAsyncKeyState(int key) {
    return key == VK_LBUTTON && simulatedLeftButtonDown ? static_cast<SHORT>(0x8000) : 0;
}

BOOL WINAPI ProbeGetCursorPos(LPPOINT cursor) {
    cursor->x = 400;
    cursor->y = 300;
    return TRUE;
}

namespace HarkenSearch::AudioDragCardProbe {
using CardSpec = HarkenSearch::AudioDragCard::CardSpec;
void CancelNow();
}

#define AudioDragCard AudioDragCardProbe
#define GetAsyncKeyState ProbeGetAsyncKeyState
#define GetCursorPos ProbeGetCursorPos
#include "../src/AudioDragCard.cpp"
#undef GetCursorPos
#undef GetAsyncKeyState
#undef AudioDragCard

namespace {

using namespace Gdiplus;

HWND FindCardWindow() {
    struct Search {
        HWND found = nullptr;
    } search;
    EnumWindows([](HWND window, LPARAM param) -> BOOL {
        wchar_t className[128] = {};
        GetClassNameW(window, className, 127);
        if (wcscmp(className, L"HarkenSearchAudioDragCard") == 0) {
            reinterpret_cast<Search*>(param)->found = window;
            return FALSE;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&search));
    return search.found;
}

int EncoderClsidForMime(const wchar_t* mime, CLSID* out) {
    UINT count = 0;
    UINT size = 0;
    GetImageEncodersSize(&count, &size);
    if (size == 0) return -1;
    std::vector<unsigned char> buffer(size);
    auto* encoders = reinterpret_cast<ImageCodecInfo*>(buffer.data());
    GetImageEncoders(count, size, encoders);
    for (UINT i = 0; i < count; ++i) {
        if (wcscmp(encoders[i].MimeType, mime) == 0) {
            *out = encoders[i].Clsid;
            return static_cast<int>(i);
        }
    }
    return -1;
}

bool CaptureWindow(HWND window, const std::filesystem::path& path) {
    RECT rect = {};
    GetWindowRect(window, &rect);
    const int width = rect.right - rect.left;
    const int height = rect.bottom - rect.top;
    if (width <= 0 || height <= 0) return false;

    HDC screen = GetDC(nullptr);
    HDC memory = CreateCompatibleDC(screen);
    HBITMAP bitmap = CreateCompatibleBitmap(screen, width, height);
    HGDIOBJ previous = SelectObject(memory, bitmap);
    const BOOL printed = PrintWindow(window, memory, 0x00000002 /*PW_RENDERFULLCONTENT*/);

    Bitmap output(width, height, PixelFormat32bppPARGB);
    Graphics graphics(&output);
    HDC target = graphics.GetHDC();
    BitBlt(target, 0, 0, width, height, memory, 0, 0, SRCCOPY);
    graphics.ReleaseHDC(target);

    CLSID encoder = {};
    const bool saved = EncoderClsidForMime(L"image/png", &encoder) >= 0 &&
                       output.Save(path.c_str(), &encoder, nullptr) == Ok;

    SelectObject(memory, previous);
    DeleteObject(bitmap);
    DeleteDC(memory);
    ReleaseDC(nullptr, screen);
    std::printf("%s %s (%dx%d): %s\n", saved ? "CAPTURED" : "SAVE-FAILED",
                std::filesystem::path(path).filename().string().c_str(), width, height,
                printed ? "PrintWindow ok" : "PrintWindow failed");
    return saved;
}

void PumpMessages(int milliseconds) {
    const ULONGLONG deadline = GetTickCount64() + static_cast<ULONGLONG>(milliseconds);
    MSG message = {};
    do {
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        Sleep(4);
    } while (GetTickCount64() < deadline);
}

void Snapshot(const char* label) {
    const HWND window = FindCardWindow();
    if (!window) {
        std::printf("%s: no card window\n", label);
        return;
    }
    RECT rect = {};
    GetWindowRect(window, &rect);
    std::printf("%s: window=(%ld,%ld) size=%ldx%ld\n", label,
                rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top);
}

bool Check(bool condition, const char* description) {
    std::printf("%s: %s\n", condition ? "PASS" : "FAIL", description);
    return condition;
}

bool TestDropResultGating() {
    namespace card = HarkenSearch::AudioDragCardProbe;
    bool ok = true;
    card::CardSpec spec;
    spec.fileName = L"simulated-drag.wav";
    spec.animationsEnabled = true;

    auto beginReleasedDrag = [&]() {
        simulatedLeftButtonDown = true;
        card::BeginDrag(spec);
        PumpMessages(35);
        simulatedLeftButtonDown = false;
        PumpMessages(140);
    };

    beginReleasedDrag();
    ok &= Check(card::IsActive(), "pending rejection: card remains visible");
    ok &= Check(card::g_card.phase != card::Phase::Landing,
                "pending rejection: no landing before drop result");
    card::EndDrag(false);
    ok &= Check(!card::IsActive(), "rejection: card closes immediately");

    beginReleasedDrag();
    PumpMessages(300);
    ok &= Check(card::IsActive(), "long pending result: card remains visible");
    ok &= Check(card::IsActive() && card::g_card.phase != card::Phase::Landing,
                "long pending result: still no landing");
    const bool wasPendingBeforeRejection = card::IsActive();
    card::EndDrag(false);
    ok &= Check(wasPendingBeforeRejection && !card::IsActive(),
                "long pending rejection: card closes immediately");

    beginReleasedDrag();
    PumpMessages(300);
    ok &= Check(card::IsActive() && card::g_card.phase != card::Phase::Landing,
                "pending success: no landing before drop result");
    const float startY = card::g_card.presentedY;
    card::EndDrag(true);
    ok &= Check(card::IsActive() && card::g_card.phase == card::Phase::Landing,
                "successful drop: starts landing");
    PumpMessages(90);
    ok &= Check(card::IsActive() && card::g_card.presentedY > startY,
                "successful drop: card moves downward");
    PumpMessages(220);
    ok &= Check(!card::IsActive(), "successful drop: card closes after landing");

    beginReleasedDrag();
    card::CancelNow();
    ok &= Check(!card::IsActive(), "cancellation: card closes immediately");

    spec.animationsEnabled = false;
    simulatedLeftButtonDown = true;
    card::BeginDrag(spec);
    card::EndDrag(true);
    ok &= Check(!card::IsActive(), "animations disabled: successful drop closes immediately");
    simulatedLeftButtonDown = false;
    card::CancelNow();
    return ok;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    const bool resultGating = argc > 1 && wcscmp(argv[1], L"--result-gating") == 0;
    std::filesystem::path outputDir = argc > 1 ? argv[1] : std::filesystem::current_path();
    if (!resultGating) std::filesystem::create_directories(outputDir);

    GdiplusStartupInput input;
    ULONG_PTR token = 0;
    if (GdiplusStartup(&token, &input, nullptr) != Ok) {
        std::printf("FAIL: GdiplusStartup\n");
        return 1;
    }

    // 独立桌面：不改动用户桌面、指针与焦点。
    const std::wstring desktopName = L"HarkenDragCardProbe-" + std::to_wstring(GetCurrentProcessId());
    HDESK desktop = CreateDesktopW(desktopName.c_str(), nullptr, nullptr, 0, GENERIC_ALL, nullptr);
    if (!desktop) {
        std::printf("FAIL: CreateDesktop\n");
        return 1;
    }
    if (!SetThreadDesktop(desktop)) {
        std::printf("FAIL: SetThreadDesktop\n");
        return 1;
    }

    if (resultGating) {
        const bool ok = TestDropResultGating();
        CloseDesktop(desktop);
        GdiplusShutdown(token);
        return ok ? 0 : 1;
    }

    HarkenSearch::AudioDragCard::CardSpec spec;
    spec.fileName = L"(no audio)";
    spec.itemCount = 3;
    if (argc > 2) {
        std::vector<unsigned char> wavBytes;
        audio_drag_card_test::WavInfo wav;
        const std::filesystem::path wavPath = argv[2];
        if (audio_drag_card_test::ParseWav(wavPath, wavBytes, wav)) {
            spec.envelope = audio_drag_card_test::BuildEnvelopeFromWav(wav, 76);
            spec.fileName = wavPath.filename().wstring();
            std::printf("WAV %s: %d ch, %zu frames\n", wavPath.filename().string().c_str(),
                        wav.channels, wav.sampleFrames);
        }
    }
    spec.hasSourceRect = true;
    spec.sourceLeft = 120;
    spec.sourceTop = 160;
    spec.sourceWidth = 620;
    spec.sourceHeight = 96;

    HarkenSearch::AudioDragCard::BeginDrag(spec);
    PumpMessages(40);
    Snapshot("t=40ms");
    if (const HWND window = FindCardWindow()) CaptureWindow(window, outputDir / L"live-morph.png");
    PumpMessages(160);
    Snapshot("t=200ms");
    PumpMessages(160);
    Snapshot("t=360ms");
    if (const HWND window = FindCardWindow()) CaptureWindow(window, outputDir / L"live-follow.png");

    HarkenSearch::AudioDragCard::EndDrag(true);
    PumpMessages(70);
    Snapshot("landing t=70ms");
    if (const HWND window = FindCardWindow()) CaptureWindow(window, outputDir / L"live-landing.png");
    PumpMessages(260);
    const bool stillActive = HarkenSearch::AudioDragCard::IsActive();
    std::printf("after landing: active=%d\n", stillActive ? 1 : 0);

    HarkenSearch::AudioDragCard::CancelNow();
    CloseDesktop(desktop);
    GdiplusShutdown(token);
    return stillActive ? 1 : 0;
}
