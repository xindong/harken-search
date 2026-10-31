// SoundManager - 主题模块
// Theme.cpp - Aris theme and color definitions + granular color system
// Aris-owned preset and custom color definitions

#include "Theme.h"
#include "ui/MistBlueChrome.h"
#include "ThemeEditorSchema.h"
#include <cstring>
#include "HarkenSearchDesignTokens.generated.h"
#include "platform/filesystem.h"
#include "platform/system.h"
#include "BuildConfig.h"
#include "DiagnosticsLogger.h"
#include "Utils.h"
#include "ui/CoreWorkspaceStyle.h"
#include <algorithm>
#include <sstream>
#include <iomanip>
#include <cstdlib>
#include <regex>

// Color Definitions (Legacy)
// 注意: 这些全局颜色变量已在 main.cpp 中以 static 方式定义,
// Theme.cpp 中不再重复定义。ThemeColors::applyToImGui() 中会同步镜像值。

// ThemeColors - Current Scheme (singleton)

static ThemeColorScheme s_currentScheme;
static bool s_customized = false;
struct ThemeImportDiagState {
    bool armed = false;
    std::string source;
    std::string pathUtf8;
    int startThemeValue = -1;
    std::string startThemeLabel;
    bool livePreview = false;
    ImVec4 preWindowBg = ImVec4(0, 0, 0, 0);
    ImVec4 preChildBg = ImVec4(0, 0, 0, 0);
    ImVec4 prePopupBg = ImVec4(0, 0, 0, 0);
    ImVec4 preModalDimBg = ImVec4(0, 0, 0, 0);
    bool preCustomized = false;
};
static ThemeImportDiagState s_themeImportDiag;
static bool s_themeRenderDiagPending = false;
static std::string s_themeRenderDiagSource;
static std::string s_themeRenderDiagStartTheme;

static void AppendThemeTraceLine(const std::string& line) {
#if AURA_INTERNAL_BETA
    // 内测版：不生成游离的主题路径跟踪日志
    (void)line;
    return;
#else
    try {
        Platform::FileSystem::AppendFileText(
            Platform::GetExeDir() + "\\theme_path_trace.log", line + '\n');
    } catch (...) {
    }
#endif
}

static ImVec4 MixThemeColor(const ImVec4& a, const ImVec4& b, float t) {
    return ImVec4(
        a.x + (b.x - a.x) * t,
        a.y + (b.y - a.y) * t,
        a.z + (b.z - a.z) * t,
        a.w + (b.w - a.w) * t);
}

static ImVec4 ClampThemeColor(ImVec4 c) {
    c.x = std::clamp(c.x, 0.0f, 1.0f);
    c.y = std::clamp(c.y, 0.0f, 1.0f);
    c.z = std::clamp(c.z, 0.0f, 1.0f);
    c.w = std::clamp(c.w, 0.0f, 1.0f);
    return c;
}

static ImVec4 OpaqueThemeColor(const ImVec4& c) {
    ImVec4 out = ClampThemeColor(c);
    out.w = 1.0f;
    return out;
}

static ImVec4 ThemeGray(float v) {
    return ImVec4(v, v, v, 1.0f);
}

static ImVec4 ToThemeColor(const aris::design::Rgba& token) {
    return ImVec4(token.r, token.g, token.b, token.a);
}

static ImVec4 DeriveMeterBg(const ThemeColorScheme& s) {
    ImVec4 source = s.waveformBG.w > 0.0f ? s.waveformBG : s.toolbarBG;
    return OpaqueThemeColor(MixThemeColor(source, s.chromeBorder, 0.16f));
}

static ImVec4 DeriveMeterBase(const ThemeColorScheme& s, const ImVec4& meterBg) {
    ImVec4 quietNeutral = MixThemeColor(meterBg, s.resultsSecondaryText, 0.30f);
    ImVec4 softened = MixThemeColor(s.volumeColor, quietNeutral, 0.58f);
    softened = MixThemeColor(softened, ThemeGray(0.62f), 0.14f);
    return OpaqueThemeColor(softened);
}

static ImVec4 DeriveMeterWarning(const ThemeColorScheme& s, const ImVec4& meterBase) {
    ImVec4 warmTarget = MixThemeColor(s.waveformPositionLine, ImVec4(0.96f, 0.88f, 0.72f, 1.0f), 0.55f);
    return OpaqueThemeColor(MixThemeColor(meterBase, warmTarget, 0.62f));
}

static ImVec4 DeriveMeterMid(const ImVec4& meterBase, const ImVec4& meterWarning) {
    return OpaqueThemeColor(MixThemeColor(meterBase, meterWarning, 0.46f));
}

static ImVec4 DeriveMeterClip(const ThemeColorScheme& s) {
    ImVec4 coralTarget = MixThemeColor(s.volumeClipColor, ImVec4(0.84f, 0.67f, 0.64f, 1.0f), 0.58f);
    coralTarget = MixThemeColor(coralTarget, ThemeGray(0.70f), 0.10f);
    return OpaqueThemeColor(coralTarget);
}

static ImVec4 DeriveMeterScaleText(const ThemeColorScheme& s) {
    ImVec4 base = s.resultsSecondaryText.w > 0.0f
        ? s.resultsSecondaryText
        : (s.timecodeText.w > 0.0f ? s.timecodeText : s.interfaceText);
    return OpaqueThemeColor(MixThemeColor(base, s.interfaceText, 0.22f));
}

static void ApplyMeterThemeDefaults(ThemeColorScheme& s) {
    s.meterBg = DeriveMeterBg(s);
    s.meterBase = DeriveMeterBase(s, s.meterBg);
    s.meterWarning = DeriveMeterWarning(s, s.meterBase);
    s.meterMid = DeriveMeterMid(s.meterBase, s.meterWarning);
    s.meterClip = DeriveMeterClip(s);
    s.meterScaleText = DeriveMeterScaleText(s);
}

static void ApplySemanticThemeDefaults(ThemeColorScheme& s) {
    s.frameBgBase = s.toolbarBG;
    s.chromeBorder = s.resultsOutline;
    s.popupBG = ImVec4(s.toolbarBG.x, s.toolbarBG.y, s.toolbarBG.z, 0.98f);
    s.menuPopupBG = s.popupBG;
    s.dialogBG = MixThemeColor(s.popupBG, s.mainWindowBG, 0.08f);
    s.dialogPanelBG = MixThemeColor(s.dialogBG, s.frameBgBase, 0.16f);
    s.historyCapsuleBG = MixThemeColor(s.toolbarBG, s.mainWindowBG, 0.10f);
    s.historyCapsuleActiveBG = MixThemeColor(s.frameBgBase, ImVec4(1.0f, 1.0f, 1.0f, 1.0f), 0.72f);
    s.modeCapsuleBG = MixThemeColor(s.toolbarBG, s.mainWindowBG, 0.08f);
    s.modeCapsuleActiveBG = MixThemeColor(s.frameBgBase, ImVec4(1.0f, 1.0f, 1.0f, 1.0f), 0.72f);
    s.toolbarAccent = s.resultsSelection;
    s.controlAccent = s.toolbarAccent;
    s.resultsGradientStart = s.resultsBG;
    s.resultsGradientEnd = MixThemeColor(s.resultsBG, s.resultsSelection, 0.10f);
    s.playlistGradientHighlight = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
    s.playlistGradientShadow = s.resultsBG;
    s.coverWallBG = s.metadataPanelBG;
    s.waveformPanelBorder = MixThemeColor(
        s.waveformBG,
        MixThemeColor(s.chromeBorder, s.controlAccent, 0.18f),
        0.44f);
    s.waveformSplitterBG = MixThemeColor(s.resultsBG, s.waveformBG, 0.18f);
    s.waveformSplitterLine = MixThemeColor(
        s.waveformSplitterBG,
        MixThemeColor(s.waveformPanelBorder, s.controlAccent, 0.14f),
        0.24f);
    s.waveformFooterBG = MixThemeColor(s.waveformBG, s.mainWindowBG, 0.28f);
    s.diagnosticOverlayBG = ImVec4(s.dialogPanelBG.x, s.dialogPanelBG.y, s.dialogPanelBG.z, 0.92f);
    s.diagnosticOverlayText = s.interfaceText;
    s.diagnosticOverlayAccent = s.toolbarAccent;
    s.diagnosticOverlayMutedText = MixThemeColor(s.interfaceText, s.diagnosticOverlayBG, 0.42f);
    // F-034: Preset dial — derive from existing capsule/toolbar tokens
    // Bg: pull toward mainWindowBG so it reads as a recessed trough inside the toolbar
    s.presetDialBg               = MixThemeColor(s.mainWindowBG, s.toolbarBG, 0.22f);
    s.presetDialCardActiveBg     = s.modeCapsuleActiveBG;
    // Active text: interfaceText (cool blue-white ~#b8cbe4) — matches reference design, reads
    // well on dark toolbar. No filled card in v2, so must contrast with toolbar bg, not card bg.
    s.presetDialCardActiveText   = s.interfaceText;
    // Inactive text: same hue as active, distAlpha in the draw code dims it by distance
    s.presetDialCardInactiveText = s.interfaceText;
    // Accent: derive from cool interfaceText, NOT controlAccent (which may be amber/gold)
    // This gives electric-blue-gray lines consistent with the workstation aesthetic
    s.presetDialAccent           = MixThemeColor(s.interfaceText, s.modeCapsuleActiveBG, 0.30f);
    s.filterBrowserSearchHitText = MixThemeColor(s.controlAccent, s.filterBrowserTreeText, 0.25f);
    s.filterBrowserLocateTint = s.transferButtonBG;
    s.filterBrowserTagText = MixThemeColor(s.filterBrowserText, s.controlAccent, 0.3f);
    s.miniWaveColor = s.waveformColor;
    s.miniWaveBG = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    // Results panel border — derive from theme accent so light themes don't get warm gold
    {
        ImVec4 base = MixThemeColor(s.controlAccent, s.resultsSelection, 0.5f);
        s.resultsActiveBorder = ImVec4(base.x, base.y, base.z, 0.85f);
    }
    // F-034 Wwise semantic tokens — four distinct hue axes, all theme-rooted.
    // Green axis  (ok/leaf):   volumeColor → vivid green  — "audio works / connected"
    // Cyan axis   (new/parent): fixed sky-teal            — "new node / container"
    // Amber axis  (replaced):  resultsSelection           — "something changed"
    // Red axis    (fail/unresolved): volumeClipColor      — "error / needs attention"
    // Neutral     (nochange):  interfaceText→chromeBorder — "nothing to do"
    //
    // connectOk roots in volumeColor so it always harmonises with the theme's
    // existing green meter/progress axis instead of introducing a disconnected lime.
    // diffNew uses a separate cyan axis so status chips keep clearly distinct meanings.
    // This keeps connected/new-node status chips visually distinct in drawStatChip.
    {
        const ImVec4 themeGreen  = MixThemeColor(s.volumeColor,
                                       ImVec4(0.42f, 0.88f, 0.48f, 1.0f), 0.32f); // pull toward vivid green
        const ImVec4 themeCyan   = MixThemeColor(ImVec4(0.38f, 0.76f, 0.90f, 1.0f),
                                       s.menuText, 0.10f);                          // sky-teal, softened toward theme temp
        const ImVec4 warmAnchor  = s.resultsSelection;                               // amber/gold
        const ImVec4 dangerAnchor = s.volumeClipColor;                               // red
        const ImVec4 textBase    = s.interfaceText;                                  // theme text
        s.wwiseConnectOk      = MixThemeColor(themeGreen,   textBase,       0.18f); // green axis
        s.wwiseConnectFail    = MixThemeColor(dangerAnchor, textBase,       0.18f); // red axis
        s.wwiseDiffNeutral    = MixThemeColor(textBase,     s.chromeBorder, 0.40f); // neutral
        s.wwiseDiffNew        = MixThemeColor(themeCyan,    textBase,       0.15f); // cyan axis — ≠ green
        s.wwiseDiffReplaced   = MixThemeColor(warmAnchor,   textBase,       0.10f); // amber axis
        s.wwiseDiffUnresolved = MixThemeColor(dangerAnchor, textBase,       0.12f); // red axis
        s.wwiseLeafBadge      = MixThemeColor(themeGreen,   themeCyan,      0.38f); // green+cyan
        s.wwiseParentBadge    = MixThemeColor(themeCyan,    textBase,       0.28f); // cyan axis
    }
    ApplyMeterThemeDefaults(s);
}

bool& ThemeColors::customized() {
    return s_customized;
}

ThemeColorScheme& ThemeColors::current() {
    return s_currentScheme;
}

void ThemeColors::applyStyleBaseline(ImGuiStyle& style) {
    // ScaleAllSizes() multiplies _MainScale each call; reset before re-baking sizes to avoid drift/jitter.
    style._MainScale        = 1.0f;
    style.WindowRounding    = 0.0f;
    style.ChildRounding     = 6.0f;
    style.FrameRounding     = 4.0f;
    style.PopupRounding     = 10.0f;
    style.ScrollbarRounding = 12.0f;
    style.GrabRounding      = 4.0f;
    style.TabRounding       = 4.0f;

    style.WindowPadding     = ImVec2(12, 12);
    style.WindowMinSize     = ImVec2(32.0f, 32.0f);
    style.FramePadding      = ImVec2(8, 5);
    style.ItemSpacing       = ImVec2(8, 6);
    style.ItemInnerSpacing  = ImVec2(6, 4);
    style.CellPadding       = ImVec2(8, 4);
    style.ScrollbarSize     = 12.0f;
    style.GrabMinSize       = 12.0f;
    style.WindowBorderHoverPadding = 4.0f;

    style.WindowBorderSize  = 0.0f;
    style.FrameBorderSize   = 0.0f;
    style.ChildBorderSize   = 1.0f;
    style.PopupBorderSize   = 0.0f;
}

