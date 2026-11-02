// test_import_precompute_waveform.cpp — T-297
// Verify Database scans a tiny WAV and the precompute worker actually
// writes mini + detail waveform blobs (status = "done"), not just the
// "pending" marker. Also covers the cache-hit path: a second
// getWaveformByPath() returns non-empty data without triggering further
// extraction.

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "src/Database.h"
#include "src/DiagnosticsLogger.h"
#include "src/WaveformCachePolicy.h"

namespace fs = std::filesystem;

static int g_failures = 0;
static void check(bool cond, const char* desc) {
    if (cond) {
        std::printf("PASS: %s\n", desc);
    } else {
        std::printf("FAIL: %s\n", desc);
        ++g_failures;
    }
}

// Write a minimal 16-bit PCM mono WAV with a 100ms 440Hz sine (~4410 samples
// @44.1kHz). Big enough to pass any "is this audio?" sanity check but tiny
// enough that ExtractMini + ExtractDetail finish in <50 ms.
static bool writeTinyWav(const fs::path& p) {
    constexpr uint32_t sr = 44100;
    constexpr uint32_t numSamples = 4410;
    constexpr uint16_t bits = 16;
    constexpr uint16_t channels = 1;

    std::vector<int16_t> samples(numSamples);
    const double twoPi = 6.283185307179586;
    for (uint32_t i = 0; i < numSamples; ++i) {
        double t = static_cast<double>(i) / sr;
        samples[i] = static_cast<int16_t>(0.5 * 32767.0 *
                                          std::sin(twoPi * 440.0 * t));
    }

    const uint32_t dataBytes = numSamples * sizeof(int16_t);
    const uint32_t fmtChunkSize = 16;
    const uint32_t riffSize = 4 + (8 + fmtChunkSize) + (8 + dataBytes);

    std::ofstream f(p, std::ios::binary);
    if (!f) return false;
    auto put32 = [&](uint32_t v) {
        char b[4] = {char(v & 0xff), char((v >> 8) & 0xff),
                     char((v >> 16) & 0xff), char((v >> 24) & 0xff)};
        f.write(b, 4);
    };
    auto put16 = [&](uint16_t v) {
        char b[2] = {char(v & 0xff), char((v >> 8) & 0xff)};
        f.write(b, 2);
    };

    f.write("RIFF", 4);
    put32(riffSize);
    f.write("WAVE", 4);

    f.write("fmt ", 4);
    put32(fmtChunkSize);
    put16(1);                              // PCM
    put16(channels);
    put32(sr);
    put32(sr * channels * (bits / 8));     // byte rate
    put16(channels * (bits / 8));          // block align
    put16(bits);

    f.write("data", 4);
    put32(dataBytes);
    f.write(reinterpret_cast<const char*>(samples.data()), dataBytes);
    return f.good();
}

static fs::path makeTempDir(const char* tag) {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path tempRoot = fs::temp_directory_path();
    for (int attempt = 0; attempt < 32; ++attempt) {
        const fs::path p = tempRoot /
            ("auro_t297_" + std::string(tag) + "_" + std::to_string(nonce) +
             "_" + std::to_string(attempt));
        std::error_code ec;
        if (fs::create_directory(p, ec)) return p;
    }
    throw std::runtime_error("could not create an isolated T-297 test directory");
}

static std::string readFirstExistingTextFile(const std::vector<std::string>& candidates) {
    if (candidates.empty()) return {};
    const fs::path repoRoot = fs::path(__FILE__).parent_path().parent_path();
    std::ifstream in(repoRoot / candidates.front(), std::ios::binary);
    if (!in) return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    std::string text = ss.str();
    size_t write = 0;
    for (size_t read = 0; read < text.size(); ++read) {
        if (text[read] != '\r') {
            text[write++] = text[read];
        }
    }
    text.resize(write);
    return text;
}

static bool ledgerEntryContains(const std::string& ledger,
                                const std::string& key,
                                const std::string& phrase) {
    const size_t start = ledger.find("\"id\": \"" + key + "\"");
    if (start == std::string::npos) return false;
    const size_t next = ledger.find("\"id\": \"", start + 1);
    return ledger.substr(start, next == std::string::npos ? next : next - start)
               .find(phrase) != std::string::npos;
}

static std::string extractFunctionBody(const std::string& source, const std::string& signature) {
    const size_t sig = source.find(signature);
    if (sig == std::string::npos) return {};
    const size_t open = source.find('{', sig);
    if (open == std::string::npos) return {};

    int depth = 0;
    for (size_t i = open; i < source.size(); ++i) {
        if (source[i] == '{') {
            ++depth;
        } else if (source[i] == '}') {
            --depth;
            if (depth == 0) {
                return source.substr(open, i - open + 1);
            }
        }
    }
    return {};
}

static size_t countOccurrences(const std::string& haystack, const std::string& needle) {
    size_t count = 0;
    size_t pos = 0;
    while ((pos = haystack.find(needle, pos)) != std::string::npos) {
        ++count;
        pos += needle.size();
    }
    return count;
}

static void waitDiagDrain() {
    std::this_thread::sleep_for(std::chrono::milliseconds(650));
}

