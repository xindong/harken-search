#include "BackgroundIoPolicy.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

namespace {

int g_failures = 0;

void Check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        ++g_failures;
    } else {
        std::cout << "PASS: " << message << "\n";
    }
}

std::string ReadTextFile(const std::filesystem::path& path) {
    std::ifstream input(path);
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

std::string ExtractFunctionBody(const std::string& source, const std::string& signature) {
    size_t sigPos = source.find(signature);
    while (sigPos != std::string::npos) {
        const size_t openPosCandidate = source.find('{', sigPos);
        if (openPosCandidate == std::string::npos) return {};
        const size_t semicolonCandidate = source.find(';', sigPos);
        if (semicolonCandidate == std::string::npos || openPosCandidate < semicolonCandidate) {
            const size_t openPos = openPosCandidate;

            int depth = 0;
            for (size_t i = openPos; i < source.size(); ++i) {
                if (source[i] == '{') {
                    ++depth;
                } else if (source[i] == '}') {
                    --depth;
                    if (depth == 0) {
                        return source.substr(openPos, i - openPos + 1);
                    }
                }
            }
            return {};
        }
        sigPos = source.find(signature, semicolonCandidate + 1);
    }
    return {};
}

std::string ExtractElseIfCommandBody(const std::string& source, const std::string& commandName) {
    const std::string signature = "else if (cmd == \"" + commandName + "\")";
    return ExtractFunctionBody(source, signature);
}

std::string ExtractLambdaBody(const std::string& source, const std::string& signature) {
    return ExtractFunctionBody(source, signature);
}

BackgroundIoContext IdleContext() {
    BackgroundIoContext ctx;
    ctx.mouseIdle = true;
    ctx.secondsSinceSelectionOrLoad = 5.0;
    return ctx;
}

void TestForegroundPlaybackPausesUnrelatedBackgroundIo() {
    auto ctx = IdleContext();
    ctx.playing = true;
    ctx.secondsSinceSelectionOrLoad = 0.10;

    const auto cover = ResolveCoverWallBackgroundBudget(ctx);
    const auto decoder = ResolveDecoderPrewarmDecision(ctx);
    const auto mini = ResolveMiniWaveformWorkerDecision(ctx);

    Check(cover.allowBackgroundRequests,
          "BDD: Given playback just started, visible/current cover lane stays alive");
    Check(cover.maxTextureUploads == 1,
          "BDD: Given playback just started, cover uploads are limited to one visible/current item");
    Check(cover.maxRequestsPerFrame == 1,
          "BDD: Given playback just started, cover requests are limited to one visible/current item");
    Check(cover.requestVisibleOnly,
          "BDD: Given playback just started, cover wall may only request visible/current covers");
    Check(!decoder.allowed,
          "BDD: Given playback just started, unrelated decoder prewarm is skipped");
    Check(mini.cancelQueuedListWork,
          "BDD: Given playback just started, queued list mini-waveforms are cancelled");
    Check(mini.submitListBudget == 1,
          "BDD: Given playback just started, one visible mini-waveform request may progress per frame");
    Check(mini.allowCurrentFileFastLane,
          "BDD: Given playback just started, current file mini-waveform fast lane stays allowed");
}

void TestBrowserMiniWaveformWorkerDoesNotRequireOpenDatabaseContract() {
    const auto sourcePath = std::filesystem::path(__FILE__).parent_path().parent_path() /
                            "src" / "WindowsMain.cpp";
    const std::string source = ReadTextFile(sourcePath);

    Check(!source.empty(),
          "BDD: Given Browser mini-worker startup contract validation runs, WindowsMain.cpp can be opened");
    Check(source.find("static bool waveformWorkerStarted = false;") != std::string::npos &&
          source.find("!waveformWorkerStarted && !holdColdMiniWorker") != std::string::npos,
          "BDD: Given direct-file Browser has no open database, its source-decode waveform worker still starts");
    Check(source.find("static bool searchWorkerStarted = false;") != std::string::npos &&
          source.find("!searchWorkerStarted && state.database.isOpen()") != std::string::npos,
          "BDD: Search worker keeps its database-open gate independently of Browser mini-waveforms");
    Check(source.find("!wfWorkerStarted && state.database.isOpen() && !holdColdMiniWorker") ==
              std::string::npos,
          "BDD: Browser waveform startup is no longer coupled to the search database gate");
}

void TestWaveformRebuildPausesAllBackgroundIo() {
    auto ctx = IdleContext();
    ctx.waveformRebuildActive = true;

    const auto cover = ResolveCoverWallBackgroundBudget(ctx);
    const auto decoder = ResolveDecoderPrewarmDecision(ctx);
    const auto mini = ResolveMiniWaveformWorkerDecision(ctx);

    Check(cover.maxTextureUploads == 0,
          "BDD: Given waveform rebuild is active, cover background work is paused");
    Check(!cover.allowBackgroundRequests,
          "BDD: Given waveform rebuild is active, cover wall cannot enqueue background requests");
    Check(!decoder.allowed,
          "BDD: Given waveform rebuild is active, decoder prewarm is skipped");
    Check(mini.submitListBudget == 0,
          "BDD: Given waveform rebuild is active, mini-waveform list submission is paused");
}

void TestIdleAllowsBoundedBackgroundIo() {
    auto ctx = IdleContext();

    const auto cover = ResolveCoverWallBackgroundBudget(ctx);
    const auto decoder = ResolveDecoderPrewarmDecision(ctx);
    const auto mini = ResolveMiniWaveformWorkerDecision(ctx);

    Check(cover.allowBackgroundRequests,
          "BDD: Given app is idle, cover wall can progress in background");
    Check(cover.maxTextureUploads > 0,
          "BDD: Given app is idle, cover texture uploads have a positive bounded budget");
    Check(cover.requestVisibleOnly,
          "BDD: Given app is idle, cover wall still limits work to visible items by default");
    Check(decoder.allowed,
          "BDD: Given app is idle after user browse context exists, decoder prewarm can run");
    Check(decoder.lookahead > 0 && decoder.lookahead <= 3,
          "BDD: Given decoder prewarm runs, lookahead is bounded to avoid disk sweep");
    Check(mini.submitListBudget > 0,
          "BDD: Given app is idle, mini-waveform list preloading can progress");
}

void TestInteractionShrinksButDoesNotKillVisibleCoverBudget() {
    auto idle = IdleContext();
    auto interacting = IdleContext();
    interacting.mouseIdle = false;

    const auto idleCover = ResolveCoverWallBackgroundBudget(idle);
    const auto interactingCover = ResolveCoverWallBackgroundBudget(interacting);

    Check(interactingCover.maxTextureUploads < idleCover.maxTextureUploads,
          "BDD: Given user is interacting, cover upload budget is lower than idle");
    Check(interactingCover.allowBackgroundRequests,
          "BDD: Given user is interacting, visible cover requests still progress");
    Check(interactingCover.maxTextureUploads > 0,
          "BDD: Given user is interacting, visible cover uploads are not starved");
    Check(interactingCover.requestVisibleOnly,
          "BDD: Given user is interacting, cover wall requests stay near visible range");
}

void TestIdleCoverWallPriorityAllowsOffscreenFill() {
    auto ctx = IdleContext();
    const auto normalCover = ResolveCoverWallBackgroundBudget(ctx);
    ctx.coverWallPriorityMode = true;

    const auto cover = ResolveCoverWallBackgroundBudget(ctx);

    Check(cover.allowBackgroundRequests,
          "BDD: Given cover wall is open and idle, cover requests can progress");
    Check(cover.maxTextureUploads >= 8,
          "BDD: Given cover wall is open and idle, upload budget is high enough to make real prebake progress beyond the viewport");
    Check(cover.budgetMicroseconds >= 1800,
          "BDD: Given cover wall is open and idle, upload time budget can drain decoded cover debt without touching active interaction");
    Check(cover.maxRequestsPerFrame >= 24,
          "BDD: Given cover wall is open and idle, request budget is high enough to crawl offscreen covers");
    Check(cover.maxRequestsPerFrame >= normalCover.maxRequestsPerFrame * 4,
          "BDD: Given cover wall is open and idle, request budget is not stuck at normal list-prefetch trickle speed");
    Check(!cover.requestVisibleOnly,
          "BDD: Given cover wall is open and idle, background progress mode is enabled once the visible band is satisfied");
}

void TestCoverWallWarmupRaisesOffscreenFillBudget() {
    auto idle = IdleContext();
    idle.coverWallPriorityMode = true;
    auto warmup = idle;
    warmup.coverWarmup = true;

    const auto idleCover = ResolveCoverWallBackgroundBudget(idle);
    const auto warmupCover = ResolveCoverWallBackgroundBudget(warmup);

    Check(warmupCover.maxTextureUploads > 0,
          "BDD: Given cover wall warmup, visible cover uploads still progress");
    Check(warmupCover.maxTextureUploads < idleCover.maxTextureUploads,
          "BDD: Given cover wall warmup, upload budget is lower than steady idle offscreen crawl");
    Check(warmupCover.maxRequestsPerFrame > 0,
          "BDD: Given cover wall warmup, visible cover requests still progress");
    Check(warmupCover.maxRequestsPerFrame < idleCover.maxRequestsPerFrame,
          "BDD: Given cover wall warmup, request budget is lower than steady idle offscreen crawl");
    Check(warmupCover.requestVisibleOnly,
          "BDD: Given cover wall warmup and idle state, startup stays in visible-only mode");
}

void TestCoverWallDragKeepsVisibleCoverLaneAlive() {
    auto ctx = IdleContext();
    ctx.coverWallPriorityMode = true;
    ctx.userActive = true;
    ctx.mouseIdle = false;

    const auto cover = ResolveCoverWallBackgroundBudget(ctx);

    Check(cover.allowBackgroundRequests,
          "BDD: Given cover wall scrollbar is being dragged, visible cover requests still progress");
    Check(cover.maxTextureUploads == 1,
          "BDD: Given cover wall scrollbar is being dragged, uploads are capped to one visible cover");
    Check(cover.maxRequestsPerFrame == 1,
          "BDD: Given cover wall scrollbar is being dragged, requests are capped to one visible cover");
    Check(cover.requestVisibleOnly,
          "BDD: Given cover wall scrollbar is being dragged, offscreen cover crawling remains disabled");
}

void TestSelectionCooldownKeepsVisibleCoverLaneAlive() {
    auto ctx = IdleContext();
    ctx.secondsSinceSelectionOrLoad = 0.10;

    const auto cover = ResolveCoverWallBackgroundBudget(ctx);

    Check(cover.allowBackgroundRequests,
          "BDD: Given selection just changed, visible cover requests still progress");
    Check(cover.maxTextureUploads == 1,
          "BDD: Given selection just changed, uploads are capped to one visible cover");
    Check(cover.requestVisibleOnly,
          "BDD: Given selection just changed, only visible/current covers may be requested");
}

void TestFirstScreenCursorReArmsAtRestWhenVisibleCellsRemain() {
    // At rest (no scroll), the first-screen window did not change. But if the
    // sweep cursor already reached the end of the visible window AND there are
    // still uncached/eligible visible cells (the user's "black grid won't
    // disappear unless I drag" symptom), the cursor must re-arm so the current
    // viewport keeps progressing without requiring a scroll gesture.
    Check(ShouldRearmFirstScreenCursor(/*windowChanged=*/false,
                                       /*cursorReachedEnd=*/true,
                                       /*hasEligibleVisibleCells=*/true,
                                       /*paused=*/false),
          "BDD: Given viewport unchanged but visible cells still uncached, "
          "first-screen cursor re-arms so covers fill at rest without dragging");

    Check(ShouldRearmFirstScreenCursor(/*windowChanged=*/true,
                                       /*cursorReachedEnd=*/false,
                                       /*hasEligibleVisibleCells=*/false,
                                       /*paused=*/false),
          "BDD: Given the viewport scrolled to a new window, the cursor re-arms");

    Check(!ShouldRearmFirstScreenCursor(/*windowChanged=*/false,
                                        /*cursorReachedEnd=*/true,
                                        /*hasEligibleVisibleCells=*/false,
                                        /*paused=*/false),
          "BDD: Given all visible cells are already cached, the cursor does not "
          "re-arm (no wasted re-sweep at rest)");

    Check(!ShouldRearmFirstScreenCursor(/*windowChanged=*/false,
                                        /*cursorReachedEnd=*/false,
                                        /*hasEligibleVisibleCells=*/true,
                                        /*paused=*/false),
          "BDD: Given the cursor is still mid-sweep, it keeps sweeping rather "
          "than re-arming to the window start");

    Check(!ShouldRearmFirstScreenCursor(/*windowChanged=*/false,
                                        /*cursorReachedEnd=*/true,
                                        /*hasEligibleVisibleCells=*/true,
                                        /*paused=*/true),
          "BDD: Given background IO is paused for a foreground-critical job, the "
          "cursor does not re-arm");
}

void TestCoverWallFirstScreenPrefetchIsNotGatedByOffscreenCrawlSourceContract() {
    const auto sourcePath = std::filesystem::path(__FILE__).parent_path().parent_path() / "src" / "main.cpp";
    std::ifstream input(sourcePath);
    Check(input.good(),
          "BDD: Given source contract validation runs, src/main.cpp can be opened");
    if (!input.good()) {
        return;
    }

    std::ostringstream buffer;
    buffer << input.rdbuf();
    const std::string source = buffer.str();

    const auto firstScreenGate = source.find("const bool canRequestFirstScreenCovers");
    const auto renderGate = source.find("if (canRequestFirstScreenCovers)");
    const auto offscreenGate = source.find("const bool canRequestProgressiveOffscreenCovers");
    const auto progressiveGate = source.find("} else if (canRequestProgressiveOffscreenCovers)");

    Check(firstScreenGate != std::string::npos,
          "BDD: Given cover wall renders, first-screen prefetch has its own gate");
    Check(renderGate != std::string::npos && firstScreenGate < renderGate,
          "BDD: Given visible-only mode, first-screen covers can still request textures");
    Check(offscreenGate == std::string::npos && progressiveGate == std::string::npos,
          "BDD: Given strict visible-band virtualization, wide offscreen crawl branches are absent from the cover-wall hot path");
}

void TestCoverWallTextureMirrorClearedAtInvalidationNotValidatedEveryFrameContract() {
    const auto sourcePath = std::filesystem::path(__FILE__).parent_path().parent_path() / "src" / "main.cpp";
    const auto appStatePath = std::filesystem::path(__FILE__).parent_path().parent_path() / "src" / "AppState.h";
    std::ifstream input(sourcePath);
    std::ifstream appStateInput(appStatePath);
    Check(input.good(),
          "BDD: Given cover-wall texture mirror performance contract validation runs, src/main.cpp can be opened");
    Check(appStateInput.good(),
          "BDD: Given cover-wall texture mirror performance contract validation runs, AppState.h can be opened");
    if (!input.good() || !appStateInput.good()) {
        return;
    }

    std::ostringstream buffer;
    std::ostringstream appStateBuffer;
    buffer << input.rdbuf();
    appStateBuffer << appStateInput.rdbuf();
    const std::string source = buffer.str();
    const std::string appStateSource = appStateBuffer.str();

    const std::string invalidationBody = ExtractFunctionBody(
        appStateSource, "void invalidateAfterDatabaseContentChange()");
    const std::string renderBody = ExtractFunctionBody(
        source, "void RenderFileInfoPanelTabContents(");
    const std::string mirrorHelperBody = ExtractFunctionBody(
        source, "auto coverWallHasCurrentTextureMirror = [&](const CoverModule::CoverItem& item) -> bool");

    Check(!invalidationBody.empty(),
          "BDD: Cover-wall database invalidation body is inspectable");
    Check(!renderBody.empty(),
          "BDD: Cover-wall render body is inspectable");
    Check(!mirrorHelperBody.empty(),
          "BDD: Cover-wall local texture mirror helper is inspectable");

    Check(invalidationBody.find("texturesPendingClear = true;") != std::string::npos,
          "BDD: Database-content invalidation clears non-owning texture-id mirrors at the reset boundary");
    Check(renderBody.find("CoverModule::IsCoverTextureHandleCurrent") == std::string::npos,
          "BDD: Cover-wall hot path does not lock global cover caches to validate every cached cell each frame");
    Check(mirrorHelperBody.find("cw.texCache.find(item.key)") != std::string::npos &&
          mirrorHelperBody.find("CoverModule::IsCoverTextureHandleCurrent") == std::string::npos,
          "BDD: Cover-wall mirror hit detection remains local/O(1) so first-screen fill throughput is preserved");
    Check(source.find("TextureMirrorClearedAfterReset") != std::string::npos &&
          source.find("cw.texCache.clear();") != std::string::npos,
          "BDD: Pending texture mirror clears are still consumed on the GL/render thread");
}

void TestDisplayReloadDoesNotRebuildGlobalCoverWallContract() {
    Check(!ShouldDropCoverWallLoadedForDisplayReload(/*useFacetCoverWall=*/false),
          "BDD: Given a display reload in global cover-wall mode, the loaded wall is preserved");
    Check(!ShouldDropCoverWallLoadedForDisplayReload(/*useFacetCoverWall=*/true),
          "BDD: Given a display reload in filtered/facet cover-wall mode, the stable wall is preserved until authoritative facets arrive");

    const auto sourcePath = std::filesystem::path(__FILE__).parent_path().parent_path() / "src" / "main.cpp";
    std::ifstream input(sourcePath);
    Check(input.good(),
          "BDD: Given display-reload contract validation runs, src/main.cpp can be opened");
    if (!input.good()) {
        return;
    }

    std::ostringstream buffer;
    buffer << input.rdbuf();
    const std::string source = buffer.str();

    const auto displayRefresh = source.find("const bool coverWallDisplayRefresh");
    const auto dropPolicy = source.find("ShouldDropCoverWallLoadedForDisplayReload(useFacetCoverWall)");
    const auto dropLoaded = source.find("cw.loaded = false", dropPolicy);

    Check(dropPolicy != std::string::npos,
          "BDD: Cover wall display reload is routed through a policy helper");
    Check(displayRefresh != std::string::npos && dropPolicy != std::string::npos && displayRefresh < dropPolicy,
          "BDD: Display reload policy is applied while consuming display-only cover-wall refresh");
    Check(dropLoaded != std::string::npos && dropPolicy != std::string::npos && dropPolicy < dropLoaded,
          "BDD: Cover wall only drops loaded state after the display-reload policy allows it");
    Check(source.find("if (state.needsCoverWallReload) needStartLoad = true;") == std::string::npos,
          "BDD: Global cover-wall loading is not restarted by display-only refresh flags");
}

void TestSearchRefreshOnlyRequestsCoverWallDisplayReloadForDependentViewsContract() {
    const auto sourcePath = std::filesystem::path(__FILE__).parent_path().parent_path() / "src" / "main.cpp";
    std::ifstream input(sourcePath);
    Check(input.good(),
          "BDD: Given search-refresh cover-wall contract validation runs, src/main.cpp can be opened");
    if (!input.good()) {
        return;
    }

    std::ostringstream buffer;
    buffer << input.rdbuf();
    const std::string source = buffer.str();

    const auto applicationPath =
        std::filesystem::path(__FILE__).parent_path().parent_path() /
        "src" / "AsyncSearchResultApplicationAdapter.cpp";
    std::ifstream applicationInput(applicationPath);
    std::ostringstream applicationBuffer;
    applicationBuffer << applicationInput.rdbuf();
    const std::string applyBody = ExtractFunctionBody(
        source, "static void ApplyAsyncSearchResult(AppState& state, AsyncSearchResult& result)") +
        applicationBuffer.str();
    const auto root = std::filesystem::path(__FILE__).parent_path().parent_path();
    const auto workflowPath = root / "src" / "AsyncSearchRefreshCompletionWorkflow.cpp";
    const auto stateAdapterPath =
        root / "src" / "AsyncSearchRefreshCompletionStateAdapter.cpp";
    std::ifstream workflowInput(workflowPath);
    std::ifstream stateAdapterInput(stateAdapterPath);
    std::ostringstream workflowBuffer;
    std::ostringstream stateAdapterBuffer;
    workflowBuffer << workflowInput.rdbuf();
    stateAdapterBuffer << stateAdapterInput.rdbuf();
    const std::string workflowSource = workflowBuffer.str();
    const std::string stateAdapterSource = stateAdapterBuffer.str();
    const auto compositionPath =
        root / "src" / "AsyncSearchRefreshResultCompositionAdapter.cpp";
    std::ifstream compositionInput(compositionPath);
    std::ostringstream compositionBuffer;
    compositionBuffer << compositionInput.rdbuf();
    const std::string compositionSource = compositionBuffer.str();
    Check(workflowSource.find("input.playlistView || input.playlistLibraryView") != std::string::npos,
          "BDD: Non-database cover-wall views still request a display refresh after async search apply");
    Check(workflowSource.find("input.folderFilterActive || input.advancedDbFilters ||") != std::string::npos &&
          workflowSource.find("input.findSameFilenameActive") != std::string::npos &&
          workflowSource.find("input.sameFolderScanActive") != std::string::npos &&
          workflowSource.find("input.sameFolderScanDone") != std::string::npos,
          "BDD: Database cover-wall refresh is limited to views whose wall depends on filtered search state");
    Check(stateAdapterSource.find("HandleAsyncSearchRefreshCompletion(") != std::string::npos &&
          stateAdapterSource.find("requestCoverWallRefresh_(state_)") != std::string::npos &&
          compositionSource.find("refreshCompletionAdapter.apply();") != std::string::npos,
          "BDD: Async search apply only requests cover-wall display reload when the active wall depends on that result set");
}

void TestCoverWallLocalCooldownOnlyTracksConfirmedFailuresContract() {
    const auto root = std::filesystem::path(__FILE__).parent_path().parent_path();
    const auto mainPath = root / "src" / "main.cpp";
    const auto windowsMainPath = root / "src" / "WindowsMain.cpp";
    const auto failurePolicyPath = root / "src" / "CoverWallFailureCooldownPolicy.cpp";

    const auto facetsAdapterPath = std::filesystem::path(__FILE__).parent_path().parent_path() /
                                   "src" / "AsyncSearchRefreshFacetsAdapter.cpp";
    const auto stateAdapterPath = std::filesystem::path(__FILE__).parent_path().parent_path() /
                                  "src" / "AsyncSearchRefreshFacetsStateAdapter.cpp";
    std::ifstream mainInput(mainPath);
    std::ifstream facetsAdapterInput(facetsAdapterPath);
    std::ifstream stateAdapterInput(stateAdapterPath);
    std::ifstream windowsMainInput(windowsMainPath);
    std::ifstream failurePolicyInput(failurePolicyPath);
    Check(mainInput.good(),
          "BDD: Given cover-wall cooldown contract validation runs, main.cpp can be opened");
    Check(windowsMainInput.good(),
          "BDD: Given cover-wall cooldown contract validation runs, WindowsMain.cpp can be opened");
    if (!mainInput.good() || !windowsMainInput.good()) {
        return;
    }

    std::ostringstream mainBuffer;
    std::ostringstream windowsMainBuffer;
    std::ostringstream failurePolicyBuffer;
    mainBuffer << mainInput.rdbuf();
    windowsMainBuffer << windowsMainInput.rdbuf();
    failurePolicyBuffer << failurePolicyInput.rdbuf();
    const std::string mainSource = mainBuffer.str();
    const std::string windowsMainSource = windowsMainBuffer.str();
    const std::string failurePolicySource = failurePolicyBuffer.str();

    Check(failurePolicySource.find("bool ShouldRecordConfirmedFailure(const CoverModule::CoverItem& item)") != std::string::npos &&
          failurePolicySource.find("return CoverModule::IsCoverKeyFailed(item);") != std::string::npos,
          "BDD: Runtime cover wall distinguishes confirmed failures from not-yet-ready async loads");
    Check(mainSource.find("} else if (!ok && aura::cover_wall::ShouldRecordConfirmedFailure(item)) {") != std::string::npos &&
          mainSource.find("} else if (!hasTex && aura::cover_wall::ShouldRecordConfirmedFailure(item)) {") != std::string::npos,
          "BDD: Runtime cover wall only cools down confirmed failures in first-screen and visible-cell paths");
    Check(windowsMainSource.find("static bool CoverWallShouldRecordConfirmedFailureForQa(const CoverModule::CoverItem& item)") != std::string::npos &&
          windowsMainSource.find("} else if (!ok && CoverWallShouldRecordConfirmedFailureForQa(item)) {") != std::string::npos,
          "BDD: QA cover-wall path matches runtime cooldown semantics for confirmed failures only");
}

void TestCoverWallRejectsNonCoverKeysAndWaveformTextureReuseContract() {
    const auto root = std::filesystem::path(__FILE__).parent_path().parent_path();
    const auto appStatePath = root / "src" / "AppState.h";
    const auto mainPath = root / "src" / "main.cpp";
    const auto cachedKeyBuilderPath = root / "src" / "CoverCachedKeyItemBuilder.cpp";

    std::ifstream appStateInput(appStatePath);
    std::ifstream mainInput(mainPath);
    std::ifstream cachedKeyBuilderInput(cachedKeyBuilderPath);
    Check(appStateInput.good(),
          "BDD: Given cover-only wall contract validation runs, AppState.h can be opened");
    Check(mainInput.good(),
          "BDD: Given cover-only wall contract validation runs, main.cpp can be opened");
    Check(cachedKeyBuilderInput.good(),
          "BDD: Given cover-only wall contract validation runs, CoverCachedKeyItemBuilder.cpp can be opened");
    if (!appStateInput.good() || !mainInput.good() || !cachedKeyBuilderInput.good()) {
        return;
    }

    std::ostringstream appStateBuffer;
    std::ostringstream mainBuffer;
    std::ostringstream cachedKeyBuilderBuffer;
    std::ifstream textureMirrorPolicyInput(root / "src" / "CoverWallTextureMirrorPolicy.cpp");
    std::ostringstream textureMirrorPolicyBuffer;
    appStateBuffer << appStateInput.rdbuf();
    mainBuffer << mainInput.rdbuf();
    cachedKeyBuilderBuffer << cachedKeyBuilderInput.rdbuf();
    textureMirrorPolicyBuffer << textureMirrorPolicyInput.rdbuf();
    const std::string appStateSource = appStateBuffer.str();
    const std::string mainSource = mainBuffer.str();
    const std::string cachedKeyBuilderSource = cachedKeyBuilderBuffer.str();
    const std::string textureMirrorPolicySource = textureMirrorPolicyBuffer.str();

    const std::string realCoverBody = ExtractFunctionBody(
        cachedKeyBuilderSource, "bool IsRealCoverWallItemCandidate(const CoverModule::CoverItem& item)");
    const std::string sanitizeBody = ExtractFunctionBody(
        cachedKeyBuilderSource, "void SanitizeCoverWallItemsForCache(std::vector<CoverModule::CoverItem>& items)");
    const std::string collisionCullBody = textureMirrorPolicySource;
    const std::string deleteMiniTextureBody = ExtractFunctionBody(
        mainSource, "static void DeleteMiniWaveTexture(GLuint& textureId)");
    const std::string ensureMiniBody = ExtractFunctionBody(
        mainSource, "static bool EnsureMiniWaveTextureLoaded");
    const std::string renderBody = ExtractFunctionBody(
        mainSource, "void RenderFileInfoPanelTabContents(");

    Check(!realCoverBody.empty(),
          "BDD: Cover wall has an inspectable real-cover candidate whitelist");
    Check(realCoverBody.find("E|") != std::string::npos &&
          realCoverBody.find("F|") != std::string::npos &&
          realCoverBody.find("?|") != std::string::npos,
          "BDD: Cover wall whitelist only admits embedded, folder, or folder-probe cover identities");
    Check(realCoverBody.find("isPlaceholder") != std::string::npos,
          "BDD: Placeholder items cannot be treated as loadable cover textures");
    Check(!sanitizeBody.empty() &&
          sanitizeBody.find("!IsRealCoverWallItemCandidate(item)") != std::string::npos,
          "BDD: Cover wall sanitization removes non-cover keys before rendering/cache storage");
    Check(!collisionCullBody.empty() &&
          collisionCullBody.find("it->second.texId == textureId") != std::string::npos &&
          collisionCullBody.find("state.texCache.erase") != std::string::npos,
          "BDD: Non-cover texture creation only culls cover-wall mirrors that collide on GL texture id");
    Check(!deleteMiniTextureBody.empty() &&
          deleteMiniTextureBody.find("CullTextureMirrorsMatchingNonCoverTexture(") != std::string::npos &&
          deleteMiniTextureBody.find("glDeleteTextures(1, &textureId)") != std::string::npos &&
          deleteMiniTextureBody.find("textureId = 0;") != std::string::npos,
          "BDD: Deleting a mini-wave texture culls only matching cover mirrors before releasing the GL id");
    Check(!ensureMiniBody.empty() &&
          ensureMiniBody.find("CullTextureMirrorsMatchingNonCoverTexture(") != std::string::npos &&
          ensureMiniBody.find("MarkCoverWallTextureMirrorsStaleFromNonCoverTexture();\n    }\n    glBindTexture") == std::string::npos,
          "BDD: Loading a missing mini-wave texture does not invalidate the whole cover wall");
    const size_t miniResultTextureDelete =
        mainSource.find("DeleteMiniWaveTexture(texIt->second.textureId);");
    Check(miniResultTextureDelete != std::string::npos &&
          mainSource.find("CullTextureMirrorsMatchingNonCoverTexture(",
                          miniResultTextureDelete) != std::string::npos,
          "BDD: Replacing a mini-wave texture after clicking a file culls only matching cover mirrors, not the whole cover wall epoch");
    Check(appStateSource.find("uint64_t nonCoverTextureEpoch = 0;") != std::string::npos,
          "BDD: Cover wall texture mirrors carry a non-cover texture epoch guard");
    Check(renderBody.find("texIt->second.nonCoverTextureEpoch == cw.nonCoverTextureEpoch") != std::string::npos,
          "BDD: Cover wall refuses cached texture ids after waveform/non-cover texture reuse risk changes");
}

void TestTabCoverFilterStateDoesNotLeakAcrossTabsContract() {
    const auto root = std::filesystem::path(__FILE__).parent_path().parent_path();
    const auto appStatePath = root / "src" / "AppState.h";
    const auto appStateImplPath = root / "src" / "AppState.cpp";
    const auto mainPath = root / "src" / "main.cpp";

    std::ifstream appStateInput(appStatePath);
    std::ifstream appStateImplInput(appStateImplPath);
    std::ifstream mainInput(mainPath);
    Check(appStateInput.good(),
          "BDD: Given tab cover-filter contract validation runs, AppState.h can be opened");
    Check(appStateImplInput.good(),
          "BDD: Given tab cover-filter contract validation runs, AppState.cpp can be opened");
    Check(mainInput.good(),
          "BDD: Given tab cover-filter contract validation runs, main.cpp can be opened");
    if (!appStateInput.good() || !appStateImplInput.good() || !mainInput.good()) {
        return;
    }

    std::ostringstream appStateBuffer;
    std::ostringstream appStateImplBuffer;
    std::ostringstream mainBuffer;
    appStateBuffer << appStateInput.rdbuf();
    appStateImplBuffer << appStateImplInput.rdbuf();
    mainBuffer << mainInput.rdbuf();
    const std::string appStateSource = appStateBuffer.str();
    const std::string appStateImplSource = appStateImplBuffer.str();
    const std::string mainSource = mainBuffer.str();

    Check(appStateSource.find("bool coverFilterActive = false;") != std::string::npos &&
          appStateSource.find("std::string coverFilterPath;") != std::string::npos,
          "BDD: ListTab stores cover filter state instead of leaking it globally across tab switches");
    Check(appStateImplSource.find("coverFilterActive = tab.coverFilterActive;") != std::string::npos &&
          appStateImplSource.find("coverFilterPath = tab.coverFilterPath;") != std::string::npos &&
          appStateImplSource.find("coverFilterIndex = tab.coverFilterIndex;") != std::string::npos,
          "BDD: syncFromTab restores the active tab's cover filter state");
    Check(appStateImplSource.find("tab.coverFilterActive = coverFilterActive;") != std::string::npos &&
          appStateImplSource.find("tab.coverFilterPath = coverFilterActive ? coverFilterPath : std::string();") != std::string::npos &&
          appStateImplSource.find("tab.coverFilterIndex = coverFilterIndex;") != std::string::npos,
          "BDD: syncToTab stores the current cover filter state before switching tabs");
    Check(appStateImplSource.find("coverWall.pinnedCoverFilterItem = CoverModule::CoverItem{};") != std::string::npos,
          "BDD: Switching to a tab without cover filtering clears the pinned cover-filter item");
    Check(mainSource.find("prefix + \"coverFilterActive\"") != std::string::npos &&
          mainSource.find("prefix + \"coverFilterPath\"") != std::string::npos &&
          mainSource.find("prefix + \"coverFilterIndex\"") != std::string::npos,
          "BDD: Persisted list tabs round-trip cover filter state");
    Check(mainSource.find("tp + \"coverFilterActive\"") != std::string::npos &&
          mainSource.find("tp + \"coverFilterPath\"") != std::string::npos &&
          mainSource.find("tp + \"coverFilterIndex\"") != std::string::npos,
          "BDD: Tab presets round-trip cover filter state");
}

void TestTabDbFilterStateDoesNotLeakAcrossTabsContract() {
    const auto root = std::filesystem::path(__FILE__).parent_path().parent_path();
    const auto appStatePath = root / "src" / "AppState.h";
    const auto appStateImplPath = root / "src" / "AppState.cpp";

    std::ifstream appStateInput(appStatePath);
    std::ifstream appStateImplInput(appStateImplPath);
    Check(appStateInput.good(),
          "BDD: Given tab DB-filter contract validation runs, AppState.h can be opened");
    Check(appStateImplInput.good(),
          "BDD: Given tab DB-filter contract validation runs, AppState.cpp can be opened");
    if (!appStateInput.good() || !appStateImplInput.good()) {
        return;
    }

    std::ostringstream appStateBuffer;
    std::ostringstream appStateImplBuffer;
    appStateBuffer << appStateInput.rdbuf();
    appStateImplBuffer << appStateImplInput.rdbuf();
    const std::string appStateSource = appStateBuffer.str();
    const std::string appStateImplSource = appStateImplBuffer.str();

    Check(appStateSource.find("std::string artistFilterText;") != std::string::npos &&
          appStateSource.find("std::vector<std::string> artistFilterAlternates;") != std::string::npos &&
          appStateSource.find("std::string albumFilterText;") != std::string::npos &&
          appStateSource.find("std::string categoryFilterText;") != std::string::npos &&
          appStateSource.find("int formatFilterIndex = 0;") != std::string::npos &&
          appStateSource.find("int channelFilterIndex = 0;") != std::string::npos &&
          appStateSource.find("float minDurationFilter = 0.0f;") != std::string::npos &&
          appStateSource.find("float maxDurationFilter = 0.0f;") != std::string::npos &&
          appStateSource.find("int sampleRateFilterIndex = 0;") != std::string::npos &&
          appStateSource.find("float minBpmFilter = 0.0f;") != std::string::npos &&
          appStateSource.find("float maxBpmFilter = 0.0f;") != std::string::npos &&
          appStateSource.find("std::string keyFilterText;") != std::string::npos &&
          appStateSource.find("int namingFilter = 0;") != std::string::npos,
          "BDD: ListTab stores metadata and duration/key filter state instead of borrowing the previous tab's live AppState");
    Check(appStateSource.find("SmartCollection activeSmartCollection = SmartCollection::None;") != std::string::npos &&
          appStateSource.find("int activeUserCollectionIdx = -1;") != std::string::npos &&
          appStateSource.find("std::map<std::string, int> genrePillState;") != std::string::npos &&
          appStateSource.find("std::map<std::string, int> instrumentPillState;") != std::string::npos &&
          appStateSource.find("bool showWAV = true;") != std::string::npos &&
          appStateSource.find("bool showOther = true;") != std::string::npos,
          "BDD: ListTab stores smart-collection, pill, and file-type visibility filters that affect whether cover wall is global or filtered");

    const std::string syncFromBody = ExtractFunctionBody(
        appStateImplSource, "void AppState::syncFromTab()");
    const std::string syncToBody = ExtractFunctionBody(
        appStateImplSource, "void AppState::syncToTab()");
    Check(!syncFromBody.empty(),
          "BDD: syncFromTab body is inspectable for tab DB-filter restore");
    Check(!syncToBody.empty(),
          "BDD: syncToTab body is inspectable for tab DB-filter capture");
    Check(syncFromBody.find("artistFilterAlternates = tab.artistFilterAlternates;") != std::string::npos &&
          syncFromBody.find("activeSmartCollection = tab.activeSmartCollection;") != std::string::npos &&
          syncFromBody.find("activeUserCollectionIdx = tab.activeUserCollectionIdx;") != std::string::npos &&
          syncFromBody.find("genrePillState = tab.genrePillState;") != std::string::npos &&
          syncFromBody.find("instrumentPillState = tab.instrumentPillState;") != std::string::npos &&
          syncFromBody.find("showWAV = tab.showWAV;") != std::string::npos &&
          syncFromBody.find("showOther = tab.showOther;") != std::string::npos,
          "BDD: syncFromTab restores the target tab's DB-filter semantics before cover-wall mode is recomputed");
    Check(syncToBody.find("tab.artistFilterAlternates = artistFilterAlternates;") != std::string::npos &&
          syncToBody.find("tab.activeSmartCollection = activeSmartCollection;") != std::string::npos &&
          syncToBody.find("tab.activeUserCollectionIdx = activeUserCollectionIdx;") != std::string::npos &&
          syncToBody.find("tab.genrePillState = genrePillState;") != std::string::npos &&
          syncToBody.find("tab.instrumentPillState = instrumentPillState;") != std::string::npos &&
          syncToBody.find("tab.showWAV = showWAV;") != std::string::npos &&
          syncToBody.find("tab.showOther = showOther;") != std::string::npos,
          "BDD: syncToTab captures the current tab's DB-filter semantics before another tab becomes active");

    Check(appStateImplSource.find("static bool ListTabHasActiveDbFilterForCoverWall(const ListTab& tab)") != std::string::npos &&
          appStateImplSource.find("if (!tab.artistFilterAlternates.empty()) return true;") != std::string::npos &&
          appStateImplSource.find("if (tab.activeSmartCollection != SmartCollection::None) return true;") != std::string::npos &&
          appStateImplSource.find("if (!(tab.showWAV && tab.showMP3 && tab.showFLAC && tab.showOGG &&") != std::string::npos &&
          appStateImplSource.find("if (ListTabHasActiveDbFilterForCoverWall(tab)) return false;") != std::string::npos,
          "BDD: Full-library cover-wall session carry is decided from the target tab's saved DB-filter state, not stale live globals from another tab");
}

void TestTabSwitchPreservesCoverWallDisplaySnapshotContract() {
    const auto root = std::filesystem::path(__FILE__).parent_path().parent_path();
    const auto appStatePath = root / "src" / "AppState.h";
    const auto appStateImplPath = root / "src" / "AppState.cpp";
    const auto mainPath = root / "src" / "main.cpp";

    std::ifstream appStateInput(appStatePath);
    std::ifstream appStateImplInput(appStateImplPath);
    std::ifstream mainInput(mainPath);
    Check(appStateInput.good(),
          "BDD: Given tab cover-wall snapshot contract validation runs, AppState.h can be opened");
    Check(appStateImplInput.good(),
          "BDD: Given tab cover-wall snapshot contract validation runs, AppState.cpp can be opened");
    Check(mainInput.good(),
          "BDD: Given tab cover-wall snapshot contract validation runs, main.cpp can be opened");
    if (!appStateInput.good() || !appStateImplInput.good() || !mainInput.good()) {
        return;
    }

    std::ostringstream appStateBuffer;
    std::ostringstream appStateImplBuffer;
    std::ostringstream mainBuffer;
    appStateBuffer << appStateInput.rdbuf();
    appStateImplBuffer << appStateImplInput.rdbuf();
    mainBuffer << mainInput.rdbuf();
    const std::string appStateSource = appStateBuffer.str();
    const std::string appStateImplSource = appStateImplBuffer.str();
    const std::string mainSource = mainBuffer.str();

    Check(appStateSource.find("CoverWallTabSnapshot coverWallSnapshot;") != std::string::npos,
          "BDD: ListTab owns a cover-wall display snapshot so tab switches do not share one mutable wall list");
    Check(appStateSource.find("int source = 0;") != std::string::npos &&
          appStateSource.find("std::vector<CoverModule::CoverItem> globalItemsCache;") != std::string::npos &&
          appStateSource.find("std::unordered_map<std::string, CoverWallTextureMirrorEntry> texCache;") != std::string::npos &&
          appStateSource.find("std::unordered_map<std::string, CoverWallFailedTextureEntry> failedTexCache;") != std::string::npos,
          "BDD: Tab snapshot stores a complete cover-wall display session, not just a partial item list");
    Check(appStateSource.find("uint64_t viewGeneration = 0;") != std::string::npos &&
          appStateSource.find("entriesViewGeneration") != std::string::npos &&
          appStateSource.find("foldersViewGeneration") != std::string::npos,
          "BDD: CoverWallState has a view generation token so stale async wall results cannot overwrite another tab");
    Check(appStateSource.find("std::string globalPageNextCursor;") != std::string::npos &&
          appStateSource.find("bool globalPageHasMore = false;") != std::string::npos &&
          appStateSource.find("std::string globalPageDbPath;") != std::string::npos,
          "BDD: CoverWallState owns global cover-wall page cursor state so paged browse can continue after a partial wall load");

    Check(mainSource.find("state.activateTab(ti, ImGui::GetTime())") != std::string::npos,
          "BDD: Switching existing tabs goes through AppState tab lifecycle");

    Check(mainSource.find("state.createAndActivateNewDatabaseTab(ImGui::GetTime())") != std::string::npos,
          "BDD: Creating a tab preserves the old tab wall and seeds/restores a compatible full-library wall through AppState");
    Check(appStateImplSource.find("createNewTab(u8\"全部文件\")") != std::string::npos,
          "BDD: AppState-created database tabs are explicitly named as full-library tabs, not ambiguous tag tabs");

    const size_t entriesReady = mainSource.find("cw.entriesInFlight && cw.entriesFuture.valid()");
    Check(entriesReady != std::string::npos &&
          mainSource.find("staleEntriesView", entriesReady) != std::string::npos &&
          mainSource.find("!staleEntriesView", entriesReady) != std::string::npos,
          "BDD: Async global cover-wall entries are ignored when they belong to a stale tab view");

    const size_t foldersReady = mainSource.find("cw.foldersInFlight && cw.foldersFuture.valid()");
    Check(foldersReady != std::string::npos &&
          mainSource.find("staleFoldersView", foldersReady) != std::string::npos &&
          mainSource.find("!staleFoldersView", foldersReady) != std::string::npos,
          "BDD: Async folder-discovery cover-wall results are ignored when they belong to a stale tab view");

    const std::string saveBody = ExtractFunctionBody(
        appStateImplSource, "void AppState::saveCoverWallDisplaySnapshotForTab(int tabIndex)");
    const std::string restoreBody = ExtractFunctionBody(
        appStateImplSource, "bool AppState::restoreCoverWallDisplaySnapshotForTab(int tabIndex)");
    Check(saveBody.find("snapshot.source = static_cast<int>(cw.source);") != std::string::npos &&
          saveBody.find("snapshot.globalItemsCache = cw.globalItemsCache;") != std::string::npos &&
          saveBody.find("snapshot.globalPageNextCursor = cw.globalPageNextCursor;") != std::string::npos &&
          saveBody.find("snapshot.globalPageHasMore = cw.globalPageHasMore;") != std::string::npos &&
          saveBody.find("snapshot.globalPageDbPath = cw.globalPageDbPath;") != std::string::npos &&
          saveBody.find("snapshot.pinnedCoverFilterItem = cw.pinnedCoverFilterItem;") != std::string::npos &&
          saveBody.find("snapshot.texCache = cw.texCache;") != std::string::npos &&
          saveBody.find("snapshot.failedTexCache = cw.failedTexCache;") != std::string::npos,
          "BDD: Saving a tab captures source, global cache, global page cursor, pinned cover-filter identity, texture mirrors, and failed texture state");
    Check(restoreBody.find("cw.source = static_cast<CoverWallSource>(snapshot.source);") != std::string::npos &&
          restoreBody.find("cw.globalItemsCache = snapshot.globalItemsCache;") != std::string::npos &&
          restoreBody.find("cw.globalPageNextCursor = snapshot.globalPageNextCursor;") != std::string::npos &&
          restoreBody.find("cw.globalPageHasMore = snapshot.globalPageHasMore;") != std::string::npos &&
          restoreBody.find("cw.globalPageDbPath = snapshot.globalPageDbPath;") != std::string::npos &&
          restoreBody.find("cw.pinnedCoverFilterItem = snapshot.pinnedCoverFilterItem;") != std::string::npos &&
          restoreBody.find("cw.texCache = snapshot.texCache;") != std::string::npos &&
          restoreBody.find("cw.failedTexCache = snapshot.failedTexCache;") != std::string::npos,
          "BDD: Restoring a tab reinstates the complete paged cover-wall session, including cursor and pinned cover-filter metadata, before rendering");
    Check(restoreBody.find("const bool snapshotGenerationChanged = snapshot.generation != cw.generation;") != std::string::npos &&
          restoreBody.find("if (!snapshot.valid || snapshot.dbPath != currentDbPath)") != std::string::npos &&
          restoreBody.find("snapshot.generation != cw.generation ||") == std::string::npos,
          "BDD: Tab restore may reuse the last wall snapshot even after generation changed; generation only gates stale async work, not UI recovery");
    Check(restoreBody.find("liveSession = CaptureLiveFullLibraryCoverWallSession(cw);") != std::string::npos &&
          restoreBody.find("!snapshotGenerationChanged &&") != std::string::npos &&
          restoreBody.find("ShouldCarryLiveFullLibraryCoverWallSession") != std::string::npos &&
          restoreBody.find("cw.texCache.size() > snapshot.texCache.size()") != std::string::npos,
          "BDD: Switching between compatible full-library tabs carries the warmer live cover-wall display session instead of downgrading to an older tab snapshot");
    Check(appStateImplSource.find("tab.name != u8\"全部文件\"") != std::string::npos,
          "BDD: Label/search result tabs are not treated as full-library tabs just because their filter fields are empty");
    Check(restoreBody.find("ApplyLiveFullLibraryCoverWallSession(cw, liveSession, liveNonCoverTextureEpoch);") != std::string::npos,
          "BDD: Compatible full-library tab restore preserves live item lists and texture mirrors together so validation does not prune the warm wall");
    Check(restoreBody.find("const uint64_t liveNonCoverTextureEpoch = cw.nonCoverTextureEpoch;") != std::string::npos &&
          restoreBody.find("snapshot.nonCoverTextureEpoch != liveNonCoverTextureEpoch") != std::string::npos &&
          restoreBody.find("it->second.nonCoverTextureEpoch != liveNonCoverTextureEpoch") != std::string::npos &&
          restoreBody.find("cw.nonCoverTextureEpoch = liveNonCoverTextureEpoch;") != std::string::npos,
          "BDD: Restoring a tab prunes stale texture mirrors without clearing current live mirrors wholesale");
}

void TestCoverWallTextureMirrorEligibilityChecksEpochContract() {
    const auto mainPath = std::filesystem::path(__FILE__).parent_path().parent_path() / "src" / "main.cpp";
    std::ifstream mainInput(mainPath);
    Check(mainInput.good(),
          "BDD: Given cover-wall texture mirror eligibility validation runs, main.cpp can be opened");
    if (!mainInput.good()) {
        return;
    }

    std::ostringstream mainBuffer;
    mainBuffer << mainInput.rdbuf();
    const std::string mainSource = mainBuffer.str();
    const std::string mirrorHelperBody = ExtractFunctionBody(
        mainSource, "auto coverWallHasCurrentTextureMirror = [&](const CoverModule::CoverItem& item) -> bool");

    Check(!mirrorHelperBody.empty(),
          "BDD: Cover-wall texture mirror eligibility helper is inspectable");
    Check(mirrorHelperBody.find("texIt->second.nonCoverTextureEpoch == cw.nonCoverTextureEpoch") != std::string::npos,
          "BDD: Cover-wall pre-request eligibility refuses texture mirrors from an old non-cover texture epoch");
}

void TestTabResultOwnershipBoundaryPreventsCoverWallCrossTabRebuildContract() {
    const auto root = std::filesystem::path(__FILE__).parent_path().parent_path();
    const auto appStatePath = root / "src" / "AppState.h";
    const auto appStateImplPath = root / "src" / "AppState.cpp";
    const auto mainPath = root / "src" / "main.cpp";

    std::ifstream appStateInput(appStatePath);
    std::ifstream appStateImplInput(appStateImplPath);
    std::ifstream mainInput(mainPath);
    Check(appStateInput.good(),
          "BDD: Given tab result ownership contract validation runs, AppState.h can be opened");
    Check(appStateImplInput.good(),
          "BDD: Given tab result ownership contract validation runs, AppState.cpp can be opened");
    Check(mainInput.good(),
          "BDD: Given tab result ownership contract validation runs, main.cpp can be opened");
    if (!appStateInput.good() || !appStateImplInput.good() || !mainInput.good()) {
        return;
    }

    std::ostringstream appStateBuffer;
    std::ostringstream appStateImplBuffer;
    std::ostringstream mainBuffer;
    appStateBuffer << appStateInput.rdbuf();
    appStateImplBuffer << appStateImplInput.rdbuf();
    mainBuffer << mainInput.rdbuf();
    const std::string appStateSource = appStateBuffer.str();
    const std::string appStateImplSource = appStateImplBuffer.str();
    const std::string mainSource = mainBuffer.str();

    Check(appStateSource.find("bool searchResultsSnapshotValid = false;") != std::string::npos &&
          appStateSource.find("bool facetsSnapshotValid = false;") != std::string::npos,
          "BDD: ListTab explicitly tracks parked result/facet snapshot ownership, including empty snapshots");
    Check(appStateSource.find("void parkLiveResultsInCurrentTab();") != std::string::npos,
          "BDD: AppState owns the semantic entrypoint for parking live tab result data");

    const std::string syncFromBody = ExtractFunctionBody(
        appStateImplSource, "void AppState::syncFromTab()");
    const std::string parkBody = ExtractFunctionBody(
        appStateImplSource, "void AppState::parkLiveResultsInCurrentTab()");
    Check(!syncFromBody.empty(),
          "BDD: syncFromTab body is inspectable");
    Check(!parkBody.empty(),
          "BDD: parkLiveResultsInCurrentTab body is inspectable");

    Check(syncFromBody.find("tab.searchResultsSnapshotValid") != std::string::npos &&
          syncFromBody.find("tab.facetsSnapshotValid") != std::string::npos,
          "BDD: Activating a tab restores parked snapshots by ownership flags instead of non-empty heuristics");
    Check(syncFromBody.find("!tab.searchResults.empty()") == std::string::npos &&
          syncFromBody.find("!tab.coverFacets.empty()") == std::string::npos,
          "BDD: Empty search/facet snapshots are not skipped during tab activation");
    Check(parkBody.find("std::move(searchResults)") != std::string::npos &&
          parkBody.find("std::move(currentCoverFacets)") != std::string::npos &&
          parkBody.find("searchResultsSnapshotValid = true") != std::string::npos &&
          parkBody.find("facetsSnapshotValid = true") != std::string::npos,
          "BDD: Parking a tab moves live result/facet ownership and marks both snapshots valid");

    Check(mainSource.find("std::move(state.searchResults)") == std::string::npos &&
          mainSource.find("std::move(state.currentCoverFacets)") == std::string::npos,
          "BDD: UI tab lifecycle does not bypass AppState result ownership by directly moving live containers");
    Check(appStateImplSource.find("parkLiveResultsInCurrentTab();") != std::string::npos,
          "BDD: AppState tab lifecycle parks live results before changing active ownership");

    const std::string createActivateBody = ExtractFunctionBody(
        appStateImplSource, "int AppState::createAndActivateNewDatabaseTab(double activationTimeSeconds)");
    Check(!createActivateBody.empty(),
          "BDD: createAndActivateNewDatabaseTab body is inspectable");
    Check(createActivateBody.find("database.getAllFiles") == std::string::npos &&
          createActivateBody.find("database.getFileCount") == std::string::npos,
          "BDD: Creating a tab does not synchronously query the database on the UI/QA lifecycle path");
    Check(createActivateBody.find("newTab.dataUnloaded = true;") != std::string::npos &&
          createActivateBody.find("needsTabResultsRefresh = true;") != std::string::npos &&
          createActivateBody.find("bumpSearchResultsGen();") == std::string::npos &&
          createActivateBody.find("bumpFacetsGen();") == std::string::npos,
          "BDD: Creating a tab requests the existing async refresh pipeline instead of minting a fake snapshot generation");
    Check(mainSource.find("state.createAndActivateNewDatabaseTab(ImGui::GetTime())") != std::string::npos,
          "BDD: New-tab UI path delegates result/facet ownership to AppState lifecycle");
}

void TestCoverWallTabLifecycleHasSingleSemanticOwnerContract() {
    const auto root = std::filesystem::path(__FILE__).parent_path().parent_path();
    const auto appStatePath = root / "src" / "AppState.h";
    const auto appStateImplPath = root / "src" / "AppState.cpp";
    const auto mainPath = root / "src" / "main.cpp";
    const auto windowsMainPath = root / "src" / "WindowsMain.cpp";

    std::ifstream appStateInput(appStatePath);
    std::ifstream appStateImplInput(appStateImplPath);
    std::ifstream mainInput(mainPath);
    std::ifstream windowsMainInput(windowsMainPath);
    Check(appStateInput.good(),
          "BDD: Given cover-wall tab lifecycle contract validation runs, AppState.h can be opened");
    Check(appStateImplInput.good(),
          "BDD: Given cover-wall tab lifecycle contract validation runs, AppState.cpp can be opened");
    Check(mainInput.good(),
          "BDD: Given cover-wall tab lifecycle contract validation runs, main.cpp can be opened");
    Check(windowsMainInput.good(),
          "BDD: Given cover-wall tab lifecycle contract validation runs, WindowsMain.cpp can be opened");
    if (!appStateInput.good() || !appStateImplInput.good() || !mainInput.good() || !windowsMainInput.good()) {
        return;
    }

    std::ostringstream appStateBuffer;
    std::ostringstream appStateImplBuffer;
    std::ostringstream mainBuffer;
    std::ostringstream windowsMainBuffer;
    appStateBuffer << appStateInput.rdbuf();
    appStateImplBuffer << appStateImplInput.rdbuf();
    mainBuffer << mainInput.rdbuf();
    windowsMainBuffer << windowsMainInput.rdbuf();
    const std::string appStateSource = appStateBuffer.str();
    const std::string appStateImplSource = appStateImplBuffer.str();
    const std::string mainSource = mainBuffer.str();
    const std::string windowsMainSource = windowsMainBuffer.str();

    Check(appStateSource.find("void saveCoverWallDisplaySnapshotForTab(int tabIndex);") != std::string::npos &&
          appStateSource.find("bool restoreCoverWallDisplaySnapshotForTab(int tabIndex);") != std::string::npos &&
          appStateSource.find("bool activateTab(int tabIndex") != std::string::npos &&
          appStateSource.find("int createAndActivateNewDatabaseTab(") != std::string::npos &&
          appStateSource.find("int pendingTabSelectionIndex = -1;") != std::string::npos,
          "BDD: AppState exposes one semantic owner for cover-wall tab lifecycle transitions");
    Check(appStateImplSource.find("void AppState::saveCoverWallDisplaySnapshotForTab(int tabIndex)") != std::string::npos &&
          appStateImplSource.find("bool AppState::restoreCoverWallDisplaySnapshotForTab(int tabIndex)") != std::string::npos &&
          appStateImplSource.find("bool AppState::activateTab(int tabIndex") != std::string::npos &&
          appStateImplSource.find("int AppState::createAndActivateNewDatabaseTab(") != std::string::npos,
          "BDD: AppState implements cover-wall snapshot save/restore and tab activation as one lifecycle");
    Check(mainSource.find("static void SaveCoverWallDisplaySnapshotForTab(AppState& state, int tabIndex)") == std::string::npos &&
          mainSource.find("static bool RestoreCoverWallDisplaySnapshotForTab(AppState& state, int tabIndex)") == std::string::npos,
          "BDD: main.cpp no longer owns cover-wall tab snapshot helpers that QA and non-UI paths can bypass");
    Check(mainSource.find("state.activateTab(ti, ImGui::GetTime())") != std::string::npos &&
          mainSource.find("state.createAndActivateNewDatabaseTab(ImGui::GetTime())") != std::string::npos,
          "BDD: Existing-tab switch and new-tab UI paths use the unified AppState tab lifecycle");
    Check(mainSource.find("ImGuiTabItemFlags_SetSelected") != std::string::npos &&
          mainSource.find("state.pendingTabSelectionIndex == ti") != std::string::npos &&
          mainSource.find("state.pendingTabSelectionIndex = -1;") != std::string::npos &&
          mainSource.find("pendingSelectionForAnotherTab") != std::string::npos &&
          mainSource.find("!pendingSelectionForAnotherTab && state.activeTabIndex != ti") != std::string::npos,
          "BDD: Programmatic/QA tab activation is reconciled with ImGui's selected tab state on the next frame");
    Check(mainSource.find("newTab.searchResults = state.database.getAllFiles") == std::string::npos &&
          mainSource.find("newTab.searchResultsSnapshotValid = true;") == std::string::npos,
          "BDD: New-tab UI path no longer hand-builds result ownership outside AppState");
    Check(windowsMainSource.find("cmd == \"new_tab\"") != std::string::npos &&
          windowsMainSource.find("cmd == \"activate_tab\"") != std::string::npos &&
          windowsMainSource.find("state.createAndActivateNewDatabaseTab(-1.0)") != std::string::npos &&
          windowsMainSource.find("state.activateTab(tabIndex, -1.0") != std::string::npos,
          "BDD: QA tab commands use the same AppState lifecycle as the UI, enabling runtime regression probes");
}

void TestCoverWallTextureMirrorRejectsNonCoverTextureReuseContract() {
    const auto root = std::filesystem::path(__FILE__).parent_path().parent_path();
    const auto coverHeaderPath = root / "src" / "CoverModule.h";
    const auto coverImplPath = root / "src" / "CoverModule.cpp";
    const auto appStateImplPath = root / "src" / "AppState.cpp";
    const auto mainPath = root / "src" / "main.cpp";
    const auto textureMirrorPolicyPath = root / "src" / "CoverWallTextureMirrorPolicy.cpp";
    const auto visibleMirrorAuditPath = root / "src" / "CoverWallVisibleMirrorAudit.cpp";

    std::ifstream coverHeaderInput(coverHeaderPath);
    std::ifstream coverImplInput(coverImplPath);
    std::ifstream appStateImplInput(appStateImplPath);
    std::ifstream mainInput(mainPath);
    std::ifstream textureMirrorPolicyInput(textureMirrorPolicyPath);
    std::ifstream visibleMirrorAuditInput(visibleMirrorAuditPath);
    Check(coverHeaderInput.good(),
          "BDD: Given cover-wall texture mirror validation runs, CoverModule.h can be opened");
    Check(coverImplInput.good(),
          "BDD: Given cover-wall texture mirror validation runs, CoverModule.cpp can be opened");
    Check(appStateImplInput.good(),
          "BDD: Given cover-wall texture mirror validation runs, AppState.cpp can be opened");
    Check(mainInput.good(),
          "BDD: Given cover-wall texture mirror validation runs, main.cpp can be opened");
    if (!coverHeaderInput.good() || !coverImplInput.good() || !appStateImplInput.good() ||
        !mainInput.good() || !textureMirrorPolicyInput.good() || !visibleMirrorAuditInput.good()) {
        return;
    }

    std::ostringstream coverHeaderBuffer;
    std::ostringstream coverImplBuffer;
    std::ostringstream appStateImplBuffer;
    std::ostringstream mainBuffer;
    std::ostringstream textureMirrorPolicyBuffer;
    std::ostringstream visibleMirrorAuditBuffer;
    coverHeaderBuffer << coverHeaderInput.rdbuf();
    coverImplBuffer << coverImplInput.rdbuf();
    appStateImplBuffer << appStateImplInput.rdbuf();
    mainBuffer << mainInput.rdbuf();
    textureMirrorPolicyBuffer << textureMirrorPolicyInput.rdbuf();
    visibleMirrorAuditBuffer << visibleMirrorAuditInput.rdbuf();
    const std::string coverHeaderSource = coverHeaderBuffer.str();
    const std::string coverImplSource = coverImplBuffer.str();
    const std::string appStateImplSource = appStateImplBuffer.str();
    const std::string mainSource = mainBuffer.str();
    const std::string textureMirrorPolicySource = textureMirrorPolicyBuffer.str();
    const std::string visibleMirrorAuditSource = visibleMirrorAuditBuffer.str();

    Check(coverHeaderSource.find("bool IsCoverTextureHandleCurrent(const CoverItem& item, uint32_t textureId);") != std::string::npos,
          "BDD: CoverModule exposes an owner check for non-owning cover-wall texture mirrors");

    const std::string ownerCheckBody = ExtractFunctionBody(
        coverImplSource, "bool IsCoverTextureHandleCurrent(const CoverItem& item, uint32_t textureId)");
    Check(!ownerCheckBody.empty(),
          "BDD: Cover texture owner-check body is inspectable");
    Check(ownerCheckBody.find("g_embeddedCoverCache.find(item.key)") != std::string::npos &&
          ownerCheckBody.find("g_folderCoverKeyCache.find(item.key)") != std::string::npos &&
          ownerCheckBody.find("textureId == textureId") != std::string::npos,
          "BDD: Cover texture owner check verifies the live CoverModule cache still owns the GL id for that cover key");

    const std::string mirrorHelperBody = ExtractFunctionBody(
        textureMirrorPolicySource, "bool HasCurrentTextureMirror(const CoverWallState& state,");
    Check(!mirrorHelperBody.empty(),
          "BDD: Cover-wall texture-mirror helper body is inspectable");
    Check(mirrorHelperBody.find("CoverModule::IsCoverTextureHandleCurrent") == std::string::npos,
          "BDD: Cover-wall hot render path keeps mirror hits local/O(1) and does not lock CoverModule caches per cell");
    const std::string auditBody = ExtractFunctionBody(
        mainSource, "static size_t AuditAndCullStaleVisibleCoverWallTextureMirrors(const AppState& state,");
    Check(!auditBody.empty(),
          "BDD: Cover-wall one-shot visible mirror audit helper is inspectable");
    Check(auditBody.find("if (cw.lastVisibleMirrorAuditViewGeneration == cw.viewGeneration) return 0;") != std::string::npos &&
          auditBody.find("CullStaleVisibleTextureMirrors(") != std::string::npos &&
          auditBody.find("cw.lastVisibleMirrorAuditViewGeneration = cw.viewGeneration;") != std::string::npos &&
          visibleMirrorAuditSource.find("CoverModule::IsCoverTextureHandleCurrent(item, it->second.texId)") != std::string::npos &&
          visibleMirrorAuditSource.find("cw.texCache.erase(it);") != std::string::npos,
          "BDD: Tab/view changes trigger a one-shot visible mirror audit that culls stale texture ids before first-screen prefetch resumes");
    const std::string restoreBody = ExtractFunctionBody(
        appStateImplSource, "bool AppState::restoreCoverWallDisplaySnapshotForTab(int tabIndex)");
    Check(restoreBody.find("CoverModule::IsCoverTextureHandleCurrent(*item, it->second.texId)") != std::string::npos &&
          restoreBody.find("cw.texCache.erase(it)") != std::string::npos,
          "BDD: Restoring a tab validates non-owning texture mirrors once on the cold path, preventing waveform/spectrogram texture pollution");

    const std::string activateTabBody = ExtractFunctionBody(
        appStateImplSource, "bool AppState::activateTab(int tabIndex, double activationTimeSeconds, bool runMemoryGc)");
    Check(!activateTabBody.empty(),
          "BDD: Tab activation body is inspectable for cover-wall GC ordering");
    const size_t gcPos = activateTabBody.find("performGlobalMemoryGC();");
    const size_t restorePos = activateTabBody.find("restoreCoverWallDisplaySnapshotForTab(tabIndex);");
    Check(gcPos != std::string::npos &&
          restorePos != std::string::npos &&
          gcPos < restorePos,
          "BDD: Tab activation runs global cache GC before restoring cover-wall texture mirrors, so restored mirrors never point at post-GC stale cover textures");

    const std::string spectrogramUploadBody = ExtractFunctionBody(
        mainSource, "static void UploadSpectrogramTexture(GLuint& texId, int& outWidth, int& outHeight,");
    Check(spectrogramUploadBody.find("CullTextureMirrorsMatchingNonCoverTexture(") != std::string::npos,
          "BDD: Creating a spectrogram texture culls only matching cover-wall mirrors instead of refreshing the whole wall");
}

void TestQaStateExposesCoverWallAndTabRuntimeProbeContract() {
    const auto windowsMainPath =
        std::filesystem::path(__FILE__).parent_path().parent_path() / "src" / "WindowsMain.cpp";

    std::ifstream windowsMainInput(windowsMainPath);
    Check(windowsMainInput.good(),
          "BDD: Given QA runtime probe contract validation runs, WindowsMain.cpp can be opened");
    if (!windowsMainInput.good()) {
        return;
    }

    std::ostringstream windowsMainBuffer;
    windowsMainBuffer << windowsMainInput.rdbuf();
    const std::string windowsMainSource = windowsMainBuffer.str();
    const std::string getStateBody = ExtractFunctionBody(windowsMainSource, "if (cmd == \"get_state\")");

    Check(!getStateBody.empty(),
          "BDD: QA get_state command body is inspectable");
    Check(getStateBody.find("\\\"tabs\\\"") != std::string::npos &&
          getStateBody.find("\\\"active_index\\\"") != std::string::npos &&
          getStateBody.find("\\\"active_id\\\"") != std::string::npos &&
          getStateBody.find("\\\"count\\\"") != std::string::npos,
          "BDD: QA get_state exposes active tab identity so tab switches can be verified at runtime");
    Check(getStateBody.find("\\\"cover_wall\\\"") != std::string::npos &&
          getStateBody.find("\\\"source\\\"") != std::string::npos &&
          getStateBody.find("\\\"source_name\\\"") != std::string::npos &&
          getStateBody.find("\\\"generation\\\"") != std::string::npos &&
          getStateBody.find("\\\"view_generation\\\"") != std::string::npos &&
          getStateBody.find("\\\"thumb_size\\\"") != std::string::npos &&
          getStateBody.find("\\\"items\\\"") != std::string::npos &&
          getStateBody.find("\\\"unique_keys\\\"") != std::string::npos &&
          getStateBody.find("\\\"duplicate_keys\\\"") != std::string::npos &&
          getStateBody.find("\\\"empty_keys\\\"") != std::string::npos &&
          getStateBody.find("\\\"hidden_failed_items\\\"") != std::string::npos &&
          getStateBody.find("\\\"hidden_pending_items\\\"") != std::string::npos &&
          getStateBody.find("\\\"prefetch_tiles\\\"") != std::string::npos &&
          getStateBody.find("\\\"tex_cache\\\"") != std::string::npos &&
          getStateBody.find("\\\"pending_cover_work\\\"") != std::string::npos &&
          getStateBody.find("\\\"pending_cover_keys\\\"") != std::string::npos &&
          getStateBody.find("\\\"pending_cover_queue\\\"") != std::string::npos &&
          getStateBody.find("\\\"pending_cover_uploads\\\"") != std::string::npos &&
          getStateBody.find("\\\"pending_cover_upload_keys\\\"") != std::string::npos &&
          getStateBody.find("\\\"failed_cover_keys\\\"") != std::string::npos,
          "BDD: QA get_state exposes cover-wall display session, dedupe counters, texture mirrors, hidden pending items, pending cover work, and queue breakdown");
    Check(getStateBody.find("\\\"display_refresh_requested\\\"") != std::string::npos &&
          getStateBody.find("\\\"database_invalidation_requested\\\"") != std::string::npos &&
          getStateBody.find("\\\"needs_reload\\\"") != std::string::npos,
          "BDD: QA get_state separates display refresh from database invalidation at runtime");
    Check(getStateBody.find("\\\"search_results_snapshot_valid\\\"") != std::string::npos &&
          getStateBody.find("\\\"facets_snapshot_valid\\\"") != std::string::npos &&
          getStateBody.find("\\\"cover_wall_snapshot_valid\\\"") != std::string::npos,
          "BDD: QA get_state exposes parked tab ownership flags needed to catch cross-tab cover-wall rebuilds");
    Check(getStateBody.find("\\\"cover_rebuild\\\"") != std::string::npos &&
          getStateBody.find("\\\"active\\\"") != std::string::npos &&
          getStateBody.find("\\\"progress\\\"") != std::string::npos &&
          getStateBody.find("\\\"total\\\"") != std::string::npos &&
          getStateBody.find("\\\"found\\\"") != std::string::npos &&
          getStateBody.find("\\\"cancel_requested\\\"") != std::string::npos,
          "BDD: QA get_state exposes cover rebuild runtime progress so retirement probes can verify repair/rebuild recovery");
    Check(getStateBody.find("\\\"browse_surface\\\"") != std::string::npos &&
          getStateBody.find("\\\"selected_row\\\"") != std::string::npos &&
          getStateBody.find("\\\"selected_file_id\\\"") != std::string::npos &&
          getStateBody.find("\\\"selected_path\\\"") != std::string::npos &&
          getStateBody.find("\\\"effective_preview_file_id\\\"") != std::string::npos &&
          getStateBody.find("\\\"effective_preview_path\\\"") != std::string::npos &&
          getStateBody.find("\\\"preview_uses_selected_record\\\"") != std::string::npos &&
          getStateBody.find("\\\"pinned_cover_key\\\"") != std::string::npos &&
          getStateBody.find("\\\"pinned_cover_file_path\\\"") != std::string::npos &&
          getStateBody.find("\\\"pinned_cover_folder_path\\\"") != std::string::npos &&
          getStateBody.find("\\\"top_info\\\"") != std::string::npos &&
          getStateBody.find("\\\"bottom_info\\\"") != std::string::npos &&
          getStateBody.find("\\\"status_label\\\"") != std::string::npos &&
          getStateBody.find("\\\"cover_filter_label\\\"") != std::string::npos,
          "BDD: QA get_state exposes replacement browse-surface preview identity, pinned cover scope, and upper/lower info labels for Slice 3 proofs");
}

void TestHarkenSearchTargetLinksMediaFoundationForRuntimeProbeContract() {
    const auto cmakePath =
        std::filesystem::path(__FILE__).parent_path().parent_path() / "CMakeLists.txt";

    std::ifstream cmakeInput(cmakePath);
    Check(cmakeInput.good(),
          "BDD: Given Auro runtime build contract validation runs, CMakeLists.txt can be opened");
    if (!cmakeInput.good()) {
        return;
    }

    std::ostringstream cmakeBuffer;
    cmakeBuffer << cmakeInput.rdbuf();
    const std::string cmakeSource = cmakeBuffer.str();
    const size_t winBlock = cmakeSource.find("if(WIN32)", cmakeSource.find("# Platform-specific libraries"));
    const size_t arisLink = cmakeSource.find("target_link_libraries(HarkenSearch PRIVATE", winBlock);
    const size_t appleBlock = cmakeSource.find("elseif(APPLE)", arisLink);
    const std::string arisWinLinkBlock =
        (arisLink != std::string::npos && appleBlock != std::string::npos && arisLink < appleBlock)
            ? cmakeSource.substr(arisLink, appleBlock - arisLink)
            : std::string();

    Check(!arisWinLinkBlock.empty(),
          "BDD: Aris Windows target link block is inspectable");
    Check(arisWinLinkBlock.find("mfplat") != std::string::npos &&
          arisWinLinkBlock.find("mfreadwrite") != std::string::npos &&
          arisWinLinkBlock.find("mfuuid") != std::string::npos,
          "BDD: Aris target explicitly links Media Foundation libraries required by AudioEngine/Database/WaveformExtractor");
}

void TestCoverWallReloadUsesSemanticEntrypointsContract() {
    const auto root = std::filesystem::path(__FILE__).parent_path().parent_path();
    const auto mainPath = root / "src" / "main.cpp";
    const auto appStatePath = root / "src" / "AppState.h";
    const auto refreshAdapterPath =
        root / "src" / "CoverWallRefreshStateAdapter.cpp";

    std::ifstream mainInput(mainPath);
    std::ifstream appStateInput(appStatePath);
    std::ifstream refreshAdapterInput(refreshAdapterPath);
    Check(mainInput.good(),
          "BDD: Given cover-wall reload semantic contract validation runs, main.cpp can be opened");
    Check(appStateInput.good(),
          "BDD: Given cover-wall reload semantic contract validation runs, AppState.h can be opened");
    Check(refreshAdapterInput.good(),
          "BDD: Given cover-wall reload semantic contract validation runs, CoverWallRefreshStateAdapter.cpp can be opened");
    if (!mainInput.good() || !appStateInput.good() || !refreshAdapterInput.good()) {
        return;
    }

    std::ostringstream mainBuffer;
    std::ostringstream appStateBuffer;
    std::ostringstream refreshAdapterBuffer;
    mainBuffer << mainInput.rdbuf();
    appStateBuffer << appStateInput.rdbuf();
    refreshAdapterBuffer << refreshAdapterInput.rdbuf();
    const std::string mainSource = mainBuffer.str();
    const std::string appStateSource = appStateBuffer.str();
    const std::string refreshAdapterSource = refreshAdapterBuffer.str();

    Check(appStateSource.find("coverWallDisplayRefreshRequested") != std::string::npos,
          "BDD: Display-only cover-wall refresh has its own AppState flag");
    Check(appStateSource.find("coverWallDatabaseInvalidationRequested") != std::string::npos,
          "BDD: Database-content cover-wall invalidation has its own AppState flag");
    Check(refreshAdapterSource.find("void RequestDisplayRefresh(AppState& state)") != std::string::npos &&
          refreshAdapterSource.find("void RequestDatabaseInvalidation(AppState& state)") != std::string::npos &&
          mainSource.find("aura::cover_wall::RequestDisplayRefresh") != std::string::npos,
          "BDD: Cover wall refresh events are requested through semantic entrypoints");

    size_t legacyTrueWrites = 0;
    for (size_t pos = refreshAdapterSource.find("needsCoverWallReload = true");
         pos != std::string::npos;
         pos = refreshAdapterSource.find("needsCoverWallReload = true", pos + 1)) {
        ++legacyTrueWrites;
    }
    const std::string displayBody = ExtractFunctionBody(
        refreshAdapterSource, "void RequestDisplayRefresh(AppState& state)");
    Check(mainSource.find("needsCoverWallReload = true") == std::string::npos &&
          legacyTrueWrites == 1 &&
          displayBody.find("state.needsCoverWallReload = true;") != std::string::npos,
          "BDD: Production code does not directly write the legacy cover-wall reload flag outside the display-refresh adapter");
}

void TestStartupCoverWallWarmupIsBoundedContract() {
    const auto root = std::filesystem::path(__FILE__).parent_path().parent_path();
    const auto windowsMainPath = root / "src" / "WindowsMain.cpp";
    const auto databasePath = root / "src" / "Database.cpp";

    std::ifstream windowsMainInput(windowsMainPath);
    std::ifstream databaseInput(databasePath);
    Check(windowsMainInput.good(),
          "BDD: Given startup cover-wall warmup contract validation runs, WindowsMain.cpp can be opened");
    Check(databaseInput.good(),
          "BDD: Given startup cover-wall warmup contract validation runs, Database.cpp can be opened");
    if (!windowsMainInput.good() || !databaseInput.good()) {
        return;
    }

    std::ostringstream windowsMainBuffer;
    std::ostringstream databaseBuffer;
    windowsMainBuffer << windowsMainInput.rdbuf();
    databaseBuffer << databaseInput.rdbuf();
    const std::string windowsMainSource = windowsMainBuffer.str();
    const std::string databaseSource = databaseBuffer.str();

    Check(windowsMainSource.find("state.preloadedCoverWallEntries = state.database.getGlobalCoverWallEntries();") == std::string::npos,
          "BDD: Startup splash does not synchronously run the global cover-wall query before entering the main UI");
    Check(windowsMainSource.find("BuildStartupFirstScreenCoverItemsFromFacetsForQa") != std::string::npos &&
          windowsMainSource.find("WaitForStartupCoverSourceForQa") != std::string::npos &&
          windowsMainSource.find("WarmStartupFirstScreenCoversForQa") != std::string::npos &&
          windowsMainSource.find("StartupFirstScreenPreloadPrimed candidates=%zu mirrored=%zu budgetElapsed=%d elapsedMs=%.2f") != std::string::npos &&
          windowsMainSource.find("kStartupCoverCandidatePoolCount = 4096") != std::string::npos &&
          windowsMainSource.find("kStartupVisibleCoverTarget = 12") != std::string::npos &&
          windowsMainSource.find("kStartupFirstScreenCoverCount = 24") != std::string::npos,
          "BDD: Startup keeps an explicit bounded first-screen cover-wall preload before exposing the main UI");
    const size_t betaSplashStart = windowsMainSource.find("static void RunBetaSplashScreen");
    const size_t webViewLoadingStart = windowsMainSource.find("static void RunWebView2LoadingScreen");
    Check(betaSplashStart != std::string::npos &&
          webViewLoadingStart != std::string::npos &&
          betaSplashStart < webViewLoadingStart &&
          windowsMainSource.substr(betaSplashStart, webViewLoadingStart - betaSplashStart).find("WaitForStartupCoverSourceForQa") != std::string::npos &&
          windowsMainSource.substr(betaSplashStart, webViewLoadingStart - betaSplashStart).find("WarmStartupFirstScreenCoversForQa") != std::string::npos &&
          windowsMainSource.substr(betaSplashStart, webViewLoadingStart - betaSplashStart).find("renderBetaSplashFrame(L(Splash_WarmFirstCover))") != std::string::npos,
          "BDD: Default beta splash startup path waits for cover facets and warms first-screen covers before restoring the main UI");
    const size_t startupWarmStart = windowsMainSource.find("static void WarmStartupFirstScreenCoversForQa");
    const size_t readyToWorkStart = windowsMainSource.find("static void EnsureReadyToWorkFirstScreenCoverCandidatesForQa");
    Check(startupWarmStart != std::string::npos &&
          readyToWorkStart != std::string::npos &&
          startupWarmStart < readyToWorkStart &&
          windowsMainSource.substr(startupWarmStart, readyToWorkStart - startupWarmStart).find("IsCoverIdentityKeyPendingOrFailed") == std::string::npos,
          "BDD: Startup first-screen warmup rechecks pending cover requests so uploaded textures can be mirrored before timeout");
    Check(startupWarmStart != std::string::npos &&
          readyToWorkStart != std::string::npos &&
          startupWarmStart < readyToWorkStart &&
          windowsMainSource.substr(startupWarmStart, readyToWorkStart - startupWarmStart).find("CoverWallShouldRecordConfirmedFailureForQa(item)") != std::string::npos &&
          windowsMainSource.substr(startupWarmStart, readyToWorkStart - startupWarmStart).find("readyCount >= readyTarget") != std::string::npos &&
          windowsMainSource.substr(startupWarmStart, readyToWorkStart - startupWarmStart).find("retryAfterFrame[item.key] = frame + 6") != std::string::npos,
          "BDD: Startup first-screen warmup heals failed cover candidates and releases after the visible target is ready");
    Check(windowsMainSource.find("StartupPersistentPreload skipped=deferred_to_main_ui") == std::string::npos,
          "BDD: Startup cover-wall diagnostics must not report the old deferred-to-main-UI preload contract");
    Check(windowsMainSource.find("startup_first_screen_preload") != std::string::npos,
          "BDD: Startup warmup state reports the explicit first-screen cover preload contract");
    Check(windowsMainSource.find("state.database.warmupCoverIndexes();") == std::string::npos,
          "BDD: Startup loading cannot synchronously warm cover indexes before the QA/main UI loop can run");
    Check(windowsMainSource.find("state.database.preheat();") == std::string::npos,
          "BDD: Startup loading cannot synchronously preheat the database before the QA/main UI loop can run");

    const size_t globalEntries = databaseSource.find("Database::getGlobalCoverWallEntries()");
    Check(globalEntries != std::string::npos,
          "BDD: Database::getGlobalCoverWallEntries source can be inspected for indexed live-key validation");
    Check(globalEntries != std::string::npos &&
          databaseSource.find("INDEXED BY idx_files_cover_dedup_emb", globalEntries) != std::string::npos &&
          databaseSource.find("INDEXED BY idx_files_cover_dedup_fold", globalEntries) != std::string::npos,
          "BDD: Global cover-wall live-key validation forces cover-key indexes instead of risking a large-library table scan");
}

void TestEmptyGlobalCoverPreloadDoesNotRequeryEveryFrameContract() {
    const auto sourcePath = std::filesystem::path(__FILE__).parent_path().parent_path() / "src" / "main.cpp";
    std::ifstream input(sourcePath);
    Check(input.good(),
          "BDD: Given empty-database cover preload contract validation runs, src/main.cpp can be opened");
    if (!input.good()) {
        return;
    }

    std::ostringstream buffer;
    buffer << input.rdbuf();
    const std::string source = buffer.str();
    const std::string renderBody = ExtractFunctionBody(
        source, "void RenderFileInfoPanelTabContents(");

    Check(!renderBody.empty(),
          "BDD: Empty-database cover preload contract can inspect the file-info panel render path");
    Check(renderBody.find("L(Cover_NoCoverImages)") != std::string::npos,
          "BDD: Artwork empty state keeps a single in-panel no-cover message");
    Check(renderBody.find("L(Cover_NoCoverData)") == std::string::npos,
          "BDD: Artwork empty state no longer duplicates an extra top-line no-cover banner");
    Check(renderBody.find("if (cw.preloadDbPath.empty() && !cw.preloadInFlight && state.totalFileCount > 0)") != std::string::npos,
          "BDD: Global cover-wall preload starts only once per DB generation and skips empty databases");
    Check(renderBody.find("if (cw.preloadFullList.empty() && !cw.preloadInFlight)") == std::string::npos,
          "BDD: Empty preload results are not treated as 'never attempted' and cannot requery every frame");
    Check(renderBody.find("getGlobalCoverWallEntriesPage") != std::string::npos,
          "BDD: Global cover-wall render path requests bounded pages instead of a full-library wall");
    Check(renderBody.find("getGlobalCoverWallEntries()") == std::string::npos,
          "BDD: Global cover-wall render path cannot regress to the unpaged full-library query");
    Check(renderBody.find("cw.globalPageNextCursor") != std::string::npos &&
          renderBody.find("cw.globalPageHasMore") != std::string::npos &&
          renderBody.find("cw.globalPageLoadingNext") != std::string::npos,
          "BDD: Global cover-wall render path persists page cursor, has-more state, and in-flight next-page state");
    Check(renderBody.find("TrimGlobalCoverWallPageWindow(cw);") == std::string::npos,
          "BDD: Global cover-wall render path no longer trims appended pages behind a hard session ceiling");
}

void TestCoverWallResetEntrypointsSeparateDatabaseInvalidationContract() {
    const auto root = std::filesystem::path(__FILE__).parent_path().parent_path();
    const auto sourcePath = root / "src" / "main.cpp";
    const auto refreshAdapterPath =
        root / "src" / "CoverWallRefreshStateAdapter.cpp";
    std::ifstream input(sourcePath);
    std::ifstream refreshAdapterInput(refreshAdapterPath);
    Check(input.good(),
          "BDD: Given cover-wall reset entrypoint contract validation runs, src/main.cpp can be opened");
    Check(refreshAdapterInput.good(),
          "BDD: Given cover-wall reset entrypoint contract validation runs, CoverWallRefreshStateAdapter.cpp can be opened");
    if (!input.good() || !refreshAdapterInput.good()) {
        return;
    }

    std::ostringstream buffer;
    std::ostringstream refreshAdapterBuffer;
    buffer << input.rdbuf();
    refreshAdapterBuffer << refreshAdapterInput.rdbuf();
    const std::string source = buffer.str();
    const std::string refreshAdapterSource = refreshAdapterBuffer.str();

    const std::string forceBody = ExtractFunctionBody(
        refreshAdapterSource, "void ForceCoverWallReload(AppState& state)");
    const std::string displayBody = ExtractFunctionBody(
        refreshAdapterSource, "void RequestDisplayRefresh(AppState& state)");
    const std::string showAllBody = ExtractFunctionBody(source, "static void ClearAllFiltersForShowAll(AppState& state)");

    Check(!forceBody.empty(),
          "BDD: Database-content cover-wall invalidation entrypoint is inspectable");
    Check(!displayBody.empty(),
          "BDD: Display-only cover-wall refresh entrypoint is inspectable");
    Check(!showAllBody.empty(),
          "BDD: Show All filter reset body is inspectable");

    Check(forceBody.find("state.invalidateCoverWallAfterDatabaseContentChange();") != std::string::npos,
          "BDD: Database-content cover-wall invalidation clears the full-library cache through AppState");
    Check(forceBody.find("state.coverWall.invalidateAfterDatabaseContentChange();") == std::string::npos,
          "BDD: Database-content cover-wall invalidation is not double-applied");
    Check(displayBody.find("state.needsCoverWallReload = true;") != std::string::npos &&
          displayBody.find("invalidateCoverWallAfterDatabaseContentChange") == std::string::npos &&
          displayBody.find("coverWall.invalidateAfterDatabaseContentChange") == std::string::npos,
          "BDD: Display-only cover-wall refresh does not invalidate full-library cache or generation");
    Check(showAllBody.find("aura::cover_wall::RequestDisplayRefresh(state);") != std::string::npos &&
          showAllBody.find("ForceCoverWallReload(state);") == std::string::npos,
          "BDD: Show All / clear filters refreshes the displayed wall without database-content invalidation");

    const size_t folderClick = source.find("state.uiState.pendingFolderPath = folder.path;");
    const size_t folderForce = source.find("ForceCoverWallReload(state);", folderClick);
    const size_t folderDisplay = source.find(
        "aura::cover_wall::RequestDisplayRefresh(state);", folderClick);
    Check(folderClick != std::string::npos &&
          folderDisplay != std::string::npos &&
          (folderForce == std::string::npos || folderDisplay < folderForce),
          "BDD: Folder filter click uses display refresh instead of resetting the full-library cover wall");

    Check(source.find("FinalizeFolderSubtreeDbRemoval") != std::string::npos &&
          source.find("ForceCoverWallReload(state);", source.find("FinalizeFolderSubtreeDbRemoval")) != std::string::npos,
          "BDD: Removing records from the database still invalidates the cover wall as database content changed");
    const size_t rebuildButton = source.find("ImGui::Button(L(Set_RebuildCoverIndex)");
    const size_t rebuildForce = source.find("ForceCoverWallReload(state);", rebuildButton);
    const size_t rebuildStart = source.find("StartCoverRebuild(state);", rebuildButton);
    Check(rebuildButton != std::string::npos &&
          rebuildForce != std::string::npos &&
          rebuildStart != std::string::npos &&
          rebuildForce < rebuildStart,
          "BDD: Explicit cover index rebuild remains a database-content invalidation path");
    const size_t rebuildDone = source.find("if (state.coverRebuildBatch.empty()) {");
    Check(rebuildDone != std::string::npos &&
          source.find("state.currentCoverFacets.clear();", rebuildDone) != std::string::npos &&
          source.find("state.currentPathFacets.clear();", rebuildDone) != std::string::npos &&
          source.find("RefreshSearchResults(state, true);", rebuildDone) != std::string::npos,
          "BDD: Cover rebuild completion clears stale facet truth and performs a full refresh so the wall cannot stay empty on old pagination state");
}

void TestQaScanCompletionRefreshesDatabaseViewContract() {
    const auto sourcePath = std::filesystem::path(__FILE__).parent_path().parent_path() / "src" / "WindowsMain.cpp";
    std::ifstream input(sourcePath);
    Check(input.good(),
          "BDD: Given QA scan completion contract validation runs, WindowsMain.cpp can be opened");
    if (!input.good()) {
        return;
    }

    std::ostringstream buffer;
    buffer << input.rdbuf();
    const std::string source = buffer.str();

    const size_t refreshBlock = source.find("if (state.needsDatabaseRefresh && !scanPopupOpen)");
    Check(refreshBlock != std::string::npos,
          "BDD: QA scan completion refresh block is inspectable");
    const size_t refreshCall = source.find("RefreshSearchResults(state, true);", refreshBlock);
    const size_t forceDbView = source.find("state.viewMode = ViewMode::Database;", refreshBlock);
    Check(forceDbView != std::string::npos &&
          refreshCall != std::string::npos &&
          forceDbView < refreshCall,
          "BDD: QA scan completion normalizes back to Database view before refreshing results");
    Check(source.find("case ViewMode::PlaylistLibrary: return \"playlist_library\";") != std::string::npos,
          "BDD: QA get_state exposes PlaylistLibrary instead of reporting unknown active_view");
}

void TestDeferredCoverBackfillRefreshesCoverWallAfterStoppedScanContract() {
    const auto root = std::filesystem::path(__FILE__).parent_path().parent_path();
    const auto workerPath = root / "src" / "BackfillWorkerPool.cpp";
    const auto windowsMainPath = root / "src" / "WindowsMain.cpp";

    std::ifstream workerInput(workerPath);
    std::ifstream windowsInput(windowsMainPath);
    Check(workerInput.good(),
          "BDD: Given deferred cover-backfill contract validation runs, BackfillWorkerPool.cpp can be opened");
    Check(windowsInput.good(),
          "BDD: Given deferred cover-backfill contract validation runs, WindowsMain.cpp can be opened");
    if (!workerInput.good() || !windowsInput.good()) {
        return;
    }

    std::ostringstream workerBuffer;
    std::ostringstream windowsBuffer;
    workerBuffer << workerInput.rdbuf();
    windowsBuffer << windowsInput.rdbuf();
    const std::string workerSource = workerBuffer.str();
    const std::string windowsSource = windowsBuffer.str();

    const std::string coverJobBody = ExtractFunctionBody(
        workerSource, "bool processCoverJob(Database& db,");
    Check(!coverJobBody.empty(),
          "BDD: Deferred cover-backfill job body is inspectable");
    Check(workerSource.find("ConsumeCoverBackfillUiRefresh") != std::string::npos,
          "BDD: Deferred cover-backfill exposes a main-thread consumable refresh signal");
    Check(coverJobBody.find("RequestCoverBackfillUiRefresh();") != std::string::npos,
          "BDD: When cover backfill writes cover keys or registry rows, it requests a cover-wall refresh signal");
    Check(windowsSource.find("aura::ConsumeCoverBackfillUiRefresh()") != std::string::npos,
          "BDD: Main UI loop consumes cover-backfill refresh signals from worker DB connections");
    const size_t consume = windowsSource.find("aura::ConsumeCoverBackfillUiRefresh()");
    const size_t backfillBranch = windowsSource.find("if (backfillCoverRefresh) {", consume);
    const size_t backfillBranchEnd = windowsSource.find("}", backfillBranch);
    const std::string backfillBranchBody =
        (backfillBranch != std::string::npos && backfillBranchEnd != std::string::npos)
            ? windowsSource.substr(backfillBranch, backfillBranchEnd - backfillBranch)
            : std::string();
    Check(!backfillBranchBody.empty() &&
          backfillBranchBody.find("state.requestCoverWallBackfillDisplayRefresh();") != std::string::npos &&
          backfillBranchBody.find("state.invalidateCoverWallAfterDatabaseContentChange();") == std::string::npos,
          "BDD: Consuming deferred cover-backfill refresh uses the lightweight cover-wall display refresh path");
}

void TestFilteredCoverWallFallbackUsesTemporaryFolderKeysWhenRowsAlreadyHaveCoverContract() {
    const auto builderPath = std::filesystem::path(__FILE__).parent_path().parent_path() /
        "src" / "CoverCachedKeyItemBuilder.cpp";
    std::ifstream input(builderPath);
    Check(input.good(),
          "BDD: Given filtered cover-wall fallback contract validation runs, CoverCachedKeyItemBuilder.cpp can be opened");
    if (!input.good()) {
        return;
    }

    std::ostringstream buffer;
    buffer << input.rdbuf();
    const std::string source = buffer.str();
    const std::string body = ExtractFunctionBody(
        source, "void BuildCoverWallItemsFromCachedKeys(");

    Check(!body.empty(),
          "BDD: Filtered cover-wall fallback builder body is inspectable");
    Check(body.find("record.hasCover") != std::string::npos &&
          body.find("std::string(\"?|\") + HarkenSearch::CanonicalFolderPath(record.folderPath)") != std::string::npos,
          "BDD: When filtered rows already report folder cover presence but no cached key yet, cover wall emits temporary folder-cover identities");
    Check(body.find("CoverModule::TryGetFolderCoverKeyFast(") != std::string::npos &&
          body.find("record.folderPath, folderKey, &folderCoverPath") != std::string::npos,
          "BDD: Filtered cover-wall fallback still prefers cached DB/memory folder-cover identity before temporary placeholders");
}

void TestFilteredCoverWallAsyncFacetLimitMatchesUiContract() {
    const auto root = std::filesystem::path(__FILE__).parent_path().parent_path();
    const auto mainPath = root / "src" / "main.cpp";
    const auto workerPath = root / "src" / "AsyncWorker.cpp";
    const auto facetsAdapterPath = root / "src" / "AsyncSearchRefreshFacetsAdapter.cpp";
    const auto stateAdapterPath = root / "src" / "AsyncSearchRefreshFacetsStateAdapter.cpp";

    std::ifstream mainInput(mainPath);
    std::ifstream workerInput(workerPath);
    std::ifstream facetsAdapterInput(facetsAdapterPath);
    std::ifstream stateAdapterInput(stateAdapterPath);
    Check(mainInput.good(),
          "BDD: Given filtered cover-wall facet-limit contract validation runs, main.cpp can be opened");
    Check(workerInput.good(),
          "BDD: Given filtered cover-wall facet-limit contract validation runs, AsyncWorker.cpp can be opened");
    Check(facetsAdapterInput.good(),
          "BDD: Given filtered cover-wall facet-limit contract validation runs, facets adapter can be opened");
    Check(stateAdapterInput.good(),
          "BDD: Given filtered cover-wall facet-limit contract validation runs, state adapter can be opened");
    if (!mainInput.good() || !workerInput.good() || !facetsAdapterInput.good() || !stateAdapterInput.good()) {
        return;
    }

    std::ostringstream mainBuffer;
    std::ostringstream workerBuffer;
    std::ostringstream facetsAdapterBuffer;
    std::ostringstream stateAdapterBuffer;
    mainBuffer << mainInput.rdbuf();
    workerBuffer << workerInput.rdbuf();
    facetsAdapterBuffer << facetsAdapterInput.rdbuf();
    stateAdapterBuffer << stateAdapterInput.rdbuf();
    const std::string mainSource = mainBuffer.str();
    const std::string workerSource = workerBuffer.str();
    const std::string facetsAdapterSource = facetsAdapterBuffer.str();
    const std::string stateAdapterSource = stateAdapterBuffer.str();
    const auto compositionPath =
        std::filesystem::path(__FILE__).parent_path().parent_path() /
        "src" / "AsyncSearchRefreshResultCompositionAdapter.cpp";
    std::ifstream compositionInput(compositionPath);
    std::ostringstream compositionBuffer;
    compositionBuffer << compositionInput.rdbuf();
    const std::string compositionSource = compositionBuffer.str();

    Check(facetsAdapterSource.find("HandleAsyncSearchRefreshFacets(") != std::string::npos &&
          stateAdapterSource.find("facetsAdapter.apply()") == std::string::npos &&
          compositionSource.find("facetsAdapter.apply();") != std::string::npos &&
          mainSource.find("state.database.getCoverFacets(filter,") == std::string::npos,
          "BDD: Filtered cover-wall UI consumes async facets instead of issuing a second UI-thread database query");
    Check(workerSource.find("constexpr int kFilteredCoverWallFacetLimit = 10000;") != std::string::npos,
          "BDD: Async search worker defines one shared filtered cover-wall facet limit instead of ad-hoc literals");
    Check(workerSource.find("m_db->getCoverFacets(req.filter, kFilteredCoverWallFacetLimit)") != std::string::npos,
          "BDD: Async filtered cover-wall facet fetch uses the widened shared limit");
    Check(workerSource.find("m_db->getCoverFacets(req.filter, 1200)") == std::string::npos,
          "BDD: Async filtered cover-wall facet fetch no longer truncates folder/path walls at the legacy 1200 cap");
}

void TestFilteredCoverWallFacetSwapAvoidsSecondProgressiveFillContract() {
    const auto root = std::filesystem::path(__FILE__).parent_path().parent_path();
    const auto mainPath = root / "src" / "main.cpp";
    const auto facetPolicyPath = root / "src" / "CoverWallFacetRefreshPolicy.cpp";
    std::ifstream input(mainPath);
    Check(input.good(),
          "BDD: Given filtered cover-wall stable-swap contract validation runs, main.cpp can be opened");
    if (!input.good()) {
        return;
    }

    std::ostringstream buffer;
    buffer << input.rdbuf();
    const std::string source = buffer.str();
    const std::string facetPolicySource = ReadTextFile(facetPolicyPath);

    Check(facetPolicySource.find("void RefreshCoverWallFacetMetadataInPlace(") != std::string::npos &&
          facetPolicySource.find("CoverWallItemsShareVisualOrder(existing, updated)") != std::string::npos,
          "BDD: Filtered cover-wall can refresh facet metadata in place when the visual set is already stable");
    Check(source.find("RefreshCoverWallFacetMetadataInPlace(cw.items, nextFacetItems);") != std::string::npos,
          "BDD: Filtered cover-wall applies the final facet payload through one stable swap instead of clearing and refilling the wall");
    Check(source.find("if (!holdStableFacetWallUntilFollowUp || !state.currentCoverFacets.empty())") != std::string::npos &&
          source.find("cw.pendingFacetItems.clear();\n                            cw.pendingFacetCursor = cw.items.size();") != std::string::npos,
          "BDD: Filtered cover-wall clears the pending progressive-fill queue only when a stable visual swap is allowed");
}

void TestGlobalCoverWallNoFilterLoadAvoidsFallbackFlashContract() {
    const auto mainPath = std::filesystem::path(__FILE__).parent_path().parent_path() / "src" / "main.cpp";
    std::ifstream input(mainPath);
    Check(input.good(),
          "BDD: Given global no-filter stable-load contract validation runs, main.cpp can be opened");
    if (!input.good()) {
        return;
    }

    std::ostringstream buffer;
    buffer << input.rdbuf();
    const std::string source = buffer.str();
    const std::string renderBody = ExtractFunctionBody(
        source, "void RenderFileInfoPanelTabContents(");

    Check(renderBody.find("bool globalPageLoadingWithoutStableItems = false;") != std::string::npos,
          "BDD: Global cover-wall render path tracks whether the no-filter page is still loading without a stable wall");
    Check(renderBody.find("globalPageLoadingWithoutStableItems =\n                                cw.entriesInFlight || cw.loading || cw.preloadInFlight;") != std::string::npos,
          "BDD: Global no-filter cover wall explicitly distinguishes a loading wall from a real fallback result set");
    Check(renderBody.find("setDisplayItems(&state.coverWall.fallbackItems, CoverWallSource::SearchFallback);") != std::string::npos,
          "BDD: SearchFallback remains available for real filtered fallback paths");
    Check(renderBody.find("if (globalNotReadyYet) {") != std::string::npos &&
          renderBody.find("setDisplayItems(&cw.items, CoverWallSource::Global);") != std::string::npos,
          "BDD: Global no-filter cover wall keeps the global source active while the paged wall is still loading");
}

void TestQaKeywordSearchAvoidsBlockingImmediatePumpContract() {
    const auto sourcePath = std::filesystem::path(__FILE__).parent_path().parent_path() / "src" / "WindowsMain.cpp";
    std::ifstream input(sourcePath);
    Check(input.good(),
          "BDD: Given QA keyword-search perf contract validation runs, WindowsMain.cpp can be opened");
    if (!input.good()) {
        return;
    }

    std::ostringstream buffer;
    buffer << input.rdbuf();
    const std::string source = buffer.str();

    const std::string commandBody = ExtractElseIfCommandBody(source, "search");
    Check(!commandBody.empty(),
          "BDD: QA keyword-search perf contract can inspect the search command handler");
    Check(commandBody.find("state.preferDeferredCountForNextRefresh = true;") != std::string::npos,
          "BDD: QA keyword-search follows the deferred-count path so perf probes match a just-typed user search");
    Check(commandBody.find("state.suppressNextRealtimeSearchCommit = true;") != std::string::npos,
          "BDD: QA keyword-search suppresses the next realtime debounce commit so CLI probes do not double-submit the same keyword");
    Check(commandBody.find("state.suppressFacetsForNextRefresh = true;") == std::string::npos,
          "BDD: QA keyword-search does not suppress facet follow-up, so regression probes can observe the real final cover wall");
    Check(commandBody.find("pumpImmediateSearchForQa();") == std::string::npos,
          "BDD: QA keyword-search does not block the UI thread by spinning for async results inside the command handler");
}

void TestFilterChipQueryRemovalSuppressesRealtimeAutoClearContract() {
    const auto sourcePath = std::filesystem::path(__FILE__).parent_path().parent_path() / "src" / "main.cpp";
    std::ifstream input(sourcePath);
    Check(static_cast<bool>(input),
          "BDD: filter-chip query-removal contract can read main.cpp");
    if (!input) return;

    std::ostringstream buffer;
    buffer << input.rdbuf();
    const std::string source = buffer.str();

    const std::string anchor =
        "std::string rebuilt = RemoveSearchQueryChipFromText(";
    const size_t anchorPos = source.find(anchor);
    Check(anchorPos != std::string::npos,
          "BDD: query chip removal still uses RemoveSearchQueryChipFromText");
    if (anchorPos == std::string::npos) return;

    const size_t windowEnd = source.find("strncpy_s(state.searchBuffer", anchorPos);
    Check(windowEnd != std::string::npos,
          "BDD: query chip removal still rewrites searchBuffer directly");
    if (windowEnd == std::string::npos) return;

    const std::string window = source.substr(anchorPos, windowEnd - anchorPos);
    Check(window.find("state.suppressNextRealtimeSearchCommit = true;") != std::string::npos,
          "BDD: query chip removal suppresses the next realtime debounce commit so cover/path filters are not auto-cleared");
}

void TestQaPathAndMetadataFiltersDoNotSuppressFacetFollowupContract() {
    const auto sourcePath = std::filesystem::path(__FILE__).parent_path().parent_path() / "src" / "WindowsMain.cpp";
    std::ifstream input(sourcePath);
    Check(input.good(),
          "BDD: Given QA filter follow-up contract validation runs, WindowsMain.cpp can be opened");
    if (!input.good()) {
        return;
    }

    std::ostringstream buffer;
    buffer << input.rdbuf();
    const std::string source = buffer.str();

    const std::string folderBody = ExtractElseIfCommandBody(source, "set_folder_filter");
    const std::string metadataBody = ExtractElseIfCommandBody(source, "set_metadata_filter");
    const std::string coverBody = ExtractElseIfCommandBody(source, "set_cover_filter");

    Check(folderBody.find("state.suppressFacetsForNextRefresh = true;") == std::string::npos,
          "BDD: QA path-filter command keeps facet follow-up enabled so filtered cover-wall probes reach the real final wall");
    Check(metadataBody.find("state.suppressFacetsForNextRefresh = true;") == std::string::npos,
          "BDD: QA metadata-filter command keeps facet follow-up enabled so filtered cover-wall probes reach the real final wall");
    Check(coverBody.find("state.suppressFacetsForNextRefresh = true;") == std::string::npos,
          "BDD: QA cover-filter command keeps facet follow-up enabled so filtered cover-wall probes reach the real final wall");
    Check(folderBody.find("cancelPendingSearchForQaMutation();") != std::string::npos &&
          metadataBody.find("cancelPendingSearchForQaMutation();") != std::string::npos &&
          coverBody.find("cancelPendingSearchForQaMutation();") != std::string::npos,
          "BDD: QA filter commands clear stale startup/search follow-up work before submitting a new filtered cover-wall probe");
}

void TestQaBrowseSurfaceCommandsPreserveReplacementScopeContract() {
    const auto sourcePath = std::filesystem::path(__FILE__).parent_path().parent_path() / "src" / "WindowsMain.cpp";
    std::ifstream input(sourcePath);
    Check(input.good(),
          "BDD: Given QA browse-surface contract validation runs, WindowsMain.cpp can be opened");
    if (!input.good()) {
        return;
    }

    std::ostringstream buffer;
    buffer << input.rdbuf();
    const std::string source = buffer.str();
    const std::string specificCoverBody = ExtractElseIfCommandBody(source, "set_specific_cover_filter");
    const std::string thumbSizeBody = ExtractElseIfCommandBody(source, "set_cover_wall_thumb_size");
    const std::string tilePresetBody = ExtractElseIfCommandBody(source, "set_cover_wall_tile_preset");

    Check(specificCoverBody.find("state.coverFilterActive = true;") != std::string::npos &&
          specificCoverBody.find("state.coverFilterPath = key;") != std::string::npos &&
          specificCoverBody.find("state.folderFilterActive = false;") != std::string::npos &&
          specificCoverBody.find("state.selectedFolderPath.clear();") != std::string::npos &&
          specificCoverBody.find("state.coverWall.pinnedCoverFilterItem.key = key;") != std::string::npos &&
          specificCoverBody.find("cancelPendingSearchForQaMutation();") != std::string::npos &&
          specificCoverBody.find("RefreshSearchResults(state, true);") != std::string::npos,
          "BDD: QA specific-cover filter mutation keeps the replacement wall on one active cover scope and submits it through the normal refresh path");
    Check(thumbSizeBody.find("aura::coverwall::ClampThumbSize(requestedSize)") != std::string::npos,
          "BDD: QA thumb-size mutation clamps browse-surface tile size through the shared replacement helper");
    Check(tilePresetBody.find("aura::coverwall::ApplyThumbPreset(sizePreset, state.coverWall.thumbSize);") != std::string::npos,
          "BDD: QA tile-preset mutation uses the shared browse-surface preset helper instead of ad hoc render-side size math");
    Check(source.find("folder_path") != std::string::npos &&
          source.find("embedded_cover_key") != std::string::npos &&
          source.find("folder_cover_key") != std::string::npos &&
          source.find("cover_key") != std::string::npos,
          "BDD: QA row snapshots expose folder and cover identity fields so Slice 3 probes can resolve tree and preview parity from runtime data");
}

void TestFilteredRefreshDropsStaleFacetsUntilFollowupArrivesContract() {
    const auto mainPath = std::filesystem::path(__FILE__).parent_path().parent_path() / "src" / "main.cpp";
    std::ifstream input(mainPath);
    Check(input.good(),
          "BDD: Given filtered refresh stale-facet contract validation runs, main.cpp can be opened");
    if (!input.good()) {
        return;
    }

    std::ostringstream buffer;
    buffer << input.rdbuf();
    const std::string source = buffer.str();
    const std::string applyBody = ExtractFunctionBody(source, "static void ApplyAsyncSearchResult(AppState& state, AsyncSearchResult& result)");

    Check(applyBody.find("const bool awaitingFacetFollowUp =\n                !coverFilterActive &&\n                state.pendingFacetRequestId == result.requestId;") != std::string::npos,
          "BDD: Filtered async refresh explicitly detects when the final facet follow-up is still pending");
    Check(applyBody.find("const bool shouldDropStaleFacetsForFallback =\n                awaitingFacetFollowUp &&\n                !hasFacetPayload &&\n                !definitelyNoResults &&\n                HasActiveDbFilterForCoverWall(state);") != std::string::npos,
          "BDD: Filtered async refresh drops stale facets when a new filtered result set must temporarily fall back to searchResults");
    Check(applyBody.find("if (hasFacetPayload || definitelyNoResults || coverFilterActive || shouldDropStaleFacetsForFallback)") != std::string::npos,
          "BDD: Filtered async refresh routes stale-facet clearing through the same facet-generation/display-refresh path as real facet payloads");
    Check(applyBody.find("state.database.persistCoverFacetCache(result.filter, 10000, state.currentCoverFacets);") != std::string::npos &&
          applyBody.find("state.database.persistPathFacetCache(result.filter, 300, state.currentPathFacets);") != std::string::npos,
          "BDD: Filtered async facet payloads are persisted through the writable primary DB connection keyed by the submitted async filter instead of the live UI state");
}

void TestFilteredRefreshClearsStaleFacetsAtSubmitTimeContract() {
    const auto mainPath = std::filesystem::path(__FILE__).parent_path().parent_path() / "src" / "main.cpp";
    std::ifstream input(mainPath);
    Check(input.good(),
          "BDD: Given filtered refresh submit-time stale-facet contract validation runs, main.cpp can be opened");
    if (!input.good()) {
        return;
    }

    std::ostringstream buffer;
    buffer << input.rdbuf();
    const std::string source = buffer.str();
    const std::string refreshBody = ExtractFunctionBody(source, "void RefreshSearchResults(AppState& state, bool resetPagination)");

    Check(refreshBody.find("const bool filteredCoverWallRefresh =\n        resetPagination &&\n        state.viewMode == ViewMode::Database &&\n        !coverFilterActive &&\n        HasActiveDbFilterForCoverWall(state);") != std::string::npos,
          "BDD: Filtered refresh submit path explicitly detects when a new DB filter should drop the old facet wall immediately");
    Check(refreshBody.find("if (filteredCoverWallRefresh) {") != std::string::npos &&
          refreshBody.find("state.currentCoverFacets.clear();") != std::string::npos &&
          refreshBody.find("if (!state.preservePathFacetsForNextRefresh) {") != std::string::npos &&
          refreshBody.find("state.currentPathFacets.clear();") != std::string::npos &&
          refreshBody.find("state.bumpFacetsGen();") != std::string::npos &&
          refreshBody.find("aura::cover_wall::RequestDisplayRefresh(state);") != std::string::npos,
          "BDD: Filtered refresh submit path clears stale facets through the normal generation/display-refresh path before async results land");
    Check(refreshBody.find("const bool filteredCoverWallFastFirstPaint =\n        filteredCoverWallRefresh &&\n        (state.folderFilterActive ||\n         state.searchBuffer[0] != '\\0' ||\n         HasAdvancedDbFilters(state));") != std::string::npos,
          "BDD: Filtered refresh submit path marks folder/keyword/advanced DB filters as cover-wall first-paint candidates");
    Check(refreshBody.find("const bool deferCount = filteredCoverWallFastFirstPaint ||") != std::string::npos,
          "BDD: Filtered cover-wall fast first paint defers the heavy count/list lane behind the facet-first path");
}

void TestFilteredCoverWallDoesNotFlashSearchFallbackWhileFacetFollowupPendingContract() {
    const auto mainPath = std::filesystem::path(__FILE__).parent_path().parent_path() / "src" / "main.cpp";
    std::ifstream input(mainPath);
    Check(input.good(),
          "BDD: Given filtered cover-wall no-flash contract validation runs, main.cpp can be opened");
    if (!input.good()) {
        return;
    }

    std::ostringstream buffer;
    buffer << input.rdbuf();
    const std::string source = buffer.str();
    const std::string renderBody = ExtractFunctionBody(
        source, "void RenderFileInfoPanelTabContents(");

    Check(renderBody.find("const bool shouldHoldFacetWallUntilFollowUp =\n                        facetNotReady &&\n                        facetLoading;") != std::string::npos,
          "BDD: Filtered cover-wall explicitly distinguishes an in-flight facet follow-up from a genuine fallback-ready empty wall");
    Check(renderBody.find("const bool holdStableFacetWallUntilFollowUp =\n                        facetLoading &&\n                        hasActiveFilter &&\n                        state.viewMode == ViewMode::Database &&\n                        state.currentCoverFacets.empty() &&\n                        !cw.items.empty();") != std::string::npos,
          "BDD: Filtered cover-wall keeps an existing stable visual wall while the authoritative facet follow-up is still pending");
    Check(renderBody.find("if (facetLoading &&\n                        !holdStableFacetWallUntilFollowUp &&") != std::string::npos,
          "BDD: Filtered cover-wall does not overwrite a stable wall with a transient search-results preview during facet follow-up");
    Check(renderBody.find("if (!holdStableFacetWallUntilFollowUp || !state.currentCoverFacets.empty())") != std::string::npos,
          "BDD: Filtered cover-wall only swaps visual items during follow-up when authoritative facet items are available");
    Check(renderBody.find("if (facetNotReady && !shouldHoldFacetWallUntilFollowUp)") != std::string::npos,
          "BDD: Search-result fallback is only allowed after the facet follow-up stopgap no longer applies");
    Check(renderBody.find("setDisplayItems(&cw.items, CoverWallSource::Facet);") != std::string::npos,
          "BDD: A freshly switched filtered wall stays on the stable facet surface instead of flashing a search fallback before deduped facets arrive");
}

void TestFilteredCoverWallRetirementNarrowsLegacySearchFallbackContract() {
    const auto appStatePath = std::filesystem::path(__FILE__).parent_path().parent_path() / "src" / "AppState.h";
    std::ifstream appStateInput(appStatePath);
    Check(appStateInput.good(),
          "BDD: Given filtered cover-wall retirement validation runs, AppState.h can be opened");
    if (!appStateInput.good()) {
        return;
    }

    std::ostringstream appStateBuffer;
    appStateBuffer << appStateInput.rdbuf();
    const std::string appStateSource = appStateBuffer.str();
    Check(appStateSource.find("bool lastPreviewHadVisibleItems = false;") != std::string::npos,
          "BDD: Cover-wall state tracks whether the current filter already has a visible preview wall before legacy fallback is considered");

    const auto mainPath = std::filesystem::path(__FILE__).parent_path().parent_path() / "src" / "main.cpp";
    std::ifstream input(mainPath);
    Check(input.good(),
          "BDD: Given filtered cover-wall retirement validation runs, main.cpp can be opened");
    if (!input.good()) {
        return;
    }

    std::ostringstream buffer;
    buffer << input.rdbuf();
    const std::string source = buffer.str();
    const std::string renderBody = ExtractFunctionBody(
        source, "void RenderFileInfoPanelTabContents(");

    Check(renderBody.find("cw.lastPreviewHadVisibleItems = false;") != std::string::npos &&
          renderBody.find("cw.lastPreviewHadVisibleItems = true;") != std::string::npos,
          "BDD: Filtered cover-wall preview generation records whether the current filter already built a visible preview wall");
    Check(renderBody.find("const bool hasCurrentPreviewWall =") != std::string::npos &&
          renderBody.find("cw.lastPreviewHadVisibleItems &&") != std::string::npos,
          "BDD: Retirement logic distinguishes a current preview wall from a generic old wall before allowing legacy SearchFallback");
    Check(renderBody.find("const bool allowLegacySearchFallback = !hasCurrentPreviewWall;") != std::string::npos,
          "BDD: Legacy SearchFallback is now explicitly quarantined behind a no-current-preview-wall gate");

    const auto databaseHeaderPath = std::filesystem::path(__FILE__).parent_path().parent_path() / "src" / "Database.h";
    const auto databaseSourcePath = std::filesystem::path(__FILE__).parent_path().parent_path() / "src" / "Database.cpp";
    const auto coverModulePath = std::filesystem::path(__FILE__).parent_path().parent_path() / "src" / "CoverModule.cpp";
    std::ifstream databaseHeaderInput(databaseHeaderPath);
    std::ifstream databaseSourceInput(databaseSourcePath);
    std::ifstream coverModuleInput(coverModulePath);
    Check(databaseHeaderInput.good() && databaseSourceInput.good() && coverModuleInput.good(),
          "BDD: Given cover-key bridge retirement validation runs, Database.h, Database.cpp, and CoverModule.cpp can be opened");
    if (!databaseHeaderInput.good() || !databaseSourceInput.good() || !coverModuleInput.good()) {
        return;
    }

    std::ostringstream databaseHeaderBuffer;
    std::ostringstream databaseSourceBuffer;
    std::ostringstream coverModuleBuffer;
    databaseHeaderBuffer << databaseHeaderInput.rdbuf();
    databaseSourceBuffer << databaseSourceInput.rdbuf();
    coverModuleBuffer << coverModuleInput.rdbuf();
    const std::string databaseHeaderSource = databaseHeaderBuffer.str();
    const std::string databaseSource = databaseSourceBuffer.str();
    const std::string coverModuleSource = coverModuleBuffer.str();

    Check(databaseHeaderSource.find("getFilePathsForCoverCompatibilityBridge") == std::string::npos &&
          databaseHeaderSource.find("getFileIdsForCoverPlaylistCompatibilityBridge") == std::string::npos &&
          databaseHeaderSource.find("getFileIdsForCoverTileMembership") != std::string::npos &&
          databaseHeaderSource.find("not display truth or texture recovery") != std::string::npos,
          "BDD: DB cover-key compatibility bridge names are retired while replacement tile membership remains explicit");
    Check(source.find("getFileIdsForCoverPlaylistCompatibilityBridge") == std::string::npos &&
          source.find("getFileIdsForCoverTileMembership(tileIdentityKey)") != std::string::npos &&
          coverModuleSource.find("getFilePathsForCoverCompatibilityBridge(") == std::string::npos,
          "BDD: Production artwork drag/drop uses replacement tile membership; embedded recovery no longer uses file-path bridge");
    const std::string tileMembershipBody =
        ExtractFunctionBody(databaseSource, "std::vector<int64_t> Database::getFileIdsForCoverTileMembership");
    const std::string buildSearchWhereBody =
        ExtractFunctionBody(databaseSource, "Database::BuiltSearchQuery Database::buildSearchWhere");
    Check(databaseSource.find("CREATE TABLE IF NOT EXISTS cover_tile_membership") != std::string::npos &&
          databaseSource.find("void Database::rebuildCoverTileMembershipIndex()") != std::string::npos,
          "BDD: Replacement tile membership has a canonical DB index that can be rebuilt independently of legacy audio cover-key fields");
    Check(databaseSource.find("LoadCoverTileMembershipIdsForKeys") != std::string::npos &&
          databaseSource.find("EnsureCoverTileMembershipForKeys") != std::string::npos &&
          databaseSource.find("SELECT DISTINCT file_id FROM cover_tile_membership") != std::string::npos &&
          tileMembershipBody.find("ResolveEquivalentCoverKeys(this, lookupKey)") != std::string::npos &&
          tileMembershipBody.find("EnsureCoverTileMembershipForKeys(m_db, equivalentKeys, &out)") != std::string::npos &&
          databaseSource.find("SyncCoverTileMembershipForKeys(db, missingKeys)") != std::string::npos &&
          databaseSource.find("if (sqlite3_db_readonly(db, \"main\") == 1) return !target->empty();") != std::string::npos &&
          tileMembershipBody.find("Tile membership unresolved after canonical lookup/self-heal; legacy field fallback fully retired.") != std::string::npos &&
          tileMembershipBody.find("embedded_cover_key IN (") == std::string::npos &&
          tileMembershipBody.find("folder_cover_key IN (") == std::string::npos,
          "BDD: Replacement tile membership reads and self-heals the canonical membership index first; the legacy audio-field fallback is fully retired");
    Check(buildSearchWhereBody.find("SELECT DISTINCT file_id FROM cover_tile_membership WHERE tile_key IN (") != std::string::npos &&
          buildSearchWhereBody.find("SELECT id FROM audio_files WHERE folder_cover_key = ?") == std::string::npos &&
          buildSearchWhereBody.find("SELECT id FROM audio_files WHERE embedded_cover_key = ?") == std::string::npos &&
          buildSearchWhereBody.find("UNION SELECT id FROM audio_files WHERE folder_cover_key IN (") == std::string::npos,
          "BDD: specificCoverKey search/filter routes through canonical tile membership instead of direct legacy cover-key field queries");
    Check(databaseHeaderSource.find("isCoverTileMembershipMaintenancePending()") != std::string::npos &&
          databaseHeaderSource.find("repairCoverTileMembershipBatch(int batchSize)") != std::string::npos &&
          databaseSource.find("cover_tile_membership_needs_maintenance") != std::string::npos &&
          databaseSource.find("Cover tile membership maintenance armed during open") != std::string::npos &&
          databaseSource.find("CoverTileMembershipRepairBatch") != std::string::npos &&
          databaseSource.find("repairCoverTileMembershipBatch(512)") != std::string::npos,
          "BDD: Legacy membership retirement keeps an explicit maintenance marker and background repair lane for pre-index libraries");
    Check(source.find("getFilePathsForCoverCompatibilityBridge(") == std::string::npos &&
          coverModuleSource.find("getFilePathsForCoverCompatibilityBridge(") == std::string::npos,
          "BDD: Production code no longer routes cover work through the file-path compatibility bridge");
    Check(source.find("getFilePathsWithCoverKey(") == std::string::npos &&
          coverModuleSource.find("getFilePathsWithCoverKey(") == std::string::npos,
          "BDD: Production UI and cover decode paths no longer call the ambiguous legacy cover-key reverse lookup name");
}

void TestFilteredCoverWallFastFirstPaintWarmsFacetsBeforePagedListContract() {
    const auto workerPath = std::filesystem::path(__FILE__).parent_path().parent_path() / "src" / "AsyncWorker.cpp";
    std::ifstream input(workerPath);
    Check(input.good(),
          "BDD: Given filtered cover-wall warm-start contract validation runs, AsyncWorker.cpp can be opened");
    if (!input.good()) {
        return;
    }

    std::ostringstream buffer;
    buffer << input.rdbuf();
    const std::string source = buffer.str();
    const std::string workerBody = ExtractFunctionBody(source, "void SearchWorker::workerThread()");

    Check(workerBody.find("const bool filteredFacetWarmStart =") != std::string::npos,
          "BDD: Filtered cover-wall refresh explicitly detects the facet-first warm-start path");
    Check(workerBody.find("const bool hasKeywordFilter = !req.filter.keyword.empty();") != std::string::npos &&
          workerBody.find("const bool hasStructuralFilter =") != std::string::npos,
          "BDD: Filtered cover-wall warm-start explicitly separates typed keyword filters from structural DB filters");
    Check(workerBody.find("!hasKeywordFilter &&\n                    hasStructuralFilter;") != std::string::npos,
          "BDD: Typed keyword searches keep a rows-first path while folder, advanced, and file-type DB filters can still warm-start facets");
    Check(workerBody.find("warmStart.kind = AsyncSearchKind::CountAndFacetsOnly;") != std::string::npos,
          "BDD: Filtered cover-wall warm-start publishes count+facets through the normal follow-up channel");
    Check(workerBody.find("result.filter = req.filter;") != std::string::npos &&
          workerBody.find("warmStart.filter = req.filter;") != std::string::npos &&
          workerBody.find("followup.filter = req.filter;") != std::string::npos,
          "BDD: Async search list, warm-start, and facet follow-up payloads all carry the submitted filter back to the apply site");
    Check(workerBody.find("warmStart.totalCount = m_db->searchFilesCount(req.filter);") != std::string::npos &&
          workerBody.find("warmStart.coverFacets = m_db->getCoverFacets(req.filter, kFilteredCoverWallFacetLimit);") != std::string::npos,
          "BDD: Filtered cover-wall warm-start computes the cheap count/facet payload before the paged list query");
    Check(workerBody.find("AppendDbQueryPerfLog(\"[DBQuery] searchFiles (facet warm-start): %.2f ms, items=%d, total=%lld\\n\",") != std::string::npos,
          "BDD: Filtered cover-wall warm-start keeps an explicit diagnostics lane for the delayed paged-list query");
}

void TestCoverWallWheelScrollCountsAsActiveInteractionContract() {
    const auto mainPath = std::filesystem::path(__FILE__).parent_path().parent_path() / "src" / "main.cpp";
    std::ifstream input(mainPath);
    Check(input.good(),
          "BDD: Given cover-wall wheel-interaction contract validation runs, main.cpp can be opened");
    if (!input.good()) {
        return;
    }

    std::ostringstream buffer;
    buffer << input.rdbuf();
    const std::string source = buffer.str();
    const std::string renderBody = ExtractFunctionBody(
        source, "void RenderFileInfoPanelTabContents(");

    Check(renderBody.find("const bool wheelScrolling =\n                        std::abs(io.MouseWheel) > 0.0f || std::abs(io.MouseWheelH) > 0.0f;") != std::string::npos,
          "BDD: Cover wall explicitly detects mouse-wheel scrolling as an interaction");
    Check(renderBody.find("bool isUserActive = ImGui::IsAnyMouseDown() || wheelScrolling;") != std::string::npos,
          "BDD: Mouse-wheel scrolling counts as active cover-wall interaction for request budgeting");
    Check(renderBody.find("bgIo.scrollingFast = wheelScrolling;") != std::string::npos,
          "BDD: Mouse-wheel scrolling feeds the cover-wall background-budget policy instead of being treated as idle");
    Check(renderBody.find("const int lastVisibleRowExclusive = (std::max)(\n                        firstVisibleRow + 1,\n                        (int)std::ceil((coverWallScrollY + availY) / cellH));") != std::string::npos &&
          renderBody.find("int visibleRowCount = (std::max)(1, lastVisibleRowExclusive - firstVisibleRow);") != std::string::npos,
          "BDD: Mouse-wheel partial-row browsing expands the cover-wall visible window with ceil(bottom-edge) math so newly exposed rows can auto-fill before a whole-row drag jump");
}

void TestCoverWallDisplayRefreshRearmsFirstScreenSweepContract() {
    const auto root = std::filesystem::path(__FILE__).parent_path().parent_path();
    const auto mainPath = root / "src" / "main.cpp";
    const auto refreshAdapterPath =
        root / "src" / "CoverWallRefreshStateAdapter.cpp";
    std::ifstream input(mainPath);
    std::ifstream refreshAdapterInput(refreshAdapterPath);
    Check(input.good(),
          "BDD: Given cover-wall display-refresh rearm contract validation runs, main.cpp can be opened");
    Check(refreshAdapterInput.good(),
          "BDD: Given cover-wall display-refresh rearm contract validation runs, CoverWallRefreshStateAdapter.cpp can be opened");
    if (!input.good() || !refreshAdapterInput.good()) {
        return;
    }

    std::ostringstream buffer;
    std::ostringstream refreshAdapterBuffer;
    buffer << input.rdbuf();
    refreshAdapterBuffer << refreshAdapterInput.rdbuf();
    const std::string source = buffer.str();
    const std::string refreshAdapterSource = refreshAdapterBuffer.str();
    const std::string requestRefreshBody = ExtractFunctionBody(
        refreshAdapterSource, "void RequestDisplayRefresh(AppState& state)");
    const std::string renderBody = ExtractFunctionBody(
        source, "void RenderFileInfoPanelTabContents(");

    Check(requestRefreshBody.find("state.coverWall.preRequestCursor = 0;") != std::string::npos &&
          requestRefreshBody.find("state.coverWall.lastPreRequestTotalItems = -1;") != std::string::npos &&
          requestRefreshBody.find("state.coverWall.lastFirstScreenStart = static_cast<std::size_t>(-1);") != std::string::npos &&
          requestRefreshBody.find("state.coverWall.lastFirstScreenEnd = 0;") != std::string::npos,
          "BDD: Any explicit cover-wall display refresh re-arms the first-screen sweep state");

    Check(renderBody.find("const bool coverWallDisplaySourceChanged =\n                pDisplayItems != nullptr && cw.source != nextCoverWallSource;") != std::string::npos &&
          renderBody.find("if (coverWallDisplaySourceChanged) {") != std::string::npos &&
          renderBody.find("cw.preRequestCursor = 0;") != std::string::npos &&
          renderBody.find("cw.lastPreRequestTotalItems = -1;") != std::string::npos,
          "BDD: When the cover wall switches display source after a filter/result transition, the first-screen sweep restarts instead of waiting for scrollbar drag");
}

void TestExplicitRefreshClearsCoverWallFailureCooldownContract() {
    const auto root = std::filesystem::path(__FILE__).parent_path().parent_path();
    const auto mainPath = root / "src" / "main.cpp";
    const auto refreshAdapterPath =
        root / "src" / "CoverWallRefreshStateAdapter.cpp";
    std::ifstream input(mainPath);
    std::ifstream refreshAdapterInput(refreshAdapterPath);
    Check(input.good(),
          "BDD: Given explicit refresh cover-wall retry contract validation runs, main.cpp can be opened");
    Check(refreshAdapterInput.good(),
          "BDD: Given explicit refresh cover-wall retry contract validation runs, CoverWallRefreshStateAdapter.cpp can be opened");
    if (!input.good() || !refreshAdapterInput.good()) {
        return;
    }

    std::ostringstream buffer;
    std::ostringstream refreshAdapterBuffer;
    buffer << input.rdbuf();
    refreshAdapterBuffer << refreshAdapterInput.rdbuf();
    const std::string source = buffer.str();
    const std::string refreshAdapterSource = refreshAdapterBuffer.str();
    const std::string displayRetryBody = ExtractFunctionBody(
        refreshAdapterSource, "void RequestDisplayRefreshForExplicitRetry(AppState& state)");
    const std::string databaseRetryBody = ExtractFunctionBody(
        refreshAdapterSource, "void RequestDatabaseRefreshForExplicitRetry(AppState& state)");
    const std::string keybindingsBody = ExtractFunctionBody(source, "void ProcessKeybindings(AppState& state)");
    const size_t navRefreshPos = source.find("if (navClicked[4])");
    const std::string navRefreshScope = navRefreshPos == std::string::npos
        ? std::string()
        : source.substr(navRefreshPos, 1000);
    const size_t menuRefreshPos = source.find(
        "// Refresh\n", source.find("ImGui::BeginMenu(L(Menu_Edit))"));
    const std::string menuRefreshScope = menuRefreshPos == std::string::npos
        ? std::string()
        : source.substr(menuRefreshPos, 1000);

    Check(displayRetryBody.find("RequestDisplayRefresh(state);") != std::string::npos &&
          displayRetryBody.find("state.coverWall.failedTexCache.clear();") != std::string::npos &&
          displayRetryBody.find("state.coverWall.failCountPeriod = 0;") != std::string::npos &&
          displayRetryBody.find("CoverModule::ResetFailedCache();") != std::string::npos,
          "BDD: Display-only explicit cover-wall retry re-arms its sweep and clears failure cooldown state");
    Check(databaseRetryBody.find("RequestDatabaseInvalidation(state);") != std::string::npos &&
          databaseRetryBody.find("state.coverWall.failedTexCache.clear();") != std::string::npos &&
          databaseRetryBody.find("state.coverWall.failCountPeriod = 0;") != std::string::npos &&
          databaseRetryBody.find("CoverModule::ResetFailedCache();") != std::string::npos,
          "BDD: Database explicit retry invalidates the full-library wall before clearing failure cooldown state");
    Check(keybindingsBody.find("aura::cover_wall::RequestDatabaseRefreshForExplicitRetry(state);") != std::string::npos,
          "BDD: Keyboard refresh in database view invalidates and reloads the full cover wall");
    Check(keybindingsBody.find("state.preferDeferredCountForNextRefresh = true;") == std::string::npos,
          "BDD: Keyboard refresh in database view stays on the single-pass refresh lane instead of scheduling a deferred count follow-up");
    Check(keybindingsBody.find("state.suppressFacetsForNextRefresh = true;") != std::string::npos,
          "BDD: Keyboard refresh in database view suppresses no-filter facet follow-up so the explicit retry stays single-pass");
    Check(navRefreshScope.find("aura::cover_wall::RequestDatabaseRefreshForExplicitRetry(state);") != std::string::npos,
          "BDD: Top refresh button invalidates and reloads the full cover wall");
    Check(menuRefreshScope.find("aura::cover_wall::RequestDatabaseRefreshForExplicitRetry(state);") != std::string::npos,
          "BDD: Menu refresh invalidates and reloads the full cover wall");
    Check(navRefreshScope.find("state.preferDeferredCountForNextRefresh = true;") == std::string::npos,
          "BDD: Top refresh button stays on the explicit refresh lane instead of splitting into a deferred follow-up");
    Check(navRefreshScope.find("state.suppressFacetsForNextRefresh = true;") != std::string::npos,
          "BDD: Top refresh button suppresses no-filter facet follow-up so refresh does not schedule a second wall reset");
}

void TestCoverClickUsesDeferredCountFastPathContract() {
    const auto mainPath = std::filesystem::path(__FILE__).parent_path().parent_path() / "src" / "main.cpp";
    std::ifstream input(mainPath);
    Check(input.good(),
          "BDD: Given cover-click fast-path contract validation runs, main.cpp can be opened");
    if (!input.good()) {
        return;
    }

    std::ostringstream buffer;
    buffer << input.rdbuf();
    const std::string source = buffer.str();
    const size_t coverClick = source.find("if (ImGui::InvisibleButton(\"##ArtHit\", ImVec2(thumb, thumb)))");
    Check(coverClick != std::string::npos,
          "BDD: Cover-wall click handler is present");
    Check(source.find("state.preferDeferredCountForNextRefresh = true;", coverClick) != std::string::npos &&
          source.find("RefreshSearchResults(state, true);", coverClick) != std::string::npos,
          "BDD: Cover-wall click submits a deferred-count refresh so result feedback is not blocked by the heavier count path");
}

void TestInconsistentFilteredRefreshTotalSchedulesRepairContract() {
    const auto mainPath = std::filesystem::path(__FILE__).parent_path().parent_path() / "src" / "main.cpp";
    std::ifstream input(mainPath);
    Check(input.good(),
          "BDD: Given filtered refresh total-repair contract validation runs, main.cpp can be opened");
    if (!input.good()) {
        return;
    }

    std::ostringstream buffer;
    buffer << input.rdbuf();
    const std::string source = buffer.str();
    const auto submissionPath =
        std::filesystem::path(__FILE__).parent_path().parent_path() /
        "src" / "AsyncSearchCountFacetRepairSubmission.cpp";
    std::ifstream submissionInput(submissionPath);
    Check(submissionInput.good(),
          "BDD: Given filtered refresh total-repair contract validation runs, AsyncSearchCountFacetRepairSubmission.cpp can be opened");
    std::ostringstream submissionBuffer;
    submissionBuffer << submissionInput.rdbuf();
    const std::string submissionSource = submissionBuffer.str();
    const auto factoryPath = std::filesystem::path(__FILE__).parent_path().parent_path() /
        "src" / "AsyncSearchResultEffectFactory.cpp";
    std::ifstream factoryInput(factoryPath);
    std::ostringstream factoryBuffer;
    factoryBuffer << factoryInput.rdbuf();
    const std::string repairBody = ExtractFunctionBody(
        submissionSource, "std::uint64_t SubmitCountAndFacetRepair(");
    const std::string applyBody = ExtractFunctionBody(
        source, "BuildAsyncSearchResultApplicationEffects()") + factoryBuffer.str();

    Check(repairBody.find("req.kind = AsyncSearchKind::CountAndFacetsOnly;") != std::string::npos &&
          repairBody.find("state.pendingFacetRequestId = id;") != std::string::npos,
          "BDD: Filtered refresh inconsistency repair uses the facet/count follow-up channel instead of a fresh main search");

    const auto foundationPath = std::filesystem::path(__FILE__).parent_path().parent_path() / "src" / "CoverReplacementFoundation.cpp";
    std::ifstream foundationInput(foundationPath);
    Check(foundationInput.good(),
          "BDD: Given filtered refresh total-repair contract validation runs, CoverReplacementFoundation.cpp can be opened");
    std::string foundationSource;
    if (foundationInput.good()) {
        std::ostringstream foundationBuffer;
        foundationBuffer << foundationInput.rdbuf();
        foundationSource = foundationBuffer.str();
    }
    const std::string refreshHelperBody = ExtractFunctionBody(
        foundationSource,
        "bool IsFilteredRefreshCountStateImpossible(int loadedCount,");

    const auto workflowPath = std::filesystem::path(__FILE__).parent_path().parent_path() /
        "src" / "AsyncSearchRefreshCountRepairWorkflow.cpp";
    std::ifstream workflowInput(workflowPath);
    std::ostringstream workflowBuffer;
    workflowBuffer << workflowInput.rdbuf();
    const std::string workflowSource = workflowBuffer.str();
    const auto adapterPath = std::filesystem::path(__FILE__).parent_path().parent_path() /
        "src" / "AsyncSearchRefreshCountRepairAdapter.cpp";
    const auto stateAdapterPath = std::filesystem::path(__FILE__).parent_path().parent_path() /
        "src" / "AsyncSearchRefreshCountRepairStateAdapter.cpp";
    std::ifstream adapterInput(adapterPath);
    std::ifstream stateAdapterInput(stateAdapterPath);
    std::ostringstream adapterBuffer;
    std::ostringstream stateAdapterBuffer;
    adapterBuffer << adapterInput.rdbuf();
    stateAdapterBuffer << stateAdapterInput.rdbuf();
    const std::string adapterSource = adapterBuffer.str();
    const std::string stateAdapterSource = stateAdapterBuffer.str();
    const auto compositionPath =
        std::filesystem::path(__FILE__).parent_path().parent_path() /
        "src" / "AsyncSearchRefreshResultCompositionAdapter.cpp";
    std::ifstream compositionInput(compositionPath);
    std::ostringstream compositionBuffer;
    compositionBuffer << compositionInput.rdbuf();
    const std::string compositionSource = compositionBuffer.str();

    Check(refreshHelperBody.find("loadedCount > 0") != std::string::npos &&
          refreshHelperBody.find("totalCount == 0") != std::string::npos &&
          refreshHelperBody.find("hasActiveDbFilter") != std::string::npos &&
          workflowSource.find("input.loadedCount > 0") != std::string::npos &&
          workflowSource.find("input.totalCount == 0") != std::string::npos,
          "BDD: Async refresh explicitly detects the impossible filtered state 'rows loaded but total count is zero' inside the governed repair workflow");

    Check(stateAdapterSource.find("result.totalCount == -1 && !result.countFollowUpSkipped") != std::string::npos &&
          stateAdapterSource.find("result.loadedCount") != std::string::npos &&
          stateAdapterSource.find("result.totalCount") != std::string::npos &&
          adapterSource.find("HandleAsyncSearchRefreshCountRepair(") != std::string::npos &&
          compositionSource.find("countRepairAdapter.apply();") != std::string::npos,
          "BDD: Inconsistent filtered refresh totals stay unsettled instead of being accepted as complete");

    Check(source.find("SubmitAsyncCountAndFacetRepair(") == std::string::npos &&
          applyBody.find("SubmitCountAndFacetRepair") != std::string::npos &&
          submissionSource.find("SubmitCountAndFacetRepair(") != std::string::npos &&
          stateAdapterSource.find("submitRepair(state, result.filter, needRepairFacets)") != std::string::npos,
          "BDD: Inconsistent filtered refresh totals schedule a lightweight repair request with the exact submitted filter");
}

void TestCountAndFacetFollowUpSettlesStandaloneAsyncStateContract() {
    const auto mainPath = std::filesystem::path(__FILE__).parent_path().parent_path() / "src" / "main.cpp";
    std::ifstream input(mainPath);
    Check(input.good(),
          "BDD: Given count/facet follow-up async-state contract validation runs, main.cpp can be opened");
    if (!input.good()) {
        return;
    }

    std::ostringstream buffer;
    buffer << input.rdbuf();
    const std::string source = buffer.str();
    const auto applicationPath =
        std::filesystem::path(__FILE__).parent_path().parent_path() /
        "src" / "AsyncSearchResultApplicationAdapter.cpp";
    std::ifstream applicationInput(applicationPath);
    std::ostringstream applicationBuffer;
    applicationBuffer << applicationInput.rdbuf();
    std::ifstream factoryInput(
        std::filesystem::path(__FILE__).parent_path().parent_path() /
        "src" / "AsyncSearchResultEffectFactory.cpp");
    applicationBuffer << factoryInput.rdbuf();
    const std::string applyBody = ExtractFunctionBody(
        source, "BuildAsyncSearchResultApplicationEffects()") +
        applicationBuffer.str();

    const auto workflowPath = std::filesystem::path(__FILE__).parent_path().parent_path() /
                              "src" / "AsyncSearchFacetResultWorkflow.cpp";
    std::ifstream workflowInput(workflowPath);
    std::ostringstream workflowBuffer;
    workflowBuffer << workflowInput.rdbuf();
    const std::string workflowSource = workflowBuffer.str();
    const auto adapterPath = std::filesystem::path(__FILE__).parent_path().parent_path() /
                             "src" / "AsyncSearchFacetResultStateAdapter.cpp";
    std::ifstream adapterInput(adapterPath);
    std::ostringstream adapterBuffer;
    adapterBuffer << adapterInput.rdbuf();
    const std::string adapterSource = adapterBuffer.str();

    Check(applyBody.find("facetResultAdapter.apply();") != std::string::npos &&
          adapterSource.find("HandleAsyncSearchFacetResult(") != std::string::npos &&
          workflowSource.find("releasePrimarySearch()") != std::string::npos,
          "BDD: Standalone CountAndFacetsOnly follow-ups release asyncSearchInProgress so probe-ready can settle after repair completes");
}

void TestCountAndFacetWarmStartZeroTotalDefersTruthContract() {
    const auto mainPath = std::filesystem::path(__FILE__).parent_path().parent_path() / "src" / "main.cpp";
    std::ifstream input(mainPath);
    Check(input.good(),
          "BDD: Given count/facet warm-start zero-total contract validation runs, main.cpp can be opened");
    if (!input.good()) {
        return;
    }

    std::ostringstream buffer;
    buffer << input.rdbuf();
    const std::string source = buffer.str();
    const auto applicationPath =
        std::filesystem::path(__FILE__).parent_path().parent_path() /
        "src" / "AsyncSearchResultApplicationAdapter.cpp";
    std::ifstream applicationInput(applicationPath);
    std::ostringstream applicationBuffer;
    applicationBuffer << applicationInput.rdbuf();
    std::ifstream factoryInput(
        std::filesystem::path(__FILE__).parent_path().parent_path() /
        "src" / "AsyncSearchResultEffectFactory.cpp");
    applicationBuffer << factoryInput.rdbuf();
    const std::string applyBody = ExtractFunctionBody(
        source, "BuildAsyncSearchResultApplicationEffects()") +
        applicationBuffer.str();
    const auto workflowPath = std::filesystem::path(__FILE__).parent_path().parent_path() /
                              "src" / "AsyncSearchFacetResultWorkflow.cpp";
    std::ifstream workflowInput(workflowPath);
    std::ostringstream workflowBuffer;
    workflowBuffer << workflowInput.rdbuf();
    const std::string workflowSource = workflowBuffer.str();
    const auto adapterPath = std::filesystem::path(__FILE__).parent_path().parent_path() /
                             "src" / "AsyncSearchFacetResultStateAdapter.cpp";
    std::ifstream adapterInput(adapterPath);
    std::ostringstream adapterBuffer;
    adapterBuffer << adapterInput.rdbuf();
    const std::string adapterSource = adapterBuffer.str();

    const auto foundationPath = std::filesystem::path(__FILE__).parent_path().parent_path() / "src" / "CoverReplacementFoundation.cpp";
    std::ifstream foundationInput(foundationPath);
    Check(foundationInput.good(),
          "BDD: Given count/facet warm-start zero-total contract validation runs, CoverReplacementFoundation.cpp can be opened");
    std::string foundationSource;
    if (foundationInput.good()) {
        std::ostringstream foundationBuffer;
        foundationBuffer << foundationInput.rdbuf();
        foundationSource = foundationBuffer.str();
    }
    const std::string warmStartHelperBody = ExtractFunctionBody(
        foundationSource,
        "bool IsFilteredFacetWarmStartCountStateImpossible(int64_t totalCount,");
    const auto warmStartPolicyPath =
        std::filesystem::path(__FILE__).parent_path().parent_path() /
        "src" / "AsyncSearchFacetWarmStartPolicy.cpp";
    std::ifstream warmStartPolicyInput(warmStartPolicyPath);
    std::ostringstream warmStartPolicyBuffer;
    warmStartPolicyBuffer << warmStartPolicyInput.rdbuf();
    const std::string warmStartPolicySource = warmStartPolicyBuffer.str();

    Check(warmStartHelperBody.find("totalCount == 0") != std::string::npos &&
          warmStartHelperBody.find("hasActiveDbFilter") != std::string::npos &&
          warmStartHelperBody.find("facetsComputed") != std::string::npos &&
          warmStartHelperBody.find("hasAnyFacetPayload") != std::string::npos &&
          applyBody.find("IsFilteredFacetWarmStartCountStateImpossible(") != std::string::npos &&
          warmStartPolicySource.find("cover_replacement::IsFilteredFacetWarmStartCountStateImpossible(") != std::string::npos &&
          workflowSource.find("inconsistentWarmStartTotal") != std::string::npos &&
          workflowSource.find("setCountFollowUp") != std::string::npos,
          "BDD: CountAndFacetsOnly follow-up explicitly detects the inconsistent filtered state 'facets exist but total count is zero' through the shared replacement foundation helper");

    Check(applyBody.find("HasPersistentFacetFilterSemantics") != std::string::npos &&
          adapterSource.find("persistenceEligible_(result_.filter)") != std::string::npos &&
          workflowSource.find("updateTotalCount") != std::string::npos &&
          workflowSource.find("setCountFollowUp") != std::string::npos,
          "BDD: Inconsistent count/facet warm-start totals stay pending instead of overwriting the visible filtered count with zero");
}

void TestNoFilterFacetApplySkipsMainThreadPersistentCacheWritesContract() {
    const auto mainPath = std::filesystem::path(__FILE__).parent_path().parent_path() / "src" / "main.cpp";
    std::ifstream input(mainPath);
    Check(input.good(),
          "BDD: Given no-filter facet apply cache-write contract validation runs, main.cpp can be opened");
    if (!input.good()) {
        return;
    }

    std::ostringstream buffer;
    buffer << input.rdbuf();
    const std::string source = buffer.str();
    const auto foundationPath = std::filesystem::path(__FILE__).parent_path().parent_path() / "src" / "CoverReplacementFoundation.cpp";
    std::ifstream foundationInput(foundationPath);
    Check(foundationInput.good(),
          "BDD: Given facet-cache apply persistence contract validation runs, CoverReplacementFoundation.cpp can be opened");
    std::string foundationSource;
    if (foundationInput.good()) {
        std::ostringstream foundationBuffer;
        foundationBuffer << foundationInput.rdbuf();
        foundationSource = foundationBuffer.str();
    }
    const std::string helperBody = ExtractFunctionBody(
        foundationSource, "bool HasPersistentFacetFilterSemantics(const SearchFilter& filter)");
    const auto applicationPath =
        std::filesystem::path(__FILE__).parent_path().parent_path() /
        "src" / "AsyncSearchResultApplicationAdapter.cpp";
    std::ifstream applicationInput(applicationPath);
    std::ostringstream applicationBuffer;
    applicationBuffer << applicationInput.rdbuf();
    const std::string applyBody = ExtractFunctionBody(
        source, "static void ApplyAsyncSearchResult(AppState& state, AsyncSearchResult& result)") +
        applicationBuffer.str();
    const auto workflowPath = std::filesystem::path(__FILE__).parent_path().parent_path() /
                              "src" / "AsyncSearchFacetResultWorkflow.cpp";
    std::ifstream workflowInput(workflowPath);
    std::ostringstream workflowBuffer;
    workflowBuffer << workflowInput.rdbuf();
    const std::string workflowSource = workflowBuffer.str();
    const auto adapterPath = std::filesystem::path(__FILE__).parent_path().parent_path() /
                             "src" / "AsyncSearchFacetResultStateAdapter.cpp";
    std::ifstream adapterInput(adapterPath);
    std::ostringstream adapterBuffer;
    adapterBuffer << adapterInput.rdbuf();
    const std::string adapterSource = adapterBuffer.str();

    Check(helperBody.find("!filter.keyword.empty()") != std::string::npos &&
          helperBody.find("!filter.folderPath.empty()") != std::string::npos &&
          helperBody.find("filter.coverFilter != 0") != std::string::npos &&
          helperBody.find("filter.synonyms != nullptr") != std::string::npos,
          "BDD: Facet-cache persistence semantics live in the shared replacement foundation and include synonym-driven filtered queries");

    Check(applyBody.find("facetResultAdapter.apply();") != std::string::npos &&
          adapterSource.find("HandleAsyncSearchFacetResult(") != std::string::npos &&
          workflowSource.find("persistFacets") != std::string::npos &&
          workflowSource.find("preservePathFacets") != std::string::npos,
          "BDD: Main-thread apply delegates filtered facet-cache persistence to the shared replacement foundation semantics instead of a drift-prone local copy");
}

void TestGlobalCoverWallIdleAutoAdvanceContract() {
    const auto sourcePath = std::filesystem::path(__FILE__).parent_path().parent_path() / "src" / "main.cpp";
    std::ifstream input(sourcePath);
    Check(input.good(),
          "BDD: Given global cover-wall user-driven page advance contract validation runs, main.cpp can be opened");
    if (!input.good()) {
        return;
    }

    std::ostringstream buffer;
    buffer << input.rdbuf();
    const std::string source = buffer.str();
    const std::string renderBody = ExtractFunctionBody(
        source, "void RenderFileInfoPanelTabContents(");

    Check(!renderBody.empty(),
          "BDD: Global cover-wall user-driven page advance contract can inspect the file-info panel render path");
    Check(renderBody.find("const bool shouldAutoAdvanceGlobalCoverPage = false;") != std::string::npos,
          "BDD: Global cover-wall background auto-advance is disabled so startup cannot crawl cover pages while idle");
    Check(renderBody.find("cw.items.size() < kGlobalCoverWallWindowMaxItems") == std::string::npos,
          "BDD: Idle auto-advance no longer stops at the legacy 10000-item global page window");
    Check(renderBody.find("shouldNearEndAdvanceGlobalCoverPage || shouldAutoAdvanceGlobalCoverPage") != std::string::npos,
          "BDD: Global cover-wall next-page trigger is preserved for near-tail browsing without startup idle page crawl");
}

void TestFilteredFacetWorkerCacheAdmitsPersistentFilterSemanticsContract() {
    const auto workerPath = std::filesystem::path(__FILE__).parent_path().parent_path() / "src" / "AsyncWorker.cpp";
    std::ifstream input(workerPath);
    Check(input.good(),
          "BDD: Given filtered facet worker-cache contract validation runs, AsyncWorker.cpp can be opened");
    if (!input.good()) {
        return;
    }

    std::ostringstream buffer;
    buffer << input.rdbuf();
    const std::string source = buffer.str();
    const std::string helperBody = ExtractFunctionBody(
        source, "static bool ShouldUseWorkerFacetCache(const AsyncSearchRequest& req, int64_t totalCountLocal)");
    const std::string keyBody = ExtractFunctionBody(
        source, "static std::string BuildFacetCacheKey(const AsyncSearchRequest& req, int64_t totalCountLocal)");
    const std::string workerBody = ExtractFunctionBody(
        source, "void SearchWorker::workerThread()");

    Check(!helperBody.empty() &&
          helperBody.find("aura::cover_replacement::HasPersistentFacetFilterSemantics(req.filter)") != std::string::npos &&
          helperBody.find("totalCountLocal >= kFacetCacheLargeRowsMin") != std::string::npos,
          "BDD: Filtered facet worker cache admits every persistent filter-semantic query while keeping the large unfiltered fallback");
    Check(source.find("constexpr size_t kFacetCacheMaxEntries = 8;") != std::string::npos,
          "BDD: Worker facet cache retains enough recent keyword/path/filter scopes to avoid two-entry churn during repeated filter switching");
    Check(!keyBody.empty() &&
          keyBody.find("appendVector(\"artists:\", req.filter.artists);") != std::string::npos &&
          keyBody.find("appendVector(\"includeGenres:\", req.filter.includeGenres);") != std::string::npos &&
          keyBody.find("appendVector(\"includeInstruments:\", req.filter.includeInstruments);") != std::string::npos,
          "BDD: Worker facet cache key serializes vector-valued filters covered by persistent facet semantics");
    Check(workerBody.find("if (!tryLoadFacetCache(facetCacheKey, warmStart.coverFacets, warmStart.pathFacets))") != std::string::npos,
          "BDD: Filtered facet warm-start checks the worker facet cache before recomputing cover/path facets");
    Check(workerBody.find("WorkerFacetCacheHit") != std::string::npos,
          "BDD: Worker facet cache hits are diagnostics-visible for prime/repeat performance probes");
}

void TestGlobalCoverWallCacheStoresOnlyCompletePagedWallContract() {
    const auto repoRoot = std::filesystem::path(__FILE__).parent_path().parent_path();
    const auto sourcePath = repoRoot / "src" / "main.cpp";
    const auto appStatePath = repoRoot / "src" / "AppState.h";
    const auto cachePolicyPath = repoRoot / "src" / "CoverWallCachePolicy.cpp";
    std::ifstream input(sourcePath);
    std::ifstream appStateInput(appStatePath);
    Check(input.good(),
          "BDD: Given global cover-wall cache completeness contract validation runs, main.cpp can be opened");
    Check(appStateInput.good(),
          "BDD: Given global cover-wall request ownership contract validation runs, AppState.h can be opened");
    if (!input.good() || !appStateInput.good()) {
        return;
    }

    std::ostringstream buffer;
    std::ostringstream appStateBuffer;
    buffer << input.rdbuf();
    appStateBuffer << appStateInput.rdbuf();
    const std::string source = buffer.str();
    const std::string appStateSource = appStateBuffer.str();
    const std::string cachePolicySource = ReadTextFile(cachePolicyPath);
    const std::string storeBody = ExtractFunctionBody(
        cachePolicySource, "void StoreGlobalCoverWallCache(CoverWallState& cw");

    Check(!storeBody.empty(),
          "BDD: Global cover-wall cache store helper is inspectable");
    Check(storeBody.find("if (cw.globalPageHasMore || cw.globalPageLoadingNext || cw.entriesInFlight)") != std::string::npos,
          "BDD: Global cover-wall cache refuses partial paged walls that still have more DB pages or in-flight entries");
    Check(storeBody.find("cw.globalItemsCacheValid = true;") != std::string::npos,
          "BDD: Complete global cover-wall cache is still available after the final page");
    Check(source.find("!cw.preloadFullList.empty() && cw.items.empty()") != std::string::npos &&
          source.find("cw.preloadFullList.clear();") != std::string::npos,
          "BDD: Preloaded first global cover-wall page is consumed once and cannot overwrite appended pages every frame");
    Check(appStateSource.find("std::string entriesRequestedCursor;") != std::string::npos &&
              appStateSource.find("bool entriesRequestedAppend = false;") != std::string::npos,
          "BDD: Global cover-wall async requests record the requested cursor and append mode");
    Check(source.find("const bool requestMatchesCurrentPageState") != std::string::npos &&
              source.find("GlobalPageIgnored") != std::string::npos,
          "BDD: Stale global cover-wall page results cannot overwrite a newer appended page session");
}

void TestStartupSettlePausesListMiniWaveSchedulingContract() {
    const auto sourcePath = std::filesystem::path(__FILE__).parent_path().parent_path() / "src" / "main.cpp";
    std::ifstream input(sourcePath);
    Check(input.good(),
          "BDD: Given startup mini-wave scheduling contract validation runs, main.cpp can be opened");
    if (!input.good()) {
        return;
    }

    std::ostringstream buffer;
    buffer << input.rdbuf();
    const std::string source = buffer.str();
    const std::string scheduleBody = ExtractFunctionBody(
        source, "static void ScheduleMiniWaveformRequestsForRange(AppState& state, int displayStart, int displayEnd)");

    Check(!scheduleBody.empty(),
          "BDD: Mini-wave visible-range scheduling helper is inspectable");
    Check(scheduleBody.find("state.dbSwitchSettleFramesRemaining > 0") != std::string::npos,
          "BDD: Startup/database-switch settle pauses list mini-wave scheduling before it can enqueue visible-row work");
}

void TestGlobalCoverWallVisibleBandVirtualizationContract() {
    const auto sourcePath = std::filesystem::path(__FILE__).parent_path().parent_path() / "src" / "main.cpp";
    std::ifstream input(sourcePath);
    Check(input.good(),
          "BDD: Given global cover-wall visible-band virtualization contract validation runs, main.cpp can be opened");
    if (!input.good()) {
        return;
    }

    std::ostringstream buffer;
    buffer << input.rdbuf();
    const std::string source = buffer.str();
    const std::string renderBody = ExtractFunctionBody(
        source, "void RenderFileInfoPanelTabContents(");

    Check(!renderBody.empty(),
          "BDD: Global cover-wall visible-band virtualization contract can inspect the file-info panel render path");
    Check(renderBody.find("constexpr int kCoverWallPrefetchBandRows = 2;") != std::string::npos,
          "BDD: Global cover-wall request path declares one narrow prefetch band contract");
    Check(renderBody.find("cw.qaPrefetchTileCount = firstScreenEndIdx - firstScreenStartIdx;") != std::string::npos &&
          renderBody.find("cw.qaVisibleTileCount++") != std::string::npos &&
          renderBody.find("cw.qaVisibleTileCount = firstScreenEndIdx - firstScreenStartIdx;") == std::string::npos,
          "BDD: QA visible cover-wall tiles count rendered clipper cells separately from the prefetch band");
    Check(renderBody.find("cw.progressiveBefore") == std::string::npos &&
          renderBody.find("cw.progressiveAfter") == std::string::npos,
          "BDD: Global cover-wall no longer performs wide progressive offscreen texture crawl across the full item list");
    Check(renderBody.find("CoverModule::PumpCoverTextureUploads(") != std::string::npos,
          "BDD: Global cover-wall still routes texture uploads through the existing main-thread upload pump");
}

void TestGlobalCoverWallPreRequestFailuresEnterCooldownContract() {
    const auto sourcePath = std::filesystem::path(__FILE__).parent_path().parent_path() / "src" / "main.cpp";
    std::ifstream input(sourcePath);
    Check(input.good(),
          "BDD: Given cover-wall pre-request failure cooldown contract validation runs, main.cpp can be opened");
    if (!input.good()) {
        return;
    }

    std::ostringstream buffer;
    buffer << input.rdbuf();
    const std::string source = buffer.str();
    const std::string renderBody = ExtractFunctionBody(
        source, "void RenderFileInfoPanelTabContents(");

    Check(!renderBody.empty(),
          "BDD: Cover-wall pre-request failure cooldown contract can inspect the file-info panel render path");
    const size_t preRequestLoop = renderBody.find("kPreRequestFirstScreenPerFrame");
    const size_t clipperLoop = renderBody.find("ImGuiListClipper clipper;");
    Check(preRequestLoop != std::string::npos && clipperLoop != std::string::npos && preRequestLoop < clipperLoop,
          "BDD: Cover-wall first-screen pre-request loop is located before the visible draw clipper");
    const std::string preRequestBody =
        (preRequestLoop != std::string::npos && clipperLoop != std::string::npos && preRequestLoop < clipperLoop)
            ? renderBody.substr(preRequestLoop, clipperLoop - preRequestLoop)
            : std::string();
    Check(preRequestBody.find("coverWallIdleNoInput") == std::string::npos &&
          preRequestBody.find("std::min(maxRequestsThisFrame, 2)") == std::string::npos &&
          renderBody.find("std::min(maxRequests, 1)") == std::string::npos,
          "BDD: Idle first-screen cover-wall fill uses the unified background budget instead of a local 1-2 request clamp");
    Check(preRequestBody.find("aura::cover_wall::ShouldRecordConfirmedFailure(it)") != std::string::npos &&
          preRequestBody.find("coverWallRecordFailure(it.key)") != std::string::npos &&
          preRequestBody.find("cw.failCountPeriod++") != std::string::npos,
           "BDD: Failed first-screen pre-request attempts enter the same failure cooldown ledger as visible draw failures");
}

void TestAdaptiveThumbnailKeywordCoverWallWarmupBudgetContract() {
    const auto sourcePath = std::filesystem::path(__FILE__).parent_path().parent_path() / "src" / "main.cpp";
    std::ifstream input(sourcePath);
    Check(input.good(),
          "BDD: Given large-thumbnail keyword cover-wall warmup contract validation runs, main.cpp can be opened");
    if (!input.good()) {
        return;
    }

    std::ostringstream buffer;
    buffer << input.rdbuf();
    const std::string source = buffer.str();
    const std::string renderBody = ExtractFunctionBody(
        source, "void RenderFileInfoPanelTabContents(");

    Check(!renderBody.empty(),
          "BDD: Adaptive thumbnail cover-wall warmup contract can inspect the file-info panel render path");
    Check(renderBody.find("const bool largeThumbRequested = cw.thumbSize >= 180.0f;") != std::string::npos,
          "BDD: Large-thumbnail warmup is keyed from the user-selected cover-wall thumbnail size");
    Check(renderBody.find("const int firstScreenTileCount = (int)(firstScreenEndIdx - firstScreenStartIdx);") != std::string::npos,
          "BDD: Dense-thumbnail warmup is keyed from the actual visible and prefetch tile count, not a fixed thumbnail-size guess");
    Check(renderBody.find("largeThumbRequested ? 3.0 : (smallList ? 2.5 : 1.5)") != std::string::npos,
          "BDD: Large-thumbnail walls keep the bounded warmup window alive long enough to fill the first screen");
    Check(renderBody.find("largeThumbWarmup =") != std::string::npos &&
          renderBody.find("bgIo.mouseIdle") != std::string::npos &&
          renderBody.find("totalItems <= 1200") != std::string::npos,
          "BDD: Large-thumbnail fill boost only runs while the wall is idle and the filtered result set is bounded");
    Check(renderBody.find("denseThumbWarmup =") != std::string::npos &&
          renderBody.find("firstScreenTileCount >= 48") != std::string::npos,
          "BDD: Small-thumbnail dense grids also get a bounded idle fill boost when one screen contains many cells");
    Check(renderBody.find("denseThumbWarmup ? 6 : 4") != std::string::npos &&
          renderBody.find("denseThumbWarmup ? 1400 : 900") != std::string::npos &&
          renderBody.find("denseThumbWarmup ? 48 : 12") != std::string::npos,
          "BDD: Adaptive thumbnail warmup raises upload/request budget without bypassing the shared cover budget");
    Check(renderBody.find("denseThumbWarmup ? 64 : (largeThumbWarmup ? 32 : kPreRequestFirstScreenPerFrame)") != std::string::npos,
          "BDD: Dense thumbnail warmup can sweep the current visible band faster than the default pre-request loop");
}

void TestCoverWallVisibleTextureAttemptsConsumeBudgetContract() {
    const auto sourcePath = std::filesystem::path(__FILE__).parent_path().parent_path() / "src" / "main.cpp";
    std::ifstream input(sourcePath);
    Check(input.good(),
          "BDD: Given visible cover-wall texture request budget contract validation runs, main.cpp can be opened");
    if (!input.good()) {
        return;
    }

    std::ostringstream buffer;
    buffer << input.rdbuf();
    const std::string source = buffer.str();
    const std::string renderBody = ExtractFunctionBody(
        source, "void RenderFileInfoPanelTabContents(");

    Check(!renderBody.empty(),
          "BDD: Visible cover-wall texture request budget contract can inspect the file-info panel render path");
    const size_t visibleRequest = renderBody.find("CoverModule::GetCoverTextureByKey(item, tex, shouldForceRetry)");
    Check(visibleRequest != std::string::npos,
          "BDD: Visible cover-wall draw path requests textures through the cover module");
    const std::string requestWindow =
        visibleRequest != std::string::npos ? renderBody.substr(visibleRequest, 900) : std::string();
    Check(requestWindow.find("if (!shouldForceRetry)") != std::string::npos &&
          requestWindow.find("cw.requestsThisFrame++") != std::string::npos,
          "BDD: Every non-forced visible texture attempt consumes the per-frame request budget before success/failure handling");
    Check(requestWindow.find("aura::cover_wall::ShouldRecordConfirmedFailure(item)") != std::string::npos &&
          requestWindow.find("coverWallRecordFailure(item.key)") != std::string::npos,
          "BDD: Confirmed visible texture failures still enter the failure cooldown ledger without being the only budgeted path");
}

void TestCoverWallFailureCooldownUsesWallClockContract() {
    const auto repoRoot = std::filesystem::path(__FILE__).parent_path().parent_path();
    const auto appStatePath = repoRoot / "src" / "AppState.h";
    const auto mainPath = repoRoot / "src" / "main.cpp";

    std::ifstream appStateInput(appStatePath);
    std::ifstream mainInput(mainPath);
    Check(appStateInput.good(),
          "BDD: Given cover-wall failure cooldown timing contract validation runs, AppState.h can be opened");
    Check(mainInput.good(),
          "BDD: Given cover-wall failure cooldown timing contract validation runs, main.cpp can be opened");
    if (!appStateInput.good() || !mainInput.good()) {
        return;
    }

    std::ostringstream appStateBuffer;
    std::ostringstream mainBuffer;
    appStateBuffer << appStateInput.rdbuf();
    mainBuffer << mainInput.rdbuf();
    const std::string appStateSource = appStateBuffer.str();
    const std::string mainSource = mainBuffer.str();
    const std::string renderBody = ExtractFunctionBody(
        mainSource, "void RenderFileInfoPanelTabContents(");

    Check(appStateSource.find("double lastAttemptTimeSec = 0.0;") != std::string::npos,
          "BDD: Cover-wall failed texture ledger stores wall-clock attempt time, not only frame count");
    Check(renderBody.find("constexpr double kCoverWallFailCooldownSeconds") != std::string::npos &&
          renderBody.find("(now - it->second.lastAttemptTimeSec) < kCoverWallFailCooldownSeconds") != std::string::npos,
          "BDD: Runtime cover-wall failure cooldown has a wall-clock floor so abnormal frame-rate loops cannot expire failures immediately");
    Check(renderBody.find("fail.lastAttemptTimeSec = now;") != std::string::npos,
          "BDD: Runtime cover-wall failure recording refreshes the wall-clock cooldown timestamp");
}

void TestCurrentFolderCoverFailureDiagnosticsAreSampledContract() {
    const auto sourcePath = std::filesystem::path(__FILE__).parent_path().parent_path() / "src" / "CoverModule.cpp";
    std::ifstream input(sourcePath);
    Check(input.good(),
          "BDD: Given current folder-cover failure diagnostics contract validation runs, CoverModule.cpp can be opened");
    if (!input.good()) {
        return;
    }

    std::ostringstream buffer;
    buffer << input.rdbuf();
    const std::string source = buffer.str();
    const std::string functionBody = ExtractFunctionBody(
        source, "bool GetFolderCoverTexture(const std::string& folderPathUtf8, TextureHandle& outTex, bool retryIfFailed /*= false*/)");

    Check(!functionBody.empty(),
          "BDD: Current folder-cover texture path can be inspected");
    Check(source.find("ShouldEmitCurrentFolderCoverFailureDiagnostic") != std::string::npos,
          "BDD: Current folder-cover failures use a dedicated wall-clock diagnostic sampler");
    Check(functionBody.find("ShouldEmitCurrentFolderCoverFailureDiagnostic(folderPathUtf8, retryIfFailed)") != std::string::npos,
          "BDD: Repeated current folder-cover failures are sampled before emitting CoverLane diagnostics");
    Check(functionBody.find("LogCoverLaneThrottled(\"current\", \"folder_cover\"") != std::string::npos,
          "BDD: Current folder-cover diagnostics still use the established CoverLane event");
    Check(source.find("kSummaryFlushSuppressedCount = 4096") != std::string::npos,
          "BDD: CoverLane summary diagnostics are sampled coarsely enough to stay off the cover-wall hot path");
    Check(source.find("ShouldEmitCoverWallLoadFailureDiagnostic(\"Embedded\"") != std::string::npos &&
              source.find("EmbeddedLoadFailSuppressed") != std::string::npos,
          "BDD: Embedded cover-wall load failures are bucket-sampled instead of logging every failing key");
}

void TestGlobalCoverWallCompletenessCeilingAndDiagnosticsContract() {
    const auto repoRoot = std::filesystem::path(__FILE__).parent_path().parent_path();
    const auto mainPath = repoRoot / "src" / "main.cpp";
    const auto workerPath = repoRoot / "src" / "AsyncWorker.cpp";
    const auto databasePath = repoRoot / "src" / "Database.cpp";
    const auto coverFacetBuilderPath = repoRoot / "src" / "CoverFacetItemBuilder.cpp";
    const auto pageResultBuilderPath = repoRoot / "src" / "CoverWallPageResultBuilder.cpp";

    std::ifstream mainInput(mainPath);
    std::ifstream workerInput(workerPath);
    std::ifstream databaseInput(databasePath);
    std::ifstream coverFacetBuilderInput(coverFacetBuilderPath);
    Check(mainInput.good(),
          "BDD: Given global cover-wall completeness contract validation runs, src/main.cpp can be opened");
    Check(workerInput.good(),
          "BDD: Given global cover-wall completeness contract validation runs, src/AsyncWorker.cpp can be opened");
    Check(databaseInput.good(),
          "BDD: Given global cover-wall completeness contract validation runs, src/Database.cpp can be opened");
    Check(coverFacetBuilderInput.good(),
          "BDD: Given global cover-wall completeness contract validation runs, src/CoverFacetItemBuilder.cpp can be opened");
    if (!mainInput.good() || !workerInput.good() || !databaseInput.good() || !coverFacetBuilderInput.good()) {
        return;
    }

    std::ostringstream mainBuffer;
    std::ostringstream workerBuffer;
    std::ostringstream databaseBuffer;
    std::ostringstream coverFacetBuilderBuffer;
    mainBuffer << mainInput.rdbuf();
    workerBuffer << workerInput.rdbuf();
    databaseBuffer << databaseInput.rdbuf();
    coverFacetBuilderBuffer << coverFacetBuilderInput.rdbuf();
    const std::string mainSource = mainBuffer.str();
    const std::string workerSource = workerBuffer.str();
    const std::string databaseSource = databaseBuffer.str();
    const std::string coverFacetBuilderSource = coverFacetBuilderBuffer.str();
    const std::string pageResultBuilderSource = ReadTextFile(pageResultBuilderPath);
    const std::string renderBody = ExtractFunctionBody(
        mainSource, "void RenderFileInfoPanelTabContents(");
    const std::string globalPageBody = ExtractFunctionBody(
        databaseSource, "Database::CoverWallPage Database::getGlobalCoverWallEntriesPage(const std::string& afterCursor,");

    Check(mainSource.find("static constexpr size_t kGlobalCoverWallWindowMaxItems = 10000;") == std::string::npos &&
          workerSource.find("constexpr int kFilteredCoverWallFacetLimit = 10000;") != std::string::npos,
          "BDD: Global no-filter cover wall no longer shares the filtered facet wall's legacy 10000 completeness ceiling");
    Check(renderBody.find("CoverWallSource nextCoverWallSource = CoverWallSource::Global;") != std::string::npos &&
          renderBody.find("CoverWallSource::SearchFallback") != std::string::npos &&
          renderBody.find("CoverWallSource::Facet") != std::string::npos &&
          renderBody.find("CoverWallSource::Selection") != std::string::npos,
          "BDD: Cover-wall render path classifies global, facet, fallback, and selection sources before drawing");
    const size_t globalModeStart = renderBody.find(
        "// Global mode reads deduplicated covers from the database, then discovers more.");
    const size_t globalModeEnd = renderBody.find(
        "// Refresh the cover-wall load-state text.", globalModeStart);
    const std::string globalMode =
        (globalModeStart != std::string::npos && globalModeEnd != std::string::npos)
            ? renderBody.substr(globalModeStart, globalModeEnd - globalModeStart)
            : std::string();
    Check(!globalMode.empty() &&
          globalMode.find("BuildCoverWallItemsFromCoverFacets") == std::string::npos &&
          globalMode.find("state.currentCoverFacets") == std::string::npos &&
          globalMode.find("currentTotal <= 50000") == std::string::npos &&
          globalMode.find("return LoadGlobalCoverWallPage(dbPath, {}, kGlobalCoverWallPageSize);") != std::string::npos,
          "BDD: Every no-filter library keeps file-list page order even when filtered facets finish first");
    Check(databaseSource.find("AppendString(out, e.sourcePath)") != std::string::npos &&
          databaseSource.find("cover_cluster_v5") != std::string::npos &&
          databaseSource.find("BuildPersistentFacetCacheKey(filter, limit, \"cover_cluster_v4\")") == std::string::npos &&
          databaseSource.find("c.sourcePath = safeGetText(stmt, 2)") != std::string::npos &&
          databaseSource.find("e.sourcePath = c.sourcePath;") != std::string::npos &&
          coverFacetBuilderSource.find("item.folderCoverSrcPathUtf8 = facet.sourcePath;") != std::string::npos,
          "BDD: Filtered facets carry real folder cover source paths so textures do not re-discover covers one by one");
    Check(databaseSource.find("SELECT key, rep_p, source_path, is_emb, hit_count, phash FROM (") != std::string::npos,
          "BDD: Filtered cover facet SQL selects source_path before CoverFacetEntry reads it");
    {
        const size_t registryLookup = databaseSource.find("WHERE af.folder_cover_key = r.cover_key");
        const size_t folderLookup = databaseSource.find("WHERE af.folder_cover_key = cover_cache_registry.cover_key");
        const std::string registryWindow =
            registryLookup == std::string::npos ? std::string() : databaseSource.substr(registryLookup, 320);
        const std::string folderWindow =
            folderLookup == std::string::npos ? std::string() : databaseSource.substr(folderLookup, 320);
        Check(registryWindow.find("AND af.folder_cover_key <> ''") != std::string::npos &&
              folderWindow.find("AND af.folder_cover_key <> ''") != std::string::npos,
              "BDD: Cover facet source-path lookup keeps the partial folder-cover index usable");
    }
    Check(!globalMode.empty() &&
          globalMode.find("CanReuseGlobalCoverWallCache") != std::string::npos &&
          globalMode.find("ReconstructGlobalCoverWallPageState") != std::string::npos &&
          globalMode.find("cw.globalPageNextCursor = page.nextCursor;") != std::string::npos &&
          globalMode.find("cw.globalPageHasMore = page.hasMore;") != std::string::npos &&
          globalMode.find("cw.discoveryDone = !cw.globalPageHasMore;") != std::string::npos,
          "BDD: Global cache and preload reuse preserve DB pagination instead of declaring facet completion");
    Check(renderBody.find("DisplaySourceChanged: source=%s") != std::string::npos &&
          renderBody.find("cw.source = nextCoverWallSource;") != std::string::npos,
          "BDD: Cover-wall diagnostics report display-source transitions together with filter/facet state");
    Check(renderBody.find("GlobalNextPageTriggered: reason=%s") != std::string::npos &&
          renderBody.find("GlobalPageApplied: append=%d") != std::string::npos,
          "BDD: Cover-wall diagnostics report global next-page trigger and apply events");
    Check(renderBody.find("windowMax=%zu") == std::string::npos,
          "BDD: Global cover-wall diagnostics no longer report a legacy hard window max");
    Check(mainSource.find("AppendUniqueCoverWallPageItems(cw.items, page.items)") != std::string::npos &&
          pageResultBuilderSource.find("BuildCoverWallVisualDedupeToken(") != std::string::npos &&
          pageResultBuilderSource.find("byToken") != std::string::npos &&
          pageResultBuilderSource.find("byKey") != std::string::npos &&
          pageResultBuilderSource.find("AppendUniqueCoverWallPageItems(") != std::string::npos,
          "BDD: Global cover-wall page building keeps a visual dedupe token set so cross-page repeats can collapse");
    Check(renderBody.find("const bool blockGlobalStartLoadForPreload =") != std::string::npos &&
          renderBody.find("preloadReadyForReuse || preloadStillBuildingFirstPage") != std::string::npos &&
          renderBody.find("!cw.loaded && !cw.loading && !blockGlobalStartLoadForPreload") != std::string::npos &&
          renderBody.find("currentTotal != cw.lastTotalFileCount && !cw.loading && !blockGlobalStartLoadForPreload") != std::string::npos,
          "BDD: Opening the cover wall does not start a duplicate first-page DB load while the dedicated preload path is still building or ready to reuse");
    Check(mainSource.find("return LoadGlobalCoverWallPage(dbPath, {}, kGlobalCoverWallPageSize);") != std::string::npos,
          "BDD: Global cover-wall first page reuses the shared paged helper instead of depending only on a bespoke local builder");
    Check(databaseSource.find("kCoverWallEmbeddedPhashTokenPrefix") != std::string::npos &&
          databaseSource.find("row.candidate.sourcePath =") != std::string::npos,
          "BDD: Global cover-wall DB rows carry an embedded-cover visual token when phash is known");
    Check(!globalPageBody.empty(),
          "BDD: Large-library paged global cover-wall contract can inspect getGlobalCoverWallEntriesPage");
    Check(globalPageBody.find("page.largeLibrary && registrySparse") != std::string::npos &&
          globalPageBody.find("page.largeLibrary && registrySparse && page.registryEntries > 0") == std::string::npos,
          "BDD: Large-library paged global cover wall may return ready partial rows even when the registry is still empty");
    Check(databaseSource.find("Global cover wall page GROUP BY bypassed for large library. Waiting for background sync.") == std::string::npos,
          "BDD: Large-library paged global cover wall no longer returns an empty waiting page just because background maintenance is pending");
    Check(renderBody.find("const bool shouldAutoAdvanceGlobalCoverPage = false;") != std::string::npos &&
          renderBody.find("shouldNearEndAdvanceGlobalCoverPage || shouldAutoAdvanceGlobalCoverPage") != std::string::npos,
          "BDD: Global cover-wall page crawl is near-tail driven instead of continuing after the visible window is satisfied");
    Check(renderBody.find("const bool largeLibraryCoverWallQa =") != std::string::npos &&
          renderBody.find("approximateCandidateFloor") != std::string::npos &&
          renderBody.find("kLargeLibraryQaCountApproxThreshold = 50000") != std::string::npos &&
          renderBody.find("QACountSummary: shown=%lld registry=%lld candidate=%lld approximate=1") != std::string::npos,
          "BDD: Large-library QA cover-wall count overlay avoids synchronous exact candidate recounts on the render hot path");
}

void TestQaCommandJsonUnescapeAcceptsUnicodePathsContract() {
    const auto sourcePath = std::filesystem::path(__FILE__).parent_path().parent_path() / "src" / "WindowsMain.cpp";
    std::ifstream input(sourcePath);
    Check(input.good(),
          "BDD: Given QA command JSON unicode contract validation runs, WindowsMain.cpp can be opened");
    if (!input.good()) {
        return;
    }

    std::ostringstream buffer;
    buffer << input.rdbuf();
    const std::string source = buffer.str();
    const size_t unescape = source.find("auto jsonUnescape = [](const std::string& s) -> std::string");
    Check(unescape != std::string::npos,
          "BDD: QA command JSON unescape helper is inspectable");
    Check(source.find("DecodeJsonUnicodeEscapeToUtf8", unescape) != std::string::npos &&
          source.find("case 'u':", unescape) != std::string::npos,
          "BDD: QA command JSON unescape decodes \\uXXXX paths before PathExistsUtf8");
}

void TestCoverWallFailedFolderStableKeysPreserveBrowseListContract() {
    const auto coverSourcePath = std::filesystem::path(__FILE__).parent_path().parent_path() / "src" / "CoverModule.cpp";
    const auto mainSourcePath = std::filesystem::path(__FILE__).parent_path().parent_path() / "src" / "main.cpp";
    std::ifstream coverInput(coverSourcePath);
    std::ifstream mainInput(mainSourcePath);
    Check(coverInput.good(),
          "BDD: Given BoomLibrary cover-wall failure pruning contract runs, CoverModule.cpp can be opened");
    Check(mainInput.good(),
          "BDD: Given BoomLibrary cover-wall failure pruning contract runs, main.cpp can be opened");
    if (!coverInput.good() || !mainInput.good()) {
        return;
    }

    std::ostringstream coverBuffer;
    coverBuffer << coverInput.rdbuf();
    const std::string coverSource = coverBuffer.str();
    std::ostringstream mainBuffer;
    mainBuffer << mainInput.rdbuf();
    const std::string mainSource = mainBuffer.str();

    const std::string isFailedBody =
        ExtractFunctionBody(coverSource, "bool IsCoverKeyFailed(const CoverItem& item)");
    const std::string removeBody =
        ExtractFunctionBody(coverSource, "size_t RemoveFailedCoverItems(std::vector<CoverItem>& items)");
    Check(!isFailedBody.empty(),
          "BDD: Cover key failure predicate is inspectable");
    Check(!removeBody.empty(),
          "BDD: Failed cover item preservation function is inspectable");

    Check(isFailedBody.find("IsKeyInFailedSetLocked(item.key)") != std::string::npos &&
          isFailedBody.find("IsKeyInFailedSetLocked(item.folderPathUtf8)") != std::string::npos,
          "BDD: Folder cover failure checks both stable cover key and folder path");
    Check(removeBody.find("(void)items;") != std::string::npos &&
          removeBody.find("return 0;") != std::string::npos &&
          removeBody.find("items.erase") == std::string::npos,
          "BDD: Failed cover loads are not removed from the browseable cover-wall display list");

    Check(mainSource.find("RemoveFailedCoverItems(*pDisplayItems)") == std::string::npos &&
          mainSource.find("RemoveFailedCoverItems(cw.pendingFacetItems)") == std::string::npos,
          "BDD: Cover wall render path keeps the source display list stable while failures cool down");
    Check(mainSource.find("coverWallHealedDisplayItems") != std::string::npos &&
          mainSource.find("pDisplayItems = &coverWallHealedDisplayItems;") != std::string::npos &&
          mainSource.find("cw.qaHiddenFailedItemCount = coverWallHiddenFailedItems;") != std::string::npos,
          "BDD: Cover wall compacts confirmed failed items out of the frame-local render list so startup black cells self-heal");
    Check(coverSource.find("bool HasFailedCoverIdentityWork()") != std::string::npos &&
              coverSource.find("CopyCoverItemsExcludingIdentityFailed") != std::string::npos &&
              mainSource.find("CoverModule::HasFailedCoverIdentityWork()") != std::string::npos &&
              mainSource.find("CoverModule::CopyCoverItemsExcludingIdentityFailed(") != std::string::npos,
          "BDD: Cover wall healing uses CoverModule failed identity state, not only the wall-local failure cache");
    Check(coverSource.find("bool HasPendingOrFailedCoverIdentityWork()") != std::string::npos &&
              coverSource.find("CopyCoverItemsExcludingIdentityPendingOrFailed") != std::string::npos &&
              coverSource.find("CoverWorkQueueStats GetPendingCoverWorkStats()") != std::string::npos &&
              coverSource.find("stats.pendingKeys") != std::string::npos &&
              coverSource.find("stats.asyncQueue") != std::string::npos &&
              coverSource.find("stats.uploadQueue") != std::string::npos &&
              mainSource.find("const bool coverWallCanYieldPendingItems") != std::string::npos &&
              mainSource.find("cw.source == CoverWallSource::Global") != std::string::npos &&
              mainSource.find("CopyCoverItemsExcludingIdentityPendingOrFailed") != std::string::npos &&
              mainSource.find("cw.qaHiddenPendingItemCount = coverWallHiddenPendingItems;") != std::string::npos,
          "BDD: No-filter global cover wall lets pending or failed cover identities yield from the frame-local render list while queue stats expose real prebake debt");
    Check(mainSource.find("GetCoverTextureByKey(it, tex, true)") == std::string::npos,
          "BDD: Automatic cover-wall prefetch respects failed-key cooldown; only user retry may force reload");
}

void TestCoverWallPerfProbeHasDeepHoldModeContract() {
    const auto scriptPath = std::filesystem::path(__FILE__).parent_path().parent_path() /
        "scripts" / "qa_probe_cover_wall_perf.py";
    std::ifstream input(scriptPath);
    Check(input.good(),
          "BDD: Given cover-wall deep-scroll probe contract runs, qa_probe_cover_wall_perf.py can be opened");
    if (!input.good()) {
        return;
    }

    std::ostringstream buffer;
    buffer << input.rdbuf();
    const std::string source = buffer.str();

    Check(source.find("\"deep_hold\"") != std::string::npos &&
          source.find("phase_states[\"afterDeepHold\"] = driver.get_state()") != std::string::npos,
          "BDD: Cover-wall perf probe has a deep-hold mode that records state after staying at a deep scroll position");
    Check(source.find("drag_path(scrollbar_x, scrollbar_top_y, scrollbar_x, scrollbar_bottom_y, steps=24)") != std::string::npos &&
          source.find("drag_path(scrollbar_x, scrollbar_bottom_y, scrollbar_x, scrollbar_top_y, steps=24)") == std::string::npos,
          "BDD: Deep-hold probe does not drag back to the top before final visible placeholder assertions run");
    Check(source.find("--require-cover-debt-progress") != std::string::npos &&
          source.find("coverDebtProgress") != std::string::npos &&
          source.find("wait_cover_wall_black_grid_free") != std::string::npos &&
          source.find("pending_cover_work") != std::string::npos &&
          source.find("cover pending debt did not decrease enough") != std::string::npos,
          "BDD: Deep-hold probe can require real cover debt progress instead of accepting hidden black-cell healing alone");
    Check(source.find("black_grid_free_wait_ms") != std::string::npos &&
          source.find("black_grid_free_wait_ok") != std::string::npos,
          "BDD: Cover-wall perf probe records how long the visible black grid takes to clear");
}

void TestCoverBrowseSurfaceProbeScriptContract() {
    const auto scriptPath = std::filesystem::path(__FILE__).parent_path().parent_path() /
        "scripts" / "qa_probe_cover_browse_surface.py";
    std::ifstream input(scriptPath);
    Check(input.good(),
          "BDD: Given replacement browse-surface probe contract runs, qa_probe_cover_browse_surface.py can be opened");
    if (!input.good()) {
        return;
    }

    std::ostringstream buffer;
    buffer << input.rdbuf();
    const std::string source = buffer.str();

    Check(source.find("\"PROOF-COVER-TREE-SCOPE\"") != std::string::npos &&
          source.find("\"PROOF-COVER-SLIDER-STABILITY\"") != std::string::npos &&
          source.find("\"PROOF-COVER-PREVIEW-PARITY\"") != std::string::npos &&
          source.find("\"PROOF-COVER-FILTER\"") != std::string::npos,
          "BDD: Replacement browse-surface probe binds tree, slider, preview, and cover-filter proof IDs into one durable report");
    Check(source.find("driver.load_path(first_path)") != std::string::npos &&
          source.find("driver.select_row(1") != std::string::npos &&
          source.find("effective_preview_path") != std::string::npos &&
          source.find("preview_uses_selected_record") != std::string::npos,
          "BDD: Replacement browse-surface probe verifies preview parity by loading one row and then selecting a different row");
    Check(source.find("driver.set_folder_filter(tree_path") != std::string::npos &&
          source.find("bottom_info") != std::string::npos &&
          source.find("treeScopeProof") != std::string::npos,
          "BDD: Replacement browse-surface probe verifies that tree scope survives on the same browse-surface info contract");
    Check(source.find("driver.set_specific_cover_filter(") != std::string::npos &&
          source.find("coverFilterScopeProof") != std::string::npos &&
          source.find("pinned_cover_key") != std::string::npos &&
          source.find("cover_filter_label") != std::string::npos,
          "BDD: Replacement browse-surface probe verifies that specific cover filtering stays on the same pinned browse-surface identity contract");
    Check(source.find("driver.set_cover_wall_thumb_size(") != std::string::npos &&
          source.find("driver.set_cover_wall_tile_preset(") != std::string::npos &&
          source.find("collect_relevant_db_events") != std::string::npos &&
          source.find("sliderStabilityProof") != std::string::npos,
          "BDD: Replacement browse-surface probe verifies slider/preset mutations without accepting hidden DB churn");
}

void TestFrameSpikeDiagnosticsPreservePreviousFramePhaseContract() {
    const auto sourcePath = std::filesystem::path(__FILE__).parent_path().parent_path() / "src" / "WindowsMain.cpp";
    std::ifstream input(sourcePath);
    Check(input.good(),
          "BDD: Given frame-spike diagnostic phase contract validation runs, WindowsMain.cpp can be opened");
    if (!input.good()) {
        return;
    }

    std::ostringstream buffer;
    buffer << input.rdbuf();
    const std::string source = buffer.str();
    const size_t logSpike = source.find("DevTools_LogFrameSpike(ImGui::GetIO().DeltaTime, intentionalFrameThrottleSleep)");
    const size_t resetPhases = source.find("DevTools_ResetFramePhases()", logSpike == std::string::npos ? 0 : logSpike);
    Check(logSpike != std::string::npos && resetPhases != std::string::npos && logSpike < resetPhases,
          "BDD: Frame-spike diagnostics log the previous frame before resetting phase timing, preserving SlowestPhase evidence");
}

void TestPlaybackThrottlesMainThreadMiniAndCoverUploadsContract() {
    const auto sourcePath = std::filesystem::path(__FILE__).parent_path().parent_path() / "src" / "main.cpp";
    std::ifstream input(sourcePath);
    Check(input.good(),
          "BDD: Given playback upload throttling contract validation runs, main.cpp can be opened");
    if (!input.good()) return;

    std::ostringstream buffer;
    buffer << input.rdbuf();
    const std::string source = buffer.str();
    const std::string miniUploadBody = ExtractFunctionBody(
        source, "static void ProcessPendingMiniWaveTextures(AppState& state)");
    const auto coverPrimePath =
        std::filesystem::path(__FILE__).parent_path().parent_path() /
        "src" / "AsyncSearchCoverPrimingRuntimeAdapter.cpp";
    std::ifstream coverPrimeInput(coverPrimePath);
    std::ostringstream coverPrimeBuffer;
    coverPrimeBuffer << coverPrimeInput.rdbuf();
    const std::string coverPrimeSource = coverPrimeBuffer.str();
    const std::string coverPrimeBody = ExtractFunctionBody(
        coverPrimeSource,
        "void PumpDeferredCoverPriming(AppState& state, int maxPerFrame)");

    Check(miniUploadBody.find("ShouldThrottleMiniUploadsForPlayback(state.audioEngine.isPlaying())") != std::string::npos &&
          miniUploadBody.find("getLoadedFilePath()") == std::string::npos &&
          miniUploadBody.find("maxUploadsThisFrame = (std::min)(maxUploadsThisFrame, 1);") != std::string::npos &&
          miniUploadBody.find("budgetMicros = (std::min)(budgetMicros, 300);") != std::string::npos,
          "BDD: Active playback limits main-thread mini waveform uploads to one short-budget upload per frame");
    Check(coverPrimeBody.find("if (state.audioEngine.isPlaying())") != std::string::npos &&
          coverPrimeBody.find("maxPerFrame = (std::min)(maxPerFrame, 2);") != std::string::npos,
          "BDD: Active playback limits deferred cover priming to two items per frame");
}

void TestPlaybackAndIdleTakeoverStatePolicy() {
    Check(ShouldThrottleMiniUploadsForPlayback(true),
          "BDD: Active playback enables the mini upload safety budget");
    Check(!ShouldThrottleMiniUploadsForPlayback(false),
          "BDD: A loaded but stopped file does not keep mini uploads permanently throttled");

    using Clock = std::chrono::steady_clock;
    const auto now = Clock::time_point{} + std::chrono::minutes(10);
    Check(IsIdleAssetBackfillCheckDue(now, now - std::chrono::seconds(1)),
          "BDD: Idle eligibility is evaluated when the one-minute check deadline expires");
    Check(!IsIdleAssetBackfillCheckDue(now, now + std::chrono::seconds(1)),
          "BDD: Idle eligibility is not evaluated before the next check deadline");
    Check(HasIdleAssetBackfillDelayElapsed(now, now - std::chrono::minutes(5), 5),
          "BDD: The configured five-minute idle threshold remains effective");
    Check(!HasIdleAssetBackfillDelayElapsed(now, now - std::chrono::minutes(4), 5),
          "BDD: A one-minute check does not bypass the configured idle threshold");

    Check(ShouldBlockIdleBackfillPoolTakeover(true, 0, 3),
          "BDD: Queued shared backfill work blocks idle pool takeover even with no in-flight job");
    Check(ShouldBlockIdleBackfillPoolTakeover(true, 1, 0),
          "BDD: In-flight shared backfill work blocks idle pool takeover");
    Check(!ShouldBlockIdleBackfillPoolTakeover(false, 0, 3),
          "BDD: A stopped pool may be started to drain queued idle work");

    const auto root = std::filesystem::path(__FILE__).parent_path().parent_path();
    std::ifstream mainInput(root / "src" / "main.cpp");
    std::ifstream stateInput(root / "src" / "AppState.h");
    std::ostringstream mainBuffer;
    std::ostringstream stateBuffer;
    mainBuffer << mainInput.rdbuf();
    stateBuffer << stateInput.rdbuf();
    const std::string source = mainBuffer.str();
    const std::string appState = stateBuffer.str();
    const std::string stopBody = ExtractFunctionBody(
        source, "static void StopIdleAssetBackfill(AppState& state, bool completed, bool interrupted)");
    Check(appState.find("idleAssetBackfillOwnsBackfillPool") != std::string::npos &&
          stopBody.find("if (state.idleAssetBackfillOwnsBackfillPool)") != std::string::npos,
          "BDD: Idle maintenance only stops the global backfill pool when it owns that pool");
}

void TestIdleAssetBackfillForegroundSurfaceIsProgressOnlyContract() {
    const auto sourcePath =
        std::filesystem::path(__FILE__).parent_path().parent_path() / "src" / "main.cpp";
    std::ifstream input(sourcePath);
    Check(input.good(),
          "BDD: Given idle backfill surface validation runs, main.cpp can be opened");
    if (!input.good()) return;

    std::ostringstream buffer;
    buffer << input.rdbuf();
    const std::string source = buffer.str();
    const std::string foregroundBody = ExtractFunctionBody(
        source, "static void RenderForegroundMaintenanceToasts(AppState& state)");

    Check(source.find("static void DrawIdleAssetBackfillToast") == std::string::npos &&
              foregroundBody.find("DrawIdleAssetBackfillToast(state)") == std::string::npos &&
              source.find("BuildIdleAssetBackfillWaitingToast") == std::string::npos &&
              source.find("BuildIdleAssetBackfillBlockedToast") == std::string::npos &&
              source.find("idleAssetBackfillWaitingToastVisible") == std::string::npos,
          "BDD: Idle asset backfill remains headless and never draws foreground feedback");
}

void TestDawRunningProbeNeverBlocksRenderThreadContract() {
    const auto sourcePath = std::filesystem::path(__FILE__).parent_path().parent_path() / "src" / "main.cpp";
    std::ifstream input(sourcePath);
    Check(input.good(),
          "BDD: Given DAW probe threading validation runs, main.cpp can be opened");
    if (!input.good()) return;

    std::ostringstream buffer;
    buffer << input.rdbuf();
    const std::string source = buffer.str();
    const std::string renderBody = ExtractFunctionBody(
        source, "void RenderUI(AppState& state)");
    const std::string dispatchBody = ExtractFunctionBody(
        source, "static void DispatchPreparedSendToDAW(AppState& state,");
    const std::string reaperUpdateBody = ExtractFunctionBody(
        source, "static bool StartReaperBridgeUpdateAfterExit(");
    const std::string reaperPrepareBody = ExtractFunctionBody(
        source, "static bool PrepareReaperBridgeForSend(");
    const std::string renderPollBody = ExtractFunctionBody(
        source, "static void UpdateDAWRunningState(AppState& state)");
    const std::string workerProbeBody = ExtractFunctionBody(
        source, "static DawRunningProbeResults ProbeDawRunningStateSnapshots(DawRunningProbeBatch requests)");
    const std::string registryPumpBody = ExtractFunctionBody(
        source, "static void PumpDawBridgeRegistryProbeCache()");
    const std::string registryWorkerBody = ExtractFunctionBody(
        source, "static std::vector<HarkenSearch::DawBridge::InstallStatus> ProbeDawBridgeRegistrySnapshots(");
    const std::string autoDetectKeyBody = ExtractFunctionBody(
        source, "static std::string ReaperAutoDetectRequestKey(const DawTarget& target)");

    Check(renderPollBody.find("s_probeWorker.Poll()") != std::string::npos &&
          renderPollBody.find("s_probeWorker.Submit(std::move(requests))") != std::string::npos,
          "BDD: RenderUI only polls completed DAW snapshots and submits immutable work");
    Check(renderPollBody.find("RunReaperBridgeDoctor") == std::string::npos &&
          renderPollBody.find("GetRunningProcessPathAny") == std::string::npos &&
          renderPollBody.find("IsProcessRunning") == std::string::npos,
          "BDD: RenderUI never performs bridge handshakes or operating-system process probes");
    Check(renderBody.find("RunReaperBridgeDoctor") == std::string::npos &&
          renderBody.find("AutoDetectReaperRootPath") == std::string::npos &&
          renderBody.find("PrepareReaperBridgeForSend") == std::string::npos,
          "BDD: RenderUI keeps REAPER diagnosis and repair off the render thread");
    const size_t sendWorkerStart = dispatchBody.find("state.dawSendThread = std::thread(");
    const size_t reaperPreflight = dispatchBody.find("PrepareReaperBridgeForSend");
    Check(sendWorkerStart != std::string::npos &&
          reaperPreflight != std::string::npos &&
          sendWorkerStart < reaperPreflight,
          "BDD: DAW send dispatch preflights REAPER only inside its worker");
    Check(reaperUpdateBody.find("compare_exchange_strong") != std::string::npos &&
          reaperUpdateBody.find("return false;") != std::string::npos,
          "BDD: REAPER update reports a busy session instead of claiming an update was queued");
    Check(source.find("const std::string oldReaperCacheKey") != std::string::npos,
          "BDD: REAPER path edits invalidate the old status-cache key before changing the path");
    Check(workerProbeBody.find("RunReaperBridgeDoctor") != std::string::npos &&
          workerProbeBody.find("GetRunningProcessPathAny") != std::string::npos &&
          workerProbeBody.find("IsProcessRunning") != std::string::npos &&
          workerProbeBody.find("Platform::SetThreadBackgroundPriority()") != std::string::npos,
          "BDD: All DAW detection paths execute on the below-normal-priority probe worker");
    Check(workerProbeBody.find("AutoDetectReaperRootPath") != std::string::npos &&
          renderPollBody.find("result.autoDetectRoot") != std::string::npos &&
          renderPollBody.find("configuredRootPath") != std::string::npos,
          "BDD: REAPER auto-detect resolves on the worker and applies the detected root on the render thread");
    Check(renderBody.find("g_dawBridgeRegistry[i].probe()") == std::string::npos &&
          registryPumpBody.find("s_registryProbeWorker.Poll()") != std::string::npos &&
          registryPumpBody.find("s_registryProbeWorker.Submit(g_dawBridgeRegistry)") != std::string::npos,
          "BDD: RenderUI only polls/submits the generic DAW bridge registry cache");
    Check(registryWorkerBody.find("installer.probe()") != std::string::npos &&
          registryWorkerBody.find("Platform::SetThreadBackgroundPriority()") != std::string::npos,
          "BDD: Generic DAW bridge registry probes execute on a below-normal-priority worker");
    Check(autoDetectKeyBody.find("ReaperBridgeUiCacheKey(target)") != std::string::npos,
          "BDD: REAPER auto-detect requests include the configured root and cannot fan out to duplicate presets");
    Check(reaperPrepareBody.find("g_dawBridgeInstallInFlight") == std::string::npos &&
          reaperPrepareBody.find("InstallState::HandshakeReady") != std::string::npos,
          "BDD: A different DAW installer cannot block a handshake-ready REAPER send");
    const size_t sessionHarvest = renderBody.find("g_dawBridgeSession.reset()");
    const size_t installLeaseRelease = renderBody.find(
        "g_dawBridgeInstallInFlight.store(false)", sessionHarvest);
    Check(sessionHarvest != std::string::npos &&
          installLeaseRelease != std::string::npos &&
          sessionHarvest < installLeaseRelease &&
          reaperUpdateBody.find("g_dawBridgeInstallInFlight.store(false)") == std::string::npos,
          "BDD: DAW install lease is released only after the completed session is harvested");
}

void TestCoverPrefetchYieldsToCoverWallPriorityContract() {
    const auto sourcePath = std::filesystem::path(__FILE__).parent_path().parent_path() / "src" / "WindowsMain.cpp";
    std::ifstream input(sourcePath);
    Check(input.good(),
          "BDD: Given cover prefetch priority contract validation runs, WindowsMain.cpp can be opened");
    if (!input.good()) {
        return;
    }

    std::ostringstream buffer;
    buffer << input.rdbuf();
    const std::string source = buffer.str();
    const std::string pumpBody = ExtractFunctionBody(
        source, "static void PumpCoverPrefetch(AppState& state, const BackgroundIoContext& coverIo)");

    Check(!pumpBody.empty(),
          "BDD: Cover prefetch pump is inspectable");
    Check(pumpBody.find("if (coverIo.coverWallPriorityMode) return;") != std::string::npos,
          "BDD: Result-list cover prefetch yields while the cover wall owns foreground cover budget");
    Check(pumpBody.find("std::min(coverBudget.maxRequestsPerFrame, 1)") != std::string::npos,
          "BDD: Result-list cover prefetch is capped to one request per frame outside cover-wall priority mode");
}

void TestCoverColumnUsesCachedTexturesDuringCoverWallPriorityContract() {
    const auto mainPath = std::filesystem::path(__FILE__).parent_path().parent_path() / "src" / "main.cpp";
    std::ifstream mainInput(mainPath);
    Check(mainInput.good(),
          "BDD: Given cover-column priority contract validation runs, src/main.cpp can be opened");
    if (!mainInput.good()) {
        return;
    }

    std::ostringstream mainBuffer;
    mainBuffer << mainInput.rdbuf();
    const std::string mainSource = mainBuffer.str();

    Check(mainSource.find("const bool cacheOnlyCoverColumnReads = deferCoverLoadsForHeavyWork;") != std::string::npos,
          "BDD: Result-list cover column switches to cache-only reads while cover-wall priority or other heavy work defers new cover loads");
    Check(mainSource.find("? CoverModule::TryGetCachedCoverTextureByKey(embeddedProbe, coverTex)") != std::string::npos &&
          mainSource.find("? CoverModule::TryGetCachedCoverTextureByKey(inherited, coverTex)") != std::string::npos,
          "BDD: Result-list cover column still renders cached embedded or inherited art instead of blanking the whole column during cover-wall priority");

    const auto coverHeaderPath = std::filesystem::path(__FILE__).parent_path().parent_path() / "src" / "CoverModule.h";
    std::ifstream coverHeaderInput(coverHeaderPath);
    Check(coverHeaderInput.good(),
          "BDD: Given cover-column priority contract validation runs, src/CoverModule.h can be opened");
    if (!coverHeaderInput.good()) {
        return;
    }

    std::ostringstream coverHeaderBuffer;
    coverHeaderBuffer << coverHeaderInput.rdbuf();
    const std::string coverHeaderSource = coverHeaderBuffer.str();
    Check(coverHeaderSource.find("bool TryGetCachedCoverTextureByKey(const CoverItem& item, TextureHandle& outTex);") != std::string::npos,
          "BDD: CoverModule exposes an explicit cache-only cover read path for surfaces that must not enqueue new cover work");
}

void TestSearchResultWarmStartCoverPreloadYieldsToGlobalCoverWallContract() {
    const auto sourcePath = std::filesystem::path(__FILE__).parent_path().parent_path() / "src" / "main.cpp";
    std::ifstream input(sourcePath);
    Check(input.good(),
          "BDD: Given search-result cover warm-start contract validation runs, src/main.cpp can be opened");
    if (!input.good()) {
        return;
    }

    std::ostringstream buffer;
    buffer << input.rdbuf();
    const std::string source = buffer.str();
    const auto workflowPath = std::filesystem::path(__FILE__).parent_path().parent_path() /
        "src" / "AsyncSearchCoverPreloadWorkflow.cpp";
    std::ifstream workflowInput(workflowPath);
    Check(workflowInput.good(),
          "BDD: Search-result cover warm-start workflow source can be opened");
    std::ostringstream workflowBuffer;
    workflowBuffer << workflowInput.rdbuf();
    const std::string workflowSource = workflowBuffer.str();
    const auto stateAdapterPath =
        std::filesystem::path(__FILE__).parent_path().parent_path() /
        "src" / "AsyncSearchCoverPreloadStateAdapter.cpp";
    std::ifstream stateAdapterInput(stateAdapterPath);
    Check(stateAdapterInput.good(),
          "BDD: Search-result cover warm-start state adapter source can be opened");
    std::ostringstream stateAdapterBuffer;
    stateAdapterBuffer << stateAdapterInput.rdbuf();
    const std::string stateAdapterSource = stateAdapterBuffer.str();

    Check(workflowSource.find("const bool globalNoFilterBrowse =") != std::string::npos,
          "BDD: Search-result cover warm-start explicitly recognizes the global no-filter browse scenario");
    Check(workflowSource.find("const bool deferredForCoverWall =") != std::string::npos,
          "BDD: Search-result cover warm-start has an explicit defer-to-cover-wall gate");
    Check(source.find("IsCoverWallPriorityMode") != std::string::npos &&
          stateAdapterSource.find("state_.coverWall.preloadInFlight") != std::string::npos &&
          stateAdapterSource.find("!state_.coverWall.preloadFullList.empty()") != std::string::npos,
          "BDD: Search-result cover warm-start yields while the global cover wall owns preload or ready-to-reuse work");
    Check(stateAdapterSource.find("HandleAsyncSearchCoverPreload(") != std::string::npos &&
          stateAdapterSource.find("state_.dbSwitchSettleFramesRemaining") != std::string::npos &&
          workflowSource.find("if (deferredForCoverWall || input.settleFramesRemaining > 0)") != std::string::npos,
          "BDD: Search-result cover warm-start stays disabled until cover-wall contention clears");
}

void TestHarnessLaneKeepsStartupLoadingGateContract() {
    const auto sourcePath = std::filesystem::path(__FILE__).parent_path().parent_path() / "src" / "WindowsMain.cpp";
    std::ifstream input(sourcePath);
    Check(input.good(),
          "BDD: Given harness startup gate contract validation runs, WindowsMain.cpp can be opened");
    if (!input.good()) {
        return;
    }

    std::ostringstream buffer;
    buffer << input.rdbuf();
    const std::string source = buffer.str();

    Check(source.find("const bool bypassStartupLoadingGate =\n                startupBypassForDirectFilePlayback ||\n                s_isIngestMode;") != std::string::npos,
          "BDD: Harness lane no longer bypasses the startup loading/preheat gate");
    Check(source.find("const bool hasStartupWebView = (bool)wv2Controller && (bool)wv2Webview;") != std::string::npos &&
          source.find("const bool useBetaSplashFallback =") != std::string::npos &&
          source.find("(kUseBetaSplash || !hasStartupWebView)") != std::string::npos,
          "BDD: Startup loading gate falls back to beta splash when login is skipped and no WebView loading surface exists");
    Check(source.find("Startup loading gate bypassed for harness lane.") == std::string::npos,
          "BDD: Harness diagnostics do not advertise a startup-loading bypass that no longer exists");
    Check(source.find("const bool startupSearchIdle =\n            state.pendingSearchRequestId == 0 &&\n            state.pendingFacetRequestId == 0 &&\n            !state.asyncSearchInProgress;") != std::string::npos &&
          source.find("!state.filePlayerFastPathActive &&\n            startupSearchIdle &&\n            state.searchResults.empty()") != std::string::npos,
          "BDD: Startup fallback refresh yields to an already-submitted search/filter so launch cannot overwrite an external filtered query with a global refresh");
    Check(source.find("static std::atomic<bool> s_mainUiReady{false};") != std::string::npos &&
          source.find("const bool mainUiReady = s_mainUiReady.load(std::memory_order_acquire);") != std::string::npos &&
          source.find("bool ready = dbReady && mainUiReady && searchDone;") != std::string::npos &&
          source.find("const bool ready = dbReady") != std::string::npos &&
          source.find("s_mainUiReady.store(true, std::memory_order_release);") != std::string::npos,
          "BDD: QA ready/probe-ready gates wait for MainUIReady before external filter probes can mutate launch state");
}

void TestCurrentCoverPreviewFailureCooldownContract() {
    const auto sourcePath = std::filesystem::path(__FILE__).parent_path().parent_path() / "src" / "main.cpp";
    std::ifstream input(sourcePath);
    Check(input.good(),
          "BDD: Given current-cover failure cooldown validation runs, src/main.cpp can be opened");
    if (!input.good()) {
        return;
    }

    std::ostringstream buffer;
    buffer << input.rdbuf();
    const std::string source = buffer.str();

    Check(source.find("static std::string s_lastCoverFailedPath;") != std::string::npos &&
          source.find("static std::string s_lastCoverFailedSig;") != std::string::npos &&
          source.find("static double s_lastCoverFailedAt = -1.0;") != std::string::npos,
          "BDD: Current-cover preview tracks failed file/signature cooldown state");
    Check(source.find("constexpr double kCurrentCoverFailureRetryCooldownSec = 0.35;") != std::string::npos &&
          source.find("const bool currentCoverFailureCoolingDown =") != std::string::npos,
          "BDD: Current-cover preview uses an explicit retry cooldown instead of retrying the same failing cover every frame");
    Check(source.find("!deferHeavyCacheWork &&\n                    !currentCoverFailureCoolingDown") != std::string::npos,
          "BDD: Current-cover preview blocks repeated heavy retry work while the same failure stays in cooldown");
    Check(source.find("s_lastCoverFailedPath = info.filePath;") != std::string::npos &&
          source.find("s_lastCoverFailedSig = currentCoverSig;") != std::string::npos &&
          source.find("s_lastCoverFailedAt = ImGui::GetTime();") != std::string::npos,
          "BDD: Current-cover preview records failed attempts for cooldown bookkeeping");
}

void TestListAndPrefetchSkipKnownFailedEmbeddedCoversContract() {
    const auto sourcePath = std::filesystem::path(__FILE__).parent_path().parent_path() / "src" / "main.cpp";
    std::ifstream input(sourcePath);
    Check(input.good(),
          "BDD: Given failed-embedded short-circuit validation runs, src/main.cpp can be opened");
    if (!input.good()) {
        return;
    }

    std::ostringstream buffer;
    buffer << input.rdbuf();
    const std::string source = buffer.str();
    const auto adapterPath = std::filesystem::path(__FILE__).parent_path().parent_path() /
                             "src" / "AsyncSearchCoverPreloadStateAdapter.cpp";
    std::ifstream adapterInput(adapterPath);
    Check(adapterInput.good(),
          "BDD: Given failed-embedded short-circuit validation runs, AsyncSearchCoverPreloadStateAdapter.cpp can be opened");
    std::ostringstream adapterBuffer;
    adapterBuffer << adapterInput.rdbuf();
    const std::string prefetchSource = source + "\n" + adapterBuffer.str();

    const auto coverModulePath = std::filesystem::path(__FILE__).parent_path().parent_path() / "src" / "CoverModule.cpp";
    std::ifstream coverModuleInput(coverModulePath);
    Check(coverModuleInput.good(),
          "BDD: Given failed-embedded short-circuit validation runs, src/CoverModule.cpp can be opened");
    std::ostringstream coverModuleBuffer;
    coverModuleBuffer << coverModuleInput.rdbuf();
    const std::string coverModuleSource = coverModuleBuffer.str();

    Check(coverModuleSource.find("bool IsCoverKeyPendingOrFailed(const CoverItem& item)") != std::string::npos,
          "BDD: CoverModule exposes a pending-or-failed helper for hot cover paths");
    Check(coverModuleSource.find("bool IsCoverIdentityKeyPendingOrFailed(const CoverItem& item)") != std::string::npos &&
          coverModuleSource.find("hasStableFolderIdentity") != std::string::npos,
          "BDD: CoverModule exposes a cover-wall identity pending helper so stable wall keys are not blocked by folder-path aliases");
    Check(prefetchSource.find("if (!CoverModule::IsCoverKeyPendingOrFailed(probe))") != std::string::npos &&
          prefetchSource.find("CoverModule::GetEmbeddedCoverTexture(coverKey, filePath, dummy)") != std::string::npos,
          "BDD: Visible-row cover prefetch skips embedded keys that are already pending or failed");
    Check(source.find("if (!CoverModule::IsCoverKeyPendingOrFailed(embeddedProbe)) {\n                                hasTex = cacheOnlyCoverColumnReads") != std::string::npos &&
          source.find("CoverModule::TryGetCachedCoverTextureByKey(embeddedProbe, coverTex)") != std::string::npos &&
          source.find("CoverModule::GetEmbeddedCoverTexture(record.embeddedCoverKey, record.filePath, coverTex)") != std::string::npos,
          "BDD: List row cover column short-circuits embedded keys that are already pending or failed");
    Check(source.find("if (!CoverModule::IsCoverKeyPendingOrFailed(embeddedFallbackProbe)) {\n                                    hasTex = cacheOnlyCoverColumnReads") != std::string::npos &&
          source.find("CoverModule::TryGetCachedCoverTextureByKey(embeddedFallbackProbe, coverTex)") != std::string::npos &&
          source.find("CoverModule::GetEmbeddedCoverTexture(fallbackKey, repFile, coverTex)") != std::string::npos,
          "BDD: Folder-level embedded fallback also short-circuits embedded keys that are already pending or failed");
    Check(source.find("!coverWallFailureCoolingDown(it.key) &&\n                                !CoverModule::IsCoverIdentityKeyPendingOrFailed(it)") != std::string::npos,
          "BDD: Cover-wall first-screen pre-request skips only the foreground identity key that is still pending or already failed");
}


void TestAutomaticMainDetailPrewarmIsOptInOnly() {
    auto ctx = IdleContext();

    const auto off = ResolveAutomaticMainDetailPrewarmDecision(ctx, false);
    const auto on = ResolveAutomaticMainDetailPrewarmDecision(ctx, true);

    Check(!off.allowed,
          "BDD: Given no explicit opt-in, neighboring main detail prewarm cannot write waveform detail");
    Check(on.allowed,
          "BDD: Given explicit opt-in and idle foreground-safe state, neighboring main detail prewarm may run");
}

void TestAutomaticCoverPhashBackfillIsOptInOnly() {
    auto ctx = IdleContext();

    const auto off = ResolveAutomaticCoverMaintenanceDecision(ctx, false);
    const auto on = ResolveAutomaticCoverMaintenanceDecision(ctx, true);

    Check(!off.allowed,
          "BDD: Given no explicit opt-in, startup cover phash backfill cannot scan/write the library");
    Check(on.allowed,
          "BDD: Given explicit opt-in and idle foreground-safe state, cover phash backfill may run");
}

void TestModuleDockEdgeSnapRejectsNestedPanelSplitsContract() {
    const auto sourcePath = std::filesystem::path(__FILE__).parent_path().parent_path() / "src" / "main.cpp";
    const auto hitTestPath = std::filesystem::path(__FILE__).parent_path().parent_path() / "src" / "DockHitTest.h";
    std::ifstream in(sourcePath, std::ios::binary);
    std::stringstream buffer;
    buffer << in.rdbuf();
    const std::string source = buffer.str();
    const std::string hitTestSource = ReadTextFile(hitTestPath);

    Check(!source.empty(),
          "BDD: Given module dock source contract validation runs, src/main.cpp can be opened");
    Check(!hitTestSource.empty(),
          "BDD: Shared module dock hit-test policy can be opened");

    const std::string detectBody = ExtractLambdaBody(
        source,
        "auto detectDockZone = [&](ImVec2 winPos, ImVec2 winSize, const char* draggingId) -> aura::ui::dock::DockHit");
    const std::string previewBody = ExtractLambdaBody(
        source,
        "auto drawDockPreview = [&](bool titleBarDragging)");
    const std::string hitBody = ExtractFunctionBody(
        hitTestSource, "inline DockHit HitDockTarget(");
    const std::string planBody = ExtractFunctionBody(
        hitTestSource, "inline DockPreviewPlan PlanDockPreview(");

    Check(!detectBody.empty(),
          "BDD: Module dock zone detector is inspectable");
    Check(!previewBody.empty(),
          "BDD: Module dock preview drawer is inspectable");

    Check(source.find("const float dockSnapDistance = aura::ui::dock::kDockSnapDistance;") != std::string::npos &&
          hitTestSource.find("enum class DockHitKind { None, Main, Parent, Module };") != std::string::npos &&
          hitBody.find("if (work.width <= 0.0f || work.height <= 0.0f || snapDistance <= 0.0f) return miss;") != std::string::npos,
          "BDD: Module dock edge snap defines an explicit bounded edge-zone policy");
    Check(detectBody.find("aura::ui::dock::HitDockTarget(") != std::string::npos &&
          detectBody.find("const ImVec2 pointer = ImGui::GetMousePos();") != std::string::npos &&
          detectBody.find("{pointer.x - 0.5f, pointer.y - 0.5f, 1.0f, 1.0f}") != std::string::npos &&
          hitBody.find("RectsOverlap(panel, left)") != std::string::npos &&
          hitBody.find("RectsOverlap(panel, right)") != std::string::npos &&
          hitBody.find("RectsOverlap(panel, bottom)") != std::string::npos,
          "BDD: Module dock snap detects left, right, and bottom targets at the pointer");
    Check(previewBody.find("aura::ui::dock::PlanDockPreview(") != std::string::npos &&
          planBody.find("case DockSide::Left:") != std::string::npos &&
          planBody.find("case DockSide::Right:") != std::string::npos &&
          planBody.find("case DockSide::Bottom:") != std::string::npos &&
          previewBody.find("overlay->AddRectFilled(") != std::string::npos,
          "BDD: Module dock preview keeps the edge targets available through the shared plan");
    const std::string dragBody = ExtractLambdaBody(
        source, "auto driveFloatingDockDrag = [&](const char* moduleId, PanelState& panel, ImVec2 pos, ImVec2 size)");
    Check(dragBody.find("aura::ui::dock::ShouldEvaluateFloatingDockZone(") != std::string::npos &&
          dragBody.find("g_dockTitleGesture.DropArmed() && movedFromDockOrigin") != std::string::npos &&
          dragBody.find("zone = detectDockZone(pos, size, moduleId);") != std::string::npos &&
          dragBody.find("g_dockDrag.UpdatePreview(zone);") != std::string::npos &&
          dragBody.find("drawDockPreview(titleBarDrag);") != std::string::npos,
          "BDD: Module dock preview follows an armed title drag and current pointer zone");
    Check(dragBody.find("const aura::ui::dock::DockHit commitHit = zone;") != std::string::npos &&
          dragBody.find("g_dockDrag.CommitFloatingOnMouseUp();") != std::string::npos &&
          dragBody.find("g_dockDrag.CommitOnMouseUp();") != std::string::npos &&
          dragBody.find("MirrorDockCommitToPanel(panel, landedSide, landedSide, g_dockDropPlan.rootRect);") != std::string::npos &&
          source.find("driveFloatingDockDrag(\"filter\", state.panelSidebar, pos, size);") != std::string::npos &&
          source.find("driveFloatingDockDrag(\"info\", state.panelFileInfo, pos, size);") != std::string::npos,
          "BDD: Module dock commit uses the release-frame zone for sidebar and info panels");
}

void TestResultListFormatColumnUsesListFontContract() {
    const auto sourcePath = std::filesystem::path(__FILE__).parent_path().parent_path() / "src" / "main.cpp";
    std::ifstream in(sourcePath, std::ios::binary);
    std::stringstream buffer;
    buffer << in.rdbuf();
    const std::string source = buffer.str();

    Check(!source.empty(),
          "BDD: Given result-list font contract validation runs, src/main.cpp can be opened");

    const size_t helperPos = source.find("auto DrawListFontText = [&](const char* text)");
    Check(helperPos != std::string::npos &&
          source.find("if (listFont) ImGui::PushFont(listFont);", helperPos) != std::string::npos &&
          source.find("ImGui::TextUnformatted(text ? text : \"\");", helperPos) != std::string::npos &&
          source.find("if (listFont) ImGui::PopFont();", helperPos) != std::string::npos,
          "BDD: Result-list format text has an explicit list-font drawing helper");

    const size_t browserFormatStart = source.find("// Format (L3)");
    const size_t browserFormatEnd = source.find("// Size (L2, Mono)", browserFormatStart);
    Check(browserFormatStart != std::string::npos && browserFormatEnd != std::string::npos,
          "BDD: Browser result-list format column block is inspectable");
    const std::string browserFormatBlock =
        (browserFormatStart != std::string::npos && browserFormatEnd != std::string::npos)
            ? source.substr(browserFormatStart, browserFormatEnd - browserFormatStart)
            : std::string{};
    Check(browserFormatBlock.find("DrawListFontText(entry.extension.c_str());") != std::string::npos &&
          browserFormatBlock.find("GetUIFontRegular()") == std::string::npos &&
          browserFormatBlock.find("GetUIFontMono()") == std::string::npos &&
          browserFormatBlock.find("ImGui::Text(\"%s\", entry.extension.c_str())") == std::string::npos,
          "BDD: Browser result-list Format column follows list font, not system or mono font");

    const size_t dbFormatStart = source.find("// Format (L3 dark-gray", browserFormatEnd);
    const size_t dbFormatEnd = source.find("// Size (Mono, L2 gray)", dbFormatStart);
    Check(dbFormatStart != std::string::npos && dbFormatEnd != std::string::npos,
          "BDD: Database result-list format column block is inspectable");
    const std::string dbFormatBlock =
        (dbFormatStart != std::string::npos && dbFormatEnd != std::string::npos)
            ? source.substr(dbFormatStart, dbFormatEnd - dbFormatStart)
            : std::string{};
    Check(dbFormatBlock.find("DrawListFontText(record.format.c_str());") != std::string::npos &&
          dbFormatBlock.find("DrawListFontText(ext.c_str());") != std::string::npos &&
          dbFormatBlock.find("DrawListFontText(\"-\");") != std::string::npos &&
          dbFormatBlock.find("GetUIFontRegular()") == std::string::npos &&
          dbFormatBlock.find("GetUIFontMono()") == std::string::npos &&
          dbFormatBlock.find("ImGui::Text(\"%s\", record.format.c_str())") == std::string::npos,
          "BDD: Database result-list Format column follows list font for value, extension fallback, and placeholder");
}

void TestNaturalFilenameSortUsesSharedComparatorContract() {
    const auto utilsHeaderPath = std::filesystem::path(__FILE__).parent_path().parent_path() / "src" / "Utils.h";
    const auto utilsSourcePath = std::filesystem::path(__FILE__).parent_path().parent_path() / "src" / "Utils.cpp";
    const auto dbSourcePath = std::filesystem::path(__FILE__).parent_path().parent_path() / "src" / "Database.cpp";
    const auto mainSourcePath = std::filesystem::path(__FILE__).parent_path().parent_path() / "src" / "main.cpp";

    std::ifstream utilsHeaderInput(utilsHeaderPath, std::ios::binary);
    std::ifstream utilsSourceInput(utilsSourcePath, std::ios::binary);
    std::ifstream dbInput(dbSourcePath, std::ios::binary);
    std::ifstream mainInput(mainSourcePath, std::ios::binary);

    std::stringstream utilsHeaderBuffer;
    std::stringstream utilsSourceBuffer;
    std::stringstream dbBuffer;
    std::stringstream mainBuffer;
    utilsHeaderBuffer << utilsHeaderInput.rdbuf();
    utilsSourceBuffer << utilsSourceInput.rdbuf();
    dbBuffer << dbInput.rdbuf();
    mainBuffer << mainInput.rdbuf();

    const std::string utilsHeader = utilsHeaderBuffer.str();
    const std::string utilsSource = utilsSourceBuffer.str();
    const std::string dbSource = dbBuffer.str();
    const std::string mainSource = mainBuffer.str();

    Check(!utilsHeader.empty() && !utilsSource.empty() && !dbSource.empty() && !mainSource.empty(),
          "BDD: Given natural filename sort contract validation runs, Utils/Database/main sources can be opened");
    Check(utilsHeader.find("int CompareNaturalUtf8(std::string_view lhs, std::string_view rhs);") != std::string::npos,
          "BDD: Utils.h declares a shared natural UTF-8 comparator");
    Check(utilsSource.find("int CompareNaturalUtf8(std::string_view lhs, std::string_view rhs)") != std::string::npos &&
          utilsSource.find("isAsciiDigit") != std::string::npos,
          "BDD: Utils.cpp implements natural compare by recognizing ASCII digit runs");
    Check(dbSource.find("\"AURA_NATURAL\"") != std::string::npos &&
          dbSource.find("idx_files_name_natural") != std::string::npos &&
          dbSource.find("idx_files_folder_name_natural") != std::string::npos &&
          dbSource.find("af.file_name COLLATE AURA_NATURAL") != std::string::npos,
          "BDD: Database filename ordering uses a shared natural collation plus matching indexes");
    Check(mainSource.find("cmp = CompareNaturalUtf8(a.fileName, b.fileName);") != std::string::npos,
          "BDD: UI local filename sorting reuses the shared natural comparator");
    Check(mainSource.find("static int CompareFileEntries(const AppState& state, const FileEntry& a, const FileEntry& b, SortColumn column)") != std::string::npos &&
          mainSource.find("cmp = CompareNaturalUtf8(lowerNameA, lowerNameB);") != std::string::npos,
          "BDD: File browser entry sorting also reuses natural filename comparison instead of lowercase lexicographic order");
    Check(dbSource.find("idx_playlists_name_natural") != std::string::npos &&
          dbSource.find("idx_playlists_parent_name_natural") != std::string::npos &&
          dbSource.find("GROUP BY p.id ORDER BY p.name COLLATE AURA_NATURAL") != std::string::npos &&
          dbSource.find("FROM playlists WHERE parent_id = ? ORDER BY name COLLATE AURA_NATURAL") != std::string::npos,
          "BDD: Playlist names reuse the shared natural collation for top-level and child ordering");
    Check(dbSource.find("ORDER BY af.file_name COLLATE AURA_NATURAL") != std::string::npos &&
          dbSource.find("ORDER BY pi.position, af.file_name COLLATE AURA_NATURAL") != std::string::npos,
          "BDD: Playlist file queries use natural filename ordering instead of af.id or lexicographic file_name order");
}

} // namespace

