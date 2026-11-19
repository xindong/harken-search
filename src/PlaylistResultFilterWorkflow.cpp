#include "PlaylistResultFilterWorkflow.h"

namespace aura::playlist {
namespace {

bool IsSameOrDescendant(std::string folder, std::string selected) {
    for (auto* value : {&folder, &selected}) {
        for (char& c : *value) {
            if (c == '\\') c = '/';
        }
        while (value->size() > 1 && value->back() == '/') value->pop_back();
    }
    if (selected.empty()) return true;
    if (folder == selected) return true;
    return folder.size() > selected.size() &&
        folder.compare(0, selected.size(), selected) == 0 &&
        folder[selected.size()] == '/';
}

}  // namespace

std::vector<std::size_t> HandlePlaylistResultFilter(
    const PlaylistResultFilterInput& input,
    PlaylistResultFilterEffects& effects) {
    if (input.rebuildFolderTree) effects.rebuildFolderTree();
    std::vector<std::size_t> kept;
    kept.reserve(input.records.size());
    for (std::size_t i = 0; i < input.records.size(); ++i) {
        const auto& record = input.records[i];
        if (input.folderFilterActive && !input.selectedFolderPath.empty() &&
            !IsSameOrDescendant(record.folderPath, input.selectedFolderPath)) {
            continue;
        }
        if (!aura::file_types::isVisible(input.visibility, record.extension, record.wavPackLossless)) continue;
        kept.push_back(i);
    }
    return kept;
}

}  // namespace aura::playlist
