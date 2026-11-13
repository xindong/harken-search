#pragma once
#include <algorithm>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

// Instrumented UI operations, not a native stack trace. No file I/O occurs
// under this lock; a logger thread can sample while the UI itself is blocked.
class DiagnosticUiOperations {
public:
    struct Operation {
        std::uint64_t id = 0, parent = 0, frame = 0;
        std::int64_t startedAt = 0;
        double started = 0, duration = 0, children = 0, self = 0;
        std::string name;
    };
    void nextFrame() {
        std::lock_guard<std::mutex> lock(mutex_);
        ++frame_;
        stack_.clear();
    }
    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        stack_.clear();
        lastSample_ = -1e20;
    }
    std::uint64_t begin(const char* name, double now, std::int64_t wall) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stack_.size() >= 32) return 0;
        Operation op;
        op.id = ++sequence_;
        op.parent = stack_.empty() ? 0 : stack_.back().id;
        op.frame = frame_; op.started = now; op.startedAt = wall;
        op.name = name ? name : "unknown";
        stack_.push_back(op);
        return op.id;
    }
    Operation end(std::uint64_t id, double now) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!id || stack_.empty() || stack_.back().id != id) return {};
        Operation op = stack_.back(); stack_.pop_back();
        op.duration = std::max(0.0, now - op.started);
        op.self = std::max(0.0, op.duration - op.children);
        if (!stack_.empty()) stack_.back().children += op.duration;
        return op;
    }
    std::vector<Operation> sample(double now) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stack_.empty() || now - stack_.front().started < 500.0 ||
            now - lastSample_ < 1000.0) return {};
        lastSample_ = now;
        auto result = stack_;
        for (auto& op : result) op.duration = std::max(0.0, now - op.started);
        return result;
    }
private:
    std::mutex mutex_;
    std::vector<Operation> stack_;
    std::uint64_t sequence_ = 0, frame_ = 0;
    double lastSample_ = -1e20;
};
