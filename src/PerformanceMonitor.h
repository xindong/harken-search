#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <ratio>

namespace harken::performance_monitor {

struct ProcessSnapshot {
    std::optional<std::uint64_t> cpuTime100ns;
    std::uint32_t logicalProcessorCount = 0;
    std::optional<std::uint64_t> privateUsageBytes;
};

class Monitor {
public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;
    static constexpr std::size_t kFrameBucketCount = 40;
    using FrameBuckets = std::array<std::optional<float>, kFrameBucketCount>;

    bool IsOpen() const { return open_; }

    void SetOpen(bool open) {
        if (open_ != open) {
            open_ = open;
            Pause();
        }
    }

    void Pause() {
        if (!lastFrame_) return;
        lastFrame_.reset();
        lastProcessRead_.reset();
        process_ = {};
        cpuPercent_.reset();
        liveBuckets_ = {};
        displayedBuckets_ = {};
        chartCeilingMs_ = kBaseChartCeilingMs;
        lastDisplayUpdate_.reset();
        intervalCount_ = 0;
        intervalPeakMs_ = 0.0;
        displayedAverageFrameTimeMs_.reset();
        displayedPeakFrameTimeMs_.reset();
        displayedFps_.reset();
    }

    template <typename ReadProcess>
    void Tick(TimePoint now, ReadProcess&& readProcess) {
        if (!open_) return;
        if (lastFrame_) {
            if (now == *lastFrame_) return;
            if (now < *lastFrame_) {
                Pause();
            } else {
                RecordInterval(*lastFrame_, now);
            }
        }
        lastFrame_ = now;

        if (!lastProcessRead_ || now - *lastProcessRead_ >= std::chrono::seconds(1)) {
            const ProcessSnapshot next = readProcess();
            cpuPercent_.reset();
            if (lastProcessRead_ && next.cpuTime100ns && process_.cpuTime100ns &&
                *next.cpuTime100ns >= *process_.cpuTime100ns && next.logicalProcessorCount > 0 &&
                next.logicalProcessorCount == process_.logicalProcessorCount) {
                const double wallSeconds = std::chrono::duration<double>(now - *lastProcessRead_).count();
                const double cpuSeconds = static_cast<double>(*next.cpuTime100ns - *process_.cpuTime100ns) * 1e-7;
                cpuPercent_ = std::clamp((100.0 * cpuSeconds) / (wallSeconds * next.logicalProcessorCount),
                                         0.0, 100.0);
            }
            process_ = next;
            lastProcessRead_ = now;
        }
    }

    std::optional<double> AverageFrameTimeMs() const { return displayedAverageFrameTimeMs_; }
    std::optional<double> PeakFrameTimeMs() const { return displayedPeakFrameTimeMs_; }
    std::optional<double> FramesPerSecond() const { return displayedFps_; }
    std::optional<double> CpuPercent() const { return cpuPercent_; }
    std::optional<std::uint64_t> PrivateUsageBytes() const { return process_.privateUsageBytes; }
    const FrameBuckets& FrameTimeBuckets() const { return displayedBuckets_; }
    float ChartCeilingMs() const { return chartCeilingMs_; }

private:
    using BucketDuration = std::chrono::duration<std::int64_t, std::ratio<1, 4>>;
    using BucketIndex = BucketDuration::rep;
    static constexpr float kBaseChartCeilingMs = 50.0f;
    static constexpr auto kDisplayInterval = std::chrono::seconds(1);

    struct LiveBucket {
        BucketIndex index = 0;
        std::optional<float> peakMs;
    };

    static std::size_t SlotFor(BucketIndex index) {
        constexpr auto count = static_cast<BucketIndex>(kFrameBucketCount);
        const auto offset = index % count;
        return static_cast<std::size_t>(offset < 0 ? offset + count : offset);
    }

    void RecordInterval(TimePoint previous, TimePoint now) {
        const double frameMs = std::chrono::duration<double, std::milli>(now - previous).count();
        const auto newest = std::chrono::floor<BucketDuration>(now.time_since_epoch()).count();
        auto& bucket = liveBuckets_[SlotFor(newest)];
        if (bucket.index != newest || !bucket.peakMs || frameMs > *bucket.peakMs) {
            bucket = {newest, static_cast<float>(frameMs)};
        }
        ++intervalCount_;
        intervalPeakMs_ = (std::max)(intervalPeakMs_, frameMs);
        if (!lastDisplayUpdate_ || now - *lastDisplayUpdate_ >= kDisplayInterval) {
            Publish(now, newest, now - lastDisplayUpdate_.value_or(previous));
        }
    }

    void Publish(TimePoint now, BucketIndex newest, Clock::duration elapsed) {
        const auto oldest = newest - static_cast<BucketIndex>(kFrameBucketCount - 1);
        chartCeilingMs_ = kBaseChartCeilingMs;
        for (std::size_t slot = 0; slot < displayedBuckets_.size(); ++slot) {
            const auto index = oldest + static_cast<BucketIndex>(slot);
            const auto& bucket = liveBuckets_[SlotFor(index)];
            auto& peakMs = displayedBuckets_[slot];
            peakMs = bucket.index == index ? bucket.peakMs : std::nullopt;
            if (peakMs) {
                while (*peakMs > chartCeilingMs_) chartCeilingMs_ *= 2.0f;
            }
        }
        const double elapsedMs = std::chrono::duration<double, std::milli>(elapsed).count();
        const double intervals = static_cast<double>(intervalCount_);
        displayedAverageFrameTimeMs_ = elapsedMs / intervals;
        displayedPeakFrameTimeMs_ = intervalPeakMs_;
        displayedFps_ = 1000.0 * intervals / elapsedMs;
        lastDisplayUpdate_ = now;
        intervalCount_ = 0;
        intervalPeakMs_ = 0.0;
    }

    bool open_ = false;
    std::optional<TimePoint> lastFrame_;
    std::optional<TimePoint> lastProcessRead_;
    ProcessSnapshot process_;
    std::optional<double> cpuPercent_;
    std::array<LiveBucket, kFrameBucketCount> liveBuckets_{};
    FrameBuckets displayedBuckets_{};
    float chartCeilingMs_ = kBaseChartCeilingMs;
    std::optional<TimePoint> lastDisplayUpdate_;
    std::size_t intervalCount_ = 0;
    double intervalPeakMs_ = 0.0;
    std::optional<double> displayedAverageFrameTimeMs_;
    std::optional<double> displayedPeakFrameTimeMs_;
    std::optional<double> displayedFps_;
};

}
