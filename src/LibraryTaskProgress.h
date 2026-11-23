#pragma once

#include <chrono>
#include <cstdint>
#include <future>
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
