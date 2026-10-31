#include "bootstrap/WindowsBootstrapWebView.h"
#include "BootstrapProgressPage.generated.h"
#include <WebView2.h>
#include <wrl.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <cstdio>

namespace aris::bootstrap {
using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;
namespace {
void trace(const char* stage, HRESULT result) {
#ifdef HARKEN_BOOTSTRAP_WEBVIEW_TESTING
    std::fprintf(stderr, "webview:%s=0x%08lx\n", stage, static_cast<unsigned long>(result));
#else
    static_cast<void>(stage); static_cast<void>(result);
#endif
}
std::wstring wide(const std::string& text) {
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (count <= 0) return {};
    std::wstring value(static_cast<std::size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), value.data(), count);
    return value;
}
}
struct WindowsBootstrapWebView::State {
    HWND parent = nullptr;
    bool active = true, ready = false, started = false, initialNavigation = true;
    std::wstring latest;
    std::function<void(const std::wstring&)> command;
    ComPtr<ICoreWebView2Environment> environment;
    ComPtr<ICoreWebView2Controller> controller;
    ComPtr<ICoreWebView2> webview;
    void Resize() {
        if (controller && IsWindow(parent)) {
            RECT bounds{}; GetClientRect(parent, &bounds); controller->put_Bounds(bounds);
        }
    }
    void Fail() { ready = false; if (controller) controller->put_IsVisible(FALSE); }
};
WindowsBootstrapWebView::WindowsBootstrapWebView(HWND parent, std::function<void(const std::wstring&)> command)
    : state_(std::make_shared<State>()) { state_->parent = parent; state_->command = std::move(command); }
WindowsBootstrapWebView::~WindowsBootstrapWebView() {
    state_->active = false; state_->command = {};
    if (state_->controller) state_->controller->Close();
    state_->webview.Reset(); state_->controller.Reset(); state_->environment.Reset();
}
bool WindowsBootstrapWebView::Ready() const { return state_->ready; }
void WindowsBootstrapWebView::Resize() { state_->Resize(); }
void WindowsBootstrapWebView::Update(const std::string& json) {
    auto value = wide(json);
    if (value == state_->latest) return;
    state_->latest = std::move(value);
    if (state_->ready) {
        const HRESULT sent = state_->webview->PostWebMessageAsJson(state_->latest.c_str());
        if (FAILED(sent)) { trace("post-message-failed", sent); state_->Fail(); }
    }
}
void WindowsBootstrapWebView::Start() {
    if (state_->started) return;
    state_->started = true;
    PWSTR local = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local))) return;
    // Never create a default profile beside the authenticated runtime-pair EXE.
    std::wstring profile = std::wstring(local) + L"\\Harken Search\\UpdateWebView";
    CoTaskMemFree(local);
#ifdef HARKEN_BOOTSTRAP_WEBVIEW_TESTING
    profile += L"-test-" + std::to_wstring(GetCurrentProcessId());
