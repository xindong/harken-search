// Runtime checks for the cover key cache writeback worker.
//
// The boundary test (tests/test_cover_key_cache_persistence_workflow_boundary.py)
// only inspects source strings; this one drives the real worker against a real
// SQLite database so the retry, stop-on-switch and flush-on-exit paths are
// covered by behaviour instead of text matching. The cover queue itself is
// covered by TestCoverDiagnostics, so here it is a scripted stub.

#include "CoverKeyCachePersistenceWorkflow.h"
#include "CoverModule.h"
#include "Database.h"
#include "Utils.h"
#include "platform/harken_user_data_layout.h"
#include "sqlite3.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace fs = std::filesystem;
namespace cover_cache = aura::cover_cache;

namespace {

int g_failures = 0;

void check(bool condition, const char* label) {
    std::cout << (condition ? "PASS: " : "FAIL: ") << label << "\n";
    if (!condition) ++g_failures;
}

std::mutex g_queueMutex;
std::vector<CoverModule::CoverKeyCacheUpdate> g_queue;

void QueueUpdate(CoverModule::CoverKeyCacheUpdate update) {
    std::lock_guard<std::mutex> lock(g_queueMutex);
    g_queue.push_back(std::move(update));
}

void ClearQueue() {
    std::lock_guard<std::mutex> lock(g_queueMutex);
    g_queue.clear();
}

bool WaitFor(const std::function<bool()>& predicate, int timeoutMs) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return predicate();
}

CoverModule::CoverKeyCacheUpdate MakeUpdate(const std::string& path) {
    CoverModule::CoverKeyCacheUpdate update;
    update.filePathUtf8 = path;
    return update;
}

int64_t CurrentProcessId() {
#ifdef _WIN32
    return static_cast<int64_t>(::_getpid());
#else
    return static_cast<int64_t>(::getpid());
#endif
}

AudioRecord MakeRecord(const fs::path& folder, const std::string& name) {
    AudioRecord record;
    record.fileName = name;
    record.filePath = (folder / name).u8string();
    record.folderPath = folder.u8string();
    record.extension = ".wav";
    record.format = "WAV";
    record.durationSeconds = 1.0f;
    record.sampleRate = 48000;
    record.channels = 2;
    record.fileSize = 1024;
    record.dateAdded = 1;
    record.fileMtime = 1;
    return record;
}

}  // namespace

namespace CoverModule {

size_t DrainCoverKeyCacheUpdates(std::vector<CoverKeyCacheUpdate>& outUpdates, size_t maxItems) {
    std::lock_guard<std::mutex> lock(g_queueMutex);
    const size_t count = std::min(maxItems, g_queue.size());
    for (size_t i = 0; i < count; ++i) {
        outUpdates.push_back(std::move(g_queue[i]));
    }
    g_queue.erase(g_queue.begin(), g_queue.begin() + static_cast<std::ptrdiff_t>(count));
    return count;
}

}  // namespace CoverModule

