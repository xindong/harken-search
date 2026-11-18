#pragma once

#include "imgui.h"
#include "imgui_internal.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>

namespace aura::folder_tree::controls {

inline constexpr float kButtonSize = 24.0f;

inline bool ContainsPath(std::string_view folder, std::string_view target) {
    if (folder.empty() || target.size() < folder.size() ||
        target.compare(0, folder.size(), folder) != 0) {
        return false;
    }
    const auto separator = [](char ch) { return ch == '/' || ch == '\\'; };
    return target.size() == folder.size() || separator(folder.back()) ||
           separator(target[folder.size()]);
}

class RefreshMotion {
public:
    float Update(double now, bool clicked, bool busy) {
        if (startedAt_ < 0.0 || (!wasBusy_ && now >= stopAt_)) {
            if (!clicked && !busy) {
                return 0.0f;
            }
            startedAt_ = now;
            stopAt_ = now + kTurnSeconds;
        }
        const double turns = (now - startedAt_) / kTurnSeconds;
        if (wasBusy_ && !busy) {
            stopAt_ = startedAt_ + std::max(1.0, std::ceil(turns - 1e-6)) * kTurnSeconds;
        }
        wasBusy_ = busy;
        return !busy && now >= stopAt_ - 1e-6 ? 0.0f : static_cast<float>(turns - std::floor(turns));
    }

private:
    static constexpr double kTurnSeconds = 0.420;
    double startedAt_ = -1.0;
    double stopAt_ = -1.0;
    bool wasBusy_ = false;
};

class LocateFeedback {
public:
    void Request(std::string_view path) {
        pendingPath_ = path;
        flashedPath_.clear();
    }

    const std::string& PendingPath() const { return pendingPath_; }

    void MarkScrolled(std::string_view path, double now) {
        if (!pendingPath_.empty() && pendingPath_ == path) {
            flashedPath_ = pendingPath_;
            flashedAt_ = now;
        }
    }

    float Strength(std::string_view path, double now) const {
        if (flashedPath_.empty() || flashedPath_ != path || now < flashedAt_) {
            return 0.0f;
        }
        const float remaining = static_cast<float>(std::max(0.0, 1.0 - (now - flashedAt_) / kFlashSeconds));
        return remaining * remaining;
    }

    void EndFrame(double now) {
        pendingPath_.clear();
        if (now >= flashedAt_ + kFlashSeconds) {
            flashedPath_.clear();
        }
    }

private:
    static constexpr double kFlashSeconds = 0.800;
    std::string pendingPath_;
    std::string flashedPath_;
    double flashedAt_ = 0.0;
};

class Interaction {
public:
    void BeginFrame(int frame, ImGuiID panel, std::string_view library, int tab,
                    int64_t playlist, bool browser) {
        if (frame > lastFrame_ + 1 || panel != panel_ || library != library_ ||
            tab != tab_ || playlist != playlist_ || browser != browser_) {
            refresh = {};
            locate = {};
            panel_ = panel;
            library_ = library;
            tab_ = tab;
            playlist_ = playlist;
            browser_ = browser;
        }
        lastFrame_ = frame;
    }

    RefreshMotion refresh;
    LocateFeedback locate;

private:
    int lastFrame_ = -1;
    ImGuiID panel_ = 0;
    std::string library_;
    int tab_ = 0;
    int64_t playlist_ = -1;
    bool browser_ = false;
};

inline void DrawGlyph(ImDrawList* drawList, ImVec2 center, const char* text, ImU32 color, float turns) {
    constexpr float kIconEm = 20.0f;
    unsigned int codepoint = 0;
    ImTextCharFromUtf8(&codepoint, text, nullptr);
    ImFont* font = ImGui::GetFont();
    const ImFontGlyph* glyph = font->GetFontBaked(kIconEm)->FindGlyphNoFallback(static_cast<ImWchar>(codepoint));
    if (!glyph || !glyph->Visible) {
        return;
    }

    const float halfWidth = (glyph->X1 - glyph->X0) * 0.5f;
    const float halfHeight = (glyph->Y1 - glyph->Y0) * 0.5f;
    const float angle = turns * 2.0f * IM_PI;
    const float cosine = std::cos(angle);
    const float sine = std::sin(angle);
    const auto point = [&](float horizontal, float vertical) {
        return ImVec2(center.x + horizontal * cosine - vertical * sine,
                      center.y + horizontal * sine + vertical * cosine);
    };
    drawList->AddImageQuad(font->OwnerAtlas->TexRef,
                           point(-halfWidth, -halfHeight), point(halfWidth, -halfHeight),
                           point(halfWidth, halfHeight), point(-halfWidth, halfHeight),
                           ImVec2(glyph->U0, glyph->V0), ImVec2(glyph->U1, glyph->V0),
                           ImVec2(glyph->U1, glyph->V1), ImVec2(glyph->U0, glyph->V1), color);
}

inline bool DrawButton(const char* id, const char* glyph, bool enabled,
                       const ImVec4& glyphColor, const ImVec4& hoverFill,
                       RefreshMotion* refresh = nullptr, bool busy = false) {
    if (ImGui::GetCurrentWindow()->SkipItems) {
        return false;
    }
    ImGui::BeginDisabled(!enabled);
    const bool clicked = ImGui::InvisibleButton(id, ImVec2(kButtonSize, kButtonSize), ImGuiButtonFlags_EnableNav);
    const float turns = refresh ? refresh->Update(ImGui::GetTime(), clicked, busy) : 0.0f;
    if (ImGui::IsItemVisible()) {
        auto* drawList = ImGui::GetWindowDrawList();
        const ImVec2 min = ImGui::GetItemRectMin();
        const ImVec2 max = ImGui::GetItemRectMax();
        if (enabled && (ImGui::IsItemHovered() || ImGui::IsItemActive())) {
            drawList->AddRectFilled(min, max, ImGui::GetColorU32(hoverFill), 5.0f);
        }
        DrawGlyph(drawList, ImVec2((min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f),
                  glyph, ImGui::GetColorU32(glyphColor), turns);
    }
    ImGui::EndDisabled();
    return clicked;
}

}
