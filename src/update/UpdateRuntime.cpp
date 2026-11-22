#include "update/UpdateRuntime.h"

#if defined(AURA_UPDATE_DIAGNOSTICS) && AURA_UPDATE_DIAGNOSTICS
#include "BuildConfig.h"
#include "DiagnosticsLogger.h"
#endif

#include <chrono>
#include <limits>
#include <string>
#include <utility>

namespace aris::update {

struct UpdateRuntimeCapability final {};

struct UpdateCandidateHandoffPayload final {
    UpdateCandidateHandoffPayload(
        std::shared_ptr<const UpdateRuntimeCapability> runtimeCapability,
        std::uint64_t generation,
        std::uint64_t handoffSerial,
        UpdateCandidate checkedCandidate)
        : capability(std::move(runtimeCapability)),
          checkGeneration(generation),
          serial(handoffSerial),
          candidate(std::move(checkedCandidate)) {}

    std::shared_ptr<const UpdateRuntimeCapability> capability;
    std::uint64_t checkGeneration = 0;
    std::uint64_t serial = 0;
    UpdateCandidate candidate;
};

const std::string& UpdateCandidateHandoff::version() const noexcept {
    static const std::string empty;
    return payload_ != nullptr ? payload_->candidate.version : empty;
}

namespace {

DownloadRequest MakeRequest(const UpdateCandidate& candidate,
                            const std::string& relativeTarget) {
    DownloadRequest request;
    request.version = candidate.version;
    request.relativeTarget = relativeTarget;
    request.url = candidate.assetUrl;
    request.expectedSize = candidate.assetSize;
    request.expectedSha256 = candidate.sha256;
    return request;
}

bool SameRequest(const DownloadRequest& left, const DownloadRequest& right) {
    return left.version == right.version &&
           left.relativeTarget == right.relativeTarget &&
           left.url == right.url && left.expectedSize == right.expectedSize &&
           left.expectedSha256 == right.expectedSha256;
}

bool SameCandidate(const UpdateCandidate& left,
                   const UpdateCandidate& right) {
    return left.version == right.version && left.buildId == right.buildId &&
           left.assetUrl == right.assetUrl &&
           left.assetSize == right.assetSize && left.sha256 == right.sha256;
}

bool IsActiveCheckPhase(UpdatePhase phase) {
    return phase == UpdatePhase::Checking ||
           phase == UpdatePhase::Superseding;
}

std::string ErrorDetail(DownloadErrorKind error) {
    switch (error) {
    case DownloadErrorKind::Network: return "update network failure";
    case DownloadErrorKind::Http4xx: return "update HTTP client failure";
    case DownloadErrorKind::Http5xx: return "update HTTP server failure";
    case DownloadErrorKind::Checksum: return "update checksum mismatch";
    case DownloadErrorKind::Disk: return "update persistence failure";
    case DownloadErrorKind::Aborted: return "update cancelled";
    case DownloadErrorKind::InvalidArgument: return "update protocol failure";
    }
    return "update failure";
}

#if defined(AURA_UPDATE_DIAGNOSTICS) && AURA_UPDATE_DIAGNOSTICS
const char* ErrorKindCode(DownloadErrorKind error) {
    switch (error) {
    case DownloadErrorKind::Network: return "network";
    case DownloadErrorKind::Http4xx: return "http_4xx";
    case DownloadErrorKind::Http5xx: return "http_5xx";
    case DownloadErrorKind::Checksum: return "checksum";
    case DownloadErrorKind::Disk: return "disk";
    case DownloadErrorKind::Aborted: return "cancelled";
    case DownloadErrorKind::InvalidArgument: return "protocol";
    }
    return "unknown";
}

void LogUpdateTransition(const UpdateEvent& event) {
    using Clock = std::chrono::steady_clock;
    static Clock::time_point checkStart{};
    static Clock::time_point downloadStart{};
    static Clock::time_point lastProgress{};
    const auto now = Clock::now();
    const auto since = [&](Clock::time_point from) {
        if (from == Clock::time_point{}) return 0.0;
        return std::chrono::duration<double, std::milli>(now - from).count();
    };
    auto& log = DiagnosticsLogger::instance();
    switch (event.kind()) {
    case UpdateEventKind::CheckRequested:
        checkStart = now;
        log.logPerformance("AutoUpdate", "UpdateCheckStart", 0.0);
        break;
    case UpdateEventKind::CheckSucceeded:
        log.logPerformance("AutoUpdate", "UpdateCheckSucceeded", since(checkStart));
        break;
    case UpdateEventKind::CheckCompletedNoUpdate:
        log.logPerformance("AutoUpdate", "UpdateNoUpdate", since(checkStart));
        break;
    case UpdateEventKind::CheckFailed:
        log.logPerformance("AutoUpdate", "UpdateCheckFailed", since(checkStart));
        break;
    case UpdateEventKind::DownloadCandidateAccepted:
        // The accepted candidate is the actual download entry; resumes reuse it,
        // so the stage clock must start here as well as on an explicit request.
        downloadStart = now;
        lastProgress = now;
        log.logSystem("AutoUpdate", "UpdateCandidateAccepted");
        break;
    case UpdateEventKind::DownloadRequested:
        downloadStart = now;
        lastProgress = now;
        log.logPerformance("AutoUpdate", "UpdateDownloadStart", 0.0);
        break;
    case UpdateEventKind::DownloadProgressed:
        if (since(lastProgress) >= 2000.0) {
            lastProgress = now;
            log.logMetric("AutoUpdate", "UpdateDownloadProgressMB",
                          static_cast<double>(event.downloadedBytes()) / (1024.0 * 1024.0));
        }
        break;
    case UpdateEventKind::DownloadCompleted:
        log.logPerformance("AutoUpdate", "UpdateDownloadCompleted", since(downloadStart));
        break;
    case UpdateEventKind::DownloadFailed:
        log.logPerformance("AutoUpdate", "UpdateDownloadFailed", since(downloadStart));
        break;
    case UpdateEventKind::DownloadCancelled:
        log.logPerformance("AutoUpdate", "UpdateDownloadCancelled", since(downloadStart));
        break;
    case UpdateEventKind::RestartConfirmed:
        log.logSystem("AutoUpdate", "UpdateRestartConfirmed");
        break;
    }
}
#endif

class AttemptGuard {
public:
    AttemptGuard(UpdatePersistencePort& persistence,
                 DownloadFlightState& flight,
                 const DownloadFlightToken& token,
                 const HttpTransferIdentity& identity)
        : persistence_(persistence),
          flight_(flight),
          token_(token),
          identity_(identity) {}

