#include "Database.h"
#include "AppState.h"
#include "PathUtils.h"
#include <cstdio>
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iterator>

namespace fs = std::filesystem;

static int s_pass = 0, s_fail = 0;
#define EXPECT(cond, msg) do { \
    if (cond) { ++s_pass; std::printf("PASS: %s\n", msg); } \
    else { ++s_fail; std::printf("FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); } \
} while (0)

static std::string makeTempDbPath(const char* tag) {
    auto dir = fs::temp_directory_path() / "auro_sub_playlist_test";
    fs::create_directories(dir);
    auto p = dir / (std::string("test_") + tag + ".db");
    std::error_code ec;
    fs::remove(p, ec);
    fs::remove(fs::path(p.string() + "-wal"), ec);
    fs::remove(fs::path(p.string() + "-shm"), ec);
    return p.string();
}

static std::string readRepoFile(const char* path) {
    const fs::path repoRoot = fs::path(__FILE__).parent_path().parent_path();
    std::ifstream in(repoRoot / path, std::ios::binary);
    if (!in) return {};
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

static void test_schema_migration_adds_columns() {
    auto path = makeTempDbPath("schema");
    Database db;
    bool ok = db.open(path);
    EXPECT(ok, "Database opens");
    int64_t pid = db.createPlaylist("Top", "");
    EXPECT(pid > 0, "createPlaylist returns id");
    auto all = db.getAllPlaylists();
    EXPECT(all.size() == 1, "getAllPlaylists returns 1");
    EXPECT(all[0].parent_id == -1, "Default parent_id is -1 (NULL sentinel)");
    EXPECT(all[0].is_folder == false, "Default is_folder is false");
    db.close();
}

static void test_create_child_under_parent() {
    auto path = makeTempDbPath("create_child");
    Database db;
    db.open(path);
    int64_t parent = db.createPlaylist("Project A", "");
    EXPECT(db.markAsFolder(parent), "markAsFolder succeeds");
    int64_t child = db.createSubPlaylist(parent, "Street", "");
    EXPECT(child > 0, "createSubPlaylist returns id");
    auto kids = db.getChildrenOf(parent);
    EXPECT(kids.size() == 1, "getChildrenOf returns 1 child");
    EXPECT(kids[0].id == child, "child id matches");
    EXPECT(kids[0].parent_id == parent, "child parent_id matches");
    EXPECT(db.isFolder(parent), "parent is folder");
    EXPECT(!db.isFolder(child), "child is not folder");

    // sibling name uniqueness
    int64_t dup = db.createSubPlaylist(parent, "Street", "");
    EXPECT(dup == -1, "duplicate sibling name rejected");

    // creating sub under non-folder must fail
    int64_t orphan = db.createPlaylist("Orphan", "");
    int64_t illegal = db.createSubPlaylist(orphan, "x", "");
    EXPECT(illegal == -1, "createSubPlaylist on non-folder parent rejected");

    db.close();
}

static void test_folder_rejects_add_file() {
    auto path = makeTempDbPath("reject_add");
    Database db;
    db.open(path);
    int64_t parent = db.createPlaylist("ProjectA", "");
    db.markAsFolder(parent);
    // file_id=999 does not exist; addToPlaylist on a folder must reject before any SQL
    bool added = db.addToPlaylist(parent, 999);
    EXPECT(!added, "addToPlaylist on folder must fail");
    db.close();
}

static void test_aggregated_files_dedup() {
    auto path = makeTempDbPath("aggregate");
    Database db;
    db.open(path);

    // Insert 3 audio files, then retrieve their DB ids via getFileByPath
    AudioRecord r1; r1.filePath = "/agg_test/a.wav"; r1.fileName = "a.wav";
    AudioRecord r2; r2.filePath = "/agg_test/b.wav"; r2.fileName = "b.wav";
    AudioRecord r3; r3.filePath = "/agg_test/c.wav"; r3.fileName = "c.wav";
    db.addFile(r1);
    db.addFile(r2);
    db.addFile(r3);
    int64_t id1 = db.getFileByPath("/agg_test/a.wav").id;
    int64_t id2 = db.getFileByPath("/agg_test/b.wav").id;
    int64_t id3 = db.getFileByPath("/agg_test/c.wav").id;
    EXPECT(id1 > 0 && id2 > 0 && id3 > 0, "3 audio files inserted");

    int64_t parent = db.createPlaylist("Folder", "");
    db.markAsFolder(parent);
    int64_t s1 = db.createSubPlaylist(parent, "Sub1", "");
    int64_t s2 = db.createSubPlaylist(parent, "Sub2", "");
    db.addToPlaylist(s1, id1);
    db.addToPlaylist(s1, id2);
    db.addToPlaylist(s2, id2);  // overlap with s1
    db.addToPlaylist(s2, id3);

    auto files = db.getAggregatedPlaylistFiles(parent);
    EXPECT(files.size() == 3, "aggregated returns 3 unique files (dedup)");

    // Non-folder returns empty for aggregation
    auto orphanResult = db.getAggregatedPlaylistFiles(s1);
    EXPECT(orphanResult.empty(), "non-folder returns empty for aggregation");

    db.close();
}

static void test_delete_folder_cascades() {
    auto path = makeTempDbPath("cascade");
    Database db;
    db.open(path);
    int64_t parent = db.createPlaylist("F", "");
    db.markAsFolder(parent);
    int64_t s1 = db.createSubPlaylist(parent, "s1", "");
    int64_t s2 = db.createSubPlaylist(parent, "s2", "");
    EXPECT(s1 > 0 && s2 > 0, "two children created");
    EXPECT(db.deletePlaylist(parent), "deletePlaylist on folder succeeds");
    auto all = db.getAllPlaylists();
    EXPECT(all.empty(), "all playlists removed (parent + 2 children)");
    db.close();
}

static void test_promote_orphan_with_files() {
    auto path = makeTempDbPath("promote");
    Database db;
    db.open(path);

    AudioRecord r1; r1.filePath = "/promote_test/a.wav"; r1.fileName = "a.wav";
    AudioRecord r2; r2.filePath = "/promote_test/b.wav"; r2.fileName = "b.wav";
    db.addFile(r1);
    db.addFile(r2);
    int64_t f1 = db.getFileByPath("/promote_test/a.wav").id;
    int64_t f2 = db.getFileByPath("/promote_test/b.wav").id;
    EXPECT(f1 > 0 && f2 > 0, "two audio files inserted");

    int64_t orphan = db.createPlaylist("MyList", "");
    db.addToPlaylist(orphan, f1);
    db.addToPlaylist(orphan, f2);
    EXPECT(db.getPlaylistFileCount(orphan) == 2, "orphan has 2 files initially");

    int64_t defaultChild = db.promoteOrphanToFolderWithDefaultChild(orphan, "Original");
    EXPECT(defaultChild > 0, "promote returns new default child id");
    EXPECT(db.isFolder(orphan), "orphan promoted to folder");

    auto kids = db.getChildrenOf(orphan);
    EXPECT(kids.size() == 1 && kids[0].id == defaultChild, "single default child created");

    EXPECT(db.getPlaylistFileCount(defaultChild) == 2, "files migrated to default child");
    EXPECT(db.getPlaylistFileCount(orphan) == 0, "parent (now folder) has no direct files");

    // Calling promote on an already-folder must fail
    int64_t doublePromote = db.promoteOrphanToFolderWithDefaultChild(orphan, "Again");
    EXPECT(doublePromote == -1, "promoting an already-folder is rejected");

    db.close();
}

static void test_move_sub_to_other_parent() {
    auto path = makeTempDbPath("move");
    Database db;
    db.open(path);
    int64_t pa = db.createPlaylist("A", ""); db.markAsFolder(pa);
    int64_t pb = db.createPlaylist("B", ""); db.markAsFolder(pb);
    int64_t s = db.createSubPlaylist(pa, "x", "");
    EXPECT(s > 0, "sub created under A");
    EXPECT(db.moveSubPlaylistToParent(s, pb), "move to other parent succeeds");
    EXPECT(db.getChildrenOf(pa).empty(), "old parent has no children");
    auto bKids = db.getChildrenOf(pb);
    EXPECT(bKids.size() == 1 && bKids[0].id == s, "new parent has 1 child");

    EXPECT(db.moveSubPlaylistToParent(s, -1), "move to orphan (parent=-1) succeeds");
    EXPECT(db.getChildrenOf(pb).empty(), "no longer under pb");
    auto all = db.getAllPlaylists();
    bool sIsOrphan = false;
    for (const auto& p : all) {
        if (p.id == s) {
            sIsOrphan = (p.parent_id == -1) && !p.is_folder;
            break;
        }
    }
    EXPECT(sIsOrphan, "moved sub is now orphan with parent_id=-1");

    // Negative cases
    EXPECT(!db.moveSubPlaylistToParent(pa, pb), "cannot move folder under another folder");
    EXPECT(!db.moveSubPlaylistToParent(s, 99999), "cannot move to non-existent parent");

    db.close();
}

static void test_import_reaper_file_list_to_playlist() {
    auto path = makeTempDbPath("reaper_import");
    Database db;
    db.open(path);

    AudioRecord r1; r1.filePath = "C:\\Sfx\\whoosh one.wav"; r1.fileName = "whoosh one.wav";
    AudioRecord r2; r2.filePath = "C:\\Sfx\\hits\\impact.wav"; r2.fileName = "impact.wav";
    db.addFile(r1);
    db.addFile(r2);

    auto listPath = fs::temp_directory_path() / "auro_sub_playlist_test" / "sample.ReaperFileList";
    auto diskOnlyPath = fs::temp_directory_path() / "auro_sub_playlist_test" / "disk_only.wav";
    {
        std::ofstream wav(diskOnlyPath, std::ios::binary | std::ios::trunc);
        std::string payload(256, '\0');
        wav.write(payload.data(), (std::streamsize)payload.size());
    }
    {
        std::ofstream out(listPath, std::ios::binary | std::ios::trunc);
        out << "PATH \"C:\\Sfx\"\n";
        out << "FILE \"C:\\Sfx\\whoosh one.wav\" 100 0 1 0\n";
        out << "DATA \"t:whoosh one.wav\" g:Whoosh\n";
        out << "FILE \"C:\\Sfx\\whoosh one.wav\" 100 0 1 0\n";
        out << "FILE \"hits\\impact.wav\" 100 0 1 0\n";
        out << "FILE \"" << diskOnlyPath.string() << "\" 256 0 1 0\n";
        out << "FILE \"C:\\Sfx\\missing.wav\" 100 0 1 0\n";
    }

    int progressCalls = 0;
    int finalProgress = 0;
    int finalTotal = 0;
    bool committedProgress = false;
    auto result = db.importReaperFileListToPlaylist(
        listPath.string(),
        "Reaper Import",
        [&](int current, int total, const std::string&, bool dataCommitted) {
            ++progressCalls;
            finalProgress = current;
            finalTotal = total;
            committedProgress = committedProgress || dataCommitted;
        });
    EXPECT(result.error.empty(), "Reaper import succeeds");
    EXPECT(result.playlistId > 0, "Reaper import creates playlist");
    EXPECT(result.parsedFiles == 5, "Reaper import counts FILE rows");
    EXPECT(result.uniqueFiles == 4, "Reaper import de-duplicates repeated FILE rows");
    EXPECT(result.duplicateInputFiles == 1, "Reaper import reports duplicate input rows");
    EXPECT(result.matchedFiles == 3, "Reaper import returns existing and newly indexed Aris files");
    EXPECT(result.indexedFiles == 1, "Reaper import indexes existing-on-disk files into Aris DB");
    EXPECT(result.addedFiles == 3, "Reaper import adds imported files to playlist");
    EXPECT(result.missingFiles == 1, "Reaper import reports missing paths");
    EXPECT(result.missingOnDiskFiles == 1, "Reaper import reports paths missing on disk");
    EXPECT(progressCalls > 0, "Reaper import reports progress");
    EXPECT(committedProgress, "Reaper import reports committed progress");
    EXPECT(finalProgress == result.uniqueFiles && finalTotal == result.uniqueFiles,
           "Reaper import final progress reaches deduped path total");
    EXPECT(db.getPlaylistFileCount(result.playlistId) == 3, "imported playlist has three files");
    EXPECT(db.getFileByPath(diskOnlyPath.string()).id > 0, "disk-only Reaper file is inserted into audio_files");

    auto files = db.getPlaylistFiles(result.playlistId);
    EXPECT(files.size() == 3, "imported playlist files can be read");
    EXPECT(files.size() == 3 && files[0].filePath == "C:\\Sfx\\whoosh one.wav",
           "Reaper import preserves source order for first match");
    EXPECT(files.size() == 3 && files[1].filePath == "C:\\Sfx\\hits\\impact.wav",
           "Reaper import resolves relative FILE path against PATH row");
    EXPECT(files.size() == 3 && files[2].filePath == diskOnlyPath.string(),
           "Reaper import keeps newly indexed disk file in playlist order");

    db.close();
}

static void test_playlist_files_page_preserves_order_and_count() {
    auto path = makeTempDbPath("playlist_page");
    Database db;
    db.open(path);

    constexpr int kTotal = 3001;
    std::vector<int64_t> ids;
    ids.reserve(kTotal);
    for (int i = 0; i < kTotal; ++i) {
        AudioRecord r;
        r.fileName = "paged_" + std::to_string(i) + ".wav";
        r.filePath = "/playlist_page/" + r.fileName;
        r.extension = ".wav";
        db.addFile(r);
        ids.push_back(db.getFileByPath(r.filePath).id);
    }

    int64_t playlist = db.createPlaylist("Large Page Test", "");
    EXPECT(playlist > 0, "large playlist created");
    for (int64_t id : ids) {
        db.addToPlaylist(playlist, id);
    }

    EXPECT(db.getPlaylistFileCount(playlist) == kTotal,
           "large playlist count uses playlist_items total");

    auto firstPage = db.getPlaylistFilesPage(playlist, 500, 0);
    EXPECT(firstPage.size() == 500, "first playlist page returns requested page size");
    EXPECT(!firstPage.empty() && firstPage.front().filePath == "/playlist_page/paged_0.wav",
           "first playlist page starts at position 0");
    EXPECT(firstPage.size() == 500 && firstPage.back().filePath == "/playlist_page/paged_499.wav",
           "first playlist page preserves position order");

    auto secondPage = db.getPlaylistFilesPage(playlist, 500, 500);
    EXPECT(secondPage.size() == 500, "second playlist page returns requested page size");
    EXPECT(!secondPage.empty() && secondPage.front().filePath == "/playlist_page/paged_500.wav",
           "second playlist page starts at requested offset");

    auto tailPage = db.getPlaylistFilesPage(playlist, 500, 3000);
    EXPECT(tailPage.size() == 1, "tail playlist page returns remaining row");
    EXPECT(tailPage.size() == 1 && tailPage[0].filePath == "/playlist_page/paged_3000.wav",
           "tail playlist page preserves final position");

    db.close();
}

static void test_aggregated_playlist_files_page_dedups_and_counts() {
    auto path = makeTempDbPath("aggregated_page");
    Database db;
    db.open(path);

    std::vector<int64_t> ids;
    for (int i = 0; i < 6; ++i) {
        AudioRecord r;
        r.fileName = "agg_page_" + std::to_string(i) + ".wav";
        r.filePath = "/aggregated_page/" + r.fileName;
        r.extension = ".wav";
        db.addFile(r);
        ids.push_back(db.getFileByPath(r.filePath).id);
    }

    int64_t folder = db.createPlaylist("Folder Page Test", "");
    EXPECT(db.markAsFolder(folder), "aggregated page parent marked as folder");
    int64_t childA = db.createSubPlaylist(folder, "A", "");
    int64_t childB = db.createSubPlaylist(folder, "B", "");
    EXPECT(childA > 0 && childB > 0, "aggregated page children created");

    db.addToPlaylist(childA, ids[0]);
    db.addToPlaylist(childA, ids[1]);
    db.addToPlaylist(childA, ids[2]);
    db.addToPlaylist(childB, ids[2]);
    db.addToPlaylist(childB, ids[3]);
    db.addToPlaylist(childB, ids[4]);
    db.addToPlaylist(childB, ids[5]);

    EXPECT(db.getAggregatedPlaylistFileCount(folder) == 6,
           "aggregated playlist count de-duplicates shared file ids");

    auto page = db.getAggregatedPlaylistFilesPage(folder, 3, 2);
    EXPECT(page.size() == 3, "aggregated playlist page returns requested slice");
    EXPECT(page.size() == 3 && page[0].filePath == "/aggregated_page/agg_page_2.wav",
           "aggregated playlist page starts at deduped offset");
    EXPECT(page.size() == 3 && page[2].filePath == "/aggregated_page/agg_page_4.wav",
           "aggregated playlist page preserves stable deduped ordering");

    db.close();
}

static void test_all_playlist_files_page_dedups_and_counts() {
    auto path = makeTempDbPath("all_playlist_page");
    Database db;
    db.open(path);

    std::vector<int64_t> ids;
    for (int i = 0; i < 5; ++i) {
        AudioRecord r;
        r.fileName = "all_playlist_" + std::to_string(i) + ".wav";
        r.filePath = "/all_playlist/" + r.fileName;
        r.extension = ".wav";
        db.addFile(r);
        ids.push_back(db.getFileByPath(r.filePath).id);
    }

    int64_t a = db.createPlaylist("A", "");
    int64_t b = db.createPlaylist("B", "");
    EXPECT(a > 0 && b > 0, "all playlist page playlists created");
    db.addToPlaylist(a, ids[0]);
    db.addToPlaylist(a, ids[1]);
    db.addToPlaylist(a, ids[2]);
    db.addToPlaylist(b, ids[2]);
    db.addToPlaylist(b, ids[3]);
    db.addToPlaylist(b, ids[4]);

    EXPECT(db.getAllPlaylistFileCount() == 5,
           "all playlist library count de-duplicates shared file ids");

    auto firstPage = db.getAllPlaylistFilesPage(2, 0);
    EXPECT(firstPage.size() == 2, "all playlist library first page returns requested size");
    EXPECT(firstPage.size() == 2 && firstPage[0].filePath == "/all_playlist/all_playlist_0.wav",
           "all playlist library first page starts at lowest file id");
    EXPECT(firstPage.size() == 2 && firstPage[1].filePath == "/all_playlist/all_playlist_1.wav",
           "all playlist library first page preserves stable id order");

    auto secondPage = db.getAllPlaylistFilesPage(2, 2);
    EXPECT(secondPage.size() == 2, "all playlist library second page returns requested size");
    EXPECT(secondPage.size() == 2 && secondPage[0].filePath == "/all_playlist/all_playlist_2.wav",
           "all playlist library second page starts at deduped offset");

    auto tailPage = db.getAllPlaylistFilesPage(2, 4);
    EXPECT(tailPage.size() == 1, "all playlist library tail page returns remaining row");
    EXPECT(tailPage.size() == 1 && tailPage[0].filePath == "/all_playlist/all_playlist_4.wav",
           "all playlist library tail page preserves final row");

    db.close();
}

static void test_playlist_scoped_search_filters_before_paging() {
    auto path = makeTempDbPath("playlist_scoped_search");
    Database db;
    db.open(path);

    std::vector<int64_t> ids;
    for (const char* name : {"alpha_plain.wav", "needle_first.wav", "plain_beta.wav", "needle_second.wav"}) {
        AudioRecord r;
        r.fileName = name;
        r.folderPath = std::string(name) == "needle_first.wav"
            ? "/playlist_scoped/Archive" : "/playlist_scoped/Current";
        r.filePath = r.folderPath + "/" + name;
        r.extension = ".wav";
        r.keywords = name;
        db.addFile(r);
        ids.push_back(db.getFileByPath(r.filePath).id);
    }

    int64_t playlist = db.createPlaylist("Scoped Search", "");
    EXPECT(playlist > 0, "scoped search playlist created");
    for (int64_t id : ids) db.addToPlaylist(playlist, id);

    SearchFilter filter;
    filter.keyword = "needle";
    EXPECT(db.searchPlaylistFileCount(playlist, filter) == 2,
           "playlist scoped search count applies keyword before paging");
    auto first = db.searchPlaylistFilesPage(playlist, filter, 1, 0);
    auto second = db.searchPlaylistFilesPage(playlist, filter, 1, 1);
    EXPECT(first.size() == 1 && first[0].fileName == "needle_first.wav",
           "playlist scoped search first page preserves playlist order");
    EXPECT(second.size() == 1 && second[0].fileName == "needle_second.wav",
           "playlist scoped search second page reaches match beyond first page");
    filter.folderPath = HarkenSearch::CanonicalFolderPath("/playlist_scoped/Archive");
    EXPECT(db.searchPlaylistFileCount(playlist, filter) == 1,
           "playlist scoped count composes keyword and folder filter");
    auto folderPage = db.searchPlaylistFilesPage(playlist, filter, 10, 0);
    EXPECT(folderPage.size() == 1 && folderPage[0].fileName == "needle_first.wav",
           "playlist scoped rows use the same keyword and folder filter");

    db.close();
}

static void test_aggregated_playlist_scoped_search_dedups() {
    auto path = makeTempDbPath("agg_scoped_search");
    Database db;
    db.open(path);

    std::vector<int64_t> ids;
    for (const char* name : {"needle_shared.wav", "plain_child.wav", "needle_unique.wav"}) {
        AudioRecord r;
        r.fileName = name;
        r.filePath = std::string("/agg_scoped/") + name;
        r.extension = ".wav";
        r.keywords = name;
        db.addFile(r);
        ids.push_back(db.getFileByPath(r.filePath).id);
    }

    int64_t folder = db.createPlaylist("Scoped Folder", "");
    EXPECT(db.markAsFolder(folder), "scoped search folder created");
    int64_t a = db.createSubPlaylist(folder, "A", "");
    int64_t b = db.createSubPlaylist(folder, "B", "");
    db.addToPlaylist(a, ids[0]);
    db.addToPlaylist(b, ids[0]);
    db.addToPlaylist(b, ids[1]);
    db.addToPlaylist(b, ids[2]);

    SearchFilter filter;
    filter.keyword = "needle";
    EXPECT(db.searchAggregatedPlaylistFileCount(folder, filter) == 2,
           "aggregated playlist scoped search count de-duplicates shared matches");
    auto page = db.searchAggregatedPlaylistFilesPage(folder, filter, 10, 0);
    EXPECT(page.size() == 2, "aggregated playlist scoped search returns two unique matches");
    EXPECT(page.size() == 2 && page[0].fileName == "needle_shared.wav" && page[1].fileName == "needle_unique.wav",
           "aggregated playlist scoped search preserves stable id order");

    db.close();
}

static void test_all_playlist_scoped_search_dedups() {
    auto path = makeTempDbPath("all_scoped_search");
    Database db;
    db.open(path);

    std::vector<int64_t> ids;
    for (const char* name : {"needle_one.wav", "plain_one.wav", "needle_two.wav"}) {
        AudioRecord r;
        r.fileName = name;
        r.filePath = std::string("/all_scoped/") + name;
        r.extension = ".wav";
        r.keywords = name;
        db.addFile(r);
        ids.push_back(db.getFileByPath(r.filePath).id);
    }

    int64_t a = db.createPlaylist("All A", "");
    int64_t b = db.createPlaylist("All B", "");
    db.addToPlaylist(a, ids[0]);
    db.addToPlaylist(a, ids[1]);
    db.addToPlaylist(b, ids[0]);
    db.addToPlaylist(b, ids[2]);

    SearchFilter filter;
    filter.keyword = "needle";
    EXPECT(db.searchAllPlaylistFileCount(filter) == 2,
           "all-playlists scoped search count de-duplicates shared matches");
    auto page = db.searchAllPlaylistFilesPage(filter, 1, 1);
    EXPECT(page.size() == 1 && page[0].fileName == "needle_two.wav",
           "all-playlists scoped search pages across deduped filtered set");

    db.close();
}

static void test_search_worker_playlist_branch_uses_paged_api() {
    std::string source = readRepoFile("src/AsyncWorker.cpp");
    EXPECT(!source.empty(), "AsyncWorker.cpp source is readable");
    EXPECT(source.find("searchPlaylistFilesPage(req.playlistId, req.filter, req.limit, req.offset)") != std::string::npos,
           "SearchWorker playlist branch uses scoped paged playlist search API");
    EXPECT(source.find("searchAggregatedPlaylistFilesPage(req.playlistId, req.filter, req.limit, req.offset)") != std::string::npos,
           "SearchWorker aggregated playlist branch uses scoped paged playlist search API");
    EXPECT(source.find("searchPlaylistFileCount(req.playlistId, req.filter)") != std::string::npos,
           "SearchWorker playlist branch returns scoped playlist total count separately");
    EXPECT(source.find("WorkerFetch req=%llu playlist=%lld aggregated=%d keyword=%d limit=%d offset=%d loaded=%d total=%lld hasMore=%d") != std::string::npos,
           "SearchWorker playlist diagnostics include keyword and paging fields");
    EXPECT(source.find("searchAllPlaylistFileCount(req.filter)") != std::string::npos,
           "SearchWorker playlist library branch returns scoped distinct all-playlist total count");
    EXPECT(source.find("searchAllPlaylistFilesPage(req.filter, req.limit, req.offset)") != std::string::npos,
           "SearchWorker playlist library branch uses scoped paged all-playlist search API");
    EXPECT(source.find("WorkerFetchAllPlaylists req=%llu keyword=%d limit=%d offset=%d loaded=%d total=%lld hasMore=%d") != std::string::npos,
           "SearchWorker playlist library diagnostics include keyword and paging fields");
}

static void test_top_nav_playlist_library_contract() {
    std::string source = readRepoFile("src/main.cpp");
    EXPECT(!source.empty(), "main.cpp source is readable");
    EXPECT(source.find("{ L(View_Playlists),   ViewMode::PlaylistLibrary, true }") != std::string::npos,
           "top navigation includes playlist library button");
    EXPECT(source.find("LoadPlaylistLibraryFiltered(state);") != std::string::npos,
           "playlist library navigation activates the all-playlists view");
    EXPECT(source.find("state.viewMode = ViewMode::PlaylistLibrary;") != std::string::npos,
           "playlist library helper sets the dedicated view mode");
    EXPECT(source.find("if (state.viewMode == ViewMode::PlaylistLibrary) RefreshSearchResults(state, true)") == std::string::npos,
           "add-to-playlist flows do not force-switch into playlist library");
}

static void test_top_search_scope_contract() {
    std::string source = readRepoFile("src/main.cpp");
    std::string presetBuilders = readRepoFile("src/SearchPresetFilterBuilders.cpp");
    EXPECT(!source.empty(), "main.cpp source is readable for search scope contract");
    EXPECT(!presetBuilders.empty(), "preset filter builder source is readable for search scope contract");
    EXPECT(source.find("filter = BuildCurrentPlaylistSearchFilter(state);") != std::string::npos,
           "RefreshSearchResults builds a playlist scoped search filter");
    EXPECT(presetBuilders.find("filter.favoritesOnly = true;") == std::string::npos &&
            presetBuilders.find("BuildCurrentFavoritesFilter") == std::string::npos,
            "retired favorites scoped search filter is gone");
    EXPECT(presetBuilders.find("SearchFilter BuildCurrentPlaylistSearchFilter") != std::string::npos,
           "playlist scoped search filter helper exists");
    EXPECT(presetBuilders.find("filter.exactMode = state.searchExactMode;") != std::string::npos &&
           presetBuilders.find("const std::int64_t now = static_cast<std::int64_t>(std::time(nullptr));") != std::string::npos,
           "smart collection filter carries typed search keyword before preset rules");
    EXPECT(source.find("state.viewMode == ViewMode::PlaylistLibrary") != std::string::npos,
           "LoadMoreResults handles playlist and all-playlist pagination");
    EXPECT(source.find("showSearchQueryChips") != std::string::npos &&
           source.find("showDatabaseOnlyFilterChips") != std::string::npos,
           "search query chips render across scoped search views without leaking database-only chips");
}

static void test_file_type_visibility_chip_contract() {
    std::string source = readRepoFile("src/main.cpp");
    EXPECT(!source.empty(), "main.cpp source is readable for file-type chip contract");
    EXPECT(source.find("FilterChipType::FileTypes") != std::string::npos,
           "active filter chips include a dedicated file-type visibility chip");
    EXPECT(source.find("library look empty or \"FLAC only\"") != std::string::npos,
           "file-type chip documents the FLAC-only confusion it prevents");
    EXPECT(source.find("state.showWAV && state.showMP3 && state.showFLAC && state.showOGG") != std::string::npos,
           "file-type chip detects the all-types-visible state");
    EXPECT(source.find("state.showWAV = state.showMP3 = state.showFLAC = state.showOGG = true") != std::string::npos &&
           source.find("state.showAIFF = state.showM4A = state.showWMA = state.showOther = true") != std::string::npos,
           "clicking the file-type chip restores all file-type visibility toggles");
}

static void test_chip_plan_hides_collapsed_children() {
    std::vector<Playlist> all = {
        {1, "A", "", 0, 0, -1, true},   // folder A (parent_id=-1, is_folder=true)
        {2, "A.1", "", 0, 0, 1, false}, // child of A
        {3, "B", "", 0, 0, -1, false},  // orphan
    };
    std::vector<int64_t> hidden;
    auto plan = HarkenSearch::ComputePlaylistChipPlan(all, hidden);
    EXPECT(plan.size() == 2, "A and B render; A.1 collapsed under A");
    EXPECT(plan[0].id == 1 && plan[0].kind == HarkenSearch::ChipKind::Folder, "A is folder");
    EXPECT(plan[0].visibleChildCount == 1, "A shows (1) badge");
    EXPECT(plan[1].id == 3 && plan[1].kind == HarkenSearch::ChipKind::Orphan, "B is orphan");
}

static void test_chip_plan_independent_child_when_parent_hidden() {
    std::vector<Playlist> all = {
        {1, "A", "", 0, 0, -1, true},
        {2, "A.1", "", 0, 0, 1, false},
    };
    std::vector<int64_t> hidden = {1};  // parent hidden
    auto plan = HarkenSearch::ComputePlaylistChipPlan(all, hidden);
    EXPECT(plan.size() == 1, "only the child renders");
    EXPECT(plan[0].id == 2 && plan[0].kind == HarkenSearch::ChipKind::IndependentChild,
           "child renders independently");
    EXPECT(plan[0].parentName == "A", "parent name attached for tooltip");
}

int main() {
    test_schema_migration_adds_columns();
    test_create_child_under_parent();
    test_folder_rejects_add_file();
    test_aggregated_files_dedup();
    test_delete_folder_cascades();
    test_promote_orphan_with_files();
    test_move_sub_to_other_parent();
    test_import_reaper_file_list_to_playlist();
    test_playlist_files_page_preserves_order_and_count();
    test_aggregated_playlist_files_page_dedups_and_counts();
    test_all_playlist_files_page_dedups_and_counts();
    test_playlist_scoped_search_filters_before_paging();
    test_aggregated_playlist_scoped_search_dedups();
    test_all_playlist_scoped_search_dedups();
    test_search_worker_playlist_branch_uses_paged_api();
    test_top_nav_playlist_library_contract();
    test_top_search_scope_contract();
    test_file_type_visibility_chip_contract();
    test_chip_plan_hides_collapsed_children();
    test_chip_plan_independent_child_when_parent_hidden();
    std::printf("\n%d passed, %d failed\n", s_pass, s_fail);
    return s_fail == 0 ? 0 : 1;
}
