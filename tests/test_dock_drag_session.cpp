#include "DockDragSession.h"
#include "ui/dock/DockPanelStateBridge.h"
#include <cstdio>

namespace {
using namespace aura::ui::dock;

DockRect EdgeProbe(const DockRect& rect, DockSide side) {
    float pointerX = rect.x + rect.width * .5f;
    float pointerY = rect.y + rect.height * .5f;
    if (side == DockSide::Left) pointerX = rect.x + 6.0f;
    if (side == DockSide::Right) pointerX = rect.x + rect.width - 6.0f;
    if (side == DockSide::Top) pointerY = rect.y + 6.0f;
    if (side == DockSide::Bottom) pointerY = rect.y + rect.height - 6.0f;
    return {pointerX - .5f, pointerY - .5f, 1, 1};
}

bool CheckDrop(bool passed, const char* module, DockSide side,
               const char* assertion, float actual = 0, float expected = 0) {
    if (!passed) std::fprintf(stderr, "%s side=%d: %s; actual=%.2f expected=%.2f\n",
        module, static_cast<int>(side), assertion, actual, expected);
    return passed;
}

bool CheckFunctionDockPlacement() {
    const char* modules[] = {"filter", "info", "similar-sounds", "mod-ucs-browser",
                             "mod-tag-browser", "mod-workflow", "mod-transfer"};
    const DockRect work{40, 20, 1960, 1260};
    constexpr float centerFloor = 520.0f;
    constexpr float columnTop = 100.0f;
    constexpr float columnHeight = 1040.0f;
    int cases = 0;
    int failures = 0;
    for (const char* incoming : modules) {
        for (const bool panelEdge : {false, true}) {
            for (const auto side : {DockSide::Left, DockSide::Right, DockSide::Top, DockSide::Bottom}) {
                if (!panelEdge && side == DockSide::Top) continue;
                ++cases;
                DockLayout layout;
                const char* target = std::string_view(incoming) == "filter" ? "info" : "filter";
                const char* other = std::string_view(incoming) == "mod-transfer"
                    ? "mod-ucs-browser" : "mod-transfer";
                const auto otherSide = !panelEdge && side == DockSide::Right ? DockSide::Left : DockSide::Right;
                layout.InsertModule(other, otherSide, .5f);
                if (panelEdge) layout.InsertModule(target, DockSide::Left, .5f);
                layout.EnsureFloat(incoming);
                const bool horizontal = side == DockSide::Left || side == DockSide::Right;
                const float leftWidth = panelEdge ? (horizontal ? 340.0f : 600.0f)
                    : otherSide == DockSide::Left ? 500.0f : 460.0f;
                const float rightWidth = otherSide == DockSide::Right ? 500.0f : 460.0f;
                const float leftReserve = layout.RegionRoot(DockSide::Left) ? leftWidth + 12.0f : 0.0f;
                const float rightReserve = layout.RegionRoot(DockSide::Right) ? rightWidth + 12.0f : 0.0f;
                const float centerX = work.x + leftReserve;
                const float centerWidth = work.width - leftReserve - rightReserve;
                DockRootSlots slots{
                    {work.x, columnTop, leftWidth, columnHeight},
                    {work.x + work.width - rightWidth, columnTop, rightWidth, columnHeight},
                    {centerX, columnTop, centerWidth, 220},
                    {centerX, columnTop + columnHeight - 220, centerWidth, 220}};
                const auto frame = BuildDockFrameGeometry(layout, work, 80, 0, slots);
                const DockCenterDropTarget center{{centerX, columnTop + 60, centerWidth, columnHeight - 120},
                    DockSide::Top, true, 220, 220};
                const DockRect targetRect = panelEdge ? slots.left : DockSlotForSide(slots, side);
                const DockHit hit = HitDockTarget(EdgeProbe(targetRect, side), work, kDockSnapDistance,
                    frame.modules, frame.moduleCount, &frame.rootSlots, &layout, &center);
                const auto expectedKind = panelEdge ? DockHitKind::Module : DockHitKind::Main;
                if (!CheckDrop(hit.kind == expectedKind && hit.side == side &&
                        (!panelEdge || hit.targetModuleId == target), incoming, side,
                        panelEdge ? "panel edge must resolve to its leaf" : "workspace wall must resolve to its root",
                        static_cast<float>(hit.kind), static_cast<float>(expectedKind))) {
                    ++failures;
                    continue;
                }
                const auto plan = PlanReadableDockDrop(layout, hit, incoming, work, 80, 0,
                    frame.rootSlots, 1, centerFloor);
                if (!CheckDrop(plan.accepted && !plan.tooSmall, incoming, side,
                        "a readable drop fits without taking the other column", plan.rootRect.width,
                        DockModuleMinimum(incoming).width)) {
                    ++failures;
                    continue;
                }
                const auto preview = PlanDockPreview(plan);
                DockDragSession session(layout);
                session.BeginDrag(incoming);
                session.UpdatePreview(plan.hit);
                session.CommitOnMouseUp(plan);
                DockRect landed{}, otherRect{}, remainingTarget{};
                const bool found = FindCommittedDockModuleRect(layout, incoming, work, 80, 0, plan.slots, landed);
                const bool same = CheckDrop(found && preview.stripCount == 1 &&
                    SameDockRect(preview.strips[0].rect, landed, 1.0f), incoming, side,
                    "one blue preview must equal the committed leaf", landed.width, plan.hit.resultRect.width);
                const bool unchanged = CheckDrop(FindCommittedDockModuleRect(
                    layout, other, work, 80, 0, plan.slots, otherRect) &&
                    SameDockRect(otherRect, DockSlotForSide(slots, otherSide), 1.0f), incoming, side,
                    "an unrelated column must retain its rectangle", otherRect.width, 500.0f);
                const bool targetKeepsWidth = !panelEdge || CheckDrop(FindCommittedDockModuleRect(
                    layout, target, work, 80, 0, plan.slots, remainingTarget) &&
                    std::fabs(remainingTarget.width - leftWidth) <= 1.0f, incoming, side,
                    "the target panel must keep its width on insertion", remainingTarget.width, leftWidth);
                if (!same || !unchanged || !targetKeepsWidth) ++failures;
            }
        }
    }

    DockLayout nested;
    nested.InsertModule("filter", DockSide::Left, .5f);
    nested.InsertAdjacent("filter", "info", DockSide::Right, .5f);
    nested.InsertModule("mod-tag-browser", DockSide::Right, .5f);
    nested.EnsureFloat("mod-workflow");
    const DockRect nestedWork{0, 0, 2060, 1260};
    const DockRootSlots nestedSlots{{0, 100, 600, 1040}, {1710, 100, 350, 1040},
                                    {612, 100, 1086, 220}, {612, 920, 1086, 220}};
    const auto nestedFrame = BuildDockFrameGeometry(nested, nestedWork, 100, 0, nestedSlots);
    const DockHit nestedHit = HitDockTarget(EdgeProbe({300, 100, 300, 1040}, DockSide::Right),
        nestedWork, kDockSnapDistance, nestedFrame.modules, nestedFrame.moduleCount, &nestedFrame.rootSlots, &nested);
    const auto nestedPlan = PlanReadableDockDrop(nested, nestedHit, "mod-workflow", nestedWork, 100, 0,
        nestedFrame.rootSlots, 1, centerFloor);
    DockDragSession nestedSession(nested);
    nestedSession.BeginDrag("mod-workflow");
    nestedSession.UpdatePreview(nestedPlan.hit);
    nestedSession.CommitOnMouseUp(nestedPlan);
    DockRect sibling{}, landed{};
    ++cases;
    if (!CheckDrop(nestedPlan.accepted &&
            FindCommittedDockModuleRect(nested, "filter", nestedWork, 100, 0, nestedPlan.slots, sibling) &&
            FindCommittedDockModuleRect(nested, "mod-workflow", nestedWork, 100, 0, nestedPlan.slots, landed) &&
            SameDockRect(landed, nestedPlan.hit.resultRect, 1.0f) && std::fabs(sibling.width - 300.0f) <= 1.0f,
            "mod-workflow", DockSide::Right, "nested insertion must preserve its untouched sibling width",
            sibling.width, 300.0f)) ++failures;

    DockLayout cramped;
    cramped.InsertModule("filter", DockSide::Left, .5f);
    cramped.InsertModule("info", DockSide::Right, .5f);
    cramped.EnsureFloat("mod-ucs-browser");
    const auto crampedBefore = cramped;
    const DockRect crampedWork{0, 0, 1840, 1260};
    const DockRootSlots crampedSlots{{0, 100, 300, 1040}, {1120, 100, 720, 1040},
                                     {312, 100, 796, 220}, {312, 920, 796, 220}};
    const auto crampedFrame = BuildDockFrameGeometry(cramped, crampedWork, 100, 0, crampedSlots);
    const DockHit crampedHit = HitDockTarget(EdgeProbe(crampedSlots.left, DockSide::Right),
        crampedWork, kDockSnapDistance, crampedFrame.modules, crampedFrame.moduleCount, &crampedFrame.rootSlots, &cramped);
    const auto crampedPlan = PlanReadableDockDrop(cramped, crampedHit, "mod-ucs-browser", crampedWork,
        100, 0, crampedFrame.rootSlots, 1, centerFloor);
    ++cases;
    if (!CheckDrop(!crampedPlan.accepted && crampedPlan.tooSmall && cramped.Equals(crampedBefore),
            "mod-ucs-browser", DockSide::Right, "no drop when only a different panel can pay for it",
            crampedPlan.slots.right.width, 720.0f)) ++failures;
    std::printf("Function panel placement: %d cases, %d failures\n", cases, failures);
    return failures == 0;
}
}