    ~AttemptGuard() { Close(); }

    UpdatePersistenceResult Close() {
        if (closed_) return result_;
        closed_ = true;
        try {
            result_ = persistence_.DeactivateTransfer(token_, identity_);
        } catch (...) {
            result_.error = UpdatePersistenceError::StaleGeneration;
        }
        flight_ = UpdateDownloadManager::Release(flight_, token_);
        return result_;
    }

private:
    UpdatePersistencePort& persistence_;
    DownloadFlightState& flight_;
    const DownloadFlightToken& token_;
    const HttpTransferIdentity& identity_;
    bool closed_ = false;
    UpdatePersistenceResult result_;
};

}  // namespace

class UpdateRuntime::Sink final : public HttpTransportSink {
public:
    Sink(UpdateRuntime& runtime,
         const DownloadRequest& request,
         const HttpTransferRequestPlan& plan)
        : runtime_(runtime), request_(request), plan_(plan) {}

    bool OnResponseHead(const HttpResponseHead& response) override {
        const auto decision = UpdateHttpTransport::EvaluateResponse(plan_, response);
        if (decision.disposition == HttpResponseDisposition::RestartFresh) {
            const auto cleaned = runtime_.persistence_.ExecuteDownloadIntents(
                decision.cleanupIntents);
            if (!cleaned.ok()) {
                Fail(DownloadErrorKind::Disk,
                     "failed to clean stale download before restart");
                return false;
            }
            restartFresh_ = true;
            return false;
        }
        if (decision.disposition != HttpResponseDisposition::AcceptFresh &&
            decision.disposition != HttpResponseDisposition::AcceptAppend) {
            Fail(decision.error,
                 "download response rejected: HTTP status " +
                     std::to_string(response.statusCode) +
                     ", content-length " +
                     (response.contentLength
                          ? std::to_string(*response.contentLength)
                          : std::string("<missing>")));
            return false;
        }
        session_ = UpdateHttpTransport::BeginSession(plan_, decision);
        if (!session_.active) {
            Fail(DownloadErrorKind::InvalidArgument,
                 "download session could not be established for accepted response");
            return false;
        }
        return true;
    }

