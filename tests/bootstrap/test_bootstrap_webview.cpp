#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <objbase.h>
#include "bootstrap/WindowsBootstrapWebView.h"
#include "bootstrap/BootstrapProgressView.h"
#include "nlohmann_json.hpp"
#include <iostream>
#include <stdexcept>

using namespace aris::bootstrap;
static void require(bool ok, const char* why) { if (!ok) throw std::runtime_error(why); }
static void pump(unsigned milliseconds) {
    const auto end = GetTickCount64() + milliseconds;
    do {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); }
        Sleep(5);
    } while (GetTickCount64() < end);
}
static nlohmann::json inspect(WindowsBootstrapWebView& web) {
    struct Read { bool ready=false; std::wstring value; };
    auto read=std::make_shared<Read>();
    web.Inspect([read](std::wstring value) {read->value=std::move(value); read->ready=true;});
    const auto end=GetTickCount64()+5000;
    while(!read->ready && GetTickCount64()<end) pump(10);
    require(read->ready, "DOM inspection timed out");
    const int count=WideCharToMultiByte(CP_UTF8,0,read->value.data(),static_cast<int>(read->value.size()),nullptr,0,nullptr,nullptr);
    std::string text(static_cast<std::size_t>(count),'\0');
    WideCharToMultiByte(CP_UTF8,0,read->value.data(),static_cast<int>(read->value.size()),text.data(),count,nullptr,nullptr);
    return nlohmann::json::parse(text);
}
int main(int argc, char**) {
    if (argc == 1) {
        auto name=L"HarkenUpdateWebViewTest-"+std::to_wstring(GetCurrentProcessId());
        HDESK desktop=CreateDesktopW(name.c_str(),nullptr,nullptr,0,GENERIC_ALL,nullptr);
        if (!desktop) return 2;
        wchar_t executable[32768]{};GetModuleFileNameW(nullptr,executable,32768);
        std::wstring command=L"\""+std::wstring(executable)+L"\" --isolated";
        STARTUPINFOW startup{};startup.cb=sizeof(startup);startup.lpDesktop=name.data();
        startup.dwFlags=STARTF_USESTDHANDLES;startup.hStdInput=GetStdHandle(STD_INPUT_HANDLE);
        startup.hStdOutput=GetStdHandle(STD_OUTPUT_HANDLE);startup.hStdError=GetStdHandle(STD_ERROR_HANDLE);
        PROCESS_INFORMATION process{};
        if(!CreateProcessW(executable,command.data(),nullptr,nullptr,TRUE,0,nullptr,nullptr,&startup,&process)) {CloseDesktop(desktop);return 4;}
        const auto waited=WaitForSingleObject(process.hProcess,25000);
        DWORD code=5;GetExitCodeProcess(process.hProcess,&code);
        if(waited!=WAIT_OBJECT_0) TerminateProcess(process.hProcess,5);
        CloseHandle(process.hThread);CloseHandle(process.hProcess);CloseDesktop(desktop);
        return static_cast<int>(code);
    }
    if(FAILED(CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED))) return 3;
    int failure=0;
    HWND window=nullptr;
    try {
        WNDCLASSW cls{}; cls.hInstance=GetModuleHandleW(nullptr); cls.lpfnWndProc=DefWindowProcW; cls.lpszClassName=L"HarkenBootstrapWebViewTests";
        RegisterClassW(&cls);
        window=CreateWindowW(cls.lpszClassName,L"Harken updater isolated test",WS_OVERLAPPEDWINDOW,0,0,560,360,nullptr,nullptr,cls.hInstance,nullptr);
        require(window!=nullptr,"private desktop host"); ShowWindow(window,SW_SHOWNOACTIVATE);
        {
            WindowsBootstrapWebView web(window,[](const std::wstring&) {});
            BootstrapProgressPresentation state;
            state.targetVersion="1.0.42-test";state.snapshot={BootstrapProgressPhase::WaitingForExit};state.reducedMotion=true;
            web.Update(BootstrapProgressJson(state));web.Start();
            const auto deadline=GetTickCount64()+15000;
            while(!web.Ready() && GetTickCount64()<deadline) pump(25);
            require(web.Ready(),"actual WebView2 page did not load");pump(100);
            auto value=inspect(web);
            require(value["title"]==u8"正在退出程序…" && value["logos"]==true,"initial real phase and embedded official logos");
            require(value["version"]=="v1.0.42-test","host version is displayed");
            require(value["visibleButtons"].empty() && value["details"]==0,"busy updater has no start/later buttons or details panel");
            pump(1800);
            require(inspect(web)["title"]==u8"正在退出程序…","waiting cannot advance on a demo timer");
            state.snapshot={BootstrapProgressPhase::ReplacingFiles,3,12};web.Update(BootstrapProgressJson(state));pump(100);
            value=inspect(web);
            require(value["progress"]=="3" && value["count"]==u8"3 / 12 个","actual file count reached DOM");
            state.lastActive=BootstrapProgressPhase::ReplacingFiles;state.finished=true;state.exitCode=86;
            web.Update(BootstrapProgressJson(state));pump(100);value=inspect(web);
            require(value["mode"]=="failed" && value["busy"]=="false","failure stops the real page");
            require(value["visibleButtons"]==nlohmann::json::array({u8"关闭"}),"only close appears after failure");
            state.finished=false;state.snapshot={BootstrapProgressPhase::ReplacingFiles,4,12};web.Update(BootstrapProgressJson(state));pump(100);
            require(inspect(web)["mode"]=="failed","late updates cannot overwrite terminal state");
        }
        // A pending browser callback must not dereference a destroyed host.
        { WindowsBootstrapWebView cancelled(window,[](const std::wstring&){});cancelled.Start(); }
        pump(1500);
        std::cout<<"actual WebView2 DOM, timing, file counts, terminal state, pending destruction: passed\n";
    } catch(const std::exception& error) { std::cerr<<error.what()<<'\n';failure=1; }
    if(window) DestroyWindow(window);
    CoUninitialize();
    return failure;
}