static void checkScanDialogPrecomputeControlContract() {
    const std::string source = readFirstExistingTextFile({
        "src/main.cpp",
        "../src/main.cpp",
        "../../src/main.cpp",
    });
    check(!source.empty(), "main.cpp source is readable for scan dialog contract");
    const std::string ledger = readFirstExistingTextFile({"locale/harken.ui.strings.json"});
    check(!ledger.empty(), "localization ledger is readable for scan dialog contract");
    check(source.find("ImGui::Checkbox(L(Scan_PrecomputeCheckbox), &state.settings.cacheWaveformsOnScan)") == std::string::npos,
          "Scan dialog no longer lets scan-time UI change library waveform policy");
    check(source.find("getWaveformCacheMode()") != std::string::npos,
          "Scan dialog reads current library waveform cache policy");
    check(source.find("const bool precomputeWaveforms = state.settings.cacheWaveformsOnScan;") != std::string::npos,
          "BDD: Native scan thread now respects the scan strategy toggle for mini waveform precompute");
    check(source.find("const bool precomputeWaveforms = true;") == std::string::npos,
          "BDD: Native scan no longer hardcodes scan-time mini waveform precompute");
    check(source.find("skipWaveformBackfillOnScan") == std::string::npos,
          "Scan dialog no longer exposes inverted skip-cache semantics");
    check(source.find("cacheWaveformsOnScanSchema") != std::string::npos,
          "Saved scan cache settings migrate to the current positive checkbox semantics");

    check(source.find("ImGui::RadioButton(L(Main_FastScanMode), fastScanMode)") != std::string::npos &&
              ledgerEntryContains(ledger, "Main_FastScanMode", u8"极速模式"),
          "BDD: Scan dialog exposes the fast scan strategy radio button");
    check(source.find("ImGui::RadioButton(L(Main_FullScanMode), state.settings.cacheWaveformsOnScan)") != std::string::npos &&
              ledgerEntryContains(ledger, "Main_FullScanMode", u8"完整模式"),
          "BDD: Scan dialog exposes the full scan strategy radio button");
    check(source.find("L(Scan_FastModeHint)") != std::string::npos &&
              ledgerEntryContains(ledger, "Scan_FastModeHint", u8"mini 波形改为按需或后台渐进补齐"),
          "BDD: Scan dialog explains the fast scan strategy semantics");

    const std::string completeImport = "\xE5\xAE\x8C\xE6\x95\xB4\xE5\x85\xA5\xE5\xBA\x93";
    check(source.find(std::string("TextDisabled(\"%s\", state.settings.cacheWaveformsOnScan ? u8\"") + completeImport) == std::string::npos,
          "Scan dialog status does not call waveform backfill complete import");
    check(source.find("+ 波形补齐") == std::string::npos,
          "Scan dialog does not present scan as a slow waveform mode");
    check(source.find("ImGui::TextDisabled(u8\"扫描始终完整入库\");") == std::string::npos,
          "Scan dialog no longer shows inline complete-import explanatory copy");

    const size_t nativeForegroundReset =
        source.find("scanDb.resetScanForegroundCompletionSession(static_cast<int64_t>(scanGeneration))");
    const size_t nativeScanDirectory =
        source.find("scanDb.scanDirectoryEx(folder");
    check(nativeForegroundReset != std::string::npos &&
              nativeScanDirectory != std::string::npos &&
              nativeForegroundReset < nativeScanDirectory &&
              source.find("ScanCompletionSessionReset: session=%lld", nativeForegroundReset) != std::string::npos,
          "BDD: Native scan clears the current foreground-completion session before seeding scanDirectoryEx rows");

    check(countOccurrences(source, "WaitForScanWaveformBackfillCompletion(") >= 2,
          "BDD: Native scan performs a bounded warm drain for Standard mini waveform cache before deferring remaining debt");
    check(source.find("ScanForegroundWaveformDrain: scoped=%d pending=%d queued=%d scanTotal=%d scope=%s") != std::string::npos,
          "BDD: Scan thread reports requested mini waveform cache as foreground completion work");
    check(source.find("state.addPendingWaveform(res.filePath, true);") != std::string::npos,
          "BDD: Main visible-row cache miss remains retryable when decode promotion is already queued");
    check(source.find("if (res.errorCode == \"mini_cache_miss_no_decode\") {\n                    state.waveformMiniCacheMiss.insert(res.filePath);") == std::string::npos,
          "BDD: Main visible-row cache miss never poisons waveformMiniCacheMiss before retrying decode");
    const size_t nativeBackgroundLog =
        source.find("ScanForegroundWaveformDrain: scoped=%d pending=%d queued=%d scanTotal=%d scope=%s");
    const size_t nativeWaitCall =
        nativeBackgroundLog == std::string::npos
            ? std::string::npos
            : source.find("WaitForScanWaveformBackfillCompletion(", nativeBackgroundLog);
    check(nativeBackgroundLog != std::string::npos && nativeWaitCall != std::string::npos,
          "BDD: Native scan performs a bounded synchronous warm drain after logging waveform foreground work");
    check(source.find("WaveformCacheDrainBudgetExceeded: scoped=%d pending=%d cacheTotal=%d scanTotal=%d waitMs=%lld scope=%s") != std::string::npos,
          "BDD: Native scan logs when the mini waveform warm-drain budget expires before full completion");
    check(source.find("ScanComplete: total=%d added=%d updated=%d skipped=%d removed=%d errors=%d ingested=%d ingestMs=%lld cancelled=%d waveformCacheRequested=%d") != std::string::npos,
          "BDD: Native scan completion is emitted independently from waveform cache drain completion");
    check(source.find("state.database.consumePostProcessCoverRefresh()") != std::string::npos &&
              source.find("aura::ConsumeCoverBackfillUiRefresh()") != std::string::npos,
          "BDD: Main UI consumes deferred cover refresh signals before clearing ready-to-work cover debt");
    check(source.find("completionCounts.coverUnresolved > 0") != std::string::npos &&
              source.find("state.database.getScanForegroundCompletionCounts(") != std::string::npos,
          "BDD: Native foreground completion cover debt is driven by scanned-session ledger rows");
    check(source.find("HasOutstandingReadyToWorkCoverPostProcess(") != std::string::npos &&
              source.find("CountReadyToWorkCoverPendingRows(") != std::string::npos,
          "BDD: Main ready-to-work cover debt is derived from real cover backfill rows and post-process state");
    check(source.find("ReadyToWorkCoversSatisfied: db=%s") != std::string::npos,
          "BDD: Main UI emits a cover-ready signal when deferred cover work settles");
    check(source.find("ReadyToWorkMainWaveSatisfied: db=%s path=%s") != std::string::npos,
          "BDD: Main UI emits a main-wave-ready signal when the current work file becomes waveform-usable");
    check(source.find("ScanCompletionState: pending=%d waveform=%d covers=%d session=%lld total=%d miniUnresolved=%d coverUnresolved=%d metadataUnresolved=%d durationMs=%lld") != std::string::npos &&
              source.find("ScanCompletionSatisfied: session=%lld durationMs=%lld") != std::string::npos,
          "BDD: Native scan tracks a dedicated foreground completion gate for metadata, mini waveform, and cover debt");
    check(source.find("ScanCompletionWaveformsSatisfied: session=%lld total=%d") != std::string::npos &&
              source.find("ScanCompletionCoversSatisfied: session=%lld total=%d") != std::string::npos,
          "BDD: Native foreground completion emits dedicated waveform and cover completion signals");
    check(source.find("const bool waveformCompletionRequired = state.settings.cacheWaveformsOnScan;") != std::string::npos &&
              source.find("const int effectiveMiniUnresolved =\n        waveformCompletionRequired ? counts.miniUnresolved : 0;") != std::string::npos,
          "BDD: Native completion refresh respects the active scan strategy when recomputing Done-gate mini debt");
    check(source.find("const int effectiveMiniUnresolved =\n                            precomputeWaveforms ? completionCounts.miniUnresolved : 0;") != std::string::npos &&
              source.find("const bool waveformCompletionPending =\n                            !cancelled &&\n                            effectiveMiniUnresolved > 0;") != std::string::npos,
          "BDD: Native fast scan masks mini waveform completion debt out of the Done gate");
    check(source.find("state.scanCompletionWaveformsPending.store(\n                            waveformCompletionPending,") != std::string::npos &&
              source.find("state.scanCompletionCoversPending.store(\n                            coverCompletionPending,") != std::string::npos,
          "BDD: Native scan carries full-scope waveform and cover debt into the foreground completion gate");
    check(source.find("auto ensureScanForegroundBackfillWorkersStarted = [&](const char* reason) {") != std::string::npos &&
              source.find("const auto snapshot = state.backfillQueue.snapshot();") != std::string::npos &&
              source.find("const int queuedBackfillJobs =") != std::string::npos &&
              source.find("const bool preserveQueuedScanWork =") != std::string::npos &&
              source.find("state.backfillQueue.refillFromDatabaseIfBelowWaterMark(&scanDb, 4096, 4096);") != std::string::npos &&
              source.find("state.backfillQueue.rebuildFromDatabase(&scanDb, 4096);") != std::string::npos &&
              source.find("ScanForegroundBackfillQueueSeed: session=%lld preserve=%d queued=%d inFlight=%d") != std::string::npos &&
              source.find("ScanForegroundBackfillStart: session=%lld reason=%s strategy=%s") != std::string::npos &&
              source.find("ensureScanForegroundBackfillWorkersStarted(\"selected_strategy_full\");") != std::string::npos,
          "BDD: Native foreground completion preserves queued scan backfill work instead of wiping RecentScan jobs before waiting on strategy-owned debt");
    check(source.find("const int backgroundBackfillThreads =\n                        precomputeWaveforms ? backfillThreads : (std::min)(backfillThreads, 2);") != std::string::npos &&
              source.find("ScanBackgroundBackfillDeferred: session=%lld reason=ready_to_work strategy=fast ready=%d interactIdleMs=%lld") != std::string::npos &&
              source.find("ScanBackgroundBackfillDeferredSatisfied: session=%lld strategy=fast waitedMs=%lld") != std::string::npos &&
              source.find("ScanBackgroundBackfillResume: session=%lld strategy=%s threads=%d") != std::string::npos &&
              std::regex_search(source, std::regex(R"(&state[.]backfillQueue,[[:space:]]*backgroundBackfillThreads[)];)")),
          "BDD: Fast scan defers post-completion mini backfill briefly until ready-to-work pressure eases, then resumes on a lighter background worker count");
    check(source.find("const bool foregroundBackfillKickNeeded =") != std::string::npos &&
              source.find("state.scanCompletionPending.load(std::memory_order_acquire) &&") != std::string::npos &&
              source.find("ensureScanForegroundBackfillWorkersStarted(\"completion_wait_fast\");") != std::string::npos,
          "BDD: Native fast scan kicks cover and metadata backfill before entering the foreground completion wait");
    check(source.find("state.scanReadyToWorkMainWavePending = mainWaveReadyToWorkPending;") != std::string::npos,
          "BDD: Native scan carries current-file main waveform debt into the ready-to-work gate");
    check(source.find("HasOutstandingReadyToWorkMiniWavePresentation(") != std::string::npos &&
              source.find("HasPendingReadyToWorkSearchRefresh(") != std::string::npos,
          "BDD: Native ready-to-work waveform debt also tracks pending visible mini-wave presentation");
    check(source.find("ReadyToWorkCoverFirstScreenSatisfied: db=%s ready=%zu") != std::string::npos,
          "BDD: Main UI emits a first-screen-cover-ready signal before cover debt clears");
    check(source.find("aura::cover_wall::ReadyToWorkCandidateSatisfied(") != std::string::npos &&
              source.find("aura::cover_wall::ShouldRecordConfirmedFailure(item)") != std::string::npos,
          "BDD: Main UI treats first-screen covers as settled when they either have a texture or enter the controlled failure/cooldown path");
    check(source.find("HasPendingReadyToWorkCoverSourceRefresh(") != std::string::npos &&
              source.find("state.coverWallDisplayRefreshRequested ||") != std::string::npos &&
              source.find("state.coverWallDatabaseInvalidationRequested ||") != std::string::npos &&
              source.find("state.needsCoverWallReload ||") != std::string::npos,
          "BDD: Main first-screen cover debt remains blocked while the cover-wall source is still refreshing or reloading");
    check(source.find("coverFirstScreenReadyToWorkPending") != std::string::npos &&
              source.find("state.scanReadyToWorkCoverFirstScreenPending =") != std::string::npos &&
              source.find("coverFirstScreenReadyToWorkPending;") != std::string::npos,
          "BDD: Native scan keeps first-screen cover debt independent from deferred cover post-process rows");
    check(source.find("if (candidates.empty()) {") != std::string::npos &&
              source.find("HasPendingReadyToWorkCoverSourceRefresh(state)") != std::string::npos &&
              source.find("if (readyCount >= candidates.size()) {") != std::string::npos,
          "BDD: Main first-screen cover debt waits for source refresh only while current cover candidates are still unavailable");
    check(source.find("} else if (state.scanCompletionPending.load(std::memory_order_acquire) &&\n                   state.scanTotal.load() > 0) {") != std::string::npos &&
              source.find("} else if (state.scanSummaryToastUntil > ImGui::GetTime() && state.scanTotal.load() > 0) {") != std::string::npos,
          "BDD: Status bar only promotes to the completed import toast after foreground completion debt fully settles");
    check(source.find("if (scanCompleteSummaryActive && !state.settings.cacheWaveformsOnScan) {") != std::string::npos &&
              source.find("L(Main_IndexReadyNote)") != std::string::npos &&
              ledgerEntryContains(ledger, "Main_IndexReadyNote", u8"按需或后台渐进补齐"),
          "BDD: Fast scan completion summary explicitly tells the user mini waveforms continue through a non-blocking background/on-demand lane");
    check(source.find("if (!state.settings.cacheWaveformsOnScan) {") != std::string::npos &&
              source.find("ImGui::TextDisabled(L(Main_MiniWaveBackfillNote));") != std::string::npos &&
              ledgerEntryContains(ledger, "Main_MiniWaveBackfillNote", u8"波形后台渐进补齐"),
          "BDD: Fast scan completion toast still discloses background mini waveform fill after Done becomes available");
    check(source.find("state.scanCompletionPending.load(std::memory_order_acquire)") != std::string::npos &&
              source.find("ImGui::TextDisabled(\"%s\", BuildScanCompletionPendingLabel(state).c_str());") != std::string::npos,
          "BDD: Scan dialog summary reports foreground completion debt from the dedicated completion gate");
    check(source.find("static std::string BuildScanReadyToWorkPendingLabel(const AppState& state)") != std::string::npos &&
              source.find("RefreshScanReadyToWorkState(state);") != std::string::npos,
          "BDD: Ready-to-work debt remains a separate internal gate even after import-completion wording moves to the new full-import gate");
    check(source.find("changedRowCount >= 50") == std::string::npos,
          "BDD: Native ready-to-work cover debt no longer arms from a coarse changed-row threshold");
    check(source.find("state.database.isCoverPostProcessRunning()") != std::string::npos &&
              source.find("state.database.hasPendingPostProcessCoverRefresh()") != std::string::npos,
          "BDD: Native ready-to-work cover debt survives early refresh consumption by checking running and pending post-process state");
    check(source.find("completionCounts.coverUnresolved > 0") != std::string::npos &&
              source.find("state.scanCompletionCoverUnresolved.store(\n                            completionCounts.coverUnresolved,") != std::string::npos,
          "BDD: Native foreground completion cover gate follows scanned-session cover ledger debt only");
    check(source.find("const bool presentationPending =\n            HasOutstandingReadyToWorkMiniWavePresentation(") != std::string::npos &&
              source.find("if (pendingRows <= 0 && !presentationPending) {") != std::string::npos,
          "BDD: Native ready-to-work waveform debt does not clear until visible mini-wave presentation also settles");
    check(source.find("if (!state.scanCompletionPending.load(std::memory_order_acquire)) {\n                    state.scanProgress = 0;") != std::string::npos,
          "BDD: Closing the scan popup preserves the completed-scan summary until foreground debt actually settles");
    check(source.find("const bool scanCompletionBlocked =\n                state.scanCompletionPending.load(std::memory_order_acquire) &&") != std::string::npos &&
              source.find("ImGui::BeginDisabled(scanCompletionBlocked);\n            if (aura::ui::ActionButton(L(Done), ImVec2(100, 0))) {") != std::string::npos &&
              source.find("ImGui::EndDisabled();\n            if (scanCompletionBlocked) {") != std::string::npos,
          "BDD: Scan dialog keeps Done disabled until the foreground completion gate clears");
    check(source.find("ImGui::TextDisabled(L(Main_WaitForegroundImport));") != std::string::npos &&
              ledgerEntryContains(ledger, "Main_WaitForegroundImport", u8"等待本次入库前台完成"),
          "BDD: Scan dialog tells the user the import is still being finished in the foreground instead of allowing hidden background completion");
    check(source.find("const bool scanForegroundCompletionActive =\n            state.scanCompletionPending.load(std::memory_order_acquire) &&") != std::string::npos &&
              source.find("ImGui::BeginDisabled(state.scanCancelRequested.load());") != std::string::npos &&
              source.find("if (!scanForegroundCompletionActive) {\n                    state.requestCancelScanTask();") == std::string::npos,
          "BDD: Foreground completion keeps Cancel actionable instead of disabling rollback");
    const size_t doneButtonBlock = source.find("if (aura::ui::ActionButton(L(Done), ImVec2(100, 0))) {");
    const size_t doneButtonClose = doneButtonBlock == std::string::npos
        ? std::string::npos
        : source.find("ImGui::CloseCurrentPopup();", doneButtonBlock);
    const std::string doneButtonBody =
        (doneButtonBlock == std::string::npos || doneButtonClose == std::string::npos)
            ? std::string()
            : source.substr(doneButtonBlock, doneButtonClose - doneButtonBlock);
    check(!doneButtonBody.empty(),
          "BDD: Scan dialog Done button body is inspectable");
    check(doneButtonBody.find("state.needsDatabaseRefresh = true;") != std::string::npos,
          "BDD: Scan dialog Done leaves scan completion refresh queued for the main loop");
    check(doneButtonBody.find("state.dbSwitchSettleFramesRemaining =\n                        (std::max)(state.dbSwitchSettleFramesRemaining, 120);") != std::string::npos,
          "BDD: Scan dialog Done arms a short settle window before heavy cover/mini warmup resumes on the main UI");
    check(doneButtonBody.find("state.database.rebuildFolderIndex();") == std::string::npos &&
              doneButtonBody.find("StartDeferredSidebarBootstrap(state, true, false);") == std::string::npos &&
              doneButtonBody.find("RefreshSearchResults(state, true);") == std::string::npos &&
              doneButtonBody.find("state.invalidateCoverWallAfterDatabaseContentChange();") == std::string::npos &&
              doneButtonBody.find("state.needsDatabaseRefresh = false;") == std::string::npos,
          "BDD: Scan dialog Done does not run synchronous database or cover-wall refresh work on the click path");
    const std::string coverPrimingRuntime = readFirstExistingTextFile({
        "src/AsyncSearchCoverPrimingRuntimeAdapter.cpp",
        "../src/AsyncSearchCoverPrimingRuntimeAdapter.cpp",
        "../../src/AsyncSearchCoverPrimingRuntimeAdapter.cpp",
    });
    check(!coverPrimingRuntime.empty(),
          "AsyncSearchCoverPrimingRuntimeAdapter.cpp source is readable for deferred cover priming contract");
    check(coverPrimingRuntime.find("if (state.warmupFramesRemaining > 0 ||\n        state.dbSwitchSettleFramesRemaining > 0 ||\n        readyToWorkPending) {\n        return;") != std::string::npos &&
              coverPrimingRuntime.find("state.scanReadyToWorkPending") != std::string::npos,
          "BDD: Deferred cover priming pauses while the library is still in warmup or ready-to-work settle");
    check(source.find("const int backfillThreads =\n                        aura::ResolveScanBackfillWorkerThreads(\n                            workerThreads, std::thread::hardware_concurrency());") != std::string::npos,
          "BDD: Scan mini-cache backfill uses the unified worker-thread policy instead of a hardcoded pool");
    check(source.find("&state.backfillQueue,") != std::string::npos &&
              source.find("backfillThreads);") != std::string::npos,
          "BDD: Scan mini-cache backfill starts with the computed backfill worker count");
    check(source.find("state.scanTotal = std::max(scanTotal, 1);") == std::string::npos,
          "BDD: Native scan progress does not regress to showing full scanned file count as cache-drain total");

    const std::string windowsMain = readFirstExistingTextFile({
        "src/WindowsMain.cpp",
        "../src/WindowsMain.cpp",
        "../../src/WindowsMain.cpp",
    });
    check(!windowsMain.empty(), "WindowsMain.cpp source is readable for scan cache progress contract");
    const size_t runtimeForegroundReset =
        windowsMain.find("scanDb.resetScanForegroundCompletionSession(static_cast<int64_t>(scanGeneration))");
    const size_t runtimeScanDirectory =
        windowsMain.find("scanDb.scanDirectoryEx(folder");
    check(runtimeForegroundReset != std::string::npos &&
              runtimeScanDirectory != std::string::npos &&
              runtimeForegroundReset < runtimeScanDirectory &&
              windowsMain.find("ScanCompletionSessionReset: session=%lld", runtimeForegroundReset) != std::string::npos,
          "BDD: Runtime scan clears the current foreground-completion session before seeding scanDirectoryEx rows");
    check(windowsMain.find("bool precomputeWaveforms = cacheWaveforms;") != std::string::npos,
          "BDD: Runtime QA scan respects the cache_waveforms command parameter");
    check(windowsMain.find("const bool waveformCompletionRequired = state.settings.cacheWaveformsOnScan;") != std::string::npos &&
              windowsMain.find("const int effectiveMiniUnresolved =\n        waveformCompletionRequired ? counts.miniUnresolved : 0;") != std::string::npos,
          "BDD: Runtime completion refresh respects the active scan strategy when recomputing Done-gate mini debt");
    check(windowsMain.find("const std::string scanStrategy = getStr(raw, \"scan_strategy\");") != std::string::npos &&
              windowsMain.find("scanStrategy == \"fast\"") != std::string::npos &&
              windowsMain.find("scanStrategy == \"full\"") != std::string::npos &&
              windowsMain.find("scanStrategy == \"complete\"") != std::string::npos &&
              windowsMain.find("cacheWaveforms ? \"full\" : \"fast\"") != std::string::npos,
          "BDD: Runtime QA scan accepts an explicit fast/full scan strategy alias and reports it");
    check(countOccurrences(windowsMain, "WaitForScanWaveformBackfillCompletion(") >= 2,
          "BDD: Runtime scan path performs a bounded warm drain for requested mini waveform cache before deferring remaining debt");
    const size_t runtimeBackgroundLog =
        windowsMain.find("ScanForegroundWaveformDrain: scoped=%d");
    const size_t runtimeWaitCall =
        runtimeBackgroundLog == std::string::npos
            ? std::string::npos
            : windowsMain.find("WaitForScanWaveformBackfillCompletion(", runtimeBackgroundLog);
    check(runtimeBackgroundLog != std::string::npos && runtimeWaitCall != std::string::npos,
          "BDD: Runtime scan performs a bounded synchronous warm drain after logging waveform foreground work");
    check(windowsMain.find("WaveformCacheDrainBudgetExceeded: scoped=%d") != std::string::npos,
          "BDD: Runtime scan logs when the mini waveform warm-drain budget expires before full completion");
    check(windowsMain.find("ready_to_work_main_wave_pending") != std::string::npos,
          "BDD: Runtime QA status exposes current-file main waveform ready debt");
    check(windowsMain.find("ready_to_work_cover_first_screen_pending") != std::string::npos,
          "BDD: Runtime QA status exposes first-screen cover ready debt");
    check(windowsMain.find("scan_completion_pending") != std::string::npos &&
              windowsMain.find("scan_completion_waveforms_pending") != std::string::npos &&
              windowsMain.find("scan_completion_covers_pending") != std::string::npos &&
              windowsMain.find("state.settings.cacheWaveformsOnScan ? \"full\" : \"fast\"") != std::string::npos,
          "BDD: Runtime QA status exposes the dedicated full-import completion gate");
    check(windowsMain.find("Skipping AI engine init and module warmup for dedicated scan ingestion.") != std::string::npos,
          "BDD: Dedicated ingest mode skips non-scan startup warmup so QA scan lanes can enter the main loop promptly");
    check(windowsMain.find("state.scanReadyToWorkWaveformScope = waveformFolderScope;") != std::string::npos,
          "BDD: Runtime scan carries folder scope into ready-to-work waveform debt");
    check(windowsMain.find("HasOutstandingReadyToWorkMiniWavePresentationForQa(") != std::string::npos &&
              windowsMain.find("HasPendingReadyToWorkSearchRefreshForQa(") != std::string::npos,
          "BDD: Runtime ready-to-work waveform debt also tracks pending visible mini-wave presentation");
    check(windowsMain.find("coverFirstScreenReadyToWorkPending") != std::string::npos &&
              windowsMain.find("state.scanReadyToWorkCoverFirstScreenPending =") != std::string::npos &&
              windowsMain.find("coverFirstScreenReadyToWorkPending;") != std::string::npos,
          "BDD: Runtime scan carries first-screen cover debt into the ready-to-work gate independently from deferred post-process rows");
    check(windowsMain.find("HasOutstandingReadyToWorkCoverPostProcessForQa(") != std::string::npos &&
              windowsMain.find("CountReadyToWorkCoverPendingRowsForQa(") != std::string::npos,
          "BDD: Runtime ready-to-work cover debt is derived from real cover backfill rows and post-process state");
    check(windowsMain.find("completionCounts.coverUnresolved > 0") != std::string::npos &&
              windowsMain.find("getScanForegroundCompletionCounts(") != std::string::npos,
          "BDD: Runtime foreground completion cover debt is driven by scanned-session ledger rows");
    check(windowsMain.find("state.coverWall.preloadFirstPage.clear();") != std::string::npos,
          "BDD: Runtime scan clears first-screen preload candidates before rebuilding ready-to-work cover debt");
    check(windowsMain.find("state.scanReadyToWorkMainWavePending = mainWaveReadyToWorkPending;") != std::string::npos,
          "BDD: Runtime scan carries current-file main waveform debt into the ready-to-work gate");
    check(windowsMain.find("const int effectiveMiniUnresolved =\n                precomputeWaveforms ? completionCounts.miniUnresolved : 0;") != std::string::npos &&
              windowsMain.find("const bool waveformCompletionPending =\n                !cancelled &&\n                effectiveMiniUnresolved > 0;") != std::string::npos,
          "BDD: Runtime fast scan masks mini waveform completion debt out of the Done gate");
    check(windowsMain.find("state.scanCompletionWaveformsPending.store(\n                waveformCompletionPending,") != std::string::npos &&
              windowsMain.find("state.scanCompletionCoversPending.store(\n                coverCompletionPending,") != std::string::npos,
          "BDD: Runtime scan carries full-scope waveform and cover debt into the foreground completion gate");
    check(windowsMain.find("auto ensureScanForegroundBackfillWorkersStarted = [&](const char* reason) {") != std::string::npos &&
              windowsMain.find("const auto snapshot = state.backfillQueue.snapshot();") != std::string::npos &&
              windowsMain.find("const int queuedBackfillJobs =") != std::string::npos &&
              windowsMain.find("const bool preserveQueuedScanWork =") != std::string::npos &&
              windowsMain.find("state.backfillQueue.refillFromDatabaseIfBelowWaterMark(&scanDb, 4096, 4096);") != std::string::npos &&
              windowsMain.find("state.backfillQueue.rebuildFromDatabase(&scanDb, 4096);") != std::string::npos &&
              windowsMain.find("ScanForegroundBackfillQueueSeed: session=%lld preserve=%d queued=%d inFlight=%d") != std::string::npos &&
              windowsMain.find("ScanForegroundBackfillStart: session=%lld reason=%s strategy=%s") != std::string::npos &&
              windowsMain.find("ensureScanForegroundBackfillWorkersStarted(\"selected_strategy_full\");") != std::string::npos,
          "BDD: Runtime foreground completion preserves queued scan backfill work instead of wiping RecentScan jobs before waiting on strategy-owned debt");
    check(windowsMain.find("const int backgroundBackfillThreads =\n            precomputeWaveforms ? backfillThreads : (std::min)(backfillThreads, 2);") != std::string::npos &&
              windowsMain.find("ScanBackgroundBackfillDeferred: session=%lld reason=ready_to_work strategy=fast ready=%d interactIdleMs=%lld") != std::string::npos &&
              windowsMain.find("ScanBackgroundBackfillDeferredSatisfied: session=%lld strategy=fast waitedMs=%lld") != std::string::npos &&
              windowsMain.find("ScanBackgroundBackfillResume: session=%lld strategy=%s threads=%d") != std::string::npos &&
              windowsMain.find("&state.backfillQueue,\n                backgroundBackfillThreads);") != std::string::npos,
          "BDD: Runtime fast scan defers post-completion mini backfill briefly until ready-to-work pressure eases, then resumes on a lighter background worker count");
    check(windowsMain.find("const bool foregroundBackfillKickNeeded =") != std::string::npos &&
              windowsMain.find("state.scanCompletionPending.load(std::memory_order_acquire) &&") != std::string::npos &&
              windowsMain.find("ensureScanForegroundBackfillWorkersStarted(\"completion_wait_fast\");") != std::string::npos,
          "BDD: Runtime fast scan kicks cover and metadata backfill before entering the foreground completion wait");
    check(windowsMain.find("ReadyToWorkCoverFirstScreenSatisfied: db=%s ready=%zu") != std::string::npos,
          "BDD: Runtime UI emits a first-screen-cover-ready signal before cover debt clears");
    check(windowsMain.find("CoverWallReadyToWorkCandidateSatisfiedForQa(") != std::string::npos &&
              windowsMain.find("CoverWallShouldRecordConfirmedFailureForQa(item)") != std::string::npos,
          "BDD: Runtime UI treats first-screen covers as settled when they either have a texture or enter the controlled failure/cooldown path");
    check(windowsMain.find("HasPendingReadyToWorkCoverSourceRefreshForQa(") != std::string::npos &&
              windowsMain.find("state.coverWallDisplayRefreshRequested ||") != std::string::npos &&
              windowsMain.find("state.coverWallDatabaseInvalidationRequested ||") != std::string::npos &&
              windowsMain.find("state.needsCoverWallReload ||") != std::string::npos,
          "BDD: Runtime first-screen cover debt remains blocked while the cover-wall source is still refreshing or reloading");
    check(windowsMain.find("ReadyToWorkMainWaveSatisfied: db=%s path=%s") != std::string::npos,
          "BDD: Runtime UI emits a main-wave-ready signal when the current work file becomes waveform-usable");
    check(windowsMain.find("MarkScanReadyToWorkCoversSatisfiedForQa(state);") != std::string::npos &&
              windowsMain.find("RefreshScanReadyToWorkStateForQa(state);") != std::string::npos,
          "BDD: Runtime UI keeps ready-to-work debt alive until deferred cover and waveform work actually settles");
    check(windowsMain.find("snapshot.coversPending =\n        snapshot.coverPostPending || snapshot.coverFirstScreenPending;") != std::string::npos &&
              windowsMain.find("snapshot.pending =\n        snapshot.waveformsPending ||\n        snapshot.coversPending ||\n        snapshot.mainWavePending;") != std::string::npos,
          "BDD: Runtime ready-to-work QA snapshot recomputes pending from the live bucket flags instead of reusing stale aggregate state");
    check(windowsMain.find("RefreshScanCompletionStateForQa(state);") != std::string::npos &&
              windowsMain.find("ScanCompletionSatisfied: session=%lld durationMs=%lld") != std::string::npos,
          "BDD: Runtime QA loop separately tracks foreground completion settlement");
    check(windowsMain.find("if (candidates.empty()) {") != std::string::npos &&
              windowsMain.find("HasPendingReadyToWorkCoverSourceRefreshForQa(state)") != std::string::npos &&
              windowsMain.find("if (readyCount >= candidates.size()) {") != std::string::npos,
          "BDD: Runtime first-screen cover debt waits for source refresh only while current cover candidates are still unavailable");
    check(windowsMain.find("changedRowCount >= 50") == std::string::npos,
          "BDD: Runtime ready-to-work cover debt no longer arms from a coarse changed-row threshold");
    check(windowsMain.find("state.database.isCoverPostProcessRunning()") != std::string::npos &&
              windowsMain.find("state.database.hasPendingPostProcessCoverRefresh()") != std::string::npos,
          "BDD: Runtime ready-to-work cover debt survives early refresh consumption by checking running and pending post-process state");
    check(windowsMain.find("completionCounts.coverUnresolved > 0") != std::string::npos &&
              windowsMain.find("state.scanCompletionCoverUnresolved.store(\n                completionCounts.coverUnresolved,") != std::string::npos,
          "BDD: Runtime foreground completion cover gate follows scanned-session cover ledger debt only");
    check(windowsMain.find("const bool presentationPending =\n            HasOutstandingReadyToWorkMiniWavePresentationForQa(") != std::string::npos &&
              windowsMain.find("if (pendingRows <= 0 && !presentationPending) {") != std::string::npos,
          "BDD: Runtime ready-to-work waveform debt does not clear until visible mini-wave presentation also settles");
    check(windowsMain.find("state.dbSwitchSettleFramesRemaining =\n                    (std::max)(state.dbSwitchSettleFramesRemaining, 120);") != std::string::npos,
          "BDD: Runtime scan completion arms a short settle window before first-screen background cache work resumes");
    check(windowsMain.find("const int backfillThreads =\n            aura::ResolveScanBackfillWorkerThreads(\n                workerThreads, std::thread::hardware_concurrency());") != std::string::npos,
          "BDD: Runtime scan mini-cache backfill uses the unified worker-thread policy instead of a hardcoded pool");
    check(windowsMain.find("&state.backfillQueue,") != std::string::npos &&
              windowsMain.find("backfillThreads);") != std::string::npos,
          "BDD: Runtime scan mini-cache backfill starts with the computed backfill worker count");
    check(windowsMain.find("ScanForegroundWaveformDrain: scoped=%d") != std::string::npos,
          "BDD: Runtime scan path reports requested mini waveform cache as foreground completion work");
    check(windowsMain.find("VisibleCacheMissPromotedToDecode: path=%s") != std::string::npos,
          "BDD: Runtime visible-row cache miss is promoted to source decode instead of permanent blank mini waveform");
    check(windowsMain.find("state.postSearchMiniWaveThrottleFrames > 0 && !scanForegroundWaveformDrainActive") != std::string::npos &&
              windowsMain.find("submitBudget = (std::min)(submitBudget, 1);") != std::string::npos,
          "BDD: Runtime search-result mini-wave fill is briefly throttled so text results can paint before decode catch-up");
    check(windowsMain.find("visibleDecodePromotionsThisFrame < visibleDecodePromotionBudget") != std::string::npos &&
              windowsMain.find("++visibleDecodePromotionsThisFrame;") != std::string::npos,
          "BDD: Runtime visible-row cache-miss decode promotion is per-frame bounded after search refresh");
    check(windowsMain.find("result.errorCode == \"mini_cache_miss_no_decode\"") != std::string::npos &&
              windowsMain.find("state.waveformMiniCacheMiss.erase(result.filePath);") != std::string::npos,
          "BDD: Runtime visible-row cache miss does not poison waveformMiniCacheMiss before retrying decode");
    check(windowsMain.find("state.addPendingWaveform(result.filePath, true);") != std::string::npos,
          "BDD: Runtime visible-row cache miss remains retryable when decode promotion is already queued");
    check(windowsMain.find("state.waveformMiniCacheMiss.insert(result.filePath);") == std::string::npos,
          "BDD: Runtime visible-row cache miss never writes a permanent miss throttle");

    const std::string databaseSource = readFirstExistingTextFile({
        "src/Database.cpp",
        "../src/Database.cpp",
        "../../src/Database.cpp",
    });
    check(!databaseSource.empty(), "Database.cpp source is readable for scan backfill priority contract");
    check(databaseSource.find("aura::BackfillJobKind::CoverResolveAndThumb,\n                            currentScanCompletionBackfillPriority(),") != std::string::npos &&
              databaseSource.find("aura::BackfillJobKind::MetadataRefresh,\n                            currentScanCompletionBackfillPriority(),") != std::string::npos &&
              databaseSource.find("batchForegroundCoverIds.push_back(fileId);") != std::string::npos,
          "BDD: Scan-created cover and metadata backfill use foreground priority while the scan-session completion gate is pending");

    const std::string backfillQueueSource = readFirstExistingTextFile({
        "src/BackfillQueue.cpp",
        "../src/BackfillQueue.cpp",
        "../../src/BackfillQueue.cpp",
    });
    check(!backfillQueueSource.empty(), "BackfillQueue.cpp source is readable for durable priority contract");
    check(backfillQueueSource.find("static_cast<BackfillJobKind>(kind) == BackfillJobKind::WaveformL1Overview\n        ? BackfillPriority::RecentScan\n        : BackfillPriority::IdleBackfill;") != std::string::npos,
          "BDD: Durable waveform pending rows are refilled at RecentScan priority while cover and metadata remain idle-ledger work");

    const std::string localization = readFirstExistingTextFile({
        "src/Localization.cpp",
        "../src/Localization.cpp",
        "../../src/Localization.cpp",
    });
    check(!localization.empty(), "Localization.cpp source is readable for scan dialog contract");
    check(ledgerEntryContains(ledger, "Scan_PrecomputeCheckbox", u8"缓存波形"),
          "Scan checkbox copy says checked means waveform cache");
    check(!ledgerEntryContains(ledger, "Scan_PrecomputeCheckbox", u8"不缓存波形"),
          "Scan checkbox copy no longer says no waveform cache");
}

