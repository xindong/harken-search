#include "Database.h"
#include "PlaylistCoverScope.h"
#include "PlaylistFolderTree.h"
#include "PlaylistScopeCounts.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

constexpr const char* kFixtureRoot =
    "D:/Harken-QA/BUG-20261029-PLAYLIST-COVER-WALL/test-databases/playlist-scope";

int g_failures = 0;

bool Expect(bool condition, const char* message) {
    std::cout << (condition ? "PASS: " : "FAIL: ") << message << std::endl;
    if (!condition) ++g_failures;
    return condition;
}

void TouchFile(const fs::path& path) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    const char bytes[] = "RIFF....WAVEfmt ";
    out.write(bytes, sizeof(bytes) - 1);
}

std::string SqlQuote(const std::string& value) {
    std::string out(1, char(39));
    for (char ch : value) {
        if (ch == 39) {
            out += char(39);
            out += char(39);
        } else {
            out += ch;
        }
    }
    out += char(39);
    return out;
}

const aura::playlist::PlaylistCoverGroup* FindGroup(
    const std::vector<aura::playlist::PlaylistCoverGroup>& groups,
    const std::string& key) {
    for (const auto& group : groups) {
        if (group.key == key) return &group;
    }
    return nullptr;
}

}  // namespace

int main() {
    const fs::path root = fs::u8path(kFixtureRoot);
    const fs::path dbPath = root / "playlist-scope.db";
    const fs::path audio = root / "audio";
    fs::create_directories(audio);
    std::error_code ec;
    fs::remove(dbPath, ec);
    fs::remove(fs::path(dbPath.native() + L"-wal"), ec);
    fs::remove(fs::path(dbPath.native() + L"-shm"), ec);

    Database db;
    if (!db.open(dbPath.u8string(), false)) {
        std::cout << "FAIL: open " << dbPath.u8string() << " err=" << db.getLastError() << std::endl;
        return 1;
    }
    db.stopCoverPhashBackfillWorker();

    struct Row {
        const char* rel;
        const char* cover;
    };
    const Row rows[] = {
        {"fx/fire/scrape_01.wav", "F|PATH|D:/fx/fire"},
        {"fx/fire/scrape_02.wav", "F|PATH|D:/fx/fire"},
        {"fx/whoosh/whoosh_01.wav", "F|PATH|D:/fx/whoosh"},
        {"impacts/hit_01.wav", "F|PATH|D:/impacts"},
    };

    db.executeRawQuery("BEGIN;");
    std::vector<int64_t> ids;
    for (const auto& row : rows) {
        const fs::path file = audio / row.rel;
        TouchFile(file);
        const std::string sql =
            "INSERT INTO audio_files(file_path, file_name, folder_path, folder_cover_key, search_data) VALUES(" +
            SqlQuote(file.u8string()) + ", " + SqlQuote(file.filename().u8string()) + ", " +
            SqlQuote(file.parent_path().u8string()) + ", " + SqlQuote(row.cover) + ", " +
            SqlQuote(file.stem().u8string()) + ");";
        if (!db.executeRawQuery(sql.c_str()).success) {
            std::cout << "FAIL: insert " << row.rel << " err=" << db.getLastError() << std::endl;
            db.executeRawQuery("ROLLBACK;");
            return 1;
        }
        ids.push_back(db.executeRawQuery("SELECT last_insert_rowid();").success ?
                      (int64_t)0 : (int64_t)0);
    }
    db.executeRawQuery("COMMIT;");

    const auto idRows = db.executeRawQuery("SELECT id FROM audio_files ORDER BY id;");
    if (!idRows.success || idRows.rows.size() != 4) {
        std::cout << "FAIL: expected 4 audio_files" << std::endl;
        return 1;
    }
    ids.clear();
    for (const auto& row : idRows.rows) {
        ids.push_back(std::stoll(row[0]));
    }
    Expect(db.executeRawQuery(
        ("UPDATE audio_files SET embedded_cover_key = 'E|playlist-test' WHERE id = " +
         std::to_string(ids[0]) + ";").c_str()).success,
        "set embedded cover on first playlist member");
    const std::string folderImageSource = (audio / "fx/fire/folder-cover.png").u8string();
    Expect(db.executeRawQuery(
        ("UPDATE audio_files SET folder_cover_src_path = " + SqlQuote(folderImageSource) +
         " WHERE id = " + std::to_string(ids[1]) + ";").c_str()).success,
        "set folder image source on the second member of a shared cover group");
    const auto retainsFolderItem = [&](const auto& coverGroups) {
        const auto items = aura::playlist::BuildPlaylistCoverItemsFromGroups(coverGroups);
        return std::any_of(items.begin(), items.end(), [&](const auto& item) {
            return item.key == "F|PATH|D:/fx/fire" &&
                item.kind == CoverModule::CoverKind::FolderCover &&
                item.folderPathUtf8 == (audio / "fx/fire").u8string() &&
                item.folderCoverSrcPathUtf8 == folderImageSource;
        });
    };

    const int64_t playlistId = db.createPlaylist("scope-four");
    Expect(playlistId > 0, "create ordinary playlist");
    for (int64_t id : ids) {
        Expect(db.addToPlaylist(playlistId, id), "add member to ordinary playlist");
    }

    const SearchFilter empty;
    const int64_t completeCount = db.searchPlaylistFileCount(playlistId, empty);
    Expect(completeCount == 4, "complete filtered count is 4 with page size 2");
    const auto page1 = db.searchPlaylistFilesPage(playlistId, empty, 2, 0);
    const auto page2 = db.searchPlaylistFilesPage(playlistId, empty, 2, 2);
    Expect((int)page1.size() == 2, "first page loads 2 rows");
    Expect((int)page2.size() == 2, "second page loads remaining rows");
    Expect(aura::playlist::ResolvePlaylistResultTitleCount(completeCount, (int64_t)page1.size()) == 4,
           "title count stays on complete matches, not the loaded page");

    const auto completeMembers = db.getPlaylistFiles(playlistId);
    Expect((int)completeMembers.size() == 4, "unfiltered complete members are 4");
    const FolderInfo pageTree = BuildPlaylistFolderTreeFromRecords(page1);
    const FolderInfo fullTree = BuildPlaylistFolderTreeFromCompleteMembers(completeMembers, page1);
    Expect(fullTree.fileCount == 4, "complete member tree keeps later-page files");
    Expect(pageTree.fileCount == 2, "loaded page tree is incomplete");

    const auto coverKeys = db.searchPlaylistCoverKeys(playlistId, empty);
    bool ordinaryEmbeddedSource = false;
    for (const auto& record : coverKeys) {
        if (record.embeddedCoverKey == "E|playlist-test" &&
            record.filePath == (audio / rows[0].rel).u8string()) {
            ordinaryEmbeddedSource = true;
        }
    }
    Expect(ordinaryEmbeddedSource, "ordinary playlist cover query retains embedded source path");
    Expect(std::any_of(coverKeys.begin(), coverKeys.end(), [&](const auto& record) {
        return record.folderCoverKey == "F|PATH|D:/fx/fire" &&
            record.folderCoverSrcPath == folderImageSource;
    }), "ordinary playlist cover query retains folder image source path");
    const auto groups = aura::playlist::BuildPlaylistCoverGroupsFromCompleteMembers(coverKeys, page1);
    Expect(retainsFolderItem(groups), "ordinary playlist CoverItem retains folder image source path");
    Expect(FindGroup(groups, "F|PATH|D:/fx/fire") != nullptr &&
               FindGroup(groups, "F|PATH|D:/fx/fire")->hitCount == 2,
           "complete cover groups keep the fire folder");
    Expect(FindGroup(groups, "F|PATH|D:/fx/whoosh") != nullptr,
           "complete cover groups keep the later-page whoosh folder");

    SearchFilter whoosh;
    whoosh.folderPath = (audio / "fx/whoosh").u8string();
    const int64_t whooshCount = db.searchPlaylistFileCount(playlistId, whoosh);
    Expect(whooshCount == 1, "folder filter shrinks playlist count");
    const auto whooshKeys = db.searchPlaylistCoverKeys(playlistId, whoosh);
    const auto whooshGroups = aura::playlist::BuildPlaylistCoverGroupsFromCompleteMembers(whooshKeys, {});
    Expect(FindGroup(whooshGroups, "F|PATH|D:/fx/whoosh") != nullptr &&
               FindGroup(whooshGroups, "F|PATH|D:/fx/fire") == nullptr,
           "filtered cover keys follow the same filter as count");

    const int64_t folderId = db.createPlaylist("agg-folder");
    Expect(folderId > 0 && db.markAsFolder(folderId), "create aggregated parent folder");
    const int64_t childId = db.createSubPlaylist(folderId, "agg-child");
    Expect(childId > 0, "create aggregated child playlist");
    for (int64_t id : ids) {
        Expect(db.addToPlaylist(childId, id), "add member to aggregated child");
    }
    Expect(db.isFolder(folderId), "parent is a folder");
    Expect(db.searchAggregatedPlaylistFileCount(folderId, empty) == 4,
           "aggregated complete count is 4");
    const auto aggKeys = db.searchAggregatedPlaylistCoverKeys(folderId, empty);
    bool aggregatedEmbeddedSource = false;
    for (const auto& record : aggKeys) {
        if (record.embeddedCoverKey == "E|playlist-test" &&
            record.filePath == (audio / rows[0].rel).u8string()) {
            aggregatedEmbeddedSource = true;
        }
    }
    Expect(aggregatedEmbeddedSource, "aggregated playlist cover query retains embedded source path");
    Expect(std::any_of(aggKeys.begin(), aggKeys.end(), [&](const auto& record) {
        return record.folderCoverKey == "F|PATH|D:/fx/fire" &&
            record.folderCoverSrcPath == folderImageSource;
    }), "aggregated playlist cover query retains folder image source path");
    const auto aggGroups = aura::playlist::BuildPlaylistCoverGroupsFromCompleteMembers(aggKeys, page1);
    Expect(retainsFolderItem(aggGroups), "aggregated playlist CoverItem retains folder image source path");
    Expect(FindGroup(aggGroups, "F|PATH|D:/fx/fire") != nullptr &&
               FindGroup(aggGroups, "F|PATH|D:/fx/whoosh") != nullptr,
           "aggregated cover keys keep both groups");

    const fs::path inheritedFolder = audio / "inherited";
    const fs::path representative = inheritedFolder / "source.wav";
    std::vector<int64_t> inheritedIds;
    for (const char* name : {"member-a.wav", "member-b.wav", "source.wav"}) {
        const fs::path file = inheritedFolder / name;
        TouchFile(file);
        const std::string key = file == representative ? "E|inherited-playlist" : "";
        const std::string sql =
            "INSERT INTO audio_files(file_path,file_name,folder_path,embedded_cover_key) VALUES(" +
            SqlQuote(file.u8string()) + "," + SqlQuote(name) + "," +
            SqlQuote(inheritedFolder.u8string()) + "," + SqlQuote(key) + ");";
        if (!Expect(db.executeRawQuery(sql.c_str()).success, "insert inherited-cover fixture")) return 1;
        const auto inserted = db.executeRawQuery("SELECT last_insert_rowid();");
        if (!Expect(inserted.success && inserted.rows.size() == 1, "read inherited fixture identity")) return 1;
        inheritedIds.push_back(std::stoll(inserted.rows[0][0]));
    }
    Expect(db.propagateUniqueFolderCovers(0) >= 2, "propagate embedded cover to two unillustrated folder members");
    db.syncCoverRegistry();
    const int64_t inheritedPlaylist = db.createPlaylist("inherited-only");
    const int64_t inheritedParent = db.createPlaylist("inherited-aggregate");
    Expect(db.markAsFolder(inheritedParent), "create inherited aggregate folder");
    const int64_t inheritedChildA = db.createSubPlaylist(inheritedParent, "child-a");
    const int64_t inheritedChildB = db.createSubPlaylist(inheritedParent, "child-b");
    Expect(db.addToPlaylist(inheritedPlaylist, inheritedIds[0]), "ordinary playlist contains only an inheriting member");
    Expect(db.addToPlaylist(inheritedChildA, inheritedIds[0]) &&
           db.addToPlaylist(inheritedChildB, inheritedIds[0]) &&
           db.addToPlaylist(inheritedChildB, inheritedIds[1]), "aggregate contains two inheriting members and a duplicate membership");
    const auto hasInheritedArtwork = [&](const std::vector<AudioRecord>& records, int64_t count) {
        const auto items = aura::playlist::BuildPlaylistCoverItemsFromGroups(
            aura::playlist::BuildPlaylistCoverGroupsFromRecords(records));
        return items.size() == 1 && items[0].kind == CoverModule::CoverKind::EmbeddedCover &&
            items[0].key == "E|inherited-playlist" && items[0].hitCount == count &&
            items[0].filePathUtf8 == representative.u8string();
    };
    const auto inheritedRecords = db.searchPlaylistCoverKeys(inheritedPlaylist, empty);
    Expect(inheritedRecords.size() == 1 && inheritedRecords[0].filePath == (inheritedFolder / "member-a.wav").u8string(),
           "representative resolution preserves original playlist member path and count");
    Expect(hasInheritedArtwork(inheritedRecords, 1), "ordinary inherited-only cover resolves audio outside the playlist");
    Expect(hasInheritedArtwork(db.searchAggregatedPlaylistCoverKeys(inheritedParent, empty), 2),
           "aggregate inherited-only cover resolves external audio without double counting membership");
    Expect(db.executeRawQuery("DELETE FROM cover_cache_registry WHERE cover_key = 'E|inherited-playlist';").success,
           "simulate missing representative registry entry");
    Expect(hasInheritedArtwork(db.searchPlaylistCoverKeys(inheritedPlaylist, empty), 1) &&
           hasInheritedArtwork(db.searchAggregatedPlaylistCoverKeys(inheritedParent, empty), 2),
           "both cover queries recover source from existing audio records without a registry entry");
    Expect(db.addToPlaylist(inheritedPlaylist, inheritedIds[2]), "append actual representative after inherited member");
    Expect(hasInheritedArtwork(db.searchPlaylistCoverKeys(inheritedPlaylist, empty), 2),
           "inherited member preceding representative still selects embedded loading");

    db.close();
    std::cout << "FAILURES " << g_failures << std::endl;
    return g_failures == 0 ? 0 : 1;
}
