#include "Database.h"
// i18n-anchor (F1, RQ-20260911-04): m_lastError/outError/heartbeatMsg feeders.
// Display sites (reverse map): getLastError -> main.cpp 10484 switchError /
// 31884 newDbError, AsyncSearchRefreshCompletionStateAdapter, BackfillWorkerPool
// logs (sqlite tail passthrough); delete outError -> close/remove db toasts;
// heartbeatMsg -> progress callback. Transient classifier matches only
// lower-cased "busy"/"locked" from the sqlite tail (%s argument) — untouched.
#include "Localization.h"   // 7-A slice-4: first L() user in this TU
#include "AppState.h"  // For AppSettings
#include "BackfillQueue.h"
#include "BackfillWorkerPool.h"
#include "BackgroundIoPolicy.h"
#include "CoverModule.h"  // InvalidateCoverForPath clears stale UI artwork after a path change.
#include "CoverWallSelectionKey.h"
#include "CoverReplacementFoundation.h"
#include "MetadataReader.h"
#include "platform/database_audio_probe.h"
#include "TagVocabularyMapper.h"
#include "SearchQuery.h"
#include "SearchTextIndex.h"
#include "platform/filesystem.h"
#include "platform/harken_user_data_layout.h"
#include "platform/file_watcher.h"
#include "WaveformExtractor.h"
#include "WaveformCompression.h"
#include "WaveformCachePolicy.h"
#include "WaveformPrewarmPolicy.h"
#include "Utils.h"     // Unicode search-data helpers.
#include "PathUtils.h" // Canonical folder-path migration.
#include "../thirdparty/sqlite/sqlite3.h"
#define XXH_INLINE_ALL
#include "../thirdparty/xxhash.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <chrono>
#include <ctime>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <cmath>
#include <limits>
#include "DiagnosticsLogger.h"
#include <cctype>
#include <cwctype>
#include <set>
#include <map>
#include <queue>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#if defined(_MSC_VER)
#include <intrin.h>
#endif

static double MsSince(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

static int CountBits64(uint64_t value) {
#if defined(_MSC_VER) && defined(_M_X64)
    return static_cast<int>(__popcnt64(value));
#elif defined(__GNUC__) || defined(__clang__)
    return __builtin_popcountll(value);
#else
    int count = 0;
    while (value != 0) {
        value &= (value - 1);
        ++count;
    }
    return count;
#endif
}

static void AppendCoverPerfLog(const std::string& line) {
    const std::string tempDirectory = Platform::FileSystem::TemporaryDirectory();
    if (tempDirectory.empty()) return;
    const std::string logPath =
        (std::filesystem::u8path(tempDirectory) / "auro_perf.log").u8string();
    Platform::FileSystem::StreamWriter writer;
    if (!writer.Open(logPath, true, false)) return;
    writer.Write(line + "\n");
    writer.Flush();
    writer.Close();
}

static std::string FormatDbError(const char* fmt, const char* detail) {
    const char* safeFmt = fmt ? fmt : "%s";
    const char* safeDetail = detail ? detail : "";
    const int needed = std::snprintf(nullptr, 0, safeFmt, safeDetail);
    if (needed < 0) {
        return std::string(safeDetail);
    }
    std::string out(static_cast<size_t>(needed) + 1, '\0');
    std::snprintf(out.data(), out.size(), safeFmt, safeDetail);
    out.resize(static_cast<size_t>(needed));
    return out;
}

#ifdef _WIN32
#ifdef CopyFile
#undef CopyFile
#endif
#endif

// Register the C++ text helpers used by SQLite search expressions.
static void RegisterHarkenSearchSqliteFunctions(sqlite3* db) {
    if (!db) return;
    sqlite3_create_function(db, "aura_search_data", 1, SQLITE_UTF8 | SQLITE_DETERMINISTIC, nullptr,
        [](sqlite3_context* ctx, int argc, sqlite3_value** argv) {
            if (argc < 1 || sqlite3_value_type(argv[0]) == SQLITE_NULL) {
                sqlite3_result_null(ctx);
                return;
            }
            const char* text = (const char*)sqlite3_value_text(argv[0]);
            if (!text) {
                sqlite3_result_null(ctx);
                return;
            }
            std::string input(text);
            std::string res = HarkenSearch::Search::BuildSearchDataForFts(input);
            sqlite3_result_text(ctx, res.c_str(), -1, SQLITE_TRANSIENT);
        }, nullptr, nullptr);
    sqlite3_create_function(db, "phash_hamming", 2, SQLITE_UTF8 | SQLITE_DETERMINISTIC, nullptr,
        [](sqlite3_context* ctx, int argc, sqlite3_value** argv) {
            if (argc < 2 ||
                sqlite3_value_type(argv[0]) == SQLITE_NULL ||
                sqlite3_value_type(argv[1]) == SQLITE_NULL) {
                sqlite3_result_null(ctx);
                return;
            }
            const uint64_t lhs = static_cast<uint64_t>(sqlite3_value_int64(argv[0]));
            const uint64_t rhs = static_cast<uint64_t>(sqlite3_value_int64(argv[1]));
            sqlite3_result_int(ctx, CountBits64(lhs ^ rhs));
        }, nullptr, nullptr);
    sqlite3_create_collation(
        db,
        "AURA_NATURAL",
        SQLITE_UTF8,
        nullptr,
        [](void*, int lhsLen, const void* lhsData, int rhsLen, const void* rhsData) -> int {
            const std::string_view lhs(
                lhsData ? static_cast<const char*>(lhsData) : "",
                lhsData && lhsLen > 0 ? static_cast<size_t>(lhsLen) : 0);
            const std::string_view rhs(
                rhsData ? static_cast<const char*>(rhsData) : "",
                rhsData && rhsLen > 0 ? static_cast<size_t>(rhsLen) : 0);
            return CompareNaturalUtf8(lhs, rhs);
        });
}

static std::string Hex64_db(uint64_t v) {
    static const char* hex = "0123456789abcdef";
    std::string s(16, '0');
    for (int i = 15; i >= 0; --i) {
        s[(size_t)i] = hex[(size_t)(v & 0xF)];
        v >>= 4;
    }
    return s;
}

static int SafeSqliteBlobSize(size_t size) {
    if (size > (size_t)(std::numeric_limits<int>::max)()) return -1;
    return (int)size;
}

static const char* kWpk2SqlValidWaveformDetail =
    "(waveform_detail IS NOT NULL "
    " AND length(waveform_detail) > 8 "
    " AND substr(waveform_detail, 1, 6) = x'0057504B3201' "
    " AND substr(waveform_detail, 8, 1) = x'01' "
    " AND detail_num_channels IS NOT NULL "
    " AND detail_num_channels > 0 "
    " AND hex(substr(waveform_detail, 7, 1)) = printf('%02X', detail_num_channels) "
    " AND ((length(waveform_detail) - 8) % 4) = 0 "
    " AND (((length(waveform_detail) - 8) / 4) % detail_num_channels) = 0)";

static std::string Wpk2SqlColumn(const char* alias, const char* column) {
    std::string out;
    if (alias && *alias) {
        out += alias;
        out += ".";
    }
    out += column;
    return out;
}

static std::string Wpk2SqlValidDetailExpr(const char* alias) {
    const std::string detail = Wpk2SqlColumn(alias, "waveform_detail");
    const std::string channels = Wpk2SqlColumn(alias, "detail_num_channels");
    return "(" + detail + " IS NOT NULL "
        "AND length(" + detail + ") > 8 "
        "AND substr(" + detail + ", 1, 6) = x'0057504B3201' "
        "AND substr(" + detail + ", 8, 1) = x'01' "
        "AND " + channels + " IS NOT NULL "
        "AND " + channels + " > 0 "
        "AND hex(substr(" + detail + ", 7, 1)) = printf('%02X', " + channels + ") "
        "AND ((length(" + detail + ") - 8) % 4) = 0 "
        "AND (((length(" + detail + ") - 8) / 4) % " + channels + ") = 0)";
}

static std::string Wpk2SqlLooksLikeDetailExpr(const char* alias) {
    const std::string detail = Wpk2SqlColumn(alias, "waveform_detail");
    return "(" + detail + " IS NOT NULL "
        "AND length(" + detail + ") >= 5 "
        "AND substr(" + detail + ", 1, 5) = x'0057504B32')";
}

static std::string DetailMissingSqlExpr(const char* alias, const std::string& minBytesExpr) {
    const std::string detail = Wpk2SqlColumn(alias, "waveform_detail");
    const std::string validWpk2 = Wpk2SqlValidDetailExpr(alias);
    const std::string looksLikeWpk2 = Wpk2SqlLooksLikeDetailExpr(alias);
    return "(" + detail + " IS NULL OR (NOT " + validWpk2 + " AND (length(" + detail + ") < " + minBytesExpr + " OR " + looksLikeWpk2 + ")))";
}

static std::string WaveformCacheRetryableSqlExpr(const char* alias) {
    const std::string status = std::string(alias) + ".status";
    return "(" + status + " IS NULL OR " + status + " <> 'failed')";
}

static int ReadEnvIntClamped(const char* name, int fallback, int minValue, int maxValue) {
#ifdef _WIN32
    char* raw = nullptr;
    size_t rawLen = 0;
    if (_dupenv_s(&raw, &rawLen, name) != 0 || !raw || rawLen == 0) {
        if (raw) free(raw);
        return fallback;
    }
    std::unique_ptr<char, decltype(&free)> rawHolder(raw, free);
#else
    const char* raw = std::getenv(name);
    if (!raw || !*raw) return fallback;
#endif
    try {
        const int parsed = std::stoi(raw);
        return std::clamp(parsed, minValue, maxValue);
    } catch (...) {
        return fallback;
    }
}

static bool IsWpk2DetailWriterEnabled() {
    // PRODUCTION: new persistent detail cache rows use compact WPK2 by default.
    // Set AURA_WPK2_DETAIL_WRITER=0 only for legacy migration tests or emergency diagnostics.
    return ReadEnvIntClamped("AURA_WPK2_DETAIL_WRITER", 1, 0, 1) == 1;
}

static bool TryEncodeWpk2DetailBlob(const std::vector<float>& waveform,
                                    const std::vector<float>& minWaveform,
                                    int detailChannels,
                                    bool wpk2WriterEnabled,
                                    std::vector<uint8_t>& outBlob) {
    outBlob.clear();
    if (!wpk2WriterEnabled ||
        detailChannels <= 0 ||
        waveform.empty() ||
        minWaveform.size() != waveform.size() ||
        (waveform.size() % (size_t)detailChannels) != 0) {
        return false;
    }

    const size_t bucketCount = waveform.size() / (size_t)detailChannels;
    if (bucketCount == 0 || bucketCount > (size_t)std::numeric_limits<int>::max()) {
        return false;
    }
    return WaveformExtractor::EncodePeakEnvelopeInt16(
        waveform,
        minWaveform,
        detailChannels,
        (int)bucketCount,
        outBlob);
}

constexpr int kFacetQueryCacheVersion = 2;
/** Maximum path/filter facet-cache entries. A larger cache retains recently visited scopes. */
constexpr int kFacetQueryCacheMaxEntries = 64;
constexpr int kFolderCoverFacetIndexVersion = 3;  // v3: unified E|+F| candidate stream with final GROUP BY grp
constexpr int kMusicViewsVersion = 3;  // v3 groups by album alone so track artists do not split one album
constexpr int kFolderCoverFacetMaxDepth = 4;
constexpr int kFolderCoverFacetPrewarmMaxScopes = 192;
// An empty registry above this file-count threshold waits for background sync instead of grouping audio_files inline.
constexpr int64_t kGlobalCoverWallFallbackMaxFiles = 50000;

template<typename T>
static void AppendPod(std::vector<uint8_t>& out, const T& value) {
    const uint8_t* ptr = reinterpret_cast<const uint8_t*>(&value);
    out.insert(out.end(), ptr, ptr + sizeof(T));
}

template<typename T>
static bool ReadPod(const std::vector<uint8_t>& in, size_t& offset, T& value) {
    if (offset + sizeof(T) > in.size()) return false;
    std::memcpy(&value, in.data() + offset, sizeof(T));
    offset += sizeof(T);
    return true;
}

static void AppendString(std::vector<uint8_t>& out, const std::string& value) {
    uint32_t len = static_cast<uint32_t>(value.size());
    AppendPod(out, len);
    out.insert(out.end(), value.begin(), value.end());
}

static bool ReadString(const std::vector<uint8_t>& in, size_t& offset, std::string& value) {
    uint32_t len = 0;
    if (!ReadPod(in, offset, len)) return false;
    if (offset + len > in.size()) return false;
    value.assign(reinterpret_cast<const char*>(in.data() + offset), len);
    offset += len;
    return true;
}

static std::vector<uint8_t> SerializeCoverFacetEntries(
    const std::vector<Database::CoverFacetEntry>& entries) {
    std::vector<uint8_t> out;
    out.reserve(entries.size() * 96);
    uint32_t count = static_cast<uint32_t>(entries.size());
    AppendPod(out, count);
    for (const auto& e : entries) {
        AppendString(out, e.key);
        AppendString(out, e.path);
        AppendString(out, e.sourcePath);
        uint8_t isEmbedded = e.isEmbedded ? 1u : 0u;
        AppendPod(out, isEmbedded);
        AppendPod(out, e.count);
    }
    return out;
}

static bool DeserializeCoverFacetEntries(const std::vector<uint8_t>& payload,
                                         std::vector<Database::CoverFacetEntry>& out) {
    out.clear();
    size_t offset = 0;
    uint32_t count = 0;
    if (!ReadPod(payload, offset, count)) return false;
    out.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        Database::CoverFacetEntry e;
        uint8_t isEmbedded = 0;
        if (!ReadString(payload, offset, e.key) ||
            !ReadString(payload, offset, e.path) ||
            !ReadString(payload, offset, e.sourcePath) ||
            !ReadPod(payload, offset, isEmbedded) ||
            !ReadPod(payload, offset, e.count)) {
            out.clear();
            return false;
        }
        e.isEmbedded = (isEmbedded != 0);
        out.push_back(std::move(e));
    }
    return offset == payload.size();
}

static std::vector<uint8_t> SerializePathFacetEntries(
    const std::vector<Database::PathFacetEntry>& entries) {
    std::vector<uint8_t> out;
    out.reserve(entries.size() * 64);
    uint32_t count = static_cast<uint32_t>(entries.size());
    AppendPod(out, count);
    for (const auto& e : entries) {
        AppendString(out, e.folderPath);
        AppendPod(out, e.count);
    }
    return out;
}

static bool DeserializePathFacetEntries(const std::vector<uint8_t>& payload,
                                        std::vector<Database::PathFacetEntry>& out) {
    out.clear();
    size_t offset = 0;
    uint32_t count = 0;
    if (!ReadPod(payload, offset, count)) return false;
    out.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        Database::PathFacetEntry e;
        if (!ReadString(payload, offset, e.folderPath) ||
            !ReadPod(payload, offset, e.count)) {
            out.clear();
            return false;
        }
        out.push_back(std::move(e));
    }
    return offset == payload.size();
}

static std::string BuildPersistentFacetCacheKey(const SearchFilter& filter,
                                                int limit,
                                                const char* kind) {
    std::ostringstream oss;
    auto appendVector = [&](const char* label, const std::vector<std::string>& values) {
        oss << label << '[' << values.size() << "]|";
        for (const auto& value : values) {
            oss << value.size() << ':' << value << '|';
        }
    };

    oss << kind << '|'
        << limit << '|'
        << filter.keyword << '|'
        << filter.category << '|'
        << filter.folderPath << '|'
        << filter.minDuration << '|'
        << filter.maxDuration << '|'
        << filter.minSampleRate << '|'
        << filter.maxSampleRate << '|'
        << filter.minBpm << '|'
        << filter.maxBpm << '|'
        << filter.artist << '|'
        << filter.album << '|'
        << filter.format << '|'
        << filter.channels << '|'
        << filter.minChannels << '|'
        << filter.maxChannels << '|'
        << static_cast<int>(filter.channelCategory) << '|'
        << filter.coverFilter << '|'
        << filter.specificCoverKey << '|'
        << filter.includeOnlyKnownExts << '|'
        << filter.musicalKey << '|'
        << filter.unplayedOnly << '|'
        << filter.playedOnlyAny << '|'
        << filter.highlyRatedOnly << '|'
        << filter.addedAfter << '|'
        << filter.playedAfter << '|'
        << filter.exactMode << '|';
    appendVector("artists", filter.artists);
    appendVector("excludeExt", filter.excludeExtensions);
    appendVector("includeGenres", filter.includeGenres);
    appendVector("excludeGenres", filter.excludeGenres);
    appendVector("includeInstruments", filter.includeInstruments);
    appendVector("excludeInstruments", filter.excludeInstruments);
    if (filter.synonyms) {
        oss << "synonyms[" << filter.synonyms->size() << "]|";
        for (const auto& group : *filter.synonyms) {
            appendVector("synTriggers", group.triggers);
            appendVector("synRelated", group.related);
        }
    } else {
        oss << "synonyms:null|";
    }
    return oss.str();
}

static bool ShouldPersistFacetEntriesForFilter(const SearchFilter& filter, int entryCount) {
    return aura::cover_replacement::ShouldPersistFacetEntriesForFilter(filter, entryCount);
}

static int GetFolderPathDepth(const std::string& folderPath) {
    if (folderPath.empty()) return 0;
    int depth = 0;
    size_t i = 0;
    const size_t n = folderPath.size();
    if (n >= 2 && std::isalpha((unsigned char)folderPath[0]) && folderPath[1] == ':') {
        i = 2;
        while (i < n && (folderPath[i] == '\\' || folderPath[i] == '/')) ++i;
    } else if (n >= 2 && folderPath[0] == '\\' && folderPath[1] == '\\') {
        i = 2;
        int headParts = 0;
        while (i < n && headParts < 2) {
            while (i < n && (folderPath[i] == '\\' || folderPath[i] == '/')) ++i;
            if (i >= n) break;
            while (i < n && folderPath[i] != '\\' && folderPath[i] != '/') ++i;
            ++headParts;
        }
        while (i < n && (folderPath[i] == '\\' || folderPath[i] == '/')) ++i;
    } else if (folderPath[0] == '/' || folderPath[0] == '\\') {
        i = 1;
        while (i < n && (folderPath[i] == '\\' || folderPath[i] == '/')) ++i;
    }
    bool inSegment = false;
    for (; i < n; ++i) {
        const bool isSep = (folderPath[i] == '\\' || folderPath[i] == '/');
        if (isSep) {
            inSegment = false;
            continue;
        }
        if (!inSegment) {
            ++depth;
            inSegment = true;
        }
    }
    return depth;
}

static bool IsPureFolderFacetFilter(const SearchFilter& filter) {
    // Path-only filters and the unfiltered global scope may use the materialized view.
    // An empty folderPath represents global scope: empty scope_path and depth zero.
    return filter.keyword.empty() &&
           filter.category.empty() &&
           filter.minDuration <= 0.0f &&
           filter.maxDuration <= 0.0f &&
           filter.minSampleRate == 0 &&
           filter.maxSampleRate == 0 &&
           filter.artist.empty() &&
           filter.artists.empty() &&
           filter.album.empty() &&
           filter.format.empty() &&
           filter.channels == 0 &&
           filter.minChannels == 0 &&
           filter.maxChannels == 0 &&
           filter.channelCategory == SearchFilter::ChannelCategory::Any &&
           filter.coverFilter == 0 &&
           filter.specificCoverKey.empty() &&
           filter.minBpm <= 0.0f &&
           filter.maxBpm <= 0.0f &&
           filter.musicalKey.empty() &&
           filter.excludeExtensions.empty() &&
           !filter.includeOnlyKnownExts &&
           !filter.unplayedOnly &&
           !filter.playedOnlyAny &&
           !filter.highlyRatedOnly &&
           filter.addedAfter <= 0 &&
           filter.playedAfter <= 0 &&
           filter.includeGenres.empty() &&
           filter.excludeGenres.empty() &&
           filter.includeInstruments.empty() &&
           filter.excludeInstruments.empty();
}

static bool IsFileTypeOnlyFilter(const SearchFilter& filter) {
    return filter.keyword.empty() &&
           filter.category.empty() &&
           filter.minDuration <= 0.0f &&
           filter.maxDuration <= 0.0f &&
           filter.minSampleRate == 0 &&
           filter.maxSampleRate == 0 &&
           filter.artist.empty() &&
           filter.artists.empty() &&
           filter.album.empty() &&
           filter.format.empty() &&
           filter.channels == 0 &&
           filter.minChannels == 0 &&
           filter.maxChannels == 0 &&
           filter.channelCategory == SearchFilter::ChannelCategory::Any &&
           filter.coverFilter == 0 &&
           filter.specificCoverKey.empty() &&
           filter.minBpm <= 0.0f &&
           filter.maxBpm <= 0.0f &&
           filter.musicalKey.empty() &&
           !filter.unplayedOnly &&
           !filter.playedOnlyAny &&
           !filter.highlyRatedOnly &&
           filter.addedAfter <= 0 &&
           filter.playedAfter <= 0 &&
           filter.includeGenres.empty() &&
           filter.excludeGenres.empty() &&
           filter.includeInstruments.empty() &&
           filter.excludeInstruments.empty();
}

static int ReadAppMetaInt(sqlite3* db, const char* key) {
    if (!db || !key || !*key) return 0;
    const char* sql = "SELECT value FROM app_meta WHERE key = ? LIMIT 1;";
    sqlite3_stmt* stmt = nullptr;
    int value = 0;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, key, -1, SQLITE_TRANSIENT);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            const unsigned char* text = sqlite3_column_text(stmt, 0);
            if (text) value = std::atoi(reinterpret_cast<const char*>(text));
        }
    }
    if (stmt) sqlite3_finalize(stmt);
    return value;
}

static bool WriteAppMetaInt(sqlite3* db, const char* key, int value) {
    if (!db || !key || !*key) return false;
    const char* sql =
        "INSERT INTO app_meta(key, value) VALUES(?, ?) "
        "ON CONFLICT(key) DO UPDATE SET value = excluded.value;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(stmt, 1, key, -1, SQLITE_TRANSIENT);
    std::string valueStr = std::to_string(value);
    sqlite3_bind_text(stmt, 2, valueStr.c_str(), -1, SQLITE_TRANSIENT);
    bool ok = (sqlite3_step(stmt) == SQLITE_DONE);
    sqlite3_finalize(stmt);
    return ok;
}

static std::string ReadAppMetaString(sqlite3* db, const char* key) {
    if (!db || !key || !*key) return "";
    const char* sql = "SELECT value FROM app_meta WHERE key = ? LIMIT 1;";
    sqlite3_stmt* stmt = nullptr;
    std::string value;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, key, -1, SQLITE_TRANSIENT);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            const unsigned char* text = sqlite3_column_text(stmt, 0);
            if (text) value = reinterpret_cast<const char*>(text);
        }
    }
    if (stmt) sqlite3_finalize(stmt);
    return value;
}

static bool WriteAppMetaString(sqlite3* db, const char* key, const std::string& value) {
    if (!db || !key || !*key) return false;
    const char* sql =
        "INSERT INTO app_meta(key, value) VALUES(?, ?) "
        "ON CONFLICT(key) DO UPDATE SET value = excluded.value;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(stmt, 1, key, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, value.c_str(), -1, SQLITE_TRANSIENT);
    bool ok = (sqlite3_step(stmt) == SQLITE_DONE);
    sqlite3_finalize(stmt);
    return ok;
}

namespace fs = std::filesystem;

namespace {
constexpr const char* kLibraryRootsMetaKey = "library_roots_v1";
constexpr const char* kWaveformCacheModeMetaKey = "waveform_cache_mode_v1";
constexpr const char* kMiniWaveformFormatV2MetaKey = "mini_waveform_format_v2";

static std::string NormalizeLibraryPathUtf8(std::string s, bool lowerCaseForCompare) {
    if (s.empty()) return "";
#ifdef _WIN32
    if (s.rfind("\\\\?\\UNC\\", 0) == 0) {
        s = "\\\\" + s.substr(8);
    } else if (s.rfind("\\\\?\\", 0) == 0) {
        s = s.substr(4);
    }
    std::replace(s.begin(), s.end(), '/', '\\');
    while (s.size() > 3 && !s.empty() && (s.back() == '\\' || s.back() == '/')) {
        s.pop_back();
    }
    if (lowerCaseForCompare) {
        std::transform(s.begin(), s.end(), s.begin(),
                       [](unsigned char c) { return (char)std::tolower(c); });
    }
#else
    std::replace(s.begin(), s.end(), '\\', '/');
    while (s.size() > 1 && !s.empty() && s.back() == '/') {
        s.pop_back();
    }
#endif
    return s;
}

static std::vector<std::string> ParseLibraryRoots(std::string raw) {
    std::vector<std::string> roots;
    // Store paths with their original case so filesystem enumeration matches
    // case-sensitive file_path values. Lowercase copies are used only as
    // deduplication keys.

    size_t start = 0;
    std::set<std::string> dedupKeys;
    while (start <= raw.size()) {
        size_t end = raw.find('\n', start);
        std::string item = raw.substr(start, end == std::string::npos ? std::string::npos : end - start);
        const std::string preserved = NormalizeLibraryPathUtf8(item, /*lowerCaseForCompare=*/false);
        if (!preserved.empty()) {
            const std::string key = NormalizeLibraryPathUtf8(item, /*lowerCaseForCompare=*/true);
            if (dedupKeys.insert(key).second) roots.push_back(preserved);
        }
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return roots;
}

static bool IsPathInsideLibraryRoots(const std::string& pathUtf8,
                                     const std::vector<std::string>& rootsAnyCase) {
    if (rootsAnyCase.empty()) return true;
    const std::string pathNorm = NormalizeLibraryPathUtf8(pathUtf8, true);
    if (pathNorm.empty()) return false;
    // Prefix comparison uses lowercase normalized copies while persisted roots
    // retain their original case.

    for (const std::string& rootAny : rootsAnyCase) {
        const std::string rootNorm = NormalizeLibraryPathUtf8(rootAny, true);
        if (rootNorm.empty()) continue;
        if (pathNorm == rootNorm) return true;
        if (pathNorm.size() <= rootNorm.size()) continue;
        if (pathNorm.compare(0, rootNorm.size(), rootNorm) != 0) continue;
        const char sep = pathNorm[rootNorm.size()];
        if (sep == '\\' || sep == '/') return true;
    }
    return false;
}

struct CoverFacetCandidate {
    std::string key;
    std::string path;
    std::string sourcePath;
    bool isEmbedded = false;
    int64_t count = 0;
    int64_t phash = 0;
};

struct CoverFacetCluster {
    CoverFacetCandidate representative;
    int64_t totalCount = 0;
    std::vector<int64_t> phashes;
    std::vector<std::string> keys;
};
} // namespace

static std::vector<CoverFacetCandidate> ClusterCoverCandidates(
    const std::vector<CoverFacetCandidate>& candidates,
    int limit,
    bool sortByCount,
    bool mergeSimilarPhashes = true);
static std::vector<std::string> ResolveEquivalentCoverKeys(Database* db, const std::string& coverKey);
static std::string CanonicalizeCoverLookupKey(const std::string& coverKey);
static bool EnsureCoverTileMembershipForKeys(sqlite3* db,
                                             const std::vector<std::string>& tileKeys,
                                             std::vector<int64_t>* out = nullptr);
static void RebuildCoverTileMembershipIndex(sqlite3* db);
static bool TryExtractTemporaryFolderIdentityPath(const std::string& coverKey,
                                                  std::string* outCanonicalFolderPath);
static bool TryExtractCanonicalFolderCoverSourcePath(const std::string& coverKey,
                                                     std::string* outCanonicalSourcePath);

struct ExactCoverWallSelectionMembers {
    std::vector<std::string> keys;
    std::vector<std::string> temporaryFolders;
};

static bool ParseExactCoverWallSelection(const std::string& selection,
                                         ExactCoverWallSelectionMembers& members) {
    std::vector<std::string> encodedKeys;
    if (!aura::cover_wall::DecodeCoverWallSelectionKey(selection, encodedKeys)) return false;
    std::unordered_set<std::string> seenKeys;
    std::unordered_set<std::string> seenFolders;
    for (const auto& rawKey : encodedKeys) {
        std::string folder;
        if (TryExtractTemporaryFolderIdentityPath(rawKey, &folder)) {
            if (seenFolders.insert(folder).second) members.temporaryFolders.push_back(std::move(folder));
        } else {
            const std::string key = CanonicalizeCoverLookupKey(rawKey);
            if (!key.empty() && seenKeys.insert(key).second) members.keys.push_back(key);
        }
    }
    return !members.keys.empty() || !members.temporaryFolders.empty();
}

static std::string CoverWallSelectionJsonArray(const std::vector<std::string>& values) {
    static constexpr char hex[] = "0123456789abcdef";
    std::string json = "[";
    for (const auto& value : values) {
        if (json.size() > 1) json += ',';
        json += '"';
        for (const unsigned char c : value) {
            if (c == '"' || c == '\\') { json += '\\'; json += static_cast<char>(c); }
            else if (c < 0x20) {
                json += "\\u00";
                json += hex[c >> 4];
                json += hex[c & 15];
            } else json += static_cast<char>(c);
        }
        json += '"';
    }
    json += ']';
    return json;
}

static void ExpandExactCoverWallRegistryAliases(sqlite3* db,
                                                ExactCoverWallSelectionMembers& members) {
    if (!db || members.keys.empty()) return;
    const char* sql =
        "SELECT cover_key FROM cover_cache_registry "
        "WHERE normalized_key IN (SELECT value FROM json_each(?))";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return;
    const std::string normalizedKeys = CoverWallSelectionJsonArray(members.keys);
    sqlite3_bind_text(stmt, 1, normalizedKeys.c_str(), (int)normalizedKeys.size(), SQLITE_TRANSIENT);
    std::unordered_set<std::string> seen(members.keys.begin(), members.keys.end());
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char* text = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        const std::string raw = text ? text : "";
        if (!raw.empty() && seen.insert(raw).second) members.keys.push_back(raw);
    }
    sqlite3_finalize(stmt);
}

static void AccumulateSemicolonFacetTokens(const std::string& rawValue,
                                           std::unordered_map<std::string, int>* counts) {
    if (!counts || rawValue.empty()) return;
    size_t pos = 0;
    while (pos < rawValue.size()) {
        size_t end = rawValue.find(';', pos);
        if (end == std::string::npos) end = rawValue.size();
        std::string token = rawValue.substr(pos, end - pos);
        const size_t startTrim = token.find_first_not_of(" \t");
        const size_t endTrim = token.find_last_not_of(" \t");
        if (startTrim != std::string::npos && endTrim != std::string::npos) {
            token = token.substr(startTrim, endTrim - startTrim + 1);
            if (!token.empty()) {
                ++(*counts)[token];
            }
        }
        pos = end + 1;
    }
}

// Extended metadata column policy.
// rescanOverwrite fields refresh on rescan; false preserves user edits.
// searchable fields contribute to the aggregated search_data column.
struct ExtMetaCol { const char* col; bool rescanOverwrite; bool searchable; };
static const ExtMetaCol kExtMetaCols[] = {
    // Basic / Common
    {"subcategory",        true,  true },
    {"source",             true,  false},
    {"cat_id",             true,  false},
    {"category_full",      true,  false},
    {"fx_name",            true,  true },
    {"notes",              true,  false},
    {"user_comments",      false, true },
    {"url",                true,  false},
    {"user_category",      false, false},
    {"vendor_category",    true,  false},
    {"open_tier",          true,  false},
    {"genre",              true,  false},
    {"comment",            true,  false},
    {"copyright",          true,  false},
    {"software",           true,  false},
    // Library / Album
    {"library",            true,  true },
    {"volume",             true,  false},
    {"cd_title",           true,  true },
    {"cd_description",     true,  false},
    {"manufacturer",       true,  false},
    // Music
    {"composer",           true,  true },
    {"arranger",           true,  false},
    {"conductor",          true,  false},
    {"performer",          true,  false},
    {"publisher",          true,  false},
    {"bpm",                true,  false},
    {"musical_key",        true,  false},
    {"tempo",              true,  false},
    {"mood",               false, true },
    {"usage",              false, false},
    {"featured_instrument",true,  false},
    {"version_tag",        true,  false},
    {"era",                true,  false},
    {"track_year",         true,  false},
    {"isrc",               true,  false},
    {"iswc",               true,  false},
    {"lyrics",             true,  false},
    // SFX / Post
    {"show_name",          true,  true },
    {"show_category",      true,  false},
    {"show_subcategory",   true,  false},
    {"designer",           true,  true },
    {"location",           true,  false},
    {"microphone",         true,  false},
    {"mic_perspective",    true,  false},
    {"rec_type",           true,  false},
    {"rec_medium",         true,  false},
    {"short_id",           true,  true },
    {"long_id",            true,  false},
    // Production
    {"scene",              true,  false},
    {"take_num",           true,  false},
    {"tape",               true,  false},
    // BWF / BEXT
    {"bw_description",     true,  false},
    {"bw_originator",      true,  false},
    {"bw_originator_ref",  true,  false},
    {"bw_date",            true,  false},
    {"bw_time",            true,  false},
    {"bw_umid",            true,  false},
    {"bw_coding_history",  true,  false},
    // iXML
    {"ixml_project",       true,  false},
    {"ixml_note",          true,  false},
    {"ixml_file_uid",      true,  false},
    {"ixml_track_layout",  true,  false},
    // Location
    {"shoot_date",         true,  false},
    // System / User
    {"track_colour",       false, false},
};
static constexpr size_t kExtMetaCount = sizeof(kExtMetaCols) / sizeof(kExtMetaCols[0]);

// Columns that MetadataEditPatch may update after whitelist validation.
static const std::unordered_set<std::string>& EditableTextColumns() {
    static const std::unordered_set<std::string> s = []() {
        std::unordered_set<std::string> t;
        t.insert({"description", "keywords", "category", "title", "artist", "album"});
        for (size_t i = 0; i < kExtMetaCount; ++i) t.insert(kExtMetaCols[i].col);
        return t;
    }();
    return s;
}
static const std::unordered_set<std::string>& EditableIntColumns() {
    static const std::unordered_set<std::string> s = {"rating"};
    return s;
}

// Record columns used by list queries; large waveform and RMS BLOBs are fetched separately.
// These BLOB columns are fetched separately on demand.
static const char* kRecordColumns =
    "id, file_path, file_name, extension, file_size, folder_path, file_mtime, last_indexed, "
    "sample_rate, bit_depth, channels, total_frames, duration, format, "
    "title, artist, album, description, category, keywords, rating, "
    "play_count, date_added, last_played, has_cover, "
    "embedded_cover_key, embedded_cover_key_mtime, folder_cover_key, folder_cover_src_path, folder_cover_src_mtime, cover_suppressed, "
    "integrated_lufs, momentary_lufs, short_term_lufs, true_peak_dbtp, "
    // Extended library metadata fields
    "subcategory, source, cat_id, category_full, fx_name, notes, user_comments, url, user_category, vendor_category, open_tier, "
    "genre, comment, copyright, software, "
    "library, volume, cd_title, cd_description, manufacturer, "
    "composer, arranger, conductor, performer, publisher, bpm, musical_key, tempo, mood, usage, featured_instrument, version_tag, era, track_year, isrc, iswc, lyrics, "
    "show_name, show_category, show_subcategory, designer, location, microphone, mic_perspective, rec_type, rec_medium, short_id, long_id, "
    "scene, take_num, tape, "
    "bw_description, bw_originator, bw_originator_ref, bw_date, bw_time, bw_umid, bw_coding_history, "
    "ixml_project, ixml_note, ixml_file_uid, ixml_track_layout, "
    "shoot_date, track_colour, "
    // F-033 Music Mode extended fields
    "album_artist, track_number, disc_number, "
    "energy_tension, energy_drive, "
    "replay_gain_track, replay_gain_album, highlight_start_ms, "
    "raw_genre, raw_instruments, norm_genre, norm_instruments, norm_mood_tags, vocal_type, "
// F-033 Music analysis confidence + derived fields (must follow all other columns).
    "musical_key_camelot, key_confidence, key_source, bpm_confidence, bpm_source, "
    "mood_valence, mood_arousal, mood_source";

// Same columns with the af. alias for joins.
static const char* kRecordColumnsAf =
    "af.id, af.file_path, af.file_name, af.extension, af.file_size, af.folder_path, af.file_mtime, af.last_indexed, "
    "af.sample_rate, af.bit_depth, af.channels, af.total_frames, af.duration, af.format, "
    "af.title, af.artist, af.album, af.description, af.category, af.keywords, af.rating, "
    "af.play_count, af.date_added, af.last_played, af.has_cover, "
    "af.embedded_cover_key, af.embedded_cover_key_mtime, af.folder_cover_key, af.folder_cover_src_path, af.folder_cover_src_mtime, af.cover_suppressed, "
    "af.integrated_lufs, af.momentary_lufs, af.short_term_lufs, af.true_peak_dbtp, "
    // Extended library metadata fields
    "af.subcategory, af.source, af.cat_id, af.category_full, af.fx_name, af.notes, af.user_comments, af.url, af.user_category, af.vendor_category, af.open_tier, "
    "af.genre, af.comment, af.copyright, af.software, "
    "af.library, af.volume, af.cd_title, af.cd_description, af.manufacturer, "
    "af.composer, af.arranger, af.conductor, af.performer, af.publisher, af.bpm, af.musical_key, af.tempo, af.mood, af.usage, af.featured_instrument, af.version_tag, af.era, af.track_year, af.isrc, af.iswc, af.lyrics, "
    "af.show_name, af.show_category, af.show_subcategory, af.designer, af.location, af.microphone, af.mic_perspective, af.rec_type, af.rec_medium, af.short_id, af.long_id, "
    "af.scene, af.take_num, af.tape, "
    "af.bw_description, af.bw_originator, af.bw_originator_ref, af.bw_date, af.bw_time, af.bw_umid, af.bw_coding_history, "
    "af.ixml_project, af.ixml_note, af.ixml_file_uid, af.ixml_track_layout, "
    "af.shoot_date, af.track_colour, "
    // F-033 Music Mode extended fields
    "af.album_artist, af.track_number, af.disc_number, "
    "af.energy_tension, af.energy_drive, "
    "af.replay_gain_track, af.replay_gain_album, af.highlight_start_ms, "
    "af.raw_genre, af.raw_instruments, af.norm_genre, af.norm_instruments, af.norm_mood_tags, af.vocal_type, "

    "af.musical_key_camelot, af.key_confidence, af.key_source, af.bpm_confidence, af.bpm_source, "
    "af.mood_valence, af.mood_arousal, af.mood_source";

// Forward declaration (used by schema helpers below)
static std::string safeGetText(sqlite3_stmt* stmt, int col);
static std::string MakeFolderPathUpperBound(const std::string& folderPath);

// PathToUtf8 / Utf8ToPath are declared in Utils.h and defined in Utils.cpp

#ifdef _WIN32
static std::wstring EnsureLongPathPrefix(std::wstring p) {
    if (p.empty()) return p;
    // Already extended-length
    if (p.rfind(L"\\\\?\\", 0) == 0) return p;
    // UNC path \\server\share -> \\?\UNC\server\share
    if (p.rfind(L"\\\\", 0) == 0) {
        return L"\\\\?\\UNC\\" + p.substr(2);
    }
    // Drive absolute path C:\...
    if (p.size() >= 2 && std::iswalpha(p[0]) && p[1] == L':') {
        return L"\\\\?\\" + p;
    }
    // Relative paths: don't prefix
    return p;
}

static fs::path MakeLongPath(const fs::path& p) {
    std::wstring w = p.wstring();
    return fs::path(EnsureLongPathPrefix(std::move(w)));
}
#else
static fs::path MakeLongPath(const fs::path& p) { return p; }
#endif

static bool AreWaveformCacheKeysEquivalentForScan(const std::string& storedKey,
                                                  const std::string& currentKey) {
    if (storedKey == currentKey) return true;
    if (storedKey.empty() || currentKey.empty()) return false;

    const size_t storedLastColon = storedKey.find_last_of(':');
    const size_t currentLastColon = currentKey.find_last_of(':');
    if (storedLastColon == std::string::npos ||
        currentLastColon == std::string::npos ||
        storedLastColon != currentLastColon ||
        storedKey.compare(0, storedLastColon + 1, currentKey, 0, currentLastColon + 1) != 0) {
        return false;
    }

    try {
        const int64_t storedMtime = std::stoll(storedKey.substr(storedLastColon + 1));
        const int64_t currentMtime = std::stoll(currentKey.substr(currentLastColon + 1));
        const int64_t delta = storedMtime - currentMtime;
        return delta >= -1 && delta <= 1;
    } catch (...) {
        return false;
    }
}

// Audio file extensions
static const std::set<std::string> AUDIO_EXTENSIONS = {
    ".wav", ".wave", ".mp3", ".flac", ".ogg", ".aiff", ".aif", ".m4a", ".wma"
};

static bool isAudioFile(const std::string& ext) {
    std::string lower = ext;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    return AUDIO_EXTENSIONS.find(lower) != AUDIO_EXTENSIONS.end();
}

// 系统/临时/回收站等绝不应该扫描的目录名（大小写不敏感比较）
static bool isSystemDirectoryName(const std::wstring& name) {
    // Compare the directory name in lowercase.
    std::wstring lower = name;
    for (auto& c : lower) c = towlower(c);
    // Windows 系统目录
    if (lower == L"$recycle.bin") return true;
    if (lower == L"system volume information") return true;
    if (lower == L"$windows.~bt") return true;
    if (lower == L"$windows.~ws") return true;
    if (lower == L"windows") return true;
    // Common macOS residue in cross-platform audio libraries.
    if (lower == L"__macosx") return true;
    if (lower == L".spotlight-v100") return true;
    if (lower == L".fseventsd") return true;
    if (lower == L".trashes") return true;
    return false;
}

// Returns true for paths that should not be scanned: hidden, system, temporary, or non-audio.
static bool isHiddenPath(const fs::path& p) {
    // 1. Dot-prefixed names, excluding . and ...
    std::string name = p.filename().string();
    if (!name.empty() && name[0] == '.' && name != "." && name != "..") {
        return true;
    }

    // 2. macOS AppleDouble resource-fork files.
    // Plain ._name files are already caught by the dot-prefix rule.
    // Renaming rules may produce names such as "[tag] ._name.wav", which begin with [.
    // Check for the AppleDouble marker separately so renamed resource forks are still skipped.
    // Match either a leading "._" or "] ._" inside a renamed file name.
    {
        auto pos = name.find("._");
        if (pos != std::string::npos) {
            // A leading ._ is already covered; keep this as a defensive check.
            if (pos == 0) return true;
            // Renamed forms place a space immediately before ._.
            if (pos > 0 && name[pos - 1] == ' ') return true;
        }
    }

    // 3. Known system and recycle-bin directory names.
    try {
        std::wstring wname = p.filename().wstring();
        if (isSystemDirectoryName(wname)) return true;
    } catch (...) {}

    // 4. Windows hidden, system, or temporary attributes.
    try {
        const auto attributes =
            Platform::FileSystem::QueryPathAttributes(PathToUtf8(p));
        if (attributes.hidden || attributes.system || attributes.temporary) {
            return true;
        }
    } catch (...) {}
    return false;
}

// Files below 128 bytes cannot contain a valid audio header; a WAV header alone is 44 bytes.
static const uint64_t kMinAudioFileSize = 128;

static int64_t getCurrentTimestamp() {
    return static_cast<int64_t>(std::time(nullptr));
}

static int64_t fileTimeToUnixSeconds(const fs::file_time_type& ftime) {
    // Convert file_clock to system_clock under C++17.
    using namespace std::chrono;
    auto sctp = time_point_cast<seconds>(ftime - fs::file_time_type::clock::now() + system_clock::now());
    return (int64_t)sctp.time_since_epoch().count();
}

static std::string buildTableInfoPragmaSql(const char* table) {
    if (!table || !*table) return {};
    const std::string qualified = table;
    const size_t dot = qualified.find('.');
    if (dot == std::string::npos) return std::string("PRAGMA table_info(") + qualified + ");";
    const std::string schema = qualified.substr(0, dot);
    const std::string tableName = qualified.substr(dot + 1);
    if (schema.empty() || tableName.empty()) return {};
    return std::string("PRAGMA ") + schema + ".table_info(" + tableName + ");";
}

static std::string buildAlterTableAddColumnSql(const char* table, const char* column, const char* decl) {
    if (!table || !*table || !column || !*column || !decl || !*decl) return {};
    const std::string qualified = table;
    const size_t dot = qualified.find('.');
    if (dot == std::string::npos) {
        return std::string("ALTER TABLE ") + qualified + " ADD COLUMN " + column + " " + decl + ";";
    }
    const std::string schema = qualified.substr(0, dot);
    const std::string tableName = qualified.substr(dot + 1);
    if (schema.empty() || tableName.empty()) return {};
    return std::string("ALTER TABLE \"") + schema + "\".\"" + tableName + "\" ADD COLUMN " + column + " " + decl + ";";
}

static bool columnExists(sqlite3* db, const char* table, const char* column) {
    if (!db || !table || !*table || (table[0] == '.' && (!table[1] || table[1] == '\0'))) return false;
    std::string sql = buildTableInfoPragmaSql(table);
    if (sql.empty()) return false;
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) return false;
    bool found = false;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        std::string name = safeGetText(stmt, 1); // column name
        if (name == column) { found = true; break; }
    }
    sqlite3_finalize(stmt);
    return found;
}

static bool ensureColumn(sqlite3* db, const char* table, const char* column, const char* decl) {
    if (columnExists(db, table, column)) return true;
    std::string sql = buildAlterTableAddColumnSql(table, column, decl);
    if (sql.empty()) return false;
    char* errMsg = nullptr;
    int rc = sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &errMsg);
    if (rc != SQLITE_OK) {
        if (errMsg) sqlite3_free(errMsg);
        return false;
    }
    return true;
}

static bool tableExists(sqlite3* db, const char* name) {
    if (!db) return false;
    const char* sql = "SELECT 1 FROM sqlite_master WHERE type='table' AND name=? LIMIT 1;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(stmt, 1, name, -1, SQLITE_TRANSIENT);
    bool ok = (sqlite3_step(stmt) == SQLITE_ROW);
    sqlite3_finalize(stmt);
    return ok;
}

static bool tableExistsInSchema(sqlite3* db, const char* schemaName, const char* tableName) {
    if (!db || !schemaName || !*schemaName || !tableName || !*tableName) return false;
    std::string sql = "SELECT 1 FROM ";
    sql += schemaName;
    sql += ".sqlite_master WHERE type='table' AND name=? LIMIT 1;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(stmt, 1, tableName, -1, SQLITE_TRANSIENT);
    const bool ok = (sqlite3_step(stmt) == SQLITE_ROW);
    sqlite3_finalize(stmt);
    return ok;
}

static bool execSqlite(sqlite3* db, const char* sql, std::string* outError) {
    if (!db) return false;
    char* errMsg = nullptr;
    int rc = sqlite3_exec(db, sql, nullptr, nullptr, &errMsg);
    if (rc != SQLITE_OK) {
        if (outError) *outError = errMsg ? errMsg : sqlite3_errmsg(db);
        if (errMsg) sqlite3_free(errMsg);
        return false;
    }
    return true;
}

static void DiscardSearchDataRecomputeScratch(sqlite3* db) {
    if (!db) return;
    // A transaction on this shared connection may belong to the caller.
    if (sqlite3_get_autocommit(db) == 0) return;
    execSqlite(db, "DROP TABLE IF EXISTS temp.search_data_recompute_work;", nullptr);
    execSqlite(db, "DELETE FROM app_meta WHERE key = 'search_data_recompute_cursor';", nullptr);
}

static std::vector<std::string> getTableColumns(sqlite3* db, const char* table) {
    std::vector<std::string> cols;
    if (!db || !table || !*table || (table[0] == '.' && (!table[1] || table[1] == '\0'))) return cols;
    std::string sql = buildTableInfoPragmaSql(table);
    if (sql.empty()) return cols;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) return cols;
    while (sqlite3_step(st) == SQLITE_ROW) {
        const unsigned char* t = sqlite3_column_text(st, 1); // name
        if (t) cols.emplace_back(reinterpret_cast<const char*>(t));
    }
    sqlite3_finalize(st);
    return cols;
}

static bool columnsMatchExact(const std::vector<std::string>& actual, const std::vector<std::string>& expected) {
    if (actual.size() != expected.size()) return false;
    for (size_t i = 0; i < expected.size(); ++i) {
        if (actual[i] != expected[i]) return false;
    }
    return true;
}

static bool ftsHasRows(sqlite3* db) {
    if (!db) return false;
    // A content='audio_files' FTS5 table forwards ordinary SELECTs to the source table.
    // Inspect the _idx shadow table to confirm that the FTS B-tree has actually been populated.
    sqlite3_stmt* st = nullptr;
    bool hasData = false;
    if (sqlite3_prepare_v2(db, "SELECT 1 FROM audio_files_fts_idx LIMIT 1;", -1, &st, nullptr) == SQLITE_OK) {
        if (sqlite3_step(st) == SQLITE_ROW) hasData = true;
    }
    if (st) sqlite3_finalize(st);
    return hasData;
}

static std::string firstToken(const std::string& s) {
    size_t i = 0;
    while (i < s.size() && std::isspace((unsigned char)s[i])) ++i;
    size_t start = i;
    while (i < s.size() && !std::isspace((unsigned char)s[i])) ++i;
    return s.substr(start, i - start);
}

static std::string getCreateSql(sqlite3* db, const char* name) {
    if (!db || !name) return {};
    std::string sql = "SELECT sql FROM sqlite_master WHERE type='table' AND name=?;";
    sqlite3_stmt* st = nullptr;
    std::string out;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) return out;
    sqlite3_bind_text(st, 1, name, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(st) == SQLITE_ROW) {
        const unsigned char* s = sqlite3_column_text(st, 0);
        if (s) out = reinterpret_cast<const char*>(s);
    }
    sqlite3_finalize(st);
    return out;
}

static std::string toLowerAsciiCopy(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

static bool IsRecoverableSearchIndexMalformedError(const std::string& error) {
    if (error.empty()) return false;
    const std::string lower = toLowerAsciiCopy(error);
    return lower.find("database disk image is malformed") != std::string::npos ||
           lower.find("malformed inverted index for fts5 table") != std::string::npos;
}

static bool DropSearchIndexTablesForRecovery(sqlite3* db, std::string* outError) {
    const char* sql = R"(
        DROP TRIGGER IF EXISTS audio_files_ai;
        DROP TRIGGER IF EXISTS audio_files_ad;
        DROP TRIGGER IF EXISTS audio_files_au;
        DROP TRIGGER IF EXISTS audio_files_ai_trigram;
        DROP TRIGGER IF EXISTS audio_files_ad_trigram;
        DROP TRIGGER IF EXISTS audio_files_au_trigram;
        DROP TABLE IF EXISTS audio_files_vocab;
        DROP TABLE IF EXISTS audio_files_fts;
        DROP TABLE IF EXISTS audio_files_name_trigram;
    )";
    return execSqlite(db, sql, outError);
}

bool Database::requireFts5() {
    if (!m_db) return false;

    // Robust runtime check: attempt to create a TEMP fts5 table.
    // This works even if PRAGMA compile_options is unavailable/odd.
    std::string err;
    const char* probe = R"(
        CREATE VIRTUAL TABLE temp.__fts5_probe USING fts5(x);
        DROP TABLE temp.__fts5_probe;
    )";
    if (!execSqlite(m_db, probe, &err)) {
        m_lastError = FormatDbError(L(Db_Fts5DisabledFmt), err.c_str());
        return false;
    }
    return true;
}

bool Database::ensureFtsSchema() {
    if (!m_db) return false;
    if (!requireFts5()) return false;

    std::string err;

    // Desired FTS schema (column order matters for PRAGMA table_info on FTS tables).
    const std::vector<std::string> desiredCols = {
        "file_name", "file_path", "folder_path", "title", "artist", "album", "description", "keywords", "category",
        "search_data" // Derived search text
    };

    // If FTS table exists with old schema/options, drop and recreate.
    // SQLite does not support ALTER for virtual FTS5 tables, so we rebuild.
    bool tableWasDropped = false;
    if (tableExists(m_db, "audio_files_fts")) {
        std::vector<std::string> actualCols = getTableColumns(m_db, "audio_files_fts");
        bool needRebuild = (!actualCols.empty() && !columnsMatchExact(actualCols, desiredCols));
        if (!needRebuild) {
            std::string createSql = toLowerAsciiCopy(getCreateSql(m_db, "audio_files_fts"));
            if (createSql.find("prefix=") == std::string::npos) {
                needRebuild = true;
            }
        }
        if (needRebuild) {
            const char* dropOld = R"(
                DROP TRIGGER IF EXISTS audio_files_ai;
                DROP TRIGGER IF EXISTS audio_files_ad;
                DROP TRIGGER IF EXISTS audio_files_au;
                DROP TABLE IF EXISTS audio_files_fts;
            )";
            if (!execSqlite(m_db, dropOld, &err)) {
                m_lastError = FormatDbError(L(Db_FtsUpgradeFailFmt), err.c_str());
                return false;
            }
            tableWasDropped = true;
        }
    } else {
        tableWasDropped = true;
    }

    const char* createFts = R"(
        CREATE VIRTUAL TABLE IF NOT EXISTS audio_files_fts USING fts5(
            file_name, file_path, folder_path, title, artist, album, description, keywords, category, search_data,
            content='audio_files', content_rowid='id',
            prefix='2 3 4'
        );
    )";
    if (!execSqlite(m_db, createFts, &err)) {
        m_lastError = FormatDbError(L(Db_FtsCreateFailFmt), err.c_str());
        return false;
    }
    if (!tableExists(m_db, "audio_files_fts")) {
        m_lastError = "Failed to create FTS table: audio_files_fts not found; FTS5 may be unavailable.";
        return false;
    }

    // Build an fts5vocab shadow table for global search suggestions.
    // type='row' records each term and the number of documents containing it for ranking.
    const char* createVocab = R"(
        CREATE VIRTUAL TABLE IF NOT EXISTS audio_files_vocab USING fts5vocab(audio_files_fts, row);
    )";
    if (!execSqlite(m_db, createVocab, &err)) {
        m_lastError = FormatDbError(L(Db_FtsVocabFailFmt), err.c_str());
        return false;
    }

    // Recreate the triggers and backfill search_data before rebuilding FTS5.
    const char* trig = R"(
        DROP TRIGGER IF EXISTS audio_files_ai;
        DROP TRIGGER IF EXISTS audio_files_ad;
        DROP TRIGGER IF EXISTS audio_files_au;

        CREATE TRIGGER audio_files_ai AFTER INSERT ON audio_files BEGIN
            INSERT INTO audio_files_fts(rowid, file_name, file_path, folder_path, title, artist, album, description, keywords, category, search_data)
            VALUES (new.id, new.file_name, new.file_path, new.folder_path, new.title, new.artist, new.album, new.description, new.keywords, new.category, new.search_data);
        END;
        CREATE TRIGGER audio_files_ad AFTER DELETE ON audio_files BEGIN
            INSERT INTO audio_files_fts(audio_files_fts, rowid, file_name, file_path, folder_path, title, artist, album, description, keywords, category, search_data)
            VALUES('delete', old.id, old.file_name, old.file_path, old.folder_path, old.title, old.artist, old.album, old.description, old.keywords, old.category, old.search_data);
        END;
        CREATE TRIGGER audio_files_au AFTER UPDATE ON audio_files BEGIN
            INSERT INTO audio_files_fts(audio_files_fts, rowid, file_name, file_path, folder_path, title, artist, album, description, keywords, category, search_data)
            VALUES('delete', old.id, old.file_name, old.file_path, old.folder_path, old.title, old.artist, old.album, old.description, old.keywords, old.category, old.search_data);

            INSERT INTO audio_files_fts(rowid, file_name, file_path, folder_path, title, artist, album, description, keywords, category, search_data)
            VALUES (new.id, new.file_name, new.file_path, new.folder_path, new.title, new.artist, new.album, new.description, new.keywords, new.category, new.search_data);
        END;
    )";
    if (!execSqlite(m_db, trig, &err)) {
        m_lastError = FormatDbError(L(Db_FtsTriggerFailFmt), err.c_str());
        return false;
    }

    // Initial backfill for existing rows (external content FTS).
    // 如果刚刚 drop+recreate 了表，必须无条件 rebuild 索引
    // Otherwise inspect the FTS5 _idx shadow table for existing index rows.
    bool needsRebuild = tableWasDropped;
    if (!needsRebuild) {
        needsRebuild = !ftsHasRows(m_db);
    }
    if (needsRebuild) {
        if (!execSqlite(m_db, "INSERT INTO audio_files_fts(audio_files_fts) VALUES('rebuild');", &err)) {
            m_lastError = FormatDbError(L(Db_FtsRebuildFailFmt), err.c_str());
            return false;
        }
    }

    // Trigram sidecar supports token-level substring and suffix search.
    // 例如输入 "30017" 命中 "B30017_Gun.wav"（file_name 子串）或
    // It also matches a term inside folder_path, such as B30017 in a nested library path.
    // External-content FTS5 with tokenize='trigram case_sensitive 0'.
    // Index file_name and folder_path with case-insensitive matching.
    // FTS5 MATCH spans both indexed columns so file-name and folder-path substrings use one query.
    bool trigramWasMissing = !tableExists(m_db, "audio_files_name_trigram");

    // Rebuild the trigram table when any supported schema version drifts.
    // v1 to v2 added folder_path.
    // v2 to v3 added case-insensitive tokenization.
    bool trigramNeedsSchemaRebuild = false;
    if (!trigramWasMissing) {
        std::vector<std::string> cols = getTableColumns(m_db, "audio_files_name_trigram");
        bool hasFileName = std::find(cols.begin(), cols.end(), "file_name") != cols.end();
        bool hasFolderPath = std::find(cols.begin(), cols.end(), "folder_path") != cols.end();
        if (!hasFileName || !hasFolderPath) {
            trigramNeedsSchemaRebuild = true;
        } else {
            // Check that the tokenizer includes case_sensitive 0.
            std::string createSql = toLowerAsciiCopy(getCreateSql(m_db, "audio_files_name_trigram"));
            // case_sensitive 0 / case_sensitive=0 / case_sensitive '0' 都算通过
            bool hasCaseInsensitive =
                createSql.find("case_sensitive 0") != std::string::npos ||
                createSql.find("case_sensitive=0") != std::string::npos ||
                createSql.find("case_sensitive '0'") != std::string::npos;
            if (!hasCaseInsensitive) {
                trigramNeedsSchemaRebuild = true;
            }
        }
    }
    if (trigramNeedsSchemaRebuild) {
        const char* dropOldTrigram = R"(
            DROP TRIGGER IF EXISTS audio_files_ai_trigram;
            DROP TRIGGER IF EXISTS audio_files_ad_trigram;
            DROP TRIGGER IF EXISTS audio_files_au_trigram;
            DROP TABLE IF EXISTS audio_files_name_trigram;
        )";
        if (!execSqlite(m_db, dropOldTrigram, &err)) {
            m_lastError = FormatDbError(L(Db_TrigramUpgradeFailFmt), err.c_str());
            return false;
        }
        trigramWasMissing = true;  // IF NOT EXISTS recreates it below.
    }

    const char* createTrigram = R"(
        CREATE VIRTUAL TABLE IF NOT EXISTS audio_files_name_trigram USING fts5(
            file_name, folder_path,
            content='audio_files', content_rowid='id',
            tokenize='trigram case_sensitive 0'
        );
    )";
    if (!execSqlite(m_db, createTrigram, &err)) {
        m_lastError = FormatDbError(L(Db_TrigramCreateFailFmt), err.c_str());
        return false;
    }

    const char* trigramTriggers = R"(
        DROP TRIGGER IF EXISTS audio_files_ai_trigram;
        DROP TRIGGER IF EXISTS audio_files_ad_trigram;
        DROP TRIGGER IF EXISTS audio_files_au_trigram;

        CREATE TRIGGER audio_files_ai_trigram AFTER INSERT ON audio_files BEGIN
            INSERT INTO audio_files_name_trigram(rowid, file_name, folder_path)
            VALUES (new.id, new.file_name, new.folder_path);
        END;
        CREATE TRIGGER audio_files_ad_trigram AFTER DELETE ON audio_files BEGIN
            INSERT INTO audio_files_name_trigram(audio_files_name_trigram, rowid, file_name, folder_path)
            VALUES('delete', old.id, old.file_name, old.folder_path);
        END;
        CREATE TRIGGER audio_files_au_trigram AFTER UPDATE ON audio_files BEGIN
            INSERT INTO audio_files_name_trigram(audio_files_name_trigram, rowid, file_name, folder_path)
            VALUES('delete', old.id, old.file_name, old.folder_path);
            INSERT INTO audio_files_name_trigram(rowid, file_name, folder_path)
            VALUES (new.id, new.file_name, new.folder_path);
        END;
    )";
    if (!execSqlite(m_db, trigramTriggers, &err)) {
        m_lastError = FormatDbError(L(Db_TrigramTriggerFailFmt), err.c_str());
        return false;
    }

    // One-time migration and backfill history:
    // v1 indexed file_name with case-sensitive tokenization.
    // v2 added folder_path.
    // v3 added case_sensitive 0 so lowercase queries match uppercase content.
    // Rebuild when:
    // - the trigram table was absent or dropped for schema drift;
    // - the migration version differs from the current version; or
    // - the primary FTS index was just rebuilt.
    const int kTrigramMigrationVersion = 3;
    bool needsTrigramRebuild = trigramWasMissing || tableWasDropped ||
        (ReadAppMetaInt(m_db, "file_name_trigram_migrated") != kTrigramMigrationVersion);
    if (needsTrigramRebuild) {
        if (!execSqlite(m_db,
                "INSERT INTO audio_files_name_trigram(audio_files_name_trigram) VALUES('rebuild');",
                &err)) {
            m_lastError = FormatDbError(L(Db_TrigramRebuildFailFmt), err.c_str());
            return false;
        }
        WriteAppMetaInt(m_db, "file_name_trigram_migrated", kTrigramMigrationVersion);
    }

    return true;
}

static bool readAudioFileInfo(const fs::path& path,
                              uint32_t& sampleRate, uint32_t& channels,
                              uint64_t& totalFrames, float& durationSeconds) {
    Platform::DatabaseAudioProbe::Info info;
    const bool ok = Platform::DatabaseAudioProbe::Probe(path, info);
    sampleRate = info.sampleRate;
    channels = info.channels;
    totalFrames = info.totalFrames;
    durationSeconds = info.durationSeconds;
    return ok;
}

static void ApplyAudioMetadataToRecord(AudioRecord& record, const AudioMetadata& metadata) {
    if (!metadata.title.empty()) record.title = metadata.title;
    if (!metadata.artist.empty()) record.artist = metadata.artist;
    if (!metadata.album.empty()) record.album = metadata.album;
    if (!metadata.albumArtist.empty()) record.albumArtist = metadata.albumArtist;
    if (metadata.replayGainTrack != 0.0f) record.replayGainTrack = metadata.replayGainTrack;
    if (metadata.replayGainAlbum != 0.0f) record.replayGainAlbum = metadata.replayGainAlbum;
    if (metadata.track > 0) record.trackNumber = metadata.track;
    if (metadata.disc > 1) record.discNumber = metadata.disc;
    if (!metadata.bwDescription.empty()) record.description = metadata.bwDescription;
    else if (!metadata.description.empty()) record.description = metadata.description;
    else if (!metadata.comment.empty()) record.description = metadata.comment;
    if (!metadata.category.empty()) record.category = metadata.category;
    else if (!metadata.genre.empty()) record.category = metadata.genre;
    if (!metadata.genre.empty()) record.rawGenre = metadata.genre;
    if (!metadata.instruments.empty()) record.rawInstruments = metadata.instruments;
    TagVocabularyMapper::normalize(
        record.rawGenre, record.rawInstruments, record.normGenre,
        record.normInstruments, record.normMoodTags, record.vocalType);
    if (!metadata.keywords.empty()) record.keywords = metadata.keywords;
    else if (!metadata.comment.empty() && record.description != metadata.comment) {
        record.keywords = metadata.comment;
    }
    PopulateExtendedMeta(record.meta, metadata);
}

bool Database::probeAudioFileInfo(const std::string& filePath,
                                  uint32_t& sampleRate,
                                  uint32_t& channels,
                                  uint64_t& totalFrames,
                                  float& durationSeconds) const {
    return readAudioFileInfo(Utf8ToPath(filePath),
                             sampleRate,
                             channels,
                             totalFrames,
                             durationSeconds);
}

bool Database::readAudioRecordMetadataSnapshot(
    const std::string& filePath,
    AudioRecord& record) {
    if (filePath.empty()) return false;
    const auto fileInfo = Platform::FileSystem::ProbeRegularFile(filePath);
    if (!fileInfo.isRegularFile) return false;

    record = AudioRecord{};
    record.filePath = filePath;
    record.fileSize = fileInfo.sizeAvailable ? fileInfo.sizeBytes : 0;
    try {
        const fs::path path = Utf8ToPath(filePath);
        record.fileName = PathToUtf8(path.filename());
        record.displayName = record.fileName;
        record.extension = PathToUtf8(path.extension());
        std::transform(record.extension.begin(), record.extension.end(), record.extension.begin(), ::tolower);
        record.folderPath = HarkenSearch::CanonicalFolderPath(PathToUtf8(path.parent_path()));

        const bool hasAudioInfo = readAudioFileInfo(
            path, record.sampleRate, record.channels, record.totalFrames, record.durationSeconds);
        AudioMetadata metadata;
        const bool hasMetadata = MetadataReader::readMetadata(filePath, metadata);
        if (hasMetadata) ApplyAudioMetadataToRecord(record, metadata);
        return hasAudioInfo || hasMetadata;
    } catch (...) {
        return false;
    }
}

// Helper to safely get string from sqlite3_column_text
static std::string safeGetText(sqlite3_stmt* stmt, int col) {
    const unsigned char* text = sqlite3_column_text(stmt, col);
    return text ? reinterpret_cast<const char*>(text) : "";
}

static int clampRatingValue(int rating) {
    if (rating < 0) return 0;
    if (rating > 5) return 5;
    return rating;
}

static bool IsLargeLibraryCoverRegistryBoundedSparse(sqlite3* db,
                                                     int registryEntries,
                                                     int64_t totalFiles,
                                                     bool* outMarkerPending = nullptr);

static bool RearmCoverRegistryMaintenanceForEmptyLargeLibrary(sqlite3* db,
                                                              int registryEntries,
                                                              int64_t totalFiles);

Database::Database() {}

Database::~Database() {
    close();
}

bool Database::open(const std::string& dbPath, bool isPrimary) {
    if (m_db) close();
    if (dbPath.empty()) return false;
    if (!Platform::HarkenUserData::PreviewIsolationAllowsDataPathUtf8(dbPath)) {
        m_lastError = "Preview isolation rejected a database path outside the preview root";
        return false;
    }
    m_isPrimaryConnection = isPrimary;

    // Create the parent through the filesystem PAL and fail before SQLite if it cannot be made.
    const fs::path databasePath = Utf8ToPath(dbPath);
    const std::string parentPathUtf8 = PathToUtf8(databasePath.parent_path());
    if (Platform::FileSystem::IsNetworkPath(dbPath) ||
        (!parentPathUtf8.empty() && Platform::FileSystem::IsNetworkPath(parentPathUtf8))) {
        m_lastError = L(Db_RejectNetworkPath);
        return false;
    }
    if (!parentPathUtf8.empty() &&
        !Platform::FileSystem::EnsureDirectoryExists(parentPathUtf8)) {
        m_lastError = "Failed to create database parent directory";
        return false;
    }

    // Probe before opening so filesystem errors cannot be mistaken for a new file.
    std::string probeError;
    const Platform::FileSystem::PathStatus pathStatus =
        Platform::FileSystem::ProbePath(dbPath, &probeError);
    if (pathStatus == Platform::FileSystem::PathStatus::Error) {
        m_lastError = probeError.empty() ? "Failed to probe database path" : probeError;
        return false;
    }
    const bool isMissingFile =
        pathStatus == Platform::FileSystem::PathStatus::Missing;
    const bool isExistingFile =
        pathStatus == Platform::FileSystem::PathStatus::Present;
    if (!isMissingFile && !isExistingFile) {
        m_lastError = "Unknown database path status";
        return false;
    }
    const bool isNewFile = isMissingFile;

    int result;
#ifdef _WIN32
    fs::path path = Utf8ToPath(dbPath);
    std::wstring widePath = path.wstring();
    result = sqlite3_open16(widePath.c_str(), &m_db);
#else
    result = sqlite3_open(dbPath.c_str(), &m_db);
#endif

    // Remove a newly created database if initialization fails.
    auto cleanupOnFail = [&](const std::string& err) {
        m_lastError = err;
        if (m_db) { sqlite3_close(m_db); m_db = nullptr; }
        // Deleting only newly created files prevents the next attempt from reporting a false duplicate.
        if (isNewFile) {
            Platform::FileSystem::RemoveFile(dbPath);
            Platform::FileSystem::RemoveFile(getBlobsDbPath(dbPath));
            Platform::FileSystem::RemoveFile(getWaveformsDbPath(dbPath));
        }
        return false;
    };

    if (result != SQLITE_OK) {
        return cleanupOnFail(m_db ? sqlite3_errmsg(m_db) : "Failed to open database");
    }

    // Register search-normalization functions on this connection.
    RegisterHarkenSearchSqliteFunctions(m_db);

    // 并发安全：WAL 模式下多连接/多进程访问时，busy_timeout 避免立即 SQLITE_BUSY
    executeSQL("PRAGMA busy_timeout = 5000;");
    if (isNewFile && isPrimary) {
        executeSQL("PRAGMA page_size = 65536;");
    }

    // Performance-related PRAGMAs.
    executeSQL("PRAGMA journal_mode = WAL;");
    executeSQL("PRAGMA synchronous = NORMAL;");
    // A 512 MB mmap balances page reuse with a predictable working set.
    // Avoid 2 GB mappings, which inflate the reported working set without useful residency.
    executeSQL("PRAGMA mmap_size = 536870912;");
    executeSQL("PRAGMA cache_size = -64000;");
    executeSQL("PRAGMA temp_store = MEMORY;");
    executeSQL("PRAGMA read_uncommitted = true;");
    executeSQL("PRAGMA foreign_keys = ON;");

    // Attach the sidecars before main-schema migrations. Some one-time main
    // migrations touch blobs_db.cover_thumbs, so the schema must exist before
    // createTables() runs or startup will retry the migration forever.
    bool sidecarAttached = attachSidecarDatabases(dbPath);
    if (sidecarAttached) {
        executeSQL("PRAGMA blobs_db.busy_timeout = 5000;");
        executeSQL("PRAGMA waveforms_db.busy_timeout = 5000;");
        executeSQL("PRAGMA blobs_db.journal_mode = WAL;");
        executeSQL("PRAGMA waveforms_db.journal_mode = WAL;");
        executeSQL("PRAGMA blobs_db.synchronous = NORMAL;");
        executeSQL("PRAGMA waveforms_db.synchronous = NORMAL;");
        executeSQL(R"(
            CREATE TABLE IF NOT EXISTS blobs_db.cover_thumbs (
                cover_key   TEXT PRIMARY KEY,
                thumb_data  BLOB,
                mime        TEXT,
                created_at  INTEGER DEFAULT 0
            );
        )");
    }

    // 2. Create the schema; cleanup removes a new file if this step fails.
    if (!createTables()) {
        return cleanupOnFail("Create tables failed: " + m_lastError);
    }
    
    // 3. 挂载 Sidecar (即使失败也不强制回滚主库，但记录日志)
    if (sidecarAttached) {
        createSidecarTables();
    }
    
    migrateFromSingleDatabase(dbPath);

    {
        const char* migKey = "waveform_detail_purge_v2_engineSR";
        if (ReadAppMetaInt(m_db, migKey) != 1) {
            int purged = 0;
            if (executeSQL("UPDATE waveforms_db.waveforms SET waveform_detail = NULL, waveform_min_detail = NULL, detail_num_channels = 0 WHERE waveform_detail IS NOT NULL OR waveform_min_detail IS NOT NULL")) {
                sqlite3_stmt* cntStmt = nullptr;
                if (sqlite3_prepare_v2(m_db, "SELECT changes()", -1, &cntStmt, nullptr) == SQLITE_OK) {
                    if (sqlite3_step(cntStmt) == SQLITE_ROW) purged = sqlite3_column_int(cntStmt, 0);
                    sqlite3_finalize(cntStmt);
                }
            }
            WriteAppMetaInt(m_db, migKey, 1);
            DIAG_ACTION_FMT("Database", "PurgedStaleWaveformDetail: rows=%d (engineSR migration)", purged);
        }
    }

    // Purge legacy BLOB columns from audio_files to reclaim disk space.
    // Waveform and RMS BLOBs live in the waveforms sidecar.
    {
        const char* migKey = "legacy_blob_purge_v1";
        if (ReadAppMetaInt(m_db, migKey) != 1) {
            int purged = 0;
            const char* purgeSql =
                "UPDATE audio_files SET waveform_data = NULL, waveform_detail = NULL, rms_data = NULL "
                "WHERE waveform_data IS NOT NULL OR waveform_detail IS NOT NULL OR rms_data IS NOT NULL";
            if (executeSQL(purgeSql)) {
                sqlite3_stmt* cntStmt = nullptr;
                if (sqlite3_prepare_v2(m_db, "SELECT changes()", -1, &cntStmt, nullptr) == SQLITE_OK) {
                    if (sqlite3_step(cntStmt) == SQLITE_ROW) purged = sqlite3_column_int(cntStmt, 0);
                    sqlite3_finalize(cntStmt);
                }
                if (purged > 0) {
                    DIAG_ACTION_FMT("Database", "PurgedLegacyBlobs: rows=%d (waveform_data+waveform_detail+rms_data)", purged);
                    // VACUUM must run outside any transaction to reclaim disk space
                    executeSQL("VACUUM");
                    DIAG_ACTION_FMT("Database", "VacuumComplete: reclaimed space from %d purged BLOB rows", purged);
                }
            }
            WriteAppMetaInt(m_db, migKey, 1);
        }
    }

    // 子播放列表不变量自检:folder 不应持有 playlist_items
    {
        sqlite3_stmt* chk = nullptr;
        if (sqlite3_prepare_v2(m_db,
            "SELECT COUNT(*) FROM playlist_items pi "
            "JOIN playlists p ON pi.playlist_id = p.id WHERE p.is_folder = 1",
            -1, &chk, nullptr) == SQLITE_OK) {
            if (sqlite3_step(chk) == SQLITE_ROW) {
                int bad = sqlite3_column_int(chk, 0);
                if (bad > 0) {
                    DIAG_ACTION_FMT("Database",
                        "SchemaInconsistency_PlaylistFolderHasFiles: count=%d", bad);
                }
            }
            sqlite3_finalize(chk);
        }
    }

    // Durable backfill queue schema.
    // Foreign-key cascade removes pending jobs with their audio file.
    // The (enqueued_at, file_id, kind) index supports keyset recovery
    // without sorting or touching unrelated table rows.

    {
        const char* createTableSql =
            "CREATE TABLE IF NOT EXISTS backfill_pending("
            "    file_id     INTEGER NOT NULL REFERENCES audio_files(id) ON DELETE CASCADE,"
            "    kind        INTEGER NOT NULL,"
            "    enqueued_at INTEGER NOT NULL,"
            "    PRIMARY KEY(file_id, kind)"
            ")";
        if (!executeSQL(createTableSql)) {
            DIAG_ACTION_FMT("Database",
                "BackfillPendingSchemaCreateFailed: %s", m_lastError.c_str());
        }
        const char* createIndexSql =
            "CREATE INDEX IF NOT EXISTS idx_backfill_pending_enqueued_at "
            "ON backfill_pending(enqueued_at, file_id, kind)";
        if (!executeSQL(createIndexSql)) {
            DIAG_ACTION_FMT("Database",
                "BackfillPendingIndexCreateFailed: %s", m_lastError.c_str());
        }
    }

    if (!executeSQL(
            "CREATE TABLE IF NOT EXISTS scan_foreground_completion("
            "scan_session_id INTEGER NOT NULL,"
            "file_id INTEGER NOT NULL,"
            "mini_state INTEGER NOT NULL DEFAULT 0,"
            "cover_state INTEGER NOT NULL DEFAULT 0,"
            "metadata_state INTEGER NOT NULL DEFAULT 7,"
            "mini_reason TEXT NOT NULL DEFAULT '',"
            "cover_reason TEXT NOT NULL DEFAULT '',"
            "metadata_reason TEXT NOT NULL DEFAULT '',"
            "updated_at INTEGER NOT NULL,"
            "PRIMARY KEY(scan_session_id, file_id),"
            "FOREIGN KEY(file_id) REFERENCES audio_files(id) ON DELETE CASCADE"
            ")")) {
        return false;
    }
    ensureColumn(m_db, "scan_foreground_completion", "metadata_state", "INTEGER NOT NULL DEFAULT 7");
    ensureColumn(m_db, "scan_foreground_completion", "metadata_reason", "TEXT NOT NULL DEFAULT ''");
    if (!executeSQL(
            "CREATE INDEX IF NOT EXISTS idx_scan_foreground_completion_file "
            "ON scan_foreground_completion(file_id)")) {
        return false;
    }
    if (!executeSQL(
            "CREATE INDEX IF NOT EXISTS idx_scan_foreground_completion_state "
            "ON scan_foreground_completion(scan_session_id, mini_state, cover_state)")) {
        return false;
    }
    if (!executeSQL(
            "CREATE INDEX IF NOT EXISTS idx_scan_foreground_completion_metadata "
            "ON scan_foreground_completion(scan_session_id, metadata_state, mini_state, cover_state)")) {
        return false;
    }

    if (m_isPrimaryConnection &&
        sqlite3_db_readonly(m_db, "main") != 1) {
        const int64_t totalFiles = getFileCount();
        if (isCoverTileMembershipMaintenancePending()) {
            WriteAppMetaInt(m_db, "cover_tile_membership_needs_maintenance", 1);
            DIAG_ACTION("Database", "Cover tile membership maintenance armed during open for legacy or empty membership state.");
        }
        if (totalFiles > kGlobalCoverWallFallbackMaxFiles) {
            const int registryEntries = countCoverRegistryEntries();
            if (RearmCoverRegistryMaintenanceForEmptyLargeLibrary(m_db, registryEntries, totalFiles)) {
                DIAG_ACTION("Database", "Cover registry maintenance re-armed during open for empty large-library registry.");
            } else if (ReadAppMetaInt(m_db, "cover_registry_needs_maintenance") != 1 &&
                       isCoverRegistryMaintenancePending()) {
                WriteAppMetaInt(m_db, "cover_registry_needs_maintenance", 1);
                DIAG_ACTION("Database", "Cover registry maintenance re-armed during open for sparse legacy library.");
            }
        }
    }

    return true;
}

WaveformCachePolicy::Mode Database::getWaveformCacheMode() {
    if (!m_db) return WaveformCachePolicy::Mode::MiniAndMain;
    return WaveformCachePolicy::ModeFromStorageValue(
        ReadAppMetaString(m_db, kWaveformCacheModeMetaKey));
}

bool Database::setWaveformCacheMode(WaveformCachePolicy::Mode mode) {
    if (!m_db) return false;
    return WriteAppMetaString(
        m_db,
        kWaveformCacheModeMetaKey,
        WaveformCachePolicy::ModeStorageValue(mode));
}

// F-001 slice7-A helpers
// markBackfillPending: idempotent UPSERT. Refreshes enqueued_at on conflict
// so a re-scan promotes the row to "recent" for cursor ordering.
bool Database::markBackfillPending(int64_t fileId, int kind) {
    if (!m_db || fileId <= 0) return false;
    const char* sql =
        "INSERT INTO backfill_pending(file_id, kind, enqueued_at) "
        "VALUES (?, ?, ?) "
        "ON CONFLICT(file_id, kind) DO UPDATE SET enqueued_at = excluded.enqueued_at";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }
    sqlite3_bind_int64(stmt, 1, fileId);
    sqlite3_bind_int(stmt, 2, kind);
    sqlite3_bind_int64(stmt, 3, getCurrentTimestamp());
    const bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

bool Database::clearBackfillPending(int64_t fileId, int kind) {
    if (!m_db || fileId <= 0) return false;
    static const std::string sql =
        "DELETE FROM backfill_pending WHERE file_id = ? AND kind = ?";
    sqlite3_stmt* stmt = checkoutStmt(sql);
    if (!stmt) {
        return false;
    }
    sqlite3_bind_int64(stmt, 1, fileId);
    sqlite3_bind_int(stmt, 2, kind);
    const bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    returnStmt(sql, stmt);
    return ok;
}

bool Database::resetScanForegroundCompletionSession(int64_t scanSessionId) {
    if (!m_db || scanSessionId <= 0) return false;
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(
            m_db,
            "DELETE FROM scan_foreground_completion WHERE scan_session_id = ?",
            -1,
            &stmt,
            nullptr) != SQLITE_OK) {
        return false;
    }
    sqlite3_bind_int64(stmt, 1, scanSessionId);
    const bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

bool Database::seedScanForegroundCompletionRows(int64_t scanSessionId,
                                                const std::vector<int64_t>& fileIds,
                                                bool requireMini,
                                                bool requireCover,
                                                bool requireMetadata) {
    if (!m_db || scanSessionId <= 0) return false;
    if (fileIds.empty()) return true;

    const bool ownsTransaction = sqlite3_get_autocommit(m_db) != 0;
    if (ownsTransaction && !executeSQL("BEGIN IMMEDIATE TRANSACTION")) {
        return false;
    }

    const char* sql =
        "INSERT INTO scan_foreground_completion"
        "(scan_session_id, file_id, mini_state, cover_state, metadata_state, mini_reason, cover_reason, metadata_reason, updated_at) "
        "VALUES (?, ?, ?, ?, ?, '', '', '', ?) "
        "ON CONFLICT(scan_session_id, file_id) DO UPDATE SET "
        "mini_state = CASE "
        "WHEN scan_foreground_completion.mini_state != 0 AND scan_foreground_completion.mini_state != 7 "
        "THEN scan_foreground_completion.mini_state "
        "WHEN scan_foreground_completion.mini_state = 0 THEN 0 "
        "WHEN excluded.mini_state = 0 THEN 0 "
        "ELSE scan_foreground_completion.mini_state END, "
        "cover_state = CASE "
        "WHEN scan_foreground_completion.cover_state != 0 AND scan_foreground_completion.cover_state != 7 "
        "THEN scan_foreground_completion.cover_state "
        "WHEN scan_foreground_completion.cover_state = 0 THEN 0 "
        "WHEN excluded.cover_state = 0 THEN 0 "
        "ELSE scan_foreground_completion.cover_state END, "
        "metadata_state = CASE "
        "WHEN scan_foreground_completion.metadata_state != 0 AND scan_foreground_completion.metadata_state != 7 "
        "THEN scan_foreground_completion.metadata_state "
        "WHEN scan_foreground_completion.metadata_state = 0 THEN 0 "
        "WHEN excluded.metadata_state = 0 THEN 0 "
        "ELSE scan_foreground_completion.metadata_state END, "
        "mini_reason = CASE "
        "WHEN scan_foreground_completion.mini_state = 0 THEN excluded.mini_reason "
        "ELSE scan_foreground_completion.mini_reason END, "
        "cover_reason = CASE "
        "WHEN scan_foreground_completion.cover_state = 0 THEN excluded.cover_reason "
        "ELSE scan_foreground_completion.cover_reason END, "
        "metadata_reason = CASE "
        "WHEN scan_foreground_completion.metadata_state = 0 THEN excluded.metadata_reason "
        "ELSE scan_foreground_completion.metadata_reason END, "
        "updated_at = CASE "
        "WHEN scan_foreground_completion.mini_state = 0 OR "
        "scan_foreground_completion.cover_state = 0 OR "
        "scan_foreground_completion.metadata_state = 0 "
        "THEN excluded.updated_at ELSE scan_foreground_completion.updated_at END";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        if (ownsTransaction) executeSQL("ROLLBACK");
        return false;
    }

    const int miniState = static_cast<int>(
        requireMini ? ScanForegroundTerminalState::Pending
                    : ScanForegroundTerminalState::Skipped);
    const int coverState = static_cast<int>(
        requireCover ? ScanForegroundTerminalState::Pending
                     : ScanForegroundTerminalState::Skipped);
    const int metadataState = static_cast<int>(
        requireMetadata ? ScanForegroundTerminalState::Pending
                        : ScanForegroundTerminalState::Skipped);
    const int64_t now = getCurrentTimestamp();
    bool ok = true;
    for (int64_t fileId : fileIds) {
        if (fileId <= 0) continue;
        sqlite3_reset(stmt);
        sqlite3_clear_bindings(stmt);
        sqlite3_bind_int64(stmt, 1, scanSessionId);
        sqlite3_bind_int64(stmt, 2, fileId);
        sqlite3_bind_int(stmt, 3, miniState);
        sqlite3_bind_int(stmt, 4, coverState);
        sqlite3_bind_int(stmt, 5, metadataState);
        sqlite3_bind_int64(stmt, 6, now);
        if (sqlite3_step(stmt) != SQLITE_DONE) {
            ok = false;
            break;
        }
    }
    sqlite3_finalize(stmt);
    if (ownsTransaction) {
        if (ok) {
            ok = executeSQL("COMMIT");
            if (!ok) executeSQL("ROLLBACK");
        } else {
            executeSQL("ROLLBACK");
        }
    }
    return ok;
}

bool Database::markScanForegroundMiniTerminal(int64_t scanSessionId,
                                              int64_t fileId,
                                              ScanForegroundTerminalState state,
                                              const std::string& reason) {
    if (!m_db || scanSessionId <= 0 || fileId <= 0) return false;
    if (state == ScanForegroundTerminalState::Pending) return false;

    static const std::string sql =
        "UPDATE scan_foreground_completion SET mini_state = ?, mini_reason = ?, updated_at = ? "
        "WHERE scan_session_id = ? AND file_id = ? AND mini_state = 0";
    sqlite3_stmt* stmt = checkoutStmt(sql);
    if (!stmt) return false;
    sqlite3_bind_int(stmt, 1, static_cast<int>(state));
    sqlite3_bind_text(stmt, 2, reason.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 3, getCurrentTimestamp());
    sqlite3_bind_int64(stmt, 4, scanSessionId);
    sqlite3_bind_int64(stmt, 5, fileId);
    const bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    const int changed = ok ? sqlite3_changes(m_db) : 0;
    returnStmt(sql, stmt);
    if (!ok) return false;
    if (changed > 0) return true;

    static const std::string existsSql =
        "SELECT 1 FROM scan_foreground_completion "
        "WHERE scan_session_id = ? AND file_id = ? AND mini_state != 0 LIMIT 1";
    stmt = checkoutStmt(existsSql);
    if (!stmt) return false;
    sqlite3_bind_int64(stmt, 1, scanSessionId);
    sqlite3_bind_int64(stmt, 2, fileId);
    const bool alreadyTerminal = sqlite3_step(stmt) == SQLITE_ROW;
    returnStmt(existsSql, stmt);
    return alreadyTerminal;
}

bool Database::markScanForegroundCoverTerminal(int64_t scanSessionId,
                                               int64_t fileId,
                                               ScanForegroundTerminalState state,
                                               const std::string& reason) {
    if (!m_db || scanSessionId <= 0 || fileId <= 0) return false;
    if (state == ScanForegroundTerminalState::Pending) return false;

    static const std::string sql =
        "UPDATE scan_foreground_completion SET cover_state = ?, cover_reason = ?, updated_at = ? "
        "WHERE scan_session_id = ? AND file_id = ? AND cover_state = 0";
    sqlite3_stmt* stmt = checkoutStmt(sql);
    if (!stmt) return false;
    sqlite3_bind_int(stmt, 1, static_cast<int>(state));
    sqlite3_bind_text(stmt, 2, reason.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 3, getCurrentTimestamp());
    sqlite3_bind_int64(stmt, 4, scanSessionId);
    sqlite3_bind_int64(stmt, 5, fileId);
    const bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    const int changed = ok ? sqlite3_changes(m_db) : 0;
    returnStmt(sql, stmt);
    if (!ok) return false;
    if (changed > 0) return true;

    static const std::string existsSql =
        "SELECT 1 FROM scan_foreground_completion "
        "WHERE scan_session_id = ? AND file_id = ? AND cover_state != 0 LIMIT 1";
    stmt = checkoutStmt(existsSql);
    if (!stmt) return false;
    sqlite3_bind_int64(stmt, 1, scanSessionId);
    sqlite3_bind_int64(stmt, 2, fileId);
    const bool alreadyTerminal = sqlite3_step(stmt) == SQLITE_ROW;
    returnStmt(existsSql, stmt);
    return alreadyTerminal;
}

bool Database::markScanForegroundMetadataTerminal(int64_t scanSessionId,
                                                  int64_t fileId,
                                                  ScanForegroundTerminalState state,
                                                  const std::string& reason) {
    if (!m_db || scanSessionId <= 0 || fileId <= 0) return false;
    if (state == ScanForegroundTerminalState::Pending) return false;

    static const std::string sql =
        "UPDATE scan_foreground_completion SET metadata_state = ?, metadata_reason = ?, updated_at = ? "
        "WHERE scan_session_id = ? AND file_id = ? AND metadata_state = 0";
    sqlite3_stmt* stmt = checkoutStmt(sql);
    if (!stmt) return false;
    sqlite3_bind_int(stmt, 1, static_cast<int>(state));
    sqlite3_bind_text(stmt, 2, reason.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 3, getCurrentTimestamp());
    sqlite3_bind_int64(stmt, 4, scanSessionId);
    sqlite3_bind_int64(stmt, 5, fileId);
    const bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    const int changed = ok ? sqlite3_changes(m_db) : 0;
    returnStmt(sql, stmt);
    if (!ok) return false;
    if (changed > 0) return true;

    static const std::string existsSql =
        "SELECT 1 FROM scan_foreground_completion "
        "WHERE scan_session_id = ? AND file_id = ? AND metadata_state != 0 LIMIT 1";
    stmt = checkoutStmt(existsSql);
    if (!stmt) return false;
    sqlite3_bind_int64(stmt, 1, scanSessionId);
    sqlite3_bind_int64(stmt, 2, fileId);
    const bool alreadyTerminal = sqlite3_step(stmt) == SQLITE_ROW;
    returnStmt(existsSql, stmt);
    return alreadyTerminal;
}

ScanForegroundCompletionCounts Database::getScanForegroundCompletionCounts(int64_t scanSessionId) {
    ScanForegroundCompletionCounts counts;
    if (!m_db || scanSessionId <= 0) return counts;

    static const std::string sql =
        "SELECT "
        "COUNT(*), "
        "COALESCE(SUM(CASE WHEN mini_state = 0 OR cover_state = 0 OR metadata_state = 0 THEN 1 ELSE 0 END), 0), "
        "COALESCE(SUM(CASE WHEN mini_state != 0 THEN 1 ELSE 0 END), 0), "
        "COALESCE(SUM(CASE WHEN cover_state != 0 THEN 1 ELSE 0 END), 0), "
        "COALESCE(SUM(CASE WHEN metadata_state != 0 THEN 1 ELSE 0 END), 0), "
        "COALESCE(SUM(CASE WHEN mini_state = 0 THEN 1 ELSE 0 END), 0), "
        "COALESCE(SUM(CASE WHEN cover_state = 0 THEN 1 ELSE 0 END), 0), "
        "COALESCE(SUM(CASE WHEN metadata_state = 0 THEN 1 ELSE 0 END), 0), "
        "COALESCE(SUM(CASE WHEN mini_state >= 3 THEN 1 ELSE 0 END), 0) + "
        "COALESCE(SUM(CASE WHEN cover_state >= 3 THEN 1 ELSE 0 END), 0) + "
        "COALESCE(SUM(CASE WHEN metadata_state >= 3 THEN 1 ELSE 0 END), 0) "
        "FROM scan_foreground_completion WHERE scan_session_id = ?";
    sqlite3_stmt* stmt = checkoutStmt(sql);
    if (!stmt) return counts;
    sqlite3_bind_int64(stmt, 1, scanSessionId);

    if (sqlite3_step(stmt) == SQLITE_ROW) {
        counts.total = sqlite3_column_int(stmt, 0);
        counts.pendingFiles = sqlite3_column_int(stmt, 1);
        counts.miniTerminal = sqlite3_column_int(stmt, 2);
        counts.coverTerminal = sqlite3_column_int(stmt, 3);
        counts.metadataTerminal = sqlite3_column_int(stmt, 4);
        counts.miniUnresolved = sqlite3_column_int(stmt, 5);
        counts.coverUnresolved = sqlite3_column_int(stmt, 6);
        counts.metadataUnresolved = sqlite3_column_int(stmt, 7);
        counts.failureTerminal = sqlite3_column_int(stmt, 8);
    }
    returnStmt(sql, stmt);
    return counts;
}

int Database::countBackfillPending() {
    if (!m_db) return 0;
    static const std::string sql = "SELECT COUNT(*) FROM backfill_pending";
    sqlite3_stmt* stmt = checkoutStmt(sql);
    if (!stmt) return 0;
    int count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) count = sqlite3_column_int(stmt, 0);
    returnStmt(sql, stmt);
    return count;
}

int Database::countBackfillPendingKind(int kind) {
    if (!m_db) return 0;
    static const std::string sql = "SELECT COUNT(*) FROM backfill_pending WHERE kind = ?";
    sqlite3_stmt* stmt = checkoutStmt(sql);
    if (!stmt) return 0;
    sqlite3_bind_int(stmt, 1, kind);
    int count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) count = sqlite3_column_int(stmt, 0);
    returnStmt(sql, stmt);
    return count;
}

bool Database::hasBackfillPending(int64_t fileId, int kind) {
    if (!m_db || fileId <= 0 || kind < 0) return false;
    static const std::string sql =
        "SELECT 1 FROM backfill_pending WHERE file_id = ? AND kind = ? LIMIT 1";
    sqlite3_stmt* stmt = checkoutStmt(sql);
    if (!stmt) return false;
    sqlite3_bind_int64(stmt, 1, fileId);
    sqlite3_bind_int(stmt, 2, kind);
    const bool present = sqlite3_step(stmt) == SQLITE_ROW;
    returnStmt(sql, stmt);
    return present;
}

int Database::countBackfillPendingKindInFolder(int kind, const std::string& folderPathPrefix) {
    if (!m_db || folderPathPrefix.empty()) return 0;

    const std::string scopePath = HarkenSearch::CanonicalFolderPath(folderPathPrefix);
    const std::string upper = MakeFolderPathUpperBound(scopePath);
    static const std::string sql = R"SQL(
        SELECT COUNT(*)
        FROM backfill_pending bp
        INNER JOIN audio_files af ON af.id = bp.file_id
        WHERE bp.kind = ?
          AND af.folder_path >= ?
          AND af.folder_path < ?
    )SQL";
    sqlite3_stmt* stmt = checkoutStmt(sql);
    if (!stmt) return 0;
    sqlite3_bind_int(stmt, 1, kind);
    sqlite3_bind_text(stmt, 2, scopePath.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, upper.c_str(), -1, SQLITE_TRANSIENT);
    int count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) count = sqlite3_column_int(stmt, 0);
    returnStmt(sql, stmt);
    return count;
}

std::vector<BackfillPendingRow> Database::getBackfillPendingRows(
    int limit, int64_t afterEnqueuedAt, int64_t afterFileId, int afterKind) {
    std::vector<BackfillPendingRow> rows;
    if (!m_db || limit <= 0) return rows;

    static const std::string sql = R"SQL(
        SELECT bp.file_id, bp.kind, bp.enqueued_at,
               COALESCE((
                   SELECT s.scan_session_id
                   FROM scan_foreground_completion s
                   WHERE s.file_id = bp.file_id
                     AND (
                         (bp.kind = 0 AND s.cover_state = 0)
                         OR (bp.kind = 1 AND s.mini_state = 0)
                         OR (bp.kind = 2 AND s.metadata_state = 0)
                     )
                   ORDER BY s.scan_session_id DESC
                   LIMIT 1
               ), 0) AS scan_session_id
        FROM backfill_pending bp
        WHERE (?1 <= 0)
           OR enqueued_at > ?1
           OR (enqueued_at = ?1 AND file_id > ?2)
           OR (enqueued_at = ?1 AND file_id = ?2 AND kind > ?3)
        ORDER BY enqueued_at, file_id, kind
        LIMIT ?4
    )SQL";
    sqlite3_stmt* stmt = checkoutStmt(sql);
    if (!stmt) return rows;
    sqlite3_bind_int64(stmt, 1, afterEnqueuedAt);
    sqlite3_bind_int64(stmt, 2, afterFileId);
    sqlite3_bind_int(stmt, 3, afterKind < 0 ? 0 : afterKind);
    sqlite3_bind_int(stmt, 4, limit);

    rows.reserve((size_t)limit);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        BackfillPendingRow row;
        row.fileId = sqlite3_column_int64(stmt, 0);
        row.kind = sqlite3_column_int(stmt, 1);
        row.enqueuedAt = sqlite3_column_int64(stmt, 2);
        row.scanSessionId = sqlite3_column_int64(stmt, 3);
        rows.push_back(row);
    }
    returnStmt(sql, stmt);
    return rows;
}

std::vector<BackfillPendingRow> Database::getBackfillPendingRowsInFolder(
    const std::string& folderPathPrefix,
    int limit,
    int64_t afterEnqueuedAt,
    int64_t afterFileId,
    int afterKind) {
    std::vector<BackfillPendingRow> rows;
    if (!m_db || limit <= 0 || folderPathPrefix.empty()) return rows;

    const std::string scopePath = HarkenSearch::CanonicalFolderPath(folderPathPrefix);
    const std::string upper = MakeFolderPathUpperBound(scopePath);
    static const std::string sql = R"SQL(
        SELECT bp.file_id, bp.kind, bp.enqueued_at,
               COALESCE((
                   SELECT s.scan_session_id
                   FROM scan_foreground_completion s
                   WHERE s.file_id = bp.file_id
                     AND (
                         (bp.kind = 0 AND s.cover_state = 0)
                         OR (bp.kind = 1 AND s.mini_state = 0)
                         OR (bp.kind = 2 AND s.metadata_state = 0)
                     )
                   ORDER BY s.scan_session_id DESC
                   LIMIT 1
               ), 0) AS scan_session_id
        FROM backfill_pending bp
        INNER JOIN audio_files af ON af.id = bp.file_id
        WHERE af.folder_path >= ?1
          AND af.folder_path < ?2
          AND (
                ?3 <= 0
             OR bp.enqueued_at > ?3
             OR (bp.enqueued_at = ?3 AND bp.file_id > ?4)
             OR (bp.enqueued_at = ?3 AND bp.file_id = ?4 AND bp.kind > ?5)
          )
        ORDER BY bp.enqueued_at, bp.file_id, bp.kind
        LIMIT ?6
    )SQL";
    sqlite3_stmt* stmt = checkoutStmt(sql);
    if (!stmt) return rows;
    sqlite3_bind_text(stmt, 1, scopePath.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, upper.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 3, afterEnqueuedAt);
    sqlite3_bind_int64(stmt, 4, afterFileId);
    sqlite3_bind_int(stmt, 5, afterKind < 0 ? 0 : afterKind);
    sqlite3_bind_int(stmt, 6, limit);

    rows.reserve((size_t)limit);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        BackfillPendingRow row;
        row.fileId = sqlite3_column_int64(stmt, 0);
        row.kind = sqlite3_column_int(stmt, 1);
        row.enqueuedAt = sqlite3_column_int64(stmt, 2);
        row.scanSessionId = sqlite3_column_int64(stmt, 3);
        rows.push_back(row);
    }
    returnStmt(sql, stmt);
    return rows;
}

// Warm database state during the loading screen before UI work begins.

void Database::preheat() {
    if (!m_db) return;
    DIAG_SCOPED("Database.preheat");

    // Keep startup warmup bounded. Full ANALYZE on million-row customer
    // libraries can take tens of seconds; PRAGMA optimize refreshes stats
    // opportunistically with SQLite's own throttling.
    executeSQL("PRAGMA analysis_limit=1000;");
    executeSQL("PRAGMA optimize;");
    DIAG_ACTION("Database", "PreheatPragmaOptimize");

    // Read the FTS5 shadow index once to load its B-tree pages into mmap and the page cache.
    {
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(m_db,
                "SELECT COUNT(*) FROM audio_files_fts WHERE audio_files_fts MATCH 'preheat_token_unlikely_abcxyz'",
                -1, &st, nullptr) == SQLITE_OK) {
            sqlite3_step(st);
            sqlite3_finalize(st);
        }
    }

    // Prepare common SQL templates so the first search does not pay parse cost.
    // Return each prepared statement immediately so later checkout is a cache hit.
    const char* hotSqlTemplates[] = {
        // Total row count used by startup and status UI.
        "SELECT COUNT(*) FROM audio_files",
        // First unfiltered page.
        "SELECT id FROM audio_files af ORDER BY af.file_name ASC LIMIT 200 OFFSET 0",
        // Folder count.
        "SELECT SUM(file_count) FROM folder_index WHERE folder_path >= ? AND folder_path < ?",
        // 按路径取记录（点击文件）
        "SELECT id, file_path, file_name, extension FROM audio_files WHERE file_path = ? LIMIT 1",
        // Cover registry.
        "SELECT cover_key, rep_path, is_embedded FROM cover_cache_registry LIMIT 1",
    };
    for (const char* sql : hotSqlTemplates) {
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) == SQLITE_OK && stmt) {
            returnStmt(sql, stmt);
        }
    }

    // Clear legacy detail-waveform cache after decoder output changed from native to engine sample rate.
    // Old frame counts no longer align; playback rebuilds them on demand.
    {
        const char* migKey = "waveform_detail_purge_v2_engineSR";
        if (ReadAppMetaInt(m_db, migKey) != 1) {
            int purged = 0;
            if (executeSQL("UPDATE waveforms_db.waveforms SET waveform_detail = NULL, waveform_min_detail = NULL, detail_num_channels = 0 WHERE waveform_detail IS NOT NULL OR waveform_min_detail IS NOT NULL")) {
                sqlite3_stmt* cntStmt = nullptr;
                if (sqlite3_prepare_v2(m_db, "SELECT changes()", -1, &cntStmt, nullptr) == SQLITE_OK) {
                    if (sqlite3_step(cntStmt) == SQLITE_ROW) purged = sqlite3_column_int(cntStmt, 0);
                    sqlite3_finalize(cntStmt);
                }
            }
            WriteAppMetaInt(m_db, migKey, 1);
            DIAG_ACTION_FMT("Database", "PurgedStaleWaveformDetail: rows=%d (engineSR migration)", purged);
        }
    }

    // Prime getFileCount's two-second cache for the first frame.
    getFileCount();

    // 5) Cover maintenance stays off the startup critical path. New scans/imports
    // finalize their own cover registry deltas. Legacy large libraries with a
    // sparse registry may start a bounded background repair worker so reopen can
    // progressively fill missing registry rows without a foreground full rebuild.
    {
        BackgroundIoContext maintenanceCtx;
        maintenanceCtx.mouseIdle = true;
        maintenanceCtx.secondsSinceSelectionOrLoad = 999.0;
        const bool explicitCoverMaintenance =
            ReadEnvIntClamped("AURA_ENABLE_COVER_PHASH_BACKFILL", 0, 0, 1) == 1;
        const bool primaryWritableConnection =
            m_isPrimaryConnection && sqlite3_db_readonly(m_db, "main") != 1;
        const int64_t totalFiles = getFileCount();
        const bool largeLibrary = totalFiles > kGlobalCoverWallFallbackMaxFiles;
        const int registryEntries =
            (primaryWritableConnection && largeLibrary) ? countCoverRegistryEntries() : 0;
        if (primaryWritableConnection && largeLibrary &&
            RearmCoverRegistryMaintenanceForEmptyLargeLibrary(m_db, registryEntries, totalFiles)) {
            DIAG_ACTION("Database", "Cover registry maintenance re-armed for empty large-library registry.");
        }
        bool maintenanceMarkerPending =
            primaryWritableConnection && largeLibrary &&
            ReadAppMetaInt(m_db, "cover_registry_needs_maintenance") == 1;
        bool exactMaintenancePending = false;
        bool exactMaintenanceChecked = false;
        if (primaryWritableConnection && largeLibrary && registryEntries > 0 && !maintenanceMarkerPending) {
            exactMaintenancePending = isCoverRegistryMaintenancePending();
            exactMaintenanceChecked = true;
            if (exactMaintenancePending) {
                WriteAppMetaInt(m_db, "cover_registry_needs_maintenance", 1);
                maintenanceMarkerPending = true;
                DIAG_ACTION("Database", "Cover registry maintenance re-armed for sparse legacy library.");
            }
        }
        bool needsAutomaticLegacyRegistryRepair = false;
        if (primaryWritableConnection && largeLibrary) {
            if (registryEntries == 0) {
                needsAutomaticLegacyRegistryRepair = maintenanceMarkerPending;
            } else if (maintenanceMarkerPending) {
                if (!exactMaintenanceChecked) {
                    exactMaintenancePending = isCoverRegistryMaintenancePending();
                    exactMaintenanceChecked = true;
                }
                needsAutomaticLegacyRegistryRepair = exactMaintenancePending;
            }
        }
        const bool searchDataStillPending =
            primaryWritableConnection &&
            ReadAppMetaInt(m_db, "search_data_migrated") != 2;
        const auto decision =
            ResolveAutomaticCoverMaintenanceDecision(
                maintenanceCtx,
                explicitCoverMaintenance || needsAutomaticLegacyRegistryRepair || searchDataStillPending);
        if (decision.allowed) {
            startCoverPhashBackfillWorker();
        } else {
            DIAG_SYSTEM_FMT("Database",
                            "CoverPhashBackfillWorker skipped: reason=%s autoLegacyRepair=%d",
                            decision.reason,
                            needsAutomaticLegacyRegistryRepair ? 1 : 0);
        }
    }

    DIAG_SYSTEM("Database", "Preheat complete");
}

void Database::warmupCoverIndexes() {
    if (!m_db) return;
    // Warm the cover-key B-tree pages used by the FTS cover subquery.
    // COUNT(DISTINCT) traverses intermediate pages as well as the first leaf.
    // This runs during loading; a million-row library typically costs 10-30 ms.

    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(m_db,
            "SELECT COUNT(DISTINCT folder_cover_key) FROM audio_files "
            "WHERE folder_cover_key > ''",
            -1, &st, nullptr) == SQLITE_OK) {
        sqlite3_step(st);
        sqlite3_finalize(st);
    }
    if (sqlite3_prepare_v2(m_db,
            "SELECT COUNT(DISTINCT embedded_cover_key) FROM audio_files "
            "WHERE embedded_cover_key > ''",
            -1, &st, nullptr) == SQLITE_OK) {
        sqlite3_step(st);
        sqlite3_finalize(st);
    }
}

bool Database::openReadOnly(const std::string& dbPath) {
    if (m_db) close();
   if (dbPath.empty() ||
       !Platform::HarkenUserData::PreviewIsolationAllowsDataPathUtf8(dbPath)) {
       m_lastError = "Preview isolation rejected a database path outside the preview root";
       return false;
   }
    if (Platform::FileSystem::IsNetworkPath(dbPath)) {
        m_lastError = L(Db_RejectNetworkPath);
        return false;
    }

   // sqlite3_open_v2 accepts UTF-8 paths on Windows and Linux.
    // SQLITE_OPEN_READONLY: 禁止写操作，避免 WAL write-lock 争用
    // SQLITE_OPEN_FULLMUTEX is safe even when this connection is used from one thread.
    int result = sqlite3_open_v2(
        dbPath.c_str(), &m_db,
        SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX,
        nullptr);

    if (result != SQLITE_OK) {
        m_lastError = m_db ? sqlite3_errmsg(m_db) : "Failed to open read-only database";
        if (m_db) { sqlite3_close(m_db); m_db = nullptr; }
        return false;
    }

    // Register search functions on the read-only connection as well.
    RegisterHarkenSearchSqliteFunctions(m_db);

    // Read-only connections still need busy_timeout while writers hold WAL locks.
    executeSQL("PRAGMA busy_timeout = 5000;");

    // Apply the same read-only performance PRAGMAs as the primary connection.
    executeSQL("PRAGMA mmap_size = 536870912;");
    executeSQL("PRAGMA cache_size = -64000;");
    executeSQL("PRAGMA temp_store = MEMORY;");
    executeSQL("PRAGMA read_uncommitted = true;");
    // Do not change journal_mode or synchronous on a read-only connection.

    // A bad main database is not a rebuildable cache. Check its schema before
    // deciding whether a subsequent ATTACH failure may be downgraded.
    sqlite3_stmt* mainSchema = nullptr;
    int schemaResult = sqlite3_prepare_v2(m_db,
        "SELECT name FROM main.sqlite_master LIMIT 1;", -1, &mainSchema, nullptr);
    if (schemaResult == SQLITE_OK) schemaResult = sqlite3_step(mainSchema);
    if (schemaResult != SQLITE_ROW && schemaResult != SQLITE_DONE) {
        m_lastError = sqlite3_errmsg(m_db);
        if (mainSchema) sqlite3_finalize(mainSchema);
        close();
        return false;
    }
    sqlite3_finalize(mainSchema);

    // Read-only connections must not create missing sidecars beside the source DB.
    m_mainDbPath = dbPath;
    m_blobsDbPath = getBlobsDbPath(dbPath);
    m_waveformsDbPath = getWaveformsDbPath(dbPath);
    if (!attachSidecarDatabases(dbPath, true)) {
        close();
        return false;
    }

    return true;
}

void Database::setQueryProgressHandler(int interval, int(*callback)(void*), void* ctx) {
    if (m_db) sqlite3_progress_handler(m_db, interval, callback, ctx);
}

void Database::clearQueryProgressHandler() {
    if (m_db) sqlite3_progress_handler(m_db, 0, nullptr, nullptr);
}

void Database::close() {
    m_postProcessCancel.store(true, std::memory_order_release);
    m_coverPhashBackfillCancel.store(true, std::memory_order_release);
    if (m_db) {
        sqlite3_interrupt(m_db);
    }
    if (m_postProcessThread.joinable()) {
        m_postProcessThread.join();
    }
    m_postProcessRunning.store(false, std::memory_order_release);
    if (m_coverPhashBackfillThread.joinable()) {
        m_coverPhashBackfillThread.join();
    }
    m_coverPhashBackfillRunning.store(false, std::memory_order_release);
    // T-296: stop the watcher rescan worker before nulling m_db, otherwise
    // a queued scanDirectoryEx() can dereference a dangling sqlite handle.
    // Independent of stopFileSystemWatcher() because reconciliation can be
    // dispatched without the FS watcher ever being started.
    stopWatcherRescanWorker();
    // T-297: same reasoning for the waveform precompute worker.
    stopPrecomputeWorker();
    std::lock_guard<std::mutex> writerLock(m_writerSerializeMutex);
    if (m_searchRecomputeDb) {
        DiscardSearchDataRecomputeScratch(m_searchRecomputeDb);
        sqlite3_close_v2(m_searchRecomputeDb);
        m_searchRecomputeDb = nullptr;
    }
    clearStmtPool();
    if (m_db) {
        // sqlite3_close_v2 safely defers cleanup for any stmts that outlive the pool flush
        // (e.g. stmts prepared against ATTACH-ed sidecar databases during open()).
        // sqlite3_close would crash or return SQLITE_BUSY in that case.
        sqlite3_close_v2(m_db);
        m_db = nullptr;
    }
    m_fastScanModeActive = false;
    clearPreparedCoverFilterKeys();
    // These caches belong to the closed library. A newly opened library may
    // have the same revision, so the two-second cache must not survive close.
    m_countMemCache.clear();
    m_cachedFileCount = -1;
    m_cachedFileCountRevision = 0;
    m_cachedFileCountTime = {};
    m_isPrimaryConnection = false;
}

bool Database::createTables() {
    // A database with no audio_files table yet is brand new. Only such fresh
    // databases adopt adaptive mini-waveform density; existing libraries keep the
    // fixed 128/160 budget so their cached waveform blobs stay valid. ("仅新建库")
    const bool freshDatabase = !tableExists(m_db, "audio_files");

    // Audio files table
    const char* createFilesTable = R"(
        CREATE TABLE IF NOT EXISTS audio_files (
            id              INTEGER PRIMARY KEY AUTOINCREMENT,
            file_path       TEXT UNIQUE NOT NULL,
            file_name       TEXT NOT NULL,
            extension       TEXT,
            file_size       INTEGER DEFAULT 0,

            sample_rate     INTEGER DEFAULT 0,
            bit_depth       INTEGER DEFAULT 0,
            channels        INTEGER DEFAULT 0,
            total_frames    INTEGER DEFAULT 0,
            duration        REAL DEFAULT 0,
            format          TEXT,

            title           TEXT,
            artist          TEXT,
            album           TEXT,
            description     TEXT,
            category        TEXT,
            keywords        TEXT,
            rating          INTEGER DEFAULT 0,

            play_count      INTEGER DEFAULT 0,
            date_added      INTEGER,
            last_played     INTEGER,
            
            waveform_data   BLOB
        );
    )";
    
    // Playlists table
    const char* createPlaylistsTable = R"(
        CREATE TABLE IF NOT EXISTS playlists (
            id              INTEGER PRIMARY KEY AUTOINCREMENT,
            name            TEXT NOT NULL,
            description     TEXT,
            date_created    INTEGER
        );
    )";

    // Playlist items table
    const char* createPlaylistItemsTable = R"(
        CREATE TABLE IF NOT EXISTS playlist_items (
            playlist_id     INTEGER NOT NULL,
            file_id         INTEGER NOT NULL,
            position        INTEGER DEFAULT 0,
            PRIMARY KEY (playlist_id, file_id),
            FOREIGN KEY (playlist_id) REFERENCES playlists(id) ON DELETE CASCADE,
            FOREIGN KEY (file_id) REFERENCES audio_files(id) ON DELETE CASCADE
        );
    )";

    if (!executeSQL(createFilesTable)) return false;
    if (!executeSQL(createPlaylistsTable)) return false;
    if (!executeSQL(createPlaylistItemsTable)) return false;

    // Tag tables
    const char* createTagsTable = R"(
        CREATE TABLE IF NOT EXISTS tags (
            id      INTEGER PRIMARY KEY AUTOINCREMENT,
            name    TEXT UNIQUE NOT NULL
        );
    )";
    const char* createFileTagsTable = R"(
        CREATE TABLE IF NOT EXISTS file_tags (
            file_id INTEGER NOT NULL,
            tag_id  INTEGER NOT NULL,
            PRIMARY KEY (file_id, tag_id),
            FOREIGN KEY (file_id) REFERENCES audio_files(id) ON DELETE CASCADE,
            FOREIGN KEY (tag_id)  REFERENCES tags(id) ON DELETE CASCADE
        );
    )";
    const char* createTagIndexes = R"(
        CREATE INDEX IF NOT EXISTS idx_file_tags_tag ON file_tags(tag_id);
        CREATE INDEX IF NOT EXISTS idx_tags_name ON tags(name);
    )";
    if (!executeSQL(createTagsTable)) return false;
    if (!executeSQL(createFileTagsTable)) return false;
    if (!executeSQL(createTagIndexes)) return false;

    // App metadata is used by one-shot migrations below. New empty libraries
    // must have it before any ReadAppMetaInt/WriteAppMetaInt gate runs.
    const char* createAppMetaTable = R"(
        CREATE TABLE IF NOT EXISTS app_meta (
            key     TEXT PRIMARY KEY,
            value   TEXT NOT NULL
        );
    )";
    if (!executeSQL(createAppMetaTable)) return false;

    const char* createNetworkLibraryRootsTable = R"(
        CREATE TABLE IF NOT EXISTS network_library_roots (
            path TEXT PRIMARY KEY
        );
    )";
    if (!executeSQL(createNetworkLibraryRootsTable)) return false;

    m_fastScanDirtyOnOpen = false;
    if (m_isPrimaryConnection) {
        const std::string fastScanFlag = ReadAppMetaString(m_db, "scan_fast_mode_active");
        if (fastScanFlag == "1") {
            m_fastScanDirtyOnOpen = true;
            executeSQL("PRAGMA main.synchronous = NORMAL;");
            executeSQL("PRAGMA main.journal_mode = WAL;");
            executeSQL("PRAGMA main.locking_mode = NORMAL;");
            WriteAppMetaString(m_db, "scan_fast_mode_active", "0");
            DIAG_ACTION_FMT("Database", "Recovered interrupted fast scan mode for %s",
                            m_mainDbPath.empty() ? "<db>" : m_mainDbPath.c_str());
        }
    }

    // Stamp the adaptive mini-waveform format only on new databases.
    // Databases without the key retain the fixed-density default.
    if (freshDatabase) {
        WriteAppMetaString(m_db, "mini_waveform_density", "adaptive");
        WriteAppMetaString(m_db, kMiniWaveformFormatV2MetaKey, "1");
    }
    m_usesAdaptiveMiniWaveform =
        (ReadAppMetaString(m_db, "mini_waveform_density") == "adaptive");
    m_needsAdaptiveMiniWaveformFormatUpgrade =
        m_usesAdaptiveMiniWaveform &&
        ReadAppMetaString(m_db, kMiniWaveformFormatV2MetaKey) != "1";

    const char* createQueryViewSnapshotsTable = R"(
        CREATE TABLE IF NOT EXISTS query_view_snapshots (
            snapshot_key           TEXT PRIMARY KEY,
            search_text            TEXT NOT NULL DEFAULT '',
            folder_path            TEXT NOT NULL DEFAULT '',
            cover_key              TEXT NOT NULL DEFAULT '',
            view_mode              INTEGER NOT NULL DEFAULT 0,
            sort_column            INTEGER NOT NULL DEFAULT 0,
            sort_ascending         INTEGER NOT NULL DEFAULT 1,
            selected_file_id       INTEGER NOT NULL DEFAULT 0,
            scroll_anchor_file_id  INTEGER NOT NULL DEFAULT 0,
            result_count           INTEGER NOT NULL DEFAULT 0,
            data_revision          INTEGER NOT NULL DEFAULT 0,
            payload_json           TEXT NOT NULL DEFAULT '',
            updated_at             INTEGER NOT NULL DEFAULT 0
        );
        CREATE INDEX IF NOT EXISTS idx_query_view_snapshots_updated_at
            ON query_view_snapshots(updated_at DESC, snapshot_key ASC);
    )";
    if (!executeSQL(createQueryViewSnapshotsTable)) return false;

    const char* createActivityLogTable = R"(
        CREATE TABLE IF NOT EXISTS activity_log (
            id              INTEGER PRIMARY KEY AUTOINCREMENT,
            activity_type   TEXT NOT NULL,
            file_id         INTEGER NOT NULL DEFAULT 0,
            file_path       TEXT NOT NULL DEFAULT '',
            target_path     TEXT NOT NULL DEFAULT '',
            detail_json     TEXT NOT NULL DEFAULT '',
            created_at      INTEGER NOT NULL DEFAULT 0
        );
        CREATE INDEX IF NOT EXISTS idx_activity_log_created_at
            ON activity_log(created_at DESC, id DESC);
        CREATE INDEX IF NOT EXISTS idx_activity_log_type_created_at
            ON activity_log(activity_type, created_at DESC, id DESC);
    )";
    if (!executeSQL(createActivityLogTable)) return false;

    // Schema upgrades (non-destructive, idempotent)
    // Use ensureColumn to avoid noisy "duplicate column name" errors that can pollute m_lastError.
    ensureColumn(m_db, "audio_files", "waveform_data", "BLOB");
    ensureColumn(m_db, "audio_files", "waveform_detail", "BLOB");
    ensureColumn(m_db, "audio_files", "rms_data", "BLOB");
    ensureColumn(m_db, "audio_files", "integrated_lufs", "REAL");
    ensureColumn(m_db, "audio_files", "momentary_lufs", "REAL");
    ensureColumn(m_db, "audio_files", "short_term_lufs", "REAL");
    ensureColumn(m_db, "audio_files", "true_peak_dbtp", "REAL");

    ensureColumn(m_db, "audio_files", "folder_path", "TEXT");
    ensureColumn(m_db, "audio_files", "folder_id", "INTEGER DEFAULT 0");
    ensureColumn(m_db, "audio_files", "file_mtime", "INTEGER DEFAULT 0");
    ensureColumn(m_db, "audio_files", "last_indexed", "INTEGER DEFAULT 0");
    ensureColumn(m_db, "audio_files", "has_cover", "INTEGER DEFAULT 0");
    ensureColumn(m_db, "audio_files", "bit_depth", "INTEGER DEFAULT 0");
    ensureColumn(m_db, "audio_files", "rating", "INTEGER DEFAULT 0");

    // Retired Favorites: drop index then column. PRAGMA first so a second
    // startup is a no-op. A failed migration must not be reported as success.
    if (columnExists(m_db, "audio_files", "is_favorite")) {
        if (!executeSQL("DROP INDEX IF EXISTS idx_files_favorite;")) return false;
        if (!executeSQL("ALTER TABLE audio_files DROP COLUMN is_favorite;")) return false;
    }

    // CoverKey cache (persistent)
    ensureColumn(m_db, "audio_files", "embedded_cover_key", "TEXT");
    ensureColumn(m_db, "audio_files", "embedded_cover_key_mtime", "INTEGER DEFAULT 0");
    ensureColumn(m_db, "audio_files", "folder_cover_key", "TEXT");
    ensureColumn(m_db, "audio_files", "folder_cover_src_path", "TEXT");
    ensureColumn(m_db, "audio_files", "folder_cover_src_mtime", "INTEGER DEFAULT 0");
    ensureColumn(m_db, "audio_files", "cover_suppressed", "INTEGER DEFAULT 0");

    // F-006 editable metadata columns.
    // Basic / Common
    ensureColumn(m_db, "audio_files", "subcategory", "TEXT");
    ensureColumn(m_db, "audio_files", "source", "TEXT");
    ensureColumn(m_db, "audio_files", "cat_id", "TEXT");
    ensureColumn(m_db, "audio_files", "category_full", "TEXT");
    ensureColumn(m_db, "audio_files", "fx_name", "TEXT");
    ensureColumn(m_db, "audio_files", "notes", "TEXT");
    ensureColumn(m_db, "audio_files", "user_comments", "TEXT");
    ensureColumn(m_db, "audio_files", "url", "TEXT");
    ensureColumn(m_db, "audio_files", "user_category", "TEXT");
    ensureColumn(m_db, "audio_files", "vendor_category", "TEXT");
    ensureColumn(m_db, "audio_files", "open_tier", "TEXT");
    ensureColumn(m_db, "audio_files", "genre", "TEXT");
    ensureColumn(m_db, "audio_files", "comment", "TEXT");
    ensureColumn(m_db, "audio_files", "copyright", "TEXT");
    ensureColumn(m_db, "audio_files", "software", "TEXT");
    // Library / Album
    ensureColumn(m_db, "audio_files", "library", "TEXT");
    ensureColumn(m_db, "audio_files", "volume", "TEXT");
    ensureColumn(m_db, "audio_files", "cd_title", "TEXT");
    ensureColumn(m_db, "audio_files", "cd_description", "TEXT");
    ensureColumn(m_db, "audio_files", "manufacturer", "TEXT");
    // Music
    ensureColumn(m_db, "audio_files", "composer", "TEXT");
    ensureColumn(m_db, "audio_files", "arranger", "TEXT");
    ensureColumn(m_db, "audio_files", "conductor", "TEXT");
    ensureColumn(m_db, "audio_files", "performer", "TEXT");
    ensureColumn(m_db, "audio_files", "publisher", "TEXT");
    ensureColumn(m_db, "audio_files", "bpm", "TEXT");
    ensureColumn(m_db, "audio_files", "musical_key", "TEXT");
    ensureColumn(m_db, "audio_files", "tempo", "TEXT");
    ensureColumn(m_db, "audio_files", "mood", "TEXT");
    ensureColumn(m_db, "audio_files", "usage", "TEXT");
    ensureColumn(m_db, "audio_files", "featured_instrument", "TEXT");
    ensureColumn(m_db, "audio_files", "version_tag", "TEXT");
    ensureColumn(m_db, "audio_files", "era", "TEXT");
    ensureColumn(m_db, "audio_files", "track_year", "TEXT");
    ensureColumn(m_db, "audio_files", "isrc", "TEXT");
    ensureColumn(m_db, "audio_files", "iswc", "TEXT");
    ensureColumn(m_db, "audio_files", "lyrics", "TEXT");
    // SFX / Post
    ensureColumn(m_db, "audio_files", "show_name", "TEXT");
    ensureColumn(m_db, "audio_files", "show_category", "TEXT");
    ensureColumn(m_db, "audio_files", "show_subcategory", "TEXT");
    ensureColumn(m_db, "audio_files", "designer", "TEXT");
    ensureColumn(m_db, "audio_files", "location", "TEXT");
    ensureColumn(m_db, "audio_files", "microphone", "TEXT");
    ensureColumn(m_db, "audio_files", "mic_perspective", "TEXT");
    ensureColumn(m_db, "audio_files", "rec_type", "TEXT");
    ensureColumn(m_db, "audio_files", "rec_medium", "TEXT");
    ensureColumn(m_db, "audio_files", "short_id", "TEXT");
    ensureColumn(m_db, "audio_files", "long_id", "TEXT");
    // Production
    ensureColumn(m_db, "audio_files", "scene", "TEXT");
    ensureColumn(m_db, "audio_files", "take_num", "TEXT");
    ensureColumn(m_db, "audio_files", "tape", "TEXT");
    // BWF / BEXT
    ensureColumn(m_db, "audio_files", "bw_description", "TEXT");
    ensureColumn(m_db, "audio_files", "bw_originator", "TEXT");
    ensureColumn(m_db, "audio_files", "bw_originator_ref", "TEXT");
    ensureColumn(m_db, "audio_files", "bw_date", "TEXT");
    ensureColumn(m_db, "audio_files", "bw_time", "TEXT");
    ensureColumn(m_db, "audio_files", "bw_umid", "TEXT");
    ensureColumn(m_db, "audio_files", "bw_coding_history", "TEXT");
    // iXML
    ensureColumn(m_db, "audio_files", "ixml_project", "TEXT");
    ensureColumn(m_db, "audio_files", "ixml_note", "TEXT");
    ensureColumn(m_db, "audio_files", "ixml_file_uid", "TEXT");
    ensureColumn(m_db, "audio_files", "ixml_track_layout", "TEXT");
    // Location
    ensureColumn(m_db, "audio_files", "shoot_date", "TEXT");
    // System / User
    ensureColumn(m_db, "audio_files", "track_colour", "TEXT");

    // Audio similarity features (F-028)
    ensureColumn(m_db, "audio_files", "audio_features", "BLOB");
    ensureColumn(m_db, "audio_files", "features_version", "INTEGER DEFAULT 0");

    // Add the derived search_data column.
    ensureColumn(m_db, "audio_files", "search_data", "TEXT");

    // Music analysis columns (F-033): key/bpm/mood + confidence + source tracking
    // The legacy TEXT columns bpm and musical_key are reused; SQLite affinity
    // accepts numeric writes, so ensureColumn leaves them unchanged.
    ensureColumn(m_db, "audio_files", "musical_key_camelot", "TEXT DEFAULT ''");
    ensureColumn(m_db, "audio_files", "key_confidence",      "REAL DEFAULT 0");
    ensureColumn(m_db, "audio_files", "key_source",          "INTEGER DEFAULT 0");
    ensureColumn(m_db, "audio_files", "bpm_confidence",      "REAL DEFAULT 0");
    ensureColumn(m_db, "audio_files", "bpm_source",          "INTEGER DEFAULT 0");
    ensureColumn(m_db, "audio_files", "mood_valence",        "REAL DEFAULT 0");
    ensureColumn(m_db, "audio_files", "mood_arousal",        "REAL DEFAULT 0");
    ensureColumn(m_db, "audio_files", "mood_source",         "INTEGER DEFAULT 0");

    // F-033 Music Mode extended columns
    ensureColumn(m_db, "audio_files", "album_artist",       "TEXT DEFAULT ''");
    ensureColumn(m_db, "audio_files", "track_number",       "INTEGER DEFAULT 0");
    ensureColumn(m_db, "audio_files", "disc_number",        "INTEGER DEFAULT 1");
    ensureColumn(m_db, "audio_files", "energy_tension",     "REAL DEFAULT 0");
    ensureColumn(m_db, "audio_files", "energy_drive",       "REAL DEFAULT 0");
    ensureColumn(m_db, "audio_files", "replay_gain_track",  "REAL DEFAULT 0");
    ensureColumn(m_db, "audio_files", "replay_gain_album",  "REAL DEFAULT 0");
    ensureColumn(m_db, "audio_files", "highlight_start_ms", "INTEGER DEFAULT 0");
    ensureColumn(m_db, "audio_files", "raw_genre",          "TEXT DEFAULT ''");
    ensureColumn(m_db, "audio_files", "raw_instruments",    "TEXT DEFAULT ''");
    ensureColumn(m_db, "audio_files", "norm_genre",         "TEXT DEFAULT ''");
    ensureColumn(m_db, "audio_files", "norm_instruments",   "TEXT DEFAULT ''");
    ensureColumn(m_db, "audio_files", "norm_mood_tags",     "TEXT DEFAULT ''");
    ensureColumn(m_db, "audio_files", "vocal_type",         "TEXT DEFAULT ''");
    ensureColumn(m_db, "audio_files", "snoozed_until",      "INTEGER DEFAULT 0");

    // Version 2 recomputes every existing row. Version 1 only filled NULLs, so
    // bumping the marker alone left old pinyin tokens in place.
    const int kSearchDataMigrationVersion = 2;
    if (ReadAppMetaInt(m_db, "search_data_migrated") != kSearchDataMigrationVersion) {
        DIAG_ACTION("Database", "SearchDataBackfillDeferred: remaining rows continue in the background");
    }

    // Sub-playlist migration: add parent_id + is_folder columns to playlists (F-017 v2)
    const int kSubPlaylistMigrationVersion = 1;
    if (ReadAppMetaInt(m_db, "sub_playlist_migrated") != kSubPlaylistMigrationVersion) {
        auto columnExists = [&](const char* table, const char* column) -> bool {
            std::string sql = std::string("PRAGMA table_info(") + table + ")";
            sqlite3_stmt* stmt = nullptr;
            if (sqlite3_prepare_v2(m_db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) return false;
            bool found = false;
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                const unsigned char* name = sqlite3_column_text(stmt, 1);
                if (name && std::strcmp(reinterpret_cast<const char*>(name), column) == 0) {
                    found = true;
                    break;
                }
            }
            sqlite3_finalize(stmt);
            return found;
        };

        bool ok = true;
        if (!columnExists("playlists", "parent_id")) {
            ok = ok && executeSQL("ALTER TABLE playlists ADD COLUMN parent_id INTEGER");
        }
        if (!columnExists("playlists", "is_folder")) {
            ok = ok && executeSQL("ALTER TABLE playlists ADD COLUMN is_folder INTEGER NOT NULL DEFAULT 0");
        }
        ok = ok && executeSQL("CREATE INDEX IF NOT EXISTS idx_playlists_parent ON playlists(parent_id)");
        if (ok) {
            WriteAppMetaInt(m_db, "sub_playlist_migrated", kSubPlaylistMigrationVersion);
            DIAG_ACTION_FMT("Database", "PlaylistSchemaUpgraded: sub_playlist v=%d", kSubPlaylistMigrationVersion);
        }
    }

    // T2.8: 一次性 canonicalize folder_path 列
    // 见 docs/plans/2026-04-30-canonical-display-separation.md
    // 幂等 + 单事务 + 行数守恒；失败 ROLLBACK 不影响其它启动流程
    if (!migrateFolderPathToCanonical_v1()) {
        DIAG_SYSTEM("Migration", "FolderPathCanonicalMigration_FAILED");
        // 不阻塞启动 — 老形态 row 仍可工作（旧路径），等下次启动重试
    }

    // Create indexes for faster search (MOVED TO END)
    const char* createIndexes = R"(
        -- 1. Unique path index for locating a file
        CREATE UNIQUE INDEX IF NOT EXISTS idx_files_path ON audio_files(file_path);
        
        -- 2. Sorting indexes used by header clicks on large libraries
        -- File name (default)
        CREATE INDEX IF NOT EXISTS idx_files_name ON audio_files(file_name);
        CREATE INDEX IF NOT EXISTS idx_files_name_natural ON audio_files(file_name COLLATE AURA_NATURAL);
        -- Sample-rate sort
        CREATE INDEX IF NOT EXISTS idx_files_sample_rate ON audio_files(sample_rate);
        -- Duration sort
        CREATE INDEX IF NOT EXISTS idx_files_duration ON audio_files(duration);
        -- Channel-count sort
        CREATE INDEX IF NOT EXISTS idx_files_channels ON audio_files(channels);
        -- 文件大小排序
        CREATE INDEX IF NOT EXISTS idx_files_size ON audio_files(file_size);
        -- Format sort
        CREATE INDEX IF NOT EXISTS idx_files_format ON audio_files(format);
        -- 入库日期排序（查看最近添加）
        CREATE INDEX IF NOT EXISTS idx_files_date_added ON audio_files(date_added);
        -- File type visibility count path (WAV/MP3/FLAC toggles).
        CREATE INDEX IF NOT EXISTS idx_files_extension_nocase ON audio_files(extension COLLATE NOCASE);
        
        -- 3. Composite indexes for folder/name and format/name filters
        CREATE INDEX IF NOT EXISTS idx_files_folder_name ON audio_files(folder_path, file_name);
        CREATE INDEX IF NOT EXISTS idx_files_folder_name_natural ON audio_files(folder_path, file_name COLLATE AURA_NATURAL);
        CREATE INDEX IF NOT EXISTS idx_files_format_name ON audio_files(format, file_name);
        CREATE INDEX IF NOT EXISTS idx_playlists_name_natural ON playlists(name COLLATE AURA_NATURAL);
        CREATE INDEX IF NOT EXISTS idx_playlists_parent_name_natural ON playlists(parent_id, name COLLATE AURA_NATURAL);
        CREATE INDEX IF NOT EXISTS idx_playlist_items_playlist_position ON playlist_items(playlist_id, position, file_id);
        CREATE INDEX IF NOT EXISTS idx_playlist_items_file_playlist ON playlist_items(file_id, playlist_id);

        -- 4. Cover-wall indexes
        -- 优化全局封面去重扫描 (GROUP BY 性能)
        CREATE INDEX IF NOT EXISTS idx_files_cover_dedup_emb ON audio_files(embedded_cover_key) WHERE embedded_cover_key <> '';
        CREATE INDEX IF NOT EXISTS idx_files_cover_dedup_fold ON audio_files(folder_cover_key) WHERE folder_cover_key <> '';
        -- 4b. Cover-filter and sort indexes eliminate random I/O on selection.
        -- Cover filtering and name sorting share one covering index.
        -- Avoid table lookups for sort columns so cold-cache first pages remain fast.
        CREATE INDEX IF NOT EXISTS idx_files_cover_fold_sort ON audio_files(folder_cover_key, file_name) WHERE folder_cover_key <> '';
        CREATE INDEX IF NOT EXISTS idx_files_cover_emb_sort ON audio_files(embedded_cover_key, file_name) WHERE embedded_cover_key <> '';
        
        -- 5. Business-state indexes
        CREATE INDEX IF NOT EXISTS idx_files_played ON audio_files(last_played);
        CREATE INDEX IF NOT EXISTS idx_files_category ON audio_files(category);
        CREATE INDEX IF NOT EXISTS idx_files_artist ON audio_files(artist);
    )";

    if (!executeSQL(createIndexes)) return false;

    // Deduplicated cover-thumbnail table.
    executeSQL(R"(
        CREATE TABLE IF NOT EXISTS cover_thumbs (
            cover_key   TEXT PRIMARY KEY,
            thumb_data  BLOB,
            mime        TEXT,
            created_at  INTEGER DEFAULT 0
        );
    )");

    // Folder index table used to open the tree without scanning audio_files.
    executeSQL(R"(
        CREATE TABLE IF NOT EXISTS folder_index (
            folder_path   TEXT PRIMARY KEY,
            file_count    INTEGER DEFAULT 0,
            last_updated  INTEGER DEFAULT 0
        );
    )");

    // Cover registry cache.
    // Stores one representative row for each deduplicated cover key.
    // Startup reads this small table instead of grouping the full audio_files table.
    executeSQL(R"(
        CREATE TABLE IF NOT EXISTS cover_cache_registry (
            cover_key    TEXT PRIMARY KEY,
            rep_path     TEXT,
            is_embedded  INTEGER DEFAULT 0,
            cover_phash  INTEGER DEFAULT 0
        );
    )");

    // Older databases may lack cover_phash, used to deduplicate across key formats.
    ensureColumn(m_db, "cover_cache_registry", "cover_phash", "INTEGER DEFAULT 0");
    // normalized_key stores canonical F|H| identities so UI code does not hash files.
    ensureColumn(m_db, "cover_cache_registry", "normalized_key", "TEXT DEFAULT ''");
    // v2: phash 算法版本 (0=未算, 1=旧aHash, 2=dHash)
    ensureColumn(m_db, "cover_cache_registry", "cover_phash_version", "INTEGER DEFAULT 0");
    executeSQL("CREATE INDEX IF NOT EXISTS idx_cover_registry_normalized_key ON cover_cache_registry(normalized_key)");
    executeSQL("CREATE INDEX IF NOT EXISTS idx_cover_registry_phash ON cover_cache_registry(cover_phash)");
    // Existing aHash rows start at version 1; the backfill worker upgrades them to dHash v2.
    executeSQL("UPDATE cover_cache_registry SET cover_phash_version = 1 WHERE cover_phash != 0 AND cover_phash_version = 0;");

    executeSQL(R"(
        CREATE TABLE IF NOT EXISTS cover_tile_membership (
            tile_key         TEXT NOT NULL,
            file_id          INTEGER NOT NULL,
            membership_kind  INTEGER DEFAULT 0,
            PRIMARY KEY (tile_key, file_id)
        );
    )");
    executeSQL("CREATE INDEX IF NOT EXISTS idx_cover_tile_membership_file_id ON cover_tile_membership(file_id)");

    executeSQL(R"(
        CREATE TABLE IF NOT EXISTS app_meta (
            key     TEXT PRIMARY KEY,
            value   TEXT NOT NULL
        );
    )");

    // One-time cleanup: purge corrupt embedded cover thumbs (bruteForceScanForImage false positives).
    // Audio PCM data was mistakenly stored as JPEG due to false FF D8 FF matches.
    // After purge, covers re-extract correctly on next access via fixed bruteForceScanForImage.
    {
        sqlite3_stmt* chk = nullptr;
        bool alreadyPurged = false;
        if (sqlite3_prepare_v2(m_db, "SELECT 1 FROM app_meta WHERE key = 'cover_thumb_purge_v1'", -1, &chk, nullptr) == SQLITE_OK) {
            alreadyPurged = (sqlite3_step(chk) == SQLITE_ROW);
            sqlite3_finalize(chk);
        }
        if (!alreadyPurged) {
            executeSQL("DELETE FROM blobs_db.cover_thumbs WHERE cover_key LIKE 'E|%'");
            executeSQL("UPDATE cover_cache_registry SET cover_phash = 0, cover_phash_version = 0 WHERE cover_key LIKE 'E|%'");
            executeSQL("INSERT OR REPLACE INTO app_meta (key, value) VALUES ('cover_thumb_purge_v1', '1')");
        }
    }

    // Cover registry v2 maintenance marker. Do not run syncCoverRegistry()
    // during open: it can GROUP BY the whole library and rewrite cover rows
    // while the splash screen is waiting. New scans finalize their own cover
    // deltas incrementally; legacy libraries are only marked for explicit
    // maintenance.
    executeSQL(
        "INSERT OR IGNORE INTO app_meta (key, value) "
        "VALUES ('cover_registry_needs_maintenance', '1')");

    // T2.9: cover_hash_version invalidation (T1.3 算法升级配套)
    //
    // 当 NormalizeCoverKeyByVisualIdentity 算法版本变化时, 旧 F|H|<oldhash>|<size>
    // 全部 stale, 必须强制重算. 通过 app_meta['cover_hash_version'] 与代码常量
    // kCoverHashAlgorithmVersion 比对触发一次性 invalidation.
    //
    // 仅作用于 F|H|% (visual identity hash 算法相关), 不影响:
    //   - E|<embedded>:        embedded cover hash, 走 MetadataReader 单独算法
    //   - F|PATH|<canonical>:  纯字符串 canonical, T2.7 backfill 已处理
    //   - F|<legacy>:          T2.7 backfill legacy 段已用新算法重算
    //
    // Guard: only the primary app connection runs this migration.
    // Worker-thread local connections (CoverModule, scan thread) skip it entirely —
    // they contend on BEGIN IMMEDIATE and cause the 562–1249ms frame spikes observed
    // in diagnostics. The primary connection writes cover_hash_version to app_meta;
    // workers will see version == kCoverHashAlgorithmVersion on their next SELECT.
    if (m_isPrimaryConnection) {
        constexpr int kCoverHashAlgorithmVersion = 2;  // T1.3: head + tail + size streaming hash

        int storedVersion = 0;
        sqlite3_stmt* chk = nullptr;
        if (sqlite3_prepare_v2(m_db,
                "SELECT value FROM app_meta WHERE key = 'cover_hash_version'",
                -1, &chk, nullptr) == SQLITE_OK) {
            if (sqlite3_step(chk) == SQLITE_ROW) {
                const unsigned char* v = sqlite3_column_text(chk, 0);
                if (v) storedVersion = std::atoi(reinterpret_cast<const char*>(v));
            }
            sqlite3_finalize(chk);
        }

        if (storedVersion != kCoverHashAlgorithmVersion) {
            DIAG_ACTION_FMT("Database",
                "cover_hash_version migration: stored=%d -> target=%d (lazy invalidation; startup full-table rewrite skipped)",
                storedVersion, kCoverHashAlgorithmVersion);

            // Large customer libraries can have millions of rows and tens of GB
            // of WAL. Clearing every F|H| key during Database::open() blocks the
            // splash screen and can repeat forever if any sidecar write fails.
            // Record the algorithm version here; stale thumbnails are harmless
            // and will be replaced naturally as covers are touched/rescanned.
            if (!WriteAppMetaInt(m_db, "cover_hash_version", kCoverHashAlgorithmVersion)) {
                m_lastError = "failed to persist cover_hash_version";
                return false;
            }
        }
    }

    executeSQL(R"(
        CREATE TABLE IF NOT EXISTS facet_query_cache (
            cache_key    TEXT PRIMARY KEY,
            payload      BLOB,
            entry_count  INTEGER DEFAULT 0,
            updated_at   INTEGER DEFAULT 0
        );
    )");
    executeSQL("CREATE INDEX IF NOT EXISTS idx_facet_query_cache_updated_at ON facet_query_cache(updated_at);");
    executeSQL(R"(
        CREATE TABLE IF NOT EXISTS tag_facet_index (
            field       TEXT NOT NULL,
            value       TEXT NOT NULL,
            weighting   INTEGER DEFAULT 0,
            PRIMARY KEY (field, value)
        );
    )");

    const int currentFacetCacheVersion = ReadAppMetaInt(m_db, "facet_query_cache_version");
    if (currentFacetCacheVersion != kFacetQueryCacheVersion) {
        executeSQL("DELETE FROM facet_query_cache;");
        if (!WriteAppMetaInt(m_db, "facet_query_cache_version", kFacetQueryCacheVersion)) {
            m_lastError = "failed to persist facet query cache version";
            return false;
        }
    }

    executeSQL(R"(
        CREATE TABLE IF NOT EXISTS folder_cover_facet_index (
            scope_path   TEXT NOT NULL,
            scope_depth  INTEGER NOT NULL,
            cover_key    TEXT NOT NULL,
            rep_path     TEXT NOT NULL,
            is_embedded  INTEGER NOT NULL DEFAULT 0,
            hit_count    INTEGER NOT NULL DEFAULT 0,
            updated_at   INTEGER DEFAULT 0,
            PRIMARY KEY (scope_path, scope_depth, cover_key, is_embedded)
        );
    )");
    executeSQL(R"(
        CREATE TABLE IF NOT EXISTS folder_cover_facet_scope_state (
            scope_path    TEXT PRIMARY KEY,
            scope_depth   INTEGER NOT NULL DEFAULT 0,
            row_count     INTEGER NOT NULL DEFAULT 0,
            updated_at    INTEGER DEFAULT 0
        );
    )");
    executeSQL(R"(
        CREATE TABLE IF NOT EXISTS folder_path_facet_index (
            scope_path   TEXT NOT NULL,
            scope_depth  INTEGER NOT NULL,
            folder_path  TEXT NOT NULL,
            hit_count    INTEGER NOT NULL DEFAULT 0,
            updated_at   INTEGER DEFAULT 0,
            PRIMARY KEY (scope_path, scope_depth, folder_path)
        );
    )");
    executeSQL("CREATE INDEX IF NOT EXISTS idx_folder_cover_facet_depth ON folder_cover_facet_index(scope_depth, scope_path, hit_count DESC);");
    executeSQL("CREATE INDEX IF NOT EXISTS idx_folder_cover_scope_depth ON folder_cover_facet_scope_state(scope_depth, scope_path);");
    executeSQL("CREATE INDEX IF NOT EXISTS idx_folder_path_facet_depth ON folder_path_facet_index(scope_depth, scope_path, hit_count DESC);");

    const int currentFolderFacetVersion = ReadAppMetaInt(m_db, "folder_cover_facet_index_version");
    if (currentFolderFacetVersion != kFolderCoverFacetIndexVersion) {
        executeSQL("DELETE FROM folder_cover_facet_index;");
        executeSQL("DELETE FROM folder_path_facet_index;");
        executeSQL("DELETE FROM folder_cover_facet_scope_state;");
        if (!WriteAppMetaInt(m_db, "folder_cover_facet_index_version", kFolderCoverFacetIndexVersion)) {
            m_lastError = "failed to persist folder cover facet index version";
            return false;
        }
    }

    // F-033 Music Mode: play queue tables
    // play_queue: ordered by position ASC. Callers must maintain position values when reordering.
    executeSQL(R"(
        CREATE TABLE IF NOT EXISTS play_queue (
            position    INTEGER PRIMARY KEY,
            file_id     INTEGER NOT NULL,
            added_at    INTEGER NOT NULL DEFAULT 0,
            FOREIGN KEY (file_id) REFERENCES audio_files(id) ON DELETE CASCADE
        );
    )");
    executeSQL(R"(
        CREATE TABLE IF NOT EXISTS play_queue_meta (
            key   TEXT PRIMARY KEY,
            value TEXT NOT NULL DEFAULT ''
        );
    )");

    // F-033 Music Mode: album and artist views (version-gated recreation)
    {
        const int currentMusicViewsVersion = ReadAppMetaInt(m_db, "music_views_version");
        if (currentMusicViewsVersion != kMusicViewsVersion) {
            executeSQL("DROP VIEW IF EXISTS v_albums;");
            executeSQL("DROP VIEW IF EXISTS v_artists;");
            executeSQL(R"(
                CREATE VIEW IF NOT EXISTS v_albums AS
                SELECT
                    COALESCE(NULLIF(album_artist,''), '') AS album_artist,
                    album,
                    MAX(CAST(COALESCE(NULLIF(track_year,''), '0') AS INTEGER)) AS year,
                    COUNT(*) AS track_count,
                    SUM(duration) AS total_duration,
                    MIN(NULLIF(folder_cover_src_path, '')) AS cover_path
                FROM audio_files
                WHERE album != '' AND album IS NOT NULL
                GROUP BY album;
            )");
            executeSQL(R"(
                CREATE VIEW IF NOT EXISTS v_artists AS
                SELECT
                    COALESCE(NULLIF(album_artist,''), artist) AS artist_name,
                    COUNT(DISTINCT album) AS album_count,
                    COUNT(*) AS track_count,
                    SUM(duration) AS total_duration
                FROM audio_files
                GROUP BY COALESCE(NULLIF(album_artist,''), artist);
            )");
            if (!WriteAppMetaInt(m_db, "music_views_version", kMusicViewsVersion)) {
                m_lastError = "failed to persist music views version";
                return false;
            }
        }
    }

    // F-033 Music Mode: default library_type for new databases
    // "SFX" = sound effects library (default), "MUSIC" = production music library
    executeSQL("INSERT OR IGNORE INTO app_meta(key, value) VALUES('library_type', 'SFX');");

    if (!ensureFtsSchema()) return false;

    return true;
}

bool Database::enterFastScanMode() {
    if (!m_db || !m_isPrimaryConnection) return false;
    if (m_fastScanModeActive) return true;
    if (!WriteAppMetaString(m_db, "scan_fast_mode_active", "1")) return false;
    if (!executeSQL("PRAGMA main.locking_mode = EXCLUSIVE;")) return false;
    if (!executeSQL("PRAGMA main.journal_mode = OFF;")) return false;
    if (!executeSQL("PRAGMA main.synchronous = OFF;")) return false;
    m_fastScanModeActive = true;
    return true;
}

bool Database::exitFastScanMode() {
    if (!m_db || !m_isPrimaryConnection) return false;
    if (!executeSQL("PRAGMA main.synchronous = NORMAL;")) return false;
    if (!executeSQL("PRAGMA main.journal_mode = WAL;")) return false;
    if (!executeSQL("PRAGMA main.locking_mode = NORMAL;")) return false;
    if (!WriteAppMetaString(m_db, "scan_fast_mode_active", "0")) return false;
    m_fastScanModeActive = false;
    return true;
}

bool Database::executeSQL(const std::string& sql) {
    if (!m_db) return false;

    char* errMsg = nullptr;
    int result = sqlite3_exec(m_db, sql.c_str(), nullptr, nullptr, &errMsg);

    if (result != SQLITE_OK) {
        m_lastError = errMsg ? errMsg : "Unknown error";
        sqlite3_free(errMsg);
        return false;
    }

    return true;
}

bool Database::loadFacetQueryCache(const std::string& cacheKey, std::vector<uint8_t>& payload) {
    payload.clear();
    if (!m_db || cacheKey.empty()) return false;

    const char* sql = "SELECT payload FROM facet_query_cache WHERE cache_key = ? LIMIT 1;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        m_lastError = sqlite3_errmsg(m_db);
        return false;
    }

    sqlite3_bind_text(stmt, 1, cacheKey.c_str(), -1, SQLITE_TRANSIENT);
    bool hit = false;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const void* blob = sqlite3_column_blob(stmt, 0);
        int blobSize = sqlite3_column_bytes(stmt, 0);
        if (blob && blobSize > 0) {
            payload.assign(static_cast<const uint8_t*>(blob),
                           static_cast<const uint8_t*>(blob) + blobSize);
            hit = true;
        }
    }
    sqlite3_finalize(stmt);
    return hit;
}

bool Database::saveFacetQueryCache(const std::string& cacheKey,
                                   const std::vector<uint8_t>& payload,
                                   int entryCount) {
    if (!m_db || cacheKey.empty() || payload.empty()) return false;
    if (sqlite3_db_readonly(m_db, "main") == 1) return false;

    const char* sql =
        "INSERT INTO facet_query_cache(cache_key, payload, entry_count, updated_at) "
        "VALUES(?, ?, ?, ?) "
        "ON CONFLICT(cache_key) DO UPDATE SET "
        "payload = excluded.payload, "
        "entry_count = excluded.entry_count, "
        "updated_at = excluded.updated_at;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        m_lastError = sqlite3_errmsg(m_db);
        return false;
    }

    sqlite3_bind_text(stmt, 1, cacheKey.c_str(), -1, SQLITE_TRANSIENT);
    int blobSize = SafeSqliteBlobSize(payload.size());
    if (blobSize < 0) {
        sqlite3_finalize(stmt);
        return false;
    }
    sqlite3_bind_blob(stmt, 2, payload.data(), blobSize, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 3, entryCount);
    sqlite3_bind_int64(stmt, 4, static_cast<sqlite3_int64>(std::time(nullptr)));
    bool ok = (sqlite3_step(stmt) == SQLITE_DONE);
    if (!ok) m_lastError = sqlite3_errmsg(m_db);
    sqlite3_finalize(stmt);
    if (ok) pruneFacetQueryCache(kFacetQueryCacheMaxEntries);
    return ok;
}

bool Database::invalidateFacetQueryCache() {
    if (!m_db) return false;
    m_countMemCache.clear();  // also clear in-memory count cache
    if (sqlite3_db_readonly(m_db, "main") == 1) return true;
    bool ok = executeSQL("DELETE FROM facet_query_cache;");
    ok = invalidateTagFacetIndex() && ok;
    return ok;
}

void Database::pruneFacetQueryCache(int maxEntries) {
    if (!m_db || maxEntries <= 0) return;
    if (sqlite3_db_readonly(m_db, "main") == 1) return;
    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "DELETE FROM facet_query_cache "
        "WHERE cache_key IN ("
        "  SELECT cache_key FROM facet_query_cache "
        "  ORDER BY updated_at DESC, cache_key DESC "
        "  LIMIT -1 OFFSET ?"
        ");";
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) return;
    sqlite3_bind_int(stmt, 1, maxEntries);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

bool Database::invalidateFolderCoverFacetIndex() {
    if (!m_db) return false;
    if (sqlite3_db_readonly(m_db, "main") == 1) return true;
    bool ok = executeSQL("DELETE FROM folder_cover_facet_index;");
    ok = executeSQL("DELETE FROM folder_path_facet_index;") && ok;
    ok = executeSQL("DELETE FROM folder_cover_facet_scope_state;") && ok;
    return ok;
}

static void CollectFolderFacetScopesForPath(const std::string& filePathUtf8,
                                            std::set<std::string>& outScopes) {
    if (filePathUtf8.empty()) return;

    try {
        fs::path p = Utf8ToPath(filePathUtf8);
        fs::path folder = p.parent_path();
        if (folder.empty()) return;

        std::string folderUtf8 = PathToUtf8(folder);
        if (folderUtf8.empty()) return;

        // Global scope is affected by every change.
        outScopes.insert("");

        // 当前目录及其祖先目录都可能受影响
        fs::path cur = folder;
        while (!cur.empty()) {
            std::string scope = PathToUtf8(cur);
            if (!scope.empty()) {
                int depth = GetFolderPathDepth(scope);
                if (depth <= kFolderCoverFacetMaxDepth) {
                    outScopes.insert(scope);
                }
            }

            fs::path parent = cur.parent_path();
            if (parent == cur || parent.empty()) break;
            cur = parent;
        }
    } catch (...) {
        outScopes.insert("");
    }
}

bool Database::invalidateFolderCoverFacetIndexForPaths(const std::vector<std::string>& filePaths) {
    if (!m_db) return false;
    if (sqlite3_db_readonly(m_db, "main") == 1) return true;
    if (filePaths.empty()) return true;

    std::set<std::string> scopes;
    for (const auto& p : filePaths) {
        CollectFolderFacetScopesForPath(p, scopes);
    }
    if (scopes.empty()) return true;

    const char* sql1 = "DELETE FROM folder_cover_facet_index WHERE scope_path = ?;";
    const char* sql2 = "DELETE FROM folder_path_facet_index WHERE scope_path = ?;";
    const char* sql3 = "DELETE FROM folder_cover_facet_scope_state WHERE scope_path = ?;";

    sqlite3_stmt* st1 = nullptr;
    sqlite3_stmt* st2 = nullptr;
    sqlite3_stmt* st3 = nullptr;

    if (sqlite3_prepare_v2(m_db, sql1, -1, &st1, nullptr) != SQLITE_OK ||
        sqlite3_prepare_v2(m_db, sql2, -1, &st2, nullptr) != SQLITE_OK ||
        sqlite3_prepare_v2(m_db, sql3, -1, &st3, nullptr) != SQLITE_OK) {
        m_lastError = sqlite3_errmsg(m_db);
        if (st1) sqlite3_finalize(st1);
        if (st2) sqlite3_finalize(st2);
        if (st3) sqlite3_finalize(st3);
        return false;
    }

    bool ownTxn = beginTransaction();
    if (!ownTxn) {
        sqlite3_finalize(st1);
        sqlite3_finalize(st2);
        sqlite3_finalize(st3);
        return false;
    }

    for (const auto& scope : scopes) {
        sqlite3_reset(st1);
        sqlite3_clear_bindings(st1);
        sqlite3_bind_text(st1, 1, scope.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(st1) != SQLITE_DONE) {
            m_lastError = sqlite3_errmsg(m_db);
            rollbackTransaction();
            sqlite3_finalize(st1);
            sqlite3_finalize(st2);
            sqlite3_finalize(st3);
            return false;
        }

        sqlite3_reset(st2);
        sqlite3_clear_bindings(st2);
        sqlite3_bind_text(st2, 1, scope.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(st2) != SQLITE_DONE) {
            m_lastError = sqlite3_errmsg(m_db);
            rollbackTransaction();
            sqlite3_finalize(st1);
            sqlite3_finalize(st2);
            sqlite3_finalize(st3);
            return false;
        }

        sqlite3_reset(st3);
        sqlite3_clear_bindings(st3);
        sqlite3_bind_text(st3, 1, scope.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(st3) != SQLITE_DONE) {
            m_lastError = sqlite3_errmsg(m_db);
            rollbackTransaction();
            sqlite3_finalize(st1);
            sqlite3_finalize(st2);
            sqlite3_finalize(st3);
            return false;
        }
    }

    sqlite3_finalize(st1);
    sqlite3_finalize(st2);
    sqlite3_finalize(st3);

    return commitTransaction();
}

bool Database::materializeFolderCoverFacetScope(const std::string& scopePath, int scopeDepth) {
    if (!m_db) return false;
    if (sqlite3_db_readonly(m_db, "main") == 1) return false;
    if (scopeDepth < 0 || scopeDepth > kFolderCoverFacetMaxDepth) return false;

    const bool hasScope = !scopePath.empty();
    const std::string upper = hasScope ? MakeFolderPathUpperBound(scopePath) : std::string();
    const sqlite3_int64 nowTs = static_cast<sqlite3_int64>(std::time(nullptr));

    bool ownTxn = beginTransaction();

    auto rollbackIfNeeded = [&]() {
        if (ownTxn) rollbackTransaction();
    };

    sqlite3_stmt* stmtDeleteRows = nullptr;
    sqlite3_stmt* stmtDeletePathRows = nullptr;
    sqlite3_stmt* stmtDeleteState = nullptr;
    sqlite3_stmt* stmtInsertRows = nullptr;
    sqlite3_stmt* stmtInsertPathRows = nullptr;
    sqlite3_stmt* stmtInsertState = nullptr;

    const char* sqlDeleteRows =
        "DELETE FROM folder_cover_facet_index WHERE scope_path = ? AND scope_depth = ?;";
    const char* sqlDeletePathRows =
        "DELETE FROM folder_path_facet_index WHERE scope_path = ? AND scope_depth = ?;";
    const char* sqlDeleteState =
        "DELETE FROM folder_cover_facet_scope_state WHERE scope_path = ?;";
    // v3: unified E|+F| candidate stream with final GROUP BY grp (matches getCoverFacets main query).
    // MIN(is_emb) prefers folder covers (0) as group representative.
    const char* sqlInsertRows = R"SQL(
        INSERT INTO folder_cover_facet_index
            (scope_path, scope_depth, cover_key, rep_path, is_embedded, hit_count, updated_at)
        SELECT ?, ?, MIN(key), MIN(rep_p), MIN(is_emb), SUM(hit_count), ?
        FROM (
            SELECT embedded_cover_key AS key,
                   file_path AS rep_p,
                   1 AS is_emb,
                   1 AS hit_count,
                   COALESCE(
                       (SELECT cover_phash FROM cover_cache_registry
                        WHERE cover_key = embedded_cover_key AND cover_phash != 0),
                       embedded_cover_key
                   ) AS grp
            FROM audio_files
            WHERE (? = 0 OR (folder_path >= ? AND folder_path < ?))
              AND embedded_cover_key IS NOT NULL AND embedded_cover_key <> ''
            UNION ALL
            SELECT folder_cover_key AS key,
                   folder_path AS rep_p,
                   0 AS is_emb,
                   1 AS hit_count,
                   COALESCE(
                       (SELECT cover_phash FROM cover_cache_registry
                        WHERE cover_key = folder_cover_key AND cover_phash != 0),
                       folder_cover_key
                   ) AS grp
            FROM audio_files
            WHERE (? = 0 OR (folder_path >= ? AND folder_path < ?))
              AND folder_cover_key IS NOT NULL AND folder_cover_key <> ''
        )
        GROUP BY grp;
    )SQL";
    const char* sqlInsertPathRows = R"SQL(
        INSERT INTO folder_path_facet_index
            (scope_path, scope_depth, folder_path, hit_count, updated_at)
        SELECT ?, ?, folder_path, COUNT(*) AS hit_count, ?
        FROM audio_files
        WHERE (? = 0 OR (folder_path >= ? AND folder_path < ?))
          AND folder_path IS NOT NULL AND folder_path <> ''
        GROUP BY folder_path;
    )SQL";
    const char* sqlInsertState =
        "INSERT OR REPLACE INTO folder_cover_facet_scope_state "
        "(scope_path, scope_depth, row_count, updated_at) VALUES (?, ?, ?, ?);";

    if (sqlite3_prepare_v2(m_db, sqlDeleteRows, -1, &stmtDeleteRows, nullptr) != SQLITE_OK ||
        sqlite3_prepare_v2(m_db, sqlDeletePathRows, -1, &stmtDeletePathRows, nullptr) != SQLITE_OK ||
        sqlite3_prepare_v2(m_db, sqlDeleteState, -1, &stmtDeleteState, nullptr) != SQLITE_OK ||
        sqlite3_prepare_v2(m_db, sqlInsertRows, -1, &stmtInsertRows, nullptr) != SQLITE_OK ||
        sqlite3_prepare_v2(m_db, sqlInsertPathRows, -1, &stmtInsertPathRows, nullptr) != SQLITE_OK ||
        sqlite3_prepare_v2(m_db, sqlInsertState, -1, &stmtInsertState, nullptr) != SQLITE_OK) {
        m_lastError = sqlite3_errmsg(m_db);
        if (stmtDeleteRows) sqlite3_finalize(stmtDeleteRows);
        if (stmtDeletePathRows) sqlite3_finalize(stmtDeletePathRows);
        if (stmtDeleteState) sqlite3_finalize(stmtDeleteState);
        if (stmtInsertRows) sqlite3_finalize(stmtInsertRows);
        if (stmtInsertPathRows) sqlite3_finalize(stmtInsertPathRows);
        if (stmtInsertState) sqlite3_finalize(stmtInsertState);
        rollbackIfNeeded();
        return false;
    }

    sqlite3_bind_text(stmtDeleteRows, 1, scopePath.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmtDeleteRows, 2, scopeDepth);
    if (sqlite3_step(stmtDeleteRows) != SQLITE_DONE) {
        m_lastError = sqlite3_errmsg(m_db);
        sqlite3_finalize(stmtDeleteRows);
        sqlite3_finalize(stmtDeletePathRows);
        sqlite3_finalize(stmtDeleteState);
        sqlite3_finalize(stmtInsertRows);
        sqlite3_finalize(stmtInsertPathRows);
        sqlite3_finalize(stmtInsertState);
        rollbackIfNeeded();
        return false;
    }

    sqlite3_bind_text(stmtDeletePathRows, 1, scopePath.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmtDeletePathRows, 2, scopeDepth);
    if (sqlite3_step(stmtDeletePathRows) != SQLITE_DONE) {
        m_lastError = sqlite3_errmsg(m_db);
        sqlite3_finalize(stmtDeleteRows);
        sqlite3_finalize(stmtDeletePathRows);
        sqlite3_finalize(stmtDeleteState);
        sqlite3_finalize(stmtInsertRows);
        sqlite3_finalize(stmtInsertPathRows);
        sqlite3_finalize(stmtInsertState);
        rollbackIfNeeded();
        return false;
    }

    sqlite3_bind_text(stmtDeleteState, 1, scopePath.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmtDeleteState) != SQLITE_DONE) {
        m_lastError = sqlite3_errmsg(m_db);
        sqlite3_finalize(stmtDeleteRows);
        sqlite3_finalize(stmtDeletePathRows);
        sqlite3_finalize(stmtDeleteState);
        sqlite3_finalize(stmtInsertRows);
        sqlite3_finalize(stmtInsertPathRows);
        sqlite3_finalize(stmtInsertState);
        rollbackIfNeeded();
        return false;
    }

    sqlite3_bind_text(stmtInsertRows, 1, scopePath.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmtInsertRows, 2, scopeDepth);
    sqlite3_bind_int64(stmtInsertRows, 3, nowTs);
    sqlite3_bind_int(stmtInsertRows, 4, hasScope ? 1 : 0);
    sqlite3_bind_text(stmtInsertRows, 5, scopePath.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmtInsertRows, 6, upper.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmtInsertRows, 7, hasScope ? 1 : 0);
    sqlite3_bind_text(stmtInsertRows, 8, scopePath.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmtInsertRows, 9, upper.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmtInsertRows) != SQLITE_DONE) {
        m_lastError = sqlite3_errmsg(m_db);
        sqlite3_finalize(stmtDeleteRows);
        sqlite3_finalize(stmtDeletePathRows);
        sqlite3_finalize(stmtDeleteState);
        sqlite3_finalize(stmtInsertRows);
        sqlite3_finalize(stmtInsertPathRows);
        sqlite3_finalize(stmtInsertState);
        rollbackIfNeeded();
        return false;
    }
    const int rowCount = sqlite3_changes(m_db);

    sqlite3_bind_text(stmtInsertPathRows, 1, scopePath.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmtInsertPathRows, 2, scopeDepth);
    sqlite3_bind_int64(stmtInsertPathRows, 3, nowTs);
    sqlite3_bind_int(stmtInsertPathRows, 4, hasScope ? 1 : 0);
    sqlite3_bind_text(stmtInsertPathRows, 5, scopePath.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmtInsertPathRows, 6, upper.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmtInsertPathRows) != SQLITE_DONE) {
        m_lastError = sqlite3_errmsg(m_db);
        sqlite3_finalize(stmtDeleteRows);
        sqlite3_finalize(stmtDeletePathRows);
        sqlite3_finalize(stmtDeleteState);
        sqlite3_finalize(stmtInsertRows);
        sqlite3_finalize(stmtInsertPathRows);
        sqlite3_finalize(stmtInsertState);
        rollbackIfNeeded();
        return false;
    }

    sqlite3_bind_text(stmtInsertState, 1, scopePath.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmtInsertState, 2, scopeDepth);
    sqlite3_bind_int(stmtInsertState, 3, rowCount);
    sqlite3_bind_int64(stmtInsertState, 4, nowTs);
    if (sqlite3_step(stmtInsertState) != SQLITE_DONE) {
        m_lastError = sqlite3_errmsg(m_db);
        sqlite3_finalize(stmtDeleteRows);
        sqlite3_finalize(stmtDeleteState);
        sqlite3_finalize(stmtInsertRows);
        sqlite3_finalize(stmtInsertState);
        rollbackIfNeeded();
        return false;
    }

    sqlite3_finalize(stmtDeleteRows);
    sqlite3_finalize(stmtDeletePathRows);
    sqlite3_finalize(stmtDeleteState);
    sqlite3_finalize(stmtInsertRows);
    sqlite3_finalize(stmtInsertPathRows);
    sqlite3_finalize(stmtInsertState);

    if (ownTxn && !commitTransaction()) return false;

    DIAG_ACTION_FMT("Search",
                    "FolderCoverFacetMaterialized: scope=%s depth=%d rows=%d",
                    scopePath.c_str(), scopeDepth, rowCount);
    return true;
}

bool Database::tryGetMaterializedFolderCoverFacets(const SearchFilter& filter,
                                                   int limit,
                                                   std::vector<CoverFacetEntry>& results) {
    results.clear();
    if (!m_db || limit <= 0) return false;
    if (!IsPureFolderFacetFilter(filter)) return false;

    const std::string scopePath = filter.folderPath;
    const int scopeDepth = scopePath.empty() ? 0 : GetFolderPathDepth(scopePath);
    if (scopeDepth > kFolderCoverFacetMaxDepth) return false;

    bool scopeReady = false;
    sqlite3_stmt* stmtState = nullptr;
    const char* sqlState =
        "SELECT row_count FROM folder_cover_facet_scope_state WHERE scope_path = ? LIMIT 1;";
    if (sqlite3_prepare_v2(m_db, sqlState, -1, &stmtState, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmtState, 1, scopePath.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(stmtState) == SQLITE_ROW) {
            scopeReady = true;
        }
    }
    if (stmtState) sqlite3_finalize(stmtState);

    if (!scopeReady) {
        if (!materializeFolderCoverFacetScope(scopePath, scopeDepth)) {
            return false;
        }
    }

    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "SELECT cover_key, rep_path, is_embedded, hit_count "
        "FROM folder_cover_facet_index "
        "WHERE scope_path = ? AND scope_depth = ? "
        "ORDER BY hit_count DESC, cover_key "
        "LIMIT ?;";
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        m_lastError = sqlite3_errmsg(m_db);
        return false;
    }
    sqlite3_bind_text(stmt, 1, scopePath.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, scopeDepth);
    sqlite3_bind_int(stmt, 3, limit);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        CoverFacetEntry e;
        e.key = safeGetText(stmt, 0);
        e.path = safeGetText(stmt, 1);
        e.isEmbedded = sqlite3_column_int(stmt, 2) != 0;
        e.count = sqlite3_column_int64(stmt, 3);
        results.push_back(std::move(e));
    }
    sqlite3_finalize(stmt);
    DIAG_ACTION_FMT("Search",
                    "FolderCoverFacetIndexHit: scope=%s depth=%d rows=%d",
                    scopePath.c_str(), scopeDepth, (int)results.size());
    return true;
}

bool Database::tryGetMaterializedFolderPathFacets(const SearchFilter& filter,
                                                  int limit,
                                                  std::vector<PathFacetEntry>& results) {
    results.clear();
    if (!m_db || limit <= 0) return false;
    if (!IsPureFolderFacetFilter(filter)) return false;

    const std::string scopePath = filter.folderPath;
    const int scopeDepth = scopePath.empty() ? 0 : GetFolderPathDepth(scopePath);
    if (scopeDepth > kFolderCoverFacetMaxDepth) return false;

    bool scopeReady = false;
    sqlite3_stmt* stmtState = nullptr;
    const char* sqlState =
        "SELECT row_count FROM folder_cover_facet_scope_state WHERE scope_path = ? LIMIT 1;";
    if (sqlite3_prepare_v2(m_db, sqlState, -1, &stmtState, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmtState, 1, scopePath.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(stmtState) == SQLITE_ROW) {
            scopeReady = true;
        }
    }
    if (stmtState) sqlite3_finalize(stmtState);

    if (!scopeReady) {
        if (!materializeFolderCoverFacetScope(scopePath, scopeDepth)) {
            return false;
        }
    }

    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "SELECT folder_path, hit_count "
        "FROM folder_path_facet_index "
        "WHERE scope_path = ? AND scope_depth = ? "
        "ORDER BY hit_count DESC, folder_path "
        "LIMIT ?;";
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        m_lastError = sqlite3_errmsg(m_db);
        return false;
    }
    sqlite3_bind_text(stmt, 1, scopePath.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, scopeDepth);
    sqlite3_bind_int(stmt, 3, limit);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        PathFacetEntry e;
        e.folderPath = safeGetText(stmt, 0);
        e.count = sqlite3_column_int64(stmt, 1);
        results.push_back(std::move(e));
    }
    sqlite3_finalize(stmt);
    DIAG_ACTION_FMT("Search",
                    "FolderPathFacetIndexHit: scope=%s depth=%d rows=%d",
                    scopePath.c_str(), scopeDepth, (int)results.size());
    return true;
}

void Database::prewarmFolderCoverFacetIndex(int maxDepth, int maxScopes) {
    if (!m_db || sqlite3_db_readonly(m_db, "main") == 1) return;
    if (maxDepth <= 0 || maxScopes <= 0) return;

    std::vector<FolderIndexEntry> folders = getFolderIndex();
    std::vector<std::string> scopes;
    scopes.reserve((std::min)((int)folders.size(), maxScopes));
    scopes.push_back("");
    for (const auto& f : folders) {
        if (f.folderPath.empty()) continue;
        const int depth = GetFolderPathDepth(f.folderPath);
        if (depth <= 0 || depth > maxDepth) continue;
        scopes.push_back(f.folderPath);
    }
    std::sort(scopes.begin(), scopes.end(),
              [](const std::string& a, const std::string& b) {
                  const int da = GetFolderPathDepth(a);
                  const int db = GetFolderPathDepth(b);
                  if (da != db) return da < db;
                  return a < b;
              });
    scopes.erase(std::unique(scopes.begin(), scopes.end()), scopes.end());
    if ((int)scopes.size() > maxScopes) {
        scopes.resize(maxScopes);
    }

    for (const auto& scope : scopes) {
        const int depth = scope.empty() ? 0 : GetFolderPathDepth(scope);
        materializeFolderCoverFacetScope(scope, depth);
    }
}

int64_t Database::getLastInsertId() {
    return sqlite3_last_insert_rowid(m_db);
}

// Build the UPSERT SQL once from kExtMetaCols (thread-safe via static local)
static const std::string& GetAddFileSql() {
    static const std::string sql = []() {
        // Base columns (param 1-44)
        std::string cols =
            "file_path, file_name, extension, file_size, "
            "sample_rate, bit_depth, channels, total_frames, duration, format, "
            "title, artist, album, description, category, keywords, rating, "
            "play_count, date_added, last_played, "
            "folder_path, file_mtime, last_indexed, has_cover, "
            "embedded_cover_key, embedded_cover_key_mtime, "
            "folder_cover_key, folder_cover_src_path, folder_cover_src_mtime, "
            "search_data, "
            // F-033 Music Mode fields (param 31-44)
            "album_artist, track_number, disc_number, "
            "energy_tension, energy_drive, "
            "replay_gain_track, replay_gain_album, highlight_start_ms, "
            "raw_genre, raw_instruments, norm_genre, norm_instruments, norm_mood_tags, vocal_type";
        int baseCount = 44;

        // Extended meta columns
        for (size_t i = 0; i < kExtMetaCount; ++i) {
            cols += ", ";
            cols += kExtMetaCols[i].col;
        }

        // VALUES placeholders
        std::string vals;
        int totalParams = baseCount + (int)kExtMetaCount;
        for (int i = 1; i <= totalParams; ++i) {
            if (i > 1) vals += ", ";
            vals += "?";
        }

        // ON CONFLICT SET clauses
        // Always overwrite: technical fields + rescanOverwrite extended fields
        std::string upsert =
            "file_name = excluded.file_name, "
            "extension = excluded.extension, "
            "file_size = excluded.file_size, "
            "sample_rate = excluded.sample_rate, "
            "bit_depth = excluded.bit_depth, "
            "channels = excluded.channels, "
            "total_frames = excluded.total_frames, "
            "duration = excluded.duration, "
            "format = excluded.format, "
            "title = excluded.title, "
            "artist = excluded.artist, "
            "album = excluded.album, "
            "folder_path = excluded.folder_path, "
            "file_mtime = excluded.file_mtime, "
            "last_indexed = excluded.last_indexed, "
            "has_cover = excluded.has_cover, "
            "embedded_cover_key = excluded.embedded_cover_key, "
            "embedded_cover_key_mtime = excluded.embedded_cover_key_mtime, "
            "folder_cover_key = excluded.folder_cover_key, "
            "folder_cover_src_path = excluded.folder_cover_src_path, "
            "folder_cover_src_mtime = excluded.folder_cover_src_mtime, "
            // F-033 music fields always refresh on rescan.
            "album_artist = excluded.album_artist, "
            "track_number = excluded.track_number, "
            "disc_number = excluded.disc_number, "
            "energy_tension = excluded.energy_tension, "
            "energy_drive = excluded.energy_drive, "
            "replay_gain_track = excluded.replay_gain_track, "
            "replay_gain_album = excluded.replay_gain_album, "
            "highlight_start_ms = excluded.highlight_start_ms, "
            "raw_genre = excluded.raw_genre, "
            "raw_instruments = excluded.raw_instruments, "
            "norm_genre = excluded.norm_genre, "
            "norm_instruments = excluded.norm_instruments, "
            "norm_mood_tags = excluded.norm_mood_tags, "
            "vocal_type = excluded.vocal_type";

        // Extended: rescanOverwrite columns get overwritten, others stay as-is
        for (size_t i = 0; i < kExtMetaCount; ++i) {
            if (kExtMetaCols[i].rescanOverwrite) {
                const char* col = kExtMetaCols[i].col;
                if (strcmp(col, "bpm") == 0) {
                    upsert += std::string(", bpm = COALESCE(excluded.bpm, audio_files.bpm)");
                } else {
                    upsert += std::string(", ") + col + " = excluded." + col;
                }
            }
        }

        // search_data on upsert: use new file_name/title + preserved desc/cat/kw + searchable extended
        std::string searchExpr =
            "excluded.file_name || ' ' || IFNULL(excluded.title,'') || ' ' || "
            "IFNULL(audio_files.description,'') || ' ' || "
            "IFNULL(audio_files.category,'') || ' ' || "
            "IFNULL(audio_files.keywords,'')";
        for (size_t i = 0; i < kExtMetaCount; ++i) {
            if (kExtMetaCols[i].searchable) {
                searchExpr += std::string(" || ' ' || IFNULL(") +
                    (kExtMetaCols[i].rescanOverwrite ? "excluded." : "audio_files.") +
                    kExtMetaCols[i].col + ",'')";
            }
        }
        upsert += ", search_data = aura_search_data(" + searchExpr + ")";

        return "INSERT INTO audio_files (" + cols + ") VALUES (" + vals +
               ") ON CONFLICT(file_path) DO UPDATE SET " + upsert;
    }();
    return sql;
}

bool Database::addFile(const AudioRecord& record) {
    if (!m_db) return false;

    std::string folderPath = record.folderPath;
    if (folderPath.empty()) {
        try {
            fs::path p = Utf8ToPath(record.filePath);
            folderPath = PathToUtf8(p.parent_path());
        } catch (...) {
            folderPath.clear();
        }
    }
    folderPath = HarkenSearch::CanonicalFolderPath(folderPath);

    std::string storageFilePath = record.filePath;
#ifdef _WIN32
    if (!folderPath.empty() && !record.fileName.empty()) {
        const char* identitySql =
            "SELECT file_path FROM audio_files "
            "WHERE folder_path = ? AND file_name = ? COLLATE NOCASE LIMIT 1";
        sqlite3_stmt* identityStmt = nullptr;
        if (sqlite3_prepare_v2(m_db, identitySql, -1, &identityStmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(identityStmt, 1, folderPath.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(identityStmt, 2, record.fileName.c_str(), -1, SQLITE_TRANSIENT);
            if (sqlite3_step(identityStmt) == SQLITE_ROW) {
                storageFilePath = safeGetText(identityStmt, 0);
            }
        }
        sqlite3_finalize(identityStmt);
    }
#endif

    // --- Relink check (F-034): if a fingerprint-matching orphan exists, relink instead of INSERT ---
    // Skip relink check when fingerprint fields are incomplete (metadata decode not yet done).
    // Files with zero-valued fields will fall through to normal INSERT.
    if (record.fileSize > 0 && record.totalFrames > 0 && record.sampleRate > 0) {
        auto orphan = findOrphanByFingerprint(
            record.fileName, record.fileSize, record.totalFrames, record.sampleRate);
        if (orphan.has_value() && orphan->dbPath != storageFilePath) {
            // F-034 limitation: cache invalidation still occurs inside the batch transaction.
            // F-034 limitation: the caller still counts relink success as stats.added.
            bool ok = updateFilePath(orphan->dbPath, storageFilePath, record.fileName);
            if (ok) {
                DIAG_ACTION_FMT("Relink", "AutoRelinkSuccess: %s -> %s",
                    orphan->dbPath.c_str(), storageFilePath.c_str());
                return true;
            }
            // If relink fails, continue with the normal INSERT path.
            DIAG_ACTION_FMT("Relink", "AutoRelinkFailed: %s -> %s (falling through to INSERT)",
                orphan->dbPath.c_str(), storageFilePath.c_str());
        }
    }

    const std::string& sql = GetAddFileSql();

    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(m_db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        m_lastError = sqlite3_errmsg(m_db);
        return false;
    }

    int64_t now = getCurrentTimestamp();
    // T2.1: canonicalize folder_path before bind so all rows share the
    // same compare-key form (small-letter + backslash + strip \\?\). file_path
    // intentionally stays as-is for display recovery — see T1.0 v2 §5.1.
    int64_t fileMtime = record.fileMtime;
    int64_t lastIndexed = record.lastIndexed > 0 ? record.lastIndexed : now;

    // Base columns (params 1-31)
    sqlite3_bind_text(stmt, 1, storageFilePath.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, record.fileName.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, record.extension.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 4, record.fileSize);
    sqlite3_bind_int(stmt, 5, record.sampleRate);
    sqlite3_bind_int(stmt, 6, record.bitDepth);
    sqlite3_bind_int(stmt, 7, record.channels);
    sqlite3_bind_int64(stmt, 8, record.totalFrames);
    sqlite3_bind_double(stmt, 9, record.durationSeconds);
    sqlite3_bind_text(stmt, 10, record.format.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 11, record.title.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 12, record.artist.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 13, record.album.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 14, record.description.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 15, record.category.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 16, record.keywords.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 17, record.rating);
    sqlite3_bind_int(stmt, 18, record.playCount);
    sqlite3_bind_int64(stmt, 19, record.dateAdded > 0 ? record.dateAdded : now);
    sqlite3_bind_int64(stmt, 20, record.lastPlayed);
    sqlite3_bind_text(stmt, 21, folderPath.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 22, fileMtime);
    sqlite3_bind_int64(stmt, 23, lastIndexed);
    sqlite3_bind_int(stmt, 24, record.hasCover ? 1 : 0);
    sqlite3_bind_text(stmt, 25, record.embeddedCoverKey.empty() ? nullptr : record.embeddedCoverKey.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 26, record.embeddedCoverKeyMtime);
    sqlite3_bind_text(stmt, 27, record.folderCoverKey.empty() ? nullptr : record.folderCoverKey.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 28, record.folderCoverSrcPath.empty() ? nullptr : record.folderCoverSrcPath.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 29, record.folderCoverSrcMtime);

    // Build normalized search data from core and extended searchable fields.
    std::string searchDataText = record.fileName + " " + record.title + " " + record.description + " " + record.category + " " + record.keywords;
    for (size_t i = 0; i < kExtMetaCount; ++i) {
        if (!kExtMetaCols[i].searchable) continue;
        auto it = record.meta.find(kExtMetaCols[i].col);
        if (it != record.meta.end() && !it->second.empty()) {
            searchDataText += " " + it->second;
        }
    }
    std::string searchData = HarkenSearch::Search::BuildSearchDataForFts(searchDataText);
    sqlite3_bind_text(stmt, 30, searchData.c_str(), -1, SQLITE_TRANSIENT);

    // F-033 Music Mode fields (params 31-44)
    sqlite3_bind_text(stmt, 31, record.albumArtist.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 32, record.trackNumber);
    sqlite3_bind_int(stmt, 33, record.discNumber);
    sqlite3_bind_double(stmt, 34, record.energyTension);
    sqlite3_bind_double(stmt, 35, record.energyDrive);
    sqlite3_bind_double(stmt, 36, record.replayGainTrack);
    sqlite3_bind_double(stmt, 37, record.replayGainAlbum);
    sqlite3_bind_int(stmt, 38, record.highlightStartMs);
    sqlite3_bind_text(stmt, 39, record.rawGenre.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 40, record.rawInstruments.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 41, record.normGenre.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 42, record.normInstruments.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 43, record.normMoodTags.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 44, record.vocalType.c_str(), -1, SQLITE_TRANSIENT);

    // Extended meta columns (params 45 .. 44+kExtMetaCount)
    for (size_t i = 0; i < kExtMetaCount; ++i) {
        int paramIdx = 45 + (int)i;
        auto it = record.meta.find(kExtMetaCols[i].col);
        if (it != record.meta.end() && !it->second.empty()) {
            sqlite3_bind_text(stmt, paramIdx, it->second.c_str(), -1, SQLITE_TRANSIENT);
        } else {
            sqlite3_bind_null(stmt, paramIdx);
        }
    }

    bool success = sqlite3_step(stmt) == SQLITE_DONE;
    if (!success) m_lastError = sqlite3_errmsg(m_db);

    sqlite3_finalize(stmt);
    // Every successful insert or upsert advances the revision observed by tabs.
    // Scan-time increments are absorbed while result polling is gated by isScanning.

    if (success) bumpDataRevision();
    return success;
}

bool Database::updateFile(const AudioRecord& record) {
    // updateFile delegates to the addFile upsert, which already advances the
    // data revision for every call site.

    return addFile(record);
}

namespace {

// MetadataEditPatch SQL builder
// Supports whitelisted text and integer columns from audio_files.
struct MetadataPatchSqlPlan {
    std::string sql;
    // Ordered list of fields to bind: {column_name, is_int}
    std::vector<std::pair<std::string, bool>> bindOrder;
    int idIdx = 0; // Parameter index for WHERE id = ?
};

bool BuildMetadataPatchUpdateSql(const MetadataEditPatch& patch, MetadataPatchSqlPlan& plan) {
    if (patch.isEmpty()) return false;
    const auto& textWhitelist = EditableTextColumns();
    const auto& intWhitelist = EditableIntColumns();

    plan.bindOrder.clear();
    std::string setClauses;

    // Rebuild search_data from the replacement values when any source field changes.
    std::string searchDescExpr = "IFNULL(description,'')";
    std::string searchKwExpr   = "IFNULL(keywords,'')";
    std::string searchCatExpr  = "IFNULL(category,'')";

    int paramIdx = 1;

    // Sort text fields so equivalent patches reuse the same SQL plan.
    std::vector<std::string> sortedTextKeys;
    sortedTextKeys.reserve(patch.fields.size());
    for (const auto& kv : patch.fields) {
        if (textWhitelist.count(kv.first)) sortedTextKeys.push_back(kv.first);
    }
    std::sort(sortedTextKeys.begin(), sortedTextKeys.end());

    for (const auto& col : sortedTextKeys) {
        if (!setClauses.empty()) setClauses += ", ";
        setClauses += col + " = ?" + std::to_string(paramIdx);
        plan.bindOrder.push_back({col, false});
        // Update search_data expressions for the big 3
        if (col == "description") searchDescExpr = "?" + std::to_string(paramIdx);
        else if (col == "keywords") searchKwExpr = "?" + std::to_string(paramIdx);
        else if (col == "category") searchCatExpr = "?" + std::to_string(paramIdx);
        paramIdx++;
    }

    // Int fields
    std::vector<std::string> sortedIntKeys;
    sortedIntKeys.reserve(patch.intFields.size());
    for (const auto& kv : patch.intFields) {
        if (intWhitelist.count(kv.first)) sortedIntKeys.push_back(kv.first);
    }
    std::sort(sortedIntKeys.begin(), sortedIntKeys.end());

    for (const auto& col : sortedIntKeys) {
        if (!setClauses.empty()) setClauses += ", ";
        setClauses += col + " = ?" + std::to_string(paramIdx);
        plan.bindOrder.push_back({col, true});
        paramIdx++;
    }

    if (setClauses.empty()) return false;

    // Build search_data expression including searchable extended fields
    std::string searchExtra;
    for (size_t i = 0; i < kExtMetaCount; ++i) {
        if (!kExtMetaCols[i].searchable) continue;
        searchExtra += " || ' ' || IFNULL(" + std::string(kExtMetaCols[i].col) + ",'')";
    }

    plan.sql = "UPDATE audio_files SET " + setClauses +
        ", search_data = aura_search_data(file_name || ' ' || IFNULL(title,'') || ' ' || " +
        searchDescExpr + " || ' ' || " + searchCatExpr + " || ' ' || " + searchKwExpr +
        searchExtra + ")";

    plan.idIdx = paramIdx;
    plan.sql += " WHERE id = ?" + std::to_string(paramIdx);

    return true;
}

// Generate a deterministic key for grouping patches with identical column sets
std::string MetadataPatchKey(const MetadataEditPatch& p) {
    std::string key;
    // sorted text keys
    std::vector<std::string> tk;
    for (const auto& kv : p.fields) tk.push_back(kv.first);
    std::sort(tk.begin(), tk.end());
    for (const auto& k : tk) { key += k; key += ','; }
    key += '|';
    // sorted int keys
    std::vector<std::string> ik;
    for (const auto& kv : p.intFields) ik.push_back(kv.first);
    std::sort(ik.begin(), ik.end());
    for (const auto& k : ik) { key += k; key += ','; }
    return key;
}

void BindMetadataPatchStmt(sqlite3_stmt* stmt, const MetadataPatchSqlPlan& plan, const MetadataEditPatch& patch,
                           int64_t fileId) {
    int idx = 1;
    for (const auto& entry : plan.bindOrder) {
        if (entry.second) {
            // int field
            auto it = patch.intFields.find(entry.first);
            int val = (it != patch.intFields.end()) ? it->second : 0;
            if (entry.first == "rating") val = clampRatingValue(val);
            sqlite3_bind_int(stmt, idx, val);
        } else {
            // text field
            auto it = patch.fields.find(entry.first);
            if (it != patch.fields.end()) {
                sqlite3_bind_text(stmt, idx, it->second.c_str(), -1, SQLITE_TRANSIENT);
            } else {
                sqlite3_bind_text(stmt, idx, "", -1, SQLITE_STATIC);
            }
        }
        idx++;
    }
    sqlite3_bind_int64(stmt, plan.idIdx, fileId);
}

sqlite3_stmt* PrepareAudioFileIdExistsStmt(sqlite3* db) {
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db,
            "SELECT 1 FROM audio_files WHERE id = ? LIMIT 1",
            -1, &stmt, nullptr) != SQLITE_OK) {
        return nullptr;
    }
    return stmt;
}

bool AudioFileIdExists(sqlite3_stmt* stmt, int64_t fileId) {
    if (!stmt) return false;
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);
    sqlite3_bind_int64(stmt, 1, fileId);
    return sqlite3_step(stmt) == SQLITE_ROW;
}

void LogMetadataPatchDiag(const char* detail) {
    DIAG_ACTION("Metadata", detail);
}

}  // namespace

BatchMetadataApplyResult Database::applyMetadataPatchBatch(const std::vector<int64_t>& fileIds,
                                                            const MetadataEditPatch& patch) {
    BatchMetadataApplyResult result;
    result.total = (int)fileIds.size();

    if (!m_db || fileIds.empty()) {
        if (!m_db && result.total > 0) {
            result.failed = result.total;
            result.errors.push_back("database not open");
            LogMetadataPatchDiag("PatchBatchRejected: reason=database_not_open");
        }
        return result;
    }

    if (patch.isEmpty()) {
        result.success = result.total;
        return result;
    }

    for (int64_t fileId : fileIds) {
        const AudioRecord rec = getFileById(fileId);
        if (!rec.filePath.empty() && Platform::FileSystem::IsNetworkPath(rec.filePath)) {
            result.failed = result.total;
            result.failedFileIds = fileIds;
            result.errors.push_back(L(Write_BlockedNetworkLibrary));
            m_lastError = L(Write_BlockedNetworkLibrary);
            return result;
        }
    }

    MetadataPatchSqlPlan plan;
    if (!BuildMetadataPatchUpdateSql(patch, plan)) {
        result.failed = result.total;
        result.failedFileIds = fileIds;
        result.errors.push_back("internal: empty patch after isEmpty check");
        LogMetadataPatchDiag("PatchBatchRejected: reason=empty_sql_plan");
        return result;
    }

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, plan.sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        m_lastError = sqlite3_errmsg(m_db);
        result.failed = result.total;
        result.failedFileIds = fileIds;
        result.errors.push_back("Failed to prepare stmt: " + m_lastError);
        DIAG_ACTION_FMT("Metadata",
                        "PatchBatchRejected: reason=prepare_failed total=%d",
                        result.total);
        return result;
    }

    sqlite3_stmt* existsStmt = PrepareAudioFileIdExistsStmt(m_db);
    if (!existsStmt) {
        m_lastError = sqlite3_errmsg(m_db);
        sqlite3_finalize(stmt);
        result.failed = result.total;
        result.failedFileIds = fileIds;
        result.errors.push_back("Failed to prepare id preflight: " + m_lastError);
        DIAG_ACTION_FMT("Metadata",
                        "PatchBatchRejected: reason=id_preflight_failed total=%d",
                        result.total);
        return result;
    }

    const bool needOwnTxn = (sqlite3_get_autocommit(m_db) != 0);
    if (needOwnTxn) {
        if (!beginTransaction()) {
            m_lastError = sqlite3_errmsg(m_db);
            sqlite3_finalize(stmt);
            sqlite3_finalize(existsStmt);
            result.failed = result.total;
            result.failedFileIds = fileIds;
            result.errors.push_back("failed to begin transaction: " +
                                    (m_lastError.empty() ? std::string("unknown") : m_lastError));
            DIAG_ACTION_FMT("Metadata",
                            "PatchBatchRejected: reason=begin_transaction_failed total=%d",
                            result.total);
            return result;
        }
    }

    bool allOk = true;

    for (int64_t fileId : fileIds) {
        if (fileId <= 0) {
            result.failed++;
            result.failedFileIds.push_back(fileId);
            result.errors.push_back("invalid file id: " + std::to_string(fileId));
            allOk = false;
            DIAG_ACTION_FMT("Metadata",
                            "PatchRejected: reason=invalid_file_id id=%lld",
                            (long long)fileId);
            continue;
        }
        if (!AudioFileIdExists(existsStmt, fileId)) {
            result.failed++;
            result.failedFileIds.push_back(fileId);
            result.errors.push_back("missing file id: " + std::to_string(fileId));
            allOk = false;
            DIAG_ACTION_FMT("Metadata",
                            "PatchRejected: reason=missing_file_id id=%lld",
                            (long long)fileId);
            continue;
        }

        sqlite3_clear_bindings(stmt);
        BindMetadataPatchStmt(stmt, plan, patch, fileId);

        if (sqlite3_step(stmt) == SQLITE_DONE) {
            result.success++;
        } else {
            result.failed++;
            result.failedFileIds.push_back(fileId);
            m_lastError = sqlite3_errmsg(m_db);
            result.errors.push_back("fileId=" + std::to_string(fileId) + ": " + m_lastError);
            allOk = false;
        }
        sqlite3_reset(stmt);
    }

    sqlite3_finalize(stmt);
    sqlite3_finalize(existsStmt);

    if (needOwnTxn) {
        if (allOk) {
            if (!commitTransaction()) {
                rollbackTransaction();
                m_lastError = sqlite3_errmsg(m_db);
                result.failed = result.total;
                result.success = 0;
                result.failedFileIds = fileIds;
                result.errors.clear();
                result.errors.push_back("failed to commit transaction: " +
                                        (m_lastError.empty() ? std::string() : m_lastError));
            } else if (result.success > 0) {
                invalidateFacetQueryCache();
                bumpDataRevision();  // T-295
            }
        } else {
            rollbackTransaction();
            result.success = 0;
            result.failed = result.total;
            result.failedFileIds = fileIds;
            result.errors.push_back("Transaction rolled back due to failures.");
        }
    } else if (result.success > 0) {
        bumpDataRevision();  // T-295
    }

    DIAG_ACTION_FMT("Metadata",
                    "PatchBatchComplete: total=%d success=%d failed=%d",
                    result.total,
                    result.success,
                    result.failed);

    return result;
}

BatchMetadataApplyResult Database::applyMetadataPatchMany(
    const std::vector<std::pair<int64_t, MetadataEditPatch>>& items) {
    BatchMetadataApplyResult result;
    result.total = (int)items.size();

    if (!m_db || items.empty()) {
        if (!m_db && result.total > 0) {
            result.failed = result.total;
            result.errors.push_back("database not open");
            LogMetadataPatchDiag("PatchManyRejected: reason=database_not_open");
        }
        return result;
    }

    int emptyOk = 0;
    std::map<std::string, std::vector<std::pair<int64_t, MetadataEditPatch>>> groups;
    for (const auto& entry : items) {
        const AudioRecord rec = getFileById(entry.first);
        if (!rec.filePath.empty() && Platform::FileSystem::IsNetworkPath(rec.filePath)) {
            result.failed = result.total;
            result.failedFileIds.push_back(entry.first);
            result.errors.push_back(L(Write_BlockedNetworkLibrary));
            m_lastError = L(Write_BlockedNetworkLibrary);
            return result;
        }
        const MetadataEditPatch& p = entry.second;
        if (p.isEmpty()) {
            emptyOk++;
            continue;
        }
        groups[MetadataPatchKey(p)].push_back(entry);
    }

    const bool needOwnTxn = (sqlite3_get_autocommit(m_db) != 0);
    if (needOwnTxn) {
        if (!beginTransaction()) {
            m_lastError = sqlite3_errmsg(m_db);
            result.failed = result.total;
            result.errors.push_back("failed to begin transaction: " +
                                    (m_lastError.empty() ? std::string("unknown") : m_lastError));
            return result;
        }
    }

    sqlite3_stmt* existsStmt = PrepareAudioFileIdExistsStmt(m_db);
    if (!existsStmt) {
        m_lastError = sqlite3_errmsg(m_db);
        if (needOwnTxn) rollbackTransaction();
        result.failed = result.total;
        result.success = 0;
        result.errors.push_back("Failed to prepare id preflight: " + m_lastError);
        return result;
    }

    bool allOk = true;
    int dbSuccess = 0;
    int dbFailed = 0;

    for (auto& g : groups) {
        std::vector<std::pair<int64_t, MetadataEditPatch>>& vec = g.second;
        if (vec.empty()) continue;

        MetadataPatchSqlPlan plan;
        if (!BuildMetadataPatchUpdateSql(vec[0].second, plan)) {
            allOk = false;
            for (const auto& pr : vec) {
                (void)pr;
                dbFailed++;
                result.failedFileIds.push_back(pr.first);
            }
            result.errors.push_back("internal: empty patch in group");
            LogMetadataPatchDiag("PatchManyRejected: reason=empty_sql_plan_group");
            continue;
        }

        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(m_db, plan.sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
            m_lastError = sqlite3_errmsg(m_db);
            allOk = false;
            for (const auto& pr : vec) {
                dbFailed++;
                result.failedFileIds.push_back(pr.first);
                result.errors.push_back("prepare failed fileId=" + std::to_string(pr.first));
            }
            result.errors.push_back("Failed to prepare stmt: " + m_lastError);
            DIAG_ACTION_FMT("Metadata",
                            "PatchManyRejected: reason=prepare_failed group=%zu",
                            vec.size());
            continue;
        }

        for (const auto& pr : vec) {
            const int64_t fileId = pr.first;
            const MetadataEditPatch& rowPatch = pr.second;

            if (fileId <= 0) {
                dbFailed++;
                result.failedFileIds.push_back(fileId);
                result.errors.push_back("invalid file id: " + std::to_string(fileId));
                allOk = false;
                DIAG_ACTION_FMT("Metadata",
                                "PatchRejected: reason=invalid_file_id id=%lld",
                                (long long)fileId);
                sqlite3_reset(stmt);
                continue;
            }
            if (!AudioFileIdExists(existsStmt, fileId)) {
                dbFailed++;
                result.failedFileIds.push_back(fileId);
                result.errors.push_back("missing file id: " + std::to_string(fileId));
                allOk = false;
                DIAG_ACTION_FMT("Metadata",
                                "PatchRejected: reason=missing_file_id id=%lld",
                                (long long)fileId);
                sqlite3_reset(stmt);
                continue;
            }

            sqlite3_clear_bindings(stmt);
            BindMetadataPatchStmt(stmt, plan, rowPatch, fileId);

            if (sqlite3_step(stmt) == SQLITE_DONE) {
                dbSuccess++;
            } else {
                dbFailed++;
                result.failedFileIds.push_back(fileId);
                m_lastError = sqlite3_errmsg(m_db);
                result.errors.push_back("fileId=" + std::to_string(fileId) + ": " + m_lastError);
                allOk = false;
            }
            sqlite3_reset(stmt);
        }

        sqlite3_finalize(stmt);
    }
    sqlite3_finalize(existsStmt);

    result.success = emptyOk + dbSuccess;
    result.failed = dbFailed;

    if (needOwnTxn) {
        if (allOk) {
            if (!commitTransaction()) {
                rollbackTransaction();
                m_lastError = sqlite3_errmsg(m_db);
                result.errors.push_back("failed to commit transaction: " +
                                        (m_lastError.empty() ? std::string() : m_lastError));
                result.success = emptyOk;
                result.failed = result.total - emptyOk;
            } else if (dbSuccess > 0) {
                invalidateFacetQueryCache();
                bumpDataRevision();  // T-295
            }
        } else {
            rollbackTransaction();
            result.success = emptyOk;
            result.failed = result.total - emptyOk;
            result.errors.push_back("Transaction rolled back due to failures.");
        }
    } else if (dbSuccess > 0) {
        // Caller-managed transaction: still bump so other tabs see the
        // edit when the outer txn commits (revision is monotonic, conservative
        // over-bumping just causes a refresh on tab switch which is cheap).
        bumpDataRevision();  // T-295
    }

    DIAG_ACTION_FMT("Metadata",
                    "PatchManyComplete: total=%d success=%d failed=%d",
                    result.total,
                    result.success,
                    result.failed);

    return result;
}

bool Database::applyMetadataPatch(int64_t fileId, const MetadataEditPatch& patch) {
    BatchMetadataApplyResult res = applyMetadataPatchBatch({fileId}, patch);
    return res.success == 1;
}

bool Database::updateMetadata(int64_t fileId, const std::string& description, 
                              const std::string& keywords, const std::string& category) {
    MetadataEditPatch patch;
    patch.setDescription(description);
    patch.setKeywords(keywords);
    patch.setCategory(category);
    return applyMetadataPatch(fileId, patch);
}

bool Database::updateDescription(int64_t fileId, const std::string& description) {
    MetadataEditPatch patch;
    patch.setDescription(description);
    return applyMetadataPatch(fileId, patch);
}

// Music analysis setters (F-033)

bool Database::setMusicKey(int64_t fileId, const std::string& key,
                            const std::string& camelot, float confidence,
                            MusicFieldSource source) {
    if (!m_db) return false;
    const char* sql =
        "UPDATE audio_files SET musical_key=?, musical_key_camelot=?,"
        " key_confidence=?, key_source=? WHERE id=?";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(stmt, 1, key.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, camelot.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_double(stmt, 3, confidence);
    sqlite3_bind_int(stmt, 4, (int)source);
    sqlite3_bind_int64(stmt, 5, fileId);
    bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

bool Database::setMusicBpm(int64_t fileId, float bpm, float confidence,
                            MusicFieldSource source) {
    if (!m_db) return false;
    const char* sql =
        "UPDATE audio_files SET bpm=?, bpm_confidence=?, bpm_source=? WHERE id=?";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_double(stmt, 1, bpm);
    sqlite3_bind_double(stmt, 2, confidence);
    sqlite3_bind_int(stmt, 3, (int)source);
    sqlite3_bind_int64(stmt, 4, fileId);
    bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

bool Database::setMusicMood(int64_t fileId, float valence, float arousal,
                             MusicFieldSource source) {
    if (!m_db) return false;
    const char* sql =
        "UPDATE audio_files SET mood_valence=?, mood_arousal=?, mood_source=? WHERE id=?";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_double(stmt, 1, valence);
    sqlite3_bind_double(stmt, 2, arousal);
    sqlite3_bind_int(stmt, 3, (int)source);
    sqlite3_bind_int64(stmt, 4, fileId);
    bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

// F-033 Music Mode: library type helpers
std::string Database::getLibraryType() const {
    if (!m_db) return "SFX";
    const char* sql = "SELECT value FROM app_meta WHERE key = 'library_type' LIMIT 1;";
    sqlite3_stmt* stmt = nullptr;
    std::string result = "SFX";
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            const unsigned char* text = sqlite3_column_text(stmt, 0);
            if (text) result = reinterpret_cast<const char*>(text);
        }
        sqlite3_finalize(stmt);
    }
    return result;
}

bool Database::setLibraryType(const std::string& type) {
    if (type != "SFX" && type != "MUSIC") return false;
    if (!m_db) return false;
    const char* sql =
        "INSERT INTO app_meta(key, value) VALUES('library_type', ?) "
        "ON CONFLICT(key) DO UPDATE SET value = excluded.value;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(stmt, 1, type.c_str(), -1, SQLITE_TRANSIENT);
    bool ok = (sqlite3_step(stmt) == SQLITE_DONE);
    sqlite3_finalize(stmt);
    return ok;
}

bool Database::removeFile(int64_t id) {
    if (!m_db) return false;

    const char* sql = "DELETE FROM audio_files WHERE id = ?";
    sqlite3_stmt* stmt;

    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        m_lastError = sqlite3_errmsg(m_db);
        return false;
    }

    sqlite3_bind_int64(stmt, 1, id);
    bool success = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    if (success) {
        pruneOrphanCoverCaches();
        bumpDataRevision();  // Invalidate tab snapshots.
    }
    return success;
}

bool Database::removeFileByPath(const std::string& filePath) {
    if (!m_db) return false;

    const char* sql = "DELETE FROM audio_files WHERE file_path = ?";
    sqlite3_stmt* stmt;

    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        m_lastError = sqlite3_errmsg(m_db);
        return false;
    }

    sqlite3_bind_text(stmt, 1, filePath.c_str(), -1, SQLITE_TRANSIENT);
    bool success = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    if (success) {
        pruneOrphanCoverCaches();
        bumpDataRevision();  // Invalidate tab snapshots.
    }
    return success;
}

bool Database::fileExists(const std::string& filePath) {
    if (!m_db) return false;

    const char* sql = "SELECT 1 FROM audio_files WHERE file_path = ? LIMIT 1";
    sqlite3_stmt* stmt;

    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }

    sqlite3_bind_text(stmt, 1, filePath.c_str(), -1, SQLITE_TRANSIENT);
    bool exists = sqlite3_step(stmt) == SQLITE_ROW;
    sqlite3_finalize(stmt);
    return exists;
}

int Database::cleanupMissingFiles(ScanProgressCallback callback, std::atomic<bool>* cancelFlag) {
    if (!m_db) return 0;

    const int total = (int)getFileCount();
    if (total <= 0) return 0;

    const char* selectSql = "SELECT id, file_path FROM audio_files";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, selectSql, -1, &stmt, nullptr) != SQLITE_OK) {
        m_lastError = sqlite3_errmsg(m_db);
        return 0;
    }

    std::vector<int64_t> idsToDelete;
    int checked = 0;
    const int reportInterval = 500;

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (cancelFlag && cancelFlag->load(std::memory_order_acquire))
            break;
        int64_t id = sqlite3_column_int64(stmt, 0);
        const char* pathText = (const char*)sqlite3_column_text(stmt, 1);
        if (pathText) {
            std::string filePath(pathText);
            bool missing = false;
            missing = Platform::FileSystem::ProbePath(filePath) ==
                Platform::FileSystem::PathStatus::Missing;
            if (missing)
                idsToDelete.push_back(id);
        }
        checked++;
        if (callback && (checked % reportInterval) == 0) {
            callback(checked, total,
                u8"正在清理无效记录... (" + std::to_string(checked) + u8"/" + std::to_string(total) + u8")",
                false);
        }
    }
    sqlite3_finalize(stmt);

    if (!idsToDelete.empty()) {
        beginTransaction();
        const char* deleteSql = "DELETE FROM audio_files WHERE id = ?";
        sqlite3_stmt* deleteStmt = nullptr;
        if (sqlite3_prepare_v2(m_db, deleteSql, -1, &deleteStmt, nullptr) == SQLITE_OK) {
            for (int64_t id : idsToDelete) {
                sqlite3_reset(deleteStmt);
                sqlite3_bind_int64(deleteStmt, 1, id);
                sqlite3_step(deleteStmt);
            }
            sqlite3_finalize(deleteStmt);
        }
        commitTransaction();
        pruneOrphanCoverCaches();
        invalidateFacetQueryCache();
        compactCachePackageBestEffort("cleanup_missing_files");
    }

    return (int)idsToDelete.size();
}

int Database::removeMissingFilesDatabaseWide(ScanProgressCallback callback, std::atomic<bool>* cancelFlag) {
    if (!m_db) return 0;
    int total = (int)getFileCount();
    if (total <= 0) return 0;

    const char* sql = "SELECT file_path FROM audio_files;";
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &st, nullptr) != SQLITE_OK) {
        m_lastError = sqlite3_errmsg(m_db);
        return 0;
    }

    std::vector<std::string> allPaths;
    allPaths.reserve((size_t)(std::max)(1, total));
    while (sqlite3_step(st) == SQLITE_ROW) {
        if (cancelFlag && cancelFlag->load(std::memory_order_acquire))
            break;
        std::string fp = safeGetText(st, 0);
        if (!fp.empty())
            allPaths.push_back(std::move(fp));
    }
    sqlite3_finalize(st);

    std::vector<std::string> missingPaths;
    int checked = 0;
    const int reportInterval = 500;
    for (const auto& fp : allPaths) {
        if (cancelFlag && cancelFlag->load(std::memory_order_acquire))
            break;
        const auto pathStatus = Platform::FileSystem::ProbePath(fp);
        if (pathStatus == Platform::FileSystem::PathStatus::Missing) {
            missingPaths.push_back(fp);
        } else if (pathStatus == Platform::FileSystem::PathStatus::Unreachable) {
            // NAS disconnect / expired credentials: keep the index row.
        }
        checked++;
        if (callback && (checked % reportInterval) == 0) {
            callback(checked, total, u8"正在清理全库缺失... (" + std::to_string(checked) + u8"/" + std::to_string(total) + u8")", false);
        }
    }

    int removed = 0;
    if (!missingPaths.empty() && (!cancelFlag || !cancelFlag->load(std::memory_order_acquire))) {
        if (beginTransaction()) {
            const char* delSql = "DELETE FROM audio_files WHERE file_path = ?";
            sqlite3_stmt* delSt = nullptr;
            if (sqlite3_prepare_v2(m_db, delSql, -1, &delSt, nullptr) == SQLITE_OK) {
                for (const auto& fp : missingPaths) {
                    sqlite3_reset(delSt);
                    sqlite3_clear_bindings(delSt);
                    sqlite3_bind_text(delSt, 1, fp.c_str(), -1, SQLITE_TRANSIENT);
                    if (sqlite3_step(delSt) == SQLITE_DONE)
                        removed++;
                }
                sqlite3_finalize(delSt);
            }
            commitTransaction();
            if (removed > 0) {
                pruneOrphanCoverCaches();
                invalidateFacetQueryCache();
                compactCachePackageBestEffort("remove_missing_files");
            }
        }
    }
    return removed;
}

// Map column names once so each row avoids O(N*M) string comparisons.
enum class ColId : uint8_t {
    Unknown = 0,
    Id, FilePath, FileName, Extension, FileSize, FolderPath, FileMtime, LastIndexed,
    SampleRate, BitDepth, Channels, TotalFrames, Duration, Format,
    Title, Artist, Album, Description, Category, Keywords, Rating,
    PlayCount, DateAdded, LastPlayed, HasCover,
    EmbeddedCoverKey, EmbeddedCoverKeyMtime, FolderCoverKey, FolderCoverSrcPath, FolderCoverSrcMtime, CoverSuppressed,
    MatchSnippet,
    // F-033 music-analysis fields live directly on AudioRecord rather than ExtMeta.
    MusicalKey, MusicalKeyCamelot, KeyConfidence, KeySource,
    Bpm, BpmConfidence, BpmSource,
    MoodValence, MoodArousal, MoodSource,
    // F-033 Music Mode extended fields
    AlbumArtist, TrackNumber, DiscNumber,
    EnergyTension, EnergyDrive,
    ReplayGainTrack, ReplayGainAlbum, HighlightStartMs,
    RawGenre, RawInstruments, NormGenre, NormInstruments, NormMoodTags, VocalType,
    ExtMeta  // Extended library metadata stored in AudioRecord::meta
};

struct ColMapEntry { ColId id; const char* name = nullptr; }; // name used only when id==ExtMeta (points into kExtMetaCols)

// Build the map on first use and rebuild only when the statement shape changes.
static std::vector<ColMapEntry> buildColumnMap(sqlite3_stmt* stmt, int colCount) {
    static const std::unordered_map<std::string, ColId> kMap = {
        {"id", ColId::Id}, {"file_path", ColId::FilePath}, {"file_name", ColId::FileName},
        {"extension", ColId::Extension}, {"file_size", ColId::FileSize}, {"folder_path", ColId::FolderPath},
        {"file_mtime", ColId::FileMtime}, {"last_indexed", ColId::LastIndexed},
        {"sample_rate", ColId::SampleRate}, {"bit_depth", ColId::BitDepth}, {"channels", ColId::Channels},
        {"total_frames", ColId::TotalFrames}, {"duration", ColId::Duration}, {"format", ColId::Format},
        {"title", ColId::Title}, {"artist", ColId::Artist}, {"album", ColId::Album},
        {"description", ColId::Description}, {"category", ColId::Category}, {"keywords", ColId::Keywords},
        {"rating", ColId::Rating}, {"play_count", ColId::PlayCount},
        {"date_added", ColId::DateAdded}, {"last_played", ColId::LastPlayed}, {"has_cover", ColId::HasCover},
        {"embedded_cover_key", ColId::EmbeddedCoverKey}, {"embedded_cover_key_mtime", ColId::EmbeddedCoverKeyMtime},
        {"folder_cover_key", ColId::FolderCoverKey}, {"folder_cover_src_path", ColId::FolderCoverSrcPath},
        {"folder_cover_src_mtime", ColId::FolderCoverSrcMtime}, {"cover_suppressed", ColId::CoverSuppressed},
        {"match_snippet", ColId::MatchSnippet},
// Music analysis direct fields (F-033).
        {"musical_key",         ColId::MusicalKey},
        {"musical_key_camelot", ColId::MusicalKeyCamelot},
        {"key_confidence",      ColId::KeyConfidence},
        {"key_source",          ColId::KeySource},
        {"bpm",                 ColId::Bpm},
        {"bpm_confidence",      ColId::BpmConfidence},
        {"bpm_source",          ColId::BpmSource},
        {"mood_valence",        ColId::MoodValence},
        {"mood_arousal",        ColId::MoodArousal},
        {"mood_source",         ColId::MoodSource},
        // F-033 Music Mode extended fields
        {"album_artist",        ColId::AlbumArtist},
        {"track_number",        ColId::TrackNumber},
        {"disc_number",         ColId::DiscNumber},
        {"energy_tension",      ColId::EnergyTension},
        {"energy_drive",        ColId::EnergyDrive},
        {"replay_gain_track",   ColId::ReplayGainTrack},
        {"replay_gain_album",   ColId::ReplayGainAlbum},
        {"highlight_start_ms",  ColId::HighlightStartMs},
        {"raw_genre",           ColId::RawGenre},
        {"raw_instruments",     ColId::RawInstruments},
        {"norm_genre",          ColId::NormGenre},
        {"norm_instruments",    ColId::NormInstruments},
        {"norm_mood_tags",      ColId::NormMoodTags},
        {"vocal_type",          ColId::VocalType},
    };
    // Map extended metadata names to stable string pointers.
    static const std::unordered_map<std::string, const char*> sExtMetaMap = []() {
        std::unordered_map<std::string, const char*> m;
        for (size_t i = 0; i < kExtMetaCount; ++i) m[kExtMetaCols[i].col] = kExtMetaCols[i].col;
        return m;
    }();

    std::vector<ColMapEntry> map((size_t)colCount);
    for (int i = 0; i < colCount; ++i) {
        const char* n = sqlite3_column_name(stmt, i);
        if (!n) { map[(size_t)i] = {ColId::Unknown}; continue; }
        auto it = kMap.find(n);
        if (it != kMap.end()) {
            map[(size_t)i] = {it->second};
        } else {
            auto eit = sExtMetaMap.find(n);
            if (eit != sExtMetaMap.end()) {
                map[(size_t)i] = {ColId::ExtMeta, eit->second};
            } else {
                map[(size_t)i] = {ColId::Unknown};
            }
        }
    }
    return map;
}

static AudioRecord recordFromStatement(sqlite3_stmt* stmt) {
    AudioRecord r;
    int colCount = sqlite3_column_count(stmt);

    // Reuse the thread-local map while the statement pointer and column count match.
    thread_local sqlite3_stmt* s_cachedStmt = nullptr;
    thread_local int s_cachedColCount = 0;
    thread_local std::vector<ColMapEntry> s_colMap;
    if (stmt != s_cachedStmt || colCount != s_cachedColCount) {
        s_colMap = buildColumnMap(stmt, colCount);
        s_cachedStmt = stmt;
        s_cachedColCount = colCount;
    }

    for (int i = 0; i < colCount; ++i) {
        switch (s_colMap[(size_t)i].id) {
        case ColId::Id:                     r.id = sqlite3_column_int64(stmt, i); break;
        case ColId::FilePath:               r.filePath = safeGetText(stmt, i); break;
        case ColId::FileName:               r.fileName = safeGetText(stmt, i); r.displayName = r.fileName; break;
        case ColId::Extension:              r.extension = safeGetText(stmt, i); break;
        case ColId::FileSize:               r.fileSize = (uint64_t)sqlite3_column_int64(stmt, i); break;
        case ColId::FolderPath:             r.folderPath = safeGetText(stmt, i); break;
        case ColId::FileMtime:              r.fileMtime = sqlite3_column_int64(stmt, i); break;
        case ColId::LastIndexed:            r.lastIndexed = sqlite3_column_int64(stmt, i); break;
        case ColId::SampleRate:             r.sampleRate = (uint32_t)sqlite3_column_int(stmt, i); break;
        case ColId::BitDepth:               r.bitDepth = (uint32_t)sqlite3_column_int(stmt, i); break;
        case ColId::Channels:               r.channels = (uint32_t)sqlite3_column_int(stmt, i); break;
        case ColId::TotalFrames:            r.totalFrames = (uint64_t)sqlite3_column_int64(stmt, i); break;
        case ColId::Duration:               r.durationSeconds = (float)sqlite3_column_double(stmt, i); break;
        case ColId::Format:                 r.format = safeGetText(stmt, i); break;
        case ColId::Title:                  r.title = safeGetText(stmt, i); break;
        case ColId::Artist:                 r.artist = safeGetText(stmt, i); break;
        case ColId::Album:                  r.album = safeGetText(stmt, i); break;
        case ColId::Description:            r.description = safeGetText(stmt, i); break;
        case ColId::Category:               r.category = safeGetText(stmt, i); break;
        case ColId::Keywords:               r.keywords = safeGetText(stmt, i); break;
        case ColId::Rating:                 r.rating = sqlite3_column_int(stmt, i); break;
        case ColId::PlayCount:              r.playCount = sqlite3_column_int(stmt, i); break;
        case ColId::DateAdded:              r.dateAdded = sqlite3_column_int64(stmt, i); break;
        case ColId::LastPlayed:             r.lastPlayed = sqlite3_column_int64(stmt, i); break;
        case ColId::HasCover:               r.hasCover = sqlite3_column_int(stmt, i) != 0; break;
        case ColId::EmbeddedCoverKey:       r.embeddedCoverKey = safeGetText(stmt, i); break;
        case ColId::EmbeddedCoverKeyMtime:  r.embeddedCoverKeyMtime = sqlite3_column_int64(stmt, i); break;
        case ColId::FolderCoverKey:         r.folderCoverKey = safeGetText(stmt, i); break;
        case ColId::FolderCoverSrcPath:     r.folderCoverSrcPath = safeGetText(stmt, i); break;
        case ColId::FolderCoverSrcMtime:    r.folderCoverSrcMtime = sqlite3_column_int64(stmt, i); break;
        case ColId::CoverSuppressed:        r.coverSuppressed = sqlite3_column_int(stmt, i) != 0; break;
        case ColId::MatchSnippet:           r.matchSnippet = safeGetText(stmt, i); break;

        case ColId::MusicalKey: {
            r.musicalKey = safeGetText(stmt, i);
            // Also populate meta for backward-compat callers using getMeta("musical_key")
            if (!r.musicalKey.empty()) r.meta["musical_key"] = r.musicalKey;
            break;
        }
        case ColId::MusicalKeyCamelot:
            r.musicalKeyCamelot = safeGetText(stmt, i); break;
        case ColId::KeyConfidence:
            r.keyConfidence = (float)sqlite3_column_double(stmt, i); break;
        case ColId::KeySource:
            r.keySource = sqlite3_column_int(stmt, i); break;
        case ColId::Bpm: {
            r.bpm = (float)sqlite3_column_double(stmt, i);
            // Also populate meta for backward-compat callers using getMeta("bpm")
            if (r.bpm > 0.0f) {
                char buf[32];
                snprintf(buf, sizeof(buf), "%.2f", (double)r.bpm);
                r.meta["bpm"] = buf;
            }
            break;
        }
        case ColId::BpmConfidence:
            r.bpmConfidence = (float)sqlite3_column_double(stmt, i); break;
        case ColId::BpmSource:
            r.bpmSource = sqlite3_column_int(stmt, i); break;
        case ColId::MoodValence:
            r.moodValence = (float)sqlite3_column_double(stmt, i); break;
        case ColId::MoodArousal:
            r.moodArousal = (float)sqlite3_column_double(stmt, i); break;
        case ColId::MoodSource:
            r.moodSource = sqlite3_column_int(stmt, i); break;
        // F-033 Music Mode extended fields
        case ColId::AlbumArtist:
            r.albumArtist = safeGetText(stmt, i); break;
        case ColId::TrackNumber:
            r.trackNumber = sqlite3_column_int(stmt, i); break;
        case ColId::DiscNumber:
            r.discNumber = sqlite3_column_int(stmt, i); break;
        case ColId::EnergyTension:
            r.energyTension = (float)sqlite3_column_double(stmt, i); break;
        case ColId::EnergyDrive:
            r.energyDrive = (float)sqlite3_column_double(stmt, i); break;
        case ColId::ReplayGainTrack:
            r.replayGainTrack = (float)sqlite3_column_double(stmt, i); break;
        case ColId::ReplayGainAlbum:
            r.replayGainAlbum = (float)sqlite3_column_double(stmt, i); break;
        case ColId::HighlightStartMs:
            r.highlightStartMs = sqlite3_column_int(stmt, i); break;
        case ColId::RawGenre:
            r.rawGenre = safeGetText(stmt, i); break;
        case ColId::RawInstruments:
            r.rawInstruments = safeGetText(stmt, i); break;
        case ColId::NormGenre:
            r.normGenre = safeGetText(stmt, i); break;
        case ColId::NormInstruments:
            r.normInstruments = safeGetText(stmt, i); break;
        case ColId::NormMoodTags:
            r.normMoodTags = safeGetText(stmt, i); break;
        case ColId::VocalType:
            r.vocalType = safeGetText(stmt, i); break;
        case ColId::ExtMeta: {
            const char* v = (const char*)sqlite3_column_text(stmt, i);
            if (v && v[0]) r.meta[s_colMap[(size_t)i].name] = v;
            break;
        }
        case ColId::Unknown: break;
        }
    }
    return r;
}

static bool FindAsciiCaseInsensitiveSpanForMatch(const std::string& text,
                                                 const std::string& query,
                                                 size_t& outStart) {
    if (text.empty() || query.empty() || query.size() > text.size()) return false;
    auto lowerAscii = [](unsigned char c) -> unsigned char {
        return (c >= 'A' && c <= 'Z') ? static_cast<unsigned char>(c - 'A' + 'a') : c;
    };
    const size_t lastStart = text.size() - query.size();
    for (size_t i = 0; i <= lastStart; ++i) {
        bool matched = true;
        for (size_t j = 0; j < query.size(); ++j) {
            const unsigned char textByte = static_cast<unsigned char>(text[i + j]);
            const unsigned char queryByte = static_cast<unsigned char>(query[j]);
            if (textByte >= 0x80 || queryByte >= 0x80 || lowerAscii(textByte) != lowerAscii(queryByte)) {
                matched = false;
                break;
            }
        }
        if (matched) {
            outStart = i;
            return true;
        }
    }
    return false;
}

static bool ContainsFieldTerm(const std::string& field, const std::string& term) {
    if (field.empty() || term.empty()) return false;
    if (field.find(term) != std::string::npos) return true;
    size_t ignored = 0;
    return FindAsciiCaseInsensitiveSpanForMatch(field, term, ignored);
}

static bool ContainsAnyFieldTerm(const std::string& field, const std::vector<std::string>& terms) {
    for (const auto& term : terms) {
        if (ContainsFieldTerm(field, term)) return true;
    }
    return false;
}

static std::vector<std::string> CollectSnippetHighlightTerms(const std::string& snippet) {
    constexpr char kStart = (char)1;
    constexpr char kEnd = (char)2;
    std::vector<std::string> terms;
    size_t i = 0;
    while (i < snippet.size()) {
        if (snippet[i] != kStart) {
            ++i;
            continue;
        }
        const size_t begin = ++i;
        while (i < snippet.size() && snippet[i] != kEnd) ++i;
        if (i > begin) {
            terms.push_back(snippet.substr(begin, i - begin));
        }
        if (i < snippet.size() && snippet[i] == kEnd) ++i;
    }
    return terms;
}

static SearchMatchSource DetectSnippetMatchSource(const AudioRecord& record,
                                                  const std::vector<std::string>& terms) {
    const std::string& display = !record.displayName.empty() ? record.displayName : record.fileName;
    if (ContainsAnyFieldTerm(display, terms) || ContainsAnyFieldTerm(record.fileName, terms)) {
        return SearchMatchSource::Name;
    }
    if (ContainsAnyFieldTerm(record.filePath, terms) || ContainsAnyFieldTerm(record.folderPath, terms)) {
        return SearchMatchSource::Path;
    }
    if (ContainsAnyFieldTerm(record.description, terms)) {
        return SearchMatchSource::Description;
    }
    if (ContainsAnyFieldTerm(record.keywords, terms)) {
        return SearchMatchSource::Keywords;
    }
    if (ContainsAnyFieldTerm(record.title, terms) ||
        ContainsAnyFieldTerm(record.artist, terms) ||
        ContainsAnyFieldTerm(record.album, terms) ||
        ContainsAnyFieldTerm(record.category, terms)) {
        return SearchMatchSource::Metadata;
    }
    return SearchMatchSource::None;
}

static SearchMatchSource DeriveSearchMatchSource(const AudioRecord& record,
                                                 const SearchFilter& filter,
                                                 const std::string& normalizedKeyword,
                                                 const SearchQueryResult& query,
                                                 bool hasPowerFieldMetadata) {
    if (!record.matchSnippet.empty()) {
        const auto terms = CollectSnippetHighlightTerms(record.matchSnippet);
        const SearchMatchSource fromSnippet = DetectSnippetMatchSource(record, terms);
        if (fromSnippet != SearchMatchSource::None) return fromSnippet;
    }

    if (!query.pathContains.empty() &&
        (ContainsAnyFieldTerm(record.filePath, query.pathContains) ||
         ContainsAnyFieldTerm(record.folderPath, query.pathContains))) {
        return SearchMatchSource::Path;
    }

    const std::string& display = !record.displayName.empty() ? record.displayName : record.fileName;
    if (!query.nameContains.empty() &&
        (ContainsAnyFieldTerm(display, query.nameContains) ||
         ContainsAnyFieldTerm(record.fileName, query.nameContains))) {
        return filter.exactMode ? SearchMatchSource::Name : SearchMatchSource::FuzzyName;
    }

    if (!normalizedKeyword.empty()) {
        if (ContainsFieldTerm(display, normalizedKeyword) || ContainsFieldTerm(record.fileName, normalizedKeyword)) {
            return SearchMatchSource::Name;
        }
        if (ContainsFieldTerm(record.description, normalizedKeyword)) {
            return SearchMatchSource::Description;
        }
        if (ContainsFieldTerm(record.keywords, normalizedKeyword)) {
            return SearchMatchSource::Keywords;
        }
        if (ContainsFieldTerm(record.filePath, normalizedKeyword) || ContainsFieldTerm(record.folderPath, normalizedKeyword)) {
            return SearchMatchSource::Path;
        }
    }

    if (!filter.category.empty() ||
        !filter.artist.empty() ||
        !filter.artists.empty() ||
        !filter.album.empty() ||
        !filter.format.empty() ||
        !filter.musicalKey.empty() ||
        !filter.includeGenres.empty() ||
        !filter.excludeGenres.empty() ||
        !filter.includeInstruments.empty() ||
        !filter.excludeInstruments.empty() ||
        hasPowerFieldMetadata) {
        return SearchMatchSource::Metadata;
    }

    return SearchMatchSource::None;
}

bool Database::updateCoverKeyCacheBatch(const std::vector<CoverKeyCacheUpdate>& updates) {
    if (!m_db) return false;
    if (updates.empty()) return true;

    const char* sqlBoth = R"(
        UPDATE audio_files
        SET embedded_cover_key = ?,
            embedded_cover_key_mtime = ?,
            folder_cover_key = ?,
            folder_cover_src_path = ?,
            folder_cover_src_mtime = ?
        WHERE file_path = ?;
    )";
    const char* sqlEmb = R"(
        UPDATE audio_files
        SET embedded_cover_key = ?,
            embedded_cover_key_mtime = ?
        WHERE file_path = ?;
    )";
    const char* sqlFolder = R"(
        UPDATE audio_files
        SET folder_cover_key = ?,
            folder_cover_src_path = ?,
            folder_cover_src_mtime = ?
        WHERE file_path = ?;
    )";

    sqlite3_stmt* stBoth = nullptr;
    sqlite3_stmt* stEmb = nullptr;
    sqlite3_stmt* stFolder = nullptr;
    if (sqlite3_prepare_v2(m_db, sqlBoth, -1, &stBoth, nullptr) != SQLITE_OK ||
        sqlite3_prepare_v2(m_db, sqlEmb, -1, &stEmb, nullptr) != SQLITE_OK ||
        sqlite3_prepare_v2(m_db, sqlFolder, -1, &stFolder, nullptr) != SQLITE_OK) {
        m_lastError = sqlite3_errmsg(m_db);
        if (stBoth) sqlite3_finalize(stBoth);
        if (stEmb) sqlite3_finalize(stEmb);
        if (stFolder) sqlite3_finalize(stFolder);
        return false;
    }

    bool ok = beginTransaction();
    if (!ok) {
        sqlite3_finalize(stBoth);
        sqlite3_finalize(stEmb);
        sqlite3_finalize(stFolder);
        return false;
    }

    for (const auto& u : updates) {
        if (u.filePath.empty()) continue;

        if (u.updateEmbedded && u.updateFolder) {
            sqlite3_reset(stBoth);
            sqlite3_clear_bindings(stBoth);
            sqlite3_bind_text(stBoth, 1, u.embeddedCoverKey.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(stBoth, 2, u.embeddedCoverKeyMtime);
            sqlite3_bind_text(stBoth, 3, u.folderCoverKey.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stBoth, 4, u.folderCoverSrcPath.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(stBoth, 5, u.folderCoverSrcMtime);
            sqlite3_bind_text(stBoth, 6, u.filePath.c_str(), -1, SQLITE_TRANSIENT);
            if (sqlite3_step(stBoth) != SQLITE_DONE) {
                m_lastError = sqlite3_errmsg(m_db);
                rollbackTransaction();
                sqlite3_finalize(stBoth);
                sqlite3_finalize(stEmb);
                sqlite3_finalize(stFolder);
                return false;
            }
        } else if (u.updateEmbedded) {
            sqlite3_reset(stEmb);
            sqlite3_clear_bindings(stEmb);
            sqlite3_bind_text(stEmb, 1, u.embeddedCoverKey.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(stEmb, 2, u.embeddedCoverKeyMtime);
            sqlite3_bind_text(stEmb, 3, u.filePath.c_str(), -1, SQLITE_TRANSIENT);
            if (sqlite3_step(stEmb) != SQLITE_DONE) {
                m_lastError = sqlite3_errmsg(m_db);
                rollbackTransaction();
                sqlite3_finalize(stBoth);
                sqlite3_finalize(stEmb);
                sqlite3_finalize(stFolder);
                return false;
            }
        } else if (u.updateFolder) {
            sqlite3_reset(stFolder);
            sqlite3_clear_bindings(stFolder);
            sqlite3_bind_text(stFolder, 1, u.folderCoverKey.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stFolder, 2, u.folderCoverSrcPath.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(stFolder, 3, u.folderCoverSrcMtime);
            sqlite3_bind_text(stFolder, 4, u.filePath.c_str(), -1, SQLITE_TRANSIENT);
            if (sqlite3_step(stFolder) != SQLITE_DONE) {
                m_lastError = sqlite3_errmsg(m_db);
                rollbackTransaction();
                sqlite3_finalize(stBoth);
                sqlite3_finalize(stEmb);
                sqlite3_finalize(stFolder);
                return false;
            }
        }
    }

    sqlite3_finalize(stBoth);
    sqlite3_finalize(stEmb);
    sqlite3_finalize(stFolder);
    bool committed = commitTransaction();
    if (committed) {
        std::vector<std::string> changedPaths;
        changedPaths.reserve(updates.size());
        for (const auto& u : updates) {
            if (!u.filePath.empty()) changedPaths.push_back(u.filePath);
        }
        invalidateFacetQueryCache();
        invalidateFolderCoverFacetIndexForPaths(changedPaths);
    }
    return committed;
}

bool Database::updateHasCoverByPathsBatch(const std::vector<std::string>& filePaths, bool hasCover) {
    if (!m_db) return false;
    if (filePaths.empty()) return true;

    const char* sql = "UPDATE audio_files SET has_cover = ? WHERE file_path = ?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        m_lastError = sqlite3_errmsg(m_db);
        return false;
    }

    if (!beginTransaction()) {
        sqlite3_finalize(stmt);
        return false;
    }

    int v = hasCover ? 1 : 0;
    for (const auto& p : filePaths) {
        if (p.empty()) continue;
        sqlite3_reset(stmt);
        sqlite3_clear_bindings(stmt);
        sqlite3_bind_int(stmt, 1, v);
        sqlite3_bind_text(stmt, 2, p.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(stmt) != SQLITE_DONE) {
            m_lastError = sqlite3_errmsg(m_db);
            rollbackTransaction();
            sqlite3_finalize(stmt);
            return false;
        }
    }

    sqlite3_finalize(stmt);
    bool committed = commitTransaction();
    if (committed) {
        invalidateFacetQueryCache();
        invalidateFolderCoverFacetIndexForPaths(filePaths);
    }
    return committed;
}

bool Database::assignEmbeddedCoverByPaths(const std::vector<std::string>& filePaths,
                                          const std::string& embeddedCoverKey,
                                          int64_t embeddedCoverKeyMtime) {
    if (!m_db) return false;
    if (filePaths.empty()) return true;

    const char* sql = R"(
        UPDATE audio_files
        SET has_cover = 1,
            embedded_cover_key = ?,
            embedded_cover_key_mtime = ?,
            folder_cover_key = '',
            folder_cover_src_path = '',
            folder_cover_src_mtime = 0,
            cover_suppressed = 0
        WHERE file_path = ?;
    )";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        m_lastError = sqlite3_errmsg(m_db);
        return false;
    }
    if (!beginTransaction()) {
        sqlite3_finalize(stmt);
        return false;
    }
    for (const auto& p : filePaths) {
        if (p.empty()) continue;
        sqlite3_reset(stmt);
        sqlite3_clear_bindings(stmt);
        sqlite3_bind_text(stmt, 1, embeddedCoverKey.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 2, embeddedCoverKeyMtime);
        sqlite3_bind_text(stmt, 3, p.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(stmt) != SQLITE_DONE) {
            m_lastError = sqlite3_errmsg(m_db);
            rollbackTransaction();
            sqlite3_finalize(stmt);
            return false;
        }
    }
    sqlite3_finalize(stmt);
    bool committed = commitTransaction();
    if (committed) {
        pruneOrphanCoverCaches();
        invalidateFacetQueryCache();
        invalidateFolderCoverFacetIndexForPaths(filePaths);
    }
    return committed;
}

bool Database::clearCoverByPaths(const std::vector<std::string>& filePaths) {
    if (!m_db) return false;
    if (filePaths.empty()) return true;

    const char* sql = R"(
        UPDATE audio_files
        SET has_cover = 0,
            embedded_cover_key = '',
            embedded_cover_key_mtime = 0,
            folder_cover_key = '',
            folder_cover_src_path = '',
            folder_cover_src_mtime = 0,
            cover_suppressed = 1
        WHERE file_path = ?;
    )";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        m_lastError = sqlite3_errmsg(m_db);
        return false;
    }
    if (!beginTransaction()) {
        sqlite3_finalize(stmt);
        return false;
    }
    for (const auto& p : filePaths) {
        if (p.empty()) continue;
        sqlite3_reset(stmt);
        sqlite3_clear_bindings(stmt);
        sqlite3_bind_text(stmt, 1, p.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(stmt) != SQLITE_DONE) {
            m_lastError = sqlite3_errmsg(m_db);
            rollbackTransaction();
            sqlite3_finalize(stmt);
            return false;
        }
    }
    sqlite3_finalize(stmt);
    bool committed = commitTransaction();
    if (committed) {
        pruneOrphanCoverCaches();
        invalidateFacetQueryCache();
        invalidateFolderCoverFacetIndexForPaths(filePaths);
    }
    return committed;
}

AudioRecord Database::getFileByPath(const std::string& filePath) {
    AudioRecord r;
    if (!m_db) return r;

    std::string sql = std::string("SELECT ") + kRecordColumns + " FROM audio_files WHERE file_path = ?";
    
    // High-frequency single-row reads use the prepared-statement pool.
    sqlite3_stmt* stmt = checkoutStmt(sql);
    if (!stmt) return r;

    sqlite3_bind_text(stmt, 1, filePath.c_str(), -1, SQLITE_TRANSIENT);

    if (sqlite3_step(stmt) == SQLITE_ROW) {
        r = recordFromStatement(stmt);
    }

    returnStmt(sql, stmt);
    return r;
}

AudioRecord Database::getFileById(int64_t id) {
    AudioRecord r;
    if (!m_db) return r;

    std::string sql = std::string("SELECT ") + kRecordColumns + " FROM audio_files WHERE id = ?";
    sqlite3_stmt* stmt = checkoutStmt(sql);
    if (!stmt) return r;

    sqlite3_bind_int64(stmt, 1, id);
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        r = recordFromStatement(stmt);
    }
    returnStmt(sql, stmt);
    return r;
}

// Creates the exclusive upper bound for a folder-prefix range.
// Appending 0xFF covers valid UTF-8 suffixes without incrementing a multibyte code unit.
static std::string MakeFolderPathUpperBound(const std::string& folderPath) {
    if (folderPath.empty()) return folderPath;
    // 0xFF sorts after every valid UTF-8 byte sequence under SQLite BINARY collation.
    // The half-open range includes every descendant path.
    // Incrementing the final byte would corrupt multibyte UTF-8 paths.
    std::string upper = folderPath + '\xff';
    return upper;
}

static std::string MakeFolderChildPrefix(const std::string& folderPath) {
    if (folderPath.empty()) return folderPath;
    std::string prefix = folderPath;
    if (prefix.back() != '\\' && prefix.back() != '/') {
#ifdef _WIN32
        prefix.push_back('\\');
#else
        prefix.push_back('/');
#endif
    }
    return prefix;
}

static bool StartsWithAsciiCaseInsensitiveDb(const std::string& s, const char* prefix) {
    const size_t n = std::strlen(prefix);
    if (s.size() < n) return false;
    for (size_t i = 0; i < n; ++i) {
        unsigned char a = static_cast<unsigned char>(s[i]);
        unsigned char b = static_cast<unsigned char>(prefix[i]);
        if (a >= 'A' && a <= 'Z') a = static_cast<unsigned char>(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = static_cast<unsigned char>(b - 'A' + 'a');
        if (a != b) return false;
    }
    return true;
}

static std::string NormalizeFolderPathPreserveCaseForDbMatch(std::string_view rawPath) {
    std::string s(rawPath);
#ifdef _WIN32
    if (StartsWithAsciiCaseInsensitiveDb(s, "\\\\?\\UNC\\")) {
        s = std::string("\\\\") + s.substr(8);
    } else if (StartsWithAsciiCaseInsensitiveDb(s, "\\\\?\\")) {
        s = s.substr(4);
    }
    std::replace(s.begin(), s.end(), '/', '\\');
    while (s.size() > 3 && (s.back() == '\\' || s.back() == '/')) {
        s.pop_back();
    }
#else
    std::replace(s.begin(), s.end(), '\\', '/');
    while (s.size() > 1 && s.back() == '/') {
        s.pop_back();
    }
#endif
    return s;
}

static void AddUniqueFolderMatchPath(std::vector<std::string>& paths, std::string path) {
    if (path.empty()) return;
    if (std::find(paths.begin(), paths.end(), path) == paths.end()) {
        paths.push_back(std::move(path));
    }
}

static std::vector<std::string> BuildFolderSubtreeMatchPaths(const std::string& canonicalPath,
                                                             const std::string& rawPath) {
    std::vector<std::string> paths;
    paths.reserve(4);
    AddUniqueFolderMatchPath(paths, canonicalPath);
    AddUniqueFolderMatchPath(paths, rawPath);
#ifdef _WIN32
    if (rawPath.size() >= 2 && rawPath[1] == ':') {
        std::string driveUpper = rawPath;
        driveUpper[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(driveUpper[0])));
        AddUniqueFolderMatchPath(paths, std::move(driveUpper));

        std::string driveLower = rawPath;
        driveLower[0] = static_cast<char>(std::tolower(static_cast<unsigned char>(driveLower[0])));
        AddUniqueFolderMatchPath(paths, std::move(driveLower));
    }
#endif
    return paths;
}

// ---------------------------------------------------------------------------
// Shared WHERE-clause builder for searchFiles and searchFilesCount.
// ---------------------------------------------------------------------------
struct NormalizedPowerFieldFilters {
    std::string keyword;
    std::vector<std::string> includeGenres;
    std::vector<std::string> excludeGenres;
    std::vector<std::string> includeInstruments;
    std::vector<std::string> excludeInstruments;
};

static bool IsGenrePowerField(const std::string& field) {
    return field == "genre" || field == "genres";
}

static bool IsInstrumentPowerField(const std::string& field) {
    return field == "instrument" || field == "instruments";
}

static bool IsSearchOperatorOnly(const std::string& text) {
    if (text.empty()) return true;
    auto chips = BuildSearchQueryChips(text);
    if (chips.empty()) return true;
    for (const auto& chip : chips) {
        if (chip.kind != SearchQueryChipKind::Operator) return false;
    }
    return true;
}

static bool HasStandaloneOrOperator(const std::vector<SearchQueryChip>& chips) {
    for (const auto& chip : chips) {
        if (chip.kind != SearchQueryChipKind::Operator) continue;
        if (toLowerAsciiCopy(chip.label) == "or") return true;
    }
    return false;
}

static std::string TrimAsciiWhitespaceCopy(std::string text) {
    size_t start = 0;
    while (start < text.size() && std::isspace(static_cast<unsigned char>(text[start]))) ++start;
    size_t end = text.size();
    while (end > start && std::isspace(static_cast<unsigned char>(text[end - 1]))) --end;
    return text.substr(start, end - start);
}

static std::string EscapeLikePatternLiteral(const std::string& value) {
    std::string escaped;
    escaped.reserve(value.size());
    for (char c : value) {
        if (c == '\\' || c == '%' || c == '_') escaped.push_back('\\');
        escaped.push_back(c);
    }
    return escaped;
}

static std::vector<std::string> CollectExplainableKeywordTerms(const std::string& keyword) {
    std::vector<std::string> terms;
    auto chips = BuildSearchQueryChips(keyword);
    if (HasStandaloneOrOperator(chips)) return terms;

    auto pushUnique = [&](std::string term) {
        term = TrimAsciiWhitespaceCopy(std::move(term));
        if (term.empty()) return;
        const std::string lower = toLowerAsciiCopy(term);
        if (lower == "and" || lower == "or" || lower == "not") return;
        for (const auto& existing : terms) {
            if (toLowerAsciiCopy(existing) == lower) return;
        }
        terms.push_back(std::move(term));
    };

    for (const auto& chip : chips) {
        if (chip.kind != SearchQueryChipKind::Keyword) continue;
        pushUnique(chip.label);
    }
    return terms;
}

static NormalizedPowerFieldFilters ExtractNormalizedPowerFieldFilters(const std::string& keyword) {
    NormalizedPowerFieldFilters out;
    out.keyword = keyword;

    auto chips = BuildSearchQueryChips(keyword);
    if (HasStandaloneOrOperator(chips)) return out;

    for (const auto& chip : chips) {
        if (chip.label.empty()) continue;
        std::vector<std::string>* target = nullptr;
        if (IsGenrePowerField(chip.field)) {
            target = (chip.kind == SearchQueryChipKind::ExcludeField)
                ? &out.excludeGenres
                : &out.includeGenres;
        } else if (IsInstrumentPowerField(chip.field)) {
            target = (chip.kind == SearchQueryChipKind::ExcludeField)
                ? &out.excludeInstruments
                : &out.includeInstruments;
        }
        if (!target) continue;
        target->push_back(chip.label);
        out.keyword = RemoveSearchQueryChipFromText(out.keyword, chip);
    }

    if (IsSearchOperatorOnly(out.keyword)) out.keyword.clear();
    return out;
}

static std::string StripWrappingPowerFieldParentheses(std::string expression) {
    expression = TrimAsciiWhitespaceCopy(std::move(expression));
    while (expression.size() >= 2 && expression.front() == '(' && expression.back() == ')') {
        int nesting = 0;
        bool inQuote = false;
        bool wrapsWholeExpression = true;
        for (size_t i = 0; i < expression.size(); ++i) {
            if (expression[i] == '"') { inQuote = !inQuote; continue; }
            if (inQuote) continue;
            if (expression[i] == '(') ++nesting;
            if (expression[i] == ')' && --nesting == 0 && i + 1 != expression.size()) {
                wrapsWholeExpression = false;
                break;
            }
        }
        if (!wrapsWholeExpression || nesting != 0 || inQuote) break;
        expression = TrimAsciiWhitespaceCopy(expression.substr(1, expression.size() - 2));
    }
    return expression;
}

static std::vector<std::string> SplitTopLevelPowerFieldOr(const std::string& keyword) {
    std::string expression = StripWrappingPowerFieldParentheses(keyword);
    const auto chips = BuildSearchQueryChips(expression);
    const bool hasPowerField = std::any_of(chips.begin(), chips.end(), [](const SearchQueryChip& chip) {
        return IsGenrePowerField(chip.field) || IsInstrumentPowerField(chip.field);
    });
    if (!hasPowerField) return {};

    std::vector<std::string> arms;
    size_t start = 0;
    int depth = 0;
    bool quoted = false;
    for (size_t i = 0; i < expression.size(); ++i) {
        const char c = expression[i];
        if (c == '"') { quoted = !quoted; continue; }
        if (quoted) continue;
        if (c == '(') { ++depth; continue; }
        if (c == ')') { --depth; continue; }
        if (depth != 0 || i + 2 > expression.size() ||
            toLowerAsciiCopy(expression.substr(i, 2)) != "or") continue;
        const bool leftBoundary = i == 0 || std::isspace(static_cast<unsigned char>(expression[i - 1]));
        const bool rightBoundary = i + 2 == expression.size() ||
            std::isspace(static_cast<unsigned char>(expression[i + 2]));
        if (!leftBoundary || !rightBoundary) continue;
        arms.push_back(StripWrappingPowerFieldParentheses(expression.substr(start, i - start)));
        start = i + 2;
        ++i;
    }
    if (arms.empty()) return {};
    arms.push_back(StripWrappingPowerFieldParentheses(expression.substr(start)));
    if (std::any_of(arms.begin(), arms.end(), [](const std::string& arm) { return arm.empty(); })) return {};
    return arms;
}

Database::BuiltSearchQuery Database::buildSearchWhere(const SearchFilter& filter) {
    const auto orArms = SplitTopLevelPowerFieldOr(filter.keyword);
    if (!orArms.empty()) {
        SearchFilter commonFilter = filter;
        commonFilter.keyword.clear();
        BuiltSearchQuery combined = buildSearchWhere(commonFilter);
        combined.whereClause += " AND af.id IN (";
        for (size_t i = 0; i < orArms.size(); ++i) {
            SearchFilter armFilter;
            armFilter.keyword = orArms[i];
            armFilter.exactMode = filter.exactMode;
            armFilter.synonyms = filter.synonyms;
            BuiltSearchQuery arm = buildSearchWhere(armFilter);
            if (arm.parseError) return arm;
            if (i != 0) combined.whereClause += " UNION ";
            combined.whereClause += "SELECT af.id " + arm.fromClause + " " + arm.whereClause;
            combined.bindValues.insert(combined.bindValues.end(), arm.bindValues.begin(), arm.bindValues.end());
        }
        combined.whereClause += ")";
        return combined;
    }
    BuiltSearchQuery bq;

    NormalizedPowerFieldFilters powerFields = ExtractNormalizedPowerFieldFilters(filter.keyword);

    bool hasKeyword = !powerFields.keyword.empty();
    bool hasFolder  = !filter.folderPath.empty();
    bool hasArtists = !filter.artists.empty();
    bool hasArtist  = !filter.artist.empty() && !hasArtists;
    bool hasAlbum   = !filter.album.empty();
    bool hasFormat  = !filter.format.empty();

    SearchQueryResult q;
    if (hasKeyword) {
        q = ParseSearchQueryToFts5(powerFields.keyword, filter.exactMode, filter.synonyms);
        if (!q.ok) {
            bq.parseError = true;
            bq.errorMsg = q.error;
            return bq;
        }
    }
    bool hasFtsQuery = hasKeyword && !q.ftsMatch.empty();
    bool hasTrigramQuery = hasKeyword && !q.nameContains.empty();
    bool hasTextQuery = hasFtsQuery || hasTrigramQuery;
    auto parserLane = [&]() -> const char* {
        if (!hasKeyword) {
            if (!filter.folderPath.empty() || !q.pathContains.empty() || !q.tagFilters.empty() || !q.colorFilters.empty()) {
                return "filter_only";
            }
            return "none";
        }
        if (!q.excludeTerms.empty() && !hasFtsQuery && !hasTrigramQuery) return "negative";
        if (!q.pathContains.empty() || !q.tagFilters.empty() || !q.colorFilters.empty()) return "mixed";
        if (q.ftsMatch.empty() && q.nameContains.empty()) return "operator_only";
        return "text";
    };
    auto sqlLane = [&]() -> const char* {
        if (hasFtsQuery && hasTrigramQuery) return "fts_union_trigram";
        if (hasFtsQuery) return "fts";
        if (hasTrigramQuery) return "trigram";
        if (!filter.folderPath.empty() || !q.pathContains.empty() || !q.tagFilters.empty() || !q.colorFilters.empty()) {
            return "filter_only";
        }
        return "plain";
    };
    // hasFtsJoin: whether the driving source is FTS-like (enables cover-key
    // subquery optimization and SELECT-column aliasing). True for both
    // FTS-only and the UNION case.
    bq.hasFtsJoin = hasTextQuery;
    // canUseFtsSnippet: snippet(...) and rank are visible only when FTS5 is the direct source.
    // when the driving FROM is audio_files_fts directly. Under UNION (A+B) or
    // trigram-only (B), the outer SELECT cannot reach those FTS5 helpers and
    // Under UNION or trigram-only input, sqlite3_prepare cannot reach FTS helpers.
    bq.canUseFtsSnippet = hasFtsQuery && !hasTrigramQuery;

    // Qualify columns with af. so outer and correlated SQL use the same alias.
    auto col = [&](const char* name) -> std::string {
        return std::string("af.") + name;
    };
    // Text and path search mask secondary metadata indexes so the driving index remains stable.
    // Cover-key indexes stay unmasked because exact cover lookups must use idx_files_*_key.
    auto col_sec = [&](const char* name) -> std::string {
        std::string c = std::string("af.") + name;
        // Mask secondary indexes only when a text query already owns the driving index.
        // Metadata-only filters keep their indexes so cover facets remain responsive.
        if (hasTextQuery &&
            strcmp(name, "embedded_cover_key") != 0 &&
            strcmp(name, "folder_cover_key") != 0) {
            return "+" + c;
        }
        return c;
    };

    // Build trigram MATCH expression from nameContains terms.
    // Multiple terms are joined with AND inside the MATCH (each quoted so
    // embedded spaces/punctuation don't get reinterpreted as operators).
    auto buildTrigramMatchExpr = [](const std::vector<std::string>& terms) -> std::string {
        std::string expr;
        for (size_t i = 0; i < terms.size(); ++i) {
            if (i > 0) expr += " AND ";
            expr += "\"";
            for (char c : terms[i]) {
                if (c == '"') expr += "\"\"";  // FTS5 quote escape
                else expr += c;
            }
            expr += "\"";
        }
        return expr;
    };

    // FROM and WHERE have four shapes depending on the driving text index:
    //   (A) FTS only       : join audio_files_fts
    //   (B) Trigram only   : join audio_files_name_trigram
    //   (C) Both           : UNION candidate rowids from FTS and trigram
    //                        (supplement semantics: substring hits on file_name
    //                         augment FTS hits on description/keywords/etc.)
    //   (D) Neither        : plain audio_files
    if (hasFtsQuery && hasTrigramQuery) {
        // C: UNION gives OR semantics across FTS and trigram sources.
        std::string trigramExpr = buildTrigramMatchExpr(q.nameContains);
        bq.fromClause =
            "FROM ("
                "SELECT rowid AS id FROM audio_files_fts WHERE audio_files_fts MATCH ? "
                "UNION "
                "SELECT rowid AS id FROM audio_files_name_trigram WHERE audio_files_name_trigram MATCH ?"
            ") matches JOIN audio_files af ON af.id = matches.id";
        bq.whereClause = "WHERE 1=1";
        bq.bindValues.push_back(q.ftsMatch);
        bq.bindValues.push_back(trigramExpr);
    } else if (hasFtsQuery) {
        // A: FTS-only path.
        bq.fromClause = "FROM audio_files af "
                         "JOIN audio_files_fts ON af.id = audio_files_fts.rowid";
        bq.whereClause = "WHERE audio_files_fts MATCH ?";
        bq.bindValues.push_back(q.ftsMatch);
    } else if (hasTrigramQuery) {
        // B: trigram-only path for suffix-eligible terms beside a negative FTS query.
        // siblings is one edge case; in practice this branch is rarely hit because the
        // parser only collects trigram terms from positive AND-conjunctions, which also
        // produce a non-empty ftsMatch. Keep for completeness.
        std::string trigramExpr = buildTrigramMatchExpr(q.nameContains);
        bq.fromClause = "FROM audio_files_name_trigram "
                         "JOIN audio_files af ON af.id = audio_files_name_trigram.rowid";
        bq.whereClause = "WHERE audio_files_name_trigram MATCH ?";
        bq.bindValues.push_back(trigramExpr);
    } else {
        // (D) No text query
        bq.fromClause = "FROM audio_files af";
        bq.whereClause = "WHERE 1=1";
    }
    DIAG_ACTION_FMT("SearchLane",
                    "buildSearchWhere parser=%s sql=%s hasKeyword=%d hasFts=%d hasTrigram=%d hasFolder=%d pathTerms=%d tagTerms=%d colorTerms=%d excludeTerms=%d",
                    parserLane(),
                    sqlLane(),
                    hasKeyword ? 1 : 0,
                    hasFtsQuery ? 1 : 0,
                    hasTrigramQuery ? 1 : 0,
                    hasFolder ? 1 : 0,
                    (int)q.pathContains.size(),
                    (int)q.tagFilters.size(),
                    (int)q.colorFilters.size(),
                    (int)q.excludeTerms.size());

    const auto explainableTerms = CollectExplainableKeywordTerms(powerFields.keyword);
    if (!explainableTerms.empty()) {
        static constexpr const char* kExplainableColumns[] = {
            "file_name", "file_path", "folder_path",
            "title", "artist", "album", "category", "description", "keywords"
        };
        for (const auto& term : explainableTerms) {
            bq.whereClause += " AND (";
            const std::string pattern = "%" + EscapeLikePatternLiteral(term) + "%";
            for (size_t i = 0; i < std::size(kExplainableColumns); ++i) {
                if (i > 0) bq.whereClause += " OR ";
                bq.whereClause += "IFNULL(" + col(kExplainableColumns[i]) + ",'') LIKE ? ESCAPE '\\'";
                bq.bindValues.push_back(pattern);
            }
            // Pinyin/initial/compact aliases remain explainable only when they
            // can be regenerated from the visible search fields.  Do not use
            // the stored search_data blob here: it also contains searchable
            // extended metadata (for example designer), which would let a row
            // pass the candidate guard without a user-visible match source.
            bq.whereClause +=
                " OR aura_search_data("
                "IFNULL(af.file_name,'') || ' ' || IFNULL(af.file_path,'') || ' ' || "
                "IFNULL(af.folder_path,'') || ' ' || IFNULL(af.title,'') || ' ' || "
                "IFNULL(af.artist,'') || ' ' || IFNULL(af.album,'') || ' ' || "
                "IFNULL(af.category,'') || ' ' || IFNULL(af.description,'') || ' ' || "
                "IFNULL(af.keywords,'')) LIKE ? ESCAPE '\\'";
            bq.bindValues.push_back(pattern);
            bq.whereClause += ")";
        }
    }

    // Folder range is the driving index and must not be masked with unary +.
    if (hasFolder) {
        std::string upper = MakeFolderPathUpperBound(filter.folderPath);
        bq.whereClause += " AND " + col("folder_path") + " >= ? AND " + col("folder_path") + " < ?";
        bq.bindValues.push_back(filter.folderPath);
        bq.bindValues.push_back(upper);
    }

    // path:... filters
    for (const auto& term : q.pathContains) {
        if (term.empty()) continue;
        bq.whereClause += " AND " + col_sec("file_path") + " LIKE ?";
        bq.bindValues.push_back("%" + term + "%");
    }

    // A negative-only keyword query becomes SQL NOT LIKE because FTS5 rejects standalone NOT.
    for (const auto& excl : q.excludeTerms) {
        if (excl.empty()) continue;
        static constexpr const char* kNegativeKeywordColumns[] = {
            "file_name", "file_path", "folder_path", "title", "artist",
            "album", "description", "keywords", "category", "search_data"
        };
        constexpr size_t kNegativeKeywordColumnCount =
            sizeof(kNegativeKeywordColumns) / sizeof(kNegativeKeywordColumns[0]);
        bq.whereClause += " AND NOT (";
        for (size_t i = 0; i < kNegativeKeywordColumnCount; ++i) {
            if (i > 0) bq.whereClause += " OR ";
            bq.whereClause += "IFNULL(" + col(kNegativeKeywordColumns[i]) + ",'') LIKE ? COLLATE NOCASE";
        }
        bq.whereClause += ")";
        std::string likeVal = "%" + excl + "%";
        for (size_t i = 0; i < kNegativeKeywordColumnCount; ++i) {
            bq.bindValues.push_back(likeVal);
        }
    }

    // F-007 tag filters require every named tag to exist in file_tags.
    for (const auto& tag : q.tagFilters) {
        if (tag.empty()) continue;
        bq.whereClause += " AND " + col("id") + " IN "
            "(SELECT ft.file_id FROM file_tags ft "
            "JOIN tags t ON ft.tag_id = t.id WHERE t.name = ?)";
        bq.bindValues.push_back(tag);
    }

    // color:... filters (F-007)
    for (const auto& clr : q.colorFilters) {
        if (clr.empty()) continue;
        bq.whereClause += " AND " + col_sec("track_colour") + " = ?";
        bq.bindValues.push_back(clr);
    }

    // Snooze: always exclude files that are snoozed until a future timestamp
    {
        int64_t nowTs = (int64_t)time(nullptr);
        bq.whereClause += " AND (" + col_sec("snoozed_until") + " = 0 OR " +
                          col_sec("snoozed_until") + " < " + std::to_string(nowTs) + ")";
    }

    // F-033: smart collection preset filters
    if (filter.unplayedOnly)
        bq.whereClause += " AND " + col_sec("play_count") + " = 0";
    if (filter.playedOnlyAny)
        bq.whereClause += " AND " + col_sec("last_played") + " > 0";
    if (filter.highlyRatedOnly)
        bq.whereClause += " AND " + col_sec("rating") + " >= 4";
    if (filter.addedAfter > 0)
        bq.whereClause += " AND " + col_sec("date_added") + " > " + std::to_string(filter.addedAfter);
    if (filter.playedAfter > 0)
        bq.whereClause += " AND " + col_sec("last_played") + " > " + std::to_string(filter.playedAfter);

    // F-033 pill filters use substring matching on normalized semicolon-separated fields.
    // Uses bq.bindValues for user-supplied strings (SQL injection safe).
    for (const auto& tag : filter.includeGenres) {
        bq.whereClause += " AND " + col_sec("norm_genre") + " LIKE ?";
        bq.bindValues.push_back("%" + tag + "%");
    }
    for (const auto& tag : powerFields.includeGenres) {
        bq.whereClause += " AND " + col_sec("norm_genre") + " LIKE ?";
        bq.bindValues.push_back("%" + tag + "%");
    }
    for (const auto& tag : filter.excludeGenres) {
        bq.whereClause += " AND (" + col_sec("norm_genre") + " NOT LIKE ? OR " + col_sec("norm_genre") + " IS NULL OR " + col_sec("norm_genre") + " = '')";
        bq.bindValues.push_back("%" + tag + "%");
    }
    for (const auto& tag : powerFields.excludeGenres) {
        bq.whereClause += " AND (" + col_sec("norm_genre") + " NOT LIKE ? OR " + col_sec("norm_genre") + " IS NULL OR " + col_sec("norm_genre") + " = '')";
        bq.bindValues.push_back("%" + tag + "%");
    }
    for (const auto& tag : filter.includeInstruments) {
        bq.whereClause += " AND " + col_sec("norm_instruments") + " LIKE ?";
        bq.bindValues.push_back("%" + tag + "%");
    }
    for (const auto& tag : powerFields.includeInstruments) {
        bq.whereClause += " AND " + col_sec("norm_instruments") + " LIKE ?";
        bq.bindValues.push_back("%" + tag + "%");
    }
    for (const auto& tag : filter.excludeInstruments) {
        bq.whereClause += " AND (" + col_sec("norm_instruments") + " NOT LIKE ? OR " + col_sec("norm_instruments") + " IS NULL OR " + col_sec("norm_instruments") + " = '')";
        bq.bindValues.push_back("%" + tag + "%");
    }
    for (const auto& tag : powerFields.excludeInstruments) {
        bq.whereClause += " AND (" + col_sec("norm_instruments") + " NOT LIKE ? OR " + col_sec("norm_instruments") + " IS NULL OR " + col_sec("norm_instruments") + " = '')";
        bq.bindValues.push_back("%" + tag + "%");
    }

    // Duration
    if (filter.minDuration > 0)
        bq.whereClause += " AND " + col_sec("duration") + " >= " + std::to_string(filter.minDuration);
    if (filter.maxDuration > 0)
        bq.whereClause += " AND " + col_sec("duration") + " <= " + std::to_string(filter.maxDuration);

    // Sample rate
    if (filter.minSampleRate > 0)
        bq.whereClause += " AND " + col_sec("sample_rate") + " >= " + std::to_string(filter.minSampleRate);
    if (filter.maxSampleRate > 0)
        bq.whereClause += " AND " + col_sec("sample_rate") + " <= " + std::to_string(filter.maxSampleRate);

    // BPM range (bpm is TEXT column, cast via NULLIF to avoid empty-string NULLs)
    if (filter.minBpm > 0.0f)
        bq.whereClause += " AND CAST(NULLIF(" + col_sec("bpm") + ",'') AS REAL) >= " + std::to_string(filter.minBpm);
    if (filter.maxBpm > 0.0f)
        bq.whereClause += " AND CAST(NULLIF(" + col_sec("bpm") + ",'') AS REAL) <= " + std::to_string(filter.maxBpm);

    // Musical key (exact match on key name or Camelot code)
    if (!filter.musicalKey.empty()) {
        bq.whereClause += " AND (" + col_sec("musical_key") + " = ? OR " + col_sec("musical_key_camelot") + " = ?)";
        bq.bindValues.push_back(filter.musicalKey);
        bq.bindValues.push_back(filter.musicalKey);
    }

    // Category
    if (!filter.category.empty()) {
        bq.whereClause += " AND " + col_sec("category") + " = ?";
        bq.bindValues.push_back(filter.category);
    }

    // Artist
    if (hasArtists) {
        bq.whereClause += " AND (";
        for (size_t i = 0; i < filter.artists.size(); ++i) {
            if (i > 0) bq.whereClause += " OR ";
            bq.whereClause += col_sec("artist") + " LIKE ?";
            bq.bindValues.push_back("%" + filter.artists[i] + "%");
        }
        bq.whereClause += ")";
    } else if (hasArtist) {
        bq.whereClause += " AND " + col_sec("artist") + " LIKE ?";
        bq.bindValues.push_back("%" + filter.artist + "%");
    }

    // Album
    if (hasAlbum) {
        bq.whereClause += " AND " + col_sec("album") + " LIKE ?";
        bq.bindValues.push_back("%" + filter.album + "%");
    }

    // Format
    if (hasFormat) {
        bq.whereClause += " AND " + col_sec("format") + " = ?";
        bq.bindValues.push_back(filter.format);
    }

    // Channels: prefer enum-driven category (UI path) over numeric fields.
    if (filter.channelCategory != SearchFilter::ChannelCategory::Any) {
        const std::string ch = col_sec("channels");
        switch (filter.channelCategory) {
            case SearchFilter::ChannelCategory::Mono:
                bq.whereClause += " AND " + ch + " = 1"; break;
            case SearchFilter::ChannelCategory::Stereo:
                bq.whereClause += " AND " + ch + " = 2"; break;
            case SearchFilter::ChannelCategory::Quad:
                bq.whereClause += " AND " + ch + " = 4"; break;
            case SearchFilter::ChannelCategory::Surround_5_1:
                bq.whereClause += " AND " + ch + " = 6"; break;
            case SearchFilter::ChannelCategory::Surround_7_1:
                bq.whereClause += " AND " + ch + " = 8"; break;
            case SearchFilter::ChannelCategory::MultiOther:
                bq.whereClause += " AND " + ch + " >= 3 AND " + ch + " NOT IN (4,6,8)"; break;
            default: break;
        }
    } else {
        if (filter.channels > 0)
            bq.whereClause += " AND " + col_sec("channels") + " = " + std::to_string(filter.channels);
        if (filter.minChannels > 0)
            bq.whereClause += " AND " + col_sec("channels") + " >= " + std::to_string(filter.minChannels);
        if (filter.maxChannels > 0)
            bq.whereClause += " AND " + col_sec("channels") + " <= " + std::to_string(filter.maxChannels);
    }

    // Cover filter
    if (filter.coverFilter == 1)
        bq.whereClause += " AND " + col_sec("has_cover") + " = 1";
    else if (filter.coverFilter == 2)
        bq.whereClause += " AND " + col_sec("has_cover") + " = 0";


    if (!filter.specificCoverKey.empty()) {
        ExactCoverWallSelectionMembers exactMembers;
        if (ParseExactCoverWallSelection(filter.specificCoverKey, exactMembers)) {
            ExpandExactCoverWallRegistryAliases(m_db, exactMembers);
            if (!exactMembers.keys.empty()) {
                EnsureCoverTileMembershipForKeys(m_db, exactMembers.keys);
            }
            // json_each keeps the number of SQL bind parameters fixed even
            // when one visible tile represents thousands of legacy folders.
            bq.coverKeySubquery =
                "SELECT file_id FROM cover_tile_membership "
                "WHERE tile_key IN (SELECT value FROM json_each(?))";
            bq.coverKeyBindValues.push_back(CoverWallSelectionJsonArray(exactMembers.keys));
            if (!exactMembers.temporaryFolders.empty()) {
                bq.coverKeySubquery +=
                    " UNION SELECT id FROM audio_files "
                    "WHERE has_cover = 1 "
                    "AND (embedded_cover_key IS NULL OR embedded_cover_key = '') "
                    "AND (folder_cover_key IS NULL OR folder_cover_key = '') "
                    "AND lower(replace(folder_path, '/', '\\')) "
                    "IN (SELECT value FROM json_each(?))";
                bq.coverKeyBindValues.push_back(CoverWallSelectionJsonArray(exactMembers.temporaryFolders));
            }
            bq.coverKeyClauseKind = 3;
        } else {
        // audio-library artwork identity: one visible artwork may represent
        // multiple embedded/folder keys. Resolve the visual cluster before filtering.
        // T2.2: 用户输入的 specificCoverKey 必须先过 CoverModule::CanonicalCoverKey,
        //       否则与 audio_files 列里的 canonical 形态做 == 比较会失配.
        std::string safeKey = CanonicalizeCoverLookupKey(filter.specificCoverKey);
        std::string temporaryFolderPath;
        const bool hasTemporaryFolderIdentity =
            TryExtractTemporaryFolderIdentityPath(safeKey, &temporaryFolderPath);

        std::vector<std::string> equivalentKeys;
        const bool reusedPreparedKeys =
            getPreparedCoverFilterKeys(safeKey, equivalentKeys);
        if (!reusedPreparedKeys) {
            equivalentKeys = ResolveEquivalentCoverKeys(this, safeKey);
        }
        if (equivalentKeys.empty()) equivalentKeys.push_back(safeKey);

        auto placeholders = [](size_t count) {
            std::string out;
            for (size_t i = 0; i < count; ++i) {
                if (i > 0) out += ",";
                out += "?";
            }
            return out;
        };
        const std::string inList = placeholders(equivalentKeys.size());

        if (!hasTemporaryFolderIdentity) {
            EnsureCoverTileMembershipForKeys(m_db, equivalentKeys);
        }

        if (reusedPreparedKeys) {
            DIAG_ACTION_FMT("Database",
                            "CoverFilterPreparedReuse: key=%s keys=%d",
                            safeKey.substr(0, (std::min)(size_t(24), safeKey.size())).c_str(),
                            (int)equivalentKeys.size());
        }

        if (bq.hasFtsJoin) {
            if (equivalentKeys.size() == 1 && hasTemporaryFolderIdentity) {
                bq.coverKeySubquery =
                    "SELECT id FROM audio_files WHERE "
                    "has_cover = 1 "
                    "AND (embedded_cover_key IS NULL OR embedded_cover_key = '') "
                    "AND (folder_cover_key IS NULL OR folder_cover_key = '') "
                    "AND folder_path IS NOT NULL AND folder_path <> '' "
                    "AND lower(replace(folder_path, '/', '\\')) = ?";
                bq.coverKeyBindValues.push_back(temporaryFolderPath);
                bq.coverKeyClauseKind = 3;
            } else {
                bq.coverKeySubquery =
                    "SELECT DISTINCT file_id FROM cover_tile_membership WHERE tile_key IN (" + inList + ")";
                for (const auto& key : equivalentKeys) bq.coverKeyBindValues.push_back(key);
                bq.coverKeyClauseKind = 3;
            }
        } else {
            if (equivalentKeys.size() == 1 && hasTemporaryFolderIdentity) {
                bq.whereClause +=
                    " AND has_cover = 1"
                    " AND (" + col("embedded_cover_key") + " IS NULL OR " + col("embedded_cover_key") + " = '')"
                    " AND (" + col("folder_cover_key") + " IS NULL OR " + col("folder_cover_key") + " = '')"
                    " AND " + col("folder_path") + " IS NOT NULL"
                    " AND " + col("folder_path") + " <> ''"
                    " AND lower(replace(" + col("folder_path") + ", '/', '\\')) = ?";
                bq.bindValues.push_back(temporaryFolderPath);
                bq.coverKeyClauseKind = 3;
            } else {
                bq.coverKeySubquery =
                    "SELECT DISTINCT file_id FROM cover_tile_membership WHERE tile_key IN (" + inList + ")";
                for (const auto& key : equivalentKeys) bq.coverKeyBindValues.push_back(key);
                bq.coverKeyClauseKind = 3;
            }
        }
        }
    }

    // Extension exclusion
    if (!filter.excludeExtensions.empty()) {
        bq.whereClause += " AND " + col_sec("extension") + " COLLATE NOCASE NOT IN (";
        for (size_t i = 0; i < filter.excludeExtensions.size(); ++i) {
            if (i > 0) bq.whereClause += ",";
            bq.whereClause += "?";
            bq.bindValues.push_back(filter.excludeExtensions[i]);
        }
        bq.whereClause += ")";
    }

    // Include only known audio extensions
    if (filter.includeOnlyKnownExts) {
        bq.whereClause += " AND " + col_sec("extension") + " COLLATE NOCASE IN ("
            "'.wav','.wave','.mp3','.flac','.ogg','.aiff','.aif','.m4a','.wma')";
    }

    return bq;
}

// ---------------------------------------------------------------------------
// Helper: bind all values from BuiltSearchQuery to a prepared statement
// ---------------------------------------------------------------------------
static void BindSearchQuery(sqlite3_stmt* stmt, const Database::BuiltSearchQuery& bq) {
    for (size_t i = 0; i < bq.bindValues.size(); ++i) {
        sqlite3_bind_text(stmt, (int)(i + 1), bq.bindValues[i].c_str(), -1, SQLITE_TRANSIENT);
    }
}

static void BindSearchQueryRepeated(sqlite3_stmt* stmt, const Database::BuiltSearchQuery& bq, int repeats) {
    if (repeats <= 0) return;
    int bindIndex = 1;
    for (int r = 0; r < repeats; ++r) {
        for (const auto& value : bq.bindValues) {
            sqlite3_bind_text(stmt, bindIndex++, value.c_str(), -1, SQLITE_TRANSIENT);
        }
    }
}

static void BindCoverSearchQuery(sqlite3_stmt* stmt, const Database::BuiltSearchQuery& bq) {
    BindSearchQuery(stmt, bq);
    int nextBind = (int)bq.bindValues.size() + 1;
    for (const auto& val : bq.coverKeyBindValues) {
        sqlite3_bind_text(stmt, nextBind++, val.c_str(), -1, SQLITE_TRANSIENT);
    }
}

static int BindSearchAndCoverQuery(sqlite3_stmt* stmt,
                                   const Database::BuiltSearchQuery& bq,
                                   int firstBind = 1) {
    int bindIndex = firstBind;
    for (const auto& value : bq.bindValues) {
        sqlite3_bind_text(stmt, bindIndex++, value.c_str(), -1, SQLITE_TRANSIENT);
    }
    for (const auto& value : bq.coverKeyBindValues) {
        sqlite3_bind_text(stmt, bindIndex++, value.c_str(), -1, SQLITE_TRANSIENT);
    }
    return bindIndex;
}

static void LogCoverSearchQueryPlan(sqlite3* db,
                                    const std::string& sql,
                                    const Database::BuiltSearchQuery& bq) {
    if (!db) return;
    sqlite3_stmt* planStmt = nullptr;
    const std::string planSql = "EXPLAIN QUERY PLAN " + sql;
    if (sqlite3_prepare_v2(db, planSql.c_str(), -1, &planStmt, nullptr) != SQLITE_OK) {
        AppendCoverPerfLog(std::string("[CoverFilterTrace] queryPlanPrepareFail err=") + sqlite3_errmsg(db));
        return;
    }
    BindCoverSearchQuery(planStmt, bq);
    int row = 0;
    while (sqlite3_step(planStmt) == SQLITE_ROW && row < 16) {
        const unsigned char* detail = sqlite3_column_text(planStmt, 3);
        std::ostringstream oss;
        oss << "[CoverFilterTrace] queryPlan[" << row << "] "
            << (detail ? reinterpret_cast<const char*>(detail) : "");
        AppendCoverPerfLog(oss.str());
        ++row;
    }
    sqlite3_finalize(planStmt);
}

static std::string SearchFromClauseWithCoverIndex(const Database::BuiltSearchQuery& bq) {
    if (bq.hasFtsJoin) return bq.fromClause;
    if (bq.coverKeyClauseKind == 1) {
        return "FROM audio_files af INDEXED BY idx_files_cover_fold_sort";
    }
    if (bq.coverKeyClauseKind == 2) {
        return "FROM audio_files af INDEXED BY idx_files_cover_emb_sort";
    }
    return bq.fromClause;
}

// ---------------------------------------------------------------------------
// Returns the total rows for a filter without LIMIT or OFFSET.
// ---------------------------------------------------------------------------
int64_t Database::searchFilesCount(const SearchFilter& filter) {
    DIAG_SCOPED("Database.searchFilesCount");
    if (!m_db) return 0;
    const bool allowZeroCountCache =
        !aura::cover_replacement::HasPersistentFacetFilterSemantics(filter);

    // 1) 纯文件夹点击：直接走 folder_index，绝不跑 COUNT(*)
    if (filter.folderPath.empty() && IsPureFolderFacetFilter(filter)) {
        return getFileCount();
    }

    const std::string persistentCacheKey = BuildPersistentFacetCacheKey(filter, -1, "count");
    {
        // 2a) In-memory cache: zero SQLite I/O, avoids WAL contention during active scans
        const int64_t nowSec = static_cast<int64_t>(std::time(nullptr));
        auto it = m_countMemCache.find(persistentCacheKey);
        if (it != m_countMemCache.end() && nowSec < it->second.expirySec) {
            if (it->second.count > 0 || allowZeroCountCache) {
                DIAG_ACTION_FMT("Search", "SearchCountMemCacheHit: count=%lld", (long long)it->second.count);
                return it->second.count;
            }
        }
    }
    {
        // A repeated filter can return directly from the persistent SQLite cache.
        std::vector<uint8_t> payload;
        if (loadFacetQueryCache(persistentCacheKey, payload)) {
            if (payload.size() == sizeof(int64_t)) {
                int64_t cachedCount = 0;
                std::memcpy(&cachedCount, payload.data(), sizeof(int64_t));
                if (cachedCount > 0 || allowZeroCountCache) {
                    DIAG_ACTION_FMT("Search", "SearchCountPersistentCacheHit: count=%lld", (long long)cachedCount);
                    // Promote to in-memory cache for subsequent frames
                    if (m_countMemCache.size() >= 16) m_countMemCache.clear();
                    m_countMemCache[persistentCacheKey] = {cachedCount, static_cast<int64_t>(std::time(nullptr)) + 30};
                    return cachedCount;
                }
            }
        }
    }

    if (filter.folderPath.empty() &&
        IsFileTypeOnlyFilter(filter) &&
        (!filter.excludeExtensions.empty() || filter.includeOnlyKnownExts)) {
        static const char* kKnownExts[] = {
            ".wav", ".wave", ".mp3", ".flac", ".ogg", ".aiff", ".aif", ".m4a", ".wma"
        };
        auto toLower = [](std::string s) {
            std::transform(s.begin(), s.end(), s.begin(),
                           [](unsigned char c) { return (char)std::tolower(c); });
            return s;
        };
        std::unordered_set<std::string> excluded;
        for (const auto& ext : filter.excludeExtensions) {
            excluded.insert(toLower(ext));
        }

        std::unordered_set<std::string> known;
        for (const char* ext : kKnownExts) known.insert(ext);

        std::unordered_map<std::string, int64_t> byExt;
        sqlite3_stmt* stmt = nullptr;
        const char* sql =
            "SELECT extension, COUNT(*) "
            "FROM audio_files INDEXED BY idx_files_extension_nocase "
            "GROUP BY extension COLLATE NOCASE;";
        if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                byExt[toLower(safeGetText(stmt, 0))] += sqlite3_column_int64(stmt, 1);
            }
            sqlite3_finalize(stmt);
            int64_t count = 0;
            if (filter.includeOnlyKnownExts) {
                for (const auto& [ext, n] : byExt) {
                    if (known.find(ext) != known.end() && excluded.find(ext) == excluded.end()) {
                        count += n;
                    }
                }
            } else {
                count = getFileCount();
                for (const auto& ext : excluded) {
                    auto it = byExt.find(ext);
                    if (it != byExt.end()) count -= it->second;
                }
                count = (std::max<int64_t>)(0, count);
            }
            if (count > 0 || allowZeroCountCache) {
                if (m_countMemCache.size() >= 16) m_countMemCache.clear();
                m_countMemCache[persistentCacheKey] = {count, static_cast<int64_t>(std::time(nullptr)) + 30};
                std::vector<uint8_t> payload(sizeof(int64_t));
                std::memcpy(payload.data(), &count, sizeof(int64_t));
                saveFacetQueryCache(persistentCacheKey, payload, 1);
            }
            DIAG_ACTION_FMT("Search", "SearchCountFileTypeFastPath: count=%lld",
                            (long long)count);
            return count;
        }
    }
    if (!filter.folderPath.empty() && IsPureFolderFacetFilter(filter)) {
        return getFolderFileCount(filter.folderPath);
    }

    // 2. In-memory count cache.
    BuiltSearchQuery bq = buildSearchWhere(filter);
    if (bq.parseError) {
        m_lastError = bq.errorMsg;
        return 0;
    }

    if (bq.hasFtsJoin && !ftsHasRows(m_db)) {
        std::string tok = firstToken(filter.keyword);
        if (!tok.empty()) {
            bq.fromClause = "FROM audio_files af";
            bq.whereClause = "WHERE af.file_name LIKE ? COLLATE NOCASE";
            bq.bindValues.clear();
            bq.bindValues.push_back(tok + "%");
            bq.hasFtsJoin = false;
        }
    }

    // Limit FTS5 counting to IDs returned by the cover-key index.
    const std::string coverInClause = bq.coverKeySubquery.empty() ? std::string()
        : " AND af.id IN (" + bq.coverKeySubquery + ")";
    std::string sql = "SELECT COUNT(*) " + SearchFromClauseWithCoverIndex(bq) + " " + bq.whereClause + coverInClause;

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        m_lastError = sqlite3_errmsg(m_db);
        return 0;
    }

    BindSearchQuery(stmt, bq);
    {
        int nextBind = (int)bq.bindValues.size() + 1;
        for (const auto& val : bq.coverKeyBindValues) {
            sqlite3_bind_text(stmt, nextBind++, val.c_str(), -1, SQLITE_TRANSIENT);
        }
    }

    int64_t count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        count = sqlite3_column_int64(stmt, 0);
    }

    sqlite3_finalize(stmt);

    // Write the count back so revisiting this filter does not repeat COUNT.
    {
        if (count > 0 || allowZeroCountCache) {
            // 3a) Populate in-memory cache first (fast path for next call)
            if (m_countMemCache.size() >= 16) m_countMemCache.clear();
            m_countMemCache[persistentCacheKey] = {count, static_cast<int64_t>(std::time(nullptr)) + 30};

            // Store the count in the persistent SQLite cache.
            std::vector<uint8_t> payload(sizeof(int64_t));
            std::memcpy(payload.data(), &count, sizeof(int64_t));
            saveFacetQueryCache(persistentCacheKey, payload, 1);
        }
    }

    return count;
}

// ---------------------------------------------------------------------------
// Returns a random sample from the full result set with ORDER BY RANDOM().
// ---------------------------------------------------------------------------
std::vector<AudioRecord> Database::searchFilesRandom(const SearchFilter& filter, int limit) {
    std::vector<AudioRecord> results;
    if (!m_db || limit <= 0) return results;

    BuiltSearchQuery bq = buildSearchWhere(filter);
    if (bq.parseError) {
        m_lastError = bq.errorMsg;
        return results;
    }

    if (bq.hasFtsJoin && !ftsHasRows(m_db)) {
        std::string tok = firstToken(filter.keyword);
        if (!tok.empty()) {
            bq.fromClause = "FROM audio_files af";
            bq.whereClause = "WHERE af.file_name LIKE ? COLLATE NOCASE";
            bq.bindValues.clear();
            bq.bindValues.push_back(tok + "%");
            bq.hasFtsJoin = false;
        }
    }

    const std::string coverInClause = bq.coverKeySubquery.empty() ? std::string()
        : " AND af.id IN (" + bq.coverKeySubquery + ")";
    std::string sql;
    if (bq.hasFtsJoin) {
        sql = "SELECT " + std::string(kRecordColumnsAf) + " FROM audio_files af "
              "JOIN ("
              "  SELECT af.id AS id " + bq.fromClause + " " + bq.whereClause + coverInClause +
              "  ORDER BY RANDOM() LIMIT " + std::to_string(limit) +
              ") temp ON af.id = temp.id";
    } else {
        sql = "SELECT " + std::string(kRecordColumnsAf) + " FROM audio_files af "
              "JOIN ("
              "  SELECT af.id AS id " + bq.fromClause + " " + bq.whereClause +
              "  ORDER BY RANDOM() LIMIT " + std::to_string(limit) +
              ") temp ON af.id = temp.id";
    }

    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(m_db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        m_lastError = sqlite3_errmsg(m_db);
        return results;
    }

    BindSearchQuery(stmt, bq);
    {
        int nextBind = (int)bq.bindValues.size() + 1;
        for (const auto& val : bq.coverKeyBindValues) {
            sqlite3_bind_text(stmt, nextBind++, val.c_str(), -1, SQLITE_TRANSIENT);
        }
    }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        results.push_back(recordFromStatement(stmt));
    }

    sqlite3_finalize(stmt);
    return results;
}

// ---------------------------------------------------------------------------
// Paginated search using the shared buildSearchWhere result.
// ---------------------------------------------------------------------------
std::vector<AudioRecord> Database::searchFiles(const SearchFilter& filter, int limit, int offset) {
    DIAG_SCOPED("Database.searchFiles");
    std::vector<AudioRecord> results;
    if (!m_db) return results;

    if (!filter.folderPath.empty() &&
        IsPureFolderFacetFilter(filter) &&
        (SortColumn)filter.sortColumn == SortColumn::FileName &&
        filter.sortAscending) {
        return getFilesByFolder(filter.folderPath, limit, offset);
    }

    const bool traceCoverFilter = !filter.specificCoverKey.empty();
    const auto tTotal = std::chrono::steady_clock::now();
    auto tBuild = std::chrono::steady_clock::now();
    BuiltSearchQuery bq = buildSearchWhere(filter);
    const double buildMs = MsSince(tBuild);
    if (bq.parseError) {
        m_lastError = bq.errorMsg;
        return results;
    }

    NormalizedPowerFieldFilters explainPowerFields = ExtractNormalizedPowerFieldFilters(filter.keyword);
    SearchQueryResult explainQuery;
    if (!explainPowerFields.keyword.empty()) {
        explainQuery = ParseSearchQueryToFts5(explainPowerFields.keyword, filter.exactMode, filter.synonyms);
    }
    const bool explainHasPowerFieldMetadata =
        !explainPowerFields.includeGenres.empty() ||
        !explainPowerFields.excludeGenres.empty() ||
        !explainPowerFields.includeInstruments.empty() ||
        !explainPowerFields.excludeInstruments.empty();

    double ftsFallbackMs = 0.0;
    if (bq.hasFtsJoin && !ftsHasRows(m_db)) {
        auto tFts = std::chrono::steady_clock::now();
        std::string tok = firstToken(filter.keyword);
        if (!tok.empty()) {
            bq.fromClause = "FROM audio_files af";
            bq.whereClause = "WHERE af.file_name LIKE ? COLLATE NOCASE";
            bq.bindValues.clear();
            bq.bindValues.push_back(tok + "%");
            bq.hasFtsJoin = false;
        }
        ftsFallbackMs = MsSince(tFts);
    }

    std::string orderClause;
    switch ((SortColumn)filter.sortColumn) {
        case SortColumn::FileName:   orderClause = "af.file_name COLLATE AURA_NATURAL"; break;
        case SortColumn::Duration:   orderClause = "af.duration"; break;
        case SortColumn::SampleRate: orderClause = "af.sample_rate"; break;
        case SortColumn::Channels:   orderClause = "af.channels"; break;
        case SortColumn::Size:       orderClause = "af.file_size"; break;
        case SortColumn::Format:     orderClause = "af.format"; break;
        case SortColumn::Description: orderClause = "af.description"; break;
        case SortColumn::Keywords:    orderClause = "af.keywords"; break;
        case SortColumn::Category:    orderClause = "af.category"; break;
        case SortColumn::Artist:      orderClause = "af.artist"; break;
        case SortColumn::Album:       orderClause = "af.album"; break;
        case SortColumn::BPM:         orderClause = "CAST(NULLIF(af.bpm, '') AS REAL)"; break;
        case SortColumn::MusicalKey:  orderClause = "af.musical_key"; break;
        case SortColumn::Mood:        orderClause = "af.mood"; break;
        case SortColumn::DateAdded:  orderClause = "af.date_added"; break;
        case SortColumn::Cover:      orderClause = "af.has_cover"; break;
        case SortColumn::FilePath:   orderClause = "af.file_path"; break;
        default:                     orderClause = "af.file_name"; break;
    }
    std::string direction = filter.sortAscending ? " ASC" : " DESC";

    std::string sql;
    // Limit FTS5 row reads to IDs returned by the cover-key index.
    const std::string coverInClause = bq.coverKeySubquery.empty() ? std::string()
        : " AND af.id IN (" + bq.coverKeySubquery + ")";
    std::string fromClauseForSearch = SearchFromClauseWithCoverIndex(bq);
    if (!bq.hasFtsJoin &&
        !filter.folderPath.empty() &&
        (SortColumn)filter.sortColumn == SortColumn::FileName &&
        coverInClause.empty()) {
        // Folder-tree browsing is path-scoped navigation. Drive the first page
        // from the composite folder/name index so large libraries do not sort a
        // broad candidate set before returning visible rows.
        fromClauseForSearch = "FROM audio_files af INDEXED BY idx_files_folder_name_natural";
    } else if (!bq.hasFtsJoin &&
        filter.folderPath.empty() &&
        IsFileTypeOnlyFilter(filter) &&
        (SortColumn)filter.sortColumn == SortColumn::FileName &&
        coverInClause.empty()) {
        // File-type toggles are usually low-selectivity in SFX libraries
        // (for example "show WAV only" still matches almost everything).
        // For first-page results, scanning the name index in display order and
        // filtering extensions is far faster than using extension-index hits
        // and sorting a million candidates in a temp B-tree.
        fromClauseForSearch = "FROM audio_files af INDEXED BY idx_files_name_natural";
    }
    if (bq.canUseFtsSnippet) {
        sql =
            "SELECT " + std::string(kRecordColumnsAf) + ", temp.match_snippet "
            "FROM audio_files af "
            "JOIN ("
            "  SELECT af.id AS id, "
            "         snippet(audio_files_fts, -1, char(1), char(2), '...', 12) AS match_snippet, "
            "         rank "
            "  " + fromClauseForSearch + " " + bq.whereClause + coverInClause +
            "  ORDER BY rank, af.id "
            "  LIMIT " + std::to_string(limit) + " OFFSET " + std::to_string(offset) +
            ") temp ON af.id = temp.id "
            "ORDER BY temp.rank, " + orderClause + direction;
    } else {
        // Include coverInClause here too — without it, the FTS+Trigram path
        // (canUseFtsSnippet=false but hasFtsJoin=true, e.g. keyword "horse"
        // which matches both FTS and trigram) silently dropped the cover-key
        // filter from the SQL, returning all keyword hits regardless of the
        // user's clicked cover.
        sql = "SELECT " + std::string(kRecordColumnsAf) + " FROM audio_files af "
              "JOIN ("
              "  SELECT af.id AS id " + fromClauseForSearch + " " + bq.whereClause + coverInClause +
              "  ORDER BY " + orderClause + direction +
              "  LIMIT " + std::to_string(limit) + " OFFSET " + std::to_string(offset) +
              ") temp ON af.id = temp.id "
              "ORDER BY " + orderClause + direction;
    }

    sqlite3_stmt* stmt;
    auto tPrepare = std::chrono::steady_clock::now();
    if (sqlite3_prepare_v2(m_db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        m_lastError = sqlite3_errmsg(m_db);
        if (traceCoverFilter) {
            std::ostringstream oss;
            oss << "[CoverFilterTrace] searchFiles prepareFail err=" << m_lastError
                << " keyPrefix=" << filter.specificCoverKey.substr(0, (std::min)(size_t(16), filter.specificCoverKey.size()))
                << " buildMs=" << buildMs
                << " sqlLen=" << sql.size();
            AppendCoverPerfLog(oss.str());
        }
        return results;
    }
    const double prepareMs = MsSince(tPrepare);

    auto tBind = std::chrono::steady_clock::now();
    BindCoverSearchQuery(stmt, bq);
    const double bindMs = MsSince(tBind);

    if (traceCoverFilter) {
        std::ostringstream oss;
        oss << "[CoverFilterTrace] searchFiles begin keyPrefix="
            << filter.specificCoverKey.substr(0, (std::min)(size_t(16), filter.specificCoverKey.size()))
            << " limit=" << limit
            << " offset=" << offset
            << " buildMs=" << buildMs
            << " ftsFallbackMs=" << ftsFallbackMs
            << " prepareMs=" << prepareMs
            << " bindMs=" << bindMs
            << " hasFtsJoin=" << (bq.hasFtsJoin ? 1 : 0)
            << " canSnippet=" << (bq.canUseFtsSnippet ? 1 : 0)
            << " binds=" << bq.bindValues.size()
            << " coverBinds=" << bq.coverKeyBindValues.size()
            << " sqlLen=" << sql.size();
        AppendCoverPerfLog(oss.str());
        LogCoverSearchQueryPlan(m_db, sql, bq);
    }

    auto tStep = std::chrono::steady_clock::now();
    double firstRowMs = -1.0;
    int rc = SQLITE_ROW;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        if (results.empty()) firstRowMs = MsSince(tStep);
        AudioRecord record = recordFromStatement(stmt);
        record.matchSource = DeriveSearchMatchSource(record, filter, explainPowerFields.keyword, explainQuery, explainHasPowerFieldMetadata);
        results.push_back(std::move(record));
    }
    const double stepMs = MsSince(tStep);

    sqlite3_finalize(stmt);
    if (traceCoverFilter) {
        std::ostringstream oss;
        oss << "[CoverFilterTrace] searchFiles end rows=" << results.size()
            << " firstRowMs=" << firstRowMs
            << " stepMs=" << stepMs
            << " totalMs=" << MsSince(tTotal)
            << " rc=" << rc;
        AppendCoverPerfLog(oss.str());
    }
    return results;
}

std::vector<AudioRecord> Database::getAllFiles(int limit, int offset) {
    SearchFilter filter;
    return searchFiles(filter, limit, offset);
}

std::vector<AudioRecord> Database::getRecentFiles(int limit) {
    std::vector<AudioRecord> results;
    if (!m_db) return results;

    std::string sql = std::string("SELECT ") + kRecordColumns + " FROM audio_files ORDER BY date_added DESC LIMIT " + std::to_string(limit);
    sqlite3_stmt* stmt;

    if (sqlite3_prepare_v2(m_db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        return results;
    }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        results.push_back(recordFromStatement(stmt));
    }

    sqlite3_finalize(stmt);
    return results;
}

int64_t Database::getFolderFileCount(const std::string& folderPath) {
    if (!m_db) return 0;

    // Query folder_index first; it is much cheaper than scanning audio_files.
    // A range scan replaces LIKE so the folder_path index remains usable.
    const std::string childPrefix = MakeFolderChildPrefix(folderPath);
    const std::string upper = MakeFolderPathUpperBound(childPrefix);
    const char* sqlIndex =
        "SELECT SUM(file_count) FROM folder_index "
        "WHERE folder_path = ? OR (folder_path >= ? AND folder_path < ?)";
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(m_db, sqlIndex, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, folderPath.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, childPrefix.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, upper.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            // Return immediately when folder_index has a count.
            int64_t count = sqlite3_column_int64(stmt, 0);
            sqlite3_finalize(stmt);
            if (count > 0) return count;
        } else {
            sqlite3_finalize(stmt);
        }
    }

    // Fall back to audio_files only when the folder-index cache has no rows.
    // 同样使用范围查询优化
    std::string sql =
        "SELECT COUNT(*) FROM audio_files "
        "WHERE folder_path = ? OR (folder_path >= ? AND folder_path < ?)";
    if (sqlite3_prepare_v2(m_db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) return 0;
    sqlite3_bind_text(stmt, 1, folderPath.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, childPrefix.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, upper.c_str(), -1, SQLITE_TRANSIENT);
    int64_t count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        count = sqlite3_column_int64(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return count;
}
int64_t Database::getFileCount() {
    if (!m_db) return 0;

    // Cache total counts for two seconds to avoid full scans on every UI frame.
    auto now = std::chrono::steady_clock::now();
    const uint64_t currentRevision = dataRevision();

    if (m_cachedFileCount >= 0 &&
        m_cachedFileCountRevision == currentRevision &&
        std::chrono::duration_cast<std::chrono::seconds>(now - m_cachedFileCountTime).count() < 2) {
        return m_cachedFileCount;
    }

    sqlite3_stmt* stmt = nullptr;
    int64_t count = -1;
    const char* sqlIndex = "SELECT COALESCE(SUM(file_count), -1) FROM folder_index";
    if (sqlite3_prepare_v2(m_db, sqlIndex, -1, &stmt, nullptr) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            count = sqlite3_column_int64(stmt, 0);
        }
        sqlite3_finalize(stmt);
        stmt = nullptr;
    }

    if (count < 0) {
        const char* sql = "SELECT COUNT(*) FROM audio_files";
        if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            return (m_cachedFileCount > 0) ? m_cachedFileCount : 0;
        }

        count = 0;
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            count = sqlite3_column_int64(stmt, 0);
        }
        sqlite3_finalize(stmt);
    }

    m_cachedFileCount = count;
    m_cachedFileCountRevision = currentRevision;
    m_cachedFileCountTime = now;
    return count;
}

// F-029 database statistics
Database::DatabaseStats Database::getDatabaseStats() {
    DatabaseStats stats;
    if (!m_db) return stats;

    stats.fileCount = getFileCount();

    // Total file sizes
    {
        const char* sql = "SELECT COALESCE(SUM(file_size), 0) FROM audio_files";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
            if (sqlite3_step(stmt) == SQLITE_ROW) {
                stats.totalSizeBytes = sqlite3_column_int64(stmt, 0);
            }
        }
        if (stmt) sqlite3_finalize(stmt);
    }
    // Folder count
    {
        const char* sql = "SELECT COUNT(DISTINCT folder_path) FROM audio_files WHERE folder_path IS NOT NULL AND folder_path != ''";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
            if (sqlite3_step(stmt) == SQLITE_ROW) {
                stats.folderCount = sqlite3_column_int64(stmt, 0);
            }
        }
        if (stmt) sqlite3_finalize(stmt);
    }
    // DB file size
    {
        const auto info = Platform::FileSystem::ProbeRegularFile(m_mainDbPath);
        if (info.isRegularFile && info.sizeAvailable) {
            stats.dbFileSizeBytes = static_cast<int64_t>(info.sizeBytes);
        }
    }
    // Last scan timestamp
    {
        const char* sql = "SELECT MAX(date_added) FROM audio_files";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
            if (sqlite3_step(stmt) == SQLITE_ROW && sqlite3_column_type(stmt, 0) != SQLITE_NULL) {
                int64_t ts = sqlite3_column_int64(stmt, 0);
                time_t t = (time_t)ts;
                char buf[32];
                struct tm tm_buf;
#ifdef _WIN32
                localtime_s(&tm_buf, &t);
#else
                localtime_r(&t, &tm_buf);
#endif
                strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &tm_buf);
                stats.lastScanTime = buf;
            }
        }
        if (stmt) sqlite3_finalize(stmt);
    }
    return stats;
}

// F-029 backup and restore
bool Database::backupDatabase(const std::string& srcDbPath, const std::string& dstPath, std::string& errorMessage) {
    std::string copyError;
    if (!Platform::FileSystem::CopyFile(srcDbPath, dstPath, true, &copyError)) {
        errorMessage = copyError;
        return false;
    }
    // Also copy WAL and SHM if they exist
    std::string walSrc = srcDbPath + "-wal";
    std::string shmSrc = srcDbPath + "-shm";
    if (Platform::FileSystem::Exists(walSrc)) {
        Platform::FileSystem::CopyFile(walSrc, dstPath + "-wal", true);
    }
    if (Platform::FileSystem::Exists(shmSrc)) {
        Platform::FileSystem::CopyFile(shmSrc, dstPath + "-shm", true);
    }
    return true;
}

bool Database::restoreDatabase(const std::string& backupPath, const std::string& dstDbPath, std::string& errorMessage) {
    // Safety: backup current before overwriting
    std::string safetyCopy = dstDbPath + ".pre-restore";
    Platform::FileSystem::CopyFile(dstDbPath, safetyCopy, true);
    // Copy backup over
    std::string copyError;
    if (!Platform::FileSystem::CopyFile(backupPath, dstDbPath, true, &copyError)) {
        errorMessage = copyError;
        return false;
    }
    return true;
}

// Startup health check (F-009)
HealthCheckResult Database::performHealthCheck(int sampleSize) {
    HealthCheckResult r;
    if (!m_db) {
        r.allOk = false;
        r.dbIntegrity = false;
        r.errors.push_back("Database not open");
        return r;
    }

    DIAG_SYSTEM("HealthCheck", "Starting startup health check");

    // 1. PRAGMA integrity_check (quick mode, single page)
    {
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(m_db, "PRAGMA quick_check(1)", -1, &stmt, nullptr) == SQLITE_OK) {
            if (sqlite3_step(stmt) == SQLITE_ROW) {
                std::string result = safeGetText(stmt, 0);
                if (result != "ok") {
                    r.dbIntegrity = false;
                    r.allOk = false;
                    r.errors.push_back("DB integrity check failed: " + result);
                }
            }
            sqlite3_finalize(stmt);
        }
    }

    // 2. FTS5 index validity
    {
        sqlite3_stmt* stmt = nullptr;
        const char* sql = "SELECT COUNT(*) FROM audio_files_fts WHERE audio_files_fts MATCH 'test_health_check_token_unlikely'";
        if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
            if (sqlite3_step(stmt) != SQLITE_ROW) {
                r.ftsIndexValid = false;
                r.allOk = false;
                r.errors.push_back("FTS5 index query failed");
            }
            sqlite3_finalize(stmt);
        } else {
            r.ftsIndexValid = false;
            r.allOk = false;
            r.errors.push_back("FTS5 table not accessible: " + std::string(sqlite3_errmsg(m_db)));
        }
    }

    // 3. Sample files: check disk existence + waveform presence
    {
        std::string sql = "SELECT file_path, duration, id FROM audio_files ORDER BY RANDOM() LIMIT " + std::to_string(sampleSize);
        sqlite3_stmt* stmt = nullptr;
        // Pre-prepare waveform check (may fail if waveforms_db not attached)
        sqlite3_stmt* wstmt = nullptr;
        const char* wsql = "SELECT 1 FROM waveforms_db.waveforms WHERE file_id = ? AND waveform_data IS NOT NULL LIMIT 1";
        bool hasWaveformDb = (sqlite3_prepare_v2(m_db, wsql, -1, &wstmt, nullptr) == SQLITE_OK);

        if (sqlite3_prepare_v2(m_db, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                r.totalFilesChecked++;
                std::string path = safeGetText(stmt, 0);
                double dur = sqlite3_column_double(stmt, 1);

                // Check disk existence
                try {
                    if (Platform::FileSystem::ProbePath(path) ==
                        Platform::FileSystem::PathStatus::Missing) {
                        r.orphanedRecords++;
                    }
                } catch (...) {
                    r.orphanedRecords++;
                }

                // Check waveform for files with duration
                if (dur > 0.1 && hasWaveformDb) {
                    int64_t fid = sqlite3_column_int64(stmt, 2);
                    sqlite3_reset(wstmt);
                    sqlite3_bind_int64(wstmt, 1, fid);
                    if (sqlite3_step(wstmt) != SQLITE_ROW) {
                        r.missingWaveforms++;
                    }
                }
            }
            sqlite3_finalize(stmt);
        }
        if (wstmt) sqlite3_finalize(wstmt);

        if (r.orphanedRecords > 0) {
            r.warnings.push_back("Found " + std::to_string(r.orphanedRecords) +
                " orphaned records (files missing on disk) in " +
                std::to_string(r.totalFilesChecked) + " samples");
        }
        if (r.missingWaveforms > 0) {
            r.warnings.push_back("Found " + std::to_string(r.missingWaveforms) +
                " files with duration but missing waveform cache");
        }
    }

    if (!r.warnings.empty() || !r.errors.empty()) r.allOk = false;

    DIAG_SYSTEM_FMT("HealthCheck",
        "Health check done: ok=%d integrity=%d fts=%d orphaned=%d missingWave=%d checked=%d",
        r.allOk ? 1 : 0, r.dbIntegrity ? 1 : 0, r.ftsIndexValid ? 1 : 0,
        r.orphanedRecords, r.missingWaveforms, r.totalFilesChecked);

    return r;
}

RepairCenterSnapshot Database::getRepairCenterSnapshot(uint64_t walWarningBytes) const {
    RepairCenterSnapshot snapshot = InspectDatabasePackageForRepair(m_mainDbPath, walWarningBytes);

    if (!m_db) {
        snapshot.warnings.push_back("Database connection is not open.");
        snapshot.needsAttention = true;
        return snapshot;
    }

    const std::vector<std::string> roots = getLibraryRoots();
    snapshot.libraryRootCount = static_cast<int>(roots.size());
    for (const std::string& root : roots) {
        if (root.empty()) continue;
        const auto pathStatus = Platform::FileSystem::ProbePath(root);
        if (pathStatus == Platform::FileSystem::PathStatus::Unreachable) {
            snapshot.unreachableLibraryRoots.push_back(root);
        } else if (pathStatus == Platform::FileSystem::PathStatus::Missing) {
            snapshot.missingLibraryRoots.push_back(root);
        }
    }
    snapshot.missingLibraryRootCount =
        static_cast<int>(snapshot.missingLibraryRoots.size());
    snapshot.unreachableLibraryRootCount =
        static_cast<int>(snapshot.unreachableLibraryRoots.size());

    auto queryCount = [this](const char* sql) -> int {
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            return -1;
        }
        int value = -1;
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            value = sqlite3_column_int(stmt, 0);
        }
        sqlite3_finalize(stmt);
        return value;
    };

    snapshot.failedWaveformRows = queryCount(
        "SELECT COUNT(*) FROM waveforms_db.waveforms WHERE status = 'failed'");
    snapshot.pendingWaveformRows = queryCount(
        "SELECT COUNT(*) FROM waveforms_db.waveforms WHERE status IN ('pending', 'running')");
    snapshot.missingCoverThumbRows = queryCount(
        "SELECT COUNT(*) "
        "FROM cover_cache_registry r "
        "LEFT JOIN blobs_db.cover_thumbs t ON t.cover_key = r.cover_key "
        "WHERE r.cover_key IS NOT NULL AND r.cover_key <> '' AND t.cover_key IS NULL");

    if (snapshot.missingLibraryRootCount > 0) {
        snapshot.warnings.push_back("Missing library roots: " +
                                    std::to_string(snapshot.missingLibraryRootCount));
    }
    if (snapshot.failedWaveformRows > 0) {
        snapshot.warnings.push_back("Failed waveform cache rows: " +
                                    std::to_string(snapshot.failedWaveformRows));
    }
    if (snapshot.missingCoverThumbRows > 0) {
        snapshot.warnings.push_back("Missing cover thumbnail rows: " +
                                    std::to_string(snapshot.missingCoverThumbRows));
    }
    if (snapshot.failedWaveformRows < 0 ||
        snapshot.pendingWaveformRows < 0 ||
        snapshot.missingCoverThumbRows < 0) {
        snapshot.warnings.push_back("Repair snapshot could not read one or more cache tables.");
    }

    snapshot.needsAttention =
        snapshot.walSizeWarning ||
        snapshot.missingLibraryRootCount > 0 ||
        snapshot.failedWaveformRows > 0 ||
        snapshot.missingCoverThumbRows > 0 ||
        snapshot.failedWaveformRows < 0 ||
        snapshot.pendingWaveformRows < 0 ||
        snapshot.missingCoverThumbRows < 0;

    return snapshot;
}

int Database::purgeResourceForkFiles() {
    if (!m_db) return 0;

    // Remove macOS AppleDouble resource-fork records.
    // 1. Names beginning with ._ are standard resource forks.
    // 2. Renamed forms such as "[tag] ._name" are also removed.
    const char* sql =
        "DELETE FROM audio_files WHERE "
        "file_name LIKE '._%%' OR "                  // ._xxx.wav
        "file_name LIKE '%% ._%%'";                  // [标签] ._xxx.wav

    char* errMsg = nullptr;
    int rc = sqlite3_exec(m_db, sql, nullptr, nullptr, &errMsg);
    if (rc != SQLITE_OK) {
        if (errMsg) {
            m_lastError = errMsg;
            sqlite3_free(errMsg);
        }
        return 0;
    }
    const int removed = sqlite3_changes(m_db);
    if (removed > 0) {
        pruneOrphanCoverCaches();
        invalidateFacetQueryCache();
        bumpDataRevision();
    }
    return removed;
}

int Database::purgeLowResWaveformDetail(int minFloatCount) {
    if (!m_db) return 0;
    
    // waveform_detail stores float samples; each float occupies four bytes.
    // At req=400 in mono, a valid detail BLOB needs at least 400 floats or 1600 bytes.
    // Smaller rows came from the old low-resolution cache bug.
    int minBytes = minFloatCount * (int)sizeof(float);
    
    // Set undersized waveform_detail to NULL while retaining the other waveform and RMS fields.
    std::string sql =
        "UPDATE waveforms_db.waveforms SET waveform_detail = NULL, waveform_min_detail = NULL, detail_num_channels = 0 "
        "WHERE waveform_detail IS NOT NULL AND length(waveform_detail) < " + std::to_string(minBytes) + " "
        "AND NOT " + std::string(kWpk2SqlValidWaveformDetail) + ";";
    
    char* errMsg = nullptr;
    int rc = sqlite3_exec(m_db, sql.c_str(), nullptr, nullptr, &errMsg);
    if (rc != SQLITE_OK) {
        if (errMsg) {
            m_lastError = errMsg;
            sqlite3_free(errMsg);
        }
        return 0;
    }
    return sqlite3_changes(m_db);
}

void Database::propagateFolderCovers() {
    if (!m_db) return;
    // Use INSTR instead of LIKE so wildcard characters in paths remain literal.
    // INSTR(folder_path, parent_path) = 1 能够 100% 精准识别父子继承关系
    const char* sql = R"SQL(
        UPDATE audio_files 
        SET folder_cover_key = (
            SELECT folder_cover_key FROM audio_files AS parent
            WHERE INSTR(audio_files.folder_path, parent.folder_path) = 1
              AND parent.folder_cover_key IS NOT NULL 
              AND parent.folder_cover_key <> ''
            ORDER BY LENGTH(parent.folder_path) DESC
            LIMIT 1
        ),
        has_cover = 1
        WHERE (folder_cover_key IS NULL OR folder_cover_key = '')
          AND (embedded_cover_key IS NULL OR embedded_cover_key = '')
          AND IFNULL(cover_suppressed, 0) = 0;
    )SQL";

    beginTransaction();
    executeSQL(sql);
    commitTransaction();
}

int Database::propagateUniqueFolderCovers(int maxFolders) {
    if (!m_db) return 0;
    if (maxFolders < 0) maxFolders = 0;

    const char* sql = R"SQL(
WITH folder_key_single AS (
    SELECT folder_path, MIN(folder_cover_key) AS key
    FROM audio_files
    WHERE folder_cover_key IS NOT NULL AND folder_cover_key <> ''
    GROUP BY folder_path
    HAVING COUNT(DISTINCT folder_cover_key) = 1
),
embedded_key_single AS (
    SELECT folder_path, MIN(embedded_cover_key) AS key
    FROM audio_files
    WHERE embedded_cover_key IS NOT NULL AND embedded_cover_key <> ''
    GROUP BY folder_path
    HAVING COUNT(DISTINCT embedded_cover_key) = 1
),
single_key_folder AS (
    -- Prefer explicit folder cover key when available; otherwise fallback to embedded key.
    SELECT folder_path, key FROM folder_key_single
    UNION ALL
    SELECT e.folder_path, e.key
    FROM embedded_key_single e
    WHERE e.folder_path NOT IN (SELECT folder_path FROM folder_key_single)
),
target_folders AS (
    SELECT skf.folder_path, skf.key
    FROM single_key_folder skf
    WHERE EXISTS (
        SELECT 1
        FROM audio_files af
        WHERE af.folder_path = skf.folder_path
          AND IFNULL(af.cover_suppressed, 0) = 0
          AND (af.embedded_cover_key IS NULL OR af.embedded_cover_key = '')
          AND (af.folder_cover_key IS NULL OR af.folder_cover_key = '')
    )
    ORDER BY skf.folder_path
    LIMIT CASE WHEN ? > 0 THEN ? ELSE 2147483647 END
)
UPDATE audio_files
SET folder_cover_key = (
        SELECT key FROM target_folders
        WHERE target_folders.folder_path = audio_files.folder_path
    ),
    has_cover = 1
WHERE folder_path IN (SELECT folder_path FROM target_folders)
  AND IFNULL(cover_suppressed, 0) = 0
  AND (embedded_cover_key IS NULL OR embedded_cover_key = '')
  AND (folder_cover_key IS NULL OR folder_cover_key = '');
)SQL";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return 0;
    }
    sqlite3_bind_int(stmt, 1, maxFolders);
    sqlite3_bind_int(stmt, 2, maxFolders);

    bool ownTxn = beginTransaction();
    bool ok = (sqlite3_step(stmt) == SQLITE_DONE);
    sqlite3_finalize(stmt);
    if (!ok) {
        if (ownTxn) rollbackTransaction();
        return 0;
    }
    int changed = sqlite3_changes(m_db);
    if (ownTxn) commitTransaction();
    return changed;
}

// Rebuild cover_cache_registry from audio_files.
// The registry and filtered facets use the same deduplicated keys from audio_files.
// Invalid keys are rejected by FolderCoverKeyPathExists when the UI builds its list.
void Database::syncCoverRegistry() {
    if (!m_db) return;

    normalizeFolderCoverKeysByVisualIdentity();

    const char* sqlClear = "DELETE FROM cover_cache_registry;";
    // Embedded keys are already canonical because they do not encode paths.
    const char* sqlEmbed = R"SQL(
INSERT INTO cover_cache_registry (cover_key, rep_path, is_embedded, normalized_key)
SELECT embedded_cover_key, MIN(file_path), 1, embedded_cover_key
FROM audio_files
WHERE embedded_cover_key IS NOT NULL AND embedded_cover_key <> ''
GROUP BY embedded_cover_key;
)SQL";
    // Register folder keys even when the same row also has embedded artwork.
    // INSERT OR IGNORE preserves an existing representative when embedded and folder keys collide.
    const char* sqlFolder = R"SQL(
INSERT OR IGNORE INTO cover_cache_registry (cover_key, rep_path, is_embedded, normalized_key)
SELECT folder_cover_key, MIN(folder_path), 0,
       CASE WHEN folder_cover_key LIKE 'F|H|%' THEN folder_cover_key ELSE '' END
FROM audio_files
WHERE folder_cover_key IS NOT NULL AND folder_cover_key <> ''
GROUP BY folder_cover_key;
)SQL";

    beginTransaction();
    executeSQL(sqlClear);
    executeSQL(sqlEmbed);
    executeSQL(sqlFolder);
    RebuildCoverTileMembershipIndex(m_db);
    WriteAppMetaInt(m_db, "cover_tile_membership_needs_maintenance", 0);
    commitTransaction();
}

void Database::rebuildCoverTileMembershipIndex() {
    if (!m_db) return;

    const bool ownTxn = beginTransaction();
    RebuildCoverTileMembershipIndex(m_db);
    WriteAppMetaInt(m_db, "cover_tile_membership_needs_maintenance", 0);
    if (ownTxn) commitTransaction();
}

// Update one registry row immediately after manual artwork assignment.
int Database::countAudioCoverKeyCandidates() {
    if (!m_db) return 0;

    const char* sql = R"SQL(
SELECT COUNT(*) FROM (
    SELECT embedded_cover_key AS cover_key
    FROM audio_files
    WHERE embedded_cover_key IS NOT NULL AND embedded_cover_key <> ''
    GROUP BY embedded_cover_key
    UNION
    SELECT folder_cover_key AS cover_key
    FROM audio_files
    WHERE folder_cover_key IS NOT NULL AND folder_cover_key <> ''
    GROUP BY folder_cover_key
);
)SQL";

    sqlite3_stmt* stmt = nullptr;
    int count = 0;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            count = sqlite3_column_int(stmt, 0);
        }
        sqlite3_finalize(stmt);
    }
    return count;
}

static bool IsLargeLibraryCoverRegistryBoundedSparse(sqlite3* db,
                                                     int registryEntries,
                                                     int64_t totalFiles,
                                                     bool* outMarkerPending) {
    if (outMarkerPending) *outMarkerPending = false;
    if (!db || totalFiles <= kGlobalCoverWallFallbackMaxFiles) return false;

    const bool markerPending =
        ReadAppMetaInt(db, "cover_registry_needs_maintenance") == 1;
    if (outMarkerPending) *outMarkerPending = markerPending;

    // Keep large-library cover browse bounded. Trust the persisted maintenance
    // marker, and treat an empty registry as sparse without doing a foreground
    // exact distinct recount over audio_files.
    return markerPending || registryEntries == 0;
}

static bool RearmCoverRegistryMaintenanceForEmptyLargeLibrary(sqlite3* db,
                                                              int registryEntries,
                                                              int64_t totalFiles) {
    if (!db || totalFiles <= kGlobalCoverWallFallbackMaxFiles) return false;
    if (registryEntries != 0) return false;
    if (ReadAppMetaInt(db, "cover_registry_needs_maintenance") == 1) return false;
    WriteAppMetaInt(db, "cover_registry_needs_maintenance", 1);
    return true;
}

static int CountAudioCoverPresenceCandidates(sqlite3* db) {
    if (!db) return 0;

    const char* sql = R"SQL(
SELECT COUNT(*) FROM (
    SELECT embedded_cover_key AS cover_identity
    FROM audio_files
    WHERE embedded_cover_key IS NOT NULL AND embedded_cover_key <> ''
    GROUP BY embedded_cover_key
    UNION
    SELECT folder_cover_key AS cover_identity
    FROM audio_files
    WHERE folder_cover_key IS NOT NULL AND folder_cover_key <> ''
    GROUP BY folder_cover_key
    UNION
    SELECT '?|' || folder_path AS cover_identity
    FROM audio_files
    WHERE has_cover = 1
      AND (embedded_cover_key IS NULL OR embedded_cover_key = '')
      AND (folder_cover_key IS NULL OR folder_cover_key = '')
      AND folder_path IS NOT NULL AND folder_path <> ''
    GROUP BY folder_path
);
)SQL";

    sqlite3_stmt* stmt = nullptr;
    int count = 0;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            count = sqlite3_column_int(stmt, 0);
        }
        sqlite3_finalize(stmt);
    }
    return count;
}

static void RebuildCoverTileMembershipIndex(sqlite3* db) {
    if (!db) return;

    sqlite3_exec(db, "DELETE FROM cover_tile_membership;", nullptr, nullptr, nullptr);
    sqlite3_exec(db, R"SQL(
INSERT OR IGNORE INTO cover_tile_membership (tile_key, file_id, membership_kind)
SELECT embedded_cover_key, id, 1
FROM audio_files
WHERE embedded_cover_key IS NOT NULL AND embedded_cover_key <> '';
)SQL", nullptr, nullptr, nullptr);
    sqlite3_exec(db, R"SQL(
INSERT OR IGNORE INTO cover_tile_membership (tile_key, file_id, membership_kind)
SELECT folder_cover_key, id, 2
FROM audio_files
WHERE folder_cover_key IS NOT NULL AND folder_cover_key <> '';
)SQL", nullptr, nullptr, nullptr);
}

static int SyncCoverTileMembershipForKeys(sqlite3* db, const std::vector<std::string>& coverKeys) {
    if (!db || coverKeys.empty()) return 0;

    sqlite3_stmt* deleteStmt = nullptr;
    sqlite3_stmt* insertEmbeddedStmt = nullptr;
    sqlite3_stmt* insertFolderStmt = nullptr;
    const char* deleteSql = "DELETE FROM cover_tile_membership WHERE tile_key = ?";
    // The redundant `<> ''` predicates let the SQLite optimizer pick the
    // partial indexes idx_files_cover_dedup_emb / idx_files_cover_dedup_fold
    // (declared `WHERE <col> <> ''`) instead of falling back to a full table
    // scan of audio_files. Without the guard, `embedded_cover_key = ?` does
    // not statically imply `<> ''`, so the planner conservatively scans.
    // Microbench: at 30k audio_files this drops per-cover sync stage from
    // ~13ms (table scan) to <0.2ms (index lookup).
    const char* insertEmbeddedSql = R"SQL(
INSERT OR IGNORE INTO cover_tile_membership (tile_key, file_id, membership_kind)
SELECT embedded_cover_key, id, 1
FROM audio_files
WHERE embedded_cover_key = ? AND embedded_cover_key <> '';
)SQL";
    const char* insertFolderSql = R"SQL(
INSERT OR IGNORE INTO cover_tile_membership (tile_key, file_id, membership_kind)
SELECT folder_cover_key, id, 2
FROM audio_files
WHERE folder_cover_key = ? AND folder_cover_key <> '';
)SQL";

    if (sqlite3_prepare_v2(db, deleteSql, -1, &deleteStmt, nullptr) != SQLITE_OK ||
        sqlite3_prepare_v2(db, insertEmbeddedSql, -1, &insertEmbeddedStmt, nullptr) != SQLITE_OK ||
        sqlite3_prepare_v2(db, insertFolderSql, -1, &insertFolderStmt, nullptr) != SQLITE_OK) {
        if (deleteStmt) sqlite3_finalize(deleteStmt);
        if (insertEmbeddedStmt) sqlite3_finalize(insertEmbeddedStmt);
        if (insertFolderStmt) sqlite3_finalize(insertFolderStmt);
        return 0;
    }

    int changed = 0;
    for (const auto& key : coverKeys) {
        if (key.empty()) continue;

        sqlite3_reset(deleteStmt);
        sqlite3_clear_bindings(deleteStmt);
        sqlite3_bind_text(deleteStmt, 1, key.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(deleteStmt);

        sqlite3_reset(insertEmbeddedStmt);
        sqlite3_clear_bindings(insertEmbeddedStmt);
        sqlite3_bind_text(insertEmbeddedStmt, 1, key.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(insertEmbeddedStmt) == SQLITE_DONE) {
            changed += sqlite3_changes(db);
        }

        sqlite3_reset(insertFolderStmt);
        sqlite3_clear_bindings(insertFolderStmt);
        sqlite3_bind_text(insertFolderStmt, 1, key.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(insertFolderStmt) == SQLITE_DONE) {
            changed += sqlite3_changes(db);
        }
    }

    sqlite3_finalize(deleteStmt);
    sqlite3_finalize(insertEmbeddedStmt);
    sqlite3_finalize(insertFolderStmt);
    return changed;
}

static void LoadCoverTileMembershipIdsForKeys(sqlite3* db,
                                              const std::vector<std::string>& tileKeys,
                                              std::vector<int64_t>* out) {
    if (!db || !out || tileKeys.empty()) return;
    const char* membershipSql =
        "SELECT DISTINCT file_id FROM cover_tile_membership "
        "WHERE tile_key IN (SELECT value FROM json_each(?)) ORDER BY file_id";
    sqlite3_stmt* membershipStmt = nullptr;
    if (sqlite3_prepare_v2(db, membershipSql, -1, &membershipStmt, nullptr) != SQLITE_OK) {
        return;
    }
    const std::string keysJson = CoverWallSelectionJsonArray(tileKeys);
    sqlite3_bind_text(membershipStmt, 1, keysJson.c_str(), (int)keysJson.size(), SQLITE_TRANSIENT);
    while (sqlite3_step(membershipStmt) == SQLITE_ROW) {
        out->push_back(sqlite3_column_int64(membershipStmt, 0));
    }
    sqlite3_finalize(membershipStmt);
}

static bool EnsureCoverTileMembershipForKeys(sqlite3* db,
                                             const std::vector<std::string>& tileKeys,
                                             std::vector<int64_t>* out) {
    if (!db || tileKeys.empty()) return false;

    std::vector<int64_t> scratch;
    std::vector<int64_t>* target = out ? out : &scratch;
    target->clear();
    LoadCoverTileMembershipIdsForKeys(db, tileKeys, target);
    if (sqlite3_db_readonly(db, "main") == 1) return !target->empty();
    const char* missingSql =
        "SELECT value FROM json_each(?) AS requested "
        "WHERE NOT EXISTS (SELECT 1 FROM cover_tile_membership "
        "WHERE tile_key = requested.value)";
    sqlite3_stmt* missingStmt = nullptr;
    if (sqlite3_prepare_v2(db, missingSql, -1, &missingStmt, nullptr) != SQLITE_OK) {
        return !target->empty();
    }
    const std::string keysJson = CoverWallSelectionJsonArray(tileKeys);
    sqlite3_bind_text(missingStmt, 1, keysJson.c_str(), (int)keysJson.size(), SQLITE_TRANSIENT);
    std::vector<std::string> missingKeys;
    while (sqlite3_step(missingStmt) == SQLITE_ROW) {
        const char* text = reinterpret_cast<const char*>(sqlite3_column_text(missingStmt, 0));
        if (text) missingKeys.emplace_back(text);
    }
    sqlite3_finalize(missingStmt);
    if (!missingKeys.empty()) {
        SyncCoverTileMembershipForKeys(db, missingKeys);
        target->clear();
        LoadCoverTileMembershipIdsForKeys(db, tileKeys, target);
    }
    return !target->empty();
}

struct GlobalCoverWallRawRow {
    std::string cursorKey;
    CoverFacetCandidate candidate;
};

static constexpr const char* kCoverWallEmbeddedPhashTokenPrefix = "PHASH|";

struct GlobalCoverAggregationReadiness {
    int registryEntries = 0;
    int64_t totalFiles = 0;
    int rawCoverPresenceCandidates = 0;
    int minimumExpectedRegistry = 0;
    bool largeLibrary = false;
    bool largeLibraryMarkerPending = false;
    bool registrySparse = false;
    bool useRegistry = false;
};

static int64_t CountRows64FromTable(sqlite3* db, const char* tableName) {
    if (!db || !tableName || !*tableName) return 0;
    const std::string sql = "SELECT COUNT(*) FROM " + std::string(tableName) + ";";
    sqlite3_stmt* stmt = nullptr;
    int64_t count = 0;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            count = sqlite3_column_int64(stmt, 0);
        }
        sqlite3_finalize(stmt);
    }
    return count;
}

static GlobalCoverAggregationReadiness AssessGlobalCoverAggregationReadiness(sqlite3* db) {
    GlobalCoverAggregationReadiness readiness;
    if (!db) return readiness;

    readiness.registryEntries =
        static_cast<int>(CountRows64FromTable(db, "cover_cache_registry"));
    readiness.totalFiles = CountRows64FromTable(db, "audio_files");
    readiness.largeLibrary =
        readiness.totalFiles > kGlobalCoverWallFallbackMaxFiles;
    readiness.useRegistry = readiness.registryEntries > 0;

    if (readiness.largeLibrary) {
        readiness.registrySparse = IsLargeLibraryCoverRegistryBoundedSparse(
            db,
            readiness.registryEntries,
            readiness.totalFiles,
            &readiness.largeLibraryMarkerPending);
    } else {
        readiness.rawCoverPresenceCandidates = CountAudioCoverPresenceCandidates(db);
        readiness.minimumExpectedRegistry = readiness.rawCoverPresenceCandidates;
        readiness.registrySparse =
            readiness.rawCoverPresenceCandidates > 0 &&
            readiness.registryEntries < readiness.minimumExpectedRegistry;
        if (readiness.registrySparse) {
            readiness.useRegistry = false;
        }
    }

    return readiness;
}

static std::vector<GlobalCoverWallRawRow> LoadGlobalCoverWallRegistryRows(
    sqlite3* db,
    const std::string& afterCursor,
    int queryLimit) {
    std::vector<GlobalCoverWallRawRow> rows;
    if (!db || queryLimit <= 0) return rows;

    const char* sql = R"SQL(
        SELECT r.cover_key,
               CASE WHEN r.normalized_key IS NOT NULL AND r.normalized_key != ''
                    THEN r.normalized_key ELSE r.cover_key END,
               CASE WHEN r.is_embedded != 0 THEN COALESCE((
                   SELECT af.file_path
                   FROM audio_files af INDEXED BY idx_files_cover_dedup_emb
                   WHERE af.embedded_cover_key = r.cover_key
                     AND af.embedded_cover_key <> ''
                     AND af.file_path IS NOT NULL
                     AND af.file_path <> ''
                   ORDER BY af.id
                   LIMIT 1
               ), r.rep_path) ELSE COALESCE((
                   SELECT af.folder_path
                   FROM audio_files af INDEXED BY idx_files_cover_dedup_fold
                   WHERE af.folder_cover_key = r.cover_key
                     AND af.folder_cover_key <> ''
                     AND af.folder_path IS NOT NULL
                     AND af.folder_path <> ''
                   ORDER BY af.id
                   LIMIT 1
               ), r.rep_path) END,
               CASE WHEN r.is_embedded = 0 THEN COALESCE((
                   SELECT af.folder_cover_src_path
                   FROM audio_files af INDEXED BY idx_files_cover_dedup_fold
                   WHERE af.folder_cover_key = r.cover_key
                     AND af.folder_cover_key <> ''
                     AND af.folder_cover_src_path IS NOT NULL
                     AND af.folder_cover_src_path <> ''
                   ORDER BY af.id
                   LIMIT 1
               ), '') ELSE '' END,
               r.is_embedded,
               r.cover_phash,
               1
        FROM cover_cache_registry r
        WHERE r.cover_key IS NOT NULL AND r.cover_key <> ''
          AND (? = '' OR r.cover_key > ?)
          AND (
              (r.is_embedded != 0 AND EXISTS (
                  SELECT 1
                  FROM audio_files af INDEXED BY idx_files_cover_dedup_emb
                  WHERE af.embedded_cover_key = r.cover_key
                    AND af.embedded_cover_key <> ''
                  LIMIT 1
              ))
              OR
              (r.is_embedded = 0 AND EXISTS (
                  SELECT 1
                  FROM audio_files af INDEXED BY idx_files_cover_dedup_fold
                  WHERE af.folder_cover_key = r.cover_key
                    AND af.folder_cover_key <> ''
                  LIMIT 1
              ))
          )
        ORDER BY r.cover_key
        LIMIT ?;
    )SQL";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return rows;
    }

    sqlite3_bind_text(stmt, 1, afterCursor.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, afterCursor.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 3, queryLimit);

    rows.reserve((size_t)queryLimit);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        GlobalCoverWallRawRow row;
        row.cursorKey = safeGetText(stmt, 0);
        row.candidate.key = safeGetText(stmt, 1);
        row.candidate.path = safeGetText(stmt, 2);
        row.candidate.sourcePath = safeGetText(stmt, 3);
        row.candidate.isEmbedded = sqlite3_column_int(stmt, 4) != 0;
        row.candidate.phash = sqlite3_column_int64(stmt, 5);
        row.candidate.count = sqlite3_column_int64(stmt, 6);
        if (row.candidate.isEmbedded && row.candidate.phash != 0) {
            row.candidate.sourcePath =
                std::string(kCoverWallEmbeddedPhashTokenPrefix) +
                std::to_string(row.candidate.phash);
        }
        if (!row.cursorKey.empty() && !row.candidate.key.empty() && !row.candidate.path.empty()) {
            rows.push_back(std::move(row));
        }
    }
    sqlite3_finalize(stmt);
    return rows;
}

static std::vector<GlobalCoverWallRawRow> LoadLargeLibrarySparseGlobalCoverWallRows(
    sqlite3* db,
    const std::string& afterCursor,
    int queryLimit) {
    std::vector<GlobalCoverWallRawRow> rows;
    if (!db || queryLimit <= 0) return rows;

    const char* sql = R"SQL(
        WITH missing AS (
            SELECT
                af.embedded_cover_key AS cover_key,
                MIN(af.file_path)     AS rep_path,
                ''                    AS source_path,
                1                     AS is_embedded
            FROM audio_files af INDEXED BY idx_files_cover_dedup_emb
            WHERE af.embedded_cover_key IS NOT NULL
              AND af.embedded_cover_key <> ''
              AND (? = '' OR af.embedded_cover_key > ?)
              AND NOT EXISTS (
                  SELECT 1
                  FROM cover_cache_registry r
                  WHERE r.cover_key = af.embedded_cover_key
              )
            GROUP BY af.embedded_cover_key

            UNION ALL

            SELECT
                af.folder_cover_key              AS cover_key,
                MIN(af.folder_path)             AS rep_path,
                MIN(NULLIF(af.folder_cover_src_path, '')) AS source_path,
                0                               AS is_embedded
            FROM audio_files af INDEXED BY idx_files_cover_dedup_fold
            WHERE af.folder_cover_key IS NOT NULL
              AND af.folder_cover_key <> ''
              AND (? = '' OR af.folder_cover_key > ?)
              AND NOT EXISTS (
                  SELECT 1
                  FROM cover_cache_registry r
                  WHERE r.cover_key = af.folder_cover_key
              )
            GROUP BY af.folder_cover_key

            UNION ALL

            SELECT
                '?|' || af.folder_path          AS cover_key,
                MIN(af.folder_path)             AS rep_path,
                MIN(NULLIF(af.folder_cover_src_path, '')) AS source_path,
                0                               AS is_embedded
            FROM audio_files af INDEXED BY idx_files_folder_name
            WHERE af.has_cover = 1
              AND (af.embedded_cover_key IS NULL OR af.embedded_cover_key = '')
              AND (af.folder_cover_key IS NULL OR af.folder_cover_key = '')
              AND af.folder_path IS NOT NULL
              AND af.folder_path <> ''
              AND (? = '' OR ('?|' || af.folder_path) > ?)
            GROUP BY af.folder_path
        )
        SELECT cover_key, rep_path, source_path, is_embedded
        FROM missing
        ORDER BY cover_key
        LIMIT ?;
    )SQL";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return rows;
    }

    sqlite3_bind_text(stmt, 1, afterCursor.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, afterCursor.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, afterCursor.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, afterCursor.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, afterCursor.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 6, afterCursor.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 7, queryLimit);

    rows.reserve((size_t)queryLimit);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        GlobalCoverWallRawRow row;
        row.cursorKey = safeGetText(stmt, 0);
        row.candidate.key = row.cursorKey;
        row.candidate.path = safeGetText(stmt, 1);
        row.candidate.sourcePath = safeGetText(stmt, 2);
        row.candidate.isEmbedded = sqlite3_column_int(stmt, 3) != 0;
        row.candidate.count = 1;
        if (!row.cursorKey.empty() && !row.candidate.path.empty()) {
            rows.push_back(std::move(row));
        }
    }
    sqlite3_finalize(stmt);
    return rows;
}

static std::vector<CoverFacetCandidate> LoadGlobalRegistryCoverCandidates(
    sqlite3* db,
    int queryLimit) {
    std::vector<CoverFacetCandidate> raw;
    if (!db || queryLimit <= 0) return raw;

    auto rows = LoadGlobalCoverWallRegistryRows(db, {}, queryLimit);
    raw.reserve(rows.size());
    for (auto& row : rows) {
        raw.push_back(std::move(row.candidate));
    }
    return raw;
}

static std::vector<Database::CoverFacetEntry> BuildCoverFacetEntriesFromCandidates(
    const std::vector<CoverFacetCandidate>& candidates,
    int limit) {
    std::vector<Database::CoverFacetEntry> results;
    auto clustered = ClusterCoverCandidates(candidates, limit, true);
    results.reserve(clustered.size());
    for (const auto& c : clustered) {
        Database::CoverFacetEntry e;
        e.key = c.key;
        e.path = c.path;
        e.sourcePath = c.sourcePath;
        e.isEmbedded = c.isEmbedded;
        e.count = c.count;
        results.push_back(std::move(e));
    }
    return results;
}

static std::vector<Database::CoverWallEntry> BuildCoverWallEntriesFromCandidates(
    const std::vector<CoverFacetCandidate>& candidates,
    int limit) {
    std::vector<Database::CoverWallEntry> results;
    auto clustered = ClusterCoverCandidates(candidates, limit, false);
    results.reserve(clustered.size());
    for (const auto& c : clustered) {
        Database::CoverWallEntry e;
        e.key = c.key;
        e.path = c.path;
        e.sourcePath = c.sourcePath;
        e.isEmbedded = c.isEmbedded;
        results.push_back(std::move(e));
    }
    return results;
}

static void PrewarmCoverTileMembershipForWallEntries(Database* db,
                                                     const std::vector<Database::CoverWallEntry>& entries,
                                                     int maxEntries = 24) {
    if (!db || !db->m_db || entries.empty() || maxEntries <= 0) return;
    if (sqlite3_db_readonly(db->m_db, "main") == 1) return;

    std::unordered_set<std::string> warmedKeys;
    warmedKeys.reserve((size_t)maxEntries * 4);

    const int limit = (std::min)(maxEntries, (int)entries.size());
    for (int i = 0; i < limit; ++i) {
        const std::string lookupKey = CanonicalizeCoverLookupKey(entries[(size_t)i].key);
        if (lookupKey.empty()) continue;

        std::vector<std::string> equivalentKeys;
        if (!db->getPreparedCoverFilterKeys(lookupKey, equivalentKeys)) {
            equivalentKeys = ResolveEquivalentCoverKeys(db, lookupKey);
        }
        if (equivalentKeys.empty()) equivalentKeys.push_back(lookupKey);

        std::vector<std::string> keysToWarm;
        keysToWarm.reserve(equivalentKeys.size());
        for (const auto& key : equivalentKeys) {
            if (!key.empty() && warmedKeys.insert(key).second) {
                keysToWarm.push_back(key);
            }
        }

        if (!keysToWarm.empty()) {
            EnsureCoverTileMembershipForKeys(db->m_db, keysToWarm);
        }

        db->setPreparedCoverFilterKeys(lookupKey, equivalentKeys);
        for (const auto& key : equivalentKeys) {
            db->setPreparedCoverFilterKeys(key, equivalentKeys);
        }
    }
}

static std::vector<GlobalCoverWallRawRow> MergeGlobalCoverWallRowsForPage(
    const std::vector<GlobalCoverWallRawRow>& registryRows,
    const std::vector<GlobalCoverWallRawRow>& supplementalRows,
    int limit,
    bool* outHasMore,
    std::string* outNextCursor,
    int* outSupplementalConsumed = nullptr) {
    if (outHasMore) *outHasMore = false;
    if (outNextCursor) outNextCursor->clear();
    if (outSupplementalConsumed) *outSupplementalConsumed = 0;

    std::vector<GlobalCoverWallRawRow> merged;
    if (limit <= 0) return merged;

    merged.reserve((size_t)(limit + 1));
    size_t registryIndex = 0;
    size_t supplementalIndex = 0;
    int consumed = 0;
    std::string lastConsumedCursor;

    while (registryIndex < registryRows.size() || supplementalIndex < supplementalRows.size()) {
        bool takeRegistry = false;
        if (supplementalIndex >= supplementalRows.size()) {
            takeRegistry = true;
        } else if (registryIndex >= registryRows.size()) {
            takeRegistry = false;
        } else {
            takeRegistry = registryRows[registryIndex].cursorKey <= supplementalRows[supplementalIndex].cursorKey;
        }

        const auto& picked = takeRegistry ? registryRows[registryIndex] : supplementalRows[supplementalIndex];
        if (consumed >= limit) {
            if (outHasMore) *outHasMore = true;
            break;
        }

        merged.push_back(picked);
        lastConsumedCursor = picked.cursorKey;
        ++consumed;
        if (!takeRegistry && outSupplementalConsumed) ++(*outSupplementalConsumed);

        if (takeRegistry) {
            ++registryIndex;
            while (supplementalIndex < supplementalRows.size() &&
                   supplementalRows[supplementalIndex].cursorKey == picked.cursorKey) {
                ++supplementalIndex;
            }
        } else {
            ++supplementalIndex;
            while (registryIndex < registryRows.size() &&
                   registryRows[registryIndex].cursorKey == picked.cursorKey) {
                ++registryIndex;
            }
        }
    }

    if (outHasMore && !*outHasMore) {
        *outHasMore =
            registryIndex < registryRows.size() ||
            supplementalIndex < supplementalRows.size();
    }
    if (outNextCursor && outHasMore && *outHasMore) {
        *outNextCursor = lastConsumedCursor;
    }

    return merged;
}

int Database::countCoverRegistryEntries() {
    if (!m_db) return 0;

    sqlite3_stmt* stmt = nullptr;
    int count = 0;
    if (sqlite3_prepare_v2(m_db, "SELECT COUNT(*) FROM cover_cache_registry;", -1, &stmt, nullptr) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            count = sqlite3_column_int(stmt, 0);
        }
        sqlite3_finalize(stmt);
    }
    return count;
}

int64_t Database::countAudioCoverPresenceCandidates() {
    return static_cast<int64_t>(CountAudioCoverPresenceCandidates(m_db));
}

int Database::pruneOrphanCoverCaches() {
    if (!m_db) return 0;
    if (sqlite3_db_readonly(m_db, "main") == 1) return 0;

    const char* deleteRegistrySql = R"SQL(
WITH live_cover_keys AS (
    SELECT embedded_cover_key AS cover_key
    FROM audio_files
    WHERE embedded_cover_key IS NOT NULL AND embedded_cover_key <> ''
    UNION
    SELECT folder_cover_key AS cover_key
    FROM audio_files
    WHERE folder_cover_key IS NOT NULL AND folder_cover_key <> ''
)
DELETE FROM cover_cache_registry
WHERE cover_key IS NULL
   OR cover_key = ''
   OR NOT EXISTS (
        SELECT 1
        FROM live_cover_keys
        WHERE live_cover_keys.cover_key = cover_cache_registry.cover_key
   );
)SQL";

    const char* deleteThumbSql = R"SQL(
WITH live_cover_keys AS (
    SELECT embedded_cover_key AS cover_key
    FROM audio_files
    WHERE embedded_cover_key IS NOT NULL AND embedded_cover_key <> ''
    UNION
    SELECT folder_cover_key AS cover_key
    FROM audio_files
    WHERE folder_cover_key IS NOT NULL AND folder_cover_key <> ''
)
DELETE FROM blobs_db.cover_thumbs
WHERE cover_key IS NULL
   OR cover_key = ''
   OR NOT EXISTS (
        SELECT 1
        FROM live_cover_keys
        WHERE live_cover_keys.cover_key = cover_thumbs.cover_key
   );
)SQL";

    int removed = 0;
    if (executeSQL(deleteRegistrySql)) {
        removed += sqlite3_changes(m_db);
    }
    if (executeSQL(deleteThumbSql)) {
        removed += sqlite3_changes(m_db);
    }

    if (removed > 0) {
        clearCoverKeyEquivalentCache();
        invalidateFacetQueryCache();
        invalidateFolderCoverFacetIndex();
        m_postProcessCoverRefreshNeeded.store(true, std::memory_order_release);
        DIAG_ACTION_FMT("Database", "PruneOrphanCoverCaches: removed=%d", removed);
    }

    return removed;
}

bool Database::isCoverRegistryStale() {
    const int candidates = countAudioCoverKeyCandidates();
    if (candidates <= 0) return false;

    const int registryRows = countCoverRegistryEntries();
    // Exact equality is not required because embedded and folder keys can collide,
    // but a registry that is orders of magnitude smaller is broken and must be rebuilt.
    const int minimumExpected = std::max(1, candidates / 2);
    return registryRows < minimumExpected;
}

bool Database::isCoverRegistryMaintenancePending() {
    const int candidates = countAudioCoverKeyCandidates();
    if (candidates <= 0) return false;
    return countCoverRegistryEntries() < candidates;
}

bool Database::isCoverTileMembershipMaintenancePending() {
    if (!m_db) return false;
    if (ReadAppMetaInt(m_db, "cover_tile_membership_needs_maintenance") == 1) {
        return true;
    }
    const int candidates = countAudioCoverKeyCandidates();
    if (candidates <= 0) return false;
    return CountRows64FromTable(m_db, "cover_tile_membership") == 0;
}

int Database::repairCoverTileMembershipBatch(int batchSize) {
    if (!m_db || batchSize <= 0 || sqlite3_db_readonly(m_db, "main") == 1) return 0;

    struct MembershipRepairRow {
        int64_t id = 0;
        std::string embeddedCoverKey;
        std::string folderCoverKey;
    };

    const char* sql = R"SQL(
SELECT id, embedded_cover_key, folder_cover_key
FROM audio_files
WHERE id > ?
  AND (
      (embedded_cover_key IS NOT NULL AND embedded_cover_key <> ''
       AND NOT EXISTS (
           SELECT 1 FROM cover_tile_membership m
           WHERE m.tile_key = audio_files.embedded_cover_key
             AND m.file_id = audio_files.id
           LIMIT 1
       ))
   OR (folder_cover_key IS NOT NULL AND folder_cover_key <> ''
       AND NOT EXISTS (
           SELECT 1 FROM cover_tile_membership m
           WHERE m.tile_key = audio_files.folder_cover_key
             AND m.file_id = audio_files.id
           LIMIT 1
       ))
  )
ORDER BY id
LIMIT ?;
)SQL";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return 0;
    }

    sqlite3_bind_int64(stmt, 1, m_coverTileMembershipRepairCursorId);
    sqlite3_bind_int(stmt, 2, batchSize);

    std::vector<MembershipRepairRow> rows;
    rows.reserve((size_t)batchSize);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        MembershipRepairRow row;
        row.id = sqlite3_column_int64(stmt, 0);
        row.embeddedCoverKey = safeGetText(stmt, 1);
        row.folderCoverKey = safeGetText(stmt, 2);
        rows.push_back(std::move(row));
    }
    sqlite3_finalize(stmt);

    if (rows.empty()) {
        m_coverTileMembershipRepairCursorId = 0;
        WriteAppMetaInt(m_db, "cover_tile_membership_needs_maintenance", 0);
        return 0;
    }

    std::vector<std::string> keys;
    keys.reserve(rows.size() * 2);
    for (const auto& row : rows) {
        if (!row.embeddedCoverKey.empty()) keys.push_back(row.embeddedCoverKey);
        if (!row.folderCoverKey.empty()) keys.push_back(row.folderCoverKey);
    }

    m_coverTileMembershipRepairCursorId = rows.back().id;
    const int repaired = SyncCoverTileMembershipForKeys(m_db, keys);
    WriteAppMetaInt(m_db, "cover_tile_membership_needs_maintenance", 1);
    DIAG_ACTION_FMT("Database",
                    "CoverTileMembershipRepairBatch: rows=%d keys=%d repaired=%d cursor=%lld",
                    (int)rows.size(),
                    (int)keys.size(),
                    repaired,
                    (long long)m_coverTileMembershipRepairCursorId);
    return repaired;
}

int Database::repairSparseCoverRegistryBatch(int batchSize) {
    if (!m_db || batchSize <= 0 || sqlite3_db_readonly(m_db, "main") == 1) return 0;

    struct RegistryRepairRow {
        int64_t id = 0;
        std::string filePath;
        std::string folderPath;
        std::string embeddedCoverKey;
        std::string folderCoverKey;
    };

    const char* sql = R"SQL(
SELECT id, file_path, folder_path, embedded_cover_key, folder_cover_key
FROM audio_files
WHERE id > ?
  AND (
      (embedded_cover_key IS NOT NULL AND embedded_cover_key <> '')
   OR (folder_cover_key IS NOT NULL AND folder_cover_key <> '')
  )
ORDER BY id
LIMIT ?;
)SQL";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return 0;
    }

    sqlite3_bind_int64(stmt, 1, m_coverRegistryRepairCursorId);
    sqlite3_bind_int(stmt, 2, batchSize);

    std::vector<RegistryRepairRow> rows;
    rows.reserve((size_t)batchSize);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        RegistryRepairRow row;
        row.id = sqlite3_column_int64(stmt, 0);
        row.filePath = safeGetText(stmt, 1);
        row.folderPath = safeGetText(stmt, 2);
        row.embeddedCoverKey = safeGetText(stmt, 3);
        row.folderCoverKey = safeGetText(stmt, 4);
        rows.push_back(std::move(row));
    }
    sqlite3_finalize(stmt);

    if (rows.empty()) {
        m_coverRegistryRepairCursorId = 0;
        const bool maintenancePending = isCoverRegistryMaintenancePending();
        WriteAppMetaInt(m_db, "cover_registry_needs_maintenance", maintenancePending ? 1 : 0);
        if (!maintenancePending) {
            DIAG_ACTION("Database", "Cover registry incremental repair completed.");
        }
        return 0;
    }

    std::vector<CoverRegistryDelta> deltas;
    deltas.reserve(rows.size() * 2);
    for (const auto& row : rows) {
        if (!row.embeddedCoverKey.empty() && !row.filePath.empty()) {
            deltas.push_back({row.embeddedCoverKey, row.filePath, true});
        }
        if (!row.folderCoverKey.empty() && !row.folderPath.empty()) {
            deltas.push_back({row.folderCoverKey, HarkenSearch::CanonicalFolderPath(row.folderPath), false});
        }
    }

    m_coverRegistryRepairCursorId = rows.back().id;
    const int finalized = finalizeCoverRegistryDeltas(deltas);
    const bool maintenancePending = isCoverRegistryMaintenancePending();
    if (!maintenancePending) {
        m_coverRegistryRepairCursorId = 0;
    }
    WriteAppMetaInt(m_db, "cover_registry_needs_maintenance", maintenancePending ? 1 : 0);
    if (finalized > 0) {
        m_postProcessCoverRefreshNeeded.store(true, std::memory_order_release);
        DIAG_ACTION_FMT("Database",
                        "CoverRegistryRepairBatch: rows=%d deltas=%d finalized=%d pending=%d cursor=%lld",
                        (int)rows.size(),
                        (int)deltas.size(),
                        finalized,
                        maintenancePending ? 1 : 0,
                        (long long)m_coverRegistryRepairCursorId);
    }
    return finalized;
}

static int CoverPhashHammingDistance(int64_t a, int64_t b) {
    uint64_t v = static_cast<uint64_t>(a) ^ static_cast<uint64_t>(b);
    int count = 0;
    while (v) {
        v &= (v - 1);
        ++count;
    }
    return count;
}

static bool CoverPhashSimilar(int64_t a, int64_t b) {
    if (a == 0 || b == 0) return false;
    return CoverPhashHammingDistance(a, b) <= 2;
}

static std::string CanonicalizeCoverLookupKey(const std::string& coverKey) {
    std::string canonical = CoverModule::CanonicalCoverKey(coverKey);
    if (canonical.empty()) canonical = coverKey;
    return canonical;
}

static std::vector<std::string> ResolveEquivalentCoverKeys(Database* db, const std::string& coverKey) {
    auto t0 = std::chrono::steady_clock::now();
    std::vector<std::string> out;
    if (!db || coverKey.empty()) return out;

    // Check cache first to avoid expensive full-table scan
    if (db->getCoverKeyEquivalentFromCache(coverKey, out)) {
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();
        fprintf(stderr, "[ResolveEquivalentCoverKeys] CACHE HIT: %s (%d keys, %.2f ms)\n",
                coverKey.c_str(), (int)out.size(), (double)ms);
        return out;
    }

    sqlite3* sqlite = db->m_db;
    auto addUnique = [&out](const std::string& key) {
        if (key.empty()) return;
        if (std::find(out.begin(), out.end(), key) == out.end()) out.push_back(key);
    };
    auto cacheEquivalentKeys = [&](const std::vector<std::string>& keys) {
        if (keys.empty()) return;
        const std::string canonicalKey = CanonicalizeCoverLookupKey(coverKey);
        db->setCoverKeyEquivalentInCache(coverKey, keys);
        if (!canonicalKey.empty() && canonicalKey != coverKey) {
            db->setCoverKeyEquivalentInCache(canonicalKey, keys);
        }
        for (const auto& key : keys) {
            db->setCoverKeyEquivalentInCache(key, keys);
        }
    };

    std::vector<int64_t> targetPhashes;
    double targetLookupMs = 0.0;
    double phashLookupMs = 0.0;
    size_t phashComparisonTargets = 0;
    int phashQueryChunks = 0;
    sqlite3_stmt* stmt = nullptr;
    const char* targetSql =
        "SELECT cover_key, normalized_key, cover_phash "
        "FROM cover_cache_registry "
        "WHERE cover_key = ? OR normalized_key = ?;";
    auto tTarget = std::chrono::steady_clock::now();
    if (sqlite3_prepare_v2(sqlite, targetSql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, coverKey.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, coverKey.c_str(), -1, SQLITE_TRANSIENT);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            addUnique(safeGetText(stmt, 0));
            addUnique(safeGetText(stmt, 1));
            const int64_t phash = sqlite3_column_int64(stmt, 2);
            if (phash != 0 &&
                std::find(targetPhashes.begin(), targetPhashes.end(), phash) == targetPhashes.end()) {
                targetPhashes.push_back(phash);
            }
        }
    }
    if (stmt) sqlite3_finalize(stmt);
    targetLookupMs = MsSince(tTarget);
    addUnique(coverKey);

    if (!targetPhashes.empty()) {
        auto tPhash = std::chrono::steady_clock::now();
        std::sort(targetPhashes.begin(), targetPhashes.end());
        targetPhashes.erase(std::unique(targetPhashes.begin(), targetPhashes.end()),
                            targetPhashes.end());
        phashComparisonTargets = targetPhashes.size();

        if (!targetPhashes.empty()) {
            ++phashQueryChunks;
            std::string sql =
                "SELECT cover_key, normalized_key, cover_phash "
                "FROM cover_cache_registry "
                "WHERE cover_phash != 0 AND (";
            for (size_t i = 0; i < targetPhashes.size(); ++i) {
                if (i > 0) sql += " OR ";
                sql += "phash_hamming(cover_phash, ?) <= 2";
            }
            sql += ");";

            stmt = nullptr;
            if (sqlite3_prepare_v2(sqlite, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
                for (size_t i = 0; i < targetPhashes.size(); ++i) {
                    sqlite3_bind_int64(stmt, static_cast<int>(i) + 1, targetPhashes[i]);
                }
                while (sqlite3_step(stmt) == SQLITE_ROW) {
                    addUnique(safeGetText(stmt, 0));
                    addUnique(safeGetText(stmt, 1));
                }
            }
            if (stmt) sqlite3_finalize(stmt);
        }
        phashLookupMs = MsSince(tPhash);
    }

    // Store result in cache for future lookups across all equivalent aliases.
    cacheEquivalentKeys(out);
    const double totalMs = MsSince(t0);
    {
        std::ostringstream oss;
        oss << "[CoverFilterTrace] equivalentKeys cache=miss targetMs=" << targetLookupMs
            << " phashMs=" << phashLookupMs
            << " totalMs=" << totalMs
            << " targetPhashes=" << targetPhashes.size()
            << " phashCompareTargets=" << phashComparisonTargets
            << " chunks=" << phashQueryChunks
            << " keys=" << out.size()
            << " keyPrefix=" << coverKey.substr(0, (std::min)(size_t(16), coverKey.size()));
        AppendCoverPerfLog(oss.str());
    }
fprintf(stderr, "[ResolveEquivalentCoverKeys] CACHE MISS (indexed phash): %s (%d keys, %.2f ms)\n",
        coverKey.c_str(), (int)out.size(), totalMs);
    return out;
}

static bool TryExtractTemporaryFolderIdentityPath(const std::string& coverKey,
                                                  std::string* outCanonicalFolderPath) {
    if (outCanonicalFolderPath) outCanonicalFolderPath->clear();
    if (coverKey.rfind("?|", 0) != 0) return false;
    const std::string rawPath = coverKey.substr(2);
    if (rawPath.empty()) return false;
    const std::string canonicalPath = HarkenSearch::CanonicalFolderPath(rawPath);
    if (canonicalPath.empty()) return false;
    if (outCanonicalFolderPath) *outCanonicalFolderPath = canonicalPath;
    return true;
}

static bool TryExtractCanonicalFolderCoverSourcePath(const std::string& coverKey,
                                                     std::string* outCanonicalSourcePath) {
    if (outCanonicalSourcePath) outCanonicalSourcePath->clear();
    if (coverKey.rfind("F|PATH|", 0) != 0) return false;
    std::string rest = coverKey.substr(7);
    if (rest.empty()) return false;
    size_t pipe = rest.find('|');
    std::string pathPart = (pipe != std::string::npos) ? rest.substr(0, pipe) : rest;
    if (pathPart.empty()) return false;
    if (outCanonicalSourcePath) *outCanonicalSourcePath = pathPart;
    return true;
}

static bool IsBetterCoverRepresentative(const CoverFacetCandidate& candidate,
                                        const CoverFacetCandidate& current) {
    if (current.key.empty()) return true;
    if (candidate.count != current.count) return candidate.count > current.count;
    if (candidate.isEmbedded != current.isEmbedded) return !candidate.isEmbedded;
    return candidate.key < current.key;
}

static bool CoverCandidateMatchesCluster(const CoverFacetCandidate& candidate,
                                         const CoverFacetCluster& cluster) {
    for (const auto& key : cluster.keys) {
        if (candidate.key == key) return true;
    }
    if (candidate.phash == 0) return false;
    for (int64_t phash : cluster.phashes) {
        if (CoverPhashSimilar(candidate.phash, phash)) return true;
    }
    return false;
}

static std::vector<CoverFacetCandidate> ClusterCoverCandidates(
    const std::vector<CoverFacetCandidate>& candidates,
    int limit,
    bool sortByCount,
    bool mergeSimilarPhashes) {
    std::vector<CoverFacetCluster> clusters;
    clusters.reserve(candidates.size());
    std::unordered_map<std::string, size_t> exactKeyIndex;
    if (!mergeSimilarPhashes) exactKeyIndex.reserve(candidates.size());

    for (const auto& candidate : candidates) {
        if (candidate.key.empty() || candidate.path.empty()) continue;

        CoverFacetCluster* target = nullptr;
        if (!mergeSimilarPhashes) {
            const auto found = exactKeyIndex.find(candidate.key);
            if (found != exactKeyIndex.end()) target = &clusters[found->second];
        } else {
            for (auto& cluster : clusters) {
                if (CoverCandidateMatchesCluster(candidate, cluster)) {
                    target = &cluster;
                    break;
                }
            }
        }

        if (!target) {
            CoverFacetCluster cluster;
            cluster.representative = candidate;
            cluster.totalCount = candidate.count;
            cluster.keys.push_back(candidate.key);
            if (candidate.phash != 0) cluster.phashes.push_back(candidate.phash);
            if (!mergeSimilarPhashes) exactKeyIndex.emplace(candidate.key, clusters.size());
            clusters.push_back(std::move(cluster));
            continue;
        }

        target->totalCount += candidate.count;
        target->keys.push_back(candidate.key);
        if (candidate.phash != 0 &&
            std::find(target->phashes.begin(), target->phashes.end(), candidate.phash) == target->phashes.end()) {
            target->phashes.push_back(candidate.phash);
        }
        if (IsBetterCoverRepresentative(candidate, target->representative)) {
            target->representative = candidate;
        }
    }

    std::vector<CoverFacetCandidate> out;
    out.reserve(clusters.size());
    for (auto& cluster : clusters) {
        cluster.representative.count = cluster.totalCount;
        out.push_back(std::move(cluster.representative));
    }

    std::sort(out.begin(), out.end(), [sortByCount](const CoverFacetCandidate& a,
                                                    const CoverFacetCandidate& b) {
        if (sortByCount && a.count != b.count) return a.count > b.count;
        if (a.isEmbedded != b.isEmbedded) return a.isEmbedded > b.isEmbedded;
        return a.key < b.key;
    });
    if (limit > 0 && (int)out.size() > limit) out.resize((size_t)limit);
    return out;
}

int Database::normalizeFolderCoverKeysByVisualIdentity(int maxDistinctCovers) {
    if (!m_db || maxDistinctCovers <= 0) return 0;

    const char* sql = R"SQL(
SELECT folder_cover_src_path, MIN(folder_cover_key)
FROM audio_files
WHERE folder_cover_src_path IS NOT NULL
  AND folder_cover_src_path <> ''
  AND folder_cover_key IS NOT NULL
  AND folder_cover_key <> ''
  AND folder_cover_key NOT LIKE 'F|H|%'
GROUP BY folder_cover_src_path
LIMIT ?;
)SQL";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return 0;
    }
    sqlite3_bind_int(stmt, 1, maxDistinctCovers);

    struct CoverKeyFix {
        std::string srcPath;
        std::string oldKey;
        std::string newKey;
    };
    std::vector<CoverKeyFix> fixes;
    fixes.reserve(256);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        CoverKeyFix fix;
        fix.srcPath = safeGetText(stmt, 0);
        fix.oldKey = safeGetText(stmt, 1);
        if (fix.srcPath.empty()) continue;

        fix.newKey = CoverModule::NormalizeCoverKeyByVisualIdentity(fix.srcPath);
        if (!fix.newKey.empty() && fix.newKey != fix.oldKey) {
            fixes.push_back(std::move(fix));
        }
    }
    sqlite3_finalize(stmt);
    if (fixes.empty()) return 0;

    const char* updateSql = R"SQL(
UPDATE audio_files
SET folder_cover_key = ?
WHERE folder_cover_src_path = ?
  AND folder_cover_key IS NOT NULL
  AND folder_cover_key <> ''
  AND folder_cover_key NOT LIKE 'F|H|%';
)SQL";

    sqlite3_stmt* updateStmt = nullptr;
    if (sqlite3_prepare_v2(m_db, updateSql, -1, &updateStmt, nullptr) != SQLITE_OK) {
        return 0;
    }

    beginTransaction();
    int updatedRows = 0;
    for (const auto& fix : fixes) {
        sqlite3_reset(updateStmt);
        sqlite3_clear_bindings(updateStmt);
        sqlite3_bind_text(updateStmt, 1, fix.newKey.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(updateStmt, 2, fix.srcPath.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(updateStmt) == SQLITE_DONE) {
            updatedRows += sqlite3_changes(m_db);
        }
    }
    sqlite3_finalize(updateStmt);
    commitTransaction();

    if (updatedRows > 0) {
        invalidateFacetQueryCache();
        invalidateFolderCoverFacetIndex();
        DIAG_ACTION_FMT("Database", "NormalizedFolderCoverKeys: distinct=%d rows=%d",
            (int)fixes.size(), updatedRows);
    }
    return updatedRows;
}

int Database::finalizeCoverRegistryDeltas(const std::vector<CoverRegistryDelta>& deltas,
                                          bool deferFacetInvalidate) {
    if (!m_db || deltas.empty()) return 0;

    struct PendingCover {
        std::string key;
        std::string repPath;
        bool isEmbedded = false;
        std::string normalizedKey;
    };

    std::unordered_map<std::string, PendingCover> pendingByKey;
    pendingByKey.reserve(deltas.size());
    for (const auto& delta : deltas) {
        if (delta.coverKey.empty() || delta.repPath.empty()) continue;
        const std::string normalizedKey = CoverModule::CanonicalCoverKey(delta.coverKey);
        if (normalizedKey.empty()) continue;
        PendingCover row;
        // Keep the source alias as the registry key and put the canonical
        // identity in normalized_key.  Collapsing both here loses the alias
        // needed to build cover_tile_membership for mixed legacy/canonical
        // libraries, even though the registry schema explicitly carries both.
        row.key = delta.coverKey;
        row.repPath = delta.repPath;
        row.isEmbedded = delta.isEmbedded;
        row.normalizedKey = normalizedKey;
        pendingByKey.insert_or_assign(row.key, std::move(row));
    }
    if (pendingByKey.empty()) return 0;

    const char* upsertSql = R"SQL(
INSERT INTO cover_cache_registry (cover_key, rep_path, is_embedded, normalized_key)
VALUES (?, ?, ?, ?)
ON CONFLICT(cover_key) DO UPDATE SET
    rep_path = excluded.rep_path,
    is_embedded = excluded.is_embedded,
    normalized_key = CASE
        WHEN excluded.normalized_key IS NOT NULL AND excluded.normalized_key <> ''
        THEN excluded.normalized_key
        ELSE cover_cache_registry.normalized_key
    END;
)SQL";
    sqlite3_stmt* upsertStmt = nullptr;
    if (sqlite3_prepare_v2(m_db, upsertSql, -1, &upsertStmt, nullptr) != SQLITE_OK) {
        return 0;
    }

    const char* phashSql =
        "UPDATE cover_cache_registry SET cover_phash = ?, cover_phash_version = 2 WHERE cover_key = ?";
    sqlite3_stmt* phashStmt = nullptr;
    sqlite3_prepare_v2(m_db, phashSql, -1, &phashStmt, nullptr);

    const auto t0 = std::chrono::steady_clock::now();
    int finalized = 0;
    int phashUpdated = 0;
    if (!beginTransaction()) {
        sqlite3_finalize(upsertStmt);
        if (phashStmt) sqlite3_finalize(phashStmt);
        return 0;
    }
    for (const auto& kv : pendingByKey) {
        const PendingCover& row = kv.second;
        sqlite3_reset(upsertStmt);
        sqlite3_clear_bindings(upsertStmt);
        sqlite3_bind_text(upsertStmt, 1, row.key.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(upsertStmt, 2, row.repPath.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(upsertStmt, 3, row.isEmbedded ? 1 : 0);
        sqlite3_bind_text(upsertStmt, 4, row.normalizedKey.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(upsertStmt) != SQLITE_DONE) continue;
        finalized++;

        if (!phashStmt) continue;
        std::vector<uint8_t> thumbData;
        std::string mime;
        if (!getCoverThumb(row.key, thumbData, mime) || thumbData.size() <= 16) continue;
        const uint64_t phash = CoverModule::ComputePerceptualHash(thumbData.data(), thumbData.size());
        if (phash == 0) continue;
        sqlite3_reset(phashStmt);
        sqlite3_clear_bindings(phashStmt);
        sqlite3_bind_int64(phashStmt, 1, (int64_t)phash);
        sqlite3_bind_text(phashStmt, 2, row.key.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(phashStmt) == SQLITE_DONE) phashUpdated++;
    }
    std::vector<std::string> finalizedKeys;
    finalizedKeys.reserve(pendingByKey.size());
    for (const auto& kv : pendingByKey) {
        finalizedKeys.push_back(kv.first);
    }
    const auto tBeforeSync = std::chrono::steady_clock::now();
    SyncCoverTileMembershipForKeys(m_db, finalizedKeys);
    const auto tAfterSync = std::chrono::steady_clock::now();
    sqlite3_finalize(upsertStmt);
    if (phashStmt) sqlite3_finalize(phashStmt);
    if (!commitTransaction()) {
        return 0;
    }
    const auto tAfterCommit = std::chrono::steady_clock::now();

    if (finalized > 0) {
        // P1: SCOPED invalidate over only the repPaths touched by this
        // batch instead of nuking the entire folder facet index. Cost
        // drops from O(rows-in-3-index-tables) to O(unique-folder-scopes-of-batch).
        // Microbench at 30k files: full-table 14ms -> scoped <2ms expected.
        std::vector<std::string> affectedPaths;
        affectedPaths.reserve(pendingByKey.size());
        for (const auto& kv : pendingByKey) {
            if (!kv.second.repPath.empty()) {
                affectedPaths.push_back(kv.second.repPath);
            }
        }
        if (!affectedPaths.empty()) {
            invalidateFolderCoverFacetIndexForPaths(affectedPaths);
        } else {
            // Defensive fallback: shouldn't reach here because deltas.empty()
            // bails earlier, but keep behavior identical if it ever did.
            invalidateFolderCoverFacetIndex();
        }

        // P2: Defer the (full-table) facet_query_cache wipe when caller
        // promises to coalesce. BackfillWorkerPool's hot path opts in.
        if (!deferFacetInvalidate) {
            invalidateFacetQueryCache();
        }
    }

    const auto tEnd = std::chrono::steady_clock::now();
    const auto elapsedMs = std::chrono::duration<double, std::milli>(tEnd - t0).count();
    const auto loopMs = std::chrono::duration<double, std::milli>(tBeforeSync - t0).count();
    const auto syncMs = std::chrono::duration<double, std::milli>(tAfterSync - tBeforeSync).count();
    const auto commitMs = std::chrono::duration<double, std::milli>(tAfterCommit - tAfterSync).count();
    const auto invMs = std::chrono::duration<double, std::milli>(tEnd - tAfterCommit).count();
    DIAG_ACTION_FMT("Scan.CoverDedup",
        "candidates=%d unique=%d duplicates=%d phash=%d durationMs=%.2f loop=%.2f sync=%.2f commit=%.2f inv=%.2f",
        (int)deltas.size(), (int)pendingByKey.size(),
        (int)deltas.size() - (int)pendingByKey.size(), phashUpdated, elapsedMs,
        loopMs, syncMs, commitMs, invMs);

    return finalized;
}

void Database::upsertCoverRegistry(const std::string& coverKey, const std::string& repPath, bool isEmbedded, const std::string& normalizedKey) {
    if (!m_db || coverKey.empty()) return;

    const char* sql = "INSERT OR REPLACE INTO cover_cache_registry (cover_key, rep_path, is_embedded, normalized_key) VALUES (?, ?, ?, ?);";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, coverKey.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, repPath.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 3, isEmbedded ? 1 : 0);
        sqlite3_bind_text(stmt, 4, normalizedKey.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
    SyncCoverTileMembershipForKeys(m_db, std::vector<std::string>{coverKey});
}

// Remove registry rows no longer referenced by audio_files.
void Database::removeCoverRegistryIfOrphan(const std::string& coverKey) {
    if (!m_db || coverKey.empty()) return;

    const char* sql = R"SQL(
DELETE FROM cover_cache_registry
WHERE cover_key = ?
  AND ? NOT IN (
      SELECT embedded_cover_key FROM audio_files
      WHERE embedded_cover_key = ?
      LIMIT 1
  )
  AND ? NOT IN (
      SELECT folder_cover_key FROM audio_files
      WHERE folder_cover_key = ?
      LIMIT 1
  );
)SQL";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, coverKey.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, coverKey.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, coverKey.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 4, coverKey.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 5, coverKey.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
    const char* membershipSql = R"SQL(
DELETE FROM cover_tile_membership
WHERE tile_key = ?
  AND NOT EXISTS (
      SELECT 1 FROM audio_files WHERE embedded_cover_key = ? LIMIT 1
  )
  AND NOT EXISTS (
      SELECT 1 FROM audio_files WHERE folder_cover_key = ? LIMIT 1
  );
)SQL";
    sqlite3_stmt* membershipStmt = nullptr;
    if (sqlite3_prepare_v2(m_db, membershipSql, -1, &membershipStmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(membershipStmt, 1, coverKey.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(membershipStmt, 2, coverKey.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(membershipStmt, 3, coverKey.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(membershipStmt);
        sqlite3_finalize(membershipStmt);
    }
}

void Database::backfillCoverPhash() {
    if (!m_db) return;
    DIAG_SCOPED("Database.backfillCoverPhash");

    // Select missing, v1 aHash, or empty hashes for dHash backfill.
    const char* sql = "SELECT r.cover_key FROM cover_cache_registry r "
                      "INNER JOIN blobs_db.cover_thumbs t ON t.cover_key = r.cover_key "
                      "WHERE r.cover_phash = 0 OR r.cover_phash IS NULL OR r.cover_phash_version < 2";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) return;

    std::vector<std::string> keys;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        keys.push_back(safeGetText(stmt, 0));
    }
    sqlite3_finalize(stmt);

    if (keys.empty()) return;

    const char* upSql = "UPDATE cover_cache_registry SET cover_phash = ?, cover_phash_version = 2 WHERE cover_key = ?";
    sqlite3_stmt* upStmt = nullptr;
    if (sqlite3_prepare_v2(m_db, upSql, -1, &upStmt, nullptr) != SQLITE_OK) return;

    std::unique_lock<std::mutex> writerLk(m_writerSerializeMutex);
    if (!executeSQL("BEGIN TRANSACTION")) {
        sqlite3_finalize(upStmt);
        return;
    }
    int updated = 0;
    for (const auto& key : keys) {
        std::vector<uint8_t> thumbData;
        std::string mime;
        if (getCoverThumb(key, thumbData, mime) && thumbData.size() > 16) {
            uint64_t phash = CoverModule::ComputePerceptualHash(thumbData.data(), thumbData.size());
            if (phash != 0) {
                sqlite3_bind_int64(upStmt, 1, (int64_t)phash);
                sqlite3_bind_text(upStmt, 2, key.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_step(upStmt);
                sqlite3_reset(upStmt);
                updated++;
            }
        }
    }
    sqlite3_finalize(upStmt);
    executeSQL("COMMIT");

    // 清除 facet 缓存，让新的 phash 分组查询生效
    if (updated > 0)
        executeSQL("DELETE FROM facet_query_cache WHERE cache_key LIKE '%cover%'");

    DIAG_ACTION_FMT("Database", "BackfillCoverPhash: total=%d updated=%d", (int)keys.size(), updated);
}

// -- Gradual cover dHash backfill worker (independent of heavy maintenance) ----

static constexpr size_t kCoverThumbRawFallbackMaxBytes = 256 * 1024;
static const char* kCoverThumbRepairEligibleWhereSql =
    "r.cover_key IS NOT NULL "
    "AND r.cover_key <> '' "
    "AND t.cover_key IS NULL "
    "AND COALESCE(r.cover_phash_version, 0) <> -1";

static int CountMissingCoverThumbRowsForMaintenance(sqlite3* db) {
    if (!db) return 0;

    const std::string sql = std::string(
        "SELECT COUNT(*) "
        "FROM cover_cache_registry r "
        "LEFT JOIN blobs_db.cover_thumbs t ON t.cover_key = r.cover_key "
        "WHERE ") + kCoverThumbRepairEligibleWhereSql;

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        return 0;
    }

    int count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        count = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return count;
}

// close() joins this worker before clearing m_db, so lock-free reads are safe.

int Database::repairMissingCoverThumbsBatch(int batchSize) {
    if (!m_db || batchSize <= 0) return 0;

    const std::string sql = std::string(
        "SELECT r.cover_key, r.rep_path, r.is_embedded "
        "FROM cover_cache_registry r "
        "LEFT JOIN blobs_db.cover_thumbs t ON t.cover_key = r.cover_key "
        "WHERE ") + kCoverThumbRepairEligibleWhereSql +
        " ORDER BY r.is_embedded DESC, r.cover_key "
        "LIMIT ?";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) return 0;
    sqlite3_bind_int(stmt, 1, batchSize);

    struct MissingCoverThumb {
        std::string key;
        std::string repPath;
        bool isEmbedded = false;
    };
    std::vector<MissingCoverThumb> rows;
    rows.reserve((size_t)batchSize);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        MissingCoverThumb row;
        row.key = safeGetText(stmt, 0);
        row.repPath = safeGetText(stmt, 1);
        row.isEmbedded = sqlite3_column_int(stmt, 2) != 0;
        rows.push_back(std::move(row));
    }
    sqlite3_finalize(stmt);
    if (rows.empty()) {
        const int remainingMissingThumbRows = CountMissingCoverThumbRowsForMaintenance(m_db);
        const bool maintenancePending =
            isCoverRegistryMaintenancePending() || remainingMissingThumbRows > 0;
        WriteAppMetaInt(m_db, "cover_registry_needs_maintenance", maintenancePending ? 1 : 0);
        return 0;
    }

    struct PreparedCoverThumb {
        std::string key;
        std::vector<uint8_t> imageData;
        std::string mime;
        bool shouldWrite = false;
    };
    std::vector<PreparedCoverThumb> prepared;
    prepared.reserve(rows.size());

    const char* failSql =
        "UPDATE cover_cache_registry "
        "SET cover_phash = 0, cover_phash_version = -1 "
        "WHERE cover_key = ?";
    sqlite3_stmt* failStmt = nullptr;
    sqlite3_prepare_v2(m_db, failSql, -1, &failStmt, nullptr);

    int repaired = 0, failed = 0;
    for (const auto& row : rows) {
        if (m_coverPhashBackfillCancel.load(std::memory_order_acquire)) break;

        AlbumArt art;
        bool found = false;
        if (row.isEmbedded && !row.repPath.empty()) {
            found = MetadataReader::readAlbumArt(row.repPath, art) && art.hasData();
        } else if (!row.repPath.empty()) {
            std::string coverPath;
            if (CoverModule::GetFolderCoverImagePathBackground(row.repPath, coverPath, true, true) &&
                !coverPath.empty()) {
                Platform::FileSystem::StreamReader reader;
                if (reader.Open(coverPath, true) &&
                    reader.Seek(0, Platform::FileSystem::StreamSeekOrigin::End)) {
                    const int64_t size = reader.Tell();
                    if (size > 16 && size <= 32 * 1024 * 1024 &&
                        reader.Seek(0, Platform::FileSystem::StreamSeekOrigin::Begin)) {
                        art.data.resize((size_t)size);
                        const size_t bytesRead = reader.Read(art.data.data(), art.data.size());
                        std::string lower = coverPath;
                        std::transform(lower.begin(), lower.end(), lower.begin(),
                                       [](unsigned char c) { return (char)std::tolower(c); });
                        art.mimeType = lower.size() >= 4 && lower.rfind(".png") == lower.size() - 4
                            ? "image/png"
                            : "image/jpeg";
                        found = bytesRead == art.data.size() && art.hasData();
                    }
                }
            }
        }

        PreparedCoverThumb entry;
        entry.key = row.key;
        if (found && art.hasData()) {
            if (CoverModule::PrepareCoverThumbForStorage(art.data, entry.imageData, entry.mime) &&
                !entry.imageData.empty()) {
                entry.shouldWrite = true;
            } else if (!art.data.empty() &&
                       art.data.size() <= kCoverThumbRawFallbackMaxBytes) {
                entry.imageData = std::move(art.data);
                entry.mime = art.mimeType.empty() ? "image/jpeg" : art.mimeType;
                entry.shouldWrite = true;
            }
        }
        prepared.push_back(std::move(entry));
    }

    bool commitOk = false;
    {
        std::unique_lock<std::mutex> writerLk(m_writerSerializeMutex);
        if (!executeSQL("BEGIN TRANSACTION")) {
            if (failStmt) sqlite3_finalize(failStmt);
            return 0;
        }
        for (const auto& entry : prepared) {
            bool saved = false;
            if (entry.shouldWrite && !entry.imageData.empty()) {
                saved = saveCoverThumb(entry.key, entry.imageData, entry.mime);
            }
            if (saved) {
                repaired++;
                continue;
            }

            failed++;
            if (failStmt) {
                sqlite3_bind_text(failStmt, 1, entry.key.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_step(failStmt);
                sqlite3_reset(failStmt);
            }
        }
        if (failStmt) sqlite3_finalize(failStmt);
        commitOk = executeSQL("COMMIT");
        if (!commitOk) {
            executeSQL("ROLLBACK");
            repaired = 0;
            failed = std::max(failed, (int)prepared.size());
        }
    }

    const int remainingMissingThumbRows = CountMissingCoverThumbRowsForMaintenance(m_db);
    const bool maintenancePending =
        isCoverRegistryMaintenancePending() || remainingMissingThumbRows > 0;
    WriteAppMetaInt(m_db, "cover_registry_needs_maintenance", maintenancePending ? 1 : 0);
    if (!commitOk) {
        DIAG_ACTION("Database", "CoverThumbBakeCommitFailed");
    }
    if (repaired > 0 || failed > 0) {
        DIAG_ACTION_FMT("Database",
                        "CoverThumbBakeProgress: rows=%d repaired=%d failed=%d remaining=%d pending=%d commit=%d",
                        (int)rows.size(),
                        repaired,
                        failed,
                        remainingMissingThumbRows,
                        maintenancePending ? 1 : 0,
                        commitOk ? 1 : 0);
    }
    return repaired;
}

int Database::backfillCoverPhashBatch(int batchSize) {
    if (!m_db || batchSize <= 0) return 0;

    const char* sql = "SELECT r.cover_key FROM cover_cache_registry r "
                      "INNER JOIN blobs_db.cover_thumbs t ON t.cover_key = r.cover_key "
                      "WHERE r.cover_phash_version < 2 OR r.cover_phash_version IS NULL "
                      "LIMIT ?";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) return 0;
    sqlite3_bind_int(stmt, 1, batchSize);

    std::vector<std::string> keys;
    keys.reserve((size_t)batchSize);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        keys.push_back(safeGetText(stmt, 0));
    }
    sqlite3_finalize(stmt);
    if (keys.empty()) return 0;

    const char* upSql = "UPDATE cover_cache_registry SET cover_phash = ?, cover_phash_version = 2 WHERE cover_key = ?";
    sqlite3_stmt* upStmt = nullptr;
    if (sqlite3_prepare_v2(m_db, upSql, -1, &upStmt, nullptr) != SQLITE_OK) return 0;

    std::unique_lock<std::mutex> writerLk(m_writerSerializeMutex);
    if (!executeSQL("BEGIN TRANSACTION")) {
        sqlite3_finalize(upStmt);
        return 0;
    }
    int updated = 0, noThumb = 0, hashZero = 0;
    for (const auto& key : keys) {
        if (m_coverPhashBackfillCancel.load(std::memory_order_acquire)) break;
        std::vector<uint8_t> thumbData;
        std::string mime;
        if (!getCoverThumb(key, thumbData, mime) || thumbData.size() <= 16) { noThumb++; continue; }
        uint64_t phash = CoverModule::ComputePerceptualHash(thumbData.data(), thumbData.size());
        if (phash == 0) {
            hashZero++;
            // Mark as processed (version=2, phash=0) so we don't retry corrupt thumbs.
            sqlite3_bind_int64(upStmt, 1, 0);
            sqlite3_bind_text(upStmt, 2, key.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_step(upStmt);
            sqlite3_reset(upStmt);
            continue;
        }
        sqlite3_bind_int64(upStmt, 1, (int64_t)phash);
        sqlite3_bind_text(upStmt, 2, key.c_str(), -1, SQLITE_TRANSIENT);
        int rc = sqlite3_step(upStmt);
        sqlite3_reset(upStmt);
        if (rc == SQLITE_DONE) updated++;
    }
    sqlite3_finalize(upStmt);
    executeSQL("COMMIT");
    if (noThumb > 0 || hashZero > 0) {
        DIAG_ACTION_FMT("Database", "CoverPhashBatch: keys=%d ok=%d noThumb=%d hashZero=%d",
                         (int)keys.size(), updated, noThumb, hashZero);
    }
    return updated;
}

void Database::coverPhashBackfillWorkerMain() {
    // COM init handled per-call inside DecodeImageFromMemory (auto COM init/uninit)
    DIAG_ACTION("Database", "CoverPhashBackfillWorker started");

    // Notify the UI to reload the cover wall every N phash updates so visual
    // dedup catches up while the worker is still running. Without this, the
    // cover wall snapshot taken right after a fresh scan shows duplicate
    // covers (phash=0 → grouped by cover_key → distinct keys for visually
    // identical thumbs) until the worker finishes minutes later.
    constexpr int kCoverRefreshNotifyEvery = 256;
    int updatesSinceRefresh = 0;
    bool legacyMembershipRepairPending =
        m_isPrimaryConnection &&
        sqlite3_db_readonly(m_db, "main") != 1 &&
        ReadAppMetaInt(m_db, "cover_tile_membership_needs_maintenance") == 1 &&
        isCoverTileMembershipMaintenancePending();
    bool legacyRegistryRepairPending =
        m_isPrimaryConnection &&
        sqlite3_db_readonly(m_db, "main") != 1 &&
        ReadAppMetaInt(m_db, "cover_registry_needs_maintenance") == 1 &&
        getFileCount() > kGlobalCoverWallFallbackMaxFiles &&
        isCoverRegistryMaintenancePending();

    int idleRounds = 0, totalUpdated = 0;
    while (!m_coverPhashBackfillCancel.load(std::memory_order_acquire)) {
        if (!m_db) break;
        if (legacyMembershipRepairPending) {
            const int repairedMembership = repairCoverTileMembershipBatch(512);
            if (repairedMembership > 0) {
                totalUpdated += repairedMembership;
                updatesSinceRefresh += repairedMembership;
                idleRounds = 0;
                if (updatesSinceRefresh >= kCoverRefreshNotifyEvery) {
                    m_postProcessCoverRefreshNeeded.store(true, std::memory_order_release);
                    updatesSinceRefresh = 0;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(30));
                continue;
            }
            legacyMembershipRepairPending =
                ReadAppMetaInt(m_db, "cover_tile_membership_needs_maintenance") == 1 &&
                isCoverTileMembershipMaintenancePending();
        }
        if (legacyRegistryRepairPending) {
            const int repairedRegistry = repairSparseCoverRegistryBatch(256);
            if (repairedRegistry > 0) {
                totalUpdated += repairedRegistry;
                updatesSinceRefresh += repairedRegistry;
                idleRounds = 0;
                invalidateFacetQueryCache();
                if (updatesSinceRefresh >= kCoverRefreshNotifyEvery) {
                    m_postProcessCoverRefreshNeeded.store(true, std::memory_order_release);
                    updatesSinceRefresh = 0;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(40));
                continue;
            }
            legacyRegistryRepairPending =
                ReadAppMetaInt(m_db, "cover_registry_needs_maintenance") == 1 &&
                getFileCount() > kGlobalCoverWallFallbackMaxFiles &&
                isCoverRegistryMaintenancePending();
        }
        int repaired = repairMissingCoverThumbsBatch(200);
        if (repaired > 0) {
            totalUpdated += repaired;
            updatesSinceRefresh += repaired;
            idleRounds = 0;
            invalidateFacetQueryCache();
            if (updatesSinceRefresh >= kCoverRefreshNotifyEvery) {
                m_postProcessCoverRefreshNeeded.store(true, std::memory_order_release);
                updatesSinceRefresh = 0;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(120));
            continue;
        }
        int updated = backfillCoverPhashBatch(64);
        if (updated > 0) {
            totalUpdated += updated;
            updatesSinceRefresh += updated;
            idleRounds = 0;
            if ((totalUpdated % 512) < 64) invalidateFacetQueryCache();
            if (updatesSinceRefresh >= kCoverRefreshNotifyEvery) {
                m_postProcessCoverRefreshNeeded.store(true, std::memory_order_release);
                updatesSinceRefresh = 0;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(80));
            continue;
        }
        if (sqlite3_db_readonly(m_db, "main") != 1) {
            const auto searchStep = recomputeSearchDataBatch(2048, &m_coverPhashBackfillCancel);
            if (searchStep == HarkenSearch::Search::SearchDataRecomputeStep::Continue) {
                idleRounds = 0;
                std::this_thread::sleep_for(std::chrono::milliseconds(30));
                continue;
            }
            if (searchStep == HarkenSearch::Search::SearchDataRecomputeStep::Failed) {
                if (++idleRounds >= 3) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                break;
            }
        }
        if (++idleRounds >= 3) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    if (totalUpdated > 0) {
        invalidateFacetQueryCache();
        // Final reload notification so the wall reflects all phash updates,
        // even if the last batch didn't cross the per-batch threshold.
        m_postProcessCoverRefreshNeeded.store(true, std::memory_order_release);
    }
    if (m_db) {
        const int remainingMissingThumbRows = CountMissingCoverThumbRowsForMaintenance(m_db);
        const bool maintenancePending =
            isCoverRegistryMaintenancePending() || remainingMissingThumbRows > 0;
        WriteAppMetaInt(m_db, "cover_registry_needs_maintenance", maintenancePending ? 1 : 0);
        DIAG_ACTION_FMT("Database",
                        "CoverThumbBakeDone: totalUpdated=%d remaining=%d pending=%d",
                        totalUpdated,
                        remainingMissingThumbRows,
                        maintenancePending ? 1 : 0);
    }
    DIAG_ACTION_FMT("Database", "CoverPhashBackfillWorker done: totalUpdated=%d", totalUpdated);
    m_coverPhashBackfillRunning.store(false, std::memory_order_release);
}

void Database::startCoverPhashBackfillWorker() {
    if (!m_db) return;
    if (sqlite3_db_readonly(m_db, "main") == 1) return;
    bool expected = false;
    if (!m_coverPhashBackfillRunning.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
        return;
    m_coverPhashBackfillCancel.store(false, std::memory_order_release);
    if (m_coverPhashBackfillThread.joinable()) m_coverPhashBackfillThread.join();
    m_coverPhashBackfillThread = std::thread([this]() { coverPhashBackfillWorkerMain(); });
}

void Database::stopCoverPhashBackfillWorker() {
    m_coverPhashBackfillCancel.store(true, std::memory_order_release);
    if (m_coverPhashBackfillThread.joinable()) m_coverPhashBackfillThread.join();
    m_coverPhashBackfillRunning.store(false, std::memory_order_release);
}

static bool EnsureSearchDataRecomputeWorkTable(sqlite3* db) {
    if (!db) return false;
    std::string err;
    return execSqlite(db,
        "CREATE TEMP TABLE IF NOT EXISTS search_data_recompute_work ("
        "id INTEGER PRIMARY KEY,"
        "old_search_data TEXT,"
        "new_search_data TEXT"
        ");",
        &err);
}

static int64_t SearchDataRecomputeWorkCount(sqlite3* db) {
    int64_t count = 0;
    sqlite3_stmt* stmt = nullptr;
    if (!db) return 0;
    if (sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM temp.search_data_recompute_work;", -1, &stmt, nullptr) != SQLITE_OK) {
        return 0;
    }
    if (sqlite3_step(stmt) == SQLITE_ROW) count = sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
    return count;
}

static int64_t SearchDataRecomputeWorkMaxId(sqlite3* db) {
    int64_t maxId = 0;
    sqlite3_stmt* stmt = nullptr;
    if (!db) return 0;
    if (sqlite3_prepare_v2(db, "SELECT MAX(id) FROM temp.search_data_recompute_work;", -1, &stmt, nullptr) != SQLITE_OK) {
        return 0;
    }
    if (sqlite3_step(stmt) == SQLITE_ROW && sqlite3_column_type(stmt, 0) != SQLITE_NULL) {
        maxId = sqlite3_column_int64(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return maxId;
}

static HarkenSearch::Search::SearchDataRecomputeStep PublishSearchDataRecompute(
    sqlite3* db, std::atomic<bool>* cancel, const std::function<void()>& afterBegin) {
    using HarkenSearch::Search::SearchDataRecomputeStep;
    if (!db) return SearchDataRecomputeStep::Failed;
    if (cancel && cancel->load()) {
        DiscardSearchDataRecomputeScratch(db);
        return SearchDataRecomputeStep::Failed;
    }
    std::string err;
    if (!execSqlite(db, "BEGIN IMMEDIATE;", &err)) {
        // A foreground write transaction on the other connection may be brief.
        return sqlite3_errcode(db) == SQLITE_BUSY ? SearchDataRecomputeStep::Continue
                                                 : SearchDataRecomputeStep::Failed;
    }
    if (afterBegin) afterBegin();
    bool ok = true;
    if (cancel && cancel->load()) ok = false;
    if (ok) {
        ok = execSqlite(db,
            "UPDATE audio_files "
            "SET search_data = ("
            "  SELECT w.new_search_data FROM temp.search_data_recompute_work w WHERE w.id = audio_files.id"
            ") "
            "WHERE EXISTS ("
            "  SELECT 1 FROM temp.search_data_recompute_work w "
            "  WHERE w.id = audio_files.id "
            "    AND audio_files.search_data IS w.old_search_data"
            ");",
            &err);
    }
    if (ok && cancel && cancel->load()) ok = false;
    if (ok) {
        ok = execSqlite(db, "INSERT INTO audio_files_fts(audio_files_fts) VALUES('rebuild');", &err);
    }
    if (ok) {
        ok = WriteAppMetaInt(db, "search_data_migrated", 2);
    }
    if (ok) {
        ok = execSqlite(db, "DELETE FROM app_meta WHERE key = 'search_data_recompute_cursor';", &err);
    }
    if (ok) {
        ok = execSqlite(db, "DROP TABLE IF EXISTS temp.search_data_recompute_work;", &err);
    }
    if (!ok) {
        execSqlite(db, "ROLLBACK;", nullptr);
        DiscardSearchDataRecomputeScratch(db);
        DIAG_ACTION_FMT("Database", "SearchDataRecompute publish failed: %s", err.c_str());
        return SearchDataRecomputeStep::Failed;
    }
    if (!execSqlite(db, "COMMIT;", &err)) {
        // BEGIN above succeeded, so only this publish owns the transaction.
        execSqlite(db, "ROLLBACK;", nullptr);
        DiscardSearchDataRecomputeScratch(db);
        DIAG_ACTION_FMT("Database", "SearchDataRecompute publish commit failed: %s", err.c_str());
        return SearchDataRecomputeStep::Failed;
    }
    return SearchDataRecomputeStep::Finished;
}

bool Database::ensureSearchRecomputeConnection() {
    if (!m_db || m_mainDbPath.empty() || sqlite3_db_readonly(m_db, "main") == 1) return false;
    if (m_searchRecomputeDb) return true;
    sqlite3* opened = nullptr;
    const int rc = sqlite3_open_v2(m_mainDbPath.c_str(), &opened,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_FULLMUTEX, nullptr);
    if (rc != SQLITE_OK) {
        if (opened) sqlite3_close(opened);
        return false;
    }
    RegisterHarkenSearchSqliteFunctions(opened);
    sqlite3_busy_timeout(opened, 250);
    m_searchRecomputeDb = opened;
    return true;
}

HarkenSearch::Search::SearchDataRecomputeStep Database::recomputeSearchDataBatch(int batchSize, std::atomic<bool>* cancel) {
    using HarkenSearch::Search::SearchDataRecomputeStep;
    std::unique_lock<std::mutex> writerLk(m_writerSerializeMutex);
    if (batchSize <= 0 || !ensureSearchRecomputeConnection()) return SearchDataRecomputeStep::Failed;
    sqlite3* db = m_searchRecomputeDb;
    if (sqlite3_get_autocommit(db) == 0) return SearchDataRecomputeStep::Failed;
    constexpr int kSearchDataMigrationVersion = 2;
    if (ReadAppMetaInt(db, "search_data_migrated") == kSearchDataMigrationVersion) {
        return SearchDataRecomputeStep::Finished;
    }
    if (cancel && cancel->load()) {
        DiscardSearchDataRecomputeScratch(db);
        return SearchDataRecomputeStep::Failed;
    }
    if (!EnsureSearchDataRecomputeWorkTable(db)) {
        DiscardSearchDataRecomputeScratch(db);
        return SearchDataRecomputeStep::Failed;
    }

    int64_t cursor = 0;
    if (SearchDataRecomputeWorkCount(db) == 0) {
        if (!execSqlite(db, "DELETE FROM app_meta WHERE key = 'search_data_recompute_cursor';", nullptr)) {
            return sqlite3_errcode(db) == SQLITE_BUSY ? SearchDataRecomputeStep::Continue
                                                      : SearchDataRecomputeStep::Failed;
        }
        cursor = 0;
    } else {
        cursor = SearchDataRecomputeWorkMaxId(db);
    }

    sqlite3_stmt* maxStmt = nullptr;
    int64_t nextCursor = cursor;
    if (sqlite3_prepare_v2(db,
            "SELECT MAX(id) FROM (SELECT id FROM audio_files WHERE id > ? ORDER BY id LIMIT ?);",
            -1, &maxStmt, nullptr) != SQLITE_OK) {
        DiscardSearchDataRecomputeScratch(db);
        return SearchDataRecomputeStep::Failed;
    }
    sqlite3_bind_int64(maxStmt, 1, cursor);
    sqlite3_bind_int(maxStmt, 2, batchSize);
    if (sqlite3_step(maxStmt) == SQLITE_ROW && sqlite3_column_type(maxStmt, 0) != SQLITE_NULL) {
        nextCursor = sqlite3_column_int64(maxStmt, 0);
    }
    sqlite3_finalize(maxStmt);
    if (!HarkenSearch::Search::SearchDataRecomputeStillPending(cursor, nextCursor)) {
        return PublishSearchDataRecompute(db, cancel, m_searchRecomputePublishHookForTests);
    }

    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "INSERT INTO temp.search_data_recompute_work (id, old_search_data, new_search_data) "
        "SELECT id, search_data, aura_search_data("
        "file_name || ' ' || IFNULL(title,'') || ' ' || IFNULL(description,'') || ' ' || "
        "IFNULL(category,'') || ' ' || IFNULL(keywords,'')) "
        "FROM audio_files WHERE id > ? ORDER BY id LIMIT ?;";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        DiscardSearchDataRecomputeScratch(db);
        return SearchDataRecomputeStep::Failed;
    }
    sqlite3_bind_int64(stmt, 1, cursor);
    sqlite3_bind_int(stmt, 2, batchSize);
    const int stepRc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    const bool wrote = stepRc == SQLITE_DONE;
    if (!wrote) {
        DIAG_ACTION_FMT("Database", "SearchDataRecompute staging failed: %s", sqlite3_errmsg(db));
        DiscardSearchDataRecomputeScratch(db);
        return SearchDataRecomputeStep::Failed;
    }
    const auto outcome = HarkenSearch::Search::ClassifySearchDataRecompute(cursor, nextCursor, wrote);
    if (outcome == SearchDataRecomputeStep::Failed) {
        DiscardSearchDataRecomputeScratch(db);
        return SearchDataRecomputeStep::Failed;
    }
    if (outcome != SearchDataRecomputeStep::Continue) {
        return PublishSearchDataRecompute(db, cancel, m_searchRecomputePublishHookForTests);
    }
    const int64_t storedCursor = SearchDataRecomputeWorkMaxId(db);
    if (!WriteAppMetaInt(db, "search_data_recompute_cursor", static_cast<int>(storedCursor))) {
        if (sqlite3_errcode(db) == SQLITE_BUSY) return SearchDataRecomputeStep::Continue;
        DIAG_ACTION("Database", "SearchDataRecompute cursor write failed; stopping retry");
        DiscardSearchDataRecomputeScratch(db);
        return SearchDataRecomputeStep::Failed;
    }
    return SearchDataRecomputeStep::Continue;
}

void Database::setSearchRecomputePublishHookForTests(std::function<void()> hook) {
    std::lock_guard<std::mutex> lock(m_writerSerializeMutex);
    m_searchRecomputePublishHookForTests = std::move(hook);
}

void Database::rebuildFtsIndex() {
    if (!m_db) return;
    DIAG_SCOPED("Database.rebuildFtsIndex");
    std::string err;
    execSqlite(m_db, "INSERT INTO audio_files_fts(audio_files_fts) VALUES('rebuild');", &err);
    DIAG_ACTION_FMT("Database", "FTS rebuild: %s", err.empty() ? "OK" : err.c_str());
}

bool Database::rebuildFtsIndexBatched(int batchSize, std::atomic<float>* progress, std::atomic<bool>* cancel) {
    (void)batchSize;
    if (!m_db) return false;
    DIAG_SCOPED("Database.rebuildFtsIndexBatched");
    if (progress) progress->store(0.01f);
    {
        std::unique_lock<std::mutex> writerLk(m_writerSerializeMutex);
        if (!ensureSearchRecomputeConnection()) {
            if (progress) progress->store(-1.0f);
            return false;
        }
        if (ReadAppMetaInt(m_searchRecomputeDb, "search_data_migrated") == 2) {
            std::string err;
            const bool success = !(cancel && cancel->load()) &&
                execSqlite(m_searchRecomputeDb, "INSERT INTO audio_files_fts(audio_files_fts) VALUES('rebuild');", &err);
            if (!success) {
                DIAG_ACTION_FMT("Database", "FTS explicit rebuild failed: %s", err.c_str());
            }
            if (progress) progress->store(-1.0f);
            return success;
        }
    }
    int recomputePasses = 0;
    for (;;) {
        if (cancel && cancel->load()) {
            {
                std::unique_lock<std::mutex> writerLk(m_writerSerializeMutex);
                DiscardSearchDataRecomputeScratch(m_searchRecomputeDb);
            }
            if (progress) progress->store(-1.0f);
            return false;
        }
        const auto step = recomputeSearchDataBatch(2048, cancel);
        if (step == HarkenSearch::Search::SearchDataRecomputeStep::Continue) {
            if (progress) progress->store(0.05f);
            if (++recomputePasses > 100000) {
                DIAG_ACTION("Database", "SearchDataRecompute gave up after too many passes");
                {
                    std::unique_lock<std::mutex> writerLk(m_writerSerializeMutex);
                    DiscardSearchDataRecomputeScratch(m_searchRecomputeDb);
                }
                if (progress) progress->store(-1.0f);
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        bool migrated = false;
        {
            std::unique_lock<std::mutex> writerLk(m_writerSerializeMutex);
            migrated = m_searchRecomputeDb && ReadAppMetaInt(m_searchRecomputeDb, "search_data_migrated") == 2;
        }
        if (step != HarkenSearch::Search::SearchDataRecomputeStep::Finished || !migrated) {
            DIAG_ACTION("Database", "SearchDataRecompute stopped before replacing the FTS index");
            if (progress) progress->store(-1.0f);
            return false;
        }
        break;
    }
    if (progress) progress->store(-1.0f);
    DIAG_ACTION("Database", "FTS content rebuild after search_data recompute");
    return true;
}

void Database::backfillNormalizedKey() {
    if (!m_db) return;
    DIAG_SCOPED("Database.backfillNormalizedKey");

    // T2.7: 所有 cover_key 必须经 CoverModule::CanonicalCoverKey 才能写 normalized_key.
    //
    // 三类 key 处理策略 (按 I/O 成本由低到高):
    //   1. F|H|<hex>|<size>     纯字符串校验, 合法原样、malformed 拒绝
    //   2. F|PATH|<path>[|mt]   纯字符串重新规范化路径段 (T1.1a 自动小写化 + 反斜杠归一)
    //   3. F|<legacy_path>      触磁盘做视觉 hash, 失败则回落到 F|PATH|<canonical>

    // 1+2: 拉所有 normalized_key 为空的 F|H|% / F|PATH|% 行, 在 C++ 侧过 CanonicalCoverKey
    //      (SQL 端无法做 lower(); F|H| 还需要格式校验; F|PATH 路径段需要重新 lower())
    {
        const char* sql = R"SQL(
            SELECT cover_key FROM cover_cache_registry
            WHERE (normalized_key IS NULL OR normalized_key = '')
              AND (cover_key LIKE 'F|H|%' OR cover_key LIKE 'F|PATH|%')
        )SQL";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
            std::vector<std::string> keys;
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                keys.push_back(safeGetText(stmt, 0));
            }
            sqlite3_finalize(stmt);

            if (!keys.empty()) {
                const char* upSql = "UPDATE cover_cache_registry SET normalized_key = ? WHERE cover_key = ?";
                sqlite3_stmt* upStmt = nullptr;
                if (sqlite3_prepare_v2(m_db, upSql, -1, &upStmt, nullptr) == SQLITE_OK) {
                    executeSQL("BEGIN TRANSACTION");
                    int normalized_n = 0, rejected = 0;
                    for (const auto& k : keys) {
                        std::string canon = CoverModule::CanonicalCoverKey(k);
                        if (canon.empty()) { rejected++; continue; }  // malformed F|H|, etc.
                        sqlite3_bind_text(upStmt, 1, canon.c_str(), -1, SQLITE_TRANSIENT);
                        sqlite3_bind_text(upStmt, 2, k.c_str(), -1, SQLITE_TRANSIENT);
                        sqlite3_step(upStmt);
                        sqlite3_reset(upStmt);
                        normalized_n++;
                    }
                    sqlite3_finalize(upStmt);
                    executeSQL("COMMIT");
                    DIAG_ACTION_FMT("Database",
                        "BackfillNormalizedKey (no-I/O): scanned=%d normalized=%d rejected=%d",
                        (int)keys.size(), normalized_n, rejected);
                }
            }
        }
    }

    // 3. Legacy F|<path> 格式 (无 H/PATH 前缀): 需要读文件计算视觉 hash
    const char* sql = R"SQL(
        SELECT cover_key FROM cover_cache_registry
        WHERE (normalized_key IS NULL OR normalized_key = '')
          AND cover_key LIKE 'F|%'
          AND cover_key NOT LIKE 'F|H|%'
          AND cover_key NOT LIKE 'F|PATH|%'
    )SQL";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) return;

    std::vector<std::string> legacyKeys;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        legacyKeys.push_back(safeGetText(stmt, 0));
    }
    sqlite3_finalize(stmt);

    if (legacyKeys.empty()) return;

    const char* upSql = "UPDATE cover_cache_registry SET normalized_key = ? WHERE cover_key = ?";
    sqlite3_stmt* upStmt = nullptr;
    if (sqlite3_prepare_v2(m_db, upSql, -1, &upStmt, nullptr) != SQLITE_OK) return;

    executeSQL("BEGIN TRANSACTION");
    int updated = 0;
    for (const auto& key : legacyKeys) {
        // F|<path>  ->  <path>
        std::string coverPath = key.substr(2);
        if (coverPath.empty()) continue;

        // NormalizeCoverKeyByVisualIdentity canonicalizes every return path.
        // Canonicalize again to keep migration output defensive and idempotent.
        std::string normalized = CoverModule::CanonicalCoverKey(
            CoverModule::NormalizeCoverKeyByVisualIdentity(coverPath));
        if (normalized.empty()) continue;

        sqlite3_bind_text(upStmt, 1, normalized.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(upStmt, 2, key.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(upStmt);
        sqlite3_reset(upStmt);
        updated++;
    }
    sqlite3_finalize(upStmt);
    executeSQL("COMMIT");

    DIAG_ACTION_FMT("Database", "BackfillNormalizedKey (legacy): scanned=%d updated=%d",
        (int)legacyKeys.size(), updated);
}

// Snooze hides files from search results until the timeout expires.

bool Database::snoozeFilesById(const std::vector<int64_t>& ids, int64_t untilTimestamp) {
    if (!m_db || ids.empty()) return false;
    const char* sql = "UPDATE audio_files SET snoozed_until = ? WHERE id = ?";
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    bool ok = true;
    for (int64_t id : ids) {
        sqlite3_bind_int64(stmt, 1, untilTimestamp);
        sqlite3_bind_int64(stmt, 2, id);
        if (sqlite3_step(stmt) != SQLITE_DONE) ok = false;
        sqlite3_reset(stmt);
    }
    sqlite3_finalize(stmt);
    return ok;
}

bool Database::snoozeFilesByFolderPath(const std::string& folderPath, int64_t untilTimestamp) {
    if (!m_db || folderPath.empty()) return false;
    // Match files whose folder_path starts with the given path (subtree snooze)
    const char* sql = "UPDATE audio_files SET snoozed_until = ? WHERE folder_path = ? OR folder_path LIKE ?";
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    std::string likePattern = folderPath + "%";
    sqlite3_bind_int64(stmt, 1, untilTimestamp);
    sqlite3_bind_text(stmt, 2, folderPath.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, likePattern.c_str(), -1, SQLITE_TRANSIENT);
    bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

bool Database::unsnoozeFilesById(const std::vector<int64_t>& ids) {
    if (!m_db || ids.empty()) return false;
    const char* sql = "UPDATE audio_files SET snoozed_until = 0 WHERE id = ?";
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    bool ok = true;
    for (int64_t id : ids) {
        sqlite3_bind_int64(stmt, 1, id);
        if (sqlite3_step(stmt) != SQLITE_DONE) ok = false;
        sqlite3_reset(stmt);
    }
    sqlite3_finalize(stmt);
    return ok;
}

bool Database::unsnoozeAll() {
    if (!m_db) return false;
    return executeSQL("UPDATE audio_files SET snoozed_until = 0 WHERE snoozed_until != 0");
}

int Database::getSnoozedCount() {
    if (!m_db) return 0;
    int64_t nowTs = (int64_t)time(nullptr);
    std::string sql = "SELECT COUNT(*) FROM audio_files WHERE snoozed_until > " + std::to_string(nowTs);
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(m_db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) return 0;
    int count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW)
        count = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    return count;
}

// Waveform cache methods

static bool BindFloatVectorBlob(sqlite3_stmt* stmt, int column, const std::vector<float>& values) {
    const size_t rawByteSize = values.size() * sizeof(float);
    std::vector<uint8_t> compressed;
    if (WaveformCompression::ShouldCompressRawBytes(rawByteSize) &&
        WaveformCompression::CompressWaveform(values.data(), (int)values.size(), compressed)) {
        const int blobSize = SafeSqliteBlobSize(compressed.size());
        if (blobSize < 0) return false;
        sqlite3_bind_blob(stmt, column, compressed.data(), blobSize, SQLITE_TRANSIENT);
        return true;
    }
    const int blobSize = SafeSqliteBlobSize(rawByteSize);
    if (blobSize < 0) return false;
    sqlite3_bind_blob(stmt, column, values.data(), blobSize, SQLITE_TRANSIENT);
    return true;
}

bool Database::saveWaveformByPath(const std::string& filePath,
                                  const std::vector<float>& waveform,
                                  float trueRmsDbfs,
                                  int miniNumChannels) {
    if (!m_db || waveform.empty()) return false;

    // 1. Resolve file_id.
    static const std::string idSql = "SELECT id FROM audio_files WHERE file_path = ? LIMIT 1;";
    int64_t fileId = -1;
    sqlite3_stmt* idStmt = checkoutStmt(idSql);
    if (idStmt) {
        sqlite3_bind_text(idStmt, 1, filePath.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(idStmt) == SQLITE_ROW) {
            fileId = sqlite3_column_int64(idStmt, 0);
        }
        returnStmt(idSql, idStmt);
    }
    if (fileId <= 0) return false;

    // 2. Write to the waveforms sidecar.
    // Ensure the sidecar row exists without overwriting existing data.
    static const std::string ensureSql = "INSERT OR IGNORE INTO waveforms_db.waveforms (file_id) VALUES (?)";
    sqlite3_stmt* ensureStmt = checkoutStmt(ensureSql);
    if (ensureStmt) {
        sqlite3_bind_int64(ensureStmt, 1, fileId);
        sqlite3_step(ensureStmt);
        returnStmt(ensureSql, ensureStmt);
    }

    // Update waveform_data, true_rms_dbfs, and mini_num_channels.
    static const std::string sql = "UPDATE waveforms_db.waveforms "
                                   "SET waveform_data = ?, status = 'done', error_code = '', updated_at = ?, retry_count = 0, true_rms_dbfs = ?, mini_num_channels = ? "
                                   "WHERE file_id = ?";
    sqlite3_stmt* stmt = checkoutStmt(sql);
    if (!stmt) return false;

    if (!BindFloatVectorBlob(stmt, 1, waveform)) {
        returnStmt(sql, stmt);
        return false;
    }
    sqlite3_bind_int64(stmt, 2, getCurrentTimestamp());
    if (trueRmsDbfs > -199.0f)
        sqlite3_bind_double(stmt, 3, (double)trueRmsDbfs);
    else
        sqlite3_bind_null(stmt, 3);
    sqlite3_bind_int(stmt, 4, std::max(1, miniNumChannels));
    sqlite3_bind_int64(stmt, 5, fileId);

    bool success = sqlite3_step(stmt) == SQLITE_DONE;
    returnStmt(sql, stmt);
    return success;
}

bool Database::saveMiniWaveformEnvelopeByPath(const std::string& filePath,
                                              const std::vector<float>& maxPeaks,
                                              const std::vector<float>& minPeaks,
                                              float trueRmsDbfs,
                                              int miniNumChannels) {
    if (!m_db || maxPeaks.empty()) return false;
    if (!minPeaks.empty() && minPeaks.size() != maxPeaks.size()) return false;

    static const std::string idSql = "SELECT id FROM audio_files WHERE file_path = ? LIMIT 1;";
    int64_t fileId = -1;
    sqlite3_stmt* idStmt = checkoutStmt(idSql);
    if (idStmt) {
        sqlite3_bind_text(idStmt, 1, filePath.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(idStmt) == SQLITE_ROW) {
            fileId = sqlite3_column_int64(idStmt, 0);
        }
        returnStmt(idSql, idStmt);
    }
    if (fileId <= 0) return false;

    static const std::string ensureSql = "INSERT OR IGNORE INTO waveforms_db.waveforms (file_id) VALUES (?)";
    sqlite3_stmt* ensureStmt = checkoutStmt(ensureSql);
    if (ensureStmt) {
        sqlite3_bind_int64(ensureStmt, 1, fileId);
        sqlite3_step(ensureStmt);
        returnStmt(ensureSql, ensureStmt);
    }

    static const std::string sql =
        "UPDATE waveforms_db.waveforms "
        "SET waveform_data = ?, mini_min_data = ?, status = 'done', error_code = '', "
        "updated_at = ?, retry_count = 0, true_rms_dbfs = ?, mini_num_channels = ? "
        "WHERE file_id = ?";
    sqlite3_stmt* stmt = checkoutStmt(sql);
    if (!stmt) return false;

    if (!BindFloatVectorBlob(stmt, 1, maxPeaks)) {
        returnStmt(sql, stmt);
        return false;
    }
    if (!minPeaks.empty()) {
        if (!BindFloatVectorBlob(stmt, 2, minPeaks)) {
            returnStmt(sql, stmt);
            return false;
        }
    } else {
        sqlite3_bind_null(stmt, 2);
    }

    sqlite3_bind_int64(stmt, 3, getCurrentTimestamp());
    if (trueRmsDbfs > -199.0f) {
        sqlite3_bind_double(stmt, 4, (double)trueRmsDbfs);
    } else {
        sqlite3_bind_null(stmt, 4);
    }
    sqlite3_bind_int(stmt, 5, std::max(1, miniNumChannels));
    sqlite3_bind_int64(stmt, 6, fileId);

    const bool success = sqlite3_step(stmt) == SQLITE_DONE;
    returnStmt(sql, stmt);
    return success;
}

bool Database::markAdaptiveMiniWaveformFormatUpgradeComplete() {
    if (!m_db) return false;
    if (!WriteAppMetaString(m_db, kMiniWaveformFormatV2MetaKey, "1")) {
        return false;
    }
    m_needsAdaptiveMiniWaveformFormatUpgrade = false;
    return true;
}

std::vector<float> Database::getWaveformByPath(const std::string& filePath,
                                               float* outTrueRmsDbfs,
                                               int* outMiniNumChannels) {
    std::vector<float> result;
    if (!m_db) return result;
    if (outMiniNumChannels) *outMiniNumChannels = 1;

    std::string sql = "SELECT w.waveform_data, w.true_rms_dbfs, w.mini_num_channels FROM waveforms_db.waveforms w "
                      "JOIN audio_files af ON w.file_id = af.id "
                      "WHERE af.file_path = ? LIMIT 1";

    sqlite3_stmt* stmt = checkoutStmt(sql);
    if (!stmt) return result;

    sqlite3_bind_text(stmt, 1, filePath.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const void* blob = sqlite3_column_blob(stmt, 0);
        int blobSize = sqlite3_column_bytes(stmt, 0);
        if (blob && blobSize > 0) {
            if (WaveformCompression::IsCompressed(blob, blobSize)) {
                WaveformCompression::DecompressWaveform(blob, blobSize, result);
            } else {
                int floatCount = blobSize / (int)sizeof(float);
                result.resize(floatCount);
                memcpy(result.data(), blob, blobSize);
            }
        }
        // true_rms_dbfs may be NULL.
        if (outTrueRmsDbfs && sqlite3_column_count(stmt) > 1 && sqlite3_column_type(stmt, 1) != SQLITE_NULL) {
            *outTrueRmsDbfs = (float)sqlite3_column_double(stmt, 1);
        }
        if (outMiniNumChannels && sqlite3_column_count(stmt) > 2 && sqlite3_column_type(stmt, 2) != SQLITE_NULL) {
            *outMiniNumChannels = std::max(1, sqlite3_column_int(stmt, 2));
        }
    }

    // Return the statement to the pool instead of finalizing it.
    returnStmt(sql, stmt);
    
    return result;
}

namespace {
bool CopyFloatBlobColumn(sqlite3_stmt* stmt, int column, std::vector<float>& out) {
    out.clear();
    const void* blob = sqlite3_column_blob(stmt, column);
    const int blobSize = sqlite3_column_bytes(stmt, column);
    if (!blob || blobSize <= 0) return false;
    if (WaveformCompression::IsCompressed(blob, blobSize)) {
        return WaveformCompression::DecompressWaveform(blob, blobSize, out);
    }
    if ((blobSize % (int)sizeof(float)) != 0) return false;

    const int floatCount = blobSize / (int)sizeof(float);
    out.resize(floatCount);
    memcpy(out.data(), blob, blobSize);
    return true;
}

bool CopyBlobBytesColumn(sqlite3_stmt* stmt, int column, std::vector<uint8_t>& out) {
    out.clear();
    const void* blob = sqlite3_column_blob(stmt, column);
    const int blobSize = sqlite3_column_bytes(stmt, column);
    if (!blob || blobSize <= 0) {
        return false;
    }
    const uint8_t* begin = static_cast<const uint8_t*>(blob);
    out.assign(begin, begin + blobSize);
    return true;
}

bool LooksLikePeakEnvelopeInt16Blob(const std::vector<uint8_t>& blob) {
    return blob.size() >= 5 &&
           blob[0] == 0x00 &&
           blob[1] == 'W' &&
           blob[2] == 'P' &&
           blob[3] == 'K' &&
           blob[4] == '2';
}

bool HasValidDetailBlobColumn(sqlite3_stmt* stmt, int column) {
    std::vector<uint8_t> bytes;
    if (!CopyBlobBytesColumn(stmt, column, bytes)) {
        return false;
    }
    if (WaveformExtractor::IsPeakEnvelopeInt16Blob(bytes)) {
        return true;
    }
    if (LooksLikePeakEnvelopeInt16Blob(bytes)) {
        return false;
    }
    return true;
}

bool DecodePeakEnvelopeColumn(sqlite3_stmt* stmt,
                              int column,
                              std::vector<float>& outPeaks,
                              std::vector<float>& outMinPeaks) {
    std::vector<uint8_t> bytes;
    if (!CopyBlobBytesColumn(stmt, column, bytes) ||
        !WaveformExtractor::IsPeakEnvelopeInt16Blob(bytes)) {
        return false;
    }

    int channels = 0;
    int buckets = 0;
    return WaveformExtractor::DecodePeakEnvelopeInt16(bytes, outPeaks, outMinPeaks, channels, buckets);
}

bool CopyDetailMaxBlobColumn(sqlite3_stmt* stmt, int column, std::vector<float>& out) {
    std::vector<float> minPeaks;
    if (DecodePeakEnvelopeColumn(stmt, column, out, minPeaks)) {
        return true;
    }
    return CopyFloatBlobColumn(stmt, column, out);
}

bool CopyDetailMinBlobColumns(sqlite3_stmt* stmt, int detailColumn, int minColumn, std::vector<float>& out) {
    std::vector<float> maxPeaks;
    if (DecodePeakEnvelopeColumn(stmt, detailColumn, maxPeaks, out)) {
        return true;
    }
    return CopyFloatBlobColumn(stmt, minColumn, out);
}

bool BuildMiniWaveformFromDetailEnvelope(const std::vector<float>& maxDetail,
                                         const std::vector<float>& minDetail,
                                         int detailChannels,
                                         int targetFrames,
                                         std::vector<float>& outPeaks,
                                         std::vector<float>& outRms) {
    outPeaks.clear();
    outRms.clear();
    if (detailChannels <= 0 || targetFrames <= 0 || maxDetail.empty()) {
        return false;
    }
    const bool hasMinDetail = minDetail.size() >= maxDetail.size();

    const int sourceFrames = (int)maxDetail.size() / detailChannels;
    if (sourceFrames <= 0) {
        return false;
    }

    outPeaks.assign((size_t)targetFrames * (size_t)detailChannels, 0.0f);
    outRms.assign(outPeaks.size(), 0.0f);
    for (int frame = 0; frame < targetFrames; ++frame) {
        int begin = (int)(((int64_t)frame * sourceFrames) / targetFrames);
        int end = (int)(((int64_t)(frame + 1) * sourceFrames) / targetFrames);
        begin = std::clamp(begin, 0, sourceFrames - 1);
        end = std::clamp(end, begin + 1, sourceFrames);
        for (int ch = 0; ch < detailChannels; ++ch) {
            float peak = 0.0f;
            for (int src = begin; src < end; ++src) {
                const size_t idx = (size_t)src * (size_t)detailChannels + (size_t)ch;
                peak = (std::max)(peak, std::fabs(maxDetail[idx]));
                if (hasMinDetail) {
                    peak = (std::max)(peak, std::fabs(minDetail[idx]));
                }
            }
            const size_t dst = (size_t)frame * (size_t)detailChannels + (size_t)ch;
            outPeaks[dst] = peak;
            outRms[dst] = peak * 0.35f;
        }
    }
    return true;
}
}

bool Database::getMiniWaveformBundleByPath(const std::string& filePath,
                                           MiniWaveformBundle& out,
                                           int targetFrames) {
    out = MiniWaveformBundle{};
    if (!m_db || filePath.empty()) return false;
    if (targetFrames <= 0) {
        targetFrames = 128;
    }

    auto fillDisplayRmsFallback = [&]() {
        if (!out.rms.empty() || out.peaks.empty()) return;
        out.rms.resize(out.peaks.size());
        for (size_t i = 0; i < out.peaks.size(); ++i) {
            out.rms[i] = std::fabs(out.peaks[i]) * 0.35f;
        }
    };

    auto populateLoudnessIfNeeded = [&]() {
        if (!out.hasLoudness) {
            out.hasLoudness = getLoudnessByPath(filePath,
                                                out.integratedLUFS,
                                                out.momentaryLUFS,
                                                out.shortTermLUFS,
                                                out.truePeakDBTP);
        }
    };

    auto tryDeriveMiniFromDetail = [&]() {
        const std::vector<float> detail = getWaveformDetailByPath(filePath);
        const std::vector<float> minDetail = getWaveformMinDetailByPath(filePath);
        int detailChannels = getWaveformDetailChannelsByPath(filePath);
        if (detailChannels <= 0) {
            detailChannels = 1;
        }

        std::vector<float> derivedPeaks;
        std::vector<float> derivedRms;
        if (!BuildMiniWaveformFromDetailEnvelope(detail,
                                                 minDetail,
                                                 detailChannels,
                                                 targetFrames,
                                                 derivedPeaks,
                                                 derivedRms)) {
            return false;
        }

        out.peaks = std::move(derivedPeaks);
        out.minPeaks.clear();
        out.rms = std::move(derivedRms);
        out.miniNumChannels = detailChannels;
        populateLoudnessIfNeeded();
        DIAG_ACTION_FMT("MiniWave",
                        "DetailDerived peaks=%d channels=%d path=%s",
                        (int)out.peaks.size(),
                        out.miniNumChannels,
                        filePath.c_str());
        return true;
    };

    auto populateFallback = [&]() {
        out.peaks = getWaveformByPath(filePath, &out.trueRmsDbfs, &out.miniNumChannels);
        out.minPeaks.clear();
        out.hasTrueRms = out.trueRmsDbfs > -199.0f;
        out.rms = getRmsDataByPath(filePath);
        fillDisplayRmsFallback();
        populateLoudnessIfNeeded();
        return !out.peaks.empty();
    };

    const std::string sql =
        "SELECT w.waveform_data, w.mini_min_data, w.rms_data, w.true_rms_dbfs, w.mini_num_channels, "
        "af.integrated_lufs, af.momentary_lufs, af.short_term_lufs, af.true_peak_dbtp "
        "FROM waveforms_db.waveforms w "
        "JOIN audio_files af ON w.file_id = af.id "
        "WHERE af.file_path = ? LIMIT 1";

    sqlite3_stmt* stmt = checkoutStmt(sql);
    if (!stmt) {
        return tryDeriveMiniFromDetail() || populateFallback();
    }

    sqlite3_bind_text(stmt, 1, filePath.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        CopyFloatBlobColumn(stmt, 0, out.peaks);
        CopyFloatBlobColumn(stmt, 1, out.minPeaks);
        CopyFloatBlobColumn(stmt, 2, out.rms);

        if (sqlite3_column_type(stmt, 3) != SQLITE_NULL) {
            out.trueRmsDbfs = (float)sqlite3_column_double(stmt, 3);
            out.hasTrueRms = out.trueRmsDbfs > -199.0f;
        }
        if (sqlite3_column_type(stmt, 4) != SQLITE_NULL) {
            out.miniNumChannels = std::max(1, sqlite3_column_int(stmt, 4));
        }
        if (sqlite3_column_type(stmt, 5) != SQLITE_NULL) {
            out.integratedLUFS = (float)sqlite3_column_double(stmt, 5);
            out.momentaryLUFS = (float)sqlite3_column_double(stmt, 6);
            out.shortTermLUFS = (float)sqlite3_column_double(stmt, 7);
            out.truePeakDBTP = (float)sqlite3_column_double(stmt, 8);
            out.hasLoudness = true;
        }
    }
    returnStmt(sql, stmt);

    if (out.peaks.empty()) {
        if (tryDeriveMiniFromDetail()) {
            return true;
        }
        out = MiniWaveformBundle{};
        return populateFallback();
    }

    DIAG_ACTION_FMT("MiniWave",
                    "CacheHit peaks=%d channels=%d path=%s",
                    (int)out.peaks.size(),
                    out.miniNumChannels,
                    filePath.c_str());
    if (out.rms.empty()) {
        out.rms = getRmsDataByPath(filePath);
        fillDisplayRmsFallback();
    }
    populateLoudnessIfNeeded();

    return true;
}

// RMS data storage for accurate loudness measurement
bool Database::saveRmsDataByPath(const std::string& filePath, const std::vector<float>& rmsData) {
    if (!m_db || rmsData.empty()) return false;
    
    // 1. Resolve file_id.
    const char* idSql = "SELECT id FROM audio_files WHERE file_path = ? LIMIT 1;";
    sqlite3_stmt* idStmt;
    int64_t fileId = -1;
    if (sqlite3_prepare_v2(m_db, idSql, -1, &idStmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(idStmt, 1, filePath.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(idStmt) == SQLITE_ROW) {
            fileId = sqlite3_column_int64(idStmt, 0);
        }
        sqlite3_finalize(idStmt);
    }
    if (fileId <= 0) return false;

    // Upsert waveform fields in the sidecar; waveform_data may already exist.
    // 先确保行存在
    const char* ensureSql = "INSERT OR IGNORE INTO waveforms_db.waveforms (file_id) VALUES (?)";
    sqlite3_stmt* ensureStmt;
    if (sqlite3_prepare_v2(m_db, ensureSql, -1, &ensureStmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(ensureStmt, 1, fileId);
        sqlite3_step(ensureStmt);
        sqlite3_finalize(ensureStmt);
    }

    const char* sql = "UPDATE waveforms_db.waveforms "
                      "SET rms_data = ?, status = 'done', error_code = '', updated_at = ?, retry_count = 0 "
                      "WHERE file_id = ?";
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }
    
    size_t byteSize = rmsData.size() * sizeof(float);
    int blobSize = SafeSqliteBlobSize(byteSize);
    if (blobSize < 0) {
        sqlite3_finalize(stmt);
        return false;
    }
    sqlite3_bind_blob(stmt, 1, rmsData.data(), blobSize, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 2, getCurrentTimestamp());
    sqlite3_bind_int64(stmt, 3, fileId);
    
    bool success = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return success;
}

std::vector<float> Database::getRmsDataByPath(const std::string& filePath) {
    std::vector<float> result;
    if (!m_db) return result;
    
    // Read from the waveforms sidecar first.
    const char* sql = "SELECT w.rms_data FROM waveforms_db.waveforms w "
                      "JOIN audio_files af ON w.file_id = af.id "
                      "WHERE af.file_path = ? LIMIT 1";
    sqlite3_stmt* stmt;
    
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, filePath.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            const void* blob = sqlite3_column_blob(stmt, 0);
            int blobSize = sqlite3_column_bytes(stmt, 0);
            if (blob && blobSize > 0) {
                int floatCount = blobSize / sizeof(float);
                result.resize(floatCount);
                memcpy(result.data(), blob, blobSize);
            }
        }
        sqlite3_finalize(stmt);
    }
    
    return result;
}

// Spectral band data storage for SpectralPeaks waveform display
bool Database::saveSpectralDataByPath(const std::string& filePath, const std::vector<float>& spectralData) {
    if (!m_db || spectralData.empty()) return false;

    const char* idSql = "SELECT id FROM audio_files WHERE file_path = ? LIMIT 1;";
    sqlite3_stmt* idStmt;
    int64_t fileId = -1;
    if (sqlite3_prepare_v2(m_db, idSql, -1, &idStmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(idStmt, 1, filePath.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(idStmt) == SQLITE_ROW)
            fileId = sqlite3_column_int64(idStmt, 0);
        sqlite3_finalize(idStmt);
    }
    if (fileId <= 0) return false;

    const char* ensureSql = "INSERT OR IGNORE INTO waveforms_db.waveforms (file_id) VALUES (?)";
    sqlite3_stmt* ensureStmt;
    if (sqlite3_prepare_v2(m_db, ensureSql, -1, &ensureStmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(ensureStmt, 1, fileId);
        sqlite3_step(ensureStmt);
        sqlite3_finalize(ensureStmt);
    }

    const char* sql = "UPDATE waveforms_db.waveforms SET spectral_data = ? WHERE file_id = ?";
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;

    size_t byteSize = spectralData.size() * sizeof(float);
    int blobSize = SafeSqliteBlobSize(byteSize);
    if (blobSize < 0) { sqlite3_finalize(stmt); return false; }
    sqlite3_bind_blob(stmt, 1, spectralData.data(), blobSize, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 2, fileId);
    bool success = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return success;
}

std::vector<float> Database::getSpectralDataByPath(const std::string& filePath) {
    std::vector<float> result;
    if (!m_db) return result;

    const char* sql = "SELECT w.spectral_data FROM waveforms_db.waveforms w "
                      "JOIN audio_files af ON w.file_id = af.id "
                      "WHERE af.file_path = ? LIMIT 1";
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, filePath.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            const void* blob = sqlite3_column_blob(stmt, 0);
            int blobSize = sqlite3_column_bytes(stmt, 0);
            if (blob && blobSize > 0) {
                int floatCount = blobSize / (int)sizeof(float);
                result.resize(floatCount);
                memcpy(result.data(), blob, blobSize);
            }
        }
        sqlite3_finalize(stmt);
    }
    return result;
}

// Loudness data storage (ITU-R BS.1770-4)
bool Database::saveLoudnessByPath(const std::string& filePath, float integrated, float momentary, float shortTerm, float truePeak) {
    if (!m_db) return false;
    
    const char* sql = "UPDATE audio_files SET integrated_lufs = ?, momentary_lufs = ?, short_term_lufs = ?, true_peak_dbtp = ? WHERE file_path = ?";
    sqlite3_stmt* stmt;
    
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }
    
    sqlite3_bind_double(stmt, 1, (double)integrated);
    sqlite3_bind_double(stmt, 2, (double)momentary);
    sqlite3_bind_double(stmt, 3, (double)shortTerm);
    sqlite3_bind_double(stmt, 4, (double)truePeak);
    sqlite3_bind_text(stmt, 5, filePath.c_str(), -1, SQLITE_TRANSIENT);
    
    bool success = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return success;
}

bool Database::getLoudnessByPath(const std::string& filePath, float& integrated, float& momentary, float& shortTerm, float& truePeak) {
    if (!m_db) return false;
    
    const char* sql = "SELECT integrated_lufs, momentary_lufs, short_term_lufs, true_peak_dbtp FROM audio_files WHERE file_path = ?";
    sqlite3_stmt* stmt;
    
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }
    
    sqlite3_bind_text(stmt, 1, filePath.c_str(), -1, SQLITE_TRANSIENT);
    
    bool found = false;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        if (sqlite3_column_type(stmt, 0) != SQLITE_NULL) {
            integrated = (float)sqlite3_column_double(stmt, 0);
            momentary  = (float)sqlite3_column_double(stmt, 1);
            shortTerm  = (float)sqlite3_column_double(stmt, 2);
            truePeak   = (float)sqlite3_column_double(stmt, 3);
            found = true;
        }
    }
    
    sqlite3_finalize(stmt);
    return found;
}

// Legacy compatibility
bool Database::saveLufsByPath(const std::string& filePath, float lufs) {
    return saveLoudnessByPath(filePath, lufs, -70.0f, -70.0f, -70.0f);
}

float Database::getLufsByPath(const std::string& filePath) {
    float integrated = -70.0f, momentary = -70.0f, shortTerm = -70.0f, truePeak = -70.0f;
    getLoudnessByPath(filePath, integrated, momentary, shortTerm, truePeak);
    return integrated;
}

// Audio similarity features (F-028)

bool Database::saveAudioFeaturesByPath(const std::string& filePath, const AudioFeatureVector& vec) {
    if (!m_db) return false;
    auto blob = vec.serialize();
    const char* sql = "UPDATE audio_files SET audio_features = ?, features_version = ? WHERE file_path = ?";
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_blob(stmt, 1, blob.data(), (int)blob.size(), SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, AudioFeatureVector::VERSION);
    sqlite3_bind_text(stmt, 3, filePath.c_str(), -1, SQLITE_TRANSIENT);
    bool ok = (sqlite3_step(stmt) == SQLITE_DONE);
    sqlite3_finalize(stmt);
    return ok;
}

bool Database::getAudioFeaturesByPath(const std::string& filePath, AudioFeatureVector& out) {
    if (!m_db) return false;
    const char* sql = "SELECT audio_features FROM audio_files WHERE file_path = ? AND audio_features IS NOT NULL";
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(stmt, 1, filePath.c_str(), -1, SQLITE_TRANSIENT);
    bool ok = false;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const void* blob = sqlite3_column_blob(stmt, 0);
        int blobSize = sqlite3_column_bytes(stmt, 0);
        ok = AudioFeatureVector::deserialize(blob, blobSize, out);
    }
    sqlite3_finalize(stmt);
    return ok;
}

bool Database::getAudioFeaturesById(int64_t fileId, AudioFeatureVector& out) {
    if (!m_db) return false;
    const char* sql = "SELECT audio_features FROM audio_files WHERE id = ? AND audio_features IS NOT NULL";
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_int64(stmt, 1, fileId);
    bool ok = false;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const void* blob = sqlite3_column_blob(stmt, 0);
        int blobSize = sqlite3_column_bytes(stmt, 0);
        ok = AudioFeatureVector::deserialize(blob, blobSize, out);
    }
    sqlite3_finalize(stmt);
    return ok;
}

bool Database::tryLoadAllAudioFeatures(
    std::vector<AudioFeatureEntry>& out) {
    if (!m_db) return false;

    std::vector<AudioFeatureEntry> result;
    const char* sql = "SELECT id, file_path, audio_features FROM audio_files "
                      "WHERE audio_features IS NOT NULL AND features_version >= 1";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }

    result.reserve(4096);
    int stepResult = SQLITE_OK;
    while ((stepResult = sqlite3_step(stmt)) == SQLITE_ROW) {
        AudioFeatureEntry e;
        e.fileId = sqlite3_column_int64(stmt, 0);
        const unsigned char* text = sqlite3_column_text(stmt, 1);
        if (!text) {
            sqlite3_finalize(stmt);
            return false;
        }
        e.filePath = reinterpret_cast<const char*>(text);
        const void* blob = sqlite3_column_blob(stmt, 2);
        const int blobSize = sqlite3_column_bytes(stmt, 2);
        if (!AudioFeatureVector::deserialize(blob, blobSize, e.features)) {
            sqlite3_finalize(stmt);
            return false;
        }
        result.push_back(std::move(e));
    }

    sqlite3_finalize(stmt);
    if (stepResult != SQLITE_DONE) return false;

    out = std::move(result);
    return true;
}

std::vector<Database::AudioFeatureEntry> Database::loadAllAudioFeatures() {
    std::vector<AudioFeatureEntry> result;
    if (!m_db) return result;

    const char* sql = "SELECT id, file_path, audio_features FROM audio_files "
                      "WHERE audio_features IS NOT NULL AND features_version >= 1";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return result;
    }

    result.reserve(4096);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        AudioFeatureEntry entry;
        entry.fileId = sqlite3_column_int64(stmt, 0);
        const unsigned char* text = sqlite3_column_text(stmt, 1);
        if (text) entry.filePath = reinterpret_cast<const char*>(text);
        const void* blob = sqlite3_column_blob(stmt, 2);
        const int blobSize = sqlite3_column_bytes(stmt, 2);
        if (AudioFeatureVector::deserialize(blob, blobSize,
                                            entry.features)) {
            result.push_back(std::move(entry));
        }
    }
    sqlite3_finalize(stmt);
    return result;
}

bool Database::tryCountFilesWithFeatures(int& out) {
    if (!m_db) return false;

    const char* sql = "SELECT COUNT(*) FROM audio_files WHERE audio_features IS NOT NULL AND features_version >= 1";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }

    if (sqlite3_step(stmt) != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        return false;
    }
    const int count = sqlite3_column_int(stmt, 0);
    const int done = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (done != SQLITE_DONE) return false;

    out = count;
    return true;
}

bool Database::tryGetFeatureIndexCounts(int& indexed, int& total) {
    if (!m_db) return false;
    const char* sql = "SELECT COUNT(*), COALESCE(SUM(CASE WHEN audio_features IS NOT NULL "
                      "AND features_version >= 1 THEN 1 ELSE 0 END), 0) FROM audio_files";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    if (sqlite3_step(stmt) != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        return false;
    }
    const int newTotal = sqlite3_column_int(stmt, 0);
    const int newIndexed = sqlite3_column_int(stmt, 1);
    const int done = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (done != SQLITE_DONE) return false;
    total = newTotal;
    indexed = newIndexed;
    return true;
}

int Database::countFilesWithFeatures() {
    int count = 0;
    (void)tryCountFilesWithFeatures(count);
    return count;
}

int Database::countFilesWithoutFeatures(int limit) {
    if (!m_db) return 0;
    std::string sql = "SELECT COUNT(*) FROM audio_files WHERE audio_features IS NULL OR features_version < 1";
    if (limit > 0) sql = "SELECT COUNT(*) FROM (SELECT 1 FROM audio_files WHERE audio_features IS NULL OR features_version < 1 LIMIT " + std::to_string(limit) + ")";
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(m_db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) return 0;
    int count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW)
        count = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    return count;
}

std::vector<std::string> Database::getFilesWithoutFeatures(int limit) {
    std::vector<std::string> result;
    if (!m_db) return result;
    std::string sql = "SELECT file_path FROM audio_files WHERE audio_features IS NULL OR features_version < 1";
    if (limit > 0) sql += " LIMIT " + std::to_string(limit);
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(m_db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) return result;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char* p = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        if (p) result.emplace_back(p);
    }
    sqlite3_finalize(stmt);
    return result;
}

// Write-side helper that resolves only the database ID.
// saveWaveformDetail
bool Database::saveWaveformDetail(int64_t fileId,
                                  const std::vector<float>& waveform,
                                  const std::vector<float>& minWaveform,
                                  int detailChannels) {
    if (!m_db || fileId <= 0 || waveform.empty()) return false;
    const bool hasMinWaveform = (minWaveform.size() == waveform.size());
    if (detailChannels <= 0) detailChannels = 1;

    const char* ensureSql = "INSERT OR IGNORE INTO waveforms_db.waveforms (file_id) VALUES (?)";
    sqlite3_stmt* ensureStmt = nullptr;
    if (sqlite3_prepare_v2(m_db, ensureSql, -1, &ensureStmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(ensureStmt, 1, fileId);
        sqlite3_step(ensureStmt);
        sqlite3_finalize(ensureStmt);
    }

    const char* sql = "UPDATE waveforms_db.waveforms "
                      "SET waveform_detail = ?, waveform_min_detail = ?, detail_num_channels = ?, status = 'done', error_code = '', updated_at = ?, retry_count = 0 "
                      "WHERE file_id = ?";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }

    std::vector<uint8_t> wpk2Blob;
    const bool wpk2WriterEnabled = IsWpk2DetailWriterEnabled();
    const bool writeWpk2 = TryEncodeWpk2DetailBlob(waveform, minWaveform, detailChannels, wpk2WriterEnabled, wpk2Blob);
    const size_t byteSize = waveform.size() * sizeof(float);
    if (writeWpk2) {
        const int blobSize = SafeSqliteBlobSize(wpk2Blob.size());
        if (blobSize < 0) {
            sqlite3_finalize(stmt);
            return false;
        }
        sqlite3_bind_blob(stmt, 1, wpk2Blob.data(), blobSize, SQLITE_TRANSIENT);
        sqlite3_bind_null(stmt, 2);
    } else {
        sqlite3_bind_blob(stmt, 1, waveform.data(), (int)byteSize, SQLITE_TRANSIENT);
        if (hasMinWaveform) {
            sqlite3_bind_blob(stmt, 2, minWaveform.data(), (int)byteSize, SQLITE_TRANSIENT);
        } else {
            sqlite3_bind_null(stmt, 2);
        }
    }
    sqlite3_bind_int(stmt, 3, detailChannels);
    sqlite3_bind_int64(stmt, 4, getCurrentTimestamp());
    sqlite3_bind_int64(stmt, 5, fileId);

    bool ok = (sqlite3_step(stmt) == SQLITE_DONE);
    sqlite3_finalize(stmt);
    return ok;
}

static bool getExternalWaveformFingerprint(const std::string& filePath,
                                           int64_t& fileSize,
                                           int64_t& fileMtime) {
    fileSize = 0;
    fileMtime = 0;
    if (filePath.empty()) return false;

    const auto info = Platform::FileSystem::ProbeRegularFile(filePath);
    if (!info.isRegularFile || !info.sizeAvailable) return false;
    fileSize = static_cast<int64_t>(info.sizeBytes);
    fileMtime = Platform::FileSystem::LastWriteTimeUnixSeconds(filePath);
    return true;
}

bool Database::saveWaveformDetailByPath(const std::string& filePath,
                                        const std::vector<float>& waveform,
                                        const std::vector<float>& minWaveform,
                                        int detailChannels) {
    if (!m_db || waveform.empty()) return false;
    const bool hasMinWaveform = (minWaveform.size() == waveform.size());
    if (detailChannels <= 0) detailChannels = 1;
    
    // Query only the file ID instead of materializing a full AudioRecord.
    // This reads one int64 rather than hundreds of bytes of row data.
    const char* idSql = "SELECT id FROM audio_files INDEXED BY idx_files_path WHERE file_path = ? LIMIT 1;";
    sqlite3_stmt* idStmt;
    int64_t fileId = -1;
    
    if (sqlite3_prepare_v2(m_db, idSql, -1, &idStmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(idStmt, 1, filePath.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(idStmt) == SQLITE_ROW) {
            fileId = sqlite3_column_int64(idStmt, 0);
        }
        sqlite3_finalize(idStmt);
    }
    
    if (fileId > 0) {
        return saveWaveformDetail(fileId, waveform, minWaveform, detailChannels);
    }

    const std::vector<std::string> libraryRoots = getLibraryRoots();
    if (!libraryRoots.empty() && IsPathInsideLibraryRoots(filePath, libraryRoots)) {
        return false;
    }

    int64_t fileSize = 0;
    int64_t fileMtime = 0;
    if (!getExternalWaveformFingerprint(filePath, fileSize, fileMtime)) {
        return false;
    }

    const char* sql = R"(
        INSERT INTO waveforms_db.external_waveforms
            (file_path, file_size, file_mtime, waveform_detail, waveform_min_detail, detail_num_channels, updated_at, cache_key)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?)
        ON CONFLICT(file_path) DO UPDATE SET
            file_size = excluded.file_size,
            file_mtime = excluded.file_mtime,
            waveform_detail = excluded.waveform_detail,
            waveform_min_detail = excluded.waveform_min_detail,
            detail_num_channels = excluded.detail_num_channels,
            updated_at = excluded.updated_at,
            cache_key = excluded.cache_key
    )";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }

    const std::string cacheKey = std::to_string(fileSize) + ":" + std::to_string(fileMtime);
    std::vector<uint8_t> wpk2Blob;
    const bool wpk2WriterEnabled = IsWpk2DetailWriterEnabled();
    const bool writeWpk2 = TryEncodeWpk2DetailBlob(waveform, minWaveform, detailChannels, wpk2WriterEnabled, wpk2Blob);
    const size_t byteSize = waveform.size() * sizeof(float);
    sqlite3_bind_text(stmt, 1, filePath.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 2, fileSize);
    sqlite3_bind_int64(stmt, 3, fileMtime);
    if (writeWpk2) {
        const int blobSize = SafeSqliteBlobSize(wpk2Blob.size());
        if (blobSize < 0) {
            sqlite3_finalize(stmt);
            return false;
        }
        sqlite3_bind_blob(stmt, 4, wpk2Blob.data(), blobSize, SQLITE_TRANSIENT);
        sqlite3_bind_null(stmt, 5);
    } else {
        sqlite3_bind_blob(stmt, 4, waveform.data(), (int)byteSize, SQLITE_TRANSIENT);
        if (hasMinWaveform) {
            sqlite3_bind_blob(stmt, 5, minWaveform.data(), (int)byteSize, SQLITE_TRANSIENT);
        } else {
            sqlite3_bind_null(stmt, 5);
        }
    }
    sqlite3_bind_int(stmt, 6, detailChannels);
    sqlite3_bind_int64(stmt, 7, getCurrentTimestamp());
    sqlite3_bind_text(stmt, 8, cacheKey.c_str(), -1, SQLITE_TRANSIENT);

    const bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

bool Database::clearWaveformDetailByPath(const std::string& filePath) {
    if (!m_db || filePath.empty()) return false;

    const char* sql = R"(
        UPDATE waveforms_db.waveforms
        SET waveform_detail = NULL,
            waveform_min_detail = NULL,
            detail_num_channels = 0
        WHERE file_id = (
            SELECT id
            FROM main.audio_files INDEXED BY idx_files_path
            WHERE file_path = ?
            LIMIT 1
        )
    )";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }

    sqlite3_bind_text(stmt, 1, filePath.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    bool changed = sqlite3_changes(m_db) > 0;

    const char* externalSql = R"(
        UPDATE waveforms_db.external_waveforms
        SET waveform_detail = NULL,
            waveform_min_detail = NULL,
            detail_num_channels = 0
        WHERE file_path = ?
    )";
    stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, externalSql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, filePath.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(stmt);
        changed = changed || sqlite3_changes(m_db) > 0;
        sqlite3_finalize(stmt);
    }

    return changed;
}

bool Database::clearWaveformCacheByPath(const std::string& filePath) {
    if (!m_db || filePath.empty()) return false;

    bool changed = false;
    sqlite3_stmt* stmt = nullptr;
    const int64_t now = getCurrentTimestamp();

    const char* ensureSql = R"(
        INSERT OR IGNORE INTO waveforms_db.waveforms (file_id, status, updated_at)
        SELECT id, 'pending', ?
        FROM main.audio_files INDEXED BY idx_files_path
        WHERE file_path = ?
        LIMIT 1
    )";
    if (sqlite3_prepare_v2(m_db, ensureSql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(stmt, 1, now);
        sqlite3_bind_text(stmt, 2, filePath.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        stmt = nullptr;
    }

    const char* sidecarSql = R"(
        UPDATE waveforms_db.waveforms
        SET waveform_data = NULL,
            mini_min_data = NULL,
            waveform_detail = NULL,
            waveform_min_detail = NULL,
            detail_num_channels = 0,
            rms_data = NULL,
            mipmap_data = NULL,
            true_rms_dbfs = NULL,
            mini_num_channels = 1,
            status = 'pending',
            error_code = '',
            retry_count = 0,
            updated_at = ?,
            cache_key = ''
        WHERE file_id = (
            SELECT id
            FROM main.audio_files INDEXED BY idx_files_path
            WHERE file_path = ?
            LIMIT 1
        )
    )";
    if (sqlite3_prepare_v2(m_db, sidecarSql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(stmt, 1, now);
        sqlite3_bind_text(stmt, 2, filePath.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(stmt);
        changed = changed || sqlite3_changes(m_db) > 0;
        sqlite3_finalize(stmt);
        stmt = nullptr;
    }

    const char* legacySql = R"(
        UPDATE main.audio_files
        SET waveform_data = NULL,
            waveform_detail = NULL,
            rms_data = NULL
        WHERE file_path = ?
    )";
    if (sqlite3_prepare_v2(m_db, legacySql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, filePath.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(stmt);
        changed = changed || sqlite3_changes(m_db) > 0;
        sqlite3_finalize(stmt);
        stmt = nullptr;
    }

    const char* externalSql = R"(
        UPDATE waveforms_db.external_waveforms
        SET waveform_detail = NULL,
            waveform_min_detail = NULL,
            detail_num_channels = 0,
            rms_data = NULL,
            updated_at = ?,
            cache_key = ''
        WHERE file_path = ?
    )";
    if (sqlite3_prepare_v2(m_db, externalSql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(stmt, 1, now);
        sqlite3_bind_text(stmt, 2, filePath.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(stmt);
        changed = changed || sqlite3_changes(m_db) > 0;
        sqlite3_finalize(stmt);
    }

    if (changed) {
        compactCachePackageBestEffort("clear_waveform_path");
    }
    return changed;
}

int Database::clearWaveformCacheByFolder(const std::string& folderPath) {
    if (!m_db || folderPath.empty()) return 0;

    const std::string targetPath = HarkenSearch::CanonicalFolderPath(folderPath);
    if (targetPath.empty()) return 0;
    const std::string childPrefix = MakeFolderChildPrefix(targetPath);
    const std::string childUpper = MakeFolderPathUpperBound(childPrefix);
    const int64_t now = getCurrentTimestamp();
    int total = 0;
    sqlite3_stmt* stmt = nullptr;

    const char* sidecarSql = R"(
        UPDATE waveforms_db.waveforms
        SET waveform_data = NULL,
            mini_min_data = NULL,
            waveform_detail = NULL,
            waveform_min_detail = NULL,
            detail_num_channels = 0,
            rms_data = NULL,
            mipmap_data = NULL,
            true_rms_dbfs = NULL,
            mini_num_channels = 1,
            status = 'pending',
            error_code = '',
            retry_count = 0,
            updated_at = ?,
            cache_key = ''
        WHERE file_id IN (
            SELECT id FROM main.audio_files
            WHERE folder_path = ? OR (folder_path >= ? AND folder_path < ?)
        )
    )";
    if (sqlite3_prepare_v2(m_db, sidecarSql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(stmt, 1, now);
        sqlite3_bind_text(stmt, 2, targetPath.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, childPrefix.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 4, childUpper.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(stmt);
        total += sqlite3_changes(m_db);
        sqlite3_finalize(stmt);
        stmt = nullptr;
    }

    const char* legacySql = R"(
        UPDATE main.audio_files
        SET waveform_data = NULL,
            waveform_detail = NULL,
            rms_data = NULL
        WHERE folder_path = ? OR (folder_path >= ? AND folder_path < ?)
    )";
    if (sqlite3_prepare_v2(m_db, legacySql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, targetPath.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, childPrefix.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, childUpper.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(stmt);
        total += sqlite3_changes(m_db);
        sqlite3_finalize(stmt);
        stmt = nullptr;
    }

    const char* externalSql = R"(
        UPDATE waveforms_db.external_waveforms
        SET waveform_detail = NULL,
            waveform_min_detail = NULL,
            detail_num_channels = 0,
            rms_data = NULL,
            updated_at = ?,
            cache_key = ''
        WHERE file_path IN (
            SELECT file_path FROM main.audio_files
            WHERE folder_path = ? OR (folder_path >= ? AND folder_path < ?)
        )
    )";
    if (sqlite3_prepare_v2(m_db, externalSql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(stmt, 1, now);
        sqlite3_bind_text(stmt, 2, targetPath.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, childPrefix.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 4, childUpper.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(stmt);
        total += sqlite3_changes(m_db);
        sqlite3_finalize(stmt);
    }

    if (total > 0) {
        compactCachePackageBestEffort("clear_waveform_folder");
    }
    return total;
}

int Database::purgeAllWaveformDetail() {
    if (!m_db) return 0;
    executeSQL("UPDATE waveforms_db.waveforms SET waveform_detail = NULL, waveform_min_detail = NULL, detail_num_channels = 0 WHERE waveform_detail IS NOT NULL OR waveform_min_detail IS NOT NULL");
    int changed = sqlite3_changes(m_db);
    executeSQL("UPDATE waveforms_db.external_waveforms SET waveform_detail = NULL, waveform_min_detail = NULL, detail_num_channels = 0 WHERE waveform_detail IS NOT NULL OR waveform_min_detail IS NOT NULL");
    changed += sqlite3_changes(m_db);
    if (changed > 0) {
        compactCachePackageBestEffort("purge_waveform_detail");
    }
    return changed;
}

int Database::purgeAllMiniWaveform() {
    if (!m_db) return 0;
    executeSQL("UPDATE waveforms_db.waveforms SET waveform_data = NULL, mini_min_data = NULL WHERE waveform_data IS NOT NULL OR mini_min_data IS NOT NULL");
    const int changed = sqlite3_changes(m_db);
    if (changed > 0) {
        compactCachePackageBestEffort("purge_waveform_mini");
    }
    return changed;
}


// Read waveform data directly from the sidecar database.
std::vector<float> Database::getWaveformDetailByPath(const std::string& filePath) {
    std::vector<float> result;
    if (!m_db) return result;
    
    std::string sql = R"(
        SELECT w.waveform_detail
        FROM main.audio_files af INDEXED BY idx_files_path
        LEFT JOIN waveforms_db.waveforms w ON af.id = w.file_id
        WHERE af.file_path = ?
        LIMIT 1
    )";
    
    // Reuse a pooled statement to avoid repeated prepare cost.
    sqlite3_stmt* stmt = checkoutStmt(sql);
    if (!stmt) return result;
    
    sqlite3_bind_text(stmt, 1, filePath.c_str(), -1, SQLITE_TRANSIENT);
    
    bool trackedLibraryRow = false;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        trackedLibraryRow = true;
        const void* blob = sqlite3_column_blob(stmt, 0);
        int blobSize = sqlite3_column_bytes(stmt, 0);
        if (blob && blobSize > 0) {
            CopyDetailMaxBlobColumn(stmt, 0, result);
        }
    }
    
    returnStmt(sql, stmt);
    if (trackedLibraryRow) return result;

    const std::vector<std::string> libraryRoots = getLibraryRoots();
    if (!libraryRoots.empty() && IsPathInsideLibraryRoots(filePath, libraryRoots)) {
        return result;
    }

    int64_t fileSize = 0;
    int64_t fileMtime = 0;
    if (!getExternalWaveformFingerprint(filePath, fileSize, fileMtime)) {
        return result;
    }

    const char* externalSql = R"(
        SELECT waveform_detail
        FROM waveforms_db.external_waveforms
        WHERE file_path = ?
          AND file_size = ?
          AND file_mtime = ?
          AND waveform_detail IS NOT NULL
          AND length(waveform_detail) > 0
        LIMIT 1
    )";
    sqlite3_stmt* externalStmt = nullptr;
    if (sqlite3_prepare_v2(m_db, externalSql, -1, &externalStmt, nullptr) != SQLITE_OK) {
        return result;
    }
    sqlite3_bind_text(externalStmt, 1, filePath.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(externalStmt, 2, fileSize);
    sqlite3_bind_int64(externalStmt, 3, fileMtime);
    if (sqlite3_step(externalStmt) == SQLITE_ROW) {
        const void* blob = sqlite3_column_blob(externalStmt, 0);
        int blobSize = sqlite3_column_bytes(externalStmt, 0);
        if (blob && blobSize > 0) {
            CopyDetailMaxBlobColumn(externalStmt, 0, result);
        }
    }
    sqlite3_finalize(externalStmt);
    return result;
}

std::vector<float> Database::getWaveformMinDetailByPath(const std::string& filePath) {
    std::vector<float> result;
    if (!m_db) return result;

    std::string sql = R"(
        SELECT w.waveform_detail, w.waveform_min_detail
        FROM main.audio_files af INDEXED BY idx_files_path
        LEFT JOIN waveforms_db.waveforms w ON af.id = w.file_id
        WHERE af.file_path = ?
        LIMIT 1
    )";

    sqlite3_stmt* stmt = checkoutStmt(sql);
    if (!stmt) return result;

    sqlite3_bind_text(stmt, 1, filePath.c_str(), -1, SQLITE_TRANSIENT);

    bool trackedLibraryRow = false;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        trackedLibraryRow = true;
        CopyDetailMinBlobColumns(stmt, 0, 1, result);
    }

    returnStmt(sql, stmt);
    if (trackedLibraryRow) return result;

    const std::vector<std::string> libraryRoots = getLibraryRoots();
    if (!libraryRoots.empty() && IsPathInsideLibraryRoots(filePath, libraryRoots)) {
        return result;
    }

    int64_t fileSize = 0;
    int64_t fileMtime = 0;
    if (!getExternalWaveformFingerprint(filePath, fileSize, fileMtime)) {
        return result;
    }

    const char* externalSql = R"(
        SELECT waveform_detail, waveform_min_detail
        FROM waveforms_db.external_waveforms
        WHERE file_path = ?
          AND file_size = ?
          AND file_mtime = ?
        LIMIT 1
    )";
    sqlite3_stmt* externalStmt = nullptr;
    if (sqlite3_prepare_v2(m_db, externalSql, -1, &externalStmt, nullptr) != SQLITE_OK) {
        return result;
    }
    sqlite3_bind_text(externalStmt, 1, filePath.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(externalStmt, 2, fileSize);
    sqlite3_bind_int64(externalStmt, 3, fileMtime);
    if (sqlite3_step(externalStmt) == SQLITE_ROW) {
        CopyDetailMinBlobColumns(externalStmt, 0, 1, result);
    }
    sqlite3_finalize(externalStmt);
    return result;
}

int Database::getWaveformDetailChannelsByPath(const std::string& filePath) {
    if (!m_db || filePath.empty()) return 0;

    const char* sql = R"(
        SELECT w.detail_num_channels
        FROM main.audio_files af INDEXED BY idx_files_path
        LEFT JOIN waveforms_db.waveforms w ON af.id = w.file_id
        WHERE af.file_path = ?
        LIMIT 1
    )";
    sqlite3_stmt* stmt = nullptr;
    int channels = 0;
    bool trackedLibraryRow = false;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, filePath.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            trackedLibraryRow = true;
            if (sqlite3_column_type(stmt, 0) != SQLITE_NULL) {
                channels = sqlite3_column_int(stmt, 0);
            }
        }
        sqlite3_finalize(stmt);
    }
    if (trackedLibraryRow) return channels;

    const std::vector<std::string> libraryRoots = getLibraryRoots();
    if (!libraryRoots.empty() && IsPathInsideLibraryRoots(filePath, libraryRoots)) {
        return 0;
    }

    int64_t fileSize = 0;
    int64_t fileMtime = 0;
    if (!getExternalWaveformFingerprint(filePath, fileSize, fileMtime)) {
        return 0;
    }

    const char* externalSql = R"(
        SELECT detail_num_channels
        FROM waveforms_db.external_waveforms
        WHERE file_path = ?
          AND file_size = ?
          AND file_mtime = ?
        LIMIT 1
    )";
    stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, externalSql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, filePath.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 2, fileSize);
        sqlite3_bind_int64(stmt, 3, fileMtime);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            channels = sqlite3_column_int(stmt, 0);
        }
        sqlite3_finalize(stmt);
    }
    return channels;
}

bool Database::hasWaveformDetailByPath(const std::string& filePath) {
    if (!m_db || filePath.empty()) return false;

    const char* sql = R"(
        SELECT w.waveform_detail
        FROM main.audio_files af INDEXED BY idx_files_path
        LEFT JOIN waveforms_db.waveforms w ON af.id = w.file_id
        WHERE af.file_path = ?
        LIMIT 1
    )";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }

    sqlite3_bind_text(stmt, 1, filePath.c_str(), -1, SQLITE_TRANSIENT);
    bool trackedLibraryRow = false;
    bool found = false;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        trackedLibraryRow = true;
        found = HasValidDetailBlobColumn(stmt, 0);
    }
    sqlite3_finalize(stmt);
    if (trackedLibraryRow) return found;

    const std::vector<std::string> libraryRoots = getLibraryRoots();
    if (!libraryRoots.empty() && IsPathInsideLibraryRoots(filePath, libraryRoots)) {
        return false;
    }

    int64_t fileSize = 0;
    int64_t fileMtime = 0;
    if (!getExternalWaveformFingerprint(filePath, fileSize, fileMtime)) {
        return false;
    }

    const char* externalSql = R"(
        SELECT waveform_detail
        FROM waveforms_db.external_waveforms
        WHERE file_path = ?
          AND file_size = ?
          AND file_mtime = ?
          AND waveform_detail IS NOT NULL
          AND length(waveform_detail) > 0
        LIMIT 1
    )";
    stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, externalSql, -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }
    sqlite3_bind_text(stmt, 1, filePath.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 2, fileSize);
    sqlite3_bind_int64(stmt, 3, fileMtime);
    bool externalFound = false;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        externalFound = HasValidDetailBlobColumn(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return externalFound;
}

bool Database::upsertWaveformCacheStateByPath(const std::string& filePath,
                                              const std::string& status,
                                              const std::string& errorCode,
                                              int retryCount,
                                              const std::string& cacheKey) {
    if (!m_db || filePath.empty()) return false;

    static const std::string sql = R"(
        INSERT INTO waveforms_db.waveforms(file_id, status, error_code, retry_count, updated_at, cache_key)
        SELECT id, ?, ?, ?, ?, ?
        FROM audio_files INDEXED BY idx_files_path
        WHERE file_path = ?
        ON CONFLICT(file_id) DO UPDATE SET
            status = excluded.status,
            error_code = excluded.error_code,
            retry_count = excluded.retry_count,
            updated_at = excluded.updated_at,
            cache_key = excluded.cache_key
    )";
    sqlite3_stmt* stmt = checkoutStmt(sql);
    if (!stmt) return false;

    const int64_t now = getCurrentTimestamp();
    sqlite3_bind_text(stmt, 1, status.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, errorCode.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 3, (std::max)(0, retryCount));
    sqlite3_bind_int64(stmt, 4, now);
    sqlite3_bind_text(stmt, 5, cacheKey.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 6, filePath.c_str(), -1, SQLITE_TRANSIENT);

    const bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    returnStmt(sql, stmt);
    return ok;
}

bool Database::getWaveformCacheStateByPath(const std::string& filePath, WaveformCacheState& out) {
    out = {};
    if (!m_db || filePath.empty()) return false;

    const char* sql = R"(
        SELECT w.status, w.error_code, w.retry_count, w.updated_at, w.cache_key
        FROM main.audio_files af INDEXED BY idx_files_path
        JOIN waveforms_db.waveforms w ON af.id = w.file_id
        WHERE af.file_path = ?
        LIMIT 1
    )";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }
    sqlite3_bind_text(stmt, 1, filePath.c_str(), -1, SQLITE_TRANSIENT);
    bool found = false;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        out.status = safeGetText(stmt, 0);
        out.errorCode = safeGetText(stmt, 1);
        out.retryCount = sqlite3_column_int(stmt, 2);
        out.updatedAt = sqlite3_column_int64(stmt, 3);
        out.cacheKey = safeGetText(stmt, 4);
        found = true;
    }
    sqlite3_finalize(stmt);
    return found;
}

int Database::fixCompletedPendingWaveformStates() {
    if (!m_db) return 0;
    const char* sql = R"(
        UPDATE waveforms_db.waveforms
        SET status = 'done',
            error_code = '',
            retry_count = 0,
            updated_at = ?
        WHERE status = 'pending'
          AND waveform_data IS NOT NULL
          AND waveform_detail IS NOT NULL
    )";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return 0;
    }
    sqlite3_bind_int64(stmt, 1, getCurrentTimestamp());
    const bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok ? sqlite3_changes(m_db) : 0;
}

bool Database::pruneWaveformCacheRows(int maxRows) {
    if (!m_db) return false;
    if (maxRows < 1000) maxRows = 1000;
    std::string sql = "DELETE FROM waveforms_db.waveforms "
                      "WHERE file_id IN ("
                      "  SELECT file_id FROM waveforms_db.waveforms "
                      "  ORDER BY updated_at ASC LIMIT "
                      "  (SELECT MAX(COUNT(*) - " + std::to_string(maxRows) + ", 0) FROM waveforms_db.waveforms)"
                      ")";
    return executeSQL(sql);
}

bool Database::clearWaveformCache() {
    if (!m_db) return false;
    bool ok = executeSQL("DELETE FROM waveforms_db.waveforms;");
    ok = executeSQL("DELETE FROM waveforms_db.external_waveforms;") && ok;
    ok = executeSQL(
        "UPDATE audio_files "
        "SET waveform_data = NULL, waveform_detail = NULL, rms_data = NULL "
        "WHERE waveform_data IS NOT NULL OR waveform_detail IS NOT NULL OR rms_data IS NOT NULL") && ok;
    if (ok) {
        compactCachePackageBestEffort("clear_waveform_all");
    }
    return ok;
}

// Get list of file IDs that need waveform caching
std::vector<int64_t> Database::getFilesNeedingWaveformCache(int limit) {
    std::vector<int64_t> result;
    if (!m_db) return result;
    
    // Current mini cache lives in the sidecar. Legacy main-table
    // audio_files.waveform_data may exist from older builds, but list rendering
    // no longer treats it as the authoritative persistent mini cache.
    // SEMANTIC (post-2026-06-15, mirrors WaveformCachePolicy::ShouldPersistScanMini):
    // every file with a missing sidecar mini blob enters the backfill queue,
    // regardless of duration. The earlier `duration >= 40.0` short-file skip
    // produced a perceptible first-paint gap in scrolling and was reverted.
    const char* sqlSidecar =
        "SELECT af.id FROM audio_files af "
        "LEFT JOIN waveforms_db.waveforms w ON af.id = w.file_id "
        "WHERE (w.waveform_data IS NULL OR length(w.waveform_data) = 0) "
        "AND (w.status IS NULL OR w.status <> 'failed') "
        "LIMIT ?";
    sqlite3_stmt* stmt;

    if (sqlite3_prepare_v2(m_db, sqlSidecar, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, limit);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            result.push_back(sqlite3_column_int64(stmt, 0));
        }
        sqlite3_finalize(stmt);
        return result;
    }

    // 回退:仅查主库(sidecar 不可用时)
    const char* sqlFallback =
        "SELECT id FROM audio_files "
        "WHERE waveform_data IS NULL "
        "LIMIT ?";
    if (sqlite3_prepare_v2(m_db, sqlFallback, -1, &stmt, nullptr) != SQLITE_OK) {
        return result;
    }
    sqlite3_bind_int(stmt, 1, limit);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        result.push_back(sqlite3_column_int64(stmt, 0));
    }
    sqlite3_finalize(stmt);
    return result;
}

int Database::countFilesNeedingWaveformCache() {
    if (!m_db) return 0;

    // Mirror getFilesNeedingWaveformCache: post-2026-06-15 every file with a
    // missing sidecar mini blob counts toward backfill, regardless of duration.
    const char* sqlSidecar =
        "SELECT COUNT(*) FROM audio_files af "
        "LEFT JOIN waveforms_db.waveforms w ON af.id = w.file_id "
        "WHERE (w.waveform_data IS NULL OR length(w.waveform_data) = 0) "
        "AND (w.status IS NULL OR w.status <> 'failed')";
    sqlite3_stmt* stmt = nullptr;

    if (sqlite3_prepare_v2(m_db, sqlSidecar, -1, &stmt, nullptr) == SQLITE_OK) {
        int count = 0;
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            count = sqlite3_column_int(stmt, 0);
        }
        sqlite3_finalize(stmt);
        return count;
    }

    const char* sqlFallback =
        "SELECT COUNT(*) FROM audio_files "
        "WHERE (waveform_data IS NULL OR length(waveform_data) = 0)";
    if (sqlite3_prepare_v2(m_db, sqlFallback, -1, &stmt, nullptr) != SQLITE_OK) {
        return 0;
    }

    int count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        count = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return count;
}

std::vector<int64_t> Database::getFilesNeedingDetailCache(int limit) {
    std::vector<int64_t> result;
    if (!m_db) return result;
    if (getWaveformCacheMode() == WaveformCachePolicy::Mode::MiniOnly) return result;
    
    // Prefer the sidecar cache. "Fill missing waveforms" means restoring the
    // user-visible readable cache, not upgrading every row to the heavier
    // rebuild-only min-envelope/channel metadata shape.
    const std::string detailMinBytes =
        "((CASE WHEN af.total_frames > 0 AND af.total_frames < (CASE WHEN af.duration > 120.0 THEN 1000 WHEN af.duration > 30.0 THEN 2000 ELSE 3000 END) THEN af.total_frames ELSE (CASE WHEN af.duration > 120.0 THEN 1000 WHEN af.duration > 30.0 THEN 2000 ELSE 3000 END) END) * "
        " (CASE WHEN af.channels > 0 THEN af.channels ELSE 1 END) * 4)";
    const std::string sqlSidecar =
        "SELECT af.id FROM audio_files af "
        "LEFT JOIN waveforms_db.waveforms w ON af.id = w.file_id "
        "WHERE " + WaveformCacheRetryableSqlExpr("w") + " AND "
        "(af.duration <= 0 OR af.duration <= 300.0) AND (("
        "  (af.waveform_detail IS NULL OR length(af.waveform_detail) < " + detailMinBytes + ") "
        "  AND " + DetailMissingSqlExpr("w", detailMinBytes) +
        ")) "
        "ORDER BY af.folder_path COLLATE NOCASE, af.file_name COLLATE NOCASE "
        "LIMIT ?";
    sqlite3_stmt* stmt;
    
    if (sqlite3_prepare_v2(m_db, sqlSidecar.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, limit);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            result.push_back(sqlite3_column_int64(stmt, 0));
        }
        sqlite3_finalize(stmt);
        return result;
    }
    
    // Fall back to the primary database only.
    const char* sqlFallback =
        "SELECT id FROM audio_files "
        "WHERE (duration <= 0 OR duration <= 300.0) AND "
        "  (waveform_detail IS NULL OR length(waveform_detail) < "
        "  ((CASE WHEN total_frames > 0 AND total_frames < (CASE WHEN duration > 120.0 THEN 1000 WHEN duration > 30.0 THEN 2000 ELSE 3000 END) THEN total_frames ELSE (CASE WHEN duration > 120.0 THEN 1000 WHEN duration > 30.0 THEN 2000 ELSE 3000 END) END) * "
        "   (CASE WHEN channels > 0 THEN channels ELSE 1 END) * 4) "
        "  ) "
        "ORDER BY folder_path COLLATE NOCASE, file_name COLLATE NOCASE "
        "LIMIT ?";
    if (sqlite3_prepare_v2(m_db, sqlFallback, -1, &stmt, nullptr) != SQLITE_OK) {
        return result;
    }
    sqlite3_bind_int(stmt, 1, AppState::WAVEFORM_DEFAULT_DETAIL_PEAKS);
    sqlite3_bind_int(stmt, 2, AppState::WAVEFORM_DEFAULT_DETAIL_PEAKS);
    sqlite3_bind_int(stmt, 3, limit);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        result.push_back(sqlite3_column_int64(stmt, 0));
    }
    sqlite3_finalize(stmt);
    return result;
}

int Database::countFilesNeedingDetailCache() {
    if (!m_db) return 0;
    if (getWaveformCacheMode() == WaveformCachePolicy::Mode::MiniOnly) return 0;

    const std::string detailMinBytes =
        "((CASE WHEN af.total_frames > 0 AND af.total_frames < (CASE WHEN af.duration > 120.0 THEN 1000 WHEN af.duration > 30.0 THEN 2000 ELSE 3000 END) THEN af.total_frames ELSE (CASE WHEN af.duration > 120.0 THEN 1000 WHEN af.duration > 30.0 THEN 2000 ELSE 3000 END) END) * "
        " (CASE WHEN af.channels > 0 THEN af.channels ELSE 1 END) * 4)";
    const std::string sqlSidecar =
        "SELECT COUNT(*) FROM audio_files af "
        "LEFT JOIN waveforms_db.waveforms w ON af.id = w.file_id "
        "WHERE " + WaveformCacheRetryableSqlExpr("w") + " AND "
        "(af.duration <= 0 OR af.duration <= 300.0) AND (("
        "  (af.waveform_detail IS NULL OR length(af.waveform_detail) < " + detailMinBytes + ") "
        "  AND " + DetailMissingSqlExpr("w", detailMinBytes) +
        "))";
    sqlite3_stmt* stmt = nullptr;

    if (sqlite3_prepare_v2(m_db, sqlSidecar.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
        int count = 0;
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            count = sqlite3_column_int(stmt, 0);
        }
        sqlite3_finalize(stmt);
        return count;
    }

    const char* sqlFallback =
        "SELECT COUNT(*) FROM audio_files "
        "WHERE (duration <= 0 OR duration <= 300.0) AND "
        "  (waveform_detail IS NULL OR length(waveform_detail) < "
        "  ((CASE WHEN total_frames > 0 AND total_frames < (CASE WHEN duration > 120.0 THEN 1000 WHEN duration > 30.0 THEN 2000 ELSE 3000 END) THEN total_frames ELSE (CASE WHEN duration > 120.0 THEN 1000 WHEN duration > 30.0 THEN 2000 ELSE 3000 END) END) * "
        "   (CASE WHEN channels > 0 THEN channels ELSE 1 END) * 4))";
    if (sqlite3_prepare_v2(m_db, sqlFallback, -1, &stmt, nullptr) != SQLITE_OK) {
        return 0;
    }

    sqlite3_bind_int(stmt, 1, AppState::WAVEFORM_DEFAULT_DETAIL_PEAKS);
    sqlite3_bind_int(stmt, 2, AppState::WAVEFORM_DEFAULT_DETAIL_PEAKS);
    int count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        count = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return count;
}

int Database::countFilesNeedingAnyWaveformCache() {
    if (!m_db) return 0;
    if (getWaveformCacheMode() == WaveformCachePolicy::Mode::MiniOnly) {
        return countFilesNeedingWaveformCache();
    }

    const std::string detailMinBytes =
        "((CASE WHEN af.total_frames > 0 AND af.total_frames < (CASE WHEN af.duration > 120.0 THEN 1000 WHEN af.duration > 30.0 THEN 2000 ELSE 3000 END) THEN af.total_frames ELSE (CASE WHEN af.duration > 120.0 THEN 1000 WHEN af.duration > 30.0 THEN 2000 ELSE 3000 END) END) * "
        " (CASE WHEN af.channels > 0 THEN af.channels ELSE 1 END) * 4)";
    const std::string sqlSidecar =
        "SELECT COUNT(*) FROM audio_files af "
        "LEFT JOIN waveforms_db.waveforms w ON af.id = w.file_id "
        "WHERE " + WaveformCacheRetryableSqlExpr("w") + " AND ("
        "  (w.waveform_data IS NULL OR length(w.waveform_data) = 0) "
        "  OR ((af.duration <= 0 OR af.duration <= 300.0) AND ("
        "    (af.waveform_detail IS NULL OR length(af.waveform_detail) < " + detailMinBytes + ") "
        "    AND " + DetailMissingSqlExpr("w", detailMinBytes) +
        "  )))";
    sqlite3_stmt* stmt = nullptr;

    if (sqlite3_prepare_v2(m_db, sqlSidecar.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
        int count = 0;
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            count = sqlite3_column_int(stmt, 0);
        }
        sqlite3_finalize(stmt);
        return count;
    }

    const char* sqlFallback =
        "SELECT COUNT(*) FROM audio_files "
        "WHERE (waveform_data IS NULL OR length(waveform_data) = 0) "
        "   OR ((duration <= 0 OR duration <= 300.0) AND "
        "       (waveform_detail IS NULL OR length(waveform_detail) < "
        "       ((CASE WHEN total_frames > 0 AND total_frames < (CASE WHEN duration > 120.0 THEN 1000 WHEN duration > 30.0 THEN 2000 ELSE 3000 END) THEN total_frames ELSE (CASE WHEN duration > 120.0 THEN 1000 WHEN duration > 30.0 THEN 2000 ELSE 3000 END) END) * "
        "        (CASE WHEN channels > 0 THEN channels ELSE 1 END) * 4)))";
    if (sqlite3_prepare_v2(m_db, sqlFallback, -1, &stmt, nullptr) != SQLITE_OK) {
        return 0;
    }

    int count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        count = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return count;
}

int64_t Database::countAllFilesForWaveformRebuild() {
    if (!m_db) return 0;

    // Rebuild progress must match the actual file queue, not the folder_index
    // cache used by fast folder UI counts. folder_index can be temporarily stale.
    const char* sql = "SELECT COUNT(*) FROM audio_files";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return 0;
    }

    int64_t count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        count = sqlite3_column_int64(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return count;
}

int64_t Database::countFilesEligibleForPersistentDetailCache() {
    if (!m_db) return 0;
    if (getWaveformCacheMode() == WaveformCachePolicy::Mode::MiniOnly) return 0;

    const char* sql =
        "SELECT COUNT(*) FROM audio_files "
        "WHERE duration <= 0 OR duration <= 300.0";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return 0;
    }

    int64_t count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        count = sqlite3_column_int64(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return count;
}

bool Database::updateFilePath(const std::string& oldPath, const std::string& newPath, const std::string& newFileName) {
    if (!m_db || oldPath.empty() || newPath.empty()) return false;

    std::string finalName = newFileName;
    if (finalName.empty()) {
        try {
            fs::path p = Utf8ToPath(newPath);
            finalName = PathToUtf8(p.filename());
        } catch (...) {
            finalName = "";
        }
    }

    std::string newFolderPath;
    try {
        fs::path p = Utf8ToPath(newPath);
        fs::path parent = p.parent_path();
        if (parent != p) newFolderPath = PathToUtf8(parent);
        // Match scan normalization: remove the trailing separator so equivalent paths share one key.
        while (!newFolderPath.empty() && (newFolderPath.back() == '/' || newFolderPath.back() == '\\'))
            newFolderPath.pop_back();
    } catch (...) {
    }
    // T2.1: canonicalize folder_path before bind so updateFilePath stays
    // consistent with addFile (T1.0 v2 §5.1). file_path keeps raw form.
    newFolderPath = HarkenSearch::CanonicalFolderPath(newFolderPath);

    // Update path fields and clear folder artwork in one statement to keep list and detail views aligned.
    const char* sql = "UPDATE audio_files SET file_path = ?, file_name = ?, "
                      "folder_path = ?, folder_cover_key = NULL, folder_cover_src_path = NULL, folder_cover_src_mtime = 0, "
                      "search_data = aura_search_data(? || ' ' || IFNULL(title,'') || ' ' || IFNULL(description,'') || ' ' || IFNULL(category,'') || ' ' || IFNULL(keywords,'')) WHERE file_path = ?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        m_lastError = sqlite3_errmsg(m_db);
        return false;
    }

    sqlite3_bind_text(stmt, 1, newPath.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, finalName.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, newFolderPath.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, finalName.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, oldPath.c_str(), -1, SQLITE_TRANSIENT);

    bool ok = (sqlite3_step(stmt) == SQLITE_DONE);
    if (!ok) m_lastError = sqlite3_errmsg(m_db);
    sqlite3_finalize(stmt);
    if (ok) {
        std::vector<std::string> changedPaths;
        changedPaths.reserve(2);
        if (!oldPath.empty()) changedPaths.push_back(oldPath);
        if (!newPath.empty()) changedPaths.push_back(newPath);
        invalidateFacetQueryCache();
        invalidateFolderCoverFacetIndexForPaths(changedPaths);
        bumpDataRevision();  // Make the rename visible to other tabs.
        // Invalidate the UI artwork cache after the path changes.
        CoverModule::InvalidateCoverForPath(oldPath);
        if (!newPath.empty() && newPath != oldPath) CoverModule::InvalidateCoverForPath(newPath);
    }
    return ok;
}

bool Database::updatePathPrefix(const std::string& oldFolderPath, const std::string& newFolderPath) {
    if (!m_db || oldFolderPath.empty() || newFolderPath.empty()) return false;

    std::string oldPrefix = oldFolderPath;
    std::string newPrefix = newFolderPath;

    char sep = fs::path::preferred_separator;
    if (oldPrefix.back() != '\\' && oldPrefix.back() != '/') oldPrefix.push_back(sep);
    if (newPrefix.back() != '\\' && newPrefix.back() != '/') newPrefix.push_back(sep);

    // ---- 1. UPDATE file_path (raw, 保留原大小写) ----
    const char* sqlFile =
        "UPDATE audio_files "
        "SET file_path = (?1 || substr(file_path, ?2)) "
        "WHERE file_path LIKE ?3;";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sqlFile, -1, &stmt, nullptr) != SQLITE_OK) {
        m_lastError = sqlite3_errmsg(m_db);
        return false;
    }

    std::string pattern = oldPrefix + "%";
    int startPos = (int)oldPrefix.size() + 1; // sqlite substr is 1-based

    sqlite3_bind_text(stmt, 1, newPrefix.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, startPos);
    sqlite3_bind_text(stmt, 3, pattern.c_str(), -1, SQLITE_TRANSIENT);

    bool ok = (sqlite3_step(stmt) == SQLITE_DONE);
    if (!ok) m_lastError = sqlite3_errmsg(m_db);
    sqlite3_finalize(stmt);
    if (!ok) return false;

    // Update canonical folder_path values after the file-path prefix change.
    // Leaving the old value would break filters, counts, and cover inheritance.
    // Match canonical forms because the column has already been migrated.

    std::string canonOld = HarkenSearch::CanonicalFolderPath(oldFolderPath);
    std::string canonNew = HarkenSearch::CanonicalFolderPath(newFolderPath);
    if (canonOld.empty() || canonNew.empty()) {
        m_lastError = "updatePathPrefix: canonical normalization yielded empty";
        return false;
    }
    // canonical 形态去尾 separator (CanonicalFolderPath 已 strip)
    // SQL 匹配 2 类 folder_path: 等于 canonOld 自身 (folder 在 oldFolder 下) +
    //                           以 canonOld + '\\' 开头的子目录
    sqlite3_stmt* fpStmt = nullptr;
    const char* sqlFolder =
        "UPDATE audio_files SET folder_path = "
        "  CASE "
        "    WHEN folder_path = ?1 THEN ?2 "
        "    ELSE ?2 || substr(folder_path, ?3) "
        "  END "
        "WHERE folder_path = ?1 OR folder_path LIKE ?4;";
    if (sqlite3_prepare_v2(m_db, sqlFolder, -1, &fpStmt, nullptr) != SQLITE_OK) {
        m_lastError = sqlite3_errmsg(m_db);
        return false;
    }
    std::string canonOldWithSep = canonOld + '\\';
    std::string canonOldPattern = canonOldWithSep + "%";
    int canonStartPos = (int)canonOld.size() + 1;  // 1-based, skip canonOld prefix
    sqlite3_bind_text(fpStmt, 1, canonOld.c_str(),         -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(fpStmt, 2, canonNew.c_str(),         -1, SQLITE_TRANSIENT);
    sqlite3_bind_int (fpStmt, 3, canonStartPos);
    sqlite3_bind_text(fpStmt, 4, canonOldPattern.c_str(),  -1, SQLITE_TRANSIENT);
    bool fpOk = (sqlite3_step(fpStmt) == SQLITE_DONE);
    if (!fpOk) m_lastError = sqlite3_errmsg(m_db);
    sqlite3_finalize(fpStmt);
    if (!fpOk) return false;

    // Remove affected folder_index rows after updating folder_path.
    // The next query can rebuild from audio_files GROUP BY on demand.

    sqlite3_stmt* fiStmt = nullptr;
    const char* sqlFi =
        "DELETE FROM folder_index "
        "WHERE folder_path = ?1 OR folder_path LIKE ?2;";
    if (sqlite3_prepare_v2(m_db, sqlFi, -1, &fiStmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(fiStmt, 1, canonOld.c_str(),        -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(fiStmt, 2, canonOldPattern.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(fiStmt);  // best-effort, 不阻塞主成功路径
        sqlite3_finalize(fiStmt);
    }
    return true;
}

// F-034 relink lookup by fingerprint
// Thread safety: must be called from the main thread only (uses m_db directly).
std::optional<Database::RelinkCandidate>
Database::findOrphanByFingerprint(const std::string& fileName,
                                   int64_t fileSize,
                                   int64_t totalFrames,
                                   int     sampleRate)
{
    const char* sql =
        "SELECT id, file_path, file_name, file_size, total_frames, sample_rate "
        "FROM audio_files "
        "WHERE file_name = ? AND file_size = ? AND total_frames = ? AND sample_rate = ?";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return std::nullopt;

    // Use SQLITE_TRANSIENT: SQLite copies the string before bind returns,
    // eliminating any lifetime dependency on the caller's std::string.
    sqlite3_bind_text (stmt, 1, fileName.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 2, fileSize);
    sqlite3_bind_int64(stmt, 3, totalFrames);
    sqlite3_bind_int  (stmt, 4, sampleRate);

    std::optional<RelinkCandidate> result;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        auto* rawPath = sqlite3_column_text(stmt, 1);
        if (!rawPath) continue;
        std::string dbPath = reinterpret_cast<const char*>(rawPath);
        try {
            if (Platform::FileSystem::ProbePath(dbPath) ==
                Platform::FileSystem::PathStatus::Missing) {
                RelinkCandidate c;
                c.id          = sqlite3_column_int  (stmt, 0);
                c.dbPath      = std::move(dbPath);
                auto* rawName = sqlite3_column_text(stmt, 2);
                c.fileName    = rawName ? reinterpret_cast<const char*>(rawName) : "";
                c.fileSize    = sqlite3_column_int64(stmt, 3);
                c.totalFrames = sqlite3_column_int64(stmt, 4);
                c.sampleRate  = sqlite3_column_int  (stmt, 5);
                result = std::move(c);
                break;
            }
        } catch (...) {}
    }
    sqlite3_finalize(stmt);
    return result;
}

// F-034 relink candidates
// Thread safety: opens a private read-only SQLite connection so it is safe to
// call from background threads while the main thread continues to use m_db.
std::vector<std::pair<Database::RelinkCandidate, std::vector<std::string>>>
Database::findRelinkCandidates(const std::vector<std::string>& searchRoots,
                                std::atomic<bool>* cancelFlag)
{
    // Open a dedicated read-only connection for background thread safety.
    sqlite3* roDb = nullptr;
    if (!m_mainDbPath.empty()) {
        sqlite3_open_v2(m_mainDbPath.c_str(), &roDb,
                        SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX, nullptr);
        if (roDb) {
            sqlite3_busy_timeout(roDb, 5000);
        }
    }
    // Fall back to m_db (main-thread use only) if no path available.
    sqlite3* db = roDb ? roDb : m_db;
    if (!db) return {};

    // Step A: load orphaned records whose paths no longer exist on disk.
    const char* orphanSql =
        "SELECT id, file_path, file_name, file_size, total_frames, sample_rate "
        "FROM audio_files";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, orphanSql, -1, &stmt, nullptr) != SQLITE_OK) {
        if (roDb) sqlite3_close(roDb);
        return {};
    }

    std::vector<RelinkCandidate> orphans;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (cancelFlag && cancelFlag->load()) break;
        auto* rawPath = sqlite3_column_text(stmt, 1);
        if (!rawPath) continue;
        std::string dbPath = reinterpret_cast<const char*>(rawPath);
        try {
            if (Platform::FileSystem::ProbePath(dbPath) ==
                Platform::FileSystem::PathStatus::Missing) {
                RelinkCandidate c;
                c.id          = sqlite3_column_int  (stmt, 0);
                c.dbPath      = std::move(dbPath);
                auto* rawName = sqlite3_column_text(stmt, 2);
                c.fileName    = rawName ? reinterpret_cast<const char*>(rawName) : "";
                c.fileSize    = sqlite3_column_int64(stmt, 3);
                c.totalFrames = sqlite3_column_int64(stmt, 4);
                c.sampleRate  = sqlite3_column_int  (stmt, 5);
                orphans.push_back(std::move(c));
            }
        } catch (...) {}
    }
    sqlite3_finalize(stmt);
    if (roDb) sqlite3_close(roDb);

    if (orphans.empty()) return {};

    std::vector<std::pair<RelinkCandidate, std::vector<std::string>>> results(orphans.size());
    for (size_t i = 0; i < orphans.size(); ++i)
        results[i].first = orphans[i];

    // Step B: build an O(1) map keyed by file name and size.
    using NameSizeKey = std::pair<std::string, int64_t>;
    struct NameSizeHash {
        size_t operator()(const NameSizeKey& k) const {
            size_t h = std::hash<std::string>{}(k.first);
            h ^= std::hash<int64_t>{}(k.second) + 0x9e3779b9 + (h << 6) + (h >> 2);
            return h;
        }
    };
    std::unordered_map<NameSizeKey, std::vector<size_t>, NameSizeHash> nameSizeIndex;
    for (size_t i = 0; i < orphans.size(); ++i) {
        nameSizeIndex[{orphans[i].fileName, orphans[i].fileSize}].push_back(i);
    }

    // Step C: walk search roots, filter audio files, and probe the map in O(1).
    for (const auto& root : searchRoots) {
        if (cancelFlag && cancelFlag->load()) break;
        try {
            Platform::FileSystem::DirectoryIterator iterator;
            if (!iterator.Open(root, true, true)) continue;
            Platform::FileSystem::DirectoryEntry entry;
            while (true) {
                if (cancelFlag && cancelFlag->load()) break;
                const auto status = iterator.Next(entry);
                if (status == Platform::FileSystem::DirectoryReadStatus::End) break;
                if (status == Platform::FileSystem::DirectoryReadStatus::Error) continue;
                if (!entry.isRegularFile) continue;
                const auto nativePath = Utf8ToPath(entry.pathUtf8);
                auto ext = nativePath.extension().string();
                std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                if (!isAudioFile(ext)) continue;

                std::string diskName = PathToUtf8(nativePath.filename());
                const auto fileInfo = Platform::FileSystem::ProbeRegularFile(entry.pathUtf8);
                if (!fileInfo.isRegularFile || !fileInfo.sizeAvailable) continue;
                const int64_t diskSize = static_cast<int64_t>(fileInfo.sizeBytes);

                auto it = nameSizeIndex.find({diskName, diskSize});
                if (it != nameSizeIndex.end()) {
                    const std::string& fullPath = entry.pathUtf8;
                    for (size_t idx : it->second) {
                        results[idx].second.push_back(fullPath);
                    }
                }
            }
        } catch (...) {}
    }

    return results;
}

void Database::mergeLibraryRoots(const std::vector<std::string>& roots) {
    if (!m_db) return;
    // Persist roots with original case while deduplicating by a normalized key.
    // Re-adding an existing root replaces a legacy lowercase value in place,
    // keeping startup enumeration consistent with case-sensitive file paths.

    std::vector<std::string> merged = getLibraryRoots();
    auto keyOf = [](const std::string& s) {
        return NormalizeLibraryPathUtf8(s, /*lowerCaseForCompare=*/true);
    };
    std::map<std::string, size_t> keyToIndex;
    for (size_t i = 0; i < merged.size(); ++i) {
        keyToIndex[keyOf(merged[i])] = i;
    }
    for (const std::string& root : roots) {
        const std::string key = keyOf(root);
        if (key.empty()) continue;
        const std::string preservedCase =
            NormalizeLibraryPathUtf8(root, /*lowerCaseForCompare=*/false);
        if (preservedCase.empty()) continue;
        auto it = keyToIndex.find(key);
        if (it == keyToIndex.end()) {
            keyToIndex[key] = merged.size();
            merged.push_back(preservedCase);
        } else {
            // Replace the stored value for an existing key so original casing
            // can be restored without creating a duplicate root.

            merged[it->second] = preservedCase;
        }
    }
    // Sort by lowercased key for stable output, but keep original-case
    // strings in the persisted list.
    std::sort(merged.begin(), merged.end(), [](const std::string& a, const std::string& b) {
        std::string la = NormalizeLibraryPathUtf8(a, true);
        std::string lb = NormalizeLibraryPathUtf8(b, true);
        return la < lb;
    });
    // Defensive dedup in case getLibraryRoots() already had case duplicates.
    {
        std::set<std::string> finalKeys;
        std::vector<std::string> out;
        out.reserve(merged.size());
        for (auto& s : merged) {
            const std::string k = NormalizeLibraryPathUtf8(s, true);
            if (finalKeys.insert(k).second) out.push_back(std::move(s));
        }
        merged.swap(out);
    }

    std::string encoded;
    for (size_t i = 0; i < merged.size(); ++i) {
        if (i) encoded.push_back('\n');
        encoded += merged[i];
    }
    WriteAppMetaString(m_db, kLibraryRootsMetaKey, encoded);

    executeSQL("CREATE TABLE IF NOT EXISTS network_library_roots (path TEXT PRIMARY KEY);");
    executeSQL("DELETE FROM network_library_roots;");
    sqlite3_stmt* insertStmt = nullptr;
    if (sqlite3_prepare_v2(
            m_db,
            "INSERT OR IGNORE INTO network_library_roots(path) VALUES(?);",
            -1,
            &insertStmt,
            nullptr) == SQLITE_OK) {
        for (const auto& root : merged) {
            if (!Platform::FileSystem::IsNetworkPath(root)) continue;
            sqlite3_reset(insertStmt);
            sqlite3_bind_text(insertStmt, 1, root.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_step(insertStmt);
        }
        sqlite3_finalize(insertStmt);
    }
}

std::vector<std::string> Database::getLibraryRoots() const {
    if (!m_db) return {};
    return ParseLibraryRoots(ReadAppMetaString(m_db, kLibraryRootsMetaKey));
}

std::vector<std::string> Database::getNetworkLibraryRoots() const {
    std::vector<std::string> roots;
    if (!m_db) return roots;
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(
            m_db,
            "SELECT path FROM network_library_roots",
            -1,
            &stmt,
            nullptr) != SQLITE_OK) {
        return roots;
    }
    std::set<std::string> seen;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const std::string preserved = NormalizeLibraryPathUtf8(safeGetText(stmt, 0), false);
        if (preserved.empty()) continue;
        const std::string key = NormalizeLibraryPathUtf8(preserved, true);
        if (seen.insert(key).second) roots.push_back(preserved);
    }
    sqlite3_finalize(stmt);
    return roots;
}

bool Database::isNetworkLibraryRoot(const std::string& pathUtf8) const {
    const std::string needle = NormalizeLibraryPathUtf8(pathUtf8, true);
    if (needle.empty()) return false;
    for (const auto& root : getNetworkLibraryRoots()) {
        if (NormalizeLibraryPathUtf8(root, true) == needle) return true;
    }
    return false;
}

bool Database::isNetworkLibraryFile(const std::string& pathUtf8) const {
    const std::vector<std::string> roots = getNetworkLibraryRoots();
    if (roots.empty()) return false;
    return IsPathInsideLibraryRoots(pathUtf8, roots);
}

int Database::pruneEntriesOutsideLibraryRoots() {
    if (!m_db) return 0;
    const std::vector<std::string> roots = getLibraryRoots();
    if (roots.empty()) return 0;

    std::vector<int64_t> idsToDelete;
    const char* sql = "SELECT id, file_path, folder_path FROM audio_files";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) return 0;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const int64_t id = sqlite3_column_int64(stmt, 0);
        const std::string filePath = safeGetText(stmt, 1);
        std::string folderPath = safeGetText(stmt, 2);
        if (folderPath.empty() && !filePath.empty()) {
            try {
                folderPath = PathToUtf8(Utf8ToPath(filePath).parent_path());
            } catch (...) {
                folderPath.clear();
            }
        }
        if (!IsPathInsideLibraryRoots(folderPath, roots)) {
            idsToDelete.push_back(id);
        }
    }
    sqlite3_finalize(stmt);
    if (idsToDelete.empty()) return 0;

    beginTransaction();
    const char* deleteSql = "DELETE FROM audio_files WHERE id = ?";
    sqlite3_stmt* deleteStmt = nullptr;
    if (sqlite3_prepare_v2(m_db, deleteSql, -1, &deleteStmt, nullptr) != SQLITE_OK) {
        rollbackTransaction();
        return 0;
    }

    int deleted = 0;
    for (int64_t id : idsToDelete) {
        sqlite3_reset(deleteStmt);
        sqlite3_clear_bindings(deleteStmt);
        sqlite3_bind_int64(deleteStmt, 1, id);
        if (sqlite3_step(deleteStmt) == SQLITE_DONE) {
            ++deleted;
        }
    }
    sqlite3_finalize(deleteStmt);
    commitTransaction();
    pruneOrphanCoverCaches();
    clearFolderIndex();
    invalidateFacetQueryCache();
    return deleted;
}

// F-034 watched roots
std::vector<std::string> Database::getWatchedRoots() const {
    std::vector<std::string> roots;
    if (!m_db) return roots;
    // directories table uses parent_id IS NULL to identify root-level entries.
    // The table is created lazily during scan, so it may not exist yet on a
    // fresh DB where the user has only registered roots via mergeLibraryRoots.
    // In that case prepare_v2 fails; fall through to getLibraryRoots() so
    // T-296 startup reconciliation still sees the registered roots.
    const char* sql = "SELECT folder_path FROM directories WHERE parent_id IS NULL";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            auto* p = sqlite3_column_text(stmt, 0);
            if (p) roots.emplace_back(reinterpret_cast<const char*>(p));
        }
        sqlite3_finalize(stmt);
    }
    if (roots.empty()) {
        roots = getLibraryRoots();
    }
    return roots;
}

// Playlist operations
int64_t Database::createPlaylist(const std::string& name, const std::string& description) {
    if (!m_db) return -1;

    const char* sql = "INSERT INTO playlists (name, description, date_created) VALUES (?, ?, ?)";
    sqlite3_stmt* stmt;

    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return -1;
    }

    sqlite3_bind_text(stmt, 1, name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, description.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 3, getCurrentTimestamp());

    if (sqlite3_step(stmt) != SQLITE_DONE) {
        sqlite3_finalize(stmt);
        return -1;
    }

    sqlite3_finalize(stmt);
    bumpDataRevision();
    return getLastInsertId();
}

bool Database::markAsFolder(int64_t playlistId) {
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db,
        "UPDATE playlists SET is_folder = 1 WHERE id = ?", -1, &stmt, nullptr) != SQLITE_OK)
        return false;
    sqlite3_bind_int64(stmt, 1, playlistId);
    bool ok = (sqlite3_step(stmt) == SQLITE_DONE);
    sqlite3_finalize(stmt);
    return ok && sqlite3_changes(m_db) > 0;
}

bool Database::setPlaylistParent(int64_t playlistId, int64_t newParentId) {
    if (playlistId <= 0) {
        DIAG_ACTION_FMT("Playlist",
                        "SetParentRejected: reason=invalid_child child=%lld parent=%lld",
                        (long long)playlistId,
                        (long long)newParentId);
        return false;
    }

    // Reject self-loop.
    if (newParentId == playlistId) {
        DIAG_ACTION_FMT("Playlist",
                        "SetParentRejected: reason=self_loop child=%lld parent=%lld",
                        (long long)playlistId,
                        (long long)newParentId);
        return false;
    }

    // Folders cannot themselves become children (1-level depth cap).
    if (isFolder(playlistId)) {
        DIAG_ACTION_FMT("Playlist",
                        "SetParentRejected: reason=child_is_folder child=%lld parent=%lld",
                        (long long)playlistId,
                        (long long)newParentId);
        return false;
    }

    if (newParentId > 0) {
        // Target parent must exist and must be a folder.
        if (!isFolder(newParentId)) {
            DIAG_ACTION_FMT("Playlist",
                            "SetParentRejected: reason=target_not_folder child=%lld parent=%lld",
                            (long long)playlistId,
                            (long long)newParentId);
            return false;
        }
    }

    sqlite3_stmt* stmt = nullptr;
    const char* sql = (newParentId > 0)
        ? "UPDATE playlists SET parent_id = ? WHERE id = ?"
        : "UPDATE playlists SET parent_id = NULL WHERE id = ?";
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        DIAG_ACTION_FMT("Playlist",
                        "SetParentRejected: reason=prepare_failed child=%lld parent=%lld",
                        (long long)playlistId,
                        (long long)newParentId);
        return false;
    }
    if (newParentId > 0) {
        sqlite3_bind_int64(stmt, 1, newParentId);
        sqlite3_bind_int64(stmt, 2, playlistId);
    } else {
        sqlite3_bind_int64(stmt, 1, playlistId);
    }
    bool ok = (sqlite3_step(stmt) == SQLITE_DONE);
    sqlite3_finalize(stmt);
    if (!ok || sqlite3_changes(m_db) <= 0) {
        DIAG_ACTION_FMT("Playlist",
                        "SetParentRejected: reason=apply_failed child=%lld parent=%lld",
                        (long long)playlistId,
                        (long long)newParentId);
        return false;
    }
    DIAG_ACTION_FMT("Playlist",
                    "SetParentApplied: child=%lld parent=%lld",
                    (long long)playlistId,
                    (long long)newParentId);
    return true;
}

bool Database::isFolder(int64_t playlistId) {
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db,
        "SELECT is_folder FROM playlists WHERE id = ?", -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_int64(stmt, 1, playlistId);
    bool isFolderResult = false;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        isFolderResult = sqlite3_column_int(stmt, 0) != 0;
    }
    sqlite3_finalize(stmt);
    return isFolderResult;
}

int64_t Database::createSubPlaylist(int64_t parentId, const std::string& name,
                                    const std::string& description) {
    if (!isFolder(parentId)) return -1;  // 父必须先是 folder

    // 同父下重名校验
    sqlite3_stmt* dup = nullptr;
    if (sqlite3_prepare_v2(m_db,
        "SELECT 1 FROM playlists WHERE parent_id = ? AND name = ? LIMIT 1",
        -1, &dup, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(dup, 1, parentId);
        sqlite3_bind_text(dup, 2, name.c_str(), -1, SQLITE_TRANSIENT);
        bool exists = (sqlite3_step(dup) == SQLITE_ROW);
        sqlite3_finalize(dup);
        if (exists) return -1;
    }

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db,
        "INSERT INTO playlists (name, description, date_created, parent_id, is_folder) "
        "VALUES (?, ?, ?, ?, 0)",
        -1, &stmt, nullptr) != SQLITE_OK) return -1;
    sqlite3_bind_text(stmt, 1, name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, description.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 3, getCurrentTimestamp());
    sqlite3_bind_int64(stmt, 4, parentId);
    bool ok = (sqlite3_step(stmt) == SQLITE_DONE);
    sqlite3_finalize(stmt);
    return ok ? getLastInsertId() : -1;
}

std::vector<Playlist> Database::getChildrenOf(int64_t parentId) {
    std::vector<Playlist> result;
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db,
        "SELECT id, name, description, date_created, parent_id, is_folder "
        "FROM playlists WHERE parent_id = ? ORDER BY name COLLATE AURA_NATURAL",
        -1, &stmt, nullptr) != SQLITE_OK)
        return result;
    sqlite3_bind_int64(stmt, 1, parentId);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        Playlist p;
        p.id = sqlite3_column_int64(stmt, 0);
        p.name = safeGetText(stmt, 1);
        p.description = safeGetText(stmt, 2);
        p.dateCreated = sqlite3_column_int64(stmt, 3);
        p.parent_id = (sqlite3_column_type(stmt, 4) == SQLITE_NULL) ? -1 : sqlite3_column_int64(stmt, 4);
        p.is_folder = sqlite3_column_int(stmt, 5) != 0;
        result.push_back(std::move(p));
    }
    sqlite3_finalize(stmt);
    return result;
}

bool Database::deletePlaylist(int64_t playlistId) {
    if (!m_db) return false;

    if (sqlite3_exec(m_db, "BEGIN TRANSACTION", nullptr, nullptr, nullptr) != SQLITE_OK)
        return false;
    auto rollback = [&]() { sqlite3_exec(m_db, "ROLLBACK", nullptr, nullptr, nullptr); };

    bool wasFolder = isFolder(playlistId);
    if (wasFolder) {
        // Cascade: delete children's playlist_items
        sqlite3_stmt* clrItems = nullptr;
        if (sqlite3_prepare_v2(m_db,
            "DELETE FROM playlist_items WHERE playlist_id IN "
            "(SELECT id FROM playlists WHERE parent_id = ?)",
            -1, &clrItems, nullptr) != SQLITE_OK) { rollback(); return false; }
        sqlite3_bind_int64(clrItems, 1, playlistId);
        if (sqlite3_step(clrItems) != SQLITE_DONE) { sqlite3_finalize(clrItems); rollback(); return false; }
        sqlite3_finalize(clrItems);

        // Cascade: delete children themselves
        sqlite3_stmt* clrKids = nullptr;
        if (sqlite3_prepare_v2(m_db,
            "DELETE FROM playlists WHERE parent_id = ?",
            -1, &clrKids, nullptr) != SQLITE_OK) { rollback(); return false; }
        sqlite3_bind_int64(clrKids, 1, playlistId);
        if (sqlite3_step(clrKids) != SQLITE_DONE) { sqlite3_finalize(clrKids); rollback(); return false; }
        sqlite3_finalize(clrKids);
    }

    // Delete own playlist_items then the playlist row itself
    {
        sqlite3_stmt* clrSelf = nullptr;
        if (sqlite3_prepare_v2(m_db,
            "DELETE FROM playlist_items WHERE playlist_id = ?",
            -1, &clrSelf, nullptr) != SQLITE_OK) { rollback(); return false; }
        sqlite3_bind_int64(clrSelf, 1, playlistId);
        if (sqlite3_step(clrSelf) != SQLITE_DONE) { sqlite3_finalize(clrSelf); rollback(); return false; }
        sqlite3_finalize(clrSelf);
    }
    {
        sqlite3_stmt* delPl = nullptr;
        if (sqlite3_prepare_v2(m_db,
            "DELETE FROM playlists WHERE id = ?",
            -1, &delPl, nullptr) != SQLITE_OK) { rollback(); return false; }
        sqlite3_bind_int64(delPl, 1, playlistId);
        if (sqlite3_step(delPl) != SQLITE_DONE) { sqlite3_finalize(delPl); rollback(); return false; }
        sqlite3_finalize(delPl);
    }

    sqlite3_exec(m_db, "COMMIT", nullptr, nullptr, nullptr);
    bumpDataRevision();
    if (wasFolder) {
        DIAG_ACTION_FMT("Database", "PlaylistFolderDeleted: id=%lld", (long long)playlistId);
    }
    return true;
}

int64_t Database::copyPlaylist(int64_t playlistId, const std::string& newName) {
    if (!m_db || playlistId <= 0 || newName.empty() || isFolder(playlistId)) return -1;

    sqlite3_stmt* src = nullptr;
    if (sqlite3_prepare_v2(m_db,
        "SELECT description, parent_id FROM playlists WHERE id = ? AND is_folder = 0",
        -1, &src, nullptr) != SQLITE_OK) {
        return -1;
    }
    sqlite3_bind_int64(src, 1, playlistId);
    if (sqlite3_step(src) != SQLITE_ROW) {
        sqlite3_finalize(src);
        return -1;
    }
    const std::string description = safeGetText(src, 0);
    const bool hasParent = sqlite3_column_type(src, 1) != SQLITE_NULL;
    const int64_t parentId = hasParent ? sqlite3_column_int64(src, 1) : -1;
    sqlite3_finalize(src);

    if (hasParent && !isFolder(parentId)) return -1;

    if (sqlite3_exec(m_db, "BEGIN TRANSACTION", nullptr, nullptr, nullptr) != SQLITE_OK) {
        return -1;
    }
    auto rollback = [&]() -> int64_t {
        sqlite3_exec(m_db, "ROLLBACK", nullptr, nullptr, nullptr);
        return -1;
    };

    sqlite3_stmt* ins = nullptr;
    const char* insertSql = hasParent
        ? "INSERT INTO playlists (name, description, date_created, parent_id, is_folder) VALUES (?, ?, ?, ?, 0)"
        : "INSERT INTO playlists (name, description, date_created, is_folder) VALUES (?, ?, ?, 0)";
    if (sqlite3_prepare_v2(m_db, insertSql, -1, &ins, nullptr) != SQLITE_OK) {
        return rollback();
    }
    sqlite3_bind_text(ins, 1, newName.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(ins, 2, description.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(ins, 3, getCurrentTimestamp());
    if (hasParent) sqlite3_bind_int64(ins, 4, parentId);
    if (sqlite3_step(ins) != SQLITE_DONE) {
        sqlite3_finalize(ins);
        return rollback();
    }
    sqlite3_finalize(ins);
    const int64_t newId = sqlite3_last_insert_rowid(m_db);

    sqlite3_stmt* copyItems = nullptr;
    if (sqlite3_prepare_v2(m_db,
        "INSERT INTO playlist_items (playlist_id, file_id, position) "
        "SELECT ?, file_id, position FROM playlist_items WHERE playlist_id = ? ORDER BY position",
        -1, &copyItems, nullptr) != SQLITE_OK) {
        return rollback();
    }
    sqlite3_bind_int64(copyItems, 1, newId);
    sqlite3_bind_int64(copyItems, 2, playlistId);
    if (sqlite3_step(copyItems) != SQLITE_DONE) {
        sqlite3_finalize(copyItems);
        return rollback();
    }
    sqlite3_finalize(copyItems);

    if (sqlite3_exec(m_db, "COMMIT", nullptr, nullptr, nullptr) != SQLITE_OK) {
        return rollback();
    }
    bumpDataRevision();
    DIAG_ACTION_FMT("Database", "PlaylistCopied source=%lld copy=%lld",
                    (long long)playlistId, (long long)newId);
    return newId;
}

int64_t Database::promoteOrphanToFolderWithDefaultChild(int64_t orphanId,
                                                        const std::string& defaultChildName) {
    if (!m_db) return -1;
    if (isFolder(orphanId)) return -1;  // already a folder, misuse

    if (sqlite3_exec(m_db, "BEGIN TRANSACTION", nullptr, nullptr, nullptr) != SQLITE_OK)
        return -1;
    auto rollback = [&]() -> int64_t {
        sqlite3_exec(m_db, "ROLLBACK", nullptr, nullptr, nullptr);
        return -1;
    };

    // 1. Insert default child (parent_id=orphanId, is_folder=0)
    sqlite3_stmt* ins = nullptr;
    if (sqlite3_prepare_v2(m_db,
        "INSERT INTO playlists (name, description, date_created, parent_id, is_folder) "
        "VALUES (?, '', ?, ?, 0)",
        -1, &ins, nullptr) != SQLITE_OK) return rollback();
    sqlite3_bind_text(ins, 1, defaultChildName.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(ins, 2, static_cast<int64_t>(std::time(nullptr)));
    sqlite3_bind_int64(ins, 3, orphanId);
    if (sqlite3_step(ins) != SQLITE_DONE) { sqlite3_finalize(ins); return rollback(); }
    sqlite3_finalize(ins);
    int64_t childId = sqlite3_last_insert_rowid(m_db);

    // 2. Move all playlist_items from orphan to new child
    sqlite3_stmt* upd = nullptr;
    if (sqlite3_prepare_v2(m_db,
        "UPDATE playlist_items SET playlist_id = ? WHERE playlist_id = ?",
        -1, &upd, nullptr) != SQLITE_OK) return rollback();
    sqlite3_bind_int64(upd, 1, childId);
    sqlite3_bind_int64(upd, 2, orphanId);
    if (sqlite3_step(upd) != SQLITE_DONE) { sqlite3_finalize(upd); return rollback(); }
    sqlite3_finalize(upd);

    // 3. Mark orphan as folder
    sqlite3_stmt* mark = nullptr;
    if (sqlite3_prepare_v2(m_db,
        "UPDATE playlists SET is_folder = 1 WHERE id = ?",
        -1, &mark, nullptr) != SQLITE_OK) return rollback();
    sqlite3_bind_int64(mark, 1, orphanId);
    if (sqlite3_step(mark) != SQLITE_DONE) { sqlite3_finalize(mark); return rollback(); }
    sqlite3_finalize(mark);

    sqlite3_exec(m_db, "COMMIT", nullptr, nullptr, nullptr);
    bumpDataRevision();
    DIAG_ACTION_FMT("Database",
        "OrphanPromoted: orphanId=%lld defaultChildId=%lld",
        (long long)orphanId, (long long)childId);
    return childId;
}

bool Database::moveSubPlaylistToParent(int64_t subId, int64_t newParentId) {
    if (!m_db) return false;
    if (isFolder(subId)) return false;  // folder 不能改父(深度限制)
    if (newParentId > 0 && !isFolder(newParentId)) return false;  // 目标必须是 folder

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db,
        "UPDATE playlists SET parent_id = ? WHERE id = ?",
        -1, &stmt, nullptr) != SQLITE_OK) return false;
    if (newParentId > 0) sqlite3_bind_int64(stmt, 1, newParentId);
    else                 sqlite3_bind_null(stmt, 1);
    sqlite3_bind_int64(stmt, 2, subId);
    bool ok = (sqlite3_step(stmt) == SQLITE_DONE && sqlite3_changes(m_db) > 0);
    sqlite3_finalize(stmt);
    if (ok) bumpDataRevision();
    return ok;
}

bool Database::renamePlaylist(int64_t playlistId, const std::string& newName) {
    if (!m_db || newName.empty()) return false;

    const char* sql = "UPDATE playlists SET name = ? WHERE id = ?";
    sqlite3_stmt* stmt;

    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        m_lastError = sqlite3_errmsg(m_db);
        return false;
    }

    sqlite3_bind_text(stmt, 1, newName.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 2, playlistId);

    bool success = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    if (success && sqlite3_changes(m_db) > 0) bumpDataRevision();
    return success;
}

std::vector<Playlist> Database::getAllPlaylists(bool includeFileCounts) {
    std::vector<Playlist> results;
    if (!m_db) return results;

    // Startup/sidebar needs playlist names immediately. Counting every
    // playlist item can scan a large membership table, so callers may request
    // metadata first and let counts arrive in a second deferred refresh.
    const char* sqlWithCounts =
        "SELECT p.id, p.name, p.description, p.date_created, COUNT(pi.file_id), "
        "p.parent_id, p.is_folder "
        "FROM playlists p "
        "LEFT JOIN playlist_items pi ON p.id = pi.playlist_id "
        "GROUP BY p.id ORDER BY p.name COLLATE AURA_NATURAL";
    const char* sqlMetadataOnly =
        "SELECT p.id, p.name, p.description, p.date_created, -1, "
        "p.parent_id, p.is_folder "
        "FROM playlists p "
        "ORDER BY p.name COLLATE AURA_NATURAL";
    const char* sql = includeFileCounts ? sqlWithCounts : sqlMetadataOnly;
    sqlite3_stmt* stmt;

    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return results;
    }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        Playlist p;
        p.id = sqlite3_column_int64(stmt, 0);
        p.name = safeGetText(stmt, 1);
        p.description = safeGetText(stmt, 2);
        p.dateCreated = sqlite3_column_int64(stmt, 3);
        p.fileCount = sqlite3_column_int64(stmt, 4); // Count computed by the database.
        p.parent_id = (sqlite3_column_type(stmt, 5) == SQLITE_NULL) ? -1 : sqlite3_column_int64(stmt, 5);
        p.is_folder = sqlite3_column_int(stmt, 6) != 0;
        results.push_back(p);
    }

    sqlite3_finalize(stmt);
    return results;
}

bool Database::addToPlaylist(int64_t playlistId, int64_t fileId) {
    if (!m_db) return false;
    if (isFolder(playlistId)) {
        DIAG_ACTION_FMT("Database",
            "RejectedAddToFolder: playlistId=%lld fileId=%lld",
            (long long)playlistId, (long long)fileId);
        return false;
    }

    const char* sql =
        "INSERT OR IGNORE INTO playlist_items (playlist_id, file_id, position) "
        "VALUES (?, ?, COALESCE((SELECT MAX(position) + 1 FROM playlist_items WHERE playlist_id = ?), 0))";
    sqlite3_stmt* stmt;

    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }

    sqlite3_bind_int64(stmt, 1, playlistId);
    sqlite3_bind_int64(stmt, 2, fileId);
    sqlite3_bind_int64(stmt, 3, playlistId);

    bool success = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    if (success && sqlite3_changes(m_db) > 0) bumpDataRevision();
    return success;
}

Database::PlaylistBulkAddResult Database::addManyToPlaylist(int64_t playlistId,
                                                            const std::vector<int64_t>& fileIds) {
    PlaylistBulkAddResult result;
    result.playlistId = playlistId;
    result.requestedFiles = static_cast<int>(fileIds.size());
    if (!m_db) {
        result.error = "database is not open";
        return result;
    }
    if (playlistId <= 0) {
        result.error = "invalid playlist id";
        return result;
    }
    if (isFolder(playlistId)) {
        result.error = "target is a playlist folder";
        DIAG_ACTION_FMT("Database",
            "RejectedBulkAddToFolder: playlistId=%lld count=%d",
            (long long)playlistId, result.requestedFiles);
        return result;
    }
    if (fileIds.empty()) return result;

    const bool startedTransaction = sqlite3_get_autocommit(m_db) != 0;
    auto rollbackIfStarted = [&]() {
        if (startedTransaction) sqlite3_exec(m_db, "ROLLBACK", nullptr, nullptr, nullptr);
    };
    if (startedTransaction &&
        sqlite3_exec(m_db, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr) != SQLITE_OK) {
        result.error = sqlite3_errmsg(m_db);
        return result;
    }

    int64_t nextPosition = 0;
    sqlite3_stmt* posStmt = nullptr;
    if (sqlite3_prepare_v2(m_db,
            "SELECT COALESCE(MAX(position) + 1, 0) FROM playlist_items WHERE playlist_id = ?",
            -1, &posStmt, nullptr) != SQLITE_OK) {
        result.error = sqlite3_errmsg(m_db);
        rollbackIfStarted();
        return result;
    }
    sqlite3_bind_int64(posStmt, 1, playlistId);
    if (sqlite3_step(posStmt) == SQLITE_ROW) {
        nextPosition = sqlite3_column_int64(posStmt, 0);
    }
    sqlite3_finalize(posStmt);

    sqlite3_stmt* insertStmt = nullptr;
    if (sqlite3_prepare_v2(m_db,
            "INSERT OR IGNORE INTO playlist_items (playlist_id, file_id, position) VALUES (?, ?, ?)",
            -1, &insertStmt, nullptr) != SQLITE_OK) {
        result.error = sqlite3_errmsg(m_db);
        rollbackIfStarted();
        return result;
    }

    for (int64_t fileId : fileIds) {
        if (fileId <= 0) {
            ++result.invalidFiles;
            continue;
        }
        sqlite3_reset(insertStmt);
        sqlite3_clear_bindings(insertStmt);
        sqlite3_bind_int64(insertStmt, 1, playlistId);
        sqlite3_bind_int64(insertStmt, 2, fileId);
        sqlite3_bind_int64(insertStmt, 3, nextPosition);
        if (sqlite3_step(insertStmt) != SQLITE_DONE) {
            result.error = sqlite3_errmsg(m_db);
            sqlite3_finalize(insertStmt);
            rollbackIfStarted();
            return result;
        }
        if (sqlite3_changes(m_db) > 0) {
            ++result.addedFiles;
            ++nextPosition;
        } else {
            ++result.alreadyInPlaylist;
        }
    }
    sqlite3_finalize(insertStmt);

    if (startedTransaction &&
        sqlite3_exec(m_db, "COMMIT", nullptr, nullptr, nullptr) != SQLITE_OK) {
        result.error = sqlite3_errmsg(m_db);
        sqlite3_exec(m_db, "ROLLBACK", nullptr, nullptr, nullptr);
        return result;
    }

    if (result.addedFiles > 0) bumpDataRevision();
    DIAG_ACTION_FMT("Playlist",
        "BulkAdd playlist=%lld requested=%d added=%d duplicate=%d invalid=%d",
        (long long)playlistId,
        result.requestedFiles,
        result.addedFiles,
        result.alreadyInPlaylist,
        result.invalidFiles);
    return result;
}

static bool ParseReaperQuotedValue(const std::string& line, const char* prefix, std::string& out) {
    out.clear();
    if (!prefix) return false;
    const size_t prefixLen = std::strlen(prefix);
    if (line.compare(0, prefixLen, prefix) != 0) return false;

    const size_t firstQuote = line.find('"', prefixLen);
    if (firstQuote == std::string::npos) return false;
    const size_t secondQuote = line.find('"', firstQuote + 1);
    if (secondQuote == std::string::npos || secondQuote <= firstQuote + 1) return false;

    out = line.substr(firstQuote + 1, secondQuote - firstQuote - 1);
    return !out.empty();
}

static void StripUtf8Bom(std::string& line) {
    if (line.size() >= 3 &&
        static_cast<unsigned char>(line[0]) == 0xEF &&
        static_cast<unsigned char>(line[1]) == 0xBB &&
        static_cast<unsigned char>(line[2]) == 0xBF) {
        line.erase(0, 3);
    }
}

static std::string NormalizeReaperImportPath(const std::string& pathUtf8,
                                             const std::string& currentPathUtf8) {
    if (pathUtf8.empty()) return {};
    try {
        fs::path p = Utf8ToPath(pathUtf8);
        if (p.is_relative() && !currentPathUtf8.empty()) {
            p = Utf8ToPath(currentPathUtf8) / p;
        }
        return PathToUtf8(p);
    } catch (...) {
        return pathUtf8;
    }
}

static std::string ReaperImportFormatFromExtension(std::string ext) {
    std::transform(ext.begin(), ext.end(), ext.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (ext == ".wav" || ext == ".wave") return "WAV";
    if (ext == ".mp3") return "MP3";
    if (ext == ".flac") return "FLAC";
    if (ext == ".ogg") return "OGG";
    if (ext == ".aiff" || ext == ".aif") return "AIFF";
    if (ext == ".m4a") return "M4A";
    if (ext == ".wma") return "WMA";
    return "Unknown";
}

static bool BuildReaperImportAudioRecord(const std::string& pathUtf8, AudioRecord& out) {
    out = AudioRecord{};
    if (pathUtf8.empty()) return false;

    try {
        fs::path path = Utf8ToPath(pathUtf8);
        const auto fileInfo = Platform::FileSystem::ProbeRegularFile(pathUtf8);
        if (!fileInfo.isRegularFile) return false;

        const std::string ext = PathToUtf8(path.extension());
        if (!isAudioFile(ext)) return false;

        out.filePath = PathToUtf8(path);
        out.fileName = PathToUtf8(path.filename());
        out.extension = ext;
        out.folderPath = PathToUtf8(path.parent_path());
        out.format = ReaperImportFormatFromExtension(ext);
        out.fileSize = fileInfo.sizeAvailable ? fileInfo.sizeBytes : 0;
        out.fileMtime = Platform::FileSystem::LastWriteTimeUnixSeconds(pathUtf8);
        out.dateAdded = getCurrentTimestamp();
        out.lastIndexed = out.dateAdded;
        return !out.filePath.empty() && !out.fileName.empty();
    } catch (...) {
        return false;
    }
}

Database::PlaylistImportResult Database::importReaperFileListToPlaylist(const std::string& fileListPath,
                                                                        const std::string& playlistName,
                                                                        ScanProgressCallback callback,
                                                                        std::atomic<bool>* cancelFlag) {
    PlaylistImportResult result;
    if (!m_db) {
        result.error = "database is not open";
        return result;
    }

    Platform::FileSystem::StreamReader reader;
    if (!reader.Open(fileListPath, true)) {
        result.error = "failed to open Reaper file list";
        return result;
    }

    std::array<char, 8192> readBuffer = {};
    std::string pendingLine;
    auto readNextLine = [&](std::string& outLine) {
        for (;;) {
            const size_t newline = pendingLine.find('\n');
            if (newline != std::string::npos) {
                outLine.assign(pendingLine, 0, newline);
                pendingLine.erase(0, newline + 1);
                return true;
            }
            const size_t bytesRead = reader.Read(readBuffer.data(), readBuffer.size());
            if (bytesRead == 0) {
                if (pendingLine.empty()) return false;
                outLine = std::move(pendingLine);
                pendingLine.clear();
                return true;
            }
            pendingLine.append(readBuffer.data(), bytesRead);
        }
    };

    std::vector<std::string> orderedPaths;
    orderedPaths.reserve(1024);
    std::unordered_set<std::string> seenPaths;
    std::string currentPath;
    std::string line;
    if (callback) callback(0, 1, "Parsing REAPER MediaDB/FileList...", false);
    while (readNextLine(line)) {
        if (cancelFlag && cancelFlag->load(std::memory_order_acquire)) {
            result.error = "cancelled";
            return result;
        }
        StripUtf8Bom(line);

        std::string quoted;
        if (ParseReaperQuotedValue(line, "PATH ", quoted)) {
            currentPath = NormalizeReaperImportPath(quoted, {});
            continue;
        }
        if (!ParseReaperQuotedValue(line, "FILE ", quoted)) {
            continue;
        }

        ++result.parsedFiles;
        std::string normalized = NormalizeReaperImportPath(quoted, currentPath);
        if (normalized.empty()) continue;
        if (!seenPaths.insert(normalized).second) {
            ++result.duplicateInputFiles;
            continue;
        }
        orderedPaths.push_back(std::move(normalized));
    }

    result.uniqueFiles = static_cast<int>(orderedPaths.size());
    if (callback) callback(0, std::max(result.uniqueFiles, 1), "REAPER paths deduplicated", false);
    if (result.parsedFiles <= 0) {
        result.error = "no FILE records found in Reaper file list";
        return result;
    }
    if (orderedPaths.empty()) {
        result.error = "no usable FILE paths found in Reaper file list";
        return result;
    }

    constexpr size_t kSqliteBindChunk = 450;
    std::unordered_map<std::string, int64_t> idByPath;
    idByPath.reserve(orderedPaths.size());
    for (size_t offset = 0; offset < orderedPaths.size(); offset += kSqliteBindChunk) {
        const size_t n = std::min(kSqliteBindChunk, orderedPaths.size() - offset);
        std::string sql = "SELECT id, file_path FROM audio_files WHERE file_path IN (";
        for (size_t i = 0; i < n; ++i) {
            if (i > 0) sql += ",";
            sql += "?";
        }
        sql += ")";

        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(m_db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
            result.error = sqlite3_errmsg(m_db);
            return result;
        }
        for (size_t i = 0; i < n; ++i) {
            sqlite3_bind_text(stmt, static_cast<int>(i + 1), orderedPaths[offset + i].c_str(), -1, SQLITE_TRANSIENT);
        }
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const int64_t id = sqlite3_column_int64(stmt, 0);
            const char* path = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
            if (id > 0 && path && *path) {
                idByPath[path] = id;
            }
        }
        sqlite3_finalize(stmt);
    }

    std::vector<int64_t> orderedIds;
    orderedIds.reserve(orderedPaths.size());
    constexpr size_t kMissingSampleLimit = 128;
    sqlite3_stmt* lookupIdStmt = nullptr;
    sqlite3_prepare_v2(m_db,
        "SELECT id FROM audio_files WHERE file_path = ? LIMIT 1",
        -1, &lookupIdStmt, nullptr);
    int processedPaths = 0;
    auto reportImportProgress = [&](const std::string& path, bool force = false) {
        if (!callback) return;
        if (!force && processedPaths % 64 != 0 && processedPaths != result.uniqueFiles) return;
        callback(processedPaths, std::max(result.uniqueFiles, 1), path, false);
    };
    for (const auto& path : orderedPaths) {
        if (cancelFlag && cancelFlag->load(std::memory_order_acquire)) {
            result.error = "cancelled";
            if (lookupIdStmt) sqlite3_finalize(lookupIdStmt);
            return result;
        }
        auto it = idByPath.find(path);
        if (it == idByPath.end()) {
            AudioRecord record;
            if (!BuildReaperImportAudioRecord(path, record)) {
                ++result.missingFiles;
                try {
                    fs::path diskPath = Utf8ToPath(path);
                    if (Platform::FileSystem::ProbePath(PathToUtf8(diskPath)) ==
                        Platform::FileSystem::PathStatus::Present) {
                        ++result.unsupportedFiles;
                    } else {
                        ++result.missingOnDiskFiles;
                    }
                } catch (...) {
                    ++result.missingOnDiskFiles;
                }
                if (result.missingPathSamples.size() < kMissingSampleLimit) {
                    result.missingPathSamples.push_back(path);
                }
                ++processedPaths;
                reportImportProgress(path);
                continue;
            }

            if (!addFile(record)) {
                ++result.missingFiles;
                ++result.failedIndexFiles;
                if (result.missingPathSamples.size() < kMissingSampleLimit) {
                    result.missingPathSamples.push_back(path);
                }
                ++processedPaths;
                reportImportProgress(path);
                continue;
            }

            int64_t newId = 0;
            if (lookupIdStmt) {
                sqlite3_reset(lookupIdStmt);
                sqlite3_clear_bindings(lookupIdStmt);
                sqlite3_bind_text(lookupIdStmt, 1, record.filePath.c_str(), -1, SQLITE_TRANSIENT);
                if (sqlite3_step(lookupIdStmt) == SQLITE_ROW) {
                    newId = sqlite3_column_int64(lookupIdStmt, 0);
                }
            }
            if (newId <= 0) {
                ++result.missingFiles;
                ++result.failedIndexFiles;
                if (result.missingPathSamples.size() < kMissingSampleLimit) {
                    result.missingPathSamples.push_back(path);
                }
                ++processedPaths;
                reportImportProgress(path);
                continue;
            }

            ++result.indexedFiles;
            orderedIds.push_back(newId);
            ++processedPaths;
            reportImportProgress(path);
            continue;
        }
        orderedIds.push_back(it->second);
        ++processedPaths;
        reportImportProgress(path);
    }
    reportImportProgress("Preparing playlist entries...", true);
    if (lookupIdStmt) sqlite3_finalize(lookupIdStmt);
    result.matchedFiles = static_cast<int>(orderedIds.size());
    if (orderedIds.empty()) {
        result.error = "no Reaper paths could be imported into the current Harken Search database";
        return result;
    }

    if (sqlite3_exec(m_db, "BEGIN TRANSACTION", nullptr, nullptr, nullptr) != SQLITE_OK) {
        result.error = sqlite3_errmsg(m_db);
        return result;
    }
    auto rollback = [&]() {
        sqlite3_exec(m_db, "ROLLBACK", nullptr, nullptr, nullptr);
    };

    const std::string finalName = playlistName.empty()
        ? PathToUtf8(Utf8ToPath(fileListPath).stem())
        : playlistName;
    result.playlistId = createPlaylist(finalName.empty() ? "Reaper Import" : finalName,
                                       "Imported from Reaper MediaDB/FileList");
    if (result.playlistId <= 0) {
        result.error = "failed to create playlist";
        rollback();
        return result;
    }

    int64_t nextPosition = 0;
    {
        sqlite3_stmt* posStmt = nullptr;
        if (sqlite3_prepare_v2(m_db,
            "SELECT COALESCE(MAX(position) + 1, 0) FROM playlist_items WHERE playlist_id = ?",
            -1, &posStmt, nullptr) != SQLITE_OK) {
            result.error = sqlite3_errmsg(m_db);
            rollback();
            return result;
        }
        sqlite3_bind_int64(posStmt, 1, result.playlistId);
        if (sqlite3_step(posStmt) == SQLITE_ROW) {
            nextPosition = sqlite3_column_int64(posStmt, 0);
        }
        sqlite3_finalize(posStmt);
    }

    sqlite3_stmt* insertStmt = nullptr;
    if (sqlite3_prepare_v2(m_db,
        "INSERT OR IGNORE INTO playlist_items (playlist_id, file_id, position) VALUES (?, ?, ?)",
        -1, &insertStmt, nullptr) != SQLITE_OK) {
        result.error = sqlite3_errmsg(m_db);
        rollback();
        return result;
    }

    for (int64_t fileId : orderedIds) {
        sqlite3_reset(insertStmt);
        sqlite3_clear_bindings(insertStmt);
        sqlite3_bind_int64(insertStmt, 1, result.playlistId);
        sqlite3_bind_int64(insertStmt, 2, fileId);
        sqlite3_bind_int64(insertStmt, 3, nextPosition);
        if (sqlite3_step(insertStmt) != SQLITE_DONE) {
            result.error = sqlite3_errmsg(m_db);
            sqlite3_finalize(insertStmt);
            rollback();
            return result;
        }
        if (sqlite3_changes(m_db) > 0) {
            ++result.addedFiles;
            ++nextPosition;
        } else {
            ++result.alreadyInPlaylist;
        }
    }
    sqlite3_finalize(insertStmt);

    if (sqlite3_exec(m_db, "COMMIT", nullptr, nullptr, nullptr) != SQLITE_OK) {
        result.error = sqlite3_errmsg(m_db);
        rollback();
        return result;
    }

    bumpDataRevision();
    if (callback) callback(result.uniqueFiles, std::max(result.uniqueFiles, 1), "REAPER import committed", true);
    DIAG_ACTION_FMT("Playlist",
        "ReaperFileListImport playlist=%lld parsed=%d unique=%d matched=%d indexed=%d added=%d missing=%d missingOnDisk=%d unsupported=%d failedIndex=%d duplicateInput=%d",
        (long long)result.playlistId,
        result.parsedFiles,
        result.uniqueFiles,
        result.matchedFiles,
        result.indexedFiles,
        result.addedFiles,
        result.missingFiles,
        result.missingOnDiskFiles,
        result.unsupportedFiles,
        result.failedIndexFiles,
        result.duplicateInputFiles);
    return result;
}

bool Database::removeFromPlaylist(int64_t playlistId, int64_t fileId) {
    if (!m_db) return false;

    const char* sql = "DELETE FROM playlist_items WHERE playlist_id = ? AND file_id = ?";
    sqlite3_stmt* stmt;

    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }

    sqlite3_bind_int64(stmt, 1, playlistId);
    sqlite3_bind_int64(stmt, 2, fileId);

    bool success = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    if (success && sqlite3_changes(m_db) > 0) bumpDataRevision();
    return success;
}

std::vector<AudioRecord> Database::getPlaylistFiles(int64_t playlistId) {
    std::vector<AudioRecord> results;
    if (!m_db) return results;

    std::string sql = std::string("SELECT ") + kRecordColumnsAf +
        " FROM audio_files af"
        " INNER JOIN playlist_items pi ON af.id = pi.file_id"
        " WHERE pi.playlist_id = ?"
        " ORDER BY pi.position, af.file_name COLLATE AURA_NATURAL, pi.file_id";

    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(m_db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        return results;
    }

    sqlite3_bind_int64(stmt, 1, playlistId);

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        results.push_back(recordFromStatement(stmt));
    }

    sqlite3_finalize(stmt);
    return results;
}

std::vector<AudioRecord> Database::getPlaylistFilesPage(int64_t playlistId, int limit, int offset) {
    if (limit <= 0) {
        return getPlaylistFiles(playlistId);
    }

    std::vector<AudioRecord> results;
    if (!m_db) return results;
    if (offset < 0) offset = 0;

    std::string sql = std::string("SELECT ") + kRecordColumnsAf +
        " FROM audio_files af"
        " INNER JOIN playlist_items pi ON af.id = pi.file_id"
        " WHERE pi.playlist_id = ?"
        " ORDER BY pi.position, af.file_name COLLATE AURA_NATURAL, pi.file_id"
        " LIMIT ? OFFSET ?";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        return results;
    }

    sqlite3_bind_int64(stmt, 1, playlistId);
    sqlite3_bind_int(stmt, 2, limit);
    sqlite3_bind_int(stmt, 3, offset);

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        results.push_back(recordFromStatement(stmt));
    }

    sqlite3_finalize(stmt);
    return results;
}

static void ApplyScopedSearchFtsFallback(sqlite3* db, Database::BuiltSearchQuery& bq, const SearchFilter& filter) {
    if (bq.hasFtsJoin && !ftsHasRows(db)) {
        std::string tok = firstToken(filter.keyword);
        if (!tok.empty()) {
            bq.fromClause = "FROM audio_files af";
            bq.whereClause = "WHERE af.file_name LIKE ? COLLATE NOCASE";
            bq.bindValues.clear();
            bq.coverKeyBindValues.clear();
            bq.coverKeySubquery.clear();
            bq.bindValues.push_back(tok + "%");
            bq.hasFtsJoin = false;
            bq.canUseFtsSnippet = false;
        }
    }
}

std::vector<AudioRecord> Database::searchPlaylistFilesPage(int64_t playlistId,
                                                           const SearchFilter& filter,
                                                           int limit,
                                                           int offset) {
    std::vector<AudioRecord> results;
    if (!m_db || playlistId <= 0 || limit <= 0) return results;
    if (offset < 0) offset = 0;

    BuiltSearchQuery bq = buildSearchWhere(filter);
    if (bq.parseError) {
        m_lastError = bq.errorMsg;
        return results;
    }
    ApplyScopedSearchFtsFallback(m_db, bq, filter);

    const std::string coverInClause = bq.coverKeySubquery.empty() ? std::string()
        : " AND af.id IN (" + bq.coverKeySubquery + ")";
    const std::string sql =
        "WITH search_candidates AS ("
        " SELECT af.id AS id " + bq.fromClause + " " + bq.whereClause + coverInClause +
        ") SELECT " + std::string(kRecordColumnsAf) +
        " FROM playlist_items pi"
        " JOIN search_candidates sc ON sc.id = pi.file_id"
        " JOIN audio_files af ON af.id = pi.file_id"
        " WHERE pi.playlist_id = ?"
        " ORDER BY pi.position, af.file_name COLLATE AURA_NATURAL, pi.file_id"
        " LIMIT ? OFFSET ?";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        m_lastError = sqlite3_errmsg(m_db);
        return results;
    }
    int bindIndex = BindSearchAndCoverQuery(stmt, bq);
    sqlite3_bind_int64(stmt, bindIndex++, playlistId);
    sqlite3_bind_int(stmt, bindIndex++, limit);
    sqlite3_bind_int(stmt, bindIndex++, offset);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        results.push_back(recordFromStatement(stmt));
    }
    sqlite3_finalize(stmt);
    return results;
}

int64_t Database::searchPlaylistFileCount(int64_t playlistId, const SearchFilter& filter) {
    if (!m_db || playlistId <= 0) return 0;

    BuiltSearchQuery bq = buildSearchWhere(filter);
    if (bq.parseError) {
        m_lastError = bq.errorMsg;
        return 0;
    }
    ApplyScopedSearchFtsFallback(m_db, bq, filter);

    const std::string coverInClause = bq.coverKeySubquery.empty() ? std::string()
        : " AND af.id IN (" + bq.coverKeySubquery + ")";
    const std::string sql =
        "WITH search_candidates AS ("
        " SELECT af.id AS id " + bq.fromClause + " " + bq.whereClause + coverInClause +
        ") SELECT COUNT(*)"
        " FROM playlist_items pi"
        " JOIN search_candidates sc ON sc.id = pi.file_id"
        " WHERE pi.playlist_id = ?";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        m_lastError = sqlite3_errmsg(m_db);
        return 0;
    }
    int bindIndex = BindSearchAndCoverQuery(stmt, bq);
    sqlite3_bind_int64(stmt, bindIndex++, playlistId);
    int64_t count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        count = sqlite3_column_int64(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return count;
}

std::vector<AudioRecord> Database::searchPlaylistCoverKeys(int64_t playlistId, const SearchFilter& filter) {
    std::vector<AudioRecord> results;
    if (!m_db || playlistId <= 0) return results;

    BuiltSearchQuery bq = buildSearchWhere(filter);
    if (bq.parseError) {
        m_lastError = bq.errorMsg;
        return results;
    }
    ApplyScopedSearchFtsFallback(m_db, bq, filter);

    const std::string coverInClause = bq.coverKeySubquery.empty() ? std::string()
        : " AND af.id IN (" + bq.coverKeySubquery + ")";
    const std::string sql =
        "WITH search_candidates AS ("
        " SELECT af.id AS id, af.folder_cover_key AS folder_cover_key, af.embedded_cover_key AS embedded_cover_key "
        + bq.fromClause + " " + bq.whereClause + coverInClause +
        ") SELECT sc.folder_cover_key, sc.embedded_cover_key"
        " FROM playlist_items pi"
        " JOIN search_candidates sc ON sc.id = pi.file_id"
        " WHERE pi.playlist_id = ?";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        m_lastError = sqlite3_errmsg(m_db);
        return results;
    }
    int bindIndex = BindSearchAndCoverQuery(stmt, bq);
    sqlite3_bind_int64(stmt, bindIndex++, playlistId);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        AudioRecord record;
        record.folderCoverKey = safeGetText(stmt, 0);
        record.embeddedCoverKey = safeGetText(stmt, 1);
        results.push_back(std::move(record));
    }
    sqlite3_finalize(stmt);
    return results;
}

std::vector<AudioRecord> Database::getAllPlaylistFilesPage(int limit, int offset) {
    std::vector<AudioRecord> results;
    if (!m_db || limit <= 0) return results;
    if (offset < 0) offset = 0;

    std::string sql = std::string(
        "WITH ranked AS ("
        " SELECT pi.file_id AS file_id"
        " FROM playlist_items pi"
        " GROUP BY pi.file_id"
        ") SELECT ") + kRecordColumnsAf +
        " FROM ranked r"
        " JOIN audio_files af ON af.id = r.file_id"
        " ORDER BY af.file_name COLLATE AURA_NATURAL"
        " LIMIT ? OFFSET ?";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        return results;
    }
    sqlite3_bind_int(stmt, 1, limit);
    sqlite3_bind_int(stmt, 2, offset);

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        results.push_back(recordFromStatement(stmt));
    }

    sqlite3_finalize(stmt);
    return results;
}

std::vector<AudioRecord> Database::searchAllPlaylistFilesPage(const SearchFilter& filter, int limit, int offset) {
    std::vector<AudioRecord> results;
    if (!m_db || limit <= 0) return results;
    if (offset < 0) offset = 0;

    BuiltSearchQuery bq = buildSearchWhere(filter);
    if (bq.parseError) {
        m_lastError = bq.errorMsg;
        return results;
    }
    ApplyScopedSearchFtsFallback(m_db, bq, filter);

    const std::string coverInClause = bq.coverKeySubquery.empty() ? std::string()
        : " AND af.id IN (" + bq.coverKeySubquery + ")";
    const std::string sql =
        "WITH search_candidates AS ("
        " SELECT af.id AS id " + bq.fromClause + " " + bq.whereClause + coverInClause +
        "), ranked AS ("
        " SELECT pi.file_id AS file_id"
        " FROM playlist_items pi"
        " JOIN search_candidates sc ON sc.id = pi.file_id"
        " GROUP BY pi.file_id"
        ") SELECT " + std::string(kRecordColumnsAf) +
        " FROM ranked r"
        " JOIN audio_files af ON af.id = r.file_id"
        " ORDER BY af.file_name COLLATE AURA_NATURAL"
        " LIMIT ? OFFSET ?";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        m_lastError = sqlite3_errmsg(m_db);
        return results;
    }
    int bindIndex = BindSearchAndCoverQuery(stmt, bq);
    sqlite3_bind_int(stmt, bindIndex++, limit);
    sqlite3_bind_int(stmt, bindIndex++, offset);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        results.push_back(recordFromStatement(stmt));
    }
    sqlite3_finalize(stmt);
    return results;
}

int64_t Database::getAllPlaylistFileCount() {
    if (!m_db) return 0;

    const char* sql = "SELECT COUNT(DISTINCT file_id) FROM playlist_items";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) return 0;

    int64_t count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        count = sqlite3_column_int64(stmt, 0);
    }

    sqlite3_finalize(stmt);
    return count;
}

int64_t Database::searchAllPlaylistFileCount(const SearchFilter& filter) {
    if (!m_db) return 0;

    BuiltSearchQuery bq = buildSearchWhere(filter);
    if (bq.parseError) {
        m_lastError = bq.errorMsg;
        return 0;
    }
    ApplyScopedSearchFtsFallback(m_db, bq, filter);

    const std::string coverInClause = bq.coverKeySubquery.empty() ? std::string()
        : " AND af.id IN (" + bq.coverKeySubquery + ")";
    const std::string sql =
        "WITH search_candidates AS ("
        " SELECT af.id AS id " + bq.fromClause + " " + bq.whereClause + coverInClause +
        ") SELECT COUNT(DISTINCT pi.file_id)"
        " FROM playlist_items pi"
        " JOIN search_candidates sc ON sc.id = pi.file_id";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        m_lastError = sqlite3_errmsg(m_db);
        return 0;
    }
    BindSearchAndCoverQuery(stmt, bq);
    int64_t count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        count = sqlite3_column_int64(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return count;
}

std::vector<AudioRecord> Database::getAggregatedPlaylistFiles(int64_t parentFolderId) {
    std::vector<AudioRecord> result;
    if (!m_db) return result;
    if (!isFolder(parentFolderId)) return result;

    std::string sql = std::string("SELECT DISTINCT ") + kRecordColumnsAf +
        " FROM audio_files af"
        " JOIN playlist_items pi ON af.id = pi.file_id"
        " WHERE pi.playlist_id IN (SELECT id FROM playlists WHERE parent_id = ?)"
        " ORDER BY af.file_name COLLATE AURA_NATURAL";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) return result;
    sqlite3_bind_int64(stmt, 1, parentFolderId);

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        result.push_back(recordFromStatement(stmt));
    }

    sqlite3_finalize(stmt);
    return result;
}

std::vector<AudioRecord> Database::searchAggregatedPlaylistFilesPage(int64_t parentFolderId,
                                                                     const SearchFilter& filter,
                                                                     int limit,
                                                                     int offset) {
    std::vector<AudioRecord> result;
    if (!m_db || parentFolderId <= 0 || limit <= 0) return result;
    if (!isFolder(parentFolderId)) return result;
    if (offset < 0) offset = 0;

    BuiltSearchQuery bq = buildSearchWhere(filter);
    if (bq.parseError) {
        m_lastError = bq.errorMsg;
        return result;
    }
    ApplyScopedSearchFtsFallback(m_db, bq, filter);

    const std::string coverInClause = bq.coverKeySubquery.empty() ? std::string()
        : " AND af.id IN (" + bq.coverKeySubquery + ")";
    const std::string sql =
        "WITH search_candidates AS ("
        " SELECT af.id AS id " + bq.fromClause + " " + bq.whereClause + coverInClause +
        "), ranked AS ("
        " SELECT pi.file_id AS file_id"
        " FROM playlist_items pi"
        " JOIN playlists p ON p.id = pi.playlist_id"
        " JOIN search_candidates sc ON sc.id = pi.file_id"
        " WHERE p.parent_id = ?"
        " GROUP BY pi.file_id"
        ") SELECT " + std::string(kRecordColumnsAf) +
        " FROM ranked r"
        " JOIN audio_files af ON af.id = r.file_id"
        " ORDER BY af.file_name COLLATE AURA_NATURAL"
        " LIMIT ? OFFSET ?";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        m_lastError = sqlite3_errmsg(m_db);
        return result;
    }
    int bindIndex = BindSearchAndCoverQuery(stmt, bq);
    sqlite3_bind_int64(stmt, bindIndex++, parentFolderId);
    sqlite3_bind_int(stmt, bindIndex++, limit);
    sqlite3_bind_int(stmt, bindIndex++, offset);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        result.push_back(recordFromStatement(stmt));
    }
    sqlite3_finalize(stmt);
    return result;
}

int64_t Database::searchAggregatedPlaylistFileCount(int64_t parentFolderId, const SearchFilter& filter) {
    if (!m_db || parentFolderId <= 0) return 0;
    if (!isFolder(parentFolderId)) return 0;

    BuiltSearchQuery bq = buildSearchWhere(filter);
    if (bq.parseError) {
        m_lastError = bq.errorMsg;
        return 0;
    }
    ApplyScopedSearchFtsFallback(m_db, bq, filter);

    const std::string coverInClause = bq.coverKeySubquery.empty() ? std::string()
        : " AND af.id IN (" + bq.coverKeySubquery + ")";
    const std::string sql =
        "WITH search_candidates AS ("
        " SELECT af.id AS id " + bq.fromClause + " " + bq.whereClause + coverInClause +
        ") SELECT COUNT(DISTINCT pi.file_id)"
        " FROM playlist_items pi"
        " JOIN playlists p ON p.id = pi.playlist_id"
        " JOIN search_candidates sc ON sc.id = pi.file_id"
        " WHERE p.parent_id = ?";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        m_lastError = sqlite3_errmsg(m_db);
        return 0;
    }
    int bindIndex = BindSearchAndCoverQuery(stmt, bq);
    sqlite3_bind_int64(stmt, bindIndex++, parentFolderId);
    int64_t count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        count = sqlite3_column_int64(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return count;
}

std::vector<AudioRecord> Database::searchAggregatedPlaylistCoverKeys(int64_t parentFolderId,
                                                                    const SearchFilter& filter) {
    std::vector<AudioRecord> results;
    if (!m_db || parentFolderId <= 0) return results;
    if (!isFolder(parentFolderId)) return results;

    BuiltSearchQuery bq = buildSearchWhere(filter);
    if (bq.parseError) {
        m_lastError = bq.errorMsg;
        return results;
    }
    ApplyScopedSearchFtsFallback(m_db, bq, filter);

    const std::string coverInClause = bq.coverKeySubquery.empty() ? std::string()
        : " AND af.id IN (" + bq.coverKeySubquery + ")";
    const std::string sql =
        "WITH search_candidates AS ("
        " SELECT af.id AS id, af.folder_cover_key AS folder_cover_key, af.embedded_cover_key AS embedded_cover_key "
        + bq.fromClause + " " + bq.whereClause + coverInClause +
        "), ranked AS ("
        " SELECT sc.id AS id, sc.folder_cover_key AS folder_cover_key, sc.embedded_cover_key AS embedded_cover_key"
        " FROM playlist_items pi"
        " JOIN playlists p ON p.id = pi.playlist_id"
        " JOIN search_candidates sc ON sc.id = pi.file_id"
        " WHERE p.parent_id = ?"
        " GROUP BY sc.id"
        ") SELECT folder_cover_key, embedded_cover_key FROM ranked";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        m_lastError = sqlite3_errmsg(m_db);
        return results;
    }
    int bindIndex = BindSearchAndCoverQuery(stmt, bq);
    sqlite3_bind_int64(stmt, bindIndex++, parentFolderId);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        AudioRecord record;
        record.folderCoverKey = safeGetText(stmt, 0);
        record.embeddedCoverKey = safeGetText(stmt, 1);
        results.push_back(std::move(record));
    }
    sqlite3_finalize(stmt);
    return results;
}

std::vector<AudioRecord> Database::getAggregatedPlaylistFilesPage(int64_t parentFolderId, int limit, int offset) {
    if (limit <= 0) {
        return getAggregatedPlaylistFiles(parentFolderId);
    }

    std::vector<AudioRecord> result;
    if (!m_db) return result;
    if (!isFolder(parentFolderId)) return result;
    if (offset < 0) offset = 0;

    std::string sql = std::string(
        "WITH ranked AS ("
        " SELECT pi.file_id AS file_id"
        " FROM playlist_items pi"
        " JOIN playlists p ON p.id = pi.playlist_id"
        " WHERE p.parent_id = ?"
        " GROUP BY pi.file_id"
        ") SELECT ") + kRecordColumnsAf +
        " FROM ranked r"
        " JOIN audio_files af ON af.id = r.file_id"
        " ORDER BY af.file_name COLLATE AURA_NATURAL"
        " LIMIT ? OFFSET ?";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        return result;
    }
    sqlite3_bind_int64(stmt, 1, parentFolderId);
    sqlite3_bind_int(stmt, 2, limit);
    sqlite3_bind_int(stmt, 3, offset);

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        result.push_back(recordFromStatement(stmt));
    }

    sqlite3_finalize(stmt);
    return result;
}

int64_t Database::getAggregatedPlaylistFileCount(int64_t parentFolderId) {
    if (!m_db) return 0;
    if (!isFolder(parentFolderId)) return 0;

    const char* sql =
        "SELECT COUNT(DISTINCT pi.file_id)"
        " FROM playlist_items pi"
        " JOIN playlists p ON p.id = pi.playlist_id"
        " WHERE p.parent_id = ?";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) return 0;
    sqlite3_bind_int64(stmt, 1, parentFolderId);

    int64_t count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        count = sqlite3_column_int64(stmt, 0);
    }

    sqlite3_finalize(stmt);
    return count;
}

bool Database::incrementPlayCount(int64_t fileId) {
    if (!m_db) return false;
    return executeSQL("UPDATE audio_files SET play_count = play_count + 1 WHERE id = " + std::to_string(fileId));
}

bool Database::updateLastPlayed(int64_t fileId) {
    if (!m_db) return false;
    return executeSQL("UPDATE audio_files SET last_played = " + std::to_string(getCurrentTimestamp()) +
                      " WHERE id = " + std::to_string(fileId));
}

bool Database::beginTransaction() {
    return executeSQL("BEGIN TRANSACTION");
}

bool Database::commitTransaction() {
    return executeSQL("COMMIT");
}

bool Database::rollbackTransaction() {
    return executeSQL("ROLLBACK");
}

int Database::scanDirectory(const std::string& dirPath, bool recursive, ScanProgressCallback callback) {
    // Backward-compatible wrapper (old behavior: only add new files)
    ScanStats stats = scanDirectoryEx(dirPath, recursive, /*updateExisting*/false, /*removeMissing*/false, callback, nullptr, 0);
    return stats.added;
}

void Database::beginScanRollbackSession() {
    prepareScanRollbackJournal();
}

void Database::discardScanRollbackSession() {
    clearScanRollbackJournal();
}

bool Database::rollbackActiveScanSession(aura::BackfillQueue* backfillQueue) {
    return rollbackScanRollbackJournal(std::string(), backfillQueue);
}

bool Database::prepareScanRollbackJournal() {
    if (!m_db) return false;

    clearScanRollbackJournal();

    const auto createAudioSnapshotSql =
        std::string("CREATE TEMP TABLE temp.scan_cancel_audio AS SELECT ") +
        kRecordColumns +
        " FROM audio_files WHERE 0;";

    const std::vector<std::string> ddl = {
        "CREATE TEMP TABLE temp.scan_cancel_touched(file_path TEXT PRIMARY KEY);",
        createAudioSnapshotSql,
        "CREATE UNIQUE INDEX temp.scan_cancel_audio_path ON scan_cancel_audio(file_path);",
        "CREATE TEMP TABLE temp.scan_cancel_file_tags("
        "    file_path TEXT NOT NULL,"
        "    file_id   INTEGER NOT NULL,"
        "    tag_id    INTEGER NOT NULL,"
        "    PRIMARY KEY(file_path, tag_id)"
        ");",
        "CREATE TEMP TABLE temp.scan_cancel_playlist_items("
        "    file_path   TEXT NOT NULL,"
        "    file_id     INTEGER NOT NULL,"
        "    playlist_id INTEGER NOT NULL,"
        "    position    INTEGER NOT NULL DEFAULT 0,"
        "    PRIMARY KEY(file_path, playlist_id)"
        ");",
        "CREATE TEMP TABLE temp.scan_cancel_play_queue("
        "    file_path TEXT NOT NULL,"
        "    file_id   INTEGER NOT NULL,"
        "    position  INTEGER NOT NULL,"
        "    added_at  INTEGER NOT NULL DEFAULT 0,"
        "    PRIMARY KEY(file_path, position)"
        ");",
        "CREATE TEMP TABLE temp.scan_cancel_backfill_pending("
        "    file_path    TEXT NOT NULL,"
        "    file_id      INTEGER NOT NULL,"
        "    kind         INTEGER NOT NULL,"
        "    enqueued_at  INTEGER NOT NULL,"
        "    PRIMARY KEY(file_path, kind)"
        ");",
        "CREATE TEMP TABLE temp.scan_cancel_waveforms("
        "    file_path             TEXT PRIMARY KEY,"
        "    file_id               INTEGER NOT NULL,"
        "    waveform_data         BLOB,"
        "    mini_min_data         BLOB,"
        "    waveform_detail       BLOB,"
        "    waveform_min_detail   BLOB,"
        "    detail_num_channels   INTEGER DEFAULT 0,"
        "    rms_data              BLOB,"
        "    status                TEXT DEFAULT 'pending',"
        "    error_code            TEXT DEFAULT '',"
        "    retry_count           INTEGER DEFAULT 0,"
        "    updated_at            INTEGER DEFAULT 0,"
        "    cache_key             TEXT DEFAULT ''"
        ");",
    };

    for (const auto& sql : ddl) {
        if (!executeSQL(sql)) {
            clearScanRollbackJournal();
            return false;
        }
    }

    m_scanRollbackSessionActive = true;
    return true;
}

void Database::clearScanRollbackJournal() {
    if (m_db) {
        const std::vector<std::string> dropSql = {
            "DROP TABLE IF EXISTS temp.scan_cancel_current_ids;",
            "DROP TABLE IF EXISTS temp.scan_cancel_waveforms;",
            "DROP TABLE IF EXISTS temp.scan_cancel_backfill_pending;",
            "DROP TABLE IF EXISTS temp.scan_cancel_play_queue;",
            "DROP TABLE IF EXISTS temp.scan_cancel_playlist_items;",
            "DROP TABLE IF EXISTS temp.scan_cancel_file_tags;",
            "DROP TABLE IF EXISTS temp.scan_cancel_audio;",
            "DROP TABLE IF EXISTS temp.scan_cancel_touched;",
        };
        for (const auto& sql : dropSql) {
            executeSQL(sql);
        }
    }
    m_scanRollbackSessionActive = false;
}

bool Database::snapshotPathForScanRollback(const std::string& filePath) {
    if (!m_scanRollbackSessionActive || !m_db || filePath.empty()) return true;

    auto executeSingleText = [this, &filePath](const std::string& sql) -> bool {
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(m_db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
            return false;
        }
        sqlite3_bind_text(stmt, 1, filePath.c_str(), -1, SQLITE_TRANSIENT);
        const bool ok = sqlite3_step(stmt) == SQLITE_DONE;
        sqlite3_finalize(stmt);
        return ok;
    };

    sqlite3_stmt* touchStmt = nullptr;
    if (sqlite3_prepare_v2(m_db,
                           "INSERT OR IGNORE INTO temp.scan_cancel_touched(file_path) VALUES (?);",
                           -1,
                           &touchStmt,
                           nullptr) != SQLITE_OK) {
        return false;
    }
    sqlite3_bind_text(touchStmt, 1, filePath.c_str(), -1, SQLITE_TRANSIENT);
    const bool touchOk = sqlite3_step(touchStmt) == SQLITE_DONE;
    const bool isNewTouch = touchOk && sqlite3_changes(m_db) > 0;
    sqlite3_finalize(touchStmt);
    if (!touchOk || !isNewTouch) {
        return touchOk;
    }

    const std::string snapshotAudioSql =
        std::string("INSERT OR IGNORE INTO temp.scan_cancel_audio SELECT ") +
        kRecordColumns +
        " FROM audio_files WHERE file_path = ?;";
    const std::vector<std::string> snapshotSql = {
        snapshotAudioSql,
        "INSERT OR IGNORE INTO temp.scan_cancel_file_tags(file_path, file_id, tag_id) "
        "SELECT af.file_path, ft.file_id, ft.tag_id "
        "FROM main.audio_files af "
        "JOIN main.file_tags ft ON ft.file_id = af.id "
        "WHERE af.file_path = ?;",
        "INSERT OR IGNORE INTO temp.scan_cancel_playlist_items(file_path, file_id, playlist_id, position) "
        "SELECT af.file_path, pi.file_id, pi.playlist_id, pi.position "
        "FROM main.audio_files af "
        "JOIN main.playlist_items pi ON pi.file_id = af.id "
        "WHERE af.file_path = ?;",
        "INSERT OR IGNORE INTO temp.scan_cancel_play_queue(file_path, file_id, position, added_at) "
        "SELECT af.file_path, pq.file_id, pq.position, pq.added_at "
        "FROM main.audio_files af "
        "JOIN main.play_queue pq ON pq.file_id = af.id "
        "WHERE af.file_path = ?;",
        "INSERT OR IGNORE INTO temp.scan_cancel_backfill_pending(file_path, file_id, kind, enqueued_at) "
        "SELECT af.file_path, bp.file_id, bp.kind, bp.enqueued_at "
        "FROM main.audio_files af "
        "JOIN main.backfill_pending bp ON bp.file_id = af.id "
        "WHERE af.file_path = ?;",
        "INSERT OR IGNORE INTO temp.scan_cancel_waveforms("
        "    file_path, file_id, waveform_data, mini_min_data, waveform_detail, waveform_min_detail, "
        "    detail_num_channels, rms_data, status, error_code, retry_count, updated_at, cache_key"
        ") "
        "SELECT af.file_path, w.file_id, w.waveform_data, w.mini_min_data, w.waveform_detail, w.waveform_min_detail, "
        "       w.detail_num_channels, w.rms_data, w.status, w.error_code, w.retry_count, w.updated_at, w.cache_key "
        "FROM main.audio_files af "
        "JOIN waveforms_db.waveforms w ON w.file_id = af.id "
        "WHERE af.file_path = ?;",
    };

    for (const auto& sql : snapshotSql) {
        if (!executeSingleText(sql)) {
            return false;
        }
    }

    return true;
}

bool Database::rollbackScanRollbackJournal(const std::string& scopePath,
                                           aura::BackfillQueue* backfillQueue) {
    if (!m_scanRollbackSessionActive || !m_db) return true;

    stopPrecomputeWorker();

    int touchedCount = 0;
    sqlite3_stmt* countStmt = nullptr;
    if (sqlite3_prepare_v2(m_db,
                           "SELECT COUNT(*) FROM temp.scan_cancel_touched;",
                           -1,
                           &countStmt,
                           nullptr) == SQLITE_OK) {
        if (sqlite3_step(countStmt) == SQLITE_ROW) {
            touchedCount = sqlite3_column_int(countStmt, 0);
        }
        sqlite3_finalize(countStmt);
    }

    std::unique_lock<std::mutex> writerLk(m_writerSerializeMutex);
    if (!beginTransaction()) {
        clearScanRollbackJournal();
        return false;
    }

    auto rollbackFailure = [&](const char* reason) -> bool {
        if (reason && *reason) {
            DIAG_ACTION_FMT("Scan", "CancelRollbackFailed: reason=%s err=%s",
                            reason, sqlite3_errmsg(m_db));
        }
        rollbackTransaction();
        executeSQL("DROP TABLE IF EXISTS temp.scan_cancel_current_ids;");
        clearScanRollbackJournal();
        return false;
    };

    const std::vector<std::string> rollbackSql = {
        "DROP TABLE IF EXISTS temp.scan_cancel_current_ids;",
        "CREATE TEMP TABLE temp.scan_cancel_current_ids(file_id INTEGER PRIMARY KEY);",
        "INSERT OR IGNORE INTO temp.scan_cancel_current_ids(file_id) "
        "SELECT id FROM audio_files WHERE file_path IN (SELECT file_path FROM temp.scan_cancel_touched);",
        "DELETE FROM waveforms_db.waveforms "
        "WHERE file_id IN (SELECT file_id FROM temp.scan_cancel_current_ids);",
        "DELETE FROM audio_files "
        "WHERE file_path IN (SELECT file_path FROM temp.scan_cancel_touched);",
        std::string("INSERT INTO audio_files(") + kRecordColumns + ") "
            "SELECT " + kRecordColumns + " FROM temp.scan_cancel_audio;",
        "INSERT OR REPLACE INTO playlist_items(playlist_id, file_id, position) "
        "SELECT playlist_id, file_id, position FROM temp.scan_cancel_playlist_items;",
        "INSERT OR REPLACE INTO file_tags(file_id, tag_id) "
        "SELECT file_id, tag_id FROM temp.scan_cancel_file_tags;",
        "INSERT OR REPLACE INTO play_queue(position, file_id, added_at) "
        "SELECT position, file_id, added_at FROM temp.scan_cancel_play_queue;",
        "INSERT OR REPLACE INTO backfill_pending(file_id, kind, enqueued_at) "
        "SELECT file_id, kind, enqueued_at FROM temp.scan_cancel_backfill_pending;",
        "INSERT OR REPLACE INTO waveforms_db.waveforms("
        "    file_id, waveform_data, mini_min_data, waveform_detail, waveform_min_detail, "
        "    detail_num_channels, rms_data, status, error_code, retry_count, updated_at, cache_key"
        ") "
        "SELECT file_id, waveform_data, mini_min_data, waveform_detail, waveform_min_detail, "
        "       detail_num_channels, rms_data, status, error_code, retry_count, updated_at, cache_key "
        "FROM temp.scan_cancel_waveforms;",
    };

    for (const auto& sql : rollbackSql) {
        if (!executeSQL(sql)) {
            return rollbackFailure(sql.c_str());
        }
    }

    if (!commitTransaction()) {
        return rollbackFailure("commit");
    }
    executeSQL("DROP TABLE IF EXISTS temp.scan_cancel_current_ids;");
    writerLk.unlock();

    pruneOrphanCoverCaches();
    invalidateFacetQueryCache();
    if (!scopePath.empty()) {
        invalidateFolderCoverFacetIndexForPaths({scopePath});
    } else {
        invalidateFolderCoverFacetIndex();
    }
    bumpDataRevision();
    m_postProcessCoverRefreshNeeded.store(true, std::memory_order_release);
    checkpointCachePackageBestEffort("scan_cancel_rollback");
    if (backfillQueue) {
        backfillQueue->rebuildFromDatabase(this, 4096);
    }
    DIAG_ACTION_FMT("Scan", "CancelRollbackApplied: touched=%d scope=%s",
                    touchedCount, scopePath.empty() ? "<all>" : scopePath.c_str());
    clearScanRollbackJournal();
    return true;
}

ScanStats Database::scanDirectoryEx(const std::string& dirPath,
                                   bool recursive,
                                   bool updateExisting,
                                   bool removeMissing,
                                   ScanProgressCallback callback,
                                   std::atomic<bool>* cancelFlag,
                                   int workerThreads,
                                   bool completeImport,
                                   bool enqueueExtraction,
                                   aura::BackfillQueue* backfillQueue,
                                   const std::string& backfillDbPathSnapshot,
                                   int64_t backfillDbGenerationSnapshot,
                                   int64_t scanForegroundSessionId,
                                   const std::atomic<HarkenSearch::ScanStopDisposition>* stopDisposition) {
    ScanStats stats;
    fs::path dir = Utf8ToPath(dirPath);
    auto markCancelledAndReturn = [&]() -> ScanStats {
        stats.cancelled = true;
        return stats;
    };
    if (cancelFlag && cancelFlag->load()) {
        return markCancelledAndReturn();
    }
    const bool networkRoot = Platform::FileSystem::IsNetworkPath(dirPath);
    auto probeRoot = [&](const std::string& pathUtf8) {
        if (!networkRoot) {
            return Platform::FileSystem::ProbePath(pathUtf8);
        }
        auto done = std::make_shared<std::atomic<bool>>(false);
        auto status = std::make_shared<std::atomic<int>>(
            static_cast<int>(Platform::FileSystem::PathStatus::Unreachable));
        std::thread([pathUtf8, done, status]() {
            const auto probed = Platform::FileSystem::ProbePath(pathUtf8);
            status->store(static_cast<int>(probed), std::memory_order_release);
            done->store(true, std::memory_order_release);
        }).detach();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
        while (!done->load(std::memory_order_acquire)) {
            if (cancelFlag && cancelFlag->load()) {
                stats.cancelled = true;
                return Platform::FileSystem::PathStatus::Unreachable;
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                return Platform::FileSystem::PathStatus::Unreachable;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        return static_cast<Platform::FileSystem::PathStatus>(
            status->load(std::memory_order_acquire));
    };
    const auto rootStatus = probeRoot(dirPath);
    if (cancelFlag && cancelFlag->load()) {
        if (rootStatus == Platform::FileSystem::PathStatus::Unreachable && stats.unreachable == 0) {
            stats.unreachable = 1;
            stats.addError(std::string("Scan_NetworkLibraryUnreachable"));
        }
        return markCancelledAndReturn();
    }
    if (rootStatus == Platform::FileSystem::PathStatus::Unreachable) {
        stats.unreachable = 1;
        stats.addError(std::string("Scan_NetworkLibraryUnreachable"));
        if (callback) {
            callback(0, 1, std::string("Scan_NetworkLibraryUnreachable"), false);
        }
        return stats;
    }
    if (rootStatus != Platform::FileSystem::PathStatus::Present ||
        !Platform::FileSystem::IsDirectory(dirPath)) {
        fs::path longDir = MakeLongPath(dir);
        const std::string longDirUtf8 = PathToUtf8(longDir);
        const auto longStatus = probeRoot(longDirUtf8);
        if (cancelFlag && cancelFlag->load()) {
            return markCancelledAndReturn();
        }
        if (longStatus == Platform::FileSystem::PathStatus::Unreachable) {
            stats.unreachable = 1;
            stats.addError(std::string("Scan_NetworkLibraryUnreachable"));
            if (callback) {
                callback(0, 1, std::string("Scan_NetworkLibraryUnreachable"), false);
            }
            return stats;
        }
        if (longStatus != Platform::FileSystem::PathStatus::Present ||
            !Platform::FileSystem::IsDirectory(longDirUtf8)) {
            return stats;
        }
        dir = std::move(longDir);
    }

    const bool sessionOwnedByThisCall =
        !m_scanRollbackSessionActive &&
        cancelFlag != nullptr &&
        prepareScanRollbackJournal();

    struct FastScanModeGuard {
        Database* owner = nullptr;
        bool active = false;

        FastScanModeGuard(Database* db, bool shouldEnter) : owner(db) {
            active = shouldEnter && owner && owner->enterFastScanMode();
        }

        ~FastScanModeGuard() {
            if (active && owner) {
                owner->exitFastScanMode();
            }
        }
    } fastScanModeGuard(this, sessionOwnedByThisCall && m_isPrimaryConnection);

    // Define cancellation check early
    auto isCancelled = [&]() -> bool {
        return cancelFlag && cancelFlag->load();
    };

    auto pathForScanError = [](const fs::path& path) -> std::string {
        try {
            return PathToUtf8(path);
        } catch (...) {
            return "<path unavailable>";
        }
    };

    // Progressive scan: avoid "pre-count then collect" which can look frozen on huge trees.
    // We enumerate + process in chunks to keep memory bounded and UI responsive.
    // WORK_CHUNK_SIZE × max cover (512KB) = peak memory for in-flight data.
    // 500 * 512 KB is at most 256 MB of in-flight cover data.
    const size_t WORK_CHUNK_SIZE = 500;
    const int COMMIT_BATCH_SIZE = 500;          // Larger batches reduce WAL commits within the working-set cap.
    const int COMMIT_INTERVAL_MS = 80;          // Flush after 80 ms to keep cancellation responsive.

    if (workerThreads <= 0) {
        // Auto: leave some CPU for UI/render thread to avoid "freezing" feeling.
        unsigned hc = std::thread::hardware_concurrency();
        if (hc == 0) workerThreads = 4;
        else if (hc <= 2) workerThreads = 1;
        else workerThreads = (int)hc - 2;
    }
    if (workerThreads < 1) workerThreads = 1;

    struct WorkItem {
        fs::path path;
        std::string filePath;
        std::string fileName;
        std::string extension;
        std::string folderPath;
        uint64_t fileSize = 0;
        int64_t fileMtime = 0;
        bool exists = false;
        /// Existing row whose size or mtime changed; count it as updated.
        bool needsUpdate = false;
        int64_t existingFileId = 0;
        float existingDurationSeconds = 0.0f;
        bool existingCoverSuppressed = false;
        bool existingHasCoverKeys = false;
        bool scanTerminalFailure = false;
        std::string scanTerminalFailureMessage;
    };
    struct WorkResult {
        AudioRecord record;
        bool exists = false;
        bool ok = false;
        bool needsUpdate = false;
        bool scanTerminalFailure = false;
        // Keep scan bookkeeping separate from cover_thumbs storage.
        std::string coverKey;
        std::vector<uint8_t> coverData;
        std::string coverMime;
        std::vector<float> miniPeaks;
        std::vector<float> detailPeaks;
        std::vector<float> detailRms;
        std::string waveformStatus = "pending";
        std::string waveformErrorCode;
        std::string waveformCacheKey;
        std::string errorMessage;
    };

    // Stage 1: quick DB check to build work list (avoid heavy decode for unchanged files)
    std::vector<WorkItem> work;
    work.reserve(WORK_CHUNK_SIZE);
    std::vector<CoverRegistryDelta> scanCoverDeltas;
    scanCoverDeltas.reserve(WORK_CHUNK_SIZE);

    sqlite3_stmt* stmtExists = nullptr;
    const char* existsSql =
        "SELECT id, file_size, file_mtime, embedded_cover_key, folder_cover_key, cover_suppressed, duration "
        "FROM audio_files WHERE file_path = ? LIMIT 1;";
    if (sqlite3_prepare_v2(m_db, existsSql, -1, &stmtExists, nullptr) != SQLITE_OK) {
        stmtExists = nullptr;
    }

    // Pre-prepare the skip-branch waveform check statements once here so the
    // scan loop does NOT call sqlite3_prepare_v2 per file.  For a 100K-file
    // library with mostly-unchanged files that would be 200K–300K serial
    // prepare calls on the scan thread (~0.1–1 ms each = tens of seconds of
    // cumulative stutter).  RAII guard ensures finalize on any return path.
    struct SkipBranchStmts {
        sqlite3_stmt* hasMiniSidecar = nullptr;
        sqlite3_stmt* hasMiniLegacy  = nullptr;
        sqlite3_stmt* wfState        = nullptr;
        ~SkipBranchStmts() {
            sqlite3_finalize(hasMiniSidecar);
            sqlite3_finalize(hasMiniLegacy);
            sqlite3_finalize(wfState);
        }
    } skipStmts;
    sqlite3_prepare_v2(m_db, R"(
        SELECT length(w.waveform_data), COALESCE(w.mini_num_channels, 1)
        FROM main.audio_files af INDEXED BY idx_files_path
        JOIN waveforms_db.waveforms w ON af.id = w.file_id
        WHERE af.file_path = ?
        LIMIT 1
    )", -1, &skipStmts.hasMiniSidecar, nullptr);
    sqlite3_prepare_v2(m_db,
        "SELECT length(waveform_data) FROM main.audio_files INDEXED BY idx_files_path WHERE file_path = ? LIMIT 1",
        -1, &skipStmts.hasMiniLegacy, nullptr);
    sqlite3_prepare_v2(m_db, R"(
        SELECT w.status, w.error_code, w.retry_count, w.updated_at, w.cache_key
        FROM main.audio_files af INDEXED BY idx_files_path
        JOIN waveforms_db.waveforms w ON af.id = w.file_id
        WHERE af.file_path = ?
        LIMIT 1
    )", -1, &skipStmts.wfState, nullptr);

    auto hasUsableScanMiniWaveform = [&](const std::string& filePath) -> bool {
        constexpr int kScanMiniPeaks = AppState::WAVEFORM_MINI_PEAKS;
        if (skipStmts.hasMiniSidecar) {
            sqlite3_reset(skipStmts.hasMiniSidecar);
            sqlite3_bind_text(skipStmts.hasMiniSidecar, 1, filePath.c_str(), -1, SQLITE_TRANSIENT);
            if (sqlite3_step(skipStmts.hasMiniSidecar) == SQLITE_ROW) {
                const int bytes = sqlite3_column_type(skipStmts.hasMiniSidecar, 0) == SQLITE_NULL
                    ? 0
                    : sqlite3_column_int(skipStmts.hasMiniSidecar, 0);
                const int channels = std::max(1, sqlite3_column_int(skipStmts.hasMiniSidecar, 1));
                return bytes == kScanMiniPeaks * channels * (int)sizeof(float);
            }
        }
        if (skipStmts.hasMiniLegacy) {
            sqlite3_reset(skipStmts.hasMiniLegacy);
            sqlite3_bind_text(skipStmts.hasMiniLegacy, 1, filePath.c_str(), -1, SQLITE_TRANSIENT);
            if (sqlite3_step(skipStmts.hasMiniLegacy) == SQLITE_ROW &&
                sqlite3_column_type(skipStmts.hasMiniLegacy, 0) != SQLITE_NULL) {
                return sqlite3_column_int(skipStmts.hasMiniLegacy, 0) == kScanMiniPeaks * (int)sizeof(float);
            }
        }
        return false;
    };

    std::atomic<int> completed{0};

    int lastCallbackCount = 0;
    const bool requestScanWaveformBackfill =
        !completeImport &&
        enqueueExtraction &&
        backfillQueue != nullptr;
    const bool canEnqueueScanWaveformBackfill =
        requestScanWaveformBackfill &&
        !backfillDbPathSnapshot.empty() &&
        backfillDbGenerationSnapshot > 0;
    const bool queueScanCoverBackfill =
        !completeImport &&
        backfillQueue != nullptr &&
        !backfillDbPathSnapshot.empty() &&
        backfillDbGenerationSnapshot > 0;
    const bool foregroundScanMetadataInline = scanForegroundSessionId > 0;
    const bool queueScanMetadataBackfill =
        !completeImport &&
        !foregroundScanMetadataInline &&
        backfillQueue != nullptr &&
        !backfillDbPathSnapshot.empty() &&
        backfillDbGenerationSnapshot > 0;
    const bool blockExternalBackfillWrites =
        completeImport ||
        queueScanCoverBackfill ||
        requestScanWaveformBackfill ||
        queueScanMetadataBackfill;
    const bool readMetadataInline =
        completeImport ||
        foregroundScanMetadataInline ||
        !queueScanMetadataBackfill;
    const WaveformCachePolicy::Mode waveformCacheMode = getWaveformCacheMode();
    std::vector<int64_t> scanForegroundSkippedIds;
    scanForegroundSkippedIds.reserve((size_t)COMMIT_BATCH_SIZE);
    auto flushScanForegroundSkippedIds = [&]() -> bool {
        if (scanForegroundSessionId <= 0 || scanForegroundSkippedIds.empty()) {
            scanForegroundSkippedIds.clear();
            return true;
        }
        const bool ok = seedScanForegroundCompletionRows(
            scanForegroundSessionId,
            scanForegroundSkippedIds,
            false,
            false);
        scanForegroundSkippedIds.clear();
        if (!ok) {
            m_lastError = "Failed to seed scan foreground skipped completion rows";
        }
        return ok;
    };
    auto queueScanForegroundSkippedId = [&](int64_t fileId) -> bool {
        if (scanForegroundSessionId <= 0 || fileId <= 0) return true;
        scanForegroundSkippedIds.push_back(fileId);
        if ((int)scanForegroundSkippedIds.size() >= COMMIT_BATCH_SIZE) {
            return flushScanForegroundSkippedIds();
        }
        return true;
    };
    auto chooseWaveformBackfillPriority = [](uint64_t fileSize,
                                             float durationSeconds) -> aura::BackfillPriority {
        constexpr uint64_t kLargeBackfillFileBytes = 8ULL * 1024ULL * 1024ULL;
        constexpr float kLongBackfillDurationSeconds = 12.0f;
        const bool largeBySize = fileSize >= kLargeBackfillFileBytes;
        const bool largeByDuration = durationSeconds > kLongBackfillDurationSeconds;
        return (largeBySize || largeByDuration)
            ? aura::BackfillPriority::IdleBackfill
            : aura::BackfillPriority::RecentScan;
    };
    auto currentScanCompletionBackfillPriority = [&]() -> aura::BackfillPriority {
        return scanForegroundSessionId > 0
            ? aura::BackfillPriority::RecentScan
            : aura::BackfillPriority::IdleBackfill;
    };
    // Do not hold the backfill write gate for the whole scan. We only gate
    // each batch transaction so waveform workers can decode/cache between
    // scan commits, while ScanComplete still waits for the requested drain.
    std::unique_ptr<aura::BackfillScanWriteGuard> scanBackfillWriteGuard;
// Scan diagnostics (per-scan, capped to avoid log spam)
    // Counters for the existence-check funnel so a "second scan still says
    // 624 added" symptom can be root-caused without guessing.
    int scanDiagExistsTrue = 0;
    int scanDiagExistsFalse = 0;
    int scanDiagSkipUnchanged = 0;
    int scanDiagNeedsUpdate = 0;
    int scanDiagFalseLogged = 0;            // how many ExistsFalse samples already logged
    const int kScanDiagFalseLogCap = 5;     // first N mismatched paths printed in full

// Skip-branch waveform re-extract diagnostics
    // When a file is "unchanged" (size + mtime match within tolerance) we still
    // re-enqueue waveform work if any of 5 conditions fail. These counters tell
    // us WHICH condition is firing across the scan, so a user-reported
    // "everything re-parsed" symptom can be root-caused without guessing.
    int scanDiagSkipReextract     = 0;  // skipped files that still re-extracted
    int scanDiagReextractNoState  = 0;  // (1) !hasState
    int scanDiagReextractKeyDiff  = 0;  // (2) cacheKey mismatch
    int scanDiagReextractNotDone  = 0;  // (3) status != "done"
    int scanDiagReextractNoMini   = 0;  // (4) mini blob missing/wrong size
    int scanDiagReextractNoDetail = 0;  // (5) detail blob missing
    int scanDiagReextractLogged   = 0;
    const int kScanDiagReextractLogCap = 5;

// Per-phase wall-time accumulators (slice7-C: profile, then cut)
    // Workers race against each other so these are sums across all workers,
    // not wall time of the scan. To get average ms per file: total_us / files / 1000.
    // To estimate single-thread wall: total_us / 1000 (assumes near-perfect scaling).
    // To estimate per-worker contention: compare to wall scan time / workerThreads.
    std::atomic<uint64_t> phaseReadAudioInfoUs{0};
    std::atomic<uint64_t> phaseReadMetadataUs{0};
    std::atomic<uint64_t> phaseCoverWorkUs{0};   // only non-zero when completeImport=true
    std::atomic<int>      phaseFileCount{0};      // number of files whose worker work ran

    // Process a batch of WorkItems: parallel decode + single-thread DB writer with periodic commits.
    auto processWorkBatch = [&](std::vector<WorkItem>& batch) -> bool {
        if (batch.empty()) return true;

        // Stage 2: workers read/decode in parallel, writer commits to DB in one transaction
        std::mutex qMutex;
        std::condition_variable qCv;
        std::queue<WorkResult> resultQ;
        std::atomic<int> produced{0};
        std::atomic<int> consumed{0};
        int committedWritesInBatch = 0;
        int failedScanResultsInBatch = 0;
        int failedWriteResultsInBatch = 0;

        auto initializeScanRecord = [](AudioRecord& record, const WorkItem& wi) {
            record.filePath = wi.filePath;
            record.fileName = wi.fileName;
            record.extension = wi.extension;
            record.folderPath = wi.folderPath;
            record.fileSize = wi.fileSize;
            record.fileMtime = wi.fileMtime;
            record.lastIndexed = getCurrentTimestamp();
            std::string ext = record.extension;
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
            if (ext == ".wav" || ext == ".wave") record.format = "WAV";
            else if (ext == ".mp3") record.format = "MP3";
            else if (ext == ".flac") record.format = "FLAC";
            else if (ext == ".ogg") record.format = "OGG";
            else if (ext == ".aiff" || ext == ".aif") record.format = "AIFF";
            else if (ext == ".m4a") record.format = "M4A";
            else if (ext == ".wma") record.format = "WMA";
            else record.format = "Unknown";
        };

        auto makeRecord = [&](const WorkItem& wi) -> WorkResult {
            WorkResult wr;
            wr.exists = wi.exists;
            wr.needsUpdate = wi.needsUpdate;
            wr.ok = false;
            initializeScanRecord(wr.record, wi);
            if (wi.scanTerminalFailure) {
                wr.scanTerminalFailure = true;
                wr.errorMessage = wi.scanTerminalFailureMessage.empty()
                    ? ("Failed to scan file: " + wi.filePath)
                    : wi.scanTerminalFailureMessage;
                return wr;
            }

            try {
                AudioRecord record;
                initializeScanRecord(record, wi);

                // Determine format from extension
#if defined(AURA_TEST_SCAN_SLOW_WORKER_MS)
                if (record.fileName.find("stall") != std::string::npos) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(AURA_TEST_SCAN_SLOW_WORKER_MS));
                }
#endif

                // Audio properties (duration, sample rate, channels)
                const auto t0_audioInfo = std::chrono::steady_clock::now();
                readAudioFileInfo(wi.path, record.sampleRate, record.channels, record.totalFrames, record.durationSeconds);
                phaseReadAudioInfoUs.fetch_add(
                    (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now() - t0_audioInfo).count(),
                    std::memory_order_relaxed);

                // Metadata (ID3, BWF, iXML, etc.) - extended metadata fields
                const auto t0_metadata = std::chrono::steady_clock::now();
                AudioMetadata metadata;
                if (readMetadataInline && MetadataReader::readMetadata(record.filePath, metadata)) {
                    ApplyAudioMetadataToRecord(record, metadata);
                    // Complete import owns cover cache writes. Fast import
                    // leaves covers for demand load or explicit rebuild.
                    if (completeImport && metadata.albumArt.hasData()) {
                        record.hasCover = true;
                        // T2.2: 异步扫描出口的 cover_key 也过 CoverModule::CanonicalCoverKey
                        uint64_t h = (uint64_t)XXH3_64bits(metadata.albumArt.data.data(), metadata.albumArt.data.size());
                        wr.coverKey = CoverModule::CanonicalCoverKey(
                            std::string("E|") + Hex64_db(h) + "|" + std::to_string(metadata.albumArt.data.size()) + "|" + metadata.albumArt.mimeType);
                        wr.coverMime = metadata.albumArt.mimeType;
                        // Keep at most 512 KB of artwork per file during large-library scans.
                        if (metadata.albumArt.data.size() <= 512 * 1024) {
                            wr.coverData = std::move(metadata.albumArt.data);
                        }
                    }
                }
                phaseReadMetadataUs.fetch_add(
                    (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now() - t0_metadata).count(),
                    std::memory_order_relaxed);

                const auto t0_coverWork = std::chrono::steady_clock::now();
                // If metadata didn't have cover info, try reading album art directly
                if (completeImport && !record.hasCover) {
                    AlbumArt art;
                    if (MetadataReader::readAlbumArt(record.filePath, art) && art.hasData()) {
                        record.hasCover = true;
                        uint64_t h = (uint64_t)XXH3_64bits(art.data.data(), art.data.size());
                        // T2.2: fallback 路径 cover_key 同样过 CoverModule::CanonicalCoverKey
                        wr.coverKey = CoverModule::CanonicalCoverKey(
                            std::string("E|") + Hex64_db(h) + "|" + std::to_string(art.data.size()) + "|" + art.mimeType);
                        wr.coverMime = art.mimeType;
                        if (art.data.size() <= 512 * 1024) {
                            wr.coverData = std::move(art.data);
                        }
                    }
                }

                if (completeImport && !wr.coverKey.empty()) {
                    record.embeddedCoverKey = wr.coverKey;
                    record.embeddedCoverKeyMtime = getCurrentTimestamp();
                }
                // During complete import, resolve folder artwork when no embedded image is present.
                if (completeImport && record.embeddedCoverKey.empty() && !record.folderPath.empty()) {
                    std::string coverPath;
                    if (CoverModule::GetFolderCoverImagePathBackground(record.folderPath, coverPath, true, true) &&
                        !coverPath.empty()) {
                        record.folderCoverSrcPath = coverPath;
                        int64_t mtime = 0;
                        try {
                            mtime = Platform::FileSystem::LastWriteTimeUnixSeconds(coverPath);
                        } catch (...) {}
                        record.folderCoverSrcMtime = mtime;
                        // T2.2: NormalizeCoverKeyByVisualIdentity 出口已包 CanonicalCoverKey;
                        //       fallback 直接拼 "F|PATH|" + coverPath 必须经 CanonicalCoverKey
                        //       才能保证小写 + 反斜杠归一, 否则与 backfillNormalizedKey 输出不对齐.
                        record.folderCoverKey = CoverModule::NormalizeCoverKeyByVisualIdentity(coverPath);
                        if (record.folderCoverKey.empty()) {
                            record.folderCoverKey = CoverModule::CanonicalCoverKey(
                                std::string("F|PATH|") + coverPath);
                        }
                        record.hasCover = true;
                    }
                }
                phaseCoverWorkUs.fetch_add(
                    (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now() - t0_coverWork).count(),
                    std::memory_order_relaxed);
                phaseFileCount.fetch_add(1, std::memory_order_relaxed);
                wr.record = std::move(record);
                wr.waveformCacheKey = WaveformExtractor::BuildCacheKey(
                    wr.record.filePath, wr.record.fileSize, wr.record.fileMtime);

                // Scan workers never decode waveforms inline; complete import
                // records cache intent here, then queued workers build caches.
                wr.waveformStatus = "pending";
                wr.waveformErrorCode.clear();
                wr.ok = true;
            } catch (const std::exception& e) {
// Tolerate broken files/metadata parser failures and continue scan
                wr.ok = false;
                wr.scanTerminalFailure = true;
                wr.errorMessage = "Failed to read audio metadata: " + wi.filePath + " (" + e.what() + ")";
            } catch (...) {

                wr.ok = false;
                wr.scanTerminalFailure = true;
                wr.errorMessage = "Failed to read audio metadata: " + wi.filePath + " (unknown error)";
            }
            return wr;
        };

        std::atomic<int> workIndex{0};
        std::vector<std::thread> workers;
        workers.reserve((size_t)workerThreads);

        // Track the file currently processed by a worker for heartbeat reporting.
        std::mutex inFlightMutex;
        std::set<std::string> inFlightNames;

        auto workerFn = [&]() {
            for (;;) {
                if (isCancelled()) break;
                int idx = workIndex.fetch_add(1);
                if (idx >= (int)batch.size()) break;
                // 标记当前文件进入处理
                {
                    std::lock_guard<std::mutex> lk(inFlightMutex);
                    inFlightNames.insert(batch[(size_t)idx].fileName);
                }
                WorkResult wr;
                try {
                    wr = makeRecord(batch[(size_t)idx]);
                } catch (const std::exception& e) {
                    initializeScanRecord(wr.record, batch[(size_t)idx]);
                    wr.exists = batch[(size_t)idx].exists;
                    wr.ok = false;
                    wr.scanTerminalFailure = true;
                    wr.errorMessage = "Failed to scan file: " + batch[(size_t)idx].filePath + " (" + e.what() + ")";
                } catch (...) {
                    initializeScanRecord(wr.record, batch[(size_t)idx]);
                    wr.exists = batch[(size_t)idx].exists;
                    wr.ok = false;
                    wr.scanTerminalFailure = true;
                    wr.errorMessage = "Failed to scan file: " + batch[(size_t)idx].filePath + " (unknown error)";
                }
                {
                    std::lock_guard<std::mutex> lk(qMutex);
                    resultQ.push(std::move(wr));
                    produced++;          // Update produced while holding the queue lock.
                }
                // Remove the completed file from the in-flight set.
                {
                    std::lock_guard<std::mutex> lk(inFlightMutex);
                    inFlightNames.erase(batch[(size_t)idx].fileName);
                }
                qCv.notify_one();
            }
        };

        for (int t = 0; t < workerThreads; t++) {
            workers.emplace_back(workerFn);
        }

        int toConsume = (int)batch.size();
        // Abort after kMaxStallSeconds without a result so one file cannot hang the scan.
#if defined(AURA_TEST_SCAN_STALL_SECONDS)
        constexpr int kMaxStallSeconds = AURA_TEST_SCAN_STALL_SECONDS;
#else
        constexpr int kMaxStallSeconds = 60;
#endif
        auto lastConsumeTime = std::chrono::steady_clock::now();
        sqlite3_stmt* stmtGetId = nullptr;
        sqlite3_stmt* stmtUpsertWf = nullptr;
        bool fastWaveformWrite = false;

        const char* scanIdentitySql =
#ifdef _WIN32
            "SELECT id FROM audio_files INDEXED BY idx_files_path WHERE file_path = ? "
            "UNION ALL "
            "SELECT id FROM audio_files WHERE folder_path = ? AND file_name = ? COLLATE NOCASE "
            "LIMIT 1;";
#else
            "SELECT id FROM audio_files INDEXED BY idx_files_path WHERE file_path = ? LIMIT 1;";
#endif
        if (sqlite3_prepare_v2(m_db, scanIdentitySql, -1, &stmtGetId, nullptr) == SQLITE_OK) {
            if (completeImport) {
                const char* upsertWfSql = R"(
                    INSERT INTO waveforms_db.waveforms
                        (file_id, waveform_data, waveform_detail, rms_data, status, error_code, retry_count, updated_at, cache_key)
                    VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)
                    ON CONFLICT(file_id) DO UPDATE SET
                        waveform_data = excluded.waveform_data,
                        waveform_detail = excluded.waveform_detail,
                        rms_data = excluded.rms_data,
                        status = excluded.status,
                        error_code = excluded.error_code,
                        retry_count = excluded.retry_count,
                        updated_at = excluded.updated_at,
                        cache_key = excluded.cache_key
                )";
                if (sqlite3_prepare_v2(m_db, upsertWfSql, -1, &stmtUpsertWf, nullptr) == SQLITE_OK) {
                    fastWaveformWrite = true;
                }
            }
        }

        auto bindScanIdentity = [&](sqlite3_stmt* stmt, const AudioRecord& record) {
            sqlite3_reset(stmt);
            sqlite3_clear_bindings(stmt);
            sqlite3_bind_text(stmt, 1, record.filePath.c_str(), -1, SQLITE_TRANSIENT);
#ifdef _WIN32
            std::string folderPath = record.folderPath;
            if (folderPath.empty()) {
                try {
                    folderPath = PathToUtf8(Utf8ToPath(record.filePath).parent_path());
                } catch (...) {
                    folderPath.clear();
                }
            }
            folderPath = HarkenSearch::CanonicalFolderPath(folderPath);
            sqlite3_bind_text(stmt, 2, folderPath.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 3, record.fileName.c_str(), -1, SQLITE_TRANSIENT);
#endif
        };

        // Allocate the write buffer up front and retain it on failure for caller-directed cancellation.
        std::vector<WorkResult> writeBuffer;
        writeBuffer.reserve((size_t)COMMIT_BATCH_SIZE);

        auto accountWriteBufferFailure = [&](const std::string& message) {
            const int failedCount = (int)writeBuffer.size();
            if (failedCount > 0) {
                stats.addError(message, failedCount);
                stats.failedFiles += failedCount;
                failedWriteResultsInBatch += failedCount;
            }
            writeBuffer.clear();
        };

        auto looksTransientSqliteLock = [](const std::string& msg) -> bool {
            if (msg.empty()) return false;
            std::string lower;
            lower.reserve(msg.size());
            for (char c : msg) {
                lower.push_back((char)std::tolower((unsigned char)c));
            }
            return lower.find("locked") != std::string::npos
                || lower.find("busy") != std::string::npos;
        };

        auto currentBusyTimeoutMs = [&]() -> int {
            sqlite3_stmt* stmt = nullptr;
            int timeoutMs = 5000;
            if (sqlite3_prepare_v2(m_db, "PRAGMA busy_timeout;", -1, &stmt, nullptr) == SQLITE_OK) {
                if (sqlite3_step(stmt) == SQLITE_ROW) {
                    timeoutMs = sqlite3_column_int(stmt, 0);
                }
            }
            if (stmt) {
                sqlite3_finalize(stmt);
            }
            return timeoutMs;
        };

        constexpr int kScanWriteMaxLockRetries = 1200;
        const int scanWriteMaxLockRetryMs = ReadEnvIntClamped(
            "AURA_SCAN_WRITE_LOCK_RETRY_MS",
            120000,
            1000,
            120000);
        auto waitBeforeScanWriteRetry = [&](int& retryAttempt,
                                            std::chrono::steady_clock::time_point& firstFailureAt,
                                            const char* phase,
                                            const std::string& reason) -> bool {
            if (!looksTransientSqliteLock(reason) || isCancelled()) {
                return false;
            }

            const auto now = std::chrono::steady_clock::now();
            if (firstFailureAt == std::chrono::steady_clock::time_point{}) {
                firstFailureAt = now;
            }
            const int elapsedMs = (int)std::chrono::duration_cast<std::chrono::milliseconds>(
                now - firstFailureAt).count();
            if (retryAttempt >= kScanWriteMaxLockRetries ||
                elapsedMs >= scanWriteMaxLockRetryMs) {
                return false;
            }

            const int nextAttempt = retryAttempt + 1;
            const int remainingMs = (std::max)(0, scanWriteMaxLockRetryMs - elapsedMs);
            const int delayMs = (std::min)((std::min)(25 * nextAttempt, 125), remainingMs);
            if (nextAttempt == 1 ||
                nextAttempt == kScanWriteMaxLockRetries ||
                nextAttempt % 25 == 0) {
                DIAG_ACTION_FMT("Scan",
                    "RetryWriteBatch phase=%s attempt=%d/%d delayMs=%d elapsedMs=%d budgetMs=%d reason=%s",
                    phase,
                    nextAttempt,
                    kScanWriteMaxLockRetries,
                    delayMs,
                    elapsedMs,
                    scanWriteMaxLockRetryMs,
                    reason.c_str());
            }
            if (callback &&
                (nextAttempt == 1 || nextAttempt % 25 == 0)) {
                callback(completed.load(),
                         stats.total,
                         u8"等待数据库写锁释放...",
                         false);
            }
            retryAttempt = nextAttempt;

            const auto deadline =
                std::chrono::steady_clock::now() + std::chrono::milliseconds(delayMs);
            while (!isCancelled() && std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            return !isCancelled();
        };

        auto flushWriteBuffer = [&]() -> bool {
            if (writeBuffer.empty()) return true;
            const auto ingestStart = std::chrono::steady_clock::now();

            if (!stmtGetId) {
                m_lastError = "stmtGetId not prepared (SELECT id by file_path)";
                DIAG_ACTION_FMT("Scan", "Flush failed: %s", m_lastError.c_str());
                accountWriteBufferFailure("Database write failed during scan: " + m_lastError);
                return false;
            }

            // Hold the writer gate for the batch transaction so the precompute
            // worker cannot begin another transaction on the shared connection.
            // Release it before entering the throttle loop.

            std::unique_ptr<aura::BackfillScanWriteGuard> backfillWriteGuard;
            if (!scanBackfillWriteGuard &&
                blockExternalBackfillWrites) {
                backfillWriteGuard = std::make_unique<aura::BackfillScanWriteGuard>();
            }
            std::unique_lock<std::mutex> writerLk(m_writerSerializeMutex);
            int writeRetryAttempt = 0;
            auto writeRetryFirstFailureAt = std::chrono::steady_clock::time_point{};

            while (true) {

            // Scan batches are bulk writer sections. A deferred BEGIN can
            // start successfully, then fail on the first UPSERT if another
            // connection sneaks in with a read/write lock in between. Take the
            // writer lock up front so a whole decoded batch cannot be lost to
            // SQLITE_BUSY/LOCKED after the work has already been done.
            const bool beginOk = executeSQL("BEGIN IMMEDIATE TRANSACTION");
            if (!beginOk) {
                m_lastError = "Transaction begin failed: " + std::string(sqlite3_errmsg(m_db));
                writerLk.unlock();
                if (waitBeforeScanWriteRetry(writeRetryAttempt, writeRetryFirstFailureAt, "begin", m_lastError)) {
                    writerLk.lock();
                    continue;
                }
                writerLk.lock();
                if (isCancelled()) {
                    writeBuffer.clear();
                    return true;
                }
                DIAG_ACTION_FMT("Scan", "Flush failed: %s", m_lastError.c_str());
                accountWriteBufferFailure("Database write failed during scan: " + m_lastError);
                return false;
            }

            bool batchOk = true;
            int batchAdded = 0;
            int batchUpdated = 0;
            std::vector<CoverRegistryDelta> batchCoverDeltas;
            std::vector<int64_t> batchCoverBackfillIds;
            std::vector<int64_t> batchMetadataBackfillIds;
            std::vector<int64_t> batchForegroundAllIds;
            std::vector<int64_t> batchForegroundMiniIds;
            std::vector<int64_t> batchForegroundCoverIds;
            std::vector<int64_t> batchForegroundMetadataIds;
            std::vector<int64_t> batchForegroundFailedIds;
            struct QueuedWaveformBackfill {
                int64_t fileId = 0;
                aura::BackfillPriority priority = aura::BackfillPriority::IdleBackfill;
            };
            std::vector<QueuedWaveformBackfill> batchWaveformBackfillIds;
            if (queueScanCoverBackfill) {
                batchCoverBackfillIds.reserve(writeBuffer.size());
            }
            if (queueScanMetadataBackfill) {
                batchMetadataBackfillIds.reserve(writeBuffer.size());
            }
            if (canEnqueueScanWaveformBackfill) {
                batchWaveformBackfillIds.reserve(writeBuffer.size());
            }

            for (auto& wr : writeBuffer) {
                // Check cancellation inside the batch so a cancelled scan does not retain the write lock.
                if (isCancelled()) {
                    executeSQL("ROLLBACK");
                    writeBuffer.clear();
                    return true; // not an error, just cancelled
                }
                // Check row existence inside the transaction before UPSERT.
                bool actuallyExists = false;
                bindScanIdentity(stmtGetId, wr.record);
                if (sqlite3_step(stmtGetId) == SQLITE_ROW) {
                    actuallyExists = true;
                }
                sqlite3_reset(stmtGetId);

                if (m_scanRollbackSessionActive) {
                    if (!snapshotPathForScanRollback(wr.record.filePath)) {
                        batchOk = false;
                        m_lastError = "Failed to snapshot rollback state for: " + wr.record.filePath;
                        break;
                    }
                    if (!actuallyExists &&
                        wr.record.fileSize > 0 &&
                        wr.record.totalFrames > 0 &&
                        wr.record.sampleRate > 0) {
                        auto orphan = findOrphanByFingerprint(
                            wr.record.fileName,
                            static_cast<int64_t>(wr.record.fileSize),
                            static_cast<int64_t>(wr.record.totalFrames),
                            static_cast<int>(wr.record.sampleRate));
                        if (orphan.has_value() &&
                            orphan->dbPath != wr.record.filePath &&
                            !snapshotPathForScanRollback(orphan->dbPath)) {
                            batchOk = false;
                            m_lastError = "Failed to snapshot rollback relink source for: " + orphan->dbPath;
                            break;
                        }
                    }
                }

                if (!addFile(wr.record)) {
                    batchOk = false;
                    const std::string sqliteDetail = m_lastError;
                    m_lastError = "Failed to UPSERT file: " + wr.record.filePath;
                    if (!sqliteDetail.empty()) {
                        m_lastError += " (" + sqliteDetail + ")";
                    }
                    break;
                }

                if (!wr.scanTerminalFailure && !actuallyExists) {
                    batchAdded++;
                } else if (!wr.scanTerminalFailure && wr.needsUpdate) {
                    batchUpdated++;
                }

                int64_t fileId = -1;
                bindScanIdentity(stmtGetId, wr.record);
                if (sqlite3_step(stmtGetId) == SQLITE_ROW) {
                    fileId = sqlite3_column_int64(stmtGetId, 0);
                }
                sqlite3_reset(stmtGetId);

                if (fileId <= 0) {
                    batchOk = false;
                    m_lastError = "Lost file ID tracking for: " + wr.record.filePath;
                    break;
                }
                if (scanForegroundSessionId > 0) {
                    batchForegroundAllIds.push_back(fileId);
                }
                if (wr.scanTerminalFailure) {
                    if (scanForegroundSessionId > 0) {
                        batchForegroundMiniIds.push_back(fileId);
                        batchForegroundCoverIds.push_back(fileId);
                        batchForegroundFailedIds.push_back(fileId);
                    }
                    continue;
                }

                if (queueScanMetadataBackfill) {
                    if (!markBackfillPending(fileId, static_cast<int>(aura::BackfillJobKind::MetadataRefresh))) {
                        batchOk = false;
                        m_lastError = "Failed to mark metadata backfill pending for: " + wr.record.filePath +
                            " (" + std::string(sqlite3_errmsg(m_db)) + ")";
                        break;
                    }
                    batchMetadataBackfillIds.push_back(fileId);
                    if (scanForegroundSessionId > 0) {
                        batchForegroundMetadataIds.push_back(fileId);
                    }
                }

                if (!wr.record.keywords.empty()) {
                    std::vector<std::string> tags;
                    std::string kw = wr.record.keywords;
                    const char* sep = ",; \t\n\r";
                    for (size_t i = 0; i < kw.size(); ) {
                        size_t j = kw.find_first_of(sep, i);
                        if (j == i) { ++i; continue; }
                        if (j == std::string::npos) {
                            if (i < kw.size()) tags.push_back(kw.substr(i));
                            break;
                        }
                        if (j > i) tags.push_back(kw.substr(i, j - i));
                        i = j + 1;
                    }
                    if (!tags.empty()) {
                        if (!setFileTagsInTransaction(fileId, tags)) {
                            batchOk = false;
                            m_lastError = "Failed to set tags for: " + wr.record.filePath;
                            break;
                        }
                    }
                }

                if (!wr.coverKey.empty() && !wr.coverData.empty()) {
                    if (!hasCoverThumb(wr.coverKey)) {
                        if (!saveCoverThumb(wr.coverKey, wr.coverData, wr.coverMime)) {
                            batchOk = false;
                            m_lastError = "Failed to save cover thumb for: " + wr.record.filePath +
                                " (" + std::string(sqlite3_errmsg(m_db)) + ")";
                            break;
                        }
                    }
                }
                if (!wr.record.embeddedCoverKey.empty()) {
                    batchCoverDeltas.push_back({wr.record.embeddedCoverKey, wr.record.filePath, true});
                }
                if (!wr.record.folderCoverKey.empty()) {
                    const std::string rep = !wr.record.folderPath.empty() ? wr.record.folderPath : wr.record.filePath;
                    batchCoverDeltas.push_back({wr.record.folderCoverKey, rep, false});
                }
                if (queueScanCoverBackfill &&
                    !wr.record.coverSuppressed &&
                    wr.record.embeddedCoverKey.empty() &&
                    wr.record.folderCoverKey.empty()) {
                    if (!markBackfillPending(fileId, static_cast<int>(aura::BackfillJobKind::CoverResolveAndThumb))) {
                        batchOk = false;
                        m_lastError = "Failed to mark cover backfill pending for: " + wr.record.filePath +
                            " (" + std::string(sqlite3_errmsg(m_db)) + ")";
                        break;
                    }
                    batchCoverBackfillIds.push_back(fileId);
                }

                if (completeImport || requestScanWaveformBackfill) {
                    WaveformCacheState oldWfState;
                    bool hasOldWf = getWaveformCacheStateByPath(wr.record.filePath, oldWfState);
                    bool needsPending = true;
                    const auto detailPolicy =
                        WaveformCachePolicy::PersistentMainDetailForDuration(
                            waveformCacheMode,
                            wr.record.durationSeconds);
                    const bool detailRequired = completeImport && detailPolicy.persistMainDetail;
                    const bool hasRequiredDetail =
                        !detailRequired || hasWaveformDetailByPath(wr.record.filePath);
                    if (!detailRequired) {
                        ++stats.waveformLongDetailSkipped;
                    }
                    // Pre-2026-06-15 short-file skip path. With
                    // kMiniCachePersistenceMinSeconds == 0.0 this branch never
                    // fires for known durations; kept so a future tuning that
                    // re-introduces a positive threshold doesn't have to thread
                    // a new call site through scan.
                    if (!WaveformCachePolicy::ShouldPersistScanMini(wr.record.durationSeconds)) {
                        clearBackfillPending(fileId, static_cast<int>(aura::BackfillJobKind::WaveformL1Overview));
                        needsPending = false;
                    } else if (hasOldWf &&
                        AreWaveformCacheKeysEquivalentForScan(oldWfState.cacheKey, wr.waveformCacheKey) &&
                        hasUsableScanMiniWaveform(wr.record.filePath) &&
                        hasRequiredDetail) {
                        if (oldWfState.status != "done") {
                            upsertWaveformCacheStateByPath(
                                wr.record.filePath, "done", "", 0, wr.waveformCacheKey);
                            clearBackfillPending(fileId, static_cast<int>(aura::BackfillJobKind::WaveformL1Overview));
                            ++stats.waveformPendingFixedDone;
                        }
                        ++stats.waveformCacheReused;
                        needsPending = false;
                    }

                    if (needsPending) {
                    if (fastWaveformWrite && stmtUpsertWf) {
                        sqlite3_reset(stmtUpsertWf);
                        sqlite3_clear_bindings(stmtUpsertWf);
                        sqlite3_bind_int64(stmtUpsertWf, 1, fileId);
                        sqlite3_bind_null(stmtUpsertWf, 2);
                        sqlite3_bind_null(stmtUpsertWf, 3);
                        sqlite3_bind_null(stmtUpsertWf, 4);
                        sqlite3_bind_text(stmtUpsertWf, 5, "pending", -1, SQLITE_TRANSIENT);
                        sqlite3_bind_text(stmtUpsertWf, 6, "", -1, SQLITE_TRANSIENT);
                        sqlite3_bind_int(stmtUpsertWf, 7, 0);
                        sqlite3_bind_int64(stmtUpsertWf, 8, getCurrentTimestamp());
                        sqlite3_bind_text(stmtUpsertWf, 9, wr.waveformCacheKey.c_str(), -1, SQLITE_TRANSIENT);
                        if (sqlite3_step(stmtUpsertWf) != SQLITE_DONE) {
                            batchOk = false;
                            m_lastError = "Failed to write fast waveform pending state for: " +
                                wr.record.filePath + " (" + std::string(sqlite3_errmsg(m_db)) + ")";
                            break;
                        }
                    } else {
                        if (!upsertWaveformCacheStateByPath(wr.record.filePath, "pending", "", 0, wr.waveformCacheKey)) {
                            batchOk = false;
                            m_lastError = "Failed to write waveform pending state for: " + wr.record.filePath +
                                " (" + std::string(sqlite3_errmsg(m_db)) + ")";
                            break;
                        }
                    }
                    if (requestScanWaveformBackfill) {
                        if (!markBackfillPending(fileId, static_cast<int>(aura::BackfillJobKind::WaveformL1Overview))) {
                            batchOk = false;
                            m_lastError = "Failed to mark waveform backfill pending for: " + wr.record.filePath +
                                " (" + std::string(sqlite3_errmsg(m_db)) + ")";
                            break;
                        }
                        ++stats.waveformPendingCreated;
                        if (scanForegroundSessionId > 0) {
                            batchForegroundMiniIds.push_back(fileId);
                        }
                        batchWaveformBackfillIds.push_back({
                            fileId,
                            chooseWaveformBackfillPriority(
                                wr.record.fileSize,
                                wr.record.durationSeconds)
                        });
                    }
                }
                }

                if (scanForegroundSessionId > 0 &&
                    queueScanCoverBackfill &&
                    !wr.record.coverSuppressed &&
                    wr.record.embeddedCoverKey.empty() &&
                    wr.record.folderCoverKey.empty()) {
                    batchForegroundCoverIds.push_back(fileId);
                }
            }

            if (batchOk && scanForegroundSessionId > 0) {
                if (!batchForegroundAllIds.empty() &&
                    !seedScanForegroundCompletionRows(
                        scanForegroundSessionId,
                        batchForegroundAllIds,
                        false,
                        false)) {
                    batchOk = false;
                    m_lastError = "Failed to seed scan foreground completion rows";
                }
                if (batchOk &&
                    !batchForegroundMiniIds.empty() &&
                    !seedScanForegroundCompletionRows(
                        scanForegroundSessionId,
                        batchForegroundMiniIds,
                        true,
                        false)) {
                    batchOk = false;
                    m_lastError = "Failed to seed scan foreground mini completion rows";
                }
                if (batchOk &&
                    !batchForegroundCoverIds.empty() &&
                    !seedScanForegroundCompletionRows(
                        scanForegroundSessionId,
                        batchForegroundCoverIds,
                        false,
                        true)) {
                    batchOk = false;
                    m_lastError = "Failed to seed scan foreground cover completion rows";
                }
                if (batchOk &&
                    !batchForegroundMetadataIds.empty() &&
                    !seedScanForegroundCompletionRows(
                        scanForegroundSessionId,
                        batchForegroundMetadataIds,
                        false,
                        false,
                        true)) {
                    batchOk = false;
                    m_lastError = "Failed to seed scan foreground metadata completion rows";
                }
                if (batchOk && !batchForegroundFailedIds.empty()) {
                    for (int64_t failedFileId : batchForegroundFailedIds) {
                        if (!markScanForegroundMiniTerminal(
                                scanForegroundSessionId,
                                failedFileId,
                                ScanForegroundTerminalState::Failed,
                                "scan_file_failed") ||
                            !markScanForegroundCoverTerminal(
                                scanForegroundSessionId,
                                failedFileId,
                                ScanForegroundTerminalState::Failed,
                                "scan_file_failed") ||
                            !markScanForegroundMetadataTerminal(
                                scanForegroundSessionId,
                                failedFileId,
                                ScanForegroundTerminalState::Failed,
                                "scan_file_failed")) {
                            batchOk = false;
                            m_lastError = "Failed to mark scan foreground failed terminal rows";
                            break;
                        }
                    }
                }
            }

            if (batchOk && commitTransaction()) {
                committedWritesInBatch += (int)writeBuffer.size();
                stats.added += batchAdded;
                stats.updated += batchUpdated;
                stats.ingested += batchAdded + batchUpdated;
                const int64_t ingestMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - ingestStart).count();
                stats.ingestDurationMs += ((batchAdded + batchUpdated) > 0 && ingestMs == 0) ? 1 : ingestMs;
                scanCoverDeltas.insert(scanCoverDeltas.end(), batchCoverDeltas.begin(), batchCoverDeltas.end());
                if (callback && !writeBuffer.empty()) {
                    callback(completed.load(), stats.total, writeBuffer.back().record.fileName, true);
                    lastCallbackCount = completed.load();
                }
                // Transaction committed — release the writer mutex so the
                // precompute worker can save its results while we throttle.
                writerLk.unlock();
                // Dispatch waveform extraction only after commit so rolled-back work
                // is never queued. Watcher and startup reconciliation can invalidate
                // stale cache keys without scheduling bulk decode work; first use
                // extracts and caches on demand.

                backfillWriteGuard.reset();
                if (queueScanMetadataBackfill) {
                    for (int64_t fileId : batchMetadataBackfillIds) {
                        backfillQueue->enqueueFromScan(
                            fileId,
                            aura::BackfillJobKind::MetadataRefresh,
                            currentScanCompletionBackfillPriority(),
                            backfillDbPathSnapshot,
                            backfillDbGenerationSnapshot,
                            scanForegroundSessionId);
                    }
                }
                if (queueScanCoverBackfill) {
                    for (int64_t fileId : batchCoverBackfillIds) {
                        backfillQueue->enqueueFromScan(
                            fileId,
                            aura::BackfillJobKind::CoverResolveAndThumb,
                            currentScanCompletionBackfillPriority(),
                            backfillDbPathSnapshot,
                            backfillDbGenerationSnapshot,
                            scanForegroundSessionId);
                    }
                }
                if (canEnqueueScanWaveformBackfill) {
                    for (const auto& queuedJob : batchWaveformBackfillIds) {
                        backfillQueue->enqueueFromScan(
                            queuedJob.fileId,
                            aura::BackfillJobKind::WaveformL1Overview,
                            queuedJob.priority,
                            backfillDbPathSnapshot,
                            backfillDbGenerationSnapshot,
                            scanForegroundSessionId);
                    }
                }
                if (completeImport && enqueueExtraction) {
                    for (auto& wr : writeBuffer) {
                        WaveformCacheState s;
                        if (getWaveformCacheStateByPath(wr.record.filePath, s) &&
                            s.status == "pending") {
                            enqueuePrecomputeWaveform(wr.record.filePath,
                                                      wr.waveformCacheKey);
                        }
                    }
                    constexpr int kCompleteImportQueueThrottle = 4096;
                    auto lastThrottleReport = std::chrono::steady_clock::now();
                    while (!isCancelled() &&
                           precomputeQueueDepth() > kCompleteImportQueueThrottle) {
                        if (callback) {
                            const auto now = std::chrono::steady_clock::now();
                            if (now - lastThrottleReport >= std::chrono::milliseconds(500)) {
                                callback(completed.load(), stats.total,
                                         L(ScanPost_FullWaveCache), true);
                                lastThrottleReport = now;
                            }
                        }
                        std::this_thread::sleep_for(std::chrono::milliseconds(25));
                    }
                }
                // T-295: a successful scan batch added/updated rows visible
                // to other tabs — mark the dataset stale.
                if (batchAdded > 0 || batchUpdated > 0) bumpDataRevision();
                writeBuffer.clear();
                return true;
            }

            const std::string failureReason = m_lastError.empty()
                ? "Unknown database write failure"
                : m_lastError;
            rollbackTransaction();
            m_lastError = failureReason;
            writerLk.unlock();
            if (waitBeforeScanWriteRetry(writeRetryAttempt, writeRetryFirstFailureAt, "transaction", m_lastError)) {
                writerLk.lock();
                continue;
            }
            writerLk.lock();
            if (isCancelled()) {
                writeBuffer.clear();
                return true;
            }
            accountWriteBufferFailure("Database write failed during scan: " + m_lastError);
            DIAG_ACTION_FMT("Scan", "Batch rollback triggered. Reason: %s", m_lastError.c_str());
            return false;
            }
        };

        bool flushFailed = false;
        auto lastFlushTime = std::chrono::steady_clock::now();

        while (consumed.load() < toConsume) {
            if (isCancelled()) {
                break;
            }

            WorkResult wr;
            {
                std::unique_lock<std::mutex> lk(qMutex);
                bool gotResult = qCv.wait_for(lk, std::chrono::milliseconds(200),
                    [&]() { return !resultQ.empty() || produced.load() >= toConsume || isCancelled(); });
                if (!gotResult || resultQ.empty()) {
                    if (callback && consumed.load() < toConsume) {
                        std::string heartbeatMsg;
                        {
                            std::lock_guard<std::mutex> lkFlight(inFlightMutex);
                            if (!inFlightNames.empty()) {
                                heartbeatMsg = *inFlightNames.begin();
                            }
                        }
                        if (heartbeatMsg.empty()) {
                            heartbeatMsg = L(Db_HeartbeatFeatureExtract);
                        }
                        callback(completed.load(), stats.total, heartbeatMsg, false);
                    }
                    if (produced.load() >= toConsume && resultQ.empty()) break;
                    auto stallElapsed = std::chrono::steady_clock::now() - lastConsumeTime;
                    if (stallElapsed > std::chrono::seconds(kMaxStallSeconds)) {
                        if (callback) {
                            callback(completed.load(), stats.total,
                                     u8"正在处理较慢的音频文件，继续等待结果...",
                                     false);
                        }
                        lastConsumeTime = std::chrono::steady_clock::now();
                        continue;
                    }
                    continue;
                }
                lastConsumeTime = std::chrono::steady_clock::now();
                wr = std::move(resultQ.front());
                resultQ.pop();
            }

            const std::string cbFileName = wr.record.fileName;

            if (wr.ok || wr.scanTerminalFailure) {
                if (wr.scanTerminalFailure) {
                    std::string msg = wr.errorMessage.empty()
                        ? ("Failed to scan file: " + wr.record.filePath)
                        : wr.errorMessage;
                    stats.addError(msg);
                    ++stats.failedFiles;
                    failedScanResultsInBatch++;
                    DIAG_ACTION_FMT("Scan", "File failed: %s", msg.c_str());
                }
                writeBuffer.push_back(std::move(wr));
            } else {
                std::string msg = wr.errorMessage.empty()
                    ? ("Failed to scan file: " + wr.record.filePath)
                    : wr.errorMessage;
                stats.addError(msg);
                ++stats.failedFiles;
                failedScanResultsInBatch++;
                DIAG_ACTION_FMT("Scan", "File failed: %s", msg.c_str());
            }

            int c = ++completed;
            consumed++;

            bool timeSliceExpired = !writeBuffer.empty() &&
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - lastFlushTime).count() >= COMMIT_INTERVAL_MS;

            if (writeBuffer.size() >= (size_t)COMMIT_BATCH_SIZE || timeSliceExpired) {
                if (!flushWriteBuffer()) {
                    flushFailed = true;
                    break;
                }
                lastFlushTime = std::chrono::steady_clock::now();
            } else {
                int remaining = toConsume - consumed.load();
                if (remaining <= 20 || c - lastCallbackCount >= 50) {
                    if (callback) callback(c, stats.total, cbFileName, false);
                    lastCallbackCount = c;
                }
            }
        }

        if (!flushFailed && !writeBuffer.empty()) {
            if (!flushWriteBuffer()) {
                flushFailed = true;
                DIAG_ACTION_FMT("Scan", "Tail batch flush failed: %s", m_lastError.c_str());
            }
        }

        if (stmtGetId) sqlite3_finalize(stmtGetId);
        if (stmtUpsertWf) sqlite3_finalize(stmtUpsertWf);

        for (auto& th : workers) {
            if (th.joinable()) th.join();
        }

        if (flushFailed) {
            const int accountedInBatch =
                committedWritesInBatch + failedScanResultsInBatch + failedWriteResultsInBatch;
            const int unaccountedInBatch = (int)batch.size() - accountedInBatch;
            if (unaccountedInBatch > 0) {
                stats.addError(
                    "Scan aborted after database write failure; queued files were not imported.",
                    unaccountedInBatch);
                stats.failedFiles += unaccountedInBatch;
            }
            DIAG_ACTION_FMT("Scan",
                "FatalDbError batch=%d committed=%d scanFail=%d writeFail=%d reason=%s",
                (int)batch.size(), committedWritesInBatch,
                failedScanResultsInBatch, failedWriteResultsInBatch,
                m_lastError.c_str());
            return false;  // escalate -> fatalScanError, stops enumeration
        }

        return true;
    };

    // Stage 1 (progressive): enumerate and process in chunks
    auto markWaveformPendingPreservingBlobs = [&](const std::string& filePath,
                                                  const std::string& cacheKey) -> bool {
        const char* sql = R"(
            INSERT INTO waveforms_db.waveforms
                (file_id, waveform_data, waveform_detail, waveform_min_detail, detail_num_channels, rms_data, status, error_code, retry_count, updated_at, cache_key)
            SELECT id, NULL, NULL, NULL, 0, NULL, 'pending', '', 0, ?, ?
            FROM main.audio_files INDEXED BY idx_files_path
            WHERE file_path = ?
            ON CONFLICT(file_id) DO UPDATE SET
                status = excluded.status,
                error_code = excluded.error_code,
                retry_count = excluded.retry_count,
                updated_at = excluded.updated_at,
                cache_key = excluded.cache_key
        )";

        std::lock_guard<std::mutex> writerLk(m_writerSerializeMutex);
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            return false;
        }
        sqlite3_bind_int64(stmt, 1, getCurrentTimestamp());
        sqlite3_bind_text(stmt, 2, cacheKey.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, filePath.c_str(), -1, SQLITE_TRANSIENT);
        const bool ok = sqlite3_step(stmt) == SQLITE_DONE && sqlite3_changes(m_db) > 0;
        sqlite3_finalize(stmt);
        return ok;
    };

    auto handleAudioPath = [&](const fs::path& rawPath) {
        fs::path path = rawPath;
        if (Platform::FileSystem::ProbePath(PathToUtf8(path)) !=
            Platform::FileSystem::PathStatus::Present) {
            path = MakeLongPath(rawPath);
        }
        WorkItem wi;
        wi.path = path;
        wi.filePath = PathToUtf8(path);
        wi.fileName = PathToUtf8(path.filename());
        wi.extension = PathToUtf8(path.extension());
        wi.folderPath = PathToUtf8(path.parent_path());
        const auto fileInfo = Platform::FileSystem::ProbeRegularFile(wi.filePath);
        wi.fileSize = fileInfo.sizeAvailable ? fileInfo.sizeBytes : 0;
        wi.fileMtime = Platform::FileSystem::LastWriteTimeUnixSeconds(wi.filePath);

        // Files below kMinAudioFileSize cannot contain valid audio and are skipped.
        if (wi.fileSize < kMinAudioFileSize) {
            wi.scanTerminalFailure = true;
            wi.scanTerminalFailureMessage = "Audio file too small to decode: " + wi.filePath;
            work.push_back(std::move(wi));
            return;
        }

        bool exists = false;
        uint64_t sizeDb = 0;
        int64_t mtimeDb = 0;
        if (stmtExists) {
            sqlite3_reset(stmtExists);
            sqlite3_clear_bindings(stmtExists);
            sqlite3_bind_text(stmtExists, 1, wi.filePath.c_str(), -1, SQLITE_TRANSIENT);
            if (sqlite3_step(stmtExists) == SQLITE_ROW) {
                exists = true;
                wi.existingFileId = sqlite3_column_int64(stmtExists, 0);
                sizeDb = (uint64_t)sqlite3_column_int64(stmtExists, 1);
                mtimeDb = sqlite3_column_int64(stmtExists, 2);
                wi.existingHasCoverKeys =
                    (sqlite3_column_type(stmtExists, 3) != SQLITE_NULL &&
                     sqlite3_column_bytes(stmtExists, 3) > 0) ||
                    (sqlite3_column_type(stmtExists, 4) != SQLITE_NULL &&
                     sqlite3_column_bytes(stmtExists, 4) > 0);
                wi.existingCoverSuppressed = sqlite3_column_int(stmtExists, 5) != 0;
                wi.existingDurationSeconds = (float)sqlite3_column_double(stmtExists, 6);
            }
        } else {
            exists = fileExists(wi.filePath);
        }
        wi.exists = exists;

        // ── Diagnostics: classify each file so the scan summary at end can
        //    explain why "second scan still added N files" if it happens.
        if (exists) {
            ++scanDiagExistsTrue;
        } else {
            ++scanDiagExistsFalse;
            // Sample the first few mismatch paths to compare against what's in
            // DB. Helps catch path normalization drift (case, long-path prefix,
            // slash direction) between scans.
            if (scanDiagFalseLogged < kScanDiagFalseLogCap) {
                ++scanDiagFalseLogged;
                DIAG_ACTION_FMT("Scan",
                    "ExistsFalse #%d path=[%s] size=%llu mtime=%lld",
                    scanDiagFalseLogged, wi.filePath.c_str(),
                    (unsigned long long)wi.fileSize, (long long)wi.fileMtime);
                // Probe DB for any rows in the same folder so the user can eyeball
                // string differences between what scanner queries vs what's stored.
                sqlite3_stmt* dbg = nullptr;
                if (sqlite3_prepare_v2(m_db,
                        "SELECT file_path FROM audio_files WHERE folder_path = ? LIMIT 2",
                        -1, &dbg, nullptr) == SQLITE_OK) {
                    sqlite3_bind_text(dbg, 1, wi.folderPath.c_str(), -1, SQLITE_TRANSIENT);
                    int seen = 0;
                    while (sqlite3_step(dbg) == SQLITE_ROW) {
                        const unsigned char* dbPath = sqlite3_column_text(dbg, 0);
                        DIAG_ACTION_FMT("Scan",
                            "  └─ DB sample[%d] folder=[%s] dbPath=[%s]",
                            seen++, wi.folderPath.c_str(),
                            dbPath ? (const char*)dbPath : "(null)");
                    }
                    if (seen == 0) {
                        DIAG_ACTION_FMT("Scan",
                            "  └─ DB has 0 rows with folder_path=[%s] (folder truly new)",
                            wi.folderPath.c_str());
                    }
                    sqlite3_finalize(dbg);
                }
            }
        }

        bool skip = false;

        if (exists && !updateExisting) {
            skip = true;
            ++scanDiagSkipUnchanged;
        } else if (exists && updateExisting) {
            const bool sameSize = (sizeDb == wi.fileSize);
            const int64_t mtimeDelta = mtimeDb - wi.fileMtime;
            const bool sameMtime = (mtimeDelta >= -1 && mtimeDelta <= 1);

            if (sameSize && sameMtime) {
                skip = true;
                ++scanDiagSkipUnchanged;
            } else {
                wi.needsUpdate = true;
                ++scanDiagNeedsUpdate;
                if (scanDiagNeedsUpdate <= kScanDiagFalseLogCap) {
                    DIAG_ACTION_FMT("Scan",
                        "NeedsUpdate #%d path=[%s] dbSize=%llu fsSize=%llu dbMtime=%lld fsMtime=%lld",
                        scanDiagNeedsUpdate, wi.filePath.c_str(),
                        (unsigned long long)sizeDb, (unsigned long long)wi.fileSize,
                        (long long)mtimeDb, (long long)wi.fileMtime);
                }
            }
        }

        if (skip) {
            if (!queueScanForegroundSkippedId(wi.existingFileId)) {
                stats.addError("Failed to seed scan foreground skipped completion row: " + wi.filePath);
                return;
            }
            if (queueScanCoverBackfill) {
                int64_t fileId = wi.existingFileId;
                if (fileId > 0 &&
                    !wi.existingCoverSuppressed &&
                    !wi.existingHasCoverKeys &&
                    (!m_scanRollbackSessionActive || snapshotPathForScanRollback(wi.filePath)) &&
                    markBackfillPending(fileId, static_cast<int>(aura::BackfillJobKind::CoverResolveAndThumb))) {
                    if (scanForegroundSessionId > 0 &&
                        !seedScanForegroundCompletionRows(
                            scanForegroundSessionId,
                            {fileId},
                            false,
                            true)) {
                        stats.addError("Failed to seed scan foreground cover completion row: " + wi.filePath);
                        return;
                    }
                    backfillQueue->enqueueFromScan(
                        fileId,
                        aura::BackfillJobKind::CoverResolveAndThumb,
                        currentScanCompletionBackfillPriority(),
                        backfillDbPathSnapshot,
                        backfillDbGenerationSnapshot,
                        scanForegroundSessionId);
                }
            }
            if ((completeImport && enqueueExtraction) || requestScanWaveformBackfill) {
                const std::string waveformCacheKey = WaveformExtractor::BuildCacheKey(
                    wi.filePath, wi.fileSize, wi.fileMtime);
                WaveformCacheState state;
                bool hasState = false;
                if (skipStmts.wfState) {
                    sqlite3_reset(skipStmts.wfState);
                    sqlite3_bind_text(skipStmts.wfState, 1, wi.filePath.c_str(), -1, SQLITE_TRANSIENT);
                    if (sqlite3_step(skipStmts.wfState) == SQLITE_ROW) {
                        state.status     = safeGetText(skipStmts.wfState, 0);
                        state.errorCode  = safeGetText(skipStmts.wfState, 1);
                        state.retryCount = sqlite3_column_int(skipStmts.wfState, 2);
                        state.updatedAt  = sqlite3_column_int64(skipStmts.wfState, 3);
                        state.cacheKey   = safeGetText(skipStmts.wfState, 4);
                        hasState = true;
                    }
                } else {
                    hasState = getWaveformCacheStateByPath(wi.filePath, state);
                }
                const auto detailPolicy =
                    WaveformCachePolicy::PersistentMainDetailForDuration(
                        waveformCacheMode,
                        wi.existingDurationSeconds);
                const bool detailRequired = completeImport && detailPolicy.persistMainDetail;
                const bool condNoState  = !hasState;
                const bool condKeyDiff  = hasState &&
                    !AreWaveformCacheKeysEquivalentForScan(state.cacheKey, waveformCacheKey);
                // Missing mini data triggers extraction whenever cache policy permits it.
                // Keeping the policy call here preserves one place for duration limits.

                const bool condNoMini   = !hasUsableScanMiniWaveform(wi.filePath) &&
                    WaveformCachePolicy::ShouldPersistScanMini(wi.existingDurationSeconds);
                const bool condNoDetail = detailRequired && !hasWaveformDetailByPath(wi.filePath);
                if (!detailRequired) {
                    ++stats.waveformLongDetailSkipped;
                }
                const bool cacheBlobsComplete = hasState && !condKeyDiff && !condNoMini && !condNoDetail;
                const bool condNotDone  = hasState && state.status != "done" && !cacheBlobsComplete;
                if (cacheBlobsComplete && state.status != "done") {
                    upsertWaveformCacheStateByPath(wi.filePath, "done", "", 0, waveformCacheKey);
                    int64_t fileId = wi.existingFileId;
                    if (fileId <= 0) {
                        fileId = getFileByPath(wi.filePath).id;
                    }
                    if (fileId > 0) {
                        clearBackfillPending(fileId, static_cast<int>(aura::BackfillJobKind::WaveformL1Overview));
                    }
                    ++stats.waveformPendingFixedDone;
                }
                const bool needsWaveformWork =
                    condNoState || condKeyDiff || condNotDone ||
                    condNoMini || condNoDetail;
                if (needsWaveformWork) {
                    if (m_scanRollbackSessionActive &&
                        !snapshotPathForScanRollback(wi.filePath)) {
                        stats.addError("Failed to snapshot rollback state for unchanged file: " + wi.filePath);
                        return;
                    }
                    ++scanDiagSkipReextract;
                    if (condNoState)  ++scanDiagReextractNoState;
                    if (condKeyDiff)  ++scanDiagReextractKeyDiff;
                    if (condNotDone)  ++scanDiagReextractNotDone;
                    if (condNoMini)   ++scanDiagReextractNoMini;
                    if (condNoDetail) ++scanDiagReextractNoDetail;
                    if (scanDiagReextractLogged < kScanDiagReextractLogCap) {
                        ++scanDiagReextractLogged;
                        DIAG_ACTION_FMT("Scan",
                            "SkipReextract #%d path=[%s] cond={noState=%d keyDiff=%d notDone=%d noMini=%d noDetail=%d} dbKey=[%s] newKey=[%s] dbStatus=[%s]",
                            scanDiagReextractLogged, wi.filePath.c_str(),
                            condNoState ? 1 : 0, condKeyDiff ? 1 : 0,
                            condNotDone ? 1 : 0, condNoMini ? 1 : 0,
                            condNoDetail ? 1 : 0,
                            hasState ? state.cacheKey.c_str() : "<no-row>",
                            waveformCacheKey.c_str(),
                            hasState ? state.status.c_str() : "<no-row>");
                    }
                    if (markWaveformPendingPreservingBlobs(wi.filePath, waveformCacheKey)) {
                        if (completeImport && enqueueExtraction) {
                            enqueuePrecomputeWaveform(wi.filePath, waveformCacheKey);
                        } else if (requestScanWaveformBackfill) {
                            int64_t fileId = wi.existingFileId;
                            if (fileId <= 0) {
                                fileId = getFileByPath(wi.filePath).id;
                            }
                            if (fileId > 0 &&
                                markBackfillPending(fileId, static_cast<int>(aura::BackfillJobKind::WaveformL1Overview))) {
                                ++stats.waveformPendingCreated;
                                if (scanForegroundSessionId > 0 &&
                                    !seedScanForegroundCompletionRows(
                                        scanForegroundSessionId,
                                        {fileId},
                                        true,
                                        false)) {
                                    stats.addError("Failed to seed scan foreground mini completion row: " + wi.filePath);
                                    return;
                                }
                                if (canEnqueueScanWaveformBackfill) {
                                    backfillQueue->enqueueFromScan(
                                        fileId,
                                        aura::BackfillJobKind::WaveformL1Overview,
                                        chooseWaveformBackfillPriority(
                                            wi.fileSize,
                                            wi.existingDurationSeconds),
                                        backfillDbPathSnapshot,
                                        backfillDbGenerationSnapshot,
                                        scanForegroundSessionId);
                                }
                            }
                        }
                    }
                } else {
                    ++stats.waveformCacheReused;
                }
            }
            stats.skipped++;
            int c = ++completed;
            if (c - lastCallbackCount >= 50) {
                if (callback) callback(c, stats.total, wi.fileName, false);
                lastCallbackCount = c;
            }
        } else {
            work.push_back(std::move(wi));
        }
    };

    // Total is discovered audio files (not a pre-count).
    stats.total = 0;

    // Traverse directories with an explicit stack instead of recursive_directory_iterator.

    // recursive_directory_iterator cannot recover after certain directory-entry errors.
    // On a large library, one bad directory could otherwise stop the entire traversal.
    // Each directory is visited independently and its children are pushed onto the stack.
    // A failing directory is skipped without affecting the rest of the scan.

    // scanOneDirectory: enumerate a single directory (non-recursively).
    // Audio files go to handleAudioPath and the work batch.
    // Subdirectories are pushed onto dirStack when recursive scanning is enabled.
    bool fatalScanError = false;

    auto scanOneDirectory = [&](const fs::path& currentDir, std::vector<fs::path>& dirStack) {
        Platform::FileSystem::DirectoryIterator iterator;
        try {
            if (!iterator.Open(PathToUtf8(currentDir), false, true)) {
                return;
            }
            while (true) {
                if (isCancelled() || fatalScanError) break;

                Platform::FileSystem::DirectoryEntry entry;
                std::string entryError;
                const auto readStatus = iterator.Next(entry, &entryError);
                if (readStatus == Platform::FileSystem::DirectoryReadStatus::End) {
                    break;
                }
               if (readStatus == Platform::FileSystem::DirectoryReadStatus::Error) {
                    const std::string currentDirUtf8 = pathForScanError(currentDir);
                    if (Platform::FileSystem::IsNetworkPath(currentDirUtf8) ||
                        Platform::FileSystem::IsNetworkPath(dirPath)) {
                        stats.unreachable++;
                        stats.addError(std::string("Scan_NetworkLibraryUnreachable"));
                        m_lastError = std::string("Scan_NetworkLibraryUnreachable");
                        (void)Platform::FileSystem::PathStatus::Unreachable;
                        break;
                    }
                    stats.addError("Cannot read directory entry in " + currentDirUtf8 + ": " + entryError);
                    m_lastError = entryError;
                    continue;
                }
                if (readStatus != Platform::FileSystem::DirectoryReadStatus::Entry) {
                    continue;
                }

                fs::path rawPath;
                try { rawPath = Utf8ToPath(entry.pathUtf8); } catch (...) {
                    stats.addError("Cannot read directory entry path under: " + pathForScanError(currentDir));
                    continue;
                }

                // Skip hidden, system, and temporary files or directories.
                if (isHiddenPath(rawPath)) {
                    continue;
                }

                const bool isDir = entry.isDirectory;
                const bool isFile = entry.isRegularFile;

                if (isDir && recursive) {
                    // 子目录压栈，后续独立遍历
                    dirStack.push_back(rawPath);
                } else if (isFile) {
                    std::string ext;
                    try { ext = PathToUtf8(rawPath.extension()); } catch (...) {}
                    if (isAudioFile(ext)) {
                        stats.total++;
                        if (callback && (stats.total % 200 == 0)) {
                            try { callback(completed.load(), stats.total, PathToUtf8(rawPath.filename()), false); } catch (...) {}
                        }
                        try {
                            handleAudioPath(rawPath);
                        } catch (const std::exception& e) {
                            WorkItem failed;
                            failed.path = rawPath;
                            try { failed.filePath = PathToUtf8(rawPath); } catch (...) { failed.filePath = pathForScanError(rawPath); }
                            try { failed.fileName = PathToUtf8(rawPath.filename()); } catch (...) { failed.fileName = failed.filePath; }
                            try { failed.extension = PathToUtf8(rawPath.extension()); } catch (...) {}
                            try { failed.folderPath = PathToUtf8(rawPath.parent_path()); } catch (...) {}
                            try {
                                const auto info = Platform::FileSystem::ProbeRegularFile(PathToUtf8(rawPath));
                                failed.fileSize = info.sizeAvailable ? info.sizeBytes : 0;
                            } catch (...) { failed.fileSize = 0; }
                            try {
                                failed.fileMtime = Platform::FileSystem::LastWriteTimeUnixSeconds(PathToUtf8(rawPath));
                            } catch (...) { failed.fileMtime = 0; }
                            failed.scanTerminalFailure = true;
                            failed.scanTerminalFailureMessage =
                                "Failed to queue audio file: " + pathForScanError(rawPath) + " (" + e.what() + ")";
                            work.push_back(std::move(failed));
                        } catch (...) {
                            WorkItem failed;
                            failed.path = rawPath;
                            try { failed.filePath = PathToUtf8(rawPath); } catch (...) { failed.filePath = pathForScanError(rawPath); }
                            try { failed.fileName = PathToUtf8(rawPath.filename()); } catch (...) { failed.fileName = failed.filePath; }
                            try { failed.extension = PathToUtf8(rawPath.extension()); } catch (...) {}
                            try { failed.folderPath = PathToUtf8(rawPath.parent_path()); } catch (...) {}
                            try {
                                const auto info = Platform::FileSystem::ProbeRegularFile(PathToUtf8(rawPath));
                                failed.fileSize = info.sizeAvailable ? info.sizeBytes : 0;
                            } catch (...) { failed.fileSize = 0; }
                            try {
                                failed.fileMtime = Platform::FileSystem::LastWriteTimeUnixSeconds(PathToUtf8(rawPath));
                            } catch (...) { failed.fileMtime = 0; }
                            failed.scanTerminalFailure = true;
                            failed.scanTerminalFailureMessage =
                                "Failed to queue audio file: " + pathForScanError(rawPath) + " (unknown error)";
                            work.push_back(std::move(failed));
                        }
                        if (work.size() >= WORK_CHUNK_SIZE) {
                            if (!processWorkBatch(work)) {
                                fatalScanError = true;
                            }
                            work.clear();
                            if (fatalScanError) break;
                        }
                    }
                }

            }
        } catch (const std::exception& e) {
            stats.addError("Failed to scan directory: " + pathForScanError(currentDir) + " (" + e.what() + ")");
            m_lastError = e.what();
        } catch (...) {
            stats.addError("Failed to scan directory: " + pathForScanError(currentDir) + " (unknown error)");
            m_lastError = "Unknown exception scanning directory.";
        }
    };

    // Begin explicit-stack traversal at the root and handle each child directory independently.
    {
        std::vector<fs::path> dirStack;
        dirStack.reserve(1024);
        dirStack.push_back(dir);

        while (!dirStack.empty() && !isCancelled() && !fatalScanError) {
            fs::path currentDir = std::move(dirStack.back());
            dirStack.pop_back();

            scanOneDirectory(currentDir, dirStack);
        }
    }

    if (!isCancelled() && !fatalScanError) {
        try {
            if (!processWorkBatch(work)) {
                fatalScanError = true;
            }
        } catch (...) {
            m_lastError = "processWorkBatch failed.";
            stats.addError(m_lastError);
        }
        work.clear();
    }

    if (stmtExists) sqlite3_finalize(stmtExists);

    if (!isCancelled() && !fatalScanError && !flushScanForegroundSkippedIds()) {
        fatalScanError = true;
        stats.addError(m_lastError.empty()
            ? std::string("Failed to flush scan foreground completion rows")
            : m_lastError);
    }

    const bool hasCommittedChanges = (stats.added > 0) || (stats.updated > 0) || (stats.removed > 0);

    // Stop immediately on cancellation before starting the global index refresh.
    if (isCancelled()) {
        HarkenSearch::ScanStopDisposition disposition =
            stopDisposition
                ? stopDisposition->load(std::memory_order_acquire)
                : HarkenSearch::ScanStopDisposition::CancelRollback;
        if (disposition == HarkenSearch::ScanStopDisposition::None) {
            disposition = HarkenSearch::ScanStopDisposition::CancelRollback;
        }
        if (HarkenSearch::IsScanPause(disposition)) {
            if (m_scanRollbackSessionActive) {
                discardScanRollbackSession();
            }
            stats.paused = true;
            stats.cancelled = false;
            DIAG_SYSTEM("Scan", "Pause detected. Kept committed batches and skipped post-processing.");
            return stats;
        }

        const bool rollbackOk =
            m_scanRollbackSessionActive
                ? rollbackScanRollbackJournal(dirPath, backfillQueue)
                : true;
        if (!rollbackOk) {
            stats.addError("Scan cancelled but rollback failed; partial scan changes may remain.");
        }
        stats.added = 0;
        stats.updated = 0;
        stats.removed = 0;
        stats.ingested = 0;
        stats.ingestDurationMs = 0;
        stats.waveformPendingCreated = 0;
        stats.waveformPendingFixedDone = 0;
        stats.cancelled = true;
        DIAG_SYSTEM("Scan", "Cancel detected. Rolled back scan session and skipped post-processing.");
        return stats;
    }
    const bool wasCancelled = false; // 能执行到这里说明未取消，后续后处理可安全使用

    // Update callback once at end to ensure UI has latest counts.
    if (callback) {
        callback(completed.load(), stats.total,
                 fatalScanError ? std::string(L(ScanFatalDbStop)) : std::string{},
                 true);
    }

    // Count files whose persisted state actually changed.
    const int actualChangesCount = stats.added + stats.updated + stats.removed;
    const int postBase = (std::max)(completed.load(), stats.total);
    int postStageCount = 1; 
    if (removeMissing) postStageCount++;
    if (!scanCoverDeltas.empty()) postStageCount++;
    if (completeImport && enqueueExtraction) postStageCount += 2;
    postStageCount++; 
    int postStageIdx = 0;

    auto reportPostStage = [&](const std::string& msg) {
        postStageIdx++;
        if (!callback) return;
        int cur = postBase + postStageIdx;
        int tot = postBase + postStageCount;
        callback(cur, tot, msg, true);
    };

    // Optional: remove missing files under this folder（先读完路径再验盘，避免长事务占锁）
    if (removeMissing && !wasCancelled && !fatalScanError) {
        reportPostStage(L(ScanPost_CleanMissing));
        std::string prefix = dirPath;
        std::string sql = "SELECT file_path FROM audio_files WHERE file_path LIKE ?;";
        sqlite3_stmt* st = nullptr;
        std::vector<std::string> pathsUnderPrefix;
        if (sqlite3_prepare_v2(m_db, sql.c_str(), -1, &st, nullptr) == SQLITE_OK) {
            std::string pat = prefix + "%";
            sqlite3_bind_text(st, 1, pat.c_str(), -1, SQLITE_TRANSIENT);
            while (sqlite3_step(st) == SQLITE_ROW) {
                if (isCancelled()) break;
                std::string fp = safeGetText(st, 0);
                if (!fp.empty()) pathsUnderPrefix.push_back(std::move(fp));
            }
            sqlite3_finalize(st);
        }
        std::vector<std::string> missingUnderPrefix;
        int checked = 0;
       for (const auto& fp : pathsUnderPrefix) {
           if (isCancelled()) break;
            const auto pathStatus = Platform::FileSystem::ProbePath(fp);
            if (pathStatus == Platform::FileSystem::PathStatus::Missing) {
                missingUnderPrefix.push_back(fp);
            } else if (pathStatus == Platform::FileSystem::PathStatus::Unreachable) {
                stats.unreachable++;
            }
            checked++;
            if (callback && (checked % 500) == 0) {
                callback(postBase + postStageIdx, postBase + postStageCount,
                         std::string(L(ScanPost_CleanMissing)) + " (" + std::to_string(checked) + ")", true);
            }
        }
        if (!missingUnderPrefix.empty() && !isCancelled()) {
            if (m_scanRollbackSessionActive) {
                bool snapshotOk = true;
                for (const auto& fp : missingUnderPrefix) {
                    if (!snapshotPathForScanRollback(fp)) {
                        snapshotOk = false;
                        m_lastError = "Failed to snapshot rollback state before removeMissing: " + fp;
                        break;
                    }
                }
                if (!snapshotOk) {
                    stats.addError(m_lastError);
                    missingUnderPrefix.clear();
                }
            }
            int prefixRemoved = 0;
            if (beginTransaction()) {
                const char* delSql = "DELETE FROM audio_files WHERE file_path = ?";
                sqlite3_stmt* delSt = nullptr;
                if (sqlite3_prepare_v2(m_db, delSql, -1, &delSt, nullptr) == SQLITE_OK) {
                    for (const auto& fp : missingUnderPrefix) {
                        sqlite3_reset(delSt);
                        sqlite3_clear_bindings(delSt);
                        sqlite3_bind_text(delSt, 1, fp.c_str(), -1, SQLITE_TRANSIENT);
                        if (sqlite3_step(delSt) == SQLITE_DONE) {
                            stats.removed++;
                            prefixRemoved++;
                        }
                    }
                    sqlite3_finalize(delSt);
                }
                commitTransaction();
            }
           if (prefixRemoved > 0) {
               pruneOrphanCoverCaches();
               invalidateFacetQueryCache();
               bumpDataRevision();  // T-295: removed rows are visible to other tabs too
           }
       }
        if (stats.unreachable > 0 && callback) {
            callback(postBase + postStageIdx, postBase + postStageCount,
                     std::string("Scan_NetworkLibraryUnreachable"), true);
        }
    }

    int uniqueCoverChanged = 0;
    if (!scanCoverDeltas.empty() && !isCancelled()) {
        reportPostStage(L(ScanPost_DedupIndex));
        uniqueCoverChanged = finalizeCoverRegistryDeltas(scanCoverDeltas);
    }

    if (completeImport && enqueueExtraction && !isCancelled() && !fatalScanError) {
        reportPostStage(L(ScanPost_FullWaveCache));
        auto lastReport = std::chrono::steady_clock::now();
        while (!isCancelled()) {
            const int depth = precomputeQueueDepth();
            if (depth <= 0) break;
            if (callback) {
                const auto now = std::chrono::steady_clock::now();
                if (now - lastReport >= std::chrono::milliseconds(500)) {
                    callback(postBase + postStageIdx, postBase + postStageCount,
                             std::string(L(ScanPost_FullWaveCache)) + " (" + std::to_string(depth) + ")", true);
                    lastReport = now;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }
    }

    if (completeImport && enqueueExtraction && !isCancelled() && !fatalScanError) {
        reportPostStage(L(ScanPost_CoverThumb));
        if (isCoverRegistryStale()) {
            const int64_t totalFiles = getFileCount();
            if (totalFiles <= kGlobalCoverWallFallbackMaxFiles) {
                syncCoverRegistry();
            } else {
                DIAG_ACTION_FMT("Scan.CoverDedup",
                    "Deferred full cover registry rebuild for large library: files=%lld threshold=%lld",
                    (long long)totalFiles, (long long)kGlobalCoverWallFallbackMaxFiles);
            }
        }
        int repairedTotal = 0;
        while (!isCancelled()) {
            const int repaired = repairMissingCoverThumbsBatch(64);
            if (repaired <= 0) break;
            repairedTotal += repaired;
            if (callback) {
                callback(postBase + postStageIdx, postBase + postStageCount,
                         std::string(L(ScanPost_CoverThumb)) + " (" + std::to_string(repairedTotal) + ")", true);
            }
        }
    }

    const bool needsHeavyPostProcess = (actualChangesCount >= 50);

    // Defer expensive post-processing to the background thread.

    // Invalidate lightweight caches synchronously before the scan dialog closes.
    if (hasCommittedChanges || uniqueCoverChanged > 0) {
        invalidateFacetQueryCache();
        std::vector<std::string> changedPaths;
        changedPaths.reserve(1);
        changedPaths.push_back(dirPath);
        invalidateFolderCoverFacetIndexForPaths(changedPaths);
    }

    // Run heavy global maintenance only when the scan changed relevant data.
    if (!fatalScanError && (!wasCancelled || hasCommittedChanges) && needsHeavyPostProcess) {
        // Mark foreground scanning complete before background maintenance begins.
        if (callback) {
            callback(postBase + postStageCount, postBase + postStageCount, u8"扫描完毕！正在后台静默优化索引...", true);
        }

        if (m_postProcessThread.joinable()) {
            m_postProcessCancel.store(true, std::memory_order_release);
            if (m_db)
                sqlite3_interrupt(m_db);
            m_postProcessThread.join();
        }
        m_postProcessCancel.store(false, std::memory_order_release);

        m_postProcessRunning.store(true, std::memory_order_release);
        m_postProcessThread = std::thread([this]() {
            if (!m_db) {
                m_postProcessRunning.store(false, std::memory_order_release);
                return;
            }

            // A. 刷新文件夹索引表 (全表 GROUP BY)
            const char* folderSql = "SELECT folder_path, COUNT(*) FROM audio_files GROUP BY folder_path";
            sqlite3_stmt* stFI = nullptr;
            if (sqlite3_prepare_v2(m_db, folderSql, -1, &stFI, nullptr) == SQLITE_OK) {
                int64_t now = (int64_t)std::time(nullptr);
                std::vector<FolderIndexEntry> entries;
                entries.reserve(4096);
                while (sqlite3_step(stFI) == SQLITE_ROW) {
                    if (m_postProcessCancel.load(std::memory_order_acquire))
                        break;
                    FolderIndexEntry e;
                    e.folderPath = safeGetText(stFI, 0);
                    e.fileCount = sqlite3_column_int(stFI, 1);
                    e.lastUpdated = now;
                    if (!e.folderPath.empty()) entries.push_back(std::move(e));
                }
                sqlite3_finalize(stFI);

                if (!m_postProcessCancel.load(std::memory_order_acquire) && m_db) {
                    clearFolderIndex();
                    saveFolderIndexBatch(entries);
                }
            }

            if (!m_postProcessCancel.load(std::memory_order_acquire) && m_db) {
                prewarmFolderCoverFacetIndex(kFolderCoverFacetMaxDepth, kFolderCoverFacetPrewarmMaxScopes);
            }

            DIAG_ACTION("Database", "Background heavy post-processing completed or cancelled.");
            // Reload the cover wall after perceptual hashes are populated so visual
            // duplicates can be clustered instead of remaining separate phash=0 entries.

            m_postProcessCoverRefreshNeeded.store(true, std::memory_order_release);
            m_postProcessRunning.store(false, std::memory_order_release);
        });

    } else {
        // Scans below 50 files can skip global reordering and close immediately.
        if (callback) {
            callback(postBase + postStageCount, postBase + postStageCount,
                     fatalScanError ? std::string(L(ScanFatalDbStop)) : std::string(L(ScanDone)),
                     true);
        }
    }

// End-of-scan diagnostic summary
    // Explains the (total, added, updated, skipped) numbers shown in the UI:
    //   existsTrue  = files whose path already had a DB row
    //   existsFalse = files whose path had no DB row → counted as added
    //   skipUnchanged = exists + size/mtime match → fast-skipped
    //   needsUpdate   = exists + size/mtime differs → decoded + UPSERTed
    // If a "second scan" reports existsFalse >> 0 for files you know are
    // already in DB, the first few "ExistsFalse" log lines above show the
    // exact path strings — compare to your DB rows to find the drift.
    // Per-phase totals (across all workers): use to pick the next perf cut.
    // avg_us = total_us / phaseFileCount; sum of avgs ≈ single-thread per-file cost.
    const int   phaseN          = phaseFileCount.load(std::memory_order_relaxed);
    const auto  audioInfoUsSum  = phaseReadAudioInfoUs.load(std::memory_order_relaxed);
    const auto  metadataUsSum   = phaseReadMetadataUs.load(std::memory_order_relaxed);
    const auto  coverWorkUsSum  = phaseCoverWorkUs.load(std::memory_order_relaxed);
    const double audioInfoAvgMs = phaseN > 0 ? (double)audioInfoUsSum / (double)phaseN / 1000.0 : 0.0;
    const double metadataAvgMs  = phaseN > 0 ? (double)metadataUsSum  / (double)phaseN / 1000.0 : 0.0;
    const double coverWorkAvgMs = phaseN > 0 ? (double)coverWorkUsSum / (double)phaseN / 1000.0 : 0.0;

    // Keep the public summary tied to the committed row counts. Some scan
    // paths can finish through tail processing or abort handling; the UI
    // should never see "added > 0" with "ingested == 0".
    stats.ingested = stats.added + stats.updated;
    stats.fatalFailure = fatalScanError;
    if (stats.ingested > 0 && stats.ingestDurationMs <= 0) {
        stats.ingestDurationMs = 1;
    }

    // ScanSummary detail[] is capped at 192 bytes by DiagLogEntry; the path
    // alone eats most of it, so we split the perf-critical phase numbers into
    // a separate ScanPhase line that always fits.
    DIAG_ACTION_FMT("Scan",
        "ScanSummary path=[%s] updateExisting=%d removeMissing=%d completeImport=%d "
        "total=%d added=%d updated=%d skipped=%d removed=%d errors=%d ingested=%d ingestMs=%lld "
        "| existsTrue=%d existsFalse=%d skipUnchanged=%d needsUpdate=%d "
        "| skipReextract=%d {noState=%d keyDiff=%d notDone=%d noMini=%d noDetail=%d} "
        "| waveformReport {pendingCreated=%d cacheReused=%d pendingFixedDone=%d longDetailSkipped=%d}",
        dirPath.c_str(),
        updateExisting ? 1 : 0, removeMissing ? 1 : 0, completeImport ? 1 : 0,
        stats.total, stats.added, stats.updated, stats.skipped, stats.removed, stats.errors,
        stats.ingested, (long long)stats.ingestDurationMs,
        scanDiagExistsTrue, scanDiagExistsFalse, scanDiagSkipUnchanged, scanDiagNeedsUpdate,
        scanDiagSkipReextract,
        scanDiagReextractNoState, scanDiagReextractKeyDiff, scanDiagReextractNotDone,
        scanDiagReextractNoMini, scanDiagReextractNoDetail,
        stats.waveformPendingCreated,
        stats.waveformCacheReused,
        stats.waveformPendingFixedDone,
        stats.waveformLongDetailSkipped);

    DIAG_ACTION_FMT("Scan",
        "ScanWaveformCacheReport: pendingCreated=%d cacheReused=%d pendingFixedDone=%d longDetailSkipped=%d",
        stats.waveformPendingCreated,
        stats.waveformCacheReused,
        stats.waveformPendingFixedDone,
        stats.waveformLongDetailSkipped);

    DIAG_ACTION_FMT("Scan",
        "ScanPhase n=%d audioInfo=%.2fms/f metadata=%.2fms/f coverWork=%.2fms/f "
        "totals_ms a=%llu m=%llu c=%llu",
        phaseN, audioInfoAvgMs, metadataAvgMs, coverWorkAvgMs,
        (unsigned long long)(audioInfoUsSum / 1000),
        (unsigned long long)(metadataUsSum  / 1000),
        (unsigned long long)(coverWorkUsSum / 1000));

    if (sessionOwnedByThisCall) {
        clearScanRollbackJournal();
    }

    return stats;
}

std::vector<AudioRecord> Database::getFilesByFolder(const std::string& folderPath, int limit, int offset) {
    std::vector<AudioRecord> results;
    if (!m_db || folderPath.empty()) return results;

    const std::string childPrefix = MakeFolderChildPrefix(folderPath);
    const std::string upper = MakeFolderPathUpperBound(childPrefix);
    // Range query returns IDs first; the delayed join fetches full rows by primary key.
    std::string sql = "SELECT " + std::string(kRecordColumnsAf) + " FROM audio_files af "
                      "JOIN ("
                      "  SELECT id FROM audio_files INDEXED BY idx_files_folder_name_natural "
                      "  WHERE folder_path = ? OR (folder_path >= ? AND folder_path < ?) "
                      "  ORDER BY file_name COLLATE AURA_NATURAL LIMIT " + std::to_string(limit) + " OFFSET " + std::to_string(offset) +
                      ") temp ON af.id = temp.id "
                      "ORDER BY af.file_name COLLATE AURA_NATURAL";

    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(m_db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        m_lastError = sqlite3_errmsg(m_db);
        return results;
    }

    // 使用范围查询绑定参数
    sqlite3_bind_text(stmt, 1, folderPath.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, childPrefix.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, upper.c_str(), -1, SQLITE_TRANSIENT);

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        results.push_back(recordFromStatement(stmt));
    }

    sqlite3_finalize(stmt);
    return results;
}

std::vector<AudioRecord> Database::getFilesByExactFolder(const std::string& folderPath, int limit, int offset) {
    std::vector<AudioRecord> results;
    if (!m_db || folderPath.empty()) return results;
    const std::string canonicalFolderPath = HarkenSearch::CanonicalFolderPath(folderPath);
    if (canonicalFolderPath.empty()) return results;

    std::string sql = "SELECT " + std::string(kRecordColumnsAf) + " FROM audio_files af "
                      "JOIN ("
                      "  SELECT id FROM audio_files INDEXED BY idx_files_folder_name_natural "
                      "  WHERE folder_path = ? "
                      "  ORDER BY file_name COLLATE AURA_NATURAL LIMIT " + std::to_string(limit) + " OFFSET " + std::to_string(offset) +
                      ") temp ON af.id = temp.id "
                      "ORDER BY af.file_name COLLATE AURA_NATURAL";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        m_lastError = sqlite3_errmsg(m_db);
        return results;
    }

    sqlite3_bind_text(stmt, 1, canonicalFolderPath.c_str(), -1, SQLITE_TRANSIENT);

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        results.push_back(recordFromStatement(stmt));
    }

    sqlite3_finalize(stmt);
    return results;
}

std::vector<AudioRecord> Database::getFilesByIds(const std::vector<int64_t>& ids) {
    std::vector<AudioRecord> results;
    if (!m_db || ids.empty()) return results;
    
    // Build SQL with IN clause
    std::string sql = std::string("SELECT ") + kRecordColumns + " FROM audio_files WHERE id IN (";
    for (size_t i = 0; i < ids.size(); ++i) {
        if (i > 0) sql += ",";
        sql += std::to_string(ids[i]);
    }
    sql += ")";
    
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(m_db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        m_lastError = sqlite3_errmsg(m_db);
        return results;
    }
    
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        results.push_back(recordFromStatement(stmt));
    }
    
    sqlite3_finalize(stmt);
    return results;
}

std::vector<AudioRecord> Database::getFilesByPaths(const std::vector<std::string>& filePaths) {
    std::vector<AudioRecord> results;
    if (!m_db || filePaths.empty()) return results;

    std::vector<std::string> filteredPaths;
    filteredPaths.reserve(filePaths.size());
    for (const auto& path : filePaths) {
        if (!path.empty()) filteredPaths.push_back(path);
    }
    if (filteredPaths.empty()) return results;

    std::string sql = std::string("SELECT ") + kRecordColumns + " FROM audio_files WHERE file_path IN (";
    for (size_t i = 0; i < filteredPaths.size(); ++i) {
        if (i > 0) sql += ",";
        sql += "?";
    }
    sql += ") ORDER BY file_name";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        m_lastError = sqlite3_errmsg(m_db);
        return results;
    }

    for (size_t i = 0; i < filteredPaths.size(); ++i) {
        sqlite3_bind_text(stmt, static_cast<int>(i + 1), filteredPaths[i].c_str(), -1, SQLITE_TRANSIENT);
    }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        results.push_back(recordFromStatement(stmt));
    }

    sqlite3_finalize(stmt);
    return results;
}

std::vector<std::string> Database::getUniqueFolders() {
    std::vector<std::string> folders;
    if (!m_db) return folders;

    // 直接使用 folder_path 列（已有索引），避免遍历全部 file_path 再做路径解析
    const char* sql = "SELECT DISTINCT folder_path FROM audio_files "
                      "WHERE folder_path IS NOT NULL AND folder_path <> '' "
                      "ORDER BY folder_path";
    sqlite3_stmt* stmt;

    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return folders;
    }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        std::string folderPath = safeGetText(stmt, 0);
        if (!folderPath.empty()) folders.push_back(std::move(folderPath));
    }

    sqlite3_finalize(stmt);
    return folders;
}

static std::vector<std::string> getDistinctNonEmptyText(sqlite3* db, const char* colName, int limit) {
    std::vector<std::string> out;
    if (!db || !colName || limit <= 0) return out;

    // colName is a compile-time constant and never contains user input.
    std::string sql = std::string("SELECT DISTINCT ") + colName +
                      " FROM audio_files WHERE " + colName +
                      " IS NOT NULL AND " + colName +
                      " <> '' ORDER BY " + colName + " COLLATE NOCASE LIMIT ?;";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        return out;
    }
    sqlite3_bind_int(stmt, 1, limit);

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        std::string v = safeGetText(stmt, 0);
        if (!v.empty()) out.push_back(std::move(v));
    }
    sqlite3_finalize(stmt);
    return out;
}

std::vector<std::string> Database::getUniqueArtists(int limit) {
    if (!m_db) return {};
    return getDistinctNonEmptyText(m_db, "artist", limit);
}

std::vector<std::string> Database::getUniqueAlbums(int limit) {
    if (!m_db) return {};
    return getDistinctNonEmptyText(m_db, "album", limit);
}

std::vector<std::string> Database::getUniqueCategories(int limit) {
    if (!m_db) return {};
    return getDistinctNonEmptyText(m_db, "category", limit);
}

// F-033 Music Mode album grid
std::vector<Database::AlbumEntry> Database::getAlbums(const std::string& artistFilter) {
    std::vector<AlbumEntry> result;
    if (!m_db) return result;

    // v3 groups by album only; subqueries also match album name alone.
    static const char* sqlAll =
        "WITH base AS ("
        "  SELECT album_artist, album, track_count, year, cover_path FROM v_albums"
        ")"
        "SELECT b.album_artist, b.album, b.track_count, b.year, b.cover_path,"
        "  (SELECT AVG(CAST(NULLIF(bpm,'') AS REAL)) FROM audio_files"
        "   WHERE album=b.album AND CAST(NULLIF(bpm,'') AS REAL)>0) as avg_bpm,"
        "  (SELECT musical_key FROM audio_files"
        "   WHERE album=b.album AND musical_key!=''"
        "   GROUP BY musical_key ORDER BY COUNT(*) DESC LIMIT 1) as top_key,"
        "  (SELECT file_path FROM audio_files"
        "   WHERE album=b.album"
        "   ORDER BY file_path LIMIT 1) as first_file_path,"
        "  (SELECT embedded_cover_key FROM audio_files"
        "   WHERE album=b.album"
        "   AND embedded_cover_key!='' AND embedded_cover_key IS NOT NULL"
        "   ORDER BY file_path LIMIT 1) as emb_cover_key"
        " FROM base b"
        " ORDER BY b.album_artist, b.year, b.album LIMIT 500";

    static const char* sqlFiltered =
        "WITH base AS ("
        "  SELECT album_artist, album, track_count, year, cover_path FROM v_albums"
        "  WHERE album_artist=?"
        ")"
        "SELECT b.album_artist, b.album, b.track_count, b.year, b.cover_path,"
        "  (SELECT AVG(CAST(NULLIF(bpm,'') AS REAL)) FROM audio_files"
        "   WHERE album=b.album AND CAST(NULLIF(bpm,'') AS REAL)>0) as avg_bpm,"
        "  (SELECT musical_key FROM audio_files"
        "   WHERE album=b.album AND musical_key!=''"
        "   GROUP BY musical_key ORDER BY COUNT(*) DESC LIMIT 1) as top_key,"
        "  (SELECT file_path FROM audio_files"
        "   WHERE album=b.album"
        "   ORDER BY file_path LIMIT 1) as first_file_path,"
        "  (SELECT embedded_cover_key FROM audio_files"
        "   WHERE album=b.album"
        "   AND embedded_cover_key!='' AND embedded_cover_key IS NOT NULL"
        "   ORDER BY file_path LIMIT 1) as emb_cover_key"
        " FROM base b"
        " ORDER BY b.year, b.album LIMIT 500";

    const char* sql = artistFilter.empty() ? sqlAll : sqlFiltered;

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) return result;
    if (!artistFilter.empty())
        sqlite3_bind_text(stmt, 1, artistFilter.c_str(), -1, SQLITE_STATIC);

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        AlbumEntry e;
        auto col = [&](int i) -> std::string {
            const char* v = (const char*)sqlite3_column_text(stmt, i);
            return v ? v : "";
        };
        e.albumArtist   = col(0);
        e.album         = col(1);
        e.trackCount    = sqlite3_column_int(stmt, 2);
        e.year          = sqlite3_column_int(stmt, 3);
        e.coverPath     = col(4);
        e.avgBpm        = (float)sqlite3_column_double(stmt, 5);
        { const char* tk = (const char*)sqlite3_column_text(stmt, 6); e.topKey = tk ? tk : ""; }
        e.firstFilePath     = col(7);
        e.embeddedCoverKey  = col(8);
        result.push_back(std::move(e));
    }
    sqlite3_finalize(stmt);
    return result;
}

std::vector<Database::AlbumEntry> Database::getAlbums(
    const std::string& searchQuery,
    const std::string& artistFilter,
    int sortMode)
{
    std::vector<AlbumEntry> result;
    if (!m_db) return result;

    const char* order = "b.album ASC";
    switch (sortMode) {
        case 1: order = "b.album DESC";        break;
        case 2: order = "b.year DESC";         break;
        case 3: order = "b.track_count DESC";  break;
        default: break;
    }

    // Base CTE selects from v_albums, optionally filtered by artistFilter
    std::string sql =
        "WITH base AS ("
        "  SELECT album_artist, album, track_count, year, cover_path FROM v_albums";

    if (!artistFilter.empty())
        sql += " WHERE album_artist=?";

    sql += ")"
           " SELECT b.album_artist, b.album, b.track_count, b.year, b.cover_path,"
           "  (SELECT AVG(CAST(NULLIF(bpm,'') AS REAL)) FROM audio_files"
           "   WHERE album=b.album AND CAST(NULLIF(bpm,'') AS REAL)>0) as avg_bpm,"
           "  (SELECT musical_key FROM audio_files"
           "   WHERE album=b.album AND musical_key!=''"
           "   GROUP BY musical_key ORDER BY COUNT(*) DESC LIMIT 1) as top_key,"
           "  (SELECT file_path FROM audio_files"
           "   WHERE album=b.album"
           "   ORDER BY file_path LIMIT 1) as first_file_path,"
           "  (SELECT embedded_cover_key FROM audio_files"
           "   WHERE album=b.album"
           "   AND embedded_cover_key!='' AND embedded_cover_key IS NOT NULL"
           "   ORDER BY file_path LIMIT 1) as emb_cover_key"
           " FROM base b";

    if (!searchQuery.empty())
        sql += " WHERE EXISTS ("
               "  SELECT 1 FROM audio_files_fts f"
               "  WHERE audio_files_fts MATCH ?"
               "  AND f.rowid IN (SELECT id FROM audio_files WHERE album=b.album)"
               " )";

    sql += " ORDER BY ";
    sql += order;
    sql += " LIMIT 1000";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) return result;

    int bindIdx = 1;
    if (!artistFilter.empty())
        sqlite3_bind_text(stmt, bindIdx++, artistFilter.c_str(), -1, SQLITE_STATIC);
    if (!searchQuery.empty())
        sqlite3_bind_text(stmt, bindIdx++, searchQuery.c_str(), -1, SQLITE_STATIC);

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        AlbumEntry e;
        auto col = [&](int i) -> std::string {
            const char* v = (const char*)sqlite3_column_text(stmt, i);
            return v ? v : "";
        };
        e.albumArtist   = col(0);
        e.album         = col(1);
        e.trackCount    = sqlite3_column_int(stmt, 2);
        e.year          = sqlite3_column_int(stmt, 3);
        e.coverPath     = col(4);
        e.avgBpm        = (float)sqlite3_column_double(stmt, 5);
        { const char* tk = (const char*)sqlite3_column_text(stmt, 6); e.topKey = tk ? tk : ""; }
        e.firstFilePath    = col(7);
        e.embeddedCoverKey = col(8);
        result.push_back(std::move(e));
    }
    sqlite3_finalize(stmt);
    return result;
}

std::vector<AudioRecord> Database::getTracksByAlbum(const std::string& albumArtist, const std::string& album) {
    std::vector<AudioRecord> result;
    if (!m_db) return result;
    // v3 matches album name alone; albumArtist is informational and may be empty.
    std::string sql = std::string("SELECT ") + kRecordColumns +
        " FROM audio_files WHERE album=?"
        " ORDER BY disc_number, track_number LIMIT 500";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) return result;
    sqlite3_bind_text(stmt, 1, album.c_str(), -1, SQLITE_STATIC);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        result.push_back(recordFromStatement(stmt));
    }
    sqlite3_finalize(stmt);
    return result;
}

// Helper: insert folder path into tree hierarchy
// T2.5: helper — split path into segments, dropping empty / "/" / "\" tokens
static std::vector<std::string> SplitPathSegments(const std::string& pathUtf8) {
    std::vector<std::string> parts;
    fs::path p = Utf8ToPath(pathUtf8);
    for (const auto& part : p) {
        std::string s = PathToUtf8(part);
        if (!s.empty() && s != "/" && s != "\\") parts.push_back(std::move(s));
    }
    return parts;
}

// T2.5: insert a folder into the tree.
// folderPath = canonical (T2.8) — used as compare key
// rawSamplePath = ANY file_path in this folder subtree (raw case from scan) —
//                 used to derive displayName per layer (UI never shows lowercase)
// If rawSamplePath is empty, displayName falls back to canonical name.
static void InsertFolderIntoTree(FolderInfo& root, const std::string& folderPath,
                                 int fileCount, const std::string& rawSamplePath = "",
                                 aura::folder_tree::PathMode pathMode = aura::folder_tree::PathMode::Absolute,
                                 const std::vector<std::string>& libraryRoots = {}) {
    const auto layers = aura::folder_tree::BuildFolderTreeLayers(
        NormalizeLibraryPathUtf8(folderPath, false), libraryRoots, pathMode);
    if (layers.empty()) return;

    // T2.5: parallel-split raw sample to extract per-layer display names.
    // file_path layout = folder_path + sep + file_name, so its first len(parts)
    // segments correspond 1:1 to canonical folder parts.
    std::vector<std::string> rawParts;
    if (!rawSamplePath.empty()) {
        rawParts = SplitPathSegments(rawSamplePath);
    }
    const size_t skipped = SplitPathSegments(NormalizeLibraryPathUtf8(folderPath, false)).size() - layers.size();

    FolderInfo* current = &root;
    for (size_t i = 0; i < layers.size(); i++) {
        const std::string& part = layers[i].name;
        const size_t rawIndex = skipped + i;
        const std::string display = (rawIndex < rawParts.size()) ? rawParts[rawIndex] : part;

        FolderInfo* found = nullptr;
        for (auto& sub : current->subfolders) {
            // Match the full folder_path identity so same-name library roots stay separate.
            if (sub.path == layers[i].path) { found = &sub; break; }
        }
        if (!found) {
            FolderInfo nf;
            nf.name = part;             // canonical key
            nf.displayName = display;   // T2.5: raw-case for UI
            nf.path = layers[i].path;   // full folder_path identity
            nf.fileCount = 0;
            current->subfolders.push_back(nf);
            found = &current->subfolders.back();
        } else if (found->displayName.empty() && !display.empty()) {
            // Keep the first non-empty display sample yielded by the query. Any
            // original-case row avoids falling back to the canonical lowercase label.

            found->displayName = display;
        }
        if (i == layers.size() - 1) found->fileCount = fileCount;
        current = found;
    }
}

static void SortFolderTree(FolderInfo& folder) {
    std::sort(folder.subfolders.begin(), folder.subfolders.end(),
        [](const FolderInfo& a, const FolderInfo& b) { return a.name < b.name; });
    for (auto& sub : folder.subfolders) SortFolderTree(sub);
}

FolderInfo Database::getFolderHierarchy(aura::folder_tree::PathMode pathMode) {
    return getFolderHierarchyWithProgress(nullptr, 0, pathMode);
}

FolderInfo Database::getFolderHierarchyWithProgress(const std::function<void(int, int)>& progress,
                                                    int progressStep,
                                                    aura::folder_tree::PathMode pathMode) {
    FolderInfo root;
    root.name = L(Db_FolderTreeRoot);
    root.path = "";
    const std::vector<std::string> libraryRoots = getLibraryRoots();

    // T2.5: single-pass SQL — pull canonical folder_path + COUNT + MIN(file_path)
    // as raw-case sample. This replaces 2 separate queries (getUniqueFolders +
    // GROUP BY count) and gives us per-layer display names in one shot.
    struct FolderRow { std::string folderPath; int count; std::string sampleFilePath; };
    std::vector<FolderRow> folderRows;
    {
        const char* sql =
            "SELECT folder_path, COUNT(*), MIN(file_path) "
            "FROM audio_files "
            "WHERE folder_path IS NOT NULL AND folder_path <> '' "
            "GROUP BY folder_path";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                FolderRow r;
                r.folderPath     = safeGetText(stmt, 0);
                r.count          = sqlite3_column_int(stmt, 1);
                r.sampleFilePath = safeGetText(stmt, 2);
                if (!r.folderPath.empty()) folderRows.push_back(std::move(r));
            }
            sqlite3_finalize(stmt);
        }
    }
    if (folderRows.empty()) return root;

    // Build hierarchical tree
    int total = (int)folderRows.size();
    int built = 0;
    if (progress && progressStep > 0) progress(0, total);
    for (const auto& row : folderRows) {
        if (!IsPathInsideLibraryRoots(row.folderPath, libraryRoots)) continue;
        InsertFolderIntoTree(root, row.folderPath, row.count, row.sampleFilePath, pathMode, libraryRoots);
        ++built;
        if (progress && progressStep > 0 && (built % progressStep == 0 || built == total)) {
            progress(built, total);
        }
    }
    SortFolderTree(root);
    return root;
}

// Build the folder tree from folder_index for fast startup.

FolderInfo Database::getFolderHierarchyFromIndex(aura::folder_tree::PathMode pathMode) {
    FolderInfo root;
    root.name = L(Db_FolderTreeRoot);
    root.path = "";
    const std::vector<std::string> libraryRoots = getLibraryRoots();

    auto scalarInt64 = [this](const char* sql, int64_t& out) -> bool {
        out = 0;
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            return false;
        }
        const int rc = sqlite3_step(stmt);
        if (rc == SQLITE_ROW) {
            out = sqlite3_column_int64(stmt, 0);
            sqlite3_finalize(stmt);
            return true;
        }
        sqlite3_finalize(stmt);
        return rc == SQLITE_DONE;
    };

    int64_t liveFileCount = 0;
    int64_t liveFolderCount = 0;
    int64_t indexedFileCount = 0;
    int64_t indexedFolderCount = 0;
    const bool haveIndexHealth =
        scalarInt64("SELECT COUNT(*) FROM audio_files "
                    "WHERE folder_path IS NOT NULL AND folder_path <> ''",
                    liveFileCount) &&
        scalarInt64("SELECT COUNT(*) FROM ("
                    "  SELECT folder_path FROM audio_files "
                    "  WHERE folder_path IS NOT NULL AND folder_path <> '' "
                    "  GROUP BY folder_path"
                    ")",
                    liveFolderCount) &&
        scalarInt64("SELECT COALESCE(SUM(file_count), 0) FROM folder_index",
                    indexedFileCount) &&
        scalarInt64("SELECT COUNT(*) FROM folder_index", indexedFolderCount);
    if (haveIndexHealth &&
        (liveFileCount != indexedFileCount || liveFolderCount != indexedFolderCount)) {
        DIAG_ACTION_FMT("Database",
                        "FolderIndexStale liveFiles=%lld indexFiles=%lld liveFolders=%lld indexFolders=%lld",
                        static_cast<long long>(liveFileCount),
                        static_cast<long long>(indexedFileCount),
                        static_cast<long long>(liveFolderCount),
                        static_cast<long long>(indexedFolderCount));
        if (sqlite3_db_readonly(m_db, "main") == 1) {
            return root;
        }
        if (!rebuildFolderIndex()) {
            DIAG_ACTION("Database", "FolderIndexStale rebuild failed");
        }
    }

    auto entries = getFolderIndex();
    if (entries.empty()) return root;

    // folder_index has no file_path column, so map each canonical folder
    // to one raw-case sample before rendering the fast-startup tree.
    // This preserves display casing until the full refresh completes.


    std::unordered_map<std::string, std::string> sampleByCanon;
    {
        const char* sql =
            "SELECT folder_path, MIN(file_path) FROM audio_files "
            "WHERE folder_path IS NOT NULL AND folder_path <> '' "
            "GROUP BY folder_path";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                std::string canon  = safeGetText(stmt, 0);
                std::string sample = safeGetText(stmt, 1);
                if (!canon.empty()) sampleByCanon.emplace(std::move(canon), std::move(sample));
            }
            sqlite3_finalize(stmt);
        }
    }

    for (const auto& e : entries) {
        if (!IsPathInsideLibraryRoots(e.folderPath, libraryRoots)) continue;
        const auto sampleIt = sampleByCanon.find(e.folderPath);
        const std::string& sample = (sampleIt != sampleByCanon.end()) ? sampleIt->second : std::string();
        InsertFolderIntoTree(root, e.folderPath, e.fileCount, sample, pathMode, libraryRoots);
    }
    SortFolderTree(root);
    return root;
}

// Persisted artwork thumbnails

bool Database::saveCoverThumb(const std::string& coverKey, const std::vector<uint8_t>& imageData, const std::string& mime) {
    if (!m_db || coverKey.empty() || imageData.empty()) return false;
// Use blobs_db if attached, otherwise fall back to main database.
    const char* sql = "INSERT OR REPLACE INTO blobs_db.cover_thumbs (cover_key, thumb_data, mime, created_at) VALUES (?, ?, ?, ?)";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(stmt, 1, coverKey.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_blob(stmt, 2, imageData.data(), (int)imageData.size(), SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, mime.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 4, (int64_t)std::time(nullptr));
    bool ok = (sqlite3_step(stmt) == SQLITE_DONE);
    sqlite3_finalize(stmt);

    // Compute the perceptual hash and persist it in cover_cache_registry.
    if (ok && imageData.size() > 16) {
        uint64_t phash = CoverModule::ComputePerceptualHash(imageData.data(), imageData.size());
        if (phash != 0) {
            const char* upSql = "UPDATE cover_cache_registry SET cover_phash = ?, cover_phash_version = 2 WHERE cover_key = ?";
            sqlite3_stmt* upStmt = nullptr;
            if (sqlite3_prepare_v2(m_db, upSql, -1, &upStmt, nullptr) == SQLITE_OK) {
                sqlite3_bind_int64(upStmt, 1, (int64_t)phash);
                sqlite3_bind_text(upStmt, 2, coverKey.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_step(upStmt);
                sqlite3_finalize(upStmt);
            }
        }
    }

    return ok;
}

bool Database::getCoverThumb(const std::string& coverKey, std::vector<uint8_t>& outImageData, std::string& outMime) {
    outImageData.clear();
    outMime.clear();
    if (!m_db || coverKey.empty()) return false;

    const char* sql = "SELECT thumb_data, mime FROM blobs_db.cover_thumbs WHERE cover_key = ?";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(stmt, 1, coverKey.c_str(), -1, SQLITE_TRANSIENT);
    bool ok = false;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const void* blob = sqlite3_column_blob(stmt, 0);
        int blobSize = sqlite3_column_bytes(stmt, 0);
        if (blob && blobSize > 0) {
            outImageData.assign((const uint8_t*)blob, (const uint8_t*)blob + blobSize);
            outMime = safeGetText(stmt, 1);
            ok = true;
        }
    }
    sqlite3_finalize(stmt);
    return ok;
}

bool Database::hasCoverThumb(const std::string& coverKey) {
    if (!m_db || coverKey.empty()) return false;

    const char* sql = "SELECT 1 FROM blobs_db.cover_thumbs WHERE cover_key = ? LIMIT 1";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(stmt, 1, coverKey.c_str(), -1, SQLITE_TRANSIENT);
    bool found = (sqlite3_step(stmt) == SQLITE_ROW);
    sqlite3_finalize(stmt);
    return found;
}

// 文件夹索引持久化

bool Database::saveFolderIndexBatch(const std::vector<FolderIndexEntry>& entries) {
    if (!m_db || entries.empty()) return false;
    const char* sql = "INSERT OR REPLACE INTO folder_index (folder_path, file_count, last_updated) VALUES (?, ?, ?)";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    beginTransaction();
    for (const auto& e : entries) {
        sqlite3_reset(stmt);
        sqlite3_bind_text(stmt, 1, e.folderPath.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 2, e.fileCount);
        sqlite3_bind_int64(stmt, 3, e.lastUpdated);
        sqlite3_step(stmt);
    }
    commitTransaction();
    sqlite3_finalize(stmt);
    return true;
}

std::vector<Database::FolderIndexEntry> Database::getFolderIndex() {
    std::vector<FolderIndexEntry> result;
    if (!m_db) return result;
    const char* sql = "SELECT folder_path, file_count, last_updated FROM folder_index ORDER BY folder_path";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) return result;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        FolderIndexEntry e;
        e.folderPath = safeGetText(stmt, 0);
        e.fileCount = sqlite3_column_int(stmt, 1);
        e.lastUpdated = sqlite3_column_int64(stmt, 2);
        if (!e.folderPath.empty()) result.push_back(std::move(e));
    }
    sqlite3_finalize(stmt);
    return result;
}
bool Database::clearFolderIndex() {
    if (!m_db) return false;
    return executeSQL("DELETE FROM folder_index;");
}
bool Database::rebuildFolderIndex() {
    if (!m_db) return false;
    if (!executeSQL("DELETE FROM folder_index")) return false;
    if (!executeSQL(
        "INSERT INTO folder_index (folder_path, file_count, last_updated) "
        "SELECT folder_path, COUNT(*) AS file_count, "
        "       CAST(strftime('%s','now') AS INTEGER) AS last_updated "
        "FROM audio_files "
        "WHERE folder_path IS NOT NULL AND folder_path <> '' "
        "GROUP BY folder_path")) {
        return false;
    }
    return rebuildTagFacetIndex();
}

bool Database::rebuildTagFacetIndex() {
    if (!m_db) return false;
    if (sqlite3_db_readonly(m_db, "main") == 1) return true;
    if (!executeSQL("DELETE FROM tag_facet_index")) return false;

    static const char* fields[] = {"norm_genre", "norm_instruments"};
    static const char* insertSql =
        "INSERT OR REPLACE INTO tag_facet_index(field, value, weighting) VALUES (?, ?, ?)";

    for (const char* field : fields) {
        const std::string selectSql =
            "SELECT " + std::string(field) + " FROM audio_files "
            "WHERE " + std::string(field) + " IS NOT NULL "
            "AND " + std::string(field) + " <> ''";

        sqlite3_stmt* readStmt = nullptr;
        if (sqlite3_prepare_v2(m_db, selectSql.c_str(), -1, &readStmt, nullptr) != SQLITE_OK) {
            return false;
        }

        std::unordered_map<std::string, int> counts;
        int rc = SQLITE_DONE;
        while ((rc = sqlite3_step(readStmt)) == SQLITE_ROW) {
            const char* raw = reinterpret_cast<const char*>(sqlite3_column_text(readStmt, 0));
            if (!raw) continue;
            AccumulateSemicolonFacetTokens(raw, &counts);
        }
        const bool readOk = (rc == SQLITE_DONE);
        sqlite3_finalize(readStmt);
        if (!readOk) return false;
        if (counts.empty()) continue;

        sqlite3_stmt* insertStmt = nullptr;
        if (sqlite3_prepare_v2(m_db, insertSql, -1, &insertStmt, nullptr) != SQLITE_OK) {
            return false;
        }

        for (const auto& [value, weight] : counts) {
            sqlite3_reset(insertStmt);
            sqlite3_clear_bindings(insertStmt);
            sqlite3_bind_text(insertStmt, 1, field, -1, SQLITE_STATIC);
            sqlite3_bind_text(insertStmt, 2, value.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(insertStmt, 3, weight);
            if (sqlite3_step(insertStmt) != SQLITE_DONE) {
                sqlite3_finalize(insertStmt);
                return false;
            }
        }
        sqlite3_finalize(insertStmt);
    }

    return WriteAppMetaInt(m_db, "tag_facet_index_valid", 1);
}

bool Database::invalidateTagFacetIndex() {
    if (!m_db) return false;
    if (sqlite3_db_readonly(m_db, "main") == 1) return true;
    return WriteAppMetaInt(m_db, "tag_facet_index_valid", 0);
}

bool Database::isTagFacetIndexValid() const {
    if (!m_db) return false;
    return ReadAppMetaInt(m_db, "tag_facet_index_valid") == 1;
}


// T2.8: One-shot migration — canonicalize folder_path columns

// 见 src/Database.h 注释 + docs/plans/2026-04-30-canonical-display-separation.md
//
// 流程：
//   0. 幂等门控 (app_meta "canonical_folder_path_migrated")
//   1. 收集所有 distinct folder_path → canonical 映射（仅有差异的）
//   2. BEGIN TRANSACTION
//   3. 清空 directories（避免 UNIQUE 冲突，让 lazy 重建）
//   4. 清空 folder_index（避免 PRIMARY KEY 冲突）
//   5. 批量 UPDATE audio_files SET folder_path = canonical
//   6. 重建 folder_index = SELECT folder_path, COUNT(*) FROM audio_files GROUP BY
//   7. 行数守恒校验：UPDATE 前后 SELECT COUNT(*) FROM audio_files 必须相等
//   8. COMMIT 成功 → 写 app_meta，否则 ROLLBACK
// Scalar reads fail on any prepare, step, or finalize error; a missing count
// must never be treated as zero.
static bool strictScalarInt64(sqlite3* db, const char* sql, int64_t& out) {
    out = 0;
    if (!db || !sql) return false;
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }
    int rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        out = sqlite3_column_int64(stmt, 0);
    } else if (rc != SQLITE_DONE) {
        sqlite3_finalize(stmt);
        return false;
    }
    if (sqlite3_finalize(stmt) != SQLITE_OK) {
        return false;
    }
    return true;
}

bool Database::migrateFolderPathToCanonical_v1() {
    if (!m_db) return false;
    DIAG_SCOPED("Database.migrateFolderPathToCanonical_v1");

    const int kCanonicalFolderPathMigrationVersion = 1;
    if (ReadAppMetaInt(m_db, "canonical_folder_path_migrated") >=
        kCanonicalFolderPathMigrationVersion) {
        return true;  // 已迁移，幂等跳过
    }

    // Collect canonical replacements and require the final SELECT step to succeed.

    std::vector<std::pair<std::string, std::string>> updates;  // (raw, canonical)
    {
        sqlite3_stmt* stmt = nullptr;
        const char* sql = "SELECT DISTINCT folder_path FROM audio_files "
                          "WHERE folder_path IS NOT NULL AND folder_path <> ''";
        if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            m_lastError = "migrateFolderPathToCanonical_v1: prepare DISTINCT failed";
            return false;
        }
        int rc;
        while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
            std::string raw = safeGetText(stmt, 0);
            std::string canon = HarkenSearch::CanonicalFolderPath(raw);
            if (!canon.empty() && canon != raw) {
                updates.emplace_back(std::move(raw), std::move(canon));
            }
        }
        bool stepOk = (rc == SQLITE_DONE);
        if (sqlite3_finalize(stmt) != SQLITE_OK || !stepOk) {
            m_lastError = "migrateFolderPathToCanonical_v1: SELECT DISTINCT step failed";
            return false;
        }
    }

    // 没有需要 update 的 row → 直接标记完成（仍校验 WriteAppMetaInt 失败）
    if (updates.empty()) {
        if (!WriteAppMetaInt(m_db, "canonical_folder_path_migrated",
                             kCanonicalFolderPathMigrationVersion)) {
            m_lastError = "migrateFolderPathToCanonical_v1: WriteAppMetaInt (no-op path) failed";
            return false;
        }
        return true;
    }

    // Abort the migration if the baseline count cannot be read.

    int64_t beforeCount = 0;
    if (!strictScalarInt64(m_db, "SELECT COUNT(*) FROM audio_files", beforeCount)) {
        m_lastError = "migrateFolderPathToCanonical_v1: baseline COUNT failed";
        return false;
    }

    // Start one transaction for cleanup, row updates, and the migration marker.
    if (!executeSQL("BEGIN TRANSACTION")) {
        return false;
    }

    auto rollbackAndFail = [this](const char* reason) -> bool {
        m_lastError = std::string("migrateFolderPathToCanonical_v1: ") + reason;
        executeSQL("ROLLBACK");
        return false;
    };

    // Clear directories and let ensureDirectoryId rebuild them lazily.
    // Eager parent-chain reconstruction is O(rows x depth) and unnecessary;
    // the self-referencing cascade has no external dependents.

    if (tableExists(m_db, "directories")) {
        if (!executeSQL("DELETE FROM directories")) {
            return rollbackAndFail("DELETE FROM directories failed");
        }
    }

    // Clear folder_index before canonical keys can collide.
    if (!executeSQL("DELETE FROM folder_index")) {
        return rollbackAndFail("DELETE FROM folder_index failed");
    }

    // Roll back if statement reset, binding, or execution fails.

    {
        const char* updSql =
            "UPDATE audio_files SET folder_path = ? WHERE folder_path = ?";
        sqlite3_stmt* updStmt = nullptr;
        if (sqlite3_prepare_v2(m_db, updSql, -1, &updStmt, nullptr) != SQLITE_OK) {
            return rollbackAndFail("prepare UPDATE failed");
        }
        for (const auto& [raw, canon] : updates) {
            if (sqlite3_reset(updStmt) != SQLITE_OK) {
                sqlite3_finalize(updStmt);
                return rollbackAndFail("UPDATE reset failed");
            }
            if (sqlite3_bind_text(updStmt, 1, canon.c_str(), -1, SQLITE_TRANSIENT) != SQLITE_OK ||
                sqlite3_bind_text(updStmt, 2, raw.c_str(),   -1, SQLITE_TRANSIENT) != SQLITE_OK) {
                sqlite3_finalize(updStmt);
                return rollbackAndFail("UPDATE bind failed");
            }
            if (sqlite3_step(updStmt) != SQLITE_DONE) {
                sqlite3_finalize(updStmt);
                return rollbackAndFail("UPDATE step failed");
            }
        }
        if (sqlite3_finalize(updStmt) != SQLITE_OK) {
            return rollbackAndFail("UPDATE finalize failed");
        }
    }

    // Rebuild folder_index from canonical audio_files paths.
    // 注意：last_updated 语义是「索引重建时间戳」（同一次 migration 全相同），
    // 不是「文件夹最后修改时间」。索引访问时若需要后者，应回查 audio_files.file_mtime。
    {
        const char* rebuildSql =
            "INSERT INTO folder_index (folder_path, file_count, last_updated) "
            "SELECT folder_path, COUNT(*) AS file_count, "
            "       CAST(strftime('%s','now') AS INTEGER) AS last_updated "
            "FROM audio_files "
            "WHERE folder_path IS NOT NULL AND folder_path <> '' "
            "GROUP BY folder_path";
        if (!executeSQL(rebuildSql)) {
            return rollbackAndFail("rebuild folder_index failed");
        }
    }

    // Verify that the migration preserves the audio_files row count.
    int64_t afterCount = 0;
    if (!strictScalarInt64(m_db, "SELECT COUNT(*) FROM audio_files", afterCount)) {
        return rollbackAndFail("after-COUNT query failed");
    }
    if (afterCount != beforeCount) {
        char buf[128];
        snprintf(buf, sizeof(buf),
                 "file count mismatch (before=%lld, after=%lld)",
                 (long long)beforeCount, (long long)afterCount);
        return rollbackAndFail(buf);
    }

    // Write the migration marker in the same transaction as the row changes.

    if (!WriteAppMetaInt(m_db, "canonical_folder_path_migrated",
                         kCanonicalFolderPathMigrationVersion)) {
        return rollbackAndFail("WriteAppMetaInt failed");
    }

    // Commit only after row counts and the migration marker succeed.
    if (!executeSQL("COMMIT")) {
        return rollbackAndFail("COMMIT failed");
    }
    return true;
}


// STUBS for parallel-session-in-progress work
// 见 Database.h 的对应注释。这些 stub 让主 Harken Search target build 通，
// 真实实现进来后会自然替换。

int Database::removeFolderSubtreeFromDb(const std::string& folderPath, bool deleteFilesFromDisk) {
    if (!m_db) return 0;
    m_lastError.clear();

    const std::string targetPath = HarkenSearch::CanonicalFolderPath(folderPath);
    if (targetPath.empty()) return 0;

    std::string rawTargetPath = NormalizeFolderPathPreserveCaseForDbMatch(folderPath);
    if (rawTargetPath.empty()) rawTargetPath = targetPath;
    const std::vector<std::string> matchPaths = BuildFolderSubtreeMatchPaths(targetPath, rawTargetPath);

    std::vector<std::string> candidatePaths;
    std::vector<int64_t> candidateIds;
    int removed = 0;

    std::unique_lock<std::mutex> writerLk(m_writerSerializeMutex);

    const char* createCandidateTableSql =
        "CREATE TEMP TABLE IF NOT EXISTS temp.remove_folder_subtree_candidates ("
        "id INTEGER PRIMARY KEY, "
        "file_path TEXT"
        ");";
    if (!executeSQL(createCandidateTableSql) ||
        !executeSQL("DELETE FROM temp.remove_folder_subtree_candidates;")) {
        m_lastError = "removeFolderSubtreeFromDb: prepare candidate table failed";
        return 0;
    }

    const char* insertCandidatesSql =
        "INSERT OR IGNORE INTO temp.remove_folder_subtree_candidates(id, file_path) "
        "SELECT id, file_path "
        "FROM main.audio_files "
        "WHERE folder_path = ? "
        "   OR (folder_path >= ? AND folder_path < ?) "
        "   OR (file_path >= ? AND file_path < ?);";
    auto insertCandidatesForMatchPaths = [&](const char* sql, const char* errorContext) -> bool {
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            m_lastError = sqlite3_errmsg(m_db);
            return false;
        }
        for (const std::string& matchPath : matchPaths) {
            const std::string childPrefix = MakeFolderChildPrefix(matchPath);
            const std::string childUpper = MakeFolderPathUpperBound(childPrefix);
            sqlite3_reset(stmt);
            sqlite3_clear_bindings(stmt);
            sqlite3_bind_text(stmt, 1, matchPath.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 2, childPrefix.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 3, childUpper.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 4, childPrefix.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 5, childUpper.c_str(), -1, SQLITE_TRANSIENT);
            if (sqlite3_step(stmt) != SQLITE_DONE) {
                m_lastError = errorContext && *errorContext ? errorContext : sqlite3_errmsg(m_db);
                sqlite3_finalize(stmt);
                return false;
            }
        }
        sqlite3_finalize(stmt);
        return true;
    };
    if (!insertCandidatesForMatchPaths(insertCandidatesSql,
                                       "removeFolderSubtreeFromDb: fast candidate insert failed")) {
        executeSQL("DELETE FROM temp.remove_folder_subtree_candidates;");
        return 0;
    }

    int64_t candidateCount = 0;
    if (!strictScalarInt64(m_db,
                           "SELECT COUNT(*) FROM temp.remove_folder_subtree_candidates;",
                           candidateCount)) {
        m_lastError = "removeFolderSubtreeFromDb: candidate count failed";
        executeSQL("DELETE FROM temp.remove_folder_subtree_candidates;");
        return 0;
    }
    if (candidateCount == 0) {
        const char* insertCandidatesNoCaseSql =
            "INSERT OR IGNORE INTO temp.remove_folder_subtree_candidates(id, file_path) "
            "SELECT id, file_path "
            "FROM main.audio_files "
            "WHERE folder_path = ? COLLATE NOCASE "
            "   OR (folder_path >= ? COLLATE NOCASE AND folder_path < ? COLLATE NOCASE) "
            "   OR (file_path >= ? COLLATE NOCASE AND file_path < ? COLLATE NOCASE);";
        if (!insertCandidatesForMatchPaths(insertCandidatesNoCaseSql,
                                           "removeFolderSubtreeFromDb: nocase fallback candidate insert failed")) {
            executeSQL("DELETE FROM temp.remove_folder_subtree_candidates;");
            return 0;
        }
        if (!strictScalarInt64(m_db,
                               "SELECT COUNT(*) FROM temp.remove_folder_subtree_candidates;",
                               candidateCount)) {
            m_lastError = "removeFolderSubtreeFromDb: fallback candidate count failed";
            executeSQL("DELETE FROM temp.remove_folder_subtree_candidates;");
            return 0;
        }
        if (candidateCount > 0) {
            DIAG_ACTION_FMT("Database",
                            "RemoveFolderSubtreeNoCaseFallback path=%s candidates=%lld",
                            targetPath.c_str(),
                            static_cast<long long>(candidateCount));
        }
    }
    removed = static_cast<int>((std::min<int64_t>)(candidateCount, std::numeric_limits<int>::max()));

    sqlite3_stmt* selectPathsStmt = nullptr;
    if (sqlite3_prepare_v2(m_db,
                           "SELECT id, file_path FROM temp.remove_folder_subtree_candidates;",
                           -1, &selectPathsStmt, nullptr) == SQLITE_OK) {
        candidatePaths.reserve(static_cast<size_t>((std::min<int64_t>)(candidateCount, 1000000)));
        candidateIds.reserve(static_cast<size_t>((std::min<int64_t>)(candidateCount, 1000000)));
        int pathRc = SQLITE_OK;
        while ((pathRc = sqlite3_step(selectPathsStmt)) == SQLITE_ROW) {
            const int64_t id = sqlite3_column_int64(selectPathsStmt, 0);
            if (id > 0) candidateIds.push_back(id);
            std::string path = safeGetText(selectPathsStmt, 1);
            if (!path.empty()) candidatePaths.push_back(std::move(path));
        }
        if (pathRc != SQLITE_DONE) {
            m_lastError = sqlite3_errmsg(m_db);
            sqlite3_finalize(selectPathsStmt);
            executeSQL("DELETE FROM temp.remove_folder_subtree_candidates;");
            return 0;
        }
        sqlite3_finalize(selectPathsStmt);
    } else {
        m_lastError = sqlite3_errmsg(m_db);
        executeSQL("DELETE FROM temp.remove_folder_subtree_candidates;");
        return 0;
    }

    if (!beginTransaction()) {
        executeSQL("DELETE FROM temp.remove_folder_subtree_candidates;");
        return 0;
    }

    auto rollbackWithError = [this](const char* message) -> int {
        if (message && *message) {
            m_lastError = message;
        }
        rollbackTransaction();
        executeSQL("DELETE FROM temp.remove_folder_subtree_candidates;");
        return 0;
    };

    struct ConditionalDeleteStep {
        const char* schemaName;
        const char* tableName;
        const char* sql;
        bool required;
        const char* requiredColumn;
    };
    const ConditionalDeleteStep batchDeleteSqls[] = {
        {"waveforms_db", "waveforms",
         "DELETE FROM waveforms_db.waveforms "
         "WHERE file_id IN (SELECT id FROM temp.remove_folder_subtree_candidates)",
         true, "file_id"},
        {"waveforms_db", "external_waveforms",
         "DELETE FROM waveforms_db.external_waveforms "
         "WHERE file_path IN ("
         "  SELECT file_path FROM temp.remove_folder_subtree_candidates "
         "  WHERE file_path IS NOT NULL AND file_path <> ''"
         ")",
         true, "file_path"},
        {"main", "playlist_items",
         "DELETE FROM playlist_items "
         "WHERE file_id IN (SELECT id FROM temp.remove_folder_subtree_candidates)",
         true, "file_id"},
        {"main", "file_tags",
         "DELETE FROM file_tags "
         "WHERE file_id IN (SELECT id FROM temp.remove_folder_subtree_candidates)",
         true, "file_id"},
        {"main", "play_queue",
         "DELETE FROM play_queue "
         "WHERE file_id IN (SELECT id FROM temp.remove_folder_subtree_candidates)",
         true, "file_id"},
        {"main", "backfill_pending",
         "DELETE FROM backfill_pending "
         "WHERE file_id IN (SELECT id FROM temp.remove_folder_subtree_candidates)",
         false, "file_id"},
    };
    for (const auto& step : batchDeleteSqls) {
        std::string qualifiedTable = step.schemaName == std::string("main")
            ? step.tableName
            : std::string(step.schemaName) + "." + step.tableName;
        if (step.requiredColumn && !columnExists(m_db, qualifiedTable.c_str(), step.requiredColumn)) {
            if (!step.required) {
                continue;
            }
            m_lastError = std::string("removeFolderSubtreeFromDb: required cleanup table missing column table=")
                + qualifiedTable + " column=" + step.requiredColumn;
            return rollbackWithError(nullptr);
        }
        if (!tableExistsInSchema(m_db, step.schemaName, step.tableName)) {
            continue;
        }
        if (!executeSQL(step.sql)) {
            if (!step.required) {
                DIAG_ACTION_FMT("Database",
                                "RemoveFolderSubtreeSkipOptionalCleanup table=%s.%s err=%s",
                                step.schemaName,
                                step.tableName,
                                sqlite3_errmsg(m_db));
                continue;
            }
            m_lastError = std::string("removeFolderSubtreeFromDb: batch cleanup failed table=")
                + step.schemaName + "." + step.tableName + " err=" + sqlite3_errmsg(m_db);
            return rollbackWithError(nullptr);
        }
    }

    if (!executeSQL("DELETE FROM audio_files WHERE id IN (SELECT id FROM temp.remove_folder_subtree_candidates)")) {
        const std::string deleteErr = sqlite3_errmsg(m_db);
        static thread_local bool s_retryingAfterSearchIndexRepair = false;
        if (!s_retryingAfterSearchIndexRepair &&
            IsRecoverableSearchIndexMalformedError(deleteErr)) {
            rollbackTransaction();
            executeSQL("DELETE FROM temp.remove_folder_subtree_candidates;");
            std::string repairError;
            const bool dropped = DropSearchIndexTablesForRecovery(m_db, &repairError);
            const bool rebuilt = dropped && ensureFtsSchema();
            if (rebuilt) {
                DIAG_ACTION_FMT("Database",
                                "RemoveFolderSubtreeRecoveredSearchIndexCorruption path=%s err=%s",
                                targetPath.c_str(),
                                deleteErr.c_str());
                writerLk.unlock();
                s_retryingAfterSearchIndexRepair = true;
                const int retried = removeFolderSubtreeFromDb(folderPath, deleteFilesFromDisk);
                s_retryingAfterSearchIndexRepair = false;
                return retried;
            }
            m_lastError = std::string("removeFolderSubtreeFromDb: audio row batch delete failed err=")
                + deleteErr
                + " repair="
                + (repairError.empty() ? m_lastError : repairError);
            return 0;
        }
        m_lastError = std::string("removeFolderSubtreeFromDb: audio row batch delete failed err=")
            + deleteErr;
        return rollbackWithError(nullptr);
    }

    // Keep sidecar/cache tables honest even for older databases that were
    // created before FK cascades existed, or for test packages that reused a
    // dirty sidecar database next to a fresh main.db.
    const ConditionalDeleteStep pruneOrphanSqls[] = {
        {"main", "playlist_items",
         "DELETE FROM playlist_items "
         "WHERE file_id NOT IN (SELECT id FROM main.audio_files)",
         true, "file_id"},
        {"main", "file_tags",
         "DELETE FROM file_tags "
         "WHERE file_id NOT IN (SELECT id FROM main.audio_files)",
         true, "file_id"},
        {"main", "play_queue",
         "DELETE FROM play_queue "
         "WHERE file_id NOT IN (SELECT id FROM main.audio_files)",
         true, "file_id"},
        {"main", "backfill_pending",
         "DELETE FROM backfill_pending "
         "WHERE file_id NOT IN (SELECT id FROM main.audio_files)",
         false, "file_id"},
        {"waveforms_db", "waveforms",
         "DELETE FROM waveforms_db.waveforms "
         "WHERE file_id NOT IN (SELECT id FROM main.audio_files)",
         true, "file_id"},
        {"waveforms_db", "external_waveforms",
         "DELETE FROM waveforms_db.external_waveforms "
         "WHERE file_path NOT IN (SELECT file_path FROM main.audio_files)",
         true, "file_path"},
    };
    for (const auto& step : pruneOrphanSqls) {
        std::string qualifiedTable = step.schemaName == std::string("main")
            ? step.tableName
            : std::string(step.schemaName) + "." + step.tableName;
        if (step.requiredColumn && !columnExists(m_db, qualifiedTable.c_str(), step.requiredColumn)) {
            if (!step.required) {
                continue;
            }
            m_lastError = std::string("removeFolderSubtreeFromDb: required prune table missing column table=")
                + qualifiedTable + " column=" + step.requiredColumn;
            return rollbackWithError(nullptr);
        }
        if (!tableExistsInSchema(m_db, step.schemaName, step.tableName)) {
            continue;
        }
        if (!executeSQL(step.sql)) {
            if (!step.required) {
                DIAG_ACTION_FMT("Database",
                                "RemoveFolderSubtreeSkipOptionalPrune table=%s.%s err=%s",
                                step.schemaName,
                                step.tableName,
                                sqlite3_errmsg(m_db));
                continue;
            }
            m_lastError = std::string("removeFolderSubtreeFromDb: prune orphan cache rows failed table=")
                + step.schemaName + "." + step.tableName + " err=" + sqlite3_errmsg(m_db);
            return rollbackWithError(nullptr);
        }
    }

    if (tableExists(m_db, "directories")) {
        sqlite3_stmt* deleteDirsStmt = nullptr;
        const char* deleteDirsSql =
            "DELETE FROM directories "
            "WHERE folder_path = ? "
            "   OR (folder_path >= ? AND folder_path < ?)";
        if (sqlite3_prepare_v2(m_db, deleteDirsSql, -1, &deleteDirsStmt, nullptr) == SQLITE_OK) {
            for (const std::string& matchPath : matchPaths) {
                const std::string childPrefix = MakeFolderChildPrefix(matchPath);
                const std::string childUpper = MakeFolderPathUpperBound(childPrefix);
                sqlite3_reset(deleteDirsStmt);
                sqlite3_clear_bindings(deleteDirsStmt);
                sqlite3_bind_text(deleteDirsStmt, 1, matchPath.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(deleteDirsStmt, 2, childPrefix.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(deleteDirsStmt, 3, childUpper.c_str(), -1, SQLITE_TRANSIENT);
                if (sqlite3_step(deleteDirsStmt) != SQLITE_DONE) {
                    sqlite3_finalize(deleteDirsStmt);
                    return rollbackWithError(sqlite3_errmsg(m_db));
                }
            }
            sqlite3_finalize(deleteDirsStmt);
        } else {
            return rollbackWithError(sqlite3_errmsg(m_db));
        }
    }

    if (!executeSQL("DELETE FROM folder_index;")) {
        return rollbackWithError("removeFolderSubtreeFromDb: clear folder_index failed");
    }
    const char* rebuildFolderIndexSql =
        "INSERT INTO folder_index (folder_path, file_count, last_updated) "
        "SELECT folder_path, COUNT(*) AS file_count, "
        "       CAST(strftime('%s','now') AS INTEGER) AS last_updated "
        "FROM audio_files "
        "WHERE folder_path IS NOT NULL AND folder_path <> '' "
        "GROUP BY folder_path";
    if (!executeSQL(rebuildFolderIndexSql)) {
        return rollbackWithError("removeFolderSubtreeFromDb: rebuild folder_index failed");
    }

    if (!commitTransaction()) {
        rollbackTransaction();
        executeSQL("DELETE FROM temp.remove_folder_subtree_candidates;");
        return 0;
    }
    executeSQL("DELETE FROM temp.remove_folder_subtree_candidates;");

    if (removed > 0) {
        pruneOrphanCoverCaches();
        invalidateFolderCoverFacetIndexForPaths(candidatePaths);
        invalidateFacetQueryCache();
        bumpDataRevision();
    } else {
        invalidateFacetQueryCache();
    }

    int diskDeleted = 0;
    int diskFailed = 0;
    if (deleteFilesFromDisk) {
        for (const std::string& path : candidatePaths) {
            if (path.empty()) continue;
            try {
                const fs::path diskPath = MakeLongPath(Utf8ToPath(path));
                const std::string diskPathUtf8 = PathToUtf8(diskPath);
                std::string probeError;
                const auto pathStatus =
                    Platform::FileSystem::ProbePath(diskPathUtf8, &probeError);
                if (pathStatus == Platform::FileSystem::PathStatus::Present &&
                    Platform::FileSystem::ProbeRegularFile(diskPathUtf8).isRegularFile) {
                    std::string removeError;
                    if (Platform::FileSystem::RemoveFile(diskPathUtf8, &removeError)) {
                        ++diskDeleted;
                    } else if (!removeError.empty()) {
                        ++diskFailed;
                    }
                } else if (pathStatus == Platform::FileSystem::PathStatus::Error) {
                    ++diskFailed;
                }
            } catch (...) {
                ++diskFailed;
            }
        }
    }

    DIAG_ACTION_FMT("Database",
                    "RemoveFolderSubtree path=%s removed=%d deleteDisk=%d diskDeleted=%d diskFailed=%d",
                    targetPath.c_str(), removed, deleteFilesFromDisk ? 1 : 0, diskDeleted, diskFailed);
    m_lastError.clear();
    return removed;
}

void Database::detachConnectionForSwitch() {
    DIAG_SYSTEM("Database", "detachConnectionForSwitch_STUB_called");
    // no-op: 真实实现应释放 m_db connection 让别的进程/连接接管
}

std::vector<Database::WaveformRebuildTask>
Database::getWaveformDetailRebuildTasks(int limit,
                                         int64_t lastClaimedFileId,
                                         bool incrementalOnly) {
    std::vector<WaveformRebuildTask> tasks;
    if (!m_db) return tasks;
    if (getWaveformCacheMode() == WaveformCachePolicy::Mode::MiniOnly) return tasks;

    limit = std::clamp(limit, 1, 10000);
    const std::string detailMinBytes =
        "((CASE WHEN af.total_frames > 0 AND af.total_frames < (CASE WHEN af.duration > 120.0 THEN 1000 WHEN af.duration > 30.0 THEN 2000 ELSE 3000 END) THEN af.total_frames ELSE (CASE WHEN af.duration > 120.0 THEN 1000 WHEN af.duration > 30.0 THEN 2000 ELSE 3000 END) END) * "
        " (CASE WHEN af.channels > 0 THEN af.channels ELSE 1 END) * 4)";
    const std::string sqlIncremental =
        "SELECT af.id, af.file_path "
        "FROM main.audio_files af "
        "LEFT JOIN waveforms_db.waveforms w ON af.id = w.file_id "
        "WHERE af.id > ? "
        "  AND (af.duration <= 0 OR af.duration <= 300.0) "
        "  AND ("
        "      (af.waveform_detail IS NULL OR length(af.waveform_detail) < " + detailMinBytes + ") "
        "      AND " + DetailMissingSqlExpr("w", detailMinBytes) +
        "  ) "
        "ORDER BY af.id ASC "
        "LIMIT ?";
    const char* sqlFull = R"(
        SELECT af.id, af.file_path
        FROM main.audio_files af
        WHERE af.id > ?
        ORDER BY af.id ASC
        LIMIT ?
    )";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, incrementalOnly ? sqlIncremental.c_str() : sqlFull, -1, &stmt, nullptr) != SQLITE_OK) {
        return tasks;
    }
    sqlite3_bind_int64(stmt, 1, std::max<int64_t>(0, lastClaimedFileId));
    sqlite3_bind_int(stmt, 2, limit);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        WaveformRebuildTask task;
        task.fileId = sqlite3_column_int64(stmt, 0);
        task.filePath = safeGetText(stmt, 1);
        if (task.fileId > 0 && !task.filePath.empty()) {
            tasks.push_back(std::move(task));
        }
    }
    sqlite3_finalize(stmt);
    return tasks;
}

bool Database::saveWaveformDetailBatch(
    const std::vector<WaveformDetailBatchEntry>& batch) {
    if (!m_db) return false;
    if (batch.empty()) return true;

    const char* sql = R"(
        INSERT INTO waveforms_db.waveforms
            (file_id, waveform_detail, waveform_min_detail, detail_num_channels, status, error_code, retry_count, updated_at)
        VALUES (?, ?, ?, ?, 'done', '', 0, ?)
        ON CONFLICT(file_id) DO UPDATE SET
            waveform_detail = excluded.waveform_detail,
            waveform_min_detail = excluded.waveform_min_detail,
            detail_num_channels = excluded.detail_num_channels,
            status = 'done',
            error_code = '',
            retry_count = 0,
            updated_at = excluded.updated_at
    )";

    std::lock_guard<std::mutex> writerLk(m_writerSerializeMutex);
    const bool ownTxn = sqlite3_get_autocommit(m_db) != 0;
    bool txnBegan = false;
    if (ownTxn) {
        txnBegan = beginTransaction();
        if (!txnBegan) return false;
    }

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        if (txnBegan) rollbackTransaction();
        return false;
    }

    bool ok = true;
    const int64_t now = getCurrentTimestamp();
    const bool wpk2WriterEnabled = IsWpk2DetailWriterEnabled();
    std::vector<uint8_t> wpk2Blob;
    for (const auto& entry : batch) {
        const int detailChannels = entry.detailChannels > 0 ? entry.detailChannels : 1;
        const int legacyBlobSize = SafeSqliteBlobSize(entry.waveform.size() * sizeof(float));
        if (entry.fileId <= 0 || entry.waveform.empty() || legacyBlobSize < 0) {
            ok = false;
            break;
        }
        const bool writeWpk2 = TryEncodeWpk2DetailBlob(
            entry.waveform,
            entry.minWaveform,
            detailChannels,
            wpk2WriterEnabled,
            wpk2Blob);
        sqlite3_reset(stmt);
        sqlite3_clear_bindings(stmt);
        sqlite3_bind_int64(stmt, 1, entry.fileId);
        if (writeWpk2) {
            const int wpk2BlobSize = SafeSqliteBlobSize(wpk2Blob.size());
            if (wpk2BlobSize < 0) {
                ok = false;
                break;
            }
            sqlite3_bind_blob(stmt, 2, wpk2Blob.data(), wpk2BlobSize, SQLITE_TRANSIENT);
            sqlite3_bind_null(stmt, 3);
        } else {
            sqlite3_bind_blob(stmt, 2, entry.waveform.data(), legacyBlobSize, SQLITE_TRANSIENT);
            if (entry.minWaveform.size() == entry.waveform.size()) {
                sqlite3_bind_blob(stmt, 3, entry.minWaveform.data(), legacyBlobSize, SQLITE_TRANSIENT);
            } else {
                sqlite3_bind_null(stmt, 3);
            }
        }
        sqlite3_bind_int(stmt, 4, detailChannels);
        sqlite3_bind_int64(stmt, 5, now);
        if (sqlite3_step(stmt) != SQLITE_DONE) {
            ok = false;
            break;
        }
    }

    sqlite3_finalize(stmt);
    if (txnBegan) {
        ok = ok ? commitTransaction() : (rollbackTransaction(), false);
    }
    return ok;
}


// Database split implementation

// Store text metadata in the main database and media BLOBs in sidecars.

std::string Database::getBlobsDbPath(const std::string& mainDbPath) const {
    if (mainDbPath.empty()) return "";
    fs::path mainPath = Utf8ToPath(mainDbPath);
    fs::path parentDir = mainPath.parent_path();
    std::string fileName = toLowerAsciiCopy(PathToUtf8(mainPath.filename()));
    if (fileName == "main.db") {
        return PathToUtf8(parentDir / "blobs.db");
    }
    std::string stem = PathToUtf8(mainPath.stem());
    // Use Utf8ToPath so the UTF-8 stem is converted to a wide fs::path before
    // joining. On Chinese Windows (GBK codepage) fs::path/std::string would
    // misinterpret multi-byte UTF-8 sequences as GBK, corrupting the name.
    return PathToUtf8(parentDir / Utf8ToPath(stem + "_blobs.db"));
}

std::string Database::getWaveformsDbPath(const std::string& mainDbPath) const {
    if (mainDbPath.empty()) return "";
    fs::path mainPath = Utf8ToPath(mainDbPath);
    fs::path parentDir = mainPath.parent_path();
    std::string fileName = toLowerAsciiCopy(PathToUtf8(mainPath.filename()));
    if (fileName == "main.db") {
        return PathToUtf8(parentDir / "waveforms.db");
    }
    std::string stem = PathToUtf8(mainPath.stem());
    return PathToUtf8(parentDir / Utf8ToPath(stem + "_waveforms.db"));
}

// Cache version 8 invalidates detail peaks written with silence-padding stretch.
// Those peaks duplicate audio across the timeline after stretching is disabled.
// Lazy re-extraction restores detail waveforms; mini caches are unaffected.

static constexpr int kWaveformCacheVersion = 8;

static int ReadWaveformCacheVersion(sqlite3* db) {
    if (!db) return 0;
    sqlite3_stmt* stmt = nullptr;
    int version = 0;
    const char* sql = "SELECT value FROM waveforms_db.meta WHERE key = 'cache_version' LIMIT 1;";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            const unsigned char* txt = sqlite3_column_text(stmt, 0);
            if (txt) version = std::atoi(reinterpret_cast<const char*>(txt));
        }
    }
    if (stmt) sqlite3_finalize(stmt);
    return version;
}

static bool WriteWaveformCacheVersion(sqlite3* db, int version) {
    if (!db) return false;
    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "INSERT INTO waveforms_db.meta(key, value) VALUES('cache_version', ?) "
        "ON CONFLICT(key) DO UPDATE SET value = excluded.value;";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }
    std::string value = std::to_string(version);
    sqlite3_bind_text(stmt, 1, value.c_str(), -1, SQLITE_TRANSIENT);
    bool ok = (sqlite3_step(stmt) == SQLITE_DONE);
    sqlite3_finalize(stmt);
    return ok;
}

bool Database::attachSidecarDatabases(const std::string& mainDbPath, bool readOnly) {
    if (!m_db || mainDbPath.empty()) return false;
    if (!Platform::HarkenUserData::PreviewIsolationAllowsDataPathUtf8(mainDbPath)) {
        return false;
    }
    
    m_mainDbPath = mainDbPath;
    m_blobsDbPath = getBlobsDbPath(mainDbPath);
    m_waveformsDbPath = getWaveformsDbPath(mainDbPath);
    if (!Platform::HarkenUserData::PreviewIsolationAllowsDataPathUtf8(m_blobsDbPath) ||
        !Platform::HarkenUserData::PreviewIsolationAllowsDataPathUtf8(m_waveformsDbPath)) {
        return false;
    }
    
    bool blobsMissing = false;
    bool waveformsMissing = false;
    if (readOnly) {
        blobsMissing = Platform::FileSystem::ProbePath(m_blobsDbPath) !=
            Platform::FileSystem::PathStatus::Present;
        waveformsMissing = Platform::FileSystem::ProbePath(m_waveformsDbPath) !=
            Platform::FileSystem::PathStatus::Present;
        if (sqlite3_exec(m_db, "PRAGMA query_only=ON;", nullptr, nullptr, nullptr) != SQLITE_OK) {
            m_lastError = sqlite3_errmsg(m_db);
            return false;
        }
    } else try {
        const fs::path blobsPath = Utf8ToPath(m_blobsDbPath);
        const fs::path waveformsPath = Utf8ToPath(m_waveformsDbPath);
        auto probeSidecar = [](const std::string& pathUtf8, bool& missing) {
            std::string probeError;
            const auto status = Platform::FileSystem::ProbePath(pathUtf8, &probeError);
            if (status == Platform::FileSystem::PathStatus::Error) {
                DIAG_ACTION_FMT("Database",
                                "SidecarPrepareProbeFailed: path=%s error=%s",
                                pathUtf8.c_str(),
                                probeError.c_str());
            }
            const bool isMissing =
                status == Platform::FileSystem::PathStatus::Missing;
            const bool isPresent =
                status == Platform::FileSystem::PathStatus::Present;
            if (!isMissing && !isPresent) {
                missing = false;
                return;
            }
            missing = isMissing;
        };
        probeSidecar(m_blobsDbPath, blobsMissing);
        probeSidecar(m_waveformsDbPath, waveformsMissing);

        for (const fs::path& sidecarPath : {blobsPath, waveformsPath}) {
            const fs::path parentPath = sidecarPath.parent_path();
            if (!parentPath.empty() &&
                !Platform::FileSystem::EnsureDirectoryExists(PathToUtf8(parentPath))) {
                DIAG_ACTION_FMT("Database",
                                "SidecarPrepareParentFailed: path=%s",
                                PathToUtf8(parentPath).c_str());
            }
        }

        if (blobsMissing) {
            sqlite3* tempDb = nullptr;
#ifdef _WIN32
            const std::wstring widePath = blobsPath.wstring();
            if (sqlite3_open16(widePath.c_str(), &tempDb) == SQLITE_OK) {
                sqlite3_close(tempDb);
            }
#else
            if (sqlite3_open(m_blobsDbPath.c_str(), &tempDb) == SQLITE_OK) {
                sqlite3_close(tempDb);
            }
#endif
        }
        if (waveformsMissing) {
            sqlite3* tempDb = nullptr;
#ifdef _WIN32
            const std::wstring widePath = waveformsPath.wstring();
            if (sqlite3_open16(widePath.c_str(), &tempDb) == SQLITE_OK) {
                sqlite3_close(tempDb);
            }
#else
            if (sqlite3_open(m_waveformsDbPath.c_str(), &tempDb) == SQLITE_OK) {
                sqlite3_close(tempDb);
            }
#endif
        }
    } catch (const std::exception& e) {
        DIAG_ACTION_FMT("Database",
                        "SidecarPrepareException: %s",
                        e.what());
    } catch (...) {
        DIAG_ACTION("Database", "SidecarPrepareException: unknown");
    }

    // Attach the artwork-thumbnail sidecar.
    std::string attachBlobs = "ATTACH DATABASE ? AS blobs_db;";
    // Only a file-level failure of a rebuildable cache may use memory. SQL
    // mistakes, allocation failures, and main-database errors still fail open.
    auto attachMemoryAfterCacheFailure = [&](int error, const std::string& sql) {
        if (!readOnly) return false;
        switch (error & 0xff) {
        case SQLITE_NOTADB:
        case SQLITE_CORRUPT:
        case SQLITE_CANTOPEN:
        case SQLITE_IOERR:
        case SQLITE_PERM:
            break;
        default:
            return false;
        }
        sqlite3_stmt* fallback = nullptr;
        if (sqlite3_prepare_v2(m_db, sql.c_str(), -1, &fallback, nullptr) != SQLITE_OK) {
            m_lastError = sqlite3_errmsg(m_db);
            return false;
        }
#ifdef _WIN32
        sqlite3_bind_text16(fallback, 1, L":memory:", -1, SQLITE_TRANSIENT);
#else
        sqlite3_bind_text(fallback, 1, ":memory:", -1, SQLITE_TRANSIENT);
#endif
        const int retryResult = sqlite3_step(fallback);
        if (retryResult != SQLITE_DONE) m_lastError = sqlite3_errmsg(m_db);
        sqlite3_finalize(fallback);
        if (retryResult != SQLITE_DONE) return false;
        m_lastError.clear();
        return true;
    };
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, attachBlobs.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        m_lastError = sqlite3_errmsg(m_db);
        return false;
    }
#ifdef _WIN32
    std::wstring widePath = readOnly && blobsMissing
        ? L":memory:" : Utf8ToPath(m_blobsDbPath).wstring();
    sqlite3_bind_text16(stmt, 1, widePath.c_str(), -1, SQLITE_TRANSIENT);
#else
    sqlite3_bind_text(stmt, 1, readOnly && blobsMissing ? ":memory:" : m_blobsDbPath.c_str(), -1, SQLITE_TRANSIENT);
#endif
    if (sqlite3_step(stmt) != SQLITE_DONE) {
        const int sidecarError = sqlite3_extended_errcode(m_db);
        m_lastError = sqlite3_errmsg(m_db);
        sqlite3_finalize(stmt);
        if (!attachMemoryAfterCacheFailure(sidecarError, attachBlobs)) return false;
    } else {
        sqlite3_finalize(stmt);
    }
    
    // Attach the waveform-data sidecar.
    std::string attachWaveforms = "ATTACH DATABASE ? AS waveforms_db;";
    stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, attachWaveforms.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        m_lastError = sqlite3_errmsg(m_db);
        return false;
    }
#ifdef _WIN32
    std::wstring wideWaveformsPath = readOnly && waveformsMissing
        ? L":memory:" : Utf8ToPath(m_waveformsDbPath).wstring();
    sqlite3_bind_text16(stmt, 1, wideWaveformsPath.c_str(), -1, SQLITE_TRANSIENT);
#else
    sqlite3_bind_text(stmt, 1, readOnly && waveformsMissing ? ":memory:" : m_waveformsDbPath.c_str(), -1, SQLITE_TRANSIENT);
#endif
    if (sqlite3_step(stmt) != SQLITE_DONE) {
        const int sidecarError = sqlite3_extended_errcode(m_db);
        m_lastError = sqlite3_errmsg(m_db);
        sqlite3_finalize(stmt);
        if (!attachMemoryAfterCacheFailure(sidecarError, attachWaveforms)) return false;
    } else {
        sqlite3_finalize(stmt);
    }
    
    return true;
}

bool Database::createSidecarTables() {
    if (!m_db) return false;
    
    // Create cover_thumbs table in blobs_db
    const char* createCoverThumbs = R"(
        CREATE TABLE IF NOT EXISTS blobs_db.cover_thumbs (
            cover_key   TEXT PRIMARY KEY,
            thumb_data  BLOB,
            mime        TEXT,
            created_at  INTEGER DEFAULT 0
        );
    )";
    if (!executeSQL(createCoverThumbs)) return false;
    
    // Create waveforms table in waveforms_db
    // 注意：不使用 FOREIGN KEY 约束跨库引用，SQLite ATTACH 数据库不支持跨库 FK
    const char* createWaveforms = R"(
        CREATE TABLE IF NOT EXISTS waveforms_db.waveforms (
            file_id             INTEGER PRIMARY KEY,
            waveform_data       BLOB,
            mini_min_data       BLOB,
            waveform_detail     BLOB,
            waveform_min_detail BLOB,
            detail_num_channels INTEGER DEFAULT 0,
            rms_data            BLOB,
            status              TEXT DEFAULT 'pending',
            error_code          TEXT DEFAULT '',
            retry_count         INTEGER DEFAULT 0,
            updated_at          INTEGER DEFAULT 0,
            cache_key           TEXT DEFAULT ''
        );
    )";
    if (!executeSQL(createWaveforms)) return false;

    const char* createExternalWaveforms = R"(
        CREATE TABLE IF NOT EXISTS waveforms_db.external_waveforms (
            file_path           TEXT PRIMARY KEY,
            file_size           INTEGER NOT NULL DEFAULT 0,
            file_mtime          INTEGER NOT NULL DEFAULT 0,
            waveform_detail     BLOB,
            waveform_min_detail BLOB,
            detail_num_channels INTEGER DEFAULT 0,
            rms_data            BLOB,
            updated_at          INTEGER DEFAULT 0,
            cache_key           TEXT DEFAULT ''
        );
    )";
    if (!executeSQL(createExternalWaveforms)) return false;

    const char* createWaveformMeta = R"(
        CREATE TABLE IF NOT EXISTS waveforms_db.meta (
            key     TEXT PRIMARY KEY,
            value   TEXT NOT NULL
        );
    )";
    if (!executeSQL(createWaveformMeta)) return false;

    // Forward-compatible schema upgrade for existing sidecar DBs.
    ensureColumn(m_db, "waveforms_db.waveforms", "status", "TEXT DEFAULT 'pending'");
    ensureColumn(m_db, "waveforms_db.waveforms", "error_code", "TEXT DEFAULT ''");
    ensureColumn(m_db, "waveforms_db.waveforms", "retry_count", "INTEGER DEFAULT 0");
    ensureColumn(m_db, "waveforms_db.waveforms", "updated_at", "INTEGER DEFAULT 0");
    ensureColumn(m_db, "waveforms_db.waveforms", "cache_key", "TEXT DEFAULT ''");
    ensureColumn(m_db, "waveforms_db.waveforms", "waveform_min_detail", "BLOB");
    ensureColumn(m_db, "waveforms_db.waveforms", "detail_num_channels", "INTEGER DEFAULT 0");
    ensureColumn(m_db, "waveforms_db.external_waveforms", "waveform_min_detail", "BLOB");
    ensureColumn(m_db, "waveforms_db.external_waveforms", "detail_num_channels", "INTEGER DEFAULT 0");
    executeSQL("CREATE INDEX IF NOT EXISTS waveforms_db.waveforms_idx_status ON waveforms(status);");
    executeSQL("CREATE INDEX IF NOT EXISTS waveforms_db.waveforms_idx_updated_at ON waveforms(updated_at);");
    executeSQL("CREATE INDEX IF NOT EXISTS waveforms_db.waveforms_idx_cache_key ON waveforms(cache_key);");
    executeSQL("CREATE INDEX IF NOT EXISTS waveforms_db.external_waveforms_idx_updated_at ON external_waveforms(updated_at);");

    // Add mipmap storage to waveforms_db.waveforms.
    ensureColumn(m_db, "waveforms_db.waveforms", "mipmap_data", "BLOB");

    // True RMS dBFS over the full interleaved PCM stream, matching ffmpeg astats.
    ensureColumn(m_db, "waveforms_db.waveforms", "true_rms_dbfs", "REAL DEFAULT NULL");
    ensureColumn(m_db, "waveforms_db.waveforms", "mini_num_channels", "INTEGER DEFAULT 1");
    ensureColumn(m_db, "waveforms_db.waveforms", "mini_min_data", "BLOB");

    // Spectral band energies for SpectralPeaks waveform display
    ensureColumn(m_db, "waveforms_db.waveforms", "spectral_data", "BLOB DEFAULT NULL");

    const int currentVersion = ReadWaveformCacheVersion(m_db);
    if (currentVersion != kWaveformCacheVersion) {
        const bool needsMiniLayoutInvalidation = currentVersion < 4;
        DIAG_SYSTEM_FMT("Database",
                        "Waveform cache version upgrade: %d -> %d, invalidating %s",
                        currentVersion, kWaveformCacheVersion,
                        needsMiniLayoutInvalidation ? "mini/detail/rms cache" : "main detail cache");
        if (needsMiniLayoutInvalidation) {
            executeSQL(
                "UPDATE waveforms_db.waveforms "
                "SET waveform_data = NULL, "
                "    mini_min_data = NULL, "
                "    waveform_detail = NULL, "
                "    waveform_min_detail = NULL, "
                "    detail_num_channels = 0, "
                "    rms_data = NULL, "
                "    mini_num_channels = 1, "
                "    status = 'pending', "
                "    error_code = '', "
                "    retry_count = 0, "
                "    updated_at = 0;");
            executeSQL("DELETE FROM waveforms_db.external_waveforms;");
        } else {
            executeSQL(
                "UPDATE waveforms_db.waveforms "
                "SET waveform_detail = NULL, "
                "    waveform_min_detail = NULL, "
                "    detail_num_channels = 0, "
                "    status = 'pending', "
                "    error_code = '', "
                "    retry_count = 0, "
                "    updated_at = 0 "
                "WHERE waveform_detail IS NOT NULL "
                "   OR waveform_min_detail IS NOT NULL "
                "   OR (detail_num_channels IS NOT NULL AND detail_num_channels <> 0);");
            executeSQL(
                "UPDATE waveforms_db.external_waveforms "
                "SET waveform_detail = NULL, "
                "    waveform_min_detail = NULL, "
                "    detail_num_channels = 0 "
                "WHERE waveform_detail IS NOT NULL "
                "   OR waveform_min_detail IS NOT NULL "
                "   OR (detail_num_channels IS NOT NULL AND detail_num_channels <> 0);");
        }
        executeSQL(
            "UPDATE main.audio_files "
            "SET waveform_detail = NULL "
            "WHERE waveform_detail IS NOT NULL;");
        if (!WriteWaveformCacheVersion(m_db, kWaveformCacheVersion)) {
            m_lastError = "failed to persist waveform cache version";
            return false;
        }
    }
    {
        const char* migKey = "waveform_detail_channels_v1";
        if (ReadAppMetaInt(m_db, migKey) != 1) {
            executeSQL(
                "UPDATE waveforms_db.waveforms "
                "SET waveform_detail = NULL, "
                "    waveform_min_detail = NULL, "
                "    detail_num_channels = 0, "
                "    status = 'pending', "
                "    error_code = '', "
                "    retry_count = 0, "
                "    updated_at = 0 "
                "WHERE waveform_detail IS NOT NULL "
                "  AND NOT (waveform_detail IS NOT NULL "
                "       AND length(waveform_detail) > 8 "
                "       AND substr(waveform_detail, 1, 6) = x'0057504B3201' "
                "       AND substr(waveform_detail, 8, 1) = x'01' "
                "       AND detail_num_channels IS NOT NULL "
                "       AND detail_num_channels > 0 "
                "       AND hex(substr(waveform_detail, 7, 1)) = printf('%02X', detail_num_channels) "
                "       AND ((length(waveform_detail) - 8) % 4) = 0 "
                "       AND (((length(waveform_detail) - 8) / 4) % detail_num_channels) = 0) "
                "  AND (waveform_min_detail IS NULL OR length(waveform_min_detail) < length(waveform_detail) "
                "       OR detail_num_channels IS NULL OR detail_num_channels <= 0);");
            executeSQL(
                "UPDATE waveforms_db.external_waveforms "
                "SET waveform_detail = NULL, "
                "    waveform_min_detail = NULL, "
                "    detail_num_channels = 0 "
                "WHERE waveform_detail IS NOT NULL "
                "  AND NOT (waveform_detail IS NOT NULL "
                "       AND length(waveform_detail) > 8 "
                "       AND substr(waveform_detail, 1, 6) = x'0057504B3201' "
                "       AND substr(waveform_detail, 8, 1) = x'01' "
                "       AND detail_num_channels IS NOT NULL "
                "       AND detail_num_channels > 0 "
                "       AND hex(substr(waveform_detail, 7, 1)) = printf('%02X', detail_num_channels) "
                "       AND ((length(waveform_detail) - 8) % 4) = 0 "
                "       AND (((length(waveform_detail) - 8) / 4) % detail_num_channels) = 0) "
                "  AND (waveform_min_detail IS NULL OR length(waveform_min_detail) < length(waveform_detail) "
                "       OR detail_num_channels IS NULL OR detail_num_channels <= 0);");
            WriteAppMetaInt(m_db, migKey, 1);
        }
    }
    
    return true;
}

bool Database::migrateFromSingleDatabase(const std::string& mainDbPath) {
    if (!m_db) return false;
    (void)mainDbPath;

    // Move waveform and RMS BLOBs from audio_files into waveforms_db.
    // Migrate only rows that contain data; this is a one-time operation.
    const char* migrateSql = R"(
        INSERT OR IGNORE INTO waveforms_db.waveforms (file_id, waveform_data, rms_data)
        SELECT id, waveform_data, rms_data
        FROM audio_files
        WHERE waveform_data IS NOT NULL AND length(waveform_data) > 0;
    )";
    executeSQL(migrateSql);

    // Clear migrated BLOB columns from the main database to release space.
    const char* clearSql = R"(
        UPDATE audio_files SET waveform_data = NULL, rms_data = NULL
        WHERE waveform_data IS NOT NULL AND length(waveform_data) > 0;
    )";
    executeSQL(clearSql);

    return true;
}

// Replace string-path joins with ParentID and FolderID integer indexes.

int64_t Database::getOrCreateFolderId(const std::string& folderPath) {
    if (!m_db || folderPath.empty()) return 0;
    
    // Create the directories table one statement at a time for SQLite compatibility.
    // NULL parent_id denotes a root and avoids a foreign-key reference to id zero.
    if (!tableExists(m_db, "directories")) {
        if (!executeSQL(
            "CREATE TABLE IF NOT EXISTS directories ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT, "
            "folder_path TEXT UNIQUE NOT NULL, "
            "parent_id INTEGER, "
            "created_at INTEGER DEFAULT 0, "
            "FOREIGN KEY (parent_id) REFERENCES directories(id) ON DELETE CASCADE)")) return 0;
        if (!executeSQL("CREATE INDEX IF NOT EXISTS idx_dirs_path ON directories(folder_path)")) return 0;
        if (!executeSQL("CREATE INDEX IF NOT EXISTS idx_dirs_parent ON directories(parent_id)")) return 0;
    }
    
    // Try to get existing ID
    const char* selectSql = "SELECT id FROM directories WHERE folder_path = ? LIMIT 1";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, selectSql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, folderPath.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            int64_t id = sqlite3_column_int64(stmt, 0);
            sqlite3_finalize(stmt);
            return id;
        }
    }
    if (stmt) sqlite3_finalize(stmt);
    
    // Create new directory entry
    int64_t parentId = 0;
    try {
        fs::path p = Utf8ToPath(folderPath);
        fs::path parentPath = p.parent_path();
        if (parentPath != p && !parentPath.empty()) {
            std::string parentPathStr = PathToUtf8(parentPath);
            parentId = getOrCreateFolderId(parentPathStr); // Recursive
        }
    } catch (...) {
        parentId = 0;
    }
    
    const char* insertSql = "INSERT INTO directories (folder_path, parent_id, created_at) VALUES (?, ?, ?)";
    stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, insertSql, -1, &stmt, nullptr) != SQLITE_OK) {
        return 0;
    }
    sqlite3_bind_text(stmt, 1, folderPath.c_str(), -1, SQLITE_TRANSIENT);
    if (parentId > 0)
        sqlite3_bind_int64(stmt, 2, parentId);
    else
        sqlite3_bind_null(stmt, 2);
    sqlite3_bind_int64(stmt, 3, getCurrentTimestamp());
    
    if (sqlite3_step(stmt) != SQLITE_DONE) {
        sqlite3_finalize(stmt);
        return 0;
    }
    
    int64_t newId = getLastInsertId();
    sqlite3_finalize(stmt);
    return newId;
}

std::string Database::getFolderPathById(int64_t folderId) {
    if (!m_db || folderId <= 0) return "";
    
    const char* sql = "SELECT folder_path FROM directories WHERE id = ? LIMIT 1";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return "";
    }
    
    sqlite3_bind_int64(stmt, 1, folderId);
    std::string result;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        result = safeGetText(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return result;
}

int64_t Database::getFolderIdByPath(const std::string& folderPath) {
    if (!m_db || folderPath.empty()) return 0;
    
    const char* sql = "SELECT id FROM directories WHERE folder_path = ? LIMIT 1";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return 0;
    }
    
    sqlite3_bind_text(stmt, 1, folderPath.c_str(), -1, SQLITE_TRANSIENT);
    int64_t result = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        result = sqlite3_column_int64(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return result;
}

std::unordered_map<std::string, int64_t> Database::getFolderIdMap(const std::vector<std::string>& folderPaths) {
    std::unordered_map<std::string, int64_t> result;
    if (!m_db || folderPaths.empty()) return result;
    
    const char* sql = "SELECT folder_path, id FROM directories WHERE folder_path IN (";
    std::string sqlWithParams = sql;
    for (size_t i = 0; i < folderPaths.size(); ++i) {
        if (i > 0) sqlWithParams += ",";
        sqlWithParams += "?";
    }
    sqlWithParams += ")";
    
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sqlWithParams.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        return result;
    }
    
    for (size_t i = 0; i < folderPaths.size(); ++i) {
        sqlite3_bind_text(stmt, (int)(i + 1), folderPaths[i].c_str(), -1, SQLITE_TRANSIENT);
    }
    
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        std::string path = safeGetText(stmt, 0);
        int64_t id = sqlite3_column_int64(stmt, 1);
        result[path] = id;
    }
    sqlite3_finalize(stmt);
    return result;
}

int64_t Database::getFolderFileCountById(int64_t folderId) {
    if (!m_db || folderId <= 0) return 0;
    
    // Get folder path first
    std::string folderPath = getFolderPathById(folderId);
    if (folderPath.empty()) return 0;
    
    // Use existing getFolderFileCount with path
    return getFolderFileCount(folderPath);
}

static std::string EncodeSmallCoverFileListCursor(const Database::CoverWallEntry& entry) {
    std::string cursor;
    cursor.reserve(3 + entry.path.size() + entry.key.size());
    cursor.append("fl");
    cursor.push_back('\x1f');
    cursor.append(entry.path);
    cursor.push_back('\x1f');
    cursor.append(entry.key);
    return cursor;
}

static bool MatchesSmallCoverFileListCursor(const Database::CoverWallEntry& entry,
                                           const std::string& afterCursor) {
    return !afterCursor.empty() && EncodeSmallCoverFileListCursor(entry) == afterCursor;
}

Database::CoverWallPage Database::getGlobalCoverWallEntriesPage(const std::string& afterCursor,
                                                                int limit,
                                                                bool followFileListOrder) {
    CoverWallPage page;
    if (!m_db || limit <= 0) return page;

    limit = std::clamp(limit, 1, 50000);
    const int queryLimit = limit + 1;

    const auto readiness = AssessGlobalCoverAggregationReadiness(m_db);
    const bool registrySparse = readiness.registrySparse;
    page.registryEntries = readiness.registryEntries;
    page.largeLibrary = readiness.largeLibrary;

    if (followFileListOrder && page.largeLibrary) {
        // Read in the same natural-name order as the unfiltered file list. A
        // cursor identifies the last emitted audio row, so later pages do not
        // stop at the first 50,000 files or revisit earlier cover groups.
        int64_t cursorId = 0;
        std::string cursorName;
        std::string cursorPath;
        if (!afterCursor.empty()) {
            if (afterCursor.rfind("fl2:", 0) != 0) return page;
            char* end = nullptr;
            cursorId = std::strtoll(afterCursor.c_str() + 4, &end, 10);
            if (cursorId <= 0 || !end || *end != '\0') return page;
            sqlite3_stmt* cursorStmt = nullptr;
            if (sqlite3_prepare_v2(m_db,
                    "SELECT file_name, file_path FROM audio_files WHERE id = ?;",
                    -1, &cursorStmt, nullptr) != SQLITE_OK) return page;
            sqlite3_bind_int64(cursorStmt, 1, cursorId);
            const bool found = sqlite3_step(cursorStmt) == SQLITE_ROW;
            if (found) {
                cursorName = safeGetText(cursorStmt, 0);
                cursorPath = safeGetText(cursorStmt, 1);
            }
            sqlite3_finalize(cursorStmt);
            if (!found) return page;
        }

        // Choose the first file for each cover in one pass. Correlated
        // NOT EXISTS checks repeated a large-library scan for every candidate
        // and could leave the first page empty for tens of seconds.
        std::string sql = R"SQL(
            WITH ranked AS (
                SELECT af.id, af.file_name, af.file_path, af.folder_path,
                       af.embedded_cover_key, af.folder_cover_key,
                       af.folder_cover_src_path,
                       CASE WHEN af.embedded_cover_key IS NOT NULL AND af.embedded_cover_key <> ''
                            THEN af.embedded_cover_key
                            WHEN af.folder_cover_key IS NOT NULL AND af.folder_cover_key <> ''
                            THEN af.folder_cover_key
                            ELSE '?|' || af.folder_path END AS cover_identity,
                       ROW_NUMBER() OVER (
                           PARTITION BY CASE
                               WHEN af.embedded_cover_key IS NOT NULL AND af.embedded_cover_key <> ''
                               THEN af.embedded_cover_key
                               WHEN af.folder_cover_key IS NOT NULL AND af.folder_cover_key <> ''
                               THEN af.folder_cover_key
                               ELSE '?|' || af.folder_path END
                           ORDER BY af.file_name COLLATE AURA_NATURAL, af.file_path, af.id
                       ) AS cover_rank
                FROM audio_files af
                WHERE (af.embedded_cover_key IS NOT NULL AND af.embedded_cover_key <> '') OR
                      (af.folder_cover_key IS NOT NULL AND af.folder_cover_key <> '') OR
                      (af.has_cover = 1 AND af.folder_path IS NOT NULL AND af.folder_path <> '')
            )
            SELECT af.id,
                   COALESCE(NULLIF(r.normalized_key, ''), r.cover_key, af.cover_identity),
                   CASE WHEN af.embedded_cover_key IS NOT NULL AND af.embedded_cover_key <> ''
                        THEN af.file_path ELSE af.folder_path END,
                   CASE WHEN af.embedded_cover_key IS NOT NULL AND af.embedded_cover_key <> ''
                        THEN CASE WHEN r.cover_phash IS NOT NULL AND r.cover_phash <> 0
                                  THEN 'PHASH|' || r.cover_phash ELSE '' END
                        ELSE af.folder_cover_src_path END,
                   CASE WHEN af.embedded_cover_key IS NOT NULL AND af.embedded_cover_key <> ''
                        THEN 1 ELSE 0 END
            FROM ranked af
            LEFT JOIN cover_cache_registry r ON r.cover_key = af.cover_identity
            WHERE af.cover_rank = 1
        )SQL";
        if (cursorId > 0) {
            sql += R"SQL(
                AND (af.file_name COLLATE AURA_NATURAL > ? OR
                     (af.file_name COLLATE AURA_NATURAL = ? AND
                      (af.file_path > ? OR
                       (af.file_path = ? AND af.id > ?))))
            )SQL";
        }
        sql += " ORDER BY af.file_name COLLATE AURA_NATURAL, af.file_path, af.id LIMIT ?;";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(m_db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
            m_lastError = sqlite3_errmsg(m_db);
            return page;
        }
        int bind = 1;
        if (cursorId > 0) {
            sqlite3_bind_text(stmt, bind++, cursorName.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, bind++, cursorName.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, bind++, cursorPath.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, bind++, cursorPath.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(stmt, bind++, cursorId);
        }
        sqlite3_bind_int(stmt, bind, queryLimit);
        int64_t lastEmittedId = 0;
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            if (page.entries.size() >= static_cast<size_t>(limit)) {
                page.hasMore = true;
                break;
            }
            CoverWallEntry entry;
            entry.key = safeGetText(stmt, 1);
            entry.path = safeGetText(stmt, 2);
            entry.sourcePath = safeGetText(stmt, 3);
            entry.isEmbedded = sqlite3_column_int(stmt, 4) != 0;
            if (entry.key.empty() || entry.path.empty()) continue;
            lastEmittedId = sqlite3_column_int64(stmt, 0);
            page.entries.push_back(std::move(entry));
        }
        sqlite3_finalize(stmt);
        if (page.hasMore) page.nextCursor = "fl2:" + std::to_string(lastEmittedId);
        PrewarmCoverTileMembershipForWallEntries(this, page.entries);
        return page;
    }

    if (page.largeLibrary && registrySparse) {
        if (page.registryEntries == 0) {
            DIAG_ACTION_FMT("Database",
                            "GlobalCoverWallPageLargeLibraryEmptyRegistryFallback: marker=%d",
                            readiness.largeLibraryMarkerPending ? 1 : 0);
        } else {
            DIAG_ACTION_FMT("Database",
                            "GlobalCoverWallPageLargeLibrarySparseMergePlanned: registry=%d marker=%d cursor=%d",
                            page.registryEntries,
                            readiness.largeLibraryMarkerPending ? 1 : 0,
                            afterCursor.empty() ? 0 : 1);
        }
    } else if (registrySparse) {
        DIAG_ACTION_FMT("Database",
                        "GlobalCoverWallPageRegistrySparseFallback: registry=%d raw=%d",
                        page.registryEntries,
                        readiness.rawCoverPresenceCandidates);
    }

    auto assignEntriesFromRawRows = [&](const std::vector<GlobalCoverWallRawRow>& rawRows) {
        std::vector<CoverFacetCandidate> raw;
        raw.reserve(rawRows.size());
        for (const auto& row : rawRows) raw.push_back(row.candidate);
        auto clustered = ClusterCoverCandidates(raw, limit, false);
        page.entries.clear();
        page.entries.reserve(clustered.size());
        for (const auto& c : clustered) {
            CoverWallEntry e;
            e.key = c.key;
            e.path = c.path;
            e.sourcePath = c.sourcePath;
            e.isEmbedded = c.isEmbedded;
            page.entries.push_back(std::move(e));
        }
    };

    auto pageFromCurrentFileList = [&]() -> bool {
        if (page.largeLibrary || readiness.totalFiles >= kGlobalCoverWallFallbackMaxFiles) return false;
        SearchFilter fileListFilter;
        fileListFilter.sortColumn = 0;
        fileListFilter.sortAscending = true;
        auto records = searchFiles(fileListFilter, 50000, 0);
        if (records.empty()) return false;
        std::stable_sort(records.begin(), records.end(), [](const AudioRecord& lhs, const AudioRecord& rhs) {
            const int nameCmp = CompareNaturalUtf8(lhs.fileName, rhs.fileName);
            if (nameCmp != 0) return nameCmp < 0;
            if (lhs.filePath != rhs.filePath) return lhs.filePath < rhs.filePath;
            return lhs.id < rhs.id;
        });

        std::vector<CoverWallEntry> ordered;
        ordered.reserve(records.size());
        std::unordered_set<std::string> seenKeys;
        seenKeys.reserve(records.size());
        for (const auto& record : records) {
            CoverWallEntry entry;
            if (!record.embeddedCoverKey.empty()) {
                entry.key = record.embeddedCoverKey;
                entry.path = record.filePath;
                entry.isEmbedded = true;
            } else if (!record.folderCoverKey.empty()) {
                entry.key = record.folderCoverKey;
                entry.path = record.folderPath;
                entry.sourcePath = record.folderCoverSrcPath;
                entry.isEmbedded = false;
            } else if (record.hasCover && !record.folderPath.empty()) {
                entry.key = std::string("?|") + record.folderPath;
                entry.path = record.folderPath;
                entry.sourcePath = record.folderCoverSrcPath;
                entry.isEmbedded = false;
            } else {
                continue;
            }
            if (entry.key.empty() || entry.path.empty() || !seenKeys.insert(entry.key).second) continue;
            ordered.push_back(std::move(entry));
        }
        if (ordered.empty()) return false;

        size_t start = 0;
        if (!afterCursor.empty()) {
            const auto cursor = std::find_if(
                ordered.begin(),
                ordered.end(),
                [&](const CoverWallEntry& entry) {
                    return MatchesSmallCoverFileListCursor(entry, afterCursor);
                });
            if (cursor == ordered.end()) return false;
            start = static_cast<size_t>(std::distance(ordered.begin(), cursor)) + 1;
        }
        const size_t remaining = ordered.size() - start;
        const size_t take = (std::min)(remaining, static_cast<size_t>(limit));
        page.entries.assign(ordered.begin() + static_cast<std::ptrdiff_t>(start),
                            ordered.begin() + static_cast<std::ptrdiff_t>(start + take));
        page.hasMore = start + take < ordered.size();
        page.nextCursor = page.hasMore && !page.entries.empty()
            ? EncodeSmallCoverFileListCursor(page.entries.back())
            : std::string();
        page.fromRegistry = false;
        return true;
    };

    std::vector<GlobalCoverWallRawRow> registryRows;
    if (readiness.useRegistry) {
        registryRows = LoadGlobalCoverWallRegistryRows(
            m_db,
            page.largeLibrary ? afterCursor : std::string(),
            page.largeLibrary ? queryLimit : 50000);
        page.fromRegistry = true;
    }

    const bool useSparseLargeLibraryMerge =
        page.largeLibrary && registrySparse;
    if (useSparseLargeLibraryMerge) {
        if (readiness.useRegistry) {
            registryRows = LoadGlobalCoverWallRegistryRows(m_db, afterCursor, queryLimit);
        }
        auto supplementalRows =
            LoadLargeLibrarySparseGlobalCoverWallRows(m_db, afterCursor, queryLimit);
        int supplementalConsumed = 0;
        auto mergedRows =
            MergeGlobalCoverWallRowsForPage(
                registryRows,
                supplementalRows,
                limit,
                &page.hasMore,
                &page.nextCursor,
                &supplementalConsumed);
        assignEntriesFromRawRows(mergedRows);
        DIAG_ACTION_FMT("Database",
                        "GlobalCoverWallPageLargeLibrarySparseMerge: registry=%d marker=%d supplement=%d rows=%d hasMore=%d cursor=%d",
                        page.registryEntries,
                        readiness.largeLibraryMarkerPending ? 1 : 0,
                        supplementalConsumed,
                        (int)page.entries.size(),
                        page.hasMore ? 1 : 0,
                        afterCursor.empty() ? 0 : 1);
        return page;
    }

    if (!readiness.useRegistry && registrySparse) {
        // Small-library sparse registry mode must fall straight to the raw
        // fallback below so wall/facet/page all share the same readiness owner.
        registryRows.clear();
        page.fromRegistry = false;
    }

    if (pageFromCurrentFileList()) {
        PrewarmCoverTileMembershipForWallEntries(this, page.entries);
        return page;
    }

    if (page.fromRegistry) {
        const bool registryHasMore = registryRows.size() > (size_t)limit;
        if (registryHasMore) {
            registryRows.resize((size_t)limit);
        }
        assignEntriesFromRawRows(registryRows);
        page.hasMore = registryHasMore;
        page.nextCursor =
            (page.hasMore && !registryRows.empty()) ? registryRows.back().cursorKey : std::string();
    }

    if (!page.entries.empty() || page.fromRegistry) {
        PrewarmCoverTileMembershipForWallEntries(this, page.entries);
        return page;
    }

    if (readiness.totalFiles > kGlobalCoverWallFallbackMaxFiles) {
        return page;
    }

    const char* sqlFallback = R"SQL(
        WITH dedup AS (
            SELECT
                embedded_cover_key AS cover_key,
                MIN(file_path)     AS rep_path,
                ''                 AS source_path,
                1                  AS is_embedded
            FROM audio_files
            WHERE embedded_cover_key IS NOT NULL AND embedded_cover_key <> ''
            GROUP BY embedded_cover_key

            UNION ALL

            SELECT
                folder_cover_key AS cover_key,
                MIN(folder_path) AS rep_path,
                MIN(NULLIF(folder_cover_src_path, '')) AS source_path,
                0                AS is_embedded
            FROM audio_files
            WHERE folder_cover_key IS NOT NULL AND folder_cover_key <> ''
            GROUP BY folder_cover_key

            UNION ALL

            SELECT
                '?|' || folder_path AS cover_key,
                MIN(folder_path) AS rep_path,
                MIN(NULLIF(folder_cover_src_path, '')) AS source_path,
                0                AS is_embedded
            FROM audio_files
            WHERE has_cover = 1
              AND (embedded_cover_key IS NULL OR embedded_cover_key = '')
              AND (folder_cover_key IS NULL OR folder_cover_key = '')
              AND folder_path IS NOT NULL AND folder_path <> ''
            GROUP BY folder_path
        )
        SELECT cover_key, rep_path, source_path, is_embedded
        FROM dedup
        WHERE (? = '' OR cover_key > ?)
        ORDER BY cover_key
        LIMIT ?;
    )SQL";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sqlFallback, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, afterCursor.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, afterCursor.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 3, queryLimit);

        int consumed = 0;
        std::string lastConsumedCursor;
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const std::string rawCursor = safeGetText(stmt, 0);
            if (consumed >= limit) {
                page.hasMore = true;
                break;
            }
            CoverWallEntry e;
            e.key = rawCursor;
            e.path = safeGetText(stmt, 1);
            e.sourcePath = safeGetText(stmt, 2);
            e.isEmbedded = sqlite3_column_int(stmt, 3) != 0;
            if (!e.key.empty() && !e.path.empty()) {
                page.entries.push_back(std::move(e));
                lastConsumedCursor = rawCursor;
                ++consumed;
            }
        }
        sqlite3_finalize(stmt);
        page.nextCursor = page.hasMore ? lastConsumedCursor : std::string();
    }

    if (!page.entries.empty()) {
        DIAG_ACTION_FMT("Database",
                        "Global cover wall page served from small-library fallback; rows=%d more=%d",
                        (int)page.entries.size(),
                        page.hasMore ? 1 : 0);
    }
    PrewarmCoverTileMembershipForWallEntries(this, page.entries);
    return page;
}

std::vector<Database::CoverWallEntry> Database::getGlobalCoverWallEntries() {
    std::vector<CoverWallEntry> results;
    if (!m_db) return results;

    const auto readiness = AssessGlobalCoverAggregationReadiness(m_db);
    if (readiness.registryEntries == 0) {
        DIAG_ACTION("Database", "Global cover registry empty; query uses read-only fallback when safe.");
    }

    if (readiness.registrySparse) {
        if (readiness.largeLibrary) {
            if (readiness.registryEntries == 0) {
                DIAG_ACTION_FMT("Database",
                                "GlobalCoverWallLargeLibraryEmptyRegistryFallback: marker=%d",
                                readiness.largeLibraryMarkerPending ? 1 : 0);
            } else {
                DIAG_ACTION_FMT("Database",
                                "GlobalCoverWallLargeLibraryIncrementalOnly: registry=%d marker=%d",
                                readiness.registryEntries,
                                readiness.largeLibraryMarkerPending ? 1 : 0);
            }
        } else {
            DIAG_ACTION_FMT("Database",
                            "GlobalCoverWallRegistrySparseFallback: registry=%d raw=%d",
                            readiness.registryEntries,
                            readiness.rawCoverPresenceCandidates);
        }
    }

    if (readiness.useRegistry) {
        // Keep no-filter wall truth on the same live representative validation as
        // the paged path. The shared helper SQL pins cover-key lookups to
        // INDEXED BY idx_files_cover_dedup_emb / INDEXED BY idx_files_cover_dedup_fold
        // so large-library browse never drifts back to an unbounded table scan.
        results = BuildCoverWallEntriesFromCandidates(
            LoadGlobalRegistryCoverCandidates(m_db, 50000),
            50000);
    }

    // If the registry is empty, large libraries wait for background sync; small libraries rebuild inline.
    if (results.empty()) {
        int64_t totalFiles = readiness.totalFiles >= 0 ? readiness.totalFiles : getFileCount();
        if (totalFiles > kGlobalCoverWallFallbackMaxFiles) {
            DIAG_ACTION("Database", "Global cover wall GROUP BY bypassed for large library. Waiting for background sync.");
            return results;
        }
        // Small-library fallback: rebuild keys from audio_files and populate the registry.
        const char* sqlFallback = R"(
            WITH dedup AS (
                SELECT
                    embedded_cover_key AS cover_key,
                    MIN(file_path)     AS rep_path,
                    ''                 AS source_path,
                    1                  AS is_embedded
                FROM audio_files
                WHERE embedded_cover_key IS NOT NULL AND embedded_cover_key <> ''
                GROUP BY embedded_cover_key

                UNION ALL

                SELECT
                    folder_cover_key AS cover_key,
                    MIN(folder_path) AS rep_path,
                    MIN(NULLIF(folder_cover_src_path, '')) AS source_path,
                    0                AS is_embedded
                FROM audio_files
                WHERE folder_cover_key IS NOT NULL AND folder_cover_key <> ''
                GROUP BY folder_cover_key

                UNION ALL

                SELECT
                    '?|' || folder_path AS cover_key,
                    MIN(folder_path) AS rep_path,
                    MIN(NULLIF(folder_cover_src_path, '')) AS source_path,
                    0                AS is_embedded
                FROM audio_files
                WHERE has_cover = 1
                  AND (embedded_cover_key IS NULL OR embedded_cover_key = '')
                  AND (folder_cover_key IS NULL OR folder_cover_key = '')
                  AND folder_path IS NOT NULL AND folder_path <> ''
                GROUP BY folder_path
            )
            SELECT cover_key, rep_path, source_path, is_embedded
            FROM dedup
            ORDER BY is_embedded DESC, cover_key;
        )";

        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(m_db, sqlFallback, -1, &stmt, nullptr) == SQLITE_OK) {
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                CoverWallEntry e;
                e.key = safeGetText(stmt, 0);
                e.path = safeGetText(stmt, 1);
                e.sourcePath = safeGetText(stmt, 2);
                e.isEmbedded = sqlite3_column_int(stmt, 3) != 0;
                if (!e.key.empty() && !e.path.empty()) {
                    results.push_back(std::move(e));
                }
            }
            sqlite3_finalize(stmt);
        }

        if (!results.empty()) {
            DIAG_ACTION("Database", "Global cover wall served from read-only small-library fallback; registry maintenance deferred.");
        }
    }

    PrewarmCoverTileMembershipForWallEntries(this, results);
    return results;
}

std::vector<int64_t> Database::getFileIdsForCoverTileMembership(const std::string& tileIdentityKey) {
    std::vector<int64_t> out;
    if (!m_db || tileIdentityKey.empty()) return out;
    ExactCoverWallSelectionMembers exactMembers;
    if (ParseExactCoverWallSelection(tileIdentityKey, exactMembers)) {
        ExpandExactCoverWallRegistryAliases(m_db, exactMembers);
        if (!exactMembers.keys.empty()) {
            EnsureCoverTileMembershipForKeys(m_db, exactMembers.keys, &out);
        }
        if (!exactMembers.temporaryFolders.empty()) {
            const char* sql =
                "SELECT id FROM audio_files WHERE has_cover = 1 "
                "AND (embedded_cover_key IS NULL OR embedded_cover_key = '') "
                "AND (folder_cover_key IS NULL OR folder_cover_key = '') "
                "AND lower(replace(folder_path, '/', '\\')) "
                "IN (SELECT value FROM json_each(?))";
            sqlite3_stmt* stmt = nullptr;
            if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
                const std::string paths = CoverWallSelectionJsonArray(exactMembers.temporaryFolders);
                sqlite3_bind_text(stmt, 1, paths.c_str(), (int)paths.size(), SQLITE_TRANSIENT);
                while (sqlite3_step(stmt) == SQLITE_ROW) out.push_back(sqlite3_column_int64(stmt, 0));
            }
            if (stmt) sqlite3_finalize(stmt);
        }
        std::sort(out.begin(), out.end());
        out.erase(std::unique(out.begin(), out.end()), out.end());
        return out;
    }
    const std::string lookupKey = CanonicalizeCoverLookupKey(tileIdentityKey);
    std::string temporaryFolderPath;
    if (TryExtractTemporaryFolderIdentityPath(lookupKey, &temporaryFolderPath)) {
        const char* sqlTemporary =
            "SELECT id FROM audio_files "
            "WHERE has_cover = 1 "
            "  AND (embedded_cover_key IS NULL OR embedded_cover_key = '') "
            "  AND (folder_cover_key IS NULL OR folder_cover_key = '') "
            "  AND folder_path IS NOT NULL "
            "  AND folder_path <> '' "
            "  AND lower(replace(folder_path, '/', '\\')) = ? "
            "ORDER BY id";
        sqlite3_stmt* tempStmt = nullptr;
        if (sqlite3_prepare_v2(m_db, sqlTemporary, -1, &tempStmt, nullptr) != SQLITE_OK) {
            m_lastError = sqlite3_errmsg(m_db);
            return out;
        }
        sqlite3_bind_text(tempStmt, 1, temporaryFolderPath.c_str(), (int)temporaryFolderPath.size(), SQLITE_TRANSIENT);
        while (sqlite3_step(tempStmt) == SQLITE_ROW) {
            out.push_back(sqlite3_column_int64(tempStmt, 0));
        }
        sqlite3_finalize(tempStmt);
        return out;
    }

    std::vector<std::string> equivalentKeys;
    if (!getPreparedCoverFilterKeys(lookupKey, equivalentKeys)) {
        equivalentKeys = ResolveEquivalentCoverKeys(this, lookupKey);
    }
    if (equivalentKeys.empty()) equivalentKeys.push_back(lookupKey);

    constexpr size_t kMaxTileMembershipKeys = 900;
    if (equivalentKeys.size() > kMaxTileMembershipKeys) {
        equivalentKeys.resize(kMaxTileMembershipKeys);
    }

    setPreparedCoverFilterKeys(lookupKey, equivalentKeys);
    setPreparedCoverFilterKeys(tileIdentityKey, equivalentKeys);
    for (const auto& key : equivalentKeys) {
        setPreparedCoverFilterKeys(key, equivalentKeys);
    }

    if (EnsureCoverTileMembershipForKeys(m_db, equivalentKeys, &out)) return out;

    DIAG_ACTION("Database", "Tile membership unresolved after canonical lookup/self-heal; legacy field fallback fully retired.");
    return out;
}

std::vector<Database::CoverFacetEntry> Database::getCoverFacets(const SearchFilter& filter, int limit) {
    DIAG_SCOPED("Database.getCoverFacets");
    std::vector<CoverFacetEntry> results;
    if (!m_db || limit <= 0) return results;

    bool isGlobal = filter.keyword.empty() && filter.folderPath.empty() &&
                    filter.artist.empty() && filter.album.empty() &&
                    filter.category.empty() &&
                    filter.specificCoverKey.empty();

    const std::string persistentCacheKey = BuildPersistentFacetCacheKey(
        filter, limit, filter.folderPath.empty() ? "cover_cluster_v5" : "cover_folder_distinct_v1");
    bool registrySparse = false;
    auto buildRegistryFacetResults = [&](int candidateLimit) {
        return BuildCoverFacetEntriesFromCandidates(
            LoadGlobalRegistryCoverCandidates(m_db, candidateLimit),
            limit);
    };
    if (isGlobal) {
        const auto readiness = AssessGlobalCoverAggregationReadiness(m_db);
        if (readiness.registryEntries == 0) {
            DIAG_ACTION("Database", "Global cover facet registry empty; query uses existing incremental registry only.");
        }

        registrySparse = readiness.registrySparse;

        const int candidateLimit = (std::min)(50000, (std::max)(limit * 8, limit));
        if (!registrySparse) {
            // Fast startup path: the cover registry is already the small shadow
            // table for the cover wall. Do not join it with a fresh GROUP BY over
            // audio_files here; exact usage counts can be rebuilt by maintenance,
            // but startup/search must stay bounded.
            results = buildRegistryFacetResults(candidateLimit);

            DIAG_ACTION_FMT("Search", "GlobalCoverFacetRegistryFastPath: rows=%d", (int)results.size());
            if (ShouldPersistFacetEntriesForFilter(filter, (int)results.size())) {
                auto payload = SerializeCoverFacetEntries(results);
                saveFacetQueryCache(persistentCacheKey, payload, (int)results.size());
            }
            return results;
        }

        if (readiness.largeLibrary) {
            results = buildRegistryFacetResults(candidateLimit);
            if (readiness.registryEntries == 0) {
                DIAG_ACTION_FMT("Search",
                                "GlobalCoverFacetLargeLibraryEmptyRegistryFallback: marker=%d rows=%d",
                                readiness.largeLibraryMarkerPending ? 1 : 0,
                                (int)results.size());
            } else {
                DIAG_ACTION_FMT("Search",
                                "GlobalCoverFacetRegistryIncrementalOnly: registry=%d marker=%d rows=%d",
                                readiness.registryEntries,
                                readiness.largeLibraryMarkerPending ? 1 : 0,
                                (int)results.size());
            }
            return results;
        }

        DIAG_ACTION_FMT("Search",
                        "GlobalCoverFacetRegistrySparseFallback: registry=%d raw=%d min=%d",
                        readiness.registryEntries,
                        readiness.rawCoverPresenceCandidates,
                        readiness.minimumExpectedRegistry);
    }

    if (!registrySparse) {
        std::vector<uint8_t> payload;
        if (loadFacetQueryCache(persistentCacheKey, payload)) {
            if (DeserializeCoverFacetEntries(payload, results)) {
                DIAG_ACTION_FMT("Search", "CoverFacetPersistentCacheHit: rows=%d", (int)results.size());
                return results;
            }
        }
    } else {
        DIAG_ACTION("Search", "CoverFacetPersistentCacheBypass: registry sparse");
    }

    if (IsPureFolderFacetFilter(filter)) {
        int64_t count = getFolderFileCount(filter.folderPath);
        if (count > 40000) {
            std::string upper = MakeFolderPathUpperBound(filter.folderPath);
            std::string sql =
                "SELECT CASE WHEN normalized_key IS NOT NULL AND normalized_key != '' THEN normalized_key ELSE cover_key END, "
                "       rep_path, "
                "       CASE WHEN is_embedded = 0 THEN COALESCE(( "
                "           SELECT af.folder_cover_src_path "
                "           FROM audio_files af INDEXED BY idx_files_cover_dedup_fold "
                "           WHERE af.folder_cover_key = cover_cache_registry.cover_key "
                "             AND af.folder_cover_key <> '' "
                "             AND af.folder_cover_src_path IS NOT NULL "
                "             AND af.folder_cover_src_path <> '' "
                "           ORDER BY af.id LIMIT 1), '') "
                "            WHEN cover_phash != 0 THEN 'PHASH|' || cover_phash "
                "            ELSE '' END, "
                "       is_embedded, 1, cover_phash "
                "FROM cover_cache_registry "
                "WHERE rep_path >= ? AND rep_path < ? "
                "LIMIT " + std::to_string((std::min)(50000, (std::max)(limit * 8, limit)));
            sqlite3_stmt* stmt = nullptr;
            if (sqlite3_prepare_v2(m_db, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
                sqlite3_bind_text(stmt, 1, filter.folderPath.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(stmt, 2, upper.c_str(), -1, SQLITE_TRANSIENT);
                std::vector<CoverFacetCandidate> raw;
                raw.reserve((size_t)limit);
                while (sqlite3_step(stmt) == SQLITE_ROW) {
                    CoverFacetCandidate c;
                    c.key = safeGetText(stmt, 0);
                    c.path = safeGetText(stmt, 1);
                    c.sourcePath = safeGetText(stmt, 2);
                    c.isEmbedded = sqlite3_column_int(stmt, 3) != 0;
                    c.count = sqlite3_column_int64(stmt, 4);
                    c.phash = sqlite3_column_int64(stmt, 5);
                    if (!c.key.empty() && !c.path.empty()) raw.push_back(std::move(c));
                }
                sqlite3_finalize(stmt);

                // A selected path shows each distinct cover key, even when
                // different artwork files happen to look nearly identical.
                auto clustered = ClusterCoverCandidates(raw, limit, true, false);
                results.reserve(clustered.size());
                for (const auto& c : clustered) {
                    CoverFacetEntry e;
                    e.key = c.key;
                    e.path = c.path;
                    e.sourcePath = c.sourcePath;
                    e.isEmbedded = c.isEmbedded;
                    e.count = c.count;
                    results.push_back(std::move(e));
                }
            }

            if (ShouldPersistFacetEntriesForFilter(filter, (int)results.size())) {
                auto payload = SerializeCoverFacetEntries(results);
                saveFacetQueryCache(persistentCacheKey, payload, (int)results.size());
            }
            return results;
        }

        // Do not use the materialized cover-facet index here: it stores only exact
        // phash/key groups and cannot merge near-identical embedded/folder covers.
        // The dynamic path below keeps folder filtering correct while registry
        // clustering is the source of truth for visual dedupe.
    }

    BuiltSearchQuery bq = buildSearchWhere(filter);
    if (bq.parseError) return results;

    const bool useIndexedFrom = (bq.fromClause.find("JOIN") == std::string::npos &&
                                 !filter.folderPath.empty());

    const std::string fromPart = useIndexedFrom
          ? std::string("FROM audio_files af INDEXED BY idx_files_folder_name ")
          : bq.fromClause + " ";

    const std::string coverInClause = bq.coverKeySubquery.empty() ? std::string()
        : " AND af.id IN (" + bq.coverKeySubquery + ")";
    // No LIMIT is needed: inner GROUP BY collapses rows to distinct cover keys.
    // to distinct covers (hundreds, not tens-of-thousands), keeping the
    // phash correlated lookup fast while returning ALL matching covers.
    //
    // Materialize matched rows once so both cover branches reuse the same result.
    // The previous UNION embedded the same subquery twice and scanned it twice.
    // This preserves is_emb semantics while sharing one result set.
    std::string subSelect = "SELECT af.file_path, af.folder_path, af.embedded_cover_key, af.folder_cover_key, af.folder_cover_src_path, af.has_cover "
                            + fromPart + bq.whereClause + coverInClause;

    // Deduplicate embedded and folder keys together by perceptual hash or canonical key.
    // Merge embedded and folder artwork in one visual group, preferring the folder representative.
    // Pre-group each branch by key to limit perceptual-hash lookups.
    const int candidateLimit = (std::min)(50000, (std::max)(limit * 8, limit));
    std::string sql =
          "WITH matched AS MATERIALIZED (" + subSelect + ") "
          "SELECT key, rep_p, source_path, is_emb, hit_count, phash FROM ("
          "  SELECT embedded_cover_key AS key, MIN(file_path) AS rep_p, '' AS source_path, 1 AS is_emb, COUNT(*) AS hit_count, "
          "    COALESCE((SELECT cover_phash FROM cover_cache_registry WHERE cover_key = embedded_cover_key), 0) AS phash "
          "  FROM matched WHERE embedded_cover_key IS NOT NULL AND embedded_cover_key <> '' "
          "  GROUP BY embedded_cover_key "
          "  UNION ALL "
          "  SELECT folder_cover_key AS key, MIN(folder_path) AS rep_p, MIN(NULLIF(folder_cover_src_path, '')) AS source_path, 0 AS is_emb, COUNT(*) AS hit_count, "
          "    COALESCE((SELECT cover_phash FROM cover_cache_registry WHERE cover_key = folder_cover_key), 0) AS phash "
          "  FROM matched WHERE folder_cover_key IS NOT NULL AND folder_cover_key <> '' "
          "  GROUP BY folder_cover_key "
          "  UNION ALL "
          "  SELECT '?|' || folder_path AS key, MIN(folder_path) AS rep_p, MIN(NULLIF(folder_cover_src_path, '')) AS source_path, 0 AS is_emb, COUNT(*) AS hit_count, 0 AS phash "
          "  FROM matched "
          "  WHERE has_cover = 1 "
          "    AND (embedded_cover_key IS NULL OR embedded_cover_key = '') "
          "    AND (folder_cover_key IS NULL OR folder_cover_key = '') "
          "    AND folder_path IS NOT NULL AND folder_path <> '' "
          "  GROUP BY folder_path "
          ") ORDER BY hit_count DESC, key LIMIT " + std::to_string(candidateLimit);

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        return results;
    }

    // CTE 版：subSelect 只出现一次，参数只绑定一次（旧版 BindSearchQueryRepeated 绑两次）
    BindSearchQuery(stmt, bq);
    if (!bq.coverKeyBindValues.empty()) {
        int nextBind = (int)bq.bindValues.size() + 1;
        for (const auto& val : bq.coverKeyBindValues) {
            sqlite3_bind_text(stmt, nextBind++, val.c_str(), -1, SQLITE_TRANSIENT);
        }
    }

    std::vector<CoverFacetCandidate> raw;
    raw.reserve((size_t)(std::min)(candidateLimit, 2048));
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        CoverFacetCandidate c;
        c.key = safeGetText(stmt, 0);
        c.path = safeGetText(stmt, 1);
        c.sourcePath = safeGetText(stmt, 2);
        c.isEmbedded = sqlite3_column_int(stmt, 3) != 0;
        c.count = sqlite3_column_int64(stmt, 4);
        c.phash = sqlite3_column_int64(stmt, 5);
        if (!c.isEmbedded && c.key.rfind("?|", 0) != 0) {
            std::string canonicalKey = CoverModule::CanonicalCoverKey(c.key);
            if (!canonicalKey.empty()) c.key = canonicalKey;
        }
        if (!c.key.empty() && !c.path.empty()) raw.push_back(std::move(c));
    }
    sqlite3_finalize(stmt);

    // Keep visual grouping for non-path searches; a selected path keeps
    // distinct artwork keys while still collapsing repeated use of one key.
    auto clustered = ClusterCoverCandidates(raw, limit, true, filter.folderPath.empty());
    results.reserve(clustered.size());
    for (const auto& c : clustered) {
        CoverFacetEntry e;
        e.key = c.key;
        e.path = c.path;
        e.sourcePath = c.sourcePath;
        e.isEmbedded = c.isEmbedded;
        e.count = c.count;
        results.push_back(std::move(e));
    }

    if (ShouldPersistFacetEntriesForFilter(filter, (int)results.size())) {
        auto payload = SerializeCoverFacetEntries(results);
        saveFacetQueryCache(persistentCacheKey, payload, (int)results.size());
    }

    return results;
}

std::vector<Database::PathFacetEntry> Database::getPathFacets(const SearchFilter& filter, int limit) {
    DIAG_SCOPED("Database.getPathFacets");
    std::vector<PathFacetEntry> results;
    if (!m_db || limit <= 0) return results;

    const std::string persistentCacheKey = BuildPersistentFacetCacheKey(filter, limit, "path");
    {
        std::vector<uint8_t> payload;
        if (loadFacetQueryCache(persistentCacheKey, payload)) {
            if (DeserializePathFacetEntries(payload, results)) {
                DIAG_ACTION_FMT("Search", "PathFacetPersistentCacheHit: rows=%d", (int)results.size());
                return results;
            }
        }
    }

    if (IsPureFolderFacetFilter(filter)) {
        sqlite3_stmt* stmt = nullptr;
        const bool hasScope = !filter.folderPath.empty();
        const std::string upper = hasScope ? MakeFolderPathUpperBound(filter.folderPath) : std::string();
        const char* sqlGlobal =
            "SELECT folder_path, file_count "
            "FROM folder_index "
            "WHERE file_count > 0 AND folder_path IS NOT NULL AND folder_path <> '' "
            "ORDER BY file_count DESC, folder_path "
            "LIMIT ?;";
        const char* sqlScoped =
            "SELECT folder_path, file_count "
            "FROM folder_index "
            "WHERE folder_path >= ? AND folder_path < ? "
            "  AND file_count > 0 AND folder_path IS NOT NULL AND folder_path <> '' "
            "ORDER BY file_count DESC, folder_path "
            "LIMIT ?;";
        const char* sql = hasScope ? sqlScoped : sqlGlobal;
        if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
            int bind = 1;
            if (hasScope) {
                sqlite3_bind_text(stmt, bind++, filter.folderPath.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(stmt, bind++, upper.c_str(), -1, SQLITE_TRANSIENT);
            }
            sqlite3_bind_int(stmt, bind, limit);
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                PathFacetEntry e;
                e.folderPath = safeGetText(stmt, 0);
                e.count = sqlite3_column_int64(stmt, 1);
                results.push_back(std::move(e));
            }
            sqlite3_finalize(stmt);
        }
        if (!results.empty()) {
            DIAG_ACTION_FMT("Search", "PathFacetFolderIndexFastPath: rows=%d", (int)results.size());
            if (ShouldPersistFacetEntriesForFilter(filter, (int)results.size())) {
                auto payload = SerializePathFacetEntries(results);
                saveFacetQueryCache(persistentCacheKey, payload, (int)results.size());
            }
            return results;
        }

        if (tryGetMaterializedFolderPathFacets(filter, limit, results)) {
            if (ShouldPersistFacetEntriesForFilter(filter, (int)results.size())) {
                auto payload = SerializePathFacetEntries(results);
                saveFacetQueryCache(persistentCacheKey, payload, (int)results.size());
            }
            return results;
        }
    }

    BuiltSearchQuery bq = buildSearchWhere(filter);
    if (bq.parseError) return results;

    bool isNarrowFilter = !filter.artist.empty() || !filter.artists.empty() ||
                          !filter.album.empty() || !filter.category.empty() ||
                          !filter.musicalKey.empty();

    std::string limitClause = isNarrowFilter ? "" : " LIMIT 30000";

    const bool useIndexedFrom = (bq.fromClause.find("JOIN") == std::string::npos &&
                                 !filter.folderPath.empty() && !isNarrowFilter);

    const std::string fromPart = useIndexedFrom
          ? std::string("FROM audio_files af INDEXED BY idx_files_folder_name ")
          : bq.fromClause + " ";

    const std::string coverInClause2 = bq.coverKeySubquery.empty() ? std::string()
        : " AND af.id IN (" + bq.coverKeySubquery + ")";
    std::string sql;
    if (!isNarrowFilter) {
        sql = "SELECT folder_path, COUNT(*) AS hit_count "
              "FROM (SELECT af.folder_path AS folder_path "
              + fromPart + bq.whereClause + coverInClause2 +
              " AND af.folder_path IS NOT NULL AND af.folder_path <> '' "
              + limitClause + ") matched "
              "GROUP BY folder_path "
              "ORDER BY hit_count DESC, folder_path "
              "LIMIT " + std::to_string(limit);
    } else {
        sql = "SELECT folder_path, COUNT(*) AS hit_count "
              + fromPart + bq.whereClause + coverInClause2 +
              " AND af.folder_path IS NOT NULL AND af.folder_path <> '' "
              "GROUP BY af.folder_path "
              "ORDER BY hit_count DESC, af.folder_path "
              "LIMIT " + std::to_string(limit);
    }

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        return results;
    }

    BindSearchQuery(stmt, bq);
    {
        int nextBind = (int)bq.bindValues.size() + 1;
        for (const auto& val : bq.coverKeyBindValues) {
            sqlite3_bind_text(stmt, nextBind++, val.c_str(), -1, SQLITE_TRANSIENT);
        }
    }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        PathFacetEntry e;
        e.folderPath = safeGetText(stmt, 0);
        e.count = sqlite3_column_int64(stmt, 1);
        results.push_back(std::move(e));
    }
    sqlite3_finalize(stmt);

    if (ShouldPersistFacetEntriesForFilter(filter, (int)results.size())) {
        auto payload = SerializePathFacetEntries(results);
        saveFacetQueryCache(persistentCacheKey, payload, (int)results.size());
    }

    return results;
}

bool Database::persistCoverFacetCache(const SearchFilter& filter,
                                      int limit,
                                      const std::vector<CoverFacetEntry>& entries) {
    if (!ShouldPersistFacetEntriesForFilter(filter, (int)entries.size())) return false;
    const std::string persistentCacheKey =
        BuildPersistentFacetCacheKey(
            filter, limit, filter.folderPath.empty() ? "cover_cluster_v5" : "cover_folder_distinct_v1");
    auto payload = SerializeCoverFacetEntries(entries);
    return saveFacetQueryCache(persistentCacheKey, payload, (int)entries.size());
}

bool Database::persistPathFacetCache(const SearchFilter& filter,
                                     int limit,
                                     const std::vector<PathFacetEntry>& entries) {
    if (!ShouldPersistFacetEntriesForFilter(filter, (int)entries.size())) return false;
    const std::string persistentCacheKey =
        BuildPersistentFacetCacheKey(filter, limit, "path");
    auto payload = SerializePathFacetEntries(entries);
    return saveFacetQueryCache(persistentCacheKey, payload, (int)entries.size());
}

std::vector<std::string> Database::getDistinctFolderPaths() {
    std::vector<std::string> results;
    if (!m_db) return results;

    const char* sql = "SELECT DISTINCT folder_path FROM audio_files "
                      "WHERE folder_path IS NOT NULL AND folder_path != ''";
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            results.push_back(safeGetText(stmt, 0));
        }
        sqlite3_finalize(stmt);
    }
    return results;
}

// F-033 tag facets for normalized genre and instrument pill filters.

std::vector<Database::TagFacetEntry> Database::getTagFacets(
        const std::string& column, const SearchFilter& filter, int limit) {
    std::vector<TagFacetEntry> results;
    if (!m_db || limit <= 0) return results;

    // Only allow safe column names (norm_genre / norm_instruments)
    if (column != "norm_genre" && column != "norm_instruments") return results;

    const bool isGlobalFacetQuery =
        filter.keyword.empty() &&
        filter.folderPath.empty() &&
        filter.specificCoverKey.empty() &&
        filter.artist.empty() &&
        filter.artists.empty() &&
        filter.album.empty() &&
        filter.category.empty();
    if (isGlobalFacetQuery && isTagFacetIndexValid()) {
        sqlite3_stmt* indexStmt = nullptr;
        const char* indexSql =
            "SELECT value, weighting FROM tag_facet_index "
            "WHERE field = ? AND weighting > 0 "
            "ORDER BY weighting DESC, value ASC LIMIT ?";
        if (sqlite3_prepare_v2(m_db, indexSql, -1, &indexStmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(indexStmt, 1, column.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(indexStmt, 2, limit);
            while (sqlite3_step(indexStmt) == SQLITE_ROW) {
                TagFacetEntry e;
                e.tag = safeGetText(indexStmt, 0);
                e.count = sqlite3_column_int(indexStmt, 1);
                results.push_back(std::move(e));
            }
            sqlite3_finalize(indexStmt);
            if (!results.empty()) return results;
        }
    }

    BuiltSearchQuery bq = buildSearchWhere(filter);
    if (bq.parseError) return results;

    // Fetch raw field values from the filtered set; cap at 5000 rows to avoid stalls.
    const std::string columnExpr = "af." + column;
    const std::string coverInClause = bq.coverKeySubquery.empty() ? std::string()
        : " AND af.id IN (" + bq.coverKeySubquery + ")";
    std::string sql = "SELECT " + columnExpr + " " +
                      bq.fromClause + " " + bq.whereClause + coverInClause +
                      " AND " + columnExpr + " IS NOT NULL AND " + columnExpr + " != ''"
                      " LIMIT 5000";

    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(m_db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK)
        return results;

    BindSearchQuery(stmt, bq);
    {
        int nextBind = (int)bq.bindValues.size() + 1;
        for (const auto& val : bq.coverKeyBindValues) {
            sqlite3_bind_text(stmt, nextBind++, val.c_str(), -1, SQLITE_TRANSIENT);
        }
    }

    // Accumulate per-token counts (split on ';')
    std::unordered_map<std::string, int> counts;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char* raw = (const char*)sqlite3_column_text(stmt, 0);
        if (!raw) continue;
        AccumulateSemicolonFacetTokens(raw, &counts);
    }
    sqlite3_finalize(stmt);

    results.reserve(counts.size());
    for (auto& [tag, cnt] : counts)
        results.push_back({tag, cnt});
    std::sort(results.begin(), results.end(),
              [](const TagFacetEntry& a, const TagFacetEntry& b) {
                  return a.count > b.count;
              });
    if ((int)results.size() > limit)
        results.resize(limit);
    return results;
}

// SQL executor implementation.

bool Database::saveQueryViewSnapshot(const QueryViewSnapshot& snapshot) {
    if (!m_db || snapshot.snapshotKey.empty()) return false;

    static const char* sql = R"(
        INSERT INTO query_view_snapshots (
            snapshot_key, search_text, folder_path, cover_key, view_mode,
            sort_column, sort_ascending, selected_file_id, scroll_anchor_file_id,
            result_count, data_revision, payload_json, updated_at
        ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
        ON CONFLICT(snapshot_key) DO UPDATE SET
            search_text = excluded.search_text,
            folder_path = excluded.folder_path,
            cover_key = excluded.cover_key,
            view_mode = excluded.view_mode,
            sort_column = excluded.sort_column,
            sort_ascending = excluded.sort_ascending,
            selected_file_id = excluded.selected_file_id,
            scroll_anchor_file_id = excluded.scroll_anchor_file_id,
            result_count = excluded.result_count,
            data_revision = excluded.data_revision,
            payload_json = excluded.payload_json,
            updated_at = excluded.updated_at;
    )";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        m_lastError = sqlite3_errmsg(m_db);
        return false;
    }

    const int64_t updatedAt = snapshot.updatedAt > 0 ? snapshot.updatedAt : getCurrentTimestamp();
    sqlite3_bind_text(stmt, 1, snapshot.snapshotKey.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, snapshot.searchText.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, snapshot.folderPath.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, snapshot.coverKey.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 5, snapshot.viewMode);
    sqlite3_bind_int(stmt, 6, snapshot.sortColumn);
    sqlite3_bind_int(stmt, 7, snapshot.sortAscending ? 1 : 0);
    sqlite3_bind_int64(stmt, 8, snapshot.selectedFileId);
    sqlite3_bind_int64(stmt, 9, snapshot.scrollAnchorFileId);
    sqlite3_bind_int64(stmt, 10, snapshot.resultCount);
    sqlite3_bind_int64(stmt, 11, static_cast<sqlite3_int64>(snapshot.dataRevision));
    sqlite3_bind_text(stmt, 12, snapshot.payloadJson.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 13, updatedAt);

    const int rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
        m_lastError = sqlite3_errmsg(m_db);
        sqlite3_finalize(stmt);
        return false;
    }

    sqlite3_finalize(stmt);
    return true;
}

bool Database::loadQueryViewSnapshot(const std::string& snapshotKey, QueryViewSnapshot& outSnapshot) {
    if (!m_db || snapshotKey.empty()) return false;

    static const char* sql = R"(
        SELECT snapshot_key, search_text, folder_path, cover_key, view_mode,
               sort_column, sort_ascending, selected_file_id, scroll_anchor_file_id,
               result_count, data_revision, payload_json, updated_at
        FROM query_view_snapshots
        WHERE snapshot_key = ?
        LIMIT 1;
    )";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        m_lastError = sqlite3_errmsg(m_db);
        return false;
    }

    sqlite3_bind_text(stmt, 1, snapshotKey.c_str(), -1, SQLITE_TRANSIENT);
    const int rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        return false;
    }

    auto columnText = [stmt](int column) -> std::string {
        const unsigned char* text = sqlite3_column_text(stmt, column);
        return text ? reinterpret_cast<const char*>(text) : std::string();
    };

    QueryViewSnapshot snapshot;
    snapshot.snapshotKey = columnText(0);
    snapshot.searchText = columnText(1);
    snapshot.folderPath = columnText(2);
    snapshot.coverKey = columnText(3);
    snapshot.viewMode = sqlite3_column_int(stmt, 4);
    snapshot.sortColumn = sqlite3_column_int(stmt, 5);
    snapshot.sortAscending = sqlite3_column_int(stmt, 6) != 0;
    snapshot.selectedFileId = sqlite3_column_int64(stmt, 7);
    snapshot.scrollAnchorFileId = sqlite3_column_int64(stmt, 8);
    snapshot.resultCount = sqlite3_column_int64(stmt, 9);
    snapshot.dataRevision = static_cast<uint64_t>(sqlite3_column_int64(stmt, 10));
    snapshot.payloadJson = columnText(11);
    snapshot.updatedAt = sqlite3_column_int64(stmt, 12);
    outSnapshot = std::move(snapshot);

    sqlite3_finalize(stmt);
    return true;
}

bool Database::deleteQueryViewSnapshot(const std::string& snapshotKey) {
    if (!m_db || snapshotKey.empty()) return false;

    static const char* sql = "DELETE FROM query_view_snapshots WHERE snapshot_key = ?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        m_lastError = sqlite3_errmsg(m_db);
        return false;
    }

    sqlite3_bind_text(stmt, 1, snapshotKey.c_str(), -1, SQLITE_TRANSIENT);
    const int rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
        m_lastError = sqlite3_errmsg(m_db);
        sqlite3_finalize(stmt);
        return false;
    }

    sqlite3_finalize(stmt);
    return true;
}

int Database::pruneQueryViewSnapshots(int maxEntries) {
    if (!m_db || maxEntries < 0) return 0;

    static const char* sql = R"(
        DELETE FROM query_view_snapshots
        WHERE snapshot_key IN (
            SELECT snapshot_key
            FROM query_view_snapshots
            ORDER BY updated_at DESC, snapshot_key ASC
            LIMIT -1 OFFSET ?
        );
    )";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        m_lastError = sqlite3_errmsg(m_db);
        return 0;
    }

    sqlite3_bind_int(stmt, 1, maxEntries);
    const int rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
        m_lastError = sqlite3_errmsg(m_db);
        sqlite3_finalize(stmt);
        return 0;
    }

    const int rowsChanged = sqlite3_changes(m_db);
    sqlite3_finalize(stmt);
    return rowsChanged;
}

bool Database::appendActivityLog(const ActivityLogEntry& entry, int64_t* outId) {
    if (!m_db || entry.activityType.empty()) return false;

    static const char* sql = R"(
        INSERT INTO activity_log (
            activity_type, file_id, file_path, target_path, detail_json, created_at
        ) VALUES (?, ?, ?, ?, ?, ?);
    )";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        m_lastError = sqlite3_errmsg(m_db);
        return false;
    }

    const int64_t createdAt = entry.createdAt > 0 ? entry.createdAt : getCurrentTimestamp();
    sqlite3_bind_text(stmt, 1, entry.activityType.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 2, entry.fileId);
    sqlite3_bind_text(stmt, 3, entry.filePath.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, entry.targetPath.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, entry.detailJson.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 6, createdAt);

    const int rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
        m_lastError = sqlite3_errmsg(m_db);
        sqlite3_finalize(stmt);
        return false;
    }

    if (outId) {
        *outId = sqlite3_last_insert_rowid(m_db);
    }
    sqlite3_finalize(stmt);
    return true;
}

std::vector<Database::ActivityLogEntry> Database::getRecentActivityLog(
    const std::string& activityType,
    int limit) {
    std::vector<ActivityLogEntry> entries;
    if (!m_db || limit <= 0) return entries;

    const int boundedLimit = std::min(limit, 1000);
    const bool filterByType = !activityType.empty();
    const char* sql = filterByType
        ? R"(
            SELECT id, activity_type, file_id, file_path, target_path, detail_json, created_at
            FROM activity_log
            WHERE activity_type = ?
            ORDER BY created_at DESC, id DESC
            LIMIT ?;
        )"
        : R"(
            SELECT id, activity_type, file_id, file_path, target_path, detail_json, created_at
            FROM activity_log
            ORDER BY created_at DESC, id DESC
            LIMIT ?;
        )";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        m_lastError = sqlite3_errmsg(m_db);
        return entries;
    }

    if (filterByType) {
        sqlite3_bind_text(stmt, 1, activityType.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 2, boundedLimit);
    } else {
        sqlite3_bind_int(stmt, 1, boundedLimit);
    }

    auto columnText = [stmt](int column) -> std::string {
        const unsigned char* text = sqlite3_column_text(stmt, column);
        return text ? reinterpret_cast<const char*>(text) : std::string();
    };

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        ActivityLogEntry entry;
        entry.id = sqlite3_column_int64(stmt, 0);
        entry.activityType = columnText(1);
        entry.fileId = sqlite3_column_int64(stmt, 2);
        entry.filePath = columnText(3);
        entry.targetPath = columnText(4);
        entry.detailJson = columnText(5);
        entry.createdAt = sqlite3_column_int64(stmt, 6);
        entries.push_back(std::move(entry));
    }

    sqlite3_finalize(stmt);
    return entries;
}

int Database::pruneActivityLog(int maxEntries) {
    if (!m_db || maxEntries < 0) return 0;

    static const char* sql = R"(
        DELETE FROM activity_log
        WHERE id IN (
            SELECT id
            FROM activity_log
            ORDER BY created_at DESC, id DESC
            LIMIT -1 OFFSET ?
        );
    )";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        m_lastError = sqlite3_errmsg(m_db);
        return 0;
    }

    sqlite3_bind_int(stmt, 1, maxEntries);
    const int rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
        m_lastError = sqlite3_errmsg(m_db);
        sqlite3_finalize(stmt);
        return 0;
    }

    const int rowsChanged = sqlite3_changes(m_db);
    sqlite3_finalize(stmt);
    return rowsChanged;
}

Database::SqlResult Database::executeRawQuery(const std::string& sql) {
    SqlResult res;
    if (!m_db) {
        res.error = "Database not open";
        return res;
    }

    sqlite3_stmt* stmt;
    // 准备语句
    if (sqlite3_prepare_v2(m_db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        res.error = sqlite3_errmsg(m_db);
        return res;
    }

    // Read column names.
    int colCount = sqlite3_column_count(stmt);
    for (int i = 0; i < colCount; i++) {
        res.columns.push_back(sqlite3_column_name(stmt, i));
    }

    // 执行并获取行数据
    bool isSelect = (colCount > 0);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        std::vector<std::string> row;
        for (int i = 0; i < colCount; i++) {
            const char* txt = (const char*)sqlite3_column_text(stmt, i);
            row.push_back(txt ? txt : "(NULL)");
        }
        res.rows.push_back(std::move(row));
    }

    // 如果不是查询语句，获取受影响行数
    if (!isSelect) {
        res.rowsAffected = sqlite3_changes(m_db);
    }

    sqlite3_finalize(stmt);
    res.success = true;
    return res;
}

// File-system watcher integration.

// 支持的音频文件扩展名
static bool IsAudioExtension(const std::string& ext) {
    static const std::unordered_set<std::string> kAudioExts = {
        ".wav", ".mp3", ".flac", ".ogg", ".aiff", ".aif", ".m4a", ".wma",
        ".opus", ".ape", ".wv", ".ac3", ".aac"
    };
    std::string lower = ext;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    return kAudioExts.count(lower) > 0;
}

static std::string GetExtensionUtf8(const std::string& pathUtf8) {
    try {
        fs::path p = Utf8ToPath(pathUtf8);
        return PathToUtf8(p.extension());
    } catch (...) {
        auto dot = pathUtf8.rfind('.');
        if (dot != std::string::npos) return pathUtf8.substr(dot);
        return "";
    }
}

namespace {
constexpr uint64_t kExactRenameDedupeTtlMs = 3000;
constexpr uint64_t kPrefixRenameDedupeTtlMs = 8000;

static uint64_t SteadyNowMs() {
    using namespace std::chrono;
    return (uint64_t)duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}
} // namespace

std::string Database::normalizeWatcherPath(const std::string& path) {
    if (path.empty()) return "";

    std::string s = path;

#ifdef _WIN32
    if (s.rfind("\\\\?\\UNC\\", 0) == 0) {
        s = "\\\\" + s.substr(8);
    } else if (s.rfind("\\\\?\\", 0) == 0) {
        s = s.substr(4);
    }

    std::replace(s.begin(), s.end(), '/', '\\');

    while (s.size() > 3 && !s.empty() && (s.back() == '\\' || s.back() == '/')) {
        s.pop_back();
    }

    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
#else
    std::replace(s.begin(), s.end(), '\\', '/');
    while (s.size() > 1 && !s.empty() && s.back() == '/') {
        s.pop_back();
    }
#endif

    return s;
}

std::string Database::makeRenamePairKey(const std::string& oldNorm, const std::string& newNorm) {
    return oldNorm + "\n" + newNorm;
}

bool Database::startsWithPathPrefix(const std::string& pathNorm, const std::string& prefixNorm) {
    if (pathNorm.empty() || prefixNorm.empty()) return false;
    if (pathNorm.size() < prefixNorm.size()) return false;
    return pathNorm.compare(0, prefixNorm.size(), prefixNorm) == 0;
}

void Database::refreshFsDedupePendingFlagLocked() {
    const bool any = !m_pendingRenamePairs.empty() || !m_pendingPathEvents.empty() ||
                     !m_pendingPrefixRenames.empty();
    m_hasPendingFsDedupe.store(any, std::memory_order_relaxed);
}

void Database::pruneFsDedupeLocked(uint64_t nowMs) {
    for (auto it = m_pendingRenamePairs.begin(); it != m_pendingRenamePairs.end();) {
        if (it->second <= nowMs)
            it = m_pendingRenamePairs.erase(it);
        else
            ++it;
    }

    for (auto it = m_pendingPathEvents.begin(); it != m_pendingPathEvents.end();) {
        if (it->second <= nowMs)
            it = m_pendingPathEvents.erase(it);
        else
            ++it;
    }

    m_pendingPrefixRenames.erase(
        std::remove_if(
            m_pendingPrefixRenames.begin(),
            m_pendingPrefixRenames.end(),
            [nowMs](const PendingPrefixRename& x) { return x.expireAtMs <= nowMs; }),
        m_pendingPrefixRenames.end());

    refreshFsDedupePendingFlagLocked();
}

void Database::noteExpectedFileRename(const std::string& oldPath, const std::string& newPath) {
    if (oldPath.empty() || newPath.empty()) return;

    const uint64_t expireAt = SteadyNowMs() + kExactRenameDedupeTtlMs;
    const std::string oldNorm = normalizeWatcherPath(oldPath);
    const std::string newNorm = normalizeWatcherPath(newPath);
    if (oldNorm.empty() || newNorm.empty()) return;

    std::lock_guard<std::mutex> lk(m_fsDedupeMutex);
    pruneFsDedupeLocked(SteadyNowMs());

    m_pendingRenamePairs[makeRenamePairKey(oldNorm, newNorm)] = expireAt;

    m_pendingPathEvents[oldNorm] = expireAt;
    m_pendingPathEvents[newNorm] = expireAt;

    refreshFsDedupePendingFlagLocked();
}

void Database::noteExpectedFolderRename(const std::string& oldFolderPath, const std::string& newFolderPath) {
    if (oldFolderPath.empty() || newFolderPath.empty()) return;

    std::string oldNorm = normalizeWatcherPath(oldFolderPath);
    std::string newNorm = normalizeWatcherPath(newFolderPath);
    if (oldNorm.empty() || newNorm.empty()) return;

#ifdef _WIN32
    if (oldNorm.back() != '\\') oldNorm.push_back('\\');
    if (newNorm.back() != '\\') newNorm.push_back('\\');
#else
    if (oldNorm.back() != '/') oldNorm.push_back('/');
    if (newNorm.back() != '/') newNorm.push_back('/');
#endif

    PendingPrefixRename rule;
    rule.oldPrefixNorm = std::move(oldNorm);
    rule.newPrefixNorm = std::move(newNorm);
    rule.expireAtMs = SteadyNowMs() + kPrefixRenameDedupeTtlMs;

    std::lock_guard<std::mutex> lk(m_fsDedupeMutex);
    pruneFsDedupeLocked(SteadyNowMs());
    m_pendingPrefixRenames.push_back(std::move(rule));

    refreshFsDedupePendingFlagLocked();
}

bool Database::shouldSuppressWatcherEvent(const FileSystemWatcher::FileEvent& event) {
    if (event.type == FileSystemWatcher::EventType::NeedRescan) {
        return false;
    }

    // Fast path with no local rename/move window: avoid normalization and locking.
    if (!m_hasPendingFsDedupe.load(std::memory_order_relaxed)) {
        return false;
    }

    // Modified events do not use rename dedupe; skip path normalization on this hot path.
    if (event.type == FileSystemWatcher::EventType::FileModified) {
        return false;
    }

    const uint64_t nowMs = SteadyNowMs();
    const std::string pathNorm = normalizeWatcherPath(event.path);
    const std::string oldNorm = normalizeWatcherPath(event.oldPath);

    std::lock_guard<std::mutex> lk(m_fsDedupeMutex);
    pruneFsDedupeLocked(nowMs);

    if (event.type == FileSystemWatcher::EventType::FileRenamed) {
        if (!oldNorm.empty() && !pathNorm.empty()) {
            const std::string pairKey = makeRenamePairKey(oldNorm, pathNorm);
            auto it = m_pendingRenamePairs.find(pairKey);
            if (it != m_pendingRenamePairs.end()) {
                return true;
            }
        }

        for (const auto& rule : m_pendingPrefixRenames) {
            if (!oldNorm.empty() && !pathNorm.empty() &&
                startsWithPathPrefix(oldNorm, rule.oldPrefixNorm) &&
                startsWithPathPrefix(pathNorm, rule.newPrefixNorm)) {
                return true;
            }
        }

        return false;
    }

    if (event.type == FileSystemWatcher::EventType::FileAdded ||
        event.type == FileSystemWatcher::EventType::FileDeleted) {
        auto it = m_pendingPathEvents.find(pathNorm);
        if (it != m_pendingPathEvents.end()) {
            m_pendingPathEvents.erase(it);
            return true;
        }
    }

    if (event.type == FileSystemWatcher::EventType::FileAdded ||
        event.type == FileSystemWatcher::EventType::FileDeleted ||
        event.type == FileSystemWatcher::EventType::FileRenamed) {
        for (const auto& rule : m_pendingPrefixRenames) {
            if (!pathNorm.empty() &&
                (startsWithPathPrefix(pathNorm, rule.oldPrefixNorm) ||
                 startsWithPathPrefix(pathNorm, rule.newPrefixNorm))) {
                return true;
            }
        }
    }

    return false;
}

void Database::ensureWatcherRescanWorkerStarted() {
    std::lock_guard<std::mutex> lk(m_watcherRescanMutex);
    if (m_watcherRescanThread.joinable()) return;
    m_watcherRescanRunning.store(true, std::memory_order_release);
    // A new worker clears cancellation left by the previous session.

    m_watcherRescanCancel.store(false, std::memory_order_release);
    m_watcherRescanThread = std::thread(&Database::watcherRescanWorkerThread, this);
}

void Database::enqueueWatcherFullRescan(const std::string& rootUtf8) {
    if (rootUtf8.empty() || !m_db) return;
    ensureWatcherRescanWorkerStarted();
    {
        std::lock_guard<std::mutex> lk(m_watcherRescanMutex);
        if (!m_watcherRescanPendingRoots.insert(rootUtf8).second) return;
        m_watcherRescanQueue.push_back(rootUtf8);
    }
    m_watcherRescanCv.notify_one();
}

// T-297: waveform pre-extraction worker

void Database::ensurePrecomputeWorkerStarted() {
    std::lock_guard<std::mutex> lk(m_precomputeMutex);
    if (m_precomputeThread.joinable()) return;
    m_precomputeCancel.store(false, std::memory_order_release);
    m_precomputeRunning.store(true, std::memory_order_release);
    m_precomputeThread = std::thread(&Database::precomputeWorkerThread, this);
}

void Database::enqueuePrecomputeWaveform(const std::string& filePath,
                                         const std::string& cacheKey) {
    if (filePath.empty() || !m_db) return;
    // T-297 safety valve: cap the queue so an accidental million-file
    // enqueue (e.g. a misconfigured watcher rescan with precompute=true)
    // can't hold worker memory hostage. 50k jobs ≈ several hours of
    // decode at typical SFX file sizes — well past any sane batch.
    constexpr size_t kMaxPendingJobs = 50000;
    ensurePrecomputeWorkerStarted();
    {
        std::lock_guard<std::mutex> lk(m_precomputeMutex);
        if (m_precomputePending.size() >= kMaxPendingJobs) {
            // Drop silently; surfaced via diagnostics counter instead of
            // a per-call log to avoid spamming when queue is saturated.
            m_precomputeFailed.fetch_add(1, std::memory_order_acq_rel);
            return;
        }
        if (!m_precomputePending.insert(filePath).second) return;
        m_precomputeQueue.push_back({filePath, cacheKey});
    }
    m_precomputeCv.notify_one();
}

void Database::stopPrecomputeWorker() {
    const auto stopStart = std::chrono::steady_clock::now();
    m_precomputeCancel.store(true, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lk(m_precomputeMutex);
        m_precomputeRunning.store(false, std::memory_order_release);
    }
    m_precomputeCv.notify_all();
    if (m_precomputeThread.joinable()) {
        m_precomputeThread.join();
    }
    std::lock_guard<std::mutex> lk(m_precomputeMutex);
    m_precomputeQueue.clear();
    m_precomputePending.clear();
    const double stopMs = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - stopStart).count();
    if (stopMs > 250.0) {
        DIAG_SYSTEM_FMT("Database", "PrecomputeWorkerStop: waitedMs=%.2f", stopMs);
    }
}

int Database::precomputeQueueDepth() const {
    std::lock_guard<std::mutex> lk(
        const_cast<std::mutex&>(m_precomputeMutex));
    return static_cast<int>(m_precomputePending.size());
}

void Database::precomputeWorkerThread() {
    // Complete import contract: once scanDirectoryEx(..., completeImport)
    // returns, valid files should have the same main-detail precision the UI
    // asks for on selection.
    std::unique_lock<std::mutex> lk(m_precomputeMutex);
    while (m_precomputeRunning.load(std::memory_order_acquire)) {
        m_precomputeCv.wait(lk, [&] {
            return !m_precomputeQueue.empty() ||
                   !m_precomputeRunning.load(std::memory_order_acquire);
        });
        // Wait while scan code owns a transaction on the shared connection.
        // This prevents worker saves from joining or disrupting the scan batch.
        // Scan batches are short, so a brief backoff keeps contention bounded.

        if (m_db && sqlite3_get_autocommit(m_db) == 0) {
            m_precomputeCv.wait_for(lk, std::chrono::milliseconds(150));
            continue;
        }
        while (!m_precomputeQueue.empty() && m_db &&
               m_precomputeRunning.load(std::memory_order_acquire) &&
               sqlite3_get_autocommit(m_db) != 0) {  // re-check inside loop
            PrecomputeJob job = std::move(m_precomputeQueue.front());
            m_precomputeQueue.pop_front();
            // Keep the path in m_precomputePending for the duration of the
            // job so a re-enqueue while we're working doesn't double-process.
            lk.unlock();

            // Decode before acquiring database state, then save both payloads in one
            // transaction so a rescan cannot interleave a new pending state.

            WaveformExtractResult mini;
            WaveformExtractResult detail;
            bool miniOk = false;
            bool detailOk = false;
            try {
                miniOk = WaveformExtractor::ExtractMini(job.filePath, /*targetPeaks=*/0, mini,
                                                       /*perChannel=*/false,
                                                       &m_precomputeCancel) &&
                         !mini.peaks.empty();
            } catch (...) {
// Swallow — corrupted files shouldn't kill the worker
            }

            bool earlyMiniSaved = false;
            if (miniOk) {
                std::unique_lock<std::mutex> writerLk(m_writerSerializeMutex);
                const bool ownTxn = (sqlite3_get_autocommit(m_db) != 0);
                bool txnBegan = false;
                if (ownTxn) {
                    txnBegan = beginTransaction();
                }

                WaveformCacheState currentState;
                const bool stillOurs = getWaveformCacheStateByPath(job.filePath, currentState) &&
                                        currentState.cacheKey == job.cacheKey;
                if (stillOurs) {
                    const bool hasMiniEnvelope =
                        mini.minPeaks.size() == mini.peaks.size() && !mini.peaks.empty();
                    const float trueRms = mini.hasTrueRms ? mini.trueRmsDbfs : -200.0f;
                    const int channels = std::max(1, mini.channels);
                    earlyMiniSaved = hasMiniEnvelope
                        ? saveMiniWaveformEnvelopeByPath(job.filePath, mini.peaks, mini.minPeaks,
                                                         trueRms, channels)
                        : saveWaveformByPath(job.filePath, mini.peaks, trueRms, channels);
                    if (earlyMiniSaved) {
                        // Keep the row eligible for detail completion, but make
                        // waveform_data immediately available to the list UI.
                        upsertWaveformCacheStateByPath(job.filePath, "pending", "", 0, job.cacheKey);
                    }
                }
                if (txnBegan) commitTransaction();
            }

            const AudioRecord recForPolicy = getFileByPath(job.filePath);
            const auto detailPolicy =
                WaveformCachePolicy::PersistentMainDetailForDuration(
                    getWaveformCacheMode(),
                    recForPolicy.durationSeconds);
            if (detailPolicy.persistMainDetail) {
                try {
                    detailOk = WaveformExtractor::ExtractDetail(job.filePath, detailPolicy.targetPeaks,
                                                               /*withRms=*/true, detail,
                                                               &m_precomputeCancel) &&
                               !detail.peaks.empty();
                } catch (...) {

                }
            }

            // Take the writer gate before checking autocommit and beginning a
            // transaction. Scan batches use the same gate, closing the race.

            std::string newKeyToRequeue;
            bool savedSomething = false;
            bool detailSaved = false;
            std::unique_lock<std::mutex> writerLk(m_writerSerializeMutex);
            const bool ownTxn = (sqlite3_get_autocommit(m_db) != 0);
            bool txnBegan = false;
            if (ownTxn) {
                txnBegan = beginTransaction();
                // If begin failed (e.g. busy on lock) we just fall through
                // and run statements without an explicit txn — sqlite still
                // commits each one atomically.
            }
            WaveformCacheState currentState;
            const bool stillOurs = getWaveformCacheStateByPath(job.filePath, currentState) &&
                                    currentState.cacheKey == job.cacheKey;
            if (stillOurs) {
                if (miniOk && !earlyMiniSaved) {
                    const bool hasMiniEnvelope =
                        mini.minPeaks.size() == mini.peaks.size() && !mini.peaks.empty();
                    const float trueRms = mini.hasTrueRms ? mini.trueRmsDbfs : -200.0f;
                    const int channels = std::max(1, mini.channels);
                    const bool miniSaved = hasMiniEnvelope
                        ? saveMiniWaveformEnvelopeByPath(job.filePath, mini.peaks, mini.minPeaks,
                                                         trueRms, channels)
                        : saveWaveformByPath(job.filePath, mini.peaks, trueRms, channels);
                    if (miniSaved) {
                        savedSomething = true;
                    }
                } else if (earlyMiniSaved) {
                    savedSomething = true;
                }
                if (detailPolicy.persistMainDetail && detailOk) {
                    if (saveWaveformDetailByPath(job.filePath, detail.peaks, detail.minPeaks, detail.channels)) {
                        savedSomething = true;
                        detailSaved = true;
                    }
                }
                if (detailSaved || (savedSomething && !detailPolicy.persistMainDetail)) {
                    upsertWaveformCacheStateByPath(job.filePath, "done", "", 0, job.cacheKey);
                } else if (savedSomething) {
                    upsertWaveformCacheStateByPath(job.filePath, "pending",
                                                   "detail_precompute_pending", 0, job.cacheKey);
                } else {
                    upsertWaveformCacheStateByPath(job.filePath, "failed",
                                                   "precompute_extract_failed", 1, job.cacheKey);
                }
            } else {
                if (!currentState.cacheKey.empty() &&
                    currentState.cacheKey != job.cacheKey) {
                    newKeyToRequeue = currentState.cacheKey;
                }
            }
            if (txnBegan) commitTransaction();

            if (stillOurs && savedSomething) {
                m_precomputeProcessed.fetch_add(1, std::memory_order_acq_rel);
            } else {
                m_precomputeFailed.fetch_add(1, std::memory_order_acq_rel);
            }

            lk.lock();
            m_precomputePending.erase(job.filePath);
            // Re-enqueue under lock so the new key is picked up on the
            // next CV wakeup (next iteration of the outer while loop).
            if (!newKeyToRequeue.empty()) {
                if (m_precomputePending.insert(job.filePath).second) {
                    m_precomputeQueue.push_back({job.filePath, newKeyToRequeue});
                    m_precomputeCv.notify_one();
                }
            }
        }
    }
}

// T-296: Reconcile every watched root after startup / DB switch so files
// that changed while Harken Search was closed are picked up automatically. The
// existing watcher rescan worker handles the scan asynchronously.
// Returns the number of non-empty roots dispatched (worker may dedupe
// internally, but this counts what we asked for).
int Database::enqueueStartupReconciliation() {
    if (!m_db) return 0;
    if (ReadEnvIntClamped("AURA_ENABLE_STARTUP_RECONCILIATION", 0, 0, 1) != 1) {
        DIAG_SYSTEM("Database",
                    "StartupReconciliation skipped: automatic root rescan disabled by default");
        return 0;
    }
    constexpr uint64_t kStartupReconcileMaxDbBytes = 1024ull * 1024ull * 1024ull;
    if (!m_mainDbPath.empty()) {
        uint64_t packageBytes = GetDatabasePackageSize(m_mainDbPath);
        if (packageBytes > kStartupReconcileMaxDbBytes) {
            DIAG_SYSTEM_FMT("Database",
                            "StartupReconciliation skipped: packageBytes=%llu exceeds cap=%llu",
                            static_cast<unsigned long long>(packageBytes),
                            static_cast<unsigned long long>(kStartupReconcileMaxDbBytes));
            return 0;
        }
    }

    // Reconcile only registered library roots. Watched-root fallbacks may include
    // a whole drive from legacy directory rows and would import unrelated files.
    // mergeLibraryRoots is authoritative for startup reconciliation.

    std::vector<std::string> roots = getLibraryRoots();
    int dispatched = 0;
    for (const auto& root : roots) {
        if (root.empty()) continue;
        enqueueWatcherFullRescan(root);
        ++dispatched;
    }
    DIAG_SYSTEM_FMT("Database",
                    "StartupReconciliation: dispatched %d/%zu library roots",
                    dispatched, roots.size());
    return dispatched;
}

void Database::stopWatcherRescanWorker() {
    // Signal cancellation before taking the queue lock so an active scan can
    // leave enumeration and batch loops before shutdown joins the worker.

    m_watcherRescanCancel.store(true, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lk(m_watcherRescanMutex);
        m_watcherRescanRunning.store(false, std::memory_order_release);
    }
    m_watcherRescanCv.notify_all();
    if (m_watcherRescanThread.joinable()) {
        m_watcherRescanThread.join();
    }
    std::lock_guard<std::mutex> lk(m_watcherRescanMutex);
    m_watcherRescanQueue.clear();
    m_watcherRescanPendingRoots.clear();
}

void Database::watcherRescanWorkerThread() {
    std::unique_lock<std::mutex> lk(m_watcherRescanMutex);
    while (m_watcherRescanRunning.load(std::memory_order_acquire)) {
        m_watcherRescanCv.wait(lk, [&] {
            return !m_watcherRescanQueue.empty() ||
                   !m_watcherRescanRunning.load(std::memory_order_acquire);
        });
        while (!m_watcherRescanQueue.empty() && m_db &&
               m_watcherRescanRunning.load(std::memory_order_acquire) &&
               !m_watcherRescanCancel.load(std::memory_order_acquire)) {
            std::string root = std::move(m_watcherRescanQueue.front());
            m_watcherRescanQueue.pop_front();
            m_watcherRescanPendingRoots.erase(root);
            lk.unlock();
            try {
                while (g_appState &&
                       g_appState->isScanning.load(std::memory_order_acquire) &&
                       m_watcherRescanRunning.load(std::memory_order_acquire) &&
                       !m_watcherRescanCancel.load(std::memory_order_acquire)) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(250));
                }
                if (m_watcherRescanCancel.load(std::memory_order_acquire) ||
                    !m_watcherRescanRunning.load(std::memory_order_acquire)) {
                    lk.lock();
                    continue;
                }
               DIAG_ACTION_FMT("Watcher", "NeedRescan: scanDirectoryEx root=%s", root.c_str());
               // Track active scan work separately from worker-thread lifetime.

               m_watcherRescanActive.store(true, std::memory_order_release);
                const auto rootStatus = Platform::FileSystem::ProbePath(root);
                const bool removeMissing =
                    rootStatus != Platform::FileSystem::PathStatus::Unreachable;
                if (rootStatus == Platform::FileSystem::PathStatus::Unreachable) {
                    DIAG_ACTION_FMT("Watcher",
                                    "NeedRescan: skip removeMissing, unreachable root=%s",
                                    root.c_str());
                }
                scanDirectoryEx(root, true, true, removeMissing, nullptr,
                                &m_watcherRescanCancel, 0,
                                /*completeImport=*/false,
                                /*enqueueExtraction=*/false);
                m_watcherRescanActive.store(false, std::memory_order_release);
            } catch (...) {
                m_watcherRescanActive.store(false, std::memory_order_release);
            }
            lk.lock();
        }
    }
}

void Database::startFileSystemWatcher(const std::vector<std::string>& watchDirectories) {
    // 停止已有的监听器
    stopFileSystemWatcher();
    
    if (watchDirectories.empty()) return;
    
    m_fileSystemWatcher = new FileSystemWatcher();
    
    // 添加监听目录
    for (const auto& dir : watchDirectories) {
        if (!dir.empty()) {
            m_fileSystemWatcher->addWatchDirectory(dir);
        }
    }
    
    m_fileSystemWatcher->setEventCallback([this](const FileSystemWatcher::FileEvent& event) {
        if (event.type == FileSystemWatcher::EventType::NeedRescan) {
            DIAG_ACTION_FMT("Watcher", "NeedRescan requested for root: %s", event.path.c_str());
            enqueueWatcherFullRescan(event.path);
            return;
        }

        if (shouldSuppressWatcherEvent(event)) {
            DIAG_ACTION_FMT("Watcher", "Suppressed self-induced FS event: type=%d path=%s old=%s",
                            (int)event.type, event.path.c_str(), event.oldPath.c_str());
            return;
        }

        const bool newIsAudio = IsAudioExtension(GetExtensionUtf8(event.path));
        const bool oldIsAudio = IsAudioExtension(GetExtensionUtf8(event.oldPath));

        switch (event.type) {
            case FileSystemWatcher::EventType::FileAdded:
                if (newIsAudio) {
                    handleFileSystemEvent(event.path, true, false, false);
                }
                break;

            case FileSystemWatcher::EventType::FileDeleted:
                if (newIsAudio) {
                    handleFileSystemEvent(event.path, false, true, false);
                }
                break;

            case FileSystemWatcher::EventType::FileModified:
                if (newIsAudio) {
                    handleFileSystemEvent(event.path, false, false, true);
                }
                break;

            case FileSystemWatcher::EventType::FileRenamed:
                if (oldIsAudio && newIsAudio) {
                    std::string newFileName;
                    try {
                        newFileName = PathToUtf8(Utf8ToPath(event.path).filename());
                    } catch (...) {
                        newFileName.clear();
                    }

                    if (!updateFilePath(event.oldPath, event.path, newFileName)) {
                        DIAG_ACTION_FMT("Watcher",
                                        "Watcher rename DB update failed: %s -> %s",
                                        event.oldPath.c_str(),
                                        event.path.c_str());
                    } else if (m_watcherChangeCallback) {
                        m_watcherChangeCallback(WatcherChangeType::Renamed, event.path);
                    }
                } else if (oldIsAudio && !newIsAudio) {
                    handleFileSystemEvent(event.oldPath, false, true, false);
                } else if (!oldIsAudio && newIsAudio) {
                    handleFileSystemEvent(event.path, true, false, false);
                }
                break;

            default:
                break;
        }
    });
    
    // 启动监听
    m_fileSystemWatcher->start();
}

void Database::stopFileSystemWatcher() {
    if (m_fileSystemWatcher) {
        m_fileSystemWatcher->stop();
        delete m_fileSystemWatcher;
        m_fileSystemWatcher = nullptr;
    }
    stopWatcherRescanWorker();
}

void Database::handleFileSystemEvent(const std::string& filePath, bool isAdded, bool isDeleted, bool isModified) {
    if (!m_db || filePath.empty()) return;

    // The watcher may race with removal; verify existence and file type before reading metadata.
    if (isAdded || isModified) {
        if (Platform::FileSystem::ProbePath(filePath) !=
                Platform::FileSystem::PathStatus::Present) return;
        if (Platform::FileSystem::IsDirectory(filePath)) return;
    }

    if (isDeleted) {
        removeFileByPath(filePath);
        if (m_watcherChangeCallback)
            m_watcherChangeCallback(WatcherChangeType::Deleted, filePath);
        return;
    }
    
    if (isAdded) {
        // 文件添加：检查是否已存在
        if (fileExists(filePath)) return;
        
        // Read audio properties.
        AudioRecord record;
        record.filePath = filePath;
        
        try {
            fs::path p = Utf8ToPath(filePath);
            record.fileName = PathToUtf8(p.filename());
            record.extension = PathToUtf8(p.extension());
            record.folderPath = PathToUtf8(p.parent_path());
            
            // Read file size and modification time.
            const auto fileInfo = Platform::FileSystem::ProbeRegularFile(filePath);
            record.fileSize = fileInfo.sizeAvailable ? fileInfo.sizeBytes : 0;
            record.fileMtime = Platform::FileSystem::LastWriteTimeUnixSeconds(filePath);
        } catch (...) {
            return; // 文件路径解析失败
        }
        
        readAudioFileInfo(Utf8ToPath(filePath),
                          record.sampleRate,
                          record.channels,
                          record.totalFrames,
                          record.durationSeconds);
        
        // Read metadata tags.
        AudioMetadata metadata;
        if (MetadataReader::readMetadata(filePath, metadata)) {
            if (!metadata.title.empty()) record.title = metadata.title;
            if (!metadata.artist.empty()) record.artist = metadata.artist;
            if (!metadata.album.empty()) record.album = metadata.album;
            if (!metadata.albumArtist.empty()) record.albumArtist = metadata.albumArtist;
            if (metadata.replayGainTrack != 0.0f) record.replayGainTrack = metadata.replayGainTrack;
            if (metadata.replayGainAlbum != 0.0f) record.replayGainAlbum = metadata.replayGainAlbum;
            if (metadata.track > 0) record.trackNumber = metadata.track;
            if (metadata.disc > 1) record.discNumber = metadata.disc;
            if (!metadata.description.empty()) record.description = metadata.description;
            if (!metadata.category.empty()) record.category = metadata.category;
            else if (!metadata.genre.empty()) record.category = metadata.genre;

            if (!metadata.genre.empty()) record.rawGenre = metadata.genre;
            if (!metadata.instruments.empty()) record.rawInstruments = metadata.instruments;
            TagVocabularyMapper::normalize(
                record.rawGenre,
                record.rawInstruments,
                record.normGenre,
                record.normInstruments,
                record.normMoodTags,
                record.vocalType
            );
            if (!metadata.keywords.empty()) record.keywords = metadata.keywords;
            else if (!metadata.comment.empty()) record.keywords = metadata.comment;
            PopulateExtendedMeta(record.meta, metadata);
        }

        record.dateAdded = (int64_t)std::time(nullptr);
        record.lastIndexed = record.dateAdded;

        addFile(record);
        if (m_watcherChangeCallback)
            m_watcherChangeCallback(WatcherChangeType::Added, filePath);
        return;
    }
    
    if (isModified) {
        // 文件修改：更新元数据
        AudioRecord existing = getFileByPath(filePath);
        if (existing.id <= 0) {
            // If the path is not in the database, handle it as an addition.
            handleFileSystemEvent(filePath, true, false, false);
            return;
        }
        
        // Check whether the modification time actually changed.
        try {
            const int64_t newMtime =
                Platform::FileSystem::LastWriteTimeUnixSeconds(filePath);
            if (newMtime > 0) {
                if (newMtime == existing.fileMtime) return; // No effective change.
                existing.fileMtime = newMtime;
            }
            const auto modifiedFileInfo = Platform::FileSystem::ProbeRegularFile(filePath);
            existing.fileSize = modifiedFileInfo.sizeAvailable
                ? modifiedFileInfo.sizeBytes
                : std::numeric_limits<uint64_t>::max();
        } catch (...) {}
        
        readAudioFileInfo(Utf8ToPath(filePath),
                          existing.sampleRate,
                          existing.channels,
                          existing.totalFrames,
                          existing.durationSeconds);
        
        // Re-read metadata tags.
        AudioMetadata metadata;
        if (MetadataReader::readMetadata(filePath, metadata)) {
            if (!metadata.title.empty()) existing.title = metadata.title;
            if (!metadata.artist.empty()) existing.artist = metadata.artist;
            if (!metadata.album.empty()) existing.album = metadata.album;
            if (!metadata.albumArtist.empty()) existing.albumArtist = metadata.albumArtist;
            if (metadata.track > 0) existing.trackNumber = metadata.track;
            if (metadata.disc > 1) existing.discNumber = metadata.disc;
            if (!metadata.description.empty()) existing.description = metadata.description;
            if (!metadata.category.empty()) existing.category = metadata.category;
            else if (!metadata.genre.empty()) existing.category = metadata.genre;
            if (!metadata.keywords.empty()) existing.keywords = metadata.keywords;
            else if (!metadata.comment.empty()) existing.keywords = metadata.comment;
            PopulateExtendedMeta(existing.meta, metadata);
        }
        
        existing.lastIndexed = (int64_t)std::time(nullptr);
        updateFile(existing);
        if (m_watcherChangeCallback)
            m_watcherChangeCallback(WatcherChangeType::Modified, filePath);
    }
}

void Database::interrupt() {
    if (m_db) {
        sqlite3_interrupt(m_db);
    }
}

// Fast search suggestions backed by the FTS5 vocabulary index.

std::vector<std::string> Database::getSearchSuggestions(const std::string& query, int limit) {
    std::vector<std::string> results;
    if (!m_db || query.empty() || limit <= 0) return results;

    // FTS terms are lowercase, so normalize the query before the range scan.
    std::string lowerQuery = query;
    std::transform(lowerQuery.begin(), lowerQuery.end(), lowerQuery.begin(), ::tolower);

    // Build the exclusive upper bound for the B-tree prefix query.
    std::string upperQuery = lowerQuery;
    upperQuery.back() = static_cast<char>(upperQuery.back() + 1);

    // Indexed prefix query:
    // Use the B-tree index and rank by document frequency without a temporary GROUP BY.
    const char* sql = "SELECT term FROM audio_files_vocab "
                      "WHERE term >= ? AND term < ? "
                      "ORDER BY doc DESC LIMIT ?";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, lowerQuery.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, upperQuery.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 3, limit);

        while (sqlite3_step(stmt) == SQLITE_ROW) {
            std::string term = safeGetText(stmt, 0);
            // Drop one-character terms and filename-extension tokens.
            if (term.length() > 1 && term.find(".wav") == std::string::npos && term.find(".mp3") == std::string::npos) {
                results.push_back(term);
            }
        }
        sqlite3_finalize(stmt);
    }
    return results;
}

// Build a multiresolution waveform pyramid.
Database::WaveformMipmap Database::generateWaveformMipmap(const std::vector<float>& rawSamples, uint32_t sampleRate) {
    (void)sampleRate; // Reserved for future level or duration policies.
    WaveformMipmap mipmap;
    mipmap.totalSamples = rawSamples.size();
    if (rawSamples.empty()) return mipmap;

    // 定义金字塔层级步进：256, 1024, 4096, 16384
    // 每一层代表该像素点包含的采样帧数
    const std::vector<int> steps = { 256, 1024, 4096, 16384 };
    const size_t totalSamples = rawSamples.size();

    for (int step : steps) {
        WaveformMipmapLevel level;
        level.level = step;
        level.samplesPerPoint = step;

        int numPoints = (int)(totalSamples / step);
        if (numPoints < 1) continue;

        level.peaks.reserve(numPoints * 2); // Stores Min/Max pairs.
        level.rms.reserve(numPoints);

        for (int i = 0; i < numPoints; ++i) {
            float maxVal = -1.0f;
            float minVal = 1.0f;
            double sumSquares = 0.0;

            size_t startIdx = (size_t)i * (size_t)step;
            size_t chunkSize = (size_t)step;
            if (startIdx + chunkSize > totalSamples)
                chunkSize = totalSamples - startIdx;
            if (chunkSize == 0) continue;

            for (size_t j = 0; j < chunkSize; ++j) {
                float s = rawSamples[startIdx + j];
                if (s > maxVal) maxVal = s;
                if (s < minVal) minVal = s;
                sumSquares += (double)s * s;
            }

            // Store the minimum and maximum pair.
            level.peaks.push_back(minVal);
            level.peaks.push_back(maxVal);

            // Calculate root-mean-square amplitude.
            // $RMS = \sqrt{\frac{1}{n} \sum x_i^2}$
            level.rms.push_back((float)std::sqrt(sumSquares / (double)chunkSize));
        }
        mipmap.levels.push_back(std::move(level));
    }
    return mipmap;
}

// Serialize and store the waveform mipmap.
bool Database::saveWaveformMipmapByPath(const std::string& filePath, const WaveformMipmap& mipmap) {
    if (!m_db || mipmap.levels.empty()) return false;

    // Ensure waveforms_db already has a row for this file ID.
    const char* ensureSql = R"(
        INSERT OR IGNORE INTO waveforms_db.waveforms (file_id)
        SELECT id FROM main.audio_files WHERE file_path = ? LIMIT 1
    )";
    sqlite3_stmt* ensureStmt = nullptr;
    if (sqlite3_prepare_v2(m_db, ensureSql, -1, &ensureStmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(ensureStmt, 1, filePath.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(ensureStmt);
        sqlite3_finalize(ensureStmt);
    }

    // Pack every level into one binary BLOB.
    std::vector<uint8_t> payload;
    uint32_t numLevels = (uint32_t)mipmap.levels.size();
    AppendPod(payload, numLevels);
    AppendPod(payload, mipmap.totalSamples);

    for (const auto& lvl : mipmap.levels) {
        AppendPod(payload, (uint32_t)lvl.samplesPerPoint);
        uint32_t peakSize = (uint32_t)lvl.peaks.size();
        AppendPod(payload, peakSize);
        payload.insert(payload.end(), reinterpret_cast<const uint8_t*>(lvl.peaks.data()),
                      reinterpret_cast<const uint8_t*>(lvl.peaks.data() + peakSize));

        uint32_t rmsSize = (uint32_t)lvl.rms.size();
        AppendPod(payload, rmsSize);
        payload.insert(payload.end(), reinterpret_cast<const uint8_t*>(lvl.rms.data()),
                      reinterpret_cast<const uint8_t*>(lvl.rms.data() + rmsSize));
    }

    const char* sql = R"(
        UPDATE waveforms_db.waveforms
        SET mipmap_data = ?, updated_at = ?
        WHERE file_id = (SELECT id FROM main.audio_files WHERE file_path = ? LIMIT 1)
    )";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;

    sqlite3_bind_blob(stmt, 1, payload.data(), (int)payload.size(), SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 2, (int64_t)std::time(nullptr));
    sqlite3_bind_text(stmt, 3, filePath.c_str(), -1, SQLITE_TRANSIENT);

    bool ok = (sqlite3_step(stmt) == SQLITE_DONE);
    sqlite3_finalize(stmt);
    return ok;
}

// Read and deserialize a waveform mipmap.
bool Database::getWaveformMipmapByPath(const std::string& filePath, WaveformMipmap& mipmap) {
    mipmap.levels.clear();
    mipmap.totalSamples = 0;
    if (!m_db || filePath.empty()) return false;

    const char* sql = "SELECT mipmap_data FROM waveforms_db.waveforms "
                      "WHERE file_id = (SELECT id FROM main.audio_files WHERE file_path = ? LIMIT 1)";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(stmt, 1, filePath.c_str(), -1, SQLITE_TRANSIENT);

    bool found = false;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const void* blob = sqlite3_column_blob(stmt, 0);
        int size = sqlite3_column_bytes(stmt, 0);
        if (blob && size > 0) {
            const uint8_t* ptr = static_cast<const uint8_t*>(blob);
            std::vector<uint8_t> payload(ptr, ptr + size);
            size_t offset = 0;
            uint32_t numLevels = 0;
            if (ReadPod(payload, offset, numLevels) && ReadPod(payload, offset, mipmap.totalSamples)) {
                for (uint32_t i = 0; i < numLevels; ++i) {
                    WaveformMipmapLevel lvl;
                    uint32_t step = 0, pSize = 0, rSize = 0;
                    if (!ReadPod(payload, offset, step)) break;
                    lvl.level = (int)step;
                    lvl.samplesPerPoint = (int)step;

                    if (!ReadPod(payload, offset, pSize)) break;
                    size_t peakBytes = (size_t)pSize * sizeof(float);
                    if (offset + peakBytes > payload.size()) break;
                    lvl.peaks.resize(pSize);
                    std::memcpy(lvl.peaks.data(), payload.data() + offset, peakBytes);
                    offset += peakBytes;

                    if (!ReadPod(payload, offset, rSize)) break;
                    size_t rmsBytes = (size_t)rSize * sizeof(float);
                    if (offset + rmsBytes > payload.size()) break;
                    lvl.rms.resize(rSize);
                    std::memcpy(lvl.rms.data(), payload.data() + offset, rmsBytes);
                    offset += rmsBytes;

                    mipmap.levels.push_back(std::move(lvl));
                }
                found = !mipmap.levels.empty();
            }
        }
    }
    sqlite3_finalize(stmt);
    return found;
}

// Select the mipmap level closest to the current pixel density.
std::vector<float> Database::getWaveformForZoom(const std::string& filePath, float zoomLevel, int displayWidth) {
    WaveformMipmap mipmap;
    if (!getWaveformMipmapByPath(filePath, mipmap)) return {};

    if (mipmap.totalSamples == 0 || displayWidth <= 0 || zoomLevel <= 0.0f) return {};

    // 核心公式：计算当前每像素代表多少个原始采样点
    // samplesPerPixel = total samples / (display width * zoom).
    float samplesPerPixel = (float)mipmap.totalSamples / ((float)displayWidth * zoomLevel);

    // Choose the nearest level no coarser than samplesPerPixel.
    const WaveformMipmapLevel* bestLevel = nullptr;
    for (const auto& lvl : mipmap.levels) {
        // A level at or below samplesPerPixel retains enough detail.
        if ((float)lvl.samplesPerPoint <= samplesPerPixel) {
            bestLevel = &lvl;
        } else {
            // 后续层级步进更大（更粗糙），停止搜索
            break;
        }
    }

    // If every level is too coarse, fall back to the finest level.
    if (!bestLevel && !mipmap.levels.empty()) bestLevel = &mipmap.levels.front();

    return bestLevel ? bestLevel->peaks : std::vector<float>();
}

// Returns playlist file count through the indexed membership table.
int64_t Database::getPlaylistFileCount(int64_t playlistId) {
    if (!m_db) return 0;
    const char* sql = "SELECT COUNT(*) FROM playlist_items WHERE playlist_id = ?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) return 0;
    sqlite3_bind_int64(stmt, 1, playlistId);
    int64_t count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) count = sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
    return count;
}

// Playlist export (F-017)

bool Database::exportPlaylist(int64_t playlistId, const std::string& outputPath, PlaylistExportFormat fmt) {
    auto files = getPlaylistFiles(playlistId);
    if (files.empty()) return false;

    std::ostringstream content;

    switch (fmt) {
    case PlaylistExportFormat::M3U:
        content << "#EXTM3U\n";
        for (const auto& f : files) {
            content << "#EXTINF:" << (int)f.durationSeconds << "," << f.fileName << "\n";
            content << f.filePath << "\n";
        }
        break;
    case PlaylistExportFormat::CSV:
        content << "FilePath,FileName,Duration,SampleRate,Channels,Format,Category,Description\n";
        for (const auto& f : files) {
            // Simple CSV escaping (wrap in quotes if contains comma)
            auto esc = [](const std::string& s) -> std::string {
                if (s.find(',') != std::string::npos || s.find('"') != std::string::npos) {
                    std::string r = "\"";
                    for (char c : s) { if (c == '"') r += "\"\""; else r += c; }
                    r += "\"";
                    return r;
                }
                return s;
            };
            content << esc(f.filePath) << "," << esc(f.fileName) << ","
                    << f.durationSeconds << "," << f.sampleRate << ","
                    << f.channels << "," << esc(f.format) << ","
                    << esc(f.category) << "," << esc(f.description) << "\n";
        }
        break;
    case PlaylistExportFormat::JSON: {
        // JSON string escaping (backslash, quotes, control chars)
        auto jsonEsc = [](const std::string& s) -> std::string {
            std::string r;
            r.reserve(s.size() + 8);
            for (char c : s) {
                switch (c) {
                case '"':  r += "\\\""; break;
                case '\\': r += "\\\\"; break;
                case '\n': r += "\\n"; break;
                case '\r': r += "\\r"; break;
                case '\t': r += "\\t"; break;
                default:   r += c; break;
                }
            }
            return r;
        };
        content << "[\n";
        for (size_t i = 0; i < files.size(); ++i) {
            const auto& f = files[i];
            content << "  {\"path\":\"" << jsonEsc(f.filePath) << "\",\"name\":\"" << jsonEsc(f.fileName)
                    << "\",\"duration\":" << f.durationSeconds
                    << ",\"sampleRate\":" << f.sampleRate << "}";
            if (i + 1 < files.size()) content << ",";
            content << "\n";
        }
        content << "]\n";
        break;
    } // case JSON
    } // switch

    return Platform::FileSystem::WriteFileText(outputPath, content.str());
}

int64_t Database::createSmartPlaylist(const std::string& name, const std::string& filterJson) {
    if (!m_db || name.empty()) return 0;
    // Ensure playlists table has filter_json column
    ensureColumn(m_db, "playlists", "filter_json", "TEXT");
    ensureColumn(m_db, "playlists", "is_smart", "INTEGER DEFAULT 0");

    const char* sql = "INSERT INTO playlists (name, description, date_created, is_smart, filter_json) VALUES (?, '', ?, 1, ?)";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) return 0;
    sqlite3_bind_text(stmt, 1, name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 2, getCurrentTimestamp());
    sqlite3_bind_text(stmt, 3, filterJson.c_str(), -1, SQLITE_TRANSIENT);
    bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    if (!ok) return 0;
    bumpDataRevision();
    return sqlite3_last_insert_rowid(m_db);
}

std::string Database::getSmartPlaylistFilter(int64_t playlistId) {
    if (!m_db || playlistId <= 0) return {};
    const char* sql = "SELECT filter_json FROM playlists WHERE id = ? AND is_smart = 1";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) return {};
    sqlite3_bind_int64(stmt, 1, playlistId);
    std::string result;
    if (sqlite3_step(stmt) == SQLITE_ROW) result = safeGetText(stmt, 0);
    sqlite3_finalize(stmt);
    return result;
}

// Tag operations: batched transactions with covering indexes.

int64_t Database::getOrCreateTagId(const std::string& tagName) {
    if (!m_db || tagName.empty()) return 0;

    const char* selectSql = "SELECT id FROM tags WHERE name = ? LIMIT 1;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, selectSql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, tagName.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            int64_t id = sqlite3_column_int64(stmt, 0);
            sqlite3_finalize(stmt);
            return id;
        }
    }
    if (stmt) sqlite3_finalize(stmt);

    const char* insertSql = "INSERT OR IGNORE INTO tags (name) VALUES (?);";
    if (sqlite3_prepare_v2(m_db, insertSql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, tagName.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(stmt);
        int64_t id = (sqlite3_changes(m_db) > 0) ? sqlite3_last_insert_rowid(m_db) : 0;
        sqlite3_finalize(stmt);
        if (id > 0) return id;
        // A concurrent INSERT may have been ignored by UNIQUE; query again.
        if (sqlite3_prepare_v2(m_db, selectSql, -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, tagName.c_str(), -1, SQLITE_TRANSIENT);
            if (sqlite3_step(stmt) == SQLITE_ROW) id = sqlite3_column_int64(stmt, 0);
            sqlite3_finalize(stmt);
        }
        return id;
    }
    return 0;
}

bool Database::setFileTagsInTransaction(int64_t fileId, const std::vector<std::string>& tagNames) {
    if (!m_db || fileId <= 0) return false;

    std::string deleteSql = "DELETE FROM file_tags WHERE file_id = " + std::to_string(fileId);
    if (!executeSQL(deleteSql)) return false;

    const char* insertSql = "INSERT OR IGNORE INTO file_tags (file_id, tag_id) VALUES (?, ?);";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, insertSql, -1, &stmt, nullptr) == SQLITE_OK) {
        for (const auto& name : tagNames) {
            if (name.empty()) continue;
            int64_t tagId = getOrCreateTagId(name);
            if (tagId > 0) {
                sqlite3_reset(stmt);
                sqlite3_clear_bindings(stmt);
                sqlite3_bind_int64(stmt, 1, fileId);
                sqlite3_bind_int64(stmt, 2, tagId);
                sqlite3_step(stmt);
            }
        }
        sqlite3_finalize(stmt);
    }
    return true;
}

bool Database::setFileTags(int64_t fileId, const std::vector<std::string>& tagNames) {
    if (!m_db || fileId <= 0) return false;
    const AudioRecord rec = getFileById(fileId);
    if (!rec.filePath.empty() && Platform::FileSystem::IsNetworkPath(rec.filePath)) {
        m_lastError = L(Write_BlockedNetworkLibrary);
        return false;
    }
    if (!beginTransaction()) return false;
    bool ok = setFileTagsInTransaction(fileId, tagNames);
    if (!ok) {
        rollbackTransaction();
        return false;
    }
    if (!commitTransaction()) return false;
    bumpDataRevision();  // T-295
    return true;
}

std::vector<std::string> Database::getFileTags(int64_t fileId) {
    std::vector<std::string> results;
    if (!m_db || fileId <= 0) return results;

    const char* sql = "SELECT t.name FROM tags t "
                      "JOIN file_tags ft ON t.id = ft.tag_id "
                      "WHERE ft.file_id = ? ORDER BY t.name;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(stmt, 1, fileId);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            results.push_back(safeGetText(stmt, 0));
        }
        sqlite3_finalize(stmt);
    }
    return results;
}

int Database::rebuildAllTagsFromKeywords(std::function<void(int, int)> progressCallback) {
    if (!m_db) return 0;

    int64_t totalFiles = getFileCount();
    int processed = 0;
    int tagsCreated = 0;
    const int batchSize = 10000;

    for (int64_t offset = 0; offset < totalFiles; offset += batchSize) {
        std::string sql = "SELECT id, keywords FROM audio_files "
                          "WHERE keywords IS NOT NULL AND keywords <> '' "
                          "LIMIT " + std::to_string(batchSize) + " OFFSET " + std::to_string(offset);
        SqlResult res = executeRawQuery(sql);
        if (!res.success) continue;
        if (res.rows.empty()) break;

        if (!beginTransaction()) continue;
        for (const auto& row : res.rows) {
            if (row.size() < 2) continue;
            int64_t fileId = 0;
            try {
                fileId = std::stoll(row[0]);
            } catch (...) {
                continue;
            }
            std::string kw = row[1];
            std::vector<std::string> tags;
            const char* sep = ",; \t\n\r";
            for (size_t i = 0; i < kw.size(); ) {
                size_t j = kw.find_first_of(sep, i);
                if (j == i) { ++i; continue; }
                if (j == std::string::npos) {
                    if (i < kw.size()) tags.push_back(kw.substr(i));
                    break;
                }
                if (j > i) tags.push_back(kw.substr(i, j - i));
                i = j + 1;
            }
            if (!tags.empty()) {
                setFileTagsInTransaction(fileId, tags);
                tagsCreated += (int)tags.size();
            }
            processed++;
        }
        commitTransaction();
        if (progressCallback) progressCallback(processed, (int)totalFiles);
    }

    executeSQL("DELETE FROM tags WHERE id NOT IN (SELECT DISTINCT tag_id FROM file_tags)");
    return tagsCreated;
}

static std::string TrimTopTagToken(std::string token) {
    auto begin = token.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) return {};
    auto end = token.find_last_not_of(" \t\r\n");
    token = token.substr(begin, end - begin + 1);
    while (!token.empty() && (token.back() == '"' || token.back() == '\'')) token.pop_back();
    while (!token.empty() && (token.front() == '"' || token.front() == '\'')) token.erase(token.begin());
    return token;
}

static std::string NormalizeTopTagKey(const std::string& token) {
    std::string key = token;
    std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return key;
}

static bool IsUsableTopTagToken(const std::string& token) {
    if (token.size() < 2 || token.size() > 64) return false;
    bool anyAlpha = false;
    for (unsigned char c : token) {
        if (std::isalpha(c) || (c & 0x80)) {
            anyAlpha = true;
            break;
        }
    }
    return anyAlpha;
}

static void AddTopTagToken(std::unordered_map<std::string, int>& counts,
                           std::unordered_map<std::string, std::string>& displayByKey,
                           std::unordered_set<std::string>& rowSeen,
                           const std::string& rawToken) {
    std::string token = TrimTopTagToken(rawToken);
    if (!IsUsableTopTagToken(token)) return;
    std::string key = NormalizeTopTagKey(token);
    if (key.empty() || !rowSeen.insert(key).second) return;
    displayByKey.emplace(key, token);
    counts[key]++;
}

static void AddDelimitedTopTags(std::unordered_map<std::string, int>& counts,
                                std::unordered_map<std::string, std::string>& displayByKey,
                                std::unordered_set<std::string>& rowSeen,
                                const std::string& text,
                                const char* separators) {
    size_t pos = 0;
    while (pos < text.size()) {
        size_t end = text.find_first_of(separators, pos);
        if (end == std::string::npos) end = text.size();
        AddTopTagToken(counts, displayByKey, rowSeen, text.substr(pos, end - pos));
        pos = end + 1;
    }
}

std::vector<std::pair<std::string, int>> Database::getTopTags(int limit) {
    std::vector<std::pair<std::string, int>> results;
    if (!m_db || limit <= 0) return results;

    const char* sql = "SELECT t.name, COUNT(ft.file_id) AS cnt FROM tags t "
                      "JOIN file_tags ft ON t.id = ft.tag_id "
                      "GROUP BY t.id ORDER BY cnt DESC LIMIT ?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) return results;
    sqlite3_bind_int(stmt, 1, limit);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        results.emplace_back(safeGetText(stmt, 0), (int)sqlite3_column_int(stmt, 1));
    }
    sqlite3_finalize(stmt);
    if (!results.empty()) return results;

    // Some libraries have rich indexed metadata but have not materialized the
    // optional F-007 tags/file_tags relation table. Keep the TagCloud useful
    // without mutating the user's DB or starting a rebuild job.
    constexpr int kFallbackMaxRows = 20000;
    const int fallbackRows = std::clamp(limit * 500, 1000, kFallbackMaxRows);
    const char* fallbackSql =
        "SELECT keywords, category, norm_genre, norm_instruments, norm_mood_tags "
        "FROM audio_files "
        "WHERE (keywords IS NOT NULL AND keywords <> '') "
        "   OR (category IS NOT NULL AND category <> '') "
        "   OR (norm_genre IS NOT NULL AND norm_genre <> '') "
        "   OR (norm_instruments IS NOT NULL AND norm_instruments <> '') "
        "   OR (norm_mood_tags IS NOT NULL AND norm_mood_tags <> '') "
        "LIMIT ?;";
    stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, fallbackSql, -1, &stmt, nullptr) != SQLITE_OK) return results;
    sqlite3_bind_int(stmt, 1, fallbackRows);

    std::unordered_map<std::string, int> counts;
    std::unordered_map<std::string, std::string> displayByKey;
    counts.reserve((size_t)limit * 8);
    displayByKey.reserve((size_t)limit * 8);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        std::unordered_set<std::string> rowSeen;
        rowSeen.reserve(16);
        AddDelimitedTopTags(counts, displayByKey, rowSeen, safeGetText(stmt, 0), ",;\t\r\n");
        AddDelimitedTopTags(counts, displayByKey, rowSeen, safeGetText(stmt, 1), ",;\t\r\n");
        AddDelimitedTopTags(counts, displayByKey, rowSeen, safeGetText(stmt, 2), ",;\t\r\n");
        AddDelimitedTopTags(counts, displayByKey, rowSeen, safeGetText(stmt, 3), ",;\t\r\n");
        AddDelimitedTopTags(counts, displayByKey, rowSeen, safeGetText(stmt, 4), ",;\t\r\n");
    }
    sqlite3_finalize(stmt);

    results.reserve(counts.size());
    for (const auto& kv : counts) {
        auto displayIt = displayByKey.find(kv.first);
        results.emplace_back(displayIt != displayByKey.end() ? displayIt->second : kv.first, kv.second);
    }
    std::sort(results.begin(), results.end(), [](const auto& a, const auto& b) {
        if (a.second != b.second) return a.second > b.second;
        return a.first < b.first;
    });
    if ((int)results.size() > limit) results.resize((size_t)limit);
    return results;
}

// Batch tag operations (F-007)

bool Database::addTagToFile(int64_t fileId, const std::string& tagName) {
    if (!m_db || tagName.empty() || fileId <= 0) return false;
    int64_t tagId = getOrCreateTagId(tagName);
    if (tagId <= 0) return false;
    const char* sql = "INSERT OR IGNORE INTO file_tags (file_id, tag_id) VALUES (?, ?)";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_int64(stmt, 1, fileId);
    sqlite3_bind_int64(stmt, 2, tagId);
    bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    if (ok) bumpDataRevision();  // T-295
    return ok;
}

bool Database::removeTagFromFile(int64_t fileId, const std::string& tagName) {
    if (!m_db || tagName.empty() || fileId <= 0) return false;
    const char* sql =
        "DELETE FROM file_tags WHERE file_id = ? AND tag_id = "
        "(SELECT id FROM tags WHERE name = ? LIMIT 1)";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_int64(stmt, 1, fileId);
    sqlite3_bind_text(stmt, 2, tagName.c_str(), -1, SQLITE_TRANSIENT);
    bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    if (ok) bumpDataRevision();  // T-295
    return ok;
}

int Database::addTagToFiles(const std::vector<int64_t>& fileIds, const std::string& tagName) {
    if (!m_db || tagName.empty() || fileIds.empty()) return 0;
    int64_t tagId = getOrCreateTagId(tagName);
    if (tagId <= 0) return 0;

    const bool ownTxn = sqlite3_get_autocommit(m_db) != 0;
    if (ownTxn) beginTransaction();

    const char* sql = "INSERT OR IGNORE INTO file_tags (file_id, tag_id) VALUES (?, ?)";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        if (ownTxn) rollbackTransaction();
        return 0;
    }
    int count = 0;
    for (int64_t fid : fileIds) {
        if (fid <= 0) continue;
        sqlite3_reset(stmt);
        sqlite3_bind_int64(stmt, 1, fid);
        sqlite3_bind_int64(stmt, 2, tagId);
        if (sqlite3_step(stmt) == SQLITE_DONE) count++;
    }
    sqlite3_finalize(stmt);
    if (ownTxn) commitTransaction();
    if (count > 0) bumpDataRevision();  // T-295
    return count;
}

int Database::removeTagFromFiles(const std::vector<int64_t>& fileIds, const std::string& tagName) {
    if (!m_db || tagName.empty() || fileIds.empty()) return 0;

    // Look up tag ID first
    const char* idSql = "SELECT id FROM tags WHERE name = ? LIMIT 1";
    sqlite3_stmt* idStmt = nullptr;
    if (sqlite3_prepare_v2(m_db, idSql, -1, &idStmt, nullptr) != SQLITE_OK) return 0;
    sqlite3_bind_text(idStmt, 1, tagName.c_str(), -1, SQLITE_TRANSIENT);
    int64_t tagId = 0;
    if (sqlite3_step(idStmt) == SQLITE_ROW) tagId = sqlite3_column_int64(idStmt, 0);
    sqlite3_finalize(idStmt);
    if (tagId <= 0) return 0;

    const bool ownTxn = sqlite3_get_autocommit(m_db) != 0;
    if (ownTxn) beginTransaction();

    const char* sql = "DELETE FROM file_tags WHERE file_id = ? AND tag_id = ?";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        if (ownTxn) rollbackTransaction();
        return 0;
    }
    int count = 0;
    for (int64_t fid : fileIds) {
        if (fid <= 0) continue;
        sqlite3_reset(stmt);
        sqlite3_bind_int64(stmt, 1, fid);
        sqlite3_bind_int64(stmt, 2, tagId);
        if (sqlite3_step(stmt) == SQLITE_DONE && sqlite3_changes(m_db) > 0) count++;
    }
    sqlite3_finalize(stmt);
    if (ownTxn) commitTransaction();
    if (count > 0) bumpDataRevision();  // T-295
    return count;
}

// Color labels (F-007)

bool Database::setColorMark(int64_t fileId, const std::string& color) {
    if (!m_db || fileId <= 0) return false;
    MetadataEditPatch patch;
    patch.set("track_colour", color);
    return applyMetadataPatch(fileId, patch);
}

bool Database::setColorMarkBatch(const std::vector<int64_t>& fileIds, const std::string& color) {
    if (!m_db || fileIds.empty()) return false;
    MetadataEditPatch patch;
    patch.set("track_colour", color);
    auto result = applyMetadataPatchBatch(fileIds, patch);
    return result.failed == 0;
}

std::string Database::getColorMark(int64_t fileId) {
    if (!m_db || fileId <= 0) return {};
    const char* sql = "SELECT track_colour FROM audio_files WHERE id = ?";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) return {};
    sqlite3_bind_int64(stmt, 1, fileId);
    std::string result;
    if (sqlite3_step(stmt) == SQLITE_ROW) result = safeGetText(stmt, 0);
    sqlite3_finalize(stmt);
    return result;
}

// Performance-mode accessors.
void Database::setSettings(AppSettings* settings) {
    m_settings = settings;
}

int Database::getMemoryProfile() const {
    if (!m_settings) return 1; // 默认 Balanced
    return m_settings->memoryProfile;
}

int Database::getThumbnailMaxResolution() const {
    if (!m_settings) return 512; // 默认 512px
    return m_settings->thumbnailMaxResolution;
}

// SQLite prepared-statement pool.

sqlite3_stmt* Database::checkoutStmt(const std::string& sql) {
    if (!m_db) return nullptr;
    {
        std::lock_guard<std::mutex> lock(m_stmtMutex);
        auto& pool = m_stmtPool[sql];
        if (!pool.empty()) {
            sqlite3_stmt* stmt = pool.back();
            pool.pop_back();
            // Reset the pooled statement and clear old bindings before reuse.
            sqlite3_reset(stmt);
            sqlite3_clear_bindings(stmt);
            return stmt;
        }
    }
    // The pool is empty; prepare a new statement.
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(m_db, sql.c_str(), -1, &stmt, nullptr);
    return stmt;
}

void Database::returnStmt(const std::string& sql, sqlite3_stmt* stmt) {
    if (!stmt) return;
    // Reset before returning to the pool so incomplete readers release locks.
    sqlite3_reset(stmt);
    std::lock_guard<std::mutex> lock(m_stmtMutex);
    m_stmtPool[sql].push_back(stmt);
}

void Database::clearStmtPool() {
    std::lock_guard<std::mutex> lock(m_stmtMutex);
    for (auto& kv : m_stmtPool) {
        for (auto* stmt : kv.second) {
            sqlite3_finalize(stmt);
        }
    }
    m_stmtPool.clear();
}

// Database package management

namespace {
struct DatabasePackagePathsForRepair {
    fs::path mainPath;
    fs::path blobsPath;
    fs::path waveformsPath;
};

DatabasePackagePathsForRepair BuildDatabasePackagePathsForRepair(const std::string& mainDbPathUtf8) {
    DatabasePackagePathsForRepair paths;
    paths.mainPath = Utf8ToPath(mainDbPathUtf8);
    const fs::path parentDir = paths.mainPath.parent_path();
    const std::string fileName = toLowerAsciiCopy(PathToUtf8(paths.mainPath.filename()));
    if (fileName == "main.db") {
        paths.blobsPath = parentDir / "blobs.db";
        paths.waveformsPath = parentDir / "waveforms.db";
    } else {
        const std::string stem = PathToUtf8(paths.mainPath.stem());
        paths.blobsPath = parentDir / Utf8ToPath(stem + "_blobs.db");
        paths.waveformsPath = parentDir / Utf8ToPath(stem + "_waveforms.db");
    }
    return paths;
}

uint64_t FileSizeIfExistsForRepair(const fs::path& p) {
    const auto info = Platform::FileSystem::ProbeRegularFile(PathToUtf8(p));
    return info.isRegularFile && info.sizeAvailable ? info.sizeBytes : 0;
}

void AddSqlitePackageMetricsForRepair(const fs::path& p,
                                      uint64_t& dbBytes,
                                      RepairCenterSnapshot& out) {
    dbBytes = FileSizeIfExistsForRepair(p);
    const uint64_t walBytes =
        FileSizeIfExistsForRepair(p.parent_path() / (p.filename().string() + "-wal"));
    const uint64_t shmBytes =
        FileSizeIfExistsForRepair(p.parent_path() / (p.filename().string() + "-shm"));
    out.packageBytes += dbBytes + walBytes + shmBytes;
    out.walBytes += walBytes;
    out.shmBytes += shmBytes;
}

uint64_t ScalarPragmaUint64(sqlite3* db, const char* sql) {
    sqlite3_stmt* stmt = nullptr;
    uint64_t value = 0;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            value = static_cast<uint64_t>(sqlite3_column_int64(stmt, 0));
        }
    }
    if (stmt) sqlite3_finalize(stmt);
    return value;
}

bool TruncateSqliteFileToLivePages(const fs::path& path,
                                   uint64_t pageSize,
                                   uint64_t pageCount) {
    if (path.empty() || pageSize == 0 || pageCount == 0) return false;
    const uint64_t wantedBytes = pageSize * pageCount;
    const uint64_t currentBytes = FileSizeIfExistsForRepair(path);
    if (currentBytes <= wantedBytes) return true;
    return Platform::FileSystem::ResizeFile(PathToUtf8(path), wantedBytes);
}

bool AttachReadOnlyDatabase(sqlite3* db, const fs::path& path, const char* schemaName) {
    if (!db || path.empty() || !schemaName || !*schemaName) return false;
    const std::string utf8Path = PathToUtf8(path);
    if (Platform::FileSystem::ProbePath(utf8Path) !=
        Platform::FileSystem::PathStatus::Present) return false;
    sqlite3_stmt* stmt = nullptr;
    std::string sql = std::string("ATTACH DATABASE ? AS ") + schemaName;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }
    sqlite3_bind_text(stmt, 1, utf8Path.c_str(), -1, SQLITE_TRANSIENT);
    const bool ok = (sqlite3_step(stmt) == SQLITE_DONE);
    sqlite3_finalize(stmt);
    return ok;
}

uint64_t LogicalSqliteBytes(sqlite3* db, const char* schemaName) {
    if (!db || !schemaName || !*schemaName) return 0;
    const std::string prefix = std::string("PRAGMA ") + schemaName + ".";
    const uint64_t pageSize = ScalarPragmaUint64(db, (prefix + "page_size;").c_str());
    const uint64_t pageCount = ScalarPragmaUint64(db, (prefix + "page_count;").c_str());
    const uint64_t freeCount = ScalarPragmaUint64(db, (prefix + "freelist_count;").c_str());
    if (pageSize == 0 || pageCount == 0) return 0;
    const uint64_t usedPages = pageCount > freeCount ? pageCount - freeCount : 0;
    return pageSize * usedPages;
}
}  // namespace

RepairCenterSnapshot Database::InspectDatabasePackageForRepair(
    const std::string& mainDbPathUtf8,
    uint64_t walWarningBytes) {
    RepairCenterSnapshot out;
    if (mainDbPathUtf8.empty()) return out;
    try {
        const DatabasePackagePathsForRepair paths =
            BuildDatabasePackagePathsForRepair(mainDbPathUtf8);
        AddSqlitePackageMetricsForRepair(paths.mainPath, out.mainDbBytes, out);
        AddSqlitePackageMetricsForRepair(paths.blobsPath, out.blobsDbBytes, out);
        AddSqlitePackageMetricsForRepair(paths.waveformsPath, out.waveformsDbBytes, out);
        if (out.walBytes > walWarningBytes) {
            out.walSizeWarning = true;
            out.needsAttention = true;
            out.warnings.push_back("Large SQLite WAL package: " +
                                   std::to_string(out.walBytes) + " bytes");
        }
    } catch (...) {}
    return out;
}

uint64_t Database::GetDatabasePackageSize(const std::string& mainDbPathUtf8) {
    return InspectDatabasePackageForRepair(
        mainDbPathUtf8,
        (std::numeric_limits<uint64_t>::max)()).packageBytes;
}

uint64_t Database::GetDatabaseEffectiveSize(const std::string& mainDbPathUtf8) {
    if (mainDbPathUtf8.empty()) return 0;
    const fs::path mainPath = Utf8ToPath(mainDbPathUtf8);
    if (Platform::FileSystem::ProbePath(mainDbPathUtf8) !=
        Platform::FileSystem::PathStatus::Present) return 0;

    sqlite3* db = nullptr;
#ifdef _WIN32
    const std::wstring widePath = mainPath.wstring();
    if (sqlite3_open16(widePath.c_str(), &db) != SQLITE_OK) {
#else
    if (sqlite3_open(mainDbPathUtf8.c_str(), &db) != SQLITE_OK) {
#endif
        if (db) sqlite3_close(db);
        return GetDatabasePackageSize(mainDbPathUtf8);
    }

    sqlite3_exec(db, "PRAGMA query_only=ON;", nullptr, nullptr, nullptr);
    const DatabasePackagePathsForRepair paths = BuildDatabasePackagePathsForRepair(mainDbPathUtf8);
    const bool blobsAttached = AttachReadOnlyDatabase(db, paths.blobsPath, "blobs_db");
    const bool waveformsAttached = AttachReadOnlyDatabase(db, paths.waveformsPath, "waveforms_db");

    uint64_t total = LogicalSqliteBytes(db, "main");
    if (blobsAttached) total += LogicalSqliteBytes(db, "blobs_db");
    if (waveformsAttached) total += LogicalSqliteBytes(db, "waveforms_db");
    sqlite3_close(db);

    return total > 0 ? total : GetDatabasePackageSize(mainDbPathUtf8);
}

Database::CacheSizeReport Database::getCacheSizeReport() {
    CacheSizeReport report;
    if (!m_db) return report;

    const RepairCenterSnapshot package =
        InspectDatabasePackageForRepair(m_mainDbPath, (std::numeric_limits<uint64_t>::max)());
    report.packageBytes = package.packageBytes;
    report.mainDbBytes = package.mainDbBytes;
    report.blobsDbBytes = package.blobsDbBytes;
    report.waveformsDbBytes = package.waveformsDbBytes;
    report.walBytes = package.walBytes;
    report.shmBytes = package.shmBytes;

    auto scalar = [this](const char* sql) -> int64_t {
        int64_t value = 0;
        if (!strictScalarInt64(m_db, sql, value)) return 0;
        return value;
    };

    report.mainPageCount = scalar("PRAGMA main.page_count;");
    report.mainFreelistCount = scalar("PRAGMA main.freelist_count;");
    report.blobsPageCount = scalar("PRAGMA blobs_db.page_count;");
    report.blobsFreelistCount = scalar("PRAGMA blobs_db.freelist_count;");
    report.waveformsPageCount = scalar("PRAGMA waveforms_db.page_count;");
    report.waveformsFreelistCount = scalar("PRAGMA waveforms_db.freelist_count;");
    report.miniWaveformBytes = scalar(
        "SELECT COALESCE(SUM(length(waveform_data)), 0) FROM waveforms_db.waveforms;");
    report.detailWaveformBytes = scalar(
        "SELECT COALESCE(SUM(length(waveform_detail)), 0) + "
        "       COALESCE(SUM(length(waveform_min_detail)), 0) "
        "FROM waveforms_db.waveforms;");
    report.coverThumbBytes = scalar(
        "SELECT COALESCE(SUM(length(thumb_data)), 0) FROM blobs_db.cover_thumbs;");
    report.ftsDataBytes = scalar(
        "SELECT COALESCE(SUM(length(block)), 0) FROM audio_files_fts_data;");
    report.trigramDataBytes = scalar(
        "SELECT COALESCE(SUM(length(block)), 0) FROM audio_files_name_trigram_data;");
    return report;
}

bool Database::compactCachePackage(std::string& outError) {
    outError.clear();
    if (!m_db) {
        outError = "Database not open";
        return false;
    }
    std::lock_guard<std::mutex> writerLk(m_writerSerializeMutex);
    if (sqlite3_get_autocommit(m_db) == 0) {
        outError = "Refusing to compact while a transaction is active";
        return false;
    }
    clearStmtPool();

    const char* steps[] = {
        "PRAGMA main.wal_checkpoint(TRUNCATE);",
        "PRAGMA blobs_db.wal_checkpoint(TRUNCATE);",
        "PRAGMA waveforms_db.wal_checkpoint(TRUNCATE);",
        "VACUUM main;",
        "VACUUM blobs_db;",
        "VACUUM waveforms_db;",
        "PRAGMA main.journal_mode=DELETE;",
        "PRAGMA blobs_db.journal_mode=DELETE;",
        "PRAGMA waveforms_db.journal_mode=DELETE;",
        "PRAGMA main.journal_mode=WAL;",
        "PRAGMA blobs_db.journal_mode=WAL;",
        "PRAGMA waveforms_db.journal_mode=WAL;",
        "PRAGMA main.wal_checkpoint(TRUNCATE);",
        "PRAGMA blobs_db.wal_checkpoint(TRUNCATE);",
        "PRAGMA waveforms_db.wal_checkpoint(TRUNCATE);",
    };
    for (const char* sql : steps) {
        if (!executeSQL(sql)) {
            outError = m_lastError.empty() ? std::string("Compact failed: ") + sql : m_lastError;
            return false;
        }
    }
    const DatabasePackagePathsForRepair paths =
        BuildDatabasePackagePathsForRepair(m_mainDbPath);
    const uint64_t mainPageSize = ScalarPragmaUint64(m_db, "PRAGMA main.page_size;");
    const uint64_t mainPageCount = ScalarPragmaUint64(m_db, "PRAGMA main.page_count;");
    const uint64_t blobsPageSize = ScalarPragmaUint64(m_db, "PRAGMA blobs_db.page_size;");
    const uint64_t blobsPageCount = ScalarPragmaUint64(m_db, "PRAGMA blobs_db.page_count;");
    const uint64_t waveformsPageSize = ScalarPragmaUint64(m_db, "PRAGMA waveforms_db.page_size;");
    const uint64_t waveformsPageCount = ScalarPragmaUint64(m_db, "PRAGMA waveforms_db.page_count;");
    // Attached SQLite databases can report the compacted page_count while the
    // Windows file still keeps stale tail pages. Trim the physical package so
    // the library manager shows effective cache size immediately after cleanup.
    const bool truncated =
        TruncateSqliteFileToLivePages(paths.mainPath, mainPageSize, mainPageCount) &&
        TruncateSqliteFileToLivePages(paths.blobsPath, blobsPageSize, blobsPageCount) &&
        TruncateSqliteFileToLivePages(paths.waveformsPath, waveformsPageSize, waveformsPageCount);
    if (!truncated) {
        DIAG_ACTION("Database", "CompactCachePackage physical truncate skipped/failed");
    }
    return true;
}

bool Database::compactCachePackageBestEffort(const char* reason) {
    std::string error;
    const bool ok = compactCachePackage(error);
    if (ok) {
        DIAG_ACTION_FMT("Database",
                        "AutoCompactCachePackage: reason=%s ok=1",
                        reason ? reason : "");
    } else {
        DIAG_ACTION_FMT("Database",
                        "AutoCompactCachePackage: reason=%s ok=0 error=%s",
                        reason ? reason : "",
                        error.c_str());
    }
    return ok;
}

bool Database::checkpointCachePackageBestEffort(const char* reason) {
    if (!m_db) return false;
    std::lock_guard<std::mutex> writerLk(m_writerSerializeMutex);
    if (sqlite3_get_autocommit(m_db) == 0) {
        DIAG_ACTION_FMT("Database",
                        "AutoCheckpointCachePackage: reason=%s ok=0 error=transaction_active",
                        reason ? reason : "");
        return false;
    }
    clearStmtPool();

    const char* steps[] = {
        "PRAGMA main.wal_checkpoint(TRUNCATE);",
        "PRAGMA blobs_db.wal_checkpoint(TRUNCATE);",
        "PRAGMA waveforms_db.wal_checkpoint(TRUNCATE);",
    };
    for (const char* sql : steps) {
        if (!executeSQL(sql)) {
            DIAG_ACTION_FMT("Database",
                            "AutoCheckpointCachePackage: reason=%s ok=0 error=%s",
                            reason ? reason : "",
                            m_lastError.c_str());
            return false;
        }
    }

    DIAG_ACTION_FMT("Database",
                    "AutoCheckpointCachePackage: reason=%s ok=1",
                    reason ? reason : "");
    return true;
}

bool Database::DestroyDatabasePackage(const std::string& mainDbPathUtf8, std::string& outError) {
    outError.clear();
    if (mainDbPathUtf8.empty()) {
        outError = u8"Database path is empty.";
        return false;
    }
    try {
        fs::path mainPath = Utf8ToPath(mainDbPathUtf8);
        fs::path parentDir = mainPath.parent_path();
        std::string fileName = toLowerAsciiCopy(PathToUtf8(mainPath.filename()));
        bool isPackageDir = (fileName == "main.db");  // package layout: parentDir/main.db
        std::string blobsPath, wavesPath;
        if (isPackageDir) {
            blobsPath = PathToUtf8(parentDir / "blobs.db");
            wavesPath = PathToUtf8(parentDir / "waveforms.db");
        } else {
            std::string stem = PathToUtf8(mainPath.stem());
            blobsPath = PathToUtf8(parentDir / Utf8ToPath(stem + "_blobs.db"));
            wavesPath = PathToUtf8(parentDir / Utf8ToPath(stem + "_waveforms.db"));
        }

        std::string firstFailure;
        auto removeSqliteSidecarsBestEffort = [](const fs::path& p) {
            // Also remove WAL/SHM sidecars (SQLite uses dash: main.db-wal, main.db-shm).
            // Sidecar failures are non-critical -- best-effort only.
            for (const char* suffix : {"-wal", "-shm"}) {
                fs::path sidecar = p.parent_path() / (p.filename().string() + suffix);
                if (Platform::FileSystem::ProbePath(PathToUtf8(sidecar)) ==
                    Platform::FileSystem::PathStatus::Present) {
                    Platform::FileSystem::RemoveFile(PathToUtf8(sidecar));
                }
            }
        };
        auto removePackageFile = [&firstFailure, &removeSqliteSidecarsBestEffort](
                                      const std::string& pathUtf8) {
            if (Platform::FileSystem::ProbePath(pathUtf8) !=
                Platform::FileSystem::PathStatus::Present) {
                return;
            }
            std::string error;
            for (int attempt = 0; attempt < 3; ++attempt) {
                error.clear();
                if (Platform::FileSystem::RemoveFile(pathUtf8, &error) || error.empty()) {
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
            }
            if (!error.empty() && firstFailure.empty()) {
                firstFailure = pathUtf8 + ": " + error;
            }
            removeSqliteSidecarsBestEffort(Utf8ToPath(pathUtf8));
        };
        removePackageFile(mainDbPathUtf8);
        removePackageFile(blobsPath);
        removePackageFile(wavesPath);

        // Package layout: also remove Cache subfolder and the package directory itself
        if (isPackageDir) {
            fs::path cacheDir = parentDir / "Cache";
            if (Platform::FileSystem::ProbePath(PathToUtf8(cacheDir)) ==
                Platform::FileSystem::PathStatus::Present) {
                std::string removeError;
                Platform::FileSystem::RemoveAll(PathToUtf8(cacheDir), &removeError);
            }
            // Try removing the now-empty package directory
            if (Platform::FileSystem::ProbePath(PathToUtf8(parentDir)) ==
                Platform::FileSystem::PathStatus::Present) {
                Platform::FileSystem::RemoveFile(PathToUtf8(parentDir));
            }
        }

        if (!firstFailure.empty()) {
            // Check if main db file is actually gone despite error on sidecars
            if (Platform::FileSystem::ProbePath(PathToUtf8(mainPath)) ==
                Platform::FileSystem::PathStatus::Present) {
                outError = FormatDbError(L(Db_DeleteLockedFmt), firstFailure.c_str());
                return false;
            }
            // Main file deleted, sidecar failure is non-critical
        }
        return true;
    } catch (const std::exception& e) {
        outError = e.what();
        return false;
    } catch (...) {
        outError = L(Db_DeleteUnknownFsErr);
        return false;
    }
}

// getCoverFacets is implemented above.

void Database::execQuery(const char* sql,
                          std::function<void(sqlite3_stmt*)> row_cb) {
    if (!m_db || !sql) return;
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) return;
    while (sqlite3_step(stmt) == SQLITE_ROW) row_cb(stmt);
    sqlite3_finalize(stmt);
}

std::vector<std::pair<int64_t, std::string>> Database::queryFilesMissingBpm(int limit, bool includeFailures, const std::string& folderPathPrefix) {
    std::vector<std::pair<int64_t, std::string>> rows;
    if (!m_db) return rows;

    const bool hasFolder = !folderPathPrefix.empty();

    // Build the BPM-missing condition
    const char* bpmCond =
        "(bpm IS NULL OR bpm = '' OR CAST(NULLIF(bpm,'') AS REAL) IS NULL OR CAST(NULLIF(bpm,'') AS REAL) = 0)";
    const char* bpmCondWithFail =
        "(bpm IS NULL OR bpm = '' OR CAST(NULLIF(bpm,'') AS REAL) IS NULL "
        "OR CAST(NULLIF(bpm,'') AS REAL) = 0 OR CAST(NULLIF(bpm,'') AS REAL) < 0)";
    const char* bpmFilter = includeFailures ? bpmCondWithFail : bpmCond;

    std::string sql = "SELECT id, file_path FROM audio_files WHERE ";
    sql += bpmFilter;
    sql += " AND file_path != ''";
    if (hasFolder)
        sql += " AND file_path LIKE ? ESCAPE '\\'";
    sql += " ORDER BY COALESCE(last_played, 0) DESC LIMIT ?";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) return rows;

    int paramIdx = 1;
    std::string likePattern;
    if (hasFolder) {
        // Escape LIKE special chars in the prefix, then append %
        likePattern.reserve(folderPathPrefix.size() + 8);
        for (char c : folderPathPrefix) {
            if (c == '%' || c == '_' || c == '\\') likePattern += '\\';
            likePattern += c;
        }
        likePattern += '%';
        sqlite3_bind_text(stmt, paramIdx++, likePattern.c_str(), -1, SQLITE_TRANSIENT);
    }
    sqlite3_bind_int(stmt, paramIdx, limit);

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        int64_t id = sqlite3_column_int64(stmt, 0);
        const char* p = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        if (p) rows.push_back({id, std::string(p)});
    }
    sqlite3_finalize(stmt);
    return rows;
}
