// Aris - Send to Wwise (F-034 S7) - Transfer Panel UI.

#include "BetaFeatureFlags.h"
#include "WwiseTransferPanel.h"

#include "imgui.h"
#include "HarkenSearchWidgets.h"
#include "Theme.h"
#include "Localization.h"
#include "DiagnosticsLogger.h"
#include "../thirdparty/lucide/IconsLucide.h"
#include "AppState.h"  // Guided-tooltip wrappers.
#include "WindowsMain.h"  // GetUIFontEmphasis() for panel section title weight

#include "WaapiClient.h"
#include "WwiseDiff.h"
#include "WwiseQuery.h"
#include "WwiseTransferConfig.h"
#include "WwiseTransferExecute.h"
#include "WwiseTransferPersist.h"
#include "platform/filesystem.h"
#include "platform/system.h"

#include "nlohmann_json.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <mutex>
#include <sstream>
#include <thread>
#include <unordered_map>

namespace aura {
namespace wwise {

namespace {

// ---------- Module state ---------------------------------------------------
// All state behind a single mutex so the render callback (main thread) and
// the optional background transfer thread don't tear on reads. Transfer
// execution itself is currently synchronous on the UI thread — if we move
// it off later, the mutex covers us without reshuffling.

struct PanelState {
    bool visible = false;

    std::unique_ptr<waapi::WaapiClient> client;
    std::string connectError;
    std::string lastActionMessage;

    // Preset-naming modal buffers. `pendingPresetAction` is one of:
    //  0 = no modal, 1 = Save As, 2 = Rename active.
    int pendingPresetAction = 0;
    char presetNameBuf[128] = {};

    std::vector<std::string> sourceWavs;
    std::vector<MetadataContext> contexts;

    WwiseDiffNode lastDiff;
    bool hasDiff = false;

    // Cached Wwise-side snapshot. Populated by "Refresh from Wwise" action
    // and re-used across subsequent live-preview recomputations without
    // re-querying WAAPI — every edit in the panel re-runs computeDiff
    // against this cache, which is what gives the UI its real-time feel.
    ExistingObjectMap cachedExisting;
    ExistingOriginalsSet cachedWavs;
    bool hasCachedExisting = false;

    TransferResult lastResult;
    bool hasResult = false;

