#include "Database.h"
#include "PlaylistFolderTree.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

bool Expect(bool condition, const std::string& message) {
    std::printf("%s: %s\n", condition ? "PASS" : "FAIL", message.c_str());
    if (!condition) ++g_failures;
    return condition;
}

bool SameFolderTree(const FolderInfo& a, const FolderInfo& b, std::string* firstMismatch) {
    if (a.path != b.path || a.name != b.name || a.displayName != b.displayName ||
        a.fileCount != b.fileCount || a.subfolders.size() != b.subfolders.size()) {
        if (firstMismatch && firstMismatch->empty()) {
            *firstMismatch = "path=" + a.path + " legacy(count=" + std::to_string(a.fileCount) +
                             ") counts(count=" + std::to_string(b.fileCount) + ")";
        }
        return false;
    }
    for (size_t i = 0; i < a.subfolders.size(); ++i) {
        if (!SameFolderTree(a.subfolders[i], b.subfolders[i], firstMismatch)) return false;
    }
    return true;
}

size_t CountNodes(const FolderInfo& node) {
    size_t total = 1;
    for (const FolderInfo& child : node.subfolders) total += CountNodes(child);
    return total;
}

int MaxDepth(const FolderInfo& node) {
    int depth = 0;
    for (const FolderInfo& child : node.subfolders) {
        const int childDepth = 1 + MaxDepth(child);
        if (childDepth > depth) depth = childDepth;
    }
    return depth;
}

std::string EnvValue(const char* name) {
#ifdef _WIN32
    size_t length = 0;
    char* buffer = nullptr;
    if (_dupenv_s(&buffer, &length, name) != 0 || !buffer) return {};
    std::string value(buffer);
    std::free(buffer);
    return value;
#else
    const char* value = std::getenv(name);
    return value ? std::string(value) : std::string();
#endif
}

}  // namespace

