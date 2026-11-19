#include "DiagnosticLogBundle.h"
#include "DiagnosticsLogger.h"
#include "nlohmann_json.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <future>
#include <iostream>
#include <iterator>
#include <limits>
#include <memory>
#include <stdexcept>

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable: 4334)
#endif
#include "../thirdparty/rlottie/src/lottie/zip/miniz.h"
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

struct DiagnosticsRetentionTestAccess {
    static void seed(DiagnosticsLogger& logger, const std::vector<DiagLogEntry>& entries) {
        logger.clear();
        logger.setMaxEntries(128);
        std::lock_guard<std::mutex> lock(logger.m_mutex);
        const size_t start = 117;
        for (size_t index = 0; index < entries.size(); ++index)
            logger.m_ring[(start + index) % logger.m_maxEntries] = entries[index];
        logger.m_count = entries.size();
        logger.m_head = (start + entries.size()) % logger.m_maxEntries;
        logger.m_timestampsOrdered = false;
        logger.m_enabled.store(true);
        logger.m_dropped.store(1234);
    }

    static bool enqueue(DiagnosticsLogger& logger, const DiagLogEntry& entry) {
        return logger.enqueue(entry);
    }

    static std::unique_lock<std::mutex> lock(DiagnosticsLogger& logger, bool filter) {
        return std::unique_lock<std::mutex>(filter ? logger.m_filterMutex : logger.m_mutex);
    }
};

