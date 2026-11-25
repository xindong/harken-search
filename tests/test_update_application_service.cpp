#include "update/UpdateApplicationService.h"
#include "update/HarkenSearchUpdateComposition.h"

#include <algorithm>
#include <cstdio>
#include <functional>
#include <optional>
#include <string>
#include <type_traits>
#include <vector>

namespace {

using namespace aris::update;

static_assert(static_cast<int>(UpdateApplicationScheduleKind::Check) == 0);
static_assert(static_cast<int>(UpdateApplicationScheduleKind::DownloadRetry) == 1);
static_assert(static_cast<int>(UpdateApplicationScheduleKind::CandidateDownload) == 2);
static_assert(static_cast<int>(UpdateApplicationScheduleKind::ResumeDownload) == 3);
static_assert(!std::is_default_constructible_v<
              UpdateBootstrapLaunchCapability>);

int failures = 0;

void check(bool value, const char* message) {
    if (value) return;
    std::fprintf(stderr, "FAIL: %s\n", message);
    ++failures;
}

class NeverCheckCancelled final : public UpdateCheckCancellation {
public:
    bool IsCancellationRequested() const override { return false; }
    std::unique_ptr<InterruptRegistration> RegisterInterrupt(
        std::function<void()>) const override {
        class Registration final : public InterruptRegistration {};
        return std::make_unique<Registration>();
    }
};

class PreCheckCancelled final : public UpdateCheckCancellation {
public:
    bool IsCancellationRequested() const override { return true; }
    std::unique_ptr<InterruptRegistration> RegisterInterrupt(
        std::function<void()>) const override {
        class Registration final : public InterruptRegistration {};
        return std::make_unique<Registration>();
    }
};

class NeverDownloadCancelled final : public HttpTransportCancellation {
public:
    bool IsCancellationRequested() const override { return false; }
    std::unique_ptr<InterruptRegistration> RegisterInterrupt(
        std::function<void()>) const override {
        class Registration final : public InterruptRegistration {};
        return std::make_unique<Registration>();
    }
};

class PreDownloadCancelled final : public HttpTransportCancellation {
public:
    bool IsCancellationRequested() const override { return true; }
    std::unique_ptr<InterruptRegistration> RegisterInterrupt(
        std::function<void()>) const override {
        class Registration final : public InterruptRegistration {};
        return std::make_unique<Registration>();
    }
};

class FakeClock final : public UpdateApplicationClockPort {
public:
    std::uint64_t NowMonotonicMs() const override { return now; }
    std::uint64_t now = 1'000;
};

class FakeCapacity final : public UpdateStagingCapacityPort {
public:
    UpdateStagingCapacityResult QueryAvailableBytes() override {
        ++queries;
        return result;
    }
    UpdateStagingCapacityResult result{true, 1'024 * 1'024, {}};
    int queries = 0;
};

FakeCapacity& capacity() {
    static FakeCapacity value;
    value.result = {true, 1'024 * 1'024, {}};
    return value;
}

class ScriptedCheckTransport final : public UpdateCheckTransportPort {
public:
    UpdateCheckFetchResult Fetch(
        const UpdateCheckFetchRequest& request,
        const UpdateCheckCancellation&) override {
        requests.push_back(request);
        if (onFetch) onFetch(requests.size() - 1);
        if (requests.size() <= responses.size()) {
            return responses[requests.size() - 1];
        }
        UpdateCheckFetchResult result;
        result.error = UpdateCheckFetchError::Network;
        result.detail = "unscripted fetch";
        return result;
    }

    std::vector<UpdateCheckFetchRequest> requests;
    std::vector<UpdateCheckFetchResult> responses;
    std::function<void(std::size_t)> onFetch;
};

class FakeDownloadTransport final : public HttpTransportPort {
public:
    HttpTransportExecutionResult Execute(
        const HttpTransferRequestPlan& plan,
        const HttpTransportCancellation&,
        HttpTransportSink& sink) override {
        const auto executionIndex = executions++;
        plans.push_back(plan);
        if (executionIndex < errors.size() &&
            errors[executionIndex] != HttpTransportExecutionError::None) {
            return {errors[executionIndex]};
        }
        HttpResponseHead head;
        head.statusCode = 200;
        head.contentLength = plan.expectedSize;
        if (!sink.OnResponseHead(head)) return {HttpTransportExecutionError::SinkRejected};
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(plan.expectedSize), 7);
        if (!sink.OnChunk(bytes) || !sink.OnEof()) {
            return {HttpTransportExecutionError::SinkRejected};
        }
        HttpTransportExecutionResult result;
        result.streamedBytes = bytes.size();
        return result;
    }

    int executions = 0;
    std::vector<HttpTransferRequestPlan> plans;
    std::vector<HttpTransportExecutionError> errors;
};

class FakePersistence final : public UpdatePersistencePort {
public:
    UpdatePersistenceResult ExecuteStoreIntents(
        const std::vector<StoreIntent>& intents) override {
        ++storePlans;
        UpdatePersistenceResult result;
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
        ++activations;
        return {};
    }
    UpdatePersistenceResult DeactivateTransfer(
        const DownloadFlightToken&,
        const HttpTransferIdentity&) override {
        return {};
    }
    UpdatePersistenceResult ExecuteTransferChunk(
        const TransferAppendIntent& append,
        const ResumeMetadataIntent&) override {
        stagedSize += append.bytes.size();
        return {};
    }
    ResumeMetadataObservation ObserveResumeMetadata(
        const std::string&) override {
        ResumeMetadataObservation result;
        result.status = ResumeMetadataStatus::Missing;
        return result;
    }
    StagedIdentityObservation ObserveStagedPatch(
        const std::string& path) override {
        StagedIdentityObservation result;
        if (stagedSize == 0) {
            result.status = StagedIdentityStatus::Missing;
            return result;
        }
        result.status = StagedIdentityStatus::Present;
        result.identity.exists = true;
        result.identity.relativePath = path;
        result.identity.size = stagedSize;
        result.identity.sha256 = std::string(64, 'a');
        return result;
    }

    int storePlans = 0;
    int activations = 0;
    std::uint64_t stagedSize = 0;
    int failStoreAt = -1;
};

class RecordingBootstrapPort final : public UpdateBootstrapAuthorityPort {
public:
    struct Authority final {};