    // Transient UI buffers. Kept here so multi-frame edits don't drop chars
    // between frames on the const std::string inside WwiseTransferConfig.
    char bufProjectPath[1024]      = {};
    char bufWaapiHost[128]         = {};
    int  bufWaapiPort              = 8765;
    char bufOriginalsSubfolder[512] = {};
    char bufImportDest[512]        = {};
    bool buffersLoaded = false;
};

std::mutex g_mu;
PanelState g_ps;

// Async op state — decoupled from g_mu so worker threads can write results
// without waiting on the render pass. Flow:
//   1. Button handler (main thread): if !active, set active=true, spawn
//      detached thread with the WAAPI call.
//   2. Worker thread: executes the call (can block for up to WaapiClient's
//      10s timeout). On completion writes result under g_asyncMu + sets
//      ready.store(true).
//   3. Next render pass (main thread): if ready, apply result to panel
//      state under g_mu, clear flags.
//
// This keeps the ImGui thread free to draw every frame — the picker /
// reveal and refresh buttons display pending state instead of blocking the UI.
// freezing the whole window for 10 seconds when Wwise is slow to answer.
enum class AsyncKind : int {
    None = 0,
    PickImportDest = 1,
    PickRowTemplate = 2,
    Reveal = 3,
    RefreshFromWwise = 4,
};

struct AsyncOp {
    std::atomic<bool> active{false};
    std::atomic<bool> ready{false};
    // Only touched under g_asyncMu once active=true; cleared on apply.
    AsyncKind kind = AsyncKind::None;
    int targetRow = -1;
    bool success = false;
    std::string result;                 // picked path / reveal path / error
    ExistingObjectMap refreshedExisting;
    ExistingOriginalsSet refreshedWavs;
};

std::mutex g_asyncMu;
AsyncOp g_async;

bool ensureTerminalSoundLayer(std::vector<WwiseStructureNode>& structure);

bool tryClaimAsync(AsyncKind kind, int targetRow = -1) {
    bool expected = false;
    if (!g_async.active.compare_exchange_strong(expected, true)) return false;
    g_async.ready.store(false);
    std::lock_guard<std::mutex> lk(g_asyncMu);
    g_async.kind = kind;
    g_async.targetRow = targetRow;
    g_async.success = false;
    g_async.result.clear();
    g_async.refreshedExisting.clear();
    g_async.refreshedWavs.clear();
    return true;
}

// Pull the latest activeTransferConfig() into the UI buffers. Runs once per
// Open — after that, the user's edits live in the buffers until they
// flush on Save/Transfer.
void syncBuffersFromConfig() {
    auto& cfg = activeTransferConfig();
    ensureTerminalSoundLayer(cfg.structure);
    std::snprintf(g_ps.bufProjectPath, sizeof(g_ps.bufProjectPath), "%s", cfg.projectPath.c_str());
    std::snprintf(g_ps.bufWaapiHost, sizeof(g_ps.bufWaapiHost), "%s", cfg.waapiHost.c_str());
    g_ps.bufWaapiPort = cfg.waapiPort;
    std::snprintf(g_ps.bufOriginalsSubfolder, sizeof(g_ps.bufOriginalsSubfolder), "%s", cfg.originalsSubfolder.c_str());
    std::snprintf(g_ps.bufImportDest, sizeof(g_ps.bufImportDest), "%s", cfg.importDestination.c_str());
    g_ps.buffersLoaded = true;
}

// Commit buffer contents back into activeTransferConfig(). Called before
// Connect / Preview / Transfer so those actions always operate on the
// latest user input.
void flushBuffersToConfig() {
    auto& cfg = activeTransferConfig();
    cfg.projectPath = g_ps.bufProjectPath;
    cfg.waapiHost = g_ps.bufWaapiHost;
    cfg.waapiPort = g_ps.bufWaapiPort;
    cfg.originalsSubfolder = g_ps.bufOriginalsSubfolder;
    cfg.importDestination = g_ps.bufImportDest;
    ensureTerminalSoundLayer(cfg.structure);
}

// Lazy-init a single client instance. WAAPI only accepts one connection per
// Wwise project at a time, so shared-across-the-process is fine.
waapi::WaapiClient& ensureClient() {
    if (!g_ps.client) g_ps.client = std::make_unique<waapi::WaapiClient>();
    return *g_ps.client;
}

bool syncProjectPathFromWwise(waapi::WaapiClient& client, std::string* errorOut = nullptr) {
    std::string version;
    std::string projectPath;
    if (!client.ping(version, projectPath)) {
        if (errorOut) {
            *errorOut = client.lastError();
            if (errorOut->empty()) *errorOut = "Wwise ping failed";
        }
        return false;
    }
    if (projectPath.empty()) {
        if (errorOut) {
            *errorOut = "Wwise did not return a project path. Open a .wproj in Wwise or fill Project Path.";
        }
        return false;
    }

    auto& cfg = activeTransferConfig();
    cfg.projectPath = projectPath;
    std::snprintf(g_ps.bufProjectPath, sizeof(g_ps.bufProjectPath), "%s", projectPath.c_str());
    return true;
}

void seedDefaultStructureIfEmpty() {
    auto& cfg = activeTransferConfig();
    if (!cfg.structure.empty()) return;
    // Sensible defaults: a Random Container grouped by UCS category, one
    // Sound SFX leaf per file. User can edit or rip out; just means the
    // first-time experience shows something rather than an empty table.
    WwiseStructureNode root;
    root.type = WwiseObjectType::RandomContainer;
    root.nameTemplate = "$ucs_cat";
    cfg.structure.push_back(root);
    WwiseStructureNode leaf;
    leaf.type = WwiseObjectType::SoundSFX;
    leaf.nameTemplate = "$filename_base";
    cfg.structure.push_back(leaf);
}

bool ensureTerminalSoundLayer(std::vector<WwiseStructureNode>& structure) {
    if (structure.empty()) return false;
    bool changed = false;
    for (std::size_t i = 0; i + 1 < structure.size(); ++i) {
        if (structure[i].type == WwiseObjectType::SoundSFX) {
            structure[i].type = WwiseObjectType::RandomContainer;
            if (structure[i].nameTemplate.empty()) {
                structure[i].nameTemplate = "$ucs_cat";
            }
            changed = true;
        }
    }
    if (structure.back().type != WwiseObjectType::SoundSFX) {
        WwiseStructureNode leaf;
        leaf.type = WwiseObjectType::SoundSFX;
        leaf.nameTemplate = "$filename_base";
        structure.push_back(std::move(leaf));
        changed = true;
    } else if (structure.back().nameTemplate.empty()) {
        structure.back().nameTemplate = "$filename_base";
        changed = true;
    }
    return changed;
}

// ---------- UI helpers -----------------------------------------------------

ImVec4 mixColor(const ImVec4& a, const ImVec4& b, float t) {
    const float u = std::clamp(t, 0.0f, 1.0f);
    return ImVec4(
        a.x + (b.x - a.x) * u,
        a.y + (b.y - a.y) * u,
        a.z + (b.z - a.z) * u,
        a.w + (b.w - a.w) * u);
}

ImVec4 withAlpha(const ImVec4& c, float alpha) {
    return ImVec4(c.x, c.y, c.z, alpha);
}

// Alpha tiers for Wwise semantic tokens — all withAlpha(th.wwiseXxx) calls use one of these.
// Scatter across 23 sites is the source of subtle inconsistency; centralise here.
constexpr float kWwiseAlphaStrong = 0.92f;  // connection status, diff New/Replaced/Unresolved
constexpr float kWwiseAlphaMid    = 0.86f;  // header stat chips, snapshot/target status
constexpr float kWwiseAlphaLow    = 0.80f;  // muted counters (Staged count, NoChange)
constexpr float kWwiseBadgeAlpha  = 0.96f;  // role badges: leaf (SFX) — fully saturated
constexpr float kWwiseBadgeLow    = 0.80f;  // role badges: parent (when leaf is in same row)

bool isChineseUI();

std::string localizedActionMessage(const std::string& msg) {
    if (!isChineseUI() || msg.empty()) return msg;
    if (msg == "Connected.") return u8"已连接。";
    if (msg == "Connect failed.") return u8"连接失败。";
    if (msg == "Querying Wwise selection...") return u8"正在读取 Wwise 当前选择...";
    if (msg == "Revealing in Wwise...") return u8"正在定位到 Wwise...";
    if (msg == "No object selected in Wwise Project Explorer")
        return u8"Wwise 工程浏览器中没有选中对象。";
    if (msg.rfind("Pick failed: ", 0) == 0) {
        return u8"吸取失败：" + localizedActionMessage(msg.substr(13));
    }
    if (msg.rfind("Reveal failed: ", 0) == 0) {
        return u8"定位失败：" + msg.substr(15);
    }
    if (msg.rfind("Refresh failed: ", 0) == 0) {
        return u8"刷新失败：" + msg.substr(16);
    }
    if (msg.rfind("Deleted preset: ", 0) == 0) {
        return u8"已删除预设：" + msg.substr(16);
    }
    if (msg.rfind("Saved preset: ", 0) == 0) {
        return u8"已保存预设：" + msg.substr(14);
    }
    if (msg.rfind("Loaded preset: ", 0) == 0) {
        return u8"已载入预设：" + msg.substr(15);
    }
    return msg;
}

void drawFieldLabel(const char* label, const char* hint = nullptr) {
    const ThemeColorScheme& th = ThemeColors::current();
    ImGui::PushStyleColor(ImGuiCol_Text,
        ImGui::ColorConvertFloat4ToU32(withAlpha(
            mixColor(th.interfaceText, th.menuText, 0.24f), 0.88f)));
    ImGui::TextUnformatted(label);
    ImGui::PopStyleColor();
    if (hint && hint[0]) {
        ImGui::SameLine(0, 8.0f);
        ImGui::TextDisabled("%s", hint);
    }
    ImGui::Dummy(ImVec2(0, 4.0f));
}

void drawSectionHint(const char* text) {
    const ThemeColorScheme& th = ThemeColors::current();
    ImGui::PushStyleColor(ImGuiCol_Text,
        ImGui::ColorConvertFloat4ToU32(withAlpha(
            mixColor(th.interfaceText, th.chromeBorder, 0.30f), 0.82f)));
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
}

ImVec4 wwiseSurfaceTone(const ImVec4& accent,
                        float accentMix,
                        float baseMix,
                        float alpha = 1.0f) {
    const ThemeColorScheme& th = ThemeColors::current();
    const ImVec4 base = mixColor(th.dialogPanelBG, th.resultsBG, baseMix);
    return withAlpha(mixColor(base, accent, accentMix), alpha);
}

ImVec4 wwiseTextTone(const ImVec4& accent,
                     float accentMix = 0.24f,
                     float alpha = 0.96f) {
    const ThemeColorScheme& th = ThemeColors::current();
    return withAlpha(mixColor(th.menuText, accent, accentMix), alpha);
}

void drawWwiseSectionTitle(const char* title,
                           const ImVec4& accent,
                           const char* subtitle = nullptr) {
    ImGui::AlignTextToFramePadding();
    ImGui::PushFont(GetUIFontEmphasis());
    ImGui::PushStyleColor(ImGuiCol_Text,
        ImGui::ColorConvertFloat4ToU32(wwiseTextTone(accent, 0.26f, 0.98f)));
    ImGui::TextUnformatted(title ? title : "");
    ImGui::PopStyleColor();
    ImGui::PopFont();
    if (subtitle && subtitle[0]) {
        ImGui::SameLine(0, 10.0f);
        ImGui::PushStyleColor(ImGuiCol_Text,
            ImGui::ColorConvertFloat4ToU32(wwiseTextTone(accent, 0.14f, 0.66f)));
        ImGui::TextUnformatted(subtitle);
        ImGui::PopStyleColor();
    }
}

bool isChineseUI() {
    return g_currentLanguage == AppLanguage::Chinese;
}

const char* wz(const char* en, const char* zh) {
    if (!isChineseUI()) return en;
    static const std::unordered_map<std::string, const char*> kZh = {
        {"Transfer to Wwise (F-034)", u8"传输到 Wwise (F-034)"},
        {"Connection", u8"连接"},
        {"Connected", u8"已连接"},
        {"Disconnected", u8"未连接"},
        {"Connect", u8"连接"},
        {"Disconnect", u8"断开"},
        {"Preset", u8"预设"},
        {"(unsaved)", u8"(未保存)"},
        {"Save", u8"保存"},
        {"Save As...", u8"另存为..."},
        {"Rename", u8"重命名"},
        {"Delete", u8"删除"},
        {"Connection error", u8"连接错误"},
        {"Diagnostics", u8"诊断"},
        {"WAAPI Trace Log", u8"WAAPI 跟踪日志"},
        {"No trace path available.", u8"当前没有可用日志路径。"},
        {"Rename preset to:", u8"将预设重命名为："},
        {"Save as preset:", u8"另存为预设："},
        {"OK", u8"确定"},
        {"Cancel", u8"取消"},
        {"Destination", u8"导入目标"},
        {"Target", u8"目标"},
        {"Set", u8"已设置"},
        {"Missing", u8"缺失"},
        {"Connection & Project", u8"连接与工程"},
        {"Project Path", u8"工程路径"},
        {"WAAPI Host", u8"WAAPI 主机"},
        {"WAAPI Port", u8"WAAPI 端口"},
        {"Originals Subfolder", u8"素材子目录"},
        {"Under Originals/SFX", u8"位于 Originals/SFX 下"},
        {"Import Destination", u8"导入位置"},
        {"Target parent in Wwise", u8"Wwise 父级节点"},
        {"UseExisting", u8"使用现有"},
        {"Replace", u8"替换"},
        {"CreateNew", u8"创建新项"},
        {"If Sound Exists", u8"同名音效处理"},
        {"NewOnly", u8"仅新建时"},
        {"Always", u8"总是应用"},
        {"Apply Template", u8"模板应用"},
        {"Build Chain", u8"结构链路"},
        {"Layers", u8"层数"},
        {"Add Parent Layer", u8"添加父层"},
        {"Layer", u8"层级"},
        {"Type", u8"类型"},
        {"Name Template", u8"名称模板"},
        {"Property Template Source", u8"属性模板来源"},
        {"Actions", u8"操作"},
        {"Leaf", u8"叶子"},
        {"Parent ", u8"父层 "},
        {"Leaf row receives the imported audio file and is always SoundSFX.", u8"叶子层固定接收导入音频，并始终是 SoundSFX。"},
        {"Transfer", u8"执行传输"},
        {"Staged", u8"已暂存"},
        {"Snapshot", u8"快照"},
        {"Ready", u8"就绪"},
        {"Transfer to Wwise", u8"传输到 Wwise"},
        {"Refresh from Wwise", u8"从 Wwise 刷新"},
        {"Staged Sources", u8"暂存源文件"},
        {"Files", u8"文件"},
        {"Result", u8"结果"},
        {"Imported", u8"已导入"},
        {"Skipped", u8"已跳过"},
        {"Failed", u8"失败"},
        {"No transfer result yet.", u8"当前还没有传输结果。"},
        {"Preview", u8"预览视窗"},
        {"Viewport", u8"视图"},
        {"Current transfer structure", u8"当前传输结构"},
        {"No target destination selected yet.", u8"当前还没有选择导入目标。"},
        {"Nodes", u8"节点"},
        {"New", u8"新建"},
        {"Replaced", u8"替换"},
        {"Issues", u8"异常"},
        {"No preview available", u8"当前没有可用预览"},
        {"Refresh Wwise once, then the structure tree will appear here.", u8"先从 Wwise 刷新一次，结构树就会显示在这里。"},
        {"Existing", u8"已存在"},
        {"Issue", u8"异常"},
        {"Template", u8"已套模板"},
        {"Copy Failed", u8"复制失败"},
        {"Import Failed", u8"导入失败"},
        {"Template Failed", u8"模板失败"},
        {"issue: ", u8"异常："},
        {"WorkUnit", u8"工作单元"},
        {"VirtualFolder", u8"虚拟文件夹"},
        {"ActorMixer", u8"Actor 混音器"},
        {"RandomContainer", u8"随机容器"},
        {"SequenceContainer", u8"顺序容器"},
        {"SwitchContainer", u8"切换容器"},
        {"BlendContainer", u8"混合容器"},
        {"SoundSFX", u8"SoundSFX"},
    };
    auto it = kZh.find(en ? std::string(en) : std::string());
    return it != kZh.end() ? it->second : (zh ? zh : en);
}

const char* diffStatusText(DiffStatus status) {
    switch (status) {
        case DiffStatus::NoChange:   return wz("Existing", u8"已存在");
        case DiffStatus::New:        return wz("New", u8"新建");
        case DiffStatus::Replaced:   return wz("Replaced", u8"替换");
        case DiffStatus::Unresolved: return wz("Issue", u8"异常");
    }
    return "?";
}

const char* itemStatusText(ItemStatus status) {
    switch (status) {
        case ItemStatus::Imported:        return wz("Imported", u8"已导入");
        case ItemStatus::TemplateApplied: return wz("Template", u8"已套模板");
        case ItemStatus::SkippedExisting: return wz("Skipped", u8"已跳过");
        case ItemStatus::FailedWavCopy:   return wz("Copy Failed", u8"复制失败");
        case ItemStatus::FailedImport:    return wz("Import Failed", u8"导入失败");
        case ItemStatus::FailedTemplate:  return wz("Template Failed", u8"模板失败");
    }
    return "?";
}

const char* objectTypeLabelAt(int index, bool parentOnly) {
    static const char* kObjectTypeLabelsEn[] = {
        "WorkUnit", "VirtualFolder", "ActorMixer",
        "RandomContainer", "SequenceContainer", "SwitchContainer", "BlendContainer",
        "SoundSFX",
    };
    static const char* kParentObjectTypeLabelsEn[] = {
        "WorkUnit", "VirtualFolder", "ActorMixer",
        "RandomContainer", "SequenceContainer", "SwitchContainer", "BlendContainer",
    };
    static const char* kObjectTypeLabelsZh[] = {
        u8"工作单元", u8"虚拟文件夹", u8"Actor-Mixer",
        u8"随机容器", u8"顺序容器", u8"切换容器", u8"混合容器",
        "SoundSFX",
    };
    static const char* kParentObjectTypeLabelsZh[] = {
        u8"工作单元", u8"虚拟文件夹", u8"Actor-Mixer",
        u8"随机容器", u8"顺序容器", u8"切换容器", u8"混合容器",
    };
    return isChineseUI()
        ? (parentOnly ? kParentObjectTypeLabelsZh[index] : kObjectTypeLabelsZh[index])
        : (parentOnly ? kParentObjectTypeLabelsEn[index] : kObjectTypeLabelsEn[index]);
}

void drawStatChip(const char* label,
                  const std::string& value,
                  const ImVec4& tone,
                  float minWidth = 0.0f) {
    const ThemeColorScheme& th = ThemeColors::current();
    const ImGuiStyle& style = ImGui::GetStyle();

    std::string text = label ? label : "";
    if (!value.empty()) {
        if (!text.empty()) text += " ";
        text += value;
    }

    const ImVec2 textSize = ImGui::CalcTextSize(text.c_str());
    ImVec2 chipSize(
        textSize.x + style.FramePadding.x * 2.0f,
        textSize.y + style.FramePadding.y * 2.0f);
    if (chipSize.x < minWidth) chipSize.x = minWidth;

    ImGui::Dummy(chipSize);
    const ImVec2 p0 = ImGui::GetItemRectMin();
    const ImVec2 p1 = ImGui::GetItemRectMax();
    const ImVec4 chipBase = mixColor(th.frameBgBase, th.resultsBG, 0.18f);
    const ImVec4 bg = withAlpha(mixColor(chipBase, tone, 0.24f), 0.96f);
    const ImVec4 border = withAlpha(mixColor(th.chromeBorder, tone, 0.56f), 0.94f);
    const ImVec4 textColor = withAlpha(mixColor(th.menuText, tone, 0.42f), 0.98f);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0, p1, ImGui::ColorConvertFloat4ToU32(bg), style.FrameRounding);
    dl->AddRect(p0, p1, ImGui::ColorConvertFloat4ToU32(border), style.FrameRounding);
    dl->AddText(ImVec2(p0.x + style.FramePadding.x, p0.y + style.FramePadding.y),
                ImGui::ColorConvertFloat4ToU32(textColor),
                text.c_str());
}

void drawStatChip(const char* label, int value, const ImVec4& tone, float minWidth = 0.0f) {
    drawStatChip(label, std::to_string(value), tone, minWidth);
}

struct DiffSummary {
    int total = 0;
    int unchanged = 0;
    int added = 0;
    int replaced = 0;
    int unresolved = 0;
    int leaves = 0;
};

void collectDiffSummary(const WwiseDiffNode& node, DiffSummary& summary) {
    ++summary.total;
    if (node.children.empty()) ++summary.leaves;
    switch (node.status) {
        case DiffStatus::NoChange:   ++summary.unchanged; break;
        case DiffStatus::New:        ++summary.added; break;
        case DiffStatus::Replaced:   ++summary.replaced; break;
        case DiffStatus::Unresolved: ++summary.unresolved; break;
    }
    for (const auto& child : node.children) {
        collectDiffSummary(child, summary);
    }
}

const char* kObjectTypeLabels[] = {
    "WorkUnit", "VirtualFolder", "ActorMixer",
    "RandomContainer", "SequenceContainer", "SwitchContainer", "BlendContainer",
    "SoundSFX",
};
constexpr int kObjectTypeCount = static_cast<int>(sizeof(kObjectTypeLabels) / sizeof(kObjectTypeLabels[0]));
const char* kParentObjectTypeLabels[] = {
    "WorkUnit", "VirtualFolder", "ActorMixer",
    "RandomContainer", "SequenceContainer", "SwitchContainer", "BlendContainer",
};
constexpr int kParentObjectTypeCount =
    static_cast<int>(sizeof(kParentObjectTypeLabels) / sizeof(kParentObjectTypeLabels[0]));

// Wildcards catalog — cascading menu under the "W" button on any template
// input. Groups mirror the design doc; token values are what the template
// engine in WwiseStructureTemplate.cpp recognises. Function wildcards carry
// a placeholder arg so the user sees a runnable template after insertion.
struct WildcardEntry {
    const char* label;
    const char* token;
};
struct WildcardGroup {
    const char* label;
    const WildcardEntry* entries;
    int count;
};

constexpr WildcardEntry kWcFiles[] = {
    {"$filename_base  —  file without extension", "$filename_base"},
    {"$filename_full  —  file with extension",    "$filename_full"},
};
constexpr WildcardEntry kWcUcs[] = {
    {"$ucs_cat       —  UCS main category",     "$ucs_cat"},
    {"$ucs_subcat    —  UCS subcategory",       "$ucs_subcat"},
    {"$ucs_catid     —  UCS short code",        "$ucs_catid"},
};
constexpr WildcardEntry kWcMeta[] = {
    {"$library       —  library name",          "$library"},
    {"$designer      —  designer",              "$designer"},
    {"$composer      —  composer",              "$composer"},
};
constexpr WildcardEntry kWcDesc[] = {
    {"$description_first  —  first word",       "$description_first"},
    {"$description_slug   —  slug of description", "$description_slug"},
};
constexpr WildcardEntry kWcBatch[] = {
    {"$index         —  1-based batch index",   "$index"},
    {"$count         —  batch total",           "$count"},
};
constexpr WildcardEntry kWcUser[] = {
    {"$user(NAME)    —  per-config user var",   "$user(NAME)"},
    {"$envvar(NAME)  —  environment variable",  "$envvar(NAME)"},
};
constexpr WildcardEntry kWcFallback[] = {
    {"$wildcard(a, b)  —  first non-empty",     "$wildcard($designer, $composer)"},
};
constexpr WildcardEntry kWcTransform[] = {
    {"$upper($x)     —  uppercase",             "$upper($filename_base)"},
    {"$lower($x)     —  lowercase",             "$lower($filename_base)"},
    {"$capitalize($x) —  Capitalize",           "$capitalize($filename_base)"},
};

constexpr WildcardGroup kWildcardGroups[] = {
    {"Files",        kWcFiles,     static_cast<int>(sizeof(kWcFiles)/sizeof(kWcFiles[0]))},
    {"UCS Category", kWcUcs,       static_cast<int>(sizeof(kWcUcs)/sizeof(kWcUcs[0]))},
    {"Metadata",     kWcMeta,      static_cast<int>(sizeof(kWcMeta)/sizeof(kWcMeta[0]))},
    {"Description",  kWcDesc,      static_cast<int>(sizeof(kWcDesc)/sizeof(kWcDesc[0]))},
    {"Batch",        kWcBatch,     static_cast<int>(sizeof(kWcBatch)/sizeof(kWcBatch[0]))},
    {"User / Env",   kWcUser,      static_cast<int>(sizeof(kWcUser)/sizeof(kWcUser[0]))},
    {"Fallback",     kWcFallback,  static_cast<int>(sizeof(kWcFallback)/sizeof(kWcFallback[0]))},
    {"Transform",    kWcTransform, static_cast<int>(sizeof(kWcTransform)/sizeof(kWcTransform[0]))},
};

// ---------- WAAPI picker / reveal helpers --------------------------------
//
// Both these helpers require a connected WaapiClient. They return false
// (for pick) or swallow the failure (for reveal) when the client is offline
// — the UI buttons are gated on isConnected() so this is a belt-and-braces
// guard rather than a real path.

// Query Wwise for the currently-selected object in its Project Explorer.
// On success, populates `outPath` with the first selection's Wwise path
// ("\\Actor-Mixer Hierarchy\\...") and returns true. Multi-selection is
// common in Wwise — we take the first for v0; picking a sibling list would
// need a sub-menu here.
bool pickSelectedFromWwise(waapi::WaapiClient& client,
                           std::string& outPath,
                           std::string* errorOut) {
    if (!client.isConnected()) {
        if (errorOut) *errorOut = "Not connected to Wwise";
        return false;
    }
    // Wwise 2022.1+ returns only "id" by default. Request path/type/name
    // explicitly via the WAAPI "options.return" mechanism (must be nested
    // under "options" — top-level "return" is rejected as invalid_arguments).
    waapi::CallResult r = client.call("ak.wwise.ui.getSelectedObjects",
        R"({"options": {"return": ["path", "type", "id", "name"]}})");
    if (!r.ok) {
        if (errorOut) {
            // Surface method name + error so the user can tell timeouts
            // ("did Wwise answer?") from RPC rejections ("wrong args?") apart.
            *errorOut = "ak.wwise.ui.getSelectedObjects: " + r.errorMessage;
        }
        return false;
    }
    try {
        auto j = nlohmann::json::parse(r.resultJson);
        // Some Wwise builds key the array as "objects", others as "return".
        const nlohmann::json* arr = nullptr;
        if (j.contains("objects") && j["objects"].is_array()) arr = &j["objects"];
        else if (j.contains("return") && j["return"].is_array()) arr = &j["return"];
        if (!arr || arr->empty()) {
            if (errorOut) *errorOut = "No object selected in Wwise Project Explorer";
            return false;
        }
        const auto& first = (*arr)[0];
        if (first.contains("path") && first["path"].is_string()) {
            outPath = first["path"].get<std::string>();
            return true;
        }
        if (!first.contains("id") || !first["id"].is_string()) {
            if (errorOut) *errorOut = "Selected object has no id or path field";
            return false;
        }

        auto tryResolvePath = [&](const nlohmann::json& query,
                                  std::string* resolvedPath,
                                  std::string* localError) -> bool {
            waapi::CallResult getResult =
                client.call("ak.wwise.core.object.get", query.dump());
            if (!getResult.ok) {
                if (localError) *localError = "ak.wwise.core.object.get: " + getResult.errorMessage;
                return false;
            }
            try {
                auto details = nlohmann::json::parse(getResult.resultJson);
                const nlohmann::json* ret = nullptr;
                if (details.contains("return") && details["return"].is_array()) ret = &details["return"];
                else if (details.contains("objects") && details["objects"].is_array()) ret = &details["objects"];
                if (!ret || ret->empty()) {
                    if (localError) *localError = "Selected object lookup returned no rows";
                    return false;
                }
                const auto& row = (*ret)[0];
                if (!row.contains("path") || !row["path"].is_string()) {
                    if (localError) *localError = "Selected object lookup returned no path field; keys: " + row.dump();
                    return false;
                }
                *resolvedPath = row["path"].get<std::string>();
                return true;
            } catch (const std::exception& e) {
                if (localError) *localError = std::string("JSON parse: ") + e.what();
                return false;
            }
        };

        std::string resolvedPath;
        std::string resolveError;
        const std::string selectedId = first["id"].get<std::string>();
        nlohmann::json queryWithReturn;
        queryWithReturn["from"]["id"] = nlohmann::json::array({selectedId});
        queryWithReturn["options"]["return"] = nlohmann::json::array({"path", "type", "id", "name"});
        if (tryResolvePath(queryWithReturn, &resolvedPath, &resolveError)) {
            outPath = std::move(resolvedPath);
            return true;
        }

        nlohmann::json fallbackQuery;
        fallbackQuery["from"]["id"] = nlohmann::json::array({selectedId});
        if (tryResolvePath(fallbackQuery, &resolvedPath, &resolveError)) {
            outPath = std::move(resolvedPath);
            return true;
        }

        if (errorOut) {
            *errorOut = resolveError.empty() ? "Selected object has no path field" : resolveError;
        }
        return false;
    } catch (const std::exception& e) {
        if (errorOut) *errorOut = std::string("JSON parse: ") + e.what();
        return false;
    }
}

// Send a "Find in Project Explorer (Sync Group 1)" command to Wwise,
// targeted at the given object path. Wwise's FindInProjectExplorerSyncGroup1
// command takes object IDs not paths, so we first resolve path→id via
// object.get, then fire the command. Two WAAPI round-trips but that's fine
// for a click-driven UX where latency budgets are generous.
bool revealInWwise(waapi::WaapiClient& client,
                   const std::string& wwisePath,
                   std::string* errorOut) {
    if (!client.isConnected()) {
        if (errorOut) *errorOut = "Not connected to Wwise";
        return false;
    }
    // Step 1: resolve path → id.
    std::string objectId;
    {
        nlohmann::json args;
        args["from"]["path"] = nlohmann::json::array({wwisePath});
        args["options"]["return"] = nlohmann::json::array({"id"});
        waapi::CallResult r = client.call("ak.wwise.core.object.get", args.dump());
        if (!r.ok) {
            if (errorOut) *errorOut = "object.get failed: " + r.errorMessage;
            return false;
        }
        try {
            auto j = nlohmann::json::parse(r.resultJson);
            // Either "return" (newer) or "objects" (older) key carries the array.
            const nlohmann::json* arr = nullptr;
            if (j.contains("return") && j["return"].is_array()) arr = &j["return"];
            else if (j.contains("objects") && j["objects"].is_array()) arr = &j["objects"];
            if (!arr || arr->empty()) {
                if (errorOut) *errorOut = "Object not found in Wwise: " + wwisePath;
                return false;
            }
            const auto& first = (*arr)[0];
            if (!first.contains("id") || !first["id"].is_string()) {
                if (errorOut) *errorOut = "Object has no id field";
                return false;
            }
            objectId = first["id"].get<std::string>();
        } catch (const std::exception& e) {
            if (errorOut) *errorOut = std::string("JSON parse: ") + e.what();
            return false;
        }
    }
    // Step 2: fire the find command.
    {
        nlohmann::json args;
        args["command"] = "FindInProjectExplorerSyncGroup1";
        args["objects"] = nlohmann::json::array({objectId});
        waapi::CallResult r = client.call("ak.wwise.ui.commands.execute", args.dump());
        if (!r.ok) {
            if (errorOut) *errorOut = "commands.execute failed: " + r.errorMessage;
            return false;
        }
    }
    return true;
}

std::string pickFolderDialog(const std::string& startPathUtf8) {
    return Platform::BrowseFolderDialog(nullptr, "", startPathUtf8);
}

// Compute a relative path from `root` to `target` using forward slashes.
// Returns empty string when `target` is not inside `root` (common case:
// user picks a folder outside the Wwise project). Trailing slash included
// because Originals Subfolder wants a directory shape, not a file shape.
std::string relativeUnder(const std::string& root, const std::string& target) {
    const std::string rootCanon = Platform::FileSystem::WeaklyCanonical(root);
    const std::string targetCanon = Platform::FileSystem::WeaklyCanonical(target);
    if (rootCanon.empty() || targetCanon.empty()) return {};
    const std::string s = Platform::FileSystem::RelativePath(targetCanon, rootCanon);
    // RelativePath returns ".." when target is above root, or "." when equal.
    if (s.empty() || s == "." || s.rfind("..", 0) == 0) return {};
    return s + "/";
}

// Render the wildcard cascade. Appends the selected token at the end of
// `*target`. The first version intentionally appends at the end; cursor-aware
// insertion can be added later if users request it.
void renderWildcardsMenu(std::string* target) {
    for (const auto& group : kWildcardGroups) {
        if (ImGui::BeginMenu(group.label)) {
            for (int i = 0; i < group.count; ++i) {
                const auto& e = group.entries[i];
                ImGui::PushID(e.token);
                if (ImGui::MenuItem(e.label)) {
                    *target += e.token;
                }
                ImGui::PopID();
            }
            ImGui::EndMenu();
        }
    }
}

int objectTypeIndex(WwiseObjectType t) {
    switch (t) {
        case WwiseObjectType::WorkUnit:          return 0;
        case WwiseObjectType::VirtualFolder:     return 1;
        case WwiseObjectType::ActorMixer:        return 2;
        case WwiseObjectType::RandomContainer:   return 3;
        case WwiseObjectType::SequenceContainer: return 4;
        case WwiseObjectType::SwitchContainer:   return 5;
        case WwiseObjectType::BlendContainer:    return 6;
        case WwiseObjectType::SoundSFX:          return 7;
    }
    return 0;
}
WwiseObjectType objectTypeFromIndex(int i) {
    switch (i) {
        case 0: return WwiseObjectType::WorkUnit;
        case 1: return WwiseObjectType::VirtualFolder;
        case 2: return WwiseObjectType::ActorMixer;
        case 3: return WwiseObjectType::RandomContainer;
        case 4: return WwiseObjectType::SequenceContainer;
        case 5: return WwiseObjectType::SwitchContainer;
        case 6: return WwiseObjectType::BlendContainer;
        case 7: return WwiseObjectType::SoundSFX;
    }
    return WwiseObjectType::VirtualFolder;
}
int parentObjectTypeIndex(WwiseObjectType t) {
    switch (t) {
        case WwiseObjectType::WorkUnit:          return 0;
        case WwiseObjectType::VirtualFolder:     return 1;
        case WwiseObjectType::ActorMixer:        return 2;
        case WwiseObjectType::RandomContainer:   return 3;
        case WwiseObjectType::SequenceContainer: return 4;
        case WwiseObjectType::SwitchContainer:   return 5;
        case WwiseObjectType::BlendContainer:    return 6;
        case WwiseObjectType::SoundSFX:          return 3;
    }
    return 3;
}
WwiseObjectType parentObjectTypeFromIndex(int i) {
    switch (i) {
        case 0: return WwiseObjectType::WorkUnit;
        case 1: return WwiseObjectType::VirtualFolder;
        case 2: return WwiseObjectType::ActorMixer;
        case 3: return WwiseObjectType::RandomContainer;
        case 4: return WwiseObjectType::SequenceContainer;
        case 5: return WwiseObjectType::SwitchContainer;
        case 6: return WwiseObjectType::BlendContainer;
    }
    return WwiseObjectType::RandomContainer;
}

// Small coloured badge + short tag used in front of each diff tree node.
// Colours picked to be distinguishable at 12px and not to collide with the
// status colour (grey/green/amber/red). 2–3-char ASCII codes render in the
// default ImGui font without needing a custom icon atlas.
struct ObjTypeVisual {
    const char* code;
    ImVec4 color;
};
ObjTypeVisual objectTypeVisual(WwiseObjectType t) {
    const ThemeColorScheme& th = ThemeColors::current();
    // F-034 /colorize: anchor on wwiseParentBadge / wwiseLeafBadge so badges stay
    // distinguishable across all 5 themes. Mix toward interfaceText at constant 0.18f
    // (was 0.08–0.24 scattered) so containers look harmonious as a family.
    const float kMix = 0.18f;
    const float kAlpha = 0.92f;
    switch (t) {
        case WwiseObjectType::WorkUnit:
            return {"WU", withAlpha(mixColor(th.wwiseParentBadge, th.menuText, kMix), kAlpha)};
        case WwiseObjectType::VirtualFolder:
            return {"VF", withAlpha(mixColor(th.wwiseParentBadge, th.interfaceText, kMix), kAlpha)};
        case WwiseObjectType::ActorMixer:
            return {"AM", withAlpha(mixColor(th.wwiseParentBadge, th.wwiseLeafBadge, kMix), kAlpha)};
        case WwiseObjectType::RandomContainer:
            return {"Rnd", withAlpha(mixColor(th.wwiseParentBadge, th.wwiseDiffReplaced, kMix), kAlpha)};
        case WwiseObjectType::SequenceContainer:
            return {"Seq", withAlpha(mixColor(th.wwiseParentBadge, th.wwiseConnectOk, kMix), kAlpha)};
        case WwiseObjectType::SwitchContainer:
            return {"Swt", withAlpha(mixColor(th.wwiseParentBadge, th.wwiseDiffNew, kMix), kAlpha)};
        case WwiseObjectType::BlendContainer:
            return {"Bld", withAlpha(mixColor(th.wwiseParentBadge, th.wwiseDiffUnresolved, kMix), kAlpha)};
        case WwiseObjectType::SoundSFX:
            return {"SFX", withAlpha(th.wwiseLeafBadge, kWwiseBadgeAlpha)};
    }
    return {"??", withAlpha(th.wwiseDiffNeutral, kWwiseAlphaMid)};
}

// ---------- Async dispatchers ---------------------------------------------

void startPickImportDest() {
    if (!g_ps.client || !g_ps.client->isConnected()) return;
    if (!tryClaimAsync(AsyncKind::PickImportDest)) return;
    waapi::WaapiClient* client = g_ps.client.get();
    std::thread([client] {
        std::string path, err;
        const bool ok = pickSelectedFromWwise(*client, path, &err);
        {
            std::lock_guard<std::mutex> lk(g_asyncMu);
            g_async.success = ok;
            g_async.result = ok ? path : err;
        }
        g_async.ready.store(true);
    }).detach();
}

void startPickRowTemplate(int rowIdx) {
    if (!g_ps.client || !g_ps.client->isConnected()) return;
    if (!tryClaimAsync(AsyncKind::PickRowTemplate, rowIdx)) return;
    waapi::WaapiClient* client = g_ps.client.get();
    std::thread([client] {
        std::string path, err;
        const bool ok = pickSelectedFromWwise(*client, path, &err);
        {
            std::lock_guard<std::mutex> lk(g_asyncMu);
            g_async.success = ok;
            g_async.result = ok ? path : err;
        }
        g_async.ready.store(true);
    }).detach();
}

void startReveal(const std::string& path) {
    if (!g_ps.client || !g_ps.client->isConnected()) return;
    if (!tryClaimAsync(AsyncKind::Reveal)) return;
    waapi::WaapiClient* client = g_ps.client.get();
    std::string targetPath = path;
    std::thread([client, targetPath] {
        std::string err;
        const bool ok = revealInWwise(*client, targetPath, &err);
        {
            std::lock_guard<std::mutex> lk(g_asyncMu);
            g_async.success = ok;
            g_async.result = ok ? targetPath : err;
        }
        g_async.ready.store(true);
    }).detach();
}

void startRefreshFromWwise() {
    if (!g_ps.client || !g_ps.client->isConnected()) return;
    if (!tryClaimAsync(AsyncKind::RefreshFromWwise)) return;
    waapi::WaapiClient* client = g_ps.client.get();
    // Snapshot the destination + projectPath at dispatch time so later
    // edits during the RPC don't point the fetch at the wrong target.
    const auto& cfg = activeTransferConfig();
    std::string destination = cfg.importDestination;
    std::string originalsRoot;
    if (!cfg.projectPath.empty()) {
        originalsRoot = (std::filesystem::path(cfg.projectPath).parent_path()
                         / "Originals" / "SFX").string();
    }
    std::thread([client, destination, originalsRoot] {
        ExistingObjectMap existing;
        ExistingOriginalsSet wavs;
        std::string err;
        const bool objOk = fetchExistingObjects(*client, destination, existing, &err);
        if (!originalsRoot.empty()) {
            scanOriginalsDirectory(originalsRoot, wavs, nullptr);
        }
        {
            std::lock_guard<std::mutex> lk(g_asyncMu);
            g_async.success = objOk;
            g_async.result = objOk ? std::string{} : err;
            g_async.refreshedExisting = std::move(existing);
            g_async.refreshedWavs = std::move(wavs);
        }
        g_async.ready.store(true);
    }).detach();
}

// Called each frame under g_mu. Drains a completed async op into panel state
// so the UI reflects the result on the next immediate redraw.
void pumpAsyncCompletion() {
    if (!g_async.ready.load()) return;
    AsyncKind kind;
    int targetRow;
    bool ok;
    std::string result;
    ExistingObjectMap refreshed;
    ExistingOriginalsSet refreshedWavs;
    {
        std::lock_guard<std::mutex> lk(g_asyncMu);
        kind = g_async.kind;
        targetRow = g_async.targetRow;
        ok = g_async.success;
        result = std::move(g_async.result);
        refreshed = std::move(g_async.refreshedExisting);
        refreshedWavs = std::move(g_async.refreshedWavs);
        g_async.kind = AsyncKind::None;
        g_async.targetRow = -1;
    }
    g_async.ready.store(false);
    g_async.active.store(false);

    switch (kind) {
        case AsyncKind::PickImportDest:
            if (ok) {
                std::snprintf(g_ps.bufImportDest, sizeof(g_ps.bufImportDest), "%s", result.c_str());
                g_ps.lastActionMessage = "Import destination set to: " + result;
            } else {
                g_ps.lastActionMessage = "Pick failed: " + result;
            }
            break;
        case AsyncKind::PickRowTemplate: {
            auto& cfg = activeTransferConfig();
            if (targetRow >= 0 && targetRow < static_cast<int>(cfg.structure.size())) {
                if (ok) {
                    cfg.structure[targetRow].propertyTemplatePath = result;
                    cfg.structure[targetRow].useTemplate = true;
                    g_ps.lastActionMessage = "Template set on layer " + std::to_string(targetRow);
                } else {
                    g_ps.lastActionMessage = "Pick template failed: " + result;
                }
            }
            break;
        }
        case AsyncKind::Reveal:
            if (!ok) g_ps.lastActionMessage = "Reveal failed: " + result;
            break;
        case AsyncKind::RefreshFromWwise:
            g_ps.cachedExisting = std::move(refreshed);
            g_ps.cachedWavs = std::move(refreshedWavs);
            g_ps.hasCachedExisting = true;
            g_ps.lastActionMessage = ok
                ? ("Refreshed Wwise snapshot: " + std::to_string(g_ps.cachedExisting.size()) + " objects, " +
                   std::to_string(g_ps.cachedWavs.size()) + " WAVs.")
                : ("Refresh failed: " + result);
            break;
        case AsyncKind::None:
            break;
    }
}

// Recurse-render a WwiseDiffNode. Three colour channels in play per row:
//   - Object-type badge (blue / yellow / orange / green / ... per WwiseObjectType)
//   - Node-name text (greyscale; readable at all theme brightnesses)
//   - Status tag (grey / green / amber / red per DiffStatus)
// Tree handle is an empty-label TreeNodeEx so we can lay out the three
// coloured segments ourselves on the same row via SameLine.
ImVec4 statusColorFor(DiffStatus s) {
    // F-034 /colorize: per-theme semantic tokens; no more inline mixing on volatile
    // base tokens (e.g. LapisBlue's controlAccent=#FFFFFF would collapse New→NoChange).
    const ThemeColorScheme& th = ThemeColors::current();
    switch (s) {
        case DiffStatus::NoChange:   return withAlpha(th.wwiseDiffNeutral,    kWwiseAlphaLow);
        case DiffStatus::New:        return withAlpha(th.wwiseDiffNew,        kWwiseAlphaStrong);
        case DiffStatus::Replaced:   return withAlpha(th.wwiseDiffReplaced,   kWwiseAlphaStrong);
        case DiffStatus::Unresolved: return withAlpha(th.wwiseDiffUnresolved, kWwiseAlphaStrong);
    }
    return th.menuText;
}

void renderDiffNode(const WwiseDiffNode& node) {
    ImGui::PushID(static_cast<const void*>(&node));
    const auto vis = objectTypeVisual(node.type);
    const ImVec4 statusColor = statusColorFor(node.status);

    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_DefaultOpen
                             | ImGuiTreeNodeFlags_OpenOnArrow
                             | ImGuiTreeNodeFlags_OpenOnDoubleClick;
    if (node.children.empty()) {
        flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
    }

    const bool open = ImGui::TreeNodeEx("##row", flags);
    ImGui::SameLine(0, 4);
    ImGui::TextColored(vis.color, "[%s]", vis.code);
    ImGui::SameLine(0, 6);
    ImGui::Text("%s", node.name.c_str());
    ImGui::SameLine(0, 8);
    ImGui::TextColored(statusColor, "[%s]", diffStatusText(node.status));
    if (!node.sourceWav.empty()) {
        ImGui::SameLine(0, 10);
        ImGui::TextDisabled("<- %s", node.sourceWav.c_str());
    }
    if (node.status == DiffStatus::Unresolved && !node.unresolvedTokens.empty()) {
        std::string msg = std::string("    ") + wz("issue: ", u8"异常: ");
        for (std::size_t i = 0; i < node.unresolvedTokens.size(); ++i) {
            msg += (i ? ", " : "") + node.unresolvedTokens[i];
        }
        ImGui::TextColored(statusColor, "%s", msg.c_str());
    }

    if (open && !node.children.empty()) {
        for (const auto& c : node.children) renderDiffNode(c);
        ImGui::TreePop();
    }
    ImGui::PopID();
}

// Pure local-only preview: uses whatever's in the cached ExistingObjectMap /
// ExistingOriginalsSet (empty on first run, populated after a successful
// RefreshFromWwise). Called every frame so typing into a template shows
// diff updates live; no WAAPI round-trip per keystroke.
void runLocalPreview() {
    auto& cfg = activeTransferConfig();
    ensureTerminalSoundLayer(cfg.structure);
    if (cfg.structure.empty() || g_ps.sourceWavs.empty()) {
        g_ps.hasDiff = false;
        return;
    }
    const auto imports = expandStructure(cfg, g_ps.contexts, g_ps.sourceWavs);
    g_ps.lastDiff = computeDiff(cfg.importDestination, imports,
                                g_ps.cachedExisting, g_ps.cachedWavs,
                                cfg.conflictPolicy);
    g_ps.hasDiff = true;
}

// ---------- Action handlers -----------------------------------------------

void handleConnect() {
    flushBuffersToConfig();
    auto& client = ensureClient();
    waapi::ClientConfig cc;
    cc.host = activeTransferConfig().waapiHost;
    cc.port = activeTransferConfig().waapiPort;
    const auto started = std::chrono::steady_clock::now();
    DIAG_SYSTEM_FMT("Wwise", "ConnectStart:host=%s port=%d", cc.host.c_str(), cc.port);
    if (client.connect(cc)) {
        std::string projectErr;
        const bool gotProjectPath = syncProjectPathFromWwise(client, &projectErr);
        g_ps.lastActionMessage = gotProjectPath
            ? "Connected. Wwise project path detected."
            : ("Connected. Project path warning: " + projectErr);
        g_ps.connectError.clear();
        const auto elapsedMs = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - started).count();
        DIAG_SYSTEM_FMT("Wwise", "ConnectComplete:success=1 durationMs=%.2f", elapsedMs);
    } else {
        g_ps.connectError = client.lastError();
        g_ps.lastActionMessage = "Connect failed.";
        const auto elapsedMs = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - started).count();
        DIAG_SYSTEM_FMT("Wwise", "ConnectComplete:success=0 durationMs=%.2f", elapsedMs);
    }
}

