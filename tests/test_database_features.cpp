
// 数据库拆分和路径索引功能测试程序

#include "Database.h"
#include "AudioEngine.h"
#include "CoverModule.h"
#include "CoverWallSelectionKey.h"
#include "PathUtils.h"
#include "SimilaritySearch.h"
#include "Utils.h"
#include "sqlite3.h"
#include <iostream>
#include <filesystem>
#include <vector>
#include <string>
#include <fstream>
#include <sstream>
#include <chrono>
#include <thread>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <algorithm>

namespace fs = std::filesystem;

void printSeparator(const std::string& title) {
    std::cout << "\n" << std::string(60, '=') << "\n";
    std::cout << "  " << title << "\n";
    std::cout << std::string(60, '=') << "\n";
}

static std::string readFirstExistingTextFile(const std::vector<std::string>& candidates) {
    for (const std::string& path : candidates) {
        std::ifstream in(path, std::ios::binary);
        if (!in) continue;
        std::ostringstream ss;
        ss << in.rdbuf();
        std::string text = ss.str();
        size_t write = 0;
        for (size_t read = 0; read < text.size(); ++read) {
            if (text[read] != '\r') {
                text[write++] = text[read];
            }
        }
        text.resize(write);
        return text;
    }
    return {};
}

static std::string extractFunctionBody(const std::string& source, const std::string& signature) {
    const size_t sig = source.find(signature);
    if (sig == std::string::npos) return {};
    const size_t open = source.find('{', sig);
    if (open == std::string::npos) return {};

    int depth = 0;
    for (size_t i = open; i < source.size(); ++i) {
        if (source[i] == '{') {
            ++depth;
        } else if (source[i] == '}') {
            --depth;
            if (depth == 0) {
                return source.substr(open, i - open + 1);
            }
        }
    }
    return {};
}

void testDatabaseSplitting(Database& db, const std::string& dbPath) {
    printSeparator("测试1: 数据库拆分");
    
    // 检查数据库文件是否存在
    std::string blobsPath = db.getBlobsDbPathPublic(dbPath);
    std::string waveformsPath = db.getWaveformsDbPathPublic(dbPath);
    
    std::cout << "主数据库: " << dbPath << "\n";
    std::cout << "Blobs数据库: " << blobsPath << "\n";
    std::cout << "Waveforms数据库: " << waveformsPath << "\n\n";
    
    // 检查文件是否存在
    bool mainExists = fs::exists(fs::path(dbPath));
    bool blobsExists = fs::exists(fs::path(blobsPath));
    bool waveformsExists = fs::exists(fs::path(waveformsPath));
    
    std::cout << "文件存在性检查:\n";
    std::cout << "  main.db: " << (mainExists ? "✓ 存在" : "✗ 不存在") << "\n";
    std::cout << "  blobs.db: " << (blobsExists ? "✓ 存在" : "✗ 不存在") << "\n";
    std::cout << "  waveforms.db: " << (waveformsExists ? "✓ 存在" : "✗ 不存在") << "\n\n";
    
    if (!mainExists || !blobsExists || !waveformsExists) {
        std::cout << "⚠ 警告: 某些数据库文件不存在。请先运行程序打开数据库。\n";
        return;
    }
    
    // 检查文件大小
    if (mainExists) {
        auto mainSize = fs::file_size(fs::path(dbPath));
        std::cout << "主数据库大小: " << mainSize << " 字节\n";
    }
    if (blobsExists) {
        auto blobsSize = fs::file_size(fs::path(blobsPath));
        std::cout << "Blobs数据库大小: " << blobsSize << " 字节\n";
    }
    if (waveformsExists) {
        auto waveformsSize = fs::file_size(fs::path(waveformsPath));
        std::cout << "Waveforms数据库大小: " << waveformsSize << " 字节\n";
    }
}

void testPathIndexing(Database& db) {
    printSeparator("测试2: 路径索引");
    
    // 测试路径列表
    std::vector<std::string> testPaths = {
        "C:/Music",
        "C:/Music/Album1",
        "C:/Music/Album1/Track1",
        "C:/Music/Album2",
        "D:/Audio",
        "D:/Audio/SoundEffects"
    };
    
    std::cout << "测试路径索引API:\n\n";
    
    // 测试 getOrCreateFolderId
    std::cout << "1. 测试 getOrCreateFolderId():\n";
    std::vector<int64_t> folderIds;
    for (const auto& path : testPaths) {
        int64_t id = db.getOrCreateFolderId(path);
        folderIds.push_back(id);
        std::cout << "   " << path << " -> ID: " << id;
        if (id > 0) {
            std::cout << " ✓";
        } else {
            std::cout << " ✗ (失败)";
        }
        std::cout << "\n";
    }
    
    std::cout << "\n2. 测试 getFolderPathById():\n";
    for (size_t i = 0; i < folderIds.size(); ++i) {
        if (folderIds[i] > 0) {
            std::string path = db.getFolderPathById(folderIds[i]);
            std::cout << "   ID: " << folderIds[i] << " -> " << path;
            if (path == testPaths[i]) {
                std::cout << " ✓";
            } else {
                std::cout << " ✗ (不匹配: 期望 " << testPaths[i] << ")";
            }
            std::cout << "\n";
        }
    }
    
    std::cout << "\n3. 测试 getFolderIdByPath():\n";
    for (const auto& path : testPaths) {
        int64_t id = db.getFolderIdByPath(path);
        std::cout << "   " << path << " -> ID: " << id;
        if (id > 0) {
            std::cout << " ✓";
        } else {
            std::cout << " ✗ (未找到)";
        }
        std::cout << "\n";
    }
    
    std::cout << "\n4. 测试 getFolderIdMap() (批量查询):\n";
    auto idMap = db.getFolderIdMap(testPaths);
    std::cout << "   批量查询结果: " << idMap.size() << " 个路径找到ID\n";
    for (const auto& [path, id] : idMap) {
        std::cout << "   " << path << " -> ID: " << id << "\n";
    }
    
    std::cout << "\n5. 测试 getFolderFileCountById():\n";
    for (size_t i = 0; i < folderIds.size() && i < 3; ++i) {
        if (folderIds[i] > 0) {
            int64_t count = db.getFolderFileCountById(folderIds[i]);
            std::cout << "   ID: " << folderIds[i] << " (" << testPaths[i] << ") -> 文件数: " << count << "\n";
        }
    }
}

void testDatabaseStructure(Database& db) {
    printSeparator("测试3: 数据库结构验证");
    
    // 这些测试需要直接访问数据库，我们只能通过API测试
    std::cout << "数据库结构验证:\n";
    std::cout << "  - directories 表应该已创建\n";
    std::cout << "  - audio_files 表应该包含 folder_id 列\n";
    std::cout << "  - blobs_db.cover_thumbs 表应该已创建\n";
    std::cout << "  - waveforms_db.waveforms 表应该已创建\n\n";
    std::cout << "提示: 使用 SQLite 工具可以查看详细结构:\n";
    std::cout << "  sqlite3 meta.db \"SELECT name FROM sqlite_master WHERE type='table';\"\n";
    std::cout << "  sqlite3 meta.db \"PRAGMA table_info(audio_files);\"\n";
}

static void removeDbFamily(const std::string& dbPath) {
    (void)std::remove(dbPath.c_str());
    (void)std::remove((dbPath + "-wal").c_str());
    (void)std::remove((dbPath + "-shm").c_str());
    std::string base = dbPath;
    const size_t dot = base.rfind('.');
    if (dot != std::string::npos) base = base.substr(0, dot);
    (void)std::remove((base + "_blobs.db").c_str());
    (void)std::remove((base + "_blobs.db-wal").c_str());
    (void)std::remove((base + "_blobs.db-shm").c_str());
    (void)std::remove((base + "_waveforms.db").c_str());
    (void)std::remove((base + "_waveforms.db-wal").c_str());
    (void)std::remove((base + "_waveforms.db-shm").c_str());
}

static int queryAppMetaInt(Database& db, const char* key) {
    if (!key || !*key) return 0;
    int value = 0;
    std::string sql = std::string("SELECT value FROM app_meta WHERE key = '") + key + "' LIMIT 1";
    db.execQuery(sql.c_str(), [&](sqlite3_stmt* stmt) {
        const unsigned char* text = sqlite3_column_text(stmt, 0);
        if (text) value = std::atoi(reinterpret_cast<const char*>(text));
    });
    return value;
}

static std::string queryAppMetaString(Database& db, const char* key) {
    if (!key || !*key) return {};
    std::string value;
    std::string sql = std::string("SELECT value FROM app_meta WHERE key = '") + key + "' LIMIT 1";
    db.execQuery(sql.c_str(), [&](sqlite3_stmt* stmt) {
        const unsigned char* text = sqlite3_column_text(stmt, 0);
        value = text ? reinterpret_cast<const char*>(text) : "";
    });
    return value;
}

static int queryIntPragma(Database& db, const char* pragmaSql, int fallback = -1) {
    int value = fallback;
    db.execQuery(pragmaSql, [&](sqlite3_stmt* stmt) {
        value = sqlite3_column_int(stmt, 0);
    });
    return value;
}

static std::string queryTextPragma(Database& db, const char* pragmaSql) {
    std::string value;
    db.execQuery(pragmaSql, [&](sqlite3_stmt* stmt) {
        const unsigned char* text = sqlite3_column_text(stmt, 0);
        value = text ? reinterpret_cast<const char*>(text) : "";
    });
    return value;
}

static bool execSqlOnDbPath(const std::string& dbPath, const std::vector<std::string>& statements) {
    sqlite3* rawDb = nullptr;
    if (sqlite3_open(dbPath.c_str(), &rawDb) != SQLITE_OK || !rawDb) {
        if (rawDb) sqlite3_close(rawDb);
        return false;
    }

    for (const std::string& sql : statements) {
        char* err = nullptr;
        const int rc = sqlite3_exec(rawDb, sql.c_str(), nullptr, nullptr, &err);
        if (err) sqlite3_free(err);
        if (rc != SQLITE_OK) {
            sqlite3_close(rawDb);
            return false;
        }
    }

    sqlite3_close(rawDb);
    return true;
}

static std::string escapeSqlLiteral(std::string value) {
    size_t pos = 0;
    while ((pos = value.find('\'', pos)) != std::string::npos) {
        value.insert(pos, "'");
        pos += 2;
    }
    return value;
}

static fs::path makeTempRoot(const std::string& name) {
    fs::path root = fs::temp_directory_path() / ("aura_database_features_" + name);
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);
    return root;
}

static void writeSilentMonoWav(const fs::path& path, int sampleRate = 8000, int frameCount = 128) {
    const int channels = 1;
    const int bitsPerSample = 16;
    const int blockAlign = channels * (bitsPerSample / 8);
    const int byteRate = sampleRate * blockAlign;
    const std::uint32_t dataSize = static_cast<std::uint32_t>(frameCount * blockAlign);
    const std::uint32_t riffSize = 36u + dataSize;

    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write("RIFF", 4);
    out.write(reinterpret_cast<const char*>(&riffSize), sizeof(riffSize));
    out.write("WAVE", 4);
    out.write("fmt ", 4);

    const std::uint32_t fmtChunkSize = 16;
    const std::uint16_t audioFormat = 1;
    const std::uint16_t numChannels = static_cast<std::uint16_t>(channels);
    const std::uint16_t bits = static_cast<std::uint16_t>(bitsPerSample);
    const std::uint16_t align = static_cast<std::uint16_t>(blockAlign);
    out.write(reinterpret_cast<const char*>(&fmtChunkSize), sizeof(fmtChunkSize));
    out.write(reinterpret_cast<const char*>(&audioFormat), sizeof(audioFormat));
    out.write(reinterpret_cast<const char*>(&numChannels), sizeof(numChannels));
    out.write(reinterpret_cast<const char*>(&sampleRate), sizeof(sampleRate));
    out.write(reinterpret_cast<const char*>(&byteRate), sizeof(byteRate));
    out.write(reinterpret_cast<const char*>(&align), sizeof(align));
    out.write(reinterpret_cast<const char*>(&bits), sizeof(bits));

    out.write("data", 4);
    out.write(reinterpret_cast<const char*>(&dataSize), sizeof(dataSize));
    std::vector<std::int16_t> pcm(static_cast<size_t>(frameCount * channels), 0);
    out.write(reinterpret_cast<const char*>(pcm.data()),
              static_cast<std::streamsize>(pcm.size() * sizeof(std::int16_t)));
}

static void writeTinyCoverImage(const fs::path& path) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    const char bytes[] = "not-a-real-jpeg-but-large-enough-for-thumb-cache";
    out.write(bytes, sizeof(bytes));
}

