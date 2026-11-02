#pragma once

// @stage7-contract
// UI-thread services borrow AppState. Worker join functions block and must run
// before database/audio teardown. Settings paths are owned by the native host.

#include <cstdint>
#include <string>
#include <vector>
struct AppState;
struct AppSettings;
class AudioEngine;
// Functions defined in main.cpp, used by WindowsMain.cpp

// Settings
void LoadAppSettings(AppState& state);
void SaveAppSettings(const AppState& state);
bool TrySaveAppSettings(const AppState& state);
// 仅加载音频相关键，供引擎初始化前调用（缓冲周期、输出设备索引）
void LoadAudioSettingsEarly(AppState& state);
void PushRecentDbPath(AppState& state, const std::string& pathUtf8);
void MarkSettingsDirty(AppState& state);
void FlushSettingsDirty(AppState& state);

// F-033 Music Mode ↔ SFX Mode toggle is deferred to frame end because
// productMode drives many Music-only rendering branches (Module 2.5
// Key+BPM, Module 3.5 BPM/TAP, multi-channel waveform). Flipping the
// flag mid-frame desyncs SetCursorPos/ItemAdd pairing and trips the
// imgui.cpp:10789 parent-boundary assert. Call once per frame after
// RenderUI returns; defined in main.cpp.
void ProcessPendingProductModeToggle(AppState& state);

// Drag & drop export (defined in main.cpp)
void StartRegionDrag(AudioEngine& engine);

// Random playback (defined in main.cpp)
void PlayRandomFile(AppState& state);

// Search/filter (defined in main.cpp; primary declaration in SearchQuery.h)
void RefreshSearchResults(AppState& state, bool resetPagination);
void ForceCoverWallReload(AppState& state);
void StartCoverRebuild(AppState& state);
// 轮询并应用一次异步搜索结果；返回 true 表示消费了一条结果（供加载页等待首包）
bool PumpOneAsyncSearchResult(AppState& state);
void PlayRandomFile(AppState& state);
void LoadAllFilesFiltered(AppState& state, int limit = 1000);
void FilterResultsByFileType(AppState& state);

// Splash pre-upload: batch upload waveform textures to GPU (bypasses per-frame budget)
void PreUploadWaveformTextures(AppState& state, int count);

// 切库后完整状态清理（defined in main.cpp）
void ResetDatabaseState(AppState& state);
// 后台刷新文件夹树/播放列表（defined in main.cpp）
void StartDeferredSidebarBootstrap(AppState& state, bool reloadFolderTree, bool reloadPlaylists);
// 轮询异步切库结果，在每帧主循环开头调用
void PumpDatabaseSwitchIfPending(AppState& state);
// Join the owned asynchronous database-switch worker before tearing down
// shared database/runtime resources.
void WaitForDatabaseSwitchThread();
void ShutdownSelectedWaveformRebuild();
// 轮询异步随机池构建结果
void PumpInitRandomSamplingPoolIfPending(AppState& state);
// 加载完成后应用 Mini 波形预设跳转点并跳过开头无声（在 pumpLoadFileIfPending 为 true 时由主循环调用）
void OnAudioLoadPumped(AppState& state);
// 每帧可调：当有波形数据时根据阈值 seek 到首个有音位置（加载完成时波形常未就绪，故需每帧尝试直到有数据）
void MaybeSkipLeadingSilence(AppState& state);
// 开始异步切库（大库不卡主线程）；返回 true 表示已开始
bool BeginSwitchDatabase(AppState& state, const std::string& newDbPathUtf8);
// Drag & Drop (defined in main.cpp)
void StartFileDrag(const std::vector<std::string>& files);

// Utility (defined in main.cpp)
std::string GetExeDirUtf8();
std::wstring Utf8ToWide(const std::string& str);
std::string WideToUtf8(const std::wstring& wstr);
bool PathExistsUtf8(const std::string& pathUtf8);
std::string GetDefaultDbPathUtf8();
