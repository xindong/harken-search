#include "FolderTreeControls.h"
#include "HarkenSearchIcons.h"
#include "../thirdparty/lucide/IconsLucide.h"

#include <cmath>
#include <cstdio>

namespace controls = aura::folder_tree::controls;

static int failures = 0;

static void Check(bool condition, const char* name) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", name);
        ++failures;
    }
}

static bool Near(float actual, float expected) {
    return std::abs(actual - expected) < 0.001f;
}

static void TestLocatePathMatching() {
    Check(controls::ContainsPath("d:\\", "d:\\library\\effects"), "Windows drive root contains its folders");
    Check(controls::ContainsPath("/", "/library/effects"), "POSIX root contains its folders");
    Check(controls::ContainsPath("d:\\library", "d:\\library"), "folder matches its own row");
    Check(controls::ContainsPath("d:\\library", "d:\\library\\effects"), "folder contains a real descendant");
    Check(!controls::ContainsPath("d:\\library", "d:\\library-old"), "shared name prefix does not open a sibling");
}

static void TestRefreshMotion() {
    controls::RefreshMotion refresh;
    Check(refresh.Update(0.0, false, false) == 0.0f, "refresh does not spin on first render");
    Check(refresh.Update(4.0, true, false) == 0.0f, "click starts upright");
    Check(Near(refresh.Update(4.105, false, false), 0.25f), "refresh turns clockwise at a constant rate");
    Check(Near(refresh.Update(4.315, false, false), 0.75f), "click spins through the third quarter");
    Check(refresh.Update(4.421, false, false) == 0.0f, "single click completes one turn in 420 ms");
    Check(refresh.Update(8.0, false, true) == 0.0f, "real busy edge starts upright");
    Check(Near(refresh.Update(8.525, false, true), 0.25f), "busy refresh continues after one turn");
    Check(Near(refresh.Update(8.630, true, false), 0.50f), "busy exit and another click do not jump backwards");
    Check(Near(refresh.Update(8.735, false, false), 0.75f), "busy exit finishes the current turn");
    Check(refresh.Update(8.841, false, false) == 0.0f, "busy exit stops upright");
}

static void TestLocateFeedback() {
    controls::Interaction interaction;
    interaction.BeginFrame(1, 27, "library-a", 11, -1, false);
    auto& locate = interaction.locate;
    locate.MarkScrolled("search-hit", 1.0);
    Check(locate.Strength("search-hit", 1.0) == 0.0f, "search scroll never starts a locate flash");
    locate.Request("selected-folder");
    Check(locate.Strength("selected-folder", 1.0) == 0.0f, "request waits for the target row to scroll");
    locate.MarkScrolled("other-folder", 1.0);
    Check(locate.Strength("other-folder", 1.0) == 0.0f, "different row cannot claim the locate flash");
    locate.MarkScrolled("selected-folder", 1.1);
    locate.EndFrame(1.1);
    interaction.BeginFrame(2, 27, "library-a", 11, -1, false);
    Check(Near(locate.Strength("selected-folder", 1.1), 1.0f), "scrolled row starts with the strongest flash");
    Check(locate.Strength("other-folder", 1.2) == 0.0f, "flash stays on exactly one row");
    Check(locate.Strength("selected-folder", 1.3) > locate.Strength("selected-folder", 1.7), "row flash fades out");
    Check(locate.Strength("selected-folder", 1.901) == 0.0f, "row flash ends in about 800 ms");
    interaction.BeginFrame(3, 27, "library-b", 11, -1, false);
    Check(locate.Strength("selected-folder", 1.4) == 0.0f, "same folder in another library is not flashed");

    locate.Request("unreached");
    locate.EndFrame(1.5);
    locate.MarkScrolled("unreached", 1.6);
    Check(locate.Strength("unreached", 1.6) == 0.0f, "unreached request cannot flash on a later search scroll");
    locate.Request("shared");
    locate.MarkScrolled("shared", 1.7);
    interaction.BeginFrame(4, 27, "library-b", 11, 20, false);
    Check(locate.Strength("shared", 1.8) == 0.0f, "switching to a playlist ends the old flash");
    locate.Request("shared");
    locate.MarkScrolled("shared", 1.9);
    interaction.BeginFrame(5, 27, "library-b", 11, -1, true);
    Check(locate.Strength("shared", 2.0) == 0.0f, "switching to the browser ends the old flash");
    locate.Request("shared");
    locate.MarkScrolled("shared", 2.1);
    interaction.BeginFrame(7, 27, "library-b", 11, -1, true);
    Check(locate.Strength("shared", 2.2) == 0.0f, "reopening the toolbar does not replay the old flash");
}

struct ButtonDraw {
    ImVec2 min;
    ImVec2 max;
    ImVec2 inkCenter;
    ImVec2 inkCorner;
    ImU32 inkColor = 0;
    ImU32 backgroundColor = 0;
    int vertices = 0;
    bool clicked = false;
    bool helpHovered = false;
};

