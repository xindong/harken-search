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
    // Regression harness: invoked once while the worker holds the mutex,
    // immediately before it blocks. It must not call RequestStop itself.
    using BeforeWaitHook = std::function<void()>;
    void SetBeforeWaitHook(BeforeWaitHook hook);
#endif

private:
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::queue<int> queue_;
    std::atomic<bool> stopRequested_{false};
#if defined(ARIS_TASK_QUEUE_TESTING)
    BeforeWaitHook beforeWaitHook_;
#endif
};
