#include "update/UpdateCheckService.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace aris::update;

static_assert(static_cast<int>(UpdateCheckScheduleKind::Startup) == 0);
static_assert(static_cast<int>(UpdateCheckScheduleKind::Periodic) == 1);
static_assert(static_cast<int>(UpdateCheckScheduleKind::CompletionRetry) == 2);

int failures = 0;

void check(bool value, const char* message) {
    if (value) return;
    std::fprintf(stderr, "FAIL: %s\n", message);
    ++failures;
}

class NullDownloadTransport final : public HttpTransportPort {
public:
    HttpTransportExecutionResult Execute(
        const HttpTransferRequestPlan&,
        const HttpTransportCancellation&,
        HttpTransportSink&) override {
        ++executions;
        HttpTransportExecutionResult result;
        result.error = HttpTransportExecutionError::Network;
        return result;
    }

    int executions = 0;
};

class FakePersistence final : public UpdatePersistencePort {
public:
    UpdatePersistenceResult ExecuteStoreIntents(
        const std::vector<StoreIntent>& intents) override {
        UpdatePersistenceResult result;
        ++storePlans;
        if (failStoreAt >= 0 && storePlans - 1 == failStoreAt) {
            result.error = UpdatePersistenceError::WriteFailed;
            return result;
        }
        result.completedIntents = intents.size();
        return result;
    }

    UpdatePersistenceResult ExecuteDownloadIntents(
        const std::vector<DownloadCleanupIntent>& intents) override {
        UpdatePersistenceResult result;
        result.completedIntents = intents.size();
        return result;
    }

    UpdatePersistenceResult ActivateTransfer(
        const DownloadFlightState&,
        const DownloadFlightToken&,
        const HttpTransferIdentity&) override {
        return {};
    }

    UpdatePersistenceResult DeactivateTransfer(
        const DownloadFlightToken&,
        const HttpTransferIdentity&) override {
        return {};
    }

    UpdatePersistenceResult ExecuteTransferChunk(
        const TransferAppendIntent&,
        const ResumeMetadataIntent&) override {
        return {};
    }

    ResumeMetadataObservation ObserveResumeMetadata(
        const std::string&) override {
        ResumeMetadataObservation result;
        result.status = ResumeMetadataStatus::Missing;
        return result;
    }

    StagedIdentityObservation ObserveStagedPatch(
        const std::string&) override {
        StagedIdentityObservation result;
        result.status = StagedIdentityStatus::Missing;
        return result;
    }

    int storePlans = 0;
    int failStoreAt = -1;
};

class MutableCancellation final : public UpdateCheckCancellation {
public:
    bool IsCancellationRequested() const override { return cancelled; }
    std::unique_ptr<InterruptRegistration> RegisterInterrupt(
        std::function<void()>) const override {
        class Registration final : public InterruptRegistration {};
        return std::make_unique<Registration>();
    }

    bool cancelled = false;
};

class SequencedCancellation final : public UpdateCheckCancellation {
public:
    explicit SequencedCancellation(int cancelAtObservation)
        : cancelAtObservation_(cancelAtObservation) {}

    bool IsCancellationRequested() const override {
        return ++observations_ >= cancelAtObservation_;
    }
    std::unique_ptr<InterruptRegistration> RegisterInterrupt(
        std::function<void()>) const override {
        class Registration final : public InterruptRegistration {};
        return std::make_unique<Registration>();
    }

    int observations() const { return observations_; }

private:
    int cancelAtObservation_ = 0;
    mutable int observations_ = 0;
};

class ScriptedCheckTransport final : public UpdateCheckTransportPort {
public:
    UpdateCheckFetchResult Fetch(
        const UpdateCheckFetchRequest& request,
        const UpdateCheckCancellation&) override {
        requests.push_back(request);
        const std::size_t index = requests.size() - 1;
        if (onFetch) onFetch(index);
        if (index < responses.size()) return responses[index];
        UpdateCheckFetchResult result;
        result.error = UpdateCheckFetchError::Network;
        result.detail = "unscripted fetch";
        return result;
    }

    std::vector<UpdateCheckFetchResult> responses;
    std::vector<UpdateCheckFetchRequest> requests;
    std::function<void(std::size_t)> onFetch;
};

UpdateCheckServiceConfig config() {
    UpdateCheckServiceConfig value;
    value.manifestUrl = "https://updates.example.invalid/testing/manifest.json";
    value.signatureUrl = "https://updates.example.invalid/testing/manifest.sig";
    value.publicKeyHex =
        "03a107bff3ce10be1d70dd18e74bc09967e4d6309ba50d5f1ddc8664125531b8";
    value.policy.currentVersion = "1.1.0";
    value.policy.selectedChannel = UpdateChannel::Testing;
    value.policy.platform = "windows";
    value.policy.architecture = "x64";
    value.policy.allowedRepository = "xindong/harken-search";
    value.manifestTimeoutMs = 1700;
    value.signatureTimeoutMs = 900;
    value.manifestBodyLimit = 4096;
    value.signatureBodyLimit = 128;
    return value;
}

const std::string kManifest =
    R"({"schema_version":1,"product":"aris","channel":"testing","version":"1.2.0","platform":"windows","architecture":"x64","minimum_bootstrap_version":"1.0.0","minimum_direct_version":"1.0.0","data_compatibility":"backward_compatible","asset":{"url":"https://github.com/xindong/harken-search/releases/download/v1.2.0/Aris-1.2.0-win-x64-update.zip","size":42,"sha256":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"},"published_at":"2026-07-28T00:00:00Z","build_id":"abc123"})";
const std::string kSignature =
    "2aaeda32149871bb4dcdd1ea88a27397cb0f6f2a2cabc7729f4e22923db48579"
    "c4f17ce7fb0a0987a606ffc4830c9d341ac8964ede607017da6afd4b1096f10c";

UpdateCheckFetchResult fetched(const std::string& bytes) {
    UpdateCheckFetchResult result;
    result.body.assign(bytes.begin(), bytes.end());
    return result;
}

void scriptValidManifest(ScriptedCheckTransport& transport) {
    transport.responses.push_back(fetched(kManifest));
    transport.responses.push_back(fetched(kSignature));
}

UpdateCandidate candidate(const std::string& version, char hash = 'a') {
    UpdateCandidate value;
    value.version = version;
    value.buildId = "build-" + version;
    value.assetUrl = "https://github.com/xindong/harken-search/releases/download/v" +
                     version + "/Aris-" + version +
                     "-win-x64-update.zip";
    value.assetSize = 42;
    value.sha256 = std::string(64, hash);
    return value;
}

bool sameCandidate(const UpdateCandidate& left,
                   const UpdateCandidate& right) {
    return left.version == right.version && left.buildId == right.buildId &&
           left.assetUrl == right.assetUrl &&
           left.assetSize == right.assetSize && left.sha256 == right.sha256;
}

enum class CheckOutcomeCase {
    Candidate,
    NoUpdate,
    Failure,
    Cancelled,
};

void scriptOutcome(CheckOutcomeCase outcome,
                   ScriptedCheckTransport& transport,
                   UpdateCheckServiceConfig& trusted,
                   MutableCancellation& cancellation) {
    switch (outcome) {
    case CheckOutcomeCase::Candidate:
        scriptValidManifest(transport);
        break;
    case CheckOutcomeCase::NoUpdate:
        scriptValidManifest(transport);
        trusted.policy.currentVersion = "1.2.0";
        break;
    case CheckOutcomeCase::Failure: {
        UpdateCheckFetchResult network;
        network.error = UpdateCheckFetchError::Network;
        network.detail = "offline";
        transport.responses.push_back(network);
        break;
    }
    case CheckOutcomeCase::Cancelled:
        transport.responses.push_back(fetched(kManifest));
        transport.onFetch = [&](std::size_t index) {
            if (index == 0) cancellation.cancelled = true;
        };
        break;
    }
}

