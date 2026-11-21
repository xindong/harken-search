#include "TransientScrollbarHost.h"
#include "ui/CoreWorkspaceStyle.h"
#include "ui/ModuleWindowShell.h"
#include "ui/ResultsPanelLayout.h"
#include "ui/TransportScale.h"
#include "ui/UiScale.h"

#include "imgui.h"
#include "imgui_internal.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <initializer_list>
#include <string>

namespace {

int g_failures = 0;

void Check(bool condition, const char* message) {
    if (condition) return;
    std::fprintf(stderr, "FAIL: %s\n", message);
    ++g_failures;
}

bool Near(float left, float right, float epsilon = 0.0001f) {
    return std::fabs(left - right) <= epsilon;
}

void TestNativeChromeDimensionsScaleTogether() {
    using namespace aura::ui::dpi;
    using namespace aura::ui::transport;

    Check(Near(ScalePx(24.0f, 1.0f), 24.0f), "100 percent keeps the original geometry");
    Check(Near(ScalePx(24.0f, 1.5f), 36.0f), "height scales at 150 percent");
    Check(Near(ScalePx(24.0f, 2.0f), 48.0f), "height scales at 200 percent");
    Check(Near(ScalePx(52.0f, 0.75f), 39.0f), "smaller layouts use the same proportion");
    Check(Near(ScalePx(24.0f, 0.0f), 24.0f), "invalid DPI uses the original size");
    Check(Near(ResolveEqualizerWidth(520.0f, 900.0f, 1.5f), 780.0f),
          "equalizer keeps the user's design width at 150 percent");
    Check(Near(ResolveEqualizerWidth(520.0f, 435.0f, 1.5f), 435.0f),
          "the equalizer respects its existing allocation");
    Check(Near(ReserveEqualizerSlope(148.5f, 337.5f, 67.5f, 1.5f), 187.5f),
          "space for the full slope label scales with the rail");
}

bool SameColor(const ImVec4& left, const ImVec4& right) {
    return Near(left.x, right.x) && Near(left.y, right.y) &&
           Near(left.z, right.z) && Near(left.w, right.w);
}

bool SameRgb(const ImVec4& left, const ImVec4& right) {
    return Near(left.x, right.x) && Near(left.y, right.y) && Near(left.z, right.z);
}

float SurfaceLuma(const ImVec4& color) {
    return color.x * 0.299f + color.y * 0.587f + color.z * 0.114f;
}

float MaxRgb(const ImVec4& color) {
    return (std::max)((std::max)(color.x, color.y), color.z);
}

aura::ui::workspace::CoreWorkspacePalette BuildTestPalette() {
    using namespace aura::ui::workspace;
    CoreWorkspacePaletteInput input;
    input.applicationSurface = ImVec4(0.01f, 0.02f, 0.03f, 1.0f);
    input.dockedSurface = ImVec4(0.11f, 0.12f, 0.13f, 1.0f);
    input.elevatedSurface = ImVec4(0.21f, 0.22f, 0.23f, 1.0f);
    input.perimeter = ImVec4(0.31f, 0.32f, 0.33f, 0.75f);
    input.splitter = ImVec4(0.41f, 0.42f, 0.43f, 0.65f);
    input.primaryText = ImVec4(0.91f, 0.92f, 0.93f, 1.0f);
    input.mutedText = ImVec4(0.61f, 0.62f, 0.63f, 1.0f);
    input.functionalAccent = ImVec4(0.10f, 0.80f, 0.90f, 1.0f);
    input.error = ImVec4(0.90f, 0.20f, 0.20f, 1.0f);
    return BuildCoreWorkspacePalette(input);
}

void TestInvariantMetrics() {
    using namespace aura::ui::workspace;
    Check(Near(kCoreWorkspaceMetrics.space4, 4.0f), "spacing rhythm starts at 4 px");
    Check(Near(kCoreWorkspaceMetrics.space8, 8.0f), "spacing rhythm includes 8 px");
    Check(Near(kCoreWorkspaceMetrics.space12, 12.0f), "spacing rhythm includes 12 px");
    Check(Near(kCoreWorkspaceMetrics.space16, 16.0f), "spacing rhythm includes 16 px");
    Check(Near(kCoreWorkspaceMetrics.perimeterWidth, 1.0f), "module perimeter is one pixel");
    Check(Near(kCoreWorkspaceMetrics.splitterWidth, 1.0f), "splitter line is one pixel");
    Check(Near(kCoreWorkspaceMetrics.compactControlHeight, 28.0f), "repeated controls stay compact");
    Check(Near(kCoreWorkspaceMetrics.compactControlRadius, 4.0f), "repeated controls share a restrained radius");
    Check(kSurfaceLevelCount == 3, "workspace exposes exactly three surface levels");
    Check(static_cast<int>(SurfaceLevel::Application) == 0 &&
              static_cast<int>(SurfaceLevel::Docked) == 1 &&
              static_cast<int>(SurfaceLevel::Elevated) == 2,
          "surface levels are application, docked, and elevated only");
}

void TestPaletteIsCallerSupplied() {
    using namespace aura::ui::workspace;
    const CoreWorkspacePalette palette = BuildTestPalette();

    Check(SameColor(ResolveSurfaceColor(palette, SurfaceLevel::Application),
                    ImVec4(0.01f, 0.02f, 0.03f, 1.0f)),
          "application surface comes from palette input");
    Check(SameColor(ResolveSurfaceColor(palette, SurfaceLevel::Docked),
                    ImVec4(0.11f, 0.12f, 0.13f, 1.0f)),
          "docked surface comes from palette input");
    Check(SameColor(ResolveSurfaceColor(palette, SurfaceLevel::Elevated),
                    ImVec4(0.21f, 0.22f, 0.23f, 1.0f)),
          "elevated surface comes from palette input");
    Check(SameColor(ResolveAccentColor(palette, AccentRole::None), palette.perimeter),
          "idle decoration does not consume the functional accent");
    Check(SameColor(ResolveAccentColor(palette, AccentRole::Selected), palette.functionalAccent),
          "selection may use the functional accent");
    Check(SameColor(ResolveAccentColor(palette, AccentRole::Focused), palette.functionalAccent),
          "focus may use the functional accent");
    Check(SameColor(ResolveAccentColor(palette, AccentRole::Playback), palette.functionalAccent),
          "playback may use the functional accent");
    Check(SameColor(ResolveAccentColor(palette, AccentRole::Active), palette.functionalAccent),
          "active state may use the functional accent");
    Check(!SameRgb(ResolveSplitterColor(palette, SplitterInteraction::Hovered),
                   palette.functionalAccent),
          "splitter hover remains neutral rather than consuming the functional accent");
    Check(SameColor(ResolveSplitterColor(palette, SplitterInteraction::Active),
                    palette.functionalAccent),
          "active splitter may use the functional accent");
}

void TestSurfaceHierarchyResolver() {
    using namespace aura::ui::workspace;

    WorkspaceSurfaceBasis dark;
    dark.application = ImVec4(0.059f, 0.078f, 0.106f, 1.0f);
    dark.frame = ImVec4(0.094f, 0.125f, 0.165f, 1.0f);
    dark.toolbar = ImVec4(0.063f, 0.082f, 0.110f, 1.0f);
    dark.elevated = ImVec4(0.082f, 0.110f, 0.145f, 1.0f);
    dark.lightChrome = false;
    const WorkspaceSurfaceLevels darkLevels = ResolveWorkspaceSurfaceLevels(dark);
    Check(SurfaceLuma(darkLevels.docked) - SurfaceLuma(darkLevels.application) >= 0.027f,
          "dark workspace keeps a readable docked lift");
    Check(SurfaceLuma(darkLevels.elevated) - SurfaceLuma(darkLevels.application) >= 0.050f,
          "dark workspace keeps a distinct elevated lift");
    Check(SurfaceLuma(darkLevels.elevated) > SurfaceLuma(darkLevels.docked),
          "dark workspace surface order remains application, docked, elevated");

    WorkspaceSurfaceBasis light;
    light.application = ImVec4(0.965f, 0.950f, 0.940f, 1.0f);
    light.frame = ImVec4(0.995f, 0.985f, 0.985f, 1.0f);
    light.toolbar = ImVec4(0.875f, 0.820f, 0.850f, 1.0f);
    light.elevated = ImVec4(0.985f, 0.965f, 0.970f, 1.0f);
    light.lightChrome = true;
    const WorkspaceSurfaceLevels lightLevels = ResolveWorkspaceSurfaceLevels(light);
    Check(SurfaceLuma(lightLevels.application) - SurfaceLuma(lightLevels.docked) >= 0.040f,
          "light workspace keeps a readable recessed docked surface");
    Check(SurfaceLuma(lightLevels.elevated) - SurfaceLuma(lightLevels.docked) >= 0.060f,
          "light workspace keeps an elevated surface above the dock");
    Check(SurfaceLuma(lightLevels.elevated) > SurfaceLuma(lightLevels.docked),
          "light workspace surface order remains application, docked, elevated");

    // These are the five built-in theme anchors from MakeBuiltinScheme. Keep
    // the resolver contract table-driven so a theme can not silently regress
    // to a nearly flat shell (or a clipped white elevated surface).
    const WorkspaceSurfaceBasis builtInThemes[] = {
        {
            ImVec4(0.059f, 0.078f, 0.106f, 1.0f),
            ImVec4(0.094f, 0.125f, 0.165f, 1.0f),
            ImVec4(0.063f, 0.082f, 0.110f, 1.0f),
            ImVec4(0.082f, 0.110f, 0.145f, 1.0f),
            false,
        }, // Classic Dark
        {
            ImVec4(0.078f, 0.082f, 0.086f, 1.0f),
            ImVec4(0.094f, 0.153f, 0.224f, 1.0f),
            ImVec4(0.043f, 0.075f, 0.122f, 1.0f),
            ImVec4(0.063f, 0.106f, 0.161f, 1.0f),
            false,
        }, // Aurora
        {
            ImVec4(0.965f, 0.929f, 0.945f, 1.0f),
            ImVec4(0.988f, 0.976f, 0.980f, 1.0f),
            ImVec4(0.918f, 0.859f, 0.886f, 1.0f),
            ImVec4(0.988f, 0.976f, 0.980f, 1.0f),
            true,
        }, // Soft Blush
        {
            ImVec4(0.055f, 0.086f, 0.129f, 1.0f),
            ImVec4(0.082f, 0.161f, 0.267f, 1.0f),
            ImVec4(0.082f, 0.161f, 0.267f, 1.0f),
            ImVec4(0.075f, 0.149f, 0.247f, 1.0f),
            false,
        }, // Deep Navy
        {
            ImVec4(0.992f, 0.984f, 0.969f, 1.0f),
            ImVec4(1.000f, 1.000f, 1.000f, 1.0f),
            ImVec4(1.000f, 0.933f, 0.831f, 1.0f),
            ImVec4(0.973f, 0.937f, 0.894f, 1.0f),
            true,
        }, // Sea Glass
    };
    for (const WorkspaceSurfaceBasis& builtIn : builtInThemes) {
        const WorkspaceSurfaceLevels levels = ResolveWorkspaceSurfaceLevels(builtIn);
        const float applicationLuma = SurfaceLuma(levels.application);
        const float dockedLuma = SurfaceLuma(levels.docked);
        const float elevatedLuma = SurfaceLuma(levels.elevated);
        if (builtIn.lightChrome) {
            Check(applicationLuma - dockedLuma >= 0.040f,
                  "built-in light theme keeps a recessed docked shelf");
            Check(elevatedLuma - dockedLuma >= 0.0595f,
                  "built-in light theme keeps a readable elevated shelf");
            Check(MaxRgb(levels.elevated) < 0.999f,
                  "built-in light theme avoids a clipped pure-white elevated shelf");
        } else {
            Check(dockedLuma - applicationLuma >= 0.0335f,
                  "built-in dark theme keeps a readable docked lift");
            Check(elevatedLuma - dockedLuma >= 0.0275f,
                  "built-in dark theme keeps a readable elevated lift");
        }
    }
}

bool SlotInside(
    const aura::ui::workspace::ToolbarFlowSlot& inner,
    const aura::ui::workspace::ToolbarFlowSlot& outer) {
    return !inner.visible ||
           (inner.minX >= outer.minX && inner.maxX <= outer.maxX && inner.maxX >= inner.minX);
}

void CheckToolbarFlowGeometry(
    const aura::ui::workspace::ToolbarFlowLayout& layout,
    float expectedDialGap,
    const char* caseName) {
    using namespace aura::ui::workspace;
    Check(layout.valid, caseName);
    Check(layout.presetDial.visible, "preset dial remains visible in toolbar flow");
    Check(SlotInside(layout.navCapsule, layout.toolbarClip),
          "navigation capsule stays inside toolbar clip");
    Check(SlotInside(layout.presetDial, layout.toolbarClip),
          "preset dial stays inside toolbar clip");
    Check(layout.navCapsule.maxX + expectedDialGap <= layout.presetDial.minX,
          "navigation capsule and preset dial keep the requested gap");

    float previousMaxX = layout.navCapsule.minX;
    for (std::size_t i = 0; i < kToolbarFlowItemCount; ++i) {
        const ToolbarFlowSlot& item = layout.itemSlots[i];
        Check(SlotInside(item, layout.toolbarClip),
              "every visible toolbar action stays inside the toolbar clip");
        Check(SlotInside(item, layout.navCapsule),
              "every visible toolbar action stays inside the navigation capsule");
        if (!item.visible) continue;
        Check(item.minX >= previousMaxX,
              "toolbar action rectangles never overlap each other");
        Check(item.maxX <= layout.presetDial.minX,
              "toolbar actions never overlap the preset dial");
        previousMaxX = item.maxX;
    }
}

void TestToolbarFlowLayoutKeepsPresetDialSeparate() {
    using namespace aura::ui::workspace;

    ToolbarFlowLayoutInput input;
    input.toolbarMinX = 10.0f;
    input.toolbarMaxX = 1590.0f;
    input.navMinX = 850.0f;
    input.navPreferredX = 865.0f;
    input.rightInset = 10.0f;
    input.presetDialWidth = 160.0f;
    input.presetDialGap = 12.0f;
    input.navLeadingInset = 3.0f;
    input.navTrailingInset = 3.0f;
    input.expandedItemWidths = {90.0f, 78.0f, 82.0f, 30.0f, 30.0f, 28.0f};
    input.compactItemWidths = {30.0f, 30.0f, 30.0f, 30.0f, 30.0f, 28.0f};
    input.itemGaps = {2.0f, 2.0f, 2.0f, 2.0f, 13.0f};
    input.minimumVisibleItems = 3;

    const ToolbarFlowLayout wide = ResolveToolbarFlowLayout(input);
    CheckToolbarFlowGeometry(wide, input.presetDialGap,
                             "1600 px toolbar resolves a valid navigation flow");
    Check(!wide.compact, "1600 px toolbar preserves navigation labels");
    Check(wide.visibleItemCount == kToolbarFlowItemCount,
          "1600 px toolbar keeps navigation and all actions visible");
    for (std::size_t actionIndex : {3u, 4u}) {
        const auto& slot = wide.itemSlots[actionIndex];
        Check(Near(slot.maxX - slot.minX, input.compactItemWidths[actionIndex]),
              "expanded navigation keeps the Scan and Refresh hit areas icon-sized");
    }

    input.toolbarMaxX = 1270.0f;
    input.navMinX = 650.0f;
    input.navPreferredX = 700.0f;
    const ToolbarFlowLayout standard = ResolveToolbarFlowLayout(input);
    CheckToolbarFlowGeometry(standard, input.presetDialGap,
                             "1280 px toolbar resolves a valid navigation flow");
    Check(!standard.compact, "1280 px toolbar keeps navigation labels after Favorites removal");
    Check(standard.visibleItemCount == kToolbarFlowItemCount,
          "1280 px toolbar keeps scan, refresh, and more actions visible");

    input.toolbarMinX = 0.0f;
    input.toolbarMaxX = 600.0f;
    input.navMinX = 213.0f;
    input.navPreferredX = 213.0f;
    const ToolbarFlowLayout narrow = ResolveToolbarFlowLayout(input);
    CheckToolbarFlowGeometry(narrow, input.presetDialGap,
                             "narrow toolbar resolves a valid navigation flow");
    Check(narrow.compact, "narrow toolbar uses icon-only navigation");
    Check(narrow.visibleItemCount == kToolbarFlowItemCount,
          "a tight compact fit keeps Scan, Refresh and rightmost Shuffle visible");
    Check(narrow.itemSlots[3].maxX < narrow.itemSlots[4].minX &&
          narrow.itemSlots[4].maxX < narrow.itemSlots[5].minX,
          "Scan, Refresh and Shuffle keep separate hit areas in compact mode");

    input.navMinX = 250.0f;
    const ToolbarFlowLayout overflow = ResolveToolbarFlowLayout(input);
    CheckToolbarFlowGeometry(overflow, input.presetDialGap,
                             "toolbar still fits when only five slots are available");
    Check(overflow.itemSlots[3].visible && overflow.itemSlots[4].visible &&
          !overflow.itemSlots[5].visible,
          "overflow drops rightmost Shuffle before either Scan or Refresh");

    // 132..164 px：五个图标放不下、四个刚好放得下。这里以前直接返回无效布局，
    // 整组导航（三个视图入口 + 扫描 + 刷新）全部消失。
    input.rightInset = 0.0f;
    input.presetDialWidth = 0.0f;
    input.presetDialGap = 0.0f;
    input.toolbarMaxX = 140.0f;
    input.navMinX = 0.0f;
    input.navPreferredX = 0.0f;
    const ToolbarFlowLayout iconBand = ResolveToolbarFlowLayout(input);
    Check(iconBand.valid && SlotInside(iconBand.navCapsule, iconBand.toolbarClip),
          "one-icon-short toolbar still resolves a navigation capsule");
    Check(iconBand.navCapsule.visible && iconBand.visibleItemCount == 4,
          "toolbar short of five icons keeps four instead of hiding navigation");
    Check(iconBand.itemSlots[3].visible && !iconBand.itemSlots[4].visible,
          "the missing icon is dropped from the right, not the whole group");

    // 再窄也只能让到三个视图入口。
    input.toolbarMaxX = 120.0f;
    const ToolbarFlowLayout viewsOnly = ResolveToolbarFlowLayout(input);
    Check(viewsOnly.valid && SlotInside(viewsOnly.navCapsule, viewsOnly.toolbarClip),
          "two-icon-short toolbar still resolves a navigation capsule");
    Check(viewsOnly.navCapsule.visible && viewsOnly.visibleItemCount == 3,
          "extreme width floor keeps the three view entries instead of an empty toolbar");
}

void TestTransientScrollbarVisibilityPolicy() {
    using namespace aura::ui::workspace;

    TransientScrollbarVisibilityInput input;
    Check(!ShouldShowTransientScrollbar(input),
          "non-scrollable surfaces never draw a scrollbar grab");

    input.scrollable = true;
    input.secondsSinceInteraction = 0.0;
    Check(ShouldShowTransientScrollbar(input),
          "an eligible scrollbar appears immediately after interaction");

    input.secondsSinceInteraction = kTransientScrollbarIdleSeconds + 0.01;
    Check(!ShouldShowTransientScrollbar(input),
          "an eligible scrollbar hides after three idle seconds");

    input.hovered = true;
    Check(ShouldShowTransientScrollbar(input),
          "hover reveals an eligible scrollbar");

    input.hovered = false;
    input.dragging = true;
    Check(ShouldShowTransientScrollbar(input),
          "dragging keeps an eligible scrollbar visible");
}

void TestTransientScrollbarControllerKeepsRegionClocksIndependent() {
    using namespace aura::ui::workspace;

    TransientScrollbarController controller;
    constexpr ImGuiID kResultsRegion = 0x1001;
    constexpr ImGuiID kRelatedPathsRegion = 0x1002;
    constexpr ImGuiID kWaveformNavigationRegion = 0x1003;

    TransientScrollbarFrameObservation resultsFrame;
    resultsFrame.nowSeconds = 10.0;
    resultsFrame.anyScrollable = true;
    resultsFrame.becameScrollable = true;
    controller.beginFrame(kResultsRegion, resultsFrame);

    Check(controller.shouldShow(kResultsRegion, 10.0, true),
          "Results becomes visible after its own interaction");
    Check(!controller.shouldShow(kRelatedPathsRegion, 10.0, true),
          "Results interaction does not reveal Related Paths");

    controller.reportInteraction(kRelatedPathsRegion, 11.0);
    Check(controller.shouldShow(kResultsRegion, 12.99, true),
          "Results keeps its original independent timeout");
    Check(controller.shouldShow(kRelatedPathsRegion, 13.01, true),
          "Related Paths remains visible on its newer timeout");
    Check(!controller.shouldShow(kResultsRegion, 13.01, true),
          "Results expires without waiting for Related Paths");
    Check(!controller.shouldShow(kRelatedPathsRegion, 14.01, true),
          "Related Paths expires three seconds after its own interaction");

    controller.reportInteraction(kWaveformNavigationRegion, 20.0);
    Check(controller.shouldShow(kWaveformNavigationRegion, 22.99, true),
          "waveform navigation has its own independent timeout");
    Check(!controller.shouldShow(kResultsRegion, 20.0, true),
          "waveform navigation does not reveal Results");
}

void TestTransientScrollbarStyleGatePreservesRgbAndStacks() {
    using namespace aura::ui::workspace;

    ImGui::CreateContext();
    ImGuiContext* context = ImGui::GetCurrentContext();
    ImVec4* colors = ImGui::GetStyle().Colors;
    const ImVec4 originalBg(0.12f, 0.13f, 0.14f, 0.25f);
    const ImVec4 originalGrab(0.21f, 0.31f, 0.41f, 0.40f);
    const ImVec4 originalHovered(0.22f, 0.42f, 0.62f, 0.60f);
    const ImVec4 originalActive(0.34f, 0.54f, 0.74f, 0.80f);
    colors[ImGuiCol_ScrollbarBg] = originalBg;
    colors[ImGuiCol_ScrollbarGrab] = originalGrab;
    colors[ImGuiCol_ScrollbarGrabHovered] = originalHovered;
    colors[ImGuiCol_ScrollbarGrabActive] = originalActive;
    const int colorsBefore = context->ColorStack.Size;

    {
        const ImVec4 semanticGrab(0.55f, 0.65f, 0.75f, 0.36f);
        const ImVec4 semanticHovered(0.56f, 0.66f, 0.76f, 0.54f);
        const ImVec4 semanticActive(0.20f, 0.70f, 0.80f, 0.70f);
        const int semanticColorCount = PushTransientScrollbarColors(
            semanticGrab, semanticHovered, semanticActive);
        Check(semanticColorCount == 4,
              "semantic scrollbar helper owns the transparent track and three grabs");
        Check(SameColor(colors[ImGuiCol_ScrollbarGrab], semanticGrab),
              "semantic scrollbar colors remain raw before a region gate");

        TransientScrollbarStyleScope hidden(false);
        Check(Near(colors[ImGuiCol_ScrollbarBg].w, 0.0f),
              "the native scrollbar track stays transparent while hidden");
        Check(SameRgb(colors[ImGuiCol_ScrollbarGrab], semanticGrab) &&
                  Near(colors[ImGuiCol_ScrollbarGrab].w, 0.0f),
              "a hidden region preserves its own semantic RGB");
        Check(SameRgb(colors[ImGuiCol_ScrollbarGrabHovered], semanticHovered) &&
                  Near(colors[ImGuiCol_ScrollbarGrabHovered].w, 0.0f),
              "a hidden region gates its own hover alpha");
        hidden.reset();
        hidden.reset();
        Check(SameColor(colors[ImGuiCol_ScrollbarGrab], semanticGrab),
              "resetting a region gate restores the semantic module color");
        ImGui::PopStyleColor(semanticColorCount);
    }

    Check(context->ColorStack.Size == colorsBefore,
          "hidden transient scrollbar scope balances its color stack");
    Check(SameColor(colors[ImGuiCol_ScrollbarBg], originalBg) &&
              SameColor(colors[ImGuiCol_ScrollbarGrab], originalGrab) &&
              SameColor(colors[ImGuiCol_ScrollbarGrabHovered], originalHovered) &&
              SameColor(colors[ImGuiCol_ScrollbarGrabActive], originalActive),
          "transient scrollbar scope restores the caller's theme colors");

    {
        const ImVec4 semanticGrab(0.45f, 0.55f, 0.65f, 0.36f);
        const ImVec4 semanticHovered(0.46f, 0.56f, 0.66f, 0.54f);
        const ImVec4 semanticActive(0.47f, 0.57f, 0.67f, 0.70f);
        const int semanticColorCount = PushTransientScrollbarColors(
            semanticGrab, semanticHovered, semanticActive);
        TransientScrollbarStyleScope visible(true);
        Check(SameColor(colors[ImGuiCol_ScrollbarGrab], semanticGrab),
              "visible gate preserves the semantic grab alpha");
        Check(SameColor(colors[ImGuiCol_ScrollbarGrabHovered], semanticHovered),
              "visible gate preserves the semantic hover alpha");
        Check(SameColor(colors[ImGuiCol_ScrollbarGrabActive], semanticActive),
              "visible gate preserves the semantic active alpha");
        visible.reset();
        ImGui::PopStyleColor(semanticColorCount);
    }

    Check(context->ColorStack.Size == colorsBefore,
          "visible transient scrollbar scope balances its color stack");
    ImGui::DestroyContext();
}

void TestCoreStyleScopeBalancesStacks() {
    using namespace aura::ui::workspace;
    ImGui::CreateContext();
    ImGuiContext* context = ImGui::GetCurrentContext();
    const int colorsBefore = context->ColorStack.Size;
    const int varsBefore = context->StyleVarStack.Size;

    {
        CoreWorkspaceStyleScope scope(
            BuildTestPalette(), SurfaceLevel::Docked, AccentRole::Focused);
        Check(scope.active(), "style scope activates when an ImGui context exists");
        Check(context->ColorStack.Size > colorsBefore, "style scope pushes colors");
        Check(context->StyleVarStack.Size > varsBefore, "style scope pushes metrics");
        scope.reset();
        scope.reset();
        Check(!scope.active(), "style scope reset is idempotent");
    }

    Check(context->ColorStack.Size == colorsBefore, "style scope balances color stack");
    Check(context->StyleVarStack.Size == varsBefore, "style scope balances variable stack");
    ImGui::DestroyContext();
}

void TestModuleShellStateResolution() {
    using namespace aura::ui::workspace;
    const CoreWorkspacePalette palette = BuildTestPalette();

    ModuleWindowShellState state;
    state.mode = ModuleWindowMode::Docked;
    state.activity = ModuleShellActivity::Inactive;
    state.content = ModuleShellContentState::Ready;
    ModuleWindowShellVisual visual = ResolveModuleWindowShellVisual(palette, state);
    Check(visual.surface == SurfaceLevel::Docked, "docked shell uses docked surface");
    Check(!visual.drawShadow, "docked shell has no floating shadow");
    Check(visual.contentVisible, "ready shell exposes caller content");
    Check(SameColor(visual.borderColor, palette.perimeter), "inactive shell uses quiet perimeter");

    state.mode = ModuleWindowMode::Floating;
    state.activity = ModuleShellActivity::Active;
    visual = ResolveModuleWindowShellVisual(palette, state);
    Check(visual.surface == SurfaceLevel::Elevated, "floating shell uses elevated surface");
    Check(visual.drawShadow, "floating shell exposes one modest shadow affordance");
    Check(SameColor(visual.borderColor, palette.functionalAccent), "active shell uses functional accent");

    state.mode = ModuleWindowMode::Detached;
    state.activity = ModuleShellActivity::Focused;
    visual = ResolveModuleWindowShellVisual(palette, state);
    Check(visual.surface == SurfaceLevel::Elevated, "detached shell uses elevated surface");
    Check(visual.drawShadow, "detached shell exposes floating depth");
    Check(SameColor(visual.borderColor, palette.functionalAccent), "focused shell uses functional accent");

    for (ModuleShellContentState content : {
             ModuleShellContentState::Empty,
             ModuleShellContentState::Loading,
             ModuleShellContentState::Error,
             ModuleShellContentState::Unavailable}) {
        state.content = content;
        visual = ResolveModuleWindowShellVisual(palette, state);
        Check(!visual.contentVisible, "non-ready shell owns its status presentation");
        const ImVec4 expected = content == ModuleShellContentState::Error
            ? palette.error
            : palette.mutedText;
        Check(SameColor(visual.statusColor, expected), "shell status uses semantic quiet/error color");
    }
}

void TestModuleShellScopeBalancesBeginEndAndStyles() {
    using namespace aura::ui::workspace;
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(800.0f, 600.0f);
    io.DeltaTime = 1.0f / 60.0f;
    io.IniFilename = nullptr;
    unsigned char* pixels = nullptr;
    int width = 0;
    int height = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);

