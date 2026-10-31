#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <commctrl.h>
#include <string>
#include <thread>
#include "bootstrap/WindowsBootstrapProgress.h"

using namespace aris::bootstrap;

namespace {
HWND findWindow() {
    HWND found = nullptr;
    const auto started = GetTickCount64();
    while (!found && GetTickCount64() - started < 3000) {
    EnumDesktopWindows(GetThreadDesktop(GetCurrentThreadId()),
        [](HWND window, LPARAM data) -> BOOL {
            DWORD pid = 0;
            GetWindowThreadProcessId(window, &pid);
            wchar_t name[128]{};
            GetClassNameW(window, name, 128);
            if (pid == GetCurrentProcessId() &&
                std::wstring(name) == L"HarkenSearchUpdateProgress") {
                *reinterpret_cast<HWND*>(data) = window;
            }
            return TRUE;
        }, reinterpret_cast<LPARAM>(&found));
        if (!found) Sleep(15);
    }
    return found;
}
bool waitForText(HWND window, int control, const wchar_t* text) {
    const auto start = GetTickCount64();
    while (GetTickCount64() - start < 3000) {
        wchar_t actual[1024]{};
        GetDlgItemTextW(window, control, actual, 1024);
        if (std::wstring(actual).find(text) != std::wstring::npos) return true;
        Sleep(15);
    }
    return false;
}
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    const bool preview = std::wstring(GetCommandLineW()).find(L"--preview") != std::wstring::npos;
    HDESK original = GetThreadDesktop(GetCurrentThreadId());
    HDESK isolated = nullptr;
    if (!preview) {
        const auto name = L"HarkenProgressTest-" + std::to_wstring(GetCurrentProcessId());
        isolated = CreateDesktopW(name.c_str(), nullptr, nullptr, 0, GENERIC_ALL, nullptr);
        if (!isolated || !SetThreadDesktop(isolated)) return 2;
    }
    int failure = 0;
    {
        auto progress = CreateBootstrapProgressWindow(!preview);
        if (!progress) return 1;
        HWND window = findWindow();
        if (!window) return 3;
        if (preview) SetWindowTextW(window, L"Harken Search 更新过程预览（测试输入）");
        progress->Report({BootstrapProgressPhase::WaitingForExit});
        if (!waitForText(window, 101, L"等待程序退出")) return 4;
        // Close while busy must not cancel or hide a running transaction.
        PostMessageW(window, WM_CLOSE, 0, 0);
        DWORD_PTR response = 0;
        if (!SendMessageTimeoutW(window, WM_NULL, 0, 0, SMTO_ABORTIFHUNG, 1000, &response) ||
            !IsWindowVisible(window)) return 5;
        if (preview) Sleep(2200);
        progress->Report({BootstrapProgressPhase::PreparingFiles});
        if (preview) Sleep(1600);
        progress->Report({BootstrapProgressPhase::ReplacingFiles, 1, 4});
        if (!waitForText(window, 102, L"1 / 4")) return 6;
        const auto bar = GetDlgItem(window, 103);
        if (!bar || SendMessageW(bar, PBM_GETPOS, 0, 0) != 250) return 7;
        if ((GetWindowLongPtrW(bar, GWL_STYLE) & PBS_MARQUEE) != 0) return 8;
        if (preview) {
            Sleep(1600);
            progress->Report({BootstrapProgressPhase::ReplacingFiles, 3, 4});
            Sleep(1600);
            progress->Report({BootstrapProgressPhase::ReplacingFiles, 4, 4});
            Sleep(500);
        }
        progress->Report({BootstrapProgressPhase::Restarting});
        if (!waitForText(window, 101, L"重新启动")) return 9;
        if (!preview && (GetWindowLongPtrW(bar, GWL_STYLE) & PBS_MARQUEE) != 0) return 10;
        if (preview) Sleep(2000);
        progress->Report({BootstrapProgressPhase::CheckingStartup});
        if (preview) Sleep(2000);
        progress->Report({BootstrapProgressPhase::Finalizing});
        if (preview) Sleep(1200);
        else {
            progress->Report({BootstrapProgressPhase::Completed});
            Sleep(100);
            if (!waitForText(window, 101, L"正在完成更新")) return 16;
        }
        std::thread finish([&] { if (!progress->Finish(0)) failure = 11; });
        const bool completed = waitForText(window, 101, L"更新已完成");
        // Loss of the dedicated terminal timer must not strand the updater.
        KillTimer(window, 2);
        finish.join();
        if (!completed || IsWindow(window)) return 11;
    }
    if (!preview) {
        auto progress = CreateBootstrapProgressWindow(true);
        if (!progress) return 12;
        HWND window = findWindow();
        progress->Report({BootstrapProgressPhase::WaitingForExit});
        if (!waitForText(window, 101, L"等待程序退出")) return 13;
        std::thread finish([&] { if (!progress->Finish(21)) failure = 14; });
        const bool errorVisible = waitForText(window, 104, L"21");
        const bool stopped = waitForText(window, 101, L"更新未完成");
        const bool stageVisible = waitForText(window, 102, L"等待程序退出");
        PostMessageW(window, WM_CLOSE, 0, 0);
        finish.join();
        if (!errorVisible || !stopped || !stageVisible) failure = 15;
    }
    if (!preview) {
        auto progress = CreateBootstrapProgressWindow(true, false);
        if (!progress) return 17;
        HWND window = findWindow();
        progress->Report({BootstrapProgressPhase::CheckingStartup});
        if (!waitForText(window, 101, L"确认新版本启动")) return 18;
        progress->Report({BootstrapProgressPhase::Failed});
        Sleep(100);
        if (!waitForText(window, 101, L"确认新版本启动")) return 19;
        std::thread finish([&] { if (!progress->Finish(50)) failure = 20; });
        const bool honestHint = waitForText(window, 104, L"新版未确认就绪");
        PostMessageW(window, WM_CLOSE, 0, 0);
        finish.join();
        if (!honestHint) failure = 21;
    }
    if (isolated) {
        SetThreadDesktop(original);
        CloseDesktop(isolated);
    }
    return failure;
}
