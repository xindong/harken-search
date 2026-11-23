// Include the public interface first so it must be self-contained.
#include "TaskProgressToastModel.h"

#include <cassert>
#include <cmath>
#include <string>

namespace {

bool Near(float a, float b) { return std::fabs(a - b) < 0.01f; }

void TestAnchoringAndRowGeometry() {
    TaskProgressToastStack stack;
    const auto empty = stack.PanelRect(0.0f, 0.0f, 1280.0f, 720.0f, 0);
    assert(Near(empty.width, 0.0f));
    assert(Near(empty.height, 0.0f));
    assert(Near(empty.x, 1264.0f));
    assert(Near(empty.y, 704.0f));

    const auto panel = stack.PanelRect(0.0f, 0.0f, 1280.0f, 720.0f, 1);
    assert(Near(panel.width, 280.0f));
    assert(Near(panel.height, 68.0f)); // 8 + 52 + 8
    assert(Near(panel.x, 1280.0f - 16.0f - 280.0f));
    assert(Near(panel.y, 720.0f - 16.0f - 68.0f));

    const auto first = stack.RowRect(panel, 0);
    assert(Near(first.x, panel.x + 8.0f));
    assert(Near(first.y, panel.y + 8.0f));
    assert(Near(first.width, 264.0f));
    assert(Near(first.height, 52.0f));

    const auto second = stack.RowRect(panel, 1);
    assert(Near(second.y, first.y + 52.0f + 6.0f));

    // Three rows grow upward from the same bottom-right corner.
    const auto tall = stack.PanelRect(0.0f, 0.0f, 1280.0f, 720.0f, 3);
    assert(Near(tall.height, 2.0f * 8.0f + 3.0f * 52.0f + 2.0f * 6.0f));
    assert(Near(tall.y + tall.height, panel.y + panel.height));
    assert(Near(tall.x, panel.x));

    // A narrow work area clamps the panel instead of overflowing it.
    const auto narrow = stack.PanelRect(0.0f, 0.0f, 200.0f, 120.0f, 2);
    assert(narrow.x >= 0.0f);
    assert(narrow.x + narrow.width <= 200.0f);
    assert(narrow.y >= 0.0f);
    assert(narrow.y + narrow.height <= 120.0f);
}

void TestReportRefreshesInsteadOfDuplicating() {
    TaskProgressToastStack stack;
    stack.Report("scan", "scan", "12 / 48", 0.25f, false, 10.0);
    stack.Report("scan", "scan", "30 / 48", 0.62f, false, 11.0);
    assert(stack.Size() == 1);
    auto rows = stack.VisibleRows(11.0);
    assert(rows.size() == 1);
    assert(rows[0].phase == "30 / 48");
    assert(Near(rows[0].fraction, 0.62f));
    assert(rows[0].state == TaskProgressState::Active);

    // A negative fraction stays indeterminate rather than becoming zero.
    stack.Report("similar", "similar", "matching", -1.0f, false, 12.0);
    rows = stack.VisibleRows(12.0);
    assert(rows.size() == 2);
    assert(rows[1].fraction < 0.0f);
}

void TestSuccessHoldFadeAndExpiry() {
    TaskProgressToastStack stack;
    stack.Report("cover", "cover", "40 / 40", 1.0f, false, 100.0);
    stack.Complete("cover", 101.0);

    auto rows = stack.VisibleRows(101.0);
    assert(rows.size() == 1);
    assert(rows[0].state == TaskProgressState::Succeeded);
    assert(Near(stack.FadeAlpha(rows[0], 101.0f), 1.0f));
    assert(Near(stack.FadeAlpha(rows[0], 104.0), 1.0f)); // still holding
    assert(Near(stack.FadeAlpha(rows[0], 104.2), 0.5f)); // mid fade
    assert(Near(stack.FadeAlpha(rows[0], 105.0), 0.0f)); // fade complete

    stack.Tick(104.0);
    assert(stack.Size() == 1);
    stack.Tick(103.4 + 3.0 + 0.4);
    assert(stack.Size() == 0);
    assert(stack.VisibleRows(110.0).empty());
}

void TestFailurePersistsUntilDismissed() {
    TaskProgressToastStack stack;
    stack.Report("remove-path", "remove path", "3 / 7", 0.4f, false, 20.0);
    stack.Fail("remove-path", 21.0);

    auto rows = stack.VisibleRows(21.0);
    assert(rows.size() == 1);
    assert(rows[0].state == TaskProgressState::Failed);
    assert(rows[0].detailAvailable);
    assert(Near(stack.FadeAlpha(rows[0], 21.0f), 1.0f));

    stack.Tick(21.0 + 60.0);
    assert(stack.Size() == 1);
    assert(stack.VisibleRows(81.0).size() == 1);

    stack.Dismiss("remove-path");
    assert(stack.Size() == 0);
    assert(!stack.Has("remove-path"));
}

void TestInlineProgressSuppressesTheDuplicate() {
    TaskProgressToastStack stack;
    stack.Report("waveform", "waveform", "5 / 100", 0.05f, true, 30.0);
    stack.Report("cover", "cover", "1 / 4", 0.25f, false, 30.0);
    assert(stack.Size() == 2); // both tracked, one suppressed
    auto rows = stack.VisibleRows(30.0);
    assert(rows.size() == 1);
    assert(rows[0].id == "cover");

    stack.SetInlineProgressVisible("waveform", false);
    rows = stack.VisibleRows(30.0);
    assert(rows.size() == 2);

    stack.SetInlineProgressVisible("cover", true);
    rows = stack.VisibleRows(30.0);
    assert(rows.size() == 1);
    assert(rows[0].id == "waveform");
}

void TestStackKeepsAtMostThreeNewestRows() {
    TaskProgressToastStack stack;
    stack.Report("a", "a", "1 / 4", 0.25f, false, 40.0);
    stack.Report("b", "b", "1 / 4", 0.25f, false, 41.0);
    stack.Report("c", "c", "1 / 4", 0.25f, false, 42.0);
    stack.Report("d", "d", "1 / 4", 0.25f, false, 43.0);
    const auto rows = stack.VisibleRows(43.0);
    assert(rows.size() == 3);
    assert(rows[0].id == "b");
    assert(rows[1].id == "c");
    assert(rows[2].id == "d");
    const auto panel = stack.PanelRect(0.0f, 0.0f, 1280.0f, 720.0f, rows.size());
    assert(Near(panel.height, 2.0f * 8.0f + 3.0f * 52.0f + 2.0f * 6.0f));
}

void TestCompletedTaskRestartsAsActive() {
    TaskProgressToastStack stack;
    stack.Report("patch", "patch", "2 / 9", 0.2f, false, 50.0);
    stack.Complete("patch", 51.0);
    stack.Report("patch", "patch", "5 / 9", 0.55f, false, 52.0);
    const auto rows = stack.VisibleRows(52.0);
    assert(rows.size() == 1);
    assert(rows[0].state == TaskProgressState::Active);
    assert(Near(rows[0].fraction, 0.55f));
    stack.Tick(52.0 + 30.0);
    assert(stack.Size() == 1);
}

} // namespace

int main() {
    TestAnchoringAndRowGeometry();
    TestReportRefreshesInsteadOfDuplicating();
    TestSuccessHoldFadeAndExpiry();
    TestFailurePersistsUntilDismissed();
    TestInlineProgressSuppressesTheDuplicate();
    TestStackKeepsAtMostThreeNewestRows();
    TestCompletedTaskRestartsAsActive();
    return 0;
}
