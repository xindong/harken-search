// SoundManager - 应用状态模块
// AppState.cpp - 应用状态、设置、标签页结构实现

#include "AppState.h"
#include "Localization.h"   // L(id) used by shortcut preset names (knife-B explicit include)
#include "Utils.h"

#include <algorithm>
#include <cstring>
#include <ctime>
#include <unordered_map>

namespace {

template <size_t N>
void CopyStringToBuffer(char (&dst)[N], const std::string& src) {
    strncpy_s(dst, N, src.c_str(), _TRUNCATE);
}

bool HasActivePillFilterState(const std::map<std::string, int>& pillState) {
    for (const auto& [tag, state] : pillState) {
        (void)tag;
        if (state != 0) return true;
    }
    return false;
}

} // namespace

// Global Variables

AppState* g_appState = nullptr;

// Shortcut System Implementation

bool KeyBinding::isPressed() const {
    if (key == ImGuiKey_None) return false;
    bool keyPressed = ImGui::IsKeyPressed(key);
    bool modMatch = (ImGui::GetIO().KeyCtrl == ctrl) && 
                    (ImGui::GetIO().KeyShift == shift) &&
                    (ImGui::GetIO().KeyAlt == alt);
    return keyPressed && modMatch;
}

std::string KeyBinding::toString() const {
    if (key == ImGuiKey_None) return L(Sc_KeyUnset);
    std::string s;
    if (ctrl) s += "Ctrl+";
    if (shift) s += "Shift+";
    if (alt) s += "Alt+";
    const char* keyName = ImGui::GetKeyName(key);
    if (keyName && (strcmp(keyName, "Ctrl") == 0 || strcmp(keyName, "Shift") == 0 || strcmp(keyName, "Alt") == 0)) {
        if (!s.empty()) s.pop_back();
        return s;
    }
    if (keyName) s += keyName;
    return s;
}

// Global shortcut table (默认快捷键采用 Pro Tools 习惯)
ShortcutEntry g_shortcuts[] = {
    // Transport (Pro Tools 风格)
    { ShortcutAction::PlayPause,      LStr::Player_PlayPause,     LStr::ScDesc_PlayPause,       { ImGuiKey_Space, false, false, false } },
    { ShortcutAction::Stop,           LStr::Stop,         LStr::ScDesc_Stop,         { ImGuiKey_Period, false, false, false } },
    { ShortcutAction::Rewind,         LStr::Player_Rewind,         LStr::ScDesc_Rewind,                  { ImGuiKey_LeftArrow, false, false, false } },
    { ShortcutAction::FastForward,    LStr::Player_FastForward,         LStr::ScDesc_FastForward,                  { ImGuiKey_RightArrow, false, false, false } },
    { ShortcutAction::SkipToStart,    LStr::Player_SkipToStart,     LStr::ScDesc_SkipToStart,              { ImGuiKey_Enter, false, false, false } },
    { ShortcutAction::SkipToEnd,      LStr::Player_SkipToEnd,     LStr::ScDesc_SkipToEnd,        { ImGuiKey_Enter, true, false, false } },
    { ShortcutAction::PlayRegion,     LStr::Player_PlayRegion,     LStr::ScDesc_PlayRegion,     { ImGuiKey_Space, false, false, true } },
    { ShortcutAction::SetInPoint,     LStr::Player_SetIn,     LStr::ScDesc_SetInPoint,         { ImGuiKey_I, false, false, false } },
    { ShortcutAction::SetOutPoint,    LStr::Player_SetOut,     LStr::ScDesc_SetOutPoint,         { ImGuiKey_O, false, false, false } },
    { ShortcutAction::ClearInOut,     LStr::Player_ClearRegion,     LStr::ScDesc_ClearInOut,         { ImGuiKey_I, false, false, true } },
    
    // Navigation
    { ShortcutAction::NextFile,       LStr::Sc_NextFile,     LStr::ScDesc_NextFile,           { ImGuiKey_DownArrow, false, false, true } },
    { ShortcutAction::PrevFile,       LStr::Sc_PrevFile,     LStr::ScDesc_PrevFile,           { ImGuiKey_UpArrow, false, false, true } },
    { ShortcutAction::RandomFile,     LStr::Sc_RandomFile,     LStr::ScDesc_RandomFile,                    { ImGuiKey_RightArrow, false, true, false } },
    { ShortcutAction::PageDown,       LStr::Sc_PageDown,       LStr::ScDesc_PageDown,             { ImGuiKey_PageDown, false, false, false } },
    { ShortcutAction::PageUp,         LStr::Sc_PageUp,       LStr::ScDesc_PageUp,               { ImGuiKey_PageUp, false, false, false } },
    
    // Edit (Pro Tools 风格)
    { ShortcutAction::EditFilename,   LStr::Edit_EditFilename,   LStr::ScDesc_EditFilename,             { ImGuiKey_F2, false, false, false } },
    { ShortcutAction::Delete,         LStr::Delete,         LStr::ScDesc_Delete,                  { ImGuiKey_Delete, false, false, false } },
    { ShortcutAction::RevealInExplorer, LStr::Sc_RevealInExplorer, LStr::ScDesc_RevealInExplorer,   { ImGuiKey_R, true, false, false } },
    { ShortcutAction::BatchRename,    LStr::Batch_RenameTab,   LStr::ScDesc_BatchRename,    { ImGuiKey_R, true, true, false } },
    { ShortcutAction::SelectAll,      LStr::SelectAll,         LStr::ScDesc_SelectAll,             { ImGuiKey_A, true, false, false } },
    { ShortcutAction::ClearSelection, LStr::Sc_ClearSelection,     LStr::ScDesc_ClearSelection,                { ImGuiKey_Escape, false, false, false } },
    
    // View
    { ShortcutAction::Refresh,        LStr::Refresh,         LStr::ScDesc_Refresh,                 { ImGuiKey_F5, false, false, false } },
    { ShortcutAction::ToggleFavorite, LStr::Refresh,     LStr::ScDesc_Refresh,            { ImGuiKey_None, false, false, false } },
    
    // Audio channel and search shortcuts.
    { ShortcutAction::ToggleReverse, LStr::Sc_ToggleReverse, LStr::ScDesc_ToggleReverse, {ImGuiKey_R, false, true, false}, {} }, // Shift+R
    { ShortcutAction::ToggleMono, LStr::Sc_ToggleMono, LStr::ScDesc_ToggleMono, {ImGuiKey_M, false, true, false}, {} }, // Shift+M
    { ShortcutAction::ToggleSoloL, LStr::Sc_ToggleSoloL, LStr::ScDesc_ToggleSoloL, {ImGuiKey_L, false, true, false}, {} }, // Shift+L
    { ShortcutAction::ToggleSoloR, LStr::Sc_ToggleSoloR, LStr::ScDesc_ToggleSoloR, {ImGuiKey_R, true, false, true}, {} }, // Ctrl+Alt+R（Alt+Shift+R 在 Windows 下易被系统菜单占用导致无效）
    { ShortcutAction::FocusSearch, LStr::Sc_FocusSearch, LStr::ScDesc_FocusSearch, {ImGuiKey_F, true, false, false}, {} }, // Ctrl+F
    
    // Pitch (Pro Tools 风格: +/- 半音)
    { ShortcutAction::PitchUp,        LStr::Sc_PitchUp,     LStr::ScDesc_PitchUp,            { ImGuiKey_UpArrow, false, true, false } },
    { ShortcutAction::PitchDown,      LStr::Sc_PitchDown,     LStr::ScDesc_PitchDown,            { ImGuiKey_DownArrow, false, true, false } },
    { ShortcutAction::PitchReset,     LStr::Edit_ResetPitch,     LStr::ScDesc_PitchReset,            { ImGuiKey_0, false, true, false } },


    // Send to DAW
    { ShortcutAction::SendToDAW1,     LStr::Sc_SendToDAW1, LStr::ScDesc_SendToDAW1, { ImGuiKey_1, true, true, false } },
    { ShortcutAction::SendToDAW2,     LStr::Sc_SendToDAW2, LStr::ScDesc_SendToDAW2, { ImGuiKey_2, true, true, false } },
    { ShortcutAction::SendToDAW3,     LStr::Sc_SendToDAW3, LStr::ScDesc_SendToDAW3, { ImGuiKey_3, true, true, false } },
    { ShortcutAction::SendToDAW4,     LStr::Sc_SendToDAW4, LStr::ScDesc_SendToDAW4, { ImGuiKey_4, true, true, false } },
    // Spot to DAW (classic library-style)
    { ShortcutAction::SpotToDAW,      LStr::Sc_SpotToDAW, LStr::ScDesc_SpotToDAW, { ImGuiKey_S, false, false, false } },
    // Music mode BPM detection (F-033)
    { ShortcutAction::DetectBpm,      LStr::Sc_DetectBpm,       LStr::ScDesc_DetectBpm, { ImGuiKey_B, false, false, false } },
    { ShortcutAction::TapTempo,       LStr::Sc_TapTempo,      LStr::ScDesc_TapTempo,               { ImGuiKey_T, false, false, false } },
};

const int SHORTCUT_COUNT = sizeof(g_shortcuts) / sizeof(g_shortcuts[0]);

bool IsShortcutPressed(ShortcutAction action) {
    for (int i = 0; i < SHORTCUT_COUNT; i++) {
        if (g_shortcuts[i].action == action) {
            const KeyBinding& kb = (g_shortcuts[i].customKey.key != ImGuiKey_None) 
                                   ? g_shortcuts[i].customKey 
                                   : g_shortcuts[i].defaultKey;
            return kb.isPressed();
        }
    }
    return false;
}

