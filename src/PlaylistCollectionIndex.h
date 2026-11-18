#pragma once

#include "Database.h"

#include <cstdint>
#include <functional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace aura::playlist {

inline uint64_t MixPlaylistHash(uint64_t seed, int64_t value) {
    seed ^= static_cast<uint64_t>(value) + 0x9e3779b97f4a7c15ull + (seed << 6) + (seed >> 2);
    return seed;
}

inline uint64_t PlaylistCollectionSignature(
    const std::vector<Playlist>& playlists,
    const std::vector<int64_t>& hiddenIds) {
    uint64_t hash = 1469598103934665603ull;
    hash = MixPlaylistHash(hash, static_cast<int64_t>(playlists.size()));
    for (const Playlist& playlist : playlists) {
        hash = MixPlaylistHash(hash, playlist.id);
        hash = MixPlaylistHash(hash, playlist.parent_id);
        hash = MixPlaylistHash(hash, playlist.fileCount);
        hash = MixPlaylistHash(hash, playlist.is_folder ? 1 : 0);
    }
    hash = MixPlaylistHash(hash, static_cast<int64_t>(hiddenIds.size()));
    for (int64_t id : hiddenIds) hash = MixPlaylistHash(hash, id);
    return hash;
}

inline uint64_t PlaylistQuickBarOrderSignature(
    const std::vector<Playlist>& playlists,
    const std::vector<int64_t>& order) {
    uint64_t hash = 1469598103934665603ull;
    hash = MixPlaylistHash(hash, static_cast<int64_t>(order.size()));
    for (int64_t id : order) hash = MixPlaylistHash(hash, id);
    hash = MixPlaylistHash(hash, static_cast<int64_t>(playlists.size()));
    for (const Playlist& playlist : playlists) hash = MixPlaylistHash(hash, playlist.id);
    return hash;
}

struct PlaylistCollectionIndex {
    std::unordered_map<int64_t, size_t> indexById;
    std::unordered_map<int64_t, std::vector<size_t>> childrenByParent;
    std::unordered_map<int64_t, int64_t> displayedCountById;
    std::unordered_set<int64_t> hiddenIds;
    uint64_t signature = 0;

    void Rebuild(const std::vector<Playlist>& playlists,
                 const std::vector<int64_t>& hidden) {
        indexById.clear();
        childrenByParent.clear();
        displayedCountById.clear();
        hiddenIds.clear();
        indexById.reserve(playlists.size());
        childrenByParent.reserve(playlists.size());
        displayedCountById.reserve(playlists.size());
        hiddenIds.reserve(hidden.size());

        for (int64_t id : hidden) hiddenIds.insert(id);
        for (size_t i = 0; i < playlists.size(); ++i) {
            const Playlist& playlist = playlists[i];
            indexById[playlist.id] = i;
            if (playlist.parent_id > 0) {
                childrenByParent[playlist.parent_id].push_back(i);
            }
        }

        constexpr int64_t kUnknown = -2;
        constexpr int64_t kVisiting = -3;
        std::vector<int64_t> memo(playlists.size(), kUnknown);
        std::function<int64_t(size_t)> resolve = [&](size_t index) -> int64_t {
            if (memo[index] != kUnknown) {
                return memo[index] == kVisiting ? -1 : memo[index];
            }
            memo[index] = kVisiting;
            const Playlist& playlist = playlists[index];
            if (!playlist.is_folder) {
                memo[index] = playlist.fileCount;
                return memo[index];
            }
            int64_t total = 0;
            auto it = childrenByParent.find(playlist.id);
            if (it != childrenByParent.end()) {
                for (size_t child : it->second) {
                    const int64_t childCount = resolve(child);
                    if (childCount < 0) {
                        total = -1;
                        break;
                    }
                    total += childCount;
                }
            }
            memo[index] = total;
            return total;
        };

        for (size_t i = 0; i < playlists.size(); ++i) {
            displayedCountById[playlists[i].id] = resolve(i);
        }
    }

    bool IsHidden(int64_t playlistId) const {
        return hiddenIds.find(playlistId) != hiddenIds.end();
    }

    int64_t DisplayedCount(int64_t playlistId) const {
        auto it = displayedCountById.find(playlistId);
        return it == displayedCountById.end() ? 0 : it->second;
    }

    const std::vector<size_t>* Children(int64_t parentId) const {
        auto it = childrenByParent.find(parentId);
        return it == childrenByParent.end() ? nullptr : &it->second;
    }
};

}  // namespace aura::playlist