    ImGui::NewFrame();
    ImGui::Begin("CoreWorkspaceStyleTestHost");
    ImGuiContext* context = ImGui::GetCurrentContext();
    const int colorsBefore = context->ColorStack.Size;
    const int varsBefore = context->StyleVarStack.Size;
    const int windowsBefore = context->CurrentWindowStack.Size;

    ModuleWindowShellOptions options;
    options.title = "Audition";
    options.footerText = "Ready";
    options.size = ImVec2(320.0f, 180.0f);
    options.state.mode = ModuleWindowMode::Floating;
    options.state.activity = ModuleShellActivity::Focused;
    options.state.content = ModuleShellContentState::Ready;
    options.showFooter = true;
    options.resizable = true;
    {
        ModuleWindowShellScope shell("##CoreWorkspaceModule", BuildTestPalette(), options);
        Check(shell.visible(), "module shell begins a visible native child");
        Check(shell.shouldRenderContent(), "ready module shell permits content rendering");
        ImGui::TextUnformatted("Module content");
    }

    Check(context->CurrentWindowStack.Size == windowsBefore, "module shell balances BeginChild/EndChild");
    Check(context->ColorStack.Size == colorsBefore, "module shell balances style colors");
    Check(context->StyleVarStack.Size == varsBefore, "module shell balances style variables");
    ImGui::End();
    ImGui::EndFrame();
    ImGui::DestroyContext();
}

