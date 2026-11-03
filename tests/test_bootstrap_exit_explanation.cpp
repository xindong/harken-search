// Bootstrap distinguishes 30+ exit codes, but every nonzero one showed the
// same dialog: "更新未完成。请卸载 Aris 后重新安装最新版本。"
//
// That advice is wrong for most of them.  RolledBack means the previous
// version is intact and still running.  OldProcessTimedOut means a stray
// process held the install and retrying works.  HealthRejected means the new
// build launched but did not report ready.  Reinstalling fixes none of these,
// which is exactly why users report "I reinstalled and it still doesn't work".
//
// The enum even has a dedicated ReinstallRequired code that the dialog ignored.
// These assertions pin that only genuinely unrecoverable states ask for a
// reinstall, and that recoverable states say so.

#include "bootstrap/HarkenSearchBootstrapExitExplanation.h"
#include "platform/update_persistence_port_test_hooks.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

int g_failures = 0;

void check(bool condition, const std::string& what) {
    if (!condition) {
        ++g_failures;
        std::printf("FAIL: %s\n", what.c_str());
    }
}

}  // namespace

int main(int argc, char** argv) {
    using namespace aris::bootstrap;

    const std::string localAppData = "C:\\Users\\Someone\\AppData\\Local";
    check(ResolveBootstrapStageFailureRecordPath(
              "C:\\Program Files\\Aris", localAppData) ==
              localAppData + "\\HarkenSearch\\updates\\bootstrap-last-failure.txt",
          "a C-drive diagnostic uses the current-user LocalAppData root");
    check(ResolveBootstrapStageFailureRecordPath(
              "D:\\Apps\\Aris", localAppData) ==
              "D:\\ProgramData\\HarkenSearch\\updates\\bootstrap-last-failure.txt",
          "a D-drive diagnostic follows the installation volume");
    check(ResolveBootstrapStageFailureRecordPath(
              "E:\\Tools\\Aris", localAppData) ==
              "E:\\ProgramData\\HarkenSearch\\updates\\bootstrap-last-failure.txt",
          "an E-drive diagnostic follows the installation volume");

    const auto diagnosticPath =
        std::filesystem::path(BootstrapStageFailureRecordPath());
    // CTest's isolated data directory may be on a different volume from the
    // binary. Diagnostics follow the installed executable, never that cwd.
    const auto executablePath = argc > 0
        ? std::filesystem::absolute(argv[0]) : std::filesystem::path();
    check(!diagnosticPath.empty() &&
              diagnosticPath.root_name() ==
                  executablePath.root_name(),
          "the diagnostic follows the Bootstrap installation volume");

    const auto scratch = std::filesystem::current_path() /
                         "bootstrap-diagnostic-contract";
    std::error_code cleanupError;
    std::filesystem::remove_all(scratch, cleanupError);
    const auto isolatedLocalAppData = scratch / "missing-parent";
    const auto isolatedInstall = scratch / "install";
    const auto isolatedPath = isolatedLocalAppData / "HarkenSearch" / "updates" /
                              "bootstrap-last-failure.txt";
    Platform::UpdatePersistence::Testing::
        WriteBootstrapFailureDiagnosticForInstall(
        isolatedInstall.u8string(), isolatedLocalAppData.u8string(),
        "filesystem-begin", 30, "root-volume-mismatch");
    check(std::filesystem::exists(isolatedPath),
          "a missing diagnostic parent is created for a real failure");
    std::ifstream firstRead(isolatedPath, std::ios::binary);
    const std::string first((std::istreambuf_iterator<char>(firstRead)),
                            std::istreambuf_iterator<char>());
    check(first.find("stage=filesystem-begin") != std::string::npos &&
              first.find("code=30") != std::string::npos,
          "the diagnostic records the actual failure outcome");

    Platform::UpdatePersistence::Testing::
        WriteBootstrapFailureDiagnosticForInstall(
        isolatedInstall.u8string(), isolatedLocalAppData.u8string(), "application",
        static_cast<int>(BootstrapExitCode::Completed),
        "commit-cleanup-pending");
    std::ifstream secondRead(isolatedPath, std::ios::binary);
    const std::string second((std::istreambuf_iterator<char>(secondRead)),
                             std::istreambuf_iterator<char>());
    check(second == first,
          "successful cleanup-pending outcomes do not overwrite last failure");
    std::filesystem::remove_all(scratch, cleanupError);

    check(ExplainBootstrapExit(static_cast<int>(BootstrapExitCode::Completed))
                  .outcome == BootstrapExitOutcome::Succeeded,
          "exit code 0 is a success, not a failure");

    // Rolled back: the installed version was restored and is usable.  Telling
    // the user to reinstall here is actively wrong.
    const int rolledBack[] = {
        static_cast<int>(BootstrapExitCode::RolledBack),
        static_cast<int>(BootstrapExitCode::RolledBackHealthTimedOut),
        static_cast<int>(BootstrapExitCode::RolledBackChildExited),
        static_cast<int>(BootstrapExitCode::RolledBackHealthRejected),
    };
    for (const int code : rolledBack) {
        const auto e = ExplainBootstrapExit(code);
        check(e.outcome == BootstrapExitOutcome::RolledBack,
              "rollback code " + std::to_string(code) + " reports a rollback");
        check(!e.requiresReinstall,
              "rollback code " + std::to_string(code) + " never demands a reinstall");
        check(!e.message.empty(),
              "rollback code " + std::to_string(code) + " explains itself");
    }

    // Retryable: nothing was replaced, trying again is the right next step.
    const int retryable[] = {
        static_cast<int>(BootstrapExitCode::OldProcessRejected),
        static_cast<int>(BootstrapExitCode::OldProcessTimedOut),
        static_cast<int>(BootstrapExitCode::AdmissionRejected),
        static_cast<int>(BootstrapExitCode::FilesystemRejected),
        static_cast<int>(BootstrapExitCode::LaunchRejected),
        static_cast<int>(BootstrapExitCode::HealthRejected),
        static_cast<int>(BootstrapExitCode::InvocationRejected),
        static_cast<int>(BootstrapExitCode::AuthorityRejected),
    };
    for (const int code : retryable) {
        const auto e = ExplainBootstrapExit(code);
        check(e.outcome == BootstrapExitOutcome::Retryable,
              "code " + std::to_string(code) + " is retryable");
        check(!e.requiresReinstall,
              "retryable code " + std::to_string(code) + " never demands a reinstall");
    }

    // Quarantined and the explicit ReinstallRequired are the genuinely broken
    // states: the install is no longer in a known-good shape.
    const int reinstall[] = {
        static_cast<int>(BootstrapExitCode::Quarantined),
        static_cast<int>(BootstrapExitCode::QuarantinedRollbackFailed),
        static_cast<int>(BootstrapExitCode::QuarantinedReplacementFailed),
        static_cast<int>(BootstrapExitCode::ReinstallRequired),
    };
    for (const int code : reinstall) {
        const auto e = ExplainBootstrapExit(code);
        check(e.requiresReinstall,
              "code " + std::to_string(code) + " genuinely requires a reinstall");
    }

    // A rejected package is a release-side problem; reinstalling the same
    // broken release changes nothing.
    for (const int code : {static_cast<int>(BootstrapExitCode::ChannelRejected),
                           static_cast<int>(BootstrapExitCode::PackageRejected)}) {
        const auto e = ExplainBootstrapExit(code);
        check(!e.requiresReinstall,
              "package/channel rejection " + std::to_string(code) +
                  " is not solved by reinstalling");
    }

    // Developer and unsupported builds must not shout at the user.
    for (const int code : {static_cast<int>(BootstrapExitCode::Unprovisioned),
                           static_cast<int>(BootstrapExitCode::Unsupported)}) {
        const auto e = ExplainBootstrapExit(code);
        check(!e.requiresReinstall && !e.showDialog,
              "unprovisioned/unsupported build " + std::to_string(code) +
                  " stays silent");
    }

    // Every explained failure must carry the raw code so support can act on it.
    const auto health =
        ExplainBootstrapExit(static_cast<int>(BootstrapExitCode::HealthRejected));
    check(health.message.find("50") != std::string::npos,
          "the raw exit code appears in the message for support");

    // An unknown code stays honest instead of guessing "reinstall".
    const auto unknown = ExplainBootstrapExit(9999);
    check(!unknown.requiresReinstall && !unknown.message.empty(),
          "an unrecognised code does not default to demanding a reinstall");

    // Error 30 used to tell users to quit the application and retry, but the
    // old application has always fully exited by the time the filesystem
    // stage rejects.  The message must name the real stage and must not
    // repeat the misleading quit-and-retry advice.
    {
        const int code = static_cast<int>(BootstrapExitCode::FilesystemRejected);
        const auto e = ExplainBootstrapExit(code);
        check(e.outcome == BootstrapExitOutcome::Retryable,
              "filesystem rejection stays retryable");
        check(!e.requiresReinstall,
              "filesystem rejection never demands a reinstall");
        check(e.message.find(std::to_string(code)) != std::string::npos,
              "filesystem rejection still carries the raw code");
        check(e.message.find(u8"\u51c6\u5907\u6587\u4ef6\u9636\u6bb5") != std::string::npos,
              "filesystem rejection names the file preparation stage");
        check(e.message.find(u8"\u8bf7\u5b8c\u5168\u9000\u51fa") == std::string::npos,
              "filesystem rejection no longer gives the misleading quit advice");
        check(e.message.find(u8"不是文件被占用") == std::string::npos,
              "a filesystem error code alone cannot exclude sharing conflicts");
        check(e.message.find(u8"未被改动") == std::string::npos,
              "filesystem rejection can follow a successful replacement and must not promise unchanged files");
    }

    if (g_failures == 0) std::printf("all bootstrap exit explanations pass\n");
    return g_failures == 0 ? 0 : 1;
}
