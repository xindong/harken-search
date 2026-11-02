#pragma once

#include "bootstrap/BootstrapProgress.h"

#include "bootstrap/HarkenSearchBootstrapAdmission.h"
#include "bootstrap/WindowsBootstrapFilesystem.h"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace aris::bootstrap {

enum class BootstrapProcessStatus {
    Ok,
    InvalidIdentity,
    NotFound,
    OpenFailed,
    IdentityMismatch,
    TimedOut,
    WaitFailed,
    LaunchFailed,
    TerminationFailed,
    Unsupported,
};

struct BootstrapOwnedProcess {
    BootstrapProcessIdentity identity;
    // Opaque ownership token. Native ports keep the actual HANDLE in their
    // private object; tests use a stable nonzero token.
    std::uintptr_t ownershipToken = 0;
    // Opaque native wait capability retained by the same process port. It is
    // never supplied by callers and remains zero for simple fakes.
    std::uintptr_t waitToken = 0;

    bool valid() const noexcept {
        return identity.valid && ownershipToken != 0U;
    }
};

// The expected installed image is derived from either the validated signed
// inventory or an exact old-image handle retained by the process port. It is
// an owned value: no native handle, PID, or caller-supplied path crosses the
// runtime boundary. exactFileIdentity is set only for a handle-bound snapshot.
struct BootstrapExecutableExpectation {
    std::string absolutePath;
    std::string sha256;
    std::uint64_t byteSize = 0;
    WindowsFileIdentity exactFileIdentity;

    bool valid() const noexcept {
        return !absolutePath.empty() && sha256.size() == 64U;
    }
};

struct BootstrapLaunchCapability {
    // An inherited, allowlisted handle selected by the health port.  The
    // value is never serialized or accepted from a command line.
    std::uintptr_t inheritedStdout = 0;
    std::uintptr_t inheritedStdin = 0;

    bool valid() const noexcept {
        return inheritedStdout != 0U || inheritedStdin != 0U;
    }
};

struct BootstrapProcessResult {
    BootstrapProcessStatus status = BootstrapProcessStatus::Unsupported;
    std::uint32_t nativeError = 0;
    BootstrapOwnedProcess process;
    BootstrapExecutableExpectation executable;

    bool ok() const noexcept { return status == BootstrapProcessStatus::Ok; }
};

class BootstrapProcessPort {
public:
    virtual ~BootstrapProcessPort() = default;

    // OpenExpected retains the synchronization/query handle and verifies the
    // complete identity tuple before any wait.  A caller cannot nominate a
    // PID without the corresponding creation time and executable identity.
    virtual BootstrapProcessResult OpenExpected(
        const BootstrapProcessIdentity& expected) = 0;
    virtual BootstrapProcessResult WaitForExit(
        BootstrapOwnedProcess& process, std::uint32_t timeoutMs) = 0;
    virtual BootstrapProcessResult Launch(
        std::string_view absoluteExecutable,
        std::string_view workingDirectory,
        const BootstrapLaunchCapability& capability) = 0;
    virtual BootstrapProcessResult LaunchSuspended(
        std::string_view absoluteExecutable,
        std::string_view workingDirectory,
        const BootstrapLaunchCapability& capability) {
        return Launch(absoluteExecutable, workingDirectory, capability);
    }
    // Native ports pin the exact image, reject reparse/path/FileId
    // substitution, and verify the signed inventory size/digest. A freshly
    // launched child must still be suspended; restart recovery may validate a
    // running child only after RebindOwnedChild has recreated that exact pin.
    // The default value-only check is retained for pure fakes and remains
    // fail-closed on an invalid expectation.
    virtual BootstrapProcessResult ValidateOwnedExecutable(
        const BootstrapOwnedProcess& process,
        const BootstrapExecutableExpectation& expected) {
        if (!process.valid() || !expected.valid()) {
            return {BootstrapProcessStatus::InvalidIdentity};
        }
        return {BootstrapProcessStatus::Ok};
    }
    virtual BootstrapProcessResult ResumeOwned(BootstrapOwnedProcess&) {
        return {BootstrapProcessStatus::Ok};
    }
    // Revalidates the exact live child before commit while retaining armed
    // process-job containment. Only ReleaseOwned after durable CommitJournal
    // may disarm and hand the child to its installed-application lifetime.
    virtual BootstrapProcessResult PrepareOwnedForCommit(
        BootstrapOwnedProcess&) {
        return {BootstrapProcessStatus::Ok};
    }
    virtual BootstrapProcessResult RebindOwnedChild(
        const BootstrapProcessIdentity& expected) {
        return OpenExpected(expected);
    }
    // Termination consumes only an exact child capability returned by
    // LaunchSuspended or RebindOwnedChild. It never targets the old process
    // capability and never accepts a bare PID.
    virtual BootstrapProcessResult TerminateOwned(
        BootstrapOwnedProcess& process, std::uint32_t exitCode) = 0;
    virtual BootstrapProcessResult ReleaseOwned(
        BootstrapOwnedProcess& process) {
        Close(process);
        return {BootstrapProcessStatus::Ok};
    }
    virtual void Close(BootstrapOwnedProcess& process) noexcept = 0;
};

