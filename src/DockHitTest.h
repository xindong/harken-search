#pragma once

#include "DockLayout.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>

namespace aura::ui::dock {

struct DockRect {
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
};

enum class DockHitKind { None, Main, Parent, Module };
inline constexpr float kDockBandSplitterHeight = 6.0f;

struct DockHit {
    DockHitKind kind = DockHitKind::None;
    DockSide side = DockSide::Left;
    DockSide rootRegion = DockSide::Left;
    std::string targetModuleId;
    DockRect hitRect{};
    DockRect resultRect{};
    // When a center-surface drop recreates an empty root, keep the module's
    // remembered width/height instead of a temporary 24% fallback slot.
    // Zero means that the normal requested slot size remains authoritative.
    float preferredRootExtent = 0.0f;
};

// The center file-results surface is rendered by the workspace shell while
// side panels are represented by DockLayout. When no side root is visible,
// the shell can still provide an edge-only route back into the same Dock
// tree. rect is the actual surface, never a large magnetic center region.
struct DockCenterDropTarget {
    DockRect rect{};
    DockSide side = DockSide::Left;
    bool enabled = false;
    float preferredRootExtent = 0.0f;
    float preferredVerticalExtent = 0.0f;
};

struct DockModuleTarget {
    std::string id;
    DockRect rect;
};

struct DockMinimumSize { float width = 160.0f; float height = 120.0f; };

// Content floors for the existing modules (title/padding included). Scaling is
// taken from the destination viewport, never from the floating source monitor.
inline DockMinimumSize DockModuleMinimum(std::string_view id, float scale = 1.0f) {
    DockMinimumSize size;
    if (id == "filter") size = {280.0f, 180.0f};
    else if (id == "info") size = {300.0f, 180.0f};
    else if (id == "similar-sounds") size = {420.0f, 220.0f};
    else if (id == "search-results") size = {480.0f, 200.0f};
    else if (id == "mod-ucs-browser") size = {480.0f, 260.0f};
    else if (id == "mod-tag-browser") size = {320.0f, 320.0f};
    else if (id == "mod-workflow") size = {560.0f, 360.0f};
    else if (id == "mod-transfer") size = {480.0f, 360.0f};
    scale = std::isfinite(scale) ? std::clamp(scale, 0.5f, 4.0f) : 1.0f;
    return {size.width * scale, size.height * scale};
}

inline DockMinimumSize DockSubtreeMinimum(const DockNode* node, float scale = 1.0f) {
    if (!node) return {0, 0};
    if (node->Kind() == NodeKind::Module)
        return DockModuleMinimum(static_cast<const ModuleNode*>(node)->id, scale);
    if (node->Kind() != NodeKind::Split) return {0, 0};
    const auto* split = static_cast<const SplitNode*>(node);
    const auto a = DockSubtreeMinimum(split->first.get(), scale);
    const auto b = DockSubtreeMinimum(split->second.get(), scale);
    const float ratio = std::clamp(split->ratio, .08f, .92f);
    return split->axis == Axis::Horizontal
        ? DockMinimumSize{std::max(a.width / ratio, b.width / (1 - ratio)), std::max(a.height, b.height)}
        : DockMinimumSize{std::max(a.width, b.width), std::max(a.height / ratio, b.height / (1 - ratio))};
}

struct DockRootSlots {
    DockRect left;
    DockRect right;
    DockRect top;
    DockRect bottom;
};

inline bool RectsOverlap(const DockRect& a, const DockRect& b) {
    return a.width > 0.0f && a.height > 0.0f && b.width > 0.0f && b.height > 0.0f &&
        a.x < b.x + b.width && a.x + a.width > b.x &&
        a.y < b.y + b.height && a.y + a.height > b.y;
}

inline DockRect SlotOrWork(const DockRect& slot, const DockRect& work) {
    return slot.width > 0.0f && slot.height > 0.0f ? slot : work;
}

inline bool ShouldPreviewDockHit(bool titleBarDragging, bool sessionActive) {
    return titleBarDragging && sessionActive;
}

inline bool ShouldDrawDockPreview(bool titleBarDragging, const DockHit& hit) {
    return titleBarDragging && hit.kind != DockHitKind::None;
}

inline DockHit PreferCommittedDockHit(const DockHit& sessionPreview, const DockHit& leftoverHit) {
    if (sessionPreview.kind != DockHitKind::None) return sessionPreview;
    return leftoverHit;
}

template <typename Session>
inline DockHit CommitPreviewThenApply(Session& session, const DockHit& leftoverHit) {
    return session.CommitThenApply(leftoverHit, [](const DockHit&) {});
}

struct SimilarSoundsOpenResult {
    bool opened = false;
    bool visible = false;
    bool floating = false;
    bool requestMove = false;
    float width = 0.0f;
    float height = 0.0f;
};

inline SimilarSoundsOpenResult ResolveSimilarSoundsOpenClick(bool buttonClicked, bool alreadyOpen) {
    SimilarSoundsOpenResult result;
    if (!buttonClicked || alreadyOpen) return result;
    result.opened = true;
    result.visible = true;
    result.floating = true;
    result.requestMove = true;
    result.width = 720.0f;
    result.height = 360.0f;
    return result;
}

constexpr float kDockSplitGripThickness = 8.0f;

struct DockSplitGrip {
    DockRect rect{};
    Axis axis = Axis::Horizontal;
    DockSide rootRegion = DockSide::Left;
    bool visible = false;
};

inline DockRect SplitGripRect(const DockRect& firstLeaf, Axis axis, float thickness = kDockSplitGripThickness) {
    if (axis == Axis::Horizontal) {
        return {firstLeaf.x + firstLeaf.width - thickness * 0.5f, firstLeaf.y, thickness, firstLeaf.height};
    }
    return {firstLeaf.x, firstLeaf.y + firstLeaf.height - thickness * 0.5f, firstLeaf.width, thickness};
}

inline bool HitSplitGrip(const DockRect& pointer, const DockSplitGrip& grip) {
    return grip.visible && RectsOverlap(pointer, grip.rect);
}

inline bool ShouldDrawDockSplitGrip(bool hovered, bool dragging) {
    return hovered || dragging;
}

inline int DockSplitGripFillAlpha(bool hovered, bool dragging) {
    if (dragging) return 180;
    if (hovered) return 140;
    return 0;
}

inline bool DragSplitGrip(DockLayout& layout, DockSide region, float pixelDelta, float regionExtent) {
    SplitNode* split = layout.RegionAsSplit(region);
    if (!split || regionExtent <= 1.0f) return false;
    return layout.ResizeSplit(split, pixelDelta / regionExtent);
}

struct DockedLeafShell {
    DockRect outer{};
    DockRect chrome{};
    float titleHeight = 24.0f;
    float radius = 4.0f;
    float perimeter = 1.0f;
    float inset = 4.0f;
};

inline DockedLeafShell PlanDockedLeafShell(const DockRect& leaf) {
    DockedLeafShell shell;
    shell.outer = leaf;
    shell.chrome = {
        leaf.x + shell.inset,
        leaf.y + shell.inset,
        std::max(0.0f, leaf.width - shell.inset * 2.0f),
        std::max(0.0f, leaf.height - shell.inset * 2.0f)};
    return shell;
}

inline bool LeafShellsDoNotOverlap(const DockedLeafShell& a, const DockedLeafShell& b) {
    return a.chrome.x + a.chrome.width <= b.chrome.x + 0.5f ||
        b.chrome.x + b.chrome.width <= a.chrome.x + 0.5f ||
        a.chrome.y + a.chrome.height <= b.chrome.y + 0.5f ||
        b.chrome.y + b.chrome.height <= a.chrome.y + 0.5f;
}

struct AdjacentLeafGrip {
    DockRect rect{};
    Axis axis = Axis::Horizontal;
    std::string firstId;
    std::string secondId;
    bool visible = false;
};

inline bool SameAdjacentLeafGrip(
    const AdjacentLeafGrip& a,
    const AdjacentLeafGrip& b,
    float epsilon = 0.5f) {
    const bool samePair = (a.firstId == b.firstId && a.secondId == b.secondId) ||
        (a.firstId == b.secondId && a.secondId == b.firstId);
    return samePair && a.axis == b.axis &&
        std::fabs(a.rect.x - b.rect.x) <= epsilon &&
        std::fabs(a.rect.y - b.rect.y) <= epsilon &&
        std::fabs(a.rect.width - b.rect.width) <= epsilon &&
        std::fabs(a.rect.height - b.rect.height) <= epsilon;
}

inline int CollectAdjacentLeafGrips(
    const DockModuleTarget* leaves,
    int leafCount,
    AdjacentLeafGrip* out,
    int maxCount) {
    int count = 0;
    if (!leaves || !out || leafCount < 2 || maxCount <= 0) return 0;
    for (int i = 0; i < leafCount && count < maxCount; ++i) {
        for (int j = i + 1; j < leafCount && count < maxCount; ++j) {
            const DockRect& a = leaves[i].rect;
            const DockRect& b = leaves[j].rect;
            AdjacentLeafGrip grip;
            // Side-by-side leaves: shared vertical seam, grip spans the
            // overlapping y range so partial-height neighbors stay draggable.
            if (std::fabs((std::min)(a.x + a.width, b.x + b.width) -
                          (std::max)(a.x, b.x)) <= kDockSplitGripThickness) {
                const float overlapTop = std::max(a.y, b.y);
                const float overlapBottom = std::min(a.y + a.height, b.y + b.height);
                if (overlapBottom - overlapTop >= kDockSplitGripThickness) {
                    const bool aFirst = a.x <= b.x;
                    const float seamX = aFirst ? a.x + a.width : b.x + b.width;
                    grip.axis = Axis::Horizontal;
                    grip.rect = {seamX - kDockSplitGripThickness * 0.5f, overlapTop,
                                 kDockSplitGripThickness, overlapBottom - overlapTop};
                    grip.firstId = aFirst ? leaves[i].id : leaves[j].id;
                    grip.secondId = aFirst ? leaves[j].id : leaves[i].id;
                    grip.visible = true;
                    out[count++] = grip;
                    continue;
                }
            }
            // Stacked leaves: shared horizontal seam, grip spans the
            // overlapping x range.
            if (std::fabs((std::min)(a.y + a.height, b.y + b.height) -
                          (std::max)(a.y, b.y)) <= kDockSplitGripThickness) {
                const float overlapLeft = std::max(a.x, b.x);
                const float overlapRight = std::min(a.x + a.width, b.x + b.width);
                if (overlapRight - overlapLeft >= kDockSplitGripThickness) {
                    const bool aFirst = a.y <= b.y;
                    const float seamY = aFirst ? a.y + a.height : b.y + b.height;
                    grip.axis = Axis::Vertical;
                    grip.rect = {overlapLeft, seamY - kDockSplitGripThickness * 0.5f,
                                 overlapRight - overlapLeft, kDockSplitGripThickness};
                    grip.firstId = aFirst ? leaves[i].id : leaves[j].id;
                    grip.secondId = aFirst ? leaves[j].id : leaves[i].id;
                    grip.visible = true;
                    out[count++] = grip;
                }
            }
        }
    }
    return count;
}

inline bool DockSubtreeContainsModule(const DockNode* node, std::string_view id) {
    if (!node) return false;
    if (node->Kind() == NodeKind::Module) {
        return static_cast<const ModuleNode*>(node)->id == id;
    }
    const auto* split = static_cast<const SplitNode*>(node);
    return DockSubtreeContainsModule(split->first.get(), id) ||
           DockSubtreeContainsModule(split->second.get(), id);
}

inline SplitNode* FindSplitContainingLeaves(DockNode* node, std::string_view firstId, std::string_view secondId) {
    if (!node || node->Kind() != NodeKind::Split) return nullptr;
    auto* split = static_cast<SplitNode*>(node);
    if ((DockSubtreeContainsModule(split->first.get(), firstId) &&
         DockSubtreeContainsModule(split->second.get(), secondId)) ||
        (DockSubtreeContainsModule(split->first.get(), secondId) &&
         DockSubtreeContainsModule(split->second.get(), firstId))) {
        return split;
    }
    if (auto* found = FindSplitContainingLeaves(split->first.get(), firstId, secondId)) return found;
    return FindSplitContainingLeaves(split->second.get(), firstId, secondId);
}

inline SplitNode* FindSplitContainingLeaves(DockLayout& layout, std::string_view firstId, std::string_view secondId) {
    for (DockSide side : {DockSide::Left, DockSide::Right, DockSide::Top, DockSide::Bottom}) {
        if (auto* found = FindSplitContainingLeaves(layout.RegionRoot(side), firstId, secondId)) return found;
    }
    return FindSplitContainingLeaves(layout.Root(), firstId, secondId);
}

inline float AdjacentSplitExtent(
    DockLayout& layout,
    std::string_view firstId,
    std::string_view secondId,
    Axis axis,
    const DockModuleTarget* leaves,
    int leafCount) {
    SplitNode* split = FindSplitContainingLeaves(layout, firstId, secondId);
    if (!split || !leaves || leafCount <= 0) return 0.0f;
    float minimum = std::numeric_limits<float>::max();
    float maximum = std::numeric_limits<float>::lowest();
    for (int i = 0; i < leafCount; ++i) {
        if (!DockSubtreeContainsModule(split, leaves[i].id)) continue;
        const DockRect& rect = leaves[i].rect;
        const float start = axis == Axis::Horizontal ? rect.x : rect.y;
        const float end = start + (axis == Axis::Horizontal ? rect.width : rect.height);
        minimum = std::min(minimum, start);
        maximum = std::max(maximum, end);
    }
    return maximum > minimum ? maximum - minimum : 0.0f;
}

inline bool DragAdjacentLeafGrip(
    DockLayout& layout,
    std::string_view firstId,
    std::string_view secondId,
    float pixelDelta,
    float extent, float readabilityScale = 0.0f) {
    SplitNode* split = FindSplitContainingLeaves(layout, firstId, secondId);
    if (!split || extent <= 1.0f) return false;
    if (readabilityScale > 0.0f) {
        const auto a = DockSubtreeMinimum(split->first.get(), readabilityScale);
        const auto b = DockSubtreeMinimum(split->second.get(), readabilityScale);
        const float firstMin = split->axis == Axis::Horizontal ? a.width : a.height;
        const float secondMin = split->axis == Axis::Horizontal ? b.width : b.height;
        const float lower = std::max(0.08f, firstMin / extent);
        const float upper = std::min(0.92f, 1.0f - secondMin / extent);
        if (lower > upper) return false;
        const float next = std::clamp(split->ratio + pixelDelta / extent, lower, upper);
        return layout.ResizeSplit(split, next - split->ratio);
    }
    return layout.ResizeSplit(split, pixelDelta / extent);
}

inline DockRect RemainingColumnShell(
    const DockRect& column,
    std::string_view columnId,
    const DockModuleTarget* leaves,
    int leafCount) {
    DockRect remaining = column;
    if (!leaves || leafCount <= 0) return remaining;
    for (int i = 0; i < leafCount; ++i) {
        if (leaves[i].id == columnId) continue;
        const DockRect& leaf = leaves[i].rect;
        const float overlapTop = std::max(remaining.y, leaf.y);
        const float overlapBottom = std::min(remaining.y + remaining.height, leaf.y + leaf.height);
        const float overlapLeft = std::max(remaining.x, leaf.x);
        const float overlapRight = std::min(remaining.x + remaining.width, leaf.x + leaf.width);
        const bool yOverlap = overlapBottom - overlapTop >= kDockSplitGripThickness;
        const bool xOverlap = overlapRight - overlapLeft >= kDockSplitGripThickness;
        const float remainingRight = remaining.x + remaining.width;
        const float remainingBottom = remaining.y + remaining.height;
        const float leafRight = leaf.x + leaf.width;
        const float leafBottom = leaf.y + leaf.height;
        if (yOverlap && xOverlap && leaf.x > remaining.x + 0.5f &&
            leafRight >= remainingRight - 0.5f) {
            remaining.width = std::max(0.0f, leaf.x - remaining.x);
            continue;
        }
        if (yOverlap && xOverlap && leaf.x <= remaining.x + 0.5f &&
            leafRight < remainingRight - 0.5f) {
            remaining.width = std::max(0.0f, remainingRight - leafRight);
            remaining.x = leafRight;
            continue;
        }
        if (xOverlap && leaf.y > remaining.y + 0.5f && leafBottom >= remainingBottom - 0.5f) {
            remaining.height = std::max(0.0f, leaf.y - remaining.y);
            continue;
        }
        if (xOverlap && leaf.y <= remaining.y + 0.5f && leafBottom < remainingBottom - 0.5f) {
            remaining.height = std::max(0.0f, remainingBottom - leafBottom);
            remaining.y = leafBottom;
        }
    }
    return remaining;
}

inline int CollectLegacyColumnLeafGrips(
    const DockModuleTarget* columns,
    int columnCount,
    const DockModuleTarget* leaves,
    int leafCount,
    AdjacentLeafGrip* out,
    int maxCount) {
    if (!columns || !leaves || !out || columnCount <= 0 || leafCount <= 0 || maxCount <= 0) return 0;
    int count = 0;
    for (int c = 0; c < columnCount && count < maxCount; ++c) {
        DockModuleTarget remaining{
            columns[c].id,
            RemainingColumnShell(columns[c].rect, columns[c].id, leaves, leafCount)};
        if (remaining.rect.width <= 1.0f || remaining.rect.height <= 1.0f) continue;
        DockModuleTarget pair[2];
        pair[0] = remaining;
        for (int i = 0; i < leafCount && count < maxCount; ++i) {
            if (leaves[i].id == remaining.id) continue;
            pair[1] = leaves[i];
            AdjacentLeafGrip local[4]{};
            const int n = CollectAdjacentLeafGrips(pair, 2, local, 4);
            for (int g = 0; g < n && count < maxCount; ++g) {
                out[count++] = local[g];
            }
        }
    }
    return count;
}

struct DockedFloatBackResult {
    bool hasFloatAction = false;
    bool floating = false;
    float columnWidth = 0.0f;
};

inline DockedFloatBackResult PlanDockedFloatBack(bool docked, float originalColumnWidth) {
    DockedFloatBackResult result;
    result.hasFloatAction = docked;
    result.columnWidth = originalColumnWidth;
    return result;
}

inline DockedFloatBackResult ApplyDockedFloatBack(float originalColumnWidth, float splitLeafWidth) {
    (void)splitLeafWidth;
    DockedFloatBackResult result;
    result.hasFloatAction = true;
    result.floating = true;
    result.columnWidth = originalColumnWidth;
    return result;
}

struct DockedPairHit {
    DockRect original{};
    DockRect incoming{};
    DockRect originalHit{};
    bool disjoint = false;
    bool coversOriginal = false;
    bool hitMatchesOriginal = false;
};

inline DockedPairHit PlanDockedPairHit(const DockRect& originalSlot, const DockRect& incomingLeaf) {
    DockedPairHit plan;
    DockModuleTarget incoming{"incoming", incomingLeaf};
    plan.original = RemainingColumnShell(originalSlot, "original", &incoming, 1);
    plan.incoming = incomingLeaf;
    plan.originalHit = plan.original;
    plan.disjoint = !RectsOverlap(plan.original, plan.incoming);
    const float unionX = std::min(plan.original.x, plan.incoming.x);
    const float unionY = std::min(plan.original.y, plan.incoming.y);
    const float unionRight = std::max(plan.original.x + plan.original.width, plan.incoming.x + plan.incoming.width);
    const float unionBottom = std::max(plan.original.y + plan.original.height, plan.incoming.y + plan.incoming.height);
    plan.coversOriginal =
        std::fabs(unionX - originalSlot.x) <= 0.5f &&
        std::fabs(unionY - originalSlot.y) <= 0.5f &&
        std::fabs(unionRight - (originalSlot.x + originalSlot.width)) <= 0.5f &&
        std::fabs(unionBottom - (originalSlot.y + originalSlot.height)) <= 0.5f;
    plan.hitMatchesOriginal =
        std::fabs(plan.originalHit.x - plan.original.x) <= 0.5f &&
        std::fabs(plan.originalHit.y - plan.original.y) <= 0.5f &&
        std::fabs(plan.originalHit.width - plan.original.width) <= 0.5f &&
        std::fabs(plan.originalHit.height - plan.original.height) <= 0.5f;
    return plan;
}

struct DockPanelChrome {
    bool titleBar = false;
    bool close = false;
    bool movable = false;
    bool dockable = false;
    bool border = false;
    bool dragHandle = false;
    bool resizeFeedback = false;
    std::string entry;
};

inline DockPanelChrome SharedDockPanelChrome(bool floating) {
    DockPanelChrome chrome;
    chrome.titleBar = true;
    chrome.close = true;
    chrome.movable = floating;
    chrome.dockable = true;
    chrome.border = true;
    chrome.dragHandle = floating;
    chrome.resizeFeedback = true;
    chrome.entry = "shared-dock-window-host";
    return chrome;
}

inline bool UsesSharedDockPanelChrome(std::string_view moduleId, bool usesStandaloneWindow) {
    if (!IsKnownModuleId(moduleId)) return false;
    return !usesStandaloneWindow;
}

inline bool SameSharedPanelChrome(const DockPanelChrome& a, const DockPanelChrome& b) {
    return a.titleBar == b.titleBar &&
        a.close == b.close &&
        a.movable == b.movable &&
        a.dockable == b.dockable &&
        a.border == b.border &&
        a.dragHandle == b.dragHandle &&
        a.resizeFeedback == b.resizeFeedback &&
        a.entry == b.entry;
}

inline DockPanelChrome ResolveModulePanelChrome(std::string_view moduleId, bool floating, std::string_view entry) {
    DockPanelChrome chrome = SharedDockPanelChrome(floating);
    if (!IsKnownModuleId(moduleId)) {
        chrome.entry.clear();
        return chrome;
    }
    chrome.entry = std::string(entry);
    return chrome;
}

struct DockWindowHostRequest {
    std::string moduleId;
    std::string hostId;
    bool floating = true;
    bool usesStandaloneWindow = false;
    bool usesTransientWindow = false;
    float width = 0.0f;
    float height = 0.0f;
};

struct DockWindowHostPlan {
    bool accepted = false;
    std::string entry;
    DockPanelChrome chrome{};
    bool persistId = false;
    bool restoreSize = false;
    bool floatDockSwitch = false;
    bool splitterFeedback = false;
};

inline bool SameSharedDockWindowHost(const DockWindowHostPlan& a, const DockWindowHostPlan& b) {
    return a.accepted == b.accepted &&
        a.entry == b.entry &&
        SameSharedPanelChrome(a.chrome, b.chrome) &&
        a.persistId == b.persistId &&
        a.restoreSize == b.restoreSize &&
        a.floatDockSwitch == b.floatDockSwitch &&
        a.splitterFeedback == b.splitterFeedback;
}

inline DockWindowHostPlan PlanSharedDockWindowHost(const DockWindowHostRequest& request) {
    DockWindowHostPlan plan;
    if (!IsKnownModuleId(request.moduleId) || request.hostId.empty()) return plan;
    if (request.usesStandaloneWindow || request.usesTransientWindow) return plan;
    plan.accepted = true;
    plan.entry = "shared-dock-window-host";
    plan.chrome = SharedDockPanelChrome(request.floating);
    plan.chrome.entry = plan.entry;
    plan.persistId = true;
    plan.restoreSize = request.width > 0.0f && request.height > 0.0f;
    plan.floatDockSwitch = true;
    plan.splitterFeedback = plan.chrome.resizeFeedback;
    return plan;
}

inline DockRect InnerStripRect(const DockRect& box, DockSide side, float strip) {
    switch (side) {
        case DockSide::Left: return {box.x, box.y, strip, box.height};
        case DockSide::Right: return {box.x + box.width - strip, box.y, strip, box.height};
        case DockSide::Top: return {box.x, box.y, box.width, strip};
        case DockSide::Bottom: return {box.x, box.y + box.height - strip, box.width, strip};
    }
    return {};
}

inline DockRect SplitIncomingRect(const DockRect& box, DockSide side, float ratio) {
    const float clamped = std::clamp(ratio, 0.08f, 0.92f);
    switch (side) {
        case DockSide::Left: return {box.x, box.y, box.width * clamped, box.height};
        case DockSide::Right: return {box.x + box.width * (1.0f - clamped), box.y, box.width * clamped, box.height};
        case DockSide::Top: return {box.x, box.y, box.width, box.height * clamped};
        case DockSide::Bottom: return {box.x, box.y + box.height * (1.0f - clamped), box.width, box.height * clamped};
    }
    return {};
}

inline bool SameDockRect(const DockRect& a, const DockRect& b, float eps = 0.5f) {
    return std::fabs(a.x - b.x) <= eps &&
        std::fabs(a.y - b.y) <= eps &&
        std::fabs(a.width - b.width) <= eps &&
        std::fabs(a.height - b.height) <= eps;
}

inline DockRect UnionDockRect(const DockRect& a, const DockRect& b) {
    const float x1 = std::min(a.x, b.x);
    const float y1 = std::min(a.y, b.y);
    const float x2 = std::max(a.x + a.width, b.x + b.width);
    const float y2 = std::max(a.y + a.height, b.y + b.height);
    return {x1, y1, x2 - x1, y2 - y1};
}

inline bool RectContainedIn(const DockRect& inner, const DockRect& outer, float eps = 0.5f) {
    return inner.x + eps >= outer.x &&
        inner.y + eps >= outer.y &&
        inner.x + inner.width <= outer.x + outer.width + eps &&
        inner.y + inner.height <= outer.y + outer.height + eps;
}

inline DockRect RemainingLeafRect(const DockRect& leaf, DockSide side, float ratio) {
    const float clamped = std::clamp(ratio, 0.08f, 0.92f);
    switch (side) {
        case DockSide::Left: return {leaf.x + leaf.width * clamped, leaf.y, leaf.width * (1.0f - clamped), leaf.height};
        case DockSide::Right: return {leaf.x, leaf.y, leaf.width * (1.0f - clamped), leaf.height};
        case DockSide::Top: return {leaf.x, leaf.y + leaf.height * clamped, leaf.width, leaf.height * (1.0f - clamped)};
        case DockSide::Bottom: return {leaf.x, leaf.y, leaf.width, leaf.height * (1.0f - clamped)};
    }
    return {};
}

struct LeafSplitPreview {
    DockRect parent{};
    DockRect incoming{};
    DockRect remaining{};
    DockRect preview{};
    bool accepted = false;
};

inline LeafSplitPreview PlanLeafEdgeSplit(const DockRect& leaf, DockSide side, float bandwidth, float ratio) {
    LeafSplitPreview plan;
    if (leaf.width <= 1.0f || leaf.height <= 1.0f || bandwidth <= 0.0f) return plan;
    plan.parent = leaf;
    plan.incoming = SplitIncomingRect(leaf, side, ratio);
    plan.remaining = RemainingLeafRect(leaf, side, ratio);
    plan.preview = plan.incoming;
    const DockMinimumSize floor;
    plan.accepted = plan.incoming.width >= floor.width && plan.remaining.width >= floor.width &&
        plan.incoming.height >= floor.height && plan.remaining.height >= floor.height;
    return plan;
}

inline DockHit HitDockTarget(
    const DockRect& panel,
    const DockRect& work,
    float snapDistance,
    const DockModuleTarget* modules = nullptr,
    int moduleCount = 0,
    const DockRootSlots* slots = nullptr,
    const DockLayout* layout = nullptr,
    const DockCenterDropTarget* centerDrop = nullptr) {
    DockHit miss;
    if (work.width <= 0.0f || work.height <= 0.0f || snapDistance <= 0.0f) return miss;
    const float strip = std::min(snapDistance, std::min(work.width, work.height));
    for (int i = 0; i < moduleCount; ++i) {
        const DockRect& box = modules[i].rect;
        if (box.width <= 1.0f || box.height <= 1.0f) continue;
        const float edge = std::min(strip, std::min(box.width, box.height));
        const DockRect left{box.x, box.y, edge, box.height};
        const DockRect right{box.x + box.width - edge, box.y, edge, box.height};
        const DockRect top{box.x, box.y, box.width, edge};
        const DockRect bottom{box.x, box.y + box.height - edge, box.width, edge};
        auto hitModule = [&](DockSide side, const DockRect& edge) {
            DockHit hit;
            hit.kind = DockHitKind::Module;
            hit.side = side;
            hit.targetModuleId = modules[i].id;
            hit.hitRect = edge;
            hit.resultRect = SplitIncomingRect(box, side, kDockIncomingShare);
            hit.rootRegion = DockSide::Left;
            if (box.x <= work.x + 1.0f) hit.rootRegion = DockSide::Left;
            else if (box.x + box.width >= work.x + work.width - 1.0f) hit.rootRegion = DockSide::Right;
            else if (box.y <= work.y + 1.0f) hit.rootRegion = DockSide::Top;
            else if (box.y + box.height >= work.y + work.height - 1.0f) hit.rootRegion = DockSide::Bottom;
            return hit;
        };
        const float centerX = panel.x + panel.width * 0.5f;
        const float centerY = panel.y + panel.height * 0.5f;
        const bool touchesTop = centerY < box.y + edge;
        const bool touchesBottom = centerY > box.y + box.height - edge;
        const bool touchesLeft = centerX < box.x + edge;
        const bool touchesRight = centerX > box.x + box.width - edge;
        if (touchesTop && RectsOverlap(panel, top)) return hitModule(DockSide::Top, top);
        if (touchesBottom && RectsOverlap(panel, bottom)) return hitModule(DockSide::Bottom, bottom);
        if (touchesLeft && RectsOverlap(panel, left)) return hitModule(DockSide::Left, left);
        if (touchesRight && RectsOverlap(panel, right)) return hitModule(DockSide::Right, right);
    }
    auto occupiedByLeaf = [&](DockSide side) {
        if (layout != nullptr) {
            return layout->RegionRoot(side) != nullptr &&
                   layout->CountInRootRegion(side) > 0;
        }
        for (int i = 0; i < moduleCount; ++i) {
            const DockRect& box = modules[i].rect;
            if (box.width <= 1.0f || box.height <= 1.0f) continue;
            if (side == DockSide::Left && box.x <= work.x + 1.0f) return true;
            if (side == DockSide::Right && box.x + box.width >= work.x + work.width - 1.0f) return true;
            if (side == DockSide::Top && box.y <= work.y + 1.0f) return true;
            if (side == DockSide::Bottom && box.y + box.height >= work.y + work.height - 1.0f) return true;
        }
        return false;
    };
    const DockRect leftSlot = !slots ? work : (occupiedByLeaf(DockSide::Left) ? DockRect{} : SlotOrWork(slots->left, work));
    const DockRect rightSlot = !slots ? work : (occupiedByLeaf(DockSide::Right) ? DockRect{} : SlotOrWork(slots->right, work));
    const bool topRegionAllowed =
        layout == nullptr ||
        (layout->RegionRoot(DockSide::Top) != nullptr &&
         layout->CountInRootRegion(DockSide::Top) > 0);
    const bool bottomRegionAllowed =
        layout == nullptr ||
        (layout->RegionRoot(DockSide::Bottom) != nullptr &&
         layout->CountInRootRegion(DockSide::Bottom) > 0);
    const DockRect topSlot = !slots ? work
        : (!topRegionAllowed ? DockRect{}
             : (occupiedByLeaf(DockSide::Top) ? DockRect{} : SlotOrWork(slots->top, work)));
    const DockRect bottomSlot = !slots ? work
        : (!bottomRegionAllowed ? DockRect{}
             : (occupiedByLeaf(DockSide::Bottom) ? DockRect{} : SlotOrWork(slots->bottom, work)));
    DockRect topHitSlot = topSlot;
    DockRect bottomHitSlot = bottomSlot;
    if (slots && topHitSlot.width > 1.0f && topHitSlot.height > 1.0f && topHitSlot.y > work.y) {
        // Hit the top edge of the main work area, but keep the resulting leaf
        // below the menu/toolbar where the actual Dock slot is rendered.
        topHitSlot.height += topHitSlot.y - work.y;
        topHitSlot.y = work.y;
    }
    auto clipHorizontalSlot = [&](DockRect& box) {
        if (!slots || box.width <= 1.0f) return;
        if (occupiedByLeaf(DockSide::Left) && slots->left.width > 1.0f) {
            const float leftEdge = slots->left.x + slots->left.width;
            if (box.x < leftEdge) {
                box.width = std::max(1.0f, box.width - (leftEdge - box.x));
                box.x = leftEdge;
            }
        }
        if (occupiedByLeaf(DockSide::Right) && slots->right.width > 1.0f) {
            const float rightEdge = slots->right.x;
            if (box.x + box.width > rightEdge) box.width = std::max(1.0f, rightEdge - box.x);
        }
    };
    clipHorizontalSlot(topHitSlot);
    clipHorizontalSlot(bottomHitSlot);
    const DockRect left = InnerStripRect(leftSlot, DockSide::Left, std::min(strip, leftSlot.width));
    const DockRect right = InnerStripRect(rightSlot, DockSide::Right, std::min(strip, rightSlot.width));
    const DockRect top = InnerStripRect(topSlot, DockSide::Top, std::min(strip, topSlot.height));
    const DockRect topHit = InnerStripRect(topHitSlot, DockSide::Top, std::min(strip, topHitSlot.height));
    const DockRect bottom = InnerStripRect(bottomHitSlot, DockSide::Bottom, std::min(strip, bottomHitSlot.height));
    auto hitMain = [&](DockSide side, const DockRect& strip) {
        DockHit hit;
        hit.kind = DockHitKind::Main;
        hit.side = side;
        hit.rootRegion = side;
        hit.hitRect = strip;
        if (centerDrop && centerDrop->enabled && centerDrop->side == side)
            hit.preferredRootExtent = centerDrop->preferredRootExtent;
        const DockRect slot = side == DockSide::Left ? leftSlot :
            side == DockSide::Right ? rightSlot : side == DockSide::Top ? topSlot : bottomSlot;
        // An empty root receives the entire slot; only an occupied leaf splits.
        hit.resultRect = slots ? slot : SplitIncomingRect(slot, side, kDockIncomingShare);
        return hit;
    };
    if (RectsOverlap(panel, left)) return hitMain(DockSide::Left, left);
    if (RectsOverlap(panel, right)) return hitMain(DockSide::Right, right);
    if (RectsOverlap(panel, topHit)) return hitMain(DockSide::Top, top);
    if (RectsOverlap(panel, bottom)) return hitMain(DockSide::Bottom, bottom);
    if (centerDrop && centerDrop->enabled &&
        centerDrop->rect.width > 1.0f && centerDrop->rect.height > 1.0f) {
        const DockRect& surface = centerDrop->rect;
        const float edge = std::min(strip, std::min(surface.width, surface.height) * 0.5f);
        for (DockSide side : {DockSide::Top, DockSide::Bottom, centerDrop->side}) {
            if (occupiedByLeaf(side)) continue;
            const DockRect edgeBand = InnerStripRect(surface, side, edge);
            if (!RectsOverlap(panel, edgeBand)) continue;
            DockHit hit;
            hit.kind = DockHitKind::Main;
            hit.side = hit.rootRegion = side;
            hit.hitRect = edgeBand;
            hit.preferredRootExtent = side == centerDrop->side
                ? centerDrop->preferredRootExtent : centerDrop->preferredVerticalExtent;
            const DockRect slot = !slots ? work : side == DockSide::Left ? slots->left :
                side == DockSide::Right ? slots->right :
                side == DockSide::Top ? slots->top : slots->bottom;
            // The release simulation replaces this with the actual leaf.
            hit.resultRect = slots ? SlotOrWork(slot, work)
                                   : SplitIncomingRect(work, side, kDockIncomingShare);
            return hit;
        }
    }
    return miss;
}

struct DockPreviewStrip {
    DockRect rect;
    DockSide side = DockSide::Left;
    std::string moduleId;
};

struct DockPreviewPlan {
    int stripCount = 0;
    DockPreviewStrip strips[4]{};
    bool includesMainFrame = false;
};

inline DockPreviewPlan PlanDockPreview(
    const DockHit& hit,
    const DockRect& work,
    float menuBarAndToolbar,
    float statusBar,
    const DockModuleTarget* modules,
    int moduleCount,
    float snapDistance,
    const DockRootSlots* slots = nullptr) {
    DockPreviewPlan plan;
    if (hit.kind == DockHitKind::None || snapDistance <= 0.0f || work.width <= 0.0f || work.height <= 0.0f) {
        return plan;
    }
    const float strip = std::min(snapDistance, std::min(work.width, work.height));
    const DockRect usable{
        work.x,
        work.y + menuBarAndToolbar,
        work.width,
        std::max(1.0f, work.height - menuBarAndToolbar - statusBar)};
    if (hit.resultRect.width > 0.0f && hit.resultRect.height > 0.0f) {
        plan.strips[0] = {hit.resultRect, hit.side, hit.targetModuleId};
        plan.stripCount = 1;
        plan.includesMainFrame = false;
        return plan;
    }
    if (hit.kind == DockHitKind::Main) {
        DockRect slot = usable;
        if (slots) {
            switch (hit.side) {
                case DockSide::Left: slot = SlotOrWork(slots->left, usable); break;
                case DockSide::Right: slot = SlotOrWork(slots->right, usable); break;
                case DockSide::Top: slot = SlotOrWork(slots->top, usable); break;
                case DockSide::Bottom: slot = SlotOrWork(slots->bottom, usable); break;
            }
        }
        plan.strips[0] = {slot, hit.side, {}};
        plan.stripCount = 1;
        plan.includesMainFrame = false;
        return plan;
    }
    if (hit.kind == DockHitKind::Module) {
        for (int i = 0; i < moduleCount; ++i) {
            if (modules[i].id != hit.targetModuleId) continue;
            plan.strips[0] = {SplitIncomingRect(modules[i].rect, hit.side, kDockIncomingShare), hit.side, modules[i].id};
            plan.stripCount = 1;
            plan.includesMainFrame = false;
            return plan;
        }
    }
    return plan;
}

inline int CollectLeafRects(const DockNode* node, const DockRect& box, DockModuleTarget* out, int maxCount, int count) {
    if (!node || count >= maxCount || box.width <= 1.0f || box.height <= 1.0f) return count;
    if (node->Kind() == NodeKind::Module) {
        out[count++] = {static_cast<const ModuleNode*>(node)->id, box};
        return count;
    }
    if (node->Kind() != NodeKind::Split) return count;
    const auto* split = static_cast<const SplitNode*>(node);
    const float ratio = std::clamp(split->ratio, 0.08f, 0.92f);
    if (split->axis == Axis::Horizontal) {
        const float firstW = box.width * ratio;
        count = CollectLeafRects(split->first.get(), {box.x, box.y, firstW, box.height}, out, maxCount, count);
        return CollectLeafRects(split->second.get(), {box.x + firstW, box.y, box.width - firstW, box.height}, out, maxCount, count);
    }
    const float firstH = box.height * ratio;
    count = CollectLeafRects(split->first.get(), {box.x, box.y, box.width, firstH}, out, maxCount, count);
    return CollectLeafRects(split->second.get(), {box.x, box.y + firstH, box.width, box.height - firstH}, out, maxCount, count);
}

struct DockedSplitBand {
    bool split = false;
    DockRect originalLeaf{};
    DockRect incomingLeaf{};
};

inline DockedSplitBand ResolveDockedSplitBand(
    const DockLayout& layout,
    DockSide region,
    const DockRect& slot,
    std::string_view originalId,
    std::string_view incomingId) {
    DockedSplitBand band;
    const DockNode* root = layout.RegionRoot(region);
    if (!root || root->Kind() != NodeKind::Split || slot.width <= 1.0f || slot.height <= 1.0f) return band;
    DockModuleTarget leaves[8]{};
    const int count = CollectLeafRects(root, slot, leaves, 8, 0);
    if (count < 2) return band;
    for (int i = 0; i < count; ++i) {
        if (leaves[i].id == originalId) band.originalLeaf = leaves[i].rect;
        else if (leaves[i].id == incomingId) band.incomingLeaf = leaves[i].rect;
    }
    band.split = band.originalLeaf.width > 1.0f && band.originalLeaf.height > 1.0f &&
        band.incomingLeaf.width > 1.0f && band.incomingLeaf.height > 1.0f;
    return band;
}

// Width the filter shell draws at inside the left column. While the left
// region is split, the shell occupies only the filter leaf; the moment the
// split is gone (new window closed), the shell returns to the whole column
// even if a stale narrow leaf rect is still remembered.
inline float DockFilterShellWidth(
    bool splitInLeftRegion,
    float columnWidth,
    float rememberedLeafWidth,
    float minWidth,
    float maxWidth) {
    if (!splitInLeftRegion) return std::clamp(columnWidth, minWidth, maxWidth);
    // A legacy shell may host only one Dock leaf. It must never expand past
    // that leaf or it will cover the adjacent panel and its grip.
    const float leafWidth = std::max(1.0f, rememberedLeafWidth);
    return std::min(std::clamp(leafWidth, minWidth, maxWidth), leafWidth);
}

struct DockLeafShellParams {
    float titleHeight = 0.0f;
    float radius = 0.0f;
    float perimeter = 0.0f;
    float inset = 0.0f;
    std::string entry;
};

inline DockLeafShellParams DockLeafShellParamsFor(bool) {
    // Floating and docked windows share the filter chrome: title 24, radius 4,
    // perimeter 1, inset 4, single shared host entry.
    DockLeafShellParams params;
    params.titleHeight = 24.0f;
    params.radius = 4.0f;
    params.perimeter = 1.0f;
    params.inset = 4.0f;
    params.entry = "shared-dock-window-host";
    return params;
}

inline int LayoutRootRegionLeaves(
    const DockLayout& layout,
    const DockRect& work,
    float menuBarAndToolbar,
    float statusBar,
    DockModuleTarget* out,
    int maxCount,
    const DockRootSlots* slots = nullptr) {
    int count = 0;
    DockRect usable{
        work.x,
        work.y + menuBarAndToolbar,
        work.width,
        std::max(1.0f, work.height - menuBarAndToolbar - statusBar)};
    auto take = [&](DockSide side, DockRect box) {
        const DockNode* node = layout.RegionRoot(side);
        if (!node) return;
        count = CollectLeafRects(node, box, out, maxCount, count);
    };
    if (slots) {
        const DockRect leftBox = SlotOrWork(slots->left, usable);
        const DockRect rightBox = SlotOrWork(slots->right, usable);
        DockRect topBox = SlotOrWork(slots->top, usable);
        DockRect bottomBox = SlotOrWork(slots->bottom, usable);
        if (layout.RegionRoot(DockSide::Left) && leftBox.width > 0.0f) {
            const float leftEdge = leftBox.x + leftBox.width;
            if (RectsOverlap(leftBox, topBox) && topBox.x < leftEdge) {
                const float shrink = leftEdge - topBox.x;
                topBox.x = leftEdge;
                topBox.width = std::max(1.0f, topBox.width - shrink);
            }
            if (bottomBox.x < leftEdge) {
                const float shrink = leftEdge - bottomBox.x;
                bottomBox.x = leftEdge;
                bottomBox.width = std::max(1.0f, bottomBox.width - shrink);
            }
        }
        if (layout.RegionRoot(DockSide::Right) && rightBox.width > 0.0f) {
            const float rightEdge = rightBox.x;
            if (RectsOverlap(rightBox, topBox) && topBox.x + topBox.width > rightEdge) {
                topBox.width = std::max(1.0f, rightEdge - topBox.x);
            }
            if (bottomBox.x + bottomBox.width > rightEdge) {
                bottomBox.width = std::max(1.0f, rightEdge - bottomBox.x);
            }
        }
        take(DockSide::Left, leftBox);
        take(DockSide::Right, rightBox);
        take(DockSide::Top, topBox);
        take(DockSide::Bottom, bottomBox);
        return count;
    }
    const float leftShare = layout.RegionRoot(DockSide::Left) ? 0.24f : 0.0f;
    const float rightShare = layout.RegionRoot(DockSide::Right) ? 0.24f : 0.0f;
    const float topShare = layout.RegionRoot(DockSide::Top) ? 0.24f : 0.0f;
    const float bottomShare = layout.RegionRoot(DockSide::Bottom) ? 0.24f : 0.0f;
    const float leftW = usable.width * leftShare;
    const float rightW = usable.width * rightShare;
    const float topH = usable.height * topShare;
    const float bottomH = usable.height * bottomShare;
    take(DockSide::Left, {usable.x, usable.y, leftW, usable.height});
    take(DockSide::Right, {usable.x + usable.width - rightW, usable.y, rightW, usable.height});
    take(DockSide::Top, {usable.x + leftW, usable.y, usable.width - leftW - rightW, topH});
    take(DockSide::Bottom, {usable.x + leftW, usable.y + usable.height - bottomH, usable.width - leftW - rightW, bottomH});
    return count;
}

inline DockRect CommittedLeafRectForModule(
    const DockLayout& layout,
    std::string_view moduleId,
    const DockRect& work,
    float menuBarAndToolbar,
    float statusBar,
    const DockRootSlots* slots,
    const DockRect& fallback) {
    DockModuleTarget leaves[16]{};
    const int count = LayoutRootRegionLeaves(layout, work, menuBarAndToolbar,
                                             statusBar, leaves, 16, slots);
    for (int i = 0; i < count; ++i) {
        if (leaves[i].id == moduleId) {
            return leaves[i].rect;
        }
    }
    return fallback;
}

constexpr int kDockFrameModuleCapacity = 16;
constexpr int kDockFrameGripCapacity = 64;
// Keep the edge band narrow enough that a floating panel does not re-enter a
// dock target merely because it is still near the old leaf.
constexpr float kDockSnapDistance = 48.0f;
// Match the title gesture's intentional move threshold. A short, real move
// should stop treating the former dock leaf as the panel's origin.
constexpr float kDockOriginEscapeDistance = 6.0f;

struct DockFrameGeometry {
    DockRootSlots rootSlots{};
    DockModuleTarget modules[kDockFrameModuleCapacity]{};
    int moduleCount = 0;
    AdjacentLeafGrip grips[kDockFrameGripCapacity]{};
    int gripCount = 0;
};

inline AdjacentLeafGrip PlanRightDockOuterGrip(
    const DockFrameGeometry& frame, std::string_view moduleId) {
    AdjacentLeafGrip grip;
    if (frame.rootSlots.right.width <= 1.0f || frame.rootSlots.right.height <= 1.0f) return grip;
    bool found = false;
    for (int i = 0; i < frame.moduleCount; ++i) {
        if (frame.modules[i].id == moduleId) { found = true; break; }
    }
    if (!found) return grip;
    grip.axis = Axis::Horizontal;
    grip.firstId = "search-results";
    grip.secondId = std::string(moduleId);
    grip.rect = {frame.rootSlots.right.x - kDockSplitGripThickness * 0.5f,
                 frame.rootSlots.right.y, kDockSplitGripThickness, frame.rootSlots.right.height};
    grip.visible = true;
    return grip;
}

inline bool DragRightDockOuterGrip(
    float& dockedWidth, float mouseDeltaX, float minWidth, float maxWidth) {
    if (mouseDeltaX == 0.0f) return false;
    const float next = std::clamp(dockedWidth - mouseDeltaX, minWidth, maxWidth);
    if (next == dockedWidth) return false;
    dockedWidth = next;
    return true;
}

inline DockRootSlots ResolveDockRootSlots(
    const DockLayout& layout,
    const DockRect& work,
    float menuBarAndToolbar,
    float statusBar,
    const DockRootSlots& requested) {
    DockRect usable{
        work.x,
        work.y + menuBarAndToolbar,
        work.width,
        std::max(1.0f, work.height - menuBarAndToolbar - statusBar)};
    DockRootSlots resolved{
        SlotOrWork(requested.left, usable),
        SlotOrWork(requested.right, usable),
        SlotOrWork(requested.top, usable),
        SlotOrWork(requested.bottom, usable)};

    if (layout.RegionRoot(DockSide::Left) && resolved.left.width > 0.0f) {
        const float leftEdge = resolved.left.x + resolved.left.width;
        if (RectsOverlap(resolved.left, resolved.top) && resolved.top.x < leftEdge) {
            const float shrink = leftEdge - resolved.top.x;
            resolved.top.x = leftEdge;
            resolved.top.width = std::max(1.0f, resolved.top.width - shrink);
        }
        if (resolved.bottom.x < leftEdge) {
            const float shrink = leftEdge - resolved.bottom.x;
            resolved.bottom.x = leftEdge;
            resolved.bottom.width = std::max(1.0f, resolved.bottom.width - shrink);
        }
    }
    if (layout.RegionRoot(DockSide::Right) && resolved.right.width > 0.0f) {
        const float rightEdge = resolved.right.x;
        if (RectsOverlap(resolved.right, resolved.top) &&
            resolved.top.x + resolved.top.width > rightEdge) {
            resolved.top.width = std::max(1.0f, rightEdge - resolved.top.x);
        }
        if (resolved.bottom.x + resolved.bottom.width > rightEdge) {
            resolved.bottom.width = std::max(1.0f, rightEdge - resolved.bottom.x);
        }
    }
    return resolved;
}

struct DockDropPlan {
    bool accepted = false;
    bool tooSmall = false;
    DockHit hit{};
    DockRootSlots slots{};
    DockRect rootRect{};
};

inline DockRect& DockSlotForSide(DockRootSlots& slots, DockSide side) {
    switch (side) {
    case DockSide::Left: return slots.left;
    case DockSide::Right: return slots.right;
    case DockSide::Top: return slots.top;
    case DockSide::Bottom: return slots.bottom;
    }
    return slots.left;
}

// Simulate the same tree operation used at release. Preview and validation
// consume its actual leaf rectangles, including nested splits and root clipping.
// This is read-only: a rejected drop cannot mutate the live layout.
inline DockDropPlan PlanReadableDockDrop(
    const DockLayout& layout, const DockHit& hit, std::string_view incoming,
    const DockRect& work, float menuBarAndToolbar, float statusBar,
    const DockRootSlots& requested, float scale = 1.0f,
    float centerMinimumWidth = 0.0f) {
    DockDropPlan plan;
    if (hit.kind == DockHitKind::None || incoming.empty() ||
        hit.targetModuleId == incoming) return plan;
    DockLayout candidate = layout;
    if (candidate.Find(incoming)) candidate.Detach(incoming);
    candidate.RemoveFloat(incoming);
    const bool inserted = hit.kind == DockHitKind::Module
        ? candidate.InsertAdjacent(hit.targetModuleId, incoming, hit.side, kDockIncomingShare)
        : candidate.InsertModule(incoming, hit.side, kDockIncomingShare);
    if (!inserted) return plan;
    plan.slots = requested;
    DockSide root = hit.rootRegion;
    for (auto side : {DockSide::Left, DockSide::Right, DockSide::Top, DockSide::Bottom})
        if (DockSubtreeContainsModule(candidate.RegionRoot(side), incoming)) root = side;
    // A floating module can be dropped back onto an existing narrow column.
    // Grow that root before measuring the candidate tree so the incoming
    // module and the leaves already in the column both keep their readable
    // floors. Without this, a 480px utility panel could detach successfully
    // but never produce a legal re-dock preview beside a 250px filter column.
    const DockMinimumSize rootMinimum = DockSubtreeMinimum(candidate.RegionRoot(root), scale);
    if (!layout.RegionRoot(root) && hit.preferredRootExtent > 1.0f) {
        auto& slot = DockSlotForSide(plan.slots, root);
        const bool horizontal = root == DockSide::Left || root == DockSide::Right;
        const float available = horizontal ? work.width : work.height;
        const float minimum = horizontal ? rootMinimum.width : rootMinimum.height;
        const float extent = std::clamp(
            std::max(minimum, hit.preferredRootExtent), minimum,
            std::max(minimum, available));
        if (horizontal) {
            slot.width = extent;
            if (root == DockSide::Right) slot.x = work.x + work.width - extent;
        } else {
            slot.height = extent;
        }
    }
    // A leaf can be split in any direction. Grow the containing root on both
    // axes to the candidate tree's readable minimum. A Top/Bottom split may
    // still need more width when its target leaf already sits beside another
    // leaf; growing only the requested axis would reject that recursive drop.
    auto& targetSlot = DockSlotForSide(plan.slots, root);
    if ((root == DockSide::Top || root == DockSide::Bottom) &&
        targetSlot.width + .5f < rootMinimum.width) {
        // The renderer gives bands exactly the central column's width.
        // It cannot honor a preview that silently expands into a sidebar.
        plan.tooSmall = true;
        return plan;
    }
    targetSlot.width = std::max(targetSlot.width, rootMinimum.width);
    targetSlot.height = std::max(targetSlot.height, rootMinimum.height);
    if (root == DockSide::Right)
        targetSlot.x = work.x + work.width - targetSlot.width;
    if (root == DockSide::Top || root == DockSide::Bottom) {
        // Horizontal bands belong to the results column, above the waveform.
        // Never anchor a bottom drop to the bottom of the whole application.
        const float contentTop = requested.top.height > 1.0f
            ? requested.top.y : work.y + menuBarAndToolbar;
        const float contentBottom = requested.bottom.height > 1.0f
            ? requested.bottom.y + requested.bottom.height : work.y + work.height - statusBar;
        const DockSide opposite = root == DockSide::Top ? DockSide::Bottom : DockSide::Top;
        const float oppositeHeight = candidate.RegionRoot(opposite)
            ? DockSlotForSide(plan.slots, opposite).height : 0.0f;
        const float centerHeight = candidate.Find("search-results") ? 0.0f : 200.0f * scale;
        const float splitterSpace = kDockBandSplitterHeight * (oppositeHeight > 0.0f ? 2.0f : 1.0f);
        const float maxHeight = contentBottom - contentTop - oppositeHeight - centerHeight - splitterSpace;
        if (maxHeight + .5f < rootMinimum.height) {
            plan.tooSmall = true;
            return plan;
        }
        targetSlot.height = std::min(targetSlot.height, maxHeight);
        targetSlot.y = root == DockSide::Top ? contentTop : contentBottom - targetSlot.height;
    }
    // A real workspace can have both side roots occupied while the central
    // file list is not part of this Dock tree. When the new split needs more
    // width, first reclaim unused width from the opposite root down to that
    // root's own readable floor. This keeps all leaves visible instead of
    // rejecting a valid four-side drop just because the remembered opposite
    // column was wide. If both roots are already at their floors, reject only
    // when the physical workspace genuinely cannot keep the center readable.
    if ((root == DockSide::Left || root == DockSide::Right) &&
        !candidate.Find("search-results") && centerMinimumWidth > 0.0f) {
        const DockSide opposite = root == DockSide::Left ? DockSide::Right : DockSide::Left;
        auto& oppositeSlot = DockSlotForSide(plan.slots, opposite);
        const bool oppositeOccupied = candidate.RegionRoot(opposite) != nullptr &&
            oppositeSlot.width > 1.0f;
        const float splitter = 12.0f;
        const float targetReserve = targetSlot.width + splitter;
        const float oppositeReserve = oppositeOccupied ? oppositeSlot.width + splitter : 0.0f;
        const float allowed = std::max(0.0f, work.width - centerMinimumWidth);
        const float deficit = targetReserve + oppositeReserve > allowed
            ? targetReserve + oppositeReserve - allowed : 0.0f;
        if (deficit > 0.0f && oppositeOccupied) {
            const float oppositeMinimum = DockSubtreeMinimum(
                candidate.RegionRoot(opposite), scale).width;
            const float shrinkable = std::max(0.0f, oppositeSlot.width - oppositeMinimum);
            const float shrink = std::min(deficit, shrinkable);
            oppositeSlot.width -= shrink;
            if (opposite == DockSide::Right)
                oppositeSlot.x = work.x + work.width - oppositeSlot.width;
        }
        const float finalTargetReserve = targetSlot.width + splitter;
        const float finalOppositeReserve = oppositeOccupied
            ? oppositeSlot.width + splitter : 0.0f;
        if (finalTargetReserve + finalOppositeReserve > allowed + 0.5f) {
            plan.tooSmall = true;
            return plan;
        }
    }
    plan.slots = ResolveDockRootSlots(candidate, work, menuBarAndToolbar, statusBar, plan.slots);
    plan.rootRect = DockSlotForSide(plan.slots, root);
    DockModuleTarget before[64]{}, after[64]{};
    const int beforeCount = LayoutRootRegionLeaves(layout, work, menuBarAndToolbar, statusBar, before, 64, &requested);
    const int afterCount = LayoutRootRegionLeaves(candidate, work, menuBarAndToolbar, statusBar, after, 64, &plan.slots);
    DockRect result{};
    for (int i = 0; i < afterCount; ++i) {
        const auto& leaf = after[i];
        DockRect old{};
        for (int j = 0; j < beforeCount; ++j)
            if (before[j].id == leaf.id) { old = before[j].rect; break; }
        const auto floor = DockModuleMinimum(leaf.id, scale);
        const bool isIncoming = leaf.id == incoming;
        // Existing narrow panels elsewhere are allowed to remain unchanged.
        if (((isIncoming || leaf.rect.width < old.width - .5f) && leaf.rect.width + .5f < floor.width) ||
            ((isIncoming || leaf.rect.height < old.height - .5f) && leaf.rect.height + .5f < floor.height)) {
            plan.tooSmall = true;
            return plan;
        }
        if (isIncoming) result = leaf.rect;
    }
    if ((root == DockSide::Left || root == DockSide::Right) &&
        !candidate.Find("search-results") && centerMinimumWidth > 0) {
        const float left = candidate.RegionRoot(DockSide::Left) ? plan.slots.left.width + 12.0f : 0;
        const float right = candidate.RegionRoot(DockSide::Right) ? plan.slots.right.width + 12.0f : 0;
        if (work.width - left - right < centerMinimumWidth) {
            plan.tooSmall = true;
            return plan;
        }
    }
    if (result.width <= 1 || result.height <= 1) return plan;
    plan.accepted = true;
    plan.hit = hit;
    plan.hit.rootRegion = root;
    plan.hit.resultRect = result;
    return plan;
}

inline void AppendUniqueDockFrameGrip(
    DockFrameGeometry& frame, const AdjacentLeafGrip& candidate) {
    for (int i = 0; i < frame.gripCount; ++i) {
        if (SameAdjacentLeafGrip(frame.grips[i], candidate)) return;
    }
    if (frame.gripCount < kDockFrameGripCapacity) {
        frame.grips[frame.gripCount++] = candidate;
    }
}

inline DockFrameGeometry BuildDockFrameGeometry(
    const DockLayout& layout,
    const DockRect& work,
    float menuBarAndToolbar,
    float statusBar,
    const DockRootSlots& requestedSlots,
    const DockModuleTarget* legacyColumns = nullptr,
    int legacyColumnCount = 0) {
    DockFrameGeometry frame;
    frame.rootSlots = ResolveDockRootSlots(
        layout, work, menuBarAndToolbar, statusBar, requestedSlots);
    frame.moduleCount = LayoutRootRegionLeaves(
        layout, work, menuBarAndToolbar, statusBar, frame.modules,
        kDockFrameModuleCapacity, &frame.rootSlots);

    const DockRect usable{
        work.x,
        work.y + menuBarAndToolbar,
        work.width,
        std::max(1.0f, work.height - menuBarAndToolbar - statusBar)};
    const DockSide sides[] = {DockSide::Left, DockSide::Right, DockSide::Top, DockSide::Bottom};
    const DockRect slots[] = {
        SlotOrWork(frame.rootSlots.left, usable),
        SlotOrWork(frame.rootSlots.right, usable),
        SlotOrWork(frame.rootSlots.top, usable),
        SlotOrWork(frame.rootSlots.bottom, usable)};
    for (int sideIndex = 0; sideIndex < 4; ++sideIndex) {
        const DockNode* root = layout.RegionRoot(sides[sideIndex]);
        if (!root) continue;
        DockModuleTarget leaves[kDockFrameModuleCapacity]{};
        const int leafCount = CollectLeafRects(
            root, slots[sideIndex], leaves, kDockFrameModuleCapacity, 0);
        AdjacentLeafGrip local[kDockFrameGripCapacity]{};
        const int localCount = CollectAdjacentLeafGrips(
            leaves, leafCount, local, kDockFrameGripCapacity);
        for (int i = 0; i < localCount; ++i) {
            AppendUniqueDockFrameGrip(frame, local[i]);
        }
    }

    if (legacyColumns && legacyColumnCount > 0) {
        AdjacentLeafGrip local[kDockFrameGripCapacity]{};
        const int localCount = CollectLegacyColumnLeafGrips(
            legacyColumns, legacyColumnCount, frame.modules, frame.moduleCount,
            local, kDockFrameGripCapacity);
        for (int i = 0; i < localCount; ++i) {
            AppendUniqueDockFrameGrip(frame, local[i]);
        }
    }
    return frame;
}

struct DockHint {
    DockRect probe{};
    DockHit hit{};
    DockRect visibleRect{};
};

inline int CollectLegalDockHints(
    const DockLayout& layout, const DockRect& work, float snapDistance,
    const DockFrameGeometry& frame, DockHint* out, int maxCount) {
    if (!out || maxCount <= 0) return 0;
    int count = 0;
    auto probeNearOuterEdge = [](const DockRect& strip, DockSide side) {
        float x = strip.x + strip.width * 0.5f;
        float y = strip.y + strip.height * 0.5f;
        if (side == DockSide::Left) x = strip.x + std::min(6.0f, strip.width * 0.5f);
        if (side == DockSide::Right) x = strip.x + strip.width - std::min(6.0f, strip.width * 0.5f);
        if (side == DockSide::Top) y = strip.y + std::min(6.0f, strip.height * 0.5f);
        if (side == DockSide::Bottom) y = strip.y + strip.height - std::min(6.0f, strip.height * 0.5f);
        return DockRect{x - 2.0f, y - 2.0f, 4.0f, 4.0f};
    };
    auto legalVisibleStrip = [](DockRect strip, const DockRect& box, DockSide side, float edge) {
        // HitDockTarget checks Top, Bottom, Left, Right in that order. When
        // edge bands overlap in a short leaf, draw only the part where this
        // side actually wins the hit test.
        if (side == DockSide::Bottom || side == DockSide::Left || side == DockSide::Right) {
            const float topEnd = box.y + edge;
            const float bottomStart = box.y + box.height - edge;
            if (side == DockSide::Bottom) {
                const float start = std::max(strip.y, topEnd);
                strip.height = std::max(0.0f, strip.y + strip.height - start);
                strip.y = start;
            } else {
                const float start = std::max(strip.y, topEnd);
                const float end = std::min(strip.y + strip.height, bottomStart);
                strip.y = start;
                strip.height = std::max(0.0f, end - start);
            }
        }
        if (side == DockSide::Right) {
            const float start = std::max(strip.x, box.x + edge);
            strip.width = std::max(0.0f, strip.x + strip.width - start);
            strip.x = start;
        }
        return strip;
    };
    for (int i = 0; i < frame.moduleCount && count < maxCount; ++i) {
        const DockRect& box = frame.modules[i].rect;
        if (box.width <= 1.0f || box.height <= 1.0f) continue;
        const float edge = std::min(snapDistance, std::min(box.width, box.height));
        for (DockSide side : {DockSide::Left, DockSide::Right, DockSide::Top, DockSide::Bottom}) {
            const DockRect strip = InnerStripRect(box, side, edge);
            const DockRect probe = probeNearOuterEdge(strip, side);
            const DockHit hit = HitDockTarget(probe, work, snapDistance,
                frame.modules, frame.moduleCount, &frame.rootSlots, &layout);
            if (hit.kind != DockHitKind::Module ||
                hit.targetModuleId != frame.modules[i].id || hit.side != side) continue;
            const DockRect visible = legalVisibleStrip(hit.hitRect, box, side, edge);
            if (visible.width <= 0.0f || visible.height <= 0.0f) continue;
            out[count++] = {probe, hit, visible};
            if (count >= maxCount) break;
        }
    }
    for (DockSide side : {DockSide::Left, DockSide::Right}) {
        if (count >= maxCount || layout.RegionRoot(side)) continue;
        const DockRect slot = side == DockSide::Left ? frame.rootSlots.left : frame.rootSlots.right;
        if (slot.width <= 1.0f || slot.height <= 1.0f) continue;
        const DockRect strip = InnerStripRect(slot, side,
            std::min(snapDistance, std::min(slot.width, slot.height)));
        const DockRect probe = probeNearOuterEdge(strip, side);
        const DockHit hit = HitDockTarget(probe, work, snapDistance,
            frame.modules, frame.moduleCount, &frame.rootSlots, &layout);
        if (hit.kind == DockHitKind::Main && hit.side == side) {
            out[count++] = {probe, hit, hit.hitRect};
        }
    }
    return count;
}

inline bool ShouldEvaluateFloatingDockZone(
    bool titleBarDragging,
    bool panelReleasePending,
    bool ownsActiveDockDrag,
    bool movedFromDockOrigin = true) {
    return movedFromDockOrigin &&
        (titleBarDragging || panelReleasePending || ownsActiveDockDrag);
}

// A freshly floated panel still overlaps its former leaf. Treating that
// overlap as a drop target makes a title hold snap straight back on release.
// Require a small, real move away from the origin before evaluating drops.
inline bool HasMovedFromDockOrigin(
    const DockRect& current, const DockRect& origin,
    float threshold = kDockOriginEscapeDistance) {
    if (threshold < 0.0f) threshold = 0.0f;
    return std::fabs(current.x - origin.x) > threshold ||
        std::fabs(current.y - origin.y) > threshold;
}

// Keep the central results surface as a deliberate landing pad instead of
// making every point inside the main window an automatic dock target. The
// outer edge strips remain available for explicit left/right/top/bottom drops.
inline DockRect InsetDockCenterDropRect(
    const DockRect& surface, float insetX, float insetY) {
    if (surface.width <= 1.0f || surface.height <= 1.0f) return {};
    const float safeX = std::isfinite(insetX) ? std::max(0.0f, insetX) : 0.0f;
    const float safeY = std::isfinite(insetY) ? std::max(0.0f, insetY) : 0.0f;
    const float maxX = std::max(0.0f, (surface.width - 1.0f) * 0.5f);
    const float maxY = std::max(0.0f, (surface.height - 1.0f) * 0.5f);
    const float x = std::min(safeX, maxX);
    const float y = std::min(safeY, maxY);
    return {surface.x + x, surface.y + y,
            std::max(1.0f, surface.width - x * 2.0f),
            std::max(1.0f, surface.height - y * 2.0f)};
}

inline bool FindCommittedDockModuleRect(
    const DockLayout& layout,
    std::string_view moduleId,
    const DockRect& work,
    float menuBarAndToolbar,
    float statusBar,
    const DockRootSlots& slots,
    DockRect& outRect) {
    if (!layout.Find(moduleId)) return false;
    const DockFrameGeometry frame = BuildDockFrameGeometry(
        layout, work, menuBarAndToolbar, statusBar, slots);
    for (int i = 0; i < frame.moduleCount; ++i) {
        if (frame.modules[i].id == moduleId) {
            outRect = frame.modules[i].rect;
            return outRect.width > 1.0f && outRect.height > 1.0f;
        }
    }
    return false;
}

} // namespace aura::ui::dock
