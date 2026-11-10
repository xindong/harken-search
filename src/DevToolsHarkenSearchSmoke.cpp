// src/DevToolsHarkenSearchSmoke.cpp
// Aris UI Component Gallery v2.1. Dev-only visual contract built as a
// delta over the existing Aris UI v1 baseline, HarkenSearchWidgets, HarkenSearchMetrics, and
// ThemeColorScheme baseline.
#ifdef AURA_BUILD_DIAGNOSTICS

#include "HarkenSearchWidgets.h"
#include "HarkenSearchIcons.h"
#include "HarkenSearchMetrics.h"
#include "Theme.h"
#include "platform/system.h"
#include "../thirdparty/lucide/IconsLucide.h"
#include "imgui.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace aura::ui::dev {

namespace {

struct TokenSample {
    const char* name;
    ImVec4 color;
};

struct ProtoRow {
    const char* name;
    const char* tag;
    const char* length;
    const char* rate;
    bool selected;
};

struct GalleryState {
    int section = 0;
};

GalleryState& State() {
    static GalleryState s = [] {
        GalleryState initial;
#ifdef _WIN32
        char* section = nullptr;
        size_t sectionLen = 0;
        std::string sectionValue;
        if (_dupenv_s(&section, &sectionLen, "AURO_UI_GALLERY_SECTION") == 0 && section) {
            sectionValue = section;
        }
        std::free(section);
#else
        const auto section = Platform::ReadEnvironmentVariable("AURO_UI_GALLERY_SECTION");
        const std::string sectionValue =
            section.status == Platform::EnvironmentStringStatus::Success
                ? section.valueUtf8 : std::string{};
#endif
        if (!sectionValue.empty()) {
            const int parsed = std::atoi(sectionValue.c_str());
            if (parsed >= 0 && parsed <= 8) {
                initial.section = parsed;
            }
        }
        return initial;
    }();
    return s;
}

ImVec4 WithAlpha(ImVec4 c, float alpha) {
    c.w = alpha;
    return c;
}

ImVec4 Mix(ImVec4 a, ImVec4 b, float t) {
    return ImVec4(
        a.x + (b.x - a.x) * t,
        a.y + (b.y - a.y) * t,
        a.z + (b.z - a.z) * t,
        a.w + (b.w - a.w) * t);
}

ImU32 U32(ImVec4 c) {
    return ImGui::ColorConvertFloat4ToU32(c);
}

const ProtoRow kProtoRows[] = {
    {"CA CONCERT HALL 02 Medium Audience Atmo 01 FRONT", "AMB", "00:00:14.48", "48000", false},
    {"CA CONCERT HALL 02 Medium Audience Atmo 01 REAR", "AMB", "00:00:14.48", "48000", false},
    {"CA CONCERT HALL 02 Medium Audience Applause 25 RF", "SFX", "00:00:24.78", "48000", false},
    {"CA CONCERT HALL 02 Medium Audience Applause 26 RF", "SFX", "00:01:25.19", "48000", true},
    {"CA CONCERT HALL 03 Medium Audience Applause 01 FF", "SFX", "00:00:12.28", "48000", false},
    {"CA CONCERT HALL 03 Medium Audience Applause 02 RE", "SFX", "00:00:03.84", "48000", false},
};

const char* const kInfoTabs[] = {
    "Metadata Summary",
    "Artwork",
    "Transfer History",
    "Play History"
};

void DrawPrototypeWaveBars(ImVec2 origin, float width, float centerY, int bars, ImU32 color) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    for (int i = 0; i < bars; ++i) {
        const float x = origin.x + width * (static_cast<float>(i) / static_cast<float>(bars - 1));
        const float amp = std::sin(static_cast<float>(i) * 0.41f) * 0.45f +
                          std::sin(static_cast<float>(i) * 0.13f) * 0.25f;
        const float span = 6.0f + (amp + 0.8f) * 9.0f;
        dl->AddLine(ImVec2(x, centerY - span), ImVec2(x, centerY + span), color, 1.0f);
    }
}

void HexColor(ImVec4 c, char* out, size_t outSize) {
    const int r = static_cast<int>(c.x * 255.0f + 0.5f);
    const int g = static_cast<int>(c.y * 255.0f + 0.5f);
    const int b = static_cast<int>(c.z * 255.0f + 0.5f);
    const int a = static_cast<int>(c.w * 255.0f + 0.5f);
    std::snprintf(out, outSize, "#%02X%02X%02X / %02X", r, g, b, a);
}

void TextMuted(const char* text) {
    const auto& th = ThemeColors::current();
    ImGui::PushStyleColor(ImGuiCol_Text, th.resultsSecondaryText);
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
}

void SectionTitle(const char* title, const char* note = nullptr) {
    const auto& th = ThemeColors::current();
    ImGui::PushStyleColor(ImGuiCol_Text, th.resultsText);
    ImGui::TextUnformatted(title);
    ImGui::PopStyleColor();
    if (note && note[0]) {
        ImGui::SameLine();
        TextMuted(note);
    }
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddLine(
        ImVec2(p.x, p.y + 2.0f),
        ImVec2(p.x + ImGui::GetContentRegionAvail().x, p.y + 2.0f),
        U32(WithAlpha(th.chromeBorder, 0.65f)), 1.0f);
    ImGui::Dummy(ImVec2(1.0f, 10.0f));
}

void DrawSwatch(const TokenSample& token, ImVec2 size = ImVec2(148, 48)) {
    const auto& th = ThemeColors::current();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton(token.name, size);
    dl->AddRectFilled(p, ImVec2(p.x + 48.0f, p.y + size.y), U32(token.color), 2.0f);
    dl->AddRect(p, ImVec2(p.x + 48.0f, p.y + size.y), U32(WithAlpha(th.chromeBorder, 0.85f)), 2.0f);

    char hex[32];
    HexColor(token.color, hex, sizeof(hex));
    dl->AddText(ImVec2(p.x + 58.0f, p.y + 6.0f), U32(th.resultsText), token.name);
    dl->AddText(ImVec2(p.x + 58.0f, p.y + 25.0f), U32(th.resultsSecondaryText), hex);
}

