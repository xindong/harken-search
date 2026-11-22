// HarkenSearchThemeManager.cpp — 次世代音频管理站视觉系统
// 整合主题配色、ImGui 样式、波形渲染的统一管理器

#include "HarkenSearchThemeManager.h"
#include "ThemeModuleColors.h"
#include <algorithm>

// 单例

HarkenSearchThemeManager& HarkenSearchThemeManager::instance() {
    static HarkenSearchThemeManager s_instance;
    return s_instance;
}

// 工具函数

ImU32 HarkenSearchThemeManager::toImU32(const ImVec4& color) {
    return IM_COL32(
        (int)(color.x * 255.0f),
        (int)(color.y * 255.0f),
        (int)(color.z * 255.0f),
        (int)(color.w * 255.0f)
    );
}

ImU32 HarkenSearchThemeManager::withAlpha(ImU32 color, int alpha) {
    return (color & 0x00FFFFFF) | ((ImU32)alpha << 24);
}

// Electric Blue 预设

ImGuiStyleConfig HarkenSearchThemeManager::getElectricBlueStyle() {
    ImGuiStyleConfig cfg;

    // 圆角 — 现代但不过分
    cfg.windowRounding    = 0.0f;    // 主窗全屏，不留黑边
    cfg.childRounding     = 6.0f;    // 子面板轻微圆角
    cfg.frameRounding     = 4.0f;    // 输入框/按钮圆角
    cfg.popupRounding     = 10.0f;
    cfg.scrollbarRounding = 12.0f;   // 滚动条圆润
    cfg.grabRounding      = 4.0f;
    cfg.tabRounding       = 4.0f;

    // 间距 — 呼吸感
    cfg.windowPadding     = ImVec2(12, 12);  // 留出呼吸感
    cfg.framePadding      = ImVec2(8, 5);    // 紧凑但易点击
    cfg.itemSpacing       = ImVec2(8, 6);    // 透气
    cfg.itemInnerSpacing  = ImVec2(4, 4);
    cfg.cellPadding       = ImVec2(8, 4);    // 宽敞的表头间距

    // 边框 — 靠色阶区分，非线条
    cfg.windowBorderSize  = 0.0f;
    cfg.childBorderSize   = 1.0f;    // 面板间极细分割线
    cfg.frameBorderSize   = 0.0f;    // 禁用硬边框
    cfg.popupBorderSize   = 0.0f;

    return cfg;
}

WaveformStyle HarkenSearchThemeManager::getElectricBlueWaveform() {
    WaveformStyle ws;
    ws.primaryColor     = IM_COL32(59, 130, 246, 255);   // #3B82F6 Electric Blue
    ws.secondaryColor   = IM_COL32(6, 182, 212, 255);    // #06B6D4 Cyan
    ws.rmsColor         = IM_COL32(6, 182, 212, 180);     // Cyan 半透明
    ws.backgroundColor  = IM_COL32(8, 9, 11, 255);        // #08090b (更新匹配 JSON)
    ws.playheadColor    = IM_COL32(255, 219, 77, 255);     // #FFDB4D 琥珀黄播放头
    ws.playheadGlowColor = IM_COL32(255, 219, 77, 40);    // 琥珀黄外发光
    ws.playheadDotColor = IM_COL32(255, 219, 77, 255);     // 琥珀黄顶部发光点
    ws.borderColor      = IM_COL32(45, 49, 61, 255);       // #2D313D (更新匹配 JSON)
    ws.emptyLineColor   = IM_COL32(45, 49, 61, 255);
    ws.emptyTextColor   = IM_COL32(100, 116, 139, 255);    // #64748B
    ws.cornerRadius     = 4.0f;
    ws.barGap           = 0.5f;
    ws.playheadWidth    = 1.0f;
    ws.playheadGlowWidth = 3.0f;
    ws.playheadDotRadius = 3.0f;
    ws.useGradient      = true;     // 双色垂直渐变
    return ws;
}

// 初始化

