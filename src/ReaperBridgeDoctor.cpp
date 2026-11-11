#include "DawBridgeInstaller.h"
#if defined(__APPLE__)
#include "ReaperMacHostLayout.h"
#if __has_include("harken_reaper_build_digest.h")
#include "harken_reaper_build_digest.h"
#endif
#endif
#include "Localization.h"   // G1: L() ledger accessor (RQ-20260911-04)
#include <cstdio>

#include "ReaperBridgeClient.h"
#include "platform/executable_path.h"
#include "platform/harken_user_data_layout.h"
#include "platform/durable_file_ops.h"
#include "platform/file_hash.h"
#include "platform/filesystem.h"
#include "platform/pe_inspection.h"
#include "platform/system.h"
#include "platform/text_encoding.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <filesystem>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace HarkenSearch::DawBridge {
namespace {

std::mutex g_restartMutex;
std::wstring g_restartRequiredTarget;

std::wstring Utf8ToWideLocal(const std::string& value) {
    return Platform::TextEncoding::Utf8ToWideStrict(value);
}

std::string WideToUtf8Local(const std::wstring& value) {
    return Platform::TextEncoding::WideToUtf8Strict(value);
}

bool FileExists(const fs::path& path) {
    return Platform::FileSystem::ProbeRegularFile(path.u8string()).isRegularFile;
}

bool DirectoryExists(const fs::path& path) {
    return Platform::FileSystem::IsDirectory(path.u8string());
}

fs::path ExecutableDirectory() {
    const Platform::ExecutablePathResult executable = Platform::GetExecutablePath();
    if (executable.status != Platform::ExecutablePathStatus::Success ||
        executable.pathUtf8.empty()) {
        return {};
    }
    const std::wstring widePath = Utf8ToWideLocal(executable.pathUtf8);
    if (widePath.empty()) return {};
    return fs::path(widePath).parent_path();
}

fs::path BundledReaperPlugin() {
    const fs::path exe = ExecutableDirectory();
    const fs::path neu = exe / Platform::HarkenUserData::CanonicalReaperPluginFileName();
    if (FileExists(neu)) return neu;
    const fs::path old = exe / Platform::HarkenUserData::LegacyReaperPluginFileName();
    if (FileExists(old)) return old;
    return neu;
}

bool IsManagedReaperPluginName(const fs::path& path) {
    const std::string name = path.filename().u8string();
    return name == Platform::HarkenUserData::CanonicalReaperPluginFileName() ||
           name == Platform::HarkenUserData::LegacyReaperPluginFileName();
}



fs::path AppDataReaperDirectory() {
#if defined(__APPLE__)
    if (Platform::HarkenUserData::PreviewIsolationActive()) {
        const auto roots = Platform::HarkenUserData::ResolveRoots();
        if (roots.roamingAppData.empty()) return {};
        const fs::path resource = fs::path(roots.roamingAppData) / L"REAPER";
        if (!Platform::HarkenUserData::PreviewIsolationAllowsDataPathUtf8(resource.u8string()))
            return {};
        return resource;
    }
#endif
    const Platform::UserDataDirectoryResult appData =
        Platform::GetRoamingAppDataDirectory();
    if (appData.status != Platform::UserDataDirectoryStatus::Success ||
        appData.pathUtf8.empty()) {
        return {};
    }
    const std::wstring widePath = Utf8ToWideLocal(appData.pathUtf8);
    if (widePath.empty()) return {};
    return fs::path(widePath) / L"REAPER";
}

fs::path NormalizeReaperRoot(const std::string& rootOrExecutableUtf8) {
#if defined(__APPLE__)
    return ResolveMacReaperHostLayout(rootOrExecutableUtf8).bundle;
#else
    fs::path input(Utf8ToWideLocal(rootOrExecutableUtf8));
    if (input.empty()) return {};
    input = input.lexically_normal();
    const std::string filename = input.filename().u8string();
    if (Platform::TextEncoding::EqualsAsciiCaseInsensitive(filename, "reaper.exe")) {
        return input.parent_path();
    }
    return input;
#endif
}

bool SamePath(const fs::path& left, const fs::path& right) {
    std::wstring a = left.lexically_normal().wstring();
    std::wstring b = right.lexically_normal().wstring();
    std::transform(a.begin(), a.end(), a.begin(), towlower);
    std::transform(b.begin(), b.end(), b.begin(), towlower);
    return a == b;
}

std::wstring FindRunningReaperExecutable(const fs::path& preferredExecutable = {}, bool* stateKnown = nullptr) {
    if (stateKnown) *stateKnown = false;
    const std::string preferredUtf8 = preferredExecutable.empty()
        ? std::string()
        : WideToUtf8Local(preferredExecutable.wstring());
    if (!preferredExecutable.empty() && preferredUtf8.empty()) return {};
    const Platform::ProcessImageLookupResult result =
        Platform::FindProcessByImageName("reaper.exe", preferredUtf8);
    if (stateKnown) *stateKnown = result.status == Platform::ProcessImageLookupStatus::NotFound ||
        (result.status == Platform::ProcessImageLookupStatus::Found && !result.executablePathUtf8.empty());
    if (result.status != Platform::ProcessImageLookupStatus::Found ||
        result.executablePathUtf8.empty()) {
        return {};
    }
    return Utf8ToWideLocal(result.executablePathUtf8);
}

std::string ReadFileVersion(const fs::path& path) {
    const auto result = Platform::PeInspection::ReadFileVersion(path.u8string());
    return result.status == Platform::PeInspection::Status::Success ? result.value : std::string();
}

std::string ReadVersionString(const fs::path& path, const wchar_t* key) {
    const std::string keyUtf8 = WideToUtf8Local(key ? std::wstring(key) : std::wstring());
    if (key && keyUtf8.empty()) return {};
    const auto result = Platform::PeInspection::ReadVersionString(path.u8string(), keyUtf8);
    return result.status == Platform::PeInspection::Status::Success ? result.value : std::string();
}

std::string ReadPeArchitecture(const fs::path& path) {
    const Platform::PeInspection::StringResult result =
        Platform::PeInspection::ReadArchitecture(path.u8string());
    if (result.status != Platform::PeInspection::Status::Success) return {};
    return result.value;
}

std::string ComputeSha256(const fs::path& path) {
    const auto result = Platform::Crypto::ComputeSha256File(path.u8string());
    return result.status == Platform::Crypto::FileHashStatus::Success
        ? result.hexDigest : std::string();
}

bool HasReaperPluginEntry(const fs::path& path) {
    const auto result = Platform::PeInspection::ProbeExport(
        path.u8string(), "ReaperPluginEntry");
    return result.status == Platform::PeInspection::Status::Success && result.present;
}

struct ResolvedReaperTarget {
    bool configured = false;
    bool configuredPathExists = false;
    bool targetResolved = false;
    bool hostInstalled = false;
    bool hostRunning = false;
    bool hostStateKnown = false;
    ReaperBridgeTargetResolution resolution = ReaperBridgeTargetResolution::Unresolved;
    fs::path configuredRoot;
    fs::path resourcePath;
    fs::path hostExecutable;
    fs::path pluginDirectory;
    fs::path alternatePlugin;
    std::string hostArchitecture;
};

ResolvedReaperTarget ResolveReaperTarget(const ReaperBridgeProbeOptions& options) {
    ResolvedReaperTarget target;
    const std::string configuredInput = !options.configuredReaperRootPath.empty()
        ? options.configuredReaperRootPath
        : options.configuredReaperExePath;
    target.configured = !configuredInput.empty() || !options.pluginDirectoryOverride.empty();
    target.configuredRoot = NormalizeReaperRoot(configuredInput);
    target.configuredPathExists = !target.configuredRoot.empty() &&
                                  DirectoryExists(target.configuredRoot);
#if defined(__APPLE__)
    target.hostExecutable = ResolveMacReaperHostLayout(configuredInput).executable;
#else
    target.hostExecutable = target.configuredRoot / L"reaper.exe";
#endif
    target.hostInstalled = FileExists(target.hostExecutable);
    if (target.hostInstalled) target.hostArchitecture = ReadPeArchitecture(target.hostExecutable);

    const std::wstring runningExecutable = FindRunningReaperExecutable(target.hostExecutable, &target.hostStateKnown);
    target.hostRunning = !runningExecutable.empty();
    if (options.hostRunningQueryOverride) {
        target.hostRunning = options.hostRunningQueryOverride();
        target.hostStateKnown = true;
    } else if (options.hostRunningOverride.has_value()) {
        target.hostRunning = *options.hostRunningOverride;
        target.hostStateKnown = true;
    }

    if (!options.pluginDirectoryOverride.empty()) {
        target.pluginDirectory = fs::path(Utf8ToWideLocal(options.pluginDirectoryOverride));
        target.resourcePath = target.pluginDirectory.parent_path();
        target.alternatePlugin = fs::path(Utf8ToWideLocal(options.alternatePluginPathOverride));
        target.targetResolved = !target.pluginDirectory.empty();
        target.configured = true;
        target.configuredPathExists = true;
        target.hostInstalled = true;
        target.resolution = target.targetResolved
            ? ReaperBridgeTargetResolution::ExplicitPluginDirectory
            : ReaperBridgeTargetResolution::Unresolved;
        if (target.hostArchitecture.empty()) {
            target.hostArchitecture = ReaperBridge::CurrentArchitecture();
        }
        return target;
    }

    if (!target.hostInstalled) return target;
    const fs::path standardResource = AppDataReaperDirectory();
#if defined(__APPLE__)
    // Mac portable configuration lives beside the application bundle.
    const fs::path portableResource =
        ResolveMacReaperHostLayout(configuredInput).portableResourceDirectory;
#else
    const fs::path portableResource = target.configuredRoot;
#endif
    if (!portableResource.empty() && FileExists(portableResource / L"reaper.ini")) {
        target.resourcePath = portableResource;
        target.pluginDirectory = target.resourcePath / L"UserPlugins";
        target.alternatePlugin = standardResource / L"UserPlugins" /
            Platform::HarkenUserData::CanonicalReaperPluginFileName();
        target.targetResolved = true;
        target.resolution = target.hostRunning &&
                            SamePath(fs::path(runningExecutable), target.hostExecutable)
            ? ReaperBridgeTargetResolution::RunningPortableInstall
            : ReaperBridgeTargetResolution::ConfiguredPortableInstall;
    } else if (!standardResource.empty()) {
        target.resourcePath = standardResource;
        target.pluginDirectory = standardResource / L"UserPlugins";
        target.alternatePlugin = portableResource / L"UserPlugins" /
            Platform::HarkenUserData::CanonicalReaperPluginFileName();
        target.targetResolved = true;
        target.resolution = ReaperBridgeTargetResolution::StandardResourcePath;
    }
    return target;
}

const char* HandshakeCodeKey(ReaperBridge::HandshakeCode code) {
    switch (code) {
    case ReaperBridge::HandshakeCode::Ready: return "ready";
    case ReaperBridge::HandshakeCode::TransportUnavailable: return "transport_unavailable";
    case ReaperBridge::HandshakeCode::VersionMismatch: return "version_mismatch";
    case ReaperBridge::HandshakeCode::ProtocolMismatch: return "protocol_mismatch";
    case ReaperBridge::HandshakeCode::ArchitectureMismatch: return "architecture_mismatch";
    case ReaperBridge::HandshakeCode::ManifestMismatch: return "manifest_mismatch";
    case ReaperBridge::HandshakeCode::InvalidResponse: return "invalid_response";
    case ReaperBridge::HandshakeCode::ExtensionError: return "extension_error";
    default: return "unknown";
    }
}

fs::path UniqueTransactionPath(const fs::path& target, const wchar_t* suffix) {
    static std::atomic<unsigned long long> sequence{0};
    const auto id = sequence.fetch_add(1, std::memory_order_relaxed) + 1;
    return target.parent_path() /
        (target.filename().wstring() + L".aris." +
         std::to_wstring(Platform::CurrentProcessId()) + L"." +
         std::to_wstring(Platform::MonotonicMilliseconds()) + L"." +
         std::to_wstring(id) + suffix);
}

void RemoveFileQuiet(const fs::path& path) {
    if (!path.empty()) {
        (void)Platform::FileSystem::DeleteFileIfPresent(path.u8string());
    }
}

bool ContinueInstall(const ReaperBridgeInstallObserver& observer,
                     ReaperBridgeInstallCheckpoint checkpoint) {
    return !observer || observer(checkpoint);
}

bool RestartRequiredForTarget(const fs::path& target) {
    std::lock_guard<std::mutex> lock(g_restartMutex);
    return !g_restartRequiredTarget.empty() && SamePath(target, g_restartRequiredTarget);
}

void MarkRestartRequired(const fs::path& target) {
    std::lock_guard<std::mutex> lock(g_restartMutex);
    g_restartRequiredTarget = target.wstring();
}

void ClearRestartRequired(const fs::path& target) {
    std::lock_guard<std::mutex> lock(g_restartMutex);
    if (!g_restartRequiredTarget.empty() && SamePath(target, g_restartRequiredTarget)) {
        g_restartRequiredTarget.clear();
    }
}

bool RestorePreviousBridge(const fs::path& target,
                           const fs::path& backup,
                           bool hadExistingTarget,
                           InstallResult& result,
                           const ReaperBridgeInstallObserver& observer) {
    result.rollbackPerformed = true;
    if (!hadExistingTarget) {
        const bool removed = Platform::FileSystem::DeleteFileIfPresent(
            target.u8string()).status == Platform::FileSystem::DurableFileOpStatus::Success;
        result.rollbackVerified = removed && !FileExists(target);
        ContinueInstall(observer, ReaperBridgeInstallCheckpoint::RolledBack);
        return result.rollbackVerified;
    }
    const fs::path rollbackStage = UniqueTransactionPath(target, L".rollback.tmp");
    RemoveFileQuiet(rollbackStage);
    const auto rollbackCopy = Platform::FileSystem::CopyFileExclusiveAndFlush(
        backup.u8string(), rollbackStage.u8string());
    const auto replaced = rollbackCopy.status ==
            Platform::FileSystem::DurableFileOpStatus::Success
        ? Platform::FileSystem::ReplaceFileWriteThrough(
            rollbackStage.u8string(), target.u8string())
        : Platform::FileSystem::DurableFileOpResult{};
    const bool replacedOk = replaced.status ==
        Platform::FileSystem::DurableFileOpStatus::Success;
    if (!replacedOk) RemoveFileQuiet(rollbackStage);
    result.rollbackVerified = replacedOk &&
        ComputeSha256(target) == ComputeSha256(backup);
    ContinueInstall(observer, ReaperBridgeInstallCheckpoint::RolledBack);
    return result.rollbackVerified;
}

void DescribeStatus(InstallStatus& status, const ReaperBridge::HandshakeResult& handshake) {
    switch (status.state) {
    case InstallState::Unconfigured:
        status.detail.setKey(LStr::DBst_UnconfD);
        status.remediation.setKey(LStr::DBst_UnconfR);
        break;
    case InstallState::ReaperPathMissing:
        status.detail.setKey(LStr::DBst_PathMissD);
        status.remediation.setKey(LStr::DBst_PathMissR);
        break;
    case InstallState::ReaperExecutableMissing:
        status.detail.setKey(LStr::DBst_ExeMissD);
        status.remediation.setKey(LStr::DBst_ExeMissR);
        break;
    case InstallState::WaitingForHostExit:
        status.detail.setKey(LStr::DBst_RunningD);
        status.remediation.setKey(LStr::DBst_RunningR);
        break;
    case InstallState::HandshakeReady:
        status.detail.setKey(LStr::DBst_HandOkD);
        status.remediation.clear();
        break;
    case InstallState::RestartRequired:
        status.detail.setKey(LStr::DBst_NeedRestartD);
        status.remediation.setKey(LStr::DBst_NeedRestartR);
        break;
    case InstallState::InstalledNotLoaded:
        status.detail.setKey(LStr::DBst_NotLoadedD);
        status.remediation.setKey(LStr::DBst_NotLoadedR);
        break;
    case InstallState::VersionIncompatible:
        status.detail.setKey(LStr::DBst_OldVerD);
        status.remediation.setKey(LStr::DBst_OldVerR);
        break;
    case InstallState::ProtocolIncompatible:
        status.detail.setKey(LStr::DBst_ProtoD);
        status.remediation.setKey(LStr::DBst_ProtoR);
        break;
    case InstallState::ArchitectureIncompatible:
        status.detail.setKey(LStr::DBst_ArchD);
        status.remediation.setKey(LStr::DBst_ArchR);
        break;
    case InstallState::WrongLocation:
        status.detail.setKey(LStr::DBst_StaleDirD);
        status.remediation.setKey(LStr::DBst_StaleDirR);
        break;
    case InstallState::BinaryInvalid:
        status.detail.setKey(LStr::DBst_CorruptD);
        status.remediation.setKey(LStr::DBst_CorruptR);
        break;
    case InstallState::UnknownSource:
        status.detail.setKey(LStr::DBst_UnknownD);
        status.remediation.setKey(LStr::DBst_UnknownR);
        break;
    case InstallState::FileInstalled:
        status.detail.setKey(LStr::DBst_LatestIdleD);
        status.remediation.setKey(LStr::DBst_LatestIdleR);
        break;
    case InstallState::NotInstalled:
        status.detail.setKey(LStr::DBst_NotInstD);
        status.remediation.setKey(LStr::DBst_NotInstR);
        break;
    case InstallState::BundledComponentMissing:
        status.detail.setKey(LStr::DBst_InstMissD);
        status.remediation.setKey(LStr::DBst_InstMissR);
        break;
    case InstallState::ResourcePathUnconfirmed:
        status.detail.setKey(LStr::DBst_RootUnconfD);
        status.remediation.setKey(LStr::DBst_RootUnconfR);
        break;
    case InstallState::HandshakeFailed:
        if (handshake.detail.empty())
            status.detail.setKey(LStr::DBst_HandFailD);
        else
            status.detail.setKeyStr(LStr::DBst_HandFailFmt, handshake.detail);
        status.remediation.setKey(LStr::DBst_HandFailR);
        break;
    case InstallState::Unknown:
    default:
        status.detail.setKey(LStr::DBst_UnknownStateD);
        status.remediation.setKey(LStr::DBst_UnknownStateR);
        break;
    }
}

} // namespace

