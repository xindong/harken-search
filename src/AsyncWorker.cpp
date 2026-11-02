// Search and waveform worker pool.
// AsyncWorker.cpp - 波形加载、搜索查询的异步处理实现

#include "AsyncWorker.h"
#include "Database.h"
#include "PlaylistCoverScope.h"
// sqlite3 progress handler accessed via Database::setQueryProgressHandler()
#include "SearchQuery.h"
#include "AppState.h"
#include "DiagnosticsLogger.h"
#include "CoverReplacementFoundation.h"
#include "WaveformExtractor.h"
#include "WaveformBucketMath.h"
#include "AudioEngine.h"
#include "WaveformCachePolicy.h"
#include "platform/filesystem.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdarg>
#include <filesystem>
#include <mutex>
#include <sstream>

namespace {

constexpr int kFilteredCoverWallFacetLimit = 10000;
constexpr int64_t kFacetCacheLargeRowsMin = 100000;
constexpr size_t kFacetCacheMaxEntries = 8;

void AppendDbQueryPerfLog(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);

    va_list countArgs;
    va_copy(countArgs, args);
    const int formattedSize = std::vsnprintf(nullptr, 0, fmt, countArgs);
    va_end(countArgs);
    if (formattedSize < 0) {
        va_end(args);
        return;
    }

    std::string line(static_cast<size_t>(formattedSize) + 1, '\0');
    std::vsnprintf(line.data(), line.size(), fmt, args);
    va_end(args);
    line.resize(static_cast<size_t>(formattedSize));

    const std::string tempDirectory = Platform::FileSystem::TemporaryDirectory();
    if (tempDirectory.empty()) return;
    const std::string logPath =
        (std::filesystem::u8path(tempDirectory) / "auro_perf.log").u8string();
    Platform::FileSystem::AppendFileText(logPath, line);
}

double LookupWaveformDurationSeconds(Database* db, int64_t fileId, const std::string& filePath) {
    if (!db) return 0.0;
    AudioRecord rec;
    if (fileId > 0) {
        rec = db->getFileById(fileId);
    }
    if (rec.id <= 0 && !filePath.empty()) {
        rec = db->getFileByPath(filePath);
    }
    return rec.durationSeconds;
}

#if AURA_BUILD_DIAGNOSTICS
struct MiniWaveWorkerDiagStats {
    int cacheHits = 0;
    int decodes = 0;
    double cacheHitMicros = 0.0;
    double decodeMicros = 0.0;
    int lastChannels = 0;
    int lastPeaks = 0;
    std::chrono::steady_clock::time_point lastFlush = std::chrono::steady_clock::now();
    std::mutex mutex;
};

MiniWaveWorkerDiagStats& miniWaveWorkerDiagStats() {
    static MiniWaveWorkerDiagStats stats;
    return stats;
}

void RecordMiniWaveWorkerDiagnostics(bool cacheHit, double elapsedUs, int channels, int peaks) {
    if (!DiagnosticsLogger::instance().isEnabled()) return;

    int flushCacheHits = 0;
    int flushDecodes = 0;
    double flushCacheHitMicros = 0.0;
    double flushDecodeMicros = 0.0;
    int flushLastChannels = 0;
    int flushLastPeaks = 0;
    bool shouldFlush = false;
    const auto now = std::chrono::steady_clock::now();

    {
        auto& stats = miniWaveWorkerDiagStats();
        std::lock_guard<std::mutex> lock(stats.mutex);
        if (cacheHit) {
            stats.cacheHits++;
            stats.cacheHitMicros += elapsedUs;
        } else {
            stats.decodes++;
            stats.decodeMicros += elapsedUs;
        }
        stats.lastChannels = channels;
        stats.lastPeaks = peaks;

        if (std::chrono::duration<double>(now - stats.lastFlush).count() >= 1.0) {
            flushCacheHits = stats.cacheHits;
            flushDecodes = stats.decodes;
            flushCacheHitMicros = stats.cacheHitMicros;
            flushDecodeMicros = stats.decodeMicros;
            flushLastChannels = stats.lastChannels;
            flushLastPeaks = stats.lastPeaks;
            stats.cacheHits = 0;
            stats.decodes = 0;
            stats.cacheHitMicros = 0.0;
            stats.decodeMicros = 0.0;
            stats.lastFlush = now;
            shouldFlush = true;
        }
    }

    if (shouldFlush && (flushCacheHits > 0 || flushDecodes > 0)) {
        DIAG_ACTION_FMT("MiniWave",
            "worker cacheHit=%d decode=%d cacheHitUs=%.2f decodeUs=%.2f lastChannels=%d lastPeaks=%d",
            flushCacheHits,
            flushDecodes,
            flushCacheHitMicros,
            flushDecodeMicros,
            flushLastChannels,
            flushLastPeaks);
    }
}
#endif

// Hash query inputs so cached rows can be shown before the refresh completes.
bool ShouldDeferWaveformDbWrites(Database* db) {
    if (::g_appState) {
        const AppState::ScanPhase phase =
            ::g_appState->scanPhase.load(std::memory_order_acquire);
        if (phase != AppState::ScanPhase::Idle &&
            phase != AppState::ScanPhase::WaveformCache) {
            return true;
        }
        if (::g_appState->isScanning.load(std::memory_order_acquire) &&
            phase == AppState::ScanPhase::Idle) {
            return true;
        }
    }
    return db && db->isReconcileScanRunning();
}

uint64_t BuildSearchFilterSignature(const SearchFilter& filter, int viewMode, int64_t playlistId) {
    uint64_t h = 0xCBF29CE484222325ull; // FNV-1a offset basis
    auto hashCombine = [&](const std::string& s) {
        for (char c : s) {
            h ^= (uint64_t)(unsigned char)c;
            h *= 0x100000001B3ull;
        }
    };

    h ^= (uint64_t)viewMode; h *= 0x100000001B3ull;
    h ^= (uint64_t)playlistId; h *= 0x100000001B3ull;
    hashCombine(filter.keyword);
    hashCombine(filter.folderPath);
    hashCombine(filter.category);
    hashCombine(filter.artist);
    hashCombine(filter.album);
    hashCombine(filter.format);
    h ^= (uint64_t)filter.sortColumn; h *= 0x100000001B3ull;
    h ^= (uint64_t)(filter.sortAscending ? 1 : 0); h *= 0x100000001B3ull;
    h ^= (uint64_t)filter.minDuration; h *= 0x100000001B3ull;
    h ^= (uint64_t)filter.maxDuration; h *= 0x100000001B3ull;
    h ^= (uint64_t)filter.minSampleRate; h *= 0x100000001B3ull;
    h ^= (uint64_t)filter.maxSampleRate; h *= 0x100000001B3ull;
    h ^= (uint64_t)filter.channels; h *= 0x100000001B3ull;
    h ^= (uint64_t)filter.minChannels; h *= 0x100000001B3ull;
    h ^= (uint64_t)filter.maxChannels; h *= 0x100000001B3ull;
    h ^= (uint64_t)static_cast<uint8_t>(filter.channelCategory); h *= 0x100000001B3ull;
    h ^= (uint64_t)filter.coverFilter; h *= 0x100000001B3ull;
    hashCombine(filter.specificCoverKey);
    h ^= (uint64_t)filter.includeOnlyKnownExts; h *= 0x100000001B3ull;
    for (const auto& artist : filter.artists) hashCombine(artist);
    for (const auto& ext : filter.excludeExtensions) hashCombine(ext);
    return h;
}

static std::string BuildFacetCacheKey(const AsyncSearchRequest& req, int64_t totalCountLocal) {
    std::ostringstream oss;
    auto appendVector = [&](const char* label, const std::vector<std::string>& values) {
        oss << label << '[' << values.size() << "]|";
        for (const auto& value : values) {
            oss << value.size() << ':' << value << '|';
        }
    };
    oss << req.viewMode << '|'
        << req.playlistId << '|'
        << req.limit << '|'
        << req.offset << '|'
        << totalCountLocal << '|'
        << req.playlistAggregatedView << '|'
        << req.folderFilterActive << '|'
        << req.selectedFolderPath << '|'
        << req.filter.keyword << '|'
        << req.filter.category << '|'
        << req.filter.folderPath << '|'
        << req.filter.minDuration << '|'
        << req.filter.maxDuration << '|'
        << req.filter.minSampleRate << '|'
        << req.filter.maxSampleRate << '|'
        << req.filter.minBpm << '|'
        << req.filter.maxBpm << '|'
        << req.filter.artist << '|'
        << req.filter.album << '|'
        << req.filter.format << '|'
        << req.filter.channels << '|'
        << req.filter.minChannels << '|'
        << req.filter.maxChannels << '|'
        << static_cast<int>(req.filter.channelCategory) << '|'
        << req.filter.musicalKey << '|'
        << req.filter.coverFilter << '|'
        << req.filter.specificCoverKey << '|'
        << req.filter.includeOnlyKnownExts << '|'
        << req.filter.exactMode << '|'
        << req.filter.sortColumn << '|'      // Sorting is part of the facet-cache key.
        << req.filter.sortAscending << '|'
        << req.filter.unplayedOnly << '|'
        << req.filter.playedOnlyAny << '|'
        << req.filter.highlyRatedOnly << '|'
        << req.filter.addedAfter << '|'
        << req.filter.playedAfter << '|';
    appendVector("artists:", req.filter.artists);
    appendVector("excludeExt:", req.filter.excludeExtensions);
    appendVector("includeGenres:", req.filter.includeGenres);
    appendVector("excludeGenres:", req.filter.excludeGenres);
    appendVector("includeInstruments:", req.filter.includeInstruments);
    appendVector("excludeInstruments:", req.filter.excludeInstruments);
    if (req.filter.synonyms) {
        oss << "synonyms[" << req.filter.synonyms->size() << "]|";
        for (const auto& group : *req.filter.synonyms) {
            appendVector("synTriggers:", group.triggers);
            appendVector("synRelated:", group.related);
        }
    } else {
        oss << "synonyms:null|";
    }
    return oss.str();
}

