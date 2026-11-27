#pragma once

#include "bootstrap/HarkenSearchBootstrapPlan.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace aris::bootstrap {

struct WindowsBootstrapJournalRecord;
struct BootstrapChannelResult;
struct HarkenSearchPackagePolicyResult;

// This is the immutable package-side contract.  It is intentionally separate
// from SignedInstallPlan: roots, process identities and health capabilities
// arrive through the authenticated parent channel and are never release-key
// authority.
struct PackagePlan {
    std::uint32_t schemaVersion = 0;
    std::string product;
    std::string channel;
    std::string version;
    std::string buildId;
    std::string keyId;
    std::string assetSha256;
    std::uint64_t assetSize = 0;
    std::string entrypoint;
    std::vector<InstallInventoryItem> inventory;
    std::vector<std::string> removals;
};

struct PackagePlanTrustConfig {
    // Production construction must set provisioned=true only when the
    // release build injected an approved key and matching key id.  No default
    // or test key is hidden in the production path.
    std::string keyId;
    std::string publicKeyHex;
    bool provisioned = false;
};

using PackageSignatureVerifier = bool (*)(
    std::string_view packageBytes,
    std::string_view signatureHex,
    std::string_view keyId,
    std::string* diagnostic);

enum class PackagePlanAdmissionStatus {
    Accepted,
    TooLarge,
    InvalidJson,
    InvalidSchema,
    InvalidField,
    InvalidInventory,
    InvalidSignatureEncoding,
    InvalidKey,
    UnprovisionedKey,
    SignatureRejected,
};

struct PackagePlanAdmission {
    PackagePlanAdmissionStatus status =
        PackagePlanAdmissionStatus::InvalidField;
    std::optional<PackagePlan> plan;
    std::string packageDigest;
    // Exact bounded bytes and detached signature admitted by the release key.
    // These owned values are persisted into the durable recovery record; they
    // are never reconstructed from a digest or caller-provided path.
    std::string packageBytes;
    std::string packageSignatureHex;
    std::string diagnostic;

    bool ok() const noexcept {
        return status == PackagePlanAdmissionStatus::Accepted &&
               plan.has_value() && packageDigest.size() == 64U;
    }
};

// Verify the detached Ed25519 signature over the exact bounded bytes before
// parsing.  A test-only verifier may be supplied by contract tests; the
// production call leaves it null and uses TweetNaCl with the injected key.
PackagePlanAdmission VerifyPackagePlan(
    std::string_view packageBytes,
    std::string_view signatureHex,
    const PackagePlanTrustConfig& trust,
    PackageSignatureVerifier testVerifier = nullptr);

// A durable journal is an untrusted restart input.  Reopening it must bind
// the exact package bytes and detached signature back to the already admitted
// immutable plan before any filesystem or process mutation.  This result is a
// pure value check: it performs no I/O and callers may inject a verifier only
// in focused tests.
enum class DurablePackageAuthorityStatus {
    Accepted,
    InvalidPlan,
    PackageTooLarge,
    MissingPackageBytes,
    MissingPackageSignature,
    InvalidPackageDigest,
    PackageDigestMismatch,
    PackageReferenceMismatch,
    PackageKeyMismatch,
    SignatureRejected,
    PackagePlanMismatch,
};

struct DurablePackageAuthorityResult {
    DurablePackageAuthorityStatus status =
        DurablePackageAuthorityStatus::InvalidPlan;
    std::string diagnostic;

    bool ok() const noexcept {
        return status == DurablePackageAuthorityStatus::Accepted;
    }
};

DurablePackageAuthorityResult ValidateDurablePackageAuthority(
    const ValidatedInstallPlan& expectedPlan,
    std::string_view packageBytes,
    std::string_view signatureHex,
    const PackagePlanTrustConfig& trust,
    PackageSignatureVerifier testVerifier = nullptr);

enum class DurableRecoveryAdmissionStatus {
    Accepted,
    FreshHandoffRequired,
    ChannelRejected,
    InvocationRejected,
    InvocationBindingRejected,
    HealthCapabilityRejected,
    MissingPackageAuthority,
    PackageRejected,
    PackageDigestMismatch,
    RootBindingRejected,
    ParentIdentityRejected,
    InvalidPlan,
    InvalidJournal,
};

