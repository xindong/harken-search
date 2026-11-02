#pragma once

#include <string>
#include <utility>
#include <vector>
#include <functional>
#include <cstdint>
#include <atomic>
#include <unordered_map>
#include <unordered_set>
#include <deque>
#include <mutex>
#include <thread>
#include <condition_variable>
#include <optional>
#include <chrono>

#include "platform/file_watcher.h"
#include "FeatureExtractor.h"
#include "ScanInterruption.h"
#include "WaveformCachePolicy.h"
#include "FolderTreePathModePolicy.h"
#include "SearchTextIndex.h"

// Forward declarations
struct sqlite3;
struct SynonymGroup;
struct sqlite3_stmt;
struct AppSettings;
namespace aura { class BackfillQueue; }

// Source/origin of a music-analysis field value
enum class MusicFieldSource : int {
    None        = 0,  // not yet analyzed
    AiAuto      = 1,  // computed by KeyDetector / BpmDetector
    UserManual  = 2,  // hand-edited by user
    EmbeddedTag = 3,  // read from ID3/FLAC tag during scan
    TapTempo    = 4,  // user tapped rhythm (BPM only)
};

// Audio file record in database

// SoA-optimized AudioRecord (hot/cold field separation)

// Fields are grouped by access frequency for cache-line efficiency:
//   HOT  = accessed every frame in file list render loop (ImGuiListClipper visible rows)
//   WARM = accessed when specific columns are visible
//   COLD = accessed only in dialogs, context menus, or database ops
//
// Layout rationale:
//   - Numeric hot fields packed first (40 bytes, fits in 1 cache line with padding)
//   - Frequently-displayed strings follow (needed for Selectable/Text calls)
//   - Cold strings at the end (only touched on right-click or metadata panel)
//   - Total struct size: ~456 bytes (MSVC x64 std::string = 32 bytes with SSO)

enum class SearchMatchSource : uint8_t {
    None = 0,
    Name,
    Path,
    Description,
    Keywords,
    Metadata,
    FuzzyName
};

struct AudioRecord {
    // 1. 热数据 (Hot Zone)：列表渲染每帧必访问，约 40 字节，利于 Cache Line
    int64_t id = 0;                    // 8B
    float durationSeconds = 0.0f;      // 4B
    uint32_t sampleRate = 0;           // 4B
    uint32_t channels = 0;             // 4B
    uint32_t bitDepth = 0;             // 4B；扫描/解析缓存，0 = 未知
    uint64_t fileSize = 0;             // 8B
    uint64_t totalFrames = 0;          // 8B
    bool hasCover = false;             // 1B
    char _pad[3] = {};                 // 3B 对齐

    // 2. 暖数据 (Warm Zone)：显示文字所需
    std::string displayName; // Precomputed label used by list rendering.
    std::string fileName;
    std::string filePath;
    std::string extension;
    std::string format;

    // 3. 冷数据 (Cold Zone)：右键/详情/搜索/封面时访问
    std::string description;
    std::string matchSnippet;
    SearchMatchSource matchSource = SearchMatchSource::None;
    std::string folderPath;
    std::string embeddedCoverKey;
    int64_t embeddedCoverKeyMtime = 0;
    std::string folderCoverKey;
    std::string folderCoverSrcPath;
    int64_t folderCoverSrcMtime = 0;
    bool coverSuppressed = false;

    std::string title;
    std::string artist;
    std::string album;
    std::string category;
    std::string keywords;
    int rating = 0;                    // 应用内评级（仅 DB）
    int playCount = 0;
    int64_t dateAdded = 0;
    int64_t lastPlayed = 0;
    int64_t fileMtime = 0;
    int64_t lastIndexed = 0;

    // Music analysis fields (F-033)
    std::string musicalKey;         // "C maj", "F# min"
    std::string musicalKeyCamelot;  // "8B", "5A"
    float       keyConfidence  = 0.0f;
    int         keySource      = 0;   // cast to MusicFieldSource
    float       bpm            = 0.0f;
    float       bpmConfidence  = 0.0f;
    int         bpmSource      = 0;
    float       moodValence    = 0.0f;
    float       moodArousal    = 0.0f;
    int         moodSource     = 0;

    // F-033 Music Mode extended fields
    std::string albumArtist;              // album_artist — for grouping (may differ from artist)
    int         trackNumber    = 0;       // track_number
    int         discNumber     = 1;       // disc_number
    float       energyTension  = 0.0f;   // energy_tension — spectral intensity
    float       energyDrive    = 0.0f;   // energy_drive   — rhythmic drive
    float       replayGainTrack = 0.0f;  // replay_gain_track (dB)
    float       replayGainAlbum = 0.0f;  // replay_gain_album (dB)
    int         highlightStartMs = 0;    // highlight_start_ms — best preview start
    std::string rawGenre;                // raw_genre from ID3 TCON
    std::string rawInstruments;          // raw_instruments from vendor tag
    std::string normGenre;               // norm_genre after vocabulary mapping
    std::string normInstruments;         // norm_instruments after mapping
    std::string normMoodTags;            // norm_mood_tags (semicolon-separated)
    std::string vocalType;               // vocal_type: "instrumental"|"has_vocals"|"has_lyrics"

    // 4. Extended metadata (professional-audio compatible, DB column name ? value)
    //    Key = DB column name (e.g., "subcategory", "library", "designer", "composer")
    //    All extended professional-audio/BWF/iXML fields stored here; kept off hot path.
    std::unordered_map<std::string, std::string> meta;

    // Convenience accessor for extended meta (returns empty string if missing)
    const std::string& getMeta(const std::string& key) const {
        static const std::string kEmpty;
        auto it = meta.find(key);
        return it != meta.end() ? it->second : kEmpty;
    }
};

// F-006 metadata patch: only fields present in the maps are updated.
// Key = DB 列名。字段出现在 map 中即参与 UPDATE；空字符串 = 清空该列。
// 支持 audio_files 表中的所有文本列和整数列（rating 等），通过白名单校验防注入。
struct MetadataEditPatch {
    std::unordered_map<std::string, std::string> fields;   // 文本字段：description, keywords, category, subcategory, library, ...
    std::unordered_map<std::string, int>         intFields; // 整数字段：rating, ...

// Convenience setters (common fields)
    void setDescription(const std::string& v) { fields["description"] = v; }
    void setKeywords(const std::string& v)    { fields["keywords"] = v; }
    void setCategory(const std::string& v)    { fields["category"] = v; }
    void setRating(int v)                     { intFields["rating"] = v; }
    void set(const std::string& col, const std::string& v) { fields[col] = v; }
    void setInt(const std::string& col, int v)              { intFields[col] = v; }

    bool isEmpty() const { return fields.empty() && intFields.empty(); }
};

struct BatchMetadataApplyResult {
    int total = 0;
    int success = 0;
    int failed = 0;
    std::vector<int64_t> failedFileIds;
    std::vector<std::string> errors;
};

struct CoverKeyCacheUpdate {
    std::string filePath; // UTF-8

    // Selective update flags (avoid wiping other cached fields)
    bool updateEmbedded = false;
    bool updateFolder = false;

    std::string embeddedCoverKey;
    int64_t embeddedCoverKeyMtime = 0;
    std::string folderCoverKey;
    std::string folderCoverSrcPath;
    int64_t folderCoverSrcMtime = 0;
};

struct BackfillPendingRow {
    int64_t fileId = 0;
    int kind = 0;
    int64_t enqueuedAt = 0;
    int64_t scanSessionId = 0;
};

struct ScanStats {
    int total = 0;
    int added = 0;
    int updated = 0;
    int skipped = 0;
    int failedFiles = 0;
    int removed = 0;
    int unreachable = 0;
    int errors = 0;
    int ingested = 0;
    bool fatalFailure = false;
    bool paused = false;
    bool cancelled = false;
    int64_t ingestDurationMs = 0;
    int waveformPendingCreated = 0;
    int waveformCacheReused = 0;
    int waveformPendingFixedDone = 0;
    int waveformLongDetailSkipped = 0;
    std::vector<std::string> errorMessages;

    void addError(std::string message = {}, int count = 1) {
        if (count <= 0) return;
        errors += count;
        if (!message.empty() && errorMessages.size() < 32) {
            errorMessages.push_back(std::move(message));
        }
    }

    void mergeFrom(const ScanStats& other) {
        total += other.total;
        added += other.added;
        updated += other.updated;
        skipped += other.skipped;
        failedFiles += other.failedFiles;
        removed += other.removed;
        unreachable += other.unreachable;
        errors += other.errors;
        ingested += other.ingested;
        fatalFailure = fatalFailure || other.fatalFailure;
        paused = paused || other.paused;
        cancelled = cancelled || other.cancelled;
        ingestDurationMs += other.ingestDurationMs;
        waveformPendingCreated += other.waveformPendingCreated;
        waveformCacheReused += other.waveformCacheReused;
        waveformPendingFixedDone += other.waveformPendingFixedDone;
        waveformLongDetailSkipped += other.waveformLongDetailSkipped;
        for (const auto& msg : other.errorMessages) {
            if (errorMessages.size() >= 32) break;
            if (!msg.empty()) errorMessages.push_back(msg);
        }
    }
};

struct WaveformCacheState {
    std::string status;      // pending/running/done/failed
    std::string errorCode;
    int retryCount = 0;
    int64_t updatedAt = 0;
    std::string cacheKey;
};

