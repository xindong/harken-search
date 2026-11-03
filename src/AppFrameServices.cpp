#include "AppFrameServices.h"
#include "AppState.h"
#include "AppMainServices.h"
#include "UIRenderer.h"
#include "ModuleUI.h"
#include "WindowsMain.h"
#include "WorkerThreadPolicy.h"
#include "MemoryBudget.h"
#include "CoverModule.h"
#include "DiagnosticsLogger.h"
#include "FrameBudgetDiagnostics.h"
#include "imgui.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iterator>
#include <map>

BackgroundIoContext BuildAppBackgroundIoContext(AppState& state,
                                                                  bool contextMenuHeavyFreeze,
                                                                  bool coverWallPriorityMode,
                                                                  bool mouseIdle) {
    BackgroundIoContext ctx;
    const double now = ImGui::GetTime();
    ctx.contextMenuHeavyFreeze = contextMenuHeavyFreeze;
    ctx.coverWallPriorityMode = coverWallPriorityMode;
    ctx.playing = state.audioEngine.isPlaying();
    ctx.waveformRebuildActive = state.waveformRebuildActive;
    ctx.scanning = state.isScanning.load(std::memory_order_acquire);
    ctx.dbSwitchInProgress = state.dbSwitchInProgress;
    ctx.dbSwitchSettling = state.dbSwitchSettleFramesRemaining > 0;
    ctx.loadFilePending = state.audioEngine.isLoadFilePending();
    ctx.detailPending = state.audioEngine.isWaveformDetailPending();
    ctx.detailDeferring = !state.pendingDetailFilePath.empty();
    ctx.modeSwitchCoolingDown = state.lastProductModeSwitchTime > 0.0 &&
        (now - state.lastProductModeSwitchTime) < 2.5;
    ctx.mouseIdle = mouseIdle;
    ctx.userActive = ImGui::IsAnyMouseDown();
    ctx.hasBrowseAnchor = state.selectedResultIndex >= 0 || !state.audioEngine.getLoadedFilePath().empty();
    if (state.lastSelectionOrLoadTime > 0.0) {
        ctx.secondsSinceSelectionOrLoad = now - state.lastSelectionOrLoadTime;
    }
    return ctx;
}

namespace {
static bool MarkScanForegroundMiniTerminalForJob(AppState& state,
                                                 const aura::BackfillJob& job,
                                                 bool success,
                                                 const std::string& reason) {
    if (job.scanSessionId <= 0 ||
        job.kind != aura::BackfillJobKind::WaveformL1Overview) {
        return true;
    }
    return state.database.markScanForegroundMiniTerminal(
        job.scanSessionId,
        job.fileId,
        success ? ScanForegroundTerminalState::Cached : ScanForegroundTerminalState::Failed,
        reason);
}

static bool CanClearBackfillPendingForJob(AppState& state,
                                          const aura::BackfillJob& job,
                                          bool success,
                                          const std::string& reason) {
    return MarkScanForegroundMiniTerminalForJob(state, job, success, reason);
}

} // namespace