std::size_t expectedFetches(CheckOutcomeCase outcome) {
    return outcome == CheckOutcomeCase::Candidate ||
                   outcome == CheckOutcomeCase::NoUpdate
               ? 2
               : 1;
}

bool matchesOutcome(const UpdateCheckServiceResult& result,
                    CheckOutcomeCase outcome,
                    UpdatePhase expectedPhase) {
    if (result.snapshot.phase != expectedPhase) return false;
    switch (outcome) {
    case CheckOutcomeCase::Candidate:
        return result.disposition ==
                   UpdateCheckServiceDisposition::UpdateAvailable &&
               result.candidateHandoff.has_value();
    case CheckOutcomeCase::NoUpdate:
        return result.disposition == UpdateCheckServiceDisposition::NoUpdate &&
               result.manifestError == ManifestError::NotNewer;
    case CheckOutcomeCase::Failure:
        return result.disposition == UpdateCheckServiceDisposition::Failed &&
               result.error == DownloadErrorKind::Network &&
               result.fetchError == UpdateCheckFetchError::Network &&
               result.detail == "offline";
    case CheckOutcomeCase::Cancelled:
        return result.disposition ==
                   UpdateCheckServiceDisposition::Cancelled &&
               result.error == DownloadErrorKind::Aborted;
    }
    return false;
}

UpdateStoreRecord readyRecord(const UpdateCandidate& patch) {
    UpdateStoreRecord record;
    record.snapshot.phase = UpdatePhase::Ready;
    record.snapshot.readyPatch = patch;
    record.snapshot.downloadedBytes = patch.assetSize;
    record.stagedRelativePath = patch.version + "/Aris-" + patch.version +
                                "-win-x64-update.zip";
    return record;
}

void test_start_emits_one_delayed_intent_without_io() {
    NullDownloadTransport downloadTransport;
    FakePersistence persistence;
    UpdateRuntime runtime(downloadTransport, persistence, {});
    ScriptedCheckTransport checkTransport;
    UpdateCheckService service(checkTransport, runtime, config());

    const auto started = service.Start();
    check(started.disposition == UpdateCheckServiceDisposition::Scheduled &&
              started.schedule.has_value() &&
              started.schedule->kind == UpdateCheckScheduleKind::Startup &&
              started.schedule->delayMs == 10000 &&
              started.schedule->generation != 0 &&
              checkTransport.requests.empty() && persistence.storePlans == 0,
          "start emits one 10-second startup intent with zero I/O");

    const auto repeated = service.Start();
    check(repeated.disposition == UpdateCheckServiceDisposition::NoOp &&
              !repeated.schedule && checkTransport.requests.empty() &&
              persistence.storePlans == 0,
          "start is idempotent and emits exactly one startup intent");
}

void test_scheduled_valid_newer_manifest_returns_owned_handoff_and_periodic_intent(
    bool preferredMissing = false) {
    NullDownloadTransport downloadTransport;
    FakePersistence persistence;
    UpdateRuntime runtime(downloadTransport, persistence, {});
    ScriptedCheckTransport checkTransport;
    auto trusted = config();
    if (preferredMissing) {
        trusted.fallbackManifestUrl = trusted.manifestUrl + ".legacy.json";
        UpdateCheckFetchResult missing;
        missing.error = UpdateCheckFetchError::Http4xx;
        missing.httpStatus = 404;
        checkTransport.responses.push_back(missing);
    }
    scriptValidManifest(checkTransport);
    UpdateCheckService service(checkTransport, runtime, trusted);
    MutableCancellation cancellation;

    const auto started = service.Start();
    const auto completed = service.ExecuteScheduled(
        *started.schedule, cancellation);
    check(completed.disposition ==
                  UpdateCheckServiceDisposition::UpdateAvailable &&
              completed.candidateHandoff.has_value() &&
              completed.snapshot.phase == UpdatePhase::Available &&
              completed.snapshot.candidate.has_value() &&
              completed.snapshot.candidate->version == "1.2.0" &&
              completed.snapshot.candidate->buildId == "abc123" &&
              completed.snapshot.candidate->assetSize == 42 &&
              completed.snapshot.candidate->assetUrl ==
                  "https://github.com/xindong/harken-search/releases/download/"
                  "v1.2.0/Aris-1.2.0-win-x64-update.zip" &&
              completed.snapshot.candidate->sha256 == std::string(64, 'a') &&
              completed.schedule.has_value() &&
              completed.schedule->kind == UpdateCheckScheduleKind::Periodic &&
              completed.schedule->delayMs == 30 * 60 * 1000 &&
              completed.schedule->generation > started.schedule->generation,
          "valid scheduled check returns one owned handoff and 30-minute intent");
    const std::size_t manifestIndex = preferredMissing ? 1 : 0;
    const auto manifestUrl = preferredMissing ? trusted.fallbackManifestUrl
                                              : trusted.manifestUrl;
    const auto signatureUrl = preferredMissing ? manifestUrl + ".sig"
                                               : trusted.signatureUrl;
    if (preferredMissing) {
        check(!checkTransport.requests.empty() &&
                  checkTransport.requests.front().url == trusted.manifestUrl &&
                  !checkTransport.requests.front().allowRedirects,
              "preferred mirror is attempted without redirects before legacy");
    }
    check(checkTransport.requests.size() == manifestIndex + 2 &&
              checkTransport.requests[manifestIndex].url == manifestUrl &&
              checkTransport.requests[manifestIndex].allowRedirects == !preferredMissing &&
              checkTransport.requests[manifestIndex].timeoutMs ==
                  trusted.manifestTimeoutMs &&
              checkTransport.requests[manifestIndex].maxBodyBytes ==
                  trusted.manifestBodyLimit &&
              checkTransport.requests[manifestIndex + 1].url == signatureUrl &&
              checkTransport.requests[manifestIndex + 1].allowRedirects == !preferredMissing &&
              checkTransport.requests[manifestIndex + 1].timeoutMs ==
                  trusted.signatureTimeoutMs &&
              checkTransport.requests[manifestIndex + 1].maxBodyBytes ==
                  trusted.signatureBodyLimit &&
              persistence.storePlans == 2 &&
              downloadTransport.executions == 0,
          "trusted config owns ordered manifest/signature fetch inputs");

    const auto storesBeforeReplay = persistence.storePlans;
    const auto fetchesBeforeReplay = checkTransport.requests.size();
    const auto replayed = service.ExecuteScheduled(
        *started.schedule, cancellation);
    check(replayed.disposition == UpdateCheckServiceDisposition::NoOp &&
              !replayed.schedule &&
              persistence.storePlans == storesBeforeReplay &&
              checkTransport.requests.size() == fetchesBeforeReplay,
          "stale scheduled generation is an inert no-op");
}

void test_manual_no_update_runs_immediately_and_supersedes_startup_intent() {
    NullDownloadTransport downloadTransport;
    FakePersistence persistence;
    UpdateRuntime runtime(downloadTransport, persistence, {});
    ScriptedCheckTransport checkTransport;
    scriptValidManifest(checkTransport);
    auto trusted = config();
    trusted.policy.currentVersion = "1.2.0";
    UpdateCheckService service(checkTransport, runtime, trusted);
    MutableCancellation cancellation;

    const auto started = service.Start();
    const auto manual = service.CheckNow(cancellation);
    check(manual.disposition == UpdateCheckServiceDisposition::NoUpdate &&
              manual.snapshot.phase == UpdatePhase::Idle &&
              !manual.candidateHandoff && manual.schedule.has_value() &&
              manual.schedule->kind == UpdateCheckScheduleKind::Periodic &&
              manual.schedule->delayMs == 30 * 60 * 1000 &&
              manual.schedule->generation > started.schedule->generation &&
              checkTransport.requests.size() == 2 &&
              persistence.storePlans == 2,
          "idle manual check executes immediately and schedules one periodic check");

    const auto staleStartup = service.ExecuteScheduled(
        *started.schedule, cancellation);
    check(staleStartup.disposition == UpdateCheckServiceDisposition::NoOp &&
              checkTransport.requests.size() == 2 &&
              persistence.storePlans == 2,
          "manual execution invalidates the pending startup generation");
}

