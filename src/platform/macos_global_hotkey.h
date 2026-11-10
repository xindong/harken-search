#pragma once

#ifdef __APPLE__
#include <cstdint>
#include "imgui.h"

struct KeyBinding;

namespace HarkenSearch::MacHotkey {
using ToggleHandler = void (*)();

// Carbon hot keys use physical virtual key codes. Unsupported ImGui keys return UINT32_MAX.
std::uint32_t KeyCode(ImGuiKey key) noexcept;
void SetToggleHandler(ToggleHandler handler) noexcept;
void SetRegistrationAllowed(bool allowed) noexcept;
bool RegistrationAllowed() noexcept;
bool Register(const KeyBinding& binding) noexcept;
void Unregister() noexcept;
bool IsRegistered() noexcept;
}
#endif
