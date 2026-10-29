// src/HarkenSearchWidgets.cpp
// Aris UI v1 - widget implementations.
#include "HarkenSearchWidgets.h"
#include "HarkenSearchMetrics.h"
#include "HarkenSearchIcons.h"
#include "Theme.h"
#include "ui/TransportContrast.h"
#include "ui/ActionButtonStyle.h"
#include "SettingsSearchHighlightSpans.h"
#include "imgui.h"
#include "imgui_internal.h"

#include <algorithm>
#include <cfloat>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

extern bool g_showGuidedTooltips;

namespace aura::ui {

namespace {

static bool g_settingsSearchActive = false;
static std::string g_settingsSearchQuery;
static std::string g_settingsSearchKeywords;
static std::string g_settingsSearchActiveLabel;
static bool g_settingsSearchHitCaptured = false;
static ImGuiID g_settingsSearchLastOverlayId = 0;
static ImVec2 g_settingsSearchHitMin{};
static ImVec2 g_settingsSearchHitMax{};
static std::string g_settingsSearchPulseQuery;
static std::string g_settingsSearchPulseLabel;
static std::string g_settingsSearchPulseKeywords;
static double g_settingsSearchPulseT0 = 0.0;

std::string VisibleSettingsLabel(const char* label) {
    if (!label || !label[0]) return {};
    const char* hash = std::strstr(label, "##");
    if (hash == label) return {};
    if (hash) return std::string(label, static_cast<size_t>(hash - label));
    return label;
}

void CaptureActiveHitRect() {
    if (g_settingsSearchHitCaptured) return;
    if (g_settingsSearchActiveLabel.empty()) return;
    g_settingsSearchHitMin = ImGui::GetItemRectMin();
    g_settingsSearchHitMax = ImGui::GetItemRectMax();
    g_settingsSearchHitCaptured = true;
}

void OverlayLabelHighlight(const std::string& label, SettingsSearchLabelKind kind, const ImVec2* textOrigin) {
    if (!g_settingsSearchActive || g_settingsSearchQuery.empty() || label.empty()) return;
    if (!SettingsLabelIsCurrentHit(label, g_settingsSearchActiveLabel)) return;
    const ImGuiID itemId = ImGui::GetItemID();
    if (itemId != 0 && itemId == g_settingsSearchLastOverlayId) return;
    if (itemId != 0) g_settingsSearchLastOverlayId = itemId;
    std::vector<std::pair<size_t, size_t>> spans =
        BuildSettingsLabelHighlightSpans(label, g_settingsSearchQuery, g_settingsSearchKeywords);
    EnsureActiveSettingsLabelHighlight(spans, label, g_settingsSearchQuery, g_settingsSearchActiveLabel);
    if (spans.empty()) return;

    const ImVec2 itemMin = ImGui::GetItemRectMin();
    const ImVec2 itemMax = ImGui::GetItemRectMax();
    ImFont* font = ImGui::GetFont();
    const float fontSize = ImGui::GetFontSize();
    const ImGuiStyle& style = ImGui::GetStyle();
    float textX = itemMin.x;
    float textY = itemMin.y;
    if (textOrigin) {
        textX = textOrigin->x;
        textY = textOrigin->y;
    } else if (kind == SettingsSearchLabelKind::Check) {
        textX += ImGui::GetFrameHeight() + style.ItemInnerSpacing.x;
        textY += style.FramePadding.y;
    } else if (kind == SettingsSearchLabelKind::Button) {
        const ImVec2 textSize = ImGui::CalcTextSize(label.c_str());
        textX += (itemMax.x - itemMin.x - textSize.x) * 0.5f;
        textY += (itemMax.y - itemMin.y - textSize.y) * 0.5f;
    } else if (kind == SettingsSearchLabelKind::Separator) {
        textX += style.SeparatorTextPadding.x;
        // Mirror SeparatorTextEx's full horizontal formula (imgui_widgets.cpp label_pos)
        // so the highlight follows SeparatorTextAlign.x if a theme ever sets it non-zero.
        const float availW = ImMax(0.0f, (itemMax.x - itemMin.x) - style.SeparatorTextPadding.x * 2.0f);
        const float titleW = ImGui::CalcTextSize(label.c_str()).x;
        textX += ImMax(0.0f, (availW - titleW) * style.SeparatorTextAlign.x);
        const float bbH = itemMax.y - itemMin.y;
        textY += IM_TRUNC((bbH - fontSize) * style.SeparatorTextAlign.y + 0.99999f);
    } else {
        textY += style.FramePadding.y;
    }

    const ImVec4 selectionColor = ThemeColors::current().resultsSelection;
    const float elapsed = static_cast<float>(ImGui::GetTime() - g_settingsSearchPulseT0);
    const float alpha = SettingsSearchHighlightAlpha(elapsed);
    const ImU32 highlightBgColor =
        ImGui::GetColorU32(ImVec4(selectionColor.x, selectionColor.y, selectionColor.z, alpha));
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const char* textBegin = label.c_str();
    float x = textX;
    size_t cursor = 0;
    const float highlightPadX = 2.0f;
    const float highlightPadY = 1.0f;
    for (const auto& span : spans) {
        if (span.first > cursor) {
            x += ImGui::CalcTextSize(textBegin + cursor, textBegin + span.first).x;
            cursor = span.first;
        }
        const size_t hiEnd = (std::min)(label.size(), span.second);
        if (hiEnd <= cursor) continue;
        const ImVec2 hiSize = ImGui::CalcTextSize(textBegin + cursor, textBegin + hiEnd);
        drawList->AddRectFilled(
            ImVec2(x - highlightPadX, textY - highlightPadY),
            ImVec2(x + hiSize.x + highlightPadX, textY + fontSize + highlightPadY),
            highlightBgColor,
            3.0f);
        x += hiSize.x;
        cursor = hiEnd;
    }
    (void)font;
}

} // namespace

void BeginSettingsSearchHighlight(const char* query, const char* activeLabel, const char* keywords) {
    g_settingsSearchActive = query && query[0];
    g_settingsSearchQuery = g_settingsSearchActive ? query : "";
    g_settingsSearchKeywords = (g_settingsSearchActive && keywords) ? keywords : "";
    g_settingsSearchActiveLabel = activeLabel ? activeLabel : "";
    g_settingsSearchHitCaptured = false;
    g_settingsSearchLastOverlayId = 0;
    g_settingsSearchHitMin = ImVec2();
    g_settingsSearchHitMax = ImVec2();
    if (!g_settingsSearchActive) {
        g_settingsSearchPulseQuery.clear();
        g_settingsSearchPulseLabel.clear();
        g_settingsSearchPulseKeywords.clear();
        g_settingsSearchPulseT0 = 0.0;
        return;
    }
    if (g_settingsSearchQuery != g_settingsSearchPulseQuery ||
        g_settingsSearchActiveLabel != g_settingsSearchPulseLabel ||
        g_settingsSearchKeywords != g_settingsSearchPulseKeywords) {
        g_settingsSearchPulseQuery = g_settingsSearchQuery;
        g_settingsSearchPulseLabel = g_settingsSearchActiveLabel;
        g_settingsSearchPulseKeywords = g_settingsSearchKeywords;
        g_settingsSearchPulseT0 = ImGui::GetTime();
    }
}

void EndSettingsSearchHighlight() {
    g_settingsSearchActive = false;
    g_settingsSearchQuery.clear();
    g_settingsSearchKeywords.clear();
    g_settingsSearchActiveLabel.clear();
    g_settingsSearchPulseQuery.clear();
    g_settingsSearchPulseLabel.clear();
    g_settingsSearchPulseKeywords.clear();
    g_settingsSearchPulseT0 = 0.0;
}

void NotifySettingsSearchLabeledItem(const char* label, SettingsSearchLabelKind kind, const ImVec2* textOrigin) {
    const std::string visible = VisibleSettingsLabel(label);
    if (visible.empty()) return;
    if (!g_settingsSearchQuery.empty() && SettingsLabelIsCurrentHit(visible, g_settingsSearchActiveLabel)) {
        CaptureActiveHitRect();
    }
    OverlayLabelHighlight(visible, kind, textOrigin);
}

void DrawSettingsSearchableLabel(const char* label) {
    ImGui::TextUnformatted(label ? label : "");
    NotifySettingsSearchLabeledItem(label, SettingsSearchLabelKind::Text);
}

bool TakeSettingsSearchHitLayout(ImVec2* outMin, ImVec2* outMax) {
    if (!g_settingsSearchHitCaptured || !outMin || !outMax) return false;
    *outMin = g_settingsSearchHitMin;
    *outMax = g_settingsSearchHitMax;
    return true;
}

// Focus ring (hooked from any widget after its ImGui submit)

void DrawFocusRing() {
    if (!focus::IsKeyboardInputActive()) return;
    if (!ImGui::IsItemFocused())         return;

    ImVec2 pMin = ImGui::GetItemRectMin();
    ImVec2 pMax = ImGui::GetItemRectMax();
    const float gap = aura::ui::metrics::kFocusRingGap;
    pMin.x -= gap; pMin.y -= gap;
    pMax.x += gap; pMax.y += gap;

    const ThemeColorScheme& th = ThemeColors::current();
    ImVec4 c = th.controlAccent;
    c.w *= aura::ui::metrics::kFocusRingAlpha;

    ImGui::GetWindowDrawList()->AddRect(
        pMin, pMax,
        ImGui::ColorConvertFloat4ToU32(c),
        aura::ui::metrics::kRadiusDefault + gap,
        0,
        aura::ui::metrics::kFocusRingWidth);
}

// Shared helpers

namespace {

// Linear interpolate RGBA (t positive = toward b, negative = away) and pack.
ImU32 LerpPack(const ImVec4& a, const ImVec4& b, float t) {
    ImVec4 r{ a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t,
              a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t };
    return ImGui::ColorConvertFloat4ToU32(r);
}

float ButtonHeight(ButtonSize s) {
    using namespace aura::ui::metrics;
    return s == ButtonSize::Sm ? kHeightSm : s == ButtonSize::Lg ? kHeightLg : kHeightMd;
}
float ButtonPadX(ButtonSize s) {
    using namespace aura::ui::metrics;
    return s == ButtonSize::Sm ? kPadXSm : s == ButtonSize::Lg ? kPadXLg : kPadXMd;
}

struct ButtonPalette { ImU32 bg, bgHover, bgActive, text; };

ButtonPalette ResolveGraphicPalette(ButtonVariant variant) {
    const auto& theme = ThemeColors::current();
    ImVec4 hover = theme.filterBrowserButtonBG;
    hover.w *= variant == ButtonVariant::Icon ? .50f : .40f;
    return {IM_COL32(0, 0, 0, 0), ImGui::ColorConvertFloat4ToU32(hover),
            ImGui::ColorConvertFloat4ToU32(theme.filterBrowserButtonBG),
            ImGui::ColorConvertFloat4ToU32(variant == ButtonVariant::Icon
                ? theme.sidebarIconInactive : theme.interfaceText)};
}

} // anonymous namespace

// Button (Task 7)

bool Button(const char* label, const ButtonOptions& opts) {
    using namespace aura::ui::metrics;
    const float h = ButtonHeight(opts.size);
    const float vertPad = ImMax(0.0f, (h - ImGui::GetFontSize()) * .5f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(ButtonPadX(opts.size), vertPad));
    if (opts.disabled) ImGui::BeginDisabled();
    char buf[512];
    if (opts.icon && label) std::snprintf(buf, sizeof(buf), "%s  %s", opts.icon, label);
    else std::snprintf(buf, sizeof(buf), "%s", opts.icon ? opts.icon : label ? label : "");
    bool clicked;
    if (opts.variant == ButtonVariant::Icon || opts.variant == ButtonVariant::Ghost) {
        // Compact glyph tools keep their transparent appearance and old geometry.
        const auto palette = ResolveGraphicPalette(opts.variant);
        ImGui::PushStyleColor(ImGuiCol_Button, palette.bg);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, palette.bgHover);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, palette.bgActive);
        ImGui::PushStyleColor(ImGuiCol_Text, palette.text);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, kRadiusDefault);
        clicked = opts.loading ? ActionButtonStyle::Draw(buf, ImVec2(0, h), true)
                               : ImGui::Button(buf, ImVec2(0, h));
        DrawFocusRing();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(4);
    } else {
        const ActionButtonStyle::Scope style(ActionButtonStyle::Make(
            ThemeColors::current(), ActionButtonStyle::CurrentSurface(), opts.variant));
        clicked = ActionButtonStyle::Draw(buf, ImVec2(0, h), opts.loading);
        DrawFocusRing();
    }
    if (opts.disabled) ImGui::EndDisabled();
    ImGui::PopStyleVar();
    if (opts.tooltip && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", opts.tooltip);
    return clicked && !opts.disabled && !opts.loading;
}

