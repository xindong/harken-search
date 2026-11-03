#include "AppRuntime.h"
#include "AppState.h"
#include "AppMainServices.h"
#include "WindowsMain.h"
#include "ThemeSchedule.h"
#include "DiagnosticsLogger.h"
#include "DevToolsModule.h"
#include "PlaybackHistoryStateAdapter.h"
#include "RelinkWorkflow.h"
#include "BackfillWorkerPool.h"
#include "CoverModule.h"
#include "Utils.h"
#include "PathUtils.h"
#include <filesystem>
namespace fs = std::filesystem;
std::string ResolveCanonicalHarkenSearchDatabasePathUtf8(const std::string&);
#include "platform/system.h"
#include "platform/filesystem.h"
#include "imgui.h"
extern std::string g_appSettingsPathUtf8;

bool InitializeAppSettingsAndAudio(AppState& state) {
    AppendDiagLogLine(L"STARTUP: before LoadAudioSettingsEarly");
    LoadAudioSettingsEarly(state);
    AppendDiagLogLine(L"STARTUP: after LoadAudioSettingsEarly");
    AppendDiagLogLine(L"STARTUP: before audioEngine.initialize");
    if (!state.audioEngine.initialize(state.settings.audioPeriodSizeMs)) {
        AppendDiagLogLine(L"STARTUP: audioEngine.initialize failed");
        return false;
    }
    AppendDiagLogLine(L"STARTUP: after audioEngine.initialize");
    AppendDiagLogLine(L"STARTUP: before LoadAppSettings");
    LoadAppSettings(state);
    harken::theme_schedule::Initialize(g_appSettingsPathUtf8);
    AppendDiagLogLine(L"STARTUP: after LoadAppSettings");

    return true;
}

void PumpAppDatabaseAndAudio(AppState& state) {
    // Deferred search refresh after DB switch — runs one frame after
    // ResetDatabaseState so the synchronous SQL query doesn't stack
    // onto the same frame. Must be checked BEFORE Pump so that the
    // flag set by Pump on frame N is consumed on frame N+1.
    DevTools_BeginFramePhase("PreRender.DbSwitchRefresh");
    if (state.dbSwitchNeedsRefresh) {
        state.dbSwitchNeedsRefresh = false;
        RefreshSearchResults(state, true);
    }
    DevTools_EndFramePhase();
    // 异步切库：轮询后台 open 结果，避免大库 open 卡死主线程
    DevTools_BeginFramePhase("PreRender.AsyncPumps");
    PumpDatabaseSwitchIfPending(state);
    PumpInitRandomSamplingPoolIfPending(state);

    // Complete pending audio loads before dependent UI updates.
    if (state.audioEngine.pumpLoadFileIfPending()) {
        state.lastSelectionOrLoadTime = ImGui::GetTime();  // 切歌完成，封面墙预算短时降级
        // 新文件加载成功，清除旧的错误提示
        state.loadErrorToast.clear();
        state.loadErrorToastUntil = 0.0;
        ScheduleDeferredWaveformDetail(state, state.audioEngine.getLoadedFilePath(), AppState::WAVEFORM_DEFAULT_DETAIL_PEAKS);
        // 如果有之前在 Mini 波形上预设的跳转点，现在执行它！
        OnAudioLoadPumped(state);
        // Start playback AFTER silence-skip so the sound begins at the
        // correct position — avoids a visible waveform cursor jump.
        state.audioEngine.playIfLoadReady();
        static aura::playback::PlayStatsGate s_playStatsGate;
        static std::uint64_t s_playStatsLoadToken = 0;
        aura::playback::RecordSuccessfulPlayStats(
            state.database,
            s_playStatsGate,
            aura::playback::PlayStatsEvent{
                true,
                state.audioEngine.isPlaying(),
                state.currentFileId,
                ++s_playStatsLoadToken});
    } else {
        std::string loadErr = state.audioEngine.getLastLoadError();
        if (!loadErr.empty()) {
            const std::string failedPath = state.pendingAudioTargetPath;
            state.clearPendingAudioTarget();
            const bool missingOnDisk = !failedPath.empty() &&
                Platform::FileSystem::ProbePath(failedPath) ==
                    Platform::FileSystem::PathStatus::Missing;
            if (missingOnDisk) {
                AudioRecord missingRecord;
                missingRecord.filePath = failedPath;
                missingRecord.fileName = PathToUtf8(Utf8ToPath(failedPath).filename());
                for (const auto& candidate : state.searchResults) {
                    if (candidate.filePath == failedPath) {
                        missingRecord = candidate;
                        break;
                    }
                }
                aura::ui::EvaluateRelinkForRecord(state, missingRecord);
            } else {
                state.loadErrorToast = std::move(loadErr);
                state.loadErrorToastUntil = ImGui::GetTime() + 5.0;
            }
        }
    }
    // Pump detail requests outside RenderUI so WebUI mode follows the same path.
    PumpDeferredWaveformDetail(state);
    // 跳过开头无声：加载完成时波形常未就绪，每帧尝试直至主波形或 mini 缓存有数据再 seek
    MaybeSkipLeadingSilence(state);
    DevTools_EndFramePhase();
}

