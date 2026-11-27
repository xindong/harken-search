#include "DiagnosticOverlay.h"

#include "DiagnosticLogBundle.h"
#include "DiagnosticOverlayLayout.h"
#include "DiagnosticsLogger.h"
#include "HarkenSearchWidgets.h"
#include "Localization.h"
#include "MemoryBudget.h"
#include "PerformanceMonitor.h"
#include "Theme.h"
#include "TooltipHoverDelay.h"
#include "../ThemeModuleColors.h"
#include "TransientScrollbarHost.h"
#include "platform/system.h"
#include "ui/CloseIconButton.h"
#include "lucide/IconsLucide.h"

#include <atomic>
#include <cfloat>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <future>
#include <string_view>
#include <thread>

namespace harken::diagnostic_overlay {
namespace {

using performance_monitor::Monitor;
using performance_monitor::ProcessSnapshot;

constexpr float kPadding = 12.0f;
constexpr float kBodySize = 14.0f;
constexpr float kLogSize = 15.0f;
constexpr float kCaptionSize = 12.0f;
constexpr int kRecentCount = 50;
constexpr double kBytesPerMiB = 1024.0 * 1024.0;
constexpr const char* kUnavailable = u8"—";
constexpr const char* kWindowName = "##DiagOverlay";

enum class Tab { Performance, Logs };

enum class ExportStatus { Idle, Writing, Busy, Failed, Empty, Saved };

struct LogExport {
    std::future<HarkenSearch::DiagnosticLogBundleResult> pending;
    ExportStatus status = ExportStatus::Idle;
    std::string path;
    std::atomic<bool> busy{false};
};

LogExport& ExportTask() {
    static LogExport task;
    return task;
}

bool StartLogExport(LogExport& task) {
    if (task.pending.valid()) return false;
    using namespace std::chrono;
    const auto clickedAt = duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
    task.path.clear();
    task.busy.store(false, std::memory_order_relaxed);
    auto directory = std::filesystem::u8path(Platform::GetAppDataDir());
    if (!directory.is_absolute()) {
        task.status = ExportStatus::Failed;
        return false;
    }
    directory /= "diagnostic-exports";
    try {
        // Capture and ZIP work run off the UI thread: the capture reserves the
        // full ring buffer, so doing it here would cost the click frame.
        task.pending = std::async(
            std::launch::async, [&task, clickedAt, directory = std::move(directory)] {
                DiagLogSnapshot snapshot;
                constexpr int kCaptureAttempts = 4;
                for (int attempt = 0; attempt < kCaptureAttempts; ++attempt) {
                    if (DiagnosticsLogger::instance().trySnapshotForExport(clickedAt, snapshot))
                        return HarkenSearch::WriteDiagnosticLogBundle(snapshot, directory);
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                }
                task.busy.store(true, std::memory_order_relaxed);
                return HarkenSearch::DiagnosticLogBundleResult{};
            });
        task.status = ExportStatus::Writing;
        return true;
    } catch (...) {
        task.status = ExportStatus::Failed;
        return false;
    }
}

const char* ExportStatusText(ExportStatus status) {
    switch (status) {
    case ExportStatus::Writing: return L(DiagnosticOverlay_ExportWriting);
    case ExportStatus::Busy: return L(DiagnosticOverlay_ExportBusy);
    case ExportStatus::Failed: return L(DiagnosticOverlay_ExportFailed);
    case ExportStatus::Empty: return L(DiagnosticOverlay_ExportEmpty);
    case ExportStatus::Saved: return L(DiagnosticOverlay_ExportSaved);
    case ExportStatus::Idle: return nullptr;
    }
    return nullptr;
}

struct LogRow {
    DiagLogEntry entry;
    char time[12]{};
    char detail[256]{};
};

struct ViewData {
    Tab tab = Tab::Performance;
    std::optional<Monitor::TimePoint> updated;
    std::optional<MemoryBudget::Statistics> cache;
    MemoryBudget::BudgetConfig budgets;
    std::array<LogRow, kRecentCount> logs;
    int logCount = 0;
    std::uint64_t droppedCount = 0;
};

struct Layout {
    ImFont* regular;
    ImFont* emphasis;
    ImDrawList* draw;
    ImVec2 origin;
    layout::Size size;
    float scale;
    float typeScale;
    ImVec4 ink;
    ImVec4 muted;

