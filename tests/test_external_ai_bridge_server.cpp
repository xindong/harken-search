#include "ExternalAiBridgeServer.h"

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <atomic>
#include <functional>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

namespace {

using HarkenSearch::ExternalAiBridgePipeName;
using HarkenSearch::ExternalAiBridgeServerRunning;
using HarkenSearch::DrainExternalAiBridgeServer;
using HarkenSearch::StartExternalAiBridgeServer;
using HarkenSearch::StopExternalAiBridgeServer;

std::wstring PipeName(const std::string& libraryId)
{
    const auto name = ExternalAiBridgePipeName(libraryId);
    return std::wstring(name.begin(), name.end());
}

HANDLE OpenClient(const std::wstring& name, DWORD timeoutMs)
{
    const ULONGLONG deadline = GetTickCount64() + timeoutMs;
    for (;;) {
        HANDLE pipe = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
            OPEN_EXISTING, 0, nullptr);
        if (pipe != INVALID_HANDLE_VALUE) {
            return pipe;
        }
        if (GetTickCount64() >= deadline) {
            return INVALID_HANDLE_VALUE;
        }
        Sleep(10);
    }
}

bool WaitUntil(const std::function<bool()>& predicate, DWORD timeoutMs)
{
    const ULONGLONG deadline = GetTickCount64() + timeoutMs;
    while (GetTickCount64() < deadline) {
        if (predicate()) {
            return true;
        }
        Sleep(10);
    }
    return predicate();
}

int Fail(const char* message)
{
    std::cerr << "FAIL: " << message << "\n";
    StopExternalAiBridgeServer();
    return 1;
}

int RunUnreadClientStopChild()
{
    const std::string libraryId = "external-ai-bridge-unread-stop";
    const auto pipeName = PipeName(libraryId);
    StartExternalAiBridgeServer(libraryId);

    HANDLE client = OpenClient(pipeName, 2000);
    if (client == INVALID_HANDLE_VALUE) {
        return Fail("could not connect to bridge");
    }

    const std::string request = "{\"requestId\":\"unread-stop\"}\n";
    DWORD written = 0;
    if (!WriteFile(client, request.data(), static_cast<DWORD>(request.size()), &written, nullptr)
        || written != request.size()) {
        CloseHandle(client);
        return Fail("could not write bridge request");
    }

    std::atomic<bool> handled{false};
    for (int attempt = 0; attempt < 200 && !handled.load(); ++attempt) {
        DrainExternalAiBridgeServer([&handled](const std::string&) {
            handled.store(true);
            return std::string("{}");
        });
        Sleep(10);
    }
    if (!handled.load()) {
        CloseHandle(client);
        return Fail("bridge did not queue the request");
    }

    // Leave the response unread. The stop path must still finish promptly.
    Sleep(100);
    StopExternalAiBridgeServer();
    CloseHandle(client);
    return 0;
}

bool RunChildWithTimeout()
{
    wchar_t executable[32768] = {};
    const DWORD length = GetModuleFileNameW(nullptr, executable,
        static_cast<DWORD>(std::size(executable)));
    if (length == 0 || length >= std::size(executable)) {
        return false;
    }

    std::wstring command = L"\"";
    command.append(executable, length);
    command += L"\" --unread-client-stop";
    std::vector<wchar_t> mutableCommand(command.begin(), command.end());
    mutableCommand.push_back(L'\0');

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(nullptr, mutableCommand.data(), nullptr, nullptr, FALSE, 0,
            nullptr, nullptr, &startup, &process)) {
        return false;
    }

    const DWORD waitResult = WaitForSingleObject(process.hProcess, 3000);
    if (waitResult == WAIT_TIMEOUT) {
        TerminateProcess(process.hProcess, 1);
        WaitForSingleObject(process.hProcess, 1000);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        return false;
    }
    DWORD exitCode = 1;
    GetExitCodeProcess(process.hProcess, &exitCode);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return waitResult == WAIT_OBJECT_0 && exitCode == 0;
}