const char* ReaperBridgeTargetResolutionKey(ReaperBridgeTargetResolution resolution) {
    switch (resolution) {
    case ReaperBridgeTargetResolution::ExplicitPluginDirectory: return "explicit_plugin_directory";
    case ReaperBridgeTargetResolution::RunningPortableInstall: return "running_portable_install";
    case ReaperBridgeTargetResolution::ConfiguredPortableInstall: return "configured_portable_install";
    case ReaperBridgeTargetResolution::StandardResourcePath: return "standard_resource_path";
    case ReaperBridgeTargetResolution::Unresolved:
    default: return "unresolved";
    }
}

const char* ReaperBridgeDoctorActionKey(ReaperBridgeDoctorAction action) {
    switch (action) {
    case ReaperBridgeDoctorAction::Diagnose: return "diagnose";
    case ReaperBridgeDoctorAction::Repair: return "repair";
    case ReaperBridgeDoctorAction::Update: return "update";
    case ReaperBridgeDoctorAction::Verify: return "verify";
    default: return "diagnose";
    }
}

std::string AutoDetectReaperRootPath() {
    const std::wstring running = FindRunningReaperExecutable();
    if (!running.empty()) return WideToUtf8Local(fs::path(running).parent_path().wstring());
    for (const Platform::RegisteredExecutableScope scope : {
             Platform::RegisteredExecutableScope::CurrentUser,
             Platform::RegisteredExecutableScope::LocalMachine}) {
        const auto registered = Platform::ReadRegisteredExecutablePath(scope, "reaper.exe");
        if (registered.status != Platform::RegisteredExecutablePathStatus::Success ||
            registered.executablePathUtf8.empty()) {
            continue;
        }
        const fs::path executable(Utf8ToWideLocal(registered.executablePathUtf8));
        if (!executable.empty() && FileExists(executable)) {
            return WideToUtf8Local(executable.parent_path().wstring());
        }
    }
    const auto programFiles = Platform::ExpandEnvironmentVariables("%ProgramFiles%");
    if (programFiles.status == Platform::EnvironmentStringStatus::Success &&
        !programFiles.valueUtf8.empty()) {
        const fs::path programFilesPath(Utf8ToWideLocal(programFiles.valueUtf8));
        for (const wchar_t* folder : {L"REAPER (x64)", L"REAPER"}) {
            const fs::path candidate = programFilesPath / folder;
            if (FileExists(candidate / L"reaper.exe")) {
                return WideToUtf8Local(candidate.wstring());
            }
        }
    }
    return {};
}

