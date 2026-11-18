#include "imgui_internal.h"

#include <cmath>
#include <cstdio>
#include <string>

static bool Check(bool condition, const char* message) {
    if (!condition) std::fprintf(stderr, "%s\n", message);
    return condition;
}

static bool ContainsColor(const ImDrawList& draw, int firstVertex, ImU32 color) {
    for (int index = firstVertex; index < draw.VtxBuffer.Size; ++index) {
        if (draw.VtxBuffer[index].col == color) return true;
    }
    return false;
}

static bool ExerciseBarRhythm(float scale) {
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(560.0f, 260.0f);
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    ImGui::GetStyle().FontScaleDpi = scale;
    const float spacing = std::floor(21.0f * scale);
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(42.0f, 12.0f));
    ImGui::SetNextWindowSize(ImVec2(500.0f, 200.0f));
    ImGui::Begin("Brand Host", nullptr, ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoTitleBar);
    bool passed = Check(ImGui::BeginMenuBar(), "Brand host bar must be visible.");
    if (passed) {
        ImGui::SetCursorPosX(148.0f);
        ImGui::PushStyleVarX(ImGuiStyleVar_ItemSpacing, spacing);
        ImGui::PushItemFlag(ImGuiItemFlags_MenuChrome, true);
        const char* labels[] = {"File##top", "Database", "Help"};
        for (const char* label : labels) {
            const float start = ImGui::GetCursorScreenPos().x;
            const float width = ImGui::CalcTextSize(label, nullptr, true).x;
            if (label[0] == 'H') ImGui::MenuItem(label);
            else if (ImGui::BeginMenu(label)) ImGui::EndMenu();
            passed &= Check(std::abs(ImGui::GetItemRectMin().x - start) <= 1.0f &&
                                std::abs(ImGui::GetCursorScreenPos().x - start - width - spacing) <= 1.0f,
                            "A top menu slot must advance by visible label width plus one gap, not two.");
        }
        ImGui::PopItemFlag();
        ImGui::PopStyleVar();
        ImGui::EndMenuBar();
    }
    ImGui::End();
    ImGui::Render();
    ImGui::DestroyContext();
    return passed;
}

static float DrawShortcut(const char* label, const char* shortcut) {
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const int firstVertex = draw->VtxBuffer.Size;
    ImGui::MenuItem(label, shortcut);
    const ImU32 color = ImGui::GetColorU32(ImGuiCol_TextDisabled);
    float right = -FLT_MAX;
    for (int index = firstVertex; index < draw->VtxBuffer.Size; ++index) {
        const ImDrawVert& vertex = draw->VtxBuffer[index];
        if (vertex.col == color) right = ImMax(right, vertex.pos.x);
    }
    return right;
}

static bool ExerciseMenu(float viewportWidth, float hostItemX, float scale, ImGuiDir expectedSide) {
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(viewportWidth, 340.0f);
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    ImGuiStyle& style = ImGui::GetStyle();
    style.FontScaleDpi = scale;
    style.WindowPadding = ImVec2(13.0f * scale, 9.0f * scale);
    style.ItemSpacing = ImVec2(16.0f * scale, 12.0f * scale);
    style.DisplaySafeAreaPadding = ImVec2(3.0f, 3.0f);
    style.PopupBorderSize = 1.0f;

    bool passed = false;
    const std::string longLabel(144, 'W');
    for (int frame = 0; frame < 4; ++frame) {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(io.DisplaySize);
        ImGui::Begin("Host", nullptr, ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoTitleBar);
        if (ImGui::BeginMenuBar()) {
            ImGui::SetCursorPosX(hostItemX);
            ImGui::PushItemFlag(ImGuiItemFlags_MenuChrome, true);
            if (frame == 0) ImGui::OpenPopup("File");
            if (ImGui::BeginMenu("File")) {
                const ImRect parent = ImGui::GetCurrentWindow()->Rect();
                const float shortRight = DrawShortcut("Open", "Ctrl+O");
                const float longRight = DrawShortcut("Scan files", "Ctrl+Shift+O");
                if (expectedSide == ImGuiDir_None) ImGui::MenuItem(longLabel.c_str());
                const float parentRowY = ImGui::GetCursorScreenPos().y;
                if (frame == 0) ImGui::OpenPopup("Database");
                if (ImGui::BeginMenu("Database")) {
                    const ImRect child = ImGui::GetCurrentWindow()->Rect();
                    const float childRowY = ImGui::GetCursorScreenPos().y;
                    ImGui::MenuItem("Choose database...");
                    if (frame == 3) {
                        passed = Check(shortRight != -FLT_MAX && longRight != -FLT_MAX &&
                                           std::abs(shortRight - longRight) <= 1.0f,
                                       "Shortcut glyphs must share a right edge.");
                        passed &= Check(ImGui::GetCurrentWindow()->WindowRounding == std::floor(10.0f * scale) &&
                                            ImGui::GetCurrentWindow()->ParentWindow->WindowRounding == std::floor(10.0f * scale),
                                        "Top dropdown and submenu must use the same scaled 10px corners.");
                        passed &= Check(parent.Min.x >= 0.0f && parent.Max.x <= viewportWidth &&
                                            child.Min.x >= 0.0f && child.Max.x <= viewportWidth,
                                        "Both menus must stay inside the viewport.");
                        passed &= Check(std::abs(childRowY - parentRowY) <= 1.0f,
                                        "The first child row must align with its parent row.");
                        if (viewportWidth >= 720.0f) {
                            passed &= Check(std::abs(parent.GetWidth() - 270.0f * scale) <= 1.0f &&
                                                std::abs(child.GetWidth() - 205.0f * scale) <= 1.0f,
                                            "Spacious menus must preserve the reference widths.");
                        }
                        if (expectedSide != ImGuiDir_None) {
                            const float separation = expectedSide == ImGuiDir_Right
                                ? child.Min.x - parent.Max.x : parent.Min.x - child.Max.x;
                            passed &= Check(std::abs(separation) <= 2.5f * scale,
                                            "The submenu must attach to the available side.");
                        }
                    }
                    ImGui::EndMenu();
                }
                ImGui::EndMenu();
            }
            ImGui::PopItemFlag();
            ImGui::EndMenuBar();
        }
        ImGui::End();
        ImGui::Render();
    }
    ImGui::DestroyContext();
    return passed;
}