    ImVec2 Point(float x, float y) const { return ImVec2(origin.x + x * scale, origin.y + y * scale); }

    float FontPixels(float pixels) const {
        return std::max(1.0f, ImGui::GetRoundedFontSize(pixels * scale * typeScale));
    }

    float LineHeight(float pixels) const { return FontPixels(pixels) / scale; }

    void PushFont(float pixels, bool bold = false) const {
        const auto& style = ImGui::GetStyle();
        ImGui::PushFont(bold ? emphasis : regular, FontPixels(pixels) / (style.FontScaleMain * style.FontScaleDpi));
    }

    float Width(const char* text, float pixels, bool bold = false) const {
        return (bold ? emphasis : regular)->CalcTextSizeA(FontPixels(pixels), FLT_MAX, 0.0f, text).x / scale;
    }

    void Text(float x, float y, const char* text, float pixels, ImVec4 color, bool bold = false) const {
        draw->AddText(bold ? emphasis : regular, FontPixels(pixels), Point(x, y), ImGui::GetColorU32(color), text);
    }

    void Right(float x, float y, const char* text, float pixels, ImVec4 color, bool bold = false) const {
        Text(x - Width(text, pixels, bold), y, text, pixels, color, bold);
    }

    void FitText(float x, float right, float y, const char* text, float pixels, ImVec4 color, bool bold = false) const {
        if (right <= x) return;
        if (Width(text, pixels, bold) <= right - x) {
            Text(x, y, text, pixels, color, bold);
            return;
        }
        PushFont(pixels, bold);
        ImGui::PushStyleColor(ImGuiCol_Text, color);
        const auto end = Point(right, y + LineHeight(pixels));
        ImGui::RenderTextEllipsis(draw, Point(x, y), end, end.x, text, nullptr, nullptr);
        ImGui::PopStyleColor();
        ImGui::PopFont();
    }
};

struct NativeResizeLimits {
    ImGuiWindow* window;
    layout::Bounds bounds;
    layout::Placement placed;
};

void LimitNativeResize(ImGuiSizeCallbackData* data) {
    const auto& limits = *static_cast<const NativeResizeLimits*>(data->UserData);
    if (!limits.window) return;
    const auto active = ImGui::GetActiveID();
    const bool fromLeft = active == ImGui::GetWindowResizeCornerID(limits.window, 1) ||
        active == ImGui::GetWindowResizeBorderID(limits.window, ImGuiDir_Left);
    const bool fromTop = active == ImGui::GetWindowResizeBorderID(limits.window, ImGuiDir_Up);
    const auto maximum = layout::ResizeMaximum(limits.bounds, limits.placed, fromLeft, fromTop);
    data->DesiredSize.x = std::min(data->DesiredSize.x, maximum.width);
    data->DesiredSize.y = std::min(data->DesiredSize.y, maximum.height);
}

ProcessSnapshot ReadProcess() {
    ProcessSnapshot result;
    if (const auto cpu = Platform::QueryCurrentProcessCpuTime()) {
        result.cpuTime100ns = cpu->userAndKernel100ns;
        result.logicalProcessorCount = cpu->logicalProcessorCount;
    }
    const auto memory = Platform::QueryCurrentProcessMemoryUsage();
    if (memory.status == Platform::ProcessMemoryUsageStatus::Success) {
        result.privateUsageBytes = memory.privateUsageBytes;
    }
    return result;
}

const char* Value(char (&buffer)[32], const std::optional<double>& number, const char* format) {
    if (!number) return kUnavailable;
    std::snprintf(buffer, sizeof(buffer), format, *number);
    return buffer;
}

const char* FrameValue(char (&buffer)[32], const std::optional<double>& milliseconds) {
    return Value(buffer, milliseconds,
                 milliseconds && (*milliseconds < 0.1 || *milliseconds >= 100000.0) ? "%.3g" : "%.1f");
}

void FormatLogRow(LogRow& row) {
    const auto& entry = row.entry;
    const std::time_t seconds = static_cast<std::time_t>(entry.timestampMs / 1000);
    std::tm local{};
#ifdef _WIN32
    const bool valid = entry.timestampMs > 0 && localtime_s(&local, &seconds) == 0;
#else
    const bool valid = entry.timestampMs > 0 && localtime_r(&seconds, &local);
#endif
    if (!valid || !std::strftime(row.time, sizeof(row.time), "%H:%M:%S", &local))
        std::snprintf(row.time, sizeof(row.time), "%s", kUnavailable);
    if (entry.type == DiagLogType::Performance) {
        std::snprintf(row.detail, sizeof(row.detail), "%s · %.1f ms", entry.detail, entry.durationMs);
    } else if (entry.type == DiagLogType::Metric) {
        std::snprintf(row.detail, sizeof(row.detail), "%s · %.3g", entry.detail, entry.value);
    } else {
        std::snprintf(row.detail, sizeof(row.detail), "%s", entry.detail);
    }
}

void Refresh(ViewData& view, Monitor::TimePoint now, const MemoryBudget* memoryBudget) {
    if (view.updated && now - *view.updated < std::chrono::seconds(1)) return;
    view.updated = now;
    if (view.tab == Tab::Performance) {
        if (memoryBudget) {
            view.cache = memoryBudget->getStatistics();
            view.budgets = memoryBudget->getBudgetConfig();
        } else {
            view.cache.reset();
        }
        return;
    }

    auto& diagnostics = DiagnosticsLogger::instance();
    std::array<DiagLogEntry, kRecentCount> recent;
    view.logCount = diagnostics.copyRecent(recent.data(), kRecentCount);
    view.droppedCount = diagnostics.droppedCount();
    for (int index = 0; index < view.logCount; ++index) {
        view.logs[index].entry = recent[index];
        FormatLogRow(view.logs[index]);
    }
}

bool DrawHeader(const Layout& ui, ViewData& view, const ThemeColorScheme& theme) {
    const ThemeModuleColors modules = ResolveThemeModuleColors(theme);
    constexpr float titleSize = 16.0f;
    constexpr float top = 8.0f;
    const float height = ui.LineHeight(kBodySize) + 12.0f;
    const float closeX = ui.size.width - 32.0f;
    const auto* performance = L(DiagnosticOverlay_Performance);
    const auto* logs = L(DiagnosticOverlay_Logs);
    const auto tabWidth = [&](const char* label) {
        return std::max(44.0f, std::max(ui.Width(label, kBodySize), ui.Width(label, kBodySize, true)) + 18.0f);
    };
    const float perfWidth = tabWidth(performance);
    const float logsWidth = tabWidth(logs);
    const float logsX = closeX - logsWidth - 7.0f;
    const float perfX = logsX - perfWidth - 2.0f;
    ui.FitText(kPadding, perfX - 8.0f, top + (height - ui.LineHeight(titleSize)) * 0.5f,
               L(DiagnosticOverlay_Title), titleSize, ui.ink, true);
    const auto drawTab = [&](const char* id, const char* label, Tab tab, float x, float width) {
        ImGui::SetCursorPos(ImVec2(x * ui.scale, top * ui.scale));
        if (ImGui::InvisibleButton(id, ImVec2(width * ui.scale, height * ui.scale))) {
            view.tab = tab;
            view.updated.reset();
        }
        const bool selected = view.tab == tab;
        if (selected || ImGui::IsItemHovered()) {
            const auto fill = ImLerp(modules.diagnostic.background, modules.diagnostic.accent, selected ? 0.20f : 0.11f);
            ui.draw->AddRectFilled(ui.Point(x, top), ui.Point(x + width, top + height),
                ImGui::GetColorU32(fill), 5.0f * ui.scale);
        }
        ui.Text(x + (width - ui.Width(label, kBodySize, selected)) * 0.5f, top + 6.0f, label, kBodySize,
                selected ? ui.ink : ui.muted, selected);
    };
    drawTab("##DiagnosticPerformance", performance, Tab::Performance, perfX, perfWidth);
    drawTab("##DiagnosticLogs", logs, Tab::Logs, logsX, logsWidth);
    ImGui::SetCursorPos(ImVec2(closeX * ui.scale, top * ui.scale));
    ui.PushFont(kLogSize);
    const bool closed = aura::ui::CloseIconButton::Draw(ICON_LC_X "###CloseDiagnosticOverlay",
        ImVec2(24.0f * ui.scale, height * ui.scale), ui.muted, modules.diagnostic.background);
    ImGui::PopFont();
    return !closed;
}

void DrawFrameChart(const Layout& ui, const Monitor& monitor, ImU32 accent, float labelY) {
    const float right = ui.size.width - kPadding;
    const float top = labelY + ui.LineHeight(kBodySize) + 8.0f;
    const float axisY = ui.size.height - kPadding - ui.LineHeight(kCaptionSize);
    const float bottom = axisY - 6.0f;
    const auto* title = L(DiagnosticOverlay_FrameTime);
    ui.Text(kPadding, labelY, title, kBodySize, ui.ink, true);

    char averageBuffer[32], peakBuffer[32], statistics[112];
    const auto* average = FrameValue(averageBuffer, monitor.AverageFrameTimeMs());
    const auto* peak = FrameValue(peakBuffer, monitor.PeakFrameTimeMs());
    std::snprintf(statistics, sizeof(statistics), L(DiagnosticOverlay_FrameStatsFmt), average, peak);
    const float statisticsLeft = kPadding + ui.Width(title, kBodySize, true) + 8.0f;
    ui.FitText(std::max(statisticsLeft, right - ui.Width(statistics, kCaptionSize)), right, labelY,
               statistics, kCaptionSize, ui.muted);

    const float ceiling = monitor.ChartCeilingMs();
    char scaleBuffer[32];
    std::snprintf(scaleBuffer, sizeof(scaleBuffer), ceiling < 1000000.0f ? "%.0f" : "%.2g", ceiling);
    const float plotRight = right - std::max(36.0f, ui.Width(scaleBuffer, kCaptionSize) + 10.0f);
    if (bottom <= top || plotRight <= kPadding) return;
    ui.Right(right, top, scaleBuffer, kCaptionSize, ui.muted);
    ui.Right(right, bottom - ui.LineHeight(kCaptionSize), "0", kCaptionSize, ui.muted);
    ui.Text(kPadding, axisY, L(DiagnosticOverlay_ChartTenSecondsAgo), kCaptionSize, ui.muted);
    ui.Right(plotRight, axisY, L(DiagnosticOverlay_ChartNow), kCaptionSize, ui.muted);

    const float step = (plotRight - kPadding) / Monitor::kFrameBucketCount;
    std::array<ImVec2, Monitor::kFrameBucketCount> points;
    int count = 0;
    const auto drawRun = [&] {
        if (count > 1) ui.draw->AddPolyline(points.data(), count, accent, ImDrawFlags_None, 1.3f * ui.scale);
        else if (count == 1) ui.draw->AddCircleFilled(points.front(), 1.5f * ui.scale, accent);
        count = 0;
    };
    const auto& buckets = monitor.FrameTimeBuckets();
    for (std::size_t index = 0; index < buckets.size(); ++index) {
        if (const auto interval = buckets[index]) {
            points[count++] = ui.Point(kPadding + (static_cast<float>(index) + 0.5f) * step,
                                      bottom - (bottom - top) * (*interval / ceiling));
        } else {
            drawRun();
        }
    }
    drawRun();
}

void DrawPerformance(const Layout& ui, const ViewData& view, const Monitor& monitor,
                     const ThemeColorScheme& theme) {
    const ThemeModuleColors modules = ResolveThemeModuleColors(theme);
    constexpr float fpsSize = 28.0f;
    constexpr float metricsY = 44.0f;
    const float right = ui.size.width - kPadding;
    const float metricsBottom = metricsY + ui.LineHeight(fpsSize);
    const float valueY = metricsBottom - ui.LineHeight(kBodySize);
    const float cpuX = kPadding + (right - kPadding) * 0.39f;
    char fpsBuffer[32], cpuBuffer[32], memoryBuffer[32];
    const auto* fps = Value(fpsBuffer, monitor.FramesPerSecond(), "%.0f");
    const auto* fpsLabel = L(DiagnosticOverlay_FpsLabel);
    const float fpsLabelX = std::min(cpuX - ui.Width(fpsLabel, kCaptionSize) - 8.0f,
                                    kPadding + ui.Width(fps, fpsSize, true) + 5.0f);
    ui.FitText(kPadding, fpsLabelX - 5.0f, metricsY, fps, fpsSize, ui.ink, true);
    ui.Text(fpsLabelX, metricsBottom - ui.LineHeight(kCaptionSize), fpsLabel, kCaptionSize, ui.muted);
    const auto* cpu = Value(cpuBuffer, monitor.CpuPercent(), "%.1f%%");
    const auto* cpuLabel = L(DiagnosticOverlay_CpuLabel);
    ui.Text(cpuX, valueY, cpuLabel, kBodySize, ui.muted);
    const float cpuValueX = cpuX + ui.Width(cpuLabel, kBodySize) + 5.0f;
    const auto memory = monitor.PrivateUsageBytes();
    const auto* memoryText = Value(memoryBuffer, memory ? std::optional<double>{*memory / kBytesPerMiB} : std::nullopt, "%.0f MB");
    const float memoryLabelRight = right - ui.Width(memoryText, kBodySize, true) - 5.0f;
    const auto* memoryLabel = L(DiagnosticOverlay_Memory);
    const float memoryLabelX = std::max(cpuValueX + ui.Width(cpu, kBodySize, true) + 8.0f,
                                       memoryLabelRight - ui.Width(memoryLabel, kBodySize));
    ui.FitText(cpuValueX, memoryLabelX - 8.0f, valueY, cpu, kBodySize, ui.ink, true);
    ui.FitText(memoryLabelX, memoryLabelRight, valueY, memoryLabel, kBodySize, ui.muted);
    ui.Right(right, valueY, memoryText, kBodySize, ui.ink, true);

    const float cacheTitleY = metricsBottom + 11.0f;
    const float rowStep = ui.LineHeight(kBodySize) + 5.0f;
    const float firstRowY = cacheTitleY + rowStep;
    ui.Text(kPadding, cacheTitleY, L(DiagnosticOverlay_Cache), kBodySize, ui.ink, true);
    ui.Right(right, cacheTitleY, L(DiagnosticOverlay_UsedLimit), kCaptionSize, ui.muted);
    const auto stats = view.cache.value_or(MemoryBudget::Statistics{});
    const auto cacheRow = [&](float y, const char* label, std::uint64_t used, std::uint64_t limit, bool total = false) {
        ui.Text(kPadding, y, label, kBodySize, ui.ink, total);
        if (!view.cache) {
            ui.Right(right, y, kUnavailable, kBodySize, ui.muted);
            return;
        }
        char usage[56];
        std::snprintf(usage, sizeof(usage), "%.1f / %.0f MB", used / kBytesPerMiB, limit / kBytesPerMiB);
        ui.Right(right, y, usage, kBodySize, ui.ink, total);
        const float fraction = limit ? static_cast<float>(std::min(1.0, static_cast<double>(used) / limit)) : 0.0f;
        auto barColor = modules.diagnostic.accent;
        if (fraction >= 0.9f) barColor = ImLerp(barColor, modules.meter.clip, 0.62f);
        else if (fraction >= 0.7f) barColor = ImLerp(barColor, modules.meter.warning, 0.64f);
        else if (!total) barColor = ImLerp(barColor, modules.signal.level, 0.14f);
        const float left = std::max(82.0f * ui.typeScale, kPadding + ui.Width(label, kBodySize, total) + 8.0f);
        const float end = right - std::max(105.0f * ui.typeScale, ui.Width(usage, kBodySize, total) + 10.0f);
        if (end <= left) return;
        ImGui::SetCursorPos(ImVec2(left * ui.scale, (y + (ui.LineHeight(kBodySize) - 6.0f) * 0.5f) * ui.scale));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 3.0f * ui.scale);
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImLerp(modules.diagnostic.background, modules.diagnostic.accent, 0.22f));
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, barColor);
        ImGui::ProgressBar(fraction, ImVec2((end - left) * ui.scale, 6.0f * ui.scale), "");
        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar();
    };
    const float totalY = firstRowY + 3.0f * rowStep + 2.0f;
    cacheRow(firstRowY, L(DiagnosticOverlay_Waveform), stats.waveformUsage, view.budgets.waveformBudget);
    cacheRow(firstRowY + rowStep, L(DiagnosticOverlay_Cover), stats.coverUsage, view.budgets.coverBudget);
    cacheRow(firstRowY + 2.0f * rowStep, L(DiagnosticOverlay_Metadata), stats.metadataUsage, view.budgets.metadataBudget);
    cacheRow(totalY, L(DiagnosticOverlay_Total), stats.totalUsage, view.budgets.totalBudget, true);

    DrawFrameChart(ui, monitor, ImGui::GetColorU32(modules.diagnostic.accent), totalY + rowStep + 6.0f);
}

