#pragma once
// SoundManager - 主题模块
// Theme.h - Aris theme and color definitions + granular color system
// Aris-owned preset and custom color definitions.

#include "imgui.h"
#include "LocalizationGenerated.h"
#include <string>
#include <vector>
#include <unordered_map>

// @stage7-contract
// API contract: color globals and ThemeColors are process-owned presentation
// state. Reads are side-effect free; apply/update operations mutate ImGui theme
// state only on the UI thread. Callers must preserve the existing color-token
// meanings and must not retain references across theme replacement.

// Waveform color globals (defined in main.cpp, synced by applyToImGui)
extern ImU32 g_waveformColor;
extern ImU32 g_waveformBgColor;
extern ImU32 g_waveformRmsColor;
extern ImU32 g_miniWaveColor;
extern ImU32 g_miniWaveBgColor;

// Legacy color palette globals (backward compatible)

// Main colors
extern ImVec4 COLOR_BG_DARK;         // Main background
extern ImVec4 COLOR_BG_PANEL;        // Panel background
extern ImVec4 COLOR_TEXT;            // Normal text
extern ImVec4 COLOR_TEXT_DIM;        // Dimmed text
extern ImVec4 COLOR_ACCENT;          // Accent color (green)
extern ImVec4 COLOR_ACCENT_DIM;      // Dimmed accent
extern ImVec4 COLOR_SELECTED;        // Selected item (orange/yellow)
extern ImVec4 COLOR_WAVEFORM_BG;     // Waveform background
extern ImVec4 COLOR_WAVEFORM;        // Waveform color (green)

// Extended Color Scheme (granular theme colors)

