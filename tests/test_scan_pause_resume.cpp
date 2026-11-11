#include "Database.h"
#include "ScanInterruption.h"
#include "sqlite3.h"
#include "platform/harken_user_data_layout.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

void check(bool condition, const char* label, int& failures) {
    std::cout << (condition ? "PASS: " : "FAIL: ") << label << '\n';
    if (!condition) ++failures;
}

void writeSilentMonoWav(const fs::path& path) {
    constexpr std::uint32_t sampleRate = 8000;
    constexpr std::uint16_t channels = 1;
    constexpr std::uint16_t bitsPerSample = 16;
    constexpr std::uint16_t blockAlign = channels * bitsPerSample / 8;
    constexpr std::uint32_t byteRate = sampleRate * blockAlign;
    constexpr std::uint32_t frameCount = 128;
    constexpr std::uint32_t dataSize = frameCount * blockAlign;
    constexpr std::uint32_t riffSize = 36 + dataSize;
    constexpr std::uint32_t fmtSize = 16;
    constexpr std::uint16_t pcmFormat = 1;

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write("RIFF", 4);
    out.write(reinterpret_cast<const char*>(&riffSize), sizeof(riffSize));
    out.write("WAVEfmt ", 8);
    out.write(reinterpret_cast<const char*>(&fmtSize), sizeof(fmtSize));
    out.write(reinterpret_cast<const char*>(&pcmFormat), sizeof(pcmFormat));
    out.write(reinterpret_cast<const char*>(&channels), sizeof(channels));
    out.write(reinterpret_cast<const char*>(&sampleRate), sizeof(sampleRate));
    out.write(reinterpret_cast<const char*>(&byteRate), sizeof(byteRate));
    out.write(reinterpret_cast<const char*>(&blockAlign), sizeof(blockAlign));
    out.write(reinterpret_cast<const char*>(&bitsPerSample), sizeof(bitsPerSample));
    out.write("data", 4);
    out.write(reinterpret_cast<const char*>(&dataSize), sizeof(dataSize));
    const std::vector<std::int16_t> silence(frameCount, 0);
    out.write(reinterpret_cast<const char*>(silence.data()),
              static_cast<std::streamsize>(silence.size() * sizeof(std::int16_t)));
}

fs::path makeFixture(const char* name) {
    const fs::path base = Platform::HarkenUserData::PreviewIsolationActive()
        ? fs::u8path(Platform::HarkenUserData::PreviewIsolationRootUtf8()) / "Testing"
        : fs::temp_directory_path();
    const fs::path root = base /
        (std::string("aris_scan_pause_resume_") + name);
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root / "audio", ec);
    for (int i = 1; i <= 3; ++i) {
        writeSilentMonoWav(root / "audio" /
            (std::string("stall_0") + std::to_string(i) + ".wav"));
    }
    return root;
}

ScanStats interruptAfterFirstCommit(
    Database& db,
    const fs::path& audioRoot,
    HarkenSearch::ScanStopDisposition requested) {
    std::atomic<bool> cancel{false};
    std::atomic<HarkenSearch::ScanStopDisposition> disposition{
        HarkenSearch::ScanStopDisposition::None
    };
    db.beginScanRollbackSession();
    return db.scanDirectoryEx(
        audioRoot.u8string(),
        true,
        false,
        false,
        [&](int current, int, const std::string&, bool dataCommitted) {
            if (dataCommitted && current > 0) {
                disposition.store(requested, std::memory_order_release);
                cancel.store(true, std::memory_order_release);
            }
        },
        &cancel,
        1,
        false,
        false,
        nullptr,
        {},
        0,
        0,
        &disposition);
}

