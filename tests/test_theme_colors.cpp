#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "AppState.h"
#include "HarkenSearchDesignTokens.generated.h"
#include "HarkenSearchThemeManager.h"
#include "DiagnosticsLogger.h"
#include "Theme.h"
#include "ThemeEditorSchema.h"
#include "platform/system.h"
#include "imgui.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

ImU32 g_waveformColor = 0;
ImU32 g_waveformBgColor = 0;
ImU32 g_waveformRmsColor = 0;
ImU32 g_miniWaveColor = 0;
ImU32 g_miniWaveBgColor = 0;
ImVec4 COLOR_BG_DARK = ImVec4(0, 0, 0, 1);
ImVec4 COLOR_BG_PANEL = ImVec4(0, 0, 0, 1);
ImVec4 COLOR_TEXT = ImVec4(1, 1, 1, 1);
ImVec4 COLOR_TEXT_DIM = ImVec4(0.5f, 0.5f, 0.5f, 1);
ImVec4 COLOR_ACCENT = ImVec4(0.6f, 0.8f, 0.2f, 1);
ImVec4 COLOR_ACCENT_DIM = ImVec4(0.4f, 0.5f, 0.15f, 1);
ImVec4 COLOR_SELECTED = ImVec4(0.8f, 0.6f, 0.1f, 1);
ImVec4 COLOR_WAVEFORM_BG = ImVec4(0, 0, 0, 1);
ImVec4 COLOR_WAVEFORM = ImVec4(1, 1, 1, 1);

DiagnosticsLogger& DiagnosticsLogger::instance() {
    static DiagnosticsLogger s_instance;
    return s_instance;
}

DiagnosticsLogger::DiagnosticsLogger() = default;
DiagnosticsLogger::~DiagnosticsLogger() = default;
void DiagnosticsLogger::setEnabled(bool) {}
void DiagnosticsLogger::logSystem(const char*, const char*) {}

namespace Platform {
bool IsSystemDarkMode() {
    return false;
}
std::string GetExeDir() {
    return std::filesystem::temp_directory_path().string();
}
}

std::filesystem::path Utf8ToPath(const std::string& pathUtf8) {
    return std::filesystem::path(pathUtf8);
}

std::string GetExeDirUtf8() {
    return std::filesystem::temp_directory_path().string();
}

static int g_failed = 0;
static int g_passed = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        std::cerr << "FAIL: " << (msg) << "\n"; \
        ++g_failed; \
    } else { \
        std::cout << "OK: " << (msg) << "\n"; \
        ++g_passed; \
    } \
} while (0)

static bool approxEqual(float a, float b, float eps = 0.0025f) {
    return std::fabs(a - b) <= eps;
}

static bool approxEqual(const ImVec4& a, const ImVec4& b, float eps = 0.0025f) {
    return approxEqual(a.x, b.x, eps)
        && approxEqual(a.y, b.y, eps)
        && approxEqual(a.z, b.z, eps)
        && approxEqual(a.w, b.w, eps);
}

static ImVec4 tokenColor(const aris::design::Rgba& token) {
    return ImVec4(token.r, token.g, token.b, token.a);
}

static ImVec4 mixColor(const ImVec4& a, const ImVec4& b, float t) {
    return ImVec4(
        a.x + (b.x - a.x) * t,
        a.y + (b.y - a.y) * t,
        a.z + (b.z - a.z) * t,
        a.w + (b.w - a.w) * t);
}

static ImVec4 withAlpha(ImVec4 c, float alpha) {
    c.w = alpha;
    return c;
}

static float luminance(const ImVec4& c) {
    return c.x * 0.2126f + c.y * 0.7152f + c.z * 0.0722f;
}

static float srgbChannelToLinear(float channel) {
    return channel <= 0.04045f
        ? channel / 12.92f
        : std::pow((channel + 0.055f) / 1.055f, 2.4f);
}

static float relativeLuminance(const ImVec4& c) {
    return srgbChannelToLinear(c.x) * 0.2126f
        + srgbChannelToLinear(c.y) * 0.7152f
        + srgbChannelToLinear(c.z) * 0.0722f;
}

static float contrastRatio(const ImVec4& a, const ImVec4& b) {
    const float lighter = (std::max)(relativeLuminance(a), relativeLuminance(b));
    const float darker = (std::min)(relativeLuminance(a), relativeLuminance(b));
    return (lighter + 0.05f) / (darker + 0.05f);
}

static ImVec4 compositeOver(const ImVec4& foreground, const ImVec4& background) {
    return ImVec4(
        foreground.x * foreground.w + background.x * (1.0f - foreground.w),
        foreground.y * foreground.w + background.y * (1.0f - foreground.w),
        foreground.z * foreground.w + background.z * (1.0f - foreground.w),
        1.0f);
}

static const ThemeEditorItem* findEditorItem(const char* key) {
    size_t count = 0;
    const ThemeEditorItem* items = GetThemeEditorItems(count);
    for (size_t i = 0; i < count; ++i) {
        if (std::string(items[i].key) == key) {
            return &items[i];
        }
    }
    return nullptr;
}

static bool hasEditableProperty(const char* key) {
    for (const auto& prop : ThemeColors::getEditableProperties()) {
        if (std::string(prop.name) == key) {
            return true;
        }
    }
    return false;
}

static std::string readRepoSourceFile(const char* relativePath) {
    const std::filesystem::path candidates[] = {
        std::filesystem::current_path() / relativePath,
        std::filesystem::current_path().parent_path() / relativePath,
        std::filesystem::path(__FILE__).parent_path().parent_path() / relativePath,
    };
    for (const auto& candidate : candidates) {
        std::ifstream in(candidate, std::ios::binary);
        if (in) {
            return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }
    }
    return {};
}

static std::string extractRoseCedarBuiltinBlock() {
    const std::string source = readRepoSourceFile("src/BuiltinThemeScheme.cpp");
    const std::string functionMarker = "ThemeColorScheme MakeBuiltinScheme";
    const size_t functionStart = source.find(functionMarker);
    if (functionStart == std::string::npos) {
        return {};
    }
    const std::string marker = "case AppTheme::RoseCedar:";
    const size_t start = source.find(marker, functionStart);
    if (start == std::string::npos) {
        return {};
    }
    const size_t end = source.find("case AppTheme::LapisBlue:", start);
    if (end == std::string::npos || end <= start) {
        return source.substr(start);
    }
    return source.substr(start, end - start);
}

static std::string extractAuroraBuiltinBlock() {
    const std::string source = readRepoSourceFile("src/BuiltinThemeScheme.cpp");
    const std::string functionMarker = "ThemeColorScheme MakeBuiltinScheme";
    const size_t functionStart = source.find(functionMarker);
    if (functionStart == std::string::npos) {
        return {};
    }
    const std::string marker = "case AppTheme::Ocean:";
    const size_t start = source.find(marker, functionStart);
    if (start == std::string::npos) {
        return {};
    }
    const size_t end = source.find("case AppTheme::RoseCedar:", start);
    if (end == std::string::npos || end <= start) {
        return source.substr(start);
    }
    return source.substr(start, end - start);
}

