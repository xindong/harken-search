#ifdef __APPLE__

#include "named_pipe.h"
#include "reaper_bridge_endpoint_macos.h"
#include "ReaperBridgeProtocol.h"
#include "text_encoding.h"

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>

#include <algorithm>
#include <chrono>
#include <cerrno>
#include <cstring>
#include <string>

namespace Platform::NamedPipe {
namespace {

constexpr std::size_t kMaxExchangeBytes = 1024 * 1024;
using Clock = std::chrono::steady_clock;

struct ScopedFd {
    int value = -1;
    explicit ScopedFd(int fd) : value(fd) {}
    ~ScopedFd() { if (value >= 0) close(value); }
    ScopedFd(const ScopedFd&) = delete;
    ScopedFd& operator=(const ScopedFd&) = delete;
};

bool WaitFor(int fd, short events, Clock::time_point deadline,
             MessageExchangeStatus failure, MessageExchangeResult& result) {
    for (;;) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - Clock::now()).count();
        if (left <= 0) {
            result.status = MessageExchangeStatus::TimedOut;
            result.systemError = ETIMEDOUT;
            return false;
        }
        pollfd ready{fd, events, 0};
        const int polled = poll(&ready, 1, static_cast<int>((std::min)(left, 1000LL)));
        if (polled > 0) return true;
        if (polled == 0 || errno == EINTR) continue;
        result.status = failure;
        result.systemError = errno;
        return false;
    }
}

bool ExplicitSocketPath(std::wstring_view name, std::string& path) {
    if (name == HarkenSearch::ReaperBridge::kDefaultPipeName) {
        path = HarkenSearch::ReaperBridge::MacSelectedSocketPath();
        return !path.empty();
    }
    if (name.empty() || name.front() != L'/') return false;
    path = Platform::TextEncoding::WideToUtf8Strict(name);
    if (path.empty() || path.find('\n') != std::string::npos ||
        path.find('\r') != std::string::npos) return false;
    sockaddr_un address{};
    return path.size() < sizeof(address.sun_path);
}

} // namespace