struct ThemeColorScheme {
// Main Window
    ImVec4 mainWindowBG         = ImVec4(0.047f, 0.094f, 0.153f, 1.0f);  // 主窗口背景
    ImVec4 menuBarBG            = ImVec4(0.050f, 0.070f, 0.120f, 1.0f);  // 菜单栏背景
    ImVec4 menuText             = ImVec4(0.945f, 0.949f, 0.957f, 1.0f);  // 菜单文字
    ImVec4 frameBgBase          = ImVec4(0.082f, 0.094f, 0.204f, 1.0f);  // 全局控件底色
    ImVec4 chromeBorder         = ImVec4(0.180f, 0.176f, 0.176f, 1.0f);  // 全局边框/分隔线
    ImVec4 popupBG              = ImVec4(0.082f, 0.094f, 0.204f, 0.98f); // 菜单/弹层背景
    ImVec4 menuPopupBG          = ImVec4(0.082f, 0.094f, 0.204f, 0.98f); // 下拉菜单背景
    ImVec4 dialogBG             = ImVec4(0.082f, 0.094f, 0.204f, 0.98f); // 对话框外层背景
    ImVec4 dialogPanelBG        = ImVec4(0.102f, 0.114f, 0.224f, 1.0f);  // 对话框内层面板
    ImVec4 toolbarBG            = ImVec4(0.082f, 0.094f, 0.204f, 1.0f);  // 搜索工具栏背景(渐变)
    ImVec4 historyCapsuleBG     = ImVec4(0.082f, 0.094f, 0.204f, 1.0f);  // Back/Continue 胶囊外壳
    ImVec4 historyCapsuleActiveBG = ImVec4(0.980f, 0.982f, 0.988f, 1.0f); // Back/Continue 激活项背景
    ImVec4 modeCapsuleBG        = ImVec4(0.082f, 0.094f, 0.204f, 1.0f);  // 顶部模式切换胶囊外壳
    ImVec4 modeCapsuleActiveBG  = ImVec4(0.980f, 0.982f, 0.988f, 1.0f);  // 顶部模式切换激活项背景
    ImVec4 toolbarAccent        = ImVec4(0.518f, 1.000f, 0.667f, 1.0f);  // 工具栏点缀 / 状态胶囊
    ImVec4 controlAccent        = ImVec4(0.518f, 1.000f, 0.667f, 1.0f);  // 通用交互高亮
    ImVec4 transportBarBG       = ImVec4(0.060f, 0.075f, 0.110f, 1.0f);  // 播放控制栏背景
    ImVec4 timecodeText         = ImVec4(0.518f, 1.000f, 0.667f, 0.933f);  // 时间码文字
    ImVec4 timecodeTextHover    = ImVec4(0.518f, 1.000f, 0.667f, 1.0f);  // 时间码悬停文字
    ImVec4 interfaceText        = ImVec4(0.722f, 0.796f, 0.894f, 1.0f);  // 界面文字

// Results List
    ImVec4 resultsBG            = ImVec4(0.055f, 0.055f, 0.153f, 1.0f);  // 结果列表背景
    ImVec4 resultsHeaderBG      = ImVec4(0.060f, 0.082f, 0.140f, 1.0f);  // 标签栏/筛选条背景
    ImVec4 resultsText          = ImVec4(0.906f, 0.906f, 0.906f, 1.0f);  // 结果列表文字
    ImVec4 resultsOutline       = ImVec4(0.180f, 0.176f, 0.176f, 1.0f);  // 结果列表边框
    ImVec4 resultsSelection     = ImVec4(1.000f, 0.722f, 0.031f, 1.0f);  // 选中项颜色
    ImVec4 resultsActiveBorder  = ImVec4(0.850f, 0.650f, 0.150f, 0.85f); // 结果列表面板边框
    ImVec4 resultsScrollbar     = ImVec4(0.447f, 0.447f, 0.502f, 1.0f);  // 滚动条
    ImVec4 resultsTabBG         = ImVec4(0.110f, 0.145f, 0.208f, 0.96f); // 结果页签背景
    ImVec4 resultsTabHover      = ImVec4(0.152f, 0.212f, 0.306f, 1.0f);  // 结果页签悬停
    ImVec4 resultsTabActive     = ImVec4(0.188f, 0.267f, 0.380f, 1.0f);  // 结果页签激活
    ImVec4 resultsSecondaryText = ImVec4(0.620f, 0.690f, 0.780f, 1.0f);  // 结果列表弱信息文字
    ImVec4 resultsPlayedText    = ImVec4(0.950f, 0.800f, 0.520f, 1.0f);  // 已播放文件文字
    ImVec4 resultsGradientStart = ImVec4(0.055f, 0.055f, 0.153f, 1.0f);  // 中间结果区渐变左侧
    ImVec4 resultsGradientEnd   = ImVec4(0.120f, 0.120f, 0.210f, 1.0f);  // 中间结果区渐变右侧
    ImVec4 playlistGradientStart = ImVec4(1.000f, 0.722f, 0.031f, 1.0f); // 播放列表渐变起点
    ImVec4 playlistGradientEnd   = ImVec4(0.741f, 0.431f, 0.067f, 1.0f); // 播放列表渐变终点
    ImVec4 playlistGradientHighlight = ImVec4(1.000f, 1.000f, 1.000f, 1.0f); // 播放列表渐变顶部高光目标
    ImVec4 playlistGradientShadow    = ImVec4(0.055f, 0.055f, 0.153f, 1.0f); // 播放列表渐变底部回收目标

// Metadata Panel
    ImVec4 metadataPanelBG      = ImVec4(0.051f, 0.110f, 0.192f, 1.0f);  // 元数据面板背景
    ImVec4 metadataPanelText    = ImVec4(0.988f, 0.988f, 0.988f, 1.0f);  // 元数据面板文字
    ImVec4 metadataPanelLabel   = ImVec4(1.000f, 0.725f, 0.035f, 1.0f);  // 元数据面板标签
    ImVec4 metadataPanelScrollbar = ImVec4(0.361f, 0.361f, 0.439f, 1.0f);// 元数据面板滚动条
    ImVec4 coverWallBG          = ImVec4(0.051f, 0.110f, 0.192f, 1.0f);  // 封面墙网格底板

// Waveform
    ImVec4 waveformBG           = ImVec4(0.047f, 0.098f, 0.169f, 1.0f);  // 波形背景
    ImVec4 waveformPanelBorder  = ImVec4(0.320f, 0.360f, 0.430f, 1.0f);  // 波形模块外框
    ImVec4 waveformSplitterBG   = ImVec4(0.180f, 0.220f, 0.290f, 1.0f);  // 波形上沿分隔带
    ImVec4 waveformSplitterLine = ImVec4(0.280f, 0.320f, 0.390f, 1.0f);  // 波形上沿分隔细线
    ImVec4 waveformFooterBG     = ImVec4(0.047f, 0.098f, 0.169f, 1.0f);  // 波形底部信息条背景
    ImVec4 waveformColor        = ImVec4(0.820f, 0.573f, 0.125f, 1.0f);  // 主波形颜色
    ImVec4 waveformSecondaryColor = ImVec4(1.000f, 0.725f, 0.035f, 1.0f);  // 主波形辅助渐变色
    ImVec4 waveformPositionLine = ImVec4(1.000f, 1.000f, 0.000f, 1.0f);  // 播放位置线
    ImVec4 miniWaveColor        = ImVec4(0.820f, 0.573f, 0.125f, 1.0f);  // Mini 波形颜色
    ImVec4 miniWaveBG           = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);       // Mini 波形背景(0=透明)

