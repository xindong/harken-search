#include "DockDragSession.h"
#include "ui/dock/DockPanelStateBridge.h"
#include <cstdio>

int main() {
    using namespace aura::ui::dock;
    for (const char* id : {"filter", "info", "similar-sounds", "search-results",
                          "mod-ucs-browser", "mod-tag-browser", "mod-workflow", "mod-transfer"}) {
        for (DockSide side : {DockSide::Top, DockSide::Bottom}) {
            DockLayout vertical;
            if (!vertical.InsertModule(id, side, .24f)) return 230;
            auto saved = [&](std::string_view module) { return module == id ? 280.0f : 0.0f; };
            const auto band = ResolveDockBandReserve(vertical, saved, 120.0f, 1.0f);
            const float actual = side == DockSide::Top ? band.top : band.bottom;
            if (actual != std::max(280.0f, DockModuleMinimum(id).height)) return 231;
            if ((side == DockSide::Top ? band.bottom : band.top) != 0.0f) return 232;
        }
    }
    {
        DockLayout workspace;
        workspace.InsertModule("filter", DockSide::Left, 0.24f);
        const DockLayout before = workspace;
        DockDragSession results(workspace);
        if (results.BeginDrag("search-results")) return 224;
        if (!results.BeginDrag("search-results", true)) return 225;
        workspace.EnsureFloat("search-results");
        results.Cancel();
        if (!workspace.Equals(before)) return 226;
    }
    // One press must keep its original owner and rollback snapshot, including
    // after the docked leaf has been detached into a floating window.
    for (const char* owner : {"filter", "info", "search-results", "similar-sounds"}) {
        DockLayout owned;
        owned.InsertModule(owner, DockSide::Left, 0.24f);
        const char* other = std::string_view(owner) == "info" ? "filter" : "info";
        owned.InsertModule(other, DockSide::Right, 0.28f);
        const DockLayout original = owned;
        DockDragSession gesture(owned);
        if (!gesture.BeginDrag(owner)) return 220;
        if (gesture.BeginDrag(other)) {
            std::fprintf(stderr, "another window stole the active drag: %s -> %s\n", owner, other);
            return 221;
        }
        owned.Detach(owner);
        if (gesture.BeginDrag(owner)) {
            std::fprintf(stderr, "re-entry replaced the pre-detach snapshot: %s\n", owner);
            return 222;
        }
        gesture.Cancel();
        if (!owned.Equals(original)) return 223;
        owned.Detach(owner);
        if (!gesture.BeginDrag(owner)) return 227;
        DockHit preview;
        preview.kind = DockHitKind::Module;
        preview.targetModuleId = other;
        preview.side = DockSide::Left;
        gesture.UpdatePreview(preview);
        // The pointer can leave a valid target just before mouse-up. The last
        // live hit (None) must discard the former highlight for every module.
        gesture.UpdatePreview({});
        if (gesture.CommitOnMouseUp().kind != DockHitKind::None) return 228;
        if (owned.Find(owner) || !owned.FindFloat(owner)) return 229;
    }
    DockLayout layout;
    layout.InsertModule("filter", DockSide::Left, 0.24f);
    layout.InsertModule("info", DockSide::Right, 0.28f);
    DockDragSession session(layout);

    DockLayout before = layout;
    session.BeginDrag("info");
    if (!session.OwnsDrag("info")) return 2;
    if (session.OwnsDrag("filter")) return 3;
    DockHit hitNone;
    session.UpdatePreview(hitNone);
    if (!layout.Equals(before)) return 1;
    session.CommitOnMouseUp();
    if (session.OwnsDrag("info")) return 4;
    if (layout.FindFloat("info") == nullptr) return 1;

    session.BeginDrag("info");
    DockHit hitLeftStrip;
    hitLeftStrip.kind = DockHitKind::Main;
    hitLeftStrip.side = DockSide::Left;
    session.UpdatePreview(hitLeftStrip);
    session.Cancel();
    if (layout.FindFloat("info") == nullptr) return 1;

    DockLayout splitLayout;
    splitLayout.InsertModule("info", DockSide::Right, 0.28f);
    splitLayout.EnsureFloat("filter");
    DockDragSession splitSession(splitLayout);
    splitSession.BeginDrag("filter");
    DockHit moduleHit;
    moduleHit.kind = DockHitKind::Module;
    moduleHit.side = DockSide::Left;
    moduleHit.targetModuleId = "info";
    splitSession.UpdatePreview(moduleHit);
    splitSession.CommitOnMouseUp();
    if (splitLayout.Find("filter") == nullptr) return 21;
    if (splitLayout.Find("info") == nullptr) return 22;
    if (splitLayout.FindFloat("filter") != nullptr) return 23;
    SplitNode* split = splitLayout.RegionAsSplit(DockSide::Right);
    if (!split) split = splitLayout.RootAsSplit();
    if (!split) return 24;
    if (splitLayout.CountInRootRegion(DockSide::Right) != 2) return 25;

    DockLayout alreadyDocked;
    alreadyDocked.InsertModule("filter", DockSide::Left, 0.24f);
    alreadyDocked.InsertModule("info", DockSide::Right, 0.28f);
    DockDragSession alreadySession(alreadyDocked);
    alreadySession.BeginDrag("filter");
    DockHit alreadyHit;
    alreadyHit.kind = DockHitKind::Module;
    alreadyHit.side = DockSide::Left;
    alreadyHit.targetModuleId = "info";
    alreadySession.UpdatePreview(alreadyHit);
    alreadySession.CommitOnMouseUp();
    if (alreadyDocked.Find("filter") == nullptr) return 31;
    if (alreadyDocked.CountInRootRegion(DockSide::Left) != 0) return 32;
    if (alreadyDocked.CountInRootRegion(DockSide::Right) != 2) return 33;
    if (!alreadyDocked.RegionAsSplit(DockSide::Right) && !alreadyDocked.RootAsSplit()) return 34;

    if (!IsKnownModuleId("similar-sounds")) return 41;
    DockLayout similar;
    similar.InsertModule("info", DockSide::Right, 0.28f);
    if (!similar.InsertAdjacent("info", "similar-sounds", DockSide::Left, 0.5f)) return 42;
    if (similar.Find("similar-sounds") == nullptr) return 43;
    if (similar.CountInRootRegion(DockSide::Right) != 2) return 44;

    DockLayout openFloat;
    openFloat.InsertModule("filter", DockSide::Left, 0.24f);
    if (!openFloat.EnsureFloat("similar-sounds")) return 51;
    if (openFloat.Find("similar-sounds") != nullptr) return 52;
    if (openFloat.FindFloat("similar-sounds") == nullptr) return 53;
    if (ShouldPreviewDockHit(false, false)) return 54;
    if (!ShouldPreviewDockHit(true, true)) return 55;
    DockRect similarOpen{0.0f, 80.0f, 8.0f, 900.0f};
    DockRect work{0.0f, 0.0f, 1920.0f, 1080.0f};
    DockRootSlots slots{};
    slots.left = {0.0f, 80.0f, 400.0f, 620.0f};
    DockHit clickHit = HitDockTarget(similarOpen, work, 80.0f, nullptr, 0, &slots);
    if (ShouldDrawDockPreview(false, clickHit)) return 56;
    if (ShouldPreviewDockHit(false, true)) return 57;

    DockLayout releaseLayout;
    releaseLayout.EnsureFloat("info");
    DockDragSession releaseSession(releaseLayout);
    if (!releaseSession.BeginDrag("info")) return 61;
    DockHit previewRight;
    previewRight.kind = DockHitKind::Main;
    previewRight.side = DockSide::Right;
    previewRight.rootRegion = DockSide::Right;
    releaseSession.UpdatePreview(previewRight);
    DockHit leftover = previewRight;
    leftover.kind = DockHitKind::None;
    leftover.targetModuleId.clear();
    const DockHit committed = CommitPreviewThenApply(releaseSession, leftover);
    if (committed.kind != DockHitKind::Main || committed.side != DockSide::Right) return 62;
    if (releaseLayout.Find("info") == nullptr) return 63;
    if (releaseLayout.FindFloat("info") != nullptr) return 64;
    if (releaseLayout.CountInRootRegion(DockSide::Right) != 1) return 65;

    DockLayout sides;
    if (sides.EnsureFloat("waveform")) return 66;
    DockDragSession rejectedWaveform(sides);
    if (rejectedWaveform.BeginDrag("waveform")) return 67;
    if (!sides.EnsureFloat("similar-sounds")) return 68;
    DockDragSession sidesSession(sides);
    if (!sidesSession.BeginDrag("similar-sounds")) return 69;
    DockHit previewBottom;
    previewBottom.kind = DockHitKind::Main;
    previewBottom.side = DockSide::Bottom;
    sidesSession.UpdatePreview(previewBottom);
    DockHit emptied;
    emptied.kind = DockHitKind::None;
    const DockHit bottomCommitted = CommitPreviewThenApply(sidesSession, emptied);
    if (bottomCommitted.kind != DockHitKind::Main || bottomCommitted.side != DockSide::Bottom) return 70;
    if (sides.Find("similar-sounds") == nullptr) return 71;

    DockLayout floatGeometryLayout;
    if (!floatGeometryLayout.InsertModule("info", DockSide::Right, 0.28f)) return 81;
    DockRect floatLeaf{};
    if (!FindCommittedDockModuleRect(
            floatGeometryLayout, "info", work, 80.0f, 40.0f, slots, floatLeaf)) return 82;
    PanelDockState floatGeometryPanel;
    floatGeometryPanel.dockedWidth = 345.0f;
    floatGeometryPanel.dockedHeight = 295.0f;
    if (!BeginDockPanelFloat(floatGeometryLayout, "info", floatGeometryPanel, floatLeaf)) return 83;
    if (!UpdateFloatingDockPanelGeometry(floatGeometryPanel, {140.0f, 150.0f, 810.0f, 510.0f})) return 84;
    DockDragSession floatGeometrySession(floatGeometryLayout);
    if (!floatGeometrySession.BeginDrag("info")) return 85;
    DockHit noHit;
    floatGeometrySession.UpdatePreview(noHit);
    floatGeometrySession.CommitFloatingOnMouseUp();
    if (!floatGeometryLayout.FindFloat("info") ||
        std::fabs(floatGeometryPanel.floatX - 140.0f) > 0.01f ||
        std::fabs(floatGeometryPanel.floatY - 150.0f) > 0.01f ||
        std::fabs(floatGeometryPanel.floatW - 810.0f) > 0.01f ||
        std::fabs(floatGeometryPanel.floatH - 510.0f) > 0.01f ||
        std::fabs(floatGeometryPanel.dockedWidth - 345.0f) > 0.01f ||
        std::fabs(floatGeometryPanel.dockedHeight - 295.0f) > 0.01f) return 86;

    DockLayout leftLayout;
    leftLayout.EnsureFloat("filter");
    DockDragSession leftSession(leftLayout);
    if (!leftSession.BeginDrag("filter")) return 81;
    DockHit previewLeft;
    previewLeft.kind = DockHitKind::Main;
    previewLeft.side = DockSide::Left;
    leftSession.UpdatePreview(previewLeft);
    DockHit emptyLeft;
    emptyLeft.kind = DockHitKind::None;
    const DockHit leftCommitted = CommitPreviewThenApply(leftSession, emptyLeft);
    if (leftCommitted.kind != DockHitKind::Main || leftCommitted.side != DockSide::Left) return 82;
    if (leftLayout.Find("filter") == nullptr) return 83;

    // A title-hold handoff must stay floating when release has no legal dock
    // target, even if a preview was left over from an earlier frame.
    for (int count = 1; count <= 3; ++count) {
        DockLayout floatingRelease;
        floatingRelease.InsertModule("info", DockSide::Right, 0.28f);
        if (count >= 2) floatingRelease.InsertModule("filter", DockSide::Left, 0.24f);
        if (count >= 3) floatingRelease.InsertModule("search-results", DockSide::Top, 0.24f);
        if (floatingRelease.Find("info") == nullptr) return 831 + count;
        DockDragSession floatingSession(floatingRelease);
        if (!floatingSession.BeginDrag("info")) return 834 + count;
        DockHit stale;
        stale.kind = DockHitKind::Main;
        stale.side = DockSide::Left;
        floatingSession.UpdatePreview(stale);
        floatingSession.CommitFloatingOnMouseUp();
        if (floatingRelease.Find("info") != nullptr || floatingRelease.FindFloat("info") == nullptr) {
            return 837 + count;
        }
    }

    DockLayout topLayout;
    topLayout.EnsureFloat("search-results");
    DockDragSession topSession(topLayout);
    if (!topSession.BeginDrag("search-results")) return 84;
    DockHit previewTop;
    previewTop.kind = DockHitKind::Main;
    previewTop.side = DockSide::Top;
    topSession.UpdatePreview(previewTop);
    DockHit emptyTop;
    emptyTop.kind = DockHitKind::None;
    const DockHit topCommitted = CommitPreviewThenApply(topSession, emptyTop);
    if (topCommitted.kind != DockHitKind::Main || topCommitted.side != DockSide::Top) return 85;
    if (topLayout.Find("search-results") == nullptr) return 86;

    DockLayout moduleLayout;
    moduleLayout.InsertModule("info", DockSide::Right, 0.28f);
    {
        // Docking onto a leaf must give the incoming window a readable share
        // of that leaf, not a thin strip.
        DockLayout shareLayout;
        if (!shareLayout.InsertModule("filter", DockSide::Left, 0.24f)) return 91;
        if (!shareLayout.EnsureFloat("similar-sounds")) return 92;
        DockDragSession shareSession(shareLayout);
        if (!shareSession.BeginDrag("similar-sounds")) return 92;
        DockHit moduleShare;
        moduleShare.kind = DockHitKind::Module;
        moduleShare.side = DockSide::Right;
        moduleShare.targetModuleId = "filter";
        shareSession.UpdatePreview(moduleShare);
        shareSession.CommitOnMouseUp();
        const DockRect leafBox{0.0f, 0.0f, 400.0f, 900.0f};
        DockModuleTarget shareLeaves[4]{};
        const int shareCount = CollectLeafRects(shareLayout.RegionAsSplit(DockSide::Left), leafBox, shareLeaves, 4, 0);
        if (shareCount != 2) return 93;
        const DockRect* incoming = nullptr;
        const DockRect* original = nullptr;
        for (int i = 0; i < shareCount; ++i) {
            if (shareLeaves[i].id == "similar-sounds") incoming = &shareLeaves[i].rect;
            if (shareLeaves[i].id == "filter") original = &shareLeaves[i].rect;
        }
        if (!incoming || !original) return 94;
        if (incoming->width < leafBox.width * 0.4f) return 95;
        if (original->width < leafBox.width * 0.4f) return 96;
    }
    moduleLayout.EnsureFloat("similar-sounds");
    DockDragSession moduleSession(moduleLayout);
    if (!moduleSession.BeginDrag("similar-sounds")) return 87;
    DockHit previewModule;
    previewModule.kind = DockHitKind::Module;
    previewModule.side = DockSide::Left;
    previewModule.targetModuleId = "info";
    moduleSession.UpdatePreview(previewModule);
    DockHit emptyModule;
    emptyModule.kind = DockHitKind::None;
    const DockHit moduleCommitted = CommitPreviewThenApply(moduleSession, emptyModule);
    if (moduleCommitted.kind != DockHitKind::Module || moduleCommitted.targetModuleId != "info") return 88;
    if (moduleLayout.Find("similar-sounds") == nullptr) return 89;
    if (moduleLayout.FindFloat("similar-sounds") != nullptr) return 90;
    if (moduleLayout.CountInRootRegion(DockSide::Right) != 2) return 91;
    if (!moduleLayout.RegionAsSplit(DockSide::Right)) return 92;

    DockLayout occupied;
    occupied.InsertModule("info", DockSide::Right, 0.28f);
    occupied.EnsureFloat("filter");
    DockDragSession occupiedSession(occupied);
    if (!occupiedSession.BeginDrag("filter")) return 93;
    DockHit previewIntoOccupied;
    previewIntoOccupied.kind = DockHitKind::Main;
    previewIntoOccupied.side = DockSide::Right;
    occupiedSession.UpdatePreview(previewIntoOccupied);
    DockHit emptyOccupied;
    emptyOccupied.kind = DockHitKind::None;
    const DockHit occupiedCommitted = CommitPreviewThenApply(occupiedSession, emptyOccupied);
    if (occupiedCommitted.kind != DockHitKind::Main || occupiedCommitted.side != DockSide::Right) return 94;
    if (occupied.Find("filter") == nullptr || occupied.Find("info") == nullptr) return 95;
    if (occupied.CountInRootRegion(DockSide::Right) != 2) return 96;
    if (!occupied.RegionAsSplit(DockSide::Right)) return 97;

    const DockRect infoBox{1520.0f, 80.0f, 400.0f, 900.0f};
    const DockSide fourWays[4] = {DockSide::Left, DockSide::Right, DockSide::Top, DockSide::Bottom};
    for (int i = 0; i < 4; ++i) {
        DockLayout four;
        four.InsertModule("info", DockSide::Right, 0.28f);
        four.EnsureFloat("similar-sounds");
        DockDragSession fourSession(four);
        if (!fourSession.BeginDrag("similar-sounds")) return 101 + i;
        DockHit edge;
        edge.kind = DockHitKind::Module;
        edge.side = fourWays[i];
        edge.targetModuleId = "info";
        edge.resultRect = SplitIncomingRect(infoBox, fourWays[i], 0.24f);
        fourSession.UpdatePreview(edge);
        DockHit leftoverNone;
        leftoverNone.kind = DockHitKind::None;
        const DockHit done = CommitPreviewThenApply(fourSession, leftoverNone);
        if (done.kind != DockHitKind::Module || done.side != fourWays[i]) return 111 + i;
        if (four.Find("info") == nullptr || four.Find("similar-sounds") == nullptr) return 121 + i;
        if (four.FindFloat("similar-sounds") != nullptr) return 131 + i;
        SplitNode* split = four.RegionAsSplit(DockSide::Right);
        if (!split) return 141 + i;
        DockModuleTarget leaves[8]{};
        const int leafCount = CollectLeafRects(split, infoBox, leaves, 8, 0);
        if (leafCount != 2) return 151 + i;
        if (leaves[0].rect.width <= 1.0f || leaves[0].rect.height <= 1.0f) return 161 + i;
        if (leaves[1].rect.width <= 1.0f || leaves[1].rect.height <= 1.0f) return 171 + i;
        bool sawIncoming = false;
        bool sawOriginal = false;
        for (int leaf = 0; leaf < leafCount; ++leaf) {
            if (leaves[leaf].id == "similar-sounds") sawIncoming = true;
            if (leaves[leaf].id == "info") sawOriginal = true;
        }
        if (!sawIncoming || !sawOriginal) return 181 + i;
    }

    // Full second-split path: build the same frame snapshot used for hit
    // testing, preview the second leaf edge, commit it, then rebuild leaves
    // and grips from the mutated root.
    {
        DockLayout nested;
        if (!nested.InsertModule("info", DockSide::Right, 0.28f)) return 191;
        if (!nested.InsertAdjacent("info", "similar-sounds", DockSide::Bottom, 0.5f)) return 192;
        DockRootSlots slots{};
        slots.right = {1200.0f, 80.0f, 400.0f, 720.0f};
        DockRect work{0.0f, 0.0f, 1600.0f, 900.0f};
        DockModuleTarget leaves[8]{};
        int count = CollectLeafRects(nested.RegionRoot(DockSide::Right), slots.right, leaves, 8, 0);
        const DockModuleTarget* similar = nullptr;
        for (int i = 0; i < count; ++i) if (leaves[i].id == "similar-sounds") similar = &leaves[i];
        if (!similar) return 193;
        const float edge = std::min(80.0f, std::min(similar->rect.width, similar->rect.height));
        DockRect panel{similar->rect.x + 40.0f, similar->rect.y + edge * 0.2f, 100.0f, edge * 0.6f};
        DockHit hit = HitDockTarget(panel, work, 80.0f, leaves, count, &slots, &nested);
        if (hit.kind != DockHitKind::Module || hit.targetModuleId != "similar-sounds" || hit.side != DockSide::Top) return 194;
        if (!nested.EnsureFloat("search-results")) return 195;
        DockDragSession session(nested);
        if (!session.BeginDrag("search-results")) return 196;
        session.UpdatePreview(hit);
        const DockHit committed = session.CommitOnMouseUp();
        if (committed.kind != DockHitKind::Module || committed.targetModuleId != "similar-sounds") return 197;
        if (nested.Find("search-results") == nullptr || nested.FindFloat("search-results") != nullptr) return 198;
        count = CollectLeafRects(nested.RegionRoot(DockSide::Right), slots.right, leaves, 8, 0);
        if (count != 3) return 199;
        AdjacentLeafGrip grips[8]{};
        const int gripCount = CollectAdjacentLeafGrips(leaves, count, grips, 8);
        if (gripCount < 2) return 200;
    }

    // Screenshot-shaped path: the Dock Band Test starts in the Top root,
    // receives a first vertical split, then a third independent module is
    // dropped onto the second leaf. The fixed waveform slot is unchanged.
    {
        DockLayout band;
        if (!band.InsertModule("mod-docktest-band", DockSide::Top, 0.24f)) return 201;
        if (!band.InsertAdjacent("mod-docktest-band", "mod-docktest-info", DockSide::Bottom, 0.5f)) return 202;
        DockRootSlots slots{};
        slots.top = {0.0f, 80.0f, 1600.0f, 180.0f};
        slots.bottom = {0.0f, 760.0f, 1600.0f, 100.0f};
        const DockRect work{0.0f, 0.0f, 1600.0f, 900.0f};
        const float waveformY = slots.bottom.y;
        DockModuleTarget leaves[8]{};
        int count = CollectLeafRects(band.RegionRoot(DockSide::Top), slots.top, leaves, 8, 0);
        const DockModuleTarget* infoLeaf = nullptr;
        for (int i = 0; i < count; ++i) if (leaves[i].id == "mod-docktest-info") infoLeaf = &leaves[i];
        if (!infoLeaf || infoLeaf->rect.y + infoLeaf->rect.height > waveformY) return 203;
        const float edge = std::min(80.0f, std::min(infoLeaf->rect.width, infoLeaf->rect.height));
        const DockRect panel{infoLeaf->rect.x + 40.0f, infoLeaf->rect.y + edge * 0.2f, 100.0f, edge * 0.6f};
        const DockHit hit = HitDockTarget(panel, work, 80.0f, leaves, count, &slots, &band);
        if (hit.kind != DockHitKind::Module || hit.targetModuleId != "mod-docktest-info" || hit.side != DockSide::Top) return 204;
        if (!band.EnsureFloat("mod-docktest-similar")) return 205;
        DockDragSession drag(band);
        if (!drag.BeginDrag("mod-docktest-similar")) return 206;
        drag.UpdatePreview(hit);
        if (drag.CommitOnMouseUp().kind != DockHitKind::Module) return 207;
        if (band.Find("mod-docktest-similar") == nullptr) return 208;
        count = CollectLeafRects(band.RegionRoot(DockSide::Top), slots.top, leaves, 8, 0);
        if (count != 3) return 209;
        AdjacentLeafGrip grips[8]{};
        if (CollectAdjacentLeafGrips(leaves, count, grips, 8) < 2) return 210;
        if (slots.bottom.y != waveformY || slots.bottom.height != 100.0f) return 211;
    }

    SimilarSoundsOpenResult opened = ResolveSimilarSoundsOpenClick(true, false);
    // Preview and commit agree for all four modules and target DPI scales.
    for (float scale : {1.0f, 1.5f, 2.0f}) {
        for (const char* module : {"filter", "info", "similar-sounds", "search-results"}) {
            const char* target = std::string_view(module) == "filter" ? "info" : "filter";
            DockLayout layout;
            layout.InsertModule(target, DockSide::Right, .24f);
            layout.EnsureFloat(module);
            const DockRect work{0, 0, 1800 * scale, 1200 * scale};
            DockRootSlots slots{};
            slots.right = {1080 * scale, 80 * scale, 720 * scale, 800 * scale};
            DockModuleTarget leaf{target, slots.right};
            auto hit = HitDockTarget({1400 * scale, 875 * scale, 1, 1}, work, 80 * scale, &leaf, 1, &slots, &layout);
            auto plan = PlanReadableDockDrop(layout, hit, module, work, 80 * scale, 40 * scale, slots, scale);
            if (!plan.accepted || plan.tooSmall) return 310;
            DockDragSession session(layout);
            if (!session.BeginDrag(module)) return 311;
            session.UpdatePreview(plan.hit);
            session.CommitOnMouseUp();
            const auto landed = CommittedLeafRectForModule(layout, module, work, 80 * scale, 40 * scale, &slots, {});
            if (!SameDockRect(landed, plan.hit.resultRect, 0.01f)) return 312;
        }
    }
    {
        DockLayout layout;
        layout.InsertModule("filter", DockSide::Left, .24f);
        layout.EnsureFloat("info");
        DockRootSlots slots{}; slots.left = {0, 80, 300, 700};
        DockModuleTarget leaf{"filter", slots.left};
        DockRect work{0, 0, 1600, 1000};
        const auto hit = HitDockTarget({295, 400, 1, 1}, work, 80, &leaf, 1, &slots, &layout);
        const auto plan = PlanReadableDockDrop(layout, hit, "info", work, 80, 40, slots);
        if (!plan.accepted || plan.tooSmall || plan.hit.kind != DockHitKind::Module ||
            plan.slots.left.width < 599.0f) return 313;
        if (!layout.Find("filter") || !layout.FindFloat("info")) return 314;
    }
    // Empty side roots use their entire slot, expand only to the incoming
    // content floor, and do not steal the central results' readable space.
    {
        DockLayout layout;
        layout.InsertModule("filter", DockSide::Left, .24f);
        layout.EnsureFloat("similar-sounds");
        DockRect work{0, 0, 1600, 1000};
        DockRootSlots slots{};
        slots.left = {0, 80, 300, 800}; slots.right = {1220, 80, 380, 800};
        DockHit hit; hit.kind = DockHitKind::Main; hit.side = DockSide::Right;
        const auto plan = PlanReadableDockDrop(layout, hit, "similar-sounds", work, 80, 40, slots, 1, 520);
        if (!plan.accepted || plan.hit.rootRegion != DockSide::Right || plan.rootRect.width != 420) return 315;
        DockDragSession session(layout); session.BeginDrag("similar-sounds");
        session.UpdatePreview(plan.hit); session.CommitOnMouseUp();
        if (!SameDockRect(CommittedLeafRectForModule(layout, "similar-sounds", work, 80, 40, &plan.slots, {}),
                          plan.hit.resultRect)) return 316;
        layout.Detach("similar-sounds"); layout.EnsureFloat("similar-sounds");
        work.width = 1100;
        slots.right = {720, 80, 380, 800};
        const auto rejected = PlanReadableDockDrop(layout, hit, "similar-sounds", work, 80, 40, slots, 1, 520);
        if (rejected.accepted || !rejected.tooSmall) return 317;
    }
    // Existing narrow columns expand before a large utility panel is
    // re-docked, so long-press detach and re-attach remain symmetric.
    {
        DockLayout layout;
        layout.InsertModule("filter", DockSide::Left, .24f);
        layout.EnsureFloat("mod-ucs-browser");
        DockRect work{0, 0, 1600, 1000};
        DockRootSlots slots{};
        slots.left = {0, 80, 300, 800};
        DockModuleTarget leaf{"filter", slots.left};
        const auto hit = HitDockTarget({295, 400, 1, 1}, work, 80, &leaf, 1, &slots, &layout);
        const auto plan = PlanReadableDockDrop(layout, hit, "mod-ucs-browser",
            work, 80, 40, slots);
        if (!plan.accepted || plan.tooSmall || plan.hit.kind != DockHitKind::Module ||
            plan.hit.resultRect.width < 480.0f || plan.slots.left.width < 720.0f) return 323;
    }
    // A splitter stops at each child's content floor, in either direction.
    {
        DockLayout layout;
        layout.InsertModule("info", DockSide::Right, .24f);
        layout.InsertAdjacent("info", "similar-sounds", DockSide::Right, .5f);
        if (!DragAdjacentLeafGrip(layout, "info", "similar-sounds", -10000, 1000, 1)) return 318;
        if (std::fabs(layout.RegionAsSplit(DockSide::Right)->ratio - .3f) > .001f) return 319;
        if (!DragAdjacentLeafGrip(layout, "info", "similar-sounds", 10000, 1000, 1)) return 320;
        if (std::fabs(layout.RegionAsSplit(DockSide::Right)->ratio - .58f) > .001f) return 321;
        // At this ratio a narrower outer column would make Similar < 420px.
        if (std::fabs(DockSubtreeMinimum(layout.RegionRoot(DockSide::Right)).width - 1000) > .1f) return 322;
    }
    if (!opened.opened) return 71;
    if (!opened.visible || !opened.floating) return 72;
    if (!opened.requestMove) return 73;
    if (opened.width < 220.0f || opened.height < 180.0f) return 74;
    SimilarSoundsOpenResult ignored = ResolveSimilarSoundsOpenClick(false, true);
    if (ignored.opened) return 75;
    return 0;
}
