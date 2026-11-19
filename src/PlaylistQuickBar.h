#pragma once

#include "imgui.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

struct AppState;
struct Playlist;

// @stage7-contract
// API contract: UI-thread drawing of the playlist quick bar. AppState is
// borrowed for one frame and not retained. Popups that copy, rename, delete,
// or bulk-add playlists remain owned by the composition root.

struct PlaylistQuickBarReorderAnimState {
    int64_t draggedPlaylistId = 0;
    float dragGrabOffsetX = 0.0f;
    bool orderDirty = false;
    struct Item {
        int64_t playlistId = 0;
        float currentX = 0.0f;
        float targetX = 0.0f;
        float velocityX = 0.0f;
        bool initialized = false;
    };
    std::vector<Item> items;
    std::unordered_map<int64_t, size_t> indexById;
};

struct PlaylistQuickDragState {
    bool armed = false;
    bool active = false;
    ImVec2 startMousePos = {0, 0};
    std::vector<int64_t> fileIds;
    std::string label;
};

extern PlaylistQuickBarReorderAnimState g_playlistQuickBarReorderAnim;
extern PlaylistQuickDragState g_playlistQuickDrag;

PlaylistQuickBarReorderAnimState::Item& EnsurePlaylistQuickBarAnimItem(
    int64_t playlistId,
    float initialX);
void PrunePlaylistQuickBarAnimItems(const std::vector<Playlist>& playlists);

void RenderPlaylistQuickBar(AppState& state, float height);
void RenderPlaylistQuickBarRestoreStrip(AppState& state, float height);
