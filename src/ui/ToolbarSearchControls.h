#pragma once

#include "../Theme.h"
#include "TransportContrast.h"
#include "imgui_internal.h"

#include <cstddef>
#include <limits>

namespace harken::toolbar {

inline constexpr float kSearchTextOffset = 36.0f;
inline constexpr float kSearchClearWidth = 28.0f;

inline float SearchFieldHeight()
{
    return std::max(26.0f, ImGui::GetFontSize() + 4.0f);
}

struct Palette {
    ImVec4 searchFill, searchIcon, preciseIcon, iconHover, iconActive;
    ImVec4 text, hint, selection, focus;
};

inline ImVec4 BlendTint(ImVec4 base, ImVec4 tint, float amount)
{
    tint.w *= amount;
    return TransportContrast::Over(tint, base);
}

inline Palette ResolveSearchPalette(const ThemeColorScheme& theme, ImVec4 surface, ImVec4 field, ImVec4 text)
{
    using TransportContrast::ForSurface;
    Palette palette{};
    palette.searchFill = TransportContrast::Over(field, surface);
    if (TransportContrast::Luminance(surface) < 0.15)
        palette.searchFill = ForSurface(palette.searchFill, surface, 1.5);
    palette.text = ForSurface(text, palette.searchFill);
    palette.hint = ForSurface(BlendTint(palette.text, palette.searchFill, 0.30f), palette.searchFill);
    palette.searchIcon = ForSurface(BlendTint(palette.text, theme.toolbarAccent, 0.15f), palette.searchFill);
    palette.preciseIcon = ForSurface(BlendTint(theme.searchButtonBG, theme.toolbarAccent, 0.25f), palette.searchFill);
    palette.iconHover = BlendTint(palette.searchFill, theme.toolbarAccent, 0.14f);
    palette.iconActive = BlendTint(palette.searchFill, theme.toolbarAccent, 0.24f);
    palette.selection = theme.resultsSelection;
    palette.selection.w *= 0.32f;
    palette.focus = ForSurface(theme.controlAccent, palette.searchFill, 3.0);
    return palette;
}

struct SearchFieldResult {
    bool submitted = false;
    bool cleared = false;
    bool preciseToggled = false;
    bool preciseHovered = false;
    bool focused = false;
    bool active = false;
};

struct SearchFieldOptions {
    bool allowPrecise = false;
    bool precise = false;
    bool requestFocus = false;
    float minHeight = 0.0f;
};

inline SearchFieldResult DrawSearchField(const char* label, const char* placeholder, char* buffer,
    std::size_t capacity, float width, const Palette& palette, const SearchFieldOptions& options = {})
{
    SearchFieldResult result;
    if (ImGui::GetCurrentWindow()->SkipItems || !buffer || capacity == 0
        || capacity > static_cast<std::size_t>((std::numeric_limits<int>::max)())
        || width <= kSearchTextOffset + kSearchClearWidth) {
        return result;
    }

    const float height = std::max(SearchFieldHeight(), options.minHeight);
    const float radius = height * 0.5f;
    const ImVec2 start = ImGui::GetCursorScreenPos();
    const ImVec2 end(start.x + width, start.y + height);
    const float textInset = !options.allowPrecise && width < 160.0f ? 28.0f : kSearchTextOffset;
    const ImVec2 textStart(start.x + textInset, start.y);
    const ImVec2 clearStart(end.x - kSearchClearWidth, start.y);
    const float centerY = start.y + radius;
    auto* draw = ImGui::GetWindowDrawList();
    auto& context = *ImGui::GetCurrentContext();
    const bool enabled = !(context.CurrentItemFlags & ImGuiItemFlags_Disabled);

    draw->AddRectFilled(start, end, ImGui::GetColorU32(palette.searchFill), radius);

    ImGui::PushID(label);
    ImGui::BeginGroup();
    ImGui::Dummy(ImVec2(width, height));
    const ImGuiID inputId = ImGui::GetID("##in");
    const ImGuiID lensId = ImGui::GetID("##SearchExecute");
    const ImGuiID clearId = ImGui::GetID("##SearchClear");
    const auto iconPressed = [&](const ImRect& bounds, ImGuiID id, bool& hovered) {
        hovered = ImGui::ItemHoverable(bounds, inputId, context.CurrentItemFlags
            | ImGuiItemFlags_AllowDuplicateId | ImGuiItemFlags_NoNavDisableMouseHover);
        if (!hovered) return false;
        ImGui::SetMouseCursor(ImGuiMouseCursor_Arrow);
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left, ImGuiInputFlags_None, id))
            ImGui::SetKeyOwner(ImGuiKey_MouseLeft, id, ImGuiInputFlags_LockUntilRelease);
        return ImGui::IsMouseReleased(ImGuiMouseButton_Left, id) && ImGui::GetKeyOwner(ImGuiKey_MouseLeft) == id;
    };
    bool lensHovered = false;
    bool lensPressed = false;
    bool clearHovered = false;
    if (enabled) {
        lensPressed = iconPressed(ImRect(start, ImVec2(textStart.x, end.y)), lensId, lensHovered);
        result.preciseToggled = options.allowPrecise && lensPressed;
        result.preciseHovered = options.allowPrecise && lensHovered;
        if (buffer[0] != '\0')
            result.cleared = iconPressed(ImRect(clearStart, end), clearId, clearHovered);
    }
    if (result.cleared) {
        buffer[0] = '\0';
        if (auto* textState = ImGui::GetInputTextState(inputId))
            textState->ReloadUserBufAndMoveToEnd();
    }

    ImGui::SetCursorScreenPos(textStart);
    const bool requestFocus = options.requestFocus || result.cleared || (lensPressed && !options.allowPrecise);
    if (enabled && requestFocus && context.ActiveId != inputId)
        ImGui::SetKeyboardFocusHere();
    auto& io = ImGui::GetIO();
    const bool mouseClicked = io.MouseClicked[ImGuiMouseButton_Left];
    const bool mouseDown = io.MouseDown[ImGuiMouseButton_Left];
    const ImGuiID mouseOwner = ImGui::GetKeyOwner(ImGuiKey_MouseLeft);
    const bool iconOwnsMouse = mouseOwner == clearId || mouseOwner == lensId;
    if (iconOwnsMouse) {
        io.MouseClicked[ImGuiMouseButton_Left] = false;
        io.MouseDown[ImGuiMouseButton_Left] = false;
    }
    const bool hideHint = requestFocus || context.ActiveId == inputId || context.NavId == inputId
        || (mouseClicked && ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(textStart, ImVec2(clearStart.x, end.y)));
    ImVec4 transparent = palette.searchFill;
    transparent.w = 0.0f;
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0.0f, (height - ImGui::GetFontSize()) * 0.5f));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, transparent);
    ImGui::PushStyleColor(ImGuiCol_NavCursor, transparent);
    ImGui::PushStyleColor(ImGuiCol_Text, palette.text);
    ImGui::PushStyleColor(ImGuiCol_TextDisabled, palette.hint);
    ImGui::PushStyleColor(ImGuiCol_TextSelectedBg, palette.selection);
    ImGui::PushStyleColor(ImGuiCol_InputTextCursor, palette.text);
    const float inputRight = !options.allowPrecise && !hideHint && buffer[0] == '\0'
        ? end.x - 10.0f : clearStart.x;
    result.submitted = ImGui::InputTextEx("##in", hideHint ? "" : placeholder, buffer, static_cast<int>(capacity),
        ImVec2(inputRight - textStart.x, height), ImGuiInputTextFlags_EnterReturnsTrue);
    result.active = enabled && ImGui::IsItemActive();
    result.focused = enabled && (ImGui::IsItemFocused() || result.active);
    ImGui::PopStyleColor(6);
    ImGui::PopStyleVar(2);
    if (iconOwnsMouse) {
        io.MouseClicked[ImGuiMouseButton_Left] = mouseClicked;
        io.MouseDown[ImGuiMouseButton_Left] = mouseDown;
    }
    ImGui::EndGroup();
    ImGui::PopID();

    if (result.focused) {
        constexpr float focusThickness = 1.6f;
        const ImVec2 clipMin = draw->GetClipRectMin();
        const ImVec2 clipMax = draw->GetClipRectMax();
        const float clearance = std::min({
            start.x - clipMin.x, start.y - clipMin.y, clipMax.x - end.x, clipMax.y - end.y});
        const float paintPadding = (focusThickness + draw->_FringeScale) * 0.5f;
        const float outset = std::min(1.0f, std::max(0.0f, clearance) - paintPadding);
        draw->AddRect(ImVec2(start.x - outset, start.y - outset), ImVec2(end.x + outset, end.y + outset),
            ImGui::GetColorU32(palette.focus), radius + outset, 0, focusThickness);
    }
    const auto iconSurface = [&](ImVec2 center, ImGuiID id, bool hovered) {
        if (hovered) {
            const ImVec4 fill = (mouseOwner == id) ? palette.iconActive : palette.iconHover;
            draw->AddCircleFilled(center, 9.0f, ImGui::GetColorU32(fill));
        }
    };
    const ImVec2 lens(start.x + textInset * 0.5f, centerY);
    iconSurface(lens, lensId, lensHovered);
    const bool preciseActive = options.allowPrecise && (options.precise != result.preciseToggled);
    const ImU32 searchInk = ImGui::GetColorU32(preciseActive ? palette.preciseIcon : palette.searchIcon);
    draw->AddCircle(ImVec2(lens.x, lens.y - 1.0f), 3.7f, searchInk, 0, 1.25f);
    draw->AddLine(ImVec2(lens.x + 2.5f, lens.y + 1.6f), ImVec2(lens.x + 5.0f, lens.y + 4.0f), searchInk, 1.25f);
    if (preciseActive)
        draw->AddLine(ImVec2(lens.x - 3.0f, lens.y + 6.0f), ImVec2(lens.x + 4.0f, lens.y + 6.0f), searchInk, 1.0f);

    if (buffer[0] != '\0') {
        const ImVec2 cross(end.x - kSearchClearWidth * 0.5f, centerY);
        iconSurface(cross, clearId, clearHovered);
        draw->AddCircleFilled(cross, 7.5f, ImGui::GetColorU32(palette.searchIcon));
        const ImU32 cut = ImGui::GetColorU32(palette.searchFill);
        constexpr float halfStrokeOnAxis = 0.53f;
        constexpr float inner = 3.0f - halfStrokeOnAxis;
        constexpr float outer = 3.0f + halfStrokeOnAxis;
        constexpr float notch = 2.0f * halfStrokeOnAxis;
        const ImVec2 outline[] = {
            {cross.x, cross.y - notch},
            {cross.x + inner, cross.y - outer}, {cross.x + outer, cross.y - inner},
            {cross.x + notch, cross.y},
            {cross.x + outer, cross.y + inner}, {cross.x + inner, cross.y + outer},
            {cross.x, cross.y + notch},
            {cross.x - inner, cross.y + outer}, {cross.x - outer, cross.y + inner},
            {cross.x - notch, cross.y},
            {cross.x - outer, cross.y - inner}, {cross.x - inner, cross.y - outer},
        };
        draw->AddConcavePolyFilled(outline, IM_ARRAYSIZE(outline), cut);
    }
    return result;
}

}