bool ActionButton(const char* label, const ImVec2& size, ButtonVariant variant) {
    const ActionButtonStyle::Scope style(ActionButtonStyle::Make(
        ThemeColors::current(), ActionButtonStyle::CurrentSurface(), variant));
    const bool clicked = ActionButtonStyle::Draw(label, size);
    DrawFocusRing();
    return clicked;
}

bool IconButton(const char* icon, const char* tooltip, ButtonSize size) {
    ButtonOptions o;
    o.variant = ButtonVariant::Icon;
    o.size    = size;
    o.tooltip = tooltip;
    // Pass icon as label (anonymous-id trick: ImGui::Button uses the full
    // string as its ID; Lucide glyphs are unique enough here in practice).
    return Button(icon, o);
}

bool SmallActionButton(const char* label, ButtonVariant variant) {
    const ActionButtonStyle::Scope style(ActionButtonStyle::Make(
        ThemeColors::current(), ActionButtonStyle::CurrentSurface(), variant));
    const bool clicked = ImGui::SmallButton(label);
    ActionButtonStyle::Record(label);
    DrawFocusRing();
    return clicked;
}

// TextInput / NumberInput (Task 8)

namespace {
void DrawInputLabel(const char* label, const ThemeColorScheme& th) {
    if (!label || label[0] == '\0' || label[0] == '#') return;
    ImGui::PushStyleColor(ImGuiCol_Text,
        ImGui::ColorConvertFloat4ToU32(th.interfaceText));
    ImGui::TextUnformatted(label);
    ImGui::PopStyleColor();
    ImGui::Dummy(ImVec2(0, aura::ui::metrics::kGap4));
}
} // anon