static bool ExerciseContextMenu(float scale) {
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(520.0f, 315.0f);
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    ImGuiStyle& style = ImGui::GetStyle();
    style.FontScaleDpi = scale;
    style.PopupRounding = 3.0f;
    style.PopupBorderSize = 0.0f;
    style.DisplaySafeAreaPadding = ImVec2(3.0f, 3.0f);
    const ImVec2 click(492.0f, 270.0f);
    const ImGuiWindowFlags popupFlags = ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoSavedSettings;
    bool passed = true;
    int openedFrames = 0;
    bool childVisible = false;
    for (int frame = 0; frame < 7; ++frame) {
        io.AddMousePosEvent(frame < 4 ? click.x : 35.0f, frame < 4 ? click.y : 30.0f);
        if (frame == 1) io.AddMouseButtonEvent(ImGuiMouseButton_Right, true);
        if (frame == 2) io.AddMouseButtonEvent(ImGuiMouseButton_Right, false);
        if (frame == 5) io.DisplaySize = ImVec2(410.0f, 255.0f);
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
        ImGui::SetNextWindowSize(io.DisplaySize);
        ImGui::Begin("Results Host", nullptr, ImGuiWindowFlags_NoTitleBar);
        ImGui::SetCursorPosY(247.0f);
        ImGui::Selectable("Audio result", false, 0, ImVec2(490.0f, 40.0f));
        ImGuiContext& context = *ImGui::GetCurrentContext();
        const int styleDepth = context.StyleVarStack.Size;
        const int colorDepth = context.ColorStack.Size;
        ImGui::OpenPopupOnItemClick("Results");
        if (ImGui::BeginPopupEx(ImGui::GetID("Results"), popupFlags | ImGuiWindowFlags_MenuChrome)) {
            ++openedFrames;
            ImGuiWindow* root = ImGui::GetCurrentWindow();
            passed &= Check(frame >= 2 && std::abs(context.OpenPopupStack[0].OpenPopupPos.x - click.x) <= 2.0f &&
                                std::abs(context.OpenPopupStack[0].OpenPopupPos.y - click.y) <= 2.0f,
                            "The result menu must open on right release and retain its mouse anchor.");
            passed &= Check(root->WindowRounding == std::floor(10.0f * scale) && root->WindowBorderSize == 1.0f &&
                                style.PopupRounding == 3.0f && style.PopupBorderSize == 0.0f,
                            "The result popup must lock the flat shell without changing the ambient style.");
            if (frame >= 4) {
                passed &= Check(root->Pos.x >= 0.0f && root->Pos.y >= 0.0f &&
                                    root->Rect().Max.x <= io.DisplaySize.x && root->Rect().Max.y <= io.DisplaySize.y,
                                "The result popup must stay on screen, including after a viewport resize.");
            }
            const float shortRight = DrawShortcut("Show in Explorer", "Ctrl+R");
            const float longRight = DrawShortcut("Copy path", "Ctrl+Alt+C");
            if (frame >= 4) {
                passed &= Check(shortRight != -FLT_MAX && longRight != -FLT_MAX && std::abs(shortRight - longRight) <= 1.0f,
                                "Result menu shortcuts must use the same right alignment as top menus.");
            }
            if (frame == 3) ImGui::OpenPopup("Rating");
            if (ImGui::BeginMenu("Rating")) {
                ImGuiWindow* child = ImGui::GetCurrentWindow();
                ImGui::MenuItem("Five stars");
                if (frame == 4) {
                    childVisible = true;
                    passed &= Check((child->Flags & ImGuiWindowFlags_MenuChrome) && child->Viewport == root->Viewport &&
                                        child->Rect().Max.x <= io.DisplaySize.x && child->Pos.x >= 0.0f &&
                                        child->Pos.y >= 0.0f && child->Rect().Max.y <= io.DisplaySize.y,
                                    "A result submenu must inherit menu chrome and stay in its owner's viewport.");
                }
                ImGui::EndMenu();
            }
            ImGui::EndPopup();
            passed &= Check(context.StyleVarStack.Size == styleDepth && context.ColorStack.Size == colorDepth,
                            "The result menu must unwind before the next window.");
        }
        ImGui::End();
        ImGui::Render();
    }
    ImGui::DestroyContext();
    return Check(openedFrames >= 4 && childVisible, "The result popup and its submenu must render across frames.") && passed;
}

