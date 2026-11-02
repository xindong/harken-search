#include "bootstrap/HarkenSearchBootstrapAdmission.h"
#include "bootstrap/WindowsBootstrapFilesystem.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <Aclapi.h>
#include <sddl.h>
#include <winioctl.h>
#include <winternl.h>
#endif

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <system_error>
#include <string>
#include <utility>
#include <vector>

namespace {

using aris::bootstrap::BootstrapIntent;
using aris::bootstrap::BootstrapIntentKind;
using aris::bootstrap::BootstrapProcessIdentity;
using aris::bootstrap::BootstrapSha256Hex;
using aris::bootstrap::BuildReplacementJournal;
using aris::bootstrap::CreateNativeWindowsBootstrapFilesystemBackend;
using aris::bootstrap::DecodeWindowsBootstrapJournal;
using aris::bootstrap::SignatureStatus;
using aris::bootstrap::SignedInstallPlan;
using aris::bootstrap::ValidateSignedInstallPlan;
using aris::bootstrap::ValidatedInstallPlan;
using aris::bootstrap::ReplacementJournal;
using aris::bootstrap::WindowsBootstrapFilesystemBackend;
using aris::bootstrap::WindowsBootstrapFilesystemNativeTestHooks;
using aris::bootstrap::WindowsBootstrapFilesystemNativeTestFailurePoint;
using aris::bootstrap::WindowsBootstrapFilesystemPort;
using aris::bootstrap::WindowsBootstrapFilesystemRoots;
using aris::bootstrap::WindowsBootstrapInvocationCleanup;
using aris::bootstrap::WindowsBootstrapInvocationCleanupFile;
using aris::bootstrap::WindowsBootstrapFilesystemTrustedRootPolicy;
using aris::bootstrap::WindowsBootstrapFilesystemResult;
using aris::bootstrap::WindowsBootstrapFilesystemStatus;
using aris::bootstrap::WindowsBootstrapRecoveryDisposition;
using aris::bootstrap::WindowsBootstrapJournalCandidate;
using aris::bootstrap::WindowsBootstrapJournalDecodeStatus;
using aris::bootstrap::WindowsBootstrapJournalDecodeResult;
using aris::bootstrap::WindowsBootstrapJournalItemRecord;
using aris::bootstrap::WindowsBootstrapJournalRecord;
using aris::bootstrap::WindowsBootstrapJournalRootIdentities;
using aris::bootstrap::WindowsBootstrapJournalSelectionStatus;
using aris::bootstrap::EncodeWindowsBootstrapJournal;
using aris::bootstrap::SelectDurableWindowsBootstrapJournal;
using aris::bootstrap::WindowsFileContentFingerprint;
using aris::bootstrap::WindowsFileIdentity;
using aris::bootstrap::WindowsFileMetadataSnapshot;
using aris::bootstrap::WindowsFilesystemBackendResult;
using aris::bootstrap::WindowsFilesystemBackendStatus;
using aris::bootstrap::WindowsFilesystemEntryKind;
using aris::bootstrap::WindowsFilesystemHandle;
using aris::bootstrap::WindowsFilesystemObservation;
using aris::bootstrap::WindowsFilesystemPublicationCheckpoint;
using aris::bootstrap::WindowsFilesystemReplaceMode;
using aris::bootstrap::WindowsFilesystemRootRole;
using aris::bootstrap::PackagePlanTrustConfig;
using aris::bootstrap::DurableRecoveryAdmissionResult;
using aris::bootstrap::DurableRecoveryAdmissionStatus;
using aris::bootstrap::AdmitDurableBootstrapRecovery;

int failures = 0;

void check(bool value, const char* message) {
    if (value) return;
    std::fprintf(stderr, "FAIL: %s\n", message);
    ++failures;
}

std::uint64_t journalFNV1a64(const std::uint8_t* data,
                             const std::size_t size) {
    std::uint64_t value = 14695981039346656037ULL;
    for (std::size_t index = 0; index < size; ++index) {
        value ^= data[index];
        value *= 1099511628211ULL;
    }
    return value;
}

void rewriteJournalChecksum(std::vector<std::uint8_t>& bytes) {
    constexpr std::size_t kAuthenticationTagBytes = 32U;
    constexpr std::size_t kChecksumBytes = sizeof(std::uint64_t);
    if (bytes.size() < kAuthenticationTagBytes + kChecksumBytes) return;
    const std::size_t checksumOffset =
        bytes.size() - kAuthenticationTagBytes - kChecksumBytes;
    const std::uint64_t checksum =
        journalFNV1a64(bytes.data(), checksumOffset);
    for (std::uint32_t shift = 0; shift < 64U; shift += 8U) {
        bytes[checksumOffset + shift / 8U] =
            static_cast<std::uint8_t>(checksum >> shift);
    }
}

void checkNative(const WindowsFilesystemBackendResult& result,
                 const char* message) {
    if (result.ok()) return;
    std::fprintf(stderr, "FAIL: %s (status=%u, native=%lu)\n", message,
                 static_cast<unsigned int>(result.status),
                 static_cast<unsigned long>(result.nativeError));
    ++failures;
}

#if defined(_WIN32)
bool secureNativeTrustedDirectory(const std::filesystem::path& path) {
    HANDLE token = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token) == FALSE) {
        return false;
    }
    DWORD required = 0U;
    static_cast<void>(GetTokenInformation(token, TokenUser, nullptr, 0U,
                                          &required));
    if (required == 0U || required > 64U * 1024U) {
        CloseHandle(token);
        return false;
    }
    std::vector<std::uint8_t> userStorage(required);
    const bool userQueried = GetTokenInformation(
        token, TokenUser, userStorage.data(), required, &required) != FALSE;
    CloseHandle(token);
    if (!userQueried) return false;
    const auto* user = reinterpret_cast<const TOKEN_USER*>(userStorage.data());

    std::array<std::uint8_t, SECURITY_MAX_SID_SIZE> systemStorage{};
    std::array<std::uint8_t, SECURITY_MAX_SID_SIZE> adminStorage{};
    DWORD systemSize = static_cast<DWORD>(systemStorage.size());
    DWORD adminSize = static_cast<DWORD>(adminStorage.size());
    if (CreateWellKnownSid(WinLocalSystemSid, nullptr, systemStorage.data(),
                           &systemSize) == FALSE ||
        CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr,
                           adminStorage.data(), &adminSize) == FALSE) {
        return false;
    }

    std::array<EXPLICIT_ACCESSW, 3U> entries{};
    const std::array<PSID, 3U> sids{
        user->User.Sid, systemStorage.data(), adminStorage.data()};
    for (std::size_t index = 0U; index < entries.size(); ++index) {
        entries[index].grfAccessPermissions = GENERIC_ALL;
        entries[index].grfAccessMode = GRANT_ACCESS;
        entries[index].grfInheritance = SUB_CONTAINERS_AND_OBJECTS_INHERIT;
        entries[index].Trustee.TrusteeForm = TRUSTEE_IS_SID;
        entries[index].Trustee.TrusteeType = TRUSTEE_IS_UNKNOWN;
        entries[index].Trustee.ptstrName =
            reinterpret_cast<LPWSTR>(sids[index]);
    }
    PACL dacl = nullptr;
    const DWORD built = SetEntriesInAclW(
        static_cast<ULONG>(entries.size()), entries.data(), nullptr, &dacl);
    if (built != ERROR_SUCCESS || dacl == nullptr) return false;
    const std::wstring wide = path.wstring();
    const DWORD applied = SetNamedSecurityInfoW(
        const_cast<wchar_t*>(wide.c_str()), SE_FILE_OBJECT,
        OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION |
            PROTECTED_DACL_SECURITY_INFORMATION,
        user->User.Sid, nullptr, dacl, nullptr);
    LocalFree(dacl);
    return applied == ERROR_SUCCESS;
}
#endif

WindowsFileIdentity identity(std::uint8_t marker, std::uint64_t volume = 77) {
    WindowsFileIdentity value;
    value.valid = true;
    value.volumeSerial = volume;
    value.fileId.fill(marker);
    return value;
}

WindowsFileContentFingerprint fingerprint(const std::uint8_t marker,
                                           const std::uint64_t byteSize) {
    WindowsFileContentFingerprint value;
    value.valid = true;
    value.byteSize = byteSize;
    value.sha256.fill(marker);
    return value;
}

WindowsFileMetadataSnapshot metadata(const std::uint8_t marker) {
    WindowsFileMetadataSnapshot value;
    value.valid = true;
    value.bytes.assign(88U, 0U);
    value.bytes[0] = 0x41U;
    value.bytes[1] = 0x42U;
    value.bytes[2] = 0x4dU;
    value.bytes[3] = 0x32U;
    value.bytes[4] = marker;
    value.bytes[40] = 20U;
    value.bytes[44] = 1U;
    value.bytes[47] = 0x80U;
    value.bytes[64] = 20U;
    value.bytes[68] = 1U;
    value.bytes[71] = 0x80U;
    return value;
}

BootstrapProcessIdentity processIdentity(const std::uint64_t processId,
                                         const std::uint8_t marker) {
    BootstrapProcessIdentity value;
    value.valid = true;
    value.processId = processId;
    value.creationTime = 10'000U + processId;
    value.imagePath = "D:/Apps/Aris/HarkenSearch.exe";
    value.imageVolumeSerial = 77U;
    value.imageFileId.fill(marker);
    return value;
}

bool checkpointHealthyChildForCommit(
    WindowsBootstrapFilesystemPort& filesystem) {
    const BootstrapProcessIdentity child = processIdentity(901U, 0x79U);
    return filesystem.RecordUpdatedChildIdentity(child).ok() &&
           filesystem.RecordHealthState("accepted", true).ok();
}

WindowsFileContentFingerprint fingerprintHex(const char* hex,
                                             const std::uint64_t byteSize) {
    WindowsFileContentFingerprint value;
    value.valid = true;
    value.byteSize = byteSize;
    const auto nibble = [](const char character) -> std::uint8_t {
        if (character >= '0' && character <= '9') {
            return static_cast<std::uint8_t>(character - '0');
        }
        return static_cast<std::uint8_t>(10 + character - 'a');
    };
    for (std::size_t index = 0; index < value.sha256.size(); ++index) {
        value.sha256[index] = static_cast<std::uint8_t>(
            (nibble(hex[index * 2U]) << 4U) | nibble(hex[index * 2U + 1U]));
    }
    return value;
}

SignedInstallPlan rawPlan() {
    SignedInstallPlan plan;
    plan.schemaVersion = 1;
    plan.product = "aris";
    plan.channel = "testing";
    plan.version = "2.0.0";
    plan.buildId = "build-200";
    plan.keyId = "testing-2026";
    plan.detachedSignature = std::string(128, 'a');
    plan.payloadSha256 = std::string(64, 'b');
    plan.packagePlanDigest = std::string(64, 'f');
    plan.oldProcessId = 99;
    plan.healthToken = std::string(64, 'c');
    plan.identity.targetRoot = "D:\\Apps\\Aris";
    plan.identity.installationId = "portable-aris-02";
    plan.identity.targetRootIdentity = {"D:/Apps/Aris", 77U, {}, true};
    plan.identity.targetRootIdentity.fileId.fill(0x42U);
    plan.identity.protectedDataRoots = {
        "C:\\Users\\test\\AppData\\Local\\Aris",
    };
    plan.invocation.updaterOwnedBase = "D:\\Trusted\\HarkenSearchUpdater";
    plan.invocation.stagingRoot = "D:\\Trusted\\HarkenSearchUpdater\\staging";
    plan.invocation.transactionRoot =
        "D:\\Trusted\\HarkenSearchUpdater\\transaction";
    plan.invocation.invocationIdentity = "signed-invocation-01";
    plan.invocation.transactionId = "attempt-fs-01";
    plan.invocation.packagePlanDigest = plan.packagePlanDigest;
    plan.invocation.parentProcess = processIdentity(99U, 0x31U);
    plan.invocation.oldProcess = plan.invocation.parentProcess;
    plan.identity.oldExecutableIdentity = {
        plan.invocation.oldProcess.imagePath,
        plan.invocation.oldProcess.imageVolumeSerial,
        plan.invocation.oldProcess.imageFileId,
        true};
    plan.invocation.deploymentMode = plan.identity.deploymentMode;
    plan.invocation.installationId = plan.identity.installationId;
    plan.invocation.targetRootIdentityBinding =
        plan.identity.targetRootIdentity.canonicalPath;
    plan.invocation.oldExecutableIdentityBinding =
        plan.identity.oldExecutableIdentity.canonicalPath;
    plan.entrypoint = "HarkenSearch.exe";
    plan.inventory = {
        {"HarkenSearch.exe", std::string(64, 'd'), 1024},
        {"resources\\core.pak", std::string(64, 'e'), 2048},
    };
    return plan;
}

void rebindDeploymentIdentity(SignedInstallPlan& plan,
                              const std::string& installRoot) {
    plan.identity.targetRoot = installRoot;
    plan.identity.targetRootIdentity.canonicalPath = installRoot;
#if defined(_WIN32)
    std::error_code rootError;
    if (std::filesystem::is_directory(
            std::filesystem::path(installRoot), rootError) &&
        !rootError) {
        auto observer = CreateNativeWindowsBootstrapFilesystemBackend();
        WindowsFilesystemHandle rootHandle = 0;
        WindowsFileIdentity openedRoot;
        if (observer != nullptr &&
            observer->OpenRootExact(
                installRoot, WindowsFilesystemRootRole::Install,
                rootHandle, openedRoot).ok() &&
            rootHandle != 0 && openedRoot.valid) {
            plan.identity.targetRootIdentity.volumeSerial =
                openedRoot.volumeSerial;
            plan.identity.targetRootIdentity.fileId = openedRoot.fileId;
            plan.identity.targetRootIdentity.valid = true;
            observer->CloseFilesystemHandle(rootHandle);
        }
    }
#endif
    const std::string executable =
        (std::filesystem::path(installRoot) / "HarkenSearch.exe").u8string();
    plan.invocation.parentProcess.imagePath = executable;
    plan.invocation.oldProcess.imagePath = executable;
    plan.identity.oldExecutableIdentity.canonicalPath = executable;
    plan.invocation.targetRootIdentityBinding = installRoot;
    plan.invocation.oldExecutableIdentityBinding = executable;
}

struct Fixture {
    ValidatedInstallPlan plan;
    ReplacementJournal journal;
};

Fixture fixture() {
    const auto validated = ValidateSignedInstallPlan(rawPlan(), SignatureStatus::Verified);
    const auto journal = validated.plan
        ? BuildReplacementJournal(*validated.plan, {{"HarkenSearch.exe"}, {"legacy.dll"}},
                                  "attempt-fs-01")
        : aris::bootstrap::JournalBuildResult{};
    if (!validated.plan || !journal.journal) {
        check(false, "filesystem fixture core values validate");
        return {};
    }
    return {*validated.plan, *journal.journal};
}

Fixture removalFixture() {
    SignedInstallPlan plan = rawPlan();
    plan.removals = {"legacy.dll"};
    const auto validated =
        ValidateSignedInstallPlan(plan, SignatureStatus::Verified);
    const auto journal = validated.plan
        ? BuildReplacementJournal(*validated.plan,
                                  {{"HarkenSearch.exe"}},
                                  "attempt-fs-01")
        : aris::bootstrap::JournalBuildResult{};
    if (!validated.plan || !journal.journal) {
        check(false, "signed-removal fixture core values validate");
        return {};
    }
    return {*validated.plan, *journal.journal};
}

Fixture crashFixture(const std::string& installRoot,
                     const std::string& stagingRoot,
                     const std::string& transactionRoot) {
    SignedInstallPlan plan = rawPlan();
    rebindDeploymentIdentity(plan, installRoot);
    const std::filesystem::path stagingPath(stagingRoot);
    plan.invocation.updaterOwnedBase =
        stagingPath.parent_path().u8string();
    plan.invocation.stagingRoot = stagingRoot;
    plan.invocation.transactionRoot = transactionRoot;
    plan.invocation.invocationIdentity = "signed-invocation-01";
    plan.invocation.transactionId = "attempt-crash-01";
    plan.invocation.packagePlanDigest = plan.packagePlanDigest;
    plan.invocation.parentProcess = processIdentity(99U, 0x31U);
    plan.invocation.oldProcess = plan.invocation.parentProcess;
    rebindDeploymentIdentity(plan, installRoot);
    plan.inventory = {{"HarkenSearch.exe",
                       "cc738d1c75a1f105f18f5fe4ed9aa8defa4f8ad15fc64cfbefe2e2f0da259da5",
                       10}};
    const auto validated =
        ValidateSignedInstallPlan(plan, SignatureStatus::Verified);
    const auto journal = validated.plan
        ? BuildReplacementJournal(*validated.plan, {}, "attempt-crash-01")
        : aris::bootstrap::JournalBuildResult{};
    if (!validated.plan || !journal.journal) {
        check(false, "crash-cut fixture core values validate");
        return {};
    }
    return {*validated.plan, *journal.journal};
}

Fixture replacementCrashFixture(const std::string& installRoot,
                                const std::string& stagingRoot,
                                const std::string& transactionRoot) {
    SignedInstallPlan plan = rawPlan();
    rebindDeploymentIdentity(plan, installRoot);
    const std::filesystem::path stagingPath(stagingRoot);
    plan.invocation.updaterOwnedBase =
        stagingPath.parent_path().u8string();
    plan.invocation.stagingRoot = stagingRoot;
    plan.invocation.transactionRoot = transactionRoot;
    plan.invocation.invocationIdentity = "signed-invocation-01";
    plan.invocation.transactionId = "attempt-replacement-crash-01";
    plan.invocation.packagePlanDigest = plan.packagePlanDigest;
    plan.invocation.parentProcess = processIdentity(99U, 0x31U);
    plan.invocation.oldProcess = plan.invocation.parentProcess;
    rebindDeploymentIdentity(plan, installRoot);
    plan.inventory = {{"HarkenSearch.exe",
                       "cc738d1c75a1f105f18f5fe4ed9aa8defa4f8ad15fc64cfbefe2e2f0da259da5",
                       10}};
    const auto validated =
        ValidateSignedInstallPlan(plan, SignatureStatus::Verified);
    const auto journal = validated.plan
        ? BuildReplacementJournal(*validated.plan, {{"HarkenSearch.exe"}},
                                  "attempt-replacement-crash-01")
        : aris::bootstrap::JournalBuildResult{};
    if (!validated.plan || !journal.journal) {
        check(false, "replacement crash-cut fixture core values validate");
        return {};
    }
    return {*validated.plan, *journal.journal};
}

void applyInstalledFixture(WindowsBootstrapFilesystemPort& port);

BootstrapIntent intent(BootstrapIntentKind kind,
                       const std::string& relativePath = {},
                       const std::string& reason = {}) {
    BootstrapIntent value;
    value.kind = kind;
    value.relativePath = relativePath;
    value.reason = reason;
    return value;
}

bool containsIntent(const std::vector<BootstrapIntent>& intents,
                    const BootstrapIntentKind kind,
                    const std::string& relativePath = {}) {
    return std::any_of(
        intents.begin(), intents.end(),
        [kind, &relativePath](const BootstrapIntent& candidate) {
            return candidate.kind == kind &&
                   (relativePath.empty() ||
                    candidate.relativePath == relativePath);
        });
}

enum class CallKind {
    OpenRoot,
    CreateDirectory,
    Observe,
    PersistJournal,
    AcquireJournalLease,
    ReleaseJournalLease,
    CopyFile,
    ReplaceFile,
    DeleteFile,
    CleanupInvocation,
    RetireJournal,
    RemoveEmptyDirectory,
    CloseHandle,
};

struct Call {
    CallKind kind = CallKind::Observe;
    WindowsFilesystemHandle sourceRoot = 0;
    WindowsFilesystemHandle targetRoot = 0;
    std::string sourcePath;
    std::string targetPath;
    std::string temporaryLeaf;
    WindowsFileIdentity expectedSource;
    WindowsFileIdentity expectedTarget;
    WindowsFileIdentity expectedTargetParent;
    WindowsFileContentFingerprint expectedSourceContent;
    WindowsFileContentFingerprint expectedTargetContent;
    WindowsFileMetadataSnapshot expectedSourceMetadata;
    WindowsFileMetadataSnapshot expectedTargetMetadata;
    std::vector<WindowsBootstrapInvocationCleanupFile> stagingFiles;
    std::vector<WindowsBootstrapInvocationCleanupFile> transactionFiles;
    WindowsFilesystemReplaceMode replaceMode = WindowsFilesystemReplaceMode::CreateNew;
};

class ScriptedBackend final : public WindowsBootstrapFilesystemBackend {
public:
    static constexpr WindowsFilesystemHandle kInstall = 11;
    static constexpr WindowsFilesystemHandle kStaging = 12;
    static constexpr WindowsFilesystemHandle kTransaction = 13;
    static constexpr WindowsFilesystemHandle kBackup = 14;
    static constexpr WindowsFilesystemHandle kJournalLease = 15;

    std::vector<Call> calls;
    std::map<std::string, WindowsFilesystemObservation> entries;
    std::map<std::string, WindowsFileContentFingerprint> contents;
    std::map<std::string, WindowsFileMetadataSnapshot> metadatas;
    std::map<WindowsFilesystemRootRole, WindowsFilesystemBackendStatus> rootFailures;
    std::map<WindowsFilesystemRootRole, WindowsFileIdentity> rootIdentities;
    int persistCalls = 0;
    int promotionCalls = 0;
    bool recoveryPersistedBeforePromotion = false;
    int failPersistCall = 0;
    int persistAccessDeniedCall = 0;
    int failRetireCall = 0;
    int retireAccessDeniedFailuresRemaining = 0;
    int retireCalls = 0;
    std::vector<std::string> retiredJournalNames;
    bool backupDirectoryAlreadyExists = false;
    bool backupDirectoryEmpty = true;
    bool requireJournalLease = false;
    bool journalLeaseHeld = false;
    bool failJournalLease = false;
    int journalLeaseAccessDeniedFailuresRemaining = 0;
    int readJournalAccessDeniedFailuresRemaining = 0;
    int readJournalCalls = 0;
    int createDirectoryAccessDeniedFailuresRemaining = 0;
    bool temporaryCollisionOnReplace = false;
    bool failAfterReplacementPrepared = false;
    int copyAccessDeniedFailuresRemaining = 0;
    int replaceAccessDeniedFailuresRemaining = 0;
    int deleteAccessDeniedFailuresRemaining = 0;
    int removeDirectoryAccessDeniedFailuresRemaining = 0;
    int backupObserveAccessDeniedFailuresRemaining = 0;
    bool rejectTrustedRoots = false;
    bool unknownBackupEntry = false;
    bool injectUnknownBackupEntryAfterFirstValidation = false;
    bool backupValidationInjectionDone = false;
    bool rejectPathValidationAfterHandleBound = false;
    bool backupHandleBound = false;
    bool failInvocationCleanup = false;
    WindowsFilesystemBackendResult invocationCleanupFailure{
        WindowsFilesystemBackendStatus::AccessDenied, 5U};
    int boundBackupValidationCalls = 0;
    int unleasedMutationCalls = 0;
    std::vector<std::uint8_t> lastJournalBytes;
    // Retain the two newest durable generations so restart fixtures model the
    // real two-slot namespace and can authenticate predecessor linkage.
    std::vector<WindowsBootstrapJournalCandidate> journalHistory;
    std::vector<WindowsBootstrapJournalCandidate> durableJournalCandidates;

    ScriptedBackend() {
        rootIdentities[WindowsFilesystemRootRole::Install] = identity(0x42U);
        set(WindowsFilesystemRootRole::Staging, "HarkenSearch.exe",
            WindowsFilesystemEntryKind::RegularFile, identity(31));
        set(WindowsFilesystemRootRole::Staging, "resources/core.pak",
            WindowsFilesystemEntryKind::RegularFile, identity(32));
        set(WindowsFilesystemRootRole::Staging, "resources",
            WindowsFilesystemEntryKind::Directory, identity(33));
        set(WindowsFilesystemRootRole::Install, "HarkenSearch.exe",
            WindowsFilesystemEntryKind::RegularFile, identity(21));
        set(WindowsFilesystemRootRole::Install, "legacy.dll",
            WindowsFilesystemEntryKind::RegularFile, identity(22));
        set(WindowsFilesystemRootRole::Install, "resources/core.pak",
            WindowsFilesystemEntryKind::Missing, {});
        set(WindowsFilesystemRootRole::Install, "resources",
            WindowsFilesystemEntryKind::Directory, identity(23));
        set(WindowsFilesystemRootRole::Backup, "HarkenSearch.exe",
            WindowsFilesystemEntryKind::Missing, {});
        set(WindowsFilesystemRootRole::Backup, "resources/core.pak",
            WindowsFilesystemEntryKind::Missing, {});
        set(WindowsFilesystemRootRole::Backup, "legacy.dll",
            WindowsFilesystemEntryKind::Missing, {});
        setContentByHandle(kStaging, "HarkenSearch.exe", fingerprint(0xdd, 1024));
        setContentByHandle(kStaging, "resources/core.pak", fingerprint(0xee, 2048));
        setContentByHandle(kInstall, "HarkenSearch.exe", fingerprint(0x11, 900));
        setContentByHandle(kInstall, "legacy.dll", fingerprint(0x12, 300));
        setMetadataByHandle(kStaging, "HarkenSearch.exe", metadata(0xdd));
        setMetadataByHandle(kStaging, "resources/core.pak", metadata(0xee));
        setMetadataByHandle(kInstall, "HarkenSearch.exe", metadata(0x11));
        setMetadataByHandle(kInstall, "legacy.dll", metadata(0x12));
    }

    WindowsFilesystemBackendResult OpenRootExact(
        const std::string& path,
        WindowsFilesystemRootRole role,
        WindowsFilesystemHandle& outputHandle,
        WindowsFileIdentity& outputIdentity) override {
        Call call;
        call.kind = CallKind::OpenRoot;
        call.sourcePath = path;
        calls.push_back(call);
        const auto failure = rootFailures.find(role);
        if (failure != rootFailures.end()) return {failure->second, 9001};
        outputHandle = handle(role);
        const auto configuredIdentity = rootIdentities.find(role);
        outputIdentity = configuredIdentity == rootIdentities.end()
            ? identity(static_cast<std::uint8_t>(40 + static_cast<int>(role)))
            : configuredIdentity->second;
        return {};
    }

    WindowsFilesystemBackendResult OpenOrCreateDirectoryNoFollow(
        WindowsFilesystemHandle parent,
        const std::string& relativePath,
        bool requireNew,
        WindowsFilesystemHandle& outputHandle,
        WindowsFileIdentity& outputIdentity) override {
        Call call;
        call.kind = CallKind::CreateDirectory;
        call.sourceRoot = parent;
        call.targetPath = relativePath;
        calls.push_back(call);
        if (requireJournalLease && !journalLeaseHeld) {
            ++unleasedMutationCalls;
            return {WindowsFilesystemBackendStatus::AccessDenied, 32};
        }
        if (createDirectoryAccessDeniedFailuresRemaining > 0) {
            --createDirectoryAccessDeniedFailuresRemaining;
            return {WindowsFilesystemBackendStatus::AccessDenied, 5};
        }
        if (requireNew && backupDirectoryAlreadyExists) {
            return {WindowsFilesystemBackendStatus::AlreadyExists, 183};
        }
        if (!requireNew && !backupDirectoryAlreadyExists) {
            return {WindowsFilesystemBackendStatus::NotFound, 2};
        }
        if (requireNew) backupDirectoryAlreadyExists = true;
        outputHandle = kBackup;
        outputIdentity = identity(44);
        backupHandleBound = true;
        return {};
    }

    WindowsFilesystemBackendResult ObserveNoFollow(
        WindowsFilesystemHandle root,
        const std::string& relativePath,
        WindowsFilesystemObservation& output) override {
        Call call;
        call.kind = CallKind::Observe;
        call.sourceRoot = root;
        call.sourcePath = relativePath;
        calls.push_back(call);
        if (root == kTransaction && relativePath == "backup" &&
            backupObserveAccessDeniedFailuresRemaining > 0) {
            --backupObserveAccessDeniedFailuresRemaining;
            return {WindowsFilesystemBackendStatus::AccessDenied, 5};
        }
        if (root == kTransaction && relativePath == "backup" &&
            backupDirectoryAlreadyExists) {
            output = {WindowsFilesystemEntryKind::Directory, identity(44)};
            return {};
        }
        WindowsFilesystemHandle observationRoot = root;
        std::string observationPath = relativePath;
        if (root == kTransaction && relativePath.rfind("backup/", 0U) == 0U) {
            observationRoot = kBackup;
            observationPath = relativePath.substr(7U);
        }
        const auto found = entries.find(key(observationRoot, observationPath));
        output = found == entries.end()
            ? WindowsFilesystemObservation{}
            : found->second;
        if (output.kind == WindowsFilesystemEntryKind::RegularFile &&
            observationRoot != kStaging) {
            output.metadata = lookupMetadata(observationRoot, observationPath);
        }
        if (output.kind == WindowsFilesystemEntryKind::RegularFile) {
            output.content = lookupContent(observationRoot, observationPath);
        }
        return {};
    }

    WindowsFilesystemBackendResult CopyFileDurableNoFollow(
        WindowsFilesystemHandle sourceRoot,
        const std::string& sourcePath,
        const WindowsFileIdentity& expectedSource,
        WindowsFilesystemHandle targetRoot,
        const std::string& targetPath,
        WindowsFileIdentity& outputIdentity,
        WindowsFileContentFingerprint& outputFingerprint,
        WindowsFileMetadataSnapshot& outputMetadata) override {
        Call call;
        call.kind = CallKind::CopyFile;
        call.sourceRoot = sourceRoot;
        call.targetRoot = targetRoot;
        call.sourcePath = sourcePath;
        call.targetPath = targetPath;
        call.expectedSource = expectedSource;
        calls.push_back(call);
        if (requireJournalLease && !journalLeaseHeld) {
            ++unleasedMutationCalls;
            return {WindowsFilesystemBackendStatus::AccessDenied, 32};
        }
        if (copyAccessDeniedFailuresRemaining > 0) {
            --copyAccessDeniedFailuresRemaining;
            return {WindowsFilesystemBackendStatus::AccessDenied, 32};
        }
        const auto source = lookup(sourceRoot, sourcePath);
        if (source.kind != WindowsFilesystemEntryKind::RegularFile ||
            source.identity != expectedSource) {
            return {WindowsFilesystemBackendStatus::IdentityMismatch, 1001};
        }
        if (lookup(targetRoot, targetPath).kind != WindowsFilesystemEntryKind::Missing) {
            return {WindowsFilesystemBackendStatus::AlreadyExists, 183};
        }
        outputIdentity = identity(nextIdentity_++);
        outputFingerprint = lookupContent(sourceRoot, sourcePath);
        outputMetadata = lookupMetadata(sourceRoot, sourcePath);
        if (!outputFingerprint.valid || !outputMetadata.valid) {
            return {WindowsFilesystemBackendStatus::ContentMismatch, 1007};
        }
        entries[key(targetRoot, targetPath)] = {
            WindowsFilesystemEntryKind::RegularFile, outputIdentity};
        contents[key(targetRoot, targetPath)] = outputFingerprint;
        metadatas[key(targetRoot, targetPath)] = metadata(nextMetadata_++);
        return {};
    }

    WindowsFilesystemBackendResult ReplaceFileDurableNoFollow(
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
        WindowsFileMetadataSnapshot& outputMetadata) override {
        return ReplaceFileDurableNoFollowWithCheckpoint(
            sourceRoot, sourcePath, expectedSource, expectedSourceContent,
            expectedSourceMetadata, targetRoot, targetPath,
            expectedTargetParent, temporaryLeaf, mode, expectedTarget,
            expectedTargetContent, expectedTargetMetadata, {}, {}, {}, nullptr,
            outputIdentity, outputMetadata);
    }

    WindowsFilesystemBackendResult ReplaceFileDurableNoFollowWithCheckpoint(
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
        WindowsFileMetadataSnapshot& outputMetadata) override {
        Call call;
        call.kind = CallKind::ReplaceFile;
        call.sourceRoot = sourceRoot;
        call.targetRoot = targetRoot;
        call.sourcePath = sourcePath;
        call.targetPath = targetPath;
        call.temporaryLeaf = temporaryLeaf;
        call.expectedSource = expectedSource;
        call.expectedSourceContent = expectedSourceContent;
        call.expectedTarget = expectedTarget;
        call.expectedTargetParent = expectedTargetParent;
        call.expectedTargetContent = expectedTargetContent;
        call.expectedSourceMetadata = expectedSourceMetadata;
        call.expectedTargetMetadata = expectedTargetMetadata;
        call.replaceMode = mode;
        calls.push_back(call);
        if (requireJournalLease && !journalLeaseHeld) {
            ++unleasedMutationCalls;
            return {WindowsFilesystemBackendStatus::AccessDenied, 32};
        }
        if (replaceAccessDeniedFailuresRemaining > 0) {
            --replaceAccessDeniedFailuresRemaining;
            return {WindowsFilesystemBackendStatus::AccessDenied, 32};
        }
        if (parentIdentity(targetRoot, targetPath) != expectedTargetParent) {
            return {WindowsFilesystemBackendStatus::IdentityMismatch, 1003};
        }
        if (temporaryCollisionOnReplace) {
            return {WindowsFilesystemBackendStatus::AlreadyExists, 183};
        }
        const auto source = lookup(sourceRoot, sourcePath);
        const auto target = lookup(targetRoot, targetPath);
        if (source.kind != WindowsFilesystemEntryKind::RegularFile ||
            source.identity != expectedSource) {
            return {WindowsFilesystemBackendStatus::IdentityMismatch, 1002};
        }
        const auto sourceContent = lookupContent(sourceRoot, sourcePath);
        if (!sourceContent.valid || sourceContent != expectedSourceContent) {
            return {WindowsFilesystemBackendStatus::ContentMismatch, 1008};
        }
        if (mode == WindowsFilesystemReplaceMode::CreateNew) {
            if (target.kind != WindowsFilesystemEntryKind::Missing) {
                return {WindowsFilesystemBackendStatus::AlreadyExists, 183};
            }
        } else if (target.kind != WindowsFilesystemEntryKind::RegularFile ||
                   target.identity != expectedTarget) {
            return {WindowsFilesystemBackendStatus::IdentityMismatch, 1003};
        }
        if (mode == WindowsFilesystemReplaceMode::CreateNew) {
            if (expectedTargetContent.valid || expectedTargetMetadata.valid) {
                return {WindowsFilesystemBackendStatus::ContentMismatch, 1009};
            }
        } else {
            if (lookupContent(targetRoot, targetPath) != expectedTargetContent) {
                return {WindowsFilesystemBackendStatus::ContentMismatch, 1010};
            }
            if (lookupMetadata(targetRoot, targetPath) != expectedTargetMetadata) {
                return {WindowsFilesystemBackendStatus::UnsupportedMetadata, 50};
            }
        }
        if (sourceRoot == kBackup && !expectedSourceMetadata.valid) {
            return {WindowsFilesystemBackendStatus::UnsupportedMetadata, 50};
        }
        const std::size_t separator = targetPath.find_last_of('/');
        const std::string temporaryPath = separator == std::string::npos
            ? temporaryLeaf
            : targetPath.substr(0, separator + 1U) + temporaryLeaf;
        WindowsFileIdentity preparedIdentity = expectedPreparedIdentity;
        WindowsFileContentFingerprint preparedContent = expectedPreparedContent;
        WindowsFileMetadataSnapshot preparedMetadata = expectedPreparedMetadata;
        if (expectedPreparedIdentity.valid) {
            const auto prepared = lookup(targetRoot, temporaryPath);
            if (prepared.kind != WindowsFilesystemEntryKind::RegularFile ||
                prepared.identity != expectedPreparedIdentity ||
                lookupContent(targetRoot, temporaryPath) !=
                    expectedPreparedContent ||
                lookupMetadata(targetRoot, temporaryPath) !=
                    expectedPreparedMetadata) {
                return {WindowsFilesystemBackendStatus::IdentityMismatch, 1013};
            }
        } else {
            preparedIdentity = identity(nextPublicationIdentity_++);
            preparedContent = sourceContent;
            preparedMetadata = sourceRoot == kBackup
                ? expectedSourceMetadata
                : mode == WindowsFilesystemReplaceMode::CreateNew
                    ? metadata(nextMetadata_++)
                    : expectedTargetMetadata;
            entries[key(targetRoot, temporaryPath)] = {
                WindowsFilesystemEntryKind::RegularFile, preparedIdentity};
            contents[key(targetRoot, temporaryPath)] = preparedContent;
            metadatas[key(targetRoot, temporaryPath)] = preparedMetadata;
            if (checkpoint != nullptr && checkpoint->onPrepared != nullptr) {
                const WindowsFilesystemBackendResult checkpointed =
                    checkpoint->onPrepared(
                        checkpoint->context, preparedIdentity, preparedContent,
                        preparedMetadata);
                if (!checkpointed.ok()) {
                    entries[key(targetRoot, temporaryPath)] = {};
                    contents.erase(key(targetRoot, temporaryPath));
                    metadatas.erase(key(targetRoot, temporaryPath));
                    return checkpointed;
                }
                if (failAfterReplacementPrepared) {
                    return {WindowsFilesystemBackendStatus::DurabilityFailed,
                            1008};
                }
            }
        }
        outputIdentity = preparedIdentity;
        outputMetadata = preparedMetadata;
        entries[key(targetRoot, temporaryPath)] = {};
        contents.erase(key(targetRoot, temporaryPath));
        metadatas.erase(key(targetRoot, temporaryPath));
        entries[key(targetRoot, targetPath)] = {
            WindowsFilesystemEntryKind::RegularFile, outputIdentity};
        contents[key(targetRoot, targetPath)] = preparedContent;
        metadatas[key(targetRoot, targetPath)] = outputMetadata;
        return {};
    }

    WindowsFilesystemBackendResult DeleteFileDurableNoFollow(
        WindowsFilesystemHandle root,
        const std::string& relativePath,
        const WindowsFileIdentity& expectedParent,
        const WindowsFileIdentity& expectedIdentity,
        const WindowsFileContentFingerprint& expectedContent,
        const WindowsFileMetadataSnapshot& expectedMetadata) override {
        Call call;
        call.kind = CallKind::DeleteFile;
        call.sourceRoot = root;
        call.sourcePath = relativePath;
        call.expectedTargetParent = expectedParent;
        call.expectedSource = expectedIdentity;
        call.expectedSourceContent = expectedContent;
        call.expectedSourceMetadata = expectedMetadata;
        calls.push_back(call);
        if (requireJournalLease && !journalLeaseHeld) {
            ++unleasedMutationCalls;
            return {WindowsFilesystemBackendStatus::AccessDenied, 32};
        }
        if (deleteAccessDeniedFailuresRemaining > 0) {
            --deleteAccessDeniedFailuresRemaining;
            return {WindowsFilesystemBackendStatus::AccessDenied, 32};
        }
        if (parentIdentity(root, relativePath) != expectedParent) {
            return {WindowsFilesystemBackendStatus::IdentityMismatch, 1004};
        }
        const auto existing = lookup(root, relativePath);
        if (existing.kind != WindowsFilesystemEntryKind::RegularFile ||
            existing.identity != expectedIdentity) {
            return {WindowsFilesystemBackendStatus::IdentityMismatch, 1004};
        }
        if (lookupContent(root, relativePath) != expectedContent) {
            return {WindowsFilesystemBackendStatus::ContentMismatch, 1011};
        }
        if (expectedMetadata.valid &&
            lookupMetadata(root, relativePath) != expectedMetadata) {
            return {WindowsFilesystemBackendStatus::UnsupportedMetadata, 50};
        }
        entries[key(root, relativePath)] = {};
        contents.erase(key(root, relativePath));
        metadatas.erase(key(root, relativePath));
        return {};
    }

    WindowsFilesystemBackendResult CleanupInvocationResidueNoFollow(
        WindowsFilesystemHandle stagingRoot,
        WindowsFilesystemHandle transactionRoot,
        const WindowsBootstrapInvocationCleanup& cleanup) override {
        Call call;
        call.kind = CallKind::CleanupInvocation;
        call.sourceRoot = stagingRoot;
        call.targetRoot = transactionRoot;
        call.stagingFiles = cleanup.stagingFiles;
        call.transactionFiles = cleanup.transactionFiles;
        calls.push_back(call);
        if (failInvocationCleanup) return invocationCleanupFailure;
        bool runtimePairMissing = false;
        if (!cleanup.runtimePairExpected) {
            const auto directory = lookup(transactionRoot, "runtime-pair");
            if (directory.kind != WindowsFilesystemEntryKind::Missing) {
                return {WindowsFilesystemBackendStatus::IdentityMismatch, 1008};
            }
        } else {
            const auto directory = lookup(transactionRoot, "runtime-pair");
            if (directory.kind == WindowsFilesystemEntryKind::Missing) {
                if (!cleanup.allowAlreadyRemoved) {
                    return {WindowsFilesystemBackendStatus::IdentityMismatch,
                            1003};
                }
                if (lookup(transactionRoot, "runtime-pair/HarkenSearchBootstrap.exe").kind !=
                    WindowsFilesystemEntryKind::Missing) {
                    return {WindowsFilesystemBackendStatus::IdentityMismatch,
                            1009};
                }
                runtimePairMissing = true;
            } else if (directory.kind != WindowsFilesystemEntryKind::Directory ||
                       directory.identity != cleanup.runtimePairDirectoryIdentity) {
                return {WindowsFilesystemBackendStatus::IdentityMismatch, 1004};
            }
            const auto bootstrap = lookup(
                transactionRoot, "runtime-pair/HarkenSearchBootstrap.exe");
            if (bootstrap.kind == WindowsFilesystemEntryKind::Missing) {
                if (!cleanup.allowAlreadyRemoved) {
                    return {WindowsFilesystemBackendStatus::IdentityMismatch,
                            1005};
                }
            } else if (bootstrap.kind != WindowsFilesystemEntryKind::RegularFile ||
                       bootstrap.identity != cleanup.runtimePairBootstrapIdentity) {
                return {WindowsFilesystemBackendStatus::IdentityMismatch, 1006};
            } else if (lookupContent(transactionRoot,
                                      "runtime-pair/HarkenSearchBootstrap.exe") !=
                       cleanup.runtimePairBootstrapContent) {
                return {WindowsFilesystemBackendStatus::ContentMismatch, 1007};
            }
        }
        const auto removeKnown = [this](
                                      const WindowsFilesystemHandle root,
                                      const std::vector<WindowsBootstrapInvocationCleanupFile>& files,
                                      const bool allowAlreadyRemoved,
                                      const bool allowMissingRuntimePairParent,
                                      const bool deleteFiles) {
            for (const auto& file : files) {
                const std::size_t separator = file.relativePath.find_last_of('/');
                if (separator != std::string::npos) {
                    const auto parent = lookup(
                        root, file.relativePath.substr(0, separator));
                    if (allowMissingRuntimePairParent &&
                        file.relativePath == "runtime-pair/HarkenSearchBootstrap.exe" &&
                        parent.kind == WindowsFilesystemEntryKind::Missing) {
                        if (lookup(root, file.relativePath).kind !=
                            WindowsFilesystemEntryKind::Missing) {
                            return WindowsFilesystemBackendResult{
                                WindowsFilesystemBackendStatus::IdentityMismatch,
                                1010};
                        }
                        continue;
                    }
                    if (parent.kind != WindowsFilesystemEntryKind::Directory) {
                        return WindowsFilesystemBackendResult{
                            WindowsFilesystemBackendStatus::IdentityMismatch,
                            1003};
                    }
                }
                const auto observed = lookup(root, file.relativePath);
                if (observed.kind == WindowsFilesystemEntryKind::Missing) {
                    if (allowAlreadyRemoved) continue;
                    return WindowsFilesystemBackendResult{
                        WindowsFilesystemBackendStatus::IdentityMismatch, 1001};
                }
                if (observed.kind != WindowsFilesystemEntryKind::RegularFile ||
                    observed.identity != file.identity) {
                    return WindowsFilesystemBackendResult{
                        WindowsFilesystemBackendStatus::IdentityMismatch, 1001};
                }
                if (lookupContent(root, file.relativePath) != file.content) {
                    return WindowsFilesystemBackendResult{
                        WindowsFilesystemBackendStatus::ContentMismatch, 1002};
                }
                if (root == kStaging &&
                    !lookupMetadata(root, file.relativePath).valid) {
                        return WindowsFilesystemBackendResult{
                            WindowsFilesystemBackendStatus::UnsupportedMetadata, 50};
                    }
            }
            if (!deleteFiles) return WindowsFilesystemBackendResult{};
            for (const auto& file : files) {
                if (lookup(root, file.relativePath).kind ==
                    WindowsFilesystemEntryKind::Missing) {
                    continue;
                }
                entries[key(root, file.relativePath)] = {};
                contents.erase(key(root, file.relativePath));
                metadatas.erase(key(root, file.relativePath));
            }
            return WindowsFilesystemBackendResult{};
        };
        const auto stagingPreflight = removeKnown(
            stagingRoot, cleanup.stagingFiles, cleanup.allowAlreadyRemoved,
            false, false);
        if (!stagingPreflight.ok()) return stagingPreflight;
        const auto transactionPreflight = removeKnown(
            transactionRoot, cleanup.transactionFiles,
            cleanup.allowAlreadyRemoved, runtimePairMissing, false);
        if (!transactionPreflight.ok()) return transactionPreflight;
        const auto staging = removeKnown(stagingRoot, cleanup.stagingFiles,
                                         cleanup.allowAlreadyRemoved, false,
                                         true);
        if (!staging.ok()) return staging;
        const auto transaction = removeKnown(
            transactionRoot, cleanup.transactionFiles,
            cleanup.allowAlreadyRemoved, runtimePairMissing, true);
        if (!transaction.ok()) return transaction;
        if (cleanup.runtimePairExpected &&
            lookup(transactionRoot, "runtime-pair").kind ==
                WindowsFilesystemEntryKind::Directory) {
            entries[key(transactionRoot, "runtime-pair")] = {};
        }
        return {};
    }

    WindowsFilesystemBackendResult PersistJournalDurable(
        WindowsFilesystemHandle root,
        const std::vector<std::uint8_t>& bytes,
        WindowsFilesystemReplaceMode mode,
        const WindowsFileIdentity& expectedIdentity,
        const WindowsFileContentFingerprint& expectedContent,
        WindowsFileIdentity& outputIdentity,
        WindowsFileContentFingerprint& outputContent) override {
        Call call;
        call.kind = CallKind::PersistJournal;
        call.sourceRoot = root;
        call.replaceMode = mode;
        call.expectedTarget = expectedIdentity;
        call.expectedTargetContent = expectedContent;
        calls.push_back(call);
        ++persistCalls;
        if (!durableJournalCandidates.empty() && promotionCalls == 0) {
            recoveryPersistedBeforePromotion = true;
        }
        if (persistAccessDeniedCall != 0 &&
            persistCalls == persistAccessDeniedCall) {
            return {WindowsFilesystemBackendStatus::AccessDenied, 5};
        }
        if (failPersistCall != 0 && persistCalls == failPersistCall) {
            return {WindowsFilesystemBackendStatus::DurabilityFailed, 1005};
        }
        const auto current = lookup(root, "aris-bootstrap.journal");
        if (mode == WindowsFilesystemReplaceMode::CreateNew) {
            if (current.kind != WindowsFilesystemEntryKind::Missing ||
                expectedContent.valid) {
                return {WindowsFilesystemBackendStatus::AlreadyExists, 183};
            }
        } else if (current.kind != WindowsFilesystemEntryKind::RegularFile ||
                   current.identity != expectedIdentity) {
            return {WindowsFilesystemBackendStatus::IdentityMismatch, 1006};
        } else if (lookupContent(root, "aris-bootstrap.journal") !=
                   expectedContent) {
            return {WindowsFilesystemBackendStatus::ContentMismatch, 1012};
        }
        outputIdentity = identity(nextIdentity_++);
        outputContent = fingerprint(static_cast<std::uint8_t>(0x80 + persistCalls),
                                    static_cast<std::uint64_t>(bytes.size()));
        entries[key(root, "aris-bootstrap.journal")] = {
            WindowsFilesystemEntryKind::RegularFile, outputIdentity};
        contents[key(root, "aris-bootstrap.journal")] = outputContent;
        WindowsBootstrapJournalCandidate candidate;
        const auto decoded = DecodeWindowsBootstrapJournal(bytes);
        const std::uint64_t generation = decoded.record.has_value()
            ? decoded.record->generation
            : static_cast<std::uint64_t>(persistCalls);
        candidate.name = (generation % 2U) == 0U
            ? "aris-bootstrap.journal.b"
            : "aris-bootstrap.journal.a";
        candidate.bytes = bytes;
        candidate.durable = true;
        candidate.temporary = false;
        candidate.identity = outputIdentity;
        candidate.content = outputContent;
        journalHistory.push_back(std::move(candidate));
        if (journalHistory.size() > 2U) {
            journalHistory.erase(journalHistory.begin(),
                                 journalHistory.end() - 2);
        }
        lastJournalBytes = bytes;
        return {};
    }

    WindowsFilesystemBackendResult ValidateTrustedWriteRoots(
        const ValidatedInstallPlan&,
        const WindowsBootstrapFilesystemRoots&) override {
        // The scripted backend represents an already-authenticated updater
        // capability; native policy tests exercise the rejecting boundary.
        return rejectTrustedRoots
            ? WindowsFilesystemBackendResult{
                  WindowsFilesystemBackendStatus::AccessDenied, 5}
            : WindowsFilesystemBackendResult{};
    }

    WindowsFilesystemBackendResult ReadJournalCandidatesNoFollow(
        WindowsFilesystemHandle root,
        std::vector<WindowsBootstrapJournalCandidate>& output) override {
        output.clear();
        if (root != kTransaction) {
            return {WindowsFilesystemBackendStatus::AccessDenied, 6};
        }
        ++readJournalCalls;
        if (readJournalAccessDeniedFailuresRemaining > 0) {
            --readJournalAccessDeniedFailuresRemaining;
            return {WindowsFilesystemBackendStatus::AccessDenied, 5};
        }
        output = durableJournalCandidates;
        return {};
    }

    WindowsFilesystemBackendResult RetireJournalCandidateDurable(
        WindowsFilesystemHandle root,
        const WindowsBootstrapJournalCandidate& candidate) override {
        Call call;
        call.kind = CallKind::RetireJournal;
        call.sourceRoot = root;
        call.sourcePath = candidate.name;
        calls.push_back(call);
        ++retireCalls;
        if (root != kTransaction || !candidate.durable || candidate.temporary ||
            (candidate.name != "aris-bootstrap.journal.a" &&
             candidate.name != "aris-bootstrap.journal.b")) {
            return {WindowsFilesystemBackendStatus::IdentityMismatch, 13};
        }
        if (lookup(root, "runtime-pair").kind !=
            WindowsFilesystemEntryKind::Missing) {
            return {WindowsFilesystemBackendStatus::IdentityMismatch, 1008};
        }
        const auto backupAbsent = ValidateBackupNamespaceNoFollow(
            root, {}, {}, false, {});
        if (!backupAbsent.ok()) return backupAbsent;
        const auto found = std::find_if(
            durableJournalCandidates.begin(), durableJournalCandidates.end(),
            [&candidate](const WindowsBootstrapJournalCandidate& observed) {
                return observed.name == candidate.name &&
                       observed.identity == candidate.identity &&
                       observed.content == candidate.content &&
                       observed.bytes == candidate.bytes;
            });
        if (found == durableJournalCandidates.end()) {
            return {WindowsFilesystemBackendStatus::IdentityMismatch, 13};
        }
        if (retireAccessDeniedFailuresRemaining > 0) {
            --retireAccessDeniedFailuresRemaining;
            return {WindowsFilesystemBackendStatus::AccessDenied, 5};
        }
        if (failRetireCall != 0 && retireCalls == failRetireCall) {
            return {WindowsFilesystemBackendStatus::DurabilityFailed, 1005};
        }
        retiredJournalNames.push_back(candidate.name);
        const auto canonical = lookup(root, "aris-bootstrap.journal");
        if (canonical.identity == candidate.identity &&
            lookupContent(root, "aris-bootstrap.journal") == candidate.content) {
            entries[key(root, "aris-bootstrap.journal")] = {};
            contents.erase(key(root, "aris-bootstrap.journal"));
        }
        durableJournalCandidates.erase(found);
        return {};
    }

    WindowsFilesystemBackendResult ValidateBackupNamespaceNoFollow(
        WindowsFilesystemHandle transactionRoot,
        const std::vector<std::string>& expectedEntries,
        const std::vector<std::string>& allowedMissingEntries,
        bool expectDirectory,
        const WindowsFileIdentity& expectedDirectoryIdentity) override {
        if (transactionRoot != kTransaction) {
            return {WindowsFilesystemBackendStatus::AccessDenied, 6};
        }
        if (!expectDirectory) {
            return backupDirectoryAlreadyExists
                ? WindowsFilesystemBackendResult{
                      WindowsFilesystemBackendStatus::AlreadyExists, 183}
                : WindowsFilesystemBackendResult{};
        }
        if (rejectPathValidationAfterHandleBound && backupHandleBound) {
            return {WindowsFilesystemBackendStatus::IdentityMismatch, 1007};
        }
        if (!backupDirectoryAlreadyExists) {
            return {WindowsFilesystemBackendStatus::NotFound, 2};
        }
        if (expectedDirectoryIdentity.valid &&
            expectedDirectoryIdentity != identity(44)) {
            return {WindowsFilesystemBackendStatus::IdentityMismatch, 1006};
        }
        if (unknownBackupEntry) {
            return {WindowsFilesystemBackendStatus::AlreadyExists, 183};
        }
        std::map<std::string, bool> expected;
        for (const auto& path : expectedEntries) expected[path] = true;
        std::map<std::string, bool> allowedMissing;
        for (const auto& path : allowedMissingEntries) {
            if (expected.find(path) == expected.end()) {
                return {WindowsFilesystemBackendStatus::IdentityMismatch, 1006};
            }
            allowedMissing[path] = true;
        }
        for (const auto& entry : entries) {
            const std::string prefix = std::to_string(kBackup) + ":";
            if (entry.first.rfind(prefix, 0U) != 0U) continue;
            const std::string path = entry.first.substr(prefix.size());
            if (entry.second.kind == WindowsFilesystemEntryKind::Missing) continue;
            if (expected.find(path) == expected.end() ||
                entry.second.kind != WindowsFilesystemEntryKind::RegularFile) {
                return {WindowsFilesystemBackendStatus::AlreadyExists, 183};
            }
        }
        for (const auto& expectedEntry : expectedEntries) {
            const auto observed = lookup(kBackup, expectedEntry);
            if (observed.kind != WindowsFilesystemEntryKind::RegularFile &&
                allowedMissing.find(expectedEntry) == allowedMissing.end()) {
                return {WindowsFilesystemBackendStatus::NotFound, 2};
            }
        }
        if (injectUnknownBackupEntryAfterFirstValidation &&
            !backupValidationInjectionDone) {
            backupValidationInjectionDone = true;
            unknownBackupEntry = true;
        }
        return {};
    }

    WindowsFilesystemBackendResult ValidateBackupNamespaceBoundNoFollow(
        WindowsFilesystemHandle backupRoot,
        const std::vector<std::string>& expectedEntries,
        const std::vector<std::string>& allowedMissingEntries) override {
        ++boundBackupValidationCalls;
        if (backupRoot != kBackup || !backupHandleBound) {
            return {WindowsFilesystemBackendStatus::AccessDenied, 1007};
        }
        if (unknownBackupEntry) {
            return {WindowsFilesystemBackendStatus::AlreadyExists, 183};
        }
        std::map<std::string, bool> expected;
        for (const auto& path : expectedEntries) expected[path] = true;
        std::map<std::string, bool> allowedMissing;
        for (const auto& path : allowedMissingEntries) {
            if (expected.find(path) == expected.end()) {
                return {WindowsFilesystemBackendStatus::IdentityMismatch, 1006};
            }
            allowedMissing[path] = true;
        }
        for (const auto& entry : entries) {
            const std::string prefix = std::to_string(kBackup) + ":";
            if (entry.first.rfind(prefix, 0U) != 0U) continue;
            const std::string path = entry.first.substr(prefix.size());
            if (entry.second.kind == WindowsFilesystemEntryKind::Missing) continue;
            if (expected.find(path) == expected.end() ||
                entry.second.kind != WindowsFilesystemEntryKind::RegularFile) {
                return {WindowsFilesystemBackendStatus::AlreadyExists, 183};
            }
        }
        for (const auto& expectedEntry : expectedEntries) {
            const auto observed = lookup(kBackup, expectedEntry);
            if (observed.kind != WindowsFilesystemEntryKind::RegularFile &&
                allowedMissing.find(expectedEntry) == allowedMissing.end()) {
                return {WindowsFilesystemBackendStatus::NotFound, 2};
            }
        }
        return {};
    }

    WindowsFilesystemBackendResult PromoteJournalCandidateDurable(
        WindowsFilesystemHandle root,
        const WindowsBootstrapJournalCandidate& candidate,
        WindowsFileIdentity& outputIdentity,
        WindowsFileContentFingerprint& outputContent) override {
        ++promotionCalls;
        outputIdentity = {};
        outputContent = {};
        if (root != kTransaction || !candidate.identity.valid ||
            !candidate.content.valid ||
            (candidate.name != "aris-bootstrap.journal.a" &&
             candidate.name != "aris-bootstrap.journal.b" &&
             candidate.name != "aris-bootstrap.journal.a.tmp" &&
             candidate.name != "aris-bootstrap.journal.b.tmp")) {
            return {WindowsFilesystemBackendStatus::IdentityMismatch, 13};
        }
        outputIdentity = candidate.identity;
        outputContent = candidate.content;
        entries[key(root, "aris-bootstrap.journal")] = {
            WindowsFilesystemEntryKind::RegularFile, outputIdentity};
        contents[key(root, "aris-bootstrap.journal")] = outputContent;
        return {};
    }

    WindowsFilesystemBackendResult AcquireJournalMutationLease(
        WindowsFilesystemHandle root,
        const WindowsFileIdentity& expectedIdentity,
        const WindowsFileContentFingerprint& expectedContent,
        WindowsFilesystemHandle& outputLease) override {
        Call call;
        call.kind = CallKind::AcquireJournalLease;
        call.sourceRoot = root;
        call.expectedSource = expectedIdentity;
        call.expectedSourceContent = expectedContent;
        calls.push_back(call);
        outputLease = 0;
        if (journalLeaseAccessDeniedFailuresRemaining > 0) {
            --journalLeaseAccessDeniedFailuresRemaining;
            return {WindowsFilesystemBackendStatus::AccessDenied, 32};
        }
        if (failJournalLease) {
            return {WindowsFilesystemBackendStatus::ContentMismatch, 23};
        }
        if (journalLeaseHeld ||
            lookup(root, "aris-bootstrap.journal").identity != expectedIdentity ||
            lookupContent(root, "aris-bootstrap.journal") != expectedContent) {
            return {WindowsFilesystemBackendStatus::IdentityMismatch, 13};
        }
        journalLeaseHeld = true;
        outputLease = kJournalLease;
        return {};
    }

    void ReleaseJournalMutationLease(WindowsFilesystemHandle lease) override {
        Call call;
        call.kind = CallKind::ReleaseJournalLease;
        call.sourceRoot = lease;
        calls.push_back(call);
        if (lease == kJournalLease) journalLeaseHeld = false;
    }

    WindowsFilesystemBackendResult RemoveEmptyDirectoryDurableNoFollow(
        WindowsFilesystemHandle parent,
        const std::string& relativePath,
        const WindowsFileIdentity& expectedIdentity) override {
        Call call;
        call.kind = CallKind::RemoveEmptyDirectory;
        call.sourceRoot = parent;
        call.sourcePath = relativePath;
        call.expectedSource = expectedIdentity;
        calls.push_back(call);
        if (requireJournalLease && !journalLeaseHeld) {
            ++unleasedMutationCalls;
            return {WindowsFilesystemBackendStatus::AccessDenied, 32};
        }
        if (removeDirectoryAccessDeniedFailuresRemaining > 0) {
            --removeDirectoryAccessDeniedFailuresRemaining;
            return {WindowsFilesystemBackendStatus::AccessDenied, 32};
        }
        if (!backupDirectoryEmpty) {
            return {WindowsFilesystemBackendStatus::DirectoryNotEmpty, 145};
        }
        backupDirectoryAlreadyExists = false;
        return {};
    }

    void CloseFilesystemHandle(WindowsFilesystemHandle handleValue) override {
        Call call;
        call.kind = CallKind::CloseHandle;
        call.sourceRoot = handleValue;
        calls.push_back(call);
    }

    void set(WindowsFilesystemRootRole role,
             const std::string& path,
             WindowsFilesystemEntryKind kind,
             WindowsFileIdentity fileIdentity) {
        entries[key(handle(role), path)] = {kind, fileIdentity};
    }

    void setByHandle(WindowsFilesystemHandle root,
                     const std::string& path,
                     WindowsFilesystemEntryKind kind,
                     WindowsFileIdentity fileIdentity) {
        entries[key(root, path)] = {kind, fileIdentity};
    }

    void setContentByHandle(WindowsFilesystemHandle root,
                            const std::string& path,
                            WindowsFileContentFingerprint content) {
        contents[key(root, path)] = content;
    }

    void setMetadataByHandle(WindowsFilesystemHandle root,
                             const std::string& path,
                             WindowsFileMetadataSnapshot value) {
        metadatas[key(root, path)] = std::move(value);
    }

    WindowsFilesystemObservation lookup(WindowsFilesystemHandle root,
                                        const std::string& path) const {
        const auto found = entries.find(key(root, path));
        return found == entries.end() ? WindowsFilesystemObservation{} : found->second;
    }

    WindowsFileContentFingerprint lookupContent(
        WindowsFilesystemHandle root,
        const std::string& path) const {
        const auto found = contents.find(key(root, path));
        return found == contents.end() ? WindowsFileContentFingerprint{}
                                       : found->second;
    }

    WindowsFileMetadataSnapshot lookupMetadata(
        WindowsFilesystemHandle root,
        const std::string& path) const {
        const auto found = metadatas.find(key(root, path));
        return found == metadatas.end() ? WindowsFileMetadataSnapshot{}
                                       : found->second;
    }

    WindowsFileIdentity parentIdentity(WindowsFilesystemHandle root,
                                       const std::string& path) const {
        const std::size_t separator = path.find_last_of('/');
        if (separator != std::string::npos) {
            return lookup(root, path.substr(0, separator)).identity;
        }
        if (root == kBackup) return identity(44);
        WindowsFilesystemRootRole role = WindowsFilesystemRootRole::Install;
        if (root == kStaging) {
            role = WindowsFilesystemRootRole::Staging;
        } else if (root == kTransaction) {
            role = WindowsFilesystemRootRole::Transaction;
        }
        const auto configured = rootIdentities.find(role);
        return configured == rootIdentities.end()
            ? identity(static_cast<std::uint8_t>(40 + static_cast<int>(role)))
            : configured->second;
    }

private:
    std::uint8_t nextIdentity_ = 80;
    std::uint8_t nextPublicationIdentity_ = 180;
    std::uint8_t nextMetadata_ = 200;

    static WindowsFilesystemHandle handle(WindowsFilesystemRootRole role) {
        switch (role) {
            case WindowsFilesystemRootRole::Install: return kInstall;
            case WindowsFilesystemRootRole::Staging: return kStaging;
            case WindowsFilesystemRootRole::Transaction: return kTransaction;
            case WindowsFilesystemRootRole::Backup: return kBackup;
        }
        return 0;
    }

    static std::string key(WindowsFilesystemHandle root, const std::string& path) {
        return std::to_string(root) + ":" + path;
    }
};

bool hasMutation(const ScriptedBackend& backend) {
    return std::any_of(backend.calls.begin(), backend.calls.end(), [](const Call& call) {
        return call.kind == CallKind::PersistJournal || call.kind == CallKind::CopyFile ||
               call.kind == CallKind::ReplaceFile || call.kind == CallKind::DeleteFile ||
               call.kind == CallKind::RemoveEmptyDirectory;
    });
}

std::vector<CallKind> callsAfter(const ScriptedBackend& backend, std::size_t begin) {
    std::vector<CallKind> result;
    for (std::size_t index = begin; index < backend.calls.size(); ++index) {
        const CallKind kind = backend.calls[index].kind;
        if (kind == CallKind::PersistJournal || kind == CallKind::CopyFile ||
            kind == CallKind::ReplaceFile || kind == CallKind::DeleteFile ||
            kind == CallKind::RemoveEmptyDirectory) {
            result.push_back(kind);
        }
    }
    return result;
}

std::vector<CallKind> durableCallsAfter(const ScriptedBackend& backend,
                                        std::size_t begin) {
    std::vector<CallKind> result;
    for (std::size_t index = begin; index < backend.calls.size(); ++index) {
        const CallKind kind = backend.calls[index].kind;
        if (kind == CallKind::PersistJournal || kind == CallKind::CreateDirectory ||
            kind == CallKind::CopyFile || kind == CallKind::ReplaceFile ||
            kind == CallKind::DeleteFile || kind == CallKind::RemoveEmptyDirectory) {
            result.push_back(kind);
        }
    }
    return result;
}

std::vector<CallKind> journalEnvelopeCallsAfter(const ScriptedBackend& backend,
                                                std::size_t begin) {
    std::vector<CallKind> result;
    for (std::size_t index = begin; index < backend.calls.size(); ++index) {
        const CallKind kind = backend.calls[index].kind;
        if (kind == CallKind::PersistJournal ||
            kind == CallKind::AcquireJournalLease ||
            kind == CallKind::ReleaseJournalLease ||
            kind == CallKind::CreateDirectory || kind == CallKind::CopyFile ||
            kind == CallKind::ReplaceFile || kind == CallKind::DeleteFile ||
            kind == CallKind::RemoveEmptyDirectory) {
            result.push_back(kind);
        }
    }
    return result;
}

const Call* lastCallOf(const ScriptedBackend& backend, const CallKind kind) {
    const auto found = std::find_if(backend.calls.rbegin(), backend.calls.rend(),
                                    [kind](const Call& call) {
                                        return call.kind == kind;
                                    });
    return found == backend.calls.rend() ? nullptr : &*found;
}

std::vector<WindowsBootstrapJournalCandidate> recoveryCandidates(
    const ScriptedBackend& backend) {
    if (!backend.journalHistory.empty()) return backend.journalHistory;
    return backend.durableJournalCandidates;
}

void bindRecoveryJournalCandidates(ScriptedBackend& destination,
                                   const ScriptedBackend& source) {
    destination.journalHistory = source.journalHistory;
    // Copy the source's durable two-slot observations.  Looking up the
    // destination here silently produced an empty candidate set and made the
    // fresh-process parent-rebind contract fail before selector/rebind logic.
    destination.durableJournalCandidates = recoveryCandidates(source);
}

WindowsBootstrapFilesystemRoots roots() {
    return {"D:\\Trusted\\HarkenSearchUpdater\\staging",
            "D:\\Trusted\\HarkenSearchUpdater\\transaction",
            "D:\\Trusted\\HarkenSearchUpdater",
            "signed-invocation-01"};
}

WindowsBootstrapJournalRecord journalRecord(const Fixture& values,
                                            const std::uint64_t generation = 1U) {
    WindowsBootstrapJournalRecord record;
    record.generation = generation;
    record.transactionId = values.journal.transactionId;
    record.deploymentMode = values.plan.identity.deploymentMode;
    record.installationId = values.plan.identity.installationId;
    record.registeredAppId = values.plan.identity.registeredAppId;
    record.targetRoot = values.plan.identity.targetRoot;
    record.targetRootIdentity = values.plan.identity.targetRootIdentity;
    record.oldExecutableIdentity = values.plan.identity.oldExecutableIdentity;
    record.updaterOwnedBase = values.plan.invocation.updaterOwnedBase;
    record.stagingRoot = values.plan.invocation.stagingRoot;
    record.transactionRoot = values.plan.invocation.transactionRoot;
    record.protectedDataRoots = values.plan.identity.protectedDataRoots;
    record.invocationIdentity = values.plan.invocation.invocationIdentity;
    record.version = values.plan.version;
    record.buildId = values.plan.buildId;
    record.packagePlanDigest = values.plan.packagePlanDigest;
    record.packageKeyId = values.plan.packageKeyId;
    record.packageChannel = values.plan.packageChannel;
    record.healthNonceDigest = BootstrapSha256Hex(values.plan.healthToken);
    record.healthToken = values.plan.healthToken;
    record.oldProcessId = values.plan.oldProcessId;
    record.oldProcessIdentity = values.plan.invocation.oldProcess;
    record.parentProcessIdentity = values.plan.invocation.parentProcess;
    record.entrypoint = values.plan.entrypoint;
    record.phase = "prepared";
    record.installRootIdentity = identity(1);
    record.stagingRootIdentity = identity(2);
    record.transactionRootIdentity = identity(3);
    record.initialJournalDurable = true;
    for (std::size_t index = 0; index < values.journal.items.size(); ++index) {
        const auto& source = values.journal.items[index];
        WindowsBootstrapJournalItemRecord item;
        item.relativePath = source.relativePath;
        item.backupRelativePath = "item-" + std::to_string(index) + ".bak";
        item.classification = source.classification;
        item.installParentIdentity = record.installRootIdentity;
        if (source.classification != aris::bootstrap::JournalItemClass::PreserveExisting) {
            item.forwardTemporaryLeaf = ".aris-bootstrap-" +
                                        values.journal.transactionId + "-" +
                                        std::to_string(index) + "-forward.tmp";
            item.restoreTemporaryLeaf = ".aris-bootstrap-" +
                                        values.journal.transactionId + "-" +
                                        std::to_string(index) + "-restore.tmp";
            const auto incoming = std::find_if(
                values.plan.inventory.begin(), values.plan.inventory.end(),
                [&source](const auto& candidate) {
                    return candidate.relativePath == source.relativePath;
                });
            if (incoming != values.plan.inventory.end()) {
                item.stagingContent = fingerprintHex(incoming->sha256.c_str(),
                                                     incoming->byteSize);
                item.stagingIdentity = identity(
                    static_cast<std::uint8_t>(20U + index));
            }
        }
        if (source.classification != aris::bootstrap::JournalItemClass::CreateNew) {
            item.originalIdentity = identity(
                static_cast<std::uint8_t>(40U + index));
        }
        if (source.classification ==
                aris::bootstrap::JournalItemClass::ReplaceExisting ||
            source.classification ==
                aris::bootstrap::JournalItemClass::RemoveExisting) {
            item.backupContent = fingerprint(0x11, 900);
            item.rollbackMetadata = metadata(0x11);
        }
        record.items.push_back(std::move(item));
    }
    return record;
}

void test_strict_journal_codec_and_durable_winner_selection() {
    const Fixture values = fixture();
    WindowsBootstrapJournalRecord first = journalRecord(values, 1U);
    first.schemaVersion = 9U;
    first.packagePlanBytes = "signed-package-plan-v1";
    first.packageSignatureHex = std::string(128U, 'a');
    ValidatedInstallPlan boundPlan = values.plan;
    boundPlan.packagePlanBytes = first.packagePlanBytes;
    boundPlan.packageSignatureHex = first.packageSignatureHex;
    const std::vector<std::uint8_t> bytes =
        EncodeWindowsBootstrapJournal(first);
    check(!bytes.empty(), "ARISBOOTFS2 encoder emits a bounded record");
    if (bytes.empty()) return;
    const WindowsBootstrapJournalRootIdentities rootsBinding{
        first.installRootIdentity, first.stagingRootIdentity,
        first.transactionRootIdentity, {}};
    const auto decoded = DecodeWindowsBootstrapJournal(
        bytes, &boundPlan, &values.journal, &rootsBinding);
    check(decoded.ok() && decoded.record->generation == 1U &&
              decoded.record->healthToken.empty() &&
              decoded.record->healthNonceDigest ==
                  BootstrapSha256Hex(values.plan.healthToken) &&
              decoded.record->packagePlanBytes == first.packagePlanBytes &&
              decoded.record->packageSignatureHex == first.packageSignatureHex,
          "ARISBOOTFS2 schema 7 binds signed package authority without storing the raw health nonce");

    WindowsBootstrapJournalRecord droppedAppId = first;
    droppedAppId.deploymentMode = aris::platform::DeploymentMode::Installed;
    droppedAppId.installationId =
        "installed-a8b3c9d1-e4f5-4a6b-8c9d-0e1f2a3b4c5d";
    droppedAppId.registeredAppId = aris::platform::kArisInstalledAppId;
    droppedAppId.registeredAppId.clear();
    check(EncodeWindowsBootstrapJournal(droppedAppId).empty(),
          "schema 8 rejects an installed record with a dropped registered AppId binding");

    WindowsBootstrapJournalRecord wrongMode = first;
    wrongMode.deploymentMode = aris::platform::DeploymentMode::Installed;
    check(EncodeWindowsBootstrapJournal(wrongMode).empty(),
          "schema 8 rejects mode and installation identity mismatch");

    WindowsBootstrapJournalRecord invalidRootIdentity = first;
    invalidRootIdentity.targetRootIdentity.valid = false;
    check(EncodeWindowsBootstrapJournal(invalidRootIdentity).empty(),
          "schema 8 rejects an invalid target root identity");

    WindowsBootstrapJournalRecord invalidOldIdentity = first;
    invalidOldIdentity.oldExecutableIdentity.fileId.fill(0U);
    check(EncodeWindowsBootstrapJournal(invalidOldIdentity).empty(),
          "schema 8 rejects an invalid old executable identity");

    WindowsBootstrapJournalRecord changedRootIdentity = first;
    changedRootIdentity.targetRoot = "D:/Other/Aris";
    changedRootIdentity.targetRootIdentity.canonicalPath = "D:/Other/Aris";
    const auto changedRootBytes = EncodeWindowsBootstrapJournal(changedRootIdentity);
    check(!changedRootBytes.empty() &&
              DecodeWindowsBootstrapJournal(changedRootBytes, &boundPlan,
                                            nullptr, &rootsBinding)
                      .status != WindowsBootstrapJournalDecodeStatus::Ok,
          "schema 8 rejects target root identity tampering at decode");

    WindowsBootstrapJournalRecord changedOldIdentity = first;
    changedOldIdentity.oldExecutableIdentity.fileId.fill(0x99U);
    changedOldIdentity.oldProcessIdentity.imageFileId.fill(0x99U);
    const auto changedOldBytes = EncodeWindowsBootstrapJournal(changedOldIdentity);
    check(!changedOldBytes.empty() &&
              DecodeWindowsBootstrapJournal(changedOldBytes, &boundPlan,
                                            nullptr, &rootsBinding)
                      .status != WindowsBootstrapJournalDecodeStatus::Ok,
          "schema 8 rejects old executable identity tampering at decode");

    if (bytes.size() > 11U) {
        std::vector<std::uint8_t> legacySchema = bytes;
        legacySchema[11] = 6U;
        check(DecodeWindowsBootstrapJournal(legacySchema).status ==
                  WindowsBootstrapJournalDecodeStatus::UnsupportedVersion,
              "ARISBOOTFS2 rejects the pre-removal schema 6 explicitly");
    } else {
        check(false,
              "schema 7 encoder must publish bytes before legacy rejection is exercised");
    }

    WindowsBootstrapJournalRecord forgedExtraItem = first;
    WindowsBootstrapJournalItemRecord extraItem;
    extraItem.relativePath = "forged.dll";
    const std::size_t extraIndex = forgedExtraItem.items.size();
    extraItem.backupRelativePath = "item-" + std::to_string(extraIndex) + ".bak";
    extraItem.forwardTemporaryLeaf = ".aris-bootstrap-" +
        values.journal.transactionId + "-" + std::to_string(extraIndex) +
        "-forward.tmp";
    extraItem.restoreTemporaryLeaf = ".aris-bootstrap-" +
        values.journal.transactionId + "-" + std::to_string(extraIndex) +
        "-restore.tmp";
    extraItem.classification = aris::bootstrap::JournalItemClass::CreateNew;
    extraItem.stagingIdentity = identity(0x7fU);
    extraItem.stagingContent = fingerprint(0x7fU, 77U);
    extraItem.installParentIdentity = forgedExtraItem.installRootIdentity;
    forgedExtraItem.items.push_back(extraItem);
    const std::vector<std::uint8_t> forgedExtraBytes =
        EncodeWindowsBootstrapJournal(forgedExtraItem);
    check(!forgedExtraBytes.empty() &&
              DecodeWindowsBootstrapJournal(forgedExtraBytes, &boundPlan,
                                            nullptr, &rootsBinding)
                      .status == WindowsBootstrapJournalDecodeStatus::PlanMismatch,
          "ARISBOOTFS2 plan binding rejects journal items outside signed inventory");

    std::vector<std::uint8_t> truncated = bytes;
    truncated.pop_back();
    check(DecodeWindowsBootstrapJournal(truncated).status ==
              WindowsBootstrapJournalDecodeStatus::Truncated,
          "ARISBOOTFS2 decoder rejects a truncated checksum/body envelope");

    std::vector<std::uint8_t> oversized(4U * 1024U * 1024U + 1U, 0U);
    check(DecodeWindowsBootstrapJournal(oversized).status ==
              WindowsBootstrapJournalDecodeStatus::TooLarge,
          "ARISBOOTFS2 decoder rejects records above its fixed byte bound");

    std::vector<std::uint8_t> wrongMagic = bytes;
    wrongMagic.front() ^= 0x20U;
    check(DecodeWindowsBootstrapJournal(wrongMagic).status ==
              WindowsBootstrapJournalDecodeStatus::InvalidMagic,
          "ARISBOOTFS2 decoder rejects an alternate envelope magic");

    std::vector<std::uint8_t> wrongVersion = bytes;
    wrongVersion[11] = 2U;
    check(DecodeWindowsBootstrapJournal(wrongVersion).status ==
              WindowsBootstrapJournalDecodeStatus::UnsupportedVersion,
          "ARISBOOTFS2 decoder explicitly rejects the pre-release schema 2 before field reads");

    std::vector<std::uint8_t> corrupted = bytes;
    corrupted[corrupted.size() / 2U] ^= 0x80U;
    check(DecodeWindowsBootstrapJournal(corrupted).status ==
              WindowsBootstrapJournalDecodeStatus::InvalidChecksum,
          "ARISBOOTFS2 checksum detects corruption without becoming authority");

    std::vector<std::uint8_t> forgedWithValidChecksum = bytes;
    const std::string boundPackage = first.packagePlanBytes;
    const auto packageOffset = std::search(
        forgedWithValidChecksum.begin(), forgedWithValidChecksum.end(),
        boundPackage.begin(), boundPackage.end());
    check(packageOffset != forgedWithValidChecksum.end(),
          "journal authentication fixture locates durable package bytes");
    if (packageOffset != forgedWithValidChecksum.end()) {
        *packageOffset ^= 1U;
        rewriteJournalChecksum(forgedWithValidChecksum);
        check(DecodeWindowsBootstrapJournal(
                  forgedWithValidChecksum, &boundPlan, &values.journal,
                  &rootsBinding).status ==
                  WindowsBootstrapJournalDecodeStatus::InvalidAuthentication,
              "a recomputable FNV checksum cannot forge health-capability journal authentication");
    }

    ValidatedInstallPlan wrongRecoveryCapability = boundPlan;
    wrongRecoveryCapability.healthToken.assign(64U, 'd');
    check(DecodeWindowsBootstrapJournal(
              bytes, &wrongRecoveryCapability, &values.journal,
              &rootsBinding).status ==
              WindowsBootstrapJournalDecodeStatus::InvalidAuthentication,
          "restart decoding requires the exact fresh health capability preimage");

    auto wrongPlanRaw = rawPlan();
    wrongPlanRaw.buildId = "build-201";
    const auto wrongPlan = ValidateSignedInstallPlan(
        wrongPlanRaw, SignatureStatus::Verified);
    check(wrongPlan.ok() &&
              DecodeWindowsBootstrapJournal(
                  bytes, &*wrongPlan.plan, nullptr, nullptr).status ==
                  WindowsBootstrapJournalDecodeStatus::PlanMismatch,
          "valid checksum cannot authorize a different signed plan identity");

    WindowsBootstrapJournalRootIdentities wrongRoots = rootsBinding;
    wrongRoots.transaction = identity(99);
    check(DecodeWindowsBootstrapJournal(
              bytes, nullptr, nullptr, &wrongRoots).status ==
              WindowsBootstrapJournalDecodeStatus::RootMismatch,
          "valid checksum cannot authorize a different anchored transaction root");

    WindowsBootstrapJournalRecord missingAuthority = first;
    missingAuthority.packagePlanDigest.clear();
    check(EncodeWindowsBootstrapJournal(missingAuthority).empty(),
          "schema 6 refuses a journal without the signed package digest binding");
    missingAuthority = first;
    missingAuthority.oldProcessIdentity = {};
    check(EncodeWindowsBootstrapJournal(missingAuthority).empty(),
          "schema 6 refuses a journal without the exact old-process identity");

    WindowsBootstrapJournalRecord duplicate = first;
    duplicate.items.push_back(duplicate.items.front());
    duplicate.items.back().backupRelativePath = "item-3.bak";
    duplicate.items.back().forwardTemporaryLeaf =
        ".aris-bootstrap-" + duplicate.transactionId + "-3-forward.tmp";
    duplicate.items.back().restoreTemporaryLeaf =
        ".aris-bootstrap-" + duplicate.transactionId + "-3-restore.tmp";
    const auto duplicateBytes = EncodeWindowsBootstrapJournal(duplicate);
    check(!duplicateBytes.empty() &&
              DecodeWindowsBootstrapJournal(duplicateBytes).status ==
                  WindowsBootstrapJournalDecodeStatus::DuplicateItem,
          "ARISBOOTFS2 decoder rejects case-insensitive duplicate inventory ownership");

    WindowsBootstrapJournalRecord malformedMetadata = first;
    malformedMetadata.items.front().rollbackMetadata = metadata(0x44);
    malformedMetadata.items.front().rollbackMetadata.bytes.resize(41U);
    check(EncodeWindowsBootstrapJournal(malformedMetadata).empty(),
          "ARISBOOTFS2 encoder refuses structurally truncated rollback metadata");

    WindowsBootstrapJournalRecord invalidDescriptor = first;
    invalidDescriptor.items.front().rollbackMetadata.bytes[44] = 2U;
    check(EncodeWindowsBootstrapJournal(invalidDescriptor).empty(),
          "ARISBOOTFS2 encoder rejects an invalid self-relative security descriptor revision");

    WindowsBootstrapJournalRecord reservedName = first;
    reservedName.items.front().relativePath = "CON";
    check(EncodeWindowsBootstrapJournal(reservedName).empty(),
          "ARISBOOTFS2 rejects reserved Windows inventory components before native I/O");
    WindowsBootstrapJournalRecord trailingDot = first;
    trailingDot.items.front().relativePath = "bin/HarkenSearch.exe.";
    check(EncodeWindowsBootstrapJournal(trailingDot).empty(),
          "ARISBOOTFS2 rejects trailing-dot inventory aliases");
    WindowsBootstrapJournalRecord trailingSpace = first;
    trailingSpace.items.front().relativePath = "bin/HarkenSearch.exe ";
    check(EncodeWindowsBootstrapJournal(trailingSpace).empty(),
          "ARISBOOTFS2 rejects trailing-space inventory aliases");

    for (const char* identifier : {".leading", "-leading", "_leading",
                                   "trailing.", "trailing-", "trailing_"}) {
        WindowsBootstrapJournalRecord malformedIdentifier = first;
        malformedIdentifier.transactionId = identifier;
        check(EncodeWindowsBootstrapJournal(malformedIdentifier).empty(),
              "ARISBOOTFS2 identifiers require alphanumeric boundary characters");
    }

    WindowsBootstrapJournalRecord second = first;
    second.generation = 2U;
    second.journalIdentity = identity(0xa1);
    second.journalContent = fingerprint(0xb1, 1U);
    second.phase = "replacement-started:HarkenSearch.exe";
    const std::vector<std::uint8_t> secondBytes =
        EncodeWindowsBootstrapJournal(second);
    const auto selected = SelectDurableWindowsBootstrapJournal(
        {{"aris-bootstrap.journal.a", bytes, true, false,
          identity(0xa1), fingerprint(0xb1, 1U)},
         {"aris-bootstrap.journal.b", secondBytes, true, false,
          identity(0xa2), fingerprint(0xb2, 1U)}},
        &boundPlan, &values.journal, &rootsBinding);
    check(selected.ok() && selected.record->generation == 2U &&
              selected.selectedName == "aris-bootstrap.journal.b",
          "two durable slots select the unique highest generation");

    WindowsBootstrapJournalRecord fork = second;
    fork.phase = "rolling-back";
    const auto ambiguous = SelectDurableWindowsBootstrapJournal(
        {{"aris-bootstrap.journal.a", secondBytes, true, false,
          identity(0xa1), fingerprint(0xb1, 1U)},
         {"aris-bootstrap.journal.b", EncodeWindowsBootstrapJournal(fork),
          true, false, identity(0xa2), fingerprint(0xb2, 1U)}},
        &boundPlan, &values.journal, &rootsBinding);
    check(ambiguous.status == WindowsBootstrapJournalSelectionStatus::Ambiguous,
          "equal durable generations with different content fail closed");
    const auto duplicateGeneration = SelectDurableWindowsBootstrapJournal(
        {{"aris-bootstrap.journal.a", secondBytes, true, false,
          identity(0xa1), fingerprint(0xb1, 1U)},
         {"aris-bootstrap.journal.b", secondBytes, true, false,
          identity(0xa2), fingerprint(0xb2, 1U)}},
        &boundPlan, &values.journal, &rootsBinding);
    check(duplicateGeneration.status ==
              WindowsBootstrapJournalSelectionStatus::Ambiguous,
          "multiple durable names at the highest generation fail closed even when their bytes match");

    std::vector<std::uint8_t> malformedHigher = secondBytes;
    malformedHigher[0] ^= 0x7fU;
    check(SelectDurableWindowsBootstrapJournal(
              {{"aris-bootstrap.journal.a", bytes, true, false,
                identity(0xa1), fingerprint(0xb1, 1U)},
               {"aris-bootstrap.journal.b", malformedHigher, true, false,
                identity(0xa2), fingerprint(0xb2, 1U)}} ,
              &boundPlan, &values.journal, &rootsBinding)
                  .status == WindowsBootstrapJournalSelectionStatus::InvalidCandidate,
          "a malformed higher-generation candidate cannot be guessed away in favor of a lower slot");

    check(SelectDurableWindowsBootstrapJournal(
              {{"aris-bootstrap.journal.a", bytes, true, false,
                identity(0xa1), fingerprint(0xb1, 1U)},
               {"aris-bootstrap.journal.a.tmp", bytes, true, true,
                identity(0xa1), fingerprint(0xb1, 1U)}} ,
              &boundPlan, &values.journal, &rootsBinding)
                  .status == WindowsBootstrapJournalSelectionStatus::Ambiguous,
          "a same-generation published/temp pair is ambiguous rather than promoted by name order");

    const std::uint64_t maximumGeneration =
        (std::numeric_limits<std::uint64_t>::max)();
    WindowsBootstrapJournalRecord predecessor = first;
    predecessor.generation = maximumGeneration - 1U;
    predecessor.journalIdentity = identity(0xc1);
    predecessor.journalContent = fingerprint(0xd1, 1U);
    const auto predecessorBytes = EncodeWindowsBootstrapJournal(predecessor);
    WindowsBootstrapJournalRecord maximum = first;
    maximum.generation = maximumGeneration;
    maximum.journalIdentity = identity(0xc1);
    maximum.journalContent = fingerprint(0xd1, 1U);
    maximum.phase = "maximum-generation";
    const auto maximumBytes = EncodeWindowsBootstrapJournal(maximum);
    check(!predecessorBytes.empty() && !maximumBytes.empty() &&
              SelectDurableWindowsBootstrapJournal(
                  {{"aris-bootstrap.journal.b", predecessorBytes, true, false,
                    identity(0xc1), fingerprint(0xd1, 1U)},
                   {"aris-bootstrap.journal.a", maximumBytes, true, false,
                    identity(0xc2), fingerprint(0xd2, 1U)}} ,
                  &boundPlan, &values.journal, &rootsBinding)
                      .ok(),
          "maximum journal generation remains bounded and selects through its opposite-slot predecessor");

    WindowsBootstrapJournalRecord unlinked = journalRecord(values, 2U);
    const auto unlinkedBytes = EncodeWindowsBootstrapJournal(unlinked);
    check(SelectDurableWindowsBootstrapJournal(
              {{"aris-bootstrap.journal.a", bytes, true, false,
                identity(0xa1), fingerprint(0xb1, 1U)},
               {"aris-bootstrap.journal.b", unlinkedBytes, true, false,
                identity(0xa2), fingerprint(0xb2, 1U)}},
              &boundPlan, &values.journal, &rootsBinding)
                  .status == WindowsBootstrapJournalSelectionStatus::InvalidCandidate,
          "a higher generation without an exact predecessor linkage is rejected");

    WindowsBootstrapJournalRecord linked = journalRecord(values, 2U);
    linked.journalIdentity = identity(0xa1);
    linked.journalContent = fingerprint(0xb1, 1U);
    const auto linkedBytes = EncodeWindowsBootstrapJournal(linked);
    check(SelectDurableWindowsBootstrapJournal(
              {{"aris-bootstrap.journal.b", linkedBytes, true, false,
                identity(0xa2), fingerprint(0xb2, 1U)}},
              &boundPlan, &values.journal, &rootsBinding)
                  .status == WindowsBootstrapJournalSelectionStatus::InvalidCandidate,
          "a higher generation is rejected when its bound predecessor slot is missing");
    check(SelectDurableWindowsBootstrapJournal(
              {{"aris-bootstrap.journal.a", bytes, true, false,
                identity(0xa1), fingerprint(0xb1, 1U)},
               {"aris-bootstrap.journal.a.tmp", linkedBytes, true, true,
                identity(0xa2), fingerprint(0xb2, 1U)}},
                  &boundPlan, &values.journal, &rootsBinding)
                  .status == WindowsBootstrapJournalSelectionStatus::InvalidCandidate,
          "a successor generation cannot replace its own predecessor slot");
    check(SelectDurableWindowsBootstrapJournal(
              {{"aris-bootstrap.journal.a", bytes, true, false,
                identity(0xa1), fingerprint(0xb1, 1U)},
               {"aris-bootstrap.journal.a.tmp", bytes, true, true,
                identity(0xa1), fingerprint(0xb1, 1U)},
               {"aris-bootstrap.journal.b", linkedBytes, true, false,
                identity(0xa2), fingerprint(0xb2, 1U)}},
              &boundPlan, &values.journal, &rootsBinding)
                  .status == WindowsBootstrapJournalSelectionStatus::InvalidCandidate,
          "duplicated predecessor generations are rejected instead of guessed");
    check(SelectDurableWindowsBootstrapJournal(
              {{"aris-bootstrap.journal.a.tmp", bytes, true, true,
                identity(0xa1), fingerprint(0xb1, 1U)},
               {"aris-bootstrap.journal.b", linkedBytes, true, false,
                identity(0xa2), fingerprint(0xb2, 1U)}},
              &boundPlan, &values.journal, &rootsBinding)
                  .status == WindowsBootstrapJournalSelectionStatus::InvalidCandidate,
          "a temporary predecessor cannot authorize a later durable generation");
    check(SelectDurableWindowsBootstrapJournal(
              {{"aris-bootstrap.journal.a", bytes, true, false,
                identity(0xa1), fingerprint(0xb9, 1U)},
               {"aris-bootstrap.journal.b", linkedBytes, true, false,
                identity(0xa2), fingerprint(0xb2, 1U)}},
              &boundPlan, &values.journal, &rootsBinding)
                  .status == WindowsBootstrapJournalSelectionStatus::InvalidCandidate,
          "same-file-id predecessor content tampering is rejected before winner promotion");
}

void test_journal_rejects_impossible_progress_flags() {
    const Fixture values = fixture();
    const WindowsBootstrapJournalRootIdentities rootsBinding{
        identity(1), identity(2), identity(3), {}};
    const auto expectRejected = [&](const char* name, auto mutate) {
        WindowsBootstrapJournalRecord candidate = journalRecord(values, 1U);
        mutate(candidate);
        const auto encoded = EncodeWindowsBootstrapJournal(candidate);
        WindowsBootstrapJournalDecodeResult decoded;
        if (encoded.empty()) {
            decoded.status = WindowsBootstrapJournalDecodeStatus::InvalidItem;
        } else {
            decoded = DecodeWindowsBootstrapJournal(encoded, &values.plan,
                                                    &values.journal,
                                                    &rootsBinding);
        }
        check(encoded.empty() || !decoded.ok(), name);
    };

    expectRejected("journal rejects commit before every replacement is complete",
                   [](WindowsBootstrapJournalRecord& record) {
                       record.commitComplete = true;
                   });
    expectRejected("journal rejects simultaneous commit and rollback authority",
                   [](WindowsBootstrapJournalRecord& record) {
                       record.commitComplete = true;
                       record.rollbackDurable = true;
                   });
    expectRejected("journal rejects replacement complete without replacement start",
                   [](WindowsBootstrapJournalRecord& record) {
                       auto& item = record.items.front();
                       item.replacementComplete = true;
                   });
    expectRejected("journal rejects prepared replacement without bound temp",
                   [](WindowsBootstrapJournalRecord& record) {
                       auto& item = record.items.front();
                       item.replacementStarted = true;
                       item.replacementPrepared = true;
                   });
    expectRejected("journal binds prepared forward bytes to the signed staging fingerprint",
                   [](WindowsBootstrapJournalRecord& record) {
                       auto& item = record.items.front();
                       item.replacementStarted = true;
                       item.replacementPrepared = true;
                       item.preparedIdentity = identity(71);
                       item.preparedContent = fingerprint(0x71, 1024);
                       item.preparedMetadata = item.rollbackMetadata;
                   });
    expectRejected("journal binds the completed replacement identity to its prepared temp",
                   [](WindowsBootstrapJournalRecord& record) {
                       auto& item = record.items.front();
                       item.replacementStarted = true;
                       item.replacementPrepared = true;
                       item.preparedIdentity = identity(72);
                       item.preparedContent = item.stagingContent;
                       item.preparedMetadata = item.rollbackMetadata;
                       item.replacementComplete = true;
                       item.installedIdentity = identity(73);
                       item.installedMetadata = metadata(0x73);
                   });
    expectRejected("journal rejects restore completion on a create-new item",
                   [](WindowsBootstrapJournalRecord& record) {
                       auto& item = record.items[1];
                       item.restoreComplete = true;
                   });
    expectRejected("journal rejects new-item removal flags on replacement item",
                   [](WindowsBootstrapJournalRecord& record) {
                       auto& item = record.items.front();
                       item.removeNewStarted = true;
                       item.removeNewComplete = true;
                   });
    expectRejected("journal rejects backup cleanup without a durable backup",
                   [](WindowsBootstrapJournalRecord& record) {
                       auto& item = record.items.front();
                       item.removeBackupStarted = true;
                       item.removeBackupComplete = true;
                   });
    expectRejected("journal rejects removed backup directory with incomplete items",
                   [](WindowsBootstrapJournalRecord& record) {
                       record.backupDirectoryRemoved = true;
                   });
    expectRejected("journal rejects a backup-directory start without an initial durable generation",
                   [](WindowsBootstrapJournalRecord& record) {
                       record.initialJournalDurable = false;
                       record.backupDirectoryStarted = true;
                   });
    expectRejected("journal rejects rollback authority with an unfinished forward publication",
                   [](WindowsBootstrapJournalRecord& record) {
                       auto& item = record.items.front();
                       record.rollbackDurable = true;
                       item.replacementStarted = true;
                       item.replacementPrepared = true;
                       item.preparedIdentity = identity(74);
                       item.preparedContent = item.stagingContent;
                       item.preparedMetadata = item.rollbackMetadata;
                   });
    expectRejected("journal requires a staged identity once a create-new publication starts",
                   [](WindowsBootstrapJournalRecord& record) {
                       auto& item = record.items[1];
                       item.replacementStarted = true;
                       item.stagingIdentity = {};
                   });
    expectRejected("journal rejects backup-directory cleanup before every backup is removed",
                   [](WindowsBootstrapJournalRecord& record) {
                       record.backupDirectoryStarted = true;
                       record.backupDirectoryComplete = true;
                       record.backupRootIdentity = identity(81);
                       record.commitComplete = true;
                       for (auto& item : record.items) {
                           if (item.classification ==
                               aris::bootstrap::JournalItemClass::ReplaceExisting) {
                               item.replacementStarted = true;
                               item.replacementPrepared = true;
                               item.preparedIdentity = identity(82);
                               item.preparedContent = item.stagingContent;
                               item.preparedMetadata = item.rollbackMetadata;
                               item.replacementComplete = true;
                               item.installedIdentity = item.preparedIdentity;
                               item.installedMetadata = item.preparedMetadata;
                           }
                       }
                       record.backupDirectoryRemoveStarted = true;
                   });
    expectRejected("journal rejects replacement before every required backup is durable",
                   [](WindowsBootstrapJournalRecord& record) {
                       record.backupDirectoryStarted = true;
                       record.backupDirectoryComplete = true;
                       record.backupRootIdentity = identity(83);
                       record.items.front().replacementStarted = true;
                   });
    expectRejected("journal rejects item backup cleanup before commit or rollback",
                   [](WindowsBootstrapJournalRecord& record) {
                       record.backupDirectoryStarted = true;
                       record.backupDirectoryComplete = true;
                       record.backupRootIdentity = identity(84);
                       auto& item = record.items.front();
                       item.backupStarted = true;
                       item.backupComplete = true;
                       item.backupIdentity = identity(85);
                       item.removeBackupStarted = true;
                       item.removeBackupComplete = true;
                   });
}

void test_journal_accepts_backend_normalized_acl_metadata() {
    const Fixture values = fixture();
    WindowsBootstrapJournalRecord record = journalRecord(values, 1U);
    record.backupDirectoryStarted = true;
    record.backupDirectoryComplete = true;
    record.backupRootIdentity = identity(0x70U);
    std::uint8_t backupMarker = 0x72U;
    for (auto& candidate : record.items) {
        if (candidate.classification ==
                aris::bootstrap::JournalItemClass::ReplaceExisting ||
            candidate.classification ==
                aris::bootstrap::JournalItemClass::RemoveExisting) {
            candidate.backupStarted = true;
            candidate.backupComplete = true;
            candidate.backupIdentity = identity(backupMarker++);
        }
    }
    auto& item = record.items.front();
    item.replacementStarted = true;
    item.replacementPrepared = true;
    item.preparedIdentity = identity(0x71U);
    item.preparedContent = item.stagingContent;
    // Win32 can normalize inherited/defaulted ACL provenance while preserving
    // the target security contract. The journal codec cannot interpret that
    // opaque platform metadata and must defer equivalence to the backend.
    item.preparedMetadata = metadata(0x22U);
    check(!EncodeWindowsBootstrapJournal(record).empty(),
          "journal accepts a prepared replacement with backend-normalized metadata");
}

void test_journal_rejects_impossible_runtime_lifecycle_authority() {
    const Fixture values = fixture();
    const auto expectRejected = [&](const char* name, auto mutate) {
        WindowsBootstrapJournalRecord record = journalRecord(values, 1U);
        mutate(record);
        const std::vector<std::uint8_t> bytes =
            EncodeWindowsBootstrapJournal(record);
        const bool rejected = bytes.empty() ||
            DecodeWindowsBootstrapJournal(bytes).status ==
                WindowsBootstrapJournalDecodeStatus::InvalidField;
        check(rejected, name);
    };

    expectRejected("journal rejects a failure count beyond the two-failure quarantine bound",
                   [](WindowsBootstrapJournalRecord& record) {
                       record.previousFailureCount = 3U;
                   });
    expectRejected("journal rejects child termination authority without a health lifecycle phase",
                   [](WindowsBootstrapJournalRecord& record) {
                       record.updatedChildIdentity =
                           processIdentity(301U, 0x71U);
                   });
    expectRejected("journal rejects awaiting-health without an exact launched-child identity",
                   [](WindowsBootstrapJournalRecord& record) {
                       record.healthPhase = "awaiting-health";
                   });
    expectRejected("journal rejects an accepted health phase before nonce consumption",
                   [](WindowsBootstrapJournalRecord& record) {
                       record.updatedChildIdentity =
                           processIdentity(302U, 0x72U);
                       record.healthPhase = "accepted";
                   });
    expectRejected("journal rejects nonce consumption while health is still pending",
                   [](WindowsBootstrapJournalRecord& record) {
                       record.updatedChildIdentity =
                           processIdentity(303U, 0x73U);
                       record.healthPhase = "awaiting-health";
                       record.healthTokenConsumed = true;
                   });
    expectRejected("journal rejects an unrecognized health lifecycle phase",
                   [](WindowsBootstrapJournalRecord& record) {
                       record.updatedChildIdentity =
                           processIdentity(304U, 0x74U);
                       record.healthPhase = "forged-health";
                   });

    WindowsBootstrapJournalRecord pendingRelaunch =
        journalRecord(values, 1U);
    pendingRelaunch.items.clear();
    pendingRelaunch.backupDirectoryStarted = true;
    pendingRelaunch.backupDirectoryComplete = true;
    pendingRelaunch.backupRootIdentity = identity(0x75U);
    pendingRelaunch.commitComplete = true;
    pendingRelaunch.committedApplicationRelaunchPending = true;
    const std::vector<std::uint8_t> pendingBytes =
        EncodeWindowsBootstrapJournal(pendingRelaunch);
    check(!pendingBytes.empty(),
          "lifecycle byte fixture encodes a valid committed relaunch checkpoint");
    if (pendingBytes.empty()) return;

    constexpr std::size_t kAuthenticationTagBytes = 32U;
    constexpr std::size_t kChecksumBytes = sizeof(std::uint64_t);
    constexpr std::size_t kItemCountBytes = sizeof(std::uint32_t);
    // The original lifecycle booleans are followed by the schema-9
    // runtime-pair authority and cleanup booleans before the item count.
    constexpr std::size_t kLifecycleBytes = 18U;
    const std::size_t lifecycleOffset = pendingBytes.size() -
        kAuthenticationTagBytes - kChecksumBytes - kItemCountBytes -
        kLifecycleBytes;
    const auto expectInvalidLifecycleByte =
        [&](const std::size_t index, const std::uint8_t value,
            const char* message) {
            std::vector<std::uint8_t> forged = pendingBytes;
            forged[lifecycleOffset + index] = value;
            rewriteJournalChecksum(forged);
            check(DecodeWindowsBootstrapJournal(forged).status ==
                      WindowsBootstrapJournalDecodeStatus::InvalidField,
                  message);
        };
    expectInvalidLifecycleByte(
        3U, 0U,
        "decoder classifies pending relaunch without commit as an invalid lifecycle field");
    expectInvalidLifecycleByte(
        9U, 0U,
        "decoder classifies committed empty health state without relaunch authority as an invalid lifecycle field");
    expectInvalidLifecycleByte(
        4U, 1U,
        "decoder classifies committed relaunch plus rollback authority as an invalid lifecycle field");
}

// Recovery must be able to reconstruct the dynamic invocation roots and
// protected-data boundary from the durable winner; a fresh process cannot
// safely ask a caller to resend arbitrary writable paths.  This contract is
// intentionally written before the schema fields are implemented.
void test_schema6_persists_anchored_recovery_binding() {
    const Fixture values = fixture();
    WindowsBootstrapJournalRecord record = journalRecord(values, 1U);
    record.stagingRoot = values.plan.invocation.stagingRoot;
    record.transactionRoot = values.plan.invocation.transactionRoot;
    record.protectedDataRoots = values.plan.identity.protectedDataRoots;
    record.previousFailureCount = 1U;
    const auto encoded = EncodeWindowsBootstrapJournal(record);
    check(!encoded.empty(),
          "schema 6 accepts the complete anchored recovery binding");
    const auto decoded = DecodeWindowsBootstrapJournal(encoded);
    check(decoded.ok() && decoded.record->stagingRoot == record.stagingRoot &&
              decoded.record->transactionRoot == record.transactionRoot &&
              decoded.record->protectedDataRoots == record.protectedDataRoots &&
              decoded.record->previousFailureCount == 1U,
          "schema 6 round-trips dynamic roots, protected data roots, and failure count");

    WindowsBootstrapJournalRecord forged = record;
    forged.transactionRoot = "D:/OtherWritableRoot";
    check(!EncodeWindowsBootstrapJournal(forged).empty(),
          "schema 6 keeps lexical encoding separate from trusted-root authority");
    check(!ValidateAnchoredWindowsBootstrapJournalRoots(
              forged, values.plan.invocation.updaterOwnedBase,
              values.plan.invocation.stagingRoot,
              values.plan.invocation.transactionRoot),
          "recovery rejects a caller-forged transaction root against the updater anchor");
}

void test_record_failure_advances_the_authenticated_durable_count() {
    const Fixture values = fixture();
    ScriptedBackend backend;
    WindowsBootstrapFilesystemPort filesystem(backend);
    check(filesystem.Begin(values.plan, values.journal, roots()).ok() &&
              filesystem.BindPreviousFailureCount(1U).ok() &&
              filesystem.Apply(intent(BootstrapIntentKind::PersistJournal, {},
                                      "prepared")).ok(),
          "failure-count fixture binds the authenticated history before publication");

    const auto recorded = filesystem.Apply(
        intent(BootstrapIntentKind::RecordFailure, {}, "health-timeout"));
    const auto decoded = DecodeWindowsBootstrapJournal(
        backend.lastJournalBytes, &values.plan, &values.journal, nullptr);
    check(recorded.ok() && decoded.ok() &&
              decoded.record->previousFailureCount == 2U &&
              decoded.record->phase == "failure-recorded",
          "record-failure advances and durably publishes the authenticated failure count");
}

void test_restart_restores_the_authenticated_failure_count() {
    const Fixture values = fixture();
    ScriptedBackend liveBackend;
    {
        WindowsBootstrapFilesystemPort live(liveBackend);
        check(live.Begin(values.plan, values.journal, roots()).ok() &&
                  live.BindPreviousFailureCount(1U).ok() &&
                  live.Apply(intent(BootstrapIntentKind::PersistJournal, {},
                                    "prepared")).ok(),
              "restart failure-count fixture publishes an authenticated prior count");
    }

    ScriptedBackend freshBackend;
    freshBackend.entries = liveBackend.entries;
    freshBackend.contents = liveBackend.contents;
    freshBackend.metadatas = liveBackend.metadatas;
    freshBackend.backupDirectoryAlreadyExists =
        liveBackend.backupDirectoryAlreadyExists;
    freshBackend.backupDirectoryEmpty = liveBackend.backupDirectoryEmpty;
    bindRecoveryJournalCandidates(freshBackend, liveBackend);
    WindowsBootstrapFilesystemPort recovered(freshBackend);
    const auto recovery = recovered.BeginRecovery(values.plan, roots());
    const auto recorded = recovered.Apply(
        intent(BootstrapIntentKind::RecordFailure, {}, "health-timeout"));
    const auto decoded = DecodeWindowsBootstrapJournal(
        freshBackend.lastJournalBytes, &values.plan, &values.journal, nullptr);
    check(recovery.ok() && recorded.ok() && decoded.ok() &&
              decoded.record->previousFailureCount == 2U,
          "fresh recovery advances from the durable prior count instead of resetting history");
}

void test_durable_recovery_admission_requires_authenticated_authority() {
    const Fixture values = fixture();
    WindowsBootstrapJournalRecord record = journalRecord(values, 1U);
    record.packagePlanBytes.clear();
    record.packageSignatureHex.clear();
    PackagePlanTrustConfig trust;
    trust.provisioned = true;
    trust.keyId = record.packageKeyId;
    const DurableRecoveryAdmissionResult rejected =
        AdmitDurableBootstrapRecovery(
            record, trust, values.plan.invocation.parentProcess,
            values.plan.invocation.updaterOwnedBase,
            values.plan.invocation.stagingRoot,
            values.plan.invocation.transactionRoot);
    check(rejected.status == DurableRecoveryAdmissionStatus::MissingPackageAuthority,
          "durable recovery rejects digest-only journal authority before mutation");
}

void test_restart_rebind_recovers_atomic_publish_before_next_generation() {
    const Fixture values = fixture();
    ScriptedBackend crashedBackend;
    {
        WindowsBootstrapFilesystemPort live(crashedBackend);
        check(live.Begin(values.plan, values.journal, roots()).ok() &&
                  live.Apply(intent(BootstrapIntentKind::PersistJournal, {},
                                    "prepared")).ok() &&
                  live.Apply(intent(BootstrapIntentKind::SnapshotBackup,
                                    "HarkenSearch.exe")).ok(),
              "restart fixture reaches a durable backup");
        crashedBackend.failPersistCall = crashedBackend.persistCalls + 3;
        const WindowsBootstrapFilesystemResult interrupted = live.Apply(
            intent(BootstrapIntentKind::ReplaceInventoryItem, "HarkenSearch.exe"));
        check(interrupted.status ==
                  WindowsBootstrapFilesystemStatus::PartialDurableState &&
                  crashedBackend.lookup(ScriptedBackend::kInstall,
                                        "HarkenSearch.exe").identity != identity(21),
              "restart fixture cuts after atomic target publication and before its next journal generation");
    }

    const auto journalObservation = crashedBackend.lookup(
        ScriptedBackend::kTransaction, "aris-bootstrap.journal");
    const auto journalContent = crashedBackend.lookupContent(
        ScriptedBackend::kTransaction, "aris-bootstrap.journal");
    check(journalObservation.kind == WindowsFilesystemEntryKind::RegularFile &&
              journalObservation.identity.valid && journalContent.valid &&
              !crashedBackend.lastJournalBytes.empty(),
          "restart fixture retains a readable durable winner");

    ScriptedBackend freshBackend;
    freshBackend.entries = crashedBackend.entries;
    freshBackend.contents = crashedBackend.contents;
    freshBackend.metadatas = crashedBackend.metadatas;
    freshBackend.backupDirectoryAlreadyExists =
        crashedBackend.backupDirectoryAlreadyExists;
    freshBackend.backupDirectoryEmpty = crashedBackend.backupDirectoryEmpty;
    bindRecoveryJournalCandidates(freshBackend, crashedBackend);

    {
        WindowsBootstrapFilesystemPort forgedRecovery(freshBackend);
        WindowsBootstrapFilesystemRoots forgedRoots = roots();
        forgedRoots.invocationIdentity = "forged-invocation";
        const auto rejected = forgedRecovery.BeginRecovery(
            values.plan, values.journal, forgedRoots);
        check(rejected.filesystem.status ==
                  WindowsBootstrapFilesystemStatus::InvalidRoots &&
                  freshBackend.persistCalls == 0,
              "restart recovery binds the signed invocation before promotion or mutation");
    }

    WindowsBootstrapFilesystemPort recovered(freshBackend);
    const auto recovery = recovered.BeginRecovery(values.plan, roots());
    const bool repeatsPublishedItem = std::any_of(
        recovery.intents.begin(), recovery.intents.end(),
        [](const BootstrapIntent& recoveryIntent) {
            return recoveryIntent.kind ==
                       BootstrapIntentKind::ReplaceInventoryItem &&
                   recoveryIntent.relativePath == "HarkenSearch.exe";
        });
    check(recovery.ok() && recovery.journal.has_value() &&
              recovery.journal->isValidatedFor(values.plan) &&
              recovery.disposition ==
                  WindowsBootstrapRecoveryDisposition::ContinueForward &&
              !repeatsPublishedItem,
          "a fresh port/backend reopens the journal and records the already-published item exactly once");
    check(freshBackend.persistCalls == 1,
          "restart rebind durably publishes recovered progress before returning intents");
}

void test_runtime_lifecycle_checkpoint_survives_fresh_recovery() {
    const Fixture values = fixture();
    ScriptedBackend liveBackend;
    BootstrapProcessIdentity child = processIdentity(201U, 0x51U);
    {
        WindowsBootstrapFilesystemPort live(liveBackend);
        check(live.Begin(values.plan, values.journal, roots()).ok() &&
                  live.Apply(intent(BootstrapIntentKind::PersistJournal, {},
                                    "prepared")).ok() &&
                  live.Apply(intent(BootstrapIntentKind::SnapshotBackup,
                                    "HarkenSearch.exe")).ok() &&
                  live.Apply(intent(BootstrapIntentKind::ReplaceInventoryItem,
                                    "HarkenSearch.exe")).ok() &&
                  live.Apply(intent(BootstrapIntentKind::ReplaceInventoryItem,
                                    "resources/core.pak")).ok(),
              "runtime checkpoint fixture reaches complete durable replacement");
        check(live.RecordUpdatedChildIdentity(child).ok(),
              "exact launched child identity is durable before health authority");
        check(live.RecordHealthState("awaiting-health", false).ok(),
              "awaiting-health lifecycle state is durable");
        check(live.RecordHealthState("accepted", true).ok(),
              "accepted health lifecycle state consumes the nonce durably");
        const auto decoded = DecodeWindowsBootstrapJournal(
            liveBackend.lastJournalBytes, &values.plan, &values.journal, nullptr);
        check(decoded.ok() && decoded.record->updatedChildIdentity == child &&
                  decoded.record->healthPhase == "accepted" &&
                  decoded.record->healthTokenConsumed &&
                  decoded.record->healthToken.empty(),
              "runtime checkpoint serializes identity/state without the raw nonce");
    }

    ScriptedBackend freshBackend;
    freshBackend.entries = liveBackend.entries;
    freshBackend.contents = liveBackend.contents;
    freshBackend.metadatas = liveBackend.metadatas;
    freshBackend.backupDirectoryAlreadyExists =
        liveBackend.backupDirectoryAlreadyExists;
    freshBackend.backupDirectoryEmpty = liveBackend.backupDirectoryEmpty;
    bindRecoveryJournalCandidates(freshBackend, liveBackend);
    WindowsBootstrapFilesystemPort recovered(freshBackend);
    const auto recovery = recovered.BeginRecovery(values.plan, roots());
    check(recovery.filesystem.ok() &&
              recovered.recoveredUpdatedChildIdentity() == child,
          "fresh recovery rehydrates the exact launched child identity from the durable winner");
}

void test_committed_health_relaunch_survives_cleanup_restart() {
    const Fixture values = fixture();
    ScriptedBackend committedBackend;
    const BootstrapProcessIdentity originalChild =
        processIdentity(301U, 0x61U);
    {
        WindowsBootstrapFilesystemPort committed(committedBackend);
        check(committed.Begin(values.plan, values.journal, roots()).ok() &&
                  committed.Apply(intent(BootstrapIntentKind::PersistJournal,
                                         {}, "prepared")).ok() &&
                  committed.Apply(intent(BootstrapIntentKind::SnapshotBackup,
                                         "HarkenSearch.exe")).ok() &&
                  committed.Apply(intent(
                      BootstrapIntentKind::ReplaceInventoryItem,
                      "HarkenSearch.exe")).ok() &&
                  committed.Apply(intent(
                      BootstrapIntentKind::ReplaceInventoryItem,
                      "resources/core.pak")).ok() &&
                  committed.RecordUpdatedChildIdentity(originalChild).ok() &&
                  committed.RecordHealthState("accepted", true).ok() &&
                  committed.Apply(
                      intent(BootstrapIntentKind::CommitJournal)).ok() &&
                  committed.Apply(
                      intent(BootstrapIntentKind::RemoveBackup)).ok(),
              "committed-relaunch fixture reaches durable commit and terminal backup cleanup");
        const auto prepared =
            committed.PrepareRecoveredHealthRelaunch(originalChild);
        const auto decoded = DecodeWindowsBootstrapJournal(
            committedBackend.lastJournalBytes, &values.plan, &values.journal,
            nullptr);
        check(prepared.ok() && decoded.ok() &&
                   decoded.record->commitComplete &&
                   decoded.record->committedApplicationRelaunchPending &&
                   !decoded.record->updatedChildIdentity.valid &&
                   decoded.record->healthPhase.empty() &&
                   !decoded.record->healthTokenConsumed,
              "post-commit relaunch reset durably clears only the stale child and consumed health attempt");
    }

    ScriptedBackend recoveredBackend;
    recoveredBackend.entries = committedBackend.entries;
    recoveredBackend.contents = committedBackend.contents;
    recoveredBackend.metadatas = committedBackend.metadatas;
    recoveredBackend.backupDirectoryAlreadyExists =
        committedBackend.backupDirectoryAlreadyExists;
    recoveredBackend.backupDirectoryEmpty =
        committedBackend.backupDirectoryEmpty;
    bindRecoveryJournalCandidates(recoveredBackend, committedBackend);

    WindowsBootstrapFilesystemPort recovered(recoveredBackend);
    const auto recovery = recovered.BeginRecovery(values.plan, roots());
    check(recovery.ok(),
          "committed relaunch case 1: fresh recovery accepts the durable winner");
    check(recovery.disposition ==
              WindowsBootstrapRecoveryDisposition::CleanupCommitted &&
              recovery.intents.empty(),
          "committed relaunch case 2: cleaned commit still requires availability recovery without repeated cleanup");
    const auto prematureRetirement = recovered.RetireTerminalJournal();
    check(prematureRetirement.status ==
              WindowsBootstrapFilesystemStatus::InvalidIntentOrder,
          "committed relaunch case 3: pending availability recovery blocks terminal journal retirement");
    if (prematureRetirement.status !=
        WindowsBootstrapFilesystemStatus::InvalidIntentOrder) {
        return;
    }
    const BootstrapProcessIdentity relaunchedChild =
        processIdentity(302U, 0x62U);
    const auto childRecorded =
        recovered.RecordUpdatedChildIdentity(relaunchedChild);
    const auto healthRecorded = recovered.RecordHealthState("accepted", true);
    const auto decoded = DecodeWindowsBootstrapJournal(
        recoveredBackend.lastJournalBytes, &values.plan, &values.journal,
        nullptr);
    check(childRecorded.ok(),
          "committed relaunch case 4: fresh exact child identity is durably recorded");
    check(healthRecorded.ok(),
          "committed relaunch case 5: fresh health acceptance is durably recorded");
    check(decoded.ok() && decoded.record->commitComplete &&
              decoded.record->committedApplicationRelaunchPending &&
              decoded.record->updatedChildIdentity == relaunchedChild &&
              decoded.record->healthPhase == "accepted" &&
              decoded.record->healthTokenConsumed,
          "committed relaunch case 6: decoded winner binds the healthy replacement child");
    check(recovered.CleanupCommittedInvocation().ok() &&
              recovered.RetireTerminalJournal().ok(),
          "healthy committed relaunch can retire the terminal journal exactly once");
}

void test_restart_rejects_same_content_unknown_target_identity() {
    const Fixture values = fixture();
    ScriptedBackend crashed;
    {
        WindowsBootstrapFilesystemPort live(crashed);
        check(live.Begin(values.plan, values.journal, roots()).ok() &&
                  live.Apply(intent(BootstrapIntentKind::PersistJournal, {},
                                    "prepared")).ok() &&
                  live.Apply(intent(BootstrapIntentKind::SnapshotBackup,
                                    "HarkenSearch.exe")).ok(),
              "unknown-target fixture reaches a durable backup");
        crashed.failPersistCall = crashed.persistCalls + 3;
        const auto interrupted = live.Apply(
            intent(BootstrapIntentKind::ReplaceInventoryItem, "HarkenSearch.exe"));
        check(interrupted.status ==
                  WindowsBootstrapFilesystemStatus::PartialDurableState,
              "unknown-target fixture cuts after atomic publication");
    }
    // Preserve the signed payload bytes but substitute a different file ID.
    // Content equality alone must never authorize adopting this object.
    crashed.setByHandle(ScriptedBackend::kInstall, "HarkenSearch.exe",
                        WindowsFilesystemEntryKind::RegularFile, identity(99));
    const auto journalObservation = crashed.lookup(
        ScriptedBackend::kTransaction, "aris-bootstrap.journal");
    const auto journalContent = crashed.lookupContent(
        ScriptedBackend::kTransaction, "aris-bootstrap.journal");
    ScriptedBackend fresh;
    fresh.entries = crashed.entries;
    fresh.contents = crashed.contents;
    fresh.metadatas = crashed.metadatas;
    fresh.backupDirectoryAlreadyExists = crashed.backupDirectoryAlreadyExists;
    fresh.backupDirectoryEmpty = crashed.backupDirectoryEmpty;
    bindRecoveryJournalCandidates(fresh, crashed);
    WindowsBootstrapFilesystemPort recovered(fresh);
    const auto recovery = recovered.BeginRecovery(values.plan, values.journal,
                                                  roots());
    check(recovery.filesystem.status ==
              WindowsBootstrapFilesystemStatus::PartialDurableState &&
              fresh.persistCalls == 0 && fresh.promotionCalls == 0,
          "fresh recovery quarantines a same-content target with an unknown identity");
}

void test_restart_rejects_unknown_backup_child_before_journal_promotion() {
    const Fixture values = fixture();
    ScriptedBackend crashed;
    {
        WindowsBootstrapFilesystemPort live(crashed);
        check(live.Begin(values.plan, values.journal, roots()).ok() &&
                  live.Apply(intent(BootstrapIntentKind::PersistJournal, {},
                                    "prepared")).ok() &&
                  live.Apply(intent(BootstrapIntentKind::SnapshotBackup,
                                    "HarkenSearch.exe")).ok(),
              "unknown-backup fixture reaches a journal-bound durable backup");
    }
    const auto journalObservation = crashed.lookup(
        ScriptedBackend::kTransaction, "aris-bootstrap.journal");
    const auto journalContent = crashed.lookupContent(
        ScriptedBackend::kTransaction, "aris-bootstrap.journal");

    ScriptedBackend fresh;
    fresh.entries = crashed.entries;
    fresh.contents = crashed.contents;
    fresh.metadatas = crashed.metadatas;
    fresh.backupDirectoryAlreadyExists = crashed.backupDirectoryAlreadyExists;
    fresh.backupDirectoryEmpty = false;
    fresh.unknownBackupEntry = true;
    bindRecoveryJournalCandidates(fresh, crashed);

    WindowsBootstrapFilesystemPort recovered(fresh);
    const auto recovery = recovered.BeginRecovery(values.plan, values.journal,
                                                  roots());
    check(recovery.filesystem.status ==
              WindowsBootstrapFilesystemStatus::PartialDurableState &&
              recovery.disposition ==
                  WindowsBootstrapRecoveryDisposition::Quarantine,
          "restart quarantines an unknown backup child instead of adopting the namespace");
    check(fresh.promotionCalls == 0 && fresh.persistCalls == 0 &&
              std::none_of(fresh.calls.begin(), fresh.calls.end(),
                           [](const Call& call) {
                               return call.kind == CallKind::CopyFile ||
                                      call.kind == CallKind::ReplaceFile ||
                                      call.kind == CallKind::DeleteFile;
                           }),
          "unknown backup namespace rejection precedes journal promotion and every mutation");
}

void test_restart_rechecks_backup_namespace_before_promotion() {
    const Fixture values = fixture();
    ScriptedBackend crashed;
    {
        WindowsBootstrapFilesystemPort live(crashed);
        check(live.Begin(values.plan, values.journal, roots()).ok() &&
                  live.Apply(intent(BootstrapIntentKind::PersistJournal, {},
                                    "prepared")).ok() &&
                  live.Apply(intent(BootstrapIntentKind::SnapshotBackup,
                                    "HarkenSearch.exe")).ok(),
              "backup recheck fixture reaches a journal-bound durable backup");
    }

    ScriptedBackend fresh;
    fresh.entries = crashed.entries;
    fresh.contents = crashed.contents;
    fresh.metadatas = crashed.metadatas;
    fresh.backupDirectoryAlreadyExists = crashed.backupDirectoryAlreadyExists;
    fresh.backupDirectoryEmpty = crashed.backupDirectoryEmpty;
    // The first recovery preflight succeeds; the seam then models an unknown
    // child arriving before the backup handle is rebound.  A second preflight
    // must stop recovery before journal promotion or any install mutation.
    fresh.injectUnknownBackupEntryAfterFirstValidation = true;
    bindRecoveryJournalCandidates(fresh, crashed);

    WindowsBootstrapFilesystemPort recovered(fresh);
    const auto recovery = recovered.BeginRecovery(values.plan, roots());
    check(recovery.filesystem.status ==
              WindowsBootstrapFilesystemStatus::PartialDurableState &&
              recovery.disposition ==
                  WindowsBootstrapRecoveryDisposition::Quarantine,
          "recovery quarantines a backup namespace that changes before handle binding");
    check(fresh.promotionCalls == 0 && fresh.persistCalls == 0 &&
              std::none_of(fresh.calls.begin(), fresh.calls.end(),
                           [](const Call& call) {
                               return call.kind == CallKind::CopyFile ||
                                      call.kind == CallKind::ReplaceFile ||
                                      call.kind == CallKind::DeleteFile ||
                                      call.kind == CallKind::RemoveEmptyDirectory;
                           }),
          "backup namespace recheck precedes journal promotion and every install/cleanup mutation");
}

void test_restart_revalidates_backup_from_bound_handle() {
    const Fixture values = fixture();
    ScriptedBackend crashed;
    {
        WindowsBootstrapFilesystemPort live(crashed);
        check(live.Begin(values.plan, values.journal, roots()).ok() &&
                  live.Apply(intent(BootstrapIntentKind::PersistJournal, {},
                                    "prepared")).ok() &&
                  live.Apply(intent(BootstrapIntentKind::SnapshotBackup,
                                    "HarkenSearch.exe")).ok(),
              "bound-backup fixture reaches a journal-bound durable backup");
    }

    ScriptedBackend fresh;
    fresh.entries = crashed.entries;
    fresh.contents = crashed.contents;
    fresh.metadatas = crashed.metadatas;
    fresh.backupDirectoryAlreadyExists = crashed.backupDirectoryAlreadyExists;
    // Once recovery binds the original backup directory handle, a path-only
    // lookup is no longer authoritative.  Model a same-name path replacement
    // that the bound handle must survive.
    fresh.rejectPathValidationAfterHandleBound = true;
    bindRecoveryJournalCandidates(fresh, crashed);

    WindowsBootstrapFilesystemPort recovered(fresh);
    const auto recovery = recovered.BeginRecovery(values.plan, roots());
    check(recovery.ok() &&
              recovery.disposition ==
                  WindowsBootstrapRecoveryDisposition::ContinueForward &&
              fresh.boundBackupValidationCalls == 1,
          "recovery validates the backup namespace from the already-bound handle");
}

void test_restart_rebinds_only_the_exact_journal_bound_forward_temp() {
    const Fixture values = fixture();
    ScriptedBackend crashed;
    crashed.failAfterReplacementPrepared = true;
    {
        WindowsBootstrapFilesystemPort live(crashed);
        check(live.Begin(values.plan, values.journal, roots()).ok() &&
                  live.Apply(intent(BootstrapIntentKind::PersistJournal, {},
                                    "prepared")).ok() &&
                  live.Apply(intent(BootstrapIntentKind::SnapshotBackup,
                                    "HarkenSearch.exe")).ok(),
              "prepared-temp fixture reaches a durable backup");
        const auto interrupted = live.Apply(
            intent(BootstrapIntentKind::ReplaceInventoryItem, "HarkenSearch.exe"));
        check(!interrupted.ok(),
              "prepared-temp fixture cuts after the temp checkpoint and before publication");
    }

    const auto decoded = DecodeWindowsBootstrapJournal(crashed.lastJournalBytes);
    check(decoded.ok() && decoded.record->items.front().replacementPrepared,
          "prepared-temp crash leaves a strict durable identity checkpoint");
    if (!decoded.ok() || !decoded.record->items.front().replacementPrepared) {
        return;
    }
    const auto& item = decoded.record->items.front();
    const std::string temporaryPath = item.forwardTemporaryLeaf;
    crashed.setByHandle(ScriptedBackend::kInstall, temporaryPath,
                        WindowsFilesystemEntryKind::RegularFile, identity(250));

    const auto winner = crashed.lookup(
        ScriptedBackend::kTransaction, "aris-bootstrap.journal");
    const auto winnerContent = crashed.lookupContent(
        ScriptedBackend::kTransaction, "aris-bootstrap.journal");
    ScriptedBackend fresh;
    fresh.entries = crashed.entries;
    fresh.contents = crashed.contents;
    fresh.metadatas = crashed.metadatas;
    fresh.backupDirectoryAlreadyExists = crashed.backupDirectoryAlreadyExists;
    bindRecoveryJournalCandidates(fresh, crashed);

    WindowsBootstrapFilesystemPort recovered(fresh);
    const auto recovery = recovered.BeginRecovery(values.plan, values.journal,
                                                  roots());
    check(recovery.disposition ==
              WindowsBootstrapRecoveryDisposition::Quarantine &&
              recovery.filesystem.status ==
                  WindowsBootstrapFilesystemStatus::PartialDurableState,
          "restart rejects a same-content or attacker-substituted prepared temp with an unknown file identity");
    check(fresh.persistCalls == 0,
          "prepared-temp ambiguity is rejected before a new durable generation");
}

void test_restart_rebinds_only_the_exact_journal_bound_restore_temp() {
    const Fixture values = fixture();
    ScriptedBackend crashed;
    {
        WindowsBootstrapFilesystemPort live(crashed);
        check(live.Begin(values.plan, values.journal, roots()).ok() &&
                  live.Apply(intent(BootstrapIntentKind::PersistJournal, {},
                                    "prepared")).ok() &&
                  live.Apply(intent(BootstrapIntentKind::SnapshotBackup,
                                    "HarkenSearch.exe")).ok() &&
                  live.Apply(intent(BootstrapIntentKind::ReplaceInventoryItem,
                                    "HarkenSearch.exe")).ok() &&
                  live.Apply(intent(BootstrapIntentKind::ReplaceInventoryItem,
                                    "resources/core.pak")).ok() &&
                  live.Apply(intent(BootstrapIntentKind::PersistJournal, {},
                                    "rolling-back")).ok(),
              "restore-temp fixture reaches durable rollback authority");
        crashed.failAfterReplacementPrepared = true;
        const auto interrupted = live.Apply(
            intent(BootstrapIntentKind::RestoreBackup, "HarkenSearch.exe"));
        check(!interrupted.ok(),
              "restore-temp fixture cuts after the restore checkpoint and before publication");
    }

    const auto decoded = DecodeWindowsBootstrapJournal(crashed.lastJournalBytes);
    check(decoded.ok() && decoded.record->items.front().restorePrepared,
          "restore-temp crash leaves a strict durable identity checkpoint");
    if (!decoded.ok() || !decoded.record->items.front().restorePrepared) {
        return;
    }
    const auto& item = decoded.record->items.front();
    crashed.setByHandle(ScriptedBackend::kInstall,
                        item.restoreTemporaryLeaf,
                        WindowsFilesystemEntryKind::RegularFile,
                        identity(251));

    const auto winner = crashed.lookup(
        ScriptedBackend::kTransaction, "aris-bootstrap.journal");
    const auto winnerContent = crashed.lookupContent(
        ScriptedBackend::kTransaction, "aris-bootstrap.journal");
    ScriptedBackend fresh;
    fresh.entries = crashed.entries;
    fresh.contents = crashed.contents;
    fresh.metadatas = crashed.metadatas;
    fresh.backupDirectoryAlreadyExists = crashed.backupDirectoryAlreadyExists;
    bindRecoveryJournalCandidates(fresh, crashed);

    WindowsBootstrapFilesystemPort recovered(fresh);
    const auto recovery = recovered.BeginRecovery(values.plan, values.journal,
                                                  roots());
    check(recovery.disposition ==
              WindowsBootstrapRecoveryDisposition::Quarantine &&
              recovery.filesystem.status ==
                  WindowsBootstrapFilesystemStatus::PartialDurableState,
          "restart rejects a substituted rollback temp even when its bytes and metadata match");
    check(fresh.persistCalls == 0,
          "restore-temp ambiguity is rejected before a new durable generation");
}

void test_restart_rejects_stray_restore_temp_before_forward_completion() {
    const Fixture values = fixture();
    ScriptedBackend crashed;
    crashed.failAfterReplacementPrepared = true;
    {
        WindowsBootstrapFilesystemPort live(crashed);
        check(live.Begin(values.plan, values.journal, roots()).ok() &&
                  live.Apply(intent(BootstrapIntentKind::PersistJournal, {},
                                    "prepared")).ok() &&
                  live.Apply(intent(BootstrapIntentKind::SnapshotBackup,
                                    "HarkenSearch.exe")).ok(),
              "stray-restore fixture reaches a durable backup");
        check(!live.Apply(intent(BootstrapIntentKind::ReplaceInventoryItem,
                                 "HarkenSearch.exe")).ok(),
              "stray-restore fixture cuts before forward publication");
    }
    const auto decoded = DecodeWindowsBootstrapJournal(crashed.lastJournalBytes);
    check(decoded.ok() && !decoded.record->items.front().restoreStarted,
          "stray-restore fixture retains a pre-rollback journal generation");
    if (!decoded.ok()) return;
    const auto& item = decoded.record->items.front();
    crashed.setByHandle(ScriptedBackend::kInstall, item.restoreTemporaryLeaf,
                        WindowsFilesystemEntryKind::RegularFile, identity(252));
    const auto winner = crashed.lookup(
        ScriptedBackend::kTransaction, "aris-bootstrap.journal");
    const auto winnerContent = crashed.lookupContent(
        ScriptedBackend::kTransaction, "aris-bootstrap.journal");
    ScriptedBackend fresh;
    fresh.entries = crashed.entries;
    fresh.contents = crashed.contents;
    fresh.metadatas = crashed.metadatas;
    fresh.backupDirectoryAlreadyExists = crashed.backupDirectoryAlreadyExists;
    bindRecoveryJournalCandidates(fresh, crashed);
    WindowsBootstrapFilesystemPort recovered(fresh);
    const auto recovery = recovered.BeginRecovery(values.plan, values.journal,
                                                   roots());
    check(recovery.disposition ==
              WindowsBootstrapRecoveryDisposition::Quarantine &&
              recovery.filesystem.status ==
                  WindowsBootstrapFilesystemStatus::PartialDurableState,
          "restart rejects a restore temporary object before rollback has authority");
}

void test_restart_rejects_unbound_same_content_backup_after_started_cut() {
    const Fixture values = fixture();
    ScriptedBackend crashed;
    crashed.copyAccessDeniedFailuresRemaining = 1;
    {
        WindowsBootstrapFilesystemPort live(crashed);
        check(live.Begin(values.plan, values.journal, roots()).ok() &&
                  live.Apply(intent(BootstrapIntentKind::PersistJournal, {},
                                    "prepared")).ok(),
              "unbound-backup fixture publishes its initial journal");
        const auto interrupted = live.Apply(
            intent(BootstrapIntentKind::SnapshotBackup, "HarkenSearch.exe"));
        check(interrupted.status ==
                  WindowsBootstrapFilesystemStatus::RetryableAccessDenied,
              "unbound-backup fixture cuts after backup-started and before publication");
    }

    crashed.setByHandle(ScriptedBackend::kBackup, "item-0.bak",
                        WindowsFilesystemEntryKind::RegularFile, identity(249));
    crashed.setContentByHandle(ScriptedBackend::kBackup, "item-0.bak",
                               fingerprint(0x11, 900));
    crashed.setMetadataByHandle(ScriptedBackend::kBackup, "item-0.bak",
                                metadata(0x99));
    const auto winner = crashed.lookup(
        ScriptedBackend::kTransaction, "aris-bootstrap.journal");
    const auto winnerContent = crashed.lookupContent(
        ScriptedBackend::kTransaction, "aris-bootstrap.journal");
    ScriptedBackend fresh;
    fresh.entries = crashed.entries;
    fresh.contents = crashed.contents;
    fresh.metadatas = crashed.metadatas;
    fresh.backupDirectoryAlreadyExists = crashed.backupDirectoryAlreadyExists;
    bindRecoveryJournalCandidates(fresh, crashed);

    WindowsBootstrapFilesystemPort recovered(fresh);
    const auto recovery = recovered.BeginRecovery(values.plan, values.journal,
                                                  roots());
    check(recovery.disposition ==
              WindowsBootstrapRecoveryDisposition::Quarantine &&
              recovery.filesystem.status ==
                  WindowsBootstrapFilesystemStatus::PartialDurableState,
          "restart never adopts a backup identity from matching bytes alone");
    check(fresh.persistCalls == 0,
          "unbound backup rejection publishes no recovery progress");
}

void test_restart_from_prepared_state_can_apply_its_first_forward_intent() {
    const Fixture values = fixture();
    ScriptedBackend crashed;
    {
        WindowsBootstrapFilesystemPort live(crashed);
        check(live.Begin(values.plan, values.journal, roots()).ok() &&
                  live.Apply(intent(BootstrapIntentKind::PersistJournal, {},
                                    "prepared")).ok(),
              "prepared-restart fixture has no completed inventory mutation");
    }
    const auto winner = crashed.lookup(
        ScriptedBackend::kTransaction, "aris-bootstrap.journal");
    const auto winnerContent = crashed.lookupContent(
        ScriptedBackend::kTransaction, "aris-bootstrap.journal");
    ScriptedBackend fresh;
    fresh.entries = crashed.entries;
    fresh.contents = crashed.contents;
    fresh.metadatas = crashed.metadatas;
    fresh.backupDirectoryAlreadyExists = crashed.backupDirectoryAlreadyExists;
    bindRecoveryJournalCandidates(fresh, crashed);

    WindowsBootstrapFilesystemPort recovered(fresh);
    const auto recovery = recovered.BeginRecovery(values.plan, values.journal,
                                                  roots());
    const auto first = std::find_if(
        recovery.intents.begin(), recovery.intents.end(),
        [](const BootstrapIntent& value) {
            return value.kind == BootstrapIntentKind::SnapshotBackup;
        });
    const auto applied = first == recovery.intents.end()
        ? WindowsBootstrapFilesystemResult{
              WindowsBootstrapFilesystemStatus::InvalidIntentOrder}
        : recovered.Apply(*first);
    check(recovery.ok() &&
              recovery.disposition ==
                  WindowsBootstrapRecoveryDisposition::ContinueForward &&
              first != recovery.intents.end() && applied.ok(),
          "prepared restart does not invent terminal rollback flags before its first forward checkpoint");
}

void test_restart_reconciles_started_cleanup_after_namespace_removal() {
    const Fixture values = fixture();
    ScriptedBackend crashed;
    {
        WindowsBootstrapFilesystemPort live(crashed);
        check(live.Begin(values.plan, values.journal, roots()).ok(),
              "cleanup-reconcile fixture opens");
        check(live.Apply(intent(BootstrapIntentKind::PersistJournal, {},
                           "prepared")).ok() &&
                  live.Apply(intent(BootstrapIntentKind::SnapshotBackup,
                                    "HarkenSearch.exe")).ok() &&
                  live.Apply(intent(BootstrapIntentKind::ReplaceInventoryItem,
                                    "HarkenSearch.exe")).ok() &&
                  live.Apply(intent(BootstrapIntentKind::ReplaceInventoryItem,
                                    "resources/core.pak")).ok() &&
                  checkpointHealthyChildForCommit(live) &&
                  live.Apply(intent(BootstrapIntentKind::CommitJournal)).ok(),
              "cleanup-reconcile fixture commits the forward generation");
        crashed.failPersistCall = crashed.persistCalls + 2;
        const auto interrupted =
            live.Apply(intent(BootstrapIntentKind::RemoveBackup));
        check(!interrupted.ok() &&
                  crashed.lookup(ScriptedBackend::kBackup,
                                 "item-0.bak").kind ==
                      WindowsFilesystemEntryKind::Missing,
              "cleanup-reconcile fixture cuts after backup namespace removal");
    }
    const auto winner = crashed.lookup(
        ScriptedBackend::kTransaction, "aris-bootstrap.journal");
    const auto winnerContent = crashed.lookupContent(
        ScriptedBackend::kTransaction, "aris-bootstrap.journal");
    ScriptedBackend fresh;
    fresh.entries = crashed.entries;
    fresh.contents = crashed.contents;
    fresh.metadatas = crashed.metadatas;
    fresh.backupDirectoryAlreadyExists = crashed.backupDirectoryAlreadyExists;
    bindRecoveryJournalCandidates(fresh, crashed);
    WindowsBootstrapFilesystemPort recovered(fresh);
    const auto recovery = recovered.BeginRecovery(values.plan, values.journal,
                                                  roots());
    check(recovery.ok() &&
              recovery.disposition ==
                  WindowsBootstrapRecoveryDisposition::CleanupCommitted,
          "restart marks a started backup deletion complete when its exact object is already missing");
}

void test_exact_root_and_reparse_fail_before_mutation() {
    const Fixture values = fixture();
    {
        ScriptedBackend backend;
        backend.rootIdentities[WindowsFilesystemRootRole::Install] =
            identity(0x43U);
        WindowsBootstrapFilesystemPort port(backend);
        const auto rejected = port.Begin(values.plan, values.journal, roots());
        check(rejected.status ==
                  WindowsBootstrapFilesystemStatus::RootIdentityMismatch &&
                  durableCallsAfter(backend, 0U).empty(),
              "a post-submit install-root file-id swap is rejected before journal or mutation authority");
    }
    {
        ScriptedBackend liveBackend;
        WindowsBootstrapFilesystemPort live(liveBackend);
        check(live.Begin(values.plan, values.journal, roots()).ok() &&
                  live.Apply(intent(BootstrapIntentKind::PersistJournal, {},
                                    "prepared")).ok(),
              "root-swap recovery fixture publishes an authenticated journal");

        ScriptedBackend recoveredBackend;
        recoveredBackend.entries = liveBackend.entries;
        recoveredBackend.contents = liveBackend.contents;
        recoveredBackend.metadatas = liveBackend.metadatas;
        recoveredBackend.backupDirectoryAlreadyExists =
            liveBackend.backupDirectoryAlreadyExists;
        recoveredBackend.backupDirectoryEmpty = liveBackend.backupDirectoryEmpty;
        bindRecoveryJournalCandidates(recoveredBackend, liveBackend);
        recoveredBackend.rootIdentities[WindowsFilesystemRootRole::Install] =
            identity(0x43U);
        WindowsBootstrapFilesystemPort recovered(recoveredBackend);
        const auto rejected = recovered.BeginRecovery(
            values.plan, values.journal, roots());
        check(rejected.disposition ==
                  WindowsBootstrapRecoveryDisposition::Quarantine &&
                  durableCallsAfter(recoveredBackend, 0U).empty(),
              "recovery cannot adopt a newly opened install root whose file id differs from the durable plan authority");
    }
    {
        const Fixture literalTildeValues = crashFixture(
            "D:\\Tools\\R&D~Audio\\Aris",
            "D:\\Trusted\\HarkenSearchUpdater\\staging",
            "D:\\Trusted\\HarkenSearchUpdater\\transaction");
        ScriptedBackend backend;
        backend.rootFailures[WindowsFilesystemRootRole::Install] =
            WindowsFilesystemBackendStatus::ExactPathMismatch;
        WindowsBootstrapFilesystemPort port(backend);
        const auto rejected = port.Begin(literalTildeValues.plan,
                                         literalTildeValues.journal,
                                         roots());
        check(literalTildeValues.plan.isValidated() &&
                  rejected.status ==
                      WindowsBootstrapFilesystemStatus::RootIdentityMismatch &&
                  !hasMutation(backend),
              "literal tilde roots pass lexical validation but a simulated final-path alias mismatch fails closed");
    }
    {
        ScriptedBackend backend;
        backend.rootFailures[WindowsFilesystemRootRole::Install] =
            WindowsFilesystemBackendStatus::ExactPathMismatch;
        WindowsBootstrapFilesystemPort port(backend);
        const auto result = port.Begin(values.plan, values.journal, roots());
        check(result.status == WindowsBootstrapFilesystemStatus::RootIdentityMismatch,
              "install root final identity mismatch fails closed");
        check(!hasMutation(backend), "root identity rejection occurs before durable mutation");
    }
    {
        ScriptedBackend backend;
        backend.set(WindowsFilesystemRootRole::Staging, "resources/core.pak",
                    WindowsFilesystemEntryKind::ReparsePoint, identity(66));
        WindowsBootstrapFilesystemPort port(backend);
        const auto result = port.Begin(values.plan, values.journal, roots());
        check(result.status == WindowsBootstrapFilesystemStatus::ReparsePointRejected,
              "inventory reparse point fails closed");
        check(!hasMutation(backend), "reparse rejection occurs before durable mutation");
    }
    {
        ScriptedBackend backend;
        backend.set(WindowsFilesystemRootRole::Install, "resources",
                    WindowsFilesystemEntryKind::ReparsePoint, identity(67));
        WindowsBootstrapFilesystemPort port(backend);
        const auto result = port.Begin(values.plan, values.journal, roots());
        check(result.status == WindowsBootstrapFilesystemStatus::ReparsePointRejected,
              "intermediate install junction fails closed");
        check(!hasMutation(backend),
              "intermediate-component reparse is rejected before persistence");
    }
    {
        ScriptedBackend backend;
        backend.set(WindowsFilesystemRootRole::Staging, "resources/core.pak",
                    WindowsFilesystemEntryKind::RegularFile, identity(31));
        WindowsBootstrapFilesystemPort port(backend);
        const auto result = port.Begin(values.plan, values.journal, roots());
        check(result.status == WindowsBootstrapFilesystemStatus::IdentityMismatch,
              "distinct inventory spellings cannot alias one staged NTFS object");
        check(!hasMutation(backend),
              "staging short-name or case alias is rejected before persistence");
    }
}

void test_forged_write_roots_are_rejected_before_any_open() {
    const Fixture values = fixture();
    ScriptedBackend backend;
    backend.rejectTrustedRoots = true;
    WindowsBootstrapFilesystemPort port(backend);
    WindowsBootstrapFilesystemRoots forged = roots();
    forged.updaterOwnedBase = "D:\\Trusted\\Updater";
    forged.invocationIdentity = "signed-invocation";
    const auto rejected = port.Begin(values.plan, values.journal, forged);
    check(rejected.status == WindowsBootstrapFilesystemStatus::InvalidRoots &&
              std::none_of(backend.calls.begin(), backend.calls.end(),
                           [](const Call& call) {
                               return call.kind == CallKind::OpenRoot;
                           }),
          "a valid plan plus a forged same-volume write root cannot reach an open or mutation primitive");
#if defined(_WIN32)
    auto raw = rawPlan();
    // Keep this fixture in the CTest-owned out-of-tree system-temp directory.
    // Windows' default temp path is commonly under AppData, which the pure
    // install-target policy intentionally rejects as a data-directory target.
    const std::filesystem::path policySandbox =
        std::filesystem::current_path() /
        ("aris-bootstrap-policy-" +
         std::to_string(GetCurrentProcessId()));
    const std::filesystem::path policyInstall = policySandbox / "install";
    const std::filesystem::path policyUpdater = policySandbox / "updater";
    const std::filesystem::path policyStaging = policyUpdater / "staging";
    const std::filesystem::path policyTransaction =
        policyUpdater / "transactions";
    const auto cleanupPolicySandbox = [&policySandbox]() {
        std::error_code cleanupError;
        std::error_code existsError;
        const std::uintmax_t removed =
            std::filesystem::remove_all(policySandbox, cleanupError);
        const bool stillExists =
            std::filesystem::exists(policySandbox, existsError);
        if (cleanupError || existsError ||
            removed == static_cast<std::uintmax_t>(-1) || stillExists) {
            std::fprintf(
                stderr,
                "FAIL: trusted-root sandbox cleanup path=%s error=%s "
                "exists-error=%s removed=%llu exists=%u\n",
                policySandbox.u8string().c_str(),
                cleanupError ? cleanupError.message().c_str() : "none",
                existsError ? existsError.message().c_str() : "none",
                static_cast<unsigned long long>(removed),
                static_cast<unsigned int>(stillExists));
        }
        check(!cleanupError && !existsError &&
                  removed != static_cast<std::uintmax_t>(-1) &&
                  !stillExists,
              "native trusted-root fixture removes its exact temporary sandbox");
    };
    std::error_code policyCreateError;
    const bool policyCreated =
        std::filesystem::create_directories(policyInstall, policyCreateError) &&
        std::filesystem::create_directories(policyStaging, policyCreateError) &&
        std::filesystem::create_directories(policyTransaction,
                                             policyCreateError);
    check(policyCreated,
          "native trusted-root fixture creates its exact policy roots");
    if (!policyCreated) {
        cleanupPolicySandbox();
        return;
    }
    const bool policySecured =
        secureNativeTrustedDirectory(policyUpdater) &&
        secureNativeTrustedDirectory(policyStaging) &&
        secureNativeTrustedDirectory(policyTransaction);
    check(policySecured,
          "native trusted-root fixture protects its updater-owned roots");
    if (!policySecured) {
        cleanupPolicySandbox();
        return;
    }
    raw.identity.targetRoot = policyInstall.u8string();
    raw.identity.targetRootIdentity.canonicalPath = raw.identity.targetRoot;
    raw.invocation.parentProcess.imagePath =
        (policyInstall / "HarkenSearch.exe").u8string();
    raw.invocation.oldProcess.imagePath = raw.invocation.parentProcess.imagePath;
    raw.identity.oldExecutableIdentity.canonicalPath =
        raw.invocation.parentProcess.imagePath;
    raw.invocation.targetRootIdentityBinding = raw.identity.targetRootIdentity.canonicalPath;
    raw.invocation.oldExecutableIdentityBinding = raw.identity.oldExecutableIdentity.canonicalPath;
    raw.invocation.updaterOwnedBase = policyUpdater.u8string();
    raw.invocation.stagingRoot = policyStaging.u8string();
    raw.invocation.transactionRoot = policyTransaction.u8string();
    const auto validated = ValidateSignedInstallPlan(raw, SignatureStatus::Verified);
    check(validated.ok(), "native trusted-root fixture plan validates");
    if (validated.ok()) {
        WindowsBootstrapFilesystemTrustedRootPolicy policy;
        policy.updaterOwnedBase = policyUpdater.u8string();
        policy.invocationIdentity = "signed-invocation-01";
        policy.stagingRoot = policyStaging.u8string();
        policy.transactionRoot = policyTransaction.u8string();
        policy.expectedUpdaterOwnedBase = policyUpdater.u8string();
        policy.authenticatedParentProcess = validated.plan->invocation.parentProcess;
        policy.authenticated = true;
        const auto native = CreateNativeWindowsBootstrapFilesystemBackend(policy);
        WindowsBootstrapFilesystemRoots anchored{
            policyStaging.u8string(), policyTransaction.u8string(),
            policy.updaterOwnedBase,
            policy.invocationIdentity};
        check(native != nullptr &&
                  native->ValidateTrustedWriteRoots(*validated.plan, anchored).ok(),
              "native backend accepts only roots derived below its authenticated updater base");
        auto expectAnchoredDenied =
            [&native, &validated](WindowsBootstrapFilesystemRoots candidate,
                                  const char* message) {
                check(native != nullptr &&
                          native->ValidateTrustedWriteRoots(
                              *validated.plan, candidate)
                                  .status ==
                              WindowsFilesystemBackendStatus::AccessDenied,
                      message);
            };
        auto forgedBaseOnly = anchored;
        forgedBaseOnly.updaterOwnedBase =
            (policySandbox / "forged-base").u8string();
        expectAnchoredDenied(
            forgedBaseOnly,
            "native backend rejects an invocation with only its updater base changed");
        auto forgedTransactionOnly = anchored;
        forgedTransactionOnly.transactionRoot =
            (policyUpdater / "attacker-transactions").u8string();
        expectAnchoredDenied(
            forgedTransactionOnly,
            "native backend rejects an invocation with only its transaction root changed");
        forgedTransactionOnly.transactionRoot =
            (policySandbox / "forged" / "transactions").u8string();
        expectAnchoredDenied(
            forgedTransactionOnly,
            "native backend rejects an outside-base transaction root when every other signed field is unchanged");
        auto forgedInvocationOnly = anchored;
        forgedInvocationOnly.invocationIdentity = "forged-invocation-02";
        expectAnchoredDenied(
            forgedInvocationOnly,
            "native backend rejects an invocation with only its identity changed");
        anchored.stagingRoot = (policyUpdater / "attacker-staging").u8string();
        check(native->ValidateTrustedWriteRoots(*validated.plan, anchored).status ==
                  WindowsFilesystemBackendStatus::AccessDenied,
              "native backend rejects a caller-selected writable child even when it is below the trusted base");
        anchored.stagingRoot = (policySandbox / "forged" / "staging").u8string();
        check(native->ValidateTrustedWriteRoots(*validated.plan, anchored).status ==
                  WindowsFilesystemBackendStatus::AccessDenied,
              "native backend rejects a forged writable root on the same volume");
        auto forgedParentPolicy = policy;
        forgedParentPolicy.authenticatedParentProcess.processId++;
        const auto forgedParentNative =
            CreateNativeWindowsBootstrapFilesystemBackend(forgedParentPolicy);
        check(forgedParentNative != nullptr &&
                  forgedParentNative->ValidateTrustedWriteRoots(
                      *validated.plan,
                      WindowsBootstrapFilesystemRoots{
                          policyStaging.u8string(), policyTransaction.u8string(),
                          policy.updaterOwnedBase, policy.invocationIdentity})
                          .status == WindowsFilesystemBackendStatus::AccessDenied,
              "native backend rejects a policy with a substituted authenticated parent identity");

        WindowsBootstrapFilesystemTrustedRootPolicy forgedPolicy;
        forgedPolicy.updaterOwnedBase =
            (policySandbox / "forged").u8string();
        forgedPolicy.invocationIdentity = "signed-invocation-01";
        forgedPolicy.stagingRoot =
            (policySandbox / "forged" / "staging").u8string();
        forgedPolicy.transactionRoot =
            (policySandbox / "forged" / "transactions").u8string();
        forgedPolicy.authenticated = true;
        const auto forgedNative =
            CreateNativeWindowsBootstrapFilesystemBackend(forgedPolicy);
        const WindowsBootstrapFilesystemRoots forgedPolicyRoots{
            forgedPolicy.stagingRoot, forgedPolicy.transactionRoot,
            forgedPolicy.updaterOwnedBase,
            forgedPolicy.invocationIdentity};
        check(forgedNative != nullptr &&
                  forgedNative->ValidateTrustedWriteRoots(
                      *validated.plan, forgedPolicyRoots).status ==
                      WindowsFilesystemBackendStatus::AccessDenied,
              "caller-constructed policy cannot override signed invocation roots");

        WindowsBootstrapFilesystemTrustedRootPolicy forgedTransactionPolicy =
            policy;
        forgedTransactionPolicy.transactionRoot =
            (policyUpdater / "attacker-transactions").u8string();
        const auto forgedTransactionNative =
            CreateNativeWindowsBootstrapFilesystemBackend(
                forgedTransactionPolicy);
        check(forgedTransactionNative != nullptr &&
                  forgedTransactionNative->ValidateTrustedWriteRoots(
                      *validated.plan,
                      WindowsBootstrapFilesystemRoots{
                          policyStaging.u8string(),
                          forgedTransactionPolicy.transactionRoot,
                          policy.updaterOwnedBase,
                          policy.invocationIdentity})
                          .status ==
                      WindowsFilesystemBackendStatus::AccessDenied,
              "a transaction-only policy forgery cannot override the signed transaction root");

        WindowsBootstrapFilesystemTrustedRootPolicy forgedIdentityPolicy =
            policy;
        forgedIdentityPolicy.invocationIdentity = "forged-invocation-02";
        const auto forgedIdentityNative =
            CreateNativeWindowsBootstrapFilesystemBackend(
                forgedIdentityPolicy);
        check(forgedIdentityNative != nullptr &&
                  forgedIdentityNative->ValidateTrustedWriteRoots(
                      *validated.plan,
                      WindowsBootstrapFilesystemRoots{
                          policyStaging.u8string(),
                          policyTransaction.u8string(),
                          policy.updaterOwnedBase,
                          forgedIdentityPolicy.invocationIdentity})
                          .status ==
                      WindowsFilesystemBackendStatus::AccessDenied,
              "an identity-only policy forgery cannot override the signed invocation identity");

        WindowsBootstrapFilesystemTrustedRootPolicy unauthenticatedPolicy =
            policy;
        unauthenticatedPolicy.authenticated = false;
        const auto unauthenticatedNative =
            CreateNativeWindowsBootstrapFilesystemBackend(
                unauthenticatedPolicy);
        check(unauthenticatedNative != nullptr &&
                  unauthenticatedNative->ValidateTrustedWriteRoots(
                      *validated.plan,
                      WindowsBootstrapFilesystemRoots{
                          policyStaging.u8string(), policyTransaction.u8string(),
                          policy.updaterOwnedBase, policy.invocationIdentity})
                          .status ==
                      WindowsFilesystemBackendStatus::AccessDenied,
              "an unauthenticated policy cannot acquire any writable root capability");
    }
    cleanupPolicySandbox();
#endif
}

void test_failed_recovery_poison_blocks_every_later_mutation() {
    const Fixture values = fixture();
    ScriptedBackend backend;
    backend.rejectTrustedRoots = true;
    WindowsBootstrapFilesystemPort recovered(backend);

    const auto rejected = recovered.BeginRecovery(values.plan, values.journal,
                                                  roots());
    const std::size_t callsBeforeApply = backend.calls.size();
    const auto attempted = recovered.Apply(
        intent(BootstrapIntentKind::PersistJournal, {}, "prepared"));

    check(rejected.disposition ==
              WindowsBootstrapRecoveryDisposition::Quarantine &&
              attempted.status ==
                  WindowsBootstrapFilesystemStatus::TransactionBlocked,
          "a quarantined recovery permanently poisons its port capability");
    check(backend.calls.size() == callsBeforeApply && !hasMutation(backend),
          "a caller cannot ignore quarantine and reach a later mutation");
}

void test_untyped_journal_reason_never_reaches_durable_publication() {
    const Fixture values = fixture();
    ScriptedBackend backend;
    WindowsBootstrapFilesystemPort port(backend);
    check(port.Begin(values.plan, values.journal, roots()).ok() &&
              port.Apply(intent(BootstrapIntentKind::PersistJournal, {},
                                "prepared")).ok(),
          "journal-reason fixture reaches its initial durable generation");
    const int writesBefore = backend.persistCalls;

    const auto rejected = port.Apply(intent(
        BootstrapIntentKind::PersistJournal, {}, std::string(129U, 'x')));

    check(rejected.status ==
              WindowsBootstrapFilesystemStatus::UnsupportedIntent,
          "an arbitrary journal reason is not a filesystem journal capability");
    check(backend.persistCalls == writesBefore &&
              !backend.lastJournalBytes.empty(),
          "invalid phase text can never publish an empty journal generation");
}

void test_restart_recovery_emits_idempotent_rollback_for_complete_uncommitted_publish() {
    const Fixture values = fixture();
    ScriptedBackend crashed;
    {
        WindowsBootstrapFilesystemPort live(crashed);
        check(live.Begin(values.plan, values.journal, roots()).ok() &&
                  live.Apply(intent(BootstrapIntentKind::PersistJournal, {},
                                    "prepared")).ok() &&
                  live.Apply(intent(BootstrapIntentKind::SnapshotBackup,
                                    "HarkenSearch.exe")).ok() &&
                  live.Apply(intent(BootstrapIntentKind::ReplaceInventoryItem,
                                    "HarkenSearch.exe")).ok() &&
                  live.Apply(intent(BootstrapIntentKind::ReplaceInventoryItem,
                                    "resources/core.pak")).ok(),
              "rollback restart fixture completes all durable replacements");
    }
    const auto winner = crashed.lookup(
        ScriptedBackend::kTransaction, "aris-bootstrap.journal");
    const auto winnerContent = crashed.lookupContent(
        ScriptedBackend::kTransaction, "aris-bootstrap.journal");
    ScriptedBackend fresh;
    fresh.entries = crashed.entries;
    fresh.contents = crashed.contents;
    fresh.metadatas = crashed.metadatas;
    fresh.backupDirectoryAlreadyExists = crashed.backupDirectoryAlreadyExists;
    bindRecoveryJournalCandidates(fresh, crashed);
    WindowsBootstrapFilesystemPort recovered(fresh);
    const auto recovery = recovered.BeginRecovery(values.plan, values.journal,
                                                  roots());
    check(recovery.ok() &&
              recovery.disposition ==
                  WindowsBootstrapRecoveryDisposition::Rollback,
          "fresh recovery deterministically chooses rollback when publish is complete but health/commit is absent");
    for (const BootstrapIntent& recoveryIntent : recovery.intents) {
        check(recovered.Apply(recoveryIntent).ok(),
              "restart rollback intent is individually idempotent");
    }
    check(fresh.lookupContent(ScriptedBackend::kInstall,
                              "HarkenSearch.exe") == fingerprint(0x11, 900) &&
              fresh.lookupMetadata(ScriptedBackend::kInstall,
                                   "HarkenSearch.exe") == metadata(0x11) &&
              fresh.lookup(ScriptedBackend::kInstall,
                           "resources/core.pak").kind ==
                  WindowsFilesystemEntryKind::Missing,
          "restart rollback restores old bytes/metadata and removes the new item");
}

void test_restart_recovery_resumes_after_durable_rollback_checkpoints() {
    const Fixture values = fixture();
    ScriptedBackend crashed;
    {
        WindowsBootstrapFilesystemPort live(crashed);
        check(live.Begin(values.plan, values.journal, roots()).ok() &&
                  live.Apply(intent(BootstrapIntentKind::PersistJournal, {},
                                    "prepared")).ok() &&
                  live.Apply(intent(BootstrapIntentKind::SnapshotBackup,
                                    "HarkenSearch.exe")).ok() &&
                  live.Apply(intent(BootstrapIntentKind::ReplaceInventoryItem,
                                    "HarkenSearch.exe")).ok() &&
                  live.Apply(intent(BootstrapIntentKind::ReplaceInventoryItem,
                                    "resources/core.pak")).ok(),
              "rollback-checkpoint fixture reaches complete uncommitted publication");
    }

    const auto firstJournal = crashed.lookup(
        ScriptedBackend::kTransaction, "aris-bootstrap.journal");
    ScriptedBackend firstRecovery;
    firstRecovery.entries = crashed.entries;
    firstRecovery.contents = crashed.contents;
    firstRecovery.metadatas = crashed.metadatas;
    firstRecovery.backupDirectoryAlreadyExists =
        crashed.backupDirectoryAlreadyExists;
    firstRecovery.backupDirectoryEmpty = crashed.backupDirectoryEmpty;
    bindRecoveryJournalCandidates(firstRecovery, crashed);
    WindowsBootstrapFilesystemPort rollback(firstRecovery);
    const auto rollbackRecovery = rollback.BeginRecovery(
        values.plan, values.journal, roots());
    check(rollbackRecovery.ok() &&
              rollbackRecovery.disposition ==
                  WindowsBootstrapRecoveryDisposition::Rollback,
          "first restart selects rollback");
    for (const BootstrapIntent& rollbackIntent : rollbackRecovery.intents) {
        check(rollback.Apply(rollbackIntent).ok(),
              "first restart durably checkpoints each rollback mutation");
    }

    const auto rollbackJournal = firstRecovery.lookup(
        ScriptedBackend::kTransaction, "aris-bootstrap.journal");
    ScriptedBackend cleanupRecovery;
    cleanupRecovery.entries = firstRecovery.entries;
    cleanupRecovery.contents = firstRecovery.contents;
    cleanupRecovery.metadatas = firstRecovery.metadatas;
    cleanupRecovery.backupDirectoryAlreadyExists =
        firstRecovery.backupDirectoryAlreadyExists;
    cleanupRecovery.backupDirectoryEmpty = firstRecovery.backupDirectoryEmpty;
    bindRecoveryJournalCandidates(cleanupRecovery, firstRecovery);
    WindowsBootstrapFilesystemPort cleanup(cleanupRecovery);
    const auto resumed = cleanup.BeginRecovery(
        values.plan, values.journal, roots());
    const bool removesBackup = std::any_of(
        resumed.intents.begin(), resumed.intents.end(),
        [](const BootstrapIntent& resumedIntent) {
            return resumedIntent.kind == BootstrapIntentKind::RemoveBackup;
        });
    check(resumed.ok() &&
              resumed.disposition ==
                  WindowsBootstrapRecoveryDisposition::Rollback &&
              removesBackup,
          "fresh recovery accepts restored/missing targets and resumes rollback cleanup");
    if (removesBackup) {
        check(cleanup.Apply(intent(BootstrapIntentKind::RemoveBackup)).ok(),
              "resumed rollback removes its exact backup workspace");
    }

    const auto terminalJournal = cleanupRecovery.lookup(
        ScriptedBackend::kTransaction, "aris-bootstrap.journal");
    ScriptedBackend terminalRecovery;
    terminalRecovery.entries = cleanupRecovery.entries;
    terminalRecovery.contents = cleanupRecovery.contents;
    terminalRecovery.metadatas = cleanupRecovery.metadatas;
    terminalRecovery.backupDirectoryAlreadyExists =
        cleanupRecovery.backupDirectoryAlreadyExists;
    terminalRecovery.backupDirectoryEmpty = cleanupRecovery.backupDirectoryEmpty;
    bindRecoveryJournalCandidates(terminalRecovery, cleanupRecovery);
    WindowsBootstrapFilesystemPort terminal(terminalRecovery);
    const auto completed = terminal.BeginRecovery(
        values.plan, values.journal, roots());
    check(completed.ok() &&
              completed.disposition ==
                  WindowsBootstrapRecoveryDisposition::Complete,
          "fresh recovery accepts the terminal rollback generation after backup removal");
}

void test_write_ahead_journal_wraps_backup_and_replacement() {
    const Fixture values = fixture();
    ScriptedBackend backend;
    WindowsBootstrapFilesystemPort port(backend);
    check(port.Begin(values.plan, values.journal, roots()).ok(),
          "filesystem transaction opens exact trusted roots");

    const std::size_t prepareBegin = backend.calls.size();
    check(port.Apply(intent(BootstrapIntentKind::PersistJournal, {}, "prepared")).ok(),
          "prepared journal is durable before mutations");
    check(!backend.lastJournalBytes.empty(), "durable journal owns serialized adapter state");
    check(durableCallsAfter(backend, prepareBegin) ==
              std::vector<CallKind>{CallKind::PersistJournal,
                                    CallKind::PersistJournal,
                                    CallKind::CreateDirectory,
                                    CallKind::PersistJournal},
          "backup workspace creation is itself enclosed by durable state");
    std::vector<const Call*> preparePersists;
    for (std::size_t index = prepareBegin; index < backend.calls.size(); ++index) {
        if (backend.calls[index].kind == CallKind::PersistJournal) {
            preparePersists.push_back(&backend.calls[index]);
        }
    }
    check(preparePersists.size() == 3 &&
              preparePersists[0]->replaceMode ==
                  WindowsFilesystemReplaceMode::CreateNew &&
              !preparePersists[0]->expectedTarget.valid &&
              !preparePersists[0]->expectedTargetContent.valid &&
              preparePersists[1]->replaceMode ==
                  WindowsFilesystemReplaceMode::ReplaceExisting &&
              preparePersists[1]->expectedTarget.valid &&
              preparePersists[1]->expectedTargetContent.valid &&
              preparePersists[2]->replaceMode ==
                  WindowsFilesystemReplaceMode::ReplaceExisting &&
              preparePersists[2]->expectedTarget.valid &&
              preparePersists[2]->expectedTargetContent.valid,
          "initial journal is create-new and every later generation is identity-bound");

    std::size_t begin = backend.calls.size();
    check(port.Apply(intent(BootstrapIntentKind::SnapshotBackup, "HarkenSearch.exe")).ok(),
          "existing file snapshot succeeds");
    check(callsAfter(backend, begin) ==
              std::vector<CallKind>{CallKind::PersistJournal, CallKind::CopyFile,
                                    CallKind::PersistJournal},
          "backup is enclosed by write-ahead and completion journal flushes");
    const Call* backup = lastCallOf(backend, CallKind::CopyFile);
    check(backup != nullptr && backup->sourceRoot == ScriptedBackend::kInstall &&
              backup->targetRoot == ScriptedBackend::kBackup &&
              backup->sourcePath == "HarkenSearch.exe" &&
              backup->targetPath == "item-0.bak" &&
              backup->expectedSource == identity(21),
          "backup primitive is root-relative and bound to the observed old identity");
    check(backend.lookupMetadata(ScriptedBackend::kBackup, "item-0.bak") !=
              metadata(0x11),
          "backup publication does not import the old target security metadata");

    begin = backend.calls.size();
    check(port.Apply(intent(BootstrapIntentKind::ReplaceInventoryItem, "HarkenSearch.exe")).ok(),
          "existing inventory replacement succeeds");
    check(callsAfter(backend, begin) ==
              std::vector<CallKind>{CallKind::PersistJournal, CallKind::ReplaceFile,
                                    CallKind::PersistJournal,
                                    CallKind::PersistJournal},
          "replacement is enclosed by write-ahead and completion journal flushes");
    const Call* replacement = lastCallOf(backend, CallKind::ReplaceFile);
    check(replacement != nullptr &&
              replacement->sourceRoot == ScriptedBackend::kStaging &&
              replacement->targetRoot == ScriptedBackend::kInstall &&
              replacement->sourcePath == "HarkenSearch.exe" &&
              replacement->targetPath == "HarkenSearch.exe" &&
              replacement->temporaryLeaf ==
                  ".aris-bootstrap-attempt-fs-01-0-forward.tmp" &&
              replacement->expectedSource == identity(31) &&
              replacement->expectedSourceContent == fingerprint(0xdd, 1024) &&
              !replacement->expectedSourceMetadata.valid &&
              replacement->expectedTarget == identity(21) &&
              replacement->expectedTargetContent == fingerprint(0x11, 900) &&
              replacement->expectedTargetMetadata == metadata(0x11) &&
              replacement->replaceMode == WindowsFilesystemReplaceMode::ReplaceExisting,
          "replace primitive binds both observed identities and trusted root handles");
    check(backend.lookup(ScriptedBackend::kInstall, "HarkenSearch.exe").identity !=
              identity(31),
          "scripted publication returns a fresh target identity");
    check(backend.lookupMetadata(ScriptedBackend::kInstall, "HarkenSearch.exe") ==
              metadata(0x11) &&
              backend.lookupMetadata(ScriptedBackend::kInstall, "HarkenSearch.exe") !=
                  metadata(0xdd),
          "scripted replacement preserves target metadata instead of staging metadata");

    begin = backend.calls.size();
    check(port.Apply(intent(BootstrapIntentKind::ReplaceInventoryItem,
                            "resources/core.pak")).ok(),
          "new inventory item creation succeeds");
    check(callsAfter(backend, begin) ==
              std::vector<CallKind>{CallKind::PersistJournal, CallKind::ReplaceFile,
                                    CallKind::PersistJournal,
                                    CallKind::PersistJournal},
          "new-file creation has the same durable envelope");
    const Call* creation = lastCallOf(backend, CallKind::ReplaceFile);
    check(creation != nullptr &&
              creation->replaceMode == WindowsFilesystemReplaceMode::CreateNew &&
              creation->expectedSourceContent == fingerprint(0xee, 2048) &&
              !creation->expectedSourceMetadata.valid &&
              !creation->expectedTarget.valid &&
              !creation->expectedTargetContent.valid,
          "new inventory uses no-replace creation without fabricated target identity");
    check(backend.lookupMetadata(ScriptedBackend::kInstall, "resources/core.pak") !=
              metadata(0xee),
          "scripted create-new does not import staging metadata");
}

void test_signed_removal_deletes_restores_and_recovers_idempotently() {
    const Fixture values = removalFixture();
    check(std::any_of(values.journal.items.begin(), values.journal.items.end(),
                      [](const aris::bootstrap::ReplacementJournalItem& item) {
                          return item.relativePath == "legacy.dll" &&
                                 item.classification ==
                                     aris::bootstrap::JournalItemClass::RemoveExisting;
                      }),
          "dynamic inventory omission cannot suppress a release-signed removal");
    {
        ScriptedBackend absentBackend;
        absentBackend.setByHandle(ScriptedBackend::kInstall, "legacy.dll",
                                  WindowsFilesystemEntryKind::Missing, {});
        absentBackend.contents.erase(
            std::to_string(ScriptedBackend::kInstall) + ":legacy.dll");
        absentBackend.metadatas.erase(
            std::to_string(ScriptedBackend::kInstall) + ":legacy.dll");
        const std::size_t before = absentBackend.calls.size();
        bool applied = false;
        {
            WindowsBootstrapFilesystemPort absent(absentBackend);
            applied =
                absent.Begin(values.plan, values.journal, roots()).ok() &&
                absent.Apply(intent(BootstrapIntentKind::PersistJournal, {},
                                    "prepared")).ok() &&
                absent.Apply(intent(BootstrapIntentKind::SnapshotBackup,
                                    "legacy.dll")).ok() &&
                absent.Apply(intent(BootstrapIntentKind::RemoveInstalledItem,
                                    "legacy.dll")).ok();
        }
        const auto mutationCalls = callsAfter(absentBackend, before);
        check(applied &&
                  std::find(mutationCalls.begin(), mutationCalls.end(),
                            CallKind::DeleteFile) == mutationCalls.end(),
              "native observation records an absent signed removal as a durable no-op");

        ScriptedBackend absentRecovery;
        absentRecovery.entries = absentBackend.entries;
        absentRecovery.contents = absentBackend.contents;
        absentRecovery.metadatas = absentBackend.metadatas;
        absentRecovery.backupDirectoryAlreadyExists =
            absentBackend.backupDirectoryAlreadyExists;
        absentRecovery.backupDirectoryEmpty =
            absentBackend.backupDirectoryEmpty;
        bindRecoveryJournalCandidates(absentRecovery, absentBackend);
        WindowsBootstrapFilesystemPort recoveredAbsent(absentRecovery);
        const auto recovered =
            recoveredAbsent.BeginRecovery(values.plan, roots());
        if (!recovered.ok()) {
            std::fprintf(stderr,
                         "signed-removal absent recovery status=%u backend=%u native=%lu\n",
                         static_cast<unsigned>(recovered.filesystem.status),
                         static_cast<unsigned>(recovered.filesystem.backendStatus),
                         static_cast<unsigned long>(recovered.filesystem.nativeError));
        }
        check(recovered.ok() &&
                  !containsIntent(recovered.intents,
                                  BootstrapIntentKind::RemoveInstalledItem,
                                  "legacy.dll") &&
                  !containsIntent(recovered.intents,
                                  BootstrapIntentKind::RestoreBackup,
                                  "legacy.dll"),
              "fresh recovery preserves the durable absent-removal no-op");
    }
    {
        ScriptedBackend guardedBackend;
        WindowsBootstrapFilesystemPort guarded(guardedBackend);
        check(guarded.Begin(values.plan, values.journal, roots()).ok() &&
                  guarded.Apply(intent(BootstrapIntentKind::PersistJournal, {},
                                       "prepared")).ok() &&
                  guarded.Apply(intent(BootstrapIntentKind::SnapshotBackup,
                                       "HarkenSearch.exe")).ok() &&
                  guarded.Apply(intent(BootstrapIntentKind::SnapshotBackup,
                                       "legacy.dll")).ok() &&
                  guarded.Apply(intent(BootstrapIntentKind::PersistJournal, {},
                                       "rolling-back")).ok(),
              "late-removal guard fixture makes rollback durable after backups");
        const auto before =
            guardedBackend.lookup(ScriptedBackend::kInstall, "legacy.dll");
        const auto beforeContent = guardedBackend.lookupContent(
            ScriptedBackend::kInstall, "legacy.dll");
        const auto beforeMetadata = guardedBackend.lookupMetadata(
            ScriptedBackend::kInstall, "legacy.dll");
        const auto rejected = guarded.Apply(intent(
            BootstrapIntentKind::RemoveInstalledItem, "legacy.dll"));
        check(rejected.status ==
                  WindowsBootstrapFilesystemStatus::InvalidIntentOrder &&
                  guardedBackend.lookup(ScriptedBackend::kInstall,
                                        "legacy.dll").identity ==
                      before.identity &&
                  guardedBackend.lookupContent(ScriptedBackend::kInstall,
                                               "legacy.dll") == beforeContent &&
                  guardedBackend.lookupMetadata(ScriptedBackend::kInstall,
                                                "legacy.dll") == beforeMetadata,
              "durable rollback rejects a stale removal without changing identity, content, or metadata");
    }

    ScriptedBackend published;
    {
        WindowsBootstrapFilesystemPort live(published);
        check(live.Begin(values.plan, values.journal, roots()).ok(),
              "signed-removal transaction opens exact trusted roots");
        check(live.Apply(intent(BootstrapIntentKind::PersistJournal, {},
                                "prepared")).ok(),
              "signed-removal transaction publishes its prepared journal");
        check(live.Apply(intent(BootstrapIntentKind::SnapshotBackup,
                                "HarkenSearch.exe")).ok() &&
                  live.Apply(intent(BootstrapIntentKind::SnapshotBackup,
                                    "legacy.dll")).ok(),
              "all replacement and removal originals are durable before mutation");
        check(live.Apply(intent(BootstrapIntentKind::RemoveInstalledItem,
                                "legacy.dll")).ok() &&
                  published.lookup(ScriptedBackend::kInstall, "legacy.dll").kind ==
                      WindowsFilesystemEntryKind::Missing,
              "an exact signed removal durably deletes the package-owned file");

        const std::size_t idempotentBegin = published.calls.size();
        check(live.Apply(intent(BootstrapIntentKind::RemoveInstalledItem,
                                "legacy.dll")).ok() &&
                  callsAfter(published, idempotentBegin).empty(),
              "reapplying a completed signed removal is idempotent");
    }

    ScriptedBackend recoveryBackend;
    recoveryBackend.entries = published.entries;
    recoveryBackend.contents = published.contents;
    recoveryBackend.metadatas = published.metadatas;
    recoveryBackend.backupDirectoryAlreadyExists =
        published.backupDirectoryAlreadyExists;
    recoveryBackend.backupDirectoryEmpty = published.backupDirectoryEmpty;
    bindRecoveryJournalCandidates(recoveryBackend, published);
    WindowsBootstrapFilesystemPort recovered(recoveryBackend);
    const auto forward = recovered.BeginRecovery(values.plan, roots());
    check(forward.ok() &&
              forward.disposition ==
                  WindowsBootstrapRecoveryDisposition::ContinueForward &&
              !containsIntent(forward.intents,
                              BootstrapIntentKind::RemoveInstalledItem,
                              "legacy.dll"),
          "fresh recovery recognizes the exact durable deletion without repeating it");

    check(recovered.Apply(intent(BootstrapIntentKind::PersistJournal, {},
                                 "rolling-back")).ok() &&
              recovered.Apply(intent(BootstrapIntentKind::RestoreBackup,
                                     "legacy.dll")).ok(),
          "rollback recreates a signed removal from its journal-bound backup");
    const auto restored =
        recoveryBackend.lookup(ScriptedBackend::kInstall, "legacy.dll");
    check(restored.kind == WindowsFilesystemEntryKind::RegularFile &&
              recoveryBackend.lookupContent(ScriptedBackend::kInstall,
                                            "legacy.dll") ==
                  fingerprint(0x12, 300) &&
              recoveryBackend.lookupMetadata(ScriptedBackend::kInstall,
                                             "legacy.dll") == metadata(0x12),
          "rollback restores the original signed-removal content and metadata");

    const std::size_t restoreAgainBegin = recoveryBackend.calls.size();
    check(recovered.Apply(intent(BootstrapIntentKind::RestoreBackup,
                                 "legacy.dll")).ok() &&
              callsAfter(recoveryBackend, restoreAgainBegin).empty(),
          "reapplying a completed signed-removal restore is idempotent");
}

void test_recovery_rebinds_parent_pid_to_same_image_identity() {
    const Fixture values = fixture();
    ScriptedBackend published;
    {
        WindowsBootstrapFilesystemPort live(published);
        check(live.Begin(values.plan, values.journal, roots()).ok() &&
                  live.Apply(intent(BootstrapIntentKind::PersistJournal, {},
                                    "prepared")).ok(),
              "parent-restart fixture publishes an initial durable journal");
    }

    SignedInstallPlan restartedRaw = rawPlan();
    restartedRaw.invocation.parentProcess.processId = 701U;
    restartedRaw.invocation.parentProcess.creationTime = 1701U;
    // The updater image identity is unchanged; only its process lifetime tuple
    // differs after the updater itself restarts.
    const auto restarted =
        ValidateSignedInstallPlan(restartedRaw, SignatureStatus::Verified);
    check(restarted.ok(),
          "parent-restart fixture keeps a structurally valid signed plan");
    if (!restarted.ok()) return;

    ScriptedBackend rebound;
    rebound.entries = published.entries;
    rebound.contents = published.contents;
    rebound.metadatas = published.metadatas;
    rebound.backupDirectoryAlreadyExists =
        published.backupDirectoryAlreadyExists;
    rebound.backupDirectoryEmpty = published.backupDirectoryEmpty;
    bindRecoveryJournalCandidates(rebound, published);
    WindowsBootstrapFilesystemPort recovery(rebound);
    const auto recovered = recovery.BeginRecovery(*restarted.plan, roots());
    const auto durableRebound = DecodeWindowsBootstrapJournal(
        rebound.lastJournalBytes, &*restarted.plan, nullptr, nullptr);
    check(recovered.ok() && rebound.persistCalls >= 1 &&
              !rebound.recoveryPersistedBeforePromotion &&
              durableRebound.ok() &&
              durableRebound.record->parentProcessIdentity ==
                  restarted.plan->invocation.parentProcess,
          "fresh recovery promotes the selected durable generation before publishing a rebound parent lifetime");
}

void test_recovery_promotes_temp_before_parent_rebound_publication() {
    const Fixture values = fixture();
    ScriptedBackend published;
    {
        WindowsBootstrapFilesystemPort live(published);
        check(live.Begin(values.plan, values.journal, roots()).ok() &&
                  live.Apply(intent(BootstrapIntentKind::PersistJournal, {},
                                    "prepared")).ok(),
              "temporary parent-restart fixture publishes generation one");
    }
    std::vector<WindowsBootstrapJournalCandidate> candidates =
        recoveryCandidates(published);
    check(candidates.size() == 2U,
          "temporary parent-restart fixture retains the predecessor and winner generations");
    if (candidates.size() != 2U) return;
    candidates.back().name += ".tmp";
    candidates.back().temporary = true;

    SignedInstallPlan restartedRaw = rawPlan();
    restartedRaw.invocation.parentProcess.processId = 702U;
    restartedRaw.invocation.parentProcess.creationTime = 1702U;
    const auto restarted =
        ValidateSignedInstallPlan(restartedRaw, SignatureStatus::Verified);
    check(restarted.ok(),
          "temporary parent-restart fixture keeps the exact updater image binding");
    if (!restarted.ok()) return;

    ScriptedBackend rebound;
    rebound.entries = published.entries;
    rebound.contents = published.contents;
    rebound.metadatas = published.metadatas;
    rebound.backupDirectoryAlreadyExists =
        published.backupDirectoryAlreadyExists;
    rebound.backupDirectoryEmpty = published.backupDirectoryEmpty;
    rebound.setByHandle(ScriptedBackend::kTransaction,
                        "aris-bootstrap.journal",
                        WindowsFilesystemEntryKind::Missing, {});
    rebound.durableJournalCandidates = std::move(candidates);
    WindowsBootstrapFilesystemPort recovery(rebound);
    const auto recovered = recovery.BeginRecovery(*restarted.plan, roots());
    const auto durableRebound = DecodeWindowsBootstrapJournal(
        rebound.lastJournalBytes, &*restarted.plan, nullptr, nullptr);
    if (!recovered.ok() || rebound.promotionCalls != 1 ||
        rebound.persistCalls != 1 || rebound.recoveryPersistedBeforePromotion ||
        !durableRebound.ok() ||
        (durableRebound.ok() &&
         !(durableRebound.record->parentProcessIdentity ==
           restarted.plan->invocation.parentProcess))) {
        std::fprintf(
            stderr,
            "temporary parent-rebind status=%d backend=%d native=%u promotions=%d persists=%d before-promotion=%d decode=%d\n",
            static_cast<int>(recovered.filesystem.status),
            static_cast<int>(recovered.filesystem.backendStatus),
            recovered.filesystem.nativeError, rebound.promotionCalls,
            rebound.persistCalls,
            rebound.recoveryPersistedBeforePromotion ? 1 : 0,
            static_cast<int>(durableRebound.status));
    }
    check(recovered.ok() && rebound.promotionCalls == 1 &&
              rebound.persistCalls == 1 &&
              !rebound.recoveryPersistedBeforePromotion &&
              durableRebound.ok() &&
              durableRebound.record->parentProcessIdentity ==
                  restarted.plan->invocation.parentProcess,
          "temporary recovery promotes generation g before publishing parent-rebound generation g+1");
}

void test_mutation_requires_initial_durable_journal() {
    const Fixture values = fixture();
    ScriptedBackend backend;
    WindowsBootstrapFilesystemPort port(backend);
    check(port.Begin(values.plan, values.journal, roots()).ok(),
          "journal gate fixture opens");
    const std::size_t begin = backend.calls.size();
    const auto result = port.Apply(
        intent(BootstrapIntentKind::SnapshotBackup, "HarkenSearch.exe"));
    check(result.status == WindowsBootstrapFilesystemStatus::JournalNotDurable,
          "backup cannot begin before prepared journal durability");
    check(callsAfter(backend, begin).empty(),
          "journal gate emits no filesystem mutation");

    ScriptedBackend failingBackend;
    failingBackend.failPersistCall = 1;
    WindowsBootstrapFilesystemPort failingPort(failingBackend);
    check(failingPort.Begin(values.plan, values.journal, roots()).ok(),
          "initial journal failure fixture opens");
    const auto failedPrepare = failingPort.Apply(
        intent(BootstrapIntentKind::PersistJournal, {}, "prepared"));
    check(failedPrepare.status ==
              WindowsBootstrapFilesystemStatus::PartialDurableState &&
              failingPort.blocked(),
          "ambiguous initial journal flush fails closed");
    check(std::none_of(failingBackend.calls.begin(), failingBackend.calls.end(),
                       [](const Call& call) {
                           return call.kind == CallKind::CreateDirectory;
                       }),
          "backup workspace is not created after initial journal flush failure");
}

void test_started_journal_is_pinned_through_each_mutation() {
    const Fixture values = fixture();
    ScriptedBackend backend;
    backend.requireJournalLease = true;
    WindowsBootstrapFilesystemPort port(backend);
    check(port.Begin(values.plan, values.journal, roots()).ok(),
          "journal lease fixture opens");

    std::size_t begin = backend.calls.size();
    check(port.Apply(intent(BootstrapIntentKind::PersistJournal, {}, "prepared")).ok(),
          "backup workspace mutation holds the started journal lease");
    check(journalEnvelopeCallsAfter(backend, begin) ==
              std::vector<CallKind>{CallKind::PersistJournal,
                                    CallKind::PersistJournal,
                                    CallKind::AcquireJournalLease,
                                    CallKind::CreateDirectory,
                                    CallKind::ReleaseJournalLease,
                                    CallKind::PersistJournal},
          "journal lease is acquired after started WAL and released after mutation");

    begin = backend.calls.size();
    check(port.Apply(intent(BootstrapIntentKind::SnapshotBackup, "HarkenSearch.exe")).ok(),
          "backup copy runs while its authenticated WAL generation is pinned");
    check(journalEnvelopeCallsAfter(backend, begin) ==
              std::vector<CallKind>{CallKind::PersistJournal,
                                    CallKind::AcquireJournalLease,
                                    CallKind::CopyFile,
                                    CallKind::ReleaseJournalLease,
                                    CallKind::PersistJournal},
          "copy cannot escape the journal lease envelope");
    check(backend.unleasedMutationCalls == 0 && !backend.journalLeaseHeld,
          "no mutation occurs outside a lease and no lease escapes Apply");

    ScriptedBackend tamperedBackend;
    tamperedBackend.requireJournalLease = true;
    WindowsBootstrapFilesystemPort tamperedPort(tamperedBackend);
    check(tamperedPort.Begin(values.plan, values.journal, roots()).ok() &&
              tamperedPort.Apply(
                  intent(BootstrapIntentKind::PersistJournal, {}, "prepared")).ok(),
          "journal tamper fixture reaches prepared durable state");
    tamperedBackend.failJournalLease = true;
    begin = tamperedBackend.calls.size();
    const auto refused = tamperedPort.Apply(
        intent(BootstrapIntentKind::SnapshotBackup, "HarkenSearch.exe"));
    check(refused.status == WindowsBootstrapFilesystemStatus::PartialDurableState &&
              tamperedPort.blocked(),
          "journal identity or content loss before mutation blocks the transaction");
    check(journalEnvelopeCallsAfter(tamperedBackend, begin) ==
              std::vector<CallKind>{CallKind::PersistJournal,
                                    CallKind::AcquireJournalLease},
          "failed journal pin emits no copy and no completion generation");
    check(tamperedBackend.lookup(ScriptedBackend::kBackup, "item-0.bak").kind ==
              WindowsFilesystemEntryKind::Missing,
          "journal tamper before mutation leaves backup namespace untouched");
}

void test_journal_lease_access_denied_is_retryable_before_mutation() {
    const Fixture values = fixture();
    ScriptedBackend backend;
    WindowsBootstrapFilesystemPort port(backend);
    check(port.Begin(values.plan, values.journal, roots()).ok() &&
              port.Apply(intent(BootstrapIntentKind::PersistJournal, {},
                                "prepared")).ok(),
          "journal lease retry fixture reaches prepared state");
    backend.journalLeaseAccessDeniedFailuresRemaining = 1;
    const auto denied = port.Apply(
        intent(BootstrapIntentKind::SnapshotBackup, "HarkenSearch.exe"));
    check(denied.status ==
              WindowsBootstrapFilesystemStatus::RetryableAccessDenied &&
              denied.backendStatus ==
                  WindowsFilesystemBackendStatus::AccessDenied &&
              !port.blocked(),
          "journal lease sharing denial remains retryable before backup copy");
    const auto retried = port.Apply(
        intent(BootstrapIntentKind::SnapshotBackup, "HarkenSearch.exe"));
    check(retried.ok() && !port.blocked(),
          "backup copy succeeds after the transient lease denial clears");

    ScriptedBackend directoryBackend;
    WindowsBootstrapFilesystemPort directoryPort(directoryBackend);
    check(directoryPort.Begin(values.plan, values.journal, roots()).ok(),
          "journal lease directory retry fixture opens");
    directoryBackend.journalLeaseAccessDeniedFailuresRemaining = 1;
    const auto directoryDenied = directoryPort.Apply(
        intent(BootstrapIntentKind::PersistJournal, {}, "prepared"));
    check(directoryDenied.status ==
              WindowsBootstrapFilesystemStatus::RetryableAccessDenied &&
              !directoryPort.blocked(),
          "backup-directory lease denial remains retryable before creation");
    const auto directoryRetried = directoryPort.Apply(
        intent(BootstrapIntentKind::PersistJournal, {}, "prepared"));
    check(directoryRetried.ok() && !directoryPort.blocked(),
          "backup directory is created after the transient lease denial clears");
}

void test_backup_directory_create_denial_is_retryable_before_creation() {
    const Fixture values = fixture();
    ScriptedBackend backend;
    WindowsBootstrapFilesystemPort port(backend);
    backend.createDirectoryAccessDeniedFailuresRemaining = 1;
    check(port.Begin(values.plan, values.journal, roots()).ok(),
          "backup-directory create retry fixture opens");

    const auto denied = port.Apply(
        intent(BootstrapIntentKind::PersistJournal, {}, "prepared"));
    check(denied.status ==
              WindowsBootstrapFilesystemStatus::RetryableAccessDenied &&
              denied.backendStatus ==
                  WindowsFilesystemBackendStatus::AccessDenied &&
              !port.blocked() && !backend.backupDirectoryAlreadyExists,
          "backup-directory denial before creation remains retryable and leaves no directory");

    const auto retried = port.Apply(
        intent(BootstrapIntentKind::PersistJournal, {}, "prepared"));
    check(retried.ok() && !port.blocked() &&
              backend.backupDirectoryAlreadyExists,
          "backup directory is created after the pre-create denial clears");
}

void test_remove_new_access_denied_is_retryable_before_deletion() {
    const Fixture values = fixture();
    ScriptedBackend backend;
    WindowsBootstrapFilesystemPort port(backend);
    check(port.Begin(values.plan, values.journal, roots()).ok(),
          "new-file deletion retry fixture opens");
    applyInstalledFixture(port);
    backend.deleteAccessDeniedFailuresRemaining = 1;
    const auto denied = port.Apply(
        intent(BootstrapIntentKind::RemoveNewItem, "resources/core.pak"));
    check(denied.status ==
              WindowsBootstrapFilesystemStatus::RetryableAccessDenied &&
              denied.backendStatus ==
                  WindowsFilesystemBackendStatus::AccessDenied &&
              !port.blocked(),
          "new-file deletion sharing denial remains retryable");
    const auto retried = port.Apply(
        intent(BootstrapIntentKind::RemoveNewItem, "resources/core.pak"));
    check(retried.ok() && !port.blocked(),
          "new-file deletion succeeds after the transient denial clears");
}

void test_inventory_aliases_reserve_forward_and_restore_temporary_names() {
    SignedInstallPlan plan = rawPlan();
    const std::string forwardAlias =
        ".aris-bootstrap-attempt-fs-01-0-forward.tmp";
    const std::string restoreAlias =
        ".aris-bootstrap-attempt-fs-01-0-restore.tmp";
    plan.inventory.push_back({forwardAlias, std::string(64, '1'), 111});
    plan.inventory.push_back({restoreAlias, std::string(64, '2'), 222});
    const auto validated = ValidateSignedInstallPlan(plan, SignatureStatus::Verified);
    const auto built = validated.plan
        ? BuildReplacementJournal(*validated.plan,
                                  {{"HarkenSearch.exe"}, {"legacy.dll"}},
                                  "attempt-fs-01")
        : aris::bootstrap::JournalBuildResult{};
    check(validated.plan.has_value() && built.journal.has_value(),
          "temporary alias fixture validates as ordinary signed inventory");
    if (!validated.plan || !built.journal) return;

    ScriptedBackend backend;
    backend.set(WindowsFilesystemRootRole::Staging, forwardAlias,
                WindowsFilesystemEntryKind::RegularFile, identity(34));
    backend.set(WindowsFilesystemRootRole::Staging, restoreAlias,
                WindowsFilesystemEntryKind::RegularFile, identity(35));
    backend.set(WindowsFilesystemRootRole::Install, forwardAlias,
                WindowsFilesystemEntryKind::Missing, {});
    backend.set(WindowsFilesystemRootRole::Install, restoreAlias,
                WindowsFilesystemEntryKind::Missing, {});
    backend.setContentByHandle(ScriptedBackend::kStaging, forwardAlias,
                               fingerprint(0x11, 111));
    backend.setContentByHandle(ScriptedBackend::kStaging, restoreAlias,
                               fingerprint(0x22, 222));
    backend.setMetadataByHandle(ScriptedBackend::kStaging, forwardAlias,
                                metadata(0x11));
    backend.setMetadataByHandle(ScriptedBackend::kStaging, restoreAlias,
                                metadata(0x22));

    WindowsBootstrapFilesystemPort port(backend);
    check(port.Begin(*validated.plan, *built.journal, roots()).ok() &&
              port.Apply(intent(BootstrapIntentKind::PersistJournal, {},
                                "prepared")).ok() &&
              port.Apply(intent(BootstrapIntentKind::SnapshotBackup,
                                "HarkenSearch.exe")).ok() &&
              port.Apply(intent(BootstrapIntentKind::ReplaceInventoryItem,
                                "HarkenSearch.exe")).ok(),
          "inventory alias fixture replaces the owned executable");
    const Call* replacement = lastCallOf(backend, CallKind::ReplaceFile);
    check(replacement != nullptr &&
              replacement->temporaryLeaf ==
                  ".aris-bootstrap-attempt-fs-01-0-forward-1.tmp",
          "forward temporary name skips an inventory collision deterministically");
    check(port.Apply(intent(BootstrapIntentKind::PersistJournal, {},
                            "rolling-back")).ok() &&
              port.Apply(intent(BootstrapIntentKind::RestoreBackup,
                                "HarkenSearch.exe")).ok(),
          "rollback remains possible when base restore temp is real inventory");
    const Call* restore = lastCallOf(backend, CallKind::ReplaceFile);
    check(restore != nullptr &&
              restore->temporaryLeaf ==
                  ".aris-bootstrap-attempt-fs-01-0-restore-1.tmp",
          "restore temporary name skips an inventory collision deterministically");
}

void test_post_mutation_flush_failure_blocks_further_writes() {
    const Fixture values = fixture();
    ScriptedBackend backend;
    WindowsBootstrapFilesystemPort port(backend);
    check(port.Begin(values.plan, values.journal, roots()).ok(),
          "partial durability fixture opens");
    check(port.Apply(intent(BootstrapIntentKind::PersistJournal, {}, "prepared")).ok(),
          "partial durability fixture persists initial journal");
    backend.failPersistCall = backend.persistCalls + 2;
    const auto snapshot = port.Apply(
        intent(BootstrapIntentKind::SnapshotBackup, "HarkenSearch.exe"));
    check(snapshot.status == WindowsBootstrapFilesystemStatus::PartialDurableState &&
              port.blocked(),
          "post-copy journal failure marks transaction partial and blocked");
    const std::size_t begin = backend.calls.size();
    check(port.Apply(intent(BootstrapIntentKind::ReplaceInventoryItem, "HarkenSearch.exe")).status ==
              WindowsBootstrapFilesystemStatus::TransactionBlocked,
          "blocked transaction refuses later replacement");
    check(callsAfter(backend, begin).empty(),
          "blocked transaction emits no later write");
}

void test_pre_mutation_access_denial_can_retry_the_same_backup() {
    const Fixture values = fixture();
    ScriptedBackend backend;
    WindowsBootstrapFilesystemPort port(backend);
    check(port.Begin(values.plan, values.journal, roots()).ok() &&
              port.Apply(intent(BootstrapIntentKind::PersistJournal, {},
                                "prepared")).ok(),
          "backup retry fixture reaches the snapshot boundary");
    backend.copyAccessDeniedFailuresRemaining = 1;

    const auto denied = port.Apply(
        intent(BootstrapIntentKind::SnapshotBackup, "HarkenSearch.exe"));
    check(denied.status ==
              WindowsBootstrapFilesystemStatus::RetryableAccessDenied &&
              denied.backendStatus ==
                  WindowsFilesystemBackendStatus::AccessDenied,
          "pre-backup sharing denial is surfaced to the retry owner");
    check(!port.blocked(),
          "pre-backup sharing denial keeps the durable transaction retryable");

    const auto retried = port.Apply(
        intent(BootstrapIntentKind::SnapshotBackup, "HarkenSearch.exe"));
    check(retried.ok() && !port.blocked(),
          "the same backup succeeds after the transient lock clears");
    check(std::count_if(backend.calls.begin(), backend.calls.end(),
                        [](const Call& call) {
                            return call.kind == CallKind::CopyFile;
                        }) == 2,
          "backup retry invokes the identity-bound copy once per attempt");
}

void test_pre_mutation_access_denial_can_retry_the_same_replacement() {
    const Fixture values = fixture();
    ScriptedBackend backend;
    backend.replaceAccessDeniedFailuresRemaining = 1;
    WindowsBootstrapFilesystemPort port(backend);
    check(port.Begin(values.plan, values.journal, roots()).ok() &&
              port.Apply(intent(BootstrapIntentKind::PersistJournal, {},
                                "prepared")).ok() &&
              port.Apply(intent(BootstrapIntentKind::SnapshotBackup,
                                "HarkenSearch.exe")).ok(),
          "access-denial retry fixture reaches the replacement boundary");

    const auto denied = port.Apply(
        intent(BootstrapIntentKind::ReplaceInventoryItem, "HarkenSearch.exe"));
    check(denied.status ==
              WindowsBootstrapFilesystemStatus::RetryableAccessDenied &&
              denied.backendStatus ==
                  WindowsFilesystemBackendStatus::AccessDenied,
          "pre-mutation sharing denial is surfaced to the retry owner");
    check(!port.blocked(),
          "pre-mutation sharing denial keeps the durable transaction retryable");

    const auto retried = port.Apply(
        intent(BootstrapIntentKind::ReplaceInventoryItem, "HarkenSearch.exe"));
    check(retried.ok() && !port.blocked(),
          "the same replacement succeeds after the transient lock clears");
    check(std::count_if(backend.calls.begin(), backend.calls.end(),
                        [](const Call& call) {
                            return call.kind == CallKind::ReplaceFile;
                        }) == 2,
          "retry invokes the identity-bound primitive exactly once per attempt");
}

void test_pre_mutation_access_denial_can_retry_the_same_restore() {
    const Fixture values = fixture();
    ScriptedBackend backend;
    WindowsBootstrapFilesystemPort port(backend);
    check(port.Begin(values.plan, values.journal, roots()).ok() &&
              port.Apply(intent(BootstrapIntentKind::PersistJournal, {},
                                "prepared")).ok() &&
              port.Apply(intent(BootstrapIntentKind::SnapshotBackup,
                                "HarkenSearch.exe")).ok() &&
              port.Apply(intent(BootstrapIntentKind::ReplaceInventoryItem,
                                "HarkenSearch.exe")).ok() &&
              port.Apply(intent(BootstrapIntentKind::PersistJournal, {},
                                "rolling-back")).ok(),
          "restore retry fixture reaches durable rollback");
    backend.replaceAccessDeniedFailuresRemaining = 1;

    const auto denied = port.Apply(
        intent(BootstrapIntentKind::RestoreBackup, "HarkenSearch.exe"));
    check(denied.status ==
              WindowsBootstrapFilesystemStatus::RetryableAccessDenied &&
              denied.backendStatus ==
                  WindowsFilesystemBackendStatus::AccessDenied,
          "pre-restore sharing denial is surfaced to the retry owner");
    check(!port.blocked(),
          "pre-restore sharing denial keeps durable rollback retryable");

    const auto retried = port.Apply(
        intent(BootstrapIntentKind::RestoreBackup, "HarkenSearch.exe"));
    check(retried.ok() && !port.blocked(),
          "the same restore succeeds after the transient lock clears");
}

void test_pre_mutation_access_denied_can_retry_remove_backup() {
    const Fixture values = fixture();
    const auto reachCommittedTerminal = [&values](
        WindowsBootstrapFilesystemPort& port) {
        return port.Begin(values.plan, values.journal, roots()).ok() &&
               port.Apply(intent(BootstrapIntentKind::PersistJournal, {},
                                 "prepared")).ok() &&
               port.Apply(intent(BootstrapIntentKind::SnapshotBackup,
                                 "HarkenSearch.exe")).ok() &&
               port.Apply(intent(BootstrapIntentKind::ReplaceInventoryItem,
                                 "HarkenSearch.exe")).ok() &&
               port.Apply(intent(BootstrapIntentKind::ReplaceInventoryItem,
                                 "resources/core.pak")).ok() &&
               checkpointHealthyChildForCommit(port) &&
               port.Apply(intent(BootstrapIntentKind::CommitJournal)).ok();
    };

    {
        ScriptedBackend backend;
        backend.deleteAccessDeniedFailuresRemaining = 1;
        WindowsBootstrapFilesystemPort port(backend);
        check(reachCommittedTerminal(port),
              "remove-backup file retry fixture reaches committed terminal state");
        const auto denied =
            port.Apply(intent(BootstrapIntentKind::RemoveBackup));
        check(denied.status ==
                  WindowsBootstrapFilesystemStatus::RetryableAccessDenied &&
                  !port.blocked(),
              "pre-mutation backup-file denial remains retryable");
        check(port.Apply(intent(BootstrapIntentKind::RemoveBackup)).ok() &&
                  !port.blocked(),
              "backup-file cleanup retries after the transient denial clears");
    }

    {
        ScriptedBackend backend;
        backend.removeDirectoryAccessDeniedFailuresRemaining = 1;
        WindowsBootstrapFilesystemPort port(backend);
        check(reachCommittedTerminal(port),
              "remove-backup directory retry fixture reaches committed terminal state");
        const auto denied =
            port.Apply(intent(BootstrapIntentKind::RemoveBackup));
        check(denied.status ==
                  WindowsBootstrapFilesystemStatus::RetryableAccessDenied &&
                  !port.blocked(),
              "pre-mutation backup-directory denial remains retryable");
        check(port.Apply(intent(BootstrapIntentKind::RemoveBackup)).ok() &&
                  !port.blocked(),
              "backup-directory cleanup retries after the transient denial clears");
    }
}

void test_transient_journal_access_denial_retries_inside_persist() {
    const Fixture values = fixture();

    {
        ScriptedBackend backend;
        WindowsBootstrapFilesystemPort port(backend);
        check(port.Begin(values.plan, values.journal, roots()).ok() &&
                  port.Apply(intent(BootstrapIntentKind::PersistJournal, {},
                                    "prepared")).ok() &&
                  port.Apply(intent(BootstrapIntentKind::SnapshotBackup,
                                    "HarkenSearch.exe")).ok() &&
                  port.Apply(intent(BootstrapIntentKind::ReplaceInventoryItem,
                                    "HarkenSearch.exe")).ok() &&
                  port.Apply(intent(BootstrapIntentKind::ReplaceInventoryItem,
                                    "resources/core.pak")).ok() &&
                  checkpointHealthyChildForCommit(port) &&
                  port.Apply(intent(BootstrapIntentKind::CommitJournal)).ok() &&
                  port.Apply(intent(BootstrapIntentKind::RemoveBackup)).ok(),
              "persist retry cleanup-start fixture reaches terminal state");
        backend.persistAccessDeniedCall = backend.persistCalls + 1;
        const auto cleaned = port.CleanupCommittedInvocation();
        check(cleaned.ok() && !port.blocked(),
              "cleanup-start journal denial is retried before state escapes");
    }

    {
        ScriptedBackend backend;
        WindowsBootstrapFilesystemPort port(backend);
        check(port.Begin(values.plan, values.journal, roots()).ok() &&
                  port.Apply(intent(BootstrapIntentKind::PersistJournal, {},
                                    "prepared")).ok() &&
                  port.Apply(intent(BootstrapIntentKind::SnapshotBackup,
                                    "HarkenSearch.exe")).ok() &&
                  port.Apply(intent(BootstrapIntentKind::ReplaceInventoryItem,
                                    "HarkenSearch.exe")).ok() &&
                  port.Apply(intent(BootstrapIntentKind::ReplaceInventoryItem,
                                    "resources/core.pak")).ok() &&
                  checkpointHealthyChildForCommit(port) &&
                  port.Apply(intent(BootstrapIntentKind::CommitJournal)).ok() &&
                  port.Apply(intent(BootstrapIntentKind::RemoveBackup)).ok(),
              "persist retry cleanup-complete fixture reaches terminal state");
        backend.persistAccessDeniedCall = backend.persistCalls + 2;
        const auto cleaned = port.CleanupCommittedInvocation();
        check(cleaned.ok() && !port.blocked(),
              "cleanup-complete journal denial is retried before state escapes");
    }

    {
        ScriptedBackend backend;
        WindowsBootstrapFilesystemPort port(backend);
        check(port.Begin(values.plan, values.journal, roots()).ok() &&
                  port.Apply(intent(BootstrapIntentKind::PersistJournal, {},
                                    "prepared")).ok() &&
                  port.Apply(intent(BootstrapIntentKind::SnapshotBackup,
                                    "HarkenSearch.exe")).ok(),
              "persist retry replacement fixture reaches prepared state");
        backend.persistAccessDeniedCall = backend.persistCalls + 2;
        const auto replaced = port.Apply(
            intent(BootstrapIntentKind::ReplaceInventoryItem, "HarkenSearch.exe"));
        check(replaced.ok() && !port.blocked(),
              "replacement checkpoint journal denial is retried before publication");
    }

    {
        ScriptedBackend backend;
        WindowsBootstrapFilesystemPort port(backend);
        check(port.Begin(values.plan, values.journal, roots()).ok() &&
                  port.Apply(intent(BootstrapIntentKind::PersistJournal, {},
                                    "prepared")).ok() &&
                  port.Apply(intent(BootstrapIntentKind::SnapshotBackup,
                                    "HarkenSearch.exe")).ok() &&
                  port.Apply(intent(BootstrapIntentKind::ReplaceInventoryItem,
                                    "HarkenSearch.exe")).ok() &&
                  port.Apply(intent(BootstrapIntentKind::PersistJournal, {},
                                    "rolling-back")).ok(),
              "persist retry restore fixture reaches rollback state");
        backend.persistAccessDeniedCall = backend.persistCalls + 2;
        const auto restored =
            port.Apply(intent(BootstrapIntentKind::RestoreBackup, "HarkenSearch.exe"));
        check(restored.ok() && !port.blocked(),
              "restore checkpoint journal denial is retried before publication");
    }
}

void test_pre_mutation_flush_and_ordering_gates_emit_no_content_write() {
    const Fixture values = fixture();
    {
        ScriptedBackend backend;
        WindowsBootstrapFilesystemPort port(backend);
        check(port.Begin(values.plan, values.journal, roots()).ok(),
              "ordering fixture opens");
        check(port.Apply(intent(BootstrapIntentKind::PersistJournal, {}, "prepared")).ok(),
              "ordering fixture prepares journal");
        std::size_t begin = backend.calls.size();
        check(port.Apply(intent(BootstrapIntentKind::ReplaceInventoryItem,
                                "HarkenSearch.exe")).status ==
                  WindowsBootstrapFilesystemStatus::InvalidIntentOrder,
              "replacement waits until every required backup is complete");
        check(port.Apply(intent(BootstrapIntentKind::CommitJournal)).status ==
                  WindowsBootstrapFilesystemStatus::InvalidIntentOrder,
              "commit waits until every incoming item is complete");
        check(callsAfter(backend, begin).empty(),
              "ordering rejections emit no journal or content write");
    }
    {
        ScriptedBackend backend;
        WindowsBootstrapFilesystemPort port(backend);
        check(port.Begin(values.plan, values.journal, roots()).ok(),
              "pre-mutation WAL fixture opens");
        check(port.Apply(intent(BootstrapIntentKind::PersistJournal, {}, "prepared")).ok(),
              "pre-mutation WAL fixture prepares journal");
        backend.failPersistCall = backend.persistCalls + 1;
        const std::size_t begin = backend.calls.size();
        const auto snapshot = port.Apply(
            intent(BootstrapIntentKind::SnapshotBackup, "HarkenSearch.exe"));
        check(snapshot.status == WindowsBootstrapFilesystemStatus::PartialDurableState &&
                  port.blocked(),
              "ambiguous write-ahead flush blocks the transaction");
        check(std::none_of(backend.calls.begin() + static_cast<std::ptrdiff_t>(begin),
                           backend.calls.end(), [](const Call& call) {
                               return call.kind == CallKind::CopyFile;
                           }),
              "failed write-ahead flush prevents the backup copy");
    }
    {
        ScriptedBackend backend;
        WindowsBootstrapFilesystemPort port(backend);
        check(port.Begin(values.plan, values.journal, roots()).ok(),
              "journal identity race fixture opens");
        check(port.Apply(intent(BootstrapIntentKind::PersistJournal, {}, "prepared")).ok(),
              "journal identity race fixture prepares journal");
        backend.setByHandle(ScriptedBackend::kTransaction,
                            "aris-bootstrap.journal",
                            WindowsFilesystemEntryKind::RegularFile,
                            identity(97));
        const std::size_t begin = backend.calls.size();
        const auto snapshot = port.Apply(
            intent(BootstrapIntentKind::SnapshotBackup, "HarkenSearch.exe"));
        check(snapshot.status == WindowsBootstrapFilesystemStatus::PartialDurableState &&
                  port.blocked(),
              "changed durable journal identity blocks the transaction");
        check(std::none_of(backend.calls.begin() + static_cast<std::ptrdiff_t>(begin),
                           backend.calls.end(), [](const Call& call) {
                               return call.kind == CallKind::CopyFile;
                           }),
              "journal identity collision cannot be followed by content mutation");
    }
    {
        ScriptedBackend backend;
        WindowsBootstrapFilesystemPort port(backend);
        check(port.Begin(values.plan, values.journal, roots()).ok(),
              "journal content race fixture opens");
        check(port.Apply(intent(BootstrapIntentKind::PersistJournal, {}, "prepared")).ok(),
              "journal content race fixture prepares journal");
        backend.setContentByHandle(ScriptedBackend::kTransaction,
                                   "aris-bootstrap.journal",
                                   fingerprint(0xfe, 777));
        const std::size_t begin = backend.calls.size();
        const auto snapshot = port.Apply(
            intent(BootstrapIntentKind::SnapshotBackup, "HarkenSearch.exe"));
        check(snapshot.status == WindowsBootstrapFilesystemStatus::PartialDurableState &&
                  port.blocked(),
              "same-file-id journal content tampering blocks the transaction");
        check(std::none_of(backend.calls.begin() + static_cast<std::ptrdiff_t>(begin),
                           backend.calls.end(), [](const Call& call) {
                               return call.kind == CallKind::CopyFile;
                           }),
              "journal content tampering cannot precede a content mutation");
    }
    {
        ScriptedBackend backend;
        WindowsBootstrapFilesystemPort port(backend);
        check(port.Begin(values.plan, values.journal, roots()).ok(),
              "rollback ordering fixture opens");
        check(port.Apply(intent(BootstrapIntentKind::PersistJournal, {}, "prepared")).ok() &&
                  port.Apply(intent(BootstrapIntentKind::SnapshotBackup,
                                    "HarkenSearch.exe")).ok() &&
                  port.Apply(intent(BootstrapIntentKind::ReplaceInventoryItem,
                                    "HarkenSearch.exe")).ok() &&
                  port.Apply(intent(BootstrapIntentKind::ReplaceInventoryItem,
                                    "resources/core.pak")).ok(),
              "rollback ordering fixture installs complete inventory");
        std::size_t begin = backend.calls.size();
        check(port.Apply(intent(BootstrapIntentKind::RestoreBackup, "HarkenSearch.exe")).status ==
                  WindowsBootstrapFilesystemStatus::InvalidIntentOrder,
              "restore requires a durable rolling-back marker");
        check(callsAfter(backend, begin).empty(),
              "early rollback emits no filesystem mutation");
        check(port.Apply(intent(BootstrapIntentKind::PersistJournal, {},
                                "rolling-back")).ok(),
              "rollback selection becomes durable");
        begin = backend.calls.size();
        check(port.Apply(intent(BootstrapIntentKind::ReplaceInventoryItem,
                                "resources/core.pak")).status ==
                  WindowsBootstrapFilesystemStatus::InvalidIntentOrder,
              "stale forward intent is rejected after rollback selection");
        check(callsAfter(backend, begin).empty(),
              "rollback selection permanently closes forward mutation path");
    }
}

void test_unknown_target_and_existing_backup_are_refused() {
    const Fixture values = fixture();
    {
        ScriptedBackend backend;
        backend.set(WindowsFilesystemRootRole::Install, "resources/core.pak",
                    WindowsFilesystemEntryKind::RegularFile, identity(67));
        WindowsBootstrapFilesystemPort port(backend);
        const auto result = port.Begin(values.plan, values.journal, roots());
        check(result.status == WindowsBootstrapFilesystemStatus::UnknownTargetItem,
              "create-new journal refuses an unknown existing target");
        check(!hasMutation(backend), "unknown target is never overwritten");
    }
    {
        ScriptedBackend backend;
        backend.backupDirectoryAlreadyExists = true;
        WindowsBootstrapFilesystemPort port(backend);
        const auto result = port.Begin(values.plan, values.journal, roots());
        check(result.status == WindowsBootstrapFilesystemStatus::PartialDurableState,
              "unexpected pre-existing backup directory is treated as partial state");
        check(!hasMutation(backend), "partial backup state is not overwritten");
    }
    {
        ScriptedBackend backend;
        backend.setByHandle(ScriptedBackend::kTransaction,
                            "aris-bootstrap.journal",
                            WindowsFilesystemEntryKind::RegularFile,
                            identity(68));
        WindowsBootstrapFilesystemPort port(backend);
        const auto result = port.Begin(values.plan, values.journal, roots());
        check(result.status == WindowsBootstrapFilesystemStatus::JournalCollision,
              "unexpected durable journal is not overwritten without recovery parsing");
        check(!hasMutation(backend), "journal collision is rejected before writes");
    }
    {
        ScriptedBackend backend;
        backend.setByHandle(
            ScriptedBackend::kInstall,
            ".aris-bootstrap-attempt-fs-01-0-forward.tmp",
            WindowsFilesystemEntryKind::RegularFile, identity(69));
        WindowsBootstrapFilesystemPort port(backend);
        const auto result = port.Begin(values.plan, values.journal, roots());
        check(result.status == WindowsBootstrapFilesystemStatus::UnknownTargetItem,
              "unexpected replacement temp collision is preserved");
        check(!hasMutation(backend),
              "replacement temp collision is rejected before persistence");
    }
}

void applyInstalledFixture(WindowsBootstrapFilesystemPort& port) {
    check(port.Apply(intent(BootstrapIntentKind::PersistJournal, {}, "prepared")).ok(),
          "rollback fixture persists prepared journal");
    check(port.Apply(intent(BootstrapIntentKind::SnapshotBackup, "HarkenSearch.exe")).ok(),
          "rollback fixture snapshots old executable");
    check(port.Apply(intent(BootstrapIntentKind::ReplaceInventoryItem, "HarkenSearch.exe")).ok(),
          "rollback fixture replaces executable");
    check(port.Apply(intent(BootstrapIntentKind::ReplaceInventoryItem,
                            "resources/core.pak")).ok(),
          "rollback fixture creates new inventory file");
    check(port.Apply(intent(BootstrapIntentKind::PersistJournal, {}, "rolling-back")).ok(),
          "rollback phase is durable before recovery mutations");
}

void test_rollback_is_identity_bound_and_preserves_unknown_files() {
    const Fixture values = fixture();
    {
        ScriptedBackend backend;
        WindowsBootstrapFilesystemPort port(backend);
        check(port.Begin(values.plan, values.journal, roots()).ok(),
              "identity mismatch rollback fixture opens");
        applyInstalledFixture(port);
        backend.setByHandle(ScriptedBackend::kInstall, "HarkenSearch.exe",
                            WindowsFilesystemEntryKind::RegularFile, identity(99));
        const auto result = port.Apply(
            intent(BootstrapIntentKind::RestoreBackup, "HarkenSearch.exe"));
        check(result.status == WindowsBootstrapFilesystemStatus::IdentityMismatch &&
                  port.blocked(),
              "rollback refuses to replace an unknown changed target");
    }

    {
        ScriptedBackend backend;
        WindowsBootstrapFilesystemPort port(backend);
        check(port.Begin(values.plan, values.journal, roots()).ok(),
              "backup content mismatch rollback fixture opens");
        applyInstalledFixture(port);
        const WindowsFileIdentity installedIdentity =
            backend.lookup(ScriptedBackend::kInstall, "HarkenSearch.exe").identity;
        backend.setContentByHandle(ScriptedBackend::kBackup, "item-0.bak",
                                   fingerprint(0xfb, 900));
        const auto result = port.Apply(
            intent(BootstrapIntentKind::RestoreBackup, "HarkenSearch.exe"));
        check(result.status == WindowsBootstrapFilesystemStatus::ContentMismatch &&
                  port.blocked(),
              "rollback refuses same-file-id backup content tampering");
        check(backend.lookup(ScriptedBackend::kInstall, "HarkenSearch.exe").identity ==
                  installedIdentity,
              "tampered backup never replaces the installed target");
    }

    {
        ScriptedBackend backend;
        WindowsBootstrapFilesystemPort port(backend);
        check(port.Begin(values.plan, values.journal, roots()).ok(),
              "new-file content mismatch rollback fixture opens");
        applyInstalledFixture(port);
        backend.setContentByHandle(ScriptedBackend::kInstall,
                                   "resources/core.pak",
                                   fingerprint(0xfd, 2048));
        const auto result = port.Apply(
            intent(BootstrapIntentKind::RemoveNewItem, "resources/core.pak"));
        check(result.status == WindowsBootstrapFilesystemStatus::ContentMismatch &&
                  port.blocked(),
              "rollback preserves an in-place changed new file");
        check(backend.lookup(ScriptedBackend::kInstall,
                             "resources/core.pak").kind ==
                  WindowsFilesystemEntryKind::RegularFile,
              "changed new file remains for recovery inspection");
    }

    {
        ScriptedBackend backend;
        backend.setByHandle(ScriptedBackend::kInstall, "user-note.txt",
                            WindowsFilesystemEntryKind::RegularFile, identity(70));
        WindowsBootstrapFilesystemPort port(backend);
        check(port.Begin(values.plan, values.journal, roots()).ok(),
              "successful rollback fixture opens");
        applyInstalledFixture(port);
        const WindowsFileIdentity installedExecutable =
            backend.lookup(ScriptedBackend::kInstall, "HarkenSearch.exe").identity;
        const WindowsFileIdentity installedNewItem =
            backend.lookup(ScriptedBackend::kInstall,
                           "resources/core.pak").identity;
        std::size_t begin = backend.calls.size();
        check(port.Apply(intent(BootstrapIntentKind::RestoreBackup, "HarkenSearch.exe")).ok(),
              "rollback restores the exact backed-up executable");
        check(callsAfter(backend, begin) ==
                  std::vector<CallKind>{CallKind::PersistJournal, CallKind::ReplaceFile,
                                        CallKind::PersistJournal,
                                        CallKind::PersistJournal},
              "restore is write-ahead journaled");
        const Call* restore = lastCallOf(backend, CallKind::ReplaceFile);
        check(restore != nullptr &&
                  restore->sourceRoot == ScriptedBackend::kBackup &&
                  restore->targetRoot == ScriptedBackend::kInstall &&
                  restore->sourcePath == "item-0.bak" &&
                  restore->temporaryLeaf ==
                      ".aris-bootstrap-attempt-fs-01-0-restore.tmp" &&
                  restore->expectedSourceContent == fingerprint(0x11, 900) &&
                  restore->expectedTarget == installedExecutable &&
                  restore->expectedTargetContent == fingerprint(0xdd, 1024),
              "restore is bound to the recorded backup and installed identities");
        begin = backend.calls.size();
        check(port.Apply(intent(BootstrapIntentKind::RemoveNewItem,
                                "resources/core.pak")).ok(),
              "rollback removes only the recorded new item");
        check(callsAfter(backend, begin) ==
                  std::vector<CallKind>{CallKind::PersistJournal, CallKind::DeleteFile,
                                        CallKind::PersistJournal},
              "new-file cleanup is write-ahead journaled");
        const Call* cleanup = lastCallOf(backend, CallKind::DeleteFile);
        check(cleanup != nullptr && cleanup->sourceRoot == ScriptedBackend::kInstall &&
                  cleanup->sourcePath == "resources/core.pak" &&
                  cleanup->expectedSource == installedNewItem &&
                  cleanup->expectedSourceContent == fingerprint(0xee, 2048),
              "new-file cleanup deletes only the recorded installed identity");
        check(backend.lookup(ScriptedBackend::kInstall, "user-note.txt").kind ==
                  WindowsFilesystemEntryKind::RegularFile,
              "unknown file outside journal remains untouched");
        check(std::none_of(backend.calls.begin(), backend.calls.end(), [](const Call& call) {
                  return call.sourcePath == "user-note.txt" || call.targetPath == "user-note.txt";
              }),
              "unknown file is never passed to a mutation primitive");

        begin = backend.calls.size();
        check(port.Apply(intent(BootstrapIntentKind::RestoreBackup, "HarkenSearch.exe")).ok() &&
                  port.Apply(intent(BootstrapIntentKind::RemoveNewItem,
                                    "resources/core.pak")).ok(),
              "restore and new-file cleanup replay idempotently");
        check(callsAfter(backend, begin).empty(),
              "rollback replay emits no second destructive operation");
    }
}

void test_rollback_metadata_is_journal_bound_and_tamper_checked() {
    const Fixture values = fixture();
    ScriptedBackend backend;
    WindowsBootstrapFilesystemPort port(backend);
    check(port.Begin(values.plan, values.journal, roots()).ok() &&
              port.Apply(intent(BootstrapIntentKind::PersistJournal, {},
                                "prepared")).ok() &&
              port.Apply(intent(BootstrapIntentKind::SnapshotBackup,
                                "HarkenSearch.exe")).ok(),
          "metadata binding fixture reaches a durable backup snapshot");
    const auto decoded = DecodeWindowsBootstrapJournal(
        backend.lastJournalBytes, &values.plan, &values.journal, nullptr);
    bool metadataBound = false;
    if (decoded.ok()) {
        const auto recorded = std::find_if(
            decoded.record->items.begin(), decoded.record->items.end(),
            [](const auto& item) { return item.relativePath == "HarkenSearch.exe"; });
        metadataBound = recorded != decoded.record->items.end() &&
                        recorded->rollbackMetadata == metadata(0x11);
    }
    check(metadataBound,
          "adapter journal durably binds the opaque rollback metadata bytes");
    backend.setMetadataByHandle(ScriptedBackend::kInstall, "HarkenSearch.exe",
                                metadata(0x66));
    const WindowsBootstrapFilesystemResult replaced = port.Apply(
        intent(BootstrapIntentKind::ReplaceInventoryItem, "HarkenSearch.exe"));
    check(replaced.status ==
              WindowsBootstrapFilesystemStatus::UnsupportedMetadata &&
              port.blocked(),
          "same-id same-content target metadata tampering blocks replacement");
    check(backend.lookup(ScriptedBackend::kInstall, "HarkenSearch.exe").identity ==
              identity(21),
          "metadata mismatch is rejected before target publication");
}

void test_root_alias_and_forward_identity_race_are_blocked() {
    const Fixture values = fixture();
    {
        ScriptedBackend backend;
        WindowsBootstrapFilesystemPort port(backend);
        check(port.Begin(values.plan, values.journal, roots()).ok(),
              "single-session fixture opens");
        const std::size_t begin = backend.calls.size();
        check(port.Begin(values.plan, values.journal, roots()).status ==
                  WindowsBootstrapFilesystemStatus::InvalidIntentOrder,
              "active filesystem session cannot be silently replaced");
        check(backend.calls.size() == begin,
              "second Begin preserves the active root handles and state");
    }
    {
        ScriptedBackend backend;
        backend.rootIdentities[WindowsFilesystemRootRole::Staging] =
            identity(0x42U);
        WindowsBootstrapFilesystemPort port(backend);
        const auto opened = port.Begin(values.plan, values.journal, roots());
        check(opened.status == WindowsBootstrapFilesystemStatus::RootIdentityMismatch,
              "staging root cannot alias the install root object");
        check(!hasMutation(backend), "aliased roots are rejected before persistence");
    }
    {
        ScriptedBackend backend;
        backend.rootIdentities[WindowsFilesystemRootRole::Transaction] =
            identity(42, 78);
        WindowsBootstrapFilesystemPort port(backend);
        const auto opened = port.Begin(values.plan, values.journal, roots());
        check(opened.status == WindowsBootstrapFilesystemStatus::RootVolumeMismatch,
              "all transaction roots must share a volume for handle-relative rename");
        check(!hasMutation(backend),
              "cross-volume transaction is rejected before persistence");
    }
    {
        ScriptedBackend backend;
        WindowsBootstrapFilesystemPort port(backend);
        WindowsBootstrapFilesystemRoots overlappingRoots = roots();
        overlappingRoots.stagingRoot = "D:\\Apps\\Aris\\payload";
        const auto opened = port.Begin(values.plan, values.journal,
                                       overlappingRoots);
        check(opened.status == WindowsBootstrapFilesystemStatus::InvalidRoots,
              "caller cannot override signed staging roots with an install-overlapping path");
        check(!hasMutation(backend),
              "root ancestry overlap is rejected before persistence");
    }
    {
        ScriptedBackend backend;
        WindowsBootstrapFilesystemPort port(backend);
        check(port.Begin(values.plan, values.journal, roots()).ok(),
              "forward identity race fixture opens");
        check(port.Apply(intent(BootstrapIntentKind::PersistJournal, {}, "prepared")).ok(),
              "forward identity race fixture prepares journal");
        backend.setByHandle(ScriptedBackend::kInstall, "HarkenSearch.exe",
                            WindowsFilesystemEntryKind::RegularFile, identity(73));
        const auto snapshot = port.Apply(
            intent(BootstrapIntentKind::SnapshotBackup, "HarkenSearch.exe"));
        check(snapshot.status == WindowsBootstrapFilesystemStatus::IdentityMismatch &&
                  port.blocked(),
              "changed old target identity is never copied as an owned backup");
        check(backend.lookup(ScriptedBackend::kBackup, "item-0.bak").kind ==
                  WindowsFilesystemEntryKind::Missing,
              "identity mismatch creates no backup content");
    }
    {
        ScriptedBackend backend;
        WindowsBootstrapFilesystemPort port(backend);
        check(port.Begin(values.plan, values.journal, roots()).ok(),
              "staging content race fixture opens");
        check(port.Apply(intent(BootstrapIntentKind::PersistJournal, {}, "prepared")).ok() &&
                  port.Apply(intent(BootstrapIntentKind::SnapshotBackup,
                                    "HarkenSearch.exe")).ok(),
              "staging content race fixture prepares all backups");
        backend.setContentByHandle(ScriptedBackend::kStaging, "HarkenSearch.exe",
                                   fingerprint(0xfa, 1024));
        const auto replacement = port.Apply(
            intent(BootstrapIntentKind::ReplaceInventoryItem, "HarkenSearch.exe"));
        check(replacement.status == WindowsBootstrapFilesystemStatus::ContentMismatch &&
                  port.blocked(),
              "same-file-id staging tamper fails signed size/hash verification");
        check(backend.lookup(ScriptedBackend::kInstall, "HarkenSearch.exe").identity ==
                  identity(21),
              "content mismatch preserves the observed install target");
    }
    {
        ScriptedBackend backend;
        WindowsBootstrapFilesystemPort port(backend);
        check(port.Begin(values.plan, values.journal, roots()).ok(),
              "old-target content race fixture opens");
        check(port.Apply(intent(BootstrapIntentKind::PersistJournal, {}, "prepared")).ok() &&
                  port.Apply(intent(BootstrapIntentKind::SnapshotBackup,
                                    "HarkenSearch.exe")).ok(),
              "old-target content race fixture captures exact old bytes");
        backend.setContentByHandle(ScriptedBackend::kInstall, "HarkenSearch.exe",
                                   fingerprint(0xfc, 900));
        const auto replacement = port.Apply(
            intent(BootstrapIntentKind::ReplaceInventoryItem, "HarkenSearch.exe"));
        check(replacement.status == WindowsBootstrapFilesystemStatus::ContentMismatch &&
                  port.blocked(),
              "same-file-id old-target change is not overwritten");
        check(backend.lookupContent(ScriptedBackend::kInstall, "HarkenSearch.exe") ==
                  fingerprint(0xfc, 900),
              "changed old-target content remains intact");
    }
}

void test_install_parent_identity_swap_is_blocked() {
    const Fixture values = fixture();
    ScriptedBackend backend;
    WindowsBootstrapFilesystemPort port(backend);
    check(port.Begin(values.plan, values.journal, roots()).ok() &&
              port.Apply(intent(BootstrapIntentKind::PersistJournal, {},
                                "prepared")).ok() &&
              port.Apply(intent(BootstrapIntentKind::SnapshotBackup,
                                "HarkenSearch.exe")).ok(),
          "install parent identity fixture reaches replacement boundary");
    backend.setByHandle(ScriptedBackend::kInstall, "resources",
                        WindowsFilesystemEntryKind::Directory, identity(99));
    const auto replacement = port.Apply(
        intent(BootstrapIntentKind::ReplaceInventoryItem,
               "resources/core.pak"));
    check(replacement.status == WindowsBootstrapFilesystemStatus::IdentityMismatch &&
              port.blocked(),
          "swapped regular install directory fails closed before create-new");
    const Call* call = lastCallOf(backend, CallKind::ReplaceFile);
    check(call != nullptr && call->expectedTargetParent == identity(23),
          "replace primitive receives the parent identity observed by Begin");
    check(backend.lookup(ScriptedBackend::kInstall,
                         "resources/core.pak").kind ==
              WindowsFilesystemEntryKind::Missing,
          "parent identity ambiguity leaves the target namespace untouched");
}

void test_create_new_race_and_non_filesystem_intent_fail_closed() {
    const Fixture values = fixture();
    ScriptedBackend backend;
    WindowsBootstrapFilesystemPort port(backend);
    check(port.Begin(values.plan, values.journal, roots()).ok(),
          "create-new race fixture opens");
    check(port.Apply(intent(BootstrapIntentKind::PersistJournal, {}, "prepared")).ok(),
          "create-new race fixture prepares journal");
    check(port.Apply(intent(BootstrapIntentKind::SnapshotBackup, "HarkenSearch.exe")).ok(),
          "all old items are backed up before race check");

    std::size_t begin = backend.calls.size();
    const auto processIntent = port.Apply(
        intent(BootstrapIntentKind::LaunchUpdatedChild, "HarkenSearch.exe"));
    check(processIntent.status == WindowsBootstrapFilesystemStatus::UnsupportedIntent,
          "filesystem port explicitly rejects process intents");
    check(callsAfter(backend, begin).empty(),
          "process intent cannot reach a filesystem mutation primitive");

    backend.setByHandle(ScriptedBackend::kInstall, "resources/core.pak",
                        WindowsFilesystemEntryKind::RegularFile, identity(72));
    const auto raced = port.Apply(
        intent(BootstrapIntentKind::ReplaceInventoryItem, "resources/core.pak"));
    check(raced.status == WindowsBootstrapFilesystemStatus::UnknownTargetItem &&
              port.blocked(),
          "create-new no-replace primitive refuses a post-observation collision");
}

void test_temporary_publication_collision_never_overwrites_unknown_file() {
    const Fixture values = fixture();
    ScriptedBackend backend;
    WindowsBootstrapFilesystemPort port(backend);
    check(port.Begin(values.plan, values.journal, roots()).ok() &&
              port.Apply(intent(BootstrapIntentKind::PersistJournal, {},
                                "prepared")).ok() &&
              port.Apply(intent(BootstrapIntentKind::SnapshotBackup,
                                "HarkenSearch.exe")).ok(),
          "temporary collision fixture reaches replacement boundary");
    backend.temporaryCollisionOnReplace = true;
    const auto replacement = port.Apply(
        intent(BootstrapIntentKind::ReplaceInventoryItem, "HarkenSearch.exe"));
    check(replacement.status == WindowsBootstrapFilesystemStatus::UnknownTargetItem &&
              port.blocked(),
          "exclusive temporary create collision fails closed");
    check(backend.lookup(ScriptedBackend::kInstall, "HarkenSearch.exe").identity ==
              identity(21) &&
              backend.lookupContent(ScriptedBackend::kInstall, "HarkenSearch.exe") ==
                  fingerprint(0x11, 900),
          "temporary collision preserves the exact old target");
}

void test_commit_precedes_cleanup_and_unknown_backup_blocks_only_cleanup() {
    const Fixture values = fixture();
    ScriptedBackend backend;
    WindowsBootstrapFilesystemPort port(backend);
    check(port.Begin(values.plan, values.journal, roots()).ok(),
          "commit cleanup fixture opens");
    check(port.Apply(intent(BootstrapIntentKind::PersistJournal, {}, "prepared")).ok(),
          "commit cleanup fixture prepares journal");
    check(port.Apply(intent(BootstrapIntentKind::SnapshotBackup, "HarkenSearch.exe")).ok(),
          "commit cleanup fixture snapshots old executable");
    check(port.Apply(intent(BootstrapIntentKind::ReplaceInventoryItem, "HarkenSearch.exe")).ok() &&
              port.Apply(intent(BootstrapIntentKind::ReplaceInventoryItem,
                                 "resources/core.pak")).ok(),
          "commit cleanup fixture writes complete inventory");

    check(checkpointHealthyChildForCommit(port),
          "commit cleanup fixture records accepted child health");
    const std::size_t begin = backend.calls.size();
    check(port.Apply(intent(BootstrapIntentKind::CommitJournal)).ok(),
          "commit marker becomes durable after complete replacement");
    backend.backupDirectoryEmpty = false;
    const auto cleanup = port.Apply(intent(BootstrapIntentKind::RemoveBackup));
    check(cleanup.status == WindowsBootstrapFilesystemStatus::CleanupPending &&
              port.blocked(),
          "unknown backup child preserves committed install and leaves cleanup pending");
    const auto calls = callsAfter(backend, begin);
    const auto commitPersist = std::find(calls.begin(), calls.end(),
                                         CallKind::PersistJournal);
    const auto firstDelete = std::find(calls.begin(), calls.end(), CallKind::DeleteFile);
    check(commitPersist != calls.end() && firstDelete != calls.end() &&
              commitPersist < firstDelete,
          "durable commit marker precedes every backup delete");
    check(std::none_of(backend.calls.begin() + static_cast<std::ptrdiff_t>(begin),
                       backend.calls.end(), [](const Call& call) {
                           return call.kind == CallKind::DeleteFile &&
                                  call.sourceRoot == ScriptedBackend::kInstall;
                       }),
          "commit cleanup never deletes install-root files");
    const auto backupClosed = std::find_if(
        backend.calls.begin() + static_cast<std::ptrdiff_t>(begin),
        backend.calls.end(), [](const Call& call) {
            return call.kind == CallKind::CloseHandle &&
                   call.sourceRoot == ScriptedBackend::kBackup;
        });
    const auto directoryRemoved = std::find_if(
        backend.calls.begin() + static_cast<std::ptrdiff_t>(begin),
        backend.calls.end(), [](const Call& call) {
            return call.kind == CallKind::RemoveEmptyDirectory;
        });
    check(backupClosed != backend.calls.end() &&
              directoryRemoved != backend.calls.end() &&
              backupClosed < directoryRemoved,
          "backup root handle closes before exact directory removal");
}

void test_successful_commit_cleanup_replays_idempotently() {
    const Fixture values = fixture();
    ScriptedBackend backend;
    WindowsBootstrapFilesystemPort port(backend);
    check(port.Begin(values.plan, values.journal, roots()).ok(),
          "successful commit replay fixture opens");
    check(port.Apply(intent(BootstrapIntentKind::PersistJournal, {}, "prepared")).ok() &&
              port.Apply(intent(BootstrapIntentKind::SnapshotBackup,
                                "HarkenSearch.exe")).ok() &&
              port.Apply(intent(BootstrapIntentKind::ReplaceInventoryItem,
                                "HarkenSearch.exe")).ok() &&
              port.Apply(intent(BootstrapIntentKind::ReplaceInventoryItem,
                                 "resources/core.pak")).ok() &&
              checkpointHealthyChildForCommit(port) &&
              port.Apply(intent(BootstrapIntentKind::CommitJournal)).ok() &&
              port.Apply(intent(BootstrapIntentKind::RemoveBackup)).ok(),
          "successful commit removes only known backup state");
    const std::size_t begin = backend.calls.size();
    check(port.Apply(intent(BootstrapIntentKind::CommitJournal)).ok() &&
              port.Apply(intent(BootstrapIntentKind::RemoveBackup)).ok(),
          "commit and cleanup replay idempotently after backup handle close");
    check(callsAfter(backend, begin).empty(),
          "terminal commit replay emits no second durable or destructive write");

    backend.backupObserveAccessDeniedFailuresRemaining = 1;
    const auto denied = port.Apply(intent(BootstrapIntentKind::RemoveBackup));
    check(denied.status ==
              WindowsBootstrapFilesystemStatus::RetryableAccessDenied &&
              denied.backendStatus ==
                  WindowsFilesystemBackendStatus::AccessDenied &&
              !port.blocked(),
          "terminal backup absence recheck keeps a transient observation denial retryable");
    check(port.Apply(intent(BootstrapIntentKind::RemoveBackup)).ok() &&
              !port.blocked(),
          "terminal backup absence recheck succeeds after the observation denial clears");

    backend.backupDirectoryAlreadyExists = true;
    const auto recreated = port.Apply(intent(BootstrapIntentKind::RemoveBackup));
    check(recreated.status == WindowsBootstrapFilesystemStatus::PartialDurableState &&
              port.blocked(),
          "same-session cleanup rechecks a recreated backup namespace instead of silently accepting it");
}

void test_schema_nine_persists_runtime_pair_cleanup_authority() {
    const Fixture values = fixture();
    ScriptedBackend backend;
    backend.setByHandle(ScriptedBackend::kTransaction, "runtime-pair",
                        WindowsFilesystemEntryKind::Directory, identity(61U));
    backend.setByHandle(ScriptedBackend::kTransaction,
                        "runtime-pair/HarkenSearchBootstrap.exe",
                        WindowsFilesystemEntryKind::RegularFile, identity(62U));
    backend.setContentByHandle(ScriptedBackend::kTransaction,
                               "runtime-pair/HarkenSearchBootstrap.exe",
                               fingerprint(0xabU, 4096U));
    WindowsBootstrapFilesystemPort port(backend);
    check(port.Begin(values.plan, values.journal, roots()).ok() &&
              port.Apply(intent(BootstrapIntentKind::PersistJournal, {},
                                "prepared")).ok() &&
              port.Apply(intent(BootstrapIntentKind::SnapshotBackup,
                                "HarkenSearch.exe")).ok() &&
              port.Apply(intent(BootstrapIntentKind::ReplaceInventoryItem,
                                "HarkenSearch.exe")).ok() &&
              port.Apply(intent(BootstrapIntentKind::ReplaceInventoryItem,
                                "resources/core.pak")).ok() &&
              checkpointHealthyChildForCommit(port) &&
              port.Apply(intent(BootstrapIntentKind::CommitJournal)).ok() &&
              port.Apply(intent(BootstrapIntentKind::RemoveBackup)).ok() &&
              port.CleanupCommittedInvocation().ok(),
          "schema 9 fixture reaches a durable cleanup-complete cut");
    const auto encoded = backend.lastJournalBytes;
    check(!encoded.empty(),
          "schema 9 encodes runtime-pair cleanup authority");
    if (encoded.empty()) return;
    const auto decoded = DecodeWindowsBootstrapJournal(encoded);
    check(decoded.ok() && decoded.record->schemaVersion == 9U &&
              decoded.record->runtimePairExpected &&
              decoded.record->runtimePairDirectoryIdentity == identity(61U) &&
              decoded.record->runtimePairBootstrapIdentity == identity(62U) &&
              decoded.record->runtimePairBootstrapContent == fingerprint(0xabU, 4096U) &&
              decoded.record->invocationCleanupStarted &&
              decoded.record->invocationCleanupComplete,
          "schema 9 round-trips runtime-pair identity/content and cleanup state");

    std::vector<std::uint8_t> legacy = encoded;
    legacy[11] = 8U;
    check(DecodeWindowsBootstrapJournal(legacy).status ==
              WindowsBootstrapJournalDecodeStatus::UnsupportedVersion,
          "schema 8 is rejected instead of guessing missing cleanup authority");
}

void test_committed_invocation_cleanup_is_terminal_and_identity_bound() {
    const Fixture values = fixture();
    ScriptedBackend backend;
    backend.setByHandle(ScriptedBackend::kTransaction, "runtime-pair",
                        WindowsFilesystemEntryKind::Directory, identity(61));
    backend.setByHandle(ScriptedBackend::kTransaction,
                        "runtime-pair/HarkenSearchBootstrap.exe",
                        WindowsFilesystemEntryKind::RegularFile, identity(62));
    backend.setContentByHandle(ScriptedBackend::kTransaction,
                               "runtime-pair/HarkenSearchBootstrap.exe",
                               fingerprint(0xab, 4096));
    WindowsBootstrapFilesystemPort port(backend);

    check(port.Begin(values.plan, values.journal, roots()).ok(),
          "invocation cleanup fixture opens");
    const auto premature = port.CleanupCommittedInvocation();
    check(premature.status == WindowsBootstrapFilesystemStatus::InvalidIntentOrder,
          "invocation cleanup is rejected before committed health and backup cleanup");
    check(std::none_of(backend.calls.begin(), backend.calls.end(),
                       [](const Call& call) {
                           return call.kind == CallKind::CleanupInvocation;
                       }),
          "premature invocation cleanup emits no destructive call");

    check(port.Apply(intent(BootstrapIntentKind::PersistJournal, {},
                            "prepared")).ok() &&
              port.Apply(intent(BootstrapIntentKind::SnapshotBackup,
                                "HarkenSearch.exe")).ok() &&
              port.Apply(intent(BootstrapIntentKind::ReplaceInventoryItem,
                                "HarkenSearch.exe")).ok() &&
              port.Apply(intent(BootstrapIntentKind::ReplaceInventoryItem,
                                "resources/core.pak")).ok() &&
              checkpointHealthyChildForCommit(port) &&
              port.Apply(intent(BootstrapIntentKind::CommitJournal)).ok(),
          "invocation cleanup fixture reaches durable commit");
    const auto beforeBackup = port.CleanupCommittedInvocation();
    check(beforeBackup.status == WindowsBootstrapFilesystemStatus::InvalidIntentOrder,
          "invocation cleanup waits for durable backup removal");
    check(port.Apply(intent(BootstrapIntentKind::RemoveBackup)).ok(),
          "invocation cleanup fixture removes backup first");

    const auto cleaned = port.CleanupCommittedInvocation();
    check(cleaned.ok(),
          "committed invocation cleanup removes only the bound residue");
    const Call* cleanupCall = lastCallOf(backend, CallKind::CleanupInvocation);
    check(cleanupCall != nullptr &&
              cleanupCall->stagingFiles.size() == values.plan.inventory.size() &&
              cleanupCall->transactionFiles.size() == 1U &&
              cleanupCall->transactionFiles.front().relativePath ==
                  "runtime-pair/HarkenSearchBootstrap.exe",
          "cleanup call carries exact staging and runtime-pair identities");

    backend.setByHandle(ScriptedBackend::kStaging, "HarkenSearch.exe",
                        WindowsFilesystemEntryKind::RegularFile, identity(99));
    const auto replay = port.CleanupCommittedInvocation();
    check(replay.status == WindowsBootstrapFilesystemStatus::IdentityMismatch &&
              port.blocked(),
          "identity drift blocks cleanup instead of deleting a replacement");
}

void test_absent_runtime_pair_authority_blocks_late_injection() {
    const Fixture values = fixture();
    ScriptedBackend backend;
    WindowsBootstrapFilesystemPort port(backend);
    check(port.Begin(values.plan, values.journal, roots()).ok() &&
              port.Apply(intent(BootstrapIntentKind::PersistJournal, {},
                                "prepared")).ok() &&
              port.Apply(intent(BootstrapIntentKind::SnapshotBackup,
                                "HarkenSearch.exe")).ok() &&
              port.Apply(intent(BootstrapIntentKind::ReplaceInventoryItem,
                                "HarkenSearch.exe")).ok() &&
              port.Apply(intent(BootstrapIntentKind::ReplaceInventoryItem,
                                "resources/core.pak")).ok() &&
              checkpointHealthyChildForCommit(port) &&
              port.Apply(intent(BootstrapIntentKind::CommitJournal)).ok() &&
              port.Apply(intent(BootstrapIntentKind::RemoveBackup)).ok() &&
              port.CleanupCommittedInvocation().ok(),
          "absent runtime-pair authority reaches cleanup-complete without inventing a file");

    backend.setByHandle(ScriptedBackend::kTransaction, "runtime-pair",
                        WindowsFilesystemEntryKind::Directory, identity(161U));
    backend.setByHandle(ScriptedBackend::kTransaction,
                        "runtime-pair/HarkenSearchBootstrap.exe",
                        WindowsFilesystemEntryKind::RegularFile, identity(162U));
    backend.setContentByHandle(ScriptedBackend::kTransaction,
                               "runtime-pair/HarkenSearchBootstrap.exe",
                               fingerprint(0xeeU, 4096U));
    const auto injected = port.CleanupCommittedInvocation();
    check(injected.status == WindowsBootstrapFilesystemStatus::IdentityMismatch &&
              port.blocked(),
          "late same-name runtime-pair injection is rejected after absent authority was persisted");
}

void test_pre_mutation_cleanup_access_denied_is_retryable() {
    const Fixture values = fixture();
    ScriptedBackend backend;
    backend.setByHandle(ScriptedBackend::kTransaction, "runtime-pair",
                        WindowsFilesystemEntryKind::Directory, identity(61U));
    backend.setByHandle(ScriptedBackend::kTransaction,
                        "runtime-pair/HarkenSearchBootstrap.exe",
                        WindowsFilesystemEntryKind::RegularFile, identity(62U));
    backend.setContentByHandle(ScriptedBackend::kTransaction,
                               "runtime-pair/HarkenSearchBootstrap.exe",
                               fingerprint(0xabU, 4096U));
    WindowsBootstrapFilesystemPort port(backend);
    check(port.Begin(values.plan, values.journal, roots()).ok() &&
              port.Apply(intent(BootstrapIntentKind::PersistJournal, {},
                                "prepared")).ok() &&
              port.Apply(intent(BootstrapIntentKind::SnapshotBackup,
                                "HarkenSearch.exe")).ok() &&
              port.Apply(intent(BootstrapIntentKind::ReplaceInventoryItem,
                                "HarkenSearch.exe")).ok() &&
              port.Apply(intent(BootstrapIntentKind::ReplaceInventoryItem,
                                "resources/core.pak")).ok() &&
              checkpointHealthyChildForCommit(port) &&
              port.Apply(intent(BootstrapIntentKind::CommitJournal)).ok() &&
              port.Apply(intent(BootstrapIntentKind::RemoveBackup)).ok(),
          "pre-mutation cleanup denial fixture reaches committed terminal state");

    backend.failInvocationCleanup = true;
    const auto denied = port.CleanupCommittedInvocation();
    check(denied.status == WindowsBootstrapFilesystemStatus::RetryableAccessDenied &&
              denied.backendStatus == WindowsFilesystemBackendStatus::AccessDenied &&
              !port.blocked(),
          "pre-mutation cleanup access denial remains retryable and does not block the journal");
    backend.failInvocationCleanup = false;
    const auto retried = port.CleanupCommittedInvocation();
    check(retried.ok() && !port.blocked(),
          "the same exact cleanup authority can finish after the sharing denial clears");
}

void test_invocation_cleanup_retries_after_completion_persist_failure() {
    const Fixture values = fixture();
    ScriptedBackend crashed;
    crashed.setByHandle(ScriptedBackend::kTransaction, "runtime-pair",
                        WindowsFilesystemEntryKind::Directory, identity(61U));
    crashed.setByHandle(ScriptedBackend::kTransaction,
                        "runtime-pair/HarkenSearchBootstrap.exe",
                        WindowsFilesystemEntryKind::RegularFile, identity(62U));
    crashed.setContentByHandle(ScriptedBackend::kTransaction,
                               "runtime-pair/HarkenSearchBootstrap.exe",
                               fingerprint(0xabU, 4096U));

    {
        WindowsBootstrapFilesystemPort live(crashed);
        check(live.Begin(values.plan, values.journal, roots()).ok() &&
                  live.Apply(intent(BootstrapIntentKind::PersistJournal, {},
                                    "prepared")).ok() &&
                  live.Apply(intent(BootstrapIntentKind::SnapshotBackup,
                                    "HarkenSearch.exe")).ok() &&
                  live.Apply(intent(BootstrapIntentKind::ReplaceInventoryItem,
                                    "HarkenSearch.exe")).ok() &&
                  live.Apply(intent(BootstrapIntentKind::ReplaceInventoryItem,
                                    "resources/core.pak")).ok() &&
                  checkpointHealthyChildForCommit(live) &&
                  live.Apply(intent(BootstrapIntentKind::CommitJournal)).ok() &&
                  live.Apply(intent(BootstrapIntentKind::RemoveBackup)).ok(),
              "cleanup retry fixture reaches durable commit and removes backup");

        // The first cleanup invocation removes the runtime pair, then loses
        // the cleanup-complete journal publication.  The live port is
        // intentionally blocked by the ambiguous durable write; recovery must
        // happen through a fresh port instead of retrying inside that object.
        crashed.failPersistCall = crashed.persistCalls + 2;
        const auto first = live.CleanupCommittedInvocation();
        check(first.status == WindowsBootstrapFilesystemStatus::CleanupPending &&
                  crashed.lookup(ScriptedBackend::kTransaction,
                                 "runtime-pair").kind ==
                  WindowsFilesystemEntryKind::Missing,
              "cleanup-complete publication failure leaves the authorized runtime pair removed");
    }

    ScriptedBackend recoveredBackend;
    recoveredBackend.entries = crashed.entries;
    recoveredBackend.contents = crashed.contents;
    recoveredBackend.metadatas = crashed.metadatas;
    recoveredBackend.backupDirectoryAlreadyExists =
        crashed.backupDirectoryAlreadyExists;
    recoveredBackend.backupDirectoryEmpty = crashed.backupDirectoryEmpty;
    bindRecoveryJournalCandidates(recoveredBackend, crashed);

    WindowsBootstrapFilesystemPort recovered(recoveredBackend);
    const auto recovery = recovered.BeginRecovery(values.plan, roots());
    check(recovery.ok() &&
              recovery.disposition ==
                  WindowsBootstrapRecoveryDisposition::CleanupCommitted,
          "fresh recovery reopens the committed cleanup-started journal");
    const auto retry = recovered.CleanupCommittedInvocation();
    check(retry.ok(),
          "fresh cleanup retry accepts the already-removed authorized runtime pair");
    check(recoveredBackend.lookup(ScriptedBackend::kStaging, "HarkenSearch.exe").kind ==
              WindowsFilesystemEntryKind::Missing &&
              recoveredBackend.lookup(ScriptedBackend::kTransaction,
                             "runtime-pair").kind ==
                  WindowsFilesystemEntryKind::Missing,
          "cleanup retry leaves no known invocation residue");
}

void test_invocation_cleanup_rejects_missing_nested_parent() {
    ScriptedBackend backend;
    const WindowsFileIdentity nestedIdentity = identity(71U);
    const WindowsFileContentFingerprint nestedContent = fingerprint(0x71U, 17U);
    backend.setByHandle(ScriptedBackend::kStaging, "nested",
                        WindowsFilesystemEntryKind::Directory, identity(70U));
    backend.setByHandle(ScriptedBackend::kStaging, "nested/HarkenSearch.exe",
                        WindowsFilesystemEntryKind::RegularFile, nestedIdentity);
    backend.setContentByHandle(ScriptedBackend::kStaging, "nested/HarkenSearch.exe",
                               nestedContent);
    backend.setMetadataByHandle(ScriptedBackend::kStaging, "nested/HarkenSearch.exe",
                                metadata(0x71U));

    // The leaf still exists, but its containing directory disappeared.  A
    // started cleanup may accept a missing leaf only after the exact parent
    // has been proven to exist; it must not treat a missing parent as an
    // idempotent delete.
    backend.setByHandle(ScriptedBackend::kStaging, "nested",
                        WindowsFilesystemEntryKind::Missing, {});
    WindowsBootstrapInvocationCleanup cleanup;
    cleanup.allowAlreadyRemoved = true;
    cleanup.stagingFiles.push_back(
        {"nested/HarkenSearch.exe", nestedIdentity, nestedContent});
    const WindowsFilesystemBackendResult cleaned =
        backend.CleanupInvocationResidueNoFollow(
            ScriptedBackend::kStaging, ScriptedBackend::kTransaction, cleanup);
    check(cleaned.status == WindowsFilesystemBackendStatus::IdentityMismatch,
          "invocation cleanup rejects a missing nested parent instead of accepting a false leaf absence");
    check(backend.lookup(ScriptedBackend::kStaging, "nested/HarkenSearch.exe").kind ==
              WindowsFilesystemEntryKind::RegularFile,
          "missing nested parent leaves the exact cleanup leaf untouched");
}

void test_invocation_cleanup_accepts_missing_nested_leaf_with_parent() {
    ScriptedBackend backend;
    const WindowsFileIdentity nestedIdentity = identity(72U);
    const WindowsFileContentFingerprint nestedContent = fingerprint(0x72U, 18U);
    backend.setByHandle(ScriptedBackend::kStaging, "nested",
                        WindowsFilesystemEntryKind::Directory, identity(70U));

    // The containing directory is still present and no-follow validated, but
    // the expected nested leaf is gone after a prior cleanup cut.  This is
    // idempotent; a missing containing directory remains a hard failure.
    WindowsBootstrapInvocationCleanup cleanup;
    cleanup.allowAlreadyRemoved = true;
    cleanup.stagingFiles.push_back(
        {"nested/HarkenSearch.exe", nestedIdentity, nestedContent});
    const WindowsFilesystemBackendResult cleaned =
        backend.CleanupInvocationResidueNoFollow(
            ScriptedBackend::kStaging, ScriptedBackend::kTransaction, cleanup);
    check(cleaned.ok(),
          "invocation cleanup accepts an absent nested leaf after its parent remains validated");
}

void test_invocation_cleanup_rechecks_supported_metadata() {
    ScriptedBackend backend;
    const auto observed = backend.lookup(ScriptedBackend::kStaging, "HarkenSearch.exe");
    const auto content = backend.lookupContent(ScriptedBackend::kStaging,
                                               "HarkenSearch.exe");
    check(observed.kind == WindowsFilesystemEntryKind::RegularFile &&
              observed.identity.valid && content.valid,
          "metadata-drift cleanup fixture observes its staging file");
    if (observed.kind != WindowsFilesystemEntryKind::RegularFile ||
        !observed.identity.valid || !content.valid) {
        return;
    }

    // An unsupported metadata change (hard-link/ADS/EA in the native backend)
    // is represented by an invalid metadata snapshot in the scripted parity
    // backend.  Cleanup must recheck it before deleting the file.
    backend.setMetadataByHandle(ScriptedBackend::kStaging, "HarkenSearch.exe", {});
    WindowsBootstrapInvocationCleanup cleanup;
    cleanup.stagingFiles.push_back({"HarkenSearch.exe", observed.identity, content});
    const WindowsFilesystemBackendResult cleaned =
        backend.CleanupInvocationResidueNoFollow(
            ScriptedBackend::kStaging, ScriptedBackend::kTransaction, cleanup);
    check(cleaned.status == WindowsFilesystemBackendStatus::UnsupportedMetadata,
          "invocation cleanup rejects metadata drift before deleting a staging file");
    check(backend.lookup(ScriptedBackend::kStaging, "HarkenSearch.exe").kind ==
              WindowsFilesystemEntryKind::RegularFile,
          "metadata drift leaves the staging file untouched");
}

void test_runtime_pair_authority_is_rechecked_before_journal_persist() {
    const Fixture values = fixture();
    ScriptedBackend backend;
    WindowsBootstrapFilesystemPort port(backend);
    check(port.Begin(values.plan, values.journal, roots()).ok() &&
              port.Apply(intent(BootstrapIntentKind::PersistJournal, {},
                                "prepared")).ok(),
          "runtime authority persist fixture reaches its first durable journal");
    if (backend.persistCalls == 0) return;

    backend.setByHandle(ScriptedBackend::kTransaction, "runtime-pair",
                        WindowsFilesystemEntryKind::Directory, identity(161U));
    backend.setByHandle(ScriptedBackend::kTransaction,
                        "runtime-pair/HarkenSearchBootstrap.exe",
                        WindowsFilesystemEntryKind::RegularFile, identity(162U));
    backend.setContentByHandle(ScriptedBackend::kTransaction,
                               "runtime-pair/HarkenSearchBootstrap.exe",
                               fingerprint(0xeeU, 4096U));
    const int persistsBefore = backend.persistCalls;
    const auto rejected = port.Apply(
        intent(BootstrapIntentKind::SnapshotBackup, "HarkenSearch.exe"));
    check(rejected.status == WindowsBootstrapFilesystemStatus::IdentityMismatch &&
              port.blocked(),
          "runtime-pair injection is rejected before the next journal generation");
    check(backend.persistCalls == persistsBefore,
          "runtime-pair authority failure emits no journal write");
    check(backend.lookup(ScriptedBackend::kInstall, "HarkenSearch.exe").kind ==
              WindowsFilesystemEntryKind::RegularFile,
          "runtime-pair authority failure emits no install mutation");
}

void test_terminal_retirement_requires_invocation_cleanup() {
    const Fixture values = fixture();
    ScriptedBackend backend;
    WindowsBootstrapFilesystemPort port(backend);
    check(port.Begin(values.plan, values.journal, roots()).ok() &&
              port.Apply(intent(BootstrapIntentKind::PersistJournal, {},
                                "prepared")).ok() &&
              port.Apply(intent(BootstrapIntentKind::SnapshotBackup,
                                "HarkenSearch.exe")).ok() &&
              port.Apply(intent(BootstrapIntentKind::ReplaceInventoryItem,
                                "HarkenSearch.exe")).ok() &&
              port.Apply(intent(BootstrapIntentKind::ReplaceInventoryItem,
                                "resources/core.pak")).ok() &&
              checkpointHealthyChildForCommit(port) &&
              port.Apply(intent(BootstrapIntentKind::CommitJournal)).ok() &&
              port.Apply(intent(BootstrapIntentKind::RemoveBackup)).ok(),
          "retirement guard fixture reaches a committed backup-clean state");

    const std::size_t retireCallsBefore = backend.retireCalls;
    const auto retired = port.RetireTerminalJournal();
    check(retired.status == WindowsBootstrapFilesystemStatus::InvalidIntentOrder,
          "committed retirement is blocked until invocation residue cleanup is durable");
    check(backend.retireCalls == retireCallsBefore,
          "retirement guard emits no journal deletion before invocation cleanup");
}

void test_terminal_cleanup_binds_inherited_runtime_pair_parent() {
    const Fixture values = fixture();
    ScriptedBackend liveBackend;
    liveBackend.setByHandle(ScriptedBackend::kTransaction, "runtime-pair",
                            WindowsFilesystemEntryKind::Directory,
                            identity(0x61U));
    liveBackend.setByHandle(
        ScriptedBackend::kTransaction, "runtime-pair/HarkenSearchBootstrap.exe",
        WindowsFilesystemEntryKind::RegularFile, identity(0x62U));
    liveBackend.setContentByHandle(
        ScriptedBackend::kTransaction, "runtime-pair/HarkenSearchBootstrap.exe",
        fingerprint(0xabU, 4096U));

    {
        WindowsBootstrapFilesystemPort live(liveBackend);
        check(live.Begin(values.plan, values.journal, roots()).ok() &&
                  live.Apply(intent(BootstrapIntentKind::PersistJournal, {},
                                    "prepared")).ok() &&
                  live.Apply(intent(BootstrapIntentKind::SnapshotBackup,
                                    "HarkenSearch.exe")).ok() &&
                  live.Apply(intent(BootstrapIntentKind::ReplaceInventoryItem,
                                    "HarkenSearch.exe")).ok() &&
                  live.Apply(intent(BootstrapIntentKind::ReplaceInventoryItem,
                                    "resources/core.pak")).ok() &&
                  checkpointHealthyChildForCommit(live) &&
                  live.Apply(intent(BootstrapIntentKind::CommitJournal)).ok() &&
                  live.Apply(intent(BootstrapIntentKind::RemoveBackup)).ok(),
              "terminal admission fixture reaches a committed journal with runtime-pair residue");
    }

    ScriptedBackend freshBackend;
    freshBackend.entries = liveBackend.entries;
    freshBackend.contents = liveBackend.contents;
    freshBackend.metadatas = liveBackend.metadatas;
    bindRecoveryJournalCandidates(freshBackend, liveBackend);

    BootstrapProcessIdentity inheritedParent = processIdentity(700U, 0x62U);
    inheritedParent.imagePath =
        roots().transactionRoot + "/runtime-pair/HarkenSearchBootstrap.exe";
    inheritedParent.imageVolumeSerial = 77U;
    inheritedParent.imageFileId.fill(0x62U);

    WindowsBootstrapFilesystemPort accepted(freshBackend);
    const auto admitted = accepted.BeginTerminalCleanup(
        values.plan, values.journal, roots(), inheritedParent);
    check(admitted.ok() &&
              admitted.disposition ==
                  WindowsBootstrapRecoveryDisposition::CleanupCommitted,
          "terminal admission accepts only the exact runtime-pair Bootstrap parent");

    ScriptedBackend forgedBackend = freshBackend;
    const int forgedPersistCallsBefore = forgedBackend.persistCalls;
    const int forgedPromotionCallsBefore = forgedBackend.promotionCalls;
    BootstrapProcessIdentity forgedParent = inheritedParent;
    forgedParent.imageFileId.fill(0x63U);
    WindowsBootstrapFilesystemPort forged(forgedBackend);
    const auto rejected = forged.BeginTerminalCleanup(
        values.plan, values.journal, roots(), forgedParent);
    check(rejected.filesystem.status ==
              WindowsBootstrapFilesystemStatus::IdentityMismatch &&
              forgedBackend.persistCalls == forgedPersistCallsBefore &&
              forgedBackend.promotionCalls == forgedPromotionCallsBefore,
          "a same-path but different runtime-pair parent is rejected before journal mutation");
}

void test_terminal_cleanup_retries_transient_journal_read_denial() {
    const Fixture values = fixture();
    ScriptedBackend liveBackend;
    liveBackend.setByHandle(ScriptedBackend::kTransaction, "runtime-pair",
                            WindowsFilesystemEntryKind::Directory,
                            identity(0x61U));
    liveBackend.setByHandle(
        ScriptedBackend::kTransaction, "runtime-pair/HarkenSearchBootstrap.exe",
        WindowsFilesystemEntryKind::RegularFile, identity(0x62U));
    liveBackend.setContentByHandle(
        ScriptedBackend::kTransaction, "runtime-pair/HarkenSearchBootstrap.exe",
        fingerprint(0xabU, 4096U));

    {
        WindowsBootstrapFilesystemPort live(liveBackend);
        check(live.Begin(values.plan, values.journal, roots()).ok() &&
                  live.Apply(intent(BootstrapIntentKind::PersistJournal, {},
                                    "prepared")).ok() &&
                  live.Apply(intent(BootstrapIntentKind::SnapshotBackup,
                                    "HarkenSearch.exe")).ok() &&
                  live.Apply(intent(BootstrapIntentKind::ReplaceInventoryItem,
                                    "HarkenSearch.exe")).ok() &&
                  live.Apply(intent(BootstrapIntentKind::ReplaceInventoryItem,
                                    "resources/core.pak")).ok() &&
                  checkpointHealthyChildForCommit(live) &&
                  live.Apply(intent(BootstrapIntentKind::CommitJournal)).ok() &&
                  live.Apply(intent(BootstrapIntentKind::RemoveBackup)).ok(),
              "terminal journal-read retry fixture reaches committed cleanup state");
    }

    ScriptedBackend freshBackend;
    freshBackend.entries = liveBackend.entries;
    freshBackend.contents = liveBackend.contents;
    freshBackend.metadatas = liveBackend.metadatas;
    bindRecoveryJournalCandidates(freshBackend, liveBackend);
    freshBackend.readJournalAccessDeniedFailuresRemaining = 1;

    BootstrapProcessIdentity inheritedParent = processIdentity(700U, 0x62U);
    inheritedParent.imagePath =
        roots().transactionRoot + "/runtime-pair/HarkenSearchBootstrap.exe";
    inheritedParent.imageVolumeSerial = 77U;
    inheritedParent.imageFileId.fill(0x62U);

    WindowsBootstrapFilesystemPort recovered(freshBackend);
    const auto admitted = recovered.BeginTerminalCleanup(
        values.plan, values.journal, roots(), inheritedParent);
    check(admitted.ok() &&
              admitted.disposition ==
                  WindowsBootstrapRecoveryDisposition::CleanupCommitted &&
              freshBackend.readJournalAccessDeniedFailuresRemaining == 0 &&
              freshBackend.readJournalCalls >= 2,
          "terminal cleanup retries a transient journal read denial before admitting cleanup");
}

void test_terminal_cleanup_rehydrates_when_handoff_journal_is_stale() {
    const Fixture values = fixture();
    ScriptedBackend liveBackend;
    liveBackend.setByHandle(ScriptedBackend::kTransaction, "runtime-pair",
                            WindowsFilesystemEntryKind::Directory,
                            identity(0x61U));
    liveBackend.setByHandle(
        ScriptedBackend::kTransaction, "runtime-pair/HarkenSearchBootstrap.exe",
        WindowsFilesystemEntryKind::RegularFile, identity(0x62U));
    liveBackend.setContentByHandle(
        ScriptedBackend::kTransaction, "runtime-pair/HarkenSearchBootstrap.exe",
        fingerprint(0xabU, 4096U));

    {
        WindowsBootstrapFilesystemPort live(liveBackend);
        check(live.Begin(values.plan, values.journal, roots()).ok() &&
                  live.Apply(intent(BootstrapIntentKind::PersistJournal, {},
                                    "prepared")).ok() &&
                  live.Apply(intent(BootstrapIntentKind::SnapshotBackup,
                                    "HarkenSearch.exe")).ok() &&
                  live.Apply(intent(BootstrapIntentKind::ReplaceInventoryItem,
                                    "HarkenSearch.exe")).ok() &&
                  live.Apply(intent(BootstrapIntentKind::ReplaceInventoryItem,
                                    "resources/core.pak")).ok() &&
                  checkpointHealthyChildForCommit(live) &&
                  live.Apply(intent(BootstrapIntentKind::CommitJournal)).ok() &&
                  live.Apply(intent(BootstrapIntentKind::RemoveBackup)).ok(),
              "stale handoff fixture reaches a committed journal with runtime-pair residue");
    }

    ScriptedBackend freshBackend;
    freshBackend.entries = liveBackend.entries;
    freshBackend.contents = liveBackend.contents;
    freshBackend.metadatas = liveBackend.metadatas;
    bindRecoveryJournalCandidates(freshBackend, liveBackend);

    const auto staleBuild = BuildReplacementJournal(
        values.plan, {{"HarkenSearch.exe"}}, values.journal.transactionId);
    check(staleBuild.ok() && staleBuild.journal.has_value() &&
              staleBuild.journal->isValidatedFor(values.plan),
          "stale handoff journal fixture remains value-valid but omits a durable preserve item");
    if (!staleBuild.journal.has_value()) return;

    BootstrapProcessIdentity inheritedParent = processIdentity(700U, 0x62U);
    inheritedParent.imagePath =
        roots().transactionRoot + "/runtime-pair/HarkenSearchBootstrap.exe";
    inheritedParent.imageVolumeSerial = 77U;
    inheritedParent.imageFileId.fill(0x62U);

    WindowsBootstrapFilesystemPort recovered(freshBackend);
    const auto admitted = recovered.BeginTerminalCleanup(
        values.plan, *staleBuild.journal, roots(), inheritedParent);
    check(admitted.ok() &&
              admitted.disposition ==
                  WindowsBootstrapRecoveryDisposition::CleanupCommitted &&
              admitted.journal.has_value() &&
              admitted.journal->items.size() == values.journal.items.size(),
          "terminal cleanup rehydrates the durable journal instead of rejecting a stale handoff snapshot");
}

void test_rollback_terminal_retirement_requires_invocation_cleanup() {
    const Fixture values = fixture();
    ScriptedBackend backend;
    WindowsBootstrapFilesystemPort port(backend);
    check(port.Begin(values.plan, values.journal, roots()).ok(),
          "rollback retirement guard fixture opens");
    applyInstalledFixture(port);
    const BootstrapProcessIdentity previous = processIdentity(902U, 0x7aU);
    check(port.Apply(intent(BootstrapIntentKind::RestoreBackup,
                            "HarkenSearch.exe")).ok() &&
              port.Apply(intent(BootstrapIntentKind::RemoveNewItem,
                                "resources/core.pak")).ok() &&
              port.RecordPreviousApplicationRelaunchStarted().ok() &&
              port.RecordPreviousApplicationIdentity(previous).ok() &&
              port.RecordPreviousApplicationRelaunchComplete(previous).ok() &&
              port.Apply(intent(BootstrapIntentKind::RemoveBackup)).ok(),
          "rollback retirement guard reaches restored, relaunched, backup-clean terminal state");

    backend.durableJournalCandidates = recoveryCandidates(backend);
    const std::size_t retireCallsBefore = backend.retireCalls;
    const auto premature = port.RetireTerminalJournal();
    check(premature.status ==
              WindowsBootstrapFilesystemStatus::InvalidIntentOrder &&
              backend.retireCalls == retireCallsBefore,
          "rolled-back retirement is blocked until invocation residue cleanup is durable");
    if (premature.ok()) return;

    check(port.CleanupCommittedInvocation().ok(),
          "rolled-back terminal state can clean its exact invocation residue");
    backend.durableJournalCandidates = recoveryCandidates(backend);
    check(port.RetireTerminalJournal().ok(),
          "rolled-back terminal journal retires only after invocation cleanup");
}

void test_fresh_retirement_rechecks_recreated_invocation_residue() {
    const Fixture values = fixture();
    ScriptedBackend completedBackend;
    {
        WindowsBootstrapFilesystemPort completed(completedBackend);
        check(completed.Begin(values.plan, values.journal, roots()).ok() &&
                  completed.Apply(intent(BootstrapIntentKind::PersistJournal,
                                         {}, "prepared")).ok() &&
                  completed.Apply(intent(BootstrapIntentKind::SnapshotBackup,
                                         "HarkenSearch.exe")).ok() &&
                  completed.Apply(intent(BootstrapIntentKind::ReplaceInventoryItem,
                                         "HarkenSearch.exe")).ok() &&
                  completed.Apply(intent(BootstrapIntentKind::ReplaceInventoryItem,
                                         "resources/core.pak")).ok() &&
                  checkpointHealthyChildForCommit(completed) &&
                  completed.Apply(intent(BootstrapIntentKind::CommitJournal)).ok() &&
                  completed.Apply(intent(BootstrapIntentKind::RemoveBackup)).ok() &&
                  completed.CleanupCommittedInvocation().ok(),
              "fresh residue retirement fixture reaches cleanup-complete terminal state");
    }

    ScriptedBackend freshBackend;
    freshBackend.entries = completedBackend.entries;
    freshBackend.contents = completedBackend.contents;
    freshBackend.metadatas = completedBackend.metadatas;
    bindRecoveryJournalCandidates(freshBackend, completedBackend);
    freshBackend.setByHandle(ScriptedBackend::kStaging, "HarkenSearch.exe",
                             WindowsFilesystemEntryKind::RegularFile,
                             identity(199U));
    freshBackend.setContentByHandle(ScriptedBackend::kStaging, "HarkenSearch.exe",
                                    fingerprint(0xddU, 1024U));
    freshBackend.setMetadataByHandle(ScriptedBackend::kStaging, "HarkenSearch.exe",
                                     metadata(0xddU));

    WindowsBootstrapFilesystemPort recovered(freshBackend);
    const auto recovery = recovered.BeginRecovery(
        values.plan, values.journal, roots());
    check(recovery.ok() &&
              recovery.disposition ==
                  WindowsBootstrapRecoveryDisposition::CleanupCommitted,
          "fresh recovery rehydrates the terminal cleanup authority");
    if (!recovery.ok()) return;

    const std::size_t retireCallsBefore = freshBackend.retireCalls;
    const std::size_t candidatesBefore =
        freshBackend.durableJournalCandidates.size();
    const auto retired = recovered.RetireTerminalJournal();
    check(retired.status == WindowsBootstrapFilesystemStatus::IdentityMismatch &&
              recovered.blocked() &&
              freshBackend.retireCalls == retireCallsBefore &&
              freshBackend.durableJournalCandidates.size() == candidatesBefore,
          "fresh retirement rejects recreated staging residue and preserves the journal authority");
}

void test_terminal_journal_retirement_allows_a_fresh_transaction() {
    const Fixture values = fixture();
    ScriptedBackend backend;
    WindowsBootstrapFilesystemPort completed(backend);
          check(completed.Begin(values.plan, values.journal, roots()).ok() &&
              completed.Apply(intent(BootstrapIntentKind::PersistJournal, {},
                                     "prepared")).ok() &&
              completed.Apply(intent(BootstrapIntentKind::SnapshotBackup,
                                     "HarkenSearch.exe")).ok() &&
              completed.Apply(intent(BootstrapIntentKind::ReplaceInventoryItem,
                                     "HarkenSearch.exe")).ok() &&
              completed.Apply(intent(BootstrapIntentKind::ReplaceInventoryItem,
                                      "resources/core.pak")).ok() &&
                  checkpointHealthyChildForCommit(completed) &&
                  completed.Apply(intent(BootstrapIntentKind::CommitJournal)).ok() &&
                  completed.Apply(intent(BootstrapIntentKind::RemoveBackup)).ok() &&
                  completed.CleanupCommittedInvocation().ok(),
              "terminal retirement fixture reaches committed cleanup");
    backend.durableJournalCandidates = recoveryCandidates(backend);
    const std::string winner = backend.durableJournalCandidates.back().name;
    check(completed.RetireTerminalJournal().ok(),
          "terminal retirement removes the authenticated journal chain");
    check(backend.retiredJournalNames.size() == 2U &&
              backend.retiredJournalNames.back() == winner,
          "terminal retirement durably removes the inactive slot before the winner");

    const auto nextJournal = BuildReplacementJournal(
        values.plan,
        {aris::bootstrap::ExistingInstallItem{"HarkenSearch.exe"},
         aris::bootstrap::ExistingInstallItem{"resources/core.pak"}},
        "txn-next");
    check(nextJournal.ok(),
          "later transaction reclassifies the now-installed inventory");
    // The previous invocation cleanup intentionally removed the old staging
    // payload. A new invocation receives a fresh payload namespace.
    backend.setByHandle(ScriptedBackend::kStaging, "HarkenSearch.exe",
                        WindowsFilesystemEntryKind::RegularFile, identity(31));
    backend.setByHandle(ScriptedBackend::kStaging, "resources/core.pak",
                        WindowsFilesystemEntryKind::RegularFile, identity(32));
    backend.setByHandle(ScriptedBackend::kStaging, "resources",
                        WindowsFilesystemEntryKind::Directory, identity(33));
    backend.setContentByHandle(ScriptedBackend::kStaging, "HarkenSearch.exe",
                               fingerprint(0xdd, 1024));
    backend.setContentByHandle(ScriptedBackend::kStaging, "resources/core.pak",
                               fingerprint(0xee, 2048));
    backend.setMetadataByHandle(ScriptedBackend::kStaging, "HarkenSearch.exe",
                                metadata(0xdd));
    backend.setMetadataByHandle(ScriptedBackend::kStaging, "resources/core.pak",
                                metadata(0xee));
    WindowsBootstrapFilesystemPort next(backend);
    const WindowsBootstrapFilesystemResult nextBegin =
        next.Begin(values.plan, *nextJournal.journal, roots());
    if (!nextBegin.ok()) {
        std::fprintf(stderr,
                     "terminal retirement next Begin status=%d backend=%d native=%u\n",
                     static_cast<int>(nextBegin.status),
                     static_cast<int>(nextBegin.backendStatus),
                     nextBegin.nativeError);
    }
    check(nextBegin.ok(),
          "a later authenticated update can begin after terminal retirement");
}

void test_terminal_retirement_rechecks_backup_namespace_before_delete() {
    const Fixture values = fixture();
    ScriptedBackend backend;
    WindowsBootstrapFilesystemPort completed(backend);
    const bool reachedTerminal =
        completed.Begin(values.plan, values.journal, roots()).ok() &&
        completed.Apply(intent(BootstrapIntentKind::PersistJournal, {},
                               "prepared")).ok() &&
        completed.Apply(intent(BootstrapIntentKind::SnapshotBackup,
                               "HarkenSearch.exe")).ok() &&
        completed.Apply(intent(BootstrapIntentKind::ReplaceInventoryItem,
                               "HarkenSearch.exe")).ok() &&
        completed.Apply(intent(BootstrapIntentKind::ReplaceInventoryItem,
                               "resources/core.pak")).ok() &&
        checkpointHealthyChildForCommit(completed) &&
        completed.Apply(intent(BootstrapIntentKind::CommitJournal)).ok() &&
        completed.Apply(intent(BootstrapIntentKind::RemoveBackup)).ok() &&
        completed.CleanupCommittedInvocation().ok();
    check(reachedTerminal,
          "retirement namespace fixture reaches committed invocation cleanup");
    if (!reachedTerminal) return;

    backend.durableJournalCandidates = recoveryCandidates(backend);
    const std::size_t candidatesBefore = backend.durableJournalCandidates.size();
    backend.backupDirectoryAlreadyExists = true;
    const std::size_t retireCallsBefore = backend.retireCalls;
    const auto rejected = completed.RetireTerminalJournal();
    check(rejected.status == WindowsBootstrapFilesystemStatus::PartialDurableState &&
              completed.blocked() &&
              backend.retireCalls == retireCallsBefore &&
              backend.durableJournalCandidates.size() == candidatesBefore,
          "terminal retirement rechecks that backup namespace is absent before deleting journal candidates");
}

void test_terminal_journal_retirement_recovers_after_inactive_slot_removal() {
    const Fixture values = fixture();
    ScriptedBackend backend;
    {
        WindowsBootstrapFilesystemPort completed(backend);
        check(completed.Begin(values.plan, values.journal, roots()).ok() &&
                  completed.Apply(intent(BootstrapIntentKind::PersistJournal, {},
                                         "prepared")).ok() &&
                  completed.Apply(intent(BootstrapIntentKind::SnapshotBackup,
                                         "HarkenSearch.exe")).ok() &&
                  completed.Apply(intent(BootstrapIntentKind::ReplaceInventoryItem,
                                         "HarkenSearch.exe")).ok() &&
                  completed.Apply(intent(BootstrapIntentKind::ReplaceInventoryItem,
                                          "resources/core.pak")).ok() &&
                  checkpointHealthyChildForCommit(completed) &&
                  completed.Apply(intent(BootstrapIntentKind::CommitJournal)).ok() &&
                  completed.Apply(intent(BootstrapIntentKind::RemoveBackup)).ok() &&
                  completed.CleanupCommittedInvocation().ok(),
              "retirement crash fixture reaches committed cleanup");
        backend.durableJournalCandidates = recoveryCandidates(backend);
        backend.failRetireCall = 2U;
        const WindowsBootstrapFilesystemResult interrupted =
            completed.RetireTerminalJournal();
        check(interrupted.status ==
                  WindowsBootstrapFilesystemStatus::PartialDurableState &&
                  backend.durableJournalCandidates.size() == 1U,
              "retirement failure after inactive-slot removal retains the authenticated terminal winner");
    }

    backend.failRetireCall = 0U;
    WindowsBootstrapFilesystemPort recovered(backend);
    const auto recovery = recovered.BeginRecovery(
        values.plan, values.journal, roots());
    check(recovery.filesystem.ok() &&
              recovery.disposition ==
                  WindowsBootstrapRecoveryDisposition::CleanupCommitted,
          "fresh recovery accepts a lone authenticated terminal winner after retirement interruption");
    const auto recoveredCleanup = recovered.CleanupCommittedInvocation();
    const auto recoveredRetire = recovered.RetireTerminalJournal();
    check(recoveredCleanup.ok() && recoveredRetire.ok() &&
              backend.durableJournalCandidates.empty(),
          "fresh recovery completes interrupted terminal retirement idempotently");
}

void test_restart_rejects_recreated_backup_after_terminal_cleanup() {
    const Fixture values = fixture();
    ScriptedBackend completedBackend;
    {
        WindowsBootstrapFilesystemPort completed(completedBackend);
        check(completed.Begin(values.plan, values.journal, roots()).ok() &&
                  completed.Apply(intent(BootstrapIntentKind::PersistJournal,
                                         {}, "prepared")).ok() &&
                  completed.Apply(intent(BootstrapIntentKind::SnapshotBackup,
                                         "HarkenSearch.exe")).ok() &&
                  completed.Apply(intent(BootstrapIntentKind::ReplaceInventoryItem,
                                         "HarkenSearch.exe")).ok() &&
                  completed.Apply(intent(BootstrapIntentKind::ReplaceInventoryItem,
                                          "resources/core.pak")).ok() &&
                  checkpointHealthyChildForCommit(completed) &&
                  completed.Apply(intent(BootstrapIntentKind::CommitJournal)).ok() &&
                  completed.Apply(intent(BootstrapIntentKind::RemoveBackup)).ok(),
              "restart cleanup fixture reaches a terminal durable generation");
    }
    const auto journalObservation = completedBackend.lookup(
        ScriptedBackend::kTransaction, "aris-bootstrap.journal");
    const auto journalContent = completedBackend.lookupContent(
        ScriptedBackend::kTransaction, "aris-bootstrap.journal");
    check(journalObservation.identity.valid && journalContent.valid &&
              !completedBackend.lastJournalBytes.empty(),
          "restart cleanup fixture retains the terminal journal winner");

    ScriptedBackend freshBackend;
    freshBackend.entries = completedBackend.entries;
    freshBackend.contents = completedBackend.contents;
    freshBackend.metadatas = completedBackend.metadatas;
    freshBackend.backupDirectoryAlreadyExists = true;
    bindRecoveryJournalCandidates(freshBackend, completedBackend);
    WindowsBootstrapFilesystemPort recovered(freshBackend);
    const auto recovery = recovered.BeginRecovery(
        values.plan, values.journal, roots());
    check(recovery.filesystem.status ==
              WindowsBootstrapFilesystemStatus::PartialDurableState &&
              recovery.disposition ==
                  WindowsBootstrapRecoveryDisposition::Quarantine &&
              freshBackend.promotionCalls == 0 && freshBackend.persistCalls == 0,
          "fresh recovery rejects a recreated backup directory before journal or filesystem mutation");
}

void test_terminal_state_rejects_new_journal_phases() {
    const Fixture values = fixture();
    {
        ScriptedBackend backend;
        WindowsBootstrapFilesystemPort port(backend);
        check(port.Begin(values.plan, values.journal, roots()).ok() &&
                  port.Apply(intent(BootstrapIntentKind::PersistJournal, {},
                                    "prepared")).ok() &&
                  port.Apply(intent(BootstrapIntentKind::SnapshotBackup,
                                    "HarkenSearch.exe")).ok() &&
                  port.Apply(intent(BootstrapIntentKind::ReplaceInventoryItem,
                                    "HarkenSearch.exe")).ok() &&
                  port.Apply(intent(BootstrapIntentKind::ReplaceInventoryItem,
                                    "resources/core.pak")).ok() &&
                  checkpointHealthyChildForCommit(port) &&
                  port.Apply(intent(BootstrapIntentKind::CommitJournal)).ok(),
              "terminal commit fixture reaches durable committed state");
        const std::size_t begin = backend.calls.size();
        check(port.Apply(intent(BootstrapIntentKind::PersistJournal, {},
                                "checkpoint")).status ==
                  WindowsBootstrapFilesystemStatus::InvalidIntentOrder,
              "committed transaction rejects a new non-terminal journal phase");
        check(journalEnvelopeCallsAfter(backend, begin).empty(),
              "commit-terminal journal rejection emits no write");
    }
    {
        ScriptedBackend backend;
        WindowsBootstrapFilesystemPort port(backend);
        check(port.Begin(values.plan, values.journal, roots()).ok(),
              "terminal rollback fixture opens");
        applyInstalledFixture(port);
        check(port.Apply(intent(BootstrapIntentKind::RestoreBackup,
                                "HarkenSearch.exe")).ok() &&
                  port.Apply(intent(BootstrapIntentKind::RemoveNewItem,
                                    "resources/core.pak")).ok(),
              "terminal rollback fixture restores all changed items");
        const std::size_t begin = backend.calls.size();
        check(port.Apply(intent(BootstrapIntentKind::PersistJournal, {},
                                "rolling-back")).status ==
                  WindowsBootstrapFilesystemStatus::InvalidIntentOrder,
              "completed rollback rejects a new journal generation");
        check(journalEnvelopeCallsAfter(backend, begin).empty(),
              "rollback-terminal journal rejection emits no write");
    }
}

void test_backup_only_rollback_becomes_terminal_and_cleans_backup() {
    const Fixture values = fixture();
    ScriptedBackend backend;
    WindowsBootstrapFilesystemPort port(backend);
    check(port.Begin(values.plan, values.journal, roots()).ok() &&
              port.Apply(intent(BootstrapIntentKind::PersistJournal, {},
                                "prepared")).ok() &&
              port.Apply(intent(BootstrapIntentKind::SnapshotBackup,
                                "HarkenSearch.exe")).ok(),
          "backup-only rollback fixture reaches a durable snapshot");

    check(port.Apply(intent(BootstrapIntentKind::PersistJournal, {},
                            "rolling-back")).ok(),
          "rollback with no published replacement records terminal truth");
    check(port.Apply(intent(BootstrapIntentKind::RemoveBackup)).ok(),
          "terminal rollback removes its exact backup workspace");
    check(std::count_if(backend.calls.begin(), backend.calls.end(),
                        [](const Call& call) {
                            return call.kind == CallKind::DeleteFile;
                        }) == 1,
          "backup-only rollback deletes exactly its recorded snapshot");
}

void test_native_backend_factory_loads_without_touching_install() {
#if defined(_WIN32)
    const auto backend = CreateNativeWindowsBootstrapFilesystemBackend();
    check(backend != nullptr,
          "native Windows backend factory resolves without filesystem mutation");
#endif
}

#if defined(_WIN32)
constexpr DWORD kCrashCutExitCode = UINT32_C(0xA715B007);
constexpr DWORD kWaitTimeoutExitCode = UINT32_C(0xA715B008);
constexpr DWORD kChildWaitTimeoutMilliseconds = 30'000U;
constexpr DWORD kChildTerminationWaitMilliseconds = 5'000U;

struct ChildProcessWaitOutcome {
    bool completed = false;
    bool timedOut = false;
    bool terminationRequested = false;
    bool terminationObserved = false;
    bool handlesClosed = false;
    DWORD exitCode = STILL_ACTIVE;
    DWORD waitError = ERROR_SUCCESS;
    DWORD terminateError = ERROR_SUCCESS;
    DWORD exitCodeError = ERROR_SUCCESS;
};

ChildProcessWaitOutcome waitForChildProcessBounded(
    PROCESS_INFORMATION& process,
    DWORD timeoutMilliseconds) {
    ChildProcessWaitOutcome outcome;
    const auto closeHandles = [&]() noexcept {
        bool closed = true;
        if (process.hThread != nullptr) {
            if (CloseHandle(process.hThread) == FALSE) closed = false;
            process.hThread = nullptr;
        }
        if (process.hProcess != nullptr) {
            if (CloseHandle(process.hProcess) == FALSE) closed = false;
            process.hProcess = nullptr;
        }
        outcome.handlesClosed = closed;
    };

    if (process.hProcess == nullptr) {
        outcome.waitError = ERROR_INVALID_HANDLE;
        closeHandles();
        return outcome;
    }

    DWORD waitResult = WaitForSingleObject(
        process.hProcess, timeoutMilliseconds);
    if (waitResult == WAIT_TIMEOUT) {
        outcome.timedOut = true;
        if (TerminateProcess(process.hProcess, kWaitTimeoutExitCode) == FALSE) {
            outcome.terminateError = GetLastError();
            const DWORD reapResult = WaitForSingleObject(
                process.hProcess, kChildTerminationWaitMilliseconds);
            if (reapResult == WAIT_OBJECT_0) {
                waitResult = reapResult;
            } else if (reapResult == WAIT_FAILED) {
                outcome.waitError = GetLastError();
            } else {
                outcome.waitError = ERROR_TIMEOUT;
            }
        } else {
            outcome.terminationRequested = true;
            waitResult = WaitForSingleObject(
                process.hProcess, kChildTerminationWaitMilliseconds);
        }
    }

    if (waitResult == WAIT_OBJECT_0) {
        outcome.completed = true;
        outcome.terminationObserved = outcome.timedOut;
        if (GetExitCodeProcess(process.hProcess, &outcome.exitCode) == FALSE) {
            outcome.completed = false;
            outcome.exitCodeError = GetLastError();
        }
    } else if (waitResult == WAIT_FAILED) {
        outcome.waitError = GetLastError();
    } else if (waitResult == WAIT_TIMEOUT) {
        outcome.waitError = ERROR_TIMEOUT;
    } else {
        outcome.waitError = ERROR_INVALID_DATA;
    }

    closeHandles();
    return outcome;
}

int runWaitTimeoutChild();

[[noreturn]] void terminateAtCrashCut() {
    if (TerminateProcess(GetCurrentProcess(), kCrashCutExitCode) == FALSE) {
        ExitProcess(ERROR_PROCESS_ABORTED);
    }
    // TerminateProcess is asynchronous for the current process.  ExitProcess
    // keeps this child from falling through if termination is delayed while
    // also avoiding an unbounded wait in the crash-cut harness itself.
    ExitProcess(kCrashCutExitCode);
}

void test_native_child_wait_timeout_terminates_and_closes() {
    wchar_t executableBuffer[MAX_PATH]{};
    const DWORD executableLength = GetModuleFileNameW(
        nullptr, executableBuffer,
        static_cast<DWORD>(std::size(executableBuffer)));
    check(executableLength != 0U &&
              executableLength < std::size(executableBuffer),
          "bounded-child fixture resolves its own executable");
    if (executableLength == 0U ||
        executableLength >= std::size(executableBuffer)) {
        return;
    }

    const std::string command = "\"" +
        std::filesystem::path(executableBuffer).u8string() +
        "\" --bootstrap-child-wait-timeout";
    std::vector<char> mutableCommand(command.begin(), command.end());
    mutableCommand.push_back('\0');
    STARTUPINFOA startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    const BOOL created = CreateProcessA(
        nullptr, mutableCommand.data(), nullptr, nullptr, FALSE,
        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process);
    check(created != FALSE,
          "bounded-child fixture starts a deliberately long-lived child");
    if (created == FALSE) return;

    const ChildProcessWaitOutcome outcome =
        waitForChildProcessBounded(process, 50U);
    check(outcome.completed && outcome.timedOut &&
              outcome.terminationRequested && outcome.terminationObserved,
          "bounded child wait times out, terminates, and observes the child");
    check(outcome.exitCode == kWaitTimeoutExitCode,
          "bounded child wait reports the explicit timeout cleanup exit code");
    check(outcome.handlesClosed && process.hProcess == nullptr &&
              process.hThread == nullptr,
          "bounded child wait closes and clears both process handles");
}

struct NativeSandbox {
    std::filesystem::path path;
    bool owned = false;

    static std::filesystem::path systemTempRoot() {
        const std::filesystem::path cwd = std::filesystem::current_path();
        const std::filesystem::path root =
            cwd.filename() == "system-temp"
                ? cwd
                : cwd / ".local" / "system-temp";
        std::error_code error;
        std::filesystem::create_directories(root, error);
        return root;
    }

    bool cleanup(const char* context) {
        if (!owned) return true;
        std::error_code cleanupError;
        std::error_code existsError;
        bool stillExists = true;
        std::uintmax_t removed = 0;
        for (std::uint32_t retry = 0; retry < 50U; ++retry) {
            cleanupError.clear();
            const std::uintmax_t attempt =
                std::filesystem::remove_all(path, cleanupError);
            if (attempt != static_cast<std::uintmax_t>(-1)) removed += attempt;
            existsError.clear();
            stillExists = std::filesystem::exists(path, existsError);
            if (!cleanupError && !existsError && !stillExists) break;
            Sleep(10);
        }
        const bool removedCompletely =
            !cleanupError && !existsError && !stillExists;
        if (!removedCompletely) {
            const std::string errorText = cleanupError
                ? cleanupError.message()
                : existsError.message();
            std::fprintf(stderr,
                         "FAIL: native sandbox cleanup context=%s path=%s error=%s removed=%llu exists=%u\n",
                         context, path.u8string().c_str(),
                         errorText.c_str(),
                         static_cast<unsigned long long>(removed),
                         static_cast<unsigned int>(stillExists));
            return false;
        }
        owned = false;
        return true;
    }

    ~NativeSandbox() {
        if (!owned) return;
        std::error_code ignored;
        static_cast<void>(std::filesystem::remove_all(path, ignored));
    }
};

class NativeSandboxCleanupAssertion {
public:
    NativeSandboxCleanupAssertion(NativeSandbox& sandbox,
                                  const char* context) noexcept
        : sandbox_(sandbox), context_(context) {}

    ~NativeSandboxCleanupAssertion() {
        check(sandbox_.cleanup(context_),
              "native sandbox explicit cleanup removes its exact path");
    }

    NativeSandboxCleanupAssertion(const NativeSandboxCleanupAssertion&) = delete;
    NativeSandboxCleanupAssertion& operator=(
        const NativeSandboxCleanupAssertion&) = delete;

private:
    NativeSandbox& sandbox_;
    const char* context_ = nullptr;
};

bool writeNativeFixtureFile(const std::filesystem::path& path,
                            const std::string& bytes);
std::string readNativeFixtureFile(const std::filesystem::path& path);

struct NativeCheckpointTargetSwapProbe {
    std::filesystem::path target;
    std::filesystem::path moved;
    bool called = false;
    bool swapped = false;
    std::error_code error;
};

WindowsFilesystemBackendResult swapNativeTargetAfterPreparedCheckpoint(
    void* context,
    const WindowsFileIdentity&,
    const WindowsFileContentFingerprint&,
    const WindowsFileMetadataSnapshot&) {
    auto* probe = static_cast<NativeCheckpointTargetSwapProbe*>(context);
    if (probe == nullptr) {
        return {WindowsFilesystemBackendStatus::InvalidPath, 87U};
    }
    probe->called = true;
    std::filesystem::rename(probe->target, probe->moved, probe->error);
    if (probe->error) {
        return {WindowsFilesystemBackendStatus::IoFailure,
                static_cast<std::uint32_t>(probe->error.value())};
    }
    if (!writeNativeFixtureFile(probe->target, "attacker-native")) {
        return {WindowsFilesystemBackendStatus::IoFailure, 29U};
    }
    probe->swapped = true;
    return {};
}

void test_terminal_journal_retirement_retries_pre_mutation_access_denied() {
    const Fixture values = fixture();
    ScriptedBackend backend;
    WindowsBootstrapFilesystemPort completed(backend);
    const bool reachedTerminal =
        completed.Begin(values.plan, values.journal, roots()).ok() &&
        completed.Apply(intent(BootstrapIntentKind::PersistJournal, {},
                               "prepared")).ok() &&
        completed.Apply(intent(BootstrapIntentKind::SnapshotBackup,
                               "HarkenSearch.exe")).ok() &&
        completed.Apply(intent(BootstrapIntentKind::ReplaceInventoryItem,
                               "HarkenSearch.exe")).ok() &&
        completed.Apply(intent(BootstrapIntentKind::ReplaceInventoryItem,
                               "resources/core.pak")).ok() &&
        checkpointHealthyChildForCommit(completed) &&
        completed.Apply(intent(BootstrapIntentKind::CommitJournal)).ok() &&
        completed.Apply(intent(BootstrapIntentKind::RemoveBackup)).ok() &&
        completed.CleanupCommittedInvocation().ok();
    check(reachedTerminal,
          "retirement retry fixture reaches committed invocation cleanup");
    if (!reachedTerminal) return;

    backend.durableJournalCandidates = recoveryCandidates(backend);
    backend.retireAccessDeniedFailuresRemaining = 1;
    const auto denied = completed.RetireTerminalJournal();
    check(denied.status ==
              WindowsBootstrapFilesystemStatus::RetryableAccessDenied &&
              !completed.blocked() &&
              backend.durableJournalCandidates.size() == 2U,
          "a pre-mutation journal access denial stays retryable and preserves both candidates");
    check(completed.RetireTerminalJournal().ok() &&
              backend.durableJournalCandidates.empty(),
          "terminal journal retirement succeeds on an exact retry after access denial");
}

// Native mutation fixtures must use the same authenticated capability gate as
// production.  Keep the policy construction local to this test harness so
// low-level adapter tests can still exercise individual primitives without
// smuggling an absolute path into a writable backend.
ValidatedInstallPlan nativeAuthorizationPlan(
    const std::filesystem::path& installPath,
    const std::filesystem::path& stagingPath,
    const std::filesystem::path& transactionPath) {
    SignedInstallPlan plan = rawPlan();
    rebindDeploymentIdentity(plan, installPath.u8string());
    plan.invocation.updaterOwnedBase = stagingPath.parent_path().u8string();
    plan.invocation.stagingRoot = stagingPath.u8string();
    plan.invocation.transactionRoot = transactionPath.u8string();
    plan.invocation.invocationIdentity = "signed-invocation-01";
    const auto validated =
        ValidateSignedInstallPlan(plan, SignatureStatus::Verified);
    if (!validated.ok()) {
        check(false, "native fixture authorization plan validates");
        return {};
    }
    return *validated.plan;
}

std::unique_ptr<WindowsBootstrapFilesystemBackend>
createAuthorizedNativeBackend(
    const std::filesystem::path& installPath,
    const std::filesystem::path& stagingPath,
    const std::filesystem::path& transactionPath,
    const WindowsBootstrapFilesystemNativeTestHooks& hooks = {}) {
    const bool secured =
        secureNativeTrustedDirectory(stagingPath.parent_path()) &&
        secureNativeTrustedDirectory(stagingPath) &&
        secureNativeTrustedDirectory(transactionPath);
    check(secured,
          "native fixture protects its exact updater-owned write roots");
    if (!secured) return {};
    WindowsBootstrapFilesystemTrustedRootPolicy policy;
    policy.updaterOwnedBase = stagingPath.parent_path().u8string();
    policy.invocationIdentity = "signed-invocation-01";
    policy.stagingRoot = stagingPath.u8string();
    policy.transactionRoot = transactionPath.u8string();
    policy.authenticated = true;
    auto backend = CreateNativeWindowsBootstrapFilesystemBackend(policy, hooks);
    if (backend == nullptr) return {};

    const ValidatedInstallPlan plan = nativeAuthorizationPlan(
        installPath, stagingPath, transactionPath);
    const WindowsBootstrapFilesystemRoots roots{
        stagingPath.u8string(), transactionPath.u8string(),
        policy.updaterOwnedBase, policy.invocationIdentity};
    const WindowsFilesystemBackendResult authorized =
        backend->ValidateTrustedWriteRoots(plan, roots);
    if (!authorized.ok()) {
        checkNative(authorized,
                    "native fixture authenticates its trusted write roots");
        return {};
    }
    return backend;
}

void test_native_no_policy_factory_is_read_only() {
    NativeSandbox sandbox;
    NativeSandboxCleanupAssertion cleanup(sandbox, "no-policy-native");
    const auto base = NativeSandbox::systemTempRoot();
    for (std::uint32_t attempt = 0; attempt < 32U && !sandbox.owned;
         ++attempt) {
        sandbox.path = base /
            ("no-policy-native-" + std::to_string(GetCurrentProcessId()) +
             "-" + std::to_string(attempt));
        std::error_code error;
        sandbox.owned = std::filesystem::create_directory(sandbox.path, error);
    }
    check(sandbox.owned,
          "no-policy native fixture obtains an exclusive temporary sandbox");
    if (!sandbox.owned) return;

    const auto installPath = sandbox.path / "install";
    const auto transactionPath = sandbox.path / "transaction";
    std::error_code error;
    check(std::filesystem::create_directory(installPath, error) &&
              std::filesystem::create_directory(transactionPath, error),
          "no-policy native fixture creates diagnostic roots");
    auto backend = CreateNativeWindowsBootstrapFilesystemBackend();
    check(backend != nullptr,
          "no-policy native fixture creates the default backend");
    if (backend == nullptr) return;

    WindowsFilesystemHandle installRoot = 0;
    WindowsFilesystemHandle transactionRoot = 0;
    WindowsFileIdentity installIdentity;
    WindowsFileIdentity transactionIdentity;
    check(backend->OpenRootExact(
              installPath.u8string(), WindowsFilesystemRootRole::Install,
              installRoot, installIdentity).ok() &&
              backend->OpenRootExact(
                  transactionPath.u8string(),
                  WindowsFilesystemRootRole::Transaction, transactionRoot,
                  transactionIdentity).ok(),
          "no-policy backend permits read-only root observation");
    if (installRoot == 0 || transactionRoot == 0) return;

    WindowsFilesystemObservation observation;
    check(backend->ObserveNoFollow(installRoot, "missing", observation).ok() &&
              observation.kind == WindowsFilesystemEntryKind::Missing,
          "no-policy backend permits diagnostic no-follow observation");

    WindowsFileIdentity journalIdentity;
    WindowsFileContentFingerprint journalContent;
    const auto persisted = backend->PersistJournalDurable(
        transactionRoot, {0x01U}, WindowsFilesystemReplaceMode::CreateNew,
        {}, {}, journalIdentity, journalContent);
    check(persisted.status == WindowsFilesystemBackendStatus::AccessDenied &&
              !std::filesystem::exists(
                  transactionPath / "aris-bootstrap.journal.a"),
          "no-policy backend denies journal mutation before any write");

    WindowsFilesystemHandle backupRoot = 0;
    WindowsFileIdentity backupIdentity;
    const auto directory = backend->OpenOrCreateDirectoryNoFollow(
        transactionRoot, "backup", true, backupRoot, backupIdentity);
    check(directory.status == WindowsFilesystemBackendStatus::AccessDenied &&
              !std::filesystem::exists(transactionPath / "backup"),
          "no-policy backend denies transaction-directory mutation");

    backend->CloseFilesystemHandle(transactionRoot);
    backend->CloseFilesystemHandle(installRoot);
}

void test_native_trusted_root_rejects_broad_write_acl() {
    NativeSandbox sandbox;
    NativeSandboxCleanupAssertion cleanup(sandbox, "trusted-root-acl");
    const auto base = NativeSandbox::systemTempRoot();
    for (std::uint32_t attempt = 0; attempt < 32U && !sandbox.owned;
         ++attempt) {
        sandbox.path = base /
            ("trusted-root-acl-" + std::to_string(GetCurrentProcessId()) +
             "-" + std::to_string(attempt));
        std::error_code error;
        sandbox.owned = std::filesystem::create_directory(sandbox.path, error);
    }
    check(sandbox.owned,
          "trusted-root ACL fixture obtains an exclusive temporary sandbox");
    if (!sandbox.owned) return;

    const auto installPath = sandbox.path / "install";
    const auto updaterPath = sandbox.path / "updates";
    const auto stagingPath = updaterPath / "staging";
    const auto transactionPath = updaterPath / "transaction";
    std::error_code error;
    const bool directoriesCreated =
        std::filesystem::create_directory(installPath, error) &&
        std::filesystem::create_directory(updaterPath, error) &&
        std::filesystem::create_directory(stagingPath, error) &&
        std::filesystem::create_directory(transactionPath, error);
    check(directoriesCreated,
          "trusted-root ACL fixture creates its exact roots");
    if (!directoriesCreated) return;

    PSECURITY_DESCRIPTOR descriptor = nullptr;
    PACL existingAcl = nullptr;
    const std::wstring transactionWide = transactionPath.wstring();
    const DWORD queried = GetNamedSecurityInfoW(
        const_cast<wchar_t*>(transactionWide.c_str()), SE_FILE_OBJECT,
        DACL_SECURITY_INFORMATION, nullptr, nullptr, &existingAcl, nullptr,
        &descriptor);
    std::array<std::uint8_t, SECURITY_MAX_SID_SIZE> worldStorage{};
    DWORD worldSize = static_cast<DWORD>(worldStorage.size());
    const BOOL worldCreated = CreateWellKnownSid(
        WinWorldSid, nullptr, worldStorage.data(), &worldSize);
    EXPLICIT_ACCESSW broadWrite{};
    broadWrite.grfAccessPermissions = FILE_GENERIC_WRITE | DELETE;
    broadWrite.grfAccessMode = GRANT_ACCESS;
    broadWrite.grfInheritance = SUB_CONTAINERS_AND_OBJECTS_INHERIT;
    broadWrite.Trustee.TrusteeForm = TRUSTEE_IS_SID;
    broadWrite.Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
    broadWrite.Trustee.ptstrName =
        reinterpret_cast<LPWSTR>(worldStorage.data());
    PACL broadenedAcl = nullptr;
    const DWORD aclBuilt = queried == ERROR_SUCCESS && worldCreated != FALSE
        ? SetEntriesInAclW(1U, &broadWrite, existingAcl, &broadenedAcl)
        : ERROR_INVALID_DATA;
    const DWORD aclApplied = aclBuilt == ERROR_SUCCESS
        ? SetNamedSecurityInfoW(
              const_cast<wchar_t*>(transactionWide.c_str()), SE_FILE_OBJECT,
              DACL_SECURITY_INFORMATION, nullptr, nullptr, broadenedAcl,
              nullptr)
        : aclBuilt;
    if (broadenedAcl != nullptr) LocalFree(broadenedAcl);
    if (descriptor != nullptr) LocalFree(descriptor);
    check(aclApplied == ERROR_SUCCESS,
          "trusted-root ACL fixture grants a deterministic broad writer");
    if (aclApplied != ERROR_SUCCESS) return;

    WindowsBootstrapFilesystemTrustedRootPolicy policy;
    policy.updaterOwnedBase = updaterPath.u8string();
    policy.expectedUpdaterOwnedBase = updaterPath.u8string();
    policy.stagingRoot = stagingPath.u8string();
    policy.transactionRoot = transactionPath.u8string();
    policy.invocationIdentity = "signed-invocation-01";
    policy.authenticatedParentProcess = processIdentity(501U, 0x61U);
    policy.authenticated = true;
    auto probeBackend = CreateNativeWindowsBootstrapFilesystemBackend(policy);
    check(probeBackend != nullptr,
          "trusted-root ACL fixture creates the read-only recovery probe");
    if (probeBackend == nullptr) return;
    WindowsFilesystemHandle probeRoot = 0;
    WindowsFileIdentity probeIdentity;
    const auto probeOpened = probeBackend->OpenRootExact(
        transactionPath.u8string(), WindowsFilesystemRootRole::Transaction,
        probeRoot, probeIdentity);
    std::vector<WindowsBootstrapJournalCandidate> candidates;
    const auto probed = probeOpened.ok()
        ? probeBackend->ReadJournalCandidatesNoFollow(probeRoot, candidates)
        : probeOpened;
    check(probeOpened.ok() &&
              probed.status == WindowsFilesystemBackendStatus::AccessDenied &&
              candidates.empty(),
          "read-only recovery rejects broad transaction ACL before journal bytes become authority");
    if (probeRoot != 0U) probeBackend->CloseFilesystemHandle(probeRoot);
    probeBackend.reset();

    auto backend = CreateNativeWindowsBootstrapFilesystemBackend(policy);
    check(backend != nullptr,
          "trusted-root ACL fixture creates the native backend");
    if (backend == nullptr) return;
    const ValidatedInstallPlan plan = nativeAuthorizationPlan(
        installPath, stagingPath, transactionPath);
    const WindowsBootstrapFilesystemRoots trustedRoots{
        stagingPath.u8string(), transactionPath.u8string(),
        updaterPath.u8string(), policy.invocationIdentity};
    const auto rejected = backend->ValidateTrustedWriteRoots(plan, trustedRoots);
    check(rejected.status == WindowsFilesystemBackendStatus::AccessDenied &&
              rejected.nativeError == ERROR_ACCESS_DENIED,
          "trusted write authority rejects a transaction root writable by Everyone");
    backend.reset();
}

void test_native_legacy_journal_names_fail_closed() {
    NativeSandbox sandbox;
    NativeSandboxCleanupAssertion cleanup(sandbox, "legacy-journal");
    const auto base = NativeSandbox::systemTempRoot();
    for (std::uint32_t attempt = 0; attempt < 32U && !sandbox.owned;
         ++attempt) {
        sandbox.path = base /
            ("legacy-journal-" + std::to_string(GetCurrentProcessId()) +
             "-" + std::to_string(attempt));
        std::error_code error;
        sandbox.owned = std::filesystem::create_directory(sandbox.path, error);
    }
    check(sandbox.owned,
          "legacy journal fixture obtains an exclusive temporary sandbox");
    if (!sandbox.owned) return;
    const auto installPath = sandbox.path / "install";
    const auto updaterPath = sandbox.path / "updater";
    const auto stagingPath = updaterPath / "staging";
    const auto transactionPath = updaterPath / "transaction";
    std::error_code error;
    check(std::filesystem::create_directory(installPath, error) &&
              std::filesystem::create_directory(updaterPath, error) &&
              std::filesystem::create_directory(stagingPath, error) &&
              std::filesystem::create_directory(transactionPath, error) &&
              writeNativeFixtureFile(transactionPath / "aris-bootstrap.journal",
                                     "legacy"),
          "legacy journal fixture creates the historical name");
    auto backend = createAuthorizedNativeBackend(
        installPath, stagingPath, transactionPath);
    check(backend != nullptr, "legacy journal fixture creates native backend");
    if (backend == nullptr) return;
    WindowsFilesystemHandle installRoot = 0;
    WindowsFilesystemHandle transactionRoot = 0;
    WindowsFileIdentity installIdentity;
    WindowsFileIdentity transactionIdentity;
    const auto installOpened = backend->OpenRootExact(
        installPath.u8string(), WindowsFilesystemRootRole::Install,
        installRoot, installIdentity);
    checkNative(installOpened, "legacy journal fixture opens install root");
    if (!installOpened.ok()) return;
    const auto opened = backend->OpenRootExact(
        transactionPath.u8string(), WindowsFilesystemRootRole::Transaction,
        transactionRoot, transactionIdentity);
    checkNative(opened, "legacy journal fixture opens transaction root");
    if (opened.ok()) {
        std::vector<WindowsBootstrapJournalCandidate> candidates;
        const auto observed = backend->ReadJournalCandidatesNoFollow(
            transactionRoot, candidates);
        check(observed.status == WindowsFilesystemBackendStatus::AlreadyExists,
              "legacy journal names are rejected before recovery enumeration");
    }
    if (transactionRoot != 0) backend->CloseFilesystemHandle(transactionRoot);
    if (installRoot != 0) backend->CloseFilesystemHandle(installRoot);
}

int runWaitTimeoutChild() {
    Sleep(kChildWaitTimeoutMilliseconds * 2U);
    return 0;
}

int runJournalTempCrashChild(const char* installText,
                             const char* stagingText,
                             const char* transactionText,
                             const bool beforeDirectoryFlush = false) {
    const std::filesystem::path installPath(installText);
    const std::filesystem::path stagingPath(stagingText);
    const std::filesystem::path transactionPath(transactionText);
    const Fixture values = crashFixture(
        installPath.u8string(), stagingPath.u8string(),
        transactionPath.u8string());
    WindowsBootstrapFilesystemNativeTestHooks hooks;
    hooks.accessDeniedFailurePoint = beforeDirectoryFlush
        ? WindowsBootstrapFilesystemNativeTestFailurePoint::
              JournalAfterTemporaryFileFlushBeforeDirectoryFlush
        : WindowsBootstrapFilesystemNativeTestFailurePoint::
              JournalAfterTemporaryFlush;
    auto backend = createAuthorizedNativeBackend(
        installPath, stagingPath, transactionPath, hooks);
    if (backend == nullptr) return 10;
    WindowsFilesystemHandle installRoot = 0;
    WindowsFilesystemHandle stagingRoot = 0;
    WindowsFilesystemHandle transactionRoot = 0;
    WindowsFileIdentity installIdentity;
    WindowsFileIdentity stagingIdentity;
    WindowsFileIdentity transactionIdentity;
    if (!backend->OpenRootExact(installPath.u8string(),
                                WindowsFilesystemRootRole::Install,
                                installRoot, installIdentity).ok() ||
        !backend->OpenRootExact(stagingPath.u8string(),
                                WindowsFilesystemRootRole::Staging,
                                stagingRoot, stagingIdentity).ok() ||
        !backend->OpenRootExact(transactionPath.u8string(),
                                WindowsFilesystemRootRole::Transaction,
                                transactionRoot, transactionIdentity).ok()) {
        return 11;
    }
    WindowsFilesystemObservation staged;
    if (!backend->ObserveNoFollow(stagingRoot, "HarkenSearch.exe", staged).ok() ||
        !staged.identity.valid || !staged.content.valid) {
        return 12;
    }
    WindowsBootstrapJournalRecord record = journalRecord(values, 1U);
    record.installRootIdentity = installIdentity;
    record.stagingRootIdentity = stagingIdentity;
    record.transactionRootIdentity = transactionIdentity;
    record.items.front().stagingIdentity = staged.identity;
    record.items.front().stagingContent = staged.content;
    record.items.front().installParentIdentity = installIdentity;
    const std::vector<std::uint8_t> bytes =
        EncodeWindowsBootstrapJournal(record);
    WindowsFileIdentity journalIdentity;
    WindowsFileContentFingerprint journalContent;
    const auto persisted = backend->PersistJournalDurable(
        transactionRoot, bytes, WindowsFilesystemReplaceMode::CreateNew, {}, {},
        journalIdentity, journalContent);
    if (persisted.status != WindowsFilesystemBackendStatus::DurabilityFailed) {
        return 13;
    }
    // Bypass every C++ destructor so the parent proves restart recovery from
    // durable namespace state, not from same-process cleanup/session state.
    terminateAtCrashCut();
}

int runTargetAtomicCrashChild(const char* installText,
                              const char* stagingText,
                              const char* transactionText) {
    const std::filesystem::path installPath(installText);
    const std::filesystem::path stagingPath(stagingText);
    const std::filesystem::path transactionPath(transactionText);
    WindowsBootstrapFilesystemNativeTestHooks hooks;
    hooks.accessDeniedFailurePoint =
        WindowsBootstrapFilesystemNativeTestFailurePoint::
            ReplacementAfterAtomicPublishBeforeDirectoryFlush;
    WindowsBootstrapFilesystemTrustedRootPolicy policy;
    policy.updaterOwnedBase = stagingPath.parent_path().u8string();
    policy.invocationIdentity = "signed-invocation-01";
    policy.stagingRoot = stagingPath.u8string();
    policy.transactionRoot = transactionPath.u8string();
    policy.authenticated = true;
    auto backend = createAuthorizedNativeBackend(
        installPath, stagingPath, transactionPath, hooks);
    if (backend == nullptr) return 20;
    const Fixture values = replacementCrashFixture(
        installPath.u8string(), stagingPath.u8string(),
        transactionPath.u8string());
    const WindowsBootstrapFilesystemRoots trusted{
        stagingPath.u8string(), transactionPath.u8string(),
        policy.updaterOwnedBase, policy.invocationIdentity};
    WindowsBootstrapFilesystemPort port(*backend);
    const auto begun = port.Begin(values.plan, values.journal, trusted);
    const auto prepared = begun.ok()
        ? port.Apply(intent(BootstrapIntentKind::PersistJournal, {},
                            "prepared"))
        : WindowsBootstrapFilesystemResult{
              WindowsBootstrapFilesystemStatus::BackendFailure};
    const auto backedUp = prepared.ok()
        ? port.Apply(intent(BootstrapIntentKind::SnapshotBackup,
                            "HarkenSearch.exe"))
        : WindowsBootstrapFilesystemResult{
              WindowsBootstrapFilesystemStatus::BackendFailure};
    const auto interrupted = backedUp.ok()
        ? port.Apply(intent(BootstrapIntentKind::ReplaceInventoryItem,
                            "HarkenSearch.exe"))
        : WindowsBootstrapFilesystemResult{
              WindowsBootstrapFilesystemStatus::BackendFailure};
    if (!begun.ok()) return 21;
    if (!prepared.ok()) return 22;
    if (!backedUp.ok()) return 23;
    if (interrupted.status !=
        WindowsBootstrapFilesystemStatus::PartialDurableState) {
        return 40 + static_cast<int>(interrupted.status);
    }
    if (readNativeFixtureFile(installPath / "HarkenSearch.exe") != "new-native") {
        return 24;
    }
    if (!std::filesystem::exists(
            transactionPath / "backup" / "item-0.bak")) {
        return 25;
    }
    terminateAtCrashCut();
}

int runTargetPreparedCrashChild(const char* installText,
                                const char* stagingText,
                                const char* transactionText) {
    const std::filesystem::path installPath(installText);
    const std::filesystem::path stagingPath(stagingText);
    const std::filesystem::path transactionPath(transactionText);
    WindowsBootstrapFilesystemNativeTestHooks hooks;
    hooks.accessDeniedFailurePoint =
        WindowsBootstrapFilesystemNativeTestFailurePoint::
            ReplacementAfterPreparedCheckpointBeforeAtomicPublish;
    WindowsBootstrapFilesystemTrustedRootPolicy policy;
    policy.updaterOwnedBase = stagingPath.parent_path().u8string();
    policy.invocationIdentity = "signed-invocation-01";
    policy.stagingRoot = stagingPath.u8string();
    policy.transactionRoot = transactionPath.u8string();
    policy.authenticated = true;
    auto backend = createAuthorizedNativeBackend(
        installPath, stagingPath, transactionPath, hooks);
    if (backend == nullptr) return 25;
    const Fixture values = crashFixture(
        installPath.u8string(), stagingPath.u8string(),
        transactionPath.u8string());
    WindowsBootstrapFilesystemPort port(*backend);
    const WindowsBootstrapFilesystemRoots trusted{
        stagingPath.u8string(), transactionPath.u8string(),
        policy.updaterOwnedBase, policy.invocationIdentity};
    const auto begun = port.Begin(values.plan, values.journal, trusted);
    const auto prepared = begun.ok()
        ? port.Apply(intent(BootstrapIntentKind::PersistJournal, {},
                            "prepared"))
        : WindowsBootstrapFilesystemResult{
              WindowsBootstrapFilesystemStatus::BackendFailure};
    const auto interrupted = prepared.ok()
        ? port.Apply(intent(BootstrapIntentKind::ReplaceInventoryItem,
                            "HarkenSearch.exe"))
        : WindowsBootstrapFilesystemResult{
              WindowsBootstrapFilesystemStatus::BackendFailure};
    const bool temporaryExists = std::filesystem::exists(
        installPath / ".aris-bootstrap-attempt-crash-01-0-forward.tmp");
    const bool targetExists = std::filesystem::exists(installPath / "HarkenSearch.exe");
    if (!begun.ok()) return 27;
    if (!prepared.ok()) return 28;
    if (interrupted.status !=
        WindowsBootstrapFilesystemStatus::PartialDurableState) {
        return 40 + static_cast<int>(interrupted.status);
    }
    if (!temporaryExists) return 30;
    if (targetExists) return 31;
    terminateAtCrashCut();
}

int runJournalSlotCrashChild(const char* installText,
                             const char* stagingText,
                             const char* transactionText) {
    const std::filesystem::path installPath(installText);
    const std::filesystem::path stagingPath(stagingText);
    const std::filesystem::path transactionPath(transactionText);
    WindowsBootstrapFilesystemNativeTestHooks hooks;
    hooks.accessDeniedFailurePoint =
        WindowsBootstrapFilesystemNativeTestFailurePoint::
            JournalAfterSlotPublishBeforeDirectoryFlush;
    auto backend = createAuthorizedNativeBackend(
        installPath, stagingPath, transactionPath, hooks);
    if (backend == nullptr) return 30;
    WindowsFilesystemHandle installRoot = 0;
    WindowsFilesystemHandle stagingRoot = 0;
    WindowsFilesystemHandle transactionRoot = 0;
    WindowsFileIdentity installIdentity;
    WindowsFileIdentity stagingIdentity;
    WindowsFileIdentity transactionIdentity;
    if (!backend->OpenRootExact(installText,
                                WindowsFilesystemRootRole::Install,
                                installRoot, installIdentity).ok() ||
        !backend->OpenRootExact(stagingText,
                                WindowsFilesystemRootRole::Staging,
                                stagingRoot, stagingIdentity).ok() ||
        !backend->OpenRootExact(transactionText,
                                WindowsFilesystemRootRole::Transaction,
                                transactionRoot, transactionIdentity).ok()) {
        return 31;
    }
    std::vector<WindowsBootstrapJournalCandidate> candidates;
    if (!backend->ReadJournalCandidatesNoFollow(
             transactionRoot, candidates).ok()) {
        return 32;
    }
    const Fixture values = crashFixture(
        installPath.u8string(), stagingPath.u8string(),
        transactionPath.u8string());
    const WindowsBootstrapJournalRootIdentities rootsBinding{
        installIdentity, stagingIdentity, transactionIdentity, {}};
    const auto selected = SelectDurableWindowsBootstrapJournal(
        candidates, &values.plan, &values.journal, &rootsBinding);
    if (!selected.ok()) return 33;
    const auto active = std::find_if(
        candidates.begin(), candidates.end(),
        [&selected](const auto& candidate) {
            return candidate.name == selected.selectedName;
        });
    if (active == candidates.end() || active->temporary) return 34;
    WindowsBootstrapJournalRecord next = *selected.record;
    next.healthToken = values.plan.healthToken;
    ++next.generation;
    next.journalIdentity = active->identity;
    next.journalContent = active->content;
    next.phase = "replacement-started:HarkenSearch.exe";
    const auto bytes = EncodeWindowsBootstrapJournal(next);
    WindowsFileIdentity outputIdentity;
    WindowsFileContentFingerprint outputContent;
    const auto persisted = backend->PersistJournalDurable(
        transactionRoot, bytes, WindowsFilesystemReplaceMode::ReplaceExisting,
        active->identity, active->content, outputIdentity, outputContent);
    if (persisted.status !=
        WindowsFilesystemBackendStatus::DurabilityFailed) {
        return 35;
    }
    terminateAtCrashCut();
}

int runDeleteCrashChild(const char* operationText,
                        const char* pointText,
                        const char* installText,
                        const char* stagingText,
                        const char* transactionText) {
    std::size_t pointIndex = 0;
    if (std::strcmp(pointText, "1") == 0) {
        pointIndex = 1;
    } else if (std::strcmp(pointText, "2") == 0) {
        pointIndex = 2;
    } else if (std::strcmp(pointText, "0") != 0) {
        return 40;
    }
    const bool removeDirectory = std::strcmp(operationText, "directory") == 0;
    if (!removeDirectory && std::strcmp(operationText, "file") != 0) {
        return 41;
    }
    const std::array<WindowsBootstrapFilesystemNativeTestFailurePoint, 3U>
        cutPoints{
            WindowsBootstrapFilesystemNativeTestFailurePoint::
                DeleteAfterMarkBeforeRebind,
            WindowsBootstrapFilesystemNativeTestFailurePoint::
                DeleteAfterRebindBeforeDirectoryFlush,
            WindowsBootstrapFilesystemNativeTestFailurePoint::
                DeleteAfterDirectoryFlushBeforeFinalRebind};
    const std::array<WindowsBootstrapFilesystemNativeTestFailurePoint, 3U>
        directoryCutPoints{
            WindowsBootstrapFilesystemNativeTestFailurePoint::
                RemoveDirectoryAfterMarkBeforeRebind,
            WindowsBootstrapFilesystemNativeTestFailurePoint::
                RemoveDirectoryAfterRebindBeforeDirectoryFlush,
            WindowsBootstrapFilesystemNativeTestFailurePoint::
                RemoveDirectoryAfterDirectoryFlushBeforeFinalRebind};
    const std::filesystem::path installPath(installText);
    const std::filesystem::path stagingPath(stagingText);
    const std::filesystem::path transactionPath(transactionText);
    const Fixture values = crashFixture(
        installPath.u8string(), stagingPath.u8string(),
        transactionPath.u8string());
    WindowsBootstrapFilesystemNativeTestHooks hooks;
    hooks.accessDeniedFailurePoint =
        removeDirectory ? directoryCutPoints[pointIndex]
                        : cutPoints[pointIndex];
    auto backend = createAuthorizedNativeBackend(
        installPath, stagingPath, transactionPath, hooks);
    if (backend == nullptr) return 42;
    const WindowsBootstrapFilesystemTrustedRootPolicy policy{
        stagingPath.parent_path().u8string(), "signed-invocation-01",
        stagingPath.u8string(), transactionPath.u8string(), true};
    const WindowsBootstrapFilesystemRoots trusted{
        stagingPath.u8string(), transactionPath.u8string(),
        policy.updaterOwnedBase, policy.invocationIdentity};
    WindowsBootstrapFilesystemPort port(*backend);
    const auto begun = port.Begin(values.plan, values.journal, trusted);
    const auto prepared = begun.ok()
        ? port.Apply(intent(BootstrapIntentKind::PersistJournal, {},
                            "prepared"))
        : WindowsBootstrapFilesystemResult{
              WindowsBootstrapFilesystemStatus::BackendFailure};
    const auto published = prepared.ok()
        ? port.Apply(intent(BootstrapIntentKind::ReplaceInventoryItem,
                            "HarkenSearch.exe"))
        : WindowsBootstrapFilesystemResult{
              WindowsBootstrapFilesystemStatus::BackendFailure};
    const bool healthy = !removeDirectory ||
        (published.ok() && checkpointHealthyChildForCommit(port));
    const auto terminal = published.ok() && healthy
        ? (removeDirectory
               ? port.Apply(intent(BootstrapIntentKind::CommitJournal))
               : port.Apply(intent(BootstrapIntentKind::PersistJournal, {},
                                  "rolling-back")))
        : WindowsBootstrapFilesystemResult{
              WindowsBootstrapFilesystemStatus::BackendFailure};
    if (!begun.ok() || !prepared.ok() || !published.ok() || !healthy ||
        !terminal.ok()) {
        return 43;
    }
    // Exercise one destructive operation per child.  The directory case
    // leaves the committed new target in place and cuts only while removing
    // the empty backup workspace; the file case cuts while removing the new
    // target itself.  Calling the first delete for both cases would consume
    // the one-shot hook before the intended directory cut.
    const auto interrupted = removeDirectory
        ? port.Apply(intent(BootstrapIntentKind::RemoveBackup))
        : port.Apply(intent(BootstrapIntentKind::RemoveNewItem,
                            "HarkenSearch.exe"));
    if (interrupted.backendStatus !=
        WindowsFilesystemBackendStatus::DurabilityFailed) {
        return 120 + static_cast<int>(interrupted.status);
    }
    terminateAtCrashCut();
}

bool launchJournalTempCrashChild(const std::filesystem::path& executable,
                                 const std::filesystem::path& installPath,
                                 const std::filesystem::path& stagingPath,
                                 const std::filesystem::path& transactionPath,
                                 DWORD& exitCode,
                                 const bool beforeDirectoryFlush = false) {
    std::string command = "\"" + executable.u8string() +
        "\" " + (beforeDirectoryFlush
                       ? "--bootstrap-child-journal-temp-before-directory"
                       : "--bootstrap-child-journal-temp") +
        " \"" + installPath.u8string() +
        "\" \"" + stagingPath.u8string() + "\" \"" +
        transactionPath.u8string() + "\"";
    std::vector<char> mutableCommand(command.begin(), command.end());
    mutableCommand.push_back('\0');
    STARTUPINFOA startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    const BOOL created = CreateProcessA(
        nullptr, mutableCommand.data(), nullptr, nullptr, FALSE,
        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process);
    if (!created) return false;
    const ChildProcessWaitOutcome outcome = waitForChildProcessBounded(
        process, kChildWaitTimeoutMilliseconds);
    exitCode = outcome.exitCode;
    return outcome.completed;
}

bool launchTargetAtomicCrashChild(const std::filesystem::path& executable,
                                  const std::filesystem::path& installPath,
                                  const std::filesystem::path& stagingPath,
                                  const std::filesystem::path& transactionPath,
                                  DWORD& exitCode) {
    std::string command = "\"" + executable.u8string() +
        "\" --bootstrap-child-target-atomic \"" + installPath.u8string() +
        "\" \"" + stagingPath.u8string() + "\" \"" +
        transactionPath.u8string() + "\"";
    std::vector<char> mutableCommand(command.begin(), command.end());
    mutableCommand.push_back('\0');
    STARTUPINFOA startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    const BOOL created = CreateProcessA(
        nullptr, mutableCommand.data(), nullptr, nullptr, FALSE,
        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process);
    if (!created) return false;
    const ChildProcessWaitOutcome outcome = waitForChildProcessBounded(
        process, kChildWaitTimeoutMilliseconds);
    exitCode = outcome.exitCode;
    return outcome.completed;
}

bool launchTargetPreparedCrashChild(
    const std::filesystem::path& executable,
    const std::filesystem::path& installPath,
    const std::filesystem::path& stagingPath,
    const std::filesystem::path& transactionPath,
    DWORD& exitCode) {
    std::string command = "\"" + executable.u8string() +
        "\" --bootstrap-child-target-prepared \"" + installPath.u8string() +
        "\" \"" + stagingPath.u8string() + "\" \"" +
        transactionPath.u8string() + "\"";
    std::vector<char> mutableCommand(command.begin(), command.end());
    mutableCommand.push_back('\0');
    STARTUPINFOA startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    const BOOL created = CreateProcessA(
        nullptr, mutableCommand.data(), nullptr, nullptr, FALSE,
        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process);
    if (!created) return false;
    const ChildProcessWaitOutcome outcome = waitForChildProcessBounded(
        process, kChildWaitTimeoutMilliseconds);
    exitCode = outcome.exitCode;
    return outcome.completed;
}

bool launchJournalSlotCrashChild(const std::filesystem::path& executable,
                                 const std::filesystem::path& installPath,
                                 const std::filesystem::path& stagingPath,
                                 const std::filesystem::path& transactionPath,
                                 DWORD& exitCode) {
    std::string command = "\"" + executable.u8string() +
        "\" --bootstrap-child-journal-slot \"" + installPath.u8string() +
        "\" \"" + stagingPath.u8string() + "\" \"" +
        transactionPath.u8string() + "\"";
    std::vector<char> mutableCommand(command.begin(), command.end());
    mutableCommand.push_back('\0');
    STARTUPINFOA startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    const BOOL created = CreateProcessA(
        nullptr, mutableCommand.data(), nullptr, nullptr, FALSE,
        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process);
    if (!created) return false;
    const ChildProcessWaitOutcome outcome = waitForChildProcessBounded(
        process, kChildWaitTimeoutMilliseconds);
    exitCode = outcome.exitCode;
    return outcome.completed;
}

bool launchDeleteCrashChild(const std::filesystem::path& executable,
                            const char* operation,
                            std::size_t pointIndex,
                            const std::filesystem::path& installPath,
                            const std::filesystem::path& stagingPath,
                            const std::filesystem::path& transactionPath,
                            DWORD& exitCode) {
    std::string command = "\"" + executable.u8string() +
        "\" --bootstrap-child-delete-cut " + operation + " " +
        std::to_string(pointIndex) + " \"" + installPath.u8string() +
        "\" \"" + stagingPath.u8string() + "\" \"" +
        transactionPath.u8string() + "\"";
    std::vector<char> mutableCommand(command.begin(), command.end());
    mutableCommand.push_back('\0');
    STARTUPINFOA startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    const BOOL created = CreateProcessA(
        nullptr, mutableCommand.data(), nullptr, nullptr, FALSE,
        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process);
    if (!created) return false;
    const ChildProcessWaitOutcome outcome = waitForChildProcessBounded(
        process, kChildWaitTimeoutMilliseconds);
    exitCode = outcome.exitCode;
    return outcome.completed;
}

bool writeNativeFixtureFile(const std::filesystem::path& path,
                            const std::string& bytes) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    stream.flush();
    return stream.good();
}

std::string readNativeFixtureFile(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(stream),
                       std::istreambuf_iterator<char>());
}

#if defined(_WIN32)
struct NativeFullEaInformation {
    ULONG NextEntryOffset;
    UCHAR Flags;
    UCHAR EaNameLength;
    USHORT EaValueLength;
    CHAR EaName[1];
};

bool setNativeEaFile(const std::filesystem::path& path,
                     const std::string& name,
                     const std::string& value) {
    if (name.empty() || name.size() > 255U ||
        value.size() > static_cast<std::size_t>(65535U)) {
        return false;
    }
    HANDLE handle = CreateFileW(
        path.c_str(), FILE_WRITE_EA,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return false;

    using NtSetEaFileFunction = NTSTATUS(NTAPI*)(HANDLE, PIO_STATUS_BLOCK,
                                                 PVOID, ULONG);
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    auto ntSetEaFile = ntdll == nullptr
        ? nullptr
        : reinterpret_cast<NtSetEaFileFunction>(
              GetProcAddress(ntdll, "NtSetEaFile"));
    if (ntSetEaFile == nullptr) {
        CloseHandle(handle);
        return false;
    }

    const std::size_t header = offsetof(NativeFullEaInformation, EaName);
    const std::size_t total = header + name.size() + 1U + value.size();
    std::vector<std::uint8_t> buffer(total, 0U);
    auto* ea = reinterpret_cast<NativeFullEaInformation*>(buffer.data());
    ea->NextEntryOffset = 0U;
    ea->Flags = 0U;
    ea->EaNameLength = static_cast<UCHAR>(name.size());
    ea->EaValueLength = static_cast<USHORT>(value.size());
    std::memcpy(ea->EaName, name.data(), name.size());
    ea->EaName[name.size()] = '\0';
    std::memcpy(ea->EaName + name.size() + 1U, value.data(), value.size());

    IO_STATUS_BLOCK io{};
    const NTSTATUS status = ntSetEaFile(
        handle, &io, buffer.data(), static_cast<ULONG>(total));
    CloseHandle(handle);
    return status >= 0;
}
#endif
bool setNativeBasicInfo(const std::filesystem::path& path,
                        const FILE_BASIC_INFO& basic) {
    HANDLE handle = CreateFileW(
        path.c_str(), FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return false;
    const BOOL updated = SetFileInformationByHandle(
        handle, FileBasicInfo, const_cast<FILE_BASIC_INFO*>(&basic),
        sizeof(basic));
    CloseHandle(handle);
    return updated != FALSE;
}

bool getNativeBasicInfo(const std::filesystem::path& path,
                        FILE_BASIC_INFO& basic) {
    HANDLE handle = CreateFileW(
        path.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT |
            FILE_FLAG_BACKUP_SEMANTICS,
        nullptr);
    if (handle == INVALID_HANDLE_VALUE) return false;
    const BOOL observed = GetFileInformationByHandleEx(
        handle, FileBasicInfo, &basic, sizeof(basic));
    CloseHandle(handle);
    return observed != FALSE;
}

bool sameNativeBasicInfo(const FILE_BASIC_INFO& left,
                         const FILE_BASIC_INFO& right) {
    const bool same =
           left.CreationTime.QuadPart == right.CreationTime.QuadPart &&
           left.LastAccessTime.QuadPart == right.LastAccessTime.QuadPart &&
           left.LastWriteTime.QuadPart == right.LastWriteTime.QuadPart &&
           left.ChangeTime.QuadPart == right.ChangeTime.QuadPart &&
           left.FileAttributes == right.FileAttributes;
    return same;
}

// Opening a restored file for an external inspection can itself materialize
// NTFS LastAccessTime. The adapter's journal-bound snapshot and its
// pre-publication ApplyMetadata check cover that field exactly; this physical
// post-publication probe therefore compares the non-observational fields.
bool sameNativeBasicInfoExceptLastAccess(const FILE_BASIC_INFO& left,
                                         const FILE_BASIC_INFO& right) {
    return left.CreationTime.QuadPart == right.CreationTime.QuadPart &&
           left.LastWriteTime.QuadPart == right.LastWriteTime.QuadPart &&
           left.ChangeTime.QuadPart == right.ChangeTime.QuadPart &&
           left.FileAttributes == right.FileAttributes;
}

void test_native_basic_info_comparison_includes_last_access() {
#if defined(_WIN32)
    FILE_BASIC_INFO left{};
    FILE_BASIC_INFO right{};
    right.LastAccessTime.QuadPart = 1;
    check(!sameNativeBasicInfo(left, right),
          "native basic-info comparison includes last-access time");
#endif
}

bool protectNativeDacl(const std::filesystem::path& path) {
    HANDLE handle = CreateFileW(
        path.c_str(), READ_CONTROL | WRITE_DAC,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return false;
    PACL dacl = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    const DWORD read = GetSecurityInfo(
        handle, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
        nullptr, nullptr, &dacl, nullptr, &descriptor);
    DWORD written = read;
    if (read == ERROR_SUCCESS) {
        written = SetSecurityInfo(
            handle, SE_FILE_OBJECT,
            DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
            nullptr, nullptr, dacl, nullptr);
    }
    if (descriptor != nullptr) LocalFree(descriptor);
    CloseHandle(handle);
    return written == ERROR_SUCCESS;
}

bool setNativeNullMandatoryLabel(const std::filesystem::path& path) {
    HANDLE handle = CreateFileW(
        path.c_str(), READ_CONTROL | WRITE_OWNER,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return false;
    const DWORD written = SetSecurityInfo(
        handle, SE_FILE_OBJECT, LABEL_SECURITY_INFORMATION,
        nullptr, nullptr, nullptr, nullptr);
    CloseHandle(handle);
    return written == ERROR_SUCCESS;
}

void test_native_mandatory_label_equivalence_ignores_empty_provenance() {
    SECURITY_DESCRIPTOR absent{};
    SECURITY_DESCRIPTOR present{};
    ACL emptyAcl{};
    const bool initialized =
        InitializeSecurityDescriptor(&absent,
                                     SECURITY_DESCRIPTOR_REVISION) != FALSE &&
        InitializeSecurityDescriptor(&present,
                                     SECURITY_DESCRIPTOR_REVISION) != FALSE &&
        InitializeAcl(&emptyAcl, sizeof(emptyAcl), ACL_REVISION) != FALSE &&
        SetSecurityDescriptorSacl(&absent, FALSE, nullptr, TRUE) != FALSE &&
        SetSecurityDescriptorSacl(&present, TRUE, &emptyAcl, FALSE) != FALSE &&
        SetSecurityDescriptorControl(
            &absent,
            SE_SACL_AUTO_INHERITED,
            SE_SACL_AUTO_INHERITED) != FALSE;
    check(initialized,
          "mandatory-label provenance fixture initializes descriptors");
    check(initialized &&
              aris::bootstrap::
                  WindowsBootstrapMandatoryLabelDescriptorsEquivalentForTesting(
                      &absent, &present),
          "absent and empty mandatory labels remain equivalent across Windows provenance normalization");
}

struct NativeTemporarySecurityProbe {
    bool enabled = false;
    bool called = false;
    bool changed = false;
};

struct NativePublishedChangeTimeProbe {
    bool called = false;
    bool changed = false;
};

struct NativeAppliedChangeTimeProbe {
    bool called = false;
    bool changed = false;
};

void normalizeNativeAppliedChangeTime(
    void* context,
    WindowsFilesystemRootRole,
    const std::string&,
    WindowsFilesystemHandle fileHandle) {
    auto& probe = *static_cast<NativeAppliedChangeTimeProbe*>(context);
    probe.called = true;
    HANDLE handle = reinterpret_cast<HANDLE>(fileHandle);
    FILE_BASIC_INFO basic{};
    if (GetFileInformationByHandleEx(
            handle, FileBasicInfo, &basic, sizeof(basic)) == FALSE) {
        return;
    }
    basic.ChangeTime.QuadPart += 10'000LL;
    probe.changed = SetFileInformationByHandle(
        handle, FileBasicInfo, &basic, sizeof(basic)) != FALSE;
}

void normalizeNativePublishedChangeTime(
    void* context,
    WindowsFilesystemRootRole,
    const std::string&,
    WindowsFilesystemHandle publishedHandle) {
    auto& probe = *static_cast<NativePublishedChangeTimeProbe*>(context);
    probe.called = true;
    HANDLE handle = reinterpret_cast<HANDLE>(publishedHandle);
    FILE_BASIC_INFO basic{};
    if (GetFileInformationByHandleEx(
            handle, FileBasicInfo, &basic, sizeof(basic)) == FALSE) {
        return;
    }
    basic.ChangeTime.QuadPart += 10'000LL;
    probe.changed = SetFileInformationByHandle(
        handle, FileBasicInfo, &basic, sizeof(basic)) != FALSE;
}

void attemptNativeTemporarySecurityMutation(
    void* context,
    WindowsFilesystemRootRole,
    const std::string&,
    const std::string&,
    WindowsFilesystemHandle temporaryHandle) {
    auto& probe = *static_cast<NativeTemporarySecurityProbe*>(context);
    if (!probe.enabled) return;
    probe.called = true;
    HANDLE handle = reinterpret_cast<HANDLE>(temporaryHandle);
    PACL dacl = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    const DWORD read = GetSecurityInfo(
        handle, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
        nullptr, nullptr, &dacl, nullptr, &descriptor);
    DWORD written = read;
    if (read == ERROR_SUCCESS) {
        written = SetSecurityInfo(
            handle, SE_FILE_OBJECT,
            DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
            nullptr, nullptr, dacl, nullptr);
    }
    if (descriptor != nullptr) LocalFree(descriptor);
    probe.changed = written == ERROR_SUCCESS;
}

bool nativeSecurityText(const std::filesystem::path& path,
                        std::wstring& text,
                        bool& daclProtected) {
    text.clear();
    daclProtected = false;
    HANDLE handle = CreateFileW(
        path.c_str(), READ_CONTROL,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return false;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    const DWORD read = GetSecurityInfo(
        handle, SE_FILE_OBJECT,
        OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION |
            DACL_SECURITY_INFORMATION,
        nullptr, nullptr, nullptr, nullptr, &descriptor);
    CloseHandle(handle);
    if (read != ERROR_SUCCESS || descriptor == nullptr) {
        if (descriptor != nullptr) LocalFree(descriptor);
        return false;
    }
    SECURITY_DESCRIPTOR_CONTROL control = 0;
    DWORD revision = 0;
    const bool controlRead = GetSecurityDescriptorControl(
        descriptor, &control, &revision) != FALSE;
    LPWSTR value = nullptr;
    const bool converted = ConvertSecurityDescriptorToStringSecurityDescriptorW(
        descriptor, SDDL_REVISION_1,
        OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION |
            DACL_SECURITY_INFORMATION,
        &value, nullptr) != FALSE;
    if (controlRead) {
        daclProtected = (control & SE_DACL_PROTECTED) != 0;
    }
    if (converted && value != nullptr) text = value;
    if (value != nullptr) LocalFree(value);
    LocalFree(descriptor);
    return controlRead && converted;
}

struct NativeParentRenameProbe {
    std::filesystem::path source;
    std::filesystem::path destination;
    bool called = false;
    WindowsFilesystemRootRole rootRole = WindowsFilesystemRootRole::Staging;
    std::string relativePath;
    std::error_code renameError;
};

struct NativeRuntimePairSwapProbe {
    std::filesystem::path transactionRoot;
    bool called = false;
    bool swapped = false;
    std::error_code directoryRenameError;
    std::error_code bootstrapMoveError;
    std::error_code replacementRenameError;
};

struct NativeJunctionProbe {
    std::filesystem::path junction;
    std::filesystem::path target;
    bool called = false;
    bool created = false;
    DWORD nativeError = ERROR_SUCCESS;
};

struct NativeMountPointReparseData {
    DWORD tag = 0;
    USHORT dataLength = 0;
    USHORT reserved = 0;
    USHORT substituteNameOffset = 0;
    USHORT substituteNameLength = 0;
    USHORT printNameOffset = 0;
    USHORT printNameLength = 0;
    wchar_t pathBuffer[1]{};
};

bool createNativeJunction(const std::filesystem::path& junction,
                          const std::filesystem::path& target,
                          DWORD& nativeError) {
    nativeError = ERROR_SUCCESS;
    HANDLE handle = CreateFileW(
        junction.c_str(), GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        nativeError = GetLastError();
        return false;
    }

    const std::wstring substitute = L"\\??\\" + target.native();
    const std::wstring printName = target.native();
    const std::size_t substituteBytes = substitute.size() * sizeof(wchar_t);
    const std::size_t printBytes = printName.size() * sizeof(wchar_t);
    const std::size_t pathBytes = substituteBytes + sizeof(wchar_t) +
                                  printBytes + sizeof(wchar_t);
    const std::size_t headerBytes =
        offsetof(NativeMountPointReparseData, pathBuffer);
    std::vector<std::uint8_t> buffer(headerBytes + pathBytes, 0U);
    auto* reparse =
        reinterpret_cast<NativeMountPointReparseData*>(buffer.data());
    reparse->tag = IO_REPARSE_TAG_MOUNT_POINT;
    reparse->dataLength = static_cast<USHORT>(buffer.size() - 8U);
    reparse->substituteNameOffset = 0;
    reparse->substituteNameLength = static_cast<USHORT>(substituteBytes);
    reparse->printNameOffset =
        static_cast<USHORT>(substituteBytes + sizeof(wchar_t));
    reparse->printNameLength = static_cast<USHORT>(printBytes);
    std::memcpy(reparse->pathBuffer, substitute.data(), substituteBytes);
    std::memcpy(
        reinterpret_cast<std::uint8_t*>(reparse->pathBuffer) +
            reparse->printNameOffset,
        printName.data(), printBytes);

    DWORD returned = 0;
    const BOOL created = DeviceIoControl(
        handle, FSCTL_SET_REPARSE_POINT, buffer.data(),
        static_cast<DWORD>(buffer.size()), nullptr, 0, &returned, nullptr);
    if (!created) nativeError = GetLastError();
    CloseHandle(handle);
    return created != FALSE;
}

void attemptNativeJunction(
    void* context,
    WindowsFilesystemRootRole,
    const std::string&) {
    auto& probe = *static_cast<NativeJunctionProbe*>(context);
    probe.called = true;
    probe.created = createNativeJunction(probe.junction, probe.target,
                                         probe.nativeError);
}

void attemptNativeParentRename(
    void* context,
    WindowsFilesystemRootRole rootRole,
    const std::string& relativePath) {
    auto& probe = *static_cast<NativeParentRenameProbe*>(context);
    probe.called = true;
    probe.rootRole = rootRole;
    probe.relativePath = relativePath;
    std::filesystem::rename(probe.source, probe.destination,
                            probe.renameError);
}

void test_fresh_recovery_rejects_recreated_runtime_pair_before_mutation() {
    const Fixture values = fixture();
    ScriptedBackend completedBackend;
    {
        WindowsBootstrapFilesystemPort completed(completedBackend);
        check(completed.Begin(values.plan, values.journal, roots()).ok() &&
                  completed.Apply(intent(BootstrapIntentKind::PersistJournal,
                                         {}, "prepared")).ok() &&
                  completed.Apply(intent(BootstrapIntentKind::SnapshotBackup,
                                         "HarkenSearch.exe")).ok() &&
                  completed.Apply(intent(BootstrapIntentKind::ReplaceInventoryItem,
                                         "HarkenSearch.exe")).ok() &&
                  completed.Apply(intent(BootstrapIntentKind::ReplaceInventoryItem,
                                         "resources/core.pak")).ok() &&
                  checkpointHealthyChildForCommit(completed) &&
                  completed.Apply(intent(BootstrapIntentKind::CommitJournal)).ok() &&
                  completed.Apply(intent(BootstrapIntentKind::RemoveBackup)).ok() &&
                  completed.CleanupCommittedInvocation().ok(),
              "runtime-pair recovery fixture reaches terminal cleanup");
    }

    ScriptedBackend fresh;
    fresh.entries = completedBackend.entries;
    fresh.contents = completedBackend.contents;
    fresh.metadatas = completedBackend.metadatas;
    fresh.backupDirectoryAlreadyExists = completedBackend.backupDirectoryAlreadyExists;
    bindRecoveryJournalCandidates(fresh, completedBackend);
    fresh.setByHandle(ScriptedBackend::kTransaction, "runtime-pair",
                      WindowsFilesystemEntryKind::Directory, identity(199U));
    fresh.setByHandle(ScriptedBackend::kTransaction,
                      "runtime-pair/HarkenSearchBootstrap.exe",
                      WindowsFilesystemEntryKind::RegularFile, identity(200U));
    fresh.setContentByHandle(ScriptedBackend::kTransaction,
                             "runtime-pair/HarkenSearchBootstrap.exe",
                             fingerprint(0xcdU, 4096U));

    WindowsBootstrapFilesystemPort recovered(fresh);
    const auto recovery = recovered.BeginRecovery(values.plan, values.journal,
                                                  roots());
    check(recovery.disposition == WindowsBootstrapRecoveryDisposition::Quarantine &&
              recovery.filesystem.status ==
                  WindowsBootstrapFilesystemStatus::IdentityMismatch &&
              fresh.promotionCalls == 0 && fresh.persistCalls == 0,
          "fresh recovery rejects a recreated runtime-pair before journal or residue mutation");
}

void attemptNativeRuntimePairSwap(
    void* context,
    WindowsFilesystemRootRole rootRole,
    const std::string& relativePath) {
    auto& probe = *static_cast<NativeRuntimePairSwapProbe*>(context);
    if (rootRole != WindowsFilesystemRootRole::Transaction ||
        relativePath != "runtime-pair") {
        return;
    }
    probe.called = true;
    const std::filesystem::path active =
        probe.transactionRoot / "runtime-pair";
    const std::filesystem::path original =
        probe.transactionRoot / "runtime-pair-original";
    const std::filesystem::path replacement =
        probe.transactionRoot / "runtime-pair-replacement";
    std::filesystem::rename(active, original, probe.directoryRenameError);
    if (probe.directoryRenameError) return;
    std::filesystem::rename(original / "HarkenSearchBootstrap.exe",
                            replacement / "HarkenSearchBootstrap.exe",
                            probe.bootstrapMoveError);
    if (probe.bootstrapMoveError) return;
    std::filesystem::rename(replacement, active,
                            probe.replacementRenameError);
    probe.swapped = !probe.replacementRenameError;
}

template <typename Attempt, typename VerifyUnchanged>
WindowsFilesystemBackendResult retryNativeAccessDenied(
    Attempt&& attempt,
    VerifyUnchanged&& verifyUnchanged,
    const char* unchangedMessage) {
    WindowsFilesystemBackendResult result;
    for (std::uint32_t retry = 0; retry < 50; ++retry) {
        result = attempt();
        if (result.status != WindowsFilesystemBackendStatus::AccessDenied) {
            return result;
        }
        check(verifyUnchanged(), unchangedMessage);
        Sleep(10);
    }
    return result;
}
#endif

void test_native_backend_publishes_inside_synthetic_sandbox() {
#if defined(_WIN32)
    NativeSandbox sandbox;
    NativeSandboxCleanupAssertion cleanup(sandbox, "native-publish");
    const std::filesystem::path base = NativeSandbox::systemTempRoot();
    for (std::uint32_t attempt = 0; attempt < 32 && !sandbox.owned; ++attempt) {
        sandbox.path = base /
            ("aris-bootstrap-native-" + std::to_string(GetCurrentProcessId()) +
             "-" + std::to_string(attempt));
        std::error_code createError;
        sandbox.owned = std::filesystem::create_directory(sandbox.path,
                                                          createError);
    }
    check(sandbox.owned, "native synthetic sandbox is exclusively created");
    if (!sandbox.owned) return;

    const std::filesystem::path installPath = sandbox.path / "install";
    const std::filesystem::path updaterPath = sandbox.path / "updater";
    const std::filesystem::path stagingPath = updaterPath / "staging";
    const std::filesystem::path transactionPath = updaterPath / "transaction";
    std::error_code directoryError;
    const bool directoriesCreated =
        std::filesystem::create_directory(installPath, directoryError) &&
        std::filesystem::create_directory(updaterPath, directoryError) &&
        std::filesystem::create_directory(stagingPath, directoryError) &&
        std::filesystem::create_directory(transactionPath, directoryError);
    check(directoriesCreated,
          "native synthetic roots keep install outside the updater base");
    if (!directoriesCreated) return;

    constexpr char kOldBytes[] = "old-native";
    constexpr char kNewBytes[] = "new-native";
    check(writeNativeFixtureFile(installPath / "HarkenSearch.exe", kOldBytes) &&
              writeNativeFixtureFile(stagingPath / "HarkenSearch.exe", kNewBytes),
          "native synthetic inventory bytes are written");
    check(protectNativeDacl(installPath / "HarkenSearch.exe"),
          "native rollback fixture protects its old DACL");
    check(setNativeNullMandatoryLabel(installPath / "HarkenSearch.exe"),
          "native rollback fixture preserves a present NULL mandatory label");
    FILE_BASIC_INFO expectedOldBasic{};
    expectedOldBasic.CreationTime.QuadPart = 132537600000000000LL;
    expectedOldBasic.LastAccessTime.QuadPart = 132537600100000000LL;
    expectedOldBasic.LastWriteTime.QuadPart = 132537600200000000LL;
    expectedOldBasic.ChangeTime.QuadPart = 132537600300000000LL;
    expectedOldBasic.FileAttributes = FILE_ATTRIBUTE_HIDDEN |
                                      FILE_ATTRIBUTE_ARCHIVE |
                                      FILE_ATTRIBUTE_NOT_CONTENT_INDEXED;
    check(setNativeBasicInfo(installPath / "HarkenSearch.exe", expectedOldBasic) &&
              getNativeBasicInfo(installPath / "HarkenSearch.exe", expectedOldBasic),
          "native rollback fixture records deterministic old metadata");
    std::wstring expectedOldSecurity;
    bool expectedOldDaclProtected = false;
    check(nativeSecurityText(installPath / "HarkenSearch.exe", expectedOldSecurity,
                             expectedOldDaclProtected) &&
              expectedOldDaclProtected,
          "native rollback fixture records protected owner/group/DACL metadata");
    std::wstring stagingSecurity;
    bool stagingDaclProtected = false;
    check(nativeSecurityText(stagingPath / "HarkenSearch.exe", stagingSecurity,
                             stagingDaclProtected) &&
              (!stagingDaclProtected || stagingSecurity != expectedOldSecurity),
          "native staging fixture has security metadata distinct from the trusted target");

    NativeTemporarySecurityProbe temporarySecurityProbe;
    WindowsBootstrapFilesystemNativeTestHooks nativeHooks;
    nativeHooks.context = &temporarySecurityProbe;
    nativeHooks.beforeTemporaryPublish =
        &attemptNativeTemporarySecurityMutation;
    auto backend = createAuthorizedNativeBackend(
        installPath, stagingPath, transactionPath, nativeHooks);
    check(backend != nullptr, "native synthetic backend is available");
    if (backend == nullptr) return;

    WindowsFilesystemHandle installRoot = 0;
    WindowsFilesystemHandle stagingRoot = 0;
    WindowsFilesystemHandle transactionRoot = 0;
    WindowsFilesystemHandle backupRoot = 0;
    WindowsFileIdentity installRootIdentity;
    WindowsFileIdentity stagingRootIdentity;
    WindowsFileIdentity transactionRootIdentity;
    WindowsFileIdentity backupRootIdentity;
    const bool rootsBound =
        backend->OpenRootExact(installPath.u8string(),
                               WindowsFilesystemRootRole::Install,
                               installRoot, installRootIdentity).ok() &&
        backend->OpenRootExact(stagingPath.u8string(),
                               WindowsFilesystemRootRole::Staging,
                               stagingRoot, stagingRootIdentity).ok() &&
        backend->OpenRootExact(transactionPath.u8string(),
                               WindowsFilesystemRootRole::Transaction,
                               transactionRoot,
                               transactionRootIdentity).ok();
    check(rootsBound,
          "native synthetic roots bind exact handles on one volume");
    if (!rootsBound) return;

    const std::filesystem::path literalTildeRoot =
        sandbox.path / "R&D~Audio";
    std::error_code tildeRootError;
    check(std::filesystem::create_directory(literalTildeRoot, tildeRootError) &&
              !tildeRootError,
          "native literal-tilde root fixture is created");
    WindowsFilesystemHandle tildeRootHandle = 0;
    WindowsFileIdentity tildeRootIdentity;
    const WindowsFilesystemBackendResult tildeRootOpened =
        backend->OpenRootExact(literalTildeRoot.u8string(),
                               WindowsFilesystemRootRole::Backup,
                               tildeRootHandle, tildeRootIdentity);
    check(tildeRootOpened.ok() && tildeRootHandle != 0 &&
              tildeRootIdentity.valid,
          "native absolute trusted roots allow literal tilde components");
    if (tildeRootHandle != 0) backend->CloseFilesystemHandle(tildeRootHandle);

    const std::filesystem::path mismatchedCanonicalCase =
        sandbox.path / "r&d~audio";
    WindowsFilesystemHandle mismatchedRootHandle = 0;
    WindowsFileIdentity mismatchedRootIdentity;
    const WindowsFilesystemBackendResult mismatchedRootOpened =
        backend->OpenRootExact(mismatchedCanonicalCase.u8string(),
                               WindowsFilesystemRootRole::Backup,
                               mismatchedRootHandle,
                               mismatchedRootIdentity);
    check(mismatchedRootOpened.status ==
              WindowsFilesystemBackendStatus::ExactPathMismatch &&
              mismatchedRootHandle == 0 && !mismatchedRootIdentity.valid,
          "native root authority rejects a spelling whose opened canonical final path differs");
    if (mismatchedRootHandle != 0) {
        backend->CloseFilesystemHandle(mismatchedRootHandle);
    }

    WindowsFilesystemObservation shortNameAliasObservation;
    const WindowsFilesystemBackendResult shortNameAliasResult =
        backend->ObserveNoFollow(stagingRoot, "ARIS~1.EXE",
                                 shortNameAliasObservation);
    check(shortNameAliasResult.status ==
              WindowsFilesystemBackendStatus::InvalidPath,
          "native relative adapter rejects 8.3 alias-shaped names before observation");

    const auto forgedJournalNeighbor = transactionPath / "forged.bin";
    check(writeNativeFixtureFile(forgedJournalNeighbor, "caller-forged"),
          "native journal namespace RED fixture creates an unknown neighbor");
    WindowsFileIdentity rejectedIdentity;
    WindowsFileContentFingerprint rejectedContent;
    const auto rejectedNamespaceJournal = backend->PersistJournalDurable(
        transactionRoot, {0x41U}, WindowsFilesystemReplaceMode::CreateNew, {},
        {}, rejectedIdentity, rejectedContent);
    check(rejectedNamespaceJournal.status ==
              WindowsFilesystemBackendStatus::AlreadyExists &&
              !std::filesystem::exists(transactionPath /
                                        "aris-bootstrap.journal.a"),
          "unknown transaction-root files block journal publication before any slot write");
    std::error_code forgedCleanupError;
    check(std::filesystem::remove(forgedJournalNeighbor, forgedCleanupError) &&
              !forgedCleanupError,
          "native journal namespace RED fixture is removed before the green path");

    WindowsFileIdentity journalIdentity;
    WindowsFileContentFingerprint journalContent;
    const std::vector<std::uint8_t> firstJournal{1U, 2U, 3U, 4U};
    const WindowsFilesystemBackendResult firstJournalPersisted =
        backend->PersistJournalDurable(
            transactionRoot, firstJournal,
            WindowsFilesystemReplaceMode::CreateNew, {}, {},
            journalIdentity, journalContent);
    checkNative(firstJournalPersisted,
                "native create-new journal publication survives handle rebind");
    const bool journalReady = firstJournalPersisted.ok() &&
        journalIdentity.valid && journalContent.valid;
    check(journalReady,
          "successful native journal publication returns identity and content");
    if (!journalReady) return;
    const std::vector<std::uint8_t> secondJournal{5U, 6U, 7U};
    WindowsFileIdentity secondJournalIdentity;
    WindowsFileContentFingerprint secondJournalContent;
    const bool secondJournalReady = backend->PersistJournalDurable(
              transactionRoot, secondJournal,
              WindowsFilesystemReplaceMode::ReplaceExisting,
              journalIdentity, journalContent,
              secondJournalIdentity, secondJournalContent).ok() &&
              secondJournalIdentity.valid && secondJournalContent.valid;
    check(secondJournalReady,
          "native identity-bound journal replacement survives handle rebind");
    if (!secondJournalReady) return;

    const auto forgedLeaseNeighbor = transactionPath / "lease-forged.bin";
    check(writeNativeFixtureFile(forgedLeaseNeighbor, "caller-forged"),
          "native lease namespace fixture creates an unknown neighbor");
    WindowsFilesystemHandle rejectedLease = 0;
    const auto rejectedLeaseResult = backend->AcquireJournalMutationLease(
        transactionRoot, secondJournalIdentity, secondJournalContent,
        rejectedLease);
    check(rejectedLeaseResult.status ==
              WindowsFilesystemBackendStatus::AlreadyExists &&
              rejectedLease == 0,
          "journal lease rejects unknown transaction-root namespace before mutation authority");
    if (rejectedLease != 0) {
        backend->ReleaseJournalMutationLease(rejectedLease);
    }
    std::error_code forgedLeaseCleanup;
    check(std::filesystem::remove(forgedLeaseNeighbor, forgedLeaseCleanup) &&
              !forgedLeaseCleanup,
          "native lease namespace fixture removes its unknown neighbor");

    const std::filesystem::path activeJournalPath =
        transactionPath / "aris-bootstrap.journal.b";
    const std::wstring journalStreamPath =
        activeJournalPath.native() + L":lease-test";
    HANDLE journalStream = CreateFileW(
        journalStreamPath.c_str(), GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    check(journalStream != INVALID_HANDLE_VALUE,
          "native lease metadata fixture creates a journal ADS");
    if (journalStream != INVALID_HANDLE_VALUE) CloseHandle(journalStream);
    WindowsFilesystemHandle rejectedMetadataLease = 0;
    const auto rejectedMetadataResult = backend->AcquireJournalMutationLease(
        transactionRoot, secondJournalIdentity, secondJournalContent,
        rejectedMetadataLease);
    check(rejectedMetadataResult.status ==
              WindowsFilesystemBackendStatus::UnsupportedMetadata &&
              rejectedMetadataLease == 0,
          "journal lease rejects unsupported journal metadata before mutation authority");
    if (rejectedMetadataLease != 0) {
        backend->ReleaseJournalMutationLease(rejectedMetadataLease);
    }
    check(DeleteFileW(journalStreamPath.c_str()) != FALSE,
          "native lease metadata fixture removes its rejected ADS");

    WindowsFilesystemHandle deniedBackupRoot = 0;
    WindowsFileIdentity deniedBackupIdentity;
    const auto unleasedDirectory = backend->OpenOrCreateDirectoryNoFollow(
        transactionRoot, "backup", true, deniedBackupRoot, deniedBackupIdentity);
    check(unleasedDirectory.status == WindowsFilesystemBackendStatus::AccessDenied &&
              !std::filesystem::exists(transactionPath / "backup"),
          "native backend refuses mutation without an authenticated journal lease");

    WindowsFilesystemHandle journalLease = 0;
    const bool leaseReady = backend->AcquireJournalMutationLease(
              transactionRoot, secondJournalIdentity, secondJournalContent,
              journalLease).ok() && journalLease != 0;
    check(leaseReady,
          "native journal lease pins identity and content before mutation");
    if (!leaseReady) return;
    HANDLE tamper = CreateFileW(
        activeJournalPath.c_str(), GENERIC_WRITE | DELETE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    const DWORD tamperError = tamper == INVALID_HANDLE_VALUE
        ? GetLastError()
        : ERROR_SUCCESS;
    if (tamper != INVALID_HANDLE_VALUE) CloseHandle(tamper);
    check(tamper == INVALID_HANDLE_VALUE &&
              tamperError == ERROR_SHARING_VIOLATION,
          "native journal lease denies concurrent journal write and delete access");

    WindowsFilesystemHandle absentBackupRoot = 0;
    WindowsFileIdentity absentBackupIdentity;
    const auto absentBackupOpened = backend->OpenOrCreateDirectoryNoFollow(
        transactionRoot, "backup", false, absentBackupRoot,
        absentBackupIdentity);
    check(absentBackupOpened.status == WindowsFilesystemBackendStatus::NotFound &&
              absentBackupRoot == 0 && !absentBackupIdentity.valid &&
              !std::filesystem::exists(transactionPath / "backup"),
          "recovery-style backup open never creates a missing directory");
    if (absentBackupRoot != 0) {
        backend->CloseFilesystemHandle(absentBackupRoot);
    }
    if (std::filesystem::exists(transactionPath / "backup")) {
        std::error_code unexpectedBackupError;
        std::filesystem::remove(transactionPath / "backup",
                                unexpectedBackupError);
    }

    const bool backupRootReady = backend->OpenOrCreateDirectoryNoFollow(
              transactionRoot, "backup", true,
              backupRoot, backupRootIdentity).ok();
    check(backupRootReady,
          "native backup directory is created handle-relative and durable");
    if (!backupRootReady) return;
    const std::filesystem::path backupPath = transactionPath / "backup";
    const std::filesystem::path movedBackupPath = transactionPath / "backup-moved";
    std::error_code backupRenameError;
    std::filesystem::rename(backupPath, movedBackupPath, backupRenameError);
    check(backupRenameError && std::filesystem::exists(backupPath) &&
              !std::filesystem::exists(movedBackupPath),
          "bound backup root denies external rename for the whole session");
    if (!backupRenameError) {
        std::error_code restoreNameError;
        std::filesystem::rename(movedBackupPath, backupPath, restoreNameError);
        check(!restoreNameError,
              "backup rename RED fixture restores its owned sandbox path");
    }

    WindowsFilesystemObservation oldObservation;
    WindowsFilesystemObservation stagingObservation;
    const bool inventoryObserved =
        backend->ObserveNoFollow(installRoot, "HarkenSearch.exe", oldObservation).ok() &&
        oldObservation.kind == WindowsFilesystemEntryKind::RegularFile &&
        backend->ObserveNoFollow(stagingRoot, "HarkenSearch.exe",
                                 stagingObservation).ok() &&
        stagingObservation.kind == WindowsFilesystemEntryKind::RegularFile;
    check(inventoryObserved,
          "native synthetic inventory is observed without following paths");
    if (!inventoryObserved) return;

    WindowsFileIdentity backupIdentity;
    WindowsFileContentFingerprint backupContent;
    WindowsFileMetadataSnapshot rollbackMetadata;
    const WindowsFileContentFingerprint oldContent = fingerprintHex(
        "956360794a2c32fc560b32feea1066c37221a421908bbbd877210fb30972eab4",
        sizeof(kOldBytes) - 1U);
    const WindowsFileContentFingerprint newContent = fingerprintHex(
        "cc738d1c75a1f105f18f5fe4ed9aa8defa4f8ad15fc64cfbefe2e2f0da259da5",
        sizeof(kNewBytes) - 1U);
    const WindowsFilesystemBackendResult backupResult =
        backend->CopyFileDurableNoFollow(
            installRoot, "HarkenSearch.exe", oldObservation.identity,
            backupRoot, "item-0.bak", backupIdentity, backupContent,
            rollbackMetadata);
    checkNative(backupResult,
                "native backup copy publishes exact pinned bytes");
    if (!backupResult.ok()) return;
    check(backupContent == oldContent,
          "native backup fingerprint binds the original bytes");
    WindowsFilesystemObservation backupObservation;
    const bool backupObserved =
        backend->ObserveNoFollow(backupRoot, "item-0.bak",
                                 backupObservation).ok() &&
        backupObservation.kind == WindowsFilesystemEntryKind::RegularFile &&
        backupObservation.metadata.valid;
    check(backupObserved,
          "native backup publication exposes a captured metadata snapshot");
    if (!backupObserved) return;
    FILE_BASIC_INFO backupBasic{};
    check(getNativeBasicInfo(backupPath / "item-0.bak", backupBasic),
          "native backup publication keeps an adapter-owned metadata envelope");
    std::wstring backupSecurity;
    bool backupDaclProtected = false;
    check(nativeSecurityText(backupPath / "item-0.bak", backupSecurity,
                             backupDaclProtected) &&
              (backupDaclProtected != expectedOldDaclProtected ||
               backupSecurity != expectedOldSecurity),
          "physical backup keeps transaction-parent security instead of importing the old target ACL");

    const auto metadataU32 = [](const WindowsFileMetadataSnapshot& value,
                                const std::size_t offset) {
        std::uint32_t result = 0;
        if (offset > value.bytes.size() ||
            value.bytes.size() - offset < 4U) {
            return result;
        }
        for (std::uint32_t index = 0; index < 4U; ++index) {
            result |= static_cast<std::uint32_t>(value.bytes[offset + index])
                << (index * 8U);
        }
        return result;
    };
    const auto setMetadataU32 = [](WindowsFileMetadataSnapshot& value,
                                   const std::size_t offset,
                                   const std::uint32_t replacement) {
        if (offset > value.bytes.size() ||
            value.bytes.size() - offset < 4U) {
            return;
        }
        for (std::uint32_t index = 0; index < 4U; ++index) {
            value.bytes[offset + index] = static_cast<std::uint8_t>(
                replacement >> (index * 8U));
        }
    };
    WindowsFileMetadataSnapshot descriptorTooShort = rollbackMetadata;
    setMetadataU32(descriptorTooShort, 40U, 4U);
    WindowsFileMetadataSnapshot descriptorLengthOverflow = rollbackMetadata;
    setMetadataU32(descriptorLengthOverflow, 40U, UINT32_MAX);
    WindowsFileMetadataSnapshot labelOffsetInvalid = rollbackMetadata;
    setMetadataU32(labelOffsetInvalid, 48U, 4U);
    WindowsFileMetadataSnapshot labelLengthUnaligned = rollbackMetadata;
    const std::size_t labelLengthOffset =
        44U + metadataU32(rollbackMetadata, 40U);
    setMetadataU32(labelLengthUnaligned, labelLengthOffset, 1U);
    const auto assertMalformedMetadataRejected =
        [&](const std::size_t index,
            const char* caseName,
            const WindowsFileMetadataSnapshot& malformed) {
        const std::string temporaryLeaf =
            ".native-malformed-" + std::to_string(index) + ".tmp";
        WindowsFileIdentity malformedIdentity;
        WindowsFileMetadataSnapshot malformedOutputMetadata;
        const WindowsFilesystemBackendResult malformedResult =
            backend->ReplaceFileDurableNoFollow(
                stagingRoot, "HarkenSearch.exe", stagingObservation.identity,
                newContent, {}, installRoot, "HarkenSearch.exe",
                installRootIdentity, temporaryLeaf,
                WindowsFilesystemReplaceMode::ReplaceExisting,
                oldObservation.identity, oldContent,
                malformed, malformedIdentity,
                malformedOutputMetadata);
        WindowsFilesystemObservation after;
        const WindowsFilesystemBackendResult afterObserved =
            backend->ObserveNoFollow(installRoot, "HarkenSearch.exe", after);
        FILE_BASIC_INFO afterBasic{};
        std::wstring afterSecurity;
        bool afterDaclProtected = false;
        const bool metadataUnchanged =
            getNativeBasicInfo(installPath / "HarkenSearch.exe", afterBasic) &&
            sameNativeBasicInfoExceptLastAccess(afterBasic,
                                                expectedOldBasic) &&
            nativeSecurityText(installPath / "HarkenSearch.exe", afterSecurity,
                               afterDaclProtected) &&
            afterDaclProtected == expectedOldDaclProtected &&
            afterSecurity == expectedOldSecurity;
        const bool statusMatches =
            malformedResult.status ==
                WindowsFilesystemBackendStatus::UnsupportedMetadata &&
            malformedResult.nativeError == ERROR_INVALID_SECURITY_DESCR;
        const bool identityMatches =
            afterObserved.ok() && after.identity == oldObservation.identity;
        const bool contentMatches =
            afterObserved.ok() && after.content == oldObservation.content &&
            readNativeFixtureFile(installPath / "HarkenSearch.exe") == kOldBytes;
        const bool metadataMatches =
            afterObserved.ok() && after.metadata.valid && metadataUnchanged;
        const bool namespaceUnchanged =
            !std::filesystem::exists(installPath / temporaryLeaf);
        const bool rejected = statusMatches && identityMatches &&
            contentMatches && metadataMatches && namespaceUnchanged;
        if (!rejected) {
            std::fprintf(
                stderr,
                "FAIL: malformed metadata case=%llu name=%s status=%u native=%lu "
                "before(volume=%llu,file=%u,size=%llu,metadata=%llu) "
                "after(volume=%llu,file=%u,size=%llu,metadata=%llu) "
                "checks(status=%u,identity=%u,content=%u,metadata=%u,namespace=%u)\n",
                static_cast<unsigned long long>(index),
                caseName,
                static_cast<unsigned int>(malformedResult.status),
                static_cast<unsigned long>(malformedResult.nativeError),
                static_cast<unsigned long long>(
                    oldObservation.identity.volumeSerial),
                static_cast<unsigned int>(
                    oldObservation.identity.fileId.front()),
                static_cast<unsigned long long>(
                    oldObservation.content.byteSize),
                static_cast<unsigned long long>(
                    oldObservation.metadata.bytes.size()),
                static_cast<unsigned long long>(after.identity.volumeSerial),
                static_cast<unsigned int>(after.identity.fileId.front()),
                static_cast<unsigned long long>(after.content.byteSize),
                static_cast<unsigned long long>(after.metadata.bytes.size()),
                static_cast<unsigned int>(statusMatches),
                static_cast<unsigned int>(identityMatches),
                static_cast<unsigned int>(contentMatches),
                static_cast<unsigned int>(metadataMatches),
                static_cast<unsigned int>(namespaceUnchanged));
        }
        const std::string assertion =
            "malformed metadata case[" + std::to_string(index) + "] " +
            caseName + " rejects before identity/content/metadata mutation";
        check(rejected, assertion.c_str());
    };
    assertMalformedMetadataRejected(
        0U, "descriptor-too-short", descriptorTooShort);
    assertMalformedMetadataRejected(
        1U, "descriptor-length-overflow", descriptorLengthOverflow);
    assertMalformedMetadataRejected(
        2U, "label-offset-invalid", labelOffsetInvalid);
    assertMalformedMetadataRejected(
        3U, "label-length-unaligned", labelLengthUnaligned);

    WindowsFileIdentity mismatchedSourceIdentity = stagingObservation.identity;
    mismatchedSourceIdentity.fileId.front() ^= UINT8_C(0xff);
    WindowsFileIdentity preflightIdentity;
    WindowsFileMetadataSnapshot preflightOutputMetadata;
    const WindowsFilesystemBackendResult preflightResult =
        backend->ReplaceFileDurableNoFollow(
            stagingRoot, "HarkenSearch.exe", mismatchedSourceIdentity,
            newContent, {}, installRoot, "HarkenSearch.exe",
            installRootIdentity, ".native-malformed-preflight.tmp",
            WindowsFilesystemReplaceMode::ReplaceExisting,
            oldObservation.identity, oldContent, labelOffsetInvalid,
            preflightIdentity, preflightOutputMetadata);
    check(preflightResult.status ==
              WindowsFilesystemBackendStatus::UnsupportedMetadata &&
              readNativeFixtureFile(installPath / "HarkenSearch.exe") == kOldBytes &&
              !std::filesystem::exists(
                  installPath / ".native-malformed-preflight.tmp"),
          "malformed target metadata is rejected before source identity is observed");

    WindowsFileMetadataSnapshot malformedBasic = rollbackMetadata;
    setMetadataU32(malformedBasic, 0U, 0U);
    WindowsFileIdentity malformedBasicIdentity;
    WindowsFileMetadataSnapshot malformedBasicOutputMetadata;
    const WindowsFilesystemBackendResult malformedBasicResult =
        backend->ReplaceFileDurableNoFollow(
            stagingRoot, "HarkenSearch.exe", mismatchedSourceIdentity,
            newContent, {}, installRoot, "HarkenSearch.exe",
            installRootIdentity, ".native-malformed-basic.tmp",
            WindowsFilesystemReplaceMode::ReplaceExisting,
            oldObservation.identity, oldContent, malformedBasic,
            malformedBasicIdentity, malformedBasicOutputMetadata);
    check(malformedBasicResult.status ==
              WindowsFilesystemBackendStatus::UnsupportedMetadata &&
              malformedBasicResult.nativeError == ERROR_NOT_SUPPORTED &&
              readNativeFixtureFile(installPath / "HarkenSearch.exe") == kOldBytes &&
              !std::filesystem::exists(
                  installPath / ".native-malformed-basic.tmp"),
          "malformed basic metadata keeps the generic unsupported error before source observation");

    // A restart-resumed publication must reject malformed prepared metadata
    // before it opens even the source/target names.  The identity/content
    // values are intentionally plausible but do not name a real temporary;
    // accepting them would turn a forged journal value into an I/O probe.
    WindowsFileIdentity forgedPreparedIdentity = identity(0xf1U);
    WindowsFileContentFingerprint forgedPreparedContent = newContent;
    WindowsFileIdentity forgedPreparedOutputIdentity;
    WindowsFileMetadataSnapshot forgedPreparedOutputMetadata;
    const WindowsFilesystemBackendResult malformedPreparedResult =
        backend->ReplaceFileDurableNoFollowWithCheckpoint(
            stagingRoot, "HarkenSearch.exe", stagingObservation.identity, newContent,
            {}, installRoot, "HarkenSearch.exe", installRootIdentity,
            ".native-malformed-prepared.tmp",
            WindowsFilesystemReplaceMode::ReplaceExisting,
            oldObservation.identity, oldContent, rollbackMetadata,
            forgedPreparedIdentity, forgedPreparedContent, descriptorTooShort,
            nullptr, forgedPreparedOutputIdentity,
            forgedPreparedOutputMetadata);
    check(malformedPreparedResult.status ==
              WindowsFilesystemBackendStatus::UnsupportedMetadata &&
              malformedPreparedResult.nativeError ==
                  ERROR_INVALID_SECURITY_DESCR &&
              readNativeFixtureFile(installPath / "HarkenSearch.exe") == kOldBytes &&
              !std::filesystem::exists(
                  installPath / ".native-malformed-prepared.tmp"),
          "malformed prepared metadata is rejected before restart rebind I/O");

    const std::filesystem::path collidingTemporary =
        installPath / ".native-forward.tmp";
    check(writeNativeFixtureFile(collidingTemporary, "unknown-native"),
          "native temporary collision fixture is published independently");
    WindowsFileIdentity collisionIdentity;
    WindowsFileMetadataSnapshot collisionMetadata;
    const WindowsFilesystemBackendResult collisionResult =
        backend->ReplaceFileDurableNoFollow(
            stagingRoot, "HarkenSearch.exe", stagingObservation.identity, newContent,
            {},
            installRoot, "HarkenSearch.exe", installRootIdentity,
            ".native-forward.tmp",
            WindowsFilesystemReplaceMode::ReplaceExisting,
            oldObservation.identity, oldContent, rollbackMetadata,
            collisionIdentity, collisionMetadata);
    check(collisionResult.status ==
              WindowsFilesystemBackendStatus::AlreadyExists &&
              readNativeFixtureFile(collidingTemporary) == "unknown-native" &&
              readNativeFixtureFile(installPath / "HarkenSearch.exe") == kOldBytes,
          "native replacement never overwrites a colliding temporary leaf");
    std::error_code collisionCleanupError;
    const bool collisionRemoved =
        std::filesystem::remove(collidingTemporary, collisionCleanupError);
    check(collisionRemoved && !collisionCleanupError,
          "native temporary collision fixture is removed before the green path");

    WindowsFileIdentity installedIdentity;
    WindowsFileMetadataSnapshot installedMetadata;
    const WindowsFilesystemBackendResult replacementResult =
        retryNativeAccessDenied(
            [&] {
                return backend->ReplaceFileDurableNoFollow(
                    stagingRoot, "HarkenSearch.exe", stagingObservation.identity,
                    newContent, {}, installRoot,
                    "HarkenSearch.exe", installRootIdentity,
                    ".native-forward.tmp",
                    WindowsFilesystemReplaceMode::ReplaceExisting,
                    oldObservation.identity, oldContent, rollbackMetadata,
                    installedIdentity, installedMetadata);
            },
            [&] {
                WindowsFilesystemObservation unchanged;
                return backend->ObserveNoFollow(installRoot, "HarkenSearch.exe",
                                                unchanged).ok() &&
                       unchanged.identity == oldObservation.identity &&
                       !std::filesystem::exists(
                           installPath / ".native-forward.tmp");
            },
            "retryable native replacement denial leaves the target unchanged");
    checkNative(replacementResult,
                "native replacement publishes after exact old/new content binding");
    check(!replacementResult.ok() || installedIdentity.valid,
          "successful native replacement returns the published identity");
    check(!replacementResult.ok() ||
              readNativeFixtureFile(installPath / "HarkenSearch.exe") == kNewBytes,
          "successful native replacement exposes the expected bytes");
    check(!replacementResult.ok() ||
              installedMetadata == rollbackMetadata,
          "successful native replacement preserves the journal-bound target metadata");
    std::wstring publishedSecurity;
    bool publishedDaclProtected = false;
    check(!replacementResult.ok() ||
              (nativeSecurityText(installPath / "HarkenSearch.exe", publishedSecurity,
                                  publishedDaclProtected) &&
               publishedDaclProtected == expectedOldDaclProtected &&
               publishedSecurity == expectedOldSecurity &&
               (publishedDaclProtected != stagingDaclProtected ||
                publishedSecurity != stagingSecurity)),
          "forward replacement preserves target security and never imports staging security");

    WindowsFileIdentity restoredIdentity;
    WindowsFileMetadataSnapshot restoredMetadata;
    if (replacementResult.ok()) {
        const WindowsFilesystemBackendResult restoreResult =
            retryNativeAccessDenied(
                [&] {
                    return backend->ReplaceFileDurableNoFollow(
                        backupRoot, "item-0.bak", backupIdentity,
                        backupContent, rollbackMetadata, installRoot, "HarkenSearch.exe",
                        installRootIdentity, ".native-restore.tmp",
                        WindowsFilesystemReplaceMode::ReplaceExisting,
                        installedIdentity, newContent, installedMetadata,
                        restoredIdentity, restoredMetadata);
                },
                [&] {
                    WindowsFilesystemObservation unchanged;
                    return backend->ObserveNoFollow(installRoot, "HarkenSearch.exe",
                                                    unchanged).ok() &&
                           unchanged.identity == installedIdentity &&
                           !std::filesystem::exists(
                               installPath / ".native-restore.tmp");
                },
                "retryable native restore denial leaves the target unchanged");
        checkNative(restoreResult,
                    "native restore publishes from the exact backup object");
        check(!restoreResult.ok() || restoredIdentity.valid,
              "successful native restore returns the published identity");
        FILE_BASIC_INFO restoredBasic{};
        const bool restoredBasicMatches =
            restoreResult.ok() &&
            getNativeBasicInfo(installPath / "HarkenSearch.exe", restoredBasic) &&
            sameNativeBasicInfoExceptLastAccess(restoredBasic, expectedOldBasic);
        check(!restoreResult.ok() ||
                  restoredBasicMatches,
              "successful native restore exposes the original basic metadata");
        check(!restoreResult.ok() || restoredMetadata == rollbackMetadata,
              "successful native restore returns an exact journal-bound metadata snapshot");
        std::wstring restoredSecurity;
        bool restoredDaclProtected = false;
        check(!restoreResult.ok() ||
                  (nativeSecurityText(installPath / "HarkenSearch.exe",
                                      restoredSecurity,
                                      restoredDaclProtected) &&
                   restoredDaclProtected == expectedOldDaclProtected &&
                   restoredSecurity == expectedOldSecurity),
              "successful native restore exposes the original protected owner/group/DACL metadata");
        check(!restoreResult.ok() ||
                  readNativeFixtureFile(installPath / "HarkenSearch.exe") == kOldBytes,
              "successful native restore exposes the original bytes");
    }

    temporarySecurityProbe.enabled = true;
    WindowsFileIdentity tamperedNewItemIdentity;
    WindowsFileMetadataSnapshot tamperedNewItemMetadata;
    const WindowsFilesystemBackendResult tamperedNewItemResult =
        backend->ReplaceFileDurableNoFollow(
            stagingRoot, "HarkenSearch.exe", stagingObservation.identity, newContent,
            {}, installRoot, "tampered-new-item.exe", installRootIdentity,
            ".native-new-tamper.tmp", WindowsFilesystemReplaceMode::CreateNew,
            {}, {}, {}, tamperedNewItemIdentity, tamperedNewItemMetadata);
    check(temporarySecurityProbe.called && temporarySecurityProbe.changed,
          "native metadata policy seam changes only the temporary destination security");
    check(tamperedNewItemResult.status ==
              WindowsFilesystemBackendStatus::UnsupportedMetadata &&
              !std::filesystem::exists(installPath / "tampered-new-item.exe") &&
              !std::filesystem::exists(installPath / ".native-new-tamper.tmp"),
          "unsafe create-new security inheritance fails before publication and cleans its temporary");
    temporarySecurityProbe.enabled = false;

    check(protectNativeDacl(stagingPath / "HarkenSearch.exe"),
          "native staging fixture receives a distinct protected ACL before create-new");
    std::wstring protectedStagingSecurity;
    bool protectedStagingDacl = false;
    check(nativeSecurityText(stagingPath / "HarkenSearch.exe", protectedStagingSecurity,
                             protectedStagingDacl) && protectedStagingDacl,
          "native staging security mutation is observable");
    const std::filesystem::path inheritanceControl =
        installPath / ".native-inheritance-control.tmp";
    std::wstring inheritedParentSecurity;
    bool inheritedParentDacl = false;
    check(writeNativeFixtureFile(inheritanceControl, "inherit-control") &&
              nativeSecurityText(inheritanceControl, inheritedParentSecurity,
                                 inheritedParentDacl),
          "native install parent default security is captured through a control file");
    std::error_code inheritanceControlError;
    check(std::filesystem::remove(inheritanceControl, inheritanceControlError) &&
              !inheritanceControlError,
          "native inheritance control file is removed before publication");

    WindowsFileIdentity newItemIdentity;
    WindowsFileMetadataSnapshot newItemMetadata;
    const WindowsFilesystemBackendResult newItemResult =
        backend->ReplaceFileDurableNoFollow(
            stagingRoot, "HarkenSearch.exe", stagingObservation.identity, newContent,
            {}, installRoot, "new-item.exe", installRootIdentity,
            ".native-new.tmp", WindowsFilesystemReplaceMode::CreateNew,
            {}, {}, {}, newItemIdentity, newItemMetadata);
    checkNative(newItemResult,
                "native create-new publishes through an exclusive sibling temporary");
    check(!newItemResult.ok() ||
              (newItemIdentity.valid && newItemMetadata.valid &&
               readNativeFixtureFile(installPath / "new-item.exe") ==
                   kNewBytes),
          "native create-new returns its published identity and inherited metadata");
    std::wstring newItemSecurity;
    bool newItemDaclProtected = false;
    check(!newItemResult.ok() ||
              (nativeSecurityText(installPath / "new-item.exe", newItemSecurity,
                                  newItemDaclProtected) &&
               newItemSecurity == inheritedParentSecurity &&
               newItemDaclProtected == inheritedParentDacl &&
               (newItemDaclProtected != protectedStagingDacl ||
                newItemSecurity != protectedStagingSecurity)),
          "create-new inherits install-parent security and never imports staging ACL");
    check(!newItemResult.ok() ||
              backend->DeleteFileDurableNoFollow(
                  installRoot, "new-item.exe", installRootIdentity,
                  newItemIdentity, newContent, newItemMetadata).ok(),
          "native rollback cleanup deletes only the exact journaled new file");

    const std::filesystem::path backupItemPath = backupPath / "item-0.bak";
    HANDLE heldBackupReader = CreateFileW(
        backupItemPath.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    check(heldBackupReader != INVALID_HANDLE_VALUE,
          "native durable-delete fixture holds a share-delete reader");
    check(backend->DeleteFileDurableNoFollow(
              backupRoot, "item-0.bak", backupRootIdentity,
              backupIdentity, backupContent, {}).ok(),
          "native backup cleanup deletes only the recorded object");
    HANDLE reusedBackupName = CreateFileW(
        backupItemPath.c_str(), GENERIC_WRITE | DELETE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE,
        nullptr);
    check(reusedBackupName != INVALID_HANDLE_VALUE,
          "successful durable delete immediately frees the namespace while a prior reader lives");
    if (reusedBackupName != INVALID_HANDLE_VALUE) CloseHandle(reusedBackupName);
    if (heldBackupReader != INVALID_HANDLE_VALUE) CloseHandle(heldBackupReader);
    backend->CloseFilesystemHandle(backupRoot);
    backupRoot = 0;
    check(backend->RemoveEmptyDirectoryDurableNoFollow(
              transactionRoot, "backup", backupRootIdentity).ok(),
          "native backup directory removal succeeds after handle close");

    backend->ReleaseJournalMutationLease(journalLease);

    backend->CloseFilesystemHandle(transactionRoot);
    backend->CloseFilesystemHandle(stagingRoot);
    backend->CloseFilesystemHandle(installRoot);
#endif
}

void test_native_recovery_allows_observed_change_time_only() {
#if defined(_WIN32)
    NativeSandbox sandbox;
    NativeSandboxCleanupAssertion cleanup(sandbox, "recovery-observed-change-time");
    const std::filesystem::path base = NativeSandbox::systemTempRoot();
    for (std::uint32_t attempt = 0; attempt < 32U && !sandbox.owned;
         ++attempt) {
        sandbox.path = base /
            ("aris-bootstrap-recovery-change-time-" +
             std::to_string(GetCurrentProcessId()) + "-" +
             std::to_string(attempt));
        std::error_code error;
        sandbox.owned = std::filesystem::create_directory(sandbox.path, error);
    }
    check(sandbox.owned, "recovery change-time fixture owns its root");
    if (!sandbox.owned) return;
    const auto target = sandbox.path / "HarkenSearch.exe";
    check(writeNativeFixtureFile(target, "same-signed-bytes"),
          "recovery change-time fixture creates a file");

    auto backend = CreateNativeWindowsBootstrapFilesystemBackend();
    WindowsFilesystemHandle root = 0;
    WindowsFileIdentity identity;
    const auto opened = backend->OpenRootExact(
        sandbox.path.u8string(), WindowsFilesystemRootRole::Install,
        root, identity);
    WindowsFilesystemObservation observed;
    const auto read = opened.ok()
        ? backend->ObserveNoFollow(root, "HarkenSearch.exe", observed)
        : opened;
    check(opened.ok() && read.ok() && observed.metadata.valid &&
              observed.metadata.bytes.size() > 36U,
          "recovery change-time fixture captures real native metadata");
    if (!read.ok() || !observed.metadata.valid ||
        observed.metadata.bytes.size() <= 36U) return;

    auto accessedAndChanged = observed.metadata;
    accessedAndChanged.bytes[12] ^= 1U; // LastAccessTime
    accessedAndChanged.bytes[28] ^= 1U; // ChangeTime
    check(backend->MetadataMatchesForRecovery(accessedAndChanged,
                                               observed.metadata),
          "recovery accepts access and change time drift after execution");

    auto writeChanged = observed.metadata;
    writeChanged.bytes[20] ^= 1U; // LastWriteTime
    check(!backend->MetadataMatchesForRecovery(writeChanged,
                                                observed.metadata),
          "recovery still rejects write-time drift");
    backend->CloseFilesystemHandle(root);
#endif
}

void test_native_replacement_journals_observed_post_publish_change_time() {
#if defined(_WIN32)
    NativeSandbox sandbox;
    NativeSandboxCleanupAssertion cleanup(sandbox, "post-publish-change-time");
    const std::filesystem::path base = NativeSandbox::systemTempRoot();
    for (std::uint32_t attempt = 0; attempt < 32U && !sandbox.owned;
         ++attempt) {
        sandbox.path = base /
            ("aris-bootstrap-post-publish-change-time-" +
             std::to_string(GetCurrentProcessId()) + "-" +
             std::to_string(attempt));
        std::error_code createError;
        sandbox.owned = std::filesystem::create_directory(sandbox.path,
                                                          createError);
    }
    check(sandbox.owned,
          "post-publish change-time sandbox is exclusively created");
    if (!sandbox.owned) return;

    const auto installPath = sandbox.path / "install";
    const auto updaterPath = sandbox.path / "updater";
    const auto stagingPath = updaterPath / "staging";
    const auto transactionPath = updaterPath / "transaction";
    std::error_code error;
    const bool directoriesCreated =
        std::filesystem::create_directory(installPath, error) &&
        std::filesystem::create_directory(updaterPath, error) &&
        std::filesystem::create_directory(stagingPath, error) &&
        std::filesystem::create_directory(transactionPath, error);
    check(directoriesCreated,
          "post-publish change-time fixture creates exact roots");
    if (!directoriesCreated) return;

    check(writeNativeFixtureFile(installPath / "HarkenSearch.exe", "old-native") &&
              writeNativeFixtureFile(stagingPath / "HarkenSearch.exe", "new-native"),
          "post-publish change-time fixture creates distinct target bytes");

    NativePublishedChangeTimeProbe probe;
    WindowsBootstrapFilesystemNativeTestHooks hooks;
    hooks.context = &probe;
    hooks.afterAtomicPublish = &normalizeNativePublishedChangeTime;
    auto backend = createAuthorizedNativeBackend(
        installPath, stagingPath, transactionPath, hooks);
    check(backend != nullptr,
          "post-publish change-time backend is authorized");
    if (backend == nullptr) return;

    WindowsFilesystemHandle installRoot = 0;
    WindowsFilesystemHandle stagingRoot = 0;
    WindowsFilesystemHandle transactionRoot = 0;
    WindowsFileIdentity installRootIdentity;
    WindowsFileIdentity stagingRootIdentity;
    WindowsFileIdentity transactionRootIdentity;
    const bool rootsBound =
        backend->OpenRootExact(installPath.u8string(),
                               WindowsFilesystemRootRole::Install,
                               installRoot, installRootIdentity).ok() &&
        backend->OpenRootExact(stagingPath.u8string(),
                               WindowsFilesystemRootRole::Staging,
                               stagingRoot, stagingRootIdentity).ok() &&
        backend->OpenRootExact(transactionPath.u8string(),
                               WindowsFilesystemRootRole::Transaction,
                               transactionRoot,
                               transactionRootIdentity).ok();
    check(rootsBound,
          "post-publish change-time fixture binds exact roots");
    if (!rootsBound) return;

    WindowsFileIdentity journalIdentity;
    WindowsFileContentFingerprint journalContent;
    const auto journalPersisted = backend->PersistJournalDurable(
        transactionRoot, {9U, 1U, 9U},
        WindowsFilesystemReplaceMode::CreateNew, {}, {},
        journalIdentity, journalContent);
    WindowsFilesystemHandle journalLease = 0;
    const auto journalLeased = journalPersisted.ok()
        ? backend->AcquireJournalMutationLease(
              transactionRoot, journalIdentity, journalContent,
              journalLease)
        : journalPersisted;
    check(journalPersisted.ok() && journalLeased.ok(),
          "post-publish change-time fixture holds the journal lease");
    if (!journalPersisted.ok() || !journalLeased.ok()) return;

    WindowsFilesystemObservation oldTarget;
    WindowsFilesystemObservation stagedSource;
    const bool observed =
        backend->ObserveNoFollow(installRoot, "HarkenSearch.exe", oldTarget).ok() &&
        backend->ObserveNoFollow(stagingRoot, "HarkenSearch.exe", stagedSource).ok() &&
        oldTarget.kind == WindowsFilesystemEntryKind::RegularFile &&
        stagedSource.kind == WindowsFilesystemEntryKind::RegularFile &&
        oldTarget.metadata.valid && stagedSource.content.valid;
    check(observed,
          "post-publish change-time fixture records target metadata and source content");

    WindowsFileIdentity installedIdentity;
    WindowsFileMetadataSnapshot installedMetadata;
    const WindowsFilesystemBackendResult replaced = observed
        ? backend->ReplaceFileDurableNoFollow(
              stagingRoot, "HarkenSearch.exe", stagedSource.identity,
              stagedSource.content, {}, installRoot, "HarkenSearch.exe",
              installRootIdentity, ".post-publish-change-time.tmp",
              WindowsFilesystemReplaceMode::ReplaceExisting,
              oldTarget.identity, oldTarget.content, oldTarget.metadata,
              installedIdentity, installedMetadata)
        : WindowsFilesystemBackendResult{
              WindowsFilesystemBackendStatus::InvalidPath,
              ERROR_INVALID_STATE};

    check(probe.called && probe.changed,
          "post-publish seam changes only the published ChangeTime");
    checkNative(replaced,
                "replacement accepts Windows post-publish ChangeTime normalization");
    WindowsFilesystemObservation published;
    const bool publishedObserved = replaced.ok() &&
        backend->ObserveNoFollow(installRoot, "HarkenSearch.exe", published).ok();
    check(publishedObserved && published.identity == installedIdentity &&
              published.content == stagedSource.content &&
              published.metadata == installedMetadata,
          "replacement journals the exact observed post-publish metadata");

    backend->ReleaseJournalMutationLease(journalLease);
    backend->CloseFilesystemHandle(transactionRoot);
    backend->CloseFilesystemHandle(stagingRoot);
    backend->CloseFilesystemHandle(installRoot);
#endif
}

void test_native_metadata_apply_accepts_windows_change_time_normalization() {
#if defined(_WIN32)
    NativeSandbox sandbox;
    NativeSandboxCleanupAssertion cleanup(sandbox, "metadata-apply-change-time");
    const std::filesystem::path base = NativeSandbox::systemTempRoot();
    for (std::uint32_t attempt = 0; attempt < 32U && !sandbox.owned;
         ++attempt) {
        sandbox.path = base /
            ("aris-bootstrap-metadata-apply-change-time-" +
             std::to_string(GetCurrentProcessId()) + "-" +
             std::to_string(attempt));
        std::error_code createError;
        sandbox.owned = std::filesystem::create_directory(sandbox.path,
                                                          createError);
    }
    check(sandbox.owned,
          "metadata-apply change-time sandbox is exclusively created");
    if (!sandbox.owned) return;

    const auto installPath = sandbox.path / "install";
    const auto updaterPath = sandbox.path / "updater";
    const auto stagingPath = updaterPath / "staging";
    const auto transactionPath = updaterPath / "transaction";
    std::error_code error;
    const bool directoriesCreated =
        std::filesystem::create_directory(installPath, error) &&
        std::filesystem::create_directory(updaterPath, error) &&
        std::filesystem::create_directory(stagingPath, error) &&
        std::filesystem::create_directory(transactionPath, error);
    check(directoriesCreated,
          "metadata-apply change-time fixture creates exact roots");
    if (!directoriesCreated) return;

    check(writeNativeFixtureFile(installPath / "Auro.exe", "old-alias") &&
              writeNativeFixtureFile(stagingPath / "Auro.exe", "new-alias"),
          "metadata-apply change-time fixture creates distinct alias bytes");

    NativeAppliedChangeTimeProbe probe;
    WindowsBootstrapFilesystemNativeTestHooks hooks;
    hooks.context = &probe;
    hooks.afterMetadataApply = &normalizeNativeAppliedChangeTime;
    auto backend = createAuthorizedNativeBackend(
        installPath, stagingPath, transactionPath, hooks);
    check(backend != nullptr,
          "metadata-apply change-time backend is authorized");
    if (backend == nullptr) return;

    WindowsFilesystemHandle installRoot = 0;
    WindowsFilesystemHandle stagingRoot = 0;
    WindowsFilesystemHandle transactionRoot = 0;
    WindowsFileIdentity installRootIdentity;
    WindowsFileIdentity stagingRootIdentity;
    WindowsFileIdentity transactionRootIdentity;
    const bool rootsBound =
        backend->OpenRootExact(installPath.u8string(),
                               WindowsFilesystemRootRole::Install,
                               installRoot, installRootIdentity).ok() &&
        backend->OpenRootExact(stagingPath.u8string(),
                               WindowsFilesystemRootRole::Staging,
                               stagingRoot, stagingRootIdentity).ok() &&
        backend->OpenRootExact(transactionPath.u8string(),
                               WindowsFilesystemRootRole::Transaction,
                               transactionRoot,
                               transactionRootIdentity).ok();
    check(rootsBound,
          "metadata-apply change-time fixture binds exact roots");
    if (!rootsBound) return;

    WindowsFileIdentity journalIdentity;
    WindowsFileContentFingerprint journalContent;
    const auto journalPersisted = backend->PersistJournalDurable(
        transactionRoot, {9U, 1U, 9U},
        WindowsFilesystemReplaceMode::CreateNew, {}, {},
        journalIdentity, journalContent);
    WindowsFilesystemHandle journalLease = 0;
    const auto journalLeased = journalPersisted.ok()
        ? backend->AcquireJournalMutationLease(
              transactionRoot, journalIdentity, journalContent,
              journalLease)
        : journalPersisted;
    check(journalPersisted.ok() && journalLeased.ok(),
          "metadata-apply change-time fixture holds the journal lease");
    if (!journalPersisted.ok() || !journalLeased.ok()) return;

    WindowsFilesystemObservation oldTarget;
    WindowsFilesystemObservation stagedSource;
    const bool observed =
        backend->ObserveNoFollow(installRoot, "Auro.exe", oldTarget).ok() &&
        backend->ObserveNoFollow(stagingRoot, "Auro.exe", stagedSource).ok() &&
        oldTarget.kind == WindowsFilesystemEntryKind::RegularFile &&
        stagedSource.kind == WindowsFilesystemEntryKind::RegularFile &&
        oldTarget.metadata.valid && stagedSource.content.valid;
    check(observed,
          "metadata-apply change-time fixture records alias metadata and content");

    WindowsFileIdentity installedIdentity;
    WindowsFileMetadataSnapshot installedMetadata;
    const WindowsFilesystemBackendResult replaced = observed
        ? backend->ReplaceFileDurableNoFollow(
              stagingRoot, "Auro.exe", stagedSource.identity,
              stagedSource.content, {}, installRoot, "Auro.exe",
              installRootIdentity, ".metadata-apply-change-time.tmp",
              WindowsFilesystemReplaceMode::ReplaceExisting,
              oldTarget.identity, oldTarget.content, oldTarget.metadata,
              installedIdentity, installedMetadata)
        : WindowsFilesystemBackendResult{
              WindowsFilesystemBackendStatus::InvalidPath,
              ERROR_INVALID_STATE};

    check(probe.called && probe.changed,
          "metadata-apply seam changes only the temporary ChangeTime");
    checkNative(replaced,
                "metadata apply accepts Windows ChangeTime normalization");

    backend->ReleaseJournalMutationLease(journalLease);
    backend->CloseFilesystemHandle(transactionRoot);
    backend->CloseFilesystemHandle(stagingRoot);
    backend->CloseFilesystemHandle(installRoot);
#endif
}

void test_native_target_swap_after_prepared_checkpoint_is_rejected() {
#if defined(_WIN32)
    NativeSandbox sandbox;
    NativeSandboxCleanupAssertion cleanup(sandbox, "checkpoint-target-swap");
    const std::filesystem::path base = NativeSandbox::systemTempRoot();
    for (std::uint32_t attempt = 0; attempt < 32U && !sandbox.owned;
         ++attempt) {
        sandbox.path = base /
            ("aris-bootstrap-checkpoint-target-swap-" +
             std::to_string(GetCurrentProcessId()) + "-" +
             std::to_string(attempt));
        std::error_code createError;
        sandbox.owned = std::filesystem::create_directory(sandbox.path,
                                                          createError);
    }
    check(sandbox.owned,
          "native checkpoint target-swap sandbox is exclusively created");
    if (!sandbox.owned) return;

    const std::filesystem::path installPath = sandbox.path / "install";
    const std::filesystem::path updaterPath = sandbox.path / "updater";
    const std::filesystem::path stagingPath = updaterPath / "staging";
    const std::filesystem::path transactionPath = updaterPath / "transaction";
    std::error_code directoryError;
    const bool fixtureCreated =
        std::filesystem::create_directory(installPath, directoryError) &&
        std::filesystem::create_directory(updaterPath, directoryError) &&
        std::filesystem::create_directory(stagingPath, directoryError) &&
        std::filesystem::create_directory(transactionPath, directoryError) &&
        writeNativeFixtureFile(installPath / "HarkenSearch.exe", "old-native") &&
        writeNativeFixtureFile(stagingPath / "HarkenSearch.exe", "new-native");
    check(fixtureCreated,
          "native checkpoint target-swap fixture creates exact roots and files");
    if (!fixtureCreated) return;

    auto backend = createAuthorizedNativeBackend(
        installPath, stagingPath, transactionPath);
    check(backend != nullptr,
          "native checkpoint target-swap backend is authorized");
    if (backend == nullptr) return;

    WindowsFilesystemHandle installRoot = 0;
    WindowsFilesystemHandle stagingRoot = 0;
    WindowsFilesystemHandle transactionRoot = 0;
    WindowsFileIdentity installIdentity;
    WindowsFileIdentity stagingIdentity;
    WindowsFileIdentity transactionIdentity;
    const bool rootsBound =
        backend->OpenRootExact(installPath.u8string(),
                               WindowsFilesystemRootRole::Install,
                               installRoot, installIdentity).ok() &&
        backend->OpenRootExact(stagingPath.u8string(),
                               WindowsFilesystemRootRole::Staging,
                               stagingRoot, stagingIdentity).ok() &&
        backend->OpenRootExact(transactionPath.u8string(),
                               WindowsFilesystemRootRole::Transaction,
                               transactionRoot, transactionIdentity).ok();
    check(rootsBound,
          "native checkpoint target-swap fixture binds exact roots");
    if (!rootsBound) return;

    WindowsFilesystemObservation target;
    WindowsFilesystemObservation source;
    const bool observed =
        backend->ObserveNoFollow(installRoot, "HarkenSearch.exe", target).ok() &&
        backend->ObserveNoFollow(stagingRoot, "HarkenSearch.exe", source).ok() &&
        target.kind == WindowsFilesystemEntryKind::RegularFile &&
        source.kind == WindowsFilesystemEntryKind::RegularFile &&
        target.identity.valid && target.content.valid &&
        source.identity.valid && source.content.valid;
    check(observed,
          "native checkpoint target-swap fixture observes source and target authority");
    if (!observed) return;

    WindowsFileIdentity journalIdentity;
    WindowsFileContentFingerprint journalContent;
    const auto journalPersisted = backend->PersistJournalDurable(
        transactionRoot, {1U, 4U, 9U}, WindowsFilesystemReplaceMode::CreateNew,
        {}, {}, journalIdentity, journalContent);
    WindowsFilesystemHandle journalLease = 0;
    const auto journalLeased = journalPersisted.ok()
        ? backend->AcquireJournalMutationLease(
              transactionRoot, journalIdentity, journalContent, journalLease)
        : journalPersisted;
    check(journalPersisted.ok() && journalLeased.ok(),
          "native checkpoint target-swap fixture holds the journal lease");
    if (!journalPersisted.ok() || !journalLeased.ok()) return;

    NativeCheckpointTargetSwapProbe probe;
    probe.target = installPath / "HarkenSearch.exe";
    probe.moved = installPath / "HarkenSearch.exe.moved-by-race";
    const WindowsFilesystemPublicationCheckpoint checkpoint{
        &probe, &swapNativeTargetAfterPreparedCheckpoint};
    WindowsFileIdentity publishedIdentity;
    WindowsFileMetadataSnapshot publishedMetadata;
    const auto replaced = backend->ReplaceFileDurableNoFollowWithCheckpoint(
        stagingRoot, "HarkenSearch.exe", source.identity, source.content, {},
        installRoot, "HarkenSearch.exe", installIdentity, ".checkpoint-race.tmp",
        WindowsFilesystemReplaceMode::ReplaceExisting, target.identity,
        target.content, target.metadata, {}, {}, {}, &checkpoint,
        publishedIdentity, publishedMetadata);
    check(probe.called && probe.swapped,
          "native checkpoint target-swap seam replaces the target after prepared checkpoint");
    check(replaced.status == WindowsFilesystemBackendStatus::IdentityMismatch,
          "target FileId replacement after checkpoint fails closed before publication");
    check(readNativeFixtureFile(probe.target) == "attacker-native" &&
              std::filesystem::exists(probe.moved) &&
              !std::filesystem::exists(installPath / ".checkpoint-race.tmp"),
          "checkpoint target-swap rejection preserves the unknown target and removes no temporary residue");

    backend->ReleaseJournalMutationLease(journalLease);
    backend->CloseFilesystemHandle(transactionRoot);
    backend->CloseFilesystemHandle(stagingRoot);
    backend->CloseFilesystemHandle(installRoot);
#endif
}

void test_native_port_retargets_the_journal_lease_during_prepared_checkpoint() {
#if defined(_WIN32)
    NativeSandbox sandbox;
    NativeSandboxCleanupAssertion cleanup(sandbox, "port-checkpoint");
    const std::filesystem::path base = NativeSandbox::systemTempRoot();
    for (std::uint32_t attempt = 0; attempt < 32 && !sandbox.owned; ++attempt) {
        sandbox.path = base /
            ("port-checkpoint-" + std::to_string(GetCurrentProcessId()) +
             "-" + std::to_string(attempt));
        std::error_code createError;
        sandbox.owned = std::filesystem::create_directory(sandbox.path,
                                                          createError);
    }
    check(sandbox.owned,
          "native port-checkpoint sandbox is exclusively created");
    if (!sandbox.owned) return;

    const auto installPath = sandbox.path / "install";
    const auto updaterPath = sandbox.path / "updater";
    const auto stagingPath = updaterPath / "staging";
    const auto transactionPath = updaterPath / "transaction";
    std::error_code directoryError;
    const bool fixtureCreated =
        std::filesystem::create_directory(installPath, directoryError) &&
        std::filesystem::create_directory(updaterPath, directoryError) &&
        std::filesystem::create_directory(stagingPath, directoryError) &&
        std::filesystem::create_directory(transactionPath, directoryError) &&
        writeNativeFixtureFile(stagingPath / "HarkenSearch.exe", "new-native");
    check(fixtureCreated,
          "native port-checkpoint fixture creates trusted sibling roots");
    if (!fixtureCreated) return;

    const Fixture values = crashFixture(
        installPath.u8string(), stagingPath.u8string(),
        transactionPath.u8string());
    WindowsBootstrapFilesystemTrustedRootPolicy policy;
    policy.updaterOwnedBase = updaterPath.u8string();
    policy.invocationIdentity = "signed-invocation-01";
    policy.stagingRoot = stagingPath.u8string();
    policy.transactionRoot = transactionPath.u8string();
    policy.authenticated = true;
    {
        auto backend = createAuthorizedNativeBackend(
            installPath, stagingPath, transactionPath);
        check(backend != nullptr,
              "native port-checkpoint backend is available");
        if (backend == nullptr) return;
        WindowsBootstrapFilesystemPort port(*backend);
        const WindowsBootstrapFilesystemRoots trusted{
            stagingPath.u8string(), transactionPath.u8string(),
            policy.updaterOwnedBase, policy.invocationIdentity};
        const auto begun = port.Begin(values.plan, values.journal, trusted);
        const auto journaled = begun.ok()
            ? port.Apply(intent(BootstrapIntentKind::PersistJournal, {},
                                "prepared"))
            : WindowsBootstrapFilesystemResult{
                  WindowsBootstrapFilesystemStatus::BackendFailure};
        const auto replaced = journaled.ok()
            ? port.Apply(intent(BootstrapIntentKind::ReplaceInventoryItem,
                                "HarkenSearch.exe"))
            : WindowsBootstrapFilesystemResult{
                  WindowsBootstrapFilesystemStatus::BackendFailure};
        check(begun.ok() && journaled.ok() && replaced.ok(),
              "native port replacement persists its prepared checkpoint while the mutation lease stays authoritative");
        check(replaced.ok() &&
                  readNativeFixtureFile(installPath / "HarkenSearch.exe") ==
                      "new-native",
              "native port checkpoint publishes the signed payload exactly once");
    }
#endif
}

void test_native_restart_reopens_journal_temp_from_separate_process() {
#if defined(_WIN32)
    const auto runCase = [](const bool beforeDirectoryFlush) {
        NativeSandbox sandbox;
        NativeSandboxCleanupAssertion cleanup(
            sandbox,
            beforeDirectoryFlush ? "journal-file-flush-child"
                                  : "journal-directory-flush-child");
        const auto base = NativeSandbox::systemTempRoot();
        for (std::uint32_t attempt = 0; attempt < 32 && !sandbox.owned;
             ++attempt) {
            sandbox.path = base /
                ("journal-child-" + std::to_string(GetCurrentProcessId()) +
                 (beforeDirectoryFlush ? "-file-flush-" : "-directory-flush-") +
                 std::to_string(attempt));
            std::error_code createError;
            sandbox.owned = std::filesystem::create_directory(sandbox.path,
                                                              createError);
        }
        check(sandbox.owned,
              "separate-process journal sandbox is exclusively created");
        if (!sandbox.owned) return;
        const auto installPath = sandbox.path / "install";
        const auto updaterPath = sandbox.path / "updater";
        const auto stagingPath = updaterPath / "staging";
        const auto transactionPath = updaterPath / "transaction";
        std::error_code directoryError;
        const bool fixtureCreated =
            std::filesystem::create_directory(installPath, directoryError) &&
            std::filesystem::create_directory(updaterPath, directoryError) &&
            std::filesystem::create_directory(stagingPath, directoryError) &&
            std::filesystem::create_directory(transactionPath, directoryError) &&
            writeNativeFixtureFile(stagingPath / "HarkenSearch.exe", "new-native");
        check(fixtureCreated,
              "separate-process journal roots and payload are created");
        if (!fixtureCreated) return;
        wchar_t executableBuffer[MAX_PATH]{};
        const DWORD executableLength = GetModuleFileNameW(
            nullptr, executableBuffer,
            static_cast<DWORD>(std::size(executableBuffer)));
        check(executableLength != 0U &&
                  executableLength < std::size(executableBuffer),
              "separate-process fixture resolves its own executable");
        if (executableLength == 0U ||
            executableLength >= std::size(executableBuffer)) return;
        DWORD childExit = STILL_ACTIVE;
        check(launchJournalTempCrashChild(
                  std::filesystem::path(executableBuffer), installPath,
                  stagingPath, transactionPath, childExit,
                  beforeDirectoryFlush) &&
                  childExit == kCrashCutExitCode,
              beforeDirectoryFlush
                  ? "child is forcibly terminated after journal file flush and before directory flush"
                  : "child is forcibly terminated after the journal directory-flush crash cut");
        auto backend = createAuthorizedNativeBackend(
            installPath, stagingPath, transactionPath);
        check(backend != nullptr,
              "fresh parent backend is available after child exit");
        if (backend == nullptr) return;
        const Fixture values = crashFixture(
            installPath.u8string(), stagingPath.u8string(),
            transactionPath.u8string());
        WindowsBootstrapFilesystemPort recovered(*backend);
        const WindowsBootstrapFilesystemRoots trusted{
            stagingPath.u8string(), transactionPath.u8string(),
            updaterPath.u8string(), "signed-invocation-01"};
        const auto recovery = recovered.BeginRecovery(
            values.plan, values.journal, trusted);
        check(recovery.filesystem.ok() &&
                  recovery.disposition ==
                      WindowsBootstrapRecoveryDisposition::ContinueForward &&
                  recovery.generation == 1U &&
                  containsIntent(recovery.intents,
                                 BootstrapIntentKind::ReplaceInventoryItem,
                                 "HarkenSearch.exe") &&
                  std::filesystem::exists(
                      transactionPath / "aris-bootstrap.journal.a") &&
                  !std::filesystem::exists(
                      transactionPath / "aris-bootstrap.journal.a.tmp"),
              beforeDirectoryFlush
                  ? "fresh BeginRecovery rebinds a journal temp durable after file flush but before directory flush"
                  : "fresh BeginRecovery decodes, rebinds, promotes, and emits replay intents for the durable temp generation");
    };
    runCase(false);
    runCase(true);
#endif
}

void test_native_restart_observes_atomic_target_from_separate_process() {
#if defined(_WIN32)
    NativeSandbox sandbox;
    NativeSandboxCleanupAssertion cleanup(sandbox, "target-child");
    const auto base = NativeSandbox::systemTempRoot();
    for (std::uint32_t attempt = 0; attempt < 32 && !sandbox.owned; ++attempt) {
        sandbox.path = base /
            ("target-child-" + std::to_string(GetCurrentProcessId()) +
             "-" + std::to_string(attempt));
        std::error_code createError;
        sandbox.owned = std::filesystem::create_directory(sandbox.path,
                                                          createError);
    }
    check(sandbox.owned, "separate-process target sandbox is exclusively created");
    if (!sandbox.owned) return;
    const auto installPath = sandbox.path / "install";
    const auto updaterPath = sandbox.path / "updater";
    const auto stagingPath = updaterPath / "staging";
    const auto transactionPath = updaterPath / "transaction";
    std::error_code directoryError;
    const bool fixtureCreated =
        std::filesystem::create_directory(installPath, directoryError) &&
        std::filesystem::create_directory(updaterPath, directoryError) &&
        std::filesystem::create_directory(stagingPath, directoryError) &&
        std::filesystem::create_directory(transactionPath, directoryError) &&
        writeNativeFixtureFile(installPath / "HarkenSearch.exe", "old-native") &&
        writeNativeFixtureFile(stagingPath / "HarkenSearch.exe", "new-native");
    check(fixtureCreated, "separate-process target roots and files are created");
    if (!fixtureCreated) return;
    wchar_t executableBuffer[MAX_PATH]{};
    const DWORD executableLength = GetModuleFileNameW(
        nullptr, executableBuffer, static_cast<DWORD>(std::size(executableBuffer)));
    check(executableLength != 0U &&
              executableLength < std::size(executableBuffer),
          "target crash fixture resolves its own executable");
    if (executableLength == 0U ||
        executableLength >= std::size(executableBuffer)) return;
    DWORD childExit = STILL_ACTIVE;
    check(launchTargetAtomicCrashChild(
              std::filesystem::path(executableBuffer), installPath,
              stagingPath, transactionPath, childExit) &&
              childExit == kCrashCutExitCode,
          "child is forcibly terminated after atomic target publication and before directory flush");
    auto backend = createAuthorizedNativeBackend(
        installPath, stagingPath, transactionPath);
    check(backend != nullptr, "fresh target-recovery backend is available");
    if (backend == nullptr) return;
    const Fixture values = replacementCrashFixture(
        installPath.u8string(), stagingPath.u8string(),
        transactionPath.u8string());
    const WindowsBootstrapFilesystemRoots trusted{
        stagingPath.u8string(), transactionPath.u8string(),
        updaterPath.u8string(), "signed-invocation-01"};
    WindowsBootstrapFilesystemPort recovered(*backend);
    const auto recovery = recovered.BeginRecovery(
        values.plan, values.journal, trusted);
    check(recovery.filesystem.ok() &&
              recovery.disposition ==
                  WindowsBootstrapRecoveryDisposition::Rollback &&
              containsIntent(recovery.intents,
                             BootstrapIntentKind::RestoreBackup,
                             "HarkenSearch.exe"),
          "fresh process reopens the atomic-publish cut and chooses journal-bound rollback");
    if (!recovery.filesystem.ok()) return;
    bool rollbackApplied = true;
    for (const BootstrapIntent& recoveryIntent : recovery.intents) {
        rollbackApplied = recovered.Apply(recoveryIntent).ok() &&
                          rollbackApplied;
    }
    check(rollbackApplied &&
              readNativeFixtureFile(installPath / "HarkenSearch.exe") ==
                  "old-native" &&
              !std::filesystem::exists(installPath / ".target-crash.tmp"),
          "fresh process restores the exact pre-publish target after the atomic crash cut");
#endif
}

void test_native_restart_resumes_prepared_target_from_separate_process() {
#if defined(_WIN32)
    NativeSandbox sandbox;
    NativeSandboxCleanupAssertion cleanup(sandbox, "prepared-target-child");
    const auto base = NativeSandbox::systemTempRoot();
    for (std::uint32_t attempt = 0; attempt < 32U && !sandbox.owned;
         ++attempt) {
        sandbox.path = base /
            ("prepared-target-child-" +
             std::to_string(GetCurrentProcessId()) + "-" +
             std::to_string(attempt));
        std::error_code createError;
        sandbox.owned = std::filesystem::create_directory(sandbox.path,
                                                          createError);
    }
    check(sandbox.owned,
          "prepared-target sandbox is exclusively created");
    if (!sandbox.owned) return;
    const auto installPath = sandbox.path / "install";
    const auto updaterPath = sandbox.path / "updater";
    const auto stagingPath = updaterPath / "staging";
    const auto transactionPath = updaterPath / "transaction";
    std::error_code directoryError;
    const bool fixtureCreated =
        std::filesystem::create_directory(installPath, directoryError) &&
        std::filesystem::create_directory(updaterPath, directoryError) &&
        std::filesystem::create_directory(stagingPath, directoryError) &&
        std::filesystem::create_directory(transactionPath, directoryError) &&
        writeNativeFixtureFile(stagingPath / "HarkenSearch.exe", "new-native");
    check(fixtureCreated,
          "prepared-target roots and signed payload are created");
    if (!fixtureCreated) return;
    wchar_t executableBuffer[MAX_PATH]{};
    const DWORD executableLength = GetModuleFileNameW(
        nullptr, executableBuffer,
        static_cast<DWORD>(std::size(executableBuffer)));
    check(executableLength != 0U &&
              executableLength < std::size(executableBuffer),
          "prepared-target fixture resolves its own executable");
    if (executableLength == 0U ||
        executableLength >= std::size(executableBuffer)) {
        return;
    }
    DWORD childExit = STILL_ACTIVE;
    check(launchTargetPreparedCrashChild(
              std::filesystem::path(executableBuffer), installPath,
              stagingPath, transactionPath, childExit) &&
              childExit == kCrashCutExitCode,
          "child is forcibly terminated after durable prepared checkpoint and before target publication");
    if (childExit != kCrashCutExitCode) return;

    WindowsBootstrapFilesystemTrustedRootPolicy policy;
    policy.updaterOwnedBase = updaterPath.u8string();
    policy.invocationIdentity = "signed-invocation-01";
    policy.stagingRoot = stagingPath.u8string();
    policy.transactionRoot = transactionPath.u8string();
    policy.authenticated = true;
    auto backend = createAuthorizedNativeBackend(
        installPath, stagingPath, transactionPath);
    check(backend != nullptr,
          "fresh prepared-target recovery backend is available");
    if (backend == nullptr) return;
    const Fixture values = crashFixture(
        installPath.u8string(), stagingPath.u8string(),
        transactionPath.u8string());
    WindowsBootstrapFilesystemPort recovered(*backend);
    const WindowsBootstrapFilesystemRoots trusted{
        stagingPath.u8string(), transactionPath.u8string(),
        policy.updaterOwnedBase, policy.invocationIdentity};
    const auto recovery = recovered.BeginRecovery(
        values.plan, values.journal, trusted);
    check(recovery.filesystem.ok() &&
              recovery.disposition ==
                  WindowsBootstrapRecoveryDisposition::ContinueForward &&
              containsIntent(recovery.intents,
                             BootstrapIntentKind::ReplaceInventoryItem,
                             "HarkenSearch.exe") &&
              !std::filesystem::exists(installPath / "HarkenSearch.exe"),
          "fresh process rebinds the journal-bound prepared temp before mutation");
    if (!recovery.filesystem.ok()) return;
    const auto resumed = recovered.Apply(
        intent(BootstrapIntentKind::ReplaceInventoryItem, "HarkenSearch.exe"));
    check(resumed.ok() &&
              readNativeFixtureFile(installPath / "HarkenSearch.exe") ==
                  "new-native" &&
              !std::filesystem::exists(
                  installPath /
                  ".aris-bootstrap-attempt-crash-01-0-forward.tmp"),
          "fresh process atomically resumes the exact prepared publication");
#endif
}

void test_native_restart_selects_higher_journal_generation_after_slot_cut() {
#if defined(_WIN32)
    NativeSandbox sandbox;
    NativeSandboxCleanupAssertion cleanup(sandbox, "slot-child");
    const auto base = NativeSandbox::systemTempRoot();
    for (std::uint32_t attempt = 0; attempt < 32 && !sandbox.owned; ++attempt) {
        sandbox.path = base /
            ("slot-child-" + std::to_string(GetCurrentProcessId()) +
             "-" + std::to_string(attempt));
        std::error_code createError;
        sandbox.owned = std::filesystem::create_directory(sandbox.path,
                                                          createError);
    }
    check(sandbox.owned, "journal-slot sandbox is exclusively created");
    if (!sandbox.owned) return;
    const auto installPath = sandbox.path / "install";
    const auto updaterPath = sandbox.path / "updater";
    const auto stagingPath = updaterPath / "staging";
    const auto transactionPath = updaterPath / "transaction";
    std::error_code directoryError;
    check(std::filesystem::create_directory(installPath, directoryError) &&
              std::filesystem::create_directory(updaterPath, directoryError) &&
              std::filesystem::create_directory(stagingPath, directoryError) &&
              std::filesystem::create_directory(transactionPath, directoryError) &&
              writeNativeFixtureFile(stagingPath / "HarkenSearch.exe", "new-native"),
          "journal-slot roots and payload are created");
    const Fixture values = crashFixture(
        installPath.u8string(), stagingPath.u8string(),
        transactionPath.u8string());
    auto backend = createAuthorizedNativeBackend(
        installPath, stagingPath, transactionPath);
    check(backend != nullptr, "journal-slot seed backend is available");
    if (backend == nullptr) return;
    WindowsFilesystemHandle installRoot = 0;
    WindowsFilesystemHandle stagingRoot = 0;
    WindowsFilesystemHandle transactionRoot = 0;
    WindowsFileIdentity installIdentity;
    WindowsFileIdentity stagingIdentity;
    WindowsFileIdentity transactionIdentity;
    check(backend->OpenRootExact(installPath.u8string(),
                                 WindowsFilesystemRootRole::Install,
                                 installRoot, installIdentity).ok() &&
              backend->OpenRootExact(stagingPath.u8string(),
                                     WindowsFilesystemRootRole::Staging,
                                     stagingRoot, stagingIdentity).ok() &&
              backend->OpenRootExact(transactionPath.u8string(),
                                     WindowsFilesystemRootRole::Transaction,
                                     transactionRoot, transactionIdentity).ok(),
          "journal-slot seed roots bind exactly");
    WindowsFilesystemObservation staged;
    check(backend->ObserveNoFollow(stagingRoot, "HarkenSearch.exe", staged).ok(),
          "journal-slot seed observes payload");
    WindowsBootstrapJournalRecord seed = journalRecord(values, 1U);
    seed.installRootIdentity = installIdentity;
    seed.stagingRootIdentity = stagingIdentity;
    seed.transactionRootIdentity = transactionIdentity;
    seed.items.front().stagingIdentity = staged.identity;
    seed.items.front().stagingContent = staged.content;
    seed.items.front().installParentIdentity = installIdentity;
    WindowsFileIdentity seedIdentity;
    WindowsFileContentFingerprint seedContent;
    check(backend->PersistJournalDurable(
              transactionRoot, EncodeWindowsBootstrapJournal(seed),
              WindowsFilesystemReplaceMode::CreateNew, {}, {},
              seedIdentity, seedContent).ok(),
          "journal-slot seed publishes generation one");
    backend->CloseFilesystemHandle(transactionRoot);
    backend->CloseFilesystemHandle(stagingRoot);
    backend->CloseFilesystemHandle(installRoot);
    backend.reset();

    wchar_t executableBuffer[MAX_PATH]{};
    const DWORD executableLength = GetModuleFileNameW(
        nullptr, executableBuffer, static_cast<DWORD>(std::size(executableBuffer)));
    DWORD childExit = STILL_ACTIVE;
    check(executableLength != 0U &&
              launchJournalSlotCrashChild(
                  std::filesystem::path(executableBuffer), installPath,
                  stagingPath, transactionPath, childExit) &&
              childExit == kCrashCutExitCode,
          "child is forcibly terminated between journal generations after slot publication");

    auto recoveredBackend = createAuthorizedNativeBackend(
        installPath, stagingPath, transactionPath);
    check(recoveredBackend != nullptr, "journal-slot fresh backend is available");
    if (recoveredBackend == nullptr) return;
    {
        WindowsBootstrapFilesystemPort recovered(*recoveredBackend);
        const WindowsBootstrapFilesystemRoots trusted{
            stagingPath.u8string(), transactionPath.u8string(),
            updaterPath.u8string(), "signed-invocation-01"};
        const auto recovery = recovered.BeginRecovery(
            values.plan, values.journal, trusted);
        const bool hasReplacement = containsIntent(
            recovery.intents, BootstrapIntentKind::ReplaceInventoryItem,
            "HarkenSearch.exe");
        if (!recovery.filesystem.ok() ||
            recovery.disposition !=
                WindowsBootstrapRecoveryDisposition::ContinueForward ||
            recovery.generation != 2U || !hasReplacement) {
            std::fprintf(stderr,
                         "journal-slot recovery status=%d backend=%d native=%u disposition=%d generation=%llu intents=%zu\n",
                         static_cast<int>(recovery.filesystem.status),
                         static_cast<int>(recovery.filesystem.backendStatus),
                         recovery.filesystem.nativeError,
                         static_cast<int>(recovery.disposition),
                         static_cast<unsigned long long>(recovery.generation),
                         recovery.intents.size());
            for (const auto& candidate : recovery.intents) {
                std::fprintf(stderr,
                             "journal-slot intent=%d path=%s reason=%s\n",
                             static_cast<int>(candidate.kind),
                             candidate.relativePath.c_str(),
                             candidate.reason.c_str());
            }
        }
        check(recovery.filesystem.ok() &&
                  recovery.disposition ==
                      WindowsBootstrapRecoveryDisposition::ContinueForward &&
                  recovery.generation == 2U &&
                  hasReplacement,
              "fresh BeginRecovery selects the unique higher slot generation and emits replay intents");
    }
    installRoot = stagingRoot = transactionRoot = 0;
    check(recoveredBackend->OpenRootExact(
              installPath.u8string(), WindowsFilesystemRootRole::Install,
              installRoot, installIdentity).ok() &&
              recoveredBackend->OpenRootExact(
                  stagingPath.u8string(), WindowsFilesystemRootRole::Staging,
                  stagingRoot, stagingIdentity).ok() &&
              recoveredBackend->OpenRootExact(
                  transactionPath.u8string(),
                  WindowsFilesystemRootRole::Transaction, transactionRoot,
                  transactionIdentity).ok(),
          "journal-slot fresh process can rebind all roots after recovery");
    std::vector<WindowsBootstrapJournalCandidate> candidates;
    check(recoveredBackend->ReadJournalCandidatesNoFollow(
              transactionRoot, candidates).ok() &&
              candidates.size() == 2U,
          "journal-slot recovery leaves both durable generations inspectable");
    if (transactionRoot != 0) recoveredBackend->CloseFilesystemHandle(transactionRoot);
    if (stagingRoot != 0) recoveredBackend->CloseFilesystemHandle(stagingRoot);
    if (installRoot != 0) recoveredBackend->CloseFilesystemHandle(installRoot);
#endif
}

void test_native_root_binding_denies_external_rename() {
#if defined(_WIN32)
    NativeSandbox sandbox;
    NativeSandboxCleanupAssertion cleanup(sandbox, "root-race");
    const std::filesystem::path base = NativeSandbox::systemTempRoot();
    for (std::uint32_t attempt = 0; attempt < 32 && !sandbox.owned; ++attempt) {
        sandbox.path = base /
            ("aris-bootstrap-root-race-" + std::to_string(GetCurrentProcessId()) +
             "-" + std::to_string(attempt));
        std::error_code createError;
        sandbox.owned = std::filesystem::create_directory(sandbox.path,
                                                           createError);
    }
    check(sandbox.owned, "native root race sandbox is exclusively created");
    if (!sandbox.owned) return;

    const std::filesystem::path installPath = sandbox.path / "install";
    const std::filesystem::path updaterPath = sandbox.path / "updater";
    const std::filesystem::path stagingPath = updaterPath / "staging";
    const std::filesystem::path transactionPath = updaterPath / "transaction";
    const std::filesystem::path movedInstallPath = sandbox.path / "install-moved";
    const std::filesystem::path movedStagingPath =
        updaterPath / "staging-prebound";
    const std::filesystem::path movedTransactionPath =
        updaterPath / "transaction-prebound";
    std::error_code directoryError;
    check(std::filesystem::create_directory(installPath, directoryError) &&
              std::filesystem::create_directory(updaterPath, directoryError) &&
              std::filesystem::create_directory(stagingPath, directoryError) &&
              std::filesystem::create_directory(transactionPath, directoryError) &&
              writeNativeFixtureFile(installPath / "HarkenSearch.exe", "old-native"),
          "native root race fixture creates owned roots and target");

    auto backend = createAuthorizedNativeBackend(
        installPath, stagingPath, transactionPath);
    check(backend != nullptr,
          "native root race backend prebinds its authenticated roots");
    if (backend == nullptr) return;

    // ValidateTrustedWriteRoots closes its read-only probes after recording
    // each FileId. Replace a name before its first OpenRootExact: the later
    // spelling remains canonical, but must not gain the prebound root's
    // authority merely by occupying that pathname.
    const auto rejectsFirstOpenSubstitution =
        [&backend](const std::filesystem::path& path,
                   const std::filesystem::path& movedPath,
                   const WindowsFilesystemRootRole role,
                   const char* substituteMessage,
                   const char* rejectionMessage,
                   const char* restoreMessage) {
            std::error_code moveError;
            std::filesystem::rename(path, movedPath, moveError);
            std::error_code substituteError;
            const bool substituted = !moveError &&
                std::filesystem::create_directory(path, substituteError);
            check(substituted && !substituteError, substituteMessage);
            if (!substituted || substituteError) return false;

            WindowsFilesystemHandle substitutedRoot = 0;
            WindowsFileIdentity substitutedIdentity;
            const WindowsFilesystemBackendResult substitutedOpen =
                backend->OpenRootExact(path.u8string(), role,
                                       substitutedRoot,
                                       substitutedIdentity);
            check(substitutedOpen.status ==
                          WindowsFilesystemBackendStatus::IdentityMismatch &&
                      substitutedRoot == 0 && !substitutedIdentity.valid,
                  rejectionMessage);

            std::error_code removeError;
            const bool removed =
                std::filesystem::remove(path, removeError);
            std::error_code restoreError;
            if (removed && !removeError) {
                std::filesystem::rename(movedPath, path, restoreError);
            }
            const bool restored = removed && !removeError && !restoreError &&
                std::filesystem::exists(path);
            check(restored, restoreMessage);
            return restored;
        };
    if (!rejectsFirstOpenSubstitution(
            stagingPath, movedStagingPath,
            WindowsFilesystemRootRole::Staging,
            "native first-open race fixture substitutes the validated staging pathname",
            "first staging-root open rejects a canonical same-path substitution by its prebound FileId",
            "native first-open race fixture restores the prebound staging object") ||
        !rejectsFirstOpenSubstitution(
            transactionPath, movedTransactionPath,
            WindowsFilesystemRootRole::Transaction,
            "native first-open race fixture substitutes the validated transaction pathname",
            "first transaction-root open rejects a canonical same-path substitution by its prebound FileId",
            "native first-open race fixture restores the prebound transaction object")) {
        return;
    }

    WindowsFilesystemHandle installRoot = 0;
    WindowsFilesystemHandle transactionRoot = 0;
    WindowsFileIdentity installRootIdentity;
    WindowsFileIdentity transactionRootIdentity;
    check(backend->OpenRootExact(installPath.u8string(),
                                 WindowsFilesystemRootRole::Install,
                                 installRoot, installRootIdentity).ok() &&
              backend->OpenRootExact(transactionPath.u8string(),
                                     WindowsFilesystemRootRole::Transaction,
                                     transactionRoot,
                                     transactionRootIdentity).ok(),
          "native root race fixture binds install and transaction roots");
    if (installRoot == 0 || transactionRoot == 0) return;

    WindowsFilesystemObservation target;
    check(backend->ObserveNoFollow(installRoot, "HarkenSearch.exe", target).ok() &&
              target.kind == WindowsFilesystemEntryKind::RegularFile,
          "native root race fixture records exact target identity");
    WindowsFileIdentity journalIdentity;
    WindowsFileContentFingerprint journalContent;
    const std::vector<std::uint8_t> journalBytes{9U, 8U, 7U};
    check(backend->PersistJournalDurable(
              transactionRoot, journalBytes,
              WindowsFilesystemReplaceMode::CreateNew, {}, {},
              journalIdentity, journalContent).ok(),
          "native root race fixture publishes durable journal");
    WindowsFilesystemHandle journalLease = 0;
    check(backend->AcquireJournalMutationLease(
              transactionRoot, journalIdentity, journalContent,
              journalLease).ok(),
          "native root race fixture pins the journal");

    std::error_code renameError;
    std::filesystem::rename(installPath, movedInstallPath, renameError);
    check(renameError && std::filesystem::exists(installPath / "HarkenSearch.exe") &&
              !std::filesystem::exists(movedInstallPath),
          "bound install root denies external rename for the whole session");

    backend->ReleaseJournalMutationLease(journalLease);
    backend->CloseFilesystemHandle(transactionRoot);
    backend->CloseFilesystemHandle(installRoot);
#endif
}

void test_native_nested_parent_is_pinned_through_target_mutation() {
#if defined(_WIN32)
    NativeSandbox sandbox;
    NativeSandboxCleanupAssertion cleanup(sandbox, "parent-race");
    const std::filesystem::path base = NativeSandbox::systemTempRoot();
    for (std::uint32_t attempt = 0; attempt < 32 && !sandbox.owned; ++attempt) {
        sandbox.path = base /
            ("aris-bootstrap-parent-race-" +
             std::to_string(GetCurrentProcessId()) + "-" +
             std::to_string(attempt));
        std::error_code createError;
        sandbox.owned = std::filesystem::create_directory(sandbox.path,
                                                           createError);
    }
    check(sandbox.owned,
          "native nested-parent race sandbox is exclusively created");
    if (!sandbox.owned) return;

    const std::filesystem::path installPath = sandbox.path / "install";
    const std::filesystem::path updaterPath = sandbox.path / "updater";
    const std::filesystem::path stagingPath = updaterPath / "staging";
    const std::filesystem::path transactionPath = updaterPath / "transaction";
    const std::filesystem::path nestedParent = installPath / "a";
    const std::filesystem::path movedParent = installPath / "a-moved";
    std::error_code directoryError;
    const bool fixtureCreated =
        std::filesystem::create_directories(nestedParent / "b",
                                             directoryError) &&
        std::filesystem::create_directory(updaterPath, directoryError) &&
        std::filesystem::create_directory(stagingPath, directoryError) &&
        std::filesystem::create_directory(transactionPath, directoryError) &&
        writeNativeFixtureFile(nestedParent / "b" / "HarkenSearch.exe",
                               "old-native") &&
        writeNativeFixtureFile(stagingPath / "HarkenSearch.exe", "new-native");
    check(fixtureCreated,
          "native nested-parent race fixture creates owned roots and files");
    if (!fixtureCreated) return;

    NativeParentRenameProbe renameProbe;
    renameProbe.source = nestedParent;
    renameProbe.destination = movedParent;
    WindowsBootstrapFilesystemNativeTestHooks hooks;
    hooks.context = &renameProbe;
    hooks.beforeTargetMutation = &attemptNativeParentRename;
    auto backend = createAuthorizedNativeBackend(
        installPath, stagingPath, transactionPath, hooks);
    check(backend != nullptr,
          "native nested-parent race backend accepts a mutation seam");
    if (backend == nullptr) return;

    WindowsFilesystemHandle installRoot = 0;
    WindowsFilesystemHandle stagingRoot = 0;
    WindowsFilesystemHandle transactionRoot = 0;
    WindowsFileIdentity installRootIdentity;
    WindowsFileIdentity stagingRootIdentity;
    WindowsFileIdentity transactionRootIdentity;
    check(backend->OpenRootExact(installPath.u8string(),
                                 WindowsFilesystemRootRole::Install,
                                 installRoot, installRootIdentity).ok() &&
              backend->OpenRootExact(stagingPath.u8string(),
                                     WindowsFilesystemRootRole::Staging,
                                     stagingRoot, stagingRootIdentity).ok() &&
              backend->OpenRootExact(transactionPath.u8string(),
                                     WindowsFilesystemRootRole::Transaction,
                                     transactionRoot,
                                     transactionRootIdentity).ok(),
          "native nested-parent race fixture binds exact roots");
    if (installRoot == 0 || stagingRoot == 0 || transactionRoot == 0) return;

    WindowsFilesystemObservation target;
    WindowsFilesystemObservation targetParent;
    WindowsFilesystemObservation source;
    check(backend->ObserveNoFollow(installRoot, "a\\b\\HarkenSearch.exe", target).ok() &&
              target.kind == WindowsFilesystemEntryKind::RegularFile &&
              backend->ObserveNoFollow(installRoot, "a\\b", targetParent).ok() &&
              targetParent.kind == WindowsFilesystemEntryKind::Directory &&
              backend->ObserveNoFollow(stagingRoot, "HarkenSearch.exe", source).ok() &&
              source.kind == WindowsFilesystemEntryKind::RegularFile,
          "native nested-parent race fixture records target namespace");

    WindowsFileIdentity journalIdentity;
    WindowsFileContentFingerprint journalContent;
    const std::vector<std::uint8_t> journalBytes{4U, 3U, 2U, 1U};
    check(backend->PersistJournalDurable(
              transactionRoot, journalBytes,
              WindowsFilesystemReplaceMode::CreateNew, {}, {},
              journalIdentity, journalContent).ok(),
          "native nested-parent race fixture publishes durable journal");
    WindowsFilesystemHandle journalLease = 0;
    check(backend->AcquireJournalMutationLease(
              transactionRoot, journalIdentity, journalContent,
              journalLease).ok(),
          "native nested-parent race fixture pins the journal");

    const WindowsFileContentFingerprint oldContent = fingerprintHex(
        "956360794a2c32fc560b32feea1066c37221a421908bbbd877210fb30972eab4",
        10U);
    const WindowsFileContentFingerprint newContent = fingerprintHex(
        "cc738d1c75a1f105f18f5fe4ed9aa8defa4f8ad15fc64cfbefe2e2f0da259da5",
        10U);
    WindowsFileIdentity installedIdentity;
    WindowsFileMetadataSnapshot installedMetadata;
    const WindowsFilesystemBackendResult replaced =
        retryNativeAccessDenied(
            [&] {
                return backend->ReplaceFileDurableNoFollow(
                    stagingRoot, "HarkenSearch.exe", source.identity, newContent,
                    {},
                    installRoot, "a\\b\\HarkenSearch.exe", targetParent.identity,
                    ".nested-parent.tmp",
                    WindowsFilesystemReplaceMode::ReplaceExisting,
                    target.identity, oldContent, target.metadata,
                    installedIdentity, installedMetadata);
            },
            [&] {
                WindowsFilesystemObservation unchanged;
                return backend->ObserveNoFollow(installRoot,
                                                "a\\b\\HarkenSearch.exe",
                                                unchanged).ok() &&
                       unchanged.identity == target.identity &&
                       !std::filesystem::exists(
                           nestedParent / "b" / ".nested-parent.tmp");
            },
            "retryable nested replacement denial leaves the target unchanged");
    check(renameProbe.called &&
              renameProbe.rootRole == WindowsFilesystemRootRole::Install &&
              renameProbe.relativePath == "a\\b\\HarkenSearch.exe",
          "native mutation seam runs after binding the intended target parent");
    check(renameProbe.renameError &&
              std::filesystem::exists(nestedParent / "b" / "HarkenSearch.exe") &&
              !std::filesystem::exists(movedParent),
          "pinned intermediate install parent denies mid-mutation rename");
    checkNative(replaced,
                "native replacement completes inside the still-bound namespace");
    check(!replaced.ok() || installedIdentity.valid,
          "successful nested replacement returns the published identity");
    check(!replaced.ok() ||
              readNativeFixtureFile(nestedParent / "b" / "HarkenSearch.exe") ==
                  "new-native",
          "successful nested replacement publishes bytes in the bound namespace");

    backend->ReleaseJournalMutationLease(journalLease);
    backend->CloseFilesystemHandle(transactionRoot);
    backend->CloseFilesystemHandle(stagingRoot);
    backend->CloseFilesystemHandle(installRoot);
#endif
}

void test_native_atomic_replacement_has_no_delete_rename_gap() {
#if defined(_WIN32)
    NativeSandbox sandbox;
    NativeSandboxCleanupAssertion cleanup(sandbox, "partial-flush");
    const std::filesystem::path base = NativeSandbox::systemTempRoot();
    for (std::uint32_t attempt = 0; attempt < 32 && !sandbox.owned; ++attempt) {
        sandbox.path = base /
            ("aris-bootstrap-partial-flush-" +
             std::to_string(GetCurrentProcessId()) + "-" +
             std::to_string(attempt));
        std::error_code createError;
        sandbox.owned = std::filesystem::create_directory(sandbox.path,
                                                           createError);
    }
    check(sandbox.owned,
          "native partial-flush sandbox is exclusively created");
    if (!sandbox.owned) return;

    const std::filesystem::path installPath = sandbox.path / "install";
    const std::filesystem::path updaterPath = sandbox.path / "updater";
    const std::filesystem::path stagingPath = updaterPath / "staging";
    const std::filesystem::path transactionPath = updaterPath / "transaction";
    std::error_code directoryError;
    const bool fixtureCreated =
        std::filesystem::create_directory(installPath, directoryError) &&
        std::filesystem::create_directory(updaterPath, directoryError) &&
        std::filesystem::create_directory(stagingPath, directoryError) &&
        std::filesystem::create_directory(transactionPath, directoryError) &&
        writeNativeFixtureFile(installPath / "HarkenSearch.exe", "old-native") &&
        writeNativeFixtureFile(stagingPath / "HarkenSearch.exe", "new-native");
    check(fixtureCreated,
          "native partial-flush fixture creates owned roots and files");
    if (!fixtureCreated) return;

    WindowsBootstrapFilesystemNativeTestHooks hooks;
    hooks.accessDeniedFailurePoint =
        WindowsBootstrapFilesystemNativeTestFailurePoint::
            ReplacementAfterTargetDeleteFlush;
    auto backend = createAuthorizedNativeBackend(
        installPath, stagingPath, transactionPath, hooks);
    check(backend != nullptr,
          "native partial-flush backend accepts a durability failure seam");
    if (backend == nullptr) return;

    WindowsFilesystemHandle installRoot = 0;
    WindowsFilesystemHandle stagingRoot = 0;
    WindowsFilesystemHandle transactionRoot = 0;
    WindowsFileIdentity installRootIdentity;
    WindowsFileIdentity stagingRootIdentity;
    WindowsFileIdentity transactionRootIdentity;
    const auto installOpened = backend->OpenRootExact(
        installPath.u8string(), WindowsFilesystemRootRole::Install,
        installRoot, installRootIdentity);
    checkNative(installOpened,
                "native partial-flush fixture binds install root");
    if (!installOpened.ok()) return;
    const auto stagingOpened = backend->OpenRootExact(
        stagingPath.u8string(), WindowsFilesystemRootRole::Staging,
        stagingRoot, stagingRootIdentity);
    checkNative(stagingOpened,
                "native partial-flush fixture binds staging root");
    if (!stagingOpened.ok()) return;
    const auto transactionOpened = backend->OpenRootExact(
        transactionPath.u8string(), WindowsFilesystemRootRole::Transaction,
        transactionRoot, transactionRootIdentity);
    checkNative(transactionOpened,
                "native partial-flush fixture binds transaction root");
    if (!transactionOpened.ok()) return;

    WindowsFilesystemObservation target;
    WindowsFilesystemObservation source;
    const auto targetObserved =
        backend->ObserveNoFollow(installRoot, "HarkenSearch.exe", target);
    checkNative(targetObserved,
                "native partial-flush fixture observes target");
    check(target.kind == WindowsFilesystemEntryKind::RegularFile,
          "native partial-flush target is a regular file");
    if (!targetObserved.ok() ||
        target.kind != WindowsFilesystemEntryKind::RegularFile) {
        return;
    }
    const auto sourceObserved =
        backend->ObserveNoFollow(stagingRoot, "HarkenSearch.exe", source);
    checkNative(sourceObserved,
                "native partial-flush fixture observes source");
    check(source.kind == WindowsFilesystemEntryKind::RegularFile,
          "native partial-flush source is a regular file");
    if (!sourceObserved.ok() ||
        source.kind != WindowsFilesystemEntryKind::RegularFile) {
        return;
    }

    WindowsFileIdentity journalIdentity;
    WindowsFileContentFingerprint journalContent;
    const std::vector<std::uint8_t> journalBytes{6U, 6U, 6U};
    const auto journalPublished = backend->PersistJournalDurable(
        transactionRoot, journalBytes,
        WindowsFilesystemReplaceMode::CreateNew, {}, {}, journalIdentity,
        journalContent);
    checkNative(journalPublished,
                "native partial-flush fixture publishes durable journal");
    if (!journalPublished.ok()) return;
    WindowsFilesystemHandle journalLease = 0;
    const auto leaseAcquired = backend->AcquireJournalMutationLease(
        transactionRoot, journalIdentity, journalContent, journalLease);
    checkNative(leaseAcquired,
                "native partial-flush fixture pins the journal");
    if (!leaseAcquired.ok()) return;

    const WindowsFileContentFingerprint oldContent = fingerprintHex(
        "956360794a2c32fc560b32feea1066c37221a421908bbbd877210fb30972eab4",
        10U);
    const WindowsFileContentFingerprint newContent = fingerprintHex(
        "cc738d1c75a1f105f18f5fe4ed9aa8defa4f8ad15fc64cfbefe2e2f0da259da5",
        10U);
    WindowsFileIdentity installedIdentity;
    WindowsFileMetadataSnapshot installedMetadata;
    const WindowsFilesystemBackendResult replaced =
        backend->ReplaceFileDurableNoFollow(
            stagingRoot, "HarkenSearch.exe", source.identity, newContent, {},
            installRoot, "HarkenSearch.exe", installRootIdentity,
            ".partial-forward.tmp",
            WindowsFilesystemReplaceMode::ReplaceExisting,
            target.identity, oldContent, target.metadata,
            installedIdentity, installedMetadata);
    check(replaced.status == WindowsFilesystemBackendStatus::AccessDenied,
          "pre-atomic-publish denial remains retryable because no namespace mutation occurred");
    check(std::filesystem::exists(installPath / "HarkenSearch.exe") &&
              readNativeFixtureFile(installPath / "HarkenSearch.exe") == "old-native" &&
              !std::filesystem::exists(installPath / ".partial-forward.tmp"),
          "atomic replacement exposes no old-target-removed/new-target-not-renamed cut");

    backend->ReleaseJournalMutationLease(journalLease);
    backend->CloseFilesystemHandle(transactionRoot);
    backend->CloseFilesystemHandle(stagingRoot);
    backend->CloseFilesystemHandle(installRoot);
#endif
}

void test_native_delete_cut_points_fail_durability_closed() {
#if defined(_WIN32)
    const std::array<WindowsBootstrapFilesystemNativeTestFailurePoint, 3U>
        cutPoints{
            WindowsBootstrapFilesystemNativeTestFailurePoint::
                DeleteAfterMarkBeforeRebind,
            WindowsBootstrapFilesystemNativeTestFailurePoint::
                DeleteAfterRebindBeforeDirectoryFlush,
            WindowsBootstrapFilesystemNativeTestFailurePoint::
                DeleteAfterDirectoryFlushBeforeFinalRebind};
    const std::array<WindowsBootstrapFilesystemNativeTestFailurePoint, 3U>
        directoryCutPoints{
            WindowsBootstrapFilesystemNativeTestFailurePoint::
                RemoveDirectoryAfterMarkBeforeRebind,
            WindowsBootstrapFilesystemNativeTestFailurePoint::
                RemoveDirectoryAfterRebindBeforeDirectoryFlush,
            WindowsBootstrapFilesystemNativeTestFailurePoint::
                RemoveDirectoryAfterDirectoryFlushBeforeFinalRebind};

    for (std::size_t index = 0; index < cutPoints.size(); ++index) {
        NativeSandbox sandbox;
        NativeSandboxCleanupAssertion cleanup(
            sandbox, "delete-file-post-mutation-cut");
        const auto base = NativeSandbox::systemTempRoot();
        for (std::uint32_t attempt = 0; attempt < 32U && !sandbox.owned;
             ++attempt) {
            sandbox.path = base /
                ("delete-file-cut-" + std::to_string(GetCurrentProcessId()) +
                 "-" + std::to_string(index) + "-" +
                 std::to_string(attempt));
            std::error_code createError;
            sandbox.owned = std::filesystem::create_directory(
                sandbox.path, createError);
        }
        check(sandbox.owned, "delete-file cut sandbox is exclusive");
        if (!sandbox.owned) continue;
        const auto installPath = sandbox.path / "install";
        const auto updaterPath = sandbox.path / "updater";
        const auto stagingPath = updaterPath / "staging";
        const auto transactionPath = updaterPath / "transaction";
        std::error_code directoryError;
        const bool fixtureCreated =
            std::filesystem::create_directory(installPath, directoryError) &&
            std::filesystem::create_directory(updaterPath, directoryError) &&
            std::filesystem::create_directory(stagingPath, directoryError) &&
            std::filesystem::create_directory(transactionPath, directoryError) &&
            writeNativeFixtureFile(installPath / "HarkenSearch.exe", "delete-me");
        check(fixtureCreated, "delete-file cut fixture creates its roots");
        if (!fixtureCreated) continue;

        WindowsBootstrapFilesystemNativeTestHooks hooks;
        hooks.accessDeniedFailurePoint = cutPoints[index];
        auto backend = createAuthorizedNativeBackend(
            installPath, stagingPath, transactionPath, hooks);
        check(backend != nullptr, "delete-file cut backend is authorized");
        if (backend == nullptr) continue;

        WindowsFilesystemHandle installRoot = 0;
        WindowsFilesystemHandle transactionRoot = 0;
        WindowsFileIdentity installIdentity;
        WindowsFileIdentity transactionIdentity;
        const auto installOpened = backend->OpenRootExact(
            installPath.u8string(), WindowsFilesystemRootRole::Install,
            installRoot, installIdentity);
        const auto transactionOpened = installOpened.ok()
            ? backend->OpenRootExact(
                  transactionPath.u8string(),
                  WindowsFilesystemRootRole::Transaction, transactionRoot,
                  transactionIdentity)
            : WindowsFilesystemBackendResult{
                  WindowsFilesystemBackendStatus::IoFailure, 0};
        checkNative(installOpened,
                    "delete-file cut binds the install root");
        checkNative(transactionOpened,
                    "delete-file cut binds the transaction root");
        if (!installOpened.ok() || !transactionOpened.ok()) {
            if (transactionRoot != 0) {
                backend->CloseFilesystemHandle(transactionRoot);
            }
            if (installRoot != 0) backend->CloseFilesystemHandle(installRoot);
            continue;
        }
        WindowsFileIdentity journalIdentity;
        WindowsFileContentFingerprint journalContent;
        const auto journal = backend->PersistJournalDurable(
            transactionRoot, {0x44U, 0x45U, 0x4CU},
            WindowsFilesystemReplaceMode::CreateNew, {}, {}, journalIdentity,
            journalContent);
        checkNative(journal, "delete-file cut publishes its journal");
        WindowsFilesystemHandle lease = 0;
        const auto leased = journal.ok()
            ? backend->AcquireJournalMutationLease(
                  transactionRoot, journalIdentity, journalContent, lease)
            : WindowsFilesystemBackendResult{
                  WindowsFilesystemBackendStatus::IoFailure, 0};
        checkNative(leased, "delete-file cut acquires its journal lease");
        WindowsFilesystemObservation target;
        const auto observed = leased.ok()
            ? backend->ObserveNoFollow(installRoot, "HarkenSearch.exe", target)
            : WindowsFilesystemBackendResult{
                  WindowsFilesystemBackendStatus::IoFailure, 0};
        checkNative(observed, "delete-file cut observes its target");
        if (leased.ok() && observed.ok()) {
            const auto deleted = backend->DeleteFileDurableNoFollow(
                installRoot, "HarkenSearch.exe", installIdentity, target.identity,
                target.content, target.metadata);
            check(deleted.status ==
                      WindowsFilesystemBackendStatus::DurabilityFailed,
                  "post-mutation file delete failure is DurabilityFailed");
            check(deleted.status != WindowsFilesystemBackendStatus::AccessDenied,
                  "post-mutation file delete never remains retryable");
        }
        if (lease != 0) backend->ReleaseJournalMutationLease(lease);
        backend->CloseFilesystemHandle(transactionRoot);
        backend->CloseFilesystemHandle(installRoot);
        check(!std::filesystem::exists(installPath / "HarkenSearch.exe"),
              "post-mutation file delete cut leaves no visible target");
    }

    for (std::size_t index = 0; index < cutPoints.size(); ++index) {
        NativeSandbox sandbox;
        NativeSandboxCleanupAssertion cleanup(
            sandbox, "remove-directory-post-mutation-cut");
        const auto base = NativeSandbox::systemTempRoot();
        for (std::uint32_t attempt = 0; attempt < 32U && !sandbox.owned;
             ++attempt) {
            sandbox.path = base /
                ("remove-directory-cut-" +
                 std::to_string(GetCurrentProcessId()) + "-" +
                 std::to_string(index) + "-" + std::to_string(attempt));
            std::error_code createError;
            sandbox.owned = std::filesystem::create_directory(
                sandbox.path, createError);
        }
        check(sandbox.owned, "remove-directory cut sandbox is exclusive");
        if (!sandbox.owned) continue;
        const auto installPath = sandbox.path / "install";
        const auto updaterPath = sandbox.path / "updater";
        const auto stagingPath = updaterPath / "staging";
        const auto transactionPath = updaterPath / "transaction";
        std::error_code directoryError;
        const bool fixtureCreated =
            std::filesystem::create_directory(installPath, directoryError) &&
            std::filesystem::create_directory(updaterPath, directoryError) &&
            std::filesystem::create_directory(stagingPath, directoryError) &&
            std::filesystem::create_directory(transactionPath, directoryError);
        check(fixtureCreated,
              "remove-directory cut fixture creates its roots");
        if (!fixtureCreated) continue;

        WindowsBootstrapFilesystemNativeTestHooks hooks;
        hooks.accessDeniedFailurePoint = directoryCutPoints[index];
        auto backend = createAuthorizedNativeBackend(
            installPath, stagingPath, transactionPath, hooks);
        check(backend != nullptr,
              "remove-directory cut backend is authorized");
        if (backend == nullptr) continue;
        WindowsFilesystemHandle installRoot = 0;
        WindowsFilesystemHandle transactionRoot = 0;
        WindowsFilesystemHandle backupRoot = 0;
        WindowsFileIdentity installIdentity;
        WindowsFileIdentity transactionIdentity;
        WindowsFileIdentity backupIdentity;
        const auto installOpened = backend->OpenRootExact(
            installPath.u8string(), WindowsFilesystemRootRole::Install,
            installRoot, installIdentity);
        const auto transactionOpened = backend->OpenRootExact(
            transactionPath.u8string(), WindowsFilesystemRootRole::Transaction,
            transactionRoot, transactionIdentity);
        checkNative(installOpened,
                    "remove-directory cut binds install root");
        checkNative(transactionOpened,
                    "remove-directory cut binds transaction root");
        if (!installOpened.ok() || !transactionOpened.ok()) {
            if (transactionRoot != 0) {
                backend->CloseFilesystemHandle(transactionRoot);
            }
            if (installRoot != 0) backend->CloseFilesystemHandle(installRoot);
            continue;
        }
        WindowsFileIdentity journalIdentity;
        WindowsFileContentFingerprint journalContent;
        const auto journal = backend->PersistJournalDurable(
            transactionRoot, {0x44U, 0x49U, 0x52U},
            WindowsFilesystemReplaceMode::CreateNew, {}, {}, journalIdentity,
            journalContent);
        checkNative(journal,
                    "remove-directory cut publishes its journal");
        WindowsFilesystemHandle lease = 0;
        const auto leased = journal.ok()
            ? backend->AcquireJournalMutationLease(
                  transactionRoot, journalIdentity, journalContent, lease)
            : WindowsFilesystemBackendResult{
                  WindowsFilesystemBackendStatus::IoFailure, 0};
        checkNative(leased,
                    "remove-directory cut acquires its journal lease");
        const auto backupOpened = leased.ok()
            ? backend->OpenOrCreateDirectoryNoFollow(
                  transactionRoot, "backup", true, backupRoot, backupIdentity)
            : WindowsFilesystemBackendResult{
                  WindowsFilesystemBackendStatus::IoFailure, 0};
        checkNative(backupOpened,
                    "remove-directory cut creates its empty backup directory");
        if (leased.ok() && backupOpened.ok()) {
            // The directory capability is only needed for creation. Release
            // it before the delete cut so the no-follow delete handle can
            // acquire DELETE access without a sharing violation.
            backend->CloseFilesystemHandle(backupRoot);
            backupRoot = 0;
            const auto removed = backend->RemoveEmptyDirectoryDurableNoFollow(
                transactionRoot, "backup", backupIdentity);
            check(removed.status ==
                      WindowsFilesystemBackendStatus::DurabilityFailed,
                  "post-mutation directory removal is DurabilityFailed");
            check(removed.status != WindowsFilesystemBackendStatus::AccessDenied,
                  "post-mutation directory removal never remains retryable");
        }
        if (backupRoot != 0) backend->CloseFilesystemHandle(backupRoot);
        if (lease != 0) backend->ReleaseJournalMutationLease(lease);
        backend->CloseFilesystemHandle(transactionRoot);
        backend->CloseFilesystemHandle(installRoot);
        check(!std::filesystem::exists(transactionPath / "backup"),
              "post-mutation directory removal cut leaves no backup directory");
    }
#endif
}

void test_native_restart_replays_delete_cuts_from_separate_process() {
#if defined(_WIN32)
    const std::array<const char*, 2U> operations{"file", "directory"};
    for (const char* operation : operations) {
        for (std::size_t index = 0; index < 3U; ++index) {
            NativeSandbox sandbox;
            NativeSandboxCleanupAssertion cleanup(
                sandbox, "delete-cut-restart-child");
            const auto base = NativeSandbox::systemTempRoot();
            for (std::uint32_t attempt = 0;
                 attempt < 32U && !sandbox.owned; ++attempt) {
                sandbox.path = base /
                    (std::string("delete-cut-restart-") + operation + "-" +
                     std::to_string(GetCurrentProcessId()) + "-" +
                     std::to_string(index) + "-" +
                     std::to_string(attempt));
                std::error_code createError;
                sandbox.owned = std::filesystem::create_directory(
                    sandbox.path, createError);
            }
            check(sandbox.owned,
                  "delete-cut restart sandbox is exclusively created");
            if (!sandbox.owned) continue;
            const auto installPath = sandbox.path / "install";
            const auto updaterPath = sandbox.path / "updater";
            const auto stagingPath = updaterPath / "staging";
            const auto transactionPath = updaterPath / "transaction";
            std::error_code directoryError;
            const bool fixtureCreated =
                std::filesystem::create_directory(installPath,
                                                   directoryError) &&
                std::filesystem::create_directory(updaterPath,
                                                   directoryError) &&
                std::filesystem::create_directory(stagingPath,
                                                   directoryError) &&
                std::filesystem::create_directory(transactionPath,
                                                   directoryError) &&
                writeNativeFixtureFile(stagingPath / "HarkenSearch.exe",
                                       "new-native");
            check(fixtureCreated,
                  "delete-cut restart fixture creates signed payload roots");
            if (!fixtureCreated) continue;
            wchar_t executableBuffer[MAX_PATH]{};
            const DWORD executableLength = GetModuleFileNameW(
                nullptr, executableBuffer,
                static_cast<DWORD>(std::size(executableBuffer)));
            check(executableLength != 0U &&
                      executableLength < std::size(executableBuffer),
                  "delete-cut restart fixture resolves its executable");
            if (executableLength == 0U ||
                executableLength >= std::size(executableBuffer)) {
                continue;
            }
            DWORD childExit = STILL_ACTIVE;
            const bool launched = launchDeleteCrashChild(
                std::filesystem::path(executableBuffer), operation, index,
                installPath, stagingPath, transactionPath, childExit);
            check(launched && childExit == kCrashCutExitCode,
                  "delete-cut child exits only after its durable mutation cut");
            if (!launched || childExit != kCrashCutExitCode) continue;

            auto backend = createAuthorizedNativeBackend(
                installPath, stagingPath, transactionPath);
            check(backend != nullptr,
                  "delete-cut restart creates a fresh authorized backend");
            if (backend == nullptr) continue;
            const Fixture values = crashFixture(
                installPath.u8string(), stagingPath.u8string(),
                transactionPath.u8string());
            const WindowsBootstrapFilesystemRoots trusted{
                stagingPath.u8string(), transactionPath.u8string(),
                updaterPath.u8string(), "signed-invocation-01"};
            WindowsBootstrapFilesystemPort recovered(*backend);
            const auto recovery = recovered.BeginRecovery(
                values.plan, values.journal, trusted);
            check(recovery.filesystem.ok(),
                  "fresh process accepts the delete-cut journal state");
            if (!recovery.filesystem.ok()) continue;
            if (std::strcmp(operation, "file") == 0) {
                check(recovery.disposition ==
                              WindowsBootstrapRecoveryDisposition::
                                  Rollback ||
                          recovery.disposition ==
                              WindowsBootstrapRecoveryDisposition::
                                  CleanupCommitted ||
                          recovery.disposition ==
                              WindowsBootstrapRecoveryDisposition::Complete,
                      "fresh process classifies file-delete cut as deterministic rollback/cleanup");
                bool replayed = true;
                for (const BootstrapIntent& recoveryIntent : recovery.intents) {
                    const auto applied = recovered.Apply(recoveryIntent);
                    replayed = applied.ok() && replayed;
                }
                check(replayed &&
                          !std::filesystem::exists(installPath / "HarkenSearch.exe") &&
                          !std::filesystem::exists(transactionPath / "backup"),
                      "file-delete restart leaves target and backup namespace absent");
            } else {
                check(recovery.disposition ==
                              WindowsBootstrapRecoveryDisposition::
                                  CleanupCommitted,
                      "fresh process treats a missing started backup directory as committed cleanup awaiting availability recovery");
                check(!std::filesystem::exists(transactionPath / "backup") &&
                          std::filesystem::exists(installPath / "HarkenSearch.exe") &&
                          readNativeFixtureFile(installPath / "HarkenSearch.exe") ==
                              "new-native",
                      "directory-delete restart preserves the committed target");
            }
        }
    }
#endif
}

void test_native_backup_post_publication_access_denied_is_durability_failure() {
#if defined(_WIN32)
    NativeSandbox sandbox;
    NativeSandboxCleanupAssertion cleanup(sandbox, "backup-post-publish");
    const std::filesystem::path base = NativeSandbox::systemTempRoot();
    for (std::uint32_t attempt = 0; attempt < 32 && !sandbox.owned; ++attempt) {
        sandbox.path = base /
            ("backup-post-publish-" + std::to_string(GetCurrentProcessId()) +
             "-" + std::to_string(attempt));
        std::error_code createError;
        sandbox.owned = std::filesystem::create_directory(sandbox.path,
                                                          createError);
    }
    check(sandbox.owned, "backup post-publication sandbox is exclusively created");
    if (!sandbox.owned) return;
    const auto installPath = sandbox.path / "install";
    const auto updaterPath = sandbox.path / "updater";
    const auto stagingPath = updaterPath / "staging";
    const auto transactionPath = updaterPath / "transaction";
    std::error_code directoryError;
    const bool fixtureCreated =
        std::filesystem::create_directory(installPath, directoryError) &&
        std::filesystem::create_directory(updaterPath, directoryError) &&
        std::filesystem::create_directory(stagingPath, directoryError) &&
        std::filesystem::create_directory(transactionPath, directoryError) &&
        writeNativeFixtureFile(installPath / "HarkenSearch.exe", "old-native");
    check(fixtureCreated, "backup post-publication roots and source are created");
    if (!fixtureCreated) return;

    WindowsBootstrapFilesystemNativeTestHooks hooks;
    hooks.accessDeniedFailurePoint =
        WindowsBootstrapFilesystemNativeTestFailurePoint::
            BackupAfterPublishBeforeRebind;
    auto backend = createAuthorizedNativeBackend(
        installPath, stagingPath, transactionPath, hooks);
    check(backend != nullptr, "backup post-publication backend is available");
    if (backend == nullptr) return;
    WindowsFilesystemHandle installRoot = 0;
    WindowsFilesystemHandle stagingRoot = 0;
    WindowsFilesystemHandle transactionRoot = 0;
    WindowsFilesystemHandle backupRoot = 0;
    WindowsFileIdentity installIdentity;
    WindowsFileIdentity stagingIdentity;
    WindowsFileIdentity transactionIdentity;
    WindowsFileIdentity backupIdentity;
    const auto installOpened = backend->OpenRootExact(
        installPath.u8string(), WindowsFilesystemRootRole::Install,
        installRoot, installIdentity);
    const auto stagingOpened = installOpened.ok()
        ? backend->OpenRootExact(
              stagingPath.u8string(), WindowsFilesystemRootRole::Staging,
              stagingRoot, stagingIdentity)
        : WindowsFilesystemBackendResult{
              WindowsFilesystemBackendStatus::IoFailure, 0};
    const auto transactionOpened = stagingOpened.ok()
        ? backend->OpenRootExact(
              transactionPath.u8string(),
              WindowsFilesystemRootRole::Transaction, transactionRoot,
              transactionIdentity)
        : WindowsFilesystemBackendResult{
              WindowsFilesystemBackendStatus::IoFailure, 0};
    checkNative(installOpened,
                "backup post-publication install root binds exactly");
    checkNative(stagingOpened,
                "backup post-publication staging root binds exactly");
    checkNative(transactionOpened,
                "backup post-publication transaction root binds exactly");
    if (!installOpened.ok() || !stagingOpened.ok() ||
        !transactionOpened.ok()) {
        return;
    }
    WindowsFileIdentity journalIdentity;
    WindowsFileContentFingerprint journalContent;
    const auto journalPublished = backend->PersistJournalDurable(
        transactionRoot, {9U, 9U, 9U},
        WindowsFilesystemReplaceMode::CreateNew, {}, {}, journalIdentity,
        journalContent);
    checkNative(journalPublished,
                "backup post-publication fixture publishes its journal");
    if (!journalPublished.ok()) return;
    WindowsFilesystemHandle lease = 0;
    const auto leaseAcquired = backend->AcquireJournalMutationLease(
        transactionRoot, journalIdentity, journalContent, lease);
    checkNative(leaseAcquired,
                "backup post-publication fixture acquires its journal lease");
    if (!leaseAcquired.ok()) return;
    const auto backupOpened = backend->OpenOrCreateDirectoryNoFollow(
        transactionRoot, "backup", true, backupRoot, backupIdentity);
    checkNative(backupOpened,
                "backup post-publication fixture creates its backup root");
    if (!backupOpened.ok()) return;
    WindowsFilesystemObservation source;
    check(backend->ObserveNoFollow(stagingRoot, "HarkenSearch.exe", source).ok() &&
              source.kind == WindowsFilesystemEntryKind::Missing,
          "backup post-publication fixture does not use staging as backup authority");
    WindowsFilesystemObservation old;
    check(backend->ObserveNoFollow(installRoot, "HarkenSearch.exe", old).ok(),
          "backup post-publication fixture captures source identity");
    WindowsFileIdentity copiedIdentity;
    WindowsFileContentFingerprint copiedContent;
    WindowsFileMetadataSnapshot copiedMetadata;
    const auto copied = backend->CopyFileDurableNoFollow(
        installRoot, "HarkenSearch.exe", old.identity, backupRoot, "HarkenSearch.exe",
        copiedIdentity, copiedContent, copiedMetadata);
    check(copied.status == WindowsFilesystemBackendStatus::DurabilityFailed,
          "post-publication AccessDenied is classified as DurabilityFailed");
    check(std::filesystem::exists(transactionPath / "backup" / "HarkenSearch.exe"),
          "post-publication AccessDenied leaves an explicit durable recovery object");
    if (lease != 0) backend->ReleaseJournalMutationLease(lease);
    if (backupRoot != 0) backend->CloseFilesystemHandle(backupRoot);
    if (transactionRoot != 0) backend->CloseFilesystemHandle(transactionRoot);
    if (stagingRoot != 0) backend->CloseFilesystemHandle(stagingRoot);
    if (installRoot != 0) backend->CloseFilesystemHandle(installRoot);
#endif
}

void test_native_backup_directory_access_denied_phase_classification() {
#if defined(_WIN32)
    NativeSandbox sandbox;
    NativeSandboxCleanupAssertion cleanup(sandbox, "backup-directory-access");
    const std::filesystem::path base = NativeSandbox::systemTempRoot();
    for (std::uint32_t attempt = 0; attempt < 32U && !sandbox.owned;
         ++attempt) {
        sandbox.path = base /
            ("backup-directory-access-" +
             std::to_string(GetCurrentProcessId()) + "-" +
             std::to_string(attempt));
        std::error_code error;
        sandbox.owned = std::filesystem::create_directory(sandbox.path, error);
    }
    check(sandbox.owned,
          "backup-directory access fixture obtains an exclusive sandbox");
    if (!sandbox.owned) return;

    struct Case {
        WindowsBootstrapFilesystemNativeTestFailurePoint point;
        WindowsFilesystemBackendStatus expected;
        bool directoryExpected;
        const char* name;
    };
    const std::array<Case, 2U> cases{{
        {WindowsBootstrapFilesystemNativeTestFailurePoint::
             BackupDirectoryBeforeCreate,
         WindowsFilesystemBackendStatus::AccessDenied, false,
         "before-create"},
        {WindowsBootstrapFilesystemNativeTestFailurePoint::
             BackupDirectoryAfterCreateBeforeObservation,
         WindowsFilesystemBackendStatus::DurabilityFailed, true,
         "after-create"},
    }};

    for (const Case& current : cases) {
        const std::filesystem::path caseRoot =
            sandbox.path / current.name;
        const std::filesystem::path installPath = caseRoot / "install";
        const std::filesystem::path updaterPath = caseRoot / "updater";
        const std::filesystem::path stagingPath = updaterPath / "staging";
        const std::filesystem::path transactionPath =
            updaterPath / "transaction";
        std::error_code directoryError;
        const bool fixtureCreated =
            std::filesystem::create_directories(installPath, directoryError) &&
            std::filesystem::create_directory(updaterPath, directoryError) &&
            std::filesystem::create_directory(stagingPath, directoryError) &&
            std::filesystem::create_directory(transactionPath, directoryError) &&
            writeNativeFixtureFile(installPath / "HarkenSearch.exe", "old-native");
        check(fixtureCreated, "backup-directory access roots are created");
        if (!fixtureCreated) continue;

        WindowsBootstrapFilesystemNativeTestHooks hooks;
        hooks.accessDeniedFailurePoint = current.point;
        auto backend = createAuthorizedNativeBackend(
            installPath, stagingPath, transactionPath, hooks);
        check(backend != nullptr, "backup-directory access backend is available");
        if (backend == nullptr) continue;

        WindowsFilesystemHandle installRoot = 0;
        WindowsFilesystemHandle stagingRoot = 0;
        WindowsFilesystemHandle transactionRoot = 0;
        WindowsFilesystemHandle backupRoot = 0;
        WindowsFileIdentity installIdentity;
        WindowsFileIdentity stagingIdentity;
        WindowsFileIdentity transactionIdentity;
        WindowsFileIdentity backupIdentity;
        const auto installOpened = backend->OpenRootExact(
            installPath.u8string(), WindowsFilesystemRootRole::Install,
            installRoot, installIdentity);
        const auto stagingOpened = installOpened.ok()
            ? backend->OpenRootExact(
                  stagingPath.u8string(), WindowsFilesystemRootRole::Staging,
                  stagingRoot, stagingIdentity)
            : WindowsFilesystemBackendResult{
                  WindowsFilesystemBackendStatus::IoFailure, 0};
        const auto transactionOpened = stagingOpened.ok()
            ? backend->OpenRootExact(
                  transactionPath.u8string(),
                  WindowsFilesystemRootRole::Transaction, transactionRoot,
                  transactionIdentity)
            : WindowsFilesystemBackendResult{
                  WindowsFilesystemBackendStatus::IoFailure, 0};
        checkNative(installOpened, "backup-directory access install root binds");
        checkNative(stagingOpened, "backup-directory access staging root binds");
        checkNative(transactionOpened,
                    "backup-directory access transaction root binds");
        if (!installOpened.ok() || !stagingOpened.ok() ||
            !transactionOpened.ok()) {
            if (transactionRoot != 0) backend->CloseFilesystemHandle(transactionRoot);
            if (stagingRoot != 0) backend->CloseFilesystemHandle(stagingRoot);
            if (installRoot != 0) backend->CloseFilesystemHandle(installRoot);
            continue;
        }

        WindowsFileIdentity journalIdentity;
        WindowsFileContentFingerprint journalContent;
        const auto journalPublished = backend->PersistJournalDurable(
            transactionRoot, {9U, 9U, 9U},
            WindowsFilesystemReplaceMode::CreateNew, {}, {}, journalIdentity,
            journalContent);
        checkNative(journalPublished,
                    "backup-directory access fixture publishes its journal");
        WindowsFilesystemHandle lease = 0;
        const auto leaseAcquired = journalPublished.ok()
            ? backend->AcquireJournalMutationLease(
                  transactionRoot, journalIdentity, journalContent, lease)
            : WindowsFilesystemBackendResult{
                  WindowsFilesystemBackendStatus::IoFailure, 0};
        checkNative(leaseAcquired,
                    "backup-directory access fixture acquires its lease");
        if (journalPublished.ok() && leaseAcquired.ok()) {
            const auto directory = backend->OpenOrCreateDirectoryNoFollow(
                transactionRoot, "backup", true, backupRoot, backupIdentity);
            check(directory.status == current.expected &&
                      std::filesystem::exists(transactionPath / "backup") ==
                          current.directoryExpected,
                  current.name);
        }
        if (lease != 0) backend->ReleaseJournalMutationLease(lease);
        if (backupRoot != 0) backend->CloseFilesystemHandle(backupRoot);
        if (transactionRoot != 0) backend->CloseFilesystemHandle(transactionRoot);
        if (stagingRoot != 0) backend->CloseFilesystemHandle(stagingRoot);
        if (installRoot != 0) backend->CloseFilesystemHandle(installRoot);
    }
#endif
}

void test_native_empty_parent_cannot_be_reparsed_during_create_new() {
#if defined(_WIN32)
    NativeSandbox sandbox;
    NativeSandboxCleanupAssertion cleanup(sandbox, "reparse-race");
    const std::filesystem::path base = NativeSandbox::systemTempRoot();
    for (std::uint32_t attempt = 0; attempt < 32 && !sandbox.owned; ++attempt) {
        sandbox.path = base /
            ("aris-bootstrap-reparse-race-" +
             std::to_string(GetCurrentProcessId()) + "-" +
             std::to_string(attempt));
        std::error_code createError;
        sandbox.owned = std::filesystem::create_directory(sandbox.path,
                                                           createError);
    }
    check(sandbox.owned,
          "native reparse race sandbox is exclusively created");
    if (!sandbox.owned) return;

    const std::filesystem::path installPath = sandbox.path / "install";
    const std::filesystem::path updaterPath = sandbox.path / "updater";
    const std::filesystem::path stagingPath = updaterPath / "staging";
    const std::filesystem::path transactionPath = updaterPath / "transaction";
    const std::filesystem::path emptyParent = installPath / "slot";
    const std::filesystem::path outsidePath = sandbox.path / "outside";
    std::error_code directoryError;
    const bool fixtureCreated =
        std::filesystem::create_directories(emptyParent, directoryError) &&
        std::filesystem::create_directory(updaterPath, directoryError) &&
        std::filesystem::create_directory(stagingPath, directoryError) &&
        std::filesystem::create_directory(transactionPath, directoryError) &&
        std::filesystem::create_directory(outsidePath, directoryError) &&
        writeNativeFixtureFile(stagingPath / "HarkenSearch.exe", "new-native");
    check(fixtureCreated,
          "native reparse race fixture creates an empty install parent");
    if (!fixtureCreated) return;

    NativeJunctionProbe junctionProbe;
    junctionProbe.junction = emptyParent;
    junctionProbe.target = outsidePath;
    WindowsBootstrapFilesystemNativeTestHooks hooks;
    hooks.context = &junctionProbe;
    hooks.beforeTargetMutation = &attemptNativeJunction;
    auto backend = createAuthorizedNativeBackend(
        installPath, stagingPath, transactionPath, hooks);
    check(backend != nullptr,
          "native reparse race backend accepts the mutation seam");
    if (backend == nullptr) return;

    WindowsFilesystemHandle installRoot = 0;
    WindowsFilesystemHandle stagingRoot = 0;
    WindowsFilesystemHandle transactionRoot = 0;
    WindowsFileIdentity installRootIdentity;
    WindowsFileIdentity stagingRootIdentity;
    WindowsFileIdentity transactionRootIdentity;
    check(backend->OpenRootExact(installPath.u8string(),
                                 WindowsFilesystemRootRole::Install,
                                 installRoot, installRootIdentity).ok() &&
              backend->OpenRootExact(stagingPath.u8string(),
                                     WindowsFilesystemRootRole::Staging,
                                     stagingRoot, stagingRootIdentity).ok() &&
              backend->OpenRootExact(transactionPath.u8string(),
                                     WindowsFilesystemRootRole::Transaction,
                                     transactionRoot,
                                     transactionRootIdentity).ok(),
          "native reparse race fixture binds exact roots");
    if (installRoot == 0 || stagingRoot == 0 || transactionRoot == 0) return;

    WindowsFilesystemObservation source;
    WindowsFilesystemObservation parent;
    check(backend->ObserveNoFollow(stagingRoot, "HarkenSearch.exe", source).ok() &&
              source.kind == WindowsFilesystemEntryKind::RegularFile &&
              backend->ObserveNoFollow(installRoot, "slot", parent).ok() &&
              parent.kind == WindowsFilesystemEntryKind::Directory,
          "native reparse race fixture records source and parent identity");
    WindowsFileIdentity journalIdentity;
    WindowsFileContentFingerprint journalContent;
    const std::vector<std::uint8_t> journalBytes{7U, 7U, 7U};
    check(backend->PersistJournalDurable(
              transactionRoot, journalBytes,
              WindowsFilesystemReplaceMode::CreateNew, {}, {},
              journalIdentity, journalContent).ok(),
          "native reparse race fixture publishes durable journal");
    WindowsFilesystemHandle journalLease = 0;
    check(backend->AcquireJournalMutationLease(
              transactionRoot, journalIdentity, journalContent,
              journalLease).ok(),
          "native reparse race fixture pins the journal");

    const WindowsFileContentFingerprint newContent = fingerprintHex(
        "cc738d1c75a1f105f18f5fe4ed9aa8defa4f8ad15fc64cfbefe2e2f0da259da5",
        10U);
    WindowsFileIdentity installedIdentity;
    WindowsFileMetadataSnapshot installedMetadata;
    const WindowsFilesystemBackendResult replaced =
        backend->ReplaceFileDurableNoFollow(
            stagingRoot, "HarkenSearch.exe", source.identity, newContent,
            {},
            installRoot, "slot\\HarkenSearch.exe", parent.identity,
            ".reparse-forward.tmp",
            WindowsFilesystemReplaceMode::CreateNew, {}, {}, {},
            installedIdentity, installedMetadata);
    check(junctionProbe.called && junctionProbe.created,
          "native race fixture converts the verified empty parent to a junction");
    check(replaced.status == WindowsFilesystemBackendStatus::ReparsePoint &&
              !std::filesystem::exists(outsidePath / "HarkenSearch.exe"),
          "create-new fails closed after parent reparse mutation");

    backend->ReleaseJournalMutationLease(journalLease);
    backend->CloseFilesystemHandle(transactionRoot);
    backend->CloseFilesystemHandle(stagingRoot);
    backend->CloseFilesystemHandle(installRoot);
#endif
}

void test_native_invocation_cleanup_durably_removes_authorized_residue() {
#if defined(_WIN32)
    NativeSandbox sandbox;
    NativeSandboxCleanupAssertion cleanupAssertion(
        sandbox, "invocation-cleanup-durable-success");
    const std::filesystem::path base = NativeSandbox::systemTempRoot();
    for (std::uint32_t attempt = 0; attempt < 32 && !sandbox.owned; ++attempt) {
        sandbox.path = base /
            ("aris-bootstrap-invocation-cleanup-durable-success-" +
             std::to_string(GetCurrentProcessId()) + "-" +
             std::to_string(attempt));
        std::error_code createError;
        sandbox.owned = std::filesystem::create_directory(sandbox.path,
                                                          createError);
    }
    check(sandbox.owned,
          "native invocation-cleanup success sandbox is exclusively created");
    if (!sandbox.owned) return;

    const std::filesystem::path installPath = sandbox.path / "install";
    const std::filesystem::path updaterPath = sandbox.path / "updater";
    const std::filesystem::path stagingPath = updaterPath / "staging";
    const std::filesystem::path transactionPath = updaterPath / "transaction";
    const std::filesystem::path runtimePairPath =
        transactionPath / "runtime-pair";
    std::error_code directoryError;
    const bool fixtureCreated =
        std::filesystem::create_directory(installPath, directoryError) &&
        std::filesystem::create_directory(updaterPath, directoryError) &&
        std::filesystem::create_directory(stagingPath, directoryError) &&
        std::filesystem::create_directory(transactionPath, directoryError) &&
        std::filesystem::create_directory(runtimePairPath, directoryError) &&
        writeNativeFixtureFile(stagingPath / "HarkenSearch.exe", "staging-owned") &&
        writeNativeFixtureFile(runtimePairPath / "HarkenSearchBootstrap.exe",
                               "bootstrap-owned");
    check(fixtureCreated,
          "native invocation-cleanup success fixture creates authorized residue");
    if (!fixtureCreated) return;

    auto backend = createAuthorizedNativeBackend(
        installPath, stagingPath, transactionPath);
    check(backend != nullptr,
          "native invocation-cleanup success backend is authorized");
    if (backend == nullptr) return;

    WindowsFilesystemHandle installRoot = 0;
    WindowsFilesystemHandle stagingRoot = 0;
    WindowsFilesystemHandle transactionRoot = 0;
    WindowsFileIdentity installRootIdentity;
    WindowsFileIdentity stagingRootIdentity;
    WindowsFileIdentity transactionRootIdentity;
    const WindowsFilesystemBackendResult installOpened =
        backend->OpenRootExact(installPath.u8string(),
                               WindowsFilesystemRootRole::Install,
                               installRoot, installRootIdentity);
    const WindowsFilesystemBackendResult stagingOpened =
        installOpened.ok()
            ? backend->OpenRootExact(stagingPath.u8string(),
                                     WindowsFilesystemRootRole::Staging,
                                     stagingRoot, stagingRootIdentity)
            : installOpened;
    const WindowsFilesystemBackendResult transactionOpened =
        stagingOpened.ok()
            ? backend->OpenRootExact(transactionPath.u8string(),
                                     WindowsFilesystemRootRole::Transaction,
                                     transactionRoot, transactionRootIdentity)
            : stagingOpened;
    checkNative(installOpened,
                "native invocation-cleanup success fixture opens install root");
    checkNative(stagingOpened,
                "native invocation-cleanup success fixture opens staging root");
    checkNative(transactionOpened,
                "native invocation-cleanup success fixture opens transaction root");
    const bool rootsBound = installOpened.ok() && stagingOpened.ok() &&
        transactionOpened.ok();
    check(rootsBound,
          "native invocation-cleanup success fixture binds exact roots");
    if (!rootsBound) return;

    WindowsFilesystemObservation stagingFile;
    WindowsFilesystemObservation runtimePairDirectory;
    WindowsFilesystemObservation bootstrapFile;
    const WindowsFilesystemBackendResult stagingObserved =
        backend->ObserveNoFollow(stagingRoot, "HarkenSearch.exe", stagingFile);
    const WindowsFilesystemBackendResult runtimePairObserved =
        stagingObserved.ok()
            ? backend->ObserveNoFollow(transactionRoot, "runtime-pair",
                                       runtimePairDirectory)
            : stagingObserved;
    const WindowsFilesystemBackendResult bootstrapObserved =
        runtimePairObserved.ok()
            ? backend->ObserveNoFollow(transactionRoot,
                                       "runtime-pair/HarkenSearchBootstrap.exe",
                                       bootstrapFile)
            : runtimePairObserved;
    checkNative(stagingObserved,
                "native invocation-cleanup success fixture observes staging residue");
    checkNative(bootstrapObserved,
                "native invocation-cleanup success fixture observes runtime residue");
    const bool residueBound =
        stagingObserved.ok() &&
        stagingFile.kind == WindowsFilesystemEntryKind::RegularFile &&
        stagingFile.identity.valid && stagingFile.content.valid &&
        runtimePairObserved.ok() &&
        runtimePairDirectory.kind == WindowsFilesystemEntryKind::Directory &&
        runtimePairDirectory.identity.valid &&
        bootstrapObserved.ok() &&
        bootstrapFile.kind == WindowsFilesystemEntryKind::RegularFile &&
        bootstrapFile.identity.valid && bootstrapFile.content.valid;
    check(residueBound,
          "native invocation-cleanup success fixture binds exact residue authority");
    if (!residueBound) return;

    WindowsFileIdentity journalIdentity;
    WindowsFileContentFingerprint journalContent;
    WindowsFilesystemHandle journalLease = 0;
    const WindowsFilesystemBackendResult journalPersisted =
        backend->PersistJournalDurable(
            transactionRoot, {1U, 2U, 3U},
            WindowsFilesystemReplaceMode::CreateNew, {}, {},
            journalIdentity, journalContent);
    const WindowsFilesystemBackendResult leaseAcquired =
        journalPersisted.ok()
            ? backend->AcquireJournalMutationLease(
                  transactionRoot, journalIdentity, journalContent,
                  journalLease)
            : journalPersisted;
    checkNative(journalPersisted,
                "native invocation-cleanup success fixture persists its journal");
    checkNative(leaseAcquired,
                "native invocation-cleanup success fixture acquires its journal lease");
    const bool journalReady = journalPersisted.ok() && leaseAcquired.ok();
    if (!journalReady) return;

    WindowsBootstrapInvocationCleanup invocationCleanup;
    invocationCleanup.runtimePairExpected = true;
    invocationCleanup.runtimePairDirectoryIdentity =
        runtimePairDirectory.identity;
    invocationCleanup.runtimePairBootstrapIdentity = bootstrapFile.identity;
    invocationCleanup.runtimePairBootstrapContent = bootstrapFile.content;
    invocationCleanup.stagingFiles.push_back(
        {"HarkenSearch.exe", stagingFile.identity, stagingFile.content});
    invocationCleanup.transactionFiles.push_back(
        {"runtime-pair/HarkenSearchBootstrap.exe", bootstrapFile.identity,
         bootstrapFile.content});
    const WindowsFilesystemBackendResult cleaned =
        backend->CleanupInvocationResidueNoFollow(
            stagingRoot, transactionRoot, invocationCleanup);
    checkNative(cleaned,
                "authorized invocation residue is durably removed");
    check(!std::filesystem::exists(stagingPath / "HarkenSearch.exe"),
          "durable invocation cleanup removes staging residue");
    check(!std::filesystem::exists(runtimePairPath),
          "durable invocation cleanup removes runtime-pair residue");
    check(std::filesystem::exists(transactionPath /
                                  "aris-bootstrap.journal.a") ||
              std::filesystem::exists(transactionPath /
                                      "aris-bootstrap.journal.b"),
          "durable invocation cleanup preserves its journal");

    backend->ReleaseJournalMutationLease(journalLease);
    backend->CloseFilesystemHandle(transactionRoot);
    backend->CloseFilesystemHandle(stagingRoot);
    backend->CloseFilesystemHandle(installRoot);
#endif
}

void test_native_invocation_cleanup_preflights_runtime_pair_namespace() {
#if defined(_WIN32)
    NativeSandbox sandbox;
    NativeSandboxCleanupAssertion cleanupAssertion(
        sandbox, "invocation-cleanup-namespace");
    const std::filesystem::path base = NativeSandbox::systemTempRoot();
    for (std::uint32_t attempt = 0; attempt < 32 && !sandbox.owned; ++attempt) {
        sandbox.path = base /
            ("aris-bootstrap-invocation-cleanup-namespace-" +
             std::to_string(GetCurrentProcessId()) + "-" +
             std::to_string(attempt));
        std::error_code createError;
        sandbox.owned = std::filesystem::create_directory(sandbox.path,
                                                          createError);
    }
    check(sandbox.owned,
          "native invocation-cleanup namespace sandbox is exclusively created");
    if (!sandbox.owned) return;

    const std::filesystem::path installPath = sandbox.path / "install";
    const std::filesystem::path updaterPath = sandbox.path / "updater";
    const std::filesystem::path stagingPath = updaterPath / "staging";
    const std::filesystem::path transactionPath = updaterPath / "transaction";
    const std::filesystem::path runtimePairPath =
        transactionPath / "runtime-pair";
    std::error_code directoryError;
    const bool fixtureCreated =
        std::filesystem::create_directory(installPath, directoryError) &&
        std::filesystem::create_directory(updaterPath, directoryError) &&
        std::filesystem::create_directory(stagingPath, directoryError) &&
        std::filesystem::create_directory(transactionPath, directoryError) &&
        std::filesystem::create_directory(runtimePairPath, directoryError) &&
        writeNativeFixtureFile(stagingPath / "HarkenSearch.exe", "staging-owned") &&
        writeNativeFixtureFile(runtimePairPath / "HarkenSearchBootstrap.exe",
                               "bootstrap-owned");
    check(fixtureCreated,
          "native invocation-cleanup namespace fixture creates owned residue and an unknown child");
    if (!fixtureCreated) return;

    auto backend = createAuthorizedNativeBackend(
        installPath, stagingPath, transactionPath);
    check(backend != nullptr,
          "native invocation-cleanup namespace backend is authorized");
    if (backend == nullptr) return;

    WindowsFilesystemHandle installRoot = 0;
    WindowsFilesystemHandle stagingRoot = 0;
    WindowsFilesystemHandle transactionRoot = 0;
    WindowsFileIdentity installRootIdentity;
    WindowsFileIdentity stagingRootIdentity;
    WindowsFileIdentity transactionRootIdentity;
    const WindowsFilesystemBackendResult installOpened =
        backend->OpenRootExact(installPath.u8string(),
                               WindowsFilesystemRootRole::Install,
                               installRoot, installRootIdentity);
    checkNative(installOpened,
                "native invocation-cleanup namespace fixture opens install root");
    const WindowsFilesystemBackendResult stagingOpened =
        installOpened.ok()
            ? backend->OpenRootExact(stagingPath.u8string(),
                                     WindowsFilesystemRootRole::Staging,
                                     stagingRoot, stagingRootIdentity)
            : installOpened;
    checkNative(stagingOpened,
                "native invocation-cleanup namespace fixture opens staging root");
    const WindowsFilesystemBackendResult transactionOpened =
        stagingOpened.ok()
            ? backend->OpenRootExact(transactionPath.u8string(),
                                     WindowsFilesystemRootRole::Transaction,
                                     transactionRoot, transactionRootIdentity)
            : stagingOpened;
    checkNative(transactionOpened,
                "native invocation-cleanup namespace fixture opens transaction root");
    const bool rootsBound = installOpened.ok() && stagingOpened.ok() &&
        transactionOpened.ok();
    check(rootsBound,
          "native invocation-cleanup namespace fixture binds exact roots");
    if (!rootsBound) return;

    WindowsFilesystemObservation stagingFile;
    WindowsFilesystemObservation runtimePairDirectory;
    WindowsFilesystemObservation bootstrapFile;
    const WindowsFilesystemBackendResult stagingObserved =
        backend->ObserveNoFollow(stagingRoot, "HarkenSearch.exe", stagingFile);
    checkNative(stagingObserved,
                "native invocation-cleanup namespace fixture observes staging residue");
    const WindowsFilesystemBackendResult runtimePairObserved =
        stagingObserved.ok()
            ? backend->ObserveNoFollow(transactionRoot, "runtime-pair",
                                       runtimePairDirectory)
            : stagingObserved;
    const WindowsFilesystemBackendResult bootstrapObserved =
        runtimePairObserved.ok()
            ? backend->ObserveNoFollow(transactionRoot,
                                       "runtime-pair/HarkenSearchBootstrap.exe",
                                       bootstrapFile)
            : runtimePairObserved;
    checkNative(bootstrapObserved,
                "native invocation-cleanup namespace fixture observes runtime residue");
    const bool residueBound =
        stagingObserved.ok() &&
        stagingFile.kind == WindowsFilesystemEntryKind::RegularFile &&
        stagingFile.identity.valid && stagingFile.content.valid &&
        runtimePairObserved.ok() &&
        runtimePairDirectory.kind == WindowsFilesystemEntryKind::Directory &&
        runtimePairDirectory.identity.valid &&
        bootstrapObserved.ok() &&
        bootstrapFile.kind == WindowsFilesystemEntryKind::RegularFile &&
        bootstrapFile.identity.valid && bootstrapFile.content.valid;
    check(residueBound,
          "native invocation-cleanup namespace fixture binds exact residue identity and content");
    if (!residueBound) return;

    WindowsFileIdentity journalIdentity;
    WindowsFileContentFingerprint journalContent;
    WindowsFilesystemHandle journalLease = 0;
    const WindowsFilesystemBackendResult journalPersisted =
        backend->PersistJournalDurable(
            transactionRoot, {1U, 2U, 3U},
            WindowsFilesystemReplaceMode::CreateNew, {}, {},
            journalIdentity, journalContent);
    checkNative(journalPersisted,
                "native invocation-cleanup namespace fixture persists its journal");
    const WindowsFilesystemBackendResult leaseAcquired =
        journalPersisted.ok()
            ? backend->AcquireJournalMutationLease(
                  transactionRoot, journalIdentity, journalContent,
                  journalLease)
            : journalPersisted;
    checkNative(leaseAcquired,
                "native invocation-cleanup namespace fixture acquires its journal lease");
    const bool journalReady = journalPersisted.ok() && leaseAcquired.ok();
    check(journalReady,
          "native invocation-cleanup namespace fixture holds the durable journal lease");
    if (!journalReady) return;
    check(writeNativeFixtureFile(runtimePairPath / "unknown.txt", "unknown"),
          "native invocation-cleanup namespace fixture adds an unknown child after journal binding");

    WindowsBootstrapInvocationCleanup invocationCleanup;
    invocationCleanup.runtimePairExpected = true;
    invocationCleanup.runtimePairDirectoryIdentity =
        runtimePairDirectory.identity;
    invocationCleanup.runtimePairBootstrapIdentity = bootstrapFile.identity;
    invocationCleanup.runtimePairBootstrapContent = bootstrapFile.content;
    invocationCleanup.stagingFiles.push_back(
        {"HarkenSearch.exe", stagingFile.identity, stagingFile.content});
    invocationCleanup.transactionFiles.push_back(
        {"runtime-pair/HarkenSearchBootstrap.exe", bootstrapFile.identity,
         bootstrapFile.content});
    const WindowsFilesystemBackendResult cleaned =
        backend->CleanupInvocationResidueNoFollow(
            stagingRoot, transactionRoot, invocationCleanup);
    check(cleaned.status ==
              WindowsFilesystemBackendStatus::DirectoryNotEmpty,
          "unknown runtime-pair child blocks invocation cleanup");
    check(readNativeFixtureFile(stagingPath / "HarkenSearch.exe") == "staging-owned" &&
              readNativeFixtureFile(runtimePairPath / "HarkenSearchBootstrap.exe") ==
                  "bootstrap-owned" &&
              readNativeFixtureFile(runtimePairPath / "unknown.txt") ==
                  "unknown",
           "runtime-pair namespace rejection occurs before any known residue is deleted");

    backend->ReleaseJournalMutationLease(journalLease);
    backend->CloseFilesystemHandle(transactionRoot);
    backend->CloseFilesystemHandle(stagingRoot);
    backend->CloseFilesystemHandle(installRoot);
#endif
}

void test_native_invocation_cleanup_rejects_runtime_pair_identity_swap() {
#if defined(_WIN32)
    NativeSandbox sandbox;
    NativeSandboxCleanupAssertion cleanupAssertion(
        sandbox, "invocation-cleanup-identity-swap");
    const std::filesystem::path base = NativeSandbox::systemTempRoot();
    for (std::uint32_t attempt = 0; attempt < 32 && !sandbox.owned; ++attempt) {
        sandbox.path = base /
            ("aris-bootstrap-invocation-cleanup-swap-" +
             std::to_string(GetCurrentProcessId()) + "-" +
             std::to_string(attempt));
        std::error_code createError;
        sandbox.owned = std::filesystem::create_directory(sandbox.path,
                                                          createError);
    }
    check(sandbox.owned,
          "native invocation-cleanup identity-swap sandbox is exclusively created");
    if (!sandbox.owned) return;

    const std::filesystem::path installPath = sandbox.path / "install";
    const std::filesystem::path updaterPath = sandbox.path / "updater";
    const std::filesystem::path stagingPath = updaterPath / "staging";
    const std::filesystem::path transactionPath = updaterPath / "transaction";
    const std::filesystem::path runtimePairPath =
        transactionPath / "runtime-pair";
    const std::filesystem::path replacementPath =
        transactionPath / "runtime-pair-replacement";
    std::error_code directoryError;
    const bool fixtureCreated =
        std::filesystem::create_directory(installPath, directoryError) &&
        std::filesystem::create_directory(updaterPath, directoryError) &&
        std::filesystem::create_directory(stagingPath, directoryError) &&
        std::filesystem::create_directory(transactionPath, directoryError) &&
        std::filesystem::create_directory(runtimePairPath, directoryError) &&
        writeNativeFixtureFile(stagingPath / "HarkenSearch.exe", "staging-owned") &&
        writeNativeFixtureFile(runtimePairPath / "HarkenSearchBootstrap.exe",
                               "bootstrap-owned");
    check(fixtureCreated,
          "native invocation-cleanup identity-swap fixture creates owned residue and a replacement directory");
    if (!fixtureCreated) return;

    NativeRuntimePairSwapProbe swapProbe;
    swapProbe.transactionRoot = transactionPath;
    WindowsBootstrapFilesystemNativeTestHooks hooks;
    hooks.context = &swapProbe;
    hooks.beforeTargetMutation = &attemptNativeRuntimePairSwap;
    auto backend = createAuthorizedNativeBackend(
        installPath, stagingPath, transactionPath, hooks);
    check(backend != nullptr,
          "native invocation-cleanup identity-swap backend is authorized");
    if (backend == nullptr) return;

    WindowsFilesystemHandle installRoot = 0;
    WindowsFilesystemHandle stagingRoot = 0;
    WindowsFilesystemHandle transactionRoot = 0;
    WindowsFileIdentity installRootIdentity;
    WindowsFileIdentity stagingRootIdentity;
    WindowsFileIdentity transactionRootIdentity;
    const WindowsFilesystemBackendResult installOpened =
        backend->OpenRootExact(installPath.u8string(),
                               WindowsFilesystemRootRole::Install,
                               installRoot, installRootIdentity);
    checkNative(installOpened,
                "native invocation-cleanup identity-swap fixture opens install root");
    const WindowsFilesystemBackendResult stagingOpened =
        installOpened.ok()
            ? backend->OpenRootExact(stagingPath.u8string(),
                                     WindowsFilesystemRootRole::Staging,
                                     stagingRoot, stagingRootIdentity)
            : installOpened;
    checkNative(stagingOpened,
                "native invocation-cleanup identity-swap fixture opens staging root");
    const WindowsFilesystemBackendResult transactionOpened =
        stagingOpened.ok()
            ? backend->OpenRootExact(transactionPath.u8string(),
                                     WindowsFilesystemRootRole::Transaction,
                                     transactionRoot, transactionRootIdentity)
            : stagingOpened;
    checkNative(transactionOpened,
                "native invocation-cleanup identity-swap fixture opens transaction root");
    const bool rootsBound = installOpened.ok() && stagingOpened.ok() &&
        transactionOpened.ok();
    check(rootsBound,
          "native invocation-cleanup identity-swap fixture binds exact roots");
    if (!rootsBound) return;

    WindowsFilesystemObservation stagingFile;
    WindowsFilesystemObservation runtimePairDirectory;
    WindowsFilesystemObservation bootstrapFile;
    const WindowsFilesystemBackendResult stagingObserved =
        backend->ObserveNoFollow(stagingRoot, "HarkenSearch.exe", stagingFile);
    checkNative(stagingObserved,
                "native invocation-cleanup identity-swap fixture observes staging residue");
    const WindowsFilesystemBackendResult runtimePairObserved =
        stagingObserved.ok()
            ? backend->ObserveNoFollow(transactionRoot, "runtime-pair",
                                       runtimePairDirectory)
            : stagingObserved;
    const WindowsFilesystemBackendResult bootstrapObserved =
        runtimePairObserved.ok()
            ? backend->ObserveNoFollow(transactionRoot,
                                       "runtime-pair/HarkenSearchBootstrap.exe",
                                       bootstrapFile)
            : runtimePairObserved;
    checkNative(bootstrapObserved,
                "native invocation-cleanup identity-swap fixture observes runtime residue");
    const bool residueBound =
        stagingObserved.ok() &&
        stagingFile.identity.valid && stagingFile.content.valid &&
        runtimePairObserved.ok() &&
        runtimePairDirectory.kind == WindowsFilesystemEntryKind::Directory &&
        runtimePairDirectory.identity.valid &&
        bootstrapObserved.ok() &&
        bootstrapFile.identity.valid && bootstrapFile.content.valid;
    check(residueBound,
          "native invocation-cleanup identity-swap fixture binds original residue");
    if (!residueBound) return;

    WindowsFileIdentity journalIdentity;
    WindowsFileContentFingerprint journalContent;
    WindowsFilesystemHandle journalLease = 0;
    const WindowsFilesystemBackendResult journalPersisted =
        backend->PersistJournalDurable(
            transactionRoot, {4U, 5U, 6U},
            WindowsFilesystemReplaceMode::CreateNew, {}, {},
            journalIdentity, journalContent);
    checkNative(journalPersisted,
                "native invocation-cleanup identity-swap fixture persists its journal");
    const WindowsFilesystemBackendResult leaseAcquired =
        journalPersisted.ok()
            ? backend->AcquireJournalMutationLease(
                  transactionRoot, journalIdentity, journalContent,
                  journalLease)
            : journalPersisted;
    checkNative(leaseAcquired,
                "native invocation-cleanup identity-swap fixture acquires its journal lease");
    const bool journalReady = journalPersisted.ok() && leaseAcquired.ok();
    check(journalReady,
          "native invocation-cleanup identity-swap fixture holds the durable journal lease");
    if (!journalReady) return;
    check(std::filesystem::create_directory(replacementPath, directoryError),
          "native invocation-cleanup identity-swap fixture creates the replacement after journal binding");

    WindowsBootstrapInvocationCleanup invocationCleanup;
    invocationCleanup.runtimePairExpected = true;
    invocationCleanup.runtimePairDirectoryIdentity =
        runtimePairDirectory.identity;
    invocationCleanup.runtimePairBootstrapIdentity = bootstrapFile.identity;
    invocationCleanup.runtimePairBootstrapContent = bootstrapFile.content;
    invocationCleanup.stagingFiles.push_back(
        {"HarkenSearch.exe", stagingFile.identity, stagingFile.content});
    invocationCleanup.transactionFiles.push_back(
        {"runtime-pair/HarkenSearchBootstrap.exe", bootstrapFile.identity,
         bootstrapFile.content});
    const WindowsFilesystemBackendResult cleaned =
        backend->CleanupInvocationResidueNoFollow(
            stagingRoot, transactionRoot, invocationCleanup);
    check(swapProbe.called && swapProbe.swapped,
          "native invocation-cleanup identity-swap seam replaces the preflighted directory");
    check(cleaned.status == WindowsFilesystemBackendStatus::IdentityMismatch,
          "runtime-pair directory identity swap fails closed");
    check(readNativeFixtureFile(stagingPath / "HarkenSearch.exe") == "staging-owned" &&
              readNativeFixtureFile(runtimePairPath / "HarkenSearchBootstrap.exe") ==
                  "bootstrap-owned",
          "runtime-pair identity swap is rejected before staging or replacement residue is deleted");

    backend->ReleaseJournalMutationLease(journalLease);
    backend->CloseFilesystemHandle(transactionRoot);
    backend->CloseFilesystemHandle(stagingRoot);
    backend->CloseFilesystemHandle(installRoot);
#endif
}

void test_native_invocation_cleanup_rechecks_parent_and_metadata() {
#if defined(_WIN32)
    NativeSandbox sandbox;
    NativeSandboxCleanupAssertion cleanupAssertion(
        sandbox, "invocation-cleanup-parent-metadata");
    const std::filesystem::path base = NativeSandbox::systemTempRoot();
    for (std::uint32_t attempt = 0; attempt < 32 && !sandbox.owned; ++attempt) {
        sandbox.path = base /
            ("aris-bootstrap-invocation-cleanup-parent-metadata-" +
             std::to_string(GetCurrentProcessId()) + "-" +
             std::to_string(attempt));
        std::error_code createError;
        sandbox.owned = std::filesystem::create_directory(sandbox.path,
                                                          createError);
    }
    check(sandbox.owned,
          "native invocation-cleanup parent/metadata sandbox is exclusively created");
    if (!sandbox.owned) return;

    const std::filesystem::path installPath = sandbox.path / "install";
    const std::filesystem::path updaterPath = sandbox.path / "updater";
    const std::filesystem::path stagingPath = updaterPath / "staging";
    const std::filesystem::path transactionPath = updaterPath / "transaction";
    const std::filesystem::path nestedPath = stagingPath / "nested";
    const std::filesystem::path filePath = nestedPath / "HarkenSearch.exe";
    const std::filesystem::path movedParent = stagingPath / "nested-moved";
    std::error_code directoryError;
    const bool fixtureCreated =
        std::filesystem::create_directory(installPath, directoryError) &&
        std::filesystem::create_directory(updaterPath, directoryError) &&
        std::filesystem::create_directory(stagingPath, directoryError) &&
        std::filesystem::create_directory(transactionPath, directoryError) &&
        std::filesystem::create_directory(nestedPath, directoryError) &&
        writeNativeFixtureFile(filePath, "staging-owned");
    check(fixtureCreated,
          "native invocation-cleanup parent/metadata fixture creates its owned file");
    if (!fixtureCreated) return;

    auto backend = createAuthorizedNativeBackend(
        installPath, stagingPath, transactionPath);
    check(backend != nullptr,
          "native invocation-cleanup parent/metadata backend is authorized");
    if (backend == nullptr) return;

    WindowsFilesystemHandle installRoot = 0;
    WindowsFilesystemHandle stagingRoot = 0;
    WindowsFilesystemHandle transactionRoot = 0;
    WindowsFileIdentity installIdentity;
    WindowsFileIdentity stagingIdentity;
    WindowsFileIdentity transactionIdentity;
    const auto installOpened = backend->OpenRootExact(
        installPath.u8string(), WindowsFilesystemRootRole::Install,
        installRoot, installIdentity);
    const auto stagingOpened = installOpened.ok()
        ? backend->OpenRootExact(stagingPath.u8string(),
                                 WindowsFilesystemRootRole::Staging,
                                 stagingRoot, stagingIdentity)
        : installOpened;
    const auto transactionOpened = stagingOpened.ok()
        ? backend->OpenRootExact(transactionPath.u8string(),
                                 WindowsFilesystemRootRole::Transaction,
                                 transactionRoot, transactionIdentity)
        : stagingOpened;
    check(installOpened.ok() && stagingOpened.ok() && transactionOpened.ok(),
          "native invocation-cleanup parent/metadata fixture binds exact roots");
    if (!installOpened.ok() || !stagingOpened.ok() || !transactionOpened.ok()) {
        return;
    }

    WindowsFilesystemObservation observed;
    const auto fileObserved = backend->ObserveNoFollow(
        stagingRoot, "nested/HarkenSearch.exe", observed);
    check(fileObserved.ok() &&
              observed.kind == WindowsFilesystemEntryKind::RegularFile &&
              observed.identity.valid && observed.content.valid,
          "native invocation-cleanup parent/metadata fixture observes its exact file");
    if (!fileObserved.ok() ||
        observed.kind != WindowsFilesystemEntryKind::RegularFile ||
        !observed.identity.valid || !observed.content.valid) {
        return;
    }

    WindowsFileIdentity journalIdentity;
    WindowsFileContentFingerprint journalContent;
    WindowsFilesystemHandle journalLease = 0;
    const auto journalPersisted = backend->PersistJournalDurable(
        transactionRoot, {7U, 8U, 9U}, WindowsFilesystemReplaceMode::CreateNew,
        {}, {}, journalIdentity, journalContent);
    const auto leaseAcquired = journalPersisted.ok()
        ? backend->AcquireJournalMutationLease(
              transactionRoot, journalIdentity, journalContent, journalLease)
        : journalPersisted;
    check(journalPersisted.ok() && leaseAcquired.ok(),
          "native invocation-cleanup parent/metadata fixture holds its journal lease");
    if (!journalPersisted.ok() || !leaseAcquired.ok()) return;

    WindowsBootstrapInvocationCleanup cleanup;
    cleanup.allowAlreadyRemoved = true;
    cleanup.stagingFiles.push_back(
        {"nested/HarkenSearch.exe", observed.identity, observed.content});
    directoryError.clear();
    std::filesystem::rename(nestedPath, movedParent, directoryError);
    check(!directoryError,
          "native invocation-cleanup parent/metadata fixture moves the containing directory");
    const auto missingParent = backend->CleanupInvocationResidueNoFollow(
        stagingRoot, transactionRoot, cleanup);
    check(missingParent.status == WindowsFilesystemBackendStatus::IdentityMismatch,
          "native invocation cleanup rejects a missing containing directory");
    check(readNativeFixtureFile(movedParent / "HarkenSearch.exe") == "staging-owned",
          "native missing-parent rejection preserves the moved exact file");

    directoryError.clear();
    std::filesystem::rename(movedParent, nestedPath, directoryError);
    check(!directoryError,
          "native invocation-cleanup parent/metadata fixture restores the containing directory");
    const std::filesystem::path hardLink = nestedPath / "Aris-link.exe";
    check(CreateHardLinkW(hardLink.c_str(), filePath.c_str(), nullptr) != FALSE,
          "native invocation-cleanup parent/metadata fixture creates a hard link");
    cleanup.allowAlreadyRemoved = false;
    const auto metadataDrift = backend->CleanupInvocationResidueNoFollow(
        stagingRoot, transactionRoot, cleanup);
    check(metadataDrift.status == WindowsFilesystemBackendStatus::UnsupportedMetadata,
          "native invocation cleanup rejects hard-link metadata drift");
    check(readNativeFixtureFile(filePath) == "staging-owned" &&
              std::filesystem::exists(hardLink),
          "native metadata rejection preserves the exact staging file");

    backend->ReleaseJournalMutationLease(journalLease);
    backend->CloseFilesystemHandle(transactionRoot);
    backend->CloseFilesystemHandle(stagingRoot);
    backend->CloseFilesystemHandle(installRoot);
#endif
}

void test_native_named_stream_metadata_fails_closed_before_mutation() {
#if defined(_WIN32)
    NativeSandbox sandbox;
    NativeSandboxCleanupAssertion cleanup(sandbox, "named-stream");
    const std::filesystem::path base = NativeSandbox::systemTempRoot();
    for (std::uint32_t attempt = 0; attempt < 32 && !sandbox.owned; ++attempt) {
        sandbox.path = base /
            ("aris-bootstrap-named-stream-" +
             std::to_string(GetCurrentProcessId()) + "-" +
             std::to_string(attempt));
        std::error_code createError;
        sandbox.owned = std::filesystem::create_directory(sandbox.path,
                                                          createError);
    }
    check(sandbox.owned,
          "native named-stream sandbox is exclusively created");
    if (!sandbox.owned) return;
    const std::filesystem::path installPath = sandbox.path / "install";
    const std::filesystem::path stagingPath = sandbox.path / "staging";
    const std::filesystem::path transactionPath = sandbox.path / "transaction";
    std::error_code directoryError;
    const bool fixtureCreated =
        std::filesystem::create_directory(installPath, directoryError) &&
        std::filesystem::create_directory(stagingPath, directoryError) &&
        std::filesystem::create_directory(transactionPath, directoryError) &&
        writeNativeFixtureFile(stagingPath / "HarkenSearch.exe", "named-stream");
    check(fixtureCreated,
          "native named-stream fixture creates its owned roots");
    if (!fixtureCreated) return;
    const std::wstring streamPath =
        (stagingPath / "HarkenSearch.exe").native() + L":blocked";
    HANDLE stream = CreateFileW(
        streamPath.c_str(), GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    check(stream != INVALID_HANDLE_VALUE,
          "native named-stream fixture publishes an alternate stream");
    if (stream != INVALID_HANDLE_VALUE) {
        const char marker = 'x';
        DWORD written = 0;
        static_cast<void>(WriteFile(stream, &marker, 1, &written, nullptr));
        CloseHandle(stream);
    }
    auto backend = CreateNativeWindowsBootstrapFilesystemBackend();
    check(backend != nullptr,
          "native named-stream backend is available");
    if (backend == nullptr) return;
    WindowsFilesystemHandle installRoot = 0;
    WindowsFilesystemHandle stagingRoot = 0;
    WindowsFileIdentity installIdentity;
    WindowsFileIdentity stagingIdentity;
    check(backend->OpenRootExact(installPath.u8string(),
                                 WindowsFilesystemRootRole::Install,
                                 installRoot, installIdentity).ok(),
          "native named-stream fixture binds install root");
    const WindowsFilesystemBackendResult rootOpened =
        backend->OpenRootExact(stagingPath.u8string(),
                               WindowsFilesystemRootRole::Staging,
                               stagingRoot, stagingIdentity);
    checkNative(rootOpened,
                "native named-stream fixture binds install root");
    WindowsFilesystemObservation observation;
    const WindowsFilesystemBackendResult observed =
        backend->ObserveNoFollow(stagingRoot, "HarkenSearch.exe", observation);
    check(observed.status == WindowsFilesystemBackendStatus::UnsupportedMetadata,
          "native named stream is rejected before any filesystem mutation");
    check(DeleteFileW(streamPath.c_str()) != FALSE,
          "native named-stream fixture removes its rejected stream");
    const std::filesystem::path hardLinkPath = stagingPath / "Aris-link.exe";
    check(CreateHardLinkW(hardLinkPath.c_str(),
                          (stagingPath / "HarkenSearch.exe").c_str(), nullptr) != FALSE,
          "native unsupported-metadata fixture creates a second hard link");
    WindowsFilesystemObservation hardLinkedObservation;
    const WindowsFilesystemBackendResult hardLinkObserved =
        backend->ObserveNoFollow(stagingRoot, "HarkenSearch.exe",
                                 hardLinkedObservation);
    check(hardLinkObserved.status ==
              WindowsFilesystemBackendStatus::UnsupportedMetadata,
          "native hard-linked file is rejected before any filesystem mutation");
    backend->CloseFilesystemHandle(stagingRoot);
    backend->CloseFilesystemHandle(installRoot);
#endif
}

#if defined(_WIN32)
void test_native_nonzero_ea_observe_and_copy_no_longer_rejected() {
    NativeSandbox sandbox;
    NativeSandboxCleanupAssertion cleanup(sandbox, "nonzero-ea");
    const std::filesystem::path base = NativeSandbox::systemTempRoot();
    for (std::uint32_t attempt = 0; attempt < 32U && !sandbox.owned;
         ++attempt) {
        sandbox.path = base /
            ("aris-bootstrap-nonzero-ea-" +
             std::to_string(GetCurrentProcessId()) + "-" +
             std::to_string(attempt));
        std::error_code createError;
        sandbox.owned = std::filesystem::create_directory(sandbox.path,
                                                           createError);
    }
    check(sandbox.owned,
          "native nonzero-EA sandbox is exclusively created");
    if (!sandbox.owned) return;

    const std::filesystem::path installPath = sandbox.path / "install";
    const std::filesystem::path updaterPath = sandbox.path / "updater";
    const std::filesystem::path stagingPath = updaterPath / "staging";
    const std::filesystem::path transactionPath = updaterPath / "transaction";
    std::error_code directoryError;
    const bool fixtureCreated =
        std::filesystem::create_directory(installPath, directoryError) &&
        std::filesystem::create_directory(updaterPath, directoryError) &&
        std::filesystem::create_directory(stagingPath, directoryError) &&
        std::filesystem::create_directory(transactionPath, directoryError) &&
        writeNativeFixtureFile(installPath / "HarkenSearch.exe", "old") &&
        writeNativeFixtureFile(stagingPath / "HarkenSearch.exe", "new");
    check(fixtureCreated,
          "native nonzero-EA fixture creates its owned roots");
    if (!fixtureCreated) return;

    check(setNativeEaFile(stagingPath / "HarkenSearch.exe", "aris.test", "ea"),
          "native nonzero-EA fixture sets EA on the staging file");
    check(setNativeEaFile(installPath / "HarkenSearch.exe", "aris.test", "ea"),
          "native nonzero-EA fixture sets EA on the install file");

    auto backend = createAuthorizedNativeBackend(installPath, stagingPath,
                                                  transactionPath);
    check(backend != nullptr, "native nonzero-EA backend is authorized");
    if (backend == nullptr) return;

    WindowsFilesystemHandle installRoot = 0;
    WindowsFilesystemHandle stagingRoot = 0;
    WindowsFilesystemHandle transactionRoot = 0;
    WindowsFileIdentity ignored;
    const WindowsFilesystemBackendResult installOpened =
        backend->OpenRootExact(installPath.u8string(),
                               WindowsFilesystemRootRole::Install,
                               installRoot, ignored);
    const WindowsFilesystemBackendResult stagingOpened =
        installOpened.ok()
            ? backend->OpenRootExact(stagingPath.u8string(),
                                     WindowsFilesystemRootRole::Staging,
                                     stagingRoot, ignored)
            : installOpened;
    const WindowsFilesystemBackendResult transactionOpened =
        stagingOpened.ok()
            ? backend->OpenRootExact(transactionPath.u8string(),
                                     WindowsFilesystemRootRole::Transaction,
                                     transactionRoot, ignored)
            : stagingOpened;
    check(installOpened.ok() && stagingOpened.ok() && transactionOpened.ok(),
          "native nonzero-EA fixture opens its owned roots");
    if (!installOpened.ok() || !stagingOpened.ok() ||
        !transactionOpened.ok()) {
        return;
    }

    WindowsFilesystemObservation stagingObservation;
    const WindowsFilesystemBackendResult stagingObserved =
        backend->ObserveNoFollow(stagingRoot, "HarkenSearch.exe",
                                 stagingObservation);
    check(stagingObserved.ok() &&
              stagingObservation.kind ==
                  WindowsFilesystemEntryKind::RegularFile &&
              stagingObservation.identity.valid &&
              stagingObservation.content.valid,
          "native staging file with non-zero EA is observed (no longer 282)");

    WindowsFilesystemObservation installObservation;
    const WindowsFilesystemBackendResult installObserved =
        backend->ObserveNoFollow(installRoot, "HarkenSearch.exe",
                                 installObservation);
    check(installObserved.ok() &&
              installObservation.kind ==
                  WindowsFilesystemEntryKind::RegularFile &&
              installObservation.identity.valid &&
              installObservation.content.valid,
          "native install file with non-zero EA is observed (no longer 282)");
    if (!installObserved.ok()) {
        backend->CloseFilesystemHandle(transactionRoot);
        backend->CloseFilesystemHandle(stagingRoot);
        backend->CloseFilesystemHandle(installRoot);
        return;
    }

    const std::vector<std::uint8_t> journalBytes{0xEAU};
    WindowsFileIdentity journalIdentity;
    WindowsFileContentFingerprint journalContent;
    const WindowsFilesystemBackendResult journalPersisted =
        backend->PersistJournalDurable(
            transactionRoot, journalBytes,
            WindowsFilesystemReplaceMode::CreateNew, {}, {},
            journalIdentity, journalContent);
    check(journalPersisted.ok() && journalIdentity.valid &&
              journalContent.valid,
          "native nonzero-EA fixture persists its journal");
    if (!journalPersisted.ok()) {
        backend->CloseFilesystemHandle(transactionRoot);
        backend->CloseFilesystemHandle(stagingRoot);
        backend->CloseFilesystemHandle(installRoot);
        return;
    }

    WindowsFilesystemHandle lease = 0;
    const WindowsFilesystemBackendResult leaseAcquired =
        backend->AcquireJournalMutationLease(transactionRoot, journalIdentity,
                                             journalContent, lease);
    check(leaseAcquired.ok() && lease != 0,
          "native nonzero-EA fixture acquires its journal lease");
    if (!leaseAcquired.ok()) {
        backend->CloseFilesystemHandle(transactionRoot);
        backend->CloseFilesystemHandle(stagingRoot);
        backend->CloseFilesystemHandle(installRoot);
        return;
    }

    WindowsFilesystemHandle backupRoot = 0;
    WindowsFileIdentity backupIdentity;
    const WindowsFilesystemBackendResult backupOpened =
        backend->OpenOrCreateDirectoryNoFollow(transactionRoot, "backup", true,
                                               backupRoot, backupIdentity);
    check(backupOpened.ok(),
          "native nonzero-EA fixture creates its backup root");
    if (!backupOpened.ok()) {
        backend->ReleaseJournalMutationLease(lease);
        backend->CloseFilesystemHandle(transactionRoot);
        backend->CloseFilesystemHandle(stagingRoot);
        backend->CloseFilesystemHandle(installRoot);
        return;
    }

    WindowsFileIdentity copiedIdentity;
    WindowsFileContentFingerprint copiedContent;
    WindowsFileMetadataSnapshot copiedMetadata;
    const WindowsFilesystemBackendResult copied =
        backend->CopyFileDurableNoFollow(
            installRoot, "HarkenSearch.exe", installObservation.identity,
            backupRoot, "HarkenSearch.exe", copiedIdentity, copiedContent,
            copiedMetadata);
    check(copied.ok(),
          "native copy of a non-zero EA source is no longer rejected");

    if (backupRoot != 0) backend->CloseFilesystemHandle(backupRoot);
    if (lease != 0) backend->ReleaseJournalMutationLease(lease);
    backend->CloseFilesystemHandle(transactionRoot);
    backend->CloseFilesystemHandle(stagingRoot);
    backend->CloseFilesystemHandle(installRoot);
}
#endif

#if defined(_WIN32)
int runSelfCleanupGuardChild(const char* installArgument,
                             const char* stagingArgument,
                             const char* transactionArgument) {
    const std::filesystem::path installPath =
        std::filesystem::u8path(installArgument);
    const std::filesystem::path stagingPath =
        std::filesystem::u8path(stagingArgument);
    const std::filesystem::path transactionPath =
        std::filesystem::u8path(transactionArgument);
    auto backend = createAuthorizedNativeBackend(
        installPath, stagingPath, transactionPath);
    if (backend == nullptr) return 10;

    WindowsFilesystemHandle installRoot = 0;
    WindowsFilesystemHandle stagingRoot = 0;
    WindowsFilesystemHandle transactionRoot = 0;
    WindowsFileIdentity ignoredInstall;
    WindowsFileIdentity ignoredStaging;
    WindowsFileIdentity ignoredTransaction;
    const auto installOpened = backend->OpenRootExact(
        installPath.u8string(), WindowsFilesystemRootRole::Install,
        installRoot, ignoredInstall);
    const auto stagingOpened = installOpened.ok()
        ? backend->OpenRootExact(stagingPath.u8string(),
                                 WindowsFilesystemRootRole::Staging,
                                 stagingRoot, ignoredStaging)
        : installOpened;
    const auto transactionOpened = stagingOpened.ok()
        ? backend->OpenRootExact(transactionPath.u8string(),
                                 WindowsFilesystemRootRole::Transaction,
                                 transactionRoot, ignoredTransaction)
        : stagingOpened;
    if (!installOpened.ok() || !stagingOpened.ok() ||
        !transactionOpened.ok()) {
        return 11;
    }

    WindowsFilesystemObservation stagingFile;
    WindowsFilesystemObservation runtimePairDirectory;
    WindowsFilesystemObservation bootstrapFile;
    const auto stagingObserved = backend->ObserveNoFollow(
        stagingRoot, "HarkenSearch.exe", stagingFile);
    const auto runtimePairObserved = stagingObserved.ok()
        ? backend->ObserveNoFollow(transactionRoot, "runtime-pair",
                                   runtimePairDirectory)
        : stagingObserved;
    const auto bootstrapObserved = runtimePairObserved.ok()
        ? backend->ObserveNoFollow(transactionRoot,
                                   "runtime-pair/HarkenSearchBootstrap.exe",
                                   bootstrapFile)
        : runtimePairObserved;
    if (!stagingObserved.ok() || !runtimePairObserved.ok() ||
        !bootstrapObserved.ok() || !stagingFile.identity.valid ||
        !stagingFile.content.valid || !runtimePairDirectory.identity.valid ||
        !bootstrapFile.identity.valid || !bootstrapFile.content.valid) {
        return 12;
    }

    WindowsFileIdentity journalIdentity;
    WindowsFileContentFingerprint journalContent;
    const auto journalPublished = backend->PersistJournalDurable(
        transactionRoot, {0x41U, 0x42U, 0x43U},
        WindowsFilesystemReplaceMode::CreateNew, {}, {}, journalIdentity,
        journalContent);
    WindowsFilesystemHandle lease = 0;
    const auto leaseAcquired = journalPublished.ok()
        ? backend->AcquireJournalMutationLease(transactionRoot, journalIdentity,
                                               journalContent, lease)
        : journalPublished;
    if (!journalPublished.ok() || !leaseAcquired.ok()) return 13;

    WindowsBootstrapInvocationCleanup cleanup;
    cleanup.runtimePairExpected = true;
    cleanup.runtimePairDirectoryIdentity = runtimePairDirectory.identity;
    cleanup.runtimePairBootstrapIdentity = bootstrapFile.identity;
    cleanup.runtimePairBootstrapContent = bootstrapFile.content;
    cleanup.stagingFiles.push_back(
        {"HarkenSearch.exe", stagingFile.identity, stagingFile.content});
    cleanup.transactionFiles.push_back(
        {"runtime-pair/HarkenSearchBootstrap.exe", bootstrapFile.identity,
         bootstrapFile.content});
    const auto result = backend->CleanupInvocationResidueNoFollow(
        stagingRoot, transactionRoot, cleanup);
    const bool preserved =
        std::filesystem::exists(transactionPath / "runtime-pair" /
                                "HarkenSearchBootstrap.exe") &&
        std::filesystem::exists(stagingPath / "HarkenSearch.exe");
    backend->ReleaseJournalMutationLease(lease);
    backend->CloseFilesystemHandle(transactionRoot);
    backend->CloseFilesystemHandle(stagingRoot);
    backend->CloseFilesystemHandle(installRoot);
    if (result.status == WindowsFilesystemBackendStatus::AccessDenied &&
        result.nativeError == ERROR_ACCESS_DENIED && preserved) {
        return 0;
    }
    return 20 + static_cast<int>(result.status);
}

void test_native_invocation_cleanup_rejects_current_bootstrap_image_before_delete() {
    NativeSandbox sandbox;
    NativeSandboxCleanupAssertion cleanup(sandbox, "self-image-guard");
    const std::filesystem::path base = NativeSandbox::systemTempRoot();
    for (std::uint32_t attempt = 0; attempt < 32U && !sandbox.owned;
         ++attempt) {
        sandbox.path = base /
            ("aris-bootstrap-self-image-" +
             std::to_string(GetCurrentProcessId()) + "-" +
             std::to_string(attempt));
        std::error_code error;
        sandbox.owned = std::filesystem::create_directory(sandbox.path, error);
    }
    check(sandbox.owned, "self-image guard sandbox is exclusively created");
    if (!sandbox.owned) return;

    const auto installPath = sandbox.path / "install";
    const auto updaterPath = sandbox.path / "updater";
    const auto stagingPath = updaterPath / "staging";
    const auto transactionPath = updaterPath / "transaction";
    const auto runtimePairPath = transactionPath / "runtime-pair";
    std::error_code error;
    check(std::filesystem::create_directories(installPath, error) &&
              std::filesystem::create_directory(updaterPath, error) &&
              std::filesystem::create_directory(stagingPath, error) &&
              std::filesystem::create_directory(transactionPath, error) &&
              std::filesystem::create_directory(runtimePairPath, error) &&
              writeNativeFixtureFile(stagingPath / "HarkenSearch.exe", "staging") ,
          "self-image guard fixture creates its roots");
    if (error) return;

    wchar_t executableBuffer[MAX_PATH]{};
    const DWORD executableLength = GetModuleFileNameW(
        nullptr, executableBuffer,
        static_cast<DWORD>(std::size(executableBuffer)));
    check(executableLength != 0U &&
              executableLength < std::size(executableBuffer),
          "self-image guard fixture resolves its test executable");
    if (executableLength == 0U ||
        executableLength >= std::size(executableBuffer)) return;
    const std::filesystem::path bootstrapPath =
        runtimePairPath / "HarkenSearchBootstrap.exe";
    std::filesystem::copy_file(
        std::filesystem::path(executableBuffer), bootstrapPath,
        std::filesystem::copy_options::overwrite_existing, error);
    check(!error, "self-image guard fixture copies the child image exactly");
    if (error) return;

    const std::string command =
        "\"" + bootstrapPath.u8string() +
        "\" --bootstrap-child-self-cleanup \"" + installPath.u8string() +
        "\" \"" + stagingPath.u8string() + "\" \"" +
        transactionPath.u8string() + "\"";
    std::vector<char> mutableCommand(command.begin(), command.end());
    mutableCommand.push_back('\0');
    STARTUPINFOA startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    const BOOL created = CreateProcessA(
        nullptr, mutableCommand.data(), nullptr, nullptr, FALSE,
        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process);
    check(created != FALSE, "self-image guard fixture starts the exact bootstrap image");
    if (created == FALSE) return;
    const ChildProcessWaitOutcome outcome =
        waitForChildProcessBounded(process, 5000U);
    check(outcome.completed && outcome.exitCode == 0,
          "current bootstrap image is rejected before durable deletion");
    check(std::filesystem::exists(bootstrapPath),
          "self-image guard preserves the current bootstrap image");
    check(std::filesystem::exists(stagingPath / "HarkenSearch.exe"),
          "self-image guard preserves staging residue for the successor");
}
#endif

}  // namespace

void test_missing_install_parent_is_admitted_without_mutation() {
    const Fixture values = fixture();
    ScriptedBackend backend;
    backend.set(WindowsFilesystemRootRole::Install, "resources",
                WindowsFilesystemEntryKind::Missing, {});
    WindowsBootstrapFilesystemPort port(backend);
    check(port.Begin(values.plan, values.journal, roots()).ok(),
          "a signed new file may have a missing install parent");
    check(!hasMutation(backend),
          "missing parent admission does not mutate before the durable journal");
    for (const auto kind : {WindowsFilesystemEntryKind::RegularFile,
                            WindowsFilesystemEntryKind::ReparsePoint}) {
        ScriptedBackend conflicting;
        conflicting.set(WindowsFilesystemRootRole::Install, "resources", kind, identity(93));
        WindowsBootstrapFilesystemPort rejected(conflicting);
        check(!rejected.Begin(values.plan, values.journal, roots()).ok() &&
                  !hasMutation(conflicting),
              "new directory support still rejects same-name files and reparse points");
    }
    auto pending = journalRecord(values);
    const auto added = std::find_if(pending.items.begin(), pending.items.end(),
        [](const auto& item) { return item.relativePath == "resources/core.pak"; });
    check(added != pending.items.end(), "pending-parent codec test has a new nested item");
    if (added == pending.items.end()) return;
    added->installParentIdentity = {};
    check(EncodeWindowsBootstrapJournal(pending).empty(),
          "schema 9 retains its original bound-parent requirement");
    pending.schemaVersion = 10U;
    const auto bytes = EncodeWindowsBootstrapJournal(pending);
    check(!bytes.empty() && DecodeWindowsBootstrapJournal(
              bytes, &values.plan, &values.journal).ok(),
          "schema 10 persists and recovers an unbound new-file parent");
    added->replacementStarted = true;
    check(EncodeWindowsBootstrapJournal(pending).empty(),
          "no journal may start file replacement with an unbound parent");
}

void test_native_missing_install_parents_publish_and_rollback() {
#if defined(_WIN32)
    for (unsigned scenario = 0; scenario < 4U; ++scenario) {
        const bool rollback = scenario == 1U;
        NativeSandbox sandbox;
        NativeSandboxCleanupAssertion cleanup(sandbox, "new-install-parents");
        const auto base = NativeSandbox::systemTempRoot();
        for (unsigned attempt = 0; attempt < 32U && !sandbox.owned; ++attempt) {
            sandbox.path = base / ("harken-new-parents-" +
                std::to_string(GetCurrentProcessId()) + "-" +
                std::to_string(attempt));
            std::error_code error;
            sandbox.owned = std::filesystem::create_directory(sandbox.path, error);
        }
        check(sandbox.owned, "new-parent test owns an isolated directory");
        if (!sandbox.owned) return;
        const auto installPath = sandbox.path / "install";
        const auto stagingPath = sandbox.path / "updater" / "staging";
        const auto transactionPath = sandbox.path / "updater" / "transaction";
        const std::string asset = "assets/icons/new-contributors/cindy.png";
        std::filesystem::create_directories(installPath / "user-data");
        std::filesystem::create_directories(stagingPath / "assets/icons/new-contributors");
        std::filesystem::create_directories(transactionPath);
        check(writeNativeFixtureFile(installPath / "HarkenSearch.exe", "old-native") &&
                  writeNativeFixtureFile(installPath / "user-data/keep.txt", "user-owned") &&
                  writeNativeFixtureFile(stagingPath / "HarkenSearch.exe", "new-native") &&
                  writeNativeFixtureFile(stagingPath / asset, "new-logo"),
              "new-parent test creates the old version and staged payload");
        SignedInstallPlan raw = rawPlan();
        rebindDeploymentIdentity(raw, installPath.u8string());
        raw.invocation.updaterOwnedBase = stagingPath.parent_path().u8string();
        raw.invocation.stagingRoot = stagingPath.u8string();
        raw.invocation.transactionRoot = transactionPath.u8string();
        raw.inventory = {{"HarkenSearch.exe", BootstrapSha256Hex("new-native"), 10U},
                         {asset, BootstrapSha256Hex("new-logo"), 8U}};
        const auto validated = ValidateSignedInstallPlan(raw, SignatureStatus::Verified);
        check(validated.ok(), "new-parent signed fixture plan validates");
        if (!validated.ok()) return;
        const auto journal = BuildReplacementJournal(
            *validated.plan, {{"HarkenSearch.exe"}}, raw.invocation.transactionId);
        check(journal.ok(), "new-parent journal classifies the added asset");
        if (!journal.ok()) return;
        const WindowsBootstrapFilesystemRoots trusted{
            stagingPath.u8string(), transactionPath.u8string(),
            raw.invocation.updaterOwnedBase, raw.invocation.invocationIdentity};
        WindowsBootstrapFilesystemNativeTestHooks hooks;
        if (scenario == 2U) {
            hooks.accessDeniedFailurePoint = WindowsBootstrapFilesystemNativeTestFailurePoint::
                InstallDirectoryBeforeCreate;
        } else if (scenario == 3U) {
            hooks.accessDeniedFailurePoint = WindowsBootstrapFilesystemNativeTestFailurePoint::
                InstallDirectoryAfterCreateBeforeObservation;
        }
        auto backend = createAuthorizedNativeBackend(installPath, stagingPath, transactionPath, hooks);
        check(backend != nullptr, "new-parent native backend is authorized");
        if (!backend) return;
        WindowsFilesystemHandle installHandle = 0;
        WindowsFilesystemHandle unleasedHandle = 0;
        WindowsFileIdentity installIdentity;
        WindowsFileIdentity unleasedIdentity;
        check(backend->OpenRootExact(installPath.u8string(), WindowsFilesystemRootRole::Install,
                                    installHandle, installIdentity).ok(),
              "directory authorization test binds the install root");
        check(!backend->OpenOrCreateDirectoryNoFollow(installHandle, "unleased", true,
                    unleasedHandle, unleasedIdentity).ok() &&
                  !std::filesystem::exists(installPath / "unleased"),
              "install directories cannot be created without the durable journal lease");
        if (unleasedHandle != 0) backend->CloseFilesystemHandle(unleasedHandle);
        backend->CloseFilesystemHandle(installHandle);
        auto port = std::make_unique<WindowsBootstrapFilesystemPort>(*backend);
        const auto begun = port->Begin(*validated.plan, *journal.journal, trusted);
        check(begun.ok(), "native admission accepts three missing parent directories");
        check(!std::filesystem::exists(installPath / "assets"),
              "native admission is read-only before the journal");
        if (!begun.ok()) return;
        const auto prepared = port->Apply(intent(BootstrapIntentKind::PersistJournal, {}, "prepared"));
        if (scenario >= 2U) {
            const auto expected = scenario == 2U
                ? WindowsBootstrapFilesystemStatus::RetryableAccessDenied
                : WindowsBootstrapFilesystemStatus::PartialDurableState;
            check(prepared.status == expected &&
                      !std::filesystem::exists(installPath / asset) &&
                      readNativeFixtureFile(installPath / "HarkenSearch.exe") == "old-native",
                  "directory creation cuts preserve the old installation before file mutation");
        } else {
            check(prepared.ok(), "native preparation creates missing install directories durably");
            check(std::filesystem::is_directory(installPath / "assets/icons/new-contributors") &&
                  !std::filesystem::exists(installPath / asset) &&
                  readNativeFixtureFile(installPath / "HarkenSearch.exe") == "old-native",
              "parents exist before any old or new payload file is changed");
            if (!prepared.ok()) return;
        }
        // A fresh backend must decode the journal, bind the created parent,
        // and finish the same transaction without any pre-created fixture path.
        port.reset();
        backend.reset();
        backend = createAuthorizedNativeBackend(installPath, stagingPath, transactionPath);
        check(backend != nullptr, "restart reopens the authorized roots");
        if (!backend) return;
        port = std::make_unique<WindowsBootstrapFilesystemPort>(*backend);
        const auto recovery = port->BeginRecovery(*validated.plan, *journal.journal, trusted);
        check(recovery.filesystem.ok() &&
                  recovery.disposition == WindowsBootstrapRecoveryDisposition::ContinueForward,
              "a fresh updater resumes after creating the parent directories");
        if (!recovery.filesystem.ok()) return;
        for (const auto& action : recovery.intents) {
            const auto applied = port->Apply(action);
            check(applied.ok(), "native resumed update publishes each signed file");
            if (!applied.ok()) return;
        }
        check(readNativeFixtureFile(installPath / asset) == "new-logo" &&
                  readNativeFixtureFile(installPath / "HarkenSearch.exe") == "new-native",
              "native update publishes both the executable and new nested asset");
        if (rollback) {
            check(port->Apply(intent(BootstrapIntentKind::PersistJournal, {}, "rolling-back")).ok() &&
                      port->Apply(intent(BootstrapIntentKind::RestoreBackup, "HarkenSearch.exe")).ok() &&
                      port->Apply(intent(BootstrapIntentKind::RemoveNewItem, asset)).ok() &&
                      port->Apply(intent(BootstrapIntentKind::RemoveBackup)).ok(),
                  "native rollback restores old files and removes the new nested file");
            check(readNativeFixtureFile(installPath / "HarkenSearch.exe") == "old-native" &&
                      !std::filesystem::exists(installPath / asset) &&
                      std::filesystem::is_directory(installPath / "assets/icons/new-contributors"),
                  "rollback preserves reusable directories without retaining new payload files");
        } else {
            check(checkpointHealthyChildForCommit(*port) &&
                      port->Apply(intent(BootstrapIntentKind::CommitJournal)).ok() &&
                      port->Apply(intent(BootstrapIntentKind::RemoveBackup)).ok(),
                  "native successful update commits and removes its backups");
        }
        check(readNativeFixtureFile(installPath / "user-data/keep.txt") == "user-owned",
              "both success and rollback preserve unrelated user data");
    }
#endif
}

int main(int argc, char** argv) {
    if (argc == 2 && std::strcmp(argv[1], "--only-missing-install-parent") == 0) {
        test_missing_install_parent_is_admitted_without_mutation();
        test_native_missing_install_parents_publish_and_rollback();
        return failures == 0 ? 0 : 1;
    }
#if defined(_WIN32)
    if (argc == 2 &&
        std::strcmp(argv[1], "--bootstrap-child-wait-timeout") == 0) {
        return runWaitTimeoutChild();
    }
    if (argc == 5 &&
        std::strcmp(argv[1], "--bootstrap-child-self-cleanup") == 0) {
        return runSelfCleanupGuardChild(argv[2], argv[3], argv[4]);
    }
    if (argc == 5 && std::strcmp(argv[1], "--bootstrap-child-journal-temp") == 0) {
        return runJournalTempCrashChild(argv[2], argv[3], argv[4], false);
    }
    if (argc == 5 &&
        std::strcmp(argv[1],
                    "--bootstrap-child-journal-temp-before-directory") == 0) {
        return runJournalTempCrashChild(argv[2], argv[3], argv[4], true);
    }
    if (argc == 5 &&
        std::strcmp(argv[1], "--bootstrap-child-target-atomic") == 0) {
        return runTargetAtomicCrashChild(argv[2], argv[3], argv[4]);
    }
    if (argc == 5 &&
        std::strcmp(argv[1], "--bootstrap-child-target-prepared") == 0) {
        return runTargetPreparedCrashChild(argv[2], argv[3], argv[4]);
    }
    if (argc == 5 &&
        std::strcmp(argv[1], "--bootstrap-child-journal-slot") == 0) {
        return runJournalSlotCrashChild(argv[2], argv[3], argv[4]);
    }
    if (argc == 7 &&
        std::strcmp(argv[1], "--bootstrap-child-delete-cut") == 0) {
        return runDeleteCrashChild(argv[2], argv[3], argv[4], argv[5],
                                   argv[6]);
    }
    if (argc == 2 && std::strcmp(argv[1], "--only-native-publish") == 0) {
        test_native_backend_publishes_inside_synthetic_sandbox();
        return failures == 0 ? 0 : 1;
    }
    if (argc == 2 &&
        std::strcmp(argv[1], "--only-native-post-publish-metadata") == 0) {
        test_native_replacement_journals_observed_post_publish_change_time();
        return failures == 0 ? 0 : 1;
    }
    if (argc == 2 &&
        std::strcmp(argv[1], "--only-native-recovery-change-time") == 0) {
        test_native_recovery_allows_observed_change_time_only();
        return failures == 0 ? 0 : 1;
    }
    if (argc == 2 &&
        std::strcmp(argv[1], "--only-native-metadata-apply") == 0) {
        test_native_metadata_apply_accepts_windows_change_time_normalization();
        return failures == 0 ? 0 : 1;
    }
    if (argc == 2 &&
        std::strcmp(argv[1], "--only-native-label-equivalence") == 0) {
        test_native_mandatory_label_equivalence_ignores_empty_provenance();
        return failures == 0 ? 0 : 1;
    }
    if (argc == 2 &&
        std::strcmp(argv[1], "--only-native-wait-timeout") == 0) {
        test_native_child_wait_timeout_terminates_and_closes();
        return failures == 0 ? 0 : 1;
    }
    if (argc == 2 && std::strcmp(argv[1], "--only-native-journal-temp") == 0) {
        test_native_restart_reopens_journal_temp_from_separate_process();
        return failures == 0 ? 0 : 1;
    }
    if (argc == 2 && std::strcmp(argv[1], "--only-native-target") == 0) {
        test_native_restart_observes_atomic_target_from_separate_process();
        return failures == 0 ? 0 : 1;
    }
    if (argc == 2 && std::strcmp(argv[1], "--only-native-prepared") == 0) {
        test_native_restart_resumes_prepared_target_from_separate_process();
        return failures == 0 ? 0 : 1;
    }
    if (argc == 2 && std::strcmp(argv[1], "--only-native-slot") == 0) {
        test_native_restart_selects_higher_journal_generation_after_slot_cut();
        return failures == 0 ? 0 : 1;
    }
    if (argc == 2 && std::strcmp(argv[1], "--only-native-root") == 0) {
        test_native_root_binding_denies_external_rename();
        return failures == 0 ? 0 : 1;
    }
    if (argc == 2 && std::strcmp(argv[1], "--only-native-parent") == 0) {
        test_native_nested_parent_is_pinned_through_target_mutation();
        return failures == 0 ? 0 : 1;
    }
    if (argc == 2 && std::strcmp(argv[1], "--only-native-atomic") == 0) {
        test_native_atomic_replacement_has_no_delete_rename_gap();
        return failures == 0 ? 0 : 1;
    }
    if (argc == 2 &&
        std::strcmp(argv[1], "--only-native-checkpoint-target-swap") == 0) {
        test_native_target_swap_after_prepared_checkpoint_is_rejected();
        return failures == 0 ? 0 : 1;
    }
    if (argc == 2 &&
        std::strcmp(argv[1], "--only-native-delete-cuts") == 0) {
        test_native_delete_cut_points_fail_durability_closed();
        return failures == 0 ? 0 : 1;
    }
    if (argc == 2 &&
        std::strcmp(argv[1], "--only-native-delete-restart") == 0) {
        test_native_restart_replays_delete_cuts_from_separate_process();
        return failures == 0 ? 0 : 1;
    }
    if (argc == 2 && std::strcmp(argv[1], "--only-native-backup") == 0) {
        test_native_backup_post_publication_access_denied_is_durability_failure();
        test_native_backup_directory_access_denied_phase_classification();
        return failures == 0 ? 0 : 1;
    }
    if (argc == 2 && std::strcmp(argv[1], "--only-native-reparse") == 0) {
        test_native_empty_parent_cannot_be_reparsed_during_create_new();
        return failures == 0 ? 0 : 1;
    }
    if (argc == 2 &&
        std::strcmp(argv[1], "--only-native-invocation-cleanup") == 0) {
        test_native_invocation_cleanup_durably_removes_authorized_residue();
        test_native_invocation_cleanup_preflights_runtime_pair_namespace();
        test_native_invocation_cleanup_rejects_runtime_pair_identity_swap();
        test_native_invocation_cleanup_rechecks_parent_and_metadata();
        return failures == 0 ? 0 : 1;
    }
    if (argc == 2 && std::strcmp(argv[1], "--only-native-stream") == 0) {
        test_native_named_stream_metadata_fails_closed_before_mutation();
        return failures == 0 ? 0 : 1;
    }
    if (argc == 2 && std::strcmp(argv[1], "--only-native-ea") == 0) {
        test_native_nonzero_ea_observe_and_copy_no_longer_rejected();
        return failures == 0 ? 0 : 1;
    }
    if (argc == 2 &&
        std::strcmp(argv[1], "--only-native-self-image") == 0) {
        test_native_invocation_cleanup_rejects_current_bootstrap_image_before_delete();
        return failures == 0 ? 0 : 1;
    }
    if (argc == 2 &&
        std::strcmp(argv[1], "--only-trusted-root-acl") == 0) {
        test_native_trusted_root_rejects_broad_write_acl();
        return failures == 0 ? 0 : 1;
    }
    if (argc == 2 && std::strcmp(argv[1], "--only-journal-flags") == 0) {
        test_journal_rejects_impossible_progress_flags();
        test_journal_accepts_backend_normalized_acl_metadata();
        return failures == 0 ? 0 : 1;
    }
    if (argc == 2 && std::strcmp(argv[1], "--only-lifecycle-flags") == 0) {
        test_journal_rejects_impossible_runtime_lifecycle_authority();
        return failures == 0 ? 0 : 1;
    }
    if (argc == 2 &&
        std::strcmp(argv[1], "--only-backup-namespace") == 0) {
        test_restart_rejects_unknown_backup_child_before_journal_promotion();
        test_restart_rechecks_backup_namespace_before_promotion();
        test_restart_revalidates_backup_from_bound_handle();
        return failures == 0 ? 0 : 1;
    }
    if (argc == 2 && std::strcmp(argv[1], "--only-codec") == 0) {
        test_strict_journal_codec_and_durable_winner_selection();
        test_schema_nine_persists_runtime_pair_cleanup_authority();
        return failures == 0 ? 0 : 1;
    }
    if (argc == 2 &&
        std::strcmp(argv[1], "--only-failure-count") == 0) {
        test_record_failure_advances_the_authenticated_durable_count();
        return failures == 0 ? 0 : 1;
    }
    if (argc == 2 &&
        std::strcmp(argv[1], "--only-persist-retry") == 0) {
        test_transient_journal_access_denial_retries_inside_persist();
        return failures == 0 ? 0 : 1;
    }
    if (argc == 2 &&
        std::strcmp(argv[1], "--only-recovered-failure-count") == 0) {
        test_restart_restores_the_authenticated_failure_count();
        return failures == 0 ? 0 : 1;
    }
    if (argc == 2 &&
        std::strcmp(argv[1], "--only-runtime-checkpoint") == 0) {
        test_runtime_lifecycle_checkpoint_survives_fresh_recovery();
        return failures == 0 ? 0 : 1;
    }
    if (argc == 2 &&
        std::strcmp(argv[1], "--only-committed-relaunch") == 0) {
        test_committed_health_relaunch_survives_cleanup_restart();
        return failures == 0 ? 0 : 1;
    }
    if (argc == 2 && std::strcmp(argv[1], "--only-rollback-checkpoints") == 0) {
        test_restart_recovery_resumes_after_durable_rollback_checkpoints();
        return failures == 0 ? 0 : 1;
    }
    if (argc == 2 && std::strcmp(argv[1], "--only-write-ahead") == 0) {
        test_write_ahead_journal_wraps_backup_and_replacement();
        return failures == 0 ? 0 : 1;
    }
    if (argc == 2 && std::strcmp(argv[1], "--only-signed-removals") == 0) {
        test_signed_removal_deletes_restores_and_recovers_idempotently();
        return failures == 0 ? 0 : 1;
    }
    if (argc == 2 && std::strcmp(argv[1], "--only-parent-rebind") == 0) {
        test_recovery_rebinds_parent_pid_to_same_image_identity();
        test_recovery_promotes_temp_before_parent_rebound_publication();
        return failures == 0 ? 0 : 1;
    }
    if (argc == 2 && std::strcmp(argv[1], "--only-lease") == 0) {
        test_started_journal_is_pinned_through_each_mutation();
        test_journal_lease_access_denied_is_retryable_before_mutation();
        test_backup_directory_create_denial_is_retryable_before_creation();
        test_remove_new_access_denied_is_retryable_before_deletion();
        return failures == 0 ? 0 : 1;
    }
    if (argc == 2 &&
        std::strcmp(argv[1], "--only-invocation-cleanup") == 0) {
        test_invocation_cleanup_rejects_missing_nested_parent();
        test_invocation_cleanup_accepts_missing_nested_leaf_with_parent();
        test_invocation_cleanup_rechecks_supported_metadata();
        test_runtime_pair_authority_is_rechecked_before_journal_persist();
    test_committed_invocation_cleanup_is_terminal_and_identity_bound();
    test_absent_runtime_pair_authority_blocks_late_injection();
    test_pre_mutation_cleanup_access_denied_is_retryable();
    test_pre_mutation_access_denied_can_retry_remove_backup();
    test_invocation_cleanup_retries_after_completion_persist_failure();
        test_terminal_retirement_requires_invocation_cleanup();
        test_terminal_cleanup_binds_inherited_runtime_pair_parent();
        test_rollback_terminal_retirement_requires_invocation_cleanup();
        test_fresh_retirement_rechecks_recreated_invocation_residue();
        test_fresh_recovery_rejects_recreated_runtime_pair_before_mutation();
        return failures == 0 ? 0 : 1;
    }
    if (argc == 2 &&
        std::strcmp(argv[1], "--only-terminal-journal-read-retry") == 0) {
        test_terminal_cleanup_retries_transient_journal_read_denial();
        test_terminal_cleanup_rehydrates_when_handoff_journal_is_stale();
        return failures == 0 ? 0 : 1;
    }
    if (argc == 2 &&
        std::strcmp(argv[1], "--only-terminal-retirement-namespace") == 0) {
        test_terminal_retirement_rechecks_backup_namespace_before_delete();
        return failures == 0 ? 0 : 1;
    }
#endif
    test_strict_journal_codec_and_durable_winner_selection();
    test_schema_nine_persists_runtime_pair_cleanup_authority();
    test_terminal_cleanup_binds_inherited_runtime_pair_parent();
    test_terminal_cleanup_retries_transient_journal_read_denial();
    test_terminal_cleanup_rehydrates_when_handoff_journal_is_stale();
    test_schema6_persists_anchored_recovery_binding();
    test_record_failure_advances_the_authenticated_durable_count();
    test_restart_restores_the_authenticated_failure_count();
    test_durable_recovery_admission_requires_authenticated_authority();
    test_journal_rejects_impossible_progress_flags();
    test_journal_accepts_backend_normalized_acl_metadata();
    test_journal_rejects_impossible_runtime_lifecycle_authority();
    test_restart_rebind_recovers_atomic_publish_before_next_generation();
    test_runtime_lifecycle_checkpoint_survives_fresh_recovery();
    test_committed_health_relaunch_survives_cleanup_restart();
    test_restart_rejects_same_content_unknown_target_identity();
    test_restart_rejects_unknown_backup_child_before_journal_promotion();
    test_restart_rechecks_backup_namespace_before_promotion();
    test_restart_revalidates_backup_from_bound_handle();
    test_restart_rebinds_only_the_exact_journal_bound_forward_temp();
    test_restart_rebinds_only_the_exact_journal_bound_restore_temp();
    test_restart_rejects_stray_restore_temp_before_forward_completion();
    test_restart_rejects_unbound_same_content_backup_after_started_cut();
    test_restart_from_prepared_state_can_apply_its_first_forward_intent();
    test_restart_reconciles_started_cleanup_after_namespace_removal();
    test_forged_write_roots_are_rejected_before_any_open();
    test_failed_recovery_poison_blocks_every_later_mutation();
    test_untyped_journal_reason_never_reaches_durable_publication();
    test_restart_recovery_emits_idempotent_rollback_for_complete_uncommitted_publish();
    test_restart_recovery_resumes_after_durable_rollback_checkpoints();
    test_exact_root_and_reparse_fail_before_mutation();
    test_write_ahead_journal_wraps_backup_and_replacement();
    test_signed_removal_deletes_restores_and_recovers_idempotently();
    test_recovery_rebinds_parent_pid_to_same_image_identity();
    test_recovery_promotes_temp_before_parent_rebound_publication();
    test_mutation_requires_initial_durable_journal();
    test_started_journal_is_pinned_through_each_mutation();
    test_journal_lease_access_denied_is_retryable_before_mutation();
    test_backup_directory_create_denial_is_retryable_before_creation();
    test_remove_new_access_denied_is_retryable_before_deletion();
    test_inventory_aliases_reserve_forward_and_restore_temporary_names();
    test_post_mutation_flush_failure_blocks_further_writes();
    test_pre_mutation_access_denial_can_retry_the_same_backup();
    test_pre_mutation_access_denial_can_retry_the_same_replacement();
    test_pre_mutation_access_denial_can_retry_the_same_restore();
    test_pre_mutation_access_denied_can_retry_remove_backup();
    test_transient_journal_access_denial_retries_inside_persist();
    test_pre_mutation_flush_and_ordering_gates_emit_no_content_write();
    test_unknown_target_and_existing_backup_are_refused();
    test_rollback_is_identity_bound_and_preserves_unknown_files();
    test_rollback_metadata_is_journal_bound_and_tamper_checked();
    test_root_alias_and_forward_identity_race_are_blocked();
    test_install_parent_identity_swap_is_blocked();
    test_create_new_race_and_non_filesystem_intent_fail_closed();
    test_temporary_publication_collision_never_overwrites_unknown_file();
    test_commit_precedes_cleanup_and_unknown_backup_blocks_only_cleanup();
    test_successful_commit_cleanup_replays_idempotently();
    test_committed_invocation_cleanup_is_terminal_and_identity_bound();
    test_invocation_cleanup_rejects_missing_nested_parent();
    test_invocation_cleanup_accepts_missing_nested_leaf_with_parent();
    test_invocation_cleanup_rechecks_supported_metadata();
    test_runtime_pair_authority_is_rechecked_before_journal_persist();
    test_absent_runtime_pair_authority_blocks_late_injection();
    test_invocation_cleanup_retries_after_completion_persist_failure();
    test_terminal_retirement_requires_invocation_cleanup();
    test_rollback_terminal_retirement_requires_invocation_cleanup();
    test_fresh_retirement_rechecks_recreated_invocation_residue();
    test_terminal_journal_retirement_allows_a_fresh_transaction();
    test_terminal_retirement_rechecks_backup_namespace_before_delete();
    test_terminal_journal_retirement_recovers_after_inactive_slot_removal();
    test_terminal_journal_retirement_retries_pre_mutation_access_denied();
    test_fresh_recovery_rejects_recreated_runtime_pair_before_mutation();
    test_restart_rejects_recreated_backup_after_terminal_cleanup();
    test_terminal_state_rejects_new_journal_phases();
    test_backup_only_rollback_becomes_terminal_and_cleans_backup();
    test_missing_install_parent_is_admitted_without_mutation();
    test_native_missing_install_parents_publish_and_rollback();
    test_native_backend_factory_loads_without_touching_install();
#if defined(_WIN32)
    test_native_child_wait_timeout_terminates_and_closes();
    test_native_no_policy_factory_is_read_only();
    test_native_trusted_root_rejects_broad_write_acl();
#endif
    test_native_basic_info_comparison_includes_last_access();
    test_native_mandatory_label_equivalence_ignores_empty_provenance();
    test_native_backend_publishes_inside_synthetic_sandbox();
    test_native_replacement_journals_observed_post_publish_change_time();
    test_native_recovery_allows_observed_change_time_only();
    test_native_metadata_apply_accepts_windows_change_time_normalization();
#if defined(_WIN32)
    test_native_legacy_journal_names_fail_closed();
#endif
    test_native_port_retargets_the_journal_lease_during_prepared_checkpoint();
    test_native_restart_reopens_journal_temp_from_separate_process();
    test_native_restart_observes_atomic_target_from_separate_process();
    test_native_restart_resumes_prepared_target_from_separate_process();
    test_native_restart_selects_higher_journal_generation_after_slot_cut();
    test_native_root_binding_denies_external_rename();
    test_native_nested_parent_is_pinned_through_target_mutation();
    test_native_atomic_replacement_has_no_delete_rename_gap();
    test_native_target_swap_after_prepared_checkpoint_is_rejected();
    test_native_delete_cut_points_fail_durability_closed();
    test_native_restart_replays_delete_cuts_from_separate_process();
    test_native_backup_post_publication_access_denied_is_durability_failure();
    test_native_backup_directory_access_denied_phase_classification();
    test_native_empty_parent_cannot_be_reparsed_during_create_new();
    test_native_invocation_cleanup_preflights_runtime_pair_namespace();
    test_native_invocation_cleanup_rejects_runtime_pair_identity_swap();
    test_native_invocation_cleanup_rechecks_parent_and_metadata();
    test_native_named_stream_metadata_fails_closed_before_mutation();
    test_native_nonzero_ea_observe_and_copy_no_longer_rejected();
    test_native_invocation_cleanup_rejects_current_bootstrap_image_before_delete();
    return failures == 0 ? 0 : 1;
}