BridgeBinaryInfo ProbeBridgeBinary(const std::string& pathUtf8) {
    BridgeBinaryInfo info;
    info.path = pathUtf8;
    const fs::path path(Utf8ToWideLocal(pathUtf8));
    const Platform::FileSystem::RegularFileInfo file =
        Platform::FileSystem::ProbeRegularFile(path.u8string());
    info.exists = file.isRegularFile;
    if (!info.exists) return info;
    info.sizeBytes = file.sizeBytes;
    info.version = ReadFileVersion(path);
    info.architecture = ReadPeArchitecture(path);
    info.sha256 = ComputeSha256(path);
    info.productName = ReadVersionString(path, L"ProductName");
    info.fileDescription = ReadVersionString(path, L"FileDescription");
    info.manifest = ReadVersionString(path, L"BridgeManifest");
    const std::string protocol = ReadVersionString(path, L"BridgeProtocolVersion");
    if (!protocol.empty()) {
        const auto parsed = std::from_chars(
            protocol.data(), protocol.data() + protocol.size(), info.protocolVersion);
        if (parsed.ec != std::errc{} || parsed.ptr != protocol.data() + protocol.size()) {
            info.protocolVersion = 0;
        }
    }
    info.trustedSource = info.productName == "HarkenSearch" &&
                         info.fileDescription == "HarkenSearch REAPER Extension";
#if defined(__APPLE__)
    // Static identity strings are not source authentication. Only the component
    // pinned into this application build may pass the Mac source check.
#if defined(HARKEN_MAC_REAPER_BUILD_SHA256)
    info.trustedSource = info.trustedSource &&
        info.sha256 == HARKEN_MAC_REAPER_BUILD_SHA256 &&
        info.architecture == ReaperBridge::CurrentArchitecture() && HasReaperPluginEntry(path);
#else
    info.trustedSource = false;
#endif
#endif
    const bool legacyProductIdentity = info.productName == "Aris" &&
        info.fileDescription == "Aris REAPER Extension";
    info.legacySignature = info.trustedSource || legacyProductIdentity ||
        (IsManagedReaperPluginName(path) && HasReaperPluginEntry(path));
    return info;
}