std::string GetShortcutString(ShortcutAction action) {
    for (int i = 0; i < SHORTCUT_COUNT; i++) {
        if (g_shortcuts[i].action == action) {
            const KeyBinding& kb = (g_shortcuts[i].customKey.key != ImGuiKey_None) 
                                   ? g_shortcuts[i].customKey 
                                   : g_shortcuts[i].defaultKey;
            return kb.toString();
        }
    }
    return "";
}

// Shortcut Presets (F-027)

const char* GetShortcutPresetName(ShortcutPreset preset) {
    switch (preset) {
        case ShortcutPreset::ProTools:   return L(ShortcutPreset_ProTools);
        case ShortcutPreset::ClassicLibrary: return L(ShortcutPreset_ClassicLibrary);
        case ShortcutPreset::Minimal:    return L(ShortcutPreset_Minimal);
        default: return "?";
    }
}

void ApplyShortcutPreset(ShortcutPreset preset) {
    // Clear all custom bindings first
    for (int i = 0; i < SHORTCUT_COUNT; i++) {
        g_shortcuts[i].customKey = {ImGuiKey_None, false, false, false};
    }

    if (preset == ShortcutPreset::ProTools) {
        // Default bindings are already Pro Tools style — no custom overrides needed
        return;
    }

    // Helper to set custom binding by action
    auto setKey = [](ShortcutAction action, ImGuiKey key, bool ctrl = false, bool shift = false, bool alt = false) {
        for (int i = 0; i < SHORTCUT_COUNT; i++) {
            if (g_shortcuts[i].action == action) {
                g_shortcuts[i].customKey = {key, ctrl, shift, alt};
                return;
            }
        }
    };

    if (preset == ShortcutPreset::ClassicLibrary) {
        // Classic library bindings
        setKey(ShortcutAction::PlayPause,        ImGuiKey_Space);
        setKey(ShortcutAction::Stop,             ImGuiKey_Escape);
        setKey(ShortcutAction::Rewind,           ImGuiKey_LeftArrow);
        setKey(ShortcutAction::FastForward,       ImGuiKey_RightArrow);
        setKey(ShortcutAction::SkipToStart,      ImGuiKey_Home);
        setKey(ShortcutAction::SkipToEnd,        ImGuiKey_End);
        setKey(ShortcutAction::NextFile,         ImGuiKey_DownArrow, false, false, true);
        setKey(ShortcutAction::PrevFile,         ImGuiKey_UpArrow, false, false, true);
        setKey(ShortcutAction::SetInPoint,       ImGuiKey_I);
        setKey(ShortcutAction::SetOutPoint,      ImGuiKey_O);
        setKey(ShortcutAction::ClearInOut,       ImGuiKey_X, false, false, true); // Alt+X
        setKey(ShortcutAction::RevealInExplorer, ImGuiKey_R, true);  // Ctrl+R
        setKey(ShortcutAction::FocusSearch,      ImGuiKey_L, true);  // Ctrl+L (classic library layout)
        setKey(ShortcutAction::SendToDAW1,       ImGuiKey_1, true, true); // Ctrl+Shift+1
        setKey(ShortcutAction::SendToDAW2,       ImGuiKey_2, true, true);
        setKey(ShortcutAction::SendToDAW3,       ImGuiKey_3, true, true);
        setKey(ShortcutAction::SendToDAW4,       ImGuiKey_4, true, true);
        setKey(ShortcutAction::SpotToDAW,       ImGuiKey_S);        // bare S
    }
    else if (preset == ShortcutPreset::Minimal) {
        // Minimal: only essential keys, disable most shortcuts
        setKey(ShortcutAction::PlayPause,   ImGuiKey_Space);
        setKey(ShortcutAction::NextFile,    ImGuiKey_DownArrow, false, false, true);
        setKey(ShortcutAction::PrevFile,    ImGuiKey_UpArrow, false, false, true);
        setKey(ShortcutAction::FocusSearch, ImGuiKey_F, true);
        // All others keep default (which are still active but through defaultKey)
    }
}

// AppState Implementation

ListTab& AppState::currentTab() {
    if (listTabs.empty()) {
        ListTab tab; tab.id = nextTabId++; tab.name = u8"全部文件";
        tab.viewMode = ViewMode::Database;
        listTabs.push_back(tab);
    }
    if (activeTabIndex < 0 || activeTabIndex >= (int)listTabs.size()) activeTabIndex = 0;
    return listTabs[activeTabIndex];
}

int AppState::createNewTab(const std::string& name) {
    ListTab tab;
    tab.id = nextTabId++;
    tab.name = name.empty() ? (u8"标签 " + std::to_string(tab.id)) : name;
    tab.viewMode = ViewMode::Database;
    // 首个默认标签页不上色；后续新建按开关随机取色，并避开上一个标签的颜色
    if (settings.tabRandomColor && !listTabs.empty()) {
        unsigned seed = (unsigned)time(nullptr) ^ ((unsigned)tab.id * 2654435761u);
        int c = (int)(seed % 8u);
        if (c == listTabs.back().tabColor) c = (c + 1) % 8;
        tab.tabColor = c;
    }
    listTabs.push_back(tab);
    return (int)listTabs.size() - 1;
}

void AppState::closeTab(int index) {
    if (index >= 0 && index < (int)listTabs.size() && listTabs.size() > 1) {
        // 保存到"最近关闭"栈，支持 Ctrl+Z 恢复
        closedTabsStack.push_back(std::move(listTabs[index]));
        if ((int)closedTabsStack.size() > kMaxClosedTabs)
            closedTabsStack.pop_front();

        listTabs.erase(listTabs.begin() + index);
        if (activeTabIndex >= (int)listTabs.size()) activeTabIndex = (int)listTabs.size() - 1;
        else if (activeTabIndex > index) activeTabIndex--;
    }
}

int AppState::reopenClosedTab() {
    if (closedTabsStack.empty()) return -1;
    ListTab tab = std::move(closedTabsStack.back());
    closedTabsStack.pop_back();
    // 确保 id 不与现有 tab 冲突
    bool idConflict = false;
    for (const auto& t : listTabs) { if (t.id == tab.id) { idConflict = true; break; } }
    if (idConflict) tab.id = nextTabId++;
    listTabs.push_back(std::move(tab));
    return (int)listTabs.size() - 1;
}

int AppState::duplicateTab(int index) {
    if (index >= 0 && index < (int)listTabs.size()) {
        ListTab newTab = listTabs[index];
        newTab.id = nextTabId++;
        newTab.name = listTabs[index].name + u8" (副本)";

        // Active-tab results live in AppState rather than the tab snapshot.
        // Duplicating that tab requires one copy of the live result sets.
        if (index == activeTabIndex) {
            newTab.searchResults = searchResults;
            newTab.coverFacets = currentCoverFacets;
            newTab.pathFacets = currentPathFacets;
            newTab.searchResultsSnapshotValid = true;
            newTab.facetsSnapshotValid = true;
            newTab.searchResultsSyncGen = searchResultsGen_;
            newTab.facetsSyncGen = facetsGen_;
            newTab.resultsBuiltAtRevision = searchResultsBuiltAtRevision_;
        }

        listTabs.push_back(newTab);
        return (int)listTabs.size() - 1;
    }
    return -1;
}

// Tab Presets (F-033) — save/load/delete/rename of named tab workspaces

int AppState::saveTabPresetFromCurrent(const std::string& name) {
    if (name.empty()) return -1;
    if (listTabs.empty()) return -1;
    if ((int)tabPresets.size() >= kMaxTabPresets) return -1;

    // Ensure currentTab's live state is captured back before snapshot.
    syncToTab();

    TabPreset p;
    p.name      = name;
    p.activeIndex = activeTabIndex;
    p.createdAt = (int64_t)std::time(nullptr);
    p.pinned    = false;

    const int take = (int)listTabs.size() < kMaxTabsPerPreset
                         ? (int)listTabs.size() : kMaxTabsPerPreset;
    p.tabs.reserve((size_t)take);
    for (int i = 0; i < take; ++i) {
        const ListTab& t = listTabs[(size_t)i];
        TabPresetEntry e;
        e.name                    = t.name;
        e.viewMode                = (int)t.viewMode;
        e.browserScope            = t.browserScope;
        e.searchBuffer            = std::string(t.searchBuffer);
        e.selectedFolderPath      = t.selectedFolderPath;
        e.folderFilterActive      = t.folderFilterActive;
        e.coverFilterActive       = t.coverFilterActive;
        e.coverFilterPath         = t.coverFilterPath;
        e.coverFilterIndex        = t.coverFilterIndex;
        e.artistFilterText        = t.artistFilterText;
        e.artistFilterAlternates  = t.artistFilterAlternates;
        e.albumFilterText         = t.albumFilterText;
        e.categoryFilterText      = t.categoryFilterText;
        e.formatFilterIndex       = t.formatFilterIndex;
        e.channelFilterIndex      = t.channelFilterIndex;
        e.minDurationFilter       = t.minDurationFilter;
        e.maxDurationFilter       = t.maxDurationFilter;
        e.sampleRateFilterIndex   = t.sampleRateFilterIndex;
        e.minBpmFilter            = t.minBpmFilter;
        e.maxBpmFilter            = t.maxBpmFilter;
        e.keyFilterText           = t.keyFilterText;
        e.namingFilter            = t.namingFilter;
        e.activeSmartCollection   = static_cast<int>(t.activeSmartCollection);
        e.activeUserCollectionIdx = t.activeUserCollectionIdx;
        e.genrePillState          = t.genrePillState;
        e.instrumentPillState     = t.instrumentPillState;
        e.showWAV                 = t.showWAV;
        e.showMP3                 = t.showMP3;
        e.showFLAC                = t.showFLAC;
        e.showOGG                 = t.showOGG;
        e.showAIFF                = t.showAIFF;
        e.showM4A                 = t.showM4A;
        e.showWMA                 = t.showWMA;
        e.showOther               = t.showOther;
        e.findSameFilenameQuery   = t.findSameFilenameQuery;
        e.findSameFilenameActive  = t.findSameFilenameActive;
        e.selectedPlaylistId      = t.selectedPlaylistId;
        e.tabColor                = t.tabColor;
        p.tabs.push_back(std::move(e));
    }
    if (p.activeIndex < 0 || p.activeIndex >= (int)p.tabs.size())
        p.activeIndex = 0;

    // Upsert by name: if existing preset has same name, overwrite it.
    for (size_t i = 0; i < tabPresets.size(); ++i) {
        if (tabPresets[i].name == name) {
            const bool wasPinned = tabPresets[i].pinned;
            tabPresets[i] = std::move(p);
            tabPresets[i].pinned = wasPinned;
            return (int)i;
        }
    }

    tabPresets.push_back(std::move(p));
    return (int)tabPresets.size() - 1;
}