// Waveform Selection Hearth (F-004-ext)
    ImVec4 waveformSelectionDim       = ImVec4(0.000f, 0.000f, 0.000f, 0.50f); // Non-selected region dim mask
    ImVec4 waveformSelectionHearthTop = ImVec4(1.000f, 0.784f, 0.471f, 0.28f); // 选区渐变顶部（Dark: 琥珀 255,200,120, 0.28）
    ImVec4 waveformSelectionHearthBot = ImVec4(1.000f, 0.510f, 0.235f, 0.28f); // 选区渐变底部（Dark: 暖橙 255,130,60, 0.28）
    ImVec4 waveformSelectionEdge      = ImVec4(1.000f, 0.863f, 0.549f, 1.00f); // 选区边界硬线（Dark: 金 255,220,140）

// Filter Browser
    ImVec4 filterBrowserBG      = ImVec4(0.153f, 0.153f, 0.251f, 1.0f);  // 筛选浏览器背景
    ImVec4 filterBrowserText    = ImVec4(0.988f, 0.988f, 0.988f, 1.0f);  // 筛选浏览器文字
    ImVec4 filterBrowserHeading = ImVec4(0.953f, 0.945f, 0.929f, 1.0f);  // 筛选浏览器标题
    ImVec4 filterBrowserOutline = ImVec4(1.000f, 1.000f, 1.000f, 1.0f);  // 筛选浏览器边框
    ImVec4 filterBrowserSelection = ImVec4(0.988f, 0.706f, 0.000f, 1.0f);// 筛选浏览器选中
    ImVec4 filterBrowserScrollbar = ImVec4(0.439f, 0.439f, 0.573f, 1.0f);// 筛选浏览器滚动条
    ImVec4 filterBrowserSectionBG = ImVec4(0.208f, 0.208f, 0.318f, 1.0f);// 筛选分组页签/折叠头
    ImVec4 filterBrowserButtonBG = ImVec4(0.243f, 0.243f, 0.353f, 1.0f); // 筛选区按钮
    ImVec4 filterBrowserTreeText = ImVec4(0.965f, 0.965f, 0.965f, 1.0f); // 文件夹树文字
    ImVec4 filterBrowserTreeSelection = ImVec4(0.988f, 0.706f, 0.000f, 1.0f); // 文件夹树选中
    ImVec4 filterBrowserSearchHitText = ImVec4(0.824f, 0.900f, 0.742f, 1.0f); // 文件夹树搜索命中
    ImVec4 filterBrowserLocateTint = ImVec4(0.196f, 0.784f, 0.314f, 1.0f); // 文件夹定位高亮点缀
    ImVec4 filterBrowserTagText = ImVec4(0.75f, 0.82f, 0.90f, 1.0f);     // 标签按钮文字