void testPausePersistsAcrossRestart(int& failures) {
    const fs::path root = makeFixture("keep");
    const fs::path dbPath = root / "library.db";
    Database db;
    check(db.open(dbPath.u8string()), "open pause database", failures);

    const ScanStats paused = interruptAfterFirstCommit(
        db, root / "audio", HarkenSearch::ScanStopDisposition::PauseKeepCommitted);
    check(paused.paused && !paused.cancelled,
          "pause is reported separately from cancel", failures);
    const int64_t retained = db.getFileCount();
    check(retained > 0 && retained < 3,
          "pause retains only the committed prefix", failures);
    db.close();

    check(db.open(dbPath.u8string()), "reopen pause database", failures);
    const ScanStats resumed = db.scanDirectoryEx(
        (root / "audio").u8string(), true, false, false,
        nullptr, nullptr, 1, false, false);
    check(db.getFileCount() == 3,
          "cross-restart rescan completes the folder", failures);
    check(resumed.added == 3 - retained,
          "resume inserts only unfinished files", failures);
    db.close();

    std::error_code ec;
    fs::remove_all(root, ec);
}

void testCancelStillRollsBack(int& failures) {
    const fs::path root = makeFixture("rollback");
    Database db;
    check(db.open((root / "library.db").u8string()),
          "open rollback database", failures);

    const ScanStats cancelled = interruptAfterFirstCommit(
        db, root / "audio", HarkenSearch::ScanStopDisposition::CancelRollback);
    check(cancelled.cancelled && !cancelled.paused,
          "rollback cancel is reported separately from pause", failures);
    check(db.getFileCount() == 0,
          "rollback cancel removes the committed prefix", failures);
    db.close();

    std::error_code ec;
    fs::remove_all(root, ec);
}

void testPostProcessingNeverInflatesFileCounts(int& failures) {
    const fs::path root = makeFixture("progress_counts");
    Database db;
    check(db.open((root / "library.db").u8string()), "open progress database", failures);
    int maxCurrent = 0, maxTotal = 0, lastCurrent = -1, lastTotal = -1;
    int finalizingCalls = 0;
    bool finalizingCountsSeen = false;
    const auto stats = db.scanDirectoryEx(
        (root / "audio").u8string(), true, false, false,
        [&](int current, int total, const std::string&, bool) {
            maxCurrent = std::max(maxCurrent, current);
            maxTotal = std::max(maxTotal, total);
            lastCurrent = current; lastTotal = total;
            if (finalizingCalls > 0) finalizingCountsSeen = current == 3 && total == 3;
        }, nullptr, 1, false, false, nullptr, {}, 0, 0, nullptr,
        [&]() { ++finalizingCalls; });
    check(stats.total == 3 && db.getFileCount() == 3, "progress fixture imports three real files", failures);
    check(maxCurrent == 3 && maxTotal == 3 && lastCurrent == 3 && lastTotal == 3,
          "post-processing is not counted as extra audio files", failures);
    check(finalizingCalls == 1 && finalizingCountsSeen,
          "synchronous finalizing notification precedes real-count tail reports", failures);
    db.close();
    // Retain this small fixture; the QA temp root owns its lifecycle.
}

