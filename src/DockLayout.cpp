#include "DockLayout.h"

#include <cmath>

namespace aura::ui::dock {
namespace {

Axis AxisForSide(DockSide side) {
    return (side == DockSide::Left || side == DockSide::Right) ? Axis::Horizontal : Axis::Vertical;
}

bool SidePlacesFirst(DockSide side) {
    return side == DockSide::Left || side == DockSide::Top;
}

ModuleNode* FindModule(DockNode* node, std::string_view id) {
    if (!node) return nullptr;
    if (node->Kind() == NodeKind::Module) {
        auto* module = static_cast<ModuleNode*>(node);
        return module->id == id ? module : nullptr;
    }
    if (node->Kind() == NodeKind::Split) {
        auto* split = static_cast<SplitNode*>(node);
        if (auto* found = FindModule(split->first.get(), id)) return found;
        return FindModule(split->second.get(), id);
    }
    return nullptr;
}

const ModuleNode* FindModule(const DockNode* node, std::string_view id) {
    return FindModule(const_cast<DockNode*>(node), id);
}

bool HasEmptySplitNode(const DockNode* node) {
    if (!node || node->Kind() != NodeKind::Split) return false;
    const auto* split = static_cast<const SplitNode*>(node);
    if (!split->first || !split->second) return true;
    return HasEmptySplitNode(split->first.get()) || HasEmptySplitNode(split->second.get());
}

int CountModules(const DockNode* node) {
    if (!node) return 0;
    if (node->Kind() == NodeKind::Module) return 1;
    if (node->Kind() != NodeKind::Split) return 0;
    const auto* split = static_cast<const SplitNode*>(node);
    return CountModules(split->first.get()) + CountModules(split->second.get());
}

int TreeDepth(const DockNode* node) {
    if (!node || node->Kind() != NodeKind::Split) return 0;
    const auto* split = static_cast<const SplitNode*>(node);
    return 1 + std::max(TreeDepth(split->first.get()), TreeDepth(split->second.get()));
}

std::unique_ptr<DockNode> Compress(std::unique_ptr<DockNode> node) {
    if (!node || node->Kind() != NodeKind::Split) return node;
    auto* split = static_cast<SplitNode*>(node.get());
    split->first = Compress(std::move(split->first));
    split->second = Compress(std::move(split->second));
    if (!split->first) return std::move(split->second);
    if (!split->second) return std::move(split->first);
    return node;
}

std::unique_ptr<DockNode> DetachFrom(std::unique_ptr<DockNode> node, std::string_view id, bool& removed) {
    if (!node) return nullptr;
    if (node->Kind() == NodeKind::Module) {
        auto* module = static_cast<ModuleNode*>(node.get());
        if (module->id == id) {
            removed = true;
            return nullptr;
        }
        return node;
    }
    if (node->Kind() != NodeKind::Split) return node;
    auto* split = static_cast<SplitNode*>(node.get());
    split->first = DetachFrom(std::move(split->first), id, removed);
    if (!removed) split->second = DetachFrom(std::move(split->second), id, removed);
    return Compress(std::move(node));
}

bool NodesEqual(const DockNode* a, const DockNode* b) {
    if (!a && !b) return true;
    if (!a || !b || a->Kind() != b->Kind()) return false;
    if (a->Kind() == NodeKind::Module) {
        return static_cast<const ModuleNode*>(a)->id == static_cast<const ModuleNode*>(b)->id;
    }
    if (a->Kind() == NodeKind::Float) {
        return static_cast<const FloatNode*>(a)->id == static_cast<const FloatNode*>(b)->id;
    }
    const auto* sa = static_cast<const SplitNode*>(a);
    const auto* sb = static_cast<const SplitNode*>(b);
    return sa->axis == sb->axis && std::fabs(sa->ratio - sb->ratio) < 1.0e-6f &&
        std::fabs(sa->firstWidthShare - sb->firstWidthShare) < 1.0e-6f &&
        std::fabs(sa->secondWidthShare - sb->secondWidthShare) < 1.0e-6f &&
        NodesEqual(sa->first.get(), sb->first.get()) && NodesEqual(sa->second.get(), sb->second.get());
}

std::unique_ptr<DockNode> CloneNode(const DockNode* node) {
    if (!node) return nullptr;
    if (node->Kind() == NodeKind::Module) {
        return std::make_unique<ModuleNode>(static_cast<const ModuleNode*>(node)->id);
    }
    if (node->Kind() == NodeKind::Float) {
        return std::make_unique<FloatNode>(static_cast<const FloatNode*>(node)->id);
    }
    const auto* split = static_cast<const SplitNode*>(node);
    auto copy = std::make_unique<SplitNode>();
    copy->axis = split->axis;
    copy->ratio = split->ratio;
    copy->minFirst = split->minFirst;
    copy->minSecond = split->minSecond;
    copy->firstWidthShare = split->firstWidthShare;
    copy->secondWidthShare = split->secondWidthShare;
    copy->first = CloneNode(split->first.get());
    copy->second = CloneNode(split->second.get());
    return copy;
}

void CollectLeaves(const DockNode* node, int depth, std::string& bestId, int& bestDepth) {
    if (!node) return;
    if (node->Kind() == NodeKind::Module) {
        const auto* module = static_cast<const ModuleNode*>(node);
        if (bestId.empty() || depth < bestDepth) {
            bestId = module->id;
            bestDepth = depth;
        }
        return;
    }
    if (node->Kind() != NodeKind::Split) return;
    const auto* split = static_cast<const SplitNode*>(node);
    CollectLeaves(split->first.get(), depth + 1, bestId, bestDepth);
    CollectLeaves(split->second.get(), depth + 1, bestId, bestDepth);
}

bool ReplaceModuleWithSplit(
    std::unique_ptr<DockNode>& node,
    std::string_view targetId,
    std::unique_ptr<DockNode> incoming,
    DockSide side,
    float ratio) {
    if (!node) return false;
    if (node->Kind() == NodeKind::Module) {
        auto* module = static_cast<ModuleNode*>(node.get());
        if (module->id != targetId) return false;
        auto split = std::make_unique<SplitNode>();
        split->axis = AxisForSide(side);
        const float clamped = std::clamp(ratio, 0.08f, 0.92f);
        if (SidePlacesFirst(side)) {
            split->ratio = clamped;
            split->first = std::move(incoming);
            split->second = std::move(node);
        } else {
            split->ratio = 1.0f - clamped;
            split->first = std::move(node);
            split->second = std::move(incoming);
        }
        node = std::move(split);
        return true;
    }
    if (node->Kind() != NodeKind::Split) return false;
    auto* split = static_cast<SplitNode*>(node.get());
    if (ReplaceModuleWithSplit(split->first, targetId, CloneNode(incoming.get()), side, ratio)) return true;
    return ReplaceModuleWithSplit(split->second, targetId, std::move(incoming), side, ratio);
}

} // namespace

DockLayout::DockLayout(const DockLayout& other) {
    for (int i = 0; i < 4; ++i) regions_[i] = CloneNode(other.regions_[i].get());
    root_ = CloneNode(other.root_.get());
    for (const auto& node : other.floats_) {
        floats_.push_back(std::make_unique<FloatNode>(node->id));
    }
}

DockLayout& DockLayout::operator=(const DockLayout& other) {
    if (this == &other) return *this;
    for (int i = 0; i < 4; ++i) regions_[i] = CloneNode(other.regions_[i].get());
    root_ = CloneNode(other.root_.get());
    floats_.clear();
    for (const auto& node : other.floats_) {
        floats_.push_back(std::make_unique<FloatNode>(node->id));
    }
    return *this;
}

bool DockLayout::InsertModule(std::string_view id, DockSide side, float ratio) {
    if (!IsKnownModuleId(id) || Find(id) || FindFloat(id)) return false;
    auto incoming = std::make_unique<ModuleNode>(std::string(id));
    const int index = RootRegionIndex(side);
    auto& region = regions_[index];
    if (CountModules(region.get()) >= kMaxModulesPerRootRegion) return false;
    if (TreeDepth(region.get()) >= kMaxSplitDepthPerRootRegion && region) return false;
    const float clamped = std::clamp(ratio, 0.08f, 0.92f);
    if (!region) {
        region = std::move(incoming);
        root_ = CloneNode(region.get());
        return true;
    }
    auto split = std::make_unique<SplitNode>();
    split->axis = AxisForSide(side);
    if (SidePlacesFirst(side)) {
        split->ratio = clamped;
        split->first = std::move(incoming);
        split->second = std::move(region);
    } else {
        split->ratio = 1.0f - clamped;
        split->first = std::move(region);
        split->second = std::move(incoming);
    }
    region = std::move(split);
    root_ = CloneNode(region.get());
    return true;
}

bool DockLayout::InsertAdjacent(std::string_view targetId, std::string_view incomingId, DockSide side, float ratio) {
    if (!IsKnownModuleId(incomingId) || Find(incomingId) || FindFloat(incomingId)) return false;
    if (!Find(targetId)) return false;
    for (int i = 0; i < 4; ++i) {
        if (!FindModule(regions_[i].get(), targetId)) continue;
        if (CountModules(regions_[i].get()) >= kMaxModulesPerRootRegion) return false;
        auto incoming = std::make_unique<ModuleNode>(std::string(incomingId));
        auto trial = CloneNode(regions_[i].get());
        if (!ReplaceModuleWithSplit(trial, targetId, CloneNode(incoming.get()), side, ratio)) return false;
        if (TreeDepth(trial.get()) > kMaxSplitDepthPerRootRegion) return false;
        if (!ReplaceModuleWithSplit(regions_[i], targetId, std::move(incoming), side, ratio)) return false;
        root_ = CloneNode(regions_[i].get());
        return true;
    }
    return false;
}

bool DockLayout::EnsureFloat(std::string_view id) {
    if (!IsKnownModuleId(id) || Find(id) || FindFloat(id)) return FindFloat(id) != nullptr;
    floats_.push_back(std::make_unique<FloatNode>(std::string(id)));
    return true;
}

bool DockLayout::Detach(std::string_view id) {
    bool removed = false;
    for (int i = 0; i < 4; ++i) {
        regions_[i] = DetachFrom(std::move(regions_[i]), id, removed);
        if (removed) {
            // root_ is a compatibility snapshot used by older callers. Keep it
            // aligned with the region that was actually mutated so a later
            // visibility sync cannot see a detached module through stale state.
            root_ = CloneNode(regions_[i].get());
            break;
        }
    }
    if (!removed) {
        root_ = DetachFrom(std::move(root_), id, removed);
    }
    if (!removed) {
        for (auto it = floats_.begin(); it != floats_.end(); ++it) {
            if ((*it)->id == id) {
                floats_.erase(it);
                return true;
            }
        }
        return false;
    }
    floats_.push_back(std::make_unique<FloatNode>(std::string(id)));
    return true;
}

bool DockLayout::RemoveFloat(std::string_view id) {
    for (auto it = floats_.begin(); it != floats_.end(); ++it) {
        if ((*it)->id == id) {
            floats_.erase(it);
            return true;
        }
    }
    return false;
}

ModuleNode* DockLayout::Find(std::string_view id) {
    for (int i = 0; i < 4; ++i) {
        if (auto* found = FindModule(regions_[i].get(), id)) return found;
    }
    return FindModule(root_.get(), id);
}

const ModuleNode* DockLayout::Find(std::string_view id) const {
    for (int i = 0; i < 4; ++i) {
        if (auto* found = FindModule(regions_[i].get(), id)) return found;
    }
    return FindModule(root_.get(), id);
}

FloatNode* DockLayout::FindFloat(std::string_view id) {
    for (auto& node : floats_) {
        if (node->id == id) return node.get();
    }
    return nullptr;
}

const FloatNode* DockLayout::FindFloat(std::string_view id) const {
    for (const auto& node : floats_) {
        if (node->id == id) return node.get();
    }
    return nullptr;
}

SplitNode* DockLayout::RootAsSplit() {
    for (int i = 0; i < 4; ++i) {
        if (regions_[i] && regions_[i]->Kind() == NodeKind::Split) {
            return static_cast<SplitNode*>(regions_[i].get());
        }
    }
    if (!root_ || root_->Kind() != NodeKind::Split) return nullptr;
    return static_cast<SplitNode*>(root_.get());
}

const SplitNode* DockLayout::RootAsSplit() const {
    return const_cast<DockLayout*>(this)->RootAsSplit();
}

SplitNode* DockLayout::RegionAsSplit(DockSide region) {
    auto& node = regions_[RootRegionIndex(region)];
    if (!node || node->Kind() != NodeKind::Split) return nullptr;
    return static_cast<SplitNode*>(node.get());
}

const SplitNode* DockLayout::RegionAsSplit(DockSide region) const {
    return const_cast<DockLayout*>(this)->RegionAsSplit(region);
}

bool DockLayout::ResizeSplit(SplitNode* split, float delta) {
    if (!split) return false;
    const float next = std::clamp(split->ratio + delta, split->minFirst, 1.0f - split->minSecond);
    if (next == split->ratio) return false;
    split->ratio = next;
    return true;
}

bool DockLayout::HasEmptySplit() const {
    for (int i = 0; i < 4; ++i) {
        if (HasEmptySplitNode(regions_[i].get())) return true;
    }
    return HasEmptySplitNode(root_.get());
}

bool DockLayout::Equals(const DockLayout& other) const {
    if (floats_.size() != other.floats_.size()) return false;
    for (size_t i = 0; i < floats_.size(); ++i) {
        if (floats_[i]->id != other.floats_[i]->id) return false;
    }
    for (int i = 0; i < 4; ++i) {
        if (!NodesEqual(regions_[i].get(), other.regions_[i].get())) return false;
    }
    return NodesEqual(root_.get(), other.root_.get());
}

int DockLayout::CountInRootRegion(DockSide region) const {
    return CountModules(regions_[RootRegionIndex(region)].get());
}

int DockLayout::DepthInRootRegion(DockSide region) const {
    return TreeDepth(regions_[RootRegionIndex(region)].get());
}

std::string DockLayout::ShallowestModuleInRootRegion(DockSide region) const {
    std::string id;
    int best = 1 << 30;
    CollectLeaves(regions_[RootRegionIndex(region)].get(), 0, id, best);
    return id;
}

} // namespace aura::ui::dock
