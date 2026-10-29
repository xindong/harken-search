#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace aris::update {

// @stage7-contract
// API contract: all inputs and outputs are owned values. The manager performs
// no HTTP or filesystem operations; callers execute its explicit intents.

struct DownloadRequest {
    std::string version;
    std::string relativeTarget;
    std::string url;
    std::uint64_t expectedSize = 0;
    std::string expectedSha256;
};

struct DownloadStagingPaths {
    std::string targetPath;
    std::string partPath;
    std::string metaPath;
};

enum class DownloadPathError {
    None,
    UnsafeVersion,
    UnsafeRelativeTarget,
};

struct DownloadStagingPathsResult {
    DownloadPathError error = DownloadPathError::None;
    DownloadStagingPaths paths;

    bool ok() const { return error == DownloadPathError::None; }
};

struct ResumeMetadata {
    std::string url;
    std::optional<std::uint64_t> expectedSize;
    std::string expectedSha256;
    std::uint64_t downloadedBytes = 0;
    std::string etag;
    std::string lastModified;
};

struct RemoteValidators {
    std::string etag;
    std::string lastModified;
};

enum class DownloadCleanupIntentKind {
    DeletePart,
    DeleteMeta,
    RenamePartToTarget,
};

struct DownloadCleanupIntent {
    DownloadCleanupIntentKind kind = DownloadCleanupIntentKind::DeletePart;
    std::string sourcePath;
    std::string targetPath;
};

enum class ResumeMode {
    Fresh,
    Resume,
};

struct ResumeDecision {
    ResumeMode mode = ResumeMode::Fresh;
    std::uint64_t offset = 0;
    std::string ifRange;
    std::vector<DownloadCleanupIntent> intents;
};

struct DiskSpaceDecision {
    bool allowed = false;
    std::uint64_t requiredBytes = 0;
    std::uint64_t shortfallBytes = 0;
};

struct DownloadProgress {
    std::uint64_t loadedBytes = 0;
    std::uint64_t totalBytes = 0;
    double percent = 0.0;
};

enum class DownloadErrorKind {
    Network,
    Http4xx,
    Http5xx,
    Checksum,
    Disk,
    Aborted,
    InvalidArgument,
};

struct RetryPolicy {
    std::uint32_t maxAttempts = 6;
    std::uint64_t baseDelayMs = 1000;
    std::uint64_t maxDelayMs = 30000;
};

struct RetryDecision {
    bool retry = false;
    std::uint32_t nextAttempt = 0;
    std::uint64_t delayMs = 0;
};

struct DownloadFlightState {
    std::string activeKey;
    std::uint64_t generation = 0;
};

class UpdateDownloadManager;

class DownloadFlightToken {
public:
    const std::string& key() const { return key_; }
    std::uint64_t generation() const { return generation_; }

private:
    friend class UpdateDownloadManager;
    DownloadFlightToken(std::string key, std::uint64_t generation)
        : key_(std::move(key)), generation_(generation) {}

    std::string key_;
    std::uint64_t generation_ = 0;
};

enum class DownloadAdmissionKind {
    Accepted,
    Duplicate,
    Busy,
    Invalid,
};

struct DownloadAdmission {
    DownloadAdmissionKind kind = DownloadAdmissionKind::Invalid;
    DownloadFlightState state;
    std::optional<DownloadFlightToken> token;
};

struct DownloadCleanupDecision {
    bool success = false;
    std::vector<DownloadCleanupIntent> intents;
};

// @stage7-contract
// Retention policy input: first-level directory names directly under the
// updater-owned download root ("<version>/..." staging layout). Only strict
// numeric semver names are version packages; everything else stays.
struct UpdateVersionRetentionPlan {
    // Version directory names to remove, oldest first.
    std::vector<std::string> pruneVersions;
    // False when currentVersion is unusable and nothing may be pruned.
    bool ok = false;
};

class UpdateDownloadManager {
public:
    static DownloadStagingPathsResult BuildStagingPaths(
        const DownloadRequest& request);

    static ResumeDecision DecideResume(
        const DownloadRequest& request,
        const std::optional<ResumeMetadata>& metadata,
        std::uint64_t actualPartSize,
        const RemoteValidators& validators);

    static DiskSpaceDecision CheckDiskSpace(const DownloadRequest& request,
                                            const std::optional<ResumeMetadata>& metadata,
                                            std::uint64_t actualPartSize,
                                            const RemoteValidators& validators,
                                            std::uint64_t availableBytes,
                                            std::uint64_t reserveBytes);
    static DownloadProgress ClampProgress(std::uint64_t loadedBytes,
                                          std::uint64_t totalBytes);
    static RetryDecision DecideRetry(DownloadErrorKind error,
                                     std::uint32_t completedAttempts,
                                     const RetryPolicy& policy);
    static DownloadAdmission Admit(const DownloadFlightState& state,
                                   const DownloadRequest& request);
    static DownloadFlightState Release(const DownloadFlightState& state,
                                       const DownloadFlightToken& token);
    static DownloadCleanupDecision FinalizeChecksum(
        const DownloadRequest& request,
        const std::string& actualSha256);

    // Keep the running version's package, keep at most one previous package,
    // and never touch pending packages (newer than the running version) or
    // non-version names such as staging/transaction/quarantine prefixes.
    static UpdateVersionRetentionPlan PlanVersionRetention(
        const std::vector<std::string>& topLevelDirectoryNames,
        const std::string& currentVersion);

    static bool IsVersionDirectoryName(const std::string& name);
};

}  // namespace aris::update
