// Exercise the actual UI bridge without starting its network/install worker.
// Including the implementation keeps its test setup private to this executable.
#include "update/HarkenSearchUpdateComposition.cpp"
#include <cstdio>
#include <cstdlib>

// A UI confirmation must never perform platform work on the calling thread.
// Fail immediately if it starts networking, touches staging, or launches an updater.
namespace Platform::UpdateBootstrapAuthority {
CreateResult CreateUpdateBootstrapAuthorityPort() { std::abort(); }
}
namespace Platform::UpdateBootstrapHealth {
CreateResult CreateUpdateBootstrapHealthPort() { std::abort(); }
}
namespace Platform::UpdateHttp {
CreateResult CreateUpdateHttpTransportPort() { std::abort(); }
}
namespace Platform::UpdatePersistence {
CreateResult CreateUpdatePersistencePort() { std::abort(); }
std::string DescribeCreateFailure(CreateStatus, std::uint32_t) { std::abort(); }
}
namespace Platform::UpdateStagingCapacity {
CreateResult CreateUpdateStagingCapacityPort() { std::abort(); }
}
namespace aris::update {
UpdateCheckHttpsCreateResult CreateUpdateCheckHttpsTransport() { std::abort(); }
}

int main() {
    using namespace aris::update;
    int failures = 0;
    const auto check = [&](bool value, const char* message) {
        if (!value) { ++failures; std::printf("FAIL: %s\n", message); }
    };
    HarkenSearchUpdateUiState ready;
    ready.phase = HarkenSearchUpdateUiPhase::Ready;
    SetUiState(ready);
    check(ConfirmHarkenSearchUpdateRestart(), "ready update accepts the click");
    check(GetHarkenSearchUpdateUiState().phase == HarkenSearchUpdateUiPhase::Restarting,
          "preparation is visible before the worker takes the request");
    check(!ConfirmHarkenSearchUpdateRestart(), "a second click is ignored while preparing");
    check(!ConsumeHarkenSearchUpdateGracefulExitRequest(),
          "showing preparation must not request application exit");
    check(ConsumeRestartConfirmation(), "the worker receives one request");
    check(!ConsumeRestartConfirmation(), "the worker cannot consume it twice");
    ready.phase = HarkenSearchUpdateUiPhase::Error;
    ready.lastError = "deployment identity admission failed";
    SetUiState(ready);
    check(GetHarkenSearchUpdateUiState().phase == HarkenSearchUpdateUiPhase::Error,
          "handoff failure replaces the busy indication");
    check(!ConfirmHarkenSearchUpdateRestart() && !ConsumeRestartConfirmation(),
          "an update that is not ready cannot be queued");
    return failures ? 1 : 0;
}