InstallStatus ProbeReaperBridge(const ReaperBridgeProbeOptions& options) {
    InstallStatus status;
    const ResolvedReaperTarget target = ResolveReaperTarget(options);
    const fs::path bundledPath = options.bundledDllPathOverride.empty()
        ? BundledReaperPlugin()
        : fs::path(Utf8ToWideLocal(options.bundledDllPathOverride));
    const fs::path installedPath = target.targetResolved
        ? target.pluginDirectory / Platform::HarkenUserData::CanonicalReaperPluginFileName()
        : fs::path();
    const BridgeBinaryInfo bundled = ProbeBridgeBinary(WideToUtf8Local(bundledPath.wstring()));
    const BridgeBinaryInfo installed = ProbeBridgeBinary(WideToUtf8Local(installedPath.wstring()));
    const bool fileAtOtherLocation = !target.alternatePlugin.empty() &&
        !SamePath(installedPath, target.alternatePlugin) && FileExists(target.alternatePlugin);

    ReaperBridge::HandshakeResult handshake;
    handshake.code = ReaperBridge::HandshakeCode::TransportUnavailable;
    handshake.detail = "REAPER is not running";
#if defined(__APPLE__)
    if (!target.hostStateKnown) handshake.detail = "REAPER process state is unavailable";
#endif
    if (target.hostRunning) {
        const std::wstring_view pipeName = options.pipeNameOverride.empty()
            ? std::wstring_view(ReaperBridge::kDefaultPipeName)
            : std::wstring_view(options.pipeNameOverride);
        handshake = ReaperBridge::ProbeHandshake(
            pipeName, (std::max)(1, options.handshakeTimeoutMs));
    }

    ReaperBridgeProbeFacts facts;
    facts.configured = target.configured;
    facts.configuredPathExists = target.configuredPathExists;
    facts.hostExecutableExists = target.hostInstalled;
    facts.targetResolved = target.targetResolved;
    facts.bundledFilePresent = bundled.exists;
    facts.filePresentAtTarget = installed.exists;
    facts.filePresentAtOtherLocation = fileAtOtherLocation;
    facts.hostRunning = target.hostRunning;
    facts.restartRequired = RestartRequiredForTarget(installedPath);
    facts.binaryIntegrityOk = !installed.exists ||
        (installed.sizeBytes > 0 && !installed.sha256.empty() &&
         !installed.architecture.empty() && installed.architecture != "unknown" &&
         (!bundled.exists || bundled.sha256.empty() || installed.sha256 == bundled.sha256 ||
          installed.version != bundled.version ||
          installed.protocolVersion != bundled.protocolVersion));
    facts.trustedSource = !installed.exists || installed.trustedSource || installed.legacySignature;
    facts.installedVersion = installed.version;
    facts.expectedVersion = ReaperBridge::kBridgeVersion;
    facts.installedProtocolVersion = installed.protocolVersion;
    facts.expectedProtocolVersion = ReaperBridge::kProtocolVersion;
    facts.installedArchitecture = installed.architecture;
    facts.hostArchitecture = target.hostArchitecture;
    facts.handshakeCode = handshake.code;

    status.state = EvaluateReaperBridgeState(facts);
#if defined(__APPLE__)
    if (target.configured && target.hostInstalled && !target.hostStateKnown)
        status.state = InstallState::Unknown;
#endif
    status.targetResolution = target.resolution;
    status.configuredRootPath = WideToUtf8Local(target.configuredRoot.wstring());
    status.resourcePath = WideToUtf8Local(target.resourcePath.wstring());
    status.targetDirectory = WideToUtf8Local(target.pluginDirectory.wstring());
    status.hostExecutablePath = WideToUtf8Local(target.hostExecutable.wstring());
    status.alternateInstalledPath = WideToUtf8Local(target.alternatePlugin.wstring());
    status.installedPath = WideToUtf8Local(installedPath.wstring());
    status.bundledPath = bundled.path;
    status.installedVersion = installed.version;
    status.expectedVersion = ReaperBridge::kBridgeVersion;
    status.installedArchitecture = installed.architecture;
    status.hostArchitecture = target.hostArchitecture;
    status.installedSha256 = installed.sha256;
    status.bundledSha256 = bundled.sha256;
    status.installedManifest = installed.manifest;
    status.expectedManifest = ReaperBridge::kBridgeManifest;
    status.handshakeCode = HandshakeCodeKey(handshake.code);
    status.handshakeDetail = handshake.detail;
    status.installedProtocolVersion = installed.protocolVersion;
    status.expectedProtocolVersion = ReaperBridge::kProtocolVersion;
    status.handshakeProtocolVersion = handshake.protocolVersion;
    status.handshakeActionId = handshake.actionId;
    status.configured = target.configured;
    status.configuredPathExists = target.configuredPathExists;
    status.hostInstalled = target.hostInstalled;
    status.targetResolved = target.targetResolved;
    status.bundledFilePresent = bundled.exists;
    status.binaryIntegrityOk = facts.binaryIntegrityOk;
    status.trustedSource = facts.trustedSource;
    status.bridgeLoaded = target.hostRunning &&
        handshake.code != ReaperBridge::HandshakeCode::TransportUnavailable;
    status.protocolCompatible = handshake.code == ReaperBridge::HandshakeCode::Ready;
    status.sendChannelAvailable = handshake.code == ReaperBridge::HandshakeCode::Ready;
    status.filePresent = installed.exists;
    status.correctLocation = target.targetResolved && installed.exists;
    status.hostRunning = target.hostRunning;
    status.handshakeOk = handshake.code == ReaperBridge::HandshakeCode::Ready;
    status.restartRequired = facts.restartRequired;
    if (status.handshakeOk) ClearRestartRequired(installedPath);
    DescribeStatus(status, handshake);
    return status;
}