void test_runtime_owns_check_through_fetch_and_final_cancellation() {
    {
        NullDownloadTransport downloadTransport;
        FakePersistence persistence;
        UpdateRuntime runtime(downloadTransport, persistence, {});
        ScriptedCheckTransport checkTransport;
        scriptValidManifest(checkTransport);
        UpdateCheckService service(checkTransport, runtime, config());
        MutableCancellation cancellation;
        bool checkingDuringManifest = false;
        bool checkingDuringSignature = false;
        checkTransport.onFetch = [&](std::size_t index) {
            const auto phase = runtime.snapshot().phase;
            if (index == 0) checkingDuringManifest =
                phase == UpdatePhase::Checking;
            if (index == 1) checkingDuringSignature =
                phase == UpdatePhase::Checking;
        };

        const auto result = service.CheckNow(cancellation);
        check(result.disposition ==
                  UpdateCheckServiceDisposition::UpdateAvailable &&
                  checkingDuringManifest && checkingDuringSignature &&
                  persistence.storePlans == 2,
              "runtime owns Checking throughout both blocking fetches");
    }
    {
        NullDownloadTransport downloadTransport;
        FakePersistence persistence;
        const auto oldPatch = candidate("1.1.0");
        UpdateRuntime runtime(downloadTransport, persistence, {},
                              readyRecord(oldPatch));
        ScriptedCheckTransport checkTransport;
        scriptValidManifest(checkTransport);
        UpdateCheckService service(checkTransport, runtime, config());
        MutableCancellation cancellation;
        bool supersedingDuringManifest = false;
        bool supersedingDuringSignature = false;
        checkTransport.onFetch = [&](std::size_t index) {
            const auto phase = runtime.snapshot().phase;
            if (index == 0) supersedingDuringManifest =
                phase == UpdatePhase::Superseding;
            if (index == 1) supersedingDuringSignature =
                phase == UpdatePhase::Superseding;
        };

        const auto started = service.Start();
        const auto result = service.ExecuteScheduled(
            *started.schedule, cancellation);
        check(result.disposition ==
                  UpdateCheckServiceDisposition::UpdateAvailable &&
                  supersedingDuringManifest && supersedingDuringSignature &&
                  result.snapshot.phase == UpdatePhase::Ready,
              "runtime owns Superseding throughout both blocking fetches");
    }
    {
        NullDownloadTransport downloadTransport;
        FakePersistence persistence;
        UpdateRuntime runtime(downloadTransport, persistence, {});
        ScriptedCheckTransport checkTransport;
        scriptValidManifest(checkTransport);
        UpdateCheckService service(checkTransport, runtime, config());
        SequencedCancellation cancellation(4);

        const auto result = service.CheckNow(cancellation);
        check(result.disposition == UpdateCheckServiceDisposition::Cancelled &&
                  result.error == DownloadErrorKind::Aborted &&
                  result.snapshot.phase == UpdatePhase::Idle &&
                  result.schedule.has_value() &&
                  checkTransport.requests.size() == 2 &&
                  persistence.storePlans == 2 &&
                  cancellation.observations() >= 4,
              "cancellation observed after verification aborts before publication");
    }
}

void test_response_body_caps_and_lowercase_signature_are_enforced() {
    {
        NullDownloadTransport downloadTransport;
        FakePersistence persistence;
        UpdateRuntime runtime(downloadTransport, persistence, {});
        ScriptedCheckTransport checkTransport;
        scriptValidManifest(checkTransport);
        auto trusted = config();
        trusted.manifestBodyLimit = kManifest.size() - 1;
        UpdateCheckService service(checkTransport, runtime, trusted);
        MutableCancellation cancellation;

        const auto result = service.CheckNow(cancellation);
        check(result.disposition == UpdateCheckServiceDisposition::Failed &&
                  result.error == DownloadErrorKind::InvalidArgument &&
                  result.fetchError == UpdateCheckFetchError::BodyTooLarge &&
                  !result.candidateHandoff && result.schedule.has_value() &&
                  result.schedule->delayMs == 30 * 60 * 1000 &&
                  checkTransport.requests.size() == 1 &&
                  persistence.storePlans == 2,
              "oversized manifest stops before signature and schedules periodic retry");
    }
    {
        NullDownloadTransport downloadTransport;
        FakePersistence persistence;
        UpdateRuntime runtime(downloadTransport, persistence, {});
        ScriptedCheckTransport checkTransport;
        scriptValidManifest(checkTransport);
        auto trusted = config();
        trusted.signatureBodyLimit = kSignature.size() - 1;
        UpdateCheckService service(checkTransport, runtime, trusted);
        MutableCancellation cancellation;

        const auto result = service.CheckNow(cancellation);
        check(result.disposition == UpdateCheckServiceDisposition::Failed &&
                  result.fetchError == UpdateCheckFetchError::BodyTooLarge &&
                  !result.candidateHandoff &&
                  checkTransport.requests.size() == 2,
              "oversized detached signature is rejected before verification");
    }
    {
        NullDownloadTransport downloadTransport;
        FakePersistence persistence;
        UpdateRuntime runtime(downloadTransport, persistence, {});
        ScriptedCheckTransport checkTransport;
        std::string uppercase = kSignature;
        std::transform(uppercase.begin(), uppercase.end(), uppercase.begin(),
                       [](unsigned char value) {
                           return static_cast<char>(std::toupper(value));
                       });
        checkTransport.responses.push_back(fetched(kManifest));
        checkTransport.responses.push_back(fetched(uppercase));
        UpdateCheckService service(checkTransport, runtime, config());
        MutableCancellation cancellation;

        const auto result = service.CheckNow(cancellation);
        check(result.disposition == UpdateCheckServiceDisposition::Failed &&
                  result.error == DownloadErrorKind::InvalidArgument &&
                  result.manifestError ==
                      ManifestError::SignatureEncodingInvalid &&
                  !result.candidateHandoff,
              "detached signature must use exact lowercase hexadecimal bytes");
    }
}

