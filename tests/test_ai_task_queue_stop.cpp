#include "AITaskQueueStopGate.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

namespace {

using Clock = std::chrono::steady_clock;
constexpr auto kWatchdog = std::chrono::milliseconds(3000);
constexpr auto kStopPublishWindow = std::chrono::milliseconds(500);
constexpr auto kPollInterval = std::chrono::milliseconds(2);

bool WaitForFlag(const std::atomic<bool>& flag, std::chrono::milliseconds limit) {
    const auto deadline = Clock::now() + limit;
    while (!flag.load(std::memory_order_acquire)) {
        if (Clock::now() >= deadline) return false;
        std::this_thread::sleep_for(kPollInterval);
    }
    return true;
}

int Fail(const char* message) {
    std::fputs("FAIL: ", stderr);
    std::fputs(message, stderr);
    std::fputc(10, stderr);
    return 1;
}

// Fixed implementation: the hook pins the worker at the wait decision while it
// still holds the mutex. A correct RequestStop cannot publish the stop from
// there, and once the worker releases the mutex into the wait the stop must
// wake it.
int RunFixedStopWindow() {
    auto* gate = new AITaskQueueStopGate();  // leaked if the watchdog trips
    std::atomic<bool> hookEntered{false};
    std::atomic<bool> hookRelease{false};
    std::atomic<bool> workerDone{false};
    std::atomic<bool> tookTask{true};
    std::atomic<bool> stopperDone{false};

    gate->SetBeforeWaitHook([&]() {
        hookEntered.store(true, std::memory_order_release);
        while (!hookRelease.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
    });

    std::thread worker([&]() {
        int taskId = -1;
        tookTask.store(gate->WaitForTask(taskId), std::memory_order_release);
        workerDone.store(true, std::memory_order_release);
    });

    if (!WaitForFlag(hookEntered, kWatchdog)) {
        hookRelease.store(true, std::memory_order_release);
        worker.detach();
        return Fail("worker never reached the pre-wait window");
    }

    std::thread stopper([&]() {
        gate->RequestStop();
        stopperDone.store(true, std::memory_order_release);
    });

    if (WaitForFlag(stopperDone, kStopPublishWindow)) {
        hookRelease.store(true, std::memory_order_release);
        stopper.join();
        worker.detach();
        return Fail("stop was published while the worker still held the mutex");
    }

    hookRelease.store(true, std::memory_order_release);

    if (!WaitForFlag(workerDone, kWatchdog)) {
        stopper.detach();
        worker.detach();
        return Fail("worker missed the stop notification and stayed blocked");
    }
    worker.join();
    stopper.join();

    if (tookTask.load()) return Fail("worker took a task after stop was requested");
    if (!gate->IsStopRequested()) return Fail("stop flag was not published");

    // Stop wins over queued work: a task pushed after the stop is not taken.
    gate->Push(7);
    int taskId = -1;
    if (gate->WaitForTask(taskId)) return Fail("queued task was taken after stop");
    if (gate->QueueSize() != 1) return Fail("queued task was dropped by the stop");
    return 0;
}

// Legacy harness: same gate, same hook, but RequestStop publishes the flag
// without the mutex exactly like the pre-fix implementation. The publication
// succeeds while the worker is blocked, and the following wait must miss it.
int RunLegacyStopWindow() {
    auto* gate = new AITaskQueueStopGate();  // leaked on purpose
    std::atomic<bool> hookEntered{false};
    std::atomic<bool> hookRelease{false};
    std::atomic<bool> workerDone{false};
    std::atomic<bool> stopperDone{false};

    gate->SetBeforeWaitHook([&]() {
        hookEntered.store(true, std::memory_order_release);
        while (!hookRelease.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
    });

    std::thread worker([&]() {
        int taskId = -1;
        gate->WaitForTask(taskId);
        workerDone.store(true, std::memory_order_release);
    });

    if (!WaitForFlag(hookEntered, kWatchdog)) {
        hookRelease.store(true, std::memory_order_release);
        worker.detach();
        return Fail("worker never reached the pre-wait window");
    }

    std::thread stopper([&]() {
        gate->RequestStop();
        stopperDone.store(true, std::memory_order_release);
    });

    if (!WaitForFlag(stopperDone, kWatchdog)) {
        hookRelease.store(true, std::memory_order_release);
        stopper.detach();
        worker.detach();
        return Fail("legacy stop did not publish while the worker was blocked");
    }

    hookRelease.store(true, std::memory_order_release);

    if (WaitForFlag(workerDone, kWatchdog)) {
        worker.join();
        stopper.join();
        return Fail("legacy stop request did not reproduce the lost wakeup");
    }
    stopper.join();
    worker.detach();
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    bool expectLegacyHang = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--expect-legacy-hang") == 0) {
            expectLegacyHang = true;
        }
    }
    if (expectLegacyHang) {
        const int result = RunLegacyStopWindow();
        if (result == 0) {
            std::puts("AITaskQueueStopGate legacy lost-wakeup window reproduced");
        }
        return result;
    }
    const int result = RunFixedStopWindow();
    if (result == 0) {
        std::puts("AITaskQueueStopGate stop window tests passed");
    }
    return result;
}