static void checkLibraryWaveformCacheModeContracts() {
    check(WaveformCachePolicy::ShouldPersistGeneratedMainDetail(
              WaveformCachePolicy::Mode::MiniOnly,
              1.0,
              (size_t)WaveformCachePolicy::kShortMainDetailPeaks,
              1) == false,
          "BDD: Given a Standard library, when main detail is generated, then it is not persisted");
    check(WaveformCachePolicy::ShouldPersistGeneratedMainDetail(
              WaveformCachePolicy::Mode::MiniAndMain,
              1.0,
              (size_t)WaveformCachePolicy::kShortMainDetailPeaks,
              1) == true,
          "BDD: Given a High-speed library, when main detail is generated, then current detail persistence is preserved");

    const std::string audioEngine = readFirstExistingTextFile({
        "src/AudioEngine.cpp",
        "../src/AudioEngine.cpp",
        "../../src/AudioEngine.cpp",
    });
    check(!audioEngine.empty(), "AudioEngine.cpp source is readable for main detail cache contract");
    check(audioEngine.find("MainDetailCacheBypassPolicyMiniOnly") != std::string::npos,
          "BDD: Given a Standard library has legacy detail blobs, when main waveform loads, then persisted detail cache is bypassed");
    check(audioEngine.find("getWaveformCacheMode() != WaveformCachePolicy::Mode::MiniOnly") != std::string::npos,
          "AudioEngine gates main detail cache reads on the library waveform policy");
}