// Buttons
    ImVec4 transferButtonBG     = ImVec4(0.000f, 0.502f, 0.000f, 1.0f);  // 传输按钮背景
    ImVec4 searchButtonBG       = ImVec4(0.980f, 0.322f, 0.031f, 1.0f);  // 搜索按钮背景

// Transfer History / Spotting
    ImVec4 transferHistoryBG    = ImVec4(0.275f, 0.247f, 0.247f, 1.0f);  // 传输历史背景
    ImVec4 transferHistoryText  = ImVec4(0.761f, 0.749f, 0.749f, 1.0f);  // 传输历史文字
    ImVec4 spottingBG           = ImVec4(0.251f, 0.243f, 0.243f, 1.0f);  // Spotting背景
    ImVec4 spottingText         = ImVec4(0.918f, 0.878f, 0.878f, 1.0f);  // Spotting文字
    ImVec4 diagnosticOverlayBG  = ImVec4(0.102f, 0.114f, 0.224f, 0.92f); // 诊断悬浮窗背景
    ImVec4 diagnosticOverlayText = ImVec4(0.918f, 0.940f, 0.965f, 1.0f); // 诊断悬浮窗文字
    ImVec4 diagnosticOverlayAccent = ImVec4(0.518f, 1.000f, 0.667f, 1.0f); // 诊断悬浮窗强调
    ImVec4 diagnosticOverlayMutedText = ImVec4(0.620f, 0.690f, 0.780f, 1.0f); // 诊断悬浮窗弱文字

// Tab Bar
    ImVec4 tabBG                = ImVec4(0.827f, 0.827f, 0.827f, 1.0f);  // 标签页背景

// Volume / Level Meter
    ImVec4 volumeColor          = ImVec4(0.196f, 0.784f, 0.314f, 1.0f);  // 音量条颜色
    ImVec4 volumeClipColor      = ImVec4(1.000f, 0.200f, 0.200f, 1.0f);  // 削波指示颜色
    ImVec4 meterBg              = ImVec4(0.125f, 0.137f, 0.165f, 1.0f);  // 电平表背景
    ImVec4 meterBase            = ImVec4(0.420f, 0.620f, 0.420f, 1.0f);  // 电平表基础段
    ImVec4 meterMid             = ImVec4(0.650f, 0.720f, 0.430f, 1.0f);  // 电平表中段
    ImVec4 meterWarning         = ImVec4(0.835f, 0.760f, 0.470f, 1.0f);  // 电平表预警段
    ImVec4 meterClip            = ImVec4(0.820f, 0.500f, 0.470f, 1.0f);  // 电平表削波段
    ImVec4 meterScaleText       = ImVec4(0.560f, 0.620f, 0.700f, 1.0f);  // 电平表刻度文字

// Preset Dial (F-034)
    ImVec4 presetDialBg              = ImVec4(0.082f, 0.094f, 0.204f, 1.0f);  // 拨盘背景
    ImVec4 presetDialCardActiveBg    = ImVec4(0.980f, 0.982f, 0.988f, 1.0f);  // 选中卡片背景
    ImVec4 presetDialCardActiveText  = ImVec4(0.082f, 0.094f, 0.204f, 1.0f);  // 选中卡片文字
    ImVec4 presetDialCardInactiveText= ImVec4(0.620f, 0.690f, 0.780f, 1.0f);  // 未选中文字
    ImVec4 presetDialAccent          = ImVec4(0.518f, 1.000f, 0.667f, 1.0f);  // 中间指示线