void testRollbackPreservesSimilarityAnalysis(int& failures) {
    const fs::path root = makeFixture("similarity");
    Database db;
    check(db.open((root / "library.db").u8string()), "open similarity rollback database", failures);
    db.scanDirectoryEx((root / "audio").u8string(), true, false, false,
                       nullptr, nullptr, 1, false, false);
    const std::string manuallyImportedPath = (root / "audio" / "stall_01.wav").u8string();
    check(db.markFileAsManuallyImported(manuallyImportedPath),
          "mark a scanned row as an explicit user import", failures);
    // Opaque V2 blobs must survive the database rollback byte-for-byte. The
    // deletion cascade is the same one installed by SimilarityAnalysisStore.
    const char* seed = R"SQL(
CREATE TABLE harken_similarity_v2(
 file_id INTEGER PRIMARY KEY,file_path TEXT NOT NULL,file_bytes INTEGER NOT NULL,modified INTEGER NOT NULL,
 fingerprint TEXT NOT NULL,version INTEGER NOT NULL,summary BLOB NOT NULL,acoustic BLOB NOT NULL,
 embedding BLOB,model_version TEXT NOT NULL DEFAULT '');
CREATE TABLE harken_similarity_segments_v2(
 file_id INTEGER NOT NULL,ordinal INTEGER NOT NULL,start REAL NOT NULL,end REAL NOT NULL,embedding BLOB NOT NULL,
 PRIMARY KEY(file_id,ordinal));
CREATE TRIGGER harken_similarity_audio_delete AFTER DELETE ON audio_files BEGIN
 DELETE FROM harken_similarity_v2 WHERE file_id=OLD.id; END;
CREATE TRIGGER harken_similarity_delete AFTER DELETE ON harken_similarity_v2 BEGIN
 DELETE FROM harken_similarity_segments_v2 WHERE file_id=OLD.file_id; END;
INSERT INTO harken_similarity_v2 SELECT id,file_path,file_size,123,'original-content',2,X'001122',X'334455',X'667788','test-model' FROM audio_files;
INSERT INTO harken_similarity_segments_v2 SELECT id,0,0.0,1.0,X'AABBCC' FROM audio_files;
CREATE TEMP TABLE expected_similarity AS SELECT * FROM harken_similarity_v2;
CREATE TEMP TABLE expected_segments AS SELECT * FROM harken_similarity_segments_v2;
)SQL";
    check(sqlite3_exec(db.db(), seed, nullptr, nullptr, nullptr) == SQLITE_OK,
          "seed existing analysis and segment blobs", failures);
    db.beginScanRollbackSession();
    // Unchanged audio still gets touched when missing waveform work is queued.
    const auto rescanned = db.scanDirectoryEx((root / "audio").u8string(), true, false, false,
                                             nullptr, nullptr, 1, true, true);
    int touched = 0;
    db.execQuery("SELECT COUNT(*) FROM temp.scan_cancel_audio",
                 [&](sqlite3_stmt* row) { touched = sqlite3_column_int(row, 0); });
    check(rescanned.total == 3 && touched == 3,
          "unchanged rescan snapshots files needing waveform work", failures);
    check(db.rollbackActiveScanSession(nullptr), "cancel rolls back the active scan", failures);
    auto preserved = [&](const char* table, const char* expected) {
        const std::string query = std::string("SELECT NOT EXISTS(SELECT * FROM " ) + table +
            " EXCEPT SELECT * FROM " + expected + ") AND NOT EXISTS(SELECT * FROM " + expected +
            " EXCEPT SELECT * FROM " + table + " )";
        sqlite3_stmt* statement = nullptr;
        if (sqlite3_prepare_v2(db.db(), query.c_str(), -1, &statement, nullptr) != SQLITE_OK) return false;
        const bool equal = sqlite3_step(statement) == SQLITE_ROW && sqlite3_column_int(statement, 0) == 1;
        sqlite3_finalize(statement);
        return equal;
    };
    check(preserved("harken_similarity_v2", "expected_similarity"),
          "cancel preserves all existing similarity analysis bytes", failures);
    check(preserved("harken_similarity_segments_v2", "expected_segments"),
          "cancel preserves all existing similarity segment bytes", failures);
    check(db.getFileCount() == 3, "cancel retains all pre-existing audio rows", failures);
    int manualMarkerCount = 0;
    db.execQuery("SELECT COUNT(*) FROM manually_imported_files",
                 [&](sqlite3_stmt* row) { manualMarkerCount = sqlite3_column_int(row, 0); });
    check(manualMarkerCount == 1 && db.fileExists(manuallyImportedPath),
          "cancel preserves explicit import marker with its audio row", failures);
    db.close();
}