enum class ScanForegroundTerminalState : int {
    Pending = 0,
    Cached = 1,
    Resolved = 2,
    NoSource = 3,
    Failed = 4,
    Unsupported = 5,
    Suppressed = 6,
    Skipped = 7,
};

struct ScanForegroundCompletionCounts {
    int total = 0;
    int pendingFiles = 0;
    int miniTerminal = 0;
    int coverTerminal = 0;
    int metadataTerminal = 0;
    int miniUnresolved = 0;
    int coverUnresolved = 0;
    int metadataUnresolved = 0;
    int failureTerminal = 0;

    bool complete() const {
        return total > 0 &&
               miniUnresolved == 0 &&
               coverUnresolved == 0 &&
               metadataUnresolved == 0;
    }
};

// Playlist
struct Playlist {
    int64_t id = 0;
    std::string name;
    std::string description;
    int64_t dateCreated = 0;
    int64_t fileCount = 0;   // Cached file count; -1 means metadata loaded first, count still pending.
    // Sub-playlist fields (F-017 v2)
    int64_t parent_id = -1;  // -1 = NULL (top-level); positive = child pointing to parent
    bool    is_folder = false; // true = folder node; folder nodes must not hold playlist_items
};

// Search filter options
struct SearchFilter {
    std::string keyword;
    std::string category;
    std::string folderPath;  // Filter by folder
    float minDuration = 0.0f;
    float maxDuration = 0.0f;
    uint32_t minSampleRate = 0;
    uint32_t maxSampleRate = 0;
    float minBpm = 0.0f;
    float maxBpm = 0.0f;

    // Extended metadata filters
    std::string artist;
    std::vector<std::string> artists;
    std::string album;
    std::string format;       // WAV, MP3, FLAC, etc.
    uint32_t channels = 0;    // 0 = any, 1 = mono, 2 = stereo, etc.
    uint32_t minChannels = 0;
    uint32_t maxChannels = 0;

    // Channel category filter (takes precedence over numeric channels/min/max when != Any).
    // UI-driven enum path supporting Quad / 5.1 / 7.1 / "Multichannel (Other)"
    // which cannot be expressed by simple numeric comparisons.
    enum class ChannelCategory : uint8_t {
        Any = 0,
        Mono = 1,          // channels == 1
        Stereo = 2,        // channels == 2
        Quad = 3,          // channels == 4
        Surround_5_1 = 4,  // channels == 6
        Surround_7_1 = 5,  // channels == 8
        MultiOther = 6,    // channels >= 3 AND channels NOT IN (4,6,8)
    };
    ChannelCategory channelCategory = ChannelCategory::Any;
    
    // Musical key filter (exact match, e.g. "C major" or Camelot "8B")
    std::string musicalKey;

    // Cover filter: 0=Any, 1=Has cover, 2=No cover
    int coverFilter = 0;

    std::string specificCoverKey;

    // File extension exclusion list (pushed to SQL for correct pagination).
    // Extensions should be lowercase WITH leading dot, e.g. ".wma", ".m4a".
    // When non-empty, rows whose extension IN this list are excluded.
    std::vector<std::string> excludeExtensions;

    // When true, only include rows with known audio extensions
    // (.wav,.wave,.mp3,.flac,.ogg,.aiff,.aif,.m4a,.wma).
    // Used when "show Other" is unchecked.
    bool includeOnlyKnownExts = false;

    // F-034: exact search mode — no prefix '*' expansion
    bool exactMode = false;

    // F-034: synonym expansion for cross-language recall (non-owning pointer)
    const std::vector<SynonymGroup>* synonyms = nullptr;

    // Sort fields consumed by query construction.
    int sortColumn = 0;      // 对应 SortColumn 枚举
    bool sortAscending = true;

    // F-033: smart collection preset filters
    bool unplayedOnly = false;       // play_count = 0
    bool playedOnlyAny = false;      // last_played > 0 (has been played at least once)
    bool highlyRatedOnly = false;    // rating >= 4
    int64_t addedAfter = 0;          // date_added > addedAfter (0 = inactive)
    int64_t playedAfter = 0;         // last_played > playedAfter (0 = inactive)

    // F-033: Pill tag filters (applied to norm_genre / norm_instruments semicolon-lists)
    std::vector<std::string> includeGenres;       // norm_genre LIKE '%tag%'
    std::vector<std::string> excludeGenres;       // norm_genre NOT LIKE '%tag%'
    std::vector<std::string> includeInstruments;  // norm_instruments LIKE '%tag%'
    std::vector<std::string> excludeInstruments;  // norm_instruments NOT LIKE '%tag%'
};

// Folder info for hierarchy display
struct FolderInfo {
    std::string path;          // canonical (T2.8): used as compare key against selectedFolderPath
    std::string name;          // canonical basename — internal key, lowercase post-T2.8
    std::string displayName;   // T2.5: raw-case basename for UI rendering (derived from sample file_path)
    int fileCount = 0;
    std::vector<FolderInfo> subfolders;
};

// 启动健康检查结果 (F-009)
struct HealthCheckResult {
    bool allOk = true;
    bool dbIntegrity = true;          // PRAGMA integrity_check
    bool ftsIndexValid = true;        // FTS5 表可查
    int  orphanedRecords = 0;         // 磁盘文件已不存在的 DB 记录数
    int  missingWaveforms = 0;        // 有时长但无波形缓存的文件数
    int  totalFilesChecked = 0;       // 抽样检查的总文件数
    std::vector<std::string> warnings;
    std::vector<std::string> errors;
};

struct RepairCenterSnapshot {
    uint64_t packageBytes = 0;
    uint64_t mainDbBytes = 0;
    uint64_t blobsDbBytes = 0;
    uint64_t waveformsDbBytes = 0;
    uint64_t walBytes = 0;
    uint64_t shmBytes = 0;
    bool walSizeWarning = false;

    int libraryRootCount = 0;
    int missingLibraryRootCount = 0;
    std::vector<std::string> missingLibraryRoots;
    int unreachableLibraryRootCount = 0;
    std::vector<std::string> unreachableLibraryRoots;

    int failedWaveformRows = 0;
    int pendingWaveformRows = 0;
    int missingCoverThumbRows = 0;

    bool needsAttention = false;
    std::vector<std::string> warnings;
};

// @stage7-contract
// API contract: Database owns its SQLite connection and serialized mutation
// state. Callers must use the lifecycle methods before queries or writes and
// must not retain references to internal storage. Methods report operational
// failure through their existing bool/status/result values without changing
// transaction or thread-affinity semantics.
class Database {
public:
    Database();
    ~Database();

// Relink 候选结果 (F-034)
    struct RelinkCandidate {
        int         id = 0;
        std::string dbPath;       // DB 中存的（已失效）路径
        std::string fileName;
        int64_t     fileSize   = 0;
        int64_t     totalFrames = 0;
        int         sampleRate = 0;
    };

    /// 扫描/清理进度回调类型，用于 scanDirectory、scanDirectoryEx、removeMissingFilesDatabaseWide
    using ScanProgressCallback = std::function<void(int current, int total, const std::string& currentFile, bool dataCommitted)>;

    // Initialize database.  Pass isPrimary=true for the single app-owned connection;
    // worker-thread local connections leave it false to skip one-time migrations.
    bool open(const std::string& dbPath, bool isPrimary = false);
    // 打开只读连接（WAL 模式下不阻塞写操作，用于 SearchWorker 等纯读场景）
    bool openReadOnly(const std::string& dbPath);
    void close();
    bool isOpen() const { return m_db != nullptr; }
    sqlite3* db() const { return m_db; }
    /// Execute a read-only SQL query, calling row_cb for each result row.
    void execQuery(const char* sql, std::function<void(sqlite3_stmt*)> row_cb);
    /// Return (id, path) for files that have no BPM (NULL or 0), ordered by most-played first.
    /// When includeFailures is true, also returns files with sentinel bpm < 0 (previous detection failures).
    /// When folderPathPrefix is non-empty, only files whose path starts with that prefix are returned.
    std::vector<std::pair<int64_t, std::string>> queryFilesMissingBpm(int limit = 50000, bool includeFailures = true, const std::string& folderPathPrefix = {});
    // 获取当前已打开的主数据库路径（用于创建关联的只读连接）
    const std::string& getMainDbPath() const { return m_mainDbPath; }
    // True when this DB uses adaptive mini-waveform density profile
    // (new databases only). Ingest uses a per-file adaptive peak count instead of
    // the fixed 128/160 budget. See createTables() and ComputeAdaptiveMiniPeakCount.
    bool usesAdaptiveMiniWaveform() const { return m_usesAdaptiveMiniWaveform; }
    bool needsAdaptiveMiniWaveformFormatUpgrade() const {
        return m_needsAdaptiveMiniWaveformFormatUpgrade;
    }
    bool markAdaptiveMiniWaveformFormatUpgradeComplete();
    bool enterFastScanMode();
    bool exitFastScanMode();
    bool wasFastScanInterrupted() const { return m_fastScanDirtyOnOpen; }
    WaveformCachePolicy::Mode getWaveformCacheMode();
    bool setWaveformCacheMode(WaveformCachePolicy::Mode mode);
    // Interrupt the active query so a newer request can take priority.
    void interrupt();