void PumpAppContinuousPlayback(AppState& state) {
    // Continuous playback
    static bool wasPlayingLastFrame = false;
    static float lastPlaybackPos = 0.0f;
    static int consecutiveLoadFailures = 0;

    if (state.settings.continuousPlayback && state.viewMode != ViewMode::Browser) {
        bool isPlaying = state.audioEngine.isPlaying();
        float currentPos = state.audioEngine.getPlaybackPosition();
        bool isLoaded = state.audioEngine.isLoaded();

        bool playbackEnded = (wasPlayingLastFrame && !isPlaying && lastPlaybackPos > 0.95f);
        bool stuckAtEnd = (isLoaded && !isPlaying && currentPos > 0.99f && lastPlaybackPos > 0.99f);

        if ((playbackEnded || stuckAtEnd) && !state.audioEngine.isLoadFilePending()) {
            bool success = false;

            if (state.settings.shufflePlayback) {
                PlayRandomFile(state);
                // loadFileAsync 是异步的，不能立即检查 isPlaying
                // 重置检测状态，防止下一帧重复触发
                wasPlayingLastFrame = false;
                lastPlaybackPos = 0.0f;
                success = true;
            } else {
                int nextIdx = state.selectedResultIndex + 1;
                if (nextIdx >= 0 && nextIdx < (int)state.searchResults.size()) {
                    state.selectedResultIndex = nextIdx;
                    state.selectedIndicesSet.clear();
                    state.selectedIndicesSet.insert(nextIdx);
                    state.lastClickedIndex = nextIdx;
                    state.scrollToSelected = true;
                    const auto& record = state.searchResults[nextIdx];
                    if (!record.filePath.empty()) {
                        state.audioEngine.loadFileAsync(record.filePath, true);
                        ScheduleDeferredWaveformDetail(state, record.filePath, AppState::WAVEFORM_DEFAULT_DETAIL_PEAKS);
                        state.currentFileId = record.id;
                        ResetWaveformZoom(state);
                        MarkFilePlayed(state, record.filePath, record.id);
                        success = true;
                    }
                    state.syncToTab();
                }
            }

            if (success) {
                consecutiveLoadFailures = 0;
            } else {
                consecutiveLoadFailures++;
                if (consecutiveLoadFailures > 3 && !state.settings.shufflePlayback) {
                    state.selectedResultIndex += consecutiveLoadFailures;
                    consecutiveLoadFailures = 0;
                }
            }
        }

        wasPlayingLastFrame = isPlaying;
        lastPlaybackPos = currentPos;
    } else {
        wasPlayingLastFrame = state.audioEngine.isPlaying();
        lastPlaybackPos = state.audioEngine.getPlaybackPosition();
        consecutiveLoadFailures = 0;
    }

}

