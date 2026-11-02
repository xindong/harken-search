#include "DevToolsModule.h"

#if AURA_BUILD_DIAGNOSTICS

#include "DiagnosticsLogger.h"
#include <chrono>
#include <cstdio>

// ---- Frame-phase profiling state ----
//
// Phase brackets nest (e.g. RenderUI > RenderUI.Artwork.Grid). We keep a
// fixed-depth stack so every phase (inclusive of its children) gets timed,
// and a fixed-size record of all completed phases this frame so spike
// reports can show the top-N by duration rather than just one slot.
static constexpr int kPhaseStackMax = 16;
static constexpr int kPhaseRecordMax = 64;
struct PhaseFrame {
    const char* name;
    uint64_t operationId = 0;
    std::chrono::steady_clock::time_point start;
};
struct PhaseRecord {
    const char* name;
    float ms;
};
static PhaseFrame s_phaseStack[kPhaseStackMax];
static int s_phaseStackDepth = 0;
static int s_phaseOverflow = 0;
static PhaseRecord s_phaseRecords[kPhaseRecordMax];
static int s_phaseRecordCount = 0;
static const char* s_slowestPhase = nullptr;
static float s_slowestPhaseMs = 0.0f;

void DevTools_BeginFramePhase(const char* phaseName) {
    if (!DiagnosticsLogger::instance().isEnabled()) return;
    if (s_phaseStackDepth >= kPhaseStackMax) { ++s_phaseOverflow; return; }  // overflow guard
    s_phaseStack[s_phaseStackDepth].name = phaseName;
    s_phaseStack[s_phaseStackDepth].operationId = DiagnosticsLogger::instance().beginUiOperation(phaseName);
    s_phaseStack[s_phaseStackDepth].start = std::chrono::steady_clock::now();
    ++s_phaseStackDepth;
}

void DevTools_EndFramePhase() {
    if (s_phaseOverflow > 0) { --s_phaseOverflow; return; }
    if (s_phaseStackDepth <= 0) return;
    --s_phaseStackDepth;
    const PhaseFrame& f = s_phaseStack[s_phaseStackDepth];
    DiagnosticsLogger::instance().endUiOperation(f.operationId);
    float ms = std::chrono::duration<float, std::milli>(
        std::chrono::steady_clock::now() - f.start).count();
    if (ms > s_slowestPhaseMs) {
        s_slowestPhaseMs = ms;
        s_slowestPhase = f.name;
    }
    if (s_phaseRecordCount < kPhaseRecordMax) {
        s_phaseRecords[s_phaseRecordCount].name = f.name;
        s_phaseRecords[s_phaseRecordCount].ms = ms;
        ++s_phaseRecordCount;
    }
}

void DevTools_ResetFramePhases() {
    s_slowestPhaseMs = 0.0f;
    s_slowestPhase = nullptr;
    s_phaseStackDepth = 0;
    s_phaseOverflow = 0;
    s_phaseRecordCount = 0;
}

// Internal: snapshot the top-N completed phases by ms, used by LogFrameSpike.
// Returns count written (0..outMax).
static int DevTools_SnapshotTopPhases(PhaseRecord* out, int outMax) {
    int n = (s_phaseRecordCount < outMax) ? s_phaseRecordCount : outMax;
    // Copy then partial-sort by descending ms.
    for (int i = 0; i < s_phaseRecordCount && i < kPhaseRecordMax; ++i) {
        if (i < outMax) {
            out[i] = s_phaseRecords[i];
        } else {
            // Replace smallest in `out` if larger
            int smallestIdx = 0;
            for (int k = 1; k < outMax; ++k) {
                if (out[k].ms < out[smallestIdx].ms) smallestIdx = k;
            }
            if (s_phaseRecords[i].ms > out[smallestIdx].ms) {
                out[smallestIdx] = s_phaseRecords[i];
            }
        }
    }
    // Sort `out[0..n)` descending by ms (n <= outMax small, simple insertion sort).
    for (int i = 1; i < n; ++i) {
        PhaseRecord key = out[i];
        int j = i - 1;
        while (j >= 0 && out[j].ms < key.ms) {
            out[j + 1] = out[j];
            --j;
        }
        out[j + 1] = key;
    }
    return n;
}