void DrawSurfaceStack() {
    const auto& th = ThemeColors::current();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImVec2 size(330.0f, 104.0f);
    ImGui::InvisibleButton("surface-stack", size);

    dl->AddRectFilled(p, ImVec2(p.x + size.x, p.y + size.y), U32(th.mainWindowBG), 2.0f);
    dl->AddRect(p, ImVec2(p.x + size.x, p.y + size.y), U32(WithAlpha(th.chromeBorder, 0.8f)), 2.0f);
    dl->AddText(ImVec2(p.x + 10.0f, p.y + 8.0f), U32(th.resultsText), "surface.base");

    ImVec2 raised0(p.x + 72.0f, p.y + 28.0f);
    ImVec2 raised1(p.x + 286.0f, p.y + 88.0f);
    dl->AddRectFilled(raised0, raised1, U32(th.dialogPanelBG), 2.0f);
    dl->AddRect(raised0, raised1, U32(WithAlpha(th.chromeBorder, 0.65f)), 2.0f);
    dl->AddText(ImVec2(raised0.x + 10.0f, raised0.y + 8.0f), U32(th.resultsText), "surface.raised");

    ImVec2 inset0(raised0.x + 82.0f, raised0.y + 26.0f);
    ImVec2 inset1(raised1.x - 12.0f, raised1.y - 10.0f);
    dl->AddRectFilled(inset0, inset1, U32(th.frameBgBase), 2.0f);
    dl->AddRect(inset0, inset1, U32(WithAlpha(th.chromeBorder, 0.9f)), 2.0f);
    dl->AddText(ImVec2(inset0.x + 8.0f, inset0.y + 5.0f), U32(th.resultsSecondaryText), "surface.inset");
}

void DrawBaselineLinks() {
    const auto& th = ThemeColors::current();
    SectionTitle("Current Baseline Links", "v2.1 extends these; it does not replace them");

    ImGui::PushStyleColor(ImGuiCol_Text, th.resultsText);
    ImGui::TextUnformatted("Spec    docs/superpowers/specs/2026-04-21-auro-ui-component-spec-v1-design.md");
    ImGui::TextUnformatted("Component system: docs/product/cards/AUR-P1-029-component-gallery-v2.1.md");
    ImGui::TextUnformatted("API     src/HarkenSearchWidgets.h");
    ImGui::TextUnformatted("Metrics src/HarkenSearchMetrics.h");
    ImGui::TextUnformatted("Theme   src/Theme.h");
    ImGui::PopStyleColor();

    ImGui::Spacing();
    TextMuted("Inherited rules: feature UI uses aura::ui wrappers; colors use ThemeColorScheme; sizes use HarkenSearchMetrics; five-theme QA remains required.");
    TextMuted("v2.1 delta: stronger gallery authority, state matrices, planned dense-row/badge/meter samples, and waveform visual references.");
}

void DrawTokenGallery() {
    const auto& th = ThemeColors::current();
    SectionTitle("Color Tokens", "semantic mapping from active ThemeColorScheme");
    const TokenSample tokens[] = {
        {"surface.base", th.mainWindowBG},
        {"surface.raised", th.dialogPanelBG},
        {"surface.inset", th.frameBgBase},
        {"surface.overlay", th.popupBG},
        {"border.subtle", WithAlpha(th.chromeBorder, 0.65f)},
        {"border.strong", th.chromeBorder},
        {"text.primary", th.resultsText},
        {"text.secondary", th.resultsSecondaryText},
        {"accent.primary", th.controlAccent},
        {"accent.transfer", th.wwiseDiffReplaced},
        {"status.success", th.wwiseConnectOk},
        {"status.error", th.wwiseConnectFail},
        {"waveform.primary", th.waveformColor},
        {"meter.warn", th.meterWarning},
        {"meter.clip", th.meterClip},
    };

    const int columns = 3;
    for (int i = 0; i < static_cast<int>(sizeof(tokens) / sizeof(tokens[0])); ++i) {
        ImGui::PushID(i);
        DrawSwatch(tokens[i]);
        ImGui::PopID();
        if ((i % columns) != columns - 1) {
            ImGui::SameLine(0.0f, 10.0f);
        }
    }

    ImGui::Spacing();
    SectionTitle("Surface Contrast Slice", "nested surface base / raised / inset");
    DrawSurfaceStack();
}

void DrawDensityChip(const char* label, float rowH, float icon, float font, float lineH) {
    const auto& th = ThemeColors::current();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImVec2 s(220.0f, rowH + 30.0f);
    ImGui::InvisibleButton(label, s);
    dl->AddRectFilled(p, ImVec2(p.x + s.x, p.y + s.y), U32(th.dialogPanelBG), 2.0f);
    dl->AddRect(p, ImVec2(p.x + s.x, p.y + s.y), U32(WithAlpha(th.chromeBorder, 0.8f)), 2.0f);
    dl->AddText(ImVec2(p.x + 8.0f, p.y + 7.0f), U32(th.resultsText), label);

    char buf[96];
    std::snprintf(buf, sizeof(buf), "row %.0f  icon %.0f  font %.0f  line %.0f", rowH, icon, font, lineH);
    dl->AddText(ImVec2(p.x + 8.0f, p.y + 25.0f), U32(th.resultsSecondaryText), buf);
    dl->AddRectFilled(
        ImVec2(p.x + 8.0f, p.y + s.y - rowH - 6.0f),
        ImVec2(p.x + s.x - 8.0f, p.y + s.y - 6.0f),
        U32(th.frameBgBase), 1.0f);
}

void DrawTypographyDensity() {
    SectionTitle("Typography + Density", "11/12/14 px target rhythm with mono metadata");
    DrawDensityChip("Compact", 20.0f, 12.0f, 11.0f, 14.0f);
    ImGui::SameLine(0.0f, 10.0f);
    DrawDensityChip("Default", 28.0f, 14.0f, 13.0f, 16.0f);
    ImGui::SameLine(0.0f, 10.0f);
    DrawDensityChip("Large", 36.0f, 16.0f, 14.0f, 20.0f);

    ImGui::Spacing();
    const auto& th = ThemeColors::current();
    ImGui::PushStyleColor(ImGuiCol_Text, th.resultsText);
    ImGui::TextUnformatted("Label      ABCabc 123  WAV 96k  5.1");
    ImGui::TextUnformatted("Body       Search result metadata remains scan-friendly.");
    ImGui::PopStyleColor();
    ImGui::PushStyleColor(ImGuiCol_Text, th.resultsSecondaryText);
    ImGui::TextUnformatted("Mono/TC    01:23:45:12   192000 Hz   004.82 MB   -12.5 dBFS");
    ImGui::PopStyleColor();
}

