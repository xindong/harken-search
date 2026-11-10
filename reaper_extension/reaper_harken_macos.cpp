#include "reaper_plugin_macos.h"
#include "ReaperBridgeProtocol.h"

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>
#include <poll.h>

#include <atomic>
#include <cerrno>
#include <cstring>
#include <cstdlib>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {
namespace Protocol = HarkenSearch::ReaperBridge;

constexpr char kQaSocketVariable[] = "HARKEN_REAPER_BRIDGE_TEST_SOCKET";
constexpr const char* kRequiredReaperApis[] = {
    "InsertMedia", "GetCursorPosition", "SetEditCurPos", "CountMediaItems",
    "GetMediaItem", "GetMediaItemInfo_Value", "SetMediaItemInfo_Value",
    "GetMediaItemTrack", "DeleteTrackMediaItem", "CountTracks", "GetTrack",
    "DeleteTrack"};
std::atomic<bool> g_running{false};
std::thread g_server;
int g_listener = -1;
std::string g_endpoint;
dev_t g_endpointDevice = 0;
ino_t g_endpointInode = 0;

bool HasRequiredReaperApis(reaper_plugin_info_t* host) {
    for (const char* name : kRequiredReaperApis) {
        if (!host->GetFunc(name)) return false;
    }
    return true;
}

bool ValidEndpoint(const char* raw) {
    if (!raw || raw[0] != '/' || std::strchr(raw, '\n') ||
        std::strchr(raw, '\r')) return false;
    const std::string path(raw);
    sockaddr_un address{};
    if (path.size() >= sizeof(address.sun_path)) return false;
    const auto lastSlash = path.find_last_of('/');
    if (lastSlash == std::string::npos || lastSlash == path.size() - 1) return false;
    const std::string parent = lastSlash == 0 ? "/" : path.substr(0, lastSlash);
    struct stat directory{};
    if (lstat(parent.c_str(), &directory) != 0 ||
        !S_ISDIR(directory.st_mode) || directory.st_uid != geteuid() ||
        (directory.st_mode & 0077) != 0) return false;
    struct stat existing{};
    if (lstat(path.c_str(), &existing) == 0 || errno != ENOENT) return false;
    return true;
}

void RemoveOwnEndpoint() {
    if (g_endpoint.empty()) return;
    struct stat current{};
    if (lstat(g_endpoint.c_str(), &current) == 0 &&
        S_ISSOCK(current.st_mode) && current.st_uid == geteuid() &&
        current.st_dev == g_endpointDevice &&
        current.st_ino == g_endpointInode) {
        unlink(g_endpoint.c_str());
    }
    g_endpoint.clear();
    g_endpointDevice = 0;
    g_endpointInode = 0;
}

void SendResponse(int client, std::string_view response) {
    std::size_t offset = 0;
    while (offset < response.size()) {
        const ssize_t count = send(client, response.data() + offset,
                                   response.size() - offset, 0);
        if (count > 0) offset += static_cast<std::size_t>(count);
        else if (count < 0 && errno == EINTR) continue;
        else return;
    }
}

