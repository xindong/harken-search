#include "Database.h"
#include "PathUtils.h"
#include "sqlite3.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;

static int g_failures = 0;

static void check(bool condition, const char* message) {
    if (!condition) {
        std::printf("FAIL: %s\n", message);
        ++g_failures;
    } else {
        std::printf("PASS: %s\n", message);
    }
}

static void removeDbArtifact(const fs::path& path) {
    std::error_code ec;
    fs::remove(path, ec);
}

struct TempDbPath {
    std::string value;

    explicit TempDbPath(const char* tag) {
        static std::uint64_t serial = 0;
        fs::path dir = fs::temp_directory_path() / "auro_playlist_perf_contract";
        std::error_code ec;
        fs::create_directories(dir, ec);
        const std::uint64_t nonce =
            static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
        value = (dir / (std::string(tag) + "_" + std::to_string(nonce) + "_" +
                        std::to_string(++serial) + ".db")).string();
    }

    ~TempDbPath() {
        const fs::path mainPath(value);
        removeDbArtifact(mainPath);
        removeDbArtifact(fs::path(value + "-wal"));
        removeDbArtifact(fs::path(value + "-shm"));
        const std::string stem = value.size() >= 3 ? value.substr(0, value.size() - 3) : value;
        for (const char* sidecar : {"_blobs.db", "_waveforms.db"}) {
            const std::string sidecarPath = stem + sidecar;
            removeDbArtifact(fs::path(sidecarPath));
            removeDbArtifact(fs::path(sidecarPath + "-wal"));
            removeDbArtifact(fs::path(sidecarPath + "-shm"));
        }
    }

    operator const std::string&() const { return value; }
};

static TempDbPath tempDbPath(const char* tag) {
    return TempDbPath(tag);
}

static bool indexExists(sqlite3* db, const char* expectedName) {
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, "PRAGMA index_list('playlist_items')", -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }
    bool found = false;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const unsigned char* text = sqlite3_column_text(stmt, 1);
        if (text && std::string(reinterpret_cast<const char*>(text)) == expectedName) {
            found = true;
            break;
        }
    }
    sqlite3_finalize(stmt);
    return found;
}

static std::vector<int64_t> seedAudioFiles(Database& db, int count, const char* prefix) {
    std::vector<int64_t> ids;
    ids.reserve((size_t)count);
    // Fixture creation is not part of the timed playlist operations. Keep
    // the 20k inserts in one transaction so disk flushes do not consume the
    // CTest timeout before the actual performance assertions run.
    if (!db.beginTransaction()) {
        check(false, "audio fixture transaction begins");
        return ids;
    }
    for (int i = 0; i < count; ++i) {
        AudioRecord record;
        record.fileName = std::string(prefix) + "_" + std::to_string(i) + ".wav";
        record.filePath = "/" + std::string(prefix) + "/" + record.fileName;
        record.extension = ".wav";
        db.addFile(record);
        ids.push_back(db.getFileByPath(record.filePath).id);
    }
    check(db.commitTransaction(), "audio fixture transaction commits");
    return ids;
}

