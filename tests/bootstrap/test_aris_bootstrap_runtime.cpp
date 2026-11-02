#include "bootstrap/HarkenSearchBootstrapRuntime.h"

#include <algorithm>
#include <iostream>
#include <string>
#include <vector>

namespace {

using namespace aris::bootstrap;
int failures = 0;

void check(bool condition, const char* message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

BootstrapProcessIdentity identity(std::uint64_t pid) {
    BootstrapProcessIdentity value;
    value.valid = true;
    value.processId = pid;
    value.creationTime = pid + 1000U;
    value.imagePath = "C:/HarkenSearchTest/Aris/HarkenSearch.exe";
    value.imageVolumeSerial = 7;
    value.imageFileId.fill(static_cast<std::uint8_t>(pid));
    return value;
}

BootstrapExecutableExpectation previousExecutableExpectation() {
    BootstrapExecutableExpectation value;
    value.absolutePath = "C:/HarkenSearchTest/Aris/HarkenSearch.exe";
    value.sha256 = std::string(64U, 'b');
    value.byteSize = 900U;
    value.exactFileIdentity.valid = true;
    value.exactFileIdentity.volumeSerial = 7U;
    value.exactFileIdentity.fileId.fill(41U);
    return value;
}

struct ProcessFake final : BootstrapProcessPort {
    BootstrapProcessStatus openStatus = BootstrapProcessStatus::Ok;
    std::uint32_t openNativeError = 0U;
    bool timeout = false;
    bool waitCalled = false;
    bool launchFailure = false;
    bool terminated = false;
    bool suspendedLaunch = false;
    bool resumed = false;
    std::vector<std::uint64_t> resumedProcessIds;
    bool released = false;
    bool preparedForCommit = false;
    BootstrapProcessStatus prepareForCommitStatus = BootstrapProcessStatus::Ok;
    bool childSubstitution = false;
    bool rebindCalled = false;
    BootstrapProcessStatus rebindStatus = BootstrapProcessStatus::Ok;
    std::vector<std::string>* events = nullptr;
    bool commitLaunchFailure = false;
    std::vector<std::uint64_t> launched;
    std::vector<std::string> launchedExecutables;
    std::vector<std::string> launchDirectories;
    std::vector<std::uint64_t> validatedExecutableProcessIds;
    std::vector<BootstrapExecutableExpectation> validatedExecutables;

    BootstrapProcessResult OpenExpected(const BootstrapProcessIdentity& expected) override {
        if (openStatus != BootstrapProcessStatus::Ok) {
            return {openStatus, openNativeError, {},
                    openStatus == BootstrapProcessStatus::NotFound
                        ? previousExecutableExpectation()
                        : BootstrapExecutableExpectation{}};
        }
        return {BootstrapProcessStatus::Ok, 0, {expected, 1},
                previousExecutableExpectation()};
    }
    BootstrapProcessResult WaitForExit(BootstrapOwnedProcess&, std::uint32_t) override {
        waitCalled = true;
        if (timeout) return {BootstrapProcessStatus::TimedOut};
        return {BootstrapProcessStatus::Ok};
    }
    BootstrapProcessResult Launch(std::string_view executable,
                                  std::string_view directory,
                                   const BootstrapLaunchCapability&) override {
        if (launchFailure) return {BootstrapProcessStatus::LaunchFailed};
        auto child = identity(99U + launched.size());
        if (childSubstitution && launched.empty()) {
        child.imagePath = "C:/HarkenSearchTest/Aris/Unexpected.exe";
        }
        launched.push_back(child.processId);
        launchedExecutables.emplace_back(executable);
        launchDirectories.emplace_back(directory);
        return {BootstrapProcessStatus::Ok, 0, {child, 2}};
    }
    BootstrapProcessResult RebindOwnedChild(
        const BootstrapProcessIdentity& expected) override {
        rebindCalled = true;
        if (events != nullptr) events->push_back("rebind-child");
        if (rebindStatus != BootstrapProcessStatus::Ok) {
            return {rebindStatus};
        }
        return {BootstrapProcessStatus::Ok, 0, {expected, 3}};
    }
    BootstrapProcessResult LaunchSuspended(
        std::string_view executable, std::string_view directory,
        const BootstrapLaunchCapability& capability) override {
        suspendedLaunch = true;
        return Launch(executable, directory, capability);
    }
    BootstrapProcessResult ValidateOwnedExecutable(
        const BootstrapOwnedProcess& process,
        const BootstrapExecutableExpectation& expected) override {
        if (!process.valid() || !expected.valid()) {
            return {BootstrapProcessStatus::InvalidIdentity};
        }
        validatedExecutableProcessIds.push_back(process.identity.processId);
        validatedExecutables.push_back(expected);
        return {BootstrapProcessStatus::Ok};
    }
    BootstrapProcessResult ResumeOwned(BootstrapOwnedProcess& process) override {
        resumed = true;
        resumedProcessIds.push_back(process.identity.processId);
        return {BootstrapProcessStatus::Ok};
    }
    BootstrapProcessResult PrepareOwnedForCommit(
        BootstrapOwnedProcess& process) override {
        if (process.valid() && prepareForCommitStatus == BootstrapProcessStatus::Ok) {
            preparedForCommit = true;
            if (events != nullptr) events->push_back("prepare-child-commit");
        }
        return {prepareForCommitStatus};
    }
    BootstrapProcessResult TerminateOwned(BootstrapOwnedProcess& process,
                                          std::uint32_t) override {
        terminated = process.valid();
        if (terminated && events != nullptr) events->push_back("terminate-child");
        return terminated ? BootstrapProcessResult{BootstrapProcessStatus::Ok}
                          : BootstrapProcessResult{BootstrapProcessStatus::InvalidIdentity};
    }
    void Close(BootstrapOwnedProcess& process) noexcept override { process = {}; }
    BootstrapProcessResult ReleaseOwned(BootstrapOwnedProcess& process) override {
        released = process.valid();
        if (released && events != nullptr) events->push_back("release-child");
        Close(process);
        return {BootstrapProcessStatus::Ok};
    }
};

struct HealthFake final : BootstrapHealthPort {
    bool timeout = false;
    bool mismatch = false;
    bool closed = false;
    bool prepareCalled = false;
    bool awaitCalled = false;
    std::vector<std::string>* events = nullptr;

    BootstrapHealthResult Prepare(std::string_view, std::string_view,
                                  std::string_view,
                                  BootstrapLaunchCapability& capability) override {
        prepareCalled = true;
        capability.inheritedStdout = 11;
        return {BootstrapHealthStatus::Accepted};
    }

    void ReleaseLaunchCapability() noexcept override {}

    BootstrapHealthResult Await(const BootstrapOwnedProcess&, std::string_view nonce,
                                std::string_view version,
                                std::string_view buildId,
                                std::uint32_t) override {
        awaitCalled = true;
        if (events != nullptr) events->push_back("health-await");
        if (timeout) return {BootstrapHealthStatus::TimedOut};
        HealthAcknowledgement ack;
        ack.token = mismatch ? std::string(64, 'd') : std::string(nonce);
        ack.version = mismatch ? "9.9.9" : std::string(version);
        ack.buildId = std::string(buildId);
        ack.uiReady = !mismatch;
        return {BootstrapHealthStatus::Accepted, 0, ack};
    }
    void Close() noexcept override { closed = true; }
};

struct FilesystemFake final : BootstrapFilesystemDriver {
    bool beginCalled = false;
    WindowsBootstrapRecoveryDisposition recoveryDisposition =
        WindowsBootstrapRecoveryDisposition::Complete;
    std::vector<BootstrapIntent> recoveryIntents;
    std::optional<ReplacementJournal> recoveryJournal;
    std::vector<BootstrapIntent> appliedIntents;
    std::vector<BootstrapIntentKind> intents;
    std::vector<BootstrapProcessIdentity> updatedChildren;
    std::vector<std::string> healthPhases;
    bool healthNonceConsumed = false;
    std::optional<std::uint32_t> boundPreviousFailureCount;
    bool appliedBeforeFailureCountBinding = false;
    bool failCommit = false;
    bool failRemoveBackup = false;
    bool failInvocationCleanup = false;
    bool failTerminalJournalRetirement = false;
    bool invocationCleanupCalled = false;
    bool useDefaultInvocationCleanup = false;
    std::optional<WindowsBootstrapFilesystemResult> invocationCleanupResult;
    std::optional<BootstrapIntentKind> retryableAccessDeniedKind;
    std::uint32_t retryableAccessDeniedFailuresRemaining = 0U;
    std::optional<BootstrapProcessIdentity> durableChild;
    std::optional<std::uint32_t> durablePreviousFailureCount;
    bool durableFailureRecordedForTransaction = false;
    std::string durableHealthPhase;
    bool recoveredHealthRelaunchPrepared = false;
    bool durableRollbackComplete = false;
    bool previousApplicationRelaunchStarted = false;
    bool previousApplicationRelaunchComplete = false;
    std::optional<BootstrapProcessIdentity> previousApplicationIdentity;
    WindowsFileContentFingerprint previousApplicationContent = [] {
        WindowsFileContentFingerprint value;
        value.valid = true;
        value.byteSize = 900U;
        value.sha256.fill(0xbbU);
        return value;
    }();
    bool terminalJournalRetired = false;
    std::vector<std::string>* events = nullptr;
    WindowsBootstrapFilesystemResult beginResult;
    WindowsBootstrapFilesystemResult bindResult;

    WindowsBootstrapFilesystemResult Begin(const ValidatedInstallPlan&,
                                           const ReplacementJournal&,
        const WindowsBootstrapFilesystemRoots&) override {
        beginCalled = true;
        return beginResult;
    }
    WindowsBootstrapFilesystemResult BindPreviousFailureCount(
        const std::uint32_t count) override {
        if (!bindResult.ok()) return bindResult;
        boundPreviousFailureCount = count;
        return {};
    }
    WindowsBootstrapFilesystemRecoveryResult BeginRecovery(
        const ValidatedInstallPlan&, const ReplacementJournal&,
        const WindowsBootstrapFilesystemRoots&) override {
        WindowsBootstrapFilesystemRecoveryResult result;
        result.filesystem = {};
        result.disposition = recoveryDisposition;
        result.intents = recoveryIntents;
        result.journal = recoveryJournal;
        return result;
    }
    WindowsBootstrapFilesystemRecoveryResult BeginRecovery(
        const ValidatedInstallPlan&, const WindowsBootstrapFilesystemRoots&) override {
        WindowsBootstrapFilesystemRecoveryResult result;
        result.filesystem = {};
        result.disposition = recoveryDisposition;
        result.intents = recoveryIntents;
        result.journal = recoveryJournal;
        return result;
    }
    WindowsBootstrapFilesystemResult Apply(const BootstrapIntent& intent) override {
        appliedBeforeFailureCountBinding =
            appliedBeforeFailureCountBinding || !boundPreviousFailureCount.has_value();
        appliedIntents.push_back(intent);
        intents.push_back(intent.kind);
        if (events != nullptr && intent.kind == BootstrapIntentKind::CommitJournal) {
            events->push_back("commit-journal");
        }
        if (events != nullptr && intent.kind == BootstrapIntentKind::RestoreBackup) {
            events->push_back("restore-backup");
        }
        if (events != nullptr && intent.kind == BootstrapIntentKind::RemoveBackup) {
            events->push_back("remove-backup");
        }
        if (failCommit && intent.kind == BootstrapIntentKind::CommitJournal) {
            return {WindowsBootstrapFilesystemStatus::PartialDurableState};
        }
        if (failRemoveBackup && intent.kind == BootstrapIntentKind::RemoveBackup) {
            return {WindowsBootstrapFilesystemStatus::CleanupPending};
        }
        if (retryableAccessDeniedKind == intent.kind &&
            retryableAccessDeniedFailuresRemaining > 0U) {
            --retryableAccessDeniedFailuresRemaining;
            return {WindowsBootstrapFilesystemStatus::RetryableAccessDenied,
                    WindowsFilesystemBackendStatus::AccessDenied, 32U};
        }
        return {};
    }
    WindowsBootstrapFilesystemResult CleanupCommittedInvocation() override {
        invocationCleanupCalled = true;
        if (events != nullptr) events->push_back("cleanup-invocation");
        if (useDefaultInvocationCleanup) {
            return BootstrapFilesystemDriver::CleanupCommittedInvocation();
        }
        if (invocationCleanupResult.has_value()) {
            return *invocationCleanupResult;
        }
        return failInvocationCleanup
            ? WindowsBootstrapFilesystemResult{
                  WindowsBootstrapFilesystemStatus::CleanupPending}
            : WindowsBootstrapFilesystemResult{};
    }
    WindowsBootstrapFilesystemResult RecordUpdatedChildIdentity(
        const BootstrapProcessIdentity& child) override {
        updatedChildren.push_back(child);
        return {};
    }
    WindowsBootstrapFilesystemResult RecordHealthState(
        std::string_view phase, bool nonceConsumed) override {
        healthPhases.emplace_back(phase);
        healthNonceConsumed = nonceConsumed;
        return {};
    }
    WindowsBootstrapFilesystemResult PrepareRecoveredHealthRelaunch(
        const BootstrapProcessIdentity& terminated) override {
        recoveredHealthRelaunchPrepared = durableChild.has_value() &&
            *durableChild == terminated;
        return {};
    }
    std::optional<BootstrapProcessIdentity> RecoveredUpdatedChildIdentity()
        const override {
        return durableChild;
    }
    std::string RecoveredHealthPhase() const override { return durableHealthPhase; }
    std::optional<std::uint32_t> RecoveredPreviousFailureCount()
        const override {
        return durablePreviousFailureCount;
    }
    bool RecoveredFailureRecordedForTransaction() const override {
        return durableFailureRecordedForTransaction;
    }
    bool RecoveredRollbackComplete() const override {
        return durableRollbackComplete;
    }
    bool RecoveredPreviousApplicationRelaunchStarted() const override {
        return previousApplicationRelaunchStarted;
    }
    bool RecoveredPreviousApplicationRelaunchComplete() const override {
        return previousApplicationRelaunchComplete;
    }
    std::optional<BootstrapProcessIdentity>
    RecoveredPreviousApplicationIdentity() const override {
        return previousApplicationIdentity;
    }
    std::optional<WindowsFileContentFingerprint>
    PreviousApplicationContentFingerprint() const override {
        return previousApplicationContent;
    }
    WindowsBootstrapFilesystemResult
    RecordPreviousApplicationRelaunchStarted() override {
        previousApplicationRelaunchStarted = true;
        return {};
    }
    WindowsBootstrapFilesystemResult RecordPreviousApplicationIdentity(
        const BootstrapProcessIdentity& identityValue) override {
        previousApplicationIdentity = identityValue;
        return {};
    }
    WindowsBootstrapFilesystemResult ResetPreviousApplicationIdentity(
        const BootstrapProcessIdentity& identityValue) override {
        if (!previousApplicationIdentity.has_value() ||
            !(*previousApplicationIdentity == identityValue)) {
            return {WindowsBootstrapFilesystemStatus::IdentityMismatch};
        }
        previousApplicationIdentity.reset();
        return {};
    }
    WindowsBootstrapFilesystemResult RecordPreviousApplicationRelaunchComplete(
        const BootstrapProcessIdentity& identityValue) override {
        if (!previousApplicationIdentity.has_value() ||
            !(*previousApplicationIdentity == identityValue)) {
            return {WindowsBootstrapFilesystemStatus::IdentityMismatch};
        }
        previousApplicationRelaunchComplete = true;
        return {};
    }
    WindowsBootstrapFilesystemResult RetireTerminalJournal() override {
        if (events != nullptr) events->push_back("retire-journal");
        if (failTerminalJournalRetirement) {
            return {WindowsBootstrapFilesystemStatus::CleanupPending};
        }
        terminalJournalRetired = true;
        return {};
    }
};

struct TerminalCleanupFake final : BootstrapTerminalCleanupPort {
    bool launchCalled = false;
    bool launchAccepted = false;
    std::string expectedPath;
    std::vector<std::string>* events = nullptr;

    BootstrapTerminalCleanupResult LaunchSuccessor(
        const ValidatedInstallPlan&,
        const ReplacementJournal&,
        const BootstrapExecutableExpectation& expectedBootstrap) override {
        launchCalled = true;
        expectedPath = expectedBootstrap.absolutePath;
        if (events != nullptr) events->push_back("launch-cleanup-successor");
        return launchAccepted
            ? BootstrapTerminalCleanupResult{
                  BootstrapTerminalCleanupStatus::Accepted}
            : BootstrapTerminalCleanupResult{
                  BootstrapTerminalCleanupStatus::LaunchFailed};
    }
};

BootstrapAdmission fixtureAdmission(bool includeSignedRemoval = false,
                                    bool includeBootstrap = false) {
    SignedInstallPlan raw;
    raw.schemaVersion = 1;
    raw.product = "aris";
    raw.version = "1.2.3";
    raw.buildId = "build-1";
    raw.keyId = "fixture-2026";
    raw.detachedSignature = std::string(128, '0');
    raw.payloadSha256 = std::string(64, 'a');
    raw.packagePlanDigest = std::string(64, 'e');
    raw.oldProcessId = 41;
    raw.healthToken = std::string(64, 'c');
    raw.identity.targetRoot = "C:/HarkenSearchTest/Aris";
    raw.identity.installationId = "portable-1";
    raw.identity.targetRootIdentity = {raw.identity.targetRoot, 1U, {}, true};
    raw.identity.targetRootIdentity.fileId.fill(0x11U);
    raw.identity.protectedDataRoots = {"C:/HarkenSearchTest/HarkenSearchData"};
    raw.invocation.updaterOwnedBase = "C:/HarkenSearchTest/HarkenSearchUpdates";
    raw.invocation.stagingRoot = "C:/HarkenSearchTest/HarkenSearchUpdates/staging";
    raw.invocation.transactionRoot = "C:/HarkenSearchTest/HarkenSearchUpdates/transaction";
    raw.invocation.invocationIdentity = "parent-capability-1";
    raw.invocation.transactionId = "txn-1";
    raw.invocation.packagePlanDigest = std::string(64, 'e');
    raw.invocation.oldProcess = identity(41);
    raw.identity.oldExecutableIdentity = {
        raw.invocation.oldProcess.imagePath,
        raw.invocation.oldProcess.imageVolumeSerial,
        raw.invocation.oldProcess.imageFileId,
        true};
    raw.invocation.deploymentMode = raw.identity.deploymentMode;
    raw.invocation.installationId = raw.identity.installationId;
    raw.invocation.targetRootIdentityBinding =
        raw.identity.targetRootIdentity.canonicalPath;
    raw.invocation.oldExecutableIdentityBinding =
        raw.identity.oldExecutableIdentity.canonicalPath;
    raw.entrypoint = "HarkenSearch.exe";
    raw.inventory = {{"HarkenSearch.exe", std::string(64, 'a'), 3}};
    if (includeBootstrap) {
        raw.inventory.push_back(
            {"HarkenSearchBootstrap.exe", std::string(64, 'f'), 4096U});
    }
    if (includeSignedRemoval) raw.removals = {"obsolete.dll"};
    const auto validated = ValidateSignedInstallPlan(raw, SignatureStatus::Verified);
    std::vector<ExistingInstallItem> existing{{"HarkenSearch.exe"}};
    if (includeBootstrap) existing.push_back({"HarkenSearchBootstrap.exe"});
    if (includeSignedRemoval) existing.push_back({"obsolete.dll"});
    const auto journal = BuildReplacementJournal(*validated.plan, existing, "txn-1");
    BootstrapAdmission result;
    result.status = BootstrapAdmissionStatus::Accepted;
    result.plan = validated.plan;
    result.journal = journal.journal;
    return result;
}

std::optional<ReplacementJournal> completedRecoveryJournal(
    const BootstrapAdmission& admission) {
    if (!admission.ok()) return std::nullopt;
    std::vector<ReplacementJournalItem> durableItems =
        admission.journal->items;
    for (ReplacementJournalItem& item : durableItems) {
        if (item.classification == JournalItemClass::PreserveExisting) continue;
        item.backupRecorded =
            item.classification == JournalItemClass::ReplaceExisting;
        item.replacementRecorded = true;
    }
    const JournalBuildResult rehydrated = RehydrateReplacementJournal(
        *admission.plan, durableItems, admission.journal->transactionId);
    return rehydrated.ok() ? rehydrated.journal : std::nullopt;
}

void test_authenticated_failure_count_is_bound_before_durable_publication() {
    const auto admission = fixtureAdmission();
    WindowsBootstrapFilesystemRoots roots{
        admission.plan->invocation.stagingRoot,
        admission.plan->invocation.transactionRoot,
        admission.plan->invocation.updaterOwnedBase,
        admission.plan->invocation.invocationIdentity};
    ProcessFake process;
    HealthFake health;
    FilesystemFake filesystem;

    const auto result = RunBootstrapApplication(
        admission, roots, {process, health, filesystem},
        BootstrapApplicationOptions{60'000U, 30'000U, 1U, false});

    check(result.status == BootstrapApplicationStatus::Completed &&
              filesystem.boundPreviousFailureCount == 1U &&
              !filesystem.appliedBeforeFailureCountBinding,
          "authenticated previous-failure count is bound before the first durable journal publication");
}

void test_filesystem_begin_failure_after_old_exit_relaunches_exact_untouched_application() {
    const auto admission = fixtureAdmission();
    WindowsBootstrapFilesystemRoots roots{
        admission.plan->invocation.stagingRoot,
        admission.plan->invocation.transactionRoot,
        admission.plan->invocation.updaterOwnedBase,
        admission.plan->invocation.invocationIdentity};
    ProcessFake process;
    HealthFake health;
    FilesystemFake filesystem;
    filesystem.beginResult = {
        WindowsBootstrapFilesystemStatus::JournalCollision};
    filesystem.previousApplicationContent.valid = false;

    const auto result = RunBootstrapApplication(
        admission, roots, {process, health, filesystem});

    check(result.status == BootstrapApplicationStatus::FilesystemRejected &&
              result.reason ==
                  "filesystem-begin-failed-previous-application-relaunched" &&
              !result.mutated,
          "filesystem Begin rejection remains typed after restoring application availability");
    check(process.waitCalled && filesystem.beginCalled &&
              !filesystem.boundPreviousFailureCount.has_value() &&
              filesystem.appliedIntents.empty(),
          "Begin rejection relaunches only after the exact wait and publishes no filesystem intent");
    check(process.launchedExecutables ==
                  std::vector<std::string>{"C:/HarkenSearchTest/Aris/HarkenSearch.exe"} &&
              process.launchDirectories ==
                  std::vector<std::string>{"C:/HarkenSearchTest/Aris"} &&
              process.suspendedLaunch && process.resumed &&
              process.preparedForCommit && process.released,
          "Begin rejection launches the untouched previous entrypoint suspended and releases it only after validation");
    const BootstrapExecutableExpectation expected =
        previousExecutableExpectation();
    check(process.validatedExecutables.size() == 1U &&
              process.validatedExecutables.front().absolutePath ==
                  expected.absolutePath &&
              process.validatedExecutables.front().sha256 == expected.sha256 &&
              process.validatedExecutables.front().byteSize == expected.byteSize &&
              process.validatedExecutables.front().exactFileIdentity ==
                  expected.exactFileIdentity &&
              !health.prepareCalled && !health.awaitCalled,
          "Begin rejection validates the old path, FileId, and content snapshot without opening health IPC");
}

void test_failure_count_binding_failure_after_old_exit_relaunches_exact_untouched_application() {
    const auto admission = fixtureAdmission();
    WindowsBootstrapFilesystemRoots roots{
        admission.plan->invocation.stagingRoot,
        admission.plan->invocation.transactionRoot,
        admission.plan->invocation.updaterOwnedBase,
        admission.plan->invocation.invocationIdentity};
    ProcessFake process;
    HealthFake health;
    FilesystemFake filesystem;
    filesystem.bindResult = {
        WindowsBootstrapFilesystemStatus::InvalidIntentOrder};
    filesystem.previousApplicationContent.valid = false;

    const auto result = RunBootstrapApplication(
        admission, roots, {process, health, filesystem});

    check(result.status == BootstrapApplicationStatus::FilesystemRejected &&
              result.reason ==
                  "filesystem-failure-count-binding-failed-previous-application-relaunched" &&
              !result.mutated,
          "failure-count binding rejection remains typed after restoring application availability");
    check(process.waitCalled && filesystem.beginCalled &&
              !filesystem.boundPreviousFailureCount.has_value() &&
              filesystem.appliedIntents.empty(),
          "failure-count binding rejection occurs after observation and before every durable intent");
    const BootstrapExecutableExpectation expected =
        previousExecutableExpectation();
    check(process.launchedExecutables ==
                  std::vector<std::string>{"C:/HarkenSearchTest/Aris/HarkenSearch.exe"} &&
              process.suspendedLaunch && process.resumed &&
              process.preparedForCommit && process.released &&
              process.validatedExecutables.size() == 1U &&
              process.validatedExecutables.front().absolutePath ==
                  expected.absolutePath &&
              process.validatedExecutables.front().sha256 == expected.sha256 &&
              process.validatedExecutables.front().byteSize == expected.byteSize &&
              process.validatedExecutables.front().exactFileIdentity ==
                  expected.exactFileIdentity &&
              !health.prepareCalled && !health.awaitCalled,
          "failure-count binding rejection relaunches only the exact captured old image without health IPC");
}

struct ProgressRecorder final : BootstrapProgressSink {
    std::vector<BootstrapProgressSnapshot> snapshots;
    void Report(BootstrapProgressSnapshot snapshot) noexcept override {
        snapshots.push_back(snapshot);
    }
    bool saw(BootstrapProgressPhase phase) const {
        return std::any_of(snapshots.begin(), snapshots.end(),
            [phase](const auto& snapshot) { return snapshot.phase == phase; });
    }
};

void test_progress_follows_real_operations_and_never_completes_a_failure() {
    const auto admission = fixtureAdmission();
    const WindowsBootstrapFilesystemRoots roots{
        admission.plan->invocation.stagingRoot,
        admission.plan->invocation.transactionRoot,
        admission.plan->invocation.updaterOwnedBase,
        admission.plan->invocation.invocationIdentity};
    for (int failureMode = 0; failureMode != 4; ++failureMode) {
        ProcessFake process;
        HealthFake health;
        FilesystemFake filesystem;
        ProgressRecorder progress;
        process.timeout = failureMode == 1;
        if (failureMode == 2) {
            filesystem.retryableAccessDeniedKind = BootstrapIntentKind::ReplaceInventoryItem;
            filesystem.retryableAccessDeniedFailuresRemaining = 50U;
        }
        health.timeout = failureMode == 3;
        BootstrapApplicationOptions options;
        options.forwardOnly = true;
        const auto result = RunBootstrapApplication(admission, roots,
            {process, health, filesystem, nullptr, &progress}, options);
        check(progress.saw(BootstrapProgressPhase::WaitingForExit),
              "progress exposes the real old-process wait");
        check(!progress.snapshots.empty() && progress.snapshots.back().phase ==
              (result.ok() ? BootstrapProgressPhase::Completed : BootstrapProgressPhase::Failed),
              "terminal progress agrees with the actual update result");
        check(result.ok() == (failureMode == 0), "fixture drives the intended outcome");
        if (failureMode == 1) {
            check(!progress.saw(BootstrapProgressPhase::PreparingFiles) &&
                  !progress.saw(BootstrapProgressPhase::ReplacingFiles) &&
                  !progress.saw(BootstrapProgressPhase::Restarting),
                  "exit timeout never claims files were replaced or app restarted");
        } else {
            check(progress.saw(BootstrapProgressPhase::PreparingFiles) &&
                  progress.saw(BootstrapProgressPhase::ReplacingFiles),
                  "file preparation and replacement are independently visible");
        }
        if (failureMode == 0 || failureMode == 3) {
            check(progress.saw(BootstrapProgressPhase::Restarting) &&
                  progress.saw(BootstrapProgressPhase::CheckingStartup),
                  "launch and startup acknowledgement are separate stages");
        }
        if (failureMode != 0) {
            check(!progress.saw(BootstrapProgressPhase::Completed),
                  "no timeout or replacement failure is reported as complete");
        } else {
            check(progress.saw(BootstrapProgressPhase::Finalizing),
                  "healthy startup still waits for transaction finalization");
        }
        std::size_t lastCount = 0;
        std::size_t total = 0;
        for (const auto& snapshot : progress.snapshots) {
            if (snapshot.phase != BootstrapProgressPhase::ReplacingFiles) continue;
            check(snapshot.completedFiles >= lastCount &&
                  snapshot.completedFiles <= snapshot.totalFiles,
                  "replacement count stays bounded and monotonic");
            lastCount = snapshot.completedFiles;
            total = snapshot.totalFiles;
        }
        if (failureMode == 0 || failureMode == 3) {
            check(total > 0 && lastCount == total, "successful writes reach their exact file count");
        } else if (failureMode == 2) {
            check(total > 0 && lastCount < total && !progress.saw(BootstrapProgressPhase::Restarting),
                  "failed write is not counted and never advances to restart");
        }
    }
}

void test_happy_path_and_wait_timeout() {
    const auto admission = fixtureAdmission();
    WindowsBootstrapFilesystemRoots roots{
        admission.plan->invocation.stagingRoot,
        admission.plan->invocation.transactionRoot,
        admission.plan->invocation.updaterOwnedBase,
        admission.plan->invocation.invocationIdentity};
    ProcessFake process;
    HealthFake health;
    FilesystemFake filesystem;
    const auto success = RunBootstrapApplication(
        admission, roots, {process, health, filesystem});
    check(success.status == BootstrapApplicationStatus::Completed &&
              filesystem.invocationCleanupCalled &&
              filesystem.terminalJournalRetired,
          "happy path commits after exact wait, replacement and health");
    check(filesystem.beginCalled && !filesystem.intents.empty(),
          "filesystem receives typed intents only after old-process wait");
    check(process.launched.size() == 1U && health.closed,
          "owned child launch and health channel are closed");
    check(filesystem.updatedChildren.size() == 1U &&
              !filesystem.healthPhases.empty() &&
              filesystem.healthNonceConsumed,
          "child identity and consumed health nonce are checkpointed durably");
    check(process.suspendedLaunch && process.resumed && process.released,
          "updated child is resumed only after checkpoint and released after commit");
    check(filesystem.updatedChildren.size() == 1U &&
              filesystem.updatedChildren.front().processId == 99U,
          "exact owned child identity is durably checkpointed");
    check(filesystem.healthPhases == std::vector<std::string>{"accepted"} &&
              filesystem.healthNonceConsumed,
          "accepted health is durably checkpointed before commit");

    ProcessFake timeoutProcess;
    timeoutProcess.timeout = true;
    HealthFake timeoutHealth;
    FilesystemFake timeoutFilesystem;
    const auto timedOut = RunBootstrapApplication(
        admission, roots, {timeoutProcess, timeoutHealth, timeoutFilesystem});
    check(timedOut.status == BootstrapApplicationStatus::OldProcessTimedOut &&
              !timeoutFilesystem.beginCalled,
          "old-process timeout is bounded and mutation-free");
}

void test_forward_only_terminal_failures_never_restore_installed_payload() {
    const auto admission = fixtureAdmission();
    WindowsBootstrapFilesystemRoots roots{
        admission.plan->invocation.stagingRoot,
        admission.plan->invocation.transactionRoot,
        admission.plan->invocation.updaterOwnedBase,
        admission.plan->invocation.invocationIdentity};

    BootstrapApplicationOptions forwardOnly;
    forwardOnly.forwardOnly = true;

    ProcessFake replacementProcess;
    HealthFake replacementHealth;
    FilesystemFake replacementFilesystem;
    replacementFilesystem.retryableAccessDeniedKind =
        BootstrapIntentKind::ReplaceInventoryItem;
    replacementFilesystem.retryableAccessDeniedFailuresRemaining = 50U;
    const auto replacementFailure = RunBootstrapApplication(
        admission, roots,
        {replacementProcess, replacementHealth, replacementFilesystem},
        forwardOnly);
    check(replacementFailure.status == BootstrapApplicationStatus::FilesystemRejected &&
              !replacementFailure.ok(),
          "forward-only replacement failure is a terminal filesystem result");
    check(std::none_of(
              replacementFilesystem.appliedIntents.begin(),
              replacementFilesystem.appliedIntents.end(),
              [](const BootstrapIntent& intent) {
                  return intent.kind == BootstrapIntentKind::RestoreBackup ||
                         intent.kind == BootstrapIntentKind::RemoveNewItem ||
                         intent.kind == BootstrapIntentKind::RelaunchPreviousApplication;
              }),
          "forward-only replacement failure emits no restore or old-version intents");

    ProcessFake launchProcess;
    launchProcess.launchFailure = true;
    HealthFake launchHealth;
    FilesystemFake launchFilesystem;
    const auto launchFailure = RunBootstrapApplication(
        admission, roots, {launchProcess, launchHealth, launchFilesystem},
        forwardOnly);
    check(launchFailure.status == BootstrapApplicationStatus::LaunchRejected &&
              launchFilesystem.appliedIntents.size() > 0U,
          "forward-only child launch failure is terminal after replacement intents");
    check(std::none_of(
              launchFilesystem.appliedIntents.begin(),
              launchFilesystem.appliedIntents.end(),
              [](const BootstrapIntent& intent) {
                  return intent.kind == BootstrapIntentKind::RestoreBackup ||
                         intent.kind == BootstrapIntentKind::RemoveNewItem ||
                         intent.kind == BootstrapIntentKind::RelaunchPreviousApplication;
              }),
          "forward-only launch failure leaves installed payload forward-only");

    ProcessFake healthProcess;
    HealthFake healthFailure;
    healthFailure.timeout = true;
    FilesystemFake healthFilesystem;
    const auto healthResult = RunBootstrapApplication(
        admission, roots, {healthProcess, healthFailure, healthFilesystem},
        forwardOnly);
    check(healthResult.status ==
                  BootstrapApplicationStatus::InstalledHealthUnconfirmed &&
              !healthResult.ok() && healthResult.mutated &&
              healthProcess.terminated,
          "forward-only health failure keeps the verified replacement installed without reporting success");
    check(std::none_of(
              healthFilesystem.appliedIntents.begin(),
              healthFilesystem.appliedIntents.end(),
              [](const BootstrapIntent& intent) {
                  return intent.kind == BootstrapIntentKind::RestoreBackup ||
                         intent.kind == BootstrapIntentKind::RemoveNewItem ||
                         intent.kind == BootstrapIntentKind::RelaunchPreviousApplication;
              }),
          "forward-only health failure emits no old-version restore intents");
}

void test_forward_only_recovery_requires_reinstall_without_restore_or_relaunch() {
    const auto admission = fixtureAdmission();
    WindowsBootstrapFilesystemRoots roots{
        admission.plan->invocation.stagingRoot,
        admission.plan->invocation.transactionRoot,
        admission.plan->invocation.updaterOwnedBase,
        admission.plan->invocation.invocationIdentity};
    ProcessFake process;
    HealthFake health;
    FilesystemFake filesystem;
    filesystem.recoveryDisposition =
        WindowsBootstrapRecoveryDisposition::Rollback;
    filesystem.recoveryIntents = {
        {BootstrapIntentKind::RestoreBackup, "HarkenSearch.exe", 0,
         "legacy-rollback"},
        {BootstrapIntentKind::RelaunchPreviousApplication, "HarkenSearch.exe", 0,
         "legacy-relaunch"}};

    BootstrapApplicationOptions options;
    options.recovery = true;
    options.forwardOnly = true;
    const auto result = RunBootstrapApplication(
        admission, roots, {process, health, filesystem}, options);

    check(result.status == BootstrapApplicationStatus::ReinstallRequired &&
              result.reason == "reinstall-required-recovery-rollback",
          "forward-only recovery converts rollback authority into an explicit reinstall result");
    check(filesystem.appliedIntents.empty() && process.launched.empty(),
          "forward-only recovery performs no restore or previous-version relaunch mutation");
}

void test_transient_forward_access_denied_is_retried_before_rollback() {
    const auto admission = fixtureAdmission();
    WindowsBootstrapFilesystemRoots roots{
        admission.plan->invocation.stagingRoot,
        admission.plan->invocation.transactionRoot,
        admission.plan->invocation.updaterOwnedBase,
        admission.plan->invocation.invocationIdentity};
    ProcessFake process;
    HealthFake health;
    FilesystemFake filesystem;
    filesystem.retryableAccessDeniedKind =
        BootstrapIntentKind::ReplaceInventoryItem;
    filesystem.retryableAccessDeniedFailuresRemaining = 1U;

    const auto result = RunBootstrapApplication(
        admission, roots, {process, health, filesystem});

    check(result.status == BootstrapApplicationStatus::Completed,
          "a transient pre-replacement sharing denial does not roll back a recoverable transaction");
    check(std::count(filesystem.intents.begin(), filesystem.intents.end(),
                     BootstrapIntentKind::ReplaceInventoryItem) == 2,
          "the runtime retries the exact forward intent after a typed retryable denial");
}

void test_signed_removal_flows_through_runtime_before_health_commit() {
    const auto admission = fixtureAdmission(true);
    WindowsBootstrapFilesystemRoots roots{
        admission.plan->invocation.stagingRoot,
        admission.plan->invocation.transactionRoot,
        admission.plan->invocation.updaterOwnedBase,
        admission.plan->invocation.invocationIdentity};
    ProcessFake process;
    HealthFake health;
    FilesystemFake filesystem;
    const auto result = RunBootstrapApplication(
        admission, roots, {process, health, filesystem});
    const auto backup = std::find_if(
        filesystem.appliedIntents.begin(), filesystem.appliedIntents.end(),
        [](const BootstrapIntent& intent) {
            return intent.kind == BootstrapIntentKind::SnapshotBackup &&
                   intent.relativePath == "obsolete.dll";
        });
    const auto removal = std::find_if(
        filesystem.appliedIntents.begin(), filesystem.appliedIntents.end(),
        [](const BootstrapIntent& intent) {
            return intent.kind == BootstrapIntentKind::RemoveInstalledItem &&
                   intent.relativePath == "obsolete.dll";
        });
    check(result.status == BootstrapApplicationStatus::Completed &&
              backup != filesystem.appliedIntents.end() &&
              removal != filesystem.appliedIntents.end() && backup < removal,
          "runtime durably backs up and deletes the signed removal before health commit");
}

void test_child_ownership_is_released_only_after_commit_publication() {
    const auto admission = fixtureAdmission();
    WindowsBootstrapFilesystemRoots roots{
        admission.plan->invocation.stagingRoot,
        admission.plan->invocation.transactionRoot,
        admission.plan->invocation.updaterOwnedBase,
        admission.plan->invocation.invocationIdentity};
    std::vector<std::string> events;
    ProcessFake process;
    process.events = &events;
    HealthFake health;
    FilesystemFake filesystem;
    filesystem.events = &events;

    const auto result = RunBootstrapApplication(
        admission, roots, {process, health, filesystem});
    check(result.status == BootstrapApplicationStatus::Completed,
          "ownership handoff keeps the successful transaction result");
    const auto prepared = std::find(events.begin(), events.end(),
                                    "prepare-child-commit");
    const auto committed = std::find(events.begin(), events.end(),
                                     "commit-journal");
    const auto released = std::find(events.begin(), events.end(),
                                    "release-child");
    const auto invocationCleanup = std::find(events.begin(), events.end(),
                                             "cleanup-invocation");
    const auto retired = std::find(events.begin(), events.end(),
                                   "retire-journal");
    check(process.preparedForCommit && prepared != events.end() &&
              committed != events.end() && released != events.end() &&
              invocationCleanup != events.end() && retired != events.end() &&
              prepared < committed && committed < released &&
              released < invocationCleanup && invocationCleanup < retired,
          "child containment stays owned through commit, then residue is cleaned before journal retirement");
}

void test_default_invocation_cleanup_reports_committed_update_complete() {
    const auto admission = fixtureAdmission();
    WindowsBootstrapFilesystemRoots roots{
        admission.plan->invocation.stagingRoot,
        admission.plan->invocation.transactionRoot,
        admission.plan->invocation.updaterOwnedBase,
        admission.plan->invocation.invocationIdentity};
    ProcessFake process;
    HealthFake health;
    FilesystemFake filesystem;
    filesystem.useDefaultInvocationCleanup = true;

    const auto result = RunBootstrapApplication(
        admission, roots, {process, health, filesystem});

    check(result.status == BootstrapApplicationStatus::Completed &&
              result.reason == "commit-cleanup-pending" &&
              filesystem.invocationCleanupCalled &&
              !filesystem.terminalJournalRetired,
          "a committed healthy update reports success while retaining the journal for unfinished cleanup");
}

void test_child_ownership_prepare_failure_rolls_back_with_handle() {
    const auto admission = fixtureAdmission();
    WindowsBootstrapFilesystemRoots roots{
        admission.plan->invocation.stagingRoot,
        admission.plan->invocation.transactionRoot,
        admission.plan->invocation.updaterOwnedBase,
        admission.plan->invocation.invocationIdentity};
    ProcessFake process;
    process.prepareForCommitStatus = BootstrapProcessStatus::TerminationFailed;
    HealthFake health;
    FilesystemFake filesystem;

    const auto result = RunBootstrapApplication(
        admission, roots, {process, health, filesystem});
    check(result.status == BootstrapApplicationStatus::RolledBack,
          "ownership preparation failure enters rollback before commit");
    check(process.terminated &&
              std::find(filesystem.intents.begin(), filesystem.intents.end(),
                        BootstrapIntentKind::CommitJournal) ==
                  filesystem.intents.end(),
          "ownership preparation failure retains termination authority and publishes no commit");
}

void test_old_process_exit_between_channel_admission_and_open_is_complete() {
    const auto admission = fixtureAdmission();
    WindowsBootstrapFilesystemRoots roots{
        admission.plan->invocation.stagingRoot,
        admission.plan->invocation.transactionRoot,
        admission.plan->invocation.updaterOwnedBase,
        admission.plan->invocation.invocationIdentity};
    ProcessFake process;
    process.openStatus = BootstrapProcessStatus::NotFound;
    process.openNativeError = 87U;
    HealthFake health;
    FilesystemFake filesystem;

    const auto result = RunBootstrapApplication(
        admission, roots, {process, health, filesystem});

    check(result.status == BootstrapApplicationStatus::Completed &&
              filesystem.beginCalled && !process.waitCalled,
          "an authenticated old process that already exited satisfies the lifetime wait");
}

void test_old_process_open_failure_remains_mutation_free() {
    const auto admission = fixtureAdmission();
    WindowsBootstrapFilesystemRoots roots{
        admission.plan->invocation.stagingRoot,
        admission.plan->invocation.transactionRoot,
        admission.plan->invocation.updaterOwnedBase,
        admission.plan->invocation.invocationIdentity};
    ProcessFake process;
    process.openStatus = BootstrapProcessStatus::OpenFailed;
    process.openNativeError = 5U;
    HealthFake health;
    FilesystemFake filesystem;

    const auto result = RunBootstrapApplication(
        admission, roots, {process, health, filesystem});

    check(result.status == BootstrapApplicationStatus::OldProcessRejected &&
              !filesystem.beginCalled && !process.waitCalled,
          "old-process access denial remains distinct from an already exited process");
}

void test_ambiguous_commit_failure_contains_child_for_restart_recovery() {
    const auto admission = fixtureAdmission();
    WindowsBootstrapFilesystemRoots roots{
        admission.plan->invocation.stagingRoot,
        admission.plan->invocation.transactionRoot,
        admission.plan->invocation.updaterOwnedBase,
        admission.plan->invocation.invocationIdentity};
    ProcessFake process;
    HealthFake health;
    FilesystemFake filesystem;
    filesystem.failCommit = true;
    const auto result = RunBootstrapApplication(
        admission, roots, {process, health, filesystem});
    check(result.status == BootstrapApplicationStatus::Quarantined,
          "ambiguous commit publication failure defers to restart recovery");
    check(process.terminated && !process.released,
          "ambiguous commit publication failure drains the still-armed child before recovery");
    check(std::count(filesystem.intents.begin(), filesystem.intents.end(),
                     BootstrapIntentKind::RestoreBackup) == 0,
          "ambiguous commit publication failure never guesses rollback authority");
}

void test_post_commit_cleanup_failure_keeps_healthy_child_for_recovery() {
    const auto admission = fixtureAdmission();
    WindowsBootstrapFilesystemRoots roots{
        admission.plan->invocation.stagingRoot,
        admission.plan->invocation.transactionRoot,
        admission.plan->invocation.updaterOwnedBase,
        admission.plan->invocation.invocationIdentity};
    ProcessFake process;
    HealthFake health;
    FilesystemFake filesystem;
    filesystem.failRemoveBackup = true;

    const auto result = RunBootstrapApplication(
        admission, roots, {process, health, filesystem});

    check(result.status == BootstrapApplicationStatus::Completed &&
              result.reason == "commit-cleanup-pending",
          "post-commit cleanup failure reports the already committed update as complete");
    check(process.released && !process.terminated,
          "post-commit cleanup failure releases the healthy child without terminating it");
    check(std::find(filesystem.intents.begin(), filesystem.intents.end(),
                    BootstrapIntentKind::CommitJournal) != filesystem.intents.end() &&
              std::find(filesystem.intents.begin(), filesystem.intents.end(),
                        BootstrapIntentKind::RestoreBackup) == filesystem.intents.end(),
          "post-commit cleanup failure preserves the committed journal and never restores");
}

void test_invocation_residue_cleanup_failure_keeps_committed_journal() {
    const auto admission = fixtureAdmission();
    WindowsBootstrapFilesystemRoots roots{
        admission.plan->invocation.stagingRoot,
        admission.plan->invocation.transactionRoot,
        admission.plan->invocation.updaterOwnedBase,
        admission.plan->invocation.invocationIdentity};
    ProcessFake process;
    HealthFake health;
    FilesystemFake filesystem;
    filesystem.failInvocationCleanup = true;

    const auto result = RunBootstrapApplication(
        admission, roots, {process, health, filesystem});

    check(result.status == BootstrapApplicationStatus::Completed &&
              result.reason == "commit-cleanup-pending" &&
              filesystem.invocationCleanupCalled &&
              !filesystem.terminalJournalRetired && process.released &&
              !process.terminated,
          "invocation residue cleanup failure retains the committed journal and releases but does not terminate the healthy child");
}

void test_journal_retirement_failure_reports_committed_update_complete() {
    const auto admission = fixtureAdmission();
    WindowsBootstrapFilesystemRoots roots{
        admission.plan->invocation.stagingRoot,
        admission.plan->invocation.transactionRoot,
        admission.plan->invocation.updaterOwnedBase,
        admission.plan->invocation.invocationIdentity};
    ProcessFake process;
    HealthFake health;
    FilesystemFake filesystem;
    filesystem.failTerminalJournalRetirement = true;

    const auto result = RunBootstrapApplication(
        admission, roots, {process, health, filesystem});

    check(result.status == BootstrapApplicationStatus::Completed &&
              result.reason == "commit-journal-retirement-failed-20" &&
              filesystem.invocationCleanupCalled &&
              !filesystem.terminalJournalRetired && process.released &&
              !process.terminated,
          "journal retirement failure retains recovery state without misreporting the committed update as failed");
}

void test_retryable_committed_cleanup_handoff_defers_retirement() {
    const auto admission = fixtureAdmission(false, true);
    WindowsBootstrapFilesystemRoots roots{
        admission.plan->invocation.stagingRoot,
        admission.plan->invocation.transactionRoot,
        admission.plan->invocation.updaterOwnedBase,
        admission.plan->invocation.invocationIdentity};
    ProcessFake process;
    HealthFake health;
    FilesystemFake filesystem;
    filesystem.invocationCleanupResult = WindowsBootstrapFilesystemResult{
        WindowsBootstrapFilesystemStatus::RetryableAccessDenied,
        WindowsFilesystemBackendStatus::AccessDenied, 5U};
    TerminalCleanupFake successor;
    successor.launchAccepted = true;

    const auto result = RunBootstrapApplication(
        admission, roots, {process, health, filesystem, &successor});

    check(result.status == BootstrapApplicationStatus::Completed &&
              result.reason == "committed-cleanup-deferred" &&
              successor.launchCalled &&
              successor.expectedPath ==
                  admission.plan->identity.targetRoot + "/HarkenSearchBootstrap.exe" &&
              process.released && !process.terminated &&
              !filesystem.terminalJournalRetired,
          "a typed pre-mutation cleanup denial hands off to an exact successor and keeps the journal for it");
}

void test_retryable_committed_cleanup_without_successor_reports_update_complete() {
    const auto admission = fixtureAdmission(false, true);
    WindowsBootstrapFilesystemRoots roots{
        admission.plan->invocation.stagingRoot,
        admission.plan->invocation.transactionRoot,
        admission.plan->invocation.updaterOwnedBase,
        admission.plan->invocation.invocationIdentity};
    ProcessFake process;
    HealthFake health;
    FilesystemFake filesystem;
    filesystem.invocationCleanupResult = WindowsBootstrapFilesystemResult{
        WindowsBootstrapFilesystemStatus::RetryableAccessDenied,
        WindowsFilesystemBackendStatus::AccessDenied, 5U};
    TerminalCleanupFake successor;
    successor.launchAccepted = false;

    const auto result = RunBootstrapApplication(
        admission, roots, {process, health, filesystem, &successor});

    check(result.status == BootstrapApplicationStatus::Completed &&
              result.reason == "commit-cleanup-pending" &&
              successor.launchCalled &&
              !filesystem.terminalJournalRetired && process.released,
          "a failed cleanup successor retains recovery state without misreporting the committed update as failed");
}

void test_recovery_without_original_health_nonce_quarantines() {
    const auto admission = fixtureAdmission();
    WindowsBootstrapFilesystemRoots roots{
        admission.plan->invocation.stagingRoot,
        admission.plan->invocation.transactionRoot,
        admission.plan->invocation.updaterOwnedBase,
        admission.plan->invocation.invocationIdentity};
    ProcessFake process;
    HealthFake health;
    FilesystemFake filesystem;
    filesystem.recoveryDisposition =
        WindowsBootstrapRecoveryDisposition::ContinueForward;
    const auto result = RunBootstrapApplication(
        admission, roots, {process, health, filesystem},
        BootstrapApplicationOptions{60'000U, 30'000U, 0U, true});
    check(result.status == BootstrapApplicationStatus::Quarantined &&
              !process.suspendedLaunch && !health.awaitCalled &&
              filesystem.updatedChildren.empty(),
          "fresh recovery never relaunches with a synthetic or replayed health nonce");
}

void test_health_failure_owns_rollback_child() {
    const auto admission = fixtureAdmission();
    WindowsBootstrapFilesystemRoots roots{
        admission.plan->invocation.stagingRoot,
        admission.plan->invocation.transactionRoot,
        admission.plan->invocation.updaterOwnedBase,
        admission.plan->invocation.invocationIdentity};
    ProcessFake process;
    HealthFake health;
    health.timeout = true;
    FilesystemFake filesystem;
    std::vector<std::string> events;
    filesystem.events = &events;
    const auto result = RunBootstrapApplication(
        admission, roots, {process, health, filesystem});
    check(result.status == BootstrapApplicationStatus::RolledBack,
          "health timeout chooses rollback");
    check(process.terminated,
          "rollback terminates only the child returned by launch");
    check(std::count(filesystem.intents.begin(), filesystem.intents.end(),
                     BootstrapIntentKind::RemoveBackup) == 1,
           "rollback removes the backup only after the previous application handoff is durable");
    const auto backupRemoved = std::find(events.begin(), events.end(),
                                         "remove-backup");
    const auto invocationCleaned = std::find(events.begin(), events.end(),
                                             "cleanup-invocation");
    const auto journalRetired = std::find(events.begin(), events.end(),
                                          "retire-journal");
    check(filesystem.invocationCleanupCalled &&
              filesystem.terminalJournalRetired &&
              backupRemoved != events.end() &&
              invocationCleaned != events.end() &&
              journalRetired != events.end() &&
              backupRemoved < invocationCleaned &&
              invocationCleaned < journalRetired,
          "live rollback cleans invocation residue after backup removal and before journal retirement");
}

void test_live_rollback_cleanup_failure_preserves_the_journal() {
    const auto admission = fixtureAdmission();
    WindowsBootstrapFilesystemRoots roots{
        admission.plan->invocation.stagingRoot,
        admission.plan->invocation.transactionRoot,
        admission.plan->invocation.updaterOwnedBase,
        admission.plan->invocation.invocationIdentity};
    ProcessFake process;
    HealthFake health;
    health.timeout = true;
    FilesystemFake filesystem;
    filesystem.failInvocationCleanup = true;

    const auto result = RunBootstrapApplication(
        admission, roots, {process, health, filesystem});

    check(result.status == BootstrapApplicationStatus::Quarantined &&
              filesystem.invocationCleanupCalled &&
              !filesystem.terminalJournalRetired,
          "live rollback keeps the journal when invocation residue cleanup is still pending");
}

void test_transient_restore_access_denied_is_retried_before_quarantine() {
    const auto admission = fixtureAdmission();
    WindowsBootstrapFilesystemRoots roots{
        admission.plan->invocation.stagingRoot,
        admission.plan->invocation.transactionRoot,
        admission.plan->invocation.updaterOwnedBase,
        admission.plan->invocation.invocationIdentity};
    ProcessFake process;
    HealthFake health;
    health.mismatch = true;
    FilesystemFake filesystem;
    filesystem.retryableAccessDeniedKind =
        BootstrapIntentKind::RestoreBackup;
    filesystem.retryableAccessDeniedFailuresRemaining = 1U;

    const auto result = RunBootstrapApplication(
        admission, roots, {process, health, filesystem});

    check(result.status == BootstrapApplicationStatus::RolledBack,
          "a transient pre-restore sharing denial does not quarantine a recoverable transaction");
    check(std::count(filesystem.intents.begin(), filesystem.intents.end(),
                     BootstrapIntentKind::RestoreBackup) == 2,
          "the runtime retries the exact restore intent after a typed retryable denial");
}

void test_launched_child_identity_is_plan_bound_before_resume() {
    const auto admission = fixtureAdmission();
    WindowsBootstrapFilesystemRoots roots{
        admission.plan->invocation.stagingRoot,
        admission.plan->invocation.transactionRoot,
        admission.plan->invocation.updaterOwnedBase,
        admission.plan->invocation.invocationIdentity};
    ProcessFake process;
    process.childSubstitution = true;
    HealthFake health;
    FilesystemFake filesystem;

    const auto result = RunBootstrapApplication(
        admission, roots, {process, health, filesystem});

    check(result.status == BootstrapApplicationStatus::RolledBack,
          "substituted launched image is rejected before health/commit");
    check(std::find(process.resumedProcessIds.begin(),
                    process.resumedProcessIds.end(), 99U) ==
              process.resumedProcessIds.end() &&
              !health.awaitCalled && health.closed,
          "substituted child is never resumed or health-checked before rollback cleanup");
    check(process.terminated,
          "substituted child remains owned and is terminated during rollback");
}

void test_second_failure_quarantines_instead_of_retrying_forever() {
    const auto admission = fixtureAdmission();
    WindowsBootstrapFilesystemRoots roots{
        admission.plan->invocation.stagingRoot,
        admission.plan->invocation.transactionRoot,
        admission.plan->invocation.updaterOwnedBase,
        admission.plan->invocation.invocationIdentity};
    ProcessFake process;
    HealthFake health;
    health.timeout = true;
    FilesystemFake filesystem;

    const auto result = RunBootstrapApplication(
        admission, roots, {process, health, filesystem},
        BootstrapApplicationOptions{60'000U, 30'000U, 1U, false});

    check(result.status == BootstrapApplicationStatus::Quarantined,
          "second failed update attempt is quarantined");
    check(std::find(filesystem.intents.begin(), filesystem.intents.end(),
                    BootstrapIntentKind::RecordFailure) != filesystem.intents.end() &&
              std::find(filesystem.intents.begin(), filesystem.intents.end(),
                        BootstrapIntentKind::QuarantinePackage) !=
                  filesystem.intents.end(),
          "failure count and quarantine policy are durably emitted as typed intents");
}

void test_prepared_forward_recovery_quarantines_without_publishing_rollback() {
    const auto admission = fixtureAdmission();
    WindowsBootstrapFilesystemRoots roots{
        admission.plan->invocation.stagingRoot,
        admission.plan->invocation.transactionRoot,
        admission.plan->invocation.updaterOwnedBase,
        admission.plan->invocation.invocationIdentity};
    ProcessFake process;
    HealthFake health;
    FilesystemFake filesystem;
    filesystem.recoveryDisposition =
        WindowsBootstrapRecoveryDisposition::ContinueForward;
    filesystem.recoveryIntents = {
        {BootstrapIntentKind::ReplaceInventoryItem, "HarkenSearch.exe", 0,
         "restart-forward"}};
    BootstrapApplicationOptions options;
    options.recovery = true;

    const auto result = RunBootstrapApplication(
        admission, roots, {process, health, filesystem}, options);

    check(result.status == BootstrapApplicationStatus::Quarantined,
          "prepared forward recovery quarantines when the original health secret is unavailable");
    check(!process.suspendedLaunch && filesystem.updatedChildren.empty() &&
              !health.awaitCalled && filesystem.healthPhases.empty(),
          "forward recovery never creates a child or health attempt from a digest placeholder");
    check(std::none_of(filesystem.appliedIntents.begin(),
                       filesystem.appliedIntents.end(),
                       [](const BootstrapIntent& intent) {
                           return intent.kind == BootstrapIntentKind::PersistJournal &&
                                  intent.reason == "rolling-back";
                       }),
          "prepared forward recovery does not publish a decoder-invalid rolling-back state");
}

void test_authenticated_recovery_capability_can_finish_forward_once() {
    const auto admission = fixtureAdmission();
    WindowsBootstrapFilesystemRoots roots{
        admission.plan->invocation.stagingRoot,
        admission.plan->invocation.transactionRoot,
        admission.plan->invocation.updaterOwnedBase,
        admission.plan->invocation.invocationIdentity};
    ProcessFake process;
    HealthFake health;
    FilesystemFake filesystem;
    filesystem.recoveryDisposition =
        WindowsBootstrapRecoveryDisposition::ContinueForward;
    filesystem.recoveryIntents = {
        {BootstrapIntentKind::SnapshotBackup, "HarkenSearch.exe", 0,
         "restart-forward"},
        {BootstrapIntentKind::ReplaceInventoryItem, "HarkenSearch.exe", 0,
         "restart-forward"}};

    const auto result = RunBootstrapApplication(
        admission, roots, {process, health, filesystem},
        BootstrapApplicationOptions{60'000U, 30'000U, 0U, true, true});

    check(result.status == BootstrapApplicationStatus::Recovered &&
              process.suspendedLaunch && process.resumed && process.released &&
              health.awaitCalled && filesystem.healthNonceConsumed,
          "fresh authenticated recovery uses its re-supplied nonce exactly once and commits");
}

void test_authenticated_recovery_retries_transient_forward_denial() {
    const auto admission = fixtureAdmission();
    WindowsBootstrapFilesystemRoots roots{
        admission.plan->invocation.stagingRoot,
        admission.plan->invocation.transactionRoot,
        admission.plan->invocation.updaterOwnedBase,
        admission.plan->invocation.invocationIdentity};
    ProcessFake process;
    HealthFake health;
    FilesystemFake filesystem;
    filesystem.recoveryDisposition =
        WindowsBootstrapRecoveryDisposition::ContinueForward;
    filesystem.recoveryIntents = {
        {BootstrapIntentKind::SnapshotBackup, "HarkenSearch.exe", 0,
         "restart-forward"},
        {BootstrapIntentKind::ReplaceInventoryItem, "HarkenSearch.exe", 0,
         "restart-forward"}};
    filesystem.retryableAccessDeniedKind =
        BootstrapIntentKind::ReplaceInventoryItem;
    filesystem.retryableAccessDeniedFailuresRemaining = 1U;

    const auto result = RunBootstrapApplication(
        admission, roots, {process, health, filesystem},
        BootstrapApplicationOptions{60'000U, 30'000U, 0U, true, true});

    check(result.status == BootstrapApplicationStatus::Recovered,
          "authenticated restart recovery retries a transient forward sharing denial");
    check(std::count(filesystem.intents.begin(), filesystem.intents.end(),
                     BootstrapIntentKind::ReplaceInventoryItem) == 2,
          "authenticated restart recovery retries only the exact typed forward intent");
}

void test_recovery_rollback_records_exactly_one_failure() {
    const auto admission = fixtureAdmission();
    WindowsBootstrapFilesystemRoots roots{
        admission.plan->invocation.stagingRoot,
        admission.plan->invocation.transactionRoot,
        admission.plan->invocation.updaterOwnedBase,
        admission.plan->invocation.invocationIdentity};
    ProcessFake process;
    HealthFake health;
    FilesystemFake filesystem;
    filesystem.recoveryDisposition = WindowsBootstrapRecoveryDisposition::Rollback;
    filesystem.recoveryIntents = {
        {BootstrapIntentKind::RestoreBackup, "HarkenSearch.exe", 0,
         "restart-rollback"}};

    const auto result = RunBootstrapApplication(
        admission, roots, {process, health, filesystem},
        BootstrapApplicationOptions{60'000U, 30'000U, 0U, true});

    check(result.status == BootstrapApplicationStatus::RolledBack,
          "first recovery rollback remains retryable after restoring the previous app");
    check(std::count(filesystem.intents.begin(), filesystem.intents.end(),
                     BootstrapIntentKind::RecordFailure) == 1 &&
              std::count(filesystem.intents.begin(), filesystem.intents.end(),
                         BootstrapIntentKind::QuarantinePackage) == 0,
          "first recovery rollback records exactly one durable failure without quarantine");
}

void test_recovery_rollback_retries_transient_restore_denial() {
    const auto admission = fixtureAdmission();
    WindowsBootstrapFilesystemRoots roots{
        admission.plan->invocation.stagingRoot,
        admission.plan->invocation.transactionRoot,
        admission.plan->invocation.updaterOwnedBase,
        admission.plan->invocation.invocationIdentity};
    ProcessFake process;
    HealthFake health;
    FilesystemFake filesystem;
    filesystem.recoveryDisposition =
        WindowsBootstrapRecoveryDisposition::Rollback;
    filesystem.recoveryIntents = {
        {BootstrapIntentKind::RestoreBackup, "HarkenSearch.exe", 0,
         "restart-rollback"}};
    filesystem.retryableAccessDeniedKind =
        BootstrapIntentKind::RestoreBackup;
    filesystem.retryableAccessDeniedFailuresRemaining = 1U;

    const auto result = RunBootstrapApplication(
        admission, roots, {process, health, filesystem},
        BootstrapApplicationOptions{60'000U, 30'000U, 0U, true});

    check(result.status == BootstrapApplicationStatus::RolledBack,
          "restart recovery retries a transient pre-restore sharing denial");
    check(std::count(filesystem.intents.begin(), filesystem.intents.end(),
                     BootstrapIntentKind::RestoreBackup) == 2,
          "restart recovery retries only the exact typed restore intent");
}

void test_recovery_defers_backup_cleanup_until_previous_app_is_running() {
    const auto admission = fixtureAdmission();
    WindowsBootstrapFilesystemRoots roots{
        admission.plan->invocation.stagingRoot,
        admission.plan->invocation.transactionRoot,
        admission.plan->invocation.updaterOwnedBase,
        admission.plan->invocation.invocationIdentity};
    ProcessFake process;
    HealthFake health;
    FilesystemFake filesystem;
    std::vector<std::string> events;
    filesystem.events = &events;
    filesystem.recoveryDisposition = WindowsBootstrapRecoveryDisposition::Rollback;
    filesystem.recoveryIntents = {
        {BootstrapIntentKind::RemoveBackup, {}, 0,
         "restart-rollback-cleanup"}};
    filesystem.durableRollbackComplete = true;
    filesystem.durableFailureRecordedForTransaction = true;
    filesystem.durablePreviousFailureCount = 1U;

    const auto result = RunBootstrapApplication(
        admission, roots, {process, health, filesystem},
        BootstrapApplicationOptions{60'000U, 30'000U, 0U, true, true});

    check(result.status == BootstrapApplicationStatus::RolledBack &&
              filesystem.previousApplicationRelaunchComplete,
          "recovery proves the previous application handoff before cleanup");
    check(std::count(filesystem.intents.begin(), filesystem.intents.end(),
                     BootstrapIntentKind::RemoveBackup) == 1,
           "recovery applies backup cleanup exactly once after the relaunch checkpoint");
    const auto backupRemoved = std::find(events.begin(), events.end(),
                                         "remove-backup");
    const auto invocationCleaned = std::find(events.begin(), events.end(),
                                             "cleanup-invocation");
    const auto journalRetired = std::find(events.begin(), events.end(),
                                          "retire-journal");
    check(filesystem.invocationCleanupCalled &&
              filesystem.terminalJournalRetired &&
              backupRemoved != events.end() &&
              invocationCleaned != events.end() &&
              journalRetired != events.end() &&
              backupRemoved < invocationCleaned &&
              invocationCleaned < journalRetired,
          "rollback recovery cleans invocation residue before retiring its journal");
}

void test_recovery_rollback_quarantines_on_second_failure() {
    const auto admission = fixtureAdmission();
    WindowsBootstrapFilesystemRoots roots{
        admission.plan->invocation.stagingRoot,
        admission.plan->invocation.transactionRoot,
        admission.plan->invocation.updaterOwnedBase,
        admission.plan->invocation.invocationIdentity};
    ProcessFake process;
    HealthFake health;
    FilesystemFake filesystem;
    filesystem.recoveryDisposition = WindowsBootstrapRecoveryDisposition::Rollback;
    filesystem.recoveryIntents = {
        {BootstrapIntentKind::RestoreBackup, "HarkenSearch.exe", 0,
         "restart-rollback"}};

    const auto result = RunBootstrapApplication(
        admission, roots, {process, health, filesystem},
        BootstrapApplicationOptions{60'000U, 30'000U, 1U, true});

    check(result.status == BootstrapApplicationStatus::Quarantined,
          "second recovery rollback quarantines instead of retrying indefinitely");
    check(std::count(filesystem.intents.begin(), filesystem.intents.end(),
                     BootstrapIntentKind::RecordFailure) == 1 &&
              std::count(filesystem.intents.begin(), filesystem.intents.end(),
                         BootstrapIntentKind::QuarantinePackage) == 1,
          "second recovery rollback records exactly one failure and one quarantine decision");
}

void test_recovery_uses_durable_failure_count_over_replayed_envelope_count() {
    const auto admission = fixtureAdmission();
    WindowsBootstrapFilesystemRoots roots{
        admission.plan->invocation.stagingRoot,
        admission.plan->invocation.transactionRoot,
        admission.plan->invocation.updaterOwnedBase,
        admission.plan->invocation.invocationIdentity};
    ProcessFake process;
    HealthFake health;
    FilesystemFake filesystem;
    filesystem.recoveryDisposition = WindowsBootstrapRecoveryDisposition::Rollback;
    filesystem.durablePreviousFailureCount = 1U;
    filesystem.recoveryIntents = {
        {BootstrapIntentKind::RestoreBackup, "HarkenSearch.exe", 0,
         "restart-rollback"}};

    const auto result = RunBootstrapApplication(
        admission, roots, {process, health, filesystem},
        BootstrapApplicationOptions{60'000U, 30'000U, 0U, true});

    check(result.status == BootstrapApplicationStatus::Quarantined &&
              std::count(filesystem.intents.begin(), filesystem.intents.end(),
                         BootstrapIntentKind::RecordFailure) == 1 &&
              std::count(filesystem.intents.begin(), filesystem.intents.end(),
                         BootstrapIntentKind::QuarantinePackage) == 1,
          "recovery derives the second-failure decision from the authenticated durable winner");
}

void test_recovery_replay_does_not_count_the_same_failure_twice() {
    const auto admission = fixtureAdmission();
    WindowsBootstrapFilesystemRoots roots{
        admission.plan->invocation.stagingRoot,
        admission.plan->invocation.transactionRoot,
        admission.plan->invocation.updaterOwnedBase,
        admission.plan->invocation.invocationIdentity};
    ProcessFake process;
    HealthFake health;
    FilesystemFake filesystem;
    filesystem.recoveryDisposition = WindowsBootstrapRecoveryDisposition::Rollback;
    filesystem.durablePreviousFailureCount = 1U;
    filesystem.durableFailureRecordedForTransaction = true;
    filesystem.recoveryIntents = {
        {BootstrapIntentKind::RestoreBackup, "HarkenSearch.exe", 0,
         "restart-rollback"}};

    const auto result = RunBootstrapApplication(
        admission, roots, {process, health, filesystem},
        BootstrapApplicationOptions{60'000U, 30'000U, 0U, true, true});

    check(result.status == BootstrapApplicationStatus::RolledBack &&
              std::count(filesystem.intents.begin(), filesystem.intents.end(),
                         BootstrapIntentKind::RecordFailure) == 0 &&
              std::count(filesystem.intents.begin(), filesystem.intents.end(),
                         BootstrapIntentKind::QuarantinePackage) == 0,
          "restart replay resumes the same rollback without counting a second package failure");
}

void test_recovery_rollback_rebinds_child_before_restore() {
    const auto admission = fixtureAdmission();
    WindowsBootstrapFilesystemRoots roots{
        admission.plan->invocation.stagingRoot,
        admission.plan->invocation.transactionRoot,
        admission.plan->invocation.updaterOwnedBase,
        admission.plan->invocation.invocationIdentity};
    std::vector<std::string> events;
    ProcessFake process;
    process.events = &events;
    HealthFake health;
    FilesystemFake filesystem;
    filesystem.events = &events;
    filesystem.durableChild = identity(99);
    filesystem.recoveryDisposition = WindowsBootstrapRecoveryDisposition::Rollback;
    filesystem.recoveryIntents = {
        {BootstrapIntentKind::RestoreBackup, "HarkenSearch.exe", 0,
         "restart-rollback"}};

    const auto result = RunBootstrapApplication(
        admission, roots, {process, health, filesystem},
        BootstrapApplicationOptions{60'000U, 30'000U, 0U, true});

    check(result.status == BootstrapApplicationStatus::RolledBack &&
              process.rebindCalled && process.terminated,
          "recovery rollback rebinds and terminates the durable updated child");
    check(events.size() >= 3U && events[0] == "rebind-child" &&
              events[1] == "terminate-child" && events[2] == "restore-backup",
          "child termination precedes identity-bound restore publication");
}

void test_recovery_rollback_child_substitution_quarantines() {
    const auto admission = fixtureAdmission();
    WindowsBootstrapFilesystemRoots roots{
        admission.plan->invocation.stagingRoot,
        admission.plan->invocation.transactionRoot,
        admission.plan->invocation.updaterOwnedBase,
        admission.plan->invocation.invocationIdentity};
    ProcessFake process;
    process.rebindStatus = BootstrapProcessStatus::IdentityMismatch;
    HealthFake health;
    FilesystemFake filesystem;
    filesystem.durableChild = identity(99);
    filesystem.recoveryDisposition = WindowsBootstrapRecoveryDisposition::Rollback;
    filesystem.recoveryIntents = {
        {BootstrapIntentKind::RestoreBackup, "HarkenSearch.exe", 0,
         "restart-rollback"}};

    const auto result = RunBootstrapApplication(
        admission, roots, {process, health, filesystem},
        BootstrapApplicationOptions{60'000U, 30'000U, 0U, true});

    check(result.status == BootstrapApplicationStatus::Quarantined,
          "recovery child identity substitution quarantines before restore");
    check(std::find(filesystem.intents.begin(), filesystem.intents.end(),
                    BootstrapIntentKind::RestoreBackup) == filesystem.intents.end(),
          "identity substitution performs no rollback mutation");
}

void test_recovery_rollback_allows_an_already_absent_durable_child() {
    const auto admission = fixtureAdmission();
    WindowsBootstrapFilesystemRoots roots{
        admission.plan->invocation.stagingRoot,
        admission.plan->invocation.transactionRoot,
        admission.plan->invocation.updaterOwnedBase,
        admission.plan->invocation.invocationIdentity};
    ProcessFake process;
    process.rebindStatus = BootstrapProcessStatus::NotFound;
    HealthFake health;
    FilesystemFake filesystem;
    filesystem.durableChild = identity(99);
    filesystem.recoveryDisposition = WindowsBootstrapRecoveryDisposition::Rollback;
    filesystem.recoveryIntents = {
        {BootstrapIntentKind::RestoreBackup, "HarkenSearch.exe", 0,
         "restart-rollback"}};

    const auto result = RunBootstrapApplication(
        admission, roots, {process, health, filesystem},
        BootstrapApplicationOptions{60'000U, 30'000U, 0U, true});

    check(result.status == BootstrapApplicationStatus::RolledBack &&
              process.rebindCalled && !process.terminated &&
              std::find(filesystem.intents.begin(), filesystem.intents.end(),
                        BootstrapIntentKind::RestoreBackup) !=
                  filesystem.intents.end() &&
              process.launched.size() == 1U,
          "an already absent durable child permits restart rollback and old-app relaunch");
}

void test_recovery_rollback_access_denied_quarantines_before_restore() {
    const auto admission = fixtureAdmission();
    WindowsBootstrapFilesystemRoots roots{
        admission.plan->invocation.stagingRoot,
        admission.plan->invocation.transactionRoot,
        admission.plan->invocation.updaterOwnedBase,
        admission.plan->invocation.invocationIdentity};
    ProcessFake process;
    process.rebindStatus = BootstrapProcessStatus::OpenFailed;
    HealthFake health;
    FilesystemFake filesystem;
    filesystem.durableChild = identity(99);
    filesystem.recoveryDisposition = WindowsBootstrapRecoveryDisposition::Rollback;
    filesystem.recoveryIntents = {
        {BootstrapIntentKind::RestoreBackup, "HarkenSearch.exe", 0,
         "restart-rollback"}};

    const auto result = RunBootstrapApplication(
        admission, roots, {process, health, filesystem},
        BootstrapApplicationOptions{60'000U, 30'000U, 0U, true});

    check(result.status == BootstrapApplicationStatus::Quarantined &&
              std::find(filesystem.intents.begin(), filesystem.intents.end(),
                        BootstrapIntentKind::RestoreBackup) ==
                  filesystem.intents.end() &&
              process.launched.empty(),
          "recovery child access denial quarantines before restore or relaunch");
}

void test_recovery_quarantine_does_not_apply_intents() {
    const auto admission = fixtureAdmission();
    WindowsBootstrapFilesystemRoots roots{
        admission.plan->invocation.stagingRoot,
        admission.plan->invocation.transactionRoot,
        admission.plan->invocation.updaterOwnedBase,
        admission.plan->invocation.invocationIdentity};
    ProcessFake process;
    HealthFake health;
    FilesystemFake filesystem;
    filesystem.recoveryDisposition = WindowsBootstrapRecoveryDisposition::Quarantine;
    filesystem.recoveryIntents = {
        {BootstrapIntentKind::RestoreBackup, "HarkenSearch.exe", 0,
         "ambiguous-recovery"}};

    const auto result = RunBootstrapApplication(
        admission, roots, {process, health, filesystem},
        BootstrapApplicationOptions{60'000U, 30'000U, 0U, true});

    check(result.status == BootstrapApplicationStatus::Quarantined,
          "ambiguous recovery is quarantined");
    check(filesystem.intents.empty(),
          "quarantine returns before applying destructive intents");
}

void test_recovery_awaiting_health_never_launches_second_child() {
    const auto admission = fixtureAdmission();
    WindowsBootstrapFilesystemRoots roots{
        admission.plan->invocation.stagingRoot,
        admission.plan->invocation.transactionRoot,
        admission.plan->invocation.updaterOwnedBase,
        admission.plan->invocation.invocationIdentity};
    ProcessFake process;
    HealthFake health;
    FilesystemFake filesystem;
    filesystem.recoveryDisposition =
        WindowsBootstrapRecoveryDisposition::ContinueForward;
    filesystem.durableChild = identity(99);
    filesystem.durableHealthPhase = "awaiting-health";
    filesystem.recoveryIntents = {
        {BootstrapIntentKind::ReplaceInventoryItem, "HarkenSearch.exe", 0,
         "restart-forward"}};

    const auto result = RunBootstrapApplication(
        admission, roots, {process, health, filesystem},
        BootstrapApplicationOptions{60'000U, 30'000U, 0U, true});

    check(result.status == BootstrapApplicationStatus::Quarantined,
          "restart with an unreattachable health channel quarantines safely");
    check(process.rebindCalled && process.terminated && process.launched.empty(),
          "durable awaiting-health child is rebound/terminated instead of launching a second child");
    check(filesystem.intents.empty(),
          "unreattachable child recovery performs no new filesystem mutation");
}

void test_authenticated_recovery_replaces_unreattachable_health_child() {
    const auto admission = fixtureAdmission();
    WindowsBootstrapFilesystemRoots roots{
        admission.plan->invocation.stagingRoot,
        admission.plan->invocation.transactionRoot,
        admission.plan->invocation.updaterOwnedBase,
        admission.plan->invocation.invocationIdentity};
    std::vector<std::string> events;
    ProcessFake process;
    process.events = &events;
    HealthFake health;
    FilesystemFake filesystem;
    filesystem.events = &events;
    filesystem.recoveryDisposition =
        WindowsBootstrapRecoveryDisposition::ContinueForward;
    filesystem.durableChild = identity(98);
    filesystem.durableHealthPhase = "awaiting-health";

    const auto result = RunBootstrapApplication(
        admission, roots, {process, health, filesystem},
        BootstrapApplicationOptions{60'000U, 30'000U, 0U, true, true});

    check(result.status == BootstrapApplicationStatus::Recovered &&
              process.rebindCalled && process.terminated &&
              filesystem.recoveredHealthRelaunchPrepared &&
              process.suspendedLaunch && process.resumed && process.released &&
              health.awaitCalled && filesystem.healthNonceConsumed,
          "fresh authenticated recovery replaces the unreattachable child and completes a new one-shot health attempt");
    check(events.size() >= 2U && events[0] == "rebind-child" &&
              events[1] == "terminate-child",
          "authenticated recovery terminates the exact durable child before relaunch");
}

void test_committed_recovery_relaunches_an_absent_child_with_fresh_health() {
    const auto admission = fixtureAdmission();
    WindowsBootstrapFilesystemRoots roots{
        admission.plan->invocation.stagingRoot,
        admission.plan->invocation.transactionRoot,
        admission.plan->invocation.updaterOwnedBase,
        admission.plan->invocation.invocationIdentity};
    ProcessFake process;
    process.rebindStatus = BootstrapProcessStatus::NotFound;
    HealthFake health;
    FilesystemFake filesystem;
    std::vector<std::string> events;
    process.events = &events;
    health.events = &events;
    filesystem.events = &events;
    filesystem.recoveryDisposition =
        WindowsBootstrapRecoveryDisposition::CleanupCommitted;
    filesystem.recoveryJournal = completedRecoveryJournal(admission);
    filesystem.durableChild = identity(98U);
    filesystem.durableHealthPhase = "accepted";
    filesystem.recoveryIntents = {
        {BootstrapIntentKind::RemoveBackup, {}, 0,
         "restart-committed-cleanup"}};

    const auto result = RunBootstrapApplication(
        admission, roots, {process, health, filesystem},
        BootstrapApplicationOptions{60'000U, 30'000U, 0U, true, true});

    check(result.status == BootstrapApplicationStatus::Recovered &&
              process.rebindCalled &&
              filesystem.recoveredHealthRelaunchPrepared &&
              process.suspendedLaunch && process.resumed &&
              process.preparedForCommit && process.released &&
              health.awaitCalled && filesystem.healthNonceConsumed &&
              filesystem.terminalJournalRetired,
          "committed recovery relaunches an absent durable child through a fresh authenticated health channel before retiring the journal");
    check(process.validatedExecutables.size() == 1U &&
              process.validatedExecutables.front().absolutePath ==
                  "C:/HarkenSearchTest/Aris/HarkenSearch.exe" &&
              std::find(filesystem.intents.begin(), filesystem.intents.end(),
                        BootstrapIntentKind::RestoreBackup) ==
                  filesystem.intents.end(),
          "committed recovery validates the signed installed image and never rolls committed files back");
    const auto healthEvent = std::find(events.begin(), events.end(),
                                       "health-await");
    const auto cleanupEvent = std::find(events.begin(), events.end(),
                                        "remove-backup");
    const auto releaseEvent = std::find(events.begin(), events.end(),
                                        "release-child");
    const auto invocationCleanupEvent = std::find(
        events.begin(), events.end(), "cleanup-invocation");
    const auto retiredEvent = std::find(events.begin(), events.end(),
                                        "retire-journal");
    check(healthEvent != events.end() && cleanupEvent != events.end() &&
              releaseEvent != events.end() &&
              invocationCleanupEvent != events.end() &&
              retiredEvent != events.end() && healthEvent < cleanupEvent &&
              cleanupEvent < releaseEvent &&
              releaseEvent < invocationCleanupEvent &&
              invocationCleanupEvent < retiredEvent,
          "committed recovery proves fresh availability, releases the child, then cleans residue before retiring the journal");
}

void test_committed_recovery_requires_the_authenticated_rehydrated_journal() {
    const auto admission = fixtureAdmission();
    WindowsBootstrapFilesystemRoots roots{
        admission.plan->invocation.stagingRoot,
        admission.plan->invocation.transactionRoot,
        admission.plan->invocation.updaterOwnedBase,
        admission.plan->invocation.invocationIdentity};
    ProcessFake process;
    process.rebindStatus = BootstrapProcessStatus::NotFound;
    HealthFake health;
    FilesystemFake filesystem;
    filesystem.recoveryDisposition =
        WindowsBootstrapRecoveryDisposition::CleanupCommitted;
    filesystem.durableChild = identity(98U);
    filesystem.durableHealthPhase = "accepted";

    const auto result = RunBootstrapApplication(
        admission, roots, {process, health, filesystem},
        BootstrapApplicationOptions{60'000U, 30'000U, 0U, true, true});

    check(result.status == BootstrapApplicationStatus::Quarantined &&
              process.launched.empty() && !health.awaitCalled &&
              !filesystem.terminalJournalRetired,
          "committed recovery never fabricates completed inventory progress when the authenticated rehydrated journal is absent");
}

void test_committed_recovery_replaces_a_release_ambiguous_live_child() {
    const auto admission = fixtureAdmission();
    WindowsBootstrapFilesystemRoots roots{
        admission.plan->invocation.stagingRoot,
        admission.plan->invocation.transactionRoot,
        admission.plan->invocation.updaterOwnedBase,
        admission.plan->invocation.invocationIdentity};
    ProcessFake process;
    HealthFake health;
    FilesystemFake filesystem;
    filesystem.recoveryDisposition =
        WindowsBootstrapRecoveryDisposition::CleanupCommitted;
    filesystem.recoveryJournal = completedRecoveryJournal(admission);
    filesystem.durableChild = identity(98U);
    filesystem.durableHealthPhase = "accepted";

    const auto result = RunBootstrapApplication(
        admission, roots, {process, health, filesystem},
        BootstrapApplicationOptions{60'000U, 30'000U, 0U, true, true});

    check(result.status == BootstrapApplicationStatus::Recovered &&
              process.rebindCalled && process.terminated &&
              filesystem.recoveredHealthRelaunchPrepared &&
              process.launched == std::vector<std::uint64_t>{99U} &&
              health.awaitCalled && filesystem.terminalJournalRetired,
          "committed recovery replaces a live but release-ambiguous durable child with one fresh contained health-checked child");
}

void test_committed_recovery_launch_failure_quarantines_without_byte_rollback() {
    const auto admission = fixtureAdmission();
    WindowsBootstrapFilesystemRoots roots{
        admission.plan->invocation.stagingRoot,
        admission.plan->invocation.transactionRoot,
        admission.plan->invocation.updaterOwnedBase,
        admission.plan->invocation.invocationIdentity};
    ProcessFake process;
    process.rebindStatus = BootstrapProcessStatus::NotFound;
    process.launchFailure = true;
    HealthFake health;
    FilesystemFake filesystem;
    filesystem.recoveryDisposition =
        WindowsBootstrapRecoveryDisposition::CleanupCommitted;
    filesystem.recoveryJournal = completedRecoveryJournal(admission);
    filesystem.durableChild = identity(98U);
    filesystem.durableHealthPhase = "accepted";
    filesystem.recoveryIntents = {
        {BootstrapIntentKind::RemoveBackup, {}, 0,
         "restart-committed-cleanup"}};

    const auto result = RunBootstrapApplication(
        admission, roots, {process, health, filesystem},
        BootstrapApplicationOptions{60'000U, 30'000U, 0U, true, true});

    check(result.status == BootstrapApplicationStatus::Quarantined &&
              filesystem.recoveredHealthRelaunchPrepared &&
              filesystem.intents ==
                  std::vector<BootstrapIntentKind>{
                      BootstrapIntentKind::QuarantinePackage} &&
              !filesystem.terminalJournalRetired,
          "committed relaunch launch failure records quarantine and retains the recovery journal");
    check(std::count(filesystem.intents.begin(), filesystem.intents.end(),
                     BootstrapIntentKind::RestoreBackup) == 0 &&
              std::count(filesystem.intents.begin(), filesystem.intents.end(),
                         BootstrapIntentKind::RemoveNewItem) == 0 &&
              std::count(filesystem.intents.begin(), filesystem.intents.end(),
                         BootstrapIntentKind::CommitJournal) == 0,
          "committed relaunch launch failure never mutates already committed application bytes");
}

void test_committed_recovery_health_timeout_quarantines_without_byte_rollback() {
    const auto admission = fixtureAdmission();
    WindowsBootstrapFilesystemRoots roots{
        admission.plan->invocation.stagingRoot,
        admission.plan->invocation.transactionRoot,
        admission.plan->invocation.updaterOwnedBase,
        admission.plan->invocation.invocationIdentity};
    ProcessFake process;
    process.rebindStatus = BootstrapProcessStatus::NotFound;
    HealthFake health;
    health.timeout = true;
    FilesystemFake filesystem;
    filesystem.recoveryDisposition =
        WindowsBootstrapRecoveryDisposition::CleanupCommitted;
    filesystem.recoveryJournal = completedRecoveryJournal(admission);
    filesystem.durableChild = identity(98U);
    filesystem.durableHealthPhase = "accepted";
    filesystem.recoveryIntents = {
        {BootstrapIntentKind::RemoveBackup, {}, 0,
         "restart-committed-cleanup"}};

    const auto result = RunBootstrapApplication(
        admission, roots, {process, health, filesystem},
        BootstrapApplicationOptions{60'000U, 30'000U, 0U, true, true});

    check(result.status == BootstrapApplicationStatus::Quarantined &&
              process.terminated && health.awaitCalled && health.closed &&
              filesystem.intents ==
                  std::vector<BootstrapIntentKind>{
                      BootstrapIntentKind::QuarantinePackage} &&
              !filesystem.terminalJournalRetired,
          "committed relaunch health timeout terminates the exact child, quarantines, and retains recovery authority");
    check(std::count(filesystem.intents.begin(), filesystem.intents.end(),
                     BootstrapIntentKind::RestoreBackup) == 0 &&
              std::count(filesystem.intents.begin(), filesystem.intents.end(),
                         BootstrapIntentKind::RemoveNewItem) == 0 &&
              std::count(filesystem.intents.begin(), filesystem.intents.end(),
                         BootstrapIntentKind::CommitJournal) == 0,
          "committed relaunch health timeout never mutates already committed application bytes");
}

void test_terminal_rollback_recovery_replays_previous_application_handoff() {
    const auto admission = fixtureAdmission();
    WindowsBootstrapFilesystemRoots roots{
        admission.plan->invocation.stagingRoot,
        admission.plan->invocation.transactionRoot,
        admission.plan->invocation.updaterOwnedBase,
        admission.plan->invocation.invocationIdentity};
    ProcessFake process;
    HealthFake health;
    FilesystemFake filesystem;
    std::vector<std::string> events;
    filesystem.events = &events;
    filesystem.recoveryDisposition = WindowsBootstrapRecoveryDisposition::Complete;
    filesystem.durableRollbackComplete = true;
    filesystem.durableFailureRecordedForTransaction = true;
    filesystem.durablePreviousFailureCount = 1U;

    const auto result = RunBootstrapApplication(
        admission, roots, {process, health, filesystem},
        BootstrapApplicationOptions{60'000U, 30'000U, 0U, true, true});

    check(result.status == BootstrapApplicationStatus::RolledBack &&
              filesystem.previousApplicationRelaunchStarted &&
              filesystem.previousApplicationIdentity.has_value() &&
              filesystem.previousApplicationRelaunchComplete &&
              process.suspendedLaunch && process.resumed &&
              process.preparedForCommit && process.released &&
              !health.awaitCalled,
          "terminal rollback recovery durably completes the previous-application handoff before returning");
    check(process.validatedExecutableProcessIds ==
                  std::vector<std::uint64_t>{99U} &&
              process.validatedExecutables.size() == 1U &&
              process.validatedExecutables.front().sha256 ==
                  std::string(64U, 'b') &&
              process.validatedExecutables.front().byteSize == 900U,
          "rollback validates the restored entrypoint against its journal-bound original content before resume");
    const auto backupRemoved = std::find(events.begin(), events.end(),
                                         "remove-backup");
    const auto invocationCleaned = std::find(events.begin(), events.end(),
                                             "cleanup-invocation");
    const auto journalRetired = std::find(events.begin(), events.end(),
                                          "retire-journal");
    check(filesystem.invocationCleanupCalled &&
              filesystem.terminalJournalRetired &&
              backupRemoved != events.end() &&
              invocationCleaned != events.end() &&
              journalRetired != events.end() &&
              backupRemoved < invocationCleaned &&
              invocationCleaned < journalRetired,
          "terminal rollback recovery removes backup, cleans invocation residue, then retires the journal");
}

void test_recovery_restarts_an_uncertain_previous_application_identity() {
    const auto admission = fixtureAdmission();
    WindowsBootstrapFilesystemRoots roots{
        admission.plan->invocation.stagingRoot,
        admission.plan->invocation.transactionRoot,
        admission.plan->invocation.updaterOwnedBase,
        admission.plan->invocation.invocationIdentity};
    ProcessFake process;
    HealthFake health;
    FilesystemFake filesystem;
    filesystem.recoveryDisposition = WindowsBootstrapRecoveryDisposition::Complete;
    filesystem.durableRollbackComplete = true;
    filesystem.durableFailureRecordedForTransaction = true;
    filesystem.durablePreviousFailureCount = 1U;
    filesystem.previousApplicationRelaunchStarted = true;
    filesystem.previousApplicationIdentity = identity(222U);

    const auto result = RunBootstrapApplication(
        admission, roots, {process, health, filesystem},
        BootstrapApplicationOptions{60'000U, 30'000U, 0U, true, true});

    check(result.status == BootstrapApplicationStatus::RolledBack &&
              process.rebindCalled && process.terminated &&
              process.suspendedLaunch && process.resumed && process.released,
          "recovery replaces an uncertain journaled previous process with a fresh launch");
    check(process.launched == std::vector<std::uint64_t>{99U} &&
              filesystem.previousApplicationRelaunchComplete,
          "recovery never marks an uncertain pre-resume identity complete in place");
}

void test_authenticated_channel_exposes_bounded_timeout() {
    const auto result = ReadAuthenticatedBootstrapChannel(0U, 0U, 1U);
#if defined(_WIN32)
    check(result.status == BootstrapChannelStatus::ParentIdentityRejected,
          "native channel rejects a missing parent lifetime capability before waiting");
#else
    check(result.status == BootstrapChannelStatus::Unsupported,
          "non-Windows channel remains explicitly unsupported");
#endif
}

void test_parent_image_identity_is_not_bound_to_a_fictional_filename() {
    auto parent = identity(41U);
    parent.imagePath = "C:/Tools/R&D~Audio/updates/HarkenSearchUpdater.exe";
    check(BootstrapParentImageMatchesUpdaterBase(
              parent, "C:/Tools/R&D~Audio/updates"),
          "trusted parent admission accepts a literal tilde in the derived absolute root");
    parent.imagePath = "C:/OtherWritable/HarkenSearchUpdater.exe";
    check(BootstrapParentImageMatchesUpdaterBase(
              parent, "C:/Tools/R&D~Audio/updates"),
          "exact native identity and signer checks, rather than a fictional filename, authenticate the parent");
    parent.valid = false;
    check(!BootstrapParentImageMatchesUpdaterBase(
              parent, "C:/Tools/R&D~Audio/updates"),
          "an invalid parent identity never becomes channel authority");
}

}  // namespace

int main() {
    test_progress_follows_real_operations_and_never_completes_a_failure();
    test_happy_path_and_wait_timeout();
    test_default_invocation_cleanup_reports_committed_update_complete();
    test_forward_only_terminal_failures_never_restore_installed_payload();
    test_forward_only_recovery_requires_reinstall_without_restore_or_relaunch();
    test_transient_forward_access_denied_is_retried_before_rollback();
    test_signed_removal_flows_through_runtime_before_health_commit();
    test_authenticated_failure_count_is_bound_before_durable_publication();
    test_filesystem_begin_failure_after_old_exit_relaunches_exact_untouched_application();
    test_failure_count_binding_failure_after_old_exit_relaunches_exact_untouched_application();
    test_child_ownership_is_released_only_after_commit_publication();
    test_child_ownership_prepare_failure_rolls_back_with_handle();
    test_old_process_exit_between_channel_admission_and_open_is_complete();
    test_old_process_open_failure_remains_mutation_free();
    test_health_failure_owns_rollback_child();
    test_live_rollback_cleanup_failure_preserves_the_journal();
    test_transient_restore_access_denied_is_retried_before_quarantine();
    test_launched_child_identity_is_plan_bound_before_resume();
    test_second_failure_quarantines_instead_of_retrying_forever();
    test_ambiguous_commit_failure_contains_child_for_restart_recovery();
    test_post_commit_cleanup_failure_keeps_healthy_child_for_recovery();
    test_invocation_residue_cleanup_failure_keeps_committed_journal();
    test_journal_retirement_failure_reports_committed_update_complete();
    test_retryable_committed_cleanup_handoff_defers_retirement();
    test_retryable_committed_cleanup_without_successor_reports_update_complete();
    test_recovery_without_original_health_nonce_quarantines();
    test_prepared_forward_recovery_quarantines_without_publishing_rollback();
    test_authenticated_recovery_capability_can_finish_forward_once();
    test_authenticated_recovery_retries_transient_forward_denial();
    test_recovery_rollback_records_exactly_one_failure();
    test_recovery_rollback_retries_transient_restore_denial();
    test_recovery_defers_backup_cleanup_until_previous_app_is_running();
    test_recovery_rollback_quarantines_on_second_failure();
    test_recovery_uses_durable_failure_count_over_replayed_envelope_count();
    test_recovery_replay_does_not_count_the_same_failure_twice();
    test_recovery_rollback_rebinds_child_before_restore();
    test_recovery_rollback_child_substitution_quarantines();
    test_recovery_rollback_allows_an_already_absent_durable_child();
    test_recovery_rollback_access_denied_quarantines_before_restore();
    test_recovery_quarantine_does_not_apply_intents();
    test_recovery_awaiting_health_never_launches_second_child();
    test_authenticated_recovery_replaces_unreattachable_health_child();
    test_committed_recovery_relaunches_an_absent_child_with_fresh_health();
    test_committed_recovery_requires_the_authenticated_rehydrated_journal();
    test_committed_recovery_replaces_a_release_ambiguous_live_child();
    test_committed_recovery_launch_failure_quarantines_without_byte_rollback();
    test_committed_recovery_health_timeout_quarantines_without_byte_rollback();
    test_terminal_rollback_recovery_replays_previous_application_handoff();
    test_recovery_restarts_an_uncertain_previous_application_identity();
    test_authenticated_channel_exposes_bounded_timeout();
    test_parent_image_identity_is_not_bound_to_a_fictional_filename();
    if (failures != 0) {
        std::cerr << failures << " assertion(s) failed\n";
        return 1;
    }
    std::cout << "HarkenSearchBootstrap runtime tests passed\n";
    return 0;
}
