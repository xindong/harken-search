#include "DockDragSession.h"

namespace aura::ui::dock {

bool DockDragSession::BeginDrag(std::string_view moduleId, bool fromUnplacedWorkspace) {
    if (dragging_) return false;
    if (!IsKnownModuleId(moduleId)) return false;
    if (!fromUnplacedWorkspace && !layout_.Find(moduleId) && !layout_.FindFloat(moduleId)) return false;
    snapshot_ = layout_;
    moduleId_ = std::string(moduleId);
    preview_ = {};
    dragging_ = true;
    return true;
}

void DockDragSession::UpdatePreview(const DockHit& hit) {
    if (!dragging_) return;
    preview_ = hit;
}

DockHit DockDragSession::CommitOnMouseUp() {
    DockHit committed;
    if (!dragging_) return committed;
    committed = preview_;
    if (preview_.kind == DockHitKind::None) {
        if (layout_.Find(moduleId_)) layout_.Detach(moduleId_);
    } else {
        if (layout_.Find(moduleId_)) layout_.Detach(moduleId_);
        layout_.RemoveFloat(moduleId_);
        bool inserted = preview_.kind == DockHitKind::ResultsHome && moduleId_ == "search-results";
        if (preview_.kind == DockHitKind::Module && !preview_.targetModuleId.empty() &&
            preview_.targetModuleId != moduleId_) {
            inserted = layout_.InsertAdjacent(preview_.targetModuleId, moduleId_, preview_.side, kDockIncomingShare);
        } else if (preview_.kind != DockHitKind::ResultsHome) {
            inserted = layout_.InsertModule(moduleId_, preview_.side, kDockIncomingShare);
        }
        if (!inserted) layout_.EnsureFloat(moduleId_);
    }
    dragging_ = false;
    preview_ = {};
    moduleId_.clear();
    return committed;
}

void DockDragSession::CommitFloatingOnMouseUp() {
    if (!dragging_) return;
    if (layout_.Find(moduleId_)) layout_.Detach(moduleId_);
    layout_.EnsureFloat(moduleId_);
    dragging_ = false;
    preview_ = {};
    moduleId_.clear();
}

void DockDragSession::Cancel() {
    if (!dragging_) return;
    layout_ = snapshot_;
    dragging_ = false;
    preview_ = {};
    moduleId_.clear();
}

} // namespace aura::ui::dock