void test_transport_and_manifest_failures_are_classified_and_rearmed() {
    struct FetchFailureCase {
        UpdateCheckFetchError fetchError;
        DownloadErrorKind expected;
    };
    const FetchFailureCase fetchFailures[] = {
        {UpdateCheckFetchError::Network, DownloadErrorKind::Network},
        {UpdateCheckFetchError::Http4xx, DownloadErrorKind::Http4xx},
        {UpdateCheckFetchError::Http5xx, DownloadErrorKind::Http5xx},
        {UpdateCheckFetchError::InvalidResponse,
         DownloadErrorKind::InvalidArgument},
    };
    for (const auto& failure : fetchFailures) {
        NullDownloadTransport downloadTransport;
        FakePersistence persistence;
        UpdateRuntime runtime(downloadTransport, persistence, {});
        ScriptedCheckTransport checkTransport;
        UpdateCheckFetchResult response;
        response.error = failure.fetchError;
        response.detail = "manifest transport failed";
        response.httpStatus =
            failure.fetchError == UpdateCheckFetchError::Http4xx ? 403 : 0;
        checkTransport.responses.push_back(response);
        auto trusted = config();
        trusted.fallbackManifestUrl = trusted.manifestUrl + ".legacy.json";
        UpdateCheckService service(checkTransport, runtime, trusted);
        MutableCancellation cancellation;

        const auto result = service.CheckNow(cancellation);
        check(result.disposition == UpdateCheckServiceDisposition::Failed &&
                  result.error == failure.expected &&
                  result.fetchError == failure.fetchError &&
                  result.snapshot.phase == UpdatePhase::Error &&
                  result.snapshot.lastError == "manifest transport failed" &&
                  result.schedule.has_value() &&
                  result.schedule->kind == UpdateCheckScheduleKind::Periodic &&
                  result.schedule->delayMs == 30 * 60 * 1000 &&
                  persistence.storePlans == 2 &&
                  checkTransport.requests.size() == 1,
              "transport failure classification persists and rearms once");
    }

    {
        NullDownloadTransport downloadTransport;
        FakePersistence persistence;
        UpdateRuntime runtime(downloadTransport, persistence, {});
        ScriptedCheckTransport checkTransport;
        std::string tampered = kManifest;
        tampered.replace(tampered.find("1.2.0"), 5, "9.9.9");
        checkTransport.responses.push_back(fetched(tampered));
        checkTransport.responses.push_back(fetched(kSignature));
        scriptValidManifest(checkTransport);
        auto trusted = config();
        trusted.fallbackManifestUrl = trusted.manifestUrl + ".legacy.json";
        UpdateCheckService service(checkTransport, runtime, trusted);
        MutableCancellation cancellation;

        const auto result = service.CheckNow(cancellation);
        check(result.disposition == UpdateCheckServiceDisposition::Failed &&
                  result.error == DownloadErrorKind::InvalidArgument &&
                  result.manifestError == ManifestError::SignatureInvalid &&
                  result.snapshot.phase == UpdatePhase::Error &&
                  result.schedule.has_value(),
              "tampered raw manifest fails signature before semantic parsing");
        check(checkTransport.requests.size() == 2,
              "invalid primary signature does not fetch the legacy pair");
    }
    {
        NullDownloadTransport downloadTransport;
        FakePersistence persistence;
        UpdateRuntime runtime(downloadTransport, persistence, {});
        ScriptedCheckTransport checkTransport;
        checkTransport.responses.push_back(fetched(kManifest));
        UpdateCheckFetchResult missingSignature;
        missingSignature.error = UpdateCheckFetchError::Http4xx;
        missingSignature.httpStatus = 404;
        checkTransport.responses.push_back(missingSignature);
        scriptValidManifest(checkTransport);
        auto trusted = config();
        trusted.fallbackManifestUrl = trusted.manifestUrl + ".legacy.json";
        UpdateCheckService service(checkTransport, runtime, trusted);
        MutableCancellation cancellation;

        const auto result = service.CheckNow(cancellation);
        check(result.disposition == UpdateCheckServiceDisposition::Failed &&
                  result.fetchError == UpdateCheckFetchError::Http4xx &&
                  checkTransport.requests.size() == 2,
              "missing primary signature does not fetch the legacy pair");
    }
    {
        NullDownloadTransport downloadTransport;
        FakePersistence persistence;
        UpdateRuntime runtime(downloadTransport, persistence, {});
        ScriptedCheckTransport checkTransport;
        scriptValidManifest(checkTransport);
        auto trusted = config();
        trusted.policy.selectedChannel = UpdateChannel::Stable;
        UpdateCheckService service(checkTransport, runtime, trusted);
        MutableCancellation cancellation;

        const auto result = service.CheckNow(cancellation);
        check(result.disposition == UpdateCheckServiceDisposition::Failed &&
                  result.manifestError == ManifestError::ChannelMismatch &&
                  result.error == DownloadErrorKind::InvalidArgument &&
                  result.snapshot.phase == UpdatePhase::Error &&
                  result.schedule.has_value(),
              "trusted channel policy rejects a valid signature for another channel");
    }
}

void test_trusted_config_controls_key_repository_and_fetch_contract() {
    {
        NullDownloadTransport downloadTransport;
        FakePersistence persistence;
        UpdateRuntime runtime(downloadTransport, persistence, {});
        ScriptedCheckTransport checkTransport;
        scriptValidManifest(checkTransport);
        auto trusted = config();
        trusted.publicKeyHex = std::string(64, '0');
        UpdateCheckService service(checkTransport, runtime, trusted);
        MutableCancellation cancellation;

        const auto result = service.CheckNow(cancellation);
        check(result.disposition == UpdateCheckServiceDisposition::Failed &&
                  result.manifestError == ManifestError::SignatureInvalid &&
                  result.error == DownloadErrorKind::InvalidArgument &&
                  checkTransport.requests.size() == 2 &&
                  checkTransport.requests[0].url == trusted.manifestUrl &&
                  checkTransport.requests[1].url == trusted.signatureUrl,
              "only trusted construction key and URLs govern verification fetches");
    }
    {
        NullDownloadTransport downloadTransport;
        FakePersistence persistence;
        UpdateRuntime runtime(downloadTransport, persistence, {});
        ScriptedCheckTransport checkTransport;
        scriptValidManifest(checkTransport);
        auto trusted = config();
        trusted.policy.allowedRepository = "attacker/other";
        UpdateCheckService service(checkTransport, runtime, trusted);
        MutableCancellation cancellation;

        const auto result = service.CheckNow(cancellation);
        check(result.disposition == UpdateCheckServiceDisposition::Failed &&
                  result.manifestError ==
                      ManifestError::RepositoryNotAllowed &&
                  result.error == DownloadErrorKind::InvalidArgument &&
                  !result.candidateHandoff,
              "signed manifest asset cannot override trusted repository policy");
    }
}

void test_cancellation_overlap_and_runtime_admission_are_deterministic() {
    {
        NullDownloadTransport downloadTransport;
        FakePersistence persistence;
        UpdateRuntime runtime(downloadTransport, persistence, {});
        ScriptedCheckTransport checkTransport;
        UpdateCheckService service(checkTransport, runtime, config());
        MutableCancellation cancellation;
        cancellation.cancelled = true;

        const auto result = service.CheckNow(cancellation);
        check(result.disposition == UpdateCheckServiceDisposition::Cancelled &&
                  result.error == DownloadErrorKind::Aborted &&
                  result.schedule.has_value() &&
                  checkTransport.requests.empty() && persistence.storePlans == 0,
              "pre-cancelled execution performs zero fetch and runtime I/O");
    }
    {
        NullDownloadTransport downloadTransport;
        FakePersistence persistence;
        UpdateRuntime runtime(downloadTransport, persistence, {});
        ScriptedCheckTransport checkTransport;
        checkTransport.responses.push_back(fetched(kManifest));
        checkTransport.responses.push_back(fetched(kSignature));
        UpdateCheckService service(checkTransport, runtime, config());
        MutableCancellation cancellation;
        checkTransport.onFetch = [&](std::size_t index) {
            if (index == 0) cancellation.cancelled = true;
        };

        const auto result = service.CheckNow(cancellation);
        check(result.disposition == UpdateCheckServiceDisposition::Cancelled &&
                  checkTransport.requests.size() == 1 &&
                  persistence.storePlans == 2 &&
                  runtime.snapshot().phase == UpdatePhase::Idle,
              "cancellation after manifest skips signature and closes runtime check");
    }
    {
        NullDownloadTransport downloadTransport;
        FakePersistence persistence;
        UpdateRuntime runtime(downloadTransport, persistence, {});
        ScriptedCheckTransport checkTransport;
        scriptValidManifest(checkTransport);
        UpdateCheckService service(checkTransport, runtime, config());
        MutableCancellation cancellation;
        UpdateCheckServiceDisposition nested =
            UpdateCheckServiceDisposition::NoOp;
        checkTransport.onFetch = [&](std::size_t index) {
            if (index == 0) nested = service.CheckNow(cancellation).disposition;
        };

        const auto result = service.CheckNow(cancellation);
        check(nested == UpdateCheckServiceDisposition::Joined &&
                  result.disposition ==
                      UpdateCheckServiceDisposition::UpdateAvailable &&
                  checkTransport.requests.size() == 2 &&
                  persistence.storePlans == 2,
              "overlapping manual request joins the one active generation");
    }
    {
        NullDownloadTransport downloadTransport;
        FakePersistence persistence;
        UpdateStoreRecord busyRecord;
        busyRecord.snapshot.phase = UpdatePhase::Downloading;
        busyRecord.snapshot.candidate = candidate("1.1.0");
        UpdateRuntime busyRuntime(downloadTransport, persistence, {}, busyRecord);
        ScriptedCheckTransport checkTransport;
        UpdateCheckService busyService(checkTransport, busyRuntime, config());
        MutableCancellation cancellation;
        const auto busy = busyService.CheckNow(cancellation);
        check(busy.disposition == UpdateCheckServiceDisposition::Busy &&
                  checkTransport.requests.empty() && persistence.storePlans == 0,
              "manual check reports Busy while download state owns the runtime");

        const auto scheduled = busyService.Start();
        const auto scheduledBusy = busyService.ExecuteScheduled(
            *scheduled.schedule, cancellation);
        check(scheduledBusy.disposition ==
                  UpdateCheckServiceDisposition::Busy &&
                  scheduledBusy.schedule.has_value() &&
                  scheduledBusy.schedule->kind ==
                      UpdateCheckScheduleKind::Periodic &&
                  scheduledBusy.schedule->delayMs == 30 * 60 * 1000 &&
                  checkTransport.requests.empty() && persistence.storePlans == 0,
              "scheduled check defers Busy runtime without fetch or state churn");

        FakePersistence readyPersistence;
        const auto oldPatch = candidate("1.1.0");
        UpdateRuntime readyRuntime(downloadTransport, readyPersistence, {},
                                   readyRecord(oldPatch));
        ScriptedCheckTransport readyTransport;
        UpdateCheckService readyService(readyTransport, readyRuntime, config());
        const auto ready = readyService.CheckNow(cancellation);
        check(ready.disposition == UpdateCheckServiceDisposition::Ready &&
                  ready.snapshot.readyPatch.has_value() &&
                  readyTransport.requests.empty() &&
                  readyPersistence.storePlans == 0,
              "manual check reports Ready when a patch already awaits restart");
    }
}

