#include "ExternalAiBridgeServer.h"

#include <chrono>
#include <iostream>
#include <string>
#include <thread>
#include <utility>

#ifdef _WIN32
#include <windows.h>

namespace {

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
using HarkenSearch::DrainExternalAiBridgeServer;
using HarkenSearch::ExternalAiBridgePipeName;
using HarkenSearch::ExternalAiBridgeServerRunning;
using HarkenSearch::StartExternalAiBridgeServer;
using HarkenSearch::StopExternalAiBridgeServer;

constexpr auto kWait = 2s;
constexpr auto kStopLimit = 250ms;

struct Client {
    HANDLE handle = INVALID_HANDLE_VALUE;
    ~Client() { if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle); }

    bool Send(const std::string& bytes) const {
        DWORD written = 0;
        return WriteFile(handle, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr)
            && written == bytes.size();
    }
};

bool Connect(const std::string& library, Client& client) {
    const auto narrow = ExternalAiBridgePipeName(library);
    const std::wstring name(narrow.begin(), narrow.end());
    const auto deadline = Clock::now() + kWait;
    do {
        client.handle = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (client.handle != INVALID_HANDLE_VALUE) return true;
        std::this_thread::sleep_for(1ms);
    } while (Clock::now() < deadline);
    return false;
}

bool DrainUntilHandled(std::string response = "{\"status\":\"ok\"}") {
    bool handled = false;
    const auto deadline = Clock::now() + kWait;
    do {
        DrainExternalAiBridgeServer([&](const std::string& request) {
            handled = request == "{}";
            return std::move(response);
        });
        if (handled) return true;
        std::this_thread::sleep_for(1ms);
    } while (Clock::now() < deadline);
    return false;
}

bool AwaitReply(const Client& client, bool consume) {
    const auto deadline = Clock::now() + kWait;
    do {
        DWORD available = 0;
        if (!PeekNamedPipe(client.handle, nullptr, 0, nullptr, &available, nullptr)) return false;
        if (available > 0) {
            if (!consume) return true;
            char reply[128]{};
            DWORD read = 0;
            return ReadFile(client.handle, reply, sizeof(reply), &read, nullptr) &&
                std::string(reply, read) == "{\"status\":\"ok\"}\n";
        }
        std::this_thread::sleep_for(1ms);
    } while (Clock::now() < deadline);
    return false;
}

bool StopWithinBudget() {
    const auto started = Clock::now();
    StopExternalAiBridgeServer();
    const auto elapsed = Clock::now() - started;
    std::cout << "stop_us=" << std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count() << '\n';
    return elapsed < kStopLimit && !ExternalAiBridgeServerRunning();
}

bool Run(const std::string& scenario, const std::string& library) {
    if (scenario == "startup") {
        auto slowest = Clock::duration::zero();
        for (int iteration = 0; iteration < 200; ++iteration) {
            StartExternalAiBridgeServer(library);
            if (iteration % 2 == 0) SwitchToThread();
            const auto started = Clock::now();
            StopExternalAiBridgeServer();
            const auto elapsed = Clock::now() - started;
            if (elapsed > slowest) slowest = elapsed;
            if (elapsed >= kStopLimit || ExternalAiBridgeServerRunning()) return false;
        }
        std::cout << "iterations=200 max_stop_us="
                  << std::chrono::duration_cast<std::chrono::microseconds>(slowest).count() << '\n';
        return true;
    }
    StartExternalAiBridgeServer(library);
    if (scenario == "reuse") {
        for (int iteration = 0; iteration < 20; ++iteration) {
            Client peer;
            if (!Connect(library, peer) || !peer.Send("{}\n") || !DrainUntilHandled() ||
                !AwaitReply(peer, true)) return false;
        }
        return StopWithinBudget();
    }
    if (scenario == "listening") {
        const auto narrow = ExternalAiBridgePipeName(library);
        const std::wstring name(narrow.begin(), narrow.end());
        const auto deadline = Clock::now() + kWait;
        do {
            if (WaitNamedPipeW(name.c_str(), 0)) return StopWithinBudget();
            std::this_thread::sleep_for(1ms);
        } while (Clock::now() < deadline);
        return false;
    }
    Client client;
    if (!Connect(library, client)) return false;
    if (scenario == "silent") return StopWithinBudget();
    if (scenario == "partial") {
        if (!client.Send("{")) return false;
        std::this_thread::sleep_for(200ms);
        return StopWithinBudget();
    }
    if (!client.Send("{}\n")) return false;
    if (scenario == "queued") {
        std::this_thread::sleep_for(150ms);
        if (!StopWithinBudget()) return false;
        StartExternalAiBridgeServer(library);
        Client nextClient;
        if (!Connect(library, nextClient)) return false;
        bool late = false;
        DrainExternalAiBridgeServer([&late](const std::string&) { late = true; return std::string{}; });
        return !late && nextClient.Send("{}\n") && DrainUntilHandled() &&
            AwaitReply(nextClient, true) && StopWithinBudget();
    }
    if (scenario == "reply" || scenario == "unread") {
        return DrainUntilHandled() && AwaitReply(client, scenario == "reply") && StopWithinBudget();
    }
    if (scenario == "backpressure") {
        return DrainUntilHandled(std::string(1024 * 1024, 'x')) && AwaitReply(client, false) &&
            StopWithinBudget();
    }
    return false;
}

}
#endif

int main(int argc, char** argv) {
#ifdef _WIN32
    if (argc != 2) return 2;
    const std::string scenario = argv[1];
    const std::string library = "bridge-shutdown-fixture-" + std::to_string(GetCurrentProcessId()) + "-" +
        std::to_string(GetTickCount64());
    std::cout << "scenario=" << scenario << std::endl;
    const bool passed = Run(scenario, library);
    StopExternalAiBridgeServer();
    std::cout << (passed ? "PASS " : "FAIL ") << scenario << '\n';
    return passed ? 0 : 1;
#else
    (void)argc;
    (void)argv;
    return 0;
#endif
}
