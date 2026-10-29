// test_search_workspace.cpp - saved single-search snapshot behavior.

#include "src/AppState.h"
#include "src/DatabaseSessionMediaCacheReset.h"
#include "src/DatabaseSessionScanStateReset.h"
#include "src/DatabaseSessionViewStateReset.h"
#include "src/PathUtils.h"
#include "src/WaveformRebuildRuntimeState.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

static int g_failures = 0;

static void check(bool cond, const char* desc) {
    if (cond) {
        std::printf("PASS: %s\n", desc);
    } else {
        std::printf("FAIL: %s\n", desc);
        ++g_failures;
    }
}

static void setSearchText(AppState& state, const char* text) {
#ifdef _WIN32
    strncpy_s(state.searchBuffer, sizeof(state.searchBuffer), text, _TRUNCATE);
#else
    std::strncpy(state.searchBuffer, text, sizeof(state.searchBuffer) - 1);
    state.searchBuffer[sizeof(state.searchBuffer) - 1] = '\0';
#endif
}

static std::string readMainSource() {
    const std::filesystem::path sourcePath =
        std::filesystem::path(__FILE__).parent_path().parent_path() / "src" / "main.cpp";
    for (const std::filesystem::path& path :
         {sourcePath, std::filesystem::path("src/main.cpp"),
          std::filesystem::path("../src/main.cpp"),
          std::filesystem::path("../../src/main.cpp")}) {
        std::ifstream in(path, std::ios::binary);
        if (!in) continue;
        std::ostringstream contents;
        contents << in.rdbuf();
        return contents.str();
    }
    return {};
}