static void checkWatcherRescanUsesLightweightImportContract() {
    const std::string source = readFirstExistingTextFile({
        "src/Database.cpp",
        "../src/Database.cpp",
        "../../src/Database.cpp",
    });
    check(!source.empty(), "Database.cpp source is readable for watcher rescan contract");

    const std::string body =
        extractFunctionBody(source, "void Database::watcherRescanWorkerThread()");
    check(!body.empty(), "watcherRescanWorkerThread body can be inspected");
    check(body.find("/*completeImport=*/false") != std::string::npos,
          "Watcher rescan does not perform legacy complete import work inline");
    check(body.find("/*completeImport=*/true") == std::string::npos,
          "Watcher rescan cannot reintroduce synchronous complete import");
}

static void checkWaveformWorkerCancelPreservesBackfillContract() {
    const std::string source = readFirstExistingTextFile({
        "src/AsyncWorker.cpp",
        "../src/AsyncWorker.cpp",
        "../../src/AsyncWorker.cpp",
    });
    check(!source.empty(), "AsyncWorker.cpp source is readable for backfill cancel contract");

    const std::string body =
        extractFunctionBody(source, "void WaveformWorker::cancelAllPending()");
    check(!body.empty(), "WaveformWorker::cancelAllPending body can be inspected");
    check(body.find("hasBackfillJob") != std::string::npos,
          "cancelAllPending distinguishes durable backfill jobs from UI mini-wave requests");
    check(body.find("m_requestQueue.clear()") == std::string::npos,
          "cancelAllPending does not blindly drop queued backfill jobs");
    check(body.find("preservedResults") != std::string::npos,
          "cancelAllPending preserves completed backfill results until the UI reports outcomes");
    check(body.find("req.hasBackfillJob") != std::string::npos,
          "worker generation checks allow in-flight backfill jobs to report outcomes after UI cancellation");
}