bool AppState::loadTabPreset(int presetIdx) {
    if (presetIdx < 0 || presetIdx >= (int)tabPresets.size()) return false;
    const TabPreset& p = tabPresets[(size_t)presetIdx];
    if (p.tabs.empty()) return false;

    // Archive current tabs into closedTabsStack so Ctrl+Z can restore individuals.
    for (ListTab& t : listTabs) {
        closedTabsStack.push_back(std::move(t));
        if ((int)closedTabsStack.size() > kMaxClosedTabs)
            closedTabsStack.pop_front();
    }
    listTabs.clear();

    // Rebuild tabs from preset entries.
    listTabs.reserve(p.tabs.size());
    for (const TabPresetEntry& e : p.tabs) {
        ListTab t;
        t.id                      = nextTabId++;
        t.name                    = e.name;
        t.viewMode                = MapLegacyViewMode(e.viewMode);
        t.browserScope            = e.browserScope;
        strncpy_s(t.searchBuffer, sizeof(t.searchBuffer), e.searchBuffer.c_str(), _TRUNCATE);
        t.selectedFolderPath      = e.selectedFolderPath;
        t.folderFilterActive      = e.folderFilterActive;
        t.coverFilterActive       = e.coverFilterActive;
        t.coverFilterPath         = e.coverFilterPath;
        t.coverFilterIndex        = e.coverFilterIndex;
        t.artistFilterText        = e.artistFilterText;
        t.artistFilterAlternates  = e.artistFilterAlternates;
        t.albumFilterText         = e.albumFilterText;
        t.categoryFilterText      = e.categoryFilterText;
        t.formatFilterIndex       = e.formatFilterIndex;
        t.channelFilterIndex      = e.channelFilterIndex;
        t.minDurationFilter       = e.minDurationFilter;
        t.maxDurationFilter       = e.maxDurationFilter;
        t.sampleRateFilterIndex   = e.sampleRateFilterIndex;
        t.minBpmFilter            = e.minBpmFilter;
        t.maxBpmFilter            = e.maxBpmFilter;
        t.keyFilterText           = e.keyFilterText;
        t.namingFilter            = e.namingFilter;
        t.activeSmartCollection   = static_cast<SmartCollection>(e.activeSmartCollection);
        t.activeUserCollectionIdx = e.activeUserCollectionIdx;
        t.genrePillState          = e.genrePillState;
        t.instrumentPillState     = e.instrumentPillState;
        t.showWAV                 = e.showWAV;
        t.showMP3                 = e.showMP3;
        t.showFLAC                = e.showFLAC;
        t.showOGG                 = e.showOGG;
        t.showAIFF                = e.showAIFF;
        t.showM4A                 = e.showM4A;
        t.showWMA                 = e.showWMA;
        t.showOther               = e.showOther;
        t.findSameFilenameQuery   = e.findSameFilenameQuery;
        t.findSameFilenameActive  = e.findSameFilenameActive;
        t.selectedPlaylistId      = e.selectedPlaylistId;
        t.tabColor                = e.tabColor;
        // Force searchResults to be rebuilt by the async query pipeline.
        t.dataUnloaded            = true;
        listTabs.push_back(std::move(t));
    }

    activeTabIndex = p.activeIndex;
    if (activeTabIndex < 0 || activeTabIndex >= (int)listTabs.size())
        activeTabIndex = 0;

    // Flush any live AppState data that referenced the old tabs.
    searchResults.clear();
    currentCoverFacets.clear();
    currentPathFacets.clear();

    syncFromTab();                    // pulls scalar state out of new active tab
    needsTabResultsRefresh = true;    // main loop reruns query next frame
    return true;
}

bool AppState::deleteTabPreset(int presetIdx) {
    if (presetIdx < 0 || presetIdx >= (int)tabPresets.size()) return false;
    tabPresets.erase(tabPresets.begin() + presetIdx);
    return true;
}

bool AppState::renameTabPreset(int presetIdx, const std::string& newName) {
    if (presetIdx < 0 || presetIdx >= (int)tabPresets.size()) return false;
    if (newName.empty()) return false;
    tabPresets[(size_t)presetIdx].name = newName;
    return true;
}

static bool HasMeaningfulSavedSearchState(const ViewStateSnapshot& snap) {
    return !snap.searchText.empty() ||
           (snap.folderFilterActive && !snap.selectedFolderPath.empty()) ||
           (snap.coverFilterActive && !snap.coverFilterPath.empty()) ||
           snap.coverFilterIndex != 0 ||
           snap.selectedPlaylistId >= 0 ||
           snap.findSameFilenameActive ||
           !snap.findSameFilenameQuery.empty();
}

int AppState::saveSavedSearchFromCurrent(const std::string& name) {
    if (name.empty()) return -1;

    ViewStateSnapshot snap = captureViewState();
    if (!HasMeaningfulSavedSearchState(snap)) return -1;

    const int64_t now = (int64_t)std::time(nullptr);
    for (size_t i = 0; i < savedSearches.size(); ++i) {
        if (savedSearches[i].name == name) {
            const bool wasPinned = savedSearches[i].pinned;
            const int64_t createdAt = savedSearches[i].createdAt > 0
                ? savedSearches[i].createdAt
                : now;
            const int64_t lastUsed = savedSearches[i].lastUsed;
            savedSearches[i] = SavedSearchEntry{name, std::move(snap), createdAt, lastUsed, wasPinned};
            return (int)i;
        }
    }

    if ((int)savedSearches.size() >= kMaxSavedSearches) return -1;
    savedSearches.push_back(SavedSearchEntry{name, std::move(snap), now, 0, false});
    return (int)savedSearches.size() - 1;
}

bool AppState::applySavedSearch(int savedSearchIdx) {
    if (savedSearchIdx < 0 || savedSearchIdx >= (int)savedSearches.size()) return false;
    SavedSearchEntry& entry = savedSearches[(size_t)savedSearchIdx];
    restoreViewState(entry.snapshot);
    entry.lastUsed = (int64_t)std::time(nullptr);
    needsTabResultsRefresh = true;
    return true;
}

bool AppState::deleteSavedSearch(int savedSearchIdx) {
    if (savedSearchIdx < 0 || savedSearchIdx >= (int)savedSearches.size()) return false;
    savedSearches.erase(savedSearches.begin() + savedSearchIdx);
    return true;
}

bool AppState::renameSavedSearch(int savedSearchIdx, const std::string& newName) {
    if (savedSearchIdx < 0 || savedSearchIdx >= (int)savedSearches.size()) return false;
    if (newName.empty()) return false;
    savedSearches[(size_t)savedSearchIdx].name = newName;
    return true;
}

std::vector<int> AppState::getSavedSearchDisplayOrder() const {
    std::vector<int> order;
    order.reserve(savedSearches.size());
    for (int i = 0; i < (int)savedSearches.size(); ++i) {
        order.push_back(i);
    }
    std::sort(order.begin(), order.end(), [&](int a, int b) {
        const SavedSearchEntry& A = savedSearches[(size_t)a];
        const SavedSearchEntry& B = savedSearches[(size_t)b];
        if (A.pinned != B.pinned) return A.pinned > B.pinned;
        const int64_t activityA = A.lastUsed > 0 ? A.lastUsed : A.createdAt;
        const int64_t activityB = B.lastUsed > 0 ? B.lastUsed : B.createdAt;
        if (activityA != activityB) return activityA > activityB;
        return a < b;
    });
    return order;
}

