#pragma once
// SoundManager - 异步工作线程池
// AsyncWorker.h - 波形加载、搜索查询的异步处理
//
// 设计原则：
// 1. 所有 I/O 密集操作（数据库读取、音频解码）在后台线程执行
// 2. 主线程仅提交请求和消费结果，严禁阻塞超过 16ms (60fps)
// 3. 使用 mutex + condition_variable 实现线程安全队列

#include <string>
#include <vector>
#include <queue>
#include <mutex>
#include <deque>
#include <condition_variable>
#include <thread>
#include <atomic>
#include <functional>
#include <unordered_map>
#include <unordered_set>
#include <chrono>
#include <future>
#include <memory>

#include "AudioEngine.h"

// @stage7-contract
// API contract: AsyncWorker owns its worker threads and joins them during
// shutdown. Main/UI callers enqueue requests and consume copied results without
// blocking beyond the documented handoff; worker callbacks do not borrow queue
// storage after return.
#include "Database.h"      // For SearchFilter, AudioRecord (used by value in AsyncSearchRequest)
#include "BackfillQueue.h"

// Forward declarations
struct AppState;

// Waveform Async Worker

struct WaveformResult {
    std::string filePath;
    WaveformData waveform;
    bool success = false;
    int64_t fileId = 0;
    bool cacheOnly = false;
    bool hasBackfillJob = false;
    aura::BackfillJob backfillJob{};
    // Whether the data was from DB cache (true) or newly generated (false)
    bool fromCache = false;
    // Diagnostic reason when success == false.
    std::string errorCode;
    uint64_t generation = 0;
    // Set when the worker intentionally deferred meter (LUFS/RMS/TruePeak)
    // off the visible-mini path. Main thread submits a low-priority meterOnly
    // follow-up in response.
    bool needsMeterFollowup = false;
    // Set for meterOnly follow-up results. The main thread must merge metric
    // fields into an existing mini waveform instead of replacing the mini data.
    bool metersOnlyMerge = false;
};

class WaveformWorker {
public:
    WaveformWorker();
    ~WaveformWorker();

    // 初始化工作线程（调用一次）
    void start(Database* db, int threadCount = 2);
    void ensureThreadCount(int threadCount);
    
    // 停止所有线程
    void stop();
    
    // 提交波形加载请求（主线程调用）
    // 返回 false 如果该路径已在队列中或已在处理中
    bool submitRequest(const std::string& filePath);
    bool submitVisibleRequest(const std::string& filePath);
    bool submitVisibleCacheRequest(const std::string& filePath);
    bool submitDetailRequest(const std::string& filePath, int64_t fileId = 0);
    bool submitBackfillDetailRequest(const std::string& filePath, const aura::BackfillJob& job);
    // Submit a low-priority meter-only follow-up (deduped on the main thread).
    // Reads the cached mini envelope and only computes + persists meter data.
    bool submitMeterRequest(const std::string& filePath, int64_t fileId = 0);

    // 取消所有待处理请求（例如搜索结果变化时）
    void cancelAllPending();
    
    // 从完成队列取出结果（主线程每帧调用）
    // maxResults: 每帧最多处理的结果数
    // 返回实际取出的结果数
    int pollResults(std::vector<WaveformResult>& outResults, int maxResults = 3);
    
    // 检查是否有待处理的请求
    bool hasPendingRequests() const;
    
    // 获取待处理数量
    int pendingCount() const;