struct DurableRecoveryAdmissionResult {
    DurableRecoveryAdmissionStatus status =
        DurableRecoveryAdmissionStatus::InvalidPlan;
    std::optional<ValidatedInstallPlan> plan;
    std::optional<ReplacementJournal> journal;
    std::string diagnostic;

    bool ok() const noexcept {
        return status == DurableRecoveryAdmissionStatus::Accepted &&
               plan.has_value() && journal.has_value();
    }
};

// A durable record is restart evidence, not mutation authority.  This legacy
// seam is retained only so pre-release schema callers fail closed; it never
// returns an accepted recovery without a fresh authenticated handoff.
DurableRecoveryAdmissionResult AdmitDurableBootstrapRecovery(
    const WindowsBootstrapJournalRecord& record,
    const PackagePlanTrustConfig& trust,
    const BootstrapProcessIdentity& authenticatedParent,
    const std::string& expectedUpdaterOwnedBase,
    const std::string& expectedStagingRoot,
    const std::string& expectedTransactionRoot,
    PackageSignatureVerifier testVerifier = nullptr);

// Shared exact-byte SHA-256 binding primitive used by the durable journal;
// it is corruption/identity binding only and never replaces signature
// authority.
std::string BootstrapSha256Hex(std::string_view bytes);

// Dynamic values are read from an authenticated one-shot parent channel.  A
// parser enforces shape and bounds but does not treat path text or a PID as
// authority; AuthenticateInvocationChannel in the native runtime binds the
// channel's peer identity before this value reaches mutation.
struct InvocationEnvelope {
    std::string transactionId;
    aris::platform::DeploymentMode deploymentMode =
        aris::platform::DeploymentMode::Portable;
    std::string installationId;
    std::string registeredAppId;
    std::string targetRoot;
    aris::platform::DeploymentFileIdentity targetRootIdentity;
    aris::platform::DeploymentFileIdentity oldExecutableIdentity;
    std::string targetRootIdentityBinding;
    std::string oldExecutableIdentityBinding;
    std::string updaterOwnedBase;
    std::string stagingRoot;
    std::string transactionRoot;
    std::string invocationIdentity;
    std::string packagePlanDigest;
    std::string healthNonce;
    // Authenticated updater state carried in the one-shot envelope.  The
    // value is bounded and feeds the two-failure quarantine reducer; it is
    // never inferred from a PID, path, or journal checksum.
    std::uint32_t previousFailureCount = 0;
    std::uint64_t oldProcessId = 0;
    std::string entrypoint;
    BootstrapProcessIdentity parentProcess;
    BootstrapProcessIdentity oldProcess;
    std::vector<std::string> protectedDataRoots;
    std::vector<ExistingInstallItem> existingInventory;
};

enum class InvocationDecodeStatus {
    Accepted,
    TooLarge,
    InvalidJson,
    InvalidSchema,
    InvalidField,
    InvalidIdentity,
    InvalidInventory,
};

struct InvocationDecodeResult {
    InvocationDecodeStatus status = InvocationDecodeStatus::InvalidField;
    std::optional<InvocationEnvelope> invocation;
    std::string diagnostic;

    bool ok() const noexcept {
        return status == InvocationDecodeStatus::Accepted &&
               invocation.has_value();
    }
};

InvocationDecodeResult DecodeInvocationEnvelope(std::string_view bytes);

// Numeric handle values are meaningful only after the native channel has
// proved that the handles were explicitly inherited from the authenticated
// parent.  Keeping this DTO free of Win32 types lets updater/runtime code
// construct the one-shot handoff through a stable pure value boundary.
struct BootstrapLaunchBrokerHandoff {
    std::uint64_t channelHandle = 0;
    std::uint64_t brokerProcessHandle = 0;
    std::uint64_t containmentJobHandle = 0;
    BootstrapProcessIdentity brokerIdentity;
    std::uint32_t launchTimeoutMs = 0;

    bool valid() const noexcept {
        return channelHandle != 0U && brokerProcessHandle != 0U &&
               containmentJobHandle != 0U && brokerIdentity.valid &&
               launchTimeoutMs != 0U && launchTimeoutMs <= 60'000U;
    }
};

struct BootstrapHandoff {
    std::string packagePlanBytes;
    std::string packageSignatureHex;
    std::string invocationBytes;
    BootstrapLaunchBrokerHandoff launchBroker;
};