    UpdateBootstrapAuthorizationResult Authorize(
        const VerifiedUpdateArchive& archive) override {
        ++authorizations;
        if (expected &&
            (expected->version != archive.version ||
             expected->buildId != archive.buildId ||
             expected->assetSha256 != archive.assetSha256 ||
             expected->assetSize != archive.assetSize ||
             expected->stagedRelativePath != archive.stagedRelativePath)) {
            return {UpdateBootstrapAuthorizationStatus::BindingMismatch,
                    std::nullopt, "package binding mismatch"};
        }
        if (rejectAuthorization) {
            return {UpdateBootstrapAuthorizationStatus::PackageRejected,
                    std::nullopt, "package binding rejected"};
        }
        auto marker = std::make_shared<Authority>();
        authorities.push_back(marker);
        return {UpdateBootstrapAuthorizationStatus::Authorized,
                IssueCapability(marker), {}};
    }

    UpdateBootstrapSubmitResult Submit(
        const VerifiedUpdateArchive& archive,
        const UpdateBootstrapLaunchCapability& capability) override {
        const auto authority = UpdateBootstrapAuthorityPort::Authority(capability);
        const bool owned = std::find(authorities.begin(), authorities.end(),
                                     authority) != authorities.end();
        if (!owned) {
            return {UpdateBootstrapSubmitStatus::TerminalFailure,
                    "foreign Bootstrap capability"};
        }
        requests.push_back(archive);
        if (next < scripted.size()) return scripted[next++];
        return {UpdateBootstrapSubmitStatus::Accepted, {}};
    }
    std::vector<VerifiedUpdateArchive> requests;
    std::vector<UpdateBootstrapSubmitResult> scripted;
    std::vector<std::shared_ptr<Authority>> authorities;
    int authorizations = 0;
    bool rejectAuthorization = false;
    std::optional<VerifiedUpdateArchive> expected;
    std::size_t next = 0;
};

UpdateCheckFetchResult fetched(const std::string& text) {
    UpdateCheckFetchResult result;
    result.body.assign(text.begin(), text.end());
    return result;
}

const std::string kManifest =
    R"({"schema_version":1,"product":"aris","channel":"testing","version":"1.2.0","platform":"windows","architecture":"x64","minimum_bootstrap_version":"1.0.0","minimum_direct_version":"1.0.0","data_compatibility":"backward_compatible","asset":{"url":"https://github.com/xindong/aris/releases/download/v1.2.0/Aris-1.2.0-win-x64-update.zip","size":42,"sha256":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"},"published_at":"2026-07-28T00:00:00Z","build_id":"abc123"})";
const std::string kSignature =
    "df358ea2f5dc1c212ee302fd9242466b419d0cb8a8bcd5e9c7fa16edd68ccbc6"
    "f2bf563e107ad05792ffa58bc6e9eb6160b42044c982dc83b9b26fbf44d5f40a";
const std::string kSchema2Manifest =
    R"({"schema_version":2,"product":"aris","channel":"testing","version":"1.2.0","platform":"windows","architecture":"x64","minimum_bootstrap_version":"1.0.0","minimum_direct_version":"1.0.0","data_compatibility":"backward_compatible","asset":{"url":"https://github.com/xindong/aris/releases/download/v1.2.0/Aris-1.2.0-win-x64-update.zip","size":42,"sha256":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"},"published_at":"2026-07-28T00:00:00Z","build_id":"abc123"})";
const std::string kSchema2Signature =
    "73a3f743f1dc60c58f3850808835bb5886144fd242b8e32554e1a14543bb46fc"
    "95843c19d4ac850f04ca72e6cd10e900ea7f5bae3c4e4afe113fc9f402833b08";

UpdateApplicationServiceConfig config() {
    UpdateApplicationServiceConfig value;
    value.check.manifestUrl = "https://github.com/xindong/aris/releases/download/testing/update-manifest.json";
    value.check.signatureUrl = "https://github.com/xindong/aris/releases/download/testing/update-manifest.json.sig";
    value.check.publicKeyHex =
        "03a107bff3ce10be1d70dd18e74bc09967e4d6309ba50d5f1ddc8664125531b8";
    value.check.policy.currentVersion = "1.1.0";
    value.check.policy.selectedChannel = UpdateChannel::Testing;
    value.check.policy.platform = "windows";
    value.check.policy.architecture = "x64";
    value.check.policy.allowedRepository = "xindong/aris";
    value.check.manifestTimeoutMs = 1'700;
    value.check.signatureTimeoutMs = 900;
    value.check.manifestBodyLimit = 4'096;
    value.check.signatureBodyLimit = 128;
    value.runtime.channel = UpdateChannel::Testing;
    value.runtime.retryPolicy.maxAttempts = 3;
    value.runtime.retryPolicy.baseDelayMs = 1'000;
    value.runtime.retryPolicy.maxDelayMs = 4'000;
    value.downloadRelativeTarget = "Aris-update.zip";
    return value;
}

UpdateCandidate candidate(const std::string& version, char hash = 'a') {
    UpdateCandidate value;
    value.version = version;
    value.buildId = "build-" + version;
    value.assetUrl = "https://github.com/xindong/aris/releases/download/v" +
                     version + "/Aris-" + version + "-win-x64-update.zip";
    value.assetSize = 42;
    value.sha256 = std::string(64, hash);
    return value;
}

UpdateStoreRecord readyRecord(const UpdateCandidate& patch,
                              bool restartConfirmed = false) {
    UpdateStoreRecord record;
    record.channel = UpdateChannel::Testing;
    record.snapshot.phase = UpdatePhase::Ready;
    record.snapshot.readyPatch = patch;
    record.snapshot.downloadedBytes = patch.assetSize;
    record.snapshot.restartConfirmed = restartConfirmed;
    record.stagedRelativePath = patch.version + "/Aris-update.zip";
    return record;
}

void test_process_start_retires_persisted_error_but_keeps_new_failure() {
    ScriptedCheckTransport checkTransport;
    UpdateCheckFetchResult currentFailure;
    currentFailure.error = UpdateCheckFetchError::Network;
    currentFailure.detail = "current process network failure";
    checkTransport.responses = {currentFailure};
    FakeDownloadTransport downloadTransport;
    FakePersistence persistence;
    FakeClock clock;
    RecordingBootstrapPort bootstrap;

    UpdateStoreRecord previousProcess;
    previousProcess.channel = UpdateChannel::Testing;
    previousProcess.snapshot.phase = UpdatePhase::Error;
    previousProcess.snapshot.lastError =
        "update check redirect is not approved";

    UpdateApplicationService service(
        checkTransport, downloadTransport, persistence, capacity(), clock,
        bootstrap, config(), previousProcess);
    NeverCheckCancelled checkCancellation;
    NeverDownloadCancelled downloadCancellation;

    const auto started = service.Start();
    check(started.snapshot.phase == UpdatePhase::Idle &&
              started.snapshot.lastError.empty(),
          "process start retires a persisted Error without a ready patch");

    const auto failed = service.ExecuteScheduled(
        started.schedules[0], checkCancellation, downloadCancellation);
    check(failed.snapshot.phase == UpdatePhase::Error &&
              failed.snapshot.lastError == currentFailure.detail,
          "a new Error produced by the current process remains visible");
}

