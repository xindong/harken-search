#include "UiFonts.h"
#include "Utils.h"
#include "HarkenSearchIcons.h"
// Font Loading

// Chrome group: built at the fixed kChromeFontPx. These remain the ImGui default
// font, so any call site that does not opt into a ReadingTextScope renders chrome.
static ImFont* g_uiFontRegular = nullptr;
static ImFont* g_uiFontEmphasis = nullptr;
static ImFont* g_uiFontMono = nullptr;
static ImFont* g_uiMeterFont = nullptr;  // F-018: 10px ASCII-only, for narrow meter bars

// Reading group: built at the user's readingFontSize. Returned by the GetUIFont*()
// accessors only while g_inReadingScope is true (see ReadingTextScope).
static ImFont* g_readingRegular = nullptr;
static ImFont* g_readingEmphasis = nullptr;
static ImFont* g_readingMono = nullptr;
static ImFont* g_uiListFont = nullptr;   // reading-sized; the file-list body font

// True while rendering a reading surface. Flipped by ReadingTextScope.
// File-local: every reader (accessors) and writer (the scope ctors/dtors)
// lives in this TU, so it stays static to prevent callers from bypassing
// the RAII save/restore by poking the flag directly.
static bool g_inReadingScope = false;

static ImFont* AddFontFromCandidates(ImGuiIO& io,
                                     const char* const* candidates,
                                     int candidateCount,
                                     float fontPixelSize,
                                     const ImFontConfig& config,
                                     const ImWchar* ranges) {
    for (int i = 0; i < candidateCount; ++i) {
        const char* path = candidates[i];
        if (!path || !PathExistsUtf8(path)) continue;
        ImFont* f = io.Fonts->AddFontFromFileTTF(path, fontPixelSize, &config, ranges);
        if (f) return f;
    }
    return nullptr;
}

