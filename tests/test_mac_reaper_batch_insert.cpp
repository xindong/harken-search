#include "reaper_plugin_macos.h"
#include "ReaperBridgeClient.h"

#include <dlfcn.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#ifndef HARKEN_TEST_REAPER_DYLIB
#error HARKEN_TEST_REAPER_DYLIB is required
#endif

namespace fs = std::filesystem;
namespace Bridge = HarkenSearch::ReaperBridge;

struct ReaProject {};
struct MediaTrack { int id = 1; };
struct MediaItem {
    double position = 0.0;
    double length = 1.0;
    MediaTrack* track = nullptr;
};

static MediaTrack g_track;
static std::vector<std::unique_ptr<MediaTrack>> g_extraTracks;
static std::vector<std::unique_ptr<MediaItem>> g_items;
static double g_cursor = 30.0;
static void (*g_timer)() = nullptr;
static bool g_rejectTimerRegistration = false;
static std::thread::id g_mainThread;
static int g_insertCalls = 0;
static int g_offMainCalls = 0;
static bool g_rejectCorrection = false;
static int g_failures = 0;

static void Check(bool condition, const char* label) {
    std::printf("%s %s\n", condition ? "PASS" : "FAIL", label);
    if (!condition) ++g_failures;
}
static int MockInsertMedia(const char* path, int) {
    ++g_insertCalls;
    if (std::this_thread::get_id() != g_mainThread) ++g_offMainCalls;
    if (std::string(path).find("fail-track") != std::string::npos) {
        g_extraTracks.push_back(std::make_unique<MediaTrack>());
        return 0;
    }
    if (std::string(path).find("fail") != std::string::npos) return 0;
    auto item = std::make_unique<MediaItem>();
    item->position = g_cursor - 2.0;
    item->length = std::string(path).find("second") != std::string::npos ? 2.0 : 1.5;
    item->track = &g_track;
    g_items.push_back(std::move(item));
    return 1;
}
static double MockGetCursorPosition() { return g_cursor; }
static void MockSetEditCurPos(double position, bool, bool) { g_cursor = position; }
static int MockCountMediaItems(ReaProject*) { return static_cast<int>(g_items.size()); }
static MediaItem* MockGetMediaItem(ReaProject*, int index) {
    return index >= 0 && index < static_cast<int>(g_items.size())
        ? g_items[static_cast<std::size_t>(index)].get() : nullptr;
}
static double MockGetMediaItemInfoValue(MediaItem* item, const char* key) {
    if (std::strcmp(key, "D_POSITION") == 0) return item->position;
    if (std::strcmp(key, "D_LENGTH") == 0) return item->length;
    return 0.0;
}
static bool MockSetMediaItemInfoValue(MediaItem* item, const char* key, double value) {
    if (g_rejectCorrection || std::strcmp(key, "D_POSITION") != 0) return false;
    item->position = value;
    return true;
}
static MediaTrack* MockGetMediaItemTrack(MediaItem* item) { return item->track; }
static bool MockDeleteTrackMediaItem(MediaTrack*, MediaItem* item) {
    const auto found = std::find_if(g_items.begin(), g_items.end(),
        [&](const auto& value) { return value.get() == item; });
    if (found == g_items.end()) return false;
    g_items.erase(found);
    return true;
}
static int MockCountTracks(ReaProject*) {
    return 1 + static_cast<int>(g_extraTracks.size());
}
static MediaTrack* MockGetTrack(ReaProject*, int index) {
    if (index == 0) return &g_track;
    const int extra = index - 1;
    return extra >= 0 && extra < static_cast<int>(g_extraTracks.size())
        ? g_extraTracks[static_cast<std::size_t>(extra)].get() : nullptr;
}
static void MockDeleteTrack(MediaTrack* track) {
    const auto found = std::find_if(g_extraTracks.begin(), g_extraTracks.end(),
        [&](const auto& value) { return value.get() == track; });
    if (found != g_extraTracks.end()) g_extraTracks.erase(found);
}
static void MockVoid() {}
static void MockUndoEnd(const char*, int) {}
static void* MockGetFunc(const char* name) {
#define MATCH(api, function) if (std::strcmp(name, api) == 0) return reinterpret_cast<void*>(function)
    MATCH("InsertMedia", MockInsertMedia);
    MATCH("GetCursorPosition", MockGetCursorPosition);
    MATCH("SetEditCurPos", MockSetEditCurPos);
    MATCH("CountMediaItems", MockCountMediaItems);
    MATCH("GetMediaItem", MockGetMediaItem);
    MATCH("GetMediaItemInfo_Value", MockGetMediaItemInfoValue);
    MATCH("SetMediaItemInfo_Value", MockSetMediaItemInfoValue);
    MATCH("GetMediaItemTrack", MockGetMediaItemTrack);
    MATCH("DeleteTrackMediaItem", MockDeleteTrackMediaItem);
    MATCH("CountTracks", MockCountTracks);
    MATCH("GetTrack", MockGetTrack);
    MATCH("DeleteTrack", MockDeleteTrack);
    MATCH("UpdateArrange", MockVoid);
    MATCH("Undo_BeginBlock", MockVoid);
    MATCH("Undo_EndBlock", MockUndoEnd);
#undef MATCH
    return nullptr;
}
static int MockRegister(const char* name, void* callback) {
    if (std::strcmp(name, "command_id") == 0) return 62001;
    if (std::strcmp(name, "timer") == 0) {
        if (g_rejectTimerRegistration) return 0;
        g_timer = reinterpret_cast<void (*)()>(callback);
        return 1;
    }
    if (std::strcmp(name, "-timer") == 0) {
        if (g_timer == reinterpret_cast<void (*)()>(callback)) g_timer = nullptr;
        return 1;
    }
    return 0;
}
static bool PumpUntil(std::future<Bridge::BatchSendResult>& future) {
    for (int attempt = 0; attempt < 500; ++attempt) {
        if (g_timer) g_timer();
        if (future.wait_for(std::chrono::milliseconds(5)) == std::future_status::ready)
            return true;
    }
    return false;
}
static void MakeFixture(const fs::path& path) {
    std::ofstream file(path, std::ios::binary);
    const char wave[44] = {'R','I','F','F',36,0,0,0,'W','A','V','E','f','m','t',' ',
        16,0,0,0,1,0,1,0,static_cast<char>(0x80),static_cast<char>(0x3e),0,0,
        0x00,0x7d,0,0,2,0,16,0,'d','a','t','a',0,0,0,0};
    file.write(wave, sizeof(wave));
}
int main() {
    g_mainThread = std::this_thread::get_id();
    const char* root = std::getenv("HARKEN_MAC_TEST_ROOT");
    if (!root || root[0] != '/') return 2;
    std::string pattern = (fs::path(root) / "insert-XXXXXX").string();
    std::vector<char> temp(pattern.begin(), pattern.end());
    temp.push_back('\0');
    char* directory = mkdtemp(temp.data());
    if (!directory) return 2;
    const fs::path base(directory);
    const fs::path socket = base / "bridge.sock";
    const fs::path first = base / fs::u8path(u8"中文 first.wav");
    const fs::path second = base / "second # tail.wav";
    const fs::path fail = base / "fail.wav";
    const fs::path failTrack = base / "fail-track.wav";
    MakeFixture(first);
    MakeFixture(second);
    MakeFixture(fail);
    MakeFixture(failTrack);
    const std::string socketText = socket.string();
    const std::wstring socketName(socketText.begin(), socketText.end());
    setenv("HARKEN_REAPER_BRIDGE_TEST_SOCKET", socketText.c_str(), 1);
    void* library = dlopen(HARKEN_TEST_REAPER_DYLIB, RTLD_NOW | RTLD_LOCAL);
    if (!library) { std::fprintf(stderr, "dlopen: %s\n", dlerror()); return 2; }
    auto entry = reinterpret_cast<int (*)(void*, reaper_plugin_info_t*)>(
        dlsym(library, "ReaperPluginEntry"));
    if (!entry) return 2;
    reaper_plugin_info_t host{kReaperPluginVersion, nullptr, MockRegister, MockGetFunc};
    const bool loaded = entry(nullptr, &host) == 1;
    Check(loaded && g_timer, "extension registers a host main-thread timer");
    if (!loaded || !g_timer) {
        if (loaded) entry(nullptr, nullptr);
        dlclose(library);
        std::printf("Mac REAPER batch failures: %d\n", g_failures);
        return g_failures ? 1 : 2;
    }
    Check(Bridge::ProbeHandshake(socketName, 1200).code == Bridge::HandshakeCode::Ready,
          "handshake becomes ready only after host callback registration");

    auto send = std::async(std::launch::async, [&] {
        return Bridge::SendFiles({first.u8string(), second.u8string()}, {}, socketName, 5000);
    });
    const bool completed = PumpUntil(send);
    Check(completed, "two-file batch responds while host timer runs");
    const auto result = send.get();
    Check(result.ok && result.sent == 2 && g_items.size() == 2,
          "two files are inserted exactly once");
    if (g_items.size() == 2) {
        Check(g_items[0]->position >= 30.0 &&
              g_items[1]->position >= g_items[0]->position + g_items[0]->length,
              "files are placed sequentially at or after edit cursor");
        Check(g_cursor >= g_items[1]->position + g_items[1]->length,
              "edit cursor advances to the final item end");
    }
    Check(g_offMainCalls == 0, "REAPER insert API is called only on main thread");

    g_items.clear(); g_cursor = 30.0;
    auto failed = std::async(std::launch::async, [&] {
        return Bridge::SendFiles({first.u8string(), fail.u8string()}, {}, socketName, 5000);
    });
    Check(PumpUntil(failed), "failed second insert receives a response");
    const auto failedResult = failed.get();
    Check(!failedResult.ok && g_items.empty() && g_cursor == 30.0,
          "mid-batch failure rolls back items and original cursor");

    g_items.clear(); g_cursor = 30.0;
    auto failedTrack = std::async(std::launch::async, [&] {
        return Bridge::SendFiles({first.u8string(), failTrack.u8string()},
                                 {}, socketName, 5000);
    });
    Check(PumpUntil(failedTrack), "batch with host-created track receives response");
    const auto failedTrackResult = failedTrack.get();
    Check(!failedTrackResult.ok && g_items.empty() && g_extraTracks.empty() &&
              g_cursor == 30.0,
          "failed batch removes newly created track and items");

    g_items.clear(); g_cursor = 30.0; g_rejectCorrection = true;
    auto rejected = std::async(std::launch::async, [&] {
        return Bridge::SendFiles({first.u8string()}, {}, socketName, 5000);
    });
    Check(PumpUntil(rejected), "position-correction failure receives a response");
    const auto rejectedResult = rejected.get();
    Check(!rejectedResult.ok && g_items.empty() && g_cursor == 30.0,
          "position-correction failure rolls back cleanly");
    g_rejectCorrection = false;

    const int insertCallsBeforeMissing = g_insertCalls;
    const auto missing = Bridge::SendFiles(
        {(base / "missing.wav").u8string()}, {}, socketName, 1500);
    Check(!missing.ok && g_insertCalls == insertCallsBeforeMissing && g_items.empty(),
          "missing file is rejected before any host insertion call");

    auto withoutTimer = std::async(std::launch::async, [&] {
        return Bridge::SendFiles({first.u8string()}, {}, socketName, 4000);
    });
    Check(withoutTimer.wait_for(std::chrono::seconds(4)) == std::future_status::ready,
          "unserved request times out before client timeout");
    const auto timedOut = withoutTimer.get();
    const int callsBeforeLateTimer = g_insertCalls;
    if (g_timer) g_timer();
    Check(!timedOut.ok && g_insertCalls == callsBeforeLateTimer && g_items.empty(),
          "timed-out request is never inserted later");

    auto unloading = std::async(std::launch::async, [&] {
        return Bridge::SendFiles({first.u8string()}, {}, socketName, 5000);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    entry(nullptr, nullptr);
    Check(unloading.wait_for(std::chrono::seconds(1)) == std::future_status::ready,
          "unload releases a waiting client without a host mutation");
    const auto unloaded = unloading.get();
    Check(!unloaded.ok && g_items.empty(),
          "unloaded request cannot report a successful insert");
    Check(g_timer == nullptr && !fs::exists(socket),
          "unload unregisters timer and removes its own socket");

    g_rejectTimerRegistration = true;
    Check(entry(nullptr, &host) == 0 && !fs::exists(socket) && !g_timer,
          "extension refuses a host that rejects timer registration");
    g_rejectTimerRegistration = false;
    dlclose(library);
    unsetenv("HARKEN_REAPER_BRIDGE_TEST_SOCKET");
    std::printf("Mac REAPER batch failures: %d\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