    // 主/detail busy 时由帧循环置位；限流解除时 notify_all，让等在 CV 上的 worker 立刻重评谓词（无须等超时）。
    void setThrottleMode(bool throttled) {
        const bool wasThrottled = m_throttled.exchange(throttled, std::memory_order_release);
        if (wasThrottled && !throttled) {
            m_requestCv.notify_all();
        }
    }

private:
    struct WaveformRequest {
        std::string filePath;
        int64_t fileId = 0;
        uint64_t generation = 0;
        bool buildDetail = false;
        int priority = 0;
        int miniTargetPeaks = 128;
        bool miniPerChannel = false;
        bool wantMeterData = false;
        bool allowSourceDecode = true;
        bool hasBackfillJob = false;
        aura::BackfillJob backfillJob{};
        // meterOnly requests skip mini extraction: they read the cached mini
        // envelope and only compute + persist meter data, then push a full
        // result (mini peaks + loudness) so the cache overwrite keeps the mini.
        bool meterOnly = false;
    };

    void workerThread();
    void pushResult(WaveformResult&& res);
    bool submitRequestInternal(const std::string& filePath,
                               int64_t fileId,
                               bool buildDetail,
                               int priority,
                               bool pushFront,
                               const aura::BackfillJob* backfillJob = nullptr,
                               bool allowSourceDecode = true,
                               bool meterOnly = false);

    Database* m_db = nullptr;
    
    // 请求队列
    std::deque<WaveformRequest> m_requestQueue;
    std::unordered_set<std::string> m_requestSet; // 去重
    mutable std::mutex m_requestMutex;
    std::condition_variable m_requestCv;
    
    // 结果队列
    std::queue<WaveformResult> m_resultQueue;
    mutable std::mutex m_resultMutex;
    
    // 正在处理中的路径（防止重复提交）
    std::unordered_set<std::string> m_processingSet;
    std::mutex m_processingMutex;
    
    // 工作线程
    std::vector<std::thread> m_threads;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_cancelRequested{false};
    std::atomic<uint64_t> m_generation{1};

    std::atomic<bool> m_throttled{false};
    // 已领取且正在处理的任务数：在 m_requestMutex 内 ++，处理结束 --
    std::atomic<int> m_activeWorkers{0};
};

// Search Async Worker

enum class AsyncSearchKind : int {
    Refresh = 0,
    LoadMore = 1,
    LoadAll = 2,
    FacetsOnly = 3,
    CountAndFacetsOnly = 4  // 稳定后只补 totalCount + facets，不返回 results
};

struct AsyncSearchRequest {
    uint64_t requestId = 0;       // 单调递增 ID，用于丢弃过时的结果
    SearchFilter filter;
    int viewMode = 0;             // ViewMode enum cast to int
    int64_t playlistId = -1;
    int limit = 500;
    int offset = 0;
    bool resetPagination = true;
    AsyncSearchKind kind = AsyncSearchKind::Refresh;
    bool needFacets = false;
    // 滚动/输入中推迟：true 时不执行 searchFilesCount，不执行 facets，减轻 DB 风暴
    bool deferCount = false;
    bool deferFacets = false;
    // When true, a rows-first refresh intentionally skips the automatic
    // CountAndFacetsOnly follow-up count. Used for large-library view
    // navigation where responsiveness matters more than immediate total count.
    bool skipDeferredCountFollowUp = false;
    // 文件夹过滤参数
    bool folderFilterActive = false;
    std::string selectedFolderPath;
    bool hasAdvancedFilters = false;
    bool hasFileTypeFilter = false;
    // Enables randomized result ordering for this request.
    bool isRandomShuffle = false;
    // 子播放列表 folder 聚合: true 时使用 getAggregatedPlaylistFiles
    bool playlistAggregatedView = false;
    // Database revision captured when the request is submitted. The result
    // carries it to the apply site so concurrent writes make the snapshot stale.

    uint64_t requestedAtDbRevision = 0;
};