bool PumpAppWaveforms(AppState& state, bool isMouseIdle, const WaveformFrameOptions& options) {
    bool seedSubmitted = false;
    // Async waveform loading: submit requests and consume completed items.
    {
        // Ensure waveform worker starts once.
        // Worker 可能已在 Splash Phase 4.8 中启动，start() 内部有幂等检查。
        static bool waveformWorkerStarted = false;
        static bool searchWorkerStarted = false;
        const bool holdColdMiniWorker = options.holdWorker;
        if (!waveformWorkerStarted && !holdColdMiniWorker) {
            // Browser/direct-file mode can source-decode visible mini waves
            // without an open library. The stable Database object remains
            // available for optional cache reads/writes once it is opened.
            state.waveformWorker.start(&state.database, aura::RecommendedWaveformWorkerThreads());
            waveformWorkerStarted = true;
        }
        if (!searchWorkerStarted && state.database.isOpen()) {
            state.searchWorker.start(&state.database);
            searchWorkerStarted = true;
        }

        // 1) Submit pending waveforms to background worker.
        //    Increased from 10→50: DB-cached reads complete in ~1ms each,
        //    so we can safely queue many more per frame for faster initial display.
        int submitted = 0;
#if AURA_BUILD_DIAGNOSTICS && !AURA_INTERNAL_BETA
        int listSubmitted = 0;
#endif
        // Keep the selected file's main waveform ahead of list mini-waveforms.
        // During rapid scroll/play, bulk list work can otherwise saturate CPU/disk
        // and make long-file main detail miss its user-visible window.
        const bool contextMenuHeavyFreezeForWaveform =
            ImGui::IsPopupOpen("RowContextMenu") ||
            ImGui::IsPopupOpen("##PlaylistSidebarContext");
        BackgroundIoContext miniIo = BuildAppBackgroundIoContext(
            state, contextMenuHeavyFreezeForWaveform, IsCoverWallPriorityMode(), isMouseIdle);
        if (options.ignorePlaybackForCapture) miniIo.playing = false;
        const auto miniDecision = ResolveMiniWaveformWorkerDecision(miniIo);
        static bool s_miniListWasForegroundBlocked = false;
        if (miniDecision.cancelQueuedListWork && !s_miniListWasForegroundBlocked) {
            state.waveformWorker.cancelAllPending();
            // In-flight meterOnly requests get dropped with the worker queue; clear
            // the dedup set so future visible-mini results can re-enqueue meters.
            state.meterFollowupPending.clear();
        }
        s_miniListWasForegroundBlocked = miniDecision.cancelQueuedListWork;
        const bool foregroundWaveformRebuild = state.waveformRebuildActive;
        const bool mainWaveformPending =
            !foregroundWaveformRebuild && state.audioEngine.isWaveformDetailPending();
        // Foreground scan completion owns priority-0 mini backfill. If the
        // worker stays throttled here, scan-created jobs can starve forever.
        const bool scanForegroundWaveformDrainActive =
            state.scanCompletionPending.load(std::memory_order_acquire) &&
            state.scanCompletionWaveformsPending.load(std::memory_order_acquire);
        const bool throttleWaveformWorker =
            !scanForegroundWaveformDrainActive &&
            (mainWaveformPending || miniDecision.throttleWorker);
        int submitBudget = miniDecision.submitListBudget;
        if (state.postSearchMiniWaveThrottleFrames > 0 && !scanForegroundWaveformDrainActive) {
            submitBudget = (std::min)(submitBudget, 1);
        }
        state.waveformWorker.setThrottleMode(throttleWaveformWorker);

        const std::string loadedWaveformPath = state.audioEngine.getLoadedFilePath();
#if AURA_BUILD_DIAGNOSTICS && !AURA_INTERNAL_BETA
        if (options.seedVisibleRequest && submitBudget > 0) {
            for (const auto& record : state.searchResults) {
                if (record.filePath.empty() || record.filePath == loadedWaveformPath) continue;
                state.visibleFilePaths.insert(record.filePath);
                if (state.waveformWorker.submitVisibleRequest(record.filePath)) {
                    seedSubmitted = true;
                    ++submitted;
                    ++listSubmitted;
                    break;
                }
            }
        }
#endif
        if (!foregroundWaveformRebuild &&
            miniDecision.allowCurrentFileFastLane &&
            !loadedWaveformPath.empty() &&
            state.waveformCache.count(loadedWaveformPath) == 0 &&
            state.waveformFailed.count(loadedWaveformPath) == 0) {
            state.visibleFilePaths.insert(loadedWaveformPath);
            if (state.waveformWorker.submitVisibleRequest(loadedWaveformPath)) {
                submitted++;
                DIAG_ACTION_FMT("Waveform", "LoadedFileWaveformFastLane: path=%s",
                                loadedWaveformPath.c_str());
            }
        }

        // Dequeue urgent visible waveforms before normal work and recheck liveness.
        std::string filePath;
        while (!foregroundWaveformRebuild &&
               submitted < submitBudget &&
               state.popNextPendingWaveform(filePath)) {
            // 1. 基础过滤：已加载或确认失败的跳过
            if (state.waveformCache.count(filePath) > 0) continue;
            if (state.waveformFailed.count(filePath) > 0) continue;

            // 2. 存活校验：如果这个路径不在本帧可见列表里，说明是滚过去的陈旧任务，直接丢弃
            // 这样能瞬间清空几万个积压请求，让真正需要显示的请求直接进入硬件
            // 全库重建时绕过此检查，否则非可见文件永远不会被提交
            if (!state.waveformRebuildActive &&
                state.visibleFilePaths.find(filePath) == state.visibleFilePaths.end()) {
                continue;
            }

            if (state.waveformWorker.submitVisibleCacheRequest(filePath)) {
                submitted++;
#if AURA_BUILD_DIAGNOSTICS && !AURA_INTERNAL_BETA
                listSubmitted++;
#endif
            }
        }

        // Consume at most N completed background results per frame.
        state.waveformWorker.setThrottleMode(throttleWaveformWorker);
        std::vector<WaveformResult> completedResults;
        const auto resultConsumeStart = std::chrono::steady_clock::now();
        int consumed = state.waveformWorker.pollResults(completedResults, AppState::WAVEFORMS_PER_FRAME);
#if AURA_BUILD_DIAGNOSTICS && !AURA_INTERNAL_BETA
        int listSuccessfulConsumed = 0;
#endif
        int diagCacheOnly = 0;
        int diagBackfill = 0;
        int diagTerminal = 0;
        int diagClear = 0;
        int diagDeferred = 0;
        int diagStaleDrop = 0;
        int diagUiDrop = 0;
        int visibleDecodePromotionsThisFrame = 0;
        const int visibleDecodePromotionBudget =
            (state.postSearchMiniWaveThrottleFrames > 0 && !scanForegroundWaveformDrainActive) ? 1 : 4;
        static std::map<std::string, int> s_waveformRetryCount;
        constexpr int kWaveformMaxRetries = 3;
        constexpr size_t kMaxWaveformRetryEntries = 5000;
        if (s_waveformRetryCount.size() > kMaxWaveformRetryEntries) {
            auto half = s_waveformRetryCount.begin();
            std::advance(half, s_waveformRetryCount.size() / 2);
            s_waveformRetryCount.erase(s_waveformRetryCount.begin(), half);
        }

        for (auto& result : completedResults) {
            bool isStillVisiblePageItem =
                state.visibleFilePaths.find(result.filePath) !=
                state.visibleFilePaths.end();
            if (!isStillVisiblePageItem) {
                // Keep the existing central-list retention rule for rows
                // that were submitted before the current frame's
                // visibility set was rebuilt (for example during a fast
                // scroll or page transition).
                for (const auto& rec : state.searchResults) {
                    if (rec.filePath == result.filePath) {
                        isStillVisiblePageItem = true;
                        break;
                    }
                }
            }
            const bool isLoadedFileResult =
                !loadedWaveformPath.empty() && result.filePath == loadedWaveformPath;
#if AURA_BUILD_DIAGNOSTICS && !AURA_INTERNAL_BETA
            const bool isCurrentListPath = isStillVisiblePageItem ||
                state.visibleFilePaths.count(result.filePath) > 0;
#endif
            if (result.hasBackfillJob) {
                ++diagBackfill;
                const bool deferredDbWrite = result.errorCode == "deferred_scan_db_write";
                if (deferredDbWrite) {
                    ++diagDeferred;
                }
                const aura::BackfillCompletionOutcome outcome = result.success
                    ? aura::BackfillCompletionOutcome::Success
                    : (deferredDbWrite
                        ? aura::BackfillCompletionOutcome::TransientFailure
                        : aura::BackfillCompletionOutcome::PermanentFailure);
                state.backfillQueue.reportOutcome(result.backfillJob, outcome);
                if (result.success ||
                    state.backfillQueue.itemState(result.backfillJob.fileId, result.backfillJob.kind) ==
                        aura::BackfillItemState::FailedPermanent) {
                    const std::string foregroundReason = result.success
                        ? "mini_cached"
                        : (result.errorCode.empty() ? "waveform_backfill_failed" : result.errorCode);
                    if (CanClearBackfillPendingForJob(
                            state,
                            result.backfillJob,
                            result.success,
                            foregroundReason)) {
                        ++diagTerminal;
                        state.database.clearBackfillPending(
                            result.backfillJob.fileId,
                            static_cast<int>(result.backfillJob.kind));
                        ++diagClear;
                    } else {
                        ++diagStaleDrop;
                    }
                }
            }
            if (result.cacheOnly) {
                ++diagCacheOnly;
                if (result.success) {
                    state.waveformFailed.erase(result.filePath);
                } else if (result.errorCode == "deferred_scan_db_write") {
                    ++diagDeferred;
                    continue;
                } else if (result.errorCode == "mini_cache_miss_no_decode") {
                    state.waveformMiniCacheMiss.erase(result.filePath);
                    state.removePendingWaveform(result.filePath);
                    if (visibleDecodePromotionsThisFrame < visibleDecodePromotionBudget &&
                        state.waveformWorker.submitVisibleRequest(result.filePath)) {
                        ++visibleDecodePromotionsThisFrame;
                        DIAG_ACTION_FMT("MiniWave",
                                        "VisibleCacheMissPromotedToDecode: path=%s",
                                        result.filePath.c_str());
                    } else {
                        state.addPendingWaveform(result.filePath, true);
                    }
                    continue;
                } else {
                    state.waveformFailed.insert(result.filePath);
                    state.database.upsertWaveformCacheStateByPath(
                        result.filePath,
                        "failed",
                        result.errorCode.empty() ? "decode_failed" : result.errorCode,
                        1,
                        "");
                }
                state.removePendingWaveform(result.filePath);
                continue;
            }
            // Drop stale/off-page list waveform results to keep cache/upload work focused
            // on current view state and reduce jitter after rapid filter/page changes.
            // 全库重建时保留所有结果，否则解码成果被白白丢弃
            if (!state.waveformRebuildActive && !isStillVisiblePageItem && !isLoadedFileResult) {
                ++diagUiDrop;
                continue;
            }

            if (result.success) {
#if AURA_BUILD_DIAGNOSTICS && !AURA_INTERNAL_BETA
                if (isCurrentListPath && !isLoadedFileResult && !result.hasBackfillJob)
                    ++listSuccessfulConsumed;
#endif
                s_waveformRetryCount.erase(result.filePath);
                state.waveformFailed.erase(result.filePath);
                state.waveformMiniCacheMiss.erase(result.filePath);
                // 使用字节预算管理（如果可用）
                if (state.memoryBudget) {
                    uint64_t waveformSizeBytes = result.waveform.peaks.size() * sizeof(float) +
                                                 result.waveform.rms.size() * sizeof(float);

                    if (!state.memoryBudget->canAdd(MemoryBudget::CacheType::Waveform, waveformSizeBytes)) {
                        uint64_t currentUsage = state.memoryBudget->getCurrentUsage(MemoryBudget::CacheType::Waveform);
                        uint64_t budget = state.memoryBudget->getBudgetConfig().waveformBudget;
                        if (currentUsage + waveformSizeBytes > budget) {
                            uint64_t requiredBytes = (currentUsage + waveformSizeBytes) - budget;
                            auto evicted = state.memoryBudget->evictLRU(MemoryBudget::CacheType::Waveform, requiredBytes);
                            for (const auto& evictEntry : evicted)
                                ReleaseWaveformEvictedEntry(state, evictEntry.key,
                                    MiniWaveformPresentation::Removal::BudgetEviction);
                        }
                    }

                    state.memoryBudget->registerEntry(MemoryBudget::CacheType::Waveform, result.filePath, waveformSizeBytes);
                    state.memoryBudget->updateUsage(MemoryBudget::CacheType::Waveform, result.filePath);
                } else {
                    if (state.waveformCache.find(result.filePath) == state.waveformCache.end() &&
                        state.waveformCache.size() >= AppState::MAX_WAVEFORM_CACHE_SIZE) {
                        DIAG_ACTION_FMT("MiniWave", "dataCacheTrim size=%zu limit=%d",
                                        state.waveformCache.size(),
                                        AppState::MAX_WAVEFORM_CACHE_SIZE);
                        state.waveformCache.erase(state.waveformCache.begin());
                    }
                }
                InvalidateMiniWaveTextureAfterFrame(result.filePath);
                state.waveformCache[result.filePath] = std::move(result.waveform);
            } else {
                int attempt = 1;
                auto itRetry = s_waveformRetryCount.find(result.filePath);
                if (itRetry == s_waveformRetryCount.end()) {
                    s_waveformRetryCount[result.filePath] = 1;
                } else {
                    itRetry->second += 1;
                    attempt = itRetry->second;
                }

                const char* reason = result.errorCode.empty() ? "unknown" : result.errorCode.c_str();
                if (result.errorCode == "mini_cache_miss_no_decode") {
                    state.waveformMiniCacheMiss.erase(result.filePath);
                    s_waveformRetryCount.erase(result.filePath);
                    state.removePendingWaveform(result.filePath);
                    if (visibleDecodePromotionsThisFrame < visibleDecodePromotionBudget &&
                        state.waveformWorker.submitVisibleRequest(result.filePath)) {
                        ++visibleDecodePromotionsThisFrame;
                        DIAG_ACTION_FMT("MiniWave",
                                        "VisibleCacheMissPromotedToDecode: path=%s",
                                        result.filePath.c_str());
                    } else {
                        state.addPendingWaveform(result.filePath, true);
                    }
                    continue;
                }
                DIAG_ACTION_FMT("Waveform", "MiniWaveformLoadFailed: path=%s attempt=%d reason=%s",
                                result.filePath.c_str(), attempt, reason);

                if (attempt < kWaveformMaxRetries) {
                    state.addPendingWaveform(result.filePath, true);
                    continue;
                }

                DIAG_ACTION_FMT("Waveform", "MiniWaveformPermanentFailure: path=%s reason=%s",
                                result.filePath.c_str(), reason);
                state.waveformFailed.insert(result.filePath);
            }
        }
#if AURA_BUILD_DIAGNOSTICS && !AURA_INTERNAL_BETA
        aura::diagnostics::FrameBudgetDiagnostics::shared().observeMiniActivity(
            listSubmitted, listSuccessfulConsumed);
#endif
        {
            static auto s_lastBackfillResultConsumeLog = std::chrono::steady_clock::now();
            static int s_consumed = 0;
            static int s_cacheOnly = 0;
            static int s_backfill = 0;
            static int s_terminal = 0;
            static int s_clear = 0;
            static int s_deferred = 0;
            static int s_staleDrop = 0;
            static int s_uiDrop = 0;
            static double s_durationMs = 0.0;
            static bool s_wasScanForegroundPending = false;
            const bool scanForegroundPending =
                state.scanCompletionPending.load(std::memory_order_acquire);
            if (scanForegroundPending || s_wasScanForegroundPending) {
                s_wasScanForegroundPending = scanForegroundPending;
                const auto resultConsumeEnd = std::chrono::steady_clock::now();
                s_consumed += consumed;
                s_cacheOnly += diagCacheOnly;
                s_backfill += diagBackfill;
                s_terminal += diagTerminal;
                s_clear += diagClear;
                s_deferred += diagDeferred;
                s_staleDrop += diagStaleDrop;
                s_uiDrop += diagUiDrop;
                s_durationMs += std::chrono::duration<double, std::milli>(
                    resultConsumeEnd - resultConsumeStart).count();

                const double elapsedSec =
                    std::chrono::duration<double>(resultConsumeEnd - s_lastBackfillResultConsumeLog).count();
                if (elapsedSec >= 1.0 || !scanForegroundPending) {
                    char detail[320];
                    std::snprintf(
                        detail,
                        sizeof(detail),
                        "BackfillResultConsume: consumed=%d cacheOnly=%d backfill=%d terminal=%d clear=%d deferred=%d staleDrop=%d uiDrop=%d durationMs=%.2f",
                        s_consumed,
                        s_cacheOnly,
                        s_backfill,
                        s_terminal,
                        s_clear,
                        s_deferred,
                        s_staleDrop,
                        s_uiDrop,
                        s_durationMs);
                    DIAG_SYSTEM("Waveform", detail);
                    s_lastBackfillResultConsumeLog = resultConsumeEnd;
                    s_consumed = 0;
                    s_cacheOnly = 0;
                    s_backfill = 0;
                    s_terminal = 0;
                    s_clear = 0;
                    s_deferred = 0;
                    s_staleDrop = 0;
                    s_uiDrop = 0;
                    s_durationMs = 0.0;
                    s_wasScanForegroundPending = scanForegroundPending;
                }
            }
        }
        if (state.postSearchMiniWaveThrottleFrames > 0) {
            --state.postSearchMiniWaveThrottleFrames;
        }
    }

    return seedSubmitted;
}