ThemeDialogStyleScope::ThemeDialogStyleScope(bool layeredNavy) {
    if (!ImGui::GetCurrentContext()) {
        return;
    }

    const ThemeColorScheme& cs = ThemeColors::current();
    const bool light = ThemeColors::isLightChromeTheme(cs);
    const auto controls = MistBlueChrome::Controls(cs);

    auto withAlpha = [](ImVec4 c, float alpha) {
        c.w = alpha;
        return ClampThemeColor(c);
    };
    auto pushColor = [&](ImGuiCol idx, const ImVec4& color) {
        ImGui::PushStyleColor(idx, ClampThemeColor(color));
        ++m_colorCount;
    };
    auto pushVar = [&](ImGuiStyleVar idx, float value) {
        ImGui::PushStyleVar(idx, value);
        ++m_varCount;
    };
    auto pushVar2 = [&](ImGuiStyleVar idx, const ImVec2& value) {
        ImGui::PushStyleVar(idx, value);
        ++m_varCount;
    };

    const ImVec4 popupBase = layeredNavy ? cs.dialogBG : OpaqueThemeColor(ThemeColors::popupModalBG(cs));
    const ImVec4 bg = layeredNavy ? cs.dialogBG : OpaqueThemeColor(ThemeColors::popupModalBG(cs));
    const ImVec4 panel = layeredNavy ? cs.dialogPanelBG : OpaqueThemeColor(ThemeColors::popupModalPanelBG(cs));
    const ImVec4 input = layeredNavy ? controls.normal : withAlpha(MixThemeColor(cs.frameBgBase, panel, light ? 0.18f : 0.34f), light ? 0.94f : 0.98f);
    const ImVec4 text = OpaqueThemeColor(cs.interfaceText);
    const ImVec4 textMuted = layeredNavy ? controls.muted : OpaqueThemeColor(MixThemeColor(cs.interfaceText, bg, light ? 0.40f : 0.54f));
    const ImVec4 accent = OpaqueThemeColor(cs.controlAccent);
    const ImVec4 border = ThemeColors::popupBorder(cs);
    const ImVec4 button = layeredNavy ? controls.normal : OpaqueThemeColor(MixThemeColor(panel, cs.transportBarBG, light ? 0.12f : 0.24f));
    const ImVec4 buttonHover = layeredNavy ? controls.hovered : OpaqueThemeColor(MixThemeColor(button, cs.controlAccent, light ? 0.14f : 0.18f));
    const ImVec4 buttonActive = layeredNavy ? controls.active : OpaqueThemeColor(MixThemeColor(button, cs.controlAccent, light ? 0.22f : 0.28f));
    const ImVec4 title = layeredNavy ? controls.title : OpaqueThemeColor(MixThemeColor(cs.transportBarBG, panel, light ? 0.38f : 0.10f));
    const ImVec4 scrollbarGrab = OpaqueThemeColor(MixThemeColor(cs.resultsScrollbar, panel, light ? 0.16f : 0.20f));

    pushColor(ImGuiCol_Text, text);
    pushColor(ImGuiCol_TextDisabled, textMuted);
    pushColor(ImGuiCol_WindowBg, bg);
    pushColor(ImGuiCol_ChildBg, panel);
    pushColor(ImGuiCol_PopupBg, popupBase);
    pushColor(ImGuiCol_Border, border);
    pushColor(ImGuiCol_BorderShadow, ThemeColors::popupShadow(cs));
    pushColor(ImGuiCol_FrameBg, input);
    pushColor(ImGuiCol_FrameBgHovered, layeredNavy ? controls.hovered : OpaqueThemeColor(MixThemeColor(input, accent, light ? 0.10f : 0.14f)));
    pushColor(ImGuiCol_FrameBgActive, layeredNavy ? controls.active : OpaqueThemeColor(MixThemeColor(input, accent, light ? 0.18f : 0.22f)));
    pushColor(ImGuiCol_TitleBg, title);
    pushColor(ImGuiCol_TitleBgActive, title);
    pushColor(ImGuiCol_TitleBgCollapsed, title);
    pushColor(ImGuiCol_MenuBarBg, bg);
    pushColor(ImGuiCol_Button, button);
    pushColor(ImGuiCol_ButtonHovered, buttonHover);
    pushColor(ImGuiCol_ButtonActive, buttonActive);
    pushColor(ImGuiCol_Header, ThemeColors::popupSelectionBG(cs));
    pushColor(ImGuiCol_HeaderHovered, ThemeColors::popupSelectionHoveredBG(cs));
    pushColor(ImGuiCol_HeaderActive, ThemeColors::popupSelectionActiveBG(cs));
    pushColor(ImGuiCol_Separator, border);
    pushColor(ImGuiCol_SeparatorHovered, withAlpha(accent, light ? 0.50f : 0.70f));
    pushColor(ImGuiCol_SeparatorActive, accent);
    pushColor(ImGuiCol_CheckMark, accent);
    pushColor(ImGuiCol_SliderGrab, accent);
    pushColor(ImGuiCol_SliderGrabActive, buttonActive);
    m_colorCount += aura::ui::workspace::PushTransientScrollbarColors(
        withAlpha(scrollbarGrab, light ? 0.52f : 0.62f),
        withAlpha(MixThemeColor(scrollbarGrab, accent, 0.18f), 0.82f),
        withAlpha(MixThemeColor(scrollbarGrab, accent, 0.28f), 0.95f));
    pushColor(ImGuiCol_Tab, withAlpha(panel, 0.86f));
    pushColor(ImGuiCol_TabHovered, buttonHover);
    pushColor(ImGuiCol_TabActive, buttonActive);
    pushColor(ImGuiCol_TabUnfocused, withAlpha(MixThemeColor(bg, cs.mainWindowBG, 0.28f), 0.97f));
    pushColor(ImGuiCol_TabUnfocusedActive, panel);

    pushVar2(ImGuiStyleVar_WindowPadding, ImVec2(18.0f, 18.0f));
    pushVar2(ImGuiStyleVar_FramePadding, ImVec2(12.0f, 6.0f));
    pushVar2(ImGuiStyleVar_ItemSpacing, ImVec2(12.0f, 12.0f));
    pushVar2(ImGuiStyleVar_ItemInnerSpacing, ImVec2(8.0f, 8.0f));
    pushVar(ImGuiStyleVar_WindowRounding, 6.0f);
    pushVar(ImGuiStyleVar_ChildRounding, 6.0f);
    pushVar(ImGuiStyleVar_FrameRounding, 4.0f);
    pushVar(ImGuiStyleVar_PopupRounding, 8.0f);
    pushVar(ImGuiStyleVar_ScrollbarSize, 14.0f);
    pushVar(ImGuiStyleVar_ScrollbarRounding, 10.0f);
    pushVar(ImGuiStyleVar_GrabMinSize, 12.0f);
    pushVar(ImGuiStyleVar_GrabRounding, 4.0f);
    pushVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    pushVar(ImGuiStyleVar_ChildBorderSize, 1.0f);
    pushVar(ImGuiStyleVar_PopupBorderSize, 1.0f);
    pushVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
}

void ThemeDialogStyleScope::reset() {
    if (m_varCount > 0) {
        ImGui::PopStyleVar(m_varCount);
        m_varCount = 0;
    }
    if (m_colorCount > 0) {
        ImGui::PopStyleColor(m_colorCount);
        m_colorCount = 0;
    }
}

ThemeDialogStyleScope::~ThemeDialogStyleScope() {
    reset();
}

void ThemeColors::activateCustomScheme(const ThemeColorScheme& scheme) {
    s_currentScheme = scheme;
    s_customized = true;
}

void ThemeColors::deactivateCustomTheme() {
    s_customized = false;
}

void ThemeColors::armImportDiagnostics(
    const char* source,
    const std::string& pathUtf8,
    int startThemeValue,
    const char* startThemeLabel,
    bool livePreview) {
    s_themeImportDiag.armed = true;
    s_themeImportDiag.source = source ? source : "";
    s_themeImportDiag.pathUtf8 = pathUtf8;
    s_themeImportDiag.startThemeValue = startThemeValue;
    s_themeImportDiag.startThemeLabel = startThemeLabel ? startThemeLabel : "";
    s_themeImportDiag.livePreview = livePreview;
    s_themeImportDiag.preCustomized = s_customized;
    if (ImGui::GetCurrentContext()) {
        const ImGuiStyle& style = ImGui::GetStyle();
        s_themeImportDiag.preWindowBg = style.Colors[ImGuiCol_WindowBg];
        s_themeImportDiag.preChildBg = style.Colors[ImGuiCol_ChildBg];
        s_themeImportDiag.prePopupBg = style.Colors[ImGuiCol_PopupBg];
        s_themeImportDiag.preModalDimBg = style.Colors[ImGuiCol_ModalWindowDimBg];
    }
}

bool ThemeColors::takePendingRenderDiagnostics(
    std::string& sourceOut,
    std::string& startThemeLabelOut) {
    if (!s_themeRenderDiagPending) {
        return false;
    }
    sourceOut = s_themeRenderDiagSource;
    startThemeLabelOut = s_themeRenderDiagStartTheme;
    s_themeRenderDiagPending = false;
    s_themeRenderDiagSource.clear();
    s_themeRenderDiagStartTheme.clear();
    return true;
}

static bool s_pendingUiChromeResync = false;

void ThemeColors::markPendingUiChromeResync() {
    s_pendingUiChromeResync = true;
}

bool ThemeColors::hasPendingUiChromeResync() {
    return s_pendingUiChromeResync;
}

bool ThemeColors::consumePendingUiChromeResync() {
    if (!s_pendingUiChromeResync) {
        return false;
    }
    s_pendingUiChromeResync = false;
    return true;
}

void ThemeColors::discardPendingUiChromeResync() {
    s_pendingUiChromeResync = false;
}

bool ThemeColors::loadAndApplyThemeFile(
    const std::string& pathUtf8,
    ThemeColorScheme* loadedScheme,
    bool applyNow,
    const char* source,
    int startThemeValue,
    const char* startThemeLabel,
    bool livePreview) {
    ThemeColorScheme loaded;
    if (!loadThemeFromFile(pathUtf8, loaded)) {
        return false;
    }
    if (source && source[0]) {
        armImportDiagnostics(source, pathUtf8, startThemeValue, startThemeLabel, livePreview);
    }
    activateCustomScheme(loaded);
    if (applyNow) {
        applyToImGui();
        // applyToImGui resets style baseline; without the next frame's ScaleAllSizes (or full ApplyTheme path),
        // borders/layout can look wrong until another UI action forces a resync (e.g. closing Theme Editor).
        markPendingUiChromeResync();
    }
    if (loadedScheme) {
        *loadedScheme = loaded;
    }
    return true;
}

bool ThemeColors::shouldApplyCurrentScheme(bool showThemeEditor, bool customThemeSelected) {
    return showThemeEditor || (customThemeSelected && s_customized);
}

bool ThemeColors::isLightChromeTheme(const ThemeColorScheme& scheme) {
    auto luminance = [](const ImVec4& c) -> float {
        return c.x * 0.299f + c.y * 0.587f + c.z * 0.114f;
    };
    const float mainLuma = luminance(scheme.mainWindowBG);
    const float resultsLuma = luminance(scheme.resultsBG);
    const float dialogLuma = luminance(scheme.dialogBG);
    const float waveformLuma = luminance(scheme.waveformBG);
    const float chromeAverage = (mainLuma * 2.0f + resultsLuma + dialogLuma + waveformLuma) / 5.0f;
    return chromeAverage >= 0.72f || resultsLuma >= 0.74f || waveformLuma >= 0.74f;
}

void ThemeColors::applySimpleCustomOverrides(
    ThemeColorScheme& scheme,
    const ImVec4& background,
    const ImVec4& text,
    const ImVec4& accent,
    const ImVec4& waveform,
    const ImVec4& volume) {
    const ImVec4 childBg = OpaqueThemeColor(ImVec4(
        std::clamp(background.x + 0.03f, 0.0f, 1.0f),
        std::clamp(background.y + 0.03f, 0.0f, 1.0f),
        std::clamp(background.z + 0.03f, 0.0f, 1.0f),
        1.0f));
    const ImVec4 dimText = OpaqueThemeColor(ImVec4(text.x * 0.6f, text.y * 0.6f, text.z * 0.6f, 1.0f));
    const ImVec4 accentHot = OpaqueThemeColor(ImVec4(
        std::clamp(accent.x * 1.2f, 0.0f, 1.0f),
        std::clamp(accent.y * 1.2f, 0.0f, 1.0f),
        std::clamp(accent.z * 1.2f, 0.0f, 1.0f),
        1.0f));
    const ImVec4 waveformSecondary = OpaqueThemeColor(MixThemeColor(waveform, text, 0.18f));

    scheme.mainWindowBG = OpaqueThemeColor(background);
    scheme.menuBarBG = OpaqueThemeColor(MixThemeColor(background, scheme.menuBarBG, 0.18f));
    scheme.toolbarBG = OpaqueThemeColor(MixThemeColor(background, scheme.toolbarBG, 0.18f));
    scheme.transportBarBG = OpaqueThemeColor(MixThemeColor(background, scheme.transportBarBG, 0.12f));

    scheme.interfaceText = OpaqueThemeColor(text);
    scheme.menuText = OpaqueThemeColor(text);
    scheme.resultsText = OpaqueThemeColor(text);
    scheme.metadataPanelText = OpaqueThemeColor(text);
    scheme.filterBrowserText = OpaqueThemeColor(text);
    scheme.transferHistoryText = OpaqueThemeColor(text);
    scheme.spottingText = OpaqueThemeColor(text);
    scheme.resultsSecondaryText = dimText;

    scheme.resultsBG = childBg;
    scheme.metadataPanelBG = childBg;
    scheme.filterBrowserBG = childBg;
    scheme.transferHistoryBG = childBg;
    scheme.spottingBG = childBg;

    scheme.resultsSelection = OpaqueThemeColor(accent);
    scheme.toolbarAccent = OpaqueThemeColor(accent);
    scheme.controlAccent = OpaqueThemeColor(accent);
    scheme.searchButtonBG = OpaqueThemeColor(accent);
    scheme.transferButtonBG = OpaqueThemeColor(accentHot);
    scheme.filterBrowserSelection = OpaqueThemeColor(accent);
    scheme.filterBrowserTreeSelection = OpaqueThemeColor(accent);

    scheme.waveformColor = OpaqueThemeColor(waveform);
    scheme.waveformSecondaryColor = waveformSecondary;
    scheme.volumeColor = OpaqueThemeColor(volume);
}

// ARGB Hex Parsing (format: "ffRRGGBB")

ImVec4 ThemeColors::fromARGBHex(const std::string& hex) {
    if (hex.length() < 6) return ImVec4(1, 1, 1, 1);
    
    unsigned long val = 0;
    try { val = std::stoul(hex, nullptr, 16); } catch (...) { return ImVec4(1, 1, 1, 1); }
    
    float a, r, g, b;
    if (hex.length() >= 8) {
        a = ((val >> 24) & 0xFF) / 255.0f;
        r = ((val >> 16) & 0xFF) / 255.0f;
        g = ((val >> 8) & 0xFF) / 255.0f;
        b = (val & 0xFF) / 255.0f;
    } else {
        a = 1.0f;
        r = ((val >> 16) & 0xFF) / 255.0f;
        g = ((val >> 8) & 0xFF) / 255.0f;
        b = (val & 0xFF) / 255.0f;
    }
    return ImVec4(r, g, b, a);
}

std::string ThemeColors::toARGBHex(const ImVec4& color) {
    unsigned int a = (unsigned int)(color.w * 255.0f) & 0xFF;
    unsigned int r = (unsigned int)(color.x * 255.0f) & 0xFF;
    unsigned int g = (unsigned int)(color.y * 255.0f) & 0xFF;
    unsigned int b = (unsigned int)(color.z * 255.0f) & 0xFF;
    
    std::ostringstream oss;
    oss << std::hex << std::setfill('0')
        << std::setw(2) << a
        << std::setw(2) << r
        << std::setw(2) << g
        << std::setw(2) << b;
    return oss.str();
}

// Apply Current Color Scheme to ImGui

void ThemeColors::applyToImGui() {
    ImGuiStyle& style = ImGui::GetStyle();
    ThemeColorScheme& cs = s_currentScheme;

    applyStyleBaseline(style);

    // 全量颜色映射 — 每个 ImGui 色槽只由一个语义字段控制

    ImVec4* colors = style.Colors;
    const float windowLuma = cs.mainWindowBG.x * 0.299f + cs.mainWindowBG.y * 0.587f + cs.mainWindowBG.z * 0.114f;
    const bool isLightWindowTheme = windowLuma >= 0.72f;
    const ImVec4 scrimBase = MixThemeColor(
        MixThemeColor(cs.mainWindowBG, cs.dialogBG, 0.54f),
        cs.chromeBorder,
        isLightWindowTheme ? 0.06f : 0.14f);

// 文字
    // interfaceText → 全局文字（菜单/按钮/标签等通用文字）
    // resultsText 只用于结果列表区（由 main.cpp PushStyleColor 单独控制）
    colors[ImGuiCol_Text]                 = cs.interfaceText;
    colors[ImGuiCol_TextDisabled]         = ImVec4(cs.interfaceText.x * 0.6f, cs.interfaceText.y * 0.6f, cs.interfaceText.z * 0.6f, 1.0f);

// 窗口背景
    // mainWindowBG → 根窗口; ChildBg 透明（各面板 PushStyleColor 独立控制）
    colors[ImGuiCol_WindowBg]             = cs.mainWindowBG;
    colors[ImGuiCol_ChildBg]              = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    colors[ImGuiCol_PopupBg]              = ThemeColors::popupTooltipBG(cs);
    colors[ImGuiCol_NavWindowingHighlight]= ImVec4(1.00f, 1.00f, 1.00f, 0.70f);
    colors[ImGuiCol_NavWindowingDimBg]    = ImVec4(scrimBase.x, scrimBase.y, scrimBase.z, isLightWindowTheme ? 0.14f : 0.24f);
    colors[ImGuiCol_ModalWindowDimBg]     = ImVec4(scrimBase.x, scrimBase.y, scrimBase.z, isLightWindowTheme ? 0.22f : 0.36f);

// 边框
    // resultsOutline → 边框
    colors[ImGuiCol_Border]               = cs.chromeBorder;
    colors[ImGuiCol_BorderShadow]         = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);

// 标题栏
    // transportBarBG → 标题栏
    colors[ImGuiCol_TitleBg]              = cs.transportBarBG;
    colors[ImGuiCol_TitleBgActive]        = cs.transportBarBG;

// 菜单栏
    // menuBarBG → 菜单栏
    colors[ImGuiCol_MenuBarBg]            = cs.menuBarBG;