enum class SpecState { Default, Hover, Active, Disabled, Focused };
enum class SpecButtonVariant { Primary, Secondary, Ghost, Danger };

void DrawSpecButton(const char* label, SpecButtonVariant variant, SpecState state, ImVec2 size) {
    const auto& th = ThemeColors::current();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton(label, size);

    ImVec4 bg = th.dialogPanelBG;
    ImVec4 fg = th.resultsText;
    ImVec4 border = WithAlpha(th.chromeBorder, 0.75f);
    if (variant == SpecButtonVariant::Primary) {
        bg = th.controlAccent;
        fg = ImVec4(1, 1, 1, 1);
        border = th.controlAccent;
    } else if (variant == SpecButtonVariant::Ghost) {
        bg = WithAlpha(th.dialogPanelBG, 0.0f);
        border = WithAlpha(th.chromeBorder, 0.0f);
    } else if (variant == SpecButtonVariant::Danger) {
        bg = WithAlpha(th.wwiseConnectFail, 0.18f);
        fg = th.wwiseConnectFail;
        border = WithAlpha(th.wwiseConnectFail, 0.45f);
    }

    if (state == SpecState::Hover) {
        bg = Mix(bg, ImVec4(1, 1, 1, bg.w), 0.12f);
        border = Mix(border, ImVec4(1, 1, 1, border.w), 0.18f);
    } else if (state == SpecState::Active) {
        bg = Mix(bg, ImVec4(0, 0, 0, bg.w), 0.22f);
        fg = WithAlpha(fg, 0.82f);
    } else if (state == SpecState::Disabled) {
        bg = WithAlpha(bg, 0.25f);
        fg = WithAlpha(th.resultsSecondaryText, 0.35f);
        border = WithAlpha(border, 0.18f);
    }

    dl->AddRectFilled(p, ImVec2(p.x + size.x, p.y + size.y), U32(bg), 2.0f);
    dl->AddRect(p, ImVec2(p.x + size.x, p.y + size.y), U32(border), 2.0f);
    if (state == SpecState::Focused) {
        dl->AddRect(
            ImVec2(p.x - 3.0f, p.y - 3.0f),
            ImVec2(p.x + size.x + 3.0f, p.y + size.y + 3.0f),
            U32(WithAlpha(th.controlAccent, 0.58f)), 3.0f, 0, 2.0f);
    }
    ImVec2 ts = ImGui::CalcTextSize(label);
    dl->AddText(
        ImVec2(p.x + (size.x - ts.x) * 0.5f, p.y + (size.y - ts.y) * 0.5f),
        U32(fg), label);
}

void DrawButtonMatrix() {
    SectionTitle("Button / IconButton State Matrix", "gallery-local forced states; shared widgets unchanged");
    const char* columns[] = {"Default", "Hover", "Active", "Disabled", "Focused"};
    const char* rows[] = {"Primary", "Secondary", "Ghost", "Danger"};
    const SpecButtonVariant variants[] = {
        SpecButtonVariant::Primary,
        SpecButtonVariant::Secondary,
        SpecButtonVariant::Ghost,
        SpecButtonVariant::Danger
    };
    const SpecState states[] = {
        SpecState::Default, SpecState::Hover, SpecState::Active, SpecState::Disabled, SpecState::Focused
    };

    ImGui::Columns(6, "button-matrix", false);
    ImGui::SetColumnWidth(0, 100.0f);
    ImGui::TextUnformatted("");
    ImGui::NextColumn();
    for (const char* col : columns) {
        TextMuted(col);
        ImGui::NextColumn();
    }
    for (int r = 0; r < 4; ++r) {
        ImGui::TextUnformatted(rows[r]);
        ImGui::NextColumn();
        for (int c = 0; c < 5; ++c) {
            ImGui::PushID(r * 10 + c);
            DrawSpecButton(r == 3 ? "Delete" : "Import", variants[r], states[c], ImVec2(78.0f, 24.0f));
            ImGui::PopID();
            ImGui::NextColumn();
        }
    }
    ImGui::Columns(1);

    ImGui::Spacing();
    TextMuted("Live wrapper samples");
    ButtonOptions primary;
    primary.variant = ButtonVariant::PrimarySearch;
    primary.size = ButtonSize::Md;
    Button("Search", primary);
    ImGui::SameLine();
    ButtonOptions secondary;
    secondary.variant = ButtonVariant::Secondary;
    Button("Save", secondary);
    ImGui::SameLine();
    IconButton(ICON_LC_X, "Close", ButtonSize::Sm);
    ImGui::SameLine();
    IconButton(ICON_LC_TRASH, "Delete", ButtonSize::Sm);
}

void DrawInputStateBox(const char* label, const char* value, SpecState state, bool search) {
    const auto& th = ThemeColors::current();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImVec2 size(210.0f, 28.0f);
    ImGui::InvisibleButton(label, size);
    ImVec4 border = state == SpecState::Focused ? th.controlAccent : th.chromeBorder;
    if (state == SpecState::Hover) border = Mix(border, ImVec4(1, 1, 1, 1), 0.15f);
    ImVec4 text = state == SpecState::Disabled ? WithAlpha(th.resultsSecondaryText, 0.35f) : th.resultsText;
    dl->AddRectFilled(p, ImVec2(p.x + size.x, p.y + size.y), U32(th.frameBgBase), 2.0f);
    dl->AddRect(p, ImVec2(p.x + size.x, p.y + size.y), U32(border), 2.0f);
    float textX = p.x + 8.0f;
    if (search) {
        dl->AddText(ImVec2(p.x + 8.0f, p.y + 7.0f), U32(state == SpecState::Focused ? th.controlAccent : th.resultsSecondaryText), "S");
        textX += 18.0f;
        dl->AddText(ImVec2(p.x + size.x - 18.0f, p.y + 7.0f), U32(th.resultsSecondaryText), "x");
    }
    dl->AddText(ImVec2(textX, p.y + 7.0f), U32(text), value);
}

