#ifdef __APPLE__

#include "process_launch.h"

#include <spawn.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <codecvt>
#include <cwctype>
#include <locale>
#include <string>
#include <thread>
#include <vector>

extern char** environ;

namespace Platform {
namespace {

bool ToUtf8(std::wstring_view value, std::string& output) {
    try {
        std::wstring_convert<std::codecvt_utf8<wchar_t>> converter;
        output = converter.to_bytes(value.data(), value.data() + value.size());
        return true;
    } catch (const std::range_error&) {
        return false;
    }
}

// Parse the quoted CreateProcessW-style command line into argv. Never invoke a shell.
bool ParseCommandLine(std::wstring_view commandLine,
                      std::vector<std::string>& arguments) {
    std::size_t index = 0;
    while (index < commandLine.size()) {
        while (index < commandLine.size() && std::iswspace(commandLine[index])) ++index;
        if (index == commandLine.size()) break;
        std::wstring argument;
        bool quoted = false;
        while (index < commandLine.size()) {
            const wchar_t current = commandLine[index];
            if (!quoted && std::iswspace(current)) break;
            if (current == L'\\') {
                const std::size_t begin = index;
                while (index < commandLine.size() && commandLine[index] == L'\\') ++index;
                const std::size_t count = index - begin;
                if (index < commandLine.size() && commandLine[index] == L'"') {
                    argument.append(count / 2, L'\\');
                    if (count % 2 != 0) argument.push_back(L'"');
                    else quoted = !quoted;
                    ++index;
                } else {
                    argument.append(count, L'\\');
                }
                continue;
            }
            if (current == L'"') {
                if (quoted && index + 1 < commandLine.size() &&
                    commandLine[index + 1] == L'"') {
                    argument.push_back(L'"');
                    index += 2;
                } else {
                    quoted = !quoted;
                    ++index;
                }
                continue;
            }
            argument.push_back(current);
            ++index;
        }
        if (quoted) return false;
        std::string utf8;
        if (!ToUtf8(argument, utf8)) return false;
        arguments.push_back(std::move(utf8));
    }
    return true;
}

int Spawn(std::wstring_view applicationName,
          std::wstring_view commandLine,
          const ProcessLaunchOptions& options,
          pid_t& processId) {
    if (applicationName.empty() ||
        applicationName.find(L'\0') != std::wstring_view::npos ||
        commandLine.find(L'\0') != std::wstring_view::npos) return EINVAL;
    std::string executable;
    if (!ToUtf8(applicationName, executable) || executable.empty()) return EINVAL;
    std::vector<std::string> arguments;
    if (!ParseCommandLine(commandLine, arguments)) return EINVAL;
    if (arguments.empty()) arguments.push_back(executable);
    std::vector<char*> argv;
    argv.reserve(arguments.size() + 1);
    for (auto& argument : arguments) argv.push_back(argument.data());
    argv.push_back(nullptr);

    posix_spawnattr_t attributes;
    int error = posix_spawnattr_init(&attributes);
    if (error != 0) return error;
    short flags = 0;
    if (!options.inheritHandles) flags |= POSIX_SPAWN_CLOEXEC_DEFAULT;
    error = posix_spawnattr_setflags(&attributes, flags);
    if (error == 0) {
        // Spawning a child does not open a console on macOS; noWindow needs no flag.
        error = executable.find('/') == std::string::npos
            ? posix_spawnp(&processId, executable.c_str(), nullptr, &attributes,
                           argv.data(), environ)
            : posix_spawn(&processId, executable.c_str(), nullptr, &attributes,
                          argv.data(), environ);
    }
    posix_spawnattr_destroy(&attributes);
    return error;
}

int ExitCode(int status) {
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return 1;
}

} // namespace

ProcessLaunchResult LaunchProcess(
    std::wstring_view applicationName,
    std::wstring_view commandLine,
    const ProcessLaunchOptions& options) {
    ProcessLaunchResult result;
    pid_t processId = 0;
    const int error = Spawn(applicationName, commandLine, options, processId);
    if (error != 0) {
        result.status = error == EINVAL ? ProcessLaunchStatus::InvalidArgument
                                        : ProcessLaunchStatus::Failed;
        result.systemError = static_cast<unsigned long>(error);
        return result;
    }
    try {
        // A detached launch still needs a waiter on Unix to avoid a zombie child.
        std::thread([processId] {
            int status = 0;
            while (waitpid(processId, &status, 0) < 0 && errno == EINTR) {}
        }).detach();
    } catch (...) {
        kill(processId, SIGKILL);
        int status = 0;
        while (waitpid(processId, &status, 0) < 0 && errno == EINTR) {}
        result.status = ProcessLaunchStatus::Failed;
        result.systemError = EAGAIN;
        return result;
    }
    result.status = ProcessLaunchStatus::Ok;
    result.processId = static_cast<uint32_t>(processId);
    return result;
}

ProcessRunResult RunProcessAndWait(
    std::wstring_view applicationName,
    std::wstring_view commandLine,
    const ProcessLaunchOptions& options,
    std::uint32_t timeoutMs,
    std::uint32_t timeoutTerminateExitCode) {
    ProcessRunResult result;
    pid_t processId = 0;
    const int spawnError = Spawn(applicationName, commandLine, options, processId);
    if (spawnError != 0) {
        result.status = ProcessRunStatus::LaunchFailed;
        result.systemError = static_cast<unsigned long>(spawnError);
        return result;
    }
    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::milliseconds(timeoutMs);
    while (true) {
        int status = 0;
        const pid_t waited = waitpid(processId, &status, timeoutMs == 0 ? 0 : WNOHANG);
        if (waited == processId) {
            result.status = ProcessRunStatus::Completed;
            result.exitCode = ExitCode(status);
            return result;
        }
        if (waited < 0 && errno != EINTR) {
            result.status = ProcessRunStatus::WaitFailed;
            result.systemError = static_cast<unsigned long>(errno);
            return result;
        }
        if (timeoutMs != 0 && std::chrono::steady_clock::now() >= deadline) {
            if (kill(processId, SIGKILL) != 0 && errno != ESRCH)
                result.systemError = static_cast<unsigned long>(errno);
            while (waitpid(processId, &status, 0) < 0 && errno == EINTR) {}
            result.status = ProcessRunStatus::TimedOut;
            result.exitCode = static_cast<int>(timeoutTerminateExitCode);
            return result;
        }
        if (timeoutMs != 0) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

} // namespace Platform

#endif