// Sidebar Activity Bar (VS Code-style)
    // Rail = the 52px icon column; content header/body/status bar stack to its right.
    ImVec4 sidebarRailBG          = ImVec4(0.110f, 0.110f, 0.110f, 1.0f);  // 图标栏背景 (Classic Dark 默认)
    ImVec4 sidebarRailDivider     = ImVec4(1.000f, 1.000f, 1.000f, 0.052f);// 图标栏右侧 1px 分割线
    ImVec4 sidebarIconInactive    = ImVec4(0.502f, 0.502f, 0.502f, 1.0f);  // 未激活图标色
    ImVec4 sidebarIconHover       = ImVec4(0.702f, 0.702f, 0.702f, 1.0f);  // 悬停图标色
    ImVec4 sidebarIconActive      = ImVec4(0.298f, 0.702f, 0.298f, 1.0f);  // 激活图标色
    ImVec4 sidebarActiveStrip     = ImVec4(0.298f, 0.702f, 0.298f, 1.0f);  // 激活左侧 2px 高亮条
    ImVec4 sidebarHeaderBG        = ImVec4(0.110f, 0.110f, 0.110f, 1.0f);  // 内容区 header 背景 (= sidebarRailBG 同色)
    ImVec4 sidebarStatusBarBG     = ImVec4(0.142f, 0.142f, 0.233f, 1.0f);  // 底部状态栏背景 (filterBrowserBG × 0.925)
    ImVec4 sidebarSubpaneBG       = ImVec4(0.180f, 0.220f, 0.290f, 1.0f);  // related-paths / folder-tree middle subpane background
    ImVec4 sidebarSubpaneDivider  = ImVec4(0.280f, 0.320f, 0.390f, 1.0f);  // middle subpane divider / drag hint
    ImVec4 sidebarSubHeaderLine   = ImVec4(1.000f, 1.000f, 1.000f, 0.15f); // 子标题旁延伸横线

    // Wwise Transfer (F-034) semantic colors
    // Dedicated tokens avoid overloading generic accents like controlAccent.
    // Each shipped preset provides its own values so diff states stay distinct.
    ImVec4 wwiseConnectOk        = ImVec4(0.404f, 0.902f, 0.420f, 1.0f);  // 连接成功 + Result Imported
    ImVec4 wwiseConnectFail      = ImVec4(0.902f, 0.420f, 0.420f, 1.0f);  // 连接断开 + Result Failed
    ImVec4 wwiseDiffNeutral      = ImVec4(0.616f, 0.663f, 0.722f, 1.0f);  // NoChange / Result Skipped
    ImVec4 wwiseDiffNew          = ImVec4(0.522f, 0.902f, 0.541f, 1.0f);  // New 节点
    ImVec4 wwiseDiffReplaced     = ImVec4(1.000f, 0.722f, 0.302f, 1.0f);  // Replaced 节点
    ImVec4 wwiseDiffUnresolved   = ImVec4(1.000f, 0.482f, 0.431f, 1.0f);  // Unresolved 节点
    ImVec4 wwiseLeafBadge        = ImVec4(0.404f, 0.745f, 0.478f, 1.0f);  // 叶子层（接收音频）
    ImVec4 wwiseParentBadge      = ImVec4(0.471f, 0.596f, 0.753f, 1.0f);  // 父层
};

// Theme Color Scheme Manager

enum class LStr;  // 10-D fwd (Localization ledger id; AppState.h precedent)
class ThemeColors {
public:
    // 标记方案是否被用户自定义过（编辑器修改过）
    static bool& customized();

    // 获取当前活动的颜色方案
    static ThemeColorScheme& current();

    // 统一基础 ImGui style，避免 stock/custom/editor 三条路径发生漂移
    static void applyStyleBaseline(ImGuiStyle& style);

