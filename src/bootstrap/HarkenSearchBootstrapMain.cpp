#include "bootstrap/HarkenSearchBootstrapMain.h"

#include "bootstrap/HarkenSearchBootstrapExitExplanation.h"
#include "bootstrap/HarkenSearchBootstrapRuntime.h"
#include "bootstrap/WindowsBootstrapLaunchBroker.h"

#include <algorithm>
#if defined(ARIS_BOOTSTRAP_TEST_DIAGNOSTICS) || \
    (defined(ARIS_UPDATE_LOCAL_DEMO) && ARIS_UPDATE_LOCAL_DEMO)
#include <cstdio>
#endif
#include <limits>
#include <chrono>
#include <string>
#include <string_view>
#include <thread>

#if defined(ARIS_BOOTSTRAP_RELEASE_CONFIG) && \
    (!defined(ARIS_BOOTSTRAP_PACKAGE_KEY_ID) || \
     !defined(ARIS_BOOTSTRAP_PACKAGE_PUBLIC_KEY_HEX))
#error "HarkenSearchBootstrap Release requires an injected package verification key"
#endif

#if defined(ARIS_UPDATE_LOCAL_DEMO) && ARIS_UPDATE_LOCAL_DEMO
// QA demo only: record the exact post-admission stage and exit code so a local
// run can pin where the replacement fails. Never compiled into shipping builds.
#define ARIS_DEMO_STAGE(stageName, codeVal, extraStr)                          \
    do {                                                                       \
        std::FILE* _diag = nullptr;                                            \
        if (fopen_s(&_diag, (std::string(ARIS_UPDATE_LOCAL_DEMO_ROOT) +        \
                             "/bootstrap-demo-stage.txt").c_str(),             \
                    "wb") == 0 &&                                              \
            _diag != nullptr) {                                                \
            std::fprintf(_diag, "stage=%s code=%d extra=[%s]\n", (stageName),  \
                         static_cast<int>(codeVal), (extraStr));               \
            std::fclose(_diag);                                                \
        }                                                                      \
    } while (0)

void writeLocalDemoTerminalChannelDiagnostic(
    const aris::bootstrap::BootstrapTerminalChannelResult& channel) {
    std::FILE* diagnostic = nullptr;
    if (fopen_s(
            &diagnostic,
            (std::string(ARIS_UPDATE_LOCAL_DEMO_ROOT) +
             "/bootstrap-demo-terminal-channel.txt")
                .c_str(),
            "wb") != 0 ||
        diagnostic == nullptr) {
        return;
    }
    std::fprintf(diagnostic, "status=%d native=%u\n",
                 static_cast<int>(channel.status), channel.nativeError);
    std::fclose(diagnostic);
}

void resetLocalDemoTerminalStageDiagnostic() noexcept {
    std::FILE* diagnostic = nullptr;
    const std::string path =
        std::string(ARIS_UPDATE_LOCAL_DEMO_ROOT) +
        "/bootstrap-demo-terminal-stage.txt";
    if (fopen_s(&diagnostic, path.c_str(), "wb") == 0 &&
        diagnostic != nullptr) {
        std::fclose(diagnostic);
    }
}

void writeLocalDemoTerminalStageDiagnostic(
    const char* stage, const int code, const unsigned long nativeError = 0U,
    const char* detail = "") noexcept {
    std::FILE* diagnostic = nullptr;
    const std::string path =
        std::string(ARIS_UPDATE_LOCAL_DEMO_ROOT) +
        "/bootstrap-demo-terminal-stage.txt";
    if (fopen_s(&diagnostic, path.c_str(), "ab") != 0 ||
        diagnostic == nullptr) {
        return;
    }
    std::fprintf(diagnostic, "stage=%s code=%d native=%lu detail=[%s]\n",
                 stage == nullptr ? "unknown" : stage, code,
                 static_cast<unsigned long>(nativeError),
                 detail == nullptr ? "" : detail);
    std::fclose(diagnostic);
}

void writeLocalDemoTerminalRecoveryDiagnostic(
    const aris::bootstrap::WindowsBootstrapFilesystemRecoveryResult& recovery)
    noexcept {
    std::FILE* diagnostic = nullptr;
    const std::string path =
        std::string(ARIS_UPDATE_LOCAL_DEMO_ROOT) +
        "/bootstrap-demo-terminal-recovery.txt";
    if (fopen_s(&diagnostic, path.c_str(), "wb") != 0 ||
        diagnostic == nullptr) {
        return;
    }
    std::fprintf(
        diagnostic,
        "status=%d backend=%d native=%lu disposition=%d generation=%llu "
        "intents=%zu\n",
        static_cast<int>(recovery.filesystem.status),
        static_cast<int>(recovery.filesystem.backendStatus),
        static_cast<unsigned long>(recovery.filesystem.nativeError),
        static_cast<int>(recovery.disposition),
        static_cast<unsigned long long>(recovery.generation),
        recovery.intents.size());
    std::fclose(diagnostic);
}
#else
// Shipping builds still record the failing stage.  This used to be demo-only,
// so a real failed update left no trace at all: the exit code was collapsed
// into one dialog and nothing on disk said which stage refused.  That is the
// reason the same failure looked machine-specific and could not be diagnosed
// from a user's machine.  The record is written next to the updater state, is
// overwritten each attempt, and contains no paths or user data -- only the
// stage name and exit code, which is what support needs to act.
#define ARIS_DEMO_STAGE(stageName, codeVal, extraStr)                          \
    do {                                                                       \
        ::aris::bootstrap::RecordBootstrapStageFailure(                        \
            (stageName), static_cast<int>(codeVal), (extraStr));               \
    } while (0)
#endif