enum class BootstrapHealthStatus {
    Accepted,
    TimedOut,
    ChildExited,
    ChannelRejected,
    Malformed,
    ReplayRejected,
    Unsupported,
};

struct BootstrapHealthResult {
    BootstrapHealthStatus status = BootstrapHealthStatus::Unsupported;
    std::uint32_t nativeError = 0;
    HealthAcknowledgement acknowledgement;

    bool ok() const noexcept { return status == BootstrapHealthStatus::Accepted; }
};

class BootstrapHealthPort {
public:
    virtual ~BootstrapHealthPort() = default;
    // Prepare creates the one-shot channel before the child launch and
    // returns only an opaque inherited-handle argument.  The nonce itself is
    // never placed in a command line or log.
    virtual BootstrapHealthResult Prepare(
        std::string_view nonce,
        std::string_view version,
        std::string_view buildId,
        BootstrapLaunchCapability& capability) = 0;
    virtual void ReleaseLaunchCapability() noexcept = 0;
    virtual BootstrapHealthResult Await(
        const BootstrapOwnedProcess& child,
        std::string_view nonce,
        std::string_view version,
        std::string_view buildId,
        std::uint32_t timeoutMs) = 0;
    virtual void Close() noexcept = 0;
};

class BootstrapFilesystemDriver {
public:
    virtual ~BootstrapFilesystemDriver() = default;
    virtual WindowsBootstrapFilesystemResult Begin(
        const ValidatedInstallPlan& plan,
        const ReplacementJournal& journal,
        const WindowsBootstrapFilesystemRoots& roots) = 0;
    // Binds authenticated invocation lifecycle state before the first
    // durable journal generation.  Implementations must reject late binding
    // so failure history cannot diverge from restart recovery.
    virtual WindowsBootstrapFilesystemResult BindPreviousFailureCount(
        std::uint32_t) {
        return {WindowsBootstrapFilesystemStatus::UnsupportedIntent};
    }
    virtual WindowsBootstrapFilesystemRecoveryResult BeginRecovery(
        const ValidatedInstallPlan& plan,
        const ReplacementJournal& journal,
        const WindowsBootstrapFilesystemRoots& roots) = 0;
    virtual WindowsBootstrapFilesystemRecoveryResult BeginRecovery(
        const ValidatedInstallPlan&,
        const WindowsBootstrapFilesystemRoots&) {
        return {};
    }
    virtual WindowsBootstrapFilesystemRecoveryResult BeginTerminalCleanup(
        const ValidatedInstallPlan&,
        const ReplacementJournal&,
        const WindowsBootstrapFilesystemRoots&,
        const BootstrapProcessIdentity&) {
        WindowsBootstrapFilesystemRecoveryResult result;
        result.filesystem =
            {WindowsBootstrapFilesystemStatus::UnsupportedIntent};
        result.disposition = WindowsBootstrapRecoveryDisposition::Quarantine;
        result.intents = {{BootstrapIntentKind::QuarantinePackage, {}, 0,
                           "bootstrap-terminal-cleanup-unsupported"}};
        return result;
    }
    virtual WindowsBootstrapFilesystemResult Apply(const BootstrapIntent& intent) = 0;
    virtual WindowsBootstrapFilesystemResult RecordUpdatedChildIdentity(
        const BootstrapProcessIdentity&) {
        return {WindowsBootstrapFilesystemStatus::UnsupportedIntent};
    }
    virtual WindowsBootstrapFilesystemResult RecordHealthState(
        std::string_view, bool) {
        return {WindowsBootstrapFilesystemStatus::UnsupportedIntent};
    }
    virtual WindowsBootstrapFilesystemResult PrepareRecoveredHealthRelaunch(
        const BootstrapProcessIdentity&) {
        return {WindowsBootstrapFilesystemStatus::UnsupportedIntent};
    }
    virtual std::optional<BootstrapProcessIdentity>
    RecoveredUpdatedChildIdentity() const { return std::nullopt; }
    virtual std::string RecoveredHealthPhase() const { return {}; }
    virtual std::optional<std::uint32_t>
    RecoveredPreviousFailureCount() const { return std::nullopt; }
    virtual bool RecoveredFailureRecordedForTransaction() const {
        return false;
    }
    virtual bool RecoveredRollbackComplete() const { return false; }
    virtual bool RecoveredPreviousApplicationRelaunchStarted() const {
        return false;
    }
    virtual bool RecoveredPreviousApplicationRelaunchComplete() const {
        return false;
    }
    virtual std::optional<BootstrapProcessIdentity>
    RecoveredPreviousApplicationIdentity() const { return std::nullopt; }
    virtual std::optional<WindowsFileContentFingerprint>
    PreviousApplicationContentFingerprint() const { return std::nullopt; }
    virtual WindowsBootstrapFilesystemResult
    RecordPreviousApplicationRelaunchStarted() {
        return {WindowsBootstrapFilesystemStatus::UnsupportedIntent};
    }
    virtual WindowsBootstrapFilesystemResult RecordPreviousApplicationIdentity(
        const BootstrapProcessIdentity&) {
        return {WindowsBootstrapFilesystemStatus::UnsupportedIntent};
    }
    virtual WindowsBootstrapFilesystemResult ResetPreviousApplicationIdentity(
        const BootstrapProcessIdentity&) {
        return {WindowsBootstrapFilesystemStatus::UnsupportedIntent};
    }
    virtual WindowsBootstrapFilesystemResult
    RecordPreviousApplicationRelaunchComplete(
        const BootstrapProcessIdentity&) {
        return {WindowsBootstrapFilesystemStatus::UnsupportedIntent};
    }
    virtual WindowsBootstrapFilesystemResult RetireTerminalJournal() {
        return {WindowsBootstrapFilesystemStatus::UnsupportedIntent};
    }
    virtual WindowsBootstrapFilesystemResult CleanupCommittedInvocation() {
        // A replacement driver must explicitly implement terminal invocation
        // residue cleanup for both commit and rollback. Returning success here
        // would let the runtime retire the authenticated journal while
        // staging/runtime-pair files remain.
        return {WindowsBootstrapFilesystemStatus::UnsupportedIntent};
    }
};

