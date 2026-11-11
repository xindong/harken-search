#include "DiagnosticsLogger.h"
#include "platform/emergency_file_writer.h"
#include "platform/filesystem.h"
#include "platform/thread_id.h"

#include <sstream>
#include <iomanip>
#include <thread>
#include <ctime>
#include <cstring>
#include <algorithm>

static std::string EscapeJson(const char* s);

// Singleton

DiagnosticsLogger& DiagnosticsLogger::instance() {
    static DiagnosticsLogger s_instance;
    return s_instance;
}

DiagnosticsLogger::DiagnosticsLogger()
    : m_lastFpsTick(std::chrono::steady_clock::now())
{
    m_ring.resize(m_maxEntries);
    m_queue.resize(kQueueSize);
    for (size_t i = 0; i < kQueueSize; ++i) {
        m_queue[i].sequence.store(i, std::memory_order_relaxed);
    }
}

DiagnosticsLogger::~DiagnosticsLogger() {
    // Must join the writer thread before member destruction to avoid
    // std::thread::~thread() calling std::terminate() while still joinable.
    stopWriter();
}

// Helpers

int64_t DiagnosticsLogger::nowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

uint32_t DiagnosticsLogger::threadId() {
    return Platform::CurrentThreadId();
}

void DiagnosticsLogger::copyTrunc(char* dst, size_t dstSize, const char* src) {
    if (!dst || dstSize == 0) return;
    if (!src) { dst[0] = 0; return; }
    std::snprintf(dst, dstSize, "%s", src);
    dst[dstSize - 1] = 0;
}

// Push entry into ring buffer

void DiagnosticsLogger::setEnabled(bool on) {
    bool prev = m_enabled.exchange(on, std::memory_order_relaxed);
    if (prev == on) return;
    if (on) {
        startWriter();
    } else {
        stopWriter();
    }
}

void DiagnosticsLogger::setOutputPath(const std::string& pathUtf8) {
    std::lock_guard<std::mutex> lk(m_mutex);
    m_outputPath = pathUtf8;
}

bool DiagnosticsLogger::enqueue(const DiagLogEntry& entry) {
    size_t pos = m_enqueuePos.load(std::memory_order_relaxed);
    for (;;) {
        Cell& cell = m_queue[pos % kQueueSize];
        size_t seq = cell.sequence.load(std::memory_order_acquire);
        intptr_t dif = (intptr_t)seq - (intptr_t)pos;
        if (dif == 0) {
            if (m_enqueuePos.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
                cell.data = entry;
                cell.sequence.store(pos + 1, std::memory_order_release);
                return true;
            }
        } else if (dif < 0) {
            return false; // full
        } else {
            pos = m_enqueuePos.load(std::memory_order_relaxed);
        }
    }
}

bool DiagnosticsLogger::dequeue(DiagLogEntry& out) {
    size_t pos = m_dequeuePos.load(std::memory_order_relaxed);
    for (;;) {
        Cell& cell = m_queue[pos % kQueueSize];
        size_t seq = cell.sequence.load(std::memory_order_acquire);
        intptr_t dif = (intptr_t)seq - (intptr_t)(pos + 1);
        if (dif == 0) {
            if (m_dequeuePos.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
                out = cell.data;
                cell.sequence.store(pos + kQueueSize, std::memory_order_release);
                return true;
            }
        } else if (dif < 0) {
            return false; // empty
        } else {
            pos = m_dequeuePos.load(std::memory_order_relaxed);
        }
    }
}

void DiagnosticsLogger::setCategoryFilter(const std::string& category) {
    std::lock_guard<std::mutex> lk(m_filterMutex);
    m_categoryFilter = category;
}

std::string DiagnosticsLogger::getCategoryFilter() const {
    std::lock_guard<std::mutex> lk(m_filterMutex);
    return m_categoryFilter;
}

