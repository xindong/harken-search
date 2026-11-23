#include "TaskProgressToastLayout.h"
#include "TaskProgressToastSources.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

int g_failures = 0;

void Check(bool condition, const char* what) {
    if (condition) return;
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++g_failures;
}

bool Nearly(float a, float b) { return std::fabs(a - b) < 0.01f; }

void TestPanelAndRowGeometry() {
    TaskProgressToastStack stack;
    stack.Report("scan", "Scan", "Indexing", 0.5f, false, 0.0);
    const auto panel = stack.PanelRect(0.0f, 0.0f, 1000.0f, 800.0f, 1);
    Check(Nearly(panel.width, 280.0f), "panel width is 280");
    Check(Nearly(panel.height, 52.0f + 16.0f), "panel height is one row plus padding");
    Check(Nearly(panel.x, 1000.0f - 16.0f - 280.0f), "panel hugs the right edge");
    Check(Nearly(panel.y, 800.0f - 16.0f - (52.0f + 16.0f)), "panel hugs the bottom edge");

    const auto row = stack.RowRect(panel, 0);
    Check(Nearly(row.x, panel.x + 8.0f), "row is inset by panel padding");
    Check(Nearly(row.y, panel.y + 8.0f), "row starts below panel padding");
    Check(Nearly(row.width, 280.0f - 16.0f), "row width accounts for padding");
    Check(Nearly(row.height, 52.0f), "row height follows metrics");

    const auto emptyPanel = stack.PanelRect(0.0f, 0.0f, 1000.0f, 800.0f, 0);
    Check(emptyPanel.width == 0.0f && emptyPanel.height == 0.0f,
          "empty stack reports a zero-sized panel");

    TaskProgressToastStack narrow;
    const auto clipped = narrow.PanelRect(0.0f, 0.0f, 120.0f, 90.0f, 3);
    Check(clipped.width <= 120.0f && clipped.height <= 90.0f,
          "panel never exceeds a small work area");
}

void TestHitTesting() {
    TaskProgressToastStack stack;
    stack.Report("scan", "Scan", "Indexing", 0.5f, false, 0.0);
    stack.Report("similar", "Similar", "Analyzing", 0.2f, false, 0.0);
    const auto rows = stack.VisibleRows(0.0);
    Check(rows.size() == 2, "two rows are visible");

    const auto panel = stack.PanelRect(0.0f, 0.0f, 1000.0f, 800.0f, rows.size());
    const auto row0 = stack.RowRect(panel, 0);
    const auto row1 = stack.RowRect(panel, 1);

    const auto inside0 = HitTestTaskProgressToast(
        stack, panel, {row0.x + 20.0f, row0.y + 20.0f}, rows.size(), 0.0);
    Check(inside0.panel && inside0.row && !inside0.close, "row body hits without close");
    Check(inside0.id == "scan", "first row keeps oldest entry first");

    const auto inside1 = HitTestTaskProgressToast(
        stack, panel, {row1.x + 20.0f, row1.y + 20.0f}, rows.size(), 0.0);
    Check(inside1.row && inside1.id == "similar", "second row maps to the second entry");

    const auto close0 = ToastCloseButtonRect(stack, row0);
    const auto closeHit = HitTestTaskProgressToast(
        stack, panel, {close0.x + 4.0f, close0.y + 4.0f}, rows.size(), 0.0);
    Check(closeHit.close && closeHit.closeId == "scan", "close target dismisses its row");

    const auto outside = HitTestTaskProgressToast(
        stack, panel, {10.0f, 10.0f}, rows.size(), 0.0);
    Check(!outside.panel && !outside.row, "pointer outside the panel hits nothing");
}

void TestCloseTargetStaysInsideRow() {
    TaskProgressToastStack stack;
    const auto panel = stack.PanelRect(0.0f, 0.0f, 1000.0f, 800.0f, 1);
    const auto row = stack.RowRect(panel, 0);
    const auto close = ToastCloseButtonRect(stack, row);
    Check(close.x >= row.x && close.x + close.width <= row.x + row.width,
          "close target stays inside the row");
    Check(Nearly(close.y, row.y + (row.height - close.height) * 0.5f),
          "close target is vertically centered");
}

void TestPercentFormatting() {
    char buffer[16];
    Check(FormatTaskProgressPercent(0.42f, buffer, sizeof buffer), "0.42 is determinate");
    Check(std::string(buffer) == "42%", "0.42 formats as 42%");
    Check(FormatTaskProgressPercent(1.0f, buffer, sizeof buffer) &&
              std::string(buffer) == "100%",
          "full progress formats as 100%");
    Check(!FormatTaskProgressPercent(-1.0f, buffer, sizeof buffer),
          "negative fraction is indeterminate");
    Check(!FormatTaskProgressPercent(0.5f, buffer, 0), "zero capacity is rejected");
}

