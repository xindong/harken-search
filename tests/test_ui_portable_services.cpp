#include "UiTextBuffer.h"
#include "WindowsMain.h"
#include <cassert>
#include <cstdint>
#include <iostream>
#include <string>

static std::uint64_t clockMilliseconds = 0;
namespace Platform {
std::uint64_t MonotonicMilliseconds() { return clockMilliseconds; }
}

int main() {
    struct GuardedBuffer { char before = 'L'; char text[5] = {}; char after = 'R'; } buffer;
    CopyUiText(buffer.text, "abc");
    assert(std::string(buffer.text) == "abc");
    CopyUiText(buffer.text, "abcdefgh");
    assert(std::string(buffer.text) == "abcd");
    assert(buffer.before == 'L' && buffer.after == 'R');
    CopyUiText(buffer.text, "");
    assert(buffer.text[0] == 0);
    char one[1] = {'X'};
    CopyUiText(one, "音频");
    assert(one[0] == 0);
    char chinese[16] = {};
    CopyUiText(chinese, "音频");
    assert(std::string(chinese) == "音频");
    char exact[4] = {};
    CopyUiText(exact, sizeof(exact), "abc");
    assert(std::string(exact) == "abc");
    assert(!IsRapidUiClickBurstActive());
    clockMilliseconds = 1000;
    MarkUiLeftClickNow();
    assert(IsRapidUiClickBurstActive());
    clockMilliseconds = 1320;
    assert(IsRapidUiClickBurstActive());
    clockMilliseconds = 1321;
    assert(!IsRapidUiClickBurstActive());
    clockMilliseconds = 999;
    assert(!IsRapidUiClickBurstActive());
    SetCoverWallPriorityMode(true);
    assert(IsCoverWallPriorityMode());
    SetCoverWallPriorityMode(false);
    assert(!IsCoverWallPriorityMode());
    std::cout << "PASS: bounded UTF-8 UI copies, click time boundaries, cover priority state\n";
}