InstallResult InstallReaperBridgeBinary(
    const std::string& sourceDllUtf8,
    const std::string& targetDirectoryUtf8,
    bool hostRunning,
    bool dryRun,
    ReaperBridgeInstallObserver observer)
{
    InstallResult result;
    const BridgeBinaryInfo source = ProbeBridgeBinary(sourceDllUtf8);
    if (!source.exists) {
        result.failureCode = "bundled-component-missing";
        result.detail.setKeyStr(LStr::DBflow_BundledMissingFmt, sourceDllUtf8);
        return result;
    }
    if (!source.trustedSource || source.version != ReaperBridge::kBridgeVersion ||
        source.protocolVersion != ReaperBridge::kProtocolVersion ||
        source.manifest != ReaperBridge::kBridgeManifest || source.sha256.empty()) {
        result.failureCode = "bundled-metadata-invalid";
        result.detail.setKey(LStr::DBflow_BundledVerifyFail);
        return result;
    }
    if (source.architecture != ReaperBridge::CurrentArchitecture()) {
        result.failureCode = "bundled-architecture-invalid";
        result.detail.setKey(LStr::DBflow_BundledArch);
        return result;
    }

    const fs::path targetDirectory(Utf8ToWideLocal(targetDirectoryUtf8));
    if (targetDirectory.empty()) {
        result.failureCode = "target-path-unresolved";
        result.detail.setKey(LStr::DBflow_UserPluginsUnknown);
        return result;
    }
    const fs::path target = targetDirectory / Platform::HarkenUserData::CanonicalReaperPluginFileName();
    result.installedPath = WideToUtf8Local(target.wstring());
    const fs::path legacyTarget = targetDirectory /
        Utf8ToWideLocal(Platform::HarkenUserData::LegacyReaperPluginFileName());
    const BridgeBinaryInfo existing = ProbeBridgeBinary(result.installedPath);
    if (existing.exists && !existing.trustedSource && !existing.legacySignature) {
        result.failureCode = "target-source-untrusted";
        result.detail.setKey(LStr::DBflow_UnknownTarget);
        return result;
    }
    const bool alreadyCurrent = existing.exists &&
        existing.version == source.version &&
        existing.protocolVersion == source.protocolVersion &&
        existing.architecture == source.architecture &&
        existing.manifest == source.manifest &&
        existing.sha256 == source.sha256;
    if (dryRun) {
        result.ok = true;
        result.changed = !alreadyCurrent;
        result.binaryVerified = alreadyCurrent;
        result.failureCode = "dry-run-skip";
        result.detail = LedgerText::keyed(alreadyCurrent ? LStr::DBflow_DryRunCurrent : LStr::DBflow_DryRunNeeded);
        return result;
    }
    auto retireLegacyPlugin = [&]() {
        if (hostRunning || !FileExists(legacyTarget) || SamePath(legacyTarget, target)) return;
        const BridgeBinaryInfo legacy = ProbeBridgeBinary(WideToUtf8Local(legacyTarget.wstring()));
        if (!legacy.exists || (!legacy.trustedSource && !legacy.legacySignature)) return;
        RemoveFileQuiet(legacyTarget);
        if (!FileExists(legacyTarget)) {
            result.detail += LedgerText::keyed(LStr::DBflow_OldRemovedSuffix);
        }
    };
    if (alreadyCurrent) {
        result.ok = true;
        result.binaryVerified = true;
        result.detail.setKey(LStr::DBflow_CurrentVerified);
        retireLegacyPlugin();
        return result;
    }
    if (hostRunning) {
        result.failureCode = "reaper-close-required";
        result.detail.setKey(LStr::DBflow_CloseReaper);
        return result;
    }

    if (!Platform::FileSystem::EnsureDirectoryExists(targetDirectory.u8string())) {
        result.failureCode = "target-directory-create-failed";
        result.detail.setKey(LStr::DBflow_MkdirFail);
        return result;
    }

    const fs::path sourcePath(Utf8ToWideLocal(sourceDllUtf8));
    const fs::path temporary = UniqueTransactionPath(target, L".tmp");
    RemoveFileQuiet(temporary);
    const auto stagedCopy = Platform::FileSystem::CopyFileExclusiveAndFlush(
        sourcePath.u8string(), temporary.u8string());
    if (stagedCopy.status != Platform::FileSystem::DurableFileOpStatus::Success) {
        const uint32_t error = stagedCopy.systemError;
        RemoveFileQuiet(temporary);
        result.failureCode = "stage-failed-" + std::to_string(error);
        result.detail.setKeyInt(LStr::DBflow_StageFailFmt, error);
        return result;
    }
    const BridgeBinaryInfo staged = ProbeBridgeBinary(WideToUtf8Local(temporary.wstring()));
    if (staged.sha256 != source.sha256 || staged.version != source.version ||
        staged.protocolVersion != source.protocolVersion ||
        staged.architecture != source.architecture || staged.manifest != source.manifest) {
        RemoveFileQuiet(temporary);
        result.failureCode = "staged-binary-verification-failed";
        result.detail.setKey(LStr::DBflow_StagedVerifyFail);
        return result;
    }
    if (!ContinueInstall(observer, ReaperBridgeInstallCheckpoint::Staged)) {
        RemoveFileQuiet(temporary);
        result.failureCode = "install-interrupted-after-stage";
        result.detail.setKey(LStr::DBflow_AbortBeforeWrite);
        return result;
    }

    const bool hadExistingTarget = existing.exists;
    fs::path backup;
    if (hadExistingTarget) {
        backup = UniqueTransactionPath(target, L".bak");
        RemoveFileQuiet(backup);
        const auto backupCopy = Platform::FileSystem::CopyFileExclusiveAndFlush(
            target.u8string(), backup.u8string());
        if (backupCopy.status != Platform::FileSystem::DurableFileOpStatus::Success ||
            ComputeSha256(target) != ComputeSha256(backup)) {
            const uint32_t error = backupCopy.systemError;
            RemoveFileQuiet(backup);
            RemoveFileQuiet(temporary);
            result.failureCode = "backup-failed-" + std::to_string(error);
            result.detail.setKey(LStr::DBflow_BackupFail);
            return result;
        }
        result.backupPath = WideToUtf8Local(backup.wstring());
        if (!ContinueInstall(observer, ReaperBridgeInstallCheckpoint::BackupCreated)) {
            RemoveFileQuiet(temporary);
            result.failureCode = "install-interrupted-after-backup";
            result.detail.setKey(LStr::DBflow_BackupKept);
            return result;
        }
    }

    const auto replaced = Platform::FileSystem::ReplaceFileWriteThrough(
        temporary.u8string(), target.u8string());
    if (replaced.status != Platform::FileSystem::DurableFileOpStatus::Success) {
        const uint32_t error = replaced.systemError;
        RemoveFileQuiet(temporary);
        result.failureCode = "replace-failed-" + std::to_string(error);
        result.detail.setKey(LStr::DBflow_AtomicFail);
        return result;
    }
    result.changed = true;
    auto failAfterReplacement = [&](const std::string& code, const LedgerText& detail) {
        result.failureCode = code;
        result.detail = detail;
        if (RestorePreviousBridge(target, backup, hadExistingTarget, result, observer)) {
            result.changed = false;
            result.detail += LedgerText::keyed(LStr::DBflow_RolledBackSuffix);
        } else {
            result.detail += LedgerText::keyed(LStr::DBflow_RollbackUnconfirmed);
        }
        return result;
    };
    if (!ContinueInstall(observer, ReaperBridgeInstallCheckpoint::ReplacementCommitted)) {
        return failAfterReplacement("install-interrupted-after-replace", LedgerText::keyed(LStr::DBflow_VerifyInterrupted));
    }
    const BridgeBinaryInfo copied = ProbeBridgeBinary(result.installedPath);
    if (copied.sha256 != source.sha256 || copied.version != source.version ||
        copied.protocolVersion != source.protocolVersion ||
        copied.architecture != source.architecture || copied.manifest != source.manifest) {
        return failAfterReplacement("installed-binary-verification-failed", LedgerText::keyed(LStr::DBflow_PostInstallVerifyFail));
    }
    if (!ContinueInstall(observer, ReaperBridgeInstallCheckpoint::Verified)) {
        return failAfterReplacement("install-interrupted-after-verification", LedgerText::keyed(LStr::DBflow_PostVerifyInterrupted));
    }
    result.ok = true;
    result.binaryVerified = true;
    result.detail.setKey(LStr::DBflow_InstalledVerified);
    retireLegacyPlugin();
    return result;
}

