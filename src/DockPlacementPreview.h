#pragma once

#include "DockHitTest.h"

#include <array>

namespace aura::ui::dock {

enum class DockPlacement { Tabs, Left, Right, Top, Bottom };

struct DockPlacementTarget {
    DockPlacement placement;
    DockRect marker;
    DockRect highlight;
};

using DockPlacementPreview = std::array<DockPlacementTarget, 5>;

inline bool DockTabMemberFits(const DockRect& surface, std::string_view member, float scale) {
    const auto minimum = DockModuleMinimum(member, scale, DockMinimumPurpose::Resize);
    return surface.width >= minimum.width && surface.height >= minimum.height;
}

inline DockPlacementPreview PlanDockPlacementPreview(const DockRect& surface, float scale) {
    DockPlacementPreview preview{};
    const float markerSize = 28.0f * scale;
    const float step = 36.0f * scale;
    const float span = markerSize + 2.0f * step;
    if (!(scale > 0.0f && surface.width >= span && surface.height >= span)) return preview;

    const float halfWidth = surface.width * 0.5f;
    const float halfHeight = surface.height * 0.5f;
    const float markerLeft = surface.x + halfWidth - markerSize * 0.5f;
    const float markerTop = surface.y + halfHeight - markerSize * 0.5f;
    const auto marker = [=](float dx, float dy) {
        return DockRect{markerLeft + dx, markerTop + dy, markerSize, markerSize};
    };
    return {{
        {DockPlacement::Tabs, marker(0.0f, 0.0f), surface},
        {DockPlacement::Left, marker(-step, 0.0f), {surface.x, surface.y, halfWidth, surface.height}},
        {DockPlacement::Right, marker(step, 0.0f), {surface.x + halfWidth, surface.y, halfWidth, surface.height}},
        {DockPlacement::Top, marker(0.0f, -step), {surface.x, surface.y, surface.width, halfHeight}},
        {DockPlacement::Bottom, marker(0.0f, step), {surface.x, surface.y + halfHeight, surface.width, halfHeight}},
    }};
}

inline const DockPlacementTarget* HitDockPlacementPreview(const DockPlacementPreview& preview, float x, float y) {
    for (const auto& target : preview) {
        const auto& marker = target.marker;
        if (x >= marker.x && x < marker.x + marker.width &&
            y >= marker.y && y < marker.y + marker.height) return &target;
    }
    return nullptr;
}

}
