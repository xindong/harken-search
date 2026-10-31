#include "FileInfoPanel.h"
#include "imgui_internal.h"
#include <iostream>
int main() {
    ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(640,480); io.IniFilename = nullptr;
    unsigned char* pixels; int width,height;
    io.Fonts->GetTexDataAsRGBA32(&pixels,&width,&height);
    ImGui::NewFrame();
    ImGui::SetNextWindowSize(ImVec2(400,300));
    ImGui::Begin("Info footer regression");
    const ImVec2 strip = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(200,18));
    FinishFileInfoArtworkFooter(strip,26);
    const auto* window = ImGui::GetCurrentWindowRead();
    const bool reserved = window->DC.CursorMaxPos.y >= strip.y+26;
    // Keep the failed test from opening a platform assertion dialog.
    ImGui::Dummy(ImVec2(0,0));
    ImGui::End(); ImGui::EndFrame();
    bool fits = true;
    for (float spacing : {4.0f, 6.0f, 12.0f}) {
        ImGui::GetStyle().ItemSpacing.y = spacing;
        for (bool tallContent : {false, true}) {
        for (int frame = 0; frame < 3; ++frame) {
            ImGui::NewFrame();
            ImGui::SetNextWindowSize(ImVec2(360,600));
            ImGui::Begin("Artwork fits its host");
            const float contentHeight = FileInfoArtworkContentHeight(ImGui::GetContentRegionAvail().y,22);
            ImGui::BeginChild("Artwork",ImVec2(0,contentHeight),true);
            ImGui::TextUnformatted("No artwork");
            if (tallContent) ImGui::Dummy(ImVec2(100,1000));
            if (frame == 2) {
                const auto* content = ImGui::GetCurrentWindowRead();
                fits &= tallContent ? content->ScrollMax.y > 0 : content->ScrollMax.y == 0;
            }
            ImGui::EndChild();
            const ImVec2 footer = ImGui::GetCursorScreenPos();
            ImGui::SetCursorScreenPos(ImVec2(footer.x+8,footer.y+4));
            ImGui::InvisibleButton("Thumbnail size",ImVec2(120,14));
            FinishFileInfoArtworkFooter(footer,22);
            if (frame == 2) {
                const auto* host = ImGui::GetCurrentWindowRead();
                const bool noOuterScroll = !host->ScrollbarY && host->ScrollMax.y == 0;
                fits &= noOuterScroll;
                std::cout << (noOuterScroll ? "PASS" : "FAIL") << ": artwork host, spacing=" << spacing
                          << " scrollMax=" << host->ScrollMax.y << "\n";
            }
            ImGui::End(); ImGui::EndFrame();
        }
        }
    }
    ImGui::DestroyContext();
    std::cout << (reserved ? "PASS" : "FAIL") << ": footer space belongs to the parent layout\n";
    return reserved && fits ? 0 : 1;
}