void LoadChineseFont(ImGuiIO& io, float chromePixelSize, float readingPixelSize, float listPixelSize) {
#ifdef _WIN32
    const char* uiRegularLatinBase[] = {
        "C:\\Windows\\Fonts\\SegoeUIVF.ttf", // Segoe UI Variable (Win11, closer to WebUI style)
        "C:\\Windows\\Fonts\\segoeui.ttf",   // Segoe UI fallback
    };
    const char* uiRegularCjkMerge[] = {
        "C:\\Windows\\Fonts\\msyh.ttc",   // Microsoft YaHei UI - rounded and modern CJK
        "C:\\Windows\\Fonts\\msyhbd.ttc", // Bold fallback
        "C:\\Windows\\Fonts\\Deng.ttf",   // DengXian fallback (modern sans)
        "C:\\Windows\\Fonts\\Dengb.ttf",  // DengXian Bold fallback
        "C:\\Windows\\Fonts\\simhei.ttf", // Legacy fallback only
        "C:\\Windows\\Fonts\\simsun.ttc", // Legacy fallback only
    };
    const char* uiEmphasisLatinBase[] = {
        "C:\\Windows\\Fonts\\SegoeUIVF.ttf", // Keep family tone consistent when semibold is unavailable
        "C:\\Windows\\Fonts\\seguisb.ttf",
        "C:\\Windows\\Fonts\\segoeuib.ttf",
    };
    const char* uiEmphasisCjkMerge[] = {
        "C:\\Windows\\Fonts\\msyhbd.ttc",
        "C:\\Windows\\Fonts\\msyh.ttc",
        "C:\\Windows\\Fonts\\Dengb.ttf",
        "C:\\Windows\\Fonts\\Deng.ttf",
        "C:\\Windows\\Fonts\\simhei.ttf", // Legacy fallback only
    };
    const char* monoPaths[] = {
        "C:\\Windows\\Fonts\\CascadiaMono.ttf",
        "C:\\Windows\\Fonts\\consola.ttf",
        "C:\\Windows\\Fonts\\consolab.ttf",
    };

#else
    const char* uiRegularLatinBase[] = {"/System/Library/Fonts/Supplemental/Arial.ttf"};
    const char* uiRegularCjkMerge[] = {"/System/Library/Fonts/PingFang.ttc", "/System/Library/Fonts/Supplemental/Arial Unicode.ttf"};
    const char* uiEmphasisLatinBase[] = {"/System/Library/Fonts/Supplemental/Arial Bold.ttf"};
    const char* uiEmphasisCjkMerge[] = {"/System/Library/Fonts/PingFang.ttc", "/System/Library/Fonts/Supplemental/Arial Unicode.ttf"};
    const char* monoPaths[] = {"/System/Library/Fonts/Menlo.ttc"};
#endif

    ImFontConfig uiConfig;
    uiConfig.MergeMode = false;
    uiConfig.OversampleH = 3;
    uiConfig.OversampleV = 2;
    uiConfig.PixelSnapH = true;
    // Slightly brighten rasterization to match WebUI's softer/younger text feeling on dark backgrounds.
    uiConfig.RasterizerMultiply = 1.03f;

    // PingFang's visible CJK ink is smaller than Arial's mixed-case text at
    // the same nominal ImGui size. Compensate only the merged Mac glyphs;
    // keep the base line height, Windows sources, and missing-font fallback.
    auto cjkMergePixelSize = [](float baseSize) {
#ifdef __APPLE__
        return baseSize + 2.0f;
#else
        return baseSize;
#endif
    };

    // Extended Latin glyph range: default + symbols used in UI (stars, arrows, checkmarks, etc.)
    // Segoe UI on Windows 10/11 contains all these glyphs natively.
    static const ImWchar kExtendedLatinRanges[] = {
        0x0020, 0x00FF,  // Basic Latin + Latin-1 Supplement (same as GetGlyphRangesDefault)
        0x2000, 0x206F,  // General Punctuation (—, –, ", etc.)
        0x2100, 0x214F,  // Letterlike Symbols
        0x2190, 0x21FF,  // Arrows (→, ←, ↔, ↕, etc.)
        0x2200, 0x22FF,  // Mathematical Operators (⊞, ±, etc.)
        0x2300, 0x23FF,  // Miscellaneous Technical
        0x2500, 0x25FF,  // Box Drawing + Block Elements + Geometric Shapes (●, ○, ▶, etc.)
        0x2600, 0x26FF,  // Miscellaneous Symbols
        0x2700, 0x27FF,  // Dingbats (✓, ✗, ⟳, etc.)
        0x2800, 0x28FF,  // Braille Patterns (progress spinners)
        0,
    };

    // Base Latin/UI glyphs first (Segoe-like modern look), then merge Chinese glyphs
    // to reduce CJK/Latin mismatch while keeping Chinese coverage complete.
    // Build a {regular, emphasis, mono} triplet at a given pixel size. Reuses the
    // candidate arrays declared above. Lucide icons are merged into the regular
    // face so icon buttons resolve glyphs at this size.
    auto buildTriplet = [&](float px,
                            ImFont** outRegular,
                            ImFont** outEmphasis,
                            ImFont** outMono) {
        // Regular: Latin base + CJK merge.
        *outRegular = AddFontFromCandidates(
            io, uiRegularLatinBase,
            (int)(sizeof(uiRegularLatinBase) / sizeof(uiRegularLatinBase[0])),
            px, uiConfig, kExtendedLatinRanges);
        if (!*outRegular) {
            *outRegular = AddFontFromCandidates(
                io, uiRegularCjkMerge,
                (int)(sizeof(uiRegularCjkMerge) / sizeof(uiRegularCjkMerge[0])),
                px, uiConfig, io.Fonts->GetGlyphRangesChineseFull());
            if (!*outRegular) *outRegular = io.Fonts->AddFontDefault();
        } else {
            ImFontConfig mergeCjk = uiConfig;
            mergeCjk.MergeMode = true;
            mergeCjk.RasterizerMultiply = 1.08f;
            (void)AddFontFromCandidates(
                io, uiRegularCjkMerge,
                (int)(sizeof(uiRegularCjkMerge) / sizeof(uiRegularCjkMerge[0])),
                cjkMergePixelSize(px), mergeCjk, io.Fonts->GetGlyphRangesChineseFull());
        }
        // Merge Lucide icons into THIS regular face at THIS size.
        aura::ui::MergeLucideFont(io.Fonts, px);

        // Emphasis: semibold Latin base + CJK merge.
        ImFontConfig emphasisConfig = uiConfig;
        *outEmphasis = AddFontFromCandidates(
            io, uiEmphasisLatinBase,
            (int)(sizeof(uiEmphasisLatinBase) / sizeof(uiEmphasisLatinBase[0])),
            px, emphasisConfig, kExtendedLatinRanges);
        if (!*outEmphasis) {
            *outEmphasis = AddFontFromCandidates(
                io, uiEmphasisCjkMerge,
                (int)(sizeof(uiEmphasisCjkMerge) / sizeof(uiEmphasisCjkMerge[0])),
                px, emphasisConfig, io.Fonts->GetGlyphRangesChineseFull());
            if (!*outEmphasis) *outEmphasis = *outRegular;
        } else {
            ImFontConfig mergeCjkEmphasis = emphasisConfig;
            mergeCjkEmphasis.MergeMode = true;
            mergeCjkEmphasis.RasterizerMultiply = 1.08f;
            (void)AddFontFromCandidates(
                io, uiEmphasisCjkMerge,
                (int)(sizeof(uiEmphasisCjkMerge) / sizeof(uiEmphasisCjkMerge[0])),
                cjkMergePixelSize(px), mergeCjkEmphasis, io.Fonts->GetGlyphRangesChineseFull());
        }

        // Mono: +1px optical compensation.
        ImFontConfig monoConfig;
        monoConfig.MergeMode = false;
        monoConfig.OversampleH = 2;
        monoConfig.OversampleV = 1;
        monoConfig.PixelSnapH = true;
        *outMono = AddFontFromCandidates(
            io, monoPaths,
            (int)(sizeof(monoPaths) / sizeof(monoPaths[0])),
            px + 1.0f, monoConfig, kExtendedLatinRanges);
        if (!*outMono) *outMono = *outRegular;
    };

    buildTriplet(chromePixelSize, &g_uiFontRegular, &g_uiFontEmphasis, &g_uiFontMono);
    buildTriplet(readingPixelSize, &g_readingRegular, &g_readingEmphasis, &g_readingMono);

    // Dedicated list font (independent from system font size).
    float listPx = listPixelSize;
    ImFontConfig listConfig = uiConfig;
    listConfig.MergeMode = false;
    g_uiListFont = AddFontFromCandidates(
        io,
        uiRegularLatinBase,
        (int)(sizeof(uiRegularLatinBase) / sizeof(uiRegularLatinBase[0])),
        listPx,
        listConfig,
        kExtendedLatinRanges);
    if (!g_uiListFont) {
        g_uiListFont = AddFontFromCandidates(
            io,
            uiRegularCjkMerge,
            (int)(sizeof(uiRegularCjkMerge) / sizeof(uiRegularCjkMerge[0])),
            listPx,
            listConfig,
            io.Fonts->GetGlyphRangesChineseFull());
    } else {
        ImFontConfig mergeCjkList = listConfig;
        mergeCjkList.MergeMode = true;
        mergeCjkList.RasterizerMultiply = 1.08f;
        (void)AddFontFromCandidates(
            io,
            uiRegularCjkMerge,
            (int)(sizeof(uiRegularCjkMerge) / sizeof(uiRegularCjkMerge[0])),
            cjkMergePixelSize(listPx),
            mergeCjkList,
            io.Fonts->GetGlyphRangesChineseFull());
    }
    if (!g_uiListFont) g_uiListFont = g_uiFontRegular;

    // F-018: Small ASCII-only font for Loudness Meter numeric readouts / channel labels
    // when barW is narrow. No CJK merge needed — meter labels are L/R/C/LFE/Lss/W/Y/Z etc.
    {
        ImFontConfig meterConfig;
        meterConfig.MergeMode = false;
        meterConfig.OversampleH = 2;
        meterConfig.OversampleV = 2;
        meterConfig.PixelSnapH = true;
        g_uiMeterFont = AddFontFromCandidates(
            io,
            uiRegularLatinBase,
            (int)(sizeof(uiRegularLatinBase) / sizeof(uiRegularLatinBase[0])),
            10.0f,
            meterConfig,
            kExtendedLatinRanges);
        if (!g_uiMeterFont) g_uiMeterFont = g_uiFontRegular;
    }

    // F-045: Lucide icons are merged per-triplet inside buildTriplet (into each
    // regular face at its own size), so both chrome and reading regulars carry
    // icons; the meter face deliberately does not.

    io.FontDefault = g_uiFontRegular;
}

