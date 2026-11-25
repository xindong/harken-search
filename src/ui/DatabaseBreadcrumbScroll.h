#pragma once

#include <algorithm>
#include <string>

namespace harken::breadcrumb {

class ScrollState {
public:
    void setPath(const std::string& path) {
        if (path_ != path) {
            path_ = path;
            offset_ = 0.0f;
        }
    }

    float update(float contentWidth, float viewportMinX, float viewportWidth,
                 float mouseX, float deltaTime, bool hovered, bool leftButtonDown) noexcept {
        constexpr float scrollSpeed = 120.0f;
        const float maxOffset = (std::max)(0.0f, contentWidth - viewportWidth);
        if (maxOffset > 0.0f && !leftButtonDown && hovered) {
            const float edgeWidth = (std::min)(20.0f, viewportWidth * 0.25f);
            const float step = scrollSpeed * (std::min)(deltaTime, 0.1f);
            if (mouseX < viewportMinX + edgeWidth) offset_ -= step;
            if (mouseX >= (viewportMinX + viewportWidth) - edgeWidth) offset_ += step;
        }
        offset_ = std::clamp(offset_, 0.0f, maxOffset);
        return offset_;
    }

private:
    std::string path_;
    float offset_ = 0.0f;
};

}