#endif
    const std::weak_ptr<State> weak = state_;
    const HRESULT creation = CreateCoreWebView2EnvironmentWithOptions(nullptr, profile.c_str(), nullptr,
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [weak](HRESULT hr, ICoreWebView2Environment* environment) -> HRESULT {
                trace("environment", hr);
                auto state = weak.lock();
                if (!state || !state->active || FAILED(hr) || !environment) return S_OK;
                state->environment = environment;
                return environment->CreateCoreWebView2Controller(state->parent,
                    Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                        [weak](HRESULT result, ICoreWebView2Controller* controller) -> HRESULT {
                            trace("controller", result);
                            auto s = weak.lock();
                            if (!s || !s->active) { if (controller) controller->Close(); return S_OK; }
                            if (FAILED(result) || !controller) return S_OK;
                            s->controller = controller;
                            if (FAILED(controller->get_CoreWebView2(&s->webview)) || !s->webview) return S_OK;
                            s->Resize(); controller->put_IsVisible(FALSE);
                            ComPtr<ICoreWebView2Settings> settings;
                            if (SUCCEEDED(s->webview->get_Settings(&settings))) {
                                settings->put_AreDevToolsEnabled(FALSE); settings->put_AreDefaultContextMenusEnabled(FALSE);
                                settings->put_IsStatusBarEnabled(FALSE); settings->put_IsZoomControlEnabled(FALSE);
                                ComPtr<ICoreWebView2Settings3> settings3;
                                if (SUCCEEDED(settings.As(&settings3))) settings3->put_AreBrowserAcceleratorKeysEnabled(FALSE);
                            }
                            ComPtr<ICoreWebView2Controller4> controller4;
                            if (SUCCEEDED(s->controller.As(&controller4))) controller4->put_AllowExternalDrop(FALSE);
                            s->webview->add_NavigationStarting(Callback<ICoreWebView2NavigationStartingEventHandler>(
                                [weak](ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs* args) -> HRESULT {
                                    auto current = weak.lock();
                                    // Only our first NavigateToString is admitted. There is no
                                    // address bar, external resource or navigation command.
                                    const bool blocked = !current || !current->active || !current->initialNavigation;
                                    if (current) current->initialNavigation = false;
                                    trace("navigation-blocked", blocked ? E_ACCESSDENIED : S_OK);
                                    args->put_Cancel(blocked);
                                    return S_OK;
                                }).Get(), nullptr);
                            s->webview->add_NewWindowRequested(Callback<ICoreWebView2NewWindowRequestedEventHandler>(
                                [](ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* args) -> HRESULT {
                                    args->put_Handled(TRUE); return S_OK;
                                }).Get(), nullptr);
                            s->webview->add_PermissionRequested(Callback<ICoreWebView2PermissionRequestedEventHandler>(
                                [](ICoreWebView2*, ICoreWebView2PermissionRequestedEventArgs* args) -> HRESULT {
                                    args->put_State(COREWEBVIEW2_PERMISSION_STATE_DENY); return S_OK;
                                }).Get(), nullptr);
                            s->webview->add_ProcessFailed(Callback<ICoreWebView2ProcessFailedEventHandler>(
                                [weak](ICoreWebView2*, ICoreWebView2ProcessFailedEventArgs* args) -> HRESULT {
                                    COREWEBVIEW2_PROCESS_FAILED_KIND kind{}; args->get_ProcessFailedKind(&kind);
                                    trace("process-failed-kind", static_cast<HRESULT>(kind));
                                    if (auto current = weak.lock(); current && current->active) current->Fail();
                                    return S_OK;
                                }).Get(), nullptr);
                            s->webview->add_WebMessageReceived(Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                                [weak](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                                    auto current = weak.lock(); if (!current || !current->active) return S_OK;
                                    LPWSTR raw = nullptr;
                                    if (FAILED(args->TryGetWebMessageAsString(&raw))) return S_OK;
                                    const std::wstring message = raw ? raw : L""; CoTaskMemFree(raw);
                                    if (message == L"ready") {
                                        trace("page-ready", S_OK);
                                        current->ready = true; current->controller->put_IsVisible(TRUE);
                                        if (!current->latest.empty()) current->webview->PostWebMessageAsJson(current->latest.c_str());
                                    } else if (message == L"close") {
                                        if (current->command) current->command(message);
                                    }
                                    return S_OK;
                                }).Get(), nullptr);
                            const auto page = wide(BootstrapProgressPageHtml());
                            const HRESULT navigation = s->webview->NavigateToString(page.c_str());
                            trace("navigate", navigation);
                            if (FAILED(navigation)) s->Fail();
                            return S_OK;
                        }).Get());
            }).Get());
    trace("create", creation);
}
#ifdef HARKEN_BOOTSTRAP_WEBVIEW_TESTING
void WindowsBootstrapWebView::Inspect(std::function<void(std::wstring)> done) {
    if (!Ready()) { done(L"null"); return; }
    state_->webview->ExecuteScript(L"({title:document.getElementById('phase-title').textContent,busy:document.getElementById('updater').dataset.busy,mode:document.getElementById('updater').dataset.mode,count:document.getElementById('file-count').textContent,progress:document.getElementById('progress').getAttribute('aria-valuenow'),version:document.getElementById('version').textContent,logos:[...document.images].every(i=>i.complete&&i.naturalWidth>0),visibleButtons:[...document.querySelectorAll('button')].filter(b=>b.getClientRects().length>0).map(b=>b.textContent),details:document.querySelectorAll('dl,details,.details-panel').length})",
        Callback<ICoreWebView2ExecuteScriptCompletedHandler>([done=std::move(done)](HRESULT hr, LPCWSTR json) -> HRESULT {
            done(SUCCEEDED(hr) && json ? json : L"null"); return S_OK;
        }).Get());
}
void WindowsBootstrapWebView::Capture(const std::wstring& path) {
    if (!Ready()) return;
    ComPtr<IStream> stream;
    if (FAILED(SHCreateStreamOnFileEx(path.c_str(), STGM_CREATE | STGM_WRITE | STGM_SHARE_EXCLUSIVE, FILE_ATTRIBUTE_NORMAL, TRUE, nullptr, &stream))) return;
    state_->webview->CapturePreview(COREWEBVIEW2_CAPTURE_PREVIEW_IMAGE_FORMAT_PNG, stream.Get(),
        Callback<ICoreWebView2CapturePreviewCompletedHandler>([stream](HRESULT) -> HRESULT { return S_OK; }).Get());
}
#endif
} // namespace aris::bootstrap