    /// 启动预热：ANALYZE + 预编译热门 SQL + FTS5 索引预加载，应在 loading screen 期间调用
    void preheat();
    /// 预热封面索引：让 cover_key 复合索引页进入 mmap/page cache，加速首次封面点击
    void warmupCoverIndexes();

    // File indexing
    bool addFile(const AudioRecord& record);
    bool updateFile(const AudioRecord& record);
    bool probeAudioFileInfo(const std::string& filePath,
                            uint32_t& sampleRate,
                            uint32_t& channels,
                            uint64_t& totalFrames,
                            float& durationSeconds) const;
    static bool readAudioRecordMetadataSnapshot(
        const std::string& filePath,
        AudioRecord& record);

    // F-001 slice7-A: backfill_pending helpers (locked v6.1 schema).
    // kind uses BackfillJobKind values (0=CoverResolveAndThumb,
    // 1=WaveformL1Overview). Caller MUST wrap markBackfillPending in the same
    // transaction as the audio_files INSERT/UPSERT so a crash never leaves
    // orphan pending rows or audio_files without pending entries.
    bool markBackfillPending(int64_t fileId, int kind);
    bool clearBackfillPending(int64_t fileId, int kind);
    int  countBackfillPending();
    int  countBackfillPendingKind(int kind);
    int  countBackfillPendingKindInFolder(int kind, const std::string& folderPathPrefix);
    bool hasBackfillPending(int64_t fileId, int kind);
    bool resetScanForegroundCompletionSession(int64_t scanSessionId);
    bool seedScanForegroundCompletionRows(int64_t scanSessionId,
                                          const std::vector<int64_t>& fileIds,
                                          bool requireMini,
                                          bool requireCover,
                                          bool requireMetadata = false);
    bool markScanForegroundMiniTerminal(int64_t scanSessionId,
                                        int64_t fileId,
                                        ScanForegroundTerminalState state,
                                        const std::string& reason);
    bool markScanForegroundCoverTerminal(int64_t scanSessionId,
                                         int64_t fileId,
                                         ScanForegroundTerminalState state,
                                         const std::string& reason);
    bool markScanForegroundMetadataTerminal(int64_t scanSessionId,
                                            int64_t fileId,
                                            ScanForegroundTerminalState state,
                                            const std::string& reason);
    ScanForegroundCompletionCounts getScanForegroundCompletionCounts(int64_t scanSessionId);
    std::vector<BackfillPendingRow> getBackfillPendingRows(int limit,
                                                           int64_t afterEnqueuedAt = 0,
                                                           int64_t afterFileId = 0,
                                                           int afterKind = -1);
    std::vector<BackfillPendingRow> getBackfillPendingRowsInFolder(const std::string& folderPathPrefix,
                                                                   int limit,
                                                                   int64_t afterEnqueuedAt = 0,
                                                                   int64_t afterFileId = 0,
                                                                   int afterKind = -1);
    bool updateMetadata(int64_t fileId, const std::string& description, 
                       const std::string& keywords, const std::string& category);
    bool updateDescription(int64_t fileId, const std::string& description);

    /// 仅更新 patch 中标记的字段（覆盖 / 清空 / 不修改）；会刷新 search_data
    bool applyMetadataPatch(int64_t fileId, const MetadataEditPatch& patch);
    BatchMetadataApplyResult applyMetadataPatchBatch(const std::vector<int64_t>& fileIds,
                                                     const MetadataEditPatch& patch);
    /// 每条记录可使用不同 patch；按「更新字段掩码」分组，每组仅 prepare 一次再 bind/step
    BatchMetadataApplyResult applyMetadataPatchMany(
        const std::vector<std::pair<int64_t, MetadataEditPatch>>& items);

    bool removeFile(int64_t id);
    bool removeFileByPath(const std::string& filePath);
    bool fileExists(const std::string& filePath);

    // Music analysis setters (F-033)
    bool setMusicKey(int64_t fileId, const std::string& key,
                     const std::string& camelot, float confidence,
                     MusicFieldSource source);
    bool setMusicBpm(int64_t fileId, float bpm, float confidence,
                     MusicFieldSource source);
    bool setMusicMood(int64_t fileId, float valence, float arousal,
                      MusicFieldSource source);

    // Library type (F-033 Music Mode)
    std::string getLibraryType() const;          // returns "SFX" or "MUSIC"
    bool        setLibraryType(const std::string& type); // "SFX" or "MUSIC"
    
    /// 清理无效记录：删除数据库中对应磁盘文件已不存在的记录。支持进度回调与取消。
    int cleanupMissingFiles(ScanProgressCallback callback = nullptr, std::atomic<bool>* cancelFlag = nullptr);

    /// 全库清理缺失：遍历 audio_files，检查磁盘存在性，删除不存在的记录。支持进度与取消。
    /// 与 docs/plans/2026-03-17-全库清理缺失功能设计.md 一致。
    int removeMissingFilesDatabaseWide(ScanProgressCallback callback = nullptr, std::atomic<bool>* cancelFlag = nullptr);
    AudioRecord getFileByPath(const std::string& filePath);
    AudioRecord getFileById(int64_t id);

    // Search and query
    std::vector<AudioRecord> searchFiles(const SearchFilter& filter, int limit = 1000, int offset = 0);
    int64_t searchFilesCount(const SearchFilter& filter);
    std::vector<AudioRecord> searchFilesRandom(const SearchFilter& filter, int limit = 500);
    std::vector<AudioRecord> getAllFiles(int limit = 10000, int offset = 0);
    std::vector<AudioRecord> getRecentFiles(int limit = 50);
    std::vector<AudioRecord> getFilesByFolder(const std::string& folderPath, int limit = 1000, int offset = 0);
    std::vector<AudioRecord> getFilesByExactFolder(const std::string& folderPath, int limit = 1000, int offset = 0);
    std::vector<AudioRecord> getFilesByIds(const std::vector<int64_t>& ids);
    std::vector<AudioRecord> getFilesByPaths(const std::vector<std::string>& filePaths);
    int64_t getFileCount();

    // F-029: Database statistics
    struct DatabaseStats {
        int64_t fileCount = 0;
        int64_t totalSizeBytes = 0;     // sum of file sizes
        int64_t dbFileSizeBytes = 0;    // .db file size on disk
        std::string lastScanTime;       // ISO timestamp
        int64_t folderCount = 0;
    };
    DatabaseStats getDatabaseStats();

    // F-029: Backup & Restore
    static bool backupDatabase(const std::string& srcDbPath, const std::string& dstPath, std::string& errorMessage);
    static bool restoreDatabase(const std::string& backupPath, const std::string& dstDbPath, std::string& errorMessage);

    /// 启动时健康检查（F-009）：抽样验证 DB/FTS/波形/文件一致性
    HealthCheckResult performHealthCheck(int sampleSize = 200);
    RepairCenterSnapshot getRepairCenterSnapshot(
        uint64_t walWarningBytes = 1024ull * 1024ull * 1024ull) const;

    // 清理 macOS AppleDouble 资源分叉文件（._xxx），返回删除的记录数
    int purgeResourceForkFiles();
    
    // 清理低精度 waveform_detail 遗留数据（blob 太小的设为 NULL，触发下次播放时重新解码）
    int purgeLowResWaveformDetail(int minFloatCount = 400);

    // 数据库级封面扩散：按 folder_path 继承代表封面到无封面记录
    void propagateFolderCovers();
    // 仅对“同文件夹唯一封面 key”场景做扩散，返回本次更新的行数。
    int propagateUniqueFolderCovers(int maxFolders = 1200);

// 维护工具
    void rebuildFtsIndex();  // 重建 FTS5 全文搜索索引（阻塞式，仅小库用）
    // 分批重建 FTS 索引，每批 batchSize 行，通过 progress/cancel 回调报告进度
    bool rebuildFtsIndexBatched(int batchSize, std::atomic<float>* progress, std::atomic<bool>* cancel);
    // Recalculate stored search_data before copying it into FTS. The menu rebuild
    // used to copy the old column, so a pinyin-table change never reached search.
    HarkenSearch::Search::SearchDataRecomputeStep recomputeSearchDataBatch(int batchSize, std::atomic<bool>* cancel);
    // Deterministic transaction interleaving seam for isolated database tests.
    void setSearchRecomputePublishHookForTests(std::function<void()> hook);

// 封面登记表（cover_cache_registry）
    // Add newly observed cover keys to the registry in one batch.
    void syncCoverRegistry();
    void rebuildCoverTileMembershipIndex();
    int countAudioCoverKeyCandidates();
    bool isCoverTileMembershipMaintenancePending();
    int repairCoverTileMembershipBatch(int batchSize);
    int64_t countAudioCoverPresenceCandidates();
    int countCoverRegistryEntries();
    int pruneOrphanCoverCaches();
    bool isCoverRegistryStale();
    bool isCoverRegistryMaintenancePending();
    int repairSparseCoverRegistryBatch(int batchSize);
    int normalizeFolderCoverKeysByVisualIdentity(int maxDistinctCovers = 10000);
    void backfillCoverPhash();       // 对已有缩略图批量计算感知哈希
    void backfillNormalizedKey();    // 对 legacy F|path key 批量计算视觉 hash 规范化 key
    // 单条 UPSERT：手动分配封面后即时更新
    struct CoverRegistryDelta {
        std::string coverKey;
        std::string repPath;
        bool isEmbedded = false;
    };
    // deferFacetInvalidate=true skips the full-table invalidateFacetQueryCache()
    // call at the end. Use from hot-path callers (e.g., BackfillWorkerPool) that
    // will trigger a single coalesced invalidate at batch end. Default false
    // preserves the original semantics for all existing callers.
    int finalizeCoverRegistryDeltas(const std::vector<CoverRegistryDelta>& deltas,
                                    bool deferFacetInvalidate = false);
    void upsertCoverRegistry(const std::string& coverKey, const std::string& repPath, bool isEmbedded, const std::string& normalizedKey = "");
    // 孤立清除：封面被移除后，若主表中无引用则从登记表删除
    void removeCoverRegistryIfOrphan(const std::string& coverKey);
    