void test_ready_fallback_and_persistence_failure_remain_explicit() {
    const auto oldPatch = candidate("1.1.0");
    {
        NullDownloadTransport downloadTransport;
        FakePersistence persistence;
        UpdateRuntime runtime(downloadTransport, persistence, {},
                              readyRecord(oldPatch));
        ScriptedCheckTransport checkTransport;
        scriptValidManifest(checkTransport);
        UpdateCheckService service(checkTransport, runtime, config());
        MutableCancellation cancellation;

        const auto started = service.Start();
        const auto result = service.ExecuteScheduled(
            *started.schedule, cancellation);
        check(result.disposition ==
                  UpdateCheckServiceDisposition::UpdateAvailable &&
                  result.snapshot.phase == UpdatePhase::Ready &&
                  result.snapshot.readyPatch.has_value() &&
                  result.snapshot.readyPatch->version == oldPatch.version &&
                  result.candidateHandoff.has_value() &&
                  result.schedule.has_value(),
              "scheduled superseding success preserves ready fallback and returns handoff");
    }
    {
        NullDownloadTransport downloadTransport;
        FakePersistence persistence;
        UpdateRuntime runtime(downloadTransport, persistence, {},
                              readyRecord(oldPatch));
        ScriptedCheckTransport checkTransport;
        UpdateCheckFetchResult network;
        network.error = UpdateCheckFetchError::Network;
        network.detail = "offline";
        checkTransport.responses.push_back(network);
        UpdateCheckService service(checkTransport, runtime, config());
        MutableCancellation cancellation;

        const auto started = service.Start();
        const auto result = service.ExecuteScheduled(
            *started.schedule, cancellation);
        check(result.disposition == UpdateCheckServiceDisposition::Failed &&
                  result.error == DownloadErrorKind::Network &&
                  result.snapshot.phase == UpdatePhase::Ready &&
                  result.snapshot.readyPatch.has_value() &&
                  result.snapshot.readyPatch->version == oldPatch.version &&
                  result.snapshot.lastError == "offline" &&
                  result.schedule.has_value(),
              "nonblocking scheduled failure preserves ready fallback and rearms");
    }
    {
        NullDownloadTransport downloadTransport;
        FakePersistence persistence;
        persistence.failStoreAt = 0;
        UpdateRuntime runtime(downloadTransport, persistence, {});
        ScriptedCheckTransport checkTransport;
        scriptValidManifest(checkTransport);
        UpdateCheckService service(checkTransport, runtime, config());
        MutableCancellation cancellation;

        const auto result = service.CheckNow(cancellation);
        check(result.disposition ==
                  UpdateCheckServiceDisposition::TerminalError &&
                  result.error == DownloadErrorKind::Disk &&
                  !result.schedule && !result.candidateHandoff &&
                  result.snapshot.phase == UpdatePhase::Idle &&
                  checkTransport.requests.empty(),
              "begin-check persistence failure is Disk terminal before fetch");
    }
    {
        NullDownloadTransport downloadTransport;
        FakePersistence persistence;
        persistence.failStoreAt = 1;
        UpdateRuntime runtime(downloadTransport, persistence, {});
        ScriptedCheckTransport checkTransport;
        scriptValidManifest(checkTransport);
        UpdateCheckService service(checkTransport, runtime, config());
        MutableCancellation cancellation;

        const auto result = service.CheckNow(cancellation);
        check(result.disposition ==
                  UpdateCheckServiceDisposition::TerminalError &&
                  result.error == DownloadErrorKind::Disk &&
                  result.schedule.has_value() && !result.candidateHandoff &&
                  result.snapshot.phase == UpdatePhase::Checking,
              "completion persistence failure stays explicit and schedules retry");
    }
    {
        NullDownloadTransport downloadTransport;
        FakePersistence persistence;
        persistence.failStoreAt = 1;
        UpdateRuntime runtime(downloadTransport, persistence, {});
        ScriptedCheckTransport checkTransport;
        UpdateCheckFetchResult network;
        network.error = UpdateCheckFetchError::Network;
        network.detail = "offline";
        checkTransport.responses.push_back(network);
        UpdateCheckService service(checkTransport, runtime, config());
        MutableCancellation cancellation;

        const auto result = service.CheckNow(cancellation);
        check(result.disposition ==
                  UpdateCheckServiceDisposition::TerminalError &&
                  result.error == DownloadErrorKind::Disk &&
                  result.schedule.has_value() &&
                  result.snapshot.phase == UpdatePhase::Checking &&
                  result.snapshot.lastError.empty(),
              "failure-diagnostic persistence error overrides Network with Disk");
    }
}

