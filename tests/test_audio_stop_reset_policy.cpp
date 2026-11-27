#include "AudioStopResetPolicy.h"

#include <iostream>

namespace {
int g_failures = 0;

void Check(bool condition, const char* message) {
    if (condition) return;
    std::cerr << "FAIL: " << message << '\n';
    ++g_failures;
}
} // namespace

int main() {
    using namespace AudioStopReset;

    Check(ShouldDeferSeekWhileStopPending(true, 0.0f),
          "seek(0) after a scheduled fade stop must not clear the stop time");
    Check(!ShouldDeferSeekWhileStopPending(false, 0.0f),
          "a plain seek(0) without a pending stop is still honoured");
    Check(!ShouldDeferSeekWhileStopPending(true, 0.25f),
          "a user seek to another position takes over from the pending stop");

    Check(ShouldCompleteCursorReset(true, false),
          "pending stop resets the cursor once the backend is not playing");
    Check(!ShouldCompleteCursorReset(true, true),
          "the cursor stays put while the fade is still audible");
    Check(!ShouldCompleteCursorReset(false, false),
          "no reset is done when no stop is pending");

    if (g_failures != 0) {
        std::cerr << g_failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "All audio stop/reset ordering checks passed\n";
    return 0;
}

