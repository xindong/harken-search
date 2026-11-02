#include "bootstrap/BootstrapProgressView.h"
#include "bootstrap/HarkenSearchBootstrapExitExplanation.h"
#include "nlohmann_json.hpp"
#include <algorithm>

namespace aris::bootstrap {
namespace {
const char* phaseName(BootstrapProgressPhase phase) {
    switch (phase) {
    case BootstrapProgressPhase::CheckingPackage: return "CheckingPackage";
    case BootstrapProgressPhase::WaitingForExit: return "WaitingForExit";
    case BootstrapProgressPhase::PreparingFiles: return "PreparingFiles";
    case BootstrapProgressPhase::ReplacingFiles: return "ReplacingFiles";
    case BootstrapProgressPhase::Restarting: return "Restarting";
    case BootstrapProgressPhase::CheckingStartup: return "CheckingStartup";
    case BootstrapProgressPhase::Finalizing: return "Finalizing";
    case BootstrapProgressPhase::Restoring: return "Restoring";
    case BootstrapProgressPhase::Completed: return "Completed";
    case BootstrapProgressPhase::Failed: return "Failed";
    }
    return "CheckingPackage";
}
const char* phaseTitle(BootstrapProgressPhase phase) {
    switch (phase) {
    case BootstrapProgressPhase::CheckingPackage: return u8"正在检查文件…";
    case BootstrapProgressPhase::WaitingForExit: return u8"正在退出程序…";
    case BootstrapProgressPhase::PreparingFiles: return u8"正在准备文件…";
    case BootstrapProgressPhase::ReplacingFiles: return u8"正在替换文件…";
    case BootstrapProgressPhase::Restarting: return u8"正在启动新版…";
    case BootstrapProgressPhase::CheckingStartup: return u8"正在确认启动…";
    case BootstrapProgressPhase::Finalizing: return u8"即将完成…";
    case BootstrapProgressPhase::Restoring: return u8"正在恢复文件…";
    case BootstrapProgressPhase::Completed: return u8"更新完成";
    case BootstrapProgressPhase::Failed: return u8"更新未完成";
    }
    return u8"正在更新…";
}
}
std::string BootstrapProgressFailureHint(int exitCode, BootstrapProgressPhase lastActive) {
    const auto explanation = ExplainBootstrapExit(exitCode);
    if (exitCode == 21) return u8"请完全退出 Harken Search 后重试。";
    if (explanation.requiresReinstall) return u8"请重新安装最新版，保留数据库和设置。";
    if (exitCode == 10 || exitCode == 11) return u8"更新包未通过校验，请把错误码发给我们。";
    if (explanation.outcome == BootstrapExitOutcome::RolledBack) return u8"已恢复原版本，请稍后重试。";
    if (lastActive == BootstrapProgressPhase::CheckingStartup ||
        lastActive == BootstrapProgressPhase::Restarting ||
        lastActive == BootstrapProgressPhase::Finalizing)
        return u8"新版未确认就绪，请把错误码发给我们。";
    return u8"请从开始菜单打开 Harken Search 后重试。";
}
std::string BootstrapProgressJson(const BootstrapProgressPresentation& state) {
    auto phase = state.snapshot.phase;
    // A runtime observation is not the final entrypoint result.
    if (!state.finished && phase == BootstrapProgressPhase::Completed)
        phase = BootstrapProgressPhase::Finalizing;
    if (!state.finished && phase == BootstrapProgressPhase::Failed)
        phase = state.lastActive;
    if (state.finished) phase = state.exitCode == 0 ? BootstrapProgressPhase::Completed : BootstrapProgressPhase::Failed;
    const bool failed = phase == BootstrapProgressPhase::Failed;
    const bool complete = state.finished && state.exitCode == 0;
    const bool counted = phase == BootstrapProgressPhase::ReplacingFiles && state.snapshot.totalFiles != 0;
    const auto done = std::min(state.snapshot.completedFiles, state.snapshot.totalFiles);
    nlohmann::json result = {
        {"phase", phaseName(phase)}, {"title", phaseTitle(phase)},
        {"mode", failed ? "failed" : complete ? "complete" : "working"},
        {"busy", !state.finished && !failed}, {"version", state.targetVersion},
        {"completedFiles", counted ? done : 0}, {"totalFiles", counted ? state.snapshot.totalFiles : 0},
        {"progress", nullptr}, {"hint", ""}, {"exitCode", state.finished ? state.exitCode : 0},
        {"lastPhase", phaseTitle(state.lastActive)}, {"elapsedSeconds", state.elapsedSeconds},
        {"reducedMotion", state.reducedMotion}, {"diagnostic", phaseName(phase)}
    };
    if (counted) result["progress"] = static_cast<double>(done) / static_cast<double>(state.snapshot.totalFiles);
    if (complete) result["progress"] = 1.0;
    if (failed && state.finished) {
        result["diagnostic"] = std::string(phaseName(state.lastActive)) + " / " + std::to_string(state.exitCode);
        result["hint"] = BootstrapProgressFailureHint(state.exitCode, state.lastActive);
        if (state.exitCode == 21) {
            result["title"] = u8"程序尚未退出";
        }
    }
    // ASCII JSON can be transported without interpolating executable script.
    return result.dump(-1, ' ', true);
}
} // namespace aris::bootstrap