enum class BootstrapTerminalCleanupStatus {
    Accepted,
    InvalidIdentity,
    LaunchFailed,
    Unsupported,
};

struct BootstrapTerminalCleanupResult {
    BootstrapTerminalCleanupStatus status =
        BootstrapTerminalCleanupStatus::Unsupported;
    std::uint32_t nativeError = 0;

    bool ok() const noexcept {
        return status == BootstrapTerminalCleanupStatus::Accepted;
    }
};

// A narrow successor capability used only after a terminal commit/rollback
// cleanup cut. The implementation must launch a separately authenticated
// Bootstrap image which waits for this process to exit before touching the
// durable invocation namespace. It is not a generic relaunch/recovery path.
class BootstrapTerminalCleanupPort {
public:
    virtual ~BootstrapTerminalCleanupPort() = default;
    virtual BootstrapTerminalCleanupResult LaunchSuccessor(
        const ValidatedInstallPlan& plan,
        const ReplacementJournal& journal,
        const BootstrapExecutableExpectation& expectedBootstrap) = 0;
};

// Production wrapper around the accepted filesystem port.  Keeping this
// wrapper typed lets tests inject a no-I/O driver while the composition target
// retains the existing durable/reparse-safe implementation.
class WindowsBootstrapFilesystemDriver final : public BootstrapFilesystemDriver {
public:
    explicit WindowsBootstrapFilesystemDriver(WindowsBootstrapFilesystemPort& port)
        : port_(port) {}

