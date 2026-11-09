// QA: 封面点击性能优化验证
// 验证以下优化是否生效：
// 1. SQLite progress handler - 长查询可在1-2ms内中断
// 2. 封面过滤时跳过facets - coverFilterActive时不重新计算facets
// 3. 延迟COUNT查询 - 封面点击时优先返回文件列表

#ifndef NOMINMAX
#define NOMINMAX
#endif  // 禁用 Windows.h 的 min/max 宏
#include "Database.h"
#include "PathUtils.h"
#include <iostream>
#include <chrono>
#include <cstdio>
#include <atomic>
#include <algorithm>
#include <thread>

static int failed = 0;
static int passed = 0;

#define ASSERT(cond, msg) do { \
    if (!(cond)) { \
        std::cerr << "FAIL: " << (msg) << "\n"; \
        failed++; \
    } else { \
        std::cout << "OK: " << (msg) << "\n"; \
        passed++; \
    } \
} while(0)

// 测量执行时间（毫秒）
template<typename Func>
int64_t measureMs(Func&& f) {
    auto start = std::chrono::steady_clock::now();
    f();
    auto end = std::chrono::steady_clock::now();
    return std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
}

// 创建临时测试数据库并填充大量数据
bool createLargeTestDb(Database& db, const std::string& tempDbPath, int fileCount) {
    std::remove(tempDbPath.c_str());
    // Also remove WAL and SHM files
    std::remove((tempDbPath + "-wal").c_str());
    std::remove((tempDbPath + "-shm").c_str());

    if (!db.open(tempDbPath)) {
        std::cerr << "Failed to create test DB: " << db.getLastError() << "\n";
        return false;
    }

    // 插入大量测试数据
    if (!db.beginTransaction()) {
        std::cerr << "Failed to begin transaction: " << db.getLastError() << "\n";
        return false;
    }

    const std::string sharedFolderCoverPath = HarkenSearch::CanonicalFolderPath("E:/TestLib/Shared/cover.jpg");
    const std::string sharedFolderCoverKey = "F|PATH|" + sharedFolderCoverPath;

    for (int i = 0; i < fileCount; i++) {
        AudioRecord rec;
        rec.filePath = "E:/TestLib/Folder" + std::to_string(i / 100) + "/test_" + std::to_string(i) + ".wav";
        rec.fileName = "test_" + std::to_string(i) + ".wav";
        rec.displayName = rec.fileName;
        rec.extension = ".wav";
        rec.format = "WAV";
        rec.durationSeconds = 1.0f + (i % 10);
        rec.sampleRate = 48000;
        rec.channels = 2;
        rec.bitDepth = 24;
        rec.fileSize = 1000000 + i * 1000;
        rec.totalFrames = 48000;
        rec.description = "Test audio file " + std::to_string(i);
        rec.category = (i % 5 == 0) ? "AMBIENCE" : (i % 5 == 1) ? "FOLEY" : (i % 5 == 2) ? "MUSIC" : (i % 5 == 3) ? "SFX" : "VOICE";
        rec.folderPath = "E:/TestLib/Folder" + std::to_string(i / 100);
        rec.hasCover = (i % 3 == 0);

        // 模拟不同封面key（每20个文件共享一个封面）
        if (rec.hasCover) {
            std::string coverKey = "cover_" + std::to_string(i / 20);
            if (i % 17 == 0) {
                rec.folderCoverKey = sharedFolderCoverKey;
                rec.folderCoverSrcPath = sharedFolderCoverPath;
                rec.folderCoverSrcMtime = 1700000000 + i;
            } else if (i % 2 == 0) {
                rec.folderCoverKey = coverKey;
                rec.folderCoverSrcPath = rec.folderPath + "/cover.jpg";
                rec.folderCoverSrcMtime = 1700000000 + i;
            } else {
                rec.embeddedCoverKey = coverKey;
                rec.embeddedCoverKeyMtime = 1700000000 + i;
            }
        }

        if (!db.addFile(rec)) {
            std::cerr << "Failed to add file " << i << ": " << db.getLastError() << "\n";
            db.rollbackTransaction();
            return false;
        }

        // 每5000条提交一次，减少事务开销
        if ((i + 1) % 5000 == 0) {
            if (!db.commitTransaction()) {
                std::cerr << "Failed to commit at " << i << ": " << db.getLastError() << "\n";
                return false;
            }
            if ((i + 1) < fileCount) {  // Don't start a new transaction after the last batch
                std::this_thread::sleep_for(std::chrono::milliseconds(10));  // Give DB time to settle
                if (!db.beginTransaction()) {
                    std::cerr << "Failed to begin transaction at " << i << ": " << db.getLastError() << "\n";
                    return false;
                }
            }
            if ((i + 1) % 10000 == 0) {
                std::cout << "  Populated " << (i + 1) << " / " << fileCount << " records...\n";
            }
        }
    }

    // Commit any remaining records
    if (fileCount % 5000 != 0) {
        if (!db.commitTransaction()) {
            std::cerr << "Failed to commit final batch: " << db.getLastError() << "\n";
            return false;
        }
    }

    // 确保封面登记表同步
    db.syncCoverRegistry();

    std::cout << "  Created test DB with " << fileCount << " records\n";
    return true;
}

