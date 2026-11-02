#include "DiagnosticsLogger.h"
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>

static void check(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
int main(int argc, char** argv) {
    try {
        DiagnosticUiOperations tracker;
        tracker.nextFrame();
        auto parent = tracker.begin("Parent", 0, 1000);
        auto child = tracker.begin("Child", 100, 1100);
        check(tracker.sample(499).empty(), "short operation must not alarm");
        auto sample = tracker.sample(600);
        check(sample.size() == 2 && sample[1].parent == parent, "watchdog retains hierarchy");
        check(sample[1].id == child && sample[1].duration == 500, "active child elapsed time");
        check(tracker.sample(700).empty(), "watchdog rate limit");
        auto doneChild = tracker.end(child, 800);
        auto doneParent = tracker.end(parent, 1000);
        check(doneChild.duration == 700 && doneParent.duration == 1000 && doneParent.self == 300,
              "inclusive and exclusive durations must not double-count children");
        check(tracker.sample(2000).empty(), "completed phase must not appear stalled");
        auto cancelled = tracker.begin("Old", 2000, 3000);
        tracker.clear();
        auto fresh = tracker.begin("New", 2001, 3001);
        check(tracker.end(cancelled, 2010).id == 0, "stale completion must not pop a fresh scope");
        check(tracker.end(fresh, 2020).id == fresh, "fresh scope remains valid");

        auto& logger = DiagnosticsLogger::instance();
        logger.setEnabled(true);
        logger.tickFrame();
        logger.setCategoryFilter("Audio"); // watchdog evidence bypasses UI filter
        auto root = logger.beginUiOperation("Test.RenderUI");
        auto blocked = logger.beginUiOperation("Test.DatabaseWait");
        bool workerRejected = false;
        std::thread worker([&] { workerRejected = logger.beginUiOperation("Worker") == 0; });
        worker.join();
        check(workerRejected, "worker operations cannot corrupt UI nesting");
        // Simulate a blocked UI; the independent logger must capture its
        // active operation before either end() call is made.
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
        bool captured = false;
        while (std::chrono::steady_clock::now() < deadline) {
            const auto json = logger.exportJsonString();
            if (json.find("UIStall") != std::string::npos &&
                json.find("Test.DatabaseWait") != std::string::npos) { captured = true; break; }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        check(captured, "watchdog captures active scope while UI end has not run");
        logger.endUiOperation(blocked);
        logger.endUiOperation(root);
        logger.setCategoryFilter("");
        std::string text;
        for (int i = 0; i < 100; ++i) text += u8"中文";
        logger.logAction("Unicode", text.c_str());
        logger.logAction("Unicode", "broken:\xe4\xb8");
        const auto completedDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        bool completed = false;
        while (std::chrono::steady_clock::now() < completedDeadline) {
            const auto json = logger.exportJsonString();
            if (json.find("UiOperation") != std::string::npos && json.find("broken:") != std::string::npos) {
                completed = true; break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        check(completed, "completed spans and text reach export");
        if (argc > 1) check(logger.exportJson(argv[1]), "export fixture");
        logger.setEnabled(false);
        check(logger.beginUiOperation("Disabled") == 0, "disabled mode does not record");
        std::cout << "UI operation lifecycle, watchdog, filtering and export checks passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
    return 0;
}