void CreateResultsTestContext() {
    ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(800.0f, 600.0f);
    io.IniFilename = nullptr;
    unsigned char* pixels = nullptr;
    int atlasWidth = 0;
    int atlasHeight = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &atlasWidth, &atlasHeight);
}

void BeginResultsTestFrame(float panelWidth) {
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(20.0f, 20.0f));
    ImGui::SetNextWindowSize(ImVec2(770.0f, 400.0f));
    ImGui::Begin("ResultsLayoutTestHost");

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(harken::results::kContentInsetX, 0.0f));
    const bool visible = BeginTransientScrollbarChild(
        "CenterFileList", ImVec2(panelWidth, 220.0f), ImGuiChildFlags_AlwaysUseWindowPadding,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    Check(visible, "Results content child is visible");
}

void EndResultsTestFrame() {
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::End();
    ImGui::EndFrame();
}

void TestResultsPanelPaddingAndBrowserPathAlignment() {
    using namespace harken::results;
    CreateResultsTestContext();
    for (const float panelWidth : {200.0f, 360.0f, 720.0f}) {
        for (const bool empty : {false, true}) {
            BeginResultsTestFrame(panelWidth);
            const ImGuiWindow* panel = ImGui::GetCurrentWindow();
            const float left = ImGui::GetCursorScreenPos().x;
            const float right = left + ImGui::GetContentRegionAvail().x;
            Check(Near(left - panel->Pos.x, kContentInsetX) &&
                  Near(panel->Pos.x + panel->Size.x - right, kContentInsetX),
                  "Results child honors equal real left and right padding");
            Check(Near(panel->WindowBorderSize, 0.0f),
                  "Results child does not add a border to enable padding");

            AlignBrowserPathLabel(empty);
            ImGui::TextUnformatted("Path:");
            const ImVec2 labelPos = ImGui::GetItemRectMin();
            ImGui::SameLine();
            ImGui::Button("Parent");
            Check(Near(labelPos.y, ImGui::GetItemRectMin().y + ImGui::GetStyle().FramePadding.y),
                  "path label baseline matches the parent button text");

            const bool visible = BeginTransientScrollbarTable(TableName(true), 2,
                TableOuterFlags(empty) | ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX);
            Check(visible, "empty and populated Results tables render at narrow and wide widths");
            if (visible) {
                ImGui::TableSetupColumn("File name", ImGuiTableColumnFlags_WidthFixed, 240.0f);
                ImGui::TableSetupColumn("Waveform", ImGuiTableColumnFlags_WidthFixed, 160.0f);
                ImGui::TableHeadersRow();
                if (!empty) {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextUnformatted("Example.wav");
                }
                const ImGuiTable* table = ImGui::GetCurrentTable();
                Check(Near(table->OuterRect.Min.x, left) && Near(table->OuterRect.Max.x, right),
                      "empty and populated tables have the same left and right edges");
                Check(Near(labelPos.x, table->Columns[0].WorkMinX),
                      "path label shares the first header's text origin");
                ImGui::EndTable();
            }
            EndResultsTestFrame();
        }
    }
    ImGui::DestroyContext();
}