static bool insertCoveredRowsDirect(Database& db, int count, const std::string& prefix) {
    if (!db.db() || count <= 0) return false;
    char* err = nullptr;
    if (sqlite3_exec(db.db(), "BEGIN IMMEDIATE;", nullptr, nullptr, &err) != SQLITE_OK) {
        if (err) sqlite3_free(err);
        return false;
    }

    const char* sql =
        "INSERT INTO audio_files (file_path, file_name, extension, file_size, sample_rate, "
        "bit_depth, channels, total_frames, duration, format, folder_path, has_cover, embedded_cover_key) "
        "VALUES (?, ?, '.wav', 1024, 48000, 24, 2, 48000, 1.0, 'WAV', ?, 1, ?);";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db.db(), sql, -1, &stmt, nullptr) != SQLITE_OK) {
        sqlite3_exec(db.db(), "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }

    bool ok = true;
    for (int i = 0; i < count; ++i) {
        const std::string folder = "C:/large_cover_wall/" + std::to_string(i / 1000);
        const std::string fileName = prefix + "_" + std::to_string(i) + ".wav";
        const std::string filePath = folder + "/" + fileName;
        const std::string coverKey = "E|" + prefix + "_" + std::to_string(i);
        sqlite3_reset(stmt);
        sqlite3_clear_bindings(stmt);
        sqlite3_bind_text(stmt, 1, filePath.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, fileName.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, folder.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 4, coverKey.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(stmt) != SQLITE_DONE) {
            ok = false;
            break;
        }
    }
    sqlite3_finalize(stmt);

    if (ok) {
        ok = sqlite3_exec(db.db(), "COMMIT;", nullptr, nullptr, &err) == SQLITE_OK;
    } else {
        sqlite3_exec(db.db(), "ROLLBACK;", nullptr, nullptr, nullptr);
    }
    if (err) sqlite3_free(err);
    return ok;
}

static AudioFeatureVector makeTestFeatures(float base) {
    AudioFeatureVector vec;
    for (int i = 0; i < AudioFeatureVector::DIMS; ++i) {
        vec.features[i] = base + static_cast<float>(i) * 0.01f;
    }
    return vec;
}

static std::vector<CoverModule::CoverItem> buildCoverWallItemsForTest(const std::vector<AudioRecord>& records) {
    std::vector<CoverModule::CoverItem> outItems;
    std::unordered_map<std::string, size_t> keyToIdx;
    keyToIdx.reserve(records.size());

    for (const auto& r : records) {
        std::string folderKey = r.folderCoverKey;
        if (!folderKey.empty()) {
            folderKey = CoverModule::CanonicalCoverKey(folderKey);
        }
        if (folderKey.empty()) {
            continue;
        }

        auto [it, inserted] = keyToIdx.emplace(folderKey, outItems.size());
        if (inserted) {
            CoverModule::CoverItem ci;
            ci.kind = CoverModule::CoverKind::FolderCover;
            ci.key = folderKey;
            ci.folderPathUtf8 = r.folderPath;
            ci.folderCoverSrcPathUtf8 = r.folderCoverSrcPath;
            ci.hitCount = 1;
            ci.label = r.folderPath;
            outItems.push_back(std::move(ci));
        } else {
            outItems[it->second].hitCount++;
        }
    }

    return outItems;
}

// Returns number of failed assertions (0 = pass).
int testChannelCategoryFilter() {
    printSeparator("测试: 声道分类筛选 (ChannelCategory)");

    const std::string dbPath = "test_channel_filter.db";
    (void)std::remove(dbPath.c_str());

    Database db;
    if (!db.open(dbPath)) {
        std::cerr << "FAIL: open db: " << db.getLastError() << "\n";
        return 1;
    }

    auto insertRow = [&](const std::string& path, uint32_t ch) {
        AudioRecord r;
        r.filePath = path;
        r.fileName = path.substr(path.find_last_of("/\\") + 1);
        r.extension = ".wav";
        r.format = "WAV";
        r.channels = ch;
        r.sampleRate = 48000;
        r.bitDepth = 24;
        r.durationSeconds = 1.0f;
        r.fileSize = 1024;
        r.totalFrames = 48000;
        if (!db.addFile(r)) {
            std::cerr << "FAIL: addFile " << path << ": " << db.getLastError() << "\n";
        }
    };

    insertRow("C:/t/mono.wav",   1);
    insertRow("C:/t/stereo.wav", 2);
    insertRow("C:/t/three.wav",  3);
    insertRow("C:/t/quad.wav",   4);
    insertRow("C:/t/five1.wav",  6);
    insertRow("C:/t/seven1.wav", 8);
    insertRow("C:/t/ten.wav",    10);

    int failures = 0;
    auto checkCount = [&](const char* label, SearchFilter::ChannelCategory cc, int64_t expected) {
        SearchFilter f;
        f.channelCategory = cc;
        int64_t n = db.searchFilesCount(f);
        const bool ok = (n == expected);
        std::cout << (ok ? "PASS" : "FAIL") << ": " << label
                  << " expected=" << expected << " actual=" << n << "\n";
        if (!ok) ++failures;
    };

    checkCount("Any",          SearchFilter::ChannelCategory::Any,          7);
    checkCount("Mono",         SearchFilter::ChannelCategory::Mono,         1);
    checkCount("Stereo",       SearchFilter::ChannelCategory::Stereo,       1);
    checkCount("Quad",         SearchFilter::ChannelCategory::Quad,         1);
    checkCount("Surround_5_1", SearchFilter::ChannelCategory::Surround_5_1, 1);
    checkCount("Surround_7_1", SearchFilter::ChannelCategory::Surround_7_1, 1);
    checkCount("MultiOther",   SearchFilter::ChannelCategory::MultiOther,   2); // 3ch + 10ch

    // Legacy path still honored when category == Any
    {
        SearchFilter f;
        f.channels = 2;
        int64_t n = db.searchFilesCount(f);
        const bool ok = (n == 1);
        std::cout << (ok ? "PASS" : "FAIL") << ": Legacy channels=2 expected=1 actual=" << n << "\n";
        if (!ok) ++failures;
    }

    db.close();
    (void)std::remove(dbPath.c_str());
    return failures;
}

int testAudioDatabaseBoundaryDiscard() {
    printSeparator("Test: Audio database-boundary discard");

    int failures = 0;
    AudioEngine engine;
    engine.updateWaveformDataManually({0.25f, -0.50f, 0.10f});

    auto before = engine.getWaveformData();
    const bool beforeHasWaveform = before && before->isReady && !before->peaks.empty();
    std::cout << (beforeHasWaveform ? "PASS" : "FAIL")
              << ": fixture has visible waveform before boundary discard\n";
    if (!beforeHasWaveform) ++failures;

    engine.discardLoadedFileForDatabaseBoundary();

    auto after = engine.getWaveformData();
    const bool waveformCleared = after && !after->isReady && after->peaks.empty();
    const bool loadedPathCleared = engine.getLoadedFilePath().empty();
    const bool loadStateCleared = !engine.isLoaded() && !engine.isLoadFilePending();

    std::cout << (waveformCleared ? "PASS" : "FAIL")
              << ": database boundary clears visible main waveform\n";
    if (!waveformCleared) ++failures;

    std::cout << (loadedPathCleared ? "PASS" : "FAIL")
              << ": database boundary clears loaded file path\n";
    if (!loadedPathCleared) ++failures;

    std::cout << (loadStateCleared ? "PASS" : "FAIL")
              << ": database boundary clears loaded and pending state\n";
    if (!loadStateCleared) ++failures;

    return failures;
}

int testScanTimeCoverDedupFinalize() {
    printSeparator("Test: scan-time cover dedup finalize");

    const std::string dbPath = "test_cover_scan_finalize.db";
    removeDbFamily(dbPath);

    int failures = 0;
    Database db;
    if (!db.open(dbPath)) {
        std::cerr << "FAIL: open db: " << db.getLastError() << "\n";
        return 1;
    }

    auto addCoveredFile = [&](const std::string& path, const std::string& coverKey) {
        AudioRecord r;
        r.filePath = path;
        r.fileName = path.substr(path.find_last_of("/\\") + 1);
        r.extension = ".wav";
        r.format = "WAV";
        r.channels = 2;
        r.sampleRate = 48000;
        r.bitDepth = 24;
        r.durationSeconds = 1.0f;
        r.fileSize = 1024;
        r.totalFrames = 48000;
        r.folderPath = "C:/lib";
        r.hasCover = true;
        r.embeddedCoverKey = coverKey;
        return db.addFile(r);
    };

    const std::string keyA = "E|scan_finalize_a";
    const std::string keyB = "E|scan_finalize_b";
    if (!addCoveredFile("C:/lib/a.wav", keyA)) ++failures;
    if (!addCoveredFile("C:/lib/b.wav", keyB)) ++failures;

    std::vector<uint8_t> sameThumb(64);
    for (size_t i = 0; i < sameThumb.size(); ++i) sameThumb[i] = static_cast<uint8_t>(17 + i);

    if (!db.saveCoverThumb(keyA, sameThumb, "image/jpeg")) ++failures;
    if (!db.saveCoverThumb(keyB, sameThumb, "image/jpeg")) ++failures;

    std::vector<Database::CoverRegistryDelta> deltas = {
        {keyA, "C:/lib/a.wav", true},
        {keyB, "C:/lib/b.wav", true},
    };
    const int finalized = db.finalizeCoverRegistryDeltas(deltas);
    const bool finalizedBoth = finalized == 2;
    std::cout << (finalizedBoth ? "PASS" : "FAIL")
              << ": scan finalize upserts both changed cover keys; actual=" << finalized << "\n";
    if (!finalizedBoth) ++failures;

    auto entries = db.getGlobalCoverWallEntries();
    const bool dedupedWall = entries.size() == 1;
    std::cout << (dedupedWall ? "PASS" : "FAIL")
              << ": cover wall dedupes immediately after scan finalize; actual=" << entries.size() << "\n";
    if (!dedupedWall) ++failures;

    db.close();
    removeDbFamily(dbPath);
    return failures;
}

int testOpenAndCoverQueriesDoNotFullSyncRegistry() {
    printSeparator("Test: cover registry maintenance is deferred");

    const std::string dbPath = "test_cover_registry_deferred.db";
    removeDbFamily(dbPath);

    int failures = 0;
    {
        Database db;
        if (!db.open(dbPath)) {
            std::cerr << "FAIL: open db: " << db.getLastError() << "\n";
            return 1;
        }

        AudioRecord r;
        r.filePath = "C:/lib/deferred.wav";
        r.fileName = "deferred.wav";
        r.extension = ".wav";
        r.format = "WAV";
        r.channels = 2;
        r.sampleRate = 48000;
        r.bitDepth = 24;
        r.durationSeconds = 1.0f;
        r.fileSize = 1024;
        r.totalFrames = 48000;
        r.folderPath = "C:/lib";
        r.hasCover = true;
        r.embeddedCoverKey = "E|deferred_cover";
        if (!db.addFile(r)) ++failures;

        const bool registryEmptyAfterAdd = db.countCoverRegistryEntries() == 0;
        std::cout << (registryEmptyAfterAdd ? "PASS" : "FAIL")
                  << ": addFile does not implicitly full-sync cover registry\n";
        if (!registryEmptyAfterAdd) ++failures;
        db.close();
    }

    {
        Database reopened;
        if (!reopened.open(dbPath)) {
            std::cerr << "FAIL: reopen db: " << reopened.getLastError() << "\n";
            removeDbFamily(dbPath);
            return failures + 1;
        }

        const bool maintenanceMarked =
            queryAppMetaInt(reopened, "cover_registry_needs_maintenance") == 1;
        std::cout << (maintenanceMarked ? "PASS" : "FAIL")
                  << ": open marks cover registry maintenance without syncing\n";
        if (!maintenanceMarked) ++failures;

        const bool registryStillEmptyAfterOpen = reopened.countCoverRegistryEntries() == 0;
        std::cout << (registryStillEmptyAfterOpen ? "PASS" : "FAIL")
                  << ": open does not run syncCoverRegistry\n";
        if (!registryStillEmptyAfterOpen) ++failures;

        auto wall = reopened.getGlobalCoverWallEntries();
        const bool wallFallbackWorks = wall.size() == 1;
        std::cout << (wallFallbackWorks ? "PASS" : "FAIL")
                  << ": small-library cover wall fallback remains read-only; entries=" << wall.size() << "\n";
        if (!wallFallbackWorks) ++failures;

        const bool registryStillEmptyAfterWall = reopened.countCoverRegistryEntries() == 0;
        std::cout << (registryStillEmptyAfterWall ? "PASS" : "FAIL")
                  << ": opening cover wall does not full-sync registry\n";
        if (!registryStillEmptyAfterWall) ++failures;

        SearchFilter filter;
        (void)reopened.getCoverFacets(filter, 20);
        const bool registryStillEmptyAfterFacets = reopened.countCoverRegistryEntries() == 0;
        std::cout << (registryStillEmptyAfterFacets ? "PASS" : "FAIL")
                  << ": cover facet query does not full-sync registry\n";
        if (!registryStillEmptyAfterFacets) ++failures;

        reopened.close();
    }

    removeDbFamily(dbPath);
    return failures;
}

int testLargeCoverWallUsesIncrementalRegistryOnly() {
    printSeparator("Test: large cover wall avoids full-table dedup");

    const std::string dbPath = "test_large_cover_wall_incremental.db";
    removeDbFamily(dbPath);

    Database db;
    if (!db.open(dbPath)) {
        std::cerr << "FAIL: open db: " << db.getLastError() << "\n";
        return 1;
    }

    int failures = 0;
    constexpr int kRows = 50050;
    const bool inserted = insertCoveredRowsDirect(db, kRows, "large_cover");
    std::cout << (inserted ? "PASS" : "FAIL")
              << ": inserted large covered fixture rows\n";
    if (!inserted) {
        db.close();
        removeDbFamily(dbPath);
        return failures + 1;
    }

    const bool registryEmpty = db.countCoverRegistryEntries() == 0;
    std::cout << (registryEmpty ? "PASS" : "FAIL")
              << ": large fixture starts with empty incremental cover registry\n";
    if (!registryEmpty) ++failures;

    auto emptyWall = db.getGlobalCoverWallEntries();
    const bool largeFallbackBypassed = emptyWall.empty() && db.countCoverRegistryEntries() == 0;
    std::cout << (largeFallbackBypassed ? "PASS" : "FAIL")
              << ": large cover wall bypasses full audio_files GROUP BY fallback; entries="
              << emptyWall.size() << "\n";
    if (!largeFallbackBypassed) ++failures;

    std::vector<Database::CoverRegistryDelta> deltas = {
        {"E|large_cover_0", "C:/large_cover_wall/0/large_cover_0.wav", true},
    };
    const int finalized = db.finalizeCoverRegistryDeltas(deltas);
    const bool singleDeltaFinalized = finalized == 1 && db.countCoverRegistryEntries() == 1;
    std::cout << (singleDeltaFinalized ? "PASS" : "FAIL")
              << ": large cover wall accepts single incremental registry delta; finalized="
              << finalized << "\n";
    if (!singleDeltaFinalized) ++failures;

    auto incrementalWall = db.getGlobalCoverWallEntries();
    const bool readsIncrementalRegistryOnly = incrementalWall.size() == 1;
    std::cout << (readsIncrementalRegistryOnly ? "PASS" : "FAIL")
              << ": large cover wall reads only existing incremental registry entries; entries="
              << incrementalWall.size() << "\n";
    if (!readsIncrementalRegistryOnly) ++failures;

    db.close();
    removeDbFamily(dbPath);
    return failures;
}

int testGlobalCoverWallPageApiUsesStableCursor() {
    printSeparator("Test: global cover wall page API uses stable cursor on registry fast path");

    const std::string dbPath = "test_cover_wall_page_cursor.db";
    removeDbFamily(dbPath);

    Database db;
    if (!db.open(dbPath)) {
        std::cerr << "FAIL: open db: " << db.getLastError() << "\n";
        return 1;
    }

    int failures = 0;
    constexpr int kRows = 50050;
    const bool inserted = insertCoveredRowsDirect(db, kRows, "page_cover");
    std::cout << (inserted ? "PASS" : "FAIL")
              << ": inserted large page cursor fixture rows\n";
    if (!inserted) {
        db.close();
        removeDbFamily(dbPath);
        return failures + 1;
    }

    std::vector<Database::CoverRegistryDelta> deltas = {
        {"E|page_cover_0", "C:/large_cover_wall/0/page_cover_0.wav", true},
        {"E|page_cover_1", "C:/large_cover_wall/0/page_cover_1.wav", true},
        {"E|page_cover_2", "C:/large_cover_wall/0/page_cover_2.wav", true},
        {"E|page_cover_3", "C:/large_cover_wall/0/page_cover_3.wav", true},
        {"E|page_cover_4", "C:/large_cover_wall/0/page_cover_4.wav", true},
    };
    const int finalized = db.finalizeCoverRegistryDeltas(deltas);
    const bool deltasFinalized = finalized == (int)deltas.size();
    std::cout << (deltasFinalized ? "PASS" : "FAIL")
              << ": finalized registry page fixture deltas; finalized=" << finalized << "\n";
    if (!deltasFinalized) ++failures;

    sqlite3_exec(db.db(),
                 "INSERT OR REPLACE INTO app_meta (key, value) VALUES ('cover_registry_needs_maintenance', '0');",
                 nullptr,
                 nullptr,
                 nullptr);
    const bool markerCleared = queryAppMetaInt(db, "cover_registry_needs_maintenance") == 0;
    std::cout << (markerCleared ? "PASS" : "FAIL")
              << ": registry fast-path cursor test clears sparse maintenance marker\n";
    if (!markerCleared) ++failures;

    auto first = db.getGlobalCoverWallEntriesPage({}, 2);
    const bool firstPageOk =
        first.largeLibrary &&
        first.fromRegistry &&
        first.entries.size() == 2 &&
        first.hasMore &&
        !first.nextCursor.empty() &&
        first.entries[0].key == "E|page_cover_0" &&
        first.entries[1].key == "E|page_cover_1";
    std::cout << (firstPageOk ? "PASS" : "FAIL")
              << ": first large cover wall page is stable; rows=" << first.entries.size()
              << " more=" << first.hasMore << "\n";
    if (!firstPageOk) ++failures;

    auto second = db.getGlobalCoverWallEntriesPage(first.nextCursor, 2);
    const bool secondPageOk =
        second.largeLibrary &&
        second.fromRegistry &&
        second.entries.size() == 2 &&
        second.hasMore &&
        second.entries[0].key == "E|page_cover_2" &&
        second.entries[1].key == "E|page_cover_3";
    std::cout << (secondPageOk ? "PASS" : "FAIL")
              << ": second large cover wall page advances by cursor; rows=" << second.entries.size()
              << " more=" << second.hasMore << "\n";
    if (!secondPageOk) ++failures;

    auto third = db.getGlobalCoverWallEntriesPage(second.nextCursor, 2);
    const bool thirdPageOk =
        third.largeLibrary &&
        third.fromRegistry &&
        third.entries.size() == 1 &&
        !third.hasMore &&
        third.nextCursor.empty() &&
        third.entries[0].key == "E|page_cover_4";
    std::cout << (thirdPageOk ? "PASS" : "FAIL")
              << ": final large cover wall page terminates cleanly; rows=" << third.entries.size()
              << " more=" << third.hasMore << "\n";
    if (!thirdPageOk) ++failures;

    db.close();
    removeDbFamily(dbPath);
    return failures;
}

int testGlobalCoverWallPageRegistryUsesLiveRepresentativePath() {
    printSeparator("Test: global cover wall page registry fast path uses live representative path");

    const std::string dbPath = "test_cover_wall_page_live_rep_path.db";
    removeDbFamily(dbPath);

    Database db;
    if (!db.open(dbPath)) {
        std::cerr << "FAIL: open db: " << db.getLastError() << "\n";
        return 1;
    }

    int failures = 0;
    constexpr int kRows = 50050;
    const bool inserted = insertCoveredRowsDirect(db, kRows, "live_rep");
    std::cout << (inserted ? "PASS" : "FAIL")
              << ": inserted large live representative fixture rows\n";
    if (!inserted) {
        db.close();
        removeDbFamily(dbPath);
        return failures + 1;
    }

    std::vector<Database::CoverRegistryDelta> deltas = {
        {"E|live_rep_0", "C:/stale_registry/live_rep_0.wav", true},
    };
    const int finalized = db.finalizeCoverRegistryDeltas(deltas);
    const bool deltaFinalized = finalized == 1;
    std::cout << (deltaFinalized ? "PASS" : "FAIL")
              << ": finalized live representative fixture delta; finalized=" << finalized << "\n";
    if (!deltaFinalized) ++failures;

    const bool repPathOverridden = execSqlOnDbPath(
        dbPath,
        {
            "UPDATE cover_cache_registry SET rep_path = 'C:/stale_registry/live_rep_0.wav' "
            "WHERE cover_key = 'E|live_rep_0';",
            "INSERT OR REPLACE INTO app_meta (key, value) VALUES ('cover_registry_needs_maintenance', '0');"
        });
    std::cout << (repPathOverridden ? "PASS" : "FAIL")
              << ": forced stale registry representative path for live-path contract\n";
    if (!repPathOverridden) ++failures;

    auto first = db.getGlobalCoverWallEntriesPage({}, 1);
    const bool livePathReturned =
        first.largeLibrary &&
        first.fromRegistry &&
        first.entries.size() == 1 &&
        first.entries[0].key == "E|live_rep_0" &&
        first.entries[0].path == "C:/large_cover_wall/0/live_rep_0.wav";
    std::cout << (livePathReturned ? "PASS" : "FAIL")
              << ": registry fast path resolves live representative file path instead of stale registry path; path="
              << (first.entries.empty() ? std::string("<empty>") : first.entries[0].path) << "\n";
    if (!livePathReturned) ++failures;

    db.close();
    removeDbFamily(dbPath);
    return failures;
}

int testLargeCoverWallPageSupplementsSparseRegistryWithoutFullSync() {
    printSeparator("Test: large cover wall page supplements sparse registry without full sync");

    const std::string dbPath = "test_cover_wall_page_sparse_large_merge.db";
    removeDbFamily(dbPath);

    Database db;
    if (!db.open(dbPath)) {
        std::cerr << "FAIL: open db: " << db.getLastError() << "\n";
        return 1;
    }

    int failures = 0;
    constexpr int kRows = 50050;
    const bool inserted = insertCoveredRowsDirect(db, kRows, "page_sparse_merge");
    std::cout << (inserted ? "PASS" : "FAIL")
              << ": inserted large sparse page merge fixture rows\n";
    if (!inserted) {
        db.close();
        removeDbFamily(dbPath);
        return failures + 1;
    }

    std::vector<Database::CoverRegistryDelta> deltas = {
        {"E|page_sparse_merge_0", "C:/large_cover_wall/0/page_sparse_merge_0.wav", true},
    };
    const int finalized = db.finalizeCoverRegistryDeltas(deltas);
    const bool singleDeltaFinalized = finalized == 1 && db.countCoverRegistryEntries() == 1;
    std::cout << (singleDeltaFinalized ? "PASS" : "FAIL")
              << ": sparse large page merge fixture starts with one registry row; finalized="
              << finalized << "\n";
    if (!singleDeltaFinalized) ++failures;

    const int registryBeforeBrowse = db.countCoverRegistryEntries();
    auto first = db.getGlobalCoverWallEntriesPage({}, 8);
    const bool supplementedPageVisible =
        first.largeLibrary &&
        first.fromRegistry &&
        first.entries.size() == 8 &&
        first.hasMore &&
        !first.nextCursor.empty();
    std::cout << (supplementedPageVisible ? "PASS" : "FAIL")
              << ": sparse large global wall page stays bounded and non-empty beyond registry-only rows; rows="
              << first.entries.size() << " more=" << first.hasMore << "\n";
    if (!supplementedPageVisible) ++failures;

    bool includesMissingKeys = false;
    for (const auto& entry : first.entries) {
        if (entry.key != "E|page_sparse_merge_0") {
            includesMissingKeys = true;
            break;
        }
    }
    std::cout << (includesMissingKeys ? "PASS" : "FAIL")
              << ": sparse large global wall page supplements missing live cover keys beyond registry snapshot\n";
    if (!includesMissingKeys) {
        for (const auto& entry : first.entries) {
            std::cout << "  entry key=" << entry.key
                      << " path=" << entry.path
                      << " source=" << entry.sourcePath
                      << " embedded=" << (entry.isEmbedded ? 1 : 0) << "\n";
        }
        ++failures;
    }

    const bool registryUnchanged = db.countCoverRegistryEntries() == registryBeforeBrowse;
    std::cout << (registryUnchanged ? "PASS" : "FAIL")
              << ": sparse large global wall browse does not foreground-sync registry rows\n";
    if (!registryUnchanged) ++failures;

    db.close();
    removeDbFamily(dbPath);
    return failures;
}

int testSmallCoverWallPageApiFallbackPages() {
    printSeparator("Test: small cover wall page API fallback pages");

    const std::string dbPath = "test_cover_wall_page_small_fallback.db";
    removeDbFamily(dbPath);

    Database db;
    if (!db.open(dbPath)) {
        std::cerr << "FAIL: open db: " << db.getLastError() << "\n";
        return 1;
    }

    int failures = 0;
    const bool inserted = insertCoveredRowsDirect(db, 5, "small_page_cover");
    std::cout << (inserted ? "PASS" : "FAIL")
              << ": inserted small page fallback fixture rows\n";
    if (!inserted) {
        db.close();
        removeDbFamily(dbPath);
        return failures + 1;
    }

    auto first = db.getGlobalCoverWallEntriesPage({}, 2);
    const bool firstPageOk =
        !first.largeLibrary &&
        !first.fromRegistry &&
        first.entries.size() == 2 &&
        first.hasMore &&
        !first.nextCursor.empty();
    std::cout << (firstPageOk ? "PASS" : "FAIL")
              << ": first small fallback page is bounded; rows=" << first.entries.size()
              << " more=" << first.hasMore << "\n";
    if (!firstPageOk) ++failures;

    auto second = db.getGlobalCoverWallEntriesPage(first.nextCursor, 10);
    const bool secondPageOk =
        !second.largeLibrary &&
        !second.fromRegistry &&
        second.entries.size() == 3 &&
        !second.hasMore &&
        second.nextCursor.empty();
    std::cout << (secondPageOk ? "PASS" : "FAIL")
              << ": second small fallback page drains remaining covers; rows=" << second.entries.size()
              << " more=" << second.hasMore << "\n";
    if (!secondPageOk) ++failures;

    db.close();
    removeDbFamily(dbPath);
    return failures;
}

int testGlobalCoverWallPageFollowsCurrentFileListOrder() {
    printSeparator("Test: global cover wall page follows current file-list first-seen covers");

    const std::string dbPath = "test_cover_wall_page_file_list_order.db";
    removeDbFamily(dbPath);

    Database db;
    if (!db.open(dbPath)) {
        std::cerr << "FAIL: open db: " << db.getLastError() << "\n";
        return 1;
    }

    int failures = 0;
    struct CoverFile {
        const char* folder;
        const char* fileName;
        const char* coverKey;
    };
    const CoverFile files[] = {
        {"C:/lib/zebra", "2.wav", "E|cover_z"},
        {"C:/lib/apple", "3.wav", "E|cover_a2"},
        {"C:/lib/apple", "1.wav", "E|cover_a1"},
    };
    for (const auto& file : files) {
        const std::string folder = HarkenSearch::CanonicalFolderPath(file.folder);
        AudioRecord r;
        r.fileName = file.fileName;
        r.filePath = folder + "/" + file.fileName;
        r.folderPath = folder;
        r.extension = ".wav";
        r.format = "WAV";
        r.sampleRate = 48000;
        r.channels = 2;
        r.bitDepth = 24;
        r.durationSeconds = 1.0f;
        r.fileSize = 1024;
        r.totalFrames = 48000;
        r.hasCover = true;
        r.embeddedCoverKey = file.coverKey;
        if (!db.addFile(r)) ++failures;
    }

    SearchFilter fileListFilter;
    fileListFilter.sortAscending = true;
    const auto fileList = db.searchFiles(fileListFilter, 20, 0);
    const std::vector<std::string> filenameOrderNames = {"1.wav", "2.wav", "3.wav"};
    std::vector<std::string> actualFileNames;
    actualFileNames.reserve(fileList.size());
    for (const auto& row : fileList) actualFileNames.push_back(row.fileName);
    const bool fileListMixesFolders = actualFileNames == filenameOrderNames;
    std::cout << (fileListMixesFolders ? "PASS" : "FAIL")
              << ": current file list stays zebra between the two apple rows\n";
    if (!fileListMixesFolders) ++failures;

    std::vector<std::string> expectedCoverKeys;
    expectedCoverKeys.reserve(fileList.size());
    for (const auto& row : fileList) {
        if (row.embeddedCoverKey.empty()) continue;
        if (std::find(expectedCoverKeys.begin(), expectedCoverKeys.end(), row.embeddedCoverKey) ==
            expectedCoverKeys.end()) {
            expectedCoverKeys.push_back(row.embeddedCoverKey);
        }
    }
    const std::vector<std::string> folderRegrouped = {"E|cover_a1", "E|cover_a2", "E|cover_z"};
    const bool expectedFollowsList =
        expectedCoverKeys.size() == 3 &&
        expectedCoverKeys[0] == "E|cover_a1" &&
        expectedCoverKeys[1] == "E|cover_z" &&
        expectedCoverKeys[2] == "E|cover_a2" &&
        expectedCoverKeys != folderRegrouped;
    std::cout << (expectedFollowsList ? "PASS" : "FAIL")
              << ": first-seen covers from the current file list keep zebra between apple covers\n";
    if (!expectedFollowsList) ++failures;

    auto first = db.getGlobalCoverWallEntriesPage({}, 2);
    const bool firstPageFollowsList =
        !first.largeLibrary &&
        first.entries.size() == 2 &&
        first.hasMore &&
        expectedCoverKeys.size() >= 2 &&
        first.entries[0].key == expectedCoverKeys[0] &&
        first.entries[1].key == expectedCoverKeys[1];
    std::cout << (firstPageFollowsList ? "PASS" : "FAIL")
              << ": first full-library cover wall page follows file-list first-seen covers; keys="
              << (first.entries.size() > 0 ? first.entries[0].key : std::string("<empty>"))
              << ","
              << (first.entries.size() > 1 ? first.entries[1].key : std::string("<empty>"))
              << "\n";
    if (!firstPageFollowsList) ++failures;

    auto second = db.getGlobalCoverWallEntriesPage(first.nextCursor, 10);
    const bool secondPageFollowsList =
        !second.largeLibrary &&
        second.entries.size() == 1 &&
        !second.hasMore &&
        expectedCoverKeys.size() == 3 &&
        second.entries[0].key == expectedCoverKeys[2];
    std::cout << (secondPageFollowsList ? "PASS" : "FAIL")
              << ": remaining cover wall page continues file-list first-seen covers; key="
              << (second.entries.empty() ? std::string("<empty>") : second.entries[0].key)
              << "\n";
    if (!secondPageFollowsList) ++failures;

    auto all = db.getGlobalCoverWallEntriesPage({}, 10);
    std::vector<std::string> wallKeys;
    wallKeys.reserve(all.entries.size());
    for (const auto& entry : all.entries) wallKeys.push_back(entry.key);
    const bool fullPageFollowsList =
        wallKeys == expectedCoverKeys &&
        wallKeys != folderRegrouped;
    std::cout << (fullPageFollowsList ? "PASS" : "FAIL")
              << ": unpaged full-library cover wall matches file-list first-seen covers, not folder regrouping\n";
    if (!fullPageFollowsList) {
        std::cout << "  actual keys:";
        for (const auto& key : wallKeys) std::cout << " " << key;
        std::cout << "\n";
        ++failures;
    }

    db.close();
    removeDbFamily(dbPath);
    return failures;
}

int testFolderCoverFacetsKeepDistinctSimilarArtworks() {
    printSeparator("Test: folder cover wall keeps distinct similar artwork files");
    const std::string dbPath = "test_folder_cover_distinct_similar.db";
    removeDbFamily(dbPath);
    Database db;
    if (!db.open(dbPath)) return 1;

    const std::string firstKey = "F|H|0000000000000001|100";
    const std::string secondKey = "F|H|0000000000000002|100";
    auto add = [&](const std::string& name, const std::string& folder, const std::string& key) {
        AudioRecord record;
        record.filePath = folder + "/" + name;
        record.fileName = name;
        record.folderPath = HarkenSearch::CanonicalFolderPath(folder);
        record.extension = ".wav";
        record.format = "WAV";
        record.hasCover = true;
        record.folderCoverKey = key;
        return db.addFile(record);
    };
    const bool seeded =
        add("one.wav", "C:/library/media/Album_01", firstKey) &&
        add("two.wav", "C:/library/media/Album_01", firstKey) &&
        add("three.wav", "C:/library/media/Album_02", secondKey) &&
        sqlite3_exec(db.db(), R"SQL(
            INSERT OR REPLACE INTO cover_cache_registry
                (cover_key, rep_path, is_embedded, normalized_key, cover_phash)
            VALUES
                ('F|H|0000000000000001|100', 'c:\library\media\album_01', 0,
                 'F|H|0000000000000001|100', 12345),
                ('F|H|0000000000000002|100', 'c:\library\media\album_02', 0,
                 'F|H|0000000000000002|100', 12345);
        )SQL", nullptr, nullptr, nullptr) == SQLITE_OK;
    SearchFilter parent;
    parent.folderPath = HarkenSearch::CanonicalFolderPath("C:/library/media");
    SearchFilter album;
    album.folderPath = HarkenSearch::CanonicalFolderPath("C:/library/media/Album_01");
    const auto parentFiles = db.searchFiles(parent, 10, 0);
    const auto parentCovers = db.getCoverFacets(parent, 10);
    const auto cachedParentCovers = db.getCoverFacets(parent, 10);
    const auto albumFiles = db.searchFiles(album, 10, 0);
    const auto albumCovers = db.getCoverFacets(album, 10);
    const bool ok = seeded && parentFiles.size() == 3 && parentCovers.size() == 2 &&
                    cachedParentCovers.size() == 2 &&
                    albumFiles.size() == 2 && albumCovers.size() == 1;
    std::cout << (ok ? "PASS" : "FAIL")
              << ": parent files=" << parentFiles.size()
              << " covers=" << parentCovers.size()
              << "; album files=" << albumFiles.size()
              << " covers=" << albumCovers.size() << "\n";
    db.close();
    removeDbFamily(dbPath);
    return ok ? 0 : 1;
}

int testLargeCoverWallPageFollowsCurrentFileListPastFiftyThousandRows() {
    printSeparator("Test: large cover wall follows file list and continues after 50000 rows");

    const std::string dbPath = "test_cover_wall_large_file_list_order.db";
    removeDbFamily(dbPath);
    Database db;
    if (!db.open(dbPath)) return 1;
    int failures = 0;
    if (!insertCoveredRowsDirect(db, 50001, "zz_bulk")) {
        db.close();
        removeDbFamily(dbPath);
        return 1;
    }
    const auto denseStart = std::chrono::steady_clock::now();
    const auto densePage = db.getGlobalCoverWallEntriesPage({}, 512, true);
    const auto denseElapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - denseStart).count();
    const bool denseOk = densePage.largeLibrary && densePage.entries.size() == 512 &&
        denseElapsed < 5000;
    std::cout << (denseOk ? "PASS" : "FAIL")
              << ": dense first cover page loads promptly; rows="
              << densePage.entries.size() << " elapsed_ms=" << denseElapsed
              << " error=" << db.getLastError() << "\n";
    if (!denseOk) ++failures;
    if (sqlite3_exec(db.db(),
            "UPDATE audio_files SET embedded_cover_key='E|shared' WHERE id <= 49000;",
            nullptr, nullptr, nullptr) != SQLITE_OK) {
        ++failures;
    } else {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        sqlite3_progress_handler(db.db(), 10000, [](void* context) -> int {
            const auto* end = static_cast<const std::chrono::steady_clock::time_point*>(context);
            return std::chrono::steady_clock::now() >= *end ? 1 : 0;
        }, &deadline);
        const auto repeatedStart = std::chrono::steady_clock::now();
        const auto repeatedPage = db.getGlobalCoverWallEntriesPage({}, 2, true);
        const auto repeatedElapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - repeatedStart).count();
        sqlite3_progress_handler(db.db(), 0, nullptr, nullptr);
        const bool repeatedOk = repeatedPage.entries.size() == 2 && repeatedElapsed < 5000;
        std::cout << (repeatedOk ? "PASS" : "FAIL")
                  << ": repeated cover keys do not stall global page; rows="
                  << repeatedPage.entries.size() << " elapsed_ms=" << repeatedElapsed << "\n";
        if (!repeatedOk) ++failures;
    }
    const char* fixtureSql =
        "UPDATE audio_files SET has_cover=0, embedded_cover_key='' "
        "WHERE file_name NOT IN ('zz_bulk_0.wav', 'zz_bulk_1.wav', "
        "                        'zz_bulk_2.wav', 'zz_bulk_49999.wav', "
        "                        'zz_bulk_50000.wav');"
        "UPDATE audio_files SET file_name='0001.wav', embedded_cover_key='E|cover_z' "
        "WHERE file_name='zz_bulk_0.wav';"
        "UPDATE audio_files SET file_name='0002.wav', embedded_cover_key='E|cover_a' "
        "WHERE file_name='zz_bulk_1.wav';"
        "UPDATE audio_files SET file_name='0003.wav', embedded_cover_key='E|cover_m' "
        "WHERE file_name='zz_bulk_2.wav';"
        "UPDATE audio_files SET file_name='zzzy.wav', embedded_cover_key='E|cover_z' "
        "WHERE file_name='zz_bulk_49999.wav';"
        "UPDATE audio_files SET file_name='zzzz.wav', embedded_cover_key='E|cover_late' "
        "WHERE file_name='zz_bulk_50000.wav';"
        "INSERT INTO cover_cache_registry "
        "(cover_key, rep_path, is_embedded, normalized_key, cover_phash) "
        "SELECT 'E|cover_late', file_path, 1, 'E|visual_late', 123 "
        "FROM audio_files WHERE file_name='zzzz.wav';";
    if (sqlite3_exec(db.db(), fixtureSql, nullptr, nullptr, nullptr) != SQLITE_OK) {
        db.close();
        removeDbFamily(dbPath);
        return 1;
    }

    const auto first = db.getGlobalCoverWallEntriesPage({}, 2, true);
    const bool firstOk = first.largeLibrary && first.entries.size() == 2 &&
        first.entries[0].key == "E|cover_z" && first.entries[1].key == "E|cover_a" &&
        first.hasMore && !first.nextCursor.empty();
    std::cout << (firstOk ? "PASS" : "FAIL")
              << ": large first page follows list order; first="
              << (first.entries.empty() ? "<empty>" : first.entries[0].key)
              << " error=" << db.getLastError() << "\n";
    if (!firstOk) ++failures;

    if (!first.nextCursor.empty()) {
        const auto second = db.getGlobalCoverWallEntriesPage(first.nextCursor, 2, true);
        const bool secondOk = second.entries.size() == 2 &&
            second.entries[0].key == "E|cover_m" &&
            second.entries[1].key == "E|visual_late" &&
            second.entries[1].sourcePath == "PHASH|123" && !second.hasMore;
        std::cout << (secondOk ? "PASS" : "FAIL")
                  << ": next page reaches cover after 50000 files; rows="
                  << second.entries.size() << "\n";
        if (!secondOk) ++failures;
    }
    const std::string visualSelection =
        aura::cover_wall::EncodeExactCoverWallKey("E|visual_late");
    const auto aliasIds = db.getFileIdsForCoverTileMembership(visualSelection);
    SearchFilter aliasFilter;
    aliasFilter.specificCoverKey = visualSelection;
    const auto aliasCount = db.searchFilesCount(aliasFilter);
    const bool aliasOk = aliasIds.size() == 1 && aliasCount == 1;
    std::cout << (aliasOk ? "PASS" : "FAIL")
              << ": normalized visual key resolves its stored raw membership; ids="
              << aliasIds.size() << " count=" << aliasCount << "\n";
    if (!aliasOk) ++failures;
    db.close();
    removeDbFamily(dbPath);
    return failures;
}

int testSmallCoverWallSameFilenamePageCursorDoesNotSkipOrRepeat() {
    printSeparator("Test: small cover wall same-filename page cursor does not skip or repeat");

    const std::string dbPath = "test_cover_wall_page_same_filename_cursor.db";
    removeDbFamily(dbPath);

    Database db;
    if (!db.open(dbPath)) {
        std::cerr << "FAIL: open db: " << db.getLastError() << "\n";
        return 1;
    }

    int failures = 0;
    struct CoverFile {
        const char* folder;
        const char* fileName;
        const char* coverKey;
    };
    const CoverFile files[] = {
        {"C:/dup/z-folder", "same.wav", "E|cover_same_z"},
        {"C:/dup/a-folder", "same.wav", "E|cover_same_a"},
    };

    auto addCoverFile = [&](const CoverFile& file, int64_t fileSize, int64_t totalFrames) {
        const std::string folder = HarkenSearch::CanonicalFolderPath(file.folder);
        AudioRecord r;
        r.fileName = file.fileName;
        r.filePath = folder + "/" + file.fileName;
        r.folderPath = folder;
        r.extension = ".wav";
        r.format = "WAV";
        r.sampleRate = 48000;
        r.channels = 2;
        r.bitDepth = 24;
        r.durationSeconds = 1.0f;
        r.fileSize = fileSize;
        r.totalFrames = totalFrames;
        r.hasCover = true;
        r.embeddedCoverKey = file.coverKey;
        return db.addFile(r);
    };

    if (!addCoverFile(files[0], 1024, 48000)) ++failures;
    if (!addCoverFile(files[1], 2048, 96000)) ++failures;

    SearchFilter fileListFilter;
    fileListFilter.sortColumn = 0;
    fileListFilter.sortAscending = true;
    const auto listed = db.searchFiles(fileListFilter, 20, 0);
    const bool twoSameNamedFiles =
        listed.size() == 2 &&
        listed[0].fileName == "same.wav" &&
        listed[1].fileName == "same.wav" &&
        listed[0].embeddedCoverKey != listed[1].embeddedCoverKey;
    std::cout << (twoSameNamedFiles ? "PASS" : "FAIL")
              << ": fixture keeps two same-named files with different cover keys; rows="
              << listed.size() << "\n";
    if (!twoSameNamedFiles) ++failures;

    const std::string expectedKeys[] = {"E|cover_same_z", "E|cover_same_a"};
    auto pageAllCovers = [&](const std::string& label, bool reverseInsertOrderBetweenPages) {
        std::vector<std::string> pagedKeys;
        std::string cursor;
        bool sawKeyOnlyCursor = false;
        for (int pageIndex = 0; pageIndex < 8; ++pageIndex) {
            auto page = db.getGlobalCoverWallEntriesPage(cursor, 1);
            const bool pageShapeOk =
                !page.largeLibrary &&
                !page.fromRegistry &&
                page.entries.size() <= 1;
            std::cout << (pageShapeOk ? "PASS" : "FAIL")
                      << ": " << label << " page " << pageIndex
                      << " stays on the small file-list cover wall; rows=" << page.entries.size()
                      << " more=" << page.hasMore << "\n";
            if (!pageShapeOk) ++failures;
            if (page.entries.empty()) break;
            if (page.nextCursor == page.entries.back().key) {
                sawKeyOnlyCursor = true;
            }
            pagedKeys.push_back(page.entries[0].key);
            if (!page.hasMore) break;
            if (reverseInsertOrderBetweenPages && pageIndex == 0) {
                for (const auto& file : files) {
                    const std::string folder = HarkenSearch::CanonicalFolderPath(file.folder);
                    (void)db.removeFileByPath(folder + "/" + file.fileName);
                }
                if (!addCoverFile(files[1], 2048, 96000) || !addCoverFile(files[0], 4096, 192000)) ++failures;
            }
            cursor = page.nextCursor;
            if (cursor.empty()) break;
        }

        std::vector<std::string> uniqueKeys = pagedKeys;
        std::sort(uniqueKeys.begin(), uniqueKeys.end());
        uniqueKeys.erase(std::unique(uniqueKeys.begin(), uniqueKeys.end()), uniqueKeys.end());
        const bool noRepeat = uniqueKeys.size() == pagedKeys.size();
        const bool sawBoth =
            std::find(pagedKeys.begin(), pagedKeys.end(), expectedKeys[0]) != pagedKeys.end() &&
            std::find(pagedKeys.begin(), pagedKeys.end(), expectedKeys[1]) != pagedKeys.end();
        const bool cursorNotKeyOnly = !sawKeyOnlyCursor;
        std::cout << (noRepeat && sawBoth && cursorNotKeyOnly ? "PASS" : "FAIL")
                  << ": " << label
                  << " limit=1 walk visits each same-filename cover once with a non-key-only cursor; pages="
                  << pagedKeys.size() << " unique=" << uniqueKeys.size()
                  << " keyOnly=" << (sawKeyOnlyCursor ? 1 : 0) << "\n";
        if (!(noRepeat && sawBoth && cursorNotKeyOnly)) {
            std::cout << "  actual keys:";
            for (const auto& key : pagedKeys) std::cout << " " << key;
            std::cout << "\n";
            ++failures;
        }
    };

    pageAllCovers("stable insert order", false);
    pageAllCovers("reversed insert order between pages", true);

    db.close();
    removeDbFamily(dbPath);
    return failures;
}

int testLargeCoverFacetsUseIncrementalRegistryOnly() {
    printSeparator("Test: large cover facets avoid full-table sparse fallback");

    const std::string dbPath = "test_large_cover_facets_incremental.db";
    removeDbFamily(dbPath);

    Database db;
    if (!db.open(dbPath)) {
        std::cerr << "FAIL: open db: " << db.getLastError() << "\n";
        return 1;
    }

    int failures = 0;
    constexpr int kRows = 50050;
    const bool inserted = insertCoveredRowsDirect(db, kRows, "large_facet");
    std::cout << (inserted ? "PASS" : "FAIL")
              << ": inserted large facet fixture rows\n";
    if (!inserted) {
        db.close();
        removeDbFamily(dbPath);
        return failures + 1;
    }

    SearchFilter globalFilter;
    auto emptyFacets = db.getCoverFacets(globalFilter, 2000);
    const bool sparseBypassReturnsRegistryOnly = emptyFacets.empty() && db.countCoverRegistryEntries() == 0;
    std::cout << (sparseBypassReturnsRegistryOnly ? "PASS" : "FAIL")
              << ": large sparse global facets read only incremental registry; entries="
              << emptyFacets.size() << "\n";
    if (!sparseBypassReturnsRegistryOnly) ++failures;

    std::vector<Database::CoverRegistryDelta> deltas = {
        {"E|large_facet_0", "C:/large_cover_wall/0/large_facet_0.wav", true},
    };
    const int finalized = db.finalizeCoverRegistryDeltas(deltas);
    const bool singleDeltaFinalized = finalized == 1 && db.countCoverRegistryEntries() == 1;
    std::cout << (singleDeltaFinalized ? "PASS" : "FAIL")
              << ": large sparse global facets accept single incremental registry delta; finalized="
              << finalized << "\n";
    if (!singleDeltaFinalized) ++failures;

    auto incrementalFacets = db.getCoverFacets(globalFilter, 2000);
    const bool readsIncrementalFacetRegistryOnly = incrementalFacets.size() == 1;
    std::cout << (readsIncrementalFacetRegistryOnly ? "PASS" : "FAIL")
              << ": large sparse global facets return only current registry entries; entries="
              << incrementalFacets.size() << "\n";
    if (!readsIncrementalFacetRegistryOnly) ++failures;

    db.close();
    removeDbFamily(dbPath);
    return failures;
}