// 输入框/控件背景
    // frameBgBase → 控件背景基色
    // Lift very dark frameBgBase (luminance < 0.14) so combos/inputs are never pure-black
    // on dark themes.  Light themes are unaffected (their luminance is >> 0.14).
    {
        const float lum = cs.frameBgBase.x * 0.299f + cs.frameBgBase.y * 0.587f + cs.frameBgBase.z * 0.114f;
        const float kMinLum = 0.14f;
        ImVec4 fb = cs.frameBgBase;
        if (lum < kMinLum) {
            const float lift = kMinLum - lum;
            fb.x = (std::min)(1.0f, fb.x + lift);
            fb.y = (std::min)(1.0f, fb.y + lift);
            fb.z = (std::min)(1.0f, fb.z + lift);
        }
        colors[ImGuiCol_FrameBg]        = ImVec4(fb.x,          fb.y,          fb.z,          0.74f);
        colors[ImGuiCol_FrameBgHovered] = ImVec4(fb.x + 0.04f,  fb.y + 0.04f,  fb.z + 0.06f,  0.84f);
        colors[ImGuiCol_FrameBgActive]  = ImVec4(fb.x + 0.06f,  fb.y + 0.06f,  fb.z + 0.08f,  0.90f);
    }

// 按钮
    // searchButtonBG 只用于搜索控件本身，避免误伤全局按钮 hover
    colors[ImGuiCol_Button]               = ImVec4(cs.frameBgBase.x + 0.03f, cs.frameBgBase.y + 0.03f, cs.frameBgBase.z + 0.05f, 1.0f);
    colors[ImGuiCol_ButtonHovered]        = ImVec4(
        cs.frameBgBase.x + (cs.controlAccent.x - cs.frameBgBase.x) * 0.24f,
        cs.frameBgBase.y + (cs.controlAccent.y - cs.frameBgBase.y) * 0.24f,
        cs.frameBgBase.z + (cs.controlAccent.z - cs.frameBgBase.z) * 0.24f,
        0.92f);
    colors[ImGuiCol_ButtonActive]         = cs.controlAccent;

// 列表选中 (Header)
    // resultsSelection → 仅列表选中高亮
    colors[ImGuiCol_Header]               = ImVec4(cs.resultsSelection.x, cs.resultsSelection.y, cs.resultsSelection.z, 0.25f);
    colors[ImGuiCol_HeaderHovered]        = ImVec4(cs.resultsSelection.x, cs.resultsSelection.y, cs.resultsSelection.z, 0.40f);
    colors[ImGuiCol_HeaderActive]         = ImVec4(cs.resultsSelection.x, cs.resultsSelection.y, cs.resultsSelection.z, 0.80f);

// 表格
    // resultsHeaderBG → 表头; resultsOutline → 表格线
    colors[ImGuiCol_TableHeaderBg]        = cs.resultsHeaderBG;
    colors[ImGuiCol_TableBorderStrong]    = cs.chromeBorder;
    colors[ImGuiCol_TableBorderLight]     = ImVec4(cs.chromeBorder.x, cs.chromeBorder.y, cs.chromeBorder.z, 0.4f);
    colors[ImGuiCol_TableRowBg]           = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    colors[ImGuiCol_TableRowBgAlt]        = ImVec4(1.00f, 1.00f, 1.00f, 0.02f);

// 勾选框
    // controlAccent → 通用勾选/交互强调
    colors[ImGuiCol_CheckMark]            = cs.controlAccent;

// 滑块
    // resultsScrollbar → 滑块拖块
    colors[ImGuiCol_SliderGrab]           = cs.resultsScrollbar;
    colors[ImGuiCol_SliderGrabActive]     = ImVec4(cs.resultsScrollbar.x + 0.1f, cs.resultsScrollbar.y + 0.1f, cs.resultsScrollbar.z + 0.1f, 1.0f);

// 标签页
    // tabBG → 标签页全系列; 不混用 resultsSelection
    colors[ImGuiCol_Tab]                  = cs.tabBG;
    colors[ImGuiCol_TabHovered]           = ImVec4(
        cs.tabBG.x + (cs.controlAccent.x - cs.tabBG.x) * 0.72f,
        cs.tabBG.y + (cs.controlAccent.y - cs.tabBG.y) * 0.72f,
        cs.tabBG.z + (cs.controlAccent.z - cs.tabBG.z) * 0.72f,
        1.0f);
    colors[ImGuiCol_TabActive]            = ImVec4(
        cs.tabBG.x + (cs.controlAccent.x - cs.tabBG.x) * 0.90f,
        cs.tabBG.y + (cs.controlAccent.y - cs.tabBG.y) * 0.90f,
        cs.tabBG.z + (cs.controlAccent.z - cs.tabBG.z) * 0.90f,
        1.0f);
    colors[ImGuiCol_TabUnfocused]         = ImVec4(cs.tabBG.x - 0.04f, cs.tabBG.y - 0.04f, cs.tabBG.z - 0.04f, 1.0f);
    colors[ImGuiCol_TabUnfocusedActive]   = cs.tabBG;

// 滚动条
    // resultsScrollbar → 滚动条
    colors[ImGuiCol_ScrollbarBg]          = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    colors[ImGuiCol_ScrollbarGrab]        = ImVec4(cs.resultsScrollbar.x, cs.resultsScrollbar.y, cs.resultsScrollbar.z, 0.40f);
    colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(cs.resultsScrollbar.x, cs.resultsScrollbar.y, cs.resultsScrollbar.z, 0.60f);
    colors[ImGuiCol_ScrollbarGrabActive]  = ImVec4(cs.resultsScrollbar.x, cs.resultsScrollbar.y, cs.resultsScrollbar.z, 0.80f);

// ResizeGrip
    // chromeBorder/controlAccent → 拖拽手柄
    colors[ImGuiCol_ResizeGrip]           = ImVec4(cs.chromeBorder.x, cs.chromeBorder.y, cs.chromeBorder.z, 0.30f);
    colors[ImGuiCol_ResizeGripHovered]    = ImVec4(cs.controlAccent.x, cs.controlAccent.y, cs.controlAccent.z, 0.60f);
    colors[ImGuiCol_ResizeGripActive]     = ImVec4(cs.controlAccent.x, cs.controlAccent.y, cs.controlAccent.z, 0.90f);

// 分隔线
    // chromeBorder/controlAccent → 分隔线
    colors[ImGuiCol_Separator]            = ImVec4(cs.chromeBorder.x, cs.chromeBorder.y, cs.chromeBorder.z, 0.20f);
    colors[ImGuiCol_SeparatorHovered]     = ImVec4(cs.controlAccent.x, cs.controlAccent.y, cs.controlAccent.z, 0.60f);
    colors[ImGuiCol_SeparatorActive]      = cs.controlAccent;

// 音量/电平
    // Legacy ImGui plot colors mirror the meter base / clip tokens.
    colors[ImGuiCol_PlotHistogram]        = cs.meterBase;
    colors[ImGuiCol_PlotHistogramHovered] = cs.meterClip;

    // Light-chrome ImGui polish aligned with Cloud Sea / modern light references:
    // disabled text → secondary (medium blue), frame hover → cream popup, sliders → teal + dark-blue active,
    // tabs hover → light blue mix, scroll grab hover/active → medium blue / teal, 1px frames + tighter grabs.
    if (isLightWindowTheme) {
        colors[ImGuiCol_TextDisabled] =
            ImVec4(cs.resultsSecondaryText.x, cs.resultsSecondaryText.y, cs.resultsSecondaryText.z, 1.0f);

        colors[ImGuiCol_FrameBgHovered] =
            ImVec4(cs.popupBG.x, cs.popupBG.y, cs.popupBG.z, 0.94f);
        colors[ImGuiCol_FrameBgActive] =
            ImVec4(cs.resultsScrollbar.x, cs.resultsScrollbar.y, cs.resultsScrollbar.z, 0.30f);

        colors[ImGuiCol_Button] =
            ImVec4(cs.resultsScrollbar.x, cs.resultsScrollbar.y, cs.resultsScrollbar.z, 0.22f);
        colors[ImGuiCol_ButtonHovered] = ImVec4(cs.popupBG.x, cs.popupBG.y, cs.popupBG.z, 1.0f);

        colors[ImGuiCol_Header] =
            ImVec4(cs.controlAccent.x, cs.controlAccent.y, cs.controlAccent.z, 0.20f);
        colors[ImGuiCol_HeaderHovered] =
            ImVec4(cs.controlAccent.x, cs.controlAccent.y, cs.controlAccent.z, 0.40f);
        colors[ImGuiCol_HeaderActive] = cs.controlAccent;

        colors[ImGuiCol_SliderGrab] = cs.controlAccent;
        colors[ImGuiCol_SliderGrabActive] =
            ImVec4(cs.interfaceText.x, cs.interfaceText.y, cs.interfaceText.z, 1.0f);

        colors[ImGuiCol_TabHovered] = MixThemeColor(cs.tabBG, cs.resultsScrollbar, 0.78f);
        colors[ImGuiCol_TabActive]  = cs.controlAccent;

        colors[ImGuiCol_ScrollbarGrabHovered] =
            ImVec4(cs.resultsSecondaryText.x, cs.resultsSecondaryText.y, cs.resultsSecondaryText.z, 0.65f);
        colors[ImGuiCol_ScrollbarGrabActive] = cs.controlAccent;

        colors[ImGuiCol_Separator] = ImVec4(cs.chromeBorder.x, cs.chromeBorder.y, cs.chromeBorder.z, 0.50f);
        colors[ImGuiCol_SeparatorHovered] =
            ImVec4(cs.resultsSecondaryText.x, cs.resultsSecondaryText.y, cs.resultsSecondaryText.z, 0.70f);

        colors[ImGuiCol_ResizeGrip] =
            ImVec4(cs.resultsScrollbar.x, cs.resultsScrollbar.y, cs.resultsScrollbar.z, 0.20f);
        colors[ImGuiCol_ResizeGripHovered] =
            ImVec4(cs.resultsSecondaryText.x, cs.resultsSecondaryText.y, cs.resultsSecondaryText.z, 0.67f);

        colors[ImGuiCol_TextSelectedBg] =
            ImVec4(cs.controlAccent.x, cs.controlAccent.y, cs.controlAccent.z, 0.35f);
        colors[ImGuiCol_NavHighlight] =
            ImVec4(cs.controlAccent.x, cs.controlAccent.y, cs.controlAccent.z, 0.78f);

        // Palette variants must not fork layout density or radius; keep all metrics
        // in applyStyleBaseline() so theme changes only alter color semantics.
        style.AntiAliasedLines  = true;
        style.AntiAliasedFill   = true;
    }

    // 同步波形全局变量 (g_waveformColor etc. defined in main.cpp)
    auto vec4ToU32 = [](const ImVec4& c) -> ImU32 {
        return IM_COL32(
            (int)(c.x * 255.0f + 0.5f),
            (int)(c.y * 255.0f + 0.5f),
            (int)(c.z * 255.0f + 0.5f),
            (int)(c.w * 255.0f + 0.5f));
    };
    g_waveformColor   = vec4ToU32(cs.waveformColor);
    g_waveformBgColor = vec4ToU32(cs.waveformBG);
    g_miniWaveColor   = vec4ToU32(cs.miniWaveColor);
    g_miniWaveBgColor = vec4ToU32(cs.miniWaveBG);
    g_waveformRmsColor = IM_COL32(
        (int)(cs.waveformSecondaryColor.x * 255.0f + 0.5f),
        (int)(cs.waveformSecondaryColor.y * 255.0f + 0.5f),
        (int)(cs.waveformSecondaryColor.z * 255.0f + 0.5f), 180);

    // Keep backward-compatible globals in sync for UI code paths
    // that still read legacy COLOR_* tokens after custom theme loads.
    COLOR_BG_DARK     = cs.mainWindowBG;
    COLOR_BG_PANEL    = cs.resultsBG;
    COLOR_TEXT        = cs.interfaceText;
    COLOR_TEXT_DIM    = ImVec4(cs.interfaceText.x * 0.6f, cs.interfaceText.y * 0.6f, cs.interfaceText.z * 0.6f, 1.0f);
    COLOR_ACCENT      = cs.toolbarAccent;
    COLOR_ACCENT_DIM  = ImVec4(cs.toolbarAccent.x, cs.toolbarAccent.y, cs.toolbarAccent.z, 0.72f);
    COLOR_SELECTED    = cs.resultsSelection;
    COLOR_WAVEFORM_BG = cs.waveformBG;
    COLOR_WAVEFORM    = cs.waveformColor;

    if (s_themeImportDiag.armed) {
        auto& diag = DiagnosticsLogger::instance();
        if (!diag.isEnabled()) {
            diag.setEnabled(true);
        }
        auto tailPath = [](const std::string& pathUtf8) -> std::string {
            size_t pos = pathUtf8.find_last_of("\\/");
            return (pos == std::string::npos) ? pathUtf8 : pathUtf8.substr(pos + 1);
        };
        char buf[192];
        std::snprintf(
            buf, sizeof(buf),
            "ImportArm source=%s start=%s(%d) live=%d customizedBefore=%d file=%s",
            s_themeImportDiag.source.c_str(),
            s_themeImportDiag.startThemeLabel.c_str(),
            s_themeImportDiag.startThemeValue,
            s_themeImportDiag.livePreview ? 1 : 0,
            s_themeImportDiag.preCustomized ? 1 : 0,
            tailPath(s_themeImportDiag.pathUtf8).c_str());
        diag.logSystem("Theme", buf);
        AppendThemeTraceLine(std::string("Theme ") + buf);

        std::snprintf(
            buf, sizeof(buf),
            "ImportPre style win=%.3f/%.3f/%.3f child=%.3f/%.3f/%.3f popup=%.3f/%.3f/%.3f modal=%.3f/%.3f/%.3f",
            s_themeImportDiag.preWindowBg.x, s_themeImportDiag.preWindowBg.y, s_themeImportDiag.preWindowBg.z,
            s_themeImportDiag.preChildBg.x, s_themeImportDiag.preChildBg.y, s_themeImportDiag.preChildBg.z,
            s_themeImportDiag.prePopupBg.x, s_themeImportDiag.prePopupBg.y, s_themeImportDiag.prePopupBg.z,
            s_themeImportDiag.preModalDimBg.x, s_themeImportDiag.preModalDimBg.y, s_themeImportDiag.preModalDimBg.z);
        diag.logSystem("Theme", buf);
        AppendThemeTraceLine(std::string("Theme ") + buf);

        const ImVec4 infoDockBg = infoPanelDockBG(cs);
        std::snprintf(
            buf, sizeof(buf),
            "ImportPost scheme main=%.3f/%.3f/%.3f cover=%.3f/%.3f/%.3f info=%.3f/%.3f/%.3f customNow=%d",
            cs.mainWindowBG.x, cs.mainWindowBG.y, cs.mainWindowBG.z,
            cs.coverWallBG.x, cs.coverWallBG.y, cs.coverWallBG.z,
            infoDockBg.x, infoDockBg.y, infoDockBg.z,
            s_customized ? 1 : 0);
        diag.logSystem("Theme", buf);
        AppendThemeTraceLine(std::string("Theme ") + buf);

        std::snprintf(
            buf, sizeof(buf),
            "ImportPost style win=%.3f/%.3f/%.3f child=%.3f/%.3f/%.3f popup=%.3f/%.3f/%.3f modal=%.3f/%.3f/%.3f",
            colors[ImGuiCol_WindowBg].x, colors[ImGuiCol_WindowBg].y, colors[ImGuiCol_WindowBg].z,
            colors[ImGuiCol_ChildBg].x, colors[ImGuiCol_ChildBg].y, colors[ImGuiCol_ChildBg].z,
            colors[ImGuiCol_PopupBg].x, colors[ImGuiCol_PopupBg].y, colors[ImGuiCol_PopupBg].z,
            colors[ImGuiCol_ModalWindowDimBg].x, colors[ImGuiCol_ModalWindowDimBg].y, colors[ImGuiCol_ModalWindowDimBg].z);
        diag.logSystem("Theme", buf);
        AppendThemeTraceLine(std::string("Theme ") + buf);

        s_themeRenderDiagPending = true;
        s_themeRenderDiagSource = s_themeImportDiag.source;
        s_themeRenderDiagStartTheme = s_themeImportDiag.startThemeLabel;
        s_themeImportDiag = ThemeImportDiagState{};
    }
}

// Preset Color Schemes

namespace {

constexpr const char* kThemePresetNamesZh[] = {
    u8"工作室石板",
    u8"经典深色",
    u8"海洋蓝",
    u8"日落暖调",
    u8"太空灰",
    u8"电光蓝",
    u8"粉雾柔彩",
    u8"深海军蓝"
};

constexpr const char* kThemePresetNamesEn[] = {
    "Studio Slate",
    "Classic Dark",
    "Ocean Blue",
    "Sunset Warm",
    "Space Gray",
    "Electric Blue",
    "Powdered Pastels",
    "Deep Navy"
};

constexpr int kThemePresetCount = static_cast<int>(sizeof(kThemePresetNamesEn) / sizeof(kThemePresetNamesEn[0]));

} // namespace