void DrawInputsSelection() {
    SectionTitle("Inputs / Selection", "visual matrix plus live wrapper samples");
    const char* states[] = {"Default", "Hover", "Focused", "Disabled"};
    const SpecState stateIds[] = {SpecState::Default, SpecState::Hover, SpecState::Focused, SpecState::Disabled};
    ImGui::Columns(4, "input-matrix", false);
    for (int i = 0; i < 4; ++i) {
        TextMuted(states[i]);
        DrawInputStateBox(states[i], i == 2 ? "Explosion_Sub_03" : "search...", stateIds[i], true);
        ImGui::NextColumn();
    }
    ImGui::Columns(1);

    ImGui::Spacing();
    static std::string searchText = "metal impact";
    TextInputOptions searchOpts;
    searchOpts.placeholder = "Search library";
    searchOpts.leadingIcon = "S";
    searchOpts.trailingIcon = "x";
    searchOpts.width = 280.0f;
    TextInput("SearchInput", &searchText, searchOpts);

    static std::string pathText = "E:\\Libraries\\Boom\\Designed\\Impacts";
    TextInputOptions pathOpts;
    pathOpts.variant = TextInputVariant::Path;
    pathOpts.width = 360.0f;
    pathOpts.trailingIcon = "...";
    pathOpts.trailingTooltip = "Browse";
    TextInput("PathInput", &pathText, pathOpts);

    static float hz = 96000.0f;
    NumberInput("Sample Rate", &hz, 8000.0f, 192000.0f, 1000.0f, "%.0f Hz");

    static bool checked = true;
    static bool muted = false;
    static int route = 1;
    static const char* routing[] = {"WASAPI", "ASIO Dev1", "Virtual 5.1"};
    Checkbox("Has waveform cache", &checked);
    ImGui::SameLine();
    Radio("Stereo", &route, 0);
    ImGui::SameLine();
    Radio("5.1", &route, 1);
    ImGui::SameLine();
    Toggle("Bypass", &muted);
    Combo("Output route", &route, routing, 3);
}

void DrawBadge(const char* text, ImVec4 color, bool filled = false) {
    const auto& th = ThemeColors::current();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 ts = ImGui::CalcTextSize(text);
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImVec2 size(ts.x + 10.0f, 16.0f);
    ImGui::InvisibleButton(text, size);
    ImVec4 bg = filled ? WithAlpha(color, 0.22f) : th.dialogPanelBG;
    dl->AddRectFilled(p, ImVec2(p.x + size.x, p.y + size.y), U32(bg), 2.0f);
    dl->AddRect(p, ImVec2(p.x + size.x, p.y + size.y), U32(WithAlpha(color, 0.72f)), 2.0f);
    dl->AddText(ImVec2(p.x + 5.0f, p.y + 1.0f), U32(th.resultsText), text);
}

void DrawStatusBadgeChip() {
    const auto& th = ThemeColors::current();
    SectionTitle("StatusDot / Badge / Chip", "small semantic marks only; no saturated full-row blocks");
    StatusDot(true, 6.0f);
    ImGui::SameLine();
    ImGui::TextUnformatted("Ready");
    ImGui::SameLine(0.0f, 18.0f);
    DrawBadge("WAV 96k", th.controlAccent, false);
    ImGui::SameLine();
    DrawBadge("5.1", th.wwiseDiffReplaced, false);
    ImGui::SameLine();
    DrawBadge("Scanning", th.meterWarning, true);
    ImGui::SameLine();
    DrawBadge("Offline", th.wwiseConnectFail, true);
    ImGui::Spacing();
    DrawBadge("impact  x", th.resultsSecondaryText, false);
    ImGui::SameLine();
    DrawBadge("weapon  x", th.resultsSecondaryText, false);
    ImGui::SameLine();
    DiffBadge(DiffStatus::New);
    ImGui::SameLine();
    DiffBadge(DiffStatus::Replaced);
}

void DrawDenseRow(int index, const char* file, const char* type, const char* sizeText,
                  const char* rate, const char* ch, bool selected, bool playing) {
    const auto& th = ThemeColors::current();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x;
    const float h = 22.0f;
    ImGui::InvisibleButton(file, ImVec2(w, h));

    ImVec4 bg = (index % 2 == 0) ? WithAlpha(th.resultsText, 0.020f) : WithAlpha(th.resultsText, 0.0f);
    if (selected) bg = WithAlpha(th.controlAccent, 0.20f);
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), U32(bg), 0.0f);
    if (playing) {
        dl->AddRectFilled(p, ImVec2(p.x + 3.0f, p.y + h), U32(th.controlAccent), 0.0f);
        dl->AddText(ImVec2(p.x + 8.0f, p.y + 4.0f), U32(th.controlAccent), ">");
    } else {
        char num[8];
        std::snprintf(num, sizeof(num), "%d", index + 1);
        dl->AddText(ImVec2(p.x + 8.0f, p.y + 4.0f), U32(th.resultsSecondaryText), num);
    }

    const ImU32 mainText = U32(selected ? th.resultsText : th.resultsSecondaryText);
    dl->AddText(ImVec2(p.x + 34.0f, p.y + 4.0f), mainText, file);
    dl->AddText(ImVec2(p.x + w * 0.47f, p.y + 4.0f), U32(th.resultsText), type);
    dl->AddText(ImVec2(p.x + w * 0.63f, p.y + 4.0f), U32(th.resultsSecondaryText), sizeText);
    dl->AddText(ImVec2(p.x + w * 0.76f, p.y + 4.0f), U32(th.resultsSecondaryText), rate);
    dl->AddText(ImVec2(p.x + w * 0.90f, p.y + 4.0f), U32(th.resultsSecondaryText), ch);
}