void StopAppRuntimeWorkers(AppState& state, ShutdownStageLogger logShutdownStage) {
    logShutdownStage("BatchFeatureThreadJoinBegin");
    state.batchFeatureCancelRequested.store(true);
    logShutdownStage("DatabaseSwitchThreadJoinBegin");
    WaitForDatabaseSwitchThread();
    logShutdownStage("DatabaseSwitchThreadJoinEnd");
    if (state.batchFeatureThread.joinable()) {
        state.batchFeatureThread.join();
    }
    logShutdownStage("BatchFeatureThreadJoinEnd");
    state.requestCancelScanTask();
    logShutdownStage("ScanThreadJoinBegin");
    if (state.scanThread.joinable()) {
        state.scanThread.join();
    }
    logShutdownStage("ScanThreadJoinEnd");
    logShutdownStage("RetiredScanThreadsJoinBegin");
    state.joinRetiredScanThreads();
    logShutdownStage("RetiredScanThreadsJoinEnd");
    logShutdownStage("DawSendThreadJoinBegin");
    if (state.dawSendThread.joinable()) {
        state.dawSendThread.join();
    }
    logShutdownStage("DawSendThreadJoinEnd");

    // Stop worker threads before releasing shared state.
    ShutdownSelectedWaveformRebuild();
    logShutdownStage("WaveformWorkerStopBegin");
    state.waveformWorker.stop();
    logShutdownStage("WaveformWorkerStopEnd");
    logShutdownStage("SearchWorkerStopBegin");
    state.searchWorker.stop();
    logShutdownStage("SearchWorkerStopEnd");
    logShutdownStage("BackfillWorkerPoolStopBegin");
    aura::GlobalBackfillWorkerPool().stop();
    logShutdownStage("BackfillWorkerPoolStopEnd");
    logShutdownStage("FileSystemWatcherStopBegin");
    state.database.stopFileSystemWatcher();
    logShutdownStage("FileSystemWatcherStopEnd");
}

void StopAppRelinkWorkers(AppState& state, ShutdownStageLogger logShutdownStage) {
    logShutdownStage("RelinkWorkflowShutdownBegin");
    aura::ui::ShutdownRelinkWorkflow(state);
    auto JoinRelinkThreadWithTimeout = [](std::thread& worker) {
        if (!worker.joinable()) {
            return;
        }
#ifdef _WIN32
        void* handle = reinterpret_cast<void*>(worker.native_handle());
        if (Platform::WaitForNativeThreadHandle(
                handle, static_cast<uint32_t>(aura::ui::kRelinkShutdownJoinTimeoutMs))) {
            worker.join();
            return;
        }
        worker.detach();
#else
        worker.join();
#endif
    };
    JoinRelinkThreadWithTimeout(state.relinkBannerSearchThread);
    JoinRelinkThreadWithTimeout(state.relinkToolSearchThread);
    for (auto& worker : state.retiredRelinkSearchThreads) {
        JoinRelinkThreadWithTimeout(worker);
    }
    state.retiredRelinkSearchThreads.clear();
    logShutdownStage("RelinkWorkflowShutdownEnd");

}

