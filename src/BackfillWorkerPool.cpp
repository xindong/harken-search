#include "BackfillWorkerPool.h"
#include "PerformanceMonitor.h"

#include "AppState.h"
#include "BackfillQueue.h"
#include "CoverModule.h"
#include "Database.h"
#include "DiagnosticsLogger.h"
#include "MetadataReader.h"
#include "TagVocabularyMapper.h"
#include "Utils.h"
#include "WaveformCachePolicy.h"
#include "WaveformExtractor.h"
#include "platform/filesystem.h"

#define XXH_INLINE_ALL
#include "../thirdparty/xxhash.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <mutex>
#include <sstream>

namespace aura {
namespace {

std::mutex g_backfillDbWriteMutex;
std::atomic<int> g_scanWriteRequests{0};
std::atomic<bool> g_coverBackfillUiRefreshRequested{false};

// P2: Coalesced facet_query_cache invalidate. Hot-path processCoverJob
// commits one cover at a time; the per-call DELETE FROM facet_query_cache
// in invalidateFacetQueryCache() pays a fixed cost regardless of how many
// covers have changed since the last flush, so we coalesce.
constexpr int kCoverFacetFlushThreshold = 64;
constexpr auto kCoverFacetFlushInterval = std::chrono::seconds(1);

std::mutex g_coverFacetFlushMutex;
int g_coverFacetPending = 0;
std::chrono::steady_clock::time_point g_coverFacetLastFlush =
    std::chrono::steady_clock::now();

#if AURA_BUILD_DIAGNOSTICS
struct BackfillWorkerThroughputStats {
    int count = 0;
    int success = 0;
    int transient = 0;
    int permanent = 0;
    double totalMs = 0.0;
    double maxMs = 0.0;
    bool hasWindow = false;
    std::chrono::steady_clock::time_point windowStart;
};

const char* backfillJobKindDiagName(BackfillJobKind kind) {
    switch (kind) {
        case BackfillJobKind::CoverResolveAndThumb: return "cover";
        case BackfillJobKind::WaveformL1Overview: return "mini";
        case BackfillJobKind::MetadataRefresh: return "metadata";
    }
    return "unknown";
}

int backfillJobKindIndex(BackfillJobKind kind) {
    const int idx = static_cast<int>(kind);
    return (idx >= 0 && idx < kBackfillJobKindCount) ? idx : -1;
}

void emitBackfillWorkerThroughput(int workerIndex,
                                  BackfillJobKind kind,
                                  BackfillWorkerThroughputStats& stats,
                                  const char* reason) {
    if (stats.count <= 0) return;
    const auto now = std::chrono::steady_clock::now();
    const double wallMs = stats.hasWindow
        ? std::chrono::duration<double, std::milli>(now - stats.windowStart).count()
        : stats.totalMs;
    const double avgMs = stats.totalMs / std::max(1, stats.count);
    const double rate = wallMs > 0.0 ? (stats.count * 1000.0 / wallMs) : 0.0;
    DIAG_ACTION_FMT("Backfill",
                    "WorkerThroughput: idx=%d kind=%s reason=%s count=%d success=%d transient=%d permanent=%d avgMs=%.2f maxMs=%.2f rate=%.2f",
                    workerIndex,
                    backfillJobKindDiagName(kind),
                    reason ? reason : "",
                    stats.count,
                    stats.success,
                    stats.transient,
                    stats.permanent,
                    avgMs,
                    stats.maxMs,
                    rate);
    stats = BackfillWorkerThroughputStats{};
}

void recordBackfillWorkerThroughput(int workerIndex,
                                    BackfillWorkerThroughputStats statsByKind[kBackfillJobKindCount],
                                    BackfillJobKind kind,
                                    BackfillCompletionOutcome outcome,
                                    std::chrono::steady_clock::time_point jobStart) {
    const int idx = backfillJobKindIndex(kind);
    if (idx < 0) return;
    auto& stats = statsByKind[idx];
    if (!stats.hasWindow) {
        stats.hasWindow = true;
        stats.windowStart = jobStart;
    }
    const auto now = std::chrono::steady_clock::now();
    const double elapsedMs = std::chrono::duration<double, std::milli>(now - jobStart).count();
    ++stats.count;
    stats.totalMs += elapsedMs;
    stats.maxMs = std::max(stats.maxMs, elapsedMs);
    switch (outcome) {
        case BackfillCompletionOutcome::Success:
            ++stats.success;
            break;
        case BackfillCompletionOutcome::TransientFailure:
            ++stats.transient;
            break;
        case BackfillCompletionOutcome::PermanentFailure:
            ++stats.permanent;
            break;
    }
    if (stats.count >= 128) {
        emitBackfillWorkerThroughput(workerIndex, kind, stats, "periodic");
    }
}

void emitAllBackfillWorkerThroughput(int workerIndex,
                                     BackfillWorkerThroughputStats statsByKind[kBackfillJobKindCount],
                                     const char* reason) {
    for (int i = 0; i < kBackfillJobKindCount; ++i) {
        emitBackfillWorkerThroughput(
            workerIndex,
            static_cast<BackfillJobKind>(i),
            statsByKind[i],
            reason);
    }
}
#endif

void RequestCoverBackfillUiRefresh() {
    g_coverBackfillUiRefreshRequested.store(true, std::memory_order_release);
}

// Notes that a cover commit was deferred (i.e. the underlying
// finalizeCoverRegistryDeltas was called with deferFacetInvalidate=true)
// and triggers a single coalesced invalidateFacetQueryCache() when either
// 64 covers have accumulated OR 1 second has passed since the last flush.
// Cover-search results may be at most 1s / 64-events stale during scan.
void NoteCoverFacetDirtyAndMaybeFlush(Database& db) {
    bool shouldFlush = false;
    {
        std::lock_guard<std::mutex> lock(g_coverFacetFlushMutex);
        ++g_coverFacetPending;
        const auto now = std::chrono::steady_clock::now();
        if (g_coverFacetPending >= kCoverFacetFlushThreshold ||
            now - g_coverFacetLastFlush >= kCoverFacetFlushInterval) {
            g_coverFacetPending = 0;
            g_coverFacetLastFlush = now;
            shouldFlush = true;
        }
    }
    if (shouldFlush) {
        db.invalidateFacetQueryCache();
    }
}

int64_t fileMtimeUnixSeconds(const std::string& pathUtf8) {
    return Platform::FileSystem::LastWriteTimeUnixSeconds(pathUtf8);
}

std::string hex64(uint64_t value) {
    std::ostringstream oss;
    oss << std::hex << std::setw(16) << std::setfill('0') << value;
    return oss.str();
}

bool readFileBytes(const std::string& pathUtf8, std::vector<uint8_t>& out) {
    out.clear();
    if (pathUtf8.empty()) return false;
    out = Platform::FileSystem::ReadFileBytes(pathUtf8, 50 * 1024 * 1024);
    return out.size() > 16;
}

std::string mimeFromPath(const std::string& pathUtf8) {
    std::string lower = pathUtf8;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    if (lower.size() >= 4 && lower.rfind(".png") == lower.size() - 4) return "image/png";
    if (lower.size() >= 5 && lower.rfind(".webp") == lower.size() - 5) return "image/webp";
    return "image/jpeg";
}

bool saveCoverThumb(Database& db, const std::string& key,
                    const std::vector<uint8_t>& original,
                    const std::string& fallbackMime) {
    if (key.empty() || original.empty()) return false;
    if (db.hasCoverThumb(key)) return true;
    std::vector<uint8_t> thumb;
    std::string mime;
    if (CoverModule::PrepareCoverThumbForStorage(original, thumb, mime) &&
        !thumb.empty()) {
        return db.saveCoverThumb(key, thumb, mime);
    }
    return db.saveCoverThumb(key, original, fallbackMime.empty() ? "image/jpeg" : fallbackMime);
}

bool markForegroundCoverTerminal(Database& db,
                                 const BackfillJob& job,
                                 ScanForegroundTerminalState state,
                                 const char* reason) {
    if (job.scanSessionId <= 0) return true;
    return db.markScanForegroundCoverTerminal(
        job.scanSessionId,
        job.fileId,
        state,
        reason ? reason : "");
}

bool markForegroundMiniTerminal(Database& db,
                                const BackfillJob& job,
                                ScanForegroundTerminalState state,
                                const std::string& reason) {
    if (job.scanSessionId <= 0) return true;
    return db.markScanForegroundMiniTerminal(job.scanSessionId, job.fileId, state, reason);
}

bool markForegroundMetadataTerminal(Database& db,
                                    const BackfillJob& job,
                                    ScanForegroundTerminalState state,
                                    const std::string& reason) {
    if (job.scanSessionId <= 0) return true;
    return db.markScanForegroundMetadataTerminal(job.scanSessionId, job.fileId, state, reason);
}

bool markForegroundCoverTerminalOrTransient(Database& db,
                                            const BackfillJob& job,
                                            bool& transientFailure,
                                            ScanForegroundTerminalState state,
                                            const char* reason) {
    if (markForegroundCoverTerminal(db, job, state, reason)) {
        return true;
    }
    transientFailure = job.scanSessionId > 0;
    return false;
}

bool markForegroundMiniTerminalOrTransient(Database& db,
                                           const BackfillJob& job,
                                           bool& transientFailure,
                                           ScanForegroundTerminalState state,
                                           const std::string& reason) {
    if (markForegroundMiniTerminal(db, job, state, reason)) {
        return true;
    }
    transientFailure = job.scanSessionId > 0;
    return false;
}

bool markForegroundCoverFailureOrTransient(Database& db,
                                           const BackfillJob& job,
                                           bool& transientFailure,
                                           const char* reason) {
    markForegroundCoverTerminalOrTransient(
        db, job, transientFailure, ScanForegroundTerminalState::Failed, reason);
    return false;
}

bool markForegroundMiniFailureOrTransient(Database& db,
                                          const BackfillJob& job,
                                          bool& transientFailure,
                                          const std::string& reason) {
    markForegroundMiniTerminalOrTransient(
        db, job, transientFailure, ScanForegroundTerminalState::Failed, reason);
    return false;
}

bool markForegroundMetadataFailureOrTransient(Database& db,
                                              const BackfillJob& job,
                                              bool& transientFailure,
                                              const std::string& reason) {
    if (markForegroundMetadataTerminal(db, job, ScanForegroundTerminalState::Failed, reason)) {
        return false;
    }
    transientFailure = job.scanSessionId > 0;
    return false;
}

bool lookupFolderCoverImagePathCached(
    const std::string& folderPath,
    std::string& coverPath,
    std::mutex& folderCoverLookupMutex,
    std::unordered_map<std::string, std::string>& folderCoverLookupCache) {
    coverPath.clear();
    if (folderPath.empty()) return false;
    {
        std::lock_guard<std::mutex> lk(folderCoverLookupMutex);
        auto it = folderCoverLookupCache.find(folderPath);
        if (it != folderCoverLookupCache.end()) {
            coverPath = it->second;
            return !coverPath.empty();
        }
    }

    std::string resolved;
    const bool found =
        CoverModule::GetFolderCoverImagePathBackground(folderPath, resolved, true, true) &&
        !resolved.empty();
    {
        std::lock_guard<std::mutex> lk(folderCoverLookupMutex);
        auto inserted = folderCoverLookupCache.emplace(folderPath, found ? resolved : std::string());
        coverPath = inserted.first->second;
    }
    return !coverPath.empty();
}

bool processCoverJob(Database& db,
                     const BackfillJob& job,
                     const AudioRecord& rec,
                     bool& transientFailure,
                     std::mutex& folderCoverLookupMutex,
                     std::unordered_map<std::string, std::string>& folderCoverLookupCache) {
    transientFailure = false;
    if (rec.id <= 0 || rec.filePath.empty()) return false;
    if (rec.coverSuppressed) {
        std::unique_lock<std::mutex> writeGate;
        if (!TryAcquireBackfillWorkerWriteGate(writeGate)) {
            transientFailure = true;
            return false;
        }
        return markForegroundCoverTerminalOrTransient(
            db, job, transientFailure, ScanForegroundTerminalState::Suppressed, "cover_suppressed");
    }
    if (!rec.embeddedCoverKey.empty() || !rec.folderCoverKey.empty()) {
        std::unique_lock<std::mutex> writeGate;
        if (!TryAcquireBackfillWorkerWriteGate(writeGate)) {
            transientFailure = true;
            return false;
        }
        return markForegroundCoverTerminalOrTransient(
            db, job, transientFailure, ScanForegroundTerminalState::Resolved, "cover_already_cached");
    }

    AlbumArt art;
    if (MetadataReader::readAlbumArt(rec.filePath, art) && art.hasData()) {
        const uint64_t h = (uint64_t)XXH3_64bits(art.data.data(), art.data.size());
        const std::string key = CoverModule::CanonicalCoverKey(
            std::string("E|") + hex64(h) + "|" + std::to_string(art.data.size()) + "|" + art.mimeType);
        if (key.empty()) {
            std::unique_lock<std::mutex> writeGate;
            if (!TryAcquireBackfillWorkerWriteGate(writeGate)) {
                transientFailure = true;
                return false;
            }
            return markForegroundCoverFailureOrTransient(
                db, job, transientFailure, "cover_resolve_failed");
        }
        std::unique_lock<std::mutex> writeGate;
        if (!TryAcquireBackfillWorkerWriteGate(writeGate)) {
            transientFailure = true;
            return false;
        }
        if (!saveCoverThumb(db, key, art.data, art.mimeType)) {
            return markForegroundCoverFailureOrTransient(
                db, job, transientFailure, "cover_resolve_failed");
        }

        CoverKeyCacheUpdate update;
        update.filePath = rec.filePath;
        update.updateEmbedded = true;
        update.embeddedCoverKey = key;
        update.embeddedCoverKeyMtime = fileMtimeUnixSeconds(rec.filePath);
        if (!db.updateCoverKeyCacheBatch({update}) ||
            !db.updateHasCoverByPathsBatch({rec.filePath}, true) ||
            db.finalizeCoverRegistryDeltas({{key, rec.filePath, true}},
                                            /*deferFacetInvalidate=*/true) < 0) {
            return markForegroundCoverFailureOrTransient(
                db, job, transientFailure, "cover_resolve_failed");
        }
        if (!markForegroundCoverTerminalOrTransient(
                db, job, transientFailure, ScanForegroundTerminalState::Resolved, "embedded_cover")) {
            return false;
        }
        NoteCoverFacetDirtyAndMaybeFlush(db);  // P2: coalesced facet invalidate
        RequestCoverBackfillUiRefresh();
        return true;
    }

    if (rec.folderPath.empty()) {
        std::unique_lock<std::mutex> writeGate;
        if (!TryAcquireBackfillWorkerWriteGate(writeGate)) {
            transientFailure = true;
            return false;
        }
        return markForegroundCoverTerminalOrTransient(
            db, job, transientFailure, ScanForegroundTerminalState::NoSource, "no_cover_found");
    }
    std::string coverPath;
    if (!lookupFolderCoverImagePathCached(
            rec.folderPath,
            coverPath,
            folderCoverLookupMutex,
            folderCoverLookupCache)) {
        std::unique_lock<std::mutex> writeGate;
        if (!TryAcquireBackfillWorkerWriteGate(writeGate)) {
            transientFailure = true;
            return false;
        }
        return markForegroundCoverTerminalOrTransient(
            db, job, transientFailure, ScanForegroundTerminalState::NoSource, "no_cover_found");
    }
    std::string key = CoverModule::NormalizeCoverKeyByVisualIdentity(coverPath);
    if (key.empty()) {
        key = CoverModule::CanonicalCoverKey(std::string("F|PATH|") + coverPath);
    }
    if (key.empty()) {
        std::unique_lock<std::mutex> writeGate;
        if (!TryAcquireBackfillWorkerWriteGate(writeGate)) {
            transientFailure = true;
            return false;
        }
        return markForegroundCoverFailureOrTransient(
            db, job, transientFailure, "cover_resolve_failed");
    }

    std::vector<uint8_t> bytes;
    const bool hasCoverBytes = readFileBytes(coverPath, bytes);

    std::unique_lock<std::mutex> writeGate;
    if (!TryAcquireBackfillWorkerWriteGate(writeGate)) {
        transientFailure = true;
        return false;
    }
    if (hasCoverBytes && !saveCoverThumb(db, key, bytes, mimeFromPath(coverPath))) {
        return markForegroundCoverFailureOrTransient(
            db, job, transientFailure, "cover_resolve_failed");
    }

    CoverKeyCacheUpdate update;
    update.filePath = rec.filePath;
    update.updateFolder = true;
    update.folderCoverKey = key;
    update.folderCoverSrcPath = coverPath;
    update.folderCoverSrcMtime = fileMtimeUnixSeconds(coverPath);
    if (!db.updateCoverKeyCacheBatch({update}) ||
        !db.updateHasCoverByPathsBatch({rec.filePath}, true) ||
        db.finalizeCoverRegistryDeltas({{key, rec.folderPath, false}},
                                        /*deferFacetInvalidate=*/true) < 0) {
        return markForegroundCoverFailureOrTransient(
            db, job, transientFailure, "cover_resolve_failed");
    }
    if (!markForegroundCoverTerminalOrTransient(
            db, job, transientFailure, ScanForegroundTerminalState::Resolved, "folder_cover")) {
        return false;
    }
    NoteCoverFacetDirtyAndMaybeFlush(db);  // P2: coalesced facet invalidate
    RequestCoverBackfillUiRefresh();
    return true;
}

bool looksTransientDbError(std::string message);

bool processWaveformJob(Database& db,
                        const BackfillJob& job,
                        const AudioRecord& rec,
                        bool& transientFailure,
                        const std::atomic<bool>* cancelFlag = nullptr) {
    transientFailure = false;
    if (rec.id <= 0 || rec.filePath.empty()) return false;

    // Honor the scan-mini persistence policy here too: old queues from before
    // the threshold landed may still contain short files. AsyncWorker writes
    // mini back opportunistically on first runtime access, so it's safe to
    // drop these jobs without retrying.
    if (!WaveformCachePolicy::ShouldPersistScanMini(rec.durationSeconds)) {
        {
            std::lock_guard<std::mutex> dbMutationLock(
                aura::BackfillDatabaseMutationMutex());
            db.clearBackfillPending(
                rec.id, static_cast<int>(aura::BackfillJobKind::WaveformL1Overview));
        }
        markForegroundMiniTerminalOrTransient(
            db, job, transientFailure,
            ScanForegroundTerminalState::Cached, "mini_short_file_skipped");
        return true;
    }

    // Adaptive DBs derive the mini peak count from source frames; pass the
    // sentinel 0 so backfill writes the same density the interactive path
    // expects. Legacy DBs keep the fixed budget. Capture minPeaks so we can
    // persist the signed envelope, matching AsyncWorker's cache-MISS save.
    const bool adaptiveMiniDensity = db.usesAdaptiveMiniWaveform();
    const int miniTargetPeaks =
        adaptiveMiniDensity ? 0 : AppState::WAVEFORM_MINI_PEAKS;

    WaveformExtractResult miniExtract;
    std::vector<float> mini;
    std::vector<float> miniMin;
    int miniChannels = 1;
    const bool hasMini = WaveformExtractor::ExtractMini(
                             rec.filePath, miniTargetPeaks, miniExtract,
                             /*perChannel=*/false,
                             cancelFlag) &&
                         !miniExtract.peaks.empty();

    if (hasMini) {
        mini = std::move(miniExtract.peaks);
        miniMin = std::move(miniExtract.minPeaks);
        miniChannels = std::max(1, miniExtract.channels);
    } else {
        const bool cancelled =
            miniExtract.errorCode == "cancelled" ||
            (cancelFlag && cancelFlag->load(std::memory_order_acquire));
        if (cancelled) {
            transientFailure = true;
            return false;
        }
        std::unique_lock<std::mutex> writeGate;
        if (!TryAcquireBackfillWorkerWriteGate(writeGate)) {
            transientFailure = true;
            return false;
        }
        db.upsertWaveformCacheStateByPath(rec.filePath, "failed",
                                          miniExtract.errorCode.empty() ? "mini_extract_failed" : miniExtract.errorCode,
                                          1,
                                          WaveformExtractor::BuildCacheKey(rec.filePath, rec.fileSize, rec.fileMtime));
        return markForegroundMiniFailureOrTransient(
            db,
            job,
            transientFailure,
            miniExtract.errorCode.empty() ? "mini_extract_failed" : miniExtract.errorCode);
    }

    std::unique_lock<std::mutex> writeGate;
    if (!TryAcquireBackfillWorkerWriteGate(writeGate)) {
        transientFailure = true;
        return false;
    }
    const bool haveEnvelope = (miniMin.size() == mini.size());
    const bool saved = haveEnvelope
        ? db.saveMiniWaveformEnvelopeByPath(rec.filePath, mini, miniMin,
                                            -200.0f, miniChannels)
        : db.saveWaveformByPath(rec.filePath, mini, -200.0f, miniChannels);
    if (!saved) {
        transientFailure = true;
        return false;
    }
    const std::string cacheKey =
        WaveformExtractor::BuildCacheKey(rec.filePath, rec.fileSize, rec.fileMtime);
    if (!db.upsertWaveformCacheStateByPath(rec.filePath, "done", "", 0, cacheKey)) {
        transientFailure = looksTransientDbError(db.getLastError());
        return false;
    }
    if (!markForegroundMiniTerminalOrTransient(
            db, job, transientFailure, ScanForegroundTerminalState::Cached, "mini_cached")) {
        return false;
    }
    return true;
}

void applyMetadataToRecord(AudioRecord& record, const AudioMetadata& metadata) {
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
        record.rawGenre,
        record.rawInstruments,
        record.normGenre,
        record.normInstruments,
        record.normMoodTags,
        record.vocalType);
    if (!metadata.keywords.empty()) record.keywords = metadata.keywords;
    else if (!metadata.comment.empty() && record.description != metadata.comment) {
        record.keywords = metadata.comment;
    }
    PopulateExtendedMeta(record.meta, metadata);
}

MetadataEditPatch buildMetadataPatchFromRecord(const AudioRecord& record) {
    MetadataEditPatch patch;
    if (!record.description.empty()) patch.setDescription(record.description);
    if (!record.keywords.empty()) patch.setKeywords(record.keywords);
    if (!record.category.empty()) patch.setCategory(record.category);
    if (!record.title.empty()) patch.set("title", record.title);
    if (!record.artist.empty()) patch.set("artist", record.artist);
    if (!record.album.empty()) patch.set("album", record.album);
    for (const auto& kv : record.meta) {
        if (!kv.second.empty()) patch.set(kv.first, kv.second);
    }
    return patch;
}

bool processMetadataJob(Database& db,
                        const BackfillJob& job,
                        AudioRecord rec,
                        bool& transientFailure) {
    transientFailure = false;
    if (rec.id <= 0 || rec.filePath.empty()) return false;

    AudioMetadata metadata;
    if (!MetadataReader::readMetadata(rec.filePath, metadata)) {
        return markForegroundMetadataTerminal(
            db,
            job,
            ScanForegroundTerminalState::NoSource,
            "metadata_no_source");
    }

    applyMetadataToRecord(rec, metadata);
    rec.lastIndexed = std::time(nullptr);
    MetadataEditPatch patch = buildMetadataPatchFromRecord(rec);

    std::unique_lock<std::mutex> writeGate;
    if (!TryAcquireBackfillWorkerWriteGate(writeGate)) {
        transientFailure = true;
        return false;
    }
    if (!db.updateFile(rec)) {
        transientFailure = looksTransientDbError(db.getLastError());
        return false;
    }
    if (!patch.isEmpty() && !db.applyMetadataPatch(rec.id, patch)) {
        transientFailure = looksTransientDbError(db.getLastError());
        return false;
    }
    return markForegroundMetadataTerminal(
        db,
        job,
        ScanForegroundTerminalState::Resolved,
        "metadata_resolved");
}

bool looksTransientDbError(std::string message) {
    std::transform(message.begin(), message.end(), message.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return message.find("busy") != std::string::npos ||
           message.find("locked") != std::string::npos;
}

bool clearBackfillPendingWithBlockingGate(Database& db,
                                          const BackfillJob& job,
                                          const char* reason) {
    std::unique_lock<std::mutex> writeGate(g_backfillDbWriteMutex);
    std::lock_guard<std::mutex> dbMutationLock(aura::BackfillDatabaseMutationMutex());
    if (job.scanSessionId > 0) {
        const std::string terminalReason = reason ? reason : "permanent_failure";
        if (job.kind == BackfillJobKind::CoverResolveAndThumb) {
            db.markScanForegroundCoverTerminal(
                job.scanSessionId,
                job.fileId,
                ScanForegroundTerminalState::Failed,
                terminalReason);
        } else if (job.kind == BackfillJobKind::WaveformL1Overview) {
            db.markScanForegroundMiniTerminal(
                job.scanSessionId,
                job.fileId,
                ScanForegroundTerminalState::Failed,
                terminalReason);
        } else if (job.kind == BackfillJobKind::MetadataRefresh) {
            db.markScanForegroundMetadataTerminal(
                job.scanSessionId,
                job.fileId,
                ScanForegroundTerminalState::Failed,
                terminalReason);
        }
    }
    const bool cleared = db.clearBackfillPending(job.fileId, static_cast<int>(job.kind));
    DIAG_ACTION_FMT("Backfill",
                    "PendingClearFinal: fileId=%lld kind=%d reason=%s cleared=%d err=%s",
                    (long long)job.fileId,
                    static_cast<int>(job.kind),
                    reason ? reason : "",
                    cleared ? 1 : 0,
                    db.getLastError().c_str());
    return cleared;
}

} // namespace

BackfillWorkerPool::~BackfillWorkerPool() {
    stop();
}

BackfillWorkerPool& GlobalBackfillWorkerPool() {
    static BackfillWorkerPool pool;
    return pool;
}

bool ConsumeCoverBackfillUiRefresh() {
    return g_coverBackfillUiRefreshRequested.exchange(false, std::memory_order_acq_rel);
}

BackfillScanWriteGuard::BackfillScanWriteGuard()
    : m_lock() {
    g_scanWriteRequests.fetch_add(1, std::memory_order_acq_rel);
    m_lock = std::unique_lock<std::mutex>(g_backfillDbWriteMutex);
}

BackfillScanWriteGuard::~BackfillScanWriteGuard() {
    if (m_lock.owns_lock()) {
        m_lock.unlock();
    }
    g_scanWriteRequests.fetch_sub(1, std::memory_order_acq_rel);
}

bool TryAcquireBackfillWorkerWriteGate(std::unique_lock<std::mutex>& lock) {
    if (g_scanWriteRequests.load(std::memory_order_acquire) > 0) {
        return false;
    }
    lock = std::unique_lock<std::mutex>(g_backfillDbWriteMutex);
    if (g_scanWriteRequests.load(std::memory_order_acquire) > 0) {
        lock.unlock();
        return false;
    }
    return true;
}

void BackfillWorkerPool::start(const std::string& dbPath, int64_t dbGeneration,
                               BackfillQueue* queue, int threadCount) {
    stop();
    if (dbPath.empty() || dbGeneration <= 0 || queue == nullptr) return;
    if (threadCount <= 0) threadCount = 1;
    threadCount = std::clamp(threadCount, 1, 16);

    m_dbPath = dbPath;
    m_dbGeneration = dbGeneration;
    m_queue = queue;
    {
        std::lock_guard<std::mutex> lk(m_folderCoverLookupMutex);
        m_folderCoverLookupCache.clear();
    }
    m_cancelRequested.store(false, std::memory_order_release);
    m_running.store(true, std::memory_order_release);
    for (int i = 0; i < threadCount; ++i) {
        m_threads.emplace_back([this, i]() { workerMain(i); });
    }
}

void BackfillWorkerPool::stop() {
    const auto stopStart = std::chrono::steady_clock::now();
    m_cancelRequested.store(true, std::memory_order_release);
    m_running.store(false, std::memory_order_release);
    for (auto& t : m_threads) {
        if (t.joinable()) t.join();
    }
    m_threads.clear();
    m_queue = nullptr;
    m_dbPath.clear();
    m_dbGeneration = 0;
    {
        std::lock_guard<std::mutex> lk(m_folderCoverLookupMutex);
        m_folderCoverLookupCache.clear();
    }
    const double stopMs = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - stopStart).count();
    if (stopMs > 250.0) {
        DIAG_SYSTEM_FMT("Backfill", "WorkerPoolStop: waitedMs=%.2f", stopMs);
    }
}

