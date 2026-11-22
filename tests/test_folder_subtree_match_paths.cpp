#include "FolderPathMatch.h"
#include "sqlite3.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <set>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void check(bool condition, const char* what) {
    if (!condition) {
        ++g_failures;
        std::cout << "FAIL " << what << "\n";
    } else {
        std::cout << "ok   " << what << "\n";
    }
}

bool contains(const std::vector<std::string>& paths, const std::string& value) {
    return std::find(paths.begin(), paths.end(), value) != paths.end();
}

std::set<int64_t> matchRows(sqlite3* db, const std::vector<std::string>& matchPaths) {
    std::set<int64_t> ids;
    const char* sql =
        "SELECT id FROM audio_files "
        "WHERE folder_path = ? "
        "   OR (folder_path >= ? AND folder_path < ?) "
        "   OR (file_path >= ? AND file_path < ?)";
    for (const std::string& matchPath : matchPaths) {
        const std::string childPrefix = HarkenSearch::MakeFolderChildPrefix(matchPath);
        const std::string childUpper = HarkenSearch::MakeFolderPathUpperBound(childPrefix);
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) continue;
        sqlite3_bind_text(stmt, 1, matchPath.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, childPrefix.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, childUpper.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 4, childPrefix.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 5, childUpper.c_str(), -1, SQLITE_TRANSIENT);
        while (sqlite3_step(stmt) == SQLITE_ROW) ids.insert(sqlite3_column_int64(stmt, 0));
        sqlite3_finalize(stmt);
    }
    return ids;
}

}  // namespace

int main() {
    const std::string plain = "\\\\server\\share\\Sound Library\\Business\\SFX";
    const std::string longUnc = "\\\\?\\UNC\\server\\share\\Sound Library\\Business\\SFX";
    const std::string legacy = "\\server\\share\\Sound Library\\Business\\SFX";

    const auto fromPlain = HarkenSearch::BuildFolderSubtreeMatchPaths(plain, plain);
    check(contains(fromPlain, plain), "plain UNC kept");
    check(contains(fromPlain, longUnc), "plain UNC also matches extended long-path rows");
    check(contains(fromPlain, legacy), "plain UNC also matches legacy single-backslash rows");

    const auto fromLong = HarkenSearch::BuildFolderSubtreeMatchPaths(longUnc, longUnc);
    check(contains(fromLong, longUnc), "extended long path kept");
    check(contains(fromLong, plain), "extended long path also matches plain UNC rows");
    check(contains(fromLong, legacy), "extended long path also matches legacy rows");

    const auto fromLegacy = HarkenSearch::BuildFolderSubtreeMatchPaths(legacy, legacy);
    check(contains(fromLegacy, plain), "legacy single-backslash path also matches plain UNC rows");
    check(contains(fromLegacy, longUnc), "legacy single-backslash path also matches long-path rows");

    const auto fromLocal = HarkenSearch::BuildFolderSubtreeMatchPaths("d:\\libs\\SFX", "D:\\libs\\SFX");
    check(contains(fromLocal, "D:\\libs\\SFX"), "local drive path keeps scanned case");
    check(contains(fromLocal, "d:\\libs\\SFX"), "local drive path also matches lowercase drive rows");
    check(contains(fromLocal, "\\\\?\\D:\\libs\\SFX"), "local drive path also matches extended long-path rows");

    const auto trailing = HarkenSearch::BuildFolderSubtreeMatchPaths(plain + "\\", plain + "\\");
    check(contains(trailing, plain), "trailing separator trimmed");

    const auto driveInsideUnc = HarkenSearch::BuildFolderSubtreeMatchPaths(
        "\\\\nas\\Music\\d:\\odd", "\\\\nas\\Music\\d:\\odd");
    check(contains(driveInsideUnc, "\\\\nas\\Music\\d:\\odd"),
          "colon inside a UNC name is not treated as a drive");

    std::vector<std::string> duplicateProbe = HarkenSearch::BuildFolderSubtreeMatchPaths(plain, longUnc);
    std::set<std::string> uniqueProbe(duplicateProbe.begin(), duplicateProbe.end());
    check(uniqueProbe.size() == duplicateProbe.size(), "match paths contain no duplicates");

    check(HarkenSearch::MakeFolderChildPrefix(plain) == plain + "\\", "child prefix appends one separator");
    check(HarkenSearch::MakeFolderChildPrefix(plain + "\\") == plain + "\\",
          "child prefix does not double the separator");
    check(HarkenSearch::MakeFolderPathUpperBound("a") == std::string("a\xff"),
          "upper bound appends 0xFF");

    // Behaviour regression: one root stored under two prefixes must be removed as a unit.
    const std::string dbPath = std::string(std::getenv("TEMP") ? std::getenv("TEMP") : ".") +
                               "\\harken-folder-match-test.db";
    std::remove(dbPath.c_str());
    sqlite3* db = nullptr;
    if (sqlite3_open(dbPath.c_str(), &db) != SQLITE_OK) {
        std::cout << "FAIL cannot open temp database\n";
        return 1;
    }
    sqlite3_exec(db,
                 "CREATE TABLE audio_files (id INTEGER PRIMARY KEY, file_path TEXT, folder_path TEXT);"
                 "INSERT INTO audio_files (id, file_path, folder_path) VALUES"
                 " (1, '\\\\server\\share\\Sound Library\\Business\\SFX\\a.wav',"
                 "     '\\\\server\\share\\Sound Library\\Business\\SFX'),"
                 " (2, '\\\\?\\UNC\\server\\share\\Sound Library\\Business\\SFX\\b.wav',"
                 "     '\\\\?\\UNC\\server\\share\\Sound Library\\Business\\SFX'),"
                 " (3, '\\server\\share\\Sound Library\\Business\\SFX\\c.wav',"
                 "     '\\server\\share\\Sound Library\\Business\\SFX'),"
                 " (4, '\\\\server\\share\\Sound Library\\Business\\SFX2\\keep.wav',"
                 "     '\\\\server\\share\\Sound Library\\Business\\SFX2');",
                 nullptr, nullptr, nullptr);

    const auto candidatePaths =
        HarkenSearch::BuildFolderSubtreeMatchPaths(plain, "\\\\?\\UNC\\server\\share\\Sound Library\\Business\\SFX");
    const std::set<int64_t> matched = matchRows(db, candidatePaths);
    check(matched.count(1) == 1, "db: plain UNC row matched");
    check(matched.count(2) == 1, "db: extended long-path row matched");
    check(matched.count(3) == 1, "db: legacy single-backslash row matched");
    check(matched.count(4) == 0, "db: sibling folder with a longer name is not matched");
    check(matched.size() == 3, "db: one root with two prefixes resolves to the same subtree");

    sqlite3_close(db);
    std::remove(dbPath.c_str());

    if (g_failures != 0) {
        std::cout << g_failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "all checks passed\n";
    return 0;
}