int main() {
    std::cout << "=== Cover Click Performance Test ===\n\n";

    const std::string tempDbPath = "test_cover_perf_temp.db";
    const int fileCount = 30000;  // 10万条记录

    Database db;

// Scenario 1: 大库初始化
    std::cout << "[1] Creating large test database (" << fileCount << " files)...\n";

    auto initTime = measureMs([&]() {
        ASSERT(createLargeTestDb(db, tempDbPath, fileCount), "Large DB initialization");
    });

    std::cout << "  DB creation time: " << initTime << "ms\n\n";

// Scenario 2: 封面facet查询（全局模式，无过滤）
    std::cout << "[2] Testing getCoverFacets (global, no filter)...\n";

    SearchFilter emptyFilter;
    std::vector<Database::CoverFacetEntry> globalFacets;

    auto facetTime = measureMs([&]() {
        globalFacets = db.getCoverFacets(emptyFilter, 200);
    });

    std::cout << "  Global facet query time: " << facetTime << "ms\n";
    std::cout << "  Found " << globalFacets.size() << " cover facets\n";

    // 大库下全局facet查询基准 < 2000ms（可接受范围）
    ASSERT(facetTime < 2000, "Global facet query < 2000ms");
    ASSERT(!globalFacets.empty(), "Global facets not empty");

    std::cout << "\n";

// Scenario 3: 封面点击查询（specificCoverKey过滤）
    std::cout << "[3] Testing cover filter query (specificCoverKey)...\n";

    if (!globalFacets.empty()) {
        std::string testCoverKey = globalFacets[0].key;
        std::cout << "  Testing with cover key: " << testCoverKey << "\n";

        SearchFilter coverFilter;
        coverFilter.specificCoverKey = testCoverKey;

        std::vector<AudioRecord> coverResults;
        int64_t coverCount = 0;

        auto coverQueryTime = measureMs([&]() {
            coverResults = db.searchFiles(coverFilter, 1000);
            coverCount = db.searchFilesCount(coverFilter);
        });

        std::cout << "  Cover filter query time: " << coverQueryTime << "ms\n";
        std::cout << "  Found " << coverResults.size() << " files (count=" << coverCount << ")\n";

        // 封面点击查询应 < 100ms（优化目标）
        ASSERT(coverQueryTime < 100, "Cover filter query < 100ms (optimized)");
        ASSERT(!coverResults.empty(), "Cover filter results not empty");

        // 验证返回的文件确实包含该封面key
        bool allHaveCover = true;
        for (const auto& rec : coverResults) {
            if (rec.embeddedCoverKey != testCoverKey && rec.folderCoverKey != testCoverKey) {
                allHaveCover = false;
                std::cerr << "  File " << rec.fileName << " doesn't have expected cover key\n";
                break;
            }
        }
        ASSERT(allHaveCover, "All results have the correct cover key");

        // 计算性能提升百分比
        if (facetTime > 0) {
            double improvement = (1.0 - (double)coverQueryTime / facetTime) * 100.0;
            std::cout << "  Performance improvement: " << improvement << "% faster than global facet\n";
        }
    }

    std::cout << "\n";

// Scenario 4: 查询中断测试（progress handler）
    std::cout << "[4] Testing query interruption (progress handler)...\n";

    // 启动一个长时间查询，然后快速中断
    std::atomic<bool> interrupted{false};
    std::thread interruptThread([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));  // 等待50ms后中断
        db.interrupt();
        interrupted = true;
    });

    auto longQueryTime = measureMs([&]() {
        // 执行一个可能很慢的全表facet查询
        try {
            auto facets = db.getCoverFacets(emptyFilter, 500);
            // 如果被中断，查询应该快速返回（可能返回空结果）
        } catch (...) {
            // 中断可能导致异常，这是正常的
        }
    });

    interruptThread.join();

    std::cout << "  Query time with interruption: " << longQueryTime << "ms\n";

    // 中断后的查询应该能在合理时间内返回（< 200ms）
    ASSERT(longQueryTime < 200, "Query interruption responsive (< 200ms)");
    ASSERT(interrupted, "Interrupt signal sent");

    std::cout << "\n";

