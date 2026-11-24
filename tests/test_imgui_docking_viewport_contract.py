"""Viewport support and self-Dock host contract for floating panels."""

from __future__ import annotations

import unittest
import shutil
import subprocess
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
IMGUI_H = ROOT / "thirdparty" / "imgui" / "imgui.h"
WIN32_H = ROOT / "thirdparty" / "imgui" / "backends" / "imgui_impl_win32.h"
WIN32_CPP = ROOT / "thirdparty" / "imgui" / "backends" / "imgui_impl_win32.cpp"
GL_CPP = ROOT / "thirdparty" / "imgui" / "backends" / "imgui_impl_opengl3.cpp"
WINMAIN = ROOT / "src" / "WindowsMain.cpp"
WINMAIN_H = ROOT / "src" / "WindowsMain.h"
MAIN = ROOT / "src" / "main.cpp"
APPSTATE = ROOT / "src" / "AppState.h"
FLOATING = ROOT / "src" / "FloatingPanelLayout.h"
CMAKE = ROOT / "CMakeLists.txt"


def _read(path: Path) -> str:
    return path.read_text(encoding="utf-8")


class ImguiDockingViewportVendorContract(unittest.TestCase):
    def test_same_version_docking_headers_expose_viewport_symbols(self) -> None:
        header = _read(IMGUI_H)
        self.assertIn('#define IMGUI_VERSION       "1.92.6 WIP"', header)
        self.assertIn("#define IMGUI_VERSION_NUM   19259", header)
        self.assertIn("#define IMGUI_HAS_VIEWPORT", header)
        self.assertIn("#define IMGUI_HAS_DOCK", header)
        self.assertIn("ImGuiConfigFlags_ViewportsEnable", header)
        self.assertIn("ImGuiConfigFlags_DockingEnable", header)
        self.assertIn("Platform_CreateWindow", header)
        self.assertIn("UpdatePlatformWindows", header)
        self.assertIn("RenderPlatformWindowsDefault", header)
        self.assertIn("DestroyPlatformWindows", header)

    def test_win32_and_opengl_backends_register_official_platform_windows(self) -> None:
        win32_h = _read(WIN32_H)
        win32_cpp = _read(WIN32_CPP)
        gl_cpp = _read(GL_CPP)
        self.assertIn("ImGui_ImplWin32_InitForOpenGL", win32_h)
        self.assertIn("Multi-viewport support", win32_h)
        self.assertIn("ImGuiBackendFlags_PlatformHasViewports", win32_cpp)
        self.assertIn("platform_io.Platform_CreateWindow", win32_cpp)
        self.assertIn("ImGui_ImplWin32_InitMultiViewportSupport", win32_cpp)
        self.assertIn("ImGuiBackendFlags_RendererHasViewports", gl_cpp)
        self.assertIn("ImGui_ImplOpenGL3_InitMultiViewportSupport", gl_cpp)
        self.assertNotIn("CreateWindowW", gl_cpp)
        self.assertNotIn("CreateWindowExW", gl_cpp)


