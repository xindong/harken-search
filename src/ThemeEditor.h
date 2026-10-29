#pragma once
// ThemeEditor.h - Independent theme editor module
// Extracted from ModuleUI.cpp. Owns editor lifecycle, undo, draft, save/discard.

struct AppState;

// @stage7-contract
// API contract: RenderThemeEditor is a UI-thread render pass that borrows the
// caller-owned AppState and active ImGui context; ResetThemeEditorSession
// clears editor-owned draft/undo state synchronously. Neither function
// transfers AppState ownership or retains transient caller references.
void RenderThemeEditor(AppState& state);
void ResetThemeEditorSession();

#include "BuildConfig.h"
#if AURA_BUILD_DIAGNOSTICS && ARIS_PREVIEW_ISOLATION
#include <string>
struct ImVec4;
// Isolated package only; exercises the same editor draft, undo and live preview.
bool ThemeScheduleEditorQaSetColor(const std::string& key, const ImVec4& value);
std::string ThemeScheduleEditorQaStatus();
#endif
