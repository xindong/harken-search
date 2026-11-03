#include "bootstrap/WindowsBootstrapFilesystem.h"

#include "bootstrap/HarkenSearchBootstrapAdmission.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <iomanip>
#include <iterator>
#include <set>
#include <sstream>
#include <thread>
#include <utility>

namespace aris::bootstrap {
namespace {

constexpr const char* kBackupDirectory = "backup";
constexpr const char* kJournalFile = "aris-bootstrap.journal";
constexpr const char* kJournalTemporaryFile = "aris-bootstrap.journal.tmp";
constexpr const char* kJournalSlotA = "aris-bootstrap.journal.a";
constexpr const char* kJournalSlotB = "aris-bootstrap.journal.b";
constexpr const char* kJournalSlotATemporary = "aris-bootstrap.journal.a.tmp";
constexpr const char* kJournalSlotBTemporary = "aris-bootstrap.journal.b.tmp";
constexpr std::uint32_t kJournalPersistRetryAttempts = 50U;
constexpr auto kJournalPersistRetryDelay = std::chrono::milliseconds(10);

struct ItemState {
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

class ScopedJournalMutationLease {
public:
    ScopedJournalMutationLease(WindowsBootstrapFilesystemBackend& backend,
                               WindowsFilesystemHandle lease) noexcept
        : backend_(backend), lease_(lease) {}

    ~ScopedJournalMutationLease() {
        if (lease_ != 0) backend_.ReleaseJournalMutationLease(lease_);
    }

    ScopedJournalMutationLease(const ScopedJournalMutationLease&) = delete;
    ScopedJournalMutationLease& operator=(const ScopedJournalMutationLease&) = delete;

private:
    WindowsBootstrapFilesystemBackend& backend_;
    WindowsFilesystemHandle lease_ = 0;
};

WindowsBootstrapFilesystemResult result(
    const WindowsBootstrapFilesystemStatus status,
    const WindowsFilesystemBackendResult backendResult = {}) {
    return {status, backendResult.status, backendResult.nativeError};
}

WindowsBootstrapFilesystemStatus observationFailure(
    const WindowsFilesystemBackendResult& backendResult) {
    if (backendResult.status == WindowsFilesystemBackendStatus::ReparsePoint) {
        return WindowsBootstrapFilesystemStatus::ReparsePointRejected;
    }
    if (backendResult.status == WindowsFilesystemBackendStatus::ExactPathMismatch) {
        return WindowsBootstrapFilesystemStatus::RootIdentityMismatch;
    }
    if (backendResult.status == WindowsFilesystemBackendStatus::CrossVolume) {
        return WindowsBootstrapFilesystemStatus::RootVolumeMismatch;
    }
    if (backendResult.status ==
        WindowsFilesystemBackendStatus::UnsupportedMetadata) {
        return WindowsBootstrapFilesystemStatus::UnsupportedMetadata;
    }
    return WindowsBootstrapFilesystemStatus::BackendFailure;
}

bool sameObject(const WindowsFileIdentity& left,
                const WindowsFileIdentity& right) noexcept {
    return left.valid && right.valid && left == right;
}

bool sameDeploymentObject(
    const aris::platform::DeploymentFileIdentity& expected,
    const WindowsFileIdentity& opened) noexcept {
    return expected.valid && opened.valid &&
           expected.volumeSerial == opened.volumeSerial &&
           expected.fileId == opened.fileId;
}

std::string comparableRoot(std::string path) {
    std::replace(path.begin(), path.end(), '\\', '/');
    while (path.size() > 3 && path.back() == '/') path.pop_back();
    std::transform(path.begin(), path.end(), path.begin(), [](const unsigned char value) {
        return static_cast<char>(std::tolower(value));
    });
    return path;
}

std::string insensitiveRelativePath(std::string path) {
    std::transform(path.begin(), path.end(), path.begin(), [](const unsigned char value) {
        return static_cast<char>(std::tolower(value));
    });
    return path;
}

bool rootsOverlap(const std::string& first, const std::string& second) {
    const std::string left = comparableRoot(first);
    const std::string right = comparableRoot(second);
    const auto contains = [](const std::string& parent, const std::string& child) {
        return child == parent ||
               (child.size() > parent.size() &&
                child.compare(0, parent.size(), parent) == 0 &&
                child[parent.size()] == '/');
    };
    return contains(left, right) || contains(right, left);
}

bool rootsMatchSignedInvocation(
    const ValidatedInstallPlan& plan,
    const WindowsBootstrapFilesystemRoots& roots) {
    return comparableRoot(roots.updaterOwnedBase) ==
               comparableRoot(plan.invocation.updaterOwnedBase) &&
           comparableRoot(roots.stagingRoot) ==
               comparableRoot(plan.invocation.stagingRoot) &&
           comparableRoot(roots.transactionRoot) ==
               comparableRoot(plan.invocation.transactionRoot) &&
           roots.invocationIdentity ==
               plan.invocation.invocationIdentity;
}

bool validOwnedProcessIdentity(const BootstrapProcessIdentity& identity) {
    return identity.valid && identity.processId != 0U &&
           identity.creationTime != 0U && identity.imageVolumeSerial != 0U &&
           !identity.imagePath.empty() &&
           std::any_of(identity.imageFileId.begin(), identity.imageFileId.end(),
                       [](const std::uint8_t byte) { return byte != 0U; });
}

bool isAllowedJournalReason(const std::string& reason) {
    static constexpr const char* kAllowed[] = {
        "prepared",
        "checkpoint",
        "rolling-back",
        "recovery-blocked",
        "updated-child-recorded",
        "backup-recorded",
        "replacement-recorded",
        "parent-rebound",
        "committing",
    };
    return std::any_of(std::begin(kAllowed), std::end(kAllowed),
                       [&reason](const char* allowed) {
                           return reason == allowed;
                       });
}

std::vector<std::string> parentPaths(const std::string& relativePath) {
    std::vector<std::string> result;
    std::size_t separator = relativePath.find('/');
    while (separator != std::string::npos) {
        result.push_back(relativePath.substr(0, separator));
        separator = relativePath.find('/', separator + 1U);
    }
    return result;
}

std::string siblingPath(const std::string& relativePath,
                        const std::string& siblingLeaf) {
    const std::size_t separator = relativePath.find_last_of('/');
    return separator == std::string::npos
        ? siblingLeaf
        : relativePath.substr(0, separator + 1U) + siblingLeaf;
}

std::string identityText(const WindowsFileIdentity& identity) {
    if (!identity.valid) return "-";
    std::ostringstream stream;
    stream << identity.volumeSerial << ':' << std::hex << std::setfill('0');
    for (const std::uint8_t byte : identity.fileId) {
        stream << std::setw(2) << static_cast<unsigned int>(byte);
    }
    return stream.str();
}

std::uint8_t hexNibble(const char value) noexcept {
    if (value >= '0' && value <= '9') {
        return static_cast<std::uint8_t>(value - '0');
    }
    return static_cast<std::uint8_t>(10 + value - 'a');
}

WindowsFileContentFingerprint inventoryFingerprint(
    const InstallInventoryItem& item) {
    WindowsFileContentFingerprint result;
    if (item.sha256.size() != result.sha256.size() * 2U) return result;
    result.valid = true;
    result.byteSize = item.byteSize;
    for (std::size_t index = 0; index < result.sha256.size(); ++index) {
        result.sha256[index] = static_cast<std::uint8_t>(
            (hexNibble(item.sha256[index * 2U]) << 4U) |
            hexNibble(item.sha256[index * 2U + 1U]));
    }
    return result;
}

}  // namespace

struct WindowsBootstrapFilesystemPort::State {
    explicit State(WindowsBootstrapFilesystemBackend& backendValue)
        : backend(backendValue) {}

    ~State() {
        if (backupRoot != 0) backend.CloseFilesystemHandle(backupRoot);
        if (transactionRoot != 0) backend.CloseFilesystemHandle(transactionRoot);
        if (stagingRoot != 0) backend.CloseFilesystemHandle(stagingRoot);
        if (installRoot != 0) backend.CloseFilesystemHandle(installRoot);
    }

    WindowsBootstrapFilesystemBackend& backend;
    ValidatedInstallPlan plan;
    ReplacementJournal journal;
    WindowsFilesystemHandle installRoot = 0;
    WindowsFilesystemHandle stagingRoot = 0;
    WindowsFilesystemHandle transactionRoot = 0;
    WindowsFilesystemHandle backupRoot = 0;
    WindowsFileIdentity installRootIdentity;
    WindowsFileIdentity stagingRootIdentity;
    WindowsFileIdentity transactionRootIdentity;
    WindowsFileIdentity backupRootIdentity;
    WindowsFileIdentity journalIdentity;
    WindowsFileContentFingerprint journalContent;
    // The journal retains the original authenticated updater image binding,
    // while a restart may temporarily use a new PID/creation tuple.  Recovery
    // replaces this value only after the new tuple has passed the exact image
    // identity check and has been durably published.
    BootstrapProcessIdentity durableParentProcessIdentity;
    std::vector<ItemState> items;
    std::string phase = "observed";
    std::string updaterOwnedBase;
    std::string invocationIdentity;
    std::uint64_t journalGeneration = 0;
    std::uint32_t journalSchemaVersion = 9;
    bool begun = false;
    bool initialJournalDurable = false;
    bool backupDirectoryStarted = false;
    bool backupDirectoryComplete = false;
    bool commitComplete = false;
    bool rollbackDurable = false;
    bool rollbackComplete = false;
    bool healthTokenConsumed = false;
    bool committedApplicationRelaunchPending = false;
    BootstrapProcessIdentity updatedChildIdentity;
    BootstrapProcessIdentity previousApplicationIdentity;
    std::string healthPhase;
    bool previousApplicationRelaunchStarted = false;
    bool previousApplicationRelaunchComplete = false;
    bool backupDirectoryRemoveStarted = false;
    bool backupDirectoryRemoved = false;
    bool blocked = false;
    std::uint32_t previousFailureCount = 0U;
    bool failureRecordedForTransaction = false;
    bool terminalJournalRetired = false;
    WindowsBootstrapInvocationCleanup invocationCleanup;
    bool invocationCleanupStarted = false;
    bool invocationCleanupComplete = false;
    bool runtimePairExpected = false;
    WindowsFileIdentity runtimePairDirectoryIdentity;
    WindowsFileIdentity runtimePairBootstrapIdentity;
    WindowsFileContentFingerprint runtimePairBootstrapContent;

    struct LeasedMutationResult {
        WindowsFilesystemBackendResult backendResult;
        bool mutationInvoked = false;
    };

    struct PreparedCheckpointContext {
        State* state = nullptr;
        ItemState* item = nullptr;
    };

    struct RestorePreparedCheckpointContext {
        State* state = nullptr;
        ItemState* item = nullptr;
    };

    static WindowsFilesystemBackendResult OnPrepared(
        void* context,
        const WindowsFileIdentity& preparedIdentity,
        const WindowsFileContentFingerprint& preparedContent,
        const WindowsFileMetadataSnapshot& preparedMetadata) {
        auto* checkpoint = static_cast<PreparedCheckpointContext*>(context);
        if (checkpoint == nullptr || checkpoint->state == nullptr ||
            checkpoint->item == nullptr || !preparedIdentity.valid ||
            !preparedContent.valid || !preparedMetadata.valid) {
            return {WindowsFilesystemBackendStatus::DurabilityFailed, 0};
        }
        checkpoint->item->preparedIdentity = preparedIdentity;
        checkpoint->item->preparedContent = preparedContent;
        checkpoint->item->preparedMetadata = preparedMetadata;
        checkpoint->item->replacementPrepared = true;
        const WindowsBootstrapFilesystemResult persisted =
            checkpoint->state->persist("replacement-prepared");
        return persisted.ok()
            ? WindowsFilesystemBackendResult{}
            : WindowsFilesystemBackendResult{
                  persisted.backendStatus == WindowsFilesystemBackendStatus::Ok
                      ? WindowsFilesystemBackendStatus::DurabilityFailed
                      : persisted.backendStatus,
                  persisted.nativeError};
    }

    static WindowsFilesystemBackendResult OnRestorePrepared(
        void* context,
        const WindowsFileIdentity& preparedIdentity,
        const WindowsFileContentFingerprint& preparedContent,
        const WindowsFileMetadataSnapshot& preparedMetadata) {
        auto* checkpoint =
            static_cast<RestorePreparedCheckpointContext*>(context);
        if (checkpoint == nullptr || checkpoint->state == nullptr ||
            checkpoint->item == nullptr || !preparedIdentity.valid ||
            !preparedContent.valid || !preparedMetadata.valid ||
            preparedContent != checkpoint->item->backupContent ||
            !checkpoint->state->metadataMatches(
                preparedMetadata, checkpoint->item->rollbackMetadata)) {
            return {WindowsFilesystemBackendStatus::DurabilityFailed, 0};
        }
        checkpoint->item->restorePreparedIdentity = preparedIdentity;
        checkpoint->item->restorePreparedContent = preparedContent;
        checkpoint->item->restorePreparedMetadata = preparedMetadata;
        checkpoint->item->restorePrepared = true;
        const WindowsBootstrapFilesystemResult persisted =
            checkpoint->state->persist("restore-prepared");
        return persisted.ok()
            ? WindowsFilesystemBackendResult{}
            : WindowsFilesystemBackendResult{
                  persisted.backendStatus == WindowsFilesystemBackendStatus::Ok
                      ? WindowsFilesystemBackendStatus::DurabilityFailed
                      : persisted.backendStatus,
                  persisted.nativeError};
    }

    template <typename Mutation>
    LeasedMutationResult mutateWithJournalLease(Mutation&& mutation) {
        WindowsFilesystemHandle lease = 0;
        WindowsFilesystemBackendResult acquired =
            backend.AcquireJournalMutationLease(transactionRoot, journalIdentity,
                                                journalContent, lease);
        if (!acquired.ok()) return {acquired, false};
        if (lease == 0) {
            return {{WindowsFilesystemBackendStatus::IdentityMismatch, 0}, false};
        }
        ScopedJournalMutationLease guard(backend, lease);
        return {std::forward<Mutation>(mutation)(), true};
    }

    ItemState* findItem(const std::string& relativePath) {
        const auto found = std::find_if(items.begin(), items.end(),
                                        [&relativePath](const ItemState& item) {
                                            return item.relativePath == relativePath;
                                        });
        return found == items.end() ? nullptr : &*found;
    }

    void bindStagingCleanupFile(const std::string& relativePath,
                                const WindowsFileIdentity& identity,
                                const WindowsFileContentFingerprint& content) {
        if (relativePath.empty() || !identity.valid || !content.valid) return;
        const auto alreadyBound = std::any_of(
            invocationCleanup.stagingFiles.begin(),
            invocationCleanup.stagingFiles.end(),
            [&relativePath](const WindowsBootstrapInvocationCleanupFile& file) {
                return insensitiveRelativePath(file.relativePath) ==
                       insensitiveRelativePath(relativePath);
            });
        if (alreadyBound) return;
        invocationCleanup.stagingFiles.push_back(
            {relativePath, identity, content});
    }

    void bindTransactionCleanupFile(
        const std::string& relativePath,
        const WindowsFileIdentity& identity,
        const WindowsFileContentFingerprint& content) {
        if (relativePath.empty() || !identity.valid || !content.valid) return;
        const auto alreadyBound = std::any_of(
            invocationCleanup.transactionFiles.begin(),
            invocationCleanup.transactionFiles.end(),
            [&relativePath](const WindowsBootstrapInvocationCleanupFile& file) {
                return insensitiveRelativePath(file.relativePath) ==
                       insensitiveRelativePath(relativePath);
            });
        if (alreadyBound) return;
        invocationCleanup.transactionFiles.push_back(
            {relativePath, identity, content});
    }

    void resetRuntimePairAuthority() {
        runtimePairExpected = false;
        runtimePairDirectoryIdentity = {};
        runtimePairBootstrapIdentity = {};
        runtimePairBootstrapContent = {};
        invocationCleanup.runtimePairExpected = false;
        invocationCleanup.runtimePairDirectoryIdentity = {};
        invocationCleanup.runtimePairBootstrapIdentity = {};
        invocationCleanup.runtimePairBootstrapContent = {};
        invocationCleanup.transactionFiles.clear();
    }

    void setRuntimePairAuthority(
        const bool expected,
        const WindowsFileIdentity& directoryIdentity,
        const WindowsFileIdentity& bootstrapIdentity,
        const WindowsFileContentFingerprint& bootstrapContent) {
        runtimePairExpected = expected;
        runtimePairDirectoryIdentity = directoryIdentity;
        runtimePairBootstrapIdentity = bootstrapIdentity;
        runtimePairBootstrapContent = bootstrapContent;
        invocationCleanup.runtimePairExpected = expected;
        invocationCleanup.runtimePairDirectoryIdentity = directoryIdentity;
        invocationCleanup.runtimePairBootstrapIdentity = bootstrapIdentity;
        invocationCleanup.runtimePairBootstrapContent = bootstrapContent;
    }

    WindowsBootstrapFilesystemResult bindObservedTransactionRuntimePair() {
        resetRuntimePairAuthority();
        WindowsFilesystemObservation directory;
        const WindowsFilesystemBackendResult directoryObserved =
            backend.ObserveNoFollow(transactionRoot, "runtime-pair", directory);
        if (!directoryObserved.ok()) {
            return result(observationFailure(directoryObserved), directoryObserved);
        }
        if (directory.kind == WindowsFilesystemEntryKind::Missing) return {};
        if (directory.kind == WindowsFilesystemEntryKind::ReparsePoint) {
            return result(WindowsBootstrapFilesystemStatus::ReparsePointRejected);
        }
        if (directory.kind != WindowsFilesystemEntryKind::Directory ||
            !directory.identity.valid) {
            return result(WindowsBootstrapFilesystemStatus::UnexpectedEntryType);
        }
        WindowsFilesystemObservation bootstrap;
        const WindowsFilesystemBackendResult bootstrapObserved =
            backend.ObserveNoFollow(transactionRoot,
                                     "runtime-pair/HarkenSearchBootstrap.exe",
                                     bootstrap);
        if (!bootstrapObserved.ok()) {
            return result(observationFailure(bootstrapObserved), bootstrapObserved);
        }
        if (bootstrap.kind == WindowsFilesystemEntryKind::ReparsePoint) {
            return result(WindowsBootstrapFilesystemStatus::ReparsePointRejected);
        }
        if (bootstrap.kind != WindowsFilesystemEntryKind::RegularFile ||
            !bootstrap.identity.valid || !bootstrap.content.valid) {
            return result(WindowsBootstrapFilesystemStatus::UnexpectedEntryType);
        }
        setRuntimePairAuthority(true, directory.identity, bootstrap.identity,
                                bootstrap.content);
        bindTransactionCleanupFile("runtime-pair/HarkenSearchBootstrap.exe",
                                   bootstrap.identity, bootstrap.content);
        return {};
    }