void HarkenSearchThemeManager::initialize(const ThemeColorScheme& scheme) {
    m_colorScheme  = scheme;
    m_styleConfig  = getElectricBlueStyle();
    m_waveformStyle = getElectricBlueWaveform();
    const ThemeModuleColors modules = ResolveThemeModuleColors(scheme);

    // 从 ThemeColorScheme 派生波形颜色
    m_waveformStyle.primaryColor    = toImU32(modules.waveform.primary);
    m_waveformStyle.backgroundColor = toImU32(modules.waveform.background);
    m_waveformStyle.borderColor     = toImU32(modules.results.outline);
    m_waveformStyle.secondaryColor  = toImU32(modules.waveform.secondary);
    m_waveformStyle.rmsColor        = withAlpha(toImU32(modules.waveform.secondary), 180);
    m_waveformStyle.playheadColor   = toImU32(modules.waveform.playhead);
    m_waveformStyle.playheadGlowColor = withAlpha(toImU32(modules.waveform.playhead), 40);
    m_waveformStyle.playheadDotColor  = toImU32(modules.waveform.playhead);
}

// 应用 ImGui 样式

void HarkenSearchThemeManager::applyImGuiStyle() {
    ImGuiStyle& style = ImGui::GetStyle();
    ThemeColors::applyStyleBaseline(style);

    // --- 应用尺寸/圆角参数 ---
    style.WindowRounding    = m_styleConfig.windowRounding;
    style.ChildRounding     = m_styleConfig.childRounding;
    style.FrameRounding     = m_styleConfig.frameRounding;
    style.PopupRounding     = m_styleConfig.popupRounding;
    style.ScrollbarRounding = m_styleConfig.scrollbarRounding;
    style.GrabRounding      = m_styleConfig.grabRounding;
    style.TabRounding       = m_styleConfig.tabRounding;

    style.WindowPadding     = m_styleConfig.windowPadding;
    style.FramePadding      = m_styleConfig.framePadding;
    style.ItemSpacing       = m_styleConfig.itemSpacing;
    style.ItemInnerSpacing  = m_styleConfig.itemInnerSpacing;
    style.CellPadding       = m_styleConfig.cellPadding;

    style.WindowBorderSize  = m_styleConfig.windowBorderSize;
    style.ChildBorderSize   = m_styleConfig.childBorderSize;
    style.FrameBorderSize   = m_styleConfig.frameBorderSize;
    style.PopupBorderSize   = m_styleConfig.popupBorderSize;
    style.WindowBorderHoverPadding = 4.0f;

    // --- 应用全量颜色映射 ---
    applyColorMapping(style.Colors);
}

// 全量颜色映射

void HarkenSearchThemeManager::applyColorMapping(ImVec4* colors) {
    const ThemeColorScheme& cs = m_colorScheme;

// 文本层级
    // 使用主题 interfaceText 而非硬编码白色，使浅色主题（RoseCedar/JadeMist）的深色文字生效。
    // TextDisabled 通过降 alpha 区分，避免与 Text 同色同亮度。
    colors[ImGuiCol_Text]                 = ImVec4(cs.interfaceText.x, cs.interfaceText.y, cs.interfaceText.z, 1.00f);
    colors[ImGuiCol_TextDisabled]         = ImVec4(cs.interfaceText.x, cs.interfaceText.y, cs.interfaceText.z, 0.55f);

// 窗口层级
    colors[ImGuiCol_WindowBg]             = cs.mainWindowBG;
    colors[ImGuiCol_ChildBg]              = ImVec4(0.00f, 0.00f, 0.00f, 0.00f); // 透明，依靠 WindowBg
    colors[ImGuiCol_PopupBg]              = cs.popupBG;
    colors[ImGuiCol_Border]               = cs.chromeBorder;
    colors[ImGuiCol_BorderShadow]         = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);

// 标题栏与菜单
    colors[ImGuiCol_TitleBg]              = cs.mainWindowBG;
    colors[ImGuiCol_TitleBgActive]        = cs.toolbarBG;
    colors[ImGuiCol_MenuBarBg]            = cs.toolbarBG;

