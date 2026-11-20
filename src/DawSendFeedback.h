#pragma once

#include "Localization.h"

#include <chrono>
#include <cstdint>
#include <exception>
#include <mutex>
#include <optional>
#include <utility>

namespace HarkenSearch::UI {

enum class DawSendOutcome { Idle, Sending, Success, Partial, Fallback, Failure, BridgeFailure };
using DawSendFeedbackClock = std::chrono::steady_clock;

struct DawSendFeedbackSnapshot {
    DawSendOutcome outcome = DawSendOutcome::Idle;
    std::optional<int> sent;
    std::optional<int> total;
    LedgerText detail;
    LedgerText remediation;
    DawSendFeedbackClock::time_point changedAt{};
    std::uint64_t revision = 0;
};

inline DawSendOutcome ClassifyDawSendResult(bool ok, std::optional<int> sent, std::optional<int> total) {
    if (sent && total && *sent > 0 && *sent < *total) return DawSendOutcome::Partial;
    return ok ? DawSendOutcome::Success : DawSendOutcome::Failure;
}

class DawSendFeedback {
public:
    DawSendFeedbackSnapshot snapshot() const {
        const std::lock_guard<std::mutex> lock(mutex_);
        return current_;
    }

    bool isSending() const {
        const std::lock_guard<std::mutex> lock(mutex_);
        return current_.outcome == DawSendOutcome::Sending;
    }

    bool begin(int total) {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (current_.outcome == DawSendOutcome::Sending) return false;
        DawSendFeedbackSnapshot next;
        next.outcome = DawSendOutcome::Sending;
        next.total = total;
        replace(std::move(next));
        return true;
    }

    void setProgress(int sent, int total) {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (current_.outcome != DawSendOutcome::Sending) return;
        current_.sent = sent;
        current_.total = total;
    }

    void finish(DawSendFeedbackSnapshot result) {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (current_.outcome != DawSendOutcome::Sending) return;
        if (!result.total) result.total = current_.total;
        replace(std::move(result));
    }

    void failBeforeSend(LedgerText detail) {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (current_.outcome == DawSendOutcome::Sending) return;
        DawSendFeedbackSnapshot failure;
        failure.outcome = DawSendOutcome::Failure;
        failure.detail = std::move(detail);
        replace(std::move(failure));
    }

    void expire(std::uint64_t revision) {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (current_.revision == revision && current_.outcome != DawSendOutcome::Sending) current_ = {};
    }

private:
    void replace(DawSendFeedbackSnapshot next) {
        next.changedAt = DawSendFeedbackClock::now();
        next.revision = ++revision_;
        current_ = std::move(next);
    }

    mutable std::mutex mutex_;
    DawSendFeedbackSnapshot current_;
    std::uint64_t revision_ = 0;
};

template <typename Dispatch, typename ReportException>
auto MakeDawSendWorker(DawSendFeedback& sendFeedback, Dispatch dispatch, ReportException reportException) {
    return [&sendFeedback, dispatch = std::move(dispatch), reportException = std::move(reportException)]() mutable {
        const auto fail = [&](const char* message) {
            DawSendFeedbackSnapshot failure;
            failure.outcome = DawSendOutcome::Failure;
            sendFeedback.finish(std::move(failure));
            try {
                reportException(message);
            } catch (...) {
            }
        };
        try {
            sendFeedback.finish(dispatch());
        } catch (const std::exception& error) {
            fail(error.what());
        } catch (...) {
            fail("unknown exception");
        }
    };
}

}
