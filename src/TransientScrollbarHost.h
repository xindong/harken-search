#pragma once

#include "imgui.h"

// @stage7-contract
// API contract: borrowed UI-thread scrollbar host. The controller is process
// owned; callers must not retain region IDs across shutdown. Begin* helpers
// only wrap the current ImGui item and do not own windows.

namespace aura::ui::workspace {
class TransientScrollbarController;
}

extern aura::ui::workspace::TransientScrollbarController transientScrollbarController;
extern double transientScrollbarNow;

bool BeginTransientScrollbarChild(
    const char* strId,
    const ImVec2& size,
    ImGuiChildFlags childFlags = ImGuiChildFlags_None,
    ImGuiWindowFlags flags = 0);
bool BeginTransientScrollbarChild(
    ImGuiID id,
    const ImVec2& size,
    ImGuiChildFlags childFlags = ImGuiChildFlags_None,
    ImGuiWindowFlags flags = 0);
bool BeginTransientScrollbarTable(
    const char* strId,
    int columns,
    ImGuiTableFlags flags = 0,
    const ImVec2& outerSize = ImVec2(0.0f, 0.0f),
    float innerWidth = 0.0f);
bool BeginTransientScrollbarWindow(
    const char* name,
    bool* open = nullptr,
    ImGuiWindowFlags flags = 0);
bool BeginTransientScrollbarPopup(
    const char* strId,
    ImGuiWindowFlags flags = 0);
bool BeginTransientScrollbarPopupModal(
    const char* name,
    bool* open = nullptr,
    ImGuiWindowFlags flags = 0);
bool BeginTransientScrollbarCombo(
    const char* label,
    const char* previewValue,
    ImGuiComboFlags flags = 0);
