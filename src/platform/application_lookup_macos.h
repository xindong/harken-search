#pragma once
#ifdef __APPLE__
#include <string>

namespace Platform {
enum class MacApplicationLookupStatus { Found, NotFound, InvalidArgument, QueryFailed };
struct MacApplicationLookupResult {
    MacApplicationLookupStatus status = MacApplicationLookupStatus::InvalidArgument;
    std::string bundlePathUtf8;
    std::string executablePathUtf8;
    long systemError = 0;
};
// Read-only LaunchServices discovery; never registers, launches, or modifies apps.
MacApplicationLookupResult FindMacApplicationByBundleIdentifier(const std::string& bundleIdentifier);
}
#endif