void TestBrowserNarrowRowsAndDefaultColumnBudget() {
    using namespace harken::results;
    const auto normal = LayoutBrowserSearchRow(700.0f, 400.0f, 70.0f, 8.0f);
    Check(normal.searchBesideControls && normal.sourceBesideSearch &&
          Near(normal.searchWidth + 8.0f + normal.sourceWidth, 400.0f),
          "search and short source share remaining width without overflowing");
    const auto narrow = LayoutBrowserSearchRow(200.0f, 60.0f, 500.0f, 8.0f);
    Check(!narrow.searchBesideControls && narrow.sourceBesideSearch &&
          Near(narrow.searchWidth, 110.0f) && Near(narrow.sourceWidth, 82.0f),
          "narrow search wraps and keeps usable width beside an elided source");
    const auto tight = LayoutBrowserSearchRow(128.0f, 15.0f, 500.0f, 8.0f);
    Check(!tight.sourceBesideSearch && Near(tight.searchWidth, 128.0f) && Near(tight.sourceWidth, 128.0f),
          "very narrow rows bound search and source on separate lines");
    const auto noSource = LayoutBrowserSearchRow(350.0f, 200.0f, 0.0f, 8.0f);
    Check(noSource.searchBesideControls && Near(noSource.searchWidth, 200.0f),
          "without a source the existing search fills the remaining row");

    const auto narrowColumns = ResolveBrowserColumnDefaults(350.0f, 300.0f);
    Check(Near(narrowColumns.fileName, 240.0f) && Near(narrowColumns.waveform, 160.0f),
          "narrow Browser tables keep useful default widths and let the body scroll");
    const auto wideColumns = ResolveBrowserColumnDefaults(1500.0f, 320.0f);
    Check(Near(wideColumns.waveform, 300.0f) && Near(wideColumns.fileName, 880.0f),
          "wide Browser defaults allocate the visible remainder to the file name");
}