    // Waveform cache (list view - low resolution)
    bool saveWaveformByPath(const std::string& filePath,
                            const std::vector<float>& waveform,
                            float trueRmsDbfs = -200.0f,
                            int miniNumChannels = 1);
    bool saveMiniWaveformEnvelopeByPath(const std::string& filePath,
                                        const std::vector<float>& maxPeaks,
                                        const std::vector<float>& minPeaks,
                                        float trueRmsDbfs = -200.0f,
                                        int miniNumChannels = 1);
    std::vector<float> getWaveformByPath(const std::string& filePath,
                                         float* outTrueRmsDbfs = nullptr,
                                         int* outMiniNumChannels = nullptr);
    struct MiniWaveformBundle {
        std::vector<float> peaks;
        std::vector<float> minPeaks;
        std::vector<float> rms;
        float trueRmsDbfs = -200.0f;
        int miniNumChannels = 1;
        float integratedLUFS = 0.0f;
        float momentaryLUFS = 0.0f;
        float shortTermLUFS = 0.0f;
        float truePeakDBTP = 0.0f;
        bool hasTrueRms = false;
        bool hasLoudness = false;
    };
    bool getMiniWaveformBundleByPath(const std::string& filePath,
                                     MiniWaveformBundle& out,
                                     int targetFrames = 128);
    
    // Legacy optional RMS curve cache. Kept for existing user databases and
    // explicit future analysis paths; default waveform cache completeness must
    // never require rms_data to be present.
    bool saveRmsDataByPath(const std::string& filePath, const std::vector<float>& rmsData);
    std::vector<float> getRmsDataByPath(const std::string& filePath);

    // Spectral band data for SpectralPeaks waveform display
    bool saveSpectralDataByPath(const std::string& filePath, const std::vector<float>& spectralData);
    std::vector<float> getSpectralDataByPath(const std::string& filePath);
    
    // Loudness data storage (ITU-R BS.1770-4)
    bool saveLoudnessByPath(const std::string& filePath, float integrated, float momentary, float shortTerm, float truePeak);
    bool getLoudnessByPath(const std::string& filePath, float& integrated, float& momentary, float& shortTerm, float& truePeak);
    // Legacy
    bool saveLufsByPath(const std::string& filePath, float lufs);
    float getLufsByPath(const std::string& filePath);

// Audio Similarity Features (F-028)
    struct AudioFeatureEntry {
        int64_t fileId = 0;
        std::string filePath;
        AudioFeatureVector features;
    };
    bool saveAudioFeaturesByPath(const std::string& filePath, const AudioFeatureVector& vec);
    bool getAudioFeaturesByPath(const std::string& filePath, AudioFeatureVector& out);
    bool getAudioFeaturesById(int64_t fileId, AudioFeatureVector& out);
    bool tryLoadAllAudioFeatures(std::vector<AudioFeatureEntry>& out);
    bool tryCountFilesWithFeatures(int& out);
    // One SQLite snapshot; independent of the asynchronously rebuilt folder index.
    bool tryGetFeatureIndexCounts(int& indexed, int& total);
    std::vector<AudioFeatureEntry> loadAllAudioFeatures();  // For cache loading
    int countFilesWithFeatures();
    int countFilesWithoutFeatures(int limit = 0);
    // Get file paths missing features (for batch extraction). Returns up to `limit` paths.
    std::vector<std::string> getFilesWithoutFeatures(int limit = 500);
    
    // Waveform detail cache (main view - high resolution). A complete default
    // detail cache requires waveform_detail + waveform_min_detail and
    // detail_num_channels > 0. rms_data is not part of this completeness
    // contract.
    bool saveWaveformDetail(int64_t fileId,
                            const std::vector<float>& waveform,
                            const std::vector<float>& minWaveform = {},
                            int detailChannels = 0);
    bool saveWaveformDetailByPath(const std::string& filePath,
                                  const std::vector<float>& waveform,
                                  const std::vector<float>& minWaveform = {},
                                  int detailChannels = 0);
    bool clearWaveformDetailByPath(const std::string& filePath);
    bool clearWaveformCacheByPath(const std::string& filePath);
    int clearWaveformCacheByFolder(const std::string& folderPath);
    int purgeAllWaveformDetail();   // 清空所有 detail 波形缓存，返回影响行数
    int purgeAllMiniWaveform();     // 清空所有 mini 波形缓存，返回影响行数
    std::vector<float> getWaveformDetailByPath(const std::string& filePath);
    std::vector<float> getWaveformMinDetailByPath(const std::string& filePath);
    int getWaveformDetailChannelsByPath(const std::string& filePath);
    bool hasWaveformDetailByPath(const std::string& filePath);
    bool upsertWaveformCacheStateByPath(const std::string& filePath,
                                        const std::string& status,
                                        const std::string& errorCode,
                                        int retryCount,
                                        const std::string& cacheKey);
    bool getWaveformCacheStateByPath(const std::string& filePath, WaveformCacheState& out);
    int fixCompletedPendingWaveformStates();
    bool pruneWaveformCacheRows(int maxRows);
    bool clearWaveformCache();
    
// Mipmap波形数据结构（金字塔波形）
    struct WaveformMipmapLevel {
        int level = 0;                    // Mipmap层级 (0, 1, 2, ...)
        int samplesPerPoint = 0;         // 每点采样数 (256, 1024, 4096, ...)
        std::vector<float> peaks;        // 峰值数据 (Min/Max)
        std::vector<float> rms;          // RMS数据
    };
    
    struct WaveformMipmap {
        std::vector<WaveformMipmapLevel> levels;  // 多级精度数据
        uint64_t totalSamples = 0;                // 总采样数（用于计算层级）
    };
    
    // 保存Mipmap波形数据（自动生成多级精度）
    bool saveWaveformMipmapByPath(const std::string& filePath, const WaveformMipmap& mipmap);
    // 获取Mipmap波形数据（根据Zoom Level选择合适层级）
    bool getWaveformMipmapByPath(const std::string& filePath, WaveformMipmap& mipmap);
    // 根据Zoom Level和显示宽度获取合适的波形数据
    std::vector<float> getWaveformForZoom(const std::string& filePath, float zoomLevel, int displayWidth);
    
    // 生成Mipmap波形数据（从原始音频数据生成多级精度）
    static WaveformMipmap generateWaveformMipmap(const std::vector<float>& rawSamples, uint32_t sampleRate);
    
    // Waveform cache helpers
    std::vector<int64_t> getFilesNeedingWaveformCache(int limit);
    int countFilesNeedingWaveformCache();
    std::vector<int64_t> getFilesNeedingDetailCache(int limit);
    int countFilesNeedingDetailCache();
    int countFilesNeedingAnyWaveformCache();
    int64_t countAllFilesForWaveformRebuild();
    int64_t countFilesEligibleForPersistentDetailCache();
    
    int64_t getFolderFileCount(const std::string& folderPath);
    int64_t getFolderFileCountById(int64_t folderId); // 使用DirectoryID查询，速度更快

    // Folder hierarchy
    std::vector<std::string> getUniqueFolders();
    FolderInfo getFolderHierarchy(aura::folder_tree::PathMode pathMode = aura::folder_tree::PathMode::Absolute);
    FolderInfo getFolderHierarchyWithProgress(const std::function<void(int loaded, int total)>& progress,
                                              int progressStep = 200,
                                              aura::folder_tree::PathMode pathMode = aura::folder_tree::PathMode::Absolute);
    // 从 folder_index 表快速构建（启动秒开，毫秒级）
    FolderInfo getFolderHierarchyFromIndex(aura::folder_tree::PathMode pathMode = aura::folder_tree::PathMode::Absolute);

// 路径树整数化（DirectoryID映射）
    int64_t getOrCreateFolderId(const std::string& folderPath);
    std::string getFolderPathById(int64_t folderId);
    int64_t getFolderIdByPath(const std::string& folderPath);
    std::unordered_map<std::string, int64_t> getFolderIdMap(const std::vector<std::string>& folderPaths);

