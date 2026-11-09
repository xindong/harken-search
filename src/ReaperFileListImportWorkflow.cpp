#include "ReaperFileListImportWorkflow.h"

#include "Database.h"
#include "DiagnosticsLogger.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

namespace aura::reaper_import {

void Start(AppState& state, std::string importPath, std::string playlistName) {
    if (importPath.empty() || state.isScanning.load()) return;

    state.audioEngine.stop();
    state.lastScannedPath = importPath;
    state.isScanning = true;
    state.scanPhase.store(AppState::ScanPhase::InitialImport);
    state.scanProgress = 0;
    state.scanTotal = 1;
    state.setScanFile("Parsing REAPER MediaDB/FileList...");
    state.scanAddedCount = 0;
    state.scanUpdatedCount = 0;
    state.scanSkippedCount = 0;
    state.scanRemovedCount = 0;
    state.scanErrorCount = 0;
    state.scanIngestedCount = 0;
    state.scanIngestDurationMs = 0;
    state.scanSummaryToastUntil = 0.0;
    state.clearScanWorkflowState();
    state.clearScanErrors();
    state.scanCancelRequested = false;
    state.scanWasCancelled = false;
    state.scanWasPaused = false;
    state.scanDataCommitted = false;
    state.needsDatabaseRefresh = false;
    state.scanDialogAutoStart = false;
    state.showScanDialog = true;

    const uint64_t scanGeneration = state.beginNewScanTask();
    state.beginScanWorkflow(false);
    std::shared_ptr<std::atomic<bool>> scanCancelToken = state.getActiveScanCancelToken();
    const std::string scanDbPath = !state.currentDbPathUtf8.empty()
        ? state.currentDbPathUtf8
        : state.database.getMainDbPath();

    DIAG_SYSTEM_FMT("Playlist",
                    "ReaperFileListImportStart path=%s playlist=%s",
                    importPath.c_str(),
                    playlistName.c_str());
    state.scanThread = std::thread([&state, importPath = std::move(importPath),
                                    playlistName = std::move(playlistName),
                                    scanGeneration, scanCancelToken, scanDbPath]() {
        Database::PlaylistImportResult result;
        try {
            Database importDb;
            const bool opened = !scanDbPath.empty() && importDb.open(scanDbPath);
            if (!opened) {
                result.error = importDb.getLastError();
                if (result.error.empty()) result.error = "open import database failed";
            } else {
                result = importDb.importReaperFileListToPlaylist(
                    importPath,
                    playlistName,
                    [&state, scanGeneration](int current, int total,
                                             const std::string& file,
                                             bool dataCommitted) {
                        if (!state.isScanTaskCurrent(scanGeneration)) return;
                        state.scanProgress = current;
                        state.scanTotal = std::max(total, 1);
                        state.setScanFile(file);
                        if (dataCommitted) state.scanDataCommitted = true;
                    },
                    scanCancelToken.get());
            }
        } catch (...) {
            result.error = "REAPER import thread crashed before completion";
        }

        if (!state.isScanTaskCurrent(scanGeneration)) return;
        const bool cancelled =
            state.isScanTaskCancelled(scanGeneration, scanCancelToken) ||
            result.error == "cancelled";
        state.scanWasCancelled = cancelled;
        state.scanProgress = result.uniqueFiles;
        state.scanTotal = std::max(result.uniqueFiles, 1);
        state.scanAddedCount = result.indexedFiles;
        state.scanUpdatedCount = result.addedFiles;
        state.scanSkippedCount = result.missingFiles;
        state.scanErrorCount = result.failedIndexFiles;
        state.scanIngestedCount = result.indexedFiles + result.addedFiles;
        state.setScanFile(cancelled ? "REAPER import cancelled" : "REAPER import complete");
        state.scanPhase.store(AppState::ScanPhase::Idle);
        const bool importSucceeded = result.playlistId > 0 && result.error.empty();
        // The dedicated UI result handler activates and refreshes the new
        // playlist. A generic scan refresh would switch back to Database and
        // leave the visible count stale; failures may still have indexed files.
        state.needsDatabaseRefresh = !importSucceeded;
        state.setScanOutcome(result.uniqueFiles, result.uniqueFiles,
                             result.addedFiles, result.missingFiles,
                             result.failedIndexFiles);
        state.scanForegroundTasksDrained.store(true, std::memory_order_release);
        state.markScanCompletionReadyNow();
        state.resolveScanWorkflowTerminalStage(cancelled,
                                               !importSucceeded && !cancelled);
        state.isScanning = false;
        if (!result.error.empty() && result.error != "cancelled") {
            state.addScanError(result.error);
        }
        for (const auto& failure : result.indexFailureSamples) {
            state.addScanError(failure);
        }
        {
            std::lock_guard<std::mutex> lock(state.reaperImportResultMutex);
            state.reaperImportResult_ = std::move(result);
            state.reaperImportPlaylistName_ = playlistName;
        }
        state.reaperImportResultReady.store(true, std::memory_order_release);
    });
}

}  // namespace aura::reaper_import