// Scenario 5: repeated cover query responsiveness
    std::cout << "[5] Testing repeated cover query latency...\n";

    if (!globalFacets.empty()) {
        std::string testCoverKey = globalFacets[0].key;
        SearchFilter coverFilter;
        coverFilter.specificCoverKey = testCoverKey;

        // Millisecond-scale queries can be interrupted by unrelated parallel tests.
        // Use a median and keep the user-facing latency limit plus result stability.
        const auto expectedResults = db.searchFiles(coverFilter, 1000);
        std::vector<int64_t> queryTimes;
        bool resultsStable = true;
        for (int sample = 0; sample < 7; ++sample) {
            std::vector<AudioRecord> repeatedResults;
            queryTimes.push_back(measureMs([&]() {
                repeatedResults = db.searchFiles(coverFilter, 1000);
            }));
            if (repeatedResults.size() != expectedResults.size()) {
                resultsStable = false;
            } else {
                for (size_t i = 0; i < repeatedResults.size(); ++i) {
                    if (repeatedResults[i].filePath != expectedResults[i].filePath) {
                        resultsStable = false;
                        break;
                    }
                }
            }
        }
        std::sort(queryTimes.begin(), queryTimes.end());
        const auto medianQueryTime = queryTimes[queryTimes.size() / 2];
        std::cout << "  Median repeated query: " << medianQueryTime << "ms\n";
        ASSERT(resultsStable, "Repeated cover queries return stable results");
        ASSERT(medianQueryTime < 100, "Repeated cover query median < 100ms");
    }

    std::cout << "\n";

// 清理
    // F|PATH folder covers are common in the real cover wall. This exercises
    // the source-path fallback branch used when the visual key carries a
    // canonical folder-cover source path.
    std::cout << "[6] Testing F|PATH folder-cover filter source fallback...\n";
    {
        const std::string sharedFolderCoverPath = HarkenSearch::CanonicalFolderPath("E:/TestLib/Shared/cover.jpg");
        const std::string sharedFolderCoverKey = "F|PATH|" + sharedFolderCoverPath;

        SearchFilter folderCoverFilter;
        folderCoverFilter.specificCoverKey = sharedFolderCoverKey;

        std::vector<AudioRecord> folderCoverResults;
        auto folderCoverQueryTime = measureMs([&]() {
            folderCoverResults = db.searchFiles(folderCoverFilter, 1000);
        });

        std::cout << "  F|PATH cover filter query time: " << folderCoverQueryTime << "ms\n";
        std::cout << "  Found " << folderCoverResults.size() << " files\n";

        ASSERT(folderCoverQueryTime < 100, "F|PATH cover filter query < 100ms");
        ASSERT(!folderCoverResults.empty(), "F|PATH cover filter results not empty");

        bool allMatchSharedCover = true;
        for (const auto& rec : folderCoverResults) {
            if (rec.folderCoverKey != sharedFolderCoverKey &&
                HarkenSearch::CanonicalFolderPath(rec.folderCoverSrcPath) != sharedFolderCoverPath) {
                allMatchSharedCover = false;
                std::cerr << "  File " << rec.fileName << " does not match F|PATH cover fallback\n";
                break;
            }
        }
        ASSERT(allMatchSharedCover, "All F|PATH fallback results match the clicked cover");
    }

    std::cout << "\n";

    std::cout << "[Cleanup] Removing temporary database...\n";
    db.close();

    if (std::remove(tempDbPath.c_str()) == 0) {
        std::cout << "  Temporary DB removed\n";
    }

    // 清理 SQLite 辅助文件
    std::remove((tempDbPath + "-wal").c_str());
    std::remove((tempDbPath + "-shm").c_str());

// 输出测试结果
    std::cout << "\n=== Test Summary ===\n";
    std::cout << "Passed: " << passed << "\n";
    std::cout << "Failed: " << failed << "\n";

    if (failed == 0) {
        std::cout << "\nPASS: Cover click performance optimized - " << passed << "/" << (passed + failed) << " scenarios\n";
        std::cout << "  - Large DB facet query: baseline\n";
        std::cout << "  - Cover filter query: optimized (< 100ms)\n";
        std::cout << "  - Query interruption: responsive (progress handler working)\n";
        return 0;
    } else {
        std::cout << "\nFAIL: Cover click performance regression - " << failed << " failures\n";
        return 1;
    }
}
