#pragma once

#include "../../DockHitTest.h"

namespace aura::ui::dock {

// Persisted panel-side values mirror HarkenSearch::DockSide after it gains Top/Bottom:
// 0 = Left, 1 = Right, 2 = Top, 3 = Bottom.
struct PanelDockState {
    int dockSide = 0;
    bool floating = false;
    float dockedWidth = 250.0f;
    float dockedHeight = 200.0f;
    float floatX = 100.0f;
    float floatY = 100.0f;
    float floatW = 300.0f;
    float floatH = 400.0f;
};

inline int DockSideToSavedSide(DockSide side) {
    switch (side) {
    case DockSide::Right:
        return 1;
    case DockSide::Top:
        return 2;
    case DockSide::Bottom:
        return 3;
    case DockSide::Left:
        break;
    }
    return 0;
}

inline DockSide SavedSideToDockSide(int savedSide) {
    switch (savedSide) {
    case 1:
        return DockSide::Right;
    case 2:
        return DockSide::Top;
    case 3:
        return DockSide::Bottom;
    default:
        return DockSide::Left;
    }
}

inline bool SideIsVertical(DockSide side) {
    return side == DockSide::Top || side == DockSide::Bottom;
}

// Mirror a committed dock hit into the persisted panel state. The committed
// side's axis decides which extent field changes, never the module id.
inline void ApplyDockCommitToPanel(
    PanelDockState& panel,
    DockSide rootSide,
    DockSide extentSide,
    const DockRect& committedLeaf) {
    panel.dockSide = DockSideToSavedSide(rootSide);
    panel.floating = false;
    if (SideIsVertical(extentSide)) {
        if (committedLeaf.height > 1.0f) {
            panel.dockedHeight = committedLeaf.height;
        }
    } else {
        if (committedLeaf.width > 1.0f) {
            panel.dockedWidth = committedLeaf.width;
        }
    }
}

inline void ApplyDockCommitToPanel(
    PanelDockState& panel, DockSide side, const DockRect& incoming) {
    ApplyDockCommitToPanel(panel, side, side, incoming);
}

inline bool BeginDockPanelFloat(
    DockLayout& layout,
    std::string_view moduleId,
    bool& floating,
    float& floatX,
    float& floatY,
    float& floatW,
    float& floatH,
    const DockRect& committedLeafRect) {
    if (committedLeafRect.width <= 1.0f || committedLeafRect.height <= 1.0f) {
        return false;
    }
    if (layout.Find(moduleId)) layout.Detach(moduleId);
    layout.RemoveFloat(moduleId);
    if (!layout.EnsureFloat(moduleId)) return false;

    floating = true;
    floatX = committedLeafRect.x;
    floatY = committedLeafRect.y;
    floatW = committedLeafRect.width;
    floatH = committedLeafRect.height;
    return true;
}

template <typename PanelState>
inline bool BeginDockPanelFloat(
    DockLayout& layout, std::string_view moduleId, PanelState& panel,
    const DockRect& committedLeafRect) {
    return BeginDockPanelFloat(
        layout, moduleId, panel.floating, panel.floatX, panel.floatY,
        panel.floatW, panel.floatH, committedLeafRect);
}

inline bool UpdateFloatingDockPanelGeometry(
    bool floating,
    float& floatX,
    float& floatY,
    float& floatW,
    float& floatH,
    const DockRect& actualWindowRect) {
    if (!floating || actualWindowRect.width <= 1.0f || actualWindowRect.height <= 1.0f) {
        return false;
    }
    floatX = actualWindowRect.x;
    floatY = actualWindowRect.y;
    floatW = actualWindowRect.width;
    floatH = actualWindowRect.height;
    return true;
}

template <typename PanelState>
inline bool UpdateFloatingDockPanelGeometry(
    PanelState& panel, const DockRect& actualWindowRect) {
    return UpdateFloatingDockPanelGeometry(
        panel.floating, panel.floatX, panel.floatY, panel.floatW,
        panel.floatH, actualWindowRect);
}

struct DockColumnBudget {
    bool showLeft = false;
    bool showRight = false;
    float leftWidth = 0.0f;
    float rightWidth = 0.0f;
    float centerWidth = 0.0f;
};

inline DockColumnBudget ResolveDockColumnBudget(
    float mainWidth,
    bool showLeft,
    bool showRight,
    float leftWidth,
    float rightWidth,
    float splitterWidth,
    float minReadableCenterWidth,
    float minRightWidth) {
    DockColumnBudget budget;
    budget.showLeft = showLeft;
    budget.showRight = showRight;
    budget.leftWidth = showLeft ? leftWidth : 0.0f;
    budget.rightWidth = showRight ? rightWidth : 0.0f;
    const float leftSplitter = showLeft ? splitterWidth : 0.0f;
    if (budget.showLeft && budget.showRight &&
        mainWidth - budget.leftWidth - budget.rightWidth - (leftSplitter + splitterWidth) < minReadableCenterWidth) {
        const float fittedRight = std::max(
            minRightWidth,
            mainWidth - budget.leftWidth - (leftSplitter + splitterWidth) - minReadableCenterWidth);
        if (fittedRight >= minRightWidth) {
            budget.rightWidth = std::min(budget.rightWidth, fittedRight);
        } else {
            budget.showRight = false;
            budget.rightWidth = 0.0f;
        }
    }
    budget.centerWidth = std::max(
        0.0f,
        mainWidth - budget.leftWidth - budget.rightWidth -
            (budget.showLeft ? splitterWidth : 0.0f) -
            (budget.showRight ? splitterWidth : 0.0f));
    return budget;
}

inline constexpr float kDockBandMinHeight = 96.0f;
inline constexpr float kDockBandMaxHeight = 320.0f;

inline void PurgeObsoleteDockTestModules(DockLayout& layout) {
    for (const char* id : {"mod-docktest-similar", "mod-docktest-band"}) {
        if (layout.Find(id)) layout.Detach(id);
        layout.RemoveFloat(id);
    }
}

inline void ResetDockTestLayoutLeaves(DockLayout& layout) {
    layout.InsertModule("mod-docktest-filter", DockSide::Left, 0.24f);
    layout.InsertModule("mod-docktest-info", DockSide::Right, 0.24f);
}

struct DockBandReserve {
    float top = 0.0f;
    float bottom = 0.0f;
    bool floatTop = false;
    bool floatBottom = false;
};

// Use every real module's saved height, not just the legacy Filter/Info pair.
template <typename HeightForModule>
inline DockBandReserve ResolveDockBandReserve(
    const DockLayout& layout, HeightForModule heightForModule,
    float fallbackHeight, float scale,
    float availableHeight = std::numeric_limits<float>::infinity(),
    float centerWidth = std::numeric_limits<float>::infinity(),
    float centerMinimumHeight = 200.0f) {
    auto height = [&](DockSide side) {
        const DockNode* root = layout.RegionRoot(side);
        if (!root) return 0.0f;
        float saved = 0.0f;
        auto visit = [&](auto&& self, const DockNode* node) -> void {
            if (!node) return;
            if (node->Kind() == NodeKind::Module) {
                saved = std::max(saved, heightForModule(static_cast<const ModuleNode*>(node)->id));
            } else {
                const auto* split = static_cast<const SplitNode*>(node);
                self(self, split->first.get());
                self(self, split->second.get());
            }
        };
        visit(visit, root);
        const float preferred = std::clamp(saved > 0.0f ? saved : fallbackHeight,
            kDockBandMinHeight, kDockBandMaxHeight);
        return std::max(preferred, DockSubtreeMinimum(root, scale).height);
    };
    DockBandReserve result{height(DockSide::Top), height(DockSide::Bottom)};
    auto topMinimum = DockSubtreeMinimum(layout.RegionRoot(DockSide::Top), scale);
    auto bottomMinimum = DockSubtreeMinimum(layout.RegionRoot(DockSide::Bottom), scale);
    auto floatBand = [](float& extent, DockMinimumSize& minimum, bool& floating) {
        floating = floating || extent > 0.0f;
        extent = 0.0f;
        minimum = {0.0f, 0.0f};
    };
    if (topMinimum.width > centerWidth + .5f)
        floatBand(result.top, topMinimum, result.floatTop);
    if (bottomMinimum.width > centerWidth + .5f)
        floatBand(result.bottom, bottomMinimum, result.floatBottom);
    const float budget = std::max(0.0f, availableHeight - centerMinimumHeight);
    auto gaps = [&]() {
        return kDockBandSplitterHeight * ((result.top > 0.0f ? 1 : 0) +
                                         (result.bottom > 0.0f ? 1 : 0));
    };
    // Reduce remembered heights first. If even readable floors cannot fit,
    // release bottom then top rather than moving the list/waveform offscreen.
    if (topMinimum.height + bottomMinimum.height + gaps() > budget + .5f)
        floatBand(result.bottom, bottomMinimum, result.floatBottom);
    if (topMinimum.height + gaps() > budget + .5f)
        floatBand(result.top, topMinimum, result.floatTop);
    const float extra = result.top + result.bottom - topMinimum.height - bottomMinimum.height;
    const float room = std::max(0.0f, budget - gaps() - topMinimum.height - bottomMinimum.height);
    if (extra > room && extra > 0.0f) {
        const float ratio = room / extra;
        result.top = topMinimum.height + (result.top - topMinimum.height) * ratio;
        result.bottom = bottomMinimum.height + (result.bottom - bottomMinimum.height) * ratio;
    }
    return result;
}

inline DockBandReserve ResolveDockBandReserve(
    const DockLayout& layout,
    float filterDockedHeight,
    float infoDockedHeight,
    float fallbackHeight) {
    DockBandReserve reserve;
    auto regionHeight = [&](DockSide side) -> float {
        const DockNode* root = layout.RegionRoot(side);
        if (root == nullptr) {
            return 0.0f;
        }
        float height = 0.0f;
        bool hasHeightOwner = false;
        if (DockSubtreeContainsModule(root, "filter")) {
            hasHeightOwner = true;
            height = std::max(height, filterDockedHeight);
        }
        if (DockSubtreeContainsModule(root, "info")) {
            hasHeightOwner = true;
            height = std::max(height, infoDockedHeight);
        }
        if (hasHeightOwner) {
            height = std::max(kDockBandMinHeight,
                              std::min(height, kDockBandMaxHeight));
        } else {
            height = fallbackHeight;
        }
        return height;
    };
    reserve.top = regionHeight(DockSide::Top);
    reserve.bottom = regionHeight(DockSide::Bottom);
    return reserve;
}

inline constexpr double kDockTitleHoldSeconds = 0.45;
inline constexpr float kDockTitleDragDistance = 6.0f;

struct DockTitleGestureStep {
    bool beginDrag = false;
    bool released = false;
    bool cancelled = false;
};

// One shared press owner for docked titles and floating title labels. The UI
// only calls Press on an actual title press, never on hover while already down.
// Advance deliberately does not depend on the old title's current hover state.
class DockTitleGesture {
public:
    bool Press(std::string_view module, bool floating, double now,
               float x, float y, float scale = 1.0f) {
        if (Active() || !IsKnownModuleId(module)) return false;
        module_ = std::string(module);
        fromFloating_ = floating;
        startedAt_ = now;
        startX_ = x;
        startY_ = y;
        threshold_ = kDockTitleDragDistance * std::clamp(scale, 0.5f, 4.0f);
        return true;
    }

