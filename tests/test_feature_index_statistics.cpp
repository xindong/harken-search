#include <iostream>
#include "LibraryStatisticsWorker.h"
#include "AcousticSimilarity.h"
#include "AudioEmbeddingService.h"
#include <atomic>
#include <chrono>
#include <stdexcept>
#include <thread>
#include <filesystem>
#include <future>
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
        const auto v2Path = dbPath + ".v2";
        sqlite3_open(v2Path.c_str(), &writer);
        sqlite3_exec(writer, "CREATE TABLE audio_files(id INTEGER PRIMARY KEY,file_path TEXT,file_name TEXT,format TEXT,duration REAL); INSERT INTO audio_files VALUES(1,'a','a','wav',1),(2,'b','b','wav',1);",nullptr,nullptr,nullptr);
        const auto v2Handler = HarkenSearch::MakeFeatureIndexCountHandler(true);
        auto v2 = v2Handler(v2Path, [] { return false; });
        require(v2.success && v2.total == 2 && v2.indexed == 0, "V1-only library must show V2 analysis pending");
        int availableProbes = 0, unavailableProbes = 0;
        const auto availableHandler = HarkenSearch::MakeFeatureIndexCountHandler(true, [&] { ++availableProbes; return true; });
        const auto unavailableHandler = HarkenSearch::MakeFeatureIndexCountHandler(true, [&] { ++unavailableProbes; return false; });
        require(availableHandler(v2Path, [] { return false; }).pending == 2 &&
                unavailableHandler(v2Path, [] { return false; }).pending == 2,
            "all files await first analysis regardless of model availability");
        sqlite3_exec(writer, "CREATE TABLE harken_similarity_v2(file_id INTEGER PRIMARY KEY,file_path TEXT,file_bytes INTEGER,modified INTEGER,fingerprint TEXT,version INTEGER,summary BLOB,acoustic BLOB,embedding BLOB,model_version TEXT);",nullptr,nullptr,nullptr);
        HarkenSearch::AcousticAnalysis validAnalysis;
        validAnalysis.sourceSeconds = 1.0f;
        validAnalysis.activeSeconds = 1.0f;
        validAnalysis.frames.emplace_back();
        const auto validSummary = validAnalysis.Serialize(false);
        require(!validSummary.empty(), "valid acoustic summary fixture could not be serialized");
        HarkenSearch::AudioEmbedding validEmbedding{};
        validEmbedding[0] = 1.0f;
        require(HarkenSearch::IsValidAudioEmbedding(validEmbedding), "valid semantic fixture could not be created");
        auto insertV2Row = [&](int id, const char* path, bool semantic) {
            sqlite3_stmt* insert = nullptr;
            require(sqlite3_prepare_v2(writer, "INSERT INTO harken_similarity_v2 VALUES(?,?,1024,1,?,2,?,?,?,?)",
                -1, &insert, nullptr) == SQLITE_OK, "V2 fixture insert prepare failed");
            const std::string fingerprint(64, id == 1 ? 'a' : 'b');
            sqlite3_bind_int(insert, 1, id);
            sqlite3_bind_text(insert, 2, path, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(insert, 3, fingerprint.c_str(), int(fingerprint.size()), SQLITE_TRANSIENT);
            sqlite3_bind_blob(insert, 4, validSummary.data(), int(validSummary.size()), SQLITE_TRANSIENT);
            sqlite3_bind_blob(insert, 5, validSummary.data(), int(validSummary.size()), SQLITE_TRANSIENT);
            if (semantic) sqlite3_bind_blob(insert, 6, validEmbedding.data(), int(sizeof(validEmbedding)), SQLITE_TRANSIENT);
            else sqlite3_bind_null(insert, 6);
            sqlite3_bind_text(insert, 7, semantic ? HarkenSearch::kSimilarityModelRevision : "", -1, SQLITE_TRANSIENT);
            const int step = sqlite3_step(insert);
            sqlite3_finalize(insert);
            require(step == SQLITE_DONE, "V2 fixture row insert failed");
        };
        insertV2Row(1, "a", true);
        v2 = v2Handler(v2Path, [] { return false; });
        require(v2.success && v2.total == 2 && v2.indexed == 1 && v2.semanticIndexed == 1, "V2 acoustic and semantic counts must match active model version");
        require(availableHandler(v2Path, [] { return false; }).pending == 1 &&
                unavailableHandler(v2Path, [] { return false; }).pending == 1,
            "completed file is removed from the remaining count");
        sqlite3_exec(writer, "UPDATE harken_similarity_v2 SET model_version='old' WHERE file_id=1;",nullptr,nullptr,nullptr);
        v2 = v2Handler(v2Path, [] { return false; });
        require(v2.success && v2.indexed == 1 && v2.semanticIndexed == 0, "outdated semantic model cannot appear complete");
        require(availableHandler(v2Path, [] { return false; }).pending == 2 &&
                unavailableHandler(v2Path, [] { return false; }).pending == 1,
            "semantic-only work counts when the model is ready, but not when it is unavailable");
        insertV2Row(2, "b", false);
        require(availableHandler(v2Path, [] { return false; }).pending == 2 &&
                unavailableHandler(v2Path, [] { return false; }).pending == 0 &&
                availableProbes == 1 && unavailableProbes == 1,
            "acoustic-complete files do not inflate unavailable-model remainder and probe is cached");
        require(sqlite3_exec(writer, "PRAGMA journal_mode=WAL;", nullptr, nullptr, nullptr) == SQLITE_OK, "WAL fixture failed");
        int cancelCalls = 0;
        bool concurrentWrite = false;
        v2 = v2Handler(v2Path, [&] {
            if (++cancelCalls == 3) {
                require(sqlite3_exec(writer, "INSERT INTO audio_files VALUES(3,'c','c','wav',1)", nullptr, nullptr, nullptr) == SQLITE_OK, "concurrent audio insert failed");
                insertV2Row(3, "c", false);
                concurrentWrite = true;
            }
            return false;
        });
        require(concurrentWrite && v2.success && v2.total == 2 && v2.indexed == 2,
                "concurrent insert must not mix library snapshots");
        sqlite3_close(writer);
        // Reproduce the Windows library-replacement boundary with a real SQLite
        // reader. Cancellation is observable before its connection is released.
        std::atomic<bool> sqlEntered{false}, closeAllowed{false}, cancelSeen{false}, connectionClosed{false};
        LibraryStatisticsWorker heldReader([&](const std::string& path, const auto& cancel) {
            sqlite3* db = nullptr;
            require(sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK,
                    "held reader open failed");
            sqlite3_stmt* statement = nullptr;
            require(sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM audio_files", -1, &statement, nullptr) == SQLITE_OK,
                    "held reader prepare failed");
            require(sqlite3_step(statement) == SQLITE_ROW, "held reader did not read SQLite");
            sqlEntered = true;
            while (!cancel()) std::this_thread::sleep_for(1ms);
            cancelSeen = true;
            while (!closeAllowed) std::this_thread::sleep_for(1ms);
            sqlite3_finalize(statement);
            require(sqlite3_close(db) == SQLITE_OK, "held connection did not close");
            connectionClosed = true;
            return HarkenSearch::LibraryStatisticsResult{};
        });
        std::future<void> drain;
        // Declared after the future: release both waits before its destructor
        // joins, including assertion failures or a failed async launch.
        struct ReleaseReaderOnExit {
            LibraryStatisticsWorker& worker;
            std::atomic<bool>& allowed;
            ~ReleaseReaderOnExit() { allowed = true; worker.Cancel(); }
        } releaseReader{heldReader, closeAllowed};
        heldReader.SetDatabase(dbPath, 1); heldReader.Request();
        until([&] { return sqlEntered.load(); });
        drain = std::async(std::launch::async, [&] { heldReader.CancelAndWait(); });
        until([&] { return cancelSeen.load(); });
        const bool waitedForConnection = drain.wait_for(20ms) == std::future_status::timeout;
        const auto pollingStart = std::chrono::steady_clock::now();
        bool requestsRejected = true;
        for (int i = 0; i < 1000; ++i) {
            requestsRejected &= !heldReader.Request() && !heldReader.Poll();
        }
        const bool pollingResponsive = std::chrono::steady_clock::now()-pollingStart < 100ms;
#ifdef _WIN32
        std::error_code lockedRenameError;
        std::filesystem::rename(std::filesystem::u8path(dbPath), std::filesystem::u8path(dbPath+".blocked"), lockedRenameError);
        const bool liveConnectionBlocksRename = bool(lockedRenameError);
#endif
        closeAllowed = true;
        const bool drained = drain.wait_for(3s) == std::future_status::ready;
        require(drained, "connection drain did not finish");
        drain.get();
        require(waitedForConnection && connectionClosed, "replacement started before reader closed");
        require(requestsRejected && pollingResponsive, "draining statistics blocked UI or admitted new work");
#ifdef _WIN32
        require(liveConnectionBlocksRename, "fixture did not reproduce the Windows open-file constraint");
#endif
        std::filesystem::rename(std::filesystem::u8path(dbPath), std::filesystem::u8path(dbPath+".released"));
        std::filesystem::rename(std::filesystem::u8path(dbPath+".released"), std::filesystem::u8path(dbPath));
        heldReader.CancelAndWait(); // idle repeated drain must also return
        heldReader.SetDatabase(dbPath, 2);
        require(heldReader.Request(), "drained worker could not be used for the next database");
        heldReader.CancelAndWait();
        databaseStats.SetDatabase(dbPath+".missing",2); databaseStats.Request();
        until([&] { result=databaseStats.Poll(); return result.has_value(); });
        require(!result->success && !std::filesystem::exists(dbPath+".missing"), "read-only worker created missing database");
        std::cout << "PASS: nonblocking polling, single worker, database and generation isolation, panel cancellation, bounded shutdown\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
