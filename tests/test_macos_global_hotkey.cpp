#include "platform/macos_global_hotkey.h"
#define FolderInfo MacFinderFolderInfo
#include <Carbon/Carbon.h>
#undef FolderInfo
#include "AppState.h"
#include <cstdio>
#include <cstdlib>
#include <limits>

namespace {
int toggleCount = 0;
void Toggle() { ++toggleCount; }
void Check(bool ok, const char* description) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", description); std::exit(1); }
}
void Deliver(UInt32 kind) {
    EventRef event = nullptr;
    Check(CreateEvent(nullptr, kEventClassKeyboard, kind, GetCurrentEventTime(),
              kEventAttributeNone, &event) == noErr, "create event");
    const EventHotKeyID id{'HrkS', 1};
    Check(SetEventParameter(event, kEventParamDirectObject, typeEventHotKeyID,
              sizeof(id), &id) == noErr, "set hotkey id");
    Check(SendEventToEventTarget(event, GetApplicationEventTarget()) == noErr,
          "deliver hotkey event");
    ReleaseEvent(event);
}
}

int main() {
    using namespace HarkenSearch::MacHotkey;
    Check(KeyCode(ImGuiKey_M) == kVK_ANSI_M, "letter mapping");
    Check(KeyCode(ImGuiKey_0) == kVK_ANSI_0, "digit mapping");
    Check(KeyCode(ImGuiKey_F20) == kVK_F20, "function key mapping");
    Check(KeyCode(ImGuiKey_Delete) == kVK_ForwardDelete, "delete mapping");
    Check(KeyCode(ImGuiKey_F21) == std::numeric_limits<std::uint32_t>::max(),
          "unsupported key rejected");

    SetToggleHandler(Toggle);
    SetRegistrationAllowed(false);
    const KeyBinding chosen{ImGuiKey_F20, true, true, true};
    Check(!Register(chosen), "QA isolation does not reserve shortcut");
    SetRegistrationAllowed(true);
    const KeyBinding unmodified{ImGuiKey_M, false, false, false};
    Check(!Register(unmodified), "unmodified character rejected");
    Check(!IsRegistered(), "no registration after rejection");

    Check(Register(chosen), "register unique global shortcut");
    Check(IsRegistered(), "registered state");
    Deliver(kEventHotKeyPressed);
    Deliver(kEventHotKeyPressed);
    Check(toggleCount == 1, "held key toggles only once");
    Deliver(kEventHotKeyReleased);
    Deliver(kEventHotKeyPressed);
    Check(toggleCount == 2, "release arms next press");
    Unregister();
    Check(!IsRegistered(), "unregister releases hotkey");

    EventHotKeyRef occupied = nullptr;
    const EventHotKeyID competingId{'Test', 2};
    Check(RegisterEventHotKey(kVK_F20, controlKey | optionKey | shiftKey,
              competingId, GetApplicationEventTarget(), kEventHotKeyExclusive,
              &occupied) == noErr, "register competing hotkey");
    Check(!Register(chosen), "conflicting registration rejected");
    Check(!IsRegistered(), "conflict does not leave active hotkey");
    Check(UnregisterEventHotKey(occupied) == noErr, "release competing hotkey");
    SetToggleHandler(nullptr);
    std::puts("MacGlobalHotkey: mapping, registration, dispatch, repeat, conflict, cleanup PASS");
}