namespace {
namespace fs = std::filesystem;
using Json = nlohmann::json;
using HarkenSearch::DiagnosticLogBundleStatus;
using HarkenSearch::WriteDiagnosticLogBundle;

constexpr int64_t kClick = 2000000;
int failures = 0;

void check(bool condition, const char* message) {
    if (condition) return;
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
}

DiagLogEntry entry(int64_t offset, DiagLogType type, const char* category,
                   const char* detail, uint32_t threadId = 123456789) {
    DiagLogEntry result;
    result.timestampMs = kClick + offset;
    result.type = type;
    std::snprintf(result.category, sizeof(result.category), "%s", category);
    std::snprintf(result.detail, sizeof(result.detail), "%s", detail);
    result.threadId = threadId;
    return result;
}

struct TestDirectory {
    fs::path path = fs::current_path() / fs::u8path(u8"harken-\u65e5\u5fd7-test-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    bool owned = fs::create_directory(path);

    ~TestDirectory() {
        if (!owned) return;
        std::error_code error;
        for (const auto& child : fs::directory_iterator(path, error))
            fs::remove(child.path(), error);
        fs::remove(path, error);
    }
};

struct Archive {
    std::string manifest;
    std::string logs;
};

std::string fileBytes(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

Archive unzip(const fs::path& path) {
    const auto bytes = fileBytes(path);
    mz_zip_archive zip{};
    if (!mz_zip_reader_init_mem(&zip, bytes.data(), bytes.size(), 0)) {
        check(false, "bundle is a readable ZIP");
        return {};
    }
    check(mz_zip_reader_get_num_files(&zip) == 2, "ZIP contains only two files");
    auto read = [&](const char* name) -> std::string {
        size_t size = 0;
        std::unique_ptr<void, decltype(&mz_free)> data(
            mz_zip_reader_extract_file_to_heap(&zip, name, &size, 0), &mz_free);
        check(data != nullptr, name);
        return data ? std::string(static_cast<const char*>(data.get()), size) : std::string{};
    };
    Archive result{read("manifest.json"), read("logs.json")};
    mz_zip_reader_end(&zip);
    return result;
}

void checkEmpty(DiagnosticsLogger& logger, const fs::path& root) {
    DiagLogSnapshot snapshot;
    check(logger.trySnapshotForExport(kClick, snapshot), "empty ring captures successfully");
    const auto bundle = WriteDiagnosticLogBundle(snapshot, root);
    check(bundle.status == DiagnosticLogBundleStatus::Written, "empty snapshot writes a valid bundle");
    if (bundle.status != DiagnosticLogBundleStatus::Written) return;
    const auto archive = unzip(bundle.path);
    const auto manifest = Json::parse(archive.manifest);
    const auto logs = Json::parse(archive.logs);
    const auto& coverage = manifest.at("coverage");
    check(bundle.empty && logs.is_array() && logs.empty() && coverage.at("rows") == 0 &&
          coverage.at("first_ms").is_null() && coverage.at("last_ms").is_null(),
          "empty coverage has an empty array and null endpoints");
    check(manifest.at("diagnostics_enabled") == false &&
          manifest.at("category_filter_enabled") == false &&
          manifest.at("version").is_string() && manifest.at("build_id").is_string(),
          "empty manifest still captures disabled flags and default build identity");
}

void checkBusy(DiagnosticsLogger& logger, DiagLogSnapshot& snapshot) {
    for (bool filter : {false, true}) {
        auto held = DiagnosticsRetentionTestAccess::lock(logger, filter);
        auto request = std::async(std::launch::async, [&] {
            return logger.trySnapshotForExport(kClick + 100, snapshot);
        });
        const bool ready = request.wait_for(std::chrono::seconds(1)) == std::future_status::ready;
        held.unlock();
        const bool captured = request.get();
        check(ready && !captured, "capture returns busy rather than waiting for either lock");
        check(snapshot.clickMs == kClick && snapshot.entries.size() == 81,
              "busy does not replace a captured snapshot");
    }
}

void checkWindowAndPrivacy(DiagnosticsLogger& logger, const fs::path& root) {
    std::vector<DiagLogEntry> entries{
        entry(1, DiagLogType::Metric, "Frame", "FPS"),
        entry(-5, DiagLogType::Action, "Search", "ExplainTooltip source=private-query"),
        entry(-60000, DiagLogType::System, "Scan", "ScanDatabaseOpenFailed: error=private-error"),
        entry(-60001, DiagLogType::Performance, "Audio", "play"),
        entry(-1, DiagLogType::Performance, "Audio", "loadFile", 44),
        entry(0, DiagLogType::Metric, "Frame", "FPS"),
        entry(-22, DiagLogType::Action, "Search", "ExplainTooltip source=private-detail"),
        entry(-60, DiagLogType::Action, "Search", "ExplainTooltip source: private-near-prefix"),
        entry(-20, DiagLogType::System, "private-category", "C:\\Users\\private-user\\music"),
        entry(-21, DiagLogType::Metric, "Waveform", "MainDetailCacheHit"),
        entry(-23, DiagLogType::Performance, "Database.searchFiles", ""),
        entry(-24, DiagLogType::Metric, "Frame", "FPS"),
        entry(-25, DiagLogType::Metric, "private-metric", "private-value"),
    };
    entries[2].repeatCount = 8;
    entries[4].durationMs = 2.5;
    entries[5].value = 59.4;
    entries[9].value = 1;
    entries[10].durationMs = std::numeric_limits<double>::infinity();
    entries[11].value = std::numeric_limits<double>::quiet_NaN();
    entries[12].value = 7654321;
    for (int index = 0; index < 70; ++index)
        entries.push_back(entry(-10000 - index, DiagLogType::Performance, "Waveform", "buildWaveform"));
    logger.setCategoryFilter("private-filter");
    DiagnosticsRetentionTestAccess::seed(logger, entries);
    check(DiagnosticsRetentionTestAccess::enqueue(
          logger, entry(-1, DiagLogType::Action, "Search", "Async Random Shuffle Triggered")),
          "fixture includes an in-window queue entry, not yet in the ring");
    DiagLogSnapshot snapshot;
    check(logger.trySnapshotForExport(kClick, snapshot) && snapshot.entries.size() == 81,
          "wrapped ring captures more than 50 rows and excludes the queue");
    checkBusy(logger, snapshot);
    logger.setEnabled(false);
    logger.setCategoryFilter("");
    logger.clear();

    const auto bundle = WriteDiagnosticLogBundle(snapshot, root, {{"1.2.3-test", "test-build"}});
    check(bundle.status == DiagnosticLogBundleStatus::Written, "non-empty snapshot writes a ZIP");
    if (bundle.status != DiagnosticLogBundleStatus::Written) return;
    check(!bundle.empty && bundle.path.parent_path() == root && bundle.path.extension() == ".zip",
          "non-empty status and ZIP path work in a Chinese directory");
    const auto archive = unzip(bundle.path);
    const auto manifest = Json::parse(archive.manifest);
    const auto logs = Json::parse(archive.logs);
    const auto& requested = manifest.at("requested");
    const auto& coverage = manifest.at("coverage");
    check(manifest.at("schema") == "harken.diagnostic-log-bundle.v1" &&
          manifest.at("version") == "1.2.3-test" && manifest.at("build_id") == "test-build",
          "manifest is versioned and uses supplied build labels");
    check(requested.at("start_ms") == kClick - 60000 && requested.at("end_ms") == kClick &&
          coverage.at("first_ms") == kClick - 60000 && coverage.at("last_ms") == kClick &&
          coverage.at("rows") == 81 && logs.size() == 81, "closed window and actual coverage match");
    check(std::is_sorted(logs.begin(), logs.end(), [](const Json& before, const Json& after) {
        return before.at("offset_ms") < after.at("offset_ms");
    }), "out-of-order ring rows form a chronological export");
    check(manifest.at("ring_capacity") == 128 && manifest.at("process_dropped_entries") == 1234 &&
          manifest.at("diagnostics_enabled") == true && manifest.at("category_filter_enabled") == true,
          "manifest records snapshot flags, capacity and process total");
    for (const auto* forbidden : {"private-", "123456789", "7654321", "AsyncRandomShuffleTriggered"})
        check(archive.logs.find(forbidden) == std::string::npos &&
              archive.manifest.find(forbidden) == std::string::npos, "sensitive or pending data is absent");
    const auto at = [&](int offset) -> const Json& {
        const auto found = std::find_if(logs.begin(), logs.end(), [&](const Json& row) {
            return row.at("offset_ms") == offset;
        });
        if (found == logs.end()) throw std::runtime_error("expected offset missing");
        return *found;
    };
    check(at(-5).at("event") == "ExplainTooltip" && at(-22).at("event") == "ExplainTooltip" &&
          !at(-60).contains("event") && !at(-20).contains("event") &&
          at(-20).at("category") == "Other", "exact prefix and unknown-label rules");
    check(at(-60000).at("event") == "ScanDatabaseOpenFailed" && at(-60000).at("repeat") == 8 &&
          at(-1).at("duration_ms") == 2.5 && at(0).at("value") == 59.4 &&
          at(-21).at("event") == "MainDetailCacheHit" && at(-21).at("value") == 1 &&
          at(-23).at("event") == "searchFiles" && !at(-23).contains("duration_ms") &&
          !at(-24).contains("value") && !at(-25).contains("value"),
          "only reviewed, finite measurements are exported");
    check(at(-5).at("thread_group") == at(0).at("thread_group") &&
          at(-1).at("thread_group") != at(0).at("thread_group"), "threads are grouped per bundle");

    const auto originalBytes = fileBytes(bundle.path);
    const auto next = WriteDiagnosticLogBundle(snapshot, root, {{"C:\\Users\\private-user", "private@host"}});
    check(next.status == DiagnosticLogBundleStatus::Written && next.path != bundle.path &&
          fileBytes(bundle.path) == originalBytes, "same click publishes a distinct ZIP without overwriting");
    if (next.status == DiagnosticLogBundleStatus::Written) {
        const auto nextManifest = Json::parse(unzip(next.path).manifest);
        check(nextManifest.at("version").is_null() && nextManifest.at("build_id").is_null(),
              "unsafe injected build labels are not serialized");
    }
    const auto badRoot = root / "not-a-directory";
    std::ofstream(badRoot, std::ios::binary) << "unchanged";
    const auto failed = WriteDiagnosticLogBundle(snapshot, badRoot);
    check(failed.status != DiagnosticLogBundleStatus::Written && failed.path.empty() &&
          fileBytes(badRoot) == "unchanged", "failed write reports no path and preserves existing files");
    for (const auto& file : fs::directory_iterator(root))
        check(file.path() == badRoot || file.path().extension() == ".zip", "no temporary files remain");
}

void checkDiagnosticsCoverage(DiagnosticsLogger& logger, const fs::path& root) {
    std::vector<DiagLogEntry> entries{
        entry(-1, DiagLogType::Metric, "Memory", "WorkingSetMB"),
        entry(-2, DiagLogType::Performance, "Cover", "PumpCoverTextureUploads"),
        entry(-3, DiagLogType::Performance, "Cover", "PumpDeferredCoverPriming"),
        entry(-4, DiagLogType::Performance, "DAW", "AsyncRunningStateProbe"),
        entry(-5, DiagLogType::Performance, "Audio", "loadFileAsyncInitDecoder"),
        entry(-6, DiagLogType::Action, "MiniWave", "tex hit=7 miss=2 upload=5 uploadUs=34.50 pendingTex=3"),
        entry(-7, DiagLogType::Action, "UI", "Click.Left x=10 y=20 win=main item=button"),
        entry(-8, DiagLogType::Action, "MiniWave", "MiniCacheMiss reason=no_row wantPeaks=800"),
        entry(-9, DiagLogType::Metric, "private-memory", "private-value"),
        entry(-10, DiagLogType::Metric, "Cover", "EmbeddedCacheSize"),
        entry(-11, DiagLogType::Metric, "Cover", "UploadQueuedKeysSize"),
        entry(-12, DiagLogType::Action, "Audio", "DecoderPrewarmSkippedByForeground: reason=no_anchor"),
    };
    entries[0].value = 812.5;
    entries[1].durationMs = 1.25;
    entries[2].durationMs = 3.5;
    entries[3].durationMs = 412.75;
    entries[4].durationMs = 8.5;
    entries[8].value = 1;
    entries[9].value = 42;
    entries[10].value = 7;
    DiagnosticsRetentionTestAccess::seed(logger, entries);
    DiagLogSnapshot snapshot;
    check(logger.trySnapshotForExport(kClick, snapshot) && snapshot.entries.size() == entries.size(),
          "coverage fixture captures every seeded row");
    logger.clear();

    const auto bundle = WriteDiagnosticLogBundle(snapshot, root, {{"1.2.3-test", "test-build"}});
    check(bundle.status == DiagnosticLogBundleStatus::Written, "coverage snapshot writes a ZIP");
    if (bundle.status != DiagnosticLogBundleStatus::Written) return;
    const auto archive = unzip(bundle.path);
    const auto logs = Json::parse(archive.logs);
    check(logs.size() == entries.size(), "coverage export keeps every row");
    const auto at = [&](int offset) -> const Json& {
        const auto found = std::find_if(logs.begin(), logs.end(), [&](const Json& row) {
            return row.at("offset_ms") == offset;
        });
        if (found == logs.end()) throw std::runtime_error("expected coverage offset missing");
        return *found;
    };
    check(at(-1).at("event") == "WorkingSetMB" && at(-1).at("value") == 812.5,
          "working-set metric keeps its value");
    check(at(-2).at("event") == "CoverTextureUploads" && at(-2).at("duration_ms") == 1.25 &&
          at(-3).at("event") == "CoverDeferredPriming" && at(-3).at("duration_ms") == 3.5,
          "cover cache pump keeps stable names and durations");
    check(at(-4).at("event") == "DawRunningStateProbe" && at(-4).at("duration_ms") == 412.75,
          "DAW running-state probe keeps its duration");
    check(at(-5).at("event") == "loadFileAsyncInitDecoder" && at(-5).at("duration_ms") == 8.5,
          "audio decoder init keeps its duration");
    check(at(-6).at("category") == "MiniWave" && at(-6).at("event") == "MiniWaveTextureFlush",
          "periodic mini-wave texture flush keeps a stable name");
    check(at(-7).at("event") == "UiClickLeft" &&
          at(-8).at("category") == "MiniWave" && !at(-8).contains("event") &&
          at(-9).at("category") == "Other" && !at(-9).contains("value"),
          "known UI clicks name the event while unknown labels stay opaque");
    check(at(-10).at("event") == "EmbeddedCacheSize" && at(-10).at("value") == 42 &&
          at(-11).at("event") == "UploadQueuedKeysSize" && at(-11).at("value") == 7,
          "cover cache metrics keep their values");
    check(at(-12).at("category") == "Audio" &&
          at(-12).at("event") == "DecoderPrewarmSkippedByForeground",
          "audio prewarm skips keep a stable name");
    for (const auto* forbidden : {"hit=7", "x=10", "reason=", "private-memory"})
        check(archive.logs.find(forbidden) == std::string::npos,
              "expanded coverage keeps raw details out of the ZIP");
}

void checkConcurrentPublication(const fs::path& root) {
    DiagLogSnapshot snapshot;
    snapshot.clickMs = kClick + 1000;
    const auto existing = WriteDiagnosticLogBundle(snapshot, root);
    if (existing.status != DiagnosticLogBundleStatus::Written) {
        check(false, "collision fixture publishes the first ZIP");
        return;
    }
    auto staging = existing.path;
    staging.replace_extension(".tmp");
    std::ofstream(staging, std::ios::binary) << "staging-owned-by-someone-else";
    const auto priorBytes = fileBytes(existing.path);
    auto first = std::async(std::launch::async, [&] { return WriteDiagnosticLogBundle(snapshot, root); });
    auto second = std::async(std::launch::async, [&] { return WriteDiagnosticLogBundle(snapshot, root); });
    const auto firstBundle = first.get();
    const auto secondBundle = second.get();
    check(firstBundle.status == DiagnosticLogBundleStatus::Written &&
          secondBundle.status == DiagnosticLogBundleStatus::Written &&
          firstBundle.path != secondBundle.path && firstBundle.path != existing.path &&
          secondBundle.path != existing.path, "simultaneous writes publish distinct archives");
    check(fileBytes(existing.path) == priorBytes &&
          fileBytes(staging) == "staging-owned-by-someone-else", "existing ZIP and staging files are untouched");
    if (firstBundle.status == DiagnosticLogBundleStatus::Written) unzip(firstBundle.path);
    if (secondBundle.status == DiagnosticLogBundleStatus::Written) unzip(secondBundle.path);
    for (const auto& file : fs::directory_iterator(root))
        check(file.path().extension() != ".tmp" || file.path() == staging,
              "concurrent writes clean their own staging files");
}
}

int main() {
    TestDirectory root;
    if (!root.owned) return 1;
    try {
        auto& logger = DiagnosticsLogger::instance();
        checkEmpty(logger, root.path);
        checkWindowAndPrivacy(logger, root.path);
        checkDiagnosticsCoverage(logger, root.path);
        checkConcurrentPublication(root.path);
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
    if (!failures) std::cout << "PASS: diagnostic log snapshot and ZIP bundle\n";
    return failures == 0 ? 0 : 1;
}
