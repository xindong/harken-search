#pragma once

#include "ScanWorkflow.h"
#include <array>
#include <string>
#include <vector>

namespace HarkenSearch {

inline int ScanProgressPercent(int current, int total) {
    return total > 0 ? static_cast<int>(100LL * std::clamp(current, 0, total) / total) : 0;
}

// Project legacy cleanup counters without changing its worker or stop policy.
inline ScanWorkflowSnapshot BuildCleanupScanSnapshot(bool active, bool stopped, bool failed, int current, int total) {
    ScanWorkflowSnapshot snapshot;
    snapshot.discovered = std::max(0, total);
    // The cleanup callback is throttled; successful worker exit, not its last
    // sampled counter, is the completion authority (especially for tiny tasks).
    snapshot.processed = !active && !stopped && !failed ? snapshot.discovered : std::clamp(current, 0, snapshot.discovered);
    snapshot.skipped = snapshot.processed;
    snapshot.foregroundTasksDrained = !active;
    snapshot.stage = active ? ScanWorkflowStage::Ingesting : failed ? ScanWorkflowStage::Failed :
        stopped ? ScanWorkflowStage::Cancelled : ScanWorkflowStage::Completed;
    return snapshot;
}

inline std::string ScanDiagnosticTailUtf8(const std::string& text, size_t limit) {
    if (text.size() <= limit) return text;
    size_t offset = text.size() - limit;
    while (offset < text.size() && (static_cast<unsigned char>(text[offset]) & 0xc0) == 0x80) ++offset;
    return text.substr(offset);
}

// UI-only projection. It never changes completion gates, counters or stop policy.
enum class ScanDialogSurface { Setup, Progress, Result };
enum class ScanDialogPhase {
    Setup, Counting, Ingesting, Waveforms, Finalizing, Saving, Revoking,
    Completed, Partial, Paused, Cancelled, Empty, Failed
};
struct ScanDialogInput {
    ScanWorkflowSnapshot workflow;
    bool scanning = false;
    bool completionPending = false;
    ScanStopDisposition stopDisposition = ScanStopDisposition::None;
    bool fatalFailure = false;
    int reportedErrors = 0;
};
struct ScanDialogView {
    ScanDialogSurface surface = ScanDialogSurface::Setup;
    ScanDialogPhase phase = ScanDialogPhase::Setup;
    bool progressKnown = false;
    int current = 0, total = 0;
    float fraction = 0.f;
    int succeeded = 0, failed = 0, skipped = 0, unprocessed = 0;
    bool canBackground = false, canStop = false, canClose = true;
};

struct ScanDialogMetricsSnapshot {
    std::int64_t elapsedMs = 0;
    double filesPerSecond = 0.;
    bool speedKnown = false;
};

// UI-thread sampler: no worker/database changes. Poll even when the popup is
// hidden, so background progress and reading a result do not distort timing.
class ScanDialogMetrics {
public:
    const ScanDialogMetricsSnapshot& Value() const { return value_; }
    ScanDialogMetricsSnapshot Update(std::uint64_t task, std::int64_t startedAt,
                                    std::int64_t completedMs, std::int64_t now,
                                    const ScanDialogView& view) {
        if (view.surface == ScanDialogSurface::Setup) {
            *this = {};
            return value_;
        }
        if (!initialized_ || task != task_ || startedAt != startedAt_) {
            *this = {};
            initialized_ = true;
            task_ = task;
            startedAt_ = startedAt;
        }
        if (view.surface == ScanDialogSurface::Result) {
            if (!resultLatched_) {
                value_.elapsedMs = completedMs >= 0 ? completedMs : Elapsed(now);
                resultLatched_ = true;
            }
            value_.speedKnown = false;
            value_.filesPerSecond = 0.;
            return value_;
        }
        value_.elapsedMs = Elapsed(now);
        const bool measurable = view.phase == ScanDialogPhase::Ingesting ||
            view.phase == ScanDialogPhase::Waveforms;
        if (!measurable || !sampling_ || phase_ != view.phase ||
            view.current < lastCount_ || now < lastSampleAt_) {
            value_.speedKnown = false;
            value_.filesPerSecond = 0.;
            sampling_ = measurable;
            phase_ = view.phase;
            lastCount_ = view.current;
            lastSampleAt_ = now;
        } else if (now - lastSampleAt_ >= 250) {
            value_.filesPerSecond = 1000. * (view.current - lastCount_) / (now - lastSampleAt_);
            value_.speedKnown = true;
            lastCount_ = view.current;
            lastSampleAt_ = now;
        }
        return value_;
    }
private:
    std::int64_t Elapsed(std::int64_t now) const {
        return startedAt_ > 0 ? std::max<std::int64_t>(0, now - startedAt_) : 0;
    }
    ScanDialogMetricsSnapshot value_;
    bool initialized_ = false, sampling_ = false, resultLatched_ = false;
    std::uint64_t task_ = 0;
    std::int64_t startedAt_ = 0, lastSampleAt_ = 0;
    int lastCount_ = 0;
    ScanDialogPhase phase_ = ScanDialogPhase::Setup;
};

inline ScanDialogView BuildScanDialogView(const ScanDialogInput& input) {
    ScanDialogView view;
    const auto& snapshot = input.workflow;
    const auto counts = BuildScanOutcomeCounts(snapshot.discovered, snapshot.processed,
        snapshot.succeeded, snapshot.skipped, snapshot.failed);
    view.succeeded = counts.succeeded;
    view.failed = std::max(counts.failed, std::max(0, input.reportedErrors));
    view.skipped = counts.skipped;
    view.unprocessed = counts.discovered - counts.processed;
    if (input.scanning || input.completionPending) {
        view.surface = ScanDialogSurface::Progress;
        view.canClose = false;
        if (IsScanPause(input.stopDisposition)) {
            view.phase = ScanDialogPhase::Saving;
        } else if (IsScanRollbackCancel(input.stopDisposition)) {
            view.phase = ScanDialogPhase::Revoking;
        } else {
            view.canStop = true;
            if (snapshot.stage == ScanWorkflowStage::CachingMiniWaveforms) {
                view.phase = ScanDialogPhase::Waveforms;
                view.total = std::max(0, snapshot.miniWaveformsTotal);
                view.current = std::clamp(snapshot.miniWaveformsCompleted, 0, view.total);
                view.progressKnown = view.total > 0;
            } else if (input.completionPending || snapshot.stage == ScanWorkflowStage::Finalizing ||
                       snapshot.stage == ScanWorkflowStage::Completed) {
                view.phase = ScanDialogPhase::Finalizing;
            } else {
                view.canBackground = true;
                view.current = counts.processed;
                view.total = counts.discovered;
                view.phase = snapshot.stage == ScanWorkflowStage::Ingesting
                    ? ScanDialogPhase::Ingesting : ScanDialogPhase::Counting;
                view.progressKnown = view.phase == ScanDialogPhase::Ingesting && view.total > 0;
            }
        }
        if (view.progressKnown) view.fraction = float(view.current) / float(view.total);
        return view;
    }
    if (snapshot.stage == ScanWorkflowStage::Idle && !input.fatalFailure && view.failed == 0) return view;
    view.surface = ScanDialogSurface::Result;
    if (input.fatalFailure || snapshot.stage == ScanWorkflowStage::Failed) view.phase = ScanDialogPhase::Failed;
    else if (snapshot.stage == ScanWorkflowStage::Paused) view.phase = ScanDialogPhase::Paused;
    else if (snapshot.stage == ScanWorkflowStage::Cancelled) view.phase = ScanDialogPhase::Cancelled;
    else if (view.failed > 0) view.phase = ScanDialogPhase::Partial;
    else if (snapshot.stage == ScanWorkflowStage::Completed && CanEnterCompleted(snapshot))
        view.phase = counts.discovered == 0 ? ScanDialogPhase::Empty : ScanDialogPhase::Completed;
    else {
        // A stale/incomplete terminal snapshot is not evidence of completion.
        view.surface = ScanDialogSurface::Progress;
        view.phase = ScanDialogPhase::Finalizing;
        view.canClose = false;
        view.canStop = true;
    }
    return view;
}

enum class ScanFailureCategory { Path, Read, Empty, Save, Other };
inline std::string ScanFailureSubject(const std::string& error) {
    const auto marker = error.find(": ");
    if (marker == std::string::npos) return {};
    auto path = error.substr(marker + 2);
    const bool absolute = (!path.empty() && (path[0] == '/' || path[0] == '\\')) ||
        (path.size() > 2 && path[1] == ':' && (path[2] == '/' || path[2] == '\\'));
    if (!absolute) return {};
    const auto detail = path.rfind(" (");
    if (detail != std::string::npos && path.back() == ')') path.resize(detail);
    while (!path.empty() && (path.back() == '/' || path.back() == '\\')) path.pop_back();
    const auto slash = path.find_last_of("/\\");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}
inline ScanFailureCategory ClassifyScanFailure(const std::string& error) {
    const auto starts = [&](const char* prefix) { return error.rfind(prefix, 0) == 0; };
    if (starts("Scan_NetworkLibraryUnreachable") || starts("Cannot read directory") ||
        starts("Failed to scan directory") || starts("Path not found") || starts("File not found"))
        return ScanFailureCategory::Path;
    if (starts("No audio samples") || starts("No usable audio") || starts("Empty audio") || starts("Audio file too small to decode"))
        return ScanFailureCategory::Empty;
    if (starts("Failed to decode") || starts("Failed to read audio") || starts("Cannot decode") ||
        starts("Cannot read audio") || starts("Failed to open audio"))
        return ScanFailureCategory::Read;
    if (starts("Database write failed") || starts("Failed to save") || starts("Failed to seed") ||
        starts("Failed to snapshot rollback") || starts("Scan cancelled but rollback failed"))
        return ScanFailureCategory::Save;
    return ScanFailureCategory::Other;
}
inline std::array<int, 5> BuildScanFailureSummary(const std::vector<std::string>& errors, int total) {
    std::array<int, 5> counts{};
    int remaining = std::max(0, total);
    for (const auto& error : errors) {
        if (remaining == 0) break;
        if (error.empty()) continue;
        ++counts[static_cast<size_t>(ClassifyScanFailure(error))];
        --remaining;
    }
    // The existing scan error log is capped. Never invent reasons for omissions.
    counts[static_cast<size_t>(ScanFailureCategory::Other)] += remaining;
    return counts;
}

} // namespace HarkenSearch