static void checkScanBackfillStaysMiniOnlyContract() {
    const std::string source = readFirstExistingTextFile({
        "src/BackfillWorkerPool.cpp",
        "../src/BackfillWorkerPool.cpp",
        "../../src/BackfillWorkerPool.cpp",
    });
    check(!source.empty(), "BackfillWorkerPool.cpp source is readable for scan backfill contract");

    const std::string body =
        extractFunctionBody(source, "bool processWaveformJob(Database& db,");
    check(!body.empty(), "processWaveformJob body can be inspected");
    check(body.find("ExtractMini(") != std::string::npos,
          "scan waveform backfill still builds the list mini waveform cache");
    check(body.find("ExtractDetail(") == std::string::npos,
          "scan waveform backfill does not decode main waveform detail");
    check(body.find("saveWaveformDetailByPath(") == std::string::npos,
          "scan waveform backfill does not persist main waveform detail");
    const size_t cancelGuardPos = body.find("miniExtract.errorCode == \"cancelled\"");
    const size_t failedStatePos = body.find("upsertWaveformCacheStateByPath(rec.filePath, \"failed\"");
    check(cancelGuardPos != std::string::npos,
          "BDD: Scan waveform backfill treats cancellation as retryable instead of failed cache");
    check(cancelGuardPos != std::string::npos &&
              failedStatePos != std::string::npos &&
              cancelGuardPos < failedStatePos,
          "BDD: Cancelled waveform jobs keep durable pending debt for the next run");
    check(source.find("std::clamp(threadCount, 1, 2)") == std::string::npos,
          "BDD: Backfill worker pool does not silently clamp scan mini-cache workers back to two");
    check(source.find("std::clamp(threadCount, 1, 16)") != std::string::npos,
          "BDD: Backfill worker pool allows enough workers for scan mini-cache throughput");

    const std::string waveformExtractor = readFirstExistingTextFile({
        "src/WaveformExtractor.cpp",
        "../src/WaveformExtractor.cpp",
        "../../src/WaveformExtractor.cpp",
    });
    check(!waveformExtractor.empty(), "WaveformExtractor.cpp source is readable for mini waveform decode contract");
    const std::string extractMiniBody =
        extractFunctionBody(waveformExtractor, "bool WaveformExtractor::ExtractMini(const std::string& filePath,");
    check(!extractMiniBody.empty(), "WaveformExtractor::ExtractMini body is inspectable");
    const std::string extractMiniImplementationBody =
        extractFunctionBody(waveformExtractor, "bool WaveformExtractor::ExtractMiniReturningDecoded(");
    const std::string& miniDecodeBody =
        extractMiniImplementationBody.empty() ? extractMiniBody : extractMiniImplementationBody;
    check(!miniDecodeBody.empty(), "WaveformExtractor mini decode implementation body is inspectable");
    const size_t sparseWavPos = miniDecodeBody.find("ExtractMiniSparseWav(filePath, targetPeaks, out, perChannel, cancelFlag)");
    const size_t decoderProbePos = miniDecodeBody.find("NativeFloatReader probeReader");
    const size_t seekFallbackPos = miniDecodeBody.find("ExtractMiniSeekBased(filePath, targetPeaks, out, perChannel, cancelFlag)");
    check(sparseWavPos != std::string::npos,
          "BDD: PCM WAV mini cache uses the direct sparse WAV reader instead of miniaudio seek decode");
    check(sparseWavPos != std::string::npos &&
              decoderProbePos != std::string::npos &&
              seekFallbackPos != std::string::npos &&
              sparseWavPos < decoderProbePos &&
              sparseWavPos < seekFallbackPos,
          "BDD: PCM WAV sparse mini path runs before decoder probing and seek fallback");
    check(waveformExtractor.find("!HasWavExtension(filePath)") != std::string::npos,
          "BDD: WAV mini cache uses seek-based full-file sampling instead of full sequential decode");
}

static void checkFastScanReuseDoesNotRequireMainDetailContract() {
    const std::string source = readFirstExistingTextFile({
        "src/Database.cpp",
        "../src/Database.cpp",
        "../../src/Database.cpp",
    });
    check(!source.empty(), "Database.cpp source is readable for fast-scan waveform reuse contract");
    check(countOccurrences(source, "const bool detailRequired = completeImport && detailPolicy.persistMainDetail;") >= 2,
          "fast scan waveform reuse does not require persistent main detail for queued mini backfill");
}

static void checkIncrementalWaveformMenuFillsMiniOnlyContract() {
    const std::string source = readFirstExistingTextFile({
        "src/main.cpp",
        "../src/main.cpp",
        "../../src/main.cpp",
    });
    check(!source.empty(), "main.cpp source is readable for incremental waveform menu contract");

    const std::string startBody =
        extractFunctionBody(source, "static void StartIncrementalWaveformRebuild(AppState& state,");
    check(!startBody.empty(), "StartIncrementalWaveformRebuild body can be inspected");
    check(startBody.find("countFilesNeedingWaveformCache()") != std::string::npos,
          "BDD: Given fast scan has filled mini cache, when user clicks fill missing waveforms, then only missing mini cache is counted");
    check(startBody.find("countFilesNeedingAnyWaveformCache()") == std::string::npos,
          "BDD: Fill missing waveforms does not treat missing main detail as mini-cache debt");
    check(startBody.find("g_waveformRebuildRuntimeState.setIncrementalMiniOnly(true)") != std::string::npos,
          "BDD: Fill missing waveforms runs the rebuild queue in mini-only mode");
    check(startBody.find("state.waveformRebuildMiniOnlyMode = true") != std::string::npos,
          "BDD: Fill missing waveforms exposes mini-only mode to QA state");
    check(startBody.find("Waveform mini-only rebuild started") != std::string::npos,
          "BDD: Fill missing waveforms diagnostics use mini-only wording instead of generic incremental wording");

    const std::string pumpBody =
        extractFunctionBody(source, "static void PumpWaveformRebuild(AppState& state)");
    check(!pumpBody.empty(), "PumpWaveformRebuild body can be inspected");
    const std::string batchLifecycle = readFirstExistingTextFile({
        "src/WaveformRebuildBatchLifecycle.cpp",
        "../src/WaveformRebuildBatchLifecycle.cpp",
        "../../src/WaveformRebuildBatchLifecycle.cpp",
    });
    check(pumpBody.find("LoadWaveformRebuildBatchLifecycle(") != std::string::npos &&
              batchLifecycle.find("SelectWaveformRebuildBatch(") != std::string::npos,
          "BDD: Incremental waveform rebuild delegates database batch selection");
    check(pumpBody.find("g_waveformRebuildRuntimeState.incrementalMiniOnly()") != std::string::npos,
          "BDD: Incremental mini-only rebuild fetches mini-missing rows before any detail work");
    check(pumpBody.find("state.database.getFilesNeedingWaveformCache(") == std::string::npos &&
              pumpBody.find("state.database.getFilesNeedingDetailCache(") == std::string::npos,
          "BDD: Pump no longer owns detail-first and mini-fallback database queries");
    const std::string failurePersistence = readFirstExistingTextFile({
        "src/WaveformRebuildFailurePersistence.cpp",
        "../src/WaveformRebuildFailurePersistence.cpp",
        "../../src/WaveformRebuildFailurePersistence.cpp",
    });
    check(!failurePersistence.empty(),
          "WaveformRebuildFailurePersistence.cpp source is readable");
    const std::string recordWorkflow = readFirstExistingTextFile({
        "src/WaveformRebuildRecordWorkflow.cpp",
        "../src/WaveformRebuildRecordWorkflow.cpp",
        "../../src/WaveformRebuildRecordWorkflow.cpp",
    });
    check(!recordWorkflow.empty(),
          "WaveformRebuildRecordWorkflow.cpp source is readable");
    check(pumpBody.find("ProcessNextWaveformRebuildRecord(") != std::string::npos &&
              recordWorkflow.find("PersistPreviousWaveformRebuildFailure(") != std::string::npos &&
              failurePersistence.find("foreground_mini_rebuild_previous_failure") != std::string::npos,
          "BDD: Fill missing waveforms persists already-failed mini rows so the rebuild cursor can auto-complete");
    check(pumpBody.find("upsertWaveformCacheStateByPath(") == std::string::npos &&
              failurePersistence.find("database_.upsertWaveformCacheStateByPath(") != std::string::npos,
          "BDD: Fill missing waveforms converts in-memory failed rows into durable failed cache state instead of requeueing forever");

    const std::string windowsMain = readFirstExistingTextFile({
        "src/WindowsMain.cpp",
        "../src/WindowsMain.cpp",
        "../../src/WindowsMain.cpp",
    });
    check(!windowsMain.empty(), "WindowsMain.cpp source is readable for waveform QA command contract");
    check(windowsMain.find("\\\"waveform_rebuild_mini_only\\\"") != std::string::npos,
          "BDD: QA get_state exposes whether the active waveform rebuild is mini-only");
    check(windowsMain.find("(fullRebuild ? \"full\" : \"mini_only\")") != std::string::npos,
          "BDD: QA start_waveform_rebuild reports mini_only mode for the fill-missing command");
}

static void checkManualWaveformRebuildClickPathContract() {
    const std::string source = readFirstExistingTextFile({
        "src/main.cpp",
        "../src/main.cpp",
        "../../src/main.cpp",
    });
    check(!source.empty(), "main.cpp source is readable for manual waveform rebuild click-path contract");
    const std::string selectedWorkflow = readFirstExistingTextFile({
        "src/SelectedWaveformRebuildWorkflow.cpp",
        "../src/SelectedWaveformRebuildWorkflow.cpp",
        "../../src/SelectedWaveformRebuildWorkflow.cpp",
    });
    check(!selectedWorkflow.empty(),
          "SelectedWaveformRebuildWorkflow.cpp source is readable for frame-budget contract");

    const std::string currentBody =
        extractFunctionBody(source, "static void RebuildCurrentFileMainWaveform(AppState& state)");
    check(!currentBody.empty(), "RebuildCurrentFileMainWaveform body can be inspected");
    check(currentBody.find("state.audioEngine.stop()") == std::string::npos,
          "BDD: menu-bar current-file waveform rebuild must not stop playback on the UI click path");
    check(currentBody.find("g_selectedWaveformRebuildWorkflow.queueCurrentFile(") != std::string::npos &&
              selectedWorkflow.find("SubmitWaveformRebuildRequest(") != std::string::npos &&
              source.find("g_selectedWaveformRebuildWorkflow.pump(") != std::string::npos,
          "BDD: current-file waveform rebuild queues policy-aware worker work asynchronously");
    check(selectedWorkflow.find("effects.forceLoadedItem(item)") != std::string::npos &&
              source.find("state_.audioEngine.forceRegenerateWaveform(") != std::string::npos,
          "BDD: current-file waveform rebuild refreshes the visible main waveform asynchronously");

    const size_t menuPos = source.find("if (ImGui::MenuItem(L(Misc_RebuildCurrentWave)))");
    check(menuPos != std::string::npos, "menu-bar current waveform rebuild entry can be inspected");
    const size_t rebuildCallPos = source.find("RebuildCurrentFileMainWaveform(state);", menuPos);
    check(rebuildCallPos != std::string::npos,
          "BDD: menu-bar current waveform rebuild calls the dedicated current-file rebuild helper");
    const std::string menuBlock =
        menuPos != std::string::npos && rebuildCallPos != std::string::npos
            ? source.substr(menuPos, rebuildCallPos - menuPos)
            : std::string();
    check(menuBlock.find("state.audioEngine.stop()") == std::string::npos,
          "BDD: menu-bar current waveform rebuild click path does not synchronously stop AudioEngine");

    const std::string selectedPump =
        extractFunctionBody(source, "static void PumpSelectedWaveformRebuild(AppState& state)");
    check(selectedWorkflow.find("constexpr std::size_t kPerFrame = 4;") != std::string::npos &&
              selectedWorkflow.find("queue_.pump(kPerFrame") != std::string::npos &&
              selectedPump.find("g_selectedWaveformRebuildWorkflow.pump(") != std::string::npos,
          "BDD: row-context selected waveform rebuild clears/queues a small bounded batch per frame");
}


static void checkFullWaveformRebuildDoesNotSynchronouslyClearWholeLibraryContract() {
    const std::string source = readFirstExistingTextFile({
        "src/main.cpp",
        "../src/main.cpp",
        "../../src/main.cpp",
    });
    const std::string functionBody = extractFunctionBody(source, "static bool StartFullWaveformRebuild");
    check(functionBody.find("state.database.clearWaveformCache()") == std::string::npos,
        "StartFullWaveformRebuild must not synchronously clear/compact the whole waveform cache on the UI click path");
    check(functionBody.find("countAllFilesForWaveformRebuild()") != std::string::npos,
        "StartFullWaveformRebuild should still enumerate the full rebuild cursor");
    check(functionBody.find("state.waveformFailed.clear();") != std::string::npos,
        "StartFullWaveformRebuild should clear previous per-file failure sentinels so full rebuild can retry them");
}

