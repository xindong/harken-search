#include "DockLayout.h"
#include "DockHitTest.h"
#include "ui/dock/DockPanelStateBridge.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

int main() {
    using namespace aura::ui::dock;
    auto fail = [](int code) {
        std::printf("FAIL step %d\n", code);
        return code;
    };
    DockLayout layout;
    layout.InsertModule("filter", DockSide::Left, 0.24f);
    layout.InsertModule("info", DockSide::Right, 0.28f);
    if (layout.Find("filter") == nullptr) return fail(1);
    SplitNode* split = layout.RegionAsSplit(DockSide::Left);
    if (!split) split = layout.RootAsSplit();
    if (layout.Find("info") == nullptr) return fail(2);
    if (layout.CountInRootRegion(DockSide::Right) != 1) return fail(3);
    if (split) {
        if (!layout.ResizeSplit(split, 0.10f) && split->ratio <= 0.0f) return 1;
    }
    layout.Detach("filter");
    if (layout.Find("filter") != nullptr) return fail(4);
    if (layout.HasEmptySplit()) return fail(5);

    DockLayout regions;
    if (!regions.InsertModule("filter", DockSide::Left, 0.24f)) return fail(6);
    if (!regions.InsertAdjacent("filter", "info", DockSide::Right, 0.5f)) return fail(7);
    if (regions.Find("info") == nullptr) return fail(8);
    if (regions.CountInRootRegion(DockSide::Left) != 2) return fail(9);
    if (regions.DepthInRootRegion(DockSide::Left) < 1) return fail(10);

    for (int i = 0; i < 14; ++i) {
        char extraId[32];
        std::snprintf(extraId, sizeof(extraId), "mod-left-%d", i);
        const std::string parent = regions.ShallowestModuleInRootRegion(DockSide::Left);
        if (parent.empty()) return fail(11);
        if (!regions.InsertAdjacent(parent, extraId, DockSide::Bottom, 0.5f)) {
            std::printf("insert fail i=%d parent=%s count=%d depth=%d\n", i, parent.c_str(),
                        regions.CountInRootRegion(DockSide::Left), regions.DepthInRootRegion(DockSide::Left));
            return fail(11);
        }
    }
    if (regions.CountInRootRegion(DockSide::Left) != 16) return fail(12);
    if (regions.DepthInRootRegion(DockSide::Left) > 4) return fail(13);
    DockLayout snapshot = regions;
    if (regions.InsertAdjacent("filter", "mod-left-overflow", DockSide::Top, 0.5f)) return fail(14);
    if (!regions.Equals(snapshot)) return fail(15);
    if (regions.CountInRootRegion(DockSide::Left) != 16) return fail(16);

    if (regions.InsertModule("waveform", DockSide::Bottom, 0.28f)) return fail(17);
    if (regions.CountInRootRegion(DockSide::Bottom) != 0) return fail(18);
    if (regions.CountInRootRegion(DockSide::Left) != 16) return fail(19);
    if (IsKnownModuleId("waveform")) return fail(20);
    if (!IsKnownModuleId("similar-sounds")) return fail(21);
    DockLayout similar;
    if (!similar.InsertModule("filter", DockSide::Left, 0.24f)) return fail(22);
    if (!similar.InsertAdjacent("filter", "similar-sounds", DockSide::Bottom, 0.5f)) return fail(23);
    if (similar.Find("similar-sounds") == nullptr) return fail(24);

    DockLayout occupied;
    if (!occupied.InsertModule("info", DockSide::Right, 0.28f)) return fail(25);
    occupied.EnsureFloat("filter");
    occupied.RemoveFloat("filter");
    if (!occupied.InsertModule("filter", DockSide::Right, 0.24f)) return fail(25);
    if (occupied.Find("filter") == nullptr || occupied.Find("info") == nullptr) return fail(26);
    if (occupied.CountInRootRegion(DockSide::Right) != 2) return fail(27);
    if (!occupied.RegionAsSplit(DockSide::Right)) return fail(28);

    const DockRect regionBox{1520.0f, 80.0f, 400.0f, 900.0f};
    DockModuleTarget leaves[8]{};
    const int leafCount = CollectLeafRects(occupied.RegionAsSplit(DockSide::Right), regionBox, leaves, 8, 0);
    if (leafCount != 2) return fail(29);
    DockSplitGrip grip;
    grip.axis = occupied.RegionAsSplit(DockSide::Right)->axis;
    grip.rootRegion = DockSide::Right;
    grip.rect = SplitGripRect(leaves[0].rect, grip.axis);
    grip.visible = grip.rect.width >= kDockSplitGripThickness && grip.rect.height >= kDockSplitGripThickness;
    if (!grip.visible) return fail(30);
    const DockRect pointer{grip.rect.x + 1.0f, grip.rect.y + 8.0f, 4.0f, 4.0f};
    if (!HitSplitGrip(pointer, grip)) return fail(31);
    const float before = occupied.RegionAsSplit(DockSide::Right)->ratio;
    if (!DragSplitGrip(occupied, DockSide::Right, 40.0f, regionBox.width)) return fail(32);
    const float after = occupied.RegionAsSplit(DockSide::Right)->ratio;
    if (after == before) return fail(33);

    const DockPanelChrome floatingChrome = SharedDockPanelChrome(true);
    const DockPanelChrome dockedChrome = SharedDockPanelChrome(false);
    if (!floatingChrome.titleBar || !floatingChrome.close || !floatingChrome.movable || !floatingChrome.dockable) return fail(34);
    if (!dockedChrome.titleBar || !dockedChrome.close || dockedChrome.movable || !dockedChrome.dockable) return fail(35);
    if (!UsesSharedDockPanelChrome("similar-sounds", false)) return fail(36);
    if (UsesSharedDockPanelChrome("similar-sounds", true)) return fail(37);
    if (!UsesSharedDockPanelChrome("info", false)) return fail(38);
    const DockPanelChrome filterChrome = ResolveModulePanelChrome("filter", true, "shared-dock-window-host");
    const DockPanelChrome infoChrome = ResolveModulePanelChrome("info", true, "shared-dock-window-host");
    const DockPanelChrome similarChrome = ResolveModulePanelChrome("similar-sounds", true, "shared-dock-window-host");
    if (!SameSharedPanelChrome(filterChrome, infoChrome)) return fail(39);
    if (!SameSharedPanelChrome(infoChrome, similarChrome)) return fail(40);
    if (filterChrome.entry != "shared-dock-window-host") return fail(41);
    if (!filterChrome.border || !filterChrome.dragHandle || !filterChrome.resizeFeedback) return fail(42);
    const DockPanelChrome similarAlone = ResolveModulePanelChrome("similar-sounds", true, "standalone");
    if (SameSharedPanelChrome(filterChrome, similarAlone)) return fail(43);

    DockHit idleHit;
    if (ShouldDrawDockPreview(false, idleHit)) return fail(44);
    DockHit occupiedPreview;
    occupiedPreview.kind = DockHitKind::Main;
    occupiedPreview.side = DockSide::Right;
    if (ShouldDrawDockPreview(false, occupiedPreview)) return fail(45);
    if (ShouldDrawDockSplitGrip(false, false)) return fail(46);
    if (DockSplitGripFillAlpha(false, false) != 0) return fail(47);
    if (!ShouldDrawDockSplitGrip(true, false)) return fail(48);
    if (DockSplitGripFillAlpha(true, false) <= 0) return fail(49);
    if (!ShouldDrawDockSplitGrip(false, true)) return fail(50);
    if (DockSplitGripFillAlpha(false, true) <= DockSplitGripFillAlpha(true, false)) return fail(51);

    const DockWindowHostRequest futureReq{"mod-new-layout", "###mod-new-layout", true, false, false, 320.0f, 240.0f};
    const DockWindowHostPlan futureHost = PlanSharedDockWindowHost(futureReq);
    if (!futureHost.accepted) return fail(52);
    if (futureHost.entry != "shared-dock-window-host") return fail(53);
    const DockWindowHostPlan filterHost = PlanSharedDockWindowHost({"filter", "###filter", true, false, false, 280.0f, 400.0f});
    const DockWindowHostPlan infoHost = PlanSharedDockWindowHost({"info", "###info", true, false, false, 280.0f, 400.0f});
    const DockWindowHostPlan similarHost = PlanSharedDockWindowHost({"similar-sounds", "###similar-sounds", true, false, false, 720.0f, 360.0f});
    if (!filterHost.accepted || !infoHost.accepted || !similarHost.accepted) return fail(54);
    if (!SameSharedDockWindowHost(filterHost, infoHost)) return fail(55);
    if (!SameSharedDockWindowHost(infoHost, similarHost)) return fail(56);
    if (!SameSharedDockWindowHost(filterHost, futureHost)) return fail(57);
    if (PlanSharedDockWindowHost({"similar-sounds", "###similar-sounds", true, true, false, 720.0f, 360.0f}).accepted) return fail(58);
    if (PlanSharedDockWindowHost({"filter", "###filter", true, false, true, 280.0f, 400.0f}).accepted) return fail(59);
    if (!futureHost.persistId || !futureHost.restoreSize || !futureHost.floatDockSwitch || !futureHost.splitterFeedback) return fail(60);
    if (!futureHost.chrome.titleBar || !futureHost.chrome.close || !futureHost.chrome.border || !futureHost.chrome.dragHandle) return fail(61);
    if (PlanSharedDockWindowHost({"unknown-panel", "###unknown-panel", true, false, false, 200.0f, 200.0f}).accepted) return fail(62);

    const DockRect work{0.0f, 0.0f, 1920.0f, 1080.0f};
    const float bandwidth = 80.0f;
    const float splitRatio = kDockIncomingShare;
    const DockRect leavesToSplit[] = {
        {0.0f, 80.0f, 400.0f, 900.0f},
        {1520.0f, 80.0f, 400.0f, 900.0f},
        {400.0f, 720.0f, 1120.0f, 280.0f},
        {480.0f, 160.0f, 640.0f, 360.0f},
    };
    const char* leafIds[] = {"filter", "info", "similar-sounds", "mod-new-layout"};
    const DockSide fourWays[] = {DockSide::Left, DockSide::Right, DockSide::Top, DockSide::Bottom};
    for (int leafIndex = 0; leafIndex < 4; ++leafIndex) {
        const DockRect& leaf = leavesToSplit[leafIndex];
        DockModuleTarget target{leafIds[leafIndex], leaf};
        for (DockSide side : fourWays) {
            const DockRect pointer = InnerStripRect(leaf, side, bandwidth);
            DockHit hit = HitDockTarget(pointer, work, bandwidth, &target, 1);
            if (hit.kind != DockHitKind::Module || hit.side != side || hit.targetModuleId != leafIds[leafIndex]) {
                return fail(63);
            }
            const DockRect expectedStrip = InnerStripRect(leaf, side, bandwidth);
            if (!SameDockRect(hit.hitRect, expectedStrip)) return fail(64);
            if (side == DockSide::Left || side == DockSide::Right) {
                if (std::fabs(hit.hitRect.height - leaf.height) > 0.5f) return fail(65);
                if (std::fabs(hit.hitRect.width - bandwidth) > 0.5f) return fail(66);
            } else {
                if (std::fabs(hit.hitRect.width - leaf.width) > 0.5f) return fail(67);
                if (std::fabs(hit.hitRect.height - bandwidth) > 0.5f) return fail(68);
            }
            if (hit.hitRect.width >= work.width - 1.0f || hit.hitRect.height >= work.height - 1.0f) return fail(69);
            LeafSplitPreview split = PlanLeafEdgeSplit(leaf, side, bandwidth, splitRatio);
            if (!split.accepted) return fail(70);
            if (!SameDockRect(split.parent, leaf)) return fail(71);
            if (!SameDockRect(split.preview, hit.resultRect)) return fail(72);
            if (!SameDockRect(split.incoming, hit.resultRect)) return fail(73);
            if (!SameDockRect(UnionDockRect(split.incoming, split.remaining), leaf)) return fail(74);
            if (!RectContainedIn(split.incoming, leaf) || !RectContainedIn(split.remaining, leaf)) return fail(75);
            DockPreviewPlan preview = PlanDockPreview(hit, work, 80.0f, 80.0f, &target, 1, bandwidth);
            if (preview.stripCount != 1) return fail(76);
            if (!SameDockRect(preview.strips[0].rect, hit.resultRect)) return fail(77);
            if (!SameDockRect(preview.strips[0].rect, split.preview)) return fail(78);
        }
    }

    DockLayout nested;
    if (!nested.InsertModule("info", DockSide::Right, 0.28f)) return fail(79);
    if (!nested.InsertAdjacent("info", "similar-sounds", DockSide::Left, 0.24f)) return fail(80);
    const DockRect parentLeaf{1520.0f, 80.0f, 400.0f, 900.0f};
    DockModuleTarget nestedLeaves[8]{};
    const int nestedCount = CollectLeafRects(nested.RegionAsSplit(DockSide::Right), parentLeaf, nestedLeaves, 8, 0);
    if (nestedCount != 2) return fail(81);
    if (!SameDockRect(UnionDockRect(nestedLeaves[0].rect, nestedLeaves[1].rect), parentLeaf)) return fail(82);
    if (!RectContainedIn(nestedLeaves[0].rect, parentLeaf) || !RectContainedIn(nestedLeaves[1].rect, parentLeaf)) return fail(83);
    if (!nested.InsertAdjacent(nestedLeaves[0].id, "mod-new-layout", DockSide::Bottom, 0.24f)) return fail(84);
    DockModuleTarget recursiveLeaves[8]{};
    const int recursiveCount = CollectLeafRects(nested.RegionAsSplit(DockSide::Right), parentLeaf, recursiveLeaves, 8, 0);
    if (recursiveCount != 3) return fail(85);
    DockRect recursiveUnion = recursiveLeaves[0].rect;
    for (int i = 1; i < recursiveCount; ++i) recursiveUnion = UnionDockRect(recursiveUnion, recursiveLeaves[i].rect);
    if (!SameDockRect(recursiveUnion, parentLeaf)) return fail(86);
    for (int i = 0; i < recursiveCount; ++i) {
        if (!RectContainedIn(recursiveLeaves[i].rect, parentLeaf)) return fail(87);
        DockModuleTarget child = recursiveLeaves[i];
        const float childEdge = std::min(bandwidth, std::min(child.rect.width, child.rect.height));
        DockHit childHit = HitDockTarget(InnerStripRect(child.rect, DockSide::Top, childEdge), work, bandwidth, &child, 1);
        if (childHit.kind != DockHitKind::Module || childHit.side != DockSide::Top) return fail(88);
        if (!SameDockRect(childHit.hitRect, InnerStripRect(child.rect, DockSide::Top, childEdge))) return fail(89);
        LeafSplitPreview childSplit = PlanLeafEdgeSplit(child.rect, DockSide::Top, bandwidth, splitRatio);
        const bool readable = childSplit.incoming.width >= 160 && childSplit.remaining.width >= 160 &&
            childSplit.incoming.height >= 120 && childSplit.remaining.height >= 120;
        if (childSplit.accepted != readable || !SameDockRect(childSplit.preview, childHit.resultRect)) return fail(90);
        if (!SameDockRect(UnionDockRect(childSplit.incoming, childSplit.remaining), child.rect)) return fail(91);
    }

    DockLayout occupiedLayout;
    if (!occupiedLayout.InsertModule("filter", DockSide::Left, 0.24f)) return fail(92);
    if (!occupiedLayout.InsertModule("info", DockSide::Right, 0.28f)) return fail(93);
    if (!occupiedLayout.InsertModule("similar-sounds", DockSide::Bottom, 0.24f)) return fail(94);
    const DockRect occupiedWork{0.0f, 0.0f, 1920.0f, 1080.0f};
    DockRootSlots fullRowSlots{};
    fullRowSlots.left = {0.0f, 80.0f, 400.0f, 720.0f};
    fullRowSlots.right = {1520.0f, 80.0f, 400.0f, 720.0f};
    fullRowSlots.bottom = {0.0f, 800.0f, 1920.0f, 200.0f};
    DockModuleTarget occupiedLeaves[8]{};
    const int occupiedCount = LayoutRootRegionLeaves(occupiedLayout, occupiedWork, 80.0f, 80.0f, occupiedLeaves, 8, &fullRowSlots);
    if (occupiedCount != 3) return fail(95);
    DockModuleTarget* similarTarget = nullptr;
    for (int i = 0; i < occupiedCount; ++i) {
        if (occupiedLeaves[i].id == "similar-sounds") similarTarget = &occupiedLeaves[i];
    }
    if (!similarTarget) return fail(96);
    if (std::fabs(similarTarget->rect.width - 1120.0f) > 0.5f || std::fabs(similarTarget->rect.x - 400.0f) > 0.5f) {
        return fail(97);
    }
    if (similarTarget->rect.width >= fullRowSlots.bottom.width - 1.0f) return fail(98);
    const DockRect overSimilarTop{720.0f, 800.0f, 80.0f, 80.0f};
    DockHit similarHit = HitDockTarget(overSimilarTop, occupiedWork, bandwidth, occupiedLeaves, occupiedCount, &fullRowSlots);
    if (similarHit.kind != DockHitKind::Module || similarHit.side != DockSide::Top || similarHit.targetModuleId != "similar-sounds") {
        return fail(99);
    }
    if (!SameDockRect(similarHit.hitRect, InnerStripRect(similarTarget->rect, DockSide::Top, bandwidth))) return fail(100);
    if (std::fabs(similarHit.hitRect.width - similarTarget->rect.width) > 0.5f) return fail(101);

    DockLayout splitFilter;
    if (!splitFilter.InsertModule("filter", DockSide::Left, 0.24f)) return fail(102);
    if (!splitFilter.InsertAdjacent("filter", "similar-sounds", DockSide::Right, 0.24f)) return fail(103);
    const DockRect originalFilter{0.0f, 80.0f, 400.0f, 900.0f};
    DockModuleTarget splitLeaves[8]{};
    const int splitCount = CollectLeafRects(splitFilter.RegionAsSplit(DockSide::Left), originalFilter, splitLeaves, 8, 0);
    if (splitCount != 2) return fail(104);
    if (!SameDockRect(UnionDockRect(splitLeaves[0].rect, splitLeaves[1].rect), originalFilter)) return fail(105);
    if (!RectContainedIn(splitLeaves[0].rect, originalFilter) || !RectContainedIn(splitLeaves[1].rect, originalFilter)) {
        return fail(106);
    }
    const DockedLeafShell filterShell = PlanDockedLeafShell(splitLeaves[0].rect);
    const DockedLeafShell similarShell = PlanDockedLeafShell(splitLeaves[1].rect);
    if (!RectContainedIn(filterShell.chrome, originalFilter) || !RectContainedIn(similarShell.chrome, originalFilter)) {
        return fail(107);
    }
    if (!LeafShellsDoNotOverlap(filterShell, similarShell)) return fail(108);
    if (std::fabs(filterShell.titleHeight - 24.0f) > 0.01f || std::fabs(filterShell.radius - 4.0f) > 0.01f ||
        std::fabs(filterShell.perimeter - 1.0f) > 0.01f || std::fabs(filterShell.inset - 4.0f) > 0.01f) {
        return fail(109);
    }

    // The screenshot path: a band test starts in the top root, is split once,
    // then a third module is split from one of the resulting top leaves.  The
    // production geometry path must retain all three leaves, both seams, and
    // the fixed waveform budget below the top band.
    DockLayout topBand;
    if (!topBand.InsertModule("mod-docktest-band", DockSide::Top, 0.24f)) return fail(110);
    if (!topBand.InsertAdjacent("mod-docktest-band", "mod-docktest-a", DockSide::Bottom, 0.5f)) {
        return fail(111);
    }
    if (!topBand.InsertAdjacent("mod-docktest-a", "mod-docktest-b", DockSide::Bottom, 0.5f)) {
        return fail(112);
    }
    DockRootSlots topSlots{};
    topSlots.top = {400.0f, 80.0f, 1120.0f, 320.0f};
    topSlots.bottom = {400.0f, 400.0f, 1120.0f, 200.0f};
    const DockRect topWork{0.0f, 0.0f, 1920.0f, 1080.0f};
    DockFrameGeometry topFrame = BuildDockFrameGeometry(
        topBand, topWork, 80.0f, 80.0f, topSlots);
    if (topFrame.moduleCount != 3 || topFrame.gripCount != 2) return fail(113);
    DockRect topUnion{};
    for (int i = 0; i < topFrame.moduleCount; ++i) {
        if (topFrame.modules[i].id == "mod-docktest-band" &&
            topFrame.modules[i].rect.y < topSlots.top.y) return fail(114);
        topUnion = i == 0 ? topFrame.modules[i].rect : UnionDockRect(topUnion, topFrame.modules[i].rect);
    }
    if (!SameDockRect(topUnion, topSlots.top)) return fail(115);
    for (int i = 0; i < topFrame.gripCount; ++i) {
        if (!topFrame.grips[i].visible || topFrame.grips[i].rect.height < kDockSplitGripThickness) {
            return fail(116);
        }
        if (!HitSplitGrip(topFrame.grips[i].rect, {topFrame.grips[i].rect, topFrame.grips[i].axis,
                                                  DockSide::Top, true})) return fail(117);
    }
    if (topFrame.rootSlots.bottom.height != topSlots.bottom.height ||
        topFrame.rootSlots.bottom.y != topSlots.bottom.y) return fail(118);
    if (std::fabs(similarShell.titleHeight - filterShell.titleHeight) > 0.01f ||
        std::fabs(similarShell.radius - filterShell.radius) > 0.01f) {
        return fail(110);
    }
    AdjacentLeafGrip grips[4]{};
    const int gripCount = CollectAdjacentLeafGrips(splitLeaves, splitCount, grips, 4);
    if (gripCount != 1) return fail(111);
    if (!grips[0].visible) return fail(112);
    if (!RectContainedIn(grips[0].rect, originalFilter)) return fail(113);
    const float beforeRatio = splitFilter.RegionAsSplit(DockSide::Left)->ratio;
    if (!DragAdjacentLeafGrip(splitFilter, splitLeaves[0].id, splitLeaves[1].id, 24.0f, originalFilter.width)) {
        return fail(114);
    }
    if (splitFilter.RegionAsSplit(DockSide::Left)->ratio == beforeRatio) return fail(115);
    if (!ShouldDrawDockSplitGrip(true, false) || ShouldDrawDockSplitGrip(false, false)) return fail(116);

    DockLayout bandLayout;
    if (!bandLayout.InsertModule("similar-sounds", DockSide::Bottom, 0.24f)) return fail(117);
    if (!bandLayout.InsertAdjacent("similar-sounds", "info", DockSide::Top, 0.24f)) return fail(118);
    const DockRect bottomSlot{400.0f, 800.0f, 1120.0f, 200.0f};
    const DockedSplitBand band = ResolveDockedSplitBand(bandLayout, DockSide::Bottom, bottomSlot, "similar-sounds", "info");
    if (!band.split) return fail(119);
    if (!SameDockRect(band.incomingLeaf, {bottomSlot.x, bottomSlot.y, bottomSlot.width, bottomSlot.height * 0.24f})) {
        return fail(120);
    }
    if (!SameDockRect(band.originalLeaf,
                      {bottomSlot.x, bottomSlot.y + bottomSlot.height * 0.24f, bottomSlot.width, bottomSlot.height * 0.76f})) {
        return fail(121);
    }
    if (RectsOverlap(band.originalLeaf, band.incomingLeaf)) return fail(122);
    if (!SameDockRect(UnionDockRect(band.originalLeaf, band.incomingLeaf), bottomSlot)) return fail(123);
    if (!RectContainedIn(band.originalLeaf, bottomSlot) || !RectContainedIn(band.incomingLeaf, bottomSlot)) return fail(124);

    DockLayout unsplitBand;
    if (!unsplitBand.InsertModule("similar-sounds", DockSide::Bottom, 0.24f)) return fail(125);
    const DockedSplitBand whole = ResolveDockedSplitBand(unsplitBand, DockSide::Bottom, bottomSlot, "similar-sounds", "info");
    if (whole.split) return fail(126);

    if (kDockIncomingShare < 0.4f || kDockIncomingShare > 0.6f) return fail(127);
    const DockRect shareLeaf{100.0f, 100.0f, 400.0f, 800.0f};
    for (DockSide side : fourWays) {
        const DockRect incoming = SplitIncomingRect(shareLeaf, side, kDockIncomingShare);
        const float shareAlong = (side == DockSide::Left || side == DockSide::Right)
            ? incoming.width / shareLeaf.width
            : incoming.height / shareLeaf.height;
        if (shareAlong < 0.4f) return fail(128);
    }

    const float columnWidth = 400.0f;
    const float splitLeaf = 260.0f;
    const float staleNarrowLeaf = 96.0f;
    if (std::fabs(DockFilterShellWidth(true, columnWidth, splitLeaf, 220.0f, 400.0f) - splitLeaf) > 0.01f) {
        return fail(129);
    }
    if (std::fabs(DockFilterShellWidth(false, columnWidth, staleNarrowLeaf, 220.0f, 400.0f) - columnWidth) > 0.01f) {
        return fail(130);
    }
    if (std::fabs(DockFilterShellWidth(true, columnWidth, staleNarrowLeaf, 220.0f, 400.0f) - staleNarrowLeaf) > 0.01f) {
        return fail(188);
    }

    AdjacentLeafGrip gripA;
    gripA.axis = Axis::Horizontal;
    gripA.firstId = "filter";
    gripA.secondId = "similar-sounds";
    gripA.rect = {100.0f, 200.0f, 8.0f, 400.0f};
    AdjacentLeafGrip gripB = gripA;
    gripB.firstId = "similar-sounds";
    gripB.secondId = "filter";
    if (!SameAdjacentLeafGrip(gripA, gripB)) return fail(189);
    gripB.rect.y += 1.0f;
    if (SameAdjacentLeafGrip(gripA, gripB)) return fail(190);

    // Every shared edge between adjacent leaves gets a draggable grip, not
    // only pairs with identical rows/columns.
    {
        DockModuleTarget mixedLeaves[3] = {
            {"filter", {0.0f, 80.0f, 200.0f, 900.0f}},
            {"similar-sounds", {200.0f, 80.0f, 200.0f, 900.0f}},
            {"info", {0.0f, 980.0f, 400.0f, 100.0f}},
        };
        AdjacentLeafGrip seamGrips[8]{};
        const int seamCount = CollectAdjacentLeafGrips(mixedLeaves, 3, seamGrips, 8);
        if (seamCount != 3) return fail(131);
        bool sawFilterSimilar = false;
        bool sawFilterBottom = false;
        bool sawSimilarBottom = false;
        for (int i = 0; i < seamCount; ++i) {
            if (!seamGrips[i].visible) return fail(132);
            const std::string& f = seamGrips[i].firstId;
            const std::string& s = seamGrips[i].secondId;
            if ((f == "filter" && s == "similar-sounds") || (f == "similar-sounds" && s == "filter")) {
                sawFilterSimilar = true;
                if (seamGrips[i].axis != Axis::Horizontal) return fail(133);
                if (std::fabs(seamGrips[i].rect.height - 900.0f) > 1.0f) return fail(134);
            }
            if ((f == "filter" && s == "info") || (f == "info" && s == "filter")) {
                sawFilterBottom = true;
                if (seamGrips[i].axis != Axis::Vertical) return fail(135);
                if (std::fabs(seamGrips[i].rect.width - 200.0f) > 1.0f) return fail(136);
            }
            if ((f == "similar-sounds" && s == "info") || (f == "info" && s == "similar-sounds")) {
                sawSimilarBottom = true;
            }
        }
        if (!sawFilterSimilar || !sawFilterBottom || !sawSimilarBottom) return fail(137);
    }

    const DockLeafShellParams floatingParams = DockLeafShellParamsFor(true);
    const DockLeafShellParams dockedParams = DockLeafShellParamsFor(false);
    if (std::fabs(floatingParams.titleHeight - 24.0f) > 0.01f || std::fabs(floatingParams.radius - 4.0f) > 0.01f ||
        std::fabs(floatingParams.perimeter - 1.0f) > 0.01f || std::fabs(floatingParams.inset - 4.0f) > 0.01f ||
        floatingParams.entry != "shared-dock-window-host") {
        return fail(138);
    }
    if (std::fabs(floatingParams.titleHeight - dockedParams.titleHeight) > 0.01f ||
        std::fabs(floatingParams.radius - dockedParams.radius) > 0.01f ||
        std::fabs(floatingParams.perimeter - dockedParams.perimeter) > 0.01f ||
        std::fabs(floatingParams.inset - dockedParams.inset) > 0.01f ||
        floatingParams.entry != dockedParams.entry) {
        return fail(139);
    }

    // Original window rect equals the remaining leaf in any region.
    DockLayout rightSplit;
    if (!rightSplit.InsertModule("info", DockSide::Right, 0.28f)) return fail(140);
    if (!rightSplit.InsertAdjacent("info", "similar-sounds", DockSide::Left, 0.24f)) return fail(141);
    const DockRect rightSlot{1520.0f, 80.0f, 400.0f, 900.0f};
    const DockedSplitBand rightBand = ResolveDockedSplitBand(rightSplit, DockSide::Right, rightSlot, "info", "similar-sounds");
    if (!rightBand.split) return fail(142);
    if (!SameDockRect(rightBand.incomingLeaf,
                      {rightSlot.x, rightSlot.y, rightSlot.width * 0.24f, rightSlot.height})) {
        return fail(143);
    }
    if (!SameDockRect(rightBand.originalLeaf,
                      {rightSlot.x + rightSlot.width * 0.24f, rightSlot.y, rightSlot.width * 0.76f, rightSlot.height})) {
        return fail(144);
    }
    if (RectsOverlap(rightBand.originalLeaf, rightBand.incomingLeaf)) return fail(145);
    if (!SameDockRect(UnionDockRect(rightBand.originalLeaf, rightBand.incomingLeaf), rightSlot)) return fail(146);

    // Legacy filter column is not in the docked-leaf list. Similar-sounds sits
    // on its right half, so the shared edge must still grow a grip.
    {
        DockLayout splitColumn;
        if (!splitColumn.InsertModule("filter", DockSide::Left, 0.24f)) return fail(147);
        if (!splitColumn.InsertAdjacent("filter", "similar-sounds", DockSide::Right, 0.5f)) return fail(148);
        const DockRect column{0.0f, 80.0f, 400.0f, 900.0f};
        DockModuleTarget allLeaves[8]{};
        const int allCount = CollectLeafRects(splitColumn.RegionAsSplit(DockSide::Left), column, allLeaves, 8, 0);
        if (allCount != 2) return fail(149);
        DockModuleTarget similarOnly[2]{};
        int similarCount = 0;
        for (int i = 0; i < allCount; ++i) {
            if (allLeaves[i].id == "similar-sounds") similarOnly[similarCount++] = allLeaves[i];
        }
        if (similarCount != 1) return fail(150);
        if (CollectAdjacentLeafGrips(similarOnly, similarCount, grips, 4) != 0) return fail(151);
        DockModuleTarget legacyColumn{"filter", column};
        AdjacentLeafGrip columnGrips[4]{};
        const int columnGripCount =
            CollectLegacyColumnLeafGrips(&legacyColumn, 1, similarOnly, similarCount, columnGrips, 4);
        if (columnGripCount != 1) return fail(152);
        if (!columnGrips[0].visible) return fail(153);
        if (columnGrips[0].axis != Axis::Horizontal) return fail(154);
        const bool filterSimilar =
            (columnGrips[0].firstId == "filter" && columnGrips[0].secondId == "similar-sounds") ||
            (columnGrips[0].firstId == "similar-sounds" && columnGrips[0].secondId == "filter");
        if (!filterSimilar) return fail(155);
        const DockRect expectedShell = RemainingColumnShell(column, "filter", similarOnly, similarCount);
        if (!SameDockRect(expectedShell, {0.0f, 80.0f, 200.0f, 900.0f})) return fail(156);
        const float beforeLegacyRatio = splitColumn.RegionAsSplit(DockSide::Left)->ratio;
        if (!DragAdjacentLeafGrip(splitColumn, columnGrips[0].firstId, columnGrips[0].secondId, 24.0f, column.width)) {
            return fail(157);
        }
        if (splitColumn.RegionAsSplit(DockSide::Left)->ratio == beforeLegacyRatio) return fail(158);
    }

    {
        const DockedFloatBackResult menu = PlanDockedFloatBack(true, 400.0f);
        if (!menu.hasFloatAction) return fail(159);
        if (PlanDockedFloatBack(false, 400.0f).hasFloatAction) return fail(160);
        const float originalColumn = 400.0f;
        const float splitLeaf = 200.0f;
        const DockedFloatBackResult floated = ApplyDockedFloatBack(originalColumn, splitLeaf);
        if (!floated.floating) return fail(161);
        if (std::fabs(floated.columnWidth - originalColumn) > 0.01f) return fail(162);
        if (std::fabs(DockFilterShellWidth(false, floated.columnWidth, splitLeaf, 160.0f, 400.0f) - originalColumn) > 0.01f) {
            return fail(163);
        }
    }

    // Docked neighbor must take its own leaf, not sit on top of the original
    // window. The original hit area equals the remaining leaf, so clicks there
    // still reach the original content.
    {
        const DockRect column{0.0f, 80.0f, 400.0f, 900.0f};
        const DockRect incoming{200.0f, 80.0f, 200.0f, 900.0f};
        const DockedPairHit pair = PlanDockedPairHit(column, incoming);
        if (RectsOverlap(pair.original, pair.incoming)) return fail(164);
        if (!SameDockRect(UnionDockRect(pair.original, pair.incoming), column)) return fail(165);
        if (!SameDockRect(pair.original, {0.0f, 80.0f, 200.0f, 900.0f})) return fail(166);
        if (!SameDockRect(pair.incoming, incoming)) return fail(167);
        if (!pair.disjoint || !pair.coversOriginal) return fail(168);
        if (!pair.hitMatchesOriginal) return fail(169);
        if (!SameDockRect(pair.originalHit, pair.original)) return fail(170);
        if (std::fabs(pair.originalHit.width - pair.original.width) > 0.01f ||
            std::fabs(pair.originalHit.height - pair.original.height) > 0.01f) {
            return fail(171);
        }
        const DockRect stackedIncoming{0.0f, 530.0f, 400.0f, 450.0f};
        const DockedPairHit stacked = PlanDockedPairHit(column, stackedIncoming);
        if (RectsOverlap(stacked.original, stacked.incoming)) return fail(172);
        if (!SameDockRect(UnionDockRect(stacked.original, stacked.incoming), column)) return fail(173);
        if (!SameDockRect(stacked.originalHit, stacked.original)) return fail(174);
    }

    // Docking only snaps when the dragged window actually touches one of the
    // four edges. A wide window sitting in the middle of a short leaf must not
    // count as touching the top edge just because the top strip spans the leaf.
    {
        const DockRect infoLeaf{400.0f, 800.0f, 1120.0f, 200.0f};
        DockModuleTarget infoTarget{"info", infoLeaf};
        const DockRect middle{700.0f, 840.0f, 520.0f, 120.0f};
        const DockHit middleHit = HitDockTarget(middle, work, bandwidth, &infoTarget, 1);
        if (middleHit.kind != DockHitKind::None) return fail(175);
        const DockRect onTop{700.0f, 800.0f, 520.0f, 40.0f};
        const DockHit topHit = HitDockTarget(onTop, work, bandwidth, &infoTarget, 1);
        if (topHit.kind != DockHitKind::Module || topHit.side != DockSide::Top) return fail(176);
        if (std::fabs(topHit.hitRect.width - infoLeaf.width) > 0.5f) return fail(177);
        if (std::fabs(topHit.hitRect.height - bandwidth) > 0.5f) return fail(178);
        const DockRect onLeft{400.0f, 820.0f, 40.0f, 160.0f};
        const DockHit leftHit = HitDockTarget(onLeft, work, bandwidth, &infoTarget, 1);
        if (leftHit.kind != DockHitKind::Module || leftHit.side != DockSide::Left) return fail(179);
        if (std::fabs(leftHit.hitRect.height - infoLeaf.height) > 0.5f) return fail(180);
    }

    // A visible adjacent grip may sit at the edge of a nested split subtree.
    {
        DockLayout nested;
        if (!nested.InsertModule("filter", DockSide::Left, 0.5f)) return fail(181);
        if (!nested.InsertAdjacent("filter", "search-results", DockSide::Right, 0.5f)) return fail(182);
        if (!nested.InsertAdjacent("search-results", "similar-sounds", DockSide::Right, 0.5f)) return fail(183);
        SplitNode* outer = nested.RegionAsSplit(DockSide::Left);
        if (!outer || !outer->second || outer->second->Kind() != NodeKind::Split) return fail(184);
        if (FindSplitContainingLeaves(nested, "filter", "search-results") != outer) return fail(185);
        DockModuleTarget nestedLeaves[] = {
            {"filter", {0.0f, 80.0f, 200.0f, 900.0f}},
            {"search-results", {200.0f, 80.0f, 100.0f, 900.0f}},
            {"similar-sounds", {300.0f, 80.0f, 100.0f, 900.0f}}};
        if (std::fabs(AdjacentSplitExtent(nested, "filter", "search-results", Axis::Horizontal,
                                          nestedLeaves, 3) - 400.0f) > 0.01f) return fail(186);
        if (std::fabs(AdjacentSplitExtent(nested, "search-results", "similar-sounds", Axis::Horizontal,
                                          nestedLeaves, 3) - 200.0f) > 0.01f) return fail(187);
    }
    // RED: a committed dock hit must mirror Top/Bottom ownership into the
    // persisted panel state and select width vs height by the side's axis.
    {
        PanelDockState filterTop;
        filterTop.dockSide = 0;
        filterTop.dockedWidth = 250.0f;
        filterTop.dockedHeight = 200.0f;
        ApplyDockCommitToPanel(filterTop, DockSide::Top, DockRect{0.0f, 80.0f, 400.0f, 120.0f});
        if (filterTop.dockSide != 2) return fail(191);
        if (std::fabs(filterTop.dockedHeight - 120.0f) > 0.5f) return fail(192);
        if (std::fabs(filterTop.dockedWidth - 250.0f) > 0.5f) return fail(193);
        if (filterTop.floating) return fail(194);

        PanelDockState infoBottom;
        ApplyDockCommitToPanel(infoBottom, DockSide::Bottom, DockRect{0.0f, 0.0f, 300.0f, 180.0f});
        if (infoBottom.dockSide != 3) return fail(195);
        if (std::fabs(infoBottom.dockedHeight - 180.0f) > 0.5f) return fail(196);

        PanelDockState filterLeft;
        ApplyDockCommitToPanel(filterLeft, DockSide::Left, DockRect{0.0f, 0.0f, 250.0f, 800.0f});
        if (filterLeft.dockSide != 0) return fail(197);
        if (std::fabs(filterLeft.dockedWidth - 250.0f) > 0.5f) return fail(198);
        if (std::fabs(filterLeft.dockedHeight - 200.0f) > 0.5f) return fail(199);

        if (SavedSideToDockSide(2) != DockSide::Top) return fail(200);
        if (DockSideToSavedSide(DockSide::Bottom) != 3) return fail(201);
        if (!SideIsVertical(DockSide::Top) || !SideIsVertical(DockSide::Bottom)) return fail(202);
        if (SideIsVertical(DockSide::Left) || SideIsVertical(DockSide::Right)) return fail(203);
    }

    // Reset must not create blank Similar/Band test leaves or reserve a band.
    {
        DockLayout oldLayout;
        if (!oldLayout.InsertModule("mod-docktest-similar", DockSide::Right, 0.25f) ||
            !oldLayout.InsertModule("mod-docktest-band", DockSide::Top, 0.2f)) return fail(308);
        if (!oldLayout.Detach("mod-docktest-band") ||
            !oldLayout.EnsureFloat("mod-docktest-band")) return fail(309);
        PurgeObsoleteDockTestModules(oldLayout);
        if (oldLayout.Find("mod-docktest-similar") ||
            oldLayout.FindFloat("mod-docktest-band") ||
            oldLayout.RegionRoot(DockSide::Top) || oldLayout.RegionRoot(DockSide::Right)) return fail(310);
        DockLayout testReset;
        ResetDockTestLayoutLeaves(testReset);
        if (testReset.Find("mod-docktest-similar") || testReset.Find("mod-docktest-band")) return fail(300);
        const DockBandReserve reserve = ResolveDockBandReserve(testReset, 180.0f, 220.0f, 160.0f);
        if (reserve.top != 0.0f || reserve.bottom != 0.0f) return fail(301);
        const DockRect work{0.0f, 0.0f, 1600.0f, 1000.0f};
        const float toolbar = 80.0f;
        const float waveformTop = 720.0f;
        DockRootSlots slots{};
        slots.left = {0.0f, toolbar + reserve.top, 300.0f, waveformTop - toolbar - reserve.top};
        slots.right = {1300.0f, toolbar + reserve.top, 300.0f, waveformTop - toolbar - reserve.top};
        slots.top = {0.0f, toolbar, 1600.0f, reserve.top};
        slots.bottom = {300.0f, waveformTop - reserve.bottom, 1000.0f, reserve.bottom};
        const DockFrameGeometry frame = BuildDockFrameGeometry(testReset, work, toolbar, 0.0f, slots);
        const DockModuleTarget* band = nullptr;
        for (int i = 0; i < frame.moduleCount; ++i) {
            if (frame.modules[i].id == "mod-docktest-band") band = &frame.modules[i];
        }
        if (band || frame.moduleCount != 2) return fail(302);
    }
    {
        DockLayout bands;
        if (!bands.InsertModule("filter", DockSide::Top, 0.5f)) return fail(204);
        const DockBandReserve topReserve = ResolveDockBandReserve(bands, 180.0f, 220.0f, 120.0f);
        if (std::fabs(topReserve.top - 180.0f) > 0.5f) return fail(205);
        if (topReserve.bottom != 0.0f) return fail(206);

        DockLayout bottomBands;
        if (!bottomBands.InsertModule("info", DockSide::Bottom, 0.5f)) return fail(207);
        const DockBandReserve bottomReserve = ResolveDockBandReserve(bottomBands, 180.0f, 260.0f, 120.0f);
        if (std::fabs(bottomReserve.bottom - 260.0f) > 0.5f) return fail(208);
        if (bottomReserve.top != 0.0f) return fail(209);

        DockLayout both;
        if (!both.InsertModule("filter", DockSide::Top, 0.5f)) return fail(210);
        if (!both.InsertModule("info", DockSide::Top, 0.5f)) return fail(211);
        const DockBandReserve bothReserve = ResolveDockBandReserve(both, 200.0f, 240.0f, 120.0f);
        if (std::fabs(bothReserve.top - 240.0f) > 0.5f) return fail(212);

        DockLayout emptyBands;
        const DockBandReserve emptyReserve = ResolveDockBandReserve(emptyBands, 180.0f, 220.0f, 120.0f);
        if (emptyReserve.top != 0.0f || emptyReserve.bottom != 0.0f) return fail(213);
    }

    // Focused: a Top band above the side columns must keep its full width;
    // side columns start below it and must not clip the Top leaf horizontally.
    {
        DockLayout topFull;
        if (!topFull.InsertModule("filter", DockSide::Left, 0.24f)) return fail(214);
        if (!topFull.InsertModule("info", DockSide::Top, 0.5f)) return fail(215);
        DockRootSlots stacked{};
        stacked.left = {0.0f, 260.0f, 400.0f, 620.0f};
        stacked.right = {1520.0f, 260.0f, 400.0f, 620.0f};
        stacked.top = {0.0f, 80.0f, 1920.0f, 180.0f};
        DockModuleTarget topLeaves[4]{};
        const int topCount = LayoutRootRegionLeaves(topFull, {0.0f, 0.0f, 1920.0f, 1080.0f},
                                                    80.0f, 80.0f, topLeaves, 4, &stacked);
        const DockModuleTarget* infoTop = nullptr;
        for (int i = 0; i < topCount; ++i) {
            if (topLeaves[i].id == "info") infoTop = &topLeaves[i];
        }
        if (!infoTop) return fail(216);
        if (std::fabs(infoTop->rect.width - 1920.0f) > 0.5f) return fail(217);
        if (std::fabs(infoTop->rect.x - 0.0f) > 0.5f) return fail(218);
    }

    // P1: an owner may shrink the band below the fallback, still clamped.
    {
        DockLayout shrink;
        if (!shrink.InsertModule("filter", DockSide::Top, 0.5f)) return fail(219);
        const DockBandReserve shrinkReserve = ResolveDockBandReserve(shrink, 120.0f, 220.0f, 200.0f);
        if (std::fabs(shrinkReserve.top - 120.0f) > 0.5f) return fail(220);
        if (shrinkReserve.bottom != 0.0f) return fail(221);
    }
    // P1: empty Top/Bottom first commit must resolve the full band leaf height,
    // not the preview half-strip from the hit result rect.
    {
        DockLayout emptyTop;
        if (!emptyTop.InsertModule("filter", DockSide::Top, 0.5f)) return fail(222);
        DockRootSlots bandSlots{};
        bandSlots.top = {0.0f, 80.0f, 1920.0f, 180.0f};
        const DockRect fallback{0.0f, 0.0f, 400.0f, 90.0f};
        const DockRect committed = CommittedLeafRectForModule(
            emptyTop, "filter", {0.0f, 0.0f, 1920.0f, 1080.0f}, 80.0f, 80.0f,
            &bandSlots, fallback);
        if (std::fabs(committed.height - 180.0f) > 0.5f) return fail(223);
        if (std::fabs(committed.y - 80.0f) > 0.5f) return fail(224);
    }

    // Diagnostic/regression: Info top + SimilarSounds bottom in the Right
    // Saved layouts must fit again after the viewport shrinks.
    {
        DockLayout bands;
        bands.InsertModule("mod-workflow",DockSide::Top,.5f);
        bands.InsertModule("mod-transfer",DockSide::Bottom,.5f);
        auto saved=[](std::string_view) { return 320.0f; };
        const auto shortWindow=ResolveDockBandReserve(bands,saved,220,1,500,800);
        if (!shortWindow.floatTop || !shortWindow.floatBottom ||
            shortWindow.top != 0 || shortWindow.bottom != 0) return fail(701);
        const auto oneFits=ResolveDockBandReserve(bands,saved,220,1,800,800);
        if (oneFits.floatTop || !oneFits.floatBottom || oneFits.top != 360 ||
            oneFits.bottom != 0 || oneFits.top+6+200>800) return fail(702);
        const auto narrow=ResolveDockBandReserve(bands,saved,220,1,1200,500);
        if (!narrow.floatTop || narrow.floatBottom || narrow.bottom != 360) return fail(703);
        DockLayout compact;
        compact.InsertModule("info",DockSide::Top,.5f);
        compact.InsertModule("similar-sounds",DockSide::Bottom,.5f);
        const auto fitted=ResolveDockBandReserve(compact,saved,220,1,650,700);
        if (fitted.floatTop || fitted.floatBottom || fitted.top<180 || fitted.bottom<220 ||
            fitted.top+fitted.bottom+12+200>650.01f) return fail(704);
        const auto restored=ResolveDockBandReserve(compact,saved,220,1,1200,700);
        if (restored.top!=320 || restored.bottom!=320) return fail(705);
        DockLayout mixed;
        mixed.InsertModule("info",DockSide::Top,.5f);
        mixed.InsertModule("mod-workflow",DockSide::Bottom,.5f);
        const auto tiny=ResolveDockBandReserve(mixed,saved,220,1,300,400);
        if (!tiny.floatTop || !tiny.floatBottom || tiny.top!=0 || tiny.bottom!=0) return fail(706);
    }

    // Diagnostic/regression: Info top + SimilarSounds bottom in the Right
    // region must produce only the horizontal height divider, never a vertical
    // grip on SimilarSounds left outer edge.
    {
        DockLayout stacked;
        if (!stacked.InsertModule("info", DockSide::Right, 0.28f)) return fail(225);
        if (!stacked.InsertAdjacent("info", "similar-sounds", DockSide::Bottom, 0.5f)) return fail(226);
        const DockRect rightSlot{1520.0f, 80.0f, 400.0f, 720.0f};
        DockModuleTarget rightLeaves[8]{};
        const int rightCount = CollectLeafRects(
            stacked.RegionAsSplit(DockSide::Right), rightSlot, rightLeaves, 8, 0);
        if (rightCount != 2) return fail(227);
        AdjacentLeafGrip regionGrips[4]{};
        const int regionGripCount =
            CollectAdjacentLeafGrips(rightLeaves, rightCount, regionGrips, 4);
        if (regionGripCount != 1) return fail(228);
        if (regionGrips[0].axis != Axis::Vertical) return fail(229);
        DockModuleTarget legacyColumns[2] = {
            {"filter", {0.0f, 80.0f, 400.0f, 720.0f}},
            {"info", rightSlot}};
        AdjacentLeafGrip legacyGrips[8]{};
        const int legacyCount = CollectLegacyColumnLeafGrips(
            legacyColumns, 2, rightLeaves, rightCount, legacyGrips, 8);
        for (int i = 0; i < legacyCount; ++i) {
            const bool similarInvolved =
                legacyGrips[i].firstId == "similar-sounds" ||
                legacyGrips[i].secondId == "similar-sounds";
            if (similarInvolved && legacyGrips[i].axis == Axis::Horizontal) {
                return fail(230);
            }
        }
    }

    // Docked title long-press hold state must float only after the threshold.
    {
        DockTitleGesture hold;
        if (!hold.Press("filter", false, 0.0, 100, 100)) return fail(231);
        if (std::fabs(hold.HoldProgress(0.225) - 0.5f) > 0.001f) return fail(301);
        if (hold.HoldProgress(-1.0) != 0.0f) return fail(302);
        if (hold.Advance(true, false, 0.449, 100, 100).beginDrag) return fail(232);
        if (!hold.Advance(true, false, 0.450, 100, 100).beginDrag) return fail(233);
        if (hold.HoldProgress(0.451) != 0.0f) return fail(303);
        if (hold.Advance(true, false, 0.46, 100, 100).beginDrag || hold.DropArmed()) return fail(234);
        if (!hold.Advance(false, false, 0.50, 100, 100).released) return fail(235);
        hold.Reset();
        if (hold.Active()) return fail(235);
        if (hold.HoldProgress(0.8) != 0.0f) return fail(304);
        DockTitleGesture floating;
        floating.Press("info", true, 0.0, 100, 100);
        if (floating.HoldProgress(0.225) != 0.0f) return fail(305);
        if (floating.Advance(true, false, 0.50, 100, 100).beginDrag) return fail(236);
    }

    // Detaching a stacked module must let the remaining leaf fill the region.
    {
        DockLayout collapse;
        if (!collapse.InsertModule("info", DockSide::Right, 0.28f)) return fail(237);
        if (!collapse.InsertAdjacent("info", "similar-sounds", DockSide::Bottom, 0.5f)) return fail(238);
        if (collapse.CountInRootRegion(DockSide::Right) != 2) return fail(239);
        collapse.Detach("info");
        if (collapse.Find("info") != nullptr) return fail(240);
        if (collapse.HasEmptySplit()) return fail(241);
        if (collapse.CountInRootRegion(DockSide::Right) != 1) return fail(242);
        if (collapse.Find("similar-sounds") == nullptr) return fail(243);
    }

    // Every module, including file results, uses exactly the same policy.
    // Once pressed, the original title need not remain hovered.
    {
        for (const char* module : {"filter", "info", "similar-sounds", "search-results"}) {
            DockTitleGesture gesture;
            if (gesture.Advance(true, false, 10.0, 100, 100).beginDrag) return fail(244);
            if (!gesture.Press(module, false, 0.0, 100, 100)) return fail(245);
            if (gesture.Press("mod-other", false, 0.01, 200, 200)) return fail(246);
            if (gesture.Advance(true, false, 0.1, 105, 100).beginDrag) return fail(247);
            if (!gesture.Advance(true, false, 0.2, 106, 100).beginDrag) return fail(248);
            if (!gesture.DropArmed() || !gesture.Owns(module)) return fail(249);
            if (!gesture.Advance(true, true, 0.3, 110, 100).cancelled) return fail(250);
            gesture.Reset();
            if (gesture.Active()) return fail(251);
            gesture.Press(module, false, 0.0, 100, 100, 2.0f);
            if (gesture.Advance(true, false, 0.1, 111, 100).beginDrag) return fail(252);
            if (!gesture.Advance(true, false, 0.2, 112, 100).beginDrag) return fail(253);
            gesture.Reset();
            gesture.Press(module, false, 0.0, 100, 100);
            auto shortClick = gesture.Advance(false, false, 0.1, 100, 100);
            if (!shortClick.released || shortClick.beginDrag || gesture.Dragging()) return fail(254);
            gesture.Reset();
            gesture.Press(module, true, 0.0, 100, 100);
            if (gesture.Advance(true, false, 1.0, 100, 100).beginDrag) return fail(255);
            if (!gesture.Advance(true, false, 1.1, 106, 100).beginDrag) return fail(256);
            if (!gesture.FromFloating()) return fail(257);
        }
    }

    // A shared geometry snapshot must refresh leaves and grips after float,
    // while the floating rect remains independent from its old Dock budget.
    {
        const DockRect work{0.0f, 0.0f, 1920.0f, 1080.0f};
        DockRootSlots slots{
            {0.0f, 80.0f, 400.0f, 720.0f},
            {1520.0f, 80.0f, 400.0f, 720.0f},
            {0.0f, 80.0f, 1920.0f, 180.0f},
            {0.0f, 900.0f, 1920.0f, 180.0f}};

        DockLayout rightStack;
        if (!rightStack.InsertModule("info", DockSide::Right, 0.28f)) return fail(250);
        if (!rightStack.InsertAdjacent("info", "similar-sounds", DockSide::Bottom, 0.5f)) return fail(251);
        DockFrameGeometry rightBefore = BuildDockFrameGeometry(rightStack, work, 80.0f, 40.0f, slots);
        if (rightBefore.moduleCount != 2 || rightBefore.gripCount != 1) return fail(252);
        if (!rightBefore.grips[0].visible || rightBefore.grips[0].axis != Axis::Vertical) return fail(253);
        DockSplitGrip visibleGrip{rightBefore.grips[0].rect, rightBefore.grips[0].axis, DockSide::Right, true};
        const DockRect gripPointer{visibleGrip.rect.x + 2.0f, visibleGrip.rect.y + visibleGrip.rect.height * 0.5f - 2.0f, 4.0f, 4.0f};
        if (!HitSplitGrip(gripPointer, visibleGrip)) return fail(254);

        DockRect infoLeaf{};
        if (!FindCommittedDockModuleRect(rightStack, "info", work, 80.0f, 40.0f, slots, infoLeaf)) return fail(255);
        PanelDockState storedInfo;
        storedInfo.dockedWidth = 330.0f;
        storedInfo.dockedHeight = 280.0f;
        if (!BeginDockPanelFloat(rightStack, "info", storedInfo, infoLeaf)) return fail(256);
        if (std::fabs(storedInfo.floatW - infoLeaf.width) > 0.01f ||
            std::fabs(storedInfo.floatH - infoLeaf.height) > 0.01f) return fail(257);
        DockFrameGeometry rightAfter = BuildDockFrameGeometry(rightStack, work, 80.0f, 40.0f, slots);
        if (rightAfter.moduleCount != 1 || rightAfter.gripCount != 0) return fail(258);
        if (rightAfter.modules[0].id != "similar-sounds") return fail(259);
        if (std::fabs(rightAfter.modules[0].rect.height - slots.right.height) > 0.01f) return fail(260);
        const DockRect similarExpanded = rightAfter.modules[0].rect;
        if (!UpdateFloatingDockPanelGeometry(storedInfo, {55.0f, 65.0f, 810.0f, 510.0f})) return fail(261);
        DockFrameGeometry afterResize = BuildDockFrameGeometry(rightStack, work, 80.0f, 40.0f, slots);
        if (std::fabs(afterResize.modules[0].rect.width - similarExpanded.width) > 0.01f ||
            std::fabs(afterResize.modules[0].rect.height - similarExpanded.height) > 0.01f) return fail(262);
        if (std::fabs(storedInfo.floatW - 810.0f) > 0.01f ||
            std::fabs(storedInfo.floatH - 510.0f) > 0.01f) return fail(263);
        if (std::fabs(storedInfo.dockedWidth - 330.0f) > 0.01f ||
            std::fabs(storedInfo.dockedHeight - 280.0f) > 0.01f) return fail(2631);
        rightStack.RemoveFloat("info");
        if (!rightStack.InsertAdjacent("similar-sounds", "info", DockSide::Top, 0.5f)) return fail(264);
        DockFrameGeometry redocked = BuildDockFrameGeometry(rightStack, work, 80.0f, 40.0f, slots);
        if (redocked.moduleCount != 2 || redocked.gripCount != 1) return fail(265);
        DockRect committedInfo{};
        if (!FindCommittedDockModuleRect(rightStack, "info", work, 80.0f, 40.0f, slots, committedInfo)) return fail(266);
        PanelDockState infoState = storedInfo;
        ApplyDockCommitToPanel(infoState, DockSide::Right, DockSide::Top, committedInfo);
        if (infoState.dockSide != DockSideToSavedSide(DockSide::Right)) return fail(267);
        if (std::fabs(infoState.dockedHeight - committedInfo.height) > 0.01f) return fail(268);
        if (std::fabs(infoState.dockedWidth - 330.0f) > 0.01f) return fail(269);
    }

    // Filter and Search Results share the same float, reflow, and re-dock path.
    {
        const DockRect work{0.0f, 0.0f, 1920.0f, 1080.0f};
        DockRootSlots slots{
            {0.0f, 80.0f, 400.0f, 720.0f},
            {1520.0f, 80.0f, 400.0f, 720.0f},
            {0.0f, 80.0f, 1920.0f, 180.0f},
            {0.0f, 900.0f, 1920.0f, 180.0f}};
        DockLayout leftSplit;
        if (!leftSplit.InsertModule("filter", DockSide::Left, 0.24f)) return fail(270);
        if (!leftSplit.InsertAdjacent("filter", "search-results", DockSide::Right, 0.5f)) return fail(271);
        DockFrameGeometry before = BuildDockFrameGeometry(leftSplit, work, 80.0f, 40.0f, slots);
        if (before.moduleCount != 2 || before.gripCount != 1 || !before.grips[0].visible) return fail(272);
        DockRect filterLeaf{};
        if (!FindCommittedDockModuleRect(leftSplit, "filter", work, 80.0f, 40.0f, slots, filterLeaf)) return fail(273);
        bool floating = false;
        float x = 0.0f, y = 0.0f, width = 0.0f, height = 0.0f;
        if (!BeginDockPanelFloat(leftSplit, "filter", floating, x, y, width, height, filterLeaf)) return fail(274);
        DockFrameGeometry afterFloat = BuildDockFrameGeometry(leftSplit, work, 80.0f, 40.0f, slots);
        if (afterFloat.moduleCount != 1 || afterFloat.gripCount != 0) return fail(275);
        if (afterFloat.modules[0].id != "search-results" ||
            std::fabs(afterFloat.modules[0].rect.width - slots.left.width) > 0.01f) return fail(276);
        if (!UpdateFloatingDockPanelGeometry(floating, x, y, width, height, {90.0f, 100.0f, 760.0f, 460.0f})) return fail(277);
        if (std::fabs(width - 760.0f) > 0.01f || std::fabs(height - 460.0f) > 0.01f) return fail(278);
        leftSplit.RemoveFloat("filter");
        if (!leftSplit.InsertAdjacent("search-results", "filter", DockSide::Left, 0.5f)) return fail(279);
        DockFrameGeometry afterDock = BuildDockFrameGeometry(leftSplit, work, 80.0f, 40.0f, slots);
        if (afterDock.moduleCount != 2 || afterDock.gripCount != 1) return fail(280);
    }

    // Releasing Filter removes its legacy column width from the Results budget;
    // a remaining Search Results leaf still expands through its Dock root.
    {
        const DockColumnBudget before = ResolveDockColumnBudget(
            1600.0f, true, true, 320.0f, 300.0f, 12.0f, 500.0f, 150.0f);
        const DockColumnBudget after = ResolveDockColumnBudget(
            1600.0f, false, true, 0.0f, 300.0f, 12.0f, 500.0f, 150.0f);
        if (!before.showLeft || after.showLeft || after.leftWidth != 0.0f) return fail(2701);
        if (after.centerWidth <= before.centerWidth + 320.0f) return fail(2702);
    }

    // A lone Similar Sounds leaf has no leaf-to-leaf grip. The outer right
    // column width still belongs to its own persisted width and gives Results
    // the inverse budget when the visible boundary is dragged.
    {
        DockLayout loneSimilar;
        if (!loneSimilar.InsertModule("similar-sounds", DockSide::Right, 0.5f)) return fail(303);
        const DockColumnBudget before = ResolveDockColumnBudget(
            1600.0f, false, true, 0.0f, 300.0f, 12.0f, 500.0f, 150.0f);
        const DockColumnBudget after = ResolveDockColumnBudget(
            1600.0f, false, true, 0.0f, 360.0f, 12.0f, 500.0f, 150.0f);
        if (before.centerWidth - after.centerWidth != 60.0f ||
            after.rightWidth - before.rightWidth != 60.0f) return fail(304);
        const DockRect work{0.0f, 0.0f, 1600.0f, 1000.0f};
        DockRootSlots beforeSlots{};
        beforeSlots.right = {1600.0f - before.rightWidth, 80.0f, before.rightWidth, 720.0f};
        DockRootSlots afterSlots{};
        afterSlots.right = {1600.0f - after.rightWidth, 80.0f, after.rightWidth, 720.0f};
        const DockFrameGeometry oldFrame = BuildDockFrameGeometry(
            loneSimilar, work, 80.0f, 40.0f, beforeSlots);
        const DockFrameGeometry newFrame = BuildDockFrameGeometry(
            loneSimilar, work, 80.0f, 40.0f, afterSlots);
        if (oldFrame.moduleCount != 1 || newFrame.moduleCount != 1 ||
            oldFrame.gripCount != 0 || newFrame.gripCount != 0 ||
            newFrame.modules[0].id != "similar-sounds" ||
            newFrame.modules[0].rect.width - oldFrame.modules[0].rect.width != 60.0f) return fail(305);
        const AdjacentLeafGrip outer = PlanRightDockOuterGrip(newFrame, "similar-sounds");
        if (!outer.visible || outer.axis != Axis::Horizontal ||
            outer.rect.x + outer.rect.width * 0.5f != newFrame.rootSlots.right.x) return fail(306);
        float resizedWidth = 300.0f;
        if (!DragRightDockOuterGrip(resizedWidth, -60.0f, 150.0f, 600.0f) ||
            resizedWidth != 360.0f) return fail(307);
    }

    if (ShouldEvaluateFloatingDockZone(false, false, false)) return fail(2703);
    if (!ShouldEvaluateFloatingDockZone(true, false, false)) return fail(2704);
    if (!ShouldEvaluateFloatingDockZone(false, true, false)) return fail(2705);
    if (!ShouldEvaluateFloatingDockZone(false, false, true)) return fail(2706);

    // Generic test modules leave product leaves and the fixed waveform alone.
    {
        const DockRect work{0.0f, 0.0f, 1920.0f, 1080.0f};
        DockRootSlots slots{
            {0.0f, 80.0f, 400.0f, 720.0f},
            {1520.0f, 80.0f, 400.0f, 720.0f},
            {0.0f, 80.0f, 1920.0f, 180.0f},
            {0.0f, 900.0f, 1920.0f, 180.0f}};
        DockLayout layoutWithTestPanel;
        if (!layoutWithTestPanel.InsertModule("info", DockSide::Right, 0.28f)) return fail(281);
        if (!layoutWithTestPanel.InsertAdjacent("info", "similar-sounds", DockSide::Bottom, 0.5f)) return fail(282);
        if (!layoutWithTestPanel.InsertModule("mod-docktest-stress", DockSide::Bottom, 0.2f)) return fail(283);
        DockFrameGeometry before = BuildDockFrameGeometry(layoutWithTestPanel, work, 80.0f, 40.0f, slots);
        DockRect similarBefore{};
        if (!FindCommittedDockModuleRect(layoutWithTestPanel, "similar-sounds", work, 80.0f, 40.0f, slots, similarBefore)) return fail(284);
        DockRect stressLeaf{};
        if (!FindCommittedDockModuleRect(layoutWithTestPanel, "mod-docktest-stress", work, 80.0f, 40.0f, slots, stressLeaf)) return fail(285);
        bool floating = false;
        float x = 0.0f, y = 0.0f, width = 0.0f, height = 0.0f;
        if (!BeginDockPanelFloat(layoutWithTestPanel, "mod-docktest-stress", floating, x, y, width, height, stressLeaf)) return fail(286);
        if (std::fabs(width - stressLeaf.width) > 0.01f || std::fabs(height - stressLeaf.height) > 0.01f) return fail(287);
        DockFrameGeometry after = BuildDockFrameGeometry(layoutWithTestPanel, work, 80.0f, 40.0f, slots);
        DockRect similarAfter{};
        if (!FindCommittedDockModuleRect(layoutWithTestPanel, "similar-sounds", work, 80.0f, 40.0f, slots, similarAfter)) return fail(288);
        if (std::fabs(similarAfter.x - similarBefore.x) > 0.01f || std::fabs(similarAfter.y - similarBefore.y) > 0.01f ||
            std::fabs(similarAfter.width - similarBefore.width) > 0.01f || std::fabs(similarAfter.height - similarBefore.height) > 0.01f) return fail(289);
        if (before.moduleCount != after.moduleCount + 1 || IsKnownModuleId("waveform")) return fail(290);
    }

    return 0;
}
