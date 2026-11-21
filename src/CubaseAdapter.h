#pragma once

#include "HostSendModel.h"

#include <optional>
#include <string>
#include <vector>

struct DawTarget;

namespace HarkenSearch::CubaseAdapter {

// @stage7-contract
// API contract: adapter builders/parsers return owned request/result values
// and borrow DawTarget/job/file vectors only for each synchronous call.
// Dispatch functions may perform host/bridge IO and report failures through
// ResultCode/diagnostics; they do not transfer target or transport ownership.

enum class SteinbergHostKind {
    Unknown,
    Cubase,
    Nuendo,
};

enum class Readiness {
    DirectReady,
    AdapterMissing,
    NeedsConfig,
    HostLaunchRequired,
    BridgeNotReady,
    BridgeOffline,
    BridgeNotImplemented,
};

enum class FallbackKind {
    None,
    ExportFallback,
    DragAndDrop,
};

enum class ResultCode {
    Success,
    PartialSuccess,
    TargetMismatch,
    NoFiles,
    NeedsConfig,
    AdapterMissing,
    HostNotRunning,
    BridgeNotReady,
    BridgeUnavailable,
    SessionMismatch,
    TrackUnavailable,
    BridgeNotImplemented,
};

enum class TransportKind {
    NamedPipe,
};

enum class HostLaunchMode {
    ReuseRunningHost,
    LaunchHostThenAttach,
};

enum class AttachMode {
    PipeProbe,
    PipeHelloHandshake,
};

enum class BridgeLifecyclePhase {
    DetectHost,
    EnsureBridgeProcess,
    ProbeEndpoint,
    HelloHandshake,
    InsertRequest,
    AwaitAck,
};

enum class BridgeExecutableKind {
    ExternalHelperProcess,
    HostSideExtension,
};

enum class LaunchFailureKind {
    HostExecutableMissing,
    BridgeExecutableMissing,
    HostLaunchFailed,
    BridgeLaunchFailed,
    EndpointNotFound,
    EndpointWaitTimeout,
    HelloHandshakeFailed,
};

enum class TrackTargetKind {
    SelectedTrack,
    CursorTrack,
};

enum class SpotMode {
    EditCursor,
    AppendToTrack,
};

enum class InsertPolicy {
    InsertToSingleTrack,
    SpreadAcrossSelection,
};

enum class SessionAffinity {
    CurrentSession,
    AnyOpenSession,
};

enum class TransferActionKind {
    BringIntoHost,
    SpotToHost,
};

enum class SourceSelectionKind {
    WholeFiles,
    RegionRender,
};

enum class HostProjectState {
    Unknown,
    ProjectReady,
    SessionMismatch,
};

enum class HostTrackState {
    Unknown,
    SelectedTrackReady,
    TrackRemapped,
    TrackMissing,
};

enum class AddressingDriftKind {
    None,
    SnapshotMismatch,
    ProjectMismatch,
    CursorMismatch,
};

enum class TrackResolutionKind {
    Unknown,
    SelectedTrackAccepted,
    RemappedTrack,
    SelectedTrackChanged,
    SelectedTrackMissing,
};

struct InsertRequestFile {
    std::string filePath;
};

struct InsertRequest {
    std::string protocolVersion = "cubase-direct-insert-v1";
    std::string targetName;
    std::string executablePath;
    std::string processName;
    std::string correlationId;
    TransferActionKind transferAction = TransferActionKind::SpotToHost;
    SourceSelectionKind sourceSelection = SourceSelectionKind::WholeFiles;
    TrackTargetKind trackTarget = TrackTargetKind::SelectedTrack;
    SpotMode spotMode = SpotMode::EditCursor;
    InsertPolicy insertPolicy = InsertPolicy::InsertToSingleTrack;
    SessionAffinity sessionAffinity = SessionAffinity::CurrentSession;
    std::string preferredTrackName;
    std::string requestedSessionId;
    std::string expectedSnapshotToken;
    std::string expectedProjectName;
    std::string expectedSelectedTrackName;
    std::string expectedCursorLabel;
    bool spotToCursor = true;
    bool sessionRequired = true;
    bool hostRunning = false;
    bool adapterAvailable = false;
    bool keepBridgeHelperAlive = true;
    FallbackKind fallback = FallbackKind::ExportFallback;
    std::vector<InsertRequestFile> files;
};

struct BridgeEndpoint {
    TransportKind transport = TransportKind::NamedPipe;
    std::string endpointName;
    std::string handshakeToken;
};

struct BridgeExecutableResolution {
    bool available = false;
    bool environmentOverride = false;
    bool stubExecutable = false;
    std::string executablePath;
    std::string detail;
};

struct BridgeBinaryContract {
    std::string binaryName;
    std::string protocolVersion;
    TransportKind transport = TransportKind::NamedPipe;
    std::string endpointName;
    std::string hostFamily;
    std::vector<std::string> requiredArguments;
};

struct BridgeProcessContract {
    HostLaunchMode launchMode = HostLaunchMode::ReuseRunningHost;
    AttachMode attachMode = AttachMode::PipeHelloHandshake;
    std::vector<BridgeLifecyclePhase> phases;
    BridgeExecutableKind bridgeExecutableKind = BridgeExecutableKind::ExternalHelperProcess;
    std::string bridgeExecutableHint;
    std::string bridgeLaunchArgumentsHint;
    std::string hostExecutableHint;
    std::vector<LaunchFailureKind> failureMatrix;
};

struct HostBridgeV0Contract {
    std::string protocolVersion;
    bool requiresRunningHost = true;
    bool requiresProjectSession = true;
    bool requiresTrackTarget = true;
    bool supportsSelectedTrack = true;
    bool supportsCursorTrack = true;
    bool supportsAppendToTrack = true;
    bool supportsSpreadAcrossSelection = true;
    std::vector<std::string> requiredHelloFields;
    std::vector<std::string> requiredInsertFields;
    std::vector<std::string> requiredAckFields;
    std::vector<BridgeLifecyclePhase> hostStateMachine;
    std::vector<ResultCode> hostFailureSurface;
};

struct BridgeLaunchPlan {
    bool shouldLaunchHost = false;
    bool shouldLaunchBridgeHelper = false;
    bool helperRequired = true;
    int endpointWaitTimeoutMs = 0;
    int endpointPollIntervalMs = 0;
    std::string helperExecutablePath;
    std::string helperLaunchArguments;
    std::string summary;
};

struct BridgeEndpointWaitResult {
    bool available = false;
    LaunchFailureKind failureKind = LaunchFailureKind::EndpointWaitTimeout;
    int waitedMs = 0;
    int pollAttempts = 0;
    std::string remoteSessionId;
    std::string detail;
};

struct BridgeLaunchAttemptResult {
    bool attempted = false;
    bool success = false;
    LaunchFailureKind failureKind = LaunchFailureKind::BridgeLaunchFailed;
    unsigned long processId = 0;
    bool usedStub = false;
    std::string detail;
};

struct BridgeAttachPreparationResult {
    bool ready = false;
    bool hostReady = false;
    bool helperLaunchPending = false;
    BridgeLaunchAttemptResult launchAttempt;
    BridgeEndpointWaitResult endpointWait;
    std::string remoteSessionId;
    std::string snapshotToken;
    std::string projectName;
    std::string selectedTrackName;
    std::string cursorLabel;
    HostProjectState projectState = HostProjectState::Unknown;
    HostTrackState trackState = HostTrackState::Unknown;
    bool supportsSelectedTrack = false;
    bool supportsCursorTrack = false;
    bool supportsAppendToTrack = false;
    bool supportsSpreadAcrossSelection = false;
    std::string detail;
};

struct BridgeProbeResult {
    bool available = false;
    std::string remoteSessionId;
    std::string snapshotToken;
    std::string projectName;
    std::string selectedTrackName;
    std::string cursorLabel;
    HostProjectState projectState = HostProjectState::Unknown;
    HostTrackState trackState = HostTrackState::Unknown;
    bool supportsSelectedTrack = false;
    bool supportsCursorTrack = false;
    bool supportsAppendToTrack = false;
    bool supportsSpreadAcrossSelection = false;
    std::string detail;
};

struct BridgeResponse {
    bool ok = false;
    ResultCode code = ResultCode::BridgeUnavailable;
    std::string detail;
    std::string remoteSessionId;
    std::string snapshotToken;
    std::string projectName;
    std::string selectedTrackName;
    std::string cursorLabel;
    HostProjectState projectState = HostProjectState::Unknown;
    HostTrackState trackState = HostTrackState::Unknown;
    AddressingDriftKind addressingDrift = AddressingDriftKind::None;
    TrackResolutionKind trackResolution = TrackResolutionKind::Unknown;
    std::string acceptedTrack;
    std::string remappedTrack;
    std::string acceptedPositionMode;
    bool positionAdjusted = false;
    std::vector<std::string> insertedClipIds;
    std::vector<std::string> failedFilePaths;
    std::optional<int> insertedFileCount;
};

struct DispatchResult {
    bool success = false;
    ResultCode code = ResultCode::TargetMismatch;
    FallbackKind fallback = FallbackKind::None;
    std::string diagnosticCategory = "SteinbergAdapter";
    std::string status;
    std::string snapshotToken;
    std::string projectName;
    std::string selectedTrackName;
    std::string cursorLabel;
    HostProjectState projectState = HostProjectState::Unknown;
    HostTrackState trackState = HostTrackState::Unknown;
    AddressingDriftKind addressingDrift = AddressingDriftKind::None;
    TrackResolutionKind trackResolution = TrackResolutionKind::Unknown;
    std::string acceptedTrack;
    std::string remoteSessionId;
    std::vector<std::string> insertedClipIds;
    std::optional<int> insertedFileCount;
};

struct DispatchPresentation {
    bool successTone = false;
    std::string summary;
    std::string detail;
};

struct PreflightResult {
    Readiness readiness = Readiness::NeedsConfig;
    bool targetRecognized = false;
    bool adapterAvailable = false;
    bool executableConfigured = false;
    bool hostRunning = false;
    std::string capabilityTag;
    std::string preflightSummary;
    std::string failureReason;
    std::string remoteSessionId;
    std::string snapshotToken;
    std::string projectName;
    std::string selectedTrackName;
    std::string cursorLabel;
    HostProjectState projectState = HostProjectState::Unknown;
    HostTrackState trackState = HostTrackState::Unknown;
};

SteinbergHostKind DetectSteinbergHostKind(const DawTarget& target);
bool IsCubaseLaunchTarget(const DawTarget& target);
bool IsNuendoLaunchTarget(const DawTarget& target);
bool IsSteinbergDirectInsertTarget(const DawTarget& target);
PreflightResult EvaluatePreflight(const DawTarget& target);
InsertRequest BuildInsertRequest(const DawTarget& target,
                                 const std::vector<std::string>& files);
InsertRequest BuildInsertRequest(const DawTarget& target,
                                 const std::vector<std::string>& files,
                                 TransferActionKind transferAction,
                                 SourceSelectionKind sourceSelection);
InsertRequest BuildInsertRequestFromHostSendJob(
    const DawTarget& target,
    const HarkenSearch::HostSend::Job& job);
BridgeEndpoint BuildBridgeEndpoint(const InsertRequest& request);
BridgeExecutableResolution ResolveBridgeExecutable(const InsertRequest& request);
BridgeBinaryContract BuildBridgeBinaryContract(const InsertRequest& request);
BridgeProcessContract BuildBridgeProcessContract(const InsertRequest& request);
HostBridgeV0Contract BuildHostBridgeV0Contract(const InsertRequest& request);
BridgeLaunchPlan BuildBridgeLaunchPlan(const InsertRequest& request);
BridgeLaunchAttemptResult ExecuteBridgeLaunchAttempt(const BridgeLaunchPlan& plan);
BridgeEndpointWaitResult WaitForBridgeEndpoint(const BridgeLaunchPlan& plan,
                                               const BridgeEndpoint& endpoint);
BridgeAttachPreparationResult PrepareBridgeAttach(const InsertRequest& request);
std::string BuildBridgeHelloMessage(const BridgeEndpoint& endpoint);
std::string BuildBridgeInsertMessage(const InsertRequest& request);
std::string BuildBridgeShutdownMessage(const BridgeEndpoint& endpoint);
std::string BuildBridgeLaunchArguments(const InsertRequest& request,
                                       const BridgeEndpoint& endpoint);
BridgeResponse ParseBridgeResponse(const std::string& response);
BridgeProbeResult ProbeBridgeEndpoint(const BridgeEndpoint& endpoint);
bool RequestBridgeShutdown(const BridgeEndpoint& endpoint);
DispatchResult ExecuteInsertRequest(const InsertRequest& request);
DispatchResult DispatchToCubaseAdapterDetailed(DawTarget& target,
                                               const std::vector<std::string>& files);
DispatchResult DispatchToCubaseAdapterDetailed(DawTarget& target,
                                               const ::HarkenSearch::HostSend::Job& job);
DispatchResult DispatchToCubaseAdapterDetailed(DawTarget& target,
                                               const std::vector<std::string>& files,
                                               TransferActionKind transferAction,
                                               SourceSelectionKind sourceSelection = SourceSelectionKind::WholeFiles);
DispatchPresentation BuildDispatchPresentation(const DispatchResult& result);
bool DispatchToCubaseAdapter(DawTarget& target,
                             const std::vector<std::string>& files,
                             std::string& outStatus);

} // namespace HarkenSearch::CubaseAdapter