void test_startup_check_download_and_bootstrap_handoff_are_one_flow() {
    ScriptedCheckTransport checkTransport;
    checkTransport.responses = {fetched(kManifest), fetched(kSignature)};
    FakeDownloadTransport downloadTransport;
    FakePersistence persistence;
    FakeClock clock;
    RecordingBootstrapPort bootstrap;
    UpdateApplicationService service(checkTransport, downloadTransport,
                                     persistence, capacity(), clock, bootstrap, config());
    NeverCheckCancelled checkCancellation;
    NeverDownloadCancelled downloadCancellation;

    const auto started = service.Start();
    check(started.disposition == UpdateApplicationDisposition::Scheduled &&
              started.schedules.size() == 1 &&
              started.schedules[0].kind() == UpdateApplicationScheduleKind::Check &&
              started.schedules[0].dueAtMonotonicMs() == 11'000,
          "start exposes one deterministic 10 second check intent with no I/O");
    check(checkTransport.requests.empty() && downloadTransport.executions == 0,
          "start performs no check or download I/O");

    const auto ready = service.ExecuteScheduled(
        started.schedules[0], checkCancellation, downloadCancellation);
    if (ready.snapshot.phase != UpdatePhase::Ready) {
        std::fprintf(stderr,
                     "startup flow stopped in phase=%d snapshot=%s result=%s\n",
                     static_cast<int>(ready.snapshot.phase),
                     ready.snapshot.lastError.c_str(), ready.detail.c_str());
    }
    check(ready.disposition == UpdateApplicationDisposition::Ready &&
              ready.snapshot.phase == UpdatePhase::Ready,
          "scheduled signed newer manifest downloads directly to Ready");
    check(checkTransport.requests.size() == 2 && downloadTransport.executions == 1 &&
              downloadTransport.plans.size() == 1 &&
              downloadTransport.plans[0].url.find("Aris-1.2.0-win-x64-update.zip") != std::string::npos,
          "composition fetches manifest/signature once and downloads exact verified asset once");
    check(service.record().stagedRelativePath == "1.2.0/Aris-update.zip",
          "Ready record owns the deterministic staged ZIP identity");

    const auto launched = service.ConfirmRestart();
    check(launched.disposition == UpdateApplicationDisposition::BootstrapAccepted &&
              bootstrap.requests.size() == 1,
          "restart confirmation emits one authenticated Bootstrap handoff");
    if (bootstrap.requests.empty()) return;
    check(bootstrap.requests[0].channel == UpdateChannel::Testing &&
              bootstrap.requests[0].version == "1.2.0" &&
              bootstrap.requests[0].buildId == "abc123" &&
              bootstrap.requests[0].assetSha256 == std::string(64, 'a') &&
              bootstrap.requests[0].assetSize == 42 &&
              bootstrap.requests[0].stagedRelativePath == "1.2.0/Aris-update.zip" &&
              bootstrap.authorizations == 1,
          "Bootstrap authority receives only the verified archive binding");

    const int persisted = persistence.storePlans;
    const auto replay = service.ConfirmRestart();
    check(replay.disposition == UpdateApplicationDisposition::NoOp &&
              bootstrap.requests.size() == 1 && persistence.storePlans == persisted,
          "restart handoff is consume-once with zero replay mutation");
}

void test_pre_download_cancel_retains_verified_candidate_without_refetch() {
    ScriptedCheckTransport checkTransport;
    checkTransport.responses = {fetched(kManifest), fetched(kSignature)};
    FakeDownloadTransport downloadTransport;
    FakePersistence persistence;
    FakeClock clock;
    RecordingBootstrapPort bootstrap;
    UpdateApplicationService service(checkTransport, downloadTransport,
                                     persistence, capacity(), clock, bootstrap, config());
    NeverCheckCancelled checkCancellation;
    PreDownloadCancelled preCancelled;
    NeverDownloadCancelled downloadCancellation;

    const auto started = service.Start();
    const auto cancelled = service.ExecuteScheduled(
        started.schedules[0], checkCancellation, preCancelled);
    check(cancelled.disposition == UpdateApplicationDisposition::Cancelled &&
              cancelled.schedules.size() == 2,
          "pre-download cancellation keeps periodic and candidate retry intents");
    std::optional<UpdateApplicationScheduleIntent> candidateRetry;
    for (const auto& schedule : cancelled.schedules) {
        if (schedule.kind() ==
            UpdateApplicationScheduleKind::CandidateDownload) {
            candidateRetry = schedule;
        }
    }
    check(candidateRetry.has_value() && downloadTransport.executions == 0 &&
              checkTransport.requests.size() == 2,
          "pre-download cancellation performs no transport and retains checked handoff");

    if (candidateRetry) {
        const auto ready = service.ExecuteScheduled(
            *candidateRetry, checkCancellation, downloadCancellation);
        check(ready.disposition == UpdateApplicationDisposition::Ready &&
                  downloadTransport.executions == 1 &&
                  checkTransport.requests.size() == 2,
              "candidate retry reaches Ready without refetching manifest or signature");
    }
}