void DrawResultsSettingsTestTable(bool browser, float replacement) {
    BeginResultsTestFrame(720.0f);
    const bool visible = BeginTransientScrollbarTable(harken::results::TableName(browser), 3,
        ImGuiTableFlags_Resizable | ImGuiTableFlags_Reorderable | ImGuiTableFlags_Hideable |
        ImGuiTableFlags_ScrollX | ImGuiTableFlags_ScrollY, ImVec2(0.0f, 0.0f), 950.0f);
    Check(visible, "settings fixture renders a results table");
    if (visible) {
        ImGui::TableSetupColumn("File name", ImGuiTableColumnFlags_WidthFixed, 300.0f);
        ImGui::TableSetupColumn("Waveform", ImGuiTableColumnFlags_WidthFixed, 160.0f);
        ImGui::TableSetupColumn("File path", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        if (replacement > 0.0f) ImGui::TableSetColumnWidth(0, replacement);
        ImGui::TableHeadersRow();
        ImGui::EndTable();
    }
    EndResultsTestFrame();
}

void TestBrowserSettingsDoNotOverwriteDatabaseAndPlaylist() {
    CreateResultsTestContext();
    BeginResultsTestFrame(720.0f);
    const ImGuiID legacyId = ImGui::GetID("FileTableV6");
    const ImGuiID browserId = ImGui::GetID(harken::results::TableName(true));
    Check(legacyId == ImGui::GetID(harken::results::TableName(false)) && legacyId != browserId,
          "non-browser settings keep the exact legacy hash in the Results child");
    EndResultsTestFrame();

    char fixture[256]{};
    std::snprintf(fixture, sizeof(fixture),
        "[Table][0x%08X,3]\nColumn 0  Width=451 Order=1\nColumn 1  Width=199 Visible=0 Order=2\n"
        "Column 2  Weight=0.7500 Order=0\n\n", static_cast<unsigned>(legacyId));
    ImGui::LoadIniSettingsFromMemory(fixture);
    DrawResultsSettingsTestTable(true, 0.0f);
    DrawResultsSettingsTestTable(true, 499.0f);
    DrawResultsSettingsTestTable(false, 0.0f);
    const std::string saved = ImGui::SaveIniSettingsToMemory();
    ImGui::DestroyContext();

    CreateResultsTestContext();
    ImGui::LoadIniSettingsFromMemory(saved.c_str(), saved.size());
    DrawResultsSettingsTestTable(false, 0.0f);
    DrawResultsSettingsTestTable(true, 0.0f);
    const ImGuiTable* legacyTable = ImGui::TableFindByID(legacyId);
    const ImGuiTable* browserTable = ImGui::TableFindByID(browserId);
    Check(legacyTable && Near(legacyTable->Columns[0].WidthRequest, 451.0f) &&
          Near(legacyTable->Columns[1].WidthRequest, 199.0f) &&
          Near(legacyTable->Columns[2].StretchWeight, 0.75f) &&
          legacyTable->Columns[0].DisplayOrder == 1 && !legacyTable->Columns[1].IsUserEnabled,
          "legacy widths, stretch weight, order and visibility survive Browser resizing and reload");
    Check(browserTable && Near(browserTable->Columns[0].WidthRequest, 499.0f),
          "Browser reloads the separately saved width");
    ImGui::DestroyContext();
}

} // namespace

int main() {
    TestNativeChromeDimensionsScaleTogether();
    TestInvariantMetrics();
    TestPaletteIsCallerSupplied();
    TestSurfaceHierarchyResolver();
    TestToolbarFlowLayoutKeepsPresetDialSeparate();
    TestTransientScrollbarVisibilityPolicy();
    TestTransientScrollbarControllerKeepsRegionClocksIndependent();
    TestTransientScrollbarStyleGatePreservesRgbAndStacks();
    TestCoreStyleScopeBalancesStacks();
    TestModuleShellStateResolution();
    TestModuleShellScopeBalancesBeginEndAndStyles();
    TestResultsPanelPaddingAndBrowserPathAlignment();
    TestBrowserNarrowRowsAndDefaultColumnBudget();
    TestBrowserSettingsDoNotOverwriteDatabaseAndPlaylist();

    if (g_failures == 0) {
        std::puts("All core workspace style tests passed.");
        return 0;
    }
    std::fprintf(stderr, "%d core workspace style test(s) failed.\n", g_failures);
    return 1;
}
