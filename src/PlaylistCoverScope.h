#pragma once

#include "CoverModule.h"
#include "Database.h"

#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace aura::playlist {

struct PlaylistCoverGroup {
    std::string key;
    std::string path;
    std::string sourcePath;
    bool isEmbedded = false;
    int64_t hitCount = 0;
};

inline void AddPlaylistCoverKey(std::vector<PlaylistCoverGroup>& groups,
                                std::unordered_map<std::string, size_t>& index,
                                const std::string& key,
                                const std::string& path,
                                const std::string& sourcePath,
                                bool isEmbedded) {
    if (key.empty()) return;
    const auto [position, inserted] = index.emplace(key, groups.size());
    if (inserted) {
        PlaylistCoverGroup group;
        group.key = key;
        group.path = path;
        group.sourcePath = sourcePath;
        group.isEmbedded = isEmbedded;
        group.hitCount = 1;
        groups.push_back(std::move(group));
    } else {
        auto& group = groups[position->second];
        group.hitCount++;
        if (group.path.empty()) group.path = path;
        if (group.sourcePath.empty()) group.sourcePath = sourcePath;
    }
}

inline std::vector<PlaylistCoverGroup> BuildPlaylistCoverGroupsFromRecords(
    const std::vector<AudioRecord>& records) {
    std::vector<PlaylistCoverGroup> groups;
    std::unordered_map<std::string, size_t> index;
    index.reserve(records.size());
    for (const AudioRecord& record : records) {
        AddPlaylistCoverKey(groups, index, record.folderCoverKey,
                            record.folderPath, record.folderCoverSrcPath, false);
        AddPlaylistCoverKey(groups, index, record.embeddedCoverKey,
                            record.filePath, {}, true);
    }
    return groups;
}

inline std::vector<PlaylistCoverGroup> BuildPlaylistCoverGroupsFromCompleteMembers(
    const std::vector<AudioRecord>& completeMembers,
    const std::vector<AudioRecord>& loadedPage) {
    (void)loadedPage;
    return BuildPlaylistCoverGroupsFromRecords(completeMembers);
}

inline std::vector<CoverModule::CoverItem> BuildPlaylistCoverItemsFromGroups(
    const std::vector<PlaylistCoverGroup>& groups) {
    std::vector<CoverModule::CoverItem> items;
    items.reserve(groups.size());
    for (const PlaylistCoverGroup& group : groups) {
        if (group.key.empty() || group.hitCount <= 0) continue;
        CoverModule::CoverItem item;
        item.key = group.key;
        item.hitCount = group.hitCount;
        if (group.isEmbedded) {
            item.kind = CoverModule::CoverKind::EmbeddedCover;
            item.filePathUtf8 = group.path;
        } else {
            item.kind = CoverModule::CoverKind::FolderCover;
            item.folderPathUtf8 = group.path;
            item.folderCoverSrcPathUtf8 = group.sourcePath;
        }
        items.push_back(std::move(item));
    }
    return items;
}

}  // namespace aura::playlist