void DrawDenseTable() {
    const auto& th = ThemeColors::current();
    SectionTitle("Dense Table Row", "visual sample only; real result list is not migrated in this slice");
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x;
    const float h = 26.0f;
    ImGui::InvisibleButton("dense-header", ImVec2(w, h));
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), U32(th.resultsHeaderBG), 0.0f);
    dl->AddRect(p, ImVec2(p.x + w, p.y + h), U32(WithAlpha(th.chromeBorder, 0.55f)), 0.0f);
    dl->AddText(ImVec2(p.x + 8.0f, p.y + 6.0f), U32(th.resultsSecondaryText), "#");
    dl->AddText(ImVec2(p.x + 34.0f, p.y + 6.0f), U32(th.resultsSecondaryText), "File Name");
    dl->AddText(ImVec2(p.x + w * 0.47f, p.y + 6.0f), U32(th.resultsSecondaryText), "Type");
    dl->AddText(ImVec2(p.x + w * 0.63f, p.y + 6.0f), U32(th.resultsSecondaryText), "Size");
    dl->AddText(ImVec2(p.x + w * 0.76f, p.y + 6.0f), U32(th.resultsSecondaryText), "Sample Rate");
    dl->AddText(ImVec2(p.x + w * 0.90f, p.y + 6.0f), U32(th.resultsSecondaryText), "Ch");

    DrawDenseRow(0, "Laser_Shoot_01.wav", "FX", "1.2 MB", "44100 Hz", "ST", false, true);
    DrawDenseRow(1, "Explosion_Sub_03.wav", "DESIGN", "4.8 MB", "96000 Hz", "5.1", true, false);
    DrawDenseRow(2, "Ambient_Wind_L_02.wav", "AMB", "22.4 MB", "192000 Hz", "QD", false, false);
    DrawDenseRow(3, "Door_Metal_Close_07.wav", "FOLEY", "842 KB", "48000 Hz", "MO", false, false);
}

void DrawWaveformSample() {
    const auto& th = ThemeColors::current();
    SectionTitle("Waveform / Meter Sample", "static visual reference; no audio or cache behavior changed");
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x;
    const float h = 94.0f;
    ImGui::InvisibleButton("waveform-meter", ImVec2(w, h));
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), U32(th.frameBgBase), 2.0f);
    dl->AddRect(p, ImVec2(p.x + w, p.y + h), U32(WithAlpha(th.chromeBorder, 0.8f)), 2.0f);

    const float waveH = 52.0f;
    const float cy = p.y + 28.0f;
    dl->AddLine(ImVec2(p.x + 10.0f, cy), ImVec2(p.x + w - 10.0f, cy), U32(WithAlpha(th.resultsSecondaryText, 0.25f)), 1.0f);
    for (int i = 0; i < 96; ++i) {
        const float x = p.x + 12.0f + (w - 24.0f) * (static_cast<float>(i) / 95.0f);
        const float amp = (std::sin(i * 0.42f) * 0.5f + std::sin(i * 0.15f) * 0.5f) * 0.5f + 0.5f;
        const float y0 = cy - amp * waveH * 0.45f;
        const float y1 = cy + amp * waveH * 0.45f;
        dl->AddLine(ImVec2(x, y0), ImVec2(x, y1), U32(WithAlpha(th.waveformColor, 0.86f)), 1.0f);
    }
    const float playX = p.x + w * 0.58f;
    dl->AddLine(ImVec2(playX, p.y + 8.0f), ImVec2(playX, p.y + 58.0f), U32(th.controlAccent), 1.0f);
    dl->AddRectFilled(ImVec2(p.x + w * 0.32f, p.y + 8.0f), ImVec2(p.x + w * 0.55f, p.y + 58.0f), U32(WithAlpha(th.controlAccent, 0.16f)), 0.0f);

    const float meterY = p.y + 70.0f;
    const float meterW = w - 24.0f;
    dl->AddRectFilled(ImVec2(p.x + 12.0f, meterY), ImVec2(p.x + 12.0f + meterW, meterY + 10.0f), U32(th.meterBg), 1.0f);
    dl->AddRectFilled(ImVec2(p.x + 12.0f, meterY), ImVec2(p.x + 12.0f + meterW * 0.68f, meterY + 10.0f), U32(th.meterBase), 1.0f);
    dl->AddRectFilled(ImVec2(p.x + 12.0f + meterW * 0.68f, meterY), ImVec2(p.x + 12.0f + meterW * 0.88f, meterY + 10.0f), U32(th.meterWarning), 1.0f);
    dl->AddRectFilled(ImVec2(p.x + 12.0f + meterW * 0.88f, meterY), ImVec2(p.x + 12.0f + meterW * 0.96f, meterY + 10.0f), U32(th.meterClip), 1.0f);
    dl->AddLine(ImVec2(p.x + 12.0f + meterW * 0.91f, meterY - 2.0f), ImVec2(p.x + 12.0f + meterW * 0.91f, meterY + 13.0f), U32(th.wwiseConnectFail), 1.0f);
    dl->AddText(ImVec2(p.x + 12.0f, meterY + 17.0f), U32(th.resultsSecondaryText), "-60 dB");
    dl->AddText(ImVec2(p.x + 12.0f + meterW * 0.67f, meterY + 17.0f), U32(th.resultsSecondaryText), "-6 dB");
    dl->AddText(ImVec2(p.x + 12.0f + meterW * 0.94f, meterY + 17.0f), U32(th.resultsSecondaryText), "0 dB");
}

void BeginPrototypePanel(const char* id, const char* title, const char* note, ImVec2 size,
                         ImGuiWindowFlags flags = 0) {
    const auto& th = ThemeColors::current();
    ImGui::PushStyleColor(ImGuiCol_ChildBg, th.dialogPanelBG);
    ImGui::BeginChild(id, size, true, flags);
    ImGui::TextUnformatted(title);
    if (note && note[0]) {
        ImGui::SameLine();
        TextMuted(note);
    }
    Divider(DividerVariant::Subtle);
}