void DiagnosticsLogger::pushEntry(DiagLogEntry&& entry) {
    {
        std::lock_guard<std::mutex> lk(m_filterMutex);
        if (!m_categoryFilter.empty() && std::strcmp(entry.category, m_categoryFilter.c_str()) != 0)
            return;
    }
    if (!enqueue(entry)) {
        m_dropped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    // Debug overlay ring (best-effort overwrite)
    size_t idx = m_overlayHead.fetch_add(1, std::memory_order_relaxed) % kOverlaySize;
    m_overlay[idx] = entry;
}

bool DiagnosticsLogger::tryMergeWithLastLocked(const DiagLogEntry& entry) {
    if (m_count == 0 || m_maxEntries == 0) return false;

    const size_t lastIdx = (m_head + m_maxEntries - 1) % m_maxEntries;
    DiagLogEntry& last = m_ring[lastIdx];

    if (last.type != entry.type) return false;
    if (std::strcmp(last.category, entry.category) != 0) return false;
    if (std::strcmp(last.detail, entry.detail) != 0) return false;

    const int64_t dt = entry.timestampMs - last.timestampMs;
    if (dt < 0) return false;

    // 智能折叠时间窗：
    // - ACTION/SYSTEM：2 秒内同内容折叠
    // - METRIC：1 秒内同内容折叠（保留最新值）
    // - PERF：不折叠，避免丢失耗时分布
    int64_t mergeWindowMs = 0;
    switch (entry.type) {
    case DiagLogType::Action:
    case DiagLogType::System:
        mergeWindowMs = 2000;
        break;
    case DiagLogType::Metric:
        mergeWindowMs = 1000;
        break;
    case DiagLogType::Performance:
        mergeWindowMs = 0;
        break;
    }
    if (mergeWindowMs <= 0 || dt > mergeWindowMs) return false;

    last.timestampMs = entry.timestampMs;
    last.threadId = entry.threadId;
    if (entry.type == DiagLogType::Metric) {
        last.value = entry.value; // 指标保留最新值
    } else if (entry.type == DiagLogType::Performance) {
        last.durationMs = entry.durationMs;
    }
    if (last.repeatCount < 0xFFFFFFFFu) last.repeatCount++;
    return true;
}

void DiagnosticsLogger::startWriter() {
    if (m_writerRunning.load(std::memory_order_relaxed)) return;
    m_writerExit.store(false, std::memory_order_relaxed);
    m_writerThread = std::thread([this]() { writerLoop(); });
    m_writerRunning.store(true, std::memory_order_relaxed);
}

void DiagnosticsLogger::stopWriter() {
    if (!m_writerRunning.load(std::memory_order_relaxed)) return;
    m_writerExit.store(true, std::memory_order_relaxed);
    if (m_writerThread.joinable()) {
        m_writerThread.join();
    }
    m_writerRunning.store(false, std::memory_order_relaxed);
}

void DiagnosticsLogger::writerLoop() {
    // 每 session 截断日志，避免无限增长（旧 402MB 问题）
    Platform::FileSystem::StreamWriter out;
    int64_t bytesWritten = 0;
    static constexpr int64_t kMaxFileBytes = 10 * 1024 * 1024; // 10 MB 上限

    auto openFileIfNeeded = [&]() {
        std::lock_guard<std::mutex> lk(m_mutex);
        if (m_outputPath.empty()) return;
        if (!out.IsOpen()) {
            out.Open(m_outputPath, false, true);
            bytesWritten = 0;
        }
    };

    auto rotateIfNeeded = [&]() {
        if (bytesWritten < kMaxFileBytes) return;
        // 超过 10MB — 关闭并截断重开
        if (out.IsOpen()) { out.Flush(); out.Close(); }
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            if (!m_outputPath.empty()) {
                out.Open(m_outputPath, false, true);
                bytesWritten = 0;
            }
        }
    };

    while (!m_writerExit.load(std::memory_order_relaxed)) {
        openFileIfNeeded();
        int drained = 0;
        DiagLogEntry e;
        while (dequeue(e)) {
            drained++;
            if (out.IsOpen()) {
                std::ostringstream line;
                line << "{\"category\":\"" << EscapeJson(e.category)
                    << "\",\"detail\":\"" << EscapeJson(e.detail)
                    << "\",\"type\":" << (int)e.type
                    << ",\"durationMs\":" << e.durationMs
                    << ",\"value\":" << e.value
                    << ",\"timestampMs\":" << e.timestampMs
                    << ",\"threadId\":" << e.threadId
                    << ",\"repeat\":" << e.repeatCount
                    << "}\n";
                const std::string encoded = line.str();
                if (out.Write(encoded)) bytesWritten += static_cast<int64_t>(encoded.size());
            }

            // 同步到导出环形缓冲（仅后台线程写入）
            {
                std::lock_guard<std::mutex> lk(m_mutex);
                pruneExpiredLocked(e.timestampMs);
                if (!tryMergeWithLastLocked(e)) {
                    // A delayed producer must not displace a current entry with
                    // an already-expired one. The persistent log above still
                    // records it; normal folding of queued repeats is unchanged.
                    if (m_count == m_maxEntries && nowMs() - e.timestampMs > kRetentionWindowMs) {
                        continue;
                    }
                    if (m_count == 0) {
                        m_timestampsOrdered = true;
                    } else if (e.timestampMs < m_ring[(m_head + m_maxEntries - 1) % m_maxEntries].timestampMs) {
                        m_timestampsOrdered = false;
                    }
                    m_oldestTimestampMs = m_count == 0 ? e.timestampMs
                        : std::min(m_oldestTimestampMs, e.timestampMs);
                    if (m_count == m_maxEntries) {
                        m_dropped.fetch_add(1, std::memory_order_relaxed);
                    } else {
                        ++m_count;
                    }
                    m_ring[m_head] = e;
                    m_head = (m_head + 1) % m_maxEntries;
                }
            }
        }
        if (out.IsOpen()) out.Flush();
        rotateIfNeeded();
        if (drained == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
    }
    if (out.IsOpen()) out.Flush();
}

void DiagnosticsLogger::pruneExpiredLocked(int64_t nowMs) {
    if (m_count == 0 || m_maxEntries == 0) return;
    if (nowMs - m_oldestTimestampMs <= kRetentionWindowMs) return;
    const size_t ringStart = (m_head + m_maxEntries - m_count) % m_maxEntries;
    if (m_timestampsOrdered) {
        size_t expired = 0;
        while (expired < m_count &&
               nowMs - m_ring[(ringStart + expired) % m_maxEntries].timestampMs > kRetentionWindowMs) {
            ++expired;
        }
        m_count -= expired;
        m_oldestTimestampMs = m_count == 0 ? 0
            : m_ring[(ringStart + expired) % m_maxEntries].timestampMs;
        return;
    }

    // Producers may enqueue out of timestamp order. Compact survivors in place
    // in logical ring order; the write offset never overtakes the read offset.
    size_t retained = 0;
    int64_t oldestTimestamp = 0;
    int64_t previousTimestamp = 0;
    bool ordered = true;
    for (size_t i = 0; i < m_count; ++i) {
        const auto& entry = m_ring[(ringStart + i) % m_maxEntries];
        if (nowMs - entry.timestampMs > kRetentionWindowMs) continue;
        oldestTimestamp = retained == 0 ? entry.timestampMs
            : std::min(oldestTimestamp, entry.timestampMs);
        if (retained != 0 && entry.timestampMs < previousTimestamp) ordered = false;
        previousTimestamp = entry.timestampMs;
        if (retained != i) m_ring[(ringStart + retained) % m_maxEntries] = entry;
        ++retained;
    }
    m_count = retained;
    m_head = (ringStart + retained) % m_maxEntries;
    m_oldestTimestampMs = oldestTimestamp;
    m_timestampsOrdered = ordered;
}

// Log methods

void DiagnosticsLogger::logAction(const char* category, const char* detail) {
    if (!isEnabled()) return;
    DiagLogEntry e;
    e.type = DiagLogType::Action;
    copyTrunc(e.category, sizeof(e.category), category);
    copyTrunc(e.detail, sizeof(e.detail), detail);
    e.timestampMs = nowMs();
    e.threadId = threadId();
    pushEntry(std::move(e));
}

void DiagnosticsLogger::logPerformance(const char* category, const char* detail, double durationMs) {
    if (!isEnabled()) return;
    DiagLogEntry e;
    e.type = DiagLogType::Performance;
    copyTrunc(e.category, sizeof(e.category), category);
    copyTrunc(e.detail, sizeof(e.detail), detail);
    e.durationMs = durationMs;
    e.timestampMs = nowMs();
    e.threadId = threadId();
    pushEntry(std::move(e));
}

void DiagnosticsLogger::logMetric(const char* category, const char* detail, double value) {
    if (!isEnabled()) return;
    DiagLogEntry e;
    e.type = DiagLogType::Metric;
    copyTrunc(e.category, sizeof(e.category), category);
    copyTrunc(e.detail, sizeof(e.detail), detail);
    e.value = value;
    e.timestampMs = nowMs();
    e.threadId = threadId();
    if (category && detail && std::strcmp(category, "Memory") == 0 && std::strcmp(detail, "WorkingSetMB") == 0) {
        m_lastMemoryMb.store(value, std::memory_order_relaxed);
    }
    pushEntry(std::move(e));
}

bool DiagnosticsLogger::logSystemAt(const char* category, const char* detail, int64_t timestampMs) {
    if (!isEnabled()) return false;
    DiagLogEntry e;
    e.type = DiagLogType::System;
    copyTrunc(e.category, sizeof(e.category), category);
    copyTrunc(e.detail, sizeof(e.detail), detail);
    e.timestampMs = timestampMs;
    e.threadId = threadId();
    pushEntry(std::move(e));
    return true;
}

void DiagnosticsLogger::logSystem(const char* category, const char* detail) {
    if (!isEnabled()) return;
    DiagLogEntry e;
    e.type = DiagLogType::System;
    copyTrunc(e.category, sizeof(e.category), category);
    copyTrunc(e.detail, sizeof(e.detail), detail);
    e.timestampMs = nowMs();
    e.threadId = threadId();
    pushEntry(std::move(e));
}

// Capacity

void DiagnosticsLogger::setMaxEntries(size_t n) {
    std::lock_guard<std::mutex> lk(m_mutex);
    if (n < 50) n = 50;
    if (n > 10000) n = 10000;
    if (n == m_maxEntries) return;
    const size_t retained = std::min(m_count, n);
    const size_t start = (m_head + m_maxEntries - retained) % m_maxEntries;
    std::vector<DiagLogEntry> resized(n);
    int64_t oldestTimestamp = 0;
    bool ordered = true;
    for (size_t i = 0; i < retained; ++i) {
        resized[i] = m_ring[(start + i) % m_maxEntries];
        oldestTimestamp = i == 0 ? resized[i].timestampMs
            : std::min(oldestTimestamp, resized[i].timestampMs);
        if (i != 0 && resized[i].timestampMs < resized[i - 1].timestampMs) ordered = false;
    }
    m_ring.swap(resized);
    m_maxEntries = n;
    m_count = retained;
    m_head = retained % n;
    m_oldestTimestampMs = oldestTimestamp;
    m_timestampsOrdered = ordered;
}

size_t DiagnosticsLogger::entryCount() const {
    std::lock_guard<std::mutex> lk(m_mutex);
    const_cast<DiagnosticsLogger*>(this)->pruneExpiredLocked(nowMs());
    return m_count;
}

void DiagnosticsLogger::clear() {
    std::lock_guard<std::mutex> lk(m_mutex);
    m_count = 0;
    m_head = 0;
    m_oldestTimestampMs = 0;
    m_timestampsOrdered = true;
}

// FPS tracking

void DiagnosticsLogger::tickFrame() {
    // 首次调用时重置基线（避免 Splash 阶段到主循环的时间差导致首帧 FPS 偏差）
    if (!m_fpsBaselineSet) {
        m_lastFpsTick = std::chrono::steady_clock::now();
        m_fpsBaselineSet = true;
        m_frameCounter = 0;
    }
    m_frameCounter++;
    auto now = std::chrono::steady_clock::now();
    double elapsed = std::chrono::duration<double>(now - m_lastFpsTick).count();
    if (elapsed >= 1.0) {
        m_currentFps = (float)(m_frameCounter / elapsed);
        m_frameCounter = 0;
        m_lastFpsTick = now;
    }
}

// JSON Export

static std::string EscapeJson(const char* s) {
    std::string out;
    if (!s) return out;
    out.reserve(std::strlen(s) + 8);
    for (const char* p = s; *p; ++p) {
        char c = *p;
        switch (c) {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b";  break;
        case '\f': out += "\\f";  break;
        case '\n': out += "\\n";  break;
        case '\r': out += "\\r";  break;
        case '\t': out += "\\t";  break;
        default:
            if ((unsigned char)c < 0x20) {
                char buf[8];
                snprintf(buf, sizeof(buf), "\\u%04x", (unsigned char)c);
                out += buf;
            } else {
                out += c;
            }
        }
    }
    return out;
}

static const char* LogTypeName(DiagLogType t) {
    switch (t) {
    case DiagLogType::Action:      return "ACTION";
    case DiagLogType::Performance: return "PERF";
    case DiagLogType::Metric:      return "METRIC";
    case DiagLogType::System:      return "SYSTEM";
    }
    return "UNKNOWN";
}

static std::string FormatTimestamp(int64_t ms) {
    time_t sec = (time_t)(ms / 1000);
    int millis = (int)(ms % 1000);
    struct tm tmBuf;
#ifdef _WIN32
    localtime_s(&tmBuf, &sec);
#else
    localtime_r(&sec, &tmBuf);
#endif
    char buf[64];
    strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tmBuf);
    char full[80];
    snprintf(full, sizeof(full), "%s.%03dZ", buf, millis);
    return full;
}