class ImguiDockingViewportHostContract(unittest.TestCase):
    def test_saved_detached_main_window_rejoins_host_without_disabling_panels(self) -> None:
        """Execute the product root-window setup against real ImGui and synthetic INI."""
        if not shutil.which("cmake"):
            self.skipTest("CMake is required for the native viewport regression")
        product = _read(MAIN)
        start = product.index("    ImGuiViewport* viewport = ImGui::GetMainViewport();",
                              product.index("// MAIN LAYOUT - classic library layout"))
        end = product.index("    // Initialize the first tab", start)
        root_setup = product[start:end]
        harness = r'''
#include "imgui.h"
#include "imgui_internal.h"
#include <cstdint>
#include <cstdio>
static int failures = 0;
static int rootPlatformCreates = 0;
static int panelPlatformCreates = 0;
static void Check(bool condition, const char* label) {
    if (!condition) { std::fprintf(stderr, "FAIL: %s\n", label); ++failures; }
}
static bool BeginTransientScrollbarWindow(const char* name, bool* open, ImGuiWindowFlags flags) {
    return ImGui::Begin(name, open, flags);
}
static void ProductRoot() {
    const struct { ImVec4 applicationSurface; } workspacePalette{ImVec4(0.05f,0.05f,0.05f,1)};
__ROOT_SETUP__
    Check(ImGui::GetWindowViewport()->ID == ImGui::GetMainViewport()->ID,
          "MainWindow must use the host viewport on every frame, including INI restore");
    ImGui::TextUnformatted("Synthetic library content");
    ImGui::End();
}
static void CreatePlatform(ImGuiViewport* viewport) {
    if (viewport->ID == ImHashStr("MainWindow")) ++rootPlatformCreates;
    else ++panelPlatformCreates;
    viewport->PlatformHandle = reinterpret_cast<void*>(static_cast<std::uintptr_t>(viewport->ID));
    viewport->PlatformUserData = viewport->PlatformHandle;
}
static void DestroyPlatform(ImGuiViewport* viewport) {
    viewport->PlatformHandle = nullptr;
    viewport->PlatformUserData = nullptr;
}
static void RunCase(bool restored) {
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.DisplaySize = ImVec2(1280, 720);
    io.DeltaTime = 1.0f / 60.0f;
    io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
    io.BackendFlags |= ImGuiBackendFlags_PlatformHasViewports | ImGuiBackendFlags_RendererHasViewports;
    unsigned char* pixels = nullptr; int width = 0, height = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    ImGuiPlatformIO& platform = ImGui::GetPlatformIO();
    platform.Platform_CreateWindow = CreatePlatform;
    platform.Platform_DestroyWindow = DestroyPlatform;
    platform.Platform_ShowWindow = [](ImGuiViewport*) {};
    platform.Platform_SetWindowPos = [](ImGuiViewport* v, ImVec2 p) { v->Pos = p; };
    platform.Platform_GetWindowPos = [](ImGuiViewport* v) { return v->Pos; };
    platform.Platform_SetWindowSize = [](ImGuiViewport* v, ImVec2 s) { v->Size = s; };
    platform.Platform_GetWindowSize = [](ImGuiViewport* v) { return v->Size; };
    platform.Platform_SetWindowFocus = [](ImGuiViewport*) {};
    platform.Platform_GetWindowFocus = [](ImGuiViewport*) { return false; };
    platform.Platform_GetWindowMinimized = [](ImGuiViewport*) { return false; };
    platform.Platform_SetWindowTitle = [](ImGuiViewport*, const char*) {};
    ImGuiPlatformMonitor monitor;
    monitor.MainPos = monitor.WorkPos = ImVec2(0, 0);
    monitor.MainSize = monitor.WorkSize = ImVec2(3840, 2160);
    monitor.DpiScale = 1.0f;
    platform.Monitors.push_back(monitor);
    ImGui::GetMainViewport()->PlatformHandle = reinterpret_cast<void*>(1);
    if (restored) {
        // Synthetic historical state. Never read or write a user's configuration.
        ImGui::LoadIniSettingsFromMemory(
            "[Window][MainWindow]\nViewportPos=0,23\nViewportId=0x72FC8CA3\n"
            "Size=2560,1369\nCollapsed=0\n");
    }
    for (int frame = 0; frame < 4; ++frame) {
        ImGui::GetMainViewport()->Size = frame < 2 ? ImVec2(1280,720) : ImVec2(1920,1080);
        ImGui::NewFrame();
        ProductRoot();
        ImGui::SetNextWindowPos(ImVec2(2500,100), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(320,240), ImGuiCond_Always);
        ImGui::Begin("Synthetic floating panel", nullptr, ImGuiWindowFlags_NoSavedSettings);
        Check(ImGui::GetWindowViewport()->ID != ImGui::GetMainViewport()->ID,
              "a floating panel outside the host must retain its independent viewport");
        ImGui::TextUnformatted("Synthetic floating panel content");
        ImGui::End();
        ImGui::Render();
        ImGui::UpdatePlatformWindows();
    }
    ImGui::DestroyPlatformWindows();
    ImGui::DestroyContext();
}
int main() {
    RunCase(false);
    RunCase(true);
    Check(rootPlatformCreates == 0, "root content must not create a second platform window");
    Check(panelPlatformCreates >= 2, "floating panel platform creation remains enabled");
    std::printf("rootPlatformCreates=%d panelPlatformCreates=%d failures=%d\n",
                rootPlatformCreates, panelPlatformCreates, failures);
    return failures ? 1 : 0;
}
'''.replace("__ROOT_SETUP__", root_setup)
        # Only this test's disposable directory is writable; ImGui INI/log IO is disabled.
        with tempfile.TemporaryDirectory(prefix="aris-root-viewport-") as scratch:
            folder = Path(scratch)
            (folder / "main.cpp").write_text(harness, encoding="utf-8")
            vendor = (ROOT / "thirdparty/imgui").as_posix()
            cmake = '\n'.join([
                'cmake_minimum_required(VERSION 3.20)',
                'project(RootViewportRegression LANGUAGES CXX)',
                'set(CMAKE_CXX_STANDARD 17)',
                'add_executable(root_viewport main.cpp',
                *(f'  "{vendor}/{name}"' for name in ("imgui.cpp", "imgui_draw.cpp", "imgui_tables.cpp", "imgui_widgets.cpp")),
                ')',
                f'target_include_directories(root_viewport PRIVATE "{vendor}")',
                'target_compile_definitions(root_viewport PRIVATE IMGUI_DISABLE_DEFAULT_IME_FUNCTIONS IMGUI_DISABLE_DEFAULT_CLIPBOARD_FUNCTIONS)',
                'if(MSVC)',
                '  target_compile_options(root_viewport PRIVATE /utf-8)',
                'endif()',
            ])
            (folder / "CMakeLists.txt").write_text(cmake, encoding="utf-8")
            for command in (["cmake", "-S", str(folder), "-B", str(folder / "build"), "-DCMAKE_BUILD_TYPE=Release"],
                            ["cmake", "--build", str(folder / "build"), "--config", "Release", "--parallel", "4"]):
                built = subprocess.run(command, cwd=folder, capture_output=True, text=True, timeout=180)
                self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
            candidates = [folder / "build/Release/root_viewport.exe",
                          folder / "build/root_viewport.exe", folder / "build/root_viewport"]
            executable = next(path for path in candidates if path.is_file())
            run = subprocess.run([str(executable)], cwd=folder, capture_output=True, text=True, timeout=30)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            print(run.stdout.strip())

    def test_windows_main_uses_custom_dock_and_viewports_without_native_dock(self) -> None:
        cmake = _read(CMAKE)
        source = _read(WINMAIN)
        main = _read(MAIN)
        self.assertIn("ARIS_ENABLE_IMGUI_VIEWPORTS", cmake)
        self.assertNotIn("ARIS_ENABLE_IMGUI_DOCKING", cmake)
        self.assertIn("ARIS_ENABLE_IMGUI_VIEWPORTS", source)
        self.assertNotIn("ARIS_ENABLE_IMGUI_DOCKING", source)
        self.assertIn("g_dockLayout", main)
        self.assertIn("ImGuiConfigFlags_ViewportsEnable", source)
        self.assertNotIn("ImGuiConfigFlags_DockingEnable", source)
        self.assertNotIn("ImGui::DockSpace", source)
        self.assertNotIn("ArisHostDockSpace", source)
        self.assertNotIn("DockBuilderDockWindow", source)
        self.assertNotIn("DockBuilderDockWindow", main)
        for module in ("filter", "info", "waveform"):
            self.assertNotIn(f'DockBuilderDockWindow("{module}"', source)
            self.assertNotIn(f"DockBuilderDockWindow(\"{module}\"", main)
        self.assertIn("ImGui_ImplWin32_InitForOpenGL", source)
        self.assertIn("ImGui::UpdatePlatformWindows()", source)
        self.assertIn("ImGui::RenderPlatformWindowsDefault()", source)
        self.assertIn("ImGui::DestroyPlatformWindows()", source)
        self.assertTrue(
            "wglShareLists" in source or "Hook_Renderer_CreateWindow" in source,
            "Win32+GL must use official shared-context viewport hooks",
        )
        self.assertIn("Renderer_CreateWindow", source)
        self.assertIn("wglMakeCurrent", source)
        self.assertIn("ResolveExtraViewportContext", source)
        self.assertIn("CanSwapViewportBuffers", source)
        self.assertIn("ViewportGlLifecycle.h", source)
        share_tail = source.split("wglShareLists", 1)[-1][:500]
        self.assertNotIn("data->hRC = g_hRC;", share_tail)
        self.assertIn("CanSwapViewportBuffers", source.split("Hook_Renderer_SwapBuffers", 1)[-1][:500])
        self.assertIn("TestViewportGlLifecycle", cmake)
        self.assertIn("test_viewport_gl_lifecycle.cpp", cmake)
        self.assertIn('BeginTransientScrollbarWindow("MainWindow"', main)
        main_flags = main.split('ImGuiWindowFlags windowFlags = ImGuiWindowFlags_NoTitleBar', 1)[1].split("BeginTransientScrollbarWindow(\"MainWindow\"", 1)[0]
        self.assertIn("ImGuiWindowFlags_NoDocking", main_flags)
        for title in ("##FloatingSidebar", "##FloatingFileInfo"):
            self.assertIn(title, main)
        self.assertNotIn("##FloatingWaveform", main)
        floating_host = main.split("auto beginFilterStyleFloatingWindow", 1)[1].split("auto renderFunctionDockedModule", 1)[0]
        self.assertIn("ImGuiWindowFlags_NoDocking", floating_host)
        self.assertIn("beginFilterStyleFloatingWindow(", main.split("##FloatingSidebar", 1)[0][-300:])
        self.assertIn("beginFilterStyleFloatingWindow(", main.split("##FloatingFileInfo", 1)[0][-300:])

    def test_hidden_main_window_does_not_freeze_extra_viewports(self) -> None:
        source = _read(WINMAIN)
        self.assertIn("HarkenSearchHasHostedPlatformViewports", source)
        hidden_idx = source.find("const bool mainWindowHidden = IsIconic(hwnd) || !Platform::WindowIsVisible(hwnd);")
        self.assertGreater(hidden_idx, 0)
        window = source[hidden_idx : hidden_idx + 900]
        self.assertIn("HarkenSearchHasHostedPlatformViewports", window)
        self.assertIn("continue", window)
        self.assertRegex(
            window,
            r"mainWindowHidden && !HarkenSearchHasHostedPlatformViewports\(\)",
        )

    def test_header_stays_sdk_independent(self) -> None:
        header = _read(WINMAIN_H)
        self.assertNotIn("#include <windows.h>", header)


