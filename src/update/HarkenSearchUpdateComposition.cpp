#include "update/HarkenSearchUpdateComposition.h"

#ifdef _WIN32

#include "platform/update_bootstrap_authority_port.h"
#include "platform/update_bootstrap_health_port.h"
#include "platform/update_http_transport.h"
#include "platform/update_persistence_port.h"
#include "platform/update_staging_capacity_port.h"
#include "update/UpdateApplicationService.h"
#include "update/UpdateCheckHttpsTransport.h"
#include "update/UpdateHttpTransport.h"
#include "update/UpdatePersistencePort.h"
#include "update/UpdateReleaseConfig.h"
#include "Version.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace aris::update {
namespace {

using CheckOrigin = HarkenSearchUpdateCheckOrigin;

std::mutex g_updateUiMutex;
HarkenSearchUpdateUiState g_updateUiState{HarkenSearchUpdateUiPhase::Disabled, SOUNDMANAGER_VERSION_STRING, {}, 0, 0, false, {}};
bool g_manualCheckRequested = false;
bool g_restartConfirmed = false;
bool g_workerStarted = false;
bool g_bootstrapHealthCaptureAttempted = false;
std::unique_ptr<Platform::UpdateBootstrapHealth::Port> g_bootstrapHealthPort;
std::atomic<bool> g_gracefulExitRequested{false};

void PublishHarkenSearchUpdateGracefulExitRequest() {
    g_gracefulExitRequested.store(true, std::memory_order_release);
}

void SetUiState(const HarkenSearchUpdateUiState& state) {
    std::lock_guard<std::mutex> lock(g_updateUiMutex);
    g_updateUiState = state;
}

HarkenSearchUpdateUiState SnapshotUiState() {
    std::lock_guard<std::mutex> lock(g_updateUiMutex);
    return g_updateUiState;
}

void MergeSnapshot(const UpdateSnapshot& snapshot,
                   std::optional<CheckOrigin> newFailureOrigin = std::nullopt) {
    std::lock_guard<std::mutex> lock(g_updateUiMutex);
    ApplyHarkenSearchUpdateSnapshot(g_updateUiState, snapshot, newFailureOrigin);
}

bool ConsumeManualCheckRequest() {
    std::lock_guard<std::mutex> lock(g_updateUiMutex);
    if (!g_manualCheckRequested) return false;
    g_manualCheckRequested = false;
    return true;
}

bool ConsumeRestartConfirmation() {
    std::lock_guard<std::mutex> lock(g_updateUiMutex);
    if (!g_restartConfirmed) return false;
    g_restartConfirmed = false;
    return true;
}

bool BeginScheduledWork() {
    std::lock_guard<std::mutex> lock(g_updateUiMutex);
    if (g_manualCheckRequested || g_restartConfirmed ||
        g_updateUiState.checkInFlight ||
        g_updateUiState.phase == HarkenSearchUpdateUiPhase::Ready ||
        g_updateUiState.phase == HarkenSearchUpdateUiPhase::Restarting) {
        return false;
    }
    g_updateUiState.checkInFlight = true;
    return true;
}

// Bootstrap acked the new version as healthy on first presentation: retired
// update download trees under the updater-owned root can go now.  Retention
// keeps the running version's tree, the newest older tree as the rollback
// package, and every pending tree (newer than the running version) so a
// download or a retry is never deleted.  A new port instance owns its own
// trusted-root handle, so pruning cannot interfere with the running updater.
void PruneRetiredUpdateDownloadsAfterHealthAck() {
    auto created = Platform::UpdatePersistence::CreateUpdatePersistencePort();
    if (!created.ok()) return;
    const auto listing = created.port->ObserveVersionDirectories();
    if (!listing.ok) return;
    const auto plan = UpdateDownloadManager::PlanVersionRetention(
        listing.directoryNames, SOUNDMANAGER_VERSION_STRING);
    if (!plan.ok) return;
    for (const auto& directory : plan.pruneVersions) {
        static_cast<void>(created.port->DeleteVersionDirectory(directory));
    }
}

}  // namespace

bool CaptureHarkenSearchBootstrapHealthChallenge() {
    if (g_bootstrapHealthCaptureAttempted) return false;
    g_bootstrapHealthCaptureAttempted = true;
    auto health = Platform::UpdateBootstrapHealth::CreateUpdateBootstrapHealthPort();
    if (!health.ok() || !health.port->CaptureChallenge().ok()) return false;
    g_bootstrapHealthPort = std::move(health.port);
    return true;
}