static bool ExerciseMenuTextColors() {
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(700.0f, 320.0f);
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    ImGuiContext& context = *ImGui::GetCurrentContext();
    const ImVec4 red(1.0f, 0.4f, 0.4f, 1.0f);
    const ImVec4 green(0.4f, 0.9f, 0.4f, 1.0f);
    const ImVec4 contrast(1.0f, 1.0f, 0.0f, 1.0f);
    context.MenuChromeActiveText = contrast;
    struct MenuRow {
        const char* label;
        ImVec4 color;
        bool enabled;
    };
    const MenuRow rows[] = {{"Delete file", red, true}, {"Write file", green, true}, {"Unavailable", green, false}};
    bool passed = true;
    bool verified = false;
    for (int frame = 0; frame < 4; ++frame) {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
        ImGui::SetNextWindowSize(io.DisplaySize);
        ImGui::Begin("Text Host", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_MenuBar);
        if (ImGui::BeginMenuBar()) {
            ImGui::PushItemFlag(ImGuiItemFlags_MenuChrome, true);
            if (frame == 0) ImGui::OpenPopup("File");
            if (ImGui::BeginMenu("File")) {
                ImGuiWindow* root = ImGui::GetCurrentWindow();
                ImDrawList* draw = root->DrawList;
                context.NavWindow = root;
                context.NavCursorVisible = true;
                context.NavHighlightItemUnderNav = true;
                for (const MenuRow& row : rows) {
                    context.NavId = root->GetID(row.label);
                    const int firstVertex = draw->VtxBuffer.Size;
                    ImGui::PushStyleColor(ImGuiCol_Text, row.color);
                    const bool pressed = ImGui::MenuItem(row.label, nullptr, false, row.enabled);
                    ImGui::PopStyleColor();
                    if (frame == 3) {
                        verified = true;
                        ImVec4 expected = row.color;
                        if (!row.enabled) expected.w *= ImGui::GetStyle().DisabledAlpha;
                        passed &= Check(!pressed && ContainsColor(*draw, firstVertex, ImGui::GetColorU32(expected)) &&
                                            !ContainsColor(*draw, firstVertex, ImGui::GetColorU32(contrast)),
                                        "Focused warning text must retain its explicit color; disabled text must stay dimmed.");
                    }
                }
                ImGui::EndMenu();
            }
            ImGui::PopItemFlag();
            ImGui::EndMenuBar();
        }
        ImGui::End();
        ImGui::Render();
    }
    ImGui::DestroyContext();
    return verified && passed;
}

int main() {
    const bool right = ExerciseMenu(720.0f, 50.0f, 1.0f, ImGuiDir_Right);
    const bool left = ExerciseMenu(720.0f, 500.0f, 1.0f, ImGuiDir_Left);
    const bool fitNarrowSide = ExerciseMenu(448.0f, 200.0f, 1.0f, ImGuiDir_Left);
    const bool narrow = ExerciseMenu(360.0f, 120.0f, 1.5f, ImGuiDir_None);
    const bool bar = ExerciseBarRhythm(1.0f) & ExerciseBarRhythm(1.5f);
    const bool context = ExerciseContextMenu(1.0f) & ExerciseContextMenu(1.5f);
    const bool colors = ExerciseMenuTextColors();
    return right && left && fitNarrowSide && narrow && bar && context && colors ? 0 : 1;
}