static void checkIdleAssetBackfillContract() {
    const std::string appState = readFirstExistingTextFile({
        "src/AppState.h",
        "../src/AppState.h",
        "../../src/AppState.h",
    });
    check(!appState.empty(), "AppState.h source is readable for idle asset backfill contract");
    check(appState.find("bool idleAssetBackfillEnabled = true;") != std::string::npos,
          "BDD: Idle current-library asset backfill is enabled by default");
    check(appState.find("idleAssetBackfillNextCheckAt") != std::string::npos &&
              appState.find("idleAssetBackfillNextProgressPollAt") != std::string::npos,
          "BDD: Idle current-library asset backfill owns steady-clock check and progress deadlines");
    check(appState.find("idleAssetBackfillWaitingToast") == std::string::npos &&
              appState.find("idleAssetBackfillWaitingToastVisible") == std::string::npos,
          "BDD: Idle asset backfill no longer stores a persistent waiting overlay");
    check(appState.find("idleAssetBackfillStartedMiniTotal") != std::string::npos,
          "BDD: Idle asset backfill tracks mini totals for persistent progress feedback");

    const std::string source = readFirstExistingTextFile({
        "src/main.cpp",
        "../src/main.cpp",
        "../../src/main.cpp",
    });
    check(!source.empty(), "main.cpp source is readable for idle asset backfill contract");
    check(source.find("idleAssetBackfillEnabled") != std::string::npos &&
              source.find("idleAssetBackfillDelayMinutes") != std::string::npos &&
              source.find("aura::ui::SettingsSliderRow(") != std::string::npos &&
              source.find("L(Main_IdleBackfillCheck)") != std::string::npos &&
              source.find("&idleAssetBackfillDelayMinutes") != std::string::npos,
          "BDD: Idle asset backfill keeps the existing configurable idle threshold and settings control");
    check(source.find("kIdleAssetBackfillCheckInterval = std::chrono::seconds(60)") != std::string::npos &&
              source.find("kIdleAssetBackfillProgressPollInterval = std::chrono::seconds(2)") != std::string::npos &&
              source.find("IsIdleAssetBackfillCheckDue(") != std::string::npos &&
              source.find("HasIdleAssetBackfillDelayElapsed(") != std::string::npos,
          "BDD: Idle asset eligibility is checked once per minute without bypassing the configured threshold");
    check(source.find("DetectHarkenSearchUserInteractionForIdleAssetBackfill") != std::string::npos,
          "BDD: Idle asset backfill uses a dedicated user-interaction detector");
    check(source.find("io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f") != std::string::npos &&
              source.find("io.MouseWheel != 0.0f || io.MouseWheelH != 0.0f") != std::string::npos &&
              source.find("io.WantCaptureKeyboard") != std::string::npos,
          "BDD: Mouse move, wheel, click/drag, and keyboard activity reset the idle asset timer");
    check(source.find("PumpIdleAssetBackfill(state, DetectHarkenSearchUserInteractionForIdleAssetBackfill())") != std::string::npos,
          "BDD: Idle asset backfill pump runs before the generic waveform rebuild pump");
    check(source.find("StartIdleAssetBackfill(state)") != std::string::npos,
          "BDD: Idle asset backfill starts through its own current-library mini/cover path");
    const std::string idleStart = extractFunctionBody(source, "static bool StartIdleAssetBackfill(AppState& state)");
    check(idleStart.find("idleAssetStatistics->Request()") != std::string::npos &&
          idleStart.find("countFilesNeedingWaveformCache()") == std::string::npos,
          "BDD: Idle checks submit background work without a UI-thread waveform census");
    check(source.find("StartIncrementalWaveformRebuild(state, miniPending)") != std::string::npos &&
          source.find("? knownMiniCount : state.database.countFilesNeedingWaveformCache()") != std::string::npos,
          "BDD: Idle rebuild reuses its already-computed mini count");
    check(source.find("BackfillJobKind::CoverResolveAndThumb") != std::string::npos &&
              source.find("GlobalBackfillWorkerPool().start") != std::string::npos,
          "BDD: Idle asset backfill includes current-library pending cover aggregation work");
    check(source.find("state.audioEngine.stop()") == std::string::npos ||
              extractFunctionBody(source, "static bool StartIdleAssetBackfill(AppState& state)").find("state.audioEngine.stop()") == std::string::npos,
          "BDD: Idle asset backfill does not stop playback when it starts");
    const std::string stopIdle = extractFunctionBody(
        source, "static void StopIdleAssetBackfill(AppState& state, bool completed, bool interrupted)");
    check(stopIdle.find("IdleAssetBackfillStop: completed=%d interrupted=%d mini=%lld coverMetadata=%lld db=%s") != std::string::npos &&
              stopIdle.find("state.idleAssetBackfillCompletedMini") != std::string::npos &&
              stopIdle.find("state.idleAssetBackfillCompletedCover") != std::string::npos,
          "BDD: Headless idle backfill diagnostics report completion, interruption, mini, and cover counts");
    check(source.find("StopIdleAssetBackfill(state, true, false)") != std::string::npos,
          "BDD: Finished headless idle backfill still takes the explicit completion path");
    check(source.find("RefreshIdleAssetBackfillProgress(state)") != std::string::npos &&
              source.find("IdleAssetBackfillStart: db=%s mini=%lld coverMetadata=%lld") != std::string::npos,
          "BDD: Active headless idle backfill keeps progress state and start diagnostics");
    check(source.find("BuildIdleAssetBackfillWaitingToast") == std::string::npos &&
              source.find("BuildIdleAssetBackfillBlockedToast") == std::string::npos &&
              source.find(u8"空闲补齐等待中") == std::string::npos,
          "BDD: Idle asset backfill never keeps a countdown or blocker overlay on screen");
    check(source.find("BuildIdleAssetBackfillActiveToast") == std::string::npos &&
              source.find("DrawIdleAssetBackfillToast") == std::string::npos &&
              source.find("idleAssetBackfillToast") == std::string::npos,
          "BDD: Idle asset backfill remains headless without toast state or drawing");
}

static void checkBackgroundReconcileScanToastContract() {
    const std::string appState = readFirstExistingTextFile({
        "src/AppState.h",
        "../src/AppState.h",
        "../../src/AppState.h",
    });
    check(!appState.empty(), "AppState.h source is readable for background reconcile scan toast contract");
    check(appState.find("backgroundReconcileScanToast") != std::string::npos,
          "BDD: Background root rescan has dedicated visible toast state");

    const std::string source = readFirstExistingTextFile({
        "src/main.cpp",
        "../src/main.cpp",
        "../../src/main.cpp",
    });
    check(!source.empty(), "main.cpp source is readable for background reconcile scan toast contract");
    check(source.find("PollBackgroundReconcileScanToast(state)") != std::string::npos,
          "BDD: UI main loop polls background root rescan status for visible feedback");
    check(source.find("state.database.isReconcileScanRunning()") != std::string::npos,
          "BDD: Background root rescan toast is driven from the main-thread observable reconcile state");
    check(source.find("DrawBackgroundReconcileScanToast(state)") != std::string::npos,
          "BDD: Background root rescan has a bottom-right lightweight toast renderer");
    check(source.find("ImGui::GetForegroundDrawList(vp)") != std::string::npos,
          "BDD: Background root rescan toast uses the foreground draw list");
    check(source.find("RenderForegroundMaintenanceToasts(state)") != std::string::npos &&
              source.find("DrawBackgroundReconcileScanToast(state)") != std::string::npos,
          "BDD: Background root rescan toast is rendered in the final foreground pass");
    check(source.find(u8"后台重扫中") != std::string::npos &&
              source.find(u8"后台重扫完成") != std::string::npos,
          "BDD: Background root rescan shows both start/running and finished messages");

    const std::string databaseSource = readFirstExistingTextFile({
        "src/Database.cpp",
        "../src/Database.cpp",
        "../../src/Database.cpp",
    });
    check(!databaseSource.empty(), "Database.cpp source is readable for background reconcile scan toast contract");
    check(databaseSource.find("m_watcherRescanActive.store") != std::string::npos,
          "BDD: Background root rescan worker still owns only the atomic reconcile-running signal");
}

static void checkPlaybackMainDetailStaysSessionOnlyContract() {
    const std::string windowsMain = readFirstExistingTextFile({
        "src/WindowsMain.cpp",
        "../src/WindowsMain.cpp",
        "../../src/WindowsMain.cpp",
    });
    check(!windowsMain.empty(), "WindowsMain.cpp source is readable for playback detail contract");
    const std::string playedBody =
        extractFunctionBody(windowsMain, "static void PumpPlayedMetadataCache(AppState& state, bool isMouseIdle)");
    check(!playedBody.empty(), "PumpPlayedMetadataCache body can be inspected");
    check(playedBody.find("saveWaveformDetailByPath(") == std::string::npos,
          "played-file metadata cache does not persist main waveform detail");
    check(playedBody.find("PlaybackDetailCacheSkipped") != std::string::npos,
          "played-file metadata cache records skipped detail persistence");

    const std::string audioEngine = readFirstExistingTextFile({
        "src/AudioEngine.cpp",
        "../src/AudioEngine.cpp",
        "../../src/AudioEngine.cpp",
    });
    check(!audioEngine.empty(), "AudioEngine.cpp source is readable for playback detail contract");
    const std::string detailBody =
        extractFunctionBody(audioEngine, "void AudioEngine::requestWaveformDetail(int targetPeaks)");
    check(!detailBody.empty(), "AudioEngine::requestWaveformDetail body can be inspected");
    check(detailBody.find("saveWaveformDetailByPath(") == std::string::npos,
          "AudioEngine playback detail generation does not persist main waveform detail");
    check(audioEngine.find("PlaybackDetailPersistSkipped") != std::string::npos,
          "AudioEngine records skipped playback detail persistence");
}

static void checkFolderTreeRemovePathClearsAllLocalUiStateContract() {
    const std::string source = readFirstExistingTextFile({
        "src/main.cpp",
        "../src/main.cpp",
        "../../src/main.cpp",
    });
    check(!source.empty(), "main.cpp source is readable for folder remove-path contract");

    const size_t popup = source.find("##clrcache");
    const size_t nextDialog = popup == std::string::npos
        ? std::string::npos
        : source.find("Rename playlist dialog", popup);
    const std::string clearCacheBlock =
        (popup == std::string::npos || nextDialog == std::string::npos || nextDialog <= popup)
            ? std::string{}
            : source.substr(popup, nextDialog - popup);

    check(!clearCacheBlock.empty(), "folder remove-path confirmation block is discoverable");
    check(clearCacheBlock.find("removeFolderSubtreeFromDb(targetPath)") != std::string::npos,
          "BDD: Folder-tree remove path deletes all Aris DB records for the subtree");
    check(clearCacheBlock.find("FinalizeFolderSubtreeDbRemoval(state, targetPath, removed)") != std::string::npos,
          "BDD: Folder-tree remove path runs full UI/database refresh finalization");
    check(clearCacheBlock.find("clearWaveformCacheByFolder(targetPath)") == std::string::npos,
          "BDD: Folder-tree remove path does not regress to waveform-only cache clearing");
    check(clearCacheBlock.find(u8"波形缓存记录") == std::string::npos,
          "BDD: Folder-tree remove path toast does not describe the action as waveform-only");

    const std::string removeDbPopupBody =
        extractFunctionBody(source, "static void RenderRemoveFolderDbConfirmPopup");
    check(!removeDbPopupBody.empty(), "folder remove-path secondary confirmation body can be inspected");
    check(removeDbPopupBody.find("removeFolderSubtreeFromDb(targetPath)") != std::string::npos,
          "BDD: Secondary folder remove path entry deletes all Aris DB records for the subtree");
    const std::string ledger = readFirstExistingTextFile({"locale/harken.ui.strings.json"});
    check(removeDbPopupBody.find("L(Main_ClearPathExplainWrap)") != std::string::npos &&
              ledgerEntryContains(ledger, "Main_ClearPathExplainWrap", u8"Harken Search 数据库移除此路径"),
          "BDD: Secondary folder remove path entry uses Harken Search product wording");
    check(!ledgerEntryContains(ledger, "Main_ClearPathExplainWrap", u8"从数据库移除此路径"),
          "BDD: Secondary folder remove path entry does not regress to database-only wording");
    check(ledgerEntryContains(ledger, "Main_ClearPathExplainWrap", u8"全部入库记录"),
          "BDD: Secondary folder remove path entry explains record removal scope");
    check(ledgerEntryContains(ledger, "Main_ClearPathExplainWrap", u8"封面/波形本地缓存引用"),
          "BDD: Secondary folder remove path entry explains related cache cleanup");

    std::string finalizerBody =
        extractFunctionBody(source, "static void FinalizeFolderSubtreeDbRemoval");
    if (finalizerBody.empty()) {
        finalizerBody = extractFunctionBody(source, "void FinalizeFolderSubtreeDbRemoval");
    }
    check(!finalizerBody.empty(), "folder removal finalizer body can be inspected");
    check(finalizerBody.find("ClearAllWaveformUiCaches(state)") != std::string::npos,
          "BDD: Folder removal clears mini/main waveform UI caches on the same frame");
    check(finalizerBody.find("state.audioEngine.clearCurrentWaveformVisual()") != std::string::npos,
          "BDD: Folder removal clears the currently rendered waveform visual");
    check(finalizerBody.find("RefreshFolderTreeAfterDbRemoval(state)") != std::string::npos,
          "BDD: Folder removal rebuilds and reloads the folder tree");
    check(finalizerBody.find("ForceCoverWallReload(state)") != std::string::npos,
          "BDD: Folder removal refreshes cover/facet caches after deleted rows");
    check(finalizerBody.find("RefreshSearchResults(state, true)") != std::string::npos,
          "BDD: Folder removal refreshes the visible result list after deleted rows");

    const std::string databaseSource = readFirstExistingTextFile({
        "src/Database.cpp",
        "../src/Database.cpp",
        "../../src/Database.cpp",
    });
    check(!databaseSource.empty(), "Database.cpp source is readable for folder remove-path latency contract");
    const std::string removeFolderBody =
        extractFunctionBody(databaseSource, "int Database::removeFolderSubtreeFromDb");
    check(!removeFolderBody.empty(), "Database::removeFolderSubtreeFromDb body can be inspected");
    check(removeFolderBody.find("compactCachePackageBestEffort") == std::string::npos,
          "BDD: Folder remove path does not run synchronous cache-package compact/VACUUM from the confirm action");
    check(removeFolderBody.find("CREATE TEMP TABLE IF NOT EXISTS temp.remove_folder_subtree_candidates") != std::string::npos,
          "BDD: Folder remove path stages matching rows once for batch SQL cleanup");
    check(removeFolderBody.find("DELETE FROM audio_files WHERE id = ?") == std::string::npos,
          "BDD: Folder remove path does not delete large subtrees one audio row at a time");
}