    DockTitleGestureStep Advance(bool down, bool cancel, double now, float x, float y) {
        DockTitleGestureStep step;
        if (!Active()) return step;
        if (cancel || !std::isfinite(x) || !std::isfinite(y)) {
            step.cancelled = true;
            return step;
        }
        const float dx = x - startX_;
        const float dy = y - startY_;
        moved_ |= dx * dx + dy * dy >= threshold_ * threshold_;
        if (!down) {
            step.released = true;
            return step;
        }
        if (!dragging_ && (moved_ || (!fromFloating_ && now - startedAt_ >= kDockTitleHoldSeconds))) {
            dragging_ = true;
            step.beginDrag = true;
        }
        return step;
    }

    void Reset() { *this = {}; }
    bool Active() const { return !module_.empty(); }
    bool Owns(std::string_view module) const { return Active() && module_ == module; }
    bool Dragging() const { return dragging_; }
    bool DropArmed() const { return dragging_ && moved_; }
    bool FromFloating() const { return fromFloating_; }
    float HoldProgress(double now) const {
        if (!Active() || fromFloating_ || dragging_ || !std::isfinite(now)) return 0.0f;
        return static_cast<float>(std::clamp((now - startedAt_) / kDockTitleHoldSeconds, 0.0, 1.0));
    }
    const std::string& ModuleId() const { return module_; }
    float StartX() const { return startX_; }
    float StartY() const { return startY_; }

private:
    std::string module_;
    double startedAt_ = 0.0;
    float startX_ = 0.0f;
    float startY_ = 0.0f;
    float threshold_ = kDockTitleDragDistance;
    bool fromFloating_ = false;
    bool dragging_ = false;
    bool moved_ = false;
};

} // namespace aura::ui::dock