bool TestStopWithUnreadClient()
{
    if (!RunChildWithTimeout()) {
        std::cerr << "FAIL: stop did not finish with an unread client\n";
        return false;
    }
    std::cout << "PASS: stop with unread client\n";
    return true;
}

bool TestNormalResponse()
{
    const std::string libraryId = "external-ai-bridge-normal-response";
    StartExternalAiBridgeServer(libraryId);
    HANDLE client = OpenClient(PipeName(libraryId), 2000);
    if (client == INVALID_HANDLE_VALUE) {
        std::cerr << "FAIL: could not connect for normal response\n";
        StopExternalAiBridgeServer();
        return false;
    }

    const std::string request = "{\"requestId\":\"normal-response\"}\n";
    DWORD written = 0;
    if (!WriteFile(client, request.data(), static_cast<DWORD>(request.size()), &written, nullptr)
        || written != request.size()) {
        CloseHandle(client);
        StopExternalAiBridgeServer();
        std::cerr << "FAIL: could not write normal response request\n";
        return false;
    }

    bool handled = false;
    for (int attempt = 0; attempt < 200 && !handled; ++attempt) {
        DrainExternalAiBridgeServer([&handled](const std::string&) {
            handled = true;
            return std::string("{}");
        });
        Sleep(10);
    }
    char response[128] = {};
    DWORD read = 0;
    const bool received = handled
        && ReadFile(client, response, static_cast<DWORD>(sizeof(response) - 1), &read, nullptr)
        && std::string(response, read) == "{}\n";
    CloseHandle(client);
    StopExternalAiBridgeServer();
    if (!received) {
        std::cerr << "FAIL: normal response was not delivered\n";
        return false;
    }
    std::cout << "PASS: normal response\n";
    return true;
}

bool TestFailedWorkerCanBeRestarted()
{
    const std::string libraryId = "external-ai-bridge-create-failure";
    const auto pipeName = PipeName(libraryId);
    HANDLE blocker = CreateNamedPipeW(pipeName.c_str(), PIPE_ACCESS_DUPLEX,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1, 4096, 4096, 0, nullptr);
    if (blocker == INVALID_HANDLE_VALUE) {
        std::cerr << "FAIL: could not create pipe blocker\n";
        return false;
    }

    StartExternalAiBridgeServer(libraryId);
    const bool stopped = WaitUntil([] { return !ExternalAiBridgeServerRunning(); }, 2000);
    CloseHandle(blocker);
    if (!stopped) {
        std::cerr << "FAIL: failed worker remained running\n";
        StopExternalAiBridgeServer();
        return false;
    }

    // Start must join the exited worker before replacing its joinable thread.
    StartExternalAiBridgeServer(libraryId);
    HANDLE client = OpenClient(pipeName, 2000);
    if (client == INVALID_HANDLE_VALUE) {
        std::cerr << "FAIL: bridge did not recover after pipe creation failure\n";
        StopExternalAiBridgeServer();
        return false;
    }
    CloseHandle(client);
    StopExternalAiBridgeServer();
    if (ExternalAiBridgeServerRunning()) {
        std::cerr << "FAIL: bridge still reports running after stop\n";
        return false;
    }
    std::cout << "PASS: failed worker cleanup and restart\n";
    return true;
}

}  // namespace

int main(int argc, char** argv)
{
    if (argc > 1 && std::string(argv[1]) == "--unread-client-stop") {
        return RunUnreadClientStopChild();
    }
    if (argc > 1 && std::string(argv[1]) == "--stop-test") {
        return TestStopWithUnreadClient() ? 0 : 1;
    }
    if (argc > 1 && std::string(argv[1]) == "--normal-response") {
        return TestNormalResponse() ? 0 : 1;
    }
    if (argc > 1 && std::string(argv[1]) == "--failed-worker") {
        return TestFailedWorkerCanBeRestarted() ? 0 : 1;
    }
    return TestStopWithUnreadClient() && TestNormalResponse() && TestFailedWorkerCanBeRestarted() ? 0 : 1;
}

#else

int main()
{
    return 0;
}

#endif
