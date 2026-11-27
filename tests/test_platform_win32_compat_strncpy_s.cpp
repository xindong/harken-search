// Contract for the strncpy_s shim in src/platform/win32_compat.h.
//
// The shared UI code fills search and metadata edit buffers with _TRUNCATE, so
// the printf on macOS must behave exactly like the MSVC secure CRT: truncate to
// what fits, keep the terminator, and report STRUNCATE. An explicit count that
// overflows the destination is an error and must not touch a partial copy.
// The platform header is a no-op on Windows, so this test also checks the real
// CRT there and keeps the two spellings honest against each other.

#include "platform/win32_compat.h"

#include <cerrno>
#include <cstring>
#include <iostream>

// The shim defines STRUNCATE itself; the Windows build gets it from the CRT
// headers, which the platform header does not pull in there.
#ifndef STRUNCATE
#define STRUNCATE 80
#endif

namespace {

int failures = 0;

void Check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        ++failures;
    }
}

} // namespace

int main() {
    // Fits exactly: no truncation, no error.
    {
        char destination[8] = "xxxxxxx";
        const int result = strncpy_s(destination, sizeof(destination), "abc", _TRUNCATE);
        Check(result == 0, "short source must succeed");
        Check(std::strcmp(destination, "abc") == 0, "short source must be copied verbatim");
    }

    // _TRUNCATE keeps the longest prefix that fits instead of emptying the
    // buffer, and reports STRUNCATE like MSVC.
    {
        char destination[4] = "xxx";
        const int result = strncpy_s(destination, sizeof(destination), "abcd", _TRUNCATE);
        Check(result == STRUNCATE, "_TRUNCATE must report STRUNCATE");
        Check(std::strcmp(destination, "abc") == 0, "_TRUNCATE must keep the prefix that fits");
    }

    // An explicit count that overflows the destination is an error: the buffer
    // is emptied and ERANGE returned.
    {
        char destination[4] = "xxx";
        const int result = strncpy_s(destination, sizeof(destination), "abcd", 4);
        Check(result == ERANGE, "explicit count overflow must report ERANGE");
        Check(destination[0] == '\0', "explicit count overflow must empty the destination");
    }

    // A source shorter than the explicit count is copied whole.
    {
        char destination[8] = "xxxxxxx";
        const int result = strncpy_s(destination, sizeof(destination), "ab", 4);
        Check(result == 0, "explicit count above the source length must succeed");
        Check(std::strcmp(destination, "ab") == 0, "explicit count must stop at the terminator");
    }

    // A source without a terminator must not be read past the caller's count.
    {
        char source[4] = {'a', 'b', 'c', 'd'};
        char destination[8];
        std::memset(destination, 'x', sizeof(destination));
        const int result = strncpy_s(destination, sizeof(destination), source, sizeof(source));
        Check(result == 0, "unterminated source within count must succeed");
        Check(std::strcmp(destination, "abcd") == 0, "unterminated source must be copied up to count");
    }

    // An empty source is the boundary case: success, empty destination.
    {
        char destination[4] = "xxx";
        const int result = strncpy_s(destination, sizeof(destination), "", _TRUNCATE);
        Check(result == 0, "empty source must succeed");
        Check(destination[0] == '\0', "empty source must produce an empty string");
    }

    if (failures != 0) {
        std::cerr << failures << " strncpy_s contract check(s) failed\n";
        return 1;
    }
    return 0;
}