int ThemeColors::getPresetCount() {
    return kThemePresetCount;
}

ThemeColorScheme ThemeColors::getPresetByIndex(int presetIndex) {
    switch (presetIndex) {
    case 0: return getStudioSlate();
    case 1: return getClassicDark();
    case 2: return getOceanBlue();
    case 3: return getSunsetWarm();
    case 4: return getSpaceGray();
    case 5: return getElectricBlue();
    case 6: return getPowderedPastels();
    case 7: return getDeepNavy();
    default: return getStudioSlate();
    }
}

const char* ThemeColors::getPresetDisplayNameZh(int presetIndex) {
    if (presetIndex < 0 || presetIndex >= kThemePresetCount) return ThemeColors::getSettingsThemeDisplayNameZh(3);
    return kThemePresetNamesZh[presetIndex];
}

const char* ThemeColors::getPresetDisplayNameEn(int presetIndex) {
    if (presetIndex < 0 || presetIndex >= kThemePresetCount) return ThemeColors::getSettingsThemeDisplayNameEn(3);
    return kThemePresetNamesEn[presetIndex];
}

ThemeColorScheme ThemeColors::getHarkenSearchProductDark() {
    // Canonical Aris product theme. Keep this mapping explicit: generated
    // design tokens are the source of truth, while ThemeColorScheme remains
    // the stable compatibility surface consumed by the native UI.
    const ImVec4 canvas = ToThemeColor(aris::design::kColorSurfaceCanvas);
    const ImVec4 panel = ToThemeColor(aris::design::kColorSurfacePanel);
    const ImVec4 elevated = ToThemeColor(aris::design::kColorSurfaceElevated);
    const ImVec4 input = ToThemeColor(aris::design::kColorSurfaceInput);
    const ImVec4 hover = ToThemeColor(aris::design::kColorSurfaceHover);
    const ImVec4 textPrimary = ToThemeColor(aris::design::kColorTextPrimary);
    const ImVec4 textSecondary = ToThemeColor(aris::design::kColorTextSecondary);
    const ImVec4 textMuted = ToThemeColor(aris::design::kColorTextMuted);
    const ImVec4 borderSubtle = ToThemeColor(aris::design::kColorBorderSubtle);
    const ImVec4 borderStrong = ToThemeColor(aris::design::kColorBorderStrong);
    const ImVec4 actionPrimary = ToThemeColor(aris::design::kColorActionPrimary);
    const ImVec4 actionPressed = ToThemeColor(aris::design::kColorActionPrimaryPressed);
    const ImVec4 measurement = ToThemeColor(aris::design::kColorSignalMeasurement);
    const ImVec4 playhead = ToThemeColor(aris::design::kColorSignalPlayhead);
    const ImVec4 success = ToThemeColor(aris::design::kColorStatusSuccess);
    const ImVec4 warning = ToThemeColor(aris::design::kColorStatusWarning);
    const ImVec4 danger = ToThemeColor(aris::design::kColorStatusDanger);
    const ImVec4 info = ToThemeColor(aris::design::kColorStatusInfo);

    ThemeColorScheme s;
    s.mainWindowBG = canvas;
    s.menuBarBG = canvas;
    s.menuText = textPrimary;
    s.toolbarBG = panel;
    s.transportBarBG = panel;
    s.timecodeText = measurement;
    s.timecodeTextHover = ToThemeColor(aris::design::kColorFocusRing);
    s.interfaceText = textSecondary;

    s.resultsBG = panel;
    s.resultsHeaderBG = input;
    s.resultsText = textPrimary;
    s.resultsOutline = borderSubtle;
    s.resultsSelection = actionPrimary;
    s.resultsScrollbar = textMuted;
    s.resultsTabBG = input;
    s.resultsTabHover = hover;
    s.resultsTabActive = borderStrong;
    s.resultsSecondaryText = textSecondary;
    s.resultsPlayedText = warning;
    s.playlistGradientStart = actionPrimary;
    s.playlistGradientEnd = actionPressed;

    s.metadataPanelBG = panel;
    s.metadataPanelText = textPrimary;
    s.metadataPanelLabel = textSecondary;
    s.metadataPanelScrollbar = textMuted;

    s.waveformBG = canvas;
    s.waveformColor = measurement;
    s.waveformSecondaryColor = measurement;
    s.waveformPositionLine = playhead;

    s.filterBrowserBG = panel;
    s.filterBrowserText = textPrimary;
    s.filterBrowserHeading = textPrimary;
    s.filterBrowserOutline = borderStrong;
    s.filterBrowserSelection = actionPrimary;
    s.filterBrowserScrollbar = textMuted;
    s.filterBrowserSectionBG = elevated;
    s.filterBrowserButtonBG = input;
    s.filterBrowserTreeText = textPrimary;
    s.filterBrowserTreeSelection = actionPrimary;

    s.transferButtonBG = success;
    s.searchButtonBG = actionPrimary;
    s.transferHistoryBG = panel;
    s.transferHistoryText = textSecondary;
    s.spottingBG = panel;
    s.spottingText = textPrimary;
    s.tabBG = input;
    s.volumeColor = success;
    s.volumeClipColor = danger;

    s.sidebarRailBG = canvas;
    s.sidebarRailDivider = borderSubtle;
    s.sidebarIconInactive = textMuted;
    s.sidebarIconHover = textSecondary;
    s.sidebarIconActive = actionPrimary;
    s.sidebarActiveStrip = actionPrimary;
    s.sidebarHeaderBG = canvas;
    s.sidebarStatusBarBG = panel;
    s.sidebarSubpaneBG = elevated;
    s.sidebarSubpaneDivider = borderStrong;
    s.sidebarSubHeaderLine = borderSubtle;

    ApplySemanticThemeDefaults(s);

    // ApplySemanticThemeDefaults derives compatibility fields for every old
    // preset. Reassert the direct semantic contracts that must remain identical
    // to Figma/Web tokens in the canonical Aris theme.
    s.frameBgBase = input;
    s.chromeBorder = borderSubtle;
    s.popupBG = elevated;
    s.menuPopupBG = elevated;
    s.dialogBG = elevated;
    s.dialogPanelBG = panel;
    s.toolbarAccent = actionPrimary;
    s.controlAccent = actionPrimary;
    s.resultsOutline = borderSubtle;
    s.resultsSelection = actionPrimary;
    s.searchButtonBG = actionPrimary;
    s.waveformPanelBorder = borderStrong;
    s.waveformColor = measurement;
    s.miniWaveColor = measurement;
    s.waveformPositionLine = playhead;
    s.volumeColor = success;
    s.volumeClipColor = danger;
    s.meterBase = success;
    s.meterWarning = warning;
    s.meterClip = danger;
    s.diagnosticOverlayAccent = info;
    return s;
}

ThemeColorScheme ThemeColors::getStudioSlate() {
    return getHarkenSearchProductDark();
}

ThemeColorScheme ThemeColors::getClassicDark() {
    ThemeColorScheme s;
    s.mainWindowBG       = ImVec4(0.08f, 0.08f, 0.08f, 1.0f);
    s.menuText           = ImVec4(0.92f, 0.92f, 0.92f, 1.0f);
    s.toolbarBG          = ImVec4(0.10f, 0.10f, 0.10f, 1.0f);
    s.timecodeText       = ImVec4(0.52f, 1.00f, 0.67f, 0.93f);
    s.timecodeTextHover  = ImVec4(0.52f, 1.00f, 0.67f, 1.0f);
    s.interfaceText      = ImVec4(0.80f, 0.80f, 0.80f, 1.0f);
    s.resultsBG          = ImVec4(0.10f, 0.10f, 0.10f, 1.0f);
    s.resultsText        = ImVec4(0.90f, 0.90f, 0.90f, 1.0f);
    s.resultsOutline     = ImVec4(0.25f, 0.25f, 0.25f, 1.0f);
    s.resultsSelection   = ImVec4(0.80f, 0.60f, 0.10f, 1.0f);
    s.resultsScrollbar   = ImVec4(0.40f, 0.40f, 0.40f, 1.0f);
    s.resultsTabBG       = ImVec4(0.14f, 0.14f, 0.14f, 0.96f);
    s.resultsTabHover    = ImVec4(0.20f, 0.20f, 0.20f, 1.0f);
    s.resultsTabActive   = ImVec4(0.28f, 0.28f, 0.28f, 1.0f);
    s.resultsSecondaryText = ImVec4(0.66f, 0.66f, 0.66f, 1.0f);
    s.playlistGradientStart = ImVec4(0.80f, 0.60f, 0.10f, 1.0f);
    s.playlistGradientEnd = ImVec4(0.46f, 0.34f, 0.08f, 1.0f);
    s.metadataPanelBG    = ImVec4(0.12f, 0.12f, 0.12f, 1.0f);
    s.metadataPanelText  = ImVec4(0.90f, 0.90f, 0.90f, 1.0f);
    s.metadataPanelLabel = ImVec4(0.60f, 0.80f, 1.00f, 1.0f);
    s.metadataPanelScrollbar = ImVec4(0.35f, 0.35f, 0.40f, 1.0f);
    s.waveformBG         = ImVec4(0.05f, 0.05f, 0.05f, 1.0f);
    s.waveformColor      = ImVec4(0.30f, 0.60f, 0.20f, 1.0f);
    s.waveformSecondaryColor = ImVec4(0.60f, 0.80f, 1.00f, 1.0f);
    s.waveformPositionLine = ImVec4(1.00f, 0.86f, 0.30f, 1.0f);
    s.filterBrowserBG    = ImVec4(0.14f, 0.14f, 0.14f, 1.0f);
    s.filterBrowserText  = ImVec4(0.90f, 0.90f, 0.90f, 1.0f);
    s.filterBrowserHeading = ImVec4(0.80f, 0.80f, 0.80f, 1.0f);
    s.filterBrowserOutline = ImVec4(0.30f, 0.30f, 0.30f, 1.0f);
    s.filterBrowserSelection = ImVec4(0.30f, 0.70f, 0.30f, 1.0f);
    s.filterBrowserScrollbar = ImVec4(0.40f, 0.40f, 0.40f, 1.0f);
    s.filterBrowserSectionBG = ImVec4(0.18f, 0.18f, 0.18f, 1.0f);
    s.filterBrowserButtonBG = ImVec4(0.20f, 0.20f, 0.20f, 1.0f);
    s.filterBrowserTreeText = ImVec4(0.92f, 0.92f, 0.92f, 1.0f);
    s.filterBrowserTreeSelection = ImVec4(0.30f, 0.70f, 0.30f, 1.0f);
    s.transferButtonBG   = ImVec4(0.00f, 0.50f, 0.00f, 1.0f);
    s.searchButtonBG     = ImVec4(0.30f, 0.50f, 0.80f, 1.0f);
    s.transferHistoryBG  = ImVec4(0.15f, 0.15f, 0.15f, 1.0f);
    s.transferHistoryText = ImVec4(0.70f, 0.70f, 0.70f, 1.0f);
    s.spottingBG         = ImVec4(0.15f, 0.15f, 0.15f, 1.0f);
    s.spottingText       = ImVec4(0.85f, 0.85f, 0.85f, 1.0f);
    s.tabBG              = ImVec4(0.20f, 0.20f, 0.20f, 1.0f);

// Sidebar Activity Bar (Classic Dark)
    s.sidebarRailBG          = fromARGBHex("ff1c1c1c");
    s.sidebarRailDivider     = fromARGBHex("0dffffff");
    s.sidebarIconInactive    = fromARGBHex("ff808080");
    s.sidebarIconHover       = fromARGBHex("ffb3b3b3");
    s.sidebarIconActive      = fromARGBHex("ff4cb34c");
    s.sidebarActiveStrip     = fromARGBHex("ff4cb34c");
    s.sidebarHeaderBG        = s.sidebarRailBG;
    s.sidebarStatusBarBG     = fromARGBHex("ff1d1d1d"); // filterBrowserBG(#232323) × 0.925
    s.sidebarSubHeaderLine   = fromARGBHex("26ffffff");

    ApplySemanticThemeDefaults(s);
    return s;
}

ThemeColorScheme ThemeColors::getOceanBlue() {
    ThemeColorScheme s;
// Base fields (used by ApplySemanticThemeDefaults as derivation sources)
    s.mainWindowBG          = fromARGBHex("ff0a2029");
    s.menuText              = fromARGBHex("ffe2ebf4");
    s.toolbarBG             = fromARGBHex("ff0b131f");
    s.interfaceText         = fromARGBHex("ffffffff");
    s.resultsBG             = fromARGBHex("ff0e1725");
    s.resultsText           = fromARGBHex("ffe2ebf4");
    s.resultsOutline        = fromARGBHex("ff233a5c");
    s.resultsSelection      = fromARGBHex("ff00bcd3");
    s.metadataPanelBG       = fromARGBHex("ff0e1725");
    s.metadataPanelText     = fromARGBHex("ffe2ebf4");
    s.metadataPanelLabel    = fromARGBHex("ff8ba2c0");
    s.waveformBG            = fromARGBHex("ff070b12");
    s.waveformColor         = fromARGBHex("ffa9eaff");
    s.waveformSecondaryColor= fromARGBHex("ff4177a5");
    s.waveformPositionLine  = fromARGBHex("fffcdac0");
    s.transferButtonBG      = fromARGBHex("ff3a82bc");
    s.filterBrowserTreeText = fromARGBHex("ffe2ebf4");
    ApplySemanticThemeDefaults(s);
// JSON-specific overrides (values that differ from derived defaults)
    s.frameBgBase           = fromARGBHex("ff182739");
    s.chromeBorder          = fromARGBHex("ff233a5c");
    s.popupBG               = fromARGBHex("f7101a28");
    s.menuPopupBG           = fromARGBHex("f7101a28");
    s.dialogBG              = fromARGBHex("f90f1826");
    s.dialogPanelBG         = fromARGBHex("ff101b29");
    s.historyCapsuleBG      = fromARGBHex("ff0b131d");
    s.historyCapsuleActiveBG= fromARGBHex("ff038595");
    s.modeCapsuleBG         = fromARGBHex("ff0b131e");
    s.modeCapsuleActiveBG   = fromARGBHex("ff038595");
    s.transportBarBG        = fromARGBHex("ff070b12");
    s.timecodeText          = fromARGBHex("f9a3deea");
    s.timecodeTextHover     = fromARGBHex("ffa7dfeb");
    s.resultsHeaderBG       = fromARGBHex("ff0b131f");
    s.resultsActiveBorder   = fromARGBHex("d8c69e38");
    s.resultsScrollbar      = fromARGBHex("ff2d4f77");
    s.resultsTabBG          = fromARGBHex("f40b131f");
    s.resultsTabHover       = fromARGBHex("ff285684");
    s.resultsTabActive      = fromARGBHex("ff306699");
    s.resultsSecondaryText  = fromARGBHex("ff7d8da2");
    s.resultsPlayedText     = fromARGBHex("ff2be4f1");
    s.resultsGradientStart  = fromARGBHex("ff0e1725");
    s.resultsGradientEnd    = fromARGBHex("ff0d2837");
    s.playlistGradientStart = fromARGBHex("ff00bcd3");
    s.playlistGradientEnd   = fromARGBHex("ff06778a");
    s.playlistGradientHighlight = fromARGBHex("ffffffff");
    s.playlistGradientShadow= fromARGBHex("ff0e1725");
    s.metadataPanelScrollbar= fromARGBHex("ff2d4f77");
    s.coverWallBG           = fromARGBHex("ff0e1725");
    s.waveformPanelBorder   = fromARGBHex("ff17263c");
    s.waveformSplitterBG    = fromARGBHex("ff0d1522");
    s.waveformSplitterLine  = fromARGBHex("ff101b2a");
    s.waveformFooterBG      = fromARGBHex("ff070b12");
    s.miniWaveColor         = fromARGBHex("ff80e1ec");
    s.miniWaveBG            = fromARGBHex("00000000");
    s.meterBg               = fromARGBHex("ff0c131d");
    s.meterBase             = fromARGBHex("ff66b772");
    s.meterMid              = fromARGBHex("ff8cc67f");
    s.meterWarning          = fromARGBHex("ffd1c66b");
    s.meterClip             = fromARGBHex("ffe06059");
    s.meterScaleText        = fromARGBHex("ff7d8da2");
    s.filterBrowserBG       = fromARGBHex("ff0e1725");
    s.filterBrowserText     = fromARGBHex("ffe2ebf4");
    s.filterBrowserHeading  = fromARGBHex("ffbecad7");
    s.filterBrowserOutline  = fromARGBHex("ff233a5c");
    s.filterBrowserSelection= fromARGBHex("ff00bcd3");
    s.filterBrowserScrollbar= fromARGBHex("ff2d4f77");
    s.filterBrowserSectionBG= fromARGBHex("ff0c2f3e");
    s.filterBrowserButtonBG = fromARGBHex("ff193c54");
    s.filterBrowserTreeSelection = fromARGBHex("ff00bcd3");
    s.filterBrowserSearchHitText = fromARGBHex("ff38c8db");
    s.filterBrowserLocateTint= fromARGBHex("ff3a82bc");
    s.searchButtonBG        = fromARGBHex("ff00bcd3");
    s.transferHistoryBG     = fromARGBHex("ff0e1725");
    s.transferHistoryText   = fromARGBHex("ffe2ebf4");
    s.spottingBG            = fromARGBHex("ff0e1725");
    s.spottingText          = fromARGBHex("ffe2ebf4");
    s.diagnosticOverlayBG   = fromARGBHex("ea101b29");
    s.diagnosticOverlayText = fromARGBHex("ffe2ebf4");
    s.diagnosticOverlayAccent= fromARGBHex("ff00bcd3");
    s.diagnosticOverlayMutedText = fromARGBHex("ff8a939e");
    s.tabBG                 = fromARGBHex("ff0b131f");
    s.volumeColor           = fromARGBHex("ff31c750");
    s.volumeClipColor       = fromARGBHex("ffff3333");
    // Re-sync preset dial tokens that depend on overridden modeCapsuleActiveBG
    s.presetDialCardActiveBg  = s.modeCapsuleActiveBG;
    s.presetDialAccent        = MixThemeColor(s.interfaceText, s.modeCapsuleActiveBG, 0.30f);

// Sidebar Activity Bar (Ocean Blue)
    s.sidebarRailBG          = fromARGBHex("ff080d14");
    s.sidebarRailDivider     = fromARGBHex("2600bcd3");
    s.sidebarIconInactive    = fromARGBHex("ff506680");
    s.sidebarIconHover       = fromARGBHex("ff80aacc");
    s.sidebarIconActive      = fromARGBHex("ff00bcd3");
    s.sidebarActiveStrip     = fromARGBHex("ff00bcd3");
    s.sidebarHeaderBG        = s.sidebarRailBG;
    s.sidebarStatusBarBG     = fromARGBHex("ff0d1522"); // filterBrowserBG(#0E1725) × 0.925
    s.sidebarSubHeaderLine   = fromARGBHex("2600bcd3");

    return s;
}