float DrawExportAction(const Layout& ui) {
    auto& task = ExportTask();
    const auto* label = L(DiagnosticOverlay_ExportMinute);
    const float actionY = 28.0f + ui.LineHeight(kBodySize);
    const float actionHeight = ui.LineHeight(kBodySize) + 12.0f;
    const float actionWidth = std::min(ui.Width(label, kBodySize) + 24.0f, ui.size.width - 2.0f * kPadding);
    const float actionX = ui.size.width - kPadding - actionWidth;

    ImGui::SetCursorPos(ImVec2(actionX * ui.scale, actionY * ui.scale));
    ui.PushFont(kBodySize);
    ImGui::BeginDisabled(task.pending.valid());
    const bool clicked = aura::ui::ActionButton(label, ImVec2(actionWidth * ui.scale, actionHeight * ui.scale));
    const bool hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled);
    ImGui::EndDisabled();
    ImGui::PopFont();
    if (clicked) StartLogExport(task);

    if (const auto* status = ExportStatusText(task.status)) {
        const float statusY = actionY + (actionHeight - ui.LineHeight(kCaptionSize)) * 0.5f;
        ui.FitText(kPadding, actionX - 8.0f, statusY, status, kCaptionSize, ui.muted);
    }
    if (hovered) {
        if (!task.path.empty()) HarkenSearch::SetTooltipUngated("%s\n%s", L(DiagnosticOverlay_ExportHint), task.path.c_str());
        else HarkenSearch::SetTooltipUngated("%s", L(DiagnosticOverlay_ExportHint));
    }
    return actionY + actionHeight;
}