void testExplicitFileImportSurvivesAnotherRootScan(int& failures) {
    const fs::path root = makeFixture("manual_import");
    const fs::path dbPath = root / "library.db";
    const fs::path manualPath = root / "outside" / "manual.wav";
    const fs::path existingPath = root / "outside" / "existing.wav";
    const fs::path ordinaryPath = root / "outside" / "ordinary.wav";
    fs::create_directories(manualPath.parent_path());
    for (const auto& path : {manualPath, existingPath, ordinaryPath}) {
        writeSilentMonoWav(path);
    }
    auto recordFor = [](const fs::path& path) {
        AudioRecord record;
        record.filePath = path.u8string();
        record.fileName = path.filename().u8string();
        record.folderPath = path.parent_path().u8string();
        record.extension = ".wav";
        return record;
    };

    Database db;
    if (!db.open(dbPath.u8string())) {
        check(false, "open manual import database", failures);
        return;
    }
    check(true, "open manual import database", failures);
    Database reader;
    const bool readerOpen = reader.openReadOnly(dbPath.u8string());
    check(readerOpen, "open independent count reader", failures);
    if (readerOpen) check(reader.getFileCount() == 0, "reader initially sees empty library", failures);
    const auto manual = recordFor(manualPath);
    check(db.addManuallyImportedFile(manual), "explicit file import succeeds", failures);
    check(db.getFileCount() == 1, "direct import immediately updates total", failures);
    if (readerOpen) check(reader.getFileCount() == 1,
                          "other connection immediately sees direct import count", failures);
    check(db.addManuallyImportedFile(manual) && db.getFileCount() == 1,
          "repeat import does not increase total", failures);

    const auto existing = recordFor(existingPath);
    check(db.addFile(existing), "legacy direct row exists before user re-drop", failures);
    check(db.markFileAsManuallyImported(existing.filePath),
          "user re-drop marks existing row", failures);
    check(db.getFileCount() == 2, "existing row changes no total", failures);
    if (readerOpen) check(reader.getFileCount() == 2,
                          "other connection sees marked existing file count", failures);
    const auto ordinary = recordFor(ordinaryPath);
    check(db.addFile(ordinary), "ordinary out-of-root row added", failures);
    if (readerOpen) check(reader.getFileCount() == 3,
                          "other connection counts a row before index rebuild", failures);

    db.mergeLibraryRoots({(root / "audio").u8string()});
    check(db.pruneEntriesOutsideLibraryRoots() == 1,
          "root pruning removes only ordinary out-of-root row", failures);
    check(db.fileExists(manual.filePath) && db.fileExists(existing.filePath) &&
          !db.fileExists(ordinary.filePath),
          "both explicit file identities survive pruning", failures);
    check(db.getFileCount() == 2, "count remains exact after pruning", failures);
    if (readerOpen) {
        check(reader.getFileCount() == 2, "other connection sees pruned count", failures);
        reader.close();
    }
    db.close();

    Database reopened;
    if (!reopened.open(dbPath.u8string())) {
        check(false, "reopen manual import database", failures);
        return;
    }
    check(true, "reopen manual import database", failures);
    check(reopened.pruneEntriesOutsideLibraryRoots() == 0,
          "explicit markers persist after restart", failures);
    check(reopened.fileExists(manual.filePath) && reopened.fileExists(existing.filePath) &&
          reopened.getFileCount() == 2,
          "reopened library keeps exact files and count", failures);
    reopened.close();
}

} // namespace

int main() {
    int failures = 0;
    testPausePersistsAcrossRestart(failures);
    testCancelStillRollsBack(failures);
    testPostProcessingNeverInflatesFileCounts(failures);
    testRollbackPreservesSimilarityAnalysis(failures);
    testExplicitFileImportSurvivesAnotherRootScan(failures);
    if (failures == 0) {
        std::cout << "All scan pause and resume tests passed.\n";
        return 0;
    }
    std::cerr << failures << " scan pause and resume test(s) failed.\n";
    return 1;
}
