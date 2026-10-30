#include "update/UpdateDownloadManager.h"

#include <algorithm>
#include <cctype>
#include <limits>
#include <string_view>
#include <string_view>

namespace aris::update {
namespace {

std::string NormalizeSeparators(std::string value) {
    std::replace(value.begin(), value.end(), '\\', '/');
    return value;
}

bool IsSafeSegment(const std::string& value) {
    if (value.empty() || value.size() > 255 || value == "." || value == "..") {
        return false;
    }
    for (const unsigned char ch : value) {
        const bool asciiAlphaNumeric =
            (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
            (ch >= '0' && ch <= '9');
        if (!asciiAlphaNumeric && ch != '-' && ch != '_' && ch != '.') {
            return false;
        }
    }
    return true;
}

bool IsSafeRelativePath(const std::string& input) {
    const std::string value = NormalizeSeparators(input);
    if (value.empty() || value.size() > 4096 || value.front() == '/' ||
        value.find(':') != std::string::npos) {
        return false;
    }

    std::size_t begin = 0;
    while (begin < value.size()) {
        const std::size_t end = value.find('/', begin);
        const std::string segment = value.substr(
            begin, end == std::string::npos ? std::string::npos : end - begin);
        if (!IsSafeSegment(segment)) return false;
        if (end == std::string::npos) return true;
        begin = end + 1;
    }
    return false;
}

bool IsStrictNumericSemVersion(std::string_view value) {
    std::size_t begin = 0;
    for (int component = 0; component < 3; ++component) {
        const std::size_t end = component == 2 ? value.size()
                                                : value.find('.', begin);
        if (end == std::string_view::npos || end == begin) return false;
        const std::string_view token = value.substr(begin, end - begin);
        if (token.size() > 1 && token.front() == '0') return false;
        std::uint64_t number = 0;
        for (char digit : token) {
            if (digit < '0' || digit > '9') return false;
            number = number * 10 + static_cast<unsigned>(digit - '0');
            if (number > std::numeric_limits<std::uint32_t>::max()) return false;
        }
        begin = end + 1;
    }
    return begin == value.size() + 1;
}

bool EndsWithCaseInsensitive(const std::string& value,
                             std::string_view suffix) {
    if (value.size() < suffix.size()) return false;
    const std::size_t offset = value.size() - suffix.size();
    for (std::size_t index = 0; index < suffix.size(); ++index) {
        const auto left = static_cast<unsigned char>(value[offset + index]);
        const auto right = static_cast<unsigned char>(suffix[index]);
        if (std::tolower(left) != std::tolower(right)) return false;
    }
    return true;
}

bool HasReservedTargetSuffix(const std::string& target) {
    return EndsWithCaseInsensitive(target, ".part") ||
           EndsWithCaseInsensitive(target, ".meta.json") ||
           EndsWithCaseInsensitive(target, ".tmp");
}

std::string JoinPath(const std::string& version,
                     const std::string& relativeTarget) {
    return version + "/" + NormalizeSeparators(relativeTarget);
}

bool ParseVersionComponents(std::string_view value, std::uint64_t (&parts)[3]) {
    if (!IsStrictNumericSemVersion(value)) return false;
    std::size_t begin = 0;
    for (int component = 0; component < 3; ++component) {
        const std::size_t end = component == 2 ? value.size()
                                               : value.find('.', begin);
        std::uint64_t number = 0;
        for (std::size_t index = begin; index < end; ++index) {
            number = number * 10 + static_cast<unsigned>(value[index] - '0');
        }
        parts[component] = number;
        begin = end + 1;
    }
    return true;
}

// Negative when left < right, zero when equal, positive when left > right.
int CompareVersions(const std::string& left, const std::string& right) {
    std::uint64_t leftParts[3]{};
    std::uint64_t rightParts[3]{};
    if (!ParseVersionComponents(left, leftParts) ||
        !ParseVersionComponents(right, rightParts)) {
        return 0;
    }
    for (int component = 0; component < 3; ++component) {
        if (leftParts[component] != rightParts[component]) {
            return leftParts[component] < rightParts[component] ? -1 : 1;
        }
    }
    return 0;
}

std::string LowerAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

bool IsSha256(const std::string& value) {
    return value.size() == 64 &&
           std::all_of(value.begin(), value.end(), [](unsigned char ch) {
               return std::isxdigit(ch) != 0;
           });
}

DownloadCleanupIntent Intent(DownloadCleanupIntentKind kind,
                             const std::string& source,
                             const std::string& target = {}) {
    DownloadCleanupIntent intent;
    intent.kind = kind;
    intent.sourcePath = source;
    intent.targetPath = target;
    return intent;
}

void AddResetIntents(ResumeDecision& decision,
                     const DownloadStagingPaths& paths) {
    decision.intents.push_back(
        Intent(DownloadCleanupIntentKind::DeletePart, paths.partPath));
    decision.intents.push_back(
        Intent(DownloadCleanupIntentKind::DeleteMeta, paths.metaPath));
}

bool ValidatorsMatch(const ResumeMetadata& metadata,
                     const RemoteValidators& validators) {
    if (!validators.etag.empty()) return metadata.etag == validators.etag;
    if (!validators.lastModified.empty()) {
        return metadata.lastModified == validators.lastModified;
    }
    return true;
}

std::optional<std::string> FlightKey(const DownloadRequest& request) {
    const auto paths = UpdateDownloadManager::BuildStagingPaths(request);
    if (!paths.ok() || request.url.empty() || request.expectedSize == 0 ||
        !IsSha256(request.expectedSha256)) {
        return std::nullopt;
    }
    return request.url + "|" + paths.paths.targetPath + "|" +
           LowerAscii(request.expectedSha256);
}

}  // namespace

DownloadStagingPathsResult UpdateDownloadManager::BuildStagingPaths(
    const DownloadRequest& request) {
    DownloadStagingPathsResult result;
    if (!IsStrictNumericSemVersion(request.version)) {
        result.error = DownloadPathError::UnsafeVersion;
        return result;
    }
    if (!IsSafeRelativePath(request.relativeTarget) ||
        HasReservedTargetSuffix(request.relativeTarget)) {
        result.error = DownloadPathError::UnsafeRelativeTarget;
        return result;
    }

    result.paths.targetPath =
        JoinPath(request.version, request.relativeTarget);
    result.paths.partPath = result.paths.targetPath + ".part";
    result.paths.metaPath = result.paths.targetPath + ".meta.json";
    if (!IsSafeRelativePath(result.paths.targetPath) ||
        !IsSafeRelativePath(result.paths.partPath) ||
        !IsSafeRelativePath(result.paths.metaPath)) {
        result.error = DownloadPathError::UnsafeRelativeTarget;
        result.paths = {};
    }
    return result;
}

ResumeDecision UpdateDownloadManager::DecideResume(
    const DownloadRequest& request,
    const std::optional<ResumeMetadata>& metadata,
    std::uint64_t actualPartSize,
    const RemoteValidators& validators) {
    ResumeDecision decision;
    const auto paths = BuildStagingPaths(request);
    if (!paths.ok()) return decision;

    if (!metadata) {
        if (actualPartSize != 0) AddResetIntents(decision, paths.paths);
        return decision;
    }

    const bool matches =
        metadata->url == request.url &&
        metadata->expectedSize.has_value() &&
        *metadata->expectedSize == request.expectedSize &&
        LowerAscii(metadata->expectedSha256) == LowerAscii(request.expectedSha256) &&
        metadata->downloadedBytes == actualPartSize && actualPartSize != 0 &&
        actualPartSize < request.expectedSize &&
        ValidatorsMatch(*metadata, validators);
    if (!matches) {
        AddResetIntents(decision, paths.paths);
        return decision;
    }

    decision.mode = ResumeMode::Resume;
    decision.offset = actualPartSize;
    if (!validators.etag.empty()) {
        decision.ifRange = metadata->etag;
    } else if (!validators.lastModified.empty()) {
        decision.ifRange = metadata->lastModified;
    } else {
        decision.ifRange = !metadata->etag.empty() ? metadata->etag
                                                   : metadata->lastModified;
    }
    return decision;
}

DiskSpaceDecision UpdateDownloadManager::CheckDiskSpace(
    const DownloadRequest& request,
    const std::optional<ResumeMetadata>& metadata,
    std::uint64_t actualPartSize,
    const RemoteValidators& validators,
    std::uint64_t availableBytes,
    std::uint64_t reserveBytes) {
    DiskSpaceDecision decision;
    const auto resume = DecideResume(request, metadata, actualPartSize, validators);
    const std::uint64_t reusableBytes =
        resume.mode == ResumeMode::Resume ? resume.offset : 0;
    const std::uint64_t remaining =
        request.expectedSize > reusableBytes ? request.expectedSize - reusableBytes : 0;
    const std::uint64_t maximum = std::numeric_limits<std::uint64_t>::max();
    decision.requiredBytes = reserveBytes > maximum - remaining
                                 ? maximum
                                 : remaining + reserveBytes;
    decision.allowed = availableBytes >= decision.requiredBytes;
    decision.shortfallBytes = decision.allowed
                                  ? 0
                                  : decision.requiredBytes - availableBytes;
    return decision;
}

DownloadProgress UpdateDownloadManager::ClampProgress(
    std::uint64_t loadedBytes,
    std::uint64_t totalBytes) {
    DownloadProgress progress;
    progress.totalBytes = totalBytes;
    progress.loadedBytes = std::min(loadedBytes, totalBytes);
    if (totalBytes != 0) {
        progress.percent = 100.0 * static_cast<double>(progress.loadedBytes) /
                           static_cast<double>(totalBytes);
    }
    return progress;
}

RetryDecision UpdateDownloadManager::DecideRetry(
    DownloadErrorKind error,
    std::uint32_t completedAttempts,
    const RetryPolicy& policy) {
    RetryDecision decision;
    const bool retryable = error == DownloadErrorKind::Network ||
                           error == DownloadErrorKind::Http5xx;
    if (!retryable || completedAttempts == 0 ||
        completedAttempts >= policy.maxAttempts) {
        return decision;
    }

    std::uint64_t delay = std::min(policy.baseDelayMs, policy.maxDelayMs);
    for (std::uint32_t index = 1; index < completedAttempts; ++index) {
        if (delay >= policy.maxDelayMs || delay > policy.maxDelayMs / 2) {
            delay = policy.maxDelayMs;
            break;
        }
        delay *= 2;
    }
    decision.retry = true;
    decision.nextAttempt = completedAttempts + 1;
    decision.delayMs = std::min(delay, policy.maxDelayMs);
    return decision;
}

DownloadAdmission UpdateDownloadManager::Admit(
    const DownloadFlightState& state,
    const DownloadRequest& request) {
    DownloadAdmission admission;
    admission.state = state;
    const auto key = FlightKey(request);
    if (!key || state.generation == std::numeric_limits<std::uint64_t>::max()) {
        return admission;
    }
    if (state.activeKey.empty()) {
        admission.kind = DownloadAdmissionKind::Accepted;
        admission.state.activeKey = *key;
        ++admission.state.generation;
        admission.token = DownloadFlightToken(
            admission.state.activeKey, admission.state.generation);
    } else if (state.activeKey == *key) {
        admission.kind = DownloadAdmissionKind::Duplicate;
    } else {
        admission.kind = DownloadAdmissionKind::Busy;
    }
    return admission;
}

DownloadFlightState UpdateDownloadManager::Release(
    const DownloadFlightState& state,
    const DownloadFlightToken& token) {
    DownloadFlightState released = state;
    if (state.activeKey == token.key() && state.generation == token.generation()) {
        released.activeKey.clear();
    }
    return released;
}

DownloadCleanupDecision UpdateDownloadManager::FinalizeChecksum(
    const DownloadRequest& request,
    const std::string& actualSha256) {
    DownloadCleanupDecision decision;
    const auto paths = BuildStagingPaths(request);
    if (!paths.ok() || !IsSha256(request.expectedSha256) ||
        !IsSha256(actualSha256)) {
        return decision;
    }
    decision.success = LowerAscii(request.expectedSha256) ==
                       LowerAscii(actualSha256);
    if (decision.success) {
        decision.intents.push_back(Intent(DownloadCleanupIntentKind::RenamePartToTarget,
                                          paths.paths.partPath,
                                          paths.paths.targetPath));
        decision.intents.push_back(
            Intent(DownloadCleanupIntentKind::DeleteMeta, paths.paths.metaPath));
    } else {
        decision.intents.push_back(
            Intent(DownloadCleanupIntentKind::DeletePart, paths.paths.partPath));
        decision.intents.push_back(
            Intent(DownloadCleanupIntentKind::DeleteMeta, paths.paths.metaPath));
    }
    return decision;
}

bool UpdateDownloadManager::IsVersionDirectoryName(const std::string& name) {
    return IsStrictNumericSemVersion(name);
}

UpdateVersionRetentionPlan UpdateDownloadManager::PlanVersionRetention(
    const std::vector<std::string>& topLevelDirectoryNames,
    const std::string& currentVersion) {
    UpdateVersionRetentionPlan plan;
    std::uint64_t currentParts[3]{};
    if (!ParseVersionComponents(currentVersion, currentParts)) {
        return plan;
    }
    plan.ok = true;

    std::vector<std::string> olderVersions;
    for (const auto& name : topLevelDirectoryNames) {
        std::uint64_t parts[3]{};
        if (!ParseVersionComponents(name, parts)) {
            continue;  // staging/transaction/quarantine/other names stay.
        }
        const int order = CompareVersions(name, currentVersion);
        if (order >= 0) {
            continue;  // the running package and any pending package stay.
        }
        olderVersions.push_back(name);
    }
    if (olderVersions.empty()) return plan;

    std::sort(olderVersions.begin(), olderVersions.end(),
              [](const std::string& left, const std::string& right) {
                  return CompareVersions(left, right) < 0;
              });
    // Keep exactly the newest older package; it is the only rollback window.
    plan.pruneVersions.assign(olderVersions.begin(),
                              olderVersions.end() - 1);
    return plan;
}

}  // namespace aris::update