static bool ShouldUseWorkerFacetCache(const AsyncSearchRequest& req, int64_t totalCountLocal) {
    if (totalCountLocal <= 0) return false;
    if (aura::cover_replacement::HasPersistentFacetFilterSemantics(req.filter)) {
        return true;
    }
    return totalCountLocal >= kFacetCacheLargeRowsMin;
}

} // namespace

// WaveformWorker Implementation

WaveformWorker::WaveformWorker() {}

WaveformWorker::~WaveformWorker() {
    stop();
}

void WaveformWorker::start(Database* db, int threadCount) {
    if (m_running.load()) return;
    m_db = db;
    m_cancelRequested.store(false, std::memory_order_release);
    m_running.store(true);
    
    for (int i = 0; i < threadCount; ++i) {
        m_threads.emplace_back(&WaveformWorker::workerThread, this);
    }
}

void WaveformWorker::ensureThreadCount(int threadCount) {
    if (threadCount <= 0) return;
    if (!m_running.load()) {
        start(m_db, threadCount);
        return;
    }

    while ((int)m_threads.size() < threadCount) {
        m_threads.emplace_back(&WaveformWorker::workerThread, this);
    }
    m_requestCv.notify_all();
}

void WaveformWorker::stop() {
    if (!m_running.load()) return;
    const auto stopStart = std::chrono::steady_clock::now();
    m_cancelRequested.store(true, std::memory_order_release);
    m_generation.fetch_add(1, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lk(m_requestMutex);
        m_requestQueue.clear();
        m_requestSet.clear();
    }
    m_running.store(false);
    m_requestCv.notify_all();
    
    for (auto& t : m_threads) {
        if (t.joinable()) t.join();
    }
    m_threads.clear();
    {
        std::lock_guard<std::mutex> lk(m_processingMutex);
        m_processingSet.clear();
    }
    const double stopMs = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - stopStart).count();
    if (stopMs > 250.0) {
        DIAG_SYSTEM_FMT("Waveform",
            "WaveformWorkerStop: waitedMs=%.2f active=%d",
            stopMs,
            m_activeWorkers.load(std::memory_order_acquire));
    }
}

bool WaveformWorker::submitRequestInternal(const std::string& filePath,
                                           int64_t fileId,
                                           bool buildDetail,
                                           int priority,
                                           bool pushFront,
                                           const aura::BackfillJob* backfillJob,
                                           bool allowSourceDecode,
                                           bool meterOnly) {
    if (filePath.empty() || !m_running.load()) return false;
    
    // Ignore duplicate work already in progress.
    {
        std::lock_guard<std::mutex> lk(m_processingMutex);
        if (m_processingSet.count(filePath) > 0) return false;
    }
    
    {
        std::lock_guard<std::mutex> lk(m_requestMutex);
        // Already queued; meter-only requests coexist with mini requests and are deduplicated on the main thread.
        if (!meterOnly && m_requestSet.count(filePath) > 0) {
            if (!buildDetail) return false;

            for (auto& queued : m_requestQueue) {
                if (queued.filePath != filePath) continue;
                const bool wasDetail = queued.buildDetail;
                queued.buildDetail = true;
                queued.priority = std::max(queued.priority, priority);
                // Upgrading a visible mini-wave request should still produce a
                // fresh mini result; fileId=0 takes the combined mini+detail path.
                if (wasDetail && queued.fileId <= 0 && fileId > 0) {
                    queued.fileId = fileId;
                }
                m_requestCv.notify_one();
                return true;
            }
            return false;
        }
        WaveformRequest req;
        req.filePath = filePath;
        req.fileId = fileId;
        req.generation = m_generation.load(std::memory_order_relaxed);
        req.buildDetail = buildDetail;
        req.priority = priority;
        req.allowSourceDecode = allowSourceDecode;
        req.meterOnly = meterOnly;
        extern AppState* g_appState;
        const bool musicMode = g_appState && g_appState->settings.productMode == ProductMode::Music;
        // New libraries store adaptive mini density. miniTargetPeaks == 0
        // is the sentinel that tells the extractor to derive the peak count from the source
        // frame count; legacy libraries keep the fixed 128/160 budget.
        const bool adaptiveMiniDensity = m_db && m_db->usesAdaptiveMiniWaveform();
        req.miniTargetPeaks = adaptiveMiniDensity
            ? 0
            : (musicMode ? AppState::WAVEFORM_MINI_PEAKS_MUSIC : AppState::WAVEFORM_MINI_PEAKS);
        req.miniPerChannel = musicMode;
        req.wantMeterData = g_appState && g_appState->needsMeterDataComputation;
        if (backfillJob) {
            req.hasBackfillJob = true;
            req.backfillJob = *backfillJob;
        }
        // LIFO keeps the newest visible request ahead of stale scroll work.
        if (pushFront) {
            m_requestQueue.push_front(std::move(req));
        } else {
            m_requestQueue.push_back(std::move(req));
        }
        if (!meterOnly) m_requestSet.insert(filePath);
    }
    m_requestCv.notify_one();
    return true;
}

bool WaveformWorker::submitRequest(const std::string& filePath) {
    return submitRequestInternal(filePath, 0, false, 1, true);
}

bool WaveformWorker::submitVisibleRequest(const std::string& filePath) {
    return submitRequestInternal(filePath, 0, false, 2, true);
}

bool WaveformWorker::submitVisibleCacheRequest(const std::string& filePath) {
    return submitRequestInternal(filePath, 0, false, 2, true, nullptr, false);
}

bool WaveformWorker::submitDetailRequest(const std::string& filePath, int64_t fileId) {
    return submitRequestInternal(filePath, fileId, true, 0, false);
}

bool WaveformWorker::submitBackfillDetailRequest(const std::string& filePath, const aura::BackfillJob& job) {
    // Backfill jobs persist durable L1/list waveforms only. Main detail BLOBs
    // are reserved for the current playback item and explicit rebuild commands.

    return submitRequestInternal(filePath, job.fileId, false, 0, false, &job);
}

bool WaveformWorker::submitMeterRequest(const std::string& filePath, int64_t fileId) {
    // Low priority (0), push_back, allowSourceDecode=true, meterOnly=true.
    return submitRequestInternal(filePath, fileId, false, 0, false, nullptr, true, true);
}

void WaveformWorker::cancelAllPending() {
    const uint64_t nextGeneration =
        m_generation.fetch_add(1, std::memory_order_relaxed) + 1;
    {
        std::lock_guard<std::mutex> lk(m_requestMutex);
        std::deque<WaveformRequest> preservedRequests;
        std::unordered_set<std::string> preservedRequestSet;
        for (auto& req : m_requestQueue) {
            if (!req.hasBackfillJob) continue;
            req.generation = nextGeneration;
            preservedRequestSet.insert(req.filePath);
            preservedRequests.push_back(std::move(req));
        }
        m_requestQueue.swap(preservedRequests);
        m_requestSet.swap(preservedRequestSet);
    }
    // Clear stale UI mini-wave results, but keep completed backfill outcomes.
    // Those jobs have already been dequeued from BackfillQueue, so dropping the
    // result would leave them in-flight with no success/failure report.
    {
        std::lock_guard<std::mutex> lkRes(m_resultMutex);
        std::queue<WaveformResult> preservedResults;
        while (!m_resultQueue.empty()) {
            WaveformResult res = std::move(m_resultQueue.front());
            m_resultQueue.pop();
            if (res.hasBackfillJob) {
                preservedResults.push(std::move(res));
            }
        }
        m_resultQueue.swap(preservedResults);
    }
    m_requestCv.notify_all();
}

int WaveformWorker::pollResults(std::vector<WaveformResult>& outResults, int maxResults) {
    std::lock_guard<std::mutex> lk(m_resultMutex);
    int count = 0;
    while (!m_resultQueue.empty() && count < maxResults) {
        outResults.push_back(std::move(m_resultQueue.front()));
        m_resultQueue.pop();
        count++;
    }
    return count;
}

