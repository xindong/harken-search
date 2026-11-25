#pragma once

#include "update/UpdateCheckService.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace aris::update {

// @stage7-contract
// API contract: owned values and opaque schedule capabilities cross this
// serialized composition boundary. Native scheduler/process/filesystem details
// remain in caller-owned ports, and Bootstrap admission is idempotent by the
// trusted invocation identity.

class UpdateApplicationClockPort {
public:
    virtual ~UpdateApplicationClockPort() = default;
    virtual std::uint64_t NowMonotonicMs() const = 0;
};

struct UpdateStagingCapacityResult {
    bool available = false;
    std::uint64_t bytes = 0;
    std::string detail;
};

class UpdateStagingCapacityPort {
public:
    virtual ~UpdateStagingCapacityPort() = default;
    virtual UpdateStagingCapacityResult QueryAvailableBytes() = 0;
};

// Narrow updater-owned evidence. Dynamic roots, process/channel authority,
// health nonce, executable identity, package entry names, signatures, keys and
// package-plan digest never cross from caller configuration.
struct VerifiedUpdateArchive {
    UpdateChannel channel = UpdateChannel::Stable;
    std::string version;
    std::string buildId;
    std::string assetSha256;
    std::uint64_t assetSize = 0;
    std::string stagedRelativePath;
};

class UpdateBootstrapAuthorityPort;

// Opaque Bootstrap-issued authority. The concrete Bootstrap adapter owns the
// release-signed PackagePlan binding and the authenticated invocation channel.
class UpdateBootstrapLaunchCapability {
public:
    UpdateBootstrapLaunchCapability(
        const UpdateBootstrapLaunchCapability&) = default;
    UpdateBootstrapLaunchCapability& operator=(
        const UpdateBootstrapLaunchCapability&) = default;

private:
    friend class UpdateBootstrapAuthorityPort;
    explicit UpdateBootstrapLaunchCapability(
        std::shared_ptr<const void> authority)
        : authority_(std::move(authority)) {}
    std::shared_ptr<const void> authority_;
};

enum class UpdateBootstrapAuthorizationStatus {
    Authorized,
    PackageRejected,
    BindingMismatch,
    RetryableFailure,
    TerminalFailure,
};

struct UpdateBootstrapAuthorizationResult {
    UpdateBootstrapAuthorizationStatus status =
        UpdateBootstrapAuthorizationStatus::TerminalFailure;
    std::optional<UpdateBootstrapLaunchCapability> capability;
    std::string detail;
};

enum class UpdateBootstrapSubmitStatus {
    Accepted,
    AlreadyAccepted,
    RetryableFailure,
    TerminalFailure,
};

struct UpdateBootstrapSubmitResult {
    UpdateBootstrapSubmitStatus status =
        UpdateBootstrapSubmitStatus::TerminalFailure;
    std::string detail;
};

class UpdateBootstrapAuthorityPort {
public:
    virtual ~UpdateBootstrapAuthorityPort() = default;
    // Bootstrap performs bounded package entry extraction, VerifyPackagePlan,
    // fixed entry/executable policy, exact channel/version/build/hash/size
    // binding, and authenticated parent-channel acquisition before issuing a
    // capability. Package/key/signature/digest mismatch must not issue one.
    virtual UpdateBootstrapAuthorizationResult Authorize(
        const VerifiedUpdateArchive& archive) = 0;
    // Implementations consume the exact issued capability idempotently.
    virtual UpdateBootstrapSubmitResult Submit(
        const VerifiedUpdateArchive& archive,
        const UpdateBootstrapLaunchCapability& capability) = 0;

protected:
    static UpdateBootstrapLaunchCapability IssueCapability(
        std::shared_ptr<const void> authority) {
        return UpdateBootstrapLaunchCapability(std::move(authority));
    }
    static const std::shared_ptr<const void>& Authority(
        const UpdateBootstrapLaunchCapability& capability) {
        return capability.authority_;
    }
};

struct UpdateApplicationServiceConfig {
    UpdateCheckServiceConfig check;
    UpdateRuntimeConfig runtime;
    std::string downloadRelativeTarget;
};

enum class UpdateApplicationScheduleKind {
    Check = 0,
    DownloadRetry = 1,
    CandidateDownload = 2,
    ResumeDownload = 3,
};

struct UpdateApplicationServiceCapability;
struct UpdateApplicationSchedulePayload;

class UpdateApplicationScheduleIntent {
public:
    UpdateApplicationScheduleIntent(const UpdateApplicationScheduleIntent&) = default;
    UpdateApplicationScheduleIntent& operator=(const UpdateApplicationScheduleIntent&) = default;

    UpdateApplicationScheduleKind kind() const;
    std::uint64_t dueAtMonotonicMs() const;

private:
    friend class UpdateApplicationService;
    explicit UpdateApplicationScheduleIntent(
        std::shared_ptr<const UpdateApplicationSchedulePayload> payload)
        : payload_(std::move(payload)) {}

