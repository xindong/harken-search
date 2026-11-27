// Contract for the strncpy_s shim in src/platform/win32_compat.h.
//
// The shared UI code fills search and metadata edit buffers with _TRUNCATE, so
// the shim on macOS must behave exactly like the MSVC secure CRT: truncate to
// what fits, keep the terminator, and report STRUNCATE. An explicit count that
// overflows the destination is an error and must not touch a partial copy.
//
// The platform header is a no-op on Windows, so this test also checks the real
// CRT there and keeps the two spellings honest against each other. That means
// two Windows-only precautions:
//   * the overflowing case makes MSVC invoke the invalid parameter handler,
//     which by default kills the process, so it is installed as a no-op for
//     that call and restored afterwards;
//   * the unterminated-source case puts the source at the end of a page with an
//     unreadable page behind it, so reading past the caller's count faults
//     instead of quietly sampling adjacent memory.

#include "platform/win32_compat.h"

#include <cerrno>
#include <cstring>
#include <iostream>

#if defined(_WIN32)
#include <crtdbg.h>
#include <cstdint>
#include <malloc.h>
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

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

// Four readable bytes immediately followed by an unreadable page.
const char* g_guardedSource = nullptr;

#if defined(_WIN32)

void SilentInvalidParameter(const wchar_t*, const wchar_t*, const wchar_t*, unsigned int, uintptr_t) {}

class InvalidParameterGuard {
public:
    InvalidParameterGuard()
        : previous_(_set_invalid_parameter_handler(SilentInvalidParameter)),
          previousReportMode_(_CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE)) {}
    ~InvalidParameterGuard() {
        _set_invalid_parameter_handler(previous_);
        _CrtSetReportMode(_CRT_ASSERT, previousReportMode_);
    }

private:
    _invalid_parameter_handler previous_;
    int previousReportMode_;
};

void* g_reserved = nullptr;

bool MapGuardedSource() {
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    const size_t page = info.dwPageSize;
    void* reserved = VirtualAlloc(nullptr, page * 2, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!reserved) return false;
    DWORD previousProtection = 0;
    if (!VirtualProtect(static_cast<char*>(reserved) + page, page, PAGE_NOACCESS, &previousProtection)) {
        VirtualFree(reserved, 0, MEM_RELEASE);
        return false;
    }
    char* source = static_cast<char*>(reserved) + page - 4;
    source[0] = 'a';
    source[1] = 'b';
    source[2] = 'c';
    source[3] = 'd';
    g_reserved = reserved;
    g_guardedSource = source;
    return true;
}

void UnmapGuardedSource() {
    if (g_reserved) VirtualFree(g_reserved, 0, MEM_RELEASE);
    g_reserved = nullptr;
    g_guardedSource = nullptr;
}

#else

void* g_reserved = nullptr;
size_t g_reservedSize = 0;

bool MapGuardedSource() {
    const long page = sysconf(_SC_PAGESIZE);
    if (page <= 0) return false;
    const size_t size = static_cast<size_t>(page) * 2;
    void* reserved = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (reserved == MAP_FAILED) return false;
    if (mprotect(static_cast<char*>(reserved) + page, static_cast<size_t>(page), PROT_NONE) != 0) {
        munmap(reserved, size);
        return false;
    }
    char* source = static_cast<char*>(reserved) + page - 4;
    source[0] = 'a';
    source[1] = 'b';
    source[2] = 'c';
    source[3] = 'd';
    g_reserved = reserved;
    g_reservedSize = size;
    g_guardedSource = source;
    return true;
}

void UnmapGuardedSource() {
    if (g_reserved) munmap(g_reserved, g_reservedSize);
    g_reserved = nullptr;
    g_reservedSize = 0;
    g_guardedSource = nullptr;
}

#endif

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
#if defined(_WIN32)
        // MSVC routes this case through the invalid parameter handler.
        InvalidParameterGuard guard;
#endif
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
    // The source ends at a page boundary, so the bug this guards against (the
    // length scan reading source[count]) faults instead of passing silently;
    // a crash here is this case failing, not an infrastructure problem.
    if (!MapGuardedSource()) {
        std::cerr << "FAIL: could not map the guarded source page\n";
        ++failures;
    } else {
        char destination[8];
        std::memset(destination, 'x', sizeof(destination));
        // The count is read through volatile so the optimiser cannot fold the
        // bound into a constant and delete the very read this case guards.
        volatile std::size_t count = 4;
        const int result = strncpy_s(destination, sizeof(destination), g_guardedSource, count);
        Check(result == 0, "unterminated source within count must succeed");
        Check(std::strcmp(destination, "abcd") == 0, "unterminated source must be copied up to count");
        UnmapGuardedSource();
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
