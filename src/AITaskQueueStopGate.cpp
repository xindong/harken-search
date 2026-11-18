#include "AITaskQueueStopGate.h"

#include <utility>

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
            // release-and-block. A correct RequestStop cannot publish while
            // this hook runs, so the wakeup can never be lost.
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
    // Regression harness only: the previous implementation published the stop
    // flag outside the mutex. A worker that already decided to block can miss
    // this notification and wait forever.
    stopRequested_.store(true);
    condition_.notify_all();
#else
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
#endif
