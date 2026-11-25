#pragma once

#include "DockHitTest.h"
#include "DockLayout.h"

#include <string>

namespace aura::ui::dock {

class DockDragSession {
public:
    explicit DockDragSession(DockLayout& layout) : layout_(layout) {}

    bool BeginDrag(std::string_view moduleId, bool fromUnplacedWorkspace = false);
    void UpdatePreview(const DockHit& hit);
    DockHit CommitOnMouseUp();
    DockHit CommitOnMouseUp(const DockDropPlan& plan);
    // Finish a title-bar handoff while keeping the module floating.  This
    // deliberately ignores any stale preview from an earlier frame.
    void CommitFloatingOnMouseUp();
    void Cancel();
    bool Active() const { return dragging_; }
    bool OwnsDrag(std::string_view moduleId) const { return dragging_ && moduleId_ == moduleId; }
    const DockHit& Preview() const { return preview_; }

    template <typename ApplyFn>
    DockHit CommitThenApply(const DockHit& leftoverHit, ApplyFn&& apply) {
        const DockHit committed = PreferCommittedDockHit(preview_, leftoverHit);
        CommitOnMouseUp();
        apply(committed);
        return committed;
    }

private:
    DockLayout& layout_;
    DockLayout snapshot_;
    std::string moduleId_;
    DockHit preview_;
    bool dragging_ = false;
};

} // namespace aura::ui::dock