void EndPrototypePanel() {
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

void DrawPrototypeTopChrome() {
    const auto& th = ThemeColors::current();
    BeginPrototypePanel("proto-top-chrome", "Top Chrome", "authoritative ImGui shell, React is reference-only",
                        ImVec2(0.0f, 110.0f), ImGuiWindowFlags_NoScrollbar);
    ImGui::PushStyleColor(ImGuiCol_Text, th.resultsSecondaryText);
    ImGui::TextUnformatted("Harken Search - Auro2 Internal Prototype (108761 files)");
    ImGui::PopStyleColor();

    const char* menus[] = {"File", "Edit", "View", "Database", "Playlist", "Tools", "Settings", "Help"};
    for (int i = 0; i < static_cast<int>(sizeof(menus) / sizeof(menus[0])); ++i) {
        if (i > 0) ImGui::SameLine();
        ButtonOptions menuBtn;
        menuBtn.variant = ButtonVariant::Ghost;
        menuBtn.size = ButtonSize::Sm;
        Button(menus[i], menuBtn);
    }

    ImGui::Spacing();
    ButtonOptions primary;
    primary.variant = ButtonVariant::PrimarySearch;
    primary.size = ButtonSize::Md;
    Button("Search", primary);
    ImGui::SameLine();
    ButtonOptions secondary;
    secondary.variant = ButtonVariant::Secondary;
    Button("Database", secondary);
    ImGui::SameLine();
    Button("Playlists", secondary);
    ImGui::SameLine();
    Button("Scan", secondary);
    ImGui::SameLine();
    Button("Refresh", secondary);
    ImGui::SameLine();
    TextMuted("Dense workstation chrome: menu + search + mode routing");
    EndPrototypePanel();
}

void DrawPrototypeSidebar() {
    BeginPrototypePanel("proto-sidebar", "Left Sidebar", "rail + related paths + folders",
                        ImVec2(250.0f, 0.0f));
    ImGui::BeginChild("proto-sidebar-rail", ImVec2(28.0f, 0.0f), false);
    const char* rail[] = {"O", "F", "T", "S", "G", "P"};
    for (int i = 0; i < static_cast<int>(sizeof(rail) / sizeof(rail[0])); ++i) {
        ImGui::PushID(i);
        ButtonOptions icon;
        icon.variant = (i == 0) ? ButtonVariant::PrimarySearch : ButtonVariant::Ghost;
        icon.size = ButtonSize::Sm;
        Button(rail[i], icon);
        ImGui::PopID();
    }
    ImGui::EndChild();

    ImGui::SameLine();
    ImGui::BeginChild("proto-sidebar-stack", ImVec2(0.0f, 0.0f), false);
    if (BeginPanel("proto-related", PanelVariant::Inset, 0.0f, 118.0f)) {
        ImGui::TextUnformatted("Related Paths");
        Divider(DividerVariant::Subtle);
        TextMuted("H:");
        ImGui::TextUnformatted("Audiences");
        ImGui::TextUnformatted("CONCERT HALL");
        EndPanel();
    }

    ImGui::Spacing();
    if (BeginPanel("proto-folders", PanelVariant::Inset, 0.0f, 0.0f)) {
        ImGui::TextUnformatted("Folders");
        Divider(DividerVariant::Subtle);
        TextMuted("H:");
        ImGui::BulletText("Sound Effect");
        ImGui::BulletText("BoomLibrary");
        ImGui::BulletText("Crowds Concert Audiences");
        ImGui::BulletText("CONCERT HALL");
        EndPanel();
    }
    ImGui::EndChild();
    EndPrototypePanel();
}

void DrawPrototypeResultTable() {
    const auto& th = ThemeColors::current();
    BeginPrototypePanel("proto-browser", "Center Browser", "breadcrumb + view header + dense rows",
                        ImVec2(0.0f, 0.0f));
    TextMuted("Folder");
    ImGui::SameLine();
    ImGui::TextUnformatted("H: > Sound Effect > BoomLibrary > Crowds Concert Audiences > CONCERT HALL");
    Divider(DividerVariant::Subtle);

    ButtonOptions active;
    active.variant = ButtonVariant::PrimarySearch;
    active.size = ButtonSize::Sm;
    Button("All Files (108761)", active);
    ImGui::SameLine();
    ButtonOptions ghost;
    ghost.variant = ButtonVariant::Ghost;
    ghost.size = ButtonSize::Sm;
    Button("+", ghost);
    ImGui::SameLine();
    TextMuted("Waveform column stays first-class in Auro2");

    ImGui::Spacing();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x;
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + 26.0f), U32(th.resultsHeaderBG), 0.0f);
    dl->AddRect(p, ImVec2(p.x + w, p.y + 26.0f), U32(WithAlpha(th.chromeBorder, 0.5f)), 0.0f);
    dl->AddText(ImVec2(p.x + 8.0f, p.y + 6.0f), U32(th.resultsSecondaryText), "Filename");
    dl->AddText(ImVec2(p.x + w * 0.60f, p.y + 6.0f), U32(th.resultsSecondaryText), "Waveform");
    dl->AddText(ImVec2(p.x + w * 0.78f, p.y + 6.0f), U32(th.resultsSecondaryText), "Length");
    dl->AddText(ImVec2(p.x + w * 0.90f, p.y + 6.0f), U32(th.resultsSecondaryText), "Rate");
    ImGui::Dummy(ImVec2(w, 26.0f));

    for (int i = 0; i < static_cast<int>(sizeof(kProtoRows) / sizeof(kProtoRows[0])); ++i) {
        const ProtoRow& row = kProtoRows[i];
        ImVec2 rowPos = ImGui::GetCursorScreenPos();
        const float rowH = 24.0f;
        ImGui::InvisibleButton(row.name, ImVec2(w, rowH));
        ImVec4 bg = row.selected ? WithAlpha(th.controlAccent, 0.18f)
                                 : ((i % 2 == 0) ? WithAlpha(th.resultsText, 0.02f) : WithAlpha(th.resultsText, 0.0f));
        dl->AddRectFilled(rowPos, ImVec2(rowPos.x + w, rowPos.y + rowH), U32(bg), 0.0f);
        if (row.selected) {
            dl->AddRectFilled(rowPos, ImVec2(rowPos.x + 3.0f, rowPos.y + rowH), U32(th.controlAccent), 0.0f);
        }
        dl->AddText(ImVec2(rowPos.x + 8.0f, rowPos.y + 5.0f), U32(row.selected ? th.resultsText : th.resultsSecondaryText), row.name);
        DrawPrototypeWaveBars(ImVec2(rowPos.x + w * 0.60f, rowPos.y), w * 0.12f, rowPos.y + rowH * 0.5f, 18,
                              U32(WithAlpha(th.waveformColor, 0.82f)));
        dl->AddText(ImVec2(rowPos.x + w * 0.78f, rowPos.y + 5.0f), U32(th.resultsSecondaryText), row.length);
        dl->AddText(ImVec2(rowPos.x + w * 0.90f, rowPos.y + 5.0f), U32(th.resultsSecondaryText), row.rate);
    }
    EndPrototypePanel();
}

