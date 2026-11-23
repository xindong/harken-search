#include "ExternalAiBridgeServer.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

#ifdef _WIN32
#include <windows.h>
#endif

namespace HarkenSearch {

namespace {

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

constexpr DWORD kMaxRequestBytes = 64 * 1024;
constexpr DWORD kClientIoTimeoutMs = 5000;
constexpr DWORD kResponseTimeoutMs = 15000;
constexpr DWORD kShutdownWaitMs = 100;

class OwnedHandle {
public:
    explicit OwnedHandle(HANDLE handle) : handle_(handle) {}
    OwnedHandle(const OwnedHandle&) = delete;
    OwnedHandle& operator=(const OwnedHandle&) = delete;
    ~OwnedHandle() { if (*this) CloseHandle(handle_); }
    explicit operator bool() const { return handle_ && handle_ != INVALID_HANDLE_VALUE; }
    HANDLE get() const { return handle_; }

private:
    HANDLE handle_;
};

struct PendingRequest {
    std::string request;
    std::string response;
    bool answered = false;
    bool inProgress = false;
};

struct ServerState {
    explicit ServerState(const std::string& id)
        : libraryId(id), stop(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {}

    bool Stopping() const { return WaitForSingleObject(stop.get(), 0) == WAIT_OBJECT_0; }

    const std::string libraryId;
    const OwnedHandle stop;
    std::atomic<bool> finished{false};
    std::mutex mutex;
    std::condition_variable answered;
    std::shared_ptr<PendingRequest> pending;
};

struct ServerOwner {
    ~ServerOwner() { Stop(); }

    void Stop()
    {
        const auto previous = std::move(active);
        if (!previous) return;
        {
            std::lock_guard<std::mutex> lock(previous->mutex);
            SetEvent(previous->stop.get());
            previous->pending.reset();
        }
        previous->answered.notify_all();
        if (WaitForSingleObject(worker.native_handle(), kShutdownWaitMs) == WAIT_OBJECT_0) {
            worker.join();
        } else {
            worker.detach();
        }
    }

