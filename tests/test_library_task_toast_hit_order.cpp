// Headless regression harness for the task-toast failure row:
// the 16x16 close button sits fully inside the 20px-tall full-row item, so the
// submission order decides which item owns ImGui's single HoveredId.
#include "imgui.h"

#include <cstdio>

namespace {

constexpr bool kTrace = false;

struct ScenarioResult {
    bool rowHovered = false;
    bool closeHovered = false;
    bool closeClicked = false;
};

ScenarioResult RunScenario(bool closeFirst) {
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.DisplaySize = ImVec2(480.0f, 240.0f);
    unsigned char* pixels = nullptr;
    int atlasWidth = 0;
    int atlasHeight = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &atlasWidth, &atlasHeight);

    const ImVec2 windowPos(60.0f, 60.0f);
    const float rowWidth = 360.0f;
    ScenarioResult result;
    const ImVec2 closeTopLeft(windowPos.x + rowWidth - 16.0f, windowPos.y + 2.0f);
    const ImVec2 closeCenter(closeTopLeft.x + 8.0f, closeTopLeft.y + 8.0f);

    auto frame = [&](bool mouseDown) {
        io.AddMousePosEvent(closeCenter.x, closeCenter.y);
        io.AddMouseButtonEvent(0, mouseDown);
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(windowPos);
        ImGui::SetNextWindowSize(ImVec2(400.0f, 120.0f));
        ImGui::Begin("task toast fixture", nullptr,
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
        ImGui::SetCursorScreenPos(windowPos);

        auto submitClose = [&] {
            ImGui::SetCursorScreenPos(closeTopLeft);
            result.closeClicked =
                ImGui::InvisibleButton("##close", ImVec2(16.0f, 16.0f)) || result.closeClicked;
            result.closeHovered = ImGui::IsItemHovered();
            if (kTrace) std::printf("  close hover=%d\n", result.closeHovered ? 1 : 0);
        };
        auto submitRow = [&] {
            ImGui::SetCursorScreenPos(windowPos);
            ImGui::InvisibleButton("##row", ImVec2(rowWidth, 20.0f));
            result.rowHovered = ImGui::IsItemHovered();
            if (kTrace) std::printf("  row hover=%d\n", result.rowHovered ? 1 : 0);
        };

        if (closeFirst) {
            submitClose();
            submitRow();
        } else {
            submitRow();
            submitClose();
        }
        ImGui::End();
        ImGui::Render();
    };

    frame(false);  // settle the window so hover is not blocked by "appearing" state
    frame(false);
    frame(true);   // press on the close rect
    frame(false);  // release -> a reachable close reports a click
    frame(false);

    ImGui::DestroyContext();
    return result;
}

int failures = 0;

void Check(bool condition, const char* message) {
    if (condition) return;
    std::fprintf(stderr, "%s\n", message);
    ++failures;
}

}  // namespace

int main() {
    const ScenarioResult closeFirst = RunScenario(true);
    const ScenarioResult rowFirst = RunScenario(false);

    // The fix: the close button is submitted before the full-row item.
    Check(closeFirst.closeHovered, "close submitted before the row was not hovered");
    Check(closeFirst.closeClicked, "close submitted before the row did not receive the click");

    // The bug being fixed: the row grabbed HoveredId first, so the close was dead.
    Check(!rowFirst.closeClicked, "row-first fixture unexpectedly clicked the close");

    std::printf("close-first: hovered=%d clicked=%d | row-first: hovered=%d clicked=%d\n",
                closeFirst.closeHovered, closeFirst.closeClicked,
                rowFirst.closeHovered, rowFirst.closeClicked);
    return failures == 0 ? 0 : 1;
}