    bool OnChunk(const std::vector<std::uint8_t>& bytes) override {
        const auto chunk = UpdateHttpTransport::AcceptChunk(
            session_, runtime_.flight_, plan_.identity, bytes);
        if (!chunk.accepted) {
            Fail(DownloadErrorKind::InvalidArgument,
                 "download chunk rejected at offset " +
                     std::to_string(session_.nextOffset));
            return false;
        }
        const auto persisted = runtime_.persistence_.ExecuteTransferChunk(
            chunk.intent, chunk.resumeMetadata);
        if (!persisted.ok()) {
            Fail(DownloadErrorKind::Disk,
                 "failed to persist downloaded chunk at offset " +
                     std::to_string(session_.nextOffset));
            return false;
        }
        session_ = chunk.session;
        if (!runtime_.Apply(UpdateEvent::DownloadProgressed(session_.nextOffset))) {
            Fail(DownloadErrorKind::Disk,
                 "failed to record download progress event");
            return false;
        }
        return true;
    }

    bool OnEof() override {
        const auto eof = UpdateHttpTransport::Finish(
            session_, runtime_.flight_, plan_.identity);
        if (!eof.success) {
            Fail(eof.error, "download stream failed to finalize");
            return false;
        }
        const auto observed = runtime_.persistence_.ObserveStagedPatch(
            eof.verify.partPath);
        if (observed.status != StagedIdentityStatus::Present ||
            !observed.identity.exists ||
            observed.identity.size != eof.verify.expectedSize) {
            Fail(DownloadErrorKind::Disk,
                 "staged download did not match expected size " +
                     std::to_string(eof.verify.expectedSize));
            return false;
        }
        const auto finalized = UpdateDownloadManager::FinalizeChecksum(
            request_, observed.identity.sha256);
        if (finalized.intents.empty()) {
            Fail(DownloadErrorKind::InvalidArgument,
                 "download finalize produced no persistence intents");
            return false;
        }
        const auto executed = runtime_.persistence_.ExecuteDownloadIntents(
            finalized.intents);
        if (!executed.ok()) {
            Fail(DownloadErrorKind::Disk,
                 "failed to execute download finalize intents");
            return false;
        }
        if (!finalized.success) {
            Fail(DownloadErrorKind::Checksum,
                 "downloaded asset SHA-256 did not match manifest");
            return false;
        }
        const auto paths = UpdateDownloadManager::BuildStagingPaths(request_);
        if (!paths.ok()) {
            Fail(DownloadErrorKind::InvalidArgument,
                 "download staging path could not be resolved on EOF");
            return false;
        }
        if (!runtime_.Apply(UpdateEvent::DownloadCompleted(),
                            paths.paths.targetPath)) {
            Fail(DownloadErrorKind::Disk,
                 "failed to record download completion event");
            return false;
        }
        ready_ = true;
        return true;
    }

    bool ready() const { return ready_; }
    bool restartFresh() const { return restartFresh_; }
    DownloadErrorKind error() const { return error_; }
    const std::string& errorDetail() const { return errorDetail_; }

private:
    void Fail(DownloadErrorKind kind, std::string detail) {
        error_ = kind;
        errorDetail_ = std::move(detail);
    }