bool WaveformWorker::hasPendingRequests() const {
    std::lock_guard<std::mutex> lk(m_requestMutex);
    return !m_requestQueue.empty();
}

int WaveformWorker::pendingCount() const {
    std::lock_guard<std::mutex> lk(m_requestMutex);
    return (int)m_requestQueue.size() +
           m_activeWorkers.load(std::memory_order_acquire);
}

void WaveformWorker::workerThread() {
    struct ActiveWorkerGuard {
        std::atomic<int>& activeCount;
        std::condition_variable& cv;

        explicit ActiveWorkerGuard(std::atomic<int>& count, std::condition_variable& cond)
            : activeCount(count), cv(cond) {}
        ~ActiveWorkerGuard() {
            activeCount.fetch_sub(1, std::memory_order_seq_cst);
            cv.notify_one();
        }
    };

    struct ProcessingGuard {
        std::unordered_set<std::string>& procSet;
        std::mutex& procMutex;
        std::string path;

        ProcessingGuard(std::unordered_set<std::string>& s, std::mutex& m, std::string p)
            : procSet(s), procMutex(m), path(std::move(p)) {
            std::lock_guard<std::mutex> lk(procMutex);
            procSet.insert(path);
        }
        // Explicit early release: call before pushing result to m_resultQueue so the
        // main thread never sees a result while this path is still in m_processingSet.
        // (If it does, submitMeterRequest would be silently rejected.)
        void release() {
            if (!path.empty()) {
                std::lock_guard<std::mutex> lk(procMutex);
                procSet.erase(path);
                path.clear();
            }
        }
        ~ProcessingGuard() {
            if (!path.empty()) {
                std::lock_guard<std::mutex> lk(procMutex);
                procSet.erase(path);
            }
        }
    };

    while (m_running.load()) {
        WaveformRequest req;

        {
            std::unique_lock<std::mutex> lk(m_requestMutex);
            // Sleep until work arrives or worker availability changes.
            m_requestCv.wait(lk, [this] {
                if (!m_running.load()) return true;
                if (m_requestQueue.empty()) return false;
                if (m_throttled.load(std::memory_order_acquire) &&
                    m_activeWorkers.load(std::memory_order_acquire) >= 1) {
                    return false;  // Another worker is active; keep this throttled request queued.
                }
                if (m_throttled.load(std::memory_order_acquire)) {
                    return std::any_of(m_requestQueue.begin(), m_requestQueue.end(),
                                       [](const WaveformRequest& queued) {
                                           return queued.priority > 0;
                                       });
                }
                return true;
            });

            if (!m_running.load()) break;
            if (m_requestQueue.empty()) continue;

            if (m_throttled.load(std::memory_order_acquire) &&
                m_activeWorkers.load(std::memory_order_acquire) >= 1) {
                continue;
            }

            auto requestIt = m_requestQueue.begin();
            if (m_throttled.load(std::memory_order_acquire)) {
                requestIt = std::find_if(m_requestQueue.begin(), m_requestQueue.end(),
                                         [](const WaveformRequest& queued) {
                                             return queued.priority > 0;
                                         });
                if (requestIt == m_requestQueue.end()) {
                    continue;
                }
            }

            m_activeWorkers.fetch_add(1, std::memory_order_seq_cst);
            req = std::move(*requestIt);
            m_requestQueue.erase(requestIt);
            m_requestSet.erase(req.filePath);
        }

        ActiveWorkerGuard guard(m_activeWorkers, m_requestCv);

        const uint64_t activeGeneration = m_generation.load(std::memory_order_relaxed);
        if (!req.hasBackfillJob && req.generation != activeGeneration) continue;
        const std::string& filePath = req.filePath;
        auto markBackfillMiniDone = [&]() {
            if (!m_db || !req.hasBackfillJob) return;
            WaveformCacheState cacheState;
            std::string cacheKey;
            if (m_db->getWaveformCacheStateByPath(filePath, cacheState)) {
                cacheKey = cacheState.cacheKey;
            }
            if (cacheKey.empty() && req.fileId > 0) {
                const AudioRecord rec = m_db->getFileById(req.fileId);
                if (rec.id > 0) {
                    cacheKey = WaveformExtractor::BuildCacheKey(
                        rec.filePath, rec.fileSize, rec.fileMtime);
                }
            }
            if (!cacheKey.empty()) {
                m_db->upsertWaveformCacheStateByPath(filePath, "done", "", 0, cacheKey);
            }
        };

        ProcessingGuard procGuard(m_processingSet, m_processingMutex, filePath);

        WaveformResult result;
        result.filePath = filePath;
        result.success = false;
        result.fileId = req.fileId;
        result.cacheOnly = req.hasBackfillJob;
        result.hasBackfillJob = req.hasBackfillJob;
        result.backfillJob = req.backfillJob;
        result.fromCache = false;
        result.errorCode = "unknown";
        result.generation = req.generation;

        if (m_db && req.meterOnly) {
            result.metersOnlyMerge = true;
            const bool deferDbWrites = ShouldDeferWaveformDbWrites(m_db);
            const bool adaptiveMini = req.miniTargetPeaks <= 0;
            int miniTargetPeaks;
            if (adaptiveMini) {
                const AudioRecord rec = req.fileId > 0
                    ? m_db->getFileById(req.fileId)
                    : m_db->getFileByPath(filePath);
                miniTargetPeaks = ComputeAdaptiveMiniPeakCount(rec.totalFrames);
            } else {
                miniTargetPeaks = req.miniTargetPeaks;
            }
            Database::MiniWaveformBundle bundle;
            if (m_db->getMiniWaveformBundleByPath(filePath, bundle, miniTargetPeaks) &&
                !bundle.peaks.empty()) {
                result.waveform.peaks = bundle.peaks;
                result.waveform.minPeaks =
                    (bundle.minPeaks.size() == bundle.peaks.size())
                        ? bundle.minPeaks : std::vector<float>();
                result.waveform.rms = bundle.rms;
                if (result.waveform.rms.size() != bundle.peaks.size()) {
                    result.waveform.rms.assign(bundle.peaks.size(), 0.0f);
                    for (size_t i = 0; i < bundle.peaks.size(); ++i)
                        result.waveform.rms[i] = bundle.peaks[i] * 0.35f;
                }
                result.waveform.samplesPerPeak = 1;
                result.waveform.numChannels = std::max(1, bundle.miniNumChannels);
                result.waveform.isStereo = result.waveform.numChannels >= 2;
                result.waveform.filePath = filePath;
                result.waveform.isReady = true;
                if (bundle.hasTrueRms) {
                    result.waveform.trueRmsDbfs = bundle.trueRmsDbfs;
                    result.waveform.hasTrueRms = true;
                }
                if (bundle.hasLoudness) {
                    result.waveform.integratedLUFS = bundle.integratedLUFS;
                    result.waveform.momentaryLUFS = bundle.momentaryLUFS;
                    result.waveform.shortTermLUFS = bundle.shortTermLUFS;
                    result.waveform.truePeakDBTP = bundle.truePeakDBTP;
                    result.waveform.hasLoudnessData = true;
                }
            }
            // Full-file decode for meters. This is the heavy work deferred off
            // the visible-mini path.
            WaveformData meterResult;
            if (AudioEngine::generateWaveformForFile(filePath, meterResult, 100, &m_cancelRequested,
                                                    false)) {
                result.waveform.trueRmsDbfs = meterResult.trueRmsDbfs;
                result.waveform.hasTrueRms = meterResult.hasTrueRms;
                if (meterResult.hasLoudnessData) {
                    result.waveform.integratedLUFS = meterResult.integratedLUFS;
                    result.waveform.momentaryLUFS = meterResult.momentaryLUFS;
                    result.waveform.shortTermLUFS = meterResult.shortTermLUFS;
                    result.waveform.truePeakDBTP = meterResult.truePeakDBTP;
                    result.waveform.hasLoudnessData = true;
                }
                if (!deferDbWrites) {
                    if (result.waveform.hasTrueRms && !result.waveform.peaks.empty()) {
                        m_db->saveWaveformByPath(filePath, result.waveform.peaks,
                                                 result.waveform.trueRmsDbfs,
                                                 result.waveform.numChannels);
                    }
                    if (result.waveform.hasLoudnessData) {
                        m_db->saveLoudnessByPath(filePath,
                                                 result.waveform.integratedLUFS,
                                                 result.waveform.momentaryLUFS,
                                                 result.waveform.shortTermLUFS,
                                                 result.waveform.truePeakDBTP);
                    }
                }
                if (m_cancelRequested.load(std::memory_order_acquire)) {
                    result.errorCode = "cancelled";
                } else {
                    result.success = true;
                    result.errorCode.clear();
                }
            } else {
                result.errorCode = m_cancelRequested.load(std::memory_order_acquire)
                    ? "cancelled"
                    : "meter_decode_failed";
            }
            result.fromCache = false;
            pushResult(std::move(result));
            continue;
        }

        if (m_db && req.buildDetail && req.fileId > 0) {
            const bool deferDbWrites = ShouldDeferWaveformDbWrites(m_db);
            if (deferDbWrites && req.hasBackfillJob) {
                result.errorCode = "deferred_scan_db_write";
                pushResult(std::move(result));
                continue;
            }
            const double durationSeconds = LookupWaveformDurationSeconds(m_db, req.fileId, filePath);
            const auto policy = WaveformCachePolicy::PersistentMainDetailForDuration(
                m_db->getWaveformCacheMode(),
                durationSeconds);
            if (!policy.persistMainDetail) {
                if (req.hasBackfillJob) {
                    result.success = true;
                    result.errorCode.clear();
                    pushResult(std::move(result));
                }
                continue;
            }
            WaveformExtractResult detail;
            if (WaveformExtractor::ExtractDetail(
                    filePath,
                    policy.targetPeaks,
                    true,
                    detail,
                    &m_cancelRequested) &&
                !detail.peaks.empty()) {
                if (!deferDbWrites &&
                    m_db->saveWaveformDetail(req.fileId, detail.peaks, detail.minPeaks, detail.channels)) {
                    // Adaptive DBs (miniTargetPeaks sentinel <=0) derive the mini
                    // peak count from the decoded source frames so detail-built
                    // minis match the SM-style density the extractor would emit.
                    const bool adaptiveMiniDetail = req.miniTargetPeaks <= 0;
                    const int miniTargetPeaks = adaptiveMiniDetail
                        ? ComputeAdaptiveMiniPeakCount(detail.decodedFrames)
                        : req.miniTargetPeaks;
                    float existingTrueRms = -200.0f;
                    int existingMiniChannels = 1;
                    const std::vector<float> existingMini =
                        m_db->getWaveformByPath(filePath, &existingTrueRms, &existingMiniChannels);
                    const int expectedMiniChannels =
                        req.miniPerChannel ? std::max(1, detail.channels) : 1;
                    const int expectedMiniValues = miniTargetPeaks * expectedMiniChannels;
                    // Adaptive count is a hint, not an exact contract: stored frame
                    // basis can differ from source frames, so validate structurally
                    // (non-empty, divisible by channel count) to avoid rebuild churn.
                    const bool miniShapeValid = adaptiveMiniDetail
                        ? (!existingMini.empty() &&
                           (existingMini.size() % (size_t)expectedMiniChannels) == 0 &&
                           std::max(1, existingMiniChannels) == expectedMiniChannels)
                        : ((int)existingMini.size() == expectedMiniValues &&
                           std::max(1, existingMiniChannels) == expectedMiniChannels);
                    if (!miniShapeValid) {
                        std::vector<float> miniFromDetail;
                        std::vector<float> miniMinFromDetail;
                        int miniChannels = 1;
                        if (WaveformExtractor::BuildMiniEnvelopeFromDetail(
                                detail.peaks,
                                detail.minPeaks,
                                std::max(1, detail.channels),
                                miniTargetPeaks,
                                req.miniPerChannel,
                                miniFromDetail,
                                miniMinFromDetail,
                                miniChannels) &&
                            !miniFromDetail.empty()) {
                            m_db->saveMiniWaveformEnvelopeByPath(filePath,
                                                                 miniFromDetail,
                                                                 miniMinFromDetail,
                                                                 -200.0f,
                                                                 miniChannels);
                        }
                    }
                    DIAG_ACTION_FMT("Waveform",
                        "AOTMainDetailCached: id=%lld path=%s peaks=%d channels=%d",
                        (long long)req.fileId, filePath.c_str(), (int)detail.peaks.size(), detail.channels);
                    WaveformCacheState cacheState;
                    std::string cacheKey;
                    if (m_db->getWaveformCacheStateByPath(filePath, cacheState)) {
                        cacheKey = cacheState.cacheKey;
                    }
                    if (cacheKey.empty()) {
                        const AudioRecord rec = m_db->getFileById(req.fileId);
                        if (rec.id > 0) {
                            cacheKey = WaveformExtractor::BuildCacheKey(
                                rec.filePath, rec.fileSize, rec.fileMtime);
                        }
                    }
                    if (!cacheKey.empty()) {
                        m_db->upsertWaveformCacheStateByPath(filePath, "done", "", 0, cacheKey);
                    }
                    if (req.hasBackfillJob) {
                        result.success = true;
                        result.errorCode.clear();
                        pushResult(std::move(result));
                    }
                } else if (deferDbWrites) {
                    if (req.hasBackfillJob) {
                        result.errorCode = "deferred_scan_db_write";
                        pushResult(std::move(result));
                    }
                } else {
                    result.errorCode = "detail_save_db_failed";
                    pushResult(std::move(result));
                }
            } else {
                result.errorCode = detail.errorCode.empty() ? "detail_extract_failed" : detail.errorCode;
                pushResult(std::move(result));
                DIAG_ACTION_FMT("Waveform",
                    "AOTMainDetailFailed: id=%lld path=%s error=%s",
                    (long long)req.fileId, filePath.c_str(), detail.errorCode.c_str());
            }
            continue;
        }

        if (m_db) {
            const bool deferDbWrites = ShouldDeferWaveformDbWrites(m_db);
            // Adaptive libraries (sentinel 0) size the mini by source frames, so the
            // expected peak count is per-file rather than a constant. Derive a best-effort
            // count from the stored frame total as a hint for the detail-derived fallback;
            // the actual cache check below is structural to tolerate small frame-basis drift.
            const bool adaptiveMini = req.miniTargetPeaks <= 0;
            int miniTargetPeaks;
            if (adaptiveMini) {
                const AudioRecord miniRec = req.fileId > 0
                    ? m_db->getFileById(req.fileId)
                    : m_db->getFileByPath(filePath);
                miniTargetPeaks = ComputeAdaptiveMiniPeakCount(miniRec.totalFrames);
            } else {
                miniTargetPeaks = req.miniTargetPeaks;
            }
            const bool miniPerChannel = req.miniPerChannel;
#if AURA_BUILD_DIAGNOSTICS
            const bool miniWorkerDiagEnabled = DiagnosticsLogger::instance().isEnabled();
            std::chrono::steady_clock::time_point miniWorkerStart;
            if (miniWorkerDiagEnabled) {
                miniWorkerStart = std::chrono::steady_clock::now();
            }
#endif
            Database::MiniWaveformBundle cachedBundle;
            const bool hasBundle = m_db->getMiniWaveformBundleByPath(filePath, cachedBundle, miniTargetPeaks);
            const std::vector<float>& cachedData = cachedBundle.peaks;
            const int cachedMiniChannels = cachedBundle.miniNumChannels;
            // Cache must match BOTH shape AND current channel-mode intent.
            // Without the mode check, a stereo cache row written in Music mode
            // (numChannels=2, size=miniPeaks*2) passes the SFX-mode shape check
            // (also miniPeaks*2 when cachedMiniChannels==2) and gets read back
            // as stereo, flipping mini render to two lanes in SFX. See diagnostic
            // strings below (chan_mismatch_*) for the originally intended guard.
            const bool cachedIsStereo = (cachedMiniChannels >= 2);
            // Adaptive blobs have a per-file peak count, so we validate structure
            // (non-empty, channel-mode match, size divisible by channel count) rather
            // than an exact count. Fixed-density libraries keep the strict count check.
            const int cachedChannelDivisor = std::max(1, cachedMiniChannels);
            const bool cacheShapeValid = adaptiveMini
                ? (hasBundle &&
                   !cachedData.empty() &&
                   (cachedIsStereo == miniPerChannel) &&
                   (cachedData.size() % (size_t)cachedChannelDivisor) == 0)
                : (hasBundle &&
                   !cachedData.empty() &&
                   (cachedIsStereo == miniPerChannel) &&
                   ((cachedMiniChannels <= 1 && (int)cachedData.size() == miniTargetPeaks) ||
                    (cachedMiniChannels > 1 && (int)cachedData.size() == miniTargetPeaks * cachedMiniChannels)));
#if AURA_BUILD_DIAGNOSTICS
            if (!cacheShapeValid && DiagnosticsLogger::instance().isEnabled()) {
                const char* reason = !hasBundle              ? "no_row"
                                   : cachedData.empty()      ? "empty_peaks"
                                   : (cachedMiniChannels > 1 && miniPerChannel == false) ? "chan_mismatch_mono_wanted"
                                   : (cachedMiniChannels <= 1 && miniPerChannel)         ? "chan_mismatch_stereo_wanted"
                                                            : "peak_count_mismatch";
                DIAG_ACTION_FMT("MiniWave",
                    "MiniCacheMiss reason=%s wantPeaks=%d gotPeaks=%d wantPerCh=%d gotChan=%d path=%s",
                    reason, miniTargetPeaks, (int)cachedData.size(),
                    miniPerChannel ? 1 : 0, cachedMiniChannels,
                    filePath.c_str());
            }
#endif
            if (cacheShapeValid) {
                result.waveform.peaks = cachedData;
                if (cachedBundle.minPeaks.size() == cachedData.size()) {
                    result.waveform.minPeaks = cachedBundle.minPeaks;
                } else {
                    result.waveform.minPeaks.clear();
                }

                const std::vector<float>& cachedRms = cachedBundle.rms;
                if (!cachedRms.empty() && cachedRms.size() == cachedData.size()) {
                    result.waveform.rms = cachedRms;
                } else {
                    result.waveform.rms.resize(cachedData.size());
                    for (size_t i = 0; i < cachedData.size(); ++i) {
                        result.waveform.rms[i] = cachedData[i] * 0.35f;
                    }
                }

                // True RMS dBFS from DB cache
                if (cachedBundle.hasTrueRms) {
                    result.waveform.trueRmsDbfs = cachedBundle.trueRmsDbfs;
                    result.waveform.hasTrueRms = true;
                }

                // LUFS/TruePeak from DB cache
                if (cachedBundle.hasLoudness) {
                    result.waveform.integratedLUFS = cachedBundle.integratedLUFS;
                    result.waveform.momentaryLUFS = cachedBundle.momentaryLUFS;
                    result.waveform.shortTermLUFS = cachedBundle.shortTermLUFS;
                    result.waveform.truePeakDBTP = cachedBundle.truePeakDBTP;
                    result.waveform.hasLoudnessData = true;
                }

                const bool needsMeterBackfill =
                    req.wantMeterData &&
                    (!result.waveform.hasTrueRms || !result.waveform.hasLoudnessData);
                if (needsMeterBackfill && !req.meterOnly) {
                    // Defer the heavy full-file decode; main thread enqueues a
                    // low-priority meterOnly follow-up so the cached mini shows
                    // immediately.
                    result.needsMeterFollowup = true;
                }

                result.waveform.samplesPerPeak = 1;
                result.waveform.numChannels = std::max(1, cachedMiniChannels);
                result.waveform.isStereo = result.waveform.numChannels >= 2;
                result.waveform.filePath = filePath;
                result.waveform.isReady = true;

                result.success = true;
                result.fromCache = true;
                result.errorCode.clear();
                if (req.allowSourceDecode &&
                    !deferDbWrites &&
                    m_db->getWaveformByPath(filePath).empty()) {
                    const float trueRms = cachedBundle.hasTrueRms
                        ? cachedBundle.trueRmsDbfs
                        : -200.0f;
                    m_db->saveWaveformByPath(filePath,
                                             cachedData,
                                             trueRms,
                                             std::max(1, cachedMiniChannels));
                }
                markBackfillMiniDone();
#if AURA_BUILD_DIAGNOSTICS
                if (miniWorkerDiagEnabled) {
                    RecordMiniWaveWorkerDiagnostics(
                        true,
                        std::chrono::duration<double, std::micro>(
                            std::chrono::steady_clock::now() - miniWorkerStart).count(),
                        result.waveform.numChannels,
                        (int)result.waveform.peaks.size());
                }
#endif
            } else {
                if (!req.allowSourceDecode) {
                    result.errorCode = "mini_cache_miss_no_decode";
                    pushResult(std::move(result));
                    continue;
                }
#if AURA_BUILD_DIAGNOSTICS
                const auto miniPhaseStart = std::chrono::steady_clock::now();
#endif
                WaveformExtractResult mini;
                HarkenSearch::MediaFoundationAudio::DecodedAudioBuffer miniDecoded;
                bool genOk = WaveformExtractor::ExtractMiniReturningDecoded(
                    filePath,
                    miniTargetPeaks,
                    mini,
                    miniPerChannel,
                    &m_cancelRequested,
                    miniDecoded
                );
#if AURA_BUILD_DIAGNOSTICS
                const double miniPhaseUs = std::chrono::duration<double, std::micro>(
                    std::chrono::steady_clock::now() - miniPhaseStart).count();
                double lufsPhaseUs = 0.0;
                double detailPhaseUs = 0.0;
                double dbPhaseUs = 0.0;
#endif

                if (genOk && !mini.peaks.empty()) {
#if AURA_BUILD_DIAGNOSTICS
                    if (DiagnosticsLogger::instance().isEnabled()) {
                        const int chanForDur = std::max(1, mini.channels);
                        const int peakFrames = (int)mini.peaks.size() / chanForDur;
                        const double approxDurSec = (mini.sampleRate > 0)
                            ? (double)peakFrames * (double)mini.samplesPerPeak / (double)mini.sampleRate
                            : 0.0;
                        DIAG_ACTION_FMT("MiniWave",
                            "SourceDecoded peaks=%d chan=%d path=%s",
                            (int)mini.peaks.size(), mini.channels, filePath.c_str());
                        DIAG_ACTION_FMT("MiniWave",
                            "MiniExtracted peaks=%d chan=%d samplesPerPeak=%d sr=%u approxDurSec=%.2f miniUs=%.0f path=%s",
                            (int)mini.peaks.size(), mini.channels, mini.samplesPerPeak,
                            mini.sampleRate, approxDurSec, miniPhaseUs, filePath.c_str());
                    }
#endif
                    result.waveform.peaks = std::move(mini.peaks);
                    result.waveform.minPeaks = std::move(mini.minPeaks);
                    if (result.waveform.minPeaks.size() != result.waveform.peaks.size()) {
                        result.waveform.minPeaks.clear();
                    }
                    result.waveform.rms = std::move(mini.rms);

                    if (result.waveform.rms.empty()) {
                        result.waveform.rms.resize(result.waveform.peaks.size());
                        for (size_t i = 0; i < result.waveform.peaks.size(); ++i) {
                            result.waveform.rms[i] = result.waveform.peaks[i] * 0.35f;
                        }
                    }

                    result.waveform.samplesPerPeak = mini.samplesPerPeak;
                    result.waveform.numChannels = std::max(1, mini.channels);
                    result.waveform.isStereo = result.waveform.numChannels >= 2;
                    result.waveform.filePath = filePath;
                    result.waveform.hasLoudnessData = false;  // populated below (MF: inline; non-MF: deferred)
                    result.waveform.isReady = true;

                    if (req.wantMeterData) {
                        if (!miniDecoded.interleaved.empty() && miniDecoded.totalFrames > 0) {
                            // MF format: the full file is already decoded — compute
                            // meters from the same buffer (one decode total, no deferral).
                            WaveformData meterResult;
                            if (AudioEngine::computeWaveformAndLoudnessFromSamples(
                                    miniDecoded.interleaved.data(), miniDecoded.totalFrames,
                                    miniDecoded.channels, miniDecoded.sampleRate,
                                    100, meterResult, &m_cancelRequested)) {
                                result.waveform.trueRmsDbfs = meterResult.trueRmsDbfs;
                                result.waveform.hasTrueRms = meterResult.hasTrueRms;
                                if (meterResult.hasLoudnessData) {
                                    result.waveform.integratedLUFS = meterResult.integratedLUFS;
                                    result.waveform.momentaryLUFS = meterResult.momentaryLUFS;
                                    result.waveform.shortTermLUFS = meterResult.shortTermLUFS;
                                    result.waveform.truePeakDBTP = meterResult.truePeakDBTP;
                                    result.waveform.hasLoudnessData = true;
                                    if (!deferDbWrites) {
                                        m_db->saveLoudnessByPath(filePath,
                                            meterResult.integratedLUFS, meterResult.momentaryLUFS,
                                            meterResult.shortTermLUFS, meterResult.truePeakDBTP);
                                    }
                                }
                            }
                        } else {
                            // Non-MF (cheap-mini) format: defer the full decode (Lever A).
                            result.needsMeterFollowup = true;
                        }
                    }

#if AURA_BUILD_DIAGNOSTICS
                    const auto dbStart = std::chrono::steady_clock::now();
#endif
                    float trueRms = result.waveform.hasTrueRms ? result.waveform.trueRmsDbfs : -200.0f;
                    if (deferDbWrites) {
                        result.errorCode.clear();
                    } else {
                        const bool hasMiniEnvelope =
                            result.waveform.minPeaks.size() == result.waveform.peaks.size();
                        const bool saved = hasMiniEnvelope
                            ? m_db->saveMiniWaveformEnvelopeByPath(filePath,
                                                                   result.waveform.peaks,
                                                                   result.waveform.minPeaks,
                                                                   trueRms,
                                                                   result.waveform.numChannels)
                            : m_db->saveWaveformByPath(filePath,
                                                       result.waveform.peaks,
                                                       trueRms,
                                                       result.waveform.numChannels);
                        if (!saved) {
                            result.errorCode = "mini_save_db_failed";
                        } else {
                            result.errorCode.clear();
                        }
                    }
#if AURA_BUILD_DIAGNOSTICS
                    dbPhaseUs += std::chrono::duration<double, std::micro>(
                        std::chrono::steady_clock::now() - dbStart).count();
#endif

                    if (req.buildDetail) {
                        if (req.fileId > 0 || !m_db->hasWaveformDetailByPath(filePath)) {
#if AURA_BUILD_DIAGNOSTICS
                            const auto detailStart = std::chrono::steady_clock::now();
#endif
                            const double durationSeconds =
                                LookupWaveformDurationSeconds(m_db, req.fileId, filePath);
                            const auto policy =
                                WaveformCachePolicy::PersistentMainDetailForDuration(
                                    m_db->getWaveformCacheMode(),
                                    durationSeconds);
                            WaveformExtractResult detail;
                            if (policy.persistMainDetail &&
                                WaveformExtractor::ExtractDetail(
                                    filePath,
                                    policy.targetPeaks,
                                    true,
                                    detail,
                                    &m_cancelRequested) &&
                                !detail.peaks.empty()) {
                                const bool saved = !deferDbWrites && ((req.fileId > 0)
                                    ? m_db->saveWaveformDetail(req.fileId, detail.peaks, detail.minPeaks, detail.channels)
                                    : m_db->saveWaveformDetailByPath(filePath, detail.peaks, detail.minPeaks, detail.channels));
                                if (saved) {
                                    DIAG_ACTION_FMT("Waveform",
                                        "AOTMainDetailCached: id=%lld path=%s peaks=%d channels=%d",
                                        (long long)req.fileId, filePath.c_str(), (int)detail.peaks.size(), detail.channels);
                                }
                            } else {
                                DIAG_ACTION_FMT("Waveform",
                                    "AOTMainDetailFailed: path=%s error=%s",
                                    filePath.c_str(),
                                    policy.persistMainDetail ? detail.errorCode.c_str() : "detail_persist_skipped_by_duration");
                            }
#if AURA_BUILD_DIAGNOSTICS
                            detailPhaseUs = std::chrono::duration<double, std::micro>(
                                std::chrono::steady_clock::now() - detailStart).count();
#endif
                        }
                    }

                    result.success = true;
                    result.fromCache = false;
                    if (result.errorCode.empty()) {
                        markBackfillMiniDone();
                    }
#if AURA_BUILD_DIAGNOSTICS
                    if (miniWorkerDiagEnabled) {
                        RecordMiniWaveWorkerDiagnostics(
                            false,
                            std::chrono::duration<double, std::micro>(
                                std::chrono::steady_clock::now() - miniWorkerStart).count(),
                            result.waveform.numChannels,
                            (int)result.waveform.peaks.size());
                        DIAG_ACTION_FMT("MiniWave",
                            "PhaseBreakdown miniUs=%.0f lufsUs=%.0f detailUs=%.0f dbUs=%.0f buildDetail=%d wantMeterData=%d path=%s",
                            miniPhaseUs, lufsPhaseUs, detailPhaseUs, dbPhaseUs,
                            req.buildDetail ? 1 : 0,
                            req.wantMeterData ? 1 : 0,
                            filePath.c_str());
                    }
#endif
                } else if (!genOk) {
                    result.errorCode = mini.errorCode.empty() ? "mini_extract_failed" : mini.errorCode;
                } else {
                    result.errorCode = "mini_waveform_empty";
                }
            }
        } else {
            result.errorCode = "db_unavailable";
        }

        // Release before pushing so the main thread never observes this path
        // in m_processingSet while draining the result (which would cause
        // submitMeterRequest to be silently rejected).
        procGuard.release();

        if (req.hasBackfillJob ||
            req.generation == m_generation.load(std::memory_order_relaxed)) {
            std::lock_guard<std::mutex> lk(m_resultMutex);
            m_resultQueue.push(std::move(result));
        }
    }
}

