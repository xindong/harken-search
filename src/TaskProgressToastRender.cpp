#include "TaskProgressToastRender.h"

#include <algorithm>
#include <cfloat>
#include <cstdio>

namespace {

ImVec2 TopLeft(const TaskProgressToastRect& rect) { return ImVec2(rect.x, rect.y); }

ImVec2 Offset(const TaskProgressToastRect& rect, float dx, float dy) {
    return ImVec2(rect.x + dx, rect.y + dy);
}

ImU32 WithAlpha(ImU32 color, float alpha) {
    ImVec4 value = ImGui::ColorConvertU32ToFloat4(color);
    value.w *= std::clamp(alpha, 0.0f, 1.0f);
    return ImGui::ColorConvertFloat4ToU32(value);
}

void DrawCloseGlyph(ImDrawList* drawList, const TaskProgressToastRect& close,
                    ImU32 backplate, ImU32 glyph, bool hovered) {
    if (hovered) {
        drawList->AddRectFilled(TopLeft(close),
                                Offset(close, close.width, close.height), backplate,
                                4.0f);
    }
    const float pad = 4.5f;
    drawList->AddLine(Offset(close, pad, pad),
                      Offset(close, close.width - pad, close.height - pad), glyph,
                      1.4f);
    drawList->AddLine(Offset(close, close.width - pad, pad),
                      Offset(close, pad, close.height - pad), glyph, 1.4f);
}

} // namespace

TaskProgressToastRenderResult RenderTaskProgressToast(
    ImDrawList* drawList, const TaskProgressToastStack& stack,
    const TaskProgressToastPalette& palette, ImVec2 workPos, ImVec2 workSize,
    double now, bool acceptInput) {
    TaskProgressToastRenderResult result;
    if (!drawList || workSize.x <= 0.0f || workSize.y <= 0.0f) return result;

    const auto rows = stack.VisibleRows(now);
    if (rows.empty()) return result;

    const TaskProgressToastRect panel = stack.PanelRect(
        workPos.x, workPos.y, workSize.x, workSize.y, rows.size());
    if (panel.width <= 0.0f || panel.height <= 0.0f) return result;

    const ImGuiIO& io = ImGui::GetIO();
    const TaskProgressToastPoint mouse{io.MousePos.x, io.MousePos.y};
    TaskProgressToastHit hit;
    if (acceptInput) {
        hit = HitTestTaskProgressToast(stack, panel, mouse, rows.size(), now);
    }

    const ImU32 panelBg =
        hit.panel && palette.hover != 0 ? WithAlpha(palette.hover, 0.10f)
                                        : palette.panelBg;
    drawList->AddRectFilled(TopLeft(panel), Offset(panel, panel.width, panel.height),
                            panelBg, palette.rounding);
    if (palette.border != 0) {
        drawList->AddRect(TopLeft(panel), Offset(panel, panel.width, panel.height),
                          palette.border, palette.rounding);
    }

    ImFont* font = ImGui::GetFont();
    for (std::size_t index = 0; index < rows.size(); ++index) {
        const TaskProgressEntry& entry = rows[index];
        const TaskProgressToastRect row = stack.RowRect(panel, index);
        const float alpha = stack.FadeAlpha(entry, now);
        const bool rowHovered = hit.row && hit.rowIndex == index;

        if (rowHovered && palette.hover != 0) {
            drawList->AddRectFilled(
                TopLeft(row), Offset(row, row.width, row.height),
                WithAlpha(palette.hover, 0.10f * alpha), 8.0f);
        }

        const float textLeft = row.x + 12.0f;
        const float textRight = row.x + row.width - 44.0f;
        const float wrapWidth = std::max(1.0f, textRight - textLeft);
        drawList->PushClipRect(TopLeft(row), Offset(row, row.width, row.height), true);

        const bool failed = entry.state == TaskProgressState::Failed;
        const char* title =
            entry.title.empty() ? entry.id.c_str() : entry.title.c_str();
        drawList->AddText(font, palette.titleSize, ImVec2(textLeft, row.y + 9.0f),
                          WithAlpha(failed ? palette.failed : palette.title, alpha),
                          title, nullptr, wrapWidth);
        if (!entry.phase.empty()) {
            drawList->AddText(
                font, palette.phaseSize, ImVec2(textLeft, row.y + 29.0f),
                WithAlpha(failed ? palette.failed : palette.phase, alpha),
                entry.phase.c_str(), nullptr, wrapWidth);
        }

        char percent[16] = {};
        const bool determinate =
            FormatTaskProgressPercent(entry.fraction, percent, sizeof percent);
        const char* trailing = determinate ? percent : "...";
        const ImVec2 trailingSize =
            font->CalcTextSizeA(palette.phaseSize, FLT_MAX, 0.0f, trailing);
        drawList->AddText(
            font, palette.phaseSize,
            ImVec2(row.x + row.width - 34.0f - trailingSize.x, row.y + 9.0f),
            WithAlpha(entry.state == TaskProgressState::Succeeded ? palette.success
                                                                  : palette.phase,
                      alpha),
            trailing);

        if (palette.track != 0) {
            const float trackY = row.y + row.height - 8.0f;
            const float trackLeft = textLeft;
            const float trackRight = row.x + row.width - 12.0f;
            drawList->AddRectFilled(ImVec2(trackLeft, trackY),
                                    ImVec2(trackRight, trackY + 3.0f),
                                    WithAlpha(palette.track, alpha), 1.5f);
            const float span = std::max(0.0f, trackRight - trackLeft);
            if (determinate) {
                const float filled = span * std::clamp(entry.fraction, 0.0f, 1.0f);
                if (filled > 0.0f) {
                    drawList->AddRectFilled(
                        ImVec2(trackLeft, trackY),
                        ImVec2(trackLeft + filled, trackY + 3.0f),
                        WithAlpha(failed ? palette.failed : palette.fill, alpha),
                        1.5f);
                }
            } else {
                const float sweep = TaskProgressIndeterminateSweep(now);
                const float width = span * 0.35f;
                const float start = trackLeft + (span - width) * sweep;
                drawList->AddRectFilled(ImVec2(start, trackY),
                                        ImVec2(start + width, trackY + 3.0f),
                                        WithAlpha(palette.fill, alpha), 1.5f);
            }
        }

        if (rowHovered && palette.closeGlyph != 0) {
            const TaskProgressToastRect close = ToastCloseButtonRect(stack, row);
            DrawCloseGlyph(drawList, close, WithAlpha(palette.hover, 0.30f),
                           WithAlpha(palette.closeGlyph, alpha), hit.close);
        }
        drawList->PopClipRect();
    }

    if (acceptInput && hit.row) {
        result.hovered = true;
        if (ImGui::IsMouseClicked(0)) {
            if (hit.close) {
                result.dismissedId = hit.closeId;
            } else {
                result.activatedId = hit.id;
            }
        }
    }
    return result;
}