#pragma once

#include <algorithm>
#include <cstdint>

#include "ScanInterruption.h"

namespace HarkenSearch {

// @stage7-contract
// API contract: scan workflow helpers are pure synchronous state projections.
// They borrow snapshots only for each call, return owned counts/progress/stage
// values, and perform no scan/database mutation. Completion remains gated by
// processed totals, foreground drain, and required mini-waveform completion.

enum class ScanWorkflowStage : std::uint8_t {
    Idle = 0,
    Scanning,
    Ingesting,
    CachingMiniWaveforms,
    Finalizing,
    Completed,
    Paused,
    Cancelled,
    Failed,
};

inline const char* ScanWorkflowStageName(ScanWorkflowStage stage) {
    switch (stage) {
        case ScanWorkflowStage::Scanning: return "scanning";
        case ScanWorkflowStage::Ingesting: return "ingesting";
        case ScanWorkflowStage::CachingMiniWaveforms: return "cachingMiniWaveforms";
        case ScanWorkflowStage::Finalizing: return "finalizing";
        case ScanWorkflowStage::Completed: return "completed";
        case ScanWorkflowStage::Paused: return "paused";
        case ScanWorkflowStage::Cancelled: return "cancelled";
        case ScanWorkflowStage::Failed: return "failed";
        case ScanWorkflowStage::Idle:
        default:
            return "idle";
    }
}

struct ScanWorkflowSnapshot {
    ScanWorkflowStage stage = ScanWorkflowStage::Idle;
    int discovered = 0;
    int processed = 0;
    int succeeded = 0;
    int skipped = 0;
    int failed = 0;
    int miniWaveformsCompleted = 0;
    int miniWaveformsTotal = 0;
    bool miniWaveformsRequired = false;
    bool miniWaveformsContinueInBackground = false;
    bool foregroundTasksDrained = false;
    std::int64_t durationMs = 0;
};

struct ScanOutcomeCounts {
    int discovered = 0;
    int processed = 0;
    int succeeded = 0;
    int skipped = 0;
    int failed = 0;
};

inline ScanOutcomeCounts BuildScanOutcomeCounts(int discovered,
                                                int processed,
                                                int succeeded,
                                                int reportedSkipped,
                                                int failed) {
    ScanOutcomeCounts out;
    out.discovered = std::max(0, discovered);
    out.processed = std::clamp(processed, 0, out.discovered);
    out.failed = std::clamp(failed, 0, out.processed);
    out.succeeded = std::clamp(succeeded, 0, out.processed - out.failed);
    const int remaining = out.processed - out.succeeded - out.failed;
    const int explicitSkipped = std::clamp(reportedSkipped, 0, remaining);
    // Any processed item not classified by the lower layer is surfaced as
    // skipped/duplicate/invalid. This keeps the result partition truthful and
    // guarantees succeeded + skipped + failed == processed.
    out.skipped = explicitSkipped + (remaining - explicitSkipped);
    return out;
}

inline bool CanEnterCompleted(const ScanWorkflowSnapshot& snapshot) {
    const int discovered = std::max(0, snapshot.discovered);
    const int processed = std::clamp(snapshot.processed, 0, discovered);
    if (processed != discovered || !snapshot.foregroundTasksDrained) return false;

    if (snapshot.miniWaveformsRequired) {
        const int miniTotal = std::max(0, snapshot.miniWaveformsTotal);
        const int miniCompleted = std::clamp(
            snapshot.miniWaveformsCompleted, 0, miniTotal);
        if (miniCompleted != miniTotal) return false;
    }
    return true;
}

// Fast scans index files first and deliberately continue optional cover/mini
// work through the durable backfill queue. Once every discovered file has
// been processed, that optional work must not keep the foreground scan dialog
// in Finalizing; full scans retain the stricter foreground contract.
inline bool ShouldReleaseFastScanCompletionGate(
    int discovered,
    int processed,
    bool fastMode) {
    const int safeDiscovered = std::max(0, discovered);
    return fastMode && processed >= safeDiscovered;
}

inline bool AreScanCompletionCountersReady(bool fastMode,
                                           int miniUnresolved,
                                           int coverUnresolved,
                                           int metadataUnresolved) {
    return miniUnresolved == 0 && metadataUnresolved == 0 &&
        (fastMode || coverUnresolved == 0);
}

// Scan ingestion already queues durable background jobs in memory. Rebuilding
// that populated queue before leaving the scan dialog is redundant and can
// block behind disk/database contention. An empty queue still needs recovery.
inline bool ShouldRebuildScanBackfillQueue(bool fastMode,
                                           int queuedJobs,
                                           int inFlight) {
    return !fastMode || (queuedJobs <= 0 && inFlight <= 0);
}

inline bool ShouldPublishScanCompletionToast(
    bool wasScanning,
    bool scanningNow,
    int discovered,
    bool completionPending,
    ScanStopDisposition stopDisposition) {
    return wasScanning && !scanningNow && discovered > 0 &&
        !completionPending && stopDisposition == ScanStopDisposition::None;
}

inline ScanWorkflowStage ResolveTerminalScanWorkflowStage(
    const ScanWorkflowSnapshot& snapshot,
    ScanStopDisposition stopDisposition,
    bool fatalFailure) {
    if (fatalFailure) return ScanWorkflowStage::Failed;
    if (IsScanPause(stopDisposition)) return ScanWorkflowStage::Paused;
    if (IsScanRollbackCancel(stopDisposition)) {
        return ScanWorkflowStage::Cancelled;
    }
    if (snapshot.processed < snapshot.discovered) {
        return ScanWorkflowStage::Ingesting;
    }
    if (snapshot.miniWaveformsRequired &&
        snapshot.miniWaveformsCompleted < snapshot.miniWaveformsTotal) {
        return ScanWorkflowStage::CachingMiniWaveforms;
    }
    if (!snapshot.foregroundTasksDrained) {
        return ScanWorkflowStage::Finalizing;
    }
    return CanEnterCompleted(snapshot)
        ? ScanWorkflowStage::Completed
        : ScanWorkflowStage::Finalizing;
}

struct ScanProgressView {
    int current = 0;
    int total = 0;
    float fraction = 0.0f;
};

inline bool ShouldStartPostScanSimilarityIndex(
    bool optionEnabled,
    const ScanWorkflowSnapshot& snapshot,
    ScanStopDisposition stopDisposition,
    bool fatalFailure) {
    if (!optionEnabled || fatalFailure) {
        return false;
    }
    if (stopDisposition != ScanStopDisposition::None) return false;
    if (snapshot.failed > 0) return false;
    if (snapshot.discovered <= 0 || snapshot.processed <= 0) return false;
    if (snapshot.stage != ScanWorkflowStage::Completed) return false;
    return CanEnterCompleted(snapshot);
}

inline bool ShouldConsumePendingPostScanSimilarityIndex(
    bool pending,
    bool dbSwitchInProgress,
    bool similarityIndexAlreadyRunning) {
    return pending && !dbSwitchInProgress && !similarityIndexAlreadyRunning;
}

inline ScanProgressView BuildScanProcessingProgress(
    const ScanWorkflowSnapshot& snapshot) {
    ScanProgressView view;
    view.total = std::max(0, snapshot.discovered);
    view.current = std::clamp(snapshot.processed, 0, view.total);
    if (snapshot.stage == ScanWorkflowStage::Completed &&
        CanEnterCompleted(snapshot)) {
        view.current = view.total;
    }
    view.fraction = view.total > 0
        ? static_cast<float>(view.current) / static_cast<float>(view.total)
        : ((snapshot.stage == ScanWorkflowStage::Completed) ? 1.0f : 0.0f);
    return view;
}

} // namespace HarkenSearch
