#pragma once

#include <string>
#include <string_view>
#include <vector>

// The repository is a distribution coordinate, not an authority. Package
// signatures and the embedded public key remain the update authority. Keep
// this as the only product-side GitHub coordinate so a future repository move
// is a build configuration change rather than a client logic rewrite.
#ifndef ARIS_UPDATE_GITHUB_REPOSITORY
#define ARIS_UPDATE_GITHUB_REPOSITORY "xindong/harken-search"
#endif

#ifndef ARIS_UPDATE_GITHUB_RELEASE_DOWNLOAD_PREFIX
#define ARIS_UPDATE_GITHUB_RELEASE_DOWNLOAD_PREFIX \
    "https://github.com/" ARIS_UPDATE_GITHUB_REPOSITORY \
    "/releases/download/"
#endif

#ifndef ARIS_UPDATE_GITHUB_LATEST_DOWNLOAD_PREFIX
#define ARIS_UPDATE_GITHUB_LATEST_DOWNLOAD_PREFIX \
    "https://github.com/" ARIS_UPDATE_GITHUB_REPOSITORY \
    "/releases/latest/download/"
#endif

// Bootstrap package plan/.sig authority lives on the company cloud rather than
// GitHub.  The embedded package public key remains the update authority; this
// is only the distribution origin for the Bootstrap plan/signature pair, and
// it keeps the same versioned release path shape so the change stays a build
// configuration edit rather than a client logic rewrite.
#ifndef ARIS_UPDATE_COMPANY_RELEASE_DOWNLOAD_PREFIX
#define ARIS_UPDATE_COMPANY_RELEASE_DOWNLOAD_PREFIX \
    "https://harkensearch.xd.com/aris/releases/download/"
#endif

// Stable update checks read the signed manifest from this company-cloud base.
// The signed document still names the GitHub asset. ZIP downloads map that
// asset separately. An empty value keeps the manifest on GitHub.
#ifndef ARIS_UPDATE_MIRROR_BASE_URL
#define ARIS_UPDATE_MIRROR_BASE_URL "https://harkensearch.xd.com/aris/"
#endif

// 预发布（QA）通道：只有白名单用户在运行时打开，读取公司云的 aris/qa/preview/。
// 为空表示这个构建没有预发布通道，所有人仍然走正式通道。
#ifndef ARIS_UPDATE_PRERELEASE_BASE_URL
#define ARIS_UPDATE_PRERELEASE_BASE_URL ""
#endif

namespace aris::update {

inline bool& PrereleaseChannelFlag() {
    static bool enabled = false;
    return enabled;
}

inline void SetPrereleaseChannel(bool enabled) {
    PrereleaseChannelFlag() = enabled;
}

inline bool PrereleaseChannelAvailable() {
    return std::string_view(ARIS_UPDATE_PRERELEASE_BASE_URL).size() > 0;
}

// 当前生效的公司云基址：白名单用户读预发布前缀，其余人读正式前缀。
inline std::string_view ActiveMirrorBaseUrl() {
    if (PrereleaseChannelFlag() && PrereleaseChannelAvailable()) {
        return ARIS_UPDATE_PRERELEASE_BASE_URL;
    }
    return ARIS_UPDATE_MIRROR_BASE_URL;
}

inline std::string CompanyCloudStableManifestUrl() {
    const std::string_view mirror = ActiveMirrorBaseUrl();
    constexpr std::string_view file =
        "releases/latest/download/manifest-harken-search-win-x64-stable.json";
    if (mirror.empty()) {
        return std::string(ARIS_UPDATE_GITHUB_LATEST_DOWNLOAD_PREFIX) +
               "manifest-harken-search-win-x64-stable.json";
    }
    return std::string(mirror) + std::string(file);
}

inline bool IsSafeUpdatePathToken(std::string_view value) {
    return !value.empty() &&
           value.find('/') == std::string_view::npos &&
           value.find('\\') == std::string_view::npos &&
           value.find("..") == std::string_view::npos &&
           value.find('?') == std::string_view::npos &&
           value.find('#') == std::string_view::npos;
}

inline std::string ResolveUpdateDownloadUrl(std::string_view trustedUrl) {
    const std::string_view mirror = ActiveMirrorBaseUrl();
    if (mirror.empty()) return std::string(trustedUrl);

    constexpr std::string_view release =
        ARIS_UPDATE_GITHUB_RELEASE_DOWNLOAD_PREFIX;
    if (trustedUrl.rfind(release, 0) != 0) return std::string(trustedUrl);
    if (trustedUrl.find('?') != std::string_view::npos ||
        trustedUrl.find('#') != std::string_view::npos ||
        trustedUrl.find('\\') != std::string_view::npos ||
        trustedUrl.find("..") != std::string_view::npos) {
        return std::string(trustedUrl);
    }

    const std::string_view tail = trustedUrl.substr(release.size());
    const std::size_t separator = tail.find('/');
    if (separator == std::string_view::npos || separator == 0 ||
        separator + 1 >= tail.size()) {
        return std::string(trustedUrl);
    }
    const std::string_view version = tail.substr(0, separator);
    const std::string_view filename = tail.substr(separator + 1);
    if (version.front() != 'v' || version.size() == 1 ||
        !IsSafeUpdatePathToken(version) || !IsSafeUpdatePathToken(filename)) {
        return std::string(trustedUrl);
    }
    return std::string(mirror) + "releases/download/" + std::string(version) +
           "/" + std::string(filename);
}

}  // namespace aris::update