std::string DiagnosticsLogger::getRecentJsonLines(int maxLines) const {
    std::lock_guard<std::mutex> lk(m_mutex);
    const_cast<DiagnosticsLogger*>(this)->pruneExpiredLocked(nowMs());
    if (m_count == 0 || maxLines <= 0) return {};

    size_t n = (size_t)maxLines < m_count ? (size_t)maxLines : m_count;
    size_t ringStart = (m_head + m_maxEntries - m_count) % m_maxEntries;
    size_t offset = m_count - n;

    std::ostringstream oss;
    oss << std::fixed << std::setprecision(2);
    for (size_t i = 0; i < n; i++) {
        size_t idx = (ringStart + offset + i) % m_maxEntries;
        const auto& e = m_ring[idx];
        oss << "{\"category\":\"" << EscapeJson(e.category)
            << "\",\"detail\":\"" << EscapeJson(e.detail)
            << "\",\"type\":" << (int)e.type
            << ",\"durationMs\":" << e.durationMs
            << ",\"value\":" << e.value
            << ",\"timestampMs\":" << e.timestampMs
            << ",\"threadId\":" << e.threadId
            << ",\"repeat\":" << e.repeatCount
            << "}\n";
    }
    return oss.str();
}

std::string DiagnosticsLogger::getRecentFilteredLines(int maxLines) const {
    std::lock_guard<std::mutex> lk(m_mutex);
    const_cast<DiagnosticsLogger*>(this)->pruneExpiredLocked(nowMs());
    if (m_count == 0 || maxLines <= 0) return {};

    // Collect candidate entries from the ring (oldest-first).
    // Scan the entire ring so Metric-heavy periods don't starve meaningful entries.
    std::vector<const DiagLogEntry*> raw;
    raw.reserve(m_count);
    size_t ringStart = (m_head + m_maxEntries - m_count) % m_maxEntries;
    for (size_t i = 0; i < m_count; ++i) {
        size_t idx = (ringStart + i) % m_maxEntries;
        const auto& e = m_ring[idx];
        // Filter 1: drop high-frequency Metric entries (FPS / memory counters)
        if (e.type == DiagLogType::Metric) continue;
        raw.push_back(&e);
    }

    // Filter 2: collapse consecutive runs of identical (category, detail) pairs.
    // A "run" of N identical entries becomes one entry annotated with "x<N>" unless N==1.
    struct Collapsed {
        const DiagLogEntry* entry;
        int runLen;
    };
    std::vector<Collapsed> collapsed;
    collapsed.reserve(raw.size());
    for (const auto* e : raw) {
        if (!collapsed.empty() &&
            std::strcmp(collapsed.back().entry->category, e->category) == 0 &&
            std::strcmp(collapsed.back().entry->detail,   e->detail)   == 0) {
            ++collapsed.back().runLen;
        } else {
            collapsed.push_back({e, 1});
        }
    }

    // Take the last maxLines entries after collapsing
    size_t start = collapsed.size() > (size_t)maxLines
                 ? collapsed.size() - (size_t)maxLines : 0;

    std::ostringstream oss;
    oss << std::fixed << std::setprecision(2);
    for (size_t i = start; i < collapsed.size(); ++i) {
        const auto& c = collapsed[i];
        const auto* e = c.entry;
        oss << "{\"category\":\"" << EscapeJson(e->category)
            << "\",\"detail\":\"" << EscapeJson(e->detail)
            << "\",\"type\":" << (int)e->type
            << ",\"durationMs\":" << e->durationMs
            << ",\"value\":" << e->value
            << ",\"timestampMs\":" << e->timestampMs
            << ",\"threadId\":" << e->threadId
            << ",\"repeat\":" << e->repeatCount;
        if (c.runLen > 1)
            oss << ",\"collapsed\":" << c.runLen;
        oss << "}\n";
    }
    return oss.str();
}

