#include "platform/process_launch.h"

#include <cerrno>
#include <chrono>
#include <codecvt>
#include <csignal>
#include <fcntl.h>
#include <unistd.h>
#include <filesystem>
#include <fstream>
#include <locale>
#include <string>
#include <thread>
#include <cstdio>
#include <cstdlib>

namespace {
int failures = 0;
void Check(bool condition, const char* label) {
    if (!condition) { std::printf("FAIL: %s\n", label); ++failures; }
}

std::wstring Wide(const std::string& utf8) {
    std::wstring_convert<std::codecvt_utf8<wchar_t>> converter;
    return converter.from_bytes(utf8);
}

std::wstring Quoted(const std::wstring& text) {
    return L"\"" + text + L"\"";
}
}

int main(int argc, char** argv) {
    if (argc > 1 && std::string(argv[1]) == "--child-exit")
        return argc == 3 ? std::atoi(argv[2]) : 91;
    if (argc > 1 && std::string(argv[1]) == "--child-args")
        return argc == 4 && std::string(argv[2]) == "two words" &&
               std::string(argv[3]) == u8"中文" ? 0 : 92;
    if (argc > 1 && std::string(argv[1]) == "--child-sleep") {
        if (argc == 3) {
            std::ofstream marker(argv[2]); marker << getpid();
            if (!marker) return 96;
        }
        std::this_thread::sleep_for(std::chrono::seconds(2));
        return 0;
    }
    if (argc > 1 && std::string(argv[1]) == "--child-write") {
        if (argc != 3) return 93;
        std::ofstream marker(argv[2]); marker << "started";
        return marker ? 0 : 94;
    }
    if (argc > 1 && std::string(argv[1]) == "--child-fd") {
        if (argc != 4) return 97;
        const int descriptor = std::atoi(argv[2]);
        errno = 0;
        const int flags = fcntl(descriptor, F_GETFD);
        return std::string(argv[3]) == "closed"
            ? (flags < 0 && errno == EBADF ? 0 : 98)
            : (flags >= 0 ? 0 : 99);
    }
    if (argc != 2) return 95;

    namespace fs = std::filesystem;
    const fs::path root = fs::u8path(argv[1]);
    fs::create_directories(root);
    const fs::path child = root / fs::u8path(u8"child with space 中文");
    fs::copy_file(fs::absolute(argv[0]), child, fs::copy_options::overwrite_existing);
    fs::permissions(child, fs::perms::owner_exec | fs::perms::group_exec |
                           fs::perms::others_exec, fs::perm_options::add);
    const std::wstring executable = Wide(child.u8string());
    const std::wstring command = Quoted(executable);
    const Platform::ProcessLaunchOptions options{};

    const auto success = Platform::RunProcessAndWait(
        executable, command + L" --child-exit 7", options, 3000, 99);
    Check(success.status == Platform::ProcessRunStatus::Completed &&
          success.exitCode == 7, "nonzero child exit is reported");

    const auto arguments = Platform::RunProcessAndWait(
        executable, command + L" --child-args \"two words\" \"中文\"",
        options, 3000, 99);
    Check(arguments.status == Platform::ProcessRunStatus::Completed &&
          arguments.exitCode == 0, "quoted and Unicode arguments survive");

    const int descriptor = open((root / "inherited-fd.txt").c_str(), O_CREAT | O_RDWR, 0600);
    Check(descriptor >= 3, "test descriptor opened");
    if (descriptor >= 3) {
        fcntl(descriptor, F_SETFD, 0);
        const std::wstring descriptorArgument = std::to_wstring(descriptor);
        const auto closed = Platform::RunProcessAndWait(
            executable, command + L" --child-fd " + descriptorArgument + L" closed",
            options, 3000, 99);
        Check(closed.status == Platform::ProcessRunStatus::Completed &&
              closed.exitCode == 0, "handles are closed by default");
        Platform::ProcessLaunchOptions inheritOptions;
        inheritOptions.inheritHandles = true;
        const auto inherited = Platform::RunProcessAndWait(
            executable, command + L" --child-fd " + descriptorArgument + L" open",
            inheritOptions, 3000, 99);
        Check(inherited.status == Platform::ProcessRunStatus::Completed &&
              inherited.exitCode == 0, "inheritHandles preserves descriptor");
        close(descriptor);
    }

    const auto timeoutMarker = root / ("timeout-" + std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count()));
    const auto started = std::chrono::steady_clock::now();
    const auto timed = Platform::RunProcessAndWait(
        executable, command + L" --child-sleep " + Quoted(Wide(timeoutMarker.u8string())),
        options, 200, 42);
    const auto elapsed = std::chrono::steady_clock::now() - started;
    Check(timed.status == Platform::ProcessRunStatus::TimedOut &&
          timed.exitCode == 42 && elapsed < std::chrono::seconds(1),
          "timeout terminates the child promptly");
    Check(fs::exists(timeoutMarker), "timed child started before termination");
    if (fs::exists(timeoutMarker)) {
        pid_t timedPid = 0;
        std::ifstream markerStream(timeoutMarker);
        markerStream >> timedPid;
        errno = 0;
        Check(timedPid > 0 && kill(timedPid, 0) < 0 && errno == ESRCH,
              "timed child was reaped");
    }

    const auto missing = Platform::RunProcessAndWait(
        L"/does/not/exist/harken-helper", L"", options, 1000, 99);
    Check(missing.status == Platform::ProcessRunStatus::LaunchFailed &&
          missing.systemError == ENOENT, "missing executable reports ENOENT");
    Check(Platform::LaunchProcess(L"", L"", options).status ==
          Platform::ProcessLaunchStatus::InvalidArgument,
          "empty executable rejected");
    const wchar_t embeddedNul[] = {L'/', L'b', L'i', L'n', L'/', L's', L'h', L'\0', L'x'};
    Check(Platform::LaunchProcess(std::wstring_view(embeddedNul, 9), L"", options).status ==
          Platform::ProcessLaunchStatus::InvalidArgument,
          "embedded NUL rejected");
    Check(Platform::LaunchProcess(executable, command + L" \"unfinished", options).status ==
          Platform::ProcessLaunchStatus::InvalidArgument,
          "unmatched quote rejected");

    const auto marker = root / ("marker-" + std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count()));
    const auto launched = Platform::LaunchProcess(
        executable, command + L" --child-write " + Quoted(Wide(marker.u8string())), options);
    Check(launched.status == Platform::ProcessLaunchStatus::Ok &&
          launched.processId > 0, "detached launch returns process ID");
    for (int attempt = 0; attempt < 200 && !fs::exists(marker); ++attempt)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    Check(fs::exists(marker), "detached child ran");
    if (launched.processId > 0) {
        bool reaped = false;
        for (int attempt = 0; attempt < 200; ++attempt) {
            errno = 0;
            if (kill(static_cast<pid_t>(launched.processId), 0) < 0 && errno == ESRCH) {
                reaped = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        Check(reaped, "detached child is reaped");
    }
    std::printf("Mac process launch checks: %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
