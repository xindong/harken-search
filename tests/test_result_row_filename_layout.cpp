// test_result_row_filename_layout.cpp - wrapped filename line capacity
#include <cstdio>

#include "../src/ui/ResultRowFilenameLayout.h"

static int failed = 0;
static int passed = 0;

static void CHECK(bool cond, const char* desc) {
    if (cond) { ++passed; std::printf("  PASS: %s\n", desc); }
    else      { ++failed; std::printf("  FAIL: %s\n", desc); }
}

int main() {
    using harken::results::ResolveWrappedFilenameLineCount;
    std::printf("=== ResultRowFilenameLayout Tests ===\n");

    // 18 px list font. A 54 px row band holds three complete lines.
    CHECK(ResolveWrappedFilenameLineCount(54.0f, 18.0f) == 3, "54/18 -> three complete lines");
    CHECK(ResolveWrappedFilenameLineCount(52.0f, 18.0f) == 2, "52/18 -> two complete lines");
    CHECK(ResolveWrappedFilenameLineCount(50.0f, 18.0f) == 2, "50/18 -> two complete lines");
    CHECK(ResolveWrappedFilenameLineCount(48.0f, 18.0f) == 2, "48/18 -> two complete lines");
    CHECK(ResolveWrappedFilenameLineCount(46.0f, 18.0f) == 2, "46/18 -> two complete lines");
    CHECK(ResolveWrappedFilenameLineCount(36.0f, 18.0f) == 2, "36/18 -> two complete lines");
    CHECK(ResolveWrappedFilenameLineCount(30.0f, 18.0f) == 1, "30/18 -> one complete line");
    CHECK(ResolveWrappedFilenameLineCount(100.0f, 18.0f) == 5, "100/18 -> five complete lines");
    CHECK(ResolveWrappedFilenameLineCount(60.0f, 20.0f) == 3, "60/20 -> three complete lines");
    CHECK(ResolveWrappedFilenameLineCount(24.0f, 12.0f) == 2, "24/12 -> two complete lines");

    // Degenerate inputs still keep one readable line instead of dividing by zero.
    CHECK(ResolveWrappedFilenameLineCount(0.0f, 18.0f) == 1, "zero height -> one line");
    CHECK(ResolveWrappedFilenameLineCount(-8.0f, 18.0f) == 1, "negative height -> one line");
    CHECK(ResolveWrappedFilenameLineCount(54.0f, 0.0f) == 1, "zero line advance -> one line");
    CHECK(ResolveWrappedFilenameLineCount(54.0f, -18.0f) == 1, "negative line advance -> one line");

    std::printf("\n%d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
