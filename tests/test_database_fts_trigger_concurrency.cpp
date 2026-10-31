#include "Database.h"
#include "sqlite3.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>

namespace {
struct TriggerRefreshGate {
    std::mutex mutex;
    std::condition_variable changed;
    bool firstCreateReached = false;
    bool secondOpenFinished = false;
    std::atomic<bool> interceptNextCreate{true};
} gate;

int TraceTriggerCreation(unsigned, void*, void*, void* sqlText) {
    const char* sql = static_cast<const char*>(sqlText);
    if (!sql || std::string(sql).find("CREATE TRIGGER audio_files_ai AFTER INSERT") == std::string::npos ||
        !gate.interceptNextCreate.exchange(false)) return 0;
    std::unique_lock<std::mutex> lock(gate.mutex);
    gate.firstCreateReached = true;
    gate.changed.notify_all();
    // Let a second connection attempt initialization in the gap between DROP
    // and CREATE. Atomic replacement blocks it until this bounded wait ends.
    gate.changed.wait_for(lock, std::chrono::seconds(2), [] { return gate.secondOpenFinished; });
    return 0;
}

int InstallTrace(sqlite3* db, char**, const sqlite3_api_routines*) {
    return sqlite3_trace_v2(db, SQLITE_TRACE_STMT, TraceTriggerCreation, nullptr);
}

int DenySecondTrigger(void*, int action, const char* name, const char*, const char*, const char*) {
    return action == SQLITE_CREATE_TRIGGER && name && std::string(name) == "audio_files_ad"
        ? SQLITE_DENY : SQLITE_OK;
}

int InstallFailure(sqlite3* db, char**, const sqlite3_api_routines*) {
    return sqlite3_set_authorizer(db, DenySecondTrigger, nullptr);
}
} // namespace

int testConcurrentFtsTriggerRefresh() {
    const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto directory = std::filesystem::temp_directory_path() /
        ("harken-fts-refresh-" + std::to_string(suffix));
    std::filesystem::create_directories(directory);
    const std::string path = (directory / "main.db").u8string();
    {
        Database fixture;
        if (!fixture.open(path)) {
            std::cerr << "FAIL: fixture initialization: " << fixture.getLastError() << '\n';
            return 1;
        }
    }
    const auto extension = reinterpret_cast<void (*)()>(InstallTrace);
    if (sqlite3_auto_extension(extension) != SQLITE_OK) return 1;
    bool opened[2] = {false, false};
    std::string errors[2];
    std::thread first([&] {
        Database db;
        opened[0] = db.open(path, false);
        errors[0] = db.getLastError();
    });
    bool reached = false;
    {
        std::unique_lock<std::mutex> lock(gate.mutex);
        reached = gate.changed.wait_for(lock, std::chrono::seconds(8), [] { return gate.firstCreateReached; });
    }
    std::thread second([&] {
        Database db;
        opened[1] = db.open(path, false);
        errors[1] = db.getLastError();
        std::lock_guard<std::mutex> lock(gate.mutex);
        gate.secondOpenFinished = true;
        gate.changed.notify_all();
    });
    first.join();
    second.join();
    sqlite3_cancel_auto_extension(extension);
    int failures = 0;
    const auto check = [&](bool passed, const std::string& message) {
        std::cout << (passed ? "PASS: " : "FAIL: ") << message << '\n';
        if (!passed) ++failures;
    };
    check(reached, "concurrent open reaches the trigger replacement boundary");
    check(opened[0], "first database open succeeds: " + errors[0]);
    check(opened[1], "second database open succeeds: " + errors[1]);
    // Inspect using SQLite directly: another Database::open would repair the
    // very trigger set that this test must verify.
    sqlite3* inspect = nullptr;
    if (sqlite3_open(path.c_str(), &inspect) != SQLITE_OK) return failures + 1;
    sqlite3_stmt* statement = nullptr;
    sqlite3_prepare_v2(inspect, "SELECT count(*) FROM sqlite_master WHERE type='trigger' AND name IN ('audio_files_ai','audio_files_ad','audio_files_au','audio_files_ai_trigram','audio_files_ad_trigram','audio_files_au_trigram')", -1, &statement, nullptr);
    const bool complete = statement && sqlite3_step(statement) == SQLITE_ROW && sqlite3_column_int(statement, 0) == 6;
    check(complete, "all six FTS synchronization triggers survive concurrent opens");
    sqlite3_finalize(statement);
    sqlite3_close(inspect);
    const auto failureExtension = reinterpret_cast<void (*)()>(InstallFailure);
    sqlite3_auto_extension(failureExtension);
    {
        Database failing;
        check(!failing.open(path, false), "a denied trigger creation reports initialization failure");
    }
    sqlite3_cancel_auto_extension(failureExtension);
    inspect = nullptr;
    if (sqlite3_open(path.c_str(), &inspect) != SQLITE_OK) return failures + 1;
    statement = nullptr;
    sqlite3_prepare_v2(inspect, "SELECT count(*) FROM sqlite_master WHERE type='trigger' AND name IN ('audio_files_ai','audio_files_ad','audio_files_au','audio_files_ai_trigram','audio_files_ad_trigram','audio_files_au_trigram')", -1, &statement, nullptr);
    check(statement && sqlite3_step(statement) == SQLITE_ROW && sqlite3_column_int(statement, 0) == 6,
          "failed replacement rolls back without leaving missing triggers");
    sqlite3_finalize(statement);
    sqlite3_close(inspect);
    return failures == 0 ? 0 : 1;
}