void test_no_update_and_check_failures_never_start_downloads() {
    {
        auto trusted = config();
        trusted.check.policy.currentVersion = "1.2.0";
        ScriptedCheckTransport checkTransport;
        checkTransport.responses = {fetched(kManifest), fetched(kSignature)};
        FakeDownloadTransport downloadTransport;
        FakePersistence persistence;
        FakeClock clock;
        RecordingBootstrapPort bootstrap;
        UpdateApplicationService service(checkTransport, downloadTransport,
                                         persistence, capacity(), clock, bootstrap,
                                         std::move(trusted));
        NeverCheckCancelled checkCancellation;
        NeverDownloadCancelled downloadCancellation;
        const auto result = service.CheckNow(checkCancellation,
                                             downloadCancellation);
        check(result.disposition == UpdateApplicationDisposition::NoUpdate &&
                  result.manifestError == ManifestError::NotNewer &&
                  result.snapshot.phase == UpdatePhase::Idle &&
                  result.schedules.size() == 1 &&
                  downloadTransport.executions == 0,
              "same-version manifest completes NoUpdate and rearms periodic check");
    }

    struct FailureCase {
        UpdateCheckFetchError fetch;
        DownloadErrorKind expected;
    };
    const FailureCase cases[] = {
        {UpdateCheckFetchError::Network, DownloadErrorKind::Network},
        {UpdateCheckFetchError::Http4xx, DownloadErrorKind::Http4xx},
        {UpdateCheckFetchError::Http5xx, DownloadErrorKind::Http5xx},
    };
    for (const auto& failure : cases) {
        ScriptedCheckTransport checkTransport;
        UpdateCheckFetchResult response;
        response.error = failure.fetch;
        response.detail = "scripted check failure";
        checkTransport.responses = {response};
        FakeDownloadTransport downloadTransport;
        FakePersistence persistence;
        FakeClock clock;
        RecordingBootstrapPort bootstrap;
        UpdateApplicationService service(checkTransport, downloadTransport,
                                         persistence, capacity(), clock, bootstrap, config());
        NeverCheckCancelled checkCancellation;
        NeverDownloadCancelled downloadCancellation;
        const auto result = service.CheckNow(checkCancellation,
                                             downloadCancellation);
        check(result.disposition == UpdateApplicationDisposition::Failed &&
                  result.error == failure.expected &&
                  result.fetchError == failure.fetch &&
                  checkTransport.requests.size() == 1 &&
                  downloadTransport.executions == 0,
              "network and HTTP check classifications survive black-box composition");
    }
}

void test_manual_supersedes_startup_and_periodic_rearms_without_stale_work() {
    auto trusted = config();
    trusted.check.policy.currentVersion = "1.2.0";
    ScriptedCheckTransport checkTransport;
    checkTransport.responses = {fetched(kManifest), fetched(kSignature),
                                fetched(kManifest), fetched(kSignature)};
    FakeDownloadTransport downloadTransport;
    FakePersistence persistence;
    FakeClock clock;
    RecordingBootstrapPort bootstrap;
    UpdateApplicationService service(
        checkTransport, downloadTransport, persistence, capacity(), clock,
        bootstrap, std::move(trusted));
    NeverCheckCancelled checkCancellation;
    NeverDownloadCancelled downloadCancellation;

    const auto started = service.Start();
    const auto manual = service.CheckNow(checkCancellation,
                                         downloadCancellation);
    check(manual.disposition == UpdateApplicationDisposition::NoUpdate &&
              manual.schedules.size() == 1 &&
              checkTransport.requests.size() == 2,
          "manual check immediately supersedes the pending startup intent");
    const int stores = persistence.storePlans;
    const auto staleStartup = service.ExecuteScheduled(
        started.schedules[0], checkCancellation, downloadCancellation);
    check(staleStartup.disposition == UpdateApplicationDisposition::NoOp &&
              checkTransport.requests.size() == 2 &&
              persistence.storePlans == stores,
          "superseded startup schedule is a zero-mutation NoOp");

    clock.now += 30 * 60 * 1'000;
    const auto periodic = service.ExecuteScheduled(
        manual.schedules[0], checkCancellation, downloadCancellation);
    check(periodic.disposition == UpdateApplicationDisposition::NoUpdate &&
              periodic.schedules.size() == 1 &&
              checkTransport.requests.size() == 4 &&
              downloadTransport.executions == 0,
          "owned periodic schedule executes once and rearms the next interval");
}

void test_https_check_failures_only_use_update_center_and_allow_manual_recovery() {
    using CheckOrigin = HarkenSearchUpdateCheckOrigin;
    using UiPhase = HarkenSearchUpdateUiPhase;
    auto trusted = config();
    trusted.check.policy.currentVersion = "1.2.0";
    UpdateCheckFetchResult offline;
    offline.error = UpdateCheckFetchError::Network;
    offline.detail = "update check HTTPS request failed";
    ScriptedCheckTransport checkTransport;
    checkTransport.responses = {offline, offline, fetched(kManifest), fetched(kSignature)};
    FakeDownloadTransport downloadTransport;
    FakePersistence persistence;
    FakeClock clock;
    RecordingBootstrapPort bootstrap;
    UpdateApplicationService service(
        checkTransport, downloadTransport, persistence, capacity(), clock,
        bootstrap, std::move(trusted));
    NeverCheckCancelled checkCancellation;
    NeverDownloadCancelled downloadCancellation;

    const auto started = service.Start();
    check(started.schedules.size() == 1, "startup has one scheduled check");
    if (started.schedules.empty()) return;
    clock.now = started.schedules.front().dueAtMonotonicMs();
    const auto automatic =
        service.ExecuteScheduled(started.schedules.front(), checkCancellation, downloadCancellation);
    check(automatic.disposition == UpdateApplicationDisposition::Failed &&
              automatic.fetchError == UpdateCheckFetchError::Network &&
              checkTransport.requests.size() == 1,
          "one startup manifest request returns an HTTPS failure");

    HarkenSearchUpdateUiState ui;
    const auto verifyFailure = [&](const UpdateApplicationResult& failed, CheckOrigin origin) {
        ui.phase = UiPhase::Checking;
        ui.checkInFlight = true;
        ui.candidateVersion = "1.1.0";
        ui.downloadedBytes = 12;
        ui.totalBytes = 34;
        ApplyHarkenSearchUpdateSnapshot(ui, failed.snapshot, origin);
        check(ui.phase == UiPhase::Error && ui.failedCheckOrigin == origin &&
                  ui.lastError == offline.detail && !ui.checkInFlight,
              "check failure retains the attempt origin and error for the update center");
        check(ui.SuppressesDownloadNotice(),
              "polling a check failure cannot show a download notification");
        check(ui.downloadedBytes == 0 && ui.totalBytes == 0 && ui.candidateVersion.empty(),
              "check failure retires cached download progress");
        const auto requestCount = checkTransport.requests.size();
        const auto replayed = service.ExecuteScheduled(
            started.schedules.front(), checkCancellation, downloadCancellation);
        check(replayed.disposition == UpdateApplicationDisposition::NoOp &&
                  replayed.snapshot.lastError == failed.snapshot.lastError &&
                  checkTransport.requests.size() == requestCount,
              "a stale startup schedule republishes the same failure without another HTTPS request");
        ApplyHarkenSearchUpdateSnapshot(ui, replayed.snapshot);
        check(ui.SuppressesDownloadNotice() && ui.failedCheckOrigin == origin &&
                  ui.downloadedBytes == 0 && ui.totalBytes == 0,
              "republishing the same failed snapshot cannot resurrect the download notification");
    };
    verifyFailure(automatic, CheckOrigin::Automatic);

    const auto manual = service.CheckNow(checkCancellation, downloadCancellation);
    check(manual.disposition == UpdateApplicationDisposition::Failed &&
              manual.fetchError == UpdateCheckFetchError::Network &&
              checkTransport.requests.size() == 2,
          "manual check is accepted after startup failure and reports its own request failure");
    verifyFailure(manual, CheckOrigin::Manual);

    ui.BeginCheckPresentation();
    ui.checkInFlight = true;
    check(ui.phase == UiPhase::Checking && ui.SuppressesDownloadNotice() &&
              ui.failedCheckOrigin == CheckOrigin::None && ui.lastError.empty(),
          "manual retry clears the previous failure without showing download progress");
    const auto recovered = service.CheckNow(checkCancellation, downloadCancellation);
    ApplyHarkenSearchUpdateSnapshot(ui, recovered.snapshot);
    check(recovered.disposition == UpdateApplicationDisposition::NoUpdate &&
              ui.phase == UiPhase::Idle && ui.failedCheckOrigin == CheckOrigin::None &&
              ui.lastError.empty() && !ui.checkInFlight && checkTransport.requests.size() == 4,
          "manual retry after network recovery uses the signed manifest and clears failure");
    check(downloadTransport.executions == 0 && persistence.activations == 0 &&
              bootstrap.authorizations == 0,
          "check failures and recovery with no newer release never start a download or install");
}

