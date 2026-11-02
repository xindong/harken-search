#pragma once
#include <windows.h>
#include <functional>
#include <memory>
#include <string>

namespace aris::bootstrap {
// @stage7-contract
// API contract: create/use/destroy on the owning STA thread; parent must outlive
// the host. Start is asynchronous, Update copies only display JSON, and Ready
// remains false on failure. Destruction invalidates weak callbacks and closes
// its controller without waiting for external browser processes. No install,
// navigation, shell or cancellation authority is exposed to the page.
// UI-thread-only. Async COM callbacks hold weak state, never the transaction.
class WindowsBootstrapWebView final {
public:
    WindowsBootstrapWebView(HWND parent, std::function<void(const std::wstring&)> command);
    ~WindowsBootstrapWebView();
    void Start();
    void Update(const std::string& json);
    void Resize();
    bool Ready() const;
#ifdef HARKEN_BOOTSTRAP_WEBVIEW_TESTING
    void Inspect(std::function<void(std::wstring)> done);
    void Capture(const std::wstring& path);
#endif
private:
    struct State;
    std::shared_ptr<State> state_;
};
} // namespace aris::bootstrap
