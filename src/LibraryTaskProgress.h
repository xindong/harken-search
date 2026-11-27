#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace HarkenSearch {

template <class Result>
class FolderRemovalTask {
public:
    struct Completion {
        std::string path;
        Result result;
    };

    template <class Work>
    bool Start(int64_t libraryGeneration, const std::string& path, Work work) {
        if (Active()) return false;
        auto pending = std::async(std::launch::async,
            [path, work = std::move(work)]() mutable { return Completion{path, work(path)}; });
        generation_ = libraryGeneration;
        future_ = std::move(pending);
        return true;
    }

    bool Active() const { return future_.valid(); }
    void Wait() const { if (Active()) future_.wait(); }

    std::optional<Completion> Poll(int64_t libraryGeneration) {
        if (!Active() || future_.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
            return std::nullopt;
        if (generation_ != libraryGeneration) {
            future_ = {};
            return std::nullopt;
        }
        return future_.get();
    }

private:
    int64_t generation_ = -1;
    std::future<Completion> future_;
};

enum class LibraryTaskPhase { Idle, Running, Succeeded, NoMatch, Stopped, Failed };

// 批量任务进度快照：工作线程写原子计数，界面线程每帧读一份走。
struct BatchTaskProgressSnapshot {
    bool active = false;
    bool verifying = false;     // 还在预检（网络库/路径），此时没有可信百分比
    bool cancelRequested = false;
    int done = 0;
    int total = 0;
};

// 批量改元数据这类「后台跑、只在结束时回填界面」的任务。
// 与 FolderRemovalTask 同一套做法（std::async + 结果交给界面线程取），
// 多出来的是一份可随时读的进度快照和取消位。
template <class Result>
class BatchBackgroundTask {
public:
    // work(progressSink, cancelFlag) 在工作线程执行，不得碰界面状态。
    using ProgressSink = std::function<void(int done, int total, bool verifying)>;
    using Work = std::function<Result(ProgressSink&, std::atomic<bool>*)>;

    bool Start(Work work) {
        if (Active()) return false;
        cancel_.store(false, std::memory_order_relaxed);
        done_.store(0, std::memory_order_relaxed);
        total_.store(0, std::memory_order_relaxed);
        verifying_.store(false, std::memory_order_relaxed);
        error_.store(false, std::memory_order_relaxed);
        failed_ = false;
        active_.store(true, std::memory_order_release);
        ProgressSink sink = [this](int done, int total, bool verifying) {
            done_.store(done, std::memory_order_relaxed);
            total_.store(total, std::memory_order_relaxed);
            verifying_.store(verifying, std::memory_order_relaxed);
        };
        std::atomic<bool>* cancelFlag = &cancel_;
        errorText_ = std::make_shared<std::string>();
        auto message = errorText_;
        auto* errorFlag = &error_;
        future_ = std::async(std::launch::async,
            [work = std::move(work), sink, cancelFlag, message, errorFlag]() mutable {
                try {
                    return work(sink, cancelFlag);
                } catch (const std::exception& e) {
                    *message = e.what();
                    *errorFlag = true;
                } catch (...) {
                    *message = "unknown error";
                    *errorFlag = true;
                }
                return Result{};
            });
        return true;
    }

    bool Active() const { return future_.valid(); }

    BatchTaskProgressSnapshot Snapshot() const {
        BatchTaskProgressSnapshot snapshot;
        snapshot.active = active_.load(std::memory_order_acquire);
        snapshot.verifying = verifying_.load(std::memory_order_relaxed);
        snapshot.cancelRequested = cancel_.load(std::memory_order_relaxed);
        snapshot.done = done_.load(std::memory_order_relaxed);
        snapshot.total = total_.load(std::memory_order_relaxed);
        return snapshot;
    }

    void RequestCancel() { cancel_.store(true, std::memory_order_relaxed); }

    // 成功后取走一次结果；未完成返回 nullopt。
    std::optional<Result> Poll() {
        if (!Active()) return std::nullopt;
        if (future_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return std::nullopt;
        Result result = future_.get();
        active_.store(false, std::memory_order_release);
        verifying_.store(false, std::memory_order_relaxed);
        failed_ = error_.load(std::memory_order_relaxed);
        return result;
    }

    // 工作线程抛异常时，Poll 返回的结果是默认值；界面必须看这个标记。
    bool Failed() const { return failed_; }
    std::string FailureText() const { return errorText_ ? *errorText_ : std::string(); }

    void Join() { if (future_.valid()) future_.wait(); }

private:
    std::atomic<bool> active_{false};
    std::atomic<bool> verifying_{false};
    std::atomic<bool> cancel_{false};
    std::atomic<bool> error_{false};
    std::atomic<int> done_{0};
    std::atomic<int> total_{0};
    bool failed_ = false;
    std::shared_ptr<std::string> errorText_;
    std::future<Result> future_;
};

class LibraryTaskNotice {
public:
    bool Update(uint64_t task, LibraryTaskPhase phase, bool originalVisible, double now) {
        if (task != task_ || phase != phase_) {
            task_ = task;
            phase_ = phase;
            dismissed_ = false;
            until_ = now + 4.0;
        }
        if (phase == LibraryTaskPhase::Idle) return false;
        if (phase == LibraryTaskPhase::Running) return !originalVisible;
        if (originalVisible) dismissed_ = true;
        return !dismissed_ && (phase == LibraryTaskPhase::Failed || now < until_);
    }

    void Dismiss() { dismissed_ = true; }

private:
    uint64_t task_ = 0;
    LibraryTaskPhase phase_ = LibraryTaskPhase::Idle;
    double until_ = 0.0;
    bool dismissed_ = false;
};

}
