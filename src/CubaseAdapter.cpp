#include "CubaseAdapter.h"

#include "AppState.h"
#include "platform/executable_path.h"
#include "platform/filesystem.h"
#include "platform/named_pipe.h"
#include "platform/process_launch.h"
#include "platform/system.h"
#include "platform/text_encoding.h"

#ifndef CUBASE_ADAPTER_ENABLE_DIAGNOSTICS
#define CUBASE_ADAPTER_ENABLE_DIAGNOSTICS 1
#endif

#if CUBASE_ADAPTER_ENABLE_DIAGNOSTICS
#include "DiagnosticsLogger.h"
#else
#define DIAG_ACTION_FMT(category, fmt, ...) do {} while(0)
#define DIAG_SYSTEM_FMT(category, fmt, ...) do {} while(0)
#endif

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <map>
#include <sstream>
#include <vector>

namespace HarkenSearch::CubaseAdapter {

#ifdef AURO_CUBASE_ADAPTER_TESTING
namespace Testing {
using BridgeLaunchHook = bool (*)(const std::wstring&, const std::wstring&, unsigned long&);
static BridgeLaunchHook g_bridgeLaunchHook = nullptr;

void SetBridgeLaunchHook(BridgeLaunchHook hook) {
    g_bridgeLaunchHook = hook;
}
} // namespace Testing
#endif

namespace {

bool CaseInsensitiveContains(const std::string& haystack, const std::string& needle) {
    std::string loweredHaystack = haystack;
    std::string loweredNeedle = needle;
    std::transform(loweredHaystack.begin(), loweredHaystack.end(), loweredHaystack.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    std::transform(loweredNeedle.begin(), loweredNeedle.end(), loweredNeedle.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return loweredHaystack.find(loweredNeedle) != std::string::npos;
}

std::string ReadEnvVar(const char* name) {
#ifdef _WIN32
    char* env = nullptr;
    size_t len = 0;
    if (_dupenv_s(&env, &len, name) != 0 || env == nullptr) {
        return {};
    }
    std::string value(env);
    std::free(env);
    return value;
#else
    const char* env = std::getenv(name);
    return env ? std::string(env) : std::string();
#endif
}

bool FileExistsUtf8(const std::string& path) {
    if (path.empty()) return false;
    try {
        return Platform::FileSystem::Exists(path);
    } catch (...) {
        return false;
    }
}

SteinbergHostKind DetectHostKindFromStrings(const std::string& name,
                                            const std::string& processName,
                                            const std::string& executablePath = {}) {
    if (CaseInsensitiveContains(executablePath, "nuendo") ||
        CaseInsensitiveContains(name, "nuendo") ||
        CaseInsensitiveContains(processName, "nuendo")) {
        return SteinbergHostKind::Nuendo;
    }
    if (CaseInsensitiveContains(executablePath, "cubase") ||
        CaseInsensitiveContains(name, "cubase") ||
        CaseInsensitiveContains(processName, "cubase")) {
        return SteinbergHostKind::Cubase;
    }
    return SteinbergHostKind::Unknown;
}

std::string GetHostDisplayName(SteinbergHostKind kind) {
    switch (kind) {
    case SteinbergHostKind::Cubase:
        return "Cubase";
    case SteinbergHostKind::Nuendo:
        return "Nuendo";
    case SteinbergHostKind::Unknown:
    default:
        return "Steinberg host";
    }
}

std::string GetHostDisplayName(const DawTarget& target) {
    return GetHostDisplayName(DetectHostKindFromStrings(target.name, target.processName, target.exePath));
}

std::string GetHostDisplayName(const InsertRequest& request) {
    return GetHostDisplayName(DetectHostKindFromStrings(request.targetName, request.processName, request.executablePath));
}

std::string GetProtocolVersion(SteinbergHostKind kind) {
    switch (kind) {
    case SteinbergHostKind::Nuendo:
        return "nuendo-direct-insert-v1";
    case SteinbergHostKind::Cubase:
    case SteinbergHostKind::Unknown:
    default:
        return "cubase-direct-insert-v1";
    }
}

std::string GetEndpointName(SteinbergHostKind kind) {
    switch (kind) {
    case SteinbergHostKind::Nuendo:
        return R"(\\.\pipe\auro_nuendo_direct_insert_v1)";
    case SteinbergHostKind::Cubase:
    case SteinbergHostKind::Unknown:
    default:
        return R"(\\.\pipe\auro_cubase_direct_insert_v1)";
    }
}

SteinbergHostKind DetectHostKindFromEndpoint(const std::string& endpointName) {
    if (CaseInsensitiveContains(endpointName, "nuendo")) {
        return SteinbergHostKind::Nuendo;
    }
    if (CaseInsensitiveContains(endpointName, "cubase")) {
        return SteinbergHostKind::Cubase;
    }
    return SteinbergHostKind::Unknown;
}

std::vector<std::string> GetHelperBinaryCandidates(SteinbergHostKind kind) {
    switch (kind) {
    case SteinbergHostKind::Nuendo:
        return {
            "HarkenSearchNuendoBridgeStub.exe",
            "HarkenSearchSteinbergBridgeStub.exe",
            "HarkenSearchCubaseBridgeStub.exe",
        };
    case SteinbergHostKind::Cubase:
        return {
            "HarkenSearchCubaseBridgeStub.exe",
            "HarkenSearchSteinbergBridgeStub.exe",
        };
    case SteinbergHostKind::Unknown:
    default:
        return {
            "HarkenSearchSteinbergBridgeStub.exe",
            "HarkenSearchCubaseBridgeStub.exe",
            "HarkenSearchNuendoBridgeStub.exe",
        };
    }
}

std::vector<std::string> GetHelperPathEnvCandidates(SteinbergHostKind kind) {
    switch (kind) {
    case SteinbergHostKind::Nuendo:
        return {
            "AURO_NUENDO_BRIDGE_STUB_PATH",
            "AURO_STEINBERG_BRIDGE_STUB_PATH",
            "AURO_CUBASE_BRIDGE_STUB_PATH",
        };
    case SteinbergHostKind::Cubase:
        return {
            "AURO_CUBASE_BRIDGE_STUB_PATH",
            "AURO_STEINBERG_BRIDGE_STUB_PATH",
        };
    case SteinbergHostKind::Unknown:
    default:
        return {
            "AURO_STEINBERG_BRIDGE_STUB_PATH",
            "AURO_CUBASE_BRIDGE_STUB_PATH",
            "AURO_NUENDO_BRIDGE_STUB_PATH",
        };
    }
}

std::vector<std::string> GetHelperFlagEnvCandidates(SteinbergHostKind kind) {
    switch (kind) {
    case SteinbergHostKind::Nuendo:
        return {
            "AURO_NUENDO_BRIDGE_STUB",
            "AURO_STEINBERG_BRIDGE_STUB",
            "AURO_CUBASE_BRIDGE_STUB",
        };
    case SteinbergHostKind::Cubase:
        return {
            "AURO_CUBASE_BRIDGE_STUB",
            "AURO_STEINBERG_BRIDGE_STUB",
        };
    case SteinbergHostKind::Unknown:
    default:
        return {
            "AURO_STEINBERG_BRIDGE_STUB",
            "AURO_CUBASE_BRIDGE_STUB",
            "AURO_NUENDO_BRIDGE_STUB",
        };
    }
}

bool HasBridgeFlagOverride(SteinbergHostKind kind) {
    for (const auto& envName : GetHelperFlagEnvCandidates(kind)) {
        if (ReadEnvVar(envName.c_str()) == "1") return true;
    }
    return false;
}

bool HasBridgePathOverride(SteinbergHostKind kind) {
    for (const auto& envName : GetHelperPathEnvCandidates(kind)) {
        if (!ReadEnvVar(envName.c_str()).empty()) return true;
    }
    return false;
}

std::string ReplaceFilename(const std::string& path, const std::string& fileName);
bool IsBridgeStubExecutablePath(const std::string& path);

std::string CurrentProcessPathUtf8() {
#ifdef _WIN32
    const auto result = Platform::GetExecutablePath();
    if (result.status != Platform::ExecutablePathStatus::Success ||
        result.pathUtf8.empty()) {
        return {};
    }
    return result.pathUtf8;
#else
    return {};
#endif
}

BridgeExecutableResolution ResolveBundledBridgeHelper(SteinbergHostKind hostKind) {
    BridgeExecutableResolution resolution;
    const std::string processPath = CurrentProcessPathUtf8();
    if (processPath.empty()) {
        resolution.detail = "Bridge helper cannot be resolved because the current process path is unavailable";
        return resolution;
    }

    for (const auto& binaryName : GetHelperBinaryCandidates(hostKind)) {
        const std::string candidatePath = ReplaceFilename(processPath, binaryName);
        if (FileExistsUtf8(candidatePath)) {
            resolution.executablePath = candidatePath;
            resolution.available = true;
            resolution.stubExecutable = IsBridgeStubExecutablePath(candidatePath);
            resolution.detail = "Resolved bundled bridge helper next to Harken Search: " + candidatePath;
            return resolution;
        }
        if (resolution.executablePath.empty()) {
            resolution.executablePath = candidatePath;
        }
    }
    resolution.detail = "Bundled bridge helper was not found next to Harken Search: " + resolution.executablePath;
    return resolution;
}

bool IsAdapterInstalled(SteinbergHostKind kind = SteinbergHostKind::Unknown) {
    if (HasBridgeFlagOverride(kind) || HasBridgePathOverride(kind)) {
        return true;
    }
    if (kind != SteinbergHostKind::Nuendo) {
        return false;
    }
    return ResolveBundledBridgeHelper(kind).available;
}

std::string MakeCorrelationId(const DawTarget& target, std::size_t fileCount) {
    return target.name + ":" + std::to_string(fileCount) + ":" + target.processName;
}

std::string DescribeFallback(FallbackKind fallback) {
    switch (fallback) {
    case FallbackKind::None:
        return "No fallback";
    case FallbackKind::ExportFallback:
        return "Export fallback";
    case FallbackKind::DragAndDrop:
        return "Drag-and-drop fallback";
    }
    return "Unknown fallback";
}

std::string QuoteIfNeeded(const std::string& value) {
    if (value.find(' ') == std::string::npos && value.find('\t') == std::string::npos) {
        return value;
    }
    return "\"" + value + "\"";
}

std::string ReplaceFilename(const std::string& path, const std::string& fileName) {
    const auto pos = path.find_last_of("/\\");
    if (pos == std::string::npos) return fileName;
    return path.substr(0, pos + 1) + fileName;
}

std::string BaseNameUtf8(const std::string& path) {
    const auto pos = path.find_last_of("/\\");
    return pos == std::string::npos ? path : path.substr(pos + 1);
}

bool IsBridgeStubExecutablePath(const std::string& path) {
    if (path.empty()) return false;
    return CaseInsensitiveContains(BaseNameUtf8(path), "bridgestub");
}

std::string DescribeTrackTarget(TrackTargetKind target) {
    switch (target) {
    case TrackTargetKind::SelectedTrack:
        return "selected-track";
    case TrackTargetKind::CursorTrack:
        return "cursor-track";
    }
    return "unknown-track-target";
}

std::string DescribeSpotMode(SpotMode mode) {
    switch (mode) {
    case SpotMode::EditCursor:
        return "edit-cursor";
    case SpotMode::AppendToTrack:
        return "append-to-track";
    }
    return "unknown-spot-mode";
}

std::string DescribeInsertPolicy(InsertPolicy policy) {
    switch (policy) {
    case InsertPolicy::InsertToSingleTrack:
        return "single-track";
    case InsertPolicy::SpreadAcrossSelection:
        return "spread-selection";
    }
    return "unknown-insert-policy";
}

std::string DescribeSessionAffinity(SessionAffinity affinity) {
    switch (affinity) {
    case SessionAffinity::CurrentSession:
        return "current-session";
    case SessionAffinity::AnyOpenSession:
        return "any-open-session";
    }
    return "unknown-session-affinity";
}

std::string DescribeTransferAction(TransferActionKind action) {
    switch (action) {
    case TransferActionKind::BringIntoHost:
        return "bring-into-host";
    case TransferActionKind::SpotToHost:
        return "spot-to-host";
    }
    return "unknown-transfer-action";
}

std::string DescribeSourceSelection(SourceSelectionKind source) {
    switch (source) {
    case SourceSelectionKind::WholeFiles:
        return "whole-files";
    case SourceSelectionKind::RegionRender:
        return "region-render";
    }
    return "unknown-source-selection";
}

const char* DescribeLifecyclePhase(BridgeLifecyclePhase phase) {
    switch (phase) {
    case BridgeLifecyclePhase::DetectHost: return "DetectHost";
    case BridgeLifecyclePhase::EnsureBridgeProcess: return "EnsureBridgeProcess";
    case BridgeLifecyclePhase::ProbeEndpoint: return "ProbeEndpoint";
    case BridgeLifecyclePhase::HelloHandshake: return "HelloHandshake";
    case BridgeLifecyclePhase::InsertRequest: return "InsertRequest";
    case BridgeLifecyclePhase::AwaitAck: return "AwaitAck";
    }
    return "UnknownPhase";
}

const char* DescribeLaunchFailureKind(LaunchFailureKind kind) {
    switch (kind) {
    case LaunchFailureKind::HostExecutableMissing: return "HostExecutableMissing";
    case LaunchFailureKind::BridgeExecutableMissing: return "BridgeExecutableMissing";
    case LaunchFailureKind::HostLaunchFailed: return "HostLaunchFailed";
    case LaunchFailureKind::BridgeLaunchFailed: return "BridgeLaunchFailed";
    case LaunchFailureKind::EndpointNotFound: return "EndpointNotFound";
    case LaunchFailureKind::EndpointWaitTimeout: return "EndpointWaitTimeout";
    case LaunchFailureKind::HelloHandshakeFailed: return "HelloHandshakeFailed";
    }
    return "UnknownLaunchFailure";
}

const char* DescribeResultCode(ResultCode code) {
    switch (code) {
    case ResultCode::Success: return "Success";
    case ResultCode::PartialSuccess: return "PartialSuccess";
    case ResultCode::TargetMismatch: return "TargetMismatch";
    case ResultCode::NoFiles: return "NoFiles";
    case ResultCode::NeedsConfig: return "NeedsConfig";
    case ResultCode::AdapterMissing: return "AdapterMissing";
    case ResultCode::HostNotRunning: return "HostNotRunning";
    case ResultCode::BridgeNotReady: return "BridgeNotReady";
    case ResultCode::BridgeUnavailable: return "BridgeUnavailable";
    case ResultCode::SessionMismatch: return "SessionMismatch";
    case ResultCode::TrackUnavailable: return "TrackUnavailable";
    case ResultCode::BridgeNotImplemented: return "BridgeNotImplemented";
    }
    return "Unknown";
}

const char* DescribeAddressingDriftKind(AddressingDriftKind kind) {
    switch (kind) {
    case AddressingDriftKind::None: return "None";
    case AddressingDriftKind::SnapshotMismatch: return "SnapshotMismatch";
    case AddressingDriftKind::ProjectMismatch: return "ProjectMismatch";
    case AddressingDriftKind::CursorMismatch: return "CursorMismatch";
    }
    return "Unknown";
}

const char* DescribeTrackResolutionKind(TrackResolutionKind kind) {
    switch (kind) {
    case TrackResolutionKind::Unknown: return "Unknown";
    case TrackResolutionKind::SelectedTrackAccepted: return "SelectedTrackAccepted";
    case TrackResolutionKind::RemappedTrack: return "RemappedTrack";
    case TrackResolutionKind::SelectedTrackChanged: return "SelectedTrackChanged";
    case TrackResolutionKind::SelectedTrackMissing: return "SelectedTrackMissing";
    }
    return "Unknown";
}

HostProjectState ParseHostProjectState(const std::string& value) {
    if (value == "ready") return HostProjectState::ProjectReady;
    if (value == "session-mismatch") return HostProjectState::SessionMismatch;
    return HostProjectState::Unknown;
}

HostTrackState ParseHostTrackState(const std::string& value) {
    if (value == "selected-ready") return HostTrackState::SelectedTrackReady;
    if (value == "remapped") return HostTrackState::TrackRemapped;
    if (value == "missing") return HostTrackState::TrackMissing;
    return HostTrackState::Unknown;
}

bool ParseFlagField(const std::map<std::string, std::string>& fields, const char* key) {
    const auto it = fields.find(key);
    if (it == fields.end()) return false;
    return it->second == "1" || it->second == "true" || it->second == "yes";
}

bool IsProbeHostReady(const BridgeProbeResult& probe) {
    return probe.available &&
           probe.projectState == HostProjectState::ProjectReady &&
           probe.trackState != HostTrackState::TrackMissing &&
           probe.supportsSelectedTrack;
}

AddressingDriftKind ParseAddressingDriftKind(const std::string& detail) {
    if (detail == "snapshot-mismatch") return AddressingDriftKind::SnapshotMismatch;
    if (detail == "project-mismatch") return AddressingDriftKind::ProjectMismatch;
    if (detail == "cursor-mismatch") return AddressingDriftKind::CursorMismatch;
    return AddressingDriftKind::None;
}

TrackResolutionKind ParseTrackResolutionKind(ResultCode code,
                                             HostTrackState trackState,
                                             const std::string& detail,
                                             const std::string& remappedTrack) {
    if (!remappedTrack.empty() || trackState == HostTrackState::TrackRemapped) {
        return TrackResolutionKind::RemappedTrack;
    }
    if (code == ResultCode::TrackUnavailable) {
        if (detail == "selected-track-changed") return TrackResolutionKind::SelectedTrackChanged;
        if (detail == "selected-track-not-found") return TrackResolutionKind::SelectedTrackMissing;
    }
    if (trackState == HostTrackState::SelectedTrackReady) {
        return TrackResolutionKind::SelectedTrackAccepted;
    }
    return TrackResolutionKind::Unknown;
}

void AppendPresentationDetail(std::ostringstream& oss,
                              const char* key,
                              const std::string& value,
                              bool& any) {
    if (value.empty()) return;
    if (any) oss << " | ";
    oss << key << "=" << value;
    any = true;
}

std::string DescribeProbeSnapshot(const BridgeProbeResult& probe) {
    std::ostringstream oss;
    bool any = false;
    if (!probe.projectName.empty()) {
        oss << "project=" << probe.projectName;
        any = true;
    }
    if (!probe.selectedTrackName.empty()) {
        if (any) oss << ", ";
        oss << "selected=" << probe.selectedTrackName;
        any = true;
    }
    if (!probe.cursorLabel.empty()) {
        if (any) oss << ", ";
        oss << "cursor=" << probe.cursorLabel;
        any = true;
    }
    return any ? (" [" + oss.str() + "]") : std::string();
}

std::map<std::string, std::string> ParseKeyValueFields(const std::string& response) {
    std::map<std::string, std::string> fields;
    std::stringstream ss(response);
    std::string token;
    while (std::getline(ss, token, '|')) {
        const auto pos = token.find('=');
        if (pos == std::string::npos || pos == 0 || pos + 1 >= token.size()) {
            continue;
        }
        fields[token.substr(0, pos)] = token.substr(pos + 1);
    }
    return fields;
}

std::vector<std::string> SplitCsvField(const std::string& value) {
    std::vector<std::string> items;
    std::stringstream ss(value);
    std::string item;
    while (std::getline(ss, item, ',')) {
        if (!item.empty()) items.push_back(item);
    }
    return items;
}

bool SendPipeMessage(const BridgeEndpoint& endpoint,
                     const std::string& message,
                     std::string& response,
                     int timeoutMs = 1500) {
#ifdef _WIN32
    const std::wstring pipeName(endpoint.endpointName.begin(), endpoint.endpointName.end());
    Platform::NamedPipe::MessageExchangeOptions options;
    options.connectAttempts = 5;
    options.busyWaitMs = 250;
    options.notFoundSleepMs = 25;
    options.responsePollMs = 10;
    options.responseCapacity = 2047;
    options.ignoreMessageModeFailure = true;
    options.continueAfterPeekOrReadFailure = true;
    options.trimTrailingCrLf = true;
    const auto exchange = Platform::NamedPipe::ExchangeMessageWithOptions(
        pipeName, message, static_cast<uint32_t>(timeoutMs), options);
    switch (exchange.status) {
    case Platform::NamedPipe::MessageExchangeStatus::Unsupported:
        response = "ERR|Steinberg bridge transport is only implemented on Windows";
        return false;
    case Platform::NamedPipe::MessageExchangeStatus::ConnectFailed:
        if (exchange.retryExhausted) {
            response = "ERR|Steinberg bridge pipe is not available";
        } else {
            response = "ERR|Cannot connect to Steinberg bridge (error " +
                       std::to_string(exchange.systemError) + ")";
        }
        return false;
    case Platform::NamedPipe::MessageExchangeStatus::WriteFailed:
        response = "ERR|Failed to write to Steinberg bridge pipe";
        return false;
    case Platform::NamedPipe::MessageExchangeStatus::TimedOut:
    case Platform::NamedPipe::MessageExchangeStatus::MessageModeFailed:
    case Platform::NamedPipe::MessageExchangeStatus::ResponseWaitFailed:
    case Platform::NamedPipe::MessageExchangeStatus::ReadFailed:
        response = "ERR|Steinberg bridge did not respond";
        return false;
    case Platform::NamedPipe::MessageExchangeStatus::Ok:
        response = exchange.response;
        return true;
    }
    response = "ERR|Steinberg bridge did not respond";
    return false;
#else
    (void)endpoint;
    (void)message;
    (void)timeoutMs;
    response = "ERR|Steinberg bridge transport is only implemented on Windows";
    return false;
#endif
}

} // namespace

SteinbergHostKind DetectSteinbergHostKind(const DawTarget& target) {
    if (target.mode != DawSendMode::LaunchExe) return SteinbergHostKind::Unknown;
    return DetectHostKindFromStrings(target.name, target.processName, target.exePath);
}

bool IsCubaseLaunchTarget(const DawTarget& target) {
    return DetectSteinbergHostKind(target) == SteinbergHostKind::Cubase;
}

bool IsNuendoLaunchTarget(const DawTarget& target) {
    return DetectSteinbergHostKind(target) == SteinbergHostKind::Nuendo;
}

bool IsSteinbergDirectInsertTarget(const DawTarget& target) {
    return DetectSteinbergHostKind(target) != SteinbergHostKind::Unknown;
}

PreflightResult EvaluatePreflight(const DawTarget& target) {
    PreflightResult result;
    const SteinbergHostKind hostKind = DetectSteinbergHostKind(target);
    const std::string hostName = GetHostDisplayName(hostKind);
    result.targetRecognized = hostKind != SteinbergHostKind::Unknown;
    if (!result.targetRecognized) {
        result.failureReason = "Target is not a Steinberg direct-insert LaunchExe route";
        return result;
    }

    result.hostRunning = target.isRunning;
    result.executableConfigured = !target.exePath.empty();
    result.adapterAvailable = IsAdapterInstalled(hostKind);

    if (!result.executableConfigured && !result.hostRunning) {
        result.readiness = Readiness::NeedsConfig;
        result.capabilityTag = "Not configured";
        result.preflightSummary = "Configure " + hostName + " before direct insert.";
        result.failureReason =
            target.name + " executable path is not configured and the host is not running";
        return result;
    }

    if (!result.adapterAvailable) {
        result.readiness = Readiness::AdapterMissing;
        result.capabilityTag = "Adapter required";
        result.preflightSummary =
            hostName + " direct insert needs the dedicated adapter bridge before host handoff can run.";
        result.failureReason =
            target.name + " direct insert adapter is not installed yet; host handoff stays disabled";
        return result;
    }

    if (!result.hostRunning) {
        result.readiness = Readiness::HostLaunchRequired;
        result.capabilityTag = "Not running";
        result.preflightSummary =
            hostName + " adapter is installed. Start " + hostName +
            " so Harken Search can attach the direct-insert bridge.";
        result.failureReason =
            target.name + " adapter is installed, but the host must be running before direct insert can attach";
        return result;
    }

    const auto probeRequest = BuildInsertRequest(target, {});
    const auto endpoint = BuildBridgeEndpoint(probeRequest);
    const auto helperResolution = ResolveBridgeExecutable(probeRequest);
    if (hostKind == SteinbergHostKind::Nuendo &&
        helperResolution.available &&
        helperResolution.stubExecutable) {
        result.readiness = Readiness::BridgeNotImplemented;
        result.capabilityTag = "Experimental only";
        result.preflightSummary =
            hostName + " direct insert is still using a protocol stub helper; real host import is not implemented yet.";
        result.failureReason =
            target.name + " currently resolves only to a stub bridge helper, so Harken Search cannot claim real timeline insertion yet";
        return result;
    }
    const auto probe = ProbeBridgeEndpoint(endpoint);
    if (!probe.available) {
        result.readiness = Readiness::BridgeOffline;
        result.capabilityTag = "Bridge offline";
        result.preflightSummary = helperResolution.available
            ? (hostName + " adapter is installed, but the host bridge is not responding yet.")
            : (hostName + " adapter is installed, but the bridge helper executable is not resolved yet.");
        if (!helperResolution.available) {
            result.failureReason = helperResolution.detail.empty()
                ? (target.name + " bridge helper executable is not resolved")
                : helperResolution.detail;
        } else {
            result.failureReason = probe.detail.empty()
                ? (target.name + " bridge endpoint is not responding")
                : (target.name + " bridge endpoint is not responding: " + probe.detail);
        }
        return result;
    }

    result.remoteSessionId = probe.remoteSessionId;
    result.snapshotToken = probe.snapshotToken;
    result.projectName = probe.projectName;
    result.selectedTrackName = probe.selectedTrackName;
    result.cursorLabel = probe.cursorLabel;
    result.projectState = probe.projectState;
    result.trackState = probe.trackState;

    if (!IsProbeHostReady(probe)) {
        result.readiness = Readiness::BridgeNotReady;
        result.capabilityTag = "Host not ready";
        result.preflightSummary =
            hostName + " bridge is online, but the current host project/track state is not ready for direct insert." +
            DescribeProbeSnapshot(probe);
        if (probe.projectState != HostProjectState::ProjectReady) {
            result.failureReason =
                target.name + " bridge is online, but the current project session is not ready for direct insert";
        } else if (probe.trackState == HostTrackState::TrackMissing) {
            result.failureReason =
                target.name + " bridge is online, but no valid selected track is ready for direct insert";
        } else {
            result.failureReason =
                target.name + " bridge is online, but the host does not advertise selected-track insert support";
        }
        return result;
    }

    result.readiness = Readiness::DirectReady;
    result.capabilityTag = "Running - direct insert";
    result.preflightSummary = "Ready for " + hostName + " direct insert." + DescribeProbeSnapshot(probe);
    return result;
}

InsertRequest BuildInsertRequest(const DawTarget& target,
                                 const std::vector<std::string>& files) {
    return BuildInsertRequest(
        target,
        files,
        TransferActionKind::SpotToHost,
        SourceSelectionKind::WholeFiles);
}

InsertRequest BuildInsertRequest(const DawTarget& target,
                                 const std::vector<std::string>& files,
                                 TransferActionKind transferAction,
                                 SourceSelectionKind sourceSelection) {
    InsertRequest request;
    request.protocolVersion = GetProtocolVersion(DetectSteinbergHostKind(target));
    request.targetName = target.name;
    request.executablePath = target.exePath;
    request.processName = target.processName;
    request.hostRunning = target.isRunning;
    request.adapterAvailable = IsAdapterInstalled(DetectSteinbergHostKind(target));
    request.fallback = FallbackKind::ExportFallback;
    request.correlationId = MakeCorrelationId(target, files.size());
    request.transferAction = transferAction;
    request.sourceSelection = sourceSelection;
    request.preferredTrackName = "Selected Track";
    request.requestedSessionId = "current-session";
    request.files.reserve(files.size());
    for (const auto& file : files) {
        request.files.push_back({file});
    }
    return request;
}

InsertRequest BuildInsertRequestFromHostSendJob(
    const DawTarget& target,
    const ::HarkenSearch::HostSend::Job& job) {
    std::vector<std::string> stagedFiles;
    stagedFiles.reserve(job.stagingPlan.files.size());
    for (const auto& file : job.stagingPlan.files) {
        stagedFiles.push_back(file.stagedPathUtf8.empty()
            ? file.sourcePathUtf8
            : file.stagedPathUtf8);
    }

    TransferActionKind transferAction = TransferActionKind::BringIntoHost;
    switch (job.mode) {
    case ::HarkenSearch::HostSend::DeliveryMode::BringIntoHost:
        transferAction = TransferActionKind::BringIntoHost;
        break;
    case ::HarkenSearch::HostSend::DeliveryMode::SpotToHost:
        transferAction = TransferActionKind::SpotToHost;
        break;
    case ::HarkenSearch::HostSend::DeliveryMode::ImportToClipsList:
        transferAction = TransferActionKind::BringIntoHost;
        break;
    }

    SourceSelectionKind selectionKind = SourceSelectionKind::WholeFiles;
    if (!job.items.empty() &&
        job.items.front().selectionKind == ::HarkenSearch::HostSend::SourceSelectionKind::RegionRender) {
        selectionKind = SourceSelectionKind::RegionRender;
    }

    return BuildInsertRequest(
        target,
        stagedFiles,
        transferAction,
        selectionKind);
}

BridgeEndpoint BuildBridgeEndpoint(const InsertRequest& request) {
    BridgeEndpoint endpoint;
    endpoint.transport = TransportKind::NamedPipe;
    endpoint.endpointName = GetEndpointName(DetectHostKindFromStrings(request.targetName, request.processName, request.executablePath));
    endpoint.handshakeToken =
        request.protocolVersion + "|" +
        request.correlationId + "|" +
        DescribeTrackTarget(request.trackTarget);
    return endpoint;
}

BridgeExecutableResolution ResolveBridgeExecutable(const InsertRequest& request) {
    BridgeExecutableResolution resolution;
    const SteinbergHostKind hostKind =
        DetectHostKindFromStrings(request.targetName, request.processName, request.executablePath);
    const std::string hostName = GetHostDisplayName(hostKind);
    for (const auto& envName : GetHelperPathEnvCandidates(hostKind)) {
        const std::string envPath = ReadEnvVar(envName.c_str());
        if (envPath.empty()) continue;
        resolution.environmentOverride = true;
        resolution.executablePath = envPath;
        resolution.available = FileExistsUtf8(envPath);
        resolution.stubExecutable = IsBridgeStubExecutablePath(envPath);
        resolution.detail = resolution.available
            ? ("Using bridge helper override from " + envName + ": " + envPath)
            : ("Bridge helper override path from " + envName + " does not exist: " + envPath);
        return resolution;
    }

    if (hostKind == SteinbergHostKind::Nuendo && !HasBridgeFlagOverride(hostKind)) {
        const auto bundledResolution = ResolveBundledBridgeHelper(hostKind);
        if (bundledResolution.available) {
            return bundledResolution;
        }
        if (resolution.executablePath.empty()) {
            resolution.executablePath = bundledResolution.executablePath;
        }
    }

    if (request.executablePath.empty()) {
        resolution.detail =
            hostName + " bridge helper cannot be resolved because the host executable path is not configured";
        return resolution;
    }

    for (const auto& binaryName : GetHelperBinaryCandidates(hostKind)) {
        const std::string candidatePath = ReplaceFilename(request.executablePath, binaryName);
        if (FileExistsUtf8(candidatePath)) {
            resolution.executablePath = candidatePath;
            resolution.available = true;
            resolution.stubExecutable = IsBridgeStubExecutablePath(candidatePath);
            resolution.detail = "Resolved bridge helper next to " + hostName + " host: " + candidatePath;
            return resolution;
        }
        if (resolution.executablePath.empty()) {
            resolution.executablePath = candidatePath;
        }
    }
    resolution.available = false;
    resolution.detail = "Bridge helper was not found next to " + hostName + " host: " + resolution.executablePath;
    return resolution;
}

BridgeBinaryContract BuildBridgeBinaryContract(const InsertRequest& request) {
    BridgeBinaryContract contract;
    const auto endpoint = BuildBridgeEndpoint(request);
    const SteinbergHostKind hostKind =
        DetectHostKindFromStrings(request.targetName, request.processName, request.executablePath);
    contract.binaryName = GetHelperBinaryCandidates(hostKind).front();
    contract.protocolVersion = request.protocolVersion;
    contract.transport = endpoint.transport;
    contract.endpointName = endpoint.endpointName;
    contract.hostFamily = GetHostDisplayName(hostKind);
    contract.requiredArguments = {
        "--pipe",
        "--token",
        "--target",
        "--hostExe",
        "--hostProcess",
        "--stay-alive",
    };
    return contract;
}

BridgeProcessContract BuildBridgeProcessContract(const InsertRequest& request) {
    BridgeProcessContract contract;
    const auto endpoint = BuildBridgeEndpoint(request);
    const auto helperResolution = ResolveBridgeExecutable(request);
    const auto binaryContract = BuildBridgeBinaryContract(request);
    contract.launchMode = request.hostRunning
        ? HostLaunchMode::ReuseRunningHost
        : HostLaunchMode::LaunchHostThenAttach;
    contract.attachMode = AttachMode::PipeHelloHandshake;
    contract.bridgeExecutableKind = BridgeExecutableKind::ExternalHelperProcess;
    contract.hostExecutableHint = request.executablePath;
    contract.bridgeExecutableHint = helperResolution.executablePath.empty()
        ? binaryContract.binaryName
        : helperResolution.executablePath;
    contract.bridgeLaunchArgumentsHint = BuildBridgeLaunchArguments(request, endpoint);
    contract.phases = {
        BridgeLifecyclePhase::DetectHost,
        BridgeLifecyclePhase::EnsureBridgeProcess,
        BridgeLifecyclePhase::ProbeEndpoint,
        BridgeLifecyclePhase::HelloHandshake,
        BridgeLifecyclePhase::InsertRequest,
        BridgeLifecyclePhase::AwaitAck,
    };
    contract.failureMatrix = {
        LaunchFailureKind::HostExecutableMissing,
        LaunchFailureKind::BridgeExecutableMissing,
        LaunchFailureKind::HostLaunchFailed,
        LaunchFailureKind::BridgeLaunchFailed,
        LaunchFailureKind::EndpointNotFound,
        LaunchFailureKind::EndpointWaitTimeout,
        LaunchFailureKind::HelloHandshakeFailed,
    };
    return contract;
}

HostBridgeV0Contract BuildHostBridgeV0Contract(const InsertRequest& request) {
    HostBridgeV0Contract contract;
    contract.protocolVersion = request.protocolVersion;
    contract.requiredHelloFields = {
        "session",
        "snapshotToken",
        "projectState",
        "trackState",
        "projectName",
        "selectedTrack",
        "cursorLabel",
        "supportsSelectedTrack",
        "supportsCursorTrack",
        "supportsAppendToTrack",
        "supportsSpreadAcrossSelection",
    };
    contract.requiredInsertFields = {
        "track",
        "spot",
        "policy",
        "sessionAffinity",
        "preferredTrack",
        "session",
        "snapshot",
        "projectName",
        "selectedTrack",
        "cursorLabel",
    };
    contract.requiredAckFields = {
        "session",
        "snapshotToken",
        "projectState",
        "trackState",
        "projectName",
        "selectedTrack",
        "cursorLabel",
        "track",
        "position",
        "count",
        "clips",
    };
    contract.hostStateMachine = {
        BridgeLifecyclePhase::DetectHost,
        BridgeLifecyclePhase::HelloHandshake,
        BridgeLifecyclePhase::InsertRequest,
        BridgeLifecyclePhase::AwaitAck,
    };
    contract.hostFailureSurface = {
        ResultCode::BridgeNotReady,
        ResultCode::BridgeUnavailable,
        ResultCode::SessionMismatch,
        ResultCode::TrackUnavailable,
    };
    return contract;
}

BridgeLaunchPlan BuildBridgeLaunchPlan(const InsertRequest& request) {
    BridgeLaunchPlan plan;
    const auto endpoint = BuildBridgeEndpoint(request);
    const auto helperResolution = ResolveBridgeExecutable(request);
    const std::string hostName = GetHostDisplayName(request);
    plan.shouldLaunchHost = !request.hostRunning;
    plan.shouldLaunchBridgeHelper = request.hostRunning && helperResolution.available;
    plan.helperRequired = true;
    plan.endpointWaitTimeoutMs = request.hostRunning ? 1500 : 8000;
    plan.endpointPollIntervalMs = 100;
    plan.helperExecutablePath = helperResolution.executablePath;
    plan.helperLaunchArguments = BuildBridgeLaunchArguments(request, endpoint);
    std::ostringstream oss;
    if (plan.shouldLaunchHost) {
        oss << "Launch " << hostName << " host, then attach helper";
    } else if (plan.shouldLaunchBridgeHelper) {
        oss << "Reuse running " << hostName << " host and wait for helper endpoint";
    } else if (!helperResolution.available) {
        oss << "Helper resolution failed before endpoint wait";
    } else {
        oss << "Attach to existing helper endpoint";
    }
    oss << " (timeout=" << plan.endpointWaitTimeoutMs
        << "ms, poll=" << plan.endpointPollIntervalMs << "ms)";
    plan.summary = oss.str();
    return plan;
}

BridgeLaunchAttemptResult ExecuteBridgeLaunchAttempt(const BridgeLaunchPlan& plan) {
    BridgeLaunchAttemptResult result;
    if (!plan.shouldLaunchBridgeHelper) {
        result.success = true;
        result.detail = "Bridge helper launch not required for this attach path";
        return result;
    }

    result.attempted = true;
    if (plan.helperExecutablePath.empty()) {
        result.failureKind = LaunchFailureKind::BridgeExecutableMissing;
        result.detail = "Bridge helper launch cannot start because no helper path is resolved";
        return result;
    }

    std::string launchStub;
    for (const char* envName : {
             "AURO_STEINBERG_BRIDGE_LAUNCH_STUB",
             "AURO_CUBASE_BRIDGE_LAUNCH_STUB",
             "AURO_NUENDO_BRIDGE_LAUNCH_STUB",
         }) {
        launchStub = ReadEnvVar(envName);
        if (!launchStub.empty()) break;
    }
    if (launchStub == "success") {
        result.usedStub = true;
        result.success = true;
        result.processId = 4242;
        result.detail = "Bridge helper launch stub reported success";
        return result;
    }
    if (launchStub == "fail") {
        result.usedStub = true;
        result.failureKind = LaunchFailureKind::BridgeLaunchFailed;
        result.detail = "Bridge helper launch stub reported failure";
        return result;
    }

#ifdef _WIN32
    const std::wstring helperExe =
        Platform::TextEncoding::Utf8ToWidePermissive(plan.helperExecutablePath);
    const std::wstring helperArgs =
        Platform::TextEncoding::Utf8ToWidePermissive(plan.helperLaunchArguments);
    if ((!plan.helperExecutablePath.empty() && helperExe.empty()) ||
        (!plan.helperLaunchArguments.empty() && helperArgs.empty())) {
        result.failureKind = LaunchFailureKind::BridgeLaunchFailed;
        result.detail = "Bridge helper launch path conversion failed";
        return result;
    }
    std::wstring commandLine = L"\"" + helperExe + L"\"";
    if (!helperArgs.empty()) {
        commandLine += L" ";
        commandLine += helperArgs;
    }

#ifdef AURO_CUBASE_ADAPTER_TESTING
    if (Testing::g_bridgeLaunchHook) {
        result.success = Testing::g_bridgeLaunchHook(
            helperExe, helperArgs, result.processId);
        result.failureKind = LaunchFailureKind::BridgeLaunchFailed;
        result.detail = result.success
            ? "Bridge helper process started"
            : "Bridge helper process start failed through test hook";
        return result;
    }
#endif

    Platform::ProcessLaunchOptions launchOptions;
    const auto launch = Platform::LaunchProcess(helperExe, commandLine, launchOptions);
    if (launch.status == Platform::ProcessLaunchStatus::Ok) {
        result.success = true;
        result.processId = launch.processId;
        result.detail = "Bridge helper process started";
        return result;
    }

    const unsigned long err = launch.systemError;
    result.failureKind =
        (launch.status == Platform::ProcessLaunchStatus::Failed && (err == 2 || err == 3))
        ? LaunchFailureKind::BridgeExecutableMissing
        : LaunchFailureKind::BridgeLaunchFailed;
    result.detail = "Bridge helper process start failed with error " + std::to_string(err);
    return result;
#else
    result.failureKind = LaunchFailureKind::BridgeLaunchFailed;
    result.detail =
        "Bridge helper launch executor is not implemented for real process start yet (" +
        BaseNameUtf8(plan.helperExecutablePath) + ")";
    return result;
#endif
}

BridgeEndpointWaitResult WaitForBridgeEndpoint(const BridgeLaunchPlan& plan,
                                               const BridgeEndpoint& endpoint) {
    BridgeEndpointWaitResult result;
    result.failureKind = LaunchFailureKind::EndpointWaitTimeout;

    if (plan.endpointWaitTimeoutMs <= 0) {
        result.detail = "Endpoint wait timeout is not configured";
        return result;
    }

    const int pollIntervalMs = std::max(1, plan.endpointPollIntervalMs);
    int waitedMs = 0;
    while (waitedMs <= plan.endpointWaitTimeoutMs) {
        ++result.pollAttempts;
        const auto probe = ProbeBridgeEndpoint(endpoint);
        if (probe.available) {
            result.available = true;
            result.remoteSessionId = probe.remoteSessionId;
            result.waitedMs = waitedMs;
            result.detail = probe.detail.empty()
                ? "Steinberg bridge endpoint became available"
                : probe.detail;
            return result;
        }

        result.detail = probe.detail;
        if (waitedMs == plan.endpointWaitTimeoutMs) {
            break;
        }

#ifdef _WIN32
        Platform::SleepMilliseconds(static_cast<uint32_t>(pollIntervalMs));
#endif
        waitedMs = std::min(plan.endpointWaitTimeoutMs, waitedMs + pollIntervalMs);
    }

    result.waitedMs = plan.endpointWaitTimeoutMs;
    result.detail = result.detail.empty()
        ? "Steinberg bridge endpoint did not become available within wait budget"
        : ("Steinberg bridge endpoint did not become available within wait budget: " + result.detail);
    return result;
}

BridgeAttachPreparationResult PrepareBridgeAttach(const InsertRequest& request) {
    BridgeAttachPreparationResult preparation;
    const auto launchPlan = BuildBridgeLaunchPlan(request);
    const auto endpoint = BuildBridgeEndpoint(request);
    const auto helperResolution = ResolveBridgeExecutable(request);

    if (!helperResolution.available) {
        preparation.detail = helperResolution.detail.empty()
            ? "Bridge helper is not resolved"
            : helperResolution.detail;
        return preparation;
    }

    preparation.helperLaunchPending = launchPlan.shouldLaunchBridgeHelper;
    const auto initialProbe = ProbeBridgeEndpoint(endpoint);
    if (initialProbe.available) {
        preparation.ready = true;
        preparation.hostReady = IsProbeHostReady(initialProbe);
        preparation.remoteSessionId = initialProbe.remoteSessionId;
        preparation.snapshotToken = initialProbe.snapshotToken;
        preparation.projectName = initialProbe.projectName;
        preparation.selectedTrackName = initialProbe.selectedTrackName;
        preparation.cursorLabel = initialProbe.cursorLabel;
        preparation.projectState = initialProbe.projectState;
        preparation.trackState = initialProbe.trackState;
        preparation.supportsSelectedTrack = initialProbe.supportsSelectedTrack;
        preparation.supportsCursorTrack = initialProbe.supportsCursorTrack;
        preparation.supportsAppendToTrack = initialProbe.supportsAppendToTrack;
        preparation.supportsSpreadAcrossSelection = initialProbe.supportsSpreadAcrossSelection;
        preparation.endpointWait.available = true;
        preparation.endpointWait.failureKind = LaunchFailureKind::EndpointNotFound;
        preparation.endpointWait.remoteSessionId = initialProbe.remoteSessionId;
        preparation.endpointWait.detail = initialProbe.detail;
        preparation.detail = preparation.hostReady
            ? "Bridge attach ready: endpoint already available"
            : "Bridge attach ready, but host snapshot is not insert-ready yet";
        return preparation;
    }

    preparation.launchAttempt = ExecuteBridgeLaunchAttempt(launchPlan);
    if (preparation.launchAttempt.attempted && !preparation.launchAttempt.success) {
        preparation.detail = "Bridge attach not ready: " + preparation.launchAttempt.detail;
        return preparation;
    }

    preparation.endpointWait = WaitForBridgeEndpoint(launchPlan, endpoint);
    preparation.ready = preparation.endpointWait.available;
    preparation.remoteSessionId = preparation.endpointWait.remoteSessionId;
    if (preparation.endpointWait.available) {
        const auto finalProbe = ProbeBridgeEndpoint(endpoint);
        preparation.hostReady = IsProbeHostReady(finalProbe);
        preparation.snapshotToken = finalProbe.snapshotToken;
        preparation.projectName = finalProbe.projectName;
        preparation.selectedTrackName = finalProbe.selectedTrackName;
        preparation.cursorLabel = finalProbe.cursorLabel;
        preparation.projectState = finalProbe.projectState;
        preparation.trackState = finalProbe.trackState;
        preparation.supportsSelectedTrack = finalProbe.supportsSelectedTrack;
        preparation.supportsCursorTrack = finalProbe.supportsCursorTrack;
        preparation.supportsAppendToTrack = finalProbe.supportsAppendToTrack;
        preparation.supportsSpreadAcrossSelection = finalProbe.supportsSpreadAcrossSelection;
    }
    preparation.detail = preparation.endpointWait.available
        ? (preparation.hostReady
            ? ("Bridge attach ready: " + launchPlan.summary)
            : ("Bridge attach ready, but host snapshot is not insert-ready yet: " + launchPlan.summary))
        : ("Bridge attach not ready: " + launchPlan.summary + "; " + preparation.endpointWait.detail);
    return preparation;
}

std::string BuildBridgeHelloMessage(const BridgeEndpoint& endpoint) {
    return "HELLO|" + endpoint.handshakeToken + "\n";
}

std::string BuildBridgeInsertMessage(const InsertRequest& request) {
    std::ostringstream oss;
    oss << "INSERT|" << request.correlationId
        << "|action=" << DescribeTransferAction(request.transferAction)
        << "|selection=" << DescribeSourceSelection(request.sourceSelection)
        << "|track=" << DescribeTrackTarget(request.trackTarget)
        << "|spot=" << DescribeSpotMode(request.spotMode)
        << "|policy=" << DescribeInsertPolicy(request.insertPolicy)
        << "|sessionAffinity=" << DescribeSessionAffinity(request.sessionAffinity)
        << "|preferredTrack=" << request.preferredTrackName
        << "|session=" << request.requestedSessionId;
    if (!request.expectedSnapshotToken.empty()) {
        oss << "|snapshot=" << request.expectedSnapshotToken;
    }
    if (!request.expectedProjectName.empty()) {
        oss << "|projectName=" << request.expectedProjectName;
    }
    if (!request.expectedSelectedTrackName.empty()) {
        oss << "|selectedTrack=" << request.expectedSelectedTrackName;
    }
    if (!request.expectedCursorLabel.empty()) {
        oss << "|cursorLabel=" << request.expectedCursorLabel;
    }
    for (const auto& file : request.files) {
        std::string normalized = file.filePath;
        std::replace(normalized.begin(), normalized.end(), '/', '\\');
        oss << "|" << normalized;
    }
    oss << "\n";
    return oss.str();
}

std::string BuildBridgeLaunchArguments(const InsertRequest& request,
                                       const BridgeEndpoint& endpoint) {
    std::ostringstream oss;
    oss << "--pipe " << QuoteIfNeeded(endpoint.endpointName)
        << " --token " << QuoteIfNeeded(endpoint.handshakeToken)
        << " --target " << QuoteIfNeeded(request.targetName);
    if (!request.executablePath.empty()) {
        oss << " --hostExe " << QuoteIfNeeded(request.executablePath);
    }
    if (!request.processName.empty()) {
        oss << " --hostProcess " << QuoteIfNeeded(request.processName);
    }
    if (request.keepBridgeHelperAlive) {
        oss << " --stay-alive";
    }
    return oss.str();
}

std::string BuildBridgeShutdownMessage(const BridgeEndpoint& endpoint) {
    (void)endpoint;
    return "HELLO|shutdown\n";
}

BridgeResponse ParseBridgeResponse(const std::string& response) {
    BridgeResponse parsed;
    if (response.rfind("OK", 0) == 0) {
        parsed.ok = true;
        parsed.code = ResultCode::Success;
        parsed.detail = response;
        const auto fields = ParseKeyValueFields(response);
        auto itSession = fields.find("session");
        if (itSession != fields.end()) parsed.remoteSessionId = itSession->second;
        auto itSnapshot = fields.find("snapshotToken");
        if (itSnapshot != fields.end()) parsed.snapshotToken = itSnapshot->second;
        auto itProjectName = fields.find("projectName");
        if (itProjectName != fields.end()) parsed.projectName = itProjectName->second;
        auto itSelectedTrack = fields.find("selectedTrack");
        if (itSelectedTrack != fields.end()) parsed.selectedTrackName = itSelectedTrack->second;
        auto itCursorLabel = fields.find("cursorLabel");
        if (itCursorLabel != fields.end()) parsed.cursorLabel = itCursorLabel->second;
        auto itProjectState = fields.find("projectState");
        if (itProjectState != fields.end()) parsed.projectState = ParseHostProjectState(itProjectState->second);
        auto itTrackState = fields.find("trackState");
        if (itTrackState != fields.end()) parsed.trackState = ParseHostTrackState(itTrackState->second);
        auto itTrack = fields.find("track");
        if (itTrack != fields.end()) parsed.acceptedTrack = itTrack->second;
        auto itRemapped = fields.find("remappedTrack");
        if (itRemapped != fields.end()) parsed.remappedTrack = itRemapped->second;
        auto itPosition = fields.find("position");
        if (itPosition != fields.end()) parsed.acceptedPositionMode = itPosition->second;
        auto itAdjusted = fields.find("adjusted");
        if (itAdjusted != fields.end()) parsed.positionAdjusted = (itAdjusted->second == "1" || itAdjusted->second == "true");
        auto itCount = fields.find("count");
        if (itCount != fields.end()) parsed.insertedFileCount = std::max(0, std::atoi(itCount->second.c_str()));
        auto itClips = fields.find("clips");
        if (itClips != fields.end()) parsed.insertedClipIds = SplitCsvField(itClips->second);
        auto itFailed = fields.find("failed");
        if (itFailed != fields.end()) parsed.failedFilePaths = SplitCsvField(itFailed->second);
        if (!parsed.failedFilePaths.empty()) parsed.code = ResultCode::PartialSuccess;
        parsed.trackResolution =
            ParseTrackResolutionKind(parsed.code, parsed.trackState, parsed.detail, parsed.remappedTrack);
        return parsed;
    }
    if (response.rfind("ERR|ADAPTER_MISSING|", 0) == 0) {
        parsed.code = ResultCode::AdapterMissing;
        parsed.detail = response.substr(std::string("ERR|ADAPTER_MISSING|").size());
        return parsed;
    }
    if (response.rfind("ERR|HOST_NOT_RUNNING|", 0) == 0) {
        parsed.code = ResultCode::HostNotRunning;
        parsed.detail = response.substr(std::string("ERR|HOST_NOT_RUNNING|").size());
        return parsed;
    }
    if (response.rfind("ERR|BRIDGE_UNAVAILABLE|", 0) == 0) {
        parsed.code = ResultCode::BridgeUnavailable;
        parsed.detail = response.substr(std::string("ERR|BRIDGE_UNAVAILABLE|").size());
        return parsed;
    }
    if (response.rfind("ERR|BRIDGE_NOT_READY|", 0) == 0) {
        parsed.code = ResultCode::BridgeNotReady;
        parsed.detail = response.substr(std::string("ERR|BRIDGE_NOT_READY|").size());
        parsed.addressingDrift = ParseAddressingDriftKind(parsed.detail);
        return parsed;
    }
    if (response.rfind("ERR|SESSION_MISMATCH|", 0) == 0) {
        parsed.code = ResultCode::SessionMismatch;
        parsed.detail = response.substr(std::string("ERR|SESSION_MISMATCH|").size());
        return parsed;
    }
    if (response.rfind("ERR|TRACK_UNAVAILABLE|", 0) == 0) {
        parsed.code = ResultCode::TrackUnavailable;
        parsed.detail = response.substr(std::string("ERR|TRACK_UNAVAILABLE|").size());
        parsed.trackResolution =
            ParseTrackResolutionKind(parsed.code, parsed.trackState, parsed.detail, parsed.remappedTrack);
        return parsed;
    }
    if (response.rfind("ERR|", 0) == 0) {
        parsed.code = ResultCode::BridgeNotImplemented;
        parsed.detail = response.substr(4);
        return parsed;
    }
    parsed.code = ResultCode::BridgeUnavailable;
    parsed.detail = "Unexpected bridge response: " + response;
    return parsed;
}

BridgeProbeResult ProbeBridgeEndpoint(const BridgeEndpoint& endpoint) {
    BridgeProbeResult probe;
    DIAG_ACTION_FMT("SteinbergBridge",
                    "ProbeStart endpoint=%s transport=%d",
                    endpoint.endpointName.c_str(),
                    static_cast<int>(endpoint.transport));
    if (endpoint.endpointName.empty()) {
        probe.detail = "Steinberg bridge endpoint is not configured";
        DIAG_SYSTEM_FMT("SteinbergBridge", "ProbeResult available=0 reason=%s", probe.detail.c_str());
        return probe;
    }

    if (!IsAdapterInstalled(DetectHostKindFromEndpoint(endpoint.endpointName))) {
        probe.detail = "Steinberg bridge transport stub is not installed";
        DIAG_SYSTEM_FMT("SteinbergBridge", "ProbeResult available=0 reason=%s", probe.detail.c_str());
        return probe;
    }

    std::string response;
    if (!SendPipeMessage(endpoint, BuildBridgeHelloMessage(endpoint), response)) {
        probe.detail = response.empty() ? "Steinberg bridge pipe did not respond to HELLO" : response;
        DIAG_SYSTEM_FMT("SteinbergBridge", "ProbeResult available=0 reason=%s", probe.detail.c_str());
        return probe;
    }
    const auto parsed = ParseBridgeResponse(response);
    probe.available = parsed.ok;
    probe.remoteSessionId = parsed.remoteSessionId;
    probe.snapshotToken = parsed.snapshotToken;
    probe.projectName = parsed.projectName;
    probe.selectedTrackName = parsed.selectedTrackName;
    probe.cursorLabel = parsed.cursorLabel;
    probe.projectState = parsed.projectState;
    probe.trackState = parsed.trackState;
    const auto fields = ParseKeyValueFields(response);
    probe.supportsSelectedTrack = ParseFlagField(fields, "supportsSelectedTrack");
    probe.supportsCursorTrack = ParseFlagField(fields, "supportsCursorTrack");
    probe.supportsAppendToTrack = ParseFlagField(fields, "supportsAppendToTrack");
    probe.supportsSpreadAcrossSelection = ParseFlagField(fields, "supportsSpreadAcrossSelection");
    probe.detail = parsed.ok ? "Steinberg bridge transport stub responded to HELLO" : parsed.detail;
    DIAG_SYSTEM_FMT("SteinbergBridge",
                    "ProbeResult available=%d code=%s session=%s snapshot=%s project=%s selectedTrack=%s cursor=%s projectState=%d trackState=%d detail=%s",
                    probe.available ? 1 : 0,
                    DescribeResultCode(parsed.code),
                    probe.remoteSessionId.c_str(),
                    probe.snapshotToken.c_str(),
                    probe.projectName.c_str(),
                    probe.selectedTrackName.c_str(),
                    probe.cursorLabel.c_str(),
                    static_cast<int>(probe.projectState),
                    static_cast<int>(probe.trackState),
                    probe.detail.c_str());
    return probe;
}

bool RequestBridgeShutdown(const BridgeEndpoint& endpoint) {
    std::string response;
    if (!SendPipeMessage(endpoint, BuildBridgeShutdownMessage(endpoint), response, 1000)) {
        return false;
    }
    return response.rfind("OK|HELLO|", 0) == 0;
}

DispatchResult ExecuteInsertRequest(const InsertRequest& request) {
    DispatchResult result;
    const std::string hostName = GetHostDisplayName(request);
    result.fallback = request.fallback;
    const auto processContract = BuildBridgeProcessContract(request);
    const auto helperResolution = ResolveBridgeExecutable(request);
    const auto launchPlan = BuildBridgeLaunchPlan(request);
    DIAG_ACTION_FMT("SteinbergBridge",
                    "ExecuteStart correlation=%s files=%d target=%s launchMode=%d attachMode=%d firstPhase=%s firstFailure=%s waitMs=%d helperLaunch=%d pollMs=%d",
                    request.correlationId.c_str(),
                    static_cast<int>(request.files.size()),
                    request.targetName.c_str(),
                    static_cast<int>(processContract.launchMode),
                    static_cast<int>(processContract.attachMode),
                    processContract.phases.empty() ? "None" : DescribeLifecyclePhase(processContract.phases.front()),
                    processContract.failureMatrix.empty() ? "None" : DescribeLaunchFailureKind(processContract.failureMatrix.front()),
                    launchPlan.endpointWaitTimeoutMs,
                    launchPlan.shouldLaunchBridgeHelper ? 1 : 0,
                    launchPlan.endpointPollIntervalMs);

    if (request.files.empty()) {
        result.code = ResultCode::NoFiles;
        result.status = "No audio files were selected for " + hostName + " direct insert";
        DIAG_SYSTEM_FMT("SteinbergBridge", "ExecuteResult code=%s status=%s",
                        DescribeResultCode(result.code), result.status.c_str());
        return result;
    }

    if (request.executablePath.empty() && !request.hostRunning) {
        result.code = ResultCode::NeedsConfig;
        result.status =
            request.targetName + " must be configured or already running before direct insert can start";
        DIAG_SYSTEM_FMT("SteinbergBridge", "ExecuteResult code=%s status=%s",
                        DescribeResultCode(result.code), result.status.c_str());
        return result;
    }

    if (!request.adapterAvailable) {
        result.code = ResultCode::AdapterMissing;
        result.status =
            request.targetName + " direct insert adapter is not installed yet; use " +
            DescribeFallback(result.fallback);
        DIAG_SYSTEM_FMT("SteinbergBridge", "ExecuteResult code=%s status=%s",
                        DescribeResultCode(result.code), result.status.c_str());
        return result;
    }

    if (!request.hostRunning) {
        result.code = ResultCode::HostNotRunning;
        result.status = request.targetName + " is not running; start the host before direct insert";
        DIAG_SYSTEM_FMT("SteinbergBridge", "ExecuteResult code=%s status=%s",
                        DescribeResultCode(result.code), result.status.c_str());
        return result;
    }

    if (!helperResolution.available) {
        result.code = ResultCode::BridgeUnavailable;
        result.status = request.targetName + " bridge helper is not resolved: " + helperResolution.detail;
        DIAG_SYSTEM_FMT("SteinbergBridge", "ExecuteResult code=%s status=%s",
                        DescribeResultCode(result.code), result.status.c_str());
        return result;
    }

    const SteinbergHostKind hostKind =
        DetectHostKindFromStrings(request.targetName, request.processName, request.executablePath);
    if (hostKind == SteinbergHostKind::Nuendo &&
        helperResolution.stubExecutable) {
        result.code = ResultCode::BridgeNotImplemented;
        result.status =
            request.targetName +
            " direct insert is still using a protocol stub helper; real Nuendo timeline import is not implemented yet";
        DIAG_SYSTEM_FMT("SteinbergBridge", "ExecuteResult code=%s status=%s",
                        DescribeResultCode(result.code), result.status.c_str());
        return result;
    }

    const auto attachPreparation = PrepareBridgeAttach(request);
    if (!attachPreparation.ready) {
        result.code = ResultCode::BridgeUnavailable;
        result.status =
            request.targetName + " bridge endpoint is not available; " +
            attachPreparation.detail +
            " [launch-plan: " + launchPlan.summary + "]";
        DIAG_SYSTEM_FMT("SteinbergBridge", "ExecuteResult code=%s status=%s",
                        DescribeResultCode(result.code), result.status.c_str());
        return result;
    }
    if (!attachPreparation.hostReady) {
        result.code = ResultCode::BridgeNotReady;
        result.snapshotToken = attachPreparation.snapshotToken;
        result.projectName = attachPreparation.projectName;
        result.selectedTrackName = attachPreparation.selectedTrackName;
        result.cursorLabel = attachPreparation.cursorLabel;
        result.projectState = attachPreparation.projectState;
        result.trackState = attachPreparation.trackState;
        result.remoteSessionId = attachPreparation.remoteSessionId;
        result.status =
            request.targetName + " bridge attached, but host snapshot is not ready for direct insert; " +
            attachPreparation.detail;
        DIAG_SYSTEM_FMT("SteinbergBridge", "ExecuteResult code=%s status=%s",
                        DescribeResultCode(result.code), result.status.c_str());
        return result;
    }

    const auto endpoint = BuildBridgeEndpoint(request);
    InsertRequest requestWithSnapshot = request;
    if (requestWithSnapshot.expectedSnapshotToken.empty()) {
        requestWithSnapshot.expectedSnapshotToken = attachPreparation.snapshotToken;
    }
    if (requestWithSnapshot.expectedProjectName.empty()) {
        requestWithSnapshot.expectedProjectName = attachPreparation.projectName;
    }
    if (requestWithSnapshot.expectedSelectedTrackName.empty()) {
        requestWithSnapshot.expectedSelectedTrackName = attachPreparation.selectedTrackName;
    }
    if (requestWithSnapshot.expectedCursorLabel.empty()) {
        requestWithSnapshot.expectedCursorLabel = attachPreparation.cursorLabel;
    }
    std::string response;
    if (!SendPipeMessage(endpoint, BuildBridgeInsertMessage(requestWithSnapshot), response, 5000)) {
        result.code = ResultCode::BridgeUnavailable;
        result.status = request.targetName + " bridge insert request failed: " + response;
        DIAG_SYSTEM_FMT("SteinbergBridge", "ExecuteResult code=%s status=%s",
                        DescribeResultCode(result.code), result.status.c_str());
        return result;
    }

    const auto parsed = ParseBridgeResponse(response);
    result.success = parsed.ok;
    result.code = parsed.code;
    result.addressingDrift = parsed.addressingDrift;
    result.trackResolution = parsed.trackResolution;
    result.snapshotToken = parsed.snapshotToken.empty()
        ? attachPreparation.snapshotToken
        : parsed.snapshotToken;
    result.projectName = parsed.projectName.empty()
        ? attachPreparation.projectName
        : parsed.projectName;
    result.selectedTrackName = parsed.selectedTrackName.empty()
        ? attachPreparation.selectedTrackName
        : parsed.selectedTrackName;
    result.cursorLabel = parsed.cursorLabel.empty()
        ? attachPreparation.cursorLabel
        : parsed.cursorLabel;
    result.projectState = parsed.projectState;
    result.trackState = parsed.trackState;
    result.acceptedTrack = parsed.remappedTrack.empty() ? parsed.acceptedTrack : parsed.remappedTrack;
    result.remoteSessionId = parsed.remoteSessionId.empty()
        ? attachPreparation.remoteSessionId
        : parsed.remoteSessionId;
    result.insertedClipIds = parsed.insertedClipIds;
    if (parsed.ok) {
        std::string status =
            request.targetName + " direct insert bridge accepted the request" +
            (result.acceptedTrack.empty() ? "" : " on " + result.acceptedTrack) +
            (parsed.acceptedPositionMode.empty() ? "" : " @" + parsed.acceptedPositionMode);
        if (!result.projectName.empty()) {
            status += " [project=" + result.projectName + "]";
        }
        if (!result.selectedTrackName.empty()) {
            status += " [selected=" + result.selectedTrackName + "]";
        }
        if (!result.cursorLabel.empty()) {
            status += " [cursor=" + result.cursorLabel + "]";
        }
        if (parsed.positionAdjusted) {
            status += " (host adjusted position)";
        }
        if (!parsed.failedFilePaths.empty()) {
            status += " - partial success, failed=" + std::to_string(parsed.failedFilePaths.size());
        }
        result.status = std::move(status);
    } else {
        result.status = request.targetName + " bridge returned: " + parsed.detail;
    }
    DIAG_SYSTEM_FMT("SteinbergBridge",
                    "ExecuteResult code=%s success=%d session=%s snapshot=%s project=%s selectedTrack=%s cursor=%s track=%s addressing=%s resolution=%s clips=%d status=%s",
                    DescribeResultCode(result.code),
                    result.success ? 1 : 0,
                    result.remoteSessionId.c_str(),
                    result.snapshotToken.c_str(),
                    result.projectName.c_str(),
                    result.selectedTrackName.c_str(),
                    result.cursorLabel.c_str(),
                    result.acceptedTrack.c_str(),
                    DescribeAddressingDriftKind(result.addressingDrift),
                    DescribeTrackResolutionKind(result.trackResolution),
                    static_cast<int>(result.insertedClipIds.size()),
                    result.status.c_str());
    return result;
}

DispatchResult DispatchToCubaseAdapterDetailed(DawTarget& target,
                                               const std::vector<std::string>& files) {
    return DispatchToCubaseAdapterDetailed(
        target,
        files,
        TransferActionKind::SpotToHost,
        SourceSelectionKind::WholeFiles);
}

DispatchResult DispatchToCubaseAdapterDetailed(DawTarget& target,
                                               const ::HarkenSearch::HostSend::Job& job) {
    const auto preflight = EvaluatePreflight(target);

    DispatchResult result;
    result.fallback = FallbackKind::ExportFallback;
    if (!preflight.targetRecognized) {
        result.code = ResultCode::TargetMismatch;
        result.status = "Steinberg direct-insert adapter route was requested for a non-Steinberg target";
        return result;
    }

    const auto request = BuildInsertRequestFromHostSendJob(target, job);
    return ExecuteInsertRequest(request);
}

DispatchResult DispatchToCubaseAdapterDetailed(DawTarget& target,
                                               const std::vector<std::string>& files,
                                               TransferActionKind transferAction,
                                               SourceSelectionKind sourceSelection) {
    const auto preflight = EvaluatePreflight(target);

    DispatchResult result;
    result.fallback = FallbackKind::ExportFallback;
    if (!preflight.targetRecognized) {
        result.code = ResultCode::TargetMismatch;
        result.status = "Steinberg direct-insert adapter route was requested for a non-Steinberg target";
        return result;
    }

    const auto request = BuildInsertRequest(target, files, transferAction, sourceSelection);
    return ExecuteInsertRequest(request);
}

DispatchPresentation BuildDispatchPresentation(const DispatchResult& result) {
    DispatchPresentation presentation;
    presentation.successTone = result.success;

    switch (result.code) {
    case ResultCode::Success:
        presentation.summary =
            result.trackResolution == TrackResolutionKind::RemappedTrack
                ? "Inserted via remapped track"
                : "Direct insert accepted";
        break;
    case ResultCode::PartialSuccess:
        presentation.summary =
            result.trackResolution == TrackResolutionKind::RemappedTrack
                ? "Partial insert via remapped track"
                : "Partial direct insert";
        break;
    case ResultCode::BridgeNotReady:
        switch (result.addressingDrift) {
        case AddressingDriftKind::SnapshotMismatch:
            presentation.summary = "Snapshot drift";
            break;
        case AddressingDriftKind::ProjectMismatch:
            presentation.summary = "Project changed";
            break;
        case AddressingDriftKind::CursorMismatch:
            presentation.summary = "Cursor changed";
            break;
        default:
            presentation.summary = "Host not ready";
            break;
        }
        break;
    case ResultCode::TrackUnavailable:
        switch (result.trackResolution) {
        case TrackResolutionKind::SelectedTrackChanged:
            presentation.summary = "Selected track changed";
            break;
        case TrackResolutionKind::SelectedTrackMissing:
            presentation.summary = "Selected track missing";
            break;
        case TrackResolutionKind::RemappedTrack:
            presentation.summary = "Track remapped";
            break;
        default:
            presentation.summary = "Track unavailable";
            break;
        }
        break;
    case ResultCode::SessionMismatch:
        presentation.summary = "Session mismatch";
        break;
    case ResultCode::BridgeUnavailable:
        presentation.summary = "Bridge unavailable";
        break;
    case ResultCode::HostNotRunning:
        presentation.summary = "Host not running";
        break;
    case ResultCode::AdapterMissing:
        presentation.summary = "Adapter missing";
        break;
    case ResultCode::NeedsConfig:
        presentation.summary = "Needs configuration";
        break;
    case ResultCode::NoFiles:
        presentation.summary = "No files selected";
        break;
    case ResultCode::BridgeNotImplemented:
        presentation.summary = "Direct insert not implemented";
        break;
    case ResultCode::TargetMismatch:
        presentation.summary = "Target mismatch";
        break;
    }

    if (!result.acceptedTrack.empty() &&
        result.acceptedTrack != result.selectedTrackName &&
        (result.code == ResultCode::Success || result.code == ResultCode::PartialSuccess)) {
        presentation.summary += " on " + result.acceptedTrack;
    }

    std::ostringstream oss;
    bool any = false;
    AppendPresentationDetail(oss, "project", result.projectName, any);
    AppendPresentationDetail(oss, "selected", result.selectedTrackName, any);
    if (!result.acceptedTrack.empty() &&
        result.acceptedTrack != result.selectedTrackName) {
        AppendPresentationDetail(oss, "accepted", result.acceptedTrack, any);
    }
    AppendPresentationDetail(oss, "cursor", result.cursorLabel, any);
    AppendPresentationDetail(oss, "session", result.remoteSessionId, any);
    if (!result.insertedClipIds.empty()) {
        if (any) oss << " | ";
        oss << "clips=" << result.insertedClipIds.size();
        any = true;
    }
    if (!result.status.empty() &&
        (result.code != ResultCode::Success && result.code != ResultCode::PartialSuccess)) {
        if (any) oss << " | ";
        oss << result.status;
        any = true;
    }
    presentation.detail = any ? oss.str() : result.status;
    return presentation;
}

bool DispatchToCubaseAdapter(DawTarget& target,
                             const std::vector<std::string>& files,
                             std::string& outStatus) {
    const auto preflight = EvaluatePreflight(target);
    const auto result = DispatchToCubaseAdapterDetailed(target, files);
    outStatus = result.status.empty()
        ? (preflight.failureReason.empty() ? preflight.preflightSummary : preflight.failureReason)
        : result.status;
    return result.success;
}

} // namespace HarkenSearch::CubaseAdapter