std::string DiagnosticsLogger::exportJsonString() const {
    std::lock_guard<std::mutex> lk(m_mutex);
    const_cast<DiagnosticsLogger*>(this)->pruneExpiredLocked(nowMs());

    // 先扫一遍生成摘要，便于一眼看清卡顿/封面耗时
    size_t start = (m_head + m_maxEntries - m_count) % m_maxEntries;
    int pointerSpikeCount = 0;
    double pointerSpikeMaxMs = 0;
    int coverUploadCount = 0;
    double coverUploadMaxMs = 0;
    int coverPrimingCount = 0;
    double coverPrimingMaxMs = 0;
    double coverWallReadyScanMaxMs = 0;
    int fpsCount = 0;
    double fpsSum = 0;
    double memoryMaxMb = 0;
    for (size_t i = 0; i < m_count; i++) {
        size_t idx = (start + i) % m_maxEntries;
        const auto& e = m_ring[idx];
        if (e.type == DiagLogType::Metric && strcmp(e.category, "Pointer") == 0 && strcmp(e.detail, "FrameSpikeMs") == 0) {
            pointerSpikeCount += (e.repeatCount > 0 ? (int)e.repeatCount : 1);
            if (e.value > pointerSpikeMaxMs) pointerSpikeMaxMs = e.value;
        } else if (e.type == DiagLogType::Performance && strcmp(e.category, "Cover") == 0 && strcmp(e.detail, "PumpCoverTextureUploads") == 0) {
            coverUploadCount += (e.repeatCount > 0 ? (int)e.repeatCount : 1);
            if (e.durationMs > coverUploadMaxMs) coverUploadMaxMs = e.durationMs;
        } else if (e.type == DiagLogType::Performance && strcmp(e.category, "Cover") == 0 && strcmp(e.detail, "PumpDeferredCoverPriming") == 0) {
            coverPrimingCount += (e.repeatCount > 0 ? (int)e.repeatCount : 1);
            if (e.durationMs > coverPrimingMaxMs) coverPrimingMaxMs = e.durationMs;
        } else if (e.type == DiagLogType::Performance && strcmp(e.category, "CoverWall") == 0 && strcmp(e.detail, "ReadyScanMs") == 0) {
            if (e.durationMs > coverWallReadyScanMaxMs) coverWallReadyScanMaxMs = e.durationMs;
        } else if (e.type == DiagLogType::Metric && strcmp(e.category, "Frame") == 0 && strcmp(e.detail, "FPS") == 0) {
            fpsCount += (e.repeatCount > 0 ? (int)e.repeatCount : 1);
            fpsSum += e.value * (e.repeatCount > 0 ? (int)e.repeatCount : 1);
        } else if (e.type == DiagLogType::Metric && strcmp(e.category, "Memory") == 0 && strcmp(e.detail, "WorkingSetMB") == 0) {
            if (e.value > memoryMaxMb) memoryMaxMb = e.value;
        }
    }

    std::ostringstream oss;
    oss << std::fixed << std::setprecision(2);
    oss << "{\n";
    oss << "  \"_comment\": \"Harken Search Diagnostics Export — performance analysis input\",\n";
    oss << "  \"exportTime\": \"" << FormatTimestamp(nowMs()) << "\",\n";
    oss << "  \"entryCount\": " << m_count << ",\n";
    oss << "  \"maxEntries\": " << m_maxEntries << ",\n";
    oss << "  \"summary\": {\n";
    oss << "    \"pointerFrameSpikeCount\": " << pointerSpikeCount << ",\n";
    oss << "    \"pointerFrameSpikeMaxMs\": " << pointerSpikeMaxMs << ",\n";
    oss << "    \"coverUploadCount\": " << coverUploadCount << ",\n";
    oss << "    \"coverUploadMaxMs\": " << coverUploadMaxMs << ",\n";
    oss << "    \"coverPrimingCount\": " << coverPrimingCount << ",\n";
    oss << "    \"coverPrimingMaxMs\": " << coverPrimingMaxMs << ",\n";
    oss << "    \"coverWallReadyScanMaxMs\": " << coverWallReadyScanMaxMs << ",\n";
    oss << "    \"fpsSamples\": " << fpsCount << ",\n";
    oss << "    \"fpsAvg\": " << (fpsCount > 0 ? (fpsSum / fpsCount) : 0.0) << ",\n";
    oss << "    \"memoryMaxMb\": " << memoryMaxMb << "\n";
    oss << "  },\n";
    oss << "  \"logs\": [\n";

    for (size_t i = 0; i < m_count; i++) {
        size_t idx = (start + i) % m_maxEntries;
        const auto& e = m_ring[idx];

        if (i > 0) oss << ",\n";
        oss << "    {\n";
        oss << "      \"type\": \"" << LogTypeName(e.type) << "\",\n";
        oss << "      \"timestamp\": \"" << FormatTimestamp(e.timestampMs) << "\",\n";
        oss << "      \"category\": \"" << EscapeJson(e.category) << "\",\n";
        oss << "      \"detail\": \"" << EscapeJson(e.detail) << "\"";

        if (e.type == DiagLogType::Performance) {
            oss << ",\n      \"durationMs\": " << e.durationMs;
        }
        if (e.type == DiagLogType::Metric) {
            oss << ",\n      \"value\": " << e.value;
        }
        if (e.repeatCount > 1) {
            oss << ",\n      \"repeatCount\": " << e.repeatCount;
        }
        oss << ",\n      \"threadId\": " << e.threadId;
        oss << "\n    }";
    }

    oss << "\n  ]\n";
    oss << "}\n";
    return oss.str();
}