void AppState::syncFromTab() {
    if (listTabs.empty()) return;
    ListTab& tab = currentTab();
    if (tab.dataUnloaded) {
        tab.dataUnloaded = false;
        needsTabResultsRefresh = true;  // main 层在下一帧调用 RefreshSearchResults
    }
    // Refresh unlocked tabs whose snapshots predate the database revision.
    // Successful result delivery records the revision at the apply site.
    // Locked tabs retain their pinned snapshot.

    const uint64_t currentRev = database.dataRevision();
    if (!tab.isLocked && tab.resultsBuiltAtRevision < currentRev) {
        needsTabResultsRefresh = true;
    }
    // Activating a tab restores the revision that produced its live results.
    // Keeping the previous tab revision would make the next sync stale.

    searchResultsBuiltAtRevision_ = tab.resultsBuiltAtRevision;

    // Transfer active-tab result ownership without copying.
    // The tab snapshot is invalidated after each move.
    if (tab.searchResultsSnapshotValid) {
        searchResults = std::move(tab.searchResults);
        searchResultsGen_ = tab.searchResultsSyncGen;
        tab.searchResultsSnapshotValid = false;
    }
    if (tab.facetsSnapshotValid) {
        currentCoverFacets = std::move(tab.coverFacets);
        currentPathFacets = std::move(tab.pathFacets);
        facetsGen_ = tab.facetsSyncGen;
        tab.facetsSnapshotValid = false;
    }

// 轻量级标量 / 小字符串：始终同步（开销 < 1μs）
    selectedResultIndex = tab.selectedResultIndex;
    viewMode = tab.viewMode;
    if (tab.viewMode == ViewMode::Browser) {
        fileBrowser.setScope(static_cast<BrowserScope>(tab.browserScope));
    }
    selectedPlaylistId = tab.selectedPlaylistId;
    playlistAggregatedView = tab.playlistAggregatedView;
    selectedIndicesSet = tab.selectedIndicesSet;
    allSelected = tab.allSelected;
    lastClickedIndex = tab.lastClickedIndex;
    findSameFilenameQuery = tab.findSameFilenameQuery;
    findSameFilenameActive = tab.findSameFilenameActive;
    folderFilterActive = tab.folderFilterActive;
    selectedFolderPath = tab.selectedFolderPath;
    coverFilterActive = tab.coverFilterActive;
    coverFilterPath = tab.coverFilterPath;
    coverFilterIndex = tab.coverFilterIndex;
    CopyStringToBuffer(artistFilterBuffer, tab.artistFilterText);
    artistFilterAlternates = tab.artistFilterAlternates;
    CopyStringToBuffer(albumFilterBuffer, tab.albumFilterText);
    CopyStringToBuffer(categoryFilterBuffer, tab.categoryFilterText);
    formatFilterIndex = tab.formatFilterIndex;
    channelFilterIndex = tab.channelFilterIndex;
    minDurationFilter = tab.minDurationFilter;
    maxDurationFilter = tab.maxDurationFilter;
    sampleRateFilterIndex = tab.sampleRateFilterIndex;
    minBpmFilter = tab.minBpmFilter;
    maxBpmFilter = tab.maxBpmFilter;
    CopyStringToBuffer(keyFilterBuffer, tab.keyFilterText);
    namingFilter = tab.namingFilter;
    activeSmartCollection = tab.activeSmartCollection;
    activeUserCollectionIdx = tab.activeUserCollectionIdx;
    genrePillState = tab.genrePillState;
    instrumentPillState = tab.instrumentPillState;
    showWAV = tab.showWAV;
    showMP3 = tab.showMP3;
    showFLAC = tab.showFLAC;
    showOGG = tab.showOGG;
    showAIFF = tab.showAIFF;
    showM4A = tab.showM4A;
    showWMA = tab.showWMA;
    showOther = tab.showOther;
    if (!coverFilterActive) {
        coverFilterPath.clear();
        coverFilterMatchSig = 0;
        coverFilterMatchPaths.clear();
        coverWall.pinnedCoverFilterItem = CoverModule::CoverItem{};
    }
    totalFileCount = tab.totalFileCount;
    loadedCount = tab.loadedCount;
    hasMoreResults = tab.hasMoreResults;
    memcpy(searchBuffer, tab.searchBuffer, sizeof(searchBuffer));
}

void AppState::syncToTab() {
    if (listTabs.empty()) return;
    ListTab& tab = currentTab();
    if (tab.isLocked) return;

    // The active tab does not retain a duplicate of the live result sets.

    tab.searchResultsSyncGen = searchResultsGen_;
    tab.facetsSyncGen = facetsGen_;
    // Store the revision used to build the live results.
    // A later database write must still make this tab stale on reactivation.

    tab.resultsBuiltAtRevision = searchResultsBuiltAtRevision_;

// 轻量级标量 / 小字符串：始终同步（开销 < 1μs）
    tab.selectedResultIndex = selectedResultIndex;
    tab.viewMode = viewMode;
    tab.browserScope = (int)fileBrowser.getScope();
    tab.selectedPlaylistId = selectedPlaylistId;
    tab.playlistAggregatedView = playlistAggregatedView;
    tab.selectedIndicesSet = selectedIndicesSet;
    tab.allSelected = allSelected;
    tab.lastClickedIndex = lastClickedIndex;
    tab.findSameFilenameQuery = findSameFilenameQuery;
    tab.findSameFilenameActive = findSameFilenameActive;
    tab.folderFilterActive = folderFilterActive;
    tab.selectedFolderPath = selectedFolderPath;
    tab.coverFilterActive = coverFilterActive;
    tab.coverFilterPath = coverFilterActive ? coverFilterPath : std::string();
    tab.coverFilterIndex = coverFilterIndex;
    tab.artistFilterText = artistFilterBuffer;
    tab.artistFilterAlternates = artistFilterAlternates;
    tab.albumFilterText = albumFilterBuffer;
    tab.categoryFilterText = categoryFilterBuffer;
    tab.formatFilterIndex = formatFilterIndex;
    tab.channelFilterIndex = channelFilterIndex;
    tab.minDurationFilter = minDurationFilter;
    tab.maxDurationFilter = maxDurationFilter;
    tab.sampleRateFilterIndex = sampleRateFilterIndex;
    tab.minBpmFilter = minBpmFilter;
    tab.maxBpmFilter = maxBpmFilter;
    tab.keyFilterText = keyFilterBuffer;
    tab.namingFilter = namingFilter;
    tab.activeSmartCollection = activeSmartCollection;
    tab.activeUserCollectionIdx = activeUserCollectionIdx;
    tab.genrePillState = genrePillState;
    tab.instrumentPillState = instrumentPillState;
    tab.showWAV = showWAV;
    tab.showMP3 = showMP3;
    tab.showFLAC = showFLAC;
    tab.showOGG = showOGG;
    tab.showAIFF = showAIFF;
    tab.showM4A = showM4A;
    tab.showWMA = showWMA;
    tab.showOther = showOther;
    tab.totalFileCount = totalFileCount;
    tab.loadedCount = loadedCount;
    tab.hasMoreResults = hasMoreResults;
    memcpy(tab.searchBuffer, searchBuffer, sizeof(searchBuffer));
}

void AppState::parkLiveResultsInCurrentTab() {
    if (listTabs.empty()) return;
    ListTab& tab = currentTab();

    tab.searchResults = std::move(searchResults);
    tab.coverFacets = std::move(currentCoverFacets);
    tab.pathFacets = std::move(currentPathFacets);
    tab.searchResultsSnapshotValid = true;
    tab.facetsSnapshotValid = true;
    tab.searchResultsSyncGen = searchResultsGen_;
    tab.facetsSyncGen = facetsGen_;
    tab.resultsBuiltAtRevision = searchResultsBuiltAtRevision_;
}

static bool AppStateHasActiveDbFilterForCoverWall(const AppState& state) {
    if (state.viewMode != ViewMode::Database) return false;
    if (state.searchBuffer[0] != '\0') return true;
    if (state.folderFilterActive && !state.selectedFolderPath.empty()) return true;
    if (state.coverFilterActive && !state.coverFilterPath.empty()) return true;
    if (state.artistFilterBuffer[0] != '\0') return true;
    if (!state.artistFilterAlternates.empty()) return true;
    if (state.albumFilterBuffer[0] != '\0') return true;
    if (state.categoryFilterBuffer[0] != '\0') return true;
    if (state.formatFilterIndex != 0) return true;
    if (state.channelFilterIndex != 0) return true;
    if (state.sampleRateFilterIndex != 0) return true;
    if (state.minDurationFilter > 0.0f) return true;
    if (state.maxDurationFilter > 0.0f) return true;
    if (state.namingFilter != 0) return true;
    if (state.coverFilterIndex != 0) return true;
    if (state.minBpmFilter > 0.0f) return true;
    if (state.maxBpmFilter > 0.0f) return true;
    if (state.keyFilterBuffer[0] != '\0') return true;
    if (state.activeSmartCollection != SmartCollection::None) return true;
    if (state.activeUserCollectionIdx >= 0) return true;
    if (HasActivePillFilterState(state.genrePillState)) return true;
    if (HasActivePillFilterState(state.instrumentPillState)) return true;
    if (!(state.showWAV && state.showMP3 && state.showFLAC && state.showOGG &&
          state.showAIFF && state.showM4A && state.showWMA && state.showOther)) return true;
    return false;
}

static bool ListTabHasActiveDbFilterForCoverWall(const ListTab& tab) {
    if (tab.viewMode != ViewMode::Database) return false;
    if (tab.searchBuffer[0] != '\0') return true;
    if (tab.folderFilterActive && !tab.selectedFolderPath.empty()) return true;
    if (tab.coverFilterActive && !tab.coverFilterPath.empty()) return true;
    if (!tab.artistFilterText.empty()) return true;
    if (!tab.artistFilterAlternates.empty()) return true;
    if (!tab.albumFilterText.empty()) return true;
    if (!tab.categoryFilterText.empty()) return true;
    if (tab.formatFilterIndex != 0) return true;
    if (tab.channelFilterIndex != 0) return true;
    if (tab.sampleRateFilterIndex != 0) return true;
    if (tab.minDurationFilter > 0.0f) return true;
    if (tab.maxDurationFilter > 0.0f) return true;
    if (tab.namingFilter != 0) return true;
    if (tab.coverFilterIndex != 0) return true;
    if (tab.minBpmFilter > 0.0f) return true;
    if (tab.maxBpmFilter > 0.0f) return true;
    if (!tab.keyFilterText.empty()) return true;
    if (tab.activeSmartCollection != SmartCollection::None) return true;
    if (tab.activeUserCollectionIdx >= 0) return true;
    if (HasActivePillFilterState(tab.genrePillState)) return true;
    if (HasActivePillFilterState(tab.instrumentPillState)) return true;
    if (!(tab.showWAV && tab.showMP3 && tab.showFLAC && tab.showOGG &&
          tab.showAIFF && tab.showM4A && tab.showWMA && tab.showOther)) return true;
    return false;
}