bool TextInput(const char* label, std::string* value,
               const TextInputOptions& opts, bool* trailingClicked) {
    using namespace aura::ui::metrics;
    const ThemeColorScheme& th = ThemeColors::current();

    DrawInputLabel(label, th);

    const ImVec4 whiteV{1,1,1,1};
    const ImGuiStyle& imguiStyle = ImGui::GetStyle();
    const ImVec4 frameBg = opts.inheritContextColors
        ? imguiStyle.Colors[ImGuiCol_FrameBg]
        : (opts.readonly ? th.popupBG : th.frameBgBase);
    const ImVec4 frameBgHovered = opts.inheritContextColors
        ? imguiStyle.Colors[ImGuiCol_FrameBgHovered]
        : ImGui::ColorConvertU32ToFloat4(LerpPack(th.frameBgBase, whiteV, 0.04f));
    const ImVec4 frameBgActive = opts.inheritContextColors
        ? imguiStyle.Colors[ImGuiCol_FrameBgActive]
        : th.frameBgBase;
    const ImVec4 border = opts.errorText
        ? th.volumeClipColor
        : (opts.inheritContextColors ? imguiStyle.Colors[ImGuiCol_Border] : th.chromeBorder);
    const ImVec4 text = opts.inheritContextColors
        ? imguiStyle.Colors[ImGuiCol_Text]
        : th.interfaceText;
    ImGui::PushStyleColor(ImGuiCol_FrameBg,
        ImGui::ColorConvertFloat4ToU32(frameBg));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered,
        ImGui::ColorConvertFloat4ToU32(frameBgHovered));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive,
        ImGui::ColorConvertFloat4ToU32(frameBgActive));
    ImGui::PushStyleColor(ImGuiCol_Border,
        ImGui::ColorConvertFloat4ToU32(border));
    ImGui::PushStyleColor(ImGuiCol_Text,
        ImGui::ColorConvertFloat4ToU32(text));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, kRadiusDefault);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(kPadXMd, kPadYMd));

    if (opts.disabled) ImGui::BeginDisabled();

    const bool hasTrailing = opts.trailingIcon != nullptr;
    const float leadingW   = opts.leadingIcon
        ? (ImGui::CalcTextSize(opts.leadingIcon).x + kGap4) : 0.0f;
    const float trailingW  = hasTrailing ? (kIconMd + kPadXMd * 2 + kGap8) : 0.0f;
    const float avail      = opts.width > 0.0f ? opts.width
                                                : ImGui::GetContentRegionAvail().x;
    const float inputW     = avail - trailingW - leadingW;

    ImGui::PushID(label ? label : "##txt");
    ImGui::BeginGroup();

    if (opts.leadingIcon) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(opts.leadingIcon);
        ImGui::SameLine(0, kGap4);
    }

    ImGuiInputTextFlags flags = 0;
    if (opts.readonly)                                    flags |= ImGuiInputTextFlags_ReadOnly;
    if (opts.variant == TextInputVariant::Password)       flags |= ImGuiInputTextFlags_Password;
    if (opts.variant == TextInputVariant::Number)         flags |= ImGuiInputTextFlags_CharsDecimal;
    flags |= opts.inputFlags;

    // Work in a local buffer - ImGui::InputText writes back here, then
    // we copy into *value. 1024 bytes covers typical fields; raise if needed.
    char buf[1024];
    const size_t nCopy = value->size() < sizeof(buf) - 1 ? value->size() : sizeof(buf) - 1;
    if (nCopy > 0) std::memcpy(buf, value->data(), nCopy);
    buf[nCopy] = '\0';

    ImGui::SetNextItemWidth(inputW > 0 ? inputW : 80.0f);
    bool changed = false;
    if (opts.variant == TextInputVariant::Multiline) {
        const ImVec2 size(inputW > 0 ? inputW : 200.0f,
                          kHeightMd * (float)(opts.multilineRows > 0 ? opts.multilineRows : 3));
        changed = ImGui::InputTextMultiline("##in", buf, sizeof(buf), size, flags);
    } else {
        changed = ImGui::InputTextWithHint("##in",
            opts.placeholder ? opts.placeholder : "",
            buf, sizeof(buf), flags);
    }
    DrawFocusRing();

    if (changed) *value = buf;
    if (opts.maxLength > 0 && (int)value->size() > opts.maxLength) {
        value->resize((size_t)opts.maxLength);
    }

    if (hasTrailing) {
        ImGui::SameLine(0, kGap8);
        if (IconButton(opts.trailingIcon, opts.trailingTooltip, ButtonSize::Md)) {
            if (trailingClicked) *trailingClicked = true;
        }
    }
    ImGui::EndGroup();
    ImGui::PopID();

    if (opts.disabled) ImGui::EndDisabled();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(5);

    if (opts.errorText) {
        ImGui::PushStyleColor(ImGuiCol_Text,
            ImGui::ColorConvertFloat4ToU32(th.volumeClipColor));
        ImGui::TextUnformatted(opts.errorText);
        ImGui::PopStyleColor();
    }
    return changed;
}

// Checkbox / Radio / Toggle (Task 9)

bool Checkbox(const char* label, bool* value, bool disabled) {
    using namespace aura::ui::metrics;
    const ThemeColorScheme& th = ThemeColors::current();
    const ImVec4 whiteV{1,1,1,1};
    ImGui::PushStyleColor(ImGuiCol_FrameBg,
        ImGui::ColorConvertFloat4ToU32(th.frameBgBase));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered,
        LerpPack(th.frameBgBase, whiteV, 0.08f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive,
        ImGui::ColorConvertFloat4ToU32(th.controlAccent));
    ImGui::PushStyleColor(ImGuiCol_CheckMark,
        ImGui::ColorConvertFloat4ToU32(th.controlAccent));
    ImGui::PushStyleColor(ImGuiCol_Text,
        ImGui::ColorConvertFloat4ToU32(th.interfaceText));
    ImGui::PushStyleColor(ImGuiCol_Border,
        ImGui::ColorConvertFloat4ToU32(th.chromeBorder));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, kRadiusTight);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);

    if (disabled) ImGui::BeginDisabled();
    const bool changed = ImGui::Checkbox(label ? label : "##cb", value);
    DrawFocusRing();
    NotifySettingsSearchLabeledItem(label, SettingsSearchLabelKind::Check);
    if (disabled) ImGui::EndDisabled();

    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(6);
    return changed;
}

bool Radio(const char* label, int* currentValue, int thisValue, bool disabled) {
    const ThemeColorScheme& th = ThemeColors::current();
    const ImVec4 callerText = ImGui::GetStyleColorVec4(ImGuiCol_Text);
    ImGui::PushStyleColor(ImGuiCol_Text, callerText);
    ImGui::PushStyleColor(ImGuiCol_CheckMark,
        ImGui::ColorConvertFloat4ToU32(th.controlAccent));
    ImGui::PushStyleColor(ImGuiCol_FrameBg,
        ImGui::ColorConvertFloat4ToU32(th.frameBgBase));

    if (disabled) ImGui::BeginDisabled();
    const bool changed = ImGui::RadioButton(label ? label : "##rb",
                                            currentValue, thisValue);
    DrawFocusRing();
    NotifySettingsSearchLabeledItem(label, SettingsSearchLabelKind::Check);
    if (disabled) ImGui::EndDisabled();

    ImGui::PopStyleColor(3);
    return changed;
}

bool Toggle(const char* label, bool* value, bool disabled) {
    using namespace aura::ui::metrics;
    const ThemeColorScheme& th = ThemeColors::current();
    const float trackW = 36.0f, trackH = 18.0f;

    ImGui::PushID(label ? label : "##tg");
    const ImVec2 p = ImGui::GetCursorScreenPos();

    if (disabled) ImGui::BeginDisabled();
    ImGui::InvisibleButton("##track", ImVec2(trackW, trackH));
    const bool clicked = ImGui::IsItemClicked();
    if (clicked && !disabled) *value = !*value;

    const ImU32 trackCol = *value
        ? ImGui::ColorConvertFloat4ToU32(th.controlAccent)
        : ImGui::ColorConvertFloat4ToU32(th.frameBgBase);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + trackW, p.y + trackH),
                      trackCol, trackH * 0.5f);

    const float thumbR  = trackH * 0.40f;
    const float thumbCx = *value ? (p.x + trackW - trackH * 0.5f)
                                 : (p.x + trackH * 0.5f);
    dl->AddCircleFilled(ImVec2(thumbCx, p.y + trackH * 0.5f),
        thumbR,
        ImGui::ColorConvertFloat4ToU32(th.interfaceText), 20);

    DrawFocusRing();

    if (label && label[0] && label[0] != '#') {
        ImGui::SameLine(0, kGap8);
        ImGui::AlignTextToFramePadding();
        ImGui::PushStyleColor(ImGuiCol_Text,
            ImGui::ColorConvertFloat4ToU32(th.interfaceText));
        ImGui::TextUnformatted(label);
        ImGui::PopStyleColor();
    }
    if (disabled) ImGui::EndDisabled();
    ImGui::PopID();
    return clicked;
}

