#include "macos_global_hotkey.h"

#ifdef __APPLE__
// Carbon's FolderInfo conflicts with the shared database type.
#define FolderInfo MacFinderFolderInfo
#include <Carbon/Carbon.h>
#undef FolderInfo
#include "../AppState.h"
#include <array>
#include <cstdio>
#include <limits>

namespace HarkenSearch::MacHotkey {
namespace {
constexpr EventHotKeyID kShowManagerId{'HrkS', 1};
EventHotKeyRef hotkeyRef = nullptr;
EventHandlerRef eventHandler = nullptr;
ToggleHandler toggleHandler = nullptr;
bool registrationAllowed = true;
bool pressed = false;

OSStatus HandleHotkey(EventHandlerCallRef, EventRef event, void*) {
    EventHotKeyID received{};
    if (GetEventParameter(event, kEventParamDirectObject, typeEventHotKeyID,
            nullptr, sizeof(received), nullptr, &received) != noErr ||
        received.signature != kShowManagerId.signature || received.id != kShowManagerId.id)
        return eventNotHandledErr;

    if (GetEventKind(event) == kEventHotKeyReleased) {
        pressed = false;
    } else if (GetEventKind(event) == kEventHotKeyPressed && !pressed) {
        pressed = true;
        if (toggleHandler) toggleHandler();
    }
    return noErr;
}
}

std::uint32_t KeyCode(ImGuiKey key) noexcept {
    constexpr std::array<UInt32, 26> letters = {
        kVK_ANSI_A, kVK_ANSI_B, kVK_ANSI_C, kVK_ANSI_D, kVK_ANSI_E, kVK_ANSI_F,
        kVK_ANSI_G, kVK_ANSI_H, kVK_ANSI_I, kVK_ANSI_J, kVK_ANSI_K, kVK_ANSI_L,
        kVK_ANSI_M, kVK_ANSI_N, kVK_ANSI_O, kVK_ANSI_P, kVK_ANSI_Q, kVK_ANSI_R,
        kVK_ANSI_S, kVK_ANSI_T, kVK_ANSI_U, kVK_ANSI_V, kVK_ANSI_W, kVK_ANSI_X,
        kVK_ANSI_Y, kVK_ANSI_Z
    };
    constexpr std::array<UInt32, 10> digits = {
        kVK_ANSI_0, kVK_ANSI_1, kVK_ANSI_2, kVK_ANSI_3, kVK_ANSI_4,
        kVK_ANSI_5, kVK_ANSI_6, kVK_ANSI_7, kVK_ANSI_8, kVK_ANSI_9
    };
    constexpr std::array<UInt32, 20> functionKeys = {
        kVK_F1, kVK_F2, kVK_F3, kVK_F4, kVK_F5, kVK_F6, kVK_F7, kVK_F8,
        kVK_F9, kVK_F10, kVK_F11, kVK_F12, kVK_F13, kVK_F14, kVK_F15, kVK_F16,
        kVK_F17, kVK_F18, kVK_F19, kVK_F20
    };
    if (key >= ImGuiKey_A && key <= ImGuiKey_Z) return letters[key - ImGuiKey_A];
    if (key >= ImGuiKey_0 && key <= ImGuiKey_9) return digits[key - ImGuiKey_0];
    if (key >= ImGuiKey_F1 && key <= ImGuiKey_F20) return functionKeys[key - ImGuiKey_F1];
    switch (key) {
        case ImGuiKey_Space: return kVK_Space;
        case ImGuiKey_Enter: return kVK_Return;
        case ImGuiKey_Escape: return kVK_Escape;
        case ImGuiKey_Tab: return kVK_Tab;
        case ImGuiKey_Backspace: return kVK_Delete;
        case ImGuiKey_Delete: return kVK_ForwardDelete;
        case ImGuiKey_Home: return kVK_Home;
        case ImGuiKey_End: return kVK_End;
        case ImGuiKey_PageUp: return kVK_PageUp;
        case ImGuiKey_PageDown: return kVK_PageDown;
        case ImGuiKey_UpArrow: return kVK_UpArrow;
        case ImGuiKey_DownArrow: return kVK_DownArrow;
        case ImGuiKey_LeftArrow: return kVK_LeftArrow;
        case ImGuiKey_RightArrow: return kVK_RightArrow;
        default: return std::numeric_limits<std::uint32_t>::max();
    }
}

void SetToggleHandler(ToggleHandler handler) noexcept { toggleHandler = handler; }

void SetRegistrationAllowed(bool allowed) noexcept {
    registrationAllowed = allowed;
    if (!allowed) Unregister();
}

bool RegistrationAllowed() noexcept { return registrationAllowed; }

void Unregister() noexcept {
    if (hotkeyRef) { UnregisterEventHotKey(hotkeyRef); hotkeyRef = nullptr; }
    if (eventHandler) { RemoveEventHandler(eventHandler); eventHandler = nullptr; }
    pressed = false;
}

bool IsRegistered() noexcept { return hotkeyRef != nullptr; }

bool Register(const KeyBinding& binding) noexcept {
    Unregister();
    const auto code = KeyCode(binding.key);
    // A bare character key would intercept typing in every application.
    if (!registrationAllowed || !toggleHandler ||
        code == std::numeric_limits<std::uint32_t>::max() ||
        !(binding.ctrl || binding.alt || binding.shift)) return false;
    UInt32 modifiers = 0;
    if (binding.ctrl) modifiers |= controlKey;
    if (binding.alt) modifiers |= optionKey;
    if (binding.shift) modifiers |= shiftKey;
    const EventTypeSpec events[] = {
        {kEventClassKeyboard, kEventHotKeyPressed},
        {kEventClassKeyboard, kEventHotKeyReleased}
    };
    OSStatus status = InstallApplicationEventHandler(HandleHotkey, 2, events, nullptr, &eventHandler);
    if (status == noErr) {
        status = RegisterEventHotKey(code, modifiers, kShowManagerId, GetApplicationEventTarget(),
            kEventHotKeyExclusive, &hotkeyRef);
    }
    if (status != noErr) {
        std::fprintf(stderr, "Mac global hotkey registration failed: %d\n", static_cast<int>(status));
        Unregister();
        return false;
    }
    return true;
}
}

bool g_showManagerHotkeyRegisterOk = true;
void ApplyShowManagerGlobalHotkey(const AppSettings& settings) {
    if (!HarkenSearch::MacHotkey::RegistrationAllowed()) {
        HarkenSearch::MacHotkey::Unregister();
        g_showManagerHotkeyRegisterOk = true;
        return;
    }
    if (!settings.enableShowManagerHotkey || settings.showManagerHotkey.key == ImGuiKey_None) {
        HarkenSearch::MacHotkey::Unregister();
        g_showManagerHotkeyRegisterOk = true;
        return;
    }
    g_showManagerHotkeyRegisterOk = HarkenSearch::MacHotkey::Register(settings.showManagerHotkey);
}
#endif