// 控件背景 — 从 scheme 派生，略亮于主背景
    colors[ImGuiCol_FrameBg]              = ImVec4(cs.frameBgBase.x, cs.frameBgBase.y, cs.frameBgBase.z, 0.74f);
    colors[ImGuiCol_FrameBgHovered]       = ImVec4(cs.frameBgBase.x + 0.04f, cs.frameBgBase.y + 0.04f, cs.frameBgBase.z + 0.06f, 0.84f);
    colors[ImGuiCol_FrameBgActive]        = ImVec4(cs.frameBgBase.x + 0.06f, cs.frameBgBase.y + 0.06f, cs.frameBgBase.z + 0.08f, 0.90f);

// 按钮：默认低调，Hover/Active 从 scheme 派生
    colors[ImGuiCol_Button]               = ImVec4(cs.frameBgBase.x + 0.03f, cs.frameBgBase.y + 0.03f, cs.frameBgBase.z + 0.05f, 1.0f);
    colors[ImGuiCol_ButtonHovered]        = ImVec4(
        cs.frameBgBase.x + (cs.controlAccent.x - cs.frameBgBase.x) * 0.24f,
        cs.frameBgBase.y + (cs.controlAccent.y - cs.frameBgBase.y) * 0.24f,
        cs.frameBgBase.z + (cs.controlAccent.z - cs.frameBgBase.z) * 0.24f,
        0.92f);
    colors[ImGuiCol_ButtonActive]         = cs.controlAccent;

// 列表项与选中 (Header) — 从 scheme 派生
    colors[ImGuiCol_Header]               = ImVec4(cs.toolbarBG.x + 0.02f, cs.toolbarBG.y + 0.02f, cs.toolbarBG.z + 0.04f, 0.50f);
    colors[ImGuiCol_HeaderHovered]        = ImVec4(cs.resultsSelection.x, cs.resultsSelection.y, cs.resultsSelection.z, 0.40f);
    colors[ImGuiCol_HeaderActive]         = ImVec4(cs.resultsSelection.x, cs.resultsSelection.y, cs.resultsSelection.z, 0.80f);

// 表格 — 宽松表头样式
    colors[ImGuiCol_TableHeaderBg]        = cs.resultsHeaderBG;
    colors[ImGuiCol_TableBorderStrong]    = cs.chromeBorder;
    colors[ImGuiCol_TableBorderLight]     = ImVec4(cs.chromeBorder.x, cs.chromeBorder.y, cs.chromeBorder.z, 0.4f);
    // "暗光流"隔行 — 极深蓝代替白色斑马线
    colors[ImGuiCol_TableRowBg]           = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    colors[ImGuiCol_TableRowBgAlt]        = ImVec4(1.00f, 1.00f, 1.00f, 0.02f); // 极细微隔行

// 交互控件
    colors[ImGuiCol_CheckMark]            = cs.controlAccent;
    colors[ImGuiCol_SliderGrab]           = cs.resultsScrollbar;
    colors[ImGuiCol_SliderGrabActive]     = cs.controlAccent;

// 标签页 — 从 tabBG/resultsSelection 派生
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
    colors[ImGuiCol_TabUnfocused]         = cs.tabBG;
    colors[ImGuiCol_TabUnfocusedActive]   = ImVec4(cs.tabBG.x + 0.04f, cs.tabBG.y + 0.04f, cs.tabBG.z + 0.04f, 1.00f);

// 滚动条 — macOS 风格：默认极淡，hover 才显现
    colors[ImGuiCol_ScrollbarBg]          = ImVec4(0.00f, 0.00f, 0.00f, 0.00f); // 轨道完全透明
    colors[ImGuiCol_ScrollbarGrab]        = ImVec4(cs.resultsScrollbar.x, cs.resultsScrollbar.y, cs.resultsScrollbar.z, 0.40f);
    colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(cs.resultsScrollbar.x, cs.resultsScrollbar.y, cs.resultsScrollbar.z, 0.60f);
    colors[ImGuiCol_ScrollbarGrabActive]  = ImVec4(cs.controlAccent.x, cs.controlAccent.y, cs.controlAccent.z, 0.80f);

