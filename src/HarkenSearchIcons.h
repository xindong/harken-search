// src/HarkenSearchIcons.h
// Aris UI v1 - icon system surface (Lucide font + brand PNG + ImDrawList helpers).
#pragma once

struct ImFontAtlas;
struct ImDrawList;
struct ImFont;

namespace aura::ui {

// @stage7-contract
// API contract: icon helpers consume caller-owned ImGui atlas/draw-list objects
// on the UI thread and do not retain transient draw state. LoadBrandIconsOnce
// is idempotent; draw helpers preserve the existing icon/token meanings.

enum class BrandIcon { Wwise, ProTools, Reaper, Ableton, Logic, Cubase, HarkenSearchLogo };
enum class DiffStatus { NoChange, New, Replaced, Unresolved };
enum class BrandWordmark { MenuWhite, MenuBlack };

// Populated in Tasks 4-6.
void MergeLucideFont(ImFontAtlas* atlas, float fontPixelSize);
void LoadBrandIconsOnce();
void DrawBrandIcon(BrandIcon icon, float size);
void LoadBrandWordmarksOnce();
// Loads (and caches) a PNG shipped relative to the executable, e.g.
// "assets/icons/contributors/cindy.png". Returns glId==0 when the file or
// decoding is unavailable; callers must keep a text fallback. The texture is
// process-owned and released at shutdown.
struct PackagedAssetTex {
    unsigned int glId = 0;
    int w = 0;
    int h = 0;
};
PackagedAssetTex LoadPackagedAsset(const char* relativePath);
// Draws the official Harken Search wordmark inside a horizontal band such as
// the menu bar, vertically centred between y0 and y1, and picks the white
// artwork for dark chrome or the black artwork for light chrome. When the
// packaged image is missing it draws fallbackText with fallbackFont instead, so
// the brand name stays visible. Returns the x just right of the artwork for
// callers that must shift the following items; returns x0 when nothing fits.
float DrawMenuBarBrandWordmark(float x0, float y0, float x1, float y1,
                               ImFont* fallbackFont, const char* fallbackText);
void StatusDot(bool connected, float size = 8.0f);
void DiffBadge(DiffStatus s);
void LoadingSpinner(float size = 16.0f);

}
