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

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

namespace aris::update {
namespace {

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

void MergeSnapshot(const UpdateSnapshot& snapshot) {
    std::lock_guard<std::mutex> lock(g_updateUiMutex);
    g_updateUiState.downloadedBytes = snapshot.downloadedBytes;
    if (snapshot.candidate) {
        g_updateUiState.candidateVersion = snapshot.candidate->version;
        g_updateUiState.totalBytes = snapshot.candidate->assetSize;
    }
    if (snapshot.readyPatch) {
        g_updateUiState.candidateVersion = snapshot.readyPatch->version;
        g_updateUiState.totalBytes = snapshot.readyPatch->assetSize;
    }
    if (snapshot.phase == UpdatePhase::Error) {
        g_updateUiState.lastError = snapshot.lastError;
    }
    switch (snapshot.phase) {
        case UpdatePhase::Checking:
            g_updateUiState.phase = HarkenSearchUpdateUiPhase::Checking;
            break;
        case UpdatePhase::Available:
        case UpdatePhase::Downloading:
            g_updateUiState.phase = HarkenSearchUpdateUiPhase::Downloading;
            break;
        case UpdatePhase::Ready:
        case UpdatePhase::Superseding:
            g_updateUiState.phase = HarkenSearchUpdateUiPhase::Ready;
            break;
        case UpdatePhase::Error:
            g_updateUiState.phase = HarkenSearchUpdateUiPhase::Error;
            break;
        case UpdatePhase::Idle:
            g_updateUiState.phase = HarkenSearchUpdateUiPhase::Idle;
            break;
    }
}

void SetCheckInFlight(bool value) {
    std::lock_guard<std::mutex> lock(g_updateUiMutex);
    g_updateUiState.checkInFlight = value;
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
    return port->AcknowledgeReady().ok();
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
        {
            std::lock_guard<std::mutex> lock(g_updateUiMutex);
            g_workerStarted = true;
        }
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
            const auto runCheck = [&]() -> UpdateApplicationResult {
                SetCheckInFlight(true);
                auto state = SnapshotUiState();
                state.phase = HarkenSearchUpdateUiPhase::Checking;
                // Starting a new check retires the previous failure text.
                // Without this the old message survives underneath the new
                // attempt and reappears if this one lands in a phase that does
                // not overwrite lastError, so a fixed problem keeps being
                // reported.
                state.lastError.clear();
                state.candidateReleaseNotes.clear();
                state.candidateReleaseNotesFailed = false;
                SetUiState(state);
                auto result = service.CheckNow(checkCancel, downloadCancel);
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
                MergeSnapshot(result.snapshot);
                SetCheckInFlight(false);
                return result;
            };
            const auto initial = runCheck();
            if (initial.disposition == UpdateApplicationDisposition::Ready) {
                auto state = SnapshotUiState();
                state.phase = HarkenSearchUpdateUiPhase::Ready;
                SetUiState(state);

                while (true) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(250));
                    if (!ConsumeRestartConfirmation()) continue;
                    const auto applied = service.ConfirmRestart();
                    if (applied.disposition == UpdateApplicationDisposition::BootstrapAccepted) {
                        auto confirmed = SnapshotUiState();
                        confirmed.phase = HarkenSearchUpdateUiPhase::Restarting;
                        SetUiState(confirmed);
                        PublishHarkenSearchUpdateGracefulExitRequest();
                        return;
                    }
                    auto failed = SnapshotUiState();
                    failed.phase = HarkenSearchUpdateUiPhase::Error;
                    failed.lastError = applied.detail.empty() ? "Bootstrap handoff failed" : applied.detail;
                    SetUiState(failed);
                    return;
                }
            }
            while (true) {
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
                if (!ConsumeManualCheckRequest()) continue;
                const auto result = runCheck();
                if (result.disposition == UpdateApplicationDisposition::Ready) {
                    auto state = SnapshotUiState();
                    state.phase = HarkenSearchUpdateUiPhase::Ready;
                    SetUiState(state);
                    while (true) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(250));
                        if (!ConsumeRestartConfirmation()) continue;
                        const auto applied = service.ConfirmRestart();
                        if (applied.disposition == UpdateApplicationDisposition::BootstrapAccepted) {
                            auto confirmed = SnapshotUiState();
                            confirmed.phase = HarkenSearchUpdateUiPhase::Restarting;
                            SetUiState(confirmed);
                            PublishHarkenSearchUpdateGracefulExitRequest();
                            return;
                        }
                        auto failed = SnapshotUiState();
                        failed.phase = HarkenSearchUpdateUiPhase::Error;
                        failed.lastError = applied.detail.empty() ? "Bootstrap handoff failed" : applied.detail;
                        SetUiState(failed);
                        return;
                    }
                }
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
        g_updateUiState.phase == HarkenSearchUpdateUiPhase::Ready ||
        g_updateUiState.phase == HarkenSearchUpdateUiPhase::Restarting) {
        return false;
    }
    g_manualCheckRequested = true;
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
