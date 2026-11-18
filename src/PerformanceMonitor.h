#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace harken::performance {

enum class Module { Interface, Scan, Search, Similarity, AudioDecode, Waveform, Cover, Backfill, Count };
constexpr auto ModuleCount = static_cast<std::size_t>(Module::Count);

#ifdef HARKEN_PERFORMANCE_MONITOR

struct ModuleSample {
    unsigned active = 0;
    double cpuMilliseconds = 0;
};

struct Snapshot {
    std::optional<double> cpuPercent, readBytesPerSecond, writeBytesPerSecond;
    std::optional<std::uint64_t> workingSetBytes, privateBytes;
    std::array<ModuleSample, ModuleCount> modules{};
    bool hasModuleInterval = false;
};

// Open/close and refresh are owned by the UI thread. The snapshot is borrowed
// until the next UI refresh or reset. A worker scope must end on its own thread.
void SetEnabled(bool enabled);
const Snapshot& Refresh();

class Scope {
public:
    explicit Scope(Module module);
    ~Scope();
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

private:
    friend struct Monitor;
    Module module_;
    void* thread_ = nullptr;
    Scope* parent_ = nullptr;
    Scope* next_ = nullptr;
    Scope** link_ = nullptr;
    std::optional<std::uint64_t> lastCpu_;
    bool running_ = false;
};

#else

class Scope {
public:
    explicit Scope(Module) {}
};

#endif
} // namespace harken::performance
