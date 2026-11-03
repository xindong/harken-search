#pragma once

#include "platform/harken_user_data_layout.h"

#include <string>

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#endif

namespace Platform::DatabasePaths {

// @stage7-contract
// API contract: database path helpers synchronously query the current platform
// and return owned wide strings. On Windows the default-directory helper may
// create the established application/database directories; unsupported or
// failed lookups return an empty path without retained native handles.

inline std::wstring GetDefaultDbDirWide() {
#ifdef _WIN32
    const auto layout = Platform::HarkenUserData::EnsureLayout();
    if (layout.activeDatabaseDir.empty()) return {};
    if (!layout.canonicalRoot.empty()) {
        CreateDirectoryW(layout.canonicalRoot.c_str(), nullptr);
    }
    CreateDirectoryW(layout.activeDatabaseDir.c_str(), nullptr);
    return layout.activeDatabaseDir;
#elif defined(__APPLE__)
    const auto layout = HarkenUserData::EnsureLayout();
    return layout.readOnly ? std::wstring{} : layout.activeDatabaseDir;
#endif
    return {};
}

inline std::wstring GetLegacyDefaultDbPathWide() {
#ifdef _WIN32
    return Platform::HarkenUserData::LegacyAuroDbFilePath();
#endif
    return {};
}

} // namespace Platform::DatabasePaths