int main() {
    TestForegroundPlaybackPausesUnrelatedBackgroundIo();
    TestBrowserMiniWaveformWorkerDoesNotRequireOpenDatabaseContract();
    TestWaveformRebuildPausesAllBackgroundIo();
    TestIdleAllowsBoundedBackgroundIo();
    TestInteractionShrinksButDoesNotKillVisibleCoverBudget();
    TestIdleCoverWallPriorityAllowsOffscreenFill();
    TestCoverWallWarmupRaisesOffscreenFillBudget();
    TestCoverWallDragKeepsVisibleCoverLaneAlive();
    TestSelectionCooldownKeepsVisibleCoverLaneAlive();
    TestFirstScreenCursorReArmsAtRestWhenVisibleCellsRemain();
    TestCoverWallFirstScreenPrefetchIsNotGatedByOffscreenCrawlSourceContract();
    TestCoverWallTextureMirrorClearedAtInvalidationNotValidatedEveryFrameContract();
    TestDisplayReloadDoesNotRebuildGlobalCoverWallContract();
    TestSearchRefreshOnlyRequestsCoverWallDisplayReloadForDependentViewsContract();
    TestCoverWallLocalCooldownOnlyTracksConfirmedFailuresContract();
    TestCoverWallRejectsNonCoverKeysAndWaveformTextureReuseContract();
    TestTabCoverFilterStateDoesNotLeakAcrossTabsContract();
    TestTabDbFilterStateDoesNotLeakAcrossTabsContract();
    TestTabSwitchPreservesCoverWallDisplaySnapshotContract();
    TestCoverWallTextureMirrorEligibilityChecksEpochContract();
    TestTabResultOwnershipBoundaryPreventsCoverWallCrossTabRebuildContract();
    TestCoverWallTabLifecycleHasSingleSemanticOwnerContract();
    TestCoverWallTextureMirrorRejectsNonCoverTextureReuseContract();
    TestQaStateExposesCoverWallAndTabRuntimeProbeContract();
    TestHarkenSearchTargetLinksMediaFoundationForRuntimeProbeContract();
    TestCoverWallReloadUsesSemanticEntrypointsContract();
    TestStartupCoverWallWarmupIsBoundedContract();
    TestEmptyGlobalCoverPreloadDoesNotRequeryEveryFrameContract();
    TestCoverWallResetEntrypointsSeparateDatabaseInvalidationContract();
    TestQaScanCompletionRefreshesDatabaseViewContract();
    TestDeferredCoverBackfillRefreshesCoverWallAfterStoppedScanContract();
    TestFilteredCoverWallFallbackUsesTemporaryFolderKeysWhenRowsAlreadyHaveCoverContract();
    TestFilteredCoverWallAsyncFacetLimitMatchesUiContract();
    TestFilteredCoverWallFacetSwapAvoidsSecondProgressiveFillContract();
    TestGlobalCoverWallNoFilterLoadAvoidsFallbackFlashContract();
    TestQaKeywordSearchAvoidsBlockingImmediatePumpContract();
    TestFilterChipQueryRemovalSuppressesRealtimeAutoClearContract();
    TestQaPathAndMetadataFiltersDoNotSuppressFacetFollowupContract();
    TestQaBrowseSurfaceCommandsPreserveReplacementScopeContract();
    TestFilteredRefreshClearsStaleFacetsAtSubmitTimeContract();
    TestFilteredCoverWallDoesNotFlashSearchFallbackWhileFacetFollowupPendingContract();
    TestFilteredCoverWallFastFirstPaintWarmsFacetsBeforePagedListContract();
    TestCoverWallWheelScrollCountsAsActiveInteractionContract();
    TestCoverWallDisplayRefreshRearmsFirstScreenSweepContract();
    TestExplicitRefreshClearsCoverWallFailureCooldownContract();
    TestCoverClickUsesDeferredCountFastPathContract();
    TestInconsistentFilteredRefreshTotalSchedulesRepairContract();
    TestCountAndFacetFollowUpSettlesStandaloneAsyncStateContract();
    TestCountAndFacetWarmStartZeroTotalDefersTruthContract();
    TestNoFilterFacetApplySkipsMainThreadPersistentCacheWritesContract();
    TestGlobalCoverWallIdleAutoAdvanceContract();
    TestFilteredFacetWorkerCacheAdmitsPersistentFilterSemanticsContract();
    TestGlobalCoverWallCacheStoresOnlyCompletePagedWallContract();
    TestStartupSettlePausesListMiniWaveSchedulingContract();
    TestGlobalCoverWallVisibleBandVirtualizationContract();
    TestGlobalCoverWallPreRequestFailuresEnterCooldownContract();
    TestAdaptiveThumbnailKeywordCoverWallWarmupBudgetContract();
    TestCoverWallVisibleTextureAttemptsConsumeBudgetContract();
    TestCoverWallFailureCooldownUsesWallClockContract();
    TestCurrentFolderCoverFailureDiagnosticsAreSampledContract();
    TestGlobalCoverWallCompletenessCeilingAndDiagnosticsContract();
    TestQaCommandJsonUnescapeAcceptsUnicodePathsContract();
    TestCoverWallFailedFolderStableKeysPreserveBrowseListContract();
    TestCoverWallPerfProbeHasDeepHoldModeContract();
    TestCoverBrowseSurfaceProbeScriptContract();
    TestFrameSpikeDiagnosticsPreservePreviousFramePhaseContract();
    TestPlaybackThrottlesMainThreadMiniAndCoverUploadsContract();
    TestPlaybackAndIdleTakeoverStatePolicy();
    TestIdleAssetBackfillForegroundSurfaceIsProgressOnlyContract();
    TestDawRunningProbeNeverBlocksRenderThreadContract();
    TestCoverPrefetchYieldsToCoverWallPriorityContract();
    TestCoverColumnUsesCachedTexturesDuringCoverWallPriorityContract();
    TestSearchResultWarmStartCoverPreloadYieldsToGlobalCoverWallContract();
    TestHarnessLaneKeepsStartupLoadingGateContract();
    TestCurrentCoverPreviewFailureCooldownContract();
    TestListAndPrefetchSkipKnownFailedEmbeddedCoversContract();
    TestAutomaticMainDetailPrewarmIsOptInOnly();
    TestAutomaticCoverPhashBackfillIsOptInOnly();
    TestFilteredCoverWallRetirementNarrowsLegacySearchFallbackContract();
    TestModuleDockEdgeSnapRejectsNestedPanelSplitsContract();
    TestResultListFormatColumnUsesListFontContract();
    TestNaturalFilenameSortUsesSharedComparatorContract();

    if (g_failures != 0) {
        std::cerr << g_failures << " background IO policy checks failed\n";
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