bool NumberInput(const char* label, float* value, float min, float max,
                 float step, const char* format, const TextInputOptions& opts) {
    using namespace aura::ui::metrics;
    const ThemeColorScheme& th = ThemeColors::current();

    DrawInputLabel(label, th);

    ImGui::PushStyleColor(ImGuiCol_FrameBg,
        ImGui::ColorConvertFloat4ToU32(th.frameBgBase));
    ImGui::PushStyleColor(ImGuiCol_Border,
        ImGui::ColorConvertFloat4ToU32(th.chromeBorder));
    ImGui::PushStyleColor(ImGuiCol_Text,
        ImGui::ColorConvertFloat4ToU32(th.interfaceText));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, kRadiusDefault);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(kPadXMd, kPadYMd));

    if (opts.disabled) ImGui::BeginDisabled();
    ImGui::PushID(label ? label : "##num");
    const bool changed = ImGui::DragFloat("##v", value, step, min, max, format);
    DrawFocusRing();
    ImGui::PopID();
    if (opts.disabled) ImGui::EndDisabled();

    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(3);
    return changed;
}

// Slider (Task 10)

bool SliderFloat(const char* label, float* value, float min, float max,
                 const char* format, bool showValueInline) {
    using namespace aura::ui::metrics;
    const ThemeColorScheme& th = ThemeColors::current();
    ImGui::PushStyleColor(ImGuiCol_FrameBg,
        ImGui::ColorConvertFloat4ToU32(th.frameBgBase));
    ImGui::PushStyleColor(ImGuiCol_SliderGrab,
        ImGui::ColorConvertFloat4ToU32(th.interfaceText));
    ImGui::PushStyleColor(ImGuiCol_SliderGrabActive,
        ImGui::ColorConvertFloat4ToU32(th.menuText));
    ImGui::PushStyleColor(ImGuiCol_Text,
        ImGui::ColorConvertFloat4ToU32(th.interfaceText));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, kRadiusDefault);
    const bool changed = ImGui::SliderFloat(label ? label : "##sf",
        value, min, max, showValueInline ? format : "");
    DrawFocusRing();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(4);
    return changed;
}

bool SliderRange(const char* label, float* minVal, float* maxVal,
                 float min, float max, const char* format) {
    const ThemeColorScheme& th = ThemeColors::current();
    if (label && label[0] && label[0] != '#') {
        DrawInputLabel(label, th);
    }
    ImGui::PushID(label ? label : "##sr");
    // Ensure minVal <= maxVal invariant by clamping slider ranges.
    const bool c1 = SliderFloat("min", minVal, min, *maxVal, format, true);
    const bool c2 = SliderFloat("max", maxVal, *minVal, max, format, true);
    ImGui::PopID();
    return c1 || c2;
}

// Combo / ComboMulti (Task 11)

bool Combo(const char* label, int* currentIndex,
           const char* const items[], int itemCount, ComboVariant /*variant*/) {
    using namespace aura::ui::metrics;
    const ThemeColorScheme& th = ThemeColors::current();
    const ImVec4 whiteV{1,1,1,1};
    ImGui::PushStyleColor(ImGuiCol_FrameBg,
        ImGui::ColorConvertFloat4ToU32(th.frameBgBase));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered,
        LerpPack(th.frameBgBase, whiteV, 0.04f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive,
        ImGui::ColorConvertFloat4ToU32(th.frameBgBase));
    ImGui::PushStyleColor(ImGuiCol_Border,
        ImGui::ColorConvertFloat4ToU32(th.chromeBorder));
    ImGui::PushStyleColor(ImGuiCol_PopupBg,
        ImGui::ColorConvertFloat4ToU32(ThemeColors::popupComboBG(th)));
    ImGui::PushStyleColor(ImGuiCol_BorderShadow,
        ImGui::ColorConvertFloat4ToU32(ThemeColors::popupShadow(th)));
    ImGui::PushStyleColor(ImGuiCol_Text,
        ImGui::ColorConvertFloat4ToU32(th.menuText));
    ImGui::PushStyleColor(ImGuiCol_Header,
        ImGui::ColorConvertFloat4ToU32(ThemeColors::popupSelectionBG(th)));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered,
        ImGui::ColorConvertFloat4ToU32(ThemeColors::popupSelectionHoveredBG(th)));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive,
        ImGui::ColorConvertFloat4ToU32(ThemeColors::popupSelectionActiveBG(th)));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, kRadiusDefault);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(kPadXMd, kPadYMd));

    const char* preview = (*currentIndex >= 0 && *currentIndex < itemCount)
        ? items[*currentIndex] : "";
    bool changed = false;
    if (ImGui::BeginCombo(label ? label : "##combo", preview)) {
        for (int i = 0; i < itemCount; ++i) {
            ImGui::PushID(i);
            const bool selected = (i == *currentIndex);
            if (ImGui::Selectable(items[i], selected)) {
                *currentIndex = i;
                changed = true;
            }
            if (selected) ImGui::SetItemDefaultFocus();
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    DrawFocusRing();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(10);
    return changed;
}

bool ComboMulti(const char* label, std::vector<bool>* selections,
                const char* const items[], int itemCount) {
    using namespace aura::ui::metrics;
    const ThemeColorScheme& th = ThemeColors::current();
    if ((int)selections->size() < itemCount) selections->resize((size_t)itemCount, false);

    char preview[64];
    int n = 0;
    for (int i = 0; i < itemCount; ++i) if ((*selections)[i]) ++n;
    if (n == 0)                 std::snprintf(preview, sizeof(preview), "(none)");
    else if (n == itemCount)    std::snprintf(preview, sizeof(preview), "All (%d)", n);
    else                        std::snprintf(preview, sizeof(preview), "%d selected", n);

    ImGui::PushStyleColor(ImGuiCol_FrameBg,
        ImGui::ColorConvertFloat4ToU32(th.frameBgBase));
    ImGui::PushStyleColor(ImGuiCol_Border,
        ImGui::ColorConvertFloat4ToU32(th.chromeBorder));
    ImGui::PushStyleColor(ImGuiCol_PopupBg,
        ImGui::ColorConvertFloat4ToU32(ThemeColors::popupComboBG(th)));
    ImGui::PushStyleColor(ImGuiCol_BorderShadow,
        ImGui::ColorConvertFloat4ToU32(ThemeColors::popupShadow(th)));
    ImGui::PushStyleColor(ImGuiCol_Text,
        ImGui::ColorConvertFloat4ToU32(th.menuText));
    ImGui::PushStyleColor(ImGuiCol_Header,
        ImGui::ColorConvertFloat4ToU32(ThemeColors::popupSelectionBG(th)));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered,
        ImGui::ColorConvertFloat4ToU32(ThemeColors::popupSelectionHoveredBG(th)));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive,
        ImGui::ColorConvertFloat4ToU32(ThemeColors::popupSelectionActiveBG(th)));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, kRadiusDefault);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(kPadXMd, kPadYMd));

    bool changed = false;
    if (ImGui::BeginCombo(label ? label : "##multi", preview)) {
        for (int i = 0; i < itemCount; ++i) {
            ImGui::PushID(i);
            bool b = (*selections)[i];
            if (ImGui::Checkbox(items[i], &b)) {
                (*selections)[i] = b;
                changed = true;
            }
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    DrawFocusRing();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(8);
    return changed;
}

// Tooltip / Popup (Task 12)

void Tooltip(const char* text) {
    using namespace aura::ui::metrics;
    const ThemeColorScheme& th = ThemeColors::current();
    ImGui::PushStyleColor(ImGuiCol_PopupBg,
        ImGui::ColorConvertFloat4ToU32(ThemeColors::popupTooltipBG(th)));
    ImGui::PushStyleColor(ImGuiCol_Border,
        ImGui::ColorConvertFloat4ToU32(ThemeColors::popupBorder(th)));
    ImGui::PushStyleColor(ImGuiCol_BorderShadow,
        ImGui::ColorConvertFloat4ToU32(ThemeColors::popupShadow(th)));
    ImGui::PushStyleColor(ImGuiCol_Text,
        ImGui::ColorConvertFloat4ToU32(th.menuText));
    ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, kRadiusDefault);
    ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 1.0f);
    ImGui::SetTooltip("%s", text);
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(4);
}