    // Metadata value lists (for filter UI)
    std::vector<std::string> getUniqueArtists(int limit = 2000);
    std::vector<std::string> getUniqueAlbums(int limit = 2000);
    std::vector<std::string> getUniqueCategories(int limit = 2000);

// Music Mode: Album Grid (F-033)
    struct AlbumEntry {
        std::string albumArtist;
        std::string album;
        int         trackCount   = 0;
        int         year         = 0;
        std::string coverPath;           // folder_cover_src_path of a representative file
        float       avgBpm       = 0.f;  // average BPM across tracks (0 = unknown)
        std::string topKey;              // most common musical key (empty = unknown)
        std::string firstFilePath;       // file_path of one track (for embedded-cover lookup)
        std::string embeddedCoverKey;    // embedded_cover_key from DB (avoids disk I/O in render)
        std::string groupQueryKey;       // optional alternate expansion key
        std::vector<std::string> memberFilePaths; // optional explicit expansion file paths
    };
    std::vector<AlbumEntry> getAlbums(const std::string& artistFilter = "");
    // sortMode: 0=album ASC, 1=album DESC, 2=year DESC, 3=track_count DESC
    std::vector<AlbumEntry> getAlbums(const std::string& searchQuery,
                                      const std::string& artistFilter,
                                      int sortMode);
    std::vector<AudioRecord> getTracksByAlbum(const std::string& albumArtist, const std::string& album);

// 关键词规范化（Tags表）
    int64_t getOrCreateTagId(const std::string& tagName);
    std::string getTagNameById(int64_t tagId);
    bool setFileTags(int64_t fileId, const std::vector<std::string>& tagNames);
    std::vector<std::string> getFileTags(int64_t fileId);
    std::vector<int64_t> searchFilesByTag(const std::string& tagName);
    /** 存量 keywords 一次性转化为标签关联，分批事务，progressCallback(processed, total) */
    int rebuildAllTagsFromKeywords(std::function<void(int, int)> progressCallback);
    /** 按使用频次排序的热门标签，用于标签云 UI */
    std::vector<std::pair<std::string, int>> getTopTags(int limit = 40);

// 批量标签操作 (F-007)
    /// 给多个文件添加同一个标签（去重，事务包裹）
    int addTagToFiles(const std::vector<int64_t>& fileIds, const std::string& tagName);
    /// 从多个文件移除同一个标签
    int removeTagFromFiles(const std::vector<int64_t>& fileIds, const std::string& tagName);
    /// 给单个文件追加标签（不覆盖已有）
    bool addTagToFile(int64_t fileId, const std::string& tagName);
    /// 从单个文件移除单个标签
    bool removeTagFromFile(int64_t fileId, const std::string& tagName);

// 颜色标记 (F-007)
    /// 设置文件颜色标记（空字符串 = 清除）。使用 track_colour 列。
    bool setColorMark(int64_t fileId, const std::string& color);
    bool setColorMarkBatch(const std::vector<int64_t>& fileIds, const std::string& color);
    std::string getColorMark(int64_t fileId);

// 字典编码（列式存储思维）
    int64_t getOrCreateArtistId(const std::string& artistName);
    std::string getArtistNameById(int64_t artistId);
    int64_t getOrCreateAlbumId(const std::string& albumName);
    std::string getAlbumNameById(int64_t albumId);
    int64_t getOrCreateCategoryId(const std::string& categoryName);
    std::string getCategoryNameById(int64_t categoryId);

    // Snooze: hide files for a period (snoozed files are excluded from search results)
    bool snoozeFilesById(const std::vector<int64_t>& ids, int64_t untilTimestamp);
    bool snoozeFilesByFolderPath(const std::string& folderPath, int64_t untilTimestamp);
    bool unsnoozeFilesById(const std::vector<int64_t>& ids);
    bool unsnoozeAll();
    int  getSnoozedCount();

    // Path maintenance (for rename/move on disk)
    bool updateFilePath(const std::string& oldPath, const std::string& newPath, const std::string& newFileName = "");
    bool updatePathPrefix(const std::string& oldFolderPath, const std::string& newFolderPath);

    // 业务层本地文件系统操作去重：抑制 watcher 对「本进程主动 rename」的重复回流
    void noteExpectedFileRename(const std::string& oldPath, const std::string& newPath);
    void noteExpectedFolderRename(const std::string& oldFolderPath, const std::string& newFolderPath);

    // Playlists
    struct PlaylistImportResult {
        int64_t playlistId = -1;
        int parsedFiles = 0;
        int uniqueFiles = 0;
        int matchedFiles = 0;
        int indexedFiles = 0;
        int addedFiles = 0;
        int duplicateInputFiles = 0;
        int alreadyInPlaylist = 0;
        int missingFiles = 0;
        int missingOnDiskFiles = 0;
        int unsupportedFiles = 0;
        int failedIndexFiles = 0;
        std::vector<std::string> missingPathSamples;
        std::string error;
    };

    struct PlaylistBulkAddResult {
        int64_t playlistId = -1;
        int requestedFiles = 0;
        int addedFiles = 0;
        int alreadyInPlaylist = 0;
        int invalidFiles = 0;
        std::string error;
    };

    int64_t createPlaylist(const std::string& name, const std::string& description = "");
    bool deletePlaylist(int64_t playlistId);
    bool renamePlaylist(int64_t playlistId, const std::string& newName);
    int64_t copyPlaylist(int64_t playlistId, const std::string& newName);
    std::vector<Playlist> getAllPlaylists(bool includeFileCounts = true);
    bool addToPlaylist(int64_t playlistId, int64_t fileId);
    PlaylistBulkAddResult addManyToPlaylist(int64_t playlistId, const std::vector<int64_t>& fileIds);
    PlaylistImportResult importReaperFileListToPlaylist(const std::string& fileListPath,
                                                        const std::string& playlistName,
                                                        ScanProgressCallback callback = nullptr,
                                                        std::atomic<bool>* cancelFlag = nullptr);
    bool removeFromPlaylist(int64_t playlistId, int64_t fileId);
    std::vector<AudioRecord> getPlaylistFiles(int64_t playlistId);
    std::vector<AudioRecord> getPlaylistFilesPage(int64_t playlistId, int limit, int offset);
    std::vector<AudioRecord> searchPlaylistFilesPage(int64_t playlistId, const SearchFilter& filter, int limit, int offset);
    int64_t searchPlaylistFileCount(int64_t playlistId, const SearchFilter& filter);
    std::vector<AudioRecord> searchPlaylistCoverKeys(int64_t playlistId, const SearchFilter& filter);
    std::vector<AudioRecord> getAllPlaylistFilesPage(int limit, int offset);
    std::vector<AudioRecord> searchAllPlaylistFilesPage(const SearchFilter& filter, int limit, int offset);
    int64_t getAllPlaylistFileCount();
    int64_t searchAllPlaylistFileCount(const SearchFilter& filter);
    /** 极速获取播放列表文件总数（百万级 <1ms，命中主键索引） */
    int64_t getPlaylistFileCount(int64_t playlistId);

    // 子播放列表 API (F-017 v2)
    bool markAsFolder(int64_t playlistId);                                   // 升级为 folder
    // Move a playlist into a folder, or unparent it.
    // newParentId == -1 → unparent (parent_id = NULL)
    // newParentId  > 0 → must point to an existing folder; child must not itself
    //                    be a folder; self-loop rejected.
    // Returns false (no DB write) if any precondition fails.
    bool setPlaylistParent(int64_t playlistId, int64_t newParentId);
    int64_t createSubPlaylist(int64_t parentId, const std::string& name,
                              const std::string& description = "");          // 在指定父下创建子
    std::vector<Playlist> getChildrenOf(int64_t parentId);                   // 列出子(按 id 升序)
    bool isFolder(int64_t playlistId);                                       // 查询 is_folder
    // 父 folder 的去重并集:返回所有子下文件的 distinct file_id 对应的 AudioRecord
    std::vector<AudioRecord> getAggregatedPlaylistFiles(int64_t parentFolderId);
    std::vector<AudioRecord> getAggregatedPlaylistFilesPage(int64_t parentFolderId, int limit, int offset);
    std::vector<AudioRecord> searchAggregatedPlaylistFilesPage(int64_t parentFolderId, const SearchFilter& filter, int limit, int offset);
    int64_t getAggregatedPlaylistFileCount(int64_t parentFolderId);
    int64_t searchAggregatedPlaylistFileCount(int64_t parentFolderId, const SearchFilter& filter);
    std::vector<AudioRecord> searchAggregatedPlaylistCoverKeys(int64_t parentFolderId, const SearchFilter& filter);

    // 把孤儿升级为 folder,原文件移到一个新的默认子。返回新子的 id;失败返回 -1。事务原子。
    int64_t promoteOrphanToFolderWithDefaultChild(int64_t orphanId, const std::string& defaultChildName);

    // 把子(或孤儿)挂到新父下;newParentId=-1 表示变孤儿。
    // 失败:子是 folder(深度限制),或目标父不是 folder。
    bool moveSubPlaylistToParent(int64_t subId, int64_t newParentId);

// 播放列表导出 (F-017)
    enum class PlaylistExportFormat { M3U, CSV, JSON };
    /// 导出播放列表到文件（M3U/CSV/JSON）
    bool exportPlaylist(int64_t playlistId, const std::string& outputPath, PlaylistExportFormat fmt);

// 智能播放列表 (F-017)
    /// 创建智能播放列表（基于筛选条件 JSON，每次打开时动态计算）
    int64_t createSmartPlaylist(const std::string& name, const std::string& filterJson);
    /// 获取智能播放列表的筛选条件 JSON
    std::string getSmartPlaylistFilter(int64_t playlistId);

    // Play tracking
    bool incrementPlayCount(int64_t fileId);
    bool updateLastPlayed(int64_t fileId);

    // CoverKey cache (persistent)
    bool updateCoverKeyCacheBatch(const std::vector<CoverKeyCacheUpdate>& updates);