InstallResult InstallReaperBridge(
    const ReaperBridgeProbeOptions& options,
    bool dryRun,
    ReaperBridgeInstallObserver observer)
{
    const ResolvedReaperTarget target = ResolveReaperTarget(options);
    if (!target.targetResolved) {
        InstallResult result;
        result.failureCode = "target-path-unresolved";
        result.detail.setKey(LStr::DBflow_UserPluginsUnlocatable);
        return result;
    }
#if defined(__APPLE__)
    if (!target.hostStateKnown) {
        InstallResult result;
        result.failureCode = "host-state-unknown";
        result.detail.setKey(LStr::DBst_UnknownStateD);
        return result;
    }
#endif
    const fs::path bundledPath = options.bundledDllPathOverride.empty()
        ? BundledReaperPlugin()
        : fs::path(Utf8ToWideLocal(options.bundledDllPathOverride));
    const BridgeBinaryInfo bundled = ProbeBridgeBinary(WideToUtf8Local(bundledPath.wstring()));
    if (!target.hostArchitecture.empty() && bundled.exists &&
        bundled.architecture != target.hostArchitecture) {
        InstallResult result;
        result.failureCode = "host-architecture-incompatible";
        result.detail.setKey(LStr::DBflow_BundleArchMismatch);
        return result;
    }
    return InstallReaperBridgeBinary(
        WideToUtf8Local(bundledPath.wstring()),
        WideToUtf8Local(target.pluginDirectory.wstring()),
        target.hostRunning,
        dryRun,
        std::move(observer));
}

