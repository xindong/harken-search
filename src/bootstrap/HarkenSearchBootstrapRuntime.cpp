#include "bootstrap/HarkenSearchBootstrapRuntime.h"

#include <algorithm>
#include <chrono>
#if defined(ARIS_BOOTSTRAP_TEST_DIAGNOSTICS) || \
    (defined(ARIS_UPDATE_LOCAL_DEMO) && ARIS_UPDATE_LOCAL_DEMO)
#include <cstdio>
#endif
#if defined(ARIS_BOOTSTRAP_TEST_CRASH_AFTER_CHILD_CHECKPOINT) || \
    defined(ARIS_BOOTSTRAP_TEST_CRASH_AFTER_PREVIOUS_RELEASE) || \
    defined(ARIS_BOOTSTRAP_TEST_CRASH_AFTER_COMMIT) || \
    defined(ARIS_BOOTSTRAP_TEST_CRASH_AFTER_COMMITTED_CLEANUP) || \
    defined(ARIS_BOOTSTRAP_TEST_CRASH_AFTER_COMMITTED_RELEASE)
#include <cstdlib>
#endif
#include <string>
#include <optional>
#include <utility>
#include <thread>

namespace aris::bootstrap {

std::string WindowsBootstrapFilesystemDriver::RecoveredHealthPhase() const {
    return port_.recoveredHealthPhase();
}

namespace {

void reportProgress(const BootstrapApplicationDependencies& dependencies,
                    BootstrapProgressPhase phase, std::size_t completed = 0,
                    std::size_t total = 0) noexcept {
    if (dependencies.progress != nullptr) {
        dependencies.progress->Report({phase, completed, total});
    }
}

void testDiagnostic(const char* message) {
#ifdef ARIS_BOOTSTRAP_TEST_DIAGNOSTICS
    std::fputs(message, stderr);
    std::fflush(stderr);
#else
    static_cast<void>(message);
#endif
}

#ifdef ARIS_BOOTSTRAP_TEST_DIAGNOSTICS
void writeFilesystemFailureDiagnostic(
    const std::string& transactionRoot,
    const BootstrapIntent& intent,
    const WindowsBootstrapFilesystemResult& result) {
    std::FILE* diagnostic = nullptr;
    const std::string path =
        transactionRoot + "/bootstrap-test-diagnostic.txt";
    if (fopen_s(&diagnostic, path.c_str(), "ab") != 0 ||
        diagnostic == nullptr) {
        return;
    }
    std::fprintf(diagnostic,
                 "intent=%d path=%s status=%d backend=%d native=%u\n",
                 static_cast<int>(intent.kind), intent.relativePath.c_str(),
                 static_cast<int>(result.status),
                 static_cast<int>(result.backendStatus), result.nativeError);
    std::fclose(diagnostic);
}
#endif

#if defined(ARIS_UPDATE_LOCAL_DEMO) && ARIS_UPDATE_LOCAL_DEMO
void writeLocalDemoBeginDiagnostic(
    const WindowsBootstrapFilesystemResult& result,
    const char* outcome) {
    std::FILE* diagnostic = nullptr;
    const std::string path =
        std::string(ARIS_UPDATE_LOCAL_DEMO_ROOT) +
        "/bootstrap-demo-begin.txt";
    if (fopen_s(&diagnostic, path.c_str(), "wb") != 0 ||
        diagnostic == nullptr) {
        return;
    }
    std::fprintf(diagnostic,
                 "outcome=%s status=%d backend=%d native=%u\n",
                 outcome,
                 static_cast<int>(result.status),
                 static_cast<int>(result.backendStatus),
                 result.nativeError);
    std::fclose(diagnostic);
}

void writeLocalDemoMutationDiagnostic(
    const BootstrapIntentKind intent,
    const WindowsBootstrapFilesystemResult& result) {
    std::FILE* diagnostic = nullptr;
    const std::string path =
        std::string(ARIS_UPDATE_LOCAL_DEMO_ROOT) +
        "/bootstrap-demo-mutation.txt";
    if (fopen_s(&diagnostic, path.c_str(), "wb") != 0 ||
        diagnostic == nullptr) {
        return;
    }
    std::fprintf(diagnostic,
                 "intent=%d status=%d backend=%d native=%u\n",
                 static_cast<int>(intent),
                 static_cast<int>(result.status),
                 static_cast<int>(result.backendStatus),
                 result.nativeError);
    std::fclose(diagnostic);
}

void writeLocalDemoCleanupDiagnostic(
    const WindowsBootstrapFilesystemResult& result) {
    std::FILE* diagnostic = nullptr;
    const std::string path =
        std::string(ARIS_UPDATE_LOCAL_DEMO_ROOT) +
        "/bootstrap-demo-cleanup.txt";
    if (fopen_s(&diagnostic, path.c_str(), "wb") != 0 ||
        diagnostic == nullptr) {
        return;
    }
    std::fprintf(diagnostic,
                 "outcome=%s status=%d backend=%d native=%u\n",
                 result.ok() ? "ok" : "failed",
                 static_cast<int>(result.status),
                 static_cast<int>(result.backendStatus),
                 result.nativeError);
    std::fclose(diagnostic);
}

void writeLocalDemoSuccessorDiagnostic(
    const BootstrapTerminalCleanupResult& result) {
    std::FILE* diagnostic = nullptr;
    const std::string path =
        std::string(ARIS_UPDATE_LOCAL_DEMO_ROOT) +
        "/bootstrap-demo-successor.txt";
    if (fopen_s(&diagnostic, path.c_str(), "wb") != 0 ||
        diagnostic == nullptr) {
        return;
    }
    std::fprintf(diagnostic, "status=%d native=%u\n",
                 static_cast<int>(result.status), result.nativeError);
    std::fclose(diagnostic);
}
#endif

WindowsBootstrapFilesystemResult cleanupCommittedInvocationWithDiagnostics(
    BootstrapFilesystemDriver& filesystem) {
    const WindowsBootstrapFilesystemResult result =
        filesystem.CleanupCommittedInvocation();
#if defined(ARIS_UPDATE_LOCAL_DEMO) && ARIS_UPDATE_LOCAL_DEMO
    writeLocalDemoCleanupDiagnostic(result);
#endif
    return result;
}

bool isFilesystemIntent(const BootstrapIntentKind kind) {
    switch (kind) {
        case BootstrapIntentKind::PersistJournal:
        case BootstrapIntentKind::SnapshotBackup:
        case BootstrapIntentKind::ReplaceInventoryItem:
        case BootstrapIntentKind::RemoveInstalledItem:
        case BootstrapIntentKind::CommitJournal:
        case BootstrapIntentKind::RemoveBackup:
        case BootstrapIntentKind::RestoreBackup:
        case BootstrapIntentKind::RemoveNewItem:
        case BootstrapIntentKind::RecordFailure:
        case BootstrapIntentKind::QuarantinePackage:
            return true;
        default:
            return false;
    }
}

WindowsBootstrapFilesystemResult applyFilesystemIntentWithBoundedRetry(
    BootstrapFilesystemDriver& filesystem,
    const BootstrapIntent& intent) {
    constexpr std::uint32_t kMaximumAttempts = 50U;
    constexpr auto kRetryDelay = std::chrono::milliseconds(10);
    WindowsBootstrapFilesystemResult result;
    for (std::uint32_t attempt = 0U; attempt < kMaximumAttempts; ++attempt) {
        result = filesystem.Apply(intent);
        if (result.status !=
            WindowsBootstrapFilesystemStatus::RetryableAccessDenied) {
            return result;
        }
        if (attempt + 1U < kMaximumAttempts) {
            std::this_thread::sleep_for(kRetryDelay);
        }
    }
    return result;
}

std::string executablePath(const ValidatedInstallPlan& plan) {
    if (plan.identity.targetRoot.empty() || plan.entrypoint.empty()) return {};
    return plan.identity.targetRoot + "/" + plan.entrypoint;
}

std::string normalizeExecutablePath(std::string_view value) {
    if (value.rfind("\\\\?\\", 0U) == 0U) value.remove_prefix(4U);
    std::string output;
    output.reserve(value.size());
    for (const char raw : value) {
        char character = raw == '\\' ? '/' : raw;
        if (character >= 'A' && character <= 'Z') {
            character = static_cast<char>(character - 'A' + 'a');
        }
        output.push_back(character);
    }
    while (output.size() > 3U && output.back() == '/') output.pop_back();
    return output;
}

bool sameExecutablePath(std::string_view left, std::string_view right) {
    return !left.empty() && !right.empty() &&
           normalizeExecutablePath(left) == normalizeExecutablePath(right);
}

std::optional<BootstrapExecutableExpectation> expectedExecutable(
    const ValidatedInstallPlan& plan) {
    const std::string path = executablePath(plan);
    if (path.empty()) return std::nullopt;
    for (const InstallInventoryItem& item : plan.inventory) {
        if (item.relativePath == plan.entrypoint) {
            BootstrapExecutableExpectation result;
            result.absolutePath = path;
            result.sha256 = item.sha256;
            result.byteSize = item.byteSize;
            return result.valid() ? std::optional(result) : std::nullopt;
        }
    }
    return std::nullopt;
}

std::optional<BootstrapExecutableExpectation> expectedBootstrapExecutable(
    const ValidatedInstallPlan& plan) {
    if (plan.identity.targetRoot.empty()) return std::nullopt;
    for (const InstallInventoryItem& item : plan.inventory) {
        std::string relative = item.relativePath;
        std::replace(relative.begin(), relative.end(), '\\', '/');
        if (relative != "HarkenSearchBootstrap.exe") continue;
        BootstrapExecutableExpectation result;
        result.absolutePath = plan.identity.targetRoot + "/" + relative;
        result.sha256 = item.sha256;
        result.byteSize = item.byteSize;
        return result.valid() ? std::optional(result) : std::nullopt;
    }
    return std::nullopt;
}

BootstrapTerminalCleanupResult launchTerminalCleanupSuccessor(
    const ValidatedInstallPlan& plan,
    const ReplacementJournal& journal,
    BootstrapApplicationDependencies dependencies) {
    if (dependencies.terminalCleanup == nullptr) {
        return {BootstrapTerminalCleanupStatus::Unsupported};
    }
    const auto expected = expectedBootstrapExecutable(plan);
    if (!expected.has_value()) {
        return {BootstrapTerminalCleanupStatus::InvalidIdentity};
    }
    return dependencies.terminalCleanup->LaunchSuccessor(plan, journal, *expected);
}

std::string fingerprintHex(const WindowsFileContentFingerprint& fingerprint) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(fingerprint.sha256.size() * 2U);
    for (const std::uint8_t byte : fingerprint.sha256) {
        result.push_back(digits[byte >> 4U]);
        result.push_back(digits[byte & 0x0fU]);
    }
    return result;
}

std::optional<BootstrapExecutableExpectation> expectedPreviousExecutable(
    const ValidatedInstallPlan& plan,
    const BootstrapFilesystemDriver& filesystem) {
    const auto content = filesystem.PreviousApplicationContentFingerprint();
    if (!content.has_value() || !content->valid) return std::nullopt;
    BootstrapExecutableExpectation result;
    result.absolutePath = executablePath(plan);
    result.sha256 = fingerprintHex(*content);
    result.byteSize = content->byteSize;
    return result.valid() ? std::optional(result) : std::nullopt;
}

struct PreviousApplicationRelaunchResult {
    bool completed = false;
    std::string reason;
};

PreviousApplicationRelaunchResult relaunchUntouchedPreviousApplication(
    const ValidatedInstallPlan& plan,
    BootstrapApplicationDependencies dependencies,
    const BootstrapExecutableExpectation& expectedImage) {
    if (!expectedImage.valid() || !expectedImage.exactFileIdentity.valid ||
        !sameExecutablePath(expectedImage.absolutePath, executablePath(plan))) {
        return {false, "previous-application-expectation-missing"};
    }
    BootstrapProcessResult launched = dependencies.process.LaunchSuspended(
        executablePath(plan), plan.identity.targetRoot, {});
    if (!launched.ok() || !launched.process.valid()) {
        if (launched.process.valid()) {
            static_cast<void>(dependencies.process.TerminateOwned(
                launched.process, 0xE001U));
            dependencies.process.Close(launched.process);
        }
        return {false, "previous-application-launch-failed"};
    }

    BootstrapOwnedProcess previous = launched.process;
    const auto rejectOwned = [&dependencies, &previous](const char* reason) {
        static_cast<void>(dependencies.process.TerminateOwned(previous, 0xE001U));
        if (previous.valid()) dependencies.process.Close(previous);
        return PreviousApplicationRelaunchResult{false, reason};
    };
    if (!sameExecutablePath(previous.identity.imagePath,
                            expectedImage.absolutePath)) {
        return rejectOwned("previous-application-identity-mismatch");
    }
    const BootstrapProcessResult imageValidated =
        dependencies.process.ValidateOwnedExecutable(previous, expectedImage);
    if (!imageValidated.ok()) {
        return rejectOwned("previous-application-content-mismatch");
    }
    const BootstrapProcessResult resumed =
        dependencies.process.ResumeOwned(previous);
    if (!resumed.ok()) {
        return rejectOwned("previous-application-resume-failed");
    }
    const BootstrapProcessResult prepared =
        dependencies.process.PrepareOwnedForCommit(previous);
    if (!prepared.ok()) {
        return rejectOwned("previous-application-ownership-prepare-failed");
    }
    const BootstrapProcessResult released =
        dependencies.process.ReleaseOwned(previous);
    if (!released.ok()) {
        if (previous.valid()) {
            static_cast<void>(dependencies.process.TerminateOwned(previous,
                                                                   0xE001U));
            dependencies.process.Close(previous);
        }
        return {false, "previous-application-release-failed"};
    }
    return {true, "previous-application-relaunched"};
}

BootstrapApplicationResult rejectFilesystemAfterOldExit(
    const ValidatedInstallPlan& plan,
    BootstrapApplicationDependencies dependencies,
    const BootstrapExecutableExpectation& previousExecutable,
    const std::string& reason) {
    const PreviousApplicationRelaunchResult relaunched =
        relaunchUntouchedPreviousApplication(plan, dependencies,
                                             previousExecutable);
    if (!relaunched.completed) {
        return {BootstrapApplicationStatus::LaunchRejected,
                reason + "-" + relaunched.reason, false};
    }
    testDiagnostic("runtime:pre-mutation-relaunch-ok\n");
    return {BootstrapApplicationStatus::FilesystemRejected,
            reason + "-previous-application-relaunched", false};
}

PreviousApplicationRelaunchResult ensurePreviousApplicationRelaunched(
    const ValidatedInstallPlan& plan,
    BootstrapApplicationDependencies dependencies) {
    if (dependencies.filesystem.
            RecoveredPreviousApplicationRelaunchComplete()) {
        return {true, "previous-application-already-relaunched"};
    }
    const auto expectedImage = expectedPreviousExecutable(
        plan, dependencies.filesystem);
    if (!expectedImage.has_value()) {
        return {false, "previous-application-expectation-missing"};
    }
    if (!dependencies.filesystem.
             RecoveredPreviousApplicationRelaunchStarted()) {
        const WindowsBootstrapFilesystemResult started =
            dependencies.filesystem.
                RecordPreviousApplicationRelaunchStarted();
        if (!started.ok()) {
            return {false, "previous-application-start-checkpoint-failed-" +
                               std::to_string(
                                   static_cast<int>(started.status))};
        }
    }

    BootstrapOwnedProcess previous;
    bool launchedNow = false;
    const std::optional<BootstrapProcessIdentity> durablePrevious =
        dependencies.filesystem.RecoveredPreviousApplicationIdentity();
    if (durablePrevious.has_value()) {
        BootstrapProcessResult rebound =
            dependencies.process.RebindOwnedChild(*durablePrevious);
        if (rebound.status == BootstrapProcessStatus::NotFound) {
            const WindowsBootstrapFilesystemResult reset =
                dependencies.filesystem.ResetPreviousApplicationIdentity(
                    *durablePrevious);
            if (!reset.ok()) {
                return {false, "previous-application-reset-failed"};
            }
        } else if (!rebound.ok() || !rebound.process.valid()) {
            if (rebound.process.valid()) {
                dependencies.process.Close(rebound.process);
            }
            return {false, "previous-application-rebind-failed"};
        } else {
            testDiagnostic("runtime:previous-rebind-ok\n");
            BootstrapOwnedProcess uncertainPrevious = rebound.process;
            const BootstrapProcessResult terminated =
                dependencies.process.TerminateOwned(uncertainPrevious, 0xE001U);
            if (!terminated.ok()) {
                if (uncertainPrevious.valid()) {
                    dependencies.process.Close(uncertainPrevious);
                }
                return {false,
                        "previous-application-uncertain-termination-failed"};
            }
            if (uncertainPrevious.valid()) {
                dependencies.process.Close(uncertainPrevious);
            }
            const WindowsBootstrapFilesystemResult reset =
                dependencies.filesystem.ResetPreviousApplicationIdentity(
                    *durablePrevious);
            if (!reset.ok()) {
                return {false, "previous-application-reset-failed"};
            }
        }
    }

    if (!previous.valid()) {
        const BootstrapProcessResult launched =
            dependencies.process.LaunchSuspended(
                executablePath(plan), plan.identity.targetRoot, {});
        if (!launched.ok() || !launched.process.valid()) {
            return {false, "previous-application-launch-failed"};
        }
        previous = launched.process;
        launchedNow = true;
        if (!sameExecutablePath(previous.identity.imagePath,
                                executablePath(plan))) {
            static_cast<void>(dependencies.process.TerminateOwned(previous,
                                                                   0xE001U));
            dependencies.process.Close(previous);
            return {false, "previous-application-identity-mismatch"};
        }
        const BootstrapProcessResult imageValidated =
            dependencies.process.ValidateOwnedExecutable(previous,
                                                         *expectedImage);
        if (!imageValidated.ok()) {
            static_cast<void>(dependencies.process.TerminateOwned(previous,
                                                                   0xE001U));
            dependencies.process.Close(previous);
            return {false, "previous-application-content-mismatch"};
        }
        const WindowsBootstrapFilesystemResult identityRecorded =
            dependencies.filesystem.RecordPreviousApplicationIdentity(
                previous.identity);
        if (!identityRecorded.ok()) {
            static_cast<void>(dependencies.process.TerminateOwned(previous,
                                                                   0xE001U));
            dependencies.process.Close(previous);
            return {false, "previous-application-identity-checkpoint-failed"};
        }
        const BootstrapProcessResult resumed =
            dependencies.process.ResumeOwned(previous);
        if (!resumed.ok()) {
            static_cast<void>(dependencies.process.TerminateOwned(previous,
                                                                   0xE001U));
            dependencies.process.Close(previous);
            return {false, "previous-application-resume-failed"};
        }
    }

    if (!launchedNow) {
        const BootstrapProcessResult imageValidated =
            dependencies.process.ValidateOwnedExecutable(previous,
                                                         *expectedImage);
        if (!imageValidated.ok()) {
            dependencies.process.Close(previous);
            return {false, "previous-application-content-mismatch"};
        }
    }

    const BootstrapProcessIdentity previousIdentity = previous.identity;
    const BootstrapProcessResult prepared =
        dependencies.process.PrepareOwnedForCommit(previous);
    if (!prepared.ok()) {
        if (launchedNow) {
            static_cast<void>(dependencies.process.TerminateOwned(previous,
                                                                   0xE001U));
        }
        dependencies.process.Close(previous);
        return {false, "previous-application-ownership-prepare-failed"};
    }
    const BootstrapProcessResult released =
        dependencies.process.ReleaseOwned(previous);
    if (!released.ok()) {
        if (previous.valid()) {
            static_cast<void>(dependencies.process.TerminateOwned(previous,
                                                                   0xE001U));
            dependencies.process.Close(previous);
        }
        return {false, "previous-application-release-failed"};
    }
#ifdef ARIS_BOOTSTRAP_TEST_CRASH_AFTER_PREVIOUS_RELEASE
    if (launchedNow && !durablePrevious.has_value() &&
        plan.healthToken == std::string(64U, 'e')) {
        std::_Exit(0xE3);
    }
#endif
    const WindowsBootstrapFilesystemResult completed =
        dependencies.filesystem.RecordPreviousApplicationRelaunchComplete(
            previousIdentity);
    return completed.ok()
        ? PreviousApplicationRelaunchResult{
              true, "previous-application-relaunched"}
        : PreviousApplicationRelaunchResult{
              false, "previous-application-completion-checkpoint-failed"};
}

BootstrapApplicationResult applyRollback(
    const ValidatedInstallPlan& plan,
    BootstrapApplicationDependencies dependencies,
    BootstrapOwnedProcess* child,
    const std::string& reason,
    const TransactionState& state,
    bool mutated) {
#ifdef ARIS_BOOTSTRAP_TEST_DIAGNOSTICS
    std::fprintf(stderr, "runtime:rollback-reason=%s\n", reason.c_str());
    std::fflush(stderr);
#endif
    if (child != nullptr && child->valid()) {
        const BootstrapProcessResult terminated =
            dependencies.process.TerminateOwned(*child, 0xE001U);
        if (!terminated.ok()) {
#ifdef ARIS_BOOTSTRAP_TEST_DIAGNOSTICS
            std::fprintf(stderr, "runtime:rollback-terminate-status=%d native=%u\n",
                         static_cast<int>(terminated.status),
                         terminated.nativeError);
            std::fflush(stderr);
#endif
            dependencies.health.Close();
            return {BootstrapApplicationStatus::Quarantined,
                    "owned-child-termination-failed", mutated};
        }
    }

    const TransactionDecision rollbackDecision = RollbackTransaction(state);
    if (rollbackDecision.state.phase == TransactionPhase::RecoveryBlocked) {
        dependencies.health.Close();
        return {BootstrapApplicationStatus::Quarantined, reason + "-state", mutated};
    }
    bool quarantineRequested = false;
    bool previousApplicationRelaunched = false;
    for (const BootstrapIntent& intent : rollbackDecision.intents) {
#ifdef ARIS_BOOTSTRAP_TEST_DIAGNOSTICS
        std::fprintf(stderr, "runtime:rollback-intent=%d\n",
                     static_cast<int>(intent.kind));
        std::fflush(stderr);
#endif
        if (intent.kind == BootstrapIntentKind::QuarantinePackage) {
            quarantineRequested = true;
            continue;
        }
        if (!isFilesystemIntent(intent.kind)) continue;
        const WindowsBootstrapFilesystemResult applied =
            applyFilesystemIntentWithBoundedRetry(
                dependencies.filesystem, intent);
#ifdef ARIS_BOOTSTRAP_TEST_DIAGNOSTICS
        std::fprintf(stderr, "runtime:rollback-apply=%d status=%d backend=%d native=%u\n",
                     static_cast<int>(intent.kind),
                     static_cast<int>(applied.status),
                     static_cast<int>(applied.backendStatus),
                     applied.nativeError);
        std::fflush(stderr);
#endif
        if (!applied.ok()) {
            dependencies.health.Close();
            return {BootstrapApplicationStatus::Quarantined,
                    reason + "-rollback-filesystem-failed", mutated};
        }
    }

    // Relaunch the previous executable only after the durable restore.  The
    // relaunch itself is journaled and the suspended process identity is
    // rebound after a crash, so cleanup cannot become terminal while no app is
    // running.
    for (const BootstrapIntent& intent : rollbackDecision.intents) {
        if (intent.kind != BootstrapIntentKind::RelaunchPreviousApplication) continue;
#ifdef ARIS_BOOTSTRAP_TEST_DIAGNOSTICS
        testDiagnostic("runtime:rollback-relaunch-begin\n");
#endif
        const PreviousApplicationRelaunchResult relaunched =
            ensurePreviousApplicationRelaunched(plan, dependencies);
#ifdef ARIS_BOOTSTRAP_TEST_DIAGNOSTICS
        std::fprintf(stderr, "runtime:rollback-relaunch-result=%d reason=%s\n",
                     relaunched.completed ? 1 : 0, relaunched.reason.c_str());
        std::fflush(stderr);
#endif
        if (!relaunched.completed) {
            dependencies.health.Close();
            return {BootstrapApplicationStatus::Quarantined,
                    reason + "-" + relaunched.reason, mutated};
        }
        previousApplicationRelaunched = true;
    }
    if (mutated && previousApplicationRelaunched) {
        const WindowsBootstrapFilesystemResult cleaned =
            dependencies.filesystem.Apply(
                {BootstrapIntentKind::RemoveBackup, {}, 0,
                 "rollback-relaunch-complete"});
        if (!cleaned.ok()) {
            dependencies.health.Close();
            return {BootstrapApplicationStatus::Quarantined,
                    reason + "-rollback-cleanup-failed", true};
        }
        const WindowsBootstrapFilesystemResult invocationCleaned =
            cleanupCommittedInvocationWithDiagnostics(dependencies.filesystem);
        if (!invocationCleaned.ok()) {
            dependencies.health.Close();
            return {BootstrapApplicationStatus::Quarantined,
                    reason + "-rollback-invocation-cleanup-pending", true};
        }
        if (quarantineRequested) {
            const WindowsBootstrapFilesystemResult quarantined =
                dependencies.filesystem.Apply(
                    {BootstrapIntentKind::QuarantinePackage, {}, 0,
                     "rollback-second-failure"});
            if (!quarantined.ok()) {
                dependencies.health.Close();
                return {BootstrapApplicationStatus::Quarantined,
                        reason + "-quarantine-record-failed", true};
            }
        } else {
            const WindowsBootstrapFilesystemResult retired =
                dependencies.filesystem.RetireTerminalJournal();
            if (!retired.ok()) {
                dependencies.health.Close();
                return {BootstrapApplicationStatus::Quarantined,
                        reason + "-journal-retirement-failed", true};
            }
        }
    }
    dependencies.health.Close();
    return {quarantineRequested ? BootstrapApplicationStatus::Quarantined
                                : BootstrapApplicationStatus::RolledBack,
            quarantineRequested ? reason + "-quarantined" : reason, mutated};
}

BootstrapApplicationResult quarantineCommittedRecovery(
    BootstrapApplicationDependencies dependencies,
    BootstrapOwnedProcess* child,
    const std::string& reason) {
    if (child != nullptr && child->valid()) {
        const BootstrapProcessResult terminated =
            dependencies.process.TerminateOwned(*child, 0xE001U);
        if (child->valid()) dependencies.process.Close(*child);
        if (!terminated.ok()) {
            dependencies.health.Close();
            return {BootstrapApplicationStatus::Quarantined,
                    "committed-child-termination-failed", true};
        }
    }
    const WindowsBootstrapFilesystemResult quarantined =
        dependencies.filesystem.Apply(
            {BootstrapIntentKind::QuarantinePackage, {}, 0, reason});
    dependencies.health.Close();
    return {BootstrapApplicationStatus::Quarantined,
            quarantined.ok() ? reason
                             : "committed-recovery-quarantine-record-failed",
            true};
}

bool acceptedOwnedHealthAcknowledgement(
    const ValidatedInstallPlan& plan,
    const ReplacementJournal& journal,
    const BootstrapOwnedProcess& child,
    const HealthAcknowledgement& acknowledgement) {
    if (!journal.isValidatedFor(plan) || !child.valid()) return false;
    TransactionState state;
    state.plan = plan;
    state.journal = journal;
    state.phase = TransactionPhase::AwaitingHealth;
    state.updatedChildProcessId = child.identity.processId;
    return AcknowledgeHealth(state, acknowledgement).health ==
           HealthAcknowledgementResult::Accepted;
}

BootstrapApplicationResult recoverCommittedApplication(
    const ValidatedInstallPlan& plan,
    const ReplacementJournal& journal,
    BootstrapApplicationDependencies dependencies,
    const BootstrapApplicationOptions& options,
    const std::vector<BootstrapIntent>& cleanupIntents) {
    const auto expectedImage = expectedExecutable(plan);
    if (!expectedImage.has_value()) {
        return quarantineCommittedRecovery(
            dependencies, nullptr,
            "committed-recovery-expectation-missing");
    }

    const std::optional<BootstrapProcessIdentity> durableChild =
        dependencies.filesystem.RecoveredUpdatedChildIdentity();
    const std::string durableHealthPhase =
        dependencies.filesystem.RecoveredHealthPhase();
    if (durableChild.has_value()) {
        if (durableHealthPhase != "accepted" &&
            durableHealthPhase != "awaiting-health") {
            return quarantineCommittedRecovery(
                dependencies, nullptr,
                "committed-recovery-health-state-invalid");
        }
        BootstrapProcessResult rebound =
            dependencies.process.RebindOwnedChild(*durableChild);
        if (rebound.status != BootstrapProcessStatus::NotFound) {
            if (!rebound.ok() || !rebound.process.valid()) {
                if (rebound.process.valid()) {
                    dependencies.process.Close(rebound.process);
                }
                return quarantineCommittedRecovery(
                    dependencies, nullptr,
                    "committed-recovery-child-rebind-rejected");
            }
            const BootstrapProcessResult terminated =
                dependencies.process.TerminateOwned(rebound.process, 0xE001U);
            if (rebound.process.valid()) {
                dependencies.process.Close(rebound.process);
            }
            if (!terminated.ok()) {
                return quarantineCommittedRecovery(
                    dependencies, nullptr,
                    "committed-recovery-child-termination-failed");
            }
        }
        const WindowsBootstrapFilesystemResult relaunchPrepared =
            dependencies.filesystem.PrepareRecoveredHealthRelaunch(
                *durableChild);
        if (!relaunchPrepared.ok()) {
            return quarantineCommittedRecovery(
                dependencies, nullptr,
                "committed-recovery-relaunch-checkpoint-failed");
        }
    } else if (!durableHealthPhase.empty()) {
        return quarantineCommittedRecovery(
            dependencies, nullptr,
            "committed-recovery-child-state-ambiguous");
    }

    if (!options.recoveryHealthCapabilityAvailable) {
        return quarantineCommittedRecovery(
            dependencies, nullptr,
            "committed-recovery-health-capability-unavailable");
    }

    BootstrapLaunchCapability capability;
    const BootstrapHealthResult channel = dependencies.health.Prepare(
        plan.healthToken, plan.version, plan.buildId, capability);
    if (!channel.ok()) {
        return quarantineCommittedRecovery(
            dependencies, nullptr,
            "committed-recovery-health-prepare-failed");
    }
    BootstrapProcessResult launched = dependencies.process.LaunchSuspended(
        executablePath(plan), plan.identity.targetRoot, capability);
    dependencies.health.ReleaseLaunchCapability();
    if (!launched.ok() || !launched.process.valid()) {
        return quarantineCommittedRecovery(
            dependencies, nullptr,
            "committed-recovery-launch-failed");
    }
    BootstrapOwnedProcess child = launched.process;
    if (!sameExecutablePath(child.identity.imagePath,
                            expectedImage->absolutePath) ||
        !dependencies.process.ValidateOwnedExecutable(child, *expectedImage).ok()) {
        return quarantineCommittedRecovery(
            dependencies, &child,
            "committed-recovery-child-content-mismatch");
    }
    const WindowsBootstrapFilesystemResult childCheckpoint =
        dependencies.filesystem.RecordUpdatedChildIdentity(child.identity);
    if (!childCheckpoint.ok()) {
        return quarantineCommittedRecovery(
            dependencies, &child,
            "committed-recovery-child-checkpoint-failed");
    }
    if (!dependencies.process.ResumeOwned(child).ok()) {
        return quarantineCommittedRecovery(
            dependencies, &child,
            "committed-recovery-child-resume-failed");
    }
    const BootstrapHealthResult health = dependencies.health.Await(
        child, plan.healthToken, plan.version, plan.buildId,
        options.healthWaitMs);
    if (!health.ok() ||
        !acceptedOwnedHealthAcknowledgement(plan, journal, child,
                                            health.acknowledgement)) {
        return quarantineCommittedRecovery(
            dependencies, &child,
            health.status == BootstrapHealthStatus::TimedOut
                ? "committed-recovery-health-timeout"
                : "committed-recovery-health-rejected");
    }
    const WindowsBootstrapFilesystemResult healthCheckpoint =
        dependencies.filesystem.RecordHealthState("accepted", true);
    if (!healthCheckpoint.ok()) {
        return quarantineCommittedRecovery(
            dependencies, &child,
            "committed-recovery-health-checkpoint-failed");
    }
    testDiagnostic("runtime:committed-recovery-health-accepted\n");
    if (!dependencies.process.PrepareOwnedForCommit(child).ok()) {
        return quarantineCommittedRecovery(
            dependencies, &child,
            "committed-recovery-ownership-prepare-failed");
    }
    for (const BootstrapIntent& intent : cleanupIntents) {
        if (intent.kind != BootstrapIntentKind::RemoveBackup) {
            return quarantineCommittedRecovery(
                dependencies, &child,
                "committed-recovery-intent-rejected");
        }
        const WindowsBootstrapFilesystemResult cleaned =
            dependencies.filesystem.Apply(intent);
        if (!cleaned.ok()) {
            const BootstrapProcessResult releasedAfterCleanupFailure =
                dependencies.process.ReleaseOwned(child);
            dependencies.health.Close();
            return {BootstrapApplicationStatus::Quarantined,
                    releasedAfterCleanupFailure.ok()
                        ? "committed-recovery-cleanup-pending"
                        : "committed-recovery-child-release-failed",
                    true};
        }
    }
    const BootstrapProcessResult released =
        dependencies.process.ReleaseOwned(child);
    if (!released.ok()) {
        if (child.valid()) dependencies.process.Close(child);
        dependencies.health.Close();
        return {BootstrapApplicationStatus::Quarantined,
                "committed-recovery-child-release-failed", true};
    }
    const WindowsBootstrapFilesystemResult invocationCleaned =
        cleanupCommittedInvocationWithDiagnostics(dependencies.filesystem);
    if (!invocationCleaned.ok()) {
        dependencies.health.Close();
        return {BootstrapApplicationStatus::Quarantined,
                "committed-recovery-cleanup-pending", true};
    }
    const WindowsBootstrapFilesystemResult retired =
        dependencies.filesystem.RetireTerminalJournal();
    dependencies.health.Close();
    return retired.ok()
        ? BootstrapApplicationResult{BootstrapApplicationStatus::Recovered,
                                     "recovery-committed-relaunched", true}
        : BootstrapApplicationResult{BootstrapApplicationStatus::Quarantined,
                                     "recovery-journal-retirement-failed", true};
}

BootstrapApplicationStatus terminalStatusForForwardFailure(
    const std::string_view reason) {
    if (reason.find("health") != std::string_view::npos ||
        reason.find("child-crashed") != std::string_view::npos) {
        return BootstrapApplicationStatus::HealthRejected;
    }
    if (reason.find("launch") != std::string_view::npos ||
        reason.find("child-") != std::string_view::npos ||
        reason.find("ownership") != std::string_view::npos) {
        return BootstrapApplicationStatus::LaunchRejected;
    }
    if (reason.find("filesystem") != std::string_view::npos ||
        reason.find("backup") != std::string_view::npos ||
        reason.find("replacement") != std::string_view::npos ||
        reason.find("commit") != std::string_view::npos ||
        reason.find("journal") != std::string_view::npos) {
        return BootstrapApplicationStatus::FilesystemRejected;
    }
    return BootstrapApplicationStatus::Quarantined;
}

BootstrapApplicationResult terminalForwardFailure(
    BootstrapApplicationDependencies dependencies,
    BootstrapOwnedProcess* child,
    const std::string_view reason,
    const bool mutated) {
#ifdef ARIS_BOOTSTRAP_TEST_DIAGNOSTICS
    std::fprintf(stderr, "runtime:forward-terminal=%.*s\n",
                 static_cast<int>(reason.size()), reason.data());
    std::fflush(stderr);
#endif
    if (child != nullptr && child->valid()) {
        const BootstrapProcessResult terminated =
            dependencies.process.TerminateOwned(*child, 0xE001U);
        dependencies.process.Close(*child);
        if (!terminated.ok()) {
            dependencies.health.Close();
            return {BootstrapApplicationStatus::Quarantined,
                    std::string(reason) + "-child-termination-failed", mutated};
        }
    }
    dependencies.health.Close();
    if (mutated &&
        (reason == "health-timeout" ||
         reason == "updated-child-crashed" ||
         reason == "health-channel-rejected" ||
         reason == "health-mismatch" ||
         reason == "health-checkpoint-failed")) {
        return {BootstrapApplicationStatus::InstalledHealthUnconfirmed,
                std::string("installed-health-unconfirmed:") +
                    std::string(reason), true};
    }
    return {terminalStatusForForwardFailure(reason), std::string(reason), mutated};
}

BootstrapApplicationResult launchHealthAndCommit(
    const ValidatedInstallPlan& plan,
    const ReplacementJournal& journal,
    BootstrapApplicationDependencies dependencies,
    const BootstrapApplicationOptions& options,
    TransactionDecision transaction,
    const bool mutated,
    const bool recoveryMode) {
    BootstrapOwnedProcess child;
    const auto fail = [&](const std::string_view reason,
                          const TransactionState& state,
                          const bool changed) {
        if (options.forwardOnly) {
            return terminalForwardFailure(dependencies, &child, reason, changed);
        }
        return applyRollback(plan, dependencies, &child, std::string(reason),
                             state, changed);
    };
    reportProgress(dependencies, BootstrapProgressPhase::Restarting);
    testDiagnostic("runtime:health-prepare\n");
    const TransactionState preChildState = transaction.state;
    const std::optional<BootstrapExecutableExpectation> expectedImage =
        expectedExecutable(plan);
    if (!expectedImage.has_value()) {
        return fail("updated-child-expectation-missing", preChildState,
                    mutated);
    }
    BootstrapLaunchCapability capability;
    const BootstrapHealthResult prepared = dependencies.health.Prepare(
        plan.healthToken, plan.version, plan.buildId, capability);
    if (!prepared.ok()) {
        return fail("health-channel-prepare-failed", preChildState, mutated);
    }
    testDiagnostic("runtime:health-prepared\n");
    testDiagnostic("runtime:child-launch-begin\n");
    const BootstrapProcessResult launched = dependencies.process.LaunchSuspended(
        executablePath(plan), plan.identity.targetRoot, capability);
    testDiagnostic("runtime:child-launch-returned\n");
    dependencies.health.ReleaseLaunchCapability();
    if (!launched.ok() || !launched.process.valid()) {
        return fail("updated-child-launch-failed", preChildState, mutated);
    }
    child = launched.process;
    testDiagnostic("runtime:child-launched\n");
    if (!sameExecutablePath(child.identity.imagePath,
                            expectedImage->absolutePath)) {
        return fail("updated-child-identity-mismatch", preChildState, mutated);
    }
    const BootstrapProcessResult imageValidated =
        dependencies.process.ValidateOwnedExecutable(child, *expectedImage);
    if (!imageValidated.ok()) {
        return fail("updated-child-content-mismatch", preChildState, mutated);
    }
    const WindowsBootstrapFilesystemResult childCheckpoint =
        dependencies.filesystem.RecordUpdatedChildIdentity(child.identity);
    if (!childCheckpoint.ok()) {
        return fail("updated-child-checkpoint-failed", preChildState, mutated);
    }
#ifdef ARIS_BOOTSTRAP_TEST_CRASH_AFTER_CHILD_CHECKPOINT
    if (plan.healthToken == std::string(64U, 'd')) {
        std::_Exit(0xE2);
    }
#endif
    const UpdatedChildRecordDecision childRecord =
        RecordUpdatedChildProcess(transaction.state, child.identity.processId);
    if (childRecord.result != UpdatedChildRecordResult::Recorded &&
        childRecord.result != UpdatedChildRecordResult::Idempotent) {
        return fail("updated-child-record-failed", preChildState, mutated);
    }
    transaction = childRecord.transaction;
    const BootstrapProcessResult resumed = dependencies.process.ResumeOwned(child);
    if (!resumed.ok()) {
        return fail("updated-child-resume-failed", preChildState, mutated);
    }

    reportProgress(dependencies, BootstrapProgressPhase::CheckingStartup);
    const BootstrapHealthResult health = dependencies.health.Await(
        child, plan.healthToken, plan.version, plan.buildId, options.healthWaitMs);
    if (health.status != BootstrapHealthStatus::Accepted) {
        const char* failure = health.status == BootstrapHealthStatus::TimedOut
            ? "health-timeout"
            : health.status == BootstrapHealthStatus::ChildExited
                ? "updated-child-crashed"
                : "health-channel-rejected";
        return fail(failure, transaction.state, mutated);
    }
    testDiagnostic("runtime:health-accepted\n");
    const TransactionState preCommitState = transaction.state;
    const HealthDecision healthDecision =
        AcknowledgeHealth(preCommitState, health.acknowledgement);
    if (healthDecision.health != HealthAcknowledgementResult::Accepted) {
        return fail("health-mismatch", preCommitState, mutated);
    }
    const WindowsBootstrapFilesystemResult healthCheckpoint =
        dependencies.filesystem.RecordHealthState("accepted", true);
    if (!healthCheckpoint.ok()) {
        return fail("health-checkpoint-failed", preCommitState, mutated);
    }
    transaction = healthDecision.transaction;
    const BootstrapProcessResult ownershipPrepared =
        dependencies.process.PrepareOwnedForCommit(child);
    if (!ownershipPrepared.ok()) {
        return fail("updated-child-ownership-prepare-failed",
                    transaction.state, true);
    }
    bool commitPublished = false;
    reportProgress(dependencies, BootstrapProgressPhase::Finalizing);
    for (const BootstrapIntent& intent : transaction.intents) {
        if (!isFilesystemIntent(intent.kind)) continue;
        const WindowsBootstrapFilesystemResult applied =
            dependencies.filesystem.Apply(intent);
#ifdef ARIS_BOOTSTRAP_TEST_DIAGNOSTICS
        std::fprintf(stderr,
                     "runtime:forward-apply=%d path=%s status=%d backend=%d native=%u\n",
                     static_cast<int>(intent.kind), intent.relativePath.c_str(),
                     static_cast<int>(applied.status),
                     static_cast<int>(applied.backendStatus),
                     applied.nativeError);
        std::fflush(stderr);
#endif
        if (!applied.ok()) {
            if (commitPublished) {
                const BootstrapProcessResult releasedAfterCommit =
                    dependencies.process.ReleaseOwned(child);
                dependencies.health.Close();
                return {releasedAfterCommit.ok()
                            ? (recoveryMode
                                   ? BootstrapApplicationStatus::Recovered
                                   : BootstrapApplicationStatus::Completed)
                            : BootstrapApplicationStatus::Quarantined,
                        releasedAfterCommit.ok()
                            ? "commit-cleanup-pending"
                            : "updated-child-release-failed",
                        true};
            }
            if (intent.kind == BootstrapIntentKind::CommitJournal) {
                const BootstrapProcessResult contained =
                    dependencies.process.TerminateOwned(child, 0xE001U);
                dependencies.health.Close();
                return {BootstrapApplicationStatus::Quarantined,
                        contained.ok()
                            ? "commit-publication-uncertain"
                            : "owned-child-termination-failed",
                        true};
            }
            return fail("commit-failed", preCommitState, true);
        }
        commitPublished = commitPublished ||
            intent.kind == BootstrapIntentKind::CommitJournal;
#ifdef ARIS_BOOTSTRAP_TEST_CRASH_AFTER_COMMIT
        if (intent.kind == BootstrapIntentKind::CommitJournal &&
            plan.healthToken == std::string(64U, 'f')) {
            std::_Exit(0xE4);
        }
#endif
#ifdef ARIS_BOOTSTRAP_TEST_CRASH_AFTER_COMMITTED_CLEANUP
        if (intent.kind == BootstrapIntentKind::RemoveBackup &&
            plan.healthToken == std::string(64U, 'b')) {
            std::_Exit(0xE5);
        }
#endif
    }
    const BootstrapProcessResult released = dependencies.process.ReleaseOwned(child);
    if (!released.ok()) {
        dependencies.health.Close();
        return {BootstrapApplicationStatus::Quarantined,
                "updated-child-release-failed", true};
    }
#ifdef ARIS_BOOTSTRAP_TEST_CRASH_AFTER_COMMITTED_RELEASE
    if (plan.healthToken == std::string(64U, 'a')) {
        std::_Exit(0xE6);
    }
#endif
    const WindowsBootstrapFilesystemResult invocationCleaned =
        cleanupCommittedInvocationWithDiagnostics(dependencies.filesystem);
    if (!invocationCleaned.ok()) {
        if (invocationCleaned.status ==
            WindowsBootstrapFilesystemStatus::RetryableAccessDenied) {
            const BootstrapTerminalCleanupResult successor =
                launchTerminalCleanupSuccessor(plan, journal, dependencies);
#if defined(ARIS_UPDATE_LOCAL_DEMO) && ARIS_UPDATE_LOCAL_DEMO
            writeLocalDemoSuccessorDiagnostic(successor);
#endif
            if (successor.ok()) {
                dependencies.health.Close();
                return {recoveryMode ? BootstrapApplicationStatus::Recovered
                                     : BootstrapApplicationStatus::Completed,
                        recoveryMode ? "recovery-committed-cleanup-deferred"
                                     : "committed-cleanup-deferred",
                        true};
            }
        }
        dependencies.health.Close();
        return {recoveryMode ? BootstrapApplicationStatus::Recovered
                             : BootstrapApplicationStatus::Completed,
                "commit-cleanup-pending", true};
    }
    const WindowsBootstrapFilesystemResult retired =
        dependencies.filesystem.RetireTerminalJournal();
    if (!retired.ok()) {
#ifdef ARIS_BOOTSTRAP_TEST_DIAGNOSTICS
        std::fprintf(stderr,
                     "runtime:journal-retirement-status=%d backend=%d native=%u\n",
                     static_cast<int>(retired.status),
                     static_cast<int>(retired.backendStatus),
                     retired.nativeError);
        std::fflush(stderr);
#endif
        dependencies.health.Close();
        return {recoveryMode ? BootstrapApplicationStatus::Recovered
                             : BootstrapApplicationStatus::Completed,
                "commit-journal-retirement-failed-" +
                    std::to_string(static_cast<int>(retired.status)),
                true};
    }
    dependencies.health.Close();
    testDiagnostic("runtime:committed\n");
    return {recoveryMode ? BootstrapApplicationStatus::Recovered
                         : BootstrapApplicationStatus::Completed,
            recoveryMode ? "recovery-committed" : "committed", true};
}

}  // namespace