int main() {
    const std::string dbPath = EnvValue("HARKEN_REAL_LIBRARY_DB");
    if (dbPath.empty()) {
        std::printf("SKIP: HARKEN_REAL_LIBRARY_DB is not set; real-library comparison not requested\n");
        return 0;
    }

    Database db;
    if (!db.openReadOnly(dbPath)) {
        std::printf("FAIL: open read-only %s err=%s\n", dbPath.c_str(), db.getLastError().c_str());
        return 1;
    }

    const std::vector<Playlist> playlists = db.getAllPlaylists(false);
    std::printf("library=%s playlists=%zu\n", dbPath.c_str(), playlists.size());
    Expect(!playlists.empty(), "real library exposes playlists");

    int compared = 0;
    int comparedFolders = 0;
    for (const Playlist& playlist : playlists) {
        if (playlist.is_folder) {
            if (!db.isFolder(playlist.id)) continue;
            const auto startRecords = std::chrono::steady_clock::now();
            const std::vector<AudioRecord> aggregated = db.getAggregatedPlaylistFiles(playlist.id);
            const auto startCounts = std::chrono::steady_clock::now();
            const std::vector<PlaylistFolderCount> counts = db.getAggregatedPlaylistFolderCounts(playlist.id);
            const auto endCounts = std::chrono::steady_clock::now();
            if (aggregated.empty() && counts.empty()) continue;

            const auto startLegacyTree = std::chrono::steady_clock::now();
            const FolderInfo legacyTree = BuildPlaylistFolderTreeFromRecords(aggregated);
            const auto startCountTree = std::chrono::steady_clock::now();
            const FolderInfo countTree = BuildPlaylistFolderTreeFromFolderCounts(counts, (int64_t)aggregated.size());
            const auto endCountTree = std::chrono::steady_clock::now();

            std::string mismatch;
            const bool same = SameFolderTree(legacyTree, countTree, &mismatch);
            std::printf("folder-playlist id=%lld name=%s members=%zu folders=%zu depth=%d records=%.2fms counts=%.2fms treeOld=%.2fms treeNew=%.2fms\n",
                        (long long)playlist.id, playlist.name.c_str(), aggregated.size(),
                        CountNodes(legacyTree) - 1, MaxDepth(legacyTree),
                        std::chrono::duration<double, std::milli>(startCounts - startRecords).count(),
                        std::chrono::duration<double, std::milli>(endCounts - startCounts).count(),
                        std::chrono::duration<double, std::milli>(startCountTree - startLegacyTree).count(),
                        std::chrono::duration<double, std::milli>(endCountTree - startCountTree).count());
            Expect(same, std::string("aggregated folder tree matches for ") + playlist.name +
                             (mismatch.empty() ? std::string() : " first mismatch " + mismatch));
            ++comparedFolders;
            continue;
        }

        const auto startRecords = std::chrono::steady_clock::now();
        const std::vector<AudioRecord> records = db.getPlaylistFiles(playlist.id);
        const auto startCounts = std::chrono::steady_clock::now();
        const std::vector<PlaylistFolderCount> counts = db.getPlaylistFolderCounts(playlist.id);
        const auto endCounts = std::chrono::steady_clock::now();

        const auto startLegacyTree = std::chrono::steady_clock::now();
        const FolderInfo legacyTree = BuildPlaylistFolderTreeFromRecords(records);
        const auto startCountTree = std::chrono::steady_clock::now();
        const FolderInfo countTree = BuildPlaylistFolderTreeFromFolderCounts(counts, (int64_t)records.size());
        const auto endCountTree = std::chrono::steady_clock::now();

        const double recordsMs = std::chrono::duration<double, std::milli>(startCounts - startRecords).count();
        const double countsMs = std::chrono::duration<double, std::milli>(endCounts - startCounts).count();
        const double legacyTreeMs = std::chrono::duration<double, std::milli>(startCountTree - startLegacyTree).count();
        const double countTreeMs = std::chrono::duration<double, std::milli>(endCountTree - startCountTree).count();

        std::string mismatch;
        const bool same = SameFolderTree(legacyTree, countTree, &mismatch);
        std::printf("playlist id=%lld name=%s members=%zu folders=%zu depth=%d records=%.2fms counts=%.2fms treeOld=%.2fms treeNew=%.2fms oldTotal=%.2fms newTotal=%.2fms\n",
                    (long long)playlist.id, playlist.name.c_str(), records.size(),
                    CountNodes(legacyTree) - 1, MaxDepth(legacyTree),
                    recordsMs, countsMs, legacyTreeMs, countTreeMs,
                    recordsMs + legacyTreeMs, countsMs + countTreeMs);
        Expect(same, std::string("playlist tree matches for ") + playlist.name +
                         (mismatch.empty() ? std::string() : " first mismatch " + mismatch));
        if (records.size() > 1000) {
            Expect(countTreeMs <= legacyTreeMs * 1.5 + 5.0,
                   "count-built tree stays within 1.5x of record-built tree at " +
                       std::to_string(records.size()) + " members");
        }
        ++compared;
    }


    const std::string measureSwitch = EnvValue("HARKEN_MEASURE_SWITCH");
    if (!measureSwitch.empty()) {
        SearchFilter emptyFilter;
        for (const Playlist& playlist : playlists) {
            const bool folder = playlist.is_folder;
            const int64_t storedTotal = folder
                ? db.getAggregatedPlaylistFileCount(playlist.id)
                : db.getPlaylistFileCount(playlist.id);
            if (storedTotal < 1000) continue;

            const auto t0 = std::chrono::steady_clock::now();
            const int64_t filteredTotal = folder
                ? db.searchAggregatedPlaylistFileCount(playlist.id, emptyFilter)
                : db.searchPlaylistFileCount(playlist.id, emptyFilter);
            const auto t1 = std::chrono::steady_clock::now();
            const std::vector<AudioRecord> page = folder
                ? db.searchAggregatedPlaylistFilesPage(playlist.id, emptyFilter, 500, 0)
                : db.searchPlaylistFilesPage(playlist.id, emptyFilter, 500, 0);
            const auto t2 = std::chrono::steady_clock::now();
            const std::vector<AudioRecord> coverKeys = folder
                ? db.searchAggregatedPlaylistCoverKeys(playlist.id, emptyFilter)
                : db.searchPlaylistCoverKeys(playlist.id, emptyFilter);
            const auto t3 = std::chrono::steady_clock::now();

            std::printf("switch id=%lld name=%s folder=%d total=%lld count=%.2fms page500=%.2fms rows=%zu coverKeys=%.2fms keys=%zu\n",
                        (long long)playlist.id, playlist.name.c_str(), folder ? 1 : 0,
                        (long long)filteredTotal,
                        std::chrono::duration<double, std::milli>(t1 - t0).count(),
                        std::chrono::duration<double, std::milli>(t2 - t1).count(),
                        page.size(),
                        std::chrono::duration<double, std::milli>(t3 - t2).count(),
                        coverKeys.size());
        }
    }

    std::printf("compared playlists=%d folder-playlists=%d failures=%d\n", compared, comparedFolders, g_failures);
    Expect(compared > 0, "at least one plain playlist compared against the real library");
    return g_failures == 0 ? 0 : 1;
}
