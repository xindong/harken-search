#pragma once

#include "imgui.h"
#include "imgui_internal.h"
#include <cmath>
#include <iomanip>
#include <locale>
#include <sstream>
#include <string>
#include <vector>

// UI-thread-only, short-lived observations of the actual draw colors. No model
// of theme presets is duplicated here. The isolated QA command owns collection.
namespace TransportVisualAudit {
struct Check {
    std::string id;
    bool text;
    ImU32 foreground, background;
    ImVec2 min, max;
    bool disabled;
    ImVec2 clipMin, clipMax;
    bool glyphBounds;
};
inline int collectUntil = -1, frame = -1, theme = -1;
inline int spectrumCollapse = 0;
inline bool spectrum = false, selection = false;
inline ImVec2 barMin, barMax;
inline ImU32 surface = 0;
inline std::vector<Check> checks;
inline bool Collecting() { return ImGui::GetFrameCount() <= collectUntil; }
inline void Request() { collectUntil = ImGui::GetFrameCount() + 300; }
inline ImU32 Over(ImU32 foreground, ImU32 background) {
    const ImVec4 f = ImGui::ColorConvertU32ToFloat4(foreground);
    const ImVec4 b = ImGui::ColorConvertU32ToFloat4(background);
    return ImGui::ColorConvertFloat4ToU32(ImVec4(
        f.x * f.w + b.x * (1 - f.w), f.y * f.w + b.y * (1 - f.w),
        f.z * f.w + b.z * (1 - f.w), 1));
}
inline void Begin(int themeId, ImVec2 min, ImVec2 max, ImU32 background) {
    if (!Collecting()) return;
    frame = ImGui::GetFrameCount(); theme = themeId;
    barMin = min; barMax = max; surface = background; checks.clear();
}
inline void Record(const char* id, bool text, ImU32 foreground, ImU32 background,
                   ImVec2 min, ImVec2 max, bool disabled = false) {
    if (!Collecting() || frame != ImGui::GetFrameCount()) return;
    auto* draw = ImGui::GetWindowDrawList();
    checks.push_back({id, text, foreground, Over(background, surface), min, max, disabled,
        draw->GetClipRectMin(), draw->GetClipRectMax(), false});
}
// Match AddText's unwrapped glyph geometry before clipping, including font
// scale and pixel snapping. A line-height rectangle is not the visible ink.
inline void RecordGlyphs(const char* id, ImU32 foreground, ImU32 background,
                 ImVec2 position, const char* text, ImFont* font = nullptr, float size = 0) {
    if (!Collecting() || frame != ImGui::GetFrameCount()) return;
    if (!font) font = ImGui::GetFont();
    if (size <= 0) size = ImGui::GetFontSize();
    auto* baked = font->GetFontBaked(size);
    const float scale = size / baked->Size, startX = std::trunc(position.x);
    float x = startX, y = std::trunc(position.y);
    ImVec2 inkMin(FLT_MAX, FLT_MAX), inkMax(-FLT_MAX, -FLT_MAX);
    bool visible = false;
    while (*text) {
        unsigned int codepoint;
        const int bytes = ImTextCharFromUtf8(&codepoint, text, nullptr);
        if (bytes <= 0) break;
        text += bytes;
        if (codepoint == '\n') { x = startX; y += size; continue; }
        if (codepoint == '\r') continue;
        const auto* glyph = baked->FindGlyph(static_cast<ImWchar>(codepoint));
        if (!glyph) continue;
        if (glyph->Visible) {
            inkMin.x = ImMin(inkMin.x, x + glyph->X0 * scale);
            inkMin.y = ImMin(inkMin.y, y + glyph->Y0 * scale);
            inkMax.x = ImMax(inkMax.x, x + glyph->X1 * scale);
            inkMax.y = ImMax(inkMax.y, y + glyph->Y1 * scale);
            visible = true;
        }
        x += glyph->AdvanceX * scale;
    }
    if (!visible) return;
    Record(id, true, foreground, background, inkMin, inkMax);
    checks.back().glyphBounds = true;
}
inline void Item(const char* id, bool text, ImU32 foreground, ImU32 background, bool disabled = false) {
    Record(id, text, foreground, background, ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), disabled);
}
inline std::string Json() {
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << std::setprecision(9);
    auto rect = [&](ImVec2 a, ImVec2 b) { out << '[' << a.x << ',' << a.y << ',' << b.x << ',' << b.y << ']'; };
    auto color = [&](ImU32 c) {
        out << '[' << ((c >> IM_COL32_R_SHIFT) & 255) << ',' << ((c >> IM_COL32_G_SHIFT) & 255)
            << ',' << ((c >> IM_COL32_B_SHIFT) & 255) << ',' << ((c >> IM_COL32_A_SHIFT) & 255) << ']';
    };
    out << "{\"ok\":true,\"schema\":2,\"frame\":" << frame
        << ",\"current_frame\":" << ImGui::GetFrameCount() << ",\"theme\":" << theme << ",\"bar\":";
    rect(barMin, barMax);
    out << ",\"spectrum\":" << (spectrum ? "true" : "false")
        << ",\"selection\":" << (selection ? "true" : "false")
        << ",\"spectrum_collapse\":" << spectrumCollapse << ",\"surface\":";
    color(surface);
    out << ",\"checks\":[";
    for (size_t i = 0; i < checks.size(); ++i) {
        const auto& c = checks[i];
        if (i) out << ',';
        out << "{\"id\":" << std::quoted(c.id) << ",\"kind\":\""
            << (c.text ? "text" : "graphic") << "\",\"fg\":"; color(c.foreground);
        out << ",\"bg\":"; color(c.background);
        out << ",\"bounds\":"; rect(c.min, c.max);
        out << ",\"clip\":"; rect(c.clipMin, c.clipMax);
        out << ",\"bounds_kind\":\"" << (c.glyphBounds ? "glyphs" : "control") << '"';
        out << ",\"disabled\":" << (c.disabled ? "true" : "false") << '}';
    }
    out << "]}";
    return out.str();
}
} // namespace TransportVisualAudit