// ResizeGrip
    colors[ImGuiCol_ResizeGrip]           = ImVec4(cs.chromeBorder.x, cs.chromeBorder.y, cs.chromeBorder.z, 0.20f);
    colors[ImGuiCol_ResizeGripHovered]    = ImVec4(cs.controlAccent.x, cs.controlAccent.y, cs.controlAccent.z, 0.50f);
    colors[ImGuiCol_ResizeGripActive]     = ImVec4(cs.controlAccent.x, cs.controlAccent.y, cs.controlAccent.z, 0.80f);

// Separator — 默认隐藏，hover/active 时显示
    colors[ImGuiCol_Separator]            = ImVec4(cs.chromeBorder.x, cs.chromeBorder.y, cs.chromeBorder.z, 0.15f);
    colors[ImGuiCol_SeparatorHovered]     = ImVec4(cs.controlAccent.x, cs.controlAccent.y, cs.controlAccent.z, 0.80f);
    colors[ImGuiCol_SeparatorActive]      = cs.controlAccent;
}

// Neon 播放头绘制

void HarkenSearchThemeManager::drawNeonPlayhead(ImDrawList* drawList, float x, float yTop, float yBottom) {
    const auto& ws = m_waveformStyle;
    // 外发光层
    drawList->AddLine(ImVec2(x, yTop), ImVec2(x, yBottom), ws.playheadGlowColor, ws.playheadGlowWidth);
    // 主线
    drawList->AddLine(ImVec2(x, yTop), ImVec2(x, yBottom), ws.playheadColor, ws.playheadWidth);
    // 顶部发光点
    drawList->AddCircleFilled(ImVec2(x, yTop), ws.playheadDotRadius, ws.playheadDotColor);
}

// 迷你波形绘制 (文件列表中)

void HarkenSearchThemeManager::drawMiniWaveform(
    ImDrawList* drawList,
    ImVec2 pos, float width, float height,
    const float* peaks, int peakCount,
    float playbackPos)
{
    const auto& ws = m_waveformStyle;
    float padding = 2.0f;

    // 背景
    drawList->AddRectFilled(pos, ImVec2(pos.x + width, pos.y + height), ws.backgroundColor, ws.cornerRadius);

    if (peaks && peakCount > 0) {
        float innerWidth = width - padding * 2;
        float innerHeight = height - padding * 2;
        float centerY = pos.y + height / 2;

        int peaksToShow = std::min(peakCount, (int)innerWidth);
        float pixelsPerPeak = innerWidth / peaksToShow;
        float peaksPerPixel = (float)peakCount / peaksToShow;
        float barWidth = std::max(1.0f, pixelsPerPeak - ws.barGap);

        for (int i = 0; i < peaksToShow; i++) {
            int srcIndex = (int)(i * peaksPerPixel);
            if (srcIndex >= peakCount) srcIndex = peakCount - 1;

            float x = pos.x + padding + i * pixelsPerPeak;
            float peakHeight = peaks[srcIndex] * innerHeight * 0.95f;
            if (peakHeight < 1.0f) peakHeight = 1.0f;

            float barRadius = std::min(1.0f, barWidth / 2);
            ImVec2 p_min = ImVec2(x, centerY - peakHeight / 2);
            ImVec2 p_max = ImVec2(x + barWidth, centerY + peakHeight / 2);

            if (ws.useGradient) {
                // 双色垂直渐变：上蓝下青
                drawList->AddRectFilledMultiColor(p_min, p_max,
                    ws.primaryColor, ws.primaryColor,
                    ws.secondaryColor, ws.secondaryColor);
            } else {
                drawList->AddRectFilled(p_min, p_max, ws.primaryColor, barRadius);
            }
        }

        // 播放头
        if (playbackPos >= 0.0f) {
            float playX = pos.x + padding + playbackPos * innerWidth;
            drawNeonPlayhead(drawList, playX, pos.y + padding, pos.y + height - padding);
        }
    } else {
        // 无波形 — 中线
        float centerY = pos.y + height / 2;
        drawList->AddLine(ImVec2(pos.x + padding, centerY), ImVec2(pos.x + width - padding, centerY), ws.emptyLineColor);
    }

    // 边框
    drawList->AddRect(pos, ImVec2(pos.x + width, pos.y + height), ws.borderColor, ws.cornerRadius);
}

