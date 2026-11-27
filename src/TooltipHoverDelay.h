#pragma once

#include "imgui.h"

#include <cstdarg>

extern bool g_showGuidedTooltips;

// Plain-text tooltips stay hidden until the pointer has rested on the target
// for this long, so sweeping the pointer across a toolbar no longer flashes a
// trail of hint text. Preview-class content (cover thumbnails, saved tab
// preset previews) stays immediate and uses the *Immediate entry points.
// Long-text explanations are plain text: they wait for the dwell like every
// other hint, so "why did this result match" cannot pop up on a sweep.
namespace HarkenSearch {
inline constexpr float kTooltipHoverDelaySeconds = 1.0f;

inline bool TooltipItemHoverDwelled() {
    // NoSharedDelay keeps the timer per control: after resting on one button a
    // sweep across its neighbours must start each of them from zero again.
    return ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal |
                                ImGuiHoveredFlags_NoSharedDelay |
                                ImGuiHoveredFlags_AllowWhenDisabled);
}

// Hand-drawn regions (waveform zones, cover art) submit no item under the
// cursor, so the dwell is tracked per region key while it stays hovered.
inline bool TooltipRegionHoverDwelled(const void* regionKey) {
    static const void* s_key = nullptr;
    static int s_lastFrame = -1;
    static double s_hoveredSince = 0.0;
    const int frame = ImGui::GetFrameCount();
    const double now = ImGui::GetTime();
    // Callers only invoke this while their region is hovered, so a frame gap
    // means the pointer left and the dwell has to start over.
    if (s_key != regionKey || s_lastFrame < frame - 1) {
        s_key = regionKey;
        s_hoveredSince = now;
    }
    s_lastFrame = frame;
    return (now - s_hoveredSince) >= static_cast<double>(kTooltipHoverDelaySeconds);
}

inline void SetTooltipImmediate(const char* fmt, ...) IM_FMTARGS(1);
inline void SetTooltipImmediate(const char* fmt, ...) {
    if (!g_showGuidedTooltips) return;
    va_list args;
    va_start(args, fmt);
    ImGui::SetTooltipV(fmt, args);
    va_end(args);
}

// Call sites that always show their hint even with Guided Mode off.
inline void SetTooltipUngated(const char* fmt, ...) IM_FMTARGS(1);
inline void SetTooltipUngated(const char* fmt, ...) {
    if (!TooltipItemHoverDwelled()) return;
    va_list args;
    va_start(args, fmt);
    ImGui::SetTooltipV(fmt, args);
    va_end(args);
}

inline void SetTooltip(const char* fmt, ...) IM_FMTARGS(1);
inline void SetTooltip(const char* fmt, ...) {
    if (!g_showGuidedTooltips) return;
    if (!TooltipItemHoverDwelled()) return;
    va_list args;
    va_start(args, fmt);
    ImGui::SetTooltipV(fmt, args);
    va_end(args);
}

inline bool BeginTooltipImmediate() {
    if (!g_showGuidedTooltips) return false;
    ImGui::BeginTooltip();
    return true;
}

inline bool BeginTooltip() {
    if (!g_showGuidedTooltips) return false;
    if (!TooltipItemHoverDwelled()) return false;
    ImGui::BeginTooltip();
    return true;
}

// Search-hit explainability is operational feedback that stays available
// with Guided Mode off; it is plain text, so it still waits for the dwell.
inline bool BeginTooltipUngated() {
    if (!TooltipItemHoverDwelled()) return false;
    ImGui::BeginTooltip();
    return true;
}

// Drop-target hints appear while a drag hovers a region that submits no item
// under the cursor (dock markers, tab previews). They gate on the region's own
// dwell instead of the last submitted item.
inline void SetTooltipRegionUngated(const void* regionKey, const char* fmt, ...) IM_FMTARGS(2);
inline void SetTooltipRegionUngated(const void* regionKey, const char* fmt, ...) {
    if (!TooltipRegionHoverDwelled(regionKey)) return;
    va_list args;
    va_start(args, fmt);
    ImGui::SetTooltipV(fmt, args);
    va_end(args);
}

inline void EndTooltip() { ImGui::EndTooltip(); }
}
