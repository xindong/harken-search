#include "AppState.h"
#include "BuiltinThemeScheme.h"
#include "ui/ActionButtonStyle.h"
#include "ui/CloseIconButton.h"
#include "imgui_internal.h"
#include "lucide/IconsLucide.h"
#include <cmath>
#include <cstdio>
#include <filesystem>

using aura::ui::ButtonVariant;
namespace Style = aura::ui::ActionButtonStyle;
static int failures = 0;
static void Check(bool ok, const char* label) {
    if (!ok) { ++failures; std::printf("FAIL %s\n", label); }
}
static bool Same(ImVec4 a, ImVec4 b) {
    return a.x == b.x && a.y == b.y && a.z == b.z && a.w == b.w;
}

static void CheckCloseGlyphCenter() {
    auto& io = ImGui::GetIO();
    const bool trickle = io.ConfigInputTrickleEventQueue;
    io.ConfigInputTrickleEventQueue = false;
    for (float fontSize : {14.0f, 21.7f, 28.0f}) {
        for (int frame = 0; frame < 5; ++frame) {
            io.AddMousePosEvent(frame < 2 ? -100.f : 77.f, frame < 2 ? -100.f : 80.f);
            io.AddMouseButtonEvent(0, frame == 3);
            ImGui::NewFrame();
            ImGui::SetNextWindowPos(ImVec2(20, 20));
            ImGui::SetNextWindowSize(ImVec2(300, 160));
            ImGui::Begin("close glyph geometry", nullptr, ImGuiWindowFlags_NoSavedSettings);
            ImGui::SetCursorScreenPos(ImVec2(60, 70));
            ImGui::PushFont(nullptr, fontSize);
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6, 0));
            aura::ui::CloseIconButton::Draw(ICON_LC_X, ImVec2(34, 20),
                ImVec4(.8f, .8f, .8f, 1), ImVec4(.1f, .1f, .1f, 1));
            if (frame >= 1) {
                unsigned int codepoint = 0;
                ImTextCharFromUtf8(&codepoint, ICON_LC_X, nullptr);
                Check(ImGui::GetFontBaked()->FindGlyphNoFallback((ImWchar)codepoint) != nullptr,
                      "centering test uses the real Lucide X");
                const auto& vertices = ImGui::GetWindowDrawList()->VtxBuffer;
                ImRect ink(vertices[vertices.Size - 4].pos, vertices[vertices.Size - 4].pos);
                for (int i = vertices.Size - 3; i < vertices.Size; ++i) ink.Add(vertices[i].pos);
                const ImRect button(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
                const auto delta = ImVec2(ink.GetCenter().x - button.GetCenter().x,
                                          ink.GetCenter().y - button.GetCenter().y);
                std::printf("close glyph size=%.1f frame=%d delta=(%.2f,%.2f)\n",
                            fontSize, frame, delta.x, delta.y);
                Check(std::fabs(delta.x) <= .6f && std::fabs(delta.y) <= .6f,
                      "Lucide X is centered inside its own button surface");
                Check(button.Contains(ink), "close glyph stays within its hit area");
            }
            ImGui::PopStyleVar();
            ImGui::PopFont();
            ImGui::End();
            ImGui::Render();
        }
    }
    io.ConfigInputTrickleEventQueue = trickle;
}

