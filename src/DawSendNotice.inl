#include "DawSendFeedback.h"
#include "Theme.h"

#include "imgui_internal.h"

#include <algorithm>
#include <cmath>
#include <string_view>
#include <vector>

namespace HarkenSearch::UI {

struct DawSendNoticeObservation {
    ImRect rect{};
    ImRect anchor{};
    ImGuiID viewportId = 0;
    float wrapWidth = 0.0f;
    size_t bodyLines = 0;
    size_t visibleBodyLines = 0;
    size_t page = 0;
    size_t pageCount = 0;
    bool rendered = false;
    bool overflow = false;
    bool degraded = false;
    bool expired = false;
};

static const char* DawSendHeading(DawSendOutcome outcome) {
    switch (outcome) {
    case DawSendOutcome::Idle: return "";
    case DawSendOutcome::Sending: return L(DawSend_Sending);
    case DawSendOutcome::Success: return L(DawSend_Success);
    case DawSendOutcome::Partial: return L(DawSend_Partial);
    case DawSendOutcome::Fallback: return L(DawSend_Fallback);
    case DawSendOutcome::Failure: return L(DawSend_Failure);
    case DawSendOutcome::BridgeFailure: return L(DawSend_BridgeFailure);
    }
    return "";
}

static const char* DawSendNoticeLineEnd(ImFont& font, float fontSize,
                                       const char* start, const char* end, float width) {
    const char* wordBreak = font.CalcWordWrapPosition(fontSize, start, end, width);
    if (wordBreak == end) return end;

    const char* fittingEnd = nullptr;
    font.CalcTextSizeA(fontSize, width + 0.01f, 0.0f, start, end, &fittingEnd);
    const char* lineEnd = wordBreak;
    for (const char* cursor = wordBreak; cursor < fittingEnd;) {
        unsigned int codepoint = 0;
        cursor += ImTextCharFromUtf8(&codepoint, cursor, fittingEnd);
        const bool cjk = (codepoint >= 0x3040 && codepoint <= 0x30FF) ||
            (codepoint >= 0x3400 && codepoint <= 0x9FFF) ||
            (codepoint >= 0xAC00 && codepoint <= 0xD7AF) ||
            (codepoint >= 0xF900 && codepoint <= 0xFAFF) ||
            (codepoint >= 0x20000 && codepoint <= 0x323AF);
        constexpr std::u32string_view trailingPunctuation = U"、。，！？；：）》」』…";
        if (cjk || (lineEnd > wordBreak && trailingPunctuation.find(codepoint) != std::u32string_view::npos))
            lineEnd = cursor;
    }
    return lineEnd;
}

static std::vector<std::string_view> WrapDawSendNoticeText(
    std::string_view text, ImFont& font, float fontSize, float width) {
    std::vector<std::string_view> lines;
    if (text.empty()) return lines;

    size_t paragraphStart = 0;
    while (paragraphStart < text.size()) {
        const size_t newline = text.find('\n', paragraphStart);
        const size_t paragraphEnd = newline == std::string_view::npos ? text.size() : newline;
        const char* lineStart = text.data() + paragraphStart;
        const char* end = text.data() + paragraphEnd;
        if (lineStart == end) lines.emplace_back();
        while (lineStart < end) {
            const char* lineEnd = DawSendNoticeLineEnd(font, fontSize, lineStart, end, width);
            if (lineEnd <= lineStart) break;
            lines.emplace_back(lineStart, static_cast<size_t>(lineEnd - lineStart));
            lineStart = ImTextCalcWordWrapNextLineStart(lineEnd, end);
        }
        if (newline == std::string_view::npos) break;
        paragraphStart = newline + 1;
    }
    return lines;
}

static void DrawDawSendNoticeIcon(ImDrawList& draw, const ImVec2& center, float scale,
                                  ImU32 color, DawSendOutcome outcome, double elapsed) {
    const float stroke = std::max(1.0f, 1.4f * scale);
    const auto point = [&](float dx, float dy) {
        return ImVec2(center.x + dx * scale, center.y + dy * scale);
    };
    const auto line = [&](float startX, float startY, float endX, float endY) {
        draw.AddLine(point(startX, startY), point(endX, endY), color, stroke);
    };
    switch (outcome) {
    case DawSendOutcome::Idle:
        break;
    case DawSendOutcome::Sending: {
        const float angle = static_cast<float>(std::fmod(elapsed, 1.0)) * 2.0f * IM_PI;
        draw.PathArcTo(center, 4.2f * scale, angle, angle + 1.5f * IM_PI, 14);
        draw.PathStroke(color, ImDrawFlags_None, stroke);
        break;
    }
    case DawSendOutcome::Success:
        line(-4.0f, 0.0f, -1.2f, 3.0f);
        line(-1.2f, 3.0f, 4.1f, -3.1f);
        break;
    case DawSendOutcome::Partial:
        draw.AddCircle(center, 4.3f * scale, color, 0, stroke);
        draw.PathArcTo(center, 4.2f * scale, 0.5f * IM_PI, 1.5f * IM_PI, 10);
        draw.PathFillConvex(color);
        break;
    case DawSendOutcome::Fallback:
        line(-4.0f, 2.8f, -4.0f, -1.0f);
        line(-4.0f, -1.0f, 3.6f, -1.0f);
        line(1.3f, -3.4f, 3.7f, -1.0f);
        line(3.7f, -1.0f, 1.3f, 1.4f);
        break;
    case DawSendOutcome::Failure:
        line(-3.2f, -3.2f, 3.2f, 3.2f);
        line(3.2f, -3.2f, -3.2f, 3.2f);
        break;
    case DawSendOutcome::BridgeFailure:
        draw.AddCircle(center, 4.6f * scale, color, 0, stroke);
        line(0.0f, -2.5f, 0.0f, 0.4f);
        draw.AddCircleFilled(point(0.0f, 2.2f), 0.8f * scale, color);
        break;
    }
}

static DawSendNoticeObservation DrawDawSendNotice(
    const DawSendFeedbackSnapshot& snapshot, ImGuiViewport* viewport, const ImRect& anchor,
    double elapsed, float previewWidth = 0.0f, size_t previewPage = 0) {
    DawSendNoticeObservation observation;
    if (!viewport || snapshot.outcome == DawSendOutcome::Idle) return observation;

    observation.anchor = anchor;
    observation.viewportId = viewport->ID;
    ImFont& font = *ImGui::GetFont();
    const float fontSize = ImGui::GetFontSize();
    const float scale = fontSize / 14.0f;
    const float lineHeight = std::ceil(fontSize * 1.25f);
    const float textInset = 24.0f * scale;
    const float endPadding = 12.0f * scale;
    const float verticalPadding = 8.0f * scale;
    const float margin = 8.0f * scale;
    const float left = std::ceil(viewport->WorkPos.x + margin);
    const float right = std::floor(std::min(anchor.Max.x,
        viewport->WorkPos.x + viewport->WorkSize.x - margin));
    const float top = std::ceil(viewport->WorkPos.y + margin);
    const float bottom = std::floor(std::min(anchor.Min.y - 6.0f * scale,
        viewport->WorkPos.y + viewport->WorkSize.y - margin));
    const float widthLimit = previewWidth > 0.0f ? std::min(324.0f * scale, previewWidth) : 324.0f * scale;
    const float width = std::floor(std::min(right - left, widthLimit));
    const float availableHeight = std::min(bottom - top, viewport->WorkSize.y * 0.55f);
    const float wrapWidth = width - textInset - endPadding;
    if (wrapWidth < fontSize * 2.0f || availableHeight < lineHeight + verticalPadding * 2.0f) {
        observation.degraded = true;
        observation.expired = previewPage == 0 && snapshot.outcome != DawSendOutcome::Sending && elapsed >= 3.5;
        return observation;
    }
    observation.wrapWidth = wrapWidth;
    const size_t lineCapacity = static_cast<size_t>(std::floor((availableHeight - verticalPadding * 2.0f) / lineHeight));

    std::string heading = DawSendHeading(snapshot.outcome);
    if (snapshot.sent) {
        heading += " " + std::to_string(*snapshot.sent);
        if (snapshot.total) heading += "/" + std::to_string(*snapshot.total);
    }
    std::string body = snapshot.detail.Render();
    if (!snapshot.remediation.empty()) {
        if (!body.empty()) body += '\n';
        body += snapshot.remediation.Render();
    }
    const auto headingLines = WrapDawSendNoticeText(heading, font, fontSize, wrapWidth);
    const auto bodyLines = WrapDawSendNoticeText(body, font, fontSize, wrapWidth);
    observation.bodyLines = bodyLines.size();
    if (headingLines.size() > lineCapacity) {
        observation.degraded = true;
        observation.overflow = true;
        observation.expired = previewPage == 0 && snapshot.outcome != DawSendOutcome::Sending && elapsed >= 3.5;
        return observation;
    }

    size_t pageCapacity = lineCapacity - headingLines.size();
    observation.overflow = bodyLines.size() > pageCapacity;
    const bool showPageNumber = observation.overflow && pageCapacity > 1;
    if (showPageNumber) --pageCapacity;
    observation.degraded = observation.overflow && pageCapacity == 0;
    const size_t pageCount = pageCapacity == 0 ? 1 :
        std::max<size_t>(1, (bodyLines.size() + pageCapacity - 1) / pageCapacity);
    const double pageReadingSeconds = std::max(3.0,
        1.5 + 1.15 * static_cast<double>(headingLines.size() + std::min(bodyLines.size(), pageCapacity)));
    const double fadeStart = pageReadingSeconds * static_cast<double>(pageCount);
    const bool timed = previewPage == 0 && snapshot.outcome != DawSendOutcome::Sending;
    if (timed && elapsed >= fadeStart + 0.5) {
        observation.expired = true;
        return observation;
    }
    const float alpha = timed && elapsed > fadeStart ? static_cast<float>(1.0 - (elapsed - fadeStart) / 0.5) : 1.0f;
    const size_t page = previewPage > 0 ? std::min(pageCount, previewPage) - 1 :
        static_cast<size_t>(std::clamp(elapsed / pageReadingSeconds, 0.0, static_cast<double>(pageCount - 1)));
    const size_t firstLine = page * pageCapacity;
    const size_t visibleBodyLines = std::min(pageCapacity, bodyLines.size() - firstLine);
    const float height = lineHeight * static_cast<float>(
        headingLines.size() + visibleBodyLines + (showPageNumber ? 1 : 0)) + verticalPadding * 2.0f;
    const ImVec2 min(right - width, bottom - height);
    const ImVec2 max(right, bottom);

    const auto& theme = ThemeColors::current();
    const auto tint = [alpha](ImVec4 color) {
        color.w *= alpha;
        return ImGui::GetColorU32(color);
    };
    ImDrawList* draw = ImGui::GetForegroundDrawList(viewport);
    draw->AddRectFilled(min, max, tint(ThemeColors::popupMenuBG(theme)), 9.0f * scale);
    draw->AddRect(min, max, tint(ThemeColors::popupBorder(theme)), 9.0f * scale);
    DrawDawSendNoticeIcon(*draw, ImVec2(min.x + 13.0f * scale, min.y + verticalPadding + fontSize * 0.5f),
        scale, tint(theme.controlAccent), snapshot.outcome, elapsed);

    float y = min.y + verticalPadding;
    const float textX = min.x + textInset;
    const auto drawLine = [&](std::string_view line, ImU32 color) {
        if (!line.empty())
            draw->AddText(&font, fontSize, ImVec2(textX, y), color, line.data(), line.data() + line.size());
        y += lineHeight;
    };
    for (const auto line : headingLines) drawLine(line, tint(ImGui::GetStyleColorVec4(ImGuiCol_Text)));
    for (size_t index = 0; index < visibleBodyLines; ++index)
        drawLine(bodyLines[firstLine + index], tint(theme.interfaceText));
    if (showPageNumber) drawLine(std::to_string(page + 1) + " / " + std::to_string(pageCount), tint(theme.interfaceText));

    observation.rect = ImRect(min, max);
    observation.visibleBodyLines = visibleBodyLines;
    observation.page = page + 1;
    observation.pageCount = pageCount;
    observation.rendered = true;
    return observation;
}

}
