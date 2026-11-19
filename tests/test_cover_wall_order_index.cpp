#include "Database.h"
#include "Utils.h"

#include <sqlite3.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace fs = std::filesystem;

namespace {

int Fail(const std::string& message) {
    std::cerr << "FAIL: " << message << "\n";
    return 1;
}

bool Exec(sqlite3* db, const char* sql) {
    char* error = nullptr;
    const int rc = sqlite3_exec(db, sql, nullptr, nullptr, &error);
    if (rc != SQLITE_OK) {
        std::cerr << "sqlite error: " << (error ? error : "unknown") << "\n";
        if (error) sqlite3_free(error);
        return false;
    }
    return true;
}

int RunProbe(const std::string& dbPath, bool build) {
    Database db;
    if (build) {
        if (!db.open(dbPath, true)) return Fail("open primary failed: " + db.getLastError());
        const auto start = std::chrono::steady_clock::now();
        if (!db.rebuildCoverWallOrderIndex(20000)) return Fail("rebuild failed: " + db.getLastError());
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count();
        std::cout << "build_ms=" << elapsed << " ready=" << (db.isCoverWallOrderIndexReady() ? 1 : 0) << "\n";
        return 0;
    }
    if (!db.openReadOnly(dbPath)) return Fail("open read-only failed: " + db.getLastError());
    for (int i = 0; i < 3; ++i) {
        const auto start = std::chrono::steady_clock::now();
        auto page = db.getGlobalCoverWallEntriesPage({}, 512, true);
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count();
        std::cout << "page_ms=" << elapsed << " entries=" << page.entries.size()
                  << " hasMore=" << (page.hasMore ? 1 : 0)
                  << " next=" << (page.nextCursor.empty() ? 0 : 1) << "\n";
    }
    return 0;
}

int RunPages(const std::string& dbPath) {
    Database db;
    if (!db.openReadOnly(dbPath)) return Fail("pages open failed: " + db.getLastError());
    std::string cursor;
    std::unordered_set<std::string> keys;
    size_t total = 0;
    for (int pageIndex = 0; pageIndex < 40; ++pageIndex) {
        const auto start = std::chrono::steady_clock::now();
        auto page = db.getGlobalCoverWallEntriesPage(cursor, 512, true);
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count();
        if (page.entries.empty()) break;
        for (const auto& entry : page.entries) {
            if (!keys.insert(entry.key).second) return Fail("duplicate key across pages: " + entry.key);
        }
        total += page.entries.size();
        std::cout << "page=" << pageIndex << " ms=" << elapsed << " entries=" << page.entries.size()
                  << " hasMore=" << (page.hasMore ? 1 : 0) << "\n";
        if (!page.hasMore || page.nextCursor.empty()) break;
        cursor = page.nextCursor;
    }
    std::cout << "pages_total=" << total << " unique=" << keys.size() << "\n";
    return 0;
}
int RunCheck(const std::string& dbPath) {
    sqlite3* raw = nullptr;
    if (sqlite3_open(dbPath.c_str(), &raw) != SQLITE_OK) return Fail("check open failed");
    sqlite3_create_collation(raw, "AURA_NATURAL", SQLITE_UTF8, nullptr,
        [](void*, int lhsLen, const void* lhsData, int rhsLen, const void* rhsData) -> int {
            const std::string_view lhs(lhsData ? static_cast<const char*>(lhsData) : "",
                                       lhsData && lhsLen > 0 ? static_cast<size_t>(lhsLen) : 0);
            const std::string_view rhs(rhsData ? static_cast<const char*>(rhsData) : "",
                                       rhsData && rhsLen > 0 ? static_cast<size_t>(rhsLen) : 0);
            return CompareNaturalUtf8(lhs, rhs);
        });

    std::vector<std::string> indexOrder;
    std::vector<std::string> truthOrder;
    auto load = [&](const char* sql, std::vector<std::string>& out) -> bool {
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(raw, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            std::cerr << "prepare failed: " << sqlite3_errmsg(raw) << "\n";
            return false;
        }
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const std::string name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
            const std::string path = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
            const int64_t id = sqlite3_column_int64(stmt, 2);
            out.push_back(name + "\x1f" + path + "\x1f" + std::to_string(id));
        }
        sqlite3_finalize(stmt);
        return true;
    };

    if (!load("SELECT order_name, order_path, order_id FROM cover_wall_order_index "
              "ORDER BY order_name COLLATE AURA_NATURAL, order_path, order_id LIMIT 1536;", indexOrder)) return 1;
    const auto start = std::chrono::steady_clock::now();
    if (!load("WITH ranked AS (SELECT id, file_name, file_path, ROW_NUMBER() OVER ("
              "PARTITION BY CASE WHEN embedded_cover_key IS NOT NULL AND embedded_cover_key <> '' THEN embedded_cover_key "
              "WHEN folder_cover_key IS NOT NULL AND folder_cover_key <> '' THEN folder_cover_key "
              "ELSE '?|' || folder_path END "
              "ORDER BY file_name COLLATE AURA_NATURAL, file_path, id) AS rn "
              "FROM audio_files WHERE (embedded_cover_key IS NOT NULL AND embedded_cover_key <> '') "
              "OR (folder_cover_key IS NOT NULL AND folder_cover_key <> '') "
              "OR (has_cover = 1 AND folder_path IS NOT NULL AND folder_path <> '')) "
              "SELECT file_name, file_path, id FROM ranked WHERE rn = 1 "
              "ORDER BY file_name COLLATE AURA_NATURAL, file_path, id LIMIT 1536;", truthOrder)) return 1;
    const auto truthMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();
    sqlite3_close(raw);