static void TestButtons(bool loadLucide) {
    ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(480, 320);
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    io.Fonts->AddFontDefault();
    if (loadLucide) {
        aura::ui::MergeLucideFont(io.Fonts, 20.0f);
    }
    const ImVec4 icon(0.68f, 0.74f, 0.80f, 1.0f);
    const ImVec4 fill(0.40f, 0.55f, 0.70f, 0.24f);
    controls::RefreshMotion motion;
    ButtonDraw refresh, locate;
    auto draw = [&](ButtonDraw& button, const char* id, const char* glyph,
                    bool enabled, controls::RefreshMotion* refreshMotion = nullptr) {
        auto* list = ImGui::GetWindowDrawList();
        const int first = list->VtxBuffer.Size;
        button.clicked = controls::DrawButton(id, glyph, enabled, icon, fill, refreshMotion);
        button.helpHovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled);
        button.min = ImGui::GetItemRectMin();
        button.max = ImGui::GetItemRectMax();
        button.vertices = list->VtxBuffer.Size - first;
        if (button.vertices >= 4) {
            const auto& cornerA = list->VtxBuffer[list->VtxBuffer.Size - 4];
            const auto& cornerB = list->VtxBuffer[list->VtxBuffer.Size - 2];
            button.inkCorner = cornerA.pos;
            button.inkCenter = ImVec2((cornerA.pos.x + cornerB.pos.x) * 0.5f,
                                      (cornerA.pos.y + cornerB.pos.y) * 0.5f);
            button.inkColor = cornerA.col;
            button.backgroundColor = button.vertices > 4 ? list->VtxBuffer[first].col : 0;
        }
    };
    auto frame = [&](bool canLocate = true) {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(10, 10));
        ImGui::SetNextWindowSize(ImVec2(320, 150));
        ImGui::Begin("Folder tree controls");
        draw(refresh, "##refresh", ICON_LC_REFRESH, true, &motion);
        ImGui::SameLine();
        draw(locate, "##locate", ICON_LC_FOLDER_OPEN, canLocate);
        ImGui::End();
        ImGui::Render();
    };
    frame();
    frame();
    if (!loadLucide) {
        Check(refresh.vertices > 4, "refresh draws a visible shape without Lucide");
        Check(locate.vertices > 4, "locate draws a visible shape without Lucide");
        Check(Near(refresh.max.x - refresh.min.x, locate.max.x - locate.min.x),
              "fallback icons preserve the shared hit area");
        frame(false);
        Check(locate.vertices > 4, "disabled locate remains visible without Lucide");
        ImGui::DestroyContext();
        return;
    }
    Check(refresh.vertices == 4 && locate.vertices == 4, "idle buttons draw only glyphs and no background or border");
    Check(Near(refresh.max.x - refresh.min.x, locate.max.x - locate.min.x) &&
          Near(refresh.max.y - refresh.min.y, locate.max.y - locate.min.y) &&
          Near(refresh.min.y, locate.min.y), "two buttons have the same aligned hit area");
    Check(refresh.inkColor == locate.inkColor, "both enabled glyphs have the same brightness");
    Check(Near(refresh.inkCenter.x, (refresh.min.x + refresh.max.x) * 0.5f) &&
          Near(locate.inkCenter.x, (locate.min.x + locate.max.x) * 0.5f) &&
          Near(refresh.inkCenter.y, locate.inkCenter.y), "glyph bounds are optically centered");

    const ImVec2 refreshOrigin = refresh.min;
    const ImVec2 uprightCorner = refresh.inkCorner;
    io.AddMousePosEvent(refresh.inkCenter.x, refresh.inkCenter.y);
    frame();
    const ImU32 hover = refresh.backgroundColor;
    Check(refresh.vertices > 4 && locate.vertices == 4 && hover != 0, "only the hovered button draws a background");
    io.AddMouseButtonEvent(0, true);
    frame();
    Check(refresh.backgroundColor == hover && Near(refresh.min.x, refreshOrigin.x) &&
          Near(refresh.min.y, refreshOrigin.y), "press keeps the same background and hit area as hover");
    Check(Near(refresh.inkCorner.x, uprightCorner.x), "press does not pre-spin the glyph");
    io.AddMouseButtonEvent(0, false);
    frame();
    Check(refresh.clicked, "enabled refresh can be clicked");
    io.DeltaTime = 0.105f;
    frame();
    Check(refresh.inkCorner.x > refresh.inkCenter.x && refresh.inkCorner.y < refresh.inkCenter.y,
          "refresh glyph rotates clockwise about the same center");
    io.DeltaTime = 0.316f;
    frame();
    Check(Near(refresh.inkCorner.x, uprightCorner.x) && Near(refresh.inkCorner.y, uprightCorner.y),
          "refresh glyph settles exactly upright");
    io.DeltaTime = 1.0f / 60.0f;
    io.AddMousePosEvent(locate.inkCenter.x, locate.inkCenter.y);
    frame();
    Check(locate.backgroundColor == hover, "both buttons use the same hover fill");
    frame(false);
    Check(locate.vertices == 4 && locate.inkColor != refresh.inkColor, "disabled locate is dimmed and has no hover fill");
    Check(locate.helpHovered, "disabled locate still supports its help tooltip");
    io.AddMouseButtonEvent(0, true);
    frame(false);
    io.AddMouseButtonEvent(0, false);
    frame(false);
    Check(!locate.clicked, "disabled locate cannot fire");
    ImGui::DestroyContext();
}

int main() {
    TestRefreshMotion();
    TestLocatePathMatching();
    TestLocateFeedback();
    TestButtons(true);
    TestButtons(false);
    return failures != 0;
}
