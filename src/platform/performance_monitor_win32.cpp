#include "../PerformanceMonitor.h"
#include "system.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <atomic>
#include <chrono>
#include <mutex>

namespace harken::performance {
namespace {
using Clock = std::chrono::steady_clock;

std::uint64_t Ticks(FILETIME value) {
    return (static_cast<std::uint64_t>(value.dwHighDateTime) << 32) | value.dwLowDateTime;
}

std::optional<std::uint64_t> ThreadCpu(HANDLE thread) {
    if (!thread) return {};
    FILETIME created, exited, kernel, user;
    if (!GetThreadTimes(thread, &created, &exited, &kernel, &user)) return {};
    return Ticks(kernel) + Ticks(user);
}

struct ThreadHandle {
    HANDLE value = nullptr;
    ThreadHandle() {
        DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(),
                        &value, 0, FALSE, DUPLICATE_SAME_ACCESS);
    }
    ~ThreadHandle() { if (value) CloseHandle(value); }
};

struct ProcessCounters {
    Clock::time_point time = Clock::now();
    std::optional<std::uint64_t> cpu, read, write;
};

ProcessCounters ReadProcessCounters() {
    ProcessCounters result;
    HANDLE process = GetCurrentProcess();
    FILETIME created, exited, kernel, user;
    if (GetProcessTimes(process, &created, &exited, &kernel, &user))
        result.cpu = Ticks(kernel) + Ticks(user);
    IO_COUNTERS io{};
    if (GetProcessIoCounters(process, &io)) {
        result.read = io.ReadTransferCount;
        result.write = io.WriteTransferCount;
    }
    return result;
}

std::optional<double> Rate(std::optional<std::uint64_t> oldValue,
                           std::optional<std::uint64_t> newValue, double seconds) {
    if (!oldValue || !newValue || *newValue < *oldValue || seconds <= 0) return {};
    return static_cast<double>(*newValue - *oldValue) / seconds;
}
} // namespace

struct Monitor {
    std::atomic<bool> enabled{false};
    std::array<std::uint64_t, ModuleCount> cpu{};
    std::mutex mutex;
    Scope* first = nullptr;
    std::optional<ProcessCounters> previous;
    Snapshot snapshot;

    void Checkpoint(Scope& scope) {
        auto now = ThreadCpu(scope.thread_);
        if (now && scope.lastCpu_ && *now >= *scope.lastCpu_)
            cpu[static_cast<std::size_t>(scope.module_)] += *now - *scope.lastCpu_;
        scope.lastCpu_ = now;
    }

    void ReadModules(bool hasInterval) {
        std::lock_guard<std::mutex> lock(mutex);
        for (auto& module : snapshot.modules) module.active = 0;
        for (Scope* scope = first; scope; scope = scope->next_) {
            if (!scope->running_) continue;
            Checkpoint(*scope);
            ++snapshot.modules[static_cast<std::size_t>(scope->module_)].active;
        }
        for (std::size_t i = 0; i < ModuleCount; ++i)
            snapshot.modules[i].cpuMilliseconds = hasInterval ? cpu[i] / 10000.0 : 0;
        snapshot.hasModuleInterval = hasInterval;
        cpu.fill(0);
    }
};

namespace {
Monitor monitor;
thread_local Scope* currentScope = nullptr;
} // namespace

Scope::Scope(Module module) : module_(module) {
    // Keep worker scopes visible if the window opens during a long task.
    if (module == Module::Interface && !monitor.enabled.load(std::memory_order_relaxed)) return;

    thread_local ThreadHandle handle;
    std::lock_guard<std::mutex> lock(monitor.mutex);
    const bool sampling = monitor.enabled.load(std::memory_order_relaxed);

    thread_ = handle.value;
    parent_ = currentScope;
    if (parent_) {
        if (sampling) monitor.Checkpoint(*parent_);
        parent_->running_ = false;
    }
    currentScope = this;
    link_ = &monitor.first;
    next_ = monitor.first;
    if (next_) next_->link_ = &next_;
    monitor.first = this;
    if (sampling) lastCpu_ = ThreadCpu(thread_);
    running_ = true;
}

Scope::~Scope() {
    if (!link_) return;
    std::lock_guard<std::mutex> lock(monitor.mutex);
    if (monitor.enabled.load(std::memory_order_relaxed)) monitor.Checkpoint(*this);
    *link_ = next_;
    if (next_) next_->link_ = link_;
    currentScope = parent_;
    if (parent_) {
        parent_->lastCpu_ = lastCpu_;
        parent_->running_ = true;
    }
}

void SetEnabled(bool enabled) {
    if (monitor.enabled.load(std::memory_order_relaxed) == enabled) return;
    std::lock_guard<std::mutex> lock(monitor.mutex);
    monitor.enabled.store(enabled, std::memory_order_relaxed);
    monitor.previous.reset();
    monitor.snapshot = {};
}

const Snapshot& Refresh() {
    using namespace std::chrono_literals;
    if (!monitor.enabled.load(std::memory_order_relaxed)) return monitor.snapshot;
    if (!monitor.previous || Clock::now() - monitor.previous->time >= 1s) {
        const auto now = ReadProcessCounters();
        const auto memory = Platform::QueryCurrentProcessMemoryUsage();
        auto& snapshot = monitor.snapshot;
        if (monitor.previous) {
            const auto& previous = *monitor.previous;
            const double seconds = std::chrono::duration<double>(now.time - previous.time).count();
            snapshot.cpuPercent = Rate(previous.cpu, now.cpu, seconds);
            static const DWORD processors = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
            if (snapshot.cpuPercent) *snapshot.cpuPercent /= 100000.0 * (processors ? processors : 1);
            snapshot.readBytesPerSecond = Rate(previous.read, now.read, seconds);
            snapshot.writeBytesPerSecond = Rate(previous.write, now.write, seconds);
        }
        const bool hasMemory = memory.status == Platform::ProcessMemoryUsageStatus::Success;
        snapshot.workingSetBytes = hasMemory ? std::optional{memory.workingSetBytes} : std::nullopt;
        snapshot.privateBytes = hasMemory ? std::optional{memory.privateUsageBytes} : std::nullopt;
        monitor.ReadModules(monitor.previous.has_value());
        monitor.previous = now;
    }
    return monitor.snapshot;
}

} // namespace harken::performance