namespace aris::bootstrap {

class BootstrapEntrypointComposition final {
public:
    static InvocationCapability IssueInvocationCapability(
        const VerifiedPackagePlanBinding& binding,
        const InvocationEnvelope& invocation,
        const BootstrapProcessIdentity& authenticatedParent) {
        return InvocationCapability(binding, invocation, authenticatedParent);
    }

    static const BootstrapAdmission* Admission(
        const BootstrapHandoffSubmission& submission) noexcept {
        return submission.admission_.has_value() ? &*submission.admission_
                                                 : nullptr;
    }
};

namespace {

constexpr std::uint32_t kMaximumChannelWaitMs = 60'000U;
constexpr std::uint32_t kMaximumOldProcessWaitMs = 300'000U;
constexpr std::uint32_t kMaximumHealthWaitMs = 120'000U;

bool validEntrypointWaits(const BootstrapEntrypointOptions& options) {
    return options.channelTimeoutMs != 0U &&
           options.channelTimeoutMs <= kMaximumChannelWaitMs &&
           options.oldProcessWaitMs != 0U &&
           options.oldProcessWaitMs <= kMaximumOldProcessWaitMs &&
           options.healthWaitMs != 0U &&
           options.healthWaitMs <= kMaximumHealthWaitMs;
}

std::string normalizedPathText(std::string_view value) {
    if (value.rfind("\\\\?\\", 0U) == 0U) value.remove_prefix(4U);
    std::string output;
    output.reserve(value.size());
    for (char character : value) {
        if (character == '\\') character = '/';
        if (character >= 'A' && character <= 'Z') {
            character = static_cast<char>(character - 'A' + 'a');
        }
        output.push_back(character);
    }
    while (output.size() > 3U && output.back() == '/') output.pop_back();
    return output;
}

bool samePath(std::string_view left, std::string_view right) {
    return normalizedPathText(left) == normalizedPathText(right);
}

bool validInvocationNamespaceSuffix(std::string_view suffix) {
    const std::size_t separator = suffix.find('-');
    if (separator == std::string_view::npos || separator == 0U ||
        suffix.size() - separator - 1U != 32U) {
        return false;
    }
    for (const char character : suffix.substr(0U, separator)) {
        if (character < '0' || character > '9') return false;
    }
    for (const char character : suffix.substr(separator + 1U)) {
        const bool digit = character >= '0' && character <= '9';
        const bool lowerHex = character >= 'a' && character <= 'f';
        if (!digit && !lowerHex) return false;
    }
    return true;
}

bool directInvocationRoot(
    std::string_view base,
    std::string_view root,
    std::string_view prefix,
    std::string& suffix) {
    const std::string normalizedBase = normalizedPathText(base);
    const std::string normalizedRoot = normalizedPathText(root);
    if (normalizedBase.empty() || normalizedRoot.size() <= normalizedBase.size() + 1U ||
        normalizedRoot.compare(0U, normalizedBase.size(), normalizedBase) != 0 ||
        normalizedRoot[normalizedBase.size()] != '/') {
        return false;
    }
    const std::string_view leaf(
        normalizedRoot.data() + normalizedBase.size() + 1U,
        normalizedRoot.size() - normalizedBase.size() - 1U);
    if (leaf.find('/') != std::string_view::npos ||
        leaf.rfind(prefix, 0U) != 0U) {
        return false;
    }
    suffix.assign(leaf.substr(prefix.size()));
    return validInvocationNamespaceSuffix(suffix);
}

std::string joinRoot(std::string_view base, std::string_view leaf) {
    if (base.empty() || leaf.empty()) return {};
    std::string output(base);
    if (output.back() != '/' && output.back() != '\\') output.push_back('/');
    output.append(leaf);
    return output;
}

std::string expectedUpdaterOwnedBase(const ValidatedInstallPlan& plan) {
#if defined(ARIS_BOOTSTRAP_TEST_TRUSTED_ROOTS)
    // The independent black-box fixture owns a private sandbox rather than
    // the product's per-user update directory.  This branch is test-only and
    // is never compiled into the product target.
    return plan.invocation.updaterOwnedBase;
#else
    return NativeBootstrapUpdaterOwnedBase(plan);
#endif
}

bool rootsAreDerivedAndNonOverlapping(
    const ValidatedInstallPlan& plan,
    const std::string& expectedUpdaterOwnedBase) {
    if (expectedUpdaterOwnedBase.empty() ||
        !samePath(plan.invocation.updaterOwnedBase, expectedUpdaterOwnedBase)) {
        return false;
    }
    std::string stagingNamespace;
    std::string transactionNamespace;
    if (!directInvocationRoot(
            plan.invocation.updaterOwnedBase,
            plan.invocation.stagingRoot,
            "staging-invocation-", stagingNamespace) ||
        !directInvocationRoot(
            plan.invocation.updaterOwnedBase,
            plan.invocation.transactionRoot,
            "transaction-invocation-", transactionNamespace)) {
        return false;
    }
    const bool sameInvocationNamespace =
        stagingNamespace == transactionNamespace;
    return sameInvocationNamespace &&
           !samePath(plan.invocation.stagingRoot,
                     plan.invocation.transactionRoot);
}

bool invocationProcessesUseTrustedHosts(const ValidatedInstallPlan& plan) {
    const std::string expectedOld = joinRoot(plan.identity.targetRoot,
                                             plan.entrypoint);
    return !expectedOld.empty() && plan.invocation.parentProcess.valid &&
           plan.invocation.oldProcess.valid &&
           samePath(plan.invocation.oldProcess.imagePath, expectedOld);
}

bool validParentBinding(const BootstrapProcessIdentity& observed,
                        const InvocationEnvelope& invocation) {
    const BootstrapProcessIdentity& expected = invocation.parentProcess;
    return observed.valid && expected.valid &&
           observed.processId == expected.processId &&
           observed.creationTime == expected.creationTime &&
           samePath(observed.imagePath, expected.imagePath) &&
           observed.imageVolumeSerial == expected.imageVolumeSerial &&
           observed.imageFileId == expected.imageFileId;
}

bool makeLaunchBrokerCapability(
    const BootstrapLaunchBrokerHandoff& handoff,
    WindowsBootstrapLaunchBrokerCapability& capability) {
    if (!handoff.valid() ||
        handoff.channelHandle > std::numeric_limits<std::uintptr_t>::max() ||
        handoff.brokerProcessHandle >
            std::numeric_limits<std::uintptr_t>::max() ||
        handoff.containmentJobHandle >
            std::numeric_limits<std::uintptr_t>::max()) {
        return false;
    }
    capability.channelHandle =
        static_cast<std::uintptr_t>(handoff.channelHandle);
    capability.brokerProcessHandle =
        static_cast<std::uintptr_t>(handoff.brokerProcessHandle);
    capability.containmentJobHandle =
        static_cast<std::uintptr_t>(handoff.containmentJobHandle);
    capability.brokerIdentity = handoff.brokerIdentity;
    capability.launchTimeoutMs = handoff.launchTimeoutMs;
    return capability.valid();
}

class InheritedHandoffGuard final {
public:
    explicit InheritedHandoffGuard(const std::uintptr_t value) : handle_(value) {}
    ~InheritedHandoffGuard() { CloseBootstrapChannelHandle(handle_); }
    InheritedHandoffGuard(const InheritedHandoffGuard&) = delete;
    InheritedHandoffGuard& operator=(const InheritedHandoffGuard&) = delete;

private:
    std::uintptr_t handle_ = 0;
};

class InheritedParentProcessGuard final {
public:
    explicit InheritedParentProcessGuard(const std::uintptr_t value)
        : handle_(value) {}
    ~InheritedParentProcessGuard() {
        CloseBootstrapParentProcessHandle(handle_);
    }
    InheritedParentProcessGuard(const InheritedParentProcessGuard&) = delete;
    InheritedParentProcessGuard& operator=(
        const InheritedParentProcessGuard&) = delete;

private:
    std::uintptr_t handle_ = 0;
};

class InheritedTerminalReadyGuard final {
public:
    explicit InheritedTerminalReadyGuard(const std::uintptr_t value)
        : handle_(value) {}
    ~InheritedTerminalReadyGuard() { close(); }
    InheritedTerminalReadyGuard(const InheritedTerminalReadyGuard&) = delete;
    InheritedTerminalReadyGuard& operator=(
        const InheritedTerminalReadyGuard&) = delete;