    WindowsBootstrapFilesystemResult Begin(
        const ValidatedInstallPlan& plan,
        const ReplacementJournal& journal,
        const WindowsBootstrapFilesystemRoots& roots) override {
        return port_.Begin(plan, journal, roots);
    }
    WindowsBootstrapFilesystemResult BindPreviousFailureCount(
        const std::uint32_t count) override {
        return port_.BindPreviousFailureCount(count);
    }
    WindowsBootstrapFilesystemRecoveryResult BeginRecovery(
        const ValidatedInstallPlan& plan,
        const ReplacementJournal& journal,
        const WindowsBootstrapFilesystemRoots& roots) override {
        return port_.BeginRecovery(plan, journal, roots);
    }
    WindowsBootstrapFilesystemRecoveryResult BeginRecovery(
        const ValidatedInstallPlan& plan,
        const WindowsBootstrapFilesystemRoots& roots) override {
        return port_.BeginRecovery(plan, roots);
    }
    WindowsBootstrapFilesystemRecoveryResult BeginTerminalCleanup(
        const ValidatedInstallPlan& plan,
        const ReplacementJournal& journal,
        const WindowsBootstrapFilesystemRoots& roots,
        const BootstrapProcessIdentity& inheritedParentProcess) override {
        return port_.BeginTerminalCleanup(plan, journal, roots,
                                          inheritedParentProcess);
    }
    WindowsBootstrapFilesystemResult Apply(const BootstrapIntent& intent) override {
        return port_.Apply(intent);
    }
    WindowsBootstrapFilesystemResult RecordUpdatedChildIdentity(
        const BootstrapProcessIdentity& identity) override {
        return port_.RecordUpdatedChildIdentity(identity);
    }
    WindowsBootstrapFilesystemResult RecordHealthState(
        const std::string_view healthPhase,
        const bool nonceConsumed) override {
        return port_.RecordHealthState(std::string(healthPhase), nonceConsumed);
    }
    WindowsBootstrapFilesystemResult PrepareRecoveredHealthRelaunch(
        const BootstrapProcessIdentity& terminatedChild) override {
        return port_.PrepareRecoveredHealthRelaunch(terminatedChild);
    }
    std::optional<BootstrapProcessIdentity> RecoveredUpdatedChildIdentity()
        const override {
        return port_.recoveredUpdatedChildIdentity();
    }
    std::string RecoveredHealthPhase() const override;
    std::optional<std::uint32_t> RecoveredPreviousFailureCount()
        const override {
        return port_.recoveredPreviousFailureCount();
    }
    bool RecoveredFailureRecordedForTransaction() const override {
        return port_.recoveredFailureRecordedForTransaction();
    }
    bool RecoveredRollbackComplete() const override {
        return port_.recoveredRollbackComplete();
    }
    bool RecoveredPreviousApplicationRelaunchStarted() const override {
        return port_.recoveredPreviousApplicationRelaunchStarted();
    }
    bool RecoveredPreviousApplicationRelaunchComplete() const override {
        return port_.recoveredPreviousApplicationRelaunchComplete();
    }
    std::optional<BootstrapProcessIdentity>
    RecoveredPreviousApplicationIdentity() const override {
        return port_.recoveredPreviousApplicationIdentity();
    }
    std::optional<WindowsFileContentFingerprint>
    PreviousApplicationContentFingerprint() const override {
        return port_.previousApplicationContentFingerprint();
    }
    WindowsBootstrapFilesystemResult
    RecordPreviousApplicationRelaunchStarted() override {
        return port_.RecordPreviousApplicationRelaunchStarted();
    }
    WindowsBootstrapFilesystemResult RecordPreviousApplicationIdentity(
        const BootstrapProcessIdentity& identity) override {
        return port_.RecordPreviousApplicationIdentity(identity);
    }
    WindowsBootstrapFilesystemResult ResetPreviousApplicationIdentity(
        const BootstrapProcessIdentity& identity) override {
        return port_.ResetPreviousApplicationIdentity(identity);
    }
    WindowsBootstrapFilesystemResult
    RecordPreviousApplicationRelaunchComplete(
        const BootstrapProcessIdentity& identity) override {
        return port_.RecordPreviousApplicationRelaunchComplete(identity);
    }
    WindowsBootstrapFilesystemResult RetireTerminalJournal() override {
        return port_.RetireTerminalJournal();
    }
    WindowsBootstrapFilesystemResult CleanupCommittedInvocation() override {
        return port_.CleanupCommittedInvocation();
    }

private:
    WindowsBootstrapFilesystemPort& port_;
};