void handleDisconnect() {
    if (g_ps.client) g_ps.client->disconnect();
    g_ps.lastActionMessage = "Disconnected.";
    DIAG_SYSTEM("Wwise", "Disconnect");
}

// "Refresh from Wwise" button — dispatches the fetch on a worker thread so
// the UI stays responsive while Wwise answers (which can take seconds on
// large projects, or indefinitely if the socket is stale). Local-only
// preview still updates every frame from the cached snapshot.
void handlePreview() {
    flushBuffersToConfig();
    g_ps.hasResult = false;
    const auto& cfg = activeTransferConfig();
    if (cfg.structure.empty()) {
        g_ps.lastActionMessage = "Structure is empty — add at least one layer.";
        return;
    }
    if (!g_ps.client || !g_ps.client->isConnected()) {
        // Local-only preview still shows the structure / wildcards expansion,
        // against an empty existing snapshot. That's useful even offline.
        g_ps.lastActionMessage = "Not connected — preview uses empty Wwise snapshot.";
        return;
    }
    startRefreshFromWwise();
    g_ps.lastActionMessage = "Refreshing Wwise snapshot...";
}

void handleTransfer() {
    flushBuffersToConfig();
    g_ps.hasResult = false;

    const auto& cfg = activeTransferConfig();
    if (!g_ps.client || !g_ps.client->isConnected()) {
        g_ps.lastActionMessage = "Not connected — cannot transfer.";
        return;
    }
    if (cfg.structure.empty()) {
        g_ps.lastActionMessage = "Structure is empty — add at least one layer.";
        return;
    }
    if (g_ps.sourceWavs.empty()) {
        g_ps.lastActionMessage = "No files staged — open the panel from the results list right-click.";
        return;
    }

    if (cfg.projectPath.empty()) {
        std::string projectErr;
        syncProjectPathFromWwise(*g_ps.client, &projectErr);
        if (cfg.projectPath.empty()) {
            g_ps.lastResult = TransferResult{};
            g_ps.lastResult.aggregateError = projectErr.empty()
                ? "Wwise project path is empty"
                : projectErr;
            g_ps.hasResult = true;
            g_ps.lastActionMessage = "Transfer failed: " + g_ps.lastResult.aggregateError;
            DIAG_SYSTEM_FMT("Wwise", "ImportAbort:reason=%s", g_ps.lastResult.aggregateError.c_str());
            return;
        }
    }

    const auto imports = expandStructure(cfg, g_ps.contexts, g_ps.sourceWavs);
    DIAG_SYSTEM_FMT("Wwise", "ImportStart:count=%d conflict=%s",
                    (int)imports.size(), toString(cfg.conflictPolicy).c_str());
    DiagScopedTimer transferTimer("Wwise", "importTotal");
    g_ps.lastResult = executeTransfer(*g_ps.client, cfg, imports, /*progressCb*/ nullptr);
    g_ps.hasResult = true;
    DIAG_SYSTEM_FMT("Wwise", "ImportComplete:success=%d total=%d imported=%d skipped=%d failed=%d",
                    g_ps.lastResult.failed == 0 && g_ps.lastResult.aggregateError.empty() ? 1 : 0,
                    g_ps.lastResult.total,
                    g_ps.lastResult.imported,
                    g_ps.lastResult.skipped,
                    g_ps.lastResult.failed);
    for (int i = 0; i < (int)g_ps.lastResult.items.size(); ++i) {
        const auto& item = g_ps.lastResult.items[(size_t)i];
        DIAG_ACTION_FMT("Wwise", "ImportItem:index=%d status=%s", i, toString(item.status));
    }
    if (!g_ps.lastResult.aggregateError.empty()) {
        g_ps.lastActionMessage = "Transfer failed: " + g_ps.lastResult.aggregateError;
        return;
    }
    std::ostringstream oss;
    oss << "Transfer complete: " << g_ps.lastResult.imported << " imported / "
        << g_ps.lastResult.skipped << " skipped / "
        << g_ps.lastResult.failed << " failed";
    g_ps.lastActionMessage = oss.str();
}

} // namespace