void TestIndeterminateSweepStaysInRange() {
    for (int i = 0; i < 40; ++i) {
        const float sweep = TaskProgressIndeterminateSweep(static_cast<double>(i) * 0.13);
        Check(sweep >= 0.0f && sweep <= 1.0f, "sweep stays within 0..1");
    }
    Check(TaskProgressIndeterminateSweep(0.0) == 0.0f, "sweep starts at 0");
}

void TestSourceSyncLifecycle() {
    TaskProgressToastStack stack;
    TaskProgressSyncState sync;
    TaskProgressSourcesSnapshot snapshot;

    snapshot.scan.running = true;
    snapshot.scan.title = "Scan";
    snapshot.scan.phase = "Indexing";
    snapshot.scan.fraction = 0.25f;
    SyncTaskProgressToast(stack, sync, snapshot, 0.0);
    SyncTaskProgressToast(stack, sync, snapshot, 0.1);
    Check(stack.Size() == 1, "repeated frames keep a single row per source");
    Check(stack.VisibleRows(0.1).size() == 1, "running source is visible");

    snapshot.scan.running = false;
    snapshot.scan.finished = true;
    SyncTaskProgressToast(stack, sync, snapshot, 1.0);
    const auto finished = stack.VisibleRows(1.0);
    Check(finished.size() == 1 && finished[0].state == TaskProgressState::Succeeded,
          "ending source holds a success row");
    Check(Nearly(stack.FadeAlpha(finished[0], 1.0), 1.0f), "success row is fully opaque first");
    stack.Tick(1.0 + 3.0 + 0.4 + 0.01);
    Check(stack.Size() == 0, "success row is dropped after hold and fade");
}

void TestInlineSourceIsNotDuplicated() {
    TaskProgressToastStack stack;
    TaskProgressSyncState sync;
    TaskProgressSourcesSnapshot snapshot;

    snapshot.similar.running = true;
    snapshot.similar.title = "Similar";
    snapshot.similar.fraction = 0.1f;
    SyncTaskProgressToast(stack, sync, snapshot, 0.0);
    Check(stack.VisibleRows(0.0).size() == 1, "source without inline progress shows a row");

    stack.Dismiss("similar");
    snapshot.similar.inlineProgressVisible = true;
    SyncTaskProgressToast(stack, sync, snapshot, 0.1);
    Check(stack.VisibleRows(0.1).empty(), "inline source is suppressed");

    snapshot.similar.running = false;
    snapshot.similar.finished = true;
    SyncTaskProgressToast(stack, sync, snapshot, 0.2);
    Check(stack.Size() == 0, "inline source leaves no stale success row behind");
}

void TestFailureStaysUntilDismissed() {
    TaskProgressToastStack stack;
    TaskProgressSyncState sync;
    TaskProgressSourcesSnapshot snapshot;

    snapshot.removeFolder.running = true;
    snapshot.removeFolder.title = "Remove";
    SyncTaskProgressToast(stack, sync, snapshot, 0.0);

    snapshot.removeFolder.running = false;
    snapshot.removeFolder.failed = true;
    SyncTaskProgressToast(stack, sync, snapshot, 1.0);
    const auto rows = stack.VisibleRows(1.0);
    Check(rows.size() == 1 && rows[0].state == TaskProgressState::Failed,
          "failure is reported once");
    stack.Tick(1000.0);
    Check(stack.Size() == 1, "failure never expires on its own");
    stack.Dismiss("remove-folder");
    Check(stack.Size() == 0, "failure dismisses on request");
}

void TestStackCapsThreeRows() {
    TaskProgressToastStack stack;
    TaskProgressSyncState sync;
    TaskProgressSourcesSnapshot snapshot;

    snapshot.scan.running = true;
    snapshot.scan.title = "Scan";
    snapshot.similar.running = true;
    snapshot.similar.title = "Similar";
    snapshot.removeFolder.running = true;
    snapshot.removeFolder.title = "Remove";
    snapshot.waveformRebuild.running = true;
    snapshot.waveformRebuild.title = "Waveform";
    snapshot.coverRebuild.running = true;
    snapshot.coverRebuild.title = "Cover";
    SyncTaskProgressToast(stack, sync, snapshot, 0.0);

    const auto rows = stack.VisibleRows(0.0);
    Check(rows.size() == 3, "at most three rows are painted");
    Check(rows.back().id == "cover-rebuild", "newest source sits closest to the corner");
}

} // namespace

int main() {
    TestPanelAndRowGeometry();
    TestHitTesting();
    TestCloseTargetStaysInsideRow();
    TestPercentFormatting();
    TestIndeterminateSweepStaysInRange();
    TestSourceSyncLifecycle();
    TestInlineSourceIsNotDuplicated();
    TestFailureStaysUntilDismissed();
    TestStackCapsThreeRows();

    if (g_failures != 0) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("TaskProgressToast UI checks passed\n");
    return 0;
}