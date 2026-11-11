// src/HarkenSearchIcons.cpp
// Aris UI v1 - icon system implementation.
// - MergeLucideFont / LoadBrandIconsOnce / DrawBrandIcon land in Tasks 4-5.
// - StatusDot / DiffBadge / LoadingSpinner are pure ImDrawList helpers, no
//   external resources; implemented in Task 6.
#include "HarkenSearchIcons.h"
#include "HarkenSearchMetrics.h"
#include "Theme.h"
#include "platform/filesystem.h"
#include "platform/system.h"
#include "../thirdparty/stb_image_full.h"
#include "imgui.h"
#include <unordered_map>
#include <string>

#ifdef _WIN32
  #include "../thirdparty/imgui/backends/imgui_impl_opengl3_loader.h"
#endif

namespace aura::ui {

void MergeLucideFont(ImFontAtlas* atlas, float fontPixelSize) {
    if (!atlas) return;

    // Try exe-adjacent path first (release/install layout), then project-
    // relative path (dev run from repo root). Both resolve to the same
    // font file; whichever stat-hits-first wins.
    static const char* kCandidates[] = {
        "lucide.ttf",
        "thirdparty/lucide/lucide.ttf",
    };

    // Lucide codepoints used by the widget layer (see IconsLucide.h). Keep the
    // range tight so we don't rasterise 1500+ unused glyphs into the atlas.
    // Values match _gen_header.py output as of 2026-04-21.
    static const ImWchar kGlyphRange[] = {
        0xE042, 0xE55E,
        0,
    };

    ImFontConfig cfg;
    cfg.MergeMode            = true;
    cfg.PixelSnapH           = true;
    cfg.GlyphMinAdvanceX     = fontPixelSize;  // square icon cell
    cfg.OversampleH          = 1;
    cfg.OversampleV          = 1;
    cfg.FontDataOwnedByAtlas = true;

    for (const char* path : kCandidates) {
        if (Platform::FileSystem::CanOpenFileForRead(path)) {
            atlas->AddFontFromFileTTF(path, fontPixelSize, &cfg, kGlyphRange);
            return;
        }
    }
    // Graceful degradation - icons fall back to tofu boxes.
}

// Task 5 - Brand PNG loader

namespace {

struct BrandTex {
    GLuint glId = 0;
    int    w = 0;
    int    h = 0;
};

// Keyed by (BrandIcon << 1) | hiDpi
std::unordered_map<int, BrandTex> g_brandTex;
bool g_brandLoaded = false;

const char* BrandFilename(BrandIcon i, bool hiDpi) {
    switch (i) {
        case BrandIcon::Wwise:    return hiDpi ? "wwise@2x.png"    : "wwise.png";
        case BrandIcon::ProTools: return hiDpi ? "protools@2x.png" : "protools.png";
        case BrandIcon::Reaper:   return hiDpi ? "reaper@2x.png"   : "reaper.png";
        case BrandIcon::Ableton:  return hiDpi ? "ableton@2x.png"  : "ableton.png";
        case BrandIcon::Logic:    return hiDpi ? "logic@2x.png"    : "logic.png";
        case BrandIcon::Cubase:   return hiDpi ? "cubase@2x.png"   : "cubase.png";
        case BrandIcon::HarkenSearchLogo: return hiDpi ? "auro@2x.png"     : "auro.png";
    }
    return nullptr;
}

BrandTex LoadOne(const std::string& path) {
    BrandTex t;
    int n = 0;
    unsigned char* data = stbi_load(path.c_str(), &t.w, &t.h, &n, 4);
    if (!data) return t;
    glGenTextures(1, &t.glId);
    glBindTexture(GL_TEXTURE_2D, t.glId);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, t.w, t.h, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, data);
    stbi_image_free(data);
    return t;
}

// Try exe-adjacent assets/ first (install layout), then repo-relative.
std::string ResolveBrandPath(const char* filename) {
#ifdef _WIN32
    std::string base = Platform::GetExeDir() + "\\assets\\icons\\brand\\";
    std::string p1 = base + filename;
    if (Platform::FileSystem::CanOpenFileForRead(p1)) return p1;
#endif
    std::string p2 = std::string("assets/icons/brand/") + filename;
    if (Platform::FileSystem::CanOpenFileForRead(p2)) return p2;
    return {};
}

// Official Harken Search menu wordmark (assets/logos/brand/).

const char* BrandWordmarkFilename(BrandWordmark mark) {
    switch (mark) {
        case BrandWordmark::MenuWhite: return "harken-search-menu-wordmark-white.png";
        case BrandWordmark::MenuBlack: return "harken-search-menu-wordmark-black.png";
    }
    return nullptr;
}

std::string ResolveLogoPath(const char* filename) {
#ifdef _WIN32
    std::string p1 = Platform::GetExeDir() + "\\assets\\logos\\brand\\" + filename;
    if (Platform::FileSystem::CanOpenFileForRead(p1)) return p1;
#endif
    std::string p2 = std::string("assets/logos/brand/") + filename;
    if (Platform::FileSystem::CanOpenFileForRead(p2)) return p2;
    return {};
}

std::unordered_map<int, BrandTex> g_wordmarkTex;
bool g_wordmarkLoaded = false;

std::unordered_map<std::string, BrandTex> g_packagedAssetTex;

} // anon

