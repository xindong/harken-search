#include "TaskProgressToastModel.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace {

constexpr float kMinPanelExtent = 1.0f;

TaskProgressToastRect ZeroRectAt(float workX, float workY, float workWidth,
                                 float workHeight, float inset) {
    TaskProgressToastRect rect;
    rect.width = 0.0f;
    rect.height = 0.0f;
    rect.x = workX + std::max(0.0f, workWidth) - inset;
    rect.y = workY + std::max(0.0f, workHeight) - inset;
    return rect;
}

} // namespace

TaskProgressToastStack::TaskProgressToastStack(TaskProgressToastMetrics metrics)
    : metrics_(metrics) {}

void TaskProgressToastStack::Report(const std::string& id,
                                    const std::string& title,
                                    const std::string& phase,
                                    float fraction,
                                    bool inlineProgressVisible,
                                    double now) {
    if (id.empty()) return;
    for (auto& entry : entries_) {
        if (entry.id != id) continue;
        entry.title = title;
        entry.phase = phase;
        entry.fraction = fraction;
        entry.inlineProgressVisible = inlineProgressVisible;
        entry.state = TaskProgressState::Active;
        entry.completedAt = 0.0;
        return;
    }
    TaskProgressEntry entry;
    entry.id = id;
    entry.title = title;
    entry.phase = phase;
    entry.fraction = fraction;
    entry.inlineProgressVisible = inlineProgressVisible;
    entry.state = TaskProgressState::Active;
    entries_.push_back(std::move(entry));
    (void)now;
}

void TaskProgressToastStack::Complete(const std::string& id, double now) {
    for (auto& entry : entries_) {
        if (entry.id != id) continue;
        if (entry.state == TaskProgressState::Succeeded) return;
        entry.state = TaskProgressState::Succeeded;
        entry.fraction = 1.0f;
        entry.completedAt = now;
        return;
    }
}

void TaskProgressToastStack::Fail(const std::string& id, double now) {
    for (auto& entry : entries_) {
        if (entry.id != id) continue;
        entry.state = TaskProgressState::Failed;
        entry.detailAvailable = true;
        entry.completedAt = now;
        return;
    }
}

void TaskProgressToastStack::Dismiss(const std::string& id) {
    entries_.erase(std::remove_if(entries_.begin(), entries_.end(),
                                  [&id](const TaskProgressEntry& entry) {
                                      return entry.id == id;
                                  }),
                   entries_.end());
}

void TaskProgressToastStack::SetInlineProgressVisible(const std::string& id,
                                                      bool visible) {
    for (auto& entry : entries_) {
        if (entry.id == id) entry.inlineProgressVisible = visible;
    }
}

void TaskProgressToastStack::Tick(double now) {
    entries_.erase(std::remove_if(entries_.begin(), entries_.end(),
                                  [this, now](const TaskProgressEntry& entry) {
                                      return IsExpired(entry, now);
                                  }),
                   entries_.end());
}

bool TaskProgressToastStack::IsExpired(const TaskProgressEntry& entry,
                                       double now) const {
    if (entry.state != TaskProgressState::Succeeded) return false;
    const double total = std::max(0.0, metrics_.successHoldSeconds) +
                         std::max(0.0, metrics_.fadeSeconds);
    return now - entry.completedAt >= total;
}

float TaskProgressToastStack::FadeAlpha(const TaskProgressEntry& entry,
                                        double now) const {
    if (entry.state != TaskProgressState::Succeeded) return 1.0f;
    const double elapsed = now - entry.completedAt;
    const double hold = std::max(0.0, metrics_.successHoldSeconds);
    if (elapsed <= hold) return 1.0f;
    const double fade = std::max(0.0, metrics_.fadeSeconds);
    if (fade <= 0.0) return 0.0f;
    const double t = (elapsed - hold) / fade;
    if (t >= 1.0) return 0.0f;
    return static_cast<float>(1.0 - t);
}

std::size_t TaskProgressToastStack::VisibleRowCount(double now) const {
    std::size_t count = 0;
    for (const auto& entry : entries_) {
        if (entry.inlineProgressVisible) continue;
        if (IsExpired(entry, now)) continue;
        ++count;
    }
    const std::size_t cap = static_cast<std::size_t>(std::max(0, metrics_.maxRows));
    return std::min(count, cap);
}

std::vector<TaskProgressEntry> TaskProgressToastStack::VisibleRows(
    double now) const {
    std::vector<const TaskProgressEntry*> order;
    order.reserve(entries_.size());
    for (const auto& entry : entries_) {
        if (entry.inlineProgressVisible) continue;
        if (IsExpired(entry, now)) continue;
        order.push_back(&entry);
    }
    const std::size_t cap = static_cast<std::size_t>(std::max(0, metrics_.maxRows));
    if (order.size() > cap) {
        order.erase(order.begin(), order.end() - static_cast<std::ptrdiff_t>(cap));
    }
    std::vector<TaskProgressEntry> rows;
    rows.reserve(order.size());
    for (const auto* entry : order) rows.push_back(*entry);
    return rows;
}

bool TaskProgressToastStack::Has(const std::string& id) const {
    for (const auto& entry : entries_) {
        if (entry.id == id) return true;
    }
    return false;
}

std::size_t TaskProgressToastStack::Size() const { return entries_.size(); }

TaskProgressToastRect TaskProgressToastStack::PanelRect(
    float workX, float workY, float workWidth, float workHeight,
    std::size_t rowCount) const {
    const float inset = std::max(0.0f, metrics_.edgeInset);
    if (workWidth <= 0.0f || workHeight <= 0.0f || rowCount == 0) {
        return ZeroRectAt(workX, workY, workWidth, workHeight, inset);
    }

    const float padding = std::max(0.0f, metrics_.panelPadding);
    const float gap = std::max(0.0f, metrics_.rowGap);
    const float rows = static_cast<float>(rowCount);
    const float wantedWidth = std::max(kMinPanelExtent, metrics_.width);
    const float width = std::min(wantedWidth, std::max(kMinPanelExtent, workWidth - 2.0f * inset));
    const float height = std::min(
        padding * 2.0f + rows * std::max(kMinPanelExtent, metrics_.rowHeight) +
            (rows - 1.0f) * gap,
        std::max(kMinPanelExtent, workHeight - 2.0f * inset));

    TaskProgressToastRect rect;
    rect.width = width;
    rect.height = height;
    rect.x = std::max(workX, workX + workWidth - inset - width);
    rect.y = std::max(workY, workY + workHeight - inset - height);
    return rect;
}

TaskProgressToastRect TaskProgressToastStack::RowRect(
    const TaskProgressToastRect& panel, std::size_t rowIndex) const {
    const float padding = std::max(0.0f, metrics_.panelPadding);
    const float gap = std::max(0.0f, metrics_.rowGap);
    const float rowHeight = std::max(kMinPanelExtent, metrics_.rowHeight);
    TaskProgressToastRect rect;
    rect.x = panel.x + padding;
    rect.y = panel.y + padding +
             static_cast<float>(rowIndex) * (rowHeight + gap);
    rect.width = std::max(kMinPanelExtent, panel.width - padding * 2.0f);
    rect.height = rowHeight;
    return rect;
}