    // 激活/退出自定义主题状态，统一 current/customized 写入入口
    static void activateCustomScheme(const ThemeColorScheme& scheme);
    static void deactivateCustomTheme();
    static void armImportDiagnostics(
        const char* source,
        const std::string& pathUtf8,
        int startThemeValue,
        const char* startThemeLabel,
        bool livePreview);
    static bool takePendingRenderDiagnostics(
        std::string& sourceOut,
        std::string& startThemeLabelOut);
    static bool loadAndApplyThemeFile(
        const std::string& pathUtf8,
        ThemeColorScheme* loadedScheme = nullptr,
        bool applyNow = true,
        const char* source = nullptr,
        int startThemeValue = 0,
        const char* startThemeLabel = nullptr,
        bool livePreview = false);

    // After applyToImGui from JSON/file import: ImGui sizes need ScaleAllSizes and WebView may need CSS reinjection.
    static void markPendingUiChromeResync();
    static bool hasPendingUiChromeResync();
    static bool consumePendingUiChromeResync();
    static void discardPendingUiChromeResync();

    // 仅在真正的自定义主题或主题编辑器预览时回灌 current scheme
    static bool shouldApplyCurrentScheme(bool showThemeEditor, bool customThemeSelected);
    static bool isLightChromeTheme(const ThemeColorScheme& scheme);

    // 应用颜色方案到 ImGui
    static void applyToImGui();

    // Apply simple per-setting overrides onto an existing scheme so legacy
    // custom colors and ThemeColorScheme stay in sync.
    static void applySimpleCustomOverrides(
        ThemeColorScheme& scheme,
        const ImVec4& background,
        const ImVec4& text,
        const ImVec4& accent,
        const ImVec4& waveform,
        const ImVec4& volume);
    // Parse color from ARGB hex string (format: "ffRRGGBB")
    static ImVec4 fromARGBHex(const std::string& hex);
    
    // 导出为 ARGB hex 字符串
    static std::string toARGBHex(const ImVec4& color);
    
    // 预设颜色方案
    static int getPresetCount();
    static ThemeColorScheme getPresetByIndex(int presetIndex);
    static const char* getPresetDisplayNameZh(int presetIndex);
    static const char* getPresetDisplayNameEn(int presetIndex);
    static inline constexpr const char* const* kSettingsThemeDisplayNamesZh =
        kArisSettingsThemeDisplayNamesZh;
    static inline constexpr const char* const* kSettingsThemeDisplayNamesEn =
        kArisSettingsThemeDisplayNamesEn;
    static inline constexpr int kSettingsThemeSlotCount = kArisSettingsThemeSlotCount;
    static constexpr const char* getSettingsThemeDisplayNameZh(int themeSlotIndex) {
        return (themeSlotIndex < 0 || themeSlotIndex >= kSettingsThemeSlotCount)
            ? kArisSettingsThemeDisplayNamesZh[3]
            : kArisSettingsThemeDisplayNamesZh[themeSlotIndex];
    }
    static constexpr const char* getSettingsThemeDisplayNameEn(int themeSlotIndex) {
        return (themeSlotIndex < 0 || themeSlotIndex >= kSettingsThemeSlotCount)
            ? kArisSettingsThemeDisplayNamesEn[3]
            : kArisSettingsThemeDisplayNamesEn[themeSlotIndex];
    }
    static ThemeColorScheme getHarkenSearchProductDark();    // Canonical token-backed Aris product theme
    static ThemeColorScheme getStudioSlate();        // Studio Slate preset
    static ThemeColorScheme getClassicDark();          // 经典深色
    static ThemeColorScheme getOceanBlue();            // 海洋蓝
    static ThemeColorScheme getSunsetWarm();            // 日落暖色
    static ThemeColorScheme getSpaceGray();             // 太空灰 (Pro Tools 风格)
    static ThemeColorScheme getElectricBlue();           // 电光蓝 (匹配登录界面)
    static ThemeColorScheme getPowderedPastels();        // 柔雾粉彩（参考色卡）
    static ThemeColorScheme getDeepNavy();               // 深海蓝
    