bool CloseAppRuntimeResources(AppState& state, ShutdownStageLogger logShutdownStage) {
    logShutdownStage("BackfillQueueClosingBegin");
    state.backfillQueue.onDbClosing(state.backfillDbGeneration);
    logShutdownStage("BackfillQueueClosingEnd");
    logShutdownStage("DatabaseCloseBegin");
    state.database.close();
    logShutdownStage("DatabaseCloseEnd");
    logShutdownStage("AudioEngineShutdownBegin");
    state.audioEngine.shutdown();
    logShutdownStage("AudioEngineShutdownEnd");

    logShutdownStage("CoverCacheShutdownBegin");
    CoverModule::ShutdownCoverWallPrecache();
    CoverModule::ClearAllCoverCaches();
    logShutdownStage("CoverCacheShutdownEnd");

    logShutdownStage("SettingsSaveBegin");
    const bool saved = TrySaveAppSettings(state);
    logShutdownStage(saved ? "SettingsSaveEnd" : "SettingsSaveFailed");
    return saved;

}

bool ResolveStartupDatabaseSelectionForMount(
    const AppState& state,
    std::string& outDbPathUtf8,
    AppState::StartupDatabaseSetupReason& outReason) {
    outDbPathUtf8.clear();
    outReason = AppState::StartupDatabaseSetupReason::None;

    const std::string currentDbPathUtf8 =
        ResolveCanonicalHarkenSearchDatabasePathUtf8(state.currentDbPathUtf8);
    if (IsDatabaseLibrarySelectable(
            ClassifyDatabaseLibraryPath(currentDbPathUtf8))) {
        outDbPathUtf8 = currentDbPathUtf8;
        return true;
    }

    for (const auto& recentPathRaw : state.recentDbPathsUtf8) {
        const std::string recentDbPathUtf8 =
            ResolveCanonicalHarkenSearchDatabasePathUtf8(recentPathRaw);
        if (IsDatabaseLibrarySelectable(
                ClassifyDatabaseLibraryPath(recentDbPathUtf8))) {
            outDbPathUtf8 = recentDbPathUtf8;
            return true;
        }
    }

    const bool hasKnownDatabasePath =
        !currentDbPathUtf8.empty() || !state.recentDbPathsUtf8.empty();
    outReason = hasKnownDatabasePath
        ? AppState::StartupDatabaseSetupReason::MissingConfiguredDatabase
        : AppState::StartupDatabaseSetupReason::FirstRunNoDatabase;
    return false;
}

