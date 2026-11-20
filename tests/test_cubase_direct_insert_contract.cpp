#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "src/CubaseAdapter.h"
#include "src/AppState.h"
#include "src/platform/filesystem.h"
#include "src/platform/executable_path.h"
#include "src/platform/named_pipe_client_test_seam.h"
#include "tests/process_launch_test_seam.h"
#include "src/platform/system.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <cstdlib>
#include <atomic>
#include <cstring>
#include <map>
#include <sstream>
#include <vector>
#include <string>

static int failed = 0;
static std::atomic<bool> g_serverRunning{false};
enum class MockBridgeMode {
    HappyPath,
    HappyPathNoInsertSession,
    HostNotReadyHello,
    PartialSuccess,
    SnapshotMismatch,
    SessionMismatch,
    TrackUnavailable,
};
static std::atomic<MockBridgeMode> g_bridgeMode{MockBridgeMode::HappyPath};

namespace HostSendPlatformStub {
void Reset();
void SetEnvironmentResult(Platform::EnvironmentStringResult result);
void ResetSleepCalls();
const std::vector<uint32_t>& SleepCalls();
}

namespace CubaseExecutablePathStub {
void Reset(const std::string& testExecutablePathUtf8);
void SetResult(Platform::ExecutablePathResult result);
}

namespace HarkenSearch::CubaseAdapter::Testing {
using BridgeLaunchHook = bool (*)(const std::wstring&, const std::wstring&, unsigned long&);
void SetBridgeLaunchHook(BridgeLaunchHook hook);
}

static int g_launchHookCalls = 0;
static std::wstring g_launchHookExecutable;
static std::wstring g_launchHookArguments;

static bool CaptureBridgeLaunch(const std::wstring& executable,
                                const std::wstring& arguments,
                                unsigned long& processId) {
    ++g_launchHookCalls;
    g_launchHookExecutable = executable;
    g_launchHookArguments = arguments;
    processId = 7331;
    return true;
}

static void ResetBridgeLaunchCapture() {
    g_launchHookCalls = 0;
    g_launchHookExecutable.clear();
    g_launchHookArguments.clear();
}

struct ProcessLaunchScript {
    bool createOk = true;
    bool closeResult = true;
    DWORD error = ERROR_SUCCESS;
    DWORD processId = 8123;
    HANDLE processHandle = reinterpret_cast<HANDLE>(0x101);
    HANDLE threadHandle = reinterpret_cast<HANDLE>(0x202);
    int createCalls = 0;
    std::wstring applicationName;
    std::wstring commandLine;
    LPSECURITY_ATTRIBUTES processAttributes = reinterpret_cast<LPSECURITY_ATTRIBUTES>(1);
    LPSECURITY_ATTRIBUTES threadAttributes = reinterpret_cast<LPSECURITY_ATTRIBUTES>(1);
    BOOL inheritHandles = TRUE;
    DWORD creationFlags = 0;
    LPVOID environment = reinterpret_cast<LPVOID>(1);
    LPCWSTR currentDirectory = reinterpret_cast<LPCWSTR>(1);
    DWORD startupCb = 0;
    std::vector<HANDLE> closed;
} g_processLaunch;

static BOOL CaptureProcessLaunch(
    LPCWSTR applicationName, LPWSTR commandLine,
    LPSECURITY_ATTRIBUTES processAttributes, LPSECURITY_ATTRIBUTES threadAttributes,
    BOOL inheritHandles, DWORD creationFlags, LPVOID environment,
    LPCWSTR currentDirectory, LPSTARTUPINFOW startupInfo,
    LPPROCESS_INFORMATION processInfo) {
    ++g_processLaunch.createCalls;
    g_processLaunch.applicationName = applicationName ? applicationName : L"";
    g_processLaunch.commandLine = commandLine ? commandLine : L"";
    g_processLaunch.processAttributes = processAttributes;
    g_processLaunch.threadAttributes = threadAttributes;
    g_processLaunch.inheritHandles = inheritHandles;
    g_processLaunch.creationFlags = creationFlags;
    g_processLaunch.environment = environment;
    g_processLaunch.currentDirectory = currentDirectory;
    g_processLaunch.startupCb = startupInfo ? startupInfo->cb : 0;
    processInfo->dwProcessId = g_processLaunch.processId;
    processInfo->hProcess = g_processLaunch.processHandle;
    processInfo->hThread = g_processLaunch.threadHandle;
    return g_processLaunch.createOk ? TRUE : FALSE;
}
static DWORD ProcessLaunchLastError() { return g_processLaunch.error; }
static BOOL CaptureProcessClose(HANDLE handle) {
    g_processLaunch.closed.push_back(handle);
    return g_processLaunch.closeResult ? TRUE : FALSE;
}
static const Platform::ProcessLaunchTesting::BackendOps kProcessLaunchOps{
    CaptureProcessLaunch, ProcessLaunchLastError, CaptureProcessClose};

static void ResetProcessLaunch(bool createOk = true, DWORD error = ERROR_SUCCESS) {
    g_processLaunch = {};
    g_processLaunch.createOk = createOk;
    g_processLaunch.error = error;
    g_processLaunch.processId = 8123;
    g_processLaunch.processHandle = reinterpret_cast<HANDLE>(0x101);
    g_processLaunch.threadHandle = reinterpret_cast<HANDLE>(0x202);
}

static std::string CurrentProcessPathUtf8() {
    char buf[MAX_PATH] = {};
    DWORD len = GetModuleFileNameA(NULL, buf, MAX_PATH);
    return len > 0 ? std::string(buf, len) : std::string();
}

static std::string ReplaceFilenameUtf8(const std::string& path, const std::string& fileName) {
    const auto pos = path.find_last_of("/\\");
    return pos == std::string::npos ? fileName : path.substr(0, pos + 1) + fileName;
}

static std::string BuiltBridgeHelperPathUtf8() {
    return ReplaceFilenameUtf8(CurrentProcessPathUtf8(), "HarkenSearchCubaseBridgeStub.exe");
}

static std::string BuiltNuendoBridgeHelperPathUtf8() {
    return ReplaceFilenameUtf8(CurrentProcessPathUtf8(), "HarkenSearchNuendoBridgeStub.exe");
}

static std::wstring BuiltBridgeHelperPathWide(const wchar_t* fileName) {
    wchar_t path[32768] = {};
    const DWORD length = GetModuleFileNameW(nullptr, path, 32768);
    if (length == 0 || length >= 32768) return {};
    std::wstring result(path, length);
    const size_t slash = result.find_last_of(L"/\\");
    result.resize(slash == std::wstring::npos ? 0 : slash + 1);
    result += fileName;
    return result;
}

struct HelperHandshakeResult {
    bool tokenAccepted = false;
    bool mismatchRejected = false;
    bool responseFraming = false;
    bool shutdownExited = false;
};

