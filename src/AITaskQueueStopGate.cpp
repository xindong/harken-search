#include "AITaskQueueStopGate.h"

#include <utility>

namespace {

#if defined(ARIS_TASK_QUEUE_TESTING)
template <typename Hook>
void RunOnce(Hook& slot) {
    if (!slot) return;
    Hook hook = std::move(slot);
    slot = nullptr;
    hook();
}
#endif

}  // namespace

void AITaskQueueStopGate::Reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    stopRequested_.store(false);
}

void AITaskQueueStopGate::Push(int taskId) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.push(taskId);
    }
    condition_.notify_one();
}

bool AITaskQueueStopGate::WaitForTask(int& taskId) {
    std::unique_lock<std::mutex> lock(mutex_);
    while (!stopRequested_.load() && queue_.empty()) {
#if defined(ARIS_TASK_QUEUE_TESTING)
        if (beforeWaitHook_) {
            BeforeWaitHook hook = std::move(beforeWaitHook_);
            beforeWaitHook_ = nullptr;
            // Runs with mutex_ held, after the wait decision and before the
            // release-and-block. Guarded RequestStop cannot publish while this
            // hook runs, so the wakeup can never be lost.
            hook();
        }
#endif
        condition_.wait(lock);
    }
    if (stopRequested_.load()) return false;
    taskId = queue_.front();
    queue_.pop();
    return true;
}

void AITaskQueueStopGate::RequestStop() {
#if defined(ARIS_TASK_QUEUE_REGRESSION_LEGACY_STOP)
    // Regression test only: the previous implementation published the stop
    // flag outside the mutex. A worker that already decided to block can miss
    // this notification and wait forever.
    stopRequested_.store(true);
    condition_.notify_all();
#if defined(ARIS_TASK_QUEUE_TESTING)
    RunOnce(stopPathHook_);
#endif
#else
#if defined(ARIS_TASK_QUEUE_TESTING)
    // Probe the same mutex the worker holds at its wait decision. Only a
    // failed try-lock is real contention and gets reported; a successful probe
    // is released immediately and is never reported as contention.
    if (!mutex_.try_lock()) {
        RunOnce(stopPathHook_);
    } else {
        mutex_.unlock();
    }
#endif
    // The shipped path, shared by production and the tests.
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopRequested_.store(true);
    }
    condition_.notify_all();
#endif
}

bool AITaskQueueStopGate::IsStopRequested() const {
    return stopRequested_.load();
}

std::size_t AITaskQueueStopGate::QueueSize() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.size();
}

#if defined(ARIS_TASK_QUEUE_TESTING)
void AITaskQueueStopGate::SetBeforeWaitHook(BeforeWaitHook hook) {
    std::lock_guard<std::mutex> lock(mutex_);
    beforeWaitHook_ = std::move(hook);
}

void AITaskQueueStopGate::SetStopPathHook(StopPathHook hook) {
    std::lock_guard<std::mutex> lock(mutex_);
    stopPathHook_ = std::move(hook);
}

void AITaskQueueStopGate::NotifyWaitersForTesting() {
    std::lock_guard<std::mutex> lock(mutex_);
    condition_.notify_all();
}

void AITaskQueueStopGate::ForceStopAndNotifyForTesting() {
    std::lock_guard<std::mutex> lock(mutex_);
    stopRequested_.store(true);
    condition_.notify_all();
}
#endif