void OpenWwiseTransferPanel(const std::vector<std::string>& sourceWavs,
                            const std::vector<MetadataContext>& contexts) {
    if (!HarkenSearch::BetaFeatureFlags::kExposeWwiseTransfer) {
        return;
    }

    std::lock_guard<std::mutex> lk(g_mu);
    g_ps.sourceWavs = sourceWavs;
    g_ps.contexts = contexts;
    if (g_ps.contexts.size() < g_ps.sourceWavs.size()) {
        g_ps.contexts.resize(g_ps.sourceWavs.size()); // Pad with defaults so expandStructure sees matched lengths.
    }
    syncBuffersFromConfig();
    seedDefaultStructureIfEmpty();
    g_ps.visible = true;
    g_ps.hasDiff = false;
    g_ps.hasResult = false;
    g_ps.lastActionMessage.clear();
}


void RenderWwiseTransferPanel() {
    if (!HarkenSearch::BetaFeatureFlags::kExposeWwiseTransfer) {
        return;
    }

    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_ps.visible) return;

    pumpAsyncCompletion();
    flushBuffersToConfig();
    runLocalPreview();

    const ThemeColorScheme& th = ThemeColors::current();
    static bool showProjectSettings = false;
    static bool showStagedSources = false;
    static bool showDiagnostics = false;

    // F-034 /colorize: anchor on theme-defined semantic tokens, not inline mixes.
    const ImVec4 connectedColor    = withAlpha(th.wwiseConnectOk,   kWwiseAlphaStrong);
    const ImVec4 disconnectedColor = withAlpha(th.wwiseConnectFail, kWwiseAlphaStrong);
    const bool connected = g_ps.client && g_ps.client->isConnected();

    ImGui::SetNextWindowSize(ImVec2(1060, 640), ImGuiCond_FirstUseEver);
    bool open = true;
    const char* windowTitle = wz("Transfer to Wwise (F-034)", u8"传输到 Wwise (F-034)");
    if (!ImGui::Begin(windowTitle, &open, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
        ImGui::End();
        g_ps.visible = open;
        return;
    }

    auto& presets = presetList();
    const std::string& activeName = activePresetName();
    const std::string comboLabel = activeName.empty() ? wz("(unsaved)", u8"(未保存)") : activeName;

    if (ImGui::BeginTable("wwiseTransferSessionGrid", 2,
                          ImGuiTableFlags_SizingStretchProp |
                          ImGuiTableFlags_NoSavedSettings)) {
        ImGui::TableSetupColumn("Connection", ImGuiTableColumnFlags_WidthStretch, 0.82f);
        ImGui::TableSetupColumn("Preset", ImGuiTableColumnFlags_WidthStretch, 1.18f);

        ImGui::TableNextColumn();
        drawFieldLabel(wz("Connection", u8"连接"));
        drawStatChip(connected ? wz("Connected", u8"已连接") : wz("Disconnected", u8"未连接"), "",
                     connected ? connectedColor : disconnectedColor, 120.0f);
        ImGui::SameLine(0, 10.0f);
        if (!connected) {
            if (aura::ui::Button(wz("Connect", u8"连接"),
                    aura::ui::ButtonOptions{aura::ui::ButtonVariant::PrimaryTransfer,
                                            aura::ui::ButtonSize::Md})) {
                handleConnect();
            }
        } else {
            if (aura::ui::Button(wz("Disconnect", u8"断开"),
                    aura::ui::ButtonOptions{aura::ui::ButtonVariant::Secondary,
                                            aura::ui::ButtonSize::Md})) {
                handleDisconnect();
            }
        }

        ImGui::TableNextColumn();
        drawFieldLabel(wz("Preset", u8"预设"));
        ImGui::SetNextItemWidth(220.0f);
        if (ImGui::BeginCombo("##preset", comboLabel.c_str())) {
            if (ImGui::Selectable(wz("(unsaved)", u8"(未保存)"), activeName.empty())) {
                activePresetName().clear();
            }
            ImGui::Separator();
            for (int i = 0; i < (int)presets.size(); ++i) {
                const auto& p = presets[i];
                const bool isActive = (p.name == activeName);
                ImGui::PushID(i);
                if (ImGui::Selectable(p.name.c_str(), isActive)) {
                    if (loadPresetIntoActive(p.name)) {
                        syncBuffersFromConfig();
                        g_ps.lastActionMessage = "Loaded preset: " + p.name;
                    }
                }
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine(0, 8.0f);
        if (aura::ui::Button("##wtp_save",
                aura::ui::ButtonOptions{aura::ui::ButtonVariant::Secondary,
                                        aura::ui::ButtonSize::Sm,
                                        ICON_LC_SAVE})) {
            flushBuffersToConfig();
            if (!activeName.empty()) {
                savePresetFromActive(activeName);
                g_ps.lastActionMessage = "Saved preset: " + activeName;
            } else {
                g_ps.presetNameBuf[0] = '\0';
                g_ps.pendingPresetAction = 1;
                ImGui::OpenPopup("presetNameModal");
            }
        }
        ImGui::SameLine();
        if (aura::ui::Button(wz("Save As...", u8"另存为..."),
                aura::ui::ButtonOptions{aura::ui::ButtonVariant::Secondary,
                                        aura::ui::ButtonSize::Sm})) {
            flushBuffersToConfig();
            std::snprintf(g_ps.presetNameBuf, sizeof(g_ps.presetNameBuf),
                          "%s", activeName.empty() ? "New Preset" : (activeName + " copy").c_str());
            g_ps.pendingPresetAction = 1;
            ImGui::OpenPopup("presetNameModal");
        }
        ImGui::SameLine();
        const bool hasActive = !activeName.empty();
        if (!hasActive) ImGui::BeginDisabled();
        if (aura::ui::Button(wz("Rename", u8"重命名"),
                aura::ui::ButtonOptions{aura::ui::ButtonVariant::Secondary,
                                        aura::ui::ButtonSize::Sm})) {
            std::snprintf(g_ps.presetNameBuf, sizeof(g_ps.presetNameBuf),
                          "%s", activeName.c_str());
            g_ps.pendingPresetAction = 2;
            ImGui::OpenPopup("presetNameModal");
        }
        ImGui::SameLine();
        if (aura::ui::Button(wz("Delete", u8"删除"),
                aura::ui::ButtonOptions{aura::ui::ButtonVariant::Danger,
                                        aura::ui::ButtonSize::Sm})) {
            std::snprintf(g_ps.presetNameBuf, sizeof(g_ps.presetNameBuf), "%s", activeName.c_str());
            g_ps.pendingPresetAction = 3;
            ImGui::OpenPopup("presetDeleteConfirm");
        }
        if (!hasActive) ImGui::EndDisabled();

        ImGui::EndTable();
    }

    if (!g_ps.connectError.empty()) {
        ImGui::Spacing();
        if (aura::ui::BeginPanel("wwiseConnectError", aura::ui::PanelVariant::Inset, 0.0f, 52.0f)) {
            ImGui::PushStyleColor(ImGuiCol_Text,
                ImGui::ColorConvertFloat4ToU32(disconnectedColor));
            ImGui::TextWrapped("%s: %s", wz("Connection error", u8"连接错误"), g_ps.connectError.c_str());
            ImGui::PopStyleColor();
            aura::ui::EndPanel();
        }
    }

    if (g_ps.client && aura::ui::SectionHeader(wz("Diagnostics", u8"诊断"), &showDiagnostics)) {
        const std::string trace = g_ps.client->traceLogPath();
        drawFieldLabel(wz("WAAPI Trace Log", u8"WAAPI 跟踪日志"));
        if (!trace.empty()) {
            ImGui::TextDisabled("%s", trace.c_str());
            if (ImGui::IsItemHovered()) {
                HarkenSearch::SetTooltip("Every send / recv / timeout event is written here for WAAPI diagnostics.");
            }
        } else {
            ImGui::TextDisabled("%s", wz("No trace path available.", u8"当前没有可用日志路径。"));
        }
    }

    ImGui::Spacing();

    if (ImGui::BeginPopupModal("presetNameModal", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        const char* title = (g_ps.pendingPresetAction == 2)
            ? wz("Rename preset to:", u8"重命名预设为：")
            : wz("Save as preset:", u8"另存为预设：");
        ImGui::TextUnformatted(title);
        ImGui::SetNextItemWidth(260);
        ImGui::InputText("##presetName", g_ps.presetNameBuf, sizeof(g_ps.presetNameBuf),
                         ImGuiInputTextFlags_AutoSelectAll);
        ImGui::Spacing();
        const bool nameOk = g_ps.presetNameBuf[0] != '\0';
        if (!nameOk) ImGui::BeginDisabled();
        if (aura::ui::Button(wz("OK", u8"确定"),
                aura::ui::ButtonOptions{aura::ui::ButtonVariant::PrimaryTransfer,
                                        aura::ui::ButtonSize::Md})) {
            std::string newName = g_ps.presetNameBuf;
            if (g_ps.pendingPresetAction == 2) {
                if (renamePreset(activePresetName(), newName)) {
                    g_ps.lastActionMessage = "Renamed to: " + newName;
                } else {
                    g_ps.lastActionMessage = "Rename failed (name clash or active preset missing)";
                }
            } else {
                savePresetFromActive(newName);
                g_ps.lastActionMessage = "Saved preset: " + newName;
            }
            g_ps.pendingPresetAction = 0;
            ImGui::CloseCurrentPopup();
        }
        if (!nameOk) ImGui::EndDisabled();
        ImGui::SameLine();
        if (aura::ui::Button(wz("Cancel", u8"取消"),
                aura::ui::ButtonOptions{aura::ui::ButtonVariant::Quiet,
                                        aura::ui::ButtonSize::Md})) {
            g_ps.pendingPresetAction = 0;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    if (ImGui::BeginPopupModal("presetDeleteConfirm", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted(wz("Delete this preset?", u8"确认删除此预设？"));
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Text,
            ImGui::ColorConvertFloat4ToU32(withAlpha(ThemeColors::current().menuText, 0.65f)));
        ImGui::TextUnformatted(g_ps.presetNameBuf);
        ImGui::PopStyleColor();
        ImGui::Spacing();
        if (aura::ui::Button(wz("Delete", u8"删除"),
                aura::ui::ButtonOptions{aura::ui::ButtonVariant::Danger,
                                        aura::ui::ButtonSize::Md})) {
            const std::string name = g_ps.presetNameBuf;
            deletePreset(name);
            g_ps.lastActionMessage = "Deleted preset: " + name;
            g_ps.pendingPresetAction = 0;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (aura::ui::Button(wz("Cancel", u8"取消"),
                aura::ui::ButtonOptions{aura::ui::ButtonVariant::Quiet,
                                        aura::ui::ButtonSize::Md})) {
            g_ps.pendingPresetAction = 0;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    auto& cfg = activeTransferConfig();

    ImGui::Spacing();
    const float bodyAvailH = (std::max)(240.0f, ImGui::GetContentRegionAvail().y);
    const float previewPanelHeight = g_ps.hasDiff
        ? std::clamp(bodyAvailH * 0.42f, 220.0f, 360.0f)
        : 92.0f;
    const float workspaceHeight = (std::max)(256.0f, bodyAvailH - previewPanelHeight - 12.0f);
    const float importSetupHeight = showProjectSettings ? 312.0f : 188.0f;
    const float structurePanelHeight = 0.0f;
    const float actionPanelHeight = workspaceHeight;
    {
        const float splitGap = 16.0f;
        const float totalW = ImGui::GetContentRegionAvail().x;
        const float mainW = (totalW - splitGap) * 0.635f;
        const bool mainColumnVisible = ImGui::BeginChild("wwiseTransferMainColumn", ImVec2(mainW, workspaceHeight), false, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        if (mainColumnVisible) {
        if (aura::ui::BeginPanel("wwiseTransferImportSetup", aura::ui::PanelVariant::Peer, 0.0f, importSetupHeight, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
            ImGui::AlignTextToFramePadding();
            ImGui::PushFont(GetUIFontEmphasis());
            ImGui::TextUnformatted(wz("Destination", u8"导入目标"));
            ImGui::PopFont();
            ImGui::SameLine(0, 10.0f);
            drawStatChip(wz("Target", u8"目标"), g_ps.bufImportDest[0] ? wz("Set", u8"已设置") : wz("Missing", u8"缺失"),
                         withAlpha(th.wwiseDiffNeutral, kWwiseAlphaMid),
                         90.0f);
            aura::ui::Divider(aura::ui::DividerVariant::Inset);

            if (aura::ui::SectionHeader(wz("Connection & Project", u8"连接与工程"), &showProjectSettings)) {
                drawFieldLabel(wz("Project Path", u8"工程路径"), ".wproj");
                ImGui::InputText("##projectPath", g_ps.bufProjectPath, sizeof(g_ps.bufProjectPath));

                if (ImGui::BeginTable("wwiseTransferConnectionGrid", 2,
                                      ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_NoSavedSettings)) {
                    ImGui::TableNextColumn();
                    drawFieldLabel(wz("WAAPI Host", u8"WAAPI 主机"));
                    ImGui::InputText("##waapiHost", g_ps.bufWaapiHost, sizeof(g_ps.bufWaapiHost));

                    ImGui::TableNextColumn();
                    drawFieldLabel(wz("WAAPI Port", u8"WAAPI 端口"));
                    ImGui::InputInt("##waapiPort", &g_ps.bufWaapiPort);
                    ImGui::EndTable();
                }
                aura::ui::Divider(aura::ui::DividerVariant::Inset);
            }

            if (ImGui::BeginTable("wwiseTransferPathGrid", 2,
                                  ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoSavedSettings)) {
                ImGui::TableSetupColumn("Originals", ImGuiTableColumnFlags_WidthStretch, 1.02f);
                ImGui::TableSetupColumn("Destination", ImGuiTableColumnFlags_WidthStretch, 1.18f);

                ImGui::TableNextColumn();
                drawFieldLabel(wz("Originals Subfolder", u8"素材子目录"), wz("Under Originals/SFX", u8"位于 Originals/SFX 下"));
                {
                    const float singleBtn = 28.0f;
                    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - singleBtn - 6.0f);
                    ImGui::InputText("##origSubfolder", g_ps.bufOriginalsSubfolder, sizeof(g_ps.bufOriginalsSubfolder));
                    ImGui::SameLine(0, 4.0f);
                    if (aura::ui::Button(ICON_LC_FOLDER_OPEN "##origBrowse",
                            aura::ui::ButtonOptions{aura::ui::ButtonVariant::Secondary,
                                                    aura::ui::ButtonSize::Sm})) {
                        std::string originalsRoot;
                        if (!activeTransferConfig().projectPath.empty()) {
                            originalsRoot = (std::filesystem::path(activeTransferConfig().projectPath).parent_path()
                                             / "Originals" / "SFX").string();
                        }
                        const std::string picked = pickFolderDialog(originalsRoot);
                        if (!picked.empty()) {
                            std::string rel;
                            if (!originalsRoot.empty()) rel = relativeUnder(originalsRoot, picked);
                            if (!rel.empty()) {
                                std::snprintf(g_ps.bufOriginalsSubfolder,
                                              sizeof(g_ps.bufOriginalsSubfolder),
                                              "%s", rel.c_str());
                                g_ps.lastActionMessage = "Originals subfolder set to: " + rel;
                            } else {
                                g_ps.lastActionMessage =
                                    "Picked folder is outside <Wwise>/Originals/SFX/. Pick a subfolder inside it or type a relative subpath.";
                            }
                        }
                    }
                    if (ImGui::IsItemHovered()) {
                        HarkenSearch::SetTooltip("Pick a folder under <Wwise>/Originals/SFX/.");
                    }
                }

                ImGui::TableNextColumn();
                drawFieldLabel(wz("Import Destination", u8"导入位置"), wz("Target parent in Wwise", u8"Wwise 父级节点"));
                {
                    const float pickButtonW = 32.0f;
                    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - pickButtonW - 6.0f);
                    ImGui::InputText("##importDest", g_ps.bufImportDest, sizeof(g_ps.bufImportDest));
                    ImGui::SameLine(0, 4.0f);
                    const bool canPick = g_ps.client && g_ps.client->isConnected()
                                         && !g_async.active.load();
                    const bool pickDestBusy =
                        g_async.active.load() && g_async.kind == AsyncKind::PickImportDest;
                    if (aura::ui::Button("##pickDest",
                            aura::ui::ButtonOptions{aura::ui::ButtonVariant::Secondary,
                                                    aura::ui::ButtonSize::Sm,
                                                    ICON_LC_DOWNLOAD,
                                                    pickDestBusy,
                                                    !canPick})) {
                        startPickImportDest();
                        g_ps.lastActionMessage = "Querying Wwise selection...";
                    }
                    if (ImGui::IsItemHovered() && canPick) {
                        HarkenSearch::SetTooltip("Use the current Wwise Project Explorer selection.");
                    }
                }
                ImGui::EndTable();
            }

            ImGui::Spacing();
            if (ImGui::BeginTable("wwiseTransferPolicyGrid", 2,
                                  ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_NoSavedSettings)) {
                ImGui::TableNextColumn();
                const char* conflictLabels[] = {
                    wz("UseExisting", u8"使用现有"),
                    wz("Replace", u8"替换"),
                    wz("CreateNew", u8"创建新项")
                };
                int cidx = static_cast<int>(cfg.conflictPolicy);
                if (aura::ui::Combo(wz("If Sound Exists", u8"同名音效处理"), &cidx, conflictLabels, 3)) {
                    cfg.conflictPolicy = static_cast<ConflictPolicy>(cidx);
                }

                ImGui::TableNextColumn();
                const char* tplLabels[] = {wz("NewOnly", u8"仅新建时"), wz("Always", u8"总是应用")};
                int tidx = static_cast<int>(cfg.templateApply);
                if (aura::ui::Combo(wz("Apply Template", u8"模板应用"), &tidx, tplLabels, 2)) {
                    cfg.templateApply = static_cast<TemplateApply>(tidx);
                }
                ImGui::EndTable();
            }

            aura::ui::EndPanel();
        }

        ImGui::Spacing();
        if (aura::ui::BeginPanel("wwiseTransferStructure", aura::ui::PanelVariant::Peer, 0.0f, structurePanelHeight)) {
            static int s_selectedRow = -1;

            drawWwiseSectionTitle(
                wz("Wwise Structures", u8"Wwise 结构"),
                th.wwiseParentBadge,
                wz("Build the hierarchy, then refine the selected row below.",
                   u8"先搭建层级，再在下方细化当前选中行。"));
            ImGui::Spacing();
            // Add Parent: insert AT selected idx (selected becomes child of new).
            // No selection → insert at top (becomes outermost ancestor).
            // Leaf selected → insert at size-1 (becomes leaf's parent).
            if (aura::ui::Button(wz("Add Parent Layer", u8"添加父层"),
                    aura::ui::ButtonOptions{aura::ui::ButtonVariant::Secondary,
                                            aura::ui::ButtonSize::Md,
                                            ICON_LC_PLUS})) {
                WwiseStructureNode n;
                n.type = WwiseObjectType::RandomContainer;
                n.nameTemplate = "$ucs_cat";
                int insertIdx = 0;
                if (s_selectedRow >= 0 && s_selectedRow < static_cast<int>(cfg.structure.size())) {
                    insertIdx = s_selectedRow;
                }
                cfg.structure.insert(cfg.structure.begin() + insertIdx, n);
                ensureTerminalSoundLayer(cfg.structure);
                s_selectedRow = insertIdx;
            }
            ImGui::SameLine(0, 6.0f);
            // Add Child: insert AFTER selected (becomes child of selected).
            // Leaf selected → disabled (leaf cannot have children).
            // No selection → insert at end-1 (becomes leaf's parent).
            const bool addChildDisabled =
                (s_selectedRow >= 0
                 && s_selectedRow + 1 == static_cast<int>(cfg.structure.size()));
            if (aura::ui::Button(wz("Add Child Layer", u8"添加子层"),
                    aura::ui::ButtonOptions{aura::ui::ButtonVariant::Secondary,
                                            aura::ui::ButtonSize::Md,
                                            ICON_LC_PLUS,
                                            false,
                                            addChildDisabled})) {
                WwiseStructureNode n;
                n.type = WwiseObjectType::RandomContainer;
                n.nameTemplate = "$ucs_cat";
                int insertIdx;
                if (s_selectedRow >= 0
                    && s_selectedRow + 1 < static_cast<int>(cfg.structure.size())) {
                    insertIdx = s_selectedRow + 1;
                } else if (!cfg.structure.empty()) {
                    insertIdx = static_cast<int>(cfg.structure.size()) - 1;
                } else {
                    insertIdx = 0;
                }
                cfg.structure.insert(cfg.structure.begin() + insertIdx, n);
                ensureTerminalSoundLayer(cfg.structure);
                s_selectedRow = insertIdx;
            }
            aura::ui::Divider(aura::ui::DividerVariant::Inset);
            ImGui::Spacing();



            {
                if (s_selectedRow >= static_cast<int>(cfg.structure.size())) {
                    s_selectedRow = cfg.structure.empty() ? -1 : static_cast<int>(cfg.structure.size()) - 1;
                }

                int removeIdx = -1;
                int moveUpIdx = -1;
                int moveDownIdx = -1;

                const float toolbarW = 42.0f;
                const float splitGapLocal = 10.0f;
                const float tableHeight = (std::max)(170.0f, structurePanelHeight * 0.38f);
                const float tableWidth = (std::max)(260.0f, ImGui::GetContentRegionAvail().x - toolbarW - splitGapLocal);
                const ImVec4 structureAccent = th.wwiseParentBadge;
                const ImVec4 structureTableHeader = wwiseSurfaceTone(structureAccent, 0.18f, 0.10f, 0.98f);
                const ImVec4 structureTableRow = wwiseSurfaceTone(structureAccent, 0.05f, 0.04f, 0.96f);
                const ImVec4 structureTableRowAlt = wwiseSurfaceTone(structureAccent, 0.09f, 0.16f, 0.98f);
                const ImVec4 structureTableBorder = wwiseSurfaceTone(structureAccent, 0.34f, 0.06f, 0.95f);
                const ImVec4 structureSelection = withAlpha(mixColor(th.resultsSelection, structureAccent, 0.42f), 0.26f);

                ImGui::PushStyleColor(ImGuiCol_TableHeaderBg, ImGui::ColorConvertFloat4ToU32(structureTableHeader));
                ImGui::PushStyleColor(ImGuiCol_TableBorderStrong, ImGui::ColorConvertFloat4ToU32(structureTableBorder));
                ImGui::PushStyleColor(ImGuiCol_TableBorderLight, ImGui::ColorConvertFloat4ToU32(withAlpha(structureTableBorder, 0.52f)));
                ImGui::PushStyleColor(ImGuiCol_TableRowBg, ImGui::ColorConvertFloat4ToU32(structureTableRow));
                ImGui::PushStyleColor(ImGuiCol_TableRowBgAlt, ImGui::ColorConvertFloat4ToU32(structureTableRowAlt));
                ImGui::PushStyleColor(ImGuiCol_Header, ImGui::ColorConvertFloat4ToU32(structureSelection));
                ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImGui::ColorConvertFloat4ToU32(withAlpha(mixColor(structureSelection, structureAccent, 0.28f), 0.34f)));
                ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImGui::ColorConvertFloat4ToU32(withAlpha(mixColor(structureSelection, th.resultsSelection, 0.40f), 0.42f)));

                if (ImGui::BeginTable("wwiseStructureTable", 3,
                                      ImGuiTableFlags_BordersInnerH |
                                          ImGuiTableFlags_RowBg |
                                          ImGuiTableFlags_SizingStretchProp |
                                          ImGuiTableFlags_ScrollY |
                                          ImGuiTableFlags_NoSavedSettings,
                                      ImVec2(tableWidth, tableHeight))) {
                    ImGui::TableSetupScrollFreeze(0, 1);
                    ImGui::TableSetupColumn(wz("Object Type", "Object Type"), ImGuiTableColumnFlags_WidthFixed, 180.0f);
                    ImGui::TableSetupColumn(wz("Object Name", "Object Name"));
                    ImGui::TableSetupColumn(wz("Property Template Source", "Property Template Source"));
                    ImGui::TableHeadersRow();

                    for (std::size_t i = 0; i < cfg.structure.size(); ++i) {
                        auto& layer = cfg.structure[i];
                        const bool isLeafRow = (i + 1 == cfg.structure.size());
                        const bool rowSelected = (s_selectedRow == static_cast<int>(i));
                        const auto vis = objectTypeVisual(isLeafRow ? WwiseObjectType::SoundSFX : layer.type);

                        if (isLeafRow) {
                            layer.type = WwiseObjectType::SoundSFX;
                        }

                        ImGui::TableNextRow();
                        ImGui::PushID(static_cast<int>(i));

                        ImGui::TableSetColumnIndex(0);
                        const std::string typeLabel = std::string("[") + vis.code + "] " +
                            (isLeafRow
                                 ? objectTypeLabelAt(objectTypeIndex(WwiseObjectType::SoundSFX), false)
                                 : objectTypeLabelAt(parentObjectTypeIndex(layer.type), true));
                        ImGui::PushStyleColor(ImGuiCol_Text, vis.color);
                        if (ImGui::Selectable(typeLabel.c_str(), rowSelected,
                                              ImGuiSelectableFlags_SpanAllColumns)) {
                            s_selectedRow = static_cast<int>(i);
                        }
                        ImGui::PopStyleColor();
                        if (ImGui::IsItemHovered()) {
                            HarkenSearch::SetTooltip("%s", isLeafRow
                                ? wz("Leaf row receives the imported audio file and is always SoundSFX.",
                                     "Leaf row receives the imported audio file and is always SoundSFX.")
                                : wz("Select this row to edit its structure properties below.",
                                     "Select this row to edit its structure properties below."));
                        }

                        ImGui::TableSetColumnIndex(1);
                        ImGui::TextUnformatted(layer.nameTemplate.empty() ? "-" : layer.nameTemplate.c_str());

                        ImGui::TableSetColumnIndex(2);
                        const char* templatePreview = layer.useTemplate
                            ? (layer.propertyTemplatePath.empty()
                                   ? wz("From current Wwise selection", "From current Wwise selection")
                                   : layer.propertyTemplatePath.c_str())
                            : wz("Disabled", "Disabled");
                        ImGui::TextUnformatted(templatePreview);
                        if (ImGui::IsItemHovered() && layer.useTemplate && !layer.propertyTemplatePath.empty()) {
                            HarkenSearch::SetTooltip("%s", layer.propertyTemplatePath.c_str());
                        }

                        ImGui::PopID();
                    }
                    ImGui::EndTable();
                }
                ImGui::PopStyleColor(8);

                const bool hasSel = (s_selectedRow >= 0 && s_selectedRow < static_cast<int>(cfg.structure.size()));
                const bool selIsLeaf = hasSel && (s_selectedRow + 1 == static_cast<int>(cfg.structure.size()));
                const bool selCanUp = hasSel && !selIsLeaf && s_selectedRow > 0;
                const bool selCanDn = hasSel && !selIsLeaf && (s_selectedRow + 2 < static_cast<int>(cfg.structure.size()));
                const bool selCanDel = hasSel && !selIsLeaf;
                const bool selBusy = hasSel
                    && g_async.active.load()
                    && g_async.kind == AsyncKind::PickRowTemplate
                    && g_async.targetRow == s_selectedRow;
                const bool canPickTpl = g_ps.client && g_ps.client->isConnected() && !g_async.active.load();

                ImGui::SameLine(0, splitGapLocal);
                ImGui::PushStyleColor(ImGuiCol_ChildBg,
                    ImGui::ColorConvertFloat4ToU32(wwiseSurfaceTone(th.wwiseDiffNeutral, 0.10f, 0.14f, 0.98f)));
                ImGui::PushStyleColor(ImGuiCol_Border,
                    ImGui::ColorConvertFloat4ToU32(wwiseSurfaceTone(th.wwiseDiffNeutral, 0.30f, 0.08f, 0.92f)));
                ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 8.0f);
                ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1.0f);
                ImGui::BeginChild("wwiseStructureToolbar", ImVec2(toolbarW, tableHeight), ImGuiChildFlags_Borders,
                                  ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
                {
                    if (aura::ui::Button("##pickSelectedTemplate",
                            aura::ui::ButtonOptions{aura::ui::ButtonVariant::Ghost,
                                                    aura::ui::ButtonSize::Md,
                                                    ICON_LC_DOWNLOAD,
                                                    selBusy,
                                                    !hasSel || !canPickTpl})) {
                        startPickRowTemplate(s_selectedRow);
                        g_ps.lastActionMessage = "Querying Wwise selection...";
                    }
                    if (ImGui::IsItemHovered()) {
                        HarkenSearch::SetTooltip("%s", wz("Use the current Wwise selection as this row's property template.",
                                                  "Use the current Wwise selection as this row's property template."));
                    }

                    if (aura::ui::Button("##moveSelectedUp",
                            aura::ui::ButtonOptions{aura::ui::ButtonVariant::Ghost,
                                                    aura::ui::ButtonSize::Md,
                                                    ICON_LC_ARROW_UP,
                                                    false,
                                                    !selCanUp})) {
                        moveUpIdx = s_selectedRow;
                    }

                    if (aura::ui::Button("##moveSelectedDown",
                            aura::ui::ButtonOptions{aura::ui::ButtonVariant::Ghost,
                                                    aura::ui::ButtonSize::Md,
                                                    ICON_LC_ARROW_DOWN,
                                                    false,
                                                    !selCanDn})) {
                        moveDownIdx = s_selectedRow;
                    }

                    if (aura::ui::Button("##deleteSelectedRow",
                            aura::ui::ButtonOptions{aura::ui::ButtonVariant::Ghost,
                                                    aura::ui::ButtonSize::Md,
                                                    ICON_LC_TRASH,
                                                    false,
                                                    !selCanDel})) {
                        removeIdx = s_selectedRow;
                    }
                }
                ImGui::EndChild();
                ImGui::PopStyleVar(2);
                ImGui::PopStyleColor(2);

                ImGui::Spacing();
                aura::ui::Divider(aura::ui::DividerVariant::Inset);
                ImGui::Spacing();

                ImGui::PushStyleColor(ImGuiCol_ChildBg,
                    ImGui::ColorConvertFloat4ToU32(wwiseSurfaceTone(th.wwiseLeafBadge, 0.10f, 0.14f, 0.98f)));
                ImGui::PushStyleColor(ImGuiCol_Border,
                    ImGui::ColorConvertFloat4ToU32(wwiseSurfaceTone(th.wwiseLeafBadge, 0.30f, 0.06f, 0.92f)));
                ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 8.0f);
                ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1.0f);
                if (ImGui::BeginChild("wwiseSelectedRowPropertiesCard", ImVec2(0, 0), ImGuiChildFlags_Borders,
                                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
                drawWwiseSectionTitle(wz("Selected Row Properties", "Selected Row Properties"),
                                      th.wwiseLeafBadge,
                                      hasSel
                                          ? wz("Focused editing for the currently selected structure row.",
                                               u8"当前选中结构行的聚焦编辑区。")
                                          : wz("Select a row above to start editing.",
                                               u8"先在上方选择一行，再开始编辑。"));

                if (!hasSel) {
                    drawSectionHint(wz("Select a structure row to edit its type, naming, and template behavior.",
                                       "Select a structure row to edit its type, naming, and template behavior."));
                } else {
                    auto& layer = cfg.structure[static_cast<std::size_t>(s_selectedRow)];
                    const bool isLeafRow = (s_selectedRow + 1 == static_cast<int>(cfg.structure.size()));
                    const float fieldGap = 10.0f;
                    const float typeFieldWidth = 240.0f;

                    drawFieldLabel(wz("Object Type", "Object Type"));
                    ImGui::SetNextItemWidth(typeFieldWidth);
                    if (isLeafRow) {
                        layer.type = WwiseObjectType::SoundSFX;
                        int leafIdx = objectTypeIndex(WwiseObjectType::SoundSFX);
                        const char* leafLabels[8];
                        for (int idx = 0; idx < kObjectTypeCount; ++idx) {
                            leafLabels[idx] = objectTypeLabelAt(idx, false);
                        }
                        ImGui::BeginDisabled();
                        aura::ui::Combo("##selectedRowType", &leafIdx, leafLabels, kObjectTypeCount);
                        ImGui::EndDisabled();
                    } else {
                        int typeIdx = parentObjectTypeIndex(layer.type);
                        const char* parentLabels[7];
                        for (int idx = 0; idx < kParentObjectTypeCount; ++idx) {
                            parentLabels[idx] = objectTypeLabelAt(idx, true);
                        }
                        if (aura::ui::Combo("##selectedRowType", &typeIdx, parentLabels, kParentObjectTypeCount)) {
                            layer.type = parentObjectTypeFromIndex(typeIdx);
                        }
                    }

                    ImGui::Spacing();
                    drawFieldLabel(wz("Object Name", "Object Name"));
                    {
                        const float wildcardButtonWidth = 32.0f;
                        char nameBuf[256];
                        std::snprintf(nameBuf, sizeof(nameBuf), "%s", layer.nameTemplate.c_str());
                        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - wildcardButtonWidth - fieldGap);
                        if (ImGui::InputText("##selectedRowName", nameBuf, sizeof(nameBuf))) {
                            layer.nameTemplate = nameBuf;
                        }
                        ImGui::SameLine(0, fieldGap * 0.5f);
                        if (aura::ui::Button("W##selectedRowWildcard",
                                aura::ui::ButtonOptions{aura::ui::ButtonVariant::Ghost,
                                                        aura::ui::ButtonSize::Sm})) {
                            ImGui::OpenPopup("selectedRowWildcardPopup");
                        }
                        if (ImGui::BeginPopup("selectedRowWildcardPopup")) {
                            renderWildcardsMenu(&layer.nameTemplate);
                            ImGui::EndPopup();
                        }
                    }

                    ImGui::Spacing();
                    bool useTemplate = layer.useTemplate;
                    if (ImGui::Checkbox(wz("Use Property Template", "Use Property Template"), &useTemplate)) {
                        layer.useTemplate = useTemplate;
                    }

                    ImGui::Spacing();
                    drawFieldLabel(wz("Property Template Source", "Property Template Source"));
                    {
                        const float pickButtonWidth = 34.0f;
                        char tplBuf[512];
                        std::snprintf(tplBuf, sizeof(tplBuf), "%s", layer.propertyTemplatePath.c_str());
                        ImGui::BeginDisabled(!layer.useTemplate);
                        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - pickButtonWidth - fieldGap);
                        if (ImGui::InputText("##selectedRowTemplate", tplBuf, sizeof(tplBuf))) {
                            layer.propertyTemplatePath = tplBuf;
                        }
                        ImGui::SameLine(0, fieldGap * 0.5f);
                        if (aura::ui::Button("##pickSelectedRowTemplate",
                                aura::ui::ButtonOptions{aura::ui::ButtonVariant::Ghost,
                                                        aura::ui::ButtonSize::Sm,
                                                        ICON_LC_DOWNLOAD,
                                                        selBusy,
                                                        !layer.useTemplate || !canPickTpl})) {
                            startPickRowTemplate(s_selectedRow);
                            g_ps.lastActionMessage = "Querying Wwise selection...";
                        }
                        ImGui::EndDisabled();
                    }

                    if (isLeafRow) {
                        ImGui::TextDisabled("%s", wz("The last row stays SoundSFX so the imported file always lands on a playable leaf.",
                                                     "The last row stays SoundSFX so the imported file always lands on a playable leaf."));
                    }
                }
                }
                // BeginChild still opens a scope when the properties are clipped.
                ImGui::EndChild();
                ImGui::PopStyleVar(2);
                ImGui::PopStyleColor(2);

                if (removeIdx >= 0) {
                    cfg.structure.erase(cfg.structure.begin() + removeIdx);
                    ensureTerminalSoundLayer(cfg.structure);
                    int newSel = removeIdx;
                    if (newSel >= static_cast<int>(cfg.structure.size())) {
                        newSel = static_cast<int>(cfg.structure.size()) - 1;
                    }
                    if (newSel >= 0 && newSel + 1 == static_cast<int>(cfg.structure.size())) {
                        newSel -= 1;
                    }
                    s_selectedRow = (newSel >= 0) ? newSel : -1;
                } else if (moveUpIdx > 0) {
                    std::swap(cfg.structure[moveUpIdx], cfg.structure[moveUpIdx - 1]);
                    ensureTerminalSoundLayer(cfg.structure);
                    s_selectedRow = moveUpIdx - 1;
                } else if (moveDownIdx >= 0 && moveDownIdx + 1 < static_cast<int>(cfg.structure.size())) {
                    std::swap(cfg.structure[moveDownIdx], cfg.structure[moveDownIdx + 1]);
                    ensureTerminalSoundLayer(cfg.structure);
                    s_selectedRow = moveDownIdx + 1;
                }
            }

            aura::ui::EndPanel();
        }

        }
        ImGui::EndChild();
        ImGui::SameLine(0, splitGap);
        const bool railVisible = ImGui::BeginChild("wwiseTransferRailColumn", ImVec2(0, workspaceHeight), false, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        if (railVisible) {
        if (aura::ui::BeginPanel("wwiseTransferActionPanel", aura::ui::PanelVariant::Peer, 0.0f, actionPanelHeight, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
            ImGui::AlignTextToFramePadding();
            ImGui::PushFont(GetUIFontEmphasis());
            ImGui::TextUnformatted(wz("Transfer", u8"执行传输"));
            ImGui::PopFont();
            ImGui::SameLine(0, 10.0f);
            drawStatChip(wz("Staged", u8"已暂存"), static_cast<int>(g_ps.sourceWavs.size()),
                         withAlpha(th.wwiseDiffNeutral, kWwiseAlphaMid), 76.0f);
            ImGui::SameLine(0, 8.0f);
            drawStatChip(wz("Snapshot", u8"快照"), g_ps.hasCachedExisting ? wz("Ready", u8"就绪") : wz("Missing", u8"缺失"),
                         withAlpha(th.wwiseDiffNeutral, kWwiseAlphaMid),
                         96.0f);
            aura::ui::Divider(aura::ui::DividerVariant::Inset);

            if (aura::ui::Button(wz("Transfer to Wwise", u8"传输到 Wwise"),
                    aura::ui::ButtonOptions{aura::ui::ButtonVariant::PrimaryTransfer,
                                            aura::ui::ButtonSize::Md,
                                            ICON_LC_SEND})) handleTransfer();

            ImGui::SameLine(0, 10.0f);
            const bool refreshing =
                g_async.active.load() && g_async.kind == AsyncKind::RefreshFromWwise;
            if (aura::ui::Button(wz("Refresh from Wwise", u8"从 Wwise 刷新"),
                    aura::ui::ButtonOptions{aura::ui::ButtonVariant::Secondary,
                                            aura::ui::ButtonSize::Md,
                                            nullptr,
                                            refreshing,
                                            refreshing})) handlePreview();
            if (ImGui::IsItemHovered() && !refreshing) {
                HarkenSearch::SetTooltip("Fetch the current Wwise object tree and Originals/SFX snapshot.");
            }

            if (!g_ps.lastActionMessage.empty()) {
                ImGui::Spacing();
                ImGui::PushStyleColor(ImGuiCol_Text,
                    ImGui::ColorConvertFloat4ToU32(withAlpha(th.menuText, 0.65f)));
                ImGui::TextWrapped("%s", localizedActionMessage(g_ps.lastActionMessage).c_str());
                ImGui::PopStyleColor();
            }

            ImGui::Spacing();
            if (aura::ui::SectionHeader(wz("Staged Sources", u8"暂存源文件"), &showStagedSources, [&]() {
                    drawStatChip(wz("Files", u8"文件"), static_cast<int>(g_ps.sourceWavs.size()),
                                 withAlpha(th.wwiseDiffNeutral, kWwiseAlphaLow), 72.0f);
                })) {
                for (const auto& s : g_ps.sourceWavs) {
                    ImGui::TextUnformatted(s.c_str());
                }
            }

            ImGui::Spacing();
            aura::ui::Divider(aura::ui::DividerVariant::Inset);
            ImGui::AlignTextToFramePadding();
            ImGui::PushFont(GetUIFontEmphasis());
            ImGui::TextUnformatted(wz("Result", u8"结果"));
            ImGui::PopFont();
            if (g_ps.hasResult) {
                ImGui::SameLine(0, 10.0f);
                drawStatChip(wz("Imported", u8"已导入"), g_ps.lastResult.imported, connectedColor, 88.0f);
                ImGui::SameLine(0, 8.0f);
                drawStatChip(wz("Skipped", u8"已跳过"), g_ps.lastResult.skipped,
                             withAlpha(th.wwiseDiffNeutral, kWwiseAlphaMid), 84.0f);
                ImGui::SameLine(0, 8.0f);
                drawStatChip(wz("Failed", u8"失败"), g_ps.lastResult.failed, disconnectedColor, 78.0f);
                ImGui::Spacing();
                if (!g_ps.lastResult.aggregateError.empty()) {
                    if (aura::ui::BeginPanel("wwiseTransferAggregateError", aura::ui::PanelVariant::Inset, 0.0f, 58.0f)) {
                        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertFloat4ToU32(disconnectedColor));
                        ImGui::TextWrapped("Transfer failed: %s", g_ps.lastResult.aggregateError.c_str());
                        ImGui::PopStyleColor();
                        aura::ui::EndPanel();
                    }
                    ImGui::Spacing();
                }
                {
                    const bool canReveal = g_ps.client && g_ps.client->isConnected();
                    for (std::size_t i = 0; i < g_ps.lastResult.items.size(); ++i) {
                        const auto& it = g_ps.lastResult.items[i];
                        ImVec4 col = withAlpha(th.menuText, 0.92f);
                        bool isSuccess = false;
                        switch (it.status) {
                            case ItemStatus::Imported:
                            case ItemStatus::TemplateApplied:
                                col = connectedColor;
                                isSuccess = true;
                                break;
                            case ItemStatus::SkippedExisting:
                                col = withAlpha(th.wwiseDiffNeutral, kWwiseAlphaMid);
                                isSuccess = true;
                                break;
                            case ItemStatus::FailedWavCopy:
                            case ItemStatus::FailedImport:
                            case ItemStatus::FailedTemplate:
                                col = disconnectedColor;
                                break;
                            default:
                                break;
                        }

                        std::string displayPath = it.targetObjectPath;
                        for (std::size_t p = 0; (p = displayPath.find("<", p)) != std::string::npos;) {
                            auto close = displayPath.find(">", p);
                            if (close == std::string::npos) break;
                            displayPath.erase(p, close - p + 1);
                        }

                        ImGui::PushID(static_cast<int>(i));
                        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertFloat4ToU32(col));
                        const std::string label = "[" + std::string(itemStatusText(it.status)) + "] " + displayPath +
                            (it.errorMessage.empty() ? "" : "  -> " + it.errorMessage);
                        const bool clicked = ImGui::Selectable(label.c_str(), false,
                            (isSuccess && canReveal) ? 0 : ImGuiSelectableFlags_Disabled);
                        ImGui::PopStyleColor();
                        if (isSuccess && clicked && canReveal && !g_async.active.load()) {
                            startReveal(displayPath);
                            g_ps.lastActionMessage = "Revealing in Wwise...";
                        }
                        ImGui::PopID();
                    }
                }
            } else {
                ImGui::Spacing();
                if (aura::ui::BeginPanel("wwiseTransferNoResult", aura::ui::PanelVariant::Inset)) {
                    drawSectionHint(wz("No transfer result yet.", u8"当前还没有传输结果。"));
                    aura::ui::EndPanel();
                }
            }

            aura::ui::EndPanel();
        }
        }
        ImGui::EndChild();
    }

    ImGui::Spacing();
    if (aura::ui::BeginPanel("wwiseTransferPreviewPanel", aura::ui::PanelVariant::Peer, 0.0f, previewPanelHeight, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(wz("Preview", u8"预览视窗"));
        if (g_ps.hasDiff) {
            DiffSummary hdrSummary;
            collectDiffSummary(g_ps.lastDiff, hdrSummary);
            ImGui::SameLine(0, 10.0f);
            drawStatChip(wz("Nodes", u8"节点"), hdrSummary.total,
                         withAlpha(th.wwiseDiffNeutral, kWwiseAlphaMid), 64.0f);
            if (hdrSummary.unresolved > 0) {
                ImGui::SameLine(0, 8.0f);
                drawStatChip(wz("Issues", u8"异常"), hdrSummary.unresolved,
                             statusColorFor(DiffStatus::Unresolved), 64.0f);
            }
        }
        aura::ui::Divider(aura::ui::DividerVariant::Inset);
        if (g_ps.hasDiff) {
            if (aura::ui::BeginPanel("wwiseTransferPreviewViewport", aura::ui::PanelVariant::Inset, 0.0f, 80.0f, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
                drawFieldLabel(wz("Viewport", u8"视图"), wz("Current transfer structure", u8"当前传输结构"));
                drawSectionHint(g_ps.bufImportDest[0]
                    ? g_ps.bufImportDest
                    : wz("No target destination selected yet.", u8"当前还没有选择导入目标。"));
                aura::ui::EndPanel();
            }
            ImGui::Spacing();
            {
                DiffSummary summary;
                collectDiffSummary(g_ps.lastDiff, summary);
                drawStatChip(wz("New", u8"新建"), summary.added, statusColorFor(DiffStatus::New), 64.0f);
                ImGui::SameLine(0, 8.0f);
                drawStatChip(wz("Replaced", u8"替换"), summary.replaced, statusColorFor(DiffStatus::Replaced), 82.0f);
                ImGui::SameLine(0, 8.0f);
                drawStatChip(wz("Issues", u8"异常"), summary.unresolved, statusColorFor(DiffStatus::Unresolved), 68.0f);
            }
            ImGui::Spacing();
            renderDiffNode(g_ps.lastDiff);
        } else {
            if (aura::ui::BeginPanel("wwiseTransferPreviewEmpty", aura::ui::PanelVariant::Inset, 0.0f, 80.0f, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
                drawFieldLabel(wz("Viewport", u8"视图"), wz("No preview available", u8"当前没有可用预览"));
                drawSectionHint(wz("Refresh Wwise once, then the structure tree will appear here.",
                                   u8"先从 Wwise 刷新一次，结构树就会显示在这里。"));
                aura::ui::EndPanel();
            }
        }
        aura::ui::EndPanel();
    }

    ImGui::End();
    g_ps.visible = open;
}

void ShutdownWwiseTransferPanel() {
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_ps.client) g_ps.client->disconnect();
    g_ps.client.reset();
    g_ps.visible = false;
}

} // namespace wwise
}