static bool ListTabIsFullLibraryCoverWallCandidate(const ListTab& tab) {
    if (tab.name != u8"全部文件") return false;
    if (tab.viewMode != ViewMode::Database) return false;
    if (ListTabHasActiveDbFilterForCoverWall(tab)) return false;
    if (tab.selectedPlaylistId >= 0 || tab.playlistAggregatedView) return false;
    if (tab.findSameFilenameActive || !tab.findSameFilenameQuery.empty()) return false;
    if (tab.selectedIndicesSet.size() > 1) return false;
    return true;
}

static bool ShouldCarryLiveFullLibraryCoverWallSession(
    const AppState& state,
    const ListTab& targetTab,
    const CoverWallState& liveCoverWall,
    const CoverWallTabSnapshot& snapshot,
    const std::string& currentDbPath) {
    if (!snapshot.valid) return false;
    if (snapshot.generation != liveCoverWall.generation) return false;
    if (snapshot.dbPath != currentDbPath) return false;
    if (snapshot.source != static_cast<int>(CoverWallSource::Global)) return false;
    if (liveCoverWall.source != CoverWallSource::Global) return false;
    if (!ListTabIsFullLibraryCoverWallCandidate(targetTab)) return false;
    if (AppStateHasActiveDbFilterForCoverWall(state)) return false;
    return true;
}

struct LiveFullLibraryCoverWallSession {
    bool loaded = false;
    bool discoveryDone = false;
    int64_t lastTotalFileCount = 0;
    int64_t loadTotal = 0;
    bool globalItemsCacheValid = false;
    std::string globalItemsCacheDbPath;
    int64_t globalItemsCacheTotalFileCount = 0;
    uint64_t globalItemsCacheGeneration = 0;
    size_t lastDisplaySnapshotCursor = 0;
    size_t preloadCursor = 0;
    std::string preloadDbPath;
    std::string globalPageNextCursor;
    bool globalPageHasMore = false;
    bool globalPageLoadingNext = false;
    std::string globalPageDbPath;
    size_t preRequestCursor = 0;
    int lastPreRequestTotalItems = -1;
    size_t lastFirstScreenStart = (size_t)-1;
    size_t lastFirstScreenEnd = 0;
    int lastTotalItems = -1;
    double warmupUntil = 0.0;
    std::vector<CoverModule::CoverItem> items;
    std::vector<CoverModule::CoverItem> globalItemsCache;
    std::vector<CoverModule::CoverItem> displaySnapshot;
    std::vector<CoverModule::CoverItem> preloadFullList;
    std::vector<CoverModule::CoverItem> preloadFirstPage;
    std::unordered_set<std::string> discoveryKnown;
    std::unordered_map<std::string, CoverWallTextureMirrorEntry> texCache;
    std::unordered_map<std::string, CoverWallFailedTextureEntry> failedTexCache;
};

static LiveFullLibraryCoverWallSession CaptureLiveFullLibraryCoverWallSession(const CoverWallState& cw) {
    LiveFullLibraryCoverWallSession session;
    session.loaded = cw.loaded;
    session.discoveryDone = cw.discoveryDone;
    session.lastTotalFileCount = cw.lastTotalFileCount;
    session.loadTotal = cw.loadTotal;
    session.globalItemsCacheValid = cw.globalItemsCacheValid;
    session.globalItemsCacheDbPath = cw.globalItemsCacheDbPath;
    session.globalItemsCacheTotalFileCount = cw.globalItemsCacheTotalFileCount;
    session.globalItemsCacheGeneration = cw.globalItemsCacheGeneration;
    session.lastDisplaySnapshotCursor = cw.lastDisplaySnapshotCursor;
    session.preloadCursor = cw.preloadCursor;
    session.preloadDbPath = cw.preloadDbPath;
    session.globalPageNextCursor = cw.globalPageNextCursor;
    session.globalPageHasMore = cw.globalPageHasMore;
    session.globalPageLoadingNext = cw.globalPageLoadingNext;
    session.globalPageDbPath = cw.globalPageDbPath;
    session.preRequestCursor = cw.preRequestCursor;
    session.lastPreRequestTotalItems = cw.lastPreRequestTotalItems;
    session.lastFirstScreenStart = cw.lastFirstScreenStart;
    session.lastFirstScreenEnd = cw.lastFirstScreenEnd;
    session.lastTotalItems = cw.lastTotalItems;
    session.warmupUntil = cw.warmupUntil;
    session.items = cw.items;
    session.globalItemsCache = cw.globalItemsCache;
    session.displaySnapshot = cw.displaySnapshot;
    session.preloadFullList = cw.preloadFullList;
    session.preloadFirstPage = cw.preloadFirstPage;
    session.discoveryKnown = cw.discoveryKnown;
    session.texCache = cw.texCache;
    session.failedTexCache = cw.failedTexCache;
    return session;
}

static void ApplyLiveFullLibraryCoverWallSession(
    CoverWallState& cw,
    const LiveFullLibraryCoverWallSession& session,
    uint64_t liveNonCoverTextureEpoch) {
    cw.items = session.items;
    cw.pendingFacetItems.clear();
    cw.filteredItems.clear();
    cw.fallbackItems.clear();
    cw.globalItemsCache = session.globalItemsCache;
    cw.displaySnapshot = session.displaySnapshot;
    cw.loaded = session.loaded;
    cw.loading = false;
    cw.entriesInFlight = false;
    cw.foldersInFlight = false;
    cw.preloadInFlight = false;
    cw.discoveryDone = session.discoveryDone;
    cw.lastTotalFileCount = session.lastTotalFileCount;
    cw.loadTotal = session.loadTotal;
    cw.globalItemsCacheValid = session.globalItemsCacheValid;
    cw.globalItemsCacheDbPath = session.globalItemsCacheDbPath;
    cw.globalItemsCacheTotalFileCount = session.globalItemsCacheTotalFileCount;
    cw.globalItemsCacheGeneration = session.globalItemsCacheGeneration;
    cw.lastDisplaySnapshotCursor = session.lastDisplaySnapshotCursor;
    cw.preloadFullList = session.preloadFullList;
    cw.preloadFirstPage = session.preloadFirstPage;
    cw.preloadDbPath = session.preloadDbPath;
    cw.preloadCursor = session.preloadCursor;
    cw.globalPageNextCursor = session.globalPageNextCursor;
    cw.globalPageHasMore = session.globalPageHasMore;
    cw.globalPageLoadingNext = false;
    cw.globalPageDbPath = session.globalPageDbPath;
    cw.discoveryKnown = session.discoveryKnown;
    cw.preRequestCursor = session.preRequestCursor;
    cw.lastPreRequestTotalItems = session.lastPreRequestTotalItems;
    cw.lastFirstScreenStart = session.lastFirstScreenStart;
    cw.lastFirstScreenEnd = session.lastFirstScreenEnd;
    cw.lastTotalItems = session.lastTotalItems;
    cw.warmupUntil = session.warmupUntil;
    cw.texCache.clear();
    for (const auto& entry : session.texCache) {
        if (entry.second.nonCoverTextureEpoch == liveNonCoverTextureEpoch) {
            cw.texCache[entry.first] = entry.second;
        }
    }
    cw.failedTexCache = session.failedTexCache;
}