void test_a_new_download_failure_is_visible_even_with_the_previous_check_error_text() {
    using CheckOrigin = HarkenSearchUpdateCheckOrigin;
    auto trusted = config();
    trusted.runtime.retryPolicy.maxAttempts = 1;
    UpdateCheckFetchResult offline;
    offline.error = UpdateCheckFetchError::Network;
    offline.detail = "download network transport failed (WinInet/system error 0)";
    ScriptedCheckTransport checkTransport;
    checkTransport.responses = {offline, fetched(kManifest), fetched(kSignature)};
    FakeDownloadTransport downloadTransport;
    downloadTransport.errors = {HttpTransportExecutionError::Network};
    FakePersistence persistence;
    FakeClock clock;
    RecordingBootstrapPort bootstrap;
    UpdateApplicationService service(
        checkTransport, downloadTransport, persistence, capacity(), clock,
        bootstrap, std::move(trusted));
    NeverCheckCancelled checkCancellation;
    NeverDownloadCancelled downloadCancellation;

    HarkenSearchUpdateUiState ui;
    const auto failedCheck = service.CheckNow(checkCancellation, downloadCancellation);
    ApplyHarkenSearchUpdateOperationResult(ui, failedCheck, CheckOrigin::Manual);
    check(failedCheck.disposition == UpdateApplicationDisposition::Failed &&
              ui.SuppressesDownloadNotice(),
          "a check failure is not a download failure regardless of the detail text");

    const auto failedDownload = service.CheckNow(checkCancellation, downloadCancellation);
    check(failedDownload.disposition == UpdateApplicationDisposition::TerminalError &&
              failedDownload.snapshot.lastError == ui.lastError &&
              downloadTransport.executions == 1 && bootstrap.authorizations == 0,
          "a later check finds an update and the download really fails with the same detail");
    ApplyHarkenSearchUpdateOperationResult(ui, failedDownload, CheckOrigin::Manual);
    check(ui.phase == HarkenSearchUpdateUiPhase::Error &&
              ui.failedCheckOrigin == CheckOrigin::None && !ui.SuppressesDownloadNotice(),
          "a new download failure is not suppressed as a replay of the check failure");
}

void test_check_retry_persistence_failure_stays_out_of_download_notice() {
    using CheckOrigin = HarkenSearchUpdateCheckOrigin;
    UpdateCheckFetchResult offline;
    offline.error = UpdateCheckFetchError::Network;
    offline.detail = "update check HTTPS request failed";
    ScriptedCheckTransport checkTransport;
    checkTransport.responses = {offline};
    FakeDownloadTransport downloadTransport;
    FakePersistence persistence;
    FakeClock clock;
    RecordingBootstrapPort bootstrap;
    UpdateApplicationService service(
        checkTransport, downloadTransport, persistence, capacity(), clock,
        bootstrap, config());
    NeverCheckCancelled checkCancellation;
    NeverDownloadCancelled downloadCancellation;

    HarkenSearchUpdateUiState ui;
    const auto failedCheck = service.CheckNow(checkCancellation, downloadCancellation);
    ApplyHarkenSearchUpdateOperationResult(ui, failedCheck, CheckOrigin::Manual);
    persistence.failStoreAt = persistence.storePlans;
    ui.BeginCheckPresentation();
    const auto failedRetry = service.CheckNow(checkCancellation, downloadCancellation);
    check(failedRetry.disposition == UpdateApplicationDisposition::TerminalError &&
              failedRetry.error == DownloadErrorKind::Disk &&
              failedRetry.snapshot.lastError == failedCheck.detail &&
              checkTransport.requests.size() == 1 && downloadTransport.executions == 0,
          "retry admission fails before transport and keeps the previous persisted snapshot");
    ApplyHarkenSearchUpdateOperationResult(ui, failedRetry, CheckOrigin::Manual);
    check(ui.phase == HarkenSearchUpdateUiPhase::Error && ui.SuppressesDownloadNotice() &&
              ui.failedCheckOrigin == CheckOrigin::Manual && ui.lastError == failedRetry.detail,
          "a rejected check retry shows the current disk error without a download notice");
}