void LoadBrandWordmarksOnce() {
    if (g_wordmarkLoaded) return;
    for (int i = 0; i <= (int)BrandWordmark::MenuBlack; ++i) {
        const char* fn = BrandWordmarkFilename((BrandWordmark)i);
        if (!fn) continue;
        const std::string p = ResolveLogoPath(fn);
        if (!p.empty()) g_wordmarkTex[i] = LoadOne(p);
    }
    g_wordmarkLoaded = true;
}

PackagedAssetTex LoadPackagedAsset(const char* relativePath) {
    if (!relativePath || !*relativePath) return {};
    const auto cached = g_packagedAssetTex.find(relativePath);
    if (cached != g_packagedAssetTex.end())
        return {cached->second.glId, cached->second.w, cached->second.h};
    std::string path;
#ifdef _WIN32
    std::string candidate = Platform::GetExeDir() + "\\";
    for (const char* c = relativePath; *c; ++c)
        candidate += (*c == '/') ? '\\' : *c;
    if (Platform::FileSystem::CanOpenFileForRead(candidate)) path = candidate;
#endif
    if (path.empty() && Platform::FileSystem::CanOpenFileForRead(relativePath))
        path = relativePath;
    BrandTex tex;
    if (!path.empty()) tex = LoadOne(path);
    g_packagedAssetTex[relativePath] = tex;
    return {tex.glId, tex.w, tex.h};
}

float DrawMenuBarBrandWordmark(float x0, float y0, float x1, float y1,
                               ImFont* fallbackFont, const char* fallbackText) {
    if (!g_wordmarkLoaded) LoadBrandWordmarksOnce();
    // 按字标实际所在的菜单栏底色选黑白图：主题编辑器允许单独改 menuBarBG，
    // 只看整体深浅主题会让自定义配色的字标看不见。
    const ImVec4 barBG = ThemeColors::menuBarFillBG(ThemeColors::current());
    const float barLuma = barBG.x * 0.299f + barBG.y * 0.587f + barBG.z * 0.114f;
    auto it = g_wordmarkTex.find(barLuma >= 0.5f ? (int)BrandWordmark::MenuBlack
                                                 : (int)BrandWordmark::MenuWhite);
    if (it == g_wordmarkTex.end() || it->second.glId == 0 || it->second.w <= 0
        || it->second.h <= 0) {
        // No packaged artwork: keep the plain-text brand name.
        if (!fallbackFont || !fallbackText || y1 - y0 < 6.0f) return x0;
        ImGui::PushFont(fallbackFont);
        const float fontSize = ImGui::GetFontSize();
        const ImVec2 textSize = ImGui::CalcTextSize(fallbackText);
        ImGui::PopFont();
        if (fontSize <= 0.0f || textSize.x <= 0.0f || x0 + textSize.x > x1) return x0;
        const ImVec2 pos(x0, y0 + (y1 - y0 - textSize.y) * 0.5f);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddText(fallbackFont, fontSize, pos,
                    ImGui::GetColorU32(ThemeColors::current().menuText), fallbackText);
        return pos.x + textSize.x;
    }
    const float barH = y1 - y0;
    float artH = barH * 0.58f;
    if (artH < 12.0f) artH = 12.0f;
    if (artH > 20.0f) artH = 20.0f;
    const float artW = artH * ((float)it->second.w / (float)it->second.h);
    if (artW <= 0.0f || x0 + artW > x1) return x0;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddImage((ImTextureID)(intptr_t)it->second.glId,
                 ImVec2(x0, y0 + (barH - artH) * 0.5f),
                 ImVec2(x0 + artW, y0 + (barH + artH) * 0.5f));
    return x0 + artW;
}