    // Cover flag persistence
    bool updateHasCoverByPathsBatch(const std::vector<std::string>& filePaths, bool hasCover);
    bool assignEmbeddedCoverByPaths(const std::vector<std::string>& filePaths,
                                    const std::string& embeddedCoverKey,
                                    int64_t embeddedCoverKeyMtime);
    bool clearCoverByPaths(const std::vector<std::string>& filePaths);

// 封面缩略图持久化
    bool saveCoverThumb(const std::string& coverKey, const std::vector<uint8_t>& imageData, const std::string& mime);
    bool getCoverThumb(const std::string& coverKey, std::vector<uint8_t>& outImageData, std::string& outMime);
    bool hasCoverThumb(const std::string& coverKey);
    struct CoverWallEntry {
        std::string key;        // 封面唯一标识
        std::string path;       // 代表性文件路径
        std::string sourcePath; // FolderCover: real image path when known
        bool isEmbedded;        // true=内嵌, false=文件夹封面
    };
    struct CoverWallPage {
        std::vector<CoverWallEntry> entries;
        std::string nextCursor;
        bool hasMore = false;
        bool fromRegistry = false;
        bool largeLibrary = false;
        int registryEntries = 0;
    };
    struct CoverFacetEntry {
        std::string key;        // 封面唯一标识
        std::string path;       // 代表性路径（内嵌=file_path，文件夹=folder_path）
        bool isEmbedded = false;
        int64_t count = 0;      // 当前筛选全集下命中数量
        std::string sourcePath; // FolderCover real image path; EmbeddedCover optional PHASH token
    };
    struct PathFacetEntry {
        std::string folderPath; // 路径
        int64_t count = 0;      // 当前筛选全集下命中数量
    };
    CoverWallPage getGlobalCoverWallEntriesPage(const std::string& afterCursor = {}, int limit = 512,
                                                bool followFileListOrder = false);
    std::vector<CoverWallEntry> getGlobalCoverWallEntries();
    std::vector<CoverFacetEntry> getCoverFacets(const SearchFilter& filter, int limit = 200);
    std::vector<PathFacetEntry> getPathFacets(const SearchFilter& filter, int limit = 200);
    bool persistCoverFacetCache(const SearchFilter& filter,
                                int limit,
                                const std::vector<CoverFacetEntry>& entries);
    bool persistPathFacetCache(const SearchFilter& filter,
                               int limit,
                               const std::vector<PathFacetEntry>& entries);

    // F-033: tag facets — returns distinct non-empty values from norm_genre or norm_instruments
    // Each value may be semicolon-separated; the caller receives individual split tokens.
    struct TagFacetEntry { std::string tag; int count = 0; };
    std::vector<TagFacetEntry> getTagFacets(const std::string& column,
                                            const SearchFilter& filter,
                                            int limit = 50);
    /** 返回 cover_key 对应的所有 audio_files.id，用于封面拖拽批量加入播放列表 */
    // Replacement cover-wall tile membership for playlist artwork drag/drop.
    // This is a user-action membership lookup, not display truth or texture recovery.
    std::vector<int64_t> getFileIdsForCoverTileMembership(const std::string& tileIdentityKey);
    // Get all distinct folder_path values from audio_files (for cover wall discovery)
    std::vector<std::string> getDistinctFolderPaths();

    /** 极速搜索联想词，基于 FTS5 Vocab 树状索引，百万级库 <1ms */
    std::vector<std::string> getSearchSuggestions(const std::string& query, int limit = 10);

    /** 设置 AppSettings 指针，用于访问性能模式设置 */
    void setSettings(struct AppSettings* settings);
    /** 获取当前内存性能模式 (0=Eco, 1=Balanced, 2=Ultra, 3=Custom) */
    int getMemoryProfile() const;
    /** 获取封面缩略图最大分辨率 (px) */
    int getThumbnailMaxResolution() const;

// 文件夹索引持久化（启动秒开文件夹树）
    struct FolderIndexEntry { std::string folderPath; int fileCount; int64_t lastUpdated; };
    bool saveFolderIndexBatch(const std::vector<FolderIndexEntry>& entries);
    std::vector<FolderIndexEntry> getFolderIndex();
    bool clearFolderIndex();
    bool rebuildFolderIndex();
    /** 删除/移动文件后调用，使 facet 总数与文件夹树数量下次从数据库重算 */
    bool invalidateFacetQueryCache();
    bool rebuildTagFacetIndex();
    bool invalidateTagFacetIndex();
    bool isTagFacetIndexValid() const;

    // Batch operations
    bool beginTransaction();
    bool commitTransaction();
    bool rollbackTransaction();

    // Scan progress (uses ScanProgressCallback defined above)
    int scanDirectory(const std::string& dirPath, bool recursive, ScanProgressCallback callback = nullptr);
    
    // Scan/import and reconcile share this entry point, but they do not all
    // mean "finish a complete library import before the DB is trustworthy".
    // completeImport=true keeps the legacy blocking cache path; the normal UI
    // import path passes false so cover/waveform completion can be handed to
    // BackfillQueue after each committed batch. Watcher/startup reconciliation
    // may also set enqueueExtraction=false when it only needs DB freshness or
    // cache invalidation without generating new cover/waveform artifacts.
    ScanStats scanDirectoryEx(const std::string& dirPath,
                              bool recursive,
                              bool updateExisting,
                              bool removeMissing,
                              ScanProgressCallback callback = nullptr,
                              std::atomic<bool>* cancelFlag = nullptr,
                              int workerThreads = 0,
                              bool completeImport = true,
                              bool enqueueExtraction = true,
                              aura::BackfillQueue* backfillQueue = nullptr,
                              const std::string& backfillDbPathSnapshot = {},
                              int64_t backfillDbGenerationSnapshot = 0,
                              int64_t scanForegroundSessionId = 0,
                              const std::atomic<HarkenSearch::ScanStopDisposition>* stopDisposition = nullptr);
    void beginScanRollbackSession();
    bool rollbackActiveScanSession(aura::BackfillQueue* backfillQueue);
    void discardScanRollbackSession();

// Relink 缺失文件 (F-034)
    // 按指纹查找孤儿记录（file_path 对应磁盘文件不存在）
    // 返回空 optional 表示未找到；找到则返回孤儿记录
    std::optional<RelinkCandidate> findOrphanByFingerprint(
        const std::string& fileName,
        int64_t            fileSize,
        int64_t            totalFrames,
        int                sampleRate);

    // 扫描 searchRoots 目录，返回所有孤儿与其候选路径的映射
    // key = 孤儿记录, value = 磁盘上找到的候选路径列表
    std::vector<std::pair<RelinkCandidate, std::vector<std::string>>>
        findRelinkCandidates(const std::vector<std::string>& searchRoots,
                             std::atomic<bool>* cancelFlag = nullptr);

    void mergeLibraryRoots(const std::vector<std::string>& roots);
    std::vector<std::string> getLibraryRoots() const;
    bool isNetworkLibraryRoot(const std::string& pathUtf8) const;
    bool isNetworkLibraryFile(const std::string& pathUtf8) const;
    int pruneEntriesOutsideLibraryRoots();

    // Registered library roots used for watching and reconciliation.
    std::vector<std::string> getWatchedRoots() const;

// 文件系统监听（VFS监听）
    void startFileSystemWatcher(const std::vector<std::string>& watchDirectories);
    void stopFileSystemWatcher();
    void handleFileSystemEvent(const std::string& filePath, bool isAdded, bool isDeleted, bool isModified);

    // Queue reconciliation of registered roots after startup or a database switch.
    // Work runs on the watcher thread; the return value is the number of roots
    // newly queued.

    int enqueueStartupReconciliation();

    enum class WatcherChangeType { Added, Deleted, Modified, Renamed };
    using WatcherChangeCallback = std::function<void(WatcherChangeType type, const std::string& path)>;
    void setWatcherChangeCallback(WatcherChangeCallback cb) { m_watcherChangeCallback = std::move(cb); }

    // Utility
    std::string getLastError() const { return m_lastError; }

    // Internal helper: shared WHERE clause builder for searchFiles / searchFilesCount
    struct BuiltSearchQuery {
        std::string fromClause;                // "FROM audio_files" or with JOIN
        std::string whereClause;               // "WHERE ..."
        std::vector<std::string> bindValues;   // parameter values in bind order
        bool hasFtsJoin = false;
        // True only when the driving source is audio_files_fts alone (not UNION
        // with the trigram sidecar). snippet(audio_files_fts, …) and the `rank`
        // column are only accessible under this exact branch; UNION / trigram-only
        // must fall back to plain ORDER BY column paths.
        bool canUseFtsSnippet = false;
        bool parseError = false;
        std::string errorMsg;
        // FTS5 + cover key optimization: when both are active, extract cover key
        // condition as a rowid IN subquery so FTS5 checks only cover-index IDs
        // instead of scanning all FTS5 matches and back-reading main table pages.
        std::string coverKeySubquery;                    // e.g. "SELECT id FROM audio_files WHERE folder_cover_key = ?"
        std::vector<std::string> coverKeyBindValues;     // bind values for the subquery
        // 0=none, 1=folder_cover_key single equality, 2=embedded_cover_key single equality, 3=mixed/IN.
        // Used by non-FTS cover filtering to force the cover-key composite index
        // instead of letting SQLite scan idx_files_name for ORDER BY file_name.
        int coverKeyClauseKind = 0;
    };
    BuiltSearchQuery buildSearchWhere(const SearchFilter& filter);