void BackfillWorkerPool::workerMain(int workerIndex) {
    Database db;
    if (!db.open(m_dbPath)) {
        DIAG_ACTION_FMT("Backfill", "WorkerOpenFailed: idx=%d db=%s err=%s",
                        workerIndex, m_dbPath.c_str(), db.getLastError().c_str());
        return;
    }
#if AURA_BUILD_DIAGNOSTICS
    BackfillWorkerThroughputStats throughputStats[kBackfillJobKindCount]{};
#endif
    DIAG_SYSTEM_FMT("Backfill", "WorkerStart: idx=%d gen=%lld",
                    workerIndex, (long long)m_dbGeneration);

    while (m_running.load(std::memory_order_acquire) &&
           m_queue &&
           m_queue->currentGeneration() == m_dbGeneration) {
        if (g_scanWriteRequests.load(std::memory_order_acquire) > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
            continue;
        }

        m_queue->refillFromDatabaseIfBelowWaterMark(&db, 128, 512);

        BackfillJob job{};
        if (!m_queue->tryDequeue(job)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
            continue;
        }
        using harken::performance::Module;
        const Module module = job.kind == BackfillJobKind::CoverResolveAndThumb ? Module::Cover :
            job.kind == BackfillJobKind::WaveformL1Overview ? Module::Waveform : Module::Backfill;
        harken::performance::Scope activity(module);
#if AURA_BUILD_DIAGNOSTICS
        const auto jobStart = std::chrono::steady_clock::now();
#endif
        if (job.dbGeneration != m_dbGeneration) {
            m_queue->reportOutcome(job, BackfillCompletionOutcome::PermanentFailure);
#if AURA_BUILD_DIAGNOSTICS
            recordBackfillWorkerThroughput(
                workerIndex,
                throughputStats,
                job.kind,
                BackfillCompletionOutcome::PermanentFailure,
                jobStart);
#endif
            continue;
        }

        const AudioRecord rec = db.getFileById(job.fileId);
        bool ok = rec.id > 0;
        bool transientFailure = false;
        if (ok && job.kind == BackfillJobKind::CoverResolveAndThumb) {
            ok = processCoverJob(db, job, rec, transientFailure, m_folderCoverLookupMutex, m_folderCoverLookupCache);
        } else if (ok && job.kind == BackfillJobKind::WaveformL1Overview) {
            ok = processWaveformJob(db, job, rec, transientFailure, &m_cancelRequested);
        } else if (ok && job.kind == BackfillJobKind::MetadataRefresh) {
            ok = processMetadataJob(db, job, rec, transientFailure);
        } else if (!ok && job.kind == BackfillJobKind::CoverResolveAndThumb) {
            ok = markForegroundCoverFailureOrTransient(
                db, job, transientFailure, "missing_audio_row");
        } else if (!ok && job.kind == BackfillJobKind::WaveformL1Overview) {
            ok = markForegroundMiniFailureOrTransient(
                db, job, transientFailure, "missing_audio_row");
        } else if (!ok && job.kind == BackfillJobKind::MetadataRefresh) {
            ok = markForegroundMetadataFailureOrTransient(
                db, job, transientFailure, "missing_audio_row");
        }

        if (ok) {
            std::unique_lock<std::mutex> writeGate;
            BackfillCompletionOutcome outcome = BackfillCompletionOutcome::TransientFailure;
            if (TryAcquireBackfillWorkerWriteGate(writeGate) &&
                ([&] {
                    std::lock_guard<std::mutex> dbMutationLock(
                        aura::BackfillDatabaseMutationMutex());
                    return db.clearBackfillPending(
                        job.fileId, static_cast<int>(job.kind));
                })()) {
                outcome = BackfillCompletionOutcome::Success;
            }
            m_queue->reportOutcome(job, outcome);
#if AURA_BUILD_DIAGNOSTICS
            recordBackfillWorkerThroughput(workerIndex, throughputStats, job.kind, outcome, jobStart);
#endif
        } else {
            const BackfillCompletionOutcome outcome =
                transientFailure ? BackfillCompletionOutcome::TransientFailure
                                 : BackfillCompletionOutcome::PermanentFailure;
            m_queue->reportOutcome(job, outcome);
            const bool foregroundTerminalStillUnresolved =
                transientFailure && job.scanSessionId > 0;
            if (!foregroundTerminalStillUnresolved &&
                (!transientFailure ||
                 m_queue->itemState(job.fileId, job.kind) == BackfillItemState::FailedPermanent)) {
                clearBackfillPendingWithBlockingGate(
                    db,
                    job,
                    transientFailure ? "retry_budget_exhausted" : "permanent_failure");
            }
#if AURA_BUILD_DIAGNOSTICS
            recordBackfillWorkerThroughput(workerIndex, throughputStats, job.kind, outcome, jobStart);
#endif
        }
    }

#if AURA_BUILD_DIAGNOSTICS
    emitAllBackfillWorkerThroughput(workerIndex, throughputStats, "stop");
#endif
    db.close();
    DIAG_SYSTEM_FMT("Backfill", "WorkerStop: idx=%d gen=%lld",
                    workerIndex, (long long)m_dbGeneration);
}

}
