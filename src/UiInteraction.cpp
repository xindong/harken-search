#include "WindowsMain.h"
#include "platform/system.h"
#include <atomic>
#include <chrono>
static std::atomic<std::uint64_t> g_lastUiLeftClickTickMs{0};
static std::atomic<bool> g_coverWallPriorityMode{false};
int g_suppressImGuiDragTooltipFrames = 0;

void MarkUiLeftClickNow() {
    g_lastUiLeftClickTickMs.store(
        Platform::MonotonicMilliseconds(), std::memory_order_relaxed);
}

bool IsRapidUiClickBurstActive() {
    const std::uint64_t lastTick = g_lastUiLeftClickTickMs.load(std::memory_order_relaxed);
    if (lastTick == 0) return false;
    const std::uint64_t now = Platform::MonotonicMilliseconds();
    return now >= lastTick && (now - lastTick) <= 320;
}

void SetCoverWallPriorityMode(bool enabled) {
    g_coverWallPriorityMode.store(enabled, std::memory_order_relaxed);
}

bool IsCoverWallPriorityMode() {
    return g_coverWallPriorityMode.load(std::memory_order_relaxed);
}


bool startupEntryTransitionActive = false;
std::chrono::steady_clock::time_point startupEntryTransitionStart = std::chrono::steady_clock::now();
extern const float kStartupEntryTransitionMs = 760.0f;
