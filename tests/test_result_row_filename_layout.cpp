// test_result_row_filename_layout.cpp - wrapped filename line layout
#include <cmath>
#include <cstdio>

#include "../src/ui/ResultRowFilenameLayout.h"

static int failed = 0;
static int passed = 0;

static void CHECK(bool cond, const char* desc) {
    if (cond) { ++passed; std::printf("  PASS: %s\n", desc); }
    else      { ++failed; std::printf("  FAIL: %s\n", desc); }
}

static bool near(float a, float b) {
    return std::fabs(a - b) <= 0.02f;
}

int main() {
    using harken::results::ResolveWrappedFilenameLayout;
    std::printf("=== ResultRowFilenameLayout Tests ===\n");

    // 18 px list font. A 54 px band holds three complete lines at the natural advance.
    {
        auto layout = ResolveWrappedFilenameLayout(54.0f, 18.0f);
        CHECK(layout.lines == 3, "54/18 -> three lines");
        CHECK(near(layout.lineAdvance, 18.0f), "54/18 -> natural advance");
    }
    // 52 px is 2.9 lines: tighten by 4% so the third line still fits whole.
    {
        auto layout = ResolveWrappedFilenameLayout(52.0f, 18.0f);
        CHECK(layout.lines == 3, "52/18 -> three complete lines");
        CHECK(near(layout.lineAdvance, 52.0f / 3.0f), "52/18 -> tightened advance");
        CHECK(layout.lineAdvance * layout.lines <= 52.01f, "52/18 -> lines stay inside the band");
    }
    // 50 px is 2.8 lines: still inside the 10% tightening budget.
    {
        auto layout = ResolveWrappedFilenameLayout(50.0f, 18.0f);
        CHECK(layout.lines == 3, "50/18 -> three complete lines");
        CHECK(layout.lineAdvance * layout.lines <= 50.01f, "50/18 -> lines stay inside the band");
    }
    // 48 px needs 11% tightening: not allowed, so two whole lines instead of a cut third.
    {
        auto layout = ResolveWrappedFilenameLayout(48.0f, 18.0f);
        CHECK(layout.lines == 2, "48/18 -> two complete lines, no partial third");
        CHECK(near(layout.lineAdvance, 18.0f), "48/18 -> natural advance kept");
    }
    {
        auto layout = ResolveWrappedFilenameLayout(46.0f, 18.0f);
        CHECK(layout.lines == 2, "46/18 -> two complete lines");
        CHECK(layout.lineAdvance * layout.lines <= 46.01f, "46/18 -> lines stay inside the band");
    }
    {
        auto layout = ResolveWrappedFilenameLayout(44.0f, 18.0f);
        CHECK(layout.lines == 2, "44/18 -> two complete lines");
    }
    {
        auto layout = ResolveWrappedFilenameLayout(36.0f, 18.0f);
        CHECK(layout.lines == 2, "36/18 -> two complete lines");
    }
    {
        auto layout = ResolveWrappedFilenameLayout(30.0f, 18.0f);
        CHECK(layout.lines == 1, "30/18 -> one complete line");
        CHECK(near(layout.lineAdvance, 18.0f), "30/18 -> natural advance");
    }
    {
        auto layout = ResolveWrappedFilenameLayout(100.0f, 18.0f);
        CHECK(layout.lines == 6, "100/18 -> six complete lines");
        CHECK(layout.lineAdvance * layout.lines <= 100.01f, "100/18 -> lines stay inside the band");
    }
    {
        auto layout = ResolveWrappedFilenameLayout(24.0f, 12.0f);
        CHECK(layout.lines == 2, "24/12 -> two complete lines");
        CHECK(near(layout.lineAdvance, 12.0f), "24/12 -> natural advance");
    }

    // Degenerate inputs still keep one readable line instead of dividing by zero.
    {
        auto layout = ResolveWrappedFilenameLayout(0.0f, 18.0f);
        CHECK(layout.lines == 1, "zero height -> one line");
    }
    {
        auto layout = ResolveWrappedFilenameLayout(-8.0f, 18.0f);
        CHECK(layout.lines == 1, "negative height -> one line");
    }
    {
        auto layout = ResolveWrappedFilenameLayout(54.0f, 0.0f);
        CHECK(layout.lines == 1, "zero line advance -> one line");
    }
    {
        auto layout = ResolveWrappedFilenameLayout(54.0f, -18.0f);
        CHECK(layout.lines == 1, "negative line advance -> one line");
    }

    std::printf("\n%d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