ReaperBridgeDoctorReport RunReaperBridgeDoctor(const ReaperBridgeDoctorRequest& request) {
    ReaperBridgeDoctorReport report;
    report.action = request.action;
    report.before = ProbeReaperBridge(request.probeOptions);
    report.after = report.before;
    report.recheckAttempts = 1;
    report.steps.push_back("target=" +
        std::string(ReaperBridgeTargetResolutionKey(report.before.targetResolution)));
    report.steps.push_back("state=" + std::string(ReaperBridgeStateKey(report.before.state)));
    if (request.action == ReaperBridgeDoctorAction::Diagnose) {
        report.ok = true;
        report.binaryVerified = report.after.filePresent &&
            report.after.installedSha256 == report.after.bundledSha256 &&
            report.after.installedVersion == report.after.expectedVersion &&
            report.after.installedProtocolVersion == report.after.expectedProtocolVersion;
        report.handshakeVerified = report.after.handshakeOk;
        report.restartRequired = report.after.restartRequired;
        report.summary = report.after.detail;
        return report;
    }

    if (request.action == ReaperBridgeDoctorAction::Repair ||
        request.action == ReaperBridgeDoctorAction::Update) {
        report.install = InstallReaperBridge(
            request.probeOptions, request.dryRun, request.installObserver);
        if (!report.install.ok && report.install.failureCode == "reaper-close-required" &&
            request.waitForHostExit) {
            report.steps.push_back("waiting_for_reaper_exit");
            const int waitTimeoutMs = (std::max)(0, request.hostExitWaitTimeoutMs);
            const int waitIntervalMs = (std::max)(50, request.recheckIntervalMs);
            const auto deadline = std::chrono::steady_clock::now() +
                std::chrono::milliseconds(waitTimeoutMs);
            InstallStatus hostStatus = report.before;
            while (hostStatus.hostRunning && std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(std::chrono::milliseconds(waitIntervalMs));
                hostStatus = ProbeReaperBridge(request.probeOptions);
                ++report.recheckAttempts;
            }
            if (!hostStatus.hostRunning) {
                report.steps.push_back("reaper_exit_detected");
                report.install = InstallReaperBridge(
                    request.probeOptions, request.dryRun, request.installObserver);
            } else {
                report.after = hostStatus;
                report.failureCode = "reaper-still-running";
                report.summary.setKey(LStr::DBflow_ReaperStillRunning);
                report.install.failureCode = report.failureCode;
                report.install.detail = report.summary;
                return report;
            }
        }
        report.changed = report.install.changed;
        report.binaryVerified = report.install.binaryVerified;
        if (!report.install.ok) {
            report.failureCode = report.install.failureCode;
            report.summary = report.install.detail;
            report.after = ProbeReaperBridge(request.probeOptions);
            return report;
        }
        if (report.before.hostRunning && report.install.changed &&
            !request.dryRun && !report.install.installedPath.empty()) {
            MarkRestartRequired(fs::path(Utf8ToWideLocal(report.install.installedPath)));
            report.install.restartRequired = true;
            report.restartRequired = true;
        }
    }

    const int timeoutMs = (std::max)(0, request.recheckTimeoutMs);
    const int intervalMs = (std::max)(10, request.recheckIntervalMs);
    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::milliseconds(timeoutMs);
    bool firstProbe = true;
    do {
        if (!firstProbe) std::this_thread::sleep_for(std::chrono::milliseconds(intervalMs));
        firstProbe = false;
        report.after = ProbeReaperBridge(request.probeOptions);
        ++report.recheckAttempts;
        if (report.after.state == InstallState::HandshakeReady || timeoutMs == 0) break;
    } while (std::chrono::steady_clock::now() < deadline);

    report.handshakeVerified = report.after.handshakeOk;
    report.restartRequired = report.restartRequired || report.after.restartRequired;
    if (request.action == ReaperBridgeDoctorAction::Verify && report.handshakeVerified &&
        !request.verificationFilePaths.empty()) {
        const std::wstring_view pipeName = request.probeOptions.pipeNameOverride.empty()
            ? std::wstring_view(ReaperBridge::kDefaultPipeName)
            : std::wstring_view(request.probeOptions.pipeNameOverride);
        const auto send = ReaperBridge::SendFiles(
            request.verificationFilePaths, {}, pipeName,
            (std::max)(1000, request.probeOptions.handshakeTimeoutMs));
        report.sendVerified = send.ok && send.sent ==
            static_cast<int>(request.verificationFilePaths.size());
        if (!report.sendVerified) report.failureCode = "send-verification-failed";
    }
    if (request.action == ReaperBridgeDoctorAction::Verify) {
        report.binaryVerified = report.after.filePresent &&
            report.after.installedSha256 == report.after.bundledSha256 &&
            report.after.installedProtocolVersion == report.after.expectedProtocolVersion;
        report.ok = report.handshakeVerified &&
            (request.verificationFilePaths.empty() || report.sendVerified);
        if (!report.handshakeVerified) report.failureCode = "handshake-not-ready";
    } else {
        report.ok = report.install.ok;
    }
    report.summary = report.after.detail;
    if (!report.after.remediation.empty()) {
        if (!report.summary.empty()) report.summary += std::string(" ");  // H1b: keyed parts compose without rendering
        report.summary += report.after.remediation;
    }
    return report;
}