void DevTools_TickFrame(bool diagnosticModeEnabled) {
    auto& diag = DiagnosticsLogger::instance();
    diag.setEnabled(diagnosticModeEnabled);
    diag.tickFrame();
}

void DevTools_LogFrameSpike(float deltaTimeSec, bool intentionalThrottleSleep) {
    if (!DiagnosticsLogger::instance().isEnabled()) return;
    // 聚合：按 1 秒窗口上报该窗口内最大帧时间，避免每帧写一条导致在线日志「指针」来回跳动
    using namespace std::chrono;
    static float s_spikeMaxMs = 0.0f;
    static PhaseRecord s_spikeTop[3] = {};
    static int s_spikeTopCount = 0;
    static steady_clock::time_point s_windowStart;
    static bool s_windowInitialized = false;
    const auto now = steady_clock::now();
    if (!s_windowInitialized) {
        s_windowStart = now;
        s_windowInitialized = true;
    }
    if (!intentionalThrottleSleep && deltaTimeSec > 0.033f) {
        float ms = (float)(deltaTimeSec * 1000.0);
        if (ms > s_spikeMaxMs) {
            s_spikeMaxMs = ms;
            // Capture the top-3 slowest phases from the previous frame.
            s_spikeTopCount = DevTools_SnapshotTopPhases(s_spikeTop, 3);
        }
    }
    if (duration_cast<milliseconds>(now - s_windowStart).count() >= 1000) {
        if (s_spikeMaxMs > 0.0f) {
            DiagnosticsLogger::instance().logMetric("Pointer", "FrameSpikeMs", (double)s_spikeMaxMs);
            if (s_spikeTopCount > 0) {
                char buf[256];
                int written = snprintf(buf, sizeof(buf), "TopPhases:");
                for (int i = 0; i < s_spikeTopCount && written < (int)sizeof(buf) - 1; ++i) {
                    const char* nm = s_spikeTop[i].name ? s_spikeTop[i].name : "<null>";
                    written += snprintf(buf + written, sizeof(buf) - written,
                                        " %s=%.1fms", nm, (double)s_spikeTop[i].ms);
                }
                DiagnosticsLogger::instance().logAction("Pointer", buf);
            }
        }
        s_spikeMaxMs = 0.0f;
        s_spikeTopCount = 0;
        s_windowStart = now;
    }
}

void DevTools_LogCoverUploadMs(double elapsedMs) {
    if (!DiagnosticsLogger::instance().isEnabled()) return;
    if (elapsedMs >= 2.0)
        DiagnosticsLogger::instance().logPerformance("Cover", "PumpCoverTextureUploads", elapsedMs);
}

void DevTools_LogCoverPrimingMs(double elapsedMs) {
    if (!DiagnosticsLogger::instance().isEnabled()) return;
    if (elapsedMs >= 2.0)
        DiagnosticsLogger::instance().logPerformance("Cover", "PumpDeferredCoverPriming", elapsedMs);
}

void DevTools_SetEnabled(bool on) {
    DiagnosticsLogger::instance().setEnabled(on);
}

bool DevTools_IsEnabled() {
    return DiagnosticsLogger::instance().isEnabled();
}

void DevTools_LogFrameFps(float fps) {
    if (DiagnosticsLogger::instance().isEnabled())
        DiagnosticsLogger::instance().logMetric("Frame", "FPS", (double)fps);
}

void DevTools_LogMemoryMb(double mb) {
    if (DiagnosticsLogger::instance().isEnabled())
        DiagnosticsLogger::instance().logMetric("Memory", "WorkingSetMB", mb);
}

void DevTools_LogWebViewRpcMs(double elapsedMs) {
    if (DiagnosticsLogger::instance().isEnabled())
        DiagnosticsLogger::instance().logPerformance("WebView", "RPC_RoundTrip", elapsedMs);
}

void DevTools_LogWebViewFrameOverheadMs(double ms) {
    if (DiagnosticsLogger::instance().isEnabled())
        DiagnosticsLogger::instance().logPerformance("WebView", "FrameOverhead", ms);
}

#endif // AURA_BUILD_DIAGNOSTICS
