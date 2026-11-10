#include "platform/file_watcher.h"

#include <CoreServices/CoreServices.h>

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

using EventType = FileSystemWatcher::EventType;

bool DispatchSyntheticEvents() {
    FileSystemWatcher watcher;
    std::vector<EventType> received;
    watcher.setEventCallback([&](const FileSystemWatcher::FileEvent& event) {
        received.push_back(event.type);
    });
    const char* paths[] = {"/isolated/new.wav", "/isolated", "/isolated/new.wav"};
    const uint32_t flags[] = {
        kFSEventStreamEventFlagItemCreated | kFSEventStreamEventFlagItemIsFile,
        kFSEventStreamEventFlagMustScanSubDirs,
        kFSEventStreamEventFlagItemModified | kFSEventStreamEventFlagItemIsFile,
    };
    watcher.processAppleEvents(3, paths, flags, nullptr);
    if (received != std::vector<EventType>{EventType::FileAdded, EventType::NeedRescan,
                                            EventType::FileModified}) {
        std::cerr << "synthetic callback sequence mismatch: " << received.size() << " events\n";
        return false;
    }
    watcher.setEventCallback({});
    watcher.processAppleEvents(1, paths, flags, nullptr);
    return true;
}

bool DispatchRealFileEvent(const std::filesystem::path& testRoot) {
    const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto directory = testRoot / ("watcher-dispatch-" + std::to_string(unique));
    std::filesystem::create_directories(directory);

    FileSystemWatcher watcher;
    std::mutex mutex;
    std::condition_variable changed;
    bool sawAdded = false;
    const auto file = directory / "new.wav";
    watcher.setEventCallback([&](const FileSystemWatcher::FileEvent& event) {
        if (event.type == EventType::FileAdded && event.path == file.string()) {
            std::lock_guard<std::mutex> guard(mutex);
            sawAdded = true;
            changed.notify_one();
        }
    });
    if (!watcher.addWatchDirectory(directory.string()) || !watcher.start()) {
        std::cerr << "watcher startup failed\n";
        return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    std::ofstream(file, std::ios::binary) << "synthetic audio marker";
    std::unique_lock<std::mutex> guard(mutex);
    const bool delivered = changed.wait_for(guard, std::chrono::seconds(5), [&] { return sawAdded; });
    guard.unlock();
    watcher.stop();
    if (!delivered || watcher.isRunning()) {
        std::cerr << "real file create event or watcher shutdown failed\n";
        return false;
    }
    std::cout << "real file create event delivered under " << directory << "\n";
    return true;
}

} // namespace

int main() {
    const char* rootEnv = std::getenv("HARKEN_MAC_TEST_ROOT");
    if (!rootEnv || !*rootEnv) {
        std::cerr << "HARKEN_MAC_TEST_ROOT is required\n";
        return 2;
    }
    const std::filesystem::path root(rootEnv);
    if (!root.is_absolute()) return 2;
    if (!DispatchSyntheticEvents() || !DispatchRealFileEvent(root)) return 1;
    std::cout << "Mac file watcher dispatch PASS\n";
    return 0;
}