    WindowsBootstrapFilesystemResult bindDurableTransactionRuntimePair(
        const WindowsBootstrapJournalRecord& durable) {
        resetRuntimePairAuthority();
        setRuntimePairAuthority(
            durable.runtimePairExpected, durable.runtimePairDirectoryIdentity,
            durable.runtimePairBootstrapIdentity,
            durable.runtimePairBootstrapContent);
        invocationCleanup.allowAlreadyRemoved = invocationCleanupStarted;
        if (!durable.runtimePairExpected) {
            WindowsFilesystemObservation absent;
            const WindowsFilesystemBackendResult observed =
                backend.ObserveNoFollow(transactionRoot, "runtime-pair", absent);
            if (!observed.ok()) {
                return result(observationFailure(observed), observed);
            }
            if (absent.kind == WindowsFilesystemEntryKind::Missing) return {};
            return result(absent.kind == WindowsFilesystemEntryKind::ReparsePoint
                              ? WindowsBootstrapFilesystemStatus::ReparsePointRejected
                              : WindowsBootstrapFilesystemStatus::IdentityMismatch);
        }
        if (!durable.runtimePairDirectoryIdentity.valid ||
            !durable.runtimePairBootstrapIdentity.valid ||
            !durable.runtimePairBootstrapContent.valid) {
        return result(WindowsBootstrapFilesystemStatus::InvalidJournal);
    }

    // Preserve the durable cleanup inventory even when the prior invocation
    // already removed the runtime-pair.  Recovery must still carry the exact
    // identity/content authority so a retry can accept the known absence and
    // reject any same-name replacement.
    bindTransactionCleanupFile("runtime-pair/HarkenSearchBootstrap.exe",
                               durable.runtimePairBootstrapIdentity,
                               durable.runtimePairBootstrapContent);

    WindowsFilesystemObservation directory;
        const WindowsFilesystemBackendResult directoryObserved =
            backend.ObserveNoFollow(transactionRoot, "runtime-pair", directory);
        if (!directoryObserved.ok()) {
            return result(observationFailure(directoryObserved), directoryObserved);
        }
        if (directory.kind == WindowsFilesystemEntryKind::Missing) {
            return invocationCleanupStarted
                ? WindowsBootstrapFilesystemResult{}
                : result(WindowsBootstrapFilesystemStatus::IdentityMismatch);
        }
        if (directory.kind == WindowsFilesystemEntryKind::ReparsePoint) {
            return result(WindowsBootstrapFilesystemStatus::ReparsePointRejected);
        }
        if (directory.kind != WindowsFilesystemEntryKind::Directory ||
            directory.identity != durable.runtimePairDirectoryIdentity) {
            return result(WindowsBootstrapFilesystemStatus::IdentityMismatch);
        }

        WindowsFilesystemObservation bootstrap;
        const WindowsFilesystemBackendResult bootstrapObserved =
            backend.ObserveNoFollow(transactionRoot,
                                    "runtime-pair/HarkenSearchBootstrap.exe",
                                    bootstrap);
        if (!bootstrapObserved.ok()) {
            return result(observationFailure(bootstrapObserved), bootstrapObserved);
        }
        if (bootstrap.kind == WindowsFilesystemEntryKind::Missing) {
            return invocationCleanupStarted
                ? WindowsBootstrapFilesystemResult{}
                : result(WindowsBootstrapFilesystemStatus::IdentityMismatch);
        }
        if (bootstrap.kind == WindowsFilesystemEntryKind::ReparsePoint) {
            return result(WindowsBootstrapFilesystemStatus::ReparsePointRejected);
        }
        if (bootstrap.kind != WindowsFilesystemEntryKind::RegularFile ||
            bootstrap.identity != durable.runtimePairBootstrapIdentity) {
            return result(WindowsBootstrapFilesystemStatus::IdentityMismatch);
        }
        if (!bootstrap.content.valid ||
            bootstrap.content != durable.runtimePairBootstrapContent) {
            return result(WindowsBootstrapFilesystemStatus::ContentMismatch);
        }
        return {};
    }

    // The runtime-pair is an invocation-owned namespace, not a best-effort
    // scratch directory.  Recheck its exact authority before every journal
    // generation so a same-name object injected after Begin() cannot become
    // durable merely because cleanup has not started yet.  Once cleanup has
    // a durable start cut, the known object may be absent as an idempotent
    // result; a different identity or content is never accepted.
    WindowsBootstrapFilesystemResult verifyRuntimePairAuthority(
        const bool allowKnownMissing) {
        const bool authorityComplete =
            runtimePairExpected && runtimePairDirectoryIdentity.valid &&
            runtimePairBootstrapIdentity.valid &&
            runtimePairBootstrapContent.valid;
        if (!runtimePairExpected) {
            if (runtimePairDirectoryIdentity.valid ||
                runtimePairBootstrapIdentity.valid ||
                runtimePairBootstrapContent.valid) {
                return result(WindowsBootstrapFilesystemStatus::InvalidJournal);
            }
            WindowsFilesystemObservation absent;
            const WindowsFilesystemBackendResult observed =
                backend.ObserveNoFollow(transactionRoot, "runtime-pair", absent);
            if (!observed.ok()) {
                return result(observationFailure(observed), observed);
            }
            if (absent.kind == WindowsFilesystemEntryKind::Missing) return {};
            if (absent.kind == WindowsFilesystemEntryKind::ReparsePoint) {
                return result(WindowsBootstrapFilesystemStatus::ReparsePointRejected);
            }
            return result(WindowsBootstrapFilesystemStatus::IdentityMismatch);
        }
        if (!authorityComplete) {
            return result(WindowsBootstrapFilesystemStatus::InvalidJournal);
        }

        WindowsFilesystemObservation directory;
        const WindowsFilesystemBackendResult directoryObserved =
            backend.ObserveNoFollow(transactionRoot, "runtime-pair", directory);
        if (!directoryObserved.ok()) {
            return result(observationFailure(directoryObserved), directoryObserved);
        }
        if (directory.kind == WindowsFilesystemEntryKind::Missing) {
            return allowKnownMissing
                ? WindowsBootstrapFilesystemResult{}
                : result(WindowsBootstrapFilesystemStatus::IdentityMismatch);
        }
        if (directory.kind == WindowsFilesystemEntryKind::ReparsePoint) {
            return result(WindowsBootstrapFilesystemStatus::ReparsePointRejected);
        }
        if (directory.kind != WindowsFilesystemEntryKind::Directory ||
            directory.identity != runtimePairDirectoryIdentity) {
            return result(WindowsBootstrapFilesystemStatus::IdentityMismatch);
        }

        WindowsFilesystemObservation bootstrap;
        const WindowsFilesystemBackendResult bootstrapObserved =
            backend.ObserveNoFollow(transactionRoot,
                                    "runtime-pair/HarkenSearchBootstrap.exe",
                                    bootstrap);
        if (!bootstrapObserved.ok()) {
            return result(observationFailure(bootstrapObserved), bootstrapObserved);
        }
        if (bootstrap.kind == WindowsFilesystemEntryKind::Missing) {
            return allowKnownMissing
                ? WindowsBootstrapFilesystemResult{}
                : result(WindowsBootstrapFilesystemStatus::IdentityMismatch);
        }
        if (bootstrap.kind == WindowsFilesystemEntryKind::ReparsePoint) {
            return result(WindowsBootstrapFilesystemStatus::ReparsePointRejected);
        }
        if (bootstrap.kind != WindowsFilesystemEntryKind::RegularFile ||
            bootstrap.identity != runtimePairBootstrapIdentity) {
            return result(WindowsBootstrapFilesystemStatus::IdentityMismatch);
        }
        if (!bootstrap.content.valid ||
            bootstrap.content != runtimePairBootstrapContent) {
            return result(WindowsBootstrapFilesystemStatus::ContentMismatch);
        }
        return {};
    }

    bool metadataMatches(const WindowsFileMetadataSnapshot& observed,
                         const WindowsFileMetadataSnapshot& expected) const {
        return backend.MetadataMatchesForRecovery(observed, expected);
    }

    bool allBackupsComplete() const {
        return std::all_of(items.begin(), items.end(), [](const ItemState& item) {
            return (item.classification != JournalItemClass::ReplaceExisting &&
                    item.classification != JournalItemClass::RemoveExisting) ||
                   item.backupComplete || item.removalAbsentVerified;
        });
    }

    bool allReplacementsComplete() const {
        return std::all_of(items.begin(), items.end(), [](const ItemState& item) {
            return item.classification == JournalItemClass::PreserveExisting ||
                   item.replacementComplete;
        });
    }

    void updateRollbackComplete() {
        rollbackComplete = std::all_of(items.begin(), items.end(), [](const ItemState& item) {
            if (item.classification != JournalItemClass::PreserveExisting &&
                item.replacementStarted && !item.replacementComplete) {
                // A prepared forward publication has no rollback intent that
                // can safely remove it.  Keep the transaction non-terminal;
                // restart must quarantine this residue rather than claiming
                // that rollback completed.
                return false;
            }
            if ((item.classification == JournalItemClass::ReplaceExisting ||
                 item.classification == JournalItemClass::RemoveExisting) &&
                item.replacementComplete) {
                return item.restoreComplete || item.removalAbsentVerified;
            }
            if (item.classification == JournalItemClass::CreateNew &&
                item.replacementComplete) {
                return item.removeNewComplete;
            }
            return true;
        });
    }

    std::vector<std::uint8_t> serialize() const {
        WindowsBootstrapJournalRecord record;
        record.schemaVersion = journalSchemaVersion;
        record.generation = journalGeneration;
        record.transactionId = journal.transactionId;
        record.deploymentMode = plan.identity.deploymentMode;
        record.installationId = plan.identity.installationId;
        record.registeredAppId = plan.identity.registeredAppId;
        record.targetRoot = plan.identity.targetRoot;
        record.targetRootIdentity = plan.identity.targetRootIdentity;
        record.oldExecutableIdentity = plan.identity.oldExecutableIdentity;
        record.updaterOwnedBase = updaterOwnedBase;
        record.stagingRoot = plan.invocation.stagingRoot;
        record.transactionRoot = plan.invocation.transactionRoot;
        record.protectedDataRoots = plan.identity.protectedDataRoots;
        record.previousFailureCount = previousFailureCount;
        record.failureRecordedForTransaction = failureRecordedForTransaction;
        record.invocationIdentity = invocationIdentity;
        record.version = plan.version;
        record.buildId = plan.buildId;
        record.packagePlanDigest = plan.packagePlanDigest;
        record.packageKeyId = plan.packageKeyId;
        record.packageChannel = plan.packageChannel;
        record.packagePlanBytes = plan.packagePlanBytes;
        record.packageSignatureHex = plan.packageSignatureHex;
        record.healthNonceDigest = plan.invocation.healthNonceDigest.empty()
            ? BootstrapSha256Hex(plan.healthToken)
            : plan.invocation.healthNonceDigest;
        record.healthToken = plan.healthToken;
        record.oldProcessId = plan.oldProcessId;
        record.oldProcessIdentity = plan.invocation.oldProcess;
        record.parentProcessIdentity = durableParentProcessIdentity.valid
            ? durableParentProcessIdentity
            : plan.invocation.parentProcess;
        record.updatedChildIdentity = updatedChildIdentity;
        record.previousApplicationIdentity = previousApplicationIdentity;
        record.healthPhase = healthPhase;
        record.entrypoint = plan.entrypoint;
        record.phase = phase;
        record.installRootIdentity = installRootIdentity;
        record.stagingRootIdentity = stagingRootIdentity;
        record.transactionRootIdentity = transactionRootIdentity;
        record.backupRootIdentity = backupRootIdentity;
        record.journalIdentity = journalIdentity;
        record.journalContent = journalContent;
        record.initialJournalDurable = initialJournalDurable;
        record.backupDirectoryStarted = backupDirectoryStarted;
        record.backupDirectoryComplete = backupDirectoryComplete;
        record.commitComplete = commitComplete;
        record.rollbackDurable = rollbackDurable;
        record.rollbackComplete = rollbackComplete;
        record.backupDirectoryRemoveStarted = backupDirectoryRemoveStarted;
        record.backupDirectoryRemoved = backupDirectoryRemoved;
        record.healthTokenConsumed = healthTokenConsumed;
        record.committedApplicationRelaunchPending =
            committedApplicationRelaunchPending;
        record.previousApplicationRelaunchStarted =
            previousApplicationRelaunchStarted;
        record.previousApplicationRelaunchComplete =
            previousApplicationRelaunchComplete;
        record.runtimePairExpected = runtimePairExpected;
        record.runtimePairDirectoryIdentity = runtimePairDirectoryIdentity;
        record.runtimePairBootstrapIdentity = runtimePairBootstrapIdentity;
        record.runtimePairBootstrapContent = runtimePairBootstrapContent;
        record.invocationCleanupStarted = invocationCleanupStarted;
        record.invocationCleanupComplete = invocationCleanupComplete;
        record.items.reserve(items.size());
        for (const ItemState& source : items) {
            WindowsBootstrapJournalItemRecord item;
            item.relativePath = source.relativePath;
            item.backupRelativePath = source.backupRelativePath;
            item.forwardTemporaryLeaf = source.forwardTemporaryLeaf;
            item.restoreTemporaryLeaf = source.restoreTemporaryLeaf;
            item.classification = source.classification;
            item.stagingIdentity = source.stagingIdentity;
            item.installParentIdentity = source.installParentIdentity;
            item.originalIdentity = source.originalIdentity;
            item.backupIdentity = source.backupIdentity;
            item.installedIdentity = source.installedIdentity;
            item.restoredIdentity = source.restoredIdentity;
            item.preparedIdentity = source.preparedIdentity;
            item.restorePreparedIdentity = source.restorePreparedIdentity;
            item.stagingContent = source.stagingContent;
            item.backupContent = source.backupContent;
            item.preparedContent = source.preparedContent;
            item.restorePreparedContent = source.restorePreparedContent;
            item.rollbackMetadata = source.rollbackMetadata;
            item.installedMetadata = source.installedMetadata;
            item.restoredMetadata = source.restoredMetadata;
            item.preparedMetadata = source.preparedMetadata;
            item.restorePreparedMetadata = source.restorePreparedMetadata;
            item.removalAbsentVerified = source.removalAbsentVerified;
            item.backupStarted = source.backupStarted;
            item.backupComplete = source.backupComplete;
            item.replacementStarted = source.replacementStarted;
            item.replacementPrepared = source.replacementPrepared;
            item.replacementComplete = source.replacementComplete;
            item.restoreStarted = source.restoreStarted;
            item.restorePrepared = source.restorePrepared;
            item.restoreComplete = source.restoreComplete;
            item.removeNewStarted = source.removeNewStarted;
            item.removeNewComplete = source.removeNewComplete;
            item.removeBackupStarted = source.removeBackupStarted;
            item.removeBackupComplete = source.removeBackupComplete;
            record.items.push_back(std::move(item));
        }
        return EncodeWindowsBootstrapJournal(record);
    }

    WindowsBootstrapFilesystemResult persist(const std::string& newPhase) {
        WindowsBootstrapFilesystemResult runtimePairVerified;
        bool runtimePairReady = false;
        for (std::uint32_t attempt = 0U;
             attempt < kJournalPersistRetryAttempts; ++attempt) {
            runtimePairVerified =
                verifyRuntimePairAuthority(invocationCleanupStarted);
            if (runtimePairVerified.ok()) {
                runtimePairReady = true;
                break;
            }
            if (runtimePairVerified.backendStatus !=
                WindowsFilesystemBackendStatus::AccessDenied) {
                blocked = true;
                return runtimePairVerified;
            }
            if (attempt + 1U < kJournalPersistRetryAttempts) {
                std::this_thread::sleep_for(kJournalPersistRetryDelay);
            }
        }
        if (!runtimePairReady) {
            blocked = true;
            return result(
                WindowsBootstrapFilesystemStatus::PartialDurableState,
                {runtimePairVerified.backendStatus,
                 runtimePairVerified.nativeError});
        }
        phase = newPhase;
        ++journalGeneration;
        if (journalGeneration == 0U) {
            blocked = true;
            return result(WindowsBootstrapFilesystemStatus::PartialDurableState);
        }
        WindowsFileIdentity replacementIdentity;
        WindowsFileContentFingerprint replacementContent;
        const std::vector<std::uint8_t> encoded = serialize();
        if (encoded.empty()) {
            blocked = true;
            return result(WindowsBootstrapFilesystemStatus::PartialDurableState);
        }
        const WindowsFilesystemReplaceMode mode = journalIdentity.valid
            ? WindowsFilesystemReplaceMode::ReplaceExisting
            : WindowsFilesystemReplaceMode::CreateNew;
        WindowsFilesystemBackendResult persisted;
        bool journalPersisted = false;
        for (std::uint32_t attempt = 0U;
             attempt < kJournalPersistRetryAttempts; ++attempt) {
            replacementIdentity = {};
            replacementContent = {};
            persisted = backend.PersistJournalDurable(
                transactionRoot, encoded, mode, journalIdentity,
                journalContent, replacementIdentity, replacementContent);
            if (persisted.ok() && replacementIdentity.valid &&
                replacementContent.valid) {
                journalPersisted = true;
                break;
            }
            if (persisted.status !=
                WindowsFilesystemBackendStatus::AccessDenied ||
                attempt + 1U >= kJournalPersistRetryAttempts) {
                break;
            }
            std::this_thread::sleep_for(kJournalPersistRetryDelay);
        }
        if (!journalPersisted) {
            blocked = true;
            return result(WindowsBootstrapFilesystemStatus::PartialDurableState,
                          persisted);
        }
        journalIdentity = replacementIdentity;
        journalContent = replacementContent;
        return {};
    }

