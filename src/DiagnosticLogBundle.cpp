#include "DiagnosticLogBundle.h"

#include "CueviFeedbackBuildConfig.h"
#include "DiagnosticBundleMinizDecls.h"
#include "DiagnosticsLogger.h"
#include "Version.h"
#include "nlohmann_json.hpp"
#include "platform/diagnostic_log_bundle_file.h"

#include <algorithm>
#include <cmath>
#include <ctime>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

namespace HarkenSearch {
namespace {
namespace fs = std::filesystem;
using Json = nlohmann::json;
using Status = DiagnosticLogBundleStatus;
using Type = DiagLogType;

struct EventRule {
    Type type;
    std::string_view category;
    std::string_view detail;
    const char* event;
    bool prefix = false;
    const char* exportCategory = nullptr;
};

constexpr EventRule kEvents[]{
    {Type::Action, "Search", "ExplainTooltip source=", "ExplainTooltip", true},
    {Type::Action, "Search", "InconsistentRefreshTotalRepair: oldReq=", "InconsistentRefreshTotalRepair", true},
    {Type::Action, "Search", "CountAndFacetWarmStartZeroTotalDeferred: req=", "CountAndFacetWarmStartZeroTotalDeferred", true},
    {Type::Action, "Search", "Async Random Shuffle Triggered", "AsyncRandomShuffleTriggered"},
    {Type::Performance, "Database.searchFiles", "", "searchFiles", false, "Database"},
    {Type::Performance, "Database.searchFilesCount", "", "searchFilesCount", false, "Database"},
    {Type::Performance, "Database.rebuildFtsIndex", "", "rebuildFtsIndex", false, "Database"},
    {Type::Performance, "Database.rebuildFtsIndexBatched", "", "rebuildFtsIndexBatched", false, "Database"},
    {Type::Performance, "Database.preheat", "", "preheat", false, "Database"},
    {Type::System, "Database", "SwitchDatabaseStart: from=", "SwitchDatabaseStart", true},
    {Type::System, "Database", "SwitchDatabaseComplete: success=", "SwitchDatabaseComplete", true},
    {Type::System, "Scan", "ScanStart: ", "ScanStart", true},
    {Type::System, "Scan", "ScanComplete: total=", "ScanComplete", true},
    {Type::System, "Scan", "ScanDatabaseOpenFailed: error=", "ScanDatabaseOpenFailed", true},
    {Type::System, "Scan", "WaveformCacheDrainStart: scoped=", "WaveformCacheDrainStart", true},
    {Type::System, "Scan", "ReadyToWorkCoverFirstScreenSatisfied: db=", "ReadyToWorkCoverFirstScreenSatisfied", true},
    {Type::Performance, "Scan", "refreshAfterCommit", "refreshAfterCommit"},
    {Type::Action, "Waveform", "MainDetailRequested path=", "MainDetailRequested", true},
    {Type::System, "Waveform", "MainDetailDecodeStart: path=", "MainDetailDecodeStart", true},
    {Type::Performance, "Waveform", "buildWaveform", "buildWaveform"},
    {Type::Performance, "Waveform", "buildQuickWaveform", "buildQuickWaveform"},
    {Type::Performance, "Waveform", "ProgressiveMainWaveDecode", "ProgressiveMainWaveDecode"},
    {Type::Performance, "Waveform", "MainDetailPrewarmDecode", "MainDetailPrewarmDecode"},
    {Type::Metric, "Waveform", "MainDetailCacheHit", "MainDetailCacheHit"},
    {Type::Metric, "Waveform", "MainDetailCacheMiss", "MainDetailCacheMiss"},
    {Type::Metric, "Waveform", "MainDetailReady", "MainDetailReady"},
    {Type::Action, "Audio", "LoadFile: ", "LoadFile", true},
    {Type::Performance, "Audio", "loadFile", "loadFile"},
    {Type::Performance, "Audio", "loadFileAsyncApply", "loadFileAsyncApply"},
    {Type::Performance, "Audio", "play", "play"},
    {Type::Action, "UI", "Browser All-Select Executed", "BrowserAllSelect"},
    {Type::Action, "UI", "Physical All-Select Executed", "PhysicalAllSelect"},
    {Type::Metric, "Frame", "FPS", "FPS"},
    {Type::Metric, "Pointer", "FrameSpikeMs", "FrameSpikeMs"},
    {Type::Performance, "WebView", "FrameOverhead", "FrameOverhead"},
};

constexpr const char* kCategories[]{"Search", "Scan", "Database", "Waveform",
                                    "Audio", "UI", "Frame", "Pointer", "WebView"};

template <size_t Size>
std::string_view boundedString(const char (&value)[Size]) noexcept {
    return {value, static_cast<size_t>(std::find(value, value + Size, '\0') - value)};
}

const char* typeName(Type type) noexcept {
    switch (type) {
    case Type::Action: return "action";
    case Type::Performance: return "performance";
    case Type::Metric: return "metric";
    case Type::System: return "system";
    }
    return "unknown";
}

const char* platformName() noexcept {
#if defined(_WIN32)
    return "windows";
#elif defined(__APPLE__)
    return "macos";
#elif defined(__linux__)
    return "linux";
#else
    return "other";
#endif
}

Json safeBuildLabel(std::string_view label) {
    if (label.empty() || label.size() > 96 || !std::all_of(label.begin(), label.end(), [](char ch) {
            return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                   (ch >= '0' && ch <= '9') || ch == '.' || ch == '-' || ch == '_' || ch == '+';
        })) return nullptr;
    return std::string(label);
}

Json publicRow(const DiagLogEntry& entry, int64_t clickMs, size_t threadGroup) {
    const auto category = boundedString(entry.category);
    const auto detail = boundedString(entry.detail);
    const char* publicCategory = "Other";
    for (const char* allowed : kCategories) {
        if (category == allowed) {
            publicCategory = allowed;
            break;
        }
    }
    const EventRule* match = nullptr;
    for (const auto& rule : kEvents) {
        if (category != rule.category) continue;
        if (rule.exportCategory) publicCategory = rule.exportCategory;
        if (entry.type != rule.type) continue;
        if (rule.prefix ? detail.substr(0, rule.detail.size()) == rule.detail : detail == rule.detail) {
            match = &rule;
            break;
        }
    }

    Json result = {{"offset_ms", entry.timestampMs - clickMs}, {"type", typeName(entry.type)},
                   {"category", publicCategory}, {"repeat", entry.repeatCount}, {"thread_group", threadGroup}};
    if (match) {
        result["event"] = match->event;
        if (entry.type == Type::Performance && std::isfinite(entry.durationMs) && entry.durationMs >= 0)
            result["duration_ms"] = entry.durationMs;
        if (entry.type == Type::Metric && std::isfinite(entry.value))
            result["value"] = entry.value;
    }
    return result;
}

struct BundleJson {
    std::string manifest;
    std::string logs;
    bool empty;
};

BundleJson serialize(const DiagLogSnapshot& snapshot, DiagnosticLogBuildLabels labels) {
    const auto startMs = DiagLogSnapshot::windowStart(snapshot.clickMs);
    std::vector<const DiagLogEntry*> ordered;
    ordered.reserve(snapshot.entries.size());
    for (const auto& entry : snapshot.entries) {
        if (entry.timestampMs >= startMs && entry.timestampMs <= snapshot.clickMs)
            ordered.push_back(&entry);
    }
    std::stable_sort(ordered.begin(), ordered.end(), [](const auto* before, const auto* after) {
        return before->timestampMs < after->timestampMs;
    });

    Json logs = Json::array();
    logs.get_ref<Json::array_t&>().reserve(ordered.size());
    std::unordered_map<uint32_t, size_t> threadGroups;
    for (const auto* entry : ordered) {
        const auto group = threadGroups.try_emplace(entry->threadId, threadGroups.size() + 1);
        logs.push_back(publicRow(*entry, snapshot.clickMs, group.first->second));
    }
    const bool empty = logs.empty();
    const Json manifest = {
        {"schema", "harken.diagnostic-log-bundle.v1"},
        {"version", safeBuildLabel(labels.version)},
        {"build_id", safeBuildLabel(labels.buildId)},
        {"platform", platformName()},
        {"requested", {{"start_ms", startMs}, {"end_ms", snapshot.clickMs}, {"inclusive", true}}},
        {"coverage", {{"rows", logs.size()},
                      {"first_ms", empty ? Json(nullptr) : Json(ordered.front()->timestampMs)},
                      {"last_ms", empty ? Json(nullptr) : Json(ordered.back()->timestampMs)}}},
        {"diagnostics_enabled", snapshot.enabled},
        {"category_filter_enabled", snapshot.categoryFilterEnabled},
        {"ring_capacity", snapshot.ringCapacity},
        {"process_dropped_entries", snapshot.processDroppedEntries},
        {"unknown_labels", {{"category", "Other"}, {"event", "omitted"}, {"measurements", "omitted"}}},
        {"limits", {
            "Only rows retained in the in-memory ring at capture are included; pending queue entries are not read.",
            "Coverage is the range of included rows, not proof of continuous capture; empty coverage has null endpoints.",
            "Each repeat is the saved merged count; it is not a count of occurrences within this window.",
            "Dropped entries is a process total, not a window loss count. The flags reflect the capture time.",
        }},
    };
    return {manifest.dump(), logs.dump(), empty};
}

using ArchiveBytes = std::unique_ptr<void, decltype(&mz_free)>;

ArchiveBytes compress(const BundleJson& bundle, size_t& size) {
    std::tm fixedDate{};
    fixedDate.tm_year = 80;
    fixedDate.tm_mday = 2;
    fixedDate.tm_isdst = -1;
    auto modified = std::mktime(&fixedDate);
    mz_zip_archive archive{};
    if (modified == static_cast<std::time_t>(-1) || !mz_zip_writer_init_heap(&archive, 0, 0))
        return {nullptr, &mz_free};
    const auto add = [&](const char* name, const std::string& json) {
        return mz_zip_writer_add_mem_ex_v2(&archive, name, json.data(), json.size(),
                                          nullptr, 0, MZ_BEST_SPEED, 0, 0, &modified,
                                          nullptr, 0, nullptr, 0);
    };
    void* bytes = nullptr;
    const bool ok = add("manifest.json", bundle.manifest) && add("logs.json", bundle.logs) &&
                    mz_zip_writer_finalize_heap_archive(&archive, &bytes, &size);
    mz_zip_writer_end(&archive);
    ArchiveBytes result(bytes, &mz_free);
    if (!ok) result.reset();
    return result;
}

} // namespace

DiagnosticLogBundleResult WriteDiagnosticLogBundle(
    const DiagLogSnapshot& snapshot, const fs::path& root,
    std::optional<DiagnosticLogBuildLabels> labels) noexcept {
    if (root.empty()) return {Status::InvalidRoot, {}};
    try {
        const auto bundle = serialize(snapshot, labels.value_or(
            DiagnosticLogBuildLabels{SOUNDMANAGER_VERSION_STRING, AIRS_BUILD_ID}));
        size_t size = 0;
        const auto archive = compress(bundle, size);
        if (!archive || !size) return {Status::EncodeFailed, {}};
        return Platform::DiagnosticLogBundleFile::Publish(
            root, snapshot.clickMs, archive.get(), size, bundle.empty);
    } catch (...) {
        return {Status::EncodeFailed, {}};
    }
}

} // namespace HarkenSearch