bool DiagnosticsLogger::exportJson(const std::string& pathUtf8) const {
    std::string json = exportJsonString();

#ifdef _WIN32
    json.insert(0, "\xEF\xBB\xBF");
#else
    // Non-Windows exports historically omit the BOM.
#endif
    return Platform::FileSystem::WriteFileText(pathUtf8, json);
}

int DiagnosticsLogger::copyRecent(DiagLogEntry* out, int maxOut) const {
    if (!out || maxOut <= 0) return 0;
    size_t head = m_overlayHead.load(std::memory_order_relaxed);
    size_t available = std::min((size_t)maxOut, std::min(head, kOverlaySize));
    for (size_t i = 0; i < available; ++i) {
        size_t idx = (head - 1 - i) % kOverlaySize;
        out[i] = m_overlay[idx];
    }
    return (int)available;
}

bool DiagnosticsLogger::exportJsonLinesCrashSafe(const wchar_t* pathW) {
    if (!pathW) return false;
    Platform::EmergencyFile::Writer writer;
    if (!writer.OpenOverwriteWide(pathW)) return false;

    auto escapeInto = [](const char* src, char* dst, size_t dstSize) {
        if (!dst || dstSize == 0) return;
        size_t w = 0;
        if (!src) { dst[0] = 0; return; }
        for (const char* p = src; *p && w + 2 < dstSize; ++p) {
            char c = *p;
            if (c == '\"' || c == '\\') {
                if (w + 2 >= dstSize) break;
                dst[w++] = '\\';
                dst[w++] = c;
            } else if ((unsigned char)c < 0x20) {
                // skip control chars in crash-safe mode
            } else {
                dst[w++] = c;
            }
        }
        dst[w] = 0;
    };

    DiagLogEntry snapshot[kOverlaySize];
    int count = copyRecent(snapshot, (int)kOverlaySize);
    for (int i = 0; i < count; ++i) {
        char cat[64], det[256];
        escapeInto(snapshot[i].category, cat, sizeof(cat));
        escapeInto(snapshot[i].detail, det, sizeof(det));
        char line[512];
        int n = snprintf(line, sizeof(line),
                         "{\"category\":\"%s\",\"detail\":\"%s\",\"type\":%d,"
                         "\"durationMs\":%.3f,\"value\":%.3f,\"timestampMs\":%lld,"
                         "\"threadId\":%u,\"repeat\":%u}\n",
                         cat, det, (int)snapshot[i].type,
                         snapshot[i].durationMs, snapshot[i].value,
                         (long long)snapshot[i].timestampMs,
                         snapshot[i].threadId, snapshot[i].repeatCount);
        if (n > 0 && static_cast<size_t>(n) < sizeof(line)) {
            (void)writer.WriteAll(line, static_cast<size_t>(n));
        }
    }
    (void)writer.Close();
    return true;
}
