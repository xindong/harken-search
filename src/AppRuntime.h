#pragma once

// @stage7-contract
// The native host owns AppState and ImGui/GL. Initialize once, pump on the UI
// thread, join all owned workers before closing resources, then destroy GL.
// The stage logger is a synchronous non-null callback and must not throw.

#include "AppState.h"
using ShutdownStageLogger = void (*)(const char*);
// UI-thread orchestration shared by native hosts. State owns its workers;
// stop them before closing the database/audio and before destroying GL/ImGui.
bool InitializeAppSettingsAndAudio(AppState& state);
void PumpAppDatabaseAndAudio(AppState& state);
void PumpAppScanRefresh(AppState& state, bool scanPopupOpen);
void StopAppRuntimeWorkers(AppState& state, ShutdownStageLogger logStage);
void StopAppRelinkWorkers(AppState& state, ShutdownStageLogger logStage);
bool CloseAppRuntimeResources(AppState& state, ShutdownStageLogger logStage);

bool ResolveStartupDatabaseSelectionForMount(const AppState& state, std::string& path, AppState::StartupDatabaseSetupReason& reason);
