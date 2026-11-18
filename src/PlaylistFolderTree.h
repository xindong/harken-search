#pragma once

#include "Database.h"
#include "PathUtils.h"

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

namespace PlaylistFolderTreeDetail {

inline bool StartsWithAsciiCaseInsensitive(std::string_view value,
                                           std::string_view prefix) {
    if (value.size() < prefix.size()) return false;
    for (size_t i = 0; i < prefix.size(); ++i) {
        char a = value[i];
        char b = prefix[i];
        if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = static_cast<char>(b - 'A' + 'a');
        if (a != b) return false;
    }
    return true;
}

inline std::string StripLongPathPrefixForDisplay(std::string path) {
#ifdef _WIN32
    if (StartsWithAsciiCaseInsensitive(path, R"(\\?\UNC\)")) {
        return std::string("\\\\") + path.substr(8);
    }
    if (StartsWithAsciiCaseInsensitive(path, R"(\\?\)")) {
        return path.substr(4);
    }
#endif
    return path;
}

// @stage7-contract
// API contract: playlist-folder helpers synchronously transform caller-owned
// audio records into owned tree/filter results. In-place overloads mutate only
// the supplied vector; path views are borrowed per call and canonicalized
// without database IO or retained references.

inline char PreferredSeparator() {
#ifdef _WIN32
    return '\\';
#else
    return '/';
#endif
}

inline std::string NormalizeDisplayPath(std::string path) {
    path = StripLongPathPrefixForDisplay(std::move(path));
    const char sep = PreferredSeparator();
    const char other = (sep == '\\') ? '/' : '\\';
    std::replace(path.begin(), path.end(), other, sep);
    while (path.size() > 3 && (path.back() == '\\' || path.back() == '/')) {
        path.pop_back();
    }
    return path;
}

inline std::vector<std::string> SplitPathSegments(std::string_view path) {
    std::vector<std::string> parts;
    size_t start = 0;
    for (size_t i = 0; i <= path.size(); ++i) {
        const bool atEnd = (i == path.size());
        const bool isSep = !atEnd && (path[i] == '\\' || path[i] == '/');
        if (!atEnd && !isSep) continue;

        if (i > start) {
            std::string part(path.substr(start, i - start));
            if (!part.empty()) parts.push_back(std::move(part));
        }
        start = i + 1;
    }
    return parts;
}

inline std::string JoinPathPrefix(const std::vector<std::string>& parts, size_t inclusiveIndex) {
    std::string out;
    const char sep = PreferredSeparator();
    for (size_t i = 0; i <= inclusiveIndex && i < parts.size(); ++i) {
        if (!out.empty()) out.push_back(sep);
        out += parts[i];
    }
    return out;
}

inline std::string FolderPathForRecord(const AudioRecord& record) {
    if (!record.folderPath.empty()) {
        return HarkenSearch::CanonicalFolderPath(record.folderPath);
    }
    return HarkenSearch::CanonicalFolderPath(HarkenSearch::DisplayFolderFromFilePath(record.filePath));
}

inline std::string DisplayFolderPathForRecord(const AudioRecord& record) {
    std::string display = HarkenSearch::DisplayFolderFromFilePath(record.filePath);
    if (!display.empty()) return NormalizeDisplayPath(std::move(display));
    return NormalizeDisplayPath(record.folderPath);
}

inline void InsertRecordFolder(FolderInfo& root, const AudioRecord& record) {
    const std::string canonicalFolder = FolderPathForRecord(record);
    if (canonicalFolder.empty()) return;

    const std::vector<std::string> parts = SplitPathSegments(canonicalFolder);
    if (parts.empty()) return;

    const std::vector<std::string> displayParts =
        SplitPathSegments(DisplayFolderPathForRecord(record));

    FolderInfo* current = &root;
    for (size_t i = 0; i < parts.size(); ++i) {
        const std::string& key = parts[i];
        const std::string display = (i < displayParts.size() && !displayParts[i].empty())
            ? displayParts[i]
            : key;

        FolderInfo* found = nullptr;
        for (FolderInfo& child : current->subfolders) {
            if (child.name == key) {
                found = &child;
                break;
            }
        }

        if (!found) {
            FolderInfo node;
            node.name = key;
            node.displayName = display;
            node.path = JoinPathPrefix(parts, i);
            current->subfolders.push_back(std::move(node));
            found = &current->subfolders.back();
        } else if (found->displayName.empty() && !display.empty()) {
            found->displayName = display;
        }

        if (i + 1 == parts.size()) {
            found->fileCount += 1;
        }
        current = found;
    }
}

inline void SortTree(FolderInfo& folder) {
    std::sort(folder.subfolders.begin(), folder.subfolders.end(),
              [](const FolderInfo& a, const FolderInfo& b) {
                  return a.name < b.name;
              });
    for (FolderInfo& child : folder.subfolders) {
        SortTree(child);
    }
}

inline bool IsSameOrDescendantFolder(std::string_view childPath, std::string_view parentPath) {
    const std::string child = HarkenSearch::CanonicalFolderPath(childPath);
    const std::string parent = HarkenSearch::CanonicalFolderPath(parentPath);
    if (child.empty() || parent.empty()) return false;
    if (child == parent) return true;
    if (child.size() <= parent.size()) return false;
    if (child.compare(0, parent.size(), parent) != 0) return false;
    const char boundary = child[parent.size()];
    return boundary == '\\' || boundary == '/';
}

} // namespace PlaylistFolderTreeDetail

inline FolderInfo BuildPlaylistFolderTreeFromRecords(const std::vector<AudioRecord>& records) {
    FolderInfo root;
    root.name = "playlist";
    root.displayName = "Playlist";
    root.path.clear();
    root.fileCount = static_cast<int>(records.size());

    for (const AudioRecord& record : records) {
        PlaylistFolderTreeDetail::InsertRecordFolder(root, record);
    }
    PlaylistFolderTreeDetail::SortTree(root);
    return root;
}

