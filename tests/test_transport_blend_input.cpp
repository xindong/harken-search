#include "ui/TransportBlendSlider.h"
#include "ui/TransportVisualAudit.h"
#include <cmath>
#include <cstdio>

namespace {
constexpr ImU32 backing = IM_COL32(239, 1, 254, 255);
constexpr ImU32 ink = IM_COL32(1, 254, 23, 255);
float percent = 42;
ImVec2 center;
bool editing = false, overlay = false;
int borderVertices = 0;
constexpr ImU32 borderInk = IM_COL32(23, 57, 191, 255);
int failures = 0;
void Check(bool good, const char* label) {
    std::printf("%s %s\n", good ? "PASS" : "FAIL", label);
    failures += !good;
}
void Frame() {
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(20, 20));
    ImGui::SetNextWindowSize(ImVec2(400, 120));
    ImGui::Begin("Transport input", nullptr, ImGuiWindowFlags_NoSavedSettings);
    ImGui::SetNextItemWidth(240);
    const int start = ImGui::GetWindowDrawList()->VtxBuffer.Size;
    TransportBlendSlider::Draw("##blend", &percent, ink, backing);
    const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    center = ImVec2((a.x + b.x) / 2, (a.y + b.y) / 2);
    editing = ImGui::TempInputIsActive(ImGui::GetItemID());
    overlay = false;
    borderVertices = 0;
    const auto& vertices = ImGui::GetWindowDrawList()->VtxBuffer;
    for (int i = start; i < vertices.Size; ++i) {
        overlay |= vertices[i].col == backing; // Native editor text may use the readable ink.
        borderVertices += vertices[i].col == borderInk;
    }
    ImGui::End();
    ImGui::Render();
}
}
int main() {
    ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(800, 600);
    io.DeltaTime = 1.f / 60;
    unsigned char* pixels; int width, height;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    ImGui::GetStyle().FrameBorderSize = 1;
    ImGui::GetStyle().Colors[ImGuiCol_Border] = ImGui::ColorConvertU32ToFloat4(borderInk);
    Frame(); Frame();
    Check(borderVertices > 0, "original textured slider border remains visible");
    Check(!editing && !overlay, "ordinary value has no added text backing");
    io.AddMousePosEvent(center.x, center.y); Frame();
    io.AddKeyEvent(ImGuiMod_Ctrl, true);
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, true); Frame();
    Check(editing, "Ctrl+click enters the real ImGui number editor");
    Check(!overlay, "number editor is not painted over by the display label");
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    io.AddKeyEvent(ImGuiMod_Ctrl, false); Frame();
    io.AddInputCharactersUTF8("75"); Frame();
    Check(!overlay, "typed digits stay unobscured");
    io.AddKeyEvent(ImGuiKey_Enter, true); Frame();
    io.AddKeyEvent(ImGuiKey_Enter, false); Frame(); Frame();
    Check(std::fabs(percent - 75.f) < .001f, "typing 75 and Enter commits 75 percent");
    Check(!editing && !overlay, "unboxed display returns after editing");
    ImGui::NewFrame();
    ImGui::Begin("Unboxed value", nullptr, ImGuiWindowFlags_NoSavedSettings);
    auto* draw=ImGui::GetWindowDrawList();
    const auto position=ImGui::GetCursorScreenPos();
    const int first=draw->VtxBuffer.Size;
    const int commandsBefore=draw->CmdBuffer.Size;
    TransportVisualAudit::Request();
    TransportVisualAudit::Begin(0,position,ImVec2(position.x+200,position.y+100),IM_COL32_WHITE);
    TransportValueText::Draw(draw,position,"107%",ImVec4(.4f,.4f,.4f,1),
        [&](float x) { return x<position.x+12 ? ImVec4(.95f,.95f,.95f,1) : ImVec4(.02f,.2f,.2f,1); },"volume.label");
    Check(draw->CmdBuffer.Size <= commandsBefore + 1,
        "adaptive percentage glyphs share a draw command across fill colors");
    const auto white=ImGui::GetFontTexUvWhitePixel();
    bool onlyGlyphs=draw->VtxBuffer.Size>first;
    for(int i=first;i<draw->VtxBuffer.Size;++i)
        onlyGlyphs &= draw->VtxBuffer[i].uv.x!=white.x || draw->VtxBuffer[i].uv.y!=white.y;
    Check(onlyGlyphs,"volume value emits only glyphs, no filled plate or border");
    bool readable=!TransportVisualAudit::checks.empty();
    for(const auto& c:TransportVisualAudit::checks)
        readable &= TransportContrast::Ratio(ImGui::ColorConvertU32ToFloat4(c.foreground),
            ImGui::ColorConvertU32ToFloat4(c.background))>=4.5;
    Check(readable,"unboxed value stays readable across a light/dark fill boundary");
    ImGui::End();ImGui::Render();
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(20, 20));
    ImGui::SetNextWindowSize(ImVec2(400, 120));
    ImGui::Begin("Glyph audit", nullptr, ImGuiWindowFlags_NoSavedSettings);
    TransportVisualAudit::Request();
    TransportVisualAudit::Begin(0, ImVec2(20, 20), ImVec2(420, 140), IM_COL32_BLACK);
    ImGui::GetWindowDrawList()->PushClipRect(ImVec2(60, 50), ImVec2(100, 80), true);
    ImGui::GetWindowDrawList()->AddText(ImGui::GetFont(), 20, ImVec2(40, 55), ink, "OVERFLOW");
    TransportVisualAudit::RecordGlyphs("test", ink, IM_COL32_BLACK, ImVec2(40, 55), "OVERFLOW", ImGui::GetFont(), 20);
    ImGui::GetWindowDrawList()->PopClipRect();
    const auto& measured = TransportVisualAudit::checks.at(0);
    Check(measured.glyphBounds && measured.min.x < 60 && measured.max.x > 100,
        "audit preserves uncut glyph bounds from the actual font");
    Check(measured.clipMin.x == 60 && measured.clipMax.x == 100,
        "audit preserves the active inner clip after it is popped");
    ImGui::End(); ImGui::Render();
    ImGui::DestroyContext();
    return failures ? 1 : 0;
}
