#pragma once

// DevToolsModule — 测试/诊断功能统一入口，上线可整体关闭

//
// 所有「仅开发/内测用」的探针与 UI 集中经本模块：
//   - 指针卡顿（帧尖峰）日志
//   - 封面上传 / Key 预热耗时日志
//   - 诊断模式开关、FPS/内存打点
// 当 AURA_BUILD_DIAGNOSTICS=0 时，接口为空实现，零开销。

#include "BuildConfig.h"

// @stage7-contract
// API contract: diagnostic helpers are no-ops when diagnostics are disabled;
// enabled calls record bounded metrics and do not alter product behavior. They
// are intended for the main/UI frame and do not retain caller-owned values.

// ---- 每帧 ----
// 主循环中调用：同步诊断开关、打帧率点；若开启诊断且上一帧耗时 > 33ms 则记一条「指针卡顿」
void DevTools_TickFrame(bool diagnosticModeEnabled);

// ---- 探针（仅当 AURA_BUILD_DIAGNOSTICS 且 diagnosticMode 开启时写日志）----
void DevTools_LogFrameSpike(float deltaTimeSec, bool intentionalThrottleSleep = false);
void DevTools_LogCoverUploadMs(double elapsedMs);
void DevTools_LogCoverPrimingMs(double elapsedMs);

// ---- 诊断开关（与设置里的「启用诊断模式」一致）----
void DevTools_SetEnabled(bool on);
bool DevTools_IsEnabled();

// ---- 周期指标（FPS、内存等，供悬浮窗用）----
void DevTools_LogFrameFps(float fps);
void DevTools_LogMemoryMb(double mb);

// ---- WebView2 性能基准 (F-024) ----
void DevTools_LogWebViewRpcMs(double elapsedMs);       // JSON-RPC round-trip
void DevTools_LogWebViewFrameOverheadMs(double ms);    // WebView composition overhead per frame

// ---- Frame-phase profiling: track which phase is slowest per frame ----
// Use BeginFramePhase/EndFramePhase around major render loop sections.
// When FrameSpikeMs is logged, the slowest phase name and duration are included.
void DevTools_BeginFramePhase(const char* phaseName);
void DevTools_EndFramePhase();
void DevTools_ResetFramePhases();  // call once at start of each frame

#if AURA_BUILD_DIAGNOSTICS
// 导出、崩溃快照等仍直接使用 DiagnosticsLogger，由调用方 #if AURA_BUILD_DIAGNOSTICS 包裹
#else
inline void DevTools_TickFrame(bool) {}
inline void DevTools_LogFrameSpike(float, bool) {}
inline void DevTools_LogCoverUploadMs(double) {}
inline void DevTools_LogCoverPrimingMs(double) {}
inline void DevTools_SetEnabled(bool) {}
inline bool DevTools_IsEnabled() { return false; }
inline void DevTools_LogFrameFps(float) {}
inline void DevTools_LogMemoryMb(double) {}
inline void DevTools_LogWebViewRpcMs(double) {}
inline void DevTools_LogWebViewFrameOverheadMs(double) {}
inline void DevTools_BeginFramePhase(const char*) {}
inline void DevTools_EndFramePhase() {}
inline void DevTools_ResetFramePhases() {}
#endif

// AURA_PHASE_PROBES — opt-in fine-grained RenderUI phase instrumentation.
//
// Each probe pair calls DevTools_Begin/EndFramePhase. In Debug builds these
// are non-trivial (~150-200μs per pair across ~17 sites = ~3ms/frame) so
// they're disabled by default. Flip the flag below to 1 when you need to
// bisect a frame-time regression — every probe site in main.cpp is preserved
// as scaffolding so you can immediately see which phase dominates a spike.
//
// To use: change `0` to `1` here, rebuild, run perf_phase_drive.py to see
// `Pointer/TopPhases:` entries naming the slowest sub-phase per spike second.

#ifndef AURA_PHASE_PROBES_ENABLED
#define AURA_PHASE_PROBES_ENABLED 0
#endif

#if AURA_PHASE_PROBES_ENABLED && AURA_BUILD_DIAGNOSTICS
#define AURA_PHASE_BEGIN(name) DevTools_BeginFramePhase(name)
#define AURA_PHASE_END()       DevTools_EndFramePhase()
#else
#define AURA_PHASE_BEGIN(name) ((void)0)
#define AURA_PHASE_END()       ((void)0)
#endif

// Frame-phase scope for UI-only callers; background work uses DiagScopedTimer.
class DevToolsFramePhaseScope {
public:
    explicit DevToolsFramePhaseScope(const char* name) : active_(DevTools_IsEnabled()) {
        if (active_) DevTools_BeginFramePhase(name);
    }
    ~DevToolsFramePhaseScope() { if (active_) DevTools_EndFramePhase(); }
    DevToolsFramePhaseScope(const DevToolsFramePhaseScope&) = delete;
    DevToolsFramePhaseScope& operator=(const DevToolsFramePhaseScope&) = delete;
private:
    bool active_;
};
