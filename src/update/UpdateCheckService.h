#pragma once

#include "update/UpdateManifest.h"
#include "update/UpdateCheckTransport.h"
#include "update/UpdateRuntime.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace aris::update {

// Trusted, immutable construction input. No fetch call accepts replacement
// URLs, verification policy, key material, timeouts, or body limits.
struct UpdateCheckServiceConfig {
    std::string manifestUrl;
    std::string signatureUrl;
    std::string publicKeyHex;
    ManifestPolicy policy;
    std::uint64_t manifestTimeoutMs = 0;
    std::uint64_t signatureTimeoutMs = 0;
    std::size_t manifestBodyLimit = 0;
    std::size_t signatureBodyLimit = 0;
    std::string fallbackManifestUrl;
};

enum class UpdateCheckScheduleKind {
    Startup = 0,
    Periodic = 1,
    CompletionRetry = 2,
};

struct UpdateCheckServiceCapability;

struct UpdateCheckScheduleIntent {
    std::uint64_t generation = 0;
    std::uint64_t delayMs = 0;
    UpdateCheckScheduleKind kind = UpdateCheckScheduleKind::Startup;

private:
    friend class UpdateCheckService;
    std::shared_ptr<const UpdateCheckServiceCapability> capability_;
};

enum class UpdateCheckServiceDisposition {
    NoOp,
    Scheduled,
    Joined,
    Busy,
    Ready,
    UpdateAvailable,
    NoUpdate,
    Failed,
    Cancelled,
    TerminalError,
};

struct UpdateCheckServiceResult {
    UpdateCheckServiceDisposition disposition =
        UpdateCheckServiceDisposition::NoOp;
    UpdateSnapshot snapshot;
    std::optional<UpdateCheckScheduleIntent> schedule;
    std::optional<UpdateCandidateHandoff> candidateHandoff;
    DownloadErrorKind error = DownloadErrorKind::InvalidArgument;
    ManifestError manifestError = ManifestError::None;
    UpdateCheckFetchError fetchError = UpdateCheckFetchError::None;
    std::string detail;
};

// Serialized application service only: no thread, sleep, clock, platform,
// filesystem/root/path, process, main/UI, database, audio, or web work.
// At most three caller-owned fetches: a preferred manifest probe and one
// signed pair, with the legacy pair allowed only after a preferred HTTP 404.
class UpdateCheckService {
public:
    UpdateCheckService(UpdateCheckTransportPort& transport,
                       UpdateRuntime& runtime,
                       UpdateCheckServiceConfig config);
    UpdateCheckService(const UpdateCheckService&) = delete;
    UpdateCheckService& operator=(const UpdateCheckService&) = delete;
    UpdateCheckService(UpdateCheckService&&) = delete;
    UpdateCheckService& operator=(UpdateCheckService&&) = delete;

    UpdateCheckServiceResult Start();
    UpdateCheckServiceResult CheckNow(
        const UpdateCheckCancellation& cancellation);
    UpdateCheckServiceResult ExecuteScheduled(
        const UpdateCheckScheduleIntent& intent,
        const UpdateCheckCancellation& cancellation);

private:
    enum class PendingCompletionKind {
        Candidate,
        NoUpdate,
        Failure,
        Cancelled,
    };

    struct PendingCompletion {
        PendingCompletionKind kind = PendingCompletionKind::Failure;
        std::optional<UpdateCandidate> candidate;
        DownloadErrorKind error = DownloadErrorKind::InvalidArgument;
        ManifestError manifestError = ManifestError::None;
        UpdateCheckFetchError fetchError = UpdateCheckFetchError::None;
        std::string detail;
    };

    UpdateCheckServiceResult Result(
        UpdateCheckServiceDisposition disposition,
        DownloadErrorKind error = DownloadErrorKind::InvalidArgument,
        std::string detail = {}) const;
    UpdateCheckServiceResult Schedule(
        UpdateCheckServiceDisposition disposition,
        UpdateCheckScheduleKind kind,
        std::uint64_t delayMs);
    std::optional<UpdateCheckScheduleIntent> NextSchedule(
        UpdateCheckScheduleKind kind,
        std::uint64_t delayMs);
    bool OwnsSchedule(const UpdateCheckScheduleIntent& intent) const;
    UpdateCheckServiceResult ExecuteActive(
        const UpdateCheckCancellation& cancellation);
    UpdateCheckServiceResult FinishPeriodic(UpdateCheckServiceResult result);
    UpdateCheckServiceResult FinishCompletionRetry(
        UpdateCheckServiceResult result);
    UpdateCheckServiceResult FinishTerminal(UpdateCheckServiceResult result);
    UpdateCheckServiceResult PublishCompletion(PendingCompletion completion);
    UpdateCheckServiceResult RetryPendingCompletion();
    UpdateCheckServiceResult PublishFailure(
        DownloadErrorKind error,
        std::string detail,
        ManifestError manifestError,
        UpdateCheckFetchError fetchError);
    UpdateCheckServiceResult PublishCancellation(std::string detail);

    UpdateCheckTransportPort& transport_;
    UpdateRuntime& runtime_;
    std::shared_ptr<const UpdateCheckServiceCapability> capability_;
    const UpdateCheckServiceConfig config_;
    bool started_ = false;
    std::uint64_t nextGeneration_ = 0;
    std::uint64_t activeGeneration_ = 0;
    std::optional<UpdateCheckScheduleIntent> pendingSchedule_;
    std::optional<UpdateCheckToken> activeCheckToken_;
    std::optional<PendingCompletion> pendingCompletion_;
};

}  // namespace aris::update
