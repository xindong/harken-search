#pragma once

#include "BackgroundIoPolicy.h"
struct AppState;

// @stage7-contract
// Called only by the owning UI thread, after RenderUI has recorded visible
// rows and before GL draw data is rendered. Uses AppState's existing workers,
// generation/cancellation and cache budgets; no new worker or database owner.
// Hosts must flush deferred mini textures only AFTER drawing the current frame.
struct WaveformFrameOptions {
    // Windows frame-budget capture controls; normal hosts leave these false.
    bool holdWorker = false;
    bool ignorePlaybackForCapture = false;
    bool seedVisibleRequest = false;
};
BackgroundIoContext BuildAppBackgroundIoContext(AppState& state, bool contextMenuHeavyFreeze,
                                                bool coverWallPriorityMode, bool mouseIdle);
void PumpAppContinuousPlayback(AppState& state);
// Returns true only if this frame submitted the requested diagnostic seed.
bool PumpAppWaveforms(AppState& state, bool isMouseIdle, const WaveformFrameOptions& options = {});
void PumpAppCoverTextureUploads(AppState& state, bool contextMenuHeavyFreeze, bool isMouseIdle);
void PumpAppBackgroundStatus(AppState& state, bool isMouseIdle);
