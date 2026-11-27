#pragma once

struct AppState;

namespace aura::ui {

// @stage7-contract
// Internal UI-thread orchestration boundary. Functions borrow AppState for the
// duration of the call and retain no new reference or external lifetime.
void OpenSimilarSoundsWindow(AppState& state);
void InvalidateSimilarSoundsWindow(AppState& state);
void RenderSimilarSoundsContent(AppState& state, bool independentLayout = false, bool useShellFooter = false);
void RenderSimilarSoundsFooterActions(AppState& state);
void RenderSimilarSoundsWindow(AppState& state);

} // namespace aura::ui
