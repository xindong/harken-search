#include "bootstrap/WindowsBootstrapProgress.h"
#include "platform/animation_preferences.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <commctrl.h>
#include <uxtheme.h>
#include <objbase.h>
#include "bootstrap/WindowsBootstrapWebView.h"
#include "bootstrap/BootstrapProgressView.h"
#include <algorithm>
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#ifdef HARKEN_BOOTSTRAP_WEBVIEW_TESTING
#include <filesystem>
#include <fstream>
#include <cstdio>
#endif

#pragma comment(linker, "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace aris::bootstrap {
namespace {
constexpr UINT kRefresh = WM_APP + 31;
constexpr COLORREF kBackground = RGB(21, 29, 38);
constexpr COLORREF kText = RGB(231, 237, 243);
constexpr COLORREF kMuted = RGB(178, 191, 204);

const wchar_t* phaseText(BootstrapProgressPhase phase) noexcept {
    switch (phase) {
    case BootstrapProgressPhase::CheckingPackage: return L"正在检查更新文件";
    case BootstrapProgressPhase::WaitingForExit: return L"正在等待程序退出";
    case BootstrapProgressPhase::PreparingFiles: return L"正在准备更新文件";
    case BootstrapProgressPhase::ReplacingFiles: return L"正在替换更新文件";
    case BootstrapProgressPhase::Restarting: return L"正在重新启动 Harken Search";
    case BootstrapProgressPhase::CheckingStartup: return L"正在确认新版本启动";
    case BootstrapProgressPhase::Finalizing: return L"正在完成更新";
    case BootstrapProgressPhase::Restoring: return L"正在恢复文件";
    case BootstrapProgressPhase::Completed: return L"更新已完成";
    case BootstrapProgressPhase::Failed: return L"更新未完成";
    }
    return L"正在更新";
}

std::wstring wideText(const std::string& text) {
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    if (size <= 0) return {};
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, result.data(), size);
    result.pop_back();
    return result;
}

