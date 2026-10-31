#pragma once

// DiagnosticsLogger — 智能诊断与性能回放系统

//
// 设计目标：
//   1. 为 AI 后期优化提供结构化、易解析的性能 + 行为日志
//   2. 生产级零开销：关闭时所有探针退化为空操作（内联 if 判断）
//   3. 最近一分钟的环形缓冲（默认安全上限 50000 条），不撑爆内存
//   4. 线程安全，可在任意线程（UI / Worker / Audio）调用
//   5. 一键导出 JSON，可直接喂给 AI 做瓶颈分析
//
// 构建开关：AURA_BUILD_DIAGNOSTICS=0 时（上线构建）宏全部为空，不引用本类。
// 见 BuildConfig.h、DevToolsModule。

#include "BuildConfig.h"
#include <string>
#include <vector>
#include <mutex>
#include <chrono>
#include <cstdint>
#include <atomic>
#include <thread>
#include <cstdio>
#include <array>

// Log Entry Types

enum class DiagLogType : uint8_t {
    Action,      // 用户操作（点击、筛选、搜索、播放等）
    Performance, // 性能计时（波形解码、DB 查询、封面加载等）
    Metric,      // 数值指标（内存、帧率、队列长度等）
    System,      // 系统事件（启动、关闭、错误等）
};

struct DiagLogEntry {
    DiagLogType type = DiagLogType::Action;
    char        category[32] = {};  // 分类标签，如 "Waveform", "Database", "Cover"
    char        detail[192] = {};   // 详细描述
    double      durationMs = 0.0; // 耗时 (ms)，仅 Performance 类型有效
    double      value = 0.0;      // 数值指标，仅 Metric 类型有效
    int64_t     timestampMs = 0;  // Unix 时间戳 (ms)
    uint32_t    threadId = 0;     // 线程标识
    uint32_t    repeatCount = 1;  // 智能去重后合并次数（1=未合并）
};

// DiagnosticsLogger — Singleton

// @stage7-contract
// API contract: the singleton is process-owned and all public methods are safe
// from UI, worker, and audio callers as documented by the logger design. Input
// strings are copied into the bounded ring buffer; disabled logging is a no-op.
// Export methods snapshot the current entries and report file failure as false.
class DiagnosticsLogger {
public:
    static DiagnosticsLogger& instance();

    // ---- 开关 ----
    void setEnabled(bool on);
    bool isEnabled() const   { return m_enabled.load(std::memory_order_relaxed); }
    void setOutputPath(const std::string& pathUtf8);

    // ---- 分项监听：空=全部，非空=仅写入该 category ----
    void setCategoryFilter(const std::string& category);
    std::string getCategoryFilter() const;

    // ---- 日志写入 ----
    void logAction(const char* category, const char* detail);
    void logPerformance(const char* category, const char* detail, double durationMs);
    void logMetric(const char* category, const char* detail, double value);
    void logSystem(const char* category, const char* detail);
    bool logSystemAt(const char* category, const char* detail, int64_t timestampMs);

    // ---- 导出 ----
    // 导出环形缓冲中的所有日志到 JSON 文件，返回 true 表示成功
    bool exportJson(const std::string& pathUtf8) const;
    // 导出到字符串（用于 UI 预览或剪贴板）
    std::string exportJsonString() const;

    // ---- 查询 ----
    size_t entryCount() const;
    void   clear();

    // ---- 容量配置 ----
    void setMaxEntries(size_t n);
#if AURA_BUILD_DIAGNOSTICS
    size_t maxEntries() const { return m_maxEntries; }
#else
    size_t maxEntries() const;
#endif

    // ---- 帧率追踪（主线程每帧调用一次）----
    void   tickFrame();
#if AURA_BUILD_DIAGNOSTICS
    float  currentFps() const { return m_currentFps; }
    double currentMemoryMb() const { return m_lastMemoryMb.load(std::memory_order_relaxed); }
    uint64_t droppedCount() const { return m_dropped.load(std::memory_order_relaxed); }
#else
    float  currentFps() const;
    double currentMemoryMb() const;
    uint64_t droppedCount() const;
#endif

    // ---- Debug overlay snapshot ----
    int copyRecent(DiagLogEntry* out, int maxOut) const;

    // ---- Recent log tail (for feedback / crash report payloads) ----
    std::string getRecentJsonLines(int maxLines) const;
    // Excludes high-frequency Metric entries (FPS/memory/queue counts);
    // returns up to maxLines Action + Performance + System entries.
    std::string getRecentFilteredLines(int maxLines) const;

    // ---- Crash-time emergency export (best-effort, no locks) ----
    bool exportJsonLinesCrashSafe(const wchar_t* pathW);

private:
    DiagnosticsLogger();
    ~DiagnosticsLogger();
    DiagnosticsLogger(const DiagnosticsLogger&) = delete;
    DiagnosticsLogger& operator=(const DiagnosticsLogger&) = delete;

    void pushEntry(DiagLogEntry&& entry);
    bool tryMergeWithLastLocked(const DiagLogEntry& entry);
    void pruneExpiredLocked(int64_t nowMs);
    static int64_t nowMs();
    static uint32_t threadId();

    static void copyTrunc(char* dst, size_t dstSize, const char* src);

    // ---- Lock-free MPMC ring buffer ----
    struct Cell {
        std::atomic<size_t> sequence;
        DiagLogEntry        data;
        Cell() : sequence(0), data{} {}
        Cell(Cell&& o) noexcept : sequence(o.sequence.load()), data(o.data) {}
        Cell& operator=(Cell&& o) noexcept { sequence.store(o.sequence.load()); data = o.data; return *this; }
        Cell(const Cell&) = delete;
        Cell& operator=(const Cell&) = delete;
    };
    bool enqueue(const DiagLogEntry& entry);
    bool dequeue(DiagLogEntry& out);

