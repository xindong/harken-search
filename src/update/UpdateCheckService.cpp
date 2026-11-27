#include "update/UpdateCheckService.h"

#include <limits>
#include <string>
#include <utility>

namespace aris::update {

struct UpdateCheckServiceCapability final {};

namespace {

constexpr std::uint64_t kStartupDelayMs = 10'000;
constexpr std::uint64_t kPeriodicDelayMs = 30 * 60 * 1'000;

bool SameIntent(const UpdateCheckScheduleIntent& left,
                const UpdateCheckScheduleIntent& right) {
    return left.generation == right.generation &&
           left.delayMs == right.delayMs && left.kind == right.kind;
}

std::string OwnedText(const std::vector<std::uint8_t>& bytes) {
    if (bytes.empty()) return {};
    return std::string(reinterpret_cast<const char*>(bytes.data()),
                       bytes.size());
}

DownloadErrorKind Classification(UpdateCheckFetchError error) {
    switch (error) {
    case UpdateCheckFetchError::Network: return DownloadErrorKind::Network;
    case UpdateCheckFetchError::Http4xx: return DownloadErrorKind::Http4xx;
    case UpdateCheckFetchError::Http5xx: return DownloadErrorKind::Http5xx;
    case UpdateCheckFetchError::Cancelled: return DownloadErrorKind::Aborted;
    case UpdateCheckFetchError::BodyTooLarge:
    case UpdateCheckFetchError::InvalidResponse:
    case UpdateCheckFetchError::None:
        return DownloadErrorKind::InvalidArgument;
    }
    return DownloadErrorKind::InvalidArgument;
}

UpdateCandidate CandidateFrom(const UpdateManifest& manifest) {
    UpdateCandidate candidate;
    candidate.version = manifest.version;
    candidate.buildId = manifest.buildId;
    candidate.assetUrl = manifest.asset.url;
    candidate.assetSize = manifest.asset.size;
    candidate.sha256 = manifest.asset.sha256;
    return candidate;
}

bool IsRuntimeBusy(UpdatePhase phase) {
    return phase == UpdatePhase::Available ||
           phase == UpdatePhase::Downloading;
}

bool IsLowerHexSignature(const std::vector<std::uint8_t>& bytes) {
    constexpr std::size_t kEncodedSignatureBytes = 128;
    if (bytes.size() != kEncodedSignatureBytes) return false;
    for (const auto value : bytes) {
        const bool digit = value >= static_cast<std::uint8_t>('0') &&
                           value <= static_cast<std::uint8_t>('9');
        const bool lower = value >= static_cast<std::uint8_t>('a') &&
                           value <= static_cast<std::uint8_t>('f');
        if (!digit && !lower) return false;
    }
    return true;
}

}  // namespace

UpdateCheckService::UpdateCheckService(UpdateCheckTransportPort& transport,
                                       UpdateRuntime& runtime,
                                       UpdateCheckServiceConfig config)
    : transport_(transport),
      runtime_(runtime),
      capability_(std::make_shared<UpdateCheckServiceCapability>()),
      config_(std::move(config)) {}

UpdateCheckServiceResult UpdateCheckService::Result(
    UpdateCheckServiceDisposition disposition,
    DownloadErrorKind error,
    std::string detail) const {
    UpdateCheckServiceResult result;
    result.disposition = disposition;
    result.snapshot = runtime_.snapshot();
    result.error = error;
    result.detail = std::move(detail);
    return result;
}

UpdateCheckServiceResult UpdateCheckService::Schedule(
    UpdateCheckServiceDisposition disposition,
    UpdateCheckScheduleKind kind,
    std::uint64_t delayMs) {
    const auto intent = NextSchedule(kind, delayMs);
    if (!intent) {
        return FinishTerminal(Result(
            UpdateCheckServiceDisposition::TerminalError,
            DownloadErrorKind::InvalidArgument,
            "update check service generation exhausted"));
    }
    auto result = Result(disposition);
    result.schedule = intent;
    return result;
}

std::optional<UpdateCheckScheduleIntent> UpdateCheckService::NextSchedule(
    UpdateCheckScheduleKind kind,
    std::uint64_t delayMs) {
    if (nextGeneration_ == std::numeric_limits<std::uint64_t>::max()) {
        return std::nullopt;
    }
    UpdateCheckScheduleIntent intent;
    intent.generation = ++nextGeneration_;
    intent.delayMs = delayMs;
    intent.kind = kind;
    intent.capability_ = capability_;
    pendingSchedule_ = intent;
    return intent;
}

bool UpdateCheckService::OwnsSchedule(
    const UpdateCheckScheduleIntent& intent) const {
    return intent.capability_ && intent.capability_ == capability_ &&
           pendingSchedule_ && SameIntent(*pendingSchedule_, intent);
}

UpdateCheckServiceResult UpdateCheckService::Start() {
    if (started_) return Result(UpdateCheckServiceDisposition::NoOp);
    started_ = true;
    return Schedule(UpdateCheckServiceDisposition::Scheduled,
                    UpdateCheckScheduleKind::Startup, kStartupDelayMs);
}

UpdateCheckServiceResult UpdateCheckService::FinishPeriodic(
    UpdateCheckServiceResult result) {
    activeGeneration_ = 0;
    const auto intent = NextSchedule(UpdateCheckScheduleKind::Periodic,
                                     kPeriodicDelayMs);
    if (!intent) {
        return FinishTerminal(Result(
            UpdateCheckServiceDisposition::TerminalError,
            DownloadErrorKind::InvalidArgument,
            "update check service generation exhausted"));
    }
    result.snapshot = runtime_.snapshot();
    result.schedule = intent;
    return result;
}

UpdateCheckServiceResult UpdateCheckService::FinishCompletionRetry(
    UpdateCheckServiceResult result) {
    activeGeneration_ = 0;
    const auto intent = NextSchedule(UpdateCheckScheduleKind::CompletionRetry,
                                     kPeriodicDelayMs);
    result.snapshot = runtime_.snapshot();
    result.schedule = intent;
    if (!intent) pendingSchedule_.reset();
    return result;
}

UpdateCheckServiceResult UpdateCheckService::FinishTerminal(
    UpdateCheckServiceResult result) {
    activeGeneration_ = 0;
    pendingSchedule_.reset();
    result.snapshot = runtime_.snapshot();
    result.schedule.reset();
    return result;
}

UpdateCheckServiceResult UpdateCheckService::PublishCompletion(
    PendingCompletion completion) {
    if (!activeCheckToken_) {
        return FinishTerminal(Result(
            UpdateCheckServiceDisposition::TerminalError,
            DownloadErrorKind::InvalidArgument,
            "update check completion has no active runtime token"));
    }
    pendingCompletion_ = std::move(completion);
    return RetryPendingCompletion();
}

UpdateCheckServiceResult UpdateCheckService::RetryPendingCompletion() {
    if (!activeCheckToken_ || !pendingCompletion_) {
        return FinishTerminal(Result(
            UpdateCheckServiceDisposition::TerminalError,
            DownloadErrorKind::InvalidArgument,
            "update check completion retry is not owned"));
    }

    const PendingCompletion completion = *pendingCompletion_;
    UpdateRuntimeResult completed;
    switch (completion.kind) {
    case PendingCompletionKind::Candidate:
        if (!completion.candidate) {
            return FinishTerminal(Result(
                UpdateCheckServiceDisposition::TerminalError,
                DownloadErrorKind::InvalidArgument,
                "checked candidate completion is missing its candidate"));
        }
        completed = runtime_.CompleteCheckedCandidate(
            *activeCheckToken_, *completion.candidate);
        break;
    case PendingCompletionKind::NoUpdate:
        completed = runtime_.CompleteNoUpdate(*activeCheckToken_);
        break;
    case PendingCompletionKind::Failure:
        completed = runtime_.FailActiveCheck(
            *activeCheckToken_, completion.error, completion.detail);
        break;
    case PendingCompletionKind::Cancelled:
        completed = runtime_.CancelActiveCheck(
            *activeCheckToken_, completion.detail);
        break;
    }

    if (completed.disposition == UpdateRuntimeDisposition::NoOp) {
        activeCheckToken_.reset();
        pendingCompletion_.reset();
        return FinishTerminal(Result(UpdateCheckServiceDisposition::NoOp));
    }
    if (completed.error == DownloadErrorKind::Disk) {
        return FinishCompletionRetry(Result(
            UpdateCheckServiceDisposition::TerminalError,
            DownloadErrorKind::Disk, completed.detail));
    }

    UpdateCheckServiceResult result;
    bool accepted = false;
    switch (completion.kind) {
    case PendingCompletionKind::Candidate:
        accepted = completed.disposition ==
                       UpdateRuntimeDisposition::UpdateAvailable &&
                   completed.candidateHandoff.has_value();
        result = Result(UpdateCheckServiceDisposition::UpdateAvailable);
        result.candidateHandoff = completed.candidateHandoff;
        break;
    case PendingCompletionKind::NoUpdate:
        accepted = completed.disposition == UpdateRuntimeDisposition::NoUpdate;
        result = Result(UpdateCheckServiceDisposition::NoUpdate);
        result.manifestError = completion.manifestError;
        break;
    case PendingCompletionKind::Failure:
        accepted = completed.disposition == UpdateRuntimeDisposition::Ready ||
                   completed.disposition ==
                       UpdateRuntimeDisposition::TerminalError;
        result = Result(UpdateCheckServiceDisposition::Failed,
                        completion.error, completion.detail);
        result.manifestError = completion.manifestError;
        result.fetchError = completion.fetchError;
        break;
    case PendingCompletionKind::Cancelled:
        accepted = completed.disposition ==
                       UpdateRuntimeDisposition::Cancelled;
        result = Result(UpdateCheckServiceDisposition::Cancelled,
                        DownloadErrorKind::Aborted, completion.detail);
        break;
    }
    if (!accepted) {
        return FinishTerminal(Result(
            UpdateCheckServiceDisposition::TerminalError,
            completed.error, completed.detail));
    }

    activeCheckToken_.reset();
    pendingCompletion_.reset();
    return FinishPeriodic(std::move(result));
}

UpdateCheckServiceResult UpdateCheckService::PublishFailure(
    DownloadErrorKind error,
    std::string detail,
    ManifestError manifestError,
    UpdateCheckFetchError fetchError) {
    PendingCompletion completion;
    completion.kind = PendingCompletionKind::Failure;
    completion.error = error;
    completion.detail = std::move(detail);
    completion.manifestError = manifestError;
    completion.fetchError = fetchError;
    return PublishCompletion(std::move(completion));
}

UpdateCheckServiceResult UpdateCheckService::PublishCancellation(
    std::string detail) {
    PendingCompletion completion;
    completion.kind = PendingCompletionKind::Cancelled;
    completion.error = DownloadErrorKind::Aborted;
    completion.detail = detail.empty() ? "update check cancelled"
                                       : std::move(detail);
    return PublishCompletion(std::move(completion));
}

UpdateCheckServiceResult UpdateCheckService::ExecuteActive(
    const UpdateCheckCancellation& cancellation) {
    if (cancellation.IsCancellationRequested()) {
        return FinishPeriodic(Result(UpdateCheckServiceDisposition::Cancelled,
                                     DownloadErrorKind::Aborted,
                                     "update check cancelled"));
    }

    const auto begun = runtime_.BeginCheck();
    if (!begun.checkToken) {
        return FinishTerminal(Result(UpdateCheckServiceDisposition::TerminalError,
                                     begun.error, begun.detail));
    }
    activeCheckToken_ = begun.checkToken;

    const bool allowRedirects = config_.fallbackManifestUrl.empty();
    bool useFallback = false;
    UpdateCheckFetchResult manifestFetch;
    try {
        manifestFetch = transport_.Fetch(
            {config_.manifestUrl, config_.manifestTimeoutMs,
             config_.manifestBodyLimit, allowRedirects},
            cancellation);
        if (!allowRedirects &&
            manifestFetch.error == UpdateCheckFetchError::Http4xx &&
            manifestFetch.httpStatus == 404) {
            if (cancellation.IsCancellationRequested()) {
                return PublishCancellation("update check cancelled");
            }
            useFallback = true;
            manifestFetch = transport_.Fetch(
                {config_.fallbackManifestUrl, config_.manifestTimeoutMs,
                 config_.manifestBodyLimit, allowRedirects},
                cancellation);
        }
    } catch (...) {
        manifestFetch.error = UpdateCheckFetchError::Network;
        manifestFetch.detail = "manifest fetch failed unexpectedly";
    }
    if (!manifestFetch.ok()) {
        if (manifestFetch.error == UpdateCheckFetchError::Cancelled) {
            return PublishCancellation(manifestFetch.detail);
        }
        return PublishFailure(Classification(manifestFetch.error),
                              manifestFetch.detail,
                              ManifestError::None,
                              manifestFetch.error);
    }
    if (cancellation.IsCancellationRequested()) {
        return PublishCancellation("update check cancelled");
    }
    if (manifestFetch.body.size() > config_.manifestBodyLimit) {
        return PublishFailure(DownloadErrorKind::InvalidArgument,
                              "manifest response exceeds trusted body limit",
                              ManifestError::None,
                              UpdateCheckFetchError::BodyTooLarge);
    }

    UpdateCheckFetchResult signatureFetch;
    try {
        UpdateCheckFetchRequest signatureRequest{config_.signatureUrl,
            config_.signatureTimeoutMs, config_.signatureBodyLimit, allowRedirects};
        if (useFallback) {
            signatureRequest.url = config_.fallbackManifestUrl + ".sig";
        }
        signatureFetch = transport_.Fetch(signatureRequest, cancellation);
    } catch (...) {
        signatureFetch.error = UpdateCheckFetchError::Network;
        signatureFetch.detail = "signature fetch failed unexpectedly";
    }
    if (!signatureFetch.ok()) {
        if (signatureFetch.error == UpdateCheckFetchError::Cancelled) {
            return PublishCancellation(signatureFetch.detail);
        }
        return PublishFailure(Classification(signatureFetch.error),
                              signatureFetch.detail,
                              ManifestError::None,
                              signatureFetch.error);
    }
    if (cancellation.IsCancellationRequested()) {
        return PublishCancellation("update check cancelled");
    }
    if (signatureFetch.body.size() > config_.signatureBodyLimit) {
        return PublishFailure(DownloadErrorKind::InvalidArgument,
                              "signature response exceeds trusted body limit",
                              ManifestError::None,
                              UpdateCheckFetchError::BodyTooLarge);
    }
    if (!IsLowerHexSignature(signatureFetch.body)) {
        return PublishFailure(
            DownloadErrorKind::InvalidArgument,
            "signature must be exact lowercase hexadecimal Ed25519 data",
            ManifestError::SignatureEncodingInvalid,
            UpdateCheckFetchError::None);
    }

    const auto verified = VerifyAndParseManifest(
        OwnedText(manifestFetch.body), OwnedText(signatureFetch.body),
        config_.publicKeyHex, config_.policy);
    if (cancellation.IsCancellationRequested()) {
        return PublishCancellation("update check cancelled");
    }
    if (verified.error == ManifestError::NotNewer) {
        PendingCompletion completion;
        completion.kind = PendingCompletionKind::NoUpdate;
        completion.manifestError = ManifestError::NotNewer;
        return PublishCompletion(std::move(completion));
    }
    if (!verified.ok()) {
        return PublishFailure(DownloadErrorKind::InvalidArgument,
                              verified.detail, verified.error,
                              UpdateCheckFetchError::None);
    }

    PendingCompletion completion;
    completion.kind = PendingCompletionKind::Candidate;
    completion.candidate = CandidateFrom(verified.manifest);
    return PublishCompletion(std::move(completion));
}

UpdateCheckServiceResult UpdateCheckService::CheckNow(
    const UpdateCheckCancellation& cancellation) {
    if (pendingCompletion_) return RetryPendingCompletion();
    if (activeGeneration_ != 0) {
        return Result(UpdateCheckServiceDisposition::Joined);
    }
    const auto phase = runtime_.snapshot().phase;
    // XDMaker-compatible manual semantics: a staged patch awaiting restart is
    // reported immediately; periodic checks may still enter Superseding.
    if (phase == UpdatePhase::Ready) {
        return Result(UpdateCheckServiceDisposition::Ready);
    }
    if (IsRuntimeBusy(phase)) {
        return Result(UpdateCheckServiceDisposition::Busy);
    }
    if (nextGeneration_ == std::numeric_limits<std::uint64_t>::max()) {
        return FinishTerminal(Result(
            UpdateCheckServiceDisposition::TerminalError,
            DownloadErrorKind::InvalidArgument,
            "update check service generation exhausted"));
    }

    pendingSchedule_.reset();
    activeGeneration_ = ++nextGeneration_;
    return ExecuteActive(cancellation);
}

UpdateCheckServiceResult UpdateCheckService::ExecuteScheduled(
    const UpdateCheckScheduleIntent& intent,
    const UpdateCheckCancellation& cancellation) {
    if (!OwnsSchedule(intent)) {
        return Result(UpdateCheckServiceDisposition::NoOp);
    }
    if (activeGeneration_ != 0) {
        return Result(activeGeneration_ == intent.generation
                          ? UpdateCheckServiceDisposition::Joined
                          : UpdateCheckServiceDisposition::Busy);
    }
    activeGeneration_ = intent.generation;
    if (pendingCompletion_) return RetryPendingCompletion();
    if (IsRuntimeBusy(runtime_.snapshot().phase)) {
        return FinishPeriodic(Result(UpdateCheckServiceDisposition::Busy));
    }
    return ExecuteActive(cancellation);
}

}  // namespace aris::update
