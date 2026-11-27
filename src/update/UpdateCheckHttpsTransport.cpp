#include "update/UpdateCheckHttpsTransport.h"
#include "update/UpdateReleaseConfig.h"

#include <algorithm>
#include <string_view>

namespace aris::update {
namespace {

bool StartsWith(const std::string& value, const char* prefix) {
    const std::string expected(prefix);
    return value.size() >= expected.size() &&
           value.compare(0, expected.size(), expected) == 0;
}

bool IsCanonicalGitHubReleaseUrl(const std::string& url) {
    const bool matches =
        StartsWith(url, ARIS_UPDATE_GITHUB_RELEASE_DOWNLOAD_PREFIX) ||
        StartsWith(url, ARIS_UPDATE_GITHUB_LATEST_DOWNLOAD_PREFIX);
    return matches && url.find('\\') == std::string::npos &&
           url.find("..") == std::string::npos;
}

}  // namespace

UpdateCheckFetchResult ReadUpdateCheckHttpsBody(
    UpdateCheckHttpsReadPort& port,
    const UpdateCheckCancellation& cancellation,
    std::size_t requestedBodyBytes) {
    if (requestedBodyBytes == 0 ||
        requestedBodyBytes > kUpdateCheckHttpsMaximumBodyBytes) {
        return {UpdateCheckFetchError::InvalidResponse, {},
                "update check body cap exceeds fixed HTTPS maximum"};
    }
    auto interrupt = cancellation.RegisterInterrupt([&port]() {
        port.Abort();
    });
    if (cancellation.IsCancellationRequested()) {
        port.Abort();
        return {UpdateCheckFetchError::Cancelled, {},
                "update check cancelled"};
    }
    UpdateCheckFetchResult result;
    constexpr std::size_t kReadChunkBytes = 16U * 1024U;
    for (;;) {
        const auto remaining = requestedBodyBytes - result.body.size();
        const auto read = port.Read(
            remaining == 0 ? 1 : std::min(kReadChunkBytes, remaining),
            kUpdateCheckHttpsReadTimeoutMs);
        if (read.status == UpdateCheckReadStatus::End) return result;
        if (read.status == UpdateCheckReadStatus::Aborted ||
            cancellation.IsCancellationRequested()) {
            return {UpdateCheckFetchError::Cancelled, {},
                    "update check cancelled"};
        }
        if (read.status != UpdateCheckReadStatus::Chunk) {
            return {UpdateCheckFetchError::Network, {},
                    "update check response read failed"};
        }
        if (read.bytes.empty() || read.bytes.size() >
                                      requestedBodyBytes - result.body.size()) {
            return {read.bytes.empty() ? UpdateCheckFetchError::InvalidResponse
                                       : UpdateCheckFetchError::BodyTooLarge,
                    {},
                    read.bytes.empty()
                        ? "update check response returned an empty chunk"
                        : "update check response exceeds body cap"};
        }
        result.body.insert(result.body.end(), read.bytes.begin(),
                           read.bytes.end());
    }
}

// The company-cloud Bootstrap authority is a narrow origin: it may only serve
// the exact package plan/signature pair, never an arbitrary release asset.  The
// version segment must be a safe `v`-prefixed token and the basename must equal
// one of the four package plan/signature names; query, fragment, traversal and
// any extra path component are rejected here rather than at network I/O.
bool IsCompanyCloudBootstrapPackageUrl(const std::string& url) {
    constexpr std::string_view prefix =
        ARIS_UPDATE_COMPANY_RELEASE_DOWNLOAD_PREFIX;
    if (url.size() <= prefix.size() ||
        url.compare(0, prefix.size(), prefix) != 0) {
        return false;
    }
    const std::string_view tail = std::string_view(url).substr(prefix.size());
    const std::size_t separator = tail.find('/');
    if (separator == std::string_view::npos || separator == 0U) {
        return false;
    }
    const std::string_view version = tail.substr(0, separator);
    const std::string_view filename = tail.substr(separator + 1U);
    if (version.size() < 2U || version.front() != 'v' ||
        !IsSafeUpdatePathToken(version) ||
        !IsSafeUpdatePathToken(filename)) {
        return false;
    }
    return filename == "ArisBootstrap.package.stable.json" ||
           filename == "ArisBootstrap.package.stable.json.sig" ||
           filename == "ArisBootstrap.package.json" ||
           filename == "ArisBootstrap.package.json.sig";
}


bool IsCompanyCloudStableManifestUrl(const std::string& url) {
    if (url.find('\\') != std::string::npos ||
        url.find("..") != std::string::npos ||
        url.find('?') != std::string::npos ||
        url.find('#') != std::string::npos) {
        return false;
    }
    const std::string preferred(PreferredStableManifestUrl());
    if (!preferred.empty() && (url == preferred || url == preferred + ".sig")) {
        return true;
    }
    constexpr std::string_view mirror = ARIS_UPDATE_MIRROR_BASE_URL;
    if (mirror.empty() || url.rfind(mirror, 0) != 0) return false;
    const std::string manifest = CompanyCloudStableManifestUrl();
    return url == manifest || url == manifest + ".sig";
}

bool IsApprovedUpdateCheckUrl(const std::string& url, bool initialRequest) {
    if (url.find('\\') != std::string::npos ||
        url.find("..") != std::string::npos) {
        return false;
    }
    if (IsCompanyCloudStableManifestUrl(url)) return true;
    if (IsCompanyCloudBootstrapPackageUrl(url)) return true;
    if (initialRequest) {
        return IsCanonicalGitHubReleaseUrl(url);
    }
    return IsCanonicalGitHubReleaseUrl(url) ||
           StartsWith(url, "https://release-assets.githubusercontent.com/") ||
           StartsWith(url, "https://objects.githubusercontent.com/");
}

UpdateCheckFetchError ClassifyUpdateCheckHttpStatus(
    std::uint32_t statusCode) {
    if (statusCode == 200) return UpdateCheckFetchError::None;
    if (statusCode >= 400 && statusCode < 500) {
        return UpdateCheckFetchError::Http4xx;
    }
    if (statusCode >= 500 && statusCode < 600) {
        return UpdateCheckFetchError::Http5xx;
    }
    return UpdateCheckFetchError::InvalidResponse;
}

}  // namespace aris::update