ThemeColorScheme ThemeColors::getSunsetWarm() {
    ThemeColorScheme s;
    s.mainWindowBG       = ImVec4(0.12f, 0.06f, 0.04f, 1.0f);
    s.menuText           = ImVec4(0.98f, 0.90f, 0.82f, 1.0f);
    s.toolbarBG          = ImVec4(0.16f, 0.08f, 0.06f, 1.0f);
    s.timecodeText       = ImVec4(1.00f, 0.82f, 0.48f, 0.95f);
    s.timecodeTextHover  = ImVec4(1.00f, 0.88f, 0.60f, 1.0f);
    s.interfaceText      = ImVec4(0.92f, 0.82f, 0.72f, 1.0f);
    s.resultsBG          = ImVec4(0.14f, 0.07f, 0.05f, 1.0f);
    s.resultsText        = ImVec4(0.95f, 0.90f, 0.85f, 1.0f);
    s.resultsOutline     = ImVec4(0.30f, 0.18f, 0.12f, 1.0f);
    s.resultsSelection   = ImVec4(0.95f, 0.55f, 0.15f, 1.0f);
    s.resultsScrollbar   = ImVec4(0.50f, 0.35f, 0.25f, 1.0f);
    s.resultsTabBG       = ImVec4(0.20f, 0.11f, 0.08f, 0.96f);
    s.resultsTabHover    = ImVec4(0.28f, 0.15f, 0.11f, 1.0f);
    s.resultsTabActive   = ImVec4(0.36f, 0.20f, 0.14f, 1.0f);
    s.resultsSecondaryText = ImVec4(0.78f, 0.66f, 0.58f, 1.0f);
    s.playlistGradientStart = ImVec4(0.95f, 0.55f, 0.15f, 1.0f);
    s.playlistGradientEnd = ImVec4(0.50f, 0.26f, 0.10f, 1.0f);
    s.metadataPanelBG    = ImVec4(0.14f, 0.08f, 0.06f, 1.0f);
    s.metadataPanelText  = ImVec4(0.95f, 0.90f, 0.85f, 1.0f);
    s.metadataPanelLabel = ImVec4(1.00f, 0.70f, 0.30f, 1.0f);
    s.metadataPanelScrollbar = ImVec4(0.45f, 0.30f, 0.20f, 1.0f);
    s.waveformBG         = ImVec4(0.10f, 0.05f, 0.03f, 1.0f);
    s.waveformColor      = ImVec4(0.95f, 0.55f, 0.15f, 1.0f);
    s.waveformSecondaryColor = ImVec4(1.00f, 0.70f, 0.30f, 1.0f);
    s.waveformPositionLine = ImVec4(1.00f, 0.88f, 0.50f, 1.0f);
    s.filterBrowserBG    = ImVec4(0.18f, 0.10f, 0.08f, 1.0f);
    s.filterBrowserText  = ImVec4(0.95f, 0.90f, 0.85f, 1.0f);
    s.filterBrowserHeading = ImVec4(1.00f, 0.80f, 0.50f, 1.0f);
    s.filterBrowserOutline = ImVec4(0.35f, 0.22f, 0.15f, 1.0f);
    s.filterBrowserSelection = ImVec4(0.90f, 0.50f, 0.10f, 1.0f);
    s.filterBrowserScrollbar = ImVec4(0.50f, 0.35f, 0.25f, 1.0f);
    s.filterBrowserSectionBG = ImVec4(0.22f, 0.13f, 0.09f, 1.0f);
    s.filterBrowserButtonBG = ImVec4(0.28f, 0.16f, 0.11f, 1.0f);
    s.filterBrowserTreeText = ImVec4(0.98f, 0.92f, 0.88f, 1.0f);
    s.filterBrowserTreeSelection = ImVec4(0.90f, 0.50f, 0.10f, 1.0f);
    s.transferButtonBG   = ImVec4(0.60f, 0.30f, 0.10f, 1.0f);
    s.searchButtonBG     = ImVec4(0.85f, 0.40f, 0.10f, 1.0f);
    s.transferHistoryBG  = ImVec4(0.18f, 0.10f, 0.08f, 1.0f);
    s.transferHistoryText = ImVec4(0.80f, 0.70f, 0.60f, 1.0f);
    s.spottingBG         = ImVec4(0.18f, 0.10f, 0.08f, 1.0f);
    s.spottingText       = ImVec4(0.90f, 0.82f, 0.72f, 1.0f);
    s.tabBG              = ImVec4(0.30f, 0.18f, 0.12f, 1.0f);

// Sidebar Activity Bar (Sunset Warm)
    s.sidebarRailBG          = fromARGBHex("ff23140f");
    s.sidebarRailDivider     = fromARGBHex("26e68019");
    s.sidebarIconInactive    = fromARGBHex("ff8a726a");
    s.sidebarIconHover       = fromARGBHex("ffc2a59b");
    s.sidebarIconActive      = fromARGBHex("ffe68019");
    s.sidebarActiveStrip     = fromARGBHex("ffe68019");
    s.sidebarHeaderBG        = s.sidebarRailBG;
    s.sidebarStatusBarBG     = fromARGBHex("ff2a1812"); // filterBrowserBG(#2E1A14) × 0.925
    s.sidebarSubHeaderLine   = fromARGBHex("26e68019");

    ApplySemanticThemeDefaults(s);
    return s;
}

ThemeColorScheme ThemeColors::getSpaceGray() {
    ThemeColorScheme s;
    s.mainWindowBG       = ImVec4(0.18f, 0.18f, 0.18f, 1.0f);
    s.menuText           = ImVec4(0.90f, 0.90f, 0.90f, 1.0f);
    s.toolbarBG          = ImVec4(0.22f, 0.22f, 0.22f, 1.0f);
    s.timecodeText       = ImVec4(0.72f, 0.86f, 0.98f, 0.95f);
    s.timecodeTextHover  = ImVec4(0.82f, 0.92f, 1.00f, 1.0f);
    s.interfaceText      = ImVec4(0.82f, 0.82f, 0.82f, 1.0f);
    s.resultsBG          = ImVec4(0.20f, 0.20f, 0.20f, 1.0f);
    s.resultsText        = ImVec4(0.88f, 0.88f, 0.88f, 1.0f);
    s.resultsOutline     = ImVec4(0.32f, 0.32f, 0.32f, 1.0f);
    s.resultsSelection   = ImVec4(0.30f, 0.55f, 0.80f, 1.0f);
    s.resultsScrollbar   = ImVec4(0.45f, 0.45f, 0.45f, 1.0f);
    s.resultsTabBG       = ImVec4(0.24f, 0.24f, 0.24f, 0.96f);
    s.resultsTabHover    = ImVec4(0.30f, 0.30f, 0.30f, 1.0f);
    s.resultsTabActive   = ImVec4(0.38f, 0.38f, 0.38f, 1.0f);
    s.resultsSecondaryText = ImVec4(0.68f, 0.68f, 0.68f, 1.0f);
    s.playlistGradientStart = ImVec4(0.30f, 0.55f, 0.80f, 1.0f);
    s.playlistGradientEnd = ImVec4(0.20f, 0.32f, 0.48f, 1.0f);
    s.metadataPanelBG    = ImVec4(0.22f, 0.22f, 0.22f, 1.0f);
    s.metadataPanelText  = ImVec4(0.85f, 0.85f, 0.85f, 1.0f);
    s.metadataPanelLabel = ImVec4(0.55f, 0.75f, 0.95f, 1.0f);
    s.metadataPanelScrollbar = ImVec4(0.40f, 0.40f, 0.40f, 1.0f);
    s.waveformBG         = ImVec4(0.15f, 0.15f, 0.15f, 1.0f);
    s.waveformColor      = ImVec4(0.35f, 0.65f, 0.35f, 1.0f);
    s.waveformSecondaryColor = ImVec4(0.55f, 0.75f, 0.95f, 1.0f);
    s.waveformPositionLine = ImVec4(1.00f, 0.86f, 0.30f, 1.0f);
    s.filterBrowserBG    = ImVec4(0.24f, 0.24f, 0.24f, 1.0f);
    s.filterBrowserText  = ImVec4(0.85f, 0.85f, 0.85f, 1.0f);
    s.filterBrowserHeading = ImVec4(0.75f, 0.75f, 0.75f, 1.0f);
    s.filterBrowserOutline = ImVec4(0.35f, 0.35f, 0.35f, 1.0f);
    s.filterBrowserSelection = ImVec4(0.30f, 0.55f, 0.80f, 1.0f);
    s.filterBrowserScrollbar = ImVec4(0.45f, 0.45f, 0.45f, 1.0f);
    s.filterBrowserSectionBG = ImVec4(0.28f, 0.28f, 0.28f, 1.0f);
    s.filterBrowserButtonBG = ImVec4(0.30f, 0.30f, 0.30f, 1.0f);
    s.filterBrowserTreeText = ImVec4(0.88f, 0.88f, 0.88f, 1.0f);
    s.filterBrowserTreeSelection = ImVec4(0.30f, 0.55f, 0.80f, 1.0f);
    s.transferButtonBG   = ImVec4(0.25f, 0.55f, 0.30f, 1.0f);
    s.searchButtonBG     = ImVec4(0.30f, 0.55f, 0.80f, 1.0f);
    s.transferHistoryBG  = ImVec4(0.25f, 0.25f, 0.25f, 1.0f);
    s.transferHistoryText = ImVec4(0.70f, 0.70f, 0.70f, 1.0f);
    s.spottingBG         = ImVec4(0.25f, 0.25f, 0.25f, 1.0f);
    s.spottingText       = ImVec4(0.82f, 0.82f, 0.82f, 1.0f);
    s.tabBG              = ImVec4(0.28f, 0.28f, 0.28f, 1.0f);
    ApplySemanticThemeDefaults(s);
    return s;
}

ThemeColorScheme ThemeColors::getElectricBlue() {
    ThemeColorScheme s;
    // ── Main Window ── (深蓝灰基调)
    s.mainWindowBG         = ImVec4(0.071f, 0.078f, 0.102f, 1.0f);  // ff12141a
    s.menuText             = ImVec4(0.965f, 0.969f, 0.976f, 1.0f);  // fff6f7f9
    s.toolbarBG            = ImVec4(0.118f, 0.125f, 0.157f, 1.0f);  // ff1e2028
    s.timecodeText         = ImVec4(0.400f, 0.969f, 0.831f, 0.95f); // ff66f7d4
    s.timecodeTextHover    = ImVec4(0.541f, 0.984f, 0.878f, 1.0f);  // ff8afbe0
    s.interfaceText        = ImVec4(0.796f, 0.835f, 0.882f, 1.0f);  // ffcbd5e1

// Results List
    s.resultsBG            = ImVec4(0.059f, 0.067f, 0.082f, 1.0f);  // ff0f1115
    s.resultsText          = ImVec4(1.000f, 1.000f, 1.000f, 1.0f);  // ffffffff
    s.resultsOutline       = ImVec4(0.176f, 0.192f, 0.239f, 1.0f);  // ff2d313d
    s.resultsSelection     = ImVec4(0.231f, 0.510f, 0.965f, 1.0f);  // ff3b82f6
    s.resultsScrollbar     = ImVec4(0.200f, 0.255f, 0.333f, 1.0f);  // ff334155
    s.resultsTabBG         = ImVec4(0.118f, 0.125f, 0.157f, 0.96f); // ff1e2028
    s.resultsTabHover      = ImVec4(0.153f, 0.196f, 0.282f, 1.0f);  // ff273249
    s.resultsTabActive     = ImVec4(0.192f, 0.267f, 0.396f, 1.0f);  // ff314465
    s.resultsSecondaryText = ImVec4(0.580f, 0.639f, 0.722f, 1.0f);  // ff94a3b8
    s.playlistGradientStart = ImVec4(0.231f, 0.510f, 0.965f, 1.0f); // ff3b82f6
    s.playlistGradientEnd = ImVec4(0.102f, 0.243f, 0.471f, 1.0f);   // ff1a3e78

// Metadata Panel
    s.metadataPanelBG      = ImVec4(0.094f, 0.102f, 0.125f, 1.0f);  // ff181a20
    s.metadataPanelText    = ImVec4(0.886f, 0.910f, 0.941f, 1.0f);  // ffe2e8f0
    s.metadataPanelLabel   = ImVec4(0.024f, 0.714f, 0.831f, 1.0f);  // ff06b6d4
    s.metadataPanelScrollbar = ImVec4(0.176f, 0.192f, 0.239f, 1.0f);// ff2d313d

// Waveform
    s.waveformBG           = ImVec4(0.031f, 0.035f, 0.043f, 1.0f);  // ff08090b
    s.waveformColor        = ImVec4(0.231f, 0.510f, 0.965f, 1.0f);  // ff3b82f6
    s.waveformSecondaryColor = ImVec4(0.024f, 0.714f, 0.831f, 1.0f); // ff06b6d4
    s.waveformPositionLine = ImVec4(1.000f, 0.859f, 0.302f, 1.0f);  // ffffdb4d (琥珀黄)

// Filter Browser
    s.filterBrowserBG      = ImVec4(0.071f, 0.078f, 0.102f, 1.0f);  // ff12141a
    s.filterBrowserText    = ImVec4(0.580f, 0.639f, 0.722f, 1.0f);  // ff94a3b8
    s.filterBrowserHeading = ImVec4(1.000f, 1.000f, 1.000f, 1.0f);  // ffffffff
    s.filterBrowserOutline = ImVec4(0.176f, 0.192f, 0.239f, 1.0f);  // ff2d313d
    s.filterBrowserSelection = ImVec4(0.231f, 0.510f, 0.965f, 1.0f);// ff3b82f6
    s.filterBrowserScrollbar = ImVec4(0.176f, 0.192f, 0.239f, 1.0f);// ff2d313d
    s.filterBrowserSectionBG = ImVec4(0.102f, 0.118f, 0.149f, 1.0f);// ff1a1e26
    s.filterBrowserButtonBG = ImVec4(0.141f, 0.176f, 0.239f, 1.0f); // ff242d3d
    s.filterBrowserTreeText = ImVec4(0.886f, 0.910f, 0.941f, 1.0f); // ffe2e8f0
    s.filterBrowserTreeSelection = ImVec4(0.231f, 0.510f, 0.965f, 1.0f);// ff3b82f6

// Buttons
    s.transferButtonBG     = ImVec4(0.133f, 0.773f, 0.369f, 1.0f);  // ff22c55e (绿色)
    s.searchButtonBG       = ImVec4(0.231f, 0.510f, 0.965f, 1.0f);  // ff3b82f6

// Transfer History / Spotting
    s.transferHistoryBG    = ImVec4(0.071f, 0.078f, 0.102f, 1.0f);  // ff12141a
    s.transferHistoryText  = ImVec4(0.580f, 0.639f, 0.722f, 1.0f);  // ff94a3b8
    s.spottingBG           = ImVec4(0.094f, 0.102f, 0.125f, 1.0f);  // ff181a20
    s.spottingText         = ImVec4(0.580f, 0.639f, 0.722f, 1.0f);  // ff94a3b8

// Tab Bar
    s.tabBG                = ImVec4(0.118f, 0.125f, 0.157f, 1.0f);  // ff1e2028

// Volume / Level Meter
    s.volumeColor          = ImVec4(0.133f, 0.773f, 0.369f, 1.0f);  // ff22c55e (绿色)
    s.volumeClipColor      = ImVec4(0.937f, 0.267f, 0.267f, 1.0f);  // ffef4444

// Sidebar Activity Bar (Electric Blue)
    s.sidebarRailBG          = fromARGBHex("ff0b0d12");
    s.sidebarRailDivider     = fromARGBHex("263b82f6");
    s.sidebarIconInactive    = fromARGBHex("ff545a6e");
    s.sidebarIconHover       = fromARGBHex("ff8c96b3");
    s.sidebarIconActive      = fromARGBHex("ff3b82f6");
    s.sidebarActiveStrip     = fromARGBHex("ff3b82f6");
    s.sidebarHeaderBG        = s.sidebarRailBG;
    s.sidebarStatusBarBG     = fromARGBHex("ff111317"); // filterBrowserBG(#12141A) × 0.925
    s.sidebarSubHeaderLine   = fromARGBHex("263b82f6");

    ApplySemanticThemeDefaults(s);
    return s;
}