struct AsyncSearchResult {
    uint64_t requestId = 0;
    AsyncSearchKind kind = AsyncSearchKind::Refresh;
    SearchFilter filter;
    int viewMode = 0;
    int64_t playlistId = -1;
    bool playlistAggregatedView = false;
    bool folderFilterActive = false;
    std::string selectedFolderPath;
    std::vector<AudioRecord> results;
    int64_t totalCount = 0;
    int loadedCount = 0;
    bool hasMore = false;
    bool success = false;
    std::vector<Database::CoverFacetEntry> coverFacets;
    std::vector<Database::PathFacetEntry> pathFacets;
    // True when coverFacets/pathFacets were intentionally computed.
    // Empty vectors are valid only when this flag is set.
    bool facetsComputed = false;
    bool countFollowUpSkipped = false;
    // Mirrors randomized ordering so the UI can start playback consistently.
    bool isRandomShuffle = false;
    // Revision represented by the returned rows; copied from the request.

    uint64_t requestedAtDbRevision = 0;
};

class SearchWorker {
public:
    SearchWorker();
    ~SearchWorker();

    // Uses a dedicated read-only connection to isolate search from waveform writes.
    void start(const std::string& dbPath);
    // 兼容旧接口：传入 Database* 时提取其路径创建只读连接
    void start(Database* db);
    
    // 停止
    void stop();
    
    // 提交搜索请求（会自动取消之前未完成的请求）
    uint64_t submitSearch(const AsyncSearchRequest& request);
    
    // 检查是否有完成的结果
    bool hasResult();
    
    // 获取最新完成的结果（如果 requestId 匹配）
    bool pollResult(AsyncSearchResult& outResult, uint64_t expectedRequestId);
    
    // 获取最新的 requestId
    uint64_t latestRequestId() const { return m_latestRequestId.load(); }
    
    // 搜索是否正在进行
    bool isSearching() const { return m_searching.load(); }

    // 取消当前/挂起搜索，避免旧请求继续占用查询通道
    void cancelActiveSearch();

private:
    struct FacetCacheEntry {
        std::string key;
        std::vector<Database::CoverFacetEntry> coverFacets;
        std::vector<Database::PathFacetEntry> pathFacets;
    };

    void workerThread();
    
    // SearchWorker 拥有独立的只读 Database 连接，与主连接/WaveformWorker 连接互不阻塞
    std::unique_ptr<Database> m_ownedDb;
    Database* m_db = nullptr;  // 指向 m_ownedDb.get()，保持内部代码不变
    
    // 请求（只保留最新的）
    AsyncSearchRequest m_pendingRequest;
    bool m_hasPendingRequest = false;
    std::mutex m_requestMutex;
    std::condition_variable m_requestCv;
    
    // 结果
    std::deque<AsyncSearchResult> m_completedQueue;
    std::mutex m_resultMutex;
    
    std::thread m_thread;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_searching{false};
    std::atomic<uint64_t> m_latestRequestId{0};
    std::atomic<uint64_t> m_currentProcessingId{0}; // Set when worker dequeues a request
    std::atomic<uint64_t> m_nextRequestId{1};
    std::deque<FacetCacheEntry> m_facetCache;
};

// Debounce Timer (用于搜索输入防抖)

class DebounceTimer {
public:
    // delayMs: 延迟触发时间（毫秒）
    explicit DebounceTimer(int delayMs = 200) : m_delayMs(delayMs) {}
    
    // 输入变化时调用（重置计时器）
    void trigger() {
        m_triggerTime = std::chrono::steady_clock::now();
        m_triggered = true;
        m_fired = false;
    }
    
    // 每帧调用，返回 true 表示延迟已过，应该执行操作
    bool check() {
        if (!m_triggered || m_fired) return false;
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_triggerTime).count();
        if (elapsed >= m_delayMs) {
            m_fired = true;
            m_triggered = false;
            return true;
        }
        return false;
    }
    
    // 立即触发（不等待延迟）
    void fireNow() {
        m_fired = true;
        m_triggered = false;
    }
    
    // 重置
    void reset() {
        m_triggered = false;
        m_fired = false;
    }
    
    // 是否正在等待触发
    bool isPending() const { return m_triggered && !m_fired; }

private:
    int m_delayMs;
    std::chrono::steady_clock::time_point m_triggerTime;
    bool m_triggered = false;
    bool m_fired = false;
};

