#pragma once

#include "bootstrap/HarkenSearchBootstrapAdmission.h"
#include "bootstrap/HarkenSearchBootstrapPlan.h"

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace aris::bootstrap {

// Opaque to the portable transaction policy. The Win32 implementation stores
// native HANDLE values here; tests use stable integer tokens.
using WindowsFilesystemHandle = std::uintptr_t;

struct WindowsFileIdentity {
    bool valid = false;
    std::uint64_t volumeSerial = 0;
    std::array<std::uint8_t, 16> fileId{};

    friend bool operator==(const WindowsFileIdentity& left,
                           const WindowsFileIdentity& right) noexcept {
        return left.valid == right.valid &&
               left.volumeSerial == right.volumeSerial &&
               left.fileId == right.fileId;
    }

    friend bool operator!=(const WindowsFileIdentity& left,
                           const WindowsFileIdentity& right) noexcept {
        return !(left == right);
    }
};

struct WindowsFileContentFingerprint {
    bool valid = false;
    std::uint64_t byteSize = 0;
    std::array<std::uint8_t, 32> sha256{};

    friend bool operator==(const WindowsFileContentFingerprint& left,
                           const WindowsFileContentFingerprint& right) noexcept {
        return left.valid == right.valid && left.byteSize == right.byteSize &&
               left.sha256 == right.sha256;
    }

    friend bool operator!=(const WindowsFileContentFingerprint& left,
                           const WindowsFileContentFingerprint& right) noexcept {
        return !(left == right);
    }
};

// Opaque, canonical rollback metadata owned as bytes by the portable port and
// interpreted only by the native backend. It is serialized into the durable
// adapter journal so metadata cannot silently drift outside the transaction's
// identity/content envelope. The current Win32 envelope covers basic file
// fields, owner/group/DACL, and the mandatory integrity label. Ordinary audit
// SACLs require ACCESS_SYSTEM_SECURITY/SeSecurityPrivilege and are deliberately
// not claimed as preserved by this slice; audited-target support needs a
// privilege-aware metadata contract before it is enabled.
struct WindowsFileMetadataSnapshot {
    bool valid = false;
    std::vector<std::uint8_t> bytes;

    friend bool operator==(const WindowsFileMetadataSnapshot& left,
                           const WindowsFileMetadataSnapshot& right) noexcept {
        return left.valid == right.valid && left.bytes == right.bytes;
    }

    friend bool operator!=(const WindowsFileMetadataSnapshot& left,
                           const WindowsFileMetadataSnapshot& right) noexcept {
        return !(left == right);
    }
};

enum class WindowsFilesystemRootRole {
    Install,
    Staging,
    Transaction,
    Backup,
};

enum class WindowsFilesystemEntryKind {
    Missing,
    RegularFile,
    Directory,
    ReparsePoint,
    Other,
};

struct WindowsFilesystemObservation {
    WindowsFilesystemEntryKind kind = WindowsFilesystemEntryKind::Missing;
    WindowsFileIdentity identity;
    // Install/backup observations may carry rollback metadata. Staging
    // observations intentionally leave this invalid: package security
    // descriptors are never an authority for the installed tree.
    WindowsFileMetadataSnapshot metadata;
    // Optional content fingerprint used by restart recovery.  It is captured
    // from the same no-follow handle as identity/metadata.
    WindowsFileContentFingerprint content;
};

enum class WindowsFilesystemReplaceMode {
    CreateNew,
    ReplaceExisting,
};

enum class WindowsFilesystemBackendStatus {
    Ok,
    NotFound,
    AlreadyExists,
    AccessDenied,
    InvalidPath,
    ExactPathMismatch,
    ReparsePoint,
    NotDirectory,
    NotRegularFile,
    IdentityMismatch,
    ContentMismatch,
    DirectoryNotEmpty,
    CrossVolume,
    UnsupportedMetadata,
    DurabilityFailed,
    IoFailure,
};

// CopyFileDurableNoFollow, ReplaceFileDurableNoFollow, and
// DeleteFileDurableNoFollow may report AccessDenied only while their published
// target namespace is unchanged and any adapter-owned temporary entry has
// been removed durably. Once a mutation or its cleanup is ambiguous they must
// report DurabilityFailed, which permanently blocks the portable port.

struct WindowsFilesystemBackendResult {
    WindowsFilesystemBackendStatus status = WindowsFilesystemBackendStatus::Ok;
    std::uint32_t nativeError = 0;

    bool ok() const noexcept {
        return status == WindowsFilesystemBackendStatus::Ok;
    }
};

struct WindowsBootstrapJournalCandidate;
struct WindowsBootstrapFilesystemRoots;
struct WindowsBootstrapInvocationCleanup;

// A replacement backend may publish a durable, identity-bound temporary file
// only after the port has durably recorded its identity in the adapter journal.
// The callback is invoked at the last pre-publication cut point.  It is a
// narrow capability: the callback receives owned-value observations and emits
// only the journal checkpoint result; it does not receive native handles.
struct WindowsFilesystemPublicationCheckpoint {
    using OnPrepared = WindowsFilesystemBackendResult (*)(
        void* context,
        const WindowsFileIdentity& preparedIdentity,
        const WindowsFileContentFingerprint& preparedContent,
        const WindowsFileMetadataSnapshot& preparedMetadata);

    void* context = nullptr;
    OnPrepared onPrepared = nullptr;
};

// Every relative-path primitive must resolve from the supplied trusted root
// handle without following reparse points. Mutation primitives must flush the
// file and its containing directory before returning success.
class WindowsBootstrapFilesystemBackend {
public:
    virtual ~WindowsBootstrapFilesystemBackend() = default;

    // Metadata remains backend-owned opaque data.  Recovery comparisons use
    // the backend's structural equivalence rule so native volatile fields
    // (for example last-access time) do not turn an otherwise exact publish
    // into a false quarantine, while test backends may retain byte equality.
    virtual bool MetadataMatchesForRecovery(
        const WindowsFileMetadataSnapshot& observed,
        const WindowsFileMetadataSnapshot& expected) const {
        return observed == expected;
    }

    virtual WindowsFilesystemBackendResult OpenRootExact(
        const std::string& path,
        WindowsFilesystemRootRole role,
        WindowsFilesystemHandle& outputHandle,
        WindowsFileIdentity& outputIdentity) = 0;

    virtual WindowsFilesystemBackendResult OpenOrCreateDirectoryNoFollow(
        WindowsFilesystemHandle parent,
        const std::string& relativePath,
        // true creates exclusively; false opens an existing directory only.
        // Recovery must never materialize a missing namespace while rebinding.
        // Install-root creation is allowed only under a durable journal lease,
        // during preparation of parents for verified CreateNew inventory.
        bool requireNew,
        WindowsFilesystemHandle& outputHandle,
        WindowsFileIdentity& outputIdentity) = 0;

    virtual WindowsFilesystemBackendResult ObserveNoFollow(
        WindowsFilesystemHandle root,
        const std::string& relativePath,
        WindowsFilesystemObservation& output) = 0;

    // The returned metadata describes the original source for later rollback.
    // The published backup itself keeps transaction-root-owned metadata.
    virtual WindowsFilesystemBackendResult CopyFileDurableNoFollow(
        WindowsFilesystemHandle sourceRoot,
        const std::string& sourcePath,
        const WindowsFileIdentity& expectedSource,
        WindowsFilesystemHandle targetRoot,
        const std::string& targetPath,
        WindowsFileIdentity& outputIdentity,
        WindowsFileContentFingerprint& outputFingerprint,
        WindowsFileMetadataSnapshot& outputMetadata) = 0;

    // temporaryLeaf is an adapter-owned sibling leaf. Implementations must
    // create it exclusively and must never overwrite an existing entry.
    // A staging source carries no expectedSourceMetadata: forward replacement
    // preserves a verified existing target's metadata or inherits the install
    // parent for a new item. Source/staging owner, DACL, SACL, label, and
    // other security descriptors are never copied into the install tree. A
    // backup source carries the journaled rollback metadata captured from the
    // original install object; the physical backup file remains adapter-owned
    // so an old DACL cannot block recovery.
    virtual WindowsFilesystemBackendResult ReplaceFileDurableNoFollow(
        WindowsFilesystemHandle sourceRoot,
        const std::string& sourcePath,
        const WindowsFileIdentity& expectedSource,
        const WindowsFileContentFingerprint& expectedSourceContent,
        const WindowsFileMetadataSnapshot& expectedSourceMetadata,
        WindowsFilesystemHandle targetRoot,
        const std::string& targetPath,
        const WindowsFileIdentity& expectedTargetParent,
        const std::string& temporaryLeaf,
        WindowsFilesystemReplaceMode mode,
        const WindowsFileIdentity& expectedTarget,
        const WindowsFileContentFingerprint& expectedTargetContent,
        const WindowsFileMetadataSnapshot& expectedTargetMetadata,
        WindowsFileIdentity& outputIdentity,
        WindowsFileMetadataSnapshot& outputMetadata) = 0;

    // Extended port-only seam.  When expectedPrepared* is valid, the backend
    // must reopen the adapter-owned temporary leaf by exact identity/content/
    // metadata and continue the same atomic publication; it must never copy a
    // newly discovered same-content object.  When the expected values are
    // invalid, the backend prepares the temporary and invokes checkpoint
    // before the atomic rename.  The legacy method above remains the direct
    // one-shot API used by focused adapter tests.
    virtual WindowsFilesystemBackendResult
    ReplaceFileDurableNoFollowWithCheckpoint(
        WindowsFilesystemHandle sourceRoot,
        const std::string& sourcePath,
        const WindowsFileIdentity& expectedSource,
        const WindowsFileContentFingerprint& expectedSourceContent,
        const WindowsFileMetadataSnapshot& expectedSourceMetadata,
        WindowsFilesystemHandle targetRoot,
        const std::string& targetPath,
        const WindowsFileIdentity& expectedTargetParent,
        const std::string& temporaryLeaf,
        WindowsFilesystemReplaceMode mode,
        const WindowsFileIdentity& expectedTarget,
        const WindowsFileContentFingerprint& expectedTargetContent,
        const WindowsFileMetadataSnapshot& expectedTargetMetadata,
        const WindowsFileIdentity& expectedPreparedIdentity,
        const WindowsFileContentFingerprint& expectedPreparedContent,
        const WindowsFileMetadataSnapshot& expectedPreparedMetadata,
        const WindowsFilesystemPublicationCheckpoint* checkpoint,
        WindowsFileIdentity& outputIdentity,
        WindowsFileMetadataSnapshot& outputMetadata) = 0;

    virtual WindowsFilesystemBackendResult DeleteFileDurableNoFollow(
        WindowsFilesystemHandle root,
        const std::string& relativePath,
        const WindowsFileIdentity& expectedParent,
        const WindowsFileIdentity& expectedIdentity,
        const WindowsFileContentFingerprint& expectedContent,
        const WindowsFileMetadataSnapshot& expectedMetadata) = 0;

    // Remove only the exact, invocation-owned payload/runtime files supplied
    // by the authenticated port.  This seam is intentionally separate from
    // the ordinary staging root capability: it must not widen OpenRootExact
    // or enumerate/delete unknown children.  Implementations must revalidate
    // both roots, reopen each file no-follow, and bind identity/content before
    // every durable delete.  A failure after a delete mark is durable and
    // must be reported as such so the journal remains the recovery authority.
    virtual WindowsFilesystemBackendResult
    CleanupInvocationResidueNoFollow(
        WindowsFilesystemHandle stagingRoot,
        WindowsFilesystemHandle transactionRoot,
        const WindowsBootstrapInvocationCleanup& cleanup) = 0;

    // The native backend owns the journal file name and implements
    // temp-write, file flush, handle-relative rename, and directory flush.
    virtual WindowsFilesystemBackendResult PersistJournalDurable(
        WindowsFilesystemHandle root,
        const std::vector<std::uint8_t>& bytes,
        WindowsFilesystemReplaceMode mode,
        const WindowsFileIdentity& expectedIdentity,
        const WindowsFileContentFingerprint& expectedContent,
        WindowsFileIdentity& outputIdentity,
        WindowsFileContentFingerprint& outputContent) = 0;

    // The caller supplies only an authenticated invocation binding and the
    // updater-owned base selected by this backend instance.  Implementations
    // must reject caller-forged absolute staging/transaction write roots
    // before opening or mutating either root.
    virtual WindowsFilesystemBackendResult ValidateTrustedWriteRoots(
        const ValidatedInstallPlan& plan,
        const WindowsBootstrapFilesystemRoots& roots) = 0;

    // Recovery enumerates only the adapter-owned two-slot names and their
    // corresponding temporary publication candidates.  Reads are bounded,
    // no-follow, and identity/content bound before bytes leave the backend.
    virtual WindowsFilesystemBackendResult ReadJournalCandidatesNoFollow(
        WindowsFilesystemHandle root,
        std::vector<WindowsBootstrapJournalCandidate>& output) = 0;

    // Terminal retirement is restricted to one exact adapter-owned journal
    // candidate observed by identity and content under the transaction root.
    virtual WindowsFilesystemBackendResult RetireJournalCandidateDurable(
        WindowsFilesystemHandle,
        const WindowsBootstrapJournalCandidate&) {
        return {WindowsFilesystemBackendStatus::IoFailure, 0U};
    }

    // Validate the adapter-owned backup namespace before recovery promotes a
    // journal candidate or rebinds any rollback object.  `expectedEntries`
    // contains every journal-bound backup leaf allowed in the directory;
    // `allowedMissingEntries` is the subset whose durable delete-started cut
    // permits either the exact file or absence. Every other child (including
    // temporary names, reparse points, and directories) is rejected.
    // `expectDirectory` distinguishes a durable backup workspace from the
    // post-cleanup missing state.
    virtual WindowsFilesystemBackendResult ValidateBackupNamespaceNoFollow(
        WindowsFilesystemHandle transactionRoot,
        const std::vector<std::string>& expectedEntries,
        const std::vector<std::string>& allowedMissingEntries,
        bool expectDirectory,
        const WindowsFileIdentity& expectedDirectoryIdentity) = 0;

    // Once recovery has opened and identity-bound the adapter-owned backup
    // directory, enumerate that exact handle rather than reopening the
    // transaction path.  This closes the same-name replacement window between
    // path admission and rollback-object rebinding.
    virtual WindowsFilesystemBackendResult
    ValidateBackupNamespaceBoundNoFollow(
        WindowsFilesystemHandle backupRoot,
        const std::vector<std::string>& expectedEntries,
        const std::vector<std::string>& allowedMissingEntries) = 0;

    // A fully decoded, plan/root-authorized temp candidate may be promoted to
    // its typed slot.  Arbitrary names are rejected and publication is atomic.
    virtual WindowsFilesystemBackendResult PromoteJournalCandidateDurable(
        WindowsFilesystemHandle root,
        const WindowsBootstrapJournalCandidate& candidate,
        WindowsFileIdentity& outputIdentity,
        WindowsFileContentFingerprint& outputContent) = 0;

    // Reopens the published journal by trusted root handle, identity and
    // content, then retains a native handle that denies write/delete sharing.
    // No install/backup mutation may run unless this succeeds, and the lease
    // must remain live through that mutation.
    virtual WindowsFilesystemBackendResult AcquireJournalMutationLease(
        WindowsFilesystemHandle root,
        const WindowsFileIdentity& expectedIdentity,
        const WindowsFileContentFingerprint& expectedContent,
        WindowsFilesystemHandle& outputLease) = 0;

    virtual void ReleaseJournalMutationLease(
        WindowsFilesystemHandle lease) = 0;

    virtual WindowsFilesystemBackendResult RemoveEmptyDirectoryDurableNoFollow(
        WindowsFilesystemHandle parent,
        const std::string& relativePath,
        const WindowsFileIdentity& expectedIdentity) = 0;

    virtual void CloseFilesystemHandle(WindowsFilesystemHandle handle) = 0;
};

struct WindowsBootstrapFilesystemRoots {
    std::string stagingRoot;
    std::string transactionRoot;
    // These values are copied from the authenticated updater invocation; they
    // are compared to backend-owned policy and are not accepted as authority
    // merely because a caller can spell an absolute path.
    std::string updaterOwnedBase;
    std::string invocationIdentity;
};

struct WindowsBootstrapInvocationCleanupFile {
    std::string relativePath;
    WindowsFileIdentity identity;
    WindowsFileContentFingerprint content;
};

struct WindowsBootstrapInvocationCleanup {
    // Durable authority for the optional transaction/runtime payload.  The
    // boolean is intentional: an absent runtime-pair at Begin() is an
    // authenticated observation and must not become an invitation to adopt a
    // same-name directory during fresh recovery.
    bool runtimePairExpected = false;
    WindowsFileIdentity runtimePairDirectoryIdentity;
    WindowsFileIdentity runtimePairBootstrapIdentity;
    WindowsFileContentFingerprint runtimePairBootstrapContent;
    // Once the start checkpoint is durable, a missing exact residue is an
    // idempotent result of a prior cleanup cut.  A different identity/content
    // is still rejected.
    bool allowAlreadyRemoved = false;
    std::vector<WindowsBootstrapInvocationCleanupFile> stagingFiles;
    std::vector<WindowsBootstrapInvocationCleanupFile> transactionFiles;
};

// Durable journal values are deliberately independent from the session state
// held by WindowsBootstrapFilesystemPort.  A recovery process decodes these
// values from bytes it did not produce, so the codec is bounded and validates
// every field before a platform adapter is allowed to reopen an object.
struct WindowsBootstrapJournalItemRecord {
    std::string relativePath;
    std::string backupRelativePath;
    std::string forwardTemporaryLeaf;
    std::string restoreTemporaryLeaf;
    JournalItemClass classification = JournalItemClass::PreserveExisting;
    WindowsFileIdentity stagingIdentity;
    WindowsFileIdentity installParentIdentity;
    WindowsFileIdentity originalIdentity;
    WindowsFileIdentity backupIdentity;
    WindowsFileIdentity installedIdentity;
    WindowsFileIdentity restoredIdentity;
    WindowsFileIdentity preparedIdentity;
    WindowsFileIdentity restorePreparedIdentity;
    WindowsFileContentFingerprint stagingContent;
    WindowsFileContentFingerprint backupContent;
    WindowsFileContentFingerprint preparedContent;
    WindowsFileContentFingerprint restorePreparedContent;
    WindowsFileMetadataSnapshot rollbackMetadata;
    WindowsFileMetadataSnapshot installedMetadata;
    WindowsFileMetadataSnapshot restoredMetadata;
    WindowsFileMetadataSnapshot preparedMetadata;
    WindowsFileMetadataSnapshot restorePreparedMetadata;
    // True only when a no-follow observation under the pinned install root
    // proved that this exact signed removal path was absent before mutation.
    bool removalAbsentVerified = false;
    bool backupStarted = false;
    bool backupComplete = false;
    bool replacementStarted = false;
    bool replacementPrepared = false;
    bool replacementComplete = false;
    bool restoreStarted = false;
    bool restorePrepared = false;
    bool restoreComplete = false;
    bool removeNewStarted = false;
    bool removeNewComplete = false;
    bool removeBackupStarted = false;
    bool removeBackupComplete = false;
};

struct WindowsBootstrapJournalRecord {
    // 9 remains readable/writable for historical transactions. 10 has the
    // same wire layout and additionally permits unbound CreateNew parents
    // before file mutation; they must be bound durably before publication.
    std::uint32_t schemaVersion = 9;
    std::uint64_t generation = 0;
    std::string transactionId;
    aris::platform::DeploymentMode deploymentMode =
        aris::platform::DeploymentMode::Portable;
    std::string installationId;
    std::string registeredAppId;
    std::string targetRoot;
    aris::platform::DeploymentFileIdentity targetRootIdentity;
    aris::platform::DeploymentFileIdentity oldExecutableIdentity;
    std::string updaterOwnedBase;
    // Dynamic invocation locators are persisted so a fresh Bootstrap can
    // derive and re-open the exact roots without accepting writable paths
    // from a recovery command line.  They remain unsigned policy inputs and
    // are accepted only after exact root identity/non-overlap checks.
    std::string stagingRoot;
    std::string transactionRoot;
    std::vector<std::string> protectedDataRoots;
    std::uint32_t previousFailureCount = 0;
    // A failure is counted once for this exact transaction even if recovery
    // crashes again while restoring or cleaning backup state.
    bool failureRecordedForTransaction = false;
    std::string invocationIdentity;
    std::string version;
    std::string buildId;
    std::string packagePlanDigest;
    std::string packageKeyId;
    std::string packageChannel;
    // Exact immutable release-signed authority used to re-verify a recovery
    // attempt.  The dynamic invocation capability and raw health nonce are
    // intentionally not stored here.
    std::string packagePlanBytes;
    std::string packageSignatureHex;
    // The raw health nonce is never serialized in schema 3.  Producers may
    // retain it transiently for compatibility with old in-memory tests; the
    // encoder derives/stores only this digest.
    std::string healthNonceDigest;
    std::string healthToken;
    std::uint64_t oldProcessId = 0;
    BootstrapProcessIdentity oldProcessIdentity;
    BootstrapProcessIdentity parentProcessIdentity;
    BootstrapProcessIdentity updatedChildIdentity;
    BootstrapProcessIdentity previousApplicationIdentity;
    std::string healthPhase;
    std::string entrypoint;
    std::string phase;
    WindowsFileIdentity installRootIdentity;
    WindowsFileIdentity stagingRootIdentity;
    WindowsFileIdentity transactionRootIdentity;
    WindowsFileIdentity backupRootIdentity;
    WindowsFileIdentity journalIdentity;
    WindowsFileContentFingerprint journalContent;
    bool initialJournalDurable = false;
    bool backupDirectoryStarted = false;
    bool backupDirectoryComplete = false;
    bool commitComplete = false;
    bool rollbackDurable = false;
    bool rollbackComplete = false;
    bool backupDirectoryRemoveStarted = false;
    bool backupDirectoryRemoved = false;
    bool healthTokenConsumed = false;
    // File commit is irreversible, but application availability is not
    // terminal until a fresh exact child has survived an authenticated health
    // attempt. This flag makes the reset/launch/health cuts replayable without
    // inferring lifecycle authority from a phase string.
    bool committedApplicationRelaunchPending = false;
    bool previousApplicationRelaunchStarted = false;
    bool previousApplicationRelaunchComplete = false;
    bool runtimePairExpected = false;
    WindowsFileIdentity runtimePairDirectoryIdentity;
    WindowsFileIdentity runtimePairBootstrapIdentity;
    WindowsFileContentFingerprint runtimePairBootstrapContent;
    bool invocationCleanupStarted = false;
    bool invocationCleanupComplete = false;
    std::vector<WindowsBootstrapJournalItemRecord> items;
};

enum class WindowsBootstrapJournalDecodeStatus {
    Ok,
    Empty,
    TooLarge,
    Truncated,
    InvalidMagic,
    UnsupportedVersion,
    InvalidChecksum,
    InvalidAuthentication,
    InvalidField,
    InvalidIdentity,
    InvalidFingerprint,
    InvalidMetadata,
    InvalidPackageAuthority,
    InvalidItem,
    DuplicateItem,
    PlanMismatch,
    JournalMismatch,
    RootMismatch,
};

// Optional strict restart boundary.  The journal codec itself remains a
// bounded corruption parser; callers reopening a transaction provide the
// provisioned release trust and require exact package-byte/signature
// revalidation before a candidate can become a durable winner.
struct WindowsBootstrapJournalPackageAuthority {
    const PackagePlanTrustConfig* trust = nullptr;
    PackageSignatureVerifier verifier = nullptr;
};

struct WindowsBootstrapJournalDecodeResult {
    WindowsBootstrapJournalDecodeStatus status =
        WindowsBootstrapJournalDecodeStatus::InvalidField;
    std::optional<WindowsBootstrapJournalRecord> record;
    std::size_t fieldIndex = 0;

    bool ok() const noexcept {
        return status == WindowsBootstrapJournalDecodeStatus::Ok &&
               record.has_value();
    }
};

struct WindowsBootstrapJournalRootIdentities {
    WindowsFileIdentity install;
    WindowsFileIdentity staging;
    WindowsFileIdentity transaction;
    WindowsFileIdentity backup;
};

// Pure authority seam used by recovery admission.  It compares durable
// dynamic locators with roots derived by the trusted updater boundary; it
// performs no filesystem resolution and therefore cannot mistake a lexical
// alias for a native identity.
bool ValidateAnchoredWindowsBootstrapJournalRoots(
    const WindowsBootstrapJournalRecord& record,
    const std::string& expectedUpdaterOwnedBase,
    const std::string& expectedStagingRoot,
    const std::string& expectedTransactionRoot);

// The checksum is only corruption detection.  Recovery authority comes from
// the expected validated plan/journal/root identities supplied by the caller.
std::vector<std::uint8_t> EncodeWindowsBootstrapJournal(
    const WindowsBootstrapJournalRecord& record);

WindowsBootstrapJournalDecodeResult DecodeWindowsBootstrapJournal(
    const std::vector<std::uint8_t>& bytes,
    const ValidatedInstallPlan* expectedPlan = nullptr,
    const ReplacementJournal* expectedJournal = nullptr,
    const WindowsBootstrapJournalRootIdentities* expectedRoots = nullptr,
    bool allowParentProcessRebind = false,
    const WindowsBootstrapJournalPackageAuthority* packageAuthority = nullptr);

struct WindowsBootstrapJournalCandidate {
    std::string name;
    std::vector<std::uint8_t> bytes;
    bool durable = true;
    bool temporary = false;
    WindowsFileIdentity identity;
    WindowsFileContentFingerprint content;
    WindowsFileIdentity replacedSlotIdentity;
    WindowsFileContentFingerprint replacedSlotContent;
};

enum class WindowsBootstrapJournalSelectionStatus {
    Selected,
    NoCandidate,
    InvalidCandidate,
    Ambiguous,
};

struct WindowsBootstrapJournalSelection {
    WindowsBootstrapJournalSelectionStatus status =
        WindowsBootstrapJournalSelectionStatus::NoCandidate;
    std::optional<WindowsBootstrapJournalRecord> record;
    std::string selectedName;

    bool ok() const noexcept {
        return status == WindowsBootstrapJournalSelectionStatus::Selected &&
               record.has_value();
    }
};

// Select exactly one durable winner.  A malformed candidate, a duplicate
// generation with different bytes, or a durable/temp ambiguity is fail-closed
// rather than guessed away.
WindowsBootstrapJournalSelection SelectDurableWindowsBootstrapJournal(
    const std::vector<WindowsBootstrapJournalCandidate>& candidates,
    const ValidatedInstallPlan* expectedPlan = nullptr,
    const ReplacementJournal* expectedJournal = nullptr,
    const WindowsBootstrapJournalRootIdentities* expectedRoots = nullptr,
    bool allowParentProcessRebind = false,
    const WindowsBootstrapJournalPackageAuthority* packageAuthority = nullptr);

enum class WindowsBootstrapFilesystemStatus {
    Ok,
    InvalidPlan,
    InvalidJournal,
    InvalidRoots,
    UnsupportedIntent,
    InvalidIntentPath,
    InvalidIntentOrder,
    RootIdentityMismatch,
    RootVolumeMismatch,
    RootOverlapRejected,
    ReparsePointRejected,
    MissingPayloadItem,
    ExistingItemMissing,
    UnexpectedEntryType,
    UnknownTargetItem,
    JournalCollision,
    JournalNotDurable,
    IdentityMismatch,
    ContentMismatch,
    PartialDurableState,
    CleanupPending,
    RetryableAccessDenied,
    UnsupportedMetadata,
    TransactionBlocked,
    BackendFailure,
};

struct WindowsBootstrapFilesystemResult {
    WindowsBootstrapFilesystemStatus status = WindowsBootstrapFilesystemStatus::Ok;
    WindowsFilesystemBackendStatus backendStatus = WindowsFilesystemBackendStatus::Ok;
    std::uint32_t nativeError = 0;

    bool ok() const noexcept {
        return status == WindowsBootstrapFilesystemStatus::Ok;
    }
};

// Stable names for failure records and support dialogs.  Every enumerator
// has a name; unknown values fall back to "Unknown".  These exist so a
// filesystem-begin rejection can carry its exact cause instead of only the
// coarse exit code 30.
const char* WindowsBootstrapFilesystemStatusName(
    WindowsBootstrapFilesystemStatus status);
const char* WindowsFilesystemBackendStatusName(
    WindowsFilesystemBackendStatus status);

enum class WindowsBootstrapRecoveryDisposition {
    ContinueForward,
    Rollback,
    CleanupCommitted,
    Complete,
    Quarantine,
};

struct WindowsBootstrapFilesystemRecoveryResult {
    WindowsBootstrapFilesystemResult filesystem;
    WindowsBootstrapRecoveryDisposition disposition =
        WindowsBootstrapRecoveryDisposition::Quarantine;
    std::uint64_t generation = 0;
    std::optional<ReplacementJournal> journal;
    std::vector<BootstrapIntent> intents;

    bool ok() const noexcept {
        return filesystem.ok() &&
               disposition != WindowsBootstrapRecoveryDisposition::Quarantine;
    }
};

// Optional native-only test seam. The callback runs synchronously after the
// target's containing-directory chain has been opened and pinned, but before
// replacement observes or mutates the target leaf. It must not re-enter the
// backend. Production callers leave this value empty.
enum class WindowsBootstrapFilesystemNativeTestFailurePoint {
    None,
    BackupDirectoryBeforeCreate,
    BackupDirectoryAfterCreateBeforeObservation,
    ReplacementBeforeAtomicPublish,
    ReplacementAfterPreparedCheckpointBeforeAtomicPublish,
    ReplacementAfterAtomicPublishBeforeDirectoryFlush,
    BackupAfterPublishBeforeRebind,
    // Durable journal publication has two distinct namespace cuts: the
    // temporary file flush and the containing-directory flush.  Recovery
    // must be exercised at both boundaries.
    JournalAfterTemporaryFileFlushBeforeDirectoryFlush,
    JournalAfterTemporaryFlush,
    JournalAfterSlotPublishBeforeDirectoryFlush,
    // DeleteFileDurableNoFollow and RemoveEmptyDirectoryDurableNoFollow
    // expose the same post-mark cut points.  Once the delete disposition is
    // published, every injected/native failure is a durability failure: the
    // namespace may already have changed and cannot be retried as a clean
    // pre-mutation denial.
    DeleteAfterMarkBeforeRebind,
    DeleteAfterRebindBeforeDirectoryFlush,
    DeleteAfterDirectoryFlushBeforeFinalRebind,
    RemoveDirectoryAfterMarkBeforeRebind,
    RemoveDirectoryAfterRebindBeforeDirectoryFlush,
    RemoveDirectoryAfterDirectoryFlushBeforeFinalRebind,
    InstallDirectoryBeforeCreate,
    InstallDirectoryAfterCreateBeforeObservation,
    // Retained as a source-compatible alias for earlier focused tests.  The
    // adapter no longer exposes a delete/rename gap; this point now fires
    // before the single atomic replace operation.
    ReplacementAfterTargetDeleteFlush = ReplacementBeforeAtomicPublish,
};

struct WindowsBootstrapFilesystemNativeTestHooks {
    using BeforeTargetMutation = void (*)(
        void* context,
        WindowsFilesystemRootRole rootRole,
        const std::string& relativePath);

    // Runs after the adapter has prepared a new temporary file and captured
    // its initial destination-side security metadata, but before publication.
    // The callback may mutate only the supplied temporary handle and must not
    // re-enter the backend. Production callers leave this value empty.
    using BeforeTemporaryPublish = void (*)(
        void* context,
        WindowsFilesystemRootRole rootRole,
        const std::string& relativePath,
        const std::string& temporaryLeaf,
        WindowsFilesystemHandle temporaryHandle);

    // Runs only in native tests immediately after the atomic rename has
    // published the temporary file under its final name. The handle still
    // identifies that exact file; production callers leave this empty.
    using AfterAtomicPublish = void (*)(
        void* context,
        WindowsFilesystemRootRole rootRole,
        const std::string& relativePath,
        WindowsFilesystemHandle publishedHandle);

    // Runs only in native tests after Windows has applied the requested basic
    // and security metadata, immediately before the adapter verifies it.
    using AfterMetadataApply = void (*)(
        void* context,
        WindowsFilesystemRootRole rootRole,
        const std::string& relativePath,
        WindowsFilesystemHandle fileHandle);

    void* context = nullptr;
    BeforeTargetMutation beforeTargetMutation = nullptr;
    BeforeTemporaryPublish beforeTemporaryPublish = nullptr;
    AfterAtomicPublish afterAtomicPublish = nullptr;
    AfterMetadataApply afterMetadataApply = nullptr;
    WindowsBootstrapFilesystemNativeTestFailurePoint accessDeniedFailurePoint =
        WindowsBootstrapFilesystemNativeTestFailurePoint::None;
};

struct WindowsBootstrapFilesystemTrustedRootPolicy {
    // This base and invocation identity are supplied by the updater-owned
    // process that authenticated the signed invocation.  The default policy
    // is intentionally unusable for mutations (fail closed).
    std::string updaterOwnedBase;
    std::string invocationIdentity;
    // The authenticated updater derives these exact child roots before it
    // constructs the filesystem port.  They are capabilities, not hints:
    // callers cannot select a different writable child merely by supplying a
    // path below updaterOwnedBase.  Keeping the values in the backend-owned
    // policy also lets a fresh recovery process re-check the same binding.
    std::string stagingRoot;
    std::string transactionRoot;
    bool authenticated = false;
    // Production composition may pin the updater base to an independently
    // derived OS-owned location (for example %LOCALAPPDATA%\\Aris\\updates).
    // An empty value keeps the low-level fixture/test policy compatible; the
    // executable boundary supplies a non-empty anchor before mutation.
    std::string expectedUpdaterOwnedBase;
    // When present, this exact channel-observed parent identity is bound into
    // the writable-root capability.  It prevents a second same-user process
    // from replaying a path-only invocation.
    BootstrapProcessIdentity authenticatedParentProcess;
    // Terminal cleanup is a deliberately separate capability.  The normal
    // authenticated parent remains bound to the signed invocation, while
    // this second identity is the inherited runtime-pair Bootstrap parent.
    // WindowsBootstrapFilesystemPort binds it to the durable journal's exact
    // runtime-pair image before any journal promotion or cleanup mutation.
    bool terminalCleanup = false;
    BootstrapProcessIdentity terminalCleanupParentProcess;
};

// Session-scoped adapter-private journal state. This deliberately does not
// treat the pure core's post-operation progress booleans as durable truth.
class WindowsBootstrapFilesystemPort {
public:
    explicit WindowsBootstrapFilesystemPort(
        WindowsBootstrapFilesystemBackend& backend,
        std::optional<WindowsBootstrapJournalPackageAuthority>
            packageAuthority = std::nullopt);
    ~WindowsBootstrapFilesystemPort();

    WindowsBootstrapFilesystemPort(const WindowsBootstrapFilesystemPort&) = delete;
    WindowsBootstrapFilesystemPort& operator=(const WindowsBootstrapFilesystemPort&) = delete;

    WindowsBootstrapFilesystemResult Begin(
        const ValidatedInstallPlan& plan,
        const ReplacementJournal& journal,
        const WindowsBootstrapFilesystemRoots& roots);

    WindowsBootstrapFilesystemResult BindPreviousFailureCount(
        std::uint32_t count);

    // Fresh-process recovery: reopen trusted roots, strictly decode/select a
    // durable journal generation, promote a complete temp publication when
    // required, rebind every recorded object, and emit only deterministic
    // forward/rollback/cleanup intents.
    WindowsBootstrapFilesystemRecoveryResult BeginRecovery(
        const ValidatedInstallPlan& plan,
        const ReplacementJournal& journal,
        const WindowsBootstrapFilesystemRoots& roots);

    // Restart-safe overload.  The original in-memory journal is deliberately
    // absent; the strictly decoded durable winner supplies the classifications
    // and progress used to rehydrate a plan-bound journal value.
    WindowsBootstrapFilesystemRecoveryResult BeginRecovery(
        const ValidatedInstallPlan& plan,
        const WindowsBootstrapFilesystemRoots& roots);

    // Dedicated terminal-cleanup admission.  It shares the strict recovery
    // rebind path but requires the inherited parent process image/FileId to
    // match the durable runtime-pair Bootstrap before any journal promotion.
    // Only a committed journal may pass this boundary.
    WindowsBootstrapFilesystemRecoveryResult BeginTerminalCleanup(
        const ValidatedInstallPlan& plan,
        const ReplacementJournal& journal,
        const WindowsBootstrapFilesystemRoots& roots,
        const BootstrapProcessIdentity& inheritedParentProcess);

    WindowsBootstrapFilesystemResult Apply(const BootstrapIntent& intent);

    // Runtime composition checkpoints are serialized into the same A/B
    // journal chain before the child becomes termination authority or health
    // can commit.  Only exact identities returned by the owned launch port are
    // accepted.
    WindowsBootstrapFilesystemResult RecordUpdatedChildIdentity(
        const BootstrapProcessIdentity& identity);
    WindowsBootstrapFilesystemResult RecordHealthState(
        const std::string& healthPhase,
        bool nonceConsumed);
    // After a fresh authenticated recovery process has rebound and terminated
    // the exact journal-owned awaiting-health child, durably clear that stale
    // attempt before launching a replacement child with a new one-shot health
    // channel.  A crash before or after this checkpoint remains replayable.
    WindowsBootstrapFilesystemResult PrepareRecoveredHealthRelaunch(
        const BootstrapProcessIdentity& terminatedChild);
    WindowsBootstrapFilesystemResult
    RecordPreviousApplicationRelaunchStarted();
    WindowsBootstrapFilesystemResult RecordPreviousApplicationIdentity(
        const BootstrapProcessIdentity& identity);
    WindowsBootstrapFilesystemResult ResetPreviousApplicationIdentity(
        const BootstrapProcessIdentity& identity);
    WindowsBootstrapFilesystemResult
    RecordPreviousApplicationRelaunchComplete(
        const BootstrapProcessIdentity& identity);
    // Remove exact invocation residue after either a healthy commit or a
    // durable rollback handoff. The legacy name remains to avoid widening the
    // public adapter surface while both terminal paths share one authority.
    WindowsBootstrapFilesystemResult CleanupCommittedInvocation();
    // Remove the authenticated A/B journal chain only after invocation residue
    // has been durably cleaned. Older generations are deleted before the winner.
    WindowsBootstrapFilesystemResult RetireTerminalJournal();
    std::optional<BootstrapProcessIdentity> recoveredUpdatedChildIdentity() const;
    std::string recoveredHealthPhase() const;
    std::optional<std::uint32_t> recoveredPreviousFailureCount() const;
    bool recoveredFailureRecordedForTransaction() const noexcept;
    bool recoveredRollbackComplete() const noexcept;
    bool recoveredPreviousApplicationRelaunchStarted() const noexcept;
    bool recoveredPreviousApplicationRelaunchComplete() const noexcept;
    std::optional<BootstrapProcessIdentity>
    recoveredPreviousApplicationIdentity() const;
    std::optional<WindowsFileContentFingerprint>
    previousApplicationContentFingerprint() const;

    bool blocked() const noexcept;

private:
    WindowsBootstrapFilesystemRecoveryResult BeginRecoveryInternal(
        const ValidatedInstallPlan& plan,
        const ReplacementJournal* expectedJournal,
        const WindowsBootstrapFilesystemRoots& roots,
        const BootstrapProcessIdentity* terminalParentProcess = nullptr);
    struct State;
    WindowsBootstrapFilesystemBackend& backend_;
    std::unique_ptr<State> state_;
    std::optional<WindowsBootstrapJournalPackageAuthority> packageAuthority_;
};

// Available only on Windows. The implementation keeps all Windows/NT headers
// out of the pure Bootstrap domain and out of callers that inject a test seam.
std::unique_ptr<WindowsBootstrapFilesystemBackend>
CreateNativeWindowsBootstrapFilesystemBackend(
    const WindowsBootstrapFilesystemNativeTestHooks& testHooks = {});

std::unique_ptr<WindowsBootstrapFilesystemBackend>
CreateNativeWindowsBootstrapFilesystemBackend(
    const WindowsBootstrapFilesystemTrustedRootPolicy& trustedRoots,
    const WindowsBootstrapFilesystemNativeTestHooks& testHooks = {});

#if defined(_WIN32)
bool WindowsBootstrapMandatoryLabelDescriptorsEquivalentForTesting(
    const void* left,
    const void* right);
#endif

}  // namespace aris::bootstrap