static void checkCoverWallLifecycleCacheContract() {
    const std::string appState = readFirstExistingTextFile({
        "src/AppState.h",
        "../src/AppState.h",
        "../../src/AppState.h",
    });
    check(!appState.empty(), "AppState.h source is readable for cover wall lifecycle cache contract");
    check(appState.find("globalItemsCache") != std::string::npos,
          "BDD: CoverWallState keeps a dedicated full-library cover wall cache");
    check(appState.find("globalItemsCacheValid = false") != std::string::npos,
          "BDD: Database content invalidation clears the full-library cover wall cache");

    const std::string source = readFirstExistingTextFile({
        "src/main.cpp",
        "../src/main.cpp",
        "../../src/main.cpp",
    });
    check(!source.empty(), "main.cpp source is readable for cover wall lifecycle cache contract");
    check(source.find("CanReuseGlobalCoverWallCache") != std::string::npos,
          "BDD: Global cover wall reuses a local cached list when the database has not changed");
    check(source.find("StoreGlobalCoverWallCache") != std::string::npos,
          "BDD: Global cover wall stores a sanitized local cache after loading");
    check(source.find("useFacetCoverWall != cw.prevUseFacetMode || state.needsCoverWallReload") == std::string::npos,
          "BDD: Facet/global mode switching alone does not invalidate the global cover cache");
    check(source.find("!state.needsCoverWallReload &&\n           cw.globalItemsCacheDbPath") == std::string::npos,
          "BDD: A display reload flag does not block full-library cover wall cache reuse");
    check(source.find("if (state.needsCoverWallReload) {\n                    cw.loaded = false;\n                    cw.globalItemsCache.clear();") == std::string::npos,
          "BDD: A display reload flag does not clear the full-library cover wall cache");
    check(source.find("kCoverWallFolderDiscoveryMaxFiles") != std::string::npos &&
          source.find("cw.items.empty() && allowFolderDiscovery") != std::string::npos,
          "BDD: Large-library restart does not run full folder-cover discovery just because memory cache is empty");
    check(source.find("PersistedIndexEmptySkipFolderDiscovery") != std::string::npos,
          "BDD: Cover wall diagnostics explain when large-library folder discovery is skipped");

    const std::string windowsMain = readFirstExistingTextFile({
        "src/WindowsMain.cpp",
        "../src/WindowsMain.cpp",
        "../../src/WindowsMain.cpp",
    });
    check(!windowsMain.empty(), "WindowsMain.cpp source is readable for startup cover wall preload contract");
    check(windowsMain.find("StartupFirstScreenPreloadPrimed candidates=%zu mirrored=%zu budgetElapsed=%d elapsedMs=%.2f") != std::string::npos &&
              windowsMain.find("WaitForStartupCoverSourceForQa") != std::string::npos &&
              windowsMain.find("WarmStartupFirstScreenCoversForQa") != std::string::npos &&
              windowsMain.find("kStartupCoverCandidatePoolCount = 4096") != std::string::npos &&
              windowsMain.find("kStartupVisibleCoverTarget = 12") != std::string::npos &&
              windowsMain.find("startup_first_screen_preload") != std::string::npos,
          "BDD: Startup logs explicit bounded first-screen cover-wall preload before entering the main UI");
    const size_t betaSplashStart = windowsMain.find("static void RunBetaSplashScreen");
    const size_t webViewLoadingStart = windowsMain.find("static void RunWebView2LoadingScreen");
    check(betaSplashStart != std::string::npos &&
              webViewLoadingStart != std::string::npos &&
              betaSplashStart < webViewLoadingStart &&
              windowsMain.substr(betaSplashStart, webViewLoadingStart - betaSplashStart).find("WaitForStartupCoverSourceForQa") != std::string::npos &&
              windowsMain.substr(betaSplashStart, webViewLoadingStart - betaSplashStart).find("WarmStartupFirstScreenCoversForQa") != std::string::npos &&
              windowsMain.substr(betaSplashStart, webViewLoadingStart - betaSplashStart).find("renderBetaSplashFrame(L(Splash_WarmFirstCover))") != std::string::npos,
          "BDD: Default beta splash path warms first-screen covers before restoring the main UI");
    const size_t startupWarmStart = windowsMain.find("static void WarmStartupFirstScreenCoversForQa");
    const size_t readyToWorkStart = windowsMain.find("static void EnsureReadyToWorkFirstScreenCoverCandidatesForQa");
    check(startupWarmStart != std::string::npos &&
              readyToWorkStart != std::string::npos &&
              startupWarmStart < readyToWorkStart &&
              windowsMain.substr(startupWarmStart, readyToWorkStart - startupWarmStart).find("IsCoverIdentityKeyPendingOrFailed") == std::string::npos,
          "BDD: Startup first-screen warmup keeps polling pending cover requests until uploaded textures can be mirrored");
    check(startupWarmStart != std::string::npos &&
              readyToWorkStart != std::string::npos &&
              startupWarmStart < readyToWorkStart &&
              windowsMain.substr(startupWarmStart, readyToWorkStart - startupWarmStart).find("CoverWallShouldRecordConfirmedFailureForQa(item)") != std::string::npos &&
              windowsMain.substr(startupWarmStart, readyToWorkStart - startupWarmStart).find("readyCount >= readyTarget") != std::string::npos &&
              windowsMain.substr(startupWarmStart, readyToWorkStart - startupWarmStart).find("retryAfterFrame[item.key] = frame + 6") != std::string::npos,
          "BDD: Startup first-screen warmup heals failed cover candidates and exits on the visible target");
    check(windowsMain.find("StartupPersistentPreload skipped=deferred_to_main_ui") == std::string::npos,
          "BDD: Startup cover-wall diagnostics no longer advertise deferred-to-main-UI preload");
    check(windowsMain.find("state.preloadedCoverWallEntries = state.database.getGlobalCoverWallEntries();") == std::string::npos,
          "BDD: Startup does not synchronously reuse the persisted cover-wall index before entering the main UI");

    const std::string fileInfoPanel = readFirstExistingTextFile({"src/FileInfoPanel.cpp"});
    check(source.find("RenderFileInfoPanelContent(state,") != std::string::npos,
          "main UI still renders the extracted file info panel");
    const std::string renderBody =
        extractFunctionBody(fileInfoPanel, "void RenderFileInfoPanelContent(AppState& state, float panelWidth)");
    check(!renderBody.empty(), "RenderFileInfoPanelContent body can be inspected for cover wall lifecycle");
    check(renderBody.find("RemoveFailedCoverItems(items)") == std::string::npos,
          "BDD: Cover wall render loop does not re-dedupe or prune failed cover items every frame");
}

static void checkMeterColumnFreezeContract() {
    const std::string appState = readFirstExistingTextFile({
        "src/AppState.h",
        "../src/AppState.h",
        "../../src/AppState.h",
    });
    check(!appState.empty(), "AppState.h source is readable for meter-column contract");
    check(appState.find("needsMeterDataComputation") != std::string::npos,
          "BDD: Meter-column runtime gate is expressed as meter data computation, not LUFS-only state");

    const std::string asyncWorkerHeader = readFirstExistingTextFile({
        "src/AsyncWorker.h",
        "../src/AsyncWorker.h",
        "../../src/AsyncWorker.h",
    });
    check(!asyncWorkerHeader.empty(), "AsyncWorker.h source is readable for meter-column contract");
    check(asyncWorkerHeader.find("bool wantMeterData = false;") != std::string::npos,
          "BDD: Visible waveform requests carry explicit meter-data intent");

    const std::string asyncWorker = readFirstExistingTextFile({
        "src/AsyncWorker.cpp",
        "../src/AsyncWorker.cpp",
        "../../src/AsyncWorker.cpp",
    });
    check(!asyncWorker.empty(), "AsyncWorker.cpp source is readable for meter-column contract");
    check(asyncWorker.find("req.wantMeterData = g_appState && g_appState->needsMeterDataComputation;") != std::string::npos,
          "BDD: Worker request snapshots the unified meter-column visibility gate");
    check(asyncWorker.find("const bool needsMeterBackfill =\n                    req.wantMeterData &&") != std::string::npos,
          "BDD: Cache-hit waveform path can backfill meter data without deleting mini cache");
    check(asyncWorker.find("m_db->saveLoudnessByPath(filePath,") != std::string::npos,
          "BDD: Meter backfill persists loudness data after cache-hit analysis");

    const std::string source = readFirstExistingTextFile({
        "src/main.cpp",
        "../src/main.cpp",
        "../../src/main.cpp",
    });
    check(!source.empty(), "main.cpp source is readable for meter-column contract");
    check(source.find("strcmp(name, u8\"LUFS\") == 0 ||\n                        strcmp(name, u8\"RMS\") == 0 ||\n                        strcmp(name, u8\"TruePeak\") == 0") != std::string::npos,
          "BDD: Meter-column visibility gate includes LUFS, RMS, and TruePeak");
    check(source.find("state.database.clearWaveformCache()) {\n                            state.waveformCache.clear();") == std::string::npos,
          "BDD: Enabling meter columns does not clear the whole-library waveform cache");
    check(source.find("if (!g_resultsMiniWaveColumnVisible && !g_resultsMeterColumnsVisible) return;") != std::string::npos,
          "BDD: Visible-row scheduling runs for meter columns even when the mini-wave column is hidden");
    check(source.find("const bool shouldScheduleMeters = g_resultsMeterColumnsVisible;") != std::string::npos,
          "BDD: Browser rows participate in the same bounded meter backfill scheduling");
    check(source.find("ImGui::TableSetColumnEnabled(1, true);") == std::string::npos,
          "BDD: Results table no longer force-reopens the mini-wave column after the user hides it");
    check(source.find("} else if (hasCache && !cacheIt->second.peaks.empty()) {") == std::string::npos,
          "BDD: Browser TruePeak does not fall back to pseudo dBTP derived from mini peaks");
}