MessageExchangeResult ExchangeMessage(std::wstring_view name,
                                      std::string_view request,
                                      uint32_t timeoutMilliseconds) {
    MessageExchangeResult result;
    std::string path;
    if (!ExplicitSocketPath(name, path)) return result;
    if (request.empty() || request.size() > kMaxExchangeBytes ||
        request.back() != '\n') {
        result.status = MessageExchangeStatus::WriteFailed;
        result.systemError = EINVAL;
        return result;
    }
    if (timeoutMilliseconds == 0) {
        result.status = MessageExchangeStatus::TimedOut;
        result.systemError = ETIMEDOUT;
        return result;
    }

    struct stat endpoint{};
    if (lstat(path.c_str(), &endpoint) != 0) {
        result.status = MessageExchangeStatus::ConnectFailed;
        result.systemError = errno;
        return result;
    }
    if (!S_ISSOCK(endpoint.st_mode) || endpoint.st_uid != geteuid() ||
        (endpoint.st_mode & 0077) != 0) {
        result.status = MessageExchangeStatus::ConnectFailed;
        result.systemError = EACCES;
        return result;
    }

    ScopedFd socketFd(socket(AF_UNIX, SOCK_STREAM, 0));
    if (socketFd.value < 0) {
        result.status = MessageExchangeStatus::ConnectFailed;
        result.systemError = errno;
        return result;
    }
    const int noSigpipe = 1;
    if (setsockopt(socketFd.value, SOL_SOCKET, SO_NOSIGPIPE,
                   &noSigpipe, sizeof(noSigpipe)) != 0 ||
        fcntl(socketFd.value, F_SETFL,
              fcntl(socketFd.value, F_GETFL, 0) | O_NONBLOCK) != 0) {
        result.status = MessageExchangeStatus::ConnectFailed;
        result.systemError = errno;
        return result;
    }

    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeoutMilliseconds);
    if (connect(socketFd.value, reinterpret_cast<sockaddr*>(&address),
                sizeof(address)) != 0) {
        if (errno != EINPROGRESS && errno != EAGAIN) {
            result.status = MessageExchangeStatus::ConnectFailed;
            result.systemError = errno;
            return result;
        }
        if (!WaitFor(socketFd.value, POLLOUT, deadline,
                     MessageExchangeStatus::ConnectFailed, result)) return result;
        int error = 0;
        socklen_t errorSize = sizeof(error);
        if (getsockopt(socketFd.value, SOL_SOCKET, SO_ERROR, &error, &errorSize) != 0)
            error = errno;
        if (error != 0) {
            result.status = MessageExchangeStatus::ConnectFailed;
            result.systemError = error;
            return result;
        }
    }
    uid_t peerUser = 0;
    gid_t peerGroup = 0;
    if (getpeereid(socketFd.value, &peerUser, &peerGroup) != 0 ||
        peerUser != geteuid()) {
        result.status = MessageExchangeStatus::ConnectFailed;
        result.systemError = EACCES;
        return result;
    }

    std::size_t written = 0;
    while (written < request.size()) {
        if (!WaitFor(socketFd.value, POLLOUT, deadline,
                     MessageExchangeStatus::WriteFailed, result)) return result;
        const ssize_t count = send(socketFd.value, request.data() + written,
                                   request.size() - written, 0);
        if (count > 0) {
            written += static_cast<std::size_t>(count);
        } else if (count < 0 && (errno == EAGAIN || errno == EINTR)) {
            continue;
        } else {
            result.status = MessageExchangeStatus::WriteFailed;
            result.systemError = count < 0 ? errno : EPIPE;
            return result;
        }
    }

    char buffer[4096];
    for (;;) {
        if (!WaitFor(socketFd.value, POLLIN, deadline,
                     MessageExchangeStatus::ResponseWaitFailed, result)) return result;
        const ssize_t count = recv(socketFd.value, buffer, sizeof(buffer), 0);
        if (count > 0) {
            result.response.append(buffer, static_cast<std::size_t>(count));
            if (result.response.size() > kMaxExchangeBytes) {
                result.response.clear();
                result.status = MessageExchangeStatus::ReadFailed;
                result.systemError = EMSGSIZE;
                return result;
            }
            const std::size_t newline = result.response.find('\n');
            if (newline != std::string::npos) {
                result.response.resize(newline + 1);
                result.status = MessageExchangeStatus::Ok;
                return result;
            }
        } else if (count < 0 && (errno == EAGAIN || errno == EINTR)) {
            continue;
        } else {
            result.response.clear();
            result.status = MessageExchangeStatus::ReadFailed;
            result.systemError = count < 0 ? errno : ECONNRESET;
            return result;
        }
    }
}

MessageExchangeResult ExchangeMessageWithOptions(
    std::wstring_view, std::string_view, uint32_t,
    const MessageExchangeOptions&) {
    MessageExchangeResult result;
    result.status = MessageExchangeStatus::Unsupported;
    return result;
}

struct ServerSession::Impl {};

ServerSession::ServerSession() : impl_(std::make_unique<Impl>()) {}
ServerSession::~ServerSession() = default;
ServerSession::ServerSession(ServerSession&&) noexcept = default;
ServerSession& ServerSession::operator=(ServerSession&&) noexcept = default;

ServerOperationResult ServerSession::Open(std::wstring_view,
                                          uint32_t,
                                          uint32_t,
                                          uint32_t) {
    return {ServerSessionStatus::Unsupported};
}

ServerOperationResult ServerSession::Accept() {
    return {ServerSessionStatus::Unsupported};
}

ServerReadResult ServerSession::Read(uint32_t) {
    ServerReadResult result;
    result.status = ServerSessionStatus::Unsupported;
    return result;
}

ServerOperationResult ServerSession::WriteAndFlush(std::string_view) {
    return {ServerSessionStatus::Unsupported};
}

ServerOperationResult ServerSession::DisconnectAndClose() {
    return {ServerSessionStatus::Unsupported};
}

bool ServerSession::IsOpen() const noexcept {
    return false;
}

} // namespace Platform::NamedPipe

#endif // __APPLE__
