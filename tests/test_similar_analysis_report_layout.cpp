// Directed tests for the Similar Sounds "analysis report" geometry.
//
// The defect: the dialog opened at 660x380 and never remembered a resize, so
// the failure list sat in a ~200 px strip. These cases pin the replacement
// behaviour: a 1180x780 default that survives a restart, a dialog that always
// lands inside the main window's work area, and one row per failure.

#include "src/SimilarAnalysisReportLayout.h"

#include <cmath>
#include <cstdio>
#include <limits>

namespace report_layout = HarkenSearch::similar_analysis_report;

namespace {

int g_failures = 0;

void Check(bool condition, const char* what) {
    if (condition) return;
    std::printf("FAIL: %s\n", what);
    ++g_failures;
}

void CheckClose(float actual, float expected, const char* what) {
    if (std::fabs(actual - expected) < 0.01f) return;
    std::printf("FAIL: %s (got %.2f, expected %.2f)\n", what, actual, expected);
    ++g_failures;
}

constexpr float kMainWidth = 1600.0f;
constexpr float kMainHeight = 1000.0f;

report_layout::Rect WorkArea(float width = kMainWidth, float height = kMainHeight) {
    return report_layout::Rect{0.0f, 0.0f, width, height};
}

} // namespace

int main() {
    const report_layout::Rect work = WorkArea();

    // The default must be the 1180x780 the card asks for, not the old 660x380.
    CheckClose(report_layout::kDefaultWidth, 1180.0f, "default width is 1180");
    CheckClose(report_layout::kDefaultHeight, 780.0f, "default height is 780");
    {
        const report_layout::Rect sized = report_layout::ClampSize(
            report_layout::Rect{0.0f, 0.0f, report_layout::kDefaultWidth,
                                report_layout::kDefaultHeight},
            work);
        CheckClose(sized.w, 1180.0f, "default width survives the clamp");
        CheckClose(sized.h, 780.0f, "default height survives the clamp");
    }

    // A size remembered on a 4K screen must not overflow a small main window.
    {
        const report_layout::Rect sized = report_layout::ClampSize(
            report_layout::Rect{0.0f, 0.0f, 2600.0f, 1900.0f}, WorkArea(1280.0f, 800.0f));
        CheckClose(sized.w, 1280.0f - 16.0f, "oversized width fits the work area");
        CheckClose(sized.h, 800.0f - 16.0f, "oversized height fits the work area");
    }
    // ... and a nonsense value falls back to the default instead of a 0 px box.
    {
        const report_layout::Rect sized = report_layout::ClampSize(
            report_layout::Rect{0.0f, 0.0f, std::numeric_limits<float>::quiet_NaN(),
                                -1.0f},
            work);
        CheckClose(sized.w, report_layout::kDefaultWidth, "NaN width falls back");
        CheckClose(sized.h, report_layout::kDefaultHeight, "negative height falls back");
    }
    // The dialog cannot be dragged smaller than a readable list.
    {
        const report_layout::Rect sized = report_layout::ClampSize(
            report_layout::Rect{0.0f, 0.0f, 40.0f, 40.0f}, work);
        CheckClose(sized.w, report_layout::kMinimumWidth, "minimum width is enforced");
        CheckClose(sized.h, report_layout::kMinimumHeight, "minimum height is enforced");
    }

    // A dialog that is already visible must not be moved: the caption bar drag
    // owns the position, not the appear path.
    {
        const report_layout::Rect placed = report_layout::ClampPosition(
            report_layout::Rect{200.0f, 60.0f, 1180.0f, 780.0f}, work);
        CheckClose(placed.x, 200.0f, "visible dialog keeps x");
        CheckClose(placed.y, 60.0f, "visible dialog keeps y");
    }
    // Dragging past the bottom edge pulls the whole dialog back inside with the
    // 8 px margin, which is what the field report hit.
    {
        const report_layout::Rect placed = report_layout::ClampPosition(
            report_layout::Rect{120.0f, 941.0f, 1180.0f, 780.0f}, work);
        CheckClose(placed.x, 120.0f, "vertically overflowing dialog keeps x");
        CheckClose(placed.y, kMainHeight - 8.0f - 780.0f, "overflowing dialog is pulled up");
        Check(placed.y + placed.h <= kMainHeight - 7.99f, "clamped dialog bottom edge is inside");
    }
    {
        const report_layout::Rect placed = report_layout::ClampPosition(
            report_layout::Rect{1500.0f, 100.0f, 1180.0f, 780.0f}, work);
        CheckClose(placed.x, kMainWidth - 8.0f - 1180.0f, "overflowing dialog is pulled left");
    }
    // A dialog dragged above/left of the work area comes back to the margin.
    {
        const report_layout::Rect placed = report_layout::ClampPosition(
            report_layout::Rect{-400.0f, -240.0f, 1180.0f, 780.0f}, work);
        CheckClose(placed.x, report_layout::kWorkAreaMargin, "negative x returns to the margin");
        CheckClose(placed.y, report_layout::kWorkAreaMargin, "negative y returns to the margin");
    }
    // A window smaller than the dialog pins it to the work-area origin rather
    // than producing a negative offset.
    {
        const report_layout::Rect placed = report_layout::ClampPosition(
            report_layout::Rect{10.0f, 10.0f, 1180.0f, 780.0f}, WorkArea(900.0f, 600.0f));
        CheckClose(placed.x, report_layout::kWorkAreaMargin, "oversized dialog pins to origin x");
        CheckClose(placed.y, report_layout::kWorkAreaMargin, "oversized dialog pins to origin y");
    }

    // The composed entry point used by the dialog: size first, then position.
    {
        const report_layout::Rect settled = report_layout::ClampToWorkArea(
            report_layout::Rect{700.0f, 900.0f, 2000.0f, 1400.0f},
            WorkArea(1280.0f, 800.0f));
        CheckClose(settled.w, 1264.0f, "composed clamp shrinks the width");
        CheckClose(settled.h, 784.0f, "composed clamp shrinks the height");
        CheckClose(settled.x, 8.0f, "composed clamp parks x");
        CheckClose(settled.y, 8.0f, "composed clamp parks y");
    }

    // One failure occupies exactly one text line.
    {
        CheckClose(report_layout::FailureRowHeight(17.0f, 12.0f), 29.0f,
                   "a failure row is one text line plus the row gap");
        CheckClose(report_layout::FailureRowHeight(0.0f, 12.0f), 12.0f,
                   "a zero line height cannot produce a negative row");
    }

    // The 660x380 default left the list a ~200 px strip. The 1180x780 default
    // must leave a list the user can read without scrolling for every row.
    // The running dialog measures its own list; this only pins the budget.
    {
        const float listHeight = report_layout::kDefaultHeight - report_layout::kFailureListChrome;
        const float oldListHeight = 380.0f - report_layout::kFailureListChrome;
        CheckClose(report_layout::kFailureListChrome, 36.0f + 12.0f + 32.0f + 42.0f,
                   "chrome budget matches the dialog metrics");
        Check(listHeight >= 600.0f, "the default dialog leaves at least 600 px for failures");
        Check(listHeight > oldListHeight * 2.0f,
              "the default dialog gives the list far more room than the old 380 px dialog");
    }

    if (g_failures != 0) {
        std::printf("%d geometry check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("similar analysis report layout: all checks passed\n");
    return 0;
}
