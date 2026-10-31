#include "DockHitTest.h"

#include <cmath>
#include <cstdio>

#ifdef DOCK_PERF_REPLAY
#include "DockDragSession.h"
#include "imgui.h"
#include "backends/imgui_impl_opengl3.h"
#include "backends/imgui_impl_win32.h"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <GL/gl.h>
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(
    HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
#include <algorithm>
#include <chrono>
#include <string>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;
using namespace aura::ui::dock;
static int g_replayNcHitMessages = 0;
static bool g_replayImGuiWin32Ready = false;
static HGLRC g_replayMainRc = nullptr;
static int g_replayViewportCreates = 0;
static int g_replayViewportDestroys = 0;
struct ReplayViewportWglData { HDC dc = nullptr; };
void ReplayRendererCreateWindow(ImGuiViewport* viewport) {
    ++g_replayViewportCreates;
    HWND hwnd = static_cast<HWND>(viewport->PlatformHandle);
    auto* data = new ReplayViewportWglData{};
    data->dc = GetDC(hwnd);
    PIXELFORMATDESCRIPTOR pfd{};
    pfd.nSize = sizeof(pfd);
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    const int format = data->dc ? ChoosePixelFormat(data->dc, &pfd) : 0;
    if (!format || !SetPixelFormat(data->dc, format, &pfd)) {
        if (data->dc) ReleaseDC(hwnd, data->dc);
        data->dc = nullptr;
    }
    viewport->RendererUserData = data;
}
void ReplayRendererDestroyWindow(ImGuiViewport* viewport) {
    ++g_replayViewportDestroys;
    auto* data = static_cast<ReplayViewportWglData*>(viewport->RendererUserData);
    if (!data) return;
    if (data->dc) ReleaseDC(static_cast<HWND>(viewport->PlatformHandle), data->dc);
    delete data;
    viewport->RendererUserData = nullptr;
}
void ReplayPlatformRenderWindow(ImGuiViewport* viewport, void*) {
    auto* data = static_cast<ReplayViewportWglData*>(viewport->RendererUserData);
    if (data && data->dc) wglMakeCurrent(data->dc, g_replayMainRc);
}
void ReplayRendererSwapBuffers(ImGuiViewport* viewport, void*) {
    auto* data = static_cast<ReplayViewportWglData*>(viewport->RendererUserData);
    if (data && data->dc) SwapBuffers(data->dc);
}
double Milliseconds(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}
double Percentile(std::vector<double> values, double part) {
    std::sort(values.begin(), values.end());
    if (values.empty()) return 0.0;
    const size_t index = static_cast<size_t>(std::ceil(part * values.size())) - 1;
    return values[std::min(index, values.size() - 1)];
}
LRESULT CALLBACK ReplayWindowProc(HWND hwnd, UINT message, WPARAM w, LPARAM l) {
    if (message == WM_NCHITTEST) {
        ++g_replayNcHitMessages;
        return HTCAPTION;
    }
    if (g_replayImGuiWin32Ready && ImGui_ImplWin32_WndProcHandler(hwnd, message, w, l))
        return 1;
    return DefWindowProcW(hwnd, message, w, l);
}
bool CreateOpenGLReplayContext(HWND hwnd, HDC& dc, HGLRC& rc) {
    dc = GetDC(hwnd);
    if (!dc) return false;
    PIXELFORMATDESCRIPTOR pfd{};
    pfd.nSize = sizeof(pfd);
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.cDepthBits = 24;
    const int format = ChoosePixelFormat(dc, &pfd);
    if (!format || !SetPixelFormat(dc, format, &pfd)) return false;
    rc = wglCreateContext(dc);
    return rc && wglMakeCurrent(dc, rc);
}
bool CheckRightOuterGripImGui() {
    DockLayout layout;
    if (!layout.InsertModule("similar-sounds", DockSide::Right, 0.25f)) return false;
    DockRootSlots slots{};
    slots.right = {1300.0f, 80.0f, 300.0f, 700.0f};
    const DockFrameGeometry frame = BuildDockFrameGeometry(
        layout, {0.0f, 0.0f, 1600.0f, 900.0f}, 80.0f, 40.0f, slots);
    const AdjacentLeafGrip grip = PlanRightDockOuterGrip(frame, "similar-sounds");
    if (!grip.visible || frame.gripCount != 0) return false;
    ImGuiContext* context = ImGui::CreateContext();
    ImGui::SetCurrentContext(context);
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(1600.0f, 900.0f);
    io.DeltaTime = 1.0f / 60.0f;
    io.Fonts->Build();
    io.AddMousePosEvent(grip.rect.x + grip.rect.width * 0.5f,
        grip.rect.y + grip.rect.height * 0.5f);
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(grip.rect.x, grip.rect.y), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(grip.rect.width, grip.rect.height), ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking |
        ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoFocusOnAppearing;
    const bool visible = ImGui::Begin("##RightDockLeafSplitter", nullptr, flags);
    ImGui::PopStyleVar(3);
    bool hitMatchesPaintedSeam = false;
    if (visible) {
        ImGui::InvisibleButton("##RightDockLeafSplitterHit",
            ImVec2(grip.rect.width, grip.rect.height));
        const ImVec2 min = ImGui::GetItemRectMin();
        const ImVec2 max = ImGui::GetItemRectMax();
        hitMatchesPaintedSeam = min.x == grip.rect.x && min.y == grip.rect.y &&
            max.x == grip.rect.x + grip.rect.width &&
            max.y == grip.rect.y + grip.rect.height &&
            ImGui::IsMouseHoveringRect(min, max, false);
    }
    ImGui::End();
    ImGui::Render();
    ImGui::DestroyContext(context);
    return hitMatchesPaintedSeam;
}
bool ReplayOne(HWND hwnd, HDC dc, int panelCount, bool loneSimilar, bool overlay, int rows, bool drag) {
    g_replayViewportCreates = 0;
    g_replayViewportDestroys = 0;
    DockLayout layout;
    if (!layout.EnsureFloat("filter")) return false;
    if (loneSimilar || panelCount >= 3) {
        if (!layout.InsertModule("similar-sounds", DockSide::Right, 0.25f)) return false;
    }
    if (panelCount >= 3 && !loneSimilar &&
        !layout.InsertAdjacent("similar-sounds", "info", DockSide::Top, 0.5f)) return false;
    if (panelCount >= 5) {
        if (!layout.InsertModule("search-results", DockSide::Left, 0.25f)) return false;
        if (!layout.InsertModule("mod-docktest-filter", DockSide::Top, 0.15f)) return false;
    }
    DockDragSession session(layout);
    if (drag && !session.BeginDrag("filter")) return false;
    const DockRect work{0.0f, 0.0f, 1600.0f, 900.0f};
    DockRootSlots slots{};
    slots.left = {0.0f, 80.0f, 350.0f, 620.0f};
    slots.right = {1250.0f, 80.0f, 350.0f, 620.0f};
    slots.top = {0.0f, 80.0f, 1600.0f, 120.0f};
    slots.bottom = {350.0f, 700.0f, 900.0f, 120.0f};
    std::vector<std::string> list;
    list.reserve(static_cast<size_t>(rows));
    for (int i = 0; i < rows; ++i) list.emplace_back("isolated-row-" + std::to_string(i));
    ImGuiContext* context = ImGui::CreateContext();
    ImGui::SetCurrentContext(context);
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
    io.DisplaySize = ImVec2(1600.0f, 900.0f);
    io.DeltaTime = 1.0f / 60.0f;
    if (!ImGui_ImplWin32_InitForOpenGL(hwnd) || !ImGui_ImplOpenGL3_Init("#version 130")) {
        std::printf("FAIL ImGui Win32/OpenGL backend initialization\n");
        return false;
    }
    ImGuiPlatformIO& platform = ImGui::GetPlatformIO();
    platform.Renderer_CreateWindow = ReplayRendererCreateWindow;
    platform.Renderer_DestroyWindow = ReplayRendererDestroyWindow;
    platform.Renderer_SwapBuffers = ReplayRendererSwapBuffers;
    platform.Platform_RenderWindow = ReplayPlatformRenderWindow;
    g_replayImGuiWin32Ready = true;
    std::vector<double> frameSamples, geometrySamples, hitSamples, previewSamples;
    std::vector<double> overlaySamples, captionSamples, glRenderSamples, presentSamples;
    std::vector<double> clearSamples, drawSamples, viewportSamples;
    int geometryCount = 0, hitCount = 0, previewCount = 0;
    int overlayCount = 0, captionCount = 0, hintCountTotal = 0, ncHitMessages = 0;
    int extraViewportCount = 0;
    for (int frameNumber = 0; frameNumber < 150; ++frameNumber) {
        const bool sample = frameNumber >= 30;
        const auto frameStart = Clock::now();
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        const auto geometryStart = Clock::now();
        const DockFrameGeometry frame = BuildDockFrameGeometry(layout, work, 80.0f, 40.0f, slots);
        const double geometryMs = Milliseconds(geometryStart);
        if (sample) { geometrySamples.push_back(geometryMs); ++geometryCount; }
        double hitMs = 0.0, previewMs = 0.0, captionMs = 0.0;
        if (drag) {
            const DockRect moving{1400.0f + static_cast<float>(frameNumber % 12), 200.0f, 300.0f, 350.0f};
            const auto captionStart = Clock::now();
            const int messagesBefore = g_replayNcHitMessages;
            const LRESULT caption = SendMessageW(hwnd, WM_NCHITTEST, 0,
                MAKELPARAM(static_cast<short>(moving.x + 100.0f), static_cast<short>(moving.y + 12.0f)));
            captionMs = Milliseconds(captionStart);
            if (caption != HTCAPTION) return false;
            const auto hitStart = Clock::now();
            const DockHit hit = HitDockTarget(moving, work, 80.0f,
                frame.modules, frame.moduleCount, &frame.rootSlots, &layout);
            hitMs = Milliseconds(hitStart);
            session.UpdatePreview(hit);
            const auto previewStart = Clock::now();
            const DockPreviewPlan plan = PlanDockPreview(session.Preview(), work,
                80.0f, 40.0f, frame.modules, frame.moduleCount, 80.0f, &frame.rootSlots);
            previewMs = Milliseconds(previewStart);
            auto* draw = ImGui::GetForegroundDrawList();
            for (int i = 0; i < plan.stripCount; ++i) {
                const DockRect& box = plan.strips[i].rect;
                draw->AddRectFilled(ImVec2(box.x, box.y),
                    ImVec2(box.x + box.width, box.y + box.height), IM_COL32(60, 140, 220, 110));
            }
            if (sample) { ++captionCount; ++hitCount; ++previewCount; }
            if (sample) ncHitMessages += g_replayNcHitMessages - messagesBefore;
        }
        double overlayMs = 0.0;
        if (overlay) {
            const auto overlayStart = Clock::now();
            DockHint hints[64]{};
            const int count = CollectLegalDockHints(layout, work, 80.0f, frame, hints, 64);
            auto* draw = ImGui::GetForegroundDrawList();
            for (int i = 0; i < count; ++i) {
                const DockRect& box = hints[i].visibleRect;
                draw->AddRect(ImVec2(box.x, box.y),
                    ImVec2(box.x + box.width, box.y + box.height), IM_COL32(90, 180, 240, 180));
            }
            overlayMs = Milliseconds(overlayStart);
            if (sample) { ++overlayCount; hintCountTotal += count; }
        }
        ImGui::SetNextWindowPos(ImVec2(400.0f, 220.0f));
        ImGui::SetNextWindowSize(ImVec2(600.0f, 400.0f));
        ImGui::Begin("Synthetic search results");
        ImGuiListClipper clipper;
        clipper.Begin(rows);
        while (clipper.Step()) {
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i)
                ImGui::TextUnformatted(list[static_cast<size_t>(i)].c_str());
        }
        ImGui::End();
        const ImVec2 mainPos = ImGui::GetMainViewport()->Pos;
        const ImVec2 mainSize = ImGui::GetMainViewport()->Size;
        for (int panelIndex = 0; panelIndex < panelCount; ++panelIndex) {
            ImGui::SetNextWindowPos(ImVec2(
                mainPos.x + mainSize.x + 80.0f + 340.0f * panelIndex +
                    (drag && panelIndex == 0 ? static_cast<float>(frameNumber % 12) : 0.0f),
                mainPos.y + 120.0f), ImGuiCond_Always);
            ImGui::SetNextWindowSize(ImVec2(320.0f, 240.0f), ImGuiCond_Always);
            const std::string title = "Synthetic floating Dock panel ##" + std::to_string(panelIndex);
            ImGui::Begin(title.c_str());
            ImGui::TextUnformatted(loneSimilar && panelIndex == 0
                ? "Similar Sounds" : "Isolated Dock viewport");
            ImGui::End();
        }
        ImGui::Render();
        const auto glRenderStart = Clock::now();
        glViewport(0, 0, 1600, 900);
        glClearColor(0.02f, 0.03f, 0.04f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        const double clearMs = Milliseconds(glRenderStart);
        const auto drawStart = Clock::now();
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        const double drawMs = Milliseconds(drawStart);
        const double glRenderMs = clearMs + drawMs;
        const auto viewportStart = Clock::now();
        ImGui::UpdatePlatformWindows();
        ImGui::RenderPlatformWindowsDefault();
        wglMakeCurrent(dc, g_replayMainRc);
        const double viewportMs = Milliseconds(viewportStart);
        const auto presentStart = Clock::now();
        if (!SwapBuffers(dc)) return false;
        const double presentMs = Milliseconds(presentStart);
        if (sample) {
            frameSamples.push_back(Milliseconds(frameStart));
            glRenderSamples.push_back(glRenderMs);
            presentSamples.push_back(presentMs);
            clearSamples.push_back(clearMs);
            drawSamples.push_back(drawMs);
            viewportSamples.push_back(viewportMs);
            extraViewportCount = std::max(extraViewportCount,
                ImGui::GetPlatformIO().Viewports.Size - 1);
            if (drag) { captionSamples.push_back(captionMs); hitSamples.push_back(hitMs); previewSamples.push_back(previewMs); }
            if (overlay) overlaySamples.push_back(overlayMs);
        }
    }
    if (drag) session.CommitFloatingOnMouseUp();
    const int viewportCreates = g_replayViewportCreates;
    const int viewportDestroys = g_replayViewportDestroys;
    g_replayImGuiWin32Ready = false;
    ImGui::DestroyPlatformWindows();
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext(context);
    std::printf("{\"panels\":%d,\"loneSimilar\":%s,\"overlay\":%s,\"rows\":%d,\"drag\":%s,"
                "\"samples\":%zu,\"frameP50Ms\":%.6f,\"frameP95Ms\":%.6f,\"frameP99Ms\":%.6f,"
                "\"geometryCount\":%d,\"geometryP95Ms\":%.6f,\"hitCount\":%d,\"hitP95Ms\":%.6f,"
                "\"previewCount\":%d,\"previewP95Ms\":%.6f,\"captionCount\":%d,\"captionP95Ms\":%.6f,"
                "\"overlayCount\":%d,\"overlayP95Ms\":%.6f,\"hintCountTotal\":%d,"
                "\"glRenderP95Ms\":%.6f,\"presentP95Ms\":%.6f,\"ncHitMessages\":%d,"
                "\"clearP95Ms\":%.6f,\"drawP95Ms\":%.6f,"
                "\"viewportsP95Ms\":%.6f,\"extraViewportCount\":%d,"
                "\"viewportCreates\":%d,\"viewportDestroys\":%d}\n",
                panelCount, loneSimilar ? "true" : "false", overlay ? "true" : "false", rows, drag ? "true" : "false",
                frameSamples.size(), Percentile(frameSamples, .50), Percentile(frameSamples, .95), Percentile(frameSamples, .99),
                geometryCount, Percentile(geometrySamples, .95), hitCount, Percentile(hitSamples, .95),
                previewCount, Percentile(previewSamples, .95), captionCount, Percentile(captionSamples, .95),
                overlayCount, Percentile(overlaySamples, .95), hintCountTotal,
                Percentile(glRenderSamples, .95), Percentile(presentSamples, .95), ncHitMessages,
                Percentile(clearSamples, .95), Percentile(drawSamples, .95),
                Percentile(viewportSamples, .95), extraViewportCount,
                viewportCreates, viewportDestroys);
    return true;
}
int RunDockPerfReplay() {
    if (!CheckRightOuterGripImGui()) {
        std::printf("FAIL right Similar grip render/hit geometry\n");
        return 5;
    }
    WNDCLASSW wc{};
    wc.lpfnWndProc = ReplayWindowProc;
    wc.lpszClassName = L"HarkenSearchIsolatedDockPerfReplay";
    if (!RegisterClassW(&wc)) return 2;
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPED,
        -32000, -32000, 1600, 900, nullptr, nullptr, nullptr, nullptr);
    if (!hwnd) return 3;
    HDC dc = nullptr;
    HGLRC rc = nullptr;
    if (!CreateOpenGLReplayContext(hwnd, dc, rc)) {
        std::printf("FAIL offscreen WGL context\n");
        return 6;
    }
    g_replayMainRc = rc;
    for (int rows : {0, 82000}) {
        for (int scenario = 0; scenario < 4; ++scenario) {
            const int panels = scenario == 0 ? 1 : scenario == 1 ? 3 : scenario == 2 ? 5 : 2;
            const bool loneSimilar = scenario == 3;
            for (bool overlay : {false, true}) {
                for (bool drag : {false, true}) {
                    if (!ReplayOne(hwnd, dc, panels, loneSimilar, overlay, rows, drag)) return 4;
                }
            }
        }
    }
    wglMakeCurrent(nullptr, nullptr);
    wglDeleteContext(rc);
    ReleaseDC(hwnd, dc);
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, nullptr);
    return 0;
}
} // namespace
#endif