void PumpAppScanRefresh(AppState& state, bool scanPopupOpen) {

    // Check if database needs refresh during scan (渐进式更新)
    DevTools_BeginFramePhase("PreRender.ScanRefresh");
    if (state.scanDataCommitted.load()) {
        state.scanDataCommitted = false;

        // During scan, refresh list incrementally to show newly committed data.
        if (state.viewMode == ViewMode::Database && state.isScanning.load() && !scanPopupOpen) {
            // 如果有活跃的搜索/筛选条件，不覆盖 totalFileCount
            // 避免扫描期间覆盖用户的搜索结果总数（由 RefreshSearchResults 正确计算）
            // 性能：getFileCount() 大库时耗时可观，节流为每 2 秒最多一次（规范 §11）
            bool hasActiveFilters = (state.searchBuffer[0] != '\0') ||
                state.folderFilterActive || state.coverFilterActive;
            if (!hasActiveFilters) {
                static double s_lastScanGetFileCountTime = 0.0;
                double nowSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
                if (s_lastScanGetFileCountTime == 0.0 || nowSeconds - s_lastScanGetFileCountTime >= 2.0) {
                    state.totalFileCount = state.database.getFileCount();
                    s_lastScanGetFileCountTime = nowSeconds;
                }
            }

            // 节流阀 (Throttling) - 防止刷新过快导致闪烁
            // Heavy list refresh is throttled.
            const double REFRESH_THROTTLE_INTERVAL = 1.0;
            auto now = std::chrono::steady_clock::now();
            auto nowSeconds = std::chrono::duration<double>(now.time_since_epoch()).count();
            double timeSinceLastRefresh = (state.lastIncrementalRefreshTime == 0.0) ?
                REFRESH_THROTTLE_INTERVAL : (nowSeconds - state.lastIncrementalRefreshTime);

            // 如果当前在第一页（显示前PAGE_SIZE个结果），刷新以显示新添加的文件
            // 这样用户可以看到列表在不断填充，体验更好
            if ((state.loadedCount <= AppState::PAGE_SIZE || state.loadedCount == 0) &&
                timeSinceLastRefresh >= REFRESH_THROTTLE_INTERVAL) {
                // Refresh the first page so the data source includes the new file; ImGui updates visible rows itself.
                RefreshSearchResults(state, false); // keep pagination state, refresh data source
                state.lastIncrementalRefreshTime = nowSeconds;
            }
            // 如果用户不在第一页，只更新总数，不刷新列表（避免丢失滚动位置）
            // 用户可以通过滚动到顶部或等待扫描完成来查看新数据

            // 同步到当前标签页
            if (!state.listTabs.empty()) {
                state.currentTab().searchResults = state.searchResults;
                state.currentTab().totalFileCount = state.totalFileCount;
                state.currentTab().loadedCount = state.loadedCount;
                state.currentTab().hasMoreResults = state.hasMoreResults;
            }
        }
    }
    DevTools_EndFramePhase();

    // Refresh database-backed rows after scanning completes.
    DevTools_BeginFramePhase("PreRender.DatabaseRefresh");
    if (state.needsDatabaseRefresh && !scanPopupOpen) {
        state.needsDatabaseRefresh = false;
        state.invalidateCoverWallAfterDatabaseContentChange();

        if (state.scanThread.joinable() && !state.isScanning.load(std::memory_order_acquire)) {
            state.scanThread.join();
        }

        // scanDirectoryEx commits before returning, so no second commit is needed here.
        // Rebuild folder_index (write op), then use a fresh read-only
        // connection via deferred bootstrap to avoid stale WAL snapshot
        // missing library roots written by the scan thread.
        state.database.rebuildFolderIndex();
        StartDeferredSidebarBootstrap(state, true, false);

        // Auto-filter to scanned path and load first page.
        if (state.autoFilterToScannedPath && !state.lastScannedPath.empty()) {
            state.autoFilterToScannedPath = false;
            // Scan auto-filter paths are canonical in both fallback branches.
            try {
                fs::path p = Utf8ToPath(state.lastScannedPath);
                state.selectedFolderPath = HarkenSearch::CanonicalFolderPath(PathToUtf8(p));
            } catch (...) {
                state.selectedFolderPath = HarkenSearch::CanonicalFolderPath(state.lastScannedPath);
            }
            state.folderFilterActive = true;
        }

        // QA/UI scan completion is a database-content transition. Normalize
        // back to Database before refreshing so a scan started from another
        // view cannot leave the DB populated while the visible list reports
        // zero rows.
        state.viewMode = ViewMode::Database;

        // Reset pagination and query the database; repaint alone would retain stale rows.
        state.searchResults.clear();
        state.bumpSearchResultsGen();
        state.loadedCount = 0;
        RefreshSearchResults(state, true); // true = 重置分页并重新查询数据库
        if (state.scanTotal.load() > 0 &&
            !state.isScanning.load(std::memory_order_acquire)) {
            state.dbSwitchSettleFramesRemaining =
                (std::max)(state.dbSwitchSettleFramesRemaining, 120);
        }

        // 同步到当前标签页
        if (!state.listTabs.empty()) {
            state.currentTab().searchResults = state.searchResults;
            state.currentTab().totalFileCount = state.totalFileCount;
            state.currentTab().loadedCount = state.loadedCount;
            state.currentTab().hasMoreResults = state.hasMoreResults;
            state.currentTab().selectedFolderPath = state.selectedFolderPath;
            state.currentTab().folderFilterActive = state.folderFilterActive;
        }
    }
    DevTools_EndFramePhase();

}