static void CheckCloseIconStates() {
    auto& io = ImGui::GetIO();
    const bool trickle = io.ConfigInputTrickleEventQueue;
    io.ConfigInputTrickleEventQueue = false;
    for (int themeId : {0, 1, 2, 3, 10}) {
        const auto theme = MakeBuiltinScheme(static_cast<AppTheme>(themeId));
        const auto surface = TransportContrast::Over(theme.dialogBG, theme.mainWindowBG);
        ImVec4 hover = theme.filterBrowserButtonBG; hover.w *= .5f;
        bool seenNormal = false, seenHover = false, seenActive = false;
        ImU32 normalInk = 0, hoverInk = 0, activeInk = 0;
        for (int frame = 0; frame < 7; ++frame) {
            io.AddMousePosEvent(frame < 2 ? -100.f : 74.f, frame < 2 ? -100.f : 86.f);
            io.AddMouseButtonEvent(0, frame == 3 || frame == 5);
            ImGui::NewFrame();
            ImGui::SetNextWindowPos(ImVec2(20, 20));
            ImGui::SetNextWindowSize(ImVec2(300, 160));
            ImGui::PushStyleColor(ImGuiCol_WindowBg, surface);
            ImGui::Begin("close icon", nullptr, ImGuiWindowFlags_NoSavedSettings);
            ImGui::SetCursorScreenPos(ImVec2(60, 70));
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, hover);
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, theme.filterBrowserButtonBG);
            const bool disabled = frame >= 5;
            if (disabled) ImGui::BeginDisabled();
            const int colors = GImGui->ColorStack.Size, vars = GImGui->StyleVarStack.Size;
            const int verticesBefore = ImGui::GetWindowDrawList()->VtxBuffer.Size;
            const bool clicked = aura::ui::CloseIconButton::Draw("X", ImVec2(28, 32),
                theme.sidebarIconInactive, surface);
            const bool active = ImGui::IsItemActive(), hovered = ImGui::IsItemHovered();
            Check(GImGui->ColorStack.Size == colors && GImGui->StyleVarStack.Size == vars, "close icon balances styles");
            Check(ImGui::GetItemRectSize().x == 28 && ImGui::GetItemRectSize().y == 32, "close icon preserves hit area");
            Check(clicked == (frame == 4), "close icon preserves release and disabled behavior");
            if (frame >= 1 && !disabled) {
                Check(ImGui::GetWindowDrawList()->VtxBuffer.Size - verticesBefore == 4,
                      "close button draws only its glyph without a backplate or border");
                const auto ink = ImGui::ColorConvertU32ToFloat4(ImGui::GetWindowDrawList()->VtxBuffer.back().col);
                const auto actual = TransportContrast::Quantize(surface);
                const double contrast = TransportContrast::Ratio(ink, actual);
                std::printf("close theme=%d active=%d hovered=%d contrast=%.4f\n", themeId, active, hovered, contrast);
                Check(contrast >= 3.0, "close icon three-state rendered contrast");
                seenActive |= active; seenHover |= hovered && !active; seenNormal |= !hovered && !active;
                const auto color = ImGui::GetWindowDrawList()->VtxBuffer.back().col;
                if (active) activeInk = color;
                else if (hovered) hoverInk = color;
                else normalInk = color;
            }
            if (disabled) ImGui::EndDisabled();
            ImGui::PopStyleColor(3);
            ImGui::End(); ImGui::PopStyleColor(); ImGui::Render();
        }
        Check(seenNormal && seenHover && seenActive, "close icon samples all three real input states");
        Check(normalInk != hoverInk && hoverInk != activeInk && normalInk != activeInk,
              "close icon state feedback changes the glyph color");
    }
    io.ConfigInputTrickleEventQueue = trickle;
}

static void CheckCloseKeyboardInput() {
    auto& io = ImGui::GetIO();
    int clicks = 0;
    bool sawFocus = false;
    for (int frame = -2; frame < 6; ++frame) {
        io.AddMousePosEvent(-100, -100);
        if (frame == 3) io.AddKeyEvent(ImGuiKey_Space, true);
        if (frame == 4) io.AddKeyEvent(ImGuiKey_Space, false);
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(20, 20));
        ImGui::SetNextWindowSize(ImVec2(300, 160));
        ImGui::Begin("close keyboard", nullptr, ImGuiWindowFlags_NoSavedSettings);
        ImGui::SetCursorScreenPos(ImVec2(60, 70));
        if (frame == 0) ImGui::SetKeyboardFocusHere();
        const int before = ImGui::GetWindowDrawList()->VtxBuffer.Size;
        if (aura::ui::CloseIconButton::Draw(ICON_LC_X, ImVec2(34, 20),
                ImVec4(.65f, .65f, .65f, 1), ImVec4(.1f, .1f, .1f, 1))) ++clicks;
        if (frame >= 1) {
            sawFocus |= ImGui::IsItemFocused() && ImGui::IsItemHovered();
            Check(ImGui::GetWindowDrawList()->VtxBuffer.Size - before == 4,
                  "keyboard focus also draws only the close glyph");
        }
        ImGui::End();
        ImGui::Render();
    }
    Check(sawFocus && clicks == 1, "frameless close preserves keyboard focus and Space activation");
}