enum class BootstrapHandoffDecodeStatus {
    Accepted,
    TooLarge,
    InvalidJson,
    InvalidSchema,
    InvalidField,
};

struct BootstrapHandoffDecodeResult {
    BootstrapHandoffDecodeStatus status =
        BootstrapHandoffDecodeStatus::InvalidField;
    std::optional<BootstrapHandoff> handoff;
    std::string diagnostic;

    bool ok() const noexcept {
        return status == BootstrapHandoffDecodeStatus::Accepted &&
               handoff.has_value();
    }
};

BootstrapHandoffDecodeResult DecodeBootstrapHandoff(std::string_view bytes);

// A terminal-cleanup handoff contains only the release-signed package bytes
// and the original authenticated invocation envelope. It is accepted by a
// dedicated successor entrypoint after the inherited parent HANDLE has
// signalled; it is never a normal launch/recovery handoff.
struct BootstrapTerminalCleanupHandoff {
    std::string packagePlanBytes;
    std::string packageSignatureHex;
    std::string invocationBytes;
    std::string transactionId;

    bool valid() const noexcept {
        return !packagePlanBytes.empty() &&
               packageSignatureHex.size() == 128U &&
               !invocationBytes.empty() && !transactionId.empty();
    }
};

enum class BootstrapTerminalCleanupHandoffDecodeStatus {
    Accepted,
    TooLarge,
    InvalidJson,
    InvalidSchema,
    InvalidField,
};

struct BootstrapTerminalCleanupHandoffDecodeResult {
    BootstrapTerminalCleanupHandoffDecodeStatus status =
        BootstrapTerminalCleanupHandoffDecodeStatus::InvalidField;
    std::optional<BootstrapTerminalCleanupHandoff> handoff;
    std::string diagnostic;

    bool ok() const noexcept {
        return status ==
                   BootstrapTerminalCleanupHandoffDecodeStatus::Accepted &&
               handoff.has_value() && handoff->valid();
    }
};

BootstrapTerminalCleanupHandoffDecodeResult
DecodeBootstrapTerminalCleanupHandoff(std::string_view bytes);

struct BootstrapRecoveryMarkerDecodeResult {
    bool accepted = false;
    std::string diagnostic;

    bool ok() const noexcept { return accepted; }
};

// Recovery mode carries no plan, path, PID, or nonce. The authenticated
// parent pipe itself is the one-shot capability; this marker only selects the
// durable-recovery path.
BootstrapRecoveryMarkerDecodeResult DecodeBootstrapRecoveryMarker(
    std::string_view bytes);

enum class BootstrapAdmissionStatus {
    Accepted,
    PackageRejected,
    PackageDigestMismatch,
    InvalidInvocation,
    InvalidPlan,
    InvalidJournalInputs,
};

struct BootstrapAdmission {
    BootstrapAdmissionStatus status = BootstrapAdmissionStatus::InvalidPlan;
    std::optional<ValidatedInstallPlan> plan;
    std::optional<ReplacementJournal> journal;
    std::string diagnostic;

    bool ok() const noexcept {
        return status == BootstrapAdmissionStatus::Accepted &&
               plan.has_value() && journal.has_value();
    }
};

// Builds the same plan/journal value boundary as the normal authenticated
// handoff, but deliberately does not require the channel parent to equal the
// original updater parent. The filesystem terminal-admission path binds the
// current Bootstrap parent to the durable runtime-pair identity separately.
BootstrapAdmission AdmitBootstrapTerminalCleanup(
    const PackagePlanAdmission& package,
    const InvocationEnvelope& invocation);

class BootstrapEntrypointComposition;
class InvocationCapability;
class BootstrapHandoffSubmission;
struct VerifiedPackagePlanBindingResult;

// Immutable authority created only after detached-signature verification.
// Updater code may inspect and pass this value, but cannot construct or
// mutate the signed digest/key/version/channel/asset/archive-entry binding.
class VerifiedPackagePlanBinding final {
public:
    VerifiedPackagePlanBinding(const VerifiedPackagePlanBinding&) noexcept = default;
    VerifiedPackagePlanBinding& operator=(
        const VerifiedPackagePlanBinding&) noexcept = default;
    VerifiedPackagePlanBinding(VerifiedPackagePlanBinding&&) noexcept = default;
    VerifiedPackagePlanBinding& operator=(
        VerifiedPackagePlanBinding&&) noexcept = default;
    ~VerifiedPackagePlanBinding() = default;

