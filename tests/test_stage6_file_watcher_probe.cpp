#include "platform/file_watcher.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr int kCycles = 50;
constexpr auto kEventTimeout = std::chrono::seconds(2);

enum class Expected { None, Create, Rename, Delete };

struct ProbeState {
    std::mutex mutex;
    std::condition_variable changed;
    Expected expected = Expected::None;
    std::string name;
    std::string oldName;
    std::chrono::steady_clock::time_point started;
    std::vector<double> deliveryMs;
    std::vector<std::string> eventOrder;
    int receivedCount = 0;
    bool invalidOrder = false;
};

static std::string FileName(const std::string& path) {
    return std::filesystem::path(path).filename().string();
}

static void OnEvent(ProbeState& state, const FileSystemWatcher::FileEvent& event) {
    if (event.type == FileSystemWatcher::EventType::FileModified ||
        event.type == FileSystemWatcher::EventType::NeedRescan)
        return;
    std::lock_guard<std::mutex> lock(state.mutex);
    bool match = false;
    const std::string name = FileName(event.path);
    if (state.expected == Expected::Create)
        match = event.type == FileSystemWatcher::EventType::FileAdded && name == state.name;
    else if (state.expected == Expected::Rename)
        match = event.type == FileSystemWatcher::EventType::FileRenamed &&
                name == state.name && FileName(event.oldPath) == state.oldName;
    else if (state.expected == Expected::Delete)
        match = event.type == FileSystemWatcher::EventType::FileDeleted && name == state.name;
    if (!match) {
        state.invalidOrder = true;
        state.changed.notify_one();
        return;
    }
    state.deliveryMs.push_back(std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - state.started).count());
    state.eventOrder.push_back(state.expected == Expected::Create ? "create" :
                               state.expected == Expected::Rename ? "rename" : "delete");
    ++state.receivedCount;
    state.expected = Expected::None;
    state.changed.notify_one();
}

static void Expect(ProbeState& state, Expected expected, const std::string& name,
                   const std::string& oldName = {}) {
    std::lock_guard<std::mutex> lock(state.mutex);
    state.expected = expected;
    state.name = name;
    state.oldName = oldName;
    state.started = std::chrono::steady_clock::now();
}

static bool Wait(ProbeState& state, int target) {
    std::unique_lock<std::mutex> lock(state.mutex);
    return state.changed.wait_for(lock, kEventTimeout, [&] {
        return state.receivedCount >= target || state.invalidOrder;
    }) && !state.invalidOrder && state.receivedCount == target;
}

static bool WaitForCreate(ProbeState& state, const std::filesystem::path& path, int target) {
    Expect(state, Expected::Create, path.filename().string());
    std::ofstream(path, std::ios::binary) << "probe";
    return Wait(state, target);
}

static bool WaitForRename(ProbeState& state, const std::filesystem::path& oldPath,
                          const std::filesystem::path& newPath, int target) {
    Expect(state, Expected::Rename, newPath.filename().string(), oldPath.filename().string());
    std::filesystem::rename(oldPath, newPath);
    return Wait(state, target);
}

static bool WaitForDelete(ProbeState& state, const std::filesystem::path& path, int target) {
    Expect(state, Expected::Delete, path.filename().string());
    std::filesystem::remove(path);
    return Wait(state, target);
}

static double Percentile(std::vector<double> values, double percentile) {
    std::sort(values.begin(), values.end());
    const std::size_t index = static_cast<std::size_t>(
        std::ceil(percentile * static_cast<double>(values.size()))) - 1U;
    return values[(std::min)(index, values.size() - 1U)];
}

static int EnvironmentGate(const char* reason) {
    std::cout << "{\"status\":\"environment_gate\",\"reason\":\"" << reason << "\"}\n";
    return 2;
}

static int Failed(const char* reason) {
    std::cout << "{\"status\":\"failed\",\"reason\":\"" << reason << "\"}\n";
    return 1;
}

#ifdef _WIN32
static bool RecoverMissingRoot(const std::filesystem::path& root) {
    const auto available = root / "available";
    const auto missing = root / "missing";
    std::filesystem::create_directory(available);
    ProbeState state;
    bool recovered = false;
    FileSystemWatcher watcher;
    watcher.setEventCallback([&](const FileSystemWatcher::FileEvent& event) {
        if (event.type == FileSystemWatcher::EventType::NeedRescan && event.path == missing.string()) {
            std::lock_guard<std::mutex> lock(state.mutex);
            recovered = true;
            state.changed.notify_one();
        } else {
            OnEvent(state, event);
        }
    });
    if (!watcher.addWatchDirectory(missing.string()) ||
        !watcher.addWatchDirectory(available.string()) || !watcher.start() ||
        !WaitForCreate(state, available / "before-recovery.tmp", 1))
        return false;
    std::filesystem::create_directory(missing);
    {
        std::unique_lock<std::mutex> lock(state.mutex);
        if (!state.changed.wait_for(lock, std::chrono::seconds(5), [&] { return recovered; }))
            return false;
    }
    return WaitForCreate(state, missing / "after-recovery.tmp", 2) &&
           WaitForCreate(state, available / "still-watched.tmp", 3);
}