ImFont* GetUIFontRegular() {
    if (g_inReadingScope && g_readingRegular) return g_readingRegular;
    return g_uiFontRegular;
}

ImFont* GetUIFontEmphasis() {
    if (g_inReadingScope) return g_readingEmphasis ? g_readingEmphasis : g_readingRegular;
    return g_uiFontEmphasis ? g_uiFontEmphasis : g_uiFontRegular;
}

ImFont* GetUIFontMono() {
    if (g_inReadingScope) return g_readingMono ? g_readingMono : g_readingRegular;
    return g_uiFontMono ? g_uiFontMono : g_uiFontRegular;
}

ImFont* GetUIListFont() {
    // Reading-sized fallback (not chrome) so the list keeps reading metrics.
    return g_uiListFont ? g_uiListFont : g_readingRegular;
}

ImFont* GetUIMeterFont() {
    return g_uiMeterFont ? g_uiMeterFont : g_uiFontRegular;
}

ReadingTextScope::ReadingTextScope() : prev(g_inReadingScope) {
    g_inReadingScope = true;
    ImGui::PushFont(g_readingRegular ? g_readingRegular : g_uiFontRegular);
}
ReadingTextScope::~ReadingTextScope() {
    ImGui::PopFont();
    g_inReadingScope = prev;
}

ChromeTextScope::ChromeTextScope() : prev(g_inReadingScope) {
    g_inReadingScope = false;
    ImGui::PushFont(g_uiFontRegular);
}
ChromeTextScope::~ChromeTextScope() {
    ImGui::PopFont();
    g_inReadingScope = prev;
}