namespace {
bool HoveredForHelp() {
    return ::g_showGuidedTooltips && ImGui::IsItemHovered();
}

void ShowSettingsRowTooltip(const SettingsRowOptions& opts) {
    if (HoveredForHelp() && opts.tooltip && opts.tooltip[0]) {
        Tooltip(opts.tooltip);
    }
}

std::string FormatSettingsValue(const char* valueFormat, int value) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), valueFormat && valueFormat[0] ? valueFormat : "%d", value);
    return buf;
}

std::string FormatSettingsValue(const char* valueFormat, float value) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), valueFormat && valueFormat[0] ? valueFormat : "%.1f", value);
    return buf;
}

void DrawSettingsRowLabel(const char* label, bool* enabled, bool* changed) {
    if (enabled) {
        *changed = Checkbox(label, enabled, false) || *changed;
        ImGui::SameLine();
        return;
    }
    if (label && label[0] && label[0] != '#') {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(label);
        NotifySettingsSearchLabeledItem(label, SettingsSearchLabelKind::Text);
        ImGui::SameLine();
    }
}

template <typename T, typename SliderFn, typename ParseFn>
bool DrawSettingsSliderRow(const char* label, bool* enabled, T* value,
                           T min, T max, const char* valueFormat,
                           const SettingsRowOptions& opts,
                           SliderFn slider, ParseFn parse, const char* seedFmt) {
    if (!value) return false;
    if (min > max) std::swap(min, max);
    *value = std::clamp(*value, min, max);
    bool changed = false;
    ImGui::PushID(label ? label : "##settingsSlider");
    ImGui::BeginGroup();
    if (opts.disabled) ImGui::BeginDisabled();
    DrawSettingsRowLabel(label, enabled, &changed);
    const bool sliderDisabled = enabled && !*enabled;
    if (sliderDisabled) ImGui::BeginDisabled();
    ImGui::SetNextItemWidth(ImMax(120.0f, ImGui::GetContentRegionAvail().x * 0.45f));
    T sliderValue = *value;
    if (slider("##slider", &sliderValue, min, max)) {
        *value = std::clamp(sliderValue, min, max);
        changed = true;
    }
    ImGui::SameLine();
    ImGuiStorage* storage = ImGui::GetStateStorage();
    const ImGuiID editingId = ImGui::GetID("##editing");
    const bool editing = storage->GetBool(editingId, false);
    char editBuf[32];
    std::snprintf(editBuf, sizeof(editBuf), seedFmt, *value);
    if (editing) {
        ImGui::SetNextItemWidth(72.0f);
        const bool enter = ImGui::InputText(
            "##value", editBuf, sizeof(editBuf),
            ImGuiInputTextFlags_CharsDecimal | ImGuiInputTextFlags_EnterReturnsTrue);
        if (enter || ImGui::IsItemDeactivatedAfterEdit()) {
            T next = *value;
            if (parse(editBuf, &next)) {
                next = std::clamp(next, min, max);
                if (next != *value) {
                    *value = next;
                    changed = true;
                }
            }
            storage->SetBool(editingId, false);
        }
    } else {
        const std::string caption = FormatSettingsValue(valueFormat, *value);
        if (ImGui::Selectable(caption.c_str(), false, 0, ImVec2(72.0f, 0.0f))) {
            storage->SetBool(editingId, true);
            ImGui::SetKeyboardFocusHere();
        }
    }
    if (sliderDisabled) ImGui::EndDisabled();
    if (opts.disabled) ImGui::EndDisabled();
    ShowSettingsRowTooltip(opts);
    ImGui::EndGroup();
    ShowSettingsRowTooltip(opts);
    ImGui::PopID();
    return changed;
}
} // anonymous namespace

bool BeginSettingsGroup(const char* id) {
    using namespace aura::ui::metrics;
    (void)id;
    const ImGuiStyle& style = ImGui::GetStyle();
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(kGap12, style.WindowPadding.y));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(kGap12, style.ItemSpacing.y));
    ImGui::Dummy(ImVec2(kGap12, 0.0f));
    ImGui::SameLine(0.0f, 0.0f);
    ImGui::BeginGroup();
    ImGui::PushItemWidth(ImMax(80.0f, ImGui::GetContentRegionAvail().x - kGap12));
    return true;
}

void EndSettingsGroup() {
    ImGui::PopItemWidth();
    ImGui::EndGroup();
    ImGui::PopStyleVar(2);
}

bool SettingsToggleRow(const char* label, bool* value, const SettingsRowOptions& opts) {
    if (!value) return false;
    ImGui::BeginGroup();
    if (opts.disabled) ImGui::BeginDisabled();
    const bool changed = Checkbox(label, value, false);
    NotifySettingsSearchLabeledItem(label, SettingsSearchLabelKind::Check);
    if (opts.disabled) ImGui::EndDisabled();
    ShowSettingsRowTooltip(opts);
    ImGui::EndGroup();
    ShowSettingsRowTooltip(opts);
    return changed;
}

bool SettingsSliderRow(const char* label, bool* enabled, int* value,
                       int min, int max, const char* valueFormat,
                       const SettingsRowOptions& opts) {
    return DrawSettingsSliderRow(
        label, enabled, value, min, max, valueFormat, opts,
        [](const char* id, int* current, int lo, int hi) {
            return ImGui::SliderInt(id, current, lo, hi, "");
        },
        [](const char* text, int* out) {
            char* end = nullptr;
            const long parsed = std::strtol(text, &end, 10);
            if (end == text) return false;
            *out = parsed > INT_MAX ? INT_MAX : parsed < INT_MIN ? INT_MIN : static_cast<int>(parsed);
            return true;
        },
        "%d");
}

bool SettingsSliderRow(const char* label, bool* enabled, float* value,
                       float min, float max, const char* valueFormat,
                       const SettingsRowOptions& opts) {
    return DrawSettingsSliderRow(
        label, enabled, value, min, max, valueFormat, opts,
        [](const char* id, float* current, float lo, float hi) {
            return ImGui::SliderFloat(id, current, lo, hi, "");
        },
        [](const char* text, float* out) {
            char* end = nullptr;
            const float parsed = std::strtof(text, &end);
            if (end == text) return false;
            *out = parsed;
            return true;
        },
        "%g");
}

bool SettingsButtonRow(const char* label, const ImVec2& size, const SettingsRowOptions& opts) {
    ImGui::BeginGroup();
    if (opts.disabled) ImGui::BeginDisabled();
    const bool clicked = ActionButton(label ? label : "##settingsButton", size);
    NotifySettingsSearchLabeledItem(label, SettingsSearchLabelKind::Button);
    if (opts.disabled) ImGui::EndDisabled();
    ShowSettingsRowTooltip(opts);
    ImGui::EndGroup();
    ShowSettingsRowTooltip(opts);
    return clicked && !opts.disabled;
}

void TooltipRich(const std::function<void()>& body) {
    using namespace aura::ui::metrics;
    const ThemeColorScheme& th = ThemeColors::current();
    ImGui::PushStyleColor(ImGuiCol_PopupBg,
        ImGui::ColorConvertFloat4ToU32(ThemeColors::popupTooltipBG(th)));
    ImGui::PushStyleColor(ImGuiCol_Border,
        ImGui::ColorConvertFloat4ToU32(ThemeColors::popupBorder(th)));
    ImGui::PushStyleColor(ImGuiCol_BorderShadow,
        ImGui::ColorConvertFloat4ToU32(ThemeColors::popupShadow(th)));
    ImGui::PushStyleColor(ImGuiCol_Text,
        ImGui::ColorConvertFloat4ToU32(th.menuText));
    ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, kRadiusDefault);
    if (ImGui::BeginTooltip()) {
        body();
        ImGui::EndTooltip();
    }
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(4);
}

