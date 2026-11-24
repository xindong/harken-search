// Contract for refreshing the Explorer icon cache of taskbar pins after an
// in-place upgrade: only pins that launch this executable with its default icon
// qualify, and a user-chosen icon must stay untouched.

#include "platform/pinned_taskbar_icon_refresh.h"

#include <cstdio>
#include <string>

namespace {

bool Check(bool condition, const char* message) {
    if (!condition) {
        std::printf("FAIL: %s\n", message);
        return false;
    }
    return true;
}

} // namespace

int main() {
    using Platform::Detail::PinnedShortcutUsesDefaultAppIcon;

    const std::wstring exePath = L"C:\\Program Files\\Harken Search\\HarkenSearch.exe";

    if (!Check(PinnedShortcutUsesDefaultAppIcon(exePath, L"", 0, exePath),
               "a pin with no explicit icon location uses the app's default icon")) {
        return 1;
    }
    if (!Check(PinnedShortcutUsesDefaultAppIcon(exePath, exePath, 0, exePath),
               "the app's own icon at index 0 is still the default icon")) {
        return 1;
    }
    if (!Check(PinnedShortcutUsesDefaultAppIcon(exePath, L"\"" + exePath + L"\"", 0, exePath),
               "the shell may quote the icon location")) {
        return 1;
    }
    if (!Check(PinnedShortcutUsesDefaultAppIcon(L"c:/program files/harken search/harkensearch.exe",
                                                L"", 0, exePath),
               "path comparison ignores case and slash direction")) {
        return 1;
    }
    if (!Check(!PinnedShortcutUsesDefaultAppIcon(exePath, exePath, 3, exePath),
               "an explicit non-zero icon index is a chosen icon")) {
        return 1;
    }
    if (!Check(!PinnedShortcutUsesDefaultAppIcon(exePath, L"C:\\Icons\\custom.ico", 0, exePath),
               "a user-chosen icon file must be preserved")) {
        return 1;
    }
    if (!Check(!PinnedShortcutUsesDefaultAppIcon(L"C:\\Other\\Other.exe", L"", 0, exePath),
               "pins for another application must be ignored")) {
        return 1;
    }
    if (!Check(!PinnedShortcutUsesDefaultAppIcon(exePath, L"", 0, L""),
               "an unresolved executable path must never match")) {
        return 1;
    }
    if (!Check(!PinnedShortcutUsesDefaultAppIcon(L"", L"", 0, exePath),
               "a shortcut without a target must never match")) {
        return 1;
    }
    if (!Check(!PinnedShortcutUsesDefaultAppIcon(exePath + L"x", L"", 0, exePath),
               "a longer path sharing the same prefix is a different executable")) {
        return 1;
    }

    std::printf("OK\n");
    return 0;
}
