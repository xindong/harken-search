#pragma once
// SoundManager - UI渲染模块
// UIRenderer.h - 主UI渲染、面板系统、文件列表
//
// 注意：大部分实现仍在 main.cpp 中，将在后续重构中逐步迁移

#include "imgui.h"
#include <string>
#include <vector>
#include <cstdint>

// Forward declarations
struct AppState;
struct AppSettings;
struct AudioRecord;
struct WaveformData;
struct FolderInfo;
enum class SortColumn;

enum class PlaybackAction {
    None = 0,
    Play,
    Pause
};

PlaybackAction GetPlaybackAction(
    const AppSettings& settings,
    bool wasPlaying,
    bool selectionChanged,
    bool isDoubleClick);

std::string FormatTime(float seconds, bool showMs = true);
float GetSmoothedPlaybackPosition(AppState& state);

// @stage7-contract
// API contract: renderer functions execute on the UI thread during the frame,
// consume caller-owned AppState/data references without retaining them, and do
// not own audio/database/bridge lifetimes.

// Main UI Rendering (implemented in main.cpp)

// Main UI rendering entry point
void RenderUI(AppState& state);

// Similar Sounds index controls; UI-thread only, reuses the app-owned batch task.
void DrawSimilarityIndexPanel(AppState& state);
void StartBatchFeatureExtraction(AppState& state, bool recheckAll = false);

// Drag one explicit result path using the main list's processing-aware transfer.
// UI-thread only; does not change the main selection or Similar Sounds reference.
void StartAudioFileDrag(AppState& state, const std::string& filePath);

// File Operations (implemented in main.cpp)

// Sort search results
void SortResults(std::vector<AudioRecord>& results, SortColumn column, bool ascending);

// Mark a file as played
// addToHistory=false: 仅标记高亮，不记录到播放历史（用于浏览器模式）
void MarkFilePlayed(AppState& state, const std::string& filePath, int64_t fileId = 0, bool addToHistory = true);

// Check if a file was played
bool IsFilePlayed(const AppState& state, const std::string& filePath);

// Check if a character is Chinese
bool IsChineseChar(wchar_t c);

// Waveform Rendering (implemented in main.cpp)

// Draw mini waveform (for list items)
void DrawMiniWaveform(const WaveformData& waveform, float playbackPos, float width, float height);

// Draw loudness meter (VU style)
void DrawLoudnessMeter(float peakL, float peakR, float rmsL, float rmsR, 
                       float peakHoldL, float peakHoldR,
                       float x, float y, float width, float height);

// Draw main waveform with spectrogram overlay
void DrawMainWaveform(const WaveformData& waveform, float playbackPos, float width, float height,
                            AppState& state, float inPoint, float outPoint);

// Reset waveform/spectrogram zoom
void ResetWaveformZoom(AppState& state);

// Hybrid mode: render main waveform/spectrogram/interaction in a dedicated bottom region
// (WebUI keeps list/filter/inspector/transport; waveform uses original ImGui logic)
void RenderHybridWaveformDock(AppState& state, float x, float y, float width, float height);

// Panel System (implemented in main.cpp)

// Draw a vertical splitter for resizing panels
bool VerticalSplitter(const char* id, float* panelWidth, float minWidth, float maxWidth, float height, bool invertDelta);

// Draw a horizontal splitter for resizing panels
bool HorizontalSplitter(const char* id, float* panelHeight, float minHeight, float maxHeight, float width);

// Pitch Control (implemented in main.cpp)

// Draw a horizontal metal scroll wheel for pitch control
bool PitchKnob(const char* label, float* value, float minVal, float maxVal, float defaultVal);

// Theme (implemented in main.cpp)

// Apply theme by enum
enum class AppTheme;
void ApplyTheme(AppTheme theme);

// Apply custom colors on top of theme
void ApplyCustomColors(const AppSettings& settings);
