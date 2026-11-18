#include "AITaskQueueStopGate.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>

namespace {

constexpr auto kWatchdog = std::chrono::milliseconds(1500);

struct StopPointEvents {
    std::mutex mutex;
    std::condition_variable cv;
    bool workerAtStopPoint = false;
    bool stopPathReached = false;
    bool workerLeftStopPoint = false;
    bool workerReturned = false;
};

int Fail(const char* message) {
    std::fputs("FAIL: ", stderr);
    std::fputs(message, stderr);
    std::fputc(10, stderr);
    return 1;
}

// Explicit handshake:
//   1. the worker parks at the wait decision while still holding the gate
//      mutex and publishes that event;
//   2. the stopper runs the real RequestStop path and publishes the last
//      moment it can act while the mutex is held: after a real try-lock
//      contention in the guarded path (before the shared lock-guard block), or
//      after publishing outside the lock in the legacy test path;
//   3. the worker leaves its park point only after that event and records
//      whether the stop was already published.
// No step branches on elapsed time; the watchdog only reports a hang.
int RunScenario(bool expectLegacyHang, bool delayedStopper) {
    StopPointEvents events;
    std::atomic<bool> publishedWhileHeld{false};
    AITaskQueueStopGate gate;

    gate.SetBeforeWaitHook([&]() {
        std::unique_lock<std::mutex> lock(events.mutex);
        events.workerAtStopPoint = true;
        events.cv.notify_all();
        events.cv.wait(lock, [&]() { return events.stopPathReached; });
        // Still holding the gate mutex here. The guarded stop path can only be
        // blocked on that mutex; the legacy path already published.
        publishedWhileHeld.store(gate.IsStopRequested());
        events.workerLeftStopPoint = true;
        events.cv.notify_all();
    });

    gate.SetStopPathHook([&]() {
        std::lock_guard<std::mutex> lock(events.mutex);
        events.stopPathReached = true;
        events.cv.notify_all();
    });

    std::thread worker([&]() {
        int taskId = -1;
        static_cast<void>(gate.WaitForTask(taskId));
        {
            std::lock_guard<std::mutex> lock(events.mutex);
            events.workerReturned = true;
        }
        events.cv.notify_all();
    });

    const char* failure = nullptr;
    std::thread stopper;

    {
        std::unique_lock<std::mutex> lock(events.mutex);
        if (!events.cv.wait_for(lock, kWatchdog, [&]() { return events.workerAtStopPoint; })) {
            failure = "worker never reached the pre-wait stop point";
        }
    }

    if (failure == nullptr) {
        stopper = std::thread([&]() {
            if (delayedStopper) {
                // Deliberately later than the old 500ms window. This is only
                // scheduling perturbation; control flow never branches on it.
                std::this_thread::sleep_for(std::chrono::milliseconds(600));
            }
#if defined(ARIS_TASK_QUEUE_TEST_FAULT_SKIP_STOP)
            // Fault injection: the stopper never reaches the gate at all, so
            // no stop is published and no stop-path event is reported.
#else
            gate.RequestStop();
#endif
        });

        std::unique_lock<std::mutex> lock(events.mutex);
        if (!events.cv.wait_for(lock, kWatchdog, [&]() { return events.stopPathReached; })) {
            failure = "stop path never reached its handshake point";
        }
    }

    if (failure == nullptr) {
        std::unique_lock<std::mutex> lock(events.mutex);
        if (!events.cv.wait_for(lock, kWatchdog, [&]() { return events.workerLeftStopPoint; })) {
            failure = "worker did not leave the pre-wait stop point";
        }
    }

    // Decide pass/fail from the handshake before any rescue runs, so a rescue
    // can never mask a regression.
    const bool publishedEarly = publishedWhileHeld.load();

    bool observedReturn = false;
    if (failure == nullptr) {
        if (expectLegacyHang) {
            // Early publish and early notify are already proven at the park
            // point. Wake through the same wait lock and join; a hang is not a
            // required condition because a condition variable may wake
            // spuriously.
            gate.ForceStopAndNotifyForTesting();
        } else if (publishedEarly) {
            failure = "stop was published while the worker still held the mutex";
            gate.ForceStopAndNotifyForTesting();
        } else {
            std::unique_lock<std::mutex> lock(events.mutex);
            observedReturn = events.cv.wait_for(lock, kWatchdog, [&]() { return events.workerReturned; });
            if (!observedReturn) {
                failure = "worker missed the stop notification and stayed blocked";
            }
        }
    }

    if (failure != nullptr) {
        // Release the park point first, so a thread parked while holding the
        // gate mutex can never block the reclamation below.
        {
            std::lock_guard<std::mutex> lock(events.mutex);
            events.stopPathReached = true;
        }
        events.cv.notify_all();
        // Reclaim through the gate's own wait lock instead of the behaviour
        // under test: publish the stop and wake from under the same mutex.
        gate.ForceStopAndNotifyForTesting();
    }

    if (stopper.joinable()) stopper.join();
    worker.join();

    if (failure != nullptr) return Fail(failure);

    if (expectLegacyHang) {
        if (!publishedEarly) {
            return Fail("legacy stop did not publish while the worker held the mutex");
        }
        return 0;
    }

    if (!observedReturn) return Fail("worker did not return after the stop");
    if (publishedEarly) return Fail("stop was published while the worker still held the mutex");
    if (!gate.IsStopRequested()) return Fail("stop flag was not published");

    // Stop wins over queued work: a task pushed after the stop is not taken.
    gate.Push(7);
    int taskId = -1;
    if (gate.WaitForTask(taskId)) return Fail("queued task was taken after stop");
    if (gate.QueueSize() != 1) return Fail("queued task was dropped by the stop");
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    bool expectLegacyHang = false;
    bool delayedStopper = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--expect-legacy-hang") == 0) {
            expectLegacyHang = true;
        } else if (std::strcmp(argv[i], "--delayed-stopper") == 0) {
            delayedStopper = true;
        }
    }
    const int result = RunScenario(expectLegacyHang, delayedStopper);
    if (result == 0) {
        std::puts(expectLegacyHang
                      ? "AITaskQueueStopGate legacy lost-wakeup window reproduced"
                      : "AITaskQueueStopGate stop window tests passed");
    }
    return result;
}