static double elapsedMs(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

static std::vector<int64_t> recordIds(const std::vector<AudioRecord>& records) {
    std::vector<int64_t> ids;
    ids.reserve(records.size());
    for (const auto& record : records) ids.push_back(record.id);
    return ids;
}

static void test_schema_indexes_exist() {
    Database db;
    const TempDbPath tempPath = tempDbPath("schema_indexes");
    const std::string& path = tempPath.value;
    check(db.open(path), "database opens for playlist schema index contract");
    check(indexExists(db.db(), "idx_playlist_items_playlist_position"),
          "playlist position index exists");
    check(indexExists(db.db(), "idx_playlist_items_file_playlist"),
          "playlist reverse membership index exists");
    db.close();
}

static void test_bulk_add_is_bounded_and_ordered() {
    Database db;
    const TempDbPath tempPath = tempDbPath("bulk_add");
    const std::string& path = tempPath.value;
    check(db.open(path), "database opens for playlist bulk add contract");

    constexpr int kTotal = 20000;
    std::vector<int64_t> ids = seedAudioFiles(db, kTotal, "bulk_playlist");
    const int64_t playlistId = db.createPlaylist("Bulk Perf", "");
    check(playlistId > 0, "bulk playlist created");

    auto start = std::chrono::steady_clock::now();
    Database::PlaylistBulkAddResult result = db.addManyToPlaylist(playlistId, ids);
    const double bulkMs = elapsedMs(start);
    std::printf("METRIC: Playlist.BulkAdd count=%d added=%d duplicate=%d invalid=%d durationMs=%.3f\n",
                kTotal, result.addedFiles, result.alreadyInPlaylist, result.invalidFiles, bulkMs);

    check(result.error.empty(), "bulk add reports no error");
    check(result.addedFiles == kTotal, "bulk add inserts every new file");
    check(db.getPlaylistFileCount(playlistId) == kTotal, "bulk add updates playlist count");
    check(bulkMs < 1500.0, "bulk add 20k stays below 1500ms contract");

    start = std::chrono::steady_clock::now();
    Database::PlaylistBulkAddResult duplicate = db.addManyToPlaylist(playlistId, ids);
    const double dupMs = elapsedMs(start);
    std::printf("METRIC: Playlist.BulkAddDuplicate count=%d added=%d duplicate=%d durationMs=%.3f\n",
                kTotal, duplicate.addedFiles, duplicate.alreadyInPlaylist, dupMs);
    check(duplicate.error.empty(), "duplicate bulk add reports no error");
    check(duplicate.addedFiles == 0 && duplicate.alreadyInPlaylist == kTotal,
          "duplicate bulk add is idempotent");

    start = std::chrono::steady_clock::now();
    std::vector<AudioRecord> page = db.getPlaylistFilesPage(playlistId, 500, 15000);
    const double pageMs = elapsedMs(start);
    std::printf("METRIC: Playlist.QueryPage offset=15000 loaded=%zu durationMs=%.3f\n",
                page.size(), pageMs);
    check(page.size() == 500, "playlist page returns requested page size");
    check(!page.empty() && page.front().filePath == "/bulk_playlist/bulk_playlist_15000.wav",
          "playlist page preserves membership order");
    check(pageMs < 75.0, "playlist 500-row page stays below 75ms contract");

    const std::string folder = HarkenSearch::CanonicalFolderPath("/bulk_playlist");
    const int64_t legacy = db.createPlaylist("Legacy", "");
    const int64_t candidate = db.createPlaylist("Candidate", "");
    start = std::chrono::steady_clock::now();
    const auto legacyResult = db.addManyToPlaylist(legacy, recordIds(db.getFilesByFolder(folder, 1000000, 0)));
    const double legacyMs = elapsedMs(start);
    start = std::chrono::steady_clock::now();
    const auto candidateResult = db.addFolderToPlaylist(candidate, folder);
    const double candidateMs = elapsedMs(start);
    std::printf("METRIC: Playlist.FolderDrop count=%d legacyMs=%.3f idOnlyMs=%.3f\n",
                kTotal, legacyMs, candidateMs);
    check(legacyResult.error.empty() && candidateResult.error.empty() &&
          legacyResult.addedFiles == kTotal && candidateResult.addedFiles == kTotal &&
          db.getPlaylistFileCount(candidate) == kTotal, "large folder drop adds all files");
    db.close();
}

static void test_bulk_add_rejects_folder_playlist() {
    Database db;
    const TempDbPath tempPath = tempDbPath("folder_reject");
    const std::string& path = tempPath.value;
    check(db.open(path), "database opens for folder rejection contract");
    std::vector<int64_t> ids = seedAudioFiles(db, 3, "folder_reject");
    const int64_t folderId = db.createPlaylist("Folder", "");
    check(db.markAsFolder(folderId), "playlist folder created");
    Database::PlaylistBulkAddResult result = db.addManyToPlaylist(folderId, ids);
    check(!result.error.empty(), "bulk add rejects folder target");
    check(db.getPlaylistFileCount(folderId) == 0, "folder target keeps zero direct items");
    db.close();
}

static int64_t seedFolderDropFile(Database& db, const std::string& folder, const std::string& name) {
    AudioRecord record;
    record.folderPath = folder;
    record.fileName = name;
    record.filePath = folder + "/" + name;
    record.extension = ".wav";
    db.addFile(record);
    return db.getFileByPath(record.filePath).id;
}

static void test_folder_drop_order_and_rollback() {
    Database db;
    const TempDbPath tempPath = tempDbPath("folder_drop");
    if (!db.open(tempPath.value)) {
        check(false, "database opens for folder drop contract");
        return;
    }
    const int64_t existing = seedFolderDropFile(db, "/outside", "track0.wav");
    const int64_t trackTen = seedFolderDropFile(db, "/drop", "track10.wav");
    const int64_t trackTwo = seedFolderDropFile(db, "/drop/child", "track2.wav");
    const int64_t sameName = seedFolderDropFile(db, "/drop/other", "track2.wav");
    const int64_t trackOne = seedFolderDropFile(db, "/drop", "track1.wav");
    seedFolderDropFile(db, "/drop-sibling", "track3.wav");

    const std::string folder = HarkenSearch::CanonicalFolderPath("/drop");
    const auto sourceIds = recordIds(db.getFilesByFolder(folder, 1000000, 0));
    check(sourceIds.size() == 4 && sourceIds.front() == trackOne && sourceIds.back() == trackTen,
          "legacy selection includes descendants, excludes sibling and uses natural order");
    const int64_t legacy = db.createPlaylist("Legacy", "");
    const int64_t candidate = db.createPlaylist("Candidate", "");
    check(db.addManyToPlaylist(legacy, sourceIds).addedFiles == 4, "legacy write adds all source rows");
    check(db.addFolderToPlaylist(candidate, folder).addedFiles == 4, "folder drop adds all source rows");
    check(recordIds(db.getPlaylistFilesPage(candidate, 10, 0)) ==
          recordIds(db.getPlaylistFilesPage(legacy, 10, 0)), "folder drop preserves legacy order including ties");

    const int64_t appendTarget = db.createPlaylist("Append", "");
    check(db.addManyToPlaylist(appendTarget, {existing, trackTwo}).addedFiles == 2,
          "append target has pre-existing members");
    const auto added = db.addFolderToPlaylist(appendTarget, folder);
    check(added.error.empty() && added.requestedFiles == 4 && added.addedFiles == 3 &&
          added.alreadyInPlaylist == 1, "folder drop appends only new members");
    check(recordIds(db.getPlaylistFilesPage(appendTarget, 10, 0)) ==
          std::vector<int64_t>{existing, trackTwo, trackOne, sameName, trackTen},
          "existing positions and natural append order stay unchanged");
    const auto duplicate = db.addFolderToPlaylist(appendTarget, folder);
    check(duplicate.error.empty() && duplicate.addedFiles == 0 && duplicate.alreadyInPlaylist == 4,
          "repeated folder drop is idempotent");
    const auto empty = db.addFolderToPlaylist(appendTarget, "/no-such-folder");
    check(empty.error.empty() && empty.requestedFiles == 0 && empty.addedFiles == 0,
          "empty source is a no-op");

    const int64_t failureTarget = db.createPlaylist("Rollback", "");
    check(db.addManyToPlaylist(failureTarget, {existing}).addedFiles == 1, "rollback target has one member");
    const std::string abortSql = "CREATE TRIGGER abort_folder_drop BEFORE INSERT ON playlist_items "
        "WHEN NEW.playlist_id = " + std::to_string(failureTarget) +
        " AND NEW.file_id = " + std::to_string(trackTen) +
        " BEGIN SELECT RAISE(ABORT, 'folder drop test failure'); END;";
    check(sqlite3_exec(db.db(), abortSql.c_str(), nullptr, nullptr, nullptr) == SQLITE_OK,
          "test injects a failure after earlier rows have been inserted");
    const auto failed = db.addFolderToPlaylist(failureTarget, folder);
    check(!failed.error.empty() && failed.addedFiles == 0, "failed folder drop reports no additions");
    check(db.getPlaylistFileCount(failureTarget) == 1, "failed folder drop rolls back the batch");
    check(sqlite3_exec(db.db(), "DROP TRIGGER abort_folder_drop", nullptr, nullptr, nullptr) == SQLITE_OK,
          "test removes the injected failure");
    check(db.addFolderToPlaylist(failureTarget, folder).addedFiles == 4,
          "folder drop works again after the failure is removed");
    db.close();
}

static void test_playlist_metadata_load_does_not_wait_for_counts() {
    Database db;
    const TempDbPath tempPath = tempDbPath("metadata_first");
    const std::string& path = tempPath.value;
    check(db.open(path), "database opens for playlist metadata-first contract");

    constexpr int kTotal = 20000;
    std::vector<int64_t> ids = seedAudioFiles(db, kTotal, "metadata_playlist");
    const int64_t playlistId = db.createPlaylist("Previous Session Playlist", "");
    check(playlistId > 0, "metadata-first playlist created");
    Database::PlaylistBulkAddResult result = db.addManyToPlaylist(playlistId, ids);
    check(result.addedFiles == kTotal, "metadata-first playlist seeded");

    auto start = std::chrono::steady_clock::now();
    std::vector<Playlist> metadataOnly = db.getAllPlaylists(false);
    const double metadataMs = elapsedMs(start);
    std::printf("METRIC: Playlist.MetadataOnlyLoad playlists=%zu durationMs=%.3f\n",
                metadataOnly.size(), metadataMs);

    check(metadataOnly.size() == 1, "metadata-only load returns existing playlist name");
    check(metadataOnly[0].name == "Previous Session Playlist",
          "metadata-only load preserves previous playlist identity");
    check(metadataOnly[0].fileCount == -1,
          "metadata-only load marks file count as pending");
    check(metadataMs < 25.0,
          "metadata-only playlist load stays below 25ms contract");

    std::vector<Playlist> withCounts = db.getAllPlaylists(true);
    check(withCounts.size() == 1 && withCounts[0].fileCount == kTotal,
          "counted playlist load eventually reports real file count");
    db.close();
}

int main() {
    test_schema_indexes_exist();
    test_bulk_add_is_bounded_and_ordered();
    test_bulk_add_rejects_folder_playlist();
    test_folder_drop_order_and_rollback();
    test_playlist_metadata_load_does_not_wait_for_counts();
    if (g_failures != 0) {
        std::printf("FAIL: TestPlaylistPerformanceContract failures=%d\n", g_failures);
        return 1;
    }
    std::printf("PASS: TestPlaylistPerformanceContract\n");
    return 0;
}
