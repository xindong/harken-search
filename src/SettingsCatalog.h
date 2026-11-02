#pragma once
#include "Localization.h"
#include <algorithm>
#include <cctype>
#include <sstream>
#include <string>
#include <vector>
namespace harken::settings {
enum class Page { Common, Appearance, Audio, Search, Library, Interaction, Personal, Backup, Diagnostics };
struct Entry {
    std::string id; Page page; int group;
    std::string zh, en, ja, keywords;
    Entry(const char* identifier, Page category, int section, LStr label, const char* terms)
        : id(identifier), page(category), group(section),
          zh(GetLocalizedString(label, AppLanguage::Chinese)),
          en(GetLocalizedString(label, AppLanguage::English)),
          ja(GetLocalizedString(label, AppLanguage::Japanese)), keywords(terms) {}
    const char* Label(int language) const { return (language == 1 ? en : language == 2 ? ja : zh).c_str(); }
};
inline const std::vector<Entry>& Catalog() {
    static const std::vector<Entry> entries = {
        {u8"theme",Page::Appearance,0,LStr::SetUX_Theme,u8"主题 theme color 颜色"},
        {u8"automatic",Page::Appearance,0,LStr::ThemeSchedule_Automatic,u8"日夜 自动 白天 黑夜 schedule"},
        {u8"day",Page::Appearance,0,LStr::SetUX_Day,u8"白天 主题 day"},
        {u8"night",Page::Appearance,0,LStr::SetUX_Night,u8"黑夜 主题 night"},
        {u8"themeEditor",Page::Appearance,0,LStr::Set_EditTheme,u8"编辑 颜色 配色 自定义 custom"},
        {u8"language",Page::Appearance,0,LStr::Set_UILanguage,u8"中文 英文 日语 language"},
        {u8"systemFont",Page::Appearance,0,LStr::Set_SystemFontSize,u8"界面 字体 大小 font"},
        {u8"listFont",Page::Appearance,0,LStr::Set_ListFontSize,u8"列表 字体 大小 font"},
        {u8"rowHeight",Page::Appearance,0,LStr::Set_RowHeight,u8"密度 紧凑 行高"},
        {u8"extension",Page::Appearance,0,LStr::Set_ShowFileExt,u8"文件 扩展 后缀"},
        {u8"waveMode",Page::Appearance,1,LStr::Set_DefaultDisplayMode,u8"波形 音量 响度 频谱 waveform"},
        {u8"waveStyle",Page::Appearance,1,LStr::Set_PreviewWaveStyle,u8"波形 RMS 实体 waveform"},
        {u8"miniRms",Page::Appearance,1,LStr::Main_MiniRmsStyleCheck,u8"缩略 波形 RMS"},
        {u8"meterLayout",Page::Appearance,1,LStr::Main_VolHeaderChanLabel,u8"声道 命名 SMPTE Film Atmos AmbiX"},
        {u8"showLufs",Page::Appearance,1,LStr::Set_MeterLUFS,u8"LUFS 计量 响度"},
        {u8"showRms",Page::Appearance,1,LStr::Set_MeterRMS,u8"RMS 计量"},
        {u8"showPeak",Page::Appearance,1,LStr::Set_MeterTruePeak,u8"真峰值 计量 peak"},
        {u8"spectrumEnabled",Page::Appearance,1,LStr::Set_EnableHiResSpectrum,u8"频谱 spectrum"},
        {u8"fft",Page::Appearance,1,LStr::Set_FFTWindowSize,u8"频谱 窗口 FFT"},
        {u8"spectrumBlend",Page::Appearance,1,LStr::Set_DefaultBlendRatio,u8"频谱 混合 blend"},
        {u8"spectrumSplit",Page::Appearance,1,LStr::Set_SplitMode,u8"频谱 波形 分区"},
        {u8"ripple",Page::Appearance,2,LStr::Set_EnableRipple,u8"点击 波纹 效果"},
        {u8"milliseconds",Page::Appearance,2,LStr::Set_ShowMs,u8"时间 毫秒"},
        {u8"frames",Page::Appearance,2,LStr::Set_ShowFrame,u8"时间 帧数"},
        {u8"frameRate",Page::Appearance,2,LStr::Set_VideoFPS,u8"视频 帧率"},
        {u8"resetSize",Page::Appearance,2,LStr::Set_ResetDefaultSize,u8"恢复 默认 尺寸 布局"},
        {u8"device",Page::Audio,0,LStr::Set_AudioOutputDevice,u8"声卡 声音 耳机 没声音 output device"},
        {u8"listening",Page::Audio,0,LStr::Set_PreviewMode,u8"浏览 连续 试听"},
        {u8"volume",Page::Audio,0,LStr::Set_DefaultVolume,u8"音量 声音 volume"},
        {u8"normalize",Page::Audio,0,LStr::Set_EnableLoudnessNorm,u8"统一 试听 响度 归一 化 loudness normalize LUFS"},
        {u8"target",Page::Audio,0,LStr::Set_TargetLUFS,u8"参考 电平 响度 target LUFS"},
        {u8"fade",Page::Audio,0,LStr::Set_EnableFadeOnJump,u8"防爆音 淡入 淡出 点击 保护"},
        {u8"fadeMs",Page::Audio,3,LStr::Set_FadeTime,u8"淡入 淡出 时长"},
        {u8"period",Page::Audio,3,LStr::Set_PeriodSize,u8"音频 缓冲 周期 延迟 杂音 period"},
        {u8"skipSilence",Page::Audio,3,LStr::Set_SkipLeadingSilence,u8"开头 静音"},
        {u8"silenceThreshold",Page::Audio,3,LStr::SetUX_SilenceThreshold,u8"静音 判断 阈值 threshold"},
        {u8"pitch",Page::Audio,3,LStr::Set_DefaultPitch,u8"音高 半音 pitch"},
        {u8"pitchRange",Page::Audio,3,LStr::Set_PitchRange,u8"音高 范围 pitch"},
        {u8"monitor",Page::Audio,4,LStr::Set_MonitorLayout,u8"声道 路由 监听 layout"},
        {u8"downmix",Page::Audio,4,LStr::Set_DownmixStrategy,u8"声道 下混 策略 downmix"},
        {u8"center",Page::Audio,4,LStr::Set_DownmixCenterDb,u8"中置 声道 增益"},
        {u8"surround",Page::Audio,4,LStr::Set_DownmixSurroundDb,u8"环绕 声道 增益"},
        {u8"binaural",Page::Audio,4,LStr::Set_BinauralMonitor,u8"双耳 耳机 binaural"},
        {u8"deviceInfo",Page::Audio,4,LStr::Set_DeviceChannelCap,u8"设备 声道 能力"},
        {u8"searchMode",Page::Search,0,LStr::SetUX_SearchMode,u8"实时 输入 回车 搜索 realtime enter"},
        {u8"suggest",Page::Search,0,LStr::Set_SearchSuggestSource,u8"搜索 建议 联想 补全"},
        {u8"renameTab",Page::Search,0,LStr::Set_AutoRenameTab,u8"搜索 标签 命名"},
        {u8"dictionary",Page::Search,0,LStr::Set_ThesaurusTitle,u8"词库 同义词 触发 扩展 thesaurus"},
        {u8"perf",Page::Library,0,LStr::Set_PerfMode,u8"性能 内存 卡顿 慢 performance"},
        {u8"threads",Page::Library,0,LStr::Set_ScanThreads,u8"扫描 线程 并发"},
        {u8"startupScan",Page::Library,0,LStr::Main_AutoRescanCheck,u8"启动 自动 扫描 变化"},
        {u8"backfill",Page::Library,0,LStr::Main_IdleBackfillCheck,u8"空闲 波形 封面 缓存 补齐"},
        {u8"idleMinutes",Page::Library,0,LStr::SetUX_IdleMinutes,u8"空闲 等待 多久 分钟"},
        {u8"database",Page::Library,0,LStr::Set_DBLocation,u8"素材库 数据库 路径 database"},
        {u8"maintenance",Page::Library,0,LStr::Set_DBMaintenance,u8"维护 健康 检查 修复 重建 缓存 清理 重新链接"},
        {u8"fps",Page::Library,5,LStr::Set_FPSLimit,u8"性能 帧率 FPS"},
        {u8"thumb",Page::Library,5,LStr::Set_CoverThumbPrecision,u8"封面 缩略图 精度"},
        {u8"remember",Page::Interaction,0,LStr::Set_RememberLastFolder,u8"文件夹 上次 启动 路径"},
        {u8"closeAction",Page::Interaction,0,LStr::Set_CloseButton,u8"关闭窗口 退出 托盘 close"},
        {u8"guided",Page::Interaction,0,LStr::Set_GuidedMode,u8"引导 提示 帮助 tooltip"},
        {u8"confirm",Page::Interaction,0,LStr::SetUX_Confirm,u8"操作 确认 不再 询问"},
        {u8"hotkeyEnabled",Page::Interaction,0,LStr::Set_EnableHotkey,u8"全局 快捷键 热键"},
        {u8"hotkey",Page::Interaction,0,LStr::SetUX_Hotkey,u8"唤出 窗口 快捷键 hotkey"},
        {u8"shortcuts",Page::Interaction,0,LStr::SetUX_Shortcuts,u8"快捷键 键盘 编辑 绑定"},
        {u8"hideDrag",Page::Interaction,6,LStr::Set_HideOnDrag,u8"拖拽 隐藏 DAW"},
        {u8"dragDelay",Page::Interaction,6,LStr::Set_DragDelay,u8"拖拽 隐藏 延迟"},
        {u8"pauseDrag",Page::Interaction,6,LStr::Set_PauseOnDrag,u8"拖拽 暂停 试听"},
        {u8"treePath",Page::Interaction,6,LStr::Set_FolderTreePathMode,u8"文件夹 路径 领航 绝对"},
        {u8"shell",Page::Interaction,6,LStr::Set_ShellIntegration,u8"默认 播放器 Windows 关联"},
        {u8"username",Page::Personal,0,LStr::Set_Username,u8"用户 名称"},
        {u8"avatar",Page::Personal,0,LStr::SetUX_Avatar,u8"头像 图片 裁切"},
        {u8"exportPath",Page::Personal,0,LStr::Set_DefaultExportPath,u8"导出 输出 路径 文件夹"},
        {u8"suffix",Page::Personal,0,LStr::Main_ArisOutputSuffixCheck,u8"导出 处理 后缀"},
        {u8"export",Page::Backup,0,LStr::Set_ExportSettings,u8"导出 备份 配置"},
        {u8"import",Page::Backup,0,LStr::Set_ImportSettings,u8"导入 恢复 配置"},
        {u8"resetTheme",Page::Backup,0,LStr::Set_RestoreDefaultTheme,u8"恢复 默认 主题"},
        {u8"resetAll",Page::Backup,0,LStr::Set_ResetAllDefaults,u8"恢复 默认 全部 重置"},
        {u8"logging",Page::Diagnostics,0,LStr::Set_EnableDiag,u8"诊断 日志 logging debug"},
        {u8"overlay",Page::Diagnostics,0,LStr::Set_DiagShowOverlay,u8"诊断 浮层"},
        {u8"diagnosticTools",Page::Diagnostics,0,LStr::Set_DiagnosticsLogging,u8"诊断 日志 导出 清理 开发"},
    };
    return entries;
}
inline std::string FoldAscii(std::string value) {
    for (char& c : value) if (static_cast<unsigned char>(c) < 128) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return value;
}
inline std::vector<const Entry*> Search(const std::string& query, bool diagnostics) {
    std::istringstream stream(FoldAscii(query));
    std::vector<std::string> tokens;
    for (std::string token; stream >> token;) tokens.push_back(token);
    std::vector<const Entry*> results;
    if (tokens.empty()) return results;
    for (const auto& entry : Catalog()) {
        if (!diagnostics && entry.page == Page::Diagnostics) continue;
        const auto haystack = FoldAscii(entry.zh + " " + entry.en + " " + entry.ja + " " + entry.keywords);
        if (std::all_of(tokens.begin(), tokens.end(), [&](const auto& token) { return haystack.find(token) != std::string::npos; })) results.push_back(&entry);
    }
    return results;
}
}
