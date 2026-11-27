// Windows compatibility shims for sources that are compiled on macOS.
//
// src/main.cpp is shared by the Windows and macOS front ends, but it grew up
// against MSVC and the Win32 API. This header supplies the small, behaviour-
// preserving subset that the shared UI code needs when it is built on macOS.
//
// Rules for this file:
//   1. It is only included on non-Windows builds; the Windows path keeps the
//      real headers, so nothing here can change Windows behaviour.
//   2. Only pure shims belong here: CRT spellings and constant values. Anything
//      that needs a real implementation (windows, surfaces, fonts, dialogs) goes
//      in its own platform header with a macOS implementation behind it.
#pragma once

#ifndef _WIN32

#include <cerrno>
#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <cwchar>
#include <cstdlib>
#include <strings.h>

// MSVC's "count the whole buffer" sentinel for strncpy_s and friends.
#ifndef _TRUNCATE
#define _TRUNCATE (static_cast<std::size_t>(-1))
#endif

// MSVC CRT spellings that the shared sources still call directly.
#ifndef _strnicmp
#define _strnicmp strncasecmp
#endif
#ifndef _stricmp
#define _stricmp strcasecmp
#endif

// The shared code only uses sscanf_s for numeric fields (%d), where the secure
// variant behaves exactly like sscanf. Reject any format that would need the
// extra size arguments so a future %s call cannot silently become unsafe.
inline int sscanf_s(const char* buffer, const char* format, ...) {
    for (const char* p = format; *p; ++p) {
        if (*p != '%') continue;
        ++p;
        if (*p == '%') continue;
        while (*p && std::strchr("0123456789.hlLzjt*", *p)) ++p;
        if (*p == 's' || *p == 'S' || *p == 'c' || *p == 'C' || *p == '[') {
            return -1;
        }
        if (!*p) break;
    }
    va_list args;
    va_start(args, format);
    const int read = std::vsscanf(buffer, format, args);
    va_end(args);
    return read;
}

// The secure time functions take the output first and return an errno value.
inline int localtime_s(std::tm* out, const std::time_t* timer) {
    if (!out || !timer) return EINVAL;
    return localtime_r(timer, out) ? 0 : EINVAL;
}

inline int gmtime_s(std::tm* out, const std::time_t* timer) {
    if (!out || !timer) return EINVAL;
    return gmtime_r(timer, out) ? 0 : EINVAL;
}

// Secure CRT copies. MSVC truncates with _TRUNCATE and returns ERANGE when an
// explicit count overflows the destination, emptying the buffer in that case.
inline int strncpy_s(char* destination, std::size_t destinationSize,
                     const char* source, std::size_t count) {
    if (!destination || destinationSize == 0) return EINVAL;
    std::size_t length = 0;
    if (source) {
        while (source[length] && (count == _TRUNCATE || length < count)) ++length;
    }
    if (length >= destinationSize) {
        destination[0] = '\0';
        return count == _TRUNCATE ? 0 : ERANGE;
    }
    if (length) std::memcpy(destination, source, length);
    destination[length] = '\0';
    return 0;
}

inline int strcpy_s(char* destination, std::size_t destinationSize, const char* source) {
    const std::size_t length = source ? std::strlen(source) : 0;
    if (!destination || length + 1 > destinationSize) {
        if (destination && destinationSize) destination[0] = '\0';
        return ERANGE;
    }
    if (length) std::memcpy(destination, source, length);
    destination[length] = '\0';
    return 0;
}

// Duplicate an environment variable into a heap buffer, like the MSVC helper.
// The caller owns the result and frees it with free().
inline int _wdupenv_s(wchar_t** buffer, std::size_t* count, const wchar_t* name) {
    if (!buffer || !count || !name) return EINVAL;
    *buffer = nullptr;
    *count = 0;
    char narrow[512];
    const std::size_t narrowLength = std::wcstombs(narrow, name, sizeof(narrow) - 1);
    if (narrowLength == static_cast<std::size_t>(-1)) return EINVAL;
    narrow[narrowLength] = '\0';
    const char* value = std::getenv(narrow);
    if (!value) return 0;
    const std::size_t wideLength = std::mbstowcs(nullptr, value, 0);
    if (wideLength == static_cast<std::size_t>(-1)) return EINVAL;
    wchar_t* copy = static_cast<wchar_t*>(std::malloc((wideLength + 1) * sizeof(wchar_t)));
    if (!copy) return ENOMEM;
    std::mbstowcs(copy, value, wideLength + 1);
    *buffer = copy;
    *count = wideLength + 1;
    return 0;
}

// Message box button and icon flags. Platform::ShowMessageBox takes the same
// integer flags on every platform, so the values only have to stay distinct.
enum : int {
    MB_OK = 0x00000000,
    MB_OKCANCEL = 0x00000001,
    MB_YESNO = 0x00000004,
    MB_ICONERROR = 0x00000010,
    MB_ICONQUESTION = 0x00000020,
    MB_ICONWARNING = 0x00000030,
    MB_ICONINFORMATION = 0x00000040,
};

#endif  // !_WIN32
