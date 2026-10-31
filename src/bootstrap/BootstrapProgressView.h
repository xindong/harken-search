#pragma once
#include "bootstrap/BootstrapProgress.h"
#include <cstdint>
#include <string>

namespace aris::bootstrap {
// @stage7-contract
// API contract: owns copied display values, returns ASCII JSON, performs no
// I/O and grants no update authority. Only finished + exitCode are terminal.
// UI-owned copy, never a source of transaction decisions. Terminal status is
// supplied by the entrypoint, after all update work has actually returned.
struct BootstrapProgressPresentation {
    BootstrapProgressSnapshot snapshot;
    BootstrapProgressPhase lastActive = BootstrapProgressPhase::CheckingPackage;
    std::string targetVersion;
    std::uint64_t elapsedSeconds = 0;
    bool reducedMotion = false;
    bool finished = false;
    int exitCode = 0;
};
std::string BootstrapProgressJson(const BootstrapProgressPresentation& state);
std::string BootstrapProgressFailureHint(int exitCode, BootstrapProgressPhase lastActive);
} // namespace aris::bootstrap