int testLargeCoverBrowseHotPathsUseBoundedRegistryHeuristic() {
    printSeparator("Test: large cover browse hot paths stay on bounded registry heuristic");

    int failures = 0;
    const std::string source = readFirstExistingTextFile({
        "src/Database.cpp",
        "../src/Database.cpp",
        "../../src/Database.cpp",
        "E:/UGit/Aura/Aura-Auro_Dev/src/Database.cpp"
    });
    const bool readable = !source.empty();
    std::cout << (readable ? "PASS" : "FAIL")
              << ": Database.cpp source is readable for cover boundedness contract\n";
    if (!readable) return 1;

    const std::string readinessBody = extractFunctionBody(
        source,
        "static GlobalCoverAggregationReadiness AssessGlobalCoverAggregationReadiness(sqlite3* db)");
    const size_t readinessHelper = readinessBody.find("IsLargeLibraryCoverRegistryBoundedSparse");
    const size_t readinessRaw = readinessBody.find("CountAudioCoverPresenceCandidates");
    const bool readinessKeepsBoundedOrder =
        !readinessBody.empty() &&
        readinessHelper != std::string::npos &&
        readinessRaw != std::string::npos &&
        readinessHelper < readinessRaw;
    std::cout << (readinessKeepsBoundedOrder ? "PASS" : "FAIL")
              << ": shared global cover aggregation readiness helper checks bounded sparse state before raw cover recount\n";
    if (!readinessKeepsBoundedOrder) ++failures;

    const std::string wallBody = extractFunctionBody(
        source,
        "std::vector<Database::CoverWallEntry> Database::getGlobalCoverWallEntries()");
    const bool wallUsesBoundedHelper =
        !wallBody.empty() &&
        wallBody.find("AssessGlobalCoverAggregationReadiness") != std::string::npos &&
        wallBody.find("IsLargeLibraryCoverRegistryBoundedSparse") == std::string::npos &&
        wallBody.find("CountAudioCoverPresenceCandidates") == std::string::npos;
    std::cout << (wallUsesBoundedHelper ? "PASS" : "FAIL")
              << ": large global cover wall routes bounded sparse decisions through shared readiness helper\n";
    if (!wallUsesBoundedHelper) ++failures;

    const std::string pagedWallBody = extractFunctionBody(
        source,
        "Database::CoverWallPage Database::getGlobalCoverWallEntriesPage(");
    const bool pagedWallUsesBoundedHelper =
        !pagedWallBody.empty() &&
        pagedWallBody.find("AssessGlobalCoverAggregationReadiness") != std::string::npos &&
        pagedWallBody.find("IsLargeLibraryCoverRegistryBoundedSparse") == std::string::npos &&
        pagedWallBody.find("CountAudioCoverPresenceCandidates") == std::string::npos;
    std::cout << (pagedWallUsesBoundedHelper ? "PASS" : "FAIL")
              << ": paged global cover wall routes bounded sparse decisions through shared readiness helper\n";
    if (!pagedWallUsesBoundedHelper) ++failures;

    const std::string facetBody = extractFunctionBody(
        source,
        "std::vector<Database::CoverFacetEntry> Database::getCoverFacets(const SearchFilter& filter, int limit)");
    const bool facetUsesBoundedHelper =
        !facetBody.empty() &&
        facetBody.find("AssessGlobalCoverAggregationReadiness") != std::string::npos &&
        facetBody.find("IsLargeLibraryCoverRegistryBoundedSparse") == std::string::npos &&
        facetBody.find("CountAudioCoverPresenceCandidates") == std::string::npos;
    std::cout << (facetUsesBoundedHelper ? "PASS" : "FAIL")
              << ": large global cover facets route bounded sparse decisions through shared readiness helper\n";
    if (!facetUsesBoundedHelper) ++failures;

    return failures;
}

int testSearchFilesCountMirrorsFtsFallbackContract() {
    printSeparator("Test: searchFilesCount mirrors FTS fallback contract");

    int failures = 0;
    const std::string source = readFirstExistingTextFile({
        "src/Database.cpp",
        "../src/Database.cpp",
        "../../src/Database.cpp",
        "E:/UGit/Aura/Aura-Auro_Dev/src/Database.cpp"
    });
    const bool readable = !source.empty();
    std::cout << (readable ? "PASS" : "FAIL")
              << ": Database.cpp source is readable for count fallback contract\n";
    if (!readable) return 1;

    const std::string countBody = extractFunctionBody(
        source,
        "int64_t Database::searchFilesCount(const SearchFilter& filter)");
    const bool hasFtsFallback =
        countBody.find("if (bq.hasFtsJoin && !ftsHasRows(m_db)) {") != std::string::npos &&
        countBody.find("bq.whereClause = \"WHERE af.file_name LIKE ? COLLATE NOCASE\";") != std::string::npos &&
        countBody.find("bq.bindValues.push_back(tok + \"%\");") != std::string::npos;
    std::cout << (hasFtsFallback ? "PASS" : "FAIL")
              << ": searchFilesCount mirrors searchFiles keyword fallback when FTS rows are unavailable\n";
    if (!hasFtsFallback) ++failures;

    return failures;
}

int testSearchFilesCountDoesNotCacheZeroFilteredCountsContract() {
    printSeparator("Test: searchFilesCount does not cache zero filtered counts");

    int failures = 0;
    const std::string source = readFirstExistingTextFile({
        "src/Database.cpp",
        "../src/Database.cpp",
        "../../src/Database.cpp",
        "E:/UGit/Aura/Aura-Auro_Dev/src/Database.cpp"
    });
    const bool readable = !source.empty();
    std::cout << (readable ? "PASS" : "FAIL")
              << ": Database.cpp source is readable for zero-count cache contract\n";
    if (!readable) return 1;

    const std::string countBody = extractFunctionBody(
        source,
        "int64_t Database::searchFilesCount(const SearchFilter& filter)");
    const bool skipsZeroCountCacheForFilteredQueries =
        countBody.find("const bool allowZeroCountCache =\n        !aura::cover_replacement::HasPersistentFacetFilterSemantics(filter);") != std::string::npos &&
        countBody.find("if (it->second.count > 0 || allowZeroCountCache) {") != std::string::npos &&
        countBody.find("if (cachedCount > 0 || allowZeroCountCache) {") != std::string::npos &&
        countBody.find("if (count > 0 || allowZeroCountCache) {") != std::string::npos;
    std::cout << (skipsZeroCountCacheForFilteredQueries ? "PASS" : "FAIL")
              << ": filtered zero counts are not reusable cache truth for searchFilesCount\n";
    if (!skipsZeroCountCacheForFilteredQueries) ++failures;

    return failures;
}

int testLargeCoverRegistryRepairBatchProgressivelyFillsSparseRegistry() {
    printSeparator("Test: large sparse registry repair progresses without full sync");

    const std::string dbPath = "test_large_cover_registry_repair.db";
    removeDbFamily(dbPath);

    Database db;
    if (!db.open(dbPath)) {
        std::cerr << "FAIL: open db: " << db.getLastError() << "\n";
        return 1;
    }

    int failures = 0;
    constexpr int kRows = 50050;
    const bool inserted = insertCoveredRowsDirect(db, kRows, "repair_cover");
    std::cout << (inserted ? "PASS" : "FAIL")
              << ": inserted large sparse registry repair fixture rows\n";
    if (!inserted) {
        db.close();
        removeDbFamily(dbPath);
        return failures + 1;
    }

    int totalFinalized = 0;
    int loops = 0;
    while (db.isCoverRegistryMaintenancePending() && loops < 64) {
        const int repaired = db.repairSparseCoverRegistryBatch(2048);
        if (repaired <= 0) break;
        totalFinalized += repaired;
        ++loops;
    }

    const bool registryFullyRecovered =
        !db.isCoverRegistryMaintenancePending() &&
        db.countCoverRegistryEntries() == kRows &&
        queryAppMetaInt(db, "cover_registry_needs_maintenance") == 0;
    std::cout << (registryFullyRecovered ? "PASS" : "FAIL")
              << ": incremental registry repair fills legacy sparse large library; finalized="
              << totalFinalized << " loops=" << loops
              << " registry=" << db.countCoverRegistryEntries() << "\n";
    if (!registryFullyRecovered) ++failures;

    db.close();
    removeDbFamily(dbPath);
    return failures;
}

int testSparseLargeRegistryReopenRearmsMaintenanceMarker() {
    printSeparator("Test: sparse large registry reopen re-arms maintenance marker");

    const std::string dbPath = "test_large_cover_registry_rearm.db";
    removeDbFamily(dbPath);

    {
        Database db;
        if (!db.open(dbPath)) {
            std::cerr << "FAIL: open db: " << db.getLastError() << "\n";
            return 1;
        }

        constexpr int kRows = 50050;
        const bool inserted = insertCoveredRowsDirect(db, kRows, "rearm_cover");
        std::cout << (inserted ? "PASS" : "FAIL")
                  << ": inserted sparse large registry rearm fixture rows\n";
        if (!inserted) {
            db.close();
            removeDbFamily(dbPath);
            return 1;
        }
        db.close();
    }

    {
        sqlite3* rawDb = nullptr;
        if (sqlite3_open(dbPath.c_str(), &rawDb) != SQLITE_OK) {
            std::cerr << "FAIL: reopen raw sqlite to force stale marker\n";
            if (rawDb) sqlite3_close(rawDb);
            removeDbFamily(dbPath);
            return 1;
        }
        sqlite3_exec(rawDb,
                     "INSERT OR REPLACE INTO app_meta (key, value) VALUES ('cover_registry_needs_maintenance', '0');",
                     nullptr,
                     nullptr,
                     nullptr);
        sqlite3_close(rawDb);
    }

    int failures = 0;
    {
        Database reopened;
        if (!reopened.open(dbPath, true)) {
            std::cerr << "FAIL: reopen db: " << reopened.getLastError() << "\n";
            removeDbFamily(dbPath);
            return 1;
        }

        const bool markerRearmed =
            queryAppMetaInt(reopened, "cover_registry_needs_maintenance") == 1;
        std::cout << (markerRearmed ? "PASS" : "FAIL")
                  << ": reopen re-arms cover registry maintenance for sparse large library\n";
        if (!markerRearmed) ++failures;

        const bool maintenanceStillPending = reopened.isCoverRegistryMaintenancePending();
        std::cout << (maintenanceStillPending ? "PASS" : "FAIL")
                  << ": sparse large registry still reports maintenance pending after reopen\n";
        if (!maintenanceStillPending) ++failures;

        reopened.close();
    }

    removeDbFamily(dbPath);
    return failures;
}

int testSparseLargeRegistryPreheatRearmsPartialMarker() {
    printSeparator("Test: sparse large registry preheat re-arms partial marker");

    const std::string dbPath = "test_large_cover_registry_preheat_rearm.db";
    removeDbFamily(dbPath);

    Database db;
    if (!db.open(dbPath, true)) {
        std::cerr << "FAIL: open db: " << db.getLastError() << "\n";
        return 1;
    }

    int failures = 0;
    constexpr int kRows = 50050;
    const bool inserted = insertCoveredRowsDirect(db, kRows, "preheat_rearm_cover");
    std::cout << (inserted ? "PASS" : "FAIL")
              << ": inserted partial sparse preheat fixture rows\n";
    if (!inserted) {
        db.close();
        removeDbFamily(dbPath);
        return failures + 1;
    }

    std::vector<Database::CoverRegistryDelta> deltas = {
        {"E|preheat_rearm_cover_0", "C:/large_cover_wall/0/preheat_rearm_cover_0.wav", true},
    };
    const int finalized = db.finalizeCoverRegistryDeltas(deltas);
    const bool singleDeltaFinalized = finalized == 1 && db.countCoverRegistryEntries() == 1;
    std::cout << (singleDeltaFinalized ? "PASS" : "FAIL")
              << ": partial sparse registry starts non-empty; finalized=" << finalized << "\n";
    if (!singleDeltaFinalized) ++failures;

    sqlite3_exec(db.db(),
                 "INSERT OR REPLACE INTO app_meta (key, value) VALUES ('cover_registry_needs_maintenance', '0');",
                 nullptr,
                 nullptr,
                 nullptr);
    const bool markerCleared = queryAppMetaInt(db, "cover_registry_needs_maintenance") == 0;
    std::cout << (markerCleared ? "PASS" : "FAIL")
              << ": forced partial sparse maintenance marker to 0 before preheat\n";
    if (!markerCleared) ++failures;

    db.preheat();
    const bool markerRearmed = queryAppMetaInt(db, "cover_registry_needs_maintenance") == 1;
    std::cout << (markerRearmed ? "PASS" : "FAIL")
              << ": preheat re-arms partial sparse large registry marker\n";
    if (!markerRearmed) ++failures;

    db.close();
    removeDbFamily(dbPath);
    return failures;
}