void LoadBrandIconsOnce() {
    if (g_brandLoaded) return;
    for (int i = 0; i <= (int)BrandIcon::HarkenSearchLogo; ++i) {
        const BrandIcon ic = (BrandIcon)i;
        for (int hd = 0; hd < 2; ++hd) {
            const char* fn = BrandFilename(ic, hd == 1);
            if (!fn) continue;
            std::string p = ResolveBrandPath(fn);
            if (!p.empty()) g_brandTex[i * 2 + hd] = LoadOne(p);
        }
    }
    g_brandLoaded = true;
}

void DrawBrandIcon(BrandIcon icon, float size) {
    if (!g_brandLoaded) LoadBrandIconsOnce();
    const float dpi = ImGui::GetIO().DisplayFramebufferScale.x;
    const bool  hi  = dpi >= 1.5f;
    const int   key = ((int)icon) * 2 + (hi ? 1 : 0);
    auto it = g_brandTex.find(key);
    if (it == g_brandTex.end() || it->second.glId == 0) {
        // Fall back to @1x if @2x missing, and vice versa.
        const int altKey = ((int)icon) * 2 + (hi ? 0 : 1);
        it = g_brandTex.find(altKey);
    }
    if (it == g_brandTex.end() || it->second.glId == 0) {
        // Placeholder - a dim square the size of the request.
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 p = ImGui::GetCursorScreenPos();
        dl->AddRect(p, ImVec2(p.x + size, p.y + size),
                    IM_COL32(140, 140, 140, 160));
        ImGui::Dummy(ImVec2(size, size));
        return;
    }
    ImGui::Image((ImTextureID)(intptr_t)it->second.glId, ImVec2(size, size));
}

// Task 6 - ImDrawList geometry helpers

void StatusDot(bool connected, float size) {
    const ThemeColorScheme& th = ThemeColors::current();
    ImVec2 p = ImGui::GetCursorScreenPos();
    const float r = size * 0.5f;
    const ImVec2 center(p.x + r, p.y + r);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (connected) {
        const ImU32 col = ImGui::ColorConvertFloat4ToU32(th.controlAccent);
        dl->AddCircleFilled(center, r, col, 20);
    } else {
        ImVec4 dim = th.interfaceText; dim.w *= 0.5f;
        const ImU32 col = ImGui::ColorConvertFloat4ToU32(dim);
        dl->AddCircle(center, r - 0.5f, col, 20, 1.5f);
    }
    ImGui::Dummy(ImVec2(size, size));
}

void DiffBadge(DiffStatus s) {
    const ThemeColorScheme& th = ThemeColors::current();
    ImU32 col; const char* label;
    switch (s) {
        case DiffStatus::NoChange:
            col   = ImGui::ColorConvertFloat4ToU32(ImVec4(0.5f, 0.5f, 0.5f, 1.0f));
            label = "NoChange";   break;
        case DiffStatus::New:
            col   = ImGui::ColorConvertFloat4ToU32(th.controlAccent);
            label = "New";        break;
        case DiffStatus::Replaced:
            col   = ImGui::ColorConvertFloat4ToU32(th.resultsSelection);
            label = "Replaced";   break;
        case DiffStatus::Unresolved:
            col   = ImGui::ColorConvertFloat4ToU32(th.volumeClipColor);
            label = "Unresolved"; break;
        default:
            col   = ImGui::ColorConvertFloat4ToU32(ImVec4(0.5f, 0.5f, 0.5f, 1.0f));
            label = "?";          break;
    }
    using namespace aura::ui::metrics;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const ImVec2 txt = ImGui::CalcTextSize(label);
    const ImVec2 end(p.x + txt.x + kPadXSm * 2, p.y + txt.y + kPadYSm * 2);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, end, col, kRadiusTight);
    dl->AddText(ImVec2(p.x + kPadXSm, p.y + kPadYSm), IM_COL32_WHITE, label);
    ImGui::Dummy(ImVec2(end.x - p.x, end.y - p.y));
}

void LoadingSpinner(float size) {
    const ThemeColorScheme& th = ThemeColors::current();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float  r = size * 0.45f;
    const ImVec2 center(p.x + size * 0.5f, p.y + size * 0.5f);
    const float  t  = (float)ImGui::GetTime();
    const float  a0 = t * 2.0f;
    const float  a1 = a0 + 4.0f;
    const ImU32  col = ImGui::ColorConvertFloat4ToU32(th.controlAccent);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PathArcTo(center, r, a0, a1, 32);
    dl->PathStroke(col, 0, 2.0f);
    ImGui::Dummy(ImVec2(size, size));
}

}