void WaveformWorker::pushResult(WaveformResult&& res) {
    if (!res.hasBackfillJob &&
        res.generation != m_generation.load(std::memory_order_relaxed)) return;
    std::lock_guard<std::mutex> lk(m_resultMutex);
    m_resultQueue.push(std::move(res));
}

// SearchWorker Implementation

SearchWorker::SearchWorker() {}

SearchWorker::~SearchWorker() {
    stop();
}

void SearchWorker::start(Database* db) {
    // Legacy entry point: open a dedicated read-only connection from the database path.
    if (!db || !db->isOpen()) return;
    start(db->getMainDbPath());
}

void SearchWorker::start(const std::string& dbPath) {
    if (m_running.load()) return;
    
    m_ownedDb = std::make_unique<Database>();
    // 后台搜索使用只读连接，禁止写锁，避免与主线程争用 WAL
    if (!m_ownedDb->openReadOnly(dbPath)) {
        m_ownedDb.reset();
        m_db = nullptr;
        return;
    }
    m_db = m_ownedDb.get();
    m_facetCache.clear();

    // Install SQLite progress handler: every 2000 VDBE ops, check if a newer
    // request arrived. If so, return non-zero to abort the current query.
    // This makes long-running facet queries (GROUP BY on full library) interruptible
    // within ~1-2ms instead of waiting for the entire query to finish.
    if (m_db) {
        m_db->setQueryProgressHandler(2000,
            [](void* ctx) -> int {
                auto* self = static_cast<SearchWorker*>(ctx);
                // Lock-free check: m_latestRequestId advances when new request arrives,
                // m_currentProcessingId is set when worker starts processing a request.
                return self->m_latestRequestId.load(std::memory_order_acquire)
                     > self->m_currentProcessingId.load(std::memory_order_relaxed) ? 1 : 0;
            }, this);
    }

    // Pre-warm cover key indexes: touch root/first pages of cover filter indexes
    // so subsequent cover clicks don't hit cold mmap pages.
    if (m_db) {
        m_db->warmupCoverIndexes();
    }

    m_running.store(true);
    m_thread = std::thread(&SearchWorker::workerThread, this);
}