    // 导入/导出为 JSON 格式字符串
    static std::string exportToJSON(const ThemeColorScheme& scheme);
    static bool importFromJSON(const std::string& json, ThemeColorScheme& outScheme);
    
// 文件导入/导出 (F-016)
    /// 导出主题到 JSON 文件（用于分享）
    static bool saveThemeToFile(const std::string& pathUtf8, const ThemeColorScheme& scheme);
    /// 从 JSON 文件导入主题
    static bool loadThemeFromFile(const std::string& pathUtf8, ThemeColorScheme& outScheme);

// 跟随系统深色/浅色模式 (F-016)
    /// 检测 Windows 系统是否为暗色模式
    static bool isSystemDarkMode();

    // 获取颜色属性名列表 (给 UI 编辑器用)
    struct ColorProperty {
        const char* name;
        LStr labelId;              // 10-D: ledger key; display resolves via Localization
        ImVec4* colorPtr;    // 指向当前方案中的颜色
    };
    static std::vector<ColorProperty> getEditableProperties();

    // 同级面板外壳统一令牌：筛选器 / 波形传输 / Info
    static const ImVec4& peerPanelBG(const ThemeColorScheme& scheme);
    static const ImVec4& peerPanelHeading(const ThemeColorScheme& scheme);
    static const ImVec4& peerPanelOutline(const ThemeColorScheme& scheme);
    static const ImVec4& peerPanelScrollbar(const ThemeColorScheme& scheme);
    static const ImVec4& menuBarFillBG(const ThemeColorScheme& scheme);
    static const ImVec4& popupMenuBG(const ThemeColorScheme& scheme);
    static const ImVec4& popupComboBG(const ThemeColorScheme& scheme);
    static const ImVec4& popupTooltipBG(const ThemeColorScheme& scheme);
    static const ImVec4& popupModalBG(const ThemeColorScheme& scheme);
    static const ImVec4& popupModalPanelBG(const ThemeColorScheme& scheme);
    static ImVec4 popupSelectionBG(const ThemeColorScheme& scheme);
    static ImVec4 popupSelectionHoveredBG(const ThemeColorScheme& scheme);
    static ImVec4 popupSelectionActiveBG(const ThemeColorScheme& scheme);
    static ImVec4 popupBorder(const ThemeColorScheme& scheme);
    static ImVec4 popupShadow(const ThemeColorScheme& scheme);
    static const ImVec4& fileInfoPanelBG(const ThemeColorScheme& scheme);
    static const ImVec4& fileInfoPanelScrollbar(const ThemeColorScheme& scheme);
    static ImVec4 infoPanelDockBG(const ThemeColorScheme& scheme);
    static ImVec4 infoPanelDockEdge(const ThemeColorScheme& scheme);
    static ImVec4 infoPanelAccent(const ThemeColorScheme& scheme);
    static const ImVec4& waveformPanelBG(const ThemeColorScheme& scheme);
    static ImVec4 waveformFrameBorder(const ThemeColorScheme& scheme);
};

// Scoped style for modal/dialog surfaces. The colors are derived from the
// active ThemeColorScheme, so each shipped preset keeps its own palette.
class ThemeDialogStyleScope {
public:
    explicit ThemeDialogStyleScope(bool layeredNavy = false);
    ~ThemeDialogStyleScope();

    void reset();

    ThemeDialogStyleScope(const ThemeDialogStyleScope&) = delete;
    ThemeDialogStyleScope& operator=(const ThemeDialogStyleScope&) = delete;

private:
    int m_colorCount = 0;
    int m_varCount = 0;
};

// Theme Application (Legacy + Enhanced)

// Font Loading

// Load Chinese font (SimHei or embedded font)
void LoadChineseFont(ImGuiIO& io, float chromePixelSize, float readingPixelSize, float listPixelSize);