    const std::string& packagePlanDigest() const noexcept;
    const std::string& packageSignatureDigest() const noexcept;
    const std::string& keyId() const noexcept;
    const std::string& version() const noexcept;
    const std::string& buildId() const noexcept;
    const std::string& channel() const noexcept;
    const std::string& assetSha256() const noexcept;
    std::uint64_t assetSize() const noexcept;
    const std::vector<std::string>& approvedArchiveEntries() const noexcept;
    const std::vector<InstallInventoryItem>& approvedArchiveItems() const noexcept;

private:
    struct State;
    explicit VerifiedPackagePlanBinding(std::shared_ptr<const State> state) noexcept;
    std::shared_ptr<const State> state_;

    friend struct VerifiedPackagePlanBindingResult;
    friend VerifiedPackagePlanBindingResult VerifyPackagePlanBinding(
        std::string_view, std::string_view, const PackagePlanTrustConfig&,
        PackageSignatureVerifier);
    friend struct HarkenSearchPackagePolicyResult;
    friend HarkenSearchPackagePolicyResult EvaluateHarkenSearchPackagePolicy(
        const VerifiedPackagePlanBinding&);
    friend class BootstrapEntrypointComposition;
    friend class InvocationCapability;
    friend class BootstrapHandoffSubmission;
    friend BootstrapHandoffSubmission SubmitVerifiedPackagePlan(
        const VerifiedPackagePlanBinding&, std::string_view,
        InvocationCapability&&);
#if defined(ARIS_BOOTSTRAP_TEST_CAPABILITY_ISSUER)
    friend InvocationCapability IssueInvocationCapabilityForTesting(
        const VerifiedPackagePlanBinding&, const InvocationEnvelope&,
        const BootstrapProcessIdentity&);
#endif
};

struct VerifiedPackagePlanBindingResult {
    PackagePlanAdmissionStatus status =
        PackagePlanAdmissionStatus::InvalidField;
    std::optional<VerifiedPackagePlanBinding> binding;
    std::string diagnostic;

    bool ok() const noexcept {
        return status == PackagePlanAdmissionStatus::Accepted &&
               binding.has_value();
    }
};

VerifiedPackagePlanBindingResult VerifyPackagePlanBinding(
    std::string_view packageBytes,
    std::string_view signatureHex,
    const PackagePlanTrustConfig& trust,
    PackageSignatureVerifier testVerifier = nullptr);

enum class HarkenSearchPackagePolicyStatus {
    Accepted,
    InvalidBinding,
    RemovalsForbidden,
    ProtectedInventory,
};

struct HarkenSearchPackagePolicyResult {
    HarkenSearchPackagePolicyStatus status = HarkenSearchPackagePolicyStatus::InvalidBinding;
    std::string diagnostic;

    bool ok() const noexcept {
        return status == HarkenSearchPackagePolicyStatus::Accepted;
    }
};

// A release signature authenticates package bytes; it never grants permission
// to delete or replace Aris user data. This pure-value product gate is consumed
// before updater extraction and again by Bootstrap admission.
HarkenSearchPackagePolicyResult EvaluateHarkenSearchPackagePolicy(
    const VerifiedPackagePlanBinding& binding);

// Move-only dynamic authority issued by Bootstrap-side authenticated
// composition. Its private state owns roots, exact process lifetimes, health
// nonce and the signed-package snapshot expected by SubmitVerifiedPackagePlan.
// There is no public constructor from InvocationEnvelope.
class InvocationCapability final {
public:
    InvocationCapability(InvocationCapability&&) noexcept;
    InvocationCapability& operator=(InvocationCapability&&) noexcept;
    ~InvocationCapability();

    InvocationCapability(const InvocationCapability&) = delete;
    InvocationCapability& operator=(const InvocationCapability&) = delete;

private:
    struct State;
    InvocationCapability(const VerifiedPackagePlanBinding&,
                         const InvocationEnvelope&,
                         const BootstrapProcessIdentity&);
    std::unique_ptr<State> state_;

