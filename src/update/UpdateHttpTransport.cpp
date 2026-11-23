#include "update/UpdateHttpTransport.h"
#include "update/UpdateReleaseConfig.h"

#include <algorithm>
#include <charconv>
#include <limits>
#include <string_view>

namespace aris::update {
namespace {

constexpr std::string_view kApprovedPrefix =
    ARIS_UPDATE_GITHUB_RELEASE_DOWNLOAD_PREFIX;

bool IsApprovedUrl(const std::string& url) {
    return url.rfind(kApprovedPrefix.data(), 0) == 0 &&
           url.size() > kApprovedPrefix.size() &&
           url.find('\\') == std::string::npos &&
           url.find("..") == std::string::npos;
}

bool HasExactHttpsHost(const std::string& url, std::string_view host) {
    constexpr std::string_view scheme = "https://";
    if (url.rfind(scheme.data(), 0) != 0) return false;
    const std::size_t authorityBegin = scheme.size();
    const std::size_t path = url.find('/', authorityBegin);
    const std::string_view authority(
        url.data() + authorityBegin,
        (path == std::string::npos ? url.size() : path) - authorityBegin);
    return authority == host && path != std::string::npos;
}

bool IsApprovedMirrorUrl(const std::string& url) {
    const std::string_view mirror = ActiveMirrorBaseUrl();
    if (mirror.empty() || url.rfind(mirror.data(), 0) != 0 ||
        url.size() <= mirror.size() || url.find('\\') != std::string::npos ||
        url.find("..") != std::string::npos || url.find('?') != std::string::npos ||
        url.find('#') != std::string::npos) {
        return false;
    }
    const std::string_view tail(url.data() + mirror.size(),
                                url.size() - mirror.size());
    constexpr std::string_view releasePrefix = "releases/download/";
    if (tail.rfind(releasePrefix, 0) != 0) return false;
    const std::string_view releaseTail = tail.substr(releasePrefix.size());
    const std::size_t separator = releaseTail.find('/');
    if (separator == std::string_view::npos || separator == 0 ||
        separator + 1 >= releaseTail.size()) {
        return false;
    }
    const std::string_view version = releaseTail.substr(0, separator);
    const std::string_view path = releaseTail.substr(separator + 1);
    if (version.front() != 'v' || version.size() == 1 ||
        !IsSafeUpdatePathToken(version) || path.empty()) {
        return false;
    }
    std::size_t begin = 0;
    for (;;) {
        const std::size_t end = path.find('/', begin);
        const std::string_view segment =
            path.substr(begin, end == std::string_view::npos ?
                                   path.size() - begin : end - begin);
        if (!IsSafeUpdatePathToken(segment)) return false;
        if (end == std::string_view::npos) return true;
        begin = end + 1;
        if (begin >= path.size()) return false;
    }
}

bool SameIdentity(const HttpTransferIdentity& left,
                  const HttpTransferIdentity& right) {
    return !left.flightKey.empty() && left.flightKey == right.flightKey &&
           left.generation != 0 && left.generation == right.generation &&
           left.partPath == right.partPath;
}

bool ParseUnsigned(std::string_view text, std::uint64_t& value) {
    if (text.empty()) return false;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size();
}

struct ParsedContentRange {
    std::uint64_t start = 0;
    std::uint64_t end = 0;
    std::uint64_t total = 0;
};

bool ParseContentRange(const std::string& text, ParsedContentRange& range) {
    constexpr std::string_view prefix = "bytes ";
    if (text.rfind(prefix.data(), 0) != 0) return false;
    const std::string_view value(text.data() + prefix.size(),
                                 text.size() - prefix.size());
    const auto dash = value.find('-');
    const auto slash = value.find('/');
    if (dash == std::string_view::npos || slash == std::string_view::npos ||
        dash == 0 || slash <= dash + 1 || slash + 1 >= value.size()) return false;
    return ParseUnsigned(value.substr(0, dash), range.start) &&
           ParseUnsigned(value.substr(dash + 1, slash - dash - 1), range.end) &&
           ParseUnsigned(value.substr(slash + 1), range.total) &&
           range.start <= range.end && range.end < range.total;
}

DownloadCleanupIntent Cleanup(DownloadCleanupIntentKind kind,
                              const std::string& path) {
    DownloadCleanupIntent intent;
    intent.kind = kind;
    intent.sourcePath = path;
    return intent;
}

HttpResponseDecision Reject(DownloadErrorKind error) {
    HttpResponseDecision result;
    result.error = error;
    return result;
}

}  // namespace

HttpTransferRequestPlanResult UpdateHttpTransport::BuildRequest(
    const DownloadRequest& request,
    const ResumeDecision& resume,
    const DownloadFlightState& activeFlight,
    const DownloadFlightToken& token) {
    HttpTransferRequestPlanResult result;
    const auto paths = UpdateDownloadManager::BuildStagingPaths(request);
    if (!paths.ok() || request.expectedSize == 0 ||
        request.expectedSha256.size() != 64) {
        result.error = HttpRequestPlanError::InvalidDownloadRequest;
        return result;
    }
    if (!IsApprovedUrl(request.url)) {
        result.error = HttpRequestPlanError::UnapprovedUrl;
        return result;
    }
    if (token.key().empty() || token.generation() == 0 ||
        activeFlight.activeKey != token.key() ||
        activeFlight.generation != token.generation()) {
        result.error = HttpRequestPlanError::InvalidFlight;
        return result;
    }
    const auto expectedFlight = UpdateDownloadManager::Admit(
        DownloadFlightState{}, request);
    if (!expectedFlight.token || expectedFlight.token->key() != token.key()) {
        result.error = HttpRequestPlanError::InvalidFlight;
        return result;
    }
    if ((resume.mode == ResumeMode::Fresh &&
         (resume.offset != 0 || !resume.ifRange.empty())) ||
        (resume.mode == ResumeMode::Resume &&
         (resume.offset == 0 || resume.offset >= request.expectedSize ||
          resume.ifRange.empty()))) {
        result.error = HttpRequestPlanError::InvalidResume;
        return result;
    }
    result.plan.url = request.url;
    result.plan.identity.flightKey = token.key();
    result.plan.identity.generation = token.generation();
    result.plan.identity.partPath = paths.paths.partPath;
    result.plan.expectedSize = request.expectedSize;
    result.plan.expectedSha256 = request.expectedSha256;
    result.plan.resumeOffset = resume.offset;
    result.plan.metaPath = paths.paths.metaPath;
    if (resume.mode == ResumeMode::Resume) {
        result.plan.headers.push_back({"Range", "bytes=" +
                                                   std::to_string(resume.offset) + "-"});
        result.plan.headers.push_back({"If-Range", resume.ifRange});
    }
    return result;
}

bool UpdateHttpTransport::IsApprovedRedirectUrl(const std::string& url) {
    if (url.find('\\') != std::string::npos || url.find("..") != std::string::npos) {
        return false;
    }
    return IsApprovedUrl(url) ||
           IsApprovedMirrorUrl(url) ||
           HasExactHttpsHost(url, "release-assets.githubusercontent.com") ||
           HasExactHttpsHost(url, "objects.githubusercontent.com");
}

HttpResponseDecision UpdateHttpTransport::EvaluateResponse(
    const HttpTransferRequestPlan& plan,
    const HttpResponseHead& response) {
    if (response.statusCode >= 400 && response.statusCode < 500) {
        return Reject(DownloadErrorKind::Http4xx);
    }
    if (response.statusCode >= 500 && response.statusCode < 600) {
        return Reject(DownloadErrorKind::Http5xx);
    }
    if (plan.resumeOffset != 0 && response.statusCode == 200) {
        auto result = Reject(DownloadErrorKind::Network);
        result.disposition = HttpResponseDisposition::RestartFresh;
        result.cleanupIntents.push_back(
            Cleanup(DownloadCleanupIntentKind::DeletePart, plan.identity.partPath));
        result.cleanupIntents.push_back(
            Cleanup(DownloadCleanupIntentKind::DeleteMeta, plan.metaPath));
        return result;
    }

    HttpResponseDecision result;
    result.validators.etag = response.etag;
    result.validators.lastModified = response.lastModified;
    result.resumeMetadata.identity = plan.identity;
    result.resumeMetadata.metaPath = plan.metaPath;
    result.resumeMetadata.metadata.url = plan.url;
    result.resumeMetadata.metadata.expectedSize = plan.expectedSize;
    result.resumeMetadata.metadata.expectedSha256 = plan.expectedSha256;
    result.resumeMetadata.metadata.downloadedBytes = plan.resumeOffset;
    result.resumeMetadata.metadata.etag = response.etag;
    result.resumeMetadata.metadata.lastModified = response.lastModified;
    if (plan.resumeOffset == 0) {
        if (response.statusCode != 200 ||
            (response.contentLength && *response.contentLength != plan.expectedSize)) {
            return Reject(DownloadErrorKind::InvalidArgument);
        }
        result.disposition = HttpResponseDisposition::AcceptFresh;
        result.bodyStart = 0;
        result.expectedBodyBytes = plan.expectedSize;
        return result;
    }

    if (response.statusCode != 206) {
        return Reject(DownloadErrorKind::InvalidArgument);
    }
    ParsedContentRange range;
    if (!ParseContentRange(response.contentRange, range) ||
        range.start != plan.resumeOffset || range.total != plan.expectedSize ||
        range.end + 1 != range.total) {
        return Reject(DownloadErrorKind::InvalidArgument);
    }
    const std::uint64_t bodyBytes = range.end - range.start + 1;
    if (response.contentLength && *response.contentLength != bodyBytes) {
        return Reject(DownloadErrorKind::InvalidArgument);
    }
    result.disposition = HttpResponseDisposition::AcceptAppend;
    result.bodyStart = range.start;
    result.expectedBodyBytes = bodyBytes;
    return result;
}

HttpTransferSession UpdateHttpTransport::BeginSession(
    const HttpTransferRequestPlan& plan,
    const HttpResponseDecision& response) {
    HttpTransferSession session;
    if (response.disposition != HttpResponseDisposition::AcceptFresh &&
        response.disposition != HttpResponseDisposition::AcceptAppend) return session;
    session.identity = plan.identity;
    session.expectedSize = plan.expectedSize;
    session.nextOffset = response.bodyStart;
    session.expectedSha256 = plan.expectedSha256;
    session.resumeMetadata = response.resumeMetadata;
    session.active = true;
    return session;
}

HttpChunkDecision UpdateHttpTransport::AcceptChunk(
    const HttpTransferSession& session,
    const DownloadFlightState& activeFlight,
    const HttpTransferIdentity& identity,
    const std::vector<std::uint8_t>& bytes) {
    HttpChunkDecision result;
    result.session = session;
    if (!session.active || bytes.empty() ||
        activeFlight.activeKey != identity.flightKey ||
        activeFlight.generation != identity.generation ||
        !SameIdentity(session.identity, identity) ||
        session.nextOffset > session.expectedSize ||
        bytes.size() > session.expectedSize - session.nextOffset) return result;
    result.intent.identity = identity;
    result.intent.partPath = identity.partPath;
    result.intent.offset = session.nextOffset;
    result.intent.bytes = bytes;
    result.session.nextOffset += bytes.size();
    result.session.resumeMetadata.metadata.downloadedBytes =
        result.session.nextOffset;
    result.resumeMetadata = result.session.resumeMetadata;
    result.resumeMetadata.identity = identity;
    result.accepted = true;
    result.error = DownloadErrorKind::Network;
    return result;
}

HttpEofDecision UpdateHttpTransport::Finish(
    const HttpTransferSession& session,
    const DownloadFlightState& activeFlight,
    const HttpTransferIdentity& identity) {
    HttpEofDecision result;
    if (!session.active || activeFlight.activeKey != identity.flightKey ||
        activeFlight.generation != identity.generation ||
        !SameIdentity(session.identity, identity) ||
        session.nextOffset != session.expectedSize) return result;
    result.success = true;
    result.verify.identity = identity;
    result.verify.partPath = identity.partPath;
    result.verify.expectedSize = session.expectedSize;
    result.verify.expectedSha256 = session.expectedSha256;
    return result;
}

}  // namespace aris::update
