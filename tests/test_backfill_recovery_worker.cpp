#include "BackfillQueue.h"
#include "BackfillWorkerPool.h"
#include "Database.h"

#include "sqlite3.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace {

void check(bool condition, const char* message, int& failures) {
    if (condition) {
        std::cout << "PASS: " << message << "\n";
    } else {
        std::cerr << "FAIL: " << message << "\n";
        ++failures;
    }
}

fs::path makeTempRoot(const std::string& name) {
    auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    fs::path root = fs::temp_directory_path() / ("auro_" + name + "_" + std::to_string(now));
    fs::create_directories(root);
    return root;
}

void removeDbArtifacts(const fs::path& dbPath) {
    std::error_code ec;
    fs::remove(dbPath, ec);
    fs::remove(dbPath.string() + "-wal", ec);
    fs::remove(dbPath.string() + "-shm", ec);
    fs::remove(dbPath.parent_path() / "auro_blobs.db", ec);
    fs::remove(dbPath.parent_path() / "auro_blobs.db-wal", ec);
    fs::remove(dbPath.parent_path() / "auro_blobs.db-shm", ec);
    fs::remove(dbPath.parent_path() / "auro_waveforms.db", ec);
    fs::remove(dbPath.parent_path() / "auro_waveforms.db-wal", ec);
    fs::remove(dbPath.parent_path() / "auro_waveforms.db-shm", ec);
}

void writeSilentMonoWav(const fs::path& path) {
    fs::create_directories(path.parent_path());
    constexpr uint32_t sampleRate = 8000;
    constexpr uint16_t bitsPerSample = 16;
    constexpr uint16_t channels = 1;
    constexpr uint32_t frames = 800;
    constexpr uint32_t dataBytes = frames * channels * (bitsPerSample / 8);
    constexpr uint32_t riffSize = 36 + dataBytes;

    std::ofstream out(path, std::ios::binary);
    out.write("RIFF", 4);
    out.write(reinterpret_cast<const char*>(&riffSize), 4);
    out.write("WAVE", 4);
    out.write("fmt ", 4);
    uint32_t fmtSize = 16;
    uint16_t audioFormat = 1;
    uint32_t byteRate = sampleRate * channels * (bitsPerSample / 8);
    uint16_t blockAlign = channels * (bitsPerSample / 8);
    out.write(reinterpret_cast<const char*>(&fmtSize), 4);
    out.write(reinterpret_cast<const char*>(&audioFormat), 2);
    out.write(reinterpret_cast<const char*>(&channels), 2);
    out.write(reinterpret_cast<const char*>(&sampleRate), 4);
    out.write(reinterpret_cast<const char*>(&byteRate), 4);
    out.write(reinterpret_cast<const char*>(&blockAlign), 2);
    out.write(reinterpret_cast<const char*>(&bitsPerSample), 2);
    out.write("data", 4);
    out.write(reinterpret_cast<const char*>(&dataBytes), 4);
    std::vector<int16_t> silence(frames * channels, 0);
    out.write(reinterpret_cast<const char*>(silence.data()), dataBytes);
}

void writeTinyCoverImage(const fs::path& path) {
    fs::create_directories(path.parent_path());
    const unsigned char png[] = {
        0x89,0x50,0x4E,0x47,0x0D,0x0A,0x1A,0x0A,0x00,0x00,0x00,0x0D,
        0x49,0x48,0x44,0x52,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x01,
        0x08,0x02,0x00,0x00,0x00,0x90,0x77,0x53,0xDE,0x00,0x00,0x00,
        0x0C,0x49,0x44,0x41,0x54,0x08,0xD7,0x63,0xF8,0xCF,0xC0,0x00,
        0x00,0x03,0x01,0x01,0x00,0x18,0xDD,0x8D,0xB0,0x00,0x00,0x00,
        0x00,0x49,0x45,0x4E,0x44,0xAE,0x42,0x60,0x82
    };
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(png), sizeof(png));
}

