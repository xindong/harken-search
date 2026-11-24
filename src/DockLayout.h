#pragma once

#include <algorithm>
#include <array>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace aura::ui::dock {

enum class Axis { Horizontal, Vertical };
enum class DockSide { Left, Right, Top, Bottom };
enum class NodeKind { Split, Module, Float };

constexpr int kMaxModulesPerRootRegion = 16;
constexpr int kMaxSplitDepthPerRootRegion = 4;
// Share a newly docked leaf takes from its target leaf. Both halves must stay
// readable, so this is a near-even split instead of a thin strip.
constexpr float kDockIncomingShare = 0.5f;

inline bool IsKnownModuleId(std::string_view id) {
    return id == "filter" || id == "search-results" || id == "info" ||
        id == "similar-sounds" || id.find("mod-") == 0;
}

inline int RootRegionIndex(DockSide side) {
    switch (side) {
        case DockSide::Left: return 0;
        case DockSide::Right: return 1;
        case DockSide::Top: return 2;
        case DockSide::Bottom: return 3;
    }
    return 0;
}

struct DockNode {
    virtual ~DockNode() = default;
    virtual NodeKind Kind() const = 0;
};

struct ModuleNode : DockNode {
    explicit ModuleNode(std::string moduleId) : id(std::move(moduleId)) {}
    NodeKind Kind() const override { return NodeKind::Module; }
    std::string id;
};

struct FloatNode : DockNode {
    explicit FloatNode(std::string moduleId) : id(std::move(moduleId)) {}
    NodeKind Kind() const override { return NodeKind::Float; }
    std::string id;
};

struct SplitNode : DockNode {
    NodeKind Kind() const override { return NodeKind::Split; }
    Axis axis = Axis::Horizontal;
    float ratio = 0.5f;
    float minFirst = 0.08f;
    float minSecond = 0.08f;
    float firstWidthShare = 1.0f;
    float secondWidthShare = 1.0f;
    std::unique_ptr<DockNode> first;
    std::unique_ptr<DockNode> second;
};

class DockLayout {
public:
    DockLayout() = default;
    DockLayout(const DockLayout& other);
    DockLayout& operator=(const DockLayout& other);
    DockLayout(DockLayout&&) noexcept = default;
    DockLayout& operator=(DockLayout&&) noexcept = default;

    bool InsertModule(std::string_view id, DockSide side, float ratio);
    bool InsertAdjacent(std::string_view targetId, std::string_view incomingId, DockSide side, float ratio);
    bool EnsureFloat(std::string_view id);
    bool Detach(std::string_view id);
    bool RemoveFloat(std::string_view id);
    ModuleNode* Find(std::string_view id);
    const ModuleNode* Find(std::string_view id) const;
    FloatNode* FindFloat(std::string_view id);
    const FloatNode* FindFloat(std::string_view id) const;
    SplitNode* RootAsSplit();
    const SplitNode* RootAsSplit() const;
    SplitNode* RegionAsSplit(DockSide region);
    const SplitNode* RegionAsSplit(DockSide region) const;
    bool ResizeSplit(SplitNode* split, float delta);
    bool HasEmptySplit() const;
    bool Equals(const DockLayout& other) const;
    int CountInRootRegion(DockSide region) const;
    int DepthInRootRegion(DockSide region) const;
    std::string ShallowestModuleInRootRegion(DockSide region) const;
    DockNode* RegionRoot(DockSide region) { return regions_[RootRegionIndex(region)].get(); }
    const DockNode* RegionRoot(DockSide region) const { return regions_[RootRegionIndex(region)].get(); }
    DockNode* Root() { return root_.get(); }
    const DockNode* Root() const { return root_.get(); }
    const std::vector<std::unique_ptr<FloatNode>>& Floats() const { return floats_; }

private:
    std::array<std::unique_ptr<DockNode>, 4> regions_{};
    std::unique_ptr<DockNode> root_;
    std::vector<std::unique_ptr<FloatNode>> floats_;
};

} // namespace aura::ui::dock