ThemeColorScheme ThemeColors::getPowderedPastels() {
    ThemeColorScheme s;
    s.mainWindowBG            = fromARGBHex("fff8f3ee");
    s.menuBarBG               = fromARGBHex("fff1ebe6");
    s.menuText                = fromARGBHex("ff555b63");
    s.toolbarBG               = fromARGBHex("ffebe3dd");
    s.toolbarAccent           = fromARGBHex("ffd8c7db");
    s.transportBarBG          = fromARGBHex("ffe5ddd8");
    s.timecodeText            = fromARGBHex("ff90a6b6");
    s.timecodeTextHover       = fromARGBHex("ff7d98ab");
    s.interfaceText           = fromARGBHex("ff5e646c");

    s.resultsBG               = fromARGBHex("fffaf6f2");
    s.resultsHeaderBG         = fromARGBHex("ffefe8e2");
    s.resultsText             = fromARGBHex("ff515761");
    s.resultsOutline          = fromARGBHex("ffd8cfc8");
    s.resultsSelection        = fromARGBHex("ffbdd0e3");
    s.resultsActiveBorder     = fromARGBHex("b07a9ab2");
    s.resultsScrollbar        = fromARGBHex("ffc6bec1");
    s.resultsTabBG            = fromARGBHex("fff2e5ea");
    s.resultsTabHover         = fromARGBHex("ffe8d7de");
    s.resultsTabActive        = fromARGBHex("ffe3c7d2");
    s.resultsSecondaryText    = fromARGBHex("ff878d97");
    s.resultsPlayedText       = fromARGBHex("ffbc99a7");
    s.playlistGradientStart   = fromARGBHex("ffb8c4d1");
    s.playlistGradientEnd     = fromARGBHex("ffe8cfc3");
    s.playlistGradientHighlight = fromARGBHex("fff5ebc8");
    s.playlistGradientShadow    = fromARGBHex("fff1ebe5");

    s.metadataPanelBG         = fromARGBHex("fff4eee8");
    s.metadataPanelText       = fromARGBHex("ff505660");
    s.metadataPanelLabel      = fromARGBHex("ffaebfd1");
    s.metadataPanelScrollbar  = fromARGBHex("ffc8c0c3");

    s.waveformBG              = fromARGBHex("fff7f1eb");
    s.waveformColor           = fromARGBHex("ffb1c4d8");
    s.waveformSecondaryColor  = fromARGBHex("ffd8c7db");
    s.waveformPositionLine    = fromARGBHex("ffe6d184");

    s.filterBrowserBG         = fromARGBHex("ffefe7e1");
    s.filterBrowserText       = fromARGBHex("ff565c65");
    s.filterBrowserHeading    = fromARGBHex("ff69646d");
    s.filterBrowserOutline    = fromARGBHex("ffd6cdc6");
    s.filterBrowserSelection  = fromARGBHex("ffc5d7e8");
    s.filterBrowserScrollbar  = fromARGBHex("ffc5bdc1");
    s.filterBrowserSectionBG  = fromARGBHex("ffe4dcd6");
    s.filterBrowserButtonBG   = fromARGBHex("ffddd5cf");
    s.filterBrowserTreeText   = fromARGBHex("ff555a63");
    s.filterBrowserTreeSelection = fromARGBHex("ffc5d7e8");
    s.filterBrowserLocateTint = fromARGBHex("ffcad3c0");

    s.transferButtonBG        = fromARGBHex("ffc6d7c4");
    s.searchButtonBG          = fromARGBHex("ffe4ccc0");
    s.transferHistoryBG       = fromARGBHex("ffe8dfda");
    s.transferHistoryText     = fromARGBHex("ff757b84");
    s.spottingBG              = fromARGBHex("ffefe6e0");
    s.spottingText            = fromARGBHex("ff6f747d");
    s.tabBG                   = fromARGBHex("ffe3dad5");
    s.volumeColor             = fromARGBHex("ffbfd0af");
    s.volumeClipColor         = fromARGBHex("ffd8a5a1");
    s.meterBg                 = fromARGBHex("ffe9e1da");
    s.meterBase               = fromARGBHex("ffb9c9aa");
    s.meterMid                = fromARGBHex("ffccd2a3");
    s.meterWarning            = fromARGBHex("ffe1c79a");
    s.meterClip               = fromARGBHex("ffd5a49d");
    s.meterScaleText          = fromARGBHex("ff9299a3");

    // Sidebar Activity Bar (Powdered Pastels light-theme variant)
    s.sidebarRailBG          = fromARGBHex("ffeadfd6");
    s.sidebarRailDivider     = fromARGBHex("0d000000");
    s.sidebarIconInactive    = fromARGBHex("ff8b7e74");
    s.sidebarIconHover       = fromARGBHex("ff5a4e44");
    s.sidebarIconActive      = fromARGBHex("ff4a6984");
    s.sidebarActiveStrip     = fromARGBHex("ff4a6984");
    s.sidebarHeaderBG        = s.sidebarRailBG;
    s.sidebarStatusBarBG     = fromARGBHex("ffddd5ce"); // filterBrowserBG(#EFE7E1) × 0.925
    s.sidebarSubHeaderLine   = fromARGBHex("26000000");

    ApplySemanticThemeDefaults(s);
    return s;
}

// JSON Export/Import (simple key-value format)

std::string ThemeColors::exportToJSON(const ThemeColorScheme& s) {
    std::ostringstream j;
    j << "{\n";
    j << "  \"mainWindowBG\": \"" << toARGBHex(s.mainWindowBG) << "\",\n";
    j << "  \"menuBarBG\": \"" << toARGBHex(s.menuBarBG) << "\",\n";
    j << "  \"menuText\": \"" << toARGBHex(s.menuText) << "\",\n";
    j << "  \"frameBgBase\": \"" << toARGBHex(s.frameBgBase) << "\",\n";
    j << "  \"chromeBorder\": \"" << toARGBHex(s.chromeBorder) << "\",\n";
    j << "  \"popupBG\": \"" << toARGBHex(s.popupBG) << "\",\n";
    j << "  \"menuPopupBG\": \"" << toARGBHex(s.menuPopupBG) << "\",\n";
    j << "  \"dialogBG\": \"" << toARGBHex(s.dialogBG) << "\",\n";
    j << "  \"dialogPanelBG\": \"" << toARGBHex(s.dialogPanelBG) << "\",\n";
    j << "  \"toolbarBG\": \"" << toARGBHex(s.toolbarBG) << "\",\n";
    j << "  \"historyCapsuleBG\": \"" << toARGBHex(s.historyCapsuleBG) << "\",\n";
    j << "  \"historyCapsuleActiveBG\": \"" << toARGBHex(s.historyCapsuleActiveBG) << "\",\n";
    j << "  \"modeCapsuleBG\": \"" << toARGBHex(s.modeCapsuleBG) << "\",\n";
    j << "  \"modeCapsuleActiveBG\": \"" << toARGBHex(s.modeCapsuleActiveBG) << "\",\n";
    j << "  \"toolbarAccent\": \"" << toARGBHex(s.toolbarAccent) << "\",\n";
    j << "  \"controlAccent\": \"" << toARGBHex(s.controlAccent) << "\",\n";
    j << "  \"transportBarBG\": \"" << toARGBHex(s.transportBarBG) << "\",\n";
    j << "  \"timecodeText\": \"" << toARGBHex(s.timecodeText) << "\",\n";
    j << "  \"timecodeTextHover\": \"" << toARGBHex(s.timecodeTextHover) << "\",\n";
    j << "  \"interfaceText\": \"" << toARGBHex(s.interfaceText) << "\",\n";
    j << "  \"resultsBG\": \"" << toARGBHex(s.resultsBG) << "\",\n";
    j << "  \"resultsHeaderBG\": \"" << toARGBHex(s.resultsHeaderBG) << "\",\n";
    j << "  \"resultsText\": \"" << toARGBHex(s.resultsText) << "\",\n";
    j << "  \"resultsOutline\": \"" << toARGBHex(s.resultsOutline) << "\",\n";
    j << "  \"resultsSelection\": \"" << toARGBHex(s.resultsSelection) << "\",\n";
    j << "  \"resultsActiveBorder\": \"" << toARGBHex(s.resultsActiveBorder) << "\",\n";
    j << "  \"resultsScrollbar\": \"" << toARGBHex(s.resultsScrollbar) << "\",\n";
    j << "  \"resultsTabBG\": \"" << toARGBHex(s.resultsTabBG) << "\",\n";
    j << "  \"resultsTabHover\": \"" << toARGBHex(s.resultsTabHover) << "\",\n";
    j << "  \"resultsTabActive\": \"" << toARGBHex(s.resultsTabActive) << "\",\n";
    j << "  \"resultsSecondaryText\": \"" << toARGBHex(s.resultsSecondaryText) << "\",\n";
    j << "  \"resultsPlayedText\": \"" << toARGBHex(s.resultsPlayedText) << "\",\n";
    j << "  \"resultsGradientStart\": \"" << toARGBHex(s.resultsGradientStart) << "\",\n";
    j << "  \"resultsGradientEnd\": \"" << toARGBHex(s.resultsGradientEnd) << "\",\n";
    j << "  \"playlistGradientStart\": \"" << toARGBHex(s.playlistGradientStart) << "\",\n";
    j << "  \"playlistGradientEnd\": \"" << toARGBHex(s.playlistGradientEnd) << "\",\n";
    j << "  \"playlistGradientHighlight\": \"" << toARGBHex(s.playlistGradientHighlight) << "\",\n";
    j << "  \"playlistGradientShadow\": \"" << toARGBHex(s.playlistGradientShadow) << "\",\n";
    j << "  \"metadataPanelBG\": \"" << toARGBHex(s.metadataPanelBG) << "\",\n";
    j << "  \"metadataPanelText\": \"" << toARGBHex(s.metadataPanelText) << "\",\n";
    j << "  \"metadataPanelLabel\": \"" << toARGBHex(s.metadataPanelLabel) << "\",\n";
    j << "  \"metadataPanelScrollbar\": \"" << toARGBHex(s.metadataPanelScrollbar) << "\",\n";
    j << "  \"coverWallBG\": \"" << toARGBHex(s.coverWallBG) << "\",\n";
    j << "  \"waveformBG\": \"" << toARGBHex(s.waveformBG) << "\",\n";
    j << "  \"waveformPanelBorder\": \"" << toARGBHex(s.waveformPanelBorder) << "\",\n";
    j << "  \"waveformSplitterBG\": \"" << toARGBHex(s.waveformSplitterBG) << "\",\n";
    j << "  \"waveformSplitterLine\": \"" << toARGBHex(s.waveformSplitterLine) << "\",\n";
    j << "  \"waveformFooterBG\": \"" << toARGBHex(s.waveformFooterBG) << "\",\n";
    j << "  \"waveformColor\": \"" << toARGBHex(s.waveformColor) << "\",\n";
    j << "  \"waveformSecondaryColor\": \"" << toARGBHex(s.waveformSecondaryColor) << "\",\n";
    j << "  \"waveformPositionLine\": \"" << toARGBHex(s.waveformPositionLine) << "\",\n";
    j << "  \"waveformSelectionDim\": \"" << toARGBHex(s.waveformSelectionDim) << "\",\n";
    j << "  \"waveformSelectionHearthTop\": \"" << toARGBHex(s.waveformSelectionHearthTop) << "\",\n";
    j << "  \"waveformSelectionHearthBot\": \"" << toARGBHex(s.waveformSelectionHearthBot) << "\",\n";
    j << "  \"waveformSelectionEdge\": \"" << toARGBHex(s.waveformSelectionEdge) << "\",\n";
    j << "  \"miniWaveColor\": \"" << toARGBHex(s.miniWaveColor) << "\",\n";
    j << "  \"miniWaveBG\": \"" << toARGBHex(s.miniWaveBG) << "\",\n";
    j << "  \"meterBg\": \"" << toARGBHex(s.meterBg) << "\",\n";
    j << "  \"meterBase\": \"" << toARGBHex(s.meterBase) << "\",\n";
    j << "  \"meterMid\": \"" << toARGBHex(s.meterMid) << "\",\n";
    j << "  \"meterWarning\": \"" << toARGBHex(s.meterWarning) << "\",\n";
    j << "  \"meterClip\": \"" << toARGBHex(s.meterClip) << "\",\n";
    j << "  \"meterScaleText\": \"" << toARGBHex(s.meterScaleText) << "\",\n";
    j << "  \"filterBrowserBG\": \"" << toARGBHex(s.filterBrowserBG) << "\",\n";
    j << "  \"filterBrowserText\": \"" << toARGBHex(s.filterBrowserText) << "\",\n";
    j << "  \"filterBrowserHeading\": \"" << toARGBHex(s.filterBrowserHeading) << "\",\n";
    j << "  \"filterBrowserOutline\": \"" << toARGBHex(s.filterBrowserOutline) << "\",\n";
    j << "  \"filterBrowserSelection\": \"" << toARGBHex(s.filterBrowserSelection) << "\",\n";
    j << "  \"filterBrowserScrollbar\": \"" << toARGBHex(s.filterBrowserScrollbar) << "\",\n";
    j << "  \"filterBrowserSectionBG\": \"" << toARGBHex(s.filterBrowserSectionBG) << "\",\n";
    j << "  \"filterBrowserButtonBG\": \"" << toARGBHex(s.filterBrowserButtonBG) << "\",\n";
    j << "  \"sidebarSubpaneBG\": \"" << toARGBHex(s.sidebarSubpaneBG) << "\",\n";
    j << "  \"sidebarSubpaneDivider\": \"" << toARGBHex(s.sidebarSubpaneDivider) << "\",\n";
    j << "  \"filterBrowserTreeText\": \"" << toARGBHex(s.filterBrowserTreeText) << "\",\n";
    j << "  \"filterBrowserTreeSelection\": \"" << toARGBHex(s.filterBrowserTreeSelection) << "\",\n";
    j << "  \"filterBrowserSearchHitText\": \"" << toARGBHex(s.filterBrowserSearchHitText) << "\",\n";
    j << "  \"filterBrowserLocateTint\": \"" << toARGBHex(s.filterBrowserLocateTint) << "\",\n";
    j << "  \"filterBrowserTagText\": \"" << toARGBHex(s.filterBrowserTagText) << "\",\n";
    j << "  \"transferButtonBG\": \"" << toARGBHex(s.transferButtonBG) << "\",\n";
    j << "  \"searchButtonBG\": \"" << toARGBHex(s.searchButtonBG) << "\",\n";
    j << "  \"transferHistoryBG\": \"" << toARGBHex(s.transferHistoryBG) << "\",\n";
    j << "  \"transferHistoryText\": \"" << toARGBHex(s.transferHistoryText) << "\",\n";
    j << "  \"spottingBG\": \"" << toARGBHex(s.spottingBG) << "\",\n";
    j << "  \"spottingText\": \"" << toARGBHex(s.spottingText) << "\",\n";
    j << "  \"diagnosticOverlayBG\": \"" << toARGBHex(s.diagnosticOverlayBG) << "\",\n";
    j << "  \"diagnosticOverlayText\": \"" << toARGBHex(s.diagnosticOverlayText) << "\",\n";
    j << "  \"diagnosticOverlayAccent\": \"" << toARGBHex(s.diagnosticOverlayAccent) << "\",\n";
    j << "  \"diagnosticOverlayMutedText\": \"" << toARGBHex(s.diagnosticOverlayMutedText) << "\",\n";
    j << "  \"tabBG\": \"" << toARGBHex(s.tabBG) << "\",\n";
    j << "  \"volumeColor\": \"" << toARGBHex(s.volumeColor) << "\",\n";
    j << "  \"volumeClipColor\": \"" << toARGBHex(s.volumeClipColor) << "\",\n";
    j << "  \"wwiseConnectOk\": \"" << toARGBHex(s.wwiseConnectOk) << "\",\n";
    j << "  \"wwiseConnectFail\": \"" << toARGBHex(s.wwiseConnectFail) << "\",\n";
    j << "  \"wwiseDiffNeutral\": \"" << toARGBHex(s.wwiseDiffNeutral) << "\",\n";
    j << "  \"wwiseDiffNew\": \"" << toARGBHex(s.wwiseDiffNew) << "\",\n";
    j << "  \"wwiseDiffReplaced\": \"" << toARGBHex(s.wwiseDiffReplaced) << "\",\n";
    j << "  \"wwiseDiffUnresolved\": \"" << toARGBHex(s.wwiseDiffUnresolved) << "\",\n";
    j << "  \"wwiseLeafBadge\": \"" << toARGBHex(s.wwiseLeafBadge) << "\",\n";
    j << "  \"wwiseParentBadge\": \"" << toARGBHex(s.wwiseParentBadge) << "\"\n";
    j << "}";
    return j.str();
}