void AppState::saveCoverWallDisplaySnapshotForTab(int tabIndex) {
    if (tabIndex < 0 || tabIndex >= (int)listTabs.size()) return;

    CoverWallTabSnapshot& snapshot = listTabs[(size_t)tabIndex].coverWallSnapshot;
    const CoverWallState& cw = coverWall;
    snapshot.valid = true;
    snapshot.generation = cw.generation;
    snapshot.dbPath = database.getMainDbPath();
    snapshot.source = static_cast<int>(cw.source);
    snapshot.loaded = cw.loaded;
    snapshot.discoveryDone = cw.discoveryDone;
    snapshot.lastTotalFileCount = cw.lastTotalFileCount;
    snapshot.loadTotal = cw.loadTotal;
    snapshot.globalItemsCacheValid = cw.globalItemsCacheValid;
    snapshot.globalItemsCacheDbPath = cw.globalItemsCacheDbPath;
    snapshot.globalItemsCacheTotalFileCount = cw.globalItemsCacheTotalFileCount;
    snapshot.globalItemsCacheGeneration = cw.globalItemsCacheGeneration;
    snapshot.prevUseFacetMode = cw.prevUseFacetMode;
    snapshot.lastFacetGenSnapshot = cw.lastFacetGenSnapshot;
    snapshot.lastPreviewSearchGen = cw.lastPreviewSearchGen;
    snapshot.lastPreviewFacetGen = cw.lastPreviewFacetGen;
    snapshot.lastPreviewHadVisibleItems = cw.lastPreviewHadVisibleItems;
    snapshot.pendingFacetCursor = cw.pendingFacetCursor;
    snapshot.fallbackResultsGen = cw.fallbackResultsGen;
    snapshot.fallbackFacetsGen = cw.fallbackFacetsGen;
    snapshot.lastDisplaySnapshotCursor = cw.lastDisplaySnapshotCursor;
    snapshot.preloadCursor = cw.preloadCursor;
    snapshot.preloadDbPath = cw.preloadDbPath;
    snapshot.globalPageNextCursor = cw.globalPageNextCursor;
    snapshot.globalPageHasMore = cw.globalPageHasMore;
    snapshot.globalPageLoadingNext = cw.globalPageLoadingNext;
    snapshot.globalPageDbPath = cw.globalPageDbPath;
    snapshot.preRequestCursor = cw.preRequestCursor;
    snapshot.lastPreRequestTotalItems = cw.lastPreRequestTotalItems;
    snapshot.lastFirstScreenStart = cw.lastFirstScreenStart;
    snapshot.lastFirstScreenEnd = cw.lastFirstScreenEnd;
    snapshot.lastTotalItems = cw.lastTotalItems;
    snapshot.warmupUntil = cw.warmupUntil;
    snapshot.nonCoverTextureEpoch = cw.nonCoverTextureEpoch;
    snapshot.items = cw.items;
    snapshot.pendingFacetItems = cw.pendingFacetItems;
    snapshot.filteredItems = cw.filteredItems;
    snapshot.fallbackItems = cw.fallbackItems;
    snapshot.globalItemsCache = cw.globalItemsCache;
    snapshot.displaySnapshot = cw.displaySnapshot;
    snapshot.preloadFullList = cw.preloadFullList;
    snapshot.preloadFirstPage = cw.preloadFirstPage;
    snapshot.pinnedCoverFilterItem = cw.pinnedCoverFilterItem;
    snapshot.discoveryKnown = cw.discoveryKnown;
    snapshot.texCache = cw.texCache;
    snapshot.failedTexCache = cw.failedTexCache;
}

bool AppState::restoreCoverWallDisplaySnapshotForTab(int tabIndex) {
    ++coverWall.viewGeneration;
    if (tabIndex < 0 || tabIndex >= (int)listTabs.size()) return false;

    const CoverWallTabSnapshot& snapshot = listTabs[(size_t)tabIndex].coverWallSnapshot;
    const ListTab& targetTab = listTabs[(size_t)tabIndex];
    CoverWallState& cw = coverWall;
    const uint64_t liveNonCoverTextureEpoch = cw.nonCoverTextureEpoch;
    const std::string currentDbPath = database.getMainDbPath();
    const bool snapshotGenerationChanged = snapshot.generation != cw.generation;
    if (!snapshot.valid || snapshot.dbPath != currentDbPath) {
        return false;
    }
    const bool shouldCarryLiveTexCache =
        !snapshotGenerationChanged &&
        ShouldCarryLiveFullLibraryCoverWallSession(*this, targetTab, cw, snapshot, currentDbPath) &&
        cw.texCache.size() > snapshot.texCache.size();
    LiveFullLibraryCoverWallSession liveSession;
    if (shouldCarryLiveTexCache) {
        liveSession = CaptureLiveFullLibraryCoverWallSession(cw);
    }

    cw.source = static_cast<CoverWallSource>(snapshot.source);
    cw.items = snapshot.items;
    cw.pendingFacetItems = snapshot.pendingFacetItems;
    cw.filteredItems = snapshot.filteredItems;
    cw.fallbackItems = snapshot.fallbackItems;
    cw.globalItemsCache = snapshot.globalItemsCache;
    cw.displaySnapshot = snapshot.displaySnapshot;
    cw.loaded = snapshot.loaded;
    cw.loading = false;
    cw.entriesInFlight = false;
    cw.foldersInFlight = false;
    cw.preloadInFlight = false;
    cw.discoveryDone = snapshot.discoveryDone;
    cw.lastTotalFileCount = snapshot.lastTotalFileCount;
    cw.loadTotal = snapshot.loadTotal;
    cw.globalItemsCacheValid = snapshot.globalItemsCacheValid;
    cw.globalItemsCacheDbPath = snapshot.globalItemsCacheDbPath;
    cw.globalItemsCacheTotalFileCount = snapshot.globalItemsCacheTotalFileCount;
    cw.globalItemsCacheGeneration = snapshot.globalItemsCacheGeneration;
    cw.prevUseFacetMode = snapshot.prevUseFacetMode;
    cw.lastFacetGenSnapshot = snapshot.lastFacetGenSnapshot;
    cw.lastPreviewSearchGen = snapshot.lastPreviewSearchGen;
    cw.lastPreviewFacetGen = snapshot.lastPreviewFacetGen;
    cw.lastPreviewHadVisibleItems = snapshot.lastPreviewHadVisibleItems;
    cw.pendingFacetCursor = snapshot.pendingFacetCursor;
    cw.fallbackResultsGen = snapshot.fallbackResultsGen;
    cw.fallbackFacetsGen = snapshot.fallbackFacetsGen;
    cw.lastDisplaySnapshotCursor = snapshot.lastDisplaySnapshotCursor;
    cw.preloadFullList = snapshot.preloadFullList;
    cw.preloadFirstPage = snapshot.preloadFirstPage;
    cw.preloadDbPath = snapshot.preloadDbPath;
    cw.preloadCursor = snapshot.preloadCursor;
    cw.globalPageNextCursor = snapshot.globalPageNextCursor;
    cw.globalPageHasMore = snapshot.globalPageHasMore;
    cw.globalPageLoadingNext = false;
    cw.globalPageDbPath = snapshot.globalPageDbPath;
    cw.pinnedCoverFilterItem = snapshot.pinnedCoverFilterItem;
    cw.discoveryKnown = snapshot.discoveryKnown;
    cw.texCache = snapshot.texCache;
    cw.failedTexCache = snapshot.failedTexCache;
    if (shouldCarryLiveTexCache) {
        ApplyLiveFullLibraryCoverWallSession(cw, liveSession, liveNonCoverTextureEpoch);
    }
    auto findRestoredItemForKey = [&](const std::string& key) -> const CoverModule::CoverItem* {
        const std::vector<const std::vector<CoverModule::CoverItem>*> sources = {
            &cw.items,
            &cw.pendingFacetItems,
            &cw.filteredItems,
            &cw.fallbackItems,
            &cw.globalItemsCache,
            &cw.displaySnapshot,
            &cw.preloadFullList,
            &cw.preloadFirstPage,
        };
        for (const auto* list : sources) {
            for (const auto& item : *list) {
                if (item.key == key) return &item;
            }
        }
        return nullptr;
    };
    for (auto it = cw.texCache.begin(); it != cw.texCache.end(); ) {
        const CoverModule::CoverItem* item = findRestoredItemForKey(it->first);
        if (!item || !CoverModule::IsCoverTextureHandleCurrent(*item, it->second.texId)) {
            it = cw.texCache.erase(it);
        } else {
            ++it;
        }
    }
    if (!shouldCarryLiveTexCache) {
        cw.preRequestCursor = snapshot.preRequestCursor;
        cw.lastPreRequestTotalItems = snapshot.lastPreRequestTotalItems;
        cw.lastFirstScreenStart = snapshot.lastFirstScreenStart;
        cw.lastFirstScreenEnd = snapshot.lastFirstScreenEnd;
        cw.lastTotalItems = snapshot.lastTotalItems;
        cw.warmupUntil = snapshot.warmupUntil;
    }
    if (snapshot.nonCoverTextureEpoch != liveNonCoverTextureEpoch) {
        for (auto it = cw.texCache.begin(); it != cw.texCache.end(); ) {
            if (it->second.nonCoverTextureEpoch != liveNonCoverTextureEpoch) {
                it = cw.texCache.erase(it);
            } else {
                ++it;
            }
        }
    }
    cw.nonCoverTextureEpoch = liveNonCoverTextureEpoch;
    coverWallDisplayRefreshRequested = false;
    coverWallDatabaseInvalidationRequested = false;
    needsCoverWallReload = false;
    return true;
}

void AppState::seedCoverWallSnapshotForNewTabFromCurrent(int newTabIndex) {
    if (newTabIndex < 0 || newTabIndex >= (int)listTabs.size()) return;
    const bool currentViewIsFullLibrary =
        viewMode == ViewMode::Database &&
        selectedIndicesSet.size() <= 1 &&
        !AppStateHasActiveDbFilterForCoverWall(*this);
    if (!currentViewIsFullLibrary || coverWall.items.empty()) return;
    saveCoverWallDisplaySnapshotForTab(newTabIndex);
}

bool AppState::activateTab(int tabIndex, double activationTimeSeconds, bool runMemoryGc) {
    if (tabIndex < 0 || tabIndex >= (int)listTabs.size()) return false;
    if (activeTabIndex == tabIndex) {
        if (activationTimeSeconds >= 0.0) {
            listTabs[(size_t)tabIndex].lastActivatedAt = activationTimeSeconds;
        }
        return true;
    }

    saveCoverWallDisplaySnapshotForTab(activeTabIndex);
    parkLiveResultsInCurrentTab();
    syncToTab();
    activeTabIndex = tabIndex;
    pendingTabSelectionIndex = tabIndex;
    syncFromTab();
    // Run global cache GC before restoring non-owning cover-wall texture
    // mirrors so the cold-path restore validates against the post-GC texture
    // owner state instead of reviving stale GL ids.
    if (runMemoryGc) {
        performGlobalMemoryGC();
    }
    restoreCoverWallDisplaySnapshotForTab(tabIndex);
    if (activationTimeSeconds >= 0.0) {
        listTabs[(size_t)tabIndex].lastActivatedAt = activationTimeSeconds;
    }
    return true;
}