void HandleClient(int client) {
    const int noSigpipe = 1;
    timeval deadline{2, 0};
    if (setsockopt(client, SOL_SOCKET, SO_NOSIGPIPE, &noSigpipe,
                   sizeof(noSigpipe)) != 0 ||
        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &deadline,
                   sizeof(deadline)) != 0 ||
        setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &deadline,
                   sizeof(deadline)) != 0) return;

    std::string request;
    char buffer[4096];
    while (request.size() <= Protocol::kMaxProtocolMessageBytes) {
        const ssize_t count = recv(client, buffer, sizeof(buffer), 0);
        if (count > 0) {
            request.append(buffer, static_cast<std::size_t>(count));
            const auto newline = request.find('\n');
            if (newline != std::string::npos) {
                request.resize(newline + 1);
                break;
            }
        } else if (count < 0 && errno == EINTR) {
            continue;
        } else {
            return;
        }
    }
    if (request.empty() || request.back() != '\n' ||
        request.size() > Protocol::kMaxProtocolMessageBytes) {
        SendResponse(client, "ERR|PROTOCOL|message too large or incomplete\n");
        return;
    }

    std::string version;
    std::string architecture;
    std::string manifest;
    if (Protocol::ParseHelloRequest(request, version, architecture, manifest)) {
        if (version != std::to_string(Protocol::kProtocolVersion)) {
            SendResponse(client, "ERR|PROTOCOL|" +
                         std::to_string(Protocol::kProtocolVersion) + "\n");
        } else if (architecture != Protocol::CurrentArchitecture()) {
            SendResponse(client, "ERR|ARCH|" +
                         Protocol::CurrentArchitecture() + "\n");
        } else if (manifest != Protocol::kBridgeManifest) {
            SendResponse(client, "ERR|MANIFEST|" +
                         std::string(Protocol::kBridgeManifest) + "\n");
        } else {
            SendResponse(client,
                         "ERR|UNAVAILABLE|macOS REAPER host insertion is not implemented\n");
        }
        return;
    }
    if (Protocol::TrimProtocolLine(request) == "PING") {
        SendResponse(client,
                     "ERR|UNAVAILABLE|macOS REAPER host insertion is not implemented\n");
        return;
    }
    std::vector<std::string> paths;
    std::string error;
    if (Protocol::ParseInsertBatchRequest(request, paths, error)) {
        SendResponse(client, "ERR|INSERT|macOS REAPER host insertion is not implemented\n");
    } else {
        SendResponse(client, "ERR|PROTOCOL|" + error + "\n");
    }
}

void ServerLoop(int listener) {
    while (g_running.load()) {
        pollfd ready{listener, POLLIN, 0};
        const int selected = poll(&ready, 1, 100);
        if (selected <= 0) continue;
        const int client = accept(listener, nullptr, nullptr);
        if (client < 0) continue;
        HandleClient(client);
        close(client);
    }
}

void StopServer() {
    g_running.store(false);
    if (g_server.joinable()) g_server.join();
    if (g_listener >= 0) close(g_listener);
    g_listener = -1;
    RemoveOwnEndpoint();
}

bool StartServer(const char* rawPath) {
    if (g_server.joinable() || !ValidEndpoint(rawPath)) return false;
    g_listener = socket(AF_UNIX, SOCK_STREAM, 0);
    if (g_listener < 0) return false;
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, rawPath, std::strlen(rawPath) + 1);
    if (bind(g_listener, reinterpret_cast<sockaddr*>(&address),
             sizeof(address)) != 0) {
        close(g_listener);
        g_listener = -1;
        return false;
    }
    g_endpoint = rawPath;
    struct stat created{};
    if (lstat(rawPath, &created) != 0 || !S_ISSOCK(created.st_mode) ||
        created.st_uid != geteuid()) {
        StopServer();
        return false;
    }
    g_endpointDevice = created.st_dev;
    g_endpointInode = created.st_ino;
    if (chmod(rawPath, 0600) != 0 || listen(g_listener, 4) != 0) {
        StopServer();
        return false;
    }
    g_running.store(true);
    try {
        g_server = std::thread(ServerLoop, g_listener);
    } catch (...) {
        StopServer();
        return false;
    }
    return true;
}
} // namespace

extern "C" __attribute__((visibility("default")))
int ReaperPluginEntry(void*, reaper_plugin_info_t* host) {
    if (!host) {
        StopServer();
        return 0;
    }
    if (host->caller_version != kReaperPluginVersion ||
        !host->Register || !host->GetFunc || !HasRequiredReaperApis(host)) return 0;
    // No production endpoint or REAPER insertion is enabled in this slice.
    return StartServer(std::getenv(kQaSocketVariable)) ? 1 : 0;
}