void test_signature_schema_caps_disk_and_check_cancel_fail_closed() {
    NeverCheckCancelled checkCancellation;
    NeverDownloadCancelled downloadCancellation;

    {
        ScriptedCheckTransport checkTransport;
        std::string tampered = kManifest;
        tampered.replace(tampered.find("1.2.0"), 5, "9.9.9");
        checkTransport.responses = {fetched(tampered), fetched(kSignature)};
        FakeDownloadTransport downloadTransport;
        FakePersistence persistence;
        FakeClock clock;
        RecordingBootstrapPort bootstrap;
        UpdateApplicationService service(checkTransport, downloadTransport,
                                         persistence, capacity(), clock, bootstrap, config());
        const auto result = service.CheckNow(checkCancellation,
                                             downloadCancellation);
        check(result.disposition == UpdateApplicationDisposition::Failed &&
                  result.manifestError == ManifestError::SignatureInvalid &&
                  downloadTransport.executions == 0,
              "tampered manifest fails signature before download");
    }

    {
        ScriptedCheckTransport checkTransport;
        checkTransport.responses = {fetched(kSchema2Manifest),
                                    fetched(kSchema2Signature)};
        FakeDownloadTransport downloadTransport;
        FakePersistence persistence;
        FakeClock clock;
        RecordingBootstrapPort bootstrap;
        UpdateApplicationService service(checkTransport, downloadTransport,
                                         persistence, capacity(), clock, bootstrap, config());
        const auto result = service.CheckNow(checkCancellation,
                                             downloadCancellation);
        check(result.disposition == UpdateApplicationDisposition::Failed &&
                  result.manifestError == ManifestError::SchemaUnsupported &&
                  downloadTransport.executions == 0,
              "validly signed unsupported schema fails closed before download");
    }

    {
        auto trusted = config();
        trusted.check.manifestBodyLimit = kManifest.size() - 1;
        ScriptedCheckTransport checkTransport;
        checkTransport.responses = {fetched(kManifest)};
        FakeDownloadTransport downloadTransport;
        FakePersistence persistence;
        FakeClock clock;
        RecordingBootstrapPort bootstrap;
        UpdateApplicationService service(checkTransport, downloadTransport,
                                         persistence, capacity(), clock, bootstrap,
                                         std::move(trusted));
        const auto result = service.CheckNow(checkCancellation,
                                             downloadCancellation);
        check(result.disposition == UpdateApplicationDisposition::Failed &&
                  result.fetchError == UpdateCheckFetchError::BodyTooLarge &&
                  checkTransport.requests.size() == 1 &&
                  downloadTransport.executions == 0,
              "manifest body cap blocks signature fetch and download");
    }

    {
        ScriptedCheckTransport checkTransport;
        checkTransport.responses = {fetched(kManifest), fetched(kSignature)};
        FakeDownloadTransport downloadTransport;
        FakePersistence persistence;
        persistence.failStoreAt = 0;
        FakeClock clock;
        RecordingBootstrapPort bootstrap;
        UpdateApplicationService service(checkTransport, downloadTransport,
                                         persistence, capacity(), clock, bootstrap, config());
        const auto result = service.CheckNow(checkCancellation,
                                             downloadCancellation);
        check(result.disposition == UpdateApplicationDisposition::TerminalError &&
                  result.error == DownloadErrorKind::Disk &&
                  checkTransport.requests.empty() &&
                  downloadTransport.executions == 0,
              "check admission persistence failure stays Disk and performs zero I/O");
    }

    {
        ScriptedCheckTransport checkTransport;
        FakeDownloadTransport downloadTransport;
        FakePersistence persistence;
        FakeClock clock;
        RecordingBootstrapPort bootstrap;
        UpdateApplicationService service(checkTransport, downloadTransport,
                                         persistence, capacity(), clock, bootstrap, config());
        PreCheckCancelled cancelled;
        const auto result = service.CheckNow(cancelled, downloadCancellation);
        check(result.disposition == UpdateApplicationDisposition::Cancelled &&
                  checkTransport.requests.empty() &&
                  downloadTransport.executions == 0,
              "pre-cancelled manual check performs zero fetch and download I/O");
    }
}

void test_download_retry_stale_and_cross_service_intents_are_capability_bound() {
    ScriptedCheckTransport checkTransport;
    checkTransport.responses = {fetched(kManifest), fetched(kSignature)};
    FakeDownloadTransport downloadTransport;
    downloadTransport.errors = {HttpTransportExecutionError::Network,
                                HttpTransportExecutionError::None};
    FakePersistence persistence;
    FakeClock clock;
    RecordingBootstrapPort bootstrap;
    UpdateApplicationService service(checkTransport, downloadTransport,
                                     persistence, capacity(), clock, bootstrap, config());
    NeverCheckCancelled checkCancellation;
    NeverDownloadCancelled downloadCancellation;

    const auto started = service.Start();
    const auto retrying = service.ExecuteScheduled(
        started.schedules[0], checkCancellation, downloadCancellation);
    std::optional<UpdateApplicationScheduleIntent> retry;
    for (const auto& schedule : retrying.schedules) {
        if (schedule.kind() == UpdateApplicationScheduleKind::DownloadRetry) {
            retry = schedule;
        }
    }
    check(retrying.disposition == UpdateApplicationDisposition::RetryScheduled &&
              retry.has_value() && checkTransport.requests.size() == 2 &&
              downloadTransport.executions == 1,
          "download network failure emits deterministic owned retry alongside periodic check");
    HarkenSearchUpdateUiState ui;
    ApplyHarkenSearchUpdateSnapshot(ui, retrying.snapshot);
    check(ui.phase == HarkenSearchUpdateUiPhase::Downloading &&
              !ui.SuppressesDownloadNotice(),
          "a real download retry still presents download progress");

    ScriptedCheckTransport foreignCheck;
    FakeDownloadTransport foreignDownload;
    FakePersistence foreignPersistence;
    FakeClock foreignClock;
    RecordingBootstrapPort foreignBootstrap;
    UpdateApplicationService foreign(foreignCheck, foreignDownload,
                                     foreignPersistence, capacity(), foreignClock,
                                     foreignBootstrap, config());
    if (retry) {
        const int foreignStores = foreignPersistence.storePlans;
        const auto rejected = foreign.ExecuteScheduled(
            *retry, checkCancellation, downloadCancellation);
        check(rejected.disposition == UpdateApplicationDisposition::NoOp &&
                  foreignPersistence.storePlans == foreignStores &&
                  foreignDownload.executions == 0 &&
                  foreignCheck.requests.empty(),
              "cross-service schedule substitution is a zero-mutation NoOp");

        const auto ready = service.ExecuteScheduled(
            *retry, checkCancellation, downloadCancellation);
        check(ready.disposition == UpdateApplicationDisposition::Ready &&
                  downloadTransport.executions == 2 &&
                  checkTransport.requests.size() == 2,
              "owned download retry reaches Ready without duplicate check fetch");
        const int stores = persistence.storePlans;
        const auto stale = service.ExecuteScheduled(
            *retry, checkCancellation, downloadCancellation);
        check(stale.disposition == UpdateApplicationDisposition::NoOp &&
                  persistence.storePlans == stores &&
                  downloadTransport.executions == 2,
              "consumed retry replay is a zero-mutation NoOp");
    }
}

