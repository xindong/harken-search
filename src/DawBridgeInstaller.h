#pragma once

#include "ReaperBridgeStatus.h"
#include "Localization.h"  // H1b: LedgerText key carriers (RQ-20260911-04)

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace HarkenSearch::DawBridge {

// @stage7-contract
// API contract: installer helpers resolve and validate bridge paths/options;
// process and file ownership remain with the implementation. Results preserve
// explicit resolution/error states and do not retain caller strings.

enum class ReaperBridgeTargetResolution {
    Unresolved,
    ExplicitPluginDirectory,
    RunningPortableInstall,
    ConfiguredPortableInstall,
    StandardResourcePath,
};

const char* ReaperBridgeTargetResolutionKey(ReaperBridgeTargetResolution resolution);

enum class ReaperBridgeDoctorAction { Diagnose, Repair, Update, Verify };
const char* ReaperBridgeDoctorActionKey(ReaperBridgeDoctorAction action);

enum class ReaperBridgeInstallCheckpoint {
    Staged,
    BackupCreated,
    ReplacementCommitted,
    Verified,
    RolledBack,
};

using ReaperBridgeInstallObserver =
    std::function<bool(ReaperBridgeInstallCheckpoint checkpoint)>;

struct InstallStatus {
    InstallState state = InstallState::Unknown;
    ReaperBridgeTargetResolution targetResolution =
        ReaperBridgeTargetResolution::Unresolved;
    std::string configuredRootPath;
    std::string resourcePath;
    std::string targetDirectory;
    std::string hostExecutablePath;
    std::string alternateInstalledPath;
    std::string installedPath;
    std::string bundledPath;
    std::string installedVersion;
    std::string expectedVersion;
    std::string installedArchitecture;
    std::string hostArchitecture;
    std::string installedSha256;
    std::string bundledSha256;
    std::string installedManifest;
    std::string expectedManifest;
    LedgerText detail;       // The installer worker returns keyed text for UI rendering.
    LedgerText remediation;  // H1b
    std::string handshakeCode;
    std::string handshakeDetail;
    int installedProtocolVersion = 0;
    int expectedProtocolVersion = ARIS_REAPER_BRIDGE_PROTOCOL_VERSION;
    int handshakeProtocolVersion = 0;
    int handshakeActionId = 0;
    bool configured = false;
    bool configuredPathExists = false;
    bool hostInstalled = false;
    bool targetResolved = false;
    bool bundledFilePresent = false;
    bool binaryIntegrityOk = false;
    bool trustedSource = false;
    bool bridgeLoaded = false;
    bool protocolCompatible = false;
    bool sendChannelAvailable = false;
    bool filePresent = false;
    bool alternateFilePresent = false;
    bool legacyFilePresent = false;
    bool correctLocation = false;
    bool hostRunning = false;
    bool handshakeOk = false;
    bool restartRequired = false;
};

struct InstallResult {
    bool ok = false;
    bool changed = false;
    bool binaryVerified = false;
    bool restartRequired = false;
    bool rollbackPerformed = false;
    bool rollbackVerified = false;
    std::string installedPath;
    std::string backupPath;
    LedgerText detail;       // The UI renders this keyed installation result.
    std::string failureCode;
};

struct BridgeBinaryInfo {
    bool exists = false;
    bool trustedSource = false;
    bool legacySignature = false;
    std::string path;
    std::string version;
    std::string architecture;
    std::string sha256;
    std::string productName;
    std::string fileDescription;
    std::string manifest;
    int protocolVersion = 0;
    uint64_t sizeBytes = 0;
};

struct ReaperBridgeProbeOptions {
    std::string configuredReaperRootPath;
    std::string configuredReaperExePath; // backward-compatible input
    bool preferConfiguredReaperPath = true;
    std::string bundledDllPathOverride;
    std::string pluginDirectoryOverride;
    std::string alternatePluginPathOverride;
    std::optional<bool> hostRunningOverride;
    std::function<bool()> hostRunningQueryOverride;
    std::wstring pipeNameOverride;
    int handshakeTimeoutMs = 120;
    bool inspectLegacyComponent = false;
};

struct ReaperBridgeDoctorRequest {
    ReaperBridgeDoctorAction action = ReaperBridgeDoctorAction::Diagnose;
    ReaperBridgeProbeOptions probeOptions;
    bool dryRun = false;
    int recheckTimeoutMs = 0;
    int recheckIntervalMs = 100;
    bool waitForHostExit = false;
    int hostExitWaitTimeoutMs = 0;
    std::vector<std::string> verificationFilePaths;
    ReaperBridgeInstallObserver installObserver;
    bool installMissingOnly = false;
};

struct ReaperBridgeDoctorReport {
    ReaperBridgeDoctorAction action = ReaperBridgeDoctorAction::Diagnose;
    bool ok = false;
    bool changed = false;
    bool binaryVerified = false;
    bool handshakeVerified = false;
    bool sendVerified = false;
    bool restartRequired = false;
    int recheckAttempts = 0;
    InstallStatus before;
    InstallStatus after;
    InstallResult install;
    LedgerText summary;  // H1b: composed from keyed parts
    std::string failureCode;
    std::vector<std::string> steps;
};

struct Installer {
    std::string targetId;
    std::string displayName;
    std::string descriptionLocalizationKey;
    std::function<InstallStatus()> probe;
    std::function<InstallResult(bool dryRun)> install;
    bool requiresAdmin = false;
};

std::string AutoDetectReaperRootPath();
bool ReaperProcessDefinitelyStopped();
BridgeBinaryInfo ProbeBridgeBinary(const std::string& pathUtf8);
InstallStatus ProbeReaperBridge(const ReaperBridgeProbeOptions& options = {});
InstallResult InstallReaperBridge(
    const ReaperBridgeProbeOptions& options = {},
    bool dryRun = false,
    ReaperBridgeInstallObserver observer = {});
InstallResult InstallReaperBridgeBinary(
    const std::string& sourceDllUtf8,
    const std::string& targetDirectoryUtf8,
    bool hostRunning,
    bool dryRun = false,
    ReaperBridgeInstallObserver observer = {});
ReaperBridgeDoctorReport RunReaperBridgeDoctor(
    const ReaperBridgeDoctorRequest& request = {});
std::string BuildReaperBridgeDiagnosticSummary(
    const InstallStatus& status,
    bool redactUserProfile = true);

std::vector<Installer> BuildInstallerRegistry();

} // namespace HarkenSearch::DawBridge