void SearchWorker::stop() {
    if (!m_running.load()) return;
    m_running.store(false);
    // Interrupt any in-flight sqlite3_step BEFORE join, otherwise a long FTS5
    // facet/count query on a million-row DB blocks join() for seconds.
    if (m_db) m_db->interrupt();
    m_requestCv.notify_all();
    if (m_thread.joinable()) m_thread.join();
    // Unregister progress handler before releasing DB (prevents dangling callback)
    if (m_db) m_db->clearQueryProgressHandler();
    // Release the read-only connection only after the worker thread stops.
    m_facetCache.clear();
    m_db = nullptr;
    m_ownedDb.reset();
}

uint64_t SearchWorker::submitSearch(const AsyncSearchRequest& request) {
    uint64_t id = m_nextRequestId.fetch_add(1);
    {
        std::lock_guard<std::mutex> lk(m_requestMutex);
        m_pendingRequest = request;
        m_pendingRequest.requestId = id;
        m_hasPendingRequest = true;
        m_latestRequestId.store(id);
    }
    // Interrupt the current SQLite query so the newest request can run.

    if (m_db) {
        m_db->interrupt();
    }
    m_requestCv.notify_one();
    return id;
}

bool SearchWorker::hasResult() {
    std::lock_guard<std::mutex> lk(m_resultMutex);
    return !m_completedQueue.empty();
}

