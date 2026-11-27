// test_multi_tab_sync.cpp — T-295
// Verify Database::dataRevision() bumps on user-visible writes (tag,
// favorite, metadata patch). The cross-tab refresh trigger in
// AppState::syncFromTab is exercised at a unit level here by replicating
// its 4-line revision check against a real Database — pulling the full
// AppState symbol graph (FileBrowser, GaplessEngine, MusicAnalyzer …)
// into a test executable would gain nothing over this direct check.

#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <string>
#include <thread>
#include <chrono>
#include <vector>

#include "src/Database.h"
#include "src/DiagnosticsLogger.h"

// AudioEngine.cpp references g_appState (an AppState*) — normally defined
// by AppState.cpp. We skip linking AppState.cpp to keep this test small,
// so provide a forward decl + null stub here.
struct AppState;
AppState* g_appState = nullptr;

namespace fs = std::filesystem;

static int g_failures = 0;
static void check(bool cond, const char* desc) {
    if (cond) std::printf("PASS: %s\n", desc);
    else { std::printf("FAIL: %s\n", desc); ++g_failures; }
}

static fs::path makeTempDir(const char* tag) {
    fs::path p = fs::temp_directory_path() /
                 ("auro_t295_" + std::string(tag));
    std::error_code ec;
    fs::remove_all(p, ec);
    fs::create_directories(p);
    return p;
}

static void waitDiagDrain() {
    std::this_thread::sleep_for(std::chrono::milliseconds(650));
}

// Mirror of the relevant ListTab field for the cross-tab refresh check.
struct TabSnapshot {
    uint64_t resultsBuiltAtRevision = 0;
};

// Replicates the 4-line check newly added to AppState::syncFromTab.
// Returns true iff the tab is stale and needs a refresh.
static bool tabNeedsRefresh(TabSnapshot& tab, const Database& db) {
    const uint64_t currentRev = db.dataRevision();
    if (tab.resultsBuiltAtRevision < currentRev) {
        tab.resultsBuiltAtRevision = currentRev;
        return true;
    }
    return false;
}

