// Headless regression check for the task-toast failure row.
//
// The 16x16 dismiss button and the full-row item used to share the same rect,
// and ImGui only gives one item the hover for a frame. That made the close
// button unreachable and let the row tooltip replace the dismiss tooltip.
// The row item is now 18px narrower on failed rows, so the two rects never
// overlap; these cases lock that geometry down.
#include "imgui.h"

#include <cstdio>

namespace {

enum class Hit {
    None,
    Close,
    Row,
};

struct ScenarioResult {
    bool rowHovered = false;
    bool closeHovered = false;
    bool closeClicked = false;
    bool rowClicked = false;
};

constexpr float kRowWidth = 360.0f;
constexpr float kCloseInset = 18.0f;

ScenarioResult RunScenario(bool failed, ImVec2 mouse, bool pressAndRelease) {
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
    const ImVec2 closeTopLeft(windowPos.x + kRowWidth - 16.0f, windowPos.y + 2.0f);
    ScenarioResult result;

    auto frame = [&](bool mouseDown) {
        io.AddMousePosEvent(mouse.x, mouse.y);
        io.AddMouseButtonEvent(0, mouseDown);
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(windowPos);
        ImGui::SetNextWindowSize(ImVec2(420.0f, 120.0f));
        ImGui::Begin("task toast fixture", nullptr,
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
        ImGui::SetCursorScreenPos(windowPos);

        if (failed) {
            ImGui::SetCursorScreenPos(closeTopLeft);
            result.closeClicked =
                ImGui::InvisibleButton("##close", ImVec2(16.0f, 16.0f)) || result.closeClicked;
            result.closeHovered = ImGui::IsItemHovered();
        }

        ImGui::SetCursorScreenPos(windowPos);
        ImGui::InvisibleButton("##row", ImVec2(failed ? kRowWidth - kCloseInset : kRowWidth, 20.0f));
        result.rowHovered = ImGui::IsItemHovered();
        result.rowClicked = ImGui::IsItemClicked() || result.rowClicked;

        ImGui::End();
        ImGui::Render();
    };

    frame(false);  // settle the window so hover is not blocked by "appearing" state
    frame(false);
    if (pressAndRelease) {
        frame(true);
        frame(false);
    }
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
    const ImVec2 closeCenter(60.0f + kRowWidth - 8.0f, 60.0f + 10.0f);
    const ImVec2 rowCenter(60.0f + 40.0f, 60.0f + 10.0f);

    // Hovering the dismiss button: it owns the hover, the row must keep quiet
    // so the dismiss tooltip is not replaced by the row tooltip.
    const ScenarioResult onClose = RunScenario(true, closeCenter, true);
    Check(onClose.closeHovered, "the dismiss button did not take the hover");
    Check(!onClose.rowHovered, "the row still hovered under the dismiss button");
    Check(onClose.closeClicked, "clicking the dismiss button did nothing");

    // Hovering the row itself still works, and the row click is still reported.
    const ScenarioResult onRow = RunScenario(true, rowCenter, true);
    Check(onRow.rowHovered, "the row stopped hovering on failed rows");
    Check(onRow.rowClicked, "the row stopped reporting clicks on failed rows");
    Check(!onRow.closeHovered, "the dismiss button hovered while the row was hovered");

    // Rows without a dismiss button keep the full clickable width.
    const ScenarioResult plainRow = RunScenario(false, closeCenter, true);
    Check(plainRow.rowHovered, "a running row lost hover near its right edge");
    Check(plainRow.rowClicked, "a running row lost clicks near its right edge");

    std::printf("close: hover=%d clicked=%d rowHover=%d | row: hover=%d clicked=%d | "
                "plain row: hover=%d clicked=%d\n",
                onClose.closeHovered, onClose.closeClicked, onClose.rowHovered,
                onRow.rowHovered, onRow.rowClicked,
                plainRow.rowHovered, plainRow.rowClicked);
    return failures == 0 ? 0 : 1;
}
