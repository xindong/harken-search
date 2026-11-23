#include "TaskProgressToastRender.h"

#include "imgui.h"

#include <cstdio>
#include <string>

namespace {

int g_failures = 0;

void Check(bool condition, const char* what) {
    if (condition) return;
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++g_failures;
}

void BeginFrame(ImVec2 mouse, bool down) {
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(1280.0f, 800.0f);
    io.DeltaTime = 1.0f / 60.0f;
    io.MousePos = mouse;
    io.MouseDown[0] = down;
    ImGui::NewFrame();
}

TaskProgressToastPalette MakePalette() {
    TaskProgressToastPalette palette;
    palette.panelBg = IM_COL32(24, 26, 32, 235);
    palette.border = IM_COL32(70, 74, 84, 255);
    palette.title = IM_COL32(240, 242, 248, 255);
    palette.phase = IM_COL32(160, 166, 178, 255);
    palette.track = IM_COL32(60, 64, 74, 255);
    palette.fill = IM_COL32(120, 208, 255, 255);
    palette.success = IM_COL32(120, 220, 160, 255);
    palette.failed = IM_COL32(240, 120, 120, 255);
    palette.hover = IM_COL32(255, 255, 255, 255);
    palette.closeGlyph = IM_COL32(230, 232, 238, 255);
    return palette;
}

void TestDrawsRows() {
    TaskProgressToastStack stack;
    stack.Report("scan", "Scan", "Indexing", 0.42f, false, 0.0);
    stack.Report("wave", "Waveform", "Rebuilding", -1.0f, false, 0.0);

    BeginFrame(ImVec2(-100.0f, -100.0f), false);
    ImDrawList* drawList = ImGui::GetBackgroundDrawList();
    const int before = drawList->VtxBuffer.Size;
    const auto result = RenderTaskProgressToast(
        drawList, stack, MakePalette(), ImVec2(0.0f, 0.0f), ImVec2(1280.0f, 800.0f),
        0.5, true);
    const int painted = drawList->VtxBuffer.Size - before;
    ImGui::EndFrame();

    Check(painted > 0, "overlay paints vertices for visible rows");
    Check(result.dismissedId.empty() && result.activatedId.empty(),
          "no click without input");
    Check(!result.hovered, "pointer outside the panel does not hover");
}

void TestNoRowsDrawsNothing() {
    TaskProgressToastStack stack;
    BeginFrame(ImVec2(-100.0f, -100.0f), false);
    ImDrawList* drawList = ImGui::GetBackgroundDrawList();
    const int before = drawList->VtxBuffer.Size;
    RenderTaskProgressToast(drawList, stack, MakePalette(), ImVec2(0.0f, 0.0f),
                            ImVec2(1280.0f, 800.0f), 0.0, true);
    const int painted = drawList->VtxBuffer.Size - before;
    ImGui::EndFrame();
    Check(painted == 0, "empty stack paints nothing");
}

void TestCloseClickDismisses() {
    TaskProgressToastStack stack;
    stack.Report("scan", "Scan", "Indexing", 0.42f, false, 0.0);
    const auto panel =
        stack.PanelRect(0.0f, 0.0f, 1280.0f, 800.0f, 1);
    const auto row = stack.RowRect(panel, 0);
    const auto close = ToastCloseButtonRect(stack, row);
    const ImVec2 target(close.x + 4.0f, close.y + 4.0f);

    BeginFrame(ImVec2(-100.0f, -100.0f), false);
    ImGui::EndFrame();
    BeginFrame(target, true);
    ImDrawList* drawList = ImGui::GetBackgroundDrawList();
    const auto result = RenderTaskProgressToast(
        drawList, stack, MakePalette(), ImVec2(0.0f, 0.0f), ImVec2(1280.0f, 800.0f),
        0.0, true);
    ImGui::EndFrame();

    Check(result.hovered, "hovering a row is reported");
    Check(result.dismissedId == "scan", "clicking the close target dismisses the row");
}

void TestInertWhenInputDisabled() {
    TaskProgressToastStack stack;
    stack.Report("scan", "Scan", "Indexing", 0.42f, false, 0.0);
    const auto panel = stack.PanelRect(0.0f, 0.0f, 1280.0f, 800.0f, 1);
    const auto row = stack.RowRect(panel, 0);
    const ImVec2 target(row.x + 20.0f, row.y + 20.0f);

    BeginFrame(ImVec2(-100.0f, -100.0f), false);
    ImGui::EndFrame();
    BeginFrame(target, true);
    ImDrawList* drawList = ImGui::GetBackgroundDrawList();
    const int before = drawList->VtxBuffer.Size;
    const auto result = RenderTaskProgressToast(
        drawList, stack, MakePalette(), ImVec2(0.0f, 0.0f), ImVec2(1280.0f, 800.0f),
        0.0, false);
    const int painted = drawList->VtxBuffer.Size - before;
    ImGui::EndFrame();

    Check(painted > 0, "overlay stays visible while a modal blocks input");
    Check(!result.hovered && result.activatedId.empty(), "blocked overlay ignores clicks");
}

} // namespace

int main() {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(1280.0f, 800.0f);
    unsigned char* pixels = nullptr;
    int width = 0;
    int height = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    io.Fonts->SetTexID(static_cast<ImTextureID>(1));

    TestDrawsRows();
    TestNoRowsDrawsNothing();
    TestCloseClickDismisses();
    TestInertWhenInputDisabled();

    ImGui::DestroyContext();
    if (g_failures != 0) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("TaskProgressToast render checks passed\n");
    return 0;
}