void DrawLogs(const Layout& ui, const ViewData& view) {
    const float labelY = DrawExportAction(ui) + 10.0f;
    const auto* timeLabel = L(DiagnosticOverlay_Time);
    const auto* categoryLabel = L(DiagnosticOverlay_Category);
    const float timeWidth = std::max(ui.Width(timeLabel, kCaptionSize), ui.Width("00:00:00", kLogSize)) + 12.0f;
    const float categoryWidth = std::max(ui.Width(categoryLabel, kCaptionSize), 84.0f * ui.typeScale) + 12.0f;
    const float listY = labelY + ui.LineHeight(kCaptionSize) + 9.0f;
    const float listHeight = ui.size.height - listY - 16.0f;
    ui.Text(kPadding, labelY, timeLabel, kCaptionSize, ui.muted);
    ui.Text(kPadding + timeWidth, labelY, categoryLabel, kCaptionSize, ui.muted);
    ui.Text(kPadding + timeWidth + categoryWidth, labelY, L(DiagnosticOverlay_Event), kCaptionSize, ui.muted);
    if (!view.logCount && !view.droppedCount) {
        ui.Text(kPadding, listY + 4.0f, L(DiagnosticOverlay_Empty), kLogSize, ui.muted);
        return;
    }
    if (listHeight <= 0.0f) return;
    ImGui::SetCursorPos(ImVec2(kPadding * ui.scale, listY * ui.scale));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(0.0f, 4.0f * ui.scale));
    ui.PushFont(kLogSize);
    if (ImGui::BeginChild("##DiagnosticRecentEvents",
                        ImVec2((ui.size.width - 2.0f * kPadding) * ui.scale, listHeight * ui.scale),
                        ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoSavedSettings)) {
        if (ImGui::BeginTable("##DiagnosticEventRows", 3, ImGuiTableFlags_NoSavedSettings)) {
            ImGui::TableSetupColumn("##Time", ImGuiTableColumnFlags_WidthFixed, timeWidth * ui.scale);
            ImGui::TableSetupColumn("##Category", ImGuiTableColumnFlags_WidthFixed, categoryWidth * ui.scale);
            ImGui::TableSetupColumn("##Message", ImGuiTableColumnFlags_WidthStretch);
            for (int index = 0; index < view.logCount; ++index) {
                const auto& row = view.logs[index];
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextColored(ui.muted, "%s", row.time);
                ImGui::TableSetColumnIndex(1);
                ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + (categoryWidth - 8.0f) * ui.scale);
                ImGui::TextColored(ui.muted, "%s", row.entry.category);
                ImGui::PopTextWrapPos();
                ImGui::TableSetColumnIndex(2);
                ImGui::TextWrapped("%s", row.detail);
            }
            ImGui::EndTable();
        }
        if (view.droppedCount) {
            ImGui::TextColored(ui.muted, L(Main_DroppedFmt), static_cast<unsigned long long>(view.droppedCount));
        }
    }
    ImGui::EndChild();
    ImGui::PopFont();
    ImGui::PopStyleVar(2);
}

}