void test_completion_persistence_retries_exact_outcome_without_refetch() {
    {
        NullDownloadTransport downloadTransport;
        FakePersistence persistence;
        persistence.failStoreAt = 1;
        UpdateRuntime runtime(downloadTransport, persistence, {});
        ScriptedCheckTransport checkTransport;
        scriptValidManifest(checkTransport);
        UpdateCheckService service(checkTransport, runtime, config());
        MutableCancellation cancellation;

        const auto failed = service.CheckNow(cancellation);
        const auto fetchesAfterFailure = checkTransport.requests.size();
        persistence.failStoreAt = -1;
        UpdateCheckServiceResult retried;
        if (failed.schedule) {
            retried = service.ExecuteScheduled(
                *failed.schedule, cancellation);
        }
        check(failed.disposition ==
                  UpdateCheckServiceDisposition::TerminalError &&
                  failed.error == DownloadErrorKind::Disk &&
                  failed.snapshot.phase == UpdatePhase::Checking &&
                  failed.schedule.has_value() &&
                  retried.disposition ==
                      UpdateCheckServiceDisposition::UpdateAvailable &&
                  retried.candidateHandoff.has_value() &&
                  retried.snapshot.phase == UpdatePhase::Available &&
                  retried.schedule.has_value() &&
                  checkTransport.requests.size() == fetchesAfterFailure &&
                  persistence.storePlans == 3,
              "candidate completion persistence retries without duplicate fetch");
    }
    {
        NullDownloadTransport downloadTransport;
        FakePersistence persistence;
        persistence.failStoreAt = 1;
        UpdateRuntime runtime(downloadTransport, persistence, {});
        ScriptedCheckTransport checkTransport;
        scriptValidManifest(checkTransport);
        auto trusted = config();
        trusted.policy.currentVersion = "1.2.0";
        UpdateCheckService service(checkTransport, runtime, trusted);
        MutableCancellation cancellation;

        const auto failed = service.CheckNow(cancellation);
        const auto fetchesAfterFailure = checkTransport.requests.size();
        persistence.failStoreAt = -1;
        const auto retried = service.CheckNow(cancellation);
        check(failed.error == DownloadErrorKind::Disk &&
                  failed.snapshot.phase == UpdatePhase::Checking &&
                  retried.disposition ==
                      UpdateCheckServiceDisposition::NoUpdate &&
                  retried.manifestError == ManifestError::NotNewer &&
                  retried.snapshot.phase == UpdatePhase::Idle &&
                  retried.schedule.has_value() &&
                  checkTransport.requests.size() == fetchesAfterFailure &&
                  persistence.storePlans == 3,
              "no-update persistence retries without duplicate fetch");
    }
    {
        NullDownloadTransport downloadTransport;
        FakePersistence persistence;
        persistence.failStoreAt = 1;
        UpdateRuntime runtime(downloadTransport, persistence, {});
        ScriptedCheckTransport checkTransport;
        UpdateCheckFetchResult network;
        network.error = UpdateCheckFetchError::Network;
        network.detail = "offline";
        checkTransport.responses.push_back(network);
        UpdateCheckService service(checkTransport, runtime, config());
        MutableCancellation cancellation;

        const auto failed = service.CheckNow(cancellation);
        const auto fetchesAfterFailure = checkTransport.requests.size();
        persistence.failStoreAt = -1;
        const auto retried = service.CheckNow(cancellation);
        check(failed.error == DownloadErrorKind::Disk &&
                  failed.snapshot.phase == UpdatePhase::Checking &&
                  retried.disposition == UpdateCheckServiceDisposition::Failed &&
                  retried.error == DownloadErrorKind::Network &&
                  retried.fetchError == UpdateCheckFetchError::Network &&
                  retried.detail == "offline" &&
                  retried.snapshot.phase == UpdatePhase::Error &&
                  retried.schedule.has_value() &&
                  checkTransport.requests.size() == fetchesAfterFailure &&
                  persistence.storePlans == 3,
              "failure persistence retries exact classification without refetch");
    }
    {
        NullDownloadTransport downloadTransport;
        FakePersistence persistence;
        persistence.failStoreAt = 1;
        UpdateRuntime runtime(downloadTransport, persistence, {});
        ScriptedCheckTransport checkTransport;
        scriptValidManifest(checkTransport);
        UpdateCheckService service(checkTransport, runtime, config());
        MutableCancellation cancellation;
        checkTransport.onFetch = [&](std::size_t index) {
            if (index == 0) cancellation.cancelled = true;
        };

        const auto failed = service.CheckNow(cancellation);
        const auto fetchesAfterFailure = checkTransport.requests.size();
        persistence.failStoreAt = -1;
        const auto retried = service.CheckNow(cancellation);
        check(failed.error == DownloadErrorKind::Disk &&
                  failed.snapshot.phase == UpdatePhase::Checking &&
                  retried.disposition ==
                      UpdateCheckServiceDisposition::Cancelled &&
                  retried.error == DownloadErrorKind::Aborted &&
                  retried.snapshot.phase == UpdatePhase::Idle &&
                  retried.schedule.has_value() &&
                  checkTransport.requests.size() == fetchesAfterFailure &&
                  persistence.storePlans == 3,
              "cancellation persistence retries without duplicate fetch");
    }
}

void test_scheduled_persistence_failure_emits_owned_completion_retry() {
    const CheckOutcomeCase outcomes[] = {
        CheckOutcomeCase::Candidate,
        CheckOutcomeCase::NoUpdate,
        CheckOutcomeCase::Failure,
        CheckOutcomeCase::Cancelled,
    };
    for (const auto outcome : outcomes) {
        NullDownloadTransport downloadTransport;
        FakePersistence persistence;
        persistence.failStoreAt = 1;
        UpdateRuntime runtime(downloadTransport, persistence, {});
        ScriptedCheckTransport checkTransport;
        auto trusted = config();
        MutableCancellation cancellation;
        scriptOutcome(outcome, checkTransport, trusted, cancellation);
        UpdateCheckService service(checkTransport, runtime, trusted);

        const auto started = service.Start();
        const auto failed = service.ExecuteScheduled(
            *started.schedule, cancellation);
        check(failed.disposition ==
                  UpdateCheckServiceDisposition::TerminalError &&
                  failed.error == DownloadErrorKind::Disk &&
                  failed.snapshot.phase == UpdatePhase::Checking &&
                  failed.schedule.has_value() &&
                  failed.schedule->kind ==
                      UpdateCheckScheduleKind::CompletionRetry &&
                  failed.schedule->delayMs == 30 * 60 * 1000 &&
                  failed.schedule->generation > started.schedule->generation &&
                  checkTransport.requests.size() == expectedFetches(outcome) &&
                  persistence.storePlans == 2,
              "scheduled Disk completion emits one owned retry intent");

        if (outcome == CheckOutcomeCase::Candidate && failed.schedule) {
            FakePersistence foreignPersistence;
            foreignPersistence.failStoreAt = 1;
            UpdateRuntime foreignRuntime(downloadTransport, foreignPersistence,
                                         {});
            ScriptedCheckTransport foreignTransport;
            scriptValidManifest(foreignTransport);
            UpdateCheckService foreignService(foreignTransport, foreignRuntime,
                                              config());
            MutableCancellation foreignCancellation;
            const auto foreignStarted = foreignService.Start();
            const auto foreignRetry = foreignService.ExecuteScheduled(
                *foreignStarted.schedule, foreignCancellation);
            const auto foreignFetches = foreignTransport.requests.size();
            const auto foreignStores = foreignPersistence.storePlans;
            const auto foreign = foreignService.ExecuteScheduled(
                *failed.schedule, foreignCancellation);
            check(foreignRetry.schedule.has_value() &&
                      foreignRetry.schedule->generation ==
                          failed.schedule->generation &&
                      foreignRetry.schedule->kind == failed.schedule->kind &&
                      foreign.disposition ==
                          UpdateCheckServiceDisposition::NoOp &&
                      foreignTransport.requests.size() == foreignFetches &&
                      foreignPersistence.storePlans == foreignStores,
                  "completion retry intent rejects cross-instance substitution");
        }

        const auto fetchesBeforeRetry = checkTransport.requests.size();
        persistence.failStoreAt = 2;
        UpdateCheckServiceResult secondFailure;
        if (failed.schedule) {
            secondFailure = service.ExecuteScheduled(
                *failed.schedule, cancellation);
        }
        check(secondFailure.disposition ==
                  UpdateCheckServiceDisposition::TerminalError &&
                  secondFailure.error == DownloadErrorKind::Disk &&
                  secondFailure.snapshot.phase == UpdatePhase::Checking &&
                  secondFailure.schedule.has_value() && failed.schedule &&
                  secondFailure.schedule->kind ==
                      UpdateCheckScheduleKind::CompletionRetry &&
                  secondFailure.schedule->generation >
                      failed.schedule->generation &&
                  checkTransport.requests.size() == fetchesBeforeRetry &&
                  persistence.storePlans == 3,
              "retry-on-retry Disk emits fresh intent without refetch");

        persistence.failStoreAt = -1;
        UpdateCheckServiceResult retried;
        if (secondFailure.schedule) {
            retried = service.ExecuteScheduled(
                *secondFailure.schedule, cancellation);
        }
        const auto expectedPhase =
            outcome == CheckOutcomeCase::Candidate
                ? UpdatePhase::Available
                : outcome == CheckOutcomeCase::Failure ? UpdatePhase::Error
                                                       : UpdatePhase::Idle;
        check(matchesOutcome(retried, outcome, expectedPhase) &&
                  retried.schedule.has_value() &&
                  retried.schedule->kind == UpdateCheckScheduleKind::Periodic &&
                  retried.schedule->delayMs == 30 * 60 * 1000 &&
                  secondFailure.schedule &&
                  retried.schedule->generation >
                      secondFailure.schedule->generation &&
                  checkTransport.requests.size() == fetchesBeforeRetry &&
                  persistence.storePlans == 4,
              "owned completion retry drains exact outcome before any refetch");
    }
}