bool ThemeColors::importFromJSON(const std::string& json, ThemeColorScheme& outScheme) {
    std::unordered_map<std::string, std::string> values;
    const std::regex pairPattern("\"([^\"]+)\"\\s*:\\s*\"([^\"]*)\"");
    for (std::sregex_iterator it(json.begin(), json.end(), pairPattern), end; it != end; ++it) {
        values[(*it)[1].str()] = (*it)[2].str();
    }

    auto findValue = [&](const std::string& key) -> std::string {
        auto it = values.find(key);
        return it != values.end() ? it->second : std::string{};
    };
    
    auto tryParse = [&](const std::string& key, ImVec4& target) {
        std::string val = findValue(key);
        if (!val.empty()) target = fromARGBHex(val);
    };
    auto hasValue = [&](const std::string& key) -> bool {
        return !findValue(key).empty();
    };
    
    tryParse("mainWindowBG", outScheme.mainWindowBG);
    tryParse("menuBarBG", outScheme.menuBarBG);
    tryParse("menuText", outScheme.menuText);
    tryParse("frameBgBase", outScheme.frameBgBase);
    tryParse("chromeBorder", outScheme.chromeBorder);
    tryParse("popupBG", outScheme.popupBG);
    tryParse("menuPopupBG", outScheme.menuPopupBG);
    tryParse("dialogBG", outScheme.dialogBG);
    tryParse("dialogPanelBG", outScheme.dialogPanelBG);
    tryParse("toolbarBG", outScheme.toolbarBG);
    tryParse("historyCapsuleBG", outScheme.historyCapsuleBG);
    tryParse("historyCapsuleActiveBG", outScheme.historyCapsuleActiveBG);
    tryParse("modeCapsuleBG", outScheme.modeCapsuleBG);
    tryParse("modeCapsuleActiveBG", outScheme.modeCapsuleActiveBG);
    tryParse("toolbarAccent", outScheme.toolbarAccent);
    tryParse("controlAccent", outScheme.controlAccent);
    tryParse("transportBarBG", outScheme.transportBarBG);
    tryParse("timecodeText", outScheme.timecodeText);
    tryParse("timecodeTextHover", outScheme.timecodeTextHover);
    tryParse("interfaceText", outScheme.interfaceText);
    tryParse("resultsBG", outScheme.resultsBG);
    tryParse("resultsHeaderBG", outScheme.resultsHeaderBG);
    tryParse("resultsText", outScheme.resultsText);
    tryParse("resultsOutline", outScheme.resultsOutline);
    tryParse("resultsSelection", outScheme.resultsSelection);
    tryParse("resultsActiveBorder", outScheme.resultsActiveBorder);
    tryParse("resultsScrollbar", outScheme.resultsScrollbar);
    tryParse("resultsTabBG", outScheme.resultsTabBG);
    tryParse("resultsTabHover", outScheme.resultsTabHover);
    tryParse("resultsTabActive", outScheme.resultsTabActive);
    tryParse("resultsSecondaryText", outScheme.resultsSecondaryText);
    tryParse("resultsPlayedText", outScheme.resultsPlayedText);
    tryParse("resultsGradientStart", outScheme.resultsGradientStart);
    tryParse("resultsGradientEnd", outScheme.resultsGradientEnd);
    tryParse("playlistGradientStart", outScheme.playlistGradientStart);
    tryParse("playlistGradientEnd", outScheme.playlistGradientEnd);
    tryParse("playlistGradientHighlight", outScheme.playlistGradientHighlight);
    tryParse("playlistGradientShadow", outScheme.playlistGradientShadow);
    tryParse("metadataPanelBG", outScheme.metadataPanelBG);
    tryParse("metadataPanelText", outScheme.metadataPanelText);
    tryParse("metadataPanelLabel", outScheme.metadataPanelLabel);
    tryParse("metadataPanelScrollbar", outScheme.metadataPanelScrollbar);
    tryParse("coverWallBG", outScheme.coverWallBG);
    tryParse("waveformBG", outScheme.waveformBG);
    tryParse("waveformPanelBorder", outScheme.waveformPanelBorder);
    tryParse("waveformSplitterBG", outScheme.waveformSplitterBG);
    tryParse("waveformSplitterLine", outScheme.waveformSplitterLine);
    tryParse("waveformFooterBG", outScheme.waveformFooterBG);
    tryParse("waveformColor", outScheme.waveformColor);
    tryParse("waveformSecondaryColor", outScheme.waveformSecondaryColor);
    tryParse("waveformPositionLine", outScheme.waveformPositionLine);
    tryParse("waveformSelectionDim", outScheme.waveformSelectionDim);
    tryParse("waveformSelectionHearthTop", outScheme.waveformSelectionHearthTop);
    tryParse("waveformSelectionHearthBot", outScheme.waveformSelectionHearthBot);
    tryParse("waveformSelectionEdge", outScheme.waveformSelectionEdge);
    tryParse("miniWaveColor", outScheme.miniWaveColor);
    tryParse("miniWaveBG", outScheme.miniWaveBG);
    tryParse("meterBg", outScheme.meterBg);
    tryParse("meterBase", outScheme.meterBase);
    tryParse("meterMid", outScheme.meterMid);
    tryParse("meterWarning", outScheme.meterWarning);
    tryParse("meterClip", outScheme.meterClip);
    tryParse("meterScaleText", outScheme.meterScaleText);
    tryParse("filterBrowserBG", outScheme.filterBrowserBG);
    tryParse("filterBrowserText", outScheme.filterBrowserText);
    tryParse("filterBrowserHeading", outScheme.filterBrowserHeading);
    tryParse("filterBrowserOutline", outScheme.filterBrowserOutline);
    tryParse("filterBrowserSelection", outScheme.filterBrowserSelection);
    tryParse("filterBrowserScrollbar", outScheme.filterBrowserScrollbar);
    tryParse("filterBrowserSectionBG", outScheme.filterBrowserSectionBG);
    tryParse("filterBrowserButtonBG", outScheme.filterBrowserButtonBG);
    tryParse("sidebarSubpaneBG", outScheme.sidebarSubpaneBG);
    tryParse("sidebarSubpaneDivider", outScheme.sidebarSubpaneDivider);
    tryParse("filterBrowserTreeText", outScheme.filterBrowserTreeText);
    tryParse("filterBrowserTreeSelection", outScheme.filterBrowserTreeSelection);
    tryParse("filterBrowserSearchHitText", outScheme.filterBrowserSearchHitText);
    tryParse("filterBrowserLocateTint", outScheme.filterBrowserLocateTint);
    tryParse("filterBrowserTagText", outScheme.filterBrowserTagText);
    tryParse("transferButtonBG", outScheme.transferButtonBG);
    tryParse("searchButtonBG", outScheme.searchButtonBG);
    tryParse("transferHistoryBG", outScheme.transferHistoryBG);
    tryParse("transferHistoryText", outScheme.transferHistoryText);
    tryParse("spottingBG", outScheme.spottingBG);
    tryParse("spottingText", outScheme.spottingText);
    tryParse("diagnosticOverlayBG", outScheme.diagnosticOverlayBG);
    tryParse("diagnosticOverlayText", outScheme.diagnosticOverlayText);
    tryParse("diagnosticOverlayAccent", outScheme.diagnosticOverlayAccent);
    tryParse("diagnosticOverlayMutedText", outScheme.diagnosticOverlayMutedText);
    tryParse("tabBG", outScheme.tabBG);
    tryParse("volumeColor", outScheme.volumeColor);
    tryParse("volumeClipColor", outScheme.volumeClipColor);
    // F-034 Wwise tokens — round-trip on JSON import. Missing keys fall through to
    // ApplySemanticThemeDefaults derivation (called by callers after importFromJSON).
    tryParse("wwiseConnectOk", outScheme.wwiseConnectOk);
    tryParse("wwiseConnectFail", outScheme.wwiseConnectFail);
    tryParse("wwiseDiffNeutral", outScheme.wwiseDiffNeutral);
    tryParse("wwiseDiffNew", outScheme.wwiseDiffNew);
    tryParse("wwiseDiffReplaced", outScheme.wwiseDiffReplaced);
    tryParse("wwiseDiffUnresolved", outScheme.wwiseDiffUnresolved);
    tryParse("wwiseLeafBadge", outScheme.wwiseLeafBadge);
    tryParse("wwiseParentBadge", outScheme.wwiseParentBadge);

    if (!hasValue("frameBgBase")) {
        outScheme.frameBgBase = outScheme.toolbarBG;
    }
    if (!hasValue("chromeBorder")) {
        outScheme.chromeBorder = outScheme.resultsOutline;
    }
    if (!hasValue("popupBG")) {
        outScheme.popupBG = ImVec4(outScheme.toolbarBG.x, outScheme.toolbarBG.y, outScheme.toolbarBG.z, 0.98f);
    }
    if (!hasValue("menuPopupBG")) {
        outScheme.menuPopupBG = outScheme.popupBG;
    }
    if (!hasValue("dialogBG")) {
        outScheme.dialogBG = MixThemeColor(outScheme.popupBG, outScheme.mainWindowBG, 0.08f);
    }
    if (!hasValue("dialogPanelBG")) {
        outScheme.dialogPanelBG = MixThemeColor(outScheme.dialogBG, outScheme.frameBgBase, 0.16f);
    }
    if (!hasValue("diagnosticOverlayBG")) {
        outScheme.diagnosticOverlayBG = ImVec4(
            outScheme.dialogPanelBG.x,
            outScheme.dialogPanelBG.y,
            outScheme.dialogPanelBG.z,
            0.92f);
    }
    if (!hasValue("diagnosticOverlayText")) {
        outScheme.diagnosticOverlayText = outScheme.interfaceText;
    }
    if (!hasValue("diagnosticOverlayAccent")) {
        outScheme.diagnosticOverlayAccent = outScheme.toolbarAccent;
    }
    if (!hasValue("diagnosticOverlayMutedText")) {
        outScheme.diagnosticOverlayMutedText = MixThemeColor(
            outScheme.diagnosticOverlayText,
            outScheme.diagnosticOverlayBG,
            0.42f);
    }
    if (!hasValue("historyCapsuleBG")) {
        outScheme.historyCapsuleBG = MixThemeColor(outScheme.toolbarBG, outScheme.mainWindowBG, 0.10f);
    }
    if (!hasValue("historyCapsuleActiveBG")) {
        outScheme.historyCapsuleActiveBG = MixThemeColor(outScheme.frameBgBase, ImVec4(1.0f, 1.0f, 1.0f, 1.0f), 0.72f);
    }
    if (!hasValue("modeCapsuleBG")) {
        outScheme.modeCapsuleBG = MixThemeColor(outScheme.toolbarBG, outScheme.mainWindowBG, 0.08f);
    }
    if (!hasValue("modeCapsuleActiveBG")) {
        outScheme.modeCapsuleActiveBG = MixThemeColor(outScheme.frameBgBase, ImVec4(1.0f, 1.0f, 1.0f, 1.0f), 0.72f);
    }
    if (!hasValue("toolbarAccent")) {
        outScheme.toolbarAccent = outScheme.resultsSelection;
    }
    if (!hasValue("controlAccent")) {
        outScheme.controlAccent = outScheme.toolbarAccent;
    }
    if (!hasValue("waveformPanelBorder")) {
        outScheme.waveformPanelBorder = MixThemeColor(
            outScheme.waveformBG,
            MixThemeColor(outScheme.chromeBorder, outScheme.controlAccent, 0.18f),
            0.44f);
    }
    if (!hasValue("waveformSplitterBG")) {
        outScheme.waveformSplitterBG = MixThemeColor(outScheme.resultsBG, outScheme.waveformBG, 0.18f);
    }
    if (!hasValue("waveformSplitterLine")) {
        outScheme.waveformSplitterLine = MixThemeColor(
            outScheme.waveformSplitterBG,
            MixThemeColor(outScheme.waveformPanelBorder, outScheme.controlAccent, 0.14f),
            0.24f);
    }
    if (!hasValue("meterBg")) {
        outScheme.meterBg = DeriveMeterBg(outScheme);
    }
    if (!hasValue("meterBase")) {
        outScheme.meterBase = DeriveMeterBase(outScheme, outScheme.meterBg);
    }
    if (!hasValue("meterWarning")) {
        outScheme.meterWarning = DeriveMeterWarning(outScheme, outScheme.meterBase);
    }
    if (!hasValue("meterMid")) {
        outScheme.meterMid = DeriveMeterMid(outScheme.meterBase, outScheme.meterWarning);
    }
    if (!hasValue("meterClip")) {
        outScheme.meterClip = DeriveMeterClip(outScheme);
    }
    if (!hasValue("meterScaleText")) {
        outScheme.meterScaleText = DeriveMeterScaleText(outScheme);
    }
    if (!hasValue("resultsGradientStart")) {
        outScheme.resultsGradientStart = outScheme.resultsBG;
    }
    if (!hasValue("resultsGradientEnd")) {
        outScheme.resultsGradientEnd = MixThemeColor(outScheme.resultsBG, outScheme.resultsSelection, 0.10f);
    }
    if (!hasValue("playlistGradientHighlight")) {
        outScheme.playlistGradientHighlight = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
    }
    if (!hasValue("playlistGradientShadow")) {
        outScheme.playlistGradientShadow = outScheme.resultsBG;
    }
    if (!hasValue("coverWallBG")) {
        outScheme.coverWallBG = outScheme.metadataPanelBG;
    }
    if (!hasValue("waveformFooterBG")) {
        outScheme.waveformFooterBG = MixThemeColor(outScheme.waveformBG, outScheme.mainWindowBG, 0.28f);
    }
    if (!hasValue("filterBrowserSearchHitText")) {
        outScheme.filterBrowserSearchHitText = MixThemeColor(outScheme.controlAccent, outScheme.filterBrowserTreeText, 0.25f);
    }
    if (!hasValue("filterBrowserLocateTint")) {
        outScheme.filterBrowserLocateTint = outScheme.transferButtonBG;
    }
    if (!hasValue("filterBrowserTagText")) {
        outScheme.filterBrowserTagText = MixThemeColor(outScheme.filterBrowserText, outScheme.controlAccent, 0.3f);
    }

    return true;
}