bool AcknowledgeHarkenSearchBootstrapHealthReady() {
    if (!g_bootstrapHealthPort) return false;
    auto port = std::move(g_bootstrapHealthPort);
    if (!port->AcknowledgeReady().ok()) return false;
    std::thread(PruneRetiredUpdateDownloadsAfterHealthAck).detach();
    return true;
}

bool RunHarkenSearchBootstrapHealthAcknowledgement() {
    return CaptureHarkenSearchBootstrapHealthChallenge();
}

void StartHarkenSearchAutomaticUpdate() {
#if !defined(ARIS_BOOTSTRAP_PACKAGE_PUBLIC_KEY_HEX)
    return;
#else
    static std::once_flag once;
    std::call_once(once, [] {
        std::thread([] {
            auto check = CreateUpdateCheckHttpsTransport();
            auto download = Platform::UpdateHttp::CreateUpdateHttpTransportPort();
            auto persistence = Platform::UpdatePersistence::CreateUpdatePersistencePort();
            auto capacity = Platform::UpdateStagingCapacity::CreateUpdateStagingCapacityPort();
            if (!check.ok() || !download.ok() || !capacity.ok()) return;
            if (!persistence.ok()) {
                HarkenSearchUpdateUiState failed = SnapshotUiState();
                failed.phase = HarkenSearchUpdateUiPhase::Error;
                failed.lastError = Platform::UpdatePersistence::DescribeCreateFailure(
                    persistence.status, persistence.systemError);
                SetUiState(failed);
                return;
            }
            class Clock final : public UpdateApplicationClockPort {
            public:
                std::uint64_t NowMonotonicMs() const override {
                    return static_cast<std::uint64_t>(
                        std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now().time_since_epoch()).count());
                }
            } clock;
            class NoCheckCancel final : public UpdateCheckCancellation {
            public:
                bool IsCancellationRequested() const override { return false; }
                class Registration final : public InterruptRegistration {};
                std::unique_ptr<InterruptRegistration> RegisterInterrupt(
                    std::function<void()> handler) const override {
                    static_cast<void>(handler);
                    return std::make_unique<Registration>();
                }
            } checkCancel;
            class NoDownloadCancel final : public HttpTransportCancellation {
            public:
                bool IsCancellationRequested() const override { return false; }
                class Registration final : public InterruptRegistration {};
                std::unique_ptr<InterruptRegistration> RegisterInterrupt(
                    std::function<void()> handler) const override {
                    static_cast<void>(handler);
                    return std::make_unique<Registration>();
                }
            } downloadCancel;

            auto authority =
                Platform::UpdateBootstrapAuthority::CreateUpdateBootstrapAuthorityPort();
            if (!authority.ok()) return;

            UpdateApplicationServiceConfig config;
#if defined(ARIS_UPDATE_LOCAL_DEMO) && ARIS_UPDATE_LOCAL_DEMO
            // Demo mode keeps the signed control-plane identity on GitHub.  The
            // existing transport adapter alone maps these trusted URLs to the
            // explicitly provisioned loopback mirror.
            config.check.manifestUrl =
                std::string("https://github.com/") +
                ARIS_UPDATE_GITHUB_REPOSITORY +
                "/releases/latest/download/manifest-win-x64-testing.json";
            config.check.policy.currentVersion =
#if defined(ARIS_UPDATE_LOCAL_DEMO_BUILD_VERSION)
                ARIS_UPDATE_LOCAL_DEMO_BUILD_VERSION;
#else
                ARIS_UPDATE_LOCAL_DEMO_OLD_VERSION;
#endif
            config.check.policy.selectedChannel = UpdateChannel::Testing;
            config.runtime.channel = UpdateChannel::Testing;
#else
            // Stable discovery uses manifest-win-x64-stable.json from the
            // company-cloud mirror; the signed asset identity remains GitHub.
            config.check.manifestUrl = CompanyCloudStableManifestUrl();
            config.check.policy.currentVersion = SOUNDMANAGER_VERSION_STRING;
            config.check.policy.selectedChannel = UpdateChannel::Stable;
            config.runtime.channel = UpdateChannel::Stable;
#endif
            config.check.signatureUrl = config.check.manifestUrl + ".sig";
            config.check.publicKeyHex = ARIS_BOOTSTRAP_PACKAGE_PUBLIC_KEY_HEX;
            config.check.policy.platform = "windows";
            config.check.policy.architecture = "x64";
            config.check.policy.allowedRepository = ARIS_UPDATE_GITHUB_REPOSITORY;
            config.check.manifestTimeoutMs = 15'000U;
            config.check.signatureTimeoutMs = 15'000U;
            config.check.manifestBodyLimit = 64U * 1024U;
            config.check.signatureBodyLimit = 16U * 1024U;
            config.runtime.reserveBytes = 64U * 1024U * 1024U;
            config.downloadRelativeTarget = "staging/HarkenSearch-{version}-win-x64-update.zip";
            UpdateApplicationService service(*check.port, *download.port,
                                             *persistence.port, *capacity.port, clock,
                                             *authority.port, config);
            const auto started = service.Start();
            if (started.schedules.empty()) return;
            MergeSnapshot(started.snapshot);
            {
                std::lock_guard<std::mutex> lock(g_updateUiMutex);
                g_workerStarted = true;
            }
            struct ScheduledWork {
                UpdateApplicationScheduleIntent intent;
                std::uint64_t eligibleAt;
            };
            std::vector<ScheduledWork> pending;
            const auto enqueue = [&](const UpdateApplicationResult& result) {
                for (const auto& intent : result.schedules) {
                    std::uint64_t eligibleAt = intent.dueAtMonotonicMs();
                    if (intent.kind() != UpdateApplicationScheduleKind::Check &&
                        result.disposition == UpdateApplicationDisposition::RetryScheduled &&
                        result.error == DownloadErrorKind::Disk) {
                        eligibleAt = std::max(eligibleAt, clock.NowMonotonicMs() + 5'000);
                    }
                    pending.push_back({intent, eligibleAt});
                }
            };
            enqueue(started);
            const auto runOperation = [&](const UpdateApplicationScheduleIntent* scheduled,
                                          CheckOrigin checkOrigin) {
                if (checkOrigin != CheckOrigin::None) {
                    auto state = SnapshotUiState();
                    state.BeginCheckPresentation();
                    SetUiState(state);
                }
                const auto result = scheduled
                    ? service.ExecuteScheduled(*scheduled, checkCancel, downloadCancel)
                    : service.CheckNow(checkCancel, downloadCancel);
                // Presentation-only read. Failure never changes Ready or the
                // existing verify/replace/rollback decisions.
                if (result.snapshot.readyPatch) {
                    const auto& ready = *result.snapshot.readyPatch;
                    std::string bytes;
                    const DownloadRequest notesRequest{ready.version,
                        "staging/HarkenSearch-" + ready.version + "-win-x64-update.zip",
                        ready.assetUrl, ready.assetSize, ready.sha256};
                    const auto paths = UpdateDownloadManager::BuildStagingPaths(notesRequest);
                    const bool read = paths.ok() && persistence.port->ReadReadyReleaseNotes(
                        paths.paths.targetPath, ready.assetSize, ready.sha256, bytes);
                    auto withNotes = SnapshotUiState();
                    withNotes.candidateReleaseNotes = std::move(bytes);
                    withNotes.candidateReleaseNotesFailed = !read;
                    SetUiState(withNotes);
                }
                {
                    std::lock_guard<std::mutex> lock(g_updateUiMutex);
                    ApplyHarkenSearchUpdateOperationResult(g_updateUiState, result, checkOrigin);
                }
                enqueue(result);
            };
            const auto runImmediateCheck = [&](CheckOrigin origin) {
                pending.erase(std::remove_if(pending.begin(), pending.end(),
                    [](const ScheduledWork& work) {
                        return work.intent.kind() == UpdateApplicationScheduleKind::Check;
                    }), pending.end());
                runOperation(nullptr, origin);
            };
            bool handoffFailed = false;
            bool retryHandoff = false;
            const auto runHandoff = [&] {
                auto state = SnapshotUiState();
                state.phase = HarkenSearchUpdateUiPhase::Restarting;
                state.lastError.clear();
                SetUiState(state);
                const auto applied = service.ConfirmRestart();
                if (applied.disposition == UpdateApplicationDisposition::BootstrapAccepted) {
                    PublishHarkenSearchUpdateGracefulExitRequest();
                    return true;
                }
                handoffFailed = true;
                retryHandoff = applied.disposition == UpdateApplicationDisposition::BootstrapRetryable;
                state.phase = HarkenSearchUpdateUiPhase::Error;
                state.checkInFlight = false;
                state.lastError = applied.detail.empty() ? "Bootstrap handoff failed" : applied.detail;
                SetUiState(state);
                return false;
            };
            if (started.snapshot.phase != UpdatePhase::Downloading && BeginScheduledWork()) {
                runImmediateCheck(CheckOrigin::Automatic);
            }
            while (true) {
                if (ConsumeRestartConfirmation()) {
                    if (runHandoff()) return;
                } else if (ConsumeManualCheckRequest()) {
                    if (retryHandoff) {
                        if (runHandoff()) return;
                    } else {
                        handoffFailed = false;
                        runImmediateCheck(CheckOrigin::Manual);
                    }
                } else if (!handoffFailed) {
                    const auto next = std::min_element(pending.begin(), pending.end(),
                        [](const ScheduledWork& left, const ScheduledWork& right) {
                            return left.eligibleAt < right.eligibleAt;
                        });
                    if (next != pending.end() && next->eligibleAt <= clock.NowMonotonicMs() &&
                        BeginScheduledWork()) {
                        const auto intent = next->intent;
                        pending.erase(next);
                        runOperation(&intent, intent.kind() == UpdateApplicationScheduleKind::Check
                            ? CheckOrigin::Automatic : CheckOrigin::None);
                    }
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
            }
        }).detach();
    });
#endif
}

HarkenSearchUpdateUiState GetHarkenSearchUpdateUiState() {
#if defined(ARIS_BOOTSTRAP_PACKAGE_PUBLIC_KEY_HEX)
    return SnapshotUiState();
#else
    return {HarkenSearchUpdateUiPhase::Disabled, SOUNDMANAGER_VERSION_STRING, {}, 0, 0, false, {}};
#endif
}

bool RequestHarkenSearchUpdateCheck() {
#if defined(ARIS_BOOTSTRAP_PACKAGE_PUBLIC_KEY_HEX)
    std::lock_guard<std::mutex> lock(g_updateUiMutex);
    if (!g_workerStarted || g_updateUiState.checkInFlight ||
        g_updateUiState.phase == HarkenSearchUpdateUiPhase::Downloading ||
        g_updateUiState.phase == HarkenSearchUpdateUiPhase::Ready ||
        g_updateUiState.phase == HarkenSearchUpdateUiPhase::Restarting) {
        return false;
    }
    g_manualCheckRequested = true;
    g_updateUiState.checkInFlight = true;
    return true;
#else
    return false;
#endif
}

bool ConfirmHarkenSearchUpdateRestart() {
#if defined(ARIS_BOOTSTRAP_PACKAGE_PUBLIC_KEY_HEX)
    std::lock_guard<std::mutex> lock(g_updateUiMutex);
    if (g_updateUiState.phase != HarkenSearchUpdateUiPhase::Ready) return false;
    g_restartConfirmed = true;
    // Show preparation immediately while the worker checks and hands off the update.
    g_updateUiState.phase = HarkenSearchUpdateUiPhase::Restarting;
    return true;
#else
    return false;
#endif
}

bool ConsumeHarkenSearchUpdateGracefulExitRequest() {
    return g_gracefulExitRequested.exchange(false, std::memory_order_acq_rel);
}

}  // namespace aris::update

#else

namespace aris::update {
bool CaptureHarkenSearchBootstrapHealthChallenge() { return false; }
bool AcknowledgeHarkenSearchBootstrapHealthReady() { return false; }
bool RunHarkenSearchBootstrapHealthAcknowledgement() { return false; }
void StartHarkenSearchAutomaticUpdate() {}
HarkenSearchUpdateUiState GetHarkenSearchUpdateUiState() {
    return {HarkenSearchUpdateUiPhase::Disabled, {}, {}, 0, 0, false, {}};
}
bool RequestHarkenSearchUpdateCheck() { return false; }
bool ConfirmHarkenSearchUpdateRestart() { return false; }
bool ConsumeHarkenSearchUpdateGracefulExitRequest() { return false; }
}  // namespace aris::update

#endif