static bool CoalesceRepeatedWrites(const std::filesystem::path& root) {
    const auto directory = root / "coalesce";
    std::filesystem::create_directory(directory);
    const auto path = directory / "coalesce.tmp";
    std::ofstream(path, std::ios::binary) << "start";
    std::mutex mutex;
    std::condition_variable changed;
    unsigned int modifications = 0;
    FileSystemWatcher watcher;
    watcher.setCoalesceWindowMs(400);
    watcher.setEventCallback([&](const FileSystemWatcher::FileEvent& event) {
        if (event.type == FileSystemWatcher::EventType::FileModified && event.path == path.string()) {
            std::lock_guard<std::mutex> lock(mutex);
            ++modifications;
            changed.notify_one();
        }
    });
    if (!watcher.addWatchDirectory(directory.string()) || !watcher.start()) return false;
    for (int append = 0; append < 4; ++append) {
        std::ofstream(path, std::ios::binary | std::ios::app) << "append";
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
    }
    std::unique_lock<std::mutex> lock(mutex);
    if (!changed.wait_for(lock, kEventTimeout, [&] { return modifications != 0; })) return false;
    changed.wait_for(lock, std::chrono::milliseconds(500), [&] { return modifications > 1; });
    return modifications == 1;
}
#endif

} // namespace

int main(int argc, [[maybe_unused]] char* argv[]) {
    const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = std::filesystem::temp_directory_path() /
                      ("aris-stage6-watcher-" + std::to_string(unique));
    std::filesystem::create_directories(root);

#ifdef _WIN32
    if (argc == 2 && std::string(argv[1]) == "--review-recovery") {
        const bool recovered = RecoverMissingRoot(root);
        const bool coalesced = CoalesceRepeatedWrites(root);
        std::filesystem::remove_all(root);
        std::cout << "{\"recovery\":" << (recovered ? "true" : "false")
                  << ",\"coalesced\":" << (coalesced ? "true" : "false") << "}\n";
        return recovered && coalesced ? 0 : 1;
    }
#else
    (void)argc;
#endif

    ProbeState state;
    state.deliveryMs.reserve(kCycles * 3);
    state.eventOrder.reserve(kCycles * 3);
    FileSystemWatcher watcher;
    watcher.setEventCallback([&](const FileSystemWatcher::FileEvent& event) { OnEvent(state, event); });
    if (!watcher.addWatchDirectory(root.string())) {
        std::filesystem::remove_all(root);
        return EnvironmentGate("watch directory registration failed");
    }
    if (!watcher.start()) {
        std::filesystem::remove_all(root);
        return EnvironmentGate("watcher start failed");
    }

#ifdef _WIN32
    std::this_thread::sleep_for(std::chrono::milliseconds(1300));
#endif

    bool passed = true;
    for (int cycle = 0; cycle < kCycles; ++cycle) {
        const auto created = root / ("probe-" + std::to_string(cycle) + ".tmp");
        const auto renamed = root / ("probe-" + std::to_string(cycle) + ".done");
        const int base = cycle * 3;
        if (!WaitForCreate(state, created, base + 1) ||
            !WaitForRename(state, created, renamed, base + 2) ||
            !WaitForDelete(state, renamed, base + 3)) {
            passed = false;
            break;
        }
    }

    const auto shutdownStart = std::chrono::steady_clock::now();
    watcher.stop();
    const double shutdownMs = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - shutdownStart).count();
    const bool stopped = !watcher.isRunning() && shutdownMs < 2000.0;
    std::filesystem::remove_all(root);
    if (!passed)
        return Failed(state.invalidOrder ? "duplicate or out-of-order event" : "event timeout or missing event");
    if (!stopped)
        return Failed("watcher stop/join did not finish cleanly");

    std::cout << std::fixed << std::setprecision(6)
              << "{\"status\":\"measured\",\"delivery\":{\"median_ms\":"
              << Percentile(state.deliveryMs, 0.50) << ",\"p95_ms\":"
              << Percentile(state.deliveryMs, 0.95) << ",\"max_ms\":"
              << *std::max_element(state.deliveryMs.begin(), state.deliveryMs.end())
              << "},\"received_count\":" << state.receivedCount << ",\"event_order\":[";
    for (std::size_t index = 0; index < state.eventOrder.size(); ++index) {
        if (index) std::cout << ',';
        std::cout << '\"' << state.eventOrder[index] << '\"';
    }
    std::cout << "],\"shutdown_elapsed_ms\":" << shutdownMs << "}\n";
    return 0;
}
