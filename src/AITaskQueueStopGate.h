#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <queue>

#if defined(ARIS_TASK_QUEUE_TESTING)
#include <functional>
#endif

// Coordinates the AI task worker's stop request with its wait.
//
// The stop flag, the queue and the wait predicate are guarded by one mutex.
// RequestStop publishes the flag while holding that mutex, so a worker that
// has already decided to block cannot miss the notification: it either sees
// the flag before blocking or is woken after it blocks.
//
// Stop wins over queued work. Once a stop is requested WaitForTask returns
// false even when tasks remain, and the worker finishes only a task it had
// already taken.
class AITaskQueueStopGate {
public:
    AITaskQueueStopGate() = default;
    AITaskQueueStopGate(const AITaskQueueStopGate&) = delete;
    AITaskQueueStopGate& operator=(const AITaskQueueStopGate&) = delete;

    // Clears a previous stop request for a new worker lifetime. Queued task
    // ids are intentionally preserved, matching the pre-stop queue contents.
    void Reset();

    // Appends a task id and wakes the worker.
    void Push(int taskId);

    // Blocks until a task id is available or a stop is requested. Returns
    // false when the caller must leave the worker loop.
    bool WaitForTask(int& taskId);

    // Requests a stop. Safe against a worker that is deciding to block.
    void RequestStop();

    bool IsStopRequested() const;
    std::size_t QueueSize() const;

#if defined(ARIS_TASK_QUEUE_TESTING)
    // Regression test hook: invoked once while the worker holds the mutex,
    // after the wait decision and immediately before it blocks.
    using BeforeWaitHook = std::function<void()>;
    void SetBeforeWaitHook(BeforeWaitHook hook);

    // Regression test hook: invoked once by the stop path at the last moment it
    // can act while a worker holds the mutex. In the guarded path this runs
    // after a real try-lock on the shipped path's mutex fails, immediately
    // before the shared lock-guard block blocks on that same mutex.
    // In the legacy test path it runs immediately after publishing without
    // the mutex. It must not call back into this gate.
    using StopPathHook = std::function<void()>;
    void SetStopPathHook(StopPathHook hook);

    // Takes the same mutex the worker waits on and wakes every waiter. Used to
    // release a worker that is known to be blocked so the test can join it.
    void NotifyWaitersForTesting();

    // Publishes the stop and wakes every waiter from under the same
    // mutex, bypassing RequestStop. Used only to reclaim threads after a
    // failed handshake, so a rescue can never be mistaken for the
    // behaviour under test.
    void ForceStopAndNotifyForTesting();
#endif

private:
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::queue<int> queue_;
    std::atomic<bool> stopRequested_{false};
#if defined(ARIS_TASK_QUEUE_TESTING)
    BeforeWaitHook beforeWaitHook_;
    StopPathHook stopPathHook_;
#endif
};