bool BeginPopup(const char* id) {
    using namespace aura::ui::metrics;
    const ThemeColorScheme& th = ThemeColors::current();
    ImGui::PushStyleColor(ImGuiCol_PopupBg,
        ImGui::ColorConvertFloat4ToU32(ThemeColors::popupMenuBG(th)));
    ImGui::PushStyleColor(ImGuiCol_Border,
        ImGui::ColorConvertFloat4ToU32(ThemeColors::popupBorder(th)));
    ImGui::PushStyleColor(ImGuiCol_BorderShadow,
        ImGui::ColorConvertFloat4ToU32(ThemeColors::popupShadow(th)));
    ImGui::PushStyleColor(ImGuiCol_Text,
        ImGui::ColorConvertFloat4ToU32(th.menuText));
    ImGui::PushStyleColor(ImGuiCol_Header,
        ImGui::ColorConvertFloat4ToU32(ThemeColors::popupSelectionBG(th)));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered,
        ImGui::ColorConvertFloat4ToU32(ThemeColors::popupSelectionHoveredBG(th)));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive,
        ImGui::ColorConvertFloat4ToU32(ThemeColors::popupSelectionActiveBG(th)));
    ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, kRadiusDefault);
    ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 1.0f);
    const bool open = ImGui::BeginPopup(id);
    if (!open) {
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(7);
    }
    return open;
}

void EndPopup() {
    ImGui::EndPopup();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(7);
}

// Standalone window helpers.

namespace {

bool DrawStandaloneCaptionButton(const char* id,
                                 StandaloneWindowAction action,
                                 const ImVec2& position,
                                 float extent,
                                 const ThemeColorScheme& th,
                                 bool maximized) {
    using namespace aura::ui::metrics;

    ImGui::SetCursorScreenPos(position);
    const bool pressed = ImGui::InvisibleButton(id, ImVec2(extent, extent));
    const bool hovered = ImGui::IsItemHovered();
    const bool active = ImGui::IsItemActive();
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const ImVec2 itemMin = ImGui::GetItemRectMin();
    const ImVec2 itemMax = ImGui::GetItemRectMax();

    if (hovered || active) {
        const ImVec4& background = active ? th.frameBgBase : th.filterBrowserButtonBG;
        drawList->AddRectFilled(itemMin, itemMax,
            ImGui::ColorConvertFloat4ToU32(background));
    }

    const ImVec4& iconColor = hovered ? th.interfaceText : th.sidebarIconInactive;
    const ImU32 packedIcon = ImGui::ColorConvertFloat4ToU32(iconColor);
    const float centerX = (itemMin.x + itemMax.x) * 0.5f;
    const float centerY = (itemMin.y + itemMax.y) * 0.5f;
    const float halfIcon = kIconXs * 0.5f;

    switch (action) {
        case StandaloneWindowAction::Minimize:
            drawList->AddLine(
                ImVec2(centerX - halfIcon, centerY + kRadiusTight),
                ImVec2(centerX + halfIcon, centerY + kRadiusTight),
                packedIcon, kFocusRingWidth);
            break;
        case StandaloneWindowAction::ToggleMaximize:
            if (maximized) {
                drawList->AddRect(
                    ImVec2(centerX - halfIcon + kRadiusTight, centerY - halfIcon),
                    ImVec2(centerX + halfIcon + kRadiusTight, centerY + halfIcon),
                    packedIcon, kRadiusTight, ImDrawFlags_None, kFocusRingWidth);
                drawList->AddRect(
                    ImVec2(centerX - halfIcon - kRadiusTight, centerY - halfIcon + kRadiusTight),
                    ImVec2(centerX + halfIcon - kRadiusTight, centerY + halfIcon + kRadiusTight),
                    packedIcon, kRadiusTight, ImDrawFlags_None, kFocusRingWidth);
            } else {
                drawList->AddRect(
                    ImVec2(centerX - halfIcon, centerY - halfIcon),
                    ImVec2(centerX + halfIcon, centerY + halfIcon),
                    packedIcon, kRadiusTight, ImDrawFlags_None, kFocusRingWidth);
            }
            break;
        case StandaloneWindowAction::Close:
            drawList->AddLine(
                ImVec2(centerX - halfIcon, centerY - halfIcon),
                ImVec2(centerX + halfIcon, centerY + halfIcon),
                packedIcon, kFocusRingWidth);
            drawList->AddLine(
                ImVec2(centerX + halfIcon, centerY - halfIcon),
                ImVec2(centerX - halfIcon, centerY + halfIcon),
                packedIcon, kFocusRingWidth);
            break;
        case StandaloneWindowAction::None:
            break;
    }

    DrawFocusRing();
    return pressed;
}

} // anonymous namespace

void OpenStandaloneWindow(StandaloneWindowState* state) {
    if (!state) return;
    state->open = true;
    state->minimized = false;
    state->requestFocus = true;
}

bool BeginStandaloneWindow(const char* title, const char* stableId,
                           StandaloneWindowState* state,
                           const StandaloneWindowOptions& options) {
    using namespace aura::ui::metrics;
    if (!state || !stableId || stableId[0] == '\0' || !state->open || state->minimized) {
        return false;
    }

    const char* id = stableId;
    while (*id == '#') ++id;
    if (*id == '\0') return false;
    const std::string normalizedId(id);

    const ThemeColorScheme& th = ThemeColors::current();
    ImGui::SetNextWindowSize(options.defaultSize, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints(options.minimumSize, ImVec2(FLT_MAX, FLT_MAX));
    if (state->requestFocus) {
        ImGui::SetNextWindowFocus();
    }
    if (state->maximized) {
        state->requestMove = false;
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->WorkPos, ImGuiCond_Always);
        ImGui::SetNextWindowSize(viewport->WorkSize, ImGuiCond_Always);
    } else if (state->requestMove) {
        ImGui::SetNextWindowPos(state->restorePos, ImGuiCond_Always);
        state->requestMove = false;
    }

    std::string windowLabel = title ? title : "";
    windowLabel += "###";
    windowLabel += normalizedId;
    if (state->maximized) windowLabel += "__Maximized";

    ImGui::PushStyleColor(ImGuiCol_WindowBg,
        ImGui::ColorConvertFloat4ToU32(th.mainWindowBG));
    ImGui::PushStyleColor(ImGuiCol_Border,
        ImGui::ColorConvertFloat4ToU32(th.chromeBorder));
    ImGui::PushStyleColor(ImGuiCol_Text,
        ImGui::ColorConvertFloat4ToU32(th.interfaceText));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, kRadiusTight);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(kGap16, kGap16));

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar |
                             ImGuiWindowFlags_NoCollapse |
                             ImGuiWindowFlags_NoMove;
    if (state->maximized) flags |= ImGuiWindowFlags_NoResize;
    const bool visible = ImGui::Begin(windowLabel.c_str(), &state->open, flags);
    state->requestFocus = false;
    if (!visible || !state->open) {
        if (!state->open) {
            state->minimized = false;
            state->maximized = false;
        }
        EndStandaloneWindow();
        return false;
    }

    const ImVec2 windowPos = ImGui::GetWindowPos();
    const ImVec2 windowSize = ImGui::GetWindowSize();
    const float titleHeight = kHeightLg;
    const float captionExtent = titleHeight;
    const float actionStripWidth = captionExtent * 3.0f;
    float dragWidth = windowSize.x - actionStripWidth;
    if (dragWidth < kGap24) dragWidth = kGap24;

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    drawList->AddRectFilled(
        windowPos,
        ImVec2(windowPos.x + windowSize.x, windowPos.y + titleHeight),
        ImGui::ColorConvertFloat4ToU32(th.menuBarBG));
    drawList->AddLine(
        ImVec2(windowPos.x, windowPos.y + titleHeight),
        ImVec2(windowPos.x + windowSize.x, windowPos.y + titleHeight),
        ImGui::ColorConvertFloat4ToU32(th.chromeBorder));

    ImGui::PushID(normalizedId.c_str());
    ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
    ImGui::SetCursorScreenPos(windowPos);
    ImGui::InvisibleButton("##TitleDrag", ImVec2(dragWidth, titleHeight));
    const bool draggingTitle = ImGui::IsItemActive() &&
        ImGui::IsMouseDragging(ImGuiMouseButton_Left);
    ImGui::PopItemFlag();

    if (title && title[0] != '\0') {
        const ImVec4 clipRect(
            windowPos.x + kGap16,
            windowPos.y,
            windowPos.x + dragWidth - kGap8,
            windowPos.y + titleHeight);
        drawList->AddText(
            ImGui::GetFont(), ImGui::GetFontSize(),
            ImVec2(windowPos.x + kGap16,
                   windowPos.y + (titleHeight - ImGui::GetFontSize()) * 0.5f),
            ImGui::ColorConvertFloat4ToU32(th.menuText),
            title, nullptr, 0.0f, &clipRect);
    }

    StandaloneWindowAction action = StandaloneWindowAction::None;
    ImVec2 captionPos(windowPos.x + windowSize.x - actionStripWidth, windowPos.y);
    if (DrawStandaloneCaptionButton("##Minimize", StandaloneWindowAction::Minimize,
                                    captionPos, captionExtent, th, state->maximized)) {
        action = StandaloneWindowAction::Minimize;
    }
    captionPos.x += captionExtent;
    if (DrawStandaloneCaptionButton("##ToggleMaximize", StandaloneWindowAction::ToggleMaximize,
                                    captionPos, captionExtent, th, state->maximized)) {
        action = StandaloneWindowAction::ToggleMaximize;
    }
    captionPos.x += captionExtent;
    if (DrawStandaloneCaptionButton("##Close", StandaloneWindowAction::Close,
                                    captionPos, captionExtent, th, state->maximized)) {
        action = StandaloneWindowAction::Close;
    }
    ImGui::PopID();

    if (draggingTitle && !state->maximized) {
        const ImVec2 delta = ImGui::GetIO().MouseDelta;
        state->restorePos = ImVec2(windowPos.x + delta.x, windowPos.y + delta.y);
        state->requestMove = true;
    }

    switch (action) {
        case StandaloneWindowAction::Minimize:
            state->minimized = true;
            EndStandaloneWindow();
            return false;
        case StandaloneWindowAction::ToggleMaximize:
            if (state->maximized) {
                state->maximized = false;
            } else {
                state->restorePos = windowPos;
                state->restoreSize = windowSize;
                state->maximized = true;
            }
            state->requestMove = false;
            state->requestFocus = true;
            break;
        case StandaloneWindowAction::Close:
            state->open = false;
            state->minimized = false;
            state->maximized = false;
            state->requestMove = false;
            EndStandaloneWindow();
            return false;
        case StandaloneWindowAction::None:
            break;
    }

    ImGui::SetCursorScreenPos(
        ImVec2(windowPos.x + kGap16, windowPos.y + titleHeight + kGap16));
    return true;
}