void PumpAppCoverTextureUploads(AppState& state, bool contextMenuHeavyFreeze, bool isMouseIdle) {
    {
        static bool s_lastDetailPending = false;
        static double s_lastDetailCompletedTime = 0.0;
        const bool detailPending = state.audioEngine.isWaveformDetailPending();
        if (s_lastDetailPending && !detailPending)
            s_lastDetailCompletedTime = ImGui::GetTime();
        s_lastDetailPending = detailPending;

        BackgroundIoContext bgIo = BuildAppBackgroundIoContext(
            state, contextMenuHeavyFreeze, IsCoverWallPriorityMode(), isMouseIdle);
        bgIo.detailJustCompleted = s_lastDetailCompletedTime > 0.0 &&
            (ImGui::GetTime() - s_lastDetailCompletedTime) < 0.4;
        const auto coverBudget = ResolveCoverWallBackgroundBudget(bgIo);
#if AURA_BUILD_DIAGNOSTICS
        if (coverBudget.maxTextureUploads == 0 && coverBudget.budgetMicroseconds == 0 &&
            std::string(coverBudget.reason) == "foreground") {
            static double s_lastCoverPauseLogAt = 0.0;
            const double now = ImGui::GetTime();
            if ((now - s_lastCoverPauseLogAt) >= 1.0) {
                s_lastCoverPauseLogAt = now;
                DIAG_ACTION_FMT("CoverWall", "CoverWallPausedByForeground: reason=%s", coverBudget.reason);
            }
        }
#endif
        CoverModule::PumpCoverTextureUploads(coverBudget.maxTextureUploads,
                                             coverBudget.budgetMicroseconds);
    }
}