inline FolderInfo BuildPlaylistFolderTreeFromCompleteMembers(
    const std::vector<AudioRecord>& completeMembers,
    const std::vector<AudioRecord>& loadedPage) {
    (void)loadedPage;
    return BuildPlaylistFolderTreeFromRecords(completeMembers);
}

// Builds the playlist folder tree from lightweight folder counts instead of
// complete AudioRecord rows. Entries are sorted once, then walked with a parent
// chain so each distinct folder path is inserted exactly once.
inline FolderInfo BuildPlaylistFolderTreeFromFolderCounts(
    std::vector<PlaylistFolderCount> folderCounts,
    int64_t totalMemberCount) {
    FolderInfo root;
    root.name = "playlist";
    root.displayName = "Playlist";
    root.path.clear();
    root.fileCount = totalMemberCount > 0 ? static_cast<int>(totalMemberCount) : 0;

    std::sort(folderCounts.begin(), folderCounts.end(),
              [](const PlaylistFolderCount& a, const PlaylistFolderCount& b) {
                  return a.folderPath < b.folderPath;
              });

    std::vector<FolderInfo*> chain;
    chain.push_back(&root);
    std::vector<std::string> previousParts;

    for (const PlaylistFolderCount& folderCount : folderCounts) {
        if (folderCount.folderPath.empty() || folderCount.fileCount <= 0) continue;

        const std::vector<std::string> parts =
            PlaylistFolderTreeDetail::SplitPathSegments(folderCount.folderPath);
        if (parts.empty()) continue;
        const std::vector<std::string> displayParts =
            PlaylistFolderTreeDetail::SplitPathSegments(
                PlaylistFolderTreeDetail::NormalizeDisplayPath(folderCount.displayPath));

        size_t common = 0;
        while (common < previousParts.size() && common < parts.size() &&
               previousParts[common] == parts[common]) {
            ++common;
        }
        if (common < parts.size()) {
            chain.resize(common + 1);
            for (size_t i = common; i < parts.size(); ++i) {
                FolderInfo node;
                node.name = parts[i];
                node.displayName = (i < displayParts.size() && !displayParts[i].empty())
                    ? displayParts[i]
                    : parts[i];
                node.path = PlaylistFolderTreeDetail::JoinPathPrefix(parts, i);
                chain.back()->subfolders.push_back(std::move(node));
                chain.push_back(&chain.back()->subfolders.back());
            }
        }
        chain.back()->fileCount += folderCount.fileCount;
        previousParts = parts;
    }

    PlaylistFolderTreeDetail::SortTree(root);
    return root;
}

inline bool ShouldRenderPlaylistFolderTreeForRow(int64_t rowPlaylistId,
                                                 int64_t expandedPlaylistId,
                                                 int64_t selectedPlaylistId,
                                                 bool playlistViewActive) {
    (void)rowPlaylistId;
    (void)expandedPlaylistId;
    (void)selectedPlaylistId;
    (void)playlistViewActive;
    return false;
}

inline bool ShouldUsePlaylistScopedFolderTree(int64_t selectedPlaylistId,
                                              bool playlistViewActive) {
    return playlistViewActive && selectedPlaylistId > 0;
}

inline int64_t ResolvePlaylistScopedFolderTreePlaylistId(int64_t selectedPlaylistId,
                                                         int64_t currentTabSelectedPlaylistId,
                                                         int64_t expandedPlaylistFolderTreePlaylistId,
                                                         bool playlistViewActive,
                                                         bool currentTabPlaylistViewActive) {
    if (playlistViewActive && selectedPlaylistId > 0) return selectedPlaylistId;
    if (currentTabPlaylistViewActive && currentTabSelectedPlaylistId > 0) {
        return currentTabSelectedPlaylistId;
    }
    if (expandedPlaylistFolderTreePlaylistId > 0 &&
        (selectedPlaylistId == expandedPlaylistFolderTreePlaylistId ||
         currentTabSelectedPlaylistId == expandedPlaylistFolderTreePlaylistId)) {
        return expandedPlaylistFolderTreePlaylistId;
    }
    return -1;
}

inline bool PlaylistRecordMatchesFolderFilter(const AudioRecord& record,
                                              bool folderFilterActive,
                                              std::string_view selectedFolderPath) {
    if (!folderFilterActive || selectedFolderPath.empty()) return true;
    return PlaylistFolderTreeDetail::IsSameOrDescendantFolder(
        PlaylistFolderTreeDetail::FolderPathForRecord(record),
        selectedFolderPath);
}

inline std::vector<AudioRecord> FilterPlaylistRecordsByFolder(const std::vector<AudioRecord>& records,
                                                              bool folderFilterActive,
                                                              std::string_view selectedFolderPath) {
    if (!folderFilterActive || selectedFolderPath.empty()) return records;

    std::vector<AudioRecord> filtered;
    filtered.reserve(records.size());
    for (const AudioRecord& record : records) {
        if (PlaylistRecordMatchesFolderFilter(record, true, selectedFolderPath)) {
            filtered.push_back(record);
        }
    }
    return filtered;
}

inline void FilterPlaylistRecordsByFolderInPlace(std::vector<AudioRecord>& records,
                                                 bool folderFilterActive,
                                                 std::string_view selectedFolderPath) {
    if (!folderFilterActive || selectedFolderPath.empty()) return;
    records.erase(std::remove_if(records.begin(), records.end(),
        [&](const AudioRecord& record) {
            return !PlaylistRecordMatchesFolderFilter(record, true, selectedFolderPath);
        }), records.end());
}
