#include "update/UpdateApplicationService.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace aris::update {

struct UpdateApplicationServiceCapability final {};

struct UpdateApplicationSchedulePayload final {
    std::shared_ptr<const UpdateApplicationServiceCapability> capability;
    std::uint64_t serial = 0;
    std::uint64_t dueAtMonotonicMs = 0;
    UpdateApplicationScheduleKind kind = UpdateApplicationScheduleKind::Check;
    std::optional<UpdateCheckScheduleIntent> check;
    std::optional<UpdateRetryIntent> retry;
    std::optional<UpdateCandidateHandoff> candidate;
};

namespace {

bool IsSafeRelativePath(const std::string& value) {
    if (value.empty() || value.front() == '/' || value.front() == '\\' ||
        value.find(':') != std::string::npos) {
        return false;
    }
    std::size_t start = 0;
    while (start <= value.size()) {
        const auto end = value.find_first_of("/\\", start);
        const auto length = (end == std::string::npos ? value.size() : end) - start;
        const auto component = value.substr(start, length);
        if (component.empty() || component == "." || component == "..") {
            return false;
        }
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return true;
}

std::string ExpandDownloadTarget(const std::string& pattern,
                                 const std::string& version) {
    constexpr std::string_view marker = "{version}";
    const auto offset = pattern.find(marker);
    if (offset == std::string::npos) return pattern;
    std::string expanded = pattern;
    expanded.replace(offset, marker.size(), version);
    if (expanded.find(marker) != std::string::npos) return {};
    return expanded;
}

}  // namespace

UpdateApplicationScheduleKind UpdateApplicationScheduleIntent::kind() const {
    return payload_ ? payload_->kind : UpdateApplicationScheduleKind::Check;
}

std::uint64_t UpdateApplicationScheduleIntent::dueAtMonotonicMs() const {
    return payload_ ? payload_->dueAtMonotonicMs : 0;
}

UpdateApplicationService::UpdateApplicationService(
    UpdateCheckTransportPort& checkTransport,
    HttpTransportPort& downloadTransport,
    UpdatePersistencePort& persistence,
    UpdateStagingCapacityPort& capacity,
    UpdateApplicationClockPort& clock,
    UpdateBootstrapAuthorityPort& bootstrap,
    UpdateApplicationServiceConfig config,
    UpdateStoreRecord initialRecord)
    : clock_(clock),
      capacity_(capacity),
      bootstrap_(bootstrap),
      config_(std::move(config)),
      capability_(std::make_shared<UpdateApplicationServiceCapability>()),
      runtime_(downloadTransport, persistence, config_.runtime,
               std::move(initialRecord)),
      checkService_(checkTransport, runtime_, config_.check) {}

UpdateApplicationResult UpdateApplicationService::Result(
    UpdateApplicationDisposition disposition,
    DownloadErrorKind error,
    std::string detail) const {
    UpdateApplicationResult result;
    result.disposition = disposition;
    result.snapshot = runtime_.snapshot();
    result.error = error;
    result.detail = std::move(detail);
    return result;
}

std::optional<UpdateApplicationScheduleIntent>
UpdateApplicationService::ArmCheck(const UpdateCheckScheduleIntent& intent) {
    if (nextScheduleSerial_ == std::numeric_limits<std::uint64_t>::max()) {
        return std::nullopt;
    }
    const auto now = clock_.NowMonotonicMs();
    if (intent.delayMs > std::numeric_limits<std::uint64_t>::max() - now) {
        return std::nullopt;
    }
    auto payload = std::make_shared<UpdateApplicationSchedulePayload>();
    payload->capability = capability_;
    payload->serial = ++nextScheduleSerial_;
    payload->dueAtMonotonicMs = now + intent.delayMs;
    payload->kind = UpdateApplicationScheduleKind::Check;
    payload->check = intent;
    pendingSchedules_.push_back(payload);
    return UpdateApplicationScheduleIntent(std::move(payload));
}

std::optional<UpdateApplicationScheduleIntent>
UpdateApplicationService::ArmRetry(const UpdateRetryIntent& intent) {
    if (nextScheduleSerial_ == std::numeric_limits<std::uint64_t>::max()) {
        return std::nullopt;
    }
    const auto now = clock_.NowMonotonicMs();
    if (intent.delayMs > std::numeric_limits<std::uint64_t>::max() - now) {
        return std::nullopt;
    }
    auto payload = std::make_shared<UpdateApplicationSchedulePayload>();
    payload->capability = capability_;
    payload->serial = ++nextScheduleSerial_;
    payload->dueAtMonotonicMs = now + intent.delayMs;
    payload->kind = UpdateApplicationScheduleKind::DownloadRetry;
    payload->retry = intent;
    pendingSchedules_.push_back(payload);
    return UpdateApplicationScheduleIntent(std::move(payload));
}

std::optional<UpdateApplicationScheduleIntent>
UpdateApplicationService::ArmCandidate(const UpdateCandidateHandoff& handoff) {
    if (nextScheduleSerial_ == std::numeric_limits<std::uint64_t>::max()) {
        return std::nullopt;
    }
    auto payload = std::make_shared<UpdateApplicationSchedulePayload>();
    payload->capability = capability_;
    payload->serial = ++nextScheduleSerial_;
    payload->dueAtMonotonicMs = clock_.NowMonotonicMs();
    payload->kind = UpdateApplicationScheduleKind::CandidateDownload;
    payload->candidate = handoff;
    pendingSchedules_.push_back(payload);
    return UpdateApplicationScheduleIntent(std::move(payload));
}

std::optional<UpdateApplicationScheduleIntent>
UpdateApplicationService::ArmResume() {
    if (nextScheduleSerial_ == std::numeric_limits<std::uint64_t>::max()) {
        return std::nullopt;
    }
    auto payload = std::make_shared<UpdateApplicationSchedulePayload>();
    payload->capability = capability_;
    payload->serial = ++nextScheduleSerial_;
    payload->dueAtMonotonicMs = clock_.NowMonotonicMs();
    payload->kind = UpdateApplicationScheduleKind::ResumeDownload;
    pendingSchedules_.push_back(payload);
    return UpdateApplicationScheduleIntent(std::move(payload));
}

bool UpdateApplicationService::OwnsSchedule(
    const UpdateApplicationScheduleIntent& intent) const {
    if (!intent.payload_ || intent.payload_->capability != capability_ ||
        intent.payload_->serial == 0) {
        return false;
    }
    return std::find(pendingSchedules_.begin(), pendingSchedules_.end(),
                     intent.payload_) != pendingSchedules_.end();
}

void UpdateApplicationService::RemoveSchedule(
    const std::shared_ptr<const UpdateApplicationSchedulePayload>& payload) {
    pendingSchedules_.erase(
        std::remove(pendingSchedules_.begin(), pendingSchedules_.end(), payload),
        pendingSchedules_.end());
}

void UpdateApplicationService::RemoveCheckSchedules() {
    pendingSchedules_.erase(
        std::remove_if(pendingSchedules_.begin(), pendingSchedules_.end(),
                       [](const auto& payload) {
                           return payload->kind ==
                                  UpdateApplicationScheduleKind::Check;
                       }),
        pendingSchedules_.end());
}

UpdateApplicationResult UpdateApplicationService::Start() {
    const auto started = checkService_.Start();
    auto result = Result(started.disposition ==
                                 UpdateCheckServiceDisposition::Scheduled
                             ? UpdateApplicationDisposition::Scheduled
                             : UpdateApplicationDisposition::NoOp,
                         started.error, started.detail);
    if (started.schedule) {
        const auto schedule = ArmCheck(*started.schedule);
        if (!schedule) {
            return Result(UpdateApplicationDisposition::TerminalError,
                          DownloadErrorKind::InvalidArgument,
                          "update application schedule exhausted");
        }
        result.schedules.push_back(*schedule);
    }
    if (runtime_.snapshot().phase == UpdatePhase::Downloading) {
        const auto resume = ArmResume();
        if (!resume) {
            return Result(UpdateApplicationDisposition::TerminalError,
                          DownloadErrorKind::InvalidArgument,
                          "update resume schedule exhausted");
        }
        result.schedules.push_back(*resume);
    }
    return result;
}

UpdateApplicationResult UpdateApplicationService::HandleDownloadResult(
    UpdateRuntimeResult downloaded) {
    UpdateApplicationDisposition disposition =
        UpdateApplicationDisposition::NoOp;
    switch (downloaded.disposition) {
    case UpdateRuntimeDisposition::Ready:
        disposition = UpdateApplicationDisposition::Ready;
        break;
    case UpdateRuntimeDisposition::RetryScheduled:
        disposition = UpdateApplicationDisposition::RetryScheduled;
        break;
    case UpdateRuntimeDisposition::Cancelled:
        disposition = UpdateApplicationDisposition::Cancelled;
        break;
    case UpdateRuntimeDisposition::TerminalError:
        disposition = UpdateApplicationDisposition::TerminalError;
        break;
    default:
        disposition = UpdateApplicationDisposition::NoOp;
        break;
    }
    auto result = Result(disposition, downloaded.error, downloaded.detail);
    if (downloaded.retry) {
        const auto schedule = ArmRetry(*downloaded.retry);
        if (!schedule) {
            return Result(UpdateApplicationDisposition::TerminalError,
                          DownloadErrorKind::InvalidArgument,
                          "update retry schedule exhausted");
        }
        result.schedules.push_back(*schedule);
    }
    if (downloaded.candidateHandoff) {
        const auto schedule = ArmCandidate(*downloaded.candidateHandoff);
        if (!schedule) {
            return Result(UpdateApplicationDisposition::TerminalError,
                          DownloadErrorKind::InvalidArgument,
                          "candidate download schedule exhausted");
        }
        result.schedules.push_back(*schedule);
    }
    return result;
}

UpdateStagingCapacityResult UpdateApplicationService::QueryCapacity() {
    try {
        return capacity_.QueryAvailableBytes();
    } catch (...) {
        return {false, 0,
                "update staging capacity query failed unexpectedly"};
    }
}

UpdateApplicationResult UpdateApplicationService::HandleCheckResult(
    UpdateCheckServiceResult checked,
    const HttpTransportCancellation& downloadCancellation) {
    UpdateApplicationResult result;
    if (checked.candidateHandoff) {
        const auto capacity = QueryCapacity();
        if (!capacity.available) {
            auto failed = Result(UpdateApplicationDisposition::RetryScheduled,
                                 DownloadErrorKind::Disk,
                                 capacity.detail.empty()
                                     ? "update staging capacity is unavailable"
                                     : capacity.detail);
            const auto retry = ArmCandidate(*checked.candidateHandoff);
            if (!retry) {
                return Result(UpdateApplicationDisposition::TerminalError,
                              DownloadErrorKind::InvalidArgument,
                              "candidate download schedule exhausted");
            }
            failed.schedules.push_back(*retry);
            if (checked.schedule) {
                const auto periodic = ArmCheck(*checked.schedule);
                if (!periodic) {
                    return Result(UpdateApplicationDisposition::TerminalError,
                                  DownloadErrorKind::InvalidArgument,
                                  "update check schedule exhausted");
                }
                failed.schedules.insert(failed.schedules.begin(), *periodic);
            }
            return failed;
        }
        result = HandleDownloadResult(runtime_.DownloadCandidate(
            *checked.candidateHandoff,
            ExpandDownloadTarget(config_.downloadRelativeTarget,
                                 checked.candidateHandoff->version()),
            capacity.bytes, downloadCancellation));
    } else {
        UpdateApplicationDisposition disposition =
            UpdateApplicationDisposition::NoOp;
        switch (checked.disposition) {
        case UpdateCheckServiceDisposition::Scheduled:
            disposition = UpdateApplicationDisposition::Scheduled;
            break;
        case UpdateCheckServiceDisposition::Joined:
            disposition = UpdateApplicationDisposition::Joined;
            break;
        case UpdateCheckServiceDisposition::Busy:
            disposition = UpdateApplicationDisposition::Busy;
            break;
        case UpdateCheckServiceDisposition::Ready:
            disposition = UpdateApplicationDisposition::Ready;
            break;
        case UpdateCheckServiceDisposition::NoUpdate:
            disposition = UpdateApplicationDisposition::NoUpdate;
            break;
        case UpdateCheckServiceDisposition::Failed:
            disposition = UpdateApplicationDisposition::Failed;
            break;
        case UpdateCheckServiceDisposition::Cancelled:
            disposition = UpdateApplicationDisposition::Cancelled;
            break;
        case UpdateCheckServiceDisposition::TerminalError:
            disposition = UpdateApplicationDisposition::TerminalError;
            break;
        case UpdateCheckServiceDisposition::UpdateAvailable:
        case UpdateCheckServiceDisposition::NoOp:
            disposition = UpdateApplicationDisposition::NoOp;
            break;
        }
        result = Result(disposition, checked.error, checked.detail);
        result.manifestError = checked.manifestError;
        result.fetchError = checked.fetchError;
        result.checkFailed = checked.disposition == UpdateCheckServiceDisposition::Failed ||
                             checked.disposition == UpdateCheckServiceDisposition::TerminalError;
    }
    if (checked.schedule) {
        const auto schedule = ArmCheck(*checked.schedule);
        if (!schedule) {
            return Result(UpdateApplicationDisposition::TerminalError,
                          DownloadErrorKind::InvalidArgument,
                          "update check schedule exhausted");
        }
        result.schedules.insert(result.schedules.begin(), *schedule);
    }
    result.snapshot = runtime_.snapshot();
    return result;
}

UpdateApplicationResult UpdateApplicationService::CheckNow(
    const UpdateCheckCancellation& checkCancellation,
    const HttpTransportCancellation& downloadCancellation) {
    RemoveCheckSchedules();
    return HandleCheckResult(checkService_.CheckNow(checkCancellation),
                             downloadCancellation);
}

UpdateApplicationResult UpdateApplicationService::ExecuteScheduled(
    const UpdateApplicationScheduleIntent& intent,
    const UpdateCheckCancellation& checkCancellation,
    const HttpTransportCancellation& downloadCancellation) {
    if (!OwnsSchedule(intent)) {
        return Result(UpdateApplicationDisposition::NoOp);
    }
    const auto payload = intent.payload_;
    if (activeSchedule_) {
        return Result(activeSchedule_ == payload
                          ? UpdateApplicationDisposition::Joined
                          : UpdateApplicationDisposition::Busy);
    }
    activeSchedule_ = payload;
    const auto finish = [this, &payload](UpdateApplicationResult result) {
        RemoveSchedule(payload);
        activeSchedule_.reset();
        return result;
    };
    if (payload->kind == UpdateApplicationScheduleKind::Check &&
        payload->check) {
        return finish(HandleCheckResult(
            checkService_.ExecuteScheduled(*payload->check, checkCancellation),
            downloadCancellation));
    }
    if (payload->kind == UpdateApplicationScheduleKind::DownloadRetry &&
        payload->retry) {
        const auto capacity = QueryCapacity();
        if (!capacity.available) {
            auto result = Result(UpdateApplicationDisposition::RetryScheduled,
                                 DownloadErrorKind::Disk,
                                 capacity.detail.empty()
                                     ? "update staging capacity is unavailable"
                                     : capacity.detail);
            const auto retry = ArmRetry(*payload->retry);
            if (!retry) {
                return finish(Result(
                    UpdateApplicationDisposition::TerminalError,
                    DownloadErrorKind::InvalidArgument,
                    "update retry schedule exhausted"));
            }
            result.schedules.push_back(*retry);
            return finish(std::move(result));
        }
        return finish(HandleDownloadResult(runtime_.RunScheduledRetry(
            *payload->retry, capacity.bytes,
            downloadCancellation)));
    }
    if (payload->kind == UpdateApplicationScheduleKind::CandidateDownload &&
        payload->candidate) {
        const auto capacity = QueryCapacity();
        if (!capacity.available) {
            auto result = Result(UpdateApplicationDisposition::RetryScheduled,
                                 DownloadErrorKind::Disk,
                                 capacity.detail.empty()
                                     ? "update staging capacity is unavailable"
                                     : capacity.detail);
            const auto retry = ArmCandidate(*payload->candidate);
            if (!retry) {
                return finish(Result(
                    UpdateApplicationDisposition::TerminalError,
                    DownloadErrorKind::InvalidArgument,
                    "candidate download schedule exhausted"));
            }
            result.schedules.push_back(*retry);
            return finish(std::move(result));
        }
        return finish(HandleDownloadResult(runtime_.DownloadCandidate(
            *payload->candidate,
            ExpandDownloadTarget(config_.downloadRelativeTarget,
                                 payload->candidate->version()),
            capacity.bytes, downloadCancellation)));
    }
    if (payload->kind == UpdateApplicationScheduleKind::ResumeDownload) {
        const auto capacity = QueryCapacity();
        if (!capacity.available) {
            auto result = Result(UpdateApplicationDisposition::RetryScheduled,
                                 DownloadErrorKind::Disk,
                                 capacity.detail.empty()
                                     ? "update staging capacity is unavailable"
                                     : capacity.detail);
            const auto retry = ArmResume();
            if (!retry) {
                return finish(Result(
                    UpdateApplicationDisposition::TerminalError,
                    DownloadErrorKind::InvalidArgument,
                    "update resume schedule exhausted"));
            }
            result.schedules.push_back(*retry);
            return finish(std::move(result));
        }
        const std::string resumedTarget =
            runtime_.record().snapshot.candidate
                ? ExpandDownloadTarget(config_.downloadRelativeTarget,
                                       runtime_.record().snapshot.candidate->version)
                : std::string{};
        return finish(HandleDownloadResult(runtime_.ResumeInterrupted(
            resumedTarget,
            capacity.bytes, downloadCancellation)));
    }
    return finish(Result(UpdateApplicationDisposition::NoOp));
}

std::optional<VerifiedUpdateArchive>
UpdateApplicationService::BuildBootstrapRequest() const {
    const auto& record = runtime_.record();
    if (!record.snapshot.readyPatch || record.stagedRelativePath.empty() ||
        !IsSafeRelativePath(record.stagedRelativePath)) {
        return std::nullopt;
    }
    // The persisted Ready record is the authority for the package channel.
    // A mismatch with either immutable runtime policy is fail-closed: never
    // rebind a previously verified archive to the currently selected channel.
    if (record.channel != config_.runtime.channel ||
        record.channel != config_.check.policy.selectedChannel) {
        return std::nullopt;
    }
    const auto& patch = *record.snapshot.readyPatch;
    VerifiedUpdateArchive archive;
    archive.channel = record.channel;
    archive.version = patch.version;
    archive.buildId = patch.buildId;
    archive.assetSha256 = patch.sha256;
    archive.assetSize = patch.assetSize;
    archive.stagedRelativePath = record.stagedRelativePath;
    return archive;
}

UpdateApplicationResult UpdateApplicationService::SubmitBootstrap() {
    if (!pendingBootstrap_) {
        return Result(UpdateApplicationDisposition::NoOp);
    }
    UpdateBootstrapSubmitResult submitted;
    try {
        submitted = bootstrap_.Submit(pendingBootstrap_->archive,
                                      pendingBootstrap_->capability);
    } catch (...) {
        submitted.status = UpdateBootstrapSubmitStatus::RetryableFailure;
        submitted.detail = "Bootstrap handoff failed unexpectedly";
    }
    if (submitted.status == UpdateBootstrapSubmitStatus::Accepted ||
        submitted.status == UpdateBootstrapSubmitStatus::AlreadyAccepted) {
        auto result = Result(UpdateApplicationDisposition::BootstrapAccepted);
        result.bootstrapArchive = pendingBootstrap_->archive;
        pendingBootstrap_.reset();
        bootstrapConsumed_ = true;
        return result;
    }
    auto result = Result(
        submitted.status == UpdateBootstrapSubmitStatus::RetryableFailure
            ? UpdateApplicationDisposition::BootstrapRetryable
            : UpdateApplicationDisposition::TerminalError,
        DownloadErrorKind::InvalidArgument, submitted.detail);
    result.bootstrapArchive = pendingBootstrap_->archive;
    if (submitted.status == UpdateBootstrapSubmitStatus::TerminalFailure) {
        pendingBootstrap_.reset();
    }
    return result;
}

UpdateApplicationResult UpdateApplicationService::ConfirmRestart() {
    if (bootstrapConsumed_) {
        return Result(UpdateApplicationDisposition::NoOp);
    }
    if (pendingBootstrap_) {
        if (!pendingBootstrap_->restartPersisted) {
            const auto authorized = runtime_.ConfirmRestart();
            if (authorized.disposition !=
                UpdateRuntimeDisposition::RestartAuthorized) {
                return Result(UpdateApplicationDisposition::TerminalError,
                              authorized.error, authorized.detail);
            }
            pendingBootstrap_->restartPersisted = true;
        }
        return SubmitBootstrap();
    }
    const auto prepared = BuildBootstrapRequest();
    if (!prepared) {
        return Result(UpdateApplicationDisposition::TerminalError,
                      DownloadErrorKind::InvalidArgument,
                      "verified update archive identity is invalid");
    }
    UpdateBootstrapAuthorizationResult authorization;
    try {
        authorization = bootstrap_.Authorize(*prepared);
    } catch (...) {
        authorization.status =
            UpdateBootstrapAuthorizationStatus::RetryableFailure;
        authorization.detail =
            "Bootstrap authorization failed unexpectedly";
    }
    if (authorization.status !=
            UpdateBootstrapAuthorizationStatus::Authorized ||
        !authorization.capability) {
        return Result(
            authorization.status ==
                    UpdateBootstrapAuthorizationStatus::RetryableFailure
                ? UpdateApplicationDisposition::BootstrapRetryable
                : UpdateApplicationDisposition::TerminalError,
            DownloadErrorKind::InvalidArgument, authorization.detail);
    }
    const bool alreadyPersisted = runtime_.snapshot().restartConfirmed;
    pendingBootstrap_.emplace(
        PendingBootstrap{*prepared, *authorization.capability,
                         alreadyPersisted});
    return ConfirmRestart();
}

}  // namespace aris::update
