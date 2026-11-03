#pragma once

// @stage7-contract
// Fonts belong to the active ImGui atlas. Load only outside a frame; borrowed
// font pointers and the UI-thread scopes become invalid when the atlas dies.

#include "imgui.h"
// Font Loading

void LoadChineseFont(ImGuiIO& io, float chromePixelSize, float readingPixelSize, float listPixelSize);
ImFont* GetUIFontRegular();
ImFont* GetUIFontEmphasis();
ImFont* GetUIFontMono();
ImFont* GetUIListFont();
ImFont* GetUIMeterFont();  // F-018: 10px ASCII-only, for narrow loudness meter bars

// Reading-surface scope. While alive, GetUIFont*() return the reading-sized
// faces, and the reading-regular face is pushed so plain text scales too.
// Defaults are chrome; wrap only reading surfaces (tree, list, breadcrumb,
// Info panel, sidebar filters, settings, popups, menu dropdowns). A surface
// that is never wrapped renders at the pinned chrome size — there is no
// assert, so adding a new reading surface means remembering to wrap it.
struct ReadingTextScope {
    bool prev;
    ReadingTextScope();
    ~ReadingTextScope();
};

// Explicitly forces chrome faces inside an outer reading scope (e.g. an icon
// button embedded in a reading surface).
struct ChromeTextScope {
    bool prev;
    ChromeTextScope();
    ~ChromeTextScope();
};