int testCoverThumbMaintenanceRepairsMissingThumbsEvenWhenPhashVersionCurrent() {
    printSeparator("Test: deferred cover thumb maintenance repairs missing rows even when phash version is already current");

    const fs::path root = makeTempRoot("cover_indexing_thumb_maintenance");
    const fs::path coverFolder = root / "Pack";
    const fs::path folder = coverFolder / "Sub";
    const fs::path audioPath = folder / "whoosh.wav";
    const fs::path coverPath = coverFolder / "cover.jpg";
    const std::string dbPath = (root / "cover_indexing_thumb_maintenance.db").u8string();
    writeSilentMonoWav(audioPath);
    writeTinyCoverImage(coverPath);

    Database db;
    if (!db.open(dbPath)) {
        std::cerr << "FAIL: open db: " << db.getLastError() << "\n";
        std::error_code ec;
        fs::remove_all(root, ec);
        return 1;
    }

    int failures = 0;
    AudioRecord r;
    r.filePath = audioPath.u8string();
    r.fileName = "whoosh.wav";
    r.extension = ".wav";
    r.format = "WAV";
    r.channels = 1;
    r.sampleRate = 8000;
    r.bitDepth = 16;
    r.durationSeconds = 1.0f;
    r.fileSize = 512;
    r.totalFrames = 128;
    r.folderPath = folder.u8string();
    r.hasCover = true;
    r.folderCoverSrcPath = coverPath.u8string();
    r.folderCoverKey =
        CoverModule::CanonicalCoverKey(std::string("F|") + coverPath.u8string());
    if (!db.addFile(r)) {
        std::cerr << "FAIL: add file fixture\n";
        db.close();
        removeDbFamily(dbPath);
        std::error_code ec;
        fs::remove_all(root, ec);
        return 1;
    }

    db.syncCoverRegistry();
    const AudioRecord seeded = db.getFileByPath(audioPath.u8string());
    const std::string activeCoverKey = seeded.folderCoverKey;
    std::cout << (!activeCoverKey.empty() ? "PASS" : "FAIL")
              << ": syncCoverRegistry leaves an active folder cover key for thumb maintenance\n";
    if (activeCoverKey.empty()) ++failures;
    const std::string blobsPath = db.getBlobsDbPathPublic(dbPath);
    const std::string escapedBlobsPath = escapeSqlLiteral(blobsPath);
    const bool fixturePrimed = execSqlOnDbPath(dbPath, {
        "ATTACH DATABASE '" + escapedBlobsPath + "' AS blobs_db;",
        "DELETE FROM blobs_db.cover_thumbs;",
        "UPDATE cover_cache_registry SET cover_phash = 0, cover_phash_version = 2;",
        "INSERT OR REPLACE INTO app_meta (key, value) VALUES ('cover_registry_needs_maintenance', '1');",
        "DETACH DATABASE blobs_db;"
    });
    std::cout << (fixturePrimed ? "PASS" : "FAIL")
              << ": primed missing-thumb maintenance fixture with marker=1 and phash_version=2\n";
    if (!fixturePrimed) ++failures;

    db.startCoverPhashBackfillWorker();
    for (int i = 0; i < 40 && !db.hasCoverThumb(activeCoverKey); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    db.stopCoverPhashBackfillWorker();

    const bool thumbRepaired = db.hasCoverThumb(activeCoverKey);
    std::cout << (thumbRepaired ? "PASS" : "FAIL")
              << ": deferred maintenance repairs missing thumb rows even when phash version is already 2\n";
    if (!thumbRepaired) ++failures;

    const bool markerCleared = queryAppMetaInt(db, "cover_registry_needs_maintenance") == 0;
    std::cout << (markerCleared ? "PASS" : "FAIL")
              << ": deferred maintenance clears cover_registry_needs_maintenance once thumb repair settles\n";
    if (!markerCleared) ++failures;

    db.close();
    removeDbFamily(dbPath);
    std::error_code ec;
    fs::remove_all(root, ec);
    return failures;
}

int testCoverThumbMaintenanceSkipsTerminalFailuresAndInvalidKeys() {
    printSeparator("Test: deferred cover thumb maintenance skips terminal failures and invalid keys");

    const fs::path root = makeTempRoot("cover_indexing_thumb_terminal_failures");
    const fs::path coverFolder = root / "Pack";
    const fs::path folder = coverFolder / "Sub";
    const fs::path audioPath = folder / "whoosh.wav";
    const fs::path coverPath = coverFolder / "cover.jpg";
    const std::string dbPath = (root / "cover_indexing_thumb_terminal_failures.db").u8string();
    writeSilentMonoWav(audioPath);
    writeTinyCoverImage(coverPath);

    Database db;
    if (!db.open(dbPath)) {
        std::cerr << "FAIL: open db: " << db.getLastError() << "\n";
        std::error_code ec;
        fs::remove_all(root, ec);
        return 1;
    }

    int failures = 0;
    AudioRecord r;
    r.filePath = audioPath.u8string();
    r.fileName = "whoosh.wav";
    r.extension = ".wav";
    r.format = "WAV";
    r.channels = 1;
    r.sampleRate = 8000;
    r.bitDepth = 16;
    r.durationSeconds = 1.0f;
    r.fileSize = 512;
    r.totalFrames = 128;
    r.folderPath = folder.u8string();
    r.hasCover = true;
    r.folderCoverSrcPath = coverPath.u8string();
    r.folderCoverKey =
        CoverModule::CanonicalCoverKey(std::string("F|") + coverPath.u8string());
    if (!db.addFile(r)) {
        std::cerr << "FAIL: add file fixture\n";
        db.close();
        removeDbFamily(dbPath);
        std::error_code ec;
        fs::remove_all(root, ec);
        return 1;
    }

    db.syncCoverRegistry();
    const AudioRecord seeded = db.getFileByPath(audioPath.u8string());
    const std::string activeCoverKey = seeded.folderCoverKey;
    const std::string escapedActiveCoverKey = escapeSqlLiteral(activeCoverKey);
    const std::string escapedBlobsPath = escapeSqlLiteral(db.getBlobsDbPathPublic(dbPath));
    std::vector<std::string> fixtureSql = {
        "ATTACH DATABASE '" + escapedBlobsPath + "' AS blobs_db;",
        "DELETE FROM blobs_db.cover_thumbs;",
        "UPDATE cover_cache_registry SET cover_phash = 0, cover_phash_version = 0;",
    };
    for (int i = 0; i < 205; ++i) {
        fixtureSql.push_back(
            "INSERT OR REPLACE INTO cover_cache_registry "
            "(cover_key, rep_path, is_embedded, normalized_key, cover_phash, cover_phash_version) "
            "VALUES ('E|dead_terminal_" + std::to_string(i) +
            "', 'Z:/missing_embed_" + std::to_string(i) +
            ".wav', 1, '', 0, 0);");
    }
    fixtureSql.push_back(
        "INSERT OR REPLACE INTO cover_cache_registry "
        "(cover_key, rep_path, is_embedded, normalized_key, cover_phash, cover_phash_version) "
        "VALUES ('', 'Z:/ignored_empty_key', 0, '', 0, 0);");
    fixtureSql.push_back(
        "UPDATE cover_cache_registry SET cover_phash = 0, cover_phash_version = 0 "
        "WHERE cover_key = '" + escapedActiveCoverKey + "';");
    fixtureSql.push_back(
        "INSERT OR REPLACE INTO app_meta (key, value) VALUES ('cover_registry_needs_maintenance', '1');");
    fixtureSql.push_back("DETACH DATABASE blobs_db;");
    const bool fixturePrimed = execSqlOnDbPath(dbPath, fixtureSql);
    std::cout << (fixturePrimed ? "PASS" : "FAIL")
              << ": primed fixture with >200 dead rows, one empty key row, and one valid row\n";
    if (!fixturePrimed) ++failures;

    db.startCoverPhashBackfillWorker();
    for (int i = 0; i < 40 && !db.hasCoverThumb(activeCoverKey); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    db.stopCoverPhashBackfillWorker();

    const bool validRowEventuallyBakes = db.hasCoverThumb(activeCoverKey);
    std::cout << (validRowEventuallyBakes ? "PASS" : "FAIL")
              << ": worker progresses past >200 terminal failures and still bakes the later valid row\n";
    if (!validRowEventuallyBakes) ++failures;

    const bool markerCleared = queryAppMetaInt(db, "cover_registry_needs_maintenance") == 0;
    std::cout << (markerCleared ? "PASS" : "FAIL")
              << ": maintenance marker clears after eligible rows are exhausted and invalid rows are ignored\n";
    if (!markerCleared) ++failures;

    db.close();
    removeDbFamily(dbPath);
    std::error_code ec;
    fs::remove_all(root, ec);
    return failures;
}

int testCoverThumbMaintenanceSourceContract() {
    printSeparator("Test: deferred cover thumb maintenance uses explicit batch transaction and bake diagnostics");

    const fs::path sourcePath =
        fs::path(__FILE__).parent_path().parent_path() / "src" / "Database.cpp";
    std::ifstream in(sourcePath);
    if (!in.good()) {
        std::cerr << "FAIL: open source: " << sourcePath.u8string() << "\n";
        return 1;
    }

    std::ostringstream buffer;
    buffer << in.rdbuf();
    const std::string source = buffer.str();
    const std::string countBody =
        extractFunctionBody(source, "static int CountMissingCoverThumbRowsForMaintenance(sqlite3* db)");
    const std::string body =
        extractFunctionBody(source, "int Database::repairMissingCoverThumbsBatch(int batchSize)");

    int failures = 0;
    const bool sharedEligiblePredicate =
        source.find("kCoverThumbRepairEligibleWhereSql") != std::string::npos &&
        countBody.find("kCoverThumbRepairEligibleWhereSql") != std::string::npos &&
        body.find("kCoverThumbRepairEligibleWhereSql") != std::string::npos &&
        source.find("r.cover_key IS NOT NULL") != std::string::npos &&
        source.find("r.cover_key <> ''") != std::string::npos &&
        source.find("COALESCE(r.cover_phash_version, 0) <> -1") != std::string::npos &&
        source.find("SET cover_phash = 0, cover_phash_version = -1") != std::string::npos &&
        body.find("cover_phash_version < 2") == std::string::npos;
    std::cout << (sharedEligiblePredicate ? "PASS" : "FAIL")
              << ": missing-thumb repair and count share one eligible predicate with empty-key and terminal-failure guards\n";
    if (!sharedEligiblePredicate) ++failures;

    const bool transactionalBatching =
        body.find("BEGIN TRANSACTION") != std::string::npos &&
        body.find("COMMIT") != std::string::npos;
    std::cout << (transactionalBatching ? "PASS" : "FAIL")
              << ": missing-thumb repair contract uses explicit transaction batching\n";
    if (!transactionalBatching) ++failures;

    const bool boundedRawFallback =
        source.find("kCoverThumbRawFallbackMaxBytes") != std::string::npos &&
        body.find("art.data.size() <= kCoverThumbRawFallbackMaxBytes") != std::string::npos;
    std::cout << (boundedRawFallback ? "PASS" : "FAIL")
              << ": missing-thumb repair caps raw fallback bytes before batching\n";
    if (!boundedRawFallback) ++failures;

    const bool bakeDiagnostics =
        source.find("CoverThumbBakeProgress") != std::string::npos &&
        source.find("CoverThumbBakeDone") != std::string::npos;
    std::cout << (bakeDiagnostics ? "PASS" : "FAIL")
              << ": deferred thumb prebake emits progress and done diagnostics\n";
    if (!bakeDiagnostics) ++failures;

    return failures;
}

int testCoverIdentitySamenessAcrossWallFacetAndPreview() {
    printSeparator("Test: cover identity sameness across wall, facet, and preview");

    const std::string dbPath = "test_cover_identity_sameness.db";
    removeDbFamily(dbPath);

    Database db;
    if (!db.open(dbPath)) {
        std::cerr << "FAIL: open db: " << db.getLastError() << "\n";
        return 1;
    }

    int failures = 0;
    const std::string folderPath = "C:/lib/folder";
    const std::string canonicalFolderPath = HarkenSearch::CanonicalFolderPath(folderPath);
    const std::string coverSrcPath = folderPath + "/cover.jpg";
    const std::string canonicalCoverSrcPath = HarkenSearch::CanonicalFolderPath(coverSrcPath);
    const std::string legacyKey = "F|" + coverSrcPath;
    const std::string canonicalKey = CoverModule::CanonicalCoverKey(legacyKey);

    auto addCoveredFile = [&](const std::string& path, const std::string& coverKey) {
        AudioRecord r;
        r.filePath = path;
        r.fileName = path.substr(path.find_last_of("/\\") + 1);
        r.extension = ".wav";
        r.format = "WAV";
        r.channels = 2;
        r.sampleRate = 48000;
        r.bitDepth = 24;
        r.durationSeconds = 1.0f;
        r.fileSize = 1024;
        r.totalFrames = 48000;
        r.folderPath = folderPath;
        r.hasCover = true;
        r.folderCoverKey = coverKey;
        r.folderCoverSrcPath = coverSrcPath;
        return db.addFile(r);
    };

    if (!addCoveredFile(folderPath + "/a.wav", legacyKey)) ++failures;
    if (!addCoveredFile(folderPath + "/b.wav", canonicalKey)) ++failures;

    std::vector<uint8_t> sameThumb(64);
    for (size_t i = 0; i < sameThumb.size(); ++i) sameThumb[i] = static_cast<uint8_t>(90 + i);
    if (!db.saveCoverThumb(legacyKey, sameThumb, "image/jpeg")) ++failures;
    if (!db.saveCoverThumb(canonicalKey, sameThumb, "image/jpeg")) ++failures;

    std::vector<Database::CoverRegistryDelta> deltas = {
        {legacyKey, canonicalFolderPath, false},
        {canonicalKey, canonicalFolderPath, false},
    };
    (void)db.finalizeCoverRegistryDeltas(deltas);

    SearchFilter baseFilter;
    baseFilter.folderPath = canonicalFolderPath;
    auto results = db.searchFiles(baseFilter, 20, 0);
    const bool baseSearchHasRows = results.size() == 2;
    std::cout << (baseSearchHasRows ? "PASS" : "FAIL")
              << ": base search exposes both records for sameness test; actual=" << results.size() << "\n";
    if (!baseSearchHasRows) ++failures;

    auto wallItems = buildCoverWallItemsForTest(results);
    const bool wallDeduped = wallItems.size() == 1;
    std::cout << (wallDeduped ? "PASS" : "FAIL")
              << ": wall builder dedupes mixed legacy/new keys to one identity; actual=" << wallItems.size() << "\n";
    if (!wallDeduped) ++failures;

    const bool wallCanonical = wallDeduped &&
                               wallItems[0].key == canonicalKey &&
                               HarkenSearch::CanonicalFolderPath(wallItems[0].folderPathUtf8) == canonicalFolderPath &&
                               wallItems[0].folderCoverSrcPathUtf8 == coverSrcPath;
    std::cout << (wallCanonical ? "PASS" : "FAIL")
              << ": wall item keeps canonical key plus real source path\n";
    if (!wallCanonical && wallDeduped) {
        std::cout << "  expected key=" << canonicalKey
                  << " actual key=" << wallItems[0].key
                  << " expected folder=" << canonicalFolderPath
                  << " actual folder=" << wallItems[0].folderPathUtf8
                  << " expected src=" << coverSrcPath
                  << " actual src=" << wallItems[0].folderCoverSrcPathUtf8 << "\n";
    }
    if (!wallCanonical) ++failures;

    auto globalWallEntries = db.getGlobalCoverWallEntries();
    bool globalWallKeepsSourcePath = false;
    for (const auto& entry : globalWallEntries) {
        if (!entry.isEmbedded &&
            entry.key == canonicalKey &&
            HarkenSearch::CanonicalFolderPath(entry.path) == canonicalFolderPath &&
            HarkenSearch::CanonicalFolderPath(entry.sourcePath) == canonicalCoverSrcPath) {
            globalWallKeepsSourcePath = true;
            break;
        }
    }
    std::cout << (globalWallKeepsSourcePath ? "PASS" : "FAIL")
              << ": global cover wall entry keeps folder path plus real cover source path\n";
    if (!globalWallKeepsSourcePath) ++failures;

    auto facets = db.getCoverFacets(baseFilter, 20);
    const bool facetDeduped = facets.size() == 1;
    std::cout << (facetDeduped ? "PASS" : "FAIL")
              << ": cover facets collapse mixed legacy/new keys to one visual group; actual=" << facets.size() << "\n";
    if (!facetDeduped) ++failures;

    const bool facetAligned = facetDeduped &&
                              CoverModule::CanonicalCoverKey(facets[0].key) == canonicalKey &&
                              HarkenSearch::CanonicalFolderPath(facets[0].path) == canonicalFolderPath &&
                              !facets[0].isEmbedded &&
                              facets[0].count == 2;
    std::cout << (facetAligned ? "PASS" : "FAIL")
              << ": facet representative stays aligned with wall identity and folder scope\n";
    if (!facetAligned && facetDeduped) {
        std::cout << "  expected key=" << canonicalKey
                  << " actual facet key=" << facets[0].key
                  << " canonical facet key=" << CoverModule::CanonicalCoverKey(facets[0].key)
                  << " expected path=" << canonicalFolderPath
                  << " actual path=" << facets[0].path
                  << " count=" << facets[0].count
                  << " embedded=" << (facets[0].isEmbedded ? 1 : 0) << "\n";
    }
    if (!facetAligned) ++failures;

    SearchFilter coverFilter;
    coverFilter.specificCoverKey = facets.empty() ? canonicalKey : facets[0].key;
    auto filtered = db.searchFiles(coverFilter, 20, 0);
    const bool filterHitsBoth = filtered.size() == 2;
    std::cout << (filterHitsBoth ? "PASS" : "FAIL")
              << ": specificCoverKey filter resolves both legacy/new rows; actual=" << filtered.size() << "\n";
    if (!filterHitsBoth) ++failures;

    bool previewAligned = filterHitsBoth;
    for (const auto& record : filtered) {
        if (CoverModule::CanonicalCoverKey(record.folderCoverKey) != canonicalKey ||
            record.folderCoverSrcPath != coverSrcPath ||
            HarkenSearch::CanonicalFolderPath(record.folderPath) != canonicalFolderPath) {
            previewAligned = false;
            std::cout << "  mismatched record path=" << record.filePath
                      << " key=" << record.folderCoverKey
                      << " canonical=" << CoverModule::CanonicalCoverKey(record.folderCoverKey)
                      << " folder=" << record.folderPath
                      << " src=" << record.folderCoverSrcPath << "\n";
            break;
        }
    }
    std::cout << (previewAligned ? "PASS" : "FAIL")
              << ": current preview records preserve same cover identity and source path after cover filter\n";
    if (!previewAligned) ++failures;

    db.close();
    removeDbFamily(dbPath);
    return failures;
}

int testPureFolderSearchFastPathRespectsFolderBoundary() {
    int failures = 0;
    const std::string dbPath = "test_pure_folder_fast_path.db";
    removeDbFamily(dbPath);

    Database db;
    if (!db.open(dbPath)) {
        std::cout << "FAIL: pure folder fast path setup opened temp database\n";
        return failures + 1;
    }

    auto addFile = [&](const std::string& path, const std::string& folder) {
        AudioRecord r;
        r.filePath = path;
        r.fileName = std::filesystem::path(path).filename().u8string();
        r.folderPath = folder;
        r.extension = ".wav";
        r.format = "wav";
        r.sampleRate = 48000;
        r.channels = 2;
        r.bitDepth = 24;
        r.durationSeconds = 1.0f;
        r.fileSize = 1024;
        r.totalFrames = 48000;
        return db.addFile(r);
    };

    const std::string root = HarkenSearch::CanonicalFolderPath("C:/lib/BoomLibrary");
    const std::string child = HarkenSearch::CanonicalFolderPath("C:/lib/BoomLibrary/Impacts");
    const std::string sibling = HarkenSearch::CanonicalFolderPath("C:/lib/BoomLibraryExtra");

    if (!addFile(root + "/alpha.wav", root)) ++failures;
    if (!addFile(child + "/beta.wav", child)) ++failures;
    if (!addFile(sibling + "/gamma.wav", sibling)) ++failures;

    SearchFilter filter;
    filter.folderPath = root;
    auto rows = db.searchFiles(filter, 20, 0);
    const int64_t count = db.searchFilesCount(filter);

    const bool onlyRootFamily =
        rows.size() == 2 &&
        count == 2 &&
        std::all_of(rows.begin(), rows.end(), [&](const AudioRecord& row) {
            const std::string canonical = HarkenSearch::CanonicalFolderPath(row.folderPath);
            return canonical == root || canonical == child;
        });
    std::cout << (onlyRootFamily ? "PASS" : "FAIL")
              << ": pure folder search fast path keeps root plus descendants without sibling-prefix bleed\n";
    if (!onlyRootFamily) {
        std::cout << "  actual rows=" << rows.size() << " count=" << count << "\n";
        for (const auto& row : rows) {
            std::cout << "  row: " << row.fileName << " folder=" << row.folderPath << "\n";
        }
        ++failures;
    }

    db.close();
    removeDbFamily(dbPath);
    return failures;
}

int testFilenameSortingUsesNaturalNumericOrder() {
    int failures = 0;
    const std::string dbPath = "test_filename_natural_sort.db";
    removeDbFamily(dbPath);

    Database db;
    if (!db.open(dbPath)) {
        std::cout << "FAIL: filename natural sort setup opened temp database\n";
        return failures + 1;
    }

    const std::string root = HarkenSearch::CanonicalFolderPath("C:/lib/NumericNames");
    for (int i = 1; i <= 15; ++i) {
        AudioRecord r;
        r.fileName = std::to_string(i) + ".wav";
        r.filePath = root + "/" + r.fileName;
        r.folderPath = root;
        r.extension = ".wav";
        r.format = "wav";
        r.sampleRate = 48000;
        r.channels = 2;
        r.bitDepth = 24;
        r.durationSeconds = 1.0f;
        r.fileSize = 1024;
        r.totalFrames = 48000;
        if (!db.addFile(r)) ++failures;
    }

    std::vector<std::string> expectedAsc;
    expectedAsc.reserve(15);
    for (int i = 1; i <= 15; ++i) {
        expectedAsc.push_back(std::to_string(i) + ".wav");
    }
    std::vector<std::string> expectedDesc(expectedAsc.rbegin(), expectedAsc.rend());

    auto collectNames = [](const std::vector<AudioRecord>& rows) {
        std::vector<std::string> names;
        names.reserve(rows.size());
        for (const auto& row : rows) names.push_back(row.fileName);
        return names;
    };

    auto printNames = [](const std::vector<std::string>& names) {
        for (size_t i = 0; i < names.size(); ++i) {
            if (i) std::cout << ", ";
            std::cout << names[i];
        }
        std::cout << "\n";
    };

    SearchFilter globalAsc;
    globalAsc.sortAscending = true;
    auto globalAscRows = db.searchFiles(globalAsc, 20, 0);
    const auto globalAscNames = collectNames(globalAscRows);
    const bool globalAscOk = globalAscNames == expectedAsc;
    std::cout << (globalAscOk ? "PASS" : "FAIL")
              << ": global filename ascending sort uses natural numeric order\n";
    if (!globalAscOk) {
        std::cout << "  actual: ";
        printNames(globalAscNames);
        ++failures;
    }

    SearchFilter globalDesc;
    globalDesc.sortAscending = false;
    auto globalDescRows = db.searchFiles(globalDesc, 20, 0);
    const auto globalDescNames = collectNames(globalDescRows);
    const bool globalDescOk = globalDescNames == expectedDesc;
    std::cout << (globalDescOk ? "PASS" : "FAIL")
              << ": global filename descending sort uses natural numeric order\n";
    if (!globalDescOk) {
        std::cout << "  actual: ";
        printNames(globalDescNames);
        ++failures;
    }

    SearchFilter folderAsc;
    folderAsc.folderPath = root;
    folderAsc.sortAscending = true;
    auto folderAscRows = db.searchFiles(folderAsc, 20, 0);
    const auto folderAscNames = collectNames(folderAscRows);
    const bool folderAscOk = folderAscNames == expectedAsc;
    std::cout << (folderAscOk ? "PASS" : "FAIL")
              << ": pure folder filename fast path uses natural numeric order\n";
    if (!folderAscOk) {
        std::cout << "  actual: ";
        printNames(folderAscNames);
        ++failures;
    }

    db.close();
    removeDbFamily(dbPath);
    return failures;
}

int testPlaylistSortingUsesNaturalNumericOrder() {
    int failures = 0;
    const std::string dbPath = "test_playlist_natural_sort.db";
    removeDbFamily(dbPath);

    Database db;
    if (!db.open(dbPath)) {
        std::cout << "FAIL: playlist natural sort setup opened temp database\n";
        return failures + 1;
    }

    const std::vector<std::string> expectedPlaylistNames = {"1", "2", "10", "11"};
    for (const auto& name : expectedPlaylistNames) {
        if (db.createPlaylist(name) <= 0) ++failures;
    }

    auto collectPlaylistNames = [](const std::vector<Playlist>& playlists) {
        std::vector<std::string> names;
        names.reserve(playlists.size());
        for (const auto& playlist : playlists) names.push_back(playlist.name);
        return names;
    };

    auto printNames = [](const std::vector<std::string>& names) {
        for (size_t i = 0; i < names.size(); ++i) {
            if (i) std::cout << ", ";
            std::cout << names[i];
        }
        std::cout << "\n";
    };

    const auto topLevelPlaylists = db.getAllPlaylists();
    const auto topLevelNames = collectPlaylistNames(topLevelPlaylists);
    const bool topLevelOk = topLevelNames.size() >= expectedPlaylistNames.size() &&
        std::equal(expectedPlaylistNames.begin(),
                   expectedPlaylistNames.end(),
                   topLevelNames.begin());
    std::cout << (topLevelOk ? "PASS" : "FAIL")
              << ": playlist sidebar names use natural numeric order\n";
    if (!topLevelOk) {
        std::cout << "  actual: ";
        printNames(topLevelNames);
        ++failures;
    }

    const int64_t parentId = db.createPlaylist("FolderRoot");
    const bool parentPrepared = parentId > 0 && db.markAsFolder(parentId);
    std::cout << (parentPrepared ? "PASS" : "FAIL")
              << ": playlist natural sort fixture can create a parent folder\n";
    if (!parentPrepared) {
        db.close();
        removeDbFamily(dbPath);
        return failures + 1;
    }

    const std::string root = HarkenSearch::CanonicalFolderPath("C:/lib/PlaylistNaturalNames");
    for (const auto& name : expectedPlaylistNames) {
        const int64_t childId = db.createSubPlaylist(parentId, name);
        if (childId <= 0) {
            ++failures;
            continue;
        }

        AudioRecord r;
        r.fileName = name + ".wav";
        r.filePath = root + "/" + r.fileName;
        r.folderPath = root;
        r.extension = ".wav";
        r.format = "wav";
        r.sampleRate = 48000;
        r.channels = 2;
        r.bitDepth = 24;
        r.durationSeconds = 1.0f;
        r.fileSize = 1024;
        r.totalFrames = 48000;
        if (!db.addFile(r)) {
            ++failures;
            continue;
        }
        const AudioRecord saved = db.getFileByPath(r.filePath);
        if (saved.id <= 0 || !db.addToPlaylist(childId, saved.id)) {
            ++failures;
        }
    }

    const auto childNames = collectPlaylistNames(db.getChildrenOf(parentId));
    const bool childOrderOk = childNames == expectedPlaylistNames;
    std::cout << (childOrderOk ? "PASS" : "FAIL")
              << ": child playlists use natural numeric order\n";
    if (!childOrderOk) {
        std::cout << "  actual: ";
        printNames(childNames);
        ++failures;
    }

    auto collectFileNames = [](const std::vector<AudioRecord>& rows) {
        std::vector<std::string> names;
        names.reserve(rows.size());
        for (const auto& row : rows) names.push_back(row.fileName);
        return names;
    };

    const std::vector<std::string> expectedFileNames = {"1.wav", "2.wav", "10.wav", "11.wav"};

    const auto allPlaylistFileNames = collectFileNames(db.getAllPlaylistFilesPage(20, 0));
    const bool allPlaylistPageOk = allPlaylistFileNames == expectedFileNames;
    std::cout << (allPlaylistPageOk ? "PASS" : "FAIL")
              << ": playlist library page uses natural numeric filename order\n";
    if (!allPlaylistPageOk) {
        std::cout << "  actual: ";
        printNames(allPlaylistFileNames);
        ++failures;
    }

    SearchFilter emptyFilter;
    const auto searchedAllPlaylistFileNames =
        collectFileNames(db.searchAllPlaylistFilesPage(emptyFilter, 20, 0));
    const bool searchedAllPlaylistPageOk = searchedAllPlaylistFileNames == expectedFileNames;
    std::cout << (searchedAllPlaylistPageOk ? "PASS" : "FAIL")
              << ": searched playlist library page uses natural numeric filename order\n";
    if (!searchedAllPlaylistPageOk) {
        std::cout << "  actual: ";
        printNames(searchedAllPlaylistFileNames);
        ++failures;
    }

    const auto aggregatedFileNames =
        collectFileNames(db.getAggregatedPlaylistFilesPage(parentId, 20, 0));
    const bool aggregatedPageOk = aggregatedFileNames == expectedFileNames;
    std::cout << (aggregatedPageOk ? "PASS" : "FAIL")
              << ": aggregated playlist page uses natural numeric filename order\n";
    if (!aggregatedPageOk) {
        std::cout << "  actual: ";
        printNames(aggregatedFileNames);
        ++failures;
    }

    const auto searchedAggregatedFileNames =
        collectFileNames(db.searchAggregatedPlaylistFilesPage(parentId, emptyFilter, 20, 0));
    const bool searchedAggregatedPageOk = searchedAggregatedFileNames == expectedFileNames;
    std::cout << (searchedAggregatedPageOk ? "PASS" : "FAIL")
              << ": searched aggregated playlist page uses natural numeric filename order\n";
    if (!searchedAggregatedPageOk) {
        std::cout << "  actual: ";
        printNames(searchedAggregatedFileNames);
        ++failures;
    }

    db.close();
    removeDbFamily(dbPath);
    return failures;
}

int testGlobalCoverFacetFallsBackWhenRegistrySparse() {
    printSeparator("Test: global cover facet falls back when registry is sparse");

    const std::string dbPath = "test_cover_global_sparse_registry.db";
    removeDbFamily(dbPath);

    Database db;
    if (!db.open(dbPath)) {
        std::cerr << "FAIL: open db: " << db.getLastError() << "\n";
        return 1;
    }

    int failures = 0;
    auto addFileWithCover = [&](const std::string& filePath,
                                const std::string& folderPath,
                                const std::string& embeddedKey,
                                const std::string& folderKey,
                                const std::string& folderSrcPath) {
        AudioRecord r;
        r.filePath = filePath;
        r.fileName = filePath.substr(filePath.find_last_of("/\\") + 1);
        r.extension = ".wav";
        r.format = "WAV";
        r.channels = 2;
        r.sampleRate = 48000;
        r.bitDepth = 24;
        r.durationSeconds = 1.0f;
        r.fileSize = 1024;
        r.totalFrames = 48000;
        r.folderPath = folderPath;
        r.hasCover = true;
        r.embeddedCoverKey = embeddedKey;
        r.folderCoverKey = folderKey;
        r.folderCoverSrcPath = folderSrcPath;
        return db.addFile(r);
    };

    const std::string folderA = "C:/lib/A";
    const std::string folderB = "C:/lib/B";
    const std::string folderC = "C:/lib/C";
    const std::string embedA = "E|a|100|image/jpeg";
    const std::string embedB = "E|b|100|image/jpeg";
    const std::string embedC = "E|c|100|image/jpeg";
    const std::string folderKeyA = CoverModule::CanonicalCoverKey("F|C:/lib/A/cover.jpg");
    const std::string folderKeyB = CoverModule::CanonicalCoverKey("F|C:/lib/B/cover.jpg");

    if (!addFileWithCover(folderA + "/a.wav", folderA, embedA, "", "")) ++failures;
    if (!addFileWithCover(folderB + "/b.wav", folderB, embedB, "", "")) ++failures;
    if (!addFileWithCover(folderC + "/c.wav", folderC, embedC, "", "")) ++failures;
    if (!addFileWithCover(folderA + "/d.wav", folderA, "", folderKeyA, folderA + "/cover.jpg")) ++failures;
    if (!addFileWithCover(folderB + "/e.wav", folderB, "", folderKeyB, folderB + "/cover.jpg")) ++failures;

    // Intentionally register only a sparse subset. The global wall must not truncate
    // visible covers to this stale registry footprint.
    std::vector<Database::CoverRegistryDelta> deltas = {
        {embedA, folderA + "/a.wav", true},
        {folderKeyA, HarkenSearch::CanonicalFolderPath(folderA), false},
    };
    (void)db.finalizeCoverRegistryDeltas(deltas);

    SearchFilter globalFilter;
    auto facets = db.getCoverFacets(globalFilter, 20);
    const bool fullIdentityCountVisible = facets.size() == 5;
    std::cout << (fullIdentityCountVisible ? "PASS" : "FAIL")
              << ": sparse registry fallback preserves all distinct global cover identities; actual="
              << facets.size() << "\n";
    if (!fullIdentityCountVisible) {
        for (const auto& facet : facets) {
            std::cout << "  facet key=" << facet.key
                      << " path=" << facet.path
                      << " embedded=" << (facet.isEmbedded ? 1 : 0)
                      << " count=" << facet.count << "\n";
        }
        ++failures;
    }

    db.close();
    removeDbFamily(dbPath);
    return failures;
}

int testGlobalCoverWallFallsBackWhenRegistrySparse() {
    printSeparator("Test: global cover wall falls back when registry is sparse");

    const std::string dbPath = "test_cover_wall_sparse_registry.db";
    removeDbFamily(dbPath);

    Database db;
    if (!db.open(dbPath)) {
        std::cerr << "FAIL: open db: " << db.getLastError() << "\n";
        return 1;
    }

    int failures = 0;
    auto addFileWithFolderCover = [&](const std::string& folderPath,
                                      const std::string& fileName,
                                      const std::string& folderKey) {
        AudioRecord r;
        r.filePath = folderPath + "/" + fileName;
        r.fileName = fileName;
        r.extension = ".wav";
        r.format = "WAV";
        r.channels = 2;
        r.sampleRate = 48000;
        r.bitDepth = 24;
        r.durationSeconds = 1.0f;
        r.fileSize = 1024;
        r.totalFrames = 48000;
        r.folderPath = folderPath;
        r.hasCover = true;
        r.folderCoverKey = folderKey;
        r.folderCoverSrcPath = folderPath + "/cover.jpg";
        return db.addFile(r);
    };

    const std::string folderA = "C:/lib/wall_sparse/A";
    const std::string folderB = "C:/lib/wall_sparse/B";
    const std::string folderKeyA = CoverModule::CanonicalCoverKey("F|" + folderA + "/cover.jpg");
    const std::string folderKeyB = CoverModule::CanonicalCoverKey("F|" + folderB + "/cover.jpg");

    if (!addFileWithFolderCover(folderA, "a.wav", folderKeyA)) ++failures;
    if (!addFileWithFolderCover(folderB, "b.wav", folderKeyB)) ++failures;

    // Register only one folder cover. The full-library wall must still show
    // both covers for a small library instead of trusting the stale registry.
    std::vector<Database::CoverRegistryDelta> deltas = {
        {folderKeyA, HarkenSearch::CanonicalFolderPath(folderA), false},
    };
    (void)db.finalizeCoverRegistryDeltas(deltas);

    auto wall = db.getGlobalCoverWallEntries();
    const bool fullWallCountVisible = wall.size() == 2;
    std::cout << (fullWallCountVisible ? "PASS" : "FAIL")
              << ": sparse registry fallback preserves all distinct global wall covers; actual="
              << wall.size() << "\n";
    if (!fullWallCountVisible) {
        for (const auto& entry : wall) {
            std::cout << "  wall key=" << entry.key
                      << " path=" << entry.path
                      << " source=" << entry.sourcePath
                      << " embedded=" << (entry.isEmbedded ? 1 : 0) << "\n";
        }
        ++failures;
    }

    db.close();
    removeDbFamily(dbPath);
    return failures;
}

int testGlobalCoverFacetFallsBackWhenHasCoverRowsLackKeys() {
    printSeparator("Test: global cover facet falls back when has_cover rows lack keys");

    const std::string dbPath = "test_cover_global_has_cover_without_keys.db";
    removeDbFamily(dbPath);

    Database db;
    if (!db.open(dbPath)) {
        std::cerr << "FAIL: open db: " << db.getLastError() << "\n";
        return 1;
    }

    int failures = 0;
    auto addFileWithImplicitFolderCover = [&](const std::string& folderPath, const std::string& fileName) {
        AudioRecord r;
        r.filePath = folderPath + "/" + fileName;
        r.fileName = fileName;
        r.extension = ".wav";
        r.format = "WAV";
        r.channels = 2;
        r.sampleRate = 48000;
        r.bitDepth = 24;
        r.durationSeconds = 1.0f;
        r.fileSize = 1024;
        r.totalFrames = 48000;
        r.folderPath = folderPath;
        r.hasCover = true;
        r.folderCoverSrcPath = folderPath + "/cover.jpg";
        return db.addFile(r);
    };

    const std::string folderA = "C:/lib/implicit/A";
    const std::string folderB = "C:/lib/implicit/B";
    if (!addFileWithImplicitFolderCover(folderA, "a.wav")) ++failures;
    if (!addFileWithImplicitFolderCover(folderB, "b.wav")) ++failures;

    SearchFilter globalFilter;
    auto facets = db.getCoverFacets(globalFilter, 20);
    const bool fullFacetCountVisible = facets.size() == 2;
    std::cout << (fullFacetCountVisible ? "PASS" : "FAIL")
              << ": global facets surface has_cover folders even before keys are primed; actual="
              << facets.size() << "\n";
    if (!fullFacetCountVisible) {
        for (const auto& facet : facets) {
            std::cout << "  facet key=" << facet.key
                      << " path=" << facet.path
                      << " embedded=" << (facet.isEmbedded ? 1 : 0)
                      << " count=" << facet.count << "\n";
        }
        ++failures;
    }

    bool allTempFolderKeys = facets.size() == 2;
    for (const auto& facet : facets) {
        if (facet.key.rfind("?|", 0) != 0 || facet.path.empty() || facet.isEmbedded) {
            allTempFolderKeys = false;
            break;
        }
    }
    std::cout << (allTempFolderKeys ? "PASS" : "FAIL")
              << ": global facets use temporary folder identities for has_cover rows without keys\n";
    if (!allTempFolderKeys) ++failures;

    db.close();
    removeDbFamily(dbPath);
    return failures;
}

int testTemporaryCoverIdentityReverseLookupMatchesCoverFilter() {
    printSeparator("Test: temporary cover identity reverse lookup matches cover filter");

    const std::string dbPath = "test_cover_temp_identity_reverse_lookup.db";
    removeDbFamily(dbPath);

    Database db;
    if (!db.open(dbPath)) {
        std::cerr << "FAIL: open db: " << db.getLastError() << "\n";
        return 1;
    }

    int failures = 0;
    auto addFileWithImplicitFolderCover = [&](const std::string& folderPath, const std::string& name) {
        AudioRecord r;
        r.filePath = folderPath + "/" + name;
        r.fileName = name;
        r.extension = ".wav";
        r.format = "WAV";
        r.channels = 2;
        r.sampleRate = 48000;
        r.bitDepth = 24;
        r.durationSeconds = 1.0f;
        r.fileSize = 1024;
        r.totalFrames = 48000;
        r.folderPath = folderPath;
        r.hasCover = true;
        r.folderCoverSrcPath = folderPath + "/cover.jpg";
        return db.addFile(r);
    };

    const std::string folderA = HarkenSearch::CanonicalFolderPath("C:/lib/temp_identity/A");
    const std::string folderB = HarkenSearch::CanonicalFolderPath("C:/lib/temp_identity/B");
    if (!addFileWithImplicitFolderCover(folderA, "a.wav")) ++failures;
    if (!addFileWithImplicitFolderCover(folderA, "b.wav")) ++failures;
    if (!addFileWithImplicitFolderCover(folderB, "c.wav")) ++failures;

    SearchFilter globalFilter;
    auto facets = db.getCoverFacets(globalFilter, 20);
    const auto facetIt = std::find_if(facets.begin(), facets.end(), [&](const Database::CoverFacetEntry& facet) {
        return facet.key == ("?|" + folderA);
    });
    const bool hasTemporaryFacet = facetIt != facets.end();
    std::cout << (hasTemporaryFacet ? "PASS" : "FAIL")
              << ": global facets expose the temporary folder identity used for reverse lookup\n";
    if (!hasTemporaryFacet) ++failures;

    auto membershipIds = db.getFileIdsForCoverTileMembership("?|" + folderA);
    const bool membershipIdsReturnWholeFolder = membershipIds.size() == 2;
    std::cout << (membershipIdsReturnWholeFolder ? "PASS" : "FAIL")
              << ": temporary folder identity tile membership returns all matching file ids\n";
    if (!membershipIdsReturnWholeFolder) ++failures;

    SearchFilter coverFilter;
    coverFilter.specificCoverKey = "?|" + folderA;
    auto filtered = db.searchFiles(coverFilter, 20, 0);
    const bool coverFilterMatchesTileMembership =
        filtered.size() == membershipIds.size() &&
        std::all_of(filtered.begin(), filtered.end(), [&](const AudioRecord& record) {
            return HarkenSearch::CanonicalFolderPath(record.folderPath) == folderA &&
                   record.embeddedCoverKey.empty() &&
                   record.folderCoverKey.empty();
        });
    std::cout << (coverFilterMatchesTileMembership ? "PASS" : "FAIL")
              << ": temporary folder identity cover filter matches tile membership semantics\n";
    if (!coverFilterMatchesTileMembership) ++failures;

    db.close();
    removeDbFamily(dbPath);
    return failures;
}

int testCoverWallGroupedSelectionIncludesAllFoldersWithoutFuzzyNeighbors() {
    printSeparator("Test: grouped cover wall selection uses exact member identities");
    const std::string dbPath = "test_cover_wall_grouped_selection.db";
    removeDbFamily(dbPath);
    Database db;
    if (!db.open(dbPath)) return 1;
    const std::string keyA = CoverModule::CanonicalCoverKey("F|PATH|C:/covers/shared.jpg");
    const std::string keyB = CoverModule::CanonicalCoverKey("F|PATH|C:/covers/near.jpg");
    int failures = 0;
    auto add = [&](const std::string& folder, const std::string& name, const std::string& key) {
        AudioRecord r;
        r.filePath = folder + "/" + name;
        r.fileName = name;
        r.folderPath = folder;
        r.extension = ".wav";
        r.format = "WAV";
        r.channels = 2;
        r.sampleRate = 48000;
        r.bitDepth = 24;
        r.durationSeconds = 1.0f;
        r.fileSize = 1024;
        r.totalFrames = 48000;
        r.hasCover = true;
        r.folderCoverKey = key;
        if (!key.empty()) r.folderCoverSrcPath = key.substr(7);
        if (!db.addFile(r)) ++failures;
    };
    add("C:/lib/wall-group/a", "a.wav", "");
    add("C:/lib/wall-group/b", "b.wav", "");
    add("C:/lib/wall-group/c", "c.wav", keyA);
    add("C:/lib/wall-group/d", "d.wav", keyB);
    std::ostringstream registrySql;
    registrySql << "INSERT OR REPLACE INTO cover_cache_registry "
        "(cover_key, rep_path, is_embedded, normalized_key, cover_phash, cover_phash_version) VALUES "
        "('" << keyA << "', 'C:/lib/wall-group/c', 0, 'PHASH|12345', 12345, 2), "
        "('" << keyB << "', 'C:/lib/wall-group/d', 0, 'PHASH|12345', 12345, 2);";
    if (sqlite3_exec(db.db(), registrySql.str().c_str(), nullptr, nullptr, nullptr) != SQLITE_OK) ++failures;
    const std::string group = aura::cover_wall::EncodeGroupedCoverWallKeys({
        "?|C:/lib/wall-group/a", "?|C:/lib/wall-group/b", keyA});
    std::vector<std::string> decoded;
    if (!aura::cover_wall::DecodeCoverWallSelectionKey(group, decoded) || decoded.size() != 3) ++failures;
    const auto ids = db.getFileIdsForCoverTileMembership(group);
    SearchFilter filter;
    filter.specificCoverKey = group;
    const auto groupCount = db.searchFilesCount(filter);
    const auto groupRows = db.searchFiles(filter, 10, 0).size();
    std::cout << "group ids=" << ids.size() << " count=" << groupCount
              << " rows=" << groupRows << '\n';
    if (ids.size() != 3 || groupCount != 3 || groupRows != 3) ++failures;
    const std::string exact = aura::cover_wall::EncodeExactCoverWallKey(keyA);
    filter.specificCoverKey = exact;
    const auto exactIds = db.getFileIdsForCoverTileMembership(exact).size();
    const auto exactCount = db.searchFilesCount(filter);
    std::cout << "exact ids=" << exactIds << " count=" << exactCount << '\n';
    if (exactIds != 1 || exactCount != 1) ++failures;
    sqlite3_stmt* deleteMembership = nullptr;
    if (sqlite3_prepare_v2(db.db(),
                           "DELETE FROM cover_tile_membership WHERE tile_key = ?",
                           -1, &deleteMembership, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(deleteMembership, 1, keyB.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(deleteMembership) != SQLITE_DONE) ++failures;
    } else {
        ++failures;
    }
    if (deleteMembership) sqlite3_finalize(deleteMembership);
    const std::string partlyIndexed =
        aura::cover_wall::EncodeGroupedCoverWallKeys({keyA, keyB});
    filter.specificCoverKey = partlyIndexed;
    const auto repairedIds = db.getFileIdsForCoverTileMembership(partlyIndexed);
    const auto repairedCount = db.searchFilesCount(filter);
    std::cout << "partial index group ids=" << repairedIds.size()
              << " count=" << repairedCount << '\n';
    if (repairedIds.size() != 2 || repairedCount != 2) ++failures;
    db.close();
    removeDbFamily(dbPath);
    return failures;
}

int testCoverTileMembershipUsesEquivalentRegistryKeys() {
    printSeparator("Test: cover tile membership uses equivalent registry keys");

    const std::string dbPath = "test_cover_tile_membership_equivalent_keys.db";
    removeDbFamily(dbPath);

    Database db;
    if (!db.open(dbPath)) {
        std::cerr << "FAIL: open db: " << db.getLastError() << "\n";
        return 1;
    }

    int failures = 0;
    auto addEmbeddedFile = [&](const std::string& filePath, const std::string& key) {
        AudioRecord r;
        r.filePath = filePath;
        r.fileName = filePath.substr(filePath.find_last_of("/\\") + 1);
        r.extension = ".wav";
        r.format = "WAV";
        r.channels = 2;
        r.sampleRate = 48000;
        r.bitDepth = 24;
        r.durationSeconds = 1.0f;
        r.fileSize = 1024;
        r.totalFrames = 48000;
        r.folderPath = filePath.substr(0, filePath.find_last_of("/\\"));
        r.hasCover = true;
        r.embeddedCoverKey = key;
        return db.addFile(r);
    };

    const std::string keyA = "E|tile_membership_a|100|image/jpeg";
    const std::string keyB = "E|tile_membership_b|100|image/jpeg";
    if (!addEmbeddedFile("C:/lib/tile_membership/a.wav", keyA)) ++failures;
    if (!addEmbeddedFile("C:/lib/tile_membership/b.wav", keyB)) ++failures;

    const char* registrySql =
        "INSERT OR REPLACE INTO cover_cache_registry "
        "(cover_key, rep_path, is_embedded, normalized_key, cover_phash, cover_phash_version) "
        "VALUES "
        "('E|tile_membership_a|100|image/jpeg', 'C:/lib/tile_membership/a.wav', 1, 'PHASH|81985529216486895', 81985529216486895, 2), "
        "('E|tile_membership_b|100|image/jpeg', 'C:/lib/tile_membership/b.wav', 1, 'PHASH|81985529216486895', 81985529216486895, 2);";
    char* registryErr = nullptr;
    if (sqlite3_exec(db.db(), registrySql, nullptr, nullptr, &registryErr) != SQLITE_OK) {
        if (registryErr) sqlite3_free(registryErr);
        ++failures;
    }

    auto idsFromKey = db.getFileIdsForCoverTileMembership(keyA);
    const bool keyExpandsToEquivalentRegistryGroup = idsFromKey.size() == 2;
    std::cout << (keyExpandsToEquivalentRegistryGroup ? "PASS" : "FAIL")
              << ": cover tile membership expands one key to equivalent registry group ids\n";
    if (!keyExpandsToEquivalentRegistryGroup) ++failures;

    auto idsFromNormalizedTile = db.getFileIdsForCoverTileMembership("PHASH|81985529216486895");
    const bool normalizedTileResolvesGroup = idsFromNormalizedTile.size() == 2;
    std::cout << (normalizedTileResolvesGroup ? "PASS" : "FAIL")
              << ": normalized cover tile membership resolves equivalent registry group ids\n";
    if (!normalizedTileResolvesGroup) ++failures;

    db.close();
    removeDbFamily(dbPath);
    return failures;
}

int testCoverClickPhashLookupPreservesHammingTwoCluster() {
    printSeparator("Test: cover click phash lookup preserves Hamming-2 cluster semantics");

    const std::string dbPath = "test_cover_click_phash_h2.db";
    removeDbFamily(dbPath);

    Database db;
    if (!db.open(dbPath, true)) {
        std::cerr << "FAIL: open db: " << db.getLastError() << "\n";
        return 1;
    }

    int failures = 0;
    auto addEmbeddedFile = [&](const std::string& filePath, const std::string& key) {
        AudioRecord r;
        r.filePath = filePath;
        r.fileName = filePath.substr(filePath.find_last_of("/\\") + 1);
        r.extension = ".wav";
        r.format = "WAV";
        r.channels = 2;
        r.sampleRate = 48000;
        r.bitDepth = 24;
        r.durationSeconds = 1.0f;
        r.fileSize = 1024;
        r.totalFrames = 48000;
        r.folderPath = filePath.substr(0, filePath.find_last_of("/\\"));
        r.hasCover = true;
        r.embeddedCoverKey = key;
        return db.addFile(r);
    };

    const std::string keyA = "E|click_perf_a|100|image/jpeg";
    const std::string keyB = "E|click_perf_b|100|image/jpeg";
    const std::string keyC = "E|click_perf_c|100|image/jpeg";
    const std::string keyD = "E|click_perf_d|100|image/jpeg";
    if (!addEmbeddedFile("C:/lib/click_perf/a.wav", keyA)) ++failures;
    if (!addEmbeddedFile("C:/lib/click_perf/b.wav", keyB)) ++failures;
    if (!addEmbeddedFile("C:/lib/click_perf/c.wav", keyC)) ++failures;
    if (!addEmbeddedFile("C:/lib/click_perf/d.wav", keyD)) ++failures;

    const int64_t basePhash = 0x0123456789ABCDEFull;
    const int64_t hammingOne = basePhash ^ 0x1ll;
    const int64_t hammingTwo = basePhash ^ 0x3ll;
    const int64_t hammingThree = basePhash ^ 0x7ll;
    std::ostringstream registrySql;
    registrySql
        << "INSERT OR REPLACE INTO cover_cache_registry "
        << "(cover_key, rep_path, is_embedded, normalized_key, cover_phash, cover_phash_version) VALUES "
        << "('" << keyA << "', 'C:/lib/click_perf/a.wav', 1, '" << keyA << "', " << basePhash << ", 2), "
        << "('" << keyB << "', 'C:/lib/click_perf/b.wav', 1, '" << keyB << "', " << hammingOne << ", 2), "
        << "('" << keyC << "', 'C:/lib/click_perf/c.wav', 1, '" << keyC << "', " << hammingTwo << ", 2), "
        << "('" << keyD << "', 'C:/lib/click_perf/d.wav', 1, '" << keyD << "', " << hammingThree << ", 2);";
    char* err = nullptr;
    if (sqlite3_exec(db.db(), registrySql.str().c_str(), nullptr, nullptr, &err) != SQLITE_OK) {
        if (err) sqlite3_free(err);
        ++failures;
    }

    auto ids = db.getFileIdsForCoverTileMembership(keyA);
    const bool preservesHammingTwoCluster = ids.size() == 3;
    std::cout << (preservesHammingTwoCluster ? "PASS" : "FAIL")
              << ": cover click lookup expands to exact key plus Hamming-1/Hamming-2 neighbors only; ids="
              << ids.size() << "\n";
    if (!preservesHammingTwoCluster) ++failures;

    SearchFilter filter;
    filter.specificCoverKey = keyA;
    const int64_t filteredCount = db.searchFilesCount(filter);
    const bool searchMatchesCluster = filteredCount == 3;
    std::cout << (searchMatchesCluster ? "PASS" : "FAIL")
              << ": specificCoverKey search stays aligned with Hamming-2 tile cluster; count="
              << filteredCount << "\n";
    if (!searchMatchesCluster) ++failures;

    db.close();
    removeDbFamily(dbPath);
    return failures;
}

int testCoverClickPreparedFilterReuseSurvivesEquivalentCacheClear() {
    printSeparator("Test: cover click prepared filter reuse survives equivalent cache clear");

    const std::string dbPath = "test_cover_click_prepared_reuse.db";
    removeDbFamily(dbPath);

    Database db;
    if (!db.open(dbPath, true)) {
        std::cerr << "FAIL: open db: " << db.getLastError() << "\n";
        return 1;
    }

    int failures = 0;
    auto addEmbeddedFile = [&](const std::string& filePath, const std::string& key) {
        AudioRecord r;
        r.filePath = filePath;
        r.fileName = filePath.substr(filePath.find_last_of("/\\") + 1);
        r.extension = ".wav";
        r.format = "WAV";
        r.channels = 2;
        r.sampleRate = 48000;
        r.bitDepth = 24;
        r.durationSeconds = 1.0f;
        r.fileSize = 1024;
        r.totalFrames = 48000;
        r.folderPath = filePath.substr(0, filePath.find_last_of("/\\"));
        r.hasCover = true;
        r.embeddedCoverKey = key;
        return db.addFile(r);
    };

    const std::string keyA = "E|prepared_reuse_a|100|image/jpeg";
    const std::string keyB = "E|prepared_reuse_b|100|image/jpeg";
    if (!addEmbeddedFile("C:/lib/prepared_reuse/a.wav", keyA)) ++failures;
    if (!addEmbeddedFile("C:/lib/prepared_reuse/b.wav", keyB)) ++failures;

    const int64_t basePhash = 0x00ABCDEF12345678ll;
    const int64_t hammingOne = basePhash ^ 0x1ll;
    std::ostringstream registrySql;
    registrySql
        << "INSERT OR REPLACE INTO cover_cache_registry "
        << "(cover_key, rep_path, is_embedded, normalized_key, cover_phash, cover_phash_version) VALUES "
        << "('" << keyA << "', 'C:/lib/prepared_reuse/a.wav', 1, '" << keyA << "', " << basePhash << ", 2), "
        << "('" << keyB << "', 'C:/lib/prepared_reuse/b.wav', 1, '" << keyB << "', " << hammingOne << ", 2);";
    char* err = nullptr;
    if (sqlite3_exec(db.db(), registrySql.str().c_str(), nullptr, nullptr, &err) != SQLITE_OK) {
        if (err) sqlite3_free(err);
        ++failures;
    }

    auto ids = db.getFileIdsForCoverTileMembership(keyA);
    const bool clickPreparedBothIds = ids.size() == 2;
    std::cout << (clickPreparedBothIds ? "PASS" : "FAIL")
              << ": click-path membership lookup prepares both equivalent ids\n";
    if (!clickPreparedBothIds) ++failures;

    std::vector<std::string> preparedKeys;
    const bool preparedExists = db.getPreparedCoverFilterKeys(keyA, preparedKeys) && preparedKeys.size() == 2;
    std::cout << (preparedExists ? "PASS" : "FAIL")
              << ": click-path lookup stores reusable prepared cover filter keys\n";
    if (!preparedExists) ++failures;

    db.clearCoverKeyEquivalentCache();

    SearchFilter filter;
    filter.specificCoverKey = keyA;
    auto filtered = db.searchFiles(filter, 20, 0);
    const bool searchReusesPreparedKeys =
        filtered.size() == 2 &&
        std::all_of(filtered.begin(), filtered.end(), [&](const AudioRecord& record) {
            return record.filePath == "C:/lib/prepared_reuse/a.wav" ||
                   record.filePath == "C:/lib/prepared_reuse/b.wav";
        });
    std::cout << (searchReusesPreparedKeys ? "PASS" : "FAIL")
              << ": specificCoverKey search still resolves both files after equivalent cache clear\n";
    if (!searchReusesPreparedKeys) ++failures;

    db.close();
    removeDbFamily(dbPath);
    return failures;
}

int testCoverWallAggregationPrewarmsTileMembershipForVisibleEntries() {
    printSeparator("Test: cover wall aggregation prewarms tile membership for visible entries");

    const std::string dbPath = "test_cover_wall_prewarm_membership.db";
    removeDbFamily(dbPath);

    Database db;
    if (!db.open(dbPath, true)) {
        std::cerr << "FAIL: open db: " << db.getLastError() << "\n";
        return 1;
    }

    int failures = 0;
    auto addEmbeddedFile = [&](const std::string& filePath, const std::string& key) {
        AudioRecord r;
        r.filePath = filePath;
        r.fileName = filePath.substr(filePath.find_last_of("/\\") + 1);
        r.extension = ".wav";
        r.format = "WAV";
        r.channels = 2;
        r.sampleRate = 48000;
        r.bitDepth = 24;
        r.durationSeconds = 1.0f;
        r.fileSize = 1024;
        r.totalFrames = 48000;
        r.folderPath = filePath.substr(0, filePath.find_last_of("/\\"));
        r.hasCover = true;
        r.embeddedCoverKey = key;
        return db.addFile(r);
    };

    const std::string keyA = "E|wall_prewarm_a|100|image/jpeg";
    const std::string keyB = "E|wall_prewarm_b|100|image/jpeg";
    if (!addEmbeddedFile("C:/lib/wall_prewarm/a.wav", keyA)) ++failures;
    if (!addEmbeddedFile("C:/lib/wall_prewarm/b.wav", keyB)) ++failures;

    const int64_t basePhash = 0x0F0F0F0F0F0F0F0Fll;
    const int64_t hammingOne = basePhash ^ 0x1ll;
    std::ostringstream registrySql;
    registrySql
        << "INSERT OR REPLACE INTO cover_cache_registry "
        << "(cover_key, rep_path, is_embedded, normalized_key, cover_phash, cover_phash_version) VALUES "
        << "('" << keyA << "', 'C:/lib/wall_prewarm/a.wav', 1, '" << keyA << "', " << basePhash << ", 2), "
        << "('" << keyB << "', 'C:/lib/wall_prewarm/b.wav', 1, '" << keyB << "', " << hammingOne << ", 2);";
    char* err = nullptr;
    if (sqlite3_exec(db.db(), registrySql.str().c_str(), nullptr, nullptr, &err) != SQLITE_OK) {
        if (err) sqlite3_free(err);
        ++failures;
    }

    sqlite3_exec(db.db(), "DELETE FROM cover_tile_membership;", nullptr, nullptr, nullptr);
    db.clearPreparedCoverFilterKeys();
    db.clearCoverKeyEquivalentCache();

    auto page = db.getGlobalCoverWallEntriesPage({}, 10);
    const bool pageHasEntry = !page.entries.empty();
    std::cout << (pageHasEntry ? "PASS" : "FAIL")
              << ": cover wall page returns visible entries for membership prewarm\n";
    if (!pageHasEntry) ++failures;

    std::vector<std::string> preparedKeys;
    const bool preparedOnWallLoad =
        pageHasEntry &&
        db.getPreparedCoverFilterKeys(page.entries.front().key, preparedKeys) &&
        preparedKeys.size() == 2;
    std::cout << (preparedOnWallLoad ? "PASS" : "FAIL")
              << ": cover wall aggregation prewarms prepared cover filter keys for visible entry\n";
    if (!preparedOnWallLoad) ++failures;

    int membershipRows = 0;
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db.db(), "SELECT COUNT(*) FROM cover_tile_membership", -1, &stmt, nullptr) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            membershipRows = sqlite3_column_int(stmt, 0);
        }
        sqlite3_finalize(stmt);
    }
    const bool membershipPrewarmed = membershipRows >= 2;
    std::cout << (membershipPrewarmed ? "PASS" : "FAIL")
              << ": cover wall aggregation prewarms tile membership rows before first click; rows="
              << membershipRows << "\n";
    if (!membershipPrewarmed) ++failures;

    db.close();
    removeDbFamily(dbPath);
    return failures;
}