bool BootstrapParentImageMatchesUpdaterBase(
    const BootstrapProcessIdentity& parent,
    const std::string_view updaterOwnedBase) {
    // This is a structural precondition only.  The native channel separately
    // binds the live parent handle to its exact lifetime/image FileId and to
    // the Bootstrap Authenticode signer.  The updater-owned base authorizes
    // staging/transaction roots; it is not a filename allowlist for the
    // authenticated parent executable.
    return parent.valid && !parent.imagePath.empty() &&
           !updaterOwnedBase.empty();
}

static BootstrapApplicationResult runBootstrapApplicationCore(
    const BootstrapAdmission& admission,
    const WindowsBootstrapFilesystemRoots& roots,
    BootstrapApplicationDependencies dependencies,
    const BootstrapApplicationOptions& options) {
    if (!admission.ok() || admission.plan->invocation.stagingRoot != roots.stagingRoot ||
        admission.plan->invocation.transactionRoot != roots.transactionRoot ||
        admission.plan->invocation.updaterOwnedBase != roots.updaterOwnedBase ||
        admission.plan->invocation.invocationIdentity != roots.invocationIdentity) {
        return {BootstrapApplicationStatus::AdmissionRejected,
                "invalid-admission-or-root-binding", false};
    }

    const ValidatedInstallPlan& plan = *admission.plan;
    const ReplacementJournal& journal = *admission.journal;

    if (options.recovery) {
        testDiagnostic("runtime:recovery-begin\n");
        const WindowsBootstrapFilesystemRecoveryResult recovered =
            dependencies.filesystem.BeginRecovery(plan, roots);
#ifdef ARIS_BOOTSTRAP_TEST_DIAGNOSTICS
        std::fprintf(stderr, "runtime:recovery-status=%d backend=%d native=%u\n",
                     static_cast<int>(recovered.filesystem.status),
                     static_cast<int>(recovered.filesystem.backendStatus),
                     recovered.filesystem.nativeError);
        std::fflush(stderr);
#endif
        if (!recovered.filesystem.ok()) {
            if (options.forwardOnly) {
                dependencies.health.Close();
                return {BootstrapApplicationStatus::ReinstallRequired,
                        "reinstall-required-recovery-rejected", false};
            }
            return {BootstrapApplicationStatus::Quarantined,
                    "recovery-rejected", false};
        }
        if (options.forwardOnly) {
            dependencies.health.Close();
            const char* suffix = "incomplete";
            switch (recovered.disposition) {
                case WindowsBootstrapRecoveryDisposition::Rollback:
                    suffix = "rollback";
                    break;
                case WindowsBootstrapRecoveryDisposition::Quarantine:
                    suffix = "quarantine";
                    break;
                case WindowsBootstrapRecoveryDisposition::ContinueForward:
                    suffix = "continue-forward";
                    break;
                case WindowsBootstrapRecoveryDisposition::CleanupCommitted:
                    suffix = "cleanup-committed";
                    break;
                case WindowsBootstrapRecoveryDisposition::Complete:
                    suffix = "complete";
                    break;
            }
            return {BootstrapApplicationStatus::ReinstallRequired,
                    std::string("reinstall-required-recovery-") + suffix,
                    false};
        }
        const std::uint32_t recoveryPreviousFailureCount =
            dependencies.filesystem.RecoveredPreviousFailureCount()
                .value_or(options.previousFailureCount);
        // Quarantine is an authority failure.  Do not apply any intents from
        // an ambiguous or tampered durable record before returning it.
        if (recovered.disposition == WindowsBootstrapRecoveryDisposition::Quarantine) {
            return {BootstrapApplicationStatus::Quarantined,
                    "recovery-quarantine", false};
        }
        if (recovered.disposition == WindowsBootstrapRecoveryDisposition::Rollback) {
            const bool failureAlreadyRecorded =
                dependencies.filesystem.
                    RecoveredFailureRecordedForTransaction();
            const FailureDisposition recoveryFailure = failureAlreadyRecorded
                ? FailureDisposition{recoveryPreviousFailureCount,
                                     recoveryPreviousFailureCount >= 2U}
                : NextFailureDisposition(recoveryPreviousFailureCount);
            if (!failureAlreadyRecorded) {
                const WindowsBootstrapFilesystemResult failureRecorded =
                    dependencies.filesystem.Apply(
                        {BootstrapIntentKind::RecordFailure, {}, 0,
                         "restart-rollback"});
                if (!failureRecorded.ok()) {
                    dependencies.health.Close();
                    return {BootstrapApplicationStatus::Quarantined,
                            "recovery-failure-record-failed", true};
                }
            }
            const std::optional<BootstrapProcessIdentity> durableChild =
                dependencies.filesystem.RecoveredUpdatedChildIdentity();
            if (durableChild.has_value()) {
                BootstrapProcessResult rebound = dependencies.process.RebindOwnedChild(
                    *durableChild);
                if (rebound.status != BootstrapProcessStatus::NotFound) {
                    if (!rebound.ok() || !rebound.process.valid()) {
                        if (rebound.process.valid()) {
                            dependencies.process.Close(rebound.process);
                        }
                        dependencies.health.Close();
                        return {BootstrapApplicationStatus::Quarantined,
                                "recovery-child-identity-rejected", true};
                    }
                    const BootstrapProcessResult terminated =
                        dependencies.process.TerminateOwned(rebound.process, 0xE001U);
                    if (!terminated.ok()) {
                        if (rebound.process.valid()) {
                            dependencies.process.Close(rebound.process);
                        }
                        dependencies.health.Close();
                        return {BootstrapApplicationStatus::Quarantined,
                                "recovery-child-termination-failed", true};
                    }
                }
            }
            for (const BootstrapIntent& intent : recovered.intents) {
                if (!isFilesystemIntent(intent.kind) ||
                    intent.kind == BootstrapIntentKind::RemoveBackup) {
                    continue;
                }
                const WindowsBootstrapFilesystemResult applied =
                    applyFilesystemIntentWithBoundedRetry(
                        dependencies.filesystem, intent);
                if (!applied.ok()) {
                    dependencies.health.Close();
                    return {BootstrapApplicationStatus::Quarantined,
                            "recovery-rollback-apply-failed", true};
                }
            }
            const PreviousApplicationRelaunchResult relaunched =
                ensurePreviousApplicationRelaunched(plan, dependencies);
            if (!relaunched.completed) {
                dependencies.health.Close();
                return {BootstrapApplicationStatus::Quarantined,
                        "recovery-" + relaunched.reason, true};
            }
            const WindowsBootstrapFilesystemResult cleaned =
                dependencies.filesystem.Apply(
                    {BootstrapIntentKind::RemoveBackup, {}, 0,
                     "restart-rollback-cleanup"});
            if (!cleaned.ok()) {
                dependencies.health.Close();
                return {BootstrapApplicationStatus::Quarantined,
                        "recovery-rollback-cleanup-failed", true};
            }
            const WindowsBootstrapFilesystemResult invocationCleaned =
                cleanupCommittedInvocationWithDiagnostics(dependencies.filesystem);
            if (!invocationCleaned.ok()) {
                dependencies.health.Close();
                return {BootstrapApplicationStatus::Quarantined,
                        "recovery-rollback-invocation-cleanup-pending", true};
            }
            if (recoveryFailure.quarantine) {
                const WindowsBootstrapFilesystemResult quarantined =
                    dependencies.filesystem.Apply(
                        {BootstrapIntentKind::QuarantinePackage, {}, 0,
                         "restart-rollback-second-failure"});
                if (!quarantined.ok()) {
                    dependencies.health.Close();
                    return {BootstrapApplicationStatus::Quarantined,
                            "recovery-quarantine-record-failed", true};
                }
            } else {
                const WindowsBootstrapFilesystemResult retired =
                    dependencies.filesystem.RetireTerminalJournal();
                if (!retired.ok()) {
                    dependencies.health.Close();
                    return {BootstrapApplicationStatus::Quarantined,
                            "recovery-journal-retirement-failed", true};
                }
            }
            dependencies.health.Close();
            return {recoveryFailure.quarantine
                        ? BootstrapApplicationStatus::Quarantined
                        : BootstrapApplicationStatus::RolledBack,
                    recoveryFailure.quarantine
                        ? "recovery-rollback-quarantined"
                        : "recovery-rollback",
                    true};
        }
        // The native health transport is an inherited, one-shot channel whose
        // handles do not survive a process restart.  Without a fresh envelope
        // capability the exact durable child is terminated and recovery fails
        // closed.  With a fresh capability, terminate that same child and
        // durably clear the stale attempt before constructing a new channel.
        if (dependencies.filesystem.RecoveredHealthPhase() ==
                "awaiting-health") {
            const std::optional<BootstrapProcessIdentity> durableChild =
                dependencies.filesystem.RecoveredUpdatedChildIdentity();
            if (!durableChild.has_value()) {
                dependencies.health.Close();
                return {BootstrapApplicationStatus::Quarantined,
                        "recovery-awaiting-health-identity-missing", true};
            }
            BootstrapProcessResult rebound = dependencies.process.RebindOwnedChild(
                *durableChild);
            if (rebound.status != BootstrapProcessStatus::NotFound &&
                (!rebound.ok() || !rebound.process.valid())) {
                if (rebound.process.valid()) {
                    dependencies.process.Close(rebound.process);
                }
                dependencies.health.Close();
                return {BootstrapApplicationStatus::Quarantined,
                        "recovery-awaiting-health-child-rejected", true};
            }
            if (rebound.status != BootstrapProcessStatus::NotFound) {
                const BootstrapProcessResult terminated =
                    dependencies.process.TerminateOwned(rebound.process, 0xE001U);
                dependencies.process.Close(rebound.process);
                if (!terminated.ok()) {
                    dependencies.health.Close();
                    return {BootstrapApplicationStatus::Quarantined,
                            "recovery-awaiting-health-termination-failed", true};
                }
            }
            if (!options.recoveryHealthCapabilityAvailable) {
                dependencies.health.Close();
                return {BootstrapApplicationStatus::Quarantined,
                        "recovery-awaiting-health-channel-unavailable", true};
            }
            const WindowsBootstrapFilesystemResult prepared =
                dependencies.filesystem.PrepareRecoveredHealthRelaunch(
                    *durableChild);
            if (!prepared.ok()) {
                dependencies.health.Close();
                return {BootstrapApplicationStatus::Quarantined,
                        "recovery-health-relaunch-checkpoint-failed", true};
            }
        }
        if (recovered.disposition ==
                WindowsBootstrapRecoveryDisposition::ContinueForward &&
            !options.recoveryHealthCapabilityAvailable) {
            // A restart can authenticate the durable nonce digest but cannot
            // recover the original one-shot secret.  Never turn a structural
            // placeholder into a launch capability.  Roll back only the
            // progress represented by the journal-bound classifications;
            // incomplete forward intents are deliberately not applied.
            const WindowsBootstrapFilesystemResult failureRecorded =
                dependencies.filesystem.Apply(
                    {BootstrapIntentKind::RecordFailure, {}, 0,
                     "recovery-health-capability-unavailable"});
            if (!failureRecorded.ok()) {
                dependencies.health.Close();
                return {BootstrapApplicationStatus::Quarantined,
                        "recovery-failure-record-failed", true};
            }
            const WindowsBootstrapFilesystemResult quarantined =
                dependencies.filesystem.Apply(
                    {BootstrapIntentKind::QuarantinePackage, {}, 0,
                     "recovery-forward-state-incomplete"});
            dependencies.health.Close();
            return {BootstrapApplicationStatus::Quarantined,
                    quarantined.ok()
                        ? "recovery-health-capability-unavailable"
                        : "recovery-quarantine-record-failed",
                    true};
        }
        if (recovered.disposition ==
            WindowsBootstrapRecoveryDisposition::CleanupCommitted) {
            if (!recovered.journal.has_value()) {
                return quarantineCommittedRecovery(
                    dependencies, nullptr,
                    "committed-recovery-journal-missing");
            }
            return recoverCommittedApplication(
                plan, *recovered.journal, dependencies, options,
                recovered.intents);
        }
        for (const BootstrapIntent& intent : recovered.intents) {
            if (!isFilesystemIntent(intent.kind)) continue;
            const WindowsBootstrapFilesystemResult applied =
                applyFilesystemIntentWithBoundedRetry(
                    dependencies.filesystem, intent);
            if (!applied.ok()) {
                return {BootstrapApplicationStatus::Quarantined,
                        "recovery-apply-failed", true};
            }
        }
        if (recovered.disposition == WindowsBootstrapRecoveryDisposition::Complete) {
            if (dependencies.filesystem.RecoveredRollbackComplete()) {
                const PreviousApplicationRelaunchResult relaunched =
                    ensurePreviousApplicationRelaunched(plan, dependencies);
                if (relaunched.completed) {
                    const WindowsBootstrapFilesystemResult cleaned =
                        dependencies.filesystem.Apply(
                            {BootstrapIntentKind::RemoveBackup, {}, 0,
                             "restart-terminal-rollback-cleanup"});
                    if (cleaned.ok()) {
                        const WindowsBootstrapFilesystemResult invocationCleaned =
                            cleanupCommittedInvocationWithDiagnostics(
                                dependencies.filesystem);
                        if (!invocationCleaned.ok()) {
                            dependencies.health.Close();
                            return {BootstrapApplicationStatus::Quarantined,
                                    "recovery-rollback-invocation-cleanup-pending",
                                    true};
                        }
                        const WindowsBootstrapFilesystemResult retired =
                            dependencies.filesystem.RetireTerminalJournal();
                        if (!retired.ok()) {
                            dependencies.health.Close();
                            return {BootstrapApplicationStatus::Quarantined,
                                    "recovery-journal-retirement-failed", true};
                        }
                    } else {
                        dependencies.health.Close();
                        return {BootstrapApplicationStatus::Quarantined,
                                "recovery-rollback-cleanup-failed", true};
                    }
                }
                dependencies.health.Close();
                return relaunched.completed
                    ? BootstrapApplicationResult{
                          BootstrapApplicationStatus::RolledBack,
                          "recovery-rollback-relaunch-complete", true}
                    : BootstrapApplicationResult{
                          BootstrapApplicationStatus::Quarantined,
                          "recovery-" + relaunched.reason, true};
            }
            const WindowsBootstrapFilesystemResult retired =
                dependencies.filesystem.RetireTerminalJournal();
            if (!retired.ok()) {
                dependencies.health.Close();
                return {BootstrapApplicationStatus::Quarantined,
                        "recovery-journal-retirement-failed", true};
            }
            return {BootstrapApplicationStatus::Recovered,
                    "recovery-complete", true};
        }
        const ReplacementJournal& recoveryJournal =
            recovered.journal.has_value() ? *recovered.journal : journal;
        TransactionDecision recoveryTransaction = StartTransaction(
            plan, recoveryJournal, recoveryPreviousFailureCount);
        recoveryTransaction = OnOldProcessWait(
            recoveryTransaction.state, ProcessWaitOutcome::Exited);
        // A durable recovery record may have completed the backup before the
        // restart, while the reconstructed pure journal has no live-session
        // progress bit. The filesystem recovery invariant guarantees this
        // before emitting a replacement intent; mirror that fact into the
        // reducer before health acknowledgement.
        std::vector<std::string> recoveryBackupPaths;
        for (const auto& item : recoveryTransaction.state.journal.items) {
            if ((item.classification == JournalItemClass::ReplaceExisting ||
                 item.classification == JournalItemClass::RemoveExisting) &&
                !item.backupRecorded) {
                recoveryBackupPaths.push_back(item.relativePath);
            }
        }
        for (const std::string& path : recoveryBackupPaths) {
            const JournalRecordDecision recorded = RecordBackupSnapshot(
                recoveryTransaction.state, path);
            if (recorded.result == JournalRecordResult::Recorded ||
                recorded.result == JournalRecordResult::Idempotent) {
                recoveryTransaction = recorded.transaction;
            }
        }
        for (const BootstrapIntent& intent : recovered.intents) {
            if (intent.kind == BootstrapIntentKind::SnapshotBackup) {
                const JournalRecordDecision recorded = RecordBackupSnapshot(
                    recoveryTransaction.state, intent.relativePath);
                if (recorded.result == JournalRecordResult::Recorded ||
                    recorded.result == JournalRecordResult::Idempotent) {
                    recoveryTransaction = recorded.transaction;
                }
            } else if (intent.kind == BootstrapIntentKind::ReplaceInventoryItem ||
                       intent.kind == BootstrapIntentKind::RemoveInstalledItem) {
                const JournalRecordDecision recorded = RecordReplacementWritten(
                    recoveryTransaction.state, intent.relativePath);
                if (recorded.result == JournalRecordResult::Recorded ||
                    recorded.result == JournalRecordResult::Idempotent) {
                    recoveryTransaction = recorded.transaction;
                }
            }
        }
        std::vector<std::string> completedRecoveryPaths;
        for (const auto& item : recoveryTransaction.state.journal.items) {
            if (item.classification == JournalItemClass::PreserveExisting ||
                item.replacementRecorded) {
                continue;
            }
            const bool hasReplayIntent = std::any_of(
                recovered.intents.begin(), recovered.intents.end(),
                [&item](const BootstrapIntent& candidate) {
                    return (candidate.kind ==
                                BootstrapIntentKind::ReplaceInventoryItem ||
                            candidate.kind ==
                                BootstrapIntentKind::RemoveInstalledItem) &&
                           candidate.relativePath == item.relativePath;
                });
            if (!hasReplayIntent) completedRecoveryPaths.push_back(item.relativePath);
        }
        for (const std::string& path : completedRecoveryPaths) {
            const JournalRecordDecision recorded = RecordReplacementWritten(
                recoveryTransaction.state, path);
            if (recorded.result == JournalRecordResult::Recorded ||
                recorded.result == JournalRecordResult::Idempotent) {
                recoveryTransaction = recorded.transaction;
            }
        }
        return launchHealthAndCommit(plan, recoveryTransaction.state.journal,
                                     dependencies, options,
                                     std::move(recoveryTransaction), true, true);
    }

    reportProgress(dependencies, BootstrapProgressPhase::WaitingForExit);
    if (!plan.invocation.oldProcess.valid ||
        plan.invocation.oldProcess.processId != plan.oldProcessId) {
        return {BootstrapApplicationStatus::OldProcessRejected,
                "missing-old-process-identity", false};
    }
    BootstrapProcessResult old = dependencies.process.OpenExpected(
        plan.invocation.oldProcess);
    if (!old.executable.valid() || !old.executable.exactFileIdentity.valid ||
        !sameExecutablePath(old.executable.absolutePath, executablePath(plan))) {
        if (old.process.valid()) dependencies.process.Close(old.process);
        return {BootstrapApplicationStatus::OldProcessRejected,
                "old-process-executable-snapshot-rejected", false};
    }
    const BootstrapExecutableExpectation previousExecutable = old.executable;
    if (old.status != BootstrapProcessStatus::NotFound) {
        if (!old.ok() || !old.process.valid()) {
            return {BootstrapApplicationStatus::OldProcessRejected,
                    "old-process-identity-rejected", false};
        }
        testDiagnostic("runtime:old-opened\n");
        const BootstrapProcessResult waited = dependencies.process.WaitForExit(
            old.process, options.oldProcessWaitMs);
        dependencies.process.Close(old.process);
        if (waited.status == BootstrapProcessStatus::TimedOut) {
            return {BootstrapApplicationStatus::OldProcessTimedOut,
                    "old-process-timeout", false};
        }
        if (!waited.ok()) {
            return {BootstrapApplicationStatus::OldProcessRejected,
                    "old-process-wait-failed", false};
        }
    }
    testDiagnostic("runtime:old-exited\n");
    reportProgress(dependencies, BootstrapProgressPhase::PreparingFiles);

    const WindowsBootstrapFilesystemResult begun = dependencies.filesystem.Begin(
        plan, journal, roots);
#if defined(ARIS_UPDATE_LOCAL_DEMO) && ARIS_UPDATE_LOCAL_DEMO
    writeLocalDemoBeginDiagnostic(begun, begun.ok() ? "ok" : "failed");
#endif
    if (!begun.ok()) {
#if defined(ARIS_BOOTSTRAP_TEST_DIAGNOSTICS) || \
    (defined(ARIS_UPDATE_LOCAL_DEMO) && ARIS_UPDATE_LOCAL_DEMO)
        std::fprintf(stderr,
                     "runtime:filesystem-begin-status=%d backend=%d native=%u\n",
                     static_cast<int>(begun.status),
                     static_cast<int>(begun.backendStatus),
                     begun.nativeError);
        std::fflush(stderr);
#endif
        if (options.forwardOnly) {
            dependencies.health.Close();
            return {BootstrapApplicationStatus::FilesystemRejected,
                    "filesystem-begin-failed", false};
        }
        return rejectFilesystemAfterOldExit(
            plan, dependencies, previousExecutable, "filesystem-begin-failed");
    }
    testDiagnostic("runtime:filesystem-begun\n");
    const WindowsBootstrapFilesystemResult failureCountBound =
        dependencies.filesystem.BindPreviousFailureCount(
            options.previousFailureCount);
    if (!failureCountBound.ok()) {
        if (options.forwardOnly) {
            dependencies.health.Close();
            return {BootstrapApplicationStatus::FilesystemRejected,
                    "filesystem-failure-count-binding-failed", false};
        }
        return rejectFilesystemAfterOldExit(
            plan, dependencies, previousExecutable,
            "filesystem-failure-count-binding-failed");
    }

    TransactionDecision transaction = StartTransaction(
        plan, journal, options.previousFailureCount);
    transaction = OnOldProcessWait(transaction.state, ProcessWaitOutcome::Exited);
    bool mutated = false;
    BootstrapOwnedProcess child;

    const std::vector<BootstrapIntent> operationIntents = transaction.intents;
    const auto replacementCount = static_cast<std::size_t>(std::count_if(
        operationIntents.begin(), operationIntents.end(), [](const BootstrapIntent& intent) {
            return intent.kind == BootstrapIntentKind::ReplaceInventoryItem ||
                   intent.kind == BootstrapIntentKind::RemoveInstalledItem;
        }));
    std::size_t replacementsDone = 0;
    for (const BootstrapIntent& intent : operationIntents) {
        if (intent.kind == BootstrapIntentKind::LaunchUpdatedChild ||
            intent.kind == BootstrapIntentKind::WaitForOldProcessExit ||
            intent.kind == BootstrapIntentKind::AwaitHealthAcknowledgement ||
            intent.kind == BootstrapIntentKind::RecordFailure ||
            intent.kind == BootstrapIntentKind::QuarantinePackage) {
            continue;
        }
        testDiagnostic("runtime:filesystem-apply\n");
        if (intent.kind == BootstrapIntentKind::ReplaceInventoryItem ||
            intent.kind == BootstrapIntentKind::RemoveInstalledItem) {
            reportProgress(dependencies, BootstrapProgressPhase::ReplacingFiles,
                           replacementsDone, replacementCount);
        }
        const WindowsBootstrapFilesystemResult applied =
            applyFilesystemIntentWithBoundedRetry(
                dependencies.filesystem, intent);
#if defined(ARIS_UPDATE_LOCAL_DEMO) && ARIS_UPDATE_LOCAL_DEMO
        if (!applied.ok()) {
            writeLocalDemoMutationDiagnostic(intent.kind, applied);
        }
#endif
        testDiagnostic(applied.ok() ? "runtime:filesystem-applied\n"
                                     : "runtime:filesystem-apply-failed\n");
#ifdef ARIS_BOOTSTRAP_TEST_DIAGNOSTICS
        if (!applied.ok()) {
            writeFilesystemFailureDiagnostic(roots.transactionRoot, intent,
                                             applied);
            std::fprintf(stderr, "runtime:filesystem-status=%d backend=%d native=%u\n",
                         static_cast<int>(applied.status),
                         static_cast<int>(applied.backendStatus),
                         applied.nativeError);
            std::fflush(stderr);
        }
#endif
        if (!applied.ok()) {
            if (options.forwardOnly) {
                return terminalForwardFailure(
                    dependencies, &child,
                    "filesystem-mutation-failed-" +
                        std::to_string(static_cast<int>(intent.kind)) + "-" +
                        std::to_string(static_cast<int>(applied.status)) +
                        "|backend=" + std::to_string(static_cast<int>(applied.backendStatus)) +
                        "|native=" + std::to_string(applied.nativeError),
                    mutated);
            }
            return applyRollback(plan, dependencies, &child,
                                 "filesystem-mutation-failed-" +
                                     std::to_string(static_cast<int>(intent.kind)) +
                                     "-" +
                                     std::to_string(static_cast<int>(applied.status)) +
                                     "|backend=" + std::to_string(static_cast<int>(applied.backendStatus)) +
                                     "|native=" + std::to_string(applied.nativeError),
                                 transaction.state,
                                 mutated);
        }
        if (intent.kind == BootstrapIntentKind::SnapshotBackup) {
            const JournalRecordDecision recorded =
                RecordBackupSnapshot(transaction.state, intent.relativePath);
            if (recorded.result != JournalRecordResult::Recorded &&
                recorded.result != JournalRecordResult::Idempotent) {
                if (options.forwardOnly) {
                    return terminalForwardFailure(
                        dependencies, &child, "backup-record-failed", mutated);
                }
                // Keep the result typed; callers receive rollback/quarantine
                // rather than a best-effort mutation when the journal binding
                // changes underneath the transaction.
                return applyRollback(plan, dependencies, &child,
                                     "backup-record-failed", transaction.state,
                                     mutated);
            }
            transaction = recorded.transaction;
            mutated = true;
        } else if (intent.kind == BootstrapIntentKind::ReplaceInventoryItem ||
                   intent.kind == BootstrapIntentKind::RemoveInstalledItem) {
            const JournalRecordDecision recorded =
                RecordReplacementWritten(transaction.state, intent.relativePath);
            if (recorded.result != JournalRecordResult::Recorded &&
                recorded.result != JournalRecordResult::Idempotent) {
                if (options.forwardOnly) {
                    return terminalForwardFailure(
                        dependencies,
                        &child,
                        "replacement-record-failed-" +
                            std::to_string(static_cast<int>(recorded.result)),
                        mutated);
                }
                return applyRollback(plan, dependencies, &child,
                                     "replacement-record-failed-" +
                                         std::to_string(static_cast<int>(recorded.result)), transaction.state,
                                     mutated);
            }
            transaction = recorded.transaction;
            mutated = true;
            ++replacementsDone;
            reportProgress(dependencies, BootstrapProgressPhase::ReplacingFiles,
                           replacementsDone, replacementCount);
        }
    }

    return launchHealthAndCommit(plan, transaction.state.journal,
                                 dependencies, options,
                                 std::move(transaction), mutated, false);
}

BootstrapApplicationResult RunBootstrapApplication(
    const BootstrapAdmission& admission,
    const WindowsBootstrapFilesystemRoots& roots,
    BootstrapApplicationDependencies dependencies,
    const BootstrapApplicationOptions& options) {
    auto result = runBootstrapApplicationCore(admission, roots, dependencies, options);
    reportProgress(dependencies, result.ok() ? BootstrapProgressPhase::Completed
                                           : BootstrapProgressPhase::Failed);
    return result;
}

}  // namespace aris::bootstrap