bool ConsumeFinishedExport(LogExport& task) {
    if (!task.pending.valid() ||
        task.pending.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) return false;
    try {
        auto result = task.pending.get();
        if (task.busy.load(std::memory_order_relaxed)) {
            task.busy.store(false, std::memory_order_relaxed);
            task.status = ExportStatus::Busy;
            return true;
        }
        if (result.status != HarkenSearch::DiagnosticLogBundleStatus::Written) {
            task.status = ExportStatus::Failed;
            return true;
        }
        task.path = result.path.u8string();
        task.status = result.empty ? ExportStatus::Empty : ExportStatus::Saved;
        return true;
    } catch (...) {
        task.busy.store(false, std::memory_order_relaxed);
        task.status = ExportStatus::Failed;
        return true;
    }
}

bool StartExportForQa() {
    auto& task = ExportTask();
    // The host may skip drawing (hidden window), so settle a finished task here
    // instead of waiting for PollLogExport on the next frame.
    ConsumeFinishedExport(task);
    if (task.pending.valid()) return false;
    return StartLogExport(task);
}

void PollLogExport() {
    auto& task = ExportTask();
    if (!ConsumeFinishedExport(task)) return;
    if (task.status != ExportStatus::Saved && task.status != ExportStatus::Empty) return;
#if ARIS_PREVIEW_ISOLATION && defined(_WIN32)
    const char* qaLane = std::getenv("AURA_HARNESS_LANE_KIND");
    if (qaLane && std::string_view(qaLane) == "functional") return;
#endif
    Platform::OpenInFileManager(task.path);
}