    if (indexOrder.size() != truthOrder.size()) return Fail("order size mismatch");
    for (size_t i = 0; i < indexOrder.size(); ++i) {
        if (indexOrder[i] != truthOrder[i]) {
            return Fail("order mismatch at " + std::to_string(i) + ": index=" + indexOrder[i] + " truth=" + truthOrder[i]);
        }
    }
    std::cout << "check=ok rows=" << indexOrder.size() << " truth_ms=" << truthMs << "\n";
    return 0;
}
int RunUnit() {
    const fs::path root = fs::temp_directory_path() / "harken-cover-wall-order-index-test";
    std::error_code removeError;
    fs::remove_all(root, removeError);
    fs::create_directories(root);
    const fs::path dbPath = root / "test.db";

    Database db;
    if (!db.open(dbPath.string(), true)) return Fail("open failed: " + db.getLastError());

    sqlite3* raw = nullptr;
    if (sqlite3_open(dbPath.string().c_str(), &raw) != SQLITE_OK) return Fail("raw open failed");
    sqlite3_create_collation(raw, "AURA_NATURAL", SQLITE_UTF8, nullptr,
        [](void*, int lhsLen, const void* lhsData, int rhsLen, const void* rhsData) -> int {
            const std::string_view lhs(lhsData ? static_cast<const char*>(lhsData) : "",
                                       lhsData && lhsLen > 0 ? static_cast<size_t>(lhsLen) : 0);
            const std::string_view rhs(rhsData ? static_cast<const char*>(rhsData) : "",
                                       rhsData && rhsLen > 0 ? static_cast<size_t>(rhsLen) : 0);
            return CompareNaturalUtf8(lhs, rhs);
        });
    if (!Exec(raw, "BEGIN IMMEDIATE;")) return 1;
    const char* bulkSql =
        "WITH digits(d) AS (VALUES(0),(1),(2),(3),(4),(5),(6),(7),(8),(9)), "
        "nums(n) AS (SELECT a.d + b.d*10 + c.d*100 + e.d*1000 + f.d*10000 "
        "             FROM digits a, digits b, digits c, digits e, digits f) "
        "INSERT INTO audio_files(file_name, file_path, folder_path, has_cover, folder_cover_key, "
        "embedded_cover_key, file_size, sample_rate, channels, total_frames, duration) "
        "SELECT printf('track%03d_%05d.wav', n/50, n%50), "
        "       printf('C:/testlib/folder%04d/track%03d_%05d.wav', n/50, n/50, n%50), "
        "       printf('C:/testlib/folder%04d', n/50), 1, printf('F|%04d', n/50), '', "
        "       1000, 48000, 2, 48000, 1.0 "
        "FROM nums WHERE n < 50050;";
    if (!Exec(raw, bulkSql)) return Fail(std::string("bulk insert failed: ") + sqlite3_errmsg(raw));
    if (!Exec(raw, "COMMIT;")) return 1;

    if (db.getFileCount() != 50050) return Fail("unexpected file count");
    if (!db.rebuildCoverWallOrderIndex(5000)) return Fail("rebuild failed: " + db.getLastError());
    if (!db.isCoverWallOrderIndexReady()) return Fail("index not ready");

    std::vector<std::string> expected;
    {
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(raw,
                "SELECT folder_cover_key, file_name, file_path FROM audio_files "
                "ORDER BY file_name COLLATE AURA_NATURAL, file_path, id;",
                -1, &stmt, nullptr) != SQLITE_OK) return Fail("expected query prepare failed");
        std::unordered_set<std::string> seen;
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const std::string key = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
            if (seen.insert(key).second) expected.push_back(key);
        }
        sqlite3_finalize(stmt);
    }
    if (expected.size() != 1001) return Fail("expected tile count mismatch");

    auto checkPage = [&](const std::string& cursor, int limit, size_t offset) -> std::string {
        auto page = db.getGlobalCoverWallEntriesPage(cursor, limit, true);
        if (page.entries.size() != static_cast<size_t>(limit)) { Fail("page size mismatch"); return {}; }
        std::unordered_set<std::string> unique;
        for (size_t i = 0; i < page.entries.size(); ++i) {
            if (!unique.insert(page.entries[i].key).second) { Fail("duplicate key in page"); return {}; }
            if (page.entries[i].key != expected[offset + i]) { Fail("page order mismatch"); return {}; }
        }
        return page.nextCursor;
    };
    const std::string cursor1 = checkPage({}, 10, 0);
    if (cursor1.empty()) return Fail("missing first cursor");
    const std::string cursor2 = checkPage(cursor1, 10, 10);
    if (cursor2.empty()) return Fail("second page failed");

    if (!Exec(raw, "UPDATE audio_files SET file_name = 'zzz_last.wav', file_path = 'C:/testlib/folder0000/zzz_last.wav' "
                  "WHERE folder_cover_key = 'F|0000' AND file_name = 'track000_00000.wav';")) return 1;
    if (!db.flushCoverWallOrderIndexDirty(64)) return Fail("flush after rename failed");
    {
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(raw, "SELECT order_name FROM cover_wall_order_index WHERE tile_key = 'F|0000';",
                -1, &stmt, nullptr) != SQLITE_OK) return Fail("rename check prepare failed");
        std::string orderName;
        if (sqlite3_step(stmt) == SQLITE_ROW) orderName = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        sqlite3_finalize(stmt);
        if (orderName != "track000_00001.wav") return Fail("rename did not refresh tile representative: " + orderName);
    }
    if (!Exec(raw, "INSERT INTO audio_files(file_name, file_path, folder_path, has_cover, folder_cover_key, "
                  "embedded_cover_key, file_size, sample_rate, channels, total_frames, duration) "
                  "VALUES('000_first.wav','C:/testlib/folder0500/000_first.wav','C:/testlib/folder0500',1,'F|0500','',1000,48000,2,48000,1.0);")) return 1;
    if (!db.flushCoverWallOrderIndexDirty(64)) return Fail("flush after insert failed");
    auto moved = db.getGlobalCoverWallEntriesPage({}, 1000, true);
    if (moved.entries.empty() || moved.entries.front().key != "F|0500") return Fail("new first file did not move its tile to the front");

    if (!Exec(raw, "DELETE FROM audio_files WHERE folder_cover_key = 'F|0001' AND file_name = 'track001_00000.wav';")) return 1;
    if (!db.flushCoverWallOrderIndexDirty(64)) return Fail("flush after delete failed");
    auto afterDelete = db.getGlobalCoverWallEntriesPage({}, 1000, true);
    std::unordered_set<std::string> keys;
    for (const auto& entry : afterDelete.entries) keys.insert(entry.key);
    if (keys.find("F|0001") == keys.end()) return Fail("deleted representative removed tile");

    if (!Exec(raw, "UPDATE audio_files SET file_name = 'zzz_after.wav' "
                  "WHERE folder_cover_key = 'F|0002' AND file_name = 'track002_00000.wav';")) return 1;
    if (!Database::flushCoverWallOrderIndexDirtyAtPath(dbPath.string(), 64)) {
        return Fail("separate writable connection flush failed");
    }
    {
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(raw, "SELECT order_name FROM cover_wall_order_index WHERE tile_key = 'F|0002';",
                -1, &stmt, nullptr) != SQLITE_OK) return Fail("separate flush check prepare failed");
        std::string orderName;
        if (sqlite3_step(stmt) == SQLITE_ROW) orderName = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        sqlite3_finalize(stmt);
        if (orderName != "track002_00001.wav") return Fail("separate flush did not refresh tile: " + orderName);
    }
    sqlite3_close(raw);
    db.close();
    fs::remove_all(root, removeError);
    std::cout << "unit=ok\n";
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc >= 3 && std::string(argv[1]) == "--build-db") return RunProbe(argv[2], true);
    if (argc >= 3 && std::string(argv[1]) == "--measure-db") return RunProbe(argv[2], false);
    if (argc >= 3 && std::string(argv[1]) == "--check-db") return RunCheck(argv[2]);
    if (argc >= 3 && std::string(argv[1]) == "--pages-db") return RunPages(argv[2]);
    return RunUnit();
}