int main() {
    AppState state;
    check(!state.settings.autoReconcileOnStartup,
          "startup reconcile is opt-in by default for large-library safety");

    AppState targetState;
    AudioRecord targetRecord;
    targetRecord.id = 101;
    targetRecord.filePath = "C:/SFX/impact.wav";
    targetState.searchResults.push_back(targetRecord);
    targetState.currentFileId = 7;
    check(targetState.beginAudioTargetFromSearchResult(0),
          "audio target accepts a valid search-result index");
    check(targetState.selectedResultIndex == 0,
          "audio target updates selected result index");
    check(targetState.pendingAudioTargetFileId == 101 &&
              targetState.pendingAudioTargetPath == "C:/SFX/impact.wav",
          "audio target stores pending file identity");
    check(targetState.activeAudioTargetFileId() == 101,
          "pending audio target overrides previous current file for rendering");
    targetState.clearPendingAudioTarget();
    check(targetState.activeAudioTargetFileId() == 7,
          "current file identity is restored after pending target clears");
    check(!targetState.beginAudioTargetFromSearchResult(9),
          "audio target rejects out-of-range index");

    {
        const std::string selectedFilePath =
            "H:/Sound effect/综合/火气/[刮擦声] Scrape/Scrape_Swish_16.wav";
#ifdef _WIN32
        const std::string selectedFileFolder =
            "H:\\Sound effect\\综合\\火气\\[刮擦声] Scrape";
#else
        const std::string selectedFileFolder =
            "H:/Sound effect/综合/火气/[刮擦声] Scrape";
#endif
        const std::string folderTreeFilter =
            "H:/Sound effect/综合/火气";
#ifdef _WIN32
        const std::string expectedFolderTreeFilter =
            "H:\\Sound effect\\综合\\火气";
#else
        const std::string expectedFolderTreeFilter =
            "H:/Sound effect/综合/火气";
#endif
        check(HarkenSearch::ResolveFolderBreadcrumbDisplayPath(
                  selectedFilePath,
                  "",
                  folderTreeFilter) == selectedFileFolder,
              "folder breadcrumb follows selected audio file folder before folder-tree filter");
        check(HarkenSearch::ResolveFolderBreadcrumbDisplayPath(
                  "",
                  selectedFileFolder,
                  folderTreeFilter) == selectedFileFolder,
              "folder breadcrumb can fall back to selected record folder path");
        check(HarkenSearch::ResolveFolderBreadcrumbDisplayPath(
                  "",
                  "",
                  folderTreeFilter) == expectedFolderTreeFilter,
              "folder breadcrumb falls back to folder-tree filter when no file is selected");
        check(HarkenSearch::ResolveFolderBreadcrumbDisplayLabel("", "All Files") == "All Files",
              "folder breadcrumb keeps a fallback label when no path is available");
        check(HarkenSearch::ResolveActiveFolderBreadcrumbFilterPath(false, "H:/Stale/Tree").empty(),
              "folder breadcrumb ignores stale tree path when folder filter is inactive");
        check(HarkenSearch::ResolveActiveFolderBreadcrumbFilterPath(true, folderTreeFilter) == folderTreeFilter,
              "folder breadcrumb can use active folder filter as fallback");

        const std::string mainSource = readMainSource();
        const size_t breadcrumbClick = mainSource.find("const std::string canonNext = HarkenSearch::CanonicalFolderPath(payloadPath);");
        const size_t refreshAfterClick = breadcrumbClick == std::string::npos
            ? std::string::npos
            : mainSource.find("RefreshSearchResults(state);", breadcrumbClick);
        check(breadcrumbClick != std::string::npos && refreshAfterClick != std::string::npos &&
                  mainSource.find("state.clearSelection();", breadcrumbClick) < refreshAfterClick,
              "breadcrumb ancestor click clears the complete result selection before refreshing the folder scope");
    }

    setSearchText(state, "artist:Ben impact NOT noisy");
    state.folderFilterActive = true;
    state.selectedFolderPath = "C:/SFX/Impacts";

    int idx = state.saveSavedSearchFromCurrent("Impacts");
    check(idx == 0, "first saved search returns index 0");
    check(state.savedSearches.size() == 1, "one saved search stored");
    check(state.savedSearches[0].name == "Impacts", "saved search name stored");
    check(state.savedSearches[0].snapshot.searchText == "artist:Ben impact NOT noisy",
          "saved search captures search text");
    check(state.savedSearches[0].snapshot.folderFilterActive,
          "saved search captures folder filter state");
    check(state.savedSearches[0].snapshot.selectedFolderPath == "C:/SFX/Impacts",
          "saved search captures folder path");

    setSearchText(state, "");
    state.folderFilterActive = false;
    state.selectedFolderPath.clear();
    state.needsTabResultsRefresh = false;
    bool applied = state.applySavedSearch(idx);
    check(applied, "applySavedSearch succeeds for valid index");
    check(std::string(state.searchBuffer) == "artist:Ben impact NOT noisy",
          "applySavedSearch restores search text");
    check(state.folderFilterActive && state.selectedFolderPath == "C:/SFX/Impacts",
          "applySavedSearch restores folder filter");
    check(state.needsTabResultsRefresh, "applySavedSearch requests result refresh");
    check(state.savedSearches[0].lastUsed > 0, "applySavedSearch updates lastUsed");

    state.savedSearches[0].pinned = true;
    setSearchText(state, "category:metal hit");
    int upsertIdx = state.saveSavedSearchFromCurrent("Impacts");
    check(upsertIdx == 0, "same-name save upserts existing saved search");
    check(state.savedSearches.size() == 1, "upsert does not append duplicate saved search");
    check(state.savedSearches[0].pinned, "upsert preserves pinned state");
    check(state.savedSearches[0].snapshot.searchText == "category:metal hit",
          "upsert refreshes the saved snapshot");

    check(!state.applySavedSearch(99), "applySavedSearch rejects out-of-range index");
    check(!state.deleteSavedSearch(99), "deleteSavedSearch rejects out-of-range index");
    check(!state.renameSavedSearch(0, ""), "renameSavedSearch rejects empty name");
    check(state.renameSavedSearch(0, "Metal Hits"), "renameSavedSearch accepts valid name");
    check(state.savedSearches[0].name == "Metal Hits", "renameSavedSearch stores name");
    check(state.deleteSavedSearch(0), "deleteSavedSearch removes valid saved search");
    check(state.savedSearches.empty(), "saved searches empty after delete");

    setSearchText(state, "first");
    int first = state.saveSavedSearchFromCurrent("First");
    setSearchText(state, "second");
    int second = state.saveSavedSearchFromCurrent("Second");
    state.savedSearches[(size_t)first].lastUsed = 200;
    state.savedSearches[(size_t)second].lastUsed = 100;
    auto recentOrder = state.getSavedSearchDisplayOrder();
    check(recentOrder.size() >= 2 && recentOrder[0] == first && recentOrder[1] == second,
          "saved search display order uses recent activity");
    state.savedSearches[(size_t)second].pinned = true;
    auto pinnedOrder = state.getSavedSearchDisplayOrder();
    check(pinnedOrder.size() >= 2 && pinnedOrder[0] == second,
          "pinned saved search sorts before recent unpinned search");

    AppState emptyState;
    check(emptyState.saveSavedSearchFromCurrent("Empty") == -1,
          "empty view state is not saved");
    check(emptyState.saveSavedSearchFromCurrent("") == -1,
          "empty saved search name is rejected");

    AppState scanResetState;
    ScanStats scanFailureStats;
    scanFailureStats.addError("Scan_NetworkLibraryUnreachable");
    scanFailureStats.addError("Cannot read directory entry in C:/Library");
    scanFailureStats.addError("Recursive scan failed for folder: C:/Library");
    scanFailureStats.addError("Failed to read audio metadata: broken.wav");
    scanFailureStats.addError("Audio file too small to decode: empty.wav");
    scanFailureStats.addError("Database write failed during scan: database is locked");
    scanFailureStats.addError("ScanDatabaseOpenFailed: database is unavailable");
    scanFailureStats.addError("Scan thread crashed before completion.", 2);
    check(scanFailureStats.errors == 9 &&
              scanFailureStats.failureCounts.inaccessible == 3 &&
              scanFailureStats.failureCounts.unreadableFile == 1 &&
              scanFailureStats.failureCounts.noAudio == 1 &&
              scanFailureStats.failureCounts.databaseWrite == 2 &&
              scanFailureStats.failureCounts.other == 2 &&
              scanFailureStats.failureCounts.total() == scanFailureStats.errors,
          "scan failures are classified without losing the total");

    scanResetState.isScanning = true;
    scanResetState.scanPhase = AppState::ScanPhase::IncrementalScan;
    scanResetState.scanProgress = 7;
    scanResetState.scanTotal = 11;
    scanResetState.scanAddedCount = 1;
    scanResetState.scanUpdatedCount = 2;
    scanResetState.scanSkippedCount = 3;
    scanResetState.scanRemovedCount = 4;
    scanResetState.scanErrorCount = 5;
    scanResetState.scanFailureInaccessibleCount = 1;
    scanResetState.scanFailureUnreadableFileCount = 1;
    scanResetState.scanFailureNoAudioCount = 1;
    scanResetState.scanFailureDatabaseWriteCount = 1;
    scanResetState.scanFailureOtherCount = 1;
    scanResetState.scanIngestedCount = 6;
    scanResetState.scanIngestDurationMs = 99;
    scanResetState.scanStartedAtMs = 100;
    scanResetState.beginScanWorkflow(true);
    scanResetState.scanCancelRequested = true;
    scanResetState.scanWasCancelled = true;
    scanResetState.scanWasPaused = true;
    scanResetState.scanSummaryToastUntil = 9.0;
    scanResetState.setScanFile("old.wav");
    scanResetState.addScanError("old error");
    scanResetState.lastScannedPath = "C:/old";
    scanResetState.autoFilterToScannedPath = true;
    scanResetState.scanReadyToWorkPending = true;
    scanResetState.waveformFailed.insert("old.wav");

    aura::database_session::ResetDatabaseSessionScanState(scanResetState);

    check(!scanResetState.isScanning &&
              scanResetState.scanPhase == AppState::ScanPhase::Idle &&
              scanResetState.scanProgress == 0 && scanResetState.scanTotal == 0,
          "database session scan reset clears active progress");
    check(scanResetState.scanAddedCount == 0 &&
              scanResetState.scanUpdatedCount == 0 &&
              scanResetState.scanSkippedCount == 0 &&
              scanResetState.scanRemovedCount == 0 &&
              scanResetState.scanErrorCount == 0 &&
              scanResetState.scanFailureInaccessibleCount == 0 &&
              scanResetState.scanFailureUnreadableFileCount == 0 &&
              scanResetState.scanFailureNoAudioCount == 0 &&
              scanResetState.scanFailureDatabaseWriteCount == 0 &&
              scanResetState.scanFailureOtherCount == 0 &&
              scanResetState.scanIngestedCount == 0 &&
              scanResetState.scanIngestDurationMs == 0,
          "database session scan reset clears result counters");
    check(scanResetState.scanWorkflowStage == HarkenSearch::ScanWorkflowStage::Idle &&
              scanResetState.scanStartedAtMs == 0,
          "database session scan reset clears workflow and timing");
    check(!scanResetState.scanCancelRequested &&
              !scanResetState.scanWasCancelled && !scanResetState.scanWasPaused &&
              scanResetState.scanSummaryToastUntil == 0.0,
          "database session scan reset clears stop and summary flags");
    check(scanResetState.getScanFile().empty() &&
              scanResetState.getScanErrors().empty() &&
              scanResetState.lastScannedPath.empty() &&
              !scanResetState.autoFilterToScannedPath,
          "database session scan reset clears paths and errors");
    check(scanResetState.scanReadyToWorkPending &&
              scanResetState.waveformFailed.count("old.wav") == 1,
          "database session scan reset leaves excluded readiness and cache state");

    AppState resetState;
    AudioRecord resetRecord;
    resetRecord.id = 42;
    resetState.searchResults.push_back(resetRecord);
    resetState.totalFileCount = 1;
    resetState.loadedCount = 1;
    resetState.hasMoreResults = true;
    resetState.selectedResultIndex = 0;
    setSearchText(resetState, "old-library");
    resetState.selectedFolderPath = "C:/old";
    resetState.folderFilterActive = true;
    resetState.selectedPlaylistId = 9;
    resetState.playlistAggregatedView = true;
    resetState.viewMode = ViewMode::Playlist;
    resetState.findSameFilenameActive = true;
    resetState.randomSampleMode = true;
    resetState.selectedIndicesSet.insert(0);
    resetState.isDragging = true;
    resetState.searchSuggestCandidates.push_back("old");
    resetState.pendingSearchRequestId = 99;
    resetState.asyncSearchInProgress = true;
    resetState.coverFilterActive = true;
    resetState.playHistory.resize(1);
    resetState.sameFolderRecycleBuffer.push_back(resetRecord);
    resetState.playedFiles.insert("C:/old/a.wav");
    resetState.isScanning = true;
    resetState.scanProgress = 17;

    aura::database_session::ResetDatabaseSessionViewState(resetState);

    check(resetState.searchResults.empty() && resetState.totalFileCount == 0,
          "database session reset clears result pagination");
    check(resetState.searchBuffer[0] == '\0' && !resetState.folderFilterActive,
          "database session reset clears search and folder filters");
    check(resetState.selectedPlaylistId == -1 && resetState.viewMode == ViewMode::Database,
          "database session reset returns to database view");
    check(!resetState.randomSampleMode && resetState.selectedIndicesSet.empty() && !resetState.isDragging,
          "database session reset clears sampling and selection");
    check(resetState.searchSuggestCandidates.empty() && !resetState.asyncSearchInProgress,
          "database session reset clears async search presentation state");
    check(!resetState.coverFilterActive && resetState.playHistory.empty(),
          "database session reset clears cover filter and history");
    check(resetState.sameFolderRecycleBuffer.empty() && resetState.playedFiles.empty(),
          "database session reset clears same-folder and played-file state");
    check(resetState.isScanning && resetState.scanProgress == 17,
          "database session reset does not own scan lifecycle");

    AppState mediaResetState;
    mediaResetState.preloadedCoverWallEntries.resize(1);
    mediaResetState.preloadedCoverWallFullyDiscovered = true;
    mediaResetState.coverWall.generation = 10;
    mediaResetState.coverWall.loaded = true;
    mediaResetState.waveformCache["old.wav"] = WaveformData{};
    mediaResetState.waveformUrgentPending.push_back("urgent.wav");
    mediaResetState.waveformNormalPending.push_back("normal.wav");
    mediaResetState.waveformPendingSet.insert("urgent.wav");
    mediaResetState.meterFollowupPending.insert("meter.wav");
    mediaResetState.waveformFailed.insert("failed.wav");
    mediaResetState.visibleFilePaths.insert("visible.wav");
    mediaResetState.waveformRebuildActive = true;
    mediaResetState.scanProgress = 17;
    mediaResetState.spectrogramTexture = 77;
    mediaResetState.spectrogramFilePath = "old.wav";
    mediaResetState.spectrogramTimelineFrames = 123;
    mediaResetState.spectrogramSampleRate = 48000;

    aura::database_session::ResetDatabaseSessionMediaCaches(mediaResetState);

    check(mediaResetState.preloadedCoverWallEntries.empty() &&
              !mediaResetState.preloadedCoverWallFullyDiscovered,
          "database session media reset clears preloaded cover state");
    check(mediaResetState.coverWall.generation == 12 &&
              !mediaResetState.coverWall.loaded,
          "database session media reset invalidates and resets cover wall");
    check(mediaResetState.waveformCache.empty() &&
              mediaResetState.waveformFailed.empty(),
          "database session media reset clears waveform results");
    check(mediaResetState.waveformUrgentPending.empty() &&
              mediaResetState.waveformNormalPending.empty() &&
              mediaResetState.waveformPendingSet.empty() &&
              mediaResetState.meterFollowupPending.empty() &&
              mediaResetState.visibleFilePaths.empty(),
          "database session media reset clears pending waveform presentation");
    check(mediaResetState.scanProgress == 17 &&
              mediaResetState.spectrogramTexture == 77 &&
              mediaResetState.spectrogramFilePath == "old.wav" &&
              mediaResetState.spectrogramTimelineFrames == 123 &&
              mediaResetState.spectrogramSampleRate == 48000,
          "database session media reset leaves scan and GPU resource state");

    AppState rebuildState;
    rebuildState.waveformRebuildActive = true;
    rebuildState.waveformRebuildCancel = true;
    rebuildState.waveformRebuildFullMode = true;
    rebuildState.waveformRebuildMiniOnlyMode = true;
    rebuildState.waveformRebuildQueuedIds.insert(91);
    rebuildState.waveformRebuildOffset = 7;
    rebuildState.waveformRebuildTotal = 101;
    rebuildState.waveformRebuildMiniTotal = 81;
    rebuildState.waveformRebuildDetailTotal = 20;
    rebuildState.waveformRebuildProgress = 31;
    rebuildState.waveformRebuildSkipped = 4;
    rebuildState.waveformRebuildFailed = 2;
    rebuildState.waveformRebuildBatch.push_back(resetRecord);
    rebuildState.waveformRebuildBatchIdx = 1;
    rebuildState.waveformUrgentPending.push_back("urgent.wav");
    rebuildState.waveformNormalPending.push_back("normal.wav");
    rebuildState.waveformPendingSet.insert("urgent.wav");
    rebuildState.meterFollowupPending.insert("meter.wav");

    aura::waveform_rebuild::WaveformRebuildRuntimeState rebuildRuntime;
    rebuildRuntime.setBatchMiniOnly(true);
    rebuildRuntime.setIncrementalMiniOnly(true);
    check(rebuildRuntime.batchMiniOnly(),
          "waveform rebuild runtime owns batch mini-only state");
    check(rebuildRuntime.incrementalMiniOnly(),
          "waveform rebuild runtime owns incremental mini-only state");
    rebuildRuntime.Reset(rebuildState, false);

    check(!rebuildState.waveformRebuildActive &&
              !rebuildState.waveformRebuildCancel &&
              !rebuildState.waveformRebuildFullMode &&
              !rebuildState.waveformRebuildMiniOnlyMode,
          "waveform rebuild runtime reset clears mode state");
    check(rebuildState.waveformRebuildTotal == 0 &&
              rebuildState.waveformRebuildMiniTotal == 0 &&
              rebuildState.waveformRebuildDetailTotal == 0 &&
              rebuildState.waveformRebuildProgress == 0 &&
              rebuildState.waveformRebuildSkipped == 0 &&
              rebuildState.waveformRebuildFailed == 0,
          "waveform rebuild runtime reset clears progress state");
    check(rebuildState.waveformRebuildOffset == 0 &&
              rebuildState.waveformRebuildBatchIdx == 0 &&
              rebuildState.waveformRebuildBatch.empty() &&
              rebuildState.waveformRebuildQueuedIds.empty(),
          "waveform rebuild runtime reset clears batching state");
    check(rebuildState.waveformUrgentPending.empty() &&
              rebuildState.waveformNormalPending.empty() &&
              rebuildState.waveformPendingSet.empty() &&
              rebuildState.meterFollowupPending.empty(),
          "waveform rebuild runtime reset clears pending waveform requests");
    check(!rebuildRuntime.batchMiniOnly() && !rebuildRuntime.incrementalMiniOnly(),
          "waveform rebuild runtime reset clears private mode state");

    AppState historyState;
    for (int i = 0; i < 105; ++i) {
        historyState.addToPlayHistory(
            1000 + i,
            "C:/SFX/history_" + std::to_string(i) + ".wav",
            "history_" + std::to_string(i) + ".wav");
    }
    check(historyState.playHistory.size() == AppState::MAX_PLAY_HISTORY,
          "play history keeps the newest 100 records");
    check(!historyState.playHistory.empty() &&
              historyState.playHistory.front().fileName == "history_104.wav",
          "play history keeps the newest record at the front");
    check(!historyState.playHistory.empty() &&
              historyState.playHistory.back().fileName == "history_5.wav",
          "play history evicts the oldest records beyond the 100-item limit");

    // H1c (RQ-20260911-04): keyed transfer status must never leave consumers
    // with an empty string — the render point resolves key+args on the UI thread.
    {
        g_currentLanguage = AppLanguage::Chinese;
        AppState transferState;
        transferState.setTransferStatusFmt(LStr::TR_CopyErrFmt,
                                           AppState::TransferStatusKind::Error,
                                           "a.wav", "disk full");
        const std::string shown = transferState.TransferStatusText();
        check(!shown.empty(),
              "keyed transfer status renders non-empty at the consumer");
        check(shown.find("a.wav") != std::string::npos &&
                  shown.find("disk full") != std::string::npos,
              "keyed transfer status renders both ledger args at the render site");
        transferState.setTransferStatusCount(LStr::TR_DoneAllFmt,
                                             AppState::TransferStatusKind::Neutral, 7, -1);
        check(transferState.TransferStatusText().find("7") != std::string::npos,
              "count-arg keyed transfer status renders its number");
        transferState.setTransferStatus(std::string(), AppState::TransferStatusKind::Neutral);
        check(transferState.TransferStatusText().empty(),
              "setter-based clear returns keyed state to empty");
    }

    // BUG-20260915-02: scan-thread counting copy stores key + [n/m] prefix;
    // getScanFile() translates on the UI thread after a language switch.
    {
        AppState scanCopy;
        scanCopy.setScanFileKeyPrefixed(LStr::Wf_CountingFiles, "[1/3] ");
        g_currentLanguage = AppLanguage::Chinese;
        check(scanCopy.getScanFile() == std::string("[1/3] ") + L(Wf_CountingFiles),
              "keyed counting copy keeps [n/m] prefix in Chinese");
        g_currentLanguage = AppLanguage::English;
        check(scanCopy.getScanFile() == std::string("[1/3] ") + L(Wf_CountingFiles),
              "keyed counting copy follows English after language switch");
        g_currentLanguage = AppLanguage::Japanese;
        check(scanCopy.getScanFile() == std::string("[1/3] ") + L(Wf_CountingFiles),
              "keyed counting copy follows Japanese after language switch");
        scanCopy.setScanFile("[stale] ");
        scanCopy.setScanFileKey(LStr::Wf_CountingFiles);
        g_currentLanguage = AppLanguage::English;
        check(scanCopy.getScanFile() == L(Wf_CountingFiles),
              "unprefixed setScanFileKey still drops leftover literal text");
        g_currentLanguage = AppLanguage::Chinese;
    }

    std::printf("\n=== Search Workspace: %d failure(s) ===\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