void DrawPrototypeInfoPanel() {
    const auto& th = ThemeColors::current();
    BeginPrototypePanel("proto-info", "Right Info", "summary + tabs + artwork/info cards",
                        ImVec2(320.0f, 0.0f));
    ImGui::TextUnformatted("Concert Hall Audience Set");
    TextMuted("Boom Library Crowds, Audience / Concert Hall, preview ready");
    Divider(DividerVariant::Subtle);

    for (int i = 0; i < static_cast<int>(sizeof(kInfoTabs) / sizeof(kInfoTabs[0])); ++i) {
        if (i > 0) ImGui::SameLine();
        ButtonOptions tab;
        tab.variant = (i == 1) ? ButtonVariant::PrimarySearch : ButtonVariant::Secondary;
        tab.size = ButtonSize::Sm;
        Button(kInfoTabs[i], tab);
    }

    ImGui::Spacing();
    if (BeginPanel("proto-artwork", PanelVariant::Inset, 0.0f, 160.0f)) {
        ImGui::TextUnformatted("Artwork");
        Divider(DividerVariant::Subtle);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 p = ImGui::GetCursorScreenPos();
        const float tileW = 86.0f;
        const float tileH = 64.0f;
        const char* tiles[] = {"Brussels Hall", "Titan", "Momentum", "Backstreet", "Forest", "Neon"};
        for (int i = 0; i < static_cast<int>(sizeof(tiles) / sizeof(tiles[0])); ++i) {
            const int col = i % 3;
            const int row = i / 3;
            const float x = p.x + col * (tileW + 8.0f);
            const float y = p.y + row * (tileH + 8.0f);
            ImVec4 base = (i % 2 == 0) ? th.controlAccent : th.dialogPanelBG;
            dl->AddRectFilled(ImVec2(x, y), ImVec2(x + tileW, y + tileH), U32(WithAlpha(base, 0.28f)), 4.0f);
            dl->AddRect(ImVec2(x, y), ImVec2(x + tileW, y + tileH), U32(WithAlpha(th.chromeBorder, 0.75f)), 4.0f);
            dl->AddText(ImVec2(x + 6.0f, y + 8.0f), U32(th.resultsText), "SFX");
            dl->AddText(ImVec2(x + 6.0f, y + tileH - 18.0f), U32(th.resultsText), tiles[i]);
        }
        ImGui::Dummy(ImVec2(0.0f, 144.0f));
        EndPanel();
    }

    ImGui::Spacing();
    if (BeginPanel("proto-metadata", PanelVariant::Inset, 0.0f, 0.0f)) {
        ImGui::TextUnformatted("Metadata Summary");
        Divider(DividerVariant::Subtle);
        TextMuted("Name");
        ImGui::SameLine(110.0f);
        ImGui::TextUnformatted("CA CONCERT HALL 02 Medium Audience Applause 26 RF");
        TextMuted("Format");
        ImGui::SameLine(110.0f);
        ImGui::TextUnformatted("WAV");
        TextMuted("Sample Rate");
        ImGui::SameLine(110.0f);
        ImGui::TextUnformatted("48000");
        TextMuted("Tags");
        ImGui::SameLine(110.0f);
        ImGui::TextUnformatted("Audience, Concert Hall, Medium, Applause");
        EndPanel();
    }
    EndPrototypePanel();
}

void DrawPrototypeTransport() {
    BeginPrototypePanel("proto-transport", "Transport Strip", "time + playback + progress + mode",
                        ImVec2(0.0f, 70.0f), ImGuiWindowFlags_NoScrollbar);
    ImGui::TextUnformatted("00:00:03.18");
    ImGui::SameLine(120.0f);
    ButtonOptions ghost;
    ghost.variant = ButtonVariant::Ghost;
    ghost.size = ButtonSize::Sm;
    Button("Play", ghost);
    ImGui::SameLine();
    Button("Prev", ghost);
    ImGui::SameLine();
    Button("Next", ghost);
    ImGui::SameLine();
    Button("Loop", ghost);
    ImGui::SameLine(360.0f);
    TextMuted("24%  |  ST  |  NORM  |  L R Loop");
    EndPrototypePanel();
}

void DrawPrototypeWaveStage() {
    const auto& th = ThemeColors::current();
    BeginPrototypePanel("proto-wave-stage", "Waveform Stage", "bottom audition surface remains visible in shell",
                        ImVec2(0.0f, 176.0f));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x;
    const float h = 112.0f;
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), U32(th.frameBgBase), 2.0f);
    dl->AddRect(p, ImVec2(p.x + w, p.y + h), U32(WithAlpha(th.chromeBorder, 0.8f)), 2.0f);
    for (int i = 0; i < 11; ++i) {
        const float x = p.x + (w * static_cast<float>(i) / 10.0f);
        dl->AddLine(ImVec2(x, p.y), ImVec2(x, p.y + h), U32(WithAlpha(th.resultsSecondaryText, 0.14f)), 1.0f);
    }
    DrawPrototypeWaveBars(ImVec2(p.x + 12.0f, p.y), w - 24.0f, p.y + 48.0f, 96, U32(WithAlpha(th.waveformColor, 0.88f)));
    dl->AddRectFilled(ImVec2(p.x + w * 0.34f, p.y + 10.0f), ImVec2(p.x + w * 0.57f, p.y + 82.0f),
                      U32(WithAlpha(th.controlAccent, 0.16f)), 0.0f);
    dl->AddLine(ImVec2(p.x + w * 0.59f, p.y + 8.0f), ImVec2(p.x + w * 0.59f, p.y + 90.0f), U32(th.controlAccent), 1.0f);
    const float meterY = p.y + 92.0f;
    dl->AddRectFilled(ImVec2(p.x + 12.0f, meterY), ImVec2(p.x + w - 12.0f, meterY + 10.0f), U32(th.meterBg), 1.0f);
    dl->AddRectFilled(ImVec2(p.x + 12.0f, meterY), ImVec2(p.x + 12.0f + (w - 24.0f) * 0.71f, meterY + 10.0f), U32(th.meterBase), 1.0f);
    dl->AddRectFilled(ImVec2(p.x + 12.0f + (w - 24.0f) * 0.71f, meterY), ImVec2(p.x + 12.0f + (w - 24.0f) * 0.88f, meterY + 10.0f), U32(th.meterWarning), 1.0f);
    dl->AddRectFilled(ImVec2(p.x + 12.0f + (w - 24.0f) * 0.88f, meterY), ImVec2(p.x + 12.0f + (w - 24.0f) * 0.97f, meterY + 10.0f), U32(th.meterClip), 1.0f);
    ImGui::Dummy(ImVec2(0.0f, h + 18.0f));
    TextMuted("ST");
    ImGui::SameLine();
    TextMuted("-70  -70");
    ImGui::SameLine(160.0f);
    TextMuted("Waveform is kept as a first-class shell region, not hidden behind later migration.");
    EndPrototypePanel();
}

