#pragma once

#include <algorithm>

namespace harken::caption {

struct Rect {
    int left = 0;
    int top = 0;
    int right = 0;
    int bottom = 0;

    bool Contains(int x, int y) const {
        return x >= left && x < right && y >= top && y < bottom;
    }
};

enum class Hit { None, Caption, Minimize, Maximize, Close };

struct Layout {
    Rect caption;
    Rect minimize;
    Rect maximize;
    Rect close;
};

inline Layout CalculateLayout(int width, int height, int frame,
                              int captionHeight, int buttonWidth,
                              bool hasMinimize, bool hasMaximize) {
    const int border = std::max(0, frame);
    const int left = std::min(border, width);
    const int right = std::max(left, width - border);
    const int top = std::min(border, height);
    const int bottom = std::max(top, std::min(height - border, top + std::max(0, captionHeight)));
    const int button = std::max(0, buttonWidth);
    Layout layout{};
    layout.caption = {left, top, right, bottom};
    int edge = right;
    if (edge - button >= left) {
        layout.close = {edge - button, top, edge, bottom};
        edge -= button;
    }
    if (hasMaximize && edge - button >= left) {
        layout.maximize = {edge - button, top, edge, bottom};
        edge -= button;
    }
    if (hasMinimize && edge - button >= left) {
        layout.minimize = {edge - button, top, edge, bottom};
    }
    return layout;
}

inline Hit HitTest(const Layout& layout, int x, int y) {
    if (!layout.caption.Contains(x, y)) return Hit::None;
    if (layout.close.Contains(x, y)) return Hit::Close;
    if (layout.maximize.Contains(x, y)) return Hit::Maximize;
    if (layout.minimize.Contains(x, y)) return Hit::Minimize;
    return Hit::Caption;
}

} // namespace harken::caption