void EndStandaloneWindow() {
    ImGui::End();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(3);
}

// Modal dialog helpers.

bool BeginDialog(const char* title, bool* open, float width, float height) {
    using namespace aura::ui::metrics;
    const ThemeColorScheme& th = ThemeColors::current();

    // One-shot OpenPopup when caller flipped *open to true.
    if (open && *open) {
        ImGui::OpenPopup(title);
        // Consume the flag so the next frame doesn't re-open after dismiss.
        *open = false;
    }

    ImGui::SetNextWindowSize(ImVec2(width, height), ImGuiCond_FirstUseEver);
    ImGui::PushStyleColor(ImGuiCol_WindowBg,
        ImGui::ColorConvertFloat4ToU32(ThemeColors::popupModalBG(th)));
    ImGui::PushStyleColor(ImGuiCol_PopupBg,
        ImGui::ColorConvertFloat4ToU32(ThemeColors::popupModalBG(th)));
    ImGui::PushStyleColor(ImGuiCol_Border,
        ImGui::ColorConvertFloat4ToU32(ThemeColors::popupBorder(th)));
    ImGui::PushStyleColor(ImGuiCol_BorderShadow,
        ImGui::ColorConvertFloat4ToU32(ThemeColors::popupShadow(th)));
    ImGui::PushStyleColor(ImGuiCol_Text,
        ImGui::ColorConvertFloat4ToU32(th.menuText));
    ImGui::PushStyleColor(ImGuiCol_TitleBg,
        ImGui::ColorConvertFloat4ToU32(ThemeColors::popupModalPanelBG(th)));
    ImGui::PushStyleColor(ImGuiCol_TitleBgActive,
        ImGui::ColorConvertFloat4ToU32(ThemeColors::popupModalPanelBG(th)));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, kRadiusPanel);
    ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, kRadiusPanel);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(kGap24, kGap24));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);

    // Using nullptr for the ImBool lets ESC close without user tracking flag.
    const bool visible = ImGui::BeginPopupModal(title, nullptr,
        ImGuiWindowFlags_NoSavedSettings);
    if (!visible) {
        ImGui::PopStyleVar(4);
        ImGui::PopStyleColor(7);
    }
    return visible;
}

void EndDialog() {
    ImGui::EndPopup();
    ImGui::PopStyleVar(4);
    ImGui::PopStyleColor(7);
}

// Panel / SectionHeader / Divider (Task 14)

bool BeginPanel(const char* id, PanelVariant variant, float width, float height, ImGuiWindowFlags wflags) {
    using namespace aura::ui::metrics;
    const ThemeColorScheme& th = ThemeColors::current();

    ImU32 bg = 0, border = 0;
    switch (variant) {
        case PanelVariant::Peer:
            bg     = ImGui::ColorConvertFloat4ToU32(ThemeColors::peerPanelBG(th));
            border = ImGui::ColorConvertFloat4ToU32(ThemeColors::peerPanelOutline(th));
            break;
        case PanelVariant::Surface:
            bg     = ImGui::ColorConvertFloat4ToU32(th.resultsBG);
            border = ImGui::ColorConvertFloat4ToU32(th.chromeBorder);
            break;
        case PanelVariant::Inset:
            bg     = ImGui::ColorConvertFloat4ToU32(th.dialogPanelBG);
            border = ImGui::ColorConvertFloat4ToU32(th.chromeBorder);
            break;
    }

    ImGui::PushStyleColor(ImGuiCol_ChildBg, bg);
    ImGui::PushStyleColor(ImGuiCol_Border,  border);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, kRadiusPanel);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(kGap16, kGap16));

    const ImGuiChildFlags cflags = ImGuiChildFlags_Borders;
    const bool visible = ImGui::BeginChild(id, ImVec2(width, height), cflags, wflags);
    if (!visible) {
        ImGui::EndChild();
        ImGui::PopStyleVar(3);
        ImGui::PopStyleColor(2);
    }
    return visible;
}

void EndPanel() {
    ImGui::EndChild();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(2);
}

void Divider(DividerVariant v) {
    using namespace aura::ui::metrics;
    const ThemeColorScheme& th = ThemeColors::current();
    const float alpha = (v == DividerVariant::Strong) ? 0.30f : 0.15f;
    ImVec4 c = th.chromeBorder; c.w *= alpha;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float  w = ImGui::GetContentRegionAvail().x;
    const float  inset = (v == DividerVariant::Inset) ? kGap16 : 0.0f;
    ImGui::GetWindowDrawList()->AddLine(
        ImVec2(p.x + inset, p.y),
        ImVec2(p.x + w - inset, p.y),
        ImGui::ColorConvertFloat4ToU32(c), 1.0f);
    ImGui::Dummy(ImVec2(0, kGap4));
}