    std::shared_ptr<ServerState> active;
    std::thread worker;
};

ServerOwner& Owner()
{
    static ServerOwner owner;
    return owner;
}

class PipeIo {
public:
    PipeIo(HANDLE pipe, const ServerState& state)
        : pipe_(pipe), state_(state), completed_(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {}

    bool Connect()
    {
        DWORD transferred = 0;
        return Run(INFINITE, transferred, [&](OVERLAPPED& operation) {
            return ConnectNamedPipe(pipe_, &operation) || GetLastError() == ERROR_PIPE_CONNECTED;
        });
    }

    bool Read(char* buffer, DWORD size, DWORD timeout, DWORD& read)
    {
        return Run(timeout, read, [&](OVERLAPPED& operation) {
            return ReadFile(pipe_, buffer, size, &read, &operation);
        });
    }

    bool Write(const std::string& response)
    {
        DWORD written = 0;
        return Run(kClientIoTimeoutMs, written, [&](OVERLAPPED& operation) {
            return WriteFile(pipe_, response.data(), static_cast<DWORD>(response.size()), &written, &operation);
        }) && written == response.size();
    }

    void WaitForPeerClose()
    {
        char unexpectedByte = 0;
        DWORD read = 0;
        Read(&unexpectedByte, 1, kClientIoTimeoutMs, read);
    }

private:
    template <typename Begin>
    bool Run(DWORD timeout, DWORD& transferred, Begin begin)
    {
        if (!completed_ || state_.Stopping()) return false;
        ResetEvent(completed_.get());
        OVERLAPPED operation{};
        operation.hEvent = completed_.get();
        if (begin(operation)) return true;
        if (GetLastError() != ERROR_IO_PENDING) return false;

        const HANDLE events[] = {state_.stop.get(), completed_.get()};
        if (WaitForMultipleObjects(2, events, FALSE, timeout) == WAIT_OBJECT_0 + 1) {
            return GetOverlappedResult(pipe_, &operation, &transferred, FALSE) != FALSE;
        }
        CancelIoEx(pipe_, &operation);
        GetOverlappedResult(pipe_, &operation, &transferred, TRUE);
        return false;
    }

    HANDLE pipe_;
    const ServerState& state_;
    const OwnedHandle completed_;
};

bool ReadRequestLine(PipeIo& io, std::string& request)
{
    const ULONGLONG deadline = GetTickCount64() + kClientIoTimeoutMs;
    char buffer[4096];
    while (request.size() < kMaxRequestBytes) {
        const auto now = GetTickCount64();
        if (now >= deadline) return false;
        DWORD read = 0;
        const auto count = (std::min)(sizeof(buffer), kMaxRequestBytes - request.size());
        if (!io.Read(buffer, static_cast<DWORD>(count), static_cast<DWORD>(deadline - now), read) || read == 0) {
            return false;
        }
        request.append(buffer, read);
        const auto newline = request.find('\n');
        if (newline != std::string::npos) {
            request.resize(newline);
            return !request.empty();
        }
    }
    return false;
}

std::string WaitForReply(ServerState& state, const std::shared_ptr<PendingRequest>& request)
{
    std::unique_lock<std::mutex> lock(state.mutex);
    if (state.Stopping()) return {};
    state.pending = request;
    state.answered.wait_for(lock, std::chrono::milliseconds(kResponseTimeoutMs),
        [&] { return state.pending != request || request->answered; });
    if (state.pending != request) return {};
    state.pending.reset();
    return request->answered ? std::move(request->response)
                             : R"({"status":"failed","reason":"bridge_timeout"})";
}

void WorkerLoop(ServerState& state)
{
    const auto pipeName = ExternalAiBridgePipeName(state.libraryId);
    const std::wstring name(pipeName.begin(), pipeName.end());
    const OwnedHandle pipe(CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1, kMaxRequestBytes, kMaxRequestBytes, 0, nullptr));
    if (!pipe || state.Stopping()) return;
    PipeIo io(pipe.get(), state);
    while (!state.Stopping()) {
        if (!io.Connect()) return;
        auto request = std::make_shared<PendingRequest>();
        if (ReadRequestLine(io, request->request)) {
            auto response = WaitForReply(state, request);
            if (!state.Stopping()) {
                response.push_back('\n');
                if (io.Write(response)) {
                    io.WaitForPeerClose();
                }
            }
        }
        if (state.Stopping()) return;
        DisconnectNamedPipe(pipe.get());
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
#ifdef _WIN32
    auto& owner = Owner();
    if (owner.active && owner.active->libraryId == libraryId && !owner.active->finished.load()) return;
    owner.Stop();
    if (libraryId.empty()) {
        return;
    }
    auto state = std::make_shared<ServerState>(libraryId);
    if (!state->stop) return;
    owner.worker = std::thread([state] {
        WorkerLoop(*state);
        state->finished.store(true);
    });
    owner.active = std::move(state);
#else
    (void)libraryId;
#endif
}

void StopExternalAiBridgeServer()
{
#ifdef _WIN32
    Owner().Stop();
#endif
}

bool ExternalAiBridgeServerRunning()
{
#ifdef _WIN32
    const auto& active = Owner().active;
    return active && !active->finished.load();
#else
    return false;
#endif
}

void DrainExternalAiBridgeServer(const std::function<std::string(const std::string&)>& handler)
{
#ifdef _WIN32
    const auto state = Owner().active;
    if (!state) return;
    std::shared_ptr<PendingRequest> request;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (!state->pending || state->pending->answered || state->pending->inProgress) {
            return;
        }
        request = state->pending;
        request->inProgress = true;
    }
    std::string response = handler ? handler(request->request) : std::string("{}");
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (state->pending == request) {
            request->response = std::move(response);
            request->answered = true;
            state->answered.notify_all();
        }
    }
#else
    (void)handler;
#endif
}

}  // namespace HarkenSearch
