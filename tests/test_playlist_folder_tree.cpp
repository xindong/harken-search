#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "src/PlaylistFolderTree.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

static int g_failures = 0;

static void check(bool cond, const char* desc) {
    if (cond) {
        std::printf("PASS: %s\n", desc);
    } else {
        std::printf("FAIL: %s\n", desc);
        ++g_failures;
    }
}

static AudioRecord record(int64_t id, const char* filePath, const char* folderPath = "") {
    AudioRecord r;
    r.id = id;
    r.filePath = filePath;
    r.folderPath = folderPath;
    return r;
}

static const FolderInfo* childNamed(const FolderInfo& node, const std::string& name) {
    for (const FolderInfo& child : node.subfolders) {
        if (child.name == name) return &child;
    }
    return nullptr;
}

static int countOccurrences(const std::string& haystack, const std::string& needle) {
    if (needle.empty()) return 0;
    int count = 0;
    size_t pos = 0;
    while ((pos = haystack.find(needle, pos)) != std::string::npos) {
        ++count;
        pos += needle.size();
    }
    return count;
}

namespace fs = std::filesystem;

fs::path Utf8ToPath(const std::string& str);
std::string PathToUtf8(const fs::path& p);

static std::string readTextFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

static std::string sliceAround(const std::string& haystack, const std::string& needle, size_t before, size_t after) {
    const size_t pos = haystack.find(needle);
    if (pos == std::string::npos) return {};
    const size_t start = (pos > before) ? (pos - before) : 0;
    size_t end = pos + needle.size() + after;
    if (end > haystack.size()) end = haystack.size();
    return haystack.substr(start, end - start);
}