    WindowsBootstrapFilesystemResult blockAfterStarted(
        const WindowsFilesystemBackendResult backendResult,
        const WindowsBootstrapFilesystemStatus failure,
        const bool accessDeniedIsRetryable = false) {
        if (accessDeniedIsRetryable &&
            backendResult.status ==
                WindowsFilesystemBackendStatus::AccessDenied) {
            return result(
                WindowsBootstrapFilesystemStatus::RetryableAccessDenied,
                backendResult);
        }
        blocked = true;
        return result(failure, backendResult);
    }
};

WindowsBootstrapFilesystemPort::WindowsBootstrapFilesystemPort(
    WindowsBootstrapFilesystemBackend& backend,
    const std::optional<WindowsBootstrapJournalPackageAuthority> packageAuthority)
    : backend_(backend), state_(std::make_unique<State>(backend)),
      packageAuthority_(packageAuthority) {}

WindowsBootstrapFilesystemPort::~WindowsBootstrapFilesystemPort() = default;

WindowsBootstrapFilesystemResult WindowsBootstrapFilesystemPort::Begin(
    const ValidatedInstallPlan& plan,
    const ReplacementJournal& journal,
    const WindowsBootstrapFilesystemRoots& roots) {
    if (state_ != nullptr && state_->begun) {
        return result(WindowsBootstrapFilesystemStatus::InvalidIntentOrder);
    }
    state_ = std::make_unique<State>(backend_);
    State& state = *state_;
    if (!plan.isValidated()) {
        return result(WindowsBootstrapFilesystemStatus::InvalidPlan);
    }
    if (!journal.isValidatedFor(plan)) {
        return result(WindowsBootstrapFilesystemStatus::InvalidJournal);
    }
    if (roots.stagingRoot.empty() || roots.transactionRoot.empty()) {
        return result(WindowsBootstrapFilesystemStatus::InvalidRoots);
    }
    if (!rootsMatchSignedInvocation(plan, roots)) {
        return result(WindowsBootstrapFilesystemStatus::InvalidRoots);
    }
    const WindowsFilesystemBackendResult trustedRoots =
        backend_.ValidateTrustedWriteRoots(plan, roots);
    if (!trustedRoots.ok()) {
        return result(WindowsBootstrapFilesystemStatus::InvalidRoots,
                      trustedRoots);
    }
    state.plan = plan;
    state.journal = journal;
    state.durableParentProcessIdentity = plan.invocation.parentProcess;
    state.updaterOwnedBase = plan.invocation.updaterOwnedBase;
    state.invocationIdentity = plan.invocation.invocationIdentity;

    const auto openRoot = [&state](const std::string& path,
                                   const WindowsFilesystemRootRole role,
                                   WindowsFilesystemHandle& handle,
                                   WindowsFileIdentity& identity) {
        const WindowsFilesystemBackendResult opened =
            state.backend.OpenRootExact(path, role, handle, identity);
        if (!opened.ok()) {
            return result(observationFailure(opened), opened);
        }
        if (handle == 0 || !identity.valid) {
            return result(WindowsBootstrapFilesystemStatus::RootIdentityMismatch);
        }
        return WindowsBootstrapFilesystemResult{};
    };

    WindowsBootstrapFilesystemResult opened =
        openRoot(plan.identity.targetRoot, WindowsFilesystemRootRole::Install,
                 state.installRoot, state.installRootIdentity);
    if (!opened.ok()) return opened;
    if (!sameDeploymentObject(plan.identity.targetRootIdentity,
                              state.installRootIdentity)) {
        return result(WindowsBootstrapFilesystemStatus::RootIdentityMismatch);
    }
    opened = openRoot(roots.stagingRoot, WindowsFilesystemRootRole::Staging,
                      state.stagingRoot, state.stagingRootIdentity);
    if (!opened.ok()) return opened;
    opened = openRoot(roots.transactionRoot, WindowsFilesystemRootRole::Transaction,
                      state.transactionRoot, state.transactionRootIdentity);
    if (!opened.ok()) return opened;
    if (sameObject(state.installRootIdentity, state.stagingRootIdentity) ||
        sameObject(state.installRootIdentity, state.transactionRootIdentity) ||
        sameObject(state.stagingRootIdentity, state.transactionRootIdentity)) {
        return result(WindowsBootstrapFilesystemStatus::RootIdentityMismatch);
    }
    if (state.installRootIdentity.volumeSerial !=
            state.stagingRootIdentity.volumeSerial ||
        state.installRootIdentity.volumeSerial !=
            state.transactionRootIdentity.volumeSerial) {
        return result(WindowsBootstrapFilesystemStatus::RootVolumeMismatch);
    }
    if (rootsOverlap(plan.identity.targetRoot, roots.stagingRoot) ||
        rootsOverlap(plan.identity.targetRoot, roots.transactionRoot) ||
        rootsOverlap(roots.stagingRoot, roots.transactionRoot)) {
        return result(WindowsBootstrapFilesystemStatus::RootOverlapRejected);
    }

    for (const char* journalPath : {
             kJournalFile, kJournalTemporaryFile, kJournalSlotA, kJournalSlotB,
             kJournalSlotATemporary, kJournalSlotBTemporary}) {
        WindowsFilesystemObservation observation;
        const WindowsFilesystemBackendResult observed =
            backend_.ObserveNoFollow(state.transactionRoot, journalPath, observation);
        if (!observed.ok()) return result(observationFailure(observed), observed);
        if (observation.kind == WindowsFilesystemEntryKind::ReparsePoint) {
            return result(WindowsBootstrapFilesystemStatus::ReparsePointRejected);
        }
        if (observation.kind != WindowsFilesystemEntryKind::Missing) {
            return result(WindowsBootstrapFilesystemStatus::JournalCollision);
        }
    }

    WindowsFilesystemObservation backupDirectory;
    WindowsFilesystemBackendResult observed =
        backend_.ObserveNoFollow(state.transactionRoot, kBackupDirectory,
                                 backupDirectory);
    if (!observed.ok()) return result(observationFailure(observed), observed);
    if (backupDirectory.kind == WindowsFilesystemEntryKind::ReparsePoint) {
        return result(WindowsBootstrapFilesystemStatus::ReparsePointRejected);
    }
    if (backupDirectory.kind != WindowsFilesystemEntryKind::Missing) {
        return result(WindowsBootstrapFilesystemStatus::PartialDurableState);
    }

    std::set<std::string> reservedInstallPaths;
    std::set<std::string> observedStagingIdentities;
    std::set<std::string> observedInstallIdentities;
    for (const ReplacementJournalItem& item : journal.items) {
        reservedInstallPaths.insert(insensitiveRelativePath(item.relativePath));
    }
    state.items.reserve(journal.items.size());
    for (std::size_t journalIndex = 0; journalIndex < journal.items.size();
         ++journalIndex) {
        const ReplacementJournalItem& journalItem = journal.items[journalIndex];
        ItemState item;
        item.relativePath = journalItem.relativePath;
        item.backupRelativePath = "item-" + std::to_string(journalIndex) + ".bak";
        item.installParentIdentity = state.installRootIdentity;
        const std::string temporaryPrefix =
            ".aris-bootstrap-" + journal.transactionId + "-" +
            std::to_string(journalIndex);
        item.classification = journalItem.classification;
        const auto reserveTemporaryLeaf =
            [&reservedInstallPaths, &journalItem, &temporaryPrefix](
                const std::string& operation) {
                for (std::size_t attempt = 0;; ++attempt) {
                    const std::string suffix = attempt == 0
                        ? std::string{}
                        : "-" + std::to_string(attempt);
                    const std::string leaf =
                        temporaryPrefix + "-" + operation + suffix + ".tmp";
                    const std::string relative = siblingPath(
                        journalItem.relativePath, leaf);
                    if (reservedInstallPaths.insert(
                            insensitiveRelativePath(relative)).second) {
                        return leaf;
                    }
                }
            };
        if (journalItem.classification != JournalItemClass::PreserveExisting) {
            item.forwardTemporaryLeaf = reserveTemporaryLeaf("forward");
            item.restoreTemporaryLeaf = reserveTemporaryLeaf("restore");
        }

        const auto incoming = std::find_if(
            plan.inventory.begin(), plan.inventory.end(),
            [&journalItem](const InstallInventoryItem& candidate) {
                return candidate.relativePath == journalItem.relativePath;
            });
        if (journalItem.classification == JournalItemClass::ReplaceExisting ||
            journalItem.classification == JournalItemClass::CreateNew) {
            if (incoming == plan.inventory.end()) {
                return result(WindowsBootstrapFilesystemStatus::InvalidJournal);
            }
            for (const std::string& parent : parentPaths(journalItem.relativePath)) {
                WindowsFilesystemObservation parentObservation;
                observed = backend_.ObserveNoFollow(state.stagingRoot, parent,
                                                    parentObservation);
                if (!observed.ok()) {
                    return result(observationFailure(observed), observed);
                }
                if (parentObservation.kind ==
                    WindowsFilesystemEntryKind::ReparsePoint) {
                    return result(
                        WindowsBootstrapFilesystemStatus::ReparsePointRejected);
                }
                if (parentObservation.kind != WindowsFilesystemEntryKind::Directory ||
                    !parentObservation.identity.valid) {
                    return result(WindowsBootstrapFilesystemStatus::MissingPayloadItem);
                }
            }
            WindowsFilesystemObservation stagingObservation;
            observed = backend_.ObserveNoFollow(state.stagingRoot,
                                                journalItem.relativePath,
                                                stagingObservation);
            if (!observed.ok()) return result(observationFailure(observed), observed);
            if (stagingObservation.kind == WindowsFilesystemEntryKind::ReparsePoint) {
                return result(WindowsBootstrapFilesystemStatus::ReparsePointRejected);
            }
            if (stagingObservation.kind != WindowsFilesystemEntryKind::RegularFile ||
                !stagingObservation.identity.valid) {
                return result(WindowsBootstrapFilesystemStatus::MissingPayloadItem);
            }
            item.stagingIdentity = stagingObservation.identity;
            if (!observedStagingIdentities.insert(
                    identityText(item.stagingIdentity)).second) {
                return result(WindowsBootstrapFilesystemStatus::IdentityMismatch);
            }
            item.stagingContent = inventoryFingerprint(*incoming);
            if (!item.stagingContent.valid) {
                return result(WindowsBootstrapFilesystemStatus::InvalidPlan);
            }
            if (!stagingObservation.content.valid ||
                stagingObservation.content != item.stagingContent) {
                return result(WindowsBootstrapFilesystemStatus::ContentMismatch);
            }
        }

        bool missingInstallParent = false;
        for (const std::string& parent : parentPaths(journalItem.relativePath)) {
            if (reservedInstallPaths.count(insensitiveRelativePath(parent)) != 0U) {
                return result(WindowsBootstrapFilesystemStatus::UnexpectedEntryType);
            }
            if (missingInstallParent) continue;
            WindowsFilesystemObservation parentObservation;
            observed = backend_.ObserveNoFollow(state.installRoot, parent,
                                                parentObservation);
            if (!observed.ok()) return result(observationFailure(observed), observed);
            if (parentObservation.kind == WindowsFilesystemEntryKind::ReparsePoint) {
                return result(WindowsBootstrapFilesystemStatus::ReparsePointRejected);
            }
            if (parentObservation.kind == WindowsFilesystemEntryKind::Missing &&
                item.classification == JournalItemClass::CreateNew) {
                // Admission remains read-only. Schema 10 records this signed
                // new item's parent as unbound until durable preparation.
                missingInstallParent = true;
                item.installParentIdentity = {};
                state.journalSchemaVersion = 10;
                continue;
            }
            if (parentObservation.kind != WindowsFilesystemEntryKind::Directory ||
                !parentObservation.identity.valid) {
                return result(WindowsBootstrapFilesystemStatus::UnexpectedEntryType);
            }
            item.installParentIdentity = parentObservation.identity;
        }

        if (journalItem.classification != JournalItemClass::PreserveExisting) {
            for (const std::string& temporaryPath : {
                     siblingPath(journalItem.relativePath, item.forwardTemporaryLeaf),
                     siblingPath(journalItem.relativePath, item.restoreTemporaryLeaf)}) {
                WindowsFilesystemObservation temporaryObservation;
                observed = backend_.ObserveNoFollow(state.installRoot, temporaryPath,
                                                    temporaryObservation);
                if (!observed.ok()) {
                    return result(observationFailure(observed), observed);
                }
                if (temporaryObservation.kind ==
                    WindowsFilesystemEntryKind::ReparsePoint) {
                    return result(
                        WindowsBootstrapFilesystemStatus::ReparsePointRejected);
                }
                if (temporaryObservation.kind != WindowsFilesystemEntryKind::Missing) {
                    return result(WindowsBootstrapFilesystemStatus::UnknownTargetItem);
                }
            }
        }

        WindowsFilesystemObservation targetObservation;
        observed = backend_.ObserveNoFollow(state.installRoot,
                                            journalItem.relativePath,
                                            targetObservation);
        if (!observed.ok()) return result(observationFailure(observed), observed);
        if (targetObservation.kind == WindowsFilesystemEntryKind::ReparsePoint) {
            return result(WindowsBootstrapFilesystemStatus::ReparsePointRejected);
        }
        if (journalItem.classification == JournalItemClass::CreateNew) {
            if (targetObservation.kind != WindowsFilesystemEntryKind::Missing) {
                return result(WindowsBootstrapFilesystemStatus::UnknownTargetItem);
            }
        } else if (journalItem.classification ==
                       JournalItemClass::RemoveExisting &&
                   targetObservation.kind ==
                       WindowsFilesystemEntryKind::Missing) {
            item.removalAbsentVerified = true;
            item.replacementComplete = true;
        } else {
            if (targetObservation.kind == WindowsFilesystemEntryKind::Missing) {
                return result(WindowsBootstrapFilesystemStatus::ExistingItemMissing);
            }
            if (targetObservation.kind != WindowsFilesystemEntryKind::RegularFile ||
                !targetObservation.identity.valid) {
                return result(WindowsBootstrapFilesystemStatus::UnexpectedEntryType);
            }
            item.originalIdentity = targetObservation.identity;
            item.backupContent = targetObservation.content;
            item.rollbackMetadata = targetObservation.metadata;
            if (!item.backupContent.valid ||
                !item.rollbackMetadata.valid) {
                return result(
                    WindowsBootstrapFilesystemStatus::UnsupportedMetadata);
            }
            if (!observedInstallIdentities.insert(
                    identityText(item.originalIdentity)).second) {
                return result(WindowsBootstrapFilesystemStatus::IdentityMismatch);
            }
        }
        state.items.push_back(std::move(item));
    }

    for (const ItemState& item : state.items) {
        state.bindStagingCleanupFile(item.relativePath,
                                     item.stagingIdentity,
                                     item.stagingContent);
    }
    const WindowsBootstrapFilesystemResult runtimePairBound =
        state.bindObservedTransactionRuntimePair();
    if (!runtimePairBound.ok()) return runtimePairBound;

    state.begun = true;
    return {};
}

bool terminalParentMatchesRuntimePair(
    const BootstrapProcessIdentity& parent,
    const WindowsBootstrapJournalRecord& durable,
    const std::string& transactionRoot) {
    if (!validOwnedProcessIdentity(parent) || !durable.runtimePairExpected ||
        !durable.runtimePairBootstrapIdentity.valid ||
        transactionRoot.empty()) {
        return false;
    }
    const std::string expectedImage = comparableRoot(
        transactionRoot + "/runtime-pair/HarkenSearchBootstrap.exe");
    return comparableRoot(parent.imagePath) == expectedImage &&
           parent.imageVolumeSerial ==
               durable.runtimePairBootstrapIdentity.volumeSerial &&
           parent.imageFileId == durable.runtimePairBootstrapIdentity.fileId;
}

WindowsBootstrapFilesystemResult
WindowsBootstrapFilesystemPort::BindPreviousFailureCount(
    const std::uint32_t count) {
    if (state_ == nullptr || !state_->begun || state_->blocked ||
        state_->initialJournalDurable || state_->journalGeneration != 0U ||
        state_->phase != "observed") {
        return result(WindowsBootstrapFilesystemStatus::InvalidIntentOrder);
    }
    state_->previousFailureCount = count;
    return {};
}

WindowsBootstrapFilesystemRecoveryResult
WindowsBootstrapFilesystemPort::BeginRecovery(
    const ValidatedInstallPlan& plan,
    const ReplacementJournal& journal,
    const WindowsBootstrapFilesystemRoots& roots) {
    return BeginRecoveryInternal(plan, &journal, roots, nullptr);
}

WindowsBootstrapFilesystemRecoveryResult
WindowsBootstrapFilesystemPort::BeginRecovery(
    const ValidatedInstallPlan& plan,
    const WindowsBootstrapFilesystemRoots& roots) {
    return BeginRecoveryInternal(plan, nullptr, roots, nullptr);
}

WindowsBootstrapFilesystemRecoveryResult
WindowsBootstrapFilesystemPort::BeginTerminalCleanup(
    const ValidatedInstallPlan& plan,
    const ReplacementJournal& journal,
    const WindowsBootstrapFilesystemRoots& roots,
    const BootstrapProcessIdentity& inheritedParentProcess) {
    WindowsBootstrapFilesystemRecoveryResult recovery;
    if (!journal.isValidatedFor(plan)) {
        recovery.filesystem.status =
            WindowsBootstrapFilesystemStatus::InvalidJournal;
        recovery.disposition = WindowsBootstrapRecoveryDisposition::Quarantine;
        recovery.intents = {{BootstrapIntentKind::QuarantinePackage, {}, 0,
                             "bootstrap-terminal-cleanup-invalid-journal"}};
        return recovery;
    }
    for (std::uint32_t attempt = 0U;
         attempt < kJournalPersistRetryAttempts; ++attempt) {
        // The handoff journal is reconstructed from the parent's inventory
        // snapshot and may be stale by the time the parent has published its
        // final cleanup checkpoints.  The authenticated durable journal is
        // the terminal cleanup authority; rehydrate it after validating the
        // handoff value above instead of requiring snapshot equality.
        recovery = BeginRecoveryInternal(plan, nullptr, roots,
                                         &inheritedParentProcess);
        if (recovery.filesystem.status !=
                WindowsBootstrapFilesystemStatus::RetryableAccessDenied ||
            attempt + 1U >= kJournalPersistRetryAttempts) {
            break;
        }
        std::this_thread::sleep_for(kJournalPersistRetryDelay);
    }
    if (!recovery.ok()) return recovery;
    if (recovery.disposition !=
        WindowsBootstrapRecoveryDisposition::CleanupCommitted) {
        recovery.filesystem = result(
            WindowsBootstrapFilesystemStatus::InvalidIntentOrder);
        recovery.disposition = WindowsBootstrapRecoveryDisposition::Quarantine;
        recovery.intents = {{BootstrapIntentKind::QuarantinePackage, {}, 0,
                             "bootstrap-terminal-cleanup-not-committed"}};
    }
    return recovery;
}

WindowsBootstrapFilesystemRecoveryResult
WindowsBootstrapFilesystemPort::BeginRecoveryInternal(
    const ValidatedInstallPlan& plan,
    const ReplacementJournal* expectedJournal,
    const WindowsBootstrapFilesystemRoots& roots,
    const BootstrapProcessIdentity* terminalParentProcess) {
    WindowsBootstrapFilesystemRecoveryResult recovery;
    const auto fail = [this, &recovery](
                           const WindowsBootstrapFilesystemStatus status,
                           const WindowsFilesystemBackendResult backendResult = {})
        -> WindowsBootstrapFilesystemRecoveryResult {
        if (state_ != nullptr) {
            state_->begun = true;
            state_->blocked = true;
        }
        recovery.filesystem = result(status, backendResult);
        recovery.disposition = WindowsBootstrapRecoveryDisposition::Quarantine;
        recovery.intents = {{BootstrapIntentKind::QuarantinePackage, {}, 0,
                             "bootstrap-filesystem-recovery-blocked"}};
        return recovery;
    };

    if (state_ != nullptr && state_->begun) {
        return fail(WindowsBootstrapFilesystemStatus::InvalidIntentOrder);
    }
    state_ = std::make_unique<State>(backend_);
    State& state = *state_;
    if (!plan.isValidated()) {
        return fail(WindowsBootstrapFilesystemStatus::InvalidPlan);
    }
    if (expectedJournal != nullptr && !expectedJournal->isValidatedFor(plan)) {
        return fail(WindowsBootstrapFilesystemStatus::InvalidJournal);
    }
    if (roots.stagingRoot.empty() || roots.transactionRoot.empty()) {
        return fail(WindowsBootstrapFilesystemStatus::InvalidRoots);
    }
    if (!rootsMatchSignedInvocation(plan, roots)) {
        return fail(WindowsBootstrapFilesystemStatus::InvalidRoots);
    }
    const WindowsFilesystemBackendResult trustedRoots =
        backend_.ValidateTrustedWriteRoots(plan, roots);
    if (!trustedRoots.ok()) {
        return fail(WindowsBootstrapFilesystemStatus::InvalidRoots,
                    trustedRoots);
    }
    state.plan = plan;
    if (expectedJournal != nullptr) state.journal = *expectedJournal;
    state.updaterOwnedBase = plan.invocation.updaterOwnedBase;
    state.invocationIdentity = plan.invocation.invocationIdentity;

    const auto openRoot = [&state](const std::string& path,
                                   const WindowsFilesystemRootRole role,
                                   WindowsFilesystemHandle& handle,
                                   WindowsFileIdentity& identity) {
        const WindowsFilesystemBackendResult opened =
            state.backend.OpenRootExact(path, role, handle, identity);
        if (!opened.ok()) {
            return result(observationFailure(opened), opened);
        }
        if (handle == 0 || !identity.valid) {
            return result(WindowsBootstrapFilesystemStatus::RootIdentityMismatch);
        }
        return WindowsBootstrapFilesystemResult{};
    };
    WindowsBootstrapFilesystemResult opened = openRoot(
        plan.identity.targetRoot, WindowsFilesystemRootRole::Install,
        state.installRoot, state.installRootIdentity);
    if (!opened.ok()) return fail(opened.status,
                                  {opened.backendStatus, opened.nativeError});
    if (!sameDeploymentObject(plan.identity.targetRootIdentity,
                              state.installRootIdentity)) {
        return fail(WindowsBootstrapFilesystemStatus::RootIdentityMismatch);
    }
    opened = openRoot(roots.stagingRoot, WindowsFilesystemRootRole::Staging,
                      state.stagingRoot, state.stagingRootIdentity);
    if (!opened.ok()) return fail(opened.status,
                                  {opened.backendStatus, opened.nativeError});
    opened = openRoot(roots.transactionRoot,
                      WindowsFilesystemRootRole::Transaction,
                      state.transactionRoot, state.transactionRootIdentity);
    if (!opened.ok()) return fail(opened.status,
                                  {opened.backendStatus, opened.nativeError});
    if (sameObject(state.installRootIdentity, state.stagingRootIdentity) ||
        sameObject(state.installRootIdentity, state.transactionRootIdentity) ||
        sameObject(state.stagingRootIdentity, state.transactionRootIdentity)) {
        return fail(WindowsBootstrapFilesystemStatus::RootIdentityMismatch);
    }
    if (state.installRootIdentity.volumeSerial !=
            state.stagingRootIdentity.volumeSerial ||
        state.installRootIdentity.volumeSerial !=
            state.transactionRootIdentity.volumeSerial) {
        return fail(WindowsBootstrapFilesystemStatus::RootVolumeMismatch);
    }
    if (rootsOverlap(plan.identity.targetRoot, roots.stagingRoot) ||
        rootsOverlap(plan.identity.targetRoot, roots.transactionRoot) ||
        rootsOverlap(roots.stagingRoot, roots.transactionRoot)) {
        return fail(WindowsBootstrapFilesystemStatus::RootOverlapRejected);
    }

    std::vector<WindowsBootstrapJournalCandidate> candidates;
    WindowsFilesystemBackendResult read =
        backend_.ReadJournalCandidatesNoFollow(state.transactionRoot,
                                               candidates);
    if (!read.ok()) {
        // Terminal cleanup may encounter a short-lived sharing/access denial
        // while another process closes the journal.  At this point no durable
        // mutation has happened, so expose a typed retry result only to the
        // terminal successor.  Trusted-root and identity failures remain
        // fail-closed and are never widened into retries.
        if (terminalParentProcess != nullptr &&
            read.status == WindowsFilesystemBackendStatus::AccessDenied) {
            recovery.filesystem = result(
                WindowsBootstrapFilesystemStatus::RetryableAccessDenied, read);
            recovery.disposition = WindowsBootstrapRecoveryDisposition::Quarantine;
            recovery.intents = {{BootstrapIntentKind::QuarantinePackage, {}, 0,
                                 "bootstrap-filesystem-recovery-retryable"}};
            return recovery;
        }
        return fail(observationFailure(read), read);
    }
    const WindowsBootstrapJournalRootIdentities expectedRoots{
        state.installRootIdentity, state.stagingRootIdentity,
        state.transactionRootIdentity, {}};
    const WindowsBootstrapJournalSelection selected =
        SelectDurableWindowsBootstrapJournal(
            candidates, &plan, expectedJournal, &expectedRoots,
            expectedJournal == nullptr,
            packageAuthority_.has_value() ? &*packageAuthority_ : nullptr);
    if (!selected.ok()) {
        return fail(selected.status ==
                            WindowsBootstrapJournalSelectionStatus::NoCandidate
                        ? WindowsBootstrapFilesystemStatus::JournalCollision
                        : WindowsBootstrapFilesystemStatus::PartialDurableState);
    }
    if (selected.record->updaterOwnedBase !=
            plan.invocation.updaterOwnedBase ||
        selected.record->invocationIdentity !=
            plan.invocation.invocationIdentity) {
        return fail(WindowsBootstrapFilesystemStatus::RootIdentityMismatch);
    }
    auto selectedCandidate = std::find_if(
        candidates.begin(), candidates.end(),
        [&selected](const WindowsBootstrapJournalCandidate& candidate) {
            return candidate.name == selected.selectedName;
        });
    if (selectedCandidate == candidates.end()) {
        return fail(WindowsBootstrapFilesystemStatus::PartialDurableState);
    }
    if (selectedCandidate->temporary) {
        const std::string targetName = selectedCandidate->name ==
                "aris-bootstrap.journal.a.tmp"
            ? "aris-bootstrap.journal.a"
            : "aris-bootstrap.journal.b";
        const auto replaced = std::find_if(
            candidates.begin(), candidates.end(),
            [&targetName](const WindowsBootstrapJournalCandidate& candidate) {
                return candidate.name == targetName;
            });
        if (replaced != candidates.end()) {
            selectedCandidate->replacedSlotIdentity = replaced->identity;
            selectedCandidate->replacedSlotContent = replaced->content;
        }
    }
    const WindowsBootstrapJournalRecord& durable = *selected.record;
    if (terminalParentProcess != nullptr &&
        !terminalParentMatchesRuntimePair(*terminalParentProcess, durable,
                                           roots.transactionRoot)) {
        return fail(WindowsBootstrapFilesystemStatus::IdentityMismatch);
    }
    const bool parentProcessRebound =
        expectedJournal == nullptr &&
        !(durable.parentProcessIdentity == plan.invocation.parentProcess);
    if (durable.phase == "quarantined") {
        // A prior two-failure decision is itself a durable terminal barrier.
        // Do not reopen or mutate any install/backup object until an explicit
        // operator recovery path supplies a new authenticated transaction.
        return fail(WindowsBootstrapFilesystemStatus::TransactionBlocked);
    }
    if (expectedJournal == nullptr) {
        std::vector<ReplacementJournalItem> durableItems;
        durableItems.reserve(durable.items.size());
        for (const WindowsBootstrapJournalItemRecord& item : durable.items) {
            durableItems.push_back(
                 {item.relativePath,
                  item.classification,
                  item.classification == JournalItemClass::ReplaceExisting ||
                      item.classification == JournalItemClass::RemoveExisting,
                  item.backupComplete || item.removalAbsentVerified,
                  item.replacementComplete});
        }
        const JournalBuildResult rehydrated = RehydrateReplacementJournal(
            plan, durableItems, durable.transactionId);
        if (!rehydrated.ok()) {
            return fail(WindowsBootstrapFilesystemStatus::InvalidJournal);
        }
        state.journal = *rehydrated.journal;
    }
    recovery.journal = state.journal;
    // Validate the complete adapter-owned backup namespace before promoting a
    // journal candidate or rebinding any rollback object.  The journal is the
    // authority for the exact leaves that may exist; an unknown child or a
    // crash-left temporary makes recovery ambiguous and therefore quarantines
    // without namespace mutation.
    std::vector<std::string> expectedBackupEntries;
    // A directory removal is published before its terminal journal
    // checkpoint.  If the process dies in that interval, a missing backup
    // directory is the only deterministic durable outcome once commit or
    // rollback is already recorded.  Treat it as an in-flight cleanup cut;
    // an existing directory is still validated against the journal identity
    // below and cannot be silently adopted.
    const bool backupDirectoryRemovalInFlight =
        durable.backupDirectoryRemoveStarted &&
        (durable.commitComplete || durable.rollbackComplete) &&
        !durable.backupDirectoryRemoved;
    bool expectBackupDirectory = durable.backupDirectoryComplete &&
                                   !durable.backupDirectoryRemoved &&
                                   !backupDirectoryRemovalInFlight;
    std::vector<std::string> allowedMissingBackupEntries;
    if (!durable.backupDirectoryRemoved) {
        for (const auto& item : durable.items) {
            if (item.classification != JournalItemClass::ReplaceExisting &&
                item.classification != JournalItemClass::RemoveExisting) {
                continue;
            }
            if (item.backupStarted || item.backupComplete) {
                expectBackupDirectory = true;
            }
            if (item.backupComplete && !item.removeBackupComplete) {
                expectedBackupEntries.push_back(item.backupRelativePath);
                if (item.removeBackupStarted) {
                    allowedMissingBackupEntries.push_back(
                        item.backupRelativePath);
                }
            }
        }
    }
    const WindowsFilesystemBackendResult backupNamespace =
        backend_.ValidateBackupNamespaceNoFollow(
            state.transactionRoot, expectedBackupEntries,
            allowedMissingBackupEntries,
            expectBackupDirectory,
            durable.backupDirectoryRemoved ? WindowsFileIdentity{}
                                             : durable.backupRootIdentity);
    if (!backupNamespace.ok()) {
        return fail(WindowsBootstrapFilesystemStatus::PartialDurableState,
                    backupNamespace);
    }

    // The first namespace scan is only an observation.  Re-read the complete
    // backup contract immediately before any recovery promotion so a child
    // inserted after the initial scan cannot be carried into a new durable
    // generation.  The later handle bind performs its own identity check as
    // well; this early recheck keeps journal promotion behind the namespace
    // admission boundary.
    const WindowsFilesystemBackendResult backupNamespaceRechecked =
        backend_.ValidateBackupNamespaceNoFollow(
            state.transactionRoot, expectedBackupEntries,
            allowedMissingBackupEntries, expectBackupDirectory,
            durable.backupDirectoryRemoved ? WindowsFileIdentity{}
                                             : durable.backupRootIdentity);
    if (!backupNamespaceRechecked.ok()) {
        return fail(WindowsBootstrapFilesystemStatus::PartialDurableState,
                    backupNamespaceRechecked);
    }

    // Keep the selected candidate identity/content as read-only authority
    // until every install/backup/temp object has been rebound. Promotion of a
    // temporary slot is the first namespace mutation in recovery.
    state.journalIdentity = selectedCandidate->identity;
    state.journalContent = selectedCandidate->content;
    state.journalGeneration = durable.generation;
    state.phase = durable.phase;
    // The selected generation's existence proves the adapter journal itself
    // is durable even when the serialized pre-publication flag was false.
    state.initialJournalDurable = true;
    state.backupDirectoryStarted = durable.backupDirectoryStarted;
    state.backupDirectoryComplete = durable.backupDirectoryComplete;
    state.backupRootIdentity = durable.backupRootIdentity;
    state.commitComplete = durable.commitComplete;
    state.rollbackDurable = durable.rollbackDurable;
    state.rollbackComplete = durable.rollbackComplete;
    state.healthTokenConsumed = durable.healthTokenConsumed;
    state.committedApplicationRelaunchPending =
        durable.committedApplicationRelaunchPending;
    state.journalSchemaVersion = durable.schemaVersion;
    state.previousFailureCount = durable.previousFailureCount;
    state.failureRecordedForTransaction =
        durable.failureRecordedForTransaction;
    state.durableParentProcessIdentity = durable.parentProcessIdentity;
    state.updatedChildIdentity = durable.updatedChildIdentity;
    state.previousApplicationIdentity =
        durable.previousApplicationIdentity;
    state.healthPhase = durable.healthPhase;
    state.previousApplicationRelaunchStarted =
        durable.previousApplicationRelaunchStarted;
    state.previousApplicationRelaunchComplete =
        durable.previousApplicationRelaunchComplete;
    state.backupDirectoryRemoveStarted =
        durable.backupDirectoryRemoveStarted;
    state.backupDirectoryRemoved = durable.backupDirectoryRemoved;
    state.invocationCleanupStarted = durable.invocationCleanupStarted;
    state.invocationCleanupComplete = durable.invocationCleanupComplete;
    state.runtimePairExpected = durable.runtimePairExpected;
    state.runtimePairDirectoryIdentity =
        durable.runtimePairDirectoryIdentity;
    state.runtimePairBootstrapIdentity =
        durable.runtimePairBootstrapIdentity;
    state.runtimePairBootstrapContent = durable.runtimePairBootstrapContent;
    state.items.reserve(durable.items.size());
    for (const WindowsBootstrapJournalItemRecord& source : durable.items) {
        ItemState item;
        item.relativePath = source.relativePath;
        item.backupRelativePath = source.backupRelativePath;
        item.forwardTemporaryLeaf = source.forwardTemporaryLeaf;
        item.restoreTemporaryLeaf = source.restoreTemporaryLeaf;
        item.classification = source.classification;
        item.stagingIdentity = source.stagingIdentity;
        item.installParentIdentity = source.installParentIdentity;
        item.originalIdentity = source.originalIdentity;
        item.backupIdentity = source.backupIdentity;
        item.installedIdentity = source.installedIdentity;
        item.restoredIdentity = source.restoredIdentity;
        item.preparedIdentity = source.preparedIdentity;
        item.restorePreparedIdentity = source.restorePreparedIdentity;
        item.stagingContent = source.stagingContent;
        item.backupContent = source.backupContent;
        item.preparedContent = source.preparedContent;
        item.restorePreparedContent = source.restorePreparedContent;
        item.rollbackMetadata = source.rollbackMetadata;
        item.installedMetadata = source.installedMetadata;
        item.restoredMetadata = source.restoredMetadata;
        item.preparedMetadata = source.preparedMetadata;
        item.restorePreparedMetadata = source.restorePreparedMetadata;
        item.removalAbsentVerified = source.removalAbsentVerified;
        item.backupStarted = source.backupStarted;
        item.backupComplete = source.backupComplete;
        item.replacementStarted = source.replacementStarted;
        item.replacementPrepared = source.replacementPrepared;
        item.replacementComplete = source.replacementComplete;
        item.restoreStarted = source.restoreStarted;
        item.restorePrepared = source.restorePrepared;
        item.restoreComplete = source.restoreComplete;
        item.removeNewStarted = source.removeNewStarted;
        item.removeNewComplete = source.removeNewComplete;
        item.removeBackupStarted = source.removeBackupStarted;
        item.removeBackupComplete = source.removeBackupComplete;
        state.items.push_back(std::move(item));
    }
    for (const ItemState& item : state.items) {
        state.bindStagingCleanupFile(item.relativePath,
                                     item.stagingIdentity,
                                     item.stagingContent);
    }
    const WindowsBootstrapFilesystemResult runtimePairBound =
        state.bindDurableTransactionRuntimePair(durable);
    if (!runtimePairBound.ok()) {
        return fail(runtimePairBound.status,
                    {runtimePairBound.backendStatus,
                     runtimePairBound.nativeError});
    }
    state.begun = true;

    bool recoveredProgress = false;
    if (state.backupDirectoryRemoved || state.backupDirectoryStarted ||
        state.backupDirectoryComplete) {
        WindowsFilesystemObservation backupDirectoryObservation;
        const WindowsFilesystemBackendResult observed =
            backend_.ObserveNoFollow(state.transactionRoot, kBackupDirectory,
                                     backupDirectoryObservation);
        if (!observed.ok() ||
            backupDirectoryObservation.kind ==
                WindowsFilesystemEntryKind::ReparsePoint) {
            return fail(observed.ok()
                            ? WindowsBootstrapFilesystemStatus::ReparsePointRejected
                            : observationFailure(observed),
                        observed);
        }
        if (state.backupDirectoryRemoved &&
            backupDirectoryObservation.kind !=
                WindowsFilesystemEntryKind::Missing) {
            return fail(WindowsBootstrapFilesystemStatus::PartialDurableState);
        } else if (state.backupDirectoryStarted &&
                   !state.backupDirectoryComplete) {
            // The start checkpoint is not bound to a directory identity.  A
            // fresh process cannot distinguish an updater-created directory
            // from a caller-forged same-name object, so never adopt it.
            return fail(WindowsBootstrapFilesystemStatus::PartialDurableState);
        } else if (state.backupDirectoryRemoveStarted &&
            backupDirectoryObservation.kind ==
                WindowsFilesystemEntryKind::Missing &&
            (state.commitComplete || state.rollbackComplete)) {
            state.backupDirectoryRemoved = true;
            recoveredProgress = true;
        } else if (state.backupDirectoryComplete &&
                   backupDirectoryObservation.kind !=
                       WindowsFilesystemEntryKind::Directory) {
            return fail(WindowsBootstrapFilesystemStatus::PartialDurableState);
        }
    }
    bool incompleteForward = false;
    for (ItemState& item : state.items) {
        WindowsFilesystemObservation target;
        const WindowsFilesystemBackendResult targetObserved =
            backend_.ObserveNoFollow(state.installRoot, item.relativePath,
                                     target);
        if (!targetObserved.ok() ||
            target.kind == WindowsFilesystemEntryKind::ReparsePoint) {
            return fail(targetObserved.ok()
                            ? WindowsBootstrapFilesystemStatus::ReparsePointRejected
                            : observationFailure(targetObserved),
                        targetObserved);
        }

        if (item.classification == JournalItemClass::PreserveExisting) {
            if (target.kind != WindowsFilesystemEntryKind::RegularFile ||
                target.identity != item.originalIdentity) {
                return fail(WindowsBootstrapFilesystemStatus::IdentityMismatch);
            }
            continue;
        }

        WindowsFilesystemObservation forwardTemporary;
        const WindowsFilesystemBackendResult temporaryObserved =
            backend_.ObserveNoFollow(
                state.installRoot,
                siblingPath(item.relativePath, item.forwardTemporaryLeaf),
                forwardTemporary);
        if (!temporaryObserved.ok() ||
            forwardTemporary.kind == WindowsFilesystemEntryKind::ReparsePoint) {
            return fail(
                temporaryObserved.ok()
                    ? WindowsBootstrapFilesystemStatus::ReparsePointRejected
                    : observationFailure(temporaryObserved),
                temporaryObserved);
        }

        // Both adapter-owned temporary leaves are part of the transaction's
        // namespace.  Observe the restore leaf even before rollback starts so
        // a stray/reparse object cannot be hidden behind an incomplete
        // forward publication and survive a supposedly terminal recovery.
        WindowsFilesystemObservation restoreTemporary;
        const WindowsFilesystemBackendResult restoreTemporaryObserved =
            backend_.ObserveNoFollow(
                state.installRoot,
                siblingPath(item.relativePath, item.restoreTemporaryLeaf),
                restoreTemporary);
        if (!restoreTemporaryObserved.ok() ||
            restoreTemporary.kind == WindowsFilesystemEntryKind::ReparsePoint) {
            return fail(
                restoreTemporaryObserved.ok()
                    ? WindowsBootstrapFilesystemStatus::ReparsePointRejected
                    : observationFailure(restoreTemporaryObserved),
                restoreTemporaryObserved);
        }

        if (item.backupComplete && !state.backupDirectoryRemoved) {
            const WindowsFilesystemHandle backupObservationRoot =
                state.backupRoot != 0 ? state.backupRoot : state.transactionRoot;
            const std::string backupObservationPath =
                state.backupRoot != 0
                    ? item.backupRelativePath
                    : std::string{kBackupDirectory} + "/" +
                          item.backupRelativePath;
            WindowsFilesystemObservation backup;
            const WindowsFilesystemBackendResult backupObserved =
                backend_.ObserveNoFollow(backupObservationRoot,
                                         backupObservationPath, backup);
            if (item.removeBackupStarted && !item.removeBackupComplete &&
                backup.kind == WindowsFilesystemEntryKind::Missing) {
                item.removeBackupComplete = true;
                recoveredProgress = true;
            }
            const bool expectedRemoved = item.removeBackupComplete;
            const bool matchesExpected = expectedRemoved
                ? backup.kind == WindowsFilesystemEntryKind::Missing
                : backup.kind == WindowsFilesystemEntryKind::RegularFile &&
                      backup.identity == item.backupIdentity &&
                      backup.content == item.backupContent;
            if (!backupObserved.ok() || !matchesExpected) {
                return fail(WindowsBootstrapFilesystemStatus::PartialDurableState,
                            backupObserved);
            }
        } else if (item.backupStarted && !state.backupDirectoryRemoved) {
            const WindowsFilesystemHandle backupObservationRoot =
                state.backupRoot != 0 ? state.backupRoot : state.transactionRoot;
            const std::string backupObservationPath =
                state.backupRoot != 0
                    ? item.backupRelativePath
                    : std::string{kBackupDirectory} + "/" +
                          item.backupRelativePath;
            WindowsFilesystemObservation backup;
            const WindowsFilesystemBackendResult backupObserved =
                backend_.ObserveNoFollow(backupObservationRoot,
                                         backupObservationPath, backup);
            if (!backupObserved.ok()) {
                return fail(observationFailure(backupObserved), backupObserved);
            }
            if (backup.kind != WindowsFilesystemEntryKind::Missing) {
                return fail(WindowsBootstrapFilesystemStatus::PartialDurableState);
            }
        }

        if (!item.replacementComplete &&
            restoreTemporary.kind != WindowsFilesystemEntryKind::Missing) {
            return fail(WindowsBootstrapFilesystemStatus::PartialDurableState);
        }

        if (item.replacementComplete) {
            const bool installedTargetMatches =
                target.kind == WindowsFilesystemEntryKind::RegularFile &&
                target.identity == item.installedIdentity &&
                target.content == item.stagingContent &&
                state.metadataMatches(target.metadata, item.installedMetadata);
            if ((item.classification == JournalItemClass::ReplaceExisting ||
                 item.classification == JournalItemClass::RemoveExisting) &&
                item.restoreStarted && !item.restoreComplete) {
                const bool restoredPublicationMatches =
                    item.restorePrepared &&
                    target.kind == WindowsFilesystemEntryKind::RegularFile &&
                    target.identity == item.restorePreparedIdentity &&
                    target.content == item.restorePreparedContent &&
                    state.metadataMatches(target.metadata,
                                          item.restorePreparedMetadata);
                if (restoredPublicationMatches) {
                    if (restoreTemporary.kind !=
                        WindowsFilesystemEntryKind::Missing) {
                        return fail(
                            WindowsBootstrapFilesystemStatus::PartialDurableState);
                    }
                    item.restoredIdentity = target.identity;
                    // Preserve the journal-bound rollback metadata as the
                    // durable value.  A native post-rename observation may
                    // differ in volatile fields (notably last-access), even
                    // though metadataMatches() has proved the target
                    // equivalent for recovery.  Re-serializing the observed
                    // bytes would violate the strict journal invariant that
                    // restore metadata is exactly the captured rollback
                    // authority.
                    item.restoredMetadata = item.restorePreparedMetadata;
                    item.restoreComplete = true;
                    recoveredProgress = true;
                } else if (item.restorePrepared) {
                    const bool restoreTemporaryMatches =
                        restoreTemporary.kind ==
                            WindowsFilesystemEntryKind::RegularFile &&
                        restoreTemporary.identity ==
                            item.restorePreparedIdentity &&
                        restoreTemporary.content ==
                            item.restorePreparedContent &&
                        state.metadataMatches(
                            restoreTemporary.metadata,
                            item.restorePreparedMetadata);
                    const bool originalMutationMatches =
                        item.classification == JournalItemClass::RemoveExisting
                            ? target.kind == WindowsFilesystemEntryKind::Missing
                            : installedTargetMatches;
                    if (!originalMutationMatches ||
                        !restoreTemporaryMatches) {
                        return fail(
                            WindowsBootstrapFilesystemStatus::PartialDurableState);
                    }
                } else if ((item.classification ==
                                JournalItemClass::RemoveExisting
                                ? target.kind != WindowsFilesystemEntryKind::Missing
                                : !installedTargetMatches) ||
                           restoreTemporary.kind !=
                               WindowsFilesystemEntryKind::Missing) {
                    return fail(
                        WindowsBootstrapFilesystemStatus::PartialDurableState);
                }
            } else if (item.classification ==
                           JournalItemClass::CreateNew &&
                       item.removeNewStarted && !item.removeNewComplete &&
                       target.kind == WindowsFilesystemEntryKind::Missing) {
                if (restoreTemporary.kind !=
                    WindowsFilesystemEntryKind::Missing) {
                    return fail(
                        WindowsBootstrapFilesystemStatus::PartialDurableState);
                }
                item.removeNewComplete = true;
                recoveredProgress = true;
            } else if (restoreTemporary.kind !=
                       WindowsFilesystemEntryKind::Missing) {
                return fail(
                    WindowsBootstrapFilesystemStatus::PartialDurableState);
            }

            const bool restored =
                (item.classification == JournalItemClass::ReplaceExisting ||
                 item.classification == JournalItemClass::RemoveExisting) &&
                item.restoreComplete;
            const bool removedNew =
                item.classification == JournalItemClass::CreateNew &&
                item.removeNewComplete;
            const bool removedExisting =
                item.classification == JournalItemClass::RemoveExisting &&
                !restored;
            const bool targetMatches = removedNew || removedExisting
                ? target.kind == WindowsFilesystemEntryKind::Missing
                : target.kind == WindowsFilesystemEntryKind::RegularFile &&
                      (restored
                           ? target.identity == item.restoredIdentity &&
                                 target.content == item.backupContent &&
                                 state.metadataMatches(target.metadata,
                                                        item.restoredMetadata) &&
                                 state.metadataMatches(item.restoredMetadata,
                                                        item.rollbackMetadata)
                           : target.identity == item.installedIdentity &&
                                 target.content == item.stagingContent &&
                                 state.metadataMatches(target.metadata,
                                                        item.installedMetadata));
            if (!targetMatches) {
                return fail(WindowsBootstrapFilesystemStatus::PartialDurableState);
            }
            if (forwardTemporary.kind != WindowsFilesystemEntryKind::Missing) {
                return fail(WindowsBootstrapFilesystemStatus::PartialDurableState);
            }
            continue;
        }

        if (item.replacementStarted) {
            if (item.classification == JournalItemClass::RemoveExisting) {
                if (forwardTemporary.kind !=
                    WindowsFilesystemEntryKind::Missing) {
                    return fail(
                        WindowsBootstrapFilesystemStatus::PartialDurableState);
                }
                if (target.kind == WindowsFilesystemEntryKind::Missing) {
                    item.replacementComplete = true;
                    recoveredProgress = true;
                    continue;
                }
                if (target.kind != WindowsFilesystemEntryKind::RegularFile ||
                    target.identity != item.originalIdentity ||
                    target.content != item.backupContent ||
                    !state.metadataMatches(target.metadata,
                                           item.rollbackMetadata)) {
                    return fail(
                        WindowsBootstrapFilesystemStatus::PartialDurableState);
                }
                incompleteForward = true;
                continue;
            }
            const bool published =
                item.replacementPrepared &&
                target.kind == WindowsFilesystemEntryKind::RegularFile &&
                target.identity == item.preparedIdentity &&
                target.content == item.preparedContent &&
                state.metadataMatches(target.metadata, item.preparedMetadata);
            if (published) {
                if (forwardTemporary.kind !=
                    WindowsFilesystemEntryKind::Missing) {
                    return fail(
                        WindowsBootstrapFilesystemStatus::PartialDurableState);
                }
                item.installedIdentity = target.identity;
                // Keep the journal-bound destination metadata as the durable
                // value.  Native observation may normalize volatile fields
                // such as last-access time after publication; the backend
                // comparison above already proved the target equivalent.
                item.installedMetadata = item.preparedMetadata;
                item.replacementComplete = true;
                recoveredProgress = true;
                continue;
            }
            const bool targetUnchanged =
                item.classification == JournalItemClass::CreateNew
                    ? target.kind == WindowsFilesystemEntryKind::Missing
                    : target.kind == WindowsFilesystemEntryKind::RegularFile &&
                          target.identity == item.originalIdentity &&
                          target.content == item.backupContent &&
                          state.metadataMatches(target.metadata,
                                                 item.rollbackMetadata);
            if (!targetUnchanged) {
                return fail(
                    WindowsBootstrapFilesystemStatus::PartialDurableState);
            }
            if (item.replacementPrepared) {
                const bool preparedTemporaryMatches =
                    forwardTemporary.kind ==
                        WindowsFilesystemEntryKind::RegularFile &&
                    forwardTemporary.identity == item.preparedIdentity &&
                    forwardTemporary.content == item.preparedContent &&
                    state.metadataMatches(forwardTemporary.metadata,
                                          item.preparedMetadata);
                if (!preparedTemporaryMatches) {
                    return fail(
                        WindowsBootstrapFilesystemStatus::PartialDurableState);
                }
            } else if (forwardTemporary.kind !=
                       WindowsFilesystemEntryKind::Missing) {
                return fail(
                    WindowsBootstrapFilesystemStatus::PartialDurableState);
            }
        } else if (forwardTemporary.kind !=
                   WindowsFilesystemEntryKind::Missing) {
            return fail(WindowsBootstrapFilesystemStatus::PartialDurableState);
        }

        if (state.rollbackDurable && item.replacementStarted &&
            !item.replacementComplete) {
            return fail(WindowsBootstrapFilesystemStatus::PartialDurableState);
        }

        if (item.classification == JournalItemClass::CreateNew) {
            if (target.kind != WindowsFilesystemEntryKind::Missing) {
                return fail(WindowsBootstrapFilesystemStatus::UnknownTargetItem);
            }
        } else if (target.kind != WindowsFilesystemEntryKind::RegularFile ||
                   target.identity != item.originalIdentity ||
                   target.content != item.backupContent ||
                   !state.metadataMatches(target.metadata,
                                          item.rollbackMetadata)) {
            return fail(WindowsBootstrapFilesystemStatus::PartialDurableState);
        }
        incompleteForward = true;
    }

    WindowsFileIdentity promotedIdentity;
    WindowsFileContentFingerprint promotedContent;
    const WindowsFilesystemBackendResult promoted =
        backend_.PromoteJournalCandidateDurable(
            state.transactionRoot, *selectedCandidate,
            promotedIdentity, promotedContent);
    if (!promoted.ok() || !promotedIdentity.valid ||
        !promotedContent.valid) {
        return fail(WindowsBootstrapFilesystemStatus::PartialDurableState,
                    promoted);
    }
    state.journalIdentity = promotedIdentity;
    state.journalContent = promotedContent;

    // A restarted updater may have a new PID/creation tuple. The selector has
    // already proved that its image path, volume and FileId match the durable
    // parent. Publish the new tuple only after the selected durable generation
    // is the active journal authority, so the new generation always names the
    // true predecessor and later leases cannot regress to the old candidate.
    if (parentProcessRebound) {
        state.durableParentProcessIdentity = plan.invocation.parentProcess;
        const WindowsBootstrapFilesystemResult rebound =
            state.persist("parent-rebound");
        if (!rebound.ok()) {
            return fail(WindowsBootstrapFilesystemStatus::PartialDurableState,
                        {rebound.backendStatus, rebound.nativeError});
        }
    }

    const bool backupDirectoryRemovalInProgress =
        state.backupDirectoryRemoveStarted &&
        (state.commitComplete || state.rollbackComplete) &&
        !state.backupDirectoryRemoved;
    if (!state.backupDirectoryRemoved &&
        (state.backupDirectoryStarted || state.backupDirectoryComplete) &&
        !backupDirectoryRemovalInProgress) {
        const State::LeasedMutationResult rebound =
            state.mutateWithJournalLease([&state] {
                return state.backend.OpenOrCreateDirectoryNoFollow(
                    state.transactionRoot, kBackupDirectory, false,
                    state.backupRoot, state.backupRootIdentity);
            });
        if (!rebound.mutationInvoked || !rebound.backendResult.ok() ||
            state.backupRoot == 0 || !state.backupRootIdentity.valid ||
            (durable.backupRootIdentity.valid &&
             state.backupRootIdentity != durable.backupRootIdentity)) {
            return fail(WindowsBootstrapFilesystemStatus::PartialDurableState,
                        rebound.backendResult);
        }
        const WindowsFilesystemBackendResult boundNamespace =
            state.backend.ValidateBackupNamespaceBoundNoFollow(
                state.backupRoot, expectedBackupEntries,
                allowedMissingBackupEntries);
        if (!boundNamespace.ok()) {
            return fail(WindowsBootstrapFilesystemStatus::PartialDurableState,
                        boundNamespace);
        }
        if (!state.backupDirectoryComplete) {
            state.backupDirectoryComplete = true;
            recoveredProgress = true;
        }
    }

    if (state.rollbackDurable) {
        const bool wasRollbackComplete = state.rollbackComplete;
        state.updateRollbackComplete();
        if (!wasRollbackComplete && state.rollbackComplete) {
            recoveredProgress = true;
        }
    }
    if (recoveredProgress) {
        const WindowsBootstrapFilesystemResult persisted =
            state.persist("restart-rebound-progress");
        if (!persisted.ok()) {
            return fail(persisted.status,
                        {persisted.backendStatus, persisted.nativeError});
        }
    }
    recovery.filesystem = {};
    recovery.generation = state.journalGeneration;
    if (state.commitComplete) {
        recovery.disposition =
            WindowsBootstrapRecoveryDisposition::CleanupCommitted;
        if (!state.backupDirectoryRemoved) {
            recovery.intents.push_back(
                {BootstrapIntentKind::RemoveBackup, {}, 0,
                 "restart-committed-cleanup"});
        }
        return recovery;
    }
    if (state.backupDirectoryRemoved && state.rollbackComplete) {
        recovery.disposition = WindowsBootstrapRecoveryDisposition::Complete;
        return recovery;
    }
    if (state.rollbackDurable ||
        (!incompleteForward && state.allReplacementsComplete())) {
        recovery.disposition = WindowsBootstrapRecoveryDisposition::Rollback;
        if (!state.rollbackDurable) {
            recovery.intents.push_back(
                {BootstrapIntentKind::PersistJournal, {}, 0,
                 "rolling-back"});
        }
        for (const ItemState& item : state.items) {
            if (!item.replacementComplete) continue;
            if ((item.classification == JournalItemClass::ReplaceExisting ||
                 item.classification == JournalItemClass::RemoveExisting) &&
                !item.removalAbsentVerified &&
                !item.restoreComplete) {
                recovery.intents.push_back(
                    {BootstrapIntentKind::RestoreBackup, item.relativePath, 0,
                     "restart-rollback"});
            } else if (item.classification == JournalItemClass::CreateNew &&
                       !item.removeNewComplete) {
                recovery.intents.push_back(
                    {BootstrapIntentKind::RemoveNewItem, item.relativePath, 0,
                     "restart-rollback"});
            }
        }
        if (state.rollbackComplete && !state.backupDirectoryRemoved) {
            recovery.intents.push_back(
                {BootstrapIntentKind::RemoveBackup, {}, 0,
                 "restart-rollback-cleanup"});
        }
        return recovery;
    }

    recovery.disposition =
        WindowsBootstrapRecoveryDisposition::ContinueForward;
    if (!state.backupDirectoryComplete ||
        std::any_of(state.items.begin(), state.items.end(), [](const ItemState& item) {
            return item.classification == JournalItemClass::CreateNew &&
                   !item.installParentIdentity.valid;
        })) {
        recovery.intents.push_back(
            {BootstrapIntentKind::PersistJournal, {}, 0, "prepared"});
    }
    for (const ItemState& item : state.items) {
        if ((item.classification == JournalItemClass::ReplaceExisting ||
             item.classification == JournalItemClass::RemoveExisting) &&
            !item.backupComplete) {
            recovery.intents.push_back(
                {BootstrapIntentKind::SnapshotBackup, item.relativePath, 0,
                 "restart-forward"});
        }
    }
    for (const ItemState& item : state.items) {
        if ((item.classification == JournalItemClass::ReplaceExisting ||
             item.classification == JournalItemClass::CreateNew) &&
            !item.replacementComplete) {
            recovery.intents.push_back(
                {BootstrapIntentKind::ReplaceInventoryItem,
                 item.relativePath, 0, "restart-forward"});
        } else if (item.classification == JournalItemClass::RemoveExisting &&
                   !item.replacementComplete) {
            recovery.intents.push_back(
                {BootstrapIntentKind::RemoveInstalledItem,
                 item.relativePath, 0, "restart-forward"});
        }
    }
    return recovery;
}

WindowsBootstrapFilesystemResult WindowsBootstrapFilesystemPort::Apply(
    const BootstrapIntent& intent) {
    State& state = *state_;
    if (!state.begun) {
        return result(WindowsBootstrapFilesystemStatus::InvalidIntentOrder);
    }
    if (state.blocked && intent.kind != BootstrapIntentKind::QuarantinePackage) {
        return result(WindowsBootstrapFilesystemStatus::TransactionBlocked);
    }

    // Failure bookkeeping is itself a durable journal publication.  Keep it
    // in the adapter state so a second failed attempt can quarantine the
    // package without relying on the caller's in-memory reducer.  Replaying
    // either intent is idempotent.
    if (intent.kind == BootstrapIntentKind::RecordFailure) {
        if (state.failureRecordedForTransaction) {
            return {};
        }
        state.previousFailureCount =
            NextFailureDisposition(state.previousFailureCount)
                .recordedFailureCount;
        state.failureRecordedForTransaction = true;
        return state.persist("failure-recorded");
    }
    if (intent.kind == BootstrapIntentKind::QuarantinePackage) {
        if (state.phase == "quarantined") return {};
        const WindowsBootstrapFilesystemResult persisted =
            state.persist("quarantined");
        if (!persisted.ok()) return persisted;
        state.blocked = true;
        return {};
    }

    if (intent.kind == BootstrapIntentKind::PersistJournal) {
        if (!isAllowedJournalReason(intent.reason)) {
            return result(WindowsBootstrapFilesystemStatus::UnsupportedIntent);
        }
        if (state.commitComplete || state.rollbackComplete ||
            (state.rollbackDurable && intent.reason != "rolling-back")) {
            return result(WindowsBootstrapFilesystemStatus::InvalidIntentOrder);
        }
        if (!state.initialJournalDurable && intent.reason != "prepared") {
            return result(WindowsBootstrapFilesystemStatus::InvalidIntentOrder);
        }
        if (!state.initialJournalDurable) {
            WindowsBootstrapFilesystemResult persisted =
                state.persist("prepared");
            if (!persisted.ok()) return persisted;
            state.initialJournalDurable = true;
        }
        const bool preparingBackup = !state.backupDirectoryComplete;
        if (preparingBackup) {
            WindowsBootstrapFilesystemResult persisted;
            const bool requireNewDirectory = true;
            if (!state.backupDirectoryStarted) {
                state.backupDirectoryStarted = true;
                persisted = state.persist("backup-directory-started");
                if (!persisted.ok()) return persisted;
            }
            const State::LeasedMutationResult directoryMutation =
                state.mutateWithJournalLease([&state, requireNewDirectory] {
                    return state.backend.OpenOrCreateDirectoryNoFollow(
                        state.transactionRoot, kBackupDirectory,
                        requireNewDirectory,
                        state.backupRoot, state.backupRootIdentity);
                });
            if (!directoryMutation.mutationInvoked) {
                return state.blockAfterStarted(
                    directoryMutation.backendResult,
                    WindowsBootstrapFilesystemStatus::PartialDurableState,
                    true);
            }
            const WindowsFilesystemBackendResult created =
                directoryMutation.backendResult;
            if (!created.ok() || state.backupRoot == 0 ||
                !state.backupRootIdentity.valid) {
                return state.blockAfterStarted(
                    created, WindowsBootstrapFilesystemStatus::PartialDurableState,
                    true);
            }
            state.backupDirectoryComplete = true;
            persisted = state.persist("backup-directory-complete");
            if (!persisted.ok()) return persisted;
        }
        if (intent.reason == "prepared") {
            for (ItemState& item : state.items) {
                if (item.installParentIdentity.valid) continue;
                if (item.classification != JournalItemClass::CreateNew ||
                    item.replacementStarted || item.replacementComplete) {
                    return result(WindowsBootstrapFilesystemStatus::InvalidJournal);
                }
                WindowsFileIdentity parentIdentity;
                for (const std::string& parent : parentPaths(item.relativePath)) {
                    WindowsFilesystemObservation observation;
                    const auto observedParent = state.backend.ObserveNoFollow(
                        state.installRoot, parent, observation);
                    if (!observedParent.ok()) {
                        return result(observationFailure(observedParent), observedParent);
                    }
                    if (observation.kind == WindowsFilesystemEntryKind::Missing) {
                        WindowsFilesystemHandle directory = 0;
                        const auto mutation = state.mutateWithJournalLease([&] {
                            return state.backend.OpenOrCreateDirectoryNoFollow(
                                state.installRoot, parent, true, directory, parentIdentity);
                        });
                        if (directory != 0) state.backend.CloseFilesystemHandle(directory);
                        if (!mutation.mutationInvoked || !mutation.backendResult.ok() ||
                            !parentIdentity.valid) {
                            return state.blockAfterStarted(mutation.backendResult,
                                WindowsBootstrapFilesystemStatus::PartialDurableState, true);
                        }
                    } else if (observation.kind == WindowsFilesystemEntryKind::ReparsePoint) {
                        return result(WindowsBootstrapFilesystemStatus::ReparsePointRejected);
                    } else if (observation.kind != WindowsFilesystemEntryKind::Directory ||
                               !observation.identity.valid) {
                        return result(WindowsBootstrapFilesystemStatus::UnexpectedEntryType);
                    } else {
                        WindowsFilesystemHandle directory = 0;
                        const auto mutation = state.mutateWithJournalLease([&] {
                            return state.backend.OpenOrCreateDirectoryNoFollow(
                                state.installRoot, parent, false, directory, parentIdentity);
                        });
                        if (directory != 0) state.backend.CloseFilesystemHandle(directory);
                        if (!mutation.mutationInvoked || !mutation.backendResult.ok() ||
                            !parentIdentity.valid) {
                            return state.blockAfterStarted(mutation.backendResult,
                                WindowsBootstrapFilesystemStatus::PartialDurableState, true);
                        }
                        if (parentIdentity != observation.identity) {
                            return result(WindowsBootstrapFilesystemStatus::IdentityMismatch);
                        }
                    }
                }
                if (!parentIdentity.valid) {
                    return result(WindowsBootstrapFilesystemStatus::InvalidJournal);
                }
                // Bind the final parent before file publication. Empty
                // directories are reusable infrastructure, never recursively
                // removed during rollback or assumed to belong to this run.
                item.installParentIdentity = parentIdentity;
                const auto bound = state.persist("install-parent-bound");
                if (!bound.ok()) return bound;
            }
        }
        if (preparingBackup) return {};
        if (intent.reason == "rolling-back") {
            state.rollbackDurable = true;
            state.updateRollbackComplete();
        }
        return state.persist(intent.reason);
    }

    switch (intent.kind) {
        case BootstrapIntentKind::SnapshotBackup:
        case BootstrapIntentKind::ReplaceInventoryItem:
        case BootstrapIntentKind::RemoveInstalledItem:
        case BootstrapIntentKind::CommitJournal:
        case BootstrapIntentKind::RemoveBackup:
        case BootstrapIntentKind::RestoreBackup:
        case BootstrapIntentKind::RemoveNewItem:
            break;
        default:
            return result(WindowsBootstrapFilesystemStatus::UnsupportedIntent);
    }

    if (intent.kind == BootstrapIntentKind::CommitJournal &&
        state.commitComplete) {
        return {};
    }
    if (intent.kind == BootstrapIntentKind::RemoveBackup &&
        state.backupDirectoryRemoved) {
        WindowsFilesystemObservation backupDirectory;
        const WindowsFilesystemBackendResult observed =
            backend_.ObserveNoFollow(state.transactionRoot, kBackupDirectory,
                                     backupDirectory);
        if (!observed.ok()) {
            return state.blockAfterStarted(
                observed,
                WindowsBootstrapFilesystemStatus::PartialDurableState,
                true);
        }
        if (backupDirectory.kind != WindowsFilesystemEntryKind::Missing) {
            state.blocked = true;
            return result(
                backupDirectory.kind == WindowsFilesystemEntryKind::ReparsePoint
                    ? WindowsBootstrapFilesystemStatus::ReparsePointRejected
                    : WindowsBootstrapFilesystemStatus::PartialDurableState);
        }
        return {};
    }

    const bool retryingBackupDirectoryRemovalWithoutHandle =
        intent.kind == BootstrapIntentKind::RemoveBackup &&
        state.backupDirectoryRemoveStarted &&
        std::all_of(state.items.begin(), state.items.end(),
                    [](const ItemState& item) {
                        return !item.backupComplete ||
                               item.removeBackupComplete;
                    });
    if (!state.initialJournalDurable || !state.backupDirectoryComplete ||
        (state.backupRoot == 0 &&
         !retryingBackupDirectoryRemovalWithoutHandle)) {
        return result(WindowsBootstrapFilesystemStatus::JournalNotDurable);
    }

    if ((intent.kind == BootstrapIntentKind::SnapshotBackup ||
         intent.kind == BootstrapIntentKind::ReplaceInventoryItem ||
         intent.kind == BootstrapIntentKind::RemoveInstalledItem) &&
        (state.rollbackDurable || state.commitComplete)) {
        return result(WindowsBootstrapFilesystemStatus::InvalidIntentOrder);
    }

    if (intent.kind == BootstrapIntentKind::CommitJournal) {
        if (!state.allReplacementsComplete() || state.rollbackDurable) {
            return result(WindowsBootstrapFilesystemStatus::InvalidIntentOrder);
        }
        state.commitComplete = true;
        return state.persist("committed");
    }

    if (intent.kind == BootstrapIntentKind::RemoveBackup) {
        if (state.backupDirectoryRemoved) return {};
        if (!state.commitComplete && !state.rollbackComplete) {
            return result(WindowsBootstrapFilesystemStatus::InvalidIntentOrder);
        }
        for (ItemState& item : state.items) {
            if (!item.backupComplete || item.removeBackupComplete) continue;
            item.removeBackupStarted = true;
            WindowsBootstrapFilesystemResult persisted =
                state.persist("backup-cleanup-started");
            if (!persisted.ok()) return persisted;
            const State::LeasedMutationResult deleteMutation =
                state.mutateWithJournalLease([&state, &item] {
                    return state.backend.DeleteFileDurableNoFollow(
                        state.backupRoot, item.backupRelativePath,
                        state.backupRootIdentity, item.backupIdentity,
                        item.backupContent, {});
                });
            if (!deleteMutation.mutationInvoked) {
                return state.blockAfterStarted(
                    deleteMutation.backendResult,
                    WindowsBootstrapFilesystemStatus::PartialDurableState,
                    true);
            }
            const WindowsFilesystemBackendResult removed =
                deleteMutation.backendResult;
            if (!removed.ok()) {
                return state.blockAfterStarted(
                    removed,
                    WindowsBootstrapFilesystemStatus::CleanupPending,
                    true);
            }
            item.removeBackupComplete = true;
            persisted = state.persist("backup-cleanup-complete");
            if (!persisted.ok()) {
                return result(WindowsBootstrapFilesystemStatus::CleanupPending,
                              {persisted.backendStatus, persisted.nativeError});
            }
        }
        state.backupDirectoryRemoveStarted = true;
        WindowsBootstrapFilesystemResult persisted =
            state.persist("backup-directory-remove-started");
        if (!persisted.ok()) return persisted;
        backend_.CloseFilesystemHandle(state.backupRoot);
        state.backupRoot = 0;
        const State::LeasedMutationResult directoryMutation =
            state.mutateWithJournalLease([&state] {
                return state.backend.RemoveEmptyDirectoryDurableNoFollow(
                    state.transactionRoot, kBackupDirectory,
                    state.backupRootIdentity);
            });
        if (!directoryMutation.mutationInvoked) {
            return state.blockAfterStarted(
                directoryMutation.backendResult,
                WindowsBootstrapFilesystemStatus::PartialDurableState,
                true);
        }
        const WindowsFilesystemBackendResult removed =
            directoryMutation.backendResult;
        if (!removed.ok()) {
            return state.blockAfterStarted(
                removed,
                WindowsBootstrapFilesystemStatus::CleanupPending,
                true);
        }
        state.backupDirectoryRemoved = true;
        persisted = state.persist("backup-directory-removed");
        if (!persisted.ok()) {
            return result(WindowsBootstrapFilesystemStatus::CleanupPending,
                          {persisted.backendStatus, persisted.nativeError});
        }
        return {};
    }

    ItemState* item = state.findItem(intent.relativePath);
    if (item == nullptr) {
        return result(WindowsBootstrapFilesystemStatus::InvalidIntentPath);
    }

    if (intent.kind == BootstrapIntentKind::SnapshotBackup) {
        if (item->classification != JournalItemClass::ReplaceExisting &&
            item->classification != JournalItemClass::RemoveExisting) {
            return result(WindowsBootstrapFilesystemStatus::InvalidIntentPath);
        }
        if (item->removalAbsentVerified) return {};
        if (item->backupComplete) return {};
        item->backupStarted = true;
        WindowsBootstrapFilesystemResult persisted =
            state.persist("backup-started");
        if (!persisted.ok()) return persisted;
        const State::LeasedMutationResult copyMutation =
            state.mutateWithJournalLease([&state, item] {
                return state.backend.CopyFileDurableNoFollow(
                    state.installRoot, item->relativePath, item->originalIdentity,
                    state.backupRoot, item->backupRelativePath,
                    item->backupIdentity, item->backupContent,
                    item->rollbackMetadata);
            });
        if (!copyMutation.mutationInvoked) {
            return state.blockAfterStarted(
                copyMutation.backendResult,
                WindowsBootstrapFilesystemStatus::PartialDurableState,
                true);
        }
        const WindowsFilesystemBackendResult copied = copyMutation.backendResult;
        if (!copied.ok() || !item->backupIdentity.valid ||
            !item->backupContent.valid || !item->rollbackMetadata.valid) {
            return state.blockAfterStarted(copied,
                                           copied.status == WindowsFilesystemBackendStatus::UnsupportedMetadata
                                               ? WindowsBootstrapFilesystemStatus::UnsupportedMetadata
                                               : copied.status == WindowsFilesystemBackendStatus::IdentityMismatch
                                                ? WindowsBootstrapFilesystemStatus::IdentityMismatch
                                                : WindowsBootstrapFilesystemStatus::BackendFailure,
                                           true);
        }
        item->backupComplete = true;
        return state.persist("backup-complete");
    }

    if (intent.kind == BootstrapIntentKind::ReplaceInventoryItem) {
        if (item->classification != JournalItemClass::ReplaceExisting &&
            item->classification != JournalItemClass::CreateNew) {
            return result(WindowsBootstrapFilesystemStatus::InvalidIntentPath);
        }
        if (item->replacementComplete) return {};
        if (!state.allBackupsComplete()) {
            return result(WindowsBootstrapFilesystemStatus::InvalidIntentOrder);
        }
        if (std::any_of(state.items.begin(), state.items.end(), [](const ItemState& entry) {
                return entry.classification == JournalItemClass::CreateNew &&
                       !entry.installParentIdentity.valid;
            })) {
            return result(WindowsBootstrapFilesystemStatus::InvalidIntentOrder);
        }
        item->replacementStarted = true;
        WindowsBootstrapFilesystemResult persisted =
            state.persist("replacement-started");
        if (!persisted.ok()) return persisted;
        const WindowsFilesystemReplaceMode mode =
            item->classification == JournalItemClass::CreateNew
                ? WindowsFilesystemReplaceMode::CreateNew
                : WindowsFilesystemReplaceMode::ReplaceExisting;
        const WindowsFileIdentity expectedTarget =
            mode == WindowsFilesystemReplaceMode::CreateNew
                ? WindowsFileIdentity{}
                : item->originalIdentity;
        const WindowsFileContentFingerprint expectedTargetContent =
            mode == WindowsFilesystemReplaceMode::CreateNew
                ? WindowsFileContentFingerprint{}
                : item->backupContent;
        const WindowsFileMetadataSnapshot expectedTargetMetadata =
            mode == WindowsFilesystemReplaceMode::CreateNew
                ? WindowsFileMetadataSnapshot{}
                : item->rollbackMetadata;
        State::PreparedCheckpointContext checkpointContext{&state, item};
        const WindowsFilesystemPublicationCheckpoint checkpoint{
            &checkpointContext, &State::OnPrepared};
        const WindowsFileIdentity expectedPreparedIdentity =
            item->replacementPrepared ? item->preparedIdentity
                                       : WindowsFileIdentity{};
        const WindowsFileContentFingerprint expectedPreparedContent =
            item->replacementPrepared ? item->preparedContent
                                       : WindowsFileContentFingerprint{};
        const WindowsFileMetadataSnapshot expectedPreparedMetadata =
            item->replacementPrepared ? item->preparedMetadata
                                       : WindowsFileMetadataSnapshot{};
        const State::LeasedMutationResult replaceMutation =
            state.mutateWithJournalLease(
                [&state, item, mode, &expectedTarget, &expectedTargetContent,
                 &expectedTargetMetadata, &expectedPreparedIdentity,
                 &expectedPreparedContent, &expectedPreparedMetadata,
                 &checkpoint] {
                    return state.backend.ReplaceFileDurableNoFollowWithCheckpoint(
                        state.stagingRoot, item->relativePath,
                        item->stagingIdentity, item->stagingContent, {},
                        state.installRoot, item->relativePath,
                        item->installParentIdentity,
                        item->forwardTemporaryLeaf, mode, expectedTarget,
                        expectedTargetContent, expectedTargetMetadata,
                        expectedPreparedIdentity, expectedPreparedContent,
                        expectedPreparedMetadata,
                        item->replacementPrepared ? nullptr : &checkpoint,
                        item->installedIdentity, item->installedMetadata);
                });
        if (!replaceMutation.mutationInvoked) {
            return state.blockAfterStarted(
                replaceMutation.backendResult,
                WindowsBootstrapFilesystemStatus::PartialDurableState,
                true);
        }
        const WindowsFilesystemBackendResult replaced =
            replaceMutation.backendResult;
        if (!replaced.ok() || !item->installedIdentity.valid ||
            !item->installedMetadata.valid) {
            WindowsBootstrapFilesystemStatus failure =
                WindowsBootstrapFilesystemStatus::BackendFailure;
            if (replaced.status == WindowsFilesystemBackendStatus::IdentityMismatch) {
                failure = WindowsBootstrapFilesystemStatus::IdentityMismatch;
            } else if (replaced.status == WindowsFilesystemBackendStatus::ContentMismatch) {
                failure = WindowsBootstrapFilesystemStatus::ContentMismatch;
            } else if (replaced.status ==
                       WindowsFilesystemBackendStatus::DurabilityFailed) {
                failure =
                    WindowsBootstrapFilesystemStatus::PartialDurableState;
            } else if (replaced.status == WindowsFilesystemBackendStatus::AlreadyExists) {
                failure = WindowsBootstrapFilesystemStatus::UnknownTargetItem;
            } else if (replaced.status ==
                       WindowsFilesystemBackendStatus::UnsupportedMetadata) {
                failure = WindowsBootstrapFilesystemStatus::UnsupportedMetadata;
            }
            return state.blockAfterStarted(replaced, failure, true);
        }
        item->replacementComplete = true;
        return state.persist("replacement-complete");
    }

    if (intent.kind == BootstrapIntentKind::RemoveInstalledItem) {
        if (item->classification != JournalItemClass::RemoveExisting) {
            return result(WindowsBootstrapFilesystemStatus::InvalidIntentPath);
        }
        if (item->replacementComplete) return {};
        if (!state.allBackupsComplete() || !item->backupComplete) {
            return result(WindowsBootstrapFilesystemStatus::InvalidIntentOrder);
        }
        item->replacementStarted = true;
        WindowsBootstrapFilesystemResult persisted =
            state.persist("removal-started");
        if (!persisted.ok()) return persisted;
        const State::LeasedMutationResult deleteMutation =
            state.mutateWithJournalLease([&state, item] {
                return state.backend.DeleteFileDurableNoFollow(
                    state.installRoot, item->relativePath,
                    item->installParentIdentity, item->originalIdentity,
                    item->backupContent, item->rollbackMetadata);
            });
        if (!deleteMutation.mutationInvoked) {
            return state.blockAfterStarted(
                deleteMutation.backendResult,
                WindowsBootstrapFilesystemStatus::PartialDurableState,
                true);
        }
        if (!deleteMutation.backendResult.ok()) {
            const WindowsBootstrapFilesystemStatus status =
                deleteMutation.backendResult.status ==
                        WindowsFilesystemBackendStatus::IdentityMismatch
                    ? WindowsBootstrapFilesystemStatus::IdentityMismatch
                    : deleteMutation.backendResult.status ==
                              WindowsFilesystemBackendStatus::ContentMismatch
                          ? WindowsBootstrapFilesystemStatus::ContentMismatch
                          : WindowsBootstrapFilesystemStatus::PartialDurableState;
            return state.blockAfterStarted(deleteMutation.backendResult,
                                           status, true);
        }
        item->replacementComplete = true;
        return state.persist("removal-complete");
    }

    if (!state.rollbackDurable || state.commitComplete) {
        return result(WindowsBootstrapFilesystemStatus::InvalidIntentOrder);
    }

    if (intent.kind == BootstrapIntentKind::RestoreBackup) {
        if (item->classification == JournalItemClass::RemoveExisting &&
            item->removalAbsentVerified) {
            return {};
        }
        if ((item->classification != JournalItemClass::ReplaceExisting &&
             item->classification != JournalItemClass::RemoveExisting) ||
            !item->backupComplete || !item->replacementComplete) {
            return result(WindowsBootstrapFilesystemStatus::InvalidIntentOrder);
        }
        if (item->restoreComplete) return {};
        item->restoreStarted = true;
        WindowsBootstrapFilesystemResult persisted =
            state.persist("restore-started");
        if (!persisted.ok()) return persisted;
        State::RestorePreparedCheckpointContext checkpointContext{&state, item};
        const WindowsFilesystemPublicationCheckpoint checkpoint{
            &checkpointContext, &State::OnRestorePrepared};
        const WindowsFileIdentity expectedPreparedIdentity =
            item->restorePrepared ? item->restorePreparedIdentity
                                  : WindowsFileIdentity{};
        const WindowsFileContentFingerprint expectedPreparedContent =
            item->restorePrepared ? item->restorePreparedContent
                                  : WindowsFileContentFingerprint{};
        const WindowsFileMetadataSnapshot expectedPreparedMetadata =
            item->restorePrepared ? item->restorePreparedMetadata
                                  : WindowsFileMetadataSnapshot{};
        const bool restoringRemoval =
            item->classification == JournalItemClass::RemoveExisting;
        const State::LeasedMutationResult restoreMutation =
            state.mutateWithJournalLease(
                [&state, item, restoringRemoval, &expectedPreparedIdentity,
                 &expectedPreparedContent, &expectedPreparedMetadata,
                 &checkpoint] {
                return state.backend.ReplaceFileDurableNoFollowWithCheckpoint(
                    state.backupRoot, item->backupRelativePath,
                    item->backupIdentity, item->backupContent,
                    item->rollbackMetadata,
                    state.installRoot, item->relativePath,
                    item->installParentIdentity, item->restoreTemporaryLeaf,
                    restoringRemoval
                        ? WindowsFilesystemReplaceMode::CreateNew
                        : WindowsFilesystemReplaceMode::ReplaceExisting,
                    restoringRemoval ? WindowsFileIdentity{}
                                      : item->installedIdentity,
                    restoringRemoval ? WindowsFileContentFingerprint{}
                                      : item->stagingContent,
                    restoringRemoval ? WindowsFileMetadataSnapshot{}
                                      : item->installedMetadata,
                    expectedPreparedIdentity,
                    expectedPreparedContent, expectedPreparedMetadata,
                    item->restorePrepared ? nullptr : &checkpoint,
                    item->restoredIdentity,
                    item->restoredMetadata);
            });
        if (!restoreMutation.mutationInvoked) {
            return state.blockAfterStarted(
                restoreMutation.backendResult,
                WindowsBootstrapFilesystemStatus::PartialDurableState,
                true);
        }
        const WindowsFilesystemBackendResult restored =
            restoreMutation.backendResult;
        if (!restored.ok() || !item->restoredIdentity.valid ||
            !item->restoredMetadata.valid ||
            !state.metadataMatches(item->restoredMetadata,
                                   item->rollbackMetadata)) {
            return state.blockAfterStarted(
                restored,
                restored.status == WindowsFilesystemBackendStatus::IdentityMismatch
                    ? WindowsBootstrapFilesystemStatus::IdentityMismatch
                    : restored.status == WindowsFilesystemBackendStatus::ContentMismatch
                        ? WindowsBootstrapFilesystemStatus::ContentMismatch
                        : restored.status ==
                              WindowsFilesystemBackendStatus::UnsupportedMetadata
                            ? WindowsBootstrapFilesystemStatus::UnsupportedMetadata
                        : WindowsBootstrapFilesystemStatus::BackendFailure,
                true);
        }
        item->restoreComplete = true;
        state.updateRollbackComplete();
        return state.persist("restore-complete");
    }

    if (item->classification != JournalItemClass::CreateNew ||
        !item->replacementComplete) {
        return result(WindowsBootstrapFilesystemStatus::InvalidIntentOrder);
    }
    if (item->removeNewComplete) return {};
    item->removeNewStarted = true;
    WindowsBootstrapFilesystemResult persisted =
        state.persist("new-file-remove-started");
    if (!persisted.ok()) return persisted;
    const State::LeasedMutationResult deleteMutation =
        state.mutateWithJournalLease([&state, item] {
            return state.backend.DeleteFileDurableNoFollow(
                state.installRoot, item->relativePath,
                item->installParentIdentity, item->installedIdentity,
                item->stagingContent, item->installedMetadata);
        });
        if (!deleteMutation.mutationInvoked) {
            return state.blockAfterStarted(
                deleteMutation.backendResult,
                WindowsBootstrapFilesystemStatus::PartialDurableState,
                true);
        }
        const WindowsFilesystemBackendResult removed = deleteMutation.backendResult;
        if (!removed.ok()) {
            return state.blockAfterStarted(
                removed,
            removed.status == WindowsFilesystemBackendStatus::IdentityMismatch
                ? WindowsBootstrapFilesystemStatus::IdentityMismatch
                : removed.status == WindowsFilesystemBackendStatus::ContentMismatch
                    ? WindowsBootstrapFilesystemStatus::ContentMismatch
                    : removed.status ==
                          WindowsFilesystemBackendStatus::UnsupportedMetadata
                        ? WindowsBootstrapFilesystemStatus::UnsupportedMetadata
                    : WindowsBootstrapFilesystemStatus::BackendFailure,
                true);
        }
    item->removeNewComplete = true;
    state.updateRollbackComplete();
    return state.persist("new-file-remove-complete");
}

WindowsBootstrapFilesystemResult
WindowsBootstrapFilesystemPort::RecordUpdatedChildIdentity(
    const BootstrapProcessIdentity& identity) {
    State& state = *state_;
    if (!state.begun || state.blocked) {
        return result(state.blocked
                          ? WindowsBootstrapFilesystemStatus::TransactionBlocked
                          : WindowsBootstrapFilesystemStatus::InvalidIntentOrder);
    }
    if (!validOwnedProcessIdentity(identity) || state.rollbackDurable) {
        return result(WindowsBootstrapFilesystemStatus::InvalidIntentOrder);
    }
    if (state.commitComplete &&
        (!state.committedApplicationRelaunchPending ||
         state.updatedChildIdentity.valid || !state.healthPhase.empty() ||
         state.healthTokenConsumed)) {
        return result(WindowsBootstrapFilesystemStatus::InvalidIntentOrder);
    }
    if (state.updatedChildIdentity.valid) {
        if (state.updatedChildIdentity == identity) return {};
        state.blocked = true;
        return result(WindowsBootstrapFilesystemStatus::PartialDurableState);
    }
    state.updatedChildIdentity = identity;
    state.healthPhase = "awaiting-health";
    return state.persist(state.commitComplete
                             ? "committed-child-recorded"
                             : "updated-child-recorded");
}

WindowsBootstrapFilesystemResult WindowsBootstrapFilesystemPort::RecordHealthState(
    const std::string& healthPhase,
    const bool nonceConsumed) {
    State& state = *state_;
    if (!state.begun || state.blocked) {
        return result(state.blocked
                          ? WindowsBootstrapFilesystemStatus::TransactionBlocked
                          : WindowsBootstrapFilesystemStatus::InvalidIntentOrder);
    }
    if (!state.updatedChildIdentity.valid || state.rollbackDurable ||
        (healthPhase != "awaiting-health" && healthPhase != "accepted") ||
        (healthPhase == "awaiting-health" && nonceConsumed) ||
        (healthPhase == "accepted" && !nonceConsumed)) {
        return result(WindowsBootstrapFilesystemStatus::InvalidIntentOrder);
    }
    if (state.commitComplete &&
        !state.committedApplicationRelaunchPending) {
        return result(WindowsBootstrapFilesystemStatus::InvalidIntentOrder);
    }
    if (healthPhase == "awaiting-health") {
        return state.healthPhase == healthPhase && !state.healthTokenConsumed
            ? WindowsBootstrapFilesystemResult{}
            : result(WindowsBootstrapFilesystemStatus::InvalidIntentOrder);
    }
    if (state.healthTokenConsumed) {
        return state.healthPhase == healthPhase
            ? WindowsBootstrapFilesystemResult{}
            : result(WindowsBootstrapFilesystemStatus::InvalidIntentOrder);
    }
    state.healthPhase = healthPhase;
    state.healthTokenConsumed = true;
    return state.persist(state.commitComplete
                             ? "committed-health-accepted"
                             : "committing");
}

WindowsBootstrapFilesystemResult
WindowsBootstrapFilesystemPort::PrepareRecoveredHealthRelaunch(
    const BootstrapProcessIdentity& terminatedChild) {
    State& state = *state_;
    if (!state.begun || state.blocked) {
        return result(state.blocked
                          ? WindowsBootstrapFilesystemStatus::TransactionBlocked
                          : WindowsBootstrapFilesystemStatus::InvalidIntentOrder);
    }
    const bool forwardHealthAttempt =
        !state.commitComplete && state.healthPhase == "awaiting-health" &&
        !state.healthTokenConsumed;
    const bool committedHealthAttempt =
        state.commitComplete &&
        ((state.healthPhase == "accepted" && state.healthTokenConsumed) ||
         (state.healthPhase == "awaiting-health" &&
          !state.healthTokenConsumed));
    if (!validOwnedProcessIdentity(terminatedChild) ||
        !state.updatedChildIdentity.valid ||
        !(state.updatedChildIdentity == terminatedChild) ||
        (!forwardHealthAttempt && !committedHealthAttempt) ||
        state.rollbackDurable) {
        return result(WindowsBootstrapFilesystemStatus::InvalidIntentOrder);
    }
    state.updatedChildIdentity = {};
    state.healthPhase.clear();
    state.healthTokenConsumed = false;
    state.committedApplicationRelaunchPending = state.commitComplete;
    return state.persist(state.commitComplete
                             ? "committed-health-relaunch-prepared"
                             : "health-relaunch-prepared");
}

WindowsBootstrapFilesystemResult
WindowsBootstrapFilesystemPort::RecordPreviousApplicationRelaunchStarted() {
    State& state = *state_;
    if (!state.begun || state.blocked) {
        return result(state.blocked
                          ? WindowsBootstrapFilesystemStatus::TransactionBlocked
                          : WindowsBootstrapFilesystemStatus::InvalidIntentOrder);
    }
    if (!state.rollbackComplete || state.commitComplete ||
        state.previousApplicationRelaunchComplete) {
        return result(WindowsBootstrapFilesystemStatus::InvalidIntentOrder);
    }
    if (state.previousApplicationRelaunchStarted) return {};
    state.previousApplicationRelaunchStarted = true;
    return state.persist("previous-relaunch-pending");
}

WindowsBootstrapFilesystemResult
WindowsBootstrapFilesystemPort::RecordPreviousApplicationIdentity(
    const BootstrapProcessIdentity& identity) {
    State& state = *state_;
    if (!state.begun || state.blocked) {
        return result(state.blocked
                          ? WindowsBootstrapFilesystemStatus::TransactionBlocked
                          : WindowsBootstrapFilesystemStatus::InvalidIntentOrder);
    }
    if (!state.previousApplicationRelaunchStarted ||
        state.previousApplicationRelaunchComplete ||
        !validOwnedProcessIdentity(identity)) {
        return result(WindowsBootstrapFilesystemStatus::InvalidIntentOrder);
    }
    if (state.previousApplicationIdentity.valid) {
        return state.previousApplicationIdentity == identity
            ? WindowsBootstrapFilesystemResult{}
            : result(WindowsBootstrapFilesystemStatus::IdentityMismatch);
    }
    state.previousApplicationIdentity = identity;
    return state.persist("previous-relaunch-owned");
}

WindowsBootstrapFilesystemResult
WindowsBootstrapFilesystemPort::ResetPreviousApplicationIdentity(
    const BootstrapProcessIdentity& identity) {
    State& state = *state_;
    if (!state.begun || state.blocked) {
        return result(state.blocked
                          ? WindowsBootstrapFilesystemStatus::TransactionBlocked
                          : WindowsBootstrapFilesystemStatus::InvalidIntentOrder);
    }
    if (!state.previousApplicationRelaunchStarted ||
        state.previousApplicationRelaunchComplete ||
        !state.previousApplicationIdentity.valid ||
        !(state.previousApplicationIdentity == identity)) {
        return result(WindowsBootstrapFilesystemStatus::IdentityMismatch);
    }
    state.previousApplicationIdentity = {};
    return state.persist("previous-relaunch-pending");
}

WindowsBootstrapFilesystemResult
WindowsBootstrapFilesystemPort::RecordPreviousApplicationRelaunchComplete(
    const BootstrapProcessIdentity& identity) {
    State& state = *state_;
    if (!state.begun || state.blocked) {
        return result(state.blocked
                          ? WindowsBootstrapFilesystemStatus::TransactionBlocked
                          : WindowsBootstrapFilesystemStatus::InvalidIntentOrder);
    }
    if (!state.previousApplicationRelaunchStarted ||
        !state.previousApplicationIdentity.valid ||
        !(state.previousApplicationIdentity == identity)) {
        return result(WindowsBootstrapFilesystemStatus::IdentityMismatch);
    }
    if (state.previousApplicationRelaunchComplete) return {};
    state.previousApplicationRelaunchComplete = true;
    return state.persist("rolled-back");
}

WindowsBootstrapFilesystemResult
WindowsBootstrapFilesystemPort::CleanupCommittedInvocation() {
    State& state = *state_;
    if (!state.begun || state.blocked) {
        return result(state.blocked
                          ? WindowsBootstrapFilesystemStatus::TransactionBlocked
                          : WindowsBootstrapFilesystemStatus::InvalidIntentOrder);
    }
    const bool committedHealthAccepted =
        state.updatedChildIdentity.valid && state.healthPhase == "accepted" &&
        state.healthTokenConsumed;
    const bool committedTerminal =
        state.commitComplete && state.backupDirectoryRemoved &&
        !state.rollbackDurable && !state.rollbackComplete &&
        committedHealthAccepted;
    const bool rolledBackTerminal =
        !state.commitComplete && state.rollbackComplete &&
        state.backupDirectoryRemoved &&
        state.previousApplicationRelaunchComplete;
    if ((!committedTerminal && !rolledBackTerminal) ||
        state.terminalJournalRetired) {
        return result(WindowsBootstrapFilesystemStatus::InvalidIntentOrder);
    }

    if (!state.invocationCleanupStarted) {
        state.invocationCleanupStarted = true;
        const WindowsBootstrapFilesystemResult started =
            state.persist("invocation-cleanup-started");
        if (!started.ok()) {
            return result(WindowsBootstrapFilesystemStatus::CleanupPending,
                          {started.backendStatus, started.nativeError});
        }
    }

    state.invocationCleanup.allowAlreadyRemoved =
        state.invocationCleanupStarted;
    state.invocationCleanup.runtimePairExpected = state.runtimePairExpected;
    state.invocationCleanup.runtimePairDirectoryIdentity =
        state.runtimePairDirectoryIdentity;
    state.invocationCleanup.runtimePairBootstrapIdentity =
        state.runtimePairBootstrapIdentity;
    state.invocationCleanup.runtimePairBootstrapContent =
        state.runtimePairBootstrapContent;

    const State::LeasedMutationResult cleanup =
        state.mutateWithJournalLease([&state] {
            return state.backend.CleanupInvocationResidueNoFollow(
                state.stagingRoot, state.transactionRoot,
                state.invocationCleanup);
        });
    if (!cleanup.mutationInvoked) {
        const bool retryable =
            cleanup.backendResult.status ==
            WindowsFilesystemBackendStatus::AccessDenied;
        state.blocked = !retryable;
        return result(retryable
                          ? WindowsBootstrapFilesystemStatus::RetryableAccessDenied
                          : WindowsBootstrapFilesystemStatus::CleanupPending,
                      cleanup.backendResult);
    }
    const WindowsFilesystemBackendResult cleaned = cleanup.backendResult;
    if (!cleaned.ok()) {
        const bool retryable =
            cleaned.status == WindowsFilesystemBackendStatus::AccessDenied;
        state.blocked = !retryable;
        const WindowsBootstrapFilesystemStatus status =
            cleaned.status == WindowsFilesystemBackendStatus::IdentityMismatch
                ? WindowsBootstrapFilesystemStatus::IdentityMismatch
            : cleaned.status == WindowsFilesystemBackendStatus::ContentMismatch
                ? WindowsBootstrapFilesystemStatus::ContentMismatch
            : cleaned.status == WindowsFilesystemBackendStatus::ReparsePoint
                ? WindowsBootstrapFilesystemStatus::ReparsePointRejected
            : cleaned.status == WindowsFilesystemBackendStatus::AccessDenied
                ? WindowsBootstrapFilesystemStatus::RetryableAccessDenied
                : WindowsBootstrapFilesystemStatus::CleanupPending;
        return result(status, cleaned);
    }

    state.invocationCleanupComplete = true;
    const WindowsBootstrapFilesystemResult completed =
        state.persist("invocation-cleanup-complete");
    if (!completed.ok()) {
        return result(WindowsBootstrapFilesystemStatus::CleanupPending,
                      {completed.backendStatus, completed.nativeError});
    }
    return {};
}

WindowsBootstrapFilesystemResult
WindowsBootstrapFilesystemPort::RetireTerminalJournal() {
    State& state = *state_;
    const auto retireFailure = [this, &state](
        const WindowsFilesystemBackendResult& backendResult) {
        const bool retryable =
            backendResult.status == WindowsFilesystemBackendStatus::AccessDenied;
        state.blocked = !retryable;
        return result(
            retryable
                ? WindowsBootstrapFilesystemStatus::RetryableAccessDenied
                : WindowsBootstrapFilesystemStatus::PartialDurableState,
            backendResult);
    };
    if (!state.begun || state.blocked) {
        return result(state.blocked
                          ? WindowsBootstrapFilesystemStatus::TransactionBlocked
                          : WindowsBootstrapFilesystemStatus::InvalidIntentOrder);
    }
    const bool committedAvailabilityConfirmed =
        !state.committedApplicationRelaunchPending ||
        (state.updatedChildIdentity.valid && state.healthPhase == "accepted" &&
         state.healthTokenConsumed);
    const bool committedTerminal = state.commitComplete &&
        state.backupDirectoryRemoved && committedAvailabilityConfirmed &&
        state.invocationCleanupComplete;
    const bool rolledBackTerminal =
        state.rollbackComplete && state.backupDirectoryRemoved &&
        state.previousApplicationRelaunchComplete &&
        state.invocationCleanupComplete;
    if ((!committedTerminal && !rolledBackTerminal) ||
        state.terminalJournalRetired) {
        return state.terminalJournalRetired
            ? WindowsBootstrapFilesystemResult{}
            : result(WindowsBootstrapFilesystemStatus::InvalidIntentOrder);
    }
    state.invocationCleanup.allowAlreadyRemoved = true;
    state.invocationCleanup.runtimePairExpected = state.runtimePairExpected;
    state.invocationCleanup.runtimePairDirectoryIdentity =
        state.runtimePairDirectoryIdentity;
    state.invocationCleanup.runtimePairBootstrapIdentity =
        state.runtimePairBootstrapIdentity;
    state.invocationCleanup.runtimePairBootstrapContent =
        state.runtimePairBootstrapContent;
    const State::LeasedMutationResult cleanupRechecked =
        state.mutateWithJournalLease([&state] {
            return state.backend.CleanupInvocationResidueNoFollow(
                state.stagingRoot, state.transactionRoot,
                state.invocationCleanup);
        });
    if (!cleanupRechecked.mutationInvoked ||
        !cleanupRechecked.backendResult.ok()) {
        const WindowsFilesystemBackendResult backendResult =
            cleanupRechecked.backendResult;
        const bool retryable =
            backendResult.status == WindowsFilesystemBackendStatus::AccessDenied;
        state.blocked = !retryable;
        const WindowsBootstrapFilesystemStatus status =
            backendResult.status ==
                    WindowsFilesystemBackendStatus::IdentityMismatch
                ? WindowsBootstrapFilesystemStatus::IdentityMismatch
                : backendResult.status ==
                          WindowsFilesystemBackendStatus::ContentMismatch
                    ? WindowsBootstrapFilesystemStatus::ContentMismatch
                    : backendResult.status ==
                              WindowsFilesystemBackendStatus::ReparsePoint
                        ? WindowsBootstrapFilesystemStatus::ReparsePointRejected
                        : backendResult.status ==
                                  WindowsFilesystemBackendStatus::AccessDenied
                            ? WindowsBootstrapFilesystemStatus::RetryableAccessDenied
                        : WindowsBootstrapFilesystemStatus::CleanupPending;
        return result(status, backendResult);
    }
    std::vector<WindowsBootstrapJournalCandidate> candidates;
    const WindowsFilesystemBackendResult observed =
        state.backend.ReadJournalCandidatesNoFollow(state.transactionRoot,
                                                    candidates);
    if (!observed.ok()) {
        return retireFailure(observed);
    }
    if (candidates.empty()) {
        state.blocked = true;
        return result(WindowsBootstrapFilesystemStatus::PartialDurableState,
                      observed);
    }
    // Terminal journal deletion is only safe after the adapter-owned backup
    // namespace is proven absent again. `Apply(RemoveBackup)` has its own
    // guard, but retirement is a separate destructive path and can be called
    // after a restart or an interrupted cleanup cut.
    const WindowsFilesystemBackendResult backupAbsent =
        state.backend.ValidateBackupNamespaceNoFollow(
            state.transactionRoot, {}, {}, false, {});
    if (!backupAbsent.ok()) {
        return retireFailure(backupAbsent);
    }
    const WindowsBootstrapJournalRootIdentities roots{
        state.installRootIdentity, state.stagingRootIdentity,
        state.transactionRootIdentity, {}};
    const WindowsBootstrapJournalPackageAuthority* authority =
        packageAuthority_.has_value() ? &*packageAuthority_ : nullptr;
    const WindowsBootstrapJournalSelection selected =
        SelectDurableWindowsBootstrapJournal(
            candidates, &state.plan, &state.journal, &roots, true, authority);
    if (!selected.ok()) {
        state.blocked = true;
        return result(WindowsBootstrapFilesystemStatus::PartialDurableState);
    }
    const auto removeCandidate = [&state](
        const WindowsBootstrapJournalCandidate& candidate) {
        return state.backend.RetireJournalCandidateDurable(
            state.transactionRoot, candidate);
    };
    for (const WindowsBootstrapJournalCandidate& candidate : candidates) {
        if (candidate.name == selected.selectedName) continue;
        const WindowsFilesystemBackendResult removed =
            removeCandidate(candidate);
        if (!removed.ok()) {
            return retireFailure(removed);
        }
    }
    const auto winner = std::find_if(
        candidates.begin(), candidates.end(),
        [&selected](const WindowsBootstrapJournalCandidate& candidate) {
            return candidate.name == selected.selectedName;
        });
    if (winner == candidates.end()) {
        state.blocked = true;
        return result(WindowsBootstrapFilesystemStatus::PartialDurableState);
    }
    const WindowsFilesystemBackendResult winnerRemoved =
        removeCandidate(*winner);
    if (!winnerRemoved.ok()) {
        return retireFailure(winnerRemoved);
    }
    state.journalIdentity = {};
    state.journalContent = {};
    state.terminalJournalRetired = true;
    return {};
}

std::optional<BootstrapProcessIdentity>
WindowsBootstrapFilesystemPort::recoveredUpdatedChildIdentity() const {
    if (state_ == nullptr || !state_->begun ||
        !state_->updatedChildIdentity.valid) {
        return std::nullopt;
    }
    return state_->updatedChildIdentity;
}

std::string WindowsBootstrapFilesystemPort::recoveredHealthPhase() const {
    return state_ != nullptr && state_->begun ? state_->healthPhase
                                               : std::string{};
}

std::optional<std::uint32_t>
WindowsBootstrapFilesystemPort::recoveredPreviousFailureCount() const {
    if (state_ == nullptr || !state_->begun) return std::nullopt;
    return state_->previousFailureCount;
}

bool WindowsBootstrapFilesystemPort::
recoveredFailureRecordedForTransaction() const noexcept {
    return state_ != nullptr && state_->begun &&
           state_->failureRecordedForTransaction;
}

bool WindowsBootstrapFilesystemPort::recoveredRollbackComplete() const noexcept {
    return state_ != nullptr && state_->begun && state_->rollbackComplete;
}

bool WindowsBootstrapFilesystemPort::
recoveredPreviousApplicationRelaunchStarted() const noexcept {
    return state_ != nullptr && state_->begun &&
           state_->previousApplicationRelaunchStarted;
}

bool WindowsBootstrapFilesystemPort::
recoveredPreviousApplicationRelaunchComplete() const noexcept {
    return state_ != nullptr && state_->begun &&
           state_->previousApplicationRelaunchComplete;
}

std::optional<BootstrapProcessIdentity>
WindowsBootstrapFilesystemPort::recoveredPreviousApplicationIdentity() const {
    if (state_ == nullptr || !state_->begun ||
        !state_->previousApplicationIdentity.valid) {
        return std::nullopt;
    }
    return state_->previousApplicationIdentity;
}

std::optional<WindowsFileContentFingerprint>
WindowsBootstrapFilesystemPort::previousApplicationContentFingerprint() const {
    if (state_ == nullptr || !state_->begun || !state_->rollbackComplete) {
        return std::nullopt;
    }
    const auto entrypoint = std::find_if(
        state_->items.begin(), state_->items.end(),
        [this](const ItemState& item) {
            return item.relativePath == state_->plan.entrypoint;
        });
    if (entrypoint == state_->items.end() ||
        entrypoint->classification != JournalItemClass::ReplaceExisting ||
        !entrypoint->backupContent.valid) {
        return std::nullopt;
    }
    return entrypoint->backupContent;
}

bool WindowsBootstrapFilesystemPort::blocked() const noexcept {
    return state_ != nullptr && state_->blocked;
}

const char* WindowsBootstrapFilesystemStatusName(
    WindowsBootstrapFilesystemStatus status) {
    switch (status) {
    case WindowsBootstrapFilesystemStatus::Ok: return "Ok";
    case WindowsBootstrapFilesystemStatus::InvalidPlan: return "InvalidPlan";
    case WindowsBootstrapFilesystemStatus::InvalidJournal: return "InvalidJournal";
    case WindowsBootstrapFilesystemStatus::InvalidRoots: return "InvalidRoots";
    case WindowsBootstrapFilesystemStatus::UnsupportedIntent: return "UnsupportedIntent";
    case WindowsBootstrapFilesystemStatus::InvalidIntentPath: return "InvalidIntentPath";
    case WindowsBootstrapFilesystemStatus::InvalidIntentOrder: return "InvalidIntentOrder";
    case WindowsBootstrapFilesystemStatus::RootIdentityMismatch: return "RootIdentityMismatch";
    case WindowsBootstrapFilesystemStatus::RootVolumeMismatch: return "RootVolumeMismatch";
    case WindowsBootstrapFilesystemStatus::RootOverlapRejected: return "RootOverlapRejected";
    case WindowsBootstrapFilesystemStatus::ReparsePointRejected: return "ReparsePointRejected";
    case WindowsBootstrapFilesystemStatus::MissingPayloadItem: return "MissingPayloadItem";
    case WindowsBootstrapFilesystemStatus::ExistingItemMissing: return "ExistingItemMissing";
    case WindowsBootstrapFilesystemStatus::UnexpectedEntryType: return "UnexpectedEntryType";
    case WindowsBootstrapFilesystemStatus::UnknownTargetItem: return "UnknownTargetItem";
    case WindowsBootstrapFilesystemStatus::JournalCollision: return "JournalCollision";
    case WindowsBootstrapFilesystemStatus::JournalNotDurable: return "JournalNotDurable";
    case WindowsBootstrapFilesystemStatus::IdentityMismatch: return "IdentityMismatch";
    case WindowsBootstrapFilesystemStatus::ContentMismatch: return "ContentMismatch";
    case WindowsBootstrapFilesystemStatus::PartialDurableState: return "PartialDurableState";
    case WindowsBootstrapFilesystemStatus::CleanupPending: return "CleanupPending";
    case WindowsBootstrapFilesystemStatus::RetryableAccessDenied: return "RetryableAccessDenied";
    case WindowsBootstrapFilesystemStatus::UnsupportedMetadata: return "UnsupportedMetadata";
    case WindowsBootstrapFilesystemStatus::TransactionBlocked: return "TransactionBlocked";
    case WindowsBootstrapFilesystemStatus::BackendFailure: return "BackendFailure";
    }
    return "Unknown";
}

const char* WindowsFilesystemBackendStatusName(
    WindowsFilesystemBackendStatus status) {
    switch (status) {
    case WindowsFilesystemBackendStatus::Ok: return "Ok";
    case WindowsFilesystemBackendStatus::NotFound: return "NotFound";
    case WindowsFilesystemBackendStatus::AlreadyExists: return "AlreadyExists";
    case WindowsFilesystemBackendStatus::AccessDenied: return "AccessDenied";
    case WindowsFilesystemBackendStatus::InvalidPath: return "InvalidPath";
    case WindowsFilesystemBackendStatus::ExactPathMismatch: return "ExactPathMismatch";
    case WindowsFilesystemBackendStatus::ReparsePoint: return "ReparsePoint";
    case WindowsFilesystemBackendStatus::NotDirectory: return "NotDirectory";
    case WindowsFilesystemBackendStatus::NotRegularFile: return "NotRegularFile";
    case WindowsFilesystemBackendStatus::IdentityMismatch: return "IdentityMismatch";
    case WindowsFilesystemBackendStatus::ContentMismatch: return "ContentMismatch";
    case WindowsFilesystemBackendStatus::DirectoryNotEmpty: return "DirectoryNotEmpty";
    case WindowsFilesystemBackendStatus::CrossVolume: return "CrossVolume";
    case WindowsFilesystemBackendStatus::UnsupportedMetadata: return "UnsupportedMetadata";
    case WindowsFilesystemBackendStatus::DurabilityFailed: return "DurabilityFailed";
    case WindowsFilesystemBackendStatus::IoFailure: return "IoFailure";
    }
    return "Unknown";
}

}  // namespace aris::bootstrap
