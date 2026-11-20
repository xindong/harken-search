#pragma once

#include "FileTypeVisibilityPolicy.h"

#include <cstddef>
#include <string>
#include <vector>

namespace aura::playlist {

struct PlaylistResultFilterRecord {
    std::string folderPath;
    std::string extension;
    bool wavPackLossless = false;
};

struct PlaylistResultFilterInput {
    bool rebuildFolderTree = false;
    bool folderFilterActive = false;
    std::string selectedFolderPath;
    aura::file_types::FileTypeVisibility visibility;
    std::vector<PlaylistResultFilterRecord> records;
};

class PlaylistResultFilterEffects {
public:
    virtual ~PlaylistResultFilterEffects() = default;
    virtual void rebuildFolderTree() = 0;
};

std::vector<std::size_t> HandlePlaylistResultFilter(
    const PlaylistResultFilterInput& input,
    PlaylistResultFilterEffects& effects);

}  // namespace aura::playlist