    std::shared_ptr<const UpdateApplicationSchedulePayload> payload_;
};

enum class UpdateApplicationDisposition {
    NoOp,
    Scheduled,
    Joined,
    Busy,
    Ready,
    NoUpdate,
    Failed,
    Cancelled,
    RetryScheduled,
    TerminalError,
    BootstrapAccepted,
    BootstrapRetryable,
};

struct UpdateApplicationResult {
    UpdateApplicationDisposition disposition =
        UpdateApplicationDisposition::NoOp;
    UpdateSnapshot snapshot;
    std::vector<UpdateApplicationScheduleIntent> schedules;
    std::optional<VerifiedUpdateArchive> bootstrapArchive;
    DownloadErrorKind error = DownloadErrorKind::InvalidArgument;
    ManifestError manifestError = ManifestError::None;
    UpdateCheckFetchError fetchError = UpdateCheckFetchError::None;
    std::string detail;
    bool checkFailed = false;
};

// Serialized black-box updater composition. It owns the check and download
// runtimes but no thread, sleep, filesystem root, native handle, UI, or global
// clock. External scheduling executes only opaque capability-bound intents.
class UpdateApplicationService {
public:
    UpdateApplicationService(UpdateCheckTransportPort& checkTransport,
                             HttpTransportPort& downloadTransport,
                             UpdatePersistencePort& persistence,
                             UpdateStagingCapacityPort& capacity,
                             UpdateApplicationClockPort& clock,
                             UpdateBootstrapAuthorityPort& bootstrap,
                             UpdateApplicationServiceConfig config,
                             UpdateStoreRecord initialRecord = {});
    UpdateApplicationService(const UpdateApplicationService&) = delete;
    UpdateApplicationService& operator=(const UpdateApplicationService&) = delete;

    UpdateApplicationResult Start();
    UpdateApplicationResult CheckNow(
        const UpdateCheckCancellation& checkCancellation,
        const HttpTransportCancellation& downloadCancellation);
    UpdateApplicationResult ExecuteScheduled(
        const UpdateApplicationScheduleIntent& intent,
        const UpdateCheckCancellation& checkCancellation,
        const HttpTransportCancellation& downloadCancellation);
    UpdateApplicationResult ConfirmRestart();

    UpdateSnapshot snapshot() const { return runtime_.snapshot(); }
    const UpdateStoreRecord& record() const { return runtime_.record(); }

private:
    UpdateApplicationResult Result(
        UpdateApplicationDisposition disposition,
        DownloadErrorKind error = DownloadErrorKind::InvalidArgument,
        std::string detail = {}) const;
    UpdateApplicationResult HandleCheckResult(
        UpdateCheckServiceResult checked,
        const HttpTransportCancellation& downloadCancellation);
    UpdateApplicationResult HandleDownloadResult(UpdateRuntimeResult downloaded);
    std::optional<UpdateApplicationScheduleIntent> ArmCheck(
        const UpdateCheckScheduleIntent& intent);
    std::optional<UpdateApplicationScheduleIntent> ArmRetry(
        const UpdateRetryIntent& intent);
    std::optional<UpdateApplicationScheduleIntent> ArmCandidate(
        const UpdateCandidateHandoff& handoff);
    std::optional<UpdateApplicationScheduleIntent> ArmResume();
    bool OwnsSchedule(const UpdateApplicationScheduleIntent& intent) const;
    void RemoveSchedule(
        const std::shared_ptr<const UpdateApplicationSchedulePayload>& payload);
    void RemoveCheckSchedules();
    std::optional<VerifiedUpdateArchive> BuildBootstrapRequest() const;
    UpdateApplicationResult SubmitBootstrap();
    UpdateStagingCapacityResult QueryCapacity();

    UpdateApplicationClockPort& clock_;
    UpdateStagingCapacityPort& capacity_;
    UpdateBootstrapAuthorityPort& bootstrap_;
    const UpdateApplicationServiceConfig config_;
    std::shared_ptr<const UpdateApplicationServiceCapability> capability_;
    UpdateRuntime runtime_;
    UpdateCheckService checkService_;
    std::uint64_t nextScheduleSerial_ = 0;
    std::vector<std::shared_ptr<const UpdateApplicationSchedulePayload>>
        pendingSchedules_;
    std::shared_ptr<const UpdateApplicationSchedulePayload> activeSchedule_;
    struct PendingBootstrap {
        VerifiedUpdateArchive archive;
        UpdateBootstrapLaunchCapability capability;
        bool restartPersisted = false;
    };
    std::optional<PendingBootstrap> pendingBootstrap_;
    bool bootstrapConsumed_ = false;
};

}  // namespace aris::update