namespace {

bool Check(bool ok, const char* message) {
    if (!ok) {
        std::printf("FAIL: %s\n", message);
        return false;
    }
    return true;
}

} // namespace

#ifdef DOCK_PERF_REPLAY
int main() { return RunDockPerfReplay(); }
#else
int main() {
    using namespace aura::ui::dock;
    const DockRect mainWork{0.0f, 0.0f, 1920.0f, 1080.0f};
    const float snap = 80.0f;

    // The four module windows use the shared rounded shell, so their Dock
    // floors must describe the real readable content area instead of falling
    // back to the generic 160x120 floor.
    {
        struct ModuleFloor { const char* id; float width; float height; };
        const ModuleFloor floors[] = {
            {"mod-ucs-browser", 480.0f, 260.0f},
            {"mod-tag-browser", 320.0f, 320.0f},
            {"mod-workflow", 560.0f, 360.0f},
            {"mod-transfer", 480.0f, 360.0f},
        };
        for (const auto& floor : floors) {
            if (!Check(IsKnownModuleId(floor.id), "new module id must be accepted by DockLayout")) return 1;
            const DockMinimumSize actual = DockModuleMinimum(floor.id);
            if (!Check(actual.width >= floor.width && actual.height >= floor.height,
                       "new module Dock floor must protect readable content")) return 1;
        }
    }

    // UX contract: the visible preview is the resulting leaf, not the hit band.
    {
        const DockModuleTarget target{"filter", {0, 80, 300, 600}};
        const auto candidate = HitDockTarget({295, 300, 1, 1}, mainWork, snap, &target, 1);
        const auto preview = PlanDockPreview(candidate, mainWork, 80, 40, &target, 1, snap);
        const bool fullArea = Check(preview.stripCount == 1 &&
            SameDockRect(preview.strips[0].rect, candidate.resultRect),
            "UX preview must show the full final occupied area");
        const bool readable = Check(!PlanLeafEdgeSplit(target.rect, DockSide::Right, snap, .5f).accepted,
            "UX must reject splitting 300px into unreadable 150px leaves");
        if (!fullArea || !readable) return 1;
    }

    const DockRect panelFullyLeft{-280.0f, 120.0f, 280.0f, 400.0f};
    DockHit hit = HitDockTarget(panelFullyLeft, mainWork, snap);
    if (!Check(hit.kind == DockHitKind::None, "fully past the left outer edge must be None")) return 1;

    const DockRect panelFullyRight{1920.0f, 120.0f, 250.0f, 400.0f};
    hit = HitDockTarget(panelFullyRight, mainWork, snap);
    if (!Check(hit.kind == DockHitKind::None, "fully past the right outer edge must be None")) return 1;

    const DockRect grazingOuter{-280.0f, 120.0f, 280.0f, 400.0f};
    hit = HitDockTarget(grazingOuter, mainWork, snap);
    if (!Check(hit.kind == DockHitKind::None, "kissing the left outer edge must be None")) return 1;

    const DockRect secondary{2000.0f, 40.0f, 360.0f, 280.0f};
    hit = HitDockTarget(secondary, mainWork, snap);
    if (!Check(hit.kind == DockHitKind::None, "secondary monitor must be None")) return 1;

    const DockRect insideLeft{20.0f, 120.0f, 280.0f, 400.0f};
    hit = HitDockTarget(insideLeft, mainWork, snap);
    if (!Check(hit.kind == DockHitKind::Main && hit.side == DockSide::Left,
               "inner left strip must be Main+Left")) {
        return 1;
    }

    const DockRect insideRight{1860.0f, 120.0f, 250.0f, 400.0f};
    hit = HitDockTarget(insideRight, mainWork, snap);
    if (!Check(hit.kind == DockHitKind::Main && hit.side == DockSide::Right,
               "inner right strip must be Main+Right")) {
        return 1;
    }

    const DockRect topSlot{400.0f, 80.0f, 1120.0f, 100.0f};
    DockRootSlots topSlots{};
    topSlots.top = topSlot;
    const DockRect draggedToTop{720.0f, 0.0f, 160.0f, 70.0f};
    hit = HitDockTarget(draggedToTop, mainWork, snap, nullptr, 0, &topSlots);
    if (!Check(hit.kind == DockHitKind::Main && hit.side == DockSide::Top,
               "dragging to the main window top edge must produce a Top dock hit")) {
        return 1;
    }
    if (!Check(hit.hitRect.y == topSlot.y && hit.hitRect.height <= topSlot.height,
               "top hit preview must stay on the rendered top slot below the toolbar")) {
        return 1;
    }
    DockPreviewPlan topPlan = PlanDockPreview(hit, mainWork, 80.0f, 80.0f, nullptr, 0, snap, &topSlots);
    if (!Check(topPlan.stripCount == 1 && SameDockRect(topPlan.strips[0].rect, hit.resultRect),
               "top preview must use the final occupied area")) {
        return 1;
    }
    DockRootSlots topOnlyWithPlaceholderColumns{};
    topOnlyWithPlaceholderColumns.left = {0.0f, 80.0f, 400.0f, 720.0f};
    topOnlyWithPlaceholderColumns.right = {1520.0f, 80.0f, 400.0f, 720.0f};
    topOnlyWithPlaceholderColumns.top = {0.0f, 80.0f, 1920.0f, 100.0f};
    const DockRect draggedAtUnoccupiedLeftTop{40.0f, 0.0f, 120.0f, 70.0f};
    hit = HitDockTarget(draggedAtUnoccupiedLeftTop, mainWork, snap, nullptr, 0,
                        &topOnlyWithPlaceholderColumns);
    if (!Check(hit.kind == DockHitKind::Main && hit.side == DockSide::Top,
               "synthetic hidden side columns must not block the top dock edge")) {
        return 1;
    }

    // Regression: a full-width Top band is not Left/Right occupancy. Real
    // region roots decide availability, so Left/Right docking stays reachable
    // even while the Top region spans the whole work width.
    {
        DockLayout topFullLayout;
        if (!topFullLayout.InsertModule("filter", DockSide::Top, 0.24f)) return 1;
        DockRootSlots topFullSlots{};
        topFullSlots.left = {0.0f, 200.0f, 400.0f, 620.0f};
        topFullSlots.right = {1520.0f, 200.0f, 400.0f, 620.0f};
        topFullSlots.top = {0.0f, 80.0f, 1920.0f, 120.0f};
        DockModuleTarget topLeaves[4]{};
        const int topLeafCount = CollectLeafRects(
            topFullLayout.RegionRoot(DockSide::Top), topFullSlots.top,
            topLeaves, 4, 0);
        const DockRect draggedLeftOfTopBand{0.0f, 300.0f, 120.0f, 80.0f};
        hit = HitDockTarget(draggedLeftOfTopBand, mainWork, snap, topLeaves,
                            topLeafCount, &topFullSlots, &topFullLayout);
        if (!Check(hit.kind == DockHitKind::Main && hit.side == DockSide::Left,
                   "full-width Top band must not block Left main-edge docking")) {
            return 1;
        }
        const DockRect draggedRightOfTopBand{1790.0f, 300.0f, 120.0f, 80.0f};
        hit = HitDockTarget(draggedRightOfTopBand, mainWork, snap, topLeaves,
                            topLeafCount, &topFullSlots, &topFullLayout);
        if (!Check(hit.kind == DockHitKind::Main && hit.side == DockSide::Right,
                   "full-width Top band must not block Right main-edge docking")) {
            return 1;
        }
    }

    const DockRect infoSlot{1520.0f, 80.0f, 400.0f, 620.0f};
    DockRootSlots slots{};
    slots.right = infoSlot;
    const DockRect overWaveformAtRightEdge{1860.0f, 760.0f, 80.0f, 200.0f};
    hit = HitDockTarget(overWaveformAtRightEdge, mainWork, snap, nullptr, 0, &slots);
    if (!Check(hit.kind == DockHitKind::None,
               "right-edge overlap over the main waveform must not hit Right")) {
        return 1;
    }
    const DockRect overInfoSlot{1860.0f, 120.0f, 80.0f, 200.0f};
    hit = HitDockTarget(overInfoSlot, mainWork, snap, nullptr, 0, &slots);
    if (!Check(hit.kind == DockHitKind::Main && hit.side == DockSide::Right,
               "right-edge overlap inside the Info slot must hit Right")) {
        return 1;
    }
    DockHit rightHit;
    rightHit.kind = DockHitKind::Main;
    rightHit.side = DockSide::Right;
    rightHit.resultRect = SplitIncomingRect(infoSlot, DockSide::Right, kDockIncomingShare);
    DockPreviewPlan rightPlan = PlanDockPreview(rightHit, mainWork, 80.0f, 80.0f, nullptr, 0, snap, &slots);
    if (!Check(rightPlan.stripCount == 1, "Right preview must draw one strip")) return 1;
    if (!Check(rightPlan.strips[0].rect.height < mainWork.height - 1.0f,
               "Right preview strip must not span the full work height")) {
        return 1;
    }
    if (!Check(rightPlan.strips[0].rect.height <= infoSlot.height + 0.5f,
               "Right preview strip height must equal the Info slot, not the waveform")) {
        return 1;
    }

    const DockRect filterRect{0.0f, 80.0f, 400.0f, 900.0f};
    DockModuleTarget filter{"filter", filterRect};
    const DockRect overFilterRight{360.0f, 200.0f, 80.0f, 200.0f};
    hit = HitDockTarget(overFilterRight, mainWork, snap, &filter, 1);
    if (!Check(hit.kind == DockHitKind::Module && hit.side == DockSide::Right && hit.targetModuleId == "filter",
               "module inner right strip must return Module+filter+Right")) {
        return 1;
    }
    if (!Check(hit.rootRegion == DockSide::Left, "filter module lives in the left root region")) return 1;

    DockModuleTarget docked[2] = {
        {"filter", {0.0f, 80.0f, 400.0f, 900.0f}},
        {"info", {1520.0f, 80.0f, 400.0f, 900.0f}},
    };
    const DockRect overInfoLeft{1520.0f, 200.0f, 80.0f, 200.0f};
    hit = HitDockTarget(overInfoLeft, mainWork, snap, docked, 2);
    if (!Check(hit.kind == DockHitKind::Module && hit.side == DockSide::Left && hit.targetModuleId == "info",
               "two docked modules: drag into right module left inner strip must be Module+info+Left")) {
        return 1;
    }

    DockHit hitNone;
    DockPreviewPlan nonePlan = PlanDockPreview(hitNone, mainWork, 80.0f, 80.0f, docked, 2, 80.0f);
    if (!Check(nonePlan.stripCount == 0, "Active with no hit must draw zero strips")) return 1;

    hit.kind = DockHitKind::Module;
    hit.side = DockSide::Right;
    hit.targetModuleId = "info";
    DockPreviewPlan infoRight = PlanDockPreview(hit, mainWork, 80.0f, 80.0f, docked, 2, 80.0f);
    if (!Check(infoRight.stripCount == 1, "only the current module inner strip may be drawn")) return 1;
    if (!Check(infoRight.strips[0].moduleId == "info", "preview strip must belong to info")) return 1;
    if (!Check(infoRight.strips[0].side == DockSide::Right, "preview strip must be the right inner edge")) return 1;
    if (!Check(!infoRight.includesMainFrame, "must not draw the main four-edge wall")) return 1;

    hit = HitDockTarget(insideLeft, mainWork, snap);
    DockPreviewPlan leftPlan = PlanDockPreview(hit, mainWork, 80.0f, 80.0f, nullptr, 0, snap);
    if (!Check(leftPlan.stripCount == 1, "left preview must draw the hit strip")) return 1;
    if (!Check(hit.hitRect.width > 0.0f && hit.hitRect.height > 0.0f,
               "DockHit must carry the tested strip, not a second geometry")) {
        return 1;
    }
    if (!Check(SameDockRect(leftPlan.strips[0].rect, hit.resultRect),
               "preview must draw the occupied split leaf")) {
        return 1;
    }

    DockModuleTarget infoBox{"info", {1520.0f, 80.0f, 400.0f, 900.0f}};
    const DockRect overInfoLeftEdge{1520.0f, 200.0f, 80.0f, 200.0f};
    hit = HitDockTarget(overInfoLeftEdge, mainWork, snap, &infoBox, 1);
    if (!Check(hit.kind == DockHitKind::Module && hit.side == DockSide::Left && hit.targetModuleId == "info",
               "Wwise split starts from the existing panel inner edge")) {
        return 1;
    }
    const DockRect expectedIncoming = SplitIncomingRect(infoBox.rect, DockSide::Left, kDockIncomingShare);
    if (!Check(hit.resultRect.width == expectedIncoming.width &&
               hit.resultRect.height == expectedIncoming.height &&
               hit.resultRect.x == expectedIncoming.x,
               "commit result must be the split leaf, not the thin snap strip")) {
        return 1;
    }
    DockPreviewPlan splitPreview = PlanDockPreview(hit, mainWork, 80.0f, 80.0f, &infoBox, 1, snap);
    if (!Check(splitPreview.stripCount == 1, "split preview draws the leaf-edge strip")) return 1;
    if (!Check(SameDockRect(splitPreview.strips[0].rect, expectedIncoming),
               "preview covers the final incoming leaf")) {
        return 1;
    }
    if (!Check(SameDockRect(splitPreview.strips[0].rect, hit.resultRect),
               "preview rect equals the committed result")) {
        return 1;
    }
    if (!Check(std::fabs(splitPreview.strips[0].rect.height - infoBox.rect.height) <= 0.5f &&
               std::fabs(splitPreview.strips[0].rect.width - infoBox.rect.width * kDockIncomingShare) <= 0.5f,
               "edge preview is full leaf length, not a mouse-sized patch or full work area")) {
        return 1;
    }

    const DockRect filterLeaf{0.0f, 80.0f, 400.0f, 720.0f};
    const DockRect infoLeaf{1520.0f, 80.0f, 400.0f, 720.0f};
    const DockRect similarLeaf{400.0f, 800.0f, 1120.0f, 200.0f};
    DockRootSlots fullRowSlots{};
    fullRowSlots.left = filterLeaf;
    fullRowSlots.right = infoLeaf;
    fullRowSlots.bottom = {0.0f, 800.0f, 1920.0f, 200.0f};
    DockModuleTarget occupiedLeaves[3] = {
        {"filter", filterLeaf},
        {"info", infoLeaf},
        {"similar-sounds", similarLeaf},
    };
    const int occupiedCount = 3;
    const DockRect overSimilarTop{720.0f, 800.0f, 80.0f, 80.0f};
    hit = HitDockTarget(overSimilarTop, mainWork, snap, occupiedLeaves, occupiedCount, &fullRowSlots);
    if (!Check(hit.kind == DockHitKind::Module && hit.side == DockSide::Top && hit.targetModuleId == "similar-sounds",
               "occupied bottom leaf top edge must hit Module+similar-sounds+Top, not the full-row bottom slot")) {
        return 1;
    }
    const DockRect similarTopStrip = InnerStripRect(similarLeaf, DockSide::Top, snap);
    if (!Check(SameDockRect(hit.hitRect, similarTopStrip),
               "preview strip must equal the bottom leaf top edge at fixed bandwidth")) {
        return 1;
    }
    if (!Check(std::fabs(hit.hitRect.width - similarLeaf.width) <= 0.5f &&
               std::fabs(hit.hitRect.height - snap) <= 0.5f,
               "bottom leaf top preview width equals the leaf, not the full-row slot")) {
        return 1;
    }
    DockPreviewPlan similarTopPlan = PlanDockPreview(hit, mainWork, 80.0f, 80.0f, occupiedLeaves, occupiedCount, snap, &fullRowSlots);
    if (!Check(similarTopPlan.stripCount == 1 && SameDockRect(similarTopPlan.strips[0].rect, hit.resultRect),
               "drawn preview must stay on the bottom leaf top edge")) {
        return 1;
    }
    if (!Check(similarTopPlan.strips[0].rect.width < fullRowSlots.bottom.width - 1.0f,
               "preview must not paint the whole bottom row across filter/results/info")) {
        return 1;
    }

    const DockRect overOccupiedFilterRight{320.0f, 200.0f, 80.0f, 80.0f};
    hit = HitDockTarget(overOccupiedFilterRight, mainWork, snap, occupiedLeaves, occupiedCount, &fullRowSlots);
    if (!Check(hit.kind == DockHitKind::Module && hit.side == DockSide::Right && hit.targetModuleId == "filter",
               "occupied filter right edge must hit the filter leaf, not a root slot")) {
        return 1;
    }
    if (!Check(SameDockRect(hit.hitRect, InnerStripRect(filterLeaf, DockSide::Right, snap)),
               "filter right preview must equal the filter leaf right edge")) {
        return 1;
    }

    DockRootSlots movedSlots{};
    const DockRect dockOrigin{1520.0f, 80.0f, 400.0f, 720.0f};
    if (!Check(!HasMovedFromDockOrigin(dockOrigin, dockOrigin),
               "fresh float at former leaf must not be treated as a moved drop")) return 1;
    if (!Check(HasMovedFromDockOrigin({1528.0f, 86.0f, 400.0f, 720.0f}, dockOrigin),
               "an 8px motion must leave the former leaf before drop evaluation")) return 1;
    if (!Check(HasMovedFromDockOrigin({1550.0f, 80.0f, 400.0f, 720.0f}, dockOrigin),
               "deliberate motion away from former leaf may evaluate a drop")) return 1;
    if (!Check(!ShouldEvaluateFloatingDockZone(true, false, true, false),
               "a floating panel must not evaluate a drop before leaving its origin")) return 1;
    if (!Check(ShouldEvaluateFloatingDockZone(true, false, true, true),
               "a floating panel may evaluate a drop after leaving its origin")) return 1;

    DockRootSlots emptyBottom{};
    emptyBottom.bottom = {0.0f, 800.0f, 1920.0f, 200.0f};
    const DockRect overEmptyBottom{720.0f, 920.0f, 80.0f, 80.0f};
    hit = HitDockTarget(overEmptyBottom, mainWork, snap, nullptr, 0, &emptyBottom);
    if (!Check(hit.kind == DockHitKind::Main && hit.side == DockSide::Bottom,
               "empty bottom slot without a leaf may still snap along that empty slot")) {
        return 1;
    }

    // A results-surface return target must never make its middle magnetic.
    // Only the remembered edge is a route back into the same Dock tree.
    {
        DockLayout emptyLayout;
        DockRootSlots emptyLayoutSlots{};
        emptyLayoutSlots.left = {0.0f, 80.0f, 400.0f, 720.0f};
        emptyLayoutSlots.right = {1520.0f, 80.0f, 400.0f, 720.0f};
        const DockCenterDropTarget centerDrop{
            {400.0f, 80.0f, 1120.0f, 720.0f}, DockSide::Left, true};
        const DockRect draggedOverCenter{800.0f, 350.0f, 1.0f, 1.0f};
        hit = HitDockTarget(draggedOverCenter, mainWork, snap, nullptr, 0,
                            &emptyLayoutSlots, &emptyLayout, &centerDrop);
        if (!Check(hit.kind == DockHitKind::None,
                   "dragging over the results middle must remain floating")) {
            return 1;
        }
        hit = HitDockTarget({402.0f, 350.0f, 1.0f, 1.0f}, mainWork, snap, nullptr, 0,
                            &emptyLayoutSlots, &emptyLayout, &centerDrop);
        if (!Check(hit.kind == DockHitKind::Main && hit.side == DockSide::Left,
                   "all-floating layout must still accept its remembered edge")) return 1;
        const DockDropPlan centerPlan = PlanReadableDockDrop(
            emptyLayout, hit, "filter", mainWork, 80.0f, 40.0f,
            emptyLayoutSlots, 1.0f, 520.0f);
        if (!Check(centerPlan.accepted && centerPlan.rootRect.width > 1.0f &&
                   centerPlan.rootRect.height > 1.0f,
                   "center result drop must produce a readable Dock leaf")) {
            return 1;
        }
    }

    // Every return direction uses only its own edge, never the broad center
    // or another direction. Use an offset surface to exercise viewport coords.
    {
        const DockRect work{-300.0f, 20.0f, 1920.0f, 1080.0f};
        const DockRect surface{-100.0f, 100.0f, 1320.0f, 720.0f};
        DockLayout empty;
        DockRootSlots slots{};
        for (DockSide side : {DockSide::Left, DockSide::Right, DockSide::Top, DockSide::Bottom}) {
            const DockCenterDropTarget target{surface, side, true, 320.0f};
            for (int x = 1; x < 5; ++x) {
                for (int y = 1; y < 5; ++y) {
                    const DockRect pointer{surface.x + surface.width * x / 5.0f,
                        surface.y + surface.height * y / 5.0f, 1.0f, 1.0f};
                    if (!Check(HitDockTarget(pointer, work, kDockSnapDistance,
                            nullptr, 0, &slots, &empty, &target).kind == DockHitKind::None,
                            "interior pointer grid must never dock in a remembered direction")) return 1;
                }
            }
            DockRect edge{surface.x + surface.width * .5f, surface.y + surface.height * .5f, 1, 1};
            if (side == DockSide::Left) edge.x = surface.x + 2;
            if (side == DockSide::Right) edge.x = surface.x + surface.width - 2;
            if (side == DockSide::Top) edge.y = surface.y + 2;
            if (side == DockSide::Bottom) edge.y = surface.y + surface.height - 2;
            hit = HitDockTarget(edge, work, kDockSnapDistance, nullptr, 0, &slots, &empty, &target);
            if (!Check(hit.kind == DockHitKind::Main && hit.side == side &&
                       hit.preferredRootExtent == 320.0f &&
                       SameDockRect(hit.hitRect, InnerStripRect(surface, side, kDockSnapDistance)),
                       "remembered edge must expose only its narrow hit band and retain its size")) return 1;
            DockRect away = edge;
            if (side == DockSide::Left) away.x = surface.x + kDockSnapDistance + 1;
            if (side == DockSide::Right) away.x = surface.x + surface.width - kDockSnapDistance - 2;
            if (side == DockSide::Top) away.y = surface.y + kDockSnapDistance + 1;
            if (side == DockSide::Bottom) away.y = surface.y + surface.height - kDockSnapDistance - 2;
            if (!Check(HitDockTarget(away, work, kDockSnapDistance, nullptr, 0,
                       &slots, &empty, &target).kind == DockHitKind::None,
                       "leaving the edge band must immediately remove the drop target")) return 1;
            DockCenterDropTarget disabled = target;
            disabled.enabled = false;
            if (!Check(HitDockTarget(edge, work, kDockSnapDistance, nullptr, 0,
                       &slots, &empty, &disabled).kind == DockHitKind::None,
                       "disabled return targets must remain disabled")) return 1;
            DockLayout occupied;
            occupied.InsertModule("filter", side, .24f);
            if (!Check(HitDockTarget(edge, work, kDockSnapDistance, nullptr, 0,
                       &slots, &occupied, &target).kind == DockHitKind::None,
                       "results edge must not insert into an occupied root behind its leaf")) return 1;
        }
    }

    // Portrait users can dock above/below the results, regardless of a
    // floating panel's remembered left/right side. The waveform is excluded.
    {
        DockLayout layout;
        layout.InsertModule("filter", DockSide::Left, .24f);
        const DockRect work{30, 40, 1080, 1600};
        const DockRect surface{342, 120, 768, 1180};
        const DockCenterDropTarget target{surface, DockSide::Right, true, 360};
        DockRootSlots slots{};
        slots.left = {30, 120, 300, 1180};
        slots.right = {750, 120, 360, 1180};
        slots.top = {342, 120, 768, 220};
        slots.bottom = {342, 1080, 768, 220};
        for (DockSide side : {DockSide::Top, DockSide::Bottom}) {
            const DockRect pointer{700, side == DockSide::Top ? 122.0f : 1298.0f, 1, 1};
            hit = HitDockTarget(pointer, work, kDockSnapDistance, nullptr, 0, &slots, &layout, &target);
            if (!Check(hit.kind == DockHitKind::Main && hit.side == side,
                       "results top/bottom edges must accept a formerly right-docked panel")) return 1;
            hit.preferredRootExtent = 220;
            const auto plan = PlanReadableDockDrop(layout, hit, "info", work, 80, 0, slots, 1, 520);
            if (!Check(plan.accepted && plan.rootRect.x == surface.x &&
                       plan.rootRect.width == surface.width && plan.rootRect.height == 220 &&
                       plan.rootRect.y == (side == DockSide::Top ? 120.0f : 1080.0f),
                       "vertical preview must stay inside the results column above the waveform")) return 1;
            if (!Check(SameDockRect(plan.slots.left, slots.left),
                       "vertical docking must preserve the full-height sidebar")) return 1;
        }
        for (float y : {80.0f, 700.0f, 1400.0f}) {
            if (!Check(HitDockTarget({700, y, 1, 1}, work, kDockSnapDistance, nullptr, 0,
                       &slots, &layout, &target).kind == DockHitKind::None,
                       "toolbar, results middle and waveform must not become vertical dock targets")) return 1;
        }
        DockHit bottom;
        bottom.kind = DockHitKind::Main;
        bottom.side = bottom.rootRegion = DockSide::Bottom;
        bottom.preferredRootExtent = 900;
        DockRootSlots shortSlots = slots;
        shortSlots.left.height = shortSlots.right.height = 350;
        shortSlots.bottom.y = 250;
        shortSlots.bottom.height = 220;
        const auto rejected = PlanReadableDockDrop(layout, bottom, "info",
            {30,40,1080,720}, 80, 0, shortSlots, 1, 520);
        if (!Check(!rejected.accepted && rejected.tooSmall,
                   "a vertical drop must leave 200px for the results or be rejected")) return 1;
    }

    // A vertical preview cannot borrow width that the central renderer lacks.
    for (const char* incoming : {"similar-sounds", "mod-workflow"}) {
        DockLayout narrow;
        narrow.InsertModule("filter", DockSide::Left, .24f);
        DockRootSlots slots{};
        slots.left = {0,80,300,900};
        slots.top = {312,80,388,220};
        slots.bottom = {312,760,388,220};
        for (auto side : {DockSide::Top, DockSide::Bottom}) {
            DockHit drop;
            drop.kind=DockHitKind::Main; drop.side=drop.rootRegion=side;
            drop.preferredRootExtent=220;
            const auto plan=PlanReadableDockDrop(narrow,drop,incoming,{0,0,700,1200},80,0,slots);
            if (!Check(!plan.accepted && plan.tooSmall,
                       "narrow central column rejects a wider vertical module")) return 1;
        }
    }

    // Regression: a real workspace keeps the right utility root at 720px and
    // may report a 24% fallback for an empty left root while the filter is
    // floating. Re-docking the filter must use its remembered 300px column
    // extent; otherwise the center readable-width gate rejects the drop before
    // the user can see a preview.
    {
        DockLayout rightLayout;
        rightLayout.InsertModule("info", DockSide::Right, 0.28f);
        rightLayout.InsertAdjacent("info", "similar-sounds", DockSide::Bottom, 0.5f);
        DockRootSlots realWorkspaceSlots{};
        realWorkspaceSlots.left = {0.0f, 80.0f, 384.0f, 720.0f};
        realWorkspaceSlots.right = {880.0f, 80.0f, 720.0f, 720.0f};
        const DockCenterDropTarget rememberedFilterDrop{
            {0.0f, 80.0f, 868.0f, 720.0f}, DockSide::Left, true, 300.0f};
        const DockRect draggedFilter{2.0f, 300.0f, 1.0f, 1.0f};
        hit = HitDockTarget(draggedFilter, {0.0f, 0.0f, 1600.0f, 900.0f}, snap,
                            nullptr, 0, &realWorkspaceSlots, &rightLayout,
                            &rememberedFilterDrop);
        const DockDropPlan rememberedPlan = PlanReadableDockDrop(
            rightLayout, hit, "filter", {0.0f, 0.0f, 1600.0f, 900.0f},
            80.0f, 40.0f, realWorkspaceSlots, 1.0f, 520.0f);
        if (!Check(rememberedPlan.accepted && !rememberedPlan.tooSmall &&
                   rememberedPlan.rootRect.width >= 299.0f &&
                   rememberedPlan.rootRect.width <= 301.0f,
                   "edge re-dock must preserve the floating filter's remembered 300px root extent")) {
            return 1;
        }
    }

    // Every existing leaf exposes all four split directions. If a horizontal
    // split would make the current root too narrow, the root grows before the
    // candidate is measured instead of silently rejecting Left/Right.
    {
        const DockSide sides[] = {
            DockSide::Left, DockSide::Right, DockSide::Top, DockSide::Bottom};
        for (DockSide side : sides) {
            DockLayout narrowLeft;
            if (!narrowLeft.InsertModule("filter", DockSide::Left, 0.24f)) return 1;
            DockRootSlots narrowSlots{};
            narrowSlots.left = {0.0f, 80.0f, 300.0f, 900.0f};
            DockModuleTarget filterLeaf{"filter", narrowSlots.left};
            DockRect probe{150.0f, 500.0f, 4.0f, 4.0f};
            if (side == DockSide::Left) probe = {2.0f, 500.0f, 4.0f, 4.0f};
            if (side == DockSide::Right) probe = {298.0f, 500.0f, 4.0f, 4.0f};
            if (side == DockSide::Top) probe = {150.0f, 82.0f, 4.0f, 4.0f};
            if (side == DockSide::Bottom) probe = {150.0f, 978.0f, 4.0f, 4.0f};
            const DockHit candidate = HitDockTarget(
                probe, mainWork, snap, &filterLeaf, 1, &narrowSlots, &narrowLeft);
            if (!Check(candidate.kind == DockHitKind::Module && candidate.side == side,
                       "each existing leaf must expose all four split directions")) return 1;
            const DockDropPlan plan = PlanReadableDockDrop(
                narrowLeft, candidate, "info", mainWork, 80.0f, 40.0f,
                narrowSlots, 1.0f, 520.0f);
            if (!Check(plan.accepted && !plan.tooSmall,
                       "a readable four-side split must be accepted")) return 1;
            if ((side == DockSide::Left || side == DockSide::Right) &&
                !Check(plan.rootRect.width >= 599.0f,
                       "horizontal split must grow a narrow existing root")) return 1;
        }
    }

    // When both side roots are occupied, a four-way split on the narrow left
    // root may need to reclaim remembered width from the opposite root. The
    // opposite tree must stay above its own floor while the center remains
    // readable; rejecting the drop here was the runtime failure seen by QA.
    {
        DockLayout bothRoots;
        if (!bothRoots.InsertModule("filter", DockSide::Left, 0.24f)) return 1;
        if (!bothRoots.InsertModule("info", DockSide::Right, 0.28f)) return 1;
        if (!bothRoots.InsertAdjacent("info", "similar-sounds", DockSide::Bottom, 0.5f)) return 1;
        const DockRect bothWork{294.0f, 0.0f, 1584.0f, 923.0f};
        DockRootSlots bothSlots{};
        bothSlots.left = {294.0f, 379.0f, 300.0f, 544.0f};
        bothSlots.right = {1158.0f, 379.0f, 720.0f, 544.0f};
        DockModuleTarget filterLeaf{"filter", bothSlots.left};
        const DockHit rightEdge = HitDockTarget(
            {570.0f, 640.0f, 4.0f, 4.0f}, bothWork, 80.0f,
            &filterLeaf, 1, &bothSlots, &bothRoots);
        const DockDropPlan plan = PlanReadableDockDrop(
            bothRoots, rightEdge, "info", bothWork, 80.0f, 40.0f,
            bothSlots, 1.0f, 520.0f);
        if (!Check(plan.accepted && !plan.tooSmall &&
                   plan.slots.left.width >= 599.0f &&
                   plan.slots.right.width >= 419.0f &&
                   plan.slots.right.width < 720.0f,
                   "a narrow root must reclaim spare width from the opposite root")) return 1;
    }

    // After the first split, the resulting leaves must remain recursive Dock
    // targets in all four directions as well.
    {
        const DockSide sides[] = {
            DockSide::Left, DockSide::Right, DockSide::Top, DockSide::Bottom};
        const DockRect recursiveWork{0.0f, 0.0f, 2560.0f, 1080.0f};
        for (DockSide side : sides) {
            DockLayout splitLayout;
            if (!splitLayout.InsertModule("filter", DockSide::Left, 0.5f)) return 1;
            if (!splitLayout.InsertAdjacent("filter", "info", DockSide::Right, 0.5f)) return 1;
            DockRootSlots splitSlots{};
            splitSlots.left = {0.0f, 80.0f, 600.0f, 900.0f};
            DockModuleTarget leaves[4]{};
            const int leafCount = CollectLeafRects(
                splitLayout.RegionRoot(DockSide::Left), splitSlots.left, leaves, 4, 0);
            const DockModuleTarget* infoLeaf = nullptr;
            for (int i = 0; i < leafCount; ++i)
                if (leaves[i].id == "info") infoLeaf = &leaves[i];
            if (!Check(infoLeaf != nullptr, "first split must leave a real info leaf")) return 1;
            DockRect probe{infoLeaf->rect.x + infoLeaf->rect.width * 0.5f,
                           infoLeaf->rect.y + infoLeaf->rect.height * 0.5f, 4.0f, 4.0f};
            if (side == DockSide::Left)
                probe.x = infoLeaf->rect.x + 2.0f;
            if (side == DockSide::Right)
                probe.x = infoLeaf->rect.x + infoLeaf->rect.width - 2.0f;
            if (side == DockSide::Top)
                probe.y = infoLeaf->rect.y + 2.0f;
            if (side == DockSide::Bottom)
                probe.y = infoLeaf->rect.y + infoLeaf->rect.height - 2.0f;
            const DockHit candidate = HitDockTarget(
                probe, recursiveWork, snap, leaves, leafCount, &splitSlots, &splitLayout);
            if (!Check(candidate.kind == DockHitKind::Module &&
                       candidate.targetModuleId == "info" && candidate.side == side,
                       "a split leaf must remain a four-side Dock target")) return 1;
            const DockDropPlan plan = PlanReadableDockDrop(
                splitLayout, candidate, "similar-sounds", recursiveWork, 80.0f, 40.0f,
                splitSlots, 1.0f, 520.0f);
            if (!Check(plan.accepted && !plan.tooSmall,
                       "a split leaf must accept another readable split")) return 1;
        }
    }

    // Empty Top region and the main waveform band must not be dock drop
    // targets when the real layout is provided. Central test panels remain
    // valid module targets.
    {
        DockLayout layout;
        if (!layout.InsertModule("filter", DockSide::Left, 0.24f)) return 1;
        DockRootSlots slots{};
        slots.top = {0.0f, 80.0f, 1920.0f, 120.0f};
        slots.bottom = {0.0f, 800.0f, 1920.0f, 200.0f};
        const DockRect overEmptyTop{960.0f, 20.0f, 80.0f, 60.0f};
        hit = HitDockTarget(overEmptyTop, mainWork, snap, nullptr, 0, &slots, &layout);
        if (!Check(hit.kind == DockHitKind::None,
                   "empty top region must not be a dock drop target")) return 1;
        const DockRect overWaveform{960.0f, 900.0f, 80.0f, 80.0f};
        hit = HitDockTarget(overWaveform, mainWork, snap, nullptr, 0, &slots, &layout);
        if (!Check(hit.kind == DockHitKind::None,
                   "main waveform band must not be a dock drop target")) return 1;

        DockLayout testCenter;
        if (!testCenter.InsertModule("mod-docktest-similar", DockSide::Right, 0.5f)) return 1;
        DockRootSlots testSlots{};
        testSlots.right = {1520.0f, 80.0f, 400.0f, 720.0f};
        DockModuleTarget testLeaves[4]{};
        const int testCount = CollectLeafRects(
            testCenter.RegionRoot(DockSide::Right), testSlots.right,
            testLeaves, 4, 0);
        const DockRect overTestPanel{1560.0f, 200.0f, 60.0f, 60.0f};
        hit = HitDockTarget(overTestPanel, mainWork, snap, testLeaves, testCount,
                            &testSlots, &testCenter);
        if (!Check(hit.kind == DockHitKind::Module &&
                   hit.targetModuleId == "mod-docktest-similar",
                   "central test panel must remain a dock drop target")) return 1;
    }

    // The read-only Dock Band overlay must show only targets accepted by
    // the same production hit test used for actual drops.
    {
        DockLayout hintLayout;
        if (!hintLayout.InsertModule("similar-sounds", DockSide::Right, 0.5f)) return 1;
        DockRootSlots hintSlots{};
        hintSlots.right = {1520.0f, 80.0f, 400.0f, 720.0f};
        hintSlots.left = {0.0f, 80.0f, 400.0f, 720.0f};
        hintSlots.top = {0.0f, 80.0f, 1920.0f, 120.0f};
        hintSlots.bottom = {0.0f, 800.0f, 1920.0f, 200.0f};
        const DockFrameGeometry frame = BuildDockFrameGeometry(
            hintLayout, mainWork, 80.0f, 80.0f, hintSlots);
        DockHint hints[64]{};
        const int hintCount = CollectLegalDockHints(
            hintLayout, mainWork, snap, frame, hints, 64);
        if (!Check(hintCount > 0, "real Similar Sounds leaf has legal dock hints")) return 1;
        for (int i = 0; i < hintCount; ++i) {
            const DockHit accepted = HitDockTarget(
                hints[i].probe, mainWork, snap, frame.modules, frame.moduleCount,
                &frame.rootSlots, &hintLayout);
            if (!Check(accepted.kind == hints[i].hit.kind &&
                       accepted.side == hints[i].hit.side &&
                       accepted.targetModuleId == hints[i].hit.targetModuleId,
                       "every hint is an accepted actual drop")) return 1;
            if (RectsOverlap(hints[i].hit.hitRect, hintSlots.bottom)) {
                std::printf("hint %d id=%s side=%d rect=%.1f,%.1f,%.1f,%.1f bottom=%.1f,%.1f,%.1f,%.1f\n",
                    i, hints[i].hit.targetModuleId.c_str(), static_cast<int>(hints[i].hit.side),
                    hints[i].hit.hitRect.x, hints[i].hit.hitRect.y, hints[i].hit.hitRect.width, hints[i].hit.hitRect.height,
                    hintSlots.bottom.x, hintSlots.bottom.y, hintSlots.bottom.width, hintSlots.bottom.height);
                return 1;
            }
        }
        // A short split leaf can have overlapping 80px edge bands. Its
        // bottom edge is still legal near the boundary even when the strip
        // midpoint resolves to Top; the overlay must include that drop.
        DockLayout shortSplit;
        if (!shortSplit.InsertModule("similar-sounds", DockSide::Right, 0.5f)) return 1;
        if (!shortSplit.InsertAdjacent("similar-sounds", "info", DockSide::Bottom, 0.5f)) return 1;
        DockRootSlots shortSlots = hintSlots;
        shortSlots.right.height = 220.0f;
        const DockFrameGeometry shortFrame = BuildDockFrameGeometry(
            shortSplit, mainWork, 80.0f, 80.0f, shortSlots);
        const DockModuleTarget* upper = nullptr;
        for (int i = 0; i < shortFrame.moduleCount; ++i)
            if (shortFrame.modules[i].id == "similar-sounds") upper = &shortFrame.modules[i];
        if (!Check(upper != nullptr, "short split Similar leaf exists")) return 1;
        const DockRect bottomProbe{upper->rect.x + upper->rect.width * 0.5f - 2.0f,
            upper->rect.y + upper->rect.height - 10.0f, 4.0f, 4.0f};
        const DockHit bottomHit = HitDockTarget(bottomProbe, mainWork, snap,
            shortFrame.modules, shortFrame.moduleCount, &shortFrame.rootSlots, &shortSplit);
        if (!Check(bottomHit.kind == DockHitKind::Module &&
                   bottomHit.targetModuleId == "similar-sounds" && bottomHit.side == DockSide::Bottom,
                   "short Similar bottom edge is a real drop")) return 1;
        const int shortHintCount = CollectLegalDockHints(
            shortSplit, mainWork, snap, shortFrame, hints, 64);
        for (int i = 0; i < shortHintCount; ++i) {
            const DockRect& shown = hints[i].visibleRect;
            for (float along : {0.2f, 0.5f, 0.8f}) {
                const DockRect sample{shown.x + shown.width * along - 2.0f,
                    shown.y + shown.height * along - 2.0f, 4.0f, 4.0f};
                const DockHit actual = HitDockTarget(sample, mainWork, snap,
                    shortFrame.modules, shortFrame.moduleCount, &shortFrame.rootSlots, &shortSplit);
                if (!Check(actual.kind == hints[i].hit.kind &&
                           actual.side == hints[i].hit.side &&
                           actual.targetModuleId == hints[i].hit.targetModuleId,
                           "sampled highlight pixels must use the same legal Dock target")) return 1;
            }
        }
        bool bottomHintPresent = false;
        for (int i = 0; i < shortHintCount; ++i) {
            if (hints[i].hit.kind != DockHitKind::Module ||
                hints[i].hit.targetModuleId != "similar-sounds" ||
                hints[i].hit.side != DockSide::Bottom) continue;
            bottomHintPresent = true;
            if (!Check(hints[i].visibleRect.y >= upper->rect.y + snap - 0.5f,
                       "highlight excludes top-priority overlap on short bottom edge")) return 1;
        }
        if (!Check(bottomHintPresent, "overlay includes legal bottom edge of short split leaf")) return 1;
        DockLayout emptySides;
        const DockFrameGeometry emptyFrame = BuildDockFrameGeometry(
            emptySides, mainWork, 80.0f, 80.0f, hintSlots);
        const int emptyCount = CollectLegalDockHints(
            emptySides, mainWork, snap, emptyFrame, hints, 64);
        if (!Check(emptyCount == 2, "empty left and right are legal; empty top and waveform are not")) return 1;
        for (int i = 0; i < emptyCount; ++i) {
            if (!Check(hints[i].hit.kind == DockHitKind::Main &&
                       (hints[i].hit.side == DockSide::Left || hints[i].hit.side == DockSide::Right),
                       "empty root hint uses actual hit result")) return 1;
        }
    }

    // RED: after one Top/Bottom split, the second leaf's edge must remain a
    // real module target; the root slot must not swallow the next split.
    {
        DockLayout topSplit;
        if (!topSplit.InsertModule("filter", DockSide::Top, 0.5f)) return 1;
        if (!topSplit.InsertAdjacent("filter", "info", DockSide::Bottom, 0.5f)) return 1;
        DockRootSlots slots{};
        slots.top = {0.0f, 80.0f, 1600.0f, 180.0f};
        DockModuleTarget leaves[4]{};
        const int count = CollectLeafRects(topSplit.RegionRoot(DockSide::Top), slots.top, leaves, 4, 0);
        const DockModuleTarget* second = nullptr;
        for (int i = 0; i < count; ++i) if (leaves[i].id == "info") second = &leaves[i];
        if (!second) return 1;
        const float edge = std::min(snap, std::min(second->rect.width, second->rect.height));
        const DockRect panel{second->rect.x + 40.0f, second->rect.y + edge * 0.25f,
                             120.0f, std::max(40.0f, edge * 0.5f)};
        hit = HitDockTarget(panel, mainWork, snap, leaves, count, &slots, &topSplit);
        if (!Check(hit.kind == DockHitKind::Module && hit.side == DockSide::Top &&
                   hit.targetModuleId == "info",
                   "second Top split must target the leaf edge")) return 1;
    }

    return 0;
}
#endif