int main() {
    ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr; io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigNavCursorVisibleAlways = true;
    io.DisplaySize = ImVec2(1600, 1000);
    io.Fonts->AddFontDefault();
    ImFontConfig icons;
    icons.MergeMode = true; icons.PixelSnapH = true;
    icons.GlyphMinAdvanceX = 14.0f; icons.OversampleH = icons.OversampleV = 1;
    static const ImWchar iconRange[] = {ICON_MIN_LC, ICON_MAX_LC, 0};
    const auto iconPath = std::filesystem::path(__FILE__).parent_path().parent_path() /
        "thirdparty/lucide/lucide.ttf";
    Check(io.Fonts->AddFontFromFileTTF(iconPath.string().c_str(), 14.0f, &icons, iconRange) != nullptr,
          "load shipped Lucide font");
    unsigned char* pixels; int w, h;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);
    CheckCloseGlyphCenter();
    CheckCloseIconStates();
    CheckCloseKeyboardInput();
    const auto theme = MakeBuiltinScheme(static_cast<AppTheme>(3));
    const auto palette = Style::Make(theme, theme.dialogPanelBG, ButtonVariant::Secondary);
    for (int id : {0, 1, 2, 3, 10}) {
        const auto t = MakeBuiltinScheme(static_cast<AppTheme>(id));
        for (auto surface : {t.dialogPanelBG, t.mainWindowBG, t.filterBrowserBG}) {
            const auto normal = Style::Make(t, surface, ButtonVariant::Secondary);
            const auto selected = Style::Make(t, surface, ButtonVariant::Selected);
            Check(!Same(normal.fill, selected.fill) && !Same(normal.hover, selected.hover) &&
                  !Same(normal.active, selected.active), "editing selection remains distinct in every state");
            for (auto role : {ButtonVariant::PrimarySearch, ButtonVariant::PrimaryTransfer,
                              ButtonVariant::Secondary, ButtonVariant::Quiet, ButtonVariant::Danger,
                              ButtonVariant::Selected}) {
                const auto p = Style::Make(t, surface, role);
                for (auto bg : {p.fill, p.hover, p.active}) {
                    const double ratio = TransportContrast::Ratio(TransportContrast::Quantize(p.text),
                                                                   TransportContrast::Quantize(bg));
                    if (ratio < 4.5) std::printf("theme %d role %d ratio %.3f\n", id, int(role), ratio);
                    Check(ratio >= 4.5, "five-theme text contrast");
                }
            }
        }
    }
    ImGuiID stableId = 0;
    float normalNextY = 0;
    int keyboardClicks = 0;
    // Real ImGui press/release frames, then disabled and loading frames.
    // ImGui needs two layout frames before it accepts input for a new window.
    for (int frame = -2; frame < 11; ++frame) {
        io.AddMousePosEvent(frame < 7 ? 90.0f : -100.0f, frame < 7 ? 90.0f : -100.0f);
        io.AddMouseButtonEvent(0, frame == 1 || frame == 3 || frame == 5);
        if (frame == 8) io.AddKeyEvent(ImGuiKey_Space, true);
        if (frame == 9) io.AddKeyEvent(ImGuiKey_Space, false);
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(20, 20));
        ImGui::SetNextWindowSize(ImVec2(500, 500));
        ImGui::Begin("buttons", nullptr, ImGuiWindowFlags_NoSavedSettings);
        ImGui::SetCursorScreenPos(ImVec2(60, 70));
        ImGui::PushStyleColor(ImGuiCol_BorderShadow, ImVec4(.8f, .2f, .1f, 1));
        const ImGuiStyle before = ImGui::GetStyle();
        const int colorsBefore = GImGui->ColorStack.Size, varsBefore = GImGui->StyleVarStack.Size;
        const bool disabled = frame == 3 || frame == 4;
        const bool loading = frame == 5 || frame == 6;
        if (disabled) ImGui::BeginDisabled();
        bool clicked;
        {
            Style::Scope scope(palette);
            Check(ImGui::GetStyle().Colors[ImGuiCol_BorderShadow].w == 0, "no inset shadow");
            if (frame == 7) ImGui::SetKeyboardFocusHere();
            clicked = Style::Draw("Transfer##stable", ImVec2(180, 32), loading);
            const auto size = ImGui::GetItemRectSize();
            Check(size.x == 180 && size.y == 32, "fixed hit rectangle");
            if (frame == -2) { stableId = ImGui::GetItemID(); normalNextY = ImGui::GetCursorScreenPos().y; }
            Check(stableId == ImGui::GetItemID(), "stable loading ID");
            Check(ImGui::GetCursorScreenPos().y == normalNextY, "loading leaves cursor layout intact");
            if (loading) Check((GImGui->LastItemData.ItemFlags & ImGuiItemFlags_Disabled) != 0, "loading disabled");
        }
        if (disabled) ImGui::EndDisabled();
        if (frame >= 7 && clicked) ++keyboardClicks;
        if (frame < 7 && clicked != (frame == 2))
            std::printf("frame=%d clicked=%d mouse=(%.1f,%.1f) down=%d hovered=%d active=%u stable=%u\n",
                        frame, clicked, io.MousePos.x, io.MousePos.y, io.MouseDown[0],
                        ImGui::IsItemHovered(), GImGui->ActiveId, stableId);
        if (frame < 7) Check(clicked == (frame == 2), "click release and disabled/loading suppression");
        Check(GImGui->ColorStack.Size == colorsBefore && GImGui->StyleVarStack.Size == varsBefore, "balanced styles");
        Check(Same(before.Colors[ImGuiCol_BorderShadow], ImGui::GetStyle().Colors[ImGuiCol_BorderShadow]), "restore parent shadow");
        Check(before.FrameRounding == ImGui::GetStyle().FrameRounding &&
              before.FrameBorderSize == ImGui::GetStyle().FrameBorderSize, "leave input shape unchanged");
        ImGui::PopStyleColor();
        {
            Style::Scope scope(palette);
            const float available = ImGui::GetContentRegionAvail().x;
            Style::Draw("Fill", ImVec2(-1, 0));
            Check(std::fabs(ImGui::GetItemRectSize().x - (available - 1)) < .1f, "preserve fill width");
            Style::Draw("Automatic width", ImVec2(0, 0));
            Check(ImGui::GetItemRectSize().x >= ImGui::CalcTextSize("Automatic width").x, "automatic label width");
        }
        ImGui::End(); ImGui::Render();
    }
    Check(keyboardClicks == 1, "keyboard focus and Space activation");
    ImGui::DestroyContext();
    std::printf("action button failures: %d\n", failures);
    return failures ? 1 : 0;
}