class ProgressWindow final : public BootstrapProgressWindow {
public:
    ProgressWindow(bool reduced, bool allowWebView) : reduced_(reduced), allowWebView_(allowWebView),
        desktop_(GetThreadDesktop(GetCurrentThreadId())),
        ready_(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {}

    ~ProgressWindow() override {
        stop_ = true;
        if (const HWND window = window_.load()) PostMessageW(window, kRefresh, 0, 0);
        if (thread_.joinable()) thread_.join();
        if (ready_) CloseHandle(ready_);
    }

    bool Start() {
        if (!ready_) return false;
        thread_ = std::thread([this] { Run(); });
        // Do not spend the authenticated handoff's timeout creating UI.
        return true;
    }

    void Report(BootstrapProgressSnapshot snapshot) noexcept override {
        try {
            std::lock_guard<std::mutex> lock(mutex_);
            if (finished_) return;
            if (snapshot.phase == BootstrapProgressPhase::Completed ||
                snapshot.phase == BootstrapProgressPhase::Failed) return;
            if (snapshot.phase != snapshot_.phase) stageStarted_ = GetTickCount64();
            snapshot_ = snapshot;
            if (snapshot.phase != BootstrapProgressPhase::Failed &&
                snapshot.phase != BootstrapProgressPhase::Completed) lastActive_ = snapshot.phase;
        } catch (...) {
            // Presentation must never interrupt the file transaction.
        }
    }

    void SetTargetVersion(const std::string& version) noexcept override {
        try { std::lock_guard<std::mutex> lock(mutex_); version_ = version; }
        catch (...) {}
    }

    bool Finish(int exitCode) override {
        WaitForSingleObject(ready_, 5000);
        const HWND window = window_.load();
        if (!window) return false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            finished_ = true;
            exitCode_ = exitCode;
            snapshot_.phase = exitCode == 0 ? BootstrapProgressPhase::Completed
                                           : BootstrapProgressPhase::Failed;
            if (exitCode != 0) error_ = wideText(BootstrapProgressFailureHint(exitCode, lastActive_)) +
                L"（错误码 " + std::to_wstring(exitCode) + L"）";
        }
        PostMessageW(window, kRefresh, 0, 0);
        thread_.join();
        return terminalShown_;
    }

private:
    static LRESULT CALLBACK Procedure(HWND window, UINT message, WPARAM wp, LPARAM lp) {
        auto* self = reinterpret_cast<ProgressWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            self = static_cast<ProgressWindow*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (!self) return DefWindowProcW(window, message, wp, lp);
        switch (message) {
        case WM_TIMER:
        case kRefresh:
            if (self->stop_) DestroyWindow(window);
            else if (wp == 2 && message == WM_TIMER) DestroyWindow(window);
            else self->Refresh(window);
            return 0;
        case WM_CLOSE:
            if (self->dismissible_) DestroyWindow(window);
            return 0;
        case WM_SIZE:
            if (self->web_) self->web_->Resize();
            return 0;
        case WM_COMMAND:
            if (LOWORD(wp) == IDCANCEL && self->dismissible_) DestroyWindow(window);
            return 0;
        case WM_CTLCOLORSTATIC:
            SetBkColor(reinterpret_cast<HDC>(wp), kBackground);
            SetTextColor(reinterpret_cast<HDC>(wp),
                GetDlgCtrlID(reinterpret_cast<HWND>(lp)) == 101 ? kText : kMuted);
            return reinterpret_cast<LRESULT>(self->background_);
        case WM_ERASEBKGND: {
            RECT area{};
            GetClientRect(window, &area);
            FillRect(reinterpret_cast<HDC>(wp), &area, self->background_);
            return 1;
        }
        case WM_DESTROY:
            self->window_ = nullptr;
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProcW(window, message, wp, lp);
    }

    void SetText(HWND window, int id, const std::wstring& text) {
        wchar_t previous[2048]{};
        GetDlgItemTextW(window, id, previous, 2048);
        if (text != previous) SetDlgItemTextW(window, id, text.c_str());
    }

    void Refresh(HWND window) {
        if (terminalDeadline_ != 0 && GetTickCount64() >= terminalDeadline_) {
            DestroyWindow(window);
            return;
        }
        BootstrapProgressSnapshot snapshot;
        BootstrapProgressPhase last;
        bool finished;
        int exitCode;
        ULONGLONG started;
        std::wstring error;
        std::string version;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            snapshot = snapshot_;
            last = lastActive_;
            finished = finished_;
            exitCode = exitCode_;
            started = stageStarted_;
            error = error_;
            version = version_;
        }
        if (web_) {
            // Metadata is published only after the authenticated broker owns
            // non-inheritable copies of its capability handles.
            if (!version.empty()) web_->Start();
            BootstrapProgressPresentation state;
            state.snapshot = snapshot; state.lastActive = last;
            state.targetVersion = version; state.finished = finished; state.exitCode = exitCode;
            state.elapsedSeconds = (GetTickCount64() - windowStarted_) / 1000;
            state.reducedMotion = reduced_ || !Platform::Animation::ClientAreaAnimationEnabled();
            web_->Update(BootstrapProgressJson(state));
#ifdef HARKEN_BOOTSTRAP_WEBVIEW_TESTING
            wchar_t captureRoot[32768]{};
            const DWORD length = GetEnvironmentVariableW(L"HARKEN_UPDATE_TEST_CAPTURE", captureRoot, 32768);
            if (web_->Ready() && length > 0 && length < 32768 && GetTickCount64() - capturedAt_ >= 120) {
                capturedAt_ = GetTickCount64();
                const auto base = std::filesystem::path(captureRoot) / std::to_wstring(++captureSequence_);
                web_->Capture(base.wstring() + L".png");
                std::ofstream(base.wstring() + L".json") << BootstrapProgressJson(state);
                web_->Inspect([base](std::wstring observed) {
                    const int count = WideCharToMultiByte(CP_UTF8, 0, observed.data(), static_cast<int>(observed.size()), nullptr, 0, nullptr, nullptr);
                    std::string text(static_cast<std::size_t>(count), '\0');
                    WideCharToMultiByte(CP_UTF8, 0, observed.data(), static_cast<int>(observed.size()), text.data(), count, nullptr, nullptr);
                    std::ofstream(base.wstring() + L".dom.json") << text;
                });
            }
#endif
            const bool ready = web_->Ready();
            const int height = ready && !finished ? 200 : ready && exitCode == 0 ? 200 : 280;
            if (height != clientHeight_) {
                clientHeight_ = height;
                const UINT dpi = GetDpiForWindow(window);
                RECT bounds{0, 0, MulDiv(540, static_cast<int>(dpi), 96), MulDiv(height, static_cast<int>(dpi), 96)};
                AdjustWindowRectExForDpi(&bounds, static_cast<DWORD>(GetWindowLongPtrW(window, GWL_STYLE)), FALSE, 0, dpi);
                SetWindowPos(window, nullptr, 0, 0, bounds.right - bounds.left, bounds.bottom - bounds.top,
                    SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
            }
            for (const int id : {101, 102, 103, 104})
                ShowWindow(GetDlgItem(window, id), ready ? SW_HIDE : SW_SHOW);
            ShowWindow(GetDlgItem(window, IDCANCEL), !ready && finished && exitCode != 0 ? SW_SHOW : SW_HIDE);
        }
        const bool failed = snapshot.phase == BootstrapProgressPhase::Failed;
        const bool completed = snapshot.phase == BootstrapProgressPhase::Completed;
        const bool counted = snapshot.phase == BootstrapProgressPhase::ReplacingFiles && snapshot.totalFiles > 0;
        SetText(window, 101, phaseText(snapshot.phase));
        std::wstring detail;
        if (failed) detail = std::wstring(L"停在：") + phaseText(last);
        else if (completed) detail = L"新版本已启动。";
        else if (counted) detail = L"已处理 " + std::to_wstring(snapshot.completedFiles) + L" / " +
            std::to_wstring(snapshot.totalFiles) + L" 个文件";
        else detail = L"已等待 " + std::to_wstring((GetTickCount64() - started) / 1000) + L" 秒";
        SetText(window, 102, detail);
        SetText(window, 104, failed ? error : L"完成后会自动返回 Harken Search。");

        const bool marquee = Platform::Animation::ClientAreaAnimationEnabled() &&
            !reduced_ && !counted && !failed && !completed;
        const HWND bar = GetDlgItem(window, 103);
        if (marquee != marquee_) {
            SendMessageW(bar, PBM_SETMARQUEE, FALSE, 0);
            auto style = GetWindowLongPtrW(bar, GWL_STYLE);
            SetWindowLongPtrW(bar, GWL_STYLE, marquee ? style | PBS_MARQUEE : style & ~PBS_MARQUEE);
            SendMessageW(bar, PBM_SETMARQUEE, marquee, 25);
            marquee_ = marquee;
        }
        if (!marquee) {
            const int position = completed ? 1000 : counted
                ? static_cast<int>(1000.0 * static_cast<double>(std::min(snapshot.completedFiles, snapshot.totalFiles)) /
                                   static_cast<double>(snapshot.totalFiles)) : 0;
            SendMessageW(bar, PBM_SETPOS, static_cast<WPARAM>(position), 0);
        }
        SendMessageW(bar, PBM_SETSTATE, failed ? PBST_ERROR : PBST_NORMAL, 0);
        if (finished && !terminalShown_) {
            terminalShown_ = true;
            // Bounded lifetime: terminal cleanup is waiting for this process.
            if (exitCode == 0) {
                terminalDeadline_ = GetTickCount64() + 1200;
                if (!SetTimer(window, 2, 1200, nullptr) && !refreshTimer_) DestroyWindow(window);
            }
            else {
                dismissible_ = true;
                EnableMenuItem(GetSystemMenu(window, FALSE), SC_CLOSE, MF_BYCOMMAND | MF_ENABLED);
                if (!web_ || !web_->Ready()) ShowWindow(GetDlgItem(window, IDCANCEL), SW_SHOW);
            }
        }
    }

    void Run() noexcept {
        try {
            // Test desktops are inherited explicitly; never activate a desktop.
            if (!SetThreadDesktop(desktop_)) { SetEvent(ready_); return; }
            comInitialized_ = SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED));
            SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_SYSTEM_AWARE);
            INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_PROGRESS_CLASS};
            InitCommonControlsEx(&controls);
            const HINSTANCE instance = GetModuleHandleW(nullptr);
            WNDCLASSW cls{};
            cls.lpfnWndProc = Procedure;
            cls.hInstance = instance;
            cls.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
            cls.lpszClassName = L"HarkenSearchUpdateProgress";
            RegisterClassW(&cls);
            background_ = CreateSolidBrush(kBackground);
            const UINT dpi = GetDpiForSystem();
            const auto px = [dpi](int value) { return MulDiv(value, static_cast<int>(dpi), 96); };
            font_ = CreateFontW(-px(14), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");
            titleFont_ = CreateFontW(-px(21), 0, 0, 0, FW_MEDIUM, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");
            RECT area{0, 0, px(540), px(280)};
            constexpr DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
            AdjustWindowRectExForDpi(&area, style, FALSE, 0, dpi);
            RECT screen{};
            SystemParametersInfoW(SPI_GETWORKAREA, 0, &screen, 0);
            const HWND window = CreateWindowExW(0, cls.lpszClassName, L"Harken Search 更新", style,
                screen.left + (screen.right - screen.left - area.right + area.left) / 2,
                screen.top + (screen.bottom - screen.top - area.bottom + area.top) / 2,
                area.right - area.left, area.bottom - area.top, nullptr, nullptr, instance, this);
            if (window) {
                const auto control = [&](const wchar_t* type, const wchar_t* text, DWORD extra,
                                         int id, int x, int y, int width, int height) {
                    HWND child = CreateWindowExW(0, type, text, WS_CHILD | WS_VISIBLE | extra,
                        px(x), px(y), px(width), px(height), window,
                        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance, nullptr);
                    SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(id == 101 ? titleFont_ : font_), TRUE);
                    return child;
                };
                control(L"STATIC", L"正在检查更新文件", SS_LEFT, 101, 28, 26, 484, 34);
                control(L"STATIC", L"", SS_LEFT, 102, 28, 73, 484, 24);
                const HWND bar = control(PROGRESS_CLASSW, L"", 0, 103, 28, 110, 484, 8);
                SetWindowTheme(bar, L"", L"");
                SendMessageW(bar, PBM_SETRANGE32, 0, 1000);
                SendMessageW(bar, PBM_SETBKCOLOR, 0, RGB(49, 64, 80));
                SendMessageW(bar, PBM_SETBARCOLOR, 0, RGB(161, 202, 235));
                control(L"STATIC", L"", SS_LEFT, 104, 28, 140, 484, 78);
                const HWND close = control(L"BUTTON", L"关闭", BS_PUSHBUTTON | WS_TABSTOP,
                    IDCANCEL, 414, 230, 98, 32);
                ShowWindow(close, SW_HIDE);
                EnableMenuItem(GetSystemMenu(window, FALSE), SC_CLOSE, MF_BYCOMMAND | MF_GRAYED);
                window_ = window;
                if (comInitialized_ && allowWebView_) {
                    web_ = std::make_unique<WindowsBootstrapWebView>(window,
                        [window](const std::wstring& command) {
                            if (command == L"close") PostMessageW(window, WM_CLOSE, 0, 0);
                        });
                }
                Refresh(window);
                refreshTimer_ = SetTimer(window, 1, 50, nullptr) != 0;
                ShowWindow(window, SW_SHOWNOACTIVATE);
                UpdateWindow(window);
            }
            SetEvent(ready_);
            if (window) {
                MSG message{};
                while (GetMessageW(&message, nullptr, 0, 0) > 0) {
                    if (!IsDialogMessageW(window, &message)) {
                        TranslateMessage(&message);
                        DispatchMessageW(&message);
                    }
                }
            }
        } catch (...) {
            if (const HWND window = window_.load()) DestroyWindow(window);
            SetEvent(ready_);
        }
        if (font_) DeleteObject(font_);
        if (titleFont_) DeleteObject(titleFont_);
        if (background_) DeleteObject(background_);
        web_.reset();
        if (comInitialized_) CoUninitialize();
    }

    bool reduced_;
    bool allowWebView_;
    HDESK desktop_;
    HANDLE ready_;
    std::thread thread_;
    std::mutex mutex_;
    std::atomic<HWND> window_{nullptr};
    std::atomic<bool> stop_{false};
    BootstrapProgressSnapshot snapshot_;
    BootstrapProgressPhase lastActive_ = BootstrapProgressPhase::CheckingPackage;
    ULONGLONG stageStarted_ = GetTickCount64();
    bool finished_ = false;
    int exitCode_ = 0;
    std::wstring error_;
    std::string version_;
    const ULONGLONG windowStarted_ = GetTickCount64();
    // Below fields belong only to the window thread.
    bool marquee_ = false;
    bool dismissible_ = false;
    bool terminalShown_ = false;
    bool refreshTimer_ = false;
    ULONGLONG terminalDeadline_ = 0;
    HFONT font_ = nullptr;
    HFONT titleFont_ = nullptr;
    HBRUSH background_ = nullptr;
    bool comInitialized_ = false;
    int clientHeight_ = 280;
    std::unique_ptr<WindowsBootstrapWebView> web_;
#ifdef HARKEN_BOOTSTRAP_WEBVIEW_TESTING
    ULONGLONG capturedAt_ = 0;
    unsigned captureSequence_ = 0;
#endif
};
}

std::unique_ptr<BootstrapProgressWindow> CreateBootstrapProgressWindow(bool reduced, bool allowWebView) noexcept {
    try {
        auto window = std::make_unique<ProgressWindow>(reduced, allowWebView);
        if (window->Start()) return window;
    } catch (...) {
        // Keep the original updater usable when UI resources are unavailable.
    }
    return nullptr;
}
}  // namespace aris::bootstrap