void test_started_manual_failure_keeps_automatic_completion_retry() {
    NullDownloadTransport downloadTransport;
    FakePersistence persistence;
    persistence.failStoreAt = 1;
    UpdateRuntime runtime(downloadTransport, persistence, {});
    ScriptedCheckTransport checkTransport;
    scriptValidManifest(checkTransport);
    UpdateCheckService service(checkTransport, runtime, config());
    MutableCancellation cancellation;

    const auto started = service.Start();
    const auto failed = service.CheckNow(cancellation);
    const auto fetchesBeforeRetry = checkTransport.requests.size();
    check(failed.error == DownloadErrorKind::Disk &&
              failed.schedule.has_value() &&
              failed.schedule->kind ==
                  UpdateCheckScheduleKind::CompletionRetry &&
              failed.schedule->generation > started.schedule->generation,
          "manual check after Start replaces startup with completion retry intent");

    persistence.failStoreAt = -1;
    UpdateCheckServiceResult retried;
    if (failed.schedule) {
        retried = service.ExecuteScheduled(*failed.schedule, cancellation);
    }
    check(matchesOutcome(retried, CheckOutcomeCase::Candidate,
                         UpdatePhase::Available) &&
              retried.schedule.has_value() &&
              retried.schedule->kind == UpdateCheckScheduleKind::Periodic &&
              checkTransport.requests.size() == fetchesBeforeRetry &&
              persistence.storePlans == 3,
          "started manual Disk completion recovers through owned schedule");
}

void test_reconstructed_superseding_retry_preserves_ready_fallback() {
    const auto oldPatch = candidate("1.1.0");
    auto interrupted = readyRecord(oldPatch);
    interrupted.snapshot.phase = UpdatePhase::Superseding;
    const auto originalPath = interrupted.stagedRelativePath;
    const CheckOutcomeCase outcomes[] = {
        CheckOutcomeCase::Candidate,
        CheckOutcomeCase::NoUpdate,
        CheckOutcomeCase::Failure,
        CheckOutcomeCase::Cancelled,
    };
    for (const auto outcome : outcomes) {
        NullDownloadTransport downloadTransport;
        FakePersistence persistence;
        persistence.failStoreAt = 1;
        UpdateRuntime runtime(downloadTransport, persistence, {}, interrupted);
        ScriptedCheckTransport checkTransport;
        auto trusted = config();
        MutableCancellation cancellation;
        scriptOutcome(outcome, checkTransport, trusted, cancellation);
        UpdateCheckService service(checkTransport, runtime, trusted);

        const auto started = service.Start();
        const auto failed = service.ExecuteScheduled(
            *started.schedule, cancellation);
        const auto fetchesBeforeRetry = checkTransport.requests.size();
        check(failed.error == DownloadErrorKind::Disk &&
                  failed.snapshot.phase == UpdatePhase::Superseding &&
                  failed.snapshot.readyPatch.has_value() &&
                  sameCandidate(*failed.snapshot.readyPatch, oldPatch) &&
                  failed.snapshot.downloadedBytes == oldPatch.assetSize &&
                  runtime.record().stagedRelativePath == originalPath &&
                  failed.schedule.has_value() &&
                  failed.schedule->kind ==
                      UpdateCheckScheduleKind::CompletionRetry &&
                  persistence.storePlans == 2,
              "reconstructed Superseding retains ready fallback on Disk");

        persistence.failStoreAt = -1;
        UpdateCheckServiceResult retried;
        if (failed.schedule) {
            retried = service.ExecuteScheduled(*failed.schedule, cancellation);
        }
        check(matchesOutcome(retried, outcome, UpdatePhase::Ready) &&
                  retried.snapshot.readyPatch.has_value() &&
                  sameCandidate(*retried.snapshot.readyPatch, oldPatch) &&
                  retried.snapshot.downloadedBytes == oldPatch.assetSize &&
                  runtime.record().stagedRelativePath == originalPath &&
                  retried.schedule.has_value() &&
                  checkTransport.requests.size() == fetchesBeforeRetry &&
                  persistence.storePlans == 3,
              "Superseding completion retry preserves exact ready fallback");
    }
}

void test_reconstructed_service_recovers_persisted_active_check() {
    {
        NullDownloadTransport downloadTransport;
        FakePersistence firstPersistence;
        firstPersistence.failStoreAt = 1;
        UpdateRuntime firstRuntime(downloadTransport, firstPersistence, {});
        ScriptedCheckTransport firstTransport;
        scriptValidManifest(firstTransport);
        UpdateCheckService firstService(firstTransport, firstRuntime, config());
        MutableCancellation cancellation;
        const auto failed = firstService.CheckNow(cancellation);
        const auto persistedChecking = firstRuntime.record();

        FakePersistence recoveredPersistence;
        UpdateRuntime recoveredRuntime(downloadTransport, recoveredPersistence,
                                       {}, persistedChecking);
        ScriptedCheckTransport recoveredTransport;
        scriptValidManifest(recoveredTransport);
        UpdateCheckService recoveredService(recoveredTransport,
                                            recoveredRuntime, config());
        const auto recovered = recoveredService.CheckNow(cancellation);
        check(failed.error == DownloadErrorKind::Disk &&
                  persistedChecking.snapshot.phase == UpdatePhase::Checking &&
                  recovered.disposition ==
                      UpdateCheckServiceDisposition::UpdateAvailable &&
                  recovered.snapshot.phase == UpdatePhase::Available &&
                  recoveredTransport.requests.size() == 2 &&
                  recoveredPersistence.storePlans == 2,
              "reconstructed service re-admits and recovers candidate check");
    }
    {
        NullDownloadTransport downloadTransport;
        FakePersistence firstPersistence;
        firstPersistence.failStoreAt = 1;
        UpdateRuntime firstRuntime(downloadTransport, firstPersistence, {});
        ScriptedCheckTransport firstTransport;
        scriptValidManifest(firstTransport);
        auto trusted = config();
        trusted.policy.currentVersion = "1.2.0";
        UpdateCheckService firstService(firstTransport, firstRuntime, trusted);
        MutableCancellation cancellation;
        const auto failed = firstService.CheckNow(cancellation);
        const auto persistedChecking = firstRuntime.record();

        FakePersistence recoveredPersistence;
        UpdateRuntime recoveredRuntime(downloadTransport, recoveredPersistence,
                                       {}, persistedChecking);
        ScriptedCheckTransport recoveredTransport;
        scriptValidManifest(recoveredTransport);
        UpdateCheckService recoveredService(recoveredTransport,
                                            recoveredRuntime, trusted);
        const auto recovered = recoveredService.CheckNow(cancellation);
        check(failed.error == DownloadErrorKind::Disk &&
                  recovered.disposition ==
                      UpdateCheckServiceDisposition::NoUpdate &&
                  recovered.snapshot.phase == UpdatePhase::Idle &&
                  recoveredTransport.requests.size() == 2 &&
                  recoveredPersistence.storePlans == 2,
              "reconstructed service re-admits and recovers no-update check");
    }
    {
        NullDownloadTransport downloadTransport;
        FakePersistence firstPersistence;
        firstPersistence.failStoreAt = 1;
        UpdateRuntime firstRuntime(downloadTransport, firstPersistence, {});
        ScriptedCheckTransport firstTransport;
        UpdateCheckFetchResult network;
        network.error = UpdateCheckFetchError::Network;
        network.detail = "offline";
        firstTransport.responses.push_back(network);
        UpdateCheckService firstService(firstTransport, firstRuntime, config());
        MutableCancellation cancellation;
        const auto failed = firstService.CheckNow(cancellation);
        const auto persistedChecking = firstRuntime.record();

        FakePersistence recoveredPersistence;
        UpdateRuntime recoveredRuntime(downloadTransport, recoveredPersistence,
                                       {}, persistedChecking);
        ScriptedCheckTransport recoveredTransport;
        recoveredTransport.responses.push_back(network);
        UpdateCheckService recoveredService(recoveredTransport,
                                            recoveredRuntime, config());
        const auto recovered = recoveredService.CheckNow(cancellation);
        check(failed.error == DownloadErrorKind::Disk &&
                  recovered.disposition == UpdateCheckServiceDisposition::Failed &&
                  recovered.error == DownloadErrorKind::Network &&
                  recovered.snapshot.phase == UpdatePhase::Error &&
                  recoveredTransport.requests.size() == 1 &&
                  recoveredPersistence.storePlans == 2,
              "reconstructed service re-admits and recovers failed check");
    }
}

