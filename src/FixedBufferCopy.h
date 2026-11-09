#pragma once

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <string>

namespace HarkenSearch {

// Match the existing Windows _TRUNCATE behavior for fixed C-string buffers.
// The source may contain a NUL, so only its first C-string segment is copied.
template <std::size_t N>
void CopyTruncatedCString(char (&destination)[N], const std::string& source) noexcept {
    static_assert(N > 0, "a C-string buffer needs space for its terminator");
#ifdef _WIN32
    strncpy_s(destination, N, source.c_str(), _TRUNCATE);
#else
    const std::size_t count = (std::min)(source.find(char{0}), N - 1);
    if (count != 0) std::memcpy(destination, source.data(), count);
    destination[count] = char{0};
#endif
}

} // namespace HarkenSearch