static void checkStartupTestListNameSubmitContract() {
    const std::string windowsMain = readFirstExistingTextFile({
        "src/WindowsMain.cpp",
        "../src/WindowsMain.cpp",
        "../../src/WindowsMain.cpp",
    });
    check(!windowsMain.empty(), "WindowsMain.cpp source is readable for open-source startup contract");
    check(windowsMain.find("SubmitStartupTestListRequestIfNeeded(const std::string& settingsPath,") == std::string::npos,
          "BDD: Open-source startup must not keep the tester-name authorization submission helper");
    check(windowsMain.find("startupAuthRequestMachineId") == std::string::npos &&
              windowsMain.find("startupAuthRequestSubmitted") == std::string::npos,
          "BDD: Open-source startup must not persist authorization request throttling keys");
    check(windowsMain.find("ShowNativeRegDialog(HINSTANCE hInstance)") == std::string::npos,
          "BDD: Open-source startup must not prompt for tester authorization registration");
    check(windowsMain.find("DownloadWhitelistFromCloud(") == std::string::npos,
          "BDD: Open-source startup must not download a machine whitelist before entering the app");
    check(windowsMain.find("std::vector<std::string> devIDs = {") == std::string::npos,
          "BDD: Open-source startup must not hardcode developer machine whitelist IDs");
    check(windowsMain.find("Auro 测试授权") == std::string::npos,
          "BDD: Open-source startup must not show the tester authorization copy");
    check(windowsMain.find("Authorization gate bypassed for direct file playback startup.") == std::string::npos,
          "BDD: File-player startup no longer talks about bypassing an authorization gate");
    check(windowsMain.find("g_isAuthorized") == std::string::npos,
          "BDD: Open-source startup no longer tracks an authorization gate flag");
    check(windowsMain.find("s_isDevMode = PathExistsUtf8(exeDirUtf8 + \"\\\\dev_mode.txt\");") != std::string::npos,
          "BDD: Non-internal local dev-mode startup bypass is preserved");
    check(windowsMain.find("const bool bypassStartupLogin =") != std::string::npos &&
              windowsMain.find("s_isIngestMode || IsHarkenSearchHarnessMultiInstanceEnabled();") != std::string::npos,
          "BDD: Startup login bypass is unified for ingest and harness lanes (dev mode shows the welcome screen)");
    check(windowsMain.find("Startup login skipped for dedicated scan ingestion.") != std::string::npos,
          "BDD: Dedicated scan ingestion skips the startup login surface");
    check(windowsMain.find("Startup login skipped for harness lane.") != std::string::npos,
          "BDD: Harness lanes skip the startup login surface");
    check(windowsMain.find("Startup login skipped for dev mode.") == std::string::npos,
          "BDD: Dev mode no longer skips the startup login surface (welcome screen visible to developers)");
}

static void checkFilePlayerStartupBypassContract() {
    const std::string windowsMain = readFirstExistingTextFile({
        "src/WindowsMain.cpp",
        "../src/WindowsMain.cpp",
        "../../src/WindowsMain.cpp",
    });
    check(!windowsMain.empty(), "WindowsMain.cpp source is readable for file-player startup bypass contract");
    check(windowsMain.find("const bool startupBypassForDirectFilePlayback = !startupAudioFilePathUtf8.empty();") != std::string::npos,
          "BDD: Direct audio launch is detected before startup login/loading decisions");
    check(windowsMain.find("Authorization gate bypassed for direct file playback startup.") == std::string::npos,
          "BDD: File-player startup bypass no longer references an authorization gate");
    check(windowsMain.find("Startup login skipped for direct file playback.") != std::string::npos,
          "BDD: File-player startup bypass skips the startup login surface");
    check(windowsMain.find("Startup loading gate bypassed for direct file playback.") != std::string::npos,
          "BDD: File-player startup bypass skips the startup loading surface");
}

static void checkReadyToWorkStartupBypassContract() {
    const std::string windowsMain = readFirstExistingTextFile({
        "src/WindowsMain.cpp",
        "../src/WindowsMain.cpp",
        "../../src/WindowsMain.cpp",
    });
    check(!windowsMain.empty(), "WindowsMain.cpp source is readable for startup loading bypass contract");
    check(windowsMain.find("const bool bypassStartupLoadingGate =\n                startupBypassForDirectFilePlayback ||\n                s_isIngestMode;") != std::string::npos,
          "BDD: Startup loading bypass is limited to file-player and dedicated ingest lanes");
    check(windowsMain.find("Startup loading gate bypassed for dedicated scan ingestion.") != std::string::npos,
          "BDD: Dedicated scan ingestion bypasses the startup loading gate");
    check(windowsMain.find("Startup loading gate bypassed for harness lane.") == std::string::npos,
          "BDD: Harness lanes retain the startup loading/preheat gate");
    check(windowsMain.find("Startup loading gate bypassed for dev mode.") == std::string::npos,
          "BDD: Dev mode no longer bypasses the startup loading gate (loading screen visible to developers)");
}

static void checkEndUserPackagingContract() {
    const std::string installer = readFirstExistingTextFile({
        "installer/HarkenSearch_Setup.iss",
        "../installer/HarkenSearch_Setup.iss",
        "../../installer/HarkenSearch_Setup.iss",
    });
    check(!installer.empty(), "installer/HarkenSearch_Setup.iss is readable for end-user packaging contract");
    check(installer.find("#define MyAppVersion ") != std::string::npos,
          "BDD: End-user installer declares an app version without pinning a protocol-specific literal");
    check(installer.find("#define SourceDir \"..\\build_final\\Release\"") != std::string::npos,
          "BDD: End-user installer pulls binaries from the dedicated final-release build directory");
    check(installer.find("#define MyAppName \"Harken Search\"") != std::string::npos &&
              installer.find("#define MyAppExeName \"HarkenSearch.exe\"") != std::string::npos &&
              installer.find("OutputBaseFilename=HarkenSearch_Setup_v") != std::string::npos &&
              installer.find("SetupIconFile=..\\HarkenSearch.ico") != std::string::npos,
          "BDD: End-user installer publishes the formal Aris product identity");
    const size_t filesStart = installer.find("[Files]");
    const size_t publishEnd = installer.find("[Code]", filesStart);
    const std::string publishedEntries =
        filesStart != std::string::npos && publishEnd != std::string::npos && filesStart < publishEnd
            ? installer.substr(filesStart, publishEnd - filesStart)
            : std::string();
    check(!publishedEntries.empty() &&
              publishedEntries.find("auro_features.json") == std::string::npos &&
              publishedEntries.find("Auro.exe") == std::string::npos,
          "BDD: End-user installer does not publish superseded product artifacts");
}

int main() {
    checkScanDialogPrecomputeControlContract();
    checkLibraryWaveformCacheModeContracts();
    checkWatcherRescanUsesLightweightImportContract();
    checkWaveformWorkerCancelPreservesBackfillContract();
    checkScanBackfillStaysMiniOnlyContract();
    checkFastScanReuseDoesNotRequireMainDetailContract();
    checkIncrementalWaveformMenuFillsMiniOnlyContract();
    checkManualWaveformRebuildClickPathContract();
    checkFullWaveformRebuildDoesNotSynchronouslyClearWholeLibraryContract();
    checkIdleAssetBackfillContract();
    checkBackgroundReconcileScanToastContract();
    checkPlaybackMainDetailStaysSessionOnlyContract();
    checkFolderTreeRemovePathClearsAllLocalUiStateContract();
    checkCoverWallLifecycleCacheContract();
    checkMeterColumnFreezeContract();
    checkStartupTestListNameSubmitContract();
    checkFilePlayerStartupBypassContract();
    checkReadyToWorkStartupBypassContract();
    checkEndUserPackagingContract();

    fs::path dbDir = makeTempDir("db");
    fs::path scanDir = makeTempDir("scan");
    fs::path wavPath = scanDir / "tone.wav";

    if (!writeTinyWav(wavPath)) {
        std::printf("FAIL: could not write test WAV at %s\n",
                    wavPath.string().c_str());
        return 1;
    }
    check(fs::exists(wavPath), "test WAV created on disk");

    Database db;
    bool opened = db.open((dbDir / "main.db").string(), true);
    check(opened, "Database opens");
    if (!opened) return 1;

    auto& diag = DiagnosticsLogger::instance();
    diag.setEnabled(true);
    diag.setMaxEntries(512);
    diag.clear();

    check(db.getWaveformCacheMode() == WaveformCachePolicy::Mode::MiniAndMain,
          "BDD: Given an existing library without an explicit policy, when it opens, then it preserves high-speed cache mode");
    check(db.setWaveformCacheMode(WaveformCachePolicy::Mode::MiniOnly),
          "Database stores standard mini-only waveform policy");
    check(db.getWaveformCacheMode() == WaveformCachePolicy::Mode::MiniOnly,
          "BDD: Given a new Standard library policy is stored, when code reads the DB, then mini-only policy is returned");

    // Run the scan with precomputeWaveforms=true (the native Standard scan
    // default; QA can still pass cache_waveforms=false for cover-only probes).
    auto stats = db.scanDirectoryEx(scanDir.string(),
                                    /*recursive=*/true,
                                    /*updateExisting=*/true,
                                    /*removeMissing=*/false,
                                    /*progress=*/nullptr,
                                    /*cancelFlag=*/nullptr,
                                    /*workerThreads=*/0,
                                    /*precomputeWaveforms=*/true);
    check(stats.added >= 1, "Scan added at least one file");

    // Wait up to 5s for the precompute worker to drain. Tiny 4410-sample
    // WAV finishes far below this — the budget is for cold-process IO.
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (db.precomputeQueueDepth() > 0 &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    check(db.precomputeQueueDepth() == 0, "Precompute queue drained within 5s");
    check(db.precomputeProcessedCount() >= 1,
          "At least one file was successfully precomputed");
    check(db.precomputeFailedCount() == 0,
          "No precompute failures on a valid WAV");
    waitDiagDrain();
    std::string scanDiag = diag.getRecentJsonLines(128);
    check(scanDiag.find("ScanSummary") != std::string::npos,
          "scan emits ScanSummary diagnostics");
    check(scanDiag.find("ScanWaveformCacheReport") != std::string::npos,
          "scan emits waveform cache report diagnostics");
    check(scanDiag.find("Flush failed") == std::string::npos,
          "scan diagnostics contain no flush-failed events");
    check(scanDiag.find("Batch rollback triggered") == std::string::npos,
          "scan diagnostics contain no rollback events");

    // Cache hit: getWaveformByPath returns non-empty mini peaks.
    float trueRms = -200.0f;
    int miniChannels = 0;
    auto peaks = db.getWaveformByPath(wavPath.string(), &trueRms, &miniChannels);
    check(!peaks.empty(), "getWaveformByPath returns mini peaks after precompute");
    check(miniChannels >= 1, "miniChannels reflects mono/stereo");
    check(db.getWaveformDetailByPath(wavPath.string()).empty(),
          "BDD: Given a Standard library, when scan precomputes waveforms, then mini exists but main detail is not persisted");

    db.close();
    opened = db.open((dbDir / "main.db").string(), true);
    check(opened, "Database reopens after standard policy write");
    check(db.getWaveformCacheMode() == WaveformCachePolicy::Mode::MiniOnly,
          "BDD: Given a Standard library policy was saved, when the DB reopens, then the policy remains Standard");
    check(db.setWaveformCacheMode(WaveformCachePolicy::Mode::MiniAndMain),
          "Database stores high-speed waveform policy");

    db.close();

    std::error_code ec;
    fs::remove_all(dbDir, ec);
    fs::remove_all(scanDir, ec);
    diag.setEnabled(false);

    if (g_failures == 0) {
        std::printf("\nAll T-297 import-precompute checks passed.\n");
        return 0;
    }
    std::printf("\n%d failure(s)\n", g_failures);
    return 1;
}