int main() {
    if (Platform::HarkenUserData::PreviewIsolationActive()) {
        // The writer rejects databases outside the compiled preview root, and unit
        // runs must not write into that fixed root. CI builds without isolation.
        std::cout << "SKIP: cover persistence runtime needs a non-isolated build\n";
        return 0;
    }
    const fs::path root = fs::temp_directory_path() /
        ("harken-cover-persist-" + std::to_string(CurrentProcessId()));
    std::error_code ignored;
    fs::remove_all(root, ignored);
    const fs::path folder = root / "Alpha";
    fs::create_directories(folder);

    Database database;
    const std::string databasePath = (root / "main.db").u8string();
    if (!database.open(databasePath, true)) {
        std::cout << "FAIL: open temporary database: " << database.getLastError() << "\n";
        return 1;
    }
    const AudioRecord record = MakeRecord(folder, "QA-Alpha.wav");
    if (!database.addFile(record)) {
        std::cout << "FAIL: insert fixture row: " << database.getLastError() << "\n";
        return 1;
    }
    const std::string path = record.filePath;

    // 1. A single update carrying both sources lands as one row write.
    auto merged = MakeUpdate(path);
    merged.updateEmbedded = true;
    merged.embeddedCoverKey = "E|1111111111111111|1111|image/jpeg";
    merged.embeddedCoverKeyMtime = 1111;
    merged.updateFolder = true;
    merged.folderCoverKey = "F|H|2222222222222222|2222";
    merged.folderCoverSrcPathUtf8 = "cover.png";
    merged.folderCoverSrcMtime = 2222;
    QueueUpdate(merged);

    cover_cache::BindPersistence(database);
    bool pumped = false;
    const bool mergedPersisted = WaitFor([&] {
        AudioRecord row = database.getFileByPath(path);
        pumped = pumped || cover_cache::PumpPersistence();
        return row.embeddedCoverKey == merged.embeddedCoverKey &&
               row.embeddedCoverKeyMtime == merged.embeddedCoverKeyMtime &&
               row.folderCoverKey == merged.folderCoverKey &&
               row.folderCoverSrcPath == merged.folderCoverSrcPathUtf8 &&
               row.folderCoverSrcMtime == merged.folderCoverSrcMtime;
    }, 5000);
    check(mergedPersisted, "both cover sources are written to the real database");
    check(pumped, "writeback pump reports a committed batch");

    // 2. A locked database keeps the previous row and writes once it is released.
    sqlite3* blocker = nullptr;
    if (sqlite3_open_v2(databasePath.c_str(), &blocker,
                        SQLITE_OPEN_READWRITE, nullptr) == SQLITE_OK) {
        sqlite3_exec(blocker, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr);
    }
    auto lockedUpdate = MakeUpdate(path);
    lockedUpdate.updateFolder = true;
    lockedUpdate.folderCoverKey = "F|H|3333333333333333|3333";
    lockedUpdate.folderCoverSrcPathUtf8 = "cover-alt.png";
    lockedUpdate.folderCoverSrcMtime = 3333;
    QueueUpdate(lockedUpdate);
    cover_cache::PumpPersistence();
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    AudioRecord whileLocked = database.getFileByPath(path);
    check(whileLocked.folderCoverKey == merged.folderCoverKey,
          "a locked database leaves the previous folder key in place");
    if (blocker) {
        sqlite3_exec(blocker, "ROLLBACK;", nullptr, nullptr, nullptr);
        sqlite3_close(blocker);
    }
    const bool retried = WaitFor([&] {
        AudioRecord row = database.getFileByPath(path);
        return row.folderCoverKey == lockedUpdate.folderCoverKey &&
               row.folderCoverSrcPath == lockedUpdate.folderCoverSrcPathUtf8 &&
               row.embeddedCoverKey == merged.embeddedCoverKey;
    }, 8000);
    check(retried, "the retry writes the locked batch and keeps the other source");

    // 3. Stopping the worker (database switch) drops the queued write.
    cover_cache::StopPersistence();
    check(!cover_cache::PumpPersistence(), "a stopped worker reports no batch");
    auto stoppedUpdate = MakeUpdate(path);
    stoppedUpdate.updateEmbedded = true;
    stoppedUpdate.embeddedCoverKey = "E|4444444444444444|4444|image/png";
    stoppedUpdate.embeddedCoverKeyMtime = 4444;
    QueueUpdate(stoppedUpdate);
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    check(database.getFileByPath(path).embeddedCoverKey == merged.embeddedCoverKey,
          "a stopped worker does not drain the queue");

    // 4. Flush on shutdown writes what is still pending before the worker exits.
    ClearQueue();
    cover_cache::BindPersistence(database);
    auto flushUpdate = MakeUpdate(path);
    flushUpdate.updateEmbedded = true;
    flushUpdate.embeddedCoverKey = "E|5555555555555555|5555|image/png";
    flushUpdate.embeddedCoverKeyMtime = 5555;
    QueueUpdate(flushUpdate);
    cover_cache::FlushPersistence();
    AudioRecord afterFlush = database.getFileByPath(path);
    check(afterFlush.embeddedCoverKey == flushUpdate.embeddedCoverKey &&
              afterFlush.embeddedCoverKeyMtime == flushUpdate.embeddedCoverKeyMtime,
          "flush writes the pending batch before shutdown");
    check(afterFlush.folderCoverKey == lockedUpdate.folderCoverKey,
          "flush keeps the folder key of the earlier batch");

    database.close();
    fs::remove_all(root, ignored);
    ClearQueue();
    std::cout << (g_failures == 0 ? "PASS: cover persistence runtime\n"
                                  : "FAIL: cover persistence runtime\n");
    return g_failures == 0 ? 0 : 1;
}