    void writerLoop();
    void startWriter();
    void stopWriter();

    std::atomic<bool>          m_enabled{false};
    static constexpr int64_t   kRetentionWindowMs = 60000;
    static constexpr size_t    kMemorySafetyEntries = 50000;
    size_t                     m_maxEntries = kMemorySafetyEntries;
    std::vector<DiagLogEntry>  m_ring;       // 导出缓冲（由后台线程写入）
    size_t                     m_head = 0;   // 写入位置
    size_t                     m_count = 0;  // 当前条目数
    int64_t                    m_oldestTimestampMs = 0; // Conservative minimum; enqueue order may differ.
    mutable std::mutex         m_mutex;      // 仅用于导出/管理

    static constexpr size_t    kQueueSize = 8192;
    std::vector<Cell>          m_queue;
    std::atomic<size_t>        m_enqueuePos{0};
    std::atomic<size_t>        m_dequeuePos{0};
    std::atomic<uint64_t>      m_dropped{0};

    // Debug overlay ring (fixed, lock-free overwrite)
    static constexpr size_t    kOverlaySize = 50;
    std::array<DiagLogEntry, kOverlaySize> m_overlay;
    std::atomic<size_t>        m_overlayHead{0};

    // FPS 追踪
    float  m_currentFps = 0.0f;
    int    m_frameCounter = 0;
    bool   m_fpsBaselineSet = false;   // 首次 tickFrame 时重置基线
    std::chrono::steady_clock::time_point m_lastFpsTick;

    // 最新内存指标（用于悬浮窗）
    std::atomic<double> m_lastMemoryMb{0.0};

    // 背景写线程
    std::thread          m_writerThread;
    std::atomic<bool>    m_writerRunning{false};
    std::atomic<bool>    m_writerExit{false};
    std::string          m_outputPath;

    std::string          m_categoryFilter;   // 空=全部；非空=只记录该 category
    mutable std::mutex   m_filterMutex;
};

// RAII ScopedTimer — 自动计时探针（AURA_BUILD_DIAGNOSTICS=0 时为空类）

#if AURA_BUILD_DIAGNOSTICS
class DiagScopedTimer {
public:
    DiagScopedTimer(const char* category, const char* detail = "")
        : m_category(category), m_detail(detail)
    {
        if (DiagnosticsLogger::instance().isEnabled()) {
            m_start = std::chrono::steady_clock::now();
            m_active = true;
        }
    }
    ~DiagScopedTimer() {
        if (m_active) {
            auto end = std::chrono::steady_clock::now();
            double ms = std::chrono::duration<double, std::milli>(end - m_start).count();
            DiagnosticsLogger::instance().logPerformance(m_category, m_detail, ms);
        }
    }
    DiagScopedTimer(const DiagScopedTimer&) = delete;
    DiagScopedTimer& operator=(const DiagScopedTimer&) = delete;

private:
    const char* m_category;
    const char* m_detail;
    std::chrono::steady_clock::time_point m_start;
    bool m_active = false;
};
#else
class DiagScopedTimer {
public:
    DiagScopedTimer(const char*, const char* = "") {}
};
#endif

// 便捷宏 — 上线构建（AURA_BUILD_DIAGNOSTICS=0）时零开销空操作

#if AURA_BUILD_DIAGNOSTICS
#define DIAG_SCOPED(category) \
    DiagScopedTimer _diagTimer_##__LINE__(category)
#define DIAG_SCOPED_DETAIL(category, detail) \
    DiagScopedTimer _diagTimer_##__LINE__(category, detail)
#define DIAG_ACTION(category, detail) \
    do { if (DiagnosticsLogger::instance().isEnabled()) \
        DiagnosticsLogger::instance().logAction(category, detail); } while(0)
#define DIAG_ACTION_FMT(category, fmt, ...) \
    do { if (DiagnosticsLogger::instance().isEnabled()) { \
        char _diag_buf[192]; _diag_buf[0] = 0; \
        snprintf(_diag_buf, sizeof(_diag_buf), fmt, __VA_ARGS__); \
        DiagnosticsLogger::instance().logAction(category, _diag_buf); } } while(0)
#define DIAG_METRIC(category, detail, value) \
    do { if (DiagnosticsLogger::instance().isEnabled()) \
        DiagnosticsLogger::instance().logMetric(category, detail, value); } while(0)
#define DIAG_SYSTEM(category, detail) \
    do { if (DiagnosticsLogger::instance().isEnabled()) \
        DiagnosticsLogger::instance().logSystem(category, detail); } while(0)
#define DIAG_SYSTEM_FMT(category, fmt, ...) \
    do { if (DiagnosticsLogger::instance().isEnabled()) { \
        char _diag_buf[192]; _diag_buf[0] = 0; \
        snprintf(_diag_buf, sizeof(_diag_buf), fmt, __VA_ARGS__); \
        DiagnosticsLogger::instance().logSystem(category, _diag_buf); } } while(0)
#else
#define DIAG_SCOPED(category)              do {} while(0)
#define DIAG_SCOPED_DETAIL(category, d)   do {} while(0)
#define DIAG_ACTION(category, detail)      do {} while(0)
#define DIAG_ACTION_FMT(category, fmt, ...) do {} while(0)
#define DIAG_METRIC(category, detail, v)   do {} while(0)
#define DIAG_SYSTEM(category, detail)      do {} while(0)
#define DIAG_SYSTEM_FMT(category, fmt, ...) do {} while(0)
#endif