enum class BootstrapApplicationStatus {
    Completed,
    // Forward-only updates keep the verified replacement once file mutation
    // has completed.  The child health channel may still be unavailable; the
    // installed files remain, but the update is not successful until health
    // is confirmed. Callers must retain the failure reason.
    InstalledHealthUnconfirmed,
    Recovered,
    AdmissionRejected,
    OldProcessRejected,
    OldProcessTimedOut,
    FilesystemRejected,
    LaunchRejected,
    HealthRejected,
    RolledBack,
    Quarantined,
    ReinstallRequired,
};

struct BootstrapApplicationResult {
    BootstrapApplicationStatus status = BootstrapApplicationStatus::AdmissionRejected;
    std::string reason;
    bool mutated = false;

    bool ok() const noexcept {
        return status == BootstrapApplicationStatus::Completed ||
               status == BootstrapApplicationStatus::Recovered;
    }
};

struct BootstrapApplicationDependencies {
    BootstrapProcessPort& process;
    BootstrapHealthPort& health;
    BootstrapFilesystemDriver& filesystem;
    BootstrapTerminalCleanupPort* terminalCleanup = nullptr;
    BootstrapProgressSink* progress = nullptr;
};

struct BootstrapApplicationOptions {
    std::uint32_t oldProcessWaitMs = 60'000U;
    std::uint32_t healthWaitMs = 120'000U;
    std::uint32_t previousFailureCount = 0;
    bool recovery = false;
    // True only when this recovery process received a fresh, fully decoded
    // InvocationEnvelope through the authenticated one-shot parent channel.
    // A journal digest or marker alone can never recreate this capability.
    bool recoveryHealthCapabilityAvailable = false;
    // Product composition is forward-only: after a signed replacement starts,
    // launch/health/filesystem failure is terminal and never restores the old
    // installed payload. Existing rollback/recovery tests can keep this false
    // while the runnable HarkenSearchBootstrap path sets it explicitly.
    bool forwardOnly = false;
};

BootstrapApplicationResult RunBootstrapApplication(
    const BootstrapAdmission& admission,
    const WindowsBootstrapFilesystemRoots& roots,
    BootstrapApplicationDependencies dependencies,
    const BootstrapApplicationOptions& options = {});

// Windows-only production ports.  They return nullptr on unsupported
// platforms so the executable fails closed rather than silently using a
// weaker PID/path implementation.
std::unique_ptr<BootstrapProcessPort> CreateNativeBootstrapProcessPort();
std::unique_ptr<BootstrapHealthPort> CreateNativeBootstrapHealthPort();
std::unique_ptr<BootstrapTerminalCleanupPort>
CreateNativeBootstrapTerminalCleanupPort();

// Native entrypoint boundary helpers.  The pure composition code does not
// include Win32 headers; the reviewed Windows adapter owns root derivation,
// inherited-channel handle cleanup, and the process entrypoint itself.
std::string NativeBootstrapUpdaterOwnedBase(
    const ValidatedInstallPlan& plan);
bool BootstrapParentImageMatchesUpdaterBase(
    const BootstrapProcessIdentity& parent,
    std::string_view updaterOwnedBase);
// Recovery derives the updater-owned anchor without accepting a path from the
// recovery channel or command line.
std::string NativeBootstrapUpdaterOwnedBase();
void CloseBootstrapChannelHandle(std::uintptr_t inheritedHandle) noexcept;
void CloseBootstrapParentProcessHandle(
    std::uintptr_t inheritedHandle) noexcept;

enum class BootstrapChannelStatus {
    Accepted,
    InvalidHandle,
    NotNamedPipe,
    ParentIdentityRejected,
    ReadFailed,
    TooLarge,
    Malformed,
    TimedOut,
    Unsupported,
};