static HelperHandshakeResult RunUnicodeArgvHandshake(const wchar_t* fileName,
                                                      const wchar_t* pipeLabel) {
    const std::wstring helperPath = BuiltBridgeHelperPathWide(fileName);
    const std::wstring pipeName = L"\\\\.\\pipe\\auro_stub_argv_" +
        std::wstring(pipeLabel) + L"_" + std::to_wstring(GetCurrentProcessId());
    std::wstring command = L"\"" + helperPath + L"\" --pipe " + pipeName +
        L" --token \"令牌🎧\"";
    std::vector<wchar_t> commandLine(command.begin(), command.end());
    commandLine.push_back(L'\0');

    STARTUPINFOW startup = {};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process = {};
    if (helperPath.empty() ||
        !CreateProcessW(helperPath.c_str(), commandLine.data(), nullptr, nullptr,
                        FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)) {
        return {};
    }

    const auto exchange = [&](const std::string& request) {
        HANDLE pipe = INVALID_HANDLE_VALUE;
        const ULONGLONG connectDeadline = GetTickCount64() + 5000;
        while (pipe == INVALID_HANDLE_VALUE && GetTickCount64() < connectDeadline) {
            pipe = CreateFileW(pipeName.c_str(), GENERIC_READ | GENERIC_WRITE,
                               0, nullptr, OPEN_EXISTING, 0, nullptr);
            const DWORD openError =
                pipe == INVALID_HANDLE_VALUE ? GetLastError() : ERROR_SUCCESS;
            if (pipe != INVALID_HANDLE_VALUE ||
                WaitForSingleObject(process.hProcess, 0) == WAIT_OBJECT_0) {
                break;
            }
            if (openError == ERROR_PIPE_BUSY) {
                WaitNamedPipeW(pipeName.c_str(), 200);
            } else {
                Sleep(25);
            }
        }
        if (pipe == INVALID_HANDLE_VALUE) return std::string();

        DWORD written = 0;
        char response[512] = {};
        DWORD read = 0;
        const bool exchanged =
            WriteFile(pipe, request.data(), static_cast<DWORD>(request.size()),
                      &written, nullptr) &&
            written == static_cast<DWORD>(request.size()) &&
            ReadFile(pipe, response, sizeof(response) - 1, &read, nullptr);
        CloseHandle(pipe);
        return exchanged ? std::string(response, read) : std::string();
    };

    const std::string accepted = exchange(u8"HELLO|令牌🎧\n");
    const std::string rejected = exchange(u8"HELLO|令牌🎶\n");
    const std::string shutdown = exchange("HELLO|shutdown\n");
    HelperHandshakeResult result;
    result.tokenAccepted =
        accepted.rfind("OK|HELLO|", 0) == 0 &&
        accepted.find("|session=auro-helper-session-") != std::string::npos &&
        accepted.find("|snapshotToken=snap-demo-001|") != std::string::npos;
    result.mismatchRejected =
        rejected == "ERR|SESSION_MISMATCH|handshake-token-mismatch\n";
    result.responseFraming =
        !accepted.empty() && accepted.back() == '\n' &&
        !shutdown.empty() && shutdown.back() == '\n' &&
        shutdown.rfind("OK|HELLO|", 0) == 0;

    const bool exited = WaitForSingleObject(process.hProcess, 2000) == WAIT_OBJECT_0;
    if (!exited) {
        TerminateProcess(process.hProcess, 1);
        WaitForSingleObject(process.hProcess, 2000);
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    result.shutdownExited = exited;
    return result;
}

static void SetBridgeStubPath(const std::string& path) {
#ifdef _WIN32
    _putenv_s("AURO_CUBASE_BRIDGE_STUB_PATH", path.c_str());
    _putenv_s("AURO_STEINBERG_BRIDGE_STUB_PATH", path.c_str());
#else
    setenv("AURO_CUBASE_BRIDGE_STUB_PATH", path.c_str(), 1);
    setenv("AURO_STEINBERG_BRIDGE_STUB_PATH", path.c_str(), 1);
#endif
}

static void SetBridgeStubFlagOnly() {
#ifdef _WIN32
    _putenv_s("AURO_CUBASE_BRIDGE_STUB", "1");
    _putenv_s("AURO_STEINBERG_BRIDGE_STUB", "1");
    _putenv_s("AURO_CUBASE_BRIDGE_STUB_PATH", "");
    _putenv_s("AURO_STEINBERG_BRIDGE_STUB_PATH", "");
#else
    setenv("AURO_CUBASE_BRIDGE_STUB", "1", 1);
    setenv("AURO_STEINBERG_BRIDGE_STUB", "1", 1);
    unsetenv("AURO_CUBASE_BRIDGE_STUB_PATH");
    unsetenv("AURO_STEINBERG_BRIDGE_STUB_PATH");
#endif
}

static void SetBridgeStubEnvResolved() {
#ifdef _WIN32
    _putenv_s("AURO_CUBASE_BRIDGE_STUB", "1");
    SetBridgeStubPath(CurrentProcessPathUtf8());
    _putenv_s("AURO_STEINBERG_BRIDGE_STUB", "1");
#else
    setenv("AURO_CUBASE_BRIDGE_STUB", "1", 1);
    setenv("AURO_STEINBERG_BRIDGE_STUB", "1", 1);
    setenv("AURO_CUBASE_BRIDGE_STUB_PATH", CurrentProcessPathUtf8().c_str(), 1);
    setenv("AURO_STEINBERG_BRIDGE_STUB_PATH", CurrentProcessPathUtf8().c_str(), 1);
#endif
}

static void SetBridgeHelperEnvResolved() {
#ifdef _WIN32
    _putenv_s("AURO_CUBASE_BRIDGE_STUB", "1");
    SetBridgeStubPath(BuiltBridgeHelperPathUtf8());
    _putenv_s("AURO_STEINBERG_BRIDGE_STUB", "1");
#else
    setenv("AURO_CUBASE_BRIDGE_STUB", "1", 1);
    setenv("AURO_STEINBERG_BRIDGE_STUB", "1", 1);
    setenv("AURO_CUBASE_BRIDGE_STUB_PATH", BuiltBridgeHelperPathUtf8().c_str(), 1);
    setenv("AURO_STEINBERG_BRIDGE_STUB_PATH", BuiltBridgeHelperPathUtf8().c_str(), 1);
#endif
}

static void SetNuendoBridgeHelperEnvResolved() {
#ifdef _WIN32
    _putenv_s("AURO_NUENDO_BRIDGE_STUB", "1");
    _putenv_s("AURO_STEINBERG_BRIDGE_STUB", "1");
    _putenv_s("AURO_NUENDO_BRIDGE_STUB_PATH", BuiltNuendoBridgeHelperPathUtf8().c_str());
    _putenv_s("AURO_STEINBERG_BRIDGE_STUB_PATH", BuiltNuendoBridgeHelperPathUtf8().c_str());
#else
    setenv("AURO_NUENDO_BRIDGE_STUB", "1", 1);
    setenv("AURO_STEINBERG_BRIDGE_STUB", "1", 1);
    setenv("AURO_NUENDO_BRIDGE_STUB_PATH", BuiltNuendoBridgeHelperPathUtf8().c_str(), 1);
    setenv("AURO_STEINBERG_BRIDGE_STUB_PATH", BuiltNuendoBridgeHelperPathUtf8().c_str(), 1);
#endif
}

static void ClearBridgeStubEnv() {
#ifdef _WIN32
    _putenv_s("AURO_CUBASE_BRIDGE_STUB", "");
    _putenv_s("AURO_CUBASE_BRIDGE_STUB_PATH", "");
    _putenv_s("AURO_CUBASE_BRIDGE_LAUNCH_STUB", "");
    _putenv_s("AURO_NUENDO_BRIDGE_STUB", "");
    _putenv_s("AURO_NUENDO_BRIDGE_STUB_PATH", "");
    _putenv_s("AURO_NUENDO_BRIDGE_LAUNCH_STUB", "");
    _putenv_s("AURO_STEINBERG_BRIDGE_STUB", "");
    _putenv_s("AURO_STEINBERG_BRIDGE_STUB_PATH", "");
    _putenv_s("AURO_STEINBERG_BRIDGE_LAUNCH_STUB", "");
#else
    unsetenv("AURO_CUBASE_BRIDGE_STUB");
    unsetenv("AURO_CUBASE_BRIDGE_STUB_PATH");
    unsetenv("AURO_CUBASE_BRIDGE_LAUNCH_STUB");
    unsetenv("AURO_NUENDO_BRIDGE_STUB");
    unsetenv("AURO_NUENDO_BRIDGE_STUB_PATH");
    unsetenv("AURO_NUENDO_BRIDGE_LAUNCH_STUB");
    unsetenv("AURO_STEINBERG_BRIDGE_STUB");
    unsetenv("AURO_STEINBERG_BRIDGE_STUB_PATH");
    unsetenv("AURO_STEINBERG_BRIDGE_LAUNCH_STUB");
#endif
}

static void SetBridgeLaunchStub(const char* value) {
#ifdef _WIN32
    _putenv_s("AURO_CUBASE_BRIDGE_LAUNCH_STUB", value);
    _putenv_s("AURO_NUENDO_BRIDGE_LAUNCH_STUB", value);
    _putenv_s("AURO_STEINBERG_BRIDGE_LAUNCH_STUB", value);
#else
    setenv("AURO_CUBASE_BRIDGE_LAUNCH_STUB", value, 1);
    setenv("AURO_NUENDO_BRIDGE_LAUNCH_STUB", value, 1);
    setenv("AURO_STEINBERG_BRIDGE_LAUNCH_STUB", value, 1);
#endif
}

static void KillBridgeHelperProcesses() {
#ifdef _WIN32
    for (const char* imageName : {
             "HarkenSearchCubaseBridgeStub.exe",
             "HarkenSearchNuendoBridgeStub.exe",
             "HarkenSearchSteinbergBridgeStub.exe",
         }) {
        STARTUPINFOA si = {};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi = {};
        char cmd[256] = {};
        sprintf_s(cmd, "cmd.exe /c taskkill /IM %s /F >nul 2>&1", imageName);
        if (CreateProcessA(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
            WaitForSingleObject(pi.hProcess, 2000);
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        }
    }
#endif
}

static bool WaitForBridgeToStop(const HarkenSearch::CubaseAdapter::BridgeEndpoint& endpoint,
                                int timeoutMs = 1500) {
    const int pollMs = 100;
    int waitedMs = 0;
    while (waitedMs <= timeoutMs) {
        const auto probe = HarkenSearch::CubaseAdapter::ProbeBridgeEndpoint(endpoint);
        if (!probe.available) {
            return true;
        }
        Sleep(pollMs);
        waitedMs += pollMs;
    }
    return false;
}

static DWORD WINAPI MockCubaseBridgeServer(LPVOID) {
    const wchar_t* pipeName = L"\\\\.\\pipe\\auro_cubase_direct_insert_v1";
    while (g_serverRunning.load()) {
        HANDLE hPipe = CreateNamedPipeW(
            pipeName,
            PIPE_ACCESS_DUPLEX,
            PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
            PIPE_UNLIMITED_INSTANCES,
            4096, 4096, 100, NULL);
        if (hPipe == INVALID_HANDLE_VALUE) {
            Sleep(25);
            continue;
        }
        if (!ConnectNamedPipe(hPipe, NULL) && GetLastError() != ERROR_PIPE_CONNECTED) {
            CloseHandle(hPipe);
            continue;
        }

        char buf[4096] = {};
        DWORD bytesRead = 0;
        if (ReadFile(hPipe, buf, sizeof(buf) - 1, &bytesRead, NULL) && bytesRead > 0) {
            buf[bytesRead] = '\0';
            std::string msg(buf);
            while (!msg.empty() && (msg.back() == '\n' || msg.back() == '\r'))
                msg.pop_back();

            const char* resp = "ERR|BRIDGE_UNAVAILABLE|Unknown command\n";
            if (msg.rfind("HELLO|", 0) == 0) {
                switch (g_bridgeMode.load()) {
                case MockBridgeMode::HostNotReadyHello:
                    resp = "OK|HELLO|session=auro-test-session|snapshotToken=snap-mock-001|projectName=MockProjectA|selectedTrack=Missing Track|cursorLabel=Bar 9 Beat 3|projectState=session-mismatch|trackState=missing|supportsSelectedTrack=0|supportsCursorTrack=1|supportsAppendToTrack=1|supportsSpreadAcrossSelection=1\n";
                    break;
                default:
                    resp = "OK|HELLO|session=auro-test-session|snapshotToken=snap-mock-001|projectName=MockProjectA|selectedTrack=FX 01|cursorLabel=Bar 9 Beat 3|projectState=ready|trackState=selected-ready|supportsSelectedTrack=1|supportsCursorTrack=1|supportsAppendToTrack=1|supportsSpreadAcrossSelection=1\n";
                    break;
                }
            } else if (msg.rfind("INSERT|", 0) == 0) {
                const auto fields = [&]() {
                    std::map<std::string, std::string> parsed;
                    const auto tokens = [&]() {
                        std::vector<std::string> out;
                        std::stringstream ss(msg);
                        std::string token;
                        while (std::getline(ss, token, '|')) out.push_back(token);
                        return out;
                    }();
                    for (size_t i = 2; i < tokens.size(); ++i) {
                        const auto pos = tokens[i].find('=');
                        if (pos == std::string::npos || pos == 0 || pos + 1 >= tokens[i].size()) continue;
                        parsed[tokens[i].substr(0, pos)] = tokens[i].substr(pos + 1);
                    }
                    return parsed;
                }();
                const std::string requestedSnapshot = fields.count("snapshot") ? fields.at("snapshot") : std::string();
                const std::string requestedProject = fields.count("projectName") ? fields.at("projectName") : std::string();
                const std::string requestedSelectedTrack = fields.count("selectedTrack") ? fields.at("selectedTrack") : std::string();
                const std::string requestedCursor = fields.count("cursorLabel") ? fields.at("cursorLabel") : std::string();
                if (!requestedSnapshot.empty() && requestedSnapshot != "snap-mock-001") {
                    resp = "ERR|BRIDGE_NOT_READY|snapshot-mismatch\n";
                } else if (!requestedProject.empty() && requestedProject != "MockProjectA") {
                    resp = "ERR|BRIDGE_NOT_READY|project-mismatch\n";
                } else if (!requestedSelectedTrack.empty() && requestedSelectedTrack != "FX 01") {
                    resp = "ERR|TRACK_UNAVAILABLE|selected-track-changed\n";
                } else if (!requestedCursor.empty() && requestedCursor != "Bar 9 Beat 3") {
                    resp = "ERR|BRIDGE_NOT_READY|cursor-mismatch\n";
                } else {
                switch (g_bridgeMode.load()) {
                case MockBridgeMode::HappyPath:
                    resp = "OK|INSERT|session=auro-test-session|snapshotToken=snap-mock-001|projectName=MockProjectA|selectedTrack=FX 01|cursorLabel=Bar 9 Beat 3|projectState=ready|trackState=selected-ready|track=FX 01|position=edit-cursor|count=1|clips=clip-001\n";
                    break;
                case MockBridgeMode::HappyPathNoInsertSession:
                    resp = "OK|INSERT|projectName=MockProjectA|selectedTrack=FX 01|cursorLabel=Bar 9 Beat 3|projectState=ready|trackState=selected-ready|track=FX 01|position=edit-cursor|count=1|clips=clip-001\n";
                    break;
                case MockBridgeMode::PartialSuccess:
                    resp = "OK|INSERT|session=auro-test-session|snapshotToken=snap-mock-001|projectName=MockProjectA|selectedTrack=FX 01|cursorLabel=Bar 9 Beat 3|projectState=ready|trackState=remapped|track=FX 01|remappedTrack=FX ALT|position=next-grid|adjusted=1|count=1|clips=clip-001|failed=C:\\audio\\riser.wav\n";
                    break;
                case MockBridgeMode::SnapshotMismatch:
                    resp = "ERR|BRIDGE_NOT_READY|snapshot-mismatch\n";
                    break;
                case MockBridgeMode::SessionMismatch:
                    resp = "ERR|SESSION_MISMATCH|project-session-changed\n";
                    break;
                case MockBridgeMode::TrackUnavailable:
                    resp = "ERR|TRACK_UNAVAILABLE|selected-track-not-found\n";
                    break;
                }
                }
            }
            DWORD written = 0;
            WriteFile(hPipe, resp, (DWORD)strlen(resp), &written, NULL);
            FlushFileBuffers(hPipe);
        }

        DisconnectNamedPipe(hPipe);
        CloseHandle(hPipe);
    }
    return 0;
}

static HANDLE StartMockBridgeServer(MockBridgeMode mode = MockBridgeMode::HappyPath) {
    g_serverRunning.store(true);
    g_bridgeMode.store(mode);
    HANDLE thread = CreateThread(NULL, 0, MockCubaseBridgeServer, NULL, 0, NULL);
    Sleep(50);
    return thread;
}

static void StopMockBridgeServer(HANDLE thread) {
    g_serverRunning.store(false);
    HANDLE hTmp = CreateFileW(L"\\\\.\\pipe\\auro_cubase_direct_insert_v1",
                              GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (hTmp != INVALID_HANDLE_VALUE) {
        const char* ping = "HELLO|shutdown\n";
        DWORD w = 0;
        WriteFile(hTmp, ping, (DWORD)strlen(ping), &w, NULL);
        CloseHandle(hTmp);
    }
    if (thread) {
        WaitForSingleObject(thread, 2000);
        CloseHandle(thread);
    }
}

static void check(bool cond, const char* msg) {
    if (cond) {
        std::cout << "PASS: " << msg << "\n";
    } else {
        std::cout << "FAIL: " << msg << "\n";
        ++failed;
    }
}

struct PipeOpenStep { std::uintptr_t handle; unsigned long error; };
struct PipePeekStep { bool ok; std::uint32_t available; };
struct PipeReadStep { bool ok; std::string data; std::uint32_t reported; };
struct PipeClientScript {
    std::vector<PipeOpenStep> opens;
    std::vector<PipePeekStep> peeks;
    std::vector<PipeReadStep> reads;
    std::vector<std::uint32_t> ticks;
    std::vector<std::uint32_t> sleeps;
    std::vector<std::uint32_t> waits;
    std::vector<std::uint32_t> readCapacities;
    std::size_t openAt = 0, peekAt = 0, readAt = 0, tickAt = 0;
    bool advanceTickAfterSleep = false;
    unsigned long lastError = 0;
    bool waitOk = false, modeOk = true, writeOk = true;
    std::uint32_t writeBytes = UINT32_MAX;
    int modeCalls = 0, writeCalls = 0, closeCalls = 0;
};
static PipeClientScript g_pipe;

static std::uintptr_t PipeOpen(const wchar_t*, unsigned long* error) {
    if (g_pipe.openAt >= g_pipe.opens.size()) return 0;
    const auto step = g_pipe.opens[g_pipe.openAt++];
    g_pipe.lastError = step.error;
    *error = step.error;
    return step.handle;
}
static bool PipeWait(const wchar_t*, std::uint32_t timeout, unsigned long*) {
    g_pipe.waits.push_back(timeout);
    return g_pipe.waitOk;
}
static void PipeSleep(std::uint32_t ms) { g_pipe.sleeps.push_back(ms); }
static bool PipeSetMode(std::uintptr_t, unsigned long*) {
    ++g_pipe.modeCalls; return g_pipe.modeOk;
}
static bool PipeWrite(std::uintptr_t, const void*, std::uint32_t size,
                      std::uint32_t* written, unsigned long*) {
    ++g_pipe.writeCalls;
    *written = g_pipe.writeBytes == UINT32_MAX
        ? size : g_pipe.writeBytes;
    return g_pipe.writeOk;
}
static std::uint64_t PipeTick() {
    if (g_pipe.advanceTickAfterSleep) return g_pipe.sleeps.empty() ? 100 : 1600;
    if (g_pipe.ticks.empty()) return 0;
    if (g_pipe.tickAt >= g_pipe.ticks.size()) return g_pipe.ticks.back();
    return g_pipe.ticks[g_pipe.tickAt++];
}
static bool PipePeek(std::uintptr_t, std::uint32_t* available, unsigned long*) {
    if (g_pipe.peekAt >= g_pipe.peeks.size()) { *available = 0; return true; }
    const auto step = g_pipe.peeks[g_pipe.peekAt++];
    *available = step.available;
    return step.ok;
}
static bool PipeRead(std::uintptr_t, void* buffer, std::uint32_t capacity,
                     std::uint32_t* bytesRead, unsigned long*) {
    g_pipe.readCapacities.push_back(capacity);
    if (g_pipe.readAt >= g_pipe.reads.size()) { *bytesRead = 0; return false; }
    const auto& step = g_pipe.reads[g_pipe.readAt++];
    const auto copied = (std::min)(capacity, static_cast<std::uint32_t>(step.data.size()));
    std::memcpy(buffer, step.data.data(), copied);
    *bytesRead = step.reported == UINT32_MAX ? copied : step.reported;
    return step.ok;
}
static bool PipeClose(std::uintptr_t, unsigned long*) { ++g_pipe.closeCalls; return true; }

static const Platform::NamedPipe::Testing::ClientBackendOps kPipeOps{
    PipeOpen, PipeWait, PipeSleep, PipeTick, PipeSetMode,
    PipeWrite, PipePeek, PipeRead, PipeClose};

static void ResetPipeScript() {
    g_pipe = {};
    g_pipe.opens = {{42, 0}};
    g_pipe.peeks = {{true, 1}};
    g_pipe.reads = {{true,
        "OK|HELLO|session=fake|snapshotToken=snap|projectName=Fake|selectedTrack=FX|"
        "cursorLabel=Bar 1|projectState=ready|trackState=selected-ready|"
        "supportsSelectedTrack=1|supportsCursorTrack=1|supportsAppendToTrack=1|"
        "supportsSpreadAcrossSelection=1\r\n", UINT32_MAX}};
    g_pipe.ticks = {100, 100};
    g_pipe.modeOk = g_pipe.writeOk = true;
    g_pipe.writeBytes = UINT32_MAX;
    Platform::NamedPipe::Testing::SetClientBackendOps(&kPipeOps);
}

static HarkenSearch::CubaseAdapter::BridgeProbeResult RunPipeClient() {
    HarkenSearch::CubaseAdapter::BridgeEndpoint endpoint;
    endpoint.endpointName = R"(\\.\pipe\auro_nuendo_direct_insert_v1)";
    return HarkenSearch::CubaseAdapter::ProbeBridgeEndpoint(endpoint);
}

static void testCubasePipeClientTransportFaultMatrix() {
    ResetPipeScript();
    g_pipe.opens.assign(5, {0, ERROR_FILE_NOT_FOUND});
    auto result = RunPipeClient();
    check(!result.available && g_pipe.openAt == 5 &&
              g_pipe.sleeps == std::vector<std::uint32_t>(5, 25) &&
              result.detail == "ERR|Steinberg bridge pipe is not available" && g_pipe.closeCalls == 0,
          "Cubase pipe client retries missing endpoint five times with 25ms delays");

    ResetPipeScript();
    g_pipe.opens = {{0, ERROR_PIPE_BUSY}, {0, ERROR_FILE_NOT_FOUND}, {42, 0}};
    result = RunPipeClient();
    check(result.available && g_pipe.waits == std::vector<std::uint32_t>{250} &&
              g_pipe.sleeps == std::vector<std::uint32_t>{25} &&
              result.remoteSessionId == "fake" &&
              result.supportsSpreadAcrossSelection && g_pipe.closeCalls == 1,
          "Cubase pipe client preserves busy/missing retry order and CRLF trim");

    ResetPipeScript();
    g_pipe.opens = {{0, ERROR_ACCESS_DENIED}};
    result = RunPipeClient();
    check(!result.available && g_pipe.openAt == 1 && g_pipe.sleeps.empty() &&
              result.detail == "ERR|Cannot connect to Steinberg bridge (error 5)",
          "Cubase pipe client returns other open errors immediately");

    ResetPipeScript();
    g_pipe.modeOk = false;
    result = RunPipeClient();
    check(result.available && g_pipe.modeCalls == 1 &&
              g_pipe.writeCalls == 1 && g_pipe.closeCalls == 1,
          "Cubase pipe client ignores message-mode configuration failure");

    for (const bool shortWrite : {false, true}) {
        ResetPipeScript();
        g_pipe.writeOk = shortWrite;
        g_pipe.writeBytes = shortWrite ? 1u : 0u;
        result = RunPipeClient();
        check(!result.available && g_pipe.closeCalls == 1 &&
                  result.detail == "ERR|Failed to write to Steinberg bridge pipe",
              shortWrite ? "Cubase pipe client rejects a short write and closes once"
                         : "Cubase pipe client rejects a failed write and closes once");
    }

    for (const bool failRead : {false, true}) {
        ResetPipeScript();
        g_pipe.ticks = {100, 100, 1600};
        if (failRead) g_pipe.reads = {{false, "", 0}};
        else g_pipe.peeks = {{false, 0}};
        result = RunPipeClient();
        check(!result.available && g_pipe.sleeps == std::vector<std::uint32_t>{10} &&
                  g_pipe.closeCalls == 1 &&
                  result.detail == "ERR|Steinberg bridge did not respond",
              failRead ? "Cubase pipe client treats read failure as timeout and closes once"
                       : "Cubase pipe client treats peek failure as timeout and closes once");
    }

    ResetPipeScript();
    g_pipe.ticks = {100, 100, 1600};
    g_pipe.peeks = {{true, 0}};
    result = RunPipeClient();
    check(!result.available && g_pipe.sleeps == std::vector<std::uint32_t>{10} &&
              g_pipe.closeCalls == 1 && result.detail == "ERR|Steinberg bridge did not respond",
          "Cubase pipe client polls with 10ms sleeps until timeout and closes once");

    ResetPipeScript();
    g_pipe.advanceTickAfterSleep = true;
    g_pipe.reads = {{true, "", 0}};
    result = RunPipeClient();
    check(!result.available && g_pipe.sleeps == std::vector<std::uint32_t>{10} &&
              g_pipe.closeCalls == 1 && result.detail == "ERR|Steinberg bridge did not respond",
          "Cubase pipe client treats successful zero-byte read as timeout and closes once");

    ResetPipeScript();
    std::string boundaryResponse(2047, 'x');
    boundaryResponse[0] = 'O';
    boundaryResponse[1] = 'K';
    g_pipe.reads = {{true, boundaryResponse, UINT32_MAX}};
    result = RunPipeClient();
    check(result.available &&
              g_pipe.readCapacities == std::vector<std::uint32_t>{2047} && g_pipe.closeCalls == 1,
          "Cubase pipe client reads exactly 2047 response bytes and closes once");

    ResetPipeScript();
    g_pipe.ticks = {100, 100, 1600};
    g_pipe.peeks = {{true, 2048}};
    g_pipe.reads = {{false, std::string(2047, 'y'), 2047}};
    result = RunPipeClient();
    check(!result.available &&
              g_pipe.readCapacities == std::vector<std::uint32_t>{2047} &&
              g_pipe.sleeps == std::vector<std::uint32_t>{10} && g_pipe.closeCalls == 1 &&
              result.detail == "ERR|Steinberg bridge did not respond",
          "Cubase pipe client ignores ERROR_MORE_DATA partial bytes and times out");

    Platform::NamedPipe::Testing::ResetClientBackendOps();
}

static void testCubasePreflightRequiresAdapter() {
    DawTarget target;
    target.name = "Cubase 14";
    target.processName = "Cubase14.exe";
    target.exePath = "C:/Program Files/Steinberg/Cubase 14/Cubase14.exe";
    target.mode = DawSendMode::LaunchExe;

    const auto preflight = HarkenSearch::CubaseAdapter::EvaluatePreflight(target);
    check(preflight.targetRecognized, "Cubase target is recognized by the adapter seam");
    check(preflight.readiness == HarkenSearch::CubaseAdapter::Readiness::AdapterMissing,
          "configured Cubase target is not advertised as direct-ready without adapter");
    check(preflight.capabilityTag == "Adapter required",
          "configured Cubase target exposes honest capability tag");
    check(preflight.failureReason.find("adapter is not installed yet") != std::string::npos,
          "configured Cubase target exposes explicit adapter-missing reason");
}

static void testNuendoPreflightFindsBundledAdapter() {
    DawTarget target;
    target.name = "Nuendo 14";
    target.processName = "Nuendo14.exe";
    target.exePath = "C:/Program Files/Steinberg/Nuendo 14/Nuendo14.exe";
    target.mode = DawSendMode::LaunchExe;

    ClearBridgeStubEnv();
    KillBridgeHelperProcesses();
    const auto helperPath = BuiltNuendoBridgeHelperPathUtf8();
    check(GetFileAttributesA(helperPath.c_str()) != INVALID_FILE_ATTRIBUTES,
          "Nuendo bundled preflight test can locate the built helper executable");

    const auto preflight = HarkenSearch::CubaseAdapter::EvaluatePreflight(target);
    check(preflight.targetRecognized, "Nuendo target is recognized by the shared Steinberg adapter seam");
    check(HarkenSearch::CubaseAdapter::IsNuendoLaunchTarget(target),
          "Nuendo target is recognized by the Nuendo classifier");
    check(HarkenSearch::CubaseAdapter::IsSteinbergDirectInsertTarget(target),
          "Nuendo target is recognized by the shared Steinberg direct-insert classifier");
    check(preflight.readiness == HarkenSearch::CubaseAdapter::Readiness::HostLaunchRequired,
          "configured Nuendo target finds the bundled adapter helper and asks for the host to start");
    check(preflight.preflightSummary.find("Start Nuendo") != std::string::npos,
          "configured Nuendo target exposes Nuendo-specific host-start copy after bundled helper detection");
    ClearBridgeStubEnv();
}

static void testCubasePreflightNeedsConfig() {
    DawTarget target;
    target.name = "Cubase";
    target.processName = "Cubase14.exe";
    target.mode = DawSendMode::LaunchExe;

    const auto preflight = HarkenSearch::CubaseAdapter::EvaluatePreflight(target);
    check(preflight.readiness == HarkenSearch::CubaseAdapter::Readiness::NeedsConfig,
          "unconfigured Cubase target still reports needs-config first");
    check(preflight.capabilityTag == "Not configured",
          "unconfigured Cubase target keeps configuration-first truth");
}

static void testCubasePreflightHostLaunchRequiredWhenAdapterInstalled() {
    DawTarget target;
    target.name = "Cubase";
    target.processName = "Cubase14.exe";
    target.exePath = "C:/Program Files/Steinberg/Cubase 14/Cubase14.exe";
    target.mode = DawSendMode::LaunchExe;
    target.isRunning = false;

    SetBridgeStubEnvResolved();
    const auto preflight = HarkenSearch::CubaseAdapter::EvaluatePreflight(target);
    check(preflight.readiness == HarkenSearch::CubaseAdapter::Readiness::HostLaunchRequired,
          "configured Cubase target asks to launch host when adapter is installed but host is offline");
    check(preflight.preflightSummary.find("Start Cubase") != std::string::npos,
          "configured Cubase target explains that the host must start before bridge attach");
    ClearBridgeStubEnv();
}

static void testCubasePreflightBridgeOnlineButHostNotReady() {
    DawTarget target;
    target.name = "Cubase";
    target.processName = "Cubase14.exe";
    target.exePath = "C:/Program Files/Steinberg/Cubase 14/Cubase14.exe";
    target.mode = DawSendMode::LaunchExe;
    target.isRunning = true;

    KillBridgeHelperProcesses();
    HANDLE thread = StartMockBridgeServer(MockBridgeMode::HostNotReadyHello);
    SetBridgeStubEnvResolved();
    const auto preflight = HarkenSearch::CubaseAdapter::EvaluatePreflight(target);
    check(preflight.readiness == HarkenSearch::CubaseAdapter::Readiness::BridgeNotReady,
          "running Cubase target reports host-not-ready when bridge is online but host state is not insert-ready");
    check(preflight.capabilityTag == "Host not ready",
          "running Cubase target exposes host-not-ready capability tag");
    check(preflight.preflightSummary.find("project=MockProjectA") != std::string::npos &&
              preflight.preflightSummary.find("selected=Missing Track") != std::string::npos,
          "running Cubase target surfaces host snapshot identity in host-not-ready preflight");
    check(preflight.failureReason.find("project session is not ready") != std::string::npos ||
              preflight.failureReason.find("selected track") != std::string::npos,
          "running Cubase target exposes explicit host-state-not-ready reason");
    ClearBridgeStubEnv();
    StopMockBridgeServer(thread);
}

static void testCubaseAttachReadyButHostNotReady() {
    DawTarget target;
    target.name = "Cubase";
    target.processName = "Cubase14.exe";
    target.exePath = CurrentProcessPathUtf8();
    target.mode = DawSendMode::LaunchExe;
    target.isRunning = true;

    KillBridgeHelperProcesses();
    HANDLE thread = StartMockBridgeServer(MockBridgeMode::HostNotReadyHello);
    SetBridgeStubEnvResolved();
    SetBridgeLaunchStub("success");
    const auto request = HarkenSearch::CubaseAdapter::BuildInsertRequest(
        target,
        std::vector<std::string>{"C:/audio/impact.wav"});
    const auto attach = HarkenSearch::CubaseAdapter::PrepareBridgeAttach(request);
    check(attach.ready,
          "Cubase attach preparation can attach to an online bridge even when host snapshot is not ready");
    check(!attach.hostReady,
          "Cubase attach preparation marks host-not-ready separately from endpoint-ready");
    check(attach.projectName == "MockProjectA",
          "Cubase attach preparation preserves host project name from HELLO");
    check(attach.selectedTrackName == "Missing Track",
          "Cubase attach preparation preserves selected-track identity even in host-not-ready state");
    check(attach.detail.find("not insert-ready") != std::string::npos,
          "Cubase attach preparation reports host snapshot not ready");
    const auto result = HarkenSearch::CubaseAdapter::ExecuteInsertRequest(request);
    check(result.code == HarkenSearch::CubaseAdapter::ResultCode::BridgeNotReady,
          "Cubase execute path blocks insert when attach succeeded but host snapshot is not ready");
    check(result.status.find("host snapshot is not ready") != std::string::npos,
          "Cubase execute path reports host-snapshot-not-ready context");
    ClearBridgeStubEnv();
    StopMockBridgeServer(thread);
}

static void testCubaseDispatchSeamFailsHonestly() {
    DawTarget target;
    target.name = "Cubase 14";
    target.processName = "Cubase14.exe";
    target.exePath = "C:/Program Files/Steinberg/Cubase 14/Cubase14.exe";
    target.mode = DawSendMode::LaunchExe;

    std::string status;
    const bool ok = HarkenSearch::CubaseAdapter::DispatchToCubaseAdapter(
        target,
        std::vector<std::string>{"C:/audio/impact.wav"},
        status);
    check(!ok, "Cubase dispatch seam does not fake success");
    check(status.find("adapter is not installed yet") != std::string::npos,
          "Cubase dispatch seam returns adapter-missing reason");
}

static void testCubaseInsertRequestContract() {
    DawTarget target;
    target.name = "Cubase 14";
    target.processName = "Cubase14.exe";
    target.exePath = "C:/Program Files/Steinberg/Cubase 14/Cubase14.exe";
    target.mode = DawSendMode::LaunchExe;

    const auto request = HarkenSearch::CubaseAdapter::BuildInsertRequest(
        target,
        std::vector<std::string>{"C:/audio/impact.wav", "C:/audio/riser.wav"});
    check(request.protocolVersion == "cubase-direct-insert-v1",
          "Cubase request carries an explicit protocol version");
    check(request.files.size() == 2,
          "Cubase request preserves selected file count");
    check(request.fallback == HarkenSearch::CubaseAdapter::FallbackKind::ExportFallback,
          "Cubase request declares export fallback as the current safe recovery path");
    check(request.correlationId.find("Cubase 14") != std::string::npos,
          "Cubase request carries a correlation id for future bridge diagnostics");
    check(request.transferAction == HarkenSearch::CubaseAdapter::TransferActionKind::SpotToHost,
          "Cubase request carries an explicit transfer action instead of a hidden launch-only assumption");
    check(request.sourceSelection == HarkenSearch::CubaseAdapter::SourceSelectionKind::WholeFiles,
          "Cubase request carries an explicit source-selection kind");
    check(request.preferredTrackName == "Selected Track",
          "Cubase request carries a preferred track hint");
    check(request.requestedSessionId == "current-session",
          "Cubase request carries a session affinity hint");

    const auto bringRequest = HarkenSearch::CubaseAdapter::BuildInsertRequest(
        target,
        std::vector<std::string>{"C:/audio/impact.wav"},
        HarkenSearch::CubaseAdapter::TransferActionKind::BringIntoHost,
        HarkenSearch::CubaseAdapter::SourceSelectionKind::WholeFiles);
    check(bringRequest.transferAction == HarkenSearch::CubaseAdapter::TransferActionKind::BringIntoHost,
          "Cubase request can carry explicit bring-into-host semantics");
    check(HarkenSearch::CubaseAdapter::BuildBridgeInsertMessage(bringRequest).find("|action=bring-into-host") != std::string::npos,
          "Cubase bridge insert message preserves bring-into-host semantics");
}

static void testNuendoInsertRequestCanBeBuiltFromSharedHostSendJob() {
    DawTarget target;
    target.name = "Nuendo 14";
    target.processName = "Nuendo14.exe";
    target.exePath = "C:/Program Files/Steinberg/Nuendo 14/Nuendo14.exe";
    target.mode = DawSendMode::LaunchExe;
    target.isRunning = true;

    HarkenSearch::HostSend::Request hostSendRequest;
    hostSendRequest.host = HarkenSearch::HostSend::HostId::Nuendo;
    hostSendRequest.mode = HarkenSearch::HostSend::DeliveryMode::BringIntoHost;
    hostSendRequest.items.push_back({
        "D:/Library/Impacts/impact.wav",
        "impact",
        HarkenSearch::HostSend::SourceSelectionKind::RegionRender
    });
    const auto job = HarkenSearch::HostSend::BuildJob(
        hostSendRequest,
        "C:/Temp/HarkenSearchHostSend");

    const auto request = HarkenSearch::CubaseAdapter::BuildInsertRequestFromHostSendJob(target, job);
    check(request.protocolVersion == "nuendo-direct-insert-v1",
          "Nuendo host-send job maps into Nuendo-specific protocol version");
    check(request.transferAction == HarkenSearch::CubaseAdapter::TransferActionKind::BringIntoHost,
          "Nuendo host-send job preserves bring-into-host transfer action");
    check(request.sourceSelection == HarkenSearch::CubaseAdapter::SourceSelectionKind::RegionRender,
          "Nuendo host-send job preserves region-render source selection");
    check(request.files.size() == 1,
          "Nuendo host-send job maps one staged file into one insert request file");
    check(request.files[0].filePath.find("HarkenSearchHostSend") != std::string::npos,
          "Nuendo host-send job forwards staged path into insert request");
}

static std::string ExpectedHostSendRoot(const std::string& base) {
    return (std::filesystem::u8path(base) / "Auro" / "HostSend").u8string();
}

static void testHostSendDefaultStagingRootPrefersLocalAppData() {
    HostSendPlatformStub::Reset();
    Platform::EnvironmentStringResult environment;
    environment.status = Platform::EnvironmentStringStatus::Success;
    environment.valueUtf8 = "X:/FixtureProfile/AppData/Local";
    HostSendPlatformStub::SetEnvironmentResult(environment);

    check(HarkenSearch::HostSend::DefaultStagingRootUtf8() ==
              ExpectedHostSendRoot(environment.valueUtf8),
          "HostSend staging prefers LOCALAPPDATA and keeps the Auro/HostSend suffix");
}

static void testHostSendDefaultStagingRootFallsBackToTemporaryDirectory() {
    HostSendPlatformStub::Reset();
    const std::string temporary = Platform::FileSystem::TemporaryDirectory();
    check(!temporary.empty(), "HostSend fallback fixture resolves a temporary directory");
    check(HarkenSearch::HostSend::DefaultStagingRootUtf8() ==
              ExpectedHostSendRoot(temporary),
          "HostSend staging falls back to the PAL temporary directory");
}

static void testHostSendExplicitStagingRootSkipsDefaults() {
    HostSendPlatformStub::Reset();
    Platform::EnvironmentStringResult environment;
    environment.status = Platform::EnvironmentStringStatus::Success;
    environment.valueUtf8 = "C:/ShouldNotBeUsed";
    HostSendPlatformStub::SetEnvironmentResult(environment);
    HarkenSearch::HostSend::Request request;
    const auto plan = HarkenSearch::HostSend::BuildStagingPlan(request, "E:/ExplicitRoot");
    check(plan.rootDirectoryUtf8 == "E:/ExplicitRoot",
          "HostSend explicit staging root bypasses environment and temporary defaults");
}

static void testCubaseExecuteInsertRequestModelsFallback() {
    DawTarget target;
    target.name = "Cubase 14";
    target.processName = "Cubase14.exe";
    target.exePath = "C:/Program Files/Steinberg/Cubase 14/Cubase14.exe";
    target.mode = DawSendMode::LaunchExe;

    const auto request = HarkenSearch::CubaseAdapter::BuildInsertRequest(
        target,
        std::vector<std::string>{"C:/audio/impact.wav"});
    const auto result = HarkenSearch::CubaseAdapter::ExecuteInsertRequest(request);
    check(result.code == HarkenSearch::CubaseAdapter::ResultCode::AdapterMissing,
          "Cubase execute path reports adapter-missing before bridge implementation exists");
    check(result.fallback == HarkenSearch::CubaseAdapter::FallbackKind::ExportFallback,
          "Cubase execute path preserves fallback ownership");
    check(result.status.find("Export fallback") != std::string::npos,
          "Cubase execute path names the current fallback in its status");
}

static void testCubaseBridgeEndpointContract() {
    DawTarget target;
    target.name = "Cubase 14";
    target.processName = "Cubase14.exe";
    target.exePath = "C:/Program Files/Steinberg/Cubase 14/Cubase14.exe";
    target.mode = DawSendMode::LaunchExe;
    target.isRunning = true;

    const auto request = HarkenSearch::CubaseAdapter::BuildInsertRequest(
        target,
        std::vector<std::string>{"C:/audio/impact.wav"});
    const auto endpoint = HarkenSearch::CubaseAdapter::BuildBridgeEndpoint(request);
    check(endpoint.transport == HarkenSearch::CubaseAdapter::TransportKind::NamedPipe,
          "Cubase bridge transport starts with a named-pipe contract");
    check(endpoint.endpointName == R"(\\.\pipe\auro_cubase_direct_insert_v1)",
          "Cubase bridge endpoint name is stable and explicit");
    check(endpoint.handshakeToken.find("cubase-direct-insert-v1") != std::string::npos,
          "Cubase bridge handshake token includes protocol version");
    const auto hello = HarkenSearch::CubaseAdapter::BuildBridgeHelloMessage(endpoint);
    check(hello.rfind("HELLO|", 0) == 0,
          "Cubase bridge hello message has an explicit HELLO verb");
    const auto insertMessage = HarkenSearch::CubaseAdapter::BuildBridgeInsertMessage(request);
    check(insertMessage.find("|action=spot-to-host") != std::string::npos,
          "Cubase bridge insert message preserves transfer-action semantics");
    check(insertMessage.find("|selection=whole-files") != std::string::npos,
          "Cubase bridge insert message preserves source-selection semantics");
    const auto processContract = HarkenSearch::CubaseAdapter::BuildBridgeProcessContract(request);
    const auto hostContract = HarkenSearch::CubaseAdapter::BuildHostBridgeV0Contract(request);
    check(processContract.attachMode == HarkenSearch::CubaseAdapter::AttachMode::PipeHelloHandshake,
          "Cubase bridge process contract prefers hello-handshake attach");
    check(processContract.bridgeExecutableKind == HarkenSearch::CubaseAdapter::BridgeExecutableKind::ExternalHelperProcess,
          "Cubase bridge process contract currently models an external helper process");
    check(!processContract.phases.empty() &&
              processContract.phases.front() == HarkenSearch::CubaseAdapter::BridgeLifecyclePhase::DetectHost,
          "Cubase bridge process contract starts with host detection");
    check(!processContract.failureMatrix.empty() &&
              processContract.failureMatrix.front() == HarkenSearch::CubaseAdapter::LaunchFailureKind::HostExecutableMissing,
          "Cubase bridge process contract exposes a launch failure matrix");
    check(hostContract.protocolVersion == "cubase-direct-insert-v1",
          "Cubase host bridge v0 contract preserves protocol version");
    check(hostContract.requiresProjectSession && hostContract.requiresTrackTarget,
          "Cubase host bridge v0 contract requires session and track resolution before insert");
    check(hostContract.requiredInsertFields.size() >= 6,
          "Cubase host bridge v0 contract exposes required insert fields");
    check(std::find(hostContract.requiredInsertFields.begin(), hostContract.requiredInsertFields.end(), "snapshot") != hostContract.requiredInsertFields.end(),
          "Cubase host bridge v0 contract requires snapshot token on INSERT");
    check(std::find(hostContract.requiredInsertFields.begin(), hostContract.requiredInsertFields.end(), "projectName") != hostContract.requiredInsertFields.end(),
          "Cubase host bridge v0 contract requires projectName on INSERT");
    check(std::find(hostContract.requiredInsertFields.begin(), hostContract.requiredInsertFields.end(), "selectedTrack") != hostContract.requiredInsertFields.end(),
          "Cubase host bridge v0 contract requires selectedTrack on INSERT");
    check(std::find(hostContract.requiredInsertFields.begin(), hostContract.requiredInsertFields.end(), "cursorLabel") != hostContract.requiredInsertFields.end(),
          "Cubase host bridge v0 contract requires cursorLabel on INSERT");
    check(std::find(hostContract.requiredHelloFields.begin(), hostContract.requiredHelloFields.end(), "projectName") != hostContract.requiredHelloFields.end(),
          "Cubase host bridge v0 contract requires projectName in HELLO snapshot");
    check(std::find(hostContract.requiredHelloFields.begin(), hostContract.requiredHelloFields.end(), "snapshotToken") != hostContract.requiredHelloFields.end(),
          "Cubase host bridge v0 contract requires snapshotToken in HELLO snapshot");
    check(std::find(hostContract.requiredAckFields.begin(), hostContract.requiredAckFields.end(), "selectedTrack") != hostContract.requiredAckFields.end(),
          "Cubase host bridge v0 contract requires selectedTrack in INSERT ack");
}

static void testNuendoBridgeEndpointAndBinaryContract() {
    DawTarget target;
    target.name = "Nuendo 14";
    target.processName = "Nuendo14.exe";
    target.exePath = "C:/Program Files/Steinberg/Nuendo 14/Nuendo14.exe";
    target.mode = DawSendMode::LaunchExe;
    target.isRunning = true;

    const auto request = HarkenSearch::CubaseAdapter::BuildInsertRequest(
        target,
        std::vector<std::string>{"C:/audio/impact.wav"});
    const auto endpoint = HarkenSearch::CubaseAdapter::BuildBridgeEndpoint(request);
    const auto binaryContract = HarkenSearch::CubaseAdapter::BuildBridgeBinaryContract(request);
    check(request.protocolVersion == "nuendo-direct-insert-v1",
          "Nuendo request carries a Nuendo-specific protocol version");
    check(endpoint.endpointName == R"(\\.\pipe\auro_nuendo_direct_insert_v1)",
          "Nuendo bridge endpoint name is stable and explicit");
    check(binaryContract.binaryName == "HarkenSearchNuendoBridgeStub.exe",
          "Nuendo bridge binary contract preserves the Nuendo helper binary name");
    check(binaryContract.hostFamily == "Nuendo",
          "Nuendo bridge binary contract preserves the Nuendo host family");
}

static void testCubaseBridgeExecutableResolutionAndLaunchArguments() {
    DawTarget target;
    target.name = "Cubase 14";
    target.processName = "Cubase14.exe";
    target.exePath = "C:/Program Files/Steinberg/Cubase 14/Cubase14.exe";
    target.mode = DawSendMode::LaunchExe;
    target.isRunning = true;

    SetBridgeStubEnvResolved();
    SetBridgeLaunchStub("success");
    const auto request = HarkenSearch::CubaseAdapter::BuildInsertRequest(
        target,
        std::vector<std::string>{"C:/audio/impact.wav"});
    const auto endpoint = HarkenSearch::CubaseAdapter::BuildBridgeEndpoint(request);
    const auto binaryContract = HarkenSearch::CubaseAdapter::BuildBridgeBinaryContract(request);
    const auto resolution = HarkenSearch::CubaseAdapter::ResolveBridgeExecutable(request);
    check(binaryContract.binaryName == "HarkenSearchCubaseBridgeStub.exe",
          "Cubase bridge binary contract preserves the helper binary name");
    check(binaryContract.protocolVersion == "cubase-direct-insert-v1",
          "Cubase bridge binary contract preserves protocol version");
    check(binaryContract.endpointName == endpoint.endpointName,
          "Cubase bridge binary contract preserves endpoint name");
    check(binaryContract.requiredArguments.size() >= 6,
          "Cubase bridge binary contract lists required helper arguments");
    check(resolution.available,
          "Cubase bridge executable resolution accepts an explicit environment override");
    check(resolution.environmentOverride,
          "Cubase bridge executable resolution marks environment overrides");
    check(resolution.executablePath == CurrentProcessPathUtf8(),
          "Cubase bridge executable resolution preserves the override path");
    const auto launchArgs = HarkenSearch::CubaseAdapter::BuildBridgeLaunchArguments(request, endpoint);
    check(launchArgs.find("--pipe") != std::string::npos &&
              launchArgs.find("--token") != std::string::npos,
          "Cubase bridge launch arguments carry endpoint and token");
    check(launchArgs.find("--hostProcess") != std::string::npos,
          "Cubase bridge launch arguments carry host process identity");
    check(launchArgs.find("--stay-alive") != std::string::npos,
          "Cubase bridge launch arguments request a persistent helper session");
    const auto processContract = HarkenSearch::CubaseAdapter::BuildBridgeProcessContract(request);
    check(processContract.bridgeExecutableHint == CurrentProcessPathUtf8(),
          "Cubase bridge process contract preserves the resolved helper path");
    check(processContract.bridgeLaunchArgumentsHint.find("--target") != std::string::npos,
          "Cubase bridge process contract carries helper launch arguments");
    const auto launchPlan = HarkenSearch::CubaseAdapter::BuildBridgeLaunchPlan(request);
    check(launchPlan.shouldLaunchBridgeHelper,
          "Cubase bridge launch plan launches helper when host is already running");
    check(launchPlan.endpointWaitTimeoutMs == 1500,
          "Cubase bridge launch plan uses the running-host wait timeout");
    check(launchPlan.helperExecutablePath == CurrentProcessPathUtf8(),
          "Cubase bridge launch plan preserves resolved helper path");
    ClearBridgeStubEnv();
}

static void testCubaseBridgeLaunchPlanLaunchThenAttach() {
    DawTarget target;
    target.name = "Cubase 14";
    target.processName = "Cubase14.exe";
    target.exePath = "C:/Program Files/Steinberg/Cubase 14/Cubase14.exe";
    target.mode = DawSendMode::LaunchExe;
    target.isRunning = false;

    SetBridgeStubEnvResolved();
    SetBridgeLaunchStub("success");
    const auto request = HarkenSearch::CubaseAdapter::BuildInsertRequest(
        target,
        std::vector<std::string>{"C:/audio/impact.wav"});
    const auto launchPlan = HarkenSearch::CubaseAdapter::BuildBridgeLaunchPlan(request);
    check(launchPlan.shouldLaunchHost,
          "Cubase bridge launch plan requests host launch when Cubase is offline");
    check(!launchPlan.shouldLaunchBridgeHelper,
          "Cubase bridge launch plan does not claim helper launch before host attach");
    check(launchPlan.endpointWaitTimeoutMs == 8000,
          "Cubase bridge launch plan uses the longer launch-then-attach timeout");
    check(launchPlan.summary.find("Launch Cubase host") != std::string::npos,
          "Cubase bridge launch plan summary names the launch-then-attach flow");
    ClearBridgeStubEnv();
}

static void testCubaseEndpointWaitAndAttachPreparation() {
    DawTarget target;
    target.name = "Cubase 14";
    target.processName = "Cubase14.exe";
    target.exePath = CurrentProcessPathUtf8();
    target.mode = DawSendMode::LaunchExe;
    target.isRunning = true;

    SetBridgeStubEnvResolved();
    SetBridgeLaunchStub("success");
    KillBridgeHelperProcesses();
    const auto request = HarkenSearch::CubaseAdapter::BuildInsertRequest(
        target,
        std::vector<std::string>{"C:/audio/impact.wav"});
    const auto endpoint = HarkenSearch::CubaseAdapter::BuildBridgeEndpoint(request);
    const auto plan = HarkenSearch::CubaseAdapter::BuildBridgeLaunchPlan(request);
    auto disabledPlan = plan;
    disabledPlan.endpointWaitTimeoutMs = 0;
    HostSendPlatformStub::ResetSleepCalls();
    const auto disabledWait =
        HarkenSearch::CubaseAdapter::WaitForBridgeEndpoint(disabledPlan, endpoint);
    check(disabledWait.pollAttempts == 0 &&
              HostSendPlatformStub::SleepCalls().empty(),
          "Cubase endpoint wait does not probe or sleep for nonpositive timeout");

    auto timeoutPlan = plan;
    timeoutPlan.endpointWaitTimeoutMs = 1500;
    timeoutPlan.endpointPollIntervalMs = 250;
    HostSendPlatformStub::ResetSleepCalls();
    const auto timeoutWait =
        HarkenSearch::CubaseAdapter::WaitForBridgeEndpoint(timeoutPlan, endpoint);
    check(!timeoutWait.available,
          "Cubase endpoint wait times out honestly when no endpoint appears");
    check(timeoutWait.waitedMs == timeoutPlan.endpointWaitTimeoutMs,
          "Cubase endpoint wait reports the configured timeout budget");
    check(timeoutWait.pollAttempts == 7,
          "Cubase endpoint wait probes once per interval including the final iteration");
    check(HostSendPlatformStub::SleepCalls() ==
              std::vector<uint32_t>(6, 250),
          "Cubase endpoint wait sleeps six times at 250ms and skips final sleep");
    const auto timeoutAttach = HarkenSearch::CubaseAdapter::PrepareBridgeAttach(request);
    check(!timeoutAttach.ready,
          "Cubase attach preparation stays not-ready when endpoint wait times out");
    check(timeoutAttach.detail.find("Bridge attach not ready") != std::string::npos ||
              timeoutAttach.detail.find("wait budget") != std::string::npos,
          "Cubase attach preparation reports attach context on timeout");

    HANDLE thread = StartMockBridgeServer(MockBridgeMode::HappyPath);
    HostSendPlatformStub::ResetSleepCalls();
    const auto readyWait = HarkenSearch::CubaseAdapter::WaitForBridgeEndpoint(plan, endpoint);
    check(readyWait.available,
          "Cubase endpoint wait succeeds once the endpoint is live");
    check(readyWait.remoteSessionId == "auro-test-session",
          "Cubase endpoint wait preserves remote session id from HELLO");
    check(HostSendPlatformStub::SleepCalls().empty(),
          "Cubase endpoint wait does not sleep when the first probe is available");
    const auto readyAttach = HarkenSearch::CubaseAdapter::PrepareBridgeAttach(request);
    check(readyAttach.ready,
          "Cubase attach preparation becomes ready once the endpoint responds");
    check(readyAttach.hostReady,
          "Cubase attach preparation distinguishes host-ready from mere endpoint-ready");
    check(readyAttach.helperLaunchPending,
          "Cubase attach preparation preserves helper-launch intent for running host attach");
    check(readyAttach.remoteSessionId == "auro-test-session",
          "Cubase attach preparation preserves remote session id from the ready endpoint");
    check(readyAttach.snapshotToken == "snap-mock-001",
          "Cubase attach preparation preserves snapshot token from the ready endpoint");
    check(readyAttach.projectName == "MockProjectA",
          "Cubase attach preparation preserves project name from the ready endpoint");
    check(readyAttach.selectedTrackName == "FX 01",
          "Cubase attach preparation preserves selected-track name from the ready endpoint");
    check(readyAttach.cursorLabel == "Bar 9 Beat 3",
          "Cubase attach preparation preserves cursor label from the ready endpoint");

    StopMockBridgeServer(thread);
    ClearBridgeStubEnv();
}

static void testCubaseBridgeLaunchAttemptExecutor() {
    DawTarget target;
    target.name = "Cubase 14";
    target.processName = "Cubase14.exe";
    target.exePath = CurrentProcessPathUtf8();
    target.mode = DawSendMode::LaunchExe;
    target.isRunning = true;

    StopMockBridgeServer(NULL);
    KillBridgeHelperProcesses();
    SetBridgeStubEnvResolved();
    const auto request = HarkenSearch::CubaseAdapter::BuildInsertRequest(
        target,
        std::vector<std::string>{"C:/audio/impact.wav"});
    const auto plan = HarkenSearch::CubaseAdapter::BuildBridgeLaunchPlan(request);

    SetBridgeLaunchStub("fail");
    const auto failedAttempt = HarkenSearch::CubaseAdapter::ExecuteBridgeLaunchAttempt(plan);
    check(failedAttempt.attempted,
          "Cubase bridge launch attempt marks when helper launch was attempted");
    check(!failedAttempt.success,
          "Cubase bridge launch attempt reports stubbed failure");
    check(failedAttempt.usedStub,
          "Cubase bridge launch attempt reports when a stub path was used");
    check(failedAttempt.failureKind == HarkenSearch::CubaseAdapter::LaunchFailureKind::BridgeLaunchFailed,
          "Cubase bridge launch attempt preserves BridgeLaunchFailed semantics");

    SetBridgeLaunchStub("success");
    const auto okAttempt = HarkenSearch::CubaseAdapter::ExecuteBridgeLaunchAttempt(plan);
    check(okAttempt.success,
          "Cubase bridge launch attempt reports stubbed success");
    check(okAttempt.processId == 4242,
          "Cubase bridge launch attempt preserves stubbed process id");

    SetBridgeLaunchStub("fail");
    const auto failedAttach = HarkenSearch::CubaseAdapter::PrepareBridgeAttach(request);
    check(failedAttach.launchAttempt.attempted && !failedAttach.launchAttempt.success,
          "Cubase attach preparation preserves helper launch failure detail");
    check(!failedAttach.ready,
          "Cubase attach preparation blocks when helper launch attempt fails");

    ClearBridgeStubEnv();
}

static void testCubaseBridgeLaunchAttemptRealSpawnFailureSurface() {
    HarkenSearch::CubaseAdapter::BridgeLaunchPlan plan;
    plan.shouldLaunchBridgeHelper = true;
    plan.helperExecutablePath = "Z:/HarkenSearchMissing/HarkenSearchCubaseBridgeStub.exe";
    plan.helperLaunchArguments = "--pipe \\\\.\\pipe\\auro_cubase_direct_insert_v1";

    const auto attempt = HarkenSearch::CubaseAdapter::ExecuteBridgeLaunchAttempt(plan);
    check(attempt.attempted,
          "Cubase real launch-attempt seam still marks attempted when spawn is tried");
    check(!attempt.success,
          "Cubase real launch-attempt seam reports failure when helper exe is missing");
    check(attempt.failureKind == HarkenSearch::CubaseAdapter::LaunchFailureKind::BridgeExecutableMissing ||
              attempt.failureKind == HarkenSearch::CubaseAdapter::LaunchFailureKind::BridgeLaunchFailed,
          "Cubase real launch-attempt seam preserves executable-missing/launch-failed semantics");
}

static void testCubaseBridgeResponseSchema() {
    const auto ok = HarkenSearch::CubaseAdapter::ParseBridgeResponse(
        "OK|INSERT|session=auro-session|snapshotToken=snap-response-01|projectName=ResponseProject|selectedTrack=FX 01|cursorLabel=Bar 5 Beat 2|projectState=ready|trackState=selected-ready|track=FX 01|position=edit-cursor|count=2|clips=clip-a,clip-b");
    check(ok.ok, "Cubase bridge response parser recognizes OK responses");
    check(ok.remoteSessionId == "auro-session",
          "Cubase bridge response parser extracts remote session id");
    check(ok.snapshotToken == "snap-response-01",
          "Cubase bridge response parser extracts snapshot token");
    check(ok.projectName == "ResponseProject",
          "Cubase bridge response parser extracts project name");
    check(ok.selectedTrackName == "FX 01",
          "Cubase bridge response parser extracts selected-track name");
    check(ok.cursorLabel == "Bar 5 Beat 2",
          "Cubase bridge response parser extracts cursor label");
    check(ok.acceptedTrack == "FX 01",
          "Cubase bridge response parser extracts accepted track");
    check(ok.trackResolution == HarkenSearch::CubaseAdapter::TrackResolutionKind::SelectedTrackAccepted,
          "Cubase bridge response parser recognizes selected-track acceptance");
    check(ok.projectState == HarkenSearch::CubaseAdapter::HostProjectState::ProjectReady,
          "Cubase bridge response parser extracts host project-ready state");
    check(ok.trackState == HarkenSearch::CubaseAdapter::HostTrackState::SelectedTrackReady,
          "Cubase bridge response parser extracts host selected-track state");
    check(ok.insertedFileCount == 2,
          "Cubase bridge response parser extracts inserted file count");
    check(ok.acceptedPositionMode == "edit-cursor",
          "Cubase bridge response parser extracts accepted position mode");
    check(ok.insertedClipIds.size() == 2 && ok.insertedClipIds[0] == "clip-a",
          "Cubase bridge response parser extracts inserted clip ids");

    const auto legacy = HarkenSearch::CubaseAdapter::ParseBridgeResponse("OK|INSERT|clips=clip-a");
    check(legacy.ok && !legacy.insertedFileCount && legacy.insertedClipIds.size() == 1,
          "Cubase leaves the reported count absent for legacy clip-ID receipts");
    for (const char* count : {"0", "", "1junk", "-1", "2147483648"}) {
        const auto unconfirmed = HarkenSearch::CubaseAdapter::ParseBridgeResponse(
            "OK|INSERT|count=" + std::string(count) + "|clips=clip-a");
        check(unconfirmed.ok && unconfirmed.insertedFileCount == 0 && unconfirmed.insertedClipIds.size() == 1,
              "Cubase preserves OK and clip IDs but does not confirm delivery from zero or an invalid count");
    }

    const auto partial = HarkenSearch::CubaseAdapter::ParseBridgeResponse(
        "OK|INSERT|session=auro-session|projectState=ready|trackState=remapped|track=FX 01|remappedTrack=FX ALT|position=next-grid|adjusted=1|count=1|clips=clip-a|failed=C:\\a.wav");
    check(partial.code == HarkenSearch::CubaseAdapter::ResultCode::PartialSuccess,
          "Cubase bridge response parser recognizes partial success");
    check(partial.remappedTrack == "FX ALT",
          "Cubase bridge response parser extracts remapped track");
    check(partial.trackResolution == HarkenSearch::CubaseAdapter::TrackResolutionKind::RemappedTrack,
          "Cubase bridge response parser recognizes remapped-track resolution");
    check(partial.trackState == HarkenSearch::CubaseAdapter::HostTrackState::TrackRemapped,
          "Cubase bridge response parser extracts remapped track-state");
    check(partial.positionAdjusted,
          "Cubase bridge response parser extracts adjusted position flag");
    check(partial.failedFilePaths.size() == 1 && partial.failedFilePaths[0] == "C:\\a.wav",
          "Cubase bridge response parser extracts failed file list");

    const auto sessionErr = HarkenSearch::CubaseAdapter::ParseBridgeResponse(
        "ERR|SESSION_MISMATCH|project-session-changed");
    check(sessionErr.code == HarkenSearch::CubaseAdapter::ResultCode::SessionMismatch,
          "Cubase bridge response parser recognizes session mismatch");

    const auto hostNotReadyErr = HarkenSearch::CubaseAdapter::ParseBridgeResponse(
        "ERR|BRIDGE_NOT_READY|project-or-track-not-ready");
    check(hostNotReadyErr.code == HarkenSearch::CubaseAdapter::ResultCode::BridgeNotReady,
          "Cubase bridge response parser recognizes bridge-not-ready");

    const auto snapshotErr = HarkenSearch::CubaseAdapter::ParseBridgeResponse(
        "ERR|BRIDGE_NOT_READY|snapshot-mismatch");
    check(snapshotErr.addressingDrift == HarkenSearch::CubaseAdapter::AddressingDriftKind::SnapshotMismatch,
          "Cubase bridge response parser recognizes snapshot drift");

    const auto projectErr = HarkenSearch::CubaseAdapter::ParseBridgeResponse(
        "ERR|BRIDGE_NOT_READY|project-mismatch");
    check(projectErr.addressingDrift == HarkenSearch::CubaseAdapter::AddressingDriftKind::ProjectMismatch,
          "Cubase bridge response parser recognizes project drift");

    const auto trackErr = HarkenSearch::CubaseAdapter::ParseBridgeResponse(
        "ERR|TRACK_UNAVAILABLE|selected-track-changed");
    check(trackErr.code == HarkenSearch::CubaseAdapter::ResultCode::TrackUnavailable,
          "Cubase bridge response parser recognizes track unavailable");
    check(trackErr.trackResolution == HarkenSearch::CubaseAdapter::TrackResolutionKind::SelectedTrackChanged,
          "Cubase bridge response parser recognizes selected-track-changed resolution");
}

static void testCubaseBridgeProbeAndExecuteReadyishPath() {
    DawTarget target;
    target.name = "Cubase 14";
    target.processName = "Cubase14.exe";
    target.exePath = "Z:/HarkenSearchMissing/Cubase14.exe";
    target.mode = DawSendMode::LaunchExe;
    target.isRunning = true;

    StopMockBridgeServer(NULL);
    KillBridgeHelperProcesses();
    SetBridgeStubFlagOnly();
    const auto preflight = HarkenSearch::CubaseAdapter::EvaluatePreflight(target);
    check(preflight.readiness == HarkenSearch::CubaseAdapter::Readiness::BridgeOffline,
          "running Cubase target reports bridge-offline when helper is unresolved");
    check(preflight.failureReason.find("Bridge helper") != std::string::npos,
          "running Cubase target exposes helper-resolution failure before probing a missing endpoint");
    SetBridgeStubEnvResolved();
    SetBridgeLaunchStub("success");
    const auto request = HarkenSearch::CubaseAdapter::BuildInsertRequest(
        target,
        std::vector<std::string>{"C:/audio/impact.wav"});
    const auto preflightWithHelper = HarkenSearch::CubaseAdapter::EvaluatePreflight(target);
    check(preflightWithHelper.readiness == HarkenSearch::CubaseAdapter::Readiness::BridgeOffline,
          "running Cubase target still reports bridge-offline when helper is resolved but no endpoint answers");
    check(preflightWithHelper.capabilityTag == "Bridge offline",
          "running Cubase target exposes bridge-offline capability tag");
    const auto endpoint = HarkenSearch::CubaseAdapter::BuildBridgeEndpoint(request);
    const auto probe = HarkenSearch::CubaseAdapter::ProbeBridgeEndpoint(endpoint);
    check(!probe.available,
          "Cubase bridge probe still fails honestly when no server endpoint is listening");
    const auto result = HarkenSearch::CubaseAdapter::ExecuteInsertRequest(request);
    check(result.code == HarkenSearch::CubaseAdapter::ResultCode::BridgeUnavailable,
          "Cubase execute path reports bridge-unavailable when transport stub is declared but no endpoint exists");
    check(result.status.find("wait budget") != std::string::npos ||
              result.status.find("endpoint is not available") != std::string::npos,
          "Cubase execute path reports endpoint wait context in bridge-unavailable failures");
    check(result.status.find("launch-plan:") != std::string::npos,
          "Cubase execute path reports launch-plan context in endpoint-unavailable failures");
    ClearBridgeStubEnv();
}

static void testCubaseMockBridgeServerRoundTrip() {
    DawTarget target;
    target.name = "Cubase 14";
    target.processName = "Cubase14.exe";
    target.exePath = "C:/Program Files/Steinberg/Cubase 14/Cubase14.exe";
    target.mode = DawSendMode::LaunchExe;
    target.isRunning = true;

    KillBridgeHelperProcesses();
    HANDLE thread = StartMockBridgeServer(MockBridgeMode::HappyPath);
    SetBridgeStubEnvResolved();
    SetBridgeLaunchStub("success");

    const auto request = HarkenSearch::CubaseAdapter::BuildInsertRequest(
        target,
        std::vector<std::string>{"C:/audio/impact.wav"});
    const auto preflight = HarkenSearch::CubaseAdapter::EvaluatePreflight(target);
    check(preflight.readiness == HarkenSearch::CubaseAdapter::Readiness::DirectReady,
          "running Cubase target becomes direct-ready when bridge snapshot is insert-ready");
    check(preflight.preflightSummary.find("project=MockProjectA") != std::string::npos &&
              preflight.preflightSummary.find("cursor=Bar 9 Beat 3") != std::string::npos,
          "direct-ready Cubase preflight surfaces host snapshot identity");
    const auto processContract = HarkenSearch::CubaseAdapter::BuildBridgeProcessContract(request);
    check(processContract.launchMode == HarkenSearch::CubaseAdapter::HostLaunchMode::ReuseRunningHost,
          "Cubase bridge process contract reuses a running host when available");
    check(processContract.phases.size() >= 6,
          "Cubase bridge process contract preserves the expected lifecycle phases");
    const auto endpoint = HarkenSearch::CubaseAdapter::BuildBridgeEndpoint(request);
    const auto probe = HarkenSearch::CubaseAdapter::ProbeBridgeEndpoint(endpoint);
    check(probe.available,
          "Cubase bridge probe succeeds when a mock bridge server responds to HELLO");
    check(probe.remoteSessionId == "auro-test-session",
          "Cubase bridge probe preserves remote session id from HELLO");
    check(probe.snapshotToken == "snap-mock-001",
          "Cubase bridge probe preserves snapshot token from HELLO");
    check(probe.projectState == HarkenSearch::CubaseAdapter::HostProjectState::ProjectReady,
          "Cubase bridge probe preserves host project-ready state from HELLO");
    check(probe.trackState == HarkenSearch::CubaseAdapter::HostTrackState::SelectedTrackReady,
          "Cubase bridge probe preserves selected-track state from HELLO");
    check(probe.projectName == "MockProjectA",
          "Cubase bridge probe preserves project name from HELLO");
    check(probe.selectedTrackName == "FX 01",
          "Cubase bridge probe preserves selected-track identity from HELLO");
    check(probe.cursorLabel == "Bar 9 Beat 3",
          "Cubase bridge probe preserves cursor label from HELLO");
    check(probe.supportsSelectedTrack && probe.supportsCursorTrack,
          "Cubase bridge probe preserves host capability snapshot from HELLO");
    const auto insertMessage = HarkenSearch::CubaseAdapter::BuildBridgeInsertMessage(request);
    check(insertMessage.find("INSERT|") == 0 && insertMessage.find("track=") != std::string::npos,
          "Cubase bridge insert message carries insert verb and track metadata");
    check(insertMessage.find("policy=") != std::string::npos &&
              insertMessage.find("sessionAffinity=") != std::string::npos &&
              insertMessage.find("preferredTrack=") != std::string::npos,
          "Cubase bridge insert message carries richer insert intent fields");
    auto requestWithSnapshot = request;
    requestWithSnapshot.expectedSnapshotToken = "snap-mock-001";
    requestWithSnapshot.expectedProjectName = "MockProjectA";
    requestWithSnapshot.expectedSelectedTrackName = "FX 01";
    requestWithSnapshot.expectedCursorLabel = "Bar 9 Beat 3";
    const auto insertMessageWithSnapshot = HarkenSearch::CubaseAdapter::BuildBridgeInsertMessage(requestWithSnapshot);
    check(insertMessageWithSnapshot.find("snapshot=snap-mock-001") != std::string::npos,
          "Cubase bridge insert message carries attach snapshot token when available");
    check(insertMessageWithSnapshot.find("projectName=MockProjectA") != std::string::npos &&
              insertMessageWithSnapshot.find("selectedTrack=FX 01") != std::string::npos &&
              insertMessageWithSnapshot.find("cursorLabel=Bar 9 Beat 3") != std::string::npos,
          "Cubase bridge insert message carries attach project/track/cursor addressing when available");
    const auto result = HarkenSearch::CubaseAdapter::ExecuteInsertRequest(request);
    check(result.success,
          "Cubase execute path can complete a mock bridge round-trip");
    check(result.code == HarkenSearch::CubaseAdapter::ResultCode::Success,
          "Cubase execute path returns Success when mock bridge accepts the request");
    check(result.remoteSessionId == "auro-test-session",
          "Cubase execute path preserves remote session id from bridge traffic");
    check(result.snapshotToken == "snap-mock-001",
          "Cubase execute path preserves snapshot token from bridge traffic");
    check(result.projectState == HarkenSearch::CubaseAdapter::HostProjectState::ProjectReady,
          "Cubase execute path preserves project-ready state from bridge traffic");
    check(result.trackState == HarkenSearch::CubaseAdapter::HostTrackState::SelectedTrackReady,
          "Cubase execute path preserves selected-track state from bridge traffic");
    check(result.trackResolution == HarkenSearch::CubaseAdapter::TrackResolutionKind::SelectedTrackAccepted,
          "Cubase execute path preserves selected-track acceptance semantics");
    check(result.projectName == "MockProjectA",
          "Cubase execute path preserves project name from bridge traffic");
    check(result.selectedTrackName == "FX 01",
          "Cubase execute path preserves selected-track identity from bridge traffic");
    check(result.cursorLabel == "Bar 9 Beat 3",
          "Cubase execute path preserves cursor label from bridge traffic");
    check(result.status.find("FX 01") != std::string::npos,
          "Cubase execute path surfaces accepted track from the bridge response");
    check(result.status.find("edit-cursor") != std::string::npos,
          "Cubase execute path surfaces accepted position mode from the bridge response");
    check(result.status.find("project=MockProjectA") != std::string::npos,
          "Cubase execute path surfaces project name in success status");

    ClearBridgeStubEnv();
    StopMockBridgeServer(thread);
}

static void testCubaseBridgeProcessContractLaunchPath() {
    DawTarget target;
    target.name = "Cubase 14";
    target.processName = "Cubase14.exe";
    target.exePath = "C:/Program Files/Steinberg/Cubase 14/Cubase14.exe";
    target.mode = DawSendMode::LaunchExe;
    target.isRunning = false;

    const auto request = HarkenSearch::CubaseAdapter::BuildInsertRequest(
        target,
        std::vector<std::string>{"C:/audio/impact.wav"});
    const auto processContract = HarkenSearch::CubaseAdapter::BuildBridgeProcessContract(request);
    check(processContract.launchMode == HarkenSearch::CubaseAdapter::HostLaunchMode::LaunchHostThenAttach,
          "Cubase bridge process contract can model launch-then-attach when host is not running");
    check(processContract.hostExecutableHint.find("Cubase 14") != std::string::npos,
          "Cubase bridge process contract preserves the host executable hint");
    check(processContract.failureMatrix.size() >= 4,
          "Cubase bridge process contract keeps a non-trivial launch/attach failure matrix");
    const auto hostContract = HarkenSearch::CubaseAdapter::BuildHostBridgeV0Contract(request);
    check(!hostContract.hostFailureSurface.empty() &&
              hostContract.hostFailureSurface.front() == HarkenSearch::CubaseAdapter::ResultCode::BridgeNotReady,
          "Cubase host bridge v0 contract puts BridgeNotReady at the front of host-side failure vocabulary");
}

static void testCubaseMockBridgeErrorMatrix() {
    DawTarget target;
    target.name = "Cubase 14";
    target.processName = "Cubase14.exe";
    target.exePath = "C:/Program Files/Steinberg/Cubase 14/Cubase14.exe";
    target.mode = DawSendMode::LaunchExe;
    target.isRunning = true;

    KillBridgeHelperProcesses();
    HANDLE thread = StartMockBridgeServer(MockBridgeMode::HappyPath);
    SetBridgeStubEnvResolved();
    SetBridgeLaunchStub("success");

    const auto request = HarkenSearch::CubaseAdapter::BuildInsertRequest(
        target,
        std::vector<std::string>{"C:/audio/impact.wav"});

    g_bridgeMode.store(MockBridgeMode::PartialSuccess);
    const auto partialResult = HarkenSearch::CubaseAdapter::ExecuteInsertRequest(request);
    check(partialResult.code == HarkenSearch::CubaseAdapter::ResultCode::PartialSuccess,
          "Cubase execute path preserves partial success from the bridge");
    check(partialResult.status.find("partial success") != std::string::npos,
          "Cubase execute path surfaces partial success in status");
    check(partialResult.status.find("host adjusted position") != std::string::npos,
          "Cubase execute path surfaces host-adjusted position in status");
    check(partialResult.acceptedTrack == "FX ALT",
          "Cubase execute path surfaces remapped track");
    check(partialResult.trackState == HarkenSearch::CubaseAdapter::HostTrackState::TrackRemapped,
          "Cubase execute path preserves remapped track-state");
    check(partialResult.trackResolution == HarkenSearch::CubaseAdapter::TrackResolutionKind::RemappedTrack,
          "Cubase execute path preserves remapped-track resolution");
    check(partialResult.insertedClipIds.size() == 1 && partialResult.insertedClipIds[0] == "clip-001",
          "Cubase execute path preserves inserted clip ids");

    g_bridgeMode.store(MockBridgeMode::SnapshotMismatch);
    const auto snapshotResult = HarkenSearch::CubaseAdapter::ExecuteInsertRequest(request);
    check(snapshotResult.code == HarkenSearch::CubaseAdapter::ResultCode::BridgeNotReady,
          "Cubase execute path preserves snapshot-mismatch as host-not-ready instead of bridge-unavailable");
    check(snapshotResult.addressingDrift == HarkenSearch::CubaseAdapter::AddressingDriftKind::SnapshotMismatch,
          "Cubase execute path preserves snapshot-mismatch detail");

    auto staleProjectRequest = request;
    staleProjectRequest.expectedProjectName = "WrongProject";
    g_bridgeMode.store(MockBridgeMode::HappyPath);
    const auto projectMismatchResult = HarkenSearch::CubaseAdapter::ExecuteInsertRequest(staleProjectRequest);
    check(projectMismatchResult.code == HarkenSearch::CubaseAdapter::ResultCode::BridgeNotReady,
          "Cubase execute path preserves project-address mismatch as host-not-ready");
    check(projectMismatchResult.addressingDrift == HarkenSearch::CubaseAdapter::AddressingDriftKind::ProjectMismatch,
          "Cubase execute path preserves project-address mismatch detail");

    auto staleSelectedTrackRequest = request;
    staleSelectedTrackRequest.expectedSelectedTrackName = "FX 99";
    const auto selectedTrackMismatchResult = HarkenSearch::CubaseAdapter::ExecuteInsertRequest(staleSelectedTrackRequest);
    check(selectedTrackMismatchResult.code == HarkenSearch::CubaseAdapter::ResultCode::TrackUnavailable,
          "Cubase execute path preserves selected-track-address mismatch as track unavailable");
    check(selectedTrackMismatchResult.trackResolution == HarkenSearch::CubaseAdapter::TrackResolutionKind::SelectedTrackChanged,
          "Cubase execute path preserves selected-track-address mismatch detail");

    auto staleCursorRequest = request;
    staleCursorRequest.expectedCursorLabel = "Bar 99 Beat 4";
    const auto cursorMismatchResult = HarkenSearch::CubaseAdapter::ExecuteInsertRequest(staleCursorRequest);
    check(cursorMismatchResult.code == HarkenSearch::CubaseAdapter::ResultCode::BridgeNotReady,
          "Cubase execute path preserves cursor-address mismatch as host-not-ready");
    check(cursorMismatchResult.addressingDrift == HarkenSearch::CubaseAdapter::AddressingDriftKind::CursorMismatch,
          "Cubase execute path preserves cursor-address mismatch detail");

    g_bridgeMode.store(MockBridgeMode::SessionMismatch);
    const auto sessionResult = HarkenSearch::CubaseAdapter::ExecuteInsertRequest(request);
    check(sessionResult.code == HarkenSearch::CubaseAdapter::ResultCode::SessionMismatch,
          "Cubase execute path preserves session mismatch from the bridge");

    g_bridgeMode.store(MockBridgeMode::TrackUnavailable);
    const auto trackResult = HarkenSearch::CubaseAdapter::ExecuteInsertRequest(request);
    check(trackResult.code == HarkenSearch::CubaseAdapter::ResultCode::TrackUnavailable,
          "Cubase execute path preserves track unavailable from the bridge");
    check(trackResult.trackResolution == HarkenSearch::CubaseAdapter::TrackResolutionKind::SelectedTrackMissing,
          "Cubase execute path preserves selected-track-missing detail");

    ClearBridgeStubEnv();
    StopMockBridgeServer(thread);
}

static void testCubaseExecuteFallsBackToAttachSessionId() {
    DawTarget target;
    target.name = "Cubase 14";
    target.processName = "Cubase14.exe";
    target.exePath = "C:/Program Files/Steinberg/Cubase 14/Cubase14.exe";
    target.mode = DawSendMode::LaunchExe;
    target.isRunning = true;

    KillBridgeHelperProcesses();
    HANDLE thread = StartMockBridgeServer(MockBridgeMode::HappyPathNoInsertSession);
    SetBridgeStubEnvResolved();
    SetBridgeLaunchStub("success");

    const auto request = HarkenSearch::CubaseAdapter::BuildInsertRequest(
        target,
        std::vector<std::string>{"C:/audio/impact.wav"});
    const auto result = HarkenSearch::CubaseAdapter::ExecuteInsertRequest(request);
    check(result.success,
          "Cubase execute path still succeeds when INSERT ack omits the session id");
    check(result.remoteSessionId == "auro-test-session",
          "Cubase execute path falls back to attach-session id when INSERT ack omits session");
    check(result.snapshotToken == "snap-mock-001",
          "Cubase execute path falls back to attach snapshot token when INSERT ack omits snapshot");

    ClearBridgeStubEnv();
    StopMockBridgeServer(thread);
}

static void testCubaseRealHelperSpawnRoundTrip() {
    DawTarget target;
    target.name = "Cubase 14";
    target.processName = "Cubase14.exe";
    target.exePath = "C:/Program Files/Steinberg/Cubase 14/Cubase14.exe";
    target.mode = DawSendMode::LaunchExe;
    target.isRunning = true;

    StopMockBridgeServer(NULL);
    KillBridgeHelperProcesses();
    SetBridgeHelperEnvResolved();
    const auto helperPath = BuiltBridgeHelperPathUtf8();
    check(GetFileAttributesA(helperPath.c_str()) != INVALID_FILE_ATTRIBUTES,
          "Cubase real helper spawn test can locate the built helper executable");

    const auto request = HarkenSearch::CubaseAdapter::BuildInsertRequest(
        target,
        std::vector<std::string>{"C:/audio/impact.wav"});
    const auto result = HarkenSearch::CubaseAdapter::ExecuteInsertRequest(request);
    check(result.success,
          "Cubase execute path can round-trip through the real spawned helper executable");
    check(result.code == HarkenSearch::CubaseAdapter::ResultCode::Success,
          "Cubase real helper spawn path returns Success");
    check(result.remoteSessionId.rfind("auro-helper-session-", 0) == 0,
          "Cubase real helper spawn path preserves helper session id");
    check(result.snapshotToken == "snap-demo-001",
          "Cubase real helper spawn path preserves helper snapshot token");
    check(result.projectState == HarkenSearch::CubaseAdapter::HostProjectState::ProjectReady,
          "Cubase real helper spawn path preserves project-ready state");
    check(result.trackState == HarkenSearch::CubaseAdapter::HostTrackState::SelectedTrackReady,
          "Cubase real helper spawn path preserves selected-track state");
    check(result.trackResolution == HarkenSearch::CubaseAdapter::TrackResolutionKind::SelectedTrackAccepted,
          "Cubase real helper spawn path preserves selected-track acceptance semantics");
    check(result.projectName == "Harken Search Bridge Demo Project",
          "Cubase real helper spawn path preserves helper project name");
    check(result.selectedTrackName == "Selected Track",
          "Cubase real helper spawn path preserves helper selected-track name");
    check(result.cursorLabel == "Bar 17 Beat 1",
          "Cubase real helper spawn path preserves helper cursor label");
    check(result.status.find("Selected Track") != std::string::npos,
          "Cubase real helper spawn path surfaces helper-selected track");

    ClearBridgeStubEnv();
    KillBridgeHelperProcesses();
}

static void testCubaseNativeProcessLaunchOwnershipMatrix() {
    ClearBridgeStubEnv();
    HarkenSearch::CubaseAdapter::Testing::SetBridgeLaunchHook(nullptr);
    Platform::ProcessLaunchTesting::SetBackendOps(&kProcessLaunchOps);

    HarkenSearch::CubaseAdapter::BridgeLaunchPlan plan;
    plan.shouldLaunchBridgeHelper = true;
    plan.helperExecutablePath = u8"C:/Bridge Dir/妗ユ帴 Helper.exe";
    plan.helperLaunchArguments = u8"--pipe \\\\.\\pipe\\auro test --token 浠ょ墝馃帶";
    ResetProcessLaunch();
    auto result = HarkenSearch::CubaseAdapter::ExecuteBridgeLaunchAttempt(plan);
    check(result.attempted && result.success && !result.usedStub &&
              result.processId == 8123 && result.detail == "Bridge helper process started",
          "Cubase native process launch returns the captured PID and exact success detail");
    check(g_processLaunch.createCalls == 1 &&
              g_processLaunch.applicationName == L"C:/Bridge Dir/妗ユ帴 Helper.exe" &&
              g_processLaunch.commandLine ==
                  L"\"C:/Bridge Dir/妗ユ帴 Helper.exe\" --pipe \\\\.\\pipe\\auro test --token 浠ょ墝馃帶",
          "Cubase native process launch preserves application name and mutable quoted command line");
    check(g_processLaunch.processAttributes == nullptr &&
              g_processLaunch.threadAttributes == nullptr &&
              g_processLaunch.inheritHandles == FALSE &&
              g_processLaunch.creationFlags == CREATE_NO_WINDOW &&
              g_processLaunch.environment == nullptr &&
              g_processLaunch.currentDirectory == nullptr &&
              g_processLaunch.startupCb == sizeof(STARTUPINFOW),
          "Cubase native process launch preserves security, inheritance, flags, and startup options");
    check(g_processLaunch.closed ==
              std::vector<HANDLE>{g_processLaunch.threadHandle, g_processLaunch.processHandle},
          "Cubase native process launch closes thread then process exactly once");

    ResetProcessLaunch();
    g_processLaunch.processId = 0;
    g_processLaunch.processHandle = nullptr;
    g_processLaunch.threadHandle = INVALID_HANDLE_VALUE;
    g_processLaunch.closeResult = false;
    result = HarkenSearch::CubaseAdapter::ExecuteBridgeLaunchAttempt(plan);
    check(result.attempted && result.success && result.processId == 0 &&
              !result.usedStub && result.detail == "Bridge helper process started",
          "Cubase native process launch preserves anomalous successful zero-PID result");
    check(g_processLaunch.closed ==
              std::vector<HANDLE>{INVALID_HANDLE_VALUE, nullptr},
          "Cubase native process launch attempts both failing closes in order");

    plan.helperLaunchArguments.clear();
    ResetProcessLaunch();
    result = HarkenSearch::CubaseAdapter::ExecuteBridgeLaunchAttempt(plan);
    check(result.success && g_processLaunch.commandLine ==
              L"\"C:/Bridge Dir/妗ユ帴 Helper.exe\"",
          "Cubase native process launch accepts empty arguments without a trailing separator");

    for (const auto& failure : std::vector<std::pair<DWORD, HarkenSearch::CubaseAdapter::LaunchFailureKind>>{
             {ERROR_FILE_NOT_FOUND, HarkenSearch::CubaseAdapter::LaunchFailureKind::BridgeExecutableMissing},
             {ERROR_PATH_NOT_FOUND, HarkenSearch::CubaseAdapter::LaunchFailureKind::BridgeExecutableMissing},
             {ERROR_ACCESS_DENIED, HarkenSearch::CubaseAdapter::LaunchFailureKind::BridgeLaunchFailed}}) {
        ResetProcessLaunch(false, failure.first);
        result = HarkenSearch::CubaseAdapter::ExecuteBridgeLaunchAttempt(plan);
        check(result.attempted && !result.success && !result.usedStub &&
                  result.processId == 0 && result.failureKind == failure.second &&
                  result.detail == "Bridge helper process start failed with error " +
                      std::to_string(failure.first) &&
                  g_processLaunch.createCalls == 1 && g_processLaunch.closed.empty(),
              "Cubase native process launch maps failure and never closes unowned handles");
    }

    Platform::ProcessLaunchTesting::SetBackendOps(nullptr);
}

static void testCubaseBridgeLaunchPathConversionFailsClosed() {
    ClearBridgeStubEnv();
    HarkenSearch::CubaseAdapter::Testing::SetBridgeLaunchHook(&CaptureBridgeLaunch);

    HarkenSearch::CubaseAdapter::BridgeLaunchPlan nulPlan;
    nulPlan.shouldLaunchBridgeHelper = true;
    nulPlan.helperExecutablePath = std::string("C:/bridge.exe\0shadow.exe", 24);
    nulPlan.helperLaunchArguments = "--pipe ascii-pipe";
    ResetBridgeLaunchCapture();
    const auto nulResult = HarkenSearch::CubaseAdapter::ExecuteBridgeLaunchAttempt(nulPlan);
    check(nulResult.attempted && !nulResult.success && nulResult.processId == 0 &&
              !nulResult.usedStub &&
              nulResult.failureKind == HarkenSearch::CubaseAdapter::LaunchFailureKind::BridgeLaunchFailed &&
              nulResult.detail == "Bridge helper launch path conversion failed",
          "Cubase helper launch fails closed on an embedded-NUL executable path");
    check(g_launchHookCalls == 0,
          "Cubase helper launch rejects an embedded-NUL path before process creation");

    HarkenSearch::CubaseAdapter::BridgeLaunchPlan nulArgsPlan;
    nulArgsPlan.shouldLaunchBridgeHelper = true;
    nulArgsPlan.helperExecutablePath = "C:/bridge.exe";
    nulArgsPlan.helperLaunchArguments = std::string("--pipe ascii\0shadow", 19);
    ResetBridgeLaunchCapture();
    const auto nulArgsResult =
        HarkenSearch::CubaseAdapter::ExecuteBridgeLaunchAttempt(nulArgsPlan);
    check(nulArgsResult.attempted && !nulArgsResult.success &&
              nulArgsResult.processId == 0 && !nulArgsResult.usedStub &&
              nulArgsResult.failureKind ==
                  HarkenSearch::CubaseAdapter::LaunchFailureKind::BridgeLaunchFailed &&
              nulArgsResult.detail == "Bridge helper launch path conversion failed",
          "Cubase helper launch fails closed on embedded-NUL nonempty arguments");
    check(g_launchHookCalls == 0,
          "Cubase helper launch rejects invalid nonempty arguments before process creation");

    HarkenSearch::CubaseAdapter::BridgeLaunchPlan unicodePlan;
    unicodePlan.shouldLaunchBridgeHelper = true;
    unicodePlan.helperExecutablePath = u8"C:/桥接/Helper.exe";
    unicodePlan.helperLaunchArguments = u8"--token 令牌🎧";
    ResetBridgeLaunchCapture();
    const auto unicodeResult = HarkenSearch::CubaseAdapter::ExecuteBridgeLaunchAttempt(unicodePlan);
    check(unicodeResult.success && unicodeResult.processId == 7331 &&
              g_launchHookCalls == 1 &&
              g_launchHookExecutable == L"C:/桥接/Helper.exe" &&
              g_launchHookArguments == L"--token 令牌🎧",
          "Cubase helper launch passes exact Unicode executable and arguments to the process seam");

    HarkenSearch::CubaseAdapter::BridgeLaunchPlan malformedPlan;
    malformedPlan.shouldLaunchBridgeHelper = true;
    malformedPlan.helperExecutablePath = "C:/bad_";
    malformedPlan.helperExecutablePath.append("\xF0\x28\x8C\x28", 4);
    malformedPlan.helperExecutablePath += ".exe";
    ResetBridgeLaunchCapture();
    const auto malformedResult = HarkenSearch::CubaseAdapter::ExecuteBridgeLaunchAttempt(malformedPlan);
    check(malformedResult.success && g_launchHookCalls == 1 &&
              g_launchHookExecutable == L"C:/bad_\uFFFD(\uFFFD(.exe" &&
              g_launchHookArguments.empty(),
          "Cubase helper launch permits empty args and sends malformed UTF-8 replacement to the process seam");

    HarkenSearch::CubaseAdapter::Testing::SetBridgeLaunchHook(nullptr);
    ClearBridgeStubEnv();
}

static void testCubaseBridgeExecutablePathExistenceContract() {
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() /
        ("auro_cubase_exists_" + std::to_string(GetCurrentProcessId()));
    const fs::path unicodeFile = root / u8"桥接 🎛 helper.exe";
    const fs::path unicodeDirectory = root / u8"目录 🎧";
    const fs::path missing = root / u8"缺失 🎚.exe";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(unicodeDirectory, ec);
    std::ofstream(unicodeFile, std::ios::binary).put('x');

    const std::string fileUtf8 = unicodeFile.u8string();
    const std::string directoryUtf8 = unicodeDirectory.u8string();
    const std::string missingUtf8 = missing.u8string();
    check(!Platform::FileSystem::Exists(""),
          "Filesystem PAL reports an empty helper path missing");
    check(!Platform::FileSystem::Exists(missingUtf8),
          "Filesystem PAL reports a missing Unicode helper path missing");
    check(Platform::FileSystem::Exists(fileUtf8),
          "Filesystem PAL finds a CJK/emoji helper file");
    check(Platform::FileSystem::Exists(directoryUtf8),
          "Filesystem PAL preserves directory-as-present helper semantics");
    check(!Platform::FileSystem::Exists((root / std::string(40000, 'x')).u8string()),
          "Filesystem PAL maps a path query error to false");
    std::string nulPath = fileUtf8;
    nulPath.push_back('\0');
    nulPath += ".missing";
    check(Platform::FileSystem::Exists(nulPath),
          "Filesystem PAL preserves Windows valid-prefix embedded-NUL behavior");

    DawTarget target;
    target.name = "Cubase 14";
    target.processName = "Cubase14.exe";
    target.exePath = "C:/Program Files/Steinberg/Cubase 14/Cubase14.exe";
    target.mode = DawSendMode::LaunchExe;
    target.isRunning = true;
    const auto request = HarkenSearch::CubaseAdapter::BuildInsertRequest(target, {});
    const auto resolveOverride = [&](const std::string& path) {
        ClearBridgeStubEnv();
        SetBridgeStubPath(path);
        return HarkenSearch::CubaseAdapter::ResolveBridgeExecutable(request);
    };
    const auto fileResolution = resolveOverride(fileUtf8);
    check(fileResolution.available && fileResolution.environmentOverride &&
              fileResolution.executablePath == fileUtf8 &&
              fileResolution.detail == "Using bridge helper override from AURO_CUBASE_BRIDGE_STUB_PATH: " + fileUtf8,
          "Cubase resolution accepts a Unicode helper file with exact detail");
    const auto directoryResolution = resolveOverride(directoryUtf8);
    check(directoryResolution.available && directoryResolution.environmentOverride &&
              directoryResolution.executablePath == directoryUtf8 &&
              directoryResolution.detail == "Using bridge helper override from AURO_CUBASE_BRIDGE_STUB_PATH: " + directoryUtf8,
          "Cubase resolution preserves directory-as-present availability and detail");
    const auto missingResolution = resolveOverride(missingUtf8);
    check(!missingResolution.available && missingResolution.environmentOverride &&
              missingResolution.executablePath == missingUtf8 &&
              missingResolution.detail == "Bridge helper override path from AURO_CUBASE_BRIDGE_STUB_PATH does not exist: " + missingUtf8,
          "Cubase resolution preserves missing-helper availability and detail");
    ClearBridgeStubEnv();
    std::string malformedDirectory = (root / "bad_").u8string();
    malformedDirectory.append("\xF0\x28\x8C\x28", 4);
    auto malformedRequest = request;
    malformedRequest.executablePath = malformedDirectory + "/Cubase14.exe";
    const std::string malformedCandidate =
        malformedDirectory + "/HarkenSearchCubaseBridgeStub.exe";
    const auto malformedResolution =
        HarkenSearch::CubaseAdapter::ResolveBridgeExecutable(malformedRequest);
    check(!malformedResolution.available && !malformedResolution.environmentOverride &&
              malformedResolution.executablePath == malformedCandidate &&
              malformedResolution.detail == "Bridge helper was not found next to Cubase host: " + malformedCandidate,
          "Cubase resolution maps malformed helper path bytes to unavailable without throwing");

    ClearBridgeStubEnv();
    fs::remove_all(root, ec);
}

static void testCubaseCurrentProcessExecutablePathStatusMatrix() {
    HarkenSearch::CubaseAdapter::InsertRequest request;
    request.targetName = "Nuendo 14";
    request.processName = "Nuendo14.exe";
    request.hostRunning = true;
    const std::string processPath = CurrentProcessPathUtf8();
    CubaseExecutablePathStub::Reset(processPath);
    const auto success = HarkenSearch::CubaseAdapter::ResolveBridgeExecutable(request);
    const std::string helperPath = BuiltNuendoBridgeHelperPathUtf8();
    check(success.available && success.executablePath == helperPath &&
              success.detail == "Resolved bundled bridge helper next to Harken Search: " + helperPath,
          "Cubase adapter accepts a successful nonempty executable PAL path");

    const std::string unavailableDetail =
        "Nuendo bridge helper cannot be resolved because the host executable path is not configured";
    const auto checkUnavailable = [&](Platform::ExecutablePathStatus status,
                                      const std::string& path,
                                      const char* message) {
        Platform::ExecutablePathResult result;
        result.status = status;
        result.pathUtf8 = path;
        result.systemError = 123;
        CubaseExecutablePathStub::SetResult(std::move(result));
        const auto resolution = HarkenSearch::CubaseAdapter::ResolveBridgeExecutable(request);
        check(!resolution.available && resolution.executablePath.empty() &&
                  resolution.detail == unavailableDetail,
              message);
    };
    checkUnavailable(Platform::ExecutablePathStatus::Success, {},
                     "Cubase adapter rejects Success with an empty executable path");
    for (const auto status : {Platform::ExecutablePathStatus::Unsupported,
                              Platform::ExecutablePathStatus::QueryFailed,
                              Platform::ExecutablePathStatus::Truncated,
                              Platform::ExecutablePathStatus::EncodingFailed}) {
        checkUnavailable(status, processPath,
                         "Cubase adapter ignores poison paths on failed executable status");
    }
    CubaseExecutablePathStub::Reset(processPath);
}

static void testBridgeHelpersPreserveUnicodeHandshakeArgv() {
    const auto cubase = RunUnicodeArgvHandshake(
        L"HarkenSearchCubaseBridgeStub.exe", L"cubase");
    check(cubase.tokenAccepted,
          "Cubase helper accepts the exact Unicode handshake token");
    check(cubase.mismatchRejected,
          "Cubase helper rejects a mismatched token with the exact response");
    check(cubase.responseFraming,
          "Cubase helper frames handshake responses with a newline");
    check(cubase.shutdownExited,
          "Cubase helper exits after the shutdown handshake");

    const auto nuendo = RunUnicodeArgvHandshake(
        L"HarkenSearchNuendoBridgeStub.exe", L"nuendo");
    check(nuendo.tokenAccepted,
          "Nuendo helper accepts the exact Unicode handshake token");
    check(nuendo.mismatchRejected,
          "Nuendo helper rejects a mismatched token with the exact response");
    check(nuendo.responseFraming,
          "Nuendo helper frames handshake responses with a newline");
    check(nuendo.shutdownExited,
          "Nuendo helper exits after the shutdown handshake");
}

static void testNuendoRealHelperSpawnRoundTrip() {
    DawTarget target;
    target.name = "Nuendo 14";
    target.processName = "Nuendo14.exe";
    target.exePath = "C:/Program Files/Steinberg/Nuendo 14/Nuendo14.exe";
    target.mode = DawSendMode::LaunchExe;
    target.isRunning = true;

    StopMockBridgeServer(NULL);
    KillBridgeHelperProcesses();
    SetNuendoBridgeHelperEnvResolved();
    const auto helperPath = BuiltNuendoBridgeHelperPathUtf8();
    check(GetFileAttributesA(helperPath.c_str()) != INVALID_FILE_ATTRIBUTES,
          "Nuendo real helper spawn test can locate the built helper executable");

    const auto request = HarkenSearch::CubaseAdapter::BuildInsertRequest(
        target,
        std::vector<std::string>{"C:/audio/impact.wav"});
    const auto result = HarkenSearch::CubaseAdapter::ExecuteInsertRequest(request);
    check(!result.success,
          "Nuendo execute path does not treat the spawned stub helper as a real host insert success");
    check(result.code == HarkenSearch::CubaseAdapter::ResultCode::BridgeNotImplemented,
          "Nuendo real helper spawn path reports BridgeNotImplemented while only the stub helper exists");
    check(result.status.find("protocol stub helper") != std::string::npos,
          "Nuendo real helper spawn path explains that the current helper is still a protocol stub");

    ClearBridgeStubEnv();
    KillBridgeHelperProcesses();
}

static void testNuendoBundledHelperAutoDetectedWithoutEnv() {
    DawTarget target;
    target.name = "Nuendo 14";
    target.processName = "Nuendo14.exe";
    target.exePath = "C:/Program Files/Steinberg/Nuendo 14/Nuendo14.exe";
    target.mode = DawSendMode::LaunchExe;
    target.isRunning = true;

    StopMockBridgeServer(NULL);
    KillBridgeHelperProcesses();
    ClearBridgeStubEnv();
    const auto helperPath = BuiltNuendoBridgeHelperPathUtf8();
    check(GetFileAttributesA(helperPath.c_str()) != INVALID_FILE_ATTRIBUTES,
          "Nuendo bundled helper auto-detect test can locate the built helper executable");

    const auto request = HarkenSearch::CubaseAdapter::BuildInsertRequest(
        target,
        std::vector<std::string>{"C:/audio/impact.wav"});
    check(request.adapterAvailable,
          "Nuendo request treats the bundled helper next to the application executable as an installed adapter without env overrides");

    const auto resolution = HarkenSearch::CubaseAdapter::ResolveBridgeExecutable(request);
    check(resolution.available,
          "Nuendo helper resolution finds the bundled helper next to the application executable without env overrides");
    check(resolution.executablePath.find("HarkenSearchNuendoBridgeStub.exe") != std::string::npos,
          "Nuendo helper resolution prefers the Nuendo-specific bundled helper");

    const auto preflight = HarkenSearch::CubaseAdapter::EvaluatePreflight(target);
    check(preflight.readiness == HarkenSearch::CubaseAdapter::Readiness::BridgeNotImplemented,
          "Nuendo preflight stays truthful when only the bundled protocol stub helper exists");
    check(preflight.failureReason.find("stub bridge helper") != std::string::npos,
          "Nuendo preflight explains that the resolved helper is still only a stub");

    const auto result = HarkenSearch::CubaseAdapter::ExecuteInsertRequest(request);
    check(!result.success,
          "Nuendo execute path does not claim success when only the bundled stub helper is available");
    check(result.code == HarkenSearch::CubaseAdapter::ResultCode::BridgeNotImplemented,
          "Nuendo bundled-helper no-env execute path returns BridgeNotImplemented");

    ClearBridgeStubEnv();
    KillBridgeHelperProcesses();
}

static void testCubaseRealHelperStatefulAckAndFailures() {
    DawTarget target;
    target.name = "Cubase 14";
    target.processName = "Cubase14.exe";
    target.exePath = "C:/Program Files/Steinberg/Cubase 14/Cubase14.exe";
    target.mode = DawSendMode::LaunchExe;
    target.isRunning = true;

    StopMockBridgeServer(NULL);
    KillBridgeHelperProcesses();
    SetBridgeHelperEnvResolved();

    auto partialRequest = HarkenSearch::CubaseAdapter::BuildInsertRequest(
        target,
        std::vector<std::string>{"C:/audio/impact.wav", "C:/audio/riser.wav"});
    partialRequest.insertPolicy = HarkenSearch::CubaseAdapter::InsertPolicy::SpreadAcrossSelection;
    partialRequest.preferredTrackName = "FX BUS";
    const auto partialResult = HarkenSearch::CubaseAdapter::ExecuteInsertRequest(partialRequest);
    check(partialResult.code == HarkenSearch::CubaseAdapter::ResultCode::PartialSuccess,
          "Cubase real helper can surface partial success semantics");
    check(partialResult.acceptedTrack == "FX ALT",
          "Cubase real helper can surface remapped track semantics");
    check(partialResult.trackState == HarkenSearch::CubaseAdapter::HostTrackState::TrackRemapped,
          "Cubase real helper can surface remapped track-state semantics");
    check(partialResult.trackResolution == HarkenSearch::CubaseAdapter::TrackResolutionKind::RemappedTrack,
          "Cubase real helper can surface remapped-track resolution semantics");
    check(partialResult.snapshotToken == "snap-demo-001",
          "Cubase real helper partial success preserves snapshot token");
    check(partialResult.projectName == "Harken Search Bridge Demo Project",
          "Cubase real helper partial success preserves project name");
    check(partialResult.status.find("partial success") != std::string::npos,
          "Cubase real helper surfaces partial success status text");
    check(partialResult.status.find("host adjusted position") != std::string::npos,
          "Cubase real helper surfaces adjusted position status text");

    KillBridgeHelperProcesses();
    auto sessionRequest = HarkenSearch::CubaseAdapter::BuildInsertRequest(
        target,
        std::vector<std::string>{"C:/audio/impact.wav"});
    sessionRequest.requestedSessionId = "missing-session";
    const auto sessionResult = HarkenSearch::CubaseAdapter::ExecuteInsertRequest(sessionRequest);
    check(sessionResult.code == HarkenSearch::CubaseAdapter::ResultCode::SessionMismatch,
          "Cubase real helper can surface session mismatch semantics");

    KillBridgeHelperProcesses();
    auto snapshotRequest = HarkenSearch::CubaseAdapter::BuildInsertRequest(
        target,
        std::vector<std::string>{"C:/audio/impact.wav"});
    snapshotRequest.expectedSnapshotToken = "stale-snapshot";
    const auto snapshotResult = HarkenSearch::CubaseAdapter::ExecuteInsertRequest(snapshotRequest);
    check(snapshotResult.code == HarkenSearch::CubaseAdapter::ResultCode::BridgeNotReady,
          "Cubase real helper can surface snapshot mismatch semantics");
    check(snapshotResult.addressingDrift == HarkenSearch::CubaseAdapter::AddressingDriftKind::SnapshotMismatch,
          "Cubase real helper preserves snapshot mismatch detail");

    KillBridgeHelperProcesses();
    auto projectRequest = HarkenSearch::CubaseAdapter::BuildInsertRequest(
        target,
        std::vector<std::string>{"C:/audio/impact.wav"});
    projectRequest.expectedProjectName = "Wrong Project";
    const auto projectResult = HarkenSearch::CubaseAdapter::ExecuteInsertRequest(projectRequest);
    check(projectResult.code == HarkenSearch::CubaseAdapter::ResultCode::BridgeNotReady,
          "Cubase real helper can surface project-address mismatch semantics");
    check(projectResult.addressingDrift == HarkenSearch::CubaseAdapter::AddressingDriftKind::ProjectMismatch,
          "Cubase real helper preserves project-address mismatch detail");

    KillBridgeHelperProcesses();
    auto selectedTrackAddressRequest = HarkenSearch::CubaseAdapter::BuildInsertRequest(
        target,
        std::vector<std::string>{"C:/audio/impact.wav"});
    selectedTrackAddressRequest.expectedSelectedTrackName = "Wrong Selected Track";
    const auto selectedTrackAddressResult = HarkenSearch::CubaseAdapter::ExecuteInsertRequest(selectedTrackAddressRequest);
    check(selectedTrackAddressResult.code == HarkenSearch::CubaseAdapter::ResultCode::TrackUnavailable,
          "Cubase real helper can surface selected-track-address mismatch semantics");
    check(selectedTrackAddressResult.trackResolution == HarkenSearch::CubaseAdapter::TrackResolutionKind::SelectedTrackChanged,
          "Cubase real helper preserves selected-track-address mismatch detail");

    KillBridgeHelperProcesses();
    auto cursorRequest = HarkenSearch::CubaseAdapter::BuildInsertRequest(
        target,
        std::vector<std::string>{"C:/audio/impact.wav"});
    cursorRequest.expectedCursorLabel = "Bar 32 Beat 4";
    const auto cursorResult = HarkenSearch::CubaseAdapter::ExecuteInsertRequest(cursorRequest);
    check(cursorResult.code == HarkenSearch::CubaseAdapter::ResultCode::BridgeNotReady,
          "Cubase real helper can surface cursor-address mismatch semantics");
    check(cursorResult.addressingDrift == HarkenSearch::CubaseAdapter::AddressingDriftKind::CursorMismatch,
          "Cubase real helper preserves cursor-address mismatch detail");

    KillBridgeHelperProcesses();
    auto trackRequest = HarkenSearch::CubaseAdapter::BuildInsertRequest(
        target,
        std::vector<std::string>{"C:/audio/impact.wav"});
    trackRequest.preferredTrackName = "Missing Track";
    const auto trackResult = HarkenSearch::CubaseAdapter::ExecuteInsertRequest(trackRequest);
    check(trackResult.code == HarkenSearch::CubaseAdapter::ResultCode::TrackUnavailable,
          "Cubase real helper can surface track unavailable semantics");
    check(trackResult.trackResolution == HarkenSearch::CubaseAdapter::TrackResolutionKind::SelectedTrackMissing,
          "Cubase real helper preserves selected-track-missing detail");

    ClearBridgeStubEnv();
    KillBridgeHelperProcesses();
}

static void testCubaseRealHelperPersistentSessionReuseAndShutdown() {
    DawTarget target;
    target.name = "Cubase 14";
    target.processName = "Cubase14.exe";
    target.exePath = "C:/Program Files/Steinberg/Cubase 14/Cubase14.exe";
    target.mode = DawSendMode::LaunchExe;
    target.isRunning = true;

    StopMockBridgeServer(NULL);
    KillBridgeHelperProcesses();
    SetBridgeHelperEnvResolved();

    const auto firstRequest = HarkenSearch::CubaseAdapter::BuildInsertRequest(
        target,
        std::vector<std::string>{"C:/audio/impact.wav"});
    const auto firstResult = HarkenSearch::CubaseAdapter::ExecuteInsertRequest(firstRequest);
    check(firstResult.success,
          "Cubase persistent helper test first insert succeeds");
    check(firstResult.remoteSessionId.rfind("auro-helper-session-", 0) == 0,
          "Cubase persistent helper test first insert exposes helper session id");

    const auto endpoint = HarkenSearch::CubaseAdapter::BuildBridgeEndpoint(firstRequest);
    const auto probeAfterFirstInsert = HarkenSearch::CubaseAdapter::ProbeBridgeEndpoint(endpoint);
    check(probeAfterFirstInsert.available,
          "Cubase helper stays available after first insert when stay-alive is enabled");
    check(probeAfterFirstInsert.remoteSessionId == firstResult.remoteSessionId,
          "Cubase helper reuses the same session id between insert and follow-up probe");

    const auto secondRequest = HarkenSearch::CubaseAdapter::BuildInsertRequest(
        target,
        std::vector<std::string>{"C:/audio/second.wav"});
    const auto secondResult = HarkenSearch::CubaseAdapter::ExecuteInsertRequest(secondRequest);
    check(secondResult.success,
          "Cubase persistent helper test second insert succeeds without killing the helper");
    check(secondResult.remoteSessionId == firstResult.remoteSessionId,
          "Cubase persistent helper keeps the same remote session across multiple inserts");

    const bool shutdownOk = HarkenSearch::CubaseAdapter::RequestBridgeShutdown(endpoint);
    check(shutdownOk,
          "Cubase persistent helper accepts a graceful shutdown request");
    check(WaitForBridgeToStop(endpoint),
          "Cubase persistent helper eventually stops after graceful shutdown");

    ClearBridgeStubEnv();
    KillBridgeHelperProcesses();
}

static void testCubaseDispatchPresentation() {
    HarkenSearch::CubaseAdapter::DispatchResult accepted;
    accepted.success = true;
    accepted.code = HarkenSearch::CubaseAdapter::ResultCode::Success;
    accepted.trackResolution = HarkenSearch::CubaseAdapter::TrackResolutionKind::SelectedTrackAccepted;
    accepted.projectName = "Demo Project";
    accepted.selectedTrackName = "FX 01";
    accepted.acceptedTrack = "FX 01";
    accepted.cursorLabel = "Bar 9 Beat 3";
    accepted.remoteSessionId = "auro-session";
    accepted.insertedClipIds = {"clip-1"};
    accepted.status = "Cubase direct insert bridge accepted the request";
    const auto acceptedPresentation = HarkenSearch::CubaseAdapter::BuildDispatchPresentation(accepted);
    check(acceptedPresentation.successTone,
          "Cubase dispatch presentation treats accepted insert as success-tone UI");
    check(acceptedPresentation.summary == "Direct insert accepted",
          "Cubase dispatch presentation summarizes accepted insert cleanly");
    check(acceptedPresentation.detail.find("project=Demo Project") != std::string::npos &&
              acceptedPresentation.detail.find("selected=FX 01") != std::string::npos &&
              acceptedPresentation.detail.find("cursor=Bar 9 Beat 3") != std::string::npos &&
              acceptedPresentation.detail.find("clips=1") != std::string::npos,
          "Cubase dispatch presentation preserves structured host context for accepted insert");

    HarkenSearch::CubaseAdapter::DispatchResult partial;
    partial.success = true;
    partial.code = HarkenSearch::CubaseAdapter::ResultCode::PartialSuccess;
    partial.trackResolution = HarkenSearch::CubaseAdapter::TrackResolutionKind::RemappedTrack;
    partial.projectName = "Demo Project";
    partial.selectedTrackName = "FX 01";
    partial.acceptedTrack = "FX ALT";
    partial.cursorLabel = "Bar 9 Beat 3";
    partial.status = "partial success";
    const auto partialPresentation = HarkenSearch::CubaseAdapter::BuildDispatchPresentation(partial);
    check(partialPresentation.successTone,
          "Cubase dispatch presentation keeps partial insert on the success-tone path");
    check(partialPresentation.summary == "Partial insert via remapped track on FX ALT",
          "Cubase dispatch presentation calls out remapped-track partial success");
    check(partialPresentation.detail.find("accepted=FX ALT") != std::string::npos,
          "Cubase dispatch presentation preserves remapped accepted-track detail");

    HarkenSearch::CubaseAdapter::DispatchResult snapshotDrift;
    snapshotDrift.code = HarkenSearch::CubaseAdapter::ResultCode::BridgeNotReady;
    snapshotDrift.addressingDrift = HarkenSearch::CubaseAdapter::AddressingDriftKind::SnapshotMismatch;
    snapshotDrift.projectName = "Demo Project";
    snapshotDrift.selectedTrackName = "FX 01";
    snapshotDrift.cursorLabel = "Bar 9 Beat 3";
    snapshotDrift.status = "Cubase bridge returned: snapshot-mismatch";
    const auto snapshotPresentation = HarkenSearch::CubaseAdapter::BuildDispatchPresentation(snapshotDrift);
    check(!snapshotPresentation.successTone,
          "Cubase dispatch presentation does not treat snapshot drift as success-tone UI");
    check(snapshotPresentation.summary == "Snapshot drift",
          "Cubase dispatch presentation summarizes snapshot drift explicitly");
    check(snapshotPresentation.detail.find("snapshot-mismatch") != std::string::npos,
          "Cubase dispatch presentation preserves raw drift failure detail");

    HarkenSearch::CubaseAdapter::DispatchResult selectedTrackChanged;
    selectedTrackChanged.code = HarkenSearch::CubaseAdapter::ResultCode::TrackUnavailable;
    selectedTrackChanged.trackResolution = HarkenSearch::CubaseAdapter::TrackResolutionKind::SelectedTrackChanged;
    selectedTrackChanged.selectedTrackName = "FX 01";
    selectedTrackChanged.status = "Cubase bridge returned: selected-track-changed";
    const auto selectedTrackChangedPresentation =
        HarkenSearch::CubaseAdapter::BuildDispatchPresentation(selectedTrackChanged);
    check(selectedTrackChangedPresentation.summary == "Selected track changed",
          "Cubase dispatch presentation summarizes selected-track drift explicitly");

    HarkenSearch::CubaseAdapter::DispatchResult stubOnly;
    stubOnly.code = HarkenSearch::CubaseAdapter::ResultCode::BridgeNotImplemented;
    stubOnly.status = "Nuendo direct insert is still using a protocol stub helper";
    const auto stubOnlyPresentation =
        HarkenSearch::CubaseAdapter::BuildDispatchPresentation(stubOnly);
    check(!stubOnlyPresentation.successTone,
          "Dispatch presentation does not treat stub-only helper state as success-tone UI");
    check(stubOnlyPresentation.summary == "Direct insert not implemented",
          "Dispatch presentation summarizes stub-only helper state explicitly");
}

int main() {
    char focused[32] = {};
    if (GetEnvironmentVariableA("AURO_TEST_CUBASE_CASE", focused, sizeof(focused)) > 0 &&
        std::string(focused) == "pipe_client") {
        CubaseExecutablePathStub::Reset(CurrentProcessPathUtf8());
        testCubasePipeClientTransportFaultMatrix();
        return failed == 0 ? 0 : 1;
    }
    if (std::string(focused) == "process_launch") {
        testCubaseNativeProcessLaunchOwnershipMatrix();
        return failed == 0 ? 0 : 1;
    }
    KillBridgeHelperProcesses();
    CubaseExecutablePathStub::Reset(CurrentProcessPathUtf8());
    testCubaseCurrentProcessExecutablePathStatusMatrix();
    testCubaseBridgeExecutablePathExistenceContract();
    testCubasePreflightRequiresAdapter();
    testNuendoPreflightFindsBundledAdapter();
    testCubasePreflightNeedsConfig();
    testCubasePreflightHostLaunchRequiredWhenAdapterInstalled();
    testCubasePreflightBridgeOnlineButHostNotReady();
    testCubaseAttachReadyButHostNotReady();
    testCubaseDispatchSeamFailsHonestly();
    testCubaseInsertRequestContract();
    testNuendoInsertRequestCanBeBuiltFromSharedHostSendJob();
    testHostSendDefaultStagingRootPrefersLocalAppData();
    testHostSendDefaultStagingRootFallsBackToTemporaryDirectory();
    testHostSendExplicitStagingRootSkipsDefaults();
    testCubaseExecuteInsertRequestModelsFallback();
    testCubaseBridgeEndpointContract();
    testNuendoBridgeEndpointAndBinaryContract();
    testCubaseBridgeExecutableResolutionAndLaunchArguments();
    testCubaseBridgeLaunchPlanLaunchThenAttach();
    testCubasePipeClientTransportFaultMatrix();
    testCubaseEndpointWaitAndAttachPreparation();
    testCubaseBridgeLaunchAttemptExecutor();
    testCubaseBridgeLaunchAttemptRealSpawnFailureSurface();
    testCubaseNativeProcessLaunchOwnershipMatrix();
    testCubaseBridgeLaunchPathConversionFailsClosed();
    testCubaseBridgeResponseSchema();
    testCubaseBridgeProbeAndExecuteReadyishPath();
    testCubaseMockBridgeServerRoundTrip();
    testCubaseBridgeProcessContractLaunchPath();
    testCubaseMockBridgeErrorMatrix();
    testCubaseExecuteFallsBackToAttachSessionId();
    testBridgeHelpersPreserveUnicodeHandshakeArgv();
    testCubaseRealHelperSpawnRoundTrip();
    testNuendoRealHelperSpawnRoundTrip();
    testNuendoBundledHelperAutoDetectedWithoutEnv();
    testCubaseRealHelperStatefulAckAndFailures();
    testCubaseRealHelperPersistentSessionReuseAndShutdown();
    testCubaseDispatchPresentation();

    if (failed != 0) {
        KillBridgeHelperProcesses();
        std::cerr << "Total failures: " << failed << "\n";
        return 1;
    }
    KillBridgeHelperProcesses();
    std::cout << "All Steinberg direct insert contract tests passed\n";
    return 0;
}
