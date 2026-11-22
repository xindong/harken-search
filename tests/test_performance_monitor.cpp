#include "PerformanceMonitor.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <optional>

namespace {

using namespace std::chrono_literals;
using harken::performance_monitor::Monitor;
using harken::performance_monitor::ProcessSnapshot;

int failures = 0;

void Check(bool ok, const char* name) {
    if (!ok) {
        ++failures;
        std::printf("FAIL %s\n", name);
    }
}

bool Near(const std::optional<double>& actual, double expected) {
    return actual && std::abs(*actual - expected) < 0.001;
}

using Chart = std::array<std::optional<float>, 40>;

Chart OnlyNewest(float frameMs) {
    Chart chart{};
    chart.back() = frameMs;
    return chart;
}

struct Fixture {
    Monitor monitor;
    ProcessSnapshot snapshot{10'000'000, 4, 512'000'000};
    int reads = 0;

    void Frame(std::chrono::milliseconds elapsed) {
        monitor.Tick(Monitor::TimePoint{} + elapsed, [&] {
            ++reads;
            return snapshot;
        });
    }
};

void TestFirstIntervalPublishesImmediately() {
    Fixture fixture;
    auto& monitor = fixture.monitor;
    Check(!monitor.IsOpen(), "monitor starts hidden");
    fixture.Frame(0ms);
    Check(fixture.reads == 0 && monitor.FrameTimeBuckets() == Chart{}, "hidden monitor does not sample");
    monitor.SetOpen(true);
    fixture.Frame(100ms);
    Check(!monitor.AverageFrameTimeMs() && !monitor.PeakFrameTimeMs() && !monitor.FramesPerSecond() &&
              monitor.FrameTimeBuckets() == Chart{}, "first frame has no interval");
    fixture.Frame(108ms);
    Check(Near(monitor.AverageFrameTimeMs(), 8.0) && Near(monitor.PeakFrameTimeMs(), 8.0) &&
              Near(monitor.FramesPerSecond(), 125.0) && monitor.FrameTimeBuckets() == OnlyNewest(8.0f),
          "the first real interval publishes all frame data immediately");
    fixture.Frame(108ms);
    Check(fixture.reads == 1 && monitor.FrameTimeBuckets() == OnlyNewest(8.0f) &&
              Near(monitor.FramesPerSecond(), 125.0), "same timestamp does not fabricate a frame");
}

void TestHistoryUsesCompletionTimeAtDifferentFrameRates() {
    for (const auto cadence : {10ms, 50ms}) {
        Fixture fixture;
        fixture.monitor.SetOpen(true);
        fixture.Frame(0ms);
        fixture.Frame(100ms);
        for (auto elapsed = 100ms + cadence; elapsed <= 5100ms; elapsed += cadence) {
            if (elapsed <= 3100ms || elapsed >= 3250ms) fixture.Frame(elapsed);
        }
        const auto chart = fixture.monitor.FrameTimeBuckets();
        const float regular = static_cast<float>(cadence.count());
        Check(!chart[18] && chart[19] == 100.0f && chart.back() == regular,
              "five seconds occupy the same chart width at 20 and 100 FPS");
        Check(chart[31] == regular && chart[32] == 150.0f && chart[33] == regular,
              "a spike on a bucket boundary belongs to the bucket in which it completes");
        Check(fixture.monitor.ChartCeilingMs() == 200.0f, "the chart has a discrete scale that fits its visible peak");

        for (auto elapsed = 5100ms + cadence; elapsed <= 6100ms; elapsed += cadence) fixture.Frame(elapsed);
        Check(fixture.monitor.FrameTimeBuckets()[28] == 150.0f && fixture.monitor.FrameTimeBuckets()[32] == regular,
              "one second moves the spike four buckets left without copying it");

        for (auto elapsed = 6100ms + cadence; elapsed <= 13100ms; elapsed += cadence) fixture.Frame(elapsed);
        Check(fixture.monitor.FrameTimeBuckets().front() == 150.0f, "the oldest of 40 buckets still shows its peak");
        for (auto elapsed = 13100ms + cadence; elapsed <= 14100ms; elapsed += cadence) fixture.Frame(elapsed);
        const auto expired = fixture.monitor.FrameTimeBuckets();
        Check(std::all_of(expired.begin(), expired.end(), [&](auto bucket) { return bucket == regular; }),
              "expired peaks do not survive a wrap of the time buckets");
        Check(fixture.monitor.ChartCeilingMs() == 50.0f, "typical peaks use the same base scale at either frame rate");
    }
}

void TestDisplayCadenceKeepsFrameSamples() {
    Fixture fixture;
    auto& monitor = fixture.monitor;
    monitor.SetOpen(true);
    fixture.Frame(0ms);
    fixture.Frame(10ms);
    for (auto elapsed = 20ms; elapsed <= 500ms; elapsed += 10ms) {
        if (elapsed <= 100ms || elapsed >= 200ms) fixture.Frame(elapsed);
    }
    fixture.Frame(509ms);
    fixture.Frame(510ms);
    Check(monitor.FrameTimeBuckets() == OnlyNewest(10.0f), "chart no longer refreshes at half a second");

    for (auto elapsed = 520ms; elapsed <= 1000ms; elapsed += 10ms) fixture.Frame(elapsed);
    fixture.Frame(1009ms);
    Check(monitor.FrameTimeBuckets() == OnlyNewest(10.0f) && Near(monitor.AverageFrameTimeMs(), 10.0) &&
              Near(monitor.PeakFrameTimeMs(), 10.0) && Near(monitor.FramesPerSecond(), 100.0),
          "chart, average, peak and FPS hold until one second after the first interval");
    fixture.Frame(1010ms);
    const auto chart = monitor.FrameTimeBuckets();
    Check(chart[35] == 100.0f && chart.back() == 10.0f && Near(monitor.AverageFrameTimeMs(), 1000.0 / 93) &&
              Near(monitor.PeakFrameTimeMs(), 100.0) && Near(monitor.FramesPerSecond(), 93.0),
          "one refresh publishes the chart and statistics from all 93 completed intervals");

    fixture.Frame(1027ms);
    fixture.Frame(2009ms);
    Check(monitor.FrameTimeBuckets() == chart && Near(monitor.AverageFrameTimeMs(), 1000.0 / 93) &&
              Near(monitor.PeakFrameTimeMs(), 100.0) && Near(monitor.FramesPerSecond(), 93.0),
          "an unpublished slow frame does not change any displayed frame data");
    fixture.Frame(2010ms);
    Check(monitor.FrameTimeBuckets().back() == 982.0f && Near(monitor.AverageFrameTimeMs(), 1000.0 / 3) &&
              Near(monitor.PeakFrameTimeMs(), 982.0) && Near(monitor.FramesPerSecond(), 3.0),
          "the next refresh counts exactly the intervals since the previous refresh");
}

void TestUncappedFrameRateKeepsPeakAtItsTime() {
    Fixture fixture;
    auto& monitor = fixture.monitor;
    monitor.SetOpen(true);
    fixture.Frame(0ms);
    fixture.Frame(1ms);
    for (auto elapsed = 2ms; elapsed <= 1001ms; elapsed += 1ms) {
        if (elapsed <= 300ms || elapsed >= 400ms) fixture.Frame(elapsed);
    }
    const auto chart = monitor.FrameTimeBuckets();
    Check(!chart[34] && chart[35] == 1.0f && chart[36] == 100.0f && chart[37] == 1.0f && chart.back() == 1.0f,
          "more than 120 later frames do not overwrite or move the peak out of its completion bucket");
    Check(Near(monitor.AverageFrameTimeMs(), 1000.0 / 901) && Near(monitor.PeakFrameTimeMs(), 100.0) &&
              Near(monitor.FramesPerSecond(), 901.0), "uncapped FPS and statistics include every real interval");

    for (auto elapsed = 1002ms; elapsed <= 2001ms; elapsed += 1ms) fixture.Frame(elapsed);
    Check(monitor.FrameTimeBuckets()[32] == 100.0f && Near(monitor.AverageFrameTimeMs(), 1.0) && Near(monitor.PeakFrameTimeMs(), 1.0) &&
              Near(monitor.FramesPerSecond(), 1000.0), "the chart's earlier peak does not leak into new statistics");
}

void TestLongIntervalsLeaveEmptyBuckets() {
    Fixture fixture;
    auto& monitor = fixture.monitor;
    monitor.SetOpen(true);
    fixture.Frame(0ms);
    fixture.Frame(100ms);
    fixture.Frame(200ms);
    fixture.Frame(25350ms);
    Check(monitor.FrameTimeBuckets() == OnlyNewest(25150.0f), "a stall beyond the chart horizon leaves only its completion bucket");
    Check(Near(monitor.AverageFrameTimeMs(), 12625.0) && Near(monitor.PeakFrameTimeMs(), 25150.0) &&
              Near(monitor.FramesPerSecond(), 2000.0 / 25250), "statistics span the real time between refreshes even after a stall");

    fixture.Frame(26400ms);
    auto separated = OnlyNewest(1050.0f);
    separated[35] = 25150.0f;
    Check(monitor.FrameTimeBuckets() == separated, "two real completions separated by a stall leave empty slots between them");
}

void TestProcessCadenceAndNormalization() {
    Fixture fixture;
    auto& monitor = fixture.monitor;
    monitor.SetOpen(true);
    fixture.Frame(0ms);
    Check(fixture.reads == 1 && !monitor.CpuPercent(), "CPU needs two process readings");
    Check(monitor.PrivateUsageBytes() == 512'000'000, "memory is ready from the first snapshot");
    fixture.snapshot = {15'000'000, 4, 550'000'000};
    fixture.Frame(999ms);
    Check(fixture.reads == 1 && monitor.PrivateUsageBytes() == 512'000'000,
          "process data is not polled at UI frame rate");
    fixture.Frame(1000ms);
    Check(fixture.reads == 2 && Near(monitor.CpuPercent(), 12.5),
          "process CPU is normalized across all logical processors");
    Check(monitor.PrivateUsageBytes() == 550'000'000, "memory reflects latest private usage");
    fixture.snapshot = {35'000'000, 4, 555'000'000};
    fixture.Frame(3000ms);
    Check(fixture.reads == 3 && Near(monitor.CpuPercent(), 25.0),
          "late poll uses real elapsed time without catch-up calls");
    fixture.snapshot = {35'000'000, 4, 0};
    fixture.Frame(4000ms);
    Check(Near(monitor.CpuPercent(), 0.0) && monitor.PrivateUsageBytes() == 0,
          "a legitimate zero is different from missing data");
}

void TestCloseAndResume() {
    Fixture fixture;
    auto& monitor = fixture.monitor;
    monitor.SetOpen(true);
    fixture.Frame(0ms);
    fixture.Frame(1000ms);
    monitor.SetOpen(false);
    fixture.Frame(20000ms);
    Check(fixture.reads == 2 && monitor.FrameTimeBuckets() == Chart{} && !monitor.AverageFrameTimeMs() &&
              !monitor.PeakFrameTimeMs() && !monitor.FramesPerSecond() && !monitor.PrivateUsageBytes() &&
              !monitor.CpuPercent(), "closing clears data and stops sampling");

    monitor.SetOpen(true);
    fixture.Frame(21000ms);
    Check(fixture.reads == 3 && !monitor.AverageFrameTimeMs() && !monitor.CpuPercent(),
          "reopening starts new frame and CPU baselines");
    fixture.Frame(21016ms);
    Check(Near(monitor.AverageFrameTimeMs(), 16.0) && Near(monitor.PeakFrameTimeMs(), 16.0) && monitor.FrameTimeBuckets() == OnlyNewest(16.0f),
          "reopened history excludes time spent closed");

    fixture.Frame(21166ms);
    monitor.Pause();
    monitor.Pause();
    Check(monitor.IsOpen() && monitor.FrameTimeBuckets() == Chart{} && !monitor.PeakFrameTimeMs() &&
              !monitor.FramesPerSecond() && !monitor.PrivateUsageBytes(),
          "parking clears samples without closing the monitor");
    fixture.Frame(61000ms);
    Check(fixture.reads == 4 && !monitor.AverageFrameTimeMs() && !monitor.CpuPercent(),
          "resuming starts fresh baselines after the gap");
    fixture.Frame(61020ms);
    Check(Near(monitor.AverageFrameTimeMs(), 20.0) && Near(monitor.PeakFrameTimeMs(), 20.0) && monitor.FrameTimeBuckets() == OnlyNewest(20.0f),
          "resumed graph excludes time spent parked");
}

void TestPartialAndInvalidSnapshots() {
    Fixture fixture;
    auto& monitor = fixture.monitor;
    monitor.SetOpen(true);
    fixture.Frame(0ms);
    fixture.snapshot = {std::nullopt, 4, 42};
    fixture.Frame(1000ms);
    Check(!monitor.CpuPercent() && monitor.PrivateUsageBytes() == 42, "missing CPU does not hide memory");
    fixture.snapshot = {15'000'000, 4, std::nullopt};
    fixture.Frame(2000ms);
    Check(!monitor.CpuPercent() && !monitor.PrivateUsageBytes(), "CPU recovers with a new baseline, memory clears on failure");
    fixture.snapshot = {20'000'000, 4, std::nullopt};
    fixture.Frame(3000ms);
    Check(Near(monitor.CpuPercent(), 12.5), "CPU recovers independently from memory");
    fixture.snapshot = {19'000'000, 4, 42};
    fixture.Frame(4000ms);
    Check(!monitor.CpuPercent(), "a decreasing CPU counter is unavailable, not an underflow");
    fixture.snapshot = {25'000'000, 0, 42};
    fixture.Frame(5000ms);
    Check(!monitor.CpuPercent(), "unknown logical processor count does not divide by zero");
    fixture.snapshot = {30'000'000, 4, 42};
    fixture.Frame(6000ms);
    Check(!monitor.CpuPercent(), "a valid processor count starts a fresh CPU baseline");
    fixture.snapshot = {35'000'000, 8, 42};
    fixture.Frame(7000ms);
    Check(!monitor.CpuPercent(), "a changed processor count starts a fresh CPU baseline");
}

void TestBackwardClockStartsFreshBaselines() {
    Fixture fixture;
    auto& monitor = fixture.monitor;
    monitor.SetOpen(true);
    fixture.Frame(0ms);
    fixture.Frame(16ms);
    fixture.Frame(1016ms);
    fixture.Frame(-270ms);
    Check(!monitor.AverageFrameTimeMs() && !monitor.PeakFrameTimeMs() && !monitor.FramesPerSecond() &&
              monitor.FrameTimeBuckets() == Chart{} && !monitor.CpuPercent() && fixture.reads == 3,
          "a backward clock clears the old samples and takes a new process baseline");
    fixture.Frame(-260ms);
    Check(Near(monitor.AverageFrameTimeMs(), 10.0) && Near(monitor.PeakFrameTimeMs(), 10.0) && monitor.FrameTimeBuckets() == OnlyNewest(10.0f),
          "the first forward interval after rollback is real and publishes immediately");
    fixture.Frame(740ms);
    auto chart = OnlyNewest(1000.0f);
    chart[35] = 10.0f;
    Check(monitor.FrameTimeBuckets() == chart, "bucket boundaries also hold when the clock epoch is crossed");
}

}

int main() {
    const std::array tests{
        TestFirstIntervalPublishesImmediately,
        TestHistoryUsesCompletionTimeAtDifferentFrameRates,
        TestDisplayCadenceKeepsFrameSamples,
        TestUncappedFrameRateKeepsPeakAtItsTime,
        TestLongIntervalsLeaveEmptyBuckets,
        TestProcessCadenceAndNormalization,
        TestCloseAndResume,
        TestPartialAndInvalidSnapshots,
        TestBackwardClockStartsFreshBaselines,
    };
    std::size_t failedCases = 0;
    for (const auto test : tests) {
        const int previousFailures = failures;
        test();
        if (failures != previousFailures) ++failedCases;
    }
    std::printf("%s performance monitor sampling (%zu/%zu cases passed; %d failed checks)\n",
                failures == 0 ? "PASS" : "FAIL", tests.size() - failedCases, tests.size(), failures);
    return failures == 0 ? 0 : 1;
}