    friend class BootstrapEntrypointComposition;
    friend std::string AuthorizedBootstrapArchivePath(
        const InvocationCapability&);
    friend BootstrapHandoffSubmission SubmitVerifiedPackagePlan(
        const VerifiedPackagePlanBinding&, std::string_view,
        InvocationCapability&&);
#if defined(ARIS_BOOTSTRAP_TEST_CAPABILITY_ISSUER)
    friend InvocationCapability IssueInvocationCapabilityForTesting(
        const VerifiedPackagePlanBinding&, const InvocationEnvelope&,
        const BootstrapProcessIdentity&);
#endif
};

std::string AuthorizedBootstrapArchivePath(
    const InvocationCapability& capability);

enum class BootstrapHandoffSubmissionStatus {
    Accepted,
    InvalidBinding,
    InvalidCapability,
    ArchivePathRejected,
    PackageKeyMismatch,
    ArchiveEntriesMismatch,
    AssetBindingMismatch,
    PackageDigestMismatch,
    PackageSignatureMismatch,
    InvocationRejected,
};

class BootstrapHandoffSubmission final {
public:
    BootstrapHandoffSubmissionStatus status() const noexcept { return status_; }
    const std::string& diagnostic() const noexcept { return diagnostic_; }
    bool ok() const noexcept {
        return status_ == BootstrapHandoffSubmissionStatus::Accepted &&
               admission_.has_value() && admission_->ok();
    }

private:
    BootstrapHandoffSubmissionStatus status_ =
        BootstrapHandoffSubmissionStatus::InvalidCapability;
    std::optional<BootstrapAdmission> admission_;
    BootstrapProcessIdentity authenticatedParent_;
    std::string diagnostic_;

    friend class BootstrapEntrypointComposition;
    friend BootstrapHandoffSubmission SubmitVerifiedPackagePlan(
        const VerifiedPackagePlanBinding&, std::string_view,
        InvocationCapability&&);
    friend DurableRecoveryAdmissionResult AdmitAuthenticatedBootstrapRecovery(
        const WindowsBootstrapJournalRecord&, const PackagePlanTrustConfig&,
        const BootstrapHandoffSubmission&, const std::string&,
        const std::string&, const std::string&, PackageSignatureVerifier);
};

// Stable updater-facing admission surface. The caller supplies only an
// immutable signed binding, the expected archive path, and an opaque
// Bootstrap-issued one-shot capability. No bootstrap executable path or ZIP
// entry path is accepted here.
BootstrapHandoffSubmission SubmitVerifiedPackagePlan(
    const VerifiedPackagePlanBinding& binding,
    std::string_view archivePath,
    InvocationCapability&& capability);

// Restart admission consumes an already accepted opaque submission. Durable
// bytes remain untrusted and are rebound to the verified signed plan, exact
// authenticated parent image, roots, process lifetimes and health nonce
// digest before a rehydrated journal is returned.
DurableRecoveryAdmissionResult AdmitAuthenticatedBootstrapRecovery(
    const WindowsBootstrapJournalRecord& record,
    const PackagePlanTrustConfig& trust,
    const BootstrapHandoffSubmission& submittedAuthority,
    const std::string& expectedUpdaterOwnedBase,
    const std::string& expectedStagingRoot,
    const std::string& expectedTransactionRoot,
    PackageSignatureVerifier testVerifier = nullptr);

#if defined(ARIS_BOOTSTRAP_TEST_CAPABILITY_ISSUER)
// Test-only construction seam; never compiled into HarkenSearchBootstrap.exe.
InvocationCapability IssueInvocationCapabilityForTesting(
    const VerifiedPackagePlanBinding& binding,
    const InvocationEnvelope& invocation,
    const BootstrapProcessIdentity& authenticatedParent);
#endif

#if defined(ARIS_BOOTSTRAP_TEST_RAW_INVOCATION_ADMISSION)
// Test-only seam for focused value validation. Production callers must use
// SubmitVerifiedPackagePlan with a Bootstrap-issued InvocationCapability.
BootstrapAdmission AdmitBootstrapInvocationForTesting(
    const PackagePlanAdmission& package,
    const InvocationEnvelope& invocation);
#endif

// Production trust configuration is compile-time provisioned.  An ordinary
// developer build returns {provisioned:false}, so every production admission
// deterministically fails closed.
PackagePlanTrustConfig ProductionPackagePlanTrust() noexcept;

}  // namespace aris::bootstrap