void test_superseding_preserves_ready_fallback_and_bootstrap_retry_is_exact() {
    const auto oldPatch = candidate("1.1.0", 'b');
    ScriptedCheckTransport checkTransport;
    checkTransport.responses = {fetched(kManifest), fetched(kSignature)};
    FakeDownloadTransport downloadTransport;
    downloadTransport.errors = {HttpTransportExecutionError::Network};
    FakePersistence persistence;
    FakeClock clock;
    RecordingBootstrapPort bootstrap;
    UpdateApplicationService service(checkTransport, downloadTransport,
                                     persistence, capacity(), clock, bootstrap, config(),
                                     readyRecord(oldPatch));
    NeverCheckCancelled checkCancellation;
    NeverDownloadCancelled downloadCancellation;
    const auto started = service.Start();
    const auto retrying = service.ExecuteScheduled(
        started.schedules[0], checkCancellation, downloadCancellation);
    check(retrying.disposition == UpdateApplicationDisposition::RetryScheduled &&
              retrying.snapshot.readyPatch.has_value() &&
              retrying.snapshot.readyPatch->version == "1.1.0" &&
              service.record().stagedRelativePath == "1.1.0/Aris-update.zip",
          "superseding download keeps the old authenticated Ready fallback and path");

    ScriptedCheckTransport readyCheck;
    FakeDownloadTransport readyDownload;
    FakePersistence readyPersistence;
    FakeClock readyClock;
    RecordingBootstrapPort retryingBootstrap;
    retryingBootstrap.scripted = {
        {UpdateBootstrapSubmitStatus::RetryableFailure, "launcher busy"},
        {UpdateBootstrapSubmitStatus::Accepted, {}}};
    UpdateApplicationService readyService(
        readyCheck, readyDownload, readyPersistence, capacity(), readyClock,
        retryingBootstrap, config(), readyRecord(candidate("1.2.0")));
    const auto first = readyService.ConfirmRestart();
    const int stores = readyPersistence.storePlans;
    const auto second = readyService.ConfirmRestart();
    check(first.disposition == UpdateApplicationDisposition::BootstrapRetryable &&
              second.disposition == UpdateApplicationDisposition::BootstrapAccepted &&
              retryingBootstrap.requests.size() == 2 &&
              readyPersistence.storePlans == stores &&
              retryingBootstrap.requests[0].stagedRelativePath ==
                  retryingBootstrap.requests[1].stagedRelativePath &&
              retryingBootstrap.requests[0].assetSha256 ==
                  retryingBootstrap.requests[1].assetSha256,
          "Bootstrap retry reuses the exact capability-bound archive without churn");
}

void test_bootstrap_reconstruction_is_idempotent_and_invalid_config_does_not_mutate() {
    NeverCheckCancelled checkCancellation;
    NeverDownloadCancelled downloadCancellation;
    (void)checkCancellation;
    (void)downloadCancellation;

    ScriptedCheckTransport checkTransport;
    FakeDownloadTransport downloadTransport;
    FakePersistence persistence;
    FakeClock clock;
    RecordingBootstrapPort bootstrap;
    bootstrap.scripted = {
        {UpdateBootstrapSubmitStatus::AlreadyAccepted, {}}};
    UpdateApplicationService reconstructed(
        checkTransport, downloadTransport, persistence, capacity(), clock, bootstrap,
        config(), readyRecord(candidate("1.2.0"), true));
    const auto recovered = reconstructed.ConfirmRestart();
    check(recovered.disposition ==
              UpdateApplicationDisposition::BootstrapAccepted &&
              bootstrap.requests.size() == 1 &&
              persistence.storePlans == 0,
          "reconstructed confirmed invocation is idempotently re-admitted without state churn");

    auto invalid = config();
    ScriptedCheckTransport invalidCheck;
    FakeDownloadTransport invalidDownload;
    FakePersistence invalidPersistence;
    FakeClock invalidClock;
    RecordingBootstrapPort invalidBootstrap;
    UpdateApplicationService invalidService(
        invalidCheck, invalidDownload, invalidPersistence, capacity(), invalidClock,
        invalidBootstrap, std::move(invalid),
        readyRecord(candidate("1.2.0")));
    invalidBootstrap.rejectAuthorization = true;
    const auto rejected = invalidService.ConfirmRestart();
    check(rejected.disposition == UpdateApplicationDisposition::TerminalError &&
              rejected.error == DownloadErrorKind::InvalidArgument &&
              !rejected.snapshot.restartConfirmed &&
              invalidPersistence.storePlans == 0 &&
              invalidBootstrap.requests.empty(),
          "Bootstrap package binding rejection fails before persistence or launch");

    auto persistedChannelMismatch = readyRecord(candidate("1.2.0"));
    persistedChannelMismatch.channel = UpdateChannel::Stable;
    ScriptedCheckTransport mismatchCheck;
    FakeDownloadTransport mismatchDownload;
    FakePersistence mismatchPersistence;
    FakeClock mismatchClock;
    RecordingBootstrapPort mismatchBootstrap;
    UpdateApplicationService mismatchService(
        mismatchCheck, mismatchDownload, mismatchPersistence, capacity(),
        mismatchClock, mismatchBootstrap, config(), persistedChannelMismatch);
    const auto mismatched = mismatchService.ConfirmRestart();
    check(mismatched.disposition == UpdateApplicationDisposition::TerminalError &&
              mismatched.error == DownloadErrorKind::InvalidArgument &&
              mismatchBootstrap.authorizations == 0 &&
              mismatchBootstrap.requests.empty() &&
              mismatchPersistence.storePlans == 0,
          "persisted Ready channel mismatch fails closed before Bootstrap authorization");

    RecordingBootstrapPort ownerA;
    RecordingBootstrapPort ownerB;
    VerifiedUpdateArchive archive;
    archive.version = "1.2.0";
    archive.buildId = "build-1.2.0";
    archive.assetSha256 = std::string(64, 'a');
    archive.assetSize = 42;
    archive.stagedRelativePath = "1.2.0/Aris-update.zip";
    const auto issued = ownerA.Authorize(archive);
    check(issued.capability.has_value(),
          "Bootstrap owner issues an opaque launch capability");
    if (issued.capability) {
        const auto foreign = ownerB.Submit(archive, *issued.capability);
        check(foreign.status == UpdateBootstrapSubmitStatus::TerminalFailure &&
                  ownerB.requests.empty(),
              "foreign Bootstrap capability cannot launch an archive");
    }
}