int testCoverTileMembershipPrefersCanonicalMembershipIndex() {
    printSeparator("Test: cover tile membership prefers canonical membership index");

    const std::string dbPath = "test_cover_tile_membership_canonical_index.db";
    removeDbFamily(dbPath);

    Database db;
    if (!db.open(dbPath)) {
        std::cerr << "FAIL: open db: " << db.getLastError() << "\n";
        return 1;
    }

    int failures = 0;
    AudioRecord r;
    r.filePath = "C:/lib/tile_membership_index/no_legacy_key.wav";
    r.fileName = "no_legacy_key.wav";
    r.extension = ".wav";
    r.format = "WAV";
    r.channels = 2;
    r.sampleRate = 48000;
    r.bitDepth = 24;
    r.durationSeconds = 1.0f;
    r.fileSize = 1024;
    r.totalFrames = 48000;
    r.folderPath = "C:/lib/tile_membership_index";
    r.hasCover = true;
    if (!db.addFile(r)) ++failures;

    int64_t fileId = 0;
    sqlite3_stmt* idStmt = nullptr;
    if (sqlite3_prepare_v2(db.db(), "SELECT id FROM audio_files WHERE file_path = ?", -1, &idStmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(idStmt, 1, r.filePath.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(idStmt) == SQLITE_ROW) {
            fileId = sqlite3_column_int64(idStmt, 0);
        }
        sqlite3_finalize(idStmt);
    }

    const std::string tileKey = "E|canonical_membership_only|100|image/jpeg";
    const char* registrySql =
        "INSERT OR REPLACE INTO cover_cache_registry "
        "(cover_key, rep_path, is_embedded, normalized_key, cover_phash, cover_phash_version) "
        "VALUES ('E|canonical_membership_only|100|image/jpeg', "
        "'C:/lib/tile_membership_index/no_legacy_key.wav', 1, "
        "'E|canonical_membership_only|100|image/jpeg', 0, 0);";
    char* registryErr = nullptr;
    if (sqlite3_exec(db.db(), registrySql, nullptr, nullptr, &registryErr) != SQLITE_OK) {
        if (registryErr) sqlite3_free(registryErr);
        ++failures;
    }

    sqlite3_stmt* membershipStmt = nullptr;
    if (sqlite3_prepare_v2(db.db(),
                           "INSERT OR REPLACE INTO cover_tile_membership "
                           "(tile_key, file_id, membership_kind) VALUES (?, ?, 1)",
                           -1, &membershipStmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(membershipStmt, 1, tileKey.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(membershipStmt, 2, fileId);
        if (sqlite3_step(membershipStmt) != SQLITE_DONE) ++failures;
        sqlite3_finalize(membershipStmt);
    } else {
        ++failures;
    }

    auto ids = db.getFileIdsForCoverTileMembership(tileKey);
    const bool canonicalIndexServesMembership =
        ids.size() == 1 && fileId > 0 && ids.front() == fileId;
    std::cout << (canonicalIndexServesMembership ? "PASS" : "FAIL")
              << ": cover tile membership reads canonical membership index before legacy audio cover-key fields\n";
    if (!canonicalIndexServesMembership) ++failures;

    db.close();
    removeDbFamily(dbPath);
    return failures;
}

int testSpecificCoverFilterPrefersCanonicalMembershipIndex() {
    printSeparator("Test: specificCoverKey filter prefers canonical membership index");

    const std::string dbPath = "test_cover_filter_canonical_membership_index.db";
    removeDbFamily(dbPath);

    Database db;
    if (!db.open(dbPath)) {
        std::cerr << "FAIL: open db: " << db.getLastError() << "\n";
        return 1;
    }

    int failures = 0;
    AudioRecord r;
    r.filePath = "C:/lib/cover_filter_membership/no_legacy_key.wav";
    r.fileName = "no_legacy_key.wav";
    r.extension = ".wav";
    r.format = "WAV";
    r.channels = 2;
    r.sampleRate = 48000;
    r.bitDepth = 24;
    r.durationSeconds = 1.0f;
    r.fileSize = 1024;
    r.totalFrames = 48000;
    r.folderPath = "C:/lib/cover_filter_membership";
    r.hasCover = true;
    if (!db.addFile(r)) ++failures;

    int64_t fileId = 0;
    sqlite3_stmt* idStmt = nullptr;
    if (sqlite3_prepare_v2(db.db(), "SELECT id FROM audio_files WHERE file_path = ?", -1, &idStmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(idStmt, 1, r.filePath.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(idStmt) == SQLITE_ROW) {
            fileId = sqlite3_column_int64(idStmt, 0);
        }
        sqlite3_finalize(idStmt);
    }

    const std::string tileKey = "E|cover_filter_membership_only|100|image/jpeg";
    sqlite3_exec(db.db(),
                 "INSERT OR REPLACE INTO cover_cache_registry "
                 "(cover_key, rep_path, is_embedded, normalized_key, cover_phash, cover_phash_version) "
                 "VALUES ('E|cover_filter_membership_only|100|image/jpeg', "
                 "'C:/lib/cover_filter_membership/no_legacy_key.wav', 1, "
                 "'E|cover_filter_membership_only|100|image/jpeg', 0, 0);",
                 nullptr,
                 nullptr,
                 nullptr);

    sqlite3_stmt* membershipStmt = nullptr;
    if (sqlite3_prepare_v2(db.db(),
                           "INSERT OR REPLACE INTO cover_tile_membership "
                           "(tile_key, file_id, membership_kind) VALUES (?, ?, 1)",
                           -1, &membershipStmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(membershipStmt, 1, tileKey.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(membershipStmt, 2, fileId);
        if (sqlite3_step(membershipStmt) != SQLITE_DONE) ++failures;
        sqlite3_finalize(membershipStmt);
    } else {
        ++failures;
    }

    SearchFilter coverFilter;
    coverFilter.specificCoverKey = tileKey;
    auto filtered = db.searchFiles(coverFilter, 20, 0);
    const bool coverFilterUsesMembershipIndex =
        filtered.size() == 1 && fileId > 0 && filtered.front().filePath == r.filePath;
    std::cout << (coverFilterUsesMembershipIndex ? "PASS" : "FAIL")
              << ": specificCoverKey filter resolves rows through canonical tile membership even when legacy audio cover-key fields are empty\n";
    if (!coverFilterUsesMembershipIndex) ++failures;

    const int64_t filteredCount = db.searchFilesCount(coverFilter);
    const bool countUsesMembershipIndex = filteredCount == 1;
    std::cout << (countUsesMembershipIndex ? "PASS" : "FAIL")
              << ": searchFilesCount follows canonical tile membership for specificCoverKey filters\n";
    if (!countUsesMembershipIndex) ++failures;

    db.close();
    removeDbFamily(dbPath);
    return failures;
}

int testSpecificCoverFilterSelfHealsLegacyMembershipRows() {
    printSeparator("Test: specificCoverKey filter self-heals legacy membership rows");

    const std::string dbPath = "test_cover_filter_self_heal_membership.db";
    removeDbFamily(dbPath);

    Database db;
    if (!db.open(dbPath)) {
        std::cerr << "FAIL: open db: " << db.getLastError() << "\n";
        return 1;
    }

    int failures = 0;
    AudioRecord r;
    r.filePath = "C:/lib/cover_filter_self_heal/legacy_only.wav";
    r.fileName = "legacy_only.wav";
    r.extension = ".wav";
    r.format = "WAV";
    r.channels = 2;
    r.sampleRate = 48000;
    r.bitDepth = 24;
    r.durationSeconds = 1.0f;
    r.fileSize = 1024;
    r.totalFrames = 48000;
    r.folderPath = "C:/lib/cover_filter_self_heal";
    r.hasCover = true;
    r.embeddedCoverKey = "E|cover_filter_self_heal|100|image/jpeg";
    if (!db.addFile(r)) ++failures;

    sqlite3_exec(db.db(), "DELETE FROM cover_tile_membership;", nullptr, nullptr, nullptr);

    SearchFilter coverFilter;
    coverFilter.specificCoverKey = r.embeddedCoverKey;
    auto filtered = db.searchFiles(coverFilter, 20, 0);
    const bool searchSelfHealed =
        filtered.size() == 1 && filtered.front().filePath == r.filePath;
    std::cout << (searchSelfHealed ? "PASS" : "FAIL")
              << ": specificCoverKey filter self-heals missing canonical membership rows before querying\n";
    if (!searchSelfHealed) ++failures;

    const int64_t filteredCount = db.searchFilesCount(coverFilter);
    const bool countSelfHealed = filteredCount == 1;
    std::cout << (countSelfHealed ? "PASS" : "FAIL")
              << ": searchFilesCount self-heals missing canonical membership rows for specificCoverKey filters\n";
    if (!countSelfHealed) ++failures;

    auto ids = db.getFileIdsForCoverTileMembership(r.embeddedCoverKey);
    const bool membershipRebuilt = ids.size() == 1;
    std::cout << (membershipRebuilt ? "PASS" : "FAIL")
              << ": specificCoverKey self-heal repopulates cover_tile_membership for the queried key\n";
    if (!membershipRebuilt) ++failures;

    db.close();
    removeDbFamily(dbPath);
    return failures;
}

int testCoverTileMembershipSelfHealsLegacyDatabaseRows() {
    printSeparator("Test: cover tile membership self-heals legacy database rows");

    const std::string dbPath = "test_cover_tile_membership_self_heal.db";
    removeDbFamily(dbPath);

    Database db;
    if (!db.open(dbPath)) {
        std::cerr << "FAIL: open db: " << db.getLastError() << "\n";
        return 1;
    }

    int failures = 0;
    AudioRecord r;
    r.filePath = "C:/lib/tile_membership_self_heal/legacy.wav";
    r.fileName = "legacy.wav";
    r.extension = ".wav";
    r.format = "WAV";
    r.channels = 2;
    r.sampleRate = 48000;
    r.bitDepth = 24;
    r.durationSeconds = 1.0f;
    r.fileSize = 1024;
    r.totalFrames = 48000;
    r.folderPath = "C:/lib/tile_membership_self_heal";
    r.hasCover = true;
    r.embeddedCoverKey = "E|self_heal_key|100|image/jpeg";
    if (!db.addFile(r)) ++failures;

    char* err = nullptr;
    if (sqlite3_exec(db.db(), "DELETE FROM cover_tile_membership;", nullptr, nullptr, &err) != SQLITE_OK) {
        if (err) sqlite3_free(err);
        ++failures;
    }

    auto ids = db.getFileIdsForCoverTileMembership("E|self_heal_key|100|image/jpeg");
    const bool returnsLegacyIds = ids.size() == 1;
    std::cout << (returnsLegacyIds ? "PASS" : "FAIL")
              << ": legacy database row still resolves through tile membership query during self-heal\n";
    if (!returnsLegacyIds) ++failures;

    int repairedRows = 0;
    sqlite3_stmt* countStmt = nullptr;
    if (sqlite3_prepare_v2(db.db(),
                           "SELECT COUNT(*) FROM cover_tile_membership WHERE tile_key = ?",
                           -1, &countStmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(countStmt, 1, "E|self_heal_key|100|image/jpeg", -1, SQLITE_TRANSIENT);
        if (sqlite3_step(countStmt) == SQLITE_ROW) {
            repairedRows = sqlite3_column_int(countStmt, 0);
        }
        sqlite3_finalize(countStmt);
    }
    const bool selfHealRebuiltMembership = repairedRows == 1;
    std::cout << (selfHealRebuiltMembership ? "PASS" : "FAIL")
              << ": tile membership query repopulates canonical membership rows for legacy databases before fallback retirement\n";
    if (!selfHealRebuiltMembership) ++failures;

    db.close();
    removeDbFamily(dbPath);
    return failures;
}

int testCoverTileMembershipMaintenanceBatchRepairsLegacyRows() {
    printSeparator("Test: cover tile membership maintenance batch repairs legacy rows");

    const std::string dbPath = "test_cover_tile_membership_maintenance_batch.db";
    removeDbFamily(dbPath);

    Database db;
    if (!db.open(dbPath)) {
        std::cerr << "FAIL: open db: " << db.getLastError() << "\n";
        return 1;
    }

    int failures = 0;
    for (int i = 0; i < 3; ++i) {
        AudioRecord r;
        r.filePath = "C:/lib/tile_membership_maintenance/file_" + std::to_string(i) + ".wav";
        r.fileName = "file_" + std::to_string(i) + ".wav";
        r.extension = ".wav";
        r.format = "WAV";
        r.channels = 2;
        r.sampleRate = 48000;
        r.bitDepth = 24;
        r.durationSeconds = 1.0f;
        r.fileSize = 1024;
        r.totalFrames = 48000;
        r.folderPath = "C:/lib/tile_membership_maintenance";
        r.hasCover = true;
        r.embeddedCoverKey = "E|maintenance_key_" + std::to_string(i) + "|100|image/jpeg";
        if (!db.addFile(r)) ++failures;
    }

    char* err = nullptr;
    if (sqlite3_exec(db.db(), "DELETE FROM cover_tile_membership;", nullptr, nullptr, &err) != SQLITE_OK) {
        if (err) sqlite3_free(err);
        ++failures;
    }
    sqlite3_exec(db.db(),
                 "INSERT OR REPLACE INTO app_meta (key, value) VALUES ('cover_tile_membership_needs_maintenance', '0');",
                 nullptr,
                 nullptr,
                 nullptr);

    const bool maintenancePending = db.isCoverTileMembershipMaintenancePending();
    std::cout << (maintenancePending ? "PASS" : "FAIL")
              << ": empty canonical membership index reports maintenance pending for legacy rows\n";
    if (!maintenancePending) ++failures;

    int totalRepaired = 0;
    int loops = 0;
    while (loops < 8) {
        const int repaired = db.repairCoverTileMembershipBatch(1);
        if (repaired <= 0) break;
        totalRepaired += repaired;
        ++loops;
    }
    // One extra drain pass clears the marker after the cursor reaches the end.
    (void)db.repairCoverTileMembershipBatch(1);

    int membershipRows = 0;
    sqlite3_stmt* countStmt = nullptr;
    if (sqlite3_prepare_v2(db.db(), "SELECT COUNT(*) FROM cover_tile_membership", -1, &countStmt, nullptr) == SQLITE_OK) {
        if (sqlite3_step(countStmt) == SQLITE_ROW) {
            membershipRows = sqlite3_column_int(countStmt, 0);
        }
        sqlite3_finalize(countStmt);
    } else {
        ++failures;
    }

    const bool maintenanceRecoveredRows = membershipRows == 3 && totalRepaired >= 3;
    std::cout << (maintenanceRecoveredRows ? "PASS" : "FAIL")
              << ": maintenance batch repopulates canonical membership rows incrementally; repaired="
              << totalRepaired << " rows=" << membershipRows << "\n";
    if (!maintenanceRecoveredRows) ++failures;

    const bool markerCleared = queryAppMetaInt(db, "cover_tile_membership_needs_maintenance") == 0;
    std::cout << (markerCleared ? "PASS" : "FAIL")
              << ": maintenance batch clears the legacy membership marker after draining pending rows\n";
    if (!markerCleared) ++failures;

    const bool maintenanceSettled = !db.isCoverTileMembershipMaintenancePending();
    std::cout << (maintenanceSettled ? "PASS" : "FAIL")
              << ": canonical membership maintenance settles after incremental repair\n";
    if (!maintenanceSettled) ++failures;

    db.close();
    removeDbFamily(dbPath);
    return failures;
}

int testCoverTileMembershipReopenRearmsMaintenanceMarker() {
    printSeparator("Test: cover tile membership reopen re-arms maintenance marker");

    const std::string dbPath = "test_cover_tile_membership_reopen_rearm.db";
    removeDbFamily(dbPath);

    {
        Database db;
        if (!db.open(dbPath)) {
            std::cerr << "FAIL: open db: " << db.getLastError() << "\n";
            return 1;
        }

        AudioRecord r;
        r.filePath = "C:/lib/tile_membership_reopen_rearm/legacy.wav";
        r.fileName = "legacy.wav";
        r.extension = ".wav";
        r.format = "WAV";
        r.channels = 2;
        r.sampleRate = 48000;
        r.bitDepth = 24;
        r.durationSeconds = 1.0f;
        r.fileSize = 1024;
        r.totalFrames = 48000;
        r.folderPath = "C:/lib/tile_membership_reopen_rearm";
        r.hasCover = true;
        r.embeddedCoverKey = "E|reopen_rearm_key|100|image/jpeg";
        if (!db.addFile(r)) {
            db.close();
            removeDbFamily(dbPath);
            return 1;
        }
        db.close();
    }

    sqlite3* rawDb = nullptr;
    if (sqlite3_open(dbPath.c_str(), &rawDb) != SQLITE_OK) {
        std::cerr << "FAIL: reopen raw sqlite to force membership maintenance marker\n";
        if (rawDb) sqlite3_close(rawDb);
        removeDbFamily(dbPath);
        return 1;
    }
    sqlite3_exec(rawDb, "DELETE FROM cover_tile_membership;", nullptr, nullptr, nullptr);
    sqlite3_exec(rawDb,
                 "INSERT OR REPLACE INTO app_meta (key, value) VALUES ('cover_tile_membership_needs_maintenance', '0');",
                 nullptr,
                 nullptr,
                 nullptr);
    sqlite3_close(rawDb);

    int failures = 0;
    Database reopened;
    if (!reopened.open(dbPath, true)) {
        std::cerr << "FAIL: reopen db: " << reopened.getLastError() << "\n";
        removeDbFamily(dbPath);
        return 1;
    }

    const bool markerRearmed = queryAppMetaInt(reopened, "cover_tile_membership_needs_maintenance") == 1;
    std::cout << (markerRearmed ? "PASS" : "FAIL")
              << ": reopen re-arms cover tile membership maintenance for legacy or empty membership state\n";
    if (!markerRearmed) ++failures;

    const bool maintenanceStillPending = reopened.isCoverTileMembershipMaintenancePending();
    std::cout << (maintenanceStillPending ? "PASS" : "FAIL")
              << ": reopen still reports cover tile membership maintenance pending after the marker is re-armed\n";
    if (!maintenanceStillPending) ++failures;

    reopened.close();
    removeDbFamily(dbPath);
    return failures;
}

int testCoverTileMembershipReadOnlyDoesNotReviveLegacyFallback() {
    printSeparator("Test: cover tile membership read-only open does not revive legacy fallback");

    const std::string dbPath = "test_cover_tile_membership_readonly_fallback.db";
    removeDbFamily(dbPath);

    {
        Database db;
        if (!db.open(dbPath)) {
            std::cerr << "FAIL: open db: " << db.getLastError() << "\n";
            return 1;
        }

        AudioRecord r;
        r.filePath = "C:/lib/tile_membership_readonly/legacy.wav";
        r.fileName = "legacy.wav";
        r.extension = ".wav";
        r.format = "WAV";
        r.channels = 2;
        r.sampleRate = 48000;
        r.bitDepth = 24;
        r.durationSeconds = 1.0f;
        r.fileSize = 1024;
        r.totalFrames = 48000;
        r.folderPath = "C:/lib/tile_membership_readonly";
        r.hasCover = true;
        r.embeddedCoverKey = "E|readonly_fallback_key|100|image/jpeg";
        if (!db.addFile(r)) {
            db.close();
            removeDbFamily(dbPath);
            return 1;
        }

        sqlite3_exec(db.db(), "DELETE FROM cover_tile_membership;", nullptr, nullptr, nullptr);
        db.close();
    }

    int failures = 0;
    Database readOnlyDb;
    if (!readOnlyDb.openReadOnly(dbPath)) {
        std::cerr << "FAIL: open read-only db: " << readOnlyDb.getLastError() << "\n";
        removeDbFamily(dbPath);
        return 1;
    }

    auto ids = readOnlyDb.getFileIdsForCoverTileMembership("E|readonly_fallback_key|100|image/jpeg");
    const bool readOnlyNoLongerRevivesLegacyFields = ids.empty();
    std::cout << (readOnlyNoLongerRevivesLegacyFields ? "PASS" : "FAIL")
              << ": read-only open no longer revives legacy audio cover-key fields when canonical membership rows are absent\n";
    if (!readOnlyNoLongerRevivesLegacyFields) ++failures;

    readOnlyDb.close();
    removeDbFamily(dbPath);
    return failures;
}

int testCountAudioCoverPresenceCandidatesUsesThreeWayCoverUnion() {
    printSeparator("Test: candidate count uses three-way cover union");

    const std::string dbPath = "test_cover_presence_candidates.db";
    removeDbFamily(dbPath);

    Database db;
    if (!db.open(dbPath)) {
        std::cerr << "FAIL: open db: " << db.getLastError() << "\n";
        return 1;
    }

    int failures = 0;
    auto insertFileWithCover = [&](const std::string& filePath,
                                   const std::string& folderPath,
                                   const std::string& embeddedKey,
                                   const std::string& folderKey,
                                   const std::string& folderSrcPath) {
        const char* sql =
            "INSERT INTO audio_files (file_path, file_name, extension, file_size, sample_rate, "
            "bit_depth, channels, total_frames, duration, format, folder_path, has_cover, "
            "embedded_cover_key, folder_cover_key, folder_cover_src_path) "
            "VALUES (?, ?, '.wav', 1024, 48000, 24, 2, 48000, 1.0, 'WAV', ?, 1, ?, ?, ?);";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db.db(), sql, -1, &stmt, nullptr) != SQLITE_OK) {
            return false;
        }

        const std::string fileName = filePath.substr(filePath.find_last_of("/\\") + 1);
        sqlite3_bind_text(stmt, 1, filePath.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, fileName.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, folderPath.c_str(), -1, SQLITE_TRANSIENT);
        if (embeddedKey.empty()) {
            sqlite3_bind_null(stmt, 4);
        } else {
            sqlite3_bind_text(stmt, 4, embeddedKey.c_str(), -1, SQLITE_TRANSIENT);
        }
        if (folderKey.empty()) {
            sqlite3_bind_null(stmt, 5);
        } else {
            sqlite3_bind_text(stmt, 5, folderKey.c_str(), -1, SQLITE_TRANSIENT);
        }
        if (folderSrcPath.empty()) {
            sqlite3_bind_null(stmt, 6);
        } else {
            sqlite3_bind_text(stmt, 6, folderSrcPath.c_str(), -1, SQLITE_TRANSIENT);
        }

        const bool ok = sqlite3_step(stmt) == SQLITE_DONE;
        sqlite3_finalize(stmt);
        return ok;
    };

    const std::string embeddedKey = "E|presence_embed_shared|100|image/jpeg";
    const std::string folderKey =
        CoverModule::CanonicalCoverKey("F|C:/lib/presence/folder_key/cover.jpg");
    const std::string implicitFolderA = "C:/lib/presence/implicit/A";
    const std::string implicitFolderB = "C:/lib/presence/implicit/B";

    if (!insertFileWithCover("C:/lib/presence/embed/a.wav", "C:/lib/presence/embed", embeddedKey, "", "")) ++failures;
    if (!insertFileWithCover("C:/lib/presence/embed/b.wav", "C:/lib/presence/embed", embeddedKey, "", "")) ++failures;
    if (!insertFileWithCover("C:/lib/presence/folder_key/a.wav", "C:/lib/presence/folder_key", "", folderKey, "C:/lib/presence/folder_key/cover.jpg")) ++failures;
    if (!insertFileWithCover("C:/lib/presence/folder_key/b.wav", "C:/lib/presence/folder_key", "", folderKey, "C:/lib/presence/folder_key/cover.jpg")) ++failures;
    if (!insertFileWithCover(implicitFolderA + "/a.wav", implicitFolderA, "", "", implicitFolderA + "/cover.jpg")) ++failures;
    if (!insertFileWithCover(implicitFolderA + "/b.wav", implicitFolderA, "", "", implicitFolderA + "/cover.jpg")) ++failures;
    if (!insertFileWithCover(implicitFolderB + "/c.wav", implicitFolderB, "", "", implicitFolderB + "/cover.jpg")) ++failures;

    const int64_t candidateCount = db.countAudioCoverPresenceCandidates();
    const bool usesThreeWayUnion = candidateCount == 4;
    std::cout << (usesThreeWayUnion ? "PASS" : "FAIL")
              << ": candidate count includes embedded, folder-key, and implicit has_cover identities; actual="
              << candidateCount << "\n";
    if (!usesThreeWayUnion) ++failures;

    db.close();
    removeDbFamily(dbPath);
    return failures;
}

int testGlobalCoverWallFallsBackWhenHasCoverRowsLackKeys() {
    printSeparator("Test: global cover wall falls back when has_cover rows lack keys");

    const std::string dbPath = "test_cover_wall_has_cover_without_keys.db";
    removeDbFamily(dbPath);

    Database db;
    if (!db.open(dbPath)) {
        std::cerr << "FAIL: open db: " << db.getLastError() << "\n";
        return 1;
    }

    int failures = 0;
    auto addFileWithImplicitFolderCover = [&](const std::string& folderPath, const std::string& fileName) {
        AudioRecord r;
        r.filePath = folderPath + "/" + fileName;
        r.fileName = fileName;
        r.extension = ".wav";
        r.format = "WAV";
        r.channels = 2;
        r.sampleRate = 48000;
        r.bitDepth = 24;
        r.durationSeconds = 1.0f;
        r.fileSize = 1024;
        r.totalFrames = 48000;
        r.folderPath = folderPath;
        r.hasCover = true;
        r.folderCoverSrcPath = folderPath + "/cover.jpg";
        return db.addFile(r);
    };

    const std::string folderA = "C:/lib/wall_implicit/A";
    const std::string folderB = "C:/lib/wall_implicit/B";
    if (!addFileWithImplicitFolderCover(folderA, "a.wav")) ++failures;
    if (!addFileWithImplicitFolderCover(folderB, "b.wav")) ++failures;

    auto wall = db.getGlobalCoverWallEntries();
    const bool fullWallCountVisible = wall.size() == 2;
    std::cout << (fullWallCountVisible ? "PASS" : "FAIL")
              << ": global wall surfaces has_cover folders even before keys are primed; actual="
              << wall.size() << "\n";
    if (!fullWallCountVisible) {
        for (const auto& entry : wall) {
            std::cout << "  wall key=" << entry.key
                      << " path=" << entry.path
                      << " source=" << entry.sourcePath
                      << " embedded=" << (entry.isEmbedded ? 1 : 0) << "\n";
        }
        ++failures;
    }

    bool allTempFolderKeys = wall.size() == 2;
    for (const auto& entry : wall) {
        if (entry.key.rfind("?|", 0) != 0 || entry.path.empty() || entry.isEmbedded) {
            allTempFolderKeys = false;
            break;
        }
    }
    std::cout << (allTempFolderKeys ? "PASS" : "FAIL")
              << ": global wall uses temporary folder identities for has_cover rows without keys\n";
    if (!allTempFolderKeys) ++failures;

    db.close();
    removeDbFamily(dbPath);
    return failures;
}

int testGlobalCoverFacetBypassesPersistentCacheWhenRegistrySparse() {
    printSeparator("Test: global cover facet bypasses stale cache when registry is sparse");

    const std::string dbPath = "test_cover_global_sparse_cache.db";
    removeDbFamily(dbPath);

    Database db;
    if (!db.open(dbPath)) {
        std::cerr << "FAIL: open db: " << db.getLastError() << "\n";
        return 1;
    }

    int failures = 0;
    auto addFileWithEmbeddedCover = [&](int i) {
        const std::string folder = "C:/lib/cache_sparse/" + std::to_string(i / 100);
        const std::string filePath = folder + "/file_" + std::to_string(i) + ".wav";
        AudioRecord r;
        r.filePath = filePath;
        r.fileName = "file_" + std::to_string(i) + ".wav";
        r.extension = ".wav";
        r.format = "WAV";
        r.channels = 2;
        r.sampleRate = 48000;
        r.bitDepth = 24;
        r.durationSeconds = 1.0f;
        r.fileSize = 1024 + i;
        r.totalFrames = 48000;
        r.folderPath = folder;
        r.hasCover = true;
        r.embeddedCoverKey = "E|cache_sparse_" + std::to_string(i) + "|100|image/jpeg";
        return db.addFile(r);
    };

    for (int i = 0; i < 600; ++i) {
        if (!addFileWithEmbeddedCover(i)) ++failures;
    }
    db.syncCoverRegistry();

    SearchFilter globalFilter;
    auto cachedFacets = db.getCoverFacets(globalFilter, 2000);
    const bool initialCacheBuilt = cachedFacets.size() == 600;
    std::cout << (initialCacheBuilt ? "PASS" : "FAIL")
              << ": initial global cover cache contains 600 entries; actual="
              << cachedFacets.size() << "\n";
    if (!initialCacheBuilt) ++failures;

    for (int i = 600; i < 1300; ++i) {
        if (!addFileWithEmbeddedCover(i)) ++failures;
    }

    auto refreshedFacets = db.getCoverFacets(globalFilter, 2000);
    const bool bypassedStaleCache = refreshedFacets.size() == 1300;
    std::cout << (bypassedStaleCache ? "PASS" : "FAIL")
              << ": sparse registry bypasses stale persistent cover cache; actual="
              << refreshedFacets.size() << "\n";
    if (!bypassedStaleCache) {
        ++failures;
    }

    db.close();
    removeDbFamily(dbPath);
    return failures;
}

int testSmallFilteredFacetQueriesPersistCacheEntries() {
    printSeparator("Test: small filtered facet queries persist cache entries");

    const std::string dbPath = "test_small_filtered_facet_cache.db";
    removeDbFamily(dbPath);

    Database db;
    if (!db.open(dbPath)) {
        std::cerr << "FAIL: open db: " << db.getLastError() << "\n";
        return 1;
    }

    int failures = 0;
    auto addDoorFile = [&](const std::string& filePath,
                           const std::string& folderPath,
                           const std::string& coverKey) {
        AudioRecord r;
        r.filePath = filePath;
        r.fileName = std::filesystem::path(filePath).filename().string();
        r.extension = ".wav";
        r.format = "WAV";
        r.channels = 2;
        r.sampleRate = 48000;
        r.bitDepth = 24;
        r.durationSeconds = 1.0f;
        r.fileSize = 1024;
        r.totalFrames = 48000;
        r.folderPath = folderPath;
        r.hasCover = true;
        r.folderCoverKey = coverKey;
        return db.addFile(r);
    };

    if (!addDoorFile("C:/lib/doors/metal/door_hit_01.wav", "C:/lib/doors/metal", "F|door_metal")) ++failures;
    if (!addDoorFile("C:/lib/doors/metal/door_hit_02.wav", "C:/lib/doors/metal", "F|door_metal")) ++failures;
    if (!addDoorFile("C:/lib/doors/wood/door_creak_01.wav", "C:/lib/doors/wood", "F|door_wood")) ++failures;
    if (!addDoorFile("C:/lib/whoosh/whoosh_01.wav", "C:/lib/whoosh", "F|whoosh")) ++failures;

    SearchFilter filter;
    filter.keyword = "door";

    auto initialCacheRows = db.executeRawQuery("SELECT COUNT(*) FROM facet_query_cache;");
    const int beforeCount =
        (initialCacheRows.success && !initialCacheRows.rows.empty() && !initialCacheRows.rows[0].empty())
            ? std::stoi(initialCacheRows.rows[0][0])
            : -1;

    auto coverFacets = db.getCoverFacets(filter, 20);
    auto pathFacets = db.getPathFacets(filter, 20);

    const bool smallFilteredResultsBuilt = coverFacets.size() == 2 && pathFacets.size() == 2;
    std::cout << (smallFilteredResultsBuilt ? "PASS" : "FAIL")
              << ": small filtered cover/path facet queries return expected rows\n";
    if (!smallFilteredResultsBuilt) ++failures;

    auto cachedRows = db.executeRawQuery("SELECT COUNT(*) FROM facet_query_cache;");
    const int afterCount =
        (cachedRows.success && !cachedRows.rows.empty() && !cachedRows.rows[0].empty())
            ? std::stoi(cachedRows.rows[0][0])
            : -1;
    const bool cacheRowsAdded = (beforeCount >= 0 && afterCount >= beforeCount + 2);
    std::cout << (cacheRowsAdded ? "PASS" : "FAIL")
              << ": small filtered cover/path facets persist cache entries; before="
              << beforeCount << " after=" << afterCount << "\n";
    if (!cacheRowsAdded) ++failures;

    db.close();
    removeDbFamily(dbPath);
    return failures;
}

int testSimilarityCacheRefreshesWhenFeatureRowsGrow() {
    printSeparator("Test: similarity cache refreshes after new feature rows");

    const std::string dbPath = "test_similarity_cache_refresh.db";
    removeDbFamily(dbPath);

    Database db;
    if (!db.open(dbPath)) {
        std::cerr << "FAIL: open db: " << db.getLastError() << "\n";
        return 1;
    }

    int failures = 0;
    AudioRecord first;
    first.filePath = "C:/sim/one.wav";
    first.fileName = "one.wav";
    first.extension = ".wav";
    first.format = "WAV";
    first.channels = 2;
    first.sampleRate = 48000;
    first.bitDepth = 24;
    first.durationSeconds = 1.0f;
    first.fileSize = 1024;
    first.totalFrames = 48000;
    first.folderPath = "C:/sim";

    AudioRecord second = first;
    second.filePath = "C:/sim/two.wav";
    second.fileName = "two.wav";

    if (!db.addFile(first)) ++failures;
    if (!db.addFile(second)) ++failures;
    if (!db.saveAudioFeaturesByPath(first.filePath, makeTestFeatures(0.1f))) ++failures;

    SimilaritySearch search;
    search.loadFeatureCache(db);
    const bool initialLoaded = search.isCacheLoaded() && search.cacheSize() == 1 &&
                               db.countFilesWithFeatures() == 1;
    std::cout << (initialLoaded ? "PASS" : "FAIL")
              << ": initial similarity cache contains one feature row\n";
    if (!initialLoaded) ++failures;

    if (!db.saveAudioFeaturesByPath(second.filePath, makeTestFeatures(0.9f))) ++failures;
    const int dbFeatureCount = db.countFilesWithFeatures();
    const bool staleDetected = dbFeatureCount != static_cast<int>(search.cacheSize());
    std::cout << (staleDetected ? "PASS" : "FAIL")
              << ": DB feature row count detects stale similarity cache\n";
    if (!staleDetected) ++failures;

    if (!search.isCacheLoaded() || dbFeatureCount != static_cast<int>(search.cacheSize())) {
        search.loadFeatureCache(db);
    }
    const bool refreshed = search.cacheSize() == 2 &&
                           search.cacheSize() == static_cast<size_t>(dbFeatureCount);
    std::cout << (refreshed ? "PASS" : "FAIL")
              << ": refreshed similarity cache includes new feature row\n";
    if (!refreshed) ++failures;

    db.close();
    removeDbFamily(dbPath);
    return failures;
}

int testQueryViewSnapshotPersistence() {
    int failures = 0;
    printSeparator("Query view snapshots are durable");

    const auto root = makeTempRoot("query_view_snapshot");
    const std::string dbPath = (root / "meta.db").string();
    removeDbFamily(dbPath);

    auto expect = [&failures](bool condition, const char* message) {
        std::cout << (condition ? "PASS" : "FAIL") << ": " << message << "\n";
        if (!condition) ++failures;
    };

    Database db;
    expect(db.open(dbPath, true), "database opens for query/view snapshot test");
    if (!db.isOpen()) {
        removeDbFamily(dbPath);
        std::error_code ec;
        fs::remove_all(root, ec);
        return failures;
    }

    auto tableRows = db.executeRawQuery(
        "SELECT name FROM sqlite_master WHERE type='table' AND name='query_view_snapshots';");
    expect(tableRows.success && tableRows.rows.size() == 1, "query_view_snapshots table is created");

    Database::QueryViewSnapshot saved;
    saved.snapshotKey = "last:global";
    saved.searchText = "door impact";
    saved.folderPath = "H:/SFX/BoomLibrary";
    saved.coverKey = "folder-cover:hash";
    saved.viewMode = 2;
    saved.sortColumn = 4;
    saved.sortAscending = false;
    saved.selectedFileId = 42;
    saved.scrollAnchorFileId = 41;
    saved.resultCount = 24740;
    saved.dataRevision = 12;
    saved.payloadJson = R"({"facet":"Category","zoom":1.25})";
    saved.updatedAt = 100;
    expect(db.saveQueryViewSnapshot(saved), "snapshot saves through prepared API");

    Database::QueryViewSnapshot loaded;
    expect(db.loadQueryViewSnapshot("last:global", loaded), "snapshot loads after save");
    expect(loaded.snapshotKey == saved.snapshotKey &&
               loaded.searchText == saved.searchText &&
               loaded.folderPath == saved.folderPath &&
               loaded.coverKey == saved.coverKey &&
               loaded.viewMode == saved.viewMode &&
               loaded.sortColumn == saved.sortColumn &&
               loaded.sortAscending == saved.sortAscending &&
               loaded.selectedFileId == saved.selectedFileId &&
               loaded.scrollAnchorFileId == saved.scrollAnchorFileId &&
               loaded.resultCount == saved.resultCount &&
               loaded.dataRevision == saved.dataRevision &&
               loaded.payloadJson == saved.payloadJson &&
               loaded.updatedAt == saved.updatedAt,
           "loaded snapshot preserves all lightweight fields");

    saved.searchText = "metal door";
    saved.resultCount = 88;
    saved.updatedAt = 200;
    expect(db.saveQueryViewSnapshot(saved), "snapshot upsert updates same key");
    expect(db.loadQueryViewSnapshot("last:global", loaded) &&
               loaded.searchText == "metal door" &&
               loaded.resultCount == 88 &&
               loaded.updatedAt == 200,
           "upserted snapshot is visible");

    db.close();

    Database reopened;
    expect(reopened.open(dbPath, true), "database reopens after snapshot save");
    expect(reopened.loadQueryViewSnapshot("last:global", loaded) &&
               loaded.searchText == "metal door" &&
               loaded.resultCount == 88,
           "snapshot survives close and reopen");

    Database::QueryViewSnapshot oldSnapshot = saved;
    oldSnapshot.snapshotKey = "old";
    oldSnapshot.updatedAt = 50;
    Database::QueryViewSnapshot newestSnapshot = saved;
    newestSnapshot.snapshotKey = "newest";
    newestSnapshot.updatedAt = 300;
    expect(reopened.saveQueryViewSnapshot(oldSnapshot), "older snapshot saves");
    expect(reopened.saveQueryViewSnapshot(newestSnapshot), "newer snapshot saves");
    const int pruned = reopened.pruneQueryViewSnapshots(1);
    expect(pruned == 2, "prune keeps only the newest snapshot");
    expect(!reopened.loadQueryViewSnapshot("old", loaded), "old snapshot is pruned");
    expect(!reopened.loadQueryViewSnapshot("last:global", loaded), "middle snapshot is pruned");
    expect(reopened.loadQueryViewSnapshot("newest", loaded) && loaded.updatedAt == 300,
           "newest snapshot remains after prune");

    expect(reopened.deleteQueryViewSnapshot("newest"), "snapshot delete succeeds");
    expect(!reopened.loadQueryViewSnapshot("newest", loaded), "deleted snapshot no longer loads");

    reopened.close();
    removeDbFamily(dbPath);
    std::error_code ec;
    fs::remove_all(root, ec);
    return failures;
}

int testActivityLogPersistence() {
    int failures = 0;
    printSeparator("Activity log is durable and bounded");

    const auto root = makeTempRoot("activity_log");
    const std::string dbPath = (root / "meta.db").string();
    removeDbFamily(dbPath);

    auto expect = [&failures](bool condition, const char* message) {
        std::cout << (condition ? "PASS" : "FAIL") << ": " << message << "\n";
        if (!condition) ++failures;
    };

    Database db;
    expect(db.open(dbPath, true), "database opens for activity log test");
    if (!db.isOpen()) {
        removeDbFamily(dbPath);
        std::error_code ec;
        fs::remove_all(root, ec);
        return failures;
    }

    auto tableRows = db.executeRawQuery(
        "SELECT name FROM sqlite_master WHERE type='table' AND name='activity_log';");
    expect(tableRows.success && tableRows.rows.size() == 1, "activity_log table is created");

    Database::ActivityLogEntry audition;
    audition.activityType = "audition";
    audition.fileId = 101;
    audition.filePath = "H:/SFX/Doors/door_hit_01.wav";
    audition.detailJson = R"({"source":"search","query":"door"})";
    audition.createdAt = 100;
    int64_t auditionId = 0;
    expect(db.appendActivityLog(audition, &auditionId) && auditionId > 0,
           "audition activity appends and returns an id");

    Database::ActivityLogEntry search = audition;
    search.activityType = "search";
    search.fileId = 0;
    search.filePath.clear();
    search.detailJson = R"({"query":"door","resultCount":24740})";
    search.createdAt = 200;
    expect(db.appendActivityLog(search), "search activity appends");

    Database::ActivityLogEntry transfer = audition;
    transfer.activityType = "transfer";
    transfer.fileId = 101;
    transfer.targetPath = "D:/Project/SFX/door_hit_01.wav";
    transfer.detailJson = R"({"method":"copy"})";
    transfer.createdAt = 300;
    expect(db.appendActivityLog(transfer), "transfer activity appends");

    auto recent = db.getRecentActivityLog({}, 10);
    expect(recent.size() == 3 &&
               recent[0].activityType == "transfer" &&
               recent[1].activityType == "search" &&
               recent[2].activityType == "audition",
           "recent activity returns newest first");
    auto transfers = db.getRecentActivityLog("transfer", 10);
    expect(transfers.size() == 1 &&
               transfers[0].targetPath == transfer.targetPath &&
               transfers[0].detailJson == transfer.detailJson,
           "activity type filter returns transfer metadata");
    expect(db.getRecentActivityLog({}, 0).empty(), "zero limit returns no activity rows");
    Database::ActivityLogEntry invalid;
    invalid.filePath = "H:/SFX/invalid.wav";
    expect(!db.appendActivityLog(invalid), "empty activity type is rejected");

    db.close();

    Database reopened;
    expect(reopened.open(dbPath, true), "database reopens after activity save");
    recent = reopened.getRecentActivityLog({}, 10);
    expect(recent.size() == 3 && recent[0].activityType == "transfer",
           "activity log survives close and reopen");
    const int pruned = reopened.pruneActivityLog(2);
    expect(pruned == 1, "prune keeps only the two newest activity rows");
    recent = reopened.getRecentActivityLog({}, 10);
    expect(recent.size() == 2 &&
               recent[0].activityType == "transfer" &&
               recent[1].activityType == "search",
           "oldest activity row is pruned");

    reopened.close();
    removeDbFamily(dbPath);
    std::error_code ec;
    fs::remove_all(root, ec);
    return failures;
}

int testNewPrimaryDatabaseUses64kPageSizeOnlyForFreshDb() {
    printSeparator("Test: fresh primary DB uses 64KB page size");

    const std::string freshDbPath = "test_page_size_fresh.db";
    removeDbFamily(freshDbPath);

    int failures = 0;
    {
        Database db;
        if (!db.open(freshDbPath, true)) {
            std::cerr << "FAIL: open fresh db: " << db.getLastError() << "\n";
            return 1;
        }

        const int pageSize = queryIntPragma(db, "PRAGMA main.page_size;");
        const bool uses64k = pageSize == 65536;
        std::cout << (uses64k ? "PASS" : "FAIL")
                  << ": fresh primary DB adopts page_size=65536; actual=" << pageSize << "\n";
        if (!uses64k) ++failures;
        db.close();
    }
    removeDbFamily(freshDbPath);

    const std::string legacyDbPath = "test_page_size_existing_legacy.db";
    removeDbFamily(legacyDbPath);
    const bool seededLegacyDb = execSqlOnDbPath(legacyDbPath, {
        "PRAGMA page_size = 4096;",
        "CREATE TABLE legacy_seed (id INTEGER PRIMARY KEY, value TEXT);",
        "INSERT INTO legacy_seed(value) VALUES ('ok');"
    });
    if (!seededLegacyDb) {
        std::cerr << "FAIL: seed legacy db with 4096 page size\n";
        removeDbFamily(legacyDbPath);
        return failures + 1;
    }

    {
        Database db;
        if (!db.open(legacyDbPath, true)) {
            std::cerr << "FAIL: open existing db: " << db.getLastError() << "\n";
            removeDbFamily(legacyDbPath);
            return failures + 1;
        }

        const int pageSize = queryIntPragma(db, "PRAGMA main.page_size;");
        const bool preservedLegacyPageSize = pageSize == 4096;
        std::cout << (preservedLegacyPageSize ? "PASS" : "FAIL")
                  << ": existing DB preserves prior page_size; actual=" << pageSize << "\n";
        if (!preservedLegacyPageSize) ++failures;
        db.close();
    }
    removeDbFamily(legacyDbPath);

    return failures;
}

int testFastScanPragmaModeRoundTripAndDirtyRecovery() {
    printSeparator("Test: fast scan pragma mode toggles and recovers");

    const std::string dbPath = "test_fast_scan_pragma_mode.db";
    removeDbFamily(dbPath);

    int failures = 0;
    {
        Database db;
        if (!db.open(dbPath, true)) {
            std::cerr << "FAIL: open db for fast scan toggle: " << db.getLastError() << "\n";
            return 1;
        }

        const bool enterOk = db.enterFastScanMode();
        std::cout << (enterOk ? "PASS" : "FAIL")
                  << ": enterFastScanMode succeeds on primary connection\n";
        if (!enterOk) ++failures;

        const bool enteredPragmas =
            queryTextPragma(db, "PRAGMA main.locking_mode;") == "exclusive" &&
            queryTextPragma(db, "PRAGMA main.journal_mode;") == "off" &&
            queryIntPragma(db, "PRAGMA main.synchronous;") == 0 &&
            queryAppMetaString(db, "scan_fast_mode_active") == "1";
        std::cout << (enteredPragmas ? "PASS" : "FAIL")
                  << ": fast scan mode flips locking/journal/synchronous and dirty flag\n";
        if (!enteredPragmas) ++failures;

        const bool exitOk = db.exitFastScanMode();
        std::cout << (exitOk ? "PASS" : "FAIL")
                  << ": exitFastScanMode restores normal pragma mode\n";
        if (!exitOk) ++failures;

        const bool restoredPragmas =
            queryTextPragma(db, "PRAGMA main.locking_mode;") == "normal" &&
            queryTextPragma(db, "PRAGMA main.journal_mode;") == "wal" &&
            queryIntPragma(db, "PRAGMA main.synchronous;") == 1 &&
            queryAppMetaString(db, "scan_fast_mode_active") == "0";
        std::cout << (restoredPragmas ? "PASS" : "FAIL")
                  << ": exiting fast scan mode restores WAL/NORMAL/NORMAL and clears dirty flag\n";
        if (!restoredPragmas) ++failures;

        const bool reentered = db.enterFastScanMode();
        std::cout << (reentered ? "PASS" : "FAIL")
                  << ": fast scan mode can be re-entered for interrupted session fixture\n";
        if (!reentered) ++failures;

        db.close();
    }

    {
        Database reopened;
        if (!reopened.open(dbPath, true)) {
            std::cerr << "FAIL: reopen db for dirty recovery: " << reopened.getLastError() << "\n";
            removeDbFamily(dbPath);
            return failures + 1;
        }

        const bool detectedInterrupted = reopened.wasFastScanInterrupted();
        std::cout << (detectedInterrupted ? "PASS" : "FAIL")
                  << ": reopen detects interrupted fast scan mode\n";
        if (!detectedInterrupted) ++failures;

        const bool recoveredPragmas =
            queryTextPragma(reopened, "PRAGMA main.locking_mode;") == "normal" &&
            queryTextPragma(reopened, "PRAGMA main.journal_mode;") == "wal" &&
            queryIntPragma(reopened, "PRAGMA main.synchronous;") == 1 &&
            queryAppMetaString(reopened, "scan_fast_mode_active") == "0";
        std::cout << (recoveredPragmas ? "PASS" : "FAIL")
                  << ": reopen recovers normal pragma mode and clears dirty flag\n";
        if (!recoveredPragmas) ++failures;

        reopened.close();
    }

    removeDbFamily(dbPath);
    return failures;
}

int testWaveformCompressionRoundTripAndLegacyRawCompatibility() {
    printSeparator("Test: waveform compression round-trip and legacy raw compatibility");

    const std::string dbPath = "test_waveform_compression.db";
    removeDbFamily(dbPath);

    Database db;
    if (!db.open(dbPath, true)) {
        std::cerr << "FAIL: open db: " << db.getLastError() << "\n";
        return 1;
    }

    int failures = 0;
    auto addAudioFile = [&](const std::string& filePath) {
        AudioRecord r;
        r.filePath = filePath;
        r.fileName = filePath.substr(filePath.find_last_of("/\\") + 1);
        r.extension = ".wav";
        r.format = "WAV";
        r.channels = 1;
        r.sampleRate = 48000;
        r.bitDepth = 24;
        r.durationSeconds = 1.0f;
        r.fileSize = 1024;
        r.totalFrames = 48000;
        r.folderPath = filePath.substr(0, filePath.find_last_of("/\\"));
        return db.addFile(r);
    };

    const std::string largePath = "C:/lib/waveform_compression/large.wav";
    const std::string smallPath = "C:/lib/waveform_compression/small.wav";
    const std::string rawPath = "C:/lib/waveform_compression/raw_legacy.wav";
    if (!addAudioFile(largePath)) ++failures;
    if (!addAudioFile(smallPath)) ++failures;
    if (!addAudioFile(rawPath)) ++failures;

    std::vector<float> largeWaveform(4000);
    for (int i = 0; i < 4000; ++i) {
        largeWaveform[(size_t)i] = std::sin((float)i * 0.01f) * 0.8f;
    }
    const bool storedLarge = db.saveWaveformByPath(largePath, largeWaveform, -20.0f, 1);
    std::cout << (storedLarge ? "PASS" : "FAIL")
              << ": high-density waveform save succeeds\n";
    if (!storedLarge) ++failures;

    float rmsOut = -200.0f;
    int miniChannels = 0;
    auto largeReadBack = db.getWaveformByPath(largePath, &rmsOut, &miniChannels);
    const bool largeRoundTrips =
        largeReadBack.size() == largeWaveform.size() &&
        std::abs(largeReadBack.front() - largeWaveform.front()) < 0.001f &&
        std::abs(largeReadBack.back() - largeWaveform.back()) < 0.001f &&
        std::abs(rmsOut - (-20.0f)) < 0.001f &&
        miniChannels == 1;
    std::cout << (largeRoundTrips ? "PASS" : "FAIL")
              << ": compressed high-density waveform round-trips transparently\n";
    if (!largeRoundTrips) ++failures;

    sqlite3_stmt* sizeStmt = nullptr;
    int largeStoredBytes = 0;
    const char* blobSizeSql =
        "SELECT length(w.waveform_data) "
        "FROM waveforms_db.waveforms w "
        "JOIN audio_files af ON af.id = w.file_id "
        "WHERE af.file_path = ? LIMIT 1";
    if (sqlite3_prepare_v2(db.db(), blobSizeSql, -1, &sizeStmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(sizeStmt, 1, largePath.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(sizeStmt) == SQLITE_ROW) {
            largeStoredBytes = sqlite3_column_int(sizeStmt, 0);
        }
        sqlite3_finalize(sizeStmt);
    }
    const bool largeBlobCompressed =
        largeStoredBytes > 0 && largeStoredBytes < (int)(largeWaveform.size() * sizeof(float));
    std::cout << (largeBlobCompressed ? "PASS" : "FAIL")
              << ": high-density waveform blob is stored smaller than raw; bytes="
              << largeStoredBytes << "\n";
    if (!largeBlobCompressed) ++failures;

    std::vector<float> smallWaveform(100);
    for (int i = 0; i < 100; ++i) {
        smallWaveform[(size_t)i] = (float)i / 100.0f;
    }
    const bool storedSmall = db.saveWaveformByPath(smallPath, smallWaveform, -18.0f, 1);
    std::cout << (storedSmall ? "PASS" : "FAIL")
              << ": low-density waveform save succeeds\n";
    if (!storedSmall) ++failures;

    int smallStoredBytes = 0;
    if (sqlite3_prepare_v2(db.db(), blobSizeSql, -1, &sizeStmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(sizeStmt, 1, smallPath.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(sizeStmt) == SQLITE_ROW) {
            smallStoredBytes = sqlite3_column_int(sizeStmt, 0);
        }
        sqlite3_finalize(sizeStmt);
    }
    const bool smallBlobStaysRaw =
        smallStoredBytes == (int)(smallWaveform.size() * sizeof(float));
    std::cout << (smallBlobStaysRaw ? "PASS" : "FAIL")
              << ": low-density waveform blob remains raw; bytes=" << smallStoredBytes << "\n";
    if (!smallBlobStaysRaw) ++failures;

    int64_t rawFileId = 0;
    sqlite3_stmt* idStmt = nullptr;
    if (sqlite3_prepare_v2(db.db(), "SELECT id FROM audio_files WHERE file_path = ?", -1, &idStmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(idStmt, 1, rawPath.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(idStmt) == SQLITE_ROW) {
            rawFileId = sqlite3_column_int64(idStmt, 0);
        }
        sqlite3_finalize(idStmt);
    }

    std::vector<float> legacyRaw = {0.25f, -0.5f, 0.75f, -0.125f};
    sqlite3_stmt* rawStmt = nullptr;
    const char* rawInsertSql =
        "INSERT OR REPLACE INTO waveforms_db.waveforms "
        "(file_id, waveform_data, status, error_code, retry_count, updated_at, true_rms_dbfs, mini_num_channels) "
        "VALUES (?, ?, 'done', '', 0, ?, ?, 1)";
    if (sqlite3_prepare_v2(db.db(), rawInsertSql, -1, &rawStmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(rawStmt, 1, rawFileId);
        sqlite3_bind_blob(rawStmt, 2, legacyRaw.data(), (int)(legacyRaw.size() * sizeof(float)), SQLITE_TRANSIENT);
        sqlite3_bind_int64(rawStmt, 3, 123456789);
        sqlite3_bind_double(rawStmt, 4, -12.0);
        if (sqlite3_step(rawStmt) != SQLITE_DONE) ++failures;
        sqlite3_finalize(rawStmt);
    } else {
        ++failures;
    }

    float rawRmsOut = -200.0f;
    int rawMiniChannels = 0;
    auto legacyReadBack = db.getWaveformByPath(rawPath, &rawRmsOut, &rawMiniChannels);
    const bool legacyStillReadable =
        legacyReadBack.size() == legacyRaw.size() &&
        std::abs(legacyReadBack[0] - legacyRaw[0]) < 0.0001f &&
        std::abs(legacyReadBack[3] - legacyRaw[3]) < 0.0001f &&
        std::abs(rawRmsOut - (-12.0f)) < 0.001f &&
        rawMiniChannels == 1;
    std::cout << (legacyStillReadable ? "PASS" : "FAIL")
              << ": legacy raw waveform blob remains readable without migration\n";
    if (!legacyStillReadable) ++failures;

    db.close();
    removeDbFamily(dbPath);
    return failures;
}

int testMiniMinDataRoundTripAndLegacyFallback() {
    printSeparator("Test: mini min-data round-trip and legacy fallback");

    const std::string dbPath = "test_mini_min_data.db";
    removeDbFamily(dbPath);

    Database db;
    if (!db.open(dbPath, true)) {
        std::cerr << "FAIL: open db: " << db.getLastError() << "\n";
        return 1;
    }

    int failures = 0;
    auto addAudioFile = [&](const std::string& filePath) {
        AudioRecord r;
        r.filePath = filePath;
        r.fileName = filePath.substr(filePath.find_last_of("/\\") + 1);
        r.extension = ".wav";
        r.format = "WAV";
        r.channels = 1;
        r.sampleRate = 48000;
        r.bitDepth = 24;
        r.durationSeconds = 1.0f;
        r.fileSize = 1024;
        r.totalFrames = 48000;
        r.folderPath = filePath.substr(0, filePath.find_last_of("/\\"));
        return db.addFile(r);
    };

    const std::string envPath = "C:/lib/mini_env/env.wav";
    const std::string legacyPath = "C:/lib/mini_env/legacy.wav";
    if (!addAudioFile(envPath)) ++failures;
    if (!addAudioFile(legacyPath)) ++failures;

    const std::vector<float> maxPeaks = {0.2f, 0.8f, 0.4f};
    const std::vector<float> minPeaks = {-0.1f, -0.6f, -0.3f};
    const bool stored = db.saveMiniWaveformEnvelopeByPath(envPath, maxPeaks, minPeaks, -24.0f, 1);
    std::cout << (stored ? "PASS" : "FAIL")
              << ": signed mini envelope save succeeds\n";
    if (!stored) ++failures;

    Database::MiniWaveformBundle bundle;
    const bool got = db.getMiniWaveformBundleByPath(envPath, bundle, 3);
    std::cout << (got ? "PASS" : "FAIL")
              << ": signed mini envelope bundle loads\n";
    if (!got) ++failures;

    const bool roundTrips =
        bundle.peaks == maxPeaks &&
        bundle.minPeaks == minPeaks &&
        std::abs(bundle.trueRmsDbfs - (-24.0f)) < 0.001f &&
        bundle.miniNumChannels == 1;
    std::cout << (roundTrips ? "PASS" : "FAIL")
              << ": signed mini envelope round-trips max/min blobs\n";
    if (!roundTrips) ++failures;

    sqlite3_stmt* sizeStmt = nullptr;
    int minBlobBytes = 0;
    const char* blobSizeSql =
        "SELECT length(w.mini_min_data) "
        "FROM waveforms_db.waveforms w "
        "JOIN audio_files af ON af.id = w.file_id "
        "WHERE af.file_path = ? LIMIT 1";
    if (sqlite3_prepare_v2(db.db(), blobSizeSql, -1, &sizeStmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(sizeStmt, 1, envPath.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(sizeStmt) == SQLITE_ROW) {
            minBlobBytes = sqlite3_column_int(sizeStmt, 0);
        }
        sqlite3_finalize(sizeStmt);
    }
    const bool minBlobExists = minBlobBytes > 0;
    std::cout << (minBlobExists ? "PASS" : "FAIL")
              << ": mini_min_data blob stored in sidecar; bytes=" << minBlobBytes << "\n";
    if (!minBlobExists) ++failures;

    const bool legacyStored = db.saveWaveformByPath(legacyPath, maxPeaks, -18.0f, 1);
    std::cout << (legacyStored ? "PASS" : "FAIL")
              << ": legacy max-only mini waveform save succeeds\n";
    if (!legacyStored) ++failures;

    Database::MiniWaveformBundle legacyBundle;
    const bool gotLegacy = db.getMiniWaveformBundleByPath(legacyPath, legacyBundle, 3);
    std::cout << (gotLegacy ? "PASS" : "FAIL")
              << ": legacy max-only mini waveform bundle loads\n";
    if (!gotLegacy) ++failures;

    const bool legacyFallsBack =
        legacyBundle.peaks == maxPeaks &&
        legacyBundle.minPeaks.empty();
    std::cout << (legacyFallsBack ? "PASS" : "FAIL")
              << ": legacy max-only rows keep empty minPeaks fallback\n";
    if (!legacyFallsBack) ++failures;

    db.close();
    removeDbFamily(dbPath);
    return failures;
}

int testAdaptiveMiniWaveformFormatUpgradeStamp() {
    printSeparator("Test: adaptive mini-waveform format upgrade stamp");

    const std::string dbPath = "test_mini_waveform_format_v2.db";
    removeDbFamily(dbPath);

    int failures = 0;
    {
        Database freshDb;
        if (!freshDb.open(dbPath, true)) {
            std::cerr << "FAIL: open fresh db: " << freshDb.getLastError() << "\n";
            return 1;
        }

        const bool freshAdaptive = freshDb.usesAdaptiveMiniWaveform();
        std::cout << (freshAdaptive ? "PASS" : "FAIL")
                  << ": fresh database uses adaptive mini density\n";
        if (!freshAdaptive) ++failures;

        const bool freshDoesNotNeedUpgrade = !freshDb.needsAdaptiveMiniWaveformFormatUpgrade();
        std::cout << (freshDoesNotNeedUpgrade ? "PASS" : "FAIL")
                  << ": fresh database stamps mini_waveform_format_v2 immediately\n";
        if (!freshDoesNotNeedUpgrade) ++failures;

        auto deleteStamp = freshDb.executeRawQuery(
            "DELETE FROM app_meta WHERE key = 'mini_waveform_format_v2';");
        const bool deletedStamp = deleteStamp.success;
        std::cout << (deletedStamp ? "PASS" : "FAIL")
                  << ": test fixture can clear mini_waveform_format_v2 stamp\n";
        if (!deletedStamp) ++failures;

        freshDb.close();
    }

    {
        Database reopenedDb;
        if (!reopenedDb.open(dbPath, true)) {
            std::cerr << "FAIL: reopen db: " << reopenedDb.getLastError() << "\n";
            removeDbFamily(dbPath);
            return failures + 1;
        }

        const bool nowNeedsUpgrade = reopenedDb.needsAdaptiveMiniWaveformFormatUpgrade();
        std::cout << (nowNeedsUpgrade ? "PASS" : "FAIL")
                  << ": adaptive database missing stamp requests one-time format rebuild\n";
        if (!nowNeedsUpgrade) ++failures;

        const bool stamped = reopenedDb.markAdaptiveMiniWaveformFormatUpgradeComplete();
        std::cout << (stamped ? "PASS" : "FAIL")
                  << ": markAdaptiveMiniWaveformFormatUpgradeComplete succeeds\n";
        if (!stamped) ++failures;

        const bool stampClearsNeed = !reopenedDb.needsAdaptiveMiniWaveformFormatUpgrade();
        std::cout << (stampClearsNeed ? "PASS" : "FAIL")
                  << ": completing the stamp clears the pending-upgrade state in memory\n";
        if (!stampClearsNeed) ++failures;

        reopenedDb.close();
    }

    {
        Database reopenedStampedDb;
        if (!reopenedStampedDb.open(dbPath, true)) {
            std::cerr << "FAIL: reopen stamped db: " << reopenedStampedDb.getLastError() << "\n";
            removeDbFamily(dbPath);
            return failures + 1;
        }

        const bool stampedDbStaysClean = !reopenedStampedDb.needsAdaptiveMiniWaveformFormatUpgrade();
        std::cout << (stampedDbStaysClean ? "PASS" : "FAIL")
                  << ": stamped adaptive database does not re-request the rebuild on reopen\n";
        if (!stampedDbStaysClean) ++failures;

        reopenedStampedDb.close();
    }

    removeDbFamily(dbPath);
    return failures;
}

int testTagFacetIndexPrestoreAndFallback() {
    printSeparator("Test: tag facet index pre-store and fallback");

    const std::string dbPath = "test_tag_facet_index.db";
    removeDbFamily(dbPath);

    Database db;
    if (!db.open(dbPath, true)) {
        std::cerr << "FAIL: open db: " << db.getLastError() << "\n";
        return 1;
    }

    int failures = 0;
    auto insertTaggedFile = [&](const std::string& filePath,
                                const std::string& normGenre,
                                const std::string& normInstruments) {
        const char* sql =
            "INSERT INTO audio_files (file_path, file_name, extension, format, channels, sample_rate, bit_depth, "
            "duration, file_size, total_frames, folder_path, norm_genre, norm_instruments) "
            "VALUES (?, ?, '.wav', 'WAV', 2, 48000, 24, 1.0, 1024, 48000, ?, ?, ?)";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db.db(), sql, -1, &stmt, nullptr) != SQLITE_OK) {
            return false;
        }
        const std::string fileName = filePath.substr(filePath.find_last_of("/\\") + 1);
        const std::string folderPath = filePath.substr(0, filePath.find_last_of("/\\"));
        sqlite3_bind_text(stmt, 1, filePath.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, fileName.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, folderPath.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 4, normGenre.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 5, normInstruments.c_str(), -1, SQLITE_TRANSIENT);
        const bool ok = sqlite3_step(stmt) == SQLITE_DONE;
        sqlite3_finalize(stmt);
        return ok;
    };

    if (!insertTaggedFile("C:/lib/tagfacet/a.wav", "Rock; Metal", "Guitar; Drums")) ++failures;
    if (!insertTaggedFile("C:/lib/tagfacet/b.wav", "Rock; Pop", "Synth")) ++failures;
    if (!insertTaggedFile("C:/lib/tagfacet/c.wav", "Metal", "Guitar")) ++failures;

    const bool rebuilt = db.rebuildTagFacetIndex();
    std::cout << (rebuilt ? "PASS" : "FAIL")
              << ": rebuildTagFacetIndex succeeds on populated library\n";
    if (!rebuilt) ++failures;

    auto rawRows = db.executeRawQuery(
        "SELECT value, weighting FROM tag_facet_index "
        "WHERE field = 'norm_genre' ORDER BY weighting DESC, value ASC;");
    const bool indexRowsCorrect =
        rawRows.success &&
        rawRows.rows.size() == 3 &&
        rawRows.rows[0][0] == "Metal" &&
        rawRows.rows[0][1] == "2" &&
        rawRows.rows[1][0] == "Rock" &&
        rawRows.rows[1][1] == "2" &&
        rawRows.rows[2][0] == "Pop" &&
        rawRows.rows[2][1] == "1";
    std::cout << (indexRowsCorrect ? "PASS" : "FAIL")
              << ": tag_facet_index stores expected norm_genre weighting rows\n";
    if (!indexRowsCorrect) ++failures;

    SearchFilter emptyFilter;
    auto genreFacets = db.getTagFacets("norm_genre", emptyFilter, 10);
    const bool fastPathResultsCorrect =
        genreFacets.size() == 3 &&
        genreFacets[0].tag == "Metal" && genreFacets[0].count == 2 &&
        genreFacets[1].tag == "Rock" && genreFacets[1].count == 2 &&
        genreFacets[2].tag == "Pop" && genreFacets[2].count == 1;
    std::cout << (fastPathResultsCorrect ? "PASS" : "FAIL")
              << ": getTagFacets serves global norm_genre counts from pre-stored index\n";
    if (!fastPathResultsCorrect) ++failures;

    const bool validBeforeInvalidation = db.isTagFacetIndexValid();
    std::cout << (validBeforeInvalidation ? "PASS" : "FAIL")
              << ": rebuilt tag facet index is marked valid\n";
    if (!validBeforeInvalidation) ++failures;

    const bool invalidated = db.invalidateFacetQueryCache() && !db.isTagFacetIndexValid();
    std::cout << (invalidated ? "PASS" : "FAIL")
              << ": facet cache invalidation also marks tag facet index stale\n";
    if (!invalidated) ++failures;

    auto fallbackGenreFacets = db.getTagFacets("norm_genre", emptyFilter, 10);
    const bool fallbackStillCorrect =
        fallbackGenreFacets.size() == 3 &&
        fallbackGenreFacets[0].tag == "Metal" && fallbackGenreFacets[0].count == 2 &&
        fallbackGenreFacets[1].tag == "Rock" && fallbackGenreFacets[1].count == 2 &&
        fallbackGenreFacets[2].tag == "Pop" && fallbackGenreFacets[2].count == 1;
    std::cout << (fallbackStillCorrect ? "PASS" : "FAIL")
              << ": stale tag facet index falls back to runtime tokenization without changing counts\n";
    if (!fallbackStillCorrect) ++failures;

    db.close();
    removeDbFamily(dbPath);
    return failures;
}

int testDatabaseBackupRestorePalBehavior() {
    int failures = 0;
    const fs::path root = fs::temp_directory_path() / "aris_database_backup_restore_pal";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);

    const auto writeText = [](const fs::path& path, const char* text) {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << text;
    };
    const auto readText = [](const fs::path& path) {
        std::ifstream in(path, std::ios::binary);
        std::ostringstream text;
        text << in.rdbuf();
        return text.str();
    };

    const fs::path source = root / fs::u8path(u8"source-数据库.db");
    const fs::path backup = root / fs::u8path(u8"backup-备份.db");
    writeText(source, "main-v1");
    writeText(fs::u8path(source.u8string() + "-wal"), "wal-v1");
    writeText(fs::u8path(source.u8string() + "-shm"), "shm-v1");
    writeText(backup, "stale");

    std::string errorMessage = "keep-on-success";
    const bool backedUp = Database::backupDatabase(
        source.u8string(), backup.u8string(), errorMessage);
    const bool backupMatches =
        readText(backup) == "main-v1" &&
        readText(fs::u8path(backup.u8string() + "-wal")) == "wal-v1" &&
        readText(fs::u8path(backup.u8string() + "-shm")) == "shm-v1";
    if (!backedUp || !backupMatches || errorMessage != "keep-on-success") {
        std::cerr << "FAIL: database backup should overwrite main and sidecars without clearing prior error text\n";
        ++failures;
    }

    writeText(source, "current-v2");
    writeText(backup, "restored-v1");
    errorMessage = "keep-on-success";
    const bool restored = Database::restoreDatabase(
        backup.u8string(), source.u8string(), errorMessage);
    if (!restored || readText(source) != "restored-v1" ||
        readText(fs::u8path(source.u8string() + ".pre-restore")) != "current-v2" ||
        errorMessage != "keep-on-success") {
        std::cerr << "FAIL: database restore should preserve safety copy and success error text\n";
        ++failures;
    }

    errorMessage.clear();
    if (Database::backupDatabase((root / "missing.db").u8string(),
                                 (root / "unused.db").u8string(), errorMessage) ||
        errorMessage.empty()) {
        std::cerr << "FAIL: required database backup copy failure should report an error\n";
        ++failures;
    }

    fs::remove_all(root, ec);
    return failures;
}

int testWindowsPathCaseVariantsDoNotDuplicateRows() {
#ifndef _WIN32
    return 0;
#else
    int failures = 0;
    const std::string dbPath = "test_windows_path_case_dedup.db";
    removeDbFamily(dbPath);

    Database db;
    if (!db.open(dbPath)) {
        std::cerr << "FAIL: open path-case dedup database: " << db.getLastError() << "\n";
        return 1;
    }

    AudioRecord first;
    first.filePath = "H:/Sound effect/SoundMorph/Hit.wav";
    first.fileName = "Hit.wav";
    first.folderPath = "H:/Sound effect/SoundMorph";
    first.extension = ".wav";
    first.fileSize = 1024;

    AudioRecord variant = first;
    variant.filePath = "h:/sound effect/soundmorph/hit.wav";
    variant.fileName = "hit.wav";
    variant.folderPath = "h:/sound effect/soundmorph";

    if (!db.addFile(first)) ++failures;
    if (!db.addFile(variant)) ++failures;

    const auto count = db.executeRawQuery("SELECT COUNT(*) FROM audio_files;");
    const bool oneRow = count.success && count.rows.size() == 1 &&
                        count.rows[0].size() == 1 && count.rows[0][0] == "1";
    std::cout << (oneRow ? "PASS" : "FAIL")
              << ": Windows file-path case variants share one database row\n";
    if (!oneRow) ++failures;

    db.close();
    removeDbFamily(dbPath);
    return failures;
#endif
}

int testWindowsPathCaseVariantRescanKeepsFileIdTracking() {
#ifndef _WIN32
    return 0;
#else
    int failures = 0;
    const fs::path root = makeTempRoot("windows_path_case_rescan_tracking");
    const fs::path audioPath = root / "Sample.wav";
    writeSilentMonoWav(audioPath);
    const std::string dbPath = (root / "case_rescan.db").u8string();
    const std::string originalPath = root.u8string();
    std::string caseVariantPath = originalPath;
    for (char& c : caseVariantPath) {
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - ('a' - 'A'));
    }

    Database db;
    if (!db.open(dbPath)) {
        std::cerr << "FAIL: open path-case rescan tracking database: "
                  << db.getLastError() << "\n";
        fs::remove_all(root);
        return 1;
    }

    const auto first = db.scanDirectoryEx(
        originalPath, false, true, false, nullptr, nullptr, 1,
        false, false);
    const auto second = db.scanDirectoryEx(
        caseVariantPath, false, true, false, nullptr, nullptr, 1,
        false, false);
    const auto count = db.executeRawQuery("SELECT COUNT(*) FROM audio_files;");
    const bool oneRow = count.success && count.rows.size() == 1 &&
                        count.rows[0].size() == 1 && count.rows[0][0] == "1";
    const bool noTrackingFailure = second.errors == 0 &&
                                   second.failedFiles == 0 &&
                                   second.errorMessages.empty();
    const bool passed = first.errors == 0 && first.failedFiles == 0 &&
                        noTrackingFailure && oneRow;
    std::cout << (passed ? "PASS" : "FAIL")
              << ": case-variant rescan preserves file-id tracking and one row"
              << " (firstErrors=" << first.errors
              << ", secondErrors=" << second.errors
              << ", secondFailed=" << second.failedFiles
              << ", rows=" << (oneRow ? 1 : -1) << ")\n";
    if (!passed) ++failures;

    db.close();
    fs::remove_all(root);
    return failures;
#endif
}

static int testReadOnlyPinyinRecomputeStops() {
    const fs::path root = fs::temp_directory_path() / "aris-readonly-pinyin-recompute";
    fs::remove_all(root);
    fs::create_directories(root);
    const std::string dbPath = (root / "library.db").string();
    int failures = 0;
    {
        Database writer;
        if (!writer.open(dbPath, true)) {
            std::cout << "FAIL: could not create writable library\n";
            fs::remove_all(root);
            return 1;
        }
        for (int i = 1; i <= 3; ++i) {
            writer.executeRawQuery(
                "INSERT INTO audio_files(file_path, file_name) VALUES('C:/lib/" +
                std::to_string(i) + ".wav', '大笑.wav');");
        }
        writer.executeRawQuery(
            "INSERT INTO app_meta(key, value) VALUES('search_data_migrated', '0') "
            "ON CONFLICT(key) DO UPDATE SET value = '0';");
        writer.executeRawQuery(
            "CREATE TRIGGER audio_files_readonly_guard BEFORE UPDATE ON audio_files "
            "BEGIN SELECT RAISE(FAIL, 'audio_files is read only'); END;");
        writer.close();
    }
    Database reader;
    if (!reader.open(dbPath, false)) {
        std::cout << "FAIL: could not reopen library\n";
        fs::remove_all(root);
        return 1;
    }
    std::atomic<float> progress{-2.0f};
    std::atomic<bool> cancel{false};
    const auto started = std::chrono::steady_clock::now();
    reader.rebuildFtsIndexBatched(64, &progress, &cancel);
    const auto elapsed = std::chrono::steady_clock::now() - started;
    const auto marker = reader.executeRawQuery(
        "SELECT value FROM app_meta WHERE key = 'search_data_migrated';");
    const auto fts = reader.executeRawQuery(
        "SELECT sql FROM sqlite_master WHERE name = 'audio_files_fts';");
    reader.close();
    const bool markerUnchanged = marker.rows.size() == 1 && marker.rows[0].size() == 1 &&
                                 marker.rows[0][0] == "0";
    const bool originalIndexKept = fts.rows.size() == 1 && fts.rows[0].size() == 1 &&
                                   fts.rows[0][0].find("content=") != std::string::npos;
    const bool stopped = elapsed < std::chrono::seconds(3) && progress.load() < 0.0f &&
                         markerUnchanged && originalIndexKept;
    std::cout << (stopped ? "PASS" : "FAIL")
              << ": read-only pinyin recompute stops on the failed write"
              << " (progress=" << progress.load()
              << ", marker=" << (marker.rows.empty() ? "none" : marker.rows[0][0]) << ")\n";
    if (!stopped) ++failures;
    fs::remove_all(root);
    return failures;
}

int testReadOnlyOpenDoesNotCreateLegacySidecars() {
    const fs::path root = fs::temp_directory_path() /
        ("harken-readonly-sidecars-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    const fs::path oldDb = root / "Local" / "Auro" / "auro.db";
    fs::create_directories(oldDb.parent_path());
    sqlite3* seed = nullptr;
    const std::string dbPath = PathToUtf8(oldDb);
    if (sqlite3_open_v2(dbPath.c_str(), &seed,
                        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) != SQLITE_OK) {
        if (seed) sqlite3_close(seed);
        return 1;
    }
    const bool seeded = sqlite3_exec(seed, "CREATE TABLE legacy(id INTEGER);", nullptr,
                                     nullptr, nullptr) == SQLITE_OK;
    sqlite3_close(seed);
    if (!seeded) return 1;

    auto files = [&]() {
        std::vector<std::string> names;
        for (const auto& entry : fs::directory_iterator(oldDb.parent_path())) {
            names.push_back(PathToUtf8(entry.path().filename()));
        }
        std::sort(names.begin(), names.end());
        return names;
    };
    const auto before = files();
    Database db;
    const bool opened = db.openReadOnly(dbPath);
    db.close();
    const bool ok = opened && files() == before && before.size() == 1 &&
                    before.front() == "auro.db";
    if (!ok) std::cerr << "FAIL: read-only open created legacy sidecar files\n";
    const fs::path writablePath = root / "Writable" / "Auro.db";
    fs::create_directories(writablePath.parent_path());
    Database writable;
    const bool writableOpened = writable.open(PathToUtf8(writablePath), true);
    writable.close();
    auto hasPersistedTable = [](const fs::path& path, const char* tableName) {
        sqlite3* sidecar = nullptr;
        if (sqlite3_open_v2(PathToUtf8(path).c_str(), &sidecar,
                            SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
            if (sidecar) sqlite3_close(sidecar);
            return false;
        }
        sqlite3_stmt* stmt = nullptr;
        const bool prepared = sqlite3_prepare_v2(
            sidecar, "SELECT 1 FROM sqlite_master WHERE type='table' AND name=?", -1,
            &stmt, nullptr) == SQLITE_OK;
        if (prepared) sqlite3_bind_text(stmt, 1, tableName, -1, SQLITE_TRANSIENT);
        const bool found = prepared && sqlite3_step(stmt) == SQLITE_ROW;
        if (stmt) sqlite3_finalize(stmt);
        sqlite3_close(sidecar);
        return found;
    };
    const bool persisted = writableOpened &&
        hasPersistedTable(writablePath.parent_path() / "Auro_blobs.db", "cover_thumbs") &&
        hasPersistedTable(writablePath.parent_path() / "Auro_waveforms.db", "waveforms");
    if (!persisted) std::cerr << "FAIL: writable open did not persist new sidecars\n";
    Database existingSidecars;
    const bool reopened = persisted && existingSidecars.openReadOnly(PathToUtf8(writablePath));
    auto canRead = [&](const char* sql) {
        sqlite3_stmt* stmt = nullptr;
        const bool prepared = reopened &&
            sqlite3_prepare_v2(existingSidecars.db(), sql, -1, &stmt, nullptr) == SQLITE_OK;
        const bool readable = prepared && sqlite3_step(stmt) == SQLITE_ROW;
        if (stmt) sqlite3_finalize(stmt);
        return readable;
    };
    const bool existingReadable = canRead("SELECT COUNT(*) FROM blobs_db.cover_thumbs") &&
        canRead("SELECT COUNT(*) FROM waveforms_db.waveforms");
    const bool noSidecarWrite = reopened && sqlite3_exec(
        existingSidecars.db(), "INSERT INTO blobs_db.cover_thumbs(cover_key) VALUES('forbidden')",
        nullptr, nullptr, nullptr) != SQLITE_OK;
    existingSidecars.close();
    if (!existingReadable || !noSidecarWrite) {
        std::cerr << "FAIL: existing sidecars were not readable and write-protected\n";
    }
    return ok && persisted && existingReadable && noSidecarWrite ? 0 : 1;
}

int testReadOnlyOpenSurvivesCorruptRebuildableSidecars() {
    const fs::path root = fs::temp_directory_path() /
        ("harken-readonly-corrupt-cache-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(root);
    int failures = 0;
    for (const std::string caseName : {"waveforms", "blobs", "healthy", "main"}) {
        const fs::path caseDir = root / caseName;
        fs::create_directories(caseDir);
        const std::string dbPath = PathToUtf8(caseDir / "library.db");
        Database writer;
        AudioRecord record;
        record.filePath = "C:/synthetic/readonly-cache-needle.wav";
        record.fileName = "readonly-cache-needle.wav";
        const bool seeded = writer.open(dbPath, true) && writer.addFile(record);
        writer.close();
        if (!seeded) {
            std::cerr << "FAIL: could not seed isolated " << caseName << " fixture\n";
            ++failures;
            continue;
        }
        bool checkpointed = true;
        for (const std::string path : {dbPath, writer.getBlobsDbPathPublic(dbPath),
                                       writer.getWaveformsDbPathPublic(dbPath)}) {
            sqlite3* file = nullptr;
            const int openRc = sqlite3_open_v2(path.c_str(), &file, SQLITE_OPEN_READWRITE, nullptr);
            const bool ok = openRc == SQLITE_OK &&
                sqlite3_exec(file, "PRAGMA wal_checkpoint(TRUNCATE);", nullptr, nullptr, nullptr) == SQLITE_OK &&
                sqlite3_exec(file, "PRAGMA journal_mode=DELETE;", nullptr, nullptr, nullptr) == SQLITE_OK;
            if (file) sqlite3_close(file);
            checkpointed = checkpointed && ok;
        }
        if (!checkpointed) {
            std::cerr << "FAIL: could not checkpoint isolated " << caseName << " fixture\n";
            ++failures;
            continue;
        }
        const fs::path corruptPath = caseName == "waveforms"
            ? fs::u8path(writer.getWaveformsDbPathPublic(dbPath))
            : caseName == "blobs"
                ? fs::u8path(writer.getBlobsDbPathPublic(dbPath))
                : caseDir / "library.db";
        if (caseName != "healthy") {
            std::ofstream damage(corruptPath, std::ios::binary | std::ios::trunc);
            damage << "synthetic damaged sqlite header";
        }
        auto snapshot = [&]() {
            std::vector<std::pair<std::string, std::string>> result;
            for (const auto& entry : fs::directory_iterator(caseDir)) {
                std::ifstream input(entry.path(), std::ios::binary);
                std::ostringstream bytes;
                bytes << input.rdbuf();
                result.emplace_back(PathToUtf8(entry.path().filename()), bytes.str());
            }
            std::sort(result.begin(), result.end());
            return result;
        };
        const auto before = snapshot();
        Database reader;
        const bool opened = reader.openReadOnly(dbPath);
        bool searchable = false;
        bool queryOnly = false;
        bool blobsOnDisk = false;
        bool waveformsOnDisk = false;
        bool blobsInMemory = false;
        bool waveformsInMemory = false;
        if (opened) {
            SearchFilter filter;
            filter.keyword = "readonly-cache-needle";
            const auto found = reader.searchFiles(filter, 10, 0);
            searchable = found.size() == 1 && found.front().fileName == record.fileName;
            sqlite3_stmt* stmt = nullptr;
            if (sqlite3_prepare_v2(reader.db(), "PRAGMA query_only;", -1, &stmt, nullptr) == SQLITE_OK) {
                queryOnly = sqlite3_step(stmt) == SQLITE_ROW && sqlite3_column_int(stmt, 0) == 1;
            }
            if (stmt) sqlite3_finalize(stmt);
            stmt = nullptr;
            if (sqlite3_prepare_v2(reader.db(), "PRAGMA database_list;", -1, &stmt, nullptr) == SQLITE_OK) {
                while (sqlite3_step(stmt) == SQLITE_ROW) {
                    const char* name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
                    const char* path = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
                    if (!name) continue;
                    if (std::string(name) == "blobs_db") {
                        blobsOnDisk = path && *path;
                        blobsInMemory = !path || !*path;
                    } else if (std::string(name) == "waveforms_db") {
                        waveformsOnDisk = path && *path;
                        waveformsInMemory = !path || !*path;
                    }
                }
            }
            if (stmt) sqlite3_finalize(stmt);
        }
        reader.close();
        const bool attachmentExpected = caseName == "waveforms"
            ? blobsOnDisk && waveformsInMemory
            : caseName == "blobs" ? blobsInMemory && waveformsOnDisk
            : caseName == "healthy" ? blobsOnDisk && waveformsOnDisk : true;
        const auto after = snapshot();
        const bool unchanged = after == before;
        if (!unchanged) {
            std::cerr << "  before files:";
            for (const auto& entry : before) std::cerr << ' ' << entry.first << '(' << entry.second.size() << ')';
            std::cerr << "; after files:";
            for (const auto& entry : after) std::cerr << ' ' << entry.first << '(' << entry.second.size() << ')';
            std::cerr << '\n';
        }
        const bool expected = caseName == "main"
            ? !opened && unchanged
            : opened && searchable && queryOnly && attachmentExpected && unchanged;
        std::cout << (expected ? "PASS" : "FAIL")
                  << ": read-only " << caseName
                  << " cache/main boundary (opened=" << opened
                  << ", search=" << searchable << ", query_only=" << queryOnly
                  << ", attachment=" << attachmentExpected
                  << ", unchanged=" << unchanged << " )\n";
        if (!expected) ++failures;
    }
    fs::remove_all(root);
    return failures;
}

int testFavoriteColumnMigrationReportsFailure() {
    const fs::path root = fs::temp_directory_path() /
        ("harken-favorites-migration-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(root);
    const std::string path = PathToUtf8(root / "Auro.db");
    Database seed;
    const bool initialized = seed.open(path, true);
    seed.close();
    if (!initialized) {
        std::cerr << "FAIL: could not create isolated favorites migration fixture\n";
        return 1;
    }
    sqlite3* raw = nullptr;
    const bool rawOpened = sqlite3_open_v2(path.c_str(), &raw, SQLITE_OPEN_READWRITE, nullptr) == SQLITE_OK;
    const bool prepared = rawOpened && sqlite3_exec(
        raw,
        "ALTER TABLE audio_files ADD COLUMN is_favorite INTEGER DEFAULT 0;"
        "CREATE VIEW keep_favorite_column AS SELECT is_favorite FROM audio_files;",
        nullptr, nullptr, nullptr) == SQLITE_OK;
    if (raw) sqlite3_close(raw);
    if (!prepared) {
        std::cerr << "FAIL: could not prepare old favorites schema\n";
        return 1;
    }
    Database blocked;
    const bool opened = blocked.open(path, true);
    const std::string error = blocked.getLastError();
    blocked.close();
    const bool reported = !opened && error.find("is_favorite") != std::string::npos;
    if (!reported) {
        std::cerr << "FAIL: favorite-column migration error was ignored: " << error << "\n";
    }
    if (sqlite3_open_v2(path.c_str(), &raw, SQLITE_OPEN_READWRITE, nullptr) != SQLITE_OK) {
        if (raw) sqlite3_close(raw);
        return 1;
    }
    const bool unblocked = sqlite3_exec(raw, "DROP VIEW keep_favorite_column;",
                                        nullptr, nullptr, nullptr) == SQLITE_OK;
    sqlite3_close(raw);
    Database migrated;
    const bool migratedOpen = unblocked && migrated.open(path, true);
    bool columnGone = migratedOpen;
    sqlite3_stmt* columns = nullptr;
    if (migratedOpen && sqlite3_prepare_v2(
            migrated.db(), "PRAGMA table_info(audio_files);", -1, &columns, nullptr) == SQLITE_OK) {
        while (sqlite3_step(columns) == SQLITE_ROW) {
            const auto* name = sqlite3_column_text(columns, 1);
            if (name && std::string(reinterpret_cast<const char*>(name)) == "is_favorite") {
                columnGone = false;
            }
        }
    } else {
        columnGone = false;
    }
    if (columns) sqlite3_finalize(columns);
    migrated.close();
    Database reopened;
    const bool secondOpen = migratedOpen && reopened.open(path, true);
    reopened.close();
    const bool ok = reported && migratedOpen && columnGone && secondOpen;
    if (!ok) std::cerr << "FAIL: favorite-column migration did not recover cleanly\n";
    if (root.parent_path() == fs::temp_directory_path() &&
        root.filename().string().rfind("harken-favorites-migration-", 0) == 0) {
        fs::remove_all(root);
    }
    return ok ? 0 : 1;
}

int main(int argc, char* argv[]) {
    char* favoritesOnlyRaw = nullptr;
    size_t favoritesOnlyLen = 0;
    const bool favoritesOnly =
        _dupenv_s(&favoritesOnlyRaw, &favoritesOnlyLen,
                   "HARKEN_FAVORITES_MIGRATION_ONLY") == 0 && favoritesOnlyRaw;
    std::free(favoritesOnlyRaw);
    if (favoritesOnly) return testFavoriteColumnMigrationReportsFailure();
    const int readOnlySidecarFailures = testReadOnlyOpenDoesNotCreateLegacySidecars() +
        testReadOnlyOpenSurvivesCorruptRebuildableSidecars();
    char* focusedFallbackRaw = nullptr;
    size_t focusedFallbackLen = 0;
    const bool focusedFallback =
        _dupenv_s(&focusedFallbackRaw, &focusedFallbackLen,
                   "HARKEN_READ_ONLY_FALLBACK_ONLY") == 0 && focusedFallbackRaw;
    std::free(focusedFallbackRaw);
    if (focusedFallback) return readOnlySidecarFailures;
    std::cout << "============================================================\n";
    std::cout << "  数据库拆分和路径索引功能测试程序\n";
    std::cout << "============================================================\n";
    
    // 确定数据库路径
    std::string dbPath;
    if (argc > 1) {
        dbPath = argv[1];
    } else {
        // 默认路径
        dbPath = "test_meta.db";
        std::cout << "\n使用默认数据库路径: " << dbPath << "\n";
        std::cout << "提示: 可以指定数据库路径作为参数: " << argv[0] << " <db_path>\n";
    }
    
    // 打开数据库
    Database db;
    std::cout << "\n正在打开数据库: " << dbPath << "\n";
    if (!db.open(dbPath)) {
        std::cerr << "✗ 打开数据库失败: " << db.getLastError() << "\n";
        std::cerr << "\n提示: 如果数据库不存在，程序会自动创建。\n";
        return 1;
    }
    std::cout << "✓ 数据库打开成功\n";
    
    // 运行测试
    testDatabaseSplitting(db, dbPath);
    testPathIndexing(db);
    testDatabaseStructure(db);

    // 关闭数据库
    db.close();

    char* focusedCaseRaw = nullptr;
    size_t focusedCaseLen = 0;
    if (_dupenv_s(&focusedCaseRaw, &focusedCaseLen, "AURA_TEST_DATABASE_FEATURES_CASE") == 0 && focusedCaseRaw) {
        const std::string focused(focusedCaseRaw);
        std::free(focusedCaseRaw);
        if (focused == "cover_tile_membership") {
            int focusedFailures = 0;
            focusedFailures += testCoverWallGroupedSelectionIncludesAllFoldersWithoutFuzzyNeighbors();
            focusedFailures += testTemporaryCoverIdentityReverseLookupMatchesCoverFilter();
            focusedFailures += testCoverTileMembershipUsesEquivalentRegistryKeys();
            focusedFailures += testCoverClickPhashLookupPreservesHammingTwoCluster();
            focusedFailures += testCoverClickPreparedFilterReuseSurvivesEquivalentCacheClear();
            focusedFailures += testCoverWallAggregationPrewarmsTileMembershipForVisibleEntries();
            focusedFailures += testCoverTileMembershipPrefersCanonicalMembershipIndex();
            focusedFailures += testSpecificCoverFilterPrefersCanonicalMembershipIndex();
            focusedFailures += testSpecificCoverFilterSelfHealsLegacyMembershipRows();
            focusedFailures += testCoverTileMembershipSelfHealsLegacyDatabaseRows();
            focusedFailures += testCoverTileMembershipMaintenanceBatchRepairsLegacyRows();
            focusedFailures += testCoverTileMembershipReopenRearmsMaintenanceMarker();
            focusedFailures += testCoverTileMembershipReadOnlyDoesNotReviveLegacyFallback();
            return focusedFailures == 0 ? 0 : 1;
        }
        if (focused == "cover_click_perf") {
            int focusedFailures = 0;
            focusedFailures += testCoverClickPhashLookupPreservesHammingTwoCluster();
            focusedFailures += testCoverClickPreparedFilterReuseSurvivesEquivalentCacheClear();
            focusedFailures += testCoverWallAggregationPrewarmsTileMembershipForVisibleEntries();
            return focusedFailures == 0 ? 0 : 1;
        }
        if (focused == "pragma_modes") {
            int focusedFailures = 0;
            focusedFailures += testNewPrimaryDatabaseUses64kPageSizeOnlyForFreshDb();
            focusedFailures += testFastScanPragmaModeRoundTripAndDirtyRecovery();
            return focusedFailures == 0 ? 0 : 1;
        }
        if (focused == "waveform_compression") {
            int focusedFailures = 0;
            focusedFailures += testWaveformCompressionRoundTripAndLegacyRawCompatibility();
            return focusedFailures == 0 ? 0 : 1;
        }
        if (focused == "mini_min_data") {
            int focusedFailures = 0;
            focusedFailures += testMiniMinDataRoundTripAndLegacyFallback();
            return focusedFailures == 0 ? 0 : 1;
        }
        if (focused == "mini_format_upgrade") {
            int focusedFailures = 0;
            focusedFailures += testAdaptiveMiniWaveformFormatUpgradeStamp();
            return focusedFailures == 0 ? 0 : 1;
        }
        if (focused == "tag_facet_index") {
            int focusedFailures = 0;
            focusedFailures += testTagFacetIndexPrestoreAndFallback();
            return focusedFailures == 0 ? 0 : 1;
        }
        if (focused == "backup_restore") {
            return testDatabaseBackupRestorePalBehavior() == 0 ? 0 : 1;
        }
        if (focused == "path_case_dedup") {
            int focusedFailures = 0;
            focusedFailures += testWindowsPathCaseVariantsDoNotDuplicateRows();
            focusedFailures += testWindowsPathCaseVariantRescanKeepsFileIdTracking();
            return focusedFailures == 0 ? 0 : 1;
        }
        if (focused == "cover_wall_file_list_order") {
            int focusedFailures = 0;
            focusedFailures += testSmallCoverWallPageApiFallbackPages();
            focusedFailures += testGlobalCoverWallPageFollowsCurrentFileListOrder();
            focusedFailures += testLargeCoverWallPageFollowsCurrentFileListPastFiftyThousandRows();
            focusedFailures += testSmallCoverWallSameFilenamePageCursorDoesNotSkipOrRepeat();
            return focusedFailures == 0 ? 0 : 1;
        }
        if (focused == "folder_cover_distinct_similar") {
            return testFolderCoverFacetsKeepDistinctSimilarArtworks();
        }
        if (focused == "cover_wall_page_cursor") {
            return testGlobalCoverWallPageApiUsesStableCursor() == 0 ? 0 : 1;
        }
        if (focused == "cover_wall_same_filename_page_cursor") {
            return testSmallCoverWallSameFilenamePageCursorDoesNotSkipOrRepeat() == 0 ? 0 : 1;
        }
    }

    // Run channel category filter test in its own scope (uses a separate temp db).
    int channelFailures = testChannelCategoryFilter();
    int audioBoundaryFailures = testAudioDatabaseBoundaryDiscard();
    int coverDedupFailures = testScanTimeCoverDedupFinalize();
    int coverDeferredFailures = testOpenAndCoverQueriesDoNotFullSyncRegistry();
    int largeCoverWallFailures = testLargeCoverWallUsesIncrementalRegistryOnly();
    int coverWallPageFailures = testGlobalCoverWallPageApiUsesStableCursor();
    coverWallPageFailures += testGlobalCoverWallPageRegistryUsesLiveRepresentativePath();
    int coverWallSparsePageFailures = testLargeCoverWallPageSupplementsSparseRegistryWithoutFullSync();
    int smallCoverWallPageFailures = testSmallCoverWallPageApiFallbackPages();
    int coverWallFileListOrderFailures = testGlobalCoverWallPageFollowsCurrentFileListOrder();
    coverWallFileListOrderFailures += testLargeCoverWallPageFollowsCurrentFileListPastFiftyThousandRows();
    int sameFilenameCoverWallPageFailures = testSmallCoverWallSameFilenamePageCursorDoesNotSkipOrRepeat();
    int folderCoverDistinctFailures = testFolderCoverFacetsKeepDistinctSimilarArtworks();
    int largeCoverFacetFailures = testLargeCoverFacetsUseIncrementalRegistryOnly();
    int largeCoverHotPathFailures = testLargeCoverBrowseHotPathsUseBoundedRegistryHeuristic();
    int searchCountFallbackFailures = testSearchFilesCountMirrorsFtsFallbackContract();
    int zeroFilteredCountCacheFailures = testSearchFilesCountDoesNotCacheZeroFilteredCountsContract();
    int largeCoverRepairFailures = testLargeCoverRegistryRepairBatchProgressivelyFillsSparseRegistry();
    int largeCoverRearmFailures = testSparseLargeRegistryReopenRearmsMaintenanceMarker();
    int largeCoverPreheatRearmFailures = testSparseLargeRegistryPreheatRearmsPartialMarker();
    int coverThumbMaintenanceFailures =
        testCoverThumbMaintenanceRepairsMissingThumbsEvenWhenPhashVersionCurrent();
    int coverThumbTerminalFailureFailures =
        testCoverThumbMaintenanceSkipsTerminalFailuresAndInvalidKeys();
    int coverThumbMaintenanceContractFailures = testCoverThumbMaintenanceSourceContract();
    int coverSamenessFailures = testCoverIdentitySamenessAcrossWallFacetAndPreview();
    int pureFolderFastPathFailures = testPureFolderSearchFastPathRespectsFolderBoundary();
    int naturalFilenameSortFailures = testFilenameSortingUsesNaturalNumericOrder();
    int playlistNaturalSortFailures = testPlaylistSortingUsesNaturalNumericOrder();
    int coverSparseRegistryFailures = testGlobalCoverFacetFallsBackWhenRegistrySparse();
    int coverWallSparseRegistryFailures = testGlobalCoverWallFallsBackWhenRegistrySparse();
    int coverHasCoverFacetFailures = testGlobalCoverFacetFallsBackWhenHasCoverRowsLackKeys();
    int tempIdentityReverseLookupFailures = testTemporaryCoverIdentityReverseLookupMatchesCoverFilter();
    int coverTileMembershipFailures = testCoverTileMembershipUsesEquivalentRegistryKeys();
    int coverClickPhashBoundedFailures = testCoverClickPhashLookupPreservesHammingTwoCluster();
    int coverClickPreparedReuseFailures = testCoverClickPreparedFilterReuseSurvivesEquivalentCacheClear();
    int coverWallPrewarmFailures = testCoverWallAggregationPrewarmsTileMembershipForVisibleEntries();
    int coverTileMembershipIndexFailures = testCoverTileMembershipPrefersCanonicalMembershipIndex();
    int coverTileMembershipFilterFailures = testSpecificCoverFilterPrefersCanonicalMembershipIndex();
    int coverTileMembershipFilterSelfHealFailures = testSpecificCoverFilterSelfHealsLegacyMembershipRows();
    int coverTileMembershipSelfHealFailures = testCoverTileMembershipSelfHealsLegacyDatabaseRows();
    int coverTileMembershipMaintenanceFailures = testCoverTileMembershipMaintenanceBatchRepairsLegacyRows();
    int coverTileMembershipReopenFailures = testCoverTileMembershipReopenRearmsMaintenanceMarker();
    int coverTileMembershipReadOnlyFallbackFailures = testCoverTileMembershipReadOnlyDoesNotReviveLegacyFallback();
    int coverPresenceCandidateFailures = testCountAudioCoverPresenceCandidatesUsesThreeWayCoverUnion();
    int coverHasCoverWallFailures = testGlobalCoverWallFallsBackWhenHasCoverRowsLackKeys();
    int coverSparseCacheFailures = testGlobalCoverFacetBypassesPersistentCacheWhenRegistrySparse();
    int smallFilteredFacetCacheFailures = testSmallFilteredFacetQueriesPersistCacheEntries();
    int similarityRefreshFailures = testSimilarityCacheRefreshesWhenFeatureRowsGrow();
    int queryViewSnapshotFailures = testQueryViewSnapshotPersistence();
    int activityLogFailures = testActivityLogPersistence();
    int pageSizeFailures = testNewPrimaryDatabaseUses64kPageSizeOnlyForFreshDb();
    int fastScanModeFailures = testFastScanPragmaModeRoundTripAndDirtyRecovery();
    int waveformCompressionFailures = testWaveformCompressionRoundTripAndLegacyRawCompatibility();
    int tagFacetIndexFailures = testTagFacetIndexPrestoreAndFallback();
    int readOnlyPinyinFailures = testReadOnlyPinyinRecomputeStops();

    printSeparator("测试完成");
    if (channelFailures == 0 && audioBoundaryFailures == 0 && coverDedupFailures == 0 &&
        coverDeferredFailures == 0 && largeCoverWallFailures == 0 &&
        coverWallPageFailures == 0 && coverWallSparsePageFailures == 0 &&
        smallCoverWallPageFailures == 0 &&
        coverWallFileListOrderFailures == 0 &&
        sameFilenameCoverWallPageFailures == 0 && folderCoverDistinctFailures == 0 &&
        largeCoverFacetFailures == 0 && largeCoverHotPathFailures == 0 &&
        searchCountFallbackFailures == 0 &&
        zeroFilteredCountCacheFailures == 0 &&
        largeCoverRepairFailures == 0 &&
        largeCoverRearmFailures == 0 && largeCoverPreheatRearmFailures == 0 &&
        coverThumbMaintenanceFailures == 0 &&
        coverThumbTerminalFailureFailures == 0 &&
        coverThumbMaintenanceContractFailures == 0 &&
        coverSamenessFailures == 0 && pureFolderFastPathFailures == 0 &&
        naturalFilenameSortFailures == 0 &&
        playlistNaturalSortFailures == 0 &&
        coverSparseRegistryFailures == 0 &&
        coverWallSparseRegistryFailures == 0 &&
        coverHasCoverFacetFailures == 0 &&
        tempIdentityReverseLookupFailures == 0 &&
        coverTileMembershipFailures == 0 &&
        coverClickPhashBoundedFailures == 0 &&
        coverClickPreparedReuseFailures == 0 &&
        coverWallPrewarmFailures == 0 &&
        coverTileMembershipIndexFailures == 0 &&
        coverTileMembershipFilterFailures == 0 &&
        coverTileMembershipFilterSelfHealFailures == 0 &&
        coverTileMembershipSelfHealFailures == 0 &&
        coverTileMembershipMaintenanceFailures == 0 &&
        coverTileMembershipReopenFailures == 0 &&
        coverTileMembershipReadOnlyFallbackFailures == 0 &&
        coverPresenceCandidateFailures == 0 &&
        coverHasCoverWallFailures == 0 &&
        coverSparseCacheFailures == 0 &&
        smallFilteredFacetCacheFailures == 0 &&
        similarityRefreshFailures == 0 &&
        queryViewSnapshotFailures == 0 &&
        activityLogFailures == 0 &&
        pageSizeFailures == 0 &&
        fastScanModeFailures == 0 &&
        waveformCompressionFailures == 0 &&
        tagFacetIndexFailures == 0 &&
        readOnlyPinyinFailures == 0 && readOnlySidecarFailures == 0) {
        std::cout << "所有测试已完成。\n";
        return 0;
    }
    std::cerr << "ChannelCategoryFilter: " << channelFailures << " assertion(s) failed.\n";
    std::cerr << "AudioDatabaseBoundaryDiscard: " << audioBoundaryFailures << " assertion(s) failed.\n";
    std::cerr << "ScanTimeCoverDedupFinalize: " << coverDedupFailures << " assertion(s) failed.\n";
    std::cerr << "CoverRegistryDeferred: " << coverDeferredFailures << " assertion(s) failed.\n";
    std::cerr << "LargeCoverWallIncremental: " << largeCoverWallFailures << " assertion(s) failed.\n";
    std::cerr << "CoverWallPageCursor: " << coverWallPageFailures << " assertion(s) failed.\n";
    std::cerr << "CoverWallPageSmallFallback: " << smallCoverWallPageFailures << " assertion(s) failed.\n";
    std::cerr << "CoverWallPageFileListOrder: " << coverWallFileListOrderFailures << " assertion(s) failed.\n";
    std::cerr << "CoverWallPageSameFilenameCursor: " << sameFilenameCoverWallPageFailures << " assertion(s) failed.\n";
    std::cerr << "FolderCoverDistinctSimilar: " << folderCoverDistinctFailures << " assertion(s) failed.\n";
    std::cerr << "LargeCoverFacetIncremental: " << largeCoverFacetFailures << " assertion(s) failed.\n";
    std::cerr << "LargeCoverHotPathBoundedness: " << largeCoverHotPathFailures << " assertion(s) failed.\n";
    std::cerr << "SearchCountFtsFallback: " << searchCountFallbackFailures << " assertion(s) failed.\n";
    std::cerr << "LargeCoverRegistryRepair: " << largeCoverRepairFailures << " assertion(s) failed.\n";
    std::cerr << "LargeCoverRegistryRearm: " << largeCoverRearmFailures << " assertion(s) failed.\n";
    std::cerr << "LargeCoverRegistryPreheatRearm: " << largeCoverPreheatRearmFailures << " assertion(s) failed.\n";
    std::cerr << "CoverThumbMaintenance: " << coverThumbMaintenanceFailures << " assertion(s) failed.\n";
    std::cerr << "CoverThumbTerminalFailures: " << coverThumbTerminalFailureFailures << " assertion(s) failed.\n";
    std::cerr << "CoverThumbMaintenanceContract: " << coverThumbMaintenanceContractFailures << " assertion(s) failed.\n";
    std::cerr << "CoverIdentitySameness: " << coverSamenessFailures << " assertion(s) failed.\n";
    std::cerr << "PureFolderFastPathBoundary: " << pureFolderFastPathFailures << " assertion(s) failed.\n";
    std::cerr << "NaturalFilenameSort: " << naturalFilenameSortFailures << " assertion(s) failed.\n";
    std::cerr << "PlaylistNaturalSort: " << playlistNaturalSortFailures << " assertion(s) failed.\n";
    std::cerr << "CoverSparseRegistryFallback: " << coverSparseRegistryFailures << " assertion(s) failed.\n";
    std::cerr << "CoverWallSparseRegistryFallback: " << coverWallSparseRegistryFailures << " assertion(s) failed.\n";
    std::cerr << "CoverHasCoverWithoutKeysFacet: " << coverHasCoverFacetFailures << " assertion(s) failed.\n";
    std::cerr << "CoverTempIdentityReverseLookup: " << tempIdentityReverseLookupFailures << " assertion(s) failed.\n";
    std::cerr << "CoverTileMembershipEquivalentKeys: " << coverTileMembershipFailures << " assertion(s) failed.\n";
    std::cerr << "CoverClickPhashHammingTwoCluster: " << coverClickPhashBoundedFailures << " assertion(s) failed.\n";
    std::cerr << "CoverClickPreparedReuse: " << coverClickPreparedReuseFailures << " assertion(s) failed.\n";
    std::cerr << "CoverWallMembershipPrewarm: " << coverWallPrewarmFailures << " assertion(s) failed.\n";
    std::cerr << "CoverTileMembershipCanonicalIndex: " << coverTileMembershipIndexFailures << " assertion(s) failed.\n";
    std::cerr << "CoverTileMembershipSpecificFilter: " << coverTileMembershipFilterFailures << " assertion(s) failed.\n";
    std::cerr << "CoverTileMembershipSpecificFilterSelfHeal: " << coverTileMembershipFilterSelfHealFailures << " assertion(s) failed.\n";
    std::cerr << "CoverTileMembershipSelfHeal: " << coverTileMembershipSelfHealFailures << " assertion(s) failed.\n";
    std::cerr << "CoverTileMembershipMaintenanceBatch: " << coverTileMembershipMaintenanceFailures << " assertion(s) failed.\n";
    std::cerr << "CoverTileMembershipReopenRearm: " << coverTileMembershipReopenFailures << " assertion(s) failed.\n";
    std::cerr << "CoverTileMembershipReadOnlyNoLegacyFallback: " << coverTileMembershipReadOnlyFallbackFailures << " assertion(s) failed.\n";
    std::cerr << "CoverPresenceCandidatesThreeWayUnion: " << coverPresenceCandidateFailures << " assertion(s) failed.\n";
    std::cerr << "CoverHasCoverWithoutKeysWall: " << coverHasCoverWallFailures << " assertion(s) failed.\n";
    std::cerr << "CoverSparsePersistentCacheBypass: " << coverSparseCacheFailures << " assertion(s) failed.\n";
    std::cerr << "SmallFilteredFacetCachePersist: " << smallFilteredFacetCacheFailures << " assertion(s) failed.\n";
    std::cerr << "SimilarityCacheRefresh: " << similarityRefreshFailures << " assertion(s) failed.\n";
    std::cerr << "QueryViewSnapshotPersistence: " << queryViewSnapshotFailures << " assertion(s) failed.\n";
    std::cerr << "ActivityLogPersistence: " << activityLogFailures << " assertion(s) failed.\n";
    std::cerr << "PageSize65536FreshOnly: " << pageSizeFailures << " assertion(s) failed.\n";
    std::cerr << "FastScanPragmaMode: " << fastScanModeFailures << " assertion(s) failed.\n";
    std::cerr << "WaveformCompression: " << waveformCompressionFailures << " assertion(s) failed.\n";
    std::cerr << "TagFacetIndex: " << tagFacetIndexFailures << " assertion(s) failed.\n";
    std::cerr << "ReadOnlyPinyinRecompute: " << readOnlyPinyinFailures << " assertion(s) failed.\n";
    std::cerr << "ReadOnlyLegacySidecars: " << readOnlySidecarFailures << " assertion(s) failed.\n";
    return 1;
}
