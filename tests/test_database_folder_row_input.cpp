#include "DatabaseFolderRowInput.h"
#include <cstdio>

int main() {
    ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(640, 480);
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    unsigned char* pixels;
    int width, height;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    ImVec2 origin;
    bool open = false, leaf = false;
    int selections = 0;
    auto frame = [&] {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(10, 10));
        ImGui::SetNextWindowSize(ImVec2(400, 200));
        ImGui::Begin("Tree");
        auto flags = ImGuiTreeNodeFlags_FramePadding | ImGuiTreeNodeFlags_OpenOnArrow |
                     ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_NoTreePushOnOpen;
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10, 6));
        open = DatabaseFolderRowInput::TreeNode(leaf ? "Leaf" : "Branch", flags | (leaf ? ImGuiTreeNodeFlags_Leaf : 0), 17, 6);
        selections += DatabaseFolderRowInput::SelectClicked(!leaf, 17);
        ImGui::PopStyleVar();
        origin = ImGui::GetItemRectMin();
        ImGui::End();
        ImGui::Render();
    };
    auto click = [&](float offset) {
        io.AddMousePosEvent(origin.x + offset, origin.y + 10);
        frame();
        io.AddMouseButtonEvent(0, true);
        frame();
        io.AddMouseButtonEvent(0, false);
        frame();
    };
    auto key = [&](ImGuiKey code) {
        io.AddKeyEvent(code, true);
        frame();
        io.AddKeyEvent(code, false);
        frame();
    };
    int failures = 0;
    auto check = [&](bool condition, const char* name) {
        if (!condition) { std::fprintf(stderr, "%s: open=%d selections=%d\n", name, open, selections); ++failures; }
    };
    frame(); frame();
    click(20); check(!open && selections == 1, "icon inside former native arrow");
    click(17); check(!open && selections == 2, "exact boundary selects");
    click(16); check(open && selections == 2, "last chevron pixel opens");
    click(100); check(open && selections == 3, "body preserves expanded state");
    click(2); check(!open && selections == 3, "chevron collapses");
    key(ImGuiKey_RightArrow); check(open && selections == 3, "keyboard right");
    key(ImGuiKey_LeftArrow); check(!open && selections == 3, "keyboard left");
    leaf = true;
    frame(); click(2); check(selections == 4, "leaf has no chevron");
    ImGui::DestroyContext();
    return failures != 0;
}