struct BootstrapChannelResult {
    BootstrapChannelStatus status = BootstrapChannelStatus::Unsupported;
    std::uint32_t nativeError = 0;
    std::optional<BootstrapHandoff> handoff;
    BootstrapProcessIdentity parentProcess;
    // Unsigned release builds may proceed only when the authenticated pipe
    // parent is also the exact installed Aris process named by the invocation.
    // Signed publisher-matched updater parents do not require that restriction.
    bool requiresExactTargetParent = false;
    bool recoveryMarker = false;

    bool ok() const noexcept {
        return status == BootstrapChannelStatus::Accepted &&
               (handoff.has_value() || recoveryMarker);
    }
};

// Reads a one-shot handoff from a borrowed inherited named-pipe handle. The
// separately inherited parent process HANDLE pins the exact kernel lifetime
// that created the pipe; the server PID is only a cross-check and never
// authorizes a PID reopen. The caller closes both handles after this returns.
BootstrapChannelResult ReadAuthenticatedBootstrapChannel(
    std::uintptr_t inheritedHandle,
    std::uintptr_t inheritedParentProcessHandle,
    std::uint32_t timeoutMs = 5000U);

enum class BootstrapTerminalChannelStatus {
    Accepted,
    InvalidHandle,
    NotNamedPipe,
    ParentIdentityRejected,
    ReadFailed,
    TooLarge,
    Malformed,
    TimedOut,
    Unsupported,
};

struct BootstrapTerminalChannelResult {
    BootstrapTerminalChannelStatus status =
        BootstrapTerminalChannelStatus::Unsupported;
    std::uint32_t nativeError = 0;
    std::optional<BootstrapTerminalCleanupHandoff> handoff;
    BootstrapProcessIdentity parentProcess;

    bool ok() const noexcept {
        return status == BootstrapTerminalChannelStatus::Accepted &&
               handoff.has_value() && handoff->valid();
    }
};

// Terminal cleanup uses a separate decoder because the inherited parent is
// the runtime-pair Bootstrap rather than the original updater parent.  The
// named-pipe server PID and the inherited process HANDLE remain mandatory;
// the filesystem terminal-admission path binds the image/FileId to journal
// authority after the signed handoff is decoded.
BootstrapTerminalChannelResult ReadAuthenticatedBootstrapTerminalChannel(
    std::uintptr_t inheritedHandle,
    std::uintptr_t inheritedParentProcessHandle,
    std::uint32_t timeoutMs = 5000U);

enum class BootstrapTerminalReadyStatus {
    Sent,
    InvalidHandle,
    WriteFailed,
    TimedOut,
    Unsupported,
};

struct BootstrapTerminalReadyResult {
    BootstrapTerminalReadyStatus status =
        BootstrapTerminalReadyStatus::Unsupported;
    std::uint32_t nativeError = 0;

    bool ok() const noexcept {
        return status == BootstrapTerminalReadyStatus::Sent;
    }
};

// The terminal successor sends a one-shot pre-exit readiness acknowledgement
// over an inherited anonymous pipe after package, invocation, and root
// admission have all passed. The parent must receive this before it may exit.
BootstrapTerminalReadyResult SignalBootstrapTerminalReady(
    std::uintptr_t inheritedReadyHandle,
    std::string_view transactionId,
    std::uint32_t timeoutMs = 5000U);
void CloseBootstrapTerminalReadyHandle(std::uintptr_t inheritedHandle) noexcept;

enum class BootstrapTerminalParentWaitStatus {
    Exited,
    InvalidIdentity,
    TimedOut,
    WaitFailed,
    Unsupported,
};

struct BootstrapTerminalParentWaitResult {
    BootstrapTerminalParentWaitStatus status =
        BootstrapTerminalParentWaitStatus::Unsupported;
    std::uint32_t nativeError = 0;

    bool ok() const noexcept {
        return status == BootstrapTerminalParentWaitStatus::Exited;
    }
};

BootstrapTerminalParentWaitResult WaitForBootstrapTerminalParentExit(
    std::uintptr_t inheritedParentProcessHandle,
    const BootstrapProcessIdentity& expectedParent,
    std::uint32_t timeoutMs);

}  // namespace aris::bootstrap