int main() {
    fs::path dbDir = makeTempDir("db");
    Database db;
    bool opened = db.open((dbDir / "main.db").string(), true);
    check(opened, "Database opens");
    if (!opened) return 1;

    AudioRecord rec;
    rec.filePath = "C:/dummy/test.wav";
    rec.fileName = "test.wav";
    rec.extension = "wav";
    rec.fileSize = 1024;
    rec.sampleRate = 44100;
    rec.channels = 2;
    rec.durationSeconds = 1.0f;
    rec.format = "WAV";
    bool added = db.addFile(rec);
    check(added, "addFile(rec) succeeds");

    AudioRecord fetched = db.getFileByPath(rec.filePath);
    int64_t fileId = fetched.id;
    check(fileId > 0, "getFileByPath returns valid id");

    AudioRecord recSecond = rec;
    recSecond.filePath = "C:/dummy/aaa-second.wav";
    recSecond.fileName = "aaa-second.wav";
    bool addedSecond = db.addFile(recSecond);
    check(addedSecond, "addFile(second rec) succeeds");
    AudioRecord fetchedSecond = db.getFileByPath(recSecond.filePath);
    int64_t secondFileId = fetchedSecond.id;
    check(secondFileId > 0, "getFileByPath returns second valid id");

    auto& diag = DiagnosticsLogger::instance();
    diag.setEnabled(true);
    diag.setMaxEntries(512);
    diag.clear();

    // ----- Test 1: dataRevision bumps on tag / metadata writes.
    uint64_t r0 = db.dataRevision();

    check(db.addTagToFile(fileId, "tag-alpha"), "addTagToFile succeeds");
    uint64_t r1 = db.dataRevision();
    check(r1 > r0, "dataRevision bumped after addTagToFile");

    check(db.removeTagFromFile(fileId, "tag-alpha"), "removeTagFromFile succeeds");
    uint64_t r3 = db.dataRevision();
    check(r3 > r1, "dataRevision bumped after removeTagFromFile");

    MetadataEditPatch patch;
    patch.setDescription("hello-from-tab-A");
    check(db.applyMetadataPatch(fileId, patch), "applyMetadataPatch succeeds");
    waitDiagDrain();
    std::string metadataApplyDiag = diag.getRecentJsonLines(16);
    check(metadataApplyDiag.find("PatchBatchComplete: total=1 success=1 failed=0") != std::string::npos,
          "metadata patch success emits diagnostics summary");
    uint64_t r4 = db.dataRevision();
    check(r4 > r3, "dataRevision bumped after applyMetadataPatch");

    // ----- Test 1b: batch metadata edit is atomic when one selected id
    // disappeared between preview and apply.
    diag.clear();
    MetadataEditPatch batchPatch;
    batchPatch.setDescription("should-not-stick");
    uint64_t beforeFailedBatchRev = db.dataRevision();
    BatchMetadataApplyResult failedBatch =
        db.applyMetadataPatchBatch({fileId, 987654321}, batchPatch);
    check(failedBatch.total == 2, "failed batch reports total selected ids");
    check(failedBatch.success == 0, "failed batch reports zero success after rollback");
    check(failedBatch.failed == 2, "failed batch reports all selected ids failed after rollback");
    AudioRecord afterFailedBatch = db.getFileByPath(rec.filePath);
    check(afterFailedBatch.description == "hello-from-tab-A",
          "failed batch does not modify valid row");
    check(db.dataRevision() == beforeFailedBatchRev,
          "failed batch does not bump dataRevision");
    check(!db.applyMetadataPatch(987654321, batchPatch),
          "single metadata patch reports missing id as failure");
    waitDiagDrain();
    std::string metadataFailDiag = diag.getRecentJsonLines(24);
    check(metadataFailDiag.find("PatchRejected: reason=missing_file_id id=987654321") != std::string::npos,
          "metadata patch failure emits missing-file diagnostics");
    check(metadataFailDiag.find("PatchBatchComplete: total=2 success=0 failed=2") != std::string::npos,
          "failed metadata batch emits rollback summary diagnostics");

    MetadataEditPatch manyPatch;
    manyPatch.setDescription("many-should-not-stick");
    MetadataEditPatch missingPatch;
    missingPatch.setCategory("missing-category");
    uint64_t beforeFailedManyRev = db.dataRevision();
    BatchMetadataApplyResult failedMany =
        db.applyMetadataPatchMany({{fileId, manyPatch}, {987654321, missingPatch}});
    check(failedMany.total == 2, "failed patch-many reports total selected ids");
    check(failedMany.success == 0, "failed patch-many reports zero success after rollback");
    check(failedMany.failed == 2, "failed patch-many reports all selected ids failed after rollback");
    AudioRecord afterFailedMany = db.getFileByPath(rec.filePath);
    check(afterFailedMany.description == "hello-from-tab-A",
          "failed patch-many does not modify valid row");
    check(db.dataRevision() == beforeFailedManyRev,
          "failed patch-many does not bump dataRevision");
    waitDiagDrain();
    std::string metadataManyDiag = diag.getRecentJsonLines(24);
    check(metadataManyDiag.find("PatchManyComplete: total=2 success=0 failed=2") != std::string::npos,
          "failed patch-many emits diagnostics summary");

    // ----- Test 1b-2: a harken_meta written by a metadata patch is searchable
    // straight away. The patch UPDATE is evaluated against the pre-update row,
    // so the new Harken values only reach search_data if the plan feeds them
    // in from the bound value.
    MetadataEditPatch harkenPatch;
    harkenPatch.set("harken_meta",
                    R"({"category":"Weapons","function":"Impact","source":"manual"})");
    check(db.applyMetadataPatch(fileId, harkenPatch), "harken_meta patch applies");
    SearchFilter harkenCategoryFilter;
    harkenCategoryFilter.keyword = "weapons";
    std::vector<AudioRecord> harkenCategoryHits = db.searchFiles(harkenCategoryFilter);
    check(harkenCategoryHits.size() == 1 && harkenCategoryHits[0].id == fileId,
          "harken_meta category from a patch is searchable immediately");
    SearchFilter harkenFunctionFilter;
    harkenFunctionFilter.keyword = "impact";
    std::vector<AudioRecord> harkenFunctionHits = db.searchFiles(harkenFunctionFilter);
    check(harkenFunctionHits.size() == 1 && harkenFunctionHits[0].id == fileId,
          "harken_meta function from a patch is searchable immediately");

    // ----- Test 1c: project-list carts preserve insertion order and refresh
    // other tabs when membership changes.
    uint64_t beforeCreatePlaylistRev = db.dataRevision();
    int64_t playlistId = db.createPlaylist("Spotting Cart");
    check(playlistId > 0, "createPlaylist succeeds");
    check(db.dataRevision() > beforeCreatePlaylistRev,
          "dataRevision bumped after createPlaylist");

    uint64_t beforeFirstAddRev = db.dataRevision();
    check(db.addToPlaylist(playlistId, fileId), "add first file to playlist succeeds");
    check(db.dataRevision() > beforeFirstAddRev,
          "dataRevision bumped after first playlist add");
    check(db.addToPlaylist(playlistId, secondFileId), "add second file to playlist succeeds");
    std::vector<AudioRecord> playlistFiles = db.getPlaylistFiles(playlistId);
    check(playlistFiles.size() == 2, "playlist returns two files");
    check(playlistFiles.size() == 2 &&
              playlistFiles[0].id == fileId &&
              playlistFiles[1].id == secondFileId,
          "playlist preserves insertion order instead of filename order");

    uint64_t beforeDuplicateAddRev = db.dataRevision();
    check(db.addToPlaylist(playlistId, fileId), "duplicate playlist add is idempotent");
    check(db.getPlaylistFileCount(playlistId) == 2,
          "duplicate playlist add does not duplicate rows");
    check(db.dataRevision() == beforeDuplicateAddRev,
          "duplicate playlist add does not bump dataRevision");

    uint64_t beforeRemoveRev = db.dataRevision();
    check(db.removeFromPlaylist(playlistId, secondFileId),
          "removeFromPlaylist succeeds");
    check(db.dataRevision() > beforeRemoveRev,
          "dataRevision bumped after playlist remove");

    uint64_t beforeCopyRev = db.dataRevision();
    int64_t copiedPlaylistId = db.copyPlaylist(playlistId, "Spotting Cart Copy");
    check(copiedPlaylistId > 0, "copyPlaylist succeeds");
    check(db.dataRevision() > beforeCopyRev,
          "dataRevision bumped after playlist copy");
    check(db.getPlaylistFileCount(copiedPlaylistId) == 1,
          "copied playlist preserves remaining playlist items");
    std::vector<AudioRecord> copiedPlaylistFiles = db.getPlaylistFiles(copiedPlaylistId);
    check(copiedPlaylistFiles.size() == 1 && copiedPlaylistFiles[0].id == fileId,
          "copied playlist preserves file membership");

    // ----- Test 2: tab-stale detection. Tab B was archived after r4, so
    // the next write (a tag toggle) should make it stale on activation.
    TabSnapshot tabB;
    tabB.resultsBuiltAtRevision = db.dataRevision();
    check(!tabNeedsRefresh(tabB, db),
          "Tab B activated at current revision is NOT stale");

    db.addTagToFile(fileId, "tag-beta");  // simulate user editing in Tab A
    check(db.dataRevision() > tabB.resultsBuiltAtRevision,
          "Revision moved past Tab B snapshot after a write");

    check(tabNeedsRefresh(tabB, db),
          "Tab B activation after revision bump is stale -> refresh");
    check(!tabNeedsRefresh(tabB, db),
          "Second activation without further writes is NOT stale");

    db.close();
    diag.setEnabled(false);
    std::error_code ec;
    fs::remove_all(dbDir, ec);

    if (g_failures == 0) {
        std::printf("\nAll T-295 multi-tab sync checks passed.\n");
        return 0;
    }
    std::printf("\n%d failure(s)\n", g_failures);
    return 1;
}