bool SectionHeader(const char* title, bool* expanded,
                   const std::function<void()>& trailing) {
    using namespace aura::ui::metrics;
    const ThemeColorScheme& th = ThemeColors::current();
    bool content = true;

    ImGui::PushStyleColor(ImGuiCol_Text,
        ImGui::ColorConvertFloat4ToU32(th.menuText));

    if (expanded) {
        // Lucide chevrons not yet merged (Task 4 deferred); use ASCII fallback
        // so Section is usable now. Post-Task-4, swap to ICON_LC_CHEVRON_*.
        const char* arrow = *expanded ? "v " : "> ";
        char buf[256];
        std::snprintf(buf, sizeof(buf), "%s  %s", arrow, title ? title : "");
        const ImVec2 sz(ImGui::CalcTextSize(buf).x + kPadXMd * 2, kHeightMd);
        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton(buf, sz);
        if (ImGui::IsItemClicked()) *expanded = !*expanded;
        const ImVec2 textPos(p0.x, p0.y + (kHeightMd - ImGui::GetFontSize()) * 0.5f);
        ImGui::GetWindowDrawList()->AddText(textPos, ImGui::GetColorU32(ImGuiCol_Text), buf);
        const float prefixW = ImGui::CalcTextSize(arrow).x + ImGui::CalcTextSize("  ").x;
        const ImVec2 titleOrigin(textPos.x + prefixW, textPos.y);
        NotifySettingsSearchLabeledItem(title, SettingsSearchLabelKind::Text, &titleOrigin);
        content = *expanded;
    } else {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(title ? title : "");
        NotifySettingsSearchLabeledItem(title, SettingsSearchLabelKind::Text);
    }

    if (trailing) {
        ImGui::SameLine();
        const float avail = ImGui::GetContentRegionAvail().x;
        // Rough right-align - exact trailing width varies; caller handles.
        if (avail > 0) ImGui::SameLine(avail);
        trailing();
    }
    ImGui::PopStyleColor();

    Divider(DividerVariant::Subtle);
    return content;
}

ImVec4 LerpColor(const ImVec4& a, const ImVec4& b, float t) {
    return ImVec4(
        a.x + (b.x - a.x) * t,
        a.y + (b.y - a.y) * t,
        a.z + (b.z - a.z) * t,
        a.w + (b.w - a.w) * t);
}

// Workflow feedback controls.

void ScanJobCard(const char* id, const ScanJobCardOptions& options) {
    using namespace aura::ui::metrics;
    const ThemeColorScheme& th = ThemeColors::current();

    const char* statusLabel = "Queued";
    ImVec4 statusColor = th.resultsSecondaryText;
    switch (options.status) {
        case ScanJobStatus::Running:
            statusLabel = "Running";
            statusColor = th.controlAccent;
            break;
        case ScanJobStatus::Paused:
            statusLabel = "Paused";
            statusColor = th.meterWarning;
            break;
        case ScanJobStatus::Complete:
            statusLabel = "Complete";
            statusColor = th.volumeColor;
            break;
        case ScanJobStatus::Failed:
            statusLabel = "Failed";
            statusColor = th.volumeClipColor;
            break;
        case ScanJobStatus::Queued:
        default:
            break;
    }

    if (!BeginPanel(id, PanelVariant::Surface, 0.0f, 104.0f)) return;

    ImGui::PushStyleColor(ImGuiCol_Text,
        ImGui::ColorConvertFloat4ToU32(th.resultsText));
    ImGui::TextUnformatted(options.title ? options.title : "Scan job");
    ImGui::PopStyleColor();

    const float statusWidth = ImGui::CalcTextSize(statusLabel).x;
    ImGui::SameLine(ImGui::GetContentRegionMax().x - statusWidth);
    ImGui::PushStyleColor(ImGuiCol_Text,
        ImGui::ColorConvertFloat4ToU32(statusColor));
    ImGui::TextUnformatted(statusLabel);
    ImGui::PopStyleColor();

    if (options.detail && options.detail[0] != '\0') {
        ImGui::PushStyleColor(ImGuiCol_Text,
            ImGui::ColorConvertFloat4ToU32(th.resultsSecondaryText));
        ImGui::TextUnformatted(options.detail);
        ImGui::PopStyleColor();
    }

    const float progress = std::clamp(options.progress, 0.0f, 1.0f);
    char overlay[96];
    if (options.total > 0) {
        std::snprintf(overlay, sizeof(overlay), "%d / %d", options.current, options.total);
    } else {
        std::snprintf(overlay, sizeof(overlay), "Waiting");
    }
    ImGui::PushStyleColor(ImGuiCol_FrameBg,
        ImGui::ColorConvertFloat4ToU32(th.frameBgBase));
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram,
        ImGui::ColorConvertFloat4ToU32(statusColor));
    ImGui::ProgressBar(progress, ImVec2(-1.0f, kHeightSm), overlay);
    ImGui::PopStyleColor(2);

    if (options.itemsPerSecond > 0.0f) {
        char rate[64];
        std::snprintf(rate, sizeof(rate), "%.1f items/s", options.itemsPerSecond);
        ImGui::PushStyleColor(ImGuiCol_Text,
            ImGui::ColorConvertFloat4ToU32(th.resultsSecondaryText));
        ImGui::TextUnformatted(rate);
        ImGui::PopStyleColor();
    }

    EndPanel();
}

void RecoveryBanner(const char* id, const RecoveryBannerOptions& options,
                    const std::function<void()>& actions) {
    using namespace aura::ui::metrics;
    const ThemeColorScheme& th = ThemeColors::current();

    ImVec4 intentColor = th.diagnosticOverlayAccent;
    switch (options.intent) {
        case RecoveryBannerIntent::Warning:
            intentColor = th.meterWarning;
            break;
        case RecoveryBannerIntent::Danger:
            intentColor = th.volumeClipColor;
            break;
        case RecoveryBannerIntent::Info:
        default:
            break;
    }
    const ImVec4 background = LerpColor(th.resultsBG, intentColor, 0.10f);
    const ImVec4 border = LerpColor(th.chromeBorder, intentColor, 0.38f);

    ImGui::PushStyleColor(ImGuiCol_ChildBg,
        ImGui::ColorConvertFloat4ToU32(background));
    ImGui::PushStyleColor(ImGuiCol_Border,
        ImGui::ColorConvertFloat4ToU32(border));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, kRadiusDefault);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(kGap12, kGap8));

    const bool visible = ImGui::BeginChild(
        id ? id : "##recoveryBanner",
        ImVec2(0.0f, options.height),
        ImGuiChildFlags_Borders,
        ImGuiWindowFlags_NoScrollbar);
    if (visible) {
        const ImVec2 topLeft = ImGui::GetWindowPos();
        ImGui::GetWindowDrawList()->AddRectFilled(
            topLeft,
            ImVec2(topLeft.x + 3.0f, topLeft.y + ImGui::GetWindowHeight()),
            ImGui::ColorConvertFloat4ToU32(intentColor),
            kRadiusDefault,
            ImDrawFlags_RoundCornersLeft);

        const float textWidth = actions
            ? std::max(80.0f, ImGui::GetContentRegionAvail().x - options.actionWidth - kGap16)
            : ImGui::GetContentRegionAvail().x;
        ImGui::BeginGroup();
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + textWidth);
        ImGui::PushStyleColor(ImGuiCol_Text,
            ImGui::ColorConvertFloat4ToU32(intentColor));
        ImGui::TextUnformatted(options.title ? options.title : "Recovery");
        ImGui::PopStyleColor();
        if (options.message && options.message[0] != '\0') {
            ImGui::PushStyleColor(ImGuiCol_Text,
                ImGui::ColorConvertFloat4ToU32(th.resultsText));
            ImGui::TextUnformatted(options.message);
            ImGui::PopStyleColor();
        }
        if (options.loading) {
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Text,
                ImGui::ColorConvertFloat4ToU32(th.resultsSecondaryText));
            ImGui::TextUnformatted("...");
            ImGui::PopStyleColor();
        }
        ImGui::PopTextWrapPos();
        ImGui::EndGroup();

        if (actions) {
            ImGui::SameLine();
            const float actionX = ImGui::GetWindowContentRegionMax().x - options.actionWidth;
            if (ImGui::GetCursorPosX() < actionX) ImGui::SetCursorPosX(actionX);
            actions();
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(2);
}

}