    // ------------------------------------------------------------------------
    // 数据库物理包管理 (Database Package Management)
    // ------------------------------------------------------------------------
    static uint64_t GetDatabasePackageSize(const std::string& mainDbPathUtf8);
    static uint64_t GetDatabaseEffectiveSize(const std::string& mainDbPathUtf8);
    static RepairCenterSnapshot InspectDatabasePackageForRepair(
        const std::string& mainDbPathUtf8,
        uint64_t walWarningBytes = 1024ull * 1024ull * 1024ull);
    static bool DestroyDatabasePackage(const std::string& mainDbPathUtf8, std::string& outError);

    struct CacheSizeReport {
        uint64_t packageBytes = 0;
        uint64_t mainDbBytes = 0;
        uint64_t blobsDbBytes = 0;
        uint64_t waveformsDbBytes = 0;
        uint64_t walBytes = 0;
        uint64_t shmBytes = 0;
        int64_t mainPageCount = 0;
        int64_t mainFreelistCount = 0;
        int64_t blobsPageCount = 0;
        int64_t blobsFreelistCount = 0;
        int64_t waveformsPageCount = 0;
        int64_t waveformsFreelistCount = 0;
        int64_t miniWaveformBytes = 0;
        int64_t detailWaveformBytes = 0;
        int64_t coverThumbBytes = 0;
        int64_t ftsDataBytes = 0;
        int64_t trigramDataBytes = 0;
    };
    CacheSizeReport getCacheSizeReport();
    bool compactCachePackage(std::string& outError);
    bool compactCachePackageBestEffort(const char* reason = nullptr);
    bool checkpointCachePackageBestEffort(const char* reason = nullptr);

    struct QueryViewSnapshot {
        std::string snapshotKey;
        std::string searchText;
        std::string folderPath;
        std::string coverKey;
        int viewMode = 0;
        int sortColumn = 0;
        bool sortAscending = true;
        int64_t selectedFileId = 0;
        int64_t scrollAnchorFileId = 0;
        int64_t resultCount = 0;
        uint64_t dataRevision = 0;
        std::string payloadJson;
        int64_t updatedAt = 0;
    };

    bool saveQueryViewSnapshot(const QueryViewSnapshot& snapshot);
    bool loadQueryViewSnapshot(const std::string& snapshotKey, QueryViewSnapshot& outSnapshot);
    bool deleteQueryViewSnapshot(const std::string& snapshotKey);
    int pruneQueryViewSnapshots(int maxEntries);

    struct ActivityLogEntry {
        int64_t id = 0;
        std::string activityType;
        int64_t fileId = 0;
        std::string filePath;
        std::string targetPath;
        std::string detailJson;
        int64_t createdAt = 0;
    };

    bool appendActivityLog(const ActivityLogEntry& entry, int64_t* outId = nullptr);
    std::vector<ActivityLogEntry> getRecentActivityLog(const std::string& activityType = {},
                                                       int limit = 100);
    int pruneActivityLog(int maxEntries);

public:
    /// NeedRescan：异步入队，避免在 watcher 回调线程里做阻塞式目录遍历。
    /// 也被 enqueueStartupReconciliation() 直接调用。
    void enqueueWatcherFullRescan(const std::string& rootUtf8);

private:
    void ensureWatcherRescanWorkerStarted();
    void stopWatcherRescanWorker();
    void watcherRescanWorkerThread();

    struct PendingPrefixRename {
        std::string oldPrefixNorm;
        std::string newPrefixNorm;
        uint64_t expireAtMs = 0;
    };

    std::mutex m_fsDedupeMutex;
    std::atomic<bool> m_hasPendingFsDedupe{false};
    std::unordered_map<std::string, uint64_t> m_pendingRenamePairs; // key = oldNorm + '\n' + newNorm
    std::unordered_map<std::string, uint64_t> m_pendingPathEvents;  // old/new path for add/delete fallback
    std::vector<PendingPrefixRename> m_pendingPrefixRenames;

    void refreshFsDedupePendingFlagLocked();

    static std::string normalizeWatcherPath(const std::string& path);
    static std::string makeRenamePairKey(const std::string& oldNorm, const std::string& newNorm);
    static bool startsWithPathPrefix(const std::string& pathNorm, const std::string& prefixNorm);
    void pruneFsDedupeLocked(uint64_t nowMs);
    bool shouldSuppressWatcherEvent(const FileSystemWatcher::FileEvent& event);

    std::vector<std::string> getNetworkLibraryRoots() const;
    bool setFileTagsInTransaction(int64_t fileId, const std::vector<std::string>& tagNames);
    bool createTables();
    bool executeSQL(const std::string& sql);
    int64_t getLastInsertId();
    bool requireFts5();
    
    // Pool of reusable SQLite prepared statements.
    std::mutex m_stmtMutex;
    std::unordered_map<std::string, std::vector<sqlite3_stmt*>> m_stmtPool;
    sqlite3_stmt* checkoutStmt(const std::string& sql);
    void returnStmt(const std::string& sql, sqlite3_stmt* stmt);
    void clearStmtPool();

    /// 扫描后重型后处理（文件夹索引 / cover registry / facet 预热），与 close() 生命周期绑定，禁止 detach
    std::thread m_postProcessThread;
    std::atomic<bool> m_postProcessCancel{false};
    std::atomic<bool> m_postProcessRunning{false};
    /// Set to true when post-process finishes maintenance so UI can reload cover wall with correct phash values
    std::atomic<bool> m_postProcessCoverRefreshNeeded{false};

    /// 独立渐进式 cover dHash backfill worker（不绑定 heavy maintenance，大库也能跑）
    std::thread m_coverPhashBackfillThread;
    std::atomic<bool> m_coverPhashBackfillCancel{false};
    std::atomic<bool> m_coverPhashBackfillRunning{false};
    int64_t m_coverRegistryRepairCursorId = 0;
    int64_t m_coverTileMembershipRepairCursorId = 0;
    void coverPhashBackfillWorkerMain();

    /// Cache for equivalent cover keys (phash-based clustering) to avoid repeated full-table scans
    /// Key: source cover key, Value: vector of equivalent cover keys
    std::mutex m_coverKeyEquivalentCacheMutex;
    std::unordered_map<std::string, std::vector<std::string>> m_coverKeyEquivalentCache;
    std::mutex m_preparedCoverFilterCacheMutex;
    std::unordered_map<std::string, std::vector<std::string>> m_preparedCoverFilterCache;
    int repairMissingCoverThumbsBatch(int batchSize);
    int backfillCoverPhashBatch(int batchSize);
public:
    /// Helper methods for cache access (used by ResolveEquivalentCoverKeys)
    bool getCoverKeyEquivalentFromCache(const std::string& coverKey, std::vector<std::string>& out) {
        std::lock_guard<std::mutex> lock(m_coverKeyEquivalentCacheMutex);
        auto it = m_coverKeyEquivalentCache.find(coverKey);
        if (it != m_coverKeyEquivalentCache.end()) {
            out = it->second;
            return true;
        }
        return false;
    }
    void setCoverKeyEquivalentInCache(const std::string& coverKey, const std::vector<std::string>& equivalentKeys) {
        std::lock_guard<std::mutex> lock(m_coverKeyEquivalentCacheMutex);
        m_coverKeyEquivalentCache[coverKey] = equivalentKeys;
    }
    void clearCoverKeyEquivalentCache() {
        std::lock_guard<std::mutex> lock(m_coverKeyEquivalentCacheMutex);
        m_coverKeyEquivalentCache.clear();
    }
    bool getPreparedCoverFilterKeys(const std::string& coverKey, std::vector<std::string>& out) {
        std::lock_guard<std::mutex> lock(m_preparedCoverFilterCacheMutex);
        auto it = m_preparedCoverFilterCache.find(coverKey);
        if (it != m_preparedCoverFilterCache.end()) {
            out = it->second;
            return true;
        }
        return false;
    }
    void setPreparedCoverFilterKeys(const std::string& coverKey, const std::vector<std::string>& equivalentKeys) {
        if (coverKey.empty() || equivalentKeys.empty()) return;
        std::lock_guard<std::mutex> lock(m_preparedCoverFilterCacheMutex);
        m_preparedCoverFilterCache[coverKey] = equivalentKeys;
    }
    void clearPreparedCoverFilterKeys() {
        std::lock_guard<std::mutex> lock(m_preparedCoverFilterCacheMutex);
        m_preparedCoverFilterCache.clear();
    }
    void startCoverPhashBackfillWorker();
    void stopCoverPhashBackfillWorker();
    /// Returns true (once) when post-process maintenance finished and cover wall should reload
    bool consumePostProcessCoverRefresh() {
        return m_postProcessCoverRefreshNeeded.exchange(false, std::memory_order_acq_rel);
    }
    bool hasPendingPostProcessCoverRefresh() const {
        return m_postProcessCoverRefreshNeeded.load(std::memory_order_acquire);
    }
    bool isCoverPostProcessRunning() const {
        return m_postProcessRunning.load(std::memory_order_acquire);
    }

    // Canonical folder-path migration.
    //
    // Canonicalizes folder_path in audio_files, folder_index, and directories
    // under one transaction. An app_meta marker makes the operation idempotent;
    // row counts must match before and after. Directories rebuild lazily, while
    // folder_index is regenerated with GROUP BY.
    //
    // createTables() invokes this at startup. A false result means the
    // transaction was rolled back.

