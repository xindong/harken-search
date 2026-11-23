#pragma once

#include <cstddef>
#include <string>
#include <vector>

// Bottom-right task progress overlay (card REQ-20261123-TASK-PROGRESS-TOAST).
//
// The model owns the visible list, the per-entry lifecycle and the geometry of
// the floating stack. It deliberately holds no ImGui and no clock: callers pass
// the current time in seconds, which keeps the timing rules (3 s hold, fade,
// persistent failures) testable without a window.
//
// Rules encoded here:
// * anchored 16 px from the right and bottom edges of the work area;
// * 280 px wide rows, 52 px high, at most 3 rows stacked;
// * active rows stay visible; succeeded rows hold 3 s then fade out;
// * failed rows never count down and stay until dismissed;
// * a source that already shows its own progress is not duplicated here.

enum class TaskProgressState {
    Active,
    Succeeded,
    Failed,
};

struct TaskProgressEntry {
    std::string id;
    std::string title;
    std::string phase;
    float fraction = 0.0f; // negative means indeterminate
    TaskProgressState state = TaskProgressState::Active;
    double completedAt = 0.0;
    bool inlineProgressVisible = false;
    bool detailAvailable = false;
};

struct TaskProgressToastMetrics {
    float width = 280.0f;
    float rowHeight = 52.0f;
    float rowGap = 6.0f;
    float edgeInset = 16.0f;
    float panelPadding = 8.0f;
    int maxRows = 3;
    double successHoldSeconds = 3.0;
    double fadeSeconds = 0.4;
};

struct TaskProgressToastRect {
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
};

class TaskProgressToastStack {
public:
    TaskProgressToastStack() = default;
    explicit TaskProgressToastStack(TaskProgressToastMetrics metrics);

    // Adds a row, or refreshes the row with the same id in place. An entry that
    // already faded away starts over as a fresh active row.
    void Report(const std::string& id,
                const std::string& title,
                const std::string& phase,
                float fraction,
                bool inlineProgressVisible,
                double now);

    void Complete(const std::string& id, double now);
    void Fail(const std::string& id, double now);
    void Dismiss(const std::string& id);
    void SetInlineProgressVisible(const std::string& id, bool visible);

    // Drops rows whose fade finished. Succeeded rows are removed once
    // hold + fade elapsed; failed rows are never removed here.
    void Tick(double now);

    // Rows that should be painted, oldest first, capped at maxRows so the
    // newest rows sit closest to the corner. Succeeded rows keep their slot
    // while fading.
    std::vector<TaskProgressEntry> VisibleRows(double now) const;

    // 1.0 while the row is fully visible, ramping to 0.0 across the fade.
    float FadeAlpha(const TaskProgressEntry& entry, double now) const;

    bool Has(const std::string& id) const;
    std::size_t Size() const;

    const TaskProgressToastMetrics& Metrics() const { return metrics_; }
    void SetMetrics(const TaskProgressToastMetrics& metrics) { metrics_ = metrics; }

    // Panel rect for the given stack of rows inside the work area. An empty
    // stack reports a zero-sized rect at the anchored corner.
    TaskProgressToastRect PanelRect(float workX, float workY, float workWidth,
                                    float workHeight, std::size_t rowCount) const;

    // Row rect in the same coordinates the panel was computed for.
    TaskProgressToastRect RowRect(const TaskProgressToastRect& panel,
                                  std::size_t rowIndex) const;

private:
    bool IsExpired(const TaskProgressEntry& entry, double now) const;
    std::size_t VisibleRowCount(double now) const;

    TaskProgressToastMetrics metrics_{};
    std::vector<TaskProgressEntry> entries_;
};