int AppState::createAndActivateNewDatabaseTab(double activationTimeSeconds) {
    const int oldTabIndex = activeTabIndex;
    if (oldTabIndex >= 0 && oldTabIndex < (int)listTabs.size()) {
        saveCoverWallDisplaySnapshotForTab(oldTabIndex);
        parkLiveResultsInCurrentTab();
        syncToTab();
    }

    const int newIdx = createNewTab(u8"全部文件");
    seedCoverWallSnapshotForNewTabFromCurrent(newIdx);
    activeTabIndex = newIdx;
    pendingTabSelectionIndex = newIdx;
    if (activationTimeSeconds >= 0.0) {
        listTabs[(size_t)newIdx].lastActivatedAt = activationTimeSeconds;
    }

    ListTab& newTab = listTabs[(size_t)newIdx];
    newTab.dataUnloaded = true;
    newTab.searchResults.clear();
    newTab.searchResultsSnapshotValid = false;
    newTab.totalFileCount = 0;
    newTab.loadedCount = 0;
    newTab.hasMoreResults = false;
    newTab.coverFacets.clear();
    newTab.pathFacets.clear();
    newTab.facetsSnapshotValid = false;
    newTab.resultsBuiltAtRevision = 0;
    syncFromTab();
    needsTabResultsRefresh = true;
    restoreCoverWallDisplaySnapshotForTab(newIdx);
    return newIdx;
}

void AppState::addToPlayHistory(int64_t fileId, const std::string& filePath, const std::string& fileName) {
    if (!playHistory.empty() && playHistory.front().filePath == filePath) return;
    PlayHistoryItem item;
    item.fileId = fileId;
    item.filePath = filePath;
    item.fileName = fileName;
    item.timestamp = (int64_t)time(nullptr);
    playHistory.insert(playHistory.begin(), item);
    if (playHistory.size() > MAX_PLAY_HISTORY) playHistory.pop_back();
    playHistoryIndex = -1;
}

bool AppState::canGoBack() const {
    return playHistory.size() > 1 && playHistoryIndex < (int)playHistory.size() - 1;
}

bool AppState::canGoForward() const {
    return playHistoryIndex > 0;
}

PlayHistoryItem* AppState::goBack() {
    if (!canGoBack()) return nullptr;
    if (playHistoryIndex < 0) playHistoryIndex = 0;
    playHistoryIndex++;
    if (playHistoryIndex < (int)playHistory.size()) return &playHistory[playHistoryIndex];
    return nullptr;
}

PlayHistoryItem* AppState::goForward() {
    if (!canGoForward()) return nullptr;
    playHistoryIndex--;
    if (playHistoryIndex >= 0 && playHistoryIndex < (int)playHistory.size()) return &playHistory[playHistoryIndex];
    return nullptr;
}

// View State History (browser-like back/forward for window state)

ViewStateSnapshot AppState::captureViewState() const {
    ViewStateSnapshot snap;
    snap.viewMode = viewMode;
    snap.browserScope = (int)fileBrowser.getScope();
    snap.folderFilterActive = folderFilterActive;
    snap.selectedFolderPath = selectedFolderPath;
    snap.coverFilterActive = coverFilterActive;
    snap.coverFilterPath = coverFilterPath;
    snap.coverFilterIndex = coverFilterIndex;
    snap.searchText = std::string(searchBuffer);
    snap.selectedPlaylistId = selectedPlaylistId;
    snap.playlistAggregatedView = playlistAggregatedView;
    snap.findSameFilenameActive = findSameFilenameActive;
    snap.findSameFilenameQuery = findSameFilenameQuery;
    return snap;
}

void AppState::pushViewState() {
    if (viewStateNavigating) return; // Don't push while restoring
    
    ViewStateSnapshot snap = captureViewState();
    
    // Don't push if identical to current state
    if (viewStateIndex >= 0 && viewStateIndex < (int)viewStateHistory.size()) {
        if (viewStateHistory[viewStateIndex] == snap) return;
    }
    
    // Truncate forward history (discard everything after current position)
    if (viewStateIndex >= 0 && viewStateIndex < (int)viewStateHistory.size() - 1) {
        viewStateHistory.resize(viewStateIndex + 1);
    }
    
    viewStateHistory.push_back(snap);
    if (viewStateHistory.size() > MAX_VIEW_STATE_HISTORY) {
        viewStateHistory.erase(viewStateHistory.begin());
    }
    viewStateIndex = (int)viewStateHistory.size() - 1;
}

void AppState::restoreViewState(const ViewStateSnapshot& snap) {
    viewStateNavigating = true;
    viewMode = snap.viewMode;
    if (snap.viewMode == ViewMode::Browser) {
        fileBrowser.setScope(static_cast<BrowserScope>(snap.browserScope));
    }
    folderFilterActive = snap.folderFilterActive;
    selectedFolderPath = snap.selectedFolderPath;
    coverFilterActive = snap.coverFilterActive;
    coverFilterPath = snap.coverFilterPath;
    coverFilterIndex = snap.coverFilterIndex;
#ifdef _WIN32
    strncpy_s(searchBuffer, sizeof(searchBuffer), snap.searchText.c_str(), _TRUNCATE);
#else
    std::strncpy(searchBuffer, snap.searchText.c_str(), sizeof(searchBuffer) - 1);
    searchBuffer[sizeof(searchBuffer) - 1] = '\0';
#endif
    selectedPlaylistId = snap.selectedPlaylistId;
    playlistAggregatedView = snap.playlistAggregatedView;
    findSameFilenameActive = snap.findSameFilenameActive;
    findSameFilenameQuery = snap.findSameFilenameQuery;
    viewStateNavigating = false;
}

bool AppState::canViewStateBack() const {
    return viewStateIndex > 0;
}

bool AppState::canViewStateForward() const {
    return viewStateIndex >= 0 && viewStateIndex < (int)viewStateHistory.size() - 1;
}

bool AppState::viewStateBack() {
    if (!canViewStateBack()) return false;
    viewStateIndex--;
    restoreViewState(viewStateHistory[viewStateIndex]);
    return true;
}

bool AppState::viewStateForward() {
    if (!canViewStateForward()) return false;
    viewStateIndex++;
    restoreViewState(viewStateHistory[viewStateIndex]);
    return true;
}

bool AppState::shouldShowFile(const std::string& extension) const {
    std::string ext = extension;
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    if (ext == "wav" || ext == ".wav") return showWAV;
    if (ext == "mp3" || ext == ".mp3") return showMP3;
    if (ext == "flac" || ext == ".flac") return showFLAC;
    if (ext == "ogg" || ext == ".ogg") return showOGG;
    if (ext == "aiff" || ext == ".aiff" || ext == "aif" || ext == ".aif") return showAIFF;
    if (ext == "m4a" || ext == ".m4a") return showM4A;
    if (ext == "wma" || ext == ".wma") return showWMA;
    return showOther;
}

void AppState::resetRandomSampling() {
    randomShownIds.clear();
    randomAvailableIds.clear();
    randomTotalPool = 0;
    randomAllPlayed = false;
}

// 单线程 UI 主线程调用；若未来跨线程访问，必须补互斥保护。
void AppState::addPendingWaveform(const std::string& path, bool isVisible) {
    if (path.empty()) return;
    if (waveformRebuildActive) return;
    if (waveformFailed.count(path) > 0) return;
    if (waveformMiniCacheMiss.count(path) > 0) return;
    if (waveformCache.count(path) > 0) return;
    if (waveformPendingSet.count(path) > 0) {
        if (!isVisible) return;

        // 当前选中文件再次被点中时，允许把它从旧位置提升到 urgent 队尾，
        // 避免它已经在 normal/旧 urgent 队列里却还要再等一轮。
        auto eraseOne = [&](std::vector<std::string>& queue) {
            auto it = std::find(queue.begin(), queue.end(), path);
            if (it != queue.end()) queue.erase(it);
        };
        eraseOne(waveformNormalPending);
        eraseOne(waveformUrgentPending);
        waveformUrgentPending.push_back(path);
        return;
    }

    waveformPendingSet.insert(path);
    if (isVisible) {
        waveformUrgentPending.push_back(path);
    } else {
        waveformNormalPending.push_back(path);
    }

    // 快速上下键切文件时避免队列无限增长导致大量 mini 解码卡顿：保留最近 N 个，丢弃最旧
    // 200 ~= visible window (≈30) + lookahead (20) + several pages of scrollback
    // for "scroll fast then scroll back" reuse. Submission is rate-limited
    // separately by submitBudget.
    constexpr size_t kMaxPendingWaveforms = 200;
    size_t total = waveformUrgentPending.size() + waveformNormalPending.size();
    while (total > kMaxPendingWaveforms) {
        std::string oldest;
        if (!waveformNormalPending.empty()) {
            oldest = waveformNormalPending.front();
            waveformNormalPending.erase(waveformNormalPending.begin());
        } else if (!waveformUrgentPending.empty()) {
            oldest = waveformUrgentPending.front();
            waveformUrgentPending.erase(waveformUrgentPending.begin());
        } else {
            break;
        }
        waveformPendingSet.erase(oldest);
        --total;
    }
}