int main() {
    {
        const ThemeColorScheme scheme = ThemeColors::getHarkenSearchProductDark();

        CHECK(approxEqual(scheme.mainWindowBG, tokenColor(aris::design::kColorSurfaceCanvas)),
              "Aris product theme maps surface.canvas to the main window");
        CHECK(approxEqual(scheme.toolbarBG, tokenColor(aris::design::kColorSurfacePanel)) &&
              approxEqual(scheme.resultsBG, tokenColor(aris::design::kColorSurfacePanel)),
              "Aris product theme maps surface.panel to shared panels");
        CHECK(approxEqual(scheme.popupBG, tokenColor(aris::design::kColorSurfaceElevated)) &&
              approxEqual(scheme.dialogBG, tokenColor(aris::design::kColorSurfaceElevated)),
              "Aris product theme maps surface.elevated to overlays");
        CHECK(approxEqual(scheme.frameBgBase, tokenColor(aris::design::kColorSurfaceInput)),
              "Aris product theme maps surface.input to control frames");
        CHECK(approxEqual(scheme.menuText, tokenColor(aris::design::kColorTextPrimary)) &&
              approxEqual(scheme.resultsText, tokenColor(aris::design::kColorTextPrimary)),
              "Aris product theme maps text.primary to primary copy");
        CHECK(approxEqual(scheme.interfaceText, tokenColor(aris::design::kColorTextSecondary)) &&
              approxEqual(scheme.resultsSecondaryText, tokenColor(aris::design::kColorTextSecondary)),
              "Aris product theme maps text.secondary to supporting copy");
        CHECK(approxEqual(scheme.chromeBorder, tokenColor(aris::design::kColorBorderSubtle)) &&
              approxEqual(scheme.resultsOutline, tokenColor(aris::design::kColorBorderSubtle)),
              "Aris product theme maps border.subtle to structural borders");
    }

    {
        const ThemeColorScheme scheme = ThemeColors::getHarkenSearchProductDark();

        CHECK(approxEqual(scheme.controlAccent, tokenColor(aris::design::kColorActionPrimary)) &&
              approxEqual(scheme.searchButtonBG, tokenColor(aris::design::kColorActionPrimary)),
              "Aris product theme maps action.primary to executable actions");
        CHECK(approxEqual(scheme.waveformColor, tokenColor(aris::design::kColorSignalMeasurement)),
              "Aris product theme maps signal.measurement to waveform data");
        CHECK(approxEqual(scheme.waveformPositionLine, tokenColor(aris::design::kColorSignalPlayhead)),
              "Aris product theme maps signal.playhead to the timeline cursor");
        CHECK(approxEqual(scheme.volumeColor, tokenColor(aris::design::kColorStatusSuccess)) &&
              approxEqual(scheme.meterBase, tokenColor(aris::design::kColorStatusSuccess)),
              "Aris product theme maps status.success to healthy meter state");
        CHECK(approxEqual(scheme.meterWarning, tokenColor(aris::design::kColorStatusWarning)),
              "Aris product theme maps status.warning to the warning meter state");
        CHECK(approxEqual(scheme.volumeClipColor, tokenColor(aris::design::kColorStatusDanger)) &&
              approxEqual(scheme.meterClip, tokenColor(aris::design::kColorStatusDanger)),
              "Aris product theme maps status.danger to clipping state");
    }

    {
        const ThemeColorScheme canonical = ThemeColors::getHarkenSearchProductDark();
        const ThemeColorScheme legacy = ThemeColors::getStudioSlate();

        CHECK(approxEqual(legacy.mainWindowBG, canonical.mainWindowBG) &&
              approxEqual(legacy.controlAccent, canonical.controlAccent) &&
              approxEqual(legacy.waveformColor, canonical.waveformColor),
              "Studio Slate remains a backward-compatible alias of Aris Product Dark");
    }

    std::cout << "=== Theme Color Contract Tests ===\n\n";

    {
        const std::string mainSource = readRepoSourceFile("src/main.cpp");
        const std::string schemeSource = readRepoSourceFile("src/BuiltinThemeScheme.cpp");
        CHECK(mainSource.find("static ThemeColorScheme MakeBuiltinScheme") == std::string::npos,
              "legacy main.cpp static MakeBuiltinScheme marker is gone after extraction");
        CHECK(schemeSource.find("ThemeColorScheme MakeBuiltinScheme") != std::string::npos,
              "extracted BuiltinThemeScheme.cpp owns MakeBuiltinScheme");
        CHECK(schemeSource.find("static ThemeColorScheme MakeBuiltinScheme") == std::string::npos,
              "extracted MakeBuiltinScheme is not a static local helper");
        const std::string aurora = extractAuroraBuiltinBlock();
        CHECK(!aurora.empty(), "source contract can locate Aurora built-in theme block");
        CHECK(aurora.find("s.popupBG                = H(0xf7101a28);") != std::string::npos,
              "Aurora tooltips use the dark popup family for readable primary and secondary text");
        CHECK(aurora.find("s.popupBG                = H(0xf78299b9);") == std::string::npos,
              "Aurora tooltips do not regress to the low-contrast light blue-gray surface");
        CHECK(aurora.find("s.menuText               = H(0xffe2ebf4);") != std::string::npos &&
              aurora.find("s.interfaceText          = H(0xffffffff);") != std::string::npos &&
              aurora.find("s.resultsSecondaryText   = H(0xff7d8da2);") != std::string::npos,
              "Aurora tooltip contrast contract is tied to the runtime foreground roles");
        const ImVec4 tooltipBg(0x10 / 255.0f, 0x1a / 255.0f, 0x28 / 255.0f, 0xf7 / 255.0f);
        const ImVec4 primaryText(0xe2 / 255.0f, 0xeb / 255.0f, 0xf4 / 255.0f, 1.0f);
        const ImVec4 secondaryText(0x7d / 255.0f, 0x8d / 255.0f, 0xa2 / 255.0f, 1.0f);
        const ImVec4 worstCaseCompositedBg = compositeOver(tooltipBg, ImVec4(1, 1, 1, 1));
        CHECK(contrastRatio(primaryText, worstCaseCompositedBg) >= 4.5f,
              "Aurora tooltip primary text meets the normal-text contrast floor");
        CHECK(contrastRatio(secondaryText, worstCaseCompositedBg) >= 4.5f,
              "Aurora tooltip secondary text meets the normal-text contrast floor");
    }

    {
        const std::string roseCedar = extractRoseCedarBuiltinBlock();
        CHECK(!roseCedar.empty(), "source contract can locate RoseCedar built-in theme block");
        CHECK(roseCedar.find("s.mainWindowBG           = H(0xfff6edf1);") != std::string::npos,
              "RoseCedar large backplate is distinct from raised content surfaces");
        CHECK(roseCedar.find("s.frameBgBase            = H(0xfffcf9fa);") != std::string::npos,
              "RoseCedar raised frames use the lightest blush surface");
        CHECK(roseCedar.find("s.chromeBorder           = H(0xffb996a7);") != std::string::npos,
              "RoseCedar global module border is visible on pale blush surfaces");
        CHECK(roseCedar.find("s.toolbarBG              = H(0xffeadbe2);") != std::string::npos,
              "RoseCedar toolbar is a clearer shelf above content");
        CHECK(roseCedar.find("s.resultsBG              = H(0xfffcf9fa);") != std::string::npos,
              "RoseCedar result list uses raised near-white blush background");
        CHECK(roseCedar.find("s.resultsHeaderBG        = H(0xfff0e1e7);") != std::string::npos,
              "RoseCedar result header separates from the table body");
        CHECK(roseCedar.find("s.resultsOutline         = H(0xffc3a3b2);") != std::string::npos,
              "RoseCedar result list border stays visible without becoming heavy");
        CHECK(roseCedar.find("s.resultsSecondaryText   = H(0xff6f5864);") != std::string::npos,
              "RoseCedar dense secondary text stays readable on blush surfaces");
        CHECK(roseCedar.find("s.transportBarBG         = H(0xffe1cfd7);") != std::string::npos,
              "RoseCedar transport strip is a distinct clean lower shelf");
        CHECK(roseCedar.find("s.resultsSelection       = H(0xffc78399);") != std::string::npos,
              "RoseCedar row selection uses a softer dusty rose fill");
        CHECK(roseCedar.find("s.resultsActiveBorder    = H(0x486fafd2);") != std::string::npos,
              "RoseCedar center focus border stays present without a loud rectangle");
        CHECK(roseCedar.find("s.metadataPanelBG        = H(0xfff7eef2);") != std::string::npos,
              "RoseCedar metadata panel separates from the cover wall");
        CHECK(roseCedar.find("s.metadataPanelLabel     = H(0xff604852);") != std::string::npos,
              "RoseCedar info panel edge derivation uses a darker mauve label tone");
        CHECK(roseCedar.find("s.coverWallBG            = H(0xfffcf9fa);") != std::string::npos,
              "RoseCedar cover wall uses a clean raised image container");
        CHECK(roseCedar.find("s.waveformBG             = H(0xfffcf9fa);") != std::string::npos,
              "RoseCedar waveform canvas reads as a raised data module");
        CHECK(roseCedar.find("s.waveformPanelBorder    = H(0xffc3a3b2);") != std::string::npos,
              "RoseCedar waveform panel border matches module hierarchy");
        CHECK(roseCedar.find("s.filterBrowserBG        = H(0xfff4eaee);") != std::string::npos,
              "RoseCedar sidebar backplate is distinct from the center table");
        CHECK(roseCedar.find("s.filterBrowserOutline   = H(0xffaa8496);") != std::string::npos,
              "RoseCedar filter browser outline is strong enough to read as a real boundary");
        CHECK(roseCedar.find("s.waveformColor          = H(0xff6fafd2);") != std::string::npos,
              "RoseCedar waveform keeps cool blue information axis");
        CHECK(roseCedar.find("0xffe04080") == std::string::npos,
              "RoseCedar no longer uses bright candy-magenta playlist accent");
        CHECK(roseCedar.find("0xffeb3c37") == std::string::npos,
              "RoseCedar no longer uses fire-red waveform selection edge");
        CHECK(readRepoSourceFile("src/BuiltinThemeScheme.cpp").find("s.filterBrowserSearchHitText = H(0xff1d6d55);") != std::string::npos,
              "JadeMist folder-tree search hits use a darker jade text color for readability");
    }

    {
        std::string source = readRepoSourceFile("src/main.cpp");
        source.erase(std::remove(source.begin(), source.end(), '\r'), source.end());
        const std::string themeSource = readRepoSourceFile("src/Theme.cpp");
        const std::string themeHeader = readRepoSourceFile("src/Theme.h");
        const std::string widgetSource = readRepoSourceFile("src/HarkenSearchWidgets.cpp");
        CHECK(source.find("drawRoseCedarDockModuleStroke") == std::string::npos,
              "RoseCedar no longer relies on an ad-hoc overlay stroke outside the shared layout system");
        CHECK(source.find("ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize,   1.0f);") != std::string::npos,
              "sidebar keeps visible 1px input framing for structural filter controls in every theme");
        CHECK(source.find("static ExplorerSource DatabaseExplorerSource()") != std::string::npos &&
              source.find("\"##RelatedPathsBox\"") != std::string::npos &&
              source.find("RenderRelatedPathsExplorerPane(") != std::string::npos &&
              source.find("BeginTransientScrollbarChild(\n        childId,\n        ImVec2(0, state.settings.sidebarRelatedPathsHeight)") != std::string::npos,
              "sidebar keeps a dedicated related-paths region in source");
        CHECK(source.find("\"FolderTree\"};") != std::string::npos &&
              source.find("RenderFolderTreeExplorerList(\n            databaseSource.treeListChildId,") != std::string::npos &&
              source.find("BeginTransientScrollbarChild(childId, ImVec2(-1, treeH), true);") != std::string::npos,
              "sidebar folder tree is rendered inside a bounded child region");
        CHECK(source.find("mixColor(tcs.filterBrowserSearchHitText, tcs.filterBrowserBG, isLightTreeTheme ? 0.64f : 0.78f)") != std::string::npos,
              "light sidebar themes strengthen folder-tree search-hit fill");
        CHECK(themeSource.find("ThemeColors::popupModalBG(cs)") != std::string::npos,
              "secondary dialogs use popup modal color tokens");
        CHECK(themeHeader.find("static const ImVec4& popupMenuBG") != std::string::npos &&
              themeHeader.find("static const ImVec4& popupComboBG") != std::string::npos &&
              themeHeader.find("static const ImVec4& popupTooltipBG") != std::string::npos &&
              themeHeader.find("static const ImVec4& popupModalBG") != std::string::npos &&
              themeHeader.find("static ImVec4 popupSelectionBG") != std::string::npos,
              "theme API exposes semantic popup color tokens without new geometry tokens");
        CHECK(widgetSource.find("ThemeColors::popupComboBG(th)") != std::string::npos &&
              widgetSource.find("ThemeColors::popupTooltipBG(th)") != std::string::npos &&
              widgetSource.find("ThemeColors::popupMenuBG(th)") != std::string::npos &&
              widgetSource.find("ThemeColors::popupModalBG(th)") != std::string::npos,
              "combo, tooltip, menu, and modal widgets consume semantic popup colors");
        CHECK(themeSource.find("colors[ImGuiCol_Header]               = ImVec4(cs.resultsSelection.x, cs.resultsSelection.y, cs.resultsSelection.z, 0.25f);") != std::string::npos &&
              themeSource.find("colors[ImGuiCol_HeaderHovered]        = ImVec4(cs.resultsSelection.x, cs.resultsSelection.y, cs.resultsSelection.z, 0.40f);") != std::string::npos &&
              themeSource.find("colors[ImGuiCol_HeaderActive]         = ImVec4(cs.resultsSelection.x, cs.resultsSelection.y, cs.resultsSelection.z, 0.80f);") != std::string::npos,
              "popup color recovery does not change global list and tree selection colors");
        CHECK(themeSource.find("ImGuiStyleVar_WindowPadding, ImVec2(18.0f, 18.0f)") != std::string::npos &&
              themeSource.find("ImGuiStyleVar_FramePadding, ImVec2(12.0f, 6.0f)") != std::string::npos &&
              themeSource.find("ImGuiStyleVar_ItemSpacing, ImVec2(12.0f, 12.0f)") != std::string::npos &&
              themeSource.find("ImGuiStyleVar_ItemInnerSpacing, ImVec2(8.0f, 8.0f)") != std::string::npos,
              "popup color recovery preserves dialog geometry");
        CHECK(source.find("PushCompactPopupMenuStyle") == std::string::npos,
              "popup color recovery does not introduce compact spacing");
        CHECK(source.find("ImDrawList* csDl = ImGui::GetWindowDrawList();") != std::string::npos,
              "soundstage drawing stays below secondary windows");
    }

    {
        const ThemeEditorItem* timecode = findEditorItem("timecodeText");
        CHECK(timecode != nullptr, "editor exposes timecodeText");
        if (timecode) {
            CHECK(timecode->group == ThemeEditorGroup::TopBar, "timecodeText is grouped under TopBar");
        }

        const ThemeEditorItem* timecodeHover = findEditorItem("timecodeTextHover");
        CHECK(timecodeHover != nullptr, "editor exposes timecodeTextHover");
        if (timecodeHover) {
            CHECK(timecodeHover->group == ThemeEditorGroup::TopBar, "timecodeTextHover is grouped under TopBar");
        }

        const ThemeEditorItem* frameBgBase = findEditorItem("frameBgBase");
        CHECK(frameBgBase != nullptr, "editor exposes frameBgBase");
        if (frameBgBase) {
            CHECK(frameBgBase->group == ThemeEditorGroup::Foundation, "frameBgBase is grouped under Foundation");
        }

        const ThemeEditorItem* chromeBorder = findEditorItem("chromeBorder");
        CHECK(chromeBorder != nullptr, "editor exposes chromeBorder");
        if (chromeBorder) {
            CHECK(chromeBorder->group == ThemeEditorGroup::Foundation, "chromeBorder is grouped under Foundation");
        }

        const ThemeEditorItem* waveformSecondary = findEditorItem("waveformSecondaryColor");
        CHECK(waveformSecondary != nullptr, "editor exposes waveformSecondaryColor");
        if (waveformSecondary) {
            CHECK(waveformSecondary->group == ThemeEditorGroup::Waveform, "waveformSecondaryColor is grouped under Waveform");
        }

        const ThemeEditorItem* waveformFooterBg = findEditorItem("waveformFooterBG");
        CHECK(waveformFooterBg != nullptr, "editor exposes waveformFooterBG");
        if (waveformFooterBg) {
            CHECK(waveformFooterBg->group == ThemeEditorGroup::Waveform, "waveformFooterBG is grouped under Waveform");
        }

        const ThemeEditorItem* waveformPanelBorder = findEditorItem("waveformPanelBorder");
        CHECK(waveformPanelBorder != nullptr, "editor exposes waveformPanelBorder");
        if (waveformPanelBorder) {
            CHECK(waveformPanelBorder->group == ThemeEditorGroup::Waveform, "waveformPanelBorder is grouped under Waveform");
        }

        const ThemeEditorItem* waveformSplitterBg = findEditorItem("waveformSplitterBG");
        CHECK(waveformSplitterBg != nullptr, "editor exposes waveformSplitterBG");
        if (waveformSplitterBg) {
            CHECK(waveformSplitterBg->group == ThemeEditorGroup::Waveform, "waveformSplitterBG is grouped under Waveform");
        }

        const ThemeEditorItem* waveformSplitterLine = findEditorItem("waveformSplitterLine");
        CHECK(waveformSplitterLine != nullptr, "editor exposes waveformSplitterLine");
        if (waveformSplitterLine) {
            CHECK(waveformSplitterLine->group == ThemeEditorGroup::Waveform, "waveformSplitterLine is grouped under Waveform");
        }

        const ThemeEditorItem* waveformSelectionDim = findEditorItem("waveformSelectionDim");
        CHECK(waveformSelectionDim != nullptr, "editor exposes waveformSelectionDim");
        if (waveformSelectionDim) {
            CHECK(waveformSelectionDim->group == ThemeEditorGroup::Waveform, "waveformSelectionDim is grouped under Waveform");
        }

        const ThemeEditorItem* waveformSelectionEdge = findEditorItem("waveformSelectionEdge");
        CHECK(waveformSelectionEdge != nullptr, "editor exposes waveformSelectionEdge");
        if (waveformSelectionEdge) {
            CHECK(waveformSelectionEdge->group == ThemeEditorGroup::Waveform, "waveformSelectionEdge is grouped under Waveform");
        }

        const ThemeEditorItem* menuPopupBg = findEditorItem("menuPopupBG");
        CHECK(menuPopupBg != nullptr, "editor exposes menuPopupBG");
        if (menuPopupBg) {
            CHECK(menuPopupBg->group == ThemeEditorGroup::TopBar, "menuPopupBG is grouped under TopBar");
        }

        const ThemeEditorItem* dialogBg = findEditorItem("dialogBG");
        CHECK(dialogBg != nullptr, "editor exposes dialogBG");
        if (dialogBg) {
            CHECK(dialogBg->group == ThemeEditorGroup::TopBar, "dialogBG is grouped under TopBar");
        }

        const ThemeEditorItem* dialogPanelBg = findEditorItem("dialogPanelBG");
        CHECK(dialogPanelBg != nullptr, "editor exposes dialogPanelBG");
        if (dialogPanelBg) {
            CHECK(dialogPanelBg->group == ThemeEditorGroup::TopBar, "dialogPanelBG is grouped under TopBar");
        }

        const ThemeEditorItem* toolbarAccent = findEditorItem("toolbarAccent");
        CHECK(toolbarAccent != nullptr, "editor exposes toolbarAccent");
        if (toolbarAccent) {
            CHECK(toolbarAccent->group == ThemeEditorGroup::TopBar, "toolbarAccent is grouped under TopBar");
        }

        const ThemeEditorItem* historyCapsuleBg = findEditorItem("historyCapsuleBG");
        CHECK(historyCapsuleBg != nullptr, "editor exposes historyCapsuleBG");
        if (historyCapsuleBg) {
            CHECK(historyCapsuleBg->group == ThemeEditorGroup::TopBar, "historyCapsuleBG is grouped under TopBar");
        }

        const ThemeEditorItem* historyCapsuleActiveBg = findEditorItem("historyCapsuleActiveBG");
        CHECK(historyCapsuleActiveBg != nullptr, "editor exposes historyCapsuleActiveBG");
        if (historyCapsuleActiveBg) {
            CHECK(historyCapsuleActiveBg->group == ThemeEditorGroup::TopBar, "historyCapsuleActiveBG is grouped under TopBar");
        }

        const ThemeEditorItem* modeCapsuleBg = findEditorItem("modeCapsuleBG");
        CHECK(modeCapsuleBg != nullptr, "editor exposes modeCapsuleBG");
        if (modeCapsuleBg) {
            CHECK(modeCapsuleBg->group == ThemeEditorGroup::TopBar, "modeCapsuleBG is grouped under TopBar");
        }

        const ThemeEditorItem* modeCapsuleActiveBg = findEditorItem("modeCapsuleActiveBG");
        CHECK(modeCapsuleActiveBg != nullptr, "editor exposes modeCapsuleActiveBG");
        if (modeCapsuleActiveBg) {
            CHECK(modeCapsuleActiveBg->group == ThemeEditorGroup::TopBar, "modeCapsuleActiveBG is grouped under TopBar");
        }

        const ThemeEditorItem* playlistHighlight = findEditorItem("playlistGradientHighlight");
        CHECK(playlistHighlight != nullptr, "editor exposes playlistGradientHighlight");
        if (playlistHighlight) {
            CHECK(playlistHighlight->group == ThemeEditorGroup::Results, "playlistGradientHighlight is grouped under Results");
        }

        const ThemeEditorItem* playlistShadow = findEditorItem("playlistGradientShadow");
        CHECK(playlistShadow != nullptr, "editor exposes playlistGradientShadow");
        if (playlistShadow) {
            CHECK(playlistShadow->group == ThemeEditorGroup::Results, "playlistGradientShadow is grouped under Results");
        }

        const ThemeEditorItem* menuText = findEditorItem("menuText");
        CHECK(menuText != nullptr, "editor exposes menuText");
        if (menuText) {
            CHECK(menuText->group == ThemeEditorGroup::TopBar, "menuText is grouped under TopBar");
        }

        const ThemeEditorItem* resultsTabBg = findEditorItem("resultsTabBG");
        CHECK(resultsTabBg != nullptr, "editor exposes resultsTabBG");
        if (resultsTabBg) {
            CHECK(resultsTabBg->group == ThemeEditorGroup::Results, "resultsTabBG is grouped under Results");
        }

        const ThemeEditorItem* resultsTabHover = findEditorItem("resultsTabHover");
        CHECK(resultsTabHover != nullptr, "editor exposes resultsTabHover");
        if (resultsTabHover) {
            CHECK(resultsTabHover->group == ThemeEditorGroup::Results, "resultsTabHover is grouped under Results");
        }

        const ThemeEditorItem* resultsTabActive = findEditorItem("resultsTabActive");
        CHECK(resultsTabActive != nullptr, "editor exposes resultsTabActive");
        if (resultsTabActive) {
            CHECK(resultsTabActive->group == ThemeEditorGroup::Results, "resultsTabActive is grouped under Results");
        }

        const ThemeEditorItem* resultsActiveBorder = findEditorItem("resultsActiveBorder");
        CHECK(resultsActiveBorder != nullptr, "editor exposes resultsActiveBorder");
        if (resultsActiveBorder) {
            CHECK(resultsActiveBorder->group == ThemeEditorGroup::Results, "resultsActiveBorder is grouped under Results");
        }

        const ThemeEditorItem* secondaryText = findEditorItem("resultsSecondaryText");
        CHECK(secondaryText != nullptr, "editor exposes resultsSecondaryText");
        if (secondaryText) {
            CHECK(secondaryText->group == ThemeEditorGroup::Results, "resultsSecondaryText is grouped under Results");
        }

        const ThemeEditorItem* filterSectionBg = findEditorItem("filterBrowserSectionBG");
        CHECK(filterSectionBg != nullptr, "editor exposes filterBrowserSectionBG");
        if (filterSectionBg) {
            CHECK(filterSectionBg->group == ThemeEditorGroup::FilterBrowser, "filterBrowserSectionBG is grouped under FilterBrowser");
        }

        const ThemeEditorItem* filterButtonBg = findEditorItem("filterBrowserButtonBG");
        CHECK(filterButtonBg != nullptr, "editor exposes filterBrowserButtonBG");
        if (filterButtonBg) {
            CHECK(filterButtonBg->group == ThemeEditorGroup::FilterBrowser, "filterBrowserButtonBG is grouped under FilterBrowser");
        }

        const ThemeEditorItem* filterTreeText = findEditorItem("filterBrowserTreeText");
        CHECK(filterTreeText != nullptr, "editor exposes filterBrowserTreeText");
        if (filterTreeText) {
            CHECK(filterTreeText->group == ThemeEditorGroup::FilterBrowser, "filterBrowserTreeText is grouped under FilterBrowser");
        }

        const ThemeEditorItem* filterTreeSelection = findEditorItem("filterBrowserTreeSelection");
        CHECK(filterTreeSelection != nullptr, "editor exposes filterBrowserTreeSelection");
        if (filterTreeSelection) {
            CHECK(filterTreeSelection->group == ThemeEditorGroup::FilterBrowser, "filterBrowserTreeSelection is grouped under FilterBrowser");
        }

        const ThemeEditorItem* filterTreeSearchHit = findEditorItem("filterBrowserSearchHitText");
        CHECK(filterTreeSearchHit != nullptr, "editor exposes filterBrowserSearchHitText");
        if (filterTreeSearchHit) {
            CHECK(filterTreeSearchHit->group == ThemeEditorGroup::FilterBrowser, "filterBrowserSearchHitText is grouped under FilterBrowser");
        }

        const ThemeEditorItem* filterLocateTint = findEditorItem("filterBrowserLocateTint");
        CHECK(filterLocateTint != nullptr, "editor exposes filterBrowserLocateTint");
        if (filterLocateTint) {
            CHECK(filterLocateTint->group == ThemeEditorGroup::FilterBrowser, "filterBrowserLocateTint is grouped under FilterBrowser");
        }

        const ThemeEditorItem* meterBg = findEditorItem("meterBg");
        CHECK(meterBg != nullptr, "editor exposes meterBg");
        if (meterBg) {
            CHECK(meterBg->group == ThemeEditorGroup::ButtonsAndStatus, "meterBg is grouped under ButtonsAndStatus");
        }

        const ThemeEditorItem* meterBase = findEditorItem("meterBase");
        CHECK(meterBase != nullptr, "editor exposes meterBase");
        if (meterBase) {
            CHECK(meterBase->group == ThemeEditorGroup::ButtonsAndStatus, "meterBase is grouped under ButtonsAndStatus");
        }

        const ThemeEditorItem* meterMid = findEditorItem("meterMid");
        CHECK(meterMid != nullptr, "editor exposes meterMid");
        if (meterMid) {
            CHECK(meterMid->group == ThemeEditorGroup::ButtonsAndStatus, "meterMid is grouped under ButtonsAndStatus");
        }

        const ThemeEditorItem* meterWarning = findEditorItem("meterWarning");
        CHECK(meterWarning != nullptr, "editor exposes meterWarning");
        if (meterWarning) {
            CHECK(meterWarning->group == ThemeEditorGroup::ButtonsAndStatus, "meterWarning is grouped under ButtonsAndStatus");
        }

        const ThemeEditorItem* meterClip = findEditorItem("meterClip");
        CHECK(meterClip != nullptr, "editor exposes meterClip");
        if (meterClip) {
            CHECK(meterClip->group == ThemeEditorGroup::ButtonsAndStatus, "meterClip is grouped under ButtonsAndStatus");
        }

        const ThemeEditorItem* meterScaleText = findEditorItem("meterScaleText");
        CHECK(meterScaleText != nullptr, "editor exposes meterScaleText");
        if (meterScaleText) {
            CHECK(meterScaleText->group == ThemeEditorGroup::ButtonsAndStatus, "meterScaleText is grouped under ButtonsAndStatus");
        }

        const ThemeEditorItem* diagnosticOverlayBg = findEditorItem("diagnosticOverlayBG");
        CHECK(diagnosticOverlayBg != nullptr, "editor exposes diagnosticOverlayBG");
        if (diagnosticOverlayBg) {
            CHECK(diagnosticOverlayBg->group == ThemeEditorGroup::ButtonsAndStatus, "diagnosticOverlayBG is grouped under ButtonsAndStatus");
        }

        const ThemeEditorItem* diagnosticOverlayText = findEditorItem("diagnosticOverlayText");
        CHECK(diagnosticOverlayText != nullptr, "editor exposes diagnosticOverlayText");
        if (diagnosticOverlayText) {
            CHECK(diagnosticOverlayText->group == ThemeEditorGroup::ButtonsAndStatus, "diagnosticOverlayText is grouped under ButtonsAndStatus");
        }

        CHECK(hasEditableProperty("timecodeText"), "editable properties include timecodeText");
        CHECK(hasEditableProperty("timecodeTextHover"), "editable properties include timecodeTextHover");
        CHECK(hasEditableProperty("waveformSecondaryColor"), "editable properties include waveformSecondaryColor");
        CHECK(hasEditableProperty("waveformPanelBorder"), "editable properties include waveformPanelBorder");
        CHECK(hasEditableProperty("waveformSplitterBG"), "editable properties include waveformSplitterBG");
        CHECK(hasEditableProperty("waveformSplitterLine"), "editable properties include waveformSplitterLine");
        CHECK(hasEditableProperty("waveformFooterBG"), "editable properties include waveformFooterBG");
        CHECK(hasEditableProperty("waveformSelectionDim"), "editable properties include waveformSelectionDim");
        CHECK(hasEditableProperty("waveformSelectionHearthTop"), "editable properties include waveformSelectionHearthTop");
        CHECK(hasEditableProperty("waveformSelectionHearthBot"), "editable properties include waveformSelectionHearthBot");
        CHECK(hasEditableProperty("waveformSelectionEdge"), "editable properties include waveformSelectionEdge");
        CHECK(hasEditableProperty("menuPopupBG"), "editable properties include menuPopupBG");
        CHECK(hasEditableProperty("dialogBG"), "editable properties include dialogBG");
        CHECK(hasEditableProperty("dialogPanelBG"), "editable properties include dialogPanelBG");
        CHECK(hasEditableProperty("frameBgBase"), "editable properties include frameBgBase");
        CHECK(hasEditableProperty("chromeBorder"), "editable properties include chromeBorder");
        CHECK(hasEditableProperty("popupBG"), "editable properties include popupBG");
        CHECK(hasEditableProperty("historyCapsuleBG"), "editable properties include historyCapsuleBG");
        CHECK(hasEditableProperty("historyCapsuleActiveBG"), "editable properties include historyCapsuleActiveBG");
        CHECK(hasEditableProperty("modeCapsuleBG"), "editable properties include modeCapsuleBG");
        CHECK(hasEditableProperty("modeCapsuleActiveBG"), "editable properties include modeCapsuleActiveBG");
        CHECK(hasEditableProperty("toolbarAccent"), "editable properties include toolbarAccent");
        CHECK(hasEditableProperty("controlAccent"), "editable properties include controlAccent");
        CHECK(hasEditableProperty("resultsGradientStart"), "editable properties include resultsGradientStart");
        CHECK(hasEditableProperty("resultsGradientEnd"), "editable properties include resultsGradientEnd");
        CHECK(hasEditableProperty("playlistGradientHighlight"), "editable properties include playlistGradientHighlight");
        CHECK(hasEditableProperty("playlistGradientShadow"), "editable properties include playlistGradientShadow");
        CHECK(hasEditableProperty("menuText"), "editable properties include menuText");
        CHECK(hasEditableProperty("resultsTabBG"), "editable properties include resultsTabBG");
        CHECK(hasEditableProperty("resultsTabHover"), "editable properties include resultsTabHover");
        CHECK(hasEditableProperty("resultsTabActive"), "editable properties include resultsTabActive");
        CHECK(hasEditableProperty("resultsActiveBorder"), "editable properties include resultsActiveBorder");
        CHECK(hasEditableProperty("resultsSecondaryText"), "editable properties include resultsSecondaryText");
        CHECK(hasEditableProperty("coverWallBG"), "editable properties include coverWallBG");
        CHECK(hasEditableProperty("filterBrowserSectionBG"), "editable properties include filterBrowserSectionBG");
        CHECK(hasEditableProperty("filterBrowserButtonBG"), "editable properties include filterBrowserButtonBG");
        CHECK(hasEditableProperty("filterBrowserTreeText"), "editable properties include filterBrowserTreeText");
        CHECK(hasEditableProperty("filterBrowserTreeSelection"), "editable properties include filterBrowserTreeSelection");
        CHECK(hasEditableProperty("filterBrowserSearchHitText"), "editable properties include filterBrowserSearchHitText");
        CHECK(hasEditableProperty("filterBrowserLocateTint"), "editable properties include filterBrowserLocateTint");
        CHECK(hasEditableProperty("meterBg"), "editable properties include meterBg");
        CHECK(hasEditableProperty("meterBase"), "editable properties include meterBase");
        CHECK(hasEditableProperty("meterMid"), "editable properties include meterMid");
        CHECK(hasEditableProperty("meterWarning"), "editable properties include meterWarning");
        CHECK(hasEditableProperty("meterClip"), "editable properties include meterClip");
        CHECK(hasEditableProperty("meterScaleText"), "editable properties include meterScaleText");
        CHECK(hasEditableProperty("diagnosticOverlayBG"), "editable properties include diagnosticOverlayBG");
        CHECK(hasEditableProperty("diagnosticOverlayText"), "editable properties include diagnosticOverlayText");
        CHECK(hasEditableProperty("diagnosticOverlayAccent"), "editable properties include diagnosticOverlayAccent");
        CHECK(hasEditableProperty("diagnosticOverlayMutedText"), "editable properties include diagnosticOverlayMutedText");
    }

    {
        ThemeColorScheme scheme = ThemeColors::getElectricBlue();
        scheme.menuText = ImVec4(0.91f, 0.92f, 0.93f, 1.0f);
        scheme.frameBgBase = ImVec4(39.0f / 255.0f, 44.0f / 255.0f, 58.0f / 255.0f, 1.0f);
        scheme.chromeBorder = ImVec4(96.0f / 255.0f, 102.0f / 255.0f, 114.0f / 255.0f, 1.0f);
        scheme.popupBG = ImVec4(28.0f / 255.0f, 34.0f / 255.0f, 48.0f / 255.0f, 250.0f / 255.0f);
        scheme.menuPopupBG = ImVec4(32.0f / 255.0f, 39.0f / 255.0f, 54.0f / 255.0f, 250.0f / 255.0f);
        scheme.dialogBG = ImVec4(245.0f / 255.0f, 239.0f / 255.0f, 232.0f / 255.0f, 250.0f / 255.0f);
        scheme.dialogPanelBG = ImVec4(238.0f / 255.0f, 232.0f / 255.0f, 225.0f / 255.0f, 1.0f);
        scheme.timecodeText = ImVec4(0.11f, 0.22f, 0.33f, 1.0f);
        scheme.timecodeTextHover = ImVec4(0.44f, 0.55f, 0.66f, 1.0f);
        scheme.historyCapsuleBG = ImVec4(74.0f / 255.0f, 85.0f / 255.0f, 104.0f / 255.0f, 1.0f);
        scheme.historyCapsuleActiveBG = ImVec4(242.0f / 255.0f, 244.0f / 255.0f, 248.0f / 255.0f, 1.0f);
        scheme.modeCapsuleBG = ImVec4(68.0f / 255.0f, 80.0f / 255.0f, 100.0f / 255.0f, 1.0f);
        scheme.modeCapsuleActiveBG = ImVec4(246.0f / 255.0f, 248.0f / 255.0f, 252.0f / 255.0f, 1.0f);
        scheme.waveformPanelBorder = ImVec4(199.0f / 255.0f, 205.0f / 255.0f, 218.0f / 255.0f, 1.0f);
        scheme.waveformSplitterBG = ImVec4(239.0f / 255.0f, 235.0f / 255.0f, 228.0f / 255.0f, 1.0f);
        scheme.waveformSplitterLine = ImVec4(231.0f / 255.0f, 224.0f / 255.0f, 214.0f / 255.0f, 1.0f);
        scheme.waveformFooterBG = ImVec4(58.0f / 255.0f, 70.0f / 255.0f, 90.0f / 255.0f, 1.0f);
        scheme.waveformSelectionDim = ImVec4(8.0f / 255.0f, 12.0f / 255.0f, 16.0f / 255.0f, 0.42f);
        scheme.waveformSelectionHearthTop = ImVec4(232.0f / 255.0f, 184.0f / 255.0f, 79.0f / 255.0f, 84.0f / 255.0f);
        scheme.waveformSelectionHearthBot = ImVec4(186.0f / 255.0f, 97.0f / 255.0f, 56.0f / 255.0f, 74.0f / 255.0f);
        scheme.waveformSelectionEdge = ImVec4(250.0f / 255.0f, 217.0f / 255.0f, 105.0f / 255.0f, 1.0f);
        scheme.resultsTabBG = ImVec4(46.0f / 255.0f, 56.0f / 255.0f, 76.0f / 255.0f, 1.0f);
        scheme.resultsTabHover = ImVec4(61.0f / 255.0f, 82.0f / 255.0f, 117.0f / 255.0f, 1.0f);
        scheme.resultsTabActive = ImVec4(82.0f / 255.0f, 112.0f / 255.0f, 163.0f / 255.0f, 1.0f);
        scheme.resultsSecondaryText = ImVec4(0.42f, 0.52f, 0.62f, 1.0f);
        scheme.resultsGradientStart = ImVec4(236.0f / 255.0f, 231.0f / 255.0f, 223.0f / 255.0f, 1.0f);
        scheme.resultsGradientEnd = ImVec4(221.0f / 255.0f, 213.0f / 255.0f, 224.0f / 255.0f, 1.0f);
        scheme.filterBrowserSectionBG = ImVec4(41.0f / 255.0f, 46.0f / 255.0f, 61.0f / 255.0f, 1.0f);
        scheme.filterBrowserButtonBG = ImVec4(46.0f / 255.0f, 66.0f / 255.0f, 92.0f / 255.0f, 1.0f);
        scheme.filterBrowserTreeText = ImVec4(148.0f / 255.0f, 184.0f / 255.0f, 224.0f / 255.0f, 1.0f);
        scheme.filterBrowserTreeSelection = ImVec4(240.0f / 255.0f, 51.0f / 255.0f, 26.0f / 255.0f, 1.0f);
        scheme.filterBrowserSearchHitText = ImVec4(247.0f / 255.0f, 211.0f / 255.0f, 107.0f / 255.0f, 1.0f);
        scheme.filterBrowserLocateTint = ImVec4(184.0f / 255.0f, 209.0f / 255.0f, 158.0f / 255.0f, 1.0f);
        scheme.waveformSecondaryColor = ImVec4(0.12f, 0.72f, 0.42f, 1.0f);
        scheme.searchButtonBG = ImVec4(232.0f / 255.0f, 64.0f / 255.0f, 36.0f / 255.0f, 1.0f);
        scheme.toolbarAccent = ImVec4(194.0f / 255.0f, 143.0f / 255.0f, 209.0f / 255.0f, 1.0f);
        scheme.controlAccent = ImVec4(153.0f / 255.0f, 131.0f / 255.0f, 219.0f / 255.0f, 1.0f);
        scheme.playlistGradientHighlight = ImVec4(250.0f / 255.0f, 242.0f / 255.0f, 204.0f / 255.0f, 1.0f);
        scheme.playlistGradientShadow = ImVec4(46.0f / 255.0f, 51.0f / 255.0f, 71.0f / 255.0f, 1.0f);
        scheme.coverWallBG = ImVec4(238.0f / 255.0f, 234.0f / 255.0f, 227.0f / 255.0f, 1.0f);
        scheme.meterBg = ImVec4(230.0f / 255.0f, 224.0f / 255.0f, 218.0f / 255.0f, 1.0f);
        scheme.meterBase = ImVec4(184.0f / 255.0f, 203.0f / 255.0f, 165.0f / 255.0f, 1.0f);
        scheme.meterMid = ImVec4(205.0f / 255.0f, 209.0f / 255.0f, 152.0f / 255.0f, 1.0f);
        scheme.meterWarning = ImVec4(231.0f / 255.0f, 205.0f / 255.0f, 142.0f / 255.0f, 1.0f);
        scheme.meterClip = ImVec4(216.0f / 255.0f, 160.0f / 255.0f, 153.0f / 255.0f, 1.0f);
        scheme.meterScaleText = ImVec4(128.0f / 255.0f, 142.0f / 255.0f, 156.0f / 255.0f, 1.0f);
        scheme.diagnosticOverlayBG = ImVec4(36.0f / 255.0f, 42.0f / 255.0f, 58.0f / 255.0f, 235.0f / 255.0f);
        scheme.diagnosticOverlayText = ImVec4(232.0f / 255.0f, 236.0f / 255.0f, 244.0f / 255.0f, 1.0f);
        scheme.diagnosticOverlayAccent = ImVec4(148.0f / 255.0f, 202.0f / 255.0f, 188.0f / 255.0f, 1.0f);
        scheme.diagnosticOverlayMutedText = ImVec4(156.0f / 255.0f, 170.0f / 255.0f, 188.0f / 255.0f, 1.0f);

        const std::string json = ThemeColors::exportToJSON(scheme);
        CHECK(json.find("\"menuText\"") != std::string::npos, "JSON export includes menuText");
        CHECK(json.find("\"frameBgBase\"") != std::string::npos, "JSON export includes frameBgBase");
        CHECK(json.find("\"chromeBorder\"") != std::string::npos, "JSON export includes chromeBorder");
        CHECK(json.find("\"popupBG\"") != std::string::npos, "JSON export includes popupBG");
        CHECK(json.find("\"menuPopupBG\"") != std::string::npos, "JSON export includes menuPopupBG");
        CHECK(json.find("\"dialogBG\"") != std::string::npos, "JSON export includes dialogBG");
        CHECK(json.find("\"dialogPanelBG\"") != std::string::npos, "JSON export includes dialogPanelBG");
        CHECK(json.find("\"historyCapsuleBG\"") != std::string::npos, "JSON export includes historyCapsuleBG");
        CHECK(json.find("\"historyCapsuleActiveBG\"") != std::string::npos, "JSON export includes historyCapsuleActiveBG");
        CHECK(json.find("\"modeCapsuleBG\"") != std::string::npos, "JSON export includes modeCapsuleBG");
        CHECK(json.find("\"modeCapsuleActiveBG\"") != std::string::npos, "JSON export includes modeCapsuleActiveBG");
        CHECK(json.find("\"waveformPanelBorder\"") != std::string::npos, "JSON export includes waveformPanelBorder");
        CHECK(json.find("\"waveformSplitterBG\"") != std::string::npos, "JSON export includes waveformSplitterBG");
        CHECK(json.find("\"waveformSplitterLine\"") != std::string::npos, "JSON export includes waveformSplitterLine");
        CHECK(json.find("\"waveformFooterBG\"") != std::string::npos, "JSON export includes waveformFooterBG");
        CHECK(json.find("\"waveformSelectionDim\"") != std::string::npos, "JSON export includes waveformSelectionDim");
        CHECK(json.find("\"waveformSelectionHearthTop\"") != std::string::npos, "JSON export includes waveformSelectionHearthTop");
        CHECK(json.find("\"waveformSelectionHearthBot\"") != std::string::npos, "JSON export includes waveformSelectionHearthBot");
        CHECK(json.find("\"waveformSelectionEdge\"") != std::string::npos, "JSON export includes waveformSelectionEdge");
        CHECK(json.find("\"timecodeText\"") != std::string::npos, "JSON export includes timecodeText");
        CHECK(json.find("\"timecodeTextHover\"") != std::string::npos, "JSON export includes timecodeTextHover");
        CHECK(json.find("\"resultsTabBG\"") != std::string::npos, "JSON export includes resultsTabBG");
        CHECK(json.find("\"resultsTabHover\"") != std::string::npos, "JSON export includes resultsTabHover");
        CHECK(json.find("\"resultsTabActive\"") != std::string::npos, "JSON export includes resultsTabActive");
        CHECK(json.find("\"resultsSecondaryText\"") != std::string::npos, "JSON export includes resultsSecondaryText");
        CHECK(json.find("\"resultsGradientStart\"") != std::string::npos, "JSON export includes resultsGradientStart");
        CHECK(json.find("\"resultsGradientEnd\"") != std::string::npos, "JSON export includes resultsGradientEnd");
        CHECK(json.find("\"filterBrowserSectionBG\"") != std::string::npos, "JSON export includes filterBrowserSectionBG");
        CHECK(json.find("\"filterBrowserButtonBG\"") != std::string::npos, "JSON export includes filterBrowserButtonBG");
        CHECK(json.find("\"filterBrowserTreeText\"") != std::string::npos, "JSON export includes filterBrowserTreeText");
        CHECK(json.find("\"filterBrowserTreeSelection\"") != std::string::npos, "JSON export includes filterBrowserTreeSelection");
        CHECK(json.find("\"filterBrowserSearchHitText\"") != std::string::npos, "JSON export includes filterBrowserSearchHitText");
        CHECK(json.find("\"filterBrowserLocateTint\"") != std::string::npos, "JSON export includes filterBrowserLocateTint");
        CHECK(json.find("\"waveformSecondaryColor\"") != std::string::npos, "JSON export includes waveformSecondaryColor");
        CHECK(json.find("\"toolbarAccent\"") != std::string::npos, "JSON export includes toolbarAccent");
        CHECK(json.find("\"controlAccent\"") != std::string::npos, "JSON export includes controlAccent");
        CHECK(json.find("\"playlistGradientHighlight\"") != std::string::npos, "JSON export includes playlistGradientHighlight");
        CHECK(json.find("\"playlistGradientShadow\"") != std::string::npos, "JSON export includes playlistGradientShadow");
        CHECK(json.find("\"coverWallBG\"") != std::string::npos, "JSON export includes coverWallBG");
        CHECK(json.find("\"meterBg\"") != std::string::npos, "JSON export includes meterBg");
        CHECK(json.find("\"meterBase\"") != std::string::npos, "JSON export includes meterBase");
        CHECK(json.find("\"meterMid\"") != std::string::npos, "JSON export includes meterMid");
        CHECK(json.find("\"meterWarning\"") != std::string::npos, "JSON export includes meterWarning");
        CHECK(json.find("\"meterClip\"") != std::string::npos, "JSON export includes meterClip");
        CHECK(json.find("\"meterScaleText\"") != std::string::npos, "JSON export includes meterScaleText");
        CHECK(json.find("\"diagnosticOverlayBG\"") != std::string::npos, "JSON export includes diagnosticOverlayBG");
        CHECK(json.find("\"diagnosticOverlayText\"") != std::string::npos, "JSON export includes diagnosticOverlayText");
        CHECK(json.find("\"diagnosticOverlayAccent\"") != std::string::npos, "JSON export includes diagnosticOverlayAccent");
        CHECK(json.find("\"diagnosticOverlayMutedText\"") != std::string::npos, "JSON export includes diagnosticOverlayMutedText");

        ThemeColorScheme roundTrip = ThemeColors::getClassicDark();
        const bool imported = ThemeColors::importFromJSON(json, roundTrip);
        CHECK(imported, "JSON import succeeds");
        const std::filesystem::path savedPath =
            std::filesystem::temp_directory_path() / std::filesystem::u8path(u8"harken-theme-导出-test.json");
        CHECK(ThemeColors::saveThemeToFile(savedPath.u8string(), scheme),
              "JSON theme export saves a UTF-8 path");
        ThemeColorScheme reloaded = ThemeColors::getClassicDark();
        CHECK(ThemeColors::loadThemeFromFile(savedPath.u8string(), reloaded),
              "saved JSON theme reloads");
        CHECK(approxEqual(reloaded.menuText, scheme.menuText),
              "saved JSON keeps the edited theme color");
        std::error_code removeError;
        std::filesystem::remove(savedPath, removeError);
        CHECK(approxEqual(roundTrip.menuText, scheme.menuText), "menuText survives round-trip");
        CHECK(approxEqual(roundTrip.frameBgBase, scheme.frameBgBase), "frameBgBase survives round-trip");
        CHECK(approxEqual(roundTrip.chromeBorder, scheme.chromeBorder), "chromeBorder survives round-trip");
        CHECK(approxEqual(roundTrip.popupBG, scheme.popupBG), "popupBG survives round-trip");
        CHECK(approxEqual(roundTrip.menuPopupBG, scheme.menuPopupBG), "menuPopupBG survives round-trip");
        CHECK(approxEqual(roundTrip.dialogBG, scheme.dialogBG), "dialogBG survives round-trip");
        CHECK(approxEqual(roundTrip.dialogPanelBG, scheme.dialogPanelBG), "dialogPanelBG survives round-trip");
        CHECK(approxEqual(roundTrip.historyCapsuleBG, scheme.historyCapsuleBG), "historyCapsuleBG survives round-trip");
        CHECK(approxEqual(roundTrip.historyCapsuleActiveBG, scheme.historyCapsuleActiveBG), "historyCapsuleActiveBG survives round-trip");
        CHECK(approxEqual(roundTrip.modeCapsuleBG, scheme.modeCapsuleBG), "modeCapsuleBG survives round-trip");
        CHECK(approxEqual(roundTrip.modeCapsuleActiveBG, scheme.modeCapsuleActiveBG), "modeCapsuleActiveBG survives round-trip");
        CHECK(approxEqual(roundTrip.waveformPanelBorder, scheme.waveformPanelBorder), "waveformPanelBorder survives round-trip");
        CHECK(approxEqual(roundTrip.waveformSplitterBG, scheme.waveformSplitterBG), "waveformSplitterBG survives round-trip");
        CHECK(approxEqual(roundTrip.waveformSplitterLine, scheme.waveformSplitterLine), "waveformSplitterLine survives round-trip");
        CHECK(approxEqual(roundTrip.waveformFooterBG, scheme.waveformFooterBG), "waveformFooterBG survives round-trip");
        CHECK(approxEqual(roundTrip.waveformSelectionDim, scheme.waveformSelectionDim), "waveformSelectionDim survives round-trip");
        CHECK(approxEqual(roundTrip.waveformSelectionHearthTop, scheme.waveformSelectionHearthTop), "waveformSelectionHearthTop survives round-trip");
        CHECK(approxEqual(roundTrip.waveformSelectionHearthBot, scheme.waveformSelectionHearthBot), "waveformSelectionHearthBot survives round-trip");
        CHECK(approxEqual(roundTrip.waveformSelectionEdge, scheme.waveformSelectionEdge), "waveformSelectionEdge survives round-trip");
        CHECK(approxEqual(roundTrip.timecodeText, scheme.timecodeText), "timecodeText survives round-trip");
        CHECK(approxEqual(roundTrip.timecodeTextHover, scheme.timecodeTextHover), "timecodeTextHover survives round-trip");
        CHECK(approxEqual(roundTrip.resultsTabBG, scheme.resultsTabBG), "resultsTabBG survives round-trip");
        CHECK(approxEqual(roundTrip.resultsTabHover, scheme.resultsTabHover), "resultsTabHover survives round-trip");
        CHECK(approxEqual(roundTrip.resultsTabActive, scheme.resultsTabActive), "resultsTabActive survives round-trip");
        CHECK(approxEqual(roundTrip.resultsSecondaryText, scheme.resultsSecondaryText), "resultsSecondaryText survives round-trip");
        CHECK(approxEqual(roundTrip.resultsGradientStart, scheme.resultsGradientStart), "resultsGradientStart survives round-trip");
        CHECK(approxEqual(roundTrip.resultsGradientEnd, scheme.resultsGradientEnd), "resultsGradientEnd survives round-trip");
        CHECK(approxEqual(roundTrip.filterBrowserSectionBG, scheme.filterBrowserSectionBG), "filterBrowserSectionBG survives round-trip");
        CHECK(approxEqual(roundTrip.filterBrowserButtonBG, scheme.filterBrowserButtonBG), "filterBrowserButtonBG survives round-trip");
        CHECK(approxEqual(roundTrip.filterBrowserTreeText, scheme.filterBrowserTreeText), "filterBrowserTreeText survives round-trip");
        CHECK(approxEqual(roundTrip.filterBrowserTreeSelection, scheme.filterBrowserTreeSelection), "filterBrowserTreeSelection survives round-trip");
        CHECK(approxEqual(roundTrip.filterBrowserSearchHitText, scheme.filterBrowserSearchHitText), "filterBrowserSearchHitText survives round-trip");
        CHECK(approxEqual(roundTrip.filterBrowserLocateTint, scheme.filterBrowserLocateTint), "filterBrowserLocateTint survives round-trip");
        CHECK(approxEqual(roundTrip.waveformSecondaryColor, scheme.waveformSecondaryColor), "waveformSecondaryColor survives round-trip");
        CHECK(approxEqual(roundTrip.searchButtonBG, scheme.searchButtonBG), "searchButtonBG survives round-trip");
        CHECK(approxEqual(roundTrip.toolbarAccent, scheme.toolbarAccent), "toolbarAccent survives round-trip");
        CHECK(approxEqual(roundTrip.controlAccent, scheme.controlAccent), "controlAccent survives round-trip");
        CHECK(approxEqual(roundTrip.playlistGradientHighlight, scheme.playlistGradientHighlight), "playlistGradientHighlight survives round-trip");
        CHECK(approxEqual(roundTrip.playlistGradientShadow, scheme.playlistGradientShadow), "playlistGradientShadow survives round-trip");
        CHECK(approxEqual(roundTrip.coverWallBG, scheme.coverWallBG), "coverWallBG survives round-trip");
        CHECK(approxEqual(roundTrip.meterBg, scheme.meterBg), "meterBg survives round-trip");
        CHECK(approxEqual(roundTrip.meterBase, scheme.meterBase), "meterBase survives round-trip");
        CHECK(approxEqual(roundTrip.meterMid, scheme.meterMid), "meterMid survives round-trip");
        CHECK(approxEqual(roundTrip.meterWarning, scheme.meterWarning), "meterWarning survives round-trip");
        CHECK(approxEqual(roundTrip.meterClip, scheme.meterClip), "meterClip survives round-trip");
        CHECK(approxEqual(roundTrip.meterScaleText, scheme.meterScaleText), "meterScaleText survives round-trip");
        CHECK(approxEqual(roundTrip.diagnosticOverlayBG, scheme.diagnosticOverlayBG), "diagnosticOverlayBG survives round-trip");
        CHECK(approxEqual(roundTrip.diagnosticOverlayText, scheme.diagnosticOverlayText), "diagnosticOverlayText survives round-trip");
        CHECK(approxEqual(roundTrip.diagnosticOverlayAccent, scheme.diagnosticOverlayAccent), "diagnosticOverlayAccent survives round-trip");
        CHECK(approxEqual(roundTrip.diagnosticOverlayMutedText, scheme.diagnosticOverlayMutedText), "diagnosticOverlayMutedText survives round-trip");
    }

    {
        ThemeColorScheme fallback = ThemeColors::getClassicDark();
        fallback.meterBg = ImVec4(0.01f, 0.01f, 0.01f, 0.0f);
        fallback.meterBase = ImVec4(0.01f, 0.01f, 0.01f, 0.0f);
        fallback.meterMid = ImVec4(0.01f, 0.01f, 0.01f, 0.0f);
        fallback.meterWarning = ImVec4(0.01f, 0.01f, 0.01f, 0.0f);
        fallback.meterClip = ImVec4(0.01f, 0.01f, 0.01f, 0.0f);
        fallback.meterScaleText = ImVec4(0.01f, 0.01f, 0.01f, 0.0f);

        const std::string legacyJson = R"({
  "waveformBG": "ff2a3038",
  "toolbarBG": "ff353b45",
  "resultsSecondaryText": "ff98a2af",
  "volumeColor": "ff4fd66b",
  "volumeClipColor": "ffef5d5d"
})";

        const bool imported = ThemeColors::importFromJSON(legacyJson, fallback);
        CHECK(imported, "legacy JSON import succeeds when meter tokens are absent");
        CHECK(fallback.meterBg.w > 0.99f, "legacy JSON derives opaque meterBg");
        CHECK(fallback.meterBase.w > 0.99f, "legacy JSON derives opaque meterBase");
        CHECK(fallback.meterMid.w > 0.99f, "legacy JSON derives opaque meterMid");
        CHECK(fallback.meterWarning.w > 0.99f, "legacy JSON derives opaque meterWarning");
        CHECK(fallback.meterClip.w > 0.99f, "legacy JSON derives opaque meterClip");
        CHECK(fallback.meterScaleText.w > 0.99f, "legacy JSON derives opaque meterScaleText");
        CHECK(fallback.diagnosticOverlayBG.w > 0.90f, "legacy JSON derives diagnostic overlay background");
        CHECK(fallback.diagnosticOverlayText.w > 0.99f, "legacy JSON derives diagnostic overlay text");
        CHECK(!approxEqual(fallback.meterBase, fallback.volumeColor), "legacy meterBase is no longer identical to legacy volumeColor");
        CHECK(!approxEqual(fallback.meterClip, fallback.volumeClipColor), "legacy meterClip is no longer identical to legacy volumeClipColor");
    }

    {
        ThemeColorScheme fallback = ThemeColors::getClassicDark();
        const std::string legacyDarkJson = R"({
  "waveformBG": "ff0a1018",
  "resultsBG": "ff11161d",
  "resultsOutline": "ff050607",
  "resultsSelection": "ff4a88d8",
  "toolbarBG": "ff1a2028"
})";

        const bool imported = ThemeColors::importFromJSON(legacyDarkJson, fallback);
        CHECK(imported, "legacy dark JSON import succeeds when waveform seam tokens are absent");
        CHECK(fallback.waveformPanelBorder.w > 0.99f, "legacy dark JSON derives opaque waveformPanelBorder");
        CHECK(fallback.waveformSplitterLine.w > 0.99f, "legacy dark JSON derives opaque waveformSplitterLine");
        CHECK(luminance(fallback.waveformPanelBorder) > luminance(fallback.resultsOutline) + 0.04f,
              "legacy dark JSON lifts waveformPanelBorder above near-black outline");
        CHECK(luminance(fallback.waveformSplitterLine) > luminance(fallback.resultsOutline) + 0.03f,
              "legacy dark JSON lifts waveformSplitterLine above near-black outline");
        const ImVec4 expectedSearchHit = ImVec4(
            fallback.controlAccent.x * 0.75f + fallback.filterBrowserTreeText.x * 0.25f,
            fallback.controlAccent.y * 0.75f + fallback.filterBrowserTreeText.y * 0.25f,
            fallback.controlAccent.z * 0.75f + fallback.filterBrowserTreeText.z * 0.25f,
            fallback.controlAccent.w * 0.75f + fallback.filterBrowserTreeText.w * 0.25f);
        CHECK(approxEqual(fallback.filterBrowserSearchHitText, expectedSearchHit),
              "legacy dark JSON derives folder search hit text from accent and tree text");
    }

    {
        ThemeColorScheme scheme = ThemeColors::getElectricBlue();
        scheme.metadataPanelLabel = ImVec4(0.85f, 0.10f, 0.10f, 1.0f);
        scheme.waveformSecondaryColor = ImVec4(0.10f, 0.85f, 0.10f, 1.0f);
        scheme.waveformPositionLine = ImVec4(0.10f, 0.10f, 0.85f, 1.0f);
        scheme.frameBgBase = ImVec4(0.18f, 0.20f, 0.24f, 1.0f);
        scheme.chromeBorder = ImVec4(0.42f, 0.38f, 0.34f, 1.0f);
        scheme.popupBG = ImVec4(0.22f, 0.24f, 0.28f, 0.96f);
        scheme.toolbarBG = ImVec4(0.80f, 0.70f, 0.60f, 1.0f);
        scheme.controlAccent = ImVec4(0.36f, 0.55f, 0.88f, 1.0f);
        scheme.resultsScrollbar = ImVec4(0.72f, 0.31f, 0.22f, 1.0f);

        HarkenSearchThemeManager& mgr = HarkenSearchThemeManager::instance();
        mgr.initialize(scheme);
        const WaveformStyle& ws = mgr.waveformStyle();
        ImGui::CreateContext();
        mgr.applyImGuiStyle();
        const ImGuiStyle& styleProbe = ImGui::GetStyle();

        CHECK(ws.secondaryColor == HarkenSearchThemeManager::toImU32(scheme.waveformSecondaryColor),
              "waveform secondary uses waveformSecondaryColor");
        CHECK(ws.secondaryColor != HarkenSearchThemeManager::toImU32(scheme.metadataPanelLabel),
              "waveform secondary no longer reuses metadataPanelLabel");
        CHECK(ws.rmsColor == HarkenSearchThemeManager::withAlpha(HarkenSearchThemeManager::toImU32(scheme.waveformSecondaryColor), 180),
              "waveform RMS uses waveformSecondaryColor with fixed RMS alpha");
        CHECK(ws.playheadColor == HarkenSearchThemeManager::toImU32(scheme.waveformPositionLine),
              "waveform playhead uses waveformPositionLine");
        CHECK(approxEqual(styleProbe.Colors[ImGuiCol_PopupBg], scheme.popupBG),
              "theme manager popup background uses popupBG");
        CHECK(!approxEqual(styleProbe.Colors[ImGuiCol_PopupBg], scheme.toolbarBG),
              "theme manager popup background no longer reuses toolbarBG");
        CHECK(approxEqual(styleProbe.Colors[ImGuiCol_FrameBg].x, scheme.frameBgBase.x, 0.07f),
              "theme manager frame background follows frameBgBase");
        CHECK(approxEqual(styleProbe.Colors[ImGuiCol_Border], scheme.chromeBorder),
              "theme manager border follows chromeBorder");
        CHECK(approxEqual(styleProbe.Colors[ImGuiCol_ButtonActive], scheme.controlAccent),
              "theme manager button active follows controlAccent");
        CHECK(approxEqual(styleProbe.Colors[ImGuiCol_ScrollbarGrab],
                          ImVec4(scheme.resultsScrollbar.x, scheme.resultsScrollbar.y, scheme.resultsScrollbar.z, 0.40f)),
              "theme manager scrollbar grab follows resultsScrollbar");
        CHECK(approxEqual(styleProbe.Colors[ImGuiCol_ScrollbarGrabActive],
                          ImVec4(scheme.controlAccent.x, scheme.controlAccent.y, scheme.controlAccent.z, 0.80f)),
              "theme manager active scrollbar follows controlAccent");
        CHECK(approxEqual(styleProbe.PopupRounding, 10.0f),
              "theme manager uses unified popup rounding baseline");
        CHECK(approxEqual(styleProbe.ScrollbarSize, 12.0f),
              "theme manager uses unified scrollbar size baseline");
        CHECK(approxEqual(styleProbe.WindowBorderHoverPadding, 4.0f),
              "theme manager keeps positive WindowBorderHoverPadding baseline");
        ImGui::DestroyContext();
    }

    {
        ThemeColorScheme scheme = ThemeColors::getClassicDark();
        scheme.filterBrowserBG = ImVec4(21.0f / 255.0f, 31.0f / 255.0f, 47.0f / 255.0f, 1.0f);
        scheme.filterBrowserHeading = ImVec4(250.0f / 255.0f, 214.0f / 255.0f, 126.0f / 255.0f, 1.0f);
        scheme.filterBrowserOutline = ImVec4(73.0f / 255.0f, 96.0f / 255.0f, 131.0f / 255.0f, 1.0f);
        scheme.filterBrowserScrollbar = ImVec4(98.0f / 255.0f, 120.0f / 255.0f, 150.0f / 255.0f, 1.0f);

        scheme.metadataPanelBG = ImVec4(0.90f, 0.10f, 0.10f, 1.0f);
        scheme.metadataPanelLabel = ImVec4(0.10f, 0.10f, 0.90f, 1.0f);
        scheme.metadataPanelScrollbar = ImVec4(0.80f, 0.20f, 0.80f, 1.0f);
        scheme.transferHistoryBG = ImVec4(0.85f, 0.25f, 0.25f, 1.0f);

        CHECK(approxEqual(ThemeColors::peerPanelBG(scheme), scheme.filterBrowserBG),
              "peer panel background follows filterBrowserBG");
        CHECK(approxEqual(ThemeColors::peerPanelHeading(scheme), scheme.filterBrowserHeading),
              "peer panel heading follows filterBrowserHeading");
        CHECK(approxEqual(ThemeColors::peerPanelOutline(scheme), scheme.filterBrowserOutline),
              "peer panel outline follows filterBrowserOutline");
        CHECK(approxEqual(ThemeColors::peerPanelScrollbar(scheme), scheme.filterBrowserScrollbar),
              "peer panel scrollbar follows filterBrowserScrollbar");
        CHECK(!approxEqual(ThemeColors::peerPanelBG(scheme), scheme.metadataPanelBG),
              "peer panel background no longer follows metadataPanelBG");
        CHECK(!approxEqual(ThemeColors::peerPanelBG(scheme), scheme.transferHistoryBG),
              "peer panel background no longer follows transferHistoryBG");
    }

    {
        ImGui::CreateContext();

        ThemeColorScheme scheme = ThemeColors::getClassicDark();
        scheme.toolbarBG = ImVec4(0.12f, 0.18f, 0.24f, 1.0f);
        scheme.frameBgBase = ImVec4(0.21f, 0.24f, 0.28f, 1.0f);
        scheme.searchButtonBG = ImVec4(0.94f, 0.18f, 0.12f, 1.0f);
        scheme.toolbarAccent = ImVec4(0.82f, 0.64f, 0.90f, 1.0f);
        scheme.controlAccent = ImVec4(0.48f, 0.62f, 0.93f, 1.0f);
        scheme.waveformSecondaryColor = ImVec4(0.25f, 0.75f, 0.45f, 1.0f);
        ThemeColors::current() = scheme;
        ThemeColors::applyToImGui();

        const ImVec4 hovered = ImGui::GetStyle().Colors[ImGuiCol_ButtonHovered];
        CHECK(!approxEqual(hovered, scheme.searchButtonBG),
              "searchButtonBG no longer drives global ButtonHovered");
        CHECK(approxEqual(COLOR_ACCENT, scheme.toolbarAccent),
              "legacy accent now follows toolbarAccent");
        CHECK(approxEqual(COLOR_SELECTED, scheme.resultsSelection),
              "legacy selected still follows resultsSelection");
        CHECK(g_waveformRmsColor == IM_COL32(64, 191, 115, 180),
              "waveform RMS now follows waveformSecondaryColor");

        ImGui::DestroyContext();
    }

    {
        ImGui::CreateContext();

        ThemeColorScheme scheme = ThemeColors::getPowderedPastels();
        scheme.mainWindowBG = ImVec4(253.0f / 255.0f, 251.0f / 255.0f, 247.0f / 255.0f, 1.0f);
        scheme.dialogBG = ImVec4(253.0f / 255.0f, 247.0f / 255.0f, 239.0f / 255.0f, 1.0f);
        scheme.chromeBorder = ImVec4(216.0f / 255.0f, 226.0f / 255.0f, 234.0f / 255.0f, 0.40f);
        ThemeColors::current() = scheme;
        ThemeColors::applyToImGui();

        const ImVec4 modalDim = ImGui::GetStyle().Colors[ImGuiCol_ModalWindowDimBg];
        const ImVec4 navDim = ImGui::GetStyle().Colors[ImGuiCol_NavWindowingDimBg];
        CHECK(luminance(modalDim) > 0.45f, "light custom themes keep modal dim scrim away from near-black");
        CHECK(modalDim.w < 0.30f, "light custom themes use a softer modal dim alpha");
        CHECK(navDim.w < modalDim.w, "nav dim remains softer than modal dim");
        CHECK(ThemeColors::isLightChromeTheme(scheme),
              "light custom scheme is detected as light chrome theme");

        ImGui::DestroyContext();
    }

    {
        ThemeColorScheme darkScheme = ThemeColors::getClassicDark();
        CHECK(!ThemeColors::isLightChromeTheme(darkScheme),
              "classic dark scheme is not detected as light chrome theme");

        ThemeColorScheme milkyScheme = ThemeColors::getClassicDark();
        milkyScheme.mainWindowBG = ImVec4(0.99f, 0.98f, 0.97f, 1.0f);
        milkyScheme.resultsBG = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
        milkyScheme.dialogBG = ImVec4(0.99f, 0.97f, 0.94f, 1.0f);
        milkyScheme.waveformBG = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
        CHECK(ThemeColors::isLightChromeTheme(milkyScheme),
              "milky custom scheme is detected as light chrome theme even from dark baseline");
    }

    {
        ThemeColorScheme milkyScheme = ThemeColors::getPowderedPastels();
        milkyScheme.waveformBG = ImVec4(0.97f, 0.95f, 0.92f, 1.0f);
        milkyScheme.waveformPanelBorder = ImVec4(0.82f, 0.78f, 0.74f, 1.0f);
        milkyScheme.chromeBorder = ImVec4(0.74f, 0.69f, 0.66f, 1.0f);

        const ImVec4 expectedLightBorder = withAlpha(
            mixColor(
                milkyScheme.waveformBG,
                mixColor(milkyScheme.waveformPanelBorder, milkyScheme.chromeBorder, 0.30f),
                0.16f),
            0.24f);

        CHECK(approxEqual(ThemeColors::waveformFrameBorder(milkyScheme), expectedLightBorder),
              "light waveform frame border stays near theme tokens instead of a dark edge");
    }

    {
        ThemeColorScheme original = ThemeColors::getClassicDark();
        ThemeColorScheme custom = ThemeColors::getPowderedPastels();
        ThemeColors::current() = original;
        ThemeColors::customized() = false;

        ThemeColors::activateCustomScheme(custom);
        CHECK(ThemeColors::customized(), "activateCustomScheme marks theme as customized");
        CHECK(approxEqual(ThemeColors::current().mainWindowBG, custom.mainWindowBG),
              "activateCustomScheme replaces current theme scheme");

        ThemeColors::deactivateCustomTheme();
        CHECK(!ThemeColors::customized(), "deactivateCustomTheme clears customized flag");
        CHECK(approxEqual(ThemeColors::current().mainWindowBG, custom.mainWindowBG),
              "deactivateCustomTheme does not mutate the last live scheme");
    }

    {
        CHECK(!ThemeColors::shouldApplyCurrentScheme(false, false),
              "stock theme without editor does not reapply current scheme");
        CHECK(ThemeColors::shouldApplyCurrentScheme(true, false),
              "theme editor preview reapplies current scheme");

        ThemeColors::activateCustomScheme(ThemeColors::getClassicDark());
        CHECK(ThemeColors::shouldApplyCurrentScheme(false, true),
              "custom theme selection reapplies current scheme");
        ThemeColors::deactivateCustomTheme();
    }

    {
        ImGui::CreateContext();
        ImGuiStyle& style = ImGui::GetStyle();
        style.PopupRounding = 1.0f;
        style.ScrollbarSize = 1.0f;
        style.WindowBorderHoverPadding = 0.0f;
        style.WindowMinSize = ImVec2(0.0f, 0.0f);

        ThemeColors::applyStyleBaseline(style);

        CHECK(approxEqual(style.PopupRounding, 10.0f),
              "style baseline uses unified popup rounding");
        CHECK(approxEqual(style.ScrollbarSize, 12.0f),
              "style baseline uses unified scrollbar size");
        CHECK(approxEqual(style.FramePadding.x, 8.0f) && approxEqual(style.FramePadding.y, 5.0f),
              "style baseline uses compact modern frame padding");
        CHECK(approxEqual(style.ItemSpacing.x, 8.0f) && approxEqual(style.ItemSpacing.y, 6.0f),
              "style baseline uses compact modern item spacing");
        CHECK(approxEqual(style.CellPadding.x, 8.0f) && approxEqual(style.CellPadding.y, 4.0f),
              "style baseline keeps table cells readable without bloating dense lists");
        CHECK(approxEqual(style.GrabMinSize, 12.0f),
              "style baseline uses modern but compact grab min size");
        CHECK(approxEqual(style.WindowBorderHoverPadding, 4.0f),
              "style baseline keeps positive WindowBorderHoverPadding");
        CHECK(approxEqual(style.WindowMinSize.x, 32.0f) && approxEqual(style.WindowMinSize.y, 32.0f),
              "style baseline resets WindowMinSize before scaling");

        ImGui::DestroyContext();
    }

    {
        ImGui::CreateContext();

        ThemeColorScheme darkScheme = ThemeColors::getClassicDark();
        darkScheme.mainWindowBG = ImVec4(0.11f, 0.14f, 0.19f, 1.0f);
        darkScheme.popupBG = ImVec4(0.15f, 0.17f, 0.24f, 1.0f);
        darkScheme.frameBgBase = ImVec4(0.12f, 0.14f, 0.20f, 1.0f);
        darkScheme.controlAccent = ImVec4(0.34f, 0.54f, 0.77f, 1.0f);
        darkScheme.chromeBorder = ImVec4(0.22f, 0.25f, 0.33f, 1.0f);
        ThemeColors::activateCustomScheme(darkScheme);
        ThemeColors::applyToImGui();
        const ImGuiStyle darkStyle = ImGui::GetStyle();

        ThemeColorScheme lightScheme = ThemeColors::getPowderedPastels();
        lightScheme.mainWindowBG = ImVec4(0.97f, 0.95f, 0.92f, 1.0f);
        lightScheme.popupBG = ImVec4(0.99f, 0.96f, 0.92f, 1.0f);
        lightScheme.controlAccent = ImVec4(0.20f, 0.42f, 0.38f, 1.0f);
        ThemeColors::activateCustomScheme(lightScheme);
        ThemeColors::applyToImGui();
        const ImGuiStyle lightStyle = ImGui::GetStyle();

        CHECK(!approxEqual(darkStyle.Colors[ImGuiCol_WindowBg], lightStyle.Colors[ImGuiCol_WindowBg]),
              "theme application maps WindowBg from the active scheme");
        CHECK(!approxEqual(darkStyle.Colors[ImGuiCol_PopupBg], lightStyle.Colors[ImGuiCol_PopupBg]),
              "theme application maps PopupBg from the active scheme");
        CHECK(!approxEqual(darkStyle.Colors[ImGuiCol_ButtonActive], lightStyle.Colors[ImGuiCol_ButtonActive]),
              "theme application maps active controls from the active scheme");
        CHECK(approxEqual(darkStyle.FramePadding.x, lightStyle.FramePadding.x) &&
              approxEqual(darkStyle.FramePadding.y, lightStyle.FramePadding.y),
              "theme application keeps style metrics independent from palette choice");

        ThemeColors::deactivateCustomTheme();
        ImGui::DestroyContext();
    }

    {
        ImGui::CreateContext();
        ImGuiStyle& style = ImGui::GetStyle();
        style.Colors[ImGuiCol_BorderShadow] = ImVec4(0.02f, 0.03f, 0.04f, 0.38f);

        ThemeColors::activateCustomScheme(ThemeColors::getPowderedPastels());
        ThemeColors::applyToImGui();

        CHECK(approxEqual(style.Colors[ImGuiCol_BorderShadow], ImVec4(0.0f, 0.0f, 0.0f, 0.0f)),
              "custom theme application clears inherited border shadow");

        ThemeColors::deactivateCustomTheme();
        ImGui::DestroyContext();
    }

    {
        ThemeColorScheme scheme = ThemeColors::getClassicDark();
        const ImVec4 customBg(0.18f, 0.20f, 0.24f, 1.0f);
        const ImVec4 customText(0.82f, 0.84f, 0.88f, 1.0f);
        const ImVec4 customAccent(0.42f, 0.62f, 0.94f, 1.0f);
        const ImVec4 customWave(0.30f, 0.76f, 0.62f, 1.0f);
        const ImVec4 customVolume(0.24f, 0.86f, 0.34f, 1.0f);

        ThemeColors::applySimpleCustomOverrides(
            scheme, customBg, customText, customAccent, customWave, customVolume);

        CHECK(approxEqual(scheme.mainWindowBG, customBg),
              "simple custom overrides sync mainWindowBG");
        CHECK(approxEqual(scheme.resultsBG, ImVec4(0.21f, 0.23f, 0.27f, 1.0f), 0.001f),
              "simple custom overrides derive child backgrounds");
        CHECK(approxEqual(scheme.interfaceText, customText),
              "simple custom overrides sync interfaceText");
        CHECK(approxEqual(scheme.resultsSelection, customAccent),
              "simple custom overrides sync resultsSelection");
        CHECK(approxEqual(scheme.waveformColor, customWave),
              "simple custom overrides sync waveformColor");
        CHECK(approxEqual(scheme.volumeColor, customVolume),
              "simple custom overrides sync volumeColor");
    }

    {
        namespace fs = std::filesystem;
        ThemeColorScheme scheme = ThemeColors::getPowderedPastels();
        scheme.mainWindowBG = ImVec4(0.91f, 0.88f, 0.84f, 1.0f);
        scheme.popupBG = ImVec4(0.95f, 0.93f, 0.90f, 0.98f);
        const fs::path tempPath = fs::temp_directory_path() / "aura_theme_contract_test.json";
        {
            std::ofstream ofs(tempPath, std::ios::out | std::ios::trunc);
            ofs << ThemeColors::exportToJSON(scheme);
        }

        ImGui::CreateContext();
        ImGuiStyle& style = ImGui::GetStyle();
        const ImVec4 sentinelWindowBg(0.17f, 0.13f, 0.11f, 1.0f);
        style.Colors[ImGuiCol_WindowBg] = sentinelWindowBg;
        ThemeColors::current() = ThemeColors::getClassicDark();
        ThemeColors::customized() = false;

        ThemeColorScheme loaded;
        const bool loadedDraftOnly = ThemeColors::loadAndApplyThemeFile(
            tempPath.string(),
            &loaded,
            false,
            "UnitTestThemeLoad",
            (int)AppTheme::Dark,
            "Dark",
            false);
        CHECK(loadedDraftOnly, "loadAndApplyThemeFile loads JSON without applying when applyNow is false");
        CHECK(ThemeColors::customized(), "loadAndApplyThemeFile marks scheme customized after load");
        CHECK(approxEqual(loaded.mainWindowBG, scheme.mainWindowBG),
              "loadAndApplyThemeFile returns loaded scheme snapshot");
        CHECK(approxEqual(ThemeColors::current().popupBG, loaded.popupBG),
              "loadAndApplyThemeFile updates current scheme even without immediate apply");
        CHECK(approxEqual(style.Colors[ImGuiCol_WindowBg], sentinelWindowBg),
              "loadAndApplyThemeFile preserves existing ImGui colors when applyNow is false");

        const bool loadedAndApplied = ThemeColors::loadAndApplyThemeFile(tempPath.string());
        CHECK(loadedAndApplied, "loadAndApplyThemeFile applies scheme when applyNow uses default true");
        CHECK(approxEqual(style.Colors[ImGuiCol_WindowBg], scheme.mainWindowBG),
              "loadAndApplyThemeFile updates ImGui window background on apply");
        CHECK(approxEqual(style.Colors[ImGuiCol_PopupBg], ThemeColors::current().popupBG),
              "loadAndApplyThemeFile updates ImGui popup background on apply");

        ThemeColors::deactivateCustomTheme();
        ImGui::DestroyContext();
        std::error_code ec;
        fs::remove(tempPath, ec);
    }

    {
        // 10-D R-ruling: getEditableProperties is a faithful schema projection.
        ThemeColorScheme& cur = ThemeColors::current();
        size_t schemaCount = 0;
        const ThemeEditorItem* schema = GetThemeEditorItems(schemaCount);
        const auto props = ThemeColors::getEditableProperties();
        CHECK(schemaCount == 96, "10-D: schema keeps all 96 color rows");
        CHECK(props.size() == 88, "10-D: projection keeps the historic 88-row surface");
        bool matches = props.size() == 88;
        for (const auto& p : props) {
            const ThemeEditorItem* item = findEditorItem(p.name);
            if (!item || item->labelId != p.labelId ||
                &(cur.*(item->member)) != p.colorPtr) {
                matches = false;
                break;
            }
        }
        CHECK(matches, "10-D: projection rows match schema (key/labelId/live member ptr)");
    }

    std::cout << "\nPassed: " << g_passed << "\n";
    if (g_failed > 0) {
        std::cerr << "Failed: " << g_failed << "\n";
        return 1;
    }
    std::cout << "All theme color contract tests passed.\n";
    return 0;
}