// 大波形面板绘制 (含 RMS 层)

void HarkenSearchThemeManager::drawWaveformPanel(
    ImDrawList* drawList,
    ImVec2 pos, float width, float height,
    const float* peaks, const float* rms, int peakCount,
    float playbackPos)
{
    const auto& ws = m_waveformStyle;

    // 背景
    drawList->AddRectFilled(pos, ImVec2(pos.x + width, pos.y + height), ws.backgroundColor, ws.cornerRadius);

    if (peaks && peakCount > 0) {
        float centerY = pos.y + height / 2;
        float pixelsPerPeak = width / peakCount;

        // 层1: RMS 填充 (半透明辅助色)
        if (rms) {
            for (int i = 0; i < peakCount && i * pixelsPerPeak < width; i++) {
                float x = pos.x + i * pixelsPerPeak;
                float rmsHeight = rms[i] * height * 0.8f;
                if (rmsHeight < 0.5f) continue;

                ImVec2 p_min = ImVec2(x, centerY - rmsHeight / 2);
                ImVec2 p_max = ImVec2(x + pixelsPerPeak, centerY + rmsHeight / 2);

                if (ws.useGradient) {
                    ImU32 rmsTop = ws.rmsColor;
                    ImU32 rmsBot = withAlpha(ws.secondaryColor, 100);
                    drawList->AddRectFilledMultiColor(p_min, p_max, rmsTop, rmsTop, rmsBot, rmsBot);
                } else {
                    drawList->AddRectFilled(p_min, p_max, ws.rmsColor);
                }
            }
        }

        // 层2: Peak 波形 (主色渐变)
        for (int i = 0; i < peakCount && i * pixelsPerPeak < width; i++) {
            float x = pos.x + i * pixelsPerPeak;
            float peakHeight = peaks[i] * height * 0.9f;
            if (peakHeight < 0.5f) continue;

            ImVec2 p_min = ImVec2(x, centerY - peakHeight / 2);
            ImVec2 p_max = ImVec2(x + pixelsPerPeak, centerY + peakHeight / 2);

            if (ws.useGradient) {
                drawList->AddRectFilledMultiColor(p_min, p_max,
                    ws.primaryColor, ws.primaryColor,
                    ws.secondaryColor, ws.secondaryColor);
            } else {
                drawList->AddRectFilled(p_min, p_max, ws.primaryColor);
            }
        }

        // 层3: Neon 播放头
        if (playbackPos >= 0.0f) {
            float playX = pos.x + playbackPos * width;
            drawNeonPlayhead(drawList, playX, pos.y, pos.y + height);
        }
    } else {
        // 无波形
        float centerY = pos.y + height / 2;
        drawList->AddLine(ImVec2(pos.x, centerY), ImVec2(pos.x + width, centerY), ws.emptyLineColor);

        const char* text = u8"\xe6\x9c\xaa\xe5\x8a\xa0\xe8\xbd\xbd\xe9\x9f\xb3\xe9\xa2\x91"; // "未加载音频"
        ImVec2 textSize = ImGui::CalcTextSize(text);
        drawList->AddText(
            ImVec2(pos.x + (width - textSize.x) / 2, centerY - textSize.y / 2),
            ws.emptyTextColor, text
        );
    }

    // 边框
    drawList->AddRect(pos, ImVec2(pos.x + width, pos.y + height), ws.borderColor, ws.cornerRadius);
}