class ImguiDockingViewportPanelContract(unittest.TestCase):
    def test_dockable_panels_keep_dock_entries_and_skip_per_frame_clamp(self) -> None:
        main = _read(MAIN)
        layout = _read(FLOATING)
        appstate = _read(APPSTATE)
        self.assertIn("ShouldClampFloatingPanelToWorkArea", layout)
        self.assertIn("wasDragging", layout)
        self.assertIn("displayMissing", layout)
        self.assertIn("monitorId", appstate)
        for name in ("panelSidebar", "panelFileInfo", "panelWaveform"):
            self.assertIn(name, main)
        self.assertIn("##FloatSidebarMenu", main)
        self.assertIn("##FloatFileInfoMenu", main)
        self.assertNotIn("##FloatWaveform", main)
        self.assertNotIn("##FloatWaveformMenu", main)
        self.assertNotIn("Panel_DockBottom", main)
        self.assertIn("DockSide::Bottom", main)
        self.assertIn("g_searchResultsDockPanel", main)
        self.assertIn("##FloatingSearchResults", main)
        self.assertIn("##DockedSearchResults", main)
        self.assertIn("renderResultsSurface", main)
        self.assertIn("g_dockTitleGesture.Advance(pointerHeld", main)
        self.assertIn("moduleDockedInRegion", main)
        self.assertIn("filterDockedInLeft", main)
        self.assertIn("infoDockedInRight", main)
        self.assertIn('leaf.id == "filter"', main)
        self.assertIn('leaf.id == "info"', main)
        self.assertIn("Panel_DockLeft", main)
        self.assertIn("Panel_DockRight", main)
        self.assertIn("ImGuiWindowFlags_NoSavedSettings", main)
        self.assertIn("ShouldClampFloatingPanelToWorkArea", main)
        helper = main.split("static bool ClampFloatingPanelToViewport(", 1)[1].split("static void StartBatchFeatureExtraction", 1)[0]
        self.assertIn("ShouldClampFloatingPanelToWorkArea", helper)
        self.assertIn("restoreClampPending", helper)
        sidebar_guard = 'if (state.panelSidebar.visible && state.panelSidebar.floating && !state.dockTabGroups.Find("filter"))'
        info_guard = 'if (state.panelFileInfo.visible && state.panelFileInfo.floating && !state.dockTabGroups.Find("info"))'
        sidebar = main.split(sidebar_guard, 1)[1].split(info_guard, 1)[0]
        info = main.split(info_guard, 1)[1].split("if (state.panelSimilarSounds.visible", 1)[0]
        for block in (sidebar, info):
            self.assertIn("ClampFloatingPanelToViewport(", block)
        self.assertNotIn("BeginStandaloneWindow", sidebar + info)

    def test_floating_panels_use_dock_drag_session_not_half_plane(self) -> None:
        main = _read(MAIN)
        self.assertIn("DockDragSession", main)
        self.assertIn("HitDockTarget", main)
        self.assertIn("aura::ui::dock::DockLayout", main)
        self.assertNotIn("winPos.x < mainWinPos.x + dockSnapDistance", main)
        self.assertNotIn("winPos.x + winSize.x > mainWinPos.x + mainWinSize.x - dockSnapDistance", main)
        detect = main.split("auto detectDockZone", 1)
        if len(detect) > 1:
            body = detect[1].split("auto drawDockPreview", 1)[0]
            self.assertNotIn("winPos.x <", body)
            self.assertIn("HitDockTarget", body)
            self.assertIn("DockModuleTarget", body)
            self.assertNotIn("DetectFloatingPanelDockZone", body)

    def test_grouped_utilities_propagate_content_close_state(self) -> None:
        main = _read(MAIN)
        body = main.split("const auto renderToolContent", 1)[1].split("const auto drawSimilarBodyPerimeter", 1)[0]
        for panel, flag, render in (
            ("panelUCSBrowser", "showUCSBrowser", "RenderUCSBrowserContent"),
            ("panelTagBrowser", "showTagBrowser", "RenderTagBrowserContent"),
            ("panelWorkflow", "showWorkflowDialog", "RenderWorkflowDialogContent"),
            ("panelTransfer", "showTransferDialog", "RenderTransferDialogContent"),
        ):
            self.assertIn(f"renderGroupedUtility(state.{panel}, state.{flag}, {render})", body)
        utility = main.split("const auto renderGroupedUtility", 1)[1].split("const auto renderToolContent", 1)[0]
        self.assertLess(utility.index("render(state)"), utility.index("if (!show)"))
        self.assertIn("panel.visible = false", utility)

    def test_similar_sounds_entry_reuses_existing_group(self) -> None:
        source = _read(MAIN.with_name("SimilarSoundsWindow.cpp"))
        opener = source.split("void OpenSimilarSoundsWindow(", 1)[1].split("void InvalidateSimilarSoundsWindow", 1)[0]
        group = opener.split('state.dockTabGroups.Find("similar-sounds")', 1)[1]
        self.assertIn('state.dockTabGroups.Select(group->id, "similar-sounds")', group)
        self.assertLess(group.index("return;"), group.index("state.panelSimilarSounds.floating = true"))

    def test_dock_preview_requires_title_bar_drag_and_draws_full_inner_strips(self) -> None:
        main = _read(MAIN)
        helper = main.split("auto driveFloatingDockDrag", 1)[1].split("Floating Sidebar", 1)[0]
        self.assertIn("dockDragLabel.Contains(ImGui::GetMousePos())", helper)
        self.assertIn("TitleBarRect", helper)
        self.assertIn("BeginDrag", helper)
        self.assertIn("g_dockTitleGesture.Advance(pointerHeld", helper)
        self.assertIn("step.cancelled", helper)
        self.assertIn("ShouldEvaluateFloatingDockZone", helper)
        self.assertIn("const aura::ui::dock::DockHit commitHit = zone", helper)
        self.assertNotIn("IsWindowFocused()", helper)
        self.assertNotIn("IsMouseDragging(0) && ImGui::IsWindowFocused()", helper)
        preview = main.split("auto drawDockPreview", 1)[1].split("auto driveFloatingDockDrag", 1)[0]
        self.assertIn("g_dockDrag.Active()", preview)
        self.assertIn("Preview()", preview)
        self.assertIn("DrawList", preview)
        self.assertNotIn("mainWinPos.x + 200", preview)
        self.assertNotIn("mainWinSize.x - 250", preview)
        self.assertIn("PlanDockPreview(g_dockDropPlan)", preview)
        self.assertIn("GetForegroundDrawList(ImGui::GetMainViewport())", preview)
        self.assertNotIn("rootSlots", preview)
        self.assertIn("ApplyDockedPanel", main)
        self.assertIn("FloatDockedPanel", main)
        self.assertIn("InsertModule", main)
        self.assertIn("Detach(", main)
        self.assertIn("EnsureFloat", main)

    def test_root_regions_cap_at_sixteen_and_native_caption_starts_drag(self) -> None:
        layout = _read(ROOT / "src" / "DockLayout.h")
        hit = _read(ROOT / "src" / "DockHitTest.h")
        winmain = _read(WINMAIN)
        caption = _read(ROOT / "src" / "ui" / "dock" / "DockCaptionWndProc.h")
        main = _read(MAIN)
        cmake = _read(CMAKE)
        self.assertIn("kMaxModulesPerRootRegion", layout)
        self.assertIn("16", layout)
        self.assertIn("InsertAdjacent", layout)
        self.assertIn("CountInRootRegion", layout)
        self.assertIn("DepthInRootRegion", layout)
        self.assertIn("targetModuleId", hit)
        self.assertIn("rootRegion", hit)
        self.assertIn("DockModuleTarget", hit)
        self.assertIn("ShouldBeginDockDragFromNativeCaption", winmain)
        self.assertIn("WM_NCLBUTTONDOWN", winmain)
        self.assertIn("HTCAPTION", winmain)
        self.assertIn("SubclassExtraViewportCaption", winmain)
        self.assertIn("SetWindowLongPtr", winmain)
        self.assertIn("DispatchDockCaptionWindowMessage", winmain)
        self.assertIn("SetCapture", caption)
        self.assertIn("HandleExtraViewportCaptionMessage", caption)
        self.assertIn("letDefWindowProcMoveWindow", caption)
        self.assertIn("ForwardDockPointerTermination", winmain)
        self.assertIn("NativeCaptionDragging()", winmain)
        self.assertIn("candidate.InsertAdjacent", hit)
        self.assertIn("targetModuleId", main)
        self.assertIn("collectDockedModuleTargets", main)
        self.assertNotIn("g_dockedModuleRects", main)
        self.assertIn("BuildDockFrameGeometry", main)
        self.assertIn("gripSnapshot.grips", main)
        self.assertIn("DockHitKind::Module", hit)
        preview = main.split("auto drawDockPreview", 1)[1].split("auto driveFloatingDockDrag", 1)[0]
        self.assertIn("g_dockDrag.Active()", preview)
        self.assertIn("PlanDockPreview(g_dockDropPlan)", preview)
        self.assertNotIn("rootSlots", preview)
        self.assertIn("DockRootSlots", main)
        hit_header = _read(ROOT / "src" / "DockHitTest.h")
        self.assertIn("struct DockRootSlots", hit_header)
        apply = main.split("static void ApplyDockedPanel(", 1)[1].split("static void FloatDockedPanel", 1)[0]
        self.assertIn("PlanReadableDockDrop", apply)
        self.assertIn("g_dockLayout = *plan.candidate", apply)
        self.assertIn("CommitOnMouseUp(g_dockDropPlan)", main)
        self.assertNotIn("InsertAdjacent", apply)
        self.assertIn("similar-sounds", _read(ROOT / "src" / "DockLayout.h"))
        similar = _read(ROOT / "src" / "SimilarSoundsWindow.cpp")
        self.assertIn("similar-sounds", similar)
        self.assertIn("driveFloatingDockDrag", similar)
        self.assertIn("ShouldEvaluateFloatingDockZone", main)
        self.assertIn("g_dockDrag.UpdatePreview(zone)", main)
        self.assertIn("ShouldDrawDockPreview", main)
        self.assertIn("EnsureFloat(\"similar-sounds\")", main)
        self.assertIn("TestDockNativeCaptionBridge", cmake)
        self.assertIn("test_dock_native_caption_bridge.cpp", cmake)

    def test_ctest_registers_this_contract(self) -> None:
        cmake = _read(CMAKE)
        self.assertIn("TestImguiDockingViewportContract", cmake)
        self.assertIn("test_imgui_docking_viewport_contract.py", cmake)
        self.assertIn("TestFloatingPanelClampPolicy", cmake)
        self.assertIn("test_floating_panel_clamp_policy.cpp", cmake)


if __name__ == "__main__":
    unittest.main()