int main() {
    const std::vector<AudioRecord> records = {
        record(1,
               "H:/Sound effect/FX/Fire/Scrape_01.wav",
               "h:/sound effect/fx/fire"),
        record(2,
               "H:/Sound effect/FX/Fire/Scrape_02.wav",
               "h:/sound effect/fx/fire"),
        record(3,
               "H:/Sound effect/FX/Whoosh/Whoosh_01.wav",
               "h:/sound effect/fx/whoosh"),
        record(4,
               "H:/Sound effect/Impacts/Hit_01.wav"),
    };

    const FolderInfo tree = BuildPlaylistFolderTreeFromRecords(records);
    check(tree.fileCount == 4, "playlist folder tree root count reflects playlist size");

    const FolderInfo* drive = childNamed(tree, "h:");
    check(drive != nullptr, "playlist folder tree creates drive root");
    const FolderInfo* sound = drive ? childNamed(*drive, "sound effect") : nullptr;
    check(sound != nullptr && sound->displayName == "Sound effect",
          "playlist folder tree preserves raw display case from file paths");
    const FolderInfo* fx = sound ? childNamed(*sound, "fx") : nullptr;
    const FolderInfo* fire = fx ? childNamed(*fx, "fire") : nullptr;
    const FolderInfo* whoosh = fx ? childNamed(*fx, "whoosh") : nullptr;
    check(fire != nullptr && fire->fileCount == 2,
          "playlist folder tree counts direct files in a playlist folder");
    check(whoosh != nullptr && whoosh->fileCount == 1,
          "playlist folder tree keeps sibling folders scoped to the playlist");

    const std::vector<AudioRecord> loadedPage = {records[0], records[1]};
    const std::vector<AudioRecord> fullMembers = records;
    const FolderInfo pageTree = BuildPlaylistFolderTreeFromRecords(loadedPage);
    const FolderInfo fullTree = BuildPlaylistFolderTreeFromCompleteMembers(fullMembers, loadedPage);
    const FolderInfo* pageFx = childNamed(*childNamed(*childNamed(pageTree, "h:"), "sound effect"), "fx");
    const FolderInfo* fullFx = childNamed(*childNamed(*childNamed(fullTree, "h:"), "sound effect"), "fx");
    check(pageFx && childNamed(*pageFx, "whoosh") == nullptr,
          "a loaded page alone omits folders that only exist on later pages");
    check(fullFx && childNamed(*fullFx, "whoosh") != nullptr &&
              childNamed(*fullFx, "whoosh")->fileCount == 1 &&
              fullTree.fileCount == 4,
          "playlist folder tree uses the complete member set, not the loaded page");

    // Folder comparisons use the host's native path grammar. Keep the Windows
    // fixture above for tree display, and use POSIX paths for Mac filtering.
#ifdef _WIN32
    const auto& filterRecords = records;
    const std::string filterRoot = "H:/Sound effect/FX";
#else
    const std::vector<AudioRecord> nativeRecords = {
        record(1, "/Volumes/Sound effect/FX/Fire/Scrape_01.wav"),
        record(2, "/Volumes/Sound effect/FX/Fire/Scrape_02.wav"),
        record(3, "/Volumes/Sound effect/FX/Whoosh/Whoosh_01.wav"),
        record(4, "/Volumes/Sound effect/Impacts/Hit_01.wav"),
    };
    const auto& filterRecords = nativeRecords;
    const std::string filterRoot = "/Volumes/Sound effect/FX";
#endif
    auto fxFiltered = FilterPlaylistRecordsByFolder(filterRecords, true, filterRoot);
    check(fxFiltered.size() == 3, "playlist sidebar folder filter includes descendants");
    auto fireFiltered = FilterPlaylistRecordsByFolder(filterRecords, true, filterRoot + "/Fire");
    check(fireFiltered.size() == 2, "playlist sidebar folder filter matches selected leaf folder");
    auto inactiveFiltered = FilterPlaylistRecordsByFolder(filterRecords, false, filterRoot + "/Fire");
    check(inactiveFiltered.size() == filterRecords.size(), "inactive playlist folder filter keeps all playlist files");

#ifdef _WIN32
    const std::vector<AudioRecord> longPrefixFolderOnly = {
        record(5, "", "\\\\?\\H:\\Sound effect\\Pseudo"),
    };
    const FolderInfo longPrefixTree = BuildPlaylistFolderTreeFromRecords(longPrefixFolderOnly);
    const FolderInfo* longPrefixDrive = childNamed(longPrefixTree, "h:");
    const FolderInfo* longPrefixSound = longPrefixDrive
        ? childNamed(*longPrefixDrive, "sound effect") : nullptr;
    const FolderInfo* longPrefixPseudo = longPrefixSound
        ? childNamed(*longPrefixSound, "pseudo") : nullptr;
    check(longPrefixPseudo != nullptr && longPrefixPseudo->displayName == "Pseudo",
          "folder-only long-path records do not expose the \\\\?\\ prefix in tree labels");
#else
    const FolderInfo nativeFolderOnly = BuildPlaylistFolderTreeFromRecords(
        {record(5, "", "/Volumes/Sound effect/Pseudo")});
    const FolderInfo* nativeVolume = childNamed(nativeFolderOnly, "Volumes");
    const FolderInfo* nativeSound = nativeVolume
        ? childNamed(*nativeVolume, "Sound effect") : nullptr;
    const FolderInfo* nativePseudo = nativeSound
        ? childNamed(*nativeSound, "Pseudo") : nullptr;
    check(nativePseudo != nullptr && nativePseudo->displayName == "Pseudo",
          "folder-only POSIX records preserve display labels");
#endif

    // Evidence class: conversion stub in tests/playlist_folder_tree_path_conversion_stub.cpp.
    // Not the product Utils.cpp converter.
    const std::string unicodeUtf8 = "D:/é³æåº/å°é¢å¢/æå»_01.wav";
    const fs::path unicodePath = Utf8ToPath(unicodeUtf8);
    const std::string unicodeBack = PathToUtf8(unicodePath);
    check(unicodeBack.find("é³æåº") != std::string::npos &&
          unicodeBack.find("å°é¢å¢") != std::string::npos &&
          unicodeBack.find("æå»_01.wav") != std::string::npos,
          "path conversion stub round-trips Unicode path segments (stub evidence, not product converter)");
    const std::string asciiUtf8 = "H:/Sound effect/FX/Fire/Scrape_01.wav";
    check(PathToUtf8(Utf8ToPath(asciiUtf8)).find("Scrape_01.wav") != std::string::npos,
          "path conversion stub round-trips ASCII path filename (stub evidence, not product converter)");


    check(!ShouldRenderPlaylistFolderTreeForRow(7, 7, -1, false),
          "playlist folder tree never expands from the playlist row itself");
    check(!ShouldRenderPlaylistFolderTreeForRow(7, -1, 7, true),
          "playlist folder tree stays hidden in the active playlist view");
    check(!ShouldRenderPlaylistFolderTreeForRow(7, -1, 7, false),
          "stale selected playlist id alone does not expand while another view is active");
    check(ShouldUsePlaylistScopedFolderTree(7, true),
          "left folder tree switches to playlist scope in playlist view");
    check(!ShouldUsePlaylistScopedFolderTree(7, false),
          "folder tree module keeps database scope outside playlist view");
    check(ResolvePlaylistScopedFolderTreePlaylistId(-1, 7, -1, true, true) == 7,
          "left folder tree can recover playlist id from the active tab");
    check(ResolvePlaylistScopedFolderTreePlaylistId(9, 7, -1, true, true) == 9,
          "live playlist id wins for the left folder tree");
    check(ResolvePlaylistScopedFolderTreePlaylistId(9, -1, 9, false, false) == 9,
          "left folder tree keeps playlist scope from the explicit expanded playlist id");
    check(ResolvePlaylistScopedFolderTreePlaylistId(-1, 7, -1, false, false) == -1,
          "tab playlist id is ignored when the active tab is not playlist view");

    const auto sourceRoot = std::filesystem::path(__FILE__).parent_path().parent_path();
    const std::string mainSource = readTextFile(sourceRoot / "src" / "main.cpp");
    const std::string quickBarSource = readTextFile(sourceRoot / "src" / "PlaylistQuickBar.cpp");
    check(!mainSource.empty(), "main.cpp source is readable for playlist sidebar interaction contract");
    check(!quickBarSource.empty(), "PlaylistQuickBar.cpp source is readable for playlist sidebar interaction contract");
    check(mainSource.find("bool preserveExpandedPlaylistFolder = false") == std::string::npos,
          "playlist reload helper no longer exposes an auto-collapse option");
    check(mainSource.find("if (!preserveExpandedPlaylistFolder)") == std::string::npos,
          "playlist reload never collapses the sidebar folder");
    check(mainSource.find("LoadPlaylistFiltered(state, playlistId, resetFolderFilter);\n    state.expandedPlaylistFolderTreePlaylistId = expandedFolderId;") == std::string::npos,
          "playlist navigation does not collapse by writing the default -1 expanded id");
    check(mainSource.find("if (expandedFolderId > 0) {") != std::string::npos &&
          mainSource.find("state.expandedPlaylistFolderTreePlaylistId = expandedFolderId;") != std::string::npos,
          "playlist navigation can still explicitly open the relevant parent folder");
    const std::string clearNeedle = "state.expandedPlaylistFolderTreePlaylistId = -1;";
    check(countOccurrences(mainSource, clearNeedle) == 1,
          "main.cpp clears playlist folder expansion at exactly one site");
    const std::string mainClearSlice = sliceAround(mainSource, clearNeedle, 500, 220);
    check(mainClearSlice.find("if (triClicked)") != std::string::npos &&
          mainClearSlice.find("if (state.expandedPlaylistFolderTreePlaylistId == pl.id)") != std::string::npos,
          "main.cpp expansion clear is in the triClicked already-expanded branch");
    check(countOccurrences(quickBarSource, clearNeedle) == 1,
          "PlaylistQuickBar.cpp clears playlist folder expansion at exactly one site");
    const std::string quickBarClearSlice = sliceAround(quickBarSource, clearNeedle, 900, 260);
    check(quickBarClearSlice.find("PLQ_Del2ndConfirmBtn") != std::string::npos &&
          quickBarClearSlice.find("if (state.selectedPlaylistId == fid)") != std::string::npos,
          "QuickBar expansion clear is on delete second-confirm when the deleted list is current");
    check(mainSource.find("LoadPlaylistFiltered(state, pl.id, false);") != std::string::npos,
          "drag-drop add refresh keeps the expanded playlist folder");
    const std::string bulkNeedle = "LoadPlaylistFiltered(state, state.pendingBulkAddTargetId, false);";
    check(countOccurrences(quickBarSource, bulkNeedle) == 1,
          "QuickBar has exactly one bulk-add refresh that keeps the expanded playlist folder");
    const std::string bulkSlice = sliceAround(quickBarSource, bulkNeedle, 900, 220);
    check(bulkSlice.find("PLQ_BulkConfirmBtn") != std::string::npos &&
          bulkSlice.find("state.viewMode == ViewMode::Playlist") != std::string::npos &&
          bulkSlice.find("state.selectedPlaylistId == state.pendingBulkAddTargetId") != std::string::npos,
          "QuickBar bulk-add refresh runs after confirm when the target is the current Playlist");
    const auto playlistContextAdapterPath =
        std::filesystem::path(__FILE__).parent_path().parent_path() /
        "src" / "AsyncSearchPlaylistContextStateAdapter.cpp";
    std::ifstream playlistContextAdapterInput(playlistContextAdapterPath);
    std::ostringstream playlistContextAdapterBuffer;
    playlistContextAdapterBuffer << playlistContextAdapterInput.rdbuf();
    const std::string playlistContextAdapterSource =
        playlistContextAdapterBuffer.str();
    const auto applicationAdapterPath =
        std::filesystem::path(__FILE__).parent_path().parent_path() /
        "src" / "AsyncSearchResultApplicationAdapter.cpp";
    std::ifstream applicationAdapterInput(applicationAdapterPath);
    std::ostringstream applicationAdapterBuffer;
    applicationAdapterBuffer << applicationAdapterInput.rdbuf();
    const std::string applicationAdapterSource =
        applicationAdapterBuffer.str();
    check(playlistContextAdapterSource.find("HandleAsyncSearchPlaylistContext(") != std::string::npos &&
          playlistContextAdapterSource.find("void applyPlaylistContext(") != std::string::npos &&
          applicationAdapterSource.find("playlistContextAdapter.apply();") != std::string::npos,
          "async playlist results restore playlist quick-bar view context before apply");
    check(mainSource.find("static bool IsPlaylistSystemView(ViewMode mode)") != std::string::npos &&
          mainSource.find("return mode == ViewMode::Playlist || mode == ViewMode::PlaylistLibrary;") != std::string::npos,
          "playlist library and concrete playlist views share one playlist-system predicate");
    check(mainSource.find("navItems[i].mode == ViewMode::PlaylistLibrary && IsPlaylistSystemView(state.viewMode)") != std::string::npos,
          "top navigation highlights Playlists for both sidebar and quick-bar playlist selection");
    check(mainSource.find("ImGui::MenuItem(L(View_Playlists), nullptr, IsPlaylistSystemView(state.viewMode))") != std::string::npos &&
          mainSource.find("ImGui::Selectable((std::string(\"  \") + L(View_Playlists)).c_str(), IsPlaylistSystemView(state.viewMode))") != std::string::npos,
          "menu and sidebar navigation mark the playlist system active in concrete playlist view");
    check(countOccurrences(mainSource, "KeepPlaylistSearchScopeOrEnterDatabase(state);") >= 2,
          "global search commit and realtime search keep playlist scope instead of jumping to database");
    check(mainSource.find("else if (state.viewMode == ViewMode::Database || IsPlaylistSystemView(state.viewMode))") != std::string::npos,
          "clearing realtime search refreshes the selected playlist rather than leaving stale results");
    check(countOccurrences(mainSource, "else if (IsPlaylistSystemView(state.viewMode))") >= 2,
          "refresh shortcuts include concrete playlist views, not only the playlist library");
    check(mainSource.find("enum class FolderTreeSelectionMode") != std::string::npos,
          "folder tree renderer exposes one explicit database-vs-playlist activation mode");
    check(mainSource.find("static void RenderPlaylistFolderTreeNode(") == std::string::npos,
          "playlist folder tree no longer diverges behind a dedicated renderer");
    check(mainSource.find("RenderFolderTreeNode(state, folder, folderSearch, FolderTreeSelectionMode::PlaylistFilter);") != std::string::npos,
          "playlist-scoped folder tree reuses the database folder tree renderer");
    check(mainSource.find("RenderFolderTreeNode(state, folder, folderSearch, FolderTreeSelectionMode::DatabaseFilter);") != std::string::npos,
          "database folder tree keeps using the same canonical folder renderer");

    const std::string asyncWorkerHeader = readTextFile(sourceRoot / "src" / "AsyncWorker.h");
    check(asyncWorkerHeader.find("int viewMode = 0;") != std::string::npos &&
          asyncWorkerHeader.find("int64_t playlistId = -1;") != std::string::npos &&
          asyncWorkerHeader.find("bool playlistAggregatedView = false;") != std::string::npos &&
          asyncWorkerHeader.find("bool folderFilterActive = false;") != std::string::npos &&
          asyncWorkerHeader.find("std::string selectedFolderPath;") != std::string::npos,
          "async search results carry playlist quick-bar context fields");

    const std::string asyncWorkerSource = readTextFile(sourceRoot / "src" / "AsyncWorker.cpp");
    check(asyncWorkerSource.find("result.viewMode = req.viewMode;") != std::string::npos &&
          asyncWorkerSource.find("result.playlistId = req.playlistId;") != std::string::npos &&
          asyncWorkerSource.find("result.playlistAggregatedView = req.playlistAggregatedView;") != std::string::npos &&
          asyncWorkerSource.find("result.folderFilterActive = req.folderFilterActive;") != std::string::npos &&
          asyncWorkerSource.find("result.selectedFolderPath = req.selectedFolderPath;") != std::string::npos,
          "async refresh result copies playlist quick-bar context from the request");
    check(asyncWorkerSource.find("followup.viewMode = req.viewMode;") != std::string::npos &&
          asyncWorkerSource.find("followup.playlistId = req.playlistId;") != std::string::npos &&
          asyncWorkerSource.find("followup.playlistAggregatedView = req.playlistAggregatedView;") != std::string::npos &&
          asyncWorkerSource.find("followup.folderFilterActive = req.folderFilterActive;") != std::string::npos &&
          asyncWorkerSource.find("followup.selectedFolderPath = req.selectedFolderPath;") != std::string::npos,
          "count follow-up preserves playlist quick-bar context for the same request");

    std::printf("\n=== Playlist Folder Tree: %d failure(s) ===\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