    void close() noexcept {
        if (handle_ == 0U) return;
        CloseBootstrapTerminalReadyHandle(handle_);
        handle_ = 0U;
    }

private:
    std::uintptr_t handle_ = 0;
};

}  // namespace

int FilesystemMutationStatusFromReason(const std::string& reason, int kind) {
    const std::string prefix =
        "filesystem-mutation-failed-" + std::to_string(kind) + "-";
    if (reason.compare(0, prefix.size(), prefix) != 0) {
        return -1;
    }
    const std::string_view tail(reason.data() + prefix.size(),
                                reason.size() - prefix.size());
    const std::size_t end = tail.find('|');
    const std::string_view segment =
        end == std::string_view::npos ? tail : tail.substr(0, end);
    if (segment.empty()) {
        return -1;
    }
    for (const char character : segment) {
        if (character < '0' || character > '9') {
            return -1;
        }
    }
    return std::stoi(std::string(segment));
}

BootstrapExitCode mapResult(const BootstrapApplicationResult& result) {
    switch (result.status) {
        case BootstrapApplicationStatus::Completed:
        case BootstrapApplicationStatus::Recovered:
            return BootstrapExitCode::Completed;
        case BootstrapApplicationStatus::OldProcessTimedOut:
            return BootstrapExitCode::OldProcessTimedOut;
        case BootstrapApplicationStatus::OldProcessRejected:
            return BootstrapExitCode::OldProcessRejected;
        case BootstrapApplicationStatus::FilesystemRejected:
            return BootstrapExitCode::FilesystemRejected;
        case BootstrapApplicationStatus::LaunchRejected:
            return BootstrapExitCode::LaunchRejected;
        case BootstrapApplicationStatus::InstalledHealthUnconfirmed:
        case BootstrapApplicationStatus::HealthRejected:
            return BootstrapExitCode::HealthRejected;
        case BootstrapApplicationStatus::RolledBack:
            if (result.reason == "health-timeout") {
                return BootstrapExitCode::RolledBackHealthTimedOut;
            }
            if (result.reason == "updated-child-crashed") {
                return BootstrapExitCode::RolledBackChildExited;
            }
            if (result.reason == "health-channel-rejected" ||
                result.reason == "health-mismatch") {
                return BootstrapExitCode::RolledBackHealthRejected;
            }
            return BootstrapExitCode::RolledBack;
        case BootstrapApplicationStatus::Quarantined:
#ifdef ARIS_BOOTSTRAP_TEST_DIAGNOSTICS
            {
                const int status =
                    FilesystemMutationStatusFromReason(result.reason, 1);
                if (status >= 0 && status < 100) {
                    return static_cast<BootstrapExitCode>(100 + status);
                }
            }
#endif
            if (result.reason.find("filesystem-mutation-failed-2-") !=
                std::string::npos) {
                return BootstrapExitCode::QuarantinedSnapshotFailed;
            }
            if (result.reason.find("filesystem-mutation-failed-3-") !=
                std::string::npos) {
                return BootstrapExitCode::QuarantinedReplacementFailed;
            }
            if (result.reason.find("filesystem-mutation-failed") !=
                    std::string::npos ||
                result.reason.find("backup-record-failed") != std::string::npos ||
                result.reason.find("replacement-record-failed") !=
                    std::string::npos) {
                return BootstrapExitCode::QuarantinedMutationFailed;
            }
            if (result.reason.find("health-channel-prepare-failed") !=
                std::string::npos) {
                return BootstrapExitCode::QuarantinedHealthPrepareFailed;
            }
            if (result.reason.find("updated-child-launch-failed") !=
                std::string::npos) {
                return BootstrapExitCode::QuarantinedUpdatedLaunchFailed;
            }
            if (result.reason.find("updated-child-checkpoint-failed") !=
                    std::string::npos ||
                result.reason.find("updated-child-record-failed") !=
                    std::string::npos) {
                return BootstrapExitCode::QuarantinedUpdatedCheckpointFailed;
            }
            if (result.reason.find("updated-child-resume-failed") !=
                    std::string::npos ||
                result.reason.find("updated-child-ownership-prepare-failed") !=
                    std::string::npos) {
                return BootstrapExitCode::QuarantinedUpdatedResumeFailed;
            }
            if (result.reason.find("health-timeout") !=
                    std::string::npos ||
                result.reason.find("health-channel-rejected") !=
                    std::string::npos ||
                result.reason.find("health-mismatch") != std::string::npos ||
                result.reason.find("updated-child-crashed") != std::string::npos ||
                result.reason.find("health-checkpoint-failed") !=
                    std::string::npos) {
                return BootstrapExitCode::QuarantinedHealthFailed;
            }
            if (result.reason.find("commit-failed") != std::string::npos) {
                return BootstrapExitCode::QuarantinedCommitFailed;
            }
            if (result.reason.find("rollback-filesystem-failed") !=
                std::string::npos) {
                return BootstrapExitCode::QuarantinedRollbackFailed;
            }
            if (result.reason.find("previous-application-launch-failed") !=
                    std::string::npos ||
                result.reason.find("recovery-previous-launch-failed") !=
                    std::string::npos) {
                return BootstrapExitCode::QuarantinedPreviousLaunchFailed;
            }
            if (result.reason.find("termination-failed") != std::string::npos) {
                return BootstrapExitCode::QuarantinedChildTerminationFailed;
            }
            if (result.reason.find("release-failed") != std::string::npos) {
                return BootstrapExitCode::QuarantinedChildReleaseFailed;
            }
            if (result.reason.find("recovery-rejected") != std::string::npos) {
                return BootstrapExitCode::QuarantinedRecoveryRejected;
            }
            return BootstrapExitCode::Quarantined;
        case BootstrapApplicationStatus::ReinstallRequired:
            return BootstrapExitCode::ReinstallRequired;
        case BootstrapApplicationStatus::AdmissionRejected:
        default:
            return BootstrapExitCode::InvocationRejected;
    }
}

namespace {

template <typename Operation>
WindowsBootstrapFilesystemResult runTerminalCleanupWithBoundedRetry(
    Operation&& operation) {
    constexpr std::uint32_t kMaximumAttempts = 50U;
    constexpr auto kRetryDelay = std::chrono::milliseconds(10);
    WindowsBootstrapFilesystemResult result;
    for (std::uint32_t attempt = 0U; attempt < kMaximumAttempts; ++attempt) {
        result = operation();
        // This status is reserved for a pre-mutation sharing/access denial.
        // Durability, identity, and content failures leave the loop
        // immediately and therefore remain fail-closed.
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

}  // namespace

std::string FormatTerminalFilesystemAdmissionFailure(
    const WindowsBootstrapFilesystemRecoveryResult& recovery) {
    return "filesystem.status=" +
           std::to_string(static_cast<int>(recovery.filesystem.status)) +
           "|backendStatus=" +
           std::to_string(static_cast<int>(recovery.filesystem.backendStatus)) +
           "|nativeError=" + std::to_string(recovery.filesystem.nativeError);
}

int RunHarkenSearchBootstrapEntrypoint(
    const std::uintptr_t inheritedHandoffHandle,
    const std::uintptr_t inheritedParentProcessHandle,
    const PackagePlanTrustConfig& trust,
    const BootstrapEntrypointOptions& options) {
    InheritedHandoffGuard handoffGuard(inheritedHandoffHandle);
    InheritedParentProcessGuard parentGuard(inheritedParentProcessHandle);
    if (!trust.provisioned) {
        return static_cast<int>(BootstrapExitCode::Unprovisioned);
    }
    if (!validEntrypointWaits(options)) {
        return static_cast<int>(BootstrapExitCode::InvocationRejected);
    }
    // The shipped updater is forward-only. A crashed or failed install is
    // repaired by reinstalling rather than restoring and relaunching an older
    // payload. Reject recovery before reading a channel so no recovery
    // authority can reach the legacy rollback reducer.
    if (options.recovery) {
        return static_cast<int>(BootstrapExitCode::ReinstallRequired);
    }

    const BootstrapChannelResult channel = ReadAuthenticatedBootstrapChannel(
        inheritedHandoffHandle, inheritedParentProcessHandle,
        options.channelTimeoutMs);
    if (!channel.ok()) {
        ARIS_DEMO_STAGE("channel", BootstrapExitCode::ChannelRejected, "");
        return static_cast<int>(BootstrapExitCode::ChannelRejected);
    }
    if (channel.recoveryMarker) {
        ARIS_DEMO_STAGE("recovery-marker", BootstrapExitCode::InvocationRejected, "");
        return static_cast<int>(BootstrapExitCode::InvocationRejected);
    }
    if (!channel.handoff.has_value()) {
        ARIS_DEMO_STAGE("channel-handoff", BootstrapExitCode::ChannelRejected, "");
        return static_cast<int>(BootstrapExitCode::ChannelRejected);
    }
    const BootstrapHandoff& handoff = *channel.handoff;
    const VerifiedPackagePlanBindingResult package = VerifyPackagePlanBinding(
        handoff.packagePlanBytes, handoff.packageSignatureHex, trust);
    if (!package.ok()) {
        ARIS_DEMO_STAGE("package", BootstrapExitCode::PackageRejected, "");
        return static_cast<int>(BootstrapExitCode::PackageRejected);
    }
    const InvocationDecodeResult decoded =
        DecodeInvocationEnvelope(handoff.invocationBytes);
    if (!decoded.ok()) {
        ARIS_DEMO_STAGE("decode-invocation", BootstrapExitCode::InvocationRejected, "");
        return static_cast<int>(BootstrapExitCode::InvocationRejected);
    }
    const InvocationEnvelope& invocation = *decoded.invocation;
    if (!validParentBinding(channel.parentProcess, invocation)) {
        ARIS_DEMO_STAGE("valid-parent-binding", BootstrapExitCode::AuthorityRejected, "");
        return static_cast<int>(BootstrapExitCode::AuthorityRejected);
    }
    InvocationCapability invocationCapability =
        BootstrapEntrypointComposition::IssueInvocationCapability(
            *package.binding, invocation, channel.parentProcess);
    const std::string archivePath =
        AuthorizedBootstrapArchivePath(invocationCapability);
    const BootstrapHandoffSubmission submission = SubmitVerifiedPackagePlan(
        *package.binding, archivePath, std::move(invocationCapability));
    const BootstrapAdmission* admission =
        BootstrapEntrypointComposition::Admission(submission);
    if (!submission.ok() || admission == nullptr ||
        !admission->plan.has_value() || !admission->journal.has_value()) {
        ARIS_DEMO_STAGE("admission", BootstrapExitCode::AdmissionRejected,
                        submission.diagnostic().c_str());
        return static_cast<int>(BootstrapExitCode::AdmissionRejected);
    }
    const ValidatedInstallPlan& plan = *admission->plan;
    const std::string expectedBase = expectedUpdaterOwnedBase(plan);
    const bool rootsValid =
        rootsAreDerivedAndNonOverlapping(plan, expectedBase);
    if (!rootsValid ||
        !invocationProcessesUseTrustedHosts(plan)) {
        ARIS_DEMO_STAGE(
            "roots-or-trusted-hosts", BootstrapExitCode::AuthorityRejected,
            rootsValid ? "trusted-hosts" : "roots");
        return static_cast<int>(BootstrapExitCode::AuthorityRejected);
    }
    WindowsBootstrapLaunchBrokerCapability brokerCapability;
    if (!makeLaunchBrokerCapability(handoff.launchBroker, brokerCapability)) {
        ARIS_DEMO_STAGE("launch-broker-capability",
                        BootstrapExitCode::AuthorityRejected, "");
        return static_cast<int>(BootstrapExitCode::AuthorityRejected);
    }
    auto process =
        CreateBrokeredNativeBootstrapProcessPort(brokerCapability);
    if (process == nullptr) {
        ARIS_DEMO_STAGE("brokered-process-port",
                        BootstrapExitCode::AuthorityRejected, "");
        return static_cast<int>(BootstrapExitCode::AuthorityRejected);
    }
    if (options.progress) options.progress->SetTargetVersion(plan.version);

    WindowsBootstrapFilesystemTrustedRootPolicy trustedRoots;
    trustedRoots.updaterOwnedBase = plan.invocation.updaterOwnedBase;
    trustedRoots.invocationIdentity = plan.invocation.invocationIdentity;
    trustedRoots.stagingRoot = plan.invocation.stagingRoot;
    trustedRoots.transactionRoot = plan.invocation.transactionRoot;
    trustedRoots.expectedUpdaterOwnedBase = expectedBase;
    trustedRoots.authenticatedParentProcess = channel.parentProcess;
    trustedRoots.authenticated = true;
    auto backend = CreateNativeWindowsBootstrapFilesystemBackend(trustedRoots);
    if (backend == nullptr) {
        ARIS_DEMO_STAGE("filesystem-backend", BootstrapExitCode::Unsupported, "");
        return static_cast<int>(BootstrapExitCode::Unsupported);
    }
    WindowsBootstrapFilesystemPort filesystem(*backend);
    auto health = CreateNativeBootstrapHealthPort();
    if (health == nullptr) {
        ARIS_DEMO_STAGE("health-port", BootstrapExitCode::Unsupported, "");
        return static_cast<int>(BootstrapExitCode::Unsupported);
    }
    auto terminalCleanup = CreateNativeBootstrapTerminalCleanupPort();
    if (terminalCleanup == nullptr) {
        ARIS_DEMO_STAGE("terminal-cleanup-port",
                        BootstrapExitCode::Unsupported, "");
        return static_cast<int>(BootstrapExitCode::Unsupported);
    }
    WindowsBootstrapFilesystemRoots roots;
    roots.stagingRoot = plan.invocation.stagingRoot;
    roots.transactionRoot = plan.invocation.transactionRoot;
    roots.updaterOwnedBase = plan.invocation.updaterOwnedBase;
    roots.invocationIdentity = plan.invocation.invocationIdentity;
    WindowsBootstrapFilesystemDriver filesystemDriver(filesystem);
    BootstrapApplicationDependencies dependencies{
        *process, *health, filesystemDriver, terminalCleanup.get(), options.progress};
    BootstrapApplicationOptions applicationOptions;
    applicationOptions.oldProcessWaitMs = options.oldProcessWaitMs;
    applicationOptions.healthWaitMs = options.healthWaitMs;
    applicationOptions.previousFailureCount = invocation.previousFailureCount;
    applicationOptions.recovery = false;
    // The runnable product contract is forward-only.  A failed replacement,
    // launch, or health observation is terminal; it never restores the old
    // installed payload. The legacy reducer remains testable in isolation but
    // is unreachable from the shipped executable.
    applicationOptions.forwardOnly = true;
    const BootstrapApplicationResult result = RunBootstrapApplication(
        *admission, roots, dependencies, applicationOptions);
    const int mapped = static_cast<int>(mapResult(result));
    ARIS_DEMO_STAGE("application", mapped, result.reason.c_str());
    return mapped;
}

int RunHarkenSearchBootstrapTerminalCleanupEntrypoint(
    const std::uintptr_t inheritedHandoffHandle,
    const std::uintptr_t inheritedReadyHandle,
    const std::uintptr_t inheritedParentProcessHandle,
    const PackagePlanTrustConfig& trust,
    const std::uint32_t channelTimeoutMs,
    const std::uint32_t parentWaitMs) {
#if defined(ARIS_UPDATE_LOCAL_DEMO) && ARIS_UPDATE_LOCAL_DEMO
    resetLocalDemoTerminalStageDiagnostic();
    writeLocalDemoTerminalStageDiagnostic("entry", 0);
#endif
    InheritedHandoffGuard handoffGuard(inheritedHandoffHandle);
    InheritedTerminalReadyGuard readyGuard(inheritedReadyHandle);
    InheritedParentProcessGuard parentGuard(inheritedParentProcessHandle);
    if (inheritedReadyHandle == 0U || !trust.provisioned ||
        channelTimeoutMs == 0U ||
        channelTimeoutMs > kMaximumChannelWaitMs || parentWaitMs == 0U ||
        parentWaitMs > kMaximumOldProcessWaitMs) {
#if defined(ARIS_UPDATE_LOCAL_DEMO) && ARIS_UPDATE_LOCAL_DEMO
        writeLocalDemoTerminalStageDiagnostic(
            "initial-validation", static_cast<int>(BootstrapExitCode::InvocationRejected));
#endif
        return static_cast<int>(BootstrapExitCode::InvocationRejected);
    }

    const BootstrapTerminalChannelResult channel =
        ReadAuthenticatedBootstrapTerminalChannel(
            inheritedHandoffHandle, inheritedParentProcessHandle,
            channelTimeoutMs);
#if defined(ARIS_UPDATE_LOCAL_DEMO) && ARIS_UPDATE_LOCAL_DEMO
    writeLocalDemoTerminalChannelDiagnostic(channel);
    writeLocalDemoTerminalStageDiagnostic(
        "channel-read", static_cast<int>(channel.status), channel.nativeError);
#endif
    if (!channel.ok()) {
        ARIS_DEMO_STAGE("terminal-channel", BootstrapExitCode::ChannelRejected,
                        "");
#if defined(ARIS_UPDATE_LOCAL_DEMO) && ARIS_UPDATE_LOCAL_DEMO
        writeLocalDemoTerminalStageDiagnostic(
            "terminal-channel", static_cast<int>(BootstrapExitCode::ChannelRejected),
            channel.nativeError);
#endif
        return static_cast<int>(BootstrapExitCode::ChannelRejected);
    }
#if defined(ARIS_UPDATE_LOCAL_DEMO) && ARIS_UPDATE_LOCAL_DEMO
    writeLocalDemoTerminalStageDiagnostic("channel-accepted", 0);
#endif
    const BootstrapTerminalCleanupHandoff& handoff = *channel.handoff;
    if (handoff.transactionId.empty()) {
#if defined(ARIS_UPDATE_LOCAL_DEMO) && ARIS_UPDATE_LOCAL_DEMO
        writeLocalDemoTerminalStageDiagnostic(
            "transaction-id", static_cast<int>(BootstrapExitCode::InvocationRejected));
#endif
        return static_cast<int>(BootstrapExitCode::InvocationRejected);
    }

    const PackagePlanAdmission package = VerifyPackagePlan(
        handoff.packagePlanBytes, handoff.packageSignatureHex, trust);
    if (!package.ok() || !package.plan.has_value()) {
        ARIS_DEMO_STAGE("terminal-package", BootstrapExitCode::PackageRejected,
                        "");
#if defined(ARIS_UPDATE_LOCAL_DEMO) && ARIS_UPDATE_LOCAL_DEMO
        writeLocalDemoTerminalStageDiagnostic(
            "terminal-package", static_cast<int>(BootstrapExitCode::PackageRejected));
#endif
        return static_cast<int>(BootstrapExitCode::PackageRejected);
    }
#if defined(ARIS_UPDATE_LOCAL_DEMO) && ARIS_UPDATE_LOCAL_DEMO
    writeLocalDemoTerminalStageDiagnostic("package-accepted", 0);
#endif
    const VerifiedPackagePlanBindingResult binding = VerifyPackagePlanBinding(
        handoff.packagePlanBytes, handoff.packageSignatureHex, trust);
    if (!binding.ok() || !binding.binding.has_value() ||
        !EvaluateHarkenSearchPackagePolicy(*binding.binding).ok()) {
        ARIS_DEMO_STAGE("terminal-package-policy",
                        BootstrapExitCode::PackageRejected, "");
#if defined(ARIS_UPDATE_LOCAL_DEMO) && ARIS_UPDATE_LOCAL_DEMO
        writeLocalDemoTerminalStageDiagnostic(
            "terminal-package-policy", static_cast<int>(BootstrapExitCode::PackageRejected));
#endif
        return static_cast<int>(BootstrapExitCode::PackageRejected);
    }
    const InvocationDecodeResult decoded =
        DecodeInvocationEnvelope(handoff.invocationBytes);
    const bool invocationTransactionMatches =
        decoded.invocation.has_value() &&
        decoded.invocation->transactionId == handoff.transactionId;
#if defined(ARIS_UPDATE_LOCAL_DEMO) && ARIS_UPDATE_LOCAL_DEMO
    const std::string invocationDecodeDetail =
        std::string("status=") +
        std::to_string(static_cast<int>(decoded.status)) +
        " has_invocation=" +
        std::to_string(decoded.invocation.has_value() ? 1 : 0) +
        " transaction_match=" +
        std::to_string(invocationTransactionMatches ? 1 : 0) +
        " bytes=" + std::to_string(handoff.invocationBytes.size()) +
        " diagnostic_bytes=" + std::to_string(decoded.diagnostic.size()) +
        " diagnostic=" + decoded.diagnostic;
    writeLocalDemoTerminalStageDiagnostic(
        "invocation-decode-status", static_cast<int>(decoded.status), 0U,
        invocationDecodeDetail.c_str());
#endif
    if (!decoded.ok() || !invocationTransactionMatches) {
        ARIS_DEMO_STAGE("terminal-invocation",
                        BootstrapExitCode::InvocationRejected, "");
#if defined(ARIS_UPDATE_LOCAL_DEMO) && ARIS_UPDATE_LOCAL_DEMO
        writeLocalDemoTerminalStageDiagnostic(
            "terminal-invocation", static_cast<int>(BootstrapExitCode::InvocationRejected));
#endif
        return static_cast<int>(BootstrapExitCode::InvocationRejected);
    }
    const BootstrapAdmission admission = AdmitBootstrapTerminalCleanup(
        package, *decoded.invocation);
    if (!admission.ok() || !admission.plan.has_value() ||
        !admission.journal.has_value()) {
        ARIS_DEMO_STAGE("terminal-admission",
                        BootstrapExitCode::AdmissionRejected, "");
#if defined(ARIS_UPDATE_LOCAL_DEMO) && ARIS_UPDATE_LOCAL_DEMO
        writeLocalDemoTerminalStageDiagnostic(
            "terminal-admission", static_cast<int>(BootstrapExitCode::AdmissionRejected));
#endif
        return static_cast<int>(BootstrapExitCode::AdmissionRejected);
    }
#if defined(ARIS_UPDATE_LOCAL_DEMO) && ARIS_UPDATE_LOCAL_DEMO
    writeLocalDemoTerminalStageDiagnostic("admission-accepted", 0);
#endif
    const ValidatedInstallPlan& plan = *admission.plan;
    const std::string expectedBase = expectedUpdaterOwnedBase(plan);
    if (!rootsAreDerivedAndNonOverlapping(plan, expectedBase)) {
        ARIS_DEMO_STAGE("terminal-roots", BootstrapExitCode::AuthorityRejected,
                        "");
#if defined(ARIS_UPDATE_LOCAL_DEMO) && ARIS_UPDATE_LOCAL_DEMO
        writeLocalDemoTerminalStageDiagnostic(
            "terminal-roots", static_cast<int>(BootstrapExitCode::AuthorityRejected));
#endif
        return static_cast<int>(BootstrapExitCode::AuthorityRejected);
    }

    const BootstrapTerminalReadyResult ready = SignalBootstrapTerminalReady(
        inheritedReadyHandle, plan.invocation.transactionId, channelTimeoutMs);
    if (!ready.ok()) {
        ARIS_DEMO_STAGE("terminal-ready", BootstrapExitCode::AuthorityRejected,
                        "");
#if defined(ARIS_UPDATE_LOCAL_DEMO) && ARIS_UPDATE_LOCAL_DEMO
        writeLocalDemoTerminalStageDiagnostic(
            "terminal-ready", static_cast<int>(BootstrapExitCode::AuthorityRejected),
            ready.nativeError);
#endif
        return static_cast<int>(BootstrapExitCode::AuthorityRejected);
    }
#if defined(ARIS_UPDATE_LOCAL_DEMO) && ARIS_UPDATE_LOCAL_DEMO
    writeLocalDemoTerminalStageDiagnostic("ready-sent", 0);
#endif
    // Closing the inherited write end is the authenticated end-of-ack marker;
    // the current Bootstrap may exit only after the parent has read it.
    readyGuard.close();

    // The inherited process HANDLE is the only lifecycle authority.  Wait
    // before opening any writable filesystem capability; the successor never
    // races the runtime-pair Bootstrap's final cleanup.
    const BootstrapTerminalParentWaitResult parentWait =
        WaitForBootstrapTerminalParentExit(inheritedParentProcessHandle,
                                           channel.parentProcess, parentWaitMs);
    if (!parentWait.ok()) {
        ARIS_DEMO_STAGE("terminal-parent-wait",
                        BootstrapExitCode::AuthorityRejected, "");
#if defined(ARIS_UPDATE_LOCAL_DEMO) && ARIS_UPDATE_LOCAL_DEMO
        writeLocalDemoTerminalStageDiagnostic(
            "terminal-parent-wait", static_cast<int>(BootstrapExitCode::AuthorityRejected),
            parentWait.nativeError);
#endif
        return static_cast<int>(BootstrapExitCode::AuthorityRejected);
    }

    WindowsBootstrapFilesystemTrustedRootPolicy trustedRoots;
    trustedRoots.updaterOwnedBase = plan.invocation.updaterOwnedBase;
    trustedRoots.invocationIdentity = plan.invocation.invocationIdentity;
    trustedRoots.stagingRoot = plan.invocation.stagingRoot;
    trustedRoots.transactionRoot = plan.invocation.transactionRoot;
    trustedRoots.expectedUpdaterOwnedBase = expectedBase;
    trustedRoots.authenticatedParentProcess = plan.invocation.parentProcess;
    trustedRoots.terminalCleanup = true;
    trustedRoots.terminalCleanupParentProcess = channel.parentProcess;
    trustedRoots.authenticated = true;
    auto backend = CreateNativeWindowsBootstrapFilesystemBackend(trustedRoots);
    if (backend == nullptr) {
#if defined(ARIS_UPDATE_LOCAL_DEMO) && ARIS_UPDATE_LOCAL_DEMO
        writeLocalDemoTerminalStageDiagnostic(
            "filesystem-backend", static_cast<int>(BootstrapExitCode::Unsupported));
#endif
        return static_cast<int>(BootstrapExitCode::Unsupported);
    }
    WindowsBootstrapFilesystemPort filesystem(*backend);
    WindowsBootstrapFilesystemRoots roots;
    roots.stagingRoot = plan.invocation.stagingRoot;
    roots.transactionRoot = plan.invocation.transactionRoot;
    roots.updaterOwnedBase = plan.invocation.updaterOwnedBase;
    roots.invocationIdentity = plan.invocation.invocationIdentity;

    const WindowsBootstrapFilesystemRecoveryResult recovery =
        filesystem.BeginTerminalCleanup(*admission.plan, *admission.journal,
                                        roots, channel.parentProcess);
#if defined(ARIS_UPDATE_LOCAL_DEMO) && ARIS_UPDATE_LOCAL_DEMO
    writeLocalDemoTerminalRecoveryDiagnostic(recovery);
#endif
    if (!recovery.ok() ||
        recovery.disposition !=
            WindowsBootstrapRecoveryDisposition::CleanupCommitted) {
        const std::string extra =
            FormatTerminalFilesystemAdmissionFailure(recovery);
        ARIS_DEMO_STAGE("terminal-filesystem-admission",
                        BootstrapExitCode::FilesystemRejected, extra.c_str());
#if defined(ARIS_UPDATE_LOCAL_DEMO) && ARIS_UPDATE_LOCAL_DEMO
        writeLocalDemoTerminalStageDiagnostic(
            "terminal-filesystem-admission",
            static_cast<int>(BootstrapExitCode::FilesystemRejected));
#endif
        return static_cast<int>(BootstrapExitCode::FilesystemRejected);
    }
    for (const BootstrapIntent& intent : recovery.intents) {
        // The terminal successor has one deliberately narrow capability:
        // consume the authenticated, path-free RemoveBackup intent only.
        // Recovery, replacement, quarantine, and PID/path-bearing intents
        // must never reach the terminal filesystem writer.
        if (intent.kind != BootstrapIntentKind::RemoveBackup ||
            !intent.relativePath.empty() || intent.processId != 0U) {
            ARIS_DEMO_STAGE("terminal-intent",
                            BootstrapExitCode::FilesystemRejected, "");
#if defined(ARIS_UPDATE_LOCAL_DEMO) && ARIS_UPDATE_LOCAL_DEMO
            writeLocalDemoTerminalStageDiagnostic(
                "terminal-intent", static_cast<int>(BootstrapExitCode::FilesystemRejected));
#endif
            return static_cast<int>(BootstrapExitCode::FilesystemRejected);
        }
        const WindowsBootstrapFilesystemResult applied =
            runTerminalCleanupWithBoundedRetry(
                [&filesystem, &intent] { return filesystem.Apply(intent); });
        if (!applied.ok()) {
            ARIS_DEMO_STAGE("terminal-remove-backup",
                            BootstrapExitCode::FilesystemRejected, "");
#if defined(ARIS_UPDATE_LOCAL_DEMO) && ARIS_UPDATE_LOCAL_DEMO
            writeLocalDemoTerminalStageDiagnostic(
                "terminal-remove-backup",
                static_cast<int>(BootstrapExitCode::FilesystemRejected),
                applied.nativeError);
#endif
            return static_cast<int>(BootstrapExitCode::FilesystemRejected);
        }
    }
    const WindowsBootstrapFilesystemResult cleaned =
        runTerminalCleanupWithBoundedRetry(
            [&filesystem] { return filesystem.CleanupCommittedInvocation(); });
    if (!cleaned.ok()) {
        ARIS_DEMO_STAGE("terminal-invocation-cleanup",
                        BootstrapExitCode::FilesystemRejected, "");
#if defined(ARIS_UPDATE_LOCAL_DEMO) && ARIS_UPDATE_LOCAL_DEMO
        writeLocalDemoTerminalStageDiagnostic(
            "terminal-invocation-cleanup",
            static_cast<int>(BootstrapExitCode::FilesystemRejected),
            cleaned.nativeError);
#endif
        return static_cast<int>(BootstrapExitCode::FilesystemRejected);
    }
    const WindowsBootstrapFilesystemResult retired =
        runTerminalCleanupWithBoundedRetry(
            [&filesystem] { return filesystem.RetireTerminalJournal(); });
    if (!retired.ok()) {
        ARIS_DEMO_STAGE("terminal-journal-retirement",
                        BootstrapExitCode::FilesystemRejected, "");
#if defined(ARIS_UPDATE_LOCAL_DEMO) && ARIS_UPDATE_LOCAL_DEMO
        writeLocalDemoTerminalStageDiagnostic(
            "terminal-journal-retirement",
            static_cast<int>(BootstrapExitCode::FilesystemRejected),
            retired.nativeError);
#endif
        return static_cast<int>(BootstrapExitCode::FilesystemRejected);
    }
    ARIS_DEMO_STAGE("terminal-complete", BootstrapExitCode::Completed, "");
#if defined(ARIS_UPDATE_LOCAL_DEMO) && ARIS_UPDATE_LOCAL_DEMO
    writeLocalDemoTerminalStageDiagnostic(
        "terminal-complete", static_cast<int>(BootstrapExitCode::Completed));
#endif
    return static_cast<int>(BootstrapExitCode::Completed);
}

}  // namespace aris::bootstrap
