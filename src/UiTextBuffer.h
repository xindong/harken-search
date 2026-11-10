#pragma once

// @stage7-contract
// Borrowed byte buffers; capacity includes the terminator. Truncation preserves
// the Windows edit-buffer byte policy. Callers provide a valid writable buffer.


#include <algorithm>
#include <cstring>

// Copy into a fixed UI edit buffer and always terminate it. Preserve the
// existing Windows CRT behavior; other platforms use the same truncation size.
inline void CopyUiText(char* destination, size_t capacity, const char* value) {
#ifdef _WIN32
    strncpy_s(destination, capacity, value, _TRUNCATE);
#else
    if (!destination || !capacity) return;
    if (!value) { destination[0] = 0; return; }
    const size_t size = std::min(capacity - 1, std::strlen(value));
    std::memmove(destination, value, size);
    destination[size] = 0;
#endif
}

template<size_t N>
inline void CopyUiText(char (&destination)[N], const char* value) {
    CopyUiText(destination, N, value);
}
