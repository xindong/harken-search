#include "reaper_plugin_macos.h"
#include "platform/reaper_bridge_endpoint_macos.h"
#include "ReaperBridgeProtocol.h"

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>
#include <poll.h>

#include <atomic>
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <cstdlib>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <vector>

struct ReaProject;
struct MediaTrack;
struct MediaItem;

namespace {
namespace Protocol = HarkenSearch::ReaperBridge;

struct HostApi {
    int (*insertMedia)(const char*, int) = nullptr;
    double (*getCursorPosition)() = nullptr;
    void (*setEditCurPos)(double, bool, bool) = nullptr;
    int (*countMediaItems)(ReaProject*) = nullptr;
    MediaItem* (*getMediaItem)(ReaProject*, int) = nullptr;
    double (*getMediaItemInfoValue)(MediaItem*, const char*) = nullptr;
    bool (*setMediaItemInfoValue)(MediaItem*, const char*, double) = nullptr;
    MediaTrack* (*getMediaItemTrack)(MediaItem*) = nullptr;
    bool (*deleteTrackMediaItem)(MediaTrack*, MediaItem*) = nullptr;
    int (*countTracks)(ReaProject*) = nullptr;
    MediaTrack* (*getTrack)(ReaProject*, int) = nullptr;
    void (*deleteTrack)(MediaTrack*) = nullptr;
    void (*updateArrange)() = nullptr;
    void (*undoBegin)() = nullptr;
    void (*undoEnd)(const char*, int) = nullptr;
};

struct PendingInsert {
    std::vector<std::string> paths;
    std::mutex mutex;
    std::condition_variable ready;
    bool started = false;
    bool completed = false;
    bool cancelled = false;
    std::string response;
};

std::atomic<bool> g_running{false};
std::thread g_server;
int g_listener = -1;
std::string g_endpoint;
dev_t g_endpointDevice = 0;
ino_t g_endpointInode = 0;
HostApi g_api;
int (*g_register)(const char*, void*) = nullptr;
bool g_timerRegistered = false;
int g_actionId = 0;
std::mutex g_queueMutex;
std::deque<std::shared_ptr<PendingInsert>> g_queue;

void OnTimer();

bool LoadHostApi(reaper_plugin_info_t* host, HostApi& api) {
#define LOAD(field, name) api.field = reinterpret_cast<decltype(api.field)>(host->GetFunc(name))
    LOAD(insertMedia, "InsertMedia");
    LOAD(getCursorPosition, "GetCursorPosition");
    LOAD(setEditCurPos, "SetEditCurPos");
    LOAD(countMediaItems, "CountMediaItems");
    LOAD(getMediaItem, "GetMediaItem");
    LOAD(getMediaItemInfoValue, "GetMediaItemInfo_Value");
    LOAD(setMediaItemInfoValue, "SetMediaItemInfo_Value");
    LOAD(getMediaItemTrack, "GetMediaItemTrack");
    LOAD(deleteTrackMediaItem, "DeleteTrackMediaItem");
    LOAD(countTracks, "CountTracks");
    LOAD(getTrack, "GetTrack");
    LOAD(deleteTrack, "DeleteTrack");
    LOAD(updateArrange, "UpdateArrange");
    LOAD(undoBegin, "Undo_BeginBlock");
    LOAD(undoEnd, "Undo_EndBlock");
#undef LOAD
    return api.insertMedia && api.getCursorPosition && api.setEditCurPos &&
           api.countMediaItems && api.getMediaItem && api.getMediaItemInfoValue &&
           api.setMediaItemInfoValue && api.getMediaItemTrack &&
           api.deleteTrackMediaItem && api.countTracks && api.getTrack &&
           api.deleteTrack;
}

bool FileIsReadableAudioCandidate(const std::string& path) {
    if (path.empty() || path.find('\0') != std::string::npos) return false;
    struct stat info{};
    return stat(path.c_str(), &info) == 0 && S_ISREG(info.st_mode) &&
           access(path.c_str(), R_OK) == 0;
}

std::unordered_set<MediaItem*> SnapshotItems() {
    std::unordered_set<MediaItem*> items;
    const int count = g_api.countMediaItems(nullptr);
    for (int index = 0; index < count; ++index) {
        if (MediaItem* item = g_api.getMediaItem(nullptr, index)) items.insert(item);
    }
    return items;
}

std::unordered_set<MediaTrack*> SnapshotTracks() {
    std::unordered_set<MediaTrack*> tracks;
    const int count = g_api.countTracks(nullptr);
    for (int index = 0; index < count; ++index) {
        if (MediaTrack* track = g_api.getTrack(nullptr, index)) tracks.insert(track);
    }
    return tracks;
}

std::vector<MediaItem*> FindNewItems(const std::unordered_set<MediaItem*>& before) {
    std::vector<MediaItem*> items;
    const int count = g_api.countMediaItems(nullptr);
    for (int index = 0; index < count; ++index) {
        MediaItem* item = g_api.getMediaItem(nullptr, index);
        if (item && before.find(item) == before.end()) items.push_back(item);
    }
    return items;
}

std::vector<Protocol::ItemSpan> ReadSpans(const std::vector<MediaItem*>& items) {
    std::vector<Protocol::ItemSpan> spans;
    spans.reserve(items.size());
    for (MediaItem* item : items) {
        const double start = g_api.getMediaItemInfoValue(item, "D_POSITION");
        const double length = g_api.getMediaItemInfoValue(item, "D_LENGTH");
        spans.push_back({start, start + length});
    }
    return spans;
}

bool RollBackBatch(const std::vector<MediaItem*>& created,
                   const std::unordered_set<MediaTrack*>& tracksBefore,
                   double originalCursor) {
    bool complete = true;
    for (auto it = created.rbegin(); it != created.rend(); ++it) {
        MediaTrack* track = g_api.getMediaItemTrack(*it);
        if (!track || !g_api.deleteTrackMediaItem(track, *it)) complete = false;
    }
    for (int index = g_api.countTracks(nullptr) - 1; index >= 0; --index) {
        MediaTrack* track = g_api.getTrack(nullptr, index);
        if (track && tracksBefore.find(track) == tracksBefore.end())
            g_api.deleteTrack(track);
    }
    g_api.setEditCurPos(originalCursor, false, false);
    if (g_api.updateArrange) g_api.updateArrange();
    return complete;
}

std::string InsertBatchAtEditCursor(const std::vector<std::string>& paths) {
    for (const std::string& path : paths) {
        if (!FileIsReadableAudioCandidate(path))
            return "ERR|INSERT|file does not exist or is not readable\n";
    }
    const double originalCursor = g_api.getCursorPosition();
    if (!std::isfinite(originalCursor) || originalCursor < 0.0)
        return "ERR|INSERT|invalid edit cursor\n";
    const auto tracksBefore = SnapshotTracks();
    std::vector<MediaItem*> created;
    double nextCursor = originalCursor;
    double firstStart = originalCursor;
    double finalEnd = originalCursor;
    if (g_api.undoBegin) g_api.undoBegin();
    auto fail = [&](std::string reason) {
        if (!RollBackBatch(created, tracksBefore, originalCursor))
            reason += "; rollback incomplete";
        if (g_api.undoEnd) g_api.undoEnd("Harken Search: REAPER batch rolled back", -1);
        return "ERR|INSERT|" + reason + "\n";
    };

    for (std::size_t index = 0; index < paths.size(); ++index) {
        g_api.setEditCurPos(nextCursor, false, false);
        const auto before = SnapshotItems();
        const int inserted = g_api.insertMedia(paths[index].c_str(), 0);
        auto newItems = FindNewItems(before);
        created.insert(created.end(), newItems.begin(), newItems.end());
        if (inserted <= 0 || newItems.empty())
            return fail("REAPER failed to insert file " + std::to_string(index + 1));

        auto placement = Protocol::EvaluatePlacement(nextCursor, ReadSpans(newItems));
        if (!placement.valid) return fail(placement.error);
        if (placement.correction > 0.0) {
            for (MediaItem* item : newItems) {
                const double position = g_api.getMediaItemInfoValue(item, "D_POSITION");
                if (!g_api.setMediaItemInfoValue(
                        item, "D_POSITION", position + placement.correction))
                    return fail("cannot correct REAPER insert position");
            }
            placement = Protocol::EvaluatePlacement(nextCursor, ReadSpans(newItems));
        }
        if (!placement.valid || placement.actualStart + 1.0e-9 < nextCursor)
            return fail("inserted item starts before edit cursor");
        if (index == 0) firstStart = placement.actualStart;
        finalEnd = placement.actualEnd;
        nextCursor = placement.nextCursor;
    }
    g_api.setEditCurPos(nextCursor, false, false);
    if (g_api.undoEnd) g_api.undoEnd("Harken Search: Insert Media Batch", -1);
    if (g_api.updateArrange) g_api.updateArrange();
    return Protocol::BuildInsertBatchSuccessResponse(
        static_cast<int>(paths.size()), firstStart, finalEnd);
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

void OnTimer() {
    if (!g_running.load()) return;
    std::shared_ptr<PendingInsert> pending;
    for (;;) {
        {
            std::lock_guard<std::mutex> lock(g_queueMutex);
            if (g_queue.empty()) return;
            pending = std::move(g_queue.front());
            g_queue.pop_front();
        }
        std::lock_guard<std::mutex> lock(pending->mutex);
        if (!pending->cancelled) {
            pending->started = true;
            break;
        }
    }
    const std::string response = InsertBatchAtEditCursor(pending->paths);
    {
        std::lock_guard<std::mutex> lock(pending->mutex);
        pending->response = response;
        pending->completed = true;
    }
    pending->ready.notify_one();
}

void HandleInsertRequest(int client, std::vector<std::string> paths) {
    // Reject missing files before scheduling any host mutation.
    for (const std::string& path : paths) {
        if (!FileIsReadableAudioCandidate(path)) {
            SendResponse(client, "ERR|INSERT|file does not exist or is not readable\n");
            return;
        }
    }
    auto pending = std::make_shared<PendingInsert>();
    pending->paths = std::move(paths);
    {
        std::lock_guard<std::mutex> lock(g_queueMutex);
        if (!g_running.load()) {
            SendResponse(client, "ERR|UNLOAD|REAPER extension is unloading\n");
            return;
        }
        g_queue.push_back(pending);
    }
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(3000);
    std::unique_lock<std::mutex> lock(pending->mutex);
    while (!pending->completed) {
        if (!g_running.load()) {
            pending->cancelled = true;
            pending->response = "ERR|UNLOAD|REAPER extension is unloading\n";
            break;
        }
        if (!pending->started && std::chrono::steady_clock::now() >= deadline) {
            pending->cancelled = true;
            pending->response = "ERR|TIMEOUT|REAPER host timer did not run\n";
            break;
        }
        pending->ready.wait_for(lock, std::chrono::milliseconds(25));
    }
    const std::string response = pending->response;
    lock.unlock();
    SendResponse(client, response);
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
            SendResponse(client, Protocol::BuildHelloResponse(g_actionId));
        }
        return;
    }
    if (Protocol::TrimProtocolLine(request) == "PING") {
        SendResponse(client, Protocol::BuildHelloResponse(g_actionId));
        return;
    }
    std::vector<std::string> paths;
    std::string error;
    if (Protocol::ParseInsertBatchRequest(request, paths, error)) {
        HandleInsertRequest(client, std::move(paths));
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
    if (g_timerRegistered && g_register) {
        g_register("-timer", reinterpret_cast<void*>(OnTimer));
        g_timerRegistered = false;
    }
    if (g_server.joinable()) g_server.join();
    if (g_listener >= 0) close(g_listener);
    g_listener = -1;
    {
        std::lock_guard<std::mutex> lock(g_queueMutex);
        g_queue.clear();
    }
    RemoveOwnEndpoint();
    g_api = {};
    g_register = nullptr;
    g_actionId = 0;
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
        !host->Register || !host->GetFunc || g_server.joinable()) return 0;
    HostApi api;
    if (!LoadHostApi(host, api)) return 0;
    g_api = api;
    g_register = host->Register;
    if (!g_register("timer", reinterpret_cast<void*>(OnTimer))) {
        StopServer();
        return 0;
    }
    g_timerRegistered = true;
    const std::string endpoint = Protocol::MacSelectedSocketPath();
    if (!StartServer(endpoint.c_str())) {
        StopServer();
        return 0;
    }
    return 1;
}