// 单线程 UI 主线程调用；若未来跨线程访问，必须补互斥保护。
void AppState::clearPendingWaveforms(bool cancelWorker) {
    waveformUrgentPending.clear();
    waveformNormalPending.clear();
    waveformPendingSet.clear();
    visibleFilePaths.clear();
    // Drop the meter follow-up dedup set: any in-flight meterOnly request is
    // about to be cancelled below, so a stale entry would prevent the next
    // visible-mini result from re-enqueueing meter computation indefinitely.
    meterFollowupPending.clear();
    // Clear queued worker requests so newly visible files retain priority.

    if (cancelWorker && !waveformRebuildActive) {
        waveformWorker.cancelAllPending();
    }
}

// 单线程 UI 主线程调用；若未来跨线程访问，必须补互斥保护。
bool AppState::popNextPendingWaveform(std::string& out) {
    if (!waveformUrgentPending.empty()) {
        out = std::move(waveformUrgentPending.back());
        waveformUrgentPending.pop_back();
        waveformPendingSet.erase(out);
        return true;
    }
    if (!waveformNormalPending.empty()) {
        out = std::move(waveformNormalPending.back());
        waveformNormalPending.pop_back();
        waveformPendingSet.erase(out);
        return true;
    }
    return false;
}

// 单线程 UI 主线程调用；若未来跨线程访问，必须补互斥保护。
void AppState::removePendingWaveform(const std::string& path) {
    if (path.empty()) return;
    waveformPendingSet.erase(path);
    auto itU = std::find(waveformUrgentPending.begin(), waveformUrgentPending.end(), path);
    if (itU != waveformUrgentPending.end()) waveformUrgentPending.erase(itU);
    auto itN = std::find(waveformNormalPending.begin(), waveformNormalPending.end(), path);
    if (itN != waveformNormalPending.end()) waveformNormalPending.erase(itN);
}

// 异步预读机制（鼠标悬停200ms后预读）

void AppState::recordHoverStart(const std::string& filePath) {
    if (filePath.empty()) return;

    std::lock_guard<std::mutex> lock(hoverPrefetchMutex);
    if (hoverPrefetchMap.count(filePath)) return;

    // Evict oldest entries if at capacity
    while (hoverPrefetchMap.size() >= MAX_HOVER_PREFETCH_MAP_SIZE) {
        auto oldest = hoverPrefetchMap.begin();
        for (auto it = hoverPrefetchMap.begin(); it != hoverPrefetchMap.end(); ++it) {
            if (it->second.hoverStartTime < oldest->second.hoverStartTime)
                oldest = it;
        }
        hoverPrefetchMap.erase(oldest);
    }

    HoverPrefetchEntry entry;
    entry.filePath = filePath;
    entry.hoverStartTime = std::chrono::steady_clock::now();
    entry.prefetchRequested = false;
    hoverPrefetchMap[filePath] = entry;
}

void AppState::recordHoverEnd(const std::string& filePath) {
    if (filePath.empty()) return;
    
    std::lock_guard<std::mutex> lock(hoverPrefetchMutex);
    hoverPrefetchMap.erase(filePath);
}

void AppState::processHoverPrefetch() {
    auto now = std::chrono::steady_clock::now();
    std::vector<std::string> toPrefetch;

    {
        std::lock_guard<std::mutex> lock(hoverPrefetchMutex);
        // Purge expired entries (older than HOVER_PREFETCH_EXPIRE_SEC)
        for (auto it = hoverPrefetchMap.begin(); it != hoverPrefetchMap.end(); ) {
            auto ageSec = std::chrono::duration_cast<std::chrono::seconds>(
                now - it->second.hoverStartTime).count();
            if (ageSec > HOVER_PREFETCH_EXPIRE_SEC)
                it = hoverPrefetchMap.erase(it);
            else
                ++it;
        }
        for (auto& [filePath, entry] : hoverPrefetchMap) {
            if (entry.prefetchRequested) continue;
            
            auto hoverDuration = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - entry.hoverStartTime).count();
            
            if (hoverDuration >= HOVER_PREFETCH_DELAY_MS) {
                // 检查是否已经在缓存中或正在处理
                if (waveformCache.count(filePath) > 0) {
                    hoverPrefetchMap.erase(filePath);
                    continue;
                }
                if (waveformPendingSet.count(filePath) > 0) {
                    hoverPrefetchMap.erase(filePath);
                    continue;
                }
                if (waveformFailed.count(filePath) > 0) {
                    hoverPrefetchMap.erase(filePath);
                    continue;
                }
                
                entry.prefetchRequested = true;
                toPrefetch.push_back(filePath);
            }
        }
    }
    
    // 将需要预读的文件加入队列（高优先级，插入到前面）
    for (const auto& filePath : toPrefetch) {
        addPendingWaveform(filePath, true);  // isVisible=true 表示高优先级
    }
}

std::vector<AudioRecord> AppState::getSelectedRecords() const {
    std::vector<AudioRecord> result;
    if (allSelected) {
        // 全选模式：返回所有搜索结果
        result = searchResults;
    } else {
        // 普通模式：返回集合中的记录
        for (int idx : selectedIndicesSet) {
            if (idx >= 0 && idx < (int)searchResults.size()) {
                result.push_back(searchResults[idx]);
            }
        }
    }
    return result;
}

bool AppState::beginAudioTargetFromSearchResult(int index) {
    if (index < 0 || index >= (int)searchResults.size()) {
        pendingAudioTargetFileId = -1;
        pendingAudioTargetPath.clear();
        return false;
    }

    const AudioRecord& record = searchResults[(size_t)index];
    selectedResultIndex = index;
    pendingAudioTargetFileId = (record.id > 0) ? record.id : -1;
    pendingAudioTargetPath = record.filePath;
    return true;
}

void AppState::clearPendingAudioTarget() {
    pendingAudioTargetFileId = -1;
    pendingAudioTargetPath.clear();
}

int64_t AppState::activeAudioTargetFileId() const {
    return (pendingAudioTargetFileId > 0) ? pendingAudioTargetFileId : currentFileId;
}

void AppState::clearSelection() {
    selectedIndicesSet.clear();
    allSelected = false; // Reset virtual select-all state.
    selectedResultIndex = -1;
    lastClickedIndex = -1;
}

bool AppState::isIndexSelected(int idx) const {
    return allSelected || selectedIndicesSet.find(idx) != selectedIndicesSet.end();
}

void AppState::toggleSelection(int idx) {
    // Clicking one row exits virtual select-all and begins ordinary selection.

    if (allSelected) {
        allSelected = false;
        selectedIndicesSet.clear();
        selectedIndicesSet.insert(idx);
    } else {
        if (selectedIndicesSet.find(idx) != selectedIndicesSet.end()) {
            selectedIndicesSet.erase(idx);
        } else {
            selectedIndicesSet.insert(idx);
        }
    }
}

void AppState::selectRange(int start, int end) {
    int minIdx = (start < end) ? start : end;
    int maxIdx = (start > end) ? start : end;
    for (int i = minIdx; i <= maxIdx; ++i) {
        selectedIndicesSet.insert(i);
    }
}

// ---------------------------------------------------------------------------
// Playlist quick-bar chip plan — pure helper (spec §3.1)
// ---------------------------------------------------------------------------
namespace HarkenSearch {

std::vector<PlaylistChipPlanItem> ComputePlaylistChipPlan(
    const std::vector<Playlist>& all,
    const std::vector<int64_t>& hiddenIds) {

    auto isHidden = [&](int64_t id) {
        return std::find(hiddenIds.begin(), hiddenIds.end(), id) != hiddenIds.end();
    };

    // Build id -> Playlist* index
    std::unordered_map<int64_t, const Playlist*> byId;
    byId.reserve(all.size());
    for (const auto& p : all) byId[p.id] = &p;

    std::vector<PlaylistChipPlanItem> out;
    out.reserve(all.size());

    for (const auto& p : all) {
        if (isHidden(p.id)) continue;

        // sub-playlist case
        if (p.parent_id > 0) {
            auto it = byId.find(p.parent_id);
            if (it != byId.end() && !isHidden(p.parent_id)) {
                continue;  // parent visible -> child collapses into popup
            }
            // parent hidden (or missing) -> render child independently
            PlaylistChipPlanItem item;
            item.id       = p.id;
            item.name     = p.name;
            item.kind     = ChipKind::IndependentChild;
            item.parentId = p.parent_id;
            if (it != byId.end()) item.parentName = it->second->name;
            out.push_back(std::move(item));
            continue;
        }

        // top-level: folder or orphan
        PlaylistChipPlanItem item;
        item.id   = p.id;
        item.name = p.name;
        if (p.is_folder) {
            item.kind = ChipKind::Folder;
            int n = 0;
            for (const auto& q : all) {
                if (q.parent_id == p.id && !isHidden(q.id)) ++n;
            }
            item.visibleChildCount = n;
        } else {
            item.kind = ChipKind::Orphan;
        }
        out.push_back(std::move(item));
    }
    return out;
}

} // namespace HarkenSearch

// H1 (P1-2): render the transfer status on the UI thread; workers only store
// ledger keys + args (never call GetLocalizedString off-thread).
std::string AppState::TransferStatusText() const {
    using Mode = TransferStatusArgMode;
    if (transferStatusArgMode == Mode::Literal) return transferStatusMessage;
    const char* fmt = GetLocalizedString(transferStatusFmtKey);
    char buf[1280];
    switch (transferStatusArgMode) {
    case Mode::TwoStrings:
        std::snprintf(buf, sizeof(buf), fmt, transferStatusArg1.c_str(), transferStatusArg2.c_str());
        break;
    case Mode::OneInt:
        std::snprintf(buf, sizeof(buf), fmt, transferStatusN1);
        break;
    default:
        std::snprintf(buf, sizeof(buf), fmt, transferStatusN1, transferStatusN2);
        break;
    }
    return buf;
}