void SearchWorker::cancelActiveSearch() {
    const uint64_t cancelId = m_nextRequestId.fetch_add(1);
    {
        std::lock_guard<std::mutex> lk(m_requestMutex);
        m_hasPendingRequest = false;
        m_latestRequestId.store(cancelId, std::memory_order_release);
    }
    {
        std::lock_guard<std::mutex> lk(m_resultMutex);
        m_completedQueue.clear();
    }
    if (m_db) {
        m_db->interrupt();
    }
    m_requestCv.notify_one();
}

bool SearchWorker::pollResult(AsyncSearchResult& outResult, uint64_t expectedRequestId) {
    std::lock_guard<std::mutex> lk(m_resultMutex);
    if (m_completedQueue.empty()) return false;
    for (auto it = m_completedQueue.begin(); it != m_completedQueue.end(); ++it) {
        if (it->requestId == expectedRequestId) {
            outResult = std::move(*it);
            m_completedQueue.erase(it);
            return true;
        }
        if (it->requestId < expectedRequestId) {
            it = m_completedQueue.erase(it);
            if (it == m_completedQueue.end()) break;
        }
    }
    return false;
}

void SearchWorker::workerThread() {
    while (m_running.load()) {
        AsyncSearchRequest req;
        
        // Wait for a request.
        {
            std::unique_lock<std::mutex> lk(m_requestMutex);
            m_requestCv.wait_for(lk, std::chrono::milliseconds(200), [this] {
                return m_hasPendingRequest || !m_running.load();
            });
            
            if (!m_running.load()) break;
            if (!m_hasPendingRequest) continue;
            
            req = m_pendingRequest;
            m_hasPendingRequest = false;
        }
        
        m_searching.store(true);
        m_currentProcessingId.store(req.requestId, std::memory_order_release);
        // 执行查询
        AsyncSearchResult result;
        result.requestId = req.requestId;
        result.kind = req.kind;
        result.filter = req.filter;
        result.viewMode = req.viewMode;
        result.playlistId = req.playlistId;
        result.playlistAggregatedView = req.playlistAggregatedView;
        result.folderFilterActive = req.folderFilterActive;
        result.selectedFolderPath = req.selectedFolderPath;
        result.countFollowUpSkipped = req.skipDeferredCountFollowUp;
        // Preserve the request's random-shuffle flag in the result.
        result.isRandomShuffle = req.isRandomShuffle;
        // Carry the submit-time database revision with the result so the
        // apply site records the revision represented by these rows.

        result.requestedAtDbRevision = req.requestedAtDbRevision;
        result.success = false;

        auto tryLoadFacetCache = [&](const std::string& facetCacheKey,
                                     std::vector<Database::CoverFacetEntry>& coverFacets,
                                     std::vector<Database::PathFacetEntry>& pathFacets) -> bool {
            if (facetCacheKey.empty()) return false;
            auto it = std::find_if(m_facetCache.begin(), m_facetCache.end(),
                [&](const SearchWorker::FacetCacheEntry& e) { return e.key == facetCacheKey; });
            if (it == m_facetCache.end()) return false;

            coverFacets = it->coverFacets;
            pathFacets = it->pathFacets;
            if (it != m_facetCache.begin()) {
                SearchWorker::FacetCacheEntry hot = std::move(*it);
                m_facetCache.erase(it);
                m_facetCache.push_front(std::move(hot));
            }
            DIAG_ACTION_FMT("Search", "WorkerFacetCacheHit: cover=%zu path=%zu",
                            coverFacets.size(), pathFacets.size());
            return true;
        };

        auto storeFacetCache = [&](const std::string& facetCacheKey,
                                   const std::vector<Database::CoverFacetEntry>& coverFacets,
                                   const std::vector<Database::PathFacetEntry>& pathFacets) {
            if (facetCacheKey.empty()) return;
            FacetCacheEntry entry;
            entry.key = facetCacheKey;
            entry.coverFacets = coverFacets;
            entry.pathFacets = pathFacets;
            m_facetCache.push_front(std::move(entry));
            while (m_facetCache.size() > kFacetCacheMaxEntries) {
                m_facetCache.pop_back();
            }
        };

        auto isRequestStale = [&]() -> bool {
            return req.requestId < m_latestRequestId.load(std::memory_order_acquire);
        };
        
        int64_t totalCountLocal = 0;
        if (m_db) {
            try {
                const bool hasKeywordFilter = !req.filter.keyword.empty();
                const bool hasStructuralFilter =
                    req.folderFilterActive ||
                    req.hasAdvancedFilters ||
                    req.hasFileTypeFilter;
                const bool filteredFacetWarmStart =
                    req.viewMode == 1 &&
                    req.kind == AsyncSearchKind::Refresh &&
                    req.deferCount &&
                    req.needFacets &&
                    !req.deferFacets &&
                    req.filter.specificCoverKey.empty() &&
                    !hasKeywordFilter &&
                    hasStructuralFilter;
                bool filteredFacetWarmStartApplied = false;
                if (filteredFacetWarmStart) {
                    AsyncSearchResult warmStart;
                    warmStart.requestId = req.requestId;
                    warmStart.kind = AsyncSearchKind::CountAndFacetsOnly;
                    warmStart.filter = req.filter;
                    warmStart.viewMode = req.viewMode;
                    warmStart.playlistId = req.playlistId;
                    warmStart.playlistAggregatedView = req.playlistAggregatedView;
                    warmStart.folderFilterActive = req.folderFilterActive;
                    warmStart.selectedFolderPath = req.selectedFolderPath;
                    warmStart.requestedAtDbRevision = req.requestedAtDbRevision;
                    warmStart.totalCount = m_db->searchFilesCount(req.filter);
                    totalCountLocal = warmStart.totalCount;
                    if (req.needFacets && totalCountLocal <= 2000000) {
                        const std::string facetCacheKey =
                            ShouldUseWorkerFacetCache(req, totalCountLocal)
                                ? BuildFacetCacheKey(req, totalCountLocal)
                                : std::string();
                        if (!tryLoadFacetCache(facetCacheKey, warmStart.coverFacets, warmStart.pathFacets)) {
                            warmStart.coverFacets = m_db->getCoverFacets(req.filter, kFilteredCoverWallFacetLimit);
                            warmStart.pathFacets = m_db->getPathFacets(req.filter, 300);
                            storeFacetCache(facetCacheKey, warmStart.coverFacets, warmStart.pathFacets);
                        }
                        warmStart.facetsComputed = true;
                    }
                    warmStart.success = true;
                    if (req.requestId >= m_latestRequestId.load(std::memory_order_acquire)) {
                        std::lock_guard<std::mutex> lk(m_resultMutex);
                        m_completedQueue.push_back(std::move(warmStart));
                        filteredFacetWarmStartApplied = true;
                    }
                }

                if (isRequestStale()) {
                    DIAG_ACTION_FMT("Search",
                                    "SkipStaleBeforeRows req=%llu kind=%d",
                                    (unsigned long long)req.requestId,
                                    (int)req.kind);
                    m_searching.store(false);
                    continue;
                }

                if (req.viewMode == 1 && req.kind == AsyncSearchKind::CountAndFacetsOnly) {
                    result.totalCount = m_db->searchFilesCount(req.filter);
                    result.results = {};
                    result.loadedCount = 0;
                    result.hasMore = false;
                    result.success = true;
                    totalCountLocal = result.totalCount;
                    if (req.needFacets && totalCountLocal <= 2000000) {
                        const bool allowFacetCache = ShouldUseWorkerFacetCache(req, totalCountLocal);
                        const std::string facetCacheKey =
                            allowFacetCache ? BuildFacetCacheKey(req, totalCountLocal) : std::string();
                        if (!tryLoadFacetCache(facetCacheKey, result.coverFacets, result.pathFacets)) {
                            result.coverFacets = m_db->getCoverFacets(req.filter, kFilteredCoverWallFacetLimit);
                            result.pathFacets = m_db->getPathFacets(req.filter, 300);
                            storeFacetCache(facetCacheKey, result.coverFacets, result.pathFacets);
                        }
                        result.facetsComputed = true;
                    }
                    if (req.requestId >= m_latestRequestId.load()) {
                        std::lock_guard<std::mutex> lk(m_resultMutex);
                        m_completedQueue.push_back(std::move(result));
                    }
                    m_searching.store(false);
                    continue;
                }
                if (req.viewMode == 1) { // ViewMode::Database
                    if (req.isRandomShuffle) {
                        if (isRequestStale()) {
                            DIAG_ACTION_FMT("Search", "SkipStaleBeforeRandom req=%llu",
                                            (unsigned long long)req.requestId);
                            m_searching.store(false);
                            continue;
                        }
                        result.results = m_db->searchFilesRandom(req.filter, req.limit);
                        result.totalCount = result.results.size();
                        result.loadedCount = (int)result.results.size();
                        result.hasMore = false;
                    } else if (req.deferCount && !filteredFacetWarmStartApplied) {
                        result.totalCount = -1;
                        if (isRequestStale()) {
                            DIAG_ACTION_FMT("Search", "SkipStaleBeforeDeferredRows req=%llu",
                                            (unsigned long long)req.requestId);
                            m_searching.store(false);
                            continue;
                        }
                        auto t_search = std::chrono::steady_clock::now();
                        result.results = m_db->searchFiles(req.filter, req.limit, req.offset);
                        auto t_after_search = std::chrono::steady_clock::now();
                        result.loadedCount = (int)result.results.size();
                        auto search_ms = std::chrono::duration<double, std::milli>(t_after_search - t_search).count();
                        AppendDbQueryPerfLog("[DBQuery] searchFiles (deferred): %.2f ms, items=%d\n",
                                             search_ms, result.loadedCount);
                        result.hasMore = (req.limit <= 0 || result.loadedCount > 0);
                    } else if (req.deferCount && filteredFacetWarmStartApplied) {
                        result.totalCount = totalCountLocal;
                        if (isRequestStale()) {
                            DIAG_ACTION_FMT("Search", "SkipStaleBeforeWarmStartRows req=%llu",
                                            (unsigned long long)req.requestId);
                            m_searching.store(false);
                            continue;
                        }
                        auto t_search = std::chrono::steady_clock::now();
                        result.results = m_db->searchFiles(req.filter, req.limit, req.offset);
                        auto t_after_search = std::chrono::steady_clock::now();
                        result.loadedCount = (int)result.results.size();
                        auto search_ms = std::chrono::duration<double, std::milli>(t_after_search - t_search).count();
                        AppendDbQueryPerfLog("[DBQuery] searchFiles (facet warm-start): %.2f ms, items=%d, total=%lld\n",
                                             search_ms, result.loadedCount, result.totalCount);
                        result.hasMore = (req.limit <= 0 || result.loadedCount > 0) &&
                                         (req.offset + result.loadedCount < result.totalCount);
                    } else {
                        if (isRequestStale()) {
                            DIAG_ACTION_FMT("Search", "SkipStaleBeforeCount req=%llu",
                                            (unsigned long long)req.requestId);
                            m_searching.store(false);
                            continue;
                        }
                        auto t_count = std::chrono::steady_clock::now();
                        result.totalCount = m_db->searchFilesCount(req.filter);
                        auto t_after_count = std::chrono::steady_clock::now();
                        auto count_ms = std::chrono::duration<double, std::milli>(t_after_count - t_count).count();
                        totalCountLocal = result.totalCount;

                        if (isRequestStale()) {
                            DIAG_ACTION_FMT("Search", "SkipStaleAfterCount req=%llu total=%lld",
                                            (unsigned long long)req.requestId,
                                            (long long)result.totalCount);
                            m_searching.store(false);
                            continue;
                        }
                        auto t_search = std::chrono::steady_clock::now();
                        result.results = m_db->searchFiles(req.filter, req.limit, req.offset);
                        auto t_after_search = std::chrono::steady_clock::now();
                        result.loadedCount = (int)result.results.size();
                        auto search_ms = std::chrono::duration<double, std::milli>(t_after_search - t_search).count();

                        AppendDbQueryPerfLog("[DBQuery] searchFilesCount: %.2f ms, searchFiles: %.2f ms, items=%d, total=%lld\n",
                                             count_ms, search_ms, result.loadedCount, result.totalCount);

                        result.hasMore = (req.limit <= 0 || result.loadedCount > 0) &&
                                         (req.offset + result.loadedCount < result.totalCount);
                    }
                } else if (req.viewMode == 3 && req.playlistId > 0) { // ViewMode::Playlist
                    if (req.playlistAggregatedView) {
                        result.totalCount = m_db->searchAggregatedPlaylistFileCount(req.playlistId, req.filter);
                        result.results = m_db->searchAggregatedPlaylistFilesPage(req.playlistId, req.filter, req.limit, req.offset);
                    } else {
                        result.totalCount = m_db->searchPlaylistFileCount(req.playlistId, req.filter);
                        result.results = m_db->searchPlaylistFilesPage(req.playlistId, req.filter, req.limit, req.offset);
                    }
                    result.loadedCount = (int)result.results.size();
                    result.hasMore = req.offset + result.loadedCount < result.totalCount;
                    if (req.needFacets && result.totalCount <= 2000000) {
                        if (!isRequestStale()) {
                            const std::vector<AudioRecord> completeMembers = req.playlistAggregatedView
                                ? m_db->searchAggregatedPlaylistCoverKeys(req.playlistId, req.filter)
                                : m_db->searchPlaylistCoverKeys(req.playlistId, req.filter);
                            if (!isRequestStale()) {
                                const auto groups = aura::playlist::BuildPlaylistCoverGroupsFromCompleteMembers(
                                    completeMembers, result.results);
                                result.coverFacets.clear();
                                result.coverFacets.reserve(groups.size());
                                for (const auto& group : groups) {
                                    Database::CoverFacetEntry entry;
                                    entry.key = group.key;
                                    entry.count = group.hitCount;
                                    result.coverFacets.push_back(std::move(entry));
                                }
                                result.facetsComputed = true;
                            }
                        }
                    }
                    DIAG_ACTION_FMT(
                        "Playlist",
                        "WorkerFetch req=%llu playlist=%lld aggregated=%d keyword=%d limit=%d offset=%d loaded=%d total=%lld hasMore=%d",
                        (unsigned long long)req.requestId,
                        (long long)req.playlistId,
                        req.playlistAggregatedView ? 1 : 0,
                        req.filter.keyword.empty() ? 0 : 1,
                        req.limit,
                        req.offset,
                        result.loadedCount,
                        (long long)result.totalCount,
                        result.hasMore ? 1 : 0);
                } else if (req.viewMode == 4) { // ViewMode::PlaylistLibrary
                    result.totalCount = m_db->searchAllPlaylistFileCount(req.filter);
                    result.results = m_db->searchAllPlaylistFilesPage(req.filter, req.limit, req.offset);
                    result.loadedCount = (int)result.results.size();
                    result.hasMore = req.offset + result.loadedCount < result.totalCount;
                    DIAG_ACTION_FMT(
                        "Playlist",
                        "WorkerFetchAllPlaylists req=%llu keyword=%d limit=%d offset=%d loaded=%d total=%lld hasMore=%d",
                        (unsigned long long)req.requestId,
                        req.filter.keyword.empty() ? 0 : 1,
                        req.limit,
                        req.offset,
                        result.loadedCount,
                        (long long)result.totalCount,
                        result.hasMore ? 1 : 0);
                }
                result.success = true;
            } catch (...) {
                result.success = false;
            }
        }
        totalCountLocal = result.totalCount;
        // Publish rows immediately; count and facets can follow.
        if (req.requestId >= m_latestRequestId.load()) {
            std::lock_guard<std::mutex> lk(m_resultMutex);
            m_completedQueue.push_back(std::move(result));
        }

        // Compute count and facets after publishing the rows.
        if (m_db && req.viewMode == 1 && req.kind == AsyncSearchKind::Refresh &&
            (req.deferCount || req.needFacets)) {
            const bool hasKeywordFilter = !req.filter.keyword.empty();
            const bool hasStructuralFilter =
                req.folderFilterActive ||
                req.hasAdvancedFilters ||
                req.hasFileTypeFilter;
            const bool filteredFacetWarmStartApplied =
                req.deferCount &&
                req.needFacets &&
                !req.deferFacets &&
                req.filter.specificCoverKey.empty() &&
                !hasKeywordFilter &&
                hasStructuralFilter;
            if (filteredFacetWarmStartApplied) {
                m_searching.store(false);
                continue;
            }
            if (req.deferCount && req.skipDeferredCountFollowUp && !req.needFacets) {
                DIAG_ACTION_FMT("Search",
                                "SkipDeferredCountFollowUp: req=%llu viewMode=%d",
                                (unsigned long long)req.requestId,
                                req.viewMode);
                m_searching.store(false);
                continue;
            }
            if (req.deferCount && !req.needFacets && !req.filter.specificCoverKey.empty()) {
                m_searching.store(false);
                continue;
            }
            // Drop this response if a newer request arrived while the query ran.
            if (req.requestId < m_latestRequestId.load(std::memory_order_acquire)) {
                m_searching.store(false);
                continue; 
            }

            AsyncSearchResult followup;
            followup.requestId = req.requestId;
            followup.kind = AsyncSearchKind::CountAndFacetsOnly;
            followup.filter = req.filter;
            followup.viewMode = req.viewMode;
            followup.playlistId = req.playlistId;
            followup.playlistAggregatedView = req.playlistAggregatedView;
            followup.folderFilterActive = req.folderFilterActive;
            followup.selectedFolderPath = req.selectedFolderPath;
            followup.success = false;

            try {
                // Avoid repeating disk I/O for a cover index that is already warm.
                if (req.deferCount) {
                    followup.totalCount = m_db->searchFilesCount(req.filter);
                    totalCountLocal = followup.totalCount;
                } else {
                    // Reuse the count already computed for the first response.
                    followup.totalCount = totalCountLocal;
                }

                // Run the expensive cover and path GROUP BY facets.
                if (req.needFacets && !req.deferFacets && totalCountLocal <= 2000000) {
                    const bool allowFacetCache = ShouldUseWorkerFacetCache(req, totalCountLocal);
                    const std::string facetCacheKey = allowFacetCache ? BuildFacetCacheKey(req, totalCountLocal) : std::string();

                    if (!tryLoadFacetCache(facetCacheKey, followup.coverFacets, followup.pathFacets)) {
                        // Early abort: skip expensive facets if a newer request arrived
                        if (req.requestId < m_latestRequestId.load(std::memory_order_acquire)) {
                            m_searching.store(false);
                            continue;
                        }
                        followup.coverFacets = m_db->getCoverFacets(req.filter, kFilteredCoverWallFacetLimit);
                        if (req.requestId >= m_latestRequestId.load(std::memory_order_acquire)) {
                            followup.pathFacets = m_db->getPathFacets(req.filter, 300);
                        }

                        storeFacetCache(facetCacheKey, followup.coverFacets, followup.pathFacets);
                    }
                    followup.facetsComputed = true;
                }
                followup.success = true;
            } catch (...) {
                followup.success = false;
            }

            // Publish the follow-up response with the final total count.
            if (followup.success && req.requestId >= m_latestRequestId.load(std::memory_order_acquire)) {
                std::lock_guard<std::mutex> lk(m_resultMutex);
                m_completedQueue.push_back(std::move(followup));
            }

            // Re-warm cover indexes because count and facet queries may evict their pages.
            // pages from SQLite page cache. Re-warm so the next cover click is instant.
            if (m_db && req.requestId >= m_latestRequestId.load(std::memory_order_acquire)) {
                m_db->warmupCoverIndexes();
            }
        }

        m_searching.store(false);
    }
}
