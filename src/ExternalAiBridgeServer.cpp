#include "ExternalAiBridgeServer.h"

#include <algorithm>
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
// A whole request line must arrive inside this window.
constexpr int kRequestTimeoutMs = 5000;

struct PendingRequest {
    std::string request;
    std::string response;
    bool answered = false;
    bool inProgress = false;
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

void MarkWorkerStopped(ServerState& state)
{
    std::lock_guard<std::mutex> lock(state.mutex);
    state.running = false;
#ifdef _WIN32
    state.handle = INVALID_HANDLE_VALUE;
#endif
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

// Connect waits a long time, but in 100 ms slices so a stop request is noticed.
constexpr DWORD kConnectWaitMs = 24u * 60u * 60u * 1000u;
constexpr DWORD kResponseDrainTimeoutMs = 1000;

std::wstring WidePipeName(const std::string& pipeName)
{
    return std::wstring(pipeName.begin(), pipeName.end());
}

// Waits for one overlapped operation in short slices so the worker keeps
// watching the stop flag. Returns true only when the operation completed.
bool AwaitPipeIo(HANDLE pipe, OVERLAPPED& overlapped, DWORD timeoutMs,
    const std::atomic<bool>& stop, DWORD& transferred)
{
    const ULONGLONG deadline = GetTickCount64() + timeoutMs;
    for (;;) {
        const ULONGLONG now = GetTickCount64();
        if (stop.load() || now >= deadline) {
            CancelIoEx(pipe, &overlapped);
            GetOverlappedResult(pipe, &overlapped, &transferred, TRUE);
            return false;
        }
        const DWORD slice = static_cast<DWORD>(
            std::min<ULONGLONG>(deadline - now, 100));
        const DWORD waited = WaitForSingleObject(overlapped.hEvent, slice);
        if (waited == WAIT_OBJECT_0) {
            return GetOverlappedResult(pipe, &overlapped, &transferred, FALSE) != FALSE;
        }
        if (waited != WAIT_TIMEOUT) {
            CancelIoEx(pipe, &overlapped);
            GetOverlappedResult(pipe, &overlapped, &transferred, TRUE);
            return false;
        }
    }
}

// The deadline covers the whole request line, not only its first bytes: a
// client that sends one byte and stalls must not own the only pipe instance.
bool ReadRequestLine(HANDLE pipe, HANDLE ioEvent, const std::atomic<bool>& stop, std::string& out)
{
    out.clear();
    const ULONGLONG deadline = GetTickCount64() + kRequestTimeoutMs;
    char buffer[4096];
    while (out.size() < kMaxRequestBytes) {
        const ULONGLONG now = GetTickCount64();
        if (stop.load() || now >= deadline) {
            return false;
        }
        OVERLAPPED overlapped{};
        overlapped.hEvent = ioEvent;
        ResetEvent(ioEvent);
        DWORD read = 0;
        if (!ReadFile(pipe, buffer, sizeof(buffer), &read, &overlapped)) {
            if (GetLastError() != ERROR_IO_PENDING) {
                return false;
            }
            const DWORD remaining = static_cast<DWORD>(
                std::min<ULONGLONG>(deadline - now, kRequestTimeoutMs));
            if (!AwaitPipeIo(pipe, overlapped, remaining, stop, read) || read == 0) {
                return false;
            }
        } else if (read == 0) {
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

bool WriteResponse(HANDLE pipe, HANDLE ioEvent, const std::atomic<bool>& stop, const std::string& payload)
{
    OVERLAPPED overlapped{};
    overlapped.hEvent = ioEvent;
    ResetEvent(ioEvent);
    DWORD written = 0;
    if (!WriteFile(pipe, payload.data(), static_cast<DWORD>(payload.size()), &written, &overlapped)) {
        if (GetLastError() != ERROR_IO_PENDING) {
            return false;
        }
        if (!AwaitPipeIo(pipe, overlapped, kResponseTimeoutMs, stop, written)) {
            return false;
        }
    }
    return written == payload.size();
}

// The client reads the response and closes its one-request connection. Wait
// briefly for that close so DisconnectNamedPipe does not discard unread bytes,
// while keeping shutdown bounded and cancellable.
void WaitForClientClose(HANDLE pipe, HANDLE ioEvent, const std::atomic<bool>& stop)
{
    OVERLAPPED overlapped{};
    overlapped.hEvent = ioEvent;
    ResetEvent(ioEvent);
    char discarded = 0;
    DWORD read = 0;
    if (ReadFile(pipe, &discarded, 1, &read, &overlapped)) {
        return;
    }
    if (GetLastError() != ERROR_IO_PENDING) {
        return;
    }
    AwaitPipeIo(pipe, overlapped, kResponseDrainTimeoutMs, stop, read);
}

// The worker is the only owner of the pipe handle: the stop path never closes
// it, so a cancelled wait cannot close a handle Windows already reused.
void WorkerLoop(ServerState& state)
{
    const std::wstring name = WidePipeName(state.pipeName);
    HANDLE ioEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ioEvent) {
        MarkWorkerStopped(state);
        return;
    }
    while (!state.stop.load()) {
        HANDLE pipe = CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1, kMaxRequestBytes,
            kMaxRequestBytes, 0, nullptr);
        if (pipe == INVALID_HANDLE_VALUE) {
            break;
        }
        {
            std::lock_guard<std::mutex> lock(state.mutex);
            state.handle = pipe;
        }

        bool connected = false;
        {
            OVERLAPPED overlapped{};
            overlapped.hEvent = ioEvent;
            ResetEvent(ioEvent);
            DWORD dummy = 0;
            if (ConnectNamedPipe(pipe, &overlapped)) {
                connected = true;
            } else if (GetLastError() == ERROR_PIPE_CONNECTED) {
                connected = true;
            } else if (GetLastError() == ERROR_IO_PENDING) {
                connected = AwaitPipeIo(pipe, overlapped, kConnectWaitMs, state.stop, dummy);
            }
        }

        if (connected && !state.stop.load()) {
            std::string request;
            const bool gotRequest = ReadRequestLine(pipe, ioEvent, state.stop, request)
                && !request.empty();
            if (gotRequest) {
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
                if (WriteResponse(pipe, ioEvent, state.stop, response)) {
                    WaitForClientClose(pipe, ioEvent, state.stop);
                }
            }
        }
        DisconnectNamedPipe(pipe);
        {
            std::lock_guard<std::mutex> lock(state.mutex);
            if (state.handle == pipe) {
                state.handle = INVALID_HANDLE_VALUE;
            }
        }
        CloseHandle(pipe);
    }
    CloseHandle(ioEvent);
    MarkWorkerStopped(state);
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
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        if (state.running && state.libraryId == libraryId) {
            return;
        }
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
    try {
        state.worker = std::thread(WorkerLoop, std::ref(state));
    } catch (...) {
        std::lock_guard<std::mutex> lock(state.mutex);
        state.stop.store(true);
        state.running = false;
        state.pending.reset();
        throw;
    }
#endif
}

void StopExternalAiBridgeServer()
{
    auto& state = State();
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        state.stop.store(true);
        // The pipe handle belongs to the worker: closing it here would close a
        // handle the worker still uses, and Windows may already have reused it.
        // Signal both waits and let the join below finish the teardown.
        state.answered.notify_all();
        state.queued.notify_all();
    }
#ifdef _WIN32
    if (state.worker.joinable()) {
        state.worker.join();
    }
#endif
    std::lock_guard<std::mutex> lock(state.mutex);
    state.running = false;
    state.pending.reset();
#ifdef _WIN32
    state.handle = INVALID_HANDLE_VALUE;
#endif
}

bool ExternalAiBridgeServerRunning()
{
    auto& state = State();
    std::lock_guard<std::mutex> lock(state.mutex);
    return state.running;
}

void DrainExternalAiBridgeServer(const std::function<std::string(const std::string&)>& handler)
{
    auto& state = State();
    std::string request;
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        // Keep the slot owned by the worker: moving it out here used to leave the
        // response nowhere to go, so every request timed out.
        if (!state.pending || state.pending->answered || state.pending->inProgress) {
            return;
        }
        state.pending->inProgress = true;
        request = state.pending->request;
    }
    const std::string response = handler ? handler(request) : std::string("{}");
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        // The worker may have timed out while the handler ran; only answer a live one.
        if (state.pending && state.pending->inProgress && !state.pending->answered) {
            state.pending->response = response;
            state.pending->answered = true;
            state.answered.notify_all();
        }
    }
}

}  // namespace HarkenSearch
