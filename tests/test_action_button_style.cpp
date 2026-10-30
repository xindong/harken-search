#include "AppState.h"
#include "BuiltinThemeScheme.h"
#include "ui/ActionButtonStyle.h"
#include "imgui_internal.h"
#include <cmath>
#include <cstdio>

using aura::ui::ButtonVariant;
namespace Style = aura::ui::ActionButtonStyle;
static int failures = 0;
static void Check(bool ok, const char* label) {
    if (!ok) { ++failures; std::printf("FAIL %s\n", label); }
}
static bool Same(ImVec4 a, ImVec4 b) {
    return a.x == b.x && a.y == b.y && a.z == b.z && a.w == b.w;
}

int main() {
    ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr; io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigNavCursorVisibleAlways = true;
    io.DisplaySize = ImVec2(1600, 1000);
    unsigned char* pixels; int w, h;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);
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
