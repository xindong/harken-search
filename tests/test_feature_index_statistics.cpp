#include <iostream>
#include "LibraryStatisticsWorker.h"
#include <atomic>
#include <chrono>
#include <stdexcept>
#include <thread>
#include <filesystem>
#include "sqlite3.h"
using namespace std::chrono_literals;
using HarkenSearch::LibraryStatisticsWorker;
static void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class Predicate> void until(Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (!predicate()) {
        require(std::chrono::steady_clock::now() < deadline, "worker timed out");
        std::this_thread::sleep_for(1ms);
    }
}
int main() {
    try {
        std::atomic<int> entered{0}, cancelled{0};
        std::atomic<bool> release{false};
        const auto uiThread = std::this_thread::get_id();
        LibraryStatisticsWorker stats([&](const std::string& path, const auto& cancel) {
            require(std::this_thread::get_id() != uiThread, "SQL ran on caller thread");
            ++entered;
            while (!release && !cancel()) std::this_thread::sleep_for(1ms);
            if (cancel()) ++cancelled;
            // Return success even after cancellation to test stale-result rejection.
            HarkenSearch::LibraryStatisticsResult value;
            value.success = true;
            value.indexed = path == "old" ? 1 : 7;
            value.total = path == "old" ? 2 : 9;
            return value;
        });
        stats.SetDatabase("old", 1);
        require(stats.Request(), "first request rejected");
        until([&] { return entered == 1; });
        const auto start = std::chrono::steady_clock::now();
        for (int i=0; i<1000; ++i) {
            require(!stats.Poll(), "unfinished query published");
            require(!stats.Request(), "same query queued repeatedly");
        }
        require(std::chrono::steady_clock::now()-start < 100ms, "UI polling waited for query");
        stats.SetDatabase("new", 2);
        require(stats.Request(), "replacement query rejected");
        until([&] { return entered == 2; });
        require(cancelled == 1, "old database query not cancelled");
        require(!stats.Poll(), "old database result leaked");
        release = true;
        std::optional<HarkenSearch::LibraryStatisticsResult> result;
        until([&] { result = stats.Poll(); return result.has_value(); });
        require(result->success && result->indexed == 7 && result->total == 9, "new database count mismatch");
        release = false;
        require(stats.Request(), "refresh rejected");
        until([&] { return entered == 3; });
        stats.SetDatabase("new", 3); // same file reopened: generation still matters
        require(stats.Request(), "generation refresh rejected");
        until([&] { return entered == 4; });
        require(!stats.Poll(), "stale same-path result leaked");
        stats.Cancel();
        until([&] { return cancelled == 3; });
        require(!stats.Poll(), "closed panel received a cancelled result");
        const auto shutdownStart = std::chrono::steady_clock::now();
        {
            std::atomic<bool> running{false};
            LibraryStatisticsWorker shutdown([&](const std::string&, const auto& cancel) {
                running = true;
                while (!cancel()) std::this_thread::sleep_for(1ms);
                return HarkenSearch::LibraryStatisticsResult{};
            });
            shutdown.SetDatabase("shutdown", 1); shutdown.Request();
            until([&] { return running.load(); });
        }
        require(std::chrono::steady_clock::now()-shutdownStart < 100ms, "shutdown did not cancel active query");
        const auto folder = std::filesystem::current_path() / "build/Testing/feature-statistics";
        std::filesystem::create_directories(folder);
        const auto dbPath = (folder / ("counts-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".db")).u8string();
        sqlite3* writer = nullptr;
        require(sqlite3_open(dbPath.c_str(), &writer) == SQLITE_OK, "fixture open failed");
        require(sqlite3_exec(writer, "PRAGMA journal_mode=WAL; CREATE TABLE audio_files(audio_features BLOB, features_version INTEGER);"
            "INSERT INTO audio_files VALUES(NULL,1),(x'01',0),(x'02',1),(x'03',2);", nullptr,nullptr,nullptr) == SQLITE_OK, "fixture creation failed");
        LibraryStatisticsWorker databaseStats(HarkenSearch::MakeFeatureIndexCountHandler());
        databaseStats.SetDatabase(dbPath,1); require(databaseStats.Request(), "SQL count request rejected");
        until([&] { result=databaseStats.Poll(); return result.has_value(); });
        require(result->success && result->total==4 && result->indexed==2, "SQL null/version population mismatch");
        sqlite3_exec(writer, "INSERT INTO audio_files VALUES(x'04',1);", nullptr,nullptr,nullptr);
        require(databaseStats.Request(), "refresh SQL rejected");
        until([&] { result=databaseStats.Poll(); return result.has_value(); });
        require(result->success && result->total==5 && result->indexed==3, "newly indexed file not reflected");
        sqlite3_exec(writer, "DELETE FROM audio_files;", nullptr,nullptr,nullptr);
        require(databaseStats.Request(), "empty SQL rejected");
        until([&] { result=databaseStats.Poll(); return result.has_value(); });
        require(result->success && result->total==0 && result->indexed==0, "empty library mismatch");
        sqlite3_close(writer);
        databaseStats.SetDatabase(dbPath+".missing",2); databaseStats.Request();
        until([&] { result=databaseStats.Poll(); return result.has_value(); });
        require(!result->success && !std::filesystem::exists(dbPath+".missing"), "read-only worker created missing database");
        std::cout << "PASS: nonblocking polling, single worker, database and generation isolation, panel cancellation, bounded shutdown\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