std::string BuildReaperBridgeDiagnosticSummary(const InstallStatus& status,
                                               bool redactUserProfile) {
    std::string userProfile;
    if (redactUserProfile) {
        const auto environment = Platform::ReadEnvironmentVariable("USERPROFILE");
        if (environment.status == Platform::EnvironmentStringStatus::Success &&
            !environment.valueUtf8.empty()) {
            userProfile = environment.valueUtf8;
        }
    }
    auto redact = [&](std::string value) {
        if (userProfile.empty()) return value;
        std::string lowerValue = value;
        std::string lowerProfile = userProfile;
        std::transform(lowerValue.begin(), lowerValue.end(), lowerValue.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        std::transform(lowerProfile.begin(), lowerProfile.end(), lowerProfile.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        std::size_t offset = 0;
        while ((offset = lowerValue.find(lowerProfile, offset)) != std::string::npos) {
            value.replace(offset, userProfile.size(), "%USERPROFILE%");
            lowerValue.replace(offset, userProfile.size(), "%userprofile%");
            offset += 13;
        }
        return value;
    };
    std::ostringstream summary;
    summary << L(DBflow_DiagSummaryHeader)
            << "state=" << ReaperBridgeStateKey(status.state) << "\n"
            << "target_resolution=" << ReaperBridgeTargetResolutionKey(status.targetResolution) << "\n"
            << "root=" << redact(status.configuredRootPath) << "\n"
            << "host=" << redact(status.hostExecutablePath) << "\n"
            << "resource=" << redact(status.resourcePath) << "\n"
            << "user_plugins=" << redact(status.targetDirectory) << "\n"
            << "plugin_version=" << (status.installedVersion.empty() ? "unavailable" : status.installedVersion) << "\n"
            << "expected_version=" << status.expectedVersion << "\n"
            << "plugin_protocol=" << status.installedProtocolVersion << "\n"
            << "expected_protocol=" << status.expectedProtocolVersion << "\n"
            << "plugin_arch=" << status.installedArchitecture << "\n"
            << "host_arch=" << status.hostArchitecture << "\n"
            << "plugin_sha256=" << status.installedSha256 << "\n"
            << "bundled_sha256=" << status.bundledSha256 << "\n"
            << "manifest=" << status.installedManifest << "\n"
            << "handshake=" << status.handshakeCode << "\n"
            << "host_running=" << (status.hostRunning ? "yes" : "no") << "\n"
            << "bridge_loaded=" << (status.bridgeLoaded ? "yes" : "no") << "\n"
            << "send_channel=" << (status.sendChannelAvailable ? "available" : "unavailable") << "\n"
            << "restart_required=" << (status.restartRequired ? "yes" : "no") << "\n"
            << "next_action=" << status.remediation.Render();  // H1b: caller (main.cpp copy button) is the UI thread
    return summary.str();
}

} // namespace HarkenSearch::DawBridge
