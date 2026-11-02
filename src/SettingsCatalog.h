#pragma once
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
    const char* Label(int language) const { return (language == 1 ? en : language == 2 ? ja : zh).c_str(); }
};
inline const std::vector<Entry>& Catalog() {
    static const std::vector<Entry> entries = {
        {u8"theme",Page::Appearance,0,u8"界面主题",u8"Interface theme",u8"画面テーマ",u8"主题 theme color 颜色"},
        {u8"automatic",Page::Appearance,0,u8"开启日夜切换主题模式",u8"Enable automatic day/night themes",u8"昼夜テーマの自動切り替えを有効にする",u8"日夜 自动 白天 黑夜 schedule"},
        {u8"day",Page::Appearance,0,u8"白天主题 · 08:00–16:00",u8"Day theme · 08:00–16:00",u8"昼のテーマ · 08:00–16:00",u8"白天 主题 day"},
        {u8"night",Page::Appearance,0,u8"黑夜主题 · 16:00–08:00",u8"Night theme · 16:00–08:00",u8"夜のテーマ · 16:00–08:00",u8"黑夜 主题 night"},
        {u8"themeEditor",Page::Appearance,0,u8"编辑主题",u8"Edit Theme",u8"テーマを編集",u8"编辑 颜色 配色 自定义 custom"},
        {u8"language",Page::Appearance,0,u8"界面语言 / Language:",u8"UI Language / Language:",u8"表示言語 / Language:",u8"中文 英文 日语 language"},
        {u8"systemFont",Page::Appearance,0,u8"系统字体大小",u8"System Font Size",u8"システムフォントサイズ",u8"界面 字体 大小 font"},
        {u8"listFont",Page::Appearance,0,u8"列表字体大小",u8"List Font Size",u8"リストフォントサイズ",u8"列表 字体 大小 font"},
        {u8"rowHeight",Page::Appearance,0,u8"列表行高",u8"Row Height",u8"行の高さ",u8"密度 紧凑 行高"},
        {u8"extension",Page::Appearance,0,u8"文件名显示后缀",u8"Show File Extension",u8"ファイル名に拡張子を表示",u8"文件 扩展 后缀"},
        {u8"waveMode",Page::Appearance,1,u8"默认显示模式:",u8"Default Display Mode:",u8"デフォルト表示モード:",u8"波形 音量 响度 频谱 waveform"},
        {u8"waveStyle",Page::Appearance,1,u8"预览波形样式:",u8"Preview Waveform Style:",u8"プレビュー波形スタイル:",u8"波形 RMS 实体 waveform"},
        {u8"miniRms",Page::Appearance,1,u8"Mini waveform RMS body style",u8"Mini waveform RMS body style",u8"Mini waveform RMS body style",u8"缩略 波形 RMS"},
        {u8"meterLayout",Page::Appearance,1,u8"音量表头声道标签",u8"Volume header channel labels",u8"音量ヘッダーチャンネルラベル",u8"声道 命名 SMPTE Film Atmos AmbiX"},
        {u8"showLufs",Page::Appearance,1,u8"综合响度",u8"Integrated Loudness (LUFS)",u8"積分ラウドネス",u8"LUFS 计量 响度"},
        {u8"showRms",Page::Appearance,1,u8"均方根电平 (RMS dB)",u8"RMS Level (dB)",u8"実効値レベル (RMS dB)",u8"RMS 计量"},
        {u8"showPeak",Page::Appearance,1,u8"真实峰值",u8"True Peak (dBTP)",u8"トゥルーピーク",u8"真峰值 计量 peak"},
        {u8"spectrumEnabled",Page::Appearance,1,u8"启用高分辨率频谱生成",u8"Enable high-resolution spectrogram",u8"高解像度スペクトル生成を有効化",u8"频谱 spectrum"},
        {u8"fft",Page::Appearance,1,u8"FFT 窗口大小",u8"FFT Window Size",u8"FFT ウィンドウサイズ",u8"频谱 窗口 FFT"},
        {u8"spectrumBlend",Page::Appearance,1,u8"默认频谱/波形混合比:",u8"Default Spectrum/Waveform Blend:",u8"デフォルトのスペクトル/波形ブレンド比:",u8"频谱 混合 blend"},
        {u8"spectrumSplit",Page::Appearance,1,u8"垂直分离显示",u8"Split Mode (Vertical Separation)",u8"垂直分離表示",u8"频谱 波形 分区"},
        {u8"ripple",Page::Appearance,2,u8"启用列表点击扩散波纹",u8"Enable list click ripple effect",u8"リストクリックの波紋エフェクトを有効化",u8"点击 波纹 效果"},
        {u8"milliseconds",Page::Appearance,2,u8"显示毫秒",u8"Show Milliseconds",u8"ミリ秒を表示",u8"时间 毫秒"},
        {u8"frames",Page::Appearance,2,u8"显示帧数",u8"Show Frame Number",u8"フレーム番号を表示",u8"时间 帧数"},
        {u8"frameRate",Page::Appearance,2,u8"默认帧率",u8"Default Frame Rate",u8"デフォルトフレームレート",u8"视频 帧率"},
        {u8"resetSize",Page::Appearance,2,u8"恢复默认界面尺寸",u8"Reset Default Sizes",u8"既定のUIサイズに戻す",u8"恢复 默认 尺寸 布局"},
        {u8"device",Page::Audio,0,u8"音频输出设备:",u8"Audio Output Device:",u8"オーディオ出力デバイス:",u8"声卡 声音 耳机 没声音 output device"},
        {u8"listening",Page::Audio,0,u8"试听模式:",u8"Preview Mode:",u8"プレビューモード:",u8"浏览 连续 试听"},
        {u8"volume",Page::Audio,0,u8"默认音量",u8"Default Volume",u8"デフォルト音量",u8"音量 声音 volume"},
        {u8"normalize",Page::Audio,0,u8"启用试听归一化",u8"Enable loudness normalization",u8"プレビュー正規化を有効化",u8"统一 试听 响度 归一 化 loudness normalize LUFS"},
        {u8"target",Page::Audio,0,u8"目标 LUFS",u8"Target LUFS",u8"目標 LUFS",u8"参考 电平 响度 target LUFS"},
        {u8"fade",Page::Audio,0,u8"启用跳转时的淡入淡出",u8"Enable fade-in/out on jump",u8"ジャンプ時のフェードイン/アウトを有効化",u8"防爆音 淡入 淡出 点击 保护"},
        {u8"fadeMs",Page::Audio,3,u8"淡入淡出时间 (ms)",u8"Fade Time (ms)",u8"フェード時間 (ms)",u8"淡入 淡出 时长"},
        {u8"period",Page::Audio,3,u8"驱动缓冲周期:",u8"Driver Buffer Period Size:",u8"ドライババッファ周期:",u8"音频 缓冲 周期 延迟 杂音 period"},
        {u8"skipSilence",Page::Audio,3,u8"跳过开头无声部分",u8"Skip Leading Silence",u8"冒頭の無音部分をスキップ",u8"开头 静音"},
        {u8"silenceThreshold",Page::Audio,3,u8"静音判断阈值",u8"Silence threshold",u8"無音のしきい値",u8"静音 判断 阈值 threshold"},
        {u8"pitch",Page::Audio,3,u8"默认音高",u8"Default Pitch",u8"デフォルトピッチ",u8"音高 半音 pitch"},
        {u8"pitchRange",Page::Audio,3,u8"音高范围",u8"Pitch Range",u8"ピッチ範囲",u8"音高 范围 pitch"},
        {u8"monitor",Page::Audio,4,u8"监听布局",u8"Monitor Layout",u8"モニターレイアウト",u8"声道 路由 监听 layout"},
        {u8"downmix",Page::Audio,4,u8"降混策略",u8"Downmix Strategy",u8"ダウンミックス戦略",u8"声道 下混 策略 downmix"},
        {u8"center",Page::Audio,4,u8"Center 折叠电平 (dB)",u8"Center fold-down level (dB)",u8"センターフォールドダウンレベル (dB)",u8"中置 声道 增益"},
        {u8"surround",Page::Audio,4,u8"环绕折叠电平 (dB)",u8"Surround fold-down level (dB)",u8"サラウンドフォールドダウンレベル (dB)",u8"环绕 声道 增益"},
        {u8"binaural",Page::Audio,4,u8"双耳监听",u8"Binaural Monitor",u8"バイノーラルモニター",u8"双耳 耳机 binaural"},
        {u8"deviceInfo",Page::Audio,4,u8"设备最大声道数",u8"Max device channels",u8"デバイスの最大チャンネル数",u8"设备 声道 能力"},
        {u8"searchMode",Page::Search,0,u8"搜索触发方式",u8"Start searching",u8"検索を実行するタイミング",u8"实时 输入 回车 搜索 realtime enter"},
        {u8"suggest",Page::Search,0,u8"搜索建议来源",u8"Search Suggestion Source",u8"検索サジェスト対象",u8"搜索 建议 联想 补全"},
        {u8"renameTab",Page::Search,0,u8"根据搜索词自动重命名当前标签页",u8"Auto-rename tab by search query",u8"検索語に基づき現在のタブを自動で名前変更",u8"搜索 标签 命名"},
        {u8"dictionary",Page::Search,0,u8"智能联想词库 (JSON 模式)",u8"Smart Thesaurus (JSON mode)",u8"スマート類義語辞書 (JSON モード)",u8"词库 同义词 触发 扩展 thesaurus"},
        {u8"perf",Page::Library,0,u8"系统性能模式",u8"Performance Mode",u8"システムパフォーマンスモード",u8"性能 内存 卡顿 慢 performance"},
        {u8"threads",Page::Library,0,u8"并行扫描线程",u8"Parallel Scan Threads",u8"並列スキャンスレッド",u8"扫描 线程 并发"},
        {u8"startupScan",Page::Library,0,u8"启动/切库后自动后台重扫库根目录",u8"Auto re-scan library roots in background after startup/db switch",u8"起動/DB 切替後にライブラリルートを自動でバックグラウンド再スキャン",u8"启动 自动 扫描 变化"},
        {u8"backfill",Page::Library,0,u8"空闲时补齐当前库的波形与封面",u8"Fill waveforms and covers of the current library when idle",u8"アイドル時に現在のライブラリの波形とジャケットを補完",u8"空闲 波形 封面 缓存 补齐"},
        {u8"idleMinutes",Page::Library,0,u8"空闲多久后开始",u8"Idle delay",u8"待機時間",u8"空闲 等待 多久 分钟"},
        {u8"database",Page::Library,0,u8"数据库位置",u8"Database Location",u8"データベースの場所",u8"素材库 数据库 路径 database"},
        {u8"maintenance",Page::Library,0,u8"数据库维护",u8"Database Maintenance",u8"データベースメンテナンス",u8"维护 健康 检查 修复 重建 缓存 清理 重新链接"},
        {u8"fps",Page::Library,5,u8"界面帧率限制:",u8"UI FPS Limit:",u8"UIフレームレート制限:",u8"性能 帧率 FPS"},
        {u8"thumb",Page::Library,5,u8"封面缩略图精度:",u8"Cover Thumbnail Precision:",u8"ジャケットサムネイル精度:",u8"封面 缩略图 精度"},
        {u8"remember",Page::Interaction,0,u8"记住上次浏览位置",u8"Remember Last Folder",u8"最後の閲覧位置を記憶",u8"文件夹 上次 启动 路径"},
        {u8"closeAction",Page::Interaction,0,u8"关闭按钮（X）",u8"Close Button (X)",u8"閉じるボタン（X）",u8"关闭窗口 退出 托盘 close"},
        {u8"guided",Page::Interaction,0,u8"引导模式",u8"Guided Mode",u8"ガイドモード",u8"引导 提示 帮助 tooltip"},
        {u8"confirm",Page::Interaction,0,u8"显示操作确认",u8"Show confirmation dialogs",u8"操作確認を表示",u8"操作 确认 不再 询问"},
        {u8"hotkeyEnabled",Page::Interaction,0,u8"启用全局快捷键",u8"Enable Global Hotkey",u8"グローバルホットキーを有効化",u8"全局 快捷键 热键"},
        {u8"hotkey",Page::Interaction,0,u8"唤出窗口快捷键",u8"Show-window shortcut",u8"ウィンドウを表示するショートカット",u8"唤出 窗口 快捷键 hotkey"},
        {u8"shortcuts",Page::Interaction,0,u8"编辑键盘快捷键",u8"Edit keyboard shortcuts",u8"キーボードショートカットを編集",u8"快捷键 键盘 编辑 绑定"},
        {u8"hideDrag",Page::Interaction,6,u8"拖拽时隐藏到后台（无动画）",u8"Hide Window on Drag (No Animation)",u8"ドラッグ中にウィンドウを隠す（アニメーションなし）",u8"拖拽 隐藏 DAW"},
        {u8"dragDelay",Page::Interaction,6,u8"拖拽延迟 (ms)",u8"Drag Delay (ms)",u8"ドラッグ遅延 (ms)",u8"拖拽 隐藏 延迟"},
        {u8"pauseDrag",Page::Interaction,6,u8"拖拽时自动暂停正在播放的音频",u8"Pause audio when a drag starts",u8"ドラッグ開始時に再生中のオーディオを一時停止",u8"拖拽 暂停 试听"},
        {u8"treePath",Page::Interaction,6,u8"数据库文件夹树路径",u8"Database folder tree path",u8"データベースフォルダーツリーの表示",u8"文件夹 路径 领航 绝对"},
        {u8"shell",Page::Interaction,6,u8"外壳集成",u8"Shell Integration",u8"シェル統合",u8"默认 播放器 Windows 关联"},
        {u8"username",Page::Personal,0,u8"用户名",u8"Username",u8"ユーザー名",u8"用户 名称"},
        {u8"avatar",Page::Personal,0,u8"更换头像",u8"Change avatar",u8"アバターを変更",u8"头像 图片 裁切"},
        {u8"exportPath",Page::Personal,0,u8"默认导出路径",u8"Default Export Path",u8"デフォルト書き出し先",u8"导出 输出 路径 文件夹"},
        {u8"suffix",Page::Personal,0,u8"Harken Search output: keep processing suffixes",u8"Harken Search output: keep processing suffixes",u8"Harken Search 出力：処理サフィックスを保持",u8"导出 处理 后缀"},
        {u8"export",Page::Backup,0,u8"导出设置...",u8"Export Settings...",u8"設定をエクスポート...",u8"导出 备份 配置"},
        {u8"import",Page::Backup,0,u8"导入设置...",u8"Import Settings...",u8"設定をインポート...",u8"导入 恢复 配置"},
        {u8"resetTheme",Page::Backup,0,u8"恢复默认主题",u8"Restore Default Theme",u8"デフォルトテーマに戻す",u8"恢复 默认 主题"},
        {u8"resetAll",Page::Backup,0,u8"恢复全部默认",u8"Reset All Defaults",u8"すべての既定に戻す",u8"恢复 默认 全部 重置"},
        {u8"logging",Page::Diagnostics,0,u8"启用诊断与日志记录",u8"Enable diagnostics & logging",u8"診断とログ記録を有効化",u8"诊断 日志 logging debug"},
        {u8"overlay",Page::Diagnostics,0,u8"显示诊断悬浮窗",u8"Show Diagnostic Overlay",u8"診断オーバーレイを表示",u8"诊断 浮层"},
        {u8"diagnosticTools",Page::Diagnostics,0,u8"诊断与日志",u8"Diagnostics & Logging",u8"診断とログ",u8"诊断 日志 导出 清理 开发"},
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