void DrawPrototypeStatusBar() {
    BeginPrototypePanel("proto-status", "Status Bar", "database state + current file + transfer action",
                        ImVec2(0.0f, 58.0f), ImGuiWindowFlags_NoScrollbar);
    ImGui::TextUnformatted("Database 108761 files");
    ImGui::SameLine(220.0f);
    TextMuted("Current: CA CONCERT HALL 02 Medium Audience Applause 26 RF");
    ImGui::SameLine();
    ButtonOptions action;
    action.variant = ButtonVariant::PrimaryTransfer;
    action.size = ButtonSize::Sm;
    Button("Send to REAPER", action);
    EndPrototypePanel();
}

void DrawAuro2ShellPrototype() {
    SectionTitle("Harken Search Window Template", "one reusable standalone module window");
    TextMuted("This is the baseline independent window shell for future modules.");
    TextMuted("Keep the structure, then swap the inner business content per module.");
    ImGui::Spacing();

    DrawPrototypeTopChrome();
    ImGui::Spacing();

    const float mainHeight = 360.0f;
    const float leftWidth = 250.0f;
    const float rightWidth = 320.0f;
    const float gap = 8.0f;
    const float totalWidth = ImGui::GetContentRegionAvail().x;
    const float centerWidth = std::max(280.0f, totalWidth - leftWidth - rightWidth - gap * 2.0f);
    DrawPrototypeSidebar();
    ImGui::SameLine(0.0f, gap);
    ImGui::BeginChild("proto-center-stack", ImVec2(centerWidth, mainHeight), false, ImGuiWindowFlags_NoScrollbar);
    DrawPrototypeResultTable();
    ImGui::EndChild();
    ImGui::SameLine(0.0f, gap);
    DrawPrototypeInfoPanel();

    ImGui::Spacing();
    DrawPrototypeTransport();
    ImGui::Spacing();
    DrawPrototypeWaveStage();
    ImGui::Spacing();
    DrawPrototypeStatusBar();
}

void DrawLeftNav() {
    const auto& th = ThemeColors::current();
    const char* sections[] = {
        "Auro2 Shell",
        "Overview",
        "Tokens",
        "Typography",
        "Buttons",
        "Inputs",
        "Status",
        "Dense Table",
        "Waveform"
    };
    ImGui::PushStyleColor(ImGuiCol_ChildBg, th.dialogPanelBG);
    ImGui::BeginChild("gallery-nav", ImVec2(220.0f, 0.0f), true);
    ImGui::TextUnformatted("Harken Search UI Gallery");
    TextMuted("v2.1 phase-1 contract");
    Divider(DividerVariant::Subtle);
    for (int i = 0; i < static_cast<int>(sizeof(sections) / sizeof(sections[0])); ++i) {
        ImGui::PushID(i);
        const bool selected = State().section == i;
        ImVec4 bg = selected ? WithAlpha(th.controlAccent, 0.22f) : WithAlpha(th.dialogPanelBG, 0.0f);
        ImGui::PushStyleColor(ImGuiCol_Header, bg);
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, WithAlpha(th.resultsText, 0.08f));
        ImGui::PushStyleColor(ImGuiCol_HeaderActive, WithAlpha(th.controlAccent, 0.26f));
        if (ImGui::Selectable(sections[i], selected, 0, ImVec2(0.0f, 24.0f))) {
            State().section = i;
        }
        ImGui::PopStyleColor(3);
        ImGui::PopID();
    }
    ImGui::Spacing();
    Divider(DividerVariant::Subtle);
    TextMuted("Baseline preserved:");
    TextMuted("HarkenSearchWidgets API");
    TextMuted("HarkenSearchMetrics constants");
    TextMuted("ThemeColorScheme tokens");
    TextMuted("5-theme QA gate");
    ImGui::Spacing();
    TextMuted("ImGui authority:");
    TextMuted("React = reference only");
    TextMuted("F9 window = shell prototype");
    TextMuted("production UI untouched");
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

void DrawOverview() {
    SectionTitle("Harken Search UI Component Gallery v2.1", "delta over the current Harken Search UI v1 baseline");
    TextMuted("Dark-room industrial, high-density, immediate-mode native. This gallery is the first review gate before real UI migration.");
    TextMuted("It links the current used design contract before showing v2.1 improvements, so visual iteration does not drift from production rules.");
    ImGui::Spacing();
    DrawBaselineLinks();
    ImGui::Spacing();
    DrawSurfaceStack();
    ImGui::Spacing();
    DrawTypographyDensity();
}

void DrawCanvas() {
    const auto& th = ThemeColors::current();
    ImGui::PushStyleColor(ImGuiCol_ChildBg, th.mainWindowBG);
    ImGui::BeginChild("gallery-canvas", ImVec2(0.0f, 0.0f), true);
    switch (State().section) {
        case 0:
            DrawAuro2ShellPrototype();
            break;
        case 1:
            DrawOverview();
            break;
        case 2:
            DrawTokenGallery();
            break;
        case 3:
            DrawTypographyDensity();
            break;
        case 4:
            DrawButtonMatrix();
            break;
        case 5:
            DrawInputsSelection();
            break;
        case 6:
            DrawStatusBadgeChip();
            break;
        case 7:
            DrawDenseTable();
            break;
        case 8:
            DrawWaveformSample();
            break;
        default:
            DrawAuro2ShellPrototype();
            break;
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

} // anonymous namespace

void DrawHarkenSearchSmokeWindow(bool* open) {
    if (!open || !*open) return;

    const auto& th = ThemeColors::current();
    ImGui::SetNextWindowSize(ImVec2(1180, 860), ImGuiCond_FirstUseEver);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, th.mainWindowBG);
    if (!ImGui::Begin("Harken Search Window Template Prototype", open)) {
        ImGui::PopStyleColor();
        ImGui::End();
        return;
    }

    ImGui::TextUnformatted("Harken Search Window Template Prototype");
    ImGui::SameLine();
    TextMuted("F9 dev-only shell authority; template baseline for standalone ImGui windows");
    Divider(DividerVariant::Strong);
    DrawAuro2ShellPrototype();

    ImGui::End();
    ImGui::PopStyleColor();
}

}

#endif // AURA_BUILD_DIAGNOSTICS