int main() {
    using namespace aura::ui::dock;
    if (!CheckFunctionDockPlacement()) return 1;
    // Results live in the workspace, not in a fifth/narrow side leaf.
    {
        DockLayout layout;
        layout.InsertModule("filter", DockSide::Left, .24f);
        layout.InsertModule("info", DockSide::Right, .28f);
        const DockLayout homeLayout = layout;
        layout.EnsureFloat("search-results");
        const DockLayout floatingLayout = layout;
        const DockRect work{0, 0, 1600, 1000};
        DockCenterDropTarget center{{262, 64, 1010, 644}, DockSide::Left, true};
        center.restoreSearchResults = true;
        DockRootSlots slots{};
        for (const DockRect probe : {DockRect{767, 74, 1, 1}, DockRect{767, 386, 1, 1}}) {
            const DockHit hit = HitDockTarget(probe, work, 80, nullptr, 0, &slots, &layout, &center);
            if (hit.kind != DockHitKind::ResultsHome || !SameDockRect(hit.resultRect, center.rect)) return 330;
            const auto plan = PlanReadableDockDrop(layout, hit, "search-results", work, 64, 200, slots);
            if (!plan.accepted || plan.tooSmall || !SameDockRect(plan.hit.resultRect, center.rect)) return 331;
            if (!layout.Equals(floatingLayout)) return 332;
            if (PlanReadableDockDrop(layout, hit, "info", work, 64, 200, slots).accepted) return 333;
            DockDragSession session(layout);
            if (!session.BeginDrag("search-results")) return 334;
            session.UpdatePreview(plan.hit);
            session.Cancel();
            if (!layout.Equals(floatingLayout)) return 335;
            if (!session.BeginDrag("search-results")) return 336;
            session.UpdatePreview(plan.hit);
            session.CommitOnMouseUp();
            if (!layout.Equals(homeLayout)) return 337;
            layout.EnsureFloat("search-results");
        }
        center.restoreSearchResults = false;
        if (HitDockTarget({767, 386, 1, 1}, work, 80, nullptr, 0, &slots, &layout, &center).kind != DockHitKind::None) return 338;
        center.restoreSearchResults = true;
        if (HitDockTarget({767, 800, 1, 1}, work, 80, nullptr, 0, &slots, &layout, &center).kind == DockHitKind::ResultsHome) return 339;
        bool floating = true;
        RestoreSearchResultsHome(layout, floating);
        if (floating || !layout.Equals(homeLayout)) return 340;
        for (auto side : {DockSide::Left, DockSide::Right, DockSide::Top, DockSide::Bottom}) {
            layout.InsertModule("search-results", side, .5f);
            RestoreSearchResultsHome(layout, floating);
            // Compare the rendered regions, not the legacy last-edited root snapshot.
            if (floating || layout.Find("search-results") || layout.FindFloat("search-results") ||
                layout.HasEmptySplit() || layout.CountInRootRegion(DockSide::Left) != 1 ||
                layout.ShallowestModuleInRootRegion(DockSide::Left) != "filter" ||
                layout.CountInRootRegion(DockSide::Right) != 1 ||
                layout.ShallowestModuleInRootRegion(DockSide::Right) != "info" ||
                layout.RegionRoot(DockSide::Top) || layout.RegionRoot(DockSide::Bottom)) return 341;
        }
        const DockLayout restored = layout;
        RestoreSearchResultsHome(layout, floating);
        if (!layout.Equals(restored)) return 342;
    }
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
            session.CommitOnMouseUp(plan);
            const auto landed = CommittedLeafRectForModule(
                layout, module, work, 80 * scale, 40 * scale, &plan.slots, {});
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
    for (auto band : {DockSide::Top, DockSide::Bottom}) {
        for (auto splitSide : {DockSide::Left, DockSide::Right}) {
            DockLayout layout;
            layout.InsertModule("similar-sounds", DockSide::Left, .24f);
            layout.InsertModule("filter", band, .24f);
            layout.InsertAdjacent("filter", "mod-transfer", DockSide::Right, .34f);
            layout.EnsureFloat("info");
            const DockRect work{0, 0, 2000, 1400};
            DockRootSlots slots{};
            slots.left = {0, 80, 440, 1280};
            slots.top = {440, 80, 1560, 400};
            slots.bottom = {440, 960, 1560, 400};
            const auto anchor = CommittedLeafRectForModule(layout, "filter", work, 80, 40, &slots, {});
            const auto transfer = CommittedLeafRectForModule(layout, "mod-transfer", work, 80, 40, &slots, {});
            const auto similar = CommittedLeafRectForModule(layout, "similar-sounds", work, 80, 40, &slots, {});
            DockHit hit;
            hit.kind = DockHitKind::Module;
            hit.side = splitSide;
            hit.targetModuleId = "filter";
            const auto plan = PlanReadableDockDrop(layout, hit, "info", work, 80, 40, slots);
            if (!CheckDrop(plan.accepted && !plan.tooSmall && plan.hit.rootRegion == band,
                           "info", splitSide, "fitting split inside a top or bottom band")) return 351;
            const auto landed = CommittedLeafRectForModule(
                *plan.candidate, "info", work, 80, 40, &plan.slots, {});
            const auto resizedAnchor = CommittedLeafRectForModule(
                *plan.candidate, "filter", work, 80, 40, &plan.slots, {});
            if (!SameDockRect(landed, plan.hit.resultRect, .01f) ||
                std::fabs(landed.width + resizedAnchor.width - anchor.width) > .01f ||
                std::fabs(std::min(landed.x, resizedAnchor.x) - anchor.x) > .01f ||
                (landed.x < resizedAnchor.x) != (splitSide == DockSide::Left)) return 352;
            if (!SameDockRect(transfer, CommittedLeafRectForModule(
                    *plan.candidate, "mod-transfer", work, 80, 40, &plan.slots, {}), .01f) ||
                !SameDockRect(similar, CommittedLeafRectForModule(
                    *plan.candidate, "similar-sounds", work, 80, 40, &plan.slots, {}), .01f)) return 353;
            auto narrowSlots = slots;
            narrowSlots.top.width = narrowSlots.bottom.width = 520;
            const auto tooNarrow = PlanReadableDockDrop(
                layout, hit, "info", {0, 0, 960, 1400}, 80, 40, narrowSlots);
            if (tooNarrow.accepted || !tooNarrow.tooSmall) return 354;
        }
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
    {
        DockLayout layout;
        layout.InsertModule("info", DockSide::Right, .24f);
        layout.InsertAdjacent("info", "similar-sounds", DockSide::Right, .5f);
        constexpr float extent = 600.0f;
        const auto* split = layout.RegionAsSplit(DockSide::Right);
        if (!DragAdjacentLeafGrip(layout, "info", "similar-sounds", 30.0f, extent, 1)) return 318;
        if (std::fabs(split->ratio * extent - 330.0f) > .001f) return 319;
        if (!DragAdjacentLeafGrip(layout, "info", "similar-sounds", -10000.0f, extent, 1)) return 320;
        if (std::fabs(split->ratio * extent - 150.0f) > .001f) return 321;
        if (!DragAdjacentLeafGrip(layout, "info", "similar-sounds", 10000.0f, extent, 1)) return 322;
        if (std::fabs((1.0f - split->ratio) * extent - 220.0f) > .001f) return 324;
    }
    for (float scale : {1.0f, 1.25f}) {
        for (DockSide region : {DockSide::Left, DockSide::Right}) {
            for (DockSide edge : {DockSide::Top, DockSide::Bottom}) {
                DockLayout layout;
                const bool right = region == DockSide::Right;
                const char* target = right ? "info" : "filter";
                const char* other = right ? "filter" : "info";
                const char* incoming = "mod-ucs-browser";
                layout.InsertModule(target, region, .24f);
                layout.InsertModule(other, right ? DockSide::Left : DockSide::Right, .24f);
                layout.EnsureFloat(incoming);
                const DockRect work{0, 0, 1800 * scale, 1000 * scale};
                DockRootSlots slots{};
                slots.left = {0, 80 * scale, 300 * scale, 800 * scale};
                slots.right = {1500 * scale, 80 * scale, 300 * scale, 800 * scale};
                DockHit hit;
                hit.kind = DockHitKind::Module;
                hit.targetModuleId = target;
                hit.side = edge;
                const auto plan = PlanReadableDockDrop(
                    layout, hit, incoming, work, 80 * scale, 40 * scale, slots, scale, 520 * scale);
                if (!plan.accepted || plan.rootRect.width != 480 * scale) return 334;
                const auto preview = PlanDockPreview(plan);
                DockDragSession session(layout);
                if (!session.BeginDrag(incoming)) return 335;
                session.UpdatePreview(plan.hit);
                session.CommitOnMouseUp(plan);
                const auto existing = CommittedLeafRectForModule(
                    layout, target, work, 80 * scale, 40 * scale, &plan.slots, {});
                const auto landed = CommittedLeafRectForModule(
                    layout, incoming, work, 80 * scale, 40 * scale, &plan.slots, {});
                const auto unaffected = CommittedLeafRectForModule(
                    layout, other, work, 80 * scale, 40 * scale, &plan.slots, {});
                if (std::fabs(existing.width - 480 * scale) > .01f ||
                    std::fabs(landed.width - existing.width) > .01f ||
                    std::fabs(unaffected.width - 300 * scale) > .01f) return 336;
                const float wall = right ? work.width : work.x;
                if (std::fabs((right ? existing.x + existing.width : existing.x) - wall) > .01f ||
                    std::fabs((right ? landed.x + landed.width : landed.x) - wall) > .01f) return 337;
                if (preview.stripCount != 1 || !SameDockRect(preview.strips[0].rect, landed, .01f) ||
                    (edge == DockSide::Top ? landed.y >= existing.y : landed.y <= existing.y)) return 338;
            }
        }
    }
    {
        DockLayout layout;
        layout.InsertModule("info", DockSide::Right, .24f);
        layout.InsertAdjacent("info", "mod-tag-browser", DockSide::Bottom, .5f);
        DockRect work{0, 0, 1920, 1080};
        DockRootSlots slots{}; slots.right = {1600, 80, 320, 900};
        DockHit hit;
        hit.kind = DockHitKind::Module;
        hit.targetModuleId = "info";
        hit.side = DockSide::Right;
        const auto plan = PlanReadableDockDrop(layout, hit, "filter", work, 80, 40, slots, 1, 520);
        if (!plan.accepted || std::fabs(plan.rootRect.width - 600) > .01f) return 339;
        const auto info = CommittedLeafRectForModule(
            *plan.candidate, "info", work, 80, 40, &plan.slots, {});
        const auto tags = CommittedLeafRectForModule(
            *plan.candidate, "mod-tag-browser", work, 80, 40, &plan.slots, {});
        if (std::fabs(info.width - 320) > .01f ||
            !SameDockRect(tags, {1600, 530, 320, 450}, .01f) ||
            !SameDockRect(plan.hit.resultRect, {1640, 80, 280, 450}, .01f)) return 340;
    }
    for (float scale : {1.0f, 1.25f}) {
        for (DockSide side : {DockSide::Left, DockSide::Right}) {
            DockLayout layout;
            const bool right = side == DockSide::Right;
            const char* inward = right ? "info" : "filter";
            layout.InsertModule(inward, side, .24f);
            layout.InsertAdjacent(inward, "mod-transfer", side, .6f);
            float width = 800 * scale;
            const float available = 920 * scale;
            const auto unchangedTransfer = [&]() {
                DockModuleTarget leaves[4]{};
                const DockRect slot{right ? 2500 * scale - width : 0, 0, width, 700 * scale};
                const int count = CollectLeafRects(layout.RegionRoot(side), slot, leaves, 4, 0, 0.0f, right);
                for (int index = 0; index < count; ++index)
                    if (leaves[index].id == "mod-transfer")
                        return std::fabs(leaves[index].rect.width - 480 * scale) < .01f &&
                            std::fabs(leaves[index].rect.x - (right ? 2020 * scale : 0)) < .01f;
                return false;
            };
            if (!unchangedTransfer()) return 341;
            if (!ResizeDockColumnFromCenter(layout, side, width, -80 * scale, 0, available, scale) ||
                std::fabs(width - 720 * scale) > .01f || !unchangedTransfer()) return 342;
            const float lower = (right ? 630 : 640) * scale;
            if (!ResizeDockColumnFromCenter(layout, side, width, -400 * scale, 0, available, scale) ||
                std::fabs(width - lower) > .01f || !unchangedTransfer()) return 343;
            if (ResizeDockColumnFromCenter(layout, side, width, -20 * scale, 0, available, scale) ||
                !ResizeDockColumnFromCenter(layout, side, width, 30 * scale, 0, available, scale) ||
                std::fabs(width - lower - 30 * scale) > .01f || !unchangedTransfer()) return 344;
            if (!ResizeDockColumnFromCenter(layout, side, width, 800 * scale, 0, available, scale) ||
                std::fabs(width - available) > .01f || !unchangedTransfer()) return 345;
        }
    }
    {
        DockLayout layout;
        layout.InsertModule("filter", DockSide::Left, .24f);
        layout.InsertAdjacent("filter", "mod-ucs-browser", DockSide::Bottom, .5f);
        layout.RegionAsSplit(DockSide::Left)->firstWidthShare = .625f;
        float width = 480.0f;
        if (ResizeDockColumnFromCenter(layout, DockSide::Left, width, -40.0f, 0, 800, 1)) return 346;
        if (!ResizeDockColumnFromCenter(layout, DockSide::Left, width, 80.0f, 0, 800, 1)) return 347;
        DockModuleTarget leaves[4]{};
        const int count = CollectLeafRects(layout.RegionRoot(DockSide::Left), {0, 0, width, 600}, leaves, 4, 0);
        if (count != 2 || leaves[0].id != "filter" || leaves[1].id != "mod-ucs-browser" ||
            std::fabs(leaves[0].rect.width - 300) > .01f || std::fabs(leaves[1].rect.width - 560) > .01f)
            return 348;
    }
    {
        DockLayout layout;
        layout.InsertModule("filter", DockSide::Left, .24f);
        layout.InsertAdjacent("filter", "info", DockSide::Bottom, .5f);
        constexpr float extent = 320.0f;
        const auto* split = layout.RegionAsSplit(DockSide::Left);
        if (!DragAdjacentLeafGrip(layout, "filter", "info", 30.0f, extent, 1)) return 325;
        if (std::fabs(split->ratio * extent - 190.0f) > .001f) return 326;
        if (!DragAdjacentLeafGrip(layout, "filter", "info", -30.0f, extent, 1)) return 327;
        if (std::fabs(split->ratio * extent - 160.0f) > .001f) return 328;
    }
    for (const auto incomingSide : {DockSide::Top, DockSide::Bottom}) {
        DockLayout layout;
        if (!layout.InsertModule("filter", DockSide::Left, .24f) ||
            !layout.InsertAdjacent("filter", "info", DockSide::Right, .5f)) return 349;
        const DockRect work{0, 0, 2560, 1000};
        DockRootSlots slots;
        slots.left = {0, 0, 600, 880};
        DockHit drop;
        drop.kind = DockHitKind::Module;
        drop.side = incomingSide;
        drop.targetModuleId = "filter";
        const auto plan = PlanReadableDockDrop(layout, drop, "mod-transfer", work, 0, 0, slots, 1, 520);
        if (!plan.accepted || !plan.candidate) return 359;
        layout = *plan.candidate;
        slots = plan.slots;

        const auto sharedWidthsAre = [&](float stackWidth, float infoWidth, float stackHeight) {
            DockRect filter{}, transfer{}, info{};
            return FindCommittedDockModuleRect(layout, "filter", work, 0, 0, slots, filter) &&
                FindCommittedDockModuleRect(layout, "mod-transfer", work, 0, 0, slots, transfer) &&
                FindCommittedDockModuleRect(layout, "info", work, 0, 0, slots, info) &&
                std::fabs(filter.width - stackWidth) < .01f &&
                std::fabs(transfer.width - stackWidth) < .01f &&
                std::fabs(info.width - infoWidth) < .01f &&
                std::fabs(filter.x - transfer.x) < .01f &&
                std::fabs(filter.x + filter.width - info.x) < .01f &&
                std::fabs(filter.height + transfer.height - stackHeight) < .01f;
        };
        if (!CheckDrop(sharedWidthsAre(480, 300, 880), "mod-transfer", incomingSide,
                       "stacked panes share one full column without changing Info width")) return 360;

        auto* columnSplit = layout.RegionAsSplit(DockSide::Left);
        if (!columnSplit || columnSplit->axis != Axis::Horizontal ||
            columnSplit->first->Kind() != NodeKind::Split) return 350;
        auto* stackSplit = static_cast<SplitNode*>(columnSplit->first.get());
        const float originalWidthRatio = columnSplit->ratio;
        const float originalHeightRatio = stackSplit->ratio;
        const DockModuleTarget legacyColumn{"filter", slots.left};
        const auto frame = BuildDockFrameGeometry(layout, work, 0, 0, slots, &legacyColumn, 1);
        if (frame.gripCount != 2) return 351;
        const AdjacentLeafGrip* heightGrip = nullptr;
        const AdjacentLeafGrip* widthGrip = nullptr;
        for (int index = 0; index < frame.gripCount; ++index) {
            const auto& grip = frame.grips[index];
            const auto* owner = FindSplitContainingLeaves(layout, grip.firstId, grip.secondId);
            if (owner == stackSplit && grip.axis == Axis::Vertical) heightGrip = &grip;
            if (owner == columnSplit && grip.axis == Axis::Horizontal) widthGrip = &grip;
        }
        if (!heightGrip || !widthGrip || !PlanDockColumnOuterGrip(frame, DockSide::Left).visible)
            return 352;
        if (std::fabs(widthGrip->rect.height - slots.left.height) > .01f)
            return 357;
        if (!widthGrip->indicatorAlong) return 358;
        if (std::fabs(*widthGrip->indicatorAlong - (slots.left.y + slots.left.height * .5f)) > .01f)
            return 353;

        if (!DragAdjacentLeafGrip(layout, widthGrip->firstId, widthGrip->secondId, 24, slots.left.width, 1) ||
            !sharedWidthsAre(504, 276, 880) || stackSplit->ratio != originalHeightRatio ||
            !DragAdjacentLeafGrip(layout, widthGrip->firstId, widthGrip->secondId, -24, slots.left.width, 1) ||
            !sharedWidthsAre(480, 300, 880)) return 354;
        const float heightStep = incomingSide == DockSide::Top ? 24.0f : -24.0f;
        if (!DragAdjacentLeafGrip(layout, heightGrip->firstId, heightGrip->secondId, heightStep, 880, 1) ||
            (stackSplit->ratio - originalHeightRatio) * heightStep <= 0 ||
            std::fabs(columnSplit->ratio - originalWidthRatio) > .001f ||
            !DragAdjacentLeafGrip(layout, heightGrip->firstId, heightGrip->secondId, -heightStep, 880, 1) ||
            std::fabs(stackSplit->ratio - originalHeightRatio) > .001f ||
            !sharedWidthsAre(480, 300, 880)) return 355;
    }
    {
        DockLayout layout;
        layout.InsertModule("filter", DockSide::Left, .24f);
        layout.InsertAdjacent("filter", "info", DockSide::Bottom, .5f);
        layout.InsertAdjacent("filter", "mod-transfer", DockSide::Right, .5f);
        layout.InsertAdjacent("info", "similar-sounds", DockSide::Right, .5f);
        DockRootSlots slots;
        slots.left = {0, 0, 984, 880};
        const auto frame = BuildDockFrameGeometry(layout, {0, 0, 2560, 1000}, 0, 0, slots);
        int widthGripCount = 0;
        for (int index = 0; index < frame.gripCount; ++index)
            if (frame.grips[index].axis == Axis::Horizontal) ++widthGripCount;
        if (frame.gripCount != 3 || widthGripCount != 2) return 356;
    }
    for (float scale : {1.0f, 1.25f}) {
        for (auto band : {DockSide::Top, DockSide::Bottom}) {
            for (auto target : {DockSide::Left, DockSide::Right}) {
                for (bool oppositeOccupied : {false, true}) {
                    const bool targetLeft = target == DockSide::Left;
                    const auto opposite = targetLeft ? DockSide::Right : DockSide::Left;
                    const char* targetId = targetLeft ? "filter" : "info";
                    const char* oppositeId = targetLeft ? "info" : "filter";
                    const float splitterWidth = 4.0f * scale;
                    const float targetWidth = 560.0f * scale;
                    const float oppositeWidth = 360.0f * scale;
                    const float leftWidth = targetLeft ? targetWidth : oppositeWidth;
                    const float rightWidth = targetLeft ? oppositeWidth : targetWidth;
                    const DockRect work{0.0f, 0.0f, 1920.0f * scale, 1280.0f * scale};
                    const float toolbar = 80.0f * scale;
                    const float status = 40.0f * scale;
                    const float columnHeight = work.height - toolbar - status;
                    const float bandWidth = work.width - targetWidth - splitterWidth -
                        (oppositeOccupied ? oppositeWidth + splitterWidth : 0.0f);
                    const float bandX = (targetLeft || oppositeOccupied) ? leftWidth + splitterWidth : 0.0f;
                    const float bandHeight = 300.0f * scale;
                    DockLayout layout;
                    if (!layout.InsertModule(targetId, target, .25f) ||
                        (oppositeOccupied && !layout.InsertModule(oppositeId, opposite, .25f)) ||
                        !layout.InsertModule("mod-ucs-browser", band, .25f)) return 359;
                    DockRootSlots slots{};
                    slots.left = {0.0f, toolbar, leftWidth, columnHeight};
                    slots.right = {work.width - rightWidth, toolbar, rightWidth, columnHeight};
                    slots.top = {bandX, toolbar, bandWidth, bandHeight};
                    slots.bottom = {bandX, work.height - status - bandHeight, bandWidth, bandHeight};
                    slots.visibleSideGapPx = splitterWidth;
                    const auto& targetSlot = DockSlotForSide(slots, target);
                    const DockModuleTarget leaf{targetId, targetSlot};
                    const auto hit = HitDockTarget(EdgeProbe(targetSlot, DockSide::Bottom),
                        work, 40.0f * scale, &leaf, 1, &slots, &layout);
                    if (!CheckDrop(hit.kind == DockHitKind::Module && hit.side == DockSide::Bottom,
                                   "mod-transfer", target, "target inner bottom edge")) return 360;
                    const auto plan = PlanReadableDockDrop(
                        layout, hit, "mod-transfer", work, toolbar, status, slots, scale, 520.0f * scale);
                    if (!CheckDrop(plan.accepted && !plan.tooSmall && plan.candidate,
                                   "mod-transfer", target, "same-width drop with existing band",
                                   plan.accepted ? 1.0f : 0.0f, 1.0f)) return 361;
                    const auto& originalBand = band == DockSide::Top ? slots.top : slots.bottom;
                    const auto& plannedBand = band == DockSide::Top ? plan.slots.top : plan.slots.bottom;
                    if (!CheckDrop(SameDockRect(plannedBand, originalBand) &&
                                   std::fabs(plan.hit.resultRect.width - targetWidth) < .01f,
                                   "mod-transfer", target, "drop preserves the band and the column")) return 362;
                    const auto tooTight = PlanReadableDockDrop(
                        layout, hit, "mod-transfer", work, toolbar, status, slots, scale, bandWidth + scale);
                    if (!CheckDrop(!tooTight.accepted && tooTight.tooSmall,
                                   "mod-transfer", target, "minimum center width still enforced")) return 363;
                }
            }
        }
    }
    if (!opened.opened) return 71;
    if (!opened.visible || !opened.floating) return 72;
    if (!opened.requestMove) return 73;
    if (opened.width < 220.0f || opened.height < 180.0f) return 74;
    SimilarSoundsOpenResult ignored = ResolveSimilarSoundsOpenClick(false, true);
    if (ignored.opened) return 75;
    return 0;
}
