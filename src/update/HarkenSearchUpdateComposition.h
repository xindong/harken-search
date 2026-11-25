#pragma once

#include "update/UpdateState.h"

#include <cstdint>
#include <optional>
#include <string>

namespace aris::update {

enum class HarkenSearchUpdateUiPhase {
    Disabled,
    Idle,
    Checking,
    Downloading,
    Ready,
    Restarting,
    Error,
};

enum class HarkenSearchUpdateCheckOrigin { None, Automatic, Manual };

struct HarkenSearchUpdateUiState {
    HarkenSearchUpdateUiPhase phase = HarkenSearchUpdateUiPhase::Idle;
    std::string currentVersion;
    std::string candidateVersion;
    std::uint64_t downloadedBytes = 0;
    std::uint64_t totalBytes = 0;
    bool checkInFlight = false;
    std::string lastError;
    std::string candidateReleaseNotes;
    bool candidateReleaseNotesFailed = false;
    HarkenSearchUpdateCheckOrigin failedCheckOrigin = HarkenSearchUpdateCheckOrigin::None;

    void ClearDownloadProgress() {
        candidateVersion.clear();
        downloadedBytes = 0;
        totalBytes = 0;
    }

    void BeginCheckPresentation() {
        phase = HarkenSearchUpdateUiPhase::Checking;
        failedCheckOrigin = HarkenSearchUpdateCheckOrigin::None;
        lastError.clear();
        ClearDownloadProgress();
        candidateReleaseNotes.clear();
        candidateReleaseNotesFailed = false;
    }

    bool SuppressesDownloadNotice() const {
        return phase == HarkenSearchUpdateUiPhase::Checking ||
               failedCheckOrigin != HarkenSearchUpdateCheckOrigin::None;
    }
};

inline void ApplyHarkenSearchUpdateSnapshot(
    HarkenSearchUpdateUiState& state,
    const UpdateSnapshot& snapshot,
    std::optional<HarkenSearchUpdateCheckOrigin> newFailureOrigin = std::nullopt) {
    state.checkInFlight = false;
    if (snapshot.phase != UpdatePhase::Error) {
        state.failedCheckOrigin = HarkenSearchUpdateCheckOrigin::None;
    } else if (newFailureOrigin) {
        state.failedCheckOrigin = *newFailureOrigin;
    }
    if (state.failedCheckOrigin != HarkenSearchUpdateCheckOrigin::None) {
        state.ClearDownloadProgress();
    } else {
        state.downloadedBytes = snapshot.downloadedBytes;
        if (const auto& candidate = snapshot.readyPatch ? snapshot.readyPatch : snapshot.candidate) {
            state.candidateVersion = candidate->version;
            state.totalBytes = candidate->assetSize;
        }
    }
    switch (snapshot.phase) {
        case UpdatePhase::Checking:
            state.phase = HarkenSearchUpdateUiPhase::Checking;
            break;
        case UpdatePhase::Available:
        case UpdatePhase::Downloading:
            state.phase = HarkenSearchUpdateUiPhase::Downloading;
            break;
        case UpdatePhase::Ready:
        case UpdatePhase::Superseding:
            state.phase = HarkenSearchUpdateUiPhase::Ready;
            break;
        case UpdatePhase::Error:
            state.phase = HarkenSearchUpdateUiPhase::Error;
            state.lastError = snapshot.lastError;
            break;
        case UpdatePhase::Idle:
            state.phase = HarkenSearchUpdateUiPhase::Idle;
            break;
    }
}

// What actually went wrong, derived from the raw failure detail.
//
// The single "or GitHub is unreachable" fallback made every distinct failure
// look identical to users: a rejected redirect, a rejected download plan and a
// mismatched Runtime Pair all rendered the same network-flavoured sentence, so
// the same install failure looked machine-specific and unexplainable.  The UI
// picks its headline and next step from this instead.
enum class HarkenSearchUpdateFailureKind {
    None,
    // Could not reach or read the release channel.
    Network,
    // The channel answered, but the release could not be trusted or used.
    Release,
    // The download or its verification failed.
    Download,
    // The local install could not be admitted for replacement: mismatched
    // Runtime Pair, deployment identity, protected roots, staging ownership.
    LocalInstall,
};

struct HarkenSearchUpdateFailureExplanation {
    HarkenSearchUpdateFailureKind kind = HarkenSearchUpdateFailureKind::None;
    // Headline shown in the update centre, e.g. "更新准备失败".
    std::string headline;
    // One sentence naming what happened and what the user can do next.
    std::string guidance;
};

// Pure classification of a raw failure detail; no I/O, no globals.
HarkenSearchUpdateFailureExplanation ExplainHarkenSearchUpdateFailure(
    const std::string& lastError);

// Whether a persisted failure still describes the current situation.
//
// UpdatePhase::Error survives in patch-info.json and is restored on the next
// launch, with nothing deciding whether it still applies.  Observed on
// 2026-08-11: a morning failure ("update check redirect is not approved") was
// still on screen hours later -- after that bug was fixed and after a
// successful upgrade -- because the stale record was still on disk.
//
// Stale when recorded by an older product version (the code that refused is no
// longer running), or when a ready patch for a newer version exists (the
// pipeline got further than the failure did).  The persisted snapshot currently
// has no recordedVersion field, so the integrated composition uses only the
// ready-patch test; the older-version branch remains pure-helper coverage until
// a compatible schema migration is designed.  UpdateStateStore uses a strict
// HasExactKeys whitelist, so adding the field without migration would make old
// and new patch-info records mutually unreadable during an upgrade.  When
// staleness cannot be established -- empty or unparseable versions -- the
// failure is kept: hiding a real failure is worse than showing an old one.
bool IsStaleUpdateFailure(const std::string& recordedVersion,
                          const std::string& runningVersion,
                          const std::string& readyPatchVersion);

// Captures the inherited Bootstrap challenge without acknowledging readiness.
// The nonce is never accepted from argv or the environment.
bool CaptureHarkenSearchBootstrapHealthChallenge();

// Acknowledges the captured challenge exactly once after the first successful
// non-splash main UI frame has been presented.
bool AcknowledgeHarkenSearchBootstrapHealthReady();

// Compatibility entry for older callers; it only captures the challenge.
bool RunHarkenSearchBootstrapHealthAcknowledgement();

// Starts the serialized product update owner. The owner is deliberately
// inert when the release key is not provisioned, so developer builds never
// contact a release channel or mutate an install.
void StartHarkenSearchAutomaticUpdate();

// Thread-safe UI bridge. The snapshot is a copy; the UI thread may poll it at
// any time without touching updater internals.
HarkenSearchUpdateUiState GetHarkenSearchUpdateUiState();

// Starts a manual check when the updater is provisioned and idle. Returns
// false for developer builds, active checks, or an update awaiting restart.
bool RequestHarkenSearchUpdateCheck();

// Queues a ready update for Bootstrap handoff. Bootstrap acceptance publishes
// a graceful-exit request for the UI thread instead of terminating the worker.
bool ConfirmHarkenSearchUpdateRestart();

// Consumes the one-shot graceful-exit request published after Bootstrap
// accepts the handoff.
bool ConsumeHarkenSearchUpdateGracefulExitRequest();

}  // namespace aris::update
