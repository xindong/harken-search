#pragma once

#include <cstddef>
#include <string>

namespace aris::bootstrap {

// @stage7-contract
// API contract: snapshots are owned, path-free values observed after the
// corresponding update boundary. completedFiles counts only successful writes
// and journal records, never elapsed time. The caller owns the optional sink
// and keeps it alive for the synchronous update call. Report must return
// promptly, without throwing, user input or any change to update authority.

enum class BootstrapProgressPhase {
    CheckingPackage,
    WaitingForExit,
    PreparingFiles,
    ReplacingFiles,
    Restarting,
    CheckingStartup,
    Finalizing,
    Restoring,
    Completed,
    Failed,
};

struct BootstrapProgressSnapshot {
    BootstrapProgressPhase phase = BootstrapProgressPhase::CheckingPackage;
    std::size_t completedFiles = 0;
    std::size_t totalFiles = 0;
};

// Observation only: no native handles, paths, decisions or update authority.
// Implementations must return promptly and never throw or block on user input.
class BootstrapProgressSink {
public:
    virtual ~BootstrapProgressSink() = default;
    virtual void Report(BootstrapProgressSnapshot snapshot) noexcept = 0;
    // Display metadata only, supplied after package/admission verification.
    virtual void SetTargetVersion(const std::string&) noexcept {}
};

}  // namespace aris::bootstrap
