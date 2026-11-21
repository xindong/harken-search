#include "ExternalAiBridgeServer.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#endif

namespace HarkenSearch {

namespace {

constexpr std::size_t kMaxRequestBytes = 64 * 1024;
constexpr int kResponseTimeoutMs = 15000;

struct PendingRequest {
    std::string request;
    std::string response;
    bool answered = false;
};

struct ServerState {
    std::thread worker;
    std::atomic<bool> stop{false};
    std::string libraryId;
    std::string pipeName;
    bool running = false;

    std::mutex mutex;
    std::condition_variable answered;
    std::condition_variable queued;
    std::unique_ptr<PendingRequest> pending;

#ifdef _WIN32
    HANDLE handle = INVALID_HANDLE_VALUE;
#endif
};

ServerState& State()
{
    static ServerState state;
    return state;
}

std::string HashLibraryId(const std::string& libraryId)
{
    // FNV-1a, rendered as 16 hex digits: stable pipe name per library, no path leak.
    uint64_t value = 1469598103934665603ull;
    for (const char unit : libraryId) {
        value ^= static_cast<unsigned char>(unit);
        value *= 1099511628211ull;
    }
    static const char* hex = "0123456789abcdef";
    std::string out(16, '0');
    for (int index = 15; index >= 0; --index) {
        out[static_cast<std::size_t>(index)] = hex[value & 0x0F];
        value >>= 4;
    }
    return out;
}

#ifdef _WIN32

std::wstring WidePipeName(const std::string& pipeName)
{
    return std::wstring(pipeName.begin(), pipeName.end());
}

bool ReadRequestLine(HANDLE handle, std::string& out)
{
    out.clear();
    char buffer[4096];
    while (out.size() < kMaxRequestBytes) {
        DWORD read = 0;
        if (!ReadFile(handle, buffer, sizeof(buffer), &read, nullptr) || read == 0) {
            return false;
        }
        out.append(buffer, read);
        const auto newline = out.find('\n');
        if (newline != std::string::npos) {
            out.resize(newline);
            return true;
        }
    }
    return false;
}

void WorkerLoop(ServerState& state)
{
    const std::wstring name = WidePipeName(state.pipeName);
    while (!state.stop.load()) {
        HANDLE pipe = CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1, kMaxRequestBytes,
            kMaxRequestBytes, 0, nullptr);
        if (pipe == INVALID_HANDLE_VALUE) {
            return;
        }
        {
            std::lock_guard<std::mutex> lock(state.mutex);
            state.handle = pipe;
        }
        const BOOL connected = ConnectNamedPipe(pipe, nullptr) ? TRUE : (GetLastError() == ERROR_PIPE_CONNECTED);
        if (!connected || state.stop.load()) {
            DisconnectNamedPipe(pipe);
            CloseHandle(pipe);
            continue;
        }

        std::string request;
        if (ReadRequestLine(pipe, request) && !request.empty()) {
            std::unique_lock<std::mutex> lock(state.mutex);
            state.pending = std::make_unique<PendingRequest>();
            state.pending->request = std::move(request);
            state.queued.notify_all();
            state.answered.wait_for(lock, std::chrono::milliseconds(kResponseTimeoutMs),
                [&state] { return !state.pending || state.pending->answered || state.stop.load(); });
            std::string response;
            if (state.pending && state.pending->answered) {
                response = state.pending->response;
            } else {
                response = R"({"status":"failed","reason":"bridge_timeout"})";
            }
            state.pending.reset();
            lock.unlock();
            response.push_back('\n');
            DWORD written = 0;
            WriteFile(pipe, response.data(), static_cast<DWORD>(response.size()), &written, nullptr);
        }
        FlushFileBuffers(pipe);
        DisconnectNamedPipe(pipe);
        CloseHandle(pipe);
        {
            std::lock_guard<std::mutex> lock(state.mutex);
            state.handle = INVALID_HANDLE_VALUE;
        }
    }
}

#endif  // _WIN32

}  // namespace

std::string ExternalAiBridgePipeName(const std::string& libraryId)
{
    return "\\\\.\\pipe\\harken-external-ai-" + HashLibraryId(libraryId);
}

void StartExternalAiBridgeServer(const std::string& libraryId)
{
    if (libraryId.empty()) {
        StopExternalAiBridgeServer();
        return;
    }
    auto& state = State();
    if (state.running && state.libraryId == libraryId) {
        return;
    }
    StopExternalAiBridgeServer();
#ifdef _WIN32
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        state.stop.store(false);
        state.libraryId = libraryId;
        state.pipeName = ExternalAiBridgePipeName(libraryId);
        state.running = true;
    }
    state.worker = std::thread(WorkerLoop, std::ref(state));
#endif
}

void StopExternalAiBridgeServer()
{
    auto& state = State();
    if (!state.running) {
        return;
    }
#ifdef _WIN32
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        state.stop.store(true);
        if (state.handle != INVALID_HANDLE_VALUE) {
            // Cancels a blocking ConnectNamedPipe / ReadFile so the thread can exit.
            CancelIoEx(state.handle, nullptr);
            CloseHandle(state.handle);
            state.handle = INVALID_HANDLE_VALUE;
        }
        state.answered.notify_all();
        state.queued.notify_all();
    }
    if (state.worker.joinable()) {
        state.worker.join();
    }
#endif
    std::lock_guard<std::mutex> lock(state.mutex);
    state.running = false;
    state.pending.reset();
}

bool ExternalAiBridgeServerRunning()
{
    return State().running;
}

void DrainExternalAiBridgeServer(const std::function<std::string(const std::string&)>& handler)
{
    auto& state = State();
    std::unique_ptr<PendingRequest> request;
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        if (!state.pending || state.pending->answered) {
            return;
        }
        request = std::move(state.pending);
    }
    const std::string response = handler ? handler(request->request) : std::string("{}");
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        // The worker may have timed out while the handler ran; only answer a live one.
        if (state.pending) {
            state.pending->response = response;
            state.pending->answered = true;
            state.answered.notify_all();
        }
    }
}

}  // namespace HarkenSearch