void test_schedule_capability_and_stale_completion_are_instance_bound() {
    {
        NullDownloadTransport downloadTransport;
        FakePersistence persistenceA;
        FakePersistence persistenceB;
        UpdateRuntime runtimeA(downloadTransport, persistenceA, {});
        UpdateRuntime runtimeB(downloadTransport, persistenceB, {});
        ScriptedCheckTransport transportA;
        ScriptedCheckTransport transportB;
        scriptValidManifest(transportB);
        UpdateCheckService serviceA(transportA, runtimeA, config());
        UpdateCheckService serviceB(transportB, runtimeB, config());
        MutableCancellation cancellation;
        const auto intentA = serviceA.Start();
        const auto intentB = serviceB.Start();

        const auto foreign = serviceB.ExecuteScheduled(
            *intentA.schedule, cancellation);
        const auto owned = serviceB.ExecuteScheduled(
            *intentB.schedule, cancellation);
        check(foreign.disposition == UpdateCheckServiceDisposition::NoOp &&
                  transportB.requests.size() == 2 &&
                  persistenceB.storePlans == 2 &&
                  owned.disposition ==
                      UpdateCheckServiceDisposition::UpdateAvailable,
              "schedule intent is bound to its originating service instance");
    }
    {
        NullDownloadTransport downloadTransport;
        FakePersistence persistence;
        persistence.failStoreAt = 1;
        UpdateRuntime runtime(downloadTransport, persistence, {});
        ScriptedCheckTransport firstTransport;
        scriptValidManifest(firstTransport);
        UpdateCheckService firstService(firstTransport, runtime, config());
        MutableCancellation cancellation;
        const auto failed = firstService.CheckNow(cancellation);

        persistence.failStoreAt = -1;
        ScriptedCheckTransport replacementTransport;
        scriptValidManifest(replacementTransport);
        auto noUpdateConfig = config();
        noUpdateConfig.policy.currentVersion = "1.2.0";
        UpdateCheckService replacementService(replacementTransport, runtime,
                                              noUpdateConfig);
        const auto replacement = replacementService.CheckNow(cancellation);
        const auto storesBeforeStale = persistence.storePlans;
        const auto stale = firstService.CheckNow(cancellation);
        check(failed.error == DownloadErrorKind::Disk &&
                  replacement.disposition ==
                      UpdateCheckServiceDisposition::NoUpdate &&
                  replacement.snapshot.phase == UpdatePhase::Idle &&
                  stale.disposition == UpdateCheckServiceDisposition::NoOp &&
                  stale.snapshot.phase == UpdatePhase::Idle &&
                  persistence.storePlans == storesBeforeStale &&
                  firstTransport.requests.size() == 2,
              "superseded pending completion cannot publish through stale token");
    }
}

void test_active_schedule_checks_capability_before_numeric_generation() {
    NullDownloadTransport downloadTransport;
    FakePersistence persistenceA;
    FakePersistence persistenceB;
    UpdateRuntime runtimeA(downloadTransport, persistenceA, {});
    UpdateRuntime runtimeB(downloadTransport, persistenceB, {});
    ScriptedCheckTransport transportA;
    ScriptedCheckTransport transportB;
    scriptValidManifest(transportA);
    UpdateCheckService serviceA(transportA, runtimeA, config());
    UpdateCheckService serviceB(transportB, runtimeB, config());
    MutableCancellation cancellation;
    const auto startedA = serviceA.Start();
    const auto startedB = serviceB.Start();
    UpdateCheckServiceDisposition foreignDisposition =
        UpdateCheckServiceDisposition::Busy;
    UpdateCheckServiceDisposition ownedDisposition =
        UpdateCheckServiceDisposition::NoOp;
    bool nestedCallsWereInert = false;
    transportA.onFetch = [&](std::size_t index) {
        if (index != 0) return;
        const auto fetchesBefore = transportA.requests.size();
        const auto storesBefore = persistenceA.storePlans;
        const auto phaseBefore = runtimeA.snapshot().phase;
        foreignDisposition = serviceA.ExecuteScheduled(
            *startedB.schedule, cancellation).disposition;
        ownedDisposition = serviceA.ExecuteScheduled(
            *startedA.schedule, cancellation).disposition;
        nestedCallsWereInert =
            transportA.requests.size() == fetchesBefore &&
            persistenceA.storePlans == storesBefore &&
            runtimeA.snapshot().phase == phaseBefore;
    };

    const auto completed = serviceA.ExecuteScheduled(
        *startedA.schedule, cancellation);
    check(foreignDisposition == UpdateCheckServiceDisposition::NoOp &&
              ownedDisposition == UpdateCheckServiceDisposition::Joined &&
              nestedCallsWereInert &&
              completed.disposition ==
                  UpdateCheckServiceDisposition::UpdateAvailable &&
              transportA.requests.size() == 2 &&
              persistenceA.storePlans == 2 &&
              transportB.requests.empty() && persistenceB.storePlans == 0,
          "active schedule validates capability before numeric generation join");
}

}  // namespace

int main() {
    test_start_emits_one_delayed_intent_without_io();
    test_scheduled_valid_newer_manifest_returns_owned_handoff_and_periodic_intent();
    test_scheduled_valid_newer_manifest_returns_owned_handoff_and_periodic_intent(true);
    test_manual_no_update_runs_immediately_and_supersedes_startup_intent();
    test_runtime_owns_check_through_fetch_and_final_cancellation();
    test_response_body_caps_and_lowercase_signature_are_enforced();
    test_transport_and_manifest_failures_are_classified_and_rearmed();
    test_trusted_config_controls_key_repository_and_fetch_contract();
    test_cancellation_overlap_and_runtime_admission_are_deterministic();
    test_ready_fallback_and_persistence_failure_remain_explicit();
    test_completion_persistence_retries_exact_outcome_without_refetch();
    test_scheduled_persistence_failure_emits_owned_completion_retry();
    test_started_manual_failure_keeps_automatic_completion_retry();
    test_reconstructed_service_recovers_persisted_active_check();
    test_reconstructed_superseding_retry_preserves_ready_fallback();
    test_schedule_capability_and_stale_completion_are_instance_bound();
    test_active_schedule_checks_capability_before_numeric_generation();
    return failures == 0 ? 0 : 1;
}