    UpdateRuntime& runtime_;
    const DownloadRequest& request_;
    const HttpTransferRequestPlan& plan_;
    HttpTransferSession session_;
    DownloadErrorKind error_ = DownloadErrorKind::InvalidArgument;
    std::string errorDetail_;
    bool ready_ = false;
    bool restartFresh_ = false;
};

UpdateRuntime::UpdateRuntime(HttpTransportPort& transport,
                             UpdatePersistencePort& persistence,
                             UpdateRuntimeConfig config,
                             UpdateStoreRecord initialRecord)
    : transport_(transport),
      persistence_(persistence),
      capability_(std::make_shared<UpdateRuntimeCapability>()),
      config_(std::move(config)),
      record_(std::move(initialRecord)) {
    // A truly fresh record has no persisted lifecycle identity yet; bind it
    // to the trusted runtime channel. Once any lifecycle data exists, keep
    // the persisted channel authoritative so Bootstrap handoff validation can
    // detect configuration drift instead of silently rebinding the archive.
    const auto& snapshot = record_.snapshot;
    const bool fresh = snapshot.phase == UpdatePhase::Idle &&
                       !snapshot.candidate.has_value() &&
                       !snapshot.readyPatch.has_value() &&
                       record_.stagedRelativePath.empty();
    if (fresh) record_.channel = config_.channel;
    state_ = UpdateCoordinator::Restore(record_.snapshot).state;
    record_.snapshot = UpdateCoordinator::Snapshot(state_);
}

UpdateRuntimeResult UpdateRuntime::Result(UpdateRuntimeDisposition disposition,
                                          DownloadErrorKind error,
                                          std::string detail) const {
    UpdateRuntimeResult result;
    result.disposition = disposition;
    result.snapshot = UpdateCoordinator::Snapshot(state_);
    result.error = error;
    result.detail = std::move(detail);
    return result;
}

bool UpdateRuntime::Persist() {
    record_.snapshot = UpdateCoordinator::Snapshot(state_);
    const auto plan = UpdateStateStore::PlanAtomicWrite(record_);
    return plan.ok() && persistence_.ExecuteStoreIntents(plan.intents).ok();
}

bool UpdateRuntime::Apply(
    const UpdateEvent& event,
    const std::optional<std::string>& stagedPath) {
    const auto transition = UpdateCoordinator::Reduce(state_, event);
    if (transition.effects.empty()) return false;
    auto nextRecord = record_;
    nextRecord.snapshot = UpdateCoordinator::Snapshot(transition.state);
    if (stagedPath) nextRecord.stagedRelativePath = *stagedPath;
    const auto plan = UpdateStateStore::PlanAtomicWrite(nextRecord);
    if (!plan.ok() ||
        !persistence_.ExecuteStoreIntents(plan.intents).ok()) {
        return false;
    }
    state_ = transition.state;
    record_ = std::move(nextRecord);
#if defined(AURA_UPDATE_DIAGNOSTICS) && AURA_UPDATE_DIAGNOSTICS
    LogUpdateTransition(event);
#endif
    return true;
}

UpdateRuntimeResult UpdateRuntime::Terminal(DownloadErrorKind error,
                                            std::string detail) {
    ++record_.failureCount;
    const bool transitioned = Apply(UpdateEvent::DownloadFailed(detail));
#if defined(AURA_UPDATE_DIAGNOSTICS) && AURA_UPDATE_DIAGNOSTICS
    DiagnosticsLogger::instance().logSystem(
        "AutoUpdate", (std::string("UpdateDownloadReason reason=") + ErrorKindCode(error)).c_str());
#endif
    if (!transitioned) Persist();
    pendingRetrySerial_ = 0;
    pendingRetryCompletedAttempts_ = 0;
    pendingRetryRequest_.reset();
    return Result(UpdateRuntimeDisposition::TerminalError, error,
                  std::move(detail));
}

UpdateRuntimeResult UpdateRuntime::PersistenceFailure(std::string detail) const {
    return Result(UpdateRuntimeDisposition::TerminalError,
                  DownloadErrorKind::Disk, std::move(detail));
}

UpdateRuntimeResult UpdateRuntime::ScheduleRetry(
    const DownloadRequest& request,
    std::uint32_t completedAttempts,
    std::uint64_t delayMs) {
    ++record_.failureCount;
    state_.lastError = delayMs == 0 ? "resume reset to fresh download"
                                   : "download retry scheduled";
    if (!Persist()) {
        return Terminal(DownloadErrorKind::Disk,
                        "failed to persist download retry");
    }
    UpdateRetryIntent retry;
    retry.runtimeGeneration = runtimeGeneration_;
    retry.serial = ++nextRetrySerial_;
    retry.completedAttempts = completedAttempts;
    retry.delayMs = delayMs;
    retry.request = request;
    pendingRetrySerial_ = retry.serial;
    pendingRetryCompletedAttempts_ = retry.completedAttempts;
    pendingRetryRequest_ = request;
    auto result = Result(UpdateRuntimeDisposition::RetryScheduled);
    result.retry = std::move(retry);
    return result;
}

UpdateRuntimeResult UpdateRuntime::BeginCheck() {
    if (!UpdateCoordinator::CanBeginCheck(state_.phase)) {
        return Result(UpdateRuntimeDisposition::NoOp);
    }
    if (nextCheckGeneration_ == std::numeric_limits<std::uint64_t>::max()) {
        return Result(UpdateRuntimeDisposition::TerminalError,
                      DownloadErrorKind::InvalidArgument,
                      "update check generation exhausted");
    }
    if (!Apply(UpdateEvent::CheckRequested())) {
        return PersistenceFailure("failed to persist active update check");
    }

    activeCheckGeneration_ = ++nextCheckGeneration_;
    pendingCandidateHandoff_.reset();
    UpdateRuntimeResult result = Result(UpdateRuntimeDisposition::CheckStarted);
    result.checkToken = UpdateCheckToken(
        capability_, activeCheckGeneration_);
    return result;
}

bool UpdateRuntime::OwnsActiveCheck(const UpdateCheckToken& token) const {
    return token.capability_ && token.capability_ == capability_ &&
           token.generation_ != 0 &&
           token.generation_ == activeCheckGeneration_;
}

bool UpdateRuntime::OwnsPendingHandoff(
    const UpdateCandidateHandoff& handoff) const {
    if (!handoff.payload_ || !pendingCandidateHandoff_ ||
        !pendingCandidateHandoff_->payload_ ||
        handoff.payload_ != pendingCandidateHandoff_->payload_) {
        return false;
    }
    return handoff.payload_->capability == capability_ &&
           handoff.payload_->checkGeneration == nextCheckGeneration_ &&
           handoff.payload_->serial == nextHandoffSerial_ &&
           handoff.payload_->checkGeneration != 0 &&
           handoff.payload_->serial != 0;
}

UpdateRuntimeResult UpdateRuntime::CompleteCheckedCandidate(
    const UpdateCheckToken& token,
    const UpdateCandidate& candidate) {
    if (!OwnsActiveCheck(token) ||
        !IsActiveCheckPhase(state_.phase)) {
        return Result(UpdateRuntimeDisposition::NoOp);
    }
    if (nextHandoffSerial_ == std::numeric_limits<std::uint64_t>::max()) {
        return Result(UpdateRuntimeDisposition::TerminalError,
                      DownloadErrorKind::InvalidArgument,
                      "update candidate handoff serial exhausted");
    }
    const auto serial = nextHandoffSerial_ + 1;
    UpdateCandidateHandoff handoff(
        std::make_shared<UpdateCandidateHandoffPayload>(
            capability_, token.generation_, serial, candidate));
    if (!Apply(UpdateEvent::CheckSucceeded(candidate))) {
        return PersistenceFailure(
            "failed to persist checked update candidate");
    }
    nextHandoffSerial_ = serial;
    activeCheckGeneration_ = 0;
    pendingCandidateHandoff_ = handoff;
    UpdateRuntimeResult result = Result(
        UpdateRuntimeDisposition::UpdateAvailable);
    result.candidateHandoff = std::move(handoff);
    return result;
}

UpdateRuntimeResult UpdateRuntime::CompleteNoUpdate(
    const UpdateCheckToken& token) {
    if (!OwnsActiveCheck(token) ||
        !IsActiveCheckPhase(state_.phase)) {
        return Result(UpdateRuntimeDisposition::NoOp);
    }
    if (!Apply(UpdateEvent::CheckCompletedNoUpdate())) {
        return PersistenceFailure("failed to persist no-update result");
    }
    activeCheckGeneration_ = 0;
    pendingCandidateHandoff_.reset();
    return Result(UpdateRuntimeDisposition::NoUpdate);
}

UpdateRuntimeResult UpdateRuntime::CancelActiveCheck(
    const UpdateCheckToken& token,
    std::string detail) {
    if (!OwnsActiveCheck(token) ||
        !IsActiveCheckPhase(state_.phase)) {
        return Result(UpdateRuntimeDisposition::NoOp);
    }
    if (!Apply(UpdateEvent::CheckCompletedNoUpdate())) {
        return PersistenceFailure("failed to persist update check cancellation");
    }
    activeCheckGeneration_ = 0;
    pendingCandidateHandoff_.reset();
    return Result(UpdateRuntimeDisposition::Cancelled,
                  DownloadErrorKind::Aborted, std::move(detail));
}

UpdateRuntimeResult UpdateRuntime::FailActiveCheck(
    const UpdateCheckToken& token,
    DownloadErrorKind error,
    std::string detail) {
    if (!OwnsActiveCheck(token) ||
        !IsActiveCheckPhase(state_.phase)) {
        return Result(UpdateRuntimeDisposition::NoOp);
    }
    if (!Apply(UpdateEvent::CheckFailed(detail))) {
        return PersistenceFailure("failed to persist update check failure");
    }
#if defined(AURA_UPDATE_DIAGNOSTICS) && AURA_UPDATE_DIAGNOSTICS
    DiagnosticsLogger::instance().logSystem(
        "AutoUpdate", (std::string("UpdateCheckReason reason=") + ErrorKindCode(error)).c_str());
#endif
    activeCheckGeneration_ = 0;
    pendingCandidateHandoff_.reset();
    return Result(state_.phase == UpdatePhase::Ready
                      ? UpdateRuntimeDisposition::Ready
                      : UpdateRuntimeDisposition::TerminalError,
                  error, std::move(detail));
}

UpdateRuntimeResult UpdateRuntime::DownloadCandidate(
    const UpdateCandidateHandoff& handoff,
    const std::string& relativeTarget,
    std::uint64_t availableBytes,
    const HttpTransportCancellation& cancellation) {
    if (!OwnsPendingHandoff(handoff)) {
        return Result(UpdateRuntimeDisposition::NoOp);
    }
    const auto& candidate = handoff.payload_->candidate;
    if (state_.phase == UpdatePhase::Available) {
        if (!state_.candidate ||
            !SameCandidate(*state_.candidate, candidate)) {
            return Result(UpdateRuntimeDisposition::NoOp);
        }
    } else if (state_.phase != UpdatePhase::Ready || !state_.readyPatch) {
        return Result(UpdateRuntimeDisposition::NoOp);
    }
    if (cancellation.IsCancellationRequested()) {
        auto result = Result(UpdateRuntimeDisposition::Cancelled,
                             DownloadErrorKind::Aborted,
                             "update cancelled before download admission");
        result.candidateHandoff = handoff;
        return result;
    }
    return AdmitDownloadCandidate(candidate, relativeTarget, availableBytes,
                                  cancellation);
}

UpdateRuntimeResult UpdateRuntime::DownloadCandidate(
    const UpdateCandidate& candidate,
    const std::string& relativeTarget,
    std::uint64_t availableBytes,
    const HttpTransportCancellation& cancellation) {
    if (IsActiveCheckPhase(state_.phase)) {
        return Result(UpdateRuntimeDisposition::NoOp);
    }
    if (state_.phase == UpdatePhase::Available &&
        (!state_.candidate ||
         !SameCandidate(*state_.candidate, candidate))) {
        return Result(UpdateRuntimeDisposition::NoOp);
    }
    return AdmitDownloadCandidate(candidate, relativeTarget, availableBytes,
                                  cancellation);
}

UpdateRuntimeResult UpdateRuntime::AdmitDownloadCandidate(
    const UpdateCandidate& candidate,
    const std::string& relativeTarget,
    std::uint64_t availableBytes,
    const HttpTransportCancellation& cancellation) {
    if (cancellation.IsCancellationRequested()) {
        return Result(UpdateRuntimeDisposition::Cancelled,
                      DownloadErrorKind::Aborted, "update cancelled");
    }
    if (!Apply(UpdateEvent::DownloadCandidateAccepted(candidate))) {
        return PersistenceFailure(
            "failed to persist download admission state");
    }

    pendingCandidateHandoff_.reset();
    ++runtimeGeneration_;
    pendingRetrySerial_ = 0;
    pendingRetryCompletedAttempts_ = 0;
    pendingRetryRequest_.reset();
    return RunAttempt(MakeRequest(candidate, relativeTarget), 1,
                      availableBytes, cancellation, true);
}

UpdateRuntimeResult UpdateRuntime::ResumeInterrupted(
    const std::string& relativeTarget,
    std::uint64_t availableBytes,
    const HttpTransportCancellation& cancellation) {
    if (state_.phase != UpdatePhase::Downloading || !state_.candidate) {
        return Result(UpdateRuntimeDisposition::NoOp);
    }
    ++runtimeGeneration_;
    pendingRetrySerial_ = 0;
    pendingRetryCompletedAttempts_ = 0;
    pendingRetryRequest_.reset();
    return RunAttempt(MakeRequest(*state_.candidate, relativeTarget), 1,
                      availableBytes, cancellation);
}

UpdateRuntimeResult UpdateRuntime::RunScheduledRetry(
    const UpdateRetryIntent& intent,
    std::uint64_t availableBytes,
    const HttpTransportCancellation& cancellation) {
    if (intent.runtimeGeneration != runtimeGeneration_ || intent.serial == 0 ||
        intent.serial != pendingRetrySerial_ ||
        intent.completedAttempts != pendingRetryCompletedAttempts_ ||
        !pendingRetryRequest_ ||
        !SameRequest(intent.request, *pendingRetryRequest_) ||
        state_.phase != UpdatePhase::Downloading || !state_.candidate ||
        state_.candidate->version != intent.request.version ||
        state_.candidate->assetUrl != intent.request.url ||
        state_.candidate->sha256 != intent.request.expectedSha256) {
        return Result(UpdateRuntimeDisposition::NoOp);
    }
    pendingRetrySerial_ = 0;
    pendingRetryCompletedAttempts_ = 0;
    pendingRetryRequest_.reset();
    return RunAttempt(intent.request, intent.completedAttempts,
                      availableBytes, cancellation);
}

UpdateRuntimeResult UpdateRuntime::ConfirmRestart() {
    if (state_.phase != UpdatePhase::Ready || !state_.readyPatch ||
        state_.restartConfirmed || record_.stagedRelativePath.empty()) {
        return Result(UpdateRuntimeDisposition::NoOp);
    }
    if (!Apply(UpdateEvent::RestartConfirmed())) {
        return PersistenceFailure(
            "failed to persist update restart confirmation");
    }
    return Result(UpdateRuntimeDisposition::RestartAuthorized);
}

UpdateRuntimeResult UpdateRuntime::RunAttempt(
    const DownloadRequest& request,
    std::uint32_t completedAttempts,
    std::uint64_t availableBytes,
    const HttpTransportCancellation& cancellation,
    bool cancellationAlreadyChecked) {
    const auto cancel = [this]() {
        if (!Apply(UpdateEvent::DownloadCancelled())) {
            return Terminal(DownloadErrorKind::Disk,
                            "failed to persist download cancellation");
        }
        pendingRetrySerial_ = 0;
        pendingRetryCompletedAttempts_ = 0;
        pendingRetryRequest_.reset();
        return Result(UpdateRuntimeDisposition::Cancelled,
                      DownloadErrorKind::Aborted, "update cancelled");
    };
    if (!cancellationAlreadyChecked &&
        cancellation.IsCancellationRequested()) {
        return cancel();
    }

    const auto paths = UpdateDownloadManager::BuildStagingPaths(request);
    if (!paths.ok()) {
        return Terminal(DownloadErrorKind::InvalidArgument,
                        "invalid download staging identity");
    }
    const auto metadataObserved = persistence_.ObserveResumeMetadata(
        paths.paths.metaPath);
    std::optional<ResumeMetadata> metadata;
    if (metadataObserved.status == ResumeMetadataStatus::Present) {
        metadata = metadataObserved.metadata;
    } else if (metadataObserved.status != ResumeMetadataStatus::Missing &&
               metadataObserved.status != ResumeMetadataStatus::Malformed) {
        return Terminal(DownloadErrorKind::Disk,
                        "failed to observe resume metadata");
    }
    const auto partObserved = persistence_.ObserveStagedPatch(paths.paths.partPath);
    std::uint64_t partSize = 0;
    if (partObserved.status == StagedIdentityStatus::Present) {
        partSize = partObserved.identity.size;
    } else if (partObserved.status != StagedIdentityStatus::Missing) {
        return Terminal(DownloadErrorKind::Disk,
                        "failed to observe staged part");
    }

    const RemoteValidators validators;
    const auto resume = UpdateDownloadManager::DecideResume(
        request, metadata, partSize, validators);
    if (!resume.intents.empty()) {
        const auto reset = persistence_.ExecuteDownloadIntents(resume.intents);
        if (!reset.ok()) {
            return Terminal(DownloadErrorKind::Disk,
                            "failed to reset stale resume data");
        }
        metadata.reset();
        partSize = 0;
    }
    const auto disk = UpdateDownloadManager::CheckDiskSpace(
        request, metadata, partSize, validators, availableBytes,
        config_.reserveBytes);
    if (!disk.allowed) {
        return Terminal(DownloadErrorKind::Disk,
                        "insufficient update staging space");
    }

    const auto admission = UpdateDownloadManager::Admit(flight_, request);
    if (admission.kind != DownloadAdmissionKind::Accepted || !admission.token) {
        return Result(UpdateRuntimeDisposition::NoOp);
    }
    flight_ = admission.state;
    const DownloadFlightToken token = *admission.token;
    const auto plan = UpdateHttpTransport::BuildRequest(
        request, resume, flight_, token);
    if (!plan.ok()) {
        flight_ = UpdateDownloadManager::Release(flight_, token);
        return Terminal(DownloadErrorKind::InvalidArgument,
                        "invalid HTTP transfer plan");
    }
    AttemptGuard guard(persistence_, flight_, token, plan.plan.identity);
    UpdatePersistenceResult activated;
    try {
        activated = persistence_.ActivateTransfer(
            flight_, token, plan.plan.identity);
    } catch (...) {
        guard.Close();
        return Terminal(DownloadErrorKind::Disk,
                        "transfer lease activation failed unexpectedly");
    }
    if (!activated.ok()) {
        guard.Close();
        return Terminal(DownloadErrorKind::Disk,
                        "failed to activate transfer lease");
    }

    Sink sink(*this, request, plan.plan);
    HttpTransportExecutionResult execution;
    try {
        execution = transport_.Execute(plan.plan, cancellation, sink);
    } catch (...) {
        execution.error = HttpTransportExecutionError::Network;
    }
    const auto deactivated = guard.Close();
    if (!deactivated.ok()) {
        return Terminal(DownloadErrorKind::Disk,
                        "failed to deactivate transfer lease");
    }
    if (sink.ready() && execution.ok()) {
        pendingRetrySerial_ = 0;
        pendingRetryCompletedAttempts_ = 0;
        pendingRetryRequest_.reset();
        return Result(UpdateRuntimeDisposition::Ready);
    }
    if (execution.error == HttpTransportExecutionError::Cancelled ||
        cancellation.IsCancellationRequested()) {
        return cancel();
    }
    if (sink.restartFresh()) {
        Apply(UpdateEvent::DownloadProgressed(0));
        return ScheduleRetry(request, completedAttempts, 0);
    }

    DownloadErrorKind error = sink.error();
    std::string detail = sink.errorDetail();
    // Cover every HTTP transport execution failure so the UI never falls back
    // to the opaque ErrorDetail() bucket when the transport layer aborted
    // before any sink callback fired.
    if (execution.error != HttpTransportExecutionError::None && detail.empty()) {
        switch (execution.error) {
        case HttpTransportExecutionError::Network:
            error = DownloadErrorKind::Network;
            detail = "download network transport failed (WinInet/system error " +
                     std::to_string(execution.systemError) + ")";
            break;
        case HttpTransportExecutionError::InvalidPlan:
            error = DownloadErrorKind::InvalidArgument;
            detail = "download plan rejected by transport (unsafe plan, unapproved redirect target, or exceeded redirect budget; system error " +
                     std::to_string(execution.systemError) + ")";
            break;
        case HttpTransportExecutionError::Cancelled:
            error = DownloadErrorKind::Aborted;
            detail = "download cancelled by transport (streamed " +
                     std::to_string(execution.streamedBytes) + " bytes)";
            break;
        case HttpTransportExecutionError::SinkRejected:
            // Sink should have populated errorDetail via Fail(); if not, this
            // is a bug worth surfacing rather than silently masking.
            detail = "download sink rejected the response without recording a detail (streamed " +
                     std::to_string(execution.streamedBytes) + " bytes)";
            break;
        case HttpTransportExecutionError::None:
            break;
        }
    } else if (execution.error == HttpTransportExecutionError::Network) {
        error = DownloadErrorKind::Network;
    }
    const auto retry = UpdateDownloadManager::DecideRetry(
        error, completedAttempts, config_.retryPolicy);
    if (retry.retry) {
        return ScheduleRetry(request, retry.nextAttempt, retry.delayMs);
    }
    return Terminal(error, detail.empty() ? ErrorDetail(error) : std::move(detail));
}

}  // namespace aris::update