// 文件导入/导出 (F-016)

bool ThemeColors::saveThemeToFile(const std::string& pathUtf8, const ThemeColorScheme& scheme) {
    std::string json = exportToJSON(scheme);
    if (json.empty()) return false;
    return Platform::FileSystem::WriteFileText(pathUtf8, json);
}

bool ThemeColors::loadThemeFromFile(const std::string& pathUtf8, ThemeColorScheme& outScheme) {
    std::string json;
    if (!Platform::FileSystem::ReadFileText(pathUtf8, json)) return false;
    return importFromJSON(json, outScheme);
}

bool ThemeColors::isSystemDarkMode() {
    return Platform::IsSystemDarkMode();
}

const ImVec4& ThemeColors::peerPanelBG(const ThemeColorScheme& scheme) {
    return scheme.filterBrowserBG;
}

const ImVec4& ThemeColors::peerPanelHeading(const ThemeColorScheme& scheme) {
    return scheme.filterBrowserHeading;
}

const ImVec4& ThemeColors::peerPanelOutline(const ThemeColorScheme& scheme) {
    return scheme.filterBrowserOutline;
}

const ImVec4& ThemeColors::peerPanelScrollbar(const ThemeColorScheme& scheme) {
    return scheme.filterBrowserScrollbar;
}

const ImVec4& ThemeColors::menuBarFillBG(const ThemeColorScheme& scheme) {
    return scheme.menuBarBG;
}

const ImVec4& ThemeColors::popupMenuBG(const ThemeColorScheme& scheme) {
    return scheme.menuPopupBG;
}

const ImVec4& ThemeColors::popupComboBG(const ThemeColorScheme& scheme) {
    return scheme.menuPopupBG;
}

const ImVec4& ThemeColors::popupTooltipBG(const ThemeColorScheme& scheme) {
    return scheme.popupBG;
}

const ImVec4& ThemeColors::popupModalBG(const ThemeColorScheme& scheme) {
    return scheme.dialogBG;
}

const ImVec4& ThemeColors::popupModalPanelBG(const ThemeColorScheme& scheme) {
    return scheme.dialogPanelBG;
}

ImVec4 ThemeColors::popupSelectionBG(const ThemeColorScheme& scheme) {
    const bool light = isLightChromeTheme(scheme);
    ImVec4 accent = MixThemeColor(
        scheme.controlAccent, scheme.menuText, light ? 0.10f : 0.04f);
    accent.w = light ? 0.18f : 0.24f;
    return accent;
}

ImVec4 ThemeColors::popupSelectionHoveredBG(const ThemeColorScheme& scheme) {
    ImVec4 accent = popupSelectionBG(scheme);
    accent.w = isLightChromeTheme(scheme) ? 0.28f : 0.38f;
    return accent;
}

ImVec4 ThemeColors::popupSelectionActiveBG(const ThemeColorScheme& scheme) {
    ImVec4 accent = popupSelectionBG(scheme);
    accent.w = isLightChromeTheme(scheme) ? 0.40f : 0.56f;
    return accent;
}

ImVec4 ThemeColors::popupBorder(const ThemeColorScheme& scheme) {
    const bool light = isLightChromeTheme(scheme);
    ImVec4 border = MixThemeColor(
        scheme.chromeBorder,
        popupMenuBG(scheme),
        light ? 0.18f : 0.10f);
    border.w = light ? 0.62f : 0.84f;
    return border;
}

ImVec4 ThemeColors::popupShadow(const ThemeColorScheme& scheme) {
    const bool light = isLightChromeTheme(scheme);
    return ImVec4(0.0f, 0.0f, 0.0f, light ? 0.10f : 0.28f);
}

const ImVec4& ThemeColors::fileInfoPanelBG(const ThemeColorScheme& scheme) {
    return scheme.metadataPanelBG;
}

const ImVec4& ThemeColors::fileInfoPanelScrollbar(const ThemeColorScheme& scheme) {
    return scheme.metadataPanelScrollbar;
}

ImVec4 ThemeColors::infoPanelDockBG(const ThemeColorScheme& scheme) {
    auto mix = [](const ImVec4& a, const ImVec4& b, float t) -> ImVec4 {
        return ImVec4(
            a.x + (b.x - a.x) * t,
            a.y + (b.y - a.y) * t,
            a.z + (b.z - a.z) * t,
            a.w + (b.w - a.w) * t);
    };
    ImVec4 bg = fileInfoPanelBG(scheme);
    bg = mix(bg, peerPanelBG(scheme), 0.04f);
    bg = mix(bg, scheme.metadataPanelLabel, 0.018f);
    bg = mix(bg, scheme.metadataPanelText, 0.004f);
    bg.w = 1.0f;
    return bg;
}

ImVec4 ThemeColors::infoPanelDockEdge(const ThemeColorScheme& scheme) {
    auto mix = [](const ImVec4& a, const ImVec4& b, float t) -> ImVec4 {
        return ImVec4(
            a.x + (b.x - a.x) * t,
            a.y + (b.y - a.y) * t,
            a.z + (b.z - a.z) * t,
            a.w + (b.w - a.w) * t);
    };
    ImVec4 edge = mix(scheme.metadataPanelLabel, peerPanelOutline(scheme), 0.26f);
    edge = mix(edge, scheme.metadataPanelText, 0.055f);
    edge.w = 0.64f;
    return edge;
}

ImVec4 ThemeColors::infoPanelAccent(const ThemeColorScheme& scheme) {
    auto mix = [](const ImVec4& a, const ImVec4& b, float t) -> ImVec4 {
        return ImVec4(
            a.x + (b.x - a.x) * t,
            a.y + (b.y - a.y) * t,
            a.z + (b.z - a.z) * t,
            a.w + (b.w - a.w) * t);
    };
    ImVec4 accent = mix(scheme.metadataPanelLabel, scheme.filterBrowserHeading, 0.05f);
    accent = mix(accent, scheme.metadataPanelText, 0.02f);
    accent.w = 1.0f;
    return accent;
}

const ImVec4& ThemeColors::waveformPanelBG(const ThemeColorScheme& scheme) {
    return scheme.waveformBG;
}

ImVec4 ThemeColors::waveformFrameBorder(const ThemeColorScheme& scheme) {
    const ImVec4& bg = scheme.waveformBG;
    const float luminance = bg.x * 0.299f + bg.y * 0.587f + bg.z * 0.114f;
    const bool isLightBg = luminance >= 0.72f;
    if (isLightBg) {
        ImVec4 border = MixThemeColor(
            bg,
            MixThemeColor(scheme.waveformPanelBorder, scheme.chromeBorder, 0.30f),
            0.16f);
        border.w = 0.24f;
        return border;
    }
    const ImVec4 edgeTarget = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
    const float mixT = 0.12f;
    return ImVec4(
        bg.x + (edgeTarget.x - bg.x) * mixT,
        bg.y + (edgeTarget.y - bg.y) * mixT,
        bg.z + (edgeTarget.z - bg.z) * mixT,
        0.68f);
}

// Editable Properties (for UI color editor)

std::vector<ThemeColors::ColorProperty> ThemeColors::getEditableProperties() {
    // 10-D R-ruling: single source. The historic 88-key list (order kept)
    // projects editable rows from the theme editor schema; display copy resolves
    // through the localization ledger at the consumer (labelId).
    static const char* const kLegacyOrder[] = {
        "mainWindowBG",
        "menuBarBG",
        "menuText",
        "frameBgBase",
        "chromeBorder",
        "popupBG",
        "menuPopupBG",
        "dialogBG",
        "dialogPanelBG",
        "toolbarBG",
        "historyCapsuleBG",
        "historyCapsuleActiveBG",
        "modeCapsuleBG",
        "modeCapsuleActiveBG",
        "toolbarAccent",
        "controlAccent",
        "transportBarBG",
        "timecodeText",
        "timecodeTextHover",
        "interfaceText",
        "resultsBG",
        "resultsHeaderBG",
        "resultsText",
        "resultsOutline",
        "resultsSelection",
        "resultsActiveBorder",
        "resultsScrollbar",
        "resultsTabBG",
        "resultsTabHover",
        "resultsTabActive",
        "resultsSecondaryText",
        "resultsPlayedText",
        "resultsGradientStart",
        "resultsGradientEnd",
        "playlistGradientStart",
        "playlistGradientEnd",
        "playlistGradientHighlight",
        "playlistGradientShadow",
        "metadataPanelBG",
        "metadataPanelText",
        "metadataPanelLabel",
        "metadataPanelScrollbar",
        "coverWallBG",
        "waveformBG",
        "waveformPanelBorder",
        "waveformSplitterBG",
        "waveformSplitterLine",
        "waveformFooterBG",
        "waveformColor",
        "waveformSecondaryColor",
        "waveformPositionLine",
        "waveformSelectionDim",
        "waveformSelectionHearthTop",
        "waveformSelectionHearthBot",
        "waveformSelectionEdge",
        "miniWaveColor",
        "miniWaveBG",
        "meterBg",
        "meterBase",
        "meterMid",
        "meterWarning",
        "meterClip",
        "meterScaleText",
        "filterBrowserBG",
        "filterBrowserText",
        "filterBrowserHeading",
        "filterBrowserOutline",
        "filterBrowserSelection",
        "filterBrowserScrollbar",
        "filterBrowserSectionBG",
        "filterBrowserButtonBG",
        "filterBrowserTreeText",
        "filterBrowserTreeSelection",
        "filterBrowserSearchHitText",
        "filterBrowserLocateTint",
        "transferButtonBG",
        "searchButtonBG",
        "transferHistoryBG",
        "transferHistoryText",
        "spottingBG",
        "spottingText",
        "diagnosticOverlayBG",
        "diagnosticOverlayText",
        "diagnosticOverlayAccent",
        "diagnosticOverlayMutedText",
        "tabBG",
        "volumeColor",
        "volumeClipColor",
    };
    auto& s = s_currentScheme;
    std::vector<ColorProperty> out;
    out.reserve(sizeof(kLegacyOrder) / sizeof(kLegacyOrder[0]));
    size_t schemaCount = 0;
    const ThemeEditorItem* schema = GetThemeEditorItems(schemaCount);
    for (const char* key : kLegacyOrder) {
        for (size_t i = 0; i < schemaCount; ++i) {
            if (std::strcmp(schema[i].key, key) == 0) {
                out.push_back({schema[i].key, schema[i].labelId, &(s.*(schema[i].member))});
                break;
            }
        }
    }
    return out;
}


ThemeColorScheme ThemeColors::getDeepNavy() {
    ThemeColorScheme s;
    s.mainWindowBG             = fromARGBHex("ff000000");
    s.menuBarBG                = fromARGBHex("ff454545");
    s.menuText                 = fromARGBHex("ffffffff");
    s.frameBgBase              = fromARGBHex("ff152944");
    s.chromeBorder             = fromARGBHex("7a3c4854");
    s.popupBG                  = fromARGBHex("ff152944");
    s.menuPopupBG              = fromARGBHex("ff37526f");
    s.dialogBG                 = fromARGBHex("ff152944");
    s.dialogPanelBG            = fromARGBHex("fa788aa2");
    s.toolbarBG                = fromARGBHex("ff152944");
    s.historyCapsuleBG         = fromARGBHex("ff12243d");
    s.historyCapsuleActiveBG   = fromARGBHex("ffbdc3ca");
    s.modeCapsuleBG            = fromARGBHex("ff13253e");
    s.modeCapsuleActiveBG      = fromARGBHex("ffbdc3ca");
    s.toolbarAccent            = fromARGBHex("ffffffff");
    s.controlAccent            = fromARGBHex("ffffffff");
    s.transportBarBG           = fromARGBHex("ff000000");
    s.timecodeText             = fromARGBHex("ee00ffd7");
    s.timecodeTextHover        = fromARGBHex("ffccd62a");
    s.interfaceText            = fromARGBHex("ffe4e4e4");
    s.resultsBG                = fromARGBHex("ff2d3d55");
    s.resultsHeaderBG          = fromARGBHex("ff243043");
    s.resultsText              = fromARGBHex("ffffffff");
    s.resultsOutline           = fromARGBHex("7a3c4854");
    s.resultsSelection         = fromARGBHex("ffffffff");
    s.resultsActiveBorder      = fromARGBHex("c8d4a840");
    s.resultsScrollbar         = fromARGBHex("ff6180a4");
    s.resultsTabBG             = fromARGBHex("f02f68a4");
    s.resultsTabHover          = fromARGBHex("ff6486ad");
    s.resultsTabActive         = fromARGBHex("ff7f9fc9");
    s.resultsSecondaryText     = fromARGBHex("ffe3e3e3");
    s.resultsPlayedText        = fromARGBHex("ffffcc71");
    s.resultsGradientStart     = fromARGBHex("ff162132");
    s.resultsGradientEnd       = fromARGBHex("ff4d5767");
    s.playlistGradientStart    = fromARGBHex("ff56759a");
    s.playlistGradientEnd      = fromARGBHex("ff435b77");
    s.playlistGradientHighlight = fromARGBHex("ffffffff");
    s.playlistGradientShadow   = fromARGBHex("ff2d3d55");
    s.metadataPanelBG          = fromARGBHex("ff030303");
    s.metadataPanelText        = fromARGBHex("ffa4a4a4");
    s.metadataPanelLabel       = fromARGBHex("ffe2eeff");
    s.metadataPanelScrollbar   = fromARGBHex("ffb9b9b9");
    s.coverWallBG              = fromARGBHex("ff030303");
    s.waveformBG               = fromARGBHex("ff172029");
    s.waveformPanelBorder      = fromARGBHex("cf36485b");
    s.waveformSplitterBG       = fromARGBHex("ff293a52");
    s.waveformSplitterLine     = fromARGBHex("f533435a");
    s.waveformFooterBG         = fromARGBHex("ff102134");
    s.waveformColor            = fromARGBHex("ffe8f3ff");
    s.waveformSecondaryColor   = fromARGBHex("ff8ab1c1");
    s.waveformPositionLine     = fromARGBHex("ffffe49c");
    s.meterBg                  = fromARGBHex("ff1c334a");
    s.meterBase                = fromARGBHex("ff53916f");
    s.meterMid                 = fromARGBHex("ff83a880");
    s.meterWarning             = fromARGBHex("ffbac394");
    s.meterClip                = fromARGBHex("ffe27e7a");
    s.meterScaleText           = fromARGBHex("ffe3e3e3");
    s.filterBrowserBG          = fromARGBHex("ffe8edf3");
    s.filterBrowserText        = fromARGBHex("ff000000");
    s.filterBrowserHeading     = fromARGBHex("ffc9c5c5");
    s.filterBrowserOutline     = fromARGBHex("5e7f95ab");
    s.filterBrowserSelection   = fromARGBHex("ff6f88a4");
    s.filterBrowserScrollbar   = fromARGBHex("ff889bad");
    s.filterBrowserSectionBG   = fromARGBHex("ff7287a2");
    s.filterBrowserButtonBG    = fromARGBHex("ffbcc7d3");
    s.filterBrowserTreeText    = fromARGBHex("ff000000");
    s.filterBrowserTreeSelection = fromARGBHex("ff2d0b0b");
    s.filterBrowserLocateTint  = fromARGBHex("ff2e9080");
    s.transferButtonBG         = fromARGBHex("ff799bc0");
    s.searchButtonBG           = fromARGBHex("ffffffff");
    s.transferHistoryBG        = fromARGBHex("ff263243");
    s.transferHistoryText      = fromARGBHex("ffe6ecf3");
    s.spottingBG               = fromARGBHex("ff141b26");
    s.spottingText             = fromARGBHex("ffe6ecf3");
    s.diagnosticOverlayBG      = fromARGBHex("ea788aa2");
    s.diagnosticOverlayText    = fromARGBHex("ffe4e4e4");
    s.diagnosticOverlayAccent  = fromARGBHex("ffffffff");
    s.diagnosticOverlayMutedText = fromARGBHex("f6b6bec8");
    s.tabBG                    = fromARGBHex("ff37526f");
    s.volumeColor              = fromARGBHex("ff31c750");
    s.volumeClipColor          = fromARGBHex("ffff3333");
    ApplySemanticThemeDefaults(s);
    return s;
}