bool DrawWindow(Monitor& monitor, const MemoryBudget* memoryBudget, bool justOpened,
                ImFont* regularFont, ImFont* emphasisFont) {
    static ViewData view;
    static auto preferred = layout::kDefaultPlacement;
    if (justOpened) view = {};
    const auto* viewport = ImGui::GetMainViewport();
    const ImVec2 workOrigin(std::ceil(viewport->WorkPos.x), std::ceil(viewport->WorkPos.y));
    const ImVec2 workEnd(std::floor(viewport->WorkPos.x + viewport->WorkSize.x),
                         std::floor(viewport->WorkPos.y + viewport->WorkSize.y));
    const layout::Size workSize{workEnd.x - workOrigin.x, workEnd.y - workOrigin.y};
    if ((viewport->Flags & ImGuiViewportFlags_IsMinimized) || workSize.width < 1.0f || workSize.height < 1.0f) {
        monitor.Pause();
        return true;
    }
    const auto bounds = layout::WindowBounds(workSize, viewport->DpiScale > 0.0f ? viewport->DpiScale : 1.0f);
    const auto placed = layout::FitPlacement(preferred, bounds);
    const ImVec2 pos(workEnd.x - placed.inset.right - placed.size.width, workOrigin.y + placed.inset.top);
    const ImVec2 size(placed.size.width, placed.size.height);
    auto* window = ImGui::FindWindowByName(kWindowName);
    ImGui::SetNextWindowViewport(viewport->ID);
    if (!window || window->Pos.x != pos.x || window->Pos.y != pos.y) ImGui::SetNextWindowPos(pos);
    if (!window || window->SizeFull.x != size.x || window->SizeFull.y != size.y) ImGui::SetNextWindowSize(size);
    const ImVec2 minimum(bounds.minimum.width, bounds.minimum.height);
    NativeResizeLimits limits{window, bounds, placed};
    ImGui::SetNextWindowSizeConstraints(minimum, ImVec2(workSize.width, workSize.height), LimitNativeResize, &limits);
    const float scale = bounds.scale;
    const auto& theme = ThemeColors::current();
    const ThemeModuleColors modules = ResolveThemeModuleColors(theme);
    auto surface = modules.diagnostic.background;
    surface.w = std::min(surface.w, 0.96f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, surface);
    ImGui::PushStyleColor(ImGuiCol_Border, ImLerp(modules.overlay.border, modules.diagnostic.accent, 0.18f));
    ImGui::PushStyleColor(ImGuiCol_Text, modules.diagnostic.text);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 9.0f * scale);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, minimum);
    constexpr auto flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoFocusOnAppearing;
    bool open = true;
    const bool visible = BeginTransientScrollbarWindow(kWindowName, nullptr, flags);
    const auto actualPos = ImGui::GetWindowPos();
    const auto actualSize = ImGui::GetWindowSize();
    layout::RememberResize(preferred, placed,
        {{actualSize.x, actualSize.y}, {workEnd.x - actualPos.x - actualSize.x, actualPos.y - workOrigin.y}}, scale);
    if (visible) {
        if (!ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel))
            ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());
        auto* regular = regularFont ? regularFont : ImGui::GetFont();
        const layout::Size logicalSize{actualSize.x / scale, actualSize.y / scale};
        const Layout ui{regular, emphasisFont ? emphasisFont : regular, ImGui::GetWindowDrawList(),
                        actualPos, logicalSize, scale, layout::FontScale(logicalSize),
                         modules.diagnostic.text, modules.diagnostic.mutedText};
        open = DrawHeader(ui, view, theme);
        if (open) {
            const auto now = Monitor::Clock::now();
            monitor.Tick(now, ReadProcess);
            Refresh(view, now, memoryBudget);
            if (view.tab == Tab::Performance) DrawPerformance(ui, view, monitor, theme);
            else DrawLogs(ui, view);
        }
    } else {
        monitor.Pause();
    }
    ImGui::End();
    ImGui::PopStyleVar(4);
    ImGui::PopStyleColor(3);
    return open;
}

}