bool waitForPendingKind(Database& db, aura::BackfillJobKind kind, int expected) {
    for (int i = 0; i < 200; ++i) {
        if (db.countBackfillPendingKind(static_cast<int>(kind)) == expected) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    return false;
}

std::string readSourceFile(const char* relativePath) {
    std::ifstream in(fs::path(AURA_TEST_SOURCE_ROOT) / relativePath, std::ios::binary);
    if (!in.is_open()) return {};
    std::ostringstream buffer;
    buffer << in.rdbuf();
    std::string source = buffer.str();
    for (size_t pos = 0; (pos = source.find("\r\n", pos)) != std::string::npos;) {
        source.replace(pos, 2, "\n");
    }
    return source;
}

bool containsAll(const std::string& source, const std::vector<const char*>& tokens) {
    for (const char* token : tokens) {
        if (source.find(token) == std::string::npos) {
            return false;
        }
    }
    return true;
}

bool appearsBefore(const std::string& source, const char* first, const char* second) {
    const size_t firstPos = source.find(first);
    const size_t secondPos = source.find(second);
    return firstPos != std::string::npos && secondPos != std::string::npos && firstPos < secondPos;
}

std::string sliceBetween(const std::string& source, const char* begin, const char* end) {
    const size_t beginPos = source.find(begin);
    if (beginPos == std::string::npos) return {};
    const size_t endPos = source.find(end, beginPos + 1);
    if (endPos == std::string::npos || endPos <= beginPos) return {};
    return source.substr(beginPos, endPos - beginPos);
}

int testRebuildAndRefillRecoverDurablePendingRows() {
    int failures = 0;
    const fs::path root = makeTempRoot("backfill_recovery");
    const fs::path dbPath = root / "recovery.db";
    writeSilentMonoWav(root / "a.wav");
    writeSilentMonoWav(root / "b.wav");
    writeSilentMonoWav(root / "c.wav");

    Database db;
    if (!db.open(dbPath.u8string())) {
        std::cerr << "FAIL: open db: " << db.getLastError() << "\n";
        return 1;
    }
    const ScanStats stats = db.scanDirectoryEx(root.u8string(), false, false, false,
                                               nullptr, nullptr, 1,
                                               /*completeImport=*/false,
                                               /*enqueueExtraction=*/false);
    check(stats.added == 3, "fixture scan inserts three rows", failures);
    for (const auto& name : {"a.wav", "b.wav", "c.wav"}) {
        const AudioRecord rec = db.getFileByPath((root / name).u8string());
        check(rec.id > 0, "fixture row has id", failures);
        db.markBackfillPending(rec.id, static_cast<int>(aura::BackfillJobKind::WaveformL1Overview));
    }

    aura::BackfillQueue queue;
    queue.onDbOpened(dbPath.u8string(), 41);
    queue.rebuildFromDatabase(&db, 2);
    auto snapshot = queue.snapshot();
    check(snapshot.pendingTableRows == 3, "rebuild records durable pending row count", failures);
    check(snapshot.byKind[static_cast<int>(aura::BackfillJobKind::WaveformL1Overview)] == 2,
          "rebuild only enqueues the requested recovery window", failures);

    aura::BackfillJob first{};
    aura::BackfillJob second{};
    check(queue.tryDequeueKind(aura::BackfillJobKind::WaveformL1Overview, first),
          "first recovered row dequeues", failures);
    check(queue.tryDequeueKind(aura::BackfillJobKind::WaveformL1Overview, second),
          "second recovered row dequeues", failures);
    queue.refillFromDatabaseIfBelowWaterMark(&db, 2, 2);
    snapshot = queue.snapshot();
    check(snapshot.byKind[static_cast<int>(aura::BackfillJobKind::WaveformL1Overview)] == 1,
          "refill advances cursor to the remaining durable row", failures);

    constexpr int64_t foregroundSession = 9001;
    const AudioRecord foreground = db.getFileByPath((root / "c.wav").u8string());
    check(db.resetScanForegroundCompletionSession(foregroundSession),
          "reset durable foreground priority session", failures);
    check(db.seedScanForegroundCompletionRows(
              foregroundSession, {foreground.id}, false, true),
          "seed durable foreground cover row", failures);
    check(db.markBackfillPending(
              foreground.id,
              static_cast<int>(aura::BackfillJobKind::CoverResolveAndThumb)),
          "persist durable foreground cover row", failures);
    aura::BackfillQueue foregroundQueue;
    foregroundQueue.onDbOpened(dbPath.u8string(), 42);
    foregroundQueue.rebuildFromDatabase(&db, 8);
    aura::BackfillJob recoveredForeground{};
    check(foregroundQueue.tryDequeueKind(
              aura::BackfillJobKind::CoverResolveAndThumb,
              recoveredForeground),
          "dequeue durable foreground cover row", failures);
    check(recoveredForeground.scanSessionId == foregroundSession,
          "durable recovery preserves foreground scan session", failures);
    check(recoveredForeground.priority == aura::BackfillPriority::RecentScan,
          "durable rebuild preserves foreground recent-scan priority", failures);
    foregroundQueue.shutdown();

    aura::BackfillQueue foregroundRefillQueue;
    foregroundRefillQueue.onDbOpened(dbPath.u8string(), 43);
    foregroundRefillQueue.rebuildFromDatabase(&db, 2);
    foregroundRefillQueue.refillFromDatabaseIfBelowWaterMark(&db, 3, 8);
    aura::BackfillJob refilledForeground{};
    check(foregroundRefillQueue.tryDequeueKind(
              aura::BackfillJobKind::CoverResolveAndThumb,
              refilledForeground),
          "dequeue refilled durable foreground cover row", failures);
    check(refilledForeground.scanSessionId == foregroundSession,
          "durable refill preserves foreground scan session", failures);
    check(refilledForeground.priority == aura::BackfillPriority::RecentScan,
          "durable refill preserves foreground recent-scan priority", failures);
    foregroundRefillQueue.shutdown();

    queue.shutdown();
    db.close();
    removeDbArtifacts(dbPath);
    fs::remove_all(root);
    return failures;
}

int testWorkerContinuouslyRefillsBeyond4096Rows() {
    int failures = 0;
    const fs::path root = makeTempRoot("backfill_refill_beyond_4096");
    const fs::path dbPath = root / "recovery.db";
    constexpr int kPendingRows = 4097;
    constexpr int64_t kGeneration = 4101;

    Database db;
    if (!db.open(dbPath.u8string())) {
        std::cerr << "FAIL: open >4096 refill db: " << db.getLastError() << "\n";
        return 1;
    }
    check(sqlite3_exec(db.db(), "BEGIN IMMEDIATE TRANSACTION", nullptr, nullptr, nullptr) == SQLITE_OK,
          "begin >4096 durable fixture transaction", failures);
    sqlite3_stmt* insertFile = nullptr;
    sqlite3_stmt* insertPending = nullptr;
    const bool prepared =
        sqlite3_prepare_v2(
            db.db(),
            "INSERT INTO audio_files(file_path, file_name) VALUES(?1, ?2)",
            -1, &insertFile, nullptr) == SQLITE_OK &&
        sqlite3_prepare_v2(
            db.db(),
            "INSERT INTO backfill_pending(file_id, kind, enqueued_at) VALUES(?1, ?2, 1)",
            -1, &insertPending, nullptr) == SQLITE_OK;
    check(prepared, "prepare >4096 durable fixture inserts", failures);
    if (prepared) {
        for (int i = 0; i < kPendingRows; ++i) {
            const std::string path = (root / ("missing_" + std::to_string(i) + ".wav")).u8string();
            const std::string name = "missing_" + std::to_string(i) + ".wav";
            sqlite3_bind_text(insertFile, 1, path.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(insertFile, 2, name.c_str(), -1, SQLITE_TRANSIENT);
            if (sqlite3_step(insertFile) != SQLITE_DONE) {
                ++failures;
                break;
            }
            sqlite3_reset(insertFile);
            sqlite3_clear_bindings(insertFile);
            sqlite3_bind_int64(insertPending, 1, sqlite3_last_insert_rowid(db.db()));
            sqlite3_bind_int(insertPending, 2,
                static_cast<int>(aura::BackfillJobKind::WaveformL1Overview));
            if (sqlite3_step(insertPending) != SQLITE_DONE) {
                ++failures;
                break;
            }
            sqlite3_reset(insertPending);
            sqlite3_clear_bindings(insertPending);
        }
    }
    sqlite3_finalize(insertFile);
    sqlite3_finalize(insertPending);
    check(sqlite3_exec(db.db(), "COMMIT", nullptr, nullptr, nullptr) == SQLITE_OK,
          "commit >4096 durable fixture", failures);
    check(db.countBackfillPending() == kPendingRows,
          "fixture contains more than one 4096-row recovery window", failures);

    aura::BackfillQueue queue;
    queue.onDbOpened(dbPath.u8string(), kGeneration);
    queue.rebuildFromDatabase(&db, 16000);
    const auto initialWindow = queue.snapshot();
    check(initialWindow.byKind[static_cast<int>(aura::BackfillJobKind::WaveformL1Overview)] == 4096,
          "oversized rebuild request is clamped to the bounded 4096-row hot window", failures);
    aura::BackfillWorkerPool workers;
    workers.start(dbPath.u8string(), kGeneration, &queue, 4);
    bool drained = false;
    for (int i = 0; i < 800; ++i) {
        if (db.countBackfillPending() == 0) {
            drained = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    workers.stop();
    check(drained, "worker continuously refills and drains all 4097 durable rows", failures);
    const auto finalSnapshot = queue.snapshot();
    check(finalSnapshot.done == kPendingRows,
          "worker reports every row beyond the first recovery window terminal", failures);

    queue.shutdown();
    db.close();
    removeDbArtifacts(dbPath);
    fs::remove_all(root);
    return failures;
}

int testScanEnqueueKeepsBoundedRuntimeWindow() {
    int failures = 0;
    constexpr int kScanJobs = 16000;
    constexpr int kExpectedWindow = 4096;
    constexpr int64_t kGeneration = 4201;
    const std::string dbPath = "bounded-scan-window.db";

    aura::BackfillQueue queue;
    queue.onDbOpened(dbPath, kGeneration);
    for (int fileId = 1; fileId <= kScanJobs; ++fileId) {
        queue.enqueueFromScan(
            fileId,
            aura::BackfillJobKind::WaveformL1Overview,
            aura::BackfillPriority::RecentScan,
            dbPath,
            kGeneration,
            9201);
    }
    const auto snapshot = queue.snapshot();
    const int queued =
        snapshot.byKind[static_cast<int>(aura::BackfillJobKind::CoverResolveAndThumb)] +
        snapshot.byKind[static_cast<int>(aura::BackfillJobKind::WaveformL1Overview)] +
        snapshot.byKind[static_cast<int>(aura::BackfillJobKind::MetadataRefresh)];
    check(queued == kExpectedWindow,
          "scan hot queue stays bounded instead of retaining all 16000 durable jobs", failures);
    check(snapshot.totalEnqueued == kExpectedWindow,
          "overflow scan jobs remain durable-only for low-water refill", failures);
    queue.shutdown();
    return failures;
}

int testWorkerPoolMarksForegroundCoverNoSourceTerminal() {
    int failures = 0;
    const fs::path root = makeTempRoot("foreground_cover_no_source_worker");
    const fs::path audioPath = root / "Pack" / "Sub" / "no_cover.wav";
    const fs::path dbPath = root / "worker.db";
    constexpr int64_t generation = 43;
    constexpr int64_t scanSessionId = 9101;
    writeSilentMonoWav(audioPath);

    Database db;
    if (!db.open(dbPath.u8string())) {
        std::cerr << "FAIL: open db: " << db.getLastError() << "\n";
        return 1;
    }

    const ScanStats stats = db.scanDirectoryEx(root.u8string(), true, false, false,
                                               nullptr, nullptr, 1,
                                               /*completeImport=*/false,
                                               /*enqueueExtraction=*/false);
    check(stats.added == 1, "cover no-source fixture scan inserts one row", failures);
    const AudioRecord rec = db.getFileByPath(audioPath.u8string());
    check(rec.id > 0, "cover no-source fixture row has id", failures);
    check(db.resetScanForegroundCompletionSession(scanSessionId), "reset cover foreground session", failures);
    check(db.seedScanForegroundCompletionRows(scanSessionId, {rec.id}, false, true),
          "seed cover foreground pending row", failures);
    check(db.markBackfillPending(rec.id, static_cast<int>(aura::BackfillJobKind::CoverResolveAndThumb)),
          "mark cover worker pending row", failures);

    aura::BackfillQueue queue;
    queue.onDbOpened(dbPath.u8string(), generation);
    queue.enqueueFromScan(rec.id,
                          aura::BackfillJobKind::CoverResolveAndThumb,
                          aura::BackfillPriority::RecentScan,
                          dbPath.u8string(),
                          generation,
                          scanSessionId);

    aura::BackfillWorkerPool workers;
    workers.start(dbPath.u8string(), generation, &queue, 1);
    const bool drained = waitForPendingKind(db, aura::BackfillJobKind::CoverResolveAndThumb, 0);
    workers.stop();

    check(drained, "cover no-source worker clears durable pending row", failures);
    const ScanForegroundCompletionCounts counts = db.getScanForegroundCompletionCounts(scanSessionId);
    check(counts.total == 1, "cover no-source foreground ledger keeps one row", failures);
    check(counts.pendingFiles == 0, "cover no-source foreground ledger leaves no pending files", failures);
    check(counts.coverUnresolved == 0, "cover no-source worker marks cover terminal", failures);
    check(counts.miniUnresolved == 0, "cover-only foreground row does not leave mini unresolved", failures);

    queue.shutdown();
    db.close();
    removeDbArtifacts(dbPath);
    fs::remove_all(root);
    return failures;
}

int testWorkerPoolMarksForegroundWaveformCachedTerminal() {
    int failures = 0;
    const fs::path root = makeTempRoot("foreground_waveform_cached_worker");
    const fs::path audioPath = root / "hit.wav";
    const fs::path dbPath = root / "worker.db";
    constexpr int64_t generation = 44;
    constexpr int64_t scanSessionId = 9102;
    writeSilentMonoWav(audioPath);

    Database db;
    if (!db.open(dbPath.u8string())) {
        std::cerr << "FAIL: open db: " << db.getLastError() << "\n";
        return 1;
    }

    const ScanStats stats = db.scanDirectoryEx(root.u8string(), false, false, false,
                                               nullptr, nullptr, 1,
                                               /*completeImport=*/false,
                                               /*enqueueExtraction=*/false);
    check(stats.added == 1, "waveform cached fixture scan inserts one row", failures);
    const AudioRecord rec = db.getFileByPath(audioPath.u8string());
    check(rec.id > 0, "waveform cached fixture row has id", failures);
    check(db.resetScanForegroundCompletionSession(scanSessionId), "reset waveform cached foreground session", failures);
    check(db.seedScanForegroundCompletionRows(scanSessionId, {rec.id}, true, false),
          "seed waveform cached foreground pending row", failures);
    check(db.markBackfillPending(rec.id, static_cast<int>(aura::BackfillJobKind::WaveformL1Overview)),
          "mark waveform cached worker pending row", failures);

    aura::BackfillQueue queue;
    queue.onDbOpened(dbPath.u8string(), generation);
    queue.enqueueFromScan(rec.id,
                          aura::BackfillJobKind::WaveformL1Overview,
                          aura::BackfillPriority::RecentScan,
                          dbPath.u8string(),
                          generation,
                          scanSessionId);

    aura::BackfillWorkerPool workers;
    workers.start(dbPath.u8string(), generation, &queue, 1);
    const bool drained = waitForPendingKind(db, aura::BackfillJobKind::WaveformL1Overview, 0);
    workers.stop();

    check(drained, "waveform cached worker clears durable pending row", failures);
    check(!db.getWaveformByPath(audioPath.u8string()).empty(),
          "waveform cached worker writes mini waveform cache", failures);
    const ScanForegroundCompletionCounts counts = db.getScanForegroundCompletionCounts(scanSessionId);
    check(counts.total == 1, "waveform cached foreground ledger keeps one row", failures);
    check(counts.pendingFiles == 0, "waveform cached foreground ledger leaves no pending files", failures);
    check(counts.miniUnresolved == 0, "waveform cached worker marks mini terminal", failures);
    check(counts.coverUnresolved == 0, "waveform-only foreground row does not leave cover unresolved", failures);

    queue.shutdown();
    db.close();
    removeDbArtifacts(dbPath);
    fs::remove_all(root);
    return failures;
}

int testWorkerPoolDoesNotMarkDifferentForegroundSession() {
    int failures = 0;
    const fs::path root = makeTempRoot("foreground_waveform_session_isolation_worker");
    const fs::path audioPath = root / "hit.wav";
    const fs::path dbPath = root / "worker.db";
    constexpr int64_t generation = 46;
    constexpr int64_t sessionA = 9201;
    constexpr int64_t sessionB = 9202;
    writeSilentMonoWav(audioPath);

    Database db;
    if (!db.open(dbPath.u8string())) {
        std::cerr << "FAIL: open db: " << db.getLastError() << "\n";
        return 1;
    }

    const ScanStats stats = db.scanDirectoryEx(root.u8string(), false, false, false,
                                               nullptr, nullptr, 1,
                                               /*completeImport=*/false,
                                               /*enqueueExtraction=*/false);
    check(stats.added == 1, "session isolation fixture scan inserts one row", failures);
    const AudioRecord rec = db.getFileByPath(audioPath.u8string());
    check(rec.id > 0, "session isolation fixture row has id", failures);
    check(db.resetScanForegroundCompletionSession(sessionA), "reset foreground session A", failures);
    check(db.resetScanForegroundCompletionSession(sessionB), "reset foreground session B", failures);
    check(db.seedScanForegroundCompletionRows(sessionA, {rec.id}, true, false),
          "seed waveform pending row for session A", failures);
    check(db.seedScanForegroundCompletionRows(sessionB, {rec.id}, true, false),
          "seed waveform pending row for session B", failures);
    check(db.markBackfillPending(rec.id, static_cast<int>(aura::BackfillJobKind::WaveformL1Overview)),
          "mark session isolation waveform pending row", failures);

    aura::BackfillQueue queue;
    queue.onDbOpened(dbPath.u8string(), generation);
    queue.enqueueFromScan(rec.id,
                          aura::BackfillJobKind::WaveformL1Overview,
                          aura::BackfillPriority::RecentScan,
                          dbPath.u8string(),
                          generation,
                          sessionA);

    aura::BackfillWorkerPool workers;
    workers.start(dbPath.u8string(), generation, &queue, 1);
    const bool drained = waitForPendingKind(db, aura::BackfillJobKind::WaveformL1Overview, 0);
    workers.stop();

    check(drained, "session isolation worker clears durable waveform pending row", failures);
    const ScanForegroundCompletionCounts countsA = db.getScanForegroundCompletionCounts(sessionA);
    const ScanForegroundCompletionCounts countsB = db.getScanForegroundCompletionCounts(sessionB);
    check(countsA.miniUnresolved == 0, "session A mini terminal is marked", failures);
    check(countsB.miniUnresolved == 1, "session B mini remains unresolved", failures);
    check(countsB.miniTerminal == 0, "session B mini terminal is not generalized by fileId", failures);

    queue.shutdown();
    db.close();
    removeDbArtifacts(dbPath);
    fs::remove_all(root);
    return failures;
}

int testWorkerPoolDoesNotClearForegroundPendingWhenTerminalMarkIsBlocked() {
    int failures = 0;
    const std::string source = readSourceFile("src/BackfillWorkerPool.cpp");
    check(!source.empty(), "worker source is readable for foreground pending contract", failures);
    check(source.find("foregroundTerminalStillUnresolved") != std::string::npos,
          "worker names foreground unresolved terminal-mark failures", failures);
    check(source.find("transientFailure && job.scanSessionId > 0") != std::string::npos,
          "foreground terminal mark failure is classified by scan session", failures);
    check(source.find("!foregroundTerminalStillUnresolved") != std::string::npos &&
              source.find("clearBackfillPendingWithBlockingGate") != std::string::npos,
          "foreground terminal mark failure blocks durable pending clear", failures);
    return failures;
}

int testScanDrainDoesNotClearFailedPermanentWithoutForegroundTerminal() {
    int failures = 0;
    const std::string nativeSource = readSourceFile("src/main.cpp");
    const std::string qaSource = readSourceFile("src/WindowsMain.cpp");
    const std::string drainWorkflowSource =
        readSourceFile("src/ScanWaveformBackfillDrainWorkflow.cpp");
    check(!nativeSource.empty(), "native scan drain source is readable", failures);
    check(!qaSource.empty(), "QA scan drain source is readable", failures);
    check(nativeSource.find("stale_in_flight") != std::string::npos &&
              nativeSource.find("markScanForegroundMiniTerminal") != std::string::npos,
          "native stale foreground jobs write terminal state before pending clear", failures);
    check(qaSource.find("stale_in_flight") != std::string::npos &&
              qaSource.find("markScanForegroundMiniTerminal") != std::string::npos,
          "QA stale foreground jobs write terminal state before pending clear", failures);
    check(nativeSource.find("queueState == aura::BackfillItemState::Cached ||") ==
              std::string::npos,
          "native scan drain does not clear FailedPermanent rows without terminal proof", failures);
    check(qaSource.find("queueState == aura::BackfillItemState::Cached ||") ==
              std::string::npos,
          "QA scan drain does not clear FailedPermanent rows without terminal proof", failures);
    check(nativeSource.find("queue_cached") != std::string::npos &&
              nativeSource.find("static_cast<int64_t>(scanGeneration)") != std::string::npos,
          "native cached scan drain writes current-session terminal proof before pending clear", failures);
    check(qaSource.find("queue_cached") != std::string::npos &&
              qaSource.find("static_cast<int64_t>(scanGeneration)") != std::string::npos,
          "QA cached scan drain writes current-session terminal proof before pending clear", failures);
    check(containsAll(nativeSource, {
              "state.backfillQueue.enqueueFromScan(",
              "state.backfillQueue.currentGeneration()",
              "static_cast<int64_t>(scanGeneration)",
              "++requeuedMissing",
          }),
          "native unknown scan drain requeue preserves current scan session", failures);
    check(containsAll(qaSource, {
              "state.backfillQueue.enqueueFromScan(",
              "state.backfillQueue.currentGeneration()",
              "static_cast<int64_t>(scanGeneration)",
              "++requeuedMissing",
          }),
          "QA unknown scan drain requeue preserves current scan session", failures);
    check(nativeSource.find("rebuildFromDatabase(&scanDb, 50000)") == std::string::npos,
          "native scan drain does not rebuild foreground jobs without session ids", failures);
    check(qaSource.find("rebuildFromDatabase(&scanDb, 50000)") == std::string::npos,
          "QA scan drain does not rebuild foreground jobs without session ids", failures);
    check(nativeSource.find("refillFromDatabaseIfBelowWaterMark(&scanDb, 128, 512)") ==
              std::string::npos,
          "native scan drain does not recover foreground pending rows without session ids", failures);
    check(qaSource.find("refillFromDatabaseIfBelowWaterMark(&scanDb, 128, 512)") ==
              std::string::npos,
          "QA scan drain does not recover foreground pending rows without session ids", failures);
    check(nativeSource.find("!state.scanCompletionPending.load(std::memory_order_acquire) &&") !=
              std::string::npos,
          "native scan dialog summary is blocked while foreground completion is pending", failures);
    check(qaSource.find("scan foreground completion pending") != std::string::npos,
          "QA acknowledge command rejects foreground completion pending state", failures);
    check(readSourceFile("src/BackfillWorkerPool.cpp").find("markForegroundMiniFailureOrTransient") !=
              std::string::npos,
          "worker missing waveform row records foreground terminal failure before pending clear", failures);
    check(readSourceFile("src/BackfillQueue.cpp").find("row.scanSessionId") != std::string::npos,
          "worker durable refill/rebuild preserves foreground scan session ids", failures);
    check(readSourceFile("src/BackfillQueue.h").find(
              "void rebuildFromDatabase(Database* db, int windowSize = 4096)") != std::string::npos,
          "durable recovery defaults to the bounded 4096-row hot window", failures);
    check(readSourceFile("src/Database.cpp").find(
              "backfillQueue->rebuildFromDatabase(this, 50000)") == std::string::npos,
          "scan rollback does not enqueue a 50000-row durable backlog", failures);
    check(readSourceFile("src/Database.cpp").find("scanTerminalFailure") != std::string::npos,
          "scan failed files still enter the foreground completion ledger", failures);
    check(readSourceFile("src/Database.cpp").find("scan_file_failed") != std::string::npos,
          "scan failed files record explicit mini/cover failed terminal states", failures);
    check(readSourceFile("src/Database.cpp").find("Audio file too small to decode") != std::string::npos,
          "too-small audio files are classified as foreground terminal failures", failures);
    check(readSourceFile("src/Database.cpp").find("Failed to queue audio file") != std::string::npos &&
              readSourceFile("src/Database.cpp").find("failed.scanTerminalFailure = true") != std::string::npos,
          "handleAudioPath exceptions are classified as foreground terminal failures", failures);
    check(nativeSource.find("const bool showBackgroundContinueButton =") != std::string::npos &&
              nativeSource.find("state.isScanning.load() && !scanForegroundCompletionActive") !=
                  std::string::npos &&
              nativeSource.find("if (showBackgroundContinueButton)") != std::string::npos,
          "native scan dialog cannot hide current-scan foreground completion behind background continue", failures);
    check(containsAll(nativeSource, {
              "state.beginScanWorkflow(state.settings.cacheWaveformsOnScan);",
              "state.scanDiscoveredCount.store(",
              "state.scanProcessedCount.store(",
              "HarkenSearch::ScanWorkflowStage::Ingesting",
              "state.setScanOutcome(",
              "state.scanFatalFailure.store(",
              "HarkenSearch::ScanWorkflowStage::CachingMiniWaveforms",
              "state.setScanMiniWaveformProgress(",
              "state.scanForegroundTasksDrained.store(",
              "state.resolveScanWorkflowTerminalStage(",
          }),
          "native scan path wires every scan workflow transition to truthful runtime data",
          failures);
    check(nativeSource.find("scanCompleteSummaryActive") != std::string::npos &&
              nativeSource.find("HarkenSearch::BuildScanProcessingProgress(scanWorkflowSnapshot)") != std::string::npos &&
              nativeSource.find("scanWorkflowSnapshot.stage == HarkenSearch::ScanWorkflowStage::Completed") != std::string::npos &&
              nativeSource.find("HarkenSearch::CanEnterCompleted(scanWorkflowSnapshot)") != std::string::npos &&
              nativeSource.find("L(Main_ScannedProcessedFmt)") != std::string::npos,
          "native scan dialog uses processed/discovered progress and a guarded completed state", failures);
    check(nativeSource.find("const bool miniCacheStage =") != std::string::npos &&
              nativeSource.find("static_cast<float>(scanWorkflowSnapshot.miniWaveformsCompleted)") != std::string::npos &&
              nativeSource.find("float instant = (speedBasis - lastSampleProgress)") != std::string::npos &&
              nativeSource.find("speedStr = 0.0f;") != std::string::npos,
          "native scan dialog speed switches to mini completion only during the mini-cache stage",
          failures);
    check(containsAll(nativeSource, {
              "L(Main_FoundFilesFmt)",
              "L(Main_ImportedFmt)",
              "L(Main_SkippedDupInvalidFmt)",
              "L(Main_FailedFmt)",
              "L(Main_MiniCacheProgressFmt)",
              "if (scanCompleteSummaryActive)",
        "if (aura::ui::ActionButton(L(Done), ImVec2(btnW, 30)))",
              "state.clearScanWorkflowState();",
          }),
          "completed scan summary exposes five truthful counts and waits for the Done button",
          failures);
    check(readSourceFile("src/AppState.h").find("scanCompletionDurationMs") != std::string::npos &&
              readSourceFile("src/AppState.h").find("beginScanTiming();") != std::string::npos &&
              nativeSource.find("state.markScanCompletionReadyNow();") != std::string::npos &&
              qaSource.find("state.markScanCompletionReadyNow();") != std::string::npos,
          "scan completion duration is frozen from scan start to foreground completion gate",
          failures);
    check(nativeSource.find("FormatDurationMsCompact(state.scanCompletionDurationMs.load") != std::string::npos &&
              qaSource.find("scan_completion_duration_ms") != std::string::npos,
          "UI and QA expose scan completion wall-clock duration instead of ingest-only duration",
          failures);
    check(nativeSource.find("state.scanCompletionMetadataUnresolved.load(std::memory_order_acquire) > 0") != std::string::npos &&
              qaSource.find("state.scanCompletionMetadataUnresolved.load(std::memory_order_acquire) > 0") != std::string::npos,
          "native and QA foreground completion pending gates include metadata debt",
          failures);
    check(readSourceFile("src/Database.cpp").find("metadata_state") != std::string::npos &&
              readSourceFile("src/BackfillWorkerPool.cpp").find("markScanForegroundMetadataTerminal") != std::string::npos &&
              readSourceFile("src/Database.cpp").find("BackfillJobKind::MetadataRefresh") != std::string::npos &&
              readSourceFile("src/Database.cpp").find("scanForegroundSessionId);") != std::string::npos,
          "foreground completion ledger includes hidden metadata terminal debt", failures);
    {
        const std::string dbSource = readSourceFile("src/Database.cpp");
        check(containsAll(dbSource, {
                  "currentScanCompletionBackfillPriority",
                  "scanForegroundSessionId > 0",
                  "? aura::BackfillPriority::RecentScan",
                  ": aura::BackfillPriority::IdleBackfill",
                  "aura::BackfillJobKind::MetadataRefresh,\n                            currentScanCompletionBackfillPriority(),",
                  "aura::BackfillJobKind::CoverResolveAndThumb,\n                            currentScanCompletionBackfillPriority(),",
              }),
              "foreground scan cover/metadata backfill uses RecentScan priority while non-foreground work remains idle",
              failures);
    }
    {
        const std::string dbSource = readSourceFile("src/Database.cpp");
        const std::string clearPendingBody = sliceBetween(
            dbSource,
            "bool Database::clearBackfillPending(int64_t fileId, int kind)",
            "bool Database::resetScanForegroundCompletionSession");
        check(!clearPendingBody.empty() &&
                  clearPendingBody.find("checkoutStmt(sql)") != std::string::npos &&
                  clearPendingBody.find("returnStmt(sql, stmt)") != std::string::npos &&
                  clearPendingBody.find("sqlite3_prepare_v2") == std::string::npos &&
                  clearPendingBody.find("sqlite3_finalize(stmt)") == std::string::npos,
              "backfill pending clear reuses the statement pool instead of compiling SQL per completed job",
              failures);
    }
    {
        const std::string dbSource = readSourceFile("src/Database.cpp");
        const std::string terminalRegion = sliceBetween(
            dbSource,
            "bool Database::markScanForegroundMiniTerminal(int64_t scanSessionId",
            "ScanForegroundCompletionCounts Database::getScanForegroundCompletionCounts");
        check(!terminalRegion.empty() &&
                  containsAll(terminalRegion, {
                      "UPDATE scan_foreground_completion SET mini_state = ?",
                      "UPDATE scan_foreground_completion SET cover_state = ?",
                      "UPDATE scan_foreground_completion SET metadata_state = ?",
                      "checkoutStmt(sql)",
                      "returnStmt(sql, stmt)",
                      "checkoutStmt(existsSql)",
                      "returnStmt(existsSql, stmt)",
                  }) &&
                  terminalRegion.find("sqlite3_prepare_v2") == std::string::npos &&
                  dbSource.find("markScanForegroundTerminalByFile") == std::string::npos,
              "scan foreground terminal marks reuse statement-pool SQL instead of compiling per mini/cover/metadata job",
              failures);
    }
    {
        const std::string dbSource = readSourceFile("src/Database.cpp");
        const std::string completionCountsBody = sliceBetween(
            dbSource,
            "ScanForegroundCompletionCounts Database::getScanForegroundCompletionCounts",
            "int Database::countBackfillPending()");
        check(!completionCountsBody.empty() &&
                  completionCountsBody.find("checkoutStmt(sql)") != std::string::npos &&
                  completionCountsBody.find("returnStmt(sql, stmt)") != std::string::npos &&
                  completionCountsBody.find("sqlite3_prepare_v2") == std::string::npos &&
                  completionCountsBody.find("sqlite3_finalize(stmt)") == std::string::npos,
              "scan foreground completion polling reuses the statement pool instead of compiling SQL per UI/QA refresh",
              failures);
    }
    {
        const std::string dbSource = readSourceFile("src/Database.cpp");
        const std::string pendingCountsRegion = sliceBetween(
            dbSource,
            "int Database::countBackfillPending()",
            "std::vector<BackfillPendingRow> Database::getBackfillPendingRows(");
        check(!pendingCountsRegion.empty() &&
                  pendingCountsRegion.find("checkoutStmt(sql)") != std::string::npos &&
                  pendingCountsRegion.find("returnStmt(sql, stmt)") != std::string::npos &&
                  pendingCountsRegion.find("sqlite3_prepare_v2") == std::string::npos &&
                  pendingCountsRegion.find("sqlite3_finalize(stmt)") == std::string::npos,
              "backfill pending count queries reuse the statement pool during scan-ready and queue-refill polling",
              failures);
    }
    {
        const std::string dbSource = readSourceFile("src/Database.cpp");
        const std::string pendingRowsRegion = sliceBetween(
            dbSource,
            "std::vector<BackfillPendingRow> Database::getBackfillPendingRows(",
            "void Database::preheat()");
        check(!pendingRowsRegion.empty() &&
                  pendingRowsRegion.find("checkoutStmt(sql)") != std::string::npos &&
                  pendingRowsRegion.find("returnStmt(sql, stmt)") != std::string::npos &&
                  pendingRowsRegion.find("sqlite3_prepare_v2") == std::string::npos &&
                  pendingRowsRegion.find("sqlite3_finalize(stmt)") == std::string::npos,
              "backfill pending row refill queries reuse the statement pool instead of compiling SQL per drain window",
              failures);
    }
    {
        const std::string dbSource = readSourceFile("src/Database.cpp");
        const std::string saveMiniBody = sliceBetween(
            dbSource,
            "bool Database::saveWaveformByPath(const std::string& filePath",
            "std::vector<float> Database::getWaveformByPath");
        check(!saveMiniBody.empty() &&
                  saveMiniBody.find("checkoutStmt(idSql)") != std::string::npos &&
                  saveMiniBody.find("returnStmt(idSql, idStmt)") != std::string::npos &&
                  saveMiniBody.find("checkoutStmt(ensureSql)") != std::string::npos &&
                  saveMiniBody.find("returnStmt(ensureSql, ensureStmt)") != std::string::npos &&
                  saveMiniBody.find("checkoutStmt(sql)") != std::string::npos &&
                  saveMiniBody.find("returnStmt(sql, stmt)") != std::string::npos &&
                  saveMiniBody.find("sqlite3_prepare_v2") == std::string::npos &&
                  saveMiniBody.find("sqlite3_finalize") == std::string::npos,
              "mini waveform cache writes reuse the statement pool instead of compiling SQL per worker result",
              failures);
    }
    {
        const std::string dbSource = readSourceFile("src/Database.cpp");
        const std::string cacheStateBody = sliceBetween(
            dbSource,
            "bool Database::upsertWaveformCacheStateByPath(const std::string& filePath",
            "bool Database::getWaveformCacheStateByPath");
        check(!cacheStateBody.empty() &&
                  cacheStateBody.find("checkoutStmt(sql)") != std::string::npos &&
                  cacheStateBody.find("returnStmt(sql, stmt)") != std::string::npos &&
                  cacheStateBody.find("sqlite3_prepare_v2") == std::string::npos &&
                  cacheStateBody.find("sqlite3_finalize(stmt)") == std::string::npos,
              "waveform cache state upserts reuse the statement pool instead of compiling SQL per worker result",
              failures);
    }
    {
        const std::string poolHeader = readSourceFile("src/BackfillWorkerPool.h");
        const std::string poolSource = readSourceFile("src/BackfillWorkerPool.cpp");
        check(containsAll(poolHeader, {
                  "#include <unordered_map>",
                  "std::mutex m_folderCoverLookupMutex;",
                  "std::unordered_map<std::string, std::string> m_folderCoverLookupCache;",
              }) &&
                  containsAll(poolSource, {
                      "lookupFolderCoverImagePathCached",
                      "CoverModule::GetFolderCoverImagePathBackground(folderPath, resolved, true, true)",
                      "folderCoverLookupCache.emplace(folderPath,",
                      "m_folderCoverLookupCache.clear();",
                      "processCoverJob(db, job, rec, transientFailure, m_folderCoverLookupMutex, m_folderCoverLookupCache)",
                  }),
              "cover backfill caches per-folder cover lookup results for one worker-pool run",
              failures);
    }
    check(nativeSource.find("rollbackActiveScanSession(&state.backfillQueue)") != std::string::npos &&
              qaSource.find("rollbackActiveScanSession(&state.backfillQueue)") != std::string::npos,
          "late foreground cancel routes both native and QA scan threads back through scan-session rollback", failures);
    check(containsAll(nativeSource, {
              "const bool foregroundCompletionActive",
              "state.scanCompletionPending.load(std::memory_order_acquire)",
              "DrainScanWaveformBackfillQueue(",
          }) &&
              containsAll(drainWorkflowSource, {
                  "if (!request.autoBackfillEnabled &&",
                  "!request.foregroundCompletionActive",
                  "const bool admissionScanning = observations.admissionScanning()",
                  "admissionScanning && !request.foregroundCompletionActive",
              }),
          "native foreground completion keeps scan mini-waveform drain enabled while isScanning is true",
          failures);
    check(qaSource.find("return !IsHarkenSearchHarnessMultiInstanceEnabled();") != std::string::npos &&
              qaSource.find("AURA_DISABLE_SINGLE_INSTANCE") != std::string::npos &&
              qaSource.find("two foreground scanners") != std::string::npos,
          "product launches use single-instance broker unless an explicit harness or opt-out lane is active",
          failures);
    check(containsAll(nativeSource, {
              "scanForegroundWaveformDrainActive",
              "state.scanCompletionWaveformsPending.load(std::memory_order_acquire)",
              "shouldThrottleWaveformWorker",
              "!scanForegroundWaveformDrainActive &&",
              "state.waveformWorker.setThrottleMode(shouldThrottleWaveformWorker)",
          }) &&
              containsAll(qaSource, {
                  "scanForegroundWaveformDrainActive",
                  "state.scanCompletionWaveformsPending.load(std::memory_order_acquire)",
                  "throttleWaveformWorker",
                  "!scanForegroundWaveformDrainActive &&",
                  "state.waveformWorker.setThrottleMode(throttleWaveformWorker)",
              }),
          "foreground scan mini completion disables waveform worker throttling so priority-0 scan backfill cannot starve",
          failures);
    check(containsAll(qaSource, {
              "if (result.hasBackfillJob)",
              "aura::BackfillCompletionOutcome::Success",
              "aura::BackfillCompletionOutcome::TransientFailure",
              "aura::BackfillCompletionOutcome::PermanentFailure",
              "state.backfillQueue.reportOutcome(result.backfillJob",
              "CanClearBackfillPendingForJob(",
              "state.database.clearBackfillPending(",
              "result.cacheOnly",
          }) &&
              appearsBefore(qaSource,
                            "if (result.hasBackfillJob)",
                            "Drop stale/off-page list waveform results"),
          "runtime waveform result loop writes scan foreground mini terminal proof before dropping off-page UI waveforms",
          failures);
    check(containsAll(nativeSource, {
              "while (state.isScanTaskCurrent(scanGeneration) &&",
              "state.scanCompletionPending.load(std::memory_order_acquire) &&",
              "!state.isScanTaskCancelled(scanGeneration, scanCancelToken))",
          }) &&
              containsAll(qaSource, {
                  "while (state.isScanTaskCurrent(scanGeneration) &&",
                  "state.scanCompletionPending.load(std::memory_order_acquire) &&",
                  "!state.isScanTaskCancelled(scanGeneration, scanCancelToken))",
              }),
          "native and QA scan threads stay alive while foreground completion debt is pending", failures);
    check(qaSource.find("scan_completion_pending_files") != std::string::npos &&
              qaSource.find("scan_completion_mini_unresolved") != std::string::npos &&
              qaSource.find("scan_completion_cover_unresolved") != std::string::npos &&
              qaSource.find("scan_completion_metadata_unresolved") != std::string::npos,
          "QA state exposes foreground completion pending-file and unresolved counts for CLI verification", failures);
    check(containsAll(qaSource, {
              "BackfillResultConsume: consumed=%d cacheOnly=%d backfill=%d terminal=%d clear=%d deferred=%d staleDrop=%d uiDrop=%d durationMs=%.2f",
              "terminalUnitDelta",
              "terminalFileRate",
              "ScanCompletionThroughput: session=%lld total=%d pendingFiles=%d miniUnresolved=%d coverUnresolved=%d metadataUnresolved=%d terminalUnitDelta=%d terminalFileRate=%.2f miniDoneDelta=%d miniDoneRate=%.2f",
              "scanForegroundPending=%d",
          }) &&
              appearsBefore(qaSource,
                            "int consumed = state.waveformWorker.pollResults",
                            "BackfillResultConsume: consumed=%d cacheOnly=%d backfill=%d terminal=%d clear=%d deferred=%d staleDrop=%d uiDrop=%d durationMs=%.2f") &&
              appearsBefore(qaSource,
                            "const ScanForegroundCompletionCounts counts =",
                            "ScanCompletionThroughput: session=%lld total=%d pendingFiles=%d miniUnresolved=%d coverUnresolved=%d metadataUnresolved=%d terminalUnitDelta=%d terminalFileRate=%.2f miniDoneDelta=%d miniDoneRate=%.2f"),
          "runtime scan foreground throughput diagnostics count cover, metadata, and mini terminal work instead of only mini decay",
          failures);
    {
        const std::string mainSource = readSourceFile("src/main.cpp");
        check(containsAll(mainSource, {
                  "const bool miniCacheStage =",
                  "static_cast<float>(scanWorkflowSnapshot.miniWaveformsCompleted)",
                  ": static_cast<float>(curProg);",
              }),
              "scan dialog speed samples mini progress only while mini cache work is active",
              failures);
    }
    {
        const std::string poolSource = readSourceFile("src/BackfillWorkerPool.cpp");
        check(containsAll(poolSource, {
                  "#if AURA_BUILD_DIAGNOSTICS",
                  "struct BackfillWorkerThroughputStats",
                  "emitBackfillWorkerThroughput(",
                  "recordBackfillWorkerThroughput(",
                  "emitAllBackfillWorkerThroughput(",
                  "WorkerThroughput: idx=%d kind=%s reason=%s count=%d success=%d transient=%d permanent=%d avgMs=%.2f maxMs=%.2f rate=%.2f",
                  "BackfillCompletionOutcome::TransientFailure",
                  "stats.count >= 128",
              }),
              "backfill worker throughput diagnostics aggregate per-kind outcomes without per-file log spam",
              failures);
    }
    {
        const std::string perfSource = readSourceFile("tests/test_scan_waveform_backfill_perf.cpp");
        check(containsAll(perfSource, {
                  "parseKindArg(",
                  "kindName(",
                  "seedPendingBackfillRows(",
                  "aura::BackfillJobKind::CoverResolveAndThumb",
                  "aura::BackfillJobKind::WaveformL1Overview",
                  "aura::BackfillJobKind::MetadataRefresh",
                  "result.kind",
              }),
              "scan backfill perf harness can benchmark cover, waveform, and metadata completion debt separately",
              failures);
    }
    {
        const std::string miniPerfSource = readSourceFile("tests/test_mini_waveform_decode_perf.cpp");
        check(containsAll(miniPerfSource, {
                  "targetPeaks",
                  "if (args.size() >= 4)",
                  "WaveformExtractor::ExtractMini(pathToUtf8(file), targetPeaks,",
                  "target_peaks",
              }),
              "mini waveform decode perf harness can compare 128-peak scan backfill cost against 160-peak music cost",
              failures);
    }
    check(containsAll(qaSource, {
              "|| scanForegroundPending",
              "if (scanForegroundPending) {",
              "BG_scan_completion_pending sleep=16 scanForegroundPending=%d",
              "BG_pending sleep=33 playing=%d scanForegroundPending=%d",
          }) &&
              appearsBefore(qaSource,
                            "BG_scan_completion_pending sleep=16 scanForegroundPending=%d",
                            "BG_pending sleep=33 playing=%d scanForegroundPending=%d"),
          "runtime frame throttle gives foreground scan completion a 16ms fast lane before generic background pending sleep",
          failures);
    check(readSourceFile("src/AppState.h").find("scanReadyToWorkMutex") != std::string::npos &&
              nativeSource.find("std::lock_guard<std::recursive_mutex> lk(state.scanReadyToWorkMutex)") !=
                  std::string::npos &&
              qaSource.find("GetScanReadyToWorkSnapshotForQa") != std::string::npos,
          "ready-to-work state sharing is protected while scan completion uses foreground progress", failures);
    check(containsAll(qaSource, {
              "cmd == \"scroll_results\"",
              "state.currentTab().listScrollY = (float)(requestedTopRow * rowHeight);",
              "state.currentTab().restoreScrollY = true;",
              "DIAG_ACTION_FMT(\"Browse\", \"ScrollTopRow:%d\", requestedTopRow);",
              "\"cmd\":\"scroll_results\"",
          }),
          "QA runtime exposes a dedicated scroll_results command that restores the result list near a requested top row",
          failures);
    {
        const size_t restoreBranch = nativeSource.find(
            "if (!state.listTabs.empty() && state.currentTab().restoreScrollY)");
        const size_t restoreSet = nativeSource.find(
            "ImGui::SetScrollY(targetScrollY)", restoreBranch);
        const size_t stablePersistBranch = nativeSource.find(
            "} else if (!state.listTabs.empty())", restoreSet);
        const size_t stablePersist = nativeSource.find(
            "state.currentTab().listScrollY = ImGui::GetScrollY();", stablePersistBranch);
        check(restoreBranch != std::string::npos &&
                  restoreSet != std::string::npos &&
                  stablePersistBranch != std::string::npos &&
                  stablePersist != std::string::npos &&
                  restoreBranch < restoreSet &&
                  restoreSet < stablePersistBranch &&
                  stablePersistBranch < stablePersist,
              "result list restoreScrollY applies the target before stable live scroll persistence",
              failures);
    }
    check(nativeSource.find("WaveformCacheBackground") == std::string::npos &&
              qaSource.find("WaveformCacheBackground") == std::string::npos,
          "scan foreground drain diagnostics are not labeled as background optimization", failures);
    return failures;
}

int testDurableRefillRearmsCachedRuntimeEntry() {
    int failures = 0;
    const fs::path root = makeTempRoot("backfill_rearm_cached_runtime");
    const fs::path dbPath = root / "rearm.db";
    const fs::path audioPath = root / "rearm.wav";
    constexpr int64_t generation = 42;
    constexpr int64_t previousScanSession = 9201;
    constexpr int64_t currentScanSession = 9202;
    writeSilentMonoWav(audioPath);

    Database db;
    if (!db.open(dbPath.u8string())) {
        std::cerr << "FAIL: open db: " << db.getLastError() << "\n";
        return 1;
    }
    const ScanStats stats = db.scanDirectoryEx(root.u8string(), false, false, false,
                                               nullptr, nullptr, 1,
                                               /*completeImport=*/false,
                                               /*enqueueExtraction=*/false);
    check(stats.added == 1, "rearm fixture scan inserts one row", failures);
    const AudioRecord rec = db.getFileByPath(audioPath.u8string());
    check(rec.id > 0, "rearm fixture row has id", failures);

    aura::BackfillQueue queue;
    queue.onDbOpened(dbPath.u8string(), generation);
    queue.enqueueFromScan(rec.id,
                          aura::BackfillJobKind::WaveformL1Overview,
                          aura::BackfillPriority::RecentScan,
                          dbPath.u8string(),
                          generation,
                          previousScanSession);
    aura::BackfillJob previousJob{};
    check(queue.tryDequeue(previousJob), "previous scan job dequeues", failures);
    queue.reportOutcome(previousJob, aura::BackfillCompletionOutcome::Success);
    check(queue.itemState(rec.id, aura::BackfillJobKind::WaveformL1Overview) ==
              aura::BackfillItemState::Cached,
          "previous scan leaves a cached runtime entry", failures);

    check(db.resetScanForegroundCompletionSession(currentScanSession),
          "reset current rearm foreground session", failures);
    check(db.seedScanForegroundCompletionRows(currentScanSession, {rec.id}, true, false),
          "seed current rearm mini ledger row", failures);
    check(db.markBackfillPending(
              rec.id, static_cast<int>(aura::BackfillJobKind::WaveformL1Overview)),
          "persist current rearm waveform work", failures);

    queue.refillFromDatabaseIfBelowWaterMark(&db, 128, 512);
    check(queue.itemState(rec.id, aura::BackfillJobKind::WaveformL1Overview) ==
              aura::BackfillItemState::Pending,
          "durable pending work rearms a stale cached runtime entry", failures);
    aura::BackfillJob currentJob{};
    check(queue.tryDequeue(currentJob), "rearmed durable waveform job dequeues", failures);
    check(currentJob.scanSessionId == currentScanSession,
          "rearmed job carries the current foreground scan session", failures);

    queue.shutdown();
    db.close();
    removeDbArtifacts(dbPath);
    fs::remove_all(root);
    return failures;
}

int testDurableRefillRearmsPermanentFailureRuntimeEntry() {
    int failures = 0;
    const fs::path root = makeTempRoot("backfill_rearm_failed_runtime");
    const fs::path dbPath = root / "rearm_failed.db";
    const fs::path audioPath = root / "rearm_failed.wav";
    constexpr int64_t generation = 43;
    constexpr int64_t previousScanSession = 9301;
    constexpr int64_t currentScanSession = 9302;
    writeSilentMonoWav(audioPath);

    Database db;
    if (!db.open(dbPath.u8string())) {
        std::cerr << "FAIL: open db: " << db.getLastError() << "\n";
        return 1;
    }
    const ScanStats stats = db.scanDirectoryEx(root.u8string(), false, false, false,
                                               nullptr, nullptr, 1,
                                               /*completeImport=*/false,
                                               /*enqueueExtraction=*/false);
    check(stats.added == 1, "rearm failed fixture scan inserts one row", failures);
    const AudioRecord rec = db.getFileByPath(audioPath.u8string());
    check(rec.id > 0, "rearm failed fixture row has id", failures);

    aura::BackfillQueue queue;
    queue.onDbOpened(dbPath.u8string(), generation);
    queue.enqueueFromScan(rec.id,
                          aura::BackfillJobKind::WaveformL1Overview,
                          aura::BackfillPriority::RecentScan,
                          dbPath.u8string(),
                          generation,
                          previousScanSession);
    aura::BackfillJob previousJob{};
    check(queue.tryDequeue(previousJob), "previous failed scan job dequeues", failures);
    queue.reportOutcome(previousJob, aura::BackfillCompletionOutcome::PermanentFailure);
    check(queue.itemState(rec.id, aura::BackfillJobKind::WaveformL1Overview) ==
              aura::BackfillItemState::FailedPermanent,
          "previous scan leaves a permanent failure runtime entry", failures);

    check(db.resetScanForegroundCompletionSession(currentScanSession),
          "reset current failed rearm foreground session", failures);
    check(db.seedScanForegroundCompletionRows(currentScanSession, {rec.id}, true, false),
          "seed current failed rearm mini ledger row", failures);
    check(db.markBackfillPending(
              rec.id, static_cast<int>(aura::BackfillJobKind::WaveformL1Overview)),
          "persist current failed rearm waveform work", failures);

    queue.refillFromDatabaseIfBelowWaterMark(&db, 128, 512);
    check(queue.itemState(rec.id, aura::BackfillJobKind::WaveformL1Overview) ==
              aura::BackfillItemState::Pending,
          "durable pending work rearms a stale permanent failure runtime entry", failures);
    aura::BackfillJob currentJob{};
    check(queue.tryDequeue(currentJob), "rearmed durable failed waveform job dequeues", failures);
    check(currentJob.scanSessionId == currentScanSession,
          "rearmed failed job carries the current foreground scan session", failures);

    queue.shutdown();
    db.close();
    removeDbArtifacts(dbPath);
    fs::remove_all(root);
    return failures;
}

int testForegroundTerminalMarkRequiresSeededRow() {
    int failures = 0;
    const fs::path root = makeTempRoot("foreground_terminal_requires_seed");
    const fs::path dbPath = root / "worker.db";
    const fs::path audioPath = root / "seeded.wav";
    constexpr int64_t scanSessionId = 9301;

    Database db;
    if (!db.open(dbPath.u8string())) {
        std::cerr << "FAIL: open db: " << db.getLastError() << "\n";
        return 1;
    }
    writeSilentMonoWav(audioPath);
    AudioRecord rec;
    rec.filePath = audioPath.u8string();
    rec.fileName = "seeded.wav";
    rec.folderPath = root.u8string();
    rec.extension = ".wav";
    rec.fileSize = 4096;
    rec.fileMtime = 123;
    check(db.addFile(rec), "insert foreground terminal seed fixture row", failures);
    rec = db.getFileByPath(audioPath.u8string());
    check(rec.id > 0, "foreground terminal seed fixture row has id", failures);

    check(db.resetScanForegroundCompletionSession(scanSessionId),
          "reset foreground terminal missing-row session", failures);
    check(!db.markScanForegroundMiniTerminal(
              scanSessionId,
              rec.id,
              ScanForegroundTerminalState::Cached,
              "missing_seed"),
          "foreground terminal mark fails when the scan-session row was never seeded", failures);
    check(db.seedScanForegroundCompletionRows(scanSessionId, {rec.id}, true, false),
          "seed foreground terminal row", failures);
    check(db.markScanForegroundMiniTerminal(
              scanSessionId,
              rec.id,
              ScanForegroundTerminalState::Cached,
              "seeded"),
          "foreground terminal mark succeeds after seed", failures);
    check(db.markScanForegroundMiniTerminal(
              scanSessionId,
              rec.id,
              ScanForegroundTerminalState::Cached,
              "idempotent"),
          "foreground terminal mark remains idempotent for an already terminal row", failures);

    db.close();
    removeDbArtifacts(dbPath);
    fs::remove_all(root);
    return failures;
}

int testForegroundSplitSeedKeepsBothDimensionsPending() {
    int failures = 0;
    const fs::path root = makeTempRoot("foreground_split_seed");
    const fs::path dbPath = root / "worker.db";
    const fs::path audioPath = root / "both.wav";
    constexpr int64_t scanSessionId = 9302;

    Database db;
    if (!db.open(dbPath.u8string())) {
        std::cerr << "FAIL: open db: " << db.getLastError() << "\n";
        return 1;
    }
    writeSilentMonoWav(audioPath);
    AudioRecord rec;
    rec.filePath = audioPath.u8string();
    rec.fileName = "both.wav";
    rec.folderPath = root.u8string();
    rec.extension = ".wav";
    rec.fileSize = 4096;
    rec.fileMtime = 123;
    check(db.addFile(rec), "insert split-seed fixture row", failures);
    rec = db.getFileByPath(audioPath.u8string());
    check(rec.id > 0, "split-seed fixture row has id", failures);

    check(db.resetScanForegroundCompletionSession(scanSessionId),
          "reset split-seed foreground session", failures);
    check(db.seedScanForegroundCompletionRows(scanSessionId, {rec.id}, false, false),
          "seed split-seed all-file skipped terminal row", failures);
    ScanForegroundCompletionCounts counts = db.getScanForegroundCompletionCounts(scanSessionId);
    check(counts.total == 1, "all-file skipped seed creates one ledger row", failures);
    check(counts.miniUnresolved == 0, "all-file skipped seed has mini terminal", failures);
    check(counts.coverUnresolved == 0, "all-file skipped seed has cover terminal", failures);
    check(db.seedScanForegroundCompletionRows(scanSessionId, {rec.id}, true, false),
          "seed split-seed mini pending row", failures);
    check(db.seedScanForegroundCompletionRows(scanSessionId, {rec.id}, false, true),
          "seed split-seed cover pending row", failures);
    counts = db.getScanForegroundCompletionCounts(scanSessionId);
    check(counts.total == 1, "split-seed ledger keeps one row", failures);
    check(counts.miniUnresolved == 1, "split-seed mini remains pending after cover seed", failures);
    check(counts.coverUnresolved == 1, "split-seed cover becomes pending after mini seed", failures);

    check(db.markScanForegroundMiniTerminal(
              scanSessionId,
              rec.id,
              ScanForegroundTerminalState::Cached,
              "mini_done"),
          "split-seed mini terminal mark succeeds", failures);
    check(db.seedScanForegroundCompletionRows(scanSessionId, {rec.id}, true, false),
          "split-seed later mini seed is idempotent after terminal", failures);
    counts = db.getScanForegroundCompletionCounts(scanSessionId);
    check(counts.miniUnresolved == 0, "split-seed mini terminal is not regressed by later seed", failures);
    check(counts.coverUnresolved == 1, "split-seed cover remains pending until cover terminal", failures);

    check(db.markScanForegroundCoverTerminal(
              scanSessionId,
              rec.id,
              ScanForegroundTerminalState::NoSource,
              "no_cover"),
          "split-seed cover terminal mark succeeds", failures);
    counts = db.getScanForegroundCompletionCounts(scanSessionId);
    check(counts.miniUnresolved == 0, "split-seed final mini unresolved is zero", failures);
    check(counts.coverUnresolved == 0, "split-seed final cover unresolved is zero", failures);

    db.close();
    removeDbArtifacts(dbPath);
    fs::remove_all(root);
    return failures;
}

int testWorkerPoolMarksForegroundWaveformFailedTerminal() {
    int failures = 0;
    const fs::path root = makeTempRoot("foreground_waveform_failed_worker");
    const fs::path dbPath = root / "worker.db";
    const fs::path missingAudioPath = root / "missing.wav";
    constexpr int64_t generation = 45;
    constexpr int64_t scanSessionId = 9103;

    Database db;
    if (!db.open(dbPath.u8string())) {
        std::cerr << "FAIL: open db: " << db.getLastError() << "\n";
        return 1;
    }

    AudioRecord rec;
    rec.filePath = missingAudioPath.u8string();
    rec.fileName = "missing.wav";
    rec.folderPath = root.u8string();
    rec.extension = ".wav";
    rec.fileSize = 4096;
    rec.fileMtime = 123;
    check(db.addFile(rec), "insert waveform failed fixture row", failures);
    rec = db.getFileByPath(missingAudioPath.u8string());
    check(rec.id > 0, "waveform failed fixture row has id", failures);
    check(db.resetScanForegroundCompletionSession(scanSessionId), "reset waveform failed foreground session", failures);
    check(db.seedScanForegroundCompletionRows(scanSessionId, {rec.id}, true, false),
          "seed waveform failed foreground pending row", failures);
    check(db.markBackfillPending(rec.id, static_cast<int>(aura::BackfillJobKind::WaveformL1Overview)),
          "mark waveform failed worker pending row", failures);

    aura::BackfillQueue queue;
    queue.onDbOpened(dbPath.u8string(), generation);
    queue.enqueueFromScan(rec.id,
                          aura::BackfillJobKind::WaveformL1Overview,
                          aura::BackfillPriority::RecentScan,
                          dbPath.u8string(),
                          generation,
                          scanSessionId);

    aura::BackfillWorkerPool workers;
    workers.start(dbPath.u8string(), generation, &queue, 1);
    const bool drained = waitForPendingKind(db, aura::BackfillJobKind::WaveformL1Overview, 0);
    workers.stop();

    check(drained, "waveform failed worker clears durable pending row", failures);
    WaveformCacheState wfState;
    check(db.getWaveformCacheStateByPath(missingAudioPath.u8string(), wfState) &&
              wfState.status == "failed",
          "waveform failed worker writes failed cache state", failures);
    const ScanForegroundCompletionCounts counts = db.getScanForegroundCompletionCounts(scanSessionId);
    check(counts.total == 1, "waveform failed foreground ledger keeps one row", failures);
    check(counts.miniUnresolved == 0, "waveform failed worker marks mini failed terminal", failures);
    check(counts.failureTerminal >= 2, "waveform failed terminal is counted alongside skipped cover", failures);
    check(counts.coverUnresolved == 0, "waveform failed foreground row does not leave cover unresolved", failures);

    queue.shutdown();
    db.close();
    removeDbArtifacts(dbPath);
    fs::remove_all(root);
    return failures;
}

int testWorkerPoolCompletesCoverAndWaveformPending() {
    int failures = 0;
    const fs::path root = makeTempRoot("cover_indexing_backfill_worker");
    const fs::path nested = root / "Pack" / "Sub";
    const fs::path audioPath = nested / "hit.wav";
    const fs::path coverPath = root / "Pack" / "cover.jpg";
    const fs::path dbPath = root / "worker.db";
    writeSilentMonoWav(audioPath);
    writeTinyCoverImage(coverPath);

    Database db;
    if (!db.open(dbPath.u8string())) {
        std::cerr << "FAIL: open db: " << db.getLastError() << "\n";
        return 1;
    }

    aura::BackfillQueue queue;
    queue.onDbOpened(dbPath.u8string(), 42);
    const ScanStats stats = db.scanDirectoryEx(root.u8string(), true, false, false,
                                               nullptr, nullptr, 1,
                                               /*completeImport=*/false,
                                               /*enqueueExtraction=*/true,
                                               &queue,
                                               dbPath.u8string(),
                                               42);
    check(stats.added == 1, "fast scan inserts one covered audio file", failures);
    check(db.countBackfillPendingKind(static_cast<int>(aura::BackfillJobKind::CoverResolveAndThumb)) == 1,
          "fast scan creates durable cover pending row", failures);
    check(db.countBackfillPendingKind(static_cast<int>(aura::BackfillJobKind::WaveformL1Overview)) == 1,
          "fast scan creates durable waveform pending row", failures);

    aura::BackfillWorkerPool workers;
    workers.start(dbPath.u8string(), 42, &queue, 1);

    bool drained = false;
    for (int i = 0; i < 200; ++i) {
        if (db.countBackfillPending() == 0) {
            drained = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    workers.stop();
    check(drained, "worker pool drains durable cover and waveform pending rows", failures);

    const AudioRecord rec = db.getFileByPath(audioPath.u8string());
    check(!rec.folderCoverKey.empty(), "worker pool writes folder cover key", failures);
    std::vector<uint8_t> thumb;
    std::string mime;
    check(db.getCoverThumb(rec.folderCoverKey, thumb, mime) && !thumb.empty(),
          "worker pool writes cover thumbnail", failures);
    check(!db.getWaveformByPath(audioPath.u8string()).empty(),
          "worker pool writes mini waveform cache", failures);
    check(!db.hasWaveformDetailByPath(audioPath.u8string()),
          "scan backfill worker keeps WaveformL1Overview mini-only and never writes main waveform detail", failures);
    check(db.countFilesNeedingWaveformCache() == 0,
          "worker pool clears mini waveform debt after scan cache completes", failures);
    check(db.countFilesNeedingDetailCache() == 1,
          "high-speed main waveform detail remains explicit rebuild debt after scan cache completes", failures);

    queue.shutdown();
    db.close();
    removeDbArtifacts(dbPath);
    fs::remove_all(root);
    return failures;
}

} // namespace

int main() {
    int failures = 0;
    std::cout << "=== Backfill Recovery/Worker Tests ===\n";
    std::cout << "-- testRebuildAndRefillRecoverDurablePendingRows\n";
    failures += testRebuildAndRefillRecoverDurablePendingRows();
    std::cout << "-- testDurableRefillRearmsCachedRuntimeEntry\n";
    failures += testDurableRefillRearmsCachedRuntimeEntry();
    std::cout << "-- testDurableRefillRearmsPermanentFailureRuntimeEntry\n";
    failures += testDurableRefillRearmsPermanentFailureRuntimeEntry();
    std::cout << "-- testWorkerContinuouslyRefillsBeyond4096Rows\n";
    failures += testWorkerContinuouslyRefillsBeyond4096Rows();
    std::cout << "-- testScanEnqueueKeepsBoundedRuntimeWindow\n";
    failures += testScanEnqueueKeepsBoundedRuntimeWindow();
    std::cout << "-- testWorkerPoolMarksForegroundCoverNoSourceTerminal\n";
    failures += testWorkerPoolMarksForegroundCoverNoSourceTerminal();
    std::cout << "-- testWorkerPoolMarksForegroundWaveformCachedTerminal\n";
    failures += testWorkerPoolMarksForegroundWaveformCachedTerminal();
    std::cout << "-- testWorkerPoolDoesNotMarkDifferentForegroundSession\n";
    failures += testWorkerPoolDoesNotMarkDifferentForegroundSession();
    std::cout << "-- testWorkerPoolDoesNotClearForegroundPendingWhenTerminalMarkIsBlocked\n";
    failures += testWorkerPoolDoesNotClearForegroundPendingWhenTerminalMarkIsBlocked();
    std::cout << "-- testScanDrainDoesNotClearFailedPermanentWithoutForegroundTerminal\n";
    failures += testScanDrainDoesNotClearFailedPermanentWithoutForegroundTerminal();
    std::cout << "-- testForegroundTerminalMarkRequiresSeededRow\n";
    failures += testForegroundTerminalMarkRequiresSeededRow();
    std::cout << "-- testForegroundSplitSeedKeepsBothDimensionsPending\n";
    failures += testForegroundSplitSeedKeepsBothDimensionsPending();
    std::cout << "-- testWorkerPoolMarksForegroundWaveformFailedTerminal\n";
    failures += testWorkerPoolMarksForegroundWaveformFailedTerminal();
    std::cout << "-- testWorkerPoolCompletesCoverAndWaveformPending\n";
    failures += testWorkerPoolCompletesCoverAndWaveformPending();
    if (failures == 0) {
        std::cout << "PASS\n";
        return 0;
    }
    std::cerr << "FAILURES: " << failures << "\n";
    return 1;
}
