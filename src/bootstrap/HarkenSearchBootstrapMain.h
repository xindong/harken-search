#pragma once

#include "bootstrap/HarkenSearchBootstrapAdmission.h"
#include "bootstrap/BootstrapProgress.h"

#include <cstdint>

namespace aris::bootstrap {

// The process entrypoint owns no caller-supplied path or native handle
// authority. `handoffHandle` is an inherited one-shot pipe and
// `parentProcessHandle` is a restricted inherited HANDLE pinning the exact
// process lifetime that created it. All roots and identities still come from
// the authenticated envelope. The trust configuration is compiled into the
// executable in production. Test harnesses may provide an explicit fixture
// configuration when they compile this entrypoint with a test-only target.
struct BootstrapEntrypointOptions {
    bool recovery = false;
    std::uint32_t channelTimeoutMs = 5000U;
    std::uint32_t oldProcessWaitMs = 60'000U;
    std::uint32_t healthWaitMs = 120'000U;
    std::uint32_t previousFailureCount = 0U;
    BootstrapProgressSink* progress = nullptr;
};

enum class BootstrapExitCode : int {
    Completed = 0,
    ChannelRejected = 10,
    PackageRejected = 11,
    InvocationRejected = 12,
    AuthorityRejected = 13,
    AdmissionRejected = 14,
    OldProcessRejected = 20,
    OldProcessTimedOut = 21,
    FilesystemRejected = 30,
    LaunchRejected = 40,
    HealthRejected = 50,
    RolledBack = 60,
    RolledBackHealthTimedOut = 61,
    RolledBackChildExited = 62,
    RolledBackHealthRejected = 63,
    Quarantined = 70,
    QuarantinedRollbackFailed = 71,
    QuarantinedPreviousLaunchFailed = 72,
    QuarantinedChildTerminationFailed = 73,
    QuarantinedChildReleaseFailed = 74,
    QuarantinedRecoveryRejected = 75,
    QuarantinedMutationFailed = 76,
    QuarantinedHealthPrepareFailed = 77,
    QuarantinedUpdatedLaunchFailed = 80,
    QuarantinedUpdatedCheckpointFailed = 81,
    QuarantinedUpdatedResumeFailed = 82,
    QuarantinedHealthFailed = 83,
    QuarantinedCommitFailed = 84,
    QuarantinedSnapshotFailed = 85,
    QuarantinedReplacementFailed = 86,
    ReinstallRequired = 87,
    Unprovisioned = 78,
    Unsupported = 79,
};

int RunHarkenSearchBootstrapEntrypoint(
    std::uintptr_t inheritedHandoffHandle,
    std::uintptr_t inheritedParentProcessHandle,
    const PackagePlanTrustConfig& trust,
    const BootstrapEntrypointOptions& options = {});

// Dedicated terminal successor entrypoint. It consumes only the signed
// terminal handoff, waits for the exact inherited runtime-pair Bootstrap
// HANDLE to signal, then admits and cleans the committed transaction. It is
// intentionally separate from normal forward install and --recovery.
int RunHarkenSearchBootstrapTerminalCleanupEntrypoint(
    std::uintptr_t inheritedHandoffHandle,
    std::uintptr_t inheritedReadyHandle,
    std::uintptr_t inheritedParentProcessHandle,
    const PackagePlanTrustConfig& trust,
    std::uint32_t channelTimeoutMs = 5000U,
    std::uint32_t parentWaitMs = 60'000U);

}  // namespace aris::bootstrap