void PumpAppBackgroundStatus(AppState& state, bool isMouseIdle) {
    // Track mouse movement
    static ImVec2 lastMousePos = ImGui::GetMousePos();
    ImVec2 currentMousePos = ImGui::GetMousePos();
    bool mouseMoved = (fabsf(currentMousePos.x - lastMousePos.x) > 2.0f ||
                      fabsf(currentMousePos.y - lastMousePos.y) > 2.0f);
    if (mouseMoved) {
        state.lastMouseMoveTime = std::chrono::steady_clock::now();
        lastMousePos = currentMousePos;
    }

    // 🚀 现代版：静默后台管理器 (仅保留 UI 状态，不执行全扫)

    // 交互优先：主波形正在解码时，不驱动后台 worker，把 I/O 让给大波形

    {
        bool isMainDecoding = state.audioEngine.isWaveformDetailPending();
        const bool contextMenuHeavyFreezeForBgStatus =
            ImGui::IsPopupOpen("RowContextMenu") ||
            ImGui::IsPopupOpen("##PlaylistSidebarContext");
        BackgroundIoContext bgStatusIo = BuildAppBackgroundIoContext(
            state, contextMenuHeavyFreezeForBgStatus, IsCoverWallPriorityMode(), isMouseIdle);
        const auto miniStatus = ResolveMiniWaveformWorkerDecision(bgStatusIo);
        if (state.waveformWorker.hasPendingRequests() && !isMainDecoding && !miniStatus.throttleWorker) {
            state.bgCacheRunning = true;
            state.bgCacheProgress = state.waveformWorker.pendingCount();
            state.bgCacheTotal = (int)state.searchResults.size();
        } else {
            state.bgCacheRunning = false;
        }
    }

}