    bool migrateFolderPathToCanonical_v1();

    // Database maintenance and lifecycle helpers used by the UI and workers.

    // 1. Folder tree context menu: remove a subtree from the current database.
    // Disk files are kept by default; deleteFilesFromDisk only removes indexed files.
    int removeFolderSubtreeFromDb(const std::string& folderPath, bool deleteFilesFromDisk = false);

    // 2. main.cpp:8121 — 切库/切表前断开当前连接
    void detachConnectionForSwitch();

    // 3. AsyncWorker waveform 异步重建管线
    struct WaveformRebuildTask {
        int64_t fileId = 0;
        std::string filePath;
    };
    struct WaveformDetailBatchEntry {
        int64_t fileId = 0;
        std::vector<float> waveform;
        std::vector<float> minWaveform;
        // Legacy optional analysis payload. Default scan/rebuild paths should
        // leave this empty; do not use it as a detail-cache completeness signal.
        std::vector<float> rms;
        int detailChannels = 0;
    };
    std::vector<WaveformRebuildTask> getWaveformDetailRebuildTasks(
        int limit, int64_t lastClaimedFileId, bool incrementalOnly);
    bool saveWaveformDetailBatch(const std::vector<WaveformDetailBatchEntry>& batch);
private:
    // Caller holds m_writerSerializeMutex.
    bool ensureSearchRecomputeConnection();
    bool ensureFtsSchema();
    bool loadFacetQueryCache(const std::string& cacheKey, std::vector<uint8_t>& payload);
    bool saveFacetQueryCache(const std::string& cacheKey,
                             const std::vector<uint8_t>& payload,
                             int entryCount);
    void pruneFacetQueryCache(int maxEntries);
    bool invalidateFolderCoverFacetIndex();
    bool invalidateFolderCoverFacetIndexForPaths(const std::vector<std::string>& filePaths);
    bool tryGetMaterializedFolderCoverFacets(const SearchFilter& filter,
                                             int limit,
                                             std::vector<CoverFacetEntry>& results);
    bool tryGetMaterializedFolderPathFacets(const SearchFilter& filter,
                                            int limit,
                                            std::vector<PathFacetEntry>& results);
    bool materializeFolderCoverFacetScope(const std::string& scopePath, int scopeDepth);
    void prewarmFolderCoverFacetIndex(int maxDepth, int maxScopes);
    
    // Sidecar database management
    bool attachSidecarDatabases(const std::string& mainDbPath, bool readOnly = false);
    bool createSidecarTables();
    bool migrateFromSingleDatabase(const std::string& mainDbPath);
    std::string getBlobsDbPath(const std::string& mainDbPath) const;
    std::string getWaveformsDbPath(const std::string& mainDbPath) const;

    std::mutex m_watcherRescanMutex;
    std::condition_variable m_watcherRescanCv;
    std::deque<std::string> m_watcherRescanQueue;
    std::unordered_set<std::string> m_watcherRescanPendingRoots;
    std::thread m_watcherRescanThread;
    std::atomic<bool> m_watcherRescanRunning{false};
    // Cancellation reaches scanDirectoryEx hot loops so database close and switch
    // can join the reconciliation worker without waiting for a full scan.

    std::atomic<bool> m_watcherRescanCancel{false};
    // True only while the watcher worker executes scanDirectoryEx; worker
    // liveness is tracked separately by m_watcherRescanRunning.

    std::atomic<bool> m_watcherRescanActive{false};
    // Serialize scan batches and precompute writes on the shared connection.
    // The gate covers the autocommit check and each BEGIN-to-COMMIT window.

    std::mutex m_writerSerializeMutex;
    sqlite3* m_searchRecomputeDb = nullptr; // Owns the recompute TEMP work and transaction.
    std::function<void()> m_searchRecomputePublishHookForTests;

    // T-297: dedicated worker that turns scan-time "pending" waveform rows
    // into actual mini + detail blobs, so first-click playback hits the cache
    // instead of paying a 100-300 ms decode tax.
    struct PrecomputeJob {
        std::string filePath;
        std::string cacheKey;
    };
    std::mutex m_precomputeMutex;
    std::condition_variable m_precomputeCv;
    std::deque<PrecomputeJob> m_precomputeQueue;
    std::unordered_set<std::string> m_precomputePending;
    std::thread m_precomputeThread;
    std::atomic<bool> m_precomputeRunning{false};
    std::atomic<bool> m_precomputeCancel{false};
    std::atomic<int> m_precomputeProcessed{0};
    std::atomic<int> m_precomputeFailed{0};
    void ensurePrecomputeWorkerStarted();
    void stopPrecomputeWorker();
    void precomputeWorkerThread();
    bool prepareScanRollbackJournal();
    void clearScanRollbackJournal();
    bool snapshotPathForScanRollback(const std::string& filePath);
    bool rollbackScanRollbackJournal(const std::string& scopePath,
                                     aura::BackfillQueue* backfillQueue);
    bool m_scanRollbackSessionActive = false;

public:
    /// T-297: enqueue a file for waveform pre-extraction. Idempotent —
    /// duplicates are dropped silently. Worker calls ExtractMini + ExtractDetail
    /// then saveWaveformByPath / saveWaveformDetailByPath, finally upserts the
    /// cache state to "done". Safe to call from inside scanDirectoryEx after
    /// the batch transaction commits.
    void enqueuePrecomputeWaveform(const std::string& filePath,
                                   const std::string& cacheKey);

    // T-295: monotonically increasing counter bumped on every user-visible
    // write to the database (tag/metadata edit, scan commit). UI
    // tabs cache their searchResults / facets snapshot; on tab activation,
    // if cached revision < current revision, the tab re-executes its query
    // so a tag added in tab A is visible in tab B without manual refresh.
    uint64_t dataRevision() const { return m_dataRevision.load(std::memory_order_acquire); }
    void bumpDataRevision() { m_dataRevision.fetch_add(1, std::memory_order_acq_rel); }

    // True only while scanDirectoryEx is active in the reconciliation worker.
    // The thread may remain alive and idle between scans.

    bool isReconcileScanRunning() const {
        return m_watcherRescanActive.load(std::memory_order_acquire);
    }
    /// T-297: snapshot of the precompute queue depth (in flight + waiting),
    /// for status bar display and tests that need to wait for completion.
    int precomputeQueueDepth() const;
    int precomputeProcessedCount() const { return m_precomputeProcessed.load(std::memory_order_acquire); }
    int precomputeFailedCount() const { return m_precomputeFailed.load(std::memory_order_acquire); }


    // Public accessors for testing
    std::string getBlobsDbPathPublic(const std::string& mainDbPath) const { return getBlobsDbPath(mainDbPath); }
    std::string getWaveformsDbPathPublic(const std::string& mainDbPath) const { return getWaveformsDbPath(mainDbPath); }

    // SQLite query progress handler — allows aborting long-running queries
    // callback returns non-zero to abort; interval = VDBE ops between checks
    void setQueryProgressHandler(int interval, int(*callback)(void*), void* ctx);
    void clearQueryProgressHandler();

    sqlite3* m_db = nullptr;
    // T-295: tab-sync revision counter. Initialized to 1 so a fresh tab
    // with resultsBuiltAtRevision=0 always reads as stale on first activation.
    std::atomic<uint64_t> m_dataRevision{1};
    std::string m_lastError;
    std::string m_mainDbPath;  // 主数据库路径（meta.db）
    bool m_isPrimaryConnection = false;  // true only for the single app-owned DB connection
    // True only for databases created fresh under the adaptive mini-waveform regime
    // (app_meta 'mini_waveform_density' == "adaptive"). Legacy DBs stay on the fixed
    // 128/160 peak budget so existing waveform caches are not invalidated. Set in createTables().
    bool m_usesAdaptiveMiniWaveform = false;
    bool m_needsAdaptiveMiniWaveformFormatUpgrade = false;
    bool m_fastScanModeActive = false;
    bool m_fastScanDirtyOnOpen = false;

    // In-memory count cache: avoids SQLite round-trips for repeated searchFilesCount queries.
    // Faster than the persistent SQLite facet_query_cache during active library scans.
    // Cleared by invalidateFacetQueryCache().
    struct CountMemCacheEntry { int64_t count; int64_t expirySec; };
    std::unordered_map<std::string, CountMemCacheEntry> m_countMemCache;
    int64_t m_cachedFileCount = -1;
    uint64_t m_cachedFileCountRevision = 0;
    std::chrono::steady_clock::time_point m_cachedFileCountTime{};
    std::string m_blobsDbPath; // 封面数据库路径（blobs.db）
    std::string m_waveformsDbPath; // 波形数据库路径（waveforms.db）

    FileSystemWatcher* m_fileSystemWatcher = nullptr;
    WatcherChangeCallback m_watcherChangeCallback;

    struct AppSettings* m_settings = nullptr;  // 指向 AppSettings 的指针，用于访问性能模式设置

    // 数据库管理功能 (已包含在类定义内部)

    struct SqlResult {
        bool success = false;
        std::string error;
        std::vector<std::string> columns; 
        std::vector<std::vector<std::string>> rows; 
        int rowsAffected = 0; 
    };

    SqlResult executeRawQuery(const std::string& sql);

}; // <--- 类定义必须以分号结束，且后面绝对不能有多余的 '}'