void test_reconstructed_interrupted_download_resumes_from_owned_start_intent() {
    ScriptedCheckTransport checkTransport;
    FakeDownloadTransport downloadTransport;
    FakePersistence persistence;
    FakeClock clock;
    RecordingBootstrapPort bootstrap;
    UpdateStoreRecord interrupted;
    interrupted.channel = UpdateChannel::Testing;
    interrupted.snapshot.phase = UpdatePhase::Downloading;
    interrupted.snapshot.candidate = candidate("1.2.0");
    UpdateApplicationService service(
        checkTransport, downloadTransport, persistence, capacity(), clock, bootstrap,
        config(), interrupted);
    NeverCheckCancelled checkCancellation;
    NeverDownloadCancelled downloadCancellation;

    const auto started = service.Start();
    std::optional<UpdateApplicationScheduleIntent> resume;
    for (const auto& schedule : started.schedules) {
        if (schedule.kind() == UpdateApplicationScheduleKind::ResumeDownload) {
            resume = schedule;
        }
    }
    check(started.schedules.size() == 2 && resume.has_value() &&
              resume->dueAtMonotonicMs() == clock.now &&
              checkTransport.requests.empty() && downloadTransport.executions == 0,
          "reconstructed Downloading state exposes an immediate resume intent with no Start I/O");
    if (resume) {
        const auto ready = service.ExecuteScheduled(
            *resume, checkCancellation, downloadCancellation);
        check(ready.disposition == UpdateApplicationDisposition::Ready &&
                  ready.snapshot.phase == UpdatePhase::Ready &&
                  downloadTransport.executions == 1 &&
                  checkTransport.requests.empty(),
              "owned resume intent recovers persisted download without duplicate check fetch");
    }
}

void test_capacity_failure_retains_checked_candidate_without_duplicate_fetch() {
    ScriptedCheckTransport checkTransport;
    checkTransport.responses = {fetched(kManifest), fetched(kSignature)};
    FakeDownloadTransport downloadTransport;
    FakePersistence persistence;
    FakeClock clock;
    RecordingBootstrapPort bootstrap;
    auto& stagingCapacity = capacity();
    UpdateApplicationService service(
        checkTransport, downloadTransport, persistence, stagingCapacity, clock,
        bootstrap, config());
    stagingCapacity.result = {false, 0, "disk probe failed"};
    NeverCheckCancelled checkCancellation;
    NeverDownloadCancelled downloadCancellation;

    const auto started = service.Start();
    const auto deferred = service.ExecuteScheduled(
        started.schedules[0], checkCancellation, downloadCancellation);
    std::optional<UpdateApplicationScheduleIntent> candidateRetry;
    for (const auto& schedule : deferred.schedules) {
        if (schedule.kind() ==
            UpdateApplicationScheduleKind::CandidateDownload) {
            candidateRetry = schedule;
        }
    }
    check(deferred.disposition == UpdateApplicationDisposition::RetryScheduled &&
              deferred.error == DownloadErrorKind::Disk &&
              candidateRetry.has_value() && downloadTransport.executions == 0 &&
              checkTransport.requests.size() == 2,
          "capacity probe failure retains checked candidate and performs no download");
    stagingCapacity.result = {true, 1'024 * 1'024, {}};
    if (candidateRetry) {
        const auto ready = service.ExecuteScheduled(
            *candidateRetry, checkCancellation, downloadCancellation);
        check(ready.disposition == UpdateApplicationDisposition::Ready &&
                  downloadTransport.executions == 1 &&
                  checkTransport.requests.size() == 2,
              "capacity recovery downloads retained candidate without refetch");
    }
}

void test_active_schedule_remains_owned_for_reentrant_join() {
    auto trusted = config();
    trusted.check.policy.currentVersion = "1.2.0";
    ScriptedCheckTransport checkTransport;
    checkTransport.responses = {fetched(kManifest), fetched(kSignature)};
    FakeDownloadTransport downloadTransport;
    FakePersistence persistence;
    FakeClock clock;
    RecordingBootstrapPort bootstrap;
    UpdateApplicationService service(
        checkTransport, downloadTransport, persistence, capacity(), clock,
        bootstrap, std::move(trusted));

    ScriptedCheckTransport foreignCheck;
    FakeDownloadTransport foreignDownload;
    FakePersistence foreignPersistence;
    FakeClock foreignClock;
    RecordingBootstrapPort foreignBootstrap;
    UpdateApplicationService foreign(
        foreignCheck, foreignDownload, foreignPersistence, capacity(),
        foreignClock, foreignBootstrap, config());
    NeverCheckCancelled checkCancellation;
    NeverDownloadCancelled downloadCancellation;
    const auto started = service.Start();
    const auto foreignStarted = foreign.Start();
    UpdateApplicationDisposition sameService =
        UpdateApplicationDisposition::TerminalError;
    UpdateApplicationDisposition substituted =
        UpdateApplicationDisposition::TerminalError;
    checkTransport.onFetch = [&](std::size_t index) {
        if (index != 0) return;
        sameService = service.ExecuteScheduled(
            started.schedules[0], checkCancellation, downloadCancellation)
                          .disposition;
        substituted = service.ExecuteScheduled(
            foreignStarted.schedules[0], checkCancellation,
            downloadCancellation)
                          .disposition;
    };

    const auto completed = service.ExecuteScheduled(
        started.schedules[0], checkCancellation, downloadCancellation);
    check(completed.disposition == UpdateApplicationDisposition::NoUpdate &&
              sameService == UpdateApplicationDisposition::Joined &&
              substituted == UpdateApplicationDisposition::NoOp &&
              checkTransport.requests.size() == 2 &&
              foreignCheck.requests.empty(),
          "active owned schedule joins reentrantly while foreign substitution is NoOp");
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 2 &&
        std::string(argv[1]) == "--persisted-error-lifecycle") {
        test_process_start_retires_persisted_error_but_keeps_new_failure();
        return failures == 0 ? 0 : 1;
    }
    test_process_start_retires_persisted_error_but_keeps_new_failure();
    test_startup_check_download_and_bootstrap_handoff_are_one_flow();
    test_pre_download_cancel_retains_verified_candidate_without_refetch();
    test_no_update_and_check_failures_never_start_downloads();
    test_manual_supersedes_startup_and_periodic_rearms_without_stale_work();
    test_https_check_failures_only_use_update_center_and_allow_manual_recovery();
    test_a_new_download_failure_is_visible_even_with_the_previous_check_error_text();
    test_check_retry_persistence_failure_stays_out_of_download_notice();
    test_signature_schema_caps_disk_and_check_cancel_fail_closed();
    test_download_retry_stale_and_cross_service_intents_are_capability_bound();
    test_superseding_preserves_ready_fallback_and_bootstrap_retry_is_exact();
    test_bootstrap_reconstruction_is_idempotent_and_invalid_config_does_not_mutate();
    test_reconstructed_interrupted_download_resumes_from_owned_start_intent();
    test_capacity_failure_retains_checked_candidate_without_duplicate_fetch();
    test_active_schedule_remains_owned_for_reentrant_join();
    if (failures != 0) {
        std::fprintf(stderr, "%d update application service test(s) failed\n", failures);
        return 1;
    }
    std::puts("update application service tests passed");
    return 0;
}
