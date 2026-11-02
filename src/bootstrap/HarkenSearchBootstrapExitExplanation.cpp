#include "bootstrap/HarkenSearchBootstrapExitExplanation.h"

#include "platform/update_bootstrap_authority_port.h"
#include "platform/update_persistence_port.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <knownfolders.h>
#include <shlobj.h>
#endif

#include <cstdio>
#include <filesystem>

namespace aris::bootstrap {
namespace {

std::string WithCode(const std::string& text, int exitCode) {
    return text + u8"（错误码 " + std::to_string(exitCode) + u8"）";
}

}  // namespace

// Say what actually happened.
//
// Every nonzero exit used to raise one dialog: "更新未完成。请卸载 Harken Search 后重新
// 安装最新版本。"  That advice is wrong for most codes here, and reinstalling
// does not fix them, which is why the same failure looked unexplainable and
// reinstall-proof across machines.
BootstrapExitExplanation ExplainBootstrapExit(int exitCode) {
    BootstrapExitExplanation explanation;
    if (exitCode == static_cast<int>(BootstrapExitCode::Completed)) {
        explanation.outcome = BootstrapExitOutcome::Succeeded;
        return explanation;
    }

    // Developer or unsupported builds: the updater is deliberately inert.
    // Never interrupt these with an error dialog.
    if (exitCode == static_cast<int>(BootstrapExitCode::Unprovisioned) ||
        exitCode == static_cast<int>(BootstrapExitCode::Unsupported)) {
        explanation.outcome = BootstrapExitOutcome::Retryable;
        explanation.message =
            WithCode(u8"此版本未启用自动更新。", exitCode);
        return explanation;
    }

    explanation.showDialog = true;

    switch (static_cast<BootstrapExitCode>(exitCode)) {
    // The previous version was restored.  Aris still works; nothing to fix.
    case BootstrapExitCode::RolledBack:
    case BootstrapExitCode::RolledBackHealthTimedOut:
    case BootstrapExitCode::RolledBackChildExited:
    case BootstrapExitCode::RolledBackHealthRejected:
        explanation.outcome = BootstrapExitOutcome::RolledBack;
        explanation.message = WithCode(
            u8"更新未能启动，已自动恢复到原来的版本。"
            u8"Harken Search 可以继续正常使用，你可以稍后再试一次更新。",
            exitCode);
        return explanation;

    // The release was refused before anything local changed.  Reinstalling the
    // same release cannot help; this needs a fix on the publishing side.
    case BootstrapExitCode::ChannelRejected:
    case BootstrapExitCode::PackageRejected:
        explanation.outcome = BootstrapExitOutcome::ReleaseRejected;
        explanation.message = WithCode(
            u8"这次的更新包没有通过校验，更新已安全取消，当前版本未被改动。"
            u8"请把这个错误码发给我们。",
            exitCode);
        return explanation;

    // Nothing was replaced.  A stray process or a transient admission
    // failure -- retrying genuinely works.
    case BootstrapExitCode::InvocationRejected:
    case BootstrapExitCode::AuthorityRejected:
    case BootstrapExitCode::AdmissionRejected:
    case BootstrapExitCode::OldProcessRejected:
    case BootstrapExitCode::OldProcessTimedOut:
    case BootstrapExitCode::LaunchRejected:
    case BootstrapExitCode::HealthRejected:
        explanation.outcome = BootstrapExitOutcome::Retryable;
        explanation.message = WithCode(
            u8"更新未能完成，当前版本未被改动。"
            u8"请完全退出 Harken Search（包括托盘图标）后再试一次。",
            exitCode);
        return explanation;

    // The old application has exited, but this code covers several native
    // failures. Name the stage without guessing or ruling out a cause; the
    // exact status is in bootstrap-last-failure.txt for support.
    case BootstrapExitCode::FilesystemRejected:
        explanation.outcome = BootstrapExitOutcome::Retryable;
        explanation.message = WithCode(
            u8"更新未能完成：准备文件阶段遇到问题，当前版本未被改动。"
            u8"请把这个错误码发给我们。",
            exitCode);
        return explanation;

    // The install is no longer in a known-good shape.  This is the only class
    // where reinstalling is the honest answer.
    case BootstrapExitCode::Quarantined:
    case BootstrapExitCode::QuarantinedRollbackFailed:
    case BootstrapExitCode::QuarantinedPreviousLaunchFailed:
    case BootstrapExitCode::QuarantinedChildTerminationFailed:
    case BootstrapExitCode::QuarantinedChildReleaseFailed:
    case BootstrapExitCode::QuarantinedRecoveryRejected:
    case BootstrapExitCode::QuarantinedMutationFailed:
    case BootstrapExitCode::QuarantinedHealthPrepareFailed:
    case BootstrapExitCode::QuarantinedUpdatedLaunchFailed:
    case BootstrapExitCode::QuarantinedUpdatedCheckpointFailed:
    case BootstrapExitCode::QuarantinedUpdatedResumeFailed:
    case BootstrapExitCode::QuarantinedHealthFailed:
    case BootstrapExitCode::QuarantinedCommitFailed:
    case BootstrapExitCode::QuarantinedSnapshotFailed:
    case BootstrapExitCode::QuarantinedReplacementFailed:
    case BootstrapExitCode::ReinstallRequired:
        explanation.outcome = BootstrapExitOutcome::Unrecoverable;
        explanation.requiresReinstall = true;
        explanation.message = WithCode(
            u8"更新未完成，且无法自动恢复。请卸载 Harken Search 后重新安装最新版本。\n\n"
            u8"数据库、设置和用户数据不会被删除。",
            exitCode);
        return explanation;

    default:
        break;
    }

    // Unrecognised: do not guess "reinstall".  Say what is known.
    explanation.outcome = BootstrapExitOutcome::Retryable;
    explanation.message = WithCode(
        u8"更新未能完成，原因暂时无法归类。当前版本应当未被改动，请稍后重试。"
        u8"若反复出现，请把这个错误码发给我们。",
        exitCode);
    return explanation;
}

std::string ResolveBootstrapStageFailureRecordPath(
    const std::string& installDirectory, const std::string& localAppData) {
    const auto base =
        Platform::UpdateBootstrapAuthority::ResolveUpdaterOwnedBaseForInstall(
            installDirectory, localAppData);
    return base.empty() ? std::string()
                        : base + "\\bootstrap-last-failure.txt";
}

std::string BootstrapStageFailureRecordPath() {
#ifdef _WIN32
    std::wstring executable(32768, L'\0');
    const DWORD executableLength = GetModuleFileNameW(
        nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    if (executableLength == 0 || executableLength >= executable.size()) {
        return {};
    }
    executable.resize(executableLength);

    PWSTR folder = nullptr;
    if (SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &folder) != S_OK ||
        folder == nullptr) {
        return {};
    }
    const std::filesystem::path localAppData(folder);
    CoTaskMemFree(folder);
    return ResolveBootstrapStageFailureRecordPath(
        std::filesystem::path(executable).parent_path().u8string(),
        localAppData.u8string());
#else
    return {};
#endif
}

void RecordBootstrapStageFailure(const char* stageName, int exitCode,
                                 const char* extra) {
    if (exitCode == static_cast<int>(BootstrapExitCode::Completed)) return;
    Platform::UpdatePersistence::RecordBootstrapFailureDiagnostic(
        stageName, exitCode, extra);
}

}  // namespace aris::bootstrap
