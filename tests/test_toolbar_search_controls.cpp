#include "ui/ToolbarSearchControls.h"

#include <cstdio>
#include <cstring>

namespace {

namespace toolbar = harken::toolbar;
constexpr ImVec2 searchPos(65.0f, 40.0f);
constexpr float width = 220.0f;

struct Frame {
    toolbar::SearchFieldResult search;
    ImRect searchBounds;
    bool nativeInputActive;
    bool themedFill;
    ImRect clearInk{FLT_MAX, FLT_MAX, -FLT_MAX, -FLT_MAX};
    ImRect crossInk{FLT_MAX, FLT_MAX, -FLT_MAX, -FLT_MAX};
    ImRect focusInk{FLT_MAX, FLT_MAX, -FLT_MAX, -FLT_MAX};
    ImRect modeInk{FLT_MAX, FLT_MAX, -FLT_MAX, -FLT_MAX};
    ImRect queryInk{FLT_MAX, FLT_MAX, -FLT_MAX, -FLT_MAX};
    float modeSlotWidth;
    float scrollX;
    ImU32 modeColor;
    int modeShapeVertices;
    int focusVertices;
};

struct Fixture {
    ThemeColorScheme theme;
    char query[128]{};
    bool requestFocus = false;
    bool allowPrecise = true;
    bool clipToSearchBounds = false;
    bool precise = false;
    bool showModeLabel = false;
    bool realtime = true;
    float requestedWidth = width;

    Fixture()
    {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        auto& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.LogFilename = nullptr;
        io.DisplaySize = ImVec2(480.0f, 210.0f);
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        unsigned char* pixels;
        int atlasWidth, atlasHeight;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &atlasWidth, &atlasHeight);
        io.Fonts->SetTexID(1);
    }

    ~Fixture() { ImGui::DestroyContext(); }

    toolbar::Palette SearchPalette() const
    {
        return toolbar::ResolveSearchPalette(theme, theme.toolbarBG, theme.frameBgBase, theme.interfaceText);
    }

    Frame Draw()
    {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
        ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
        ImGui::Begin("Toolbar control test", nullptr, ImGuiWindowFlags_NoTitleBar);
        ImGui::PushID("##SearchBar");
        const ImGuiID nativeId = ImGui::GetID("##in");
        ImGui::PopID();
        const auto palette = SearchPalette();
        const float fieldWidth = showModeLabel ? toolbar::SearchWidthWithModeLabel(requestedWidth) : requestedWidth;
        const auto modeLabel = showModeLabel
            ? (realtime ? toolbar::SearchModeLabel::Live : toolbar::SearchModeLabel::Enter)
            : toolbar::SearchModeLabel::None;
        ImGui::SetCursorScreenPos(searchPos);
        auto* drawList = ImGui::GetWindowDrawList();
        if (clipToSearchBounds)
            drawList->PushClipRect(searchPos, ImVec2(searchPos.x + fieldWidth,
                searchPos.y + toolbar::SearchFieldHeight()), true);
        const int firstVertex = drawList->VtxBuffer.Size;
        Frame frame{};
        frame.search = toolbar::DrawSearchField("##SearchBar", "Search", query, sizeof(query),
            fieldWidth, palette, toolbar::SearchFieldOptions{allowPrecise, precise, requestFocus, 0.0f, modeLabel});
        requestFocus = false;
        if (frame.search.preciseToggled) precise = !precise;
        frame.searchBounds = ImRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
        frame.nativeInputActive = ImGui::GetActiveID() == nativeId;
        if (const auto* textState = ImGui::GetInputTextState(nativeId))
            frame.scrollX = textState->Scroll.x;
        const float modeFontSize = ImGui::GetFontSize() * 0.84f;
        const float liveWidth = ImGui::GetFont()->CalcTextSizeA(modeFontSize, FLT_MAX, 0.0f, "live").x;
        const float enterWidth = ImGui::GetFont()->CalcTextSizeA(modeFontSize, FLT_MAX, 0.0f, "enter").x;
        frame.modeSlotWidth = std::ceil(std::max(liveWidth, enterWidth)) + 8.0f;
        const float clearLeft = frame.searchBounds.Max.x - toolbar::kSearchClearWidth;
        const ImVec2 whiteUv = ImGui::GetFontTexUvWhitePixel();
        const ImVec2 clearCenter(frame.searchBounds.Max.x - toolbar::kSearchClearWidth * 0.5f,
            frame.searchBounds.GetCenter().y);
        for (int index = firstVertex; index < drawList->VtxBuffer.Size; ++index) {
            const auto& vertex = drawList->VtxBuffer[index];
            frame.themedFill |= vertex.col == ImGui::GetColorU32(palette.searchFill);
            if (query[0] && vertex.col == ImGui::GetColorU32(palette.searchIcon)
                && vertex.pos.x > clearLeft)
                frame.clearInk.Add(vertex.pos);
            if (query[0] && vertex.col == ImGui::GetColorU32(palette.searchFill)
                && std::abs(vertex.pos.x - clearCenter.x) < 5.25f
                && std::abs(vertex.pos.y - clearCenter.y) < 5.25f)
                frame.crossInk.Add(vertex.pos);
            if (vertex.col == ImGui::GetColorU32(palette.focus)) {
                frame.focusInk.Add(vertex.pos);
                if (vertex.pos.y < searchPos.y)
                    ++frame.focusVertices;
            }
            const bool isGlyph = vertex.uv.x != whiteUv.x || vertex.uv.y != whiteUv.y;
            const bool inModeSlot = vertex.pos.x > clearLeft - frame.modeSlotWidth && vertex.pos.x < clearLeft;
            if (!isGlyph && inModeSlot)
                ++frame.modeShapeVertices;
            if (isGlyph && query[0] && vertex.col == ImGui::GetColorU32(palette.text))
                frame.queryInk.Add(vertex.pos);
            if (isGlyph && inModeSlot && query[0] && vertex.col == ImGui::GetColorU32(palette.hint)) {
                frame.modeInk.Add(vertex.pos);
                frame.modeColor = vertex.col;
            }
        }
        if (clipToSearchBounds)
            drawList->PopClipRect();
        ImGui::SetCursorScreenPos(ImVec2(searchPos.x, searchPos.y + 60.0f));
        ImGui::Button("Other control");
        ImGui::End();
        ImGui::Render();
        return frame;
    }

    Frame Mouse(float x, float y, bool down)
    {
        ImGui::GetIO().AddMousePosEvent(x, y);
        ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, down);
        return Draw();
    }
};

#define CHECK(condition) do { if (!(condition)) { \
    std::fprintf(stderr, "line %d: %s\n", __LINE__, #condition); return false; \
} } while (false)

bool SearchEditsSubmitAndClear()
{
    Fixture fixture;
    const Frame idle = fixture.Draw();
    CHECK(idle.themedFill && idle.focusVertices == 0);
    CHECK(idle.searchBounds.Min.x == searchPos.x && idle.searchBounds.GetWidth() == width);
    fixture.requestFocus = true;
    fixture.Draw();
    const Frame focused = fixture.Draw();
    CHECK(focused.search.focused && focused.nativeInputActive && focused.focusVertices > 0);
    CHECK(fixture.Draw().focusVertices == focused.focusVertices);

    ImGui::GetIO().AddInputCharactersUTF8("bass");
    CHECK(!fixture.Draw().search.submitted && std::strcmp(fixture.query, "bass") == 0);
    const float clearX = searchPos.x + width - 12.0f;
    CHECK(!fixture.Mouse(clearX, searchPos.y + 13.0f, true).search.cleared);
    const Frame cleared = fixture.Mouse(clearX, searchPos.y + 13.0f, false);
    CHECK(cleared.search.cleared && !cleared.search.submitted && !cleared.search.preciseToggled);
    CHECK(cleared.nativeInputActive && fixture.query[0] == '\0');
    ImGui::GetIO().AddInputCharactersUTF8("kick");
    CHECK(!fixture.Draw().search.submitted && std::strcmp(fixture.query, "kick") == 0);
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter, true);
    CHECK(fixture.Draw().search.submitted);
    CHECK(std::strcmp(fixture.query, "kick") == 0);
    const float preciseX = searchPos.x + 14.0f;
    CHECK(!fixture.Mouse(preciseX, searchPos.y + 13.0f, true).search.preciseToggled);
    CHECK(fixture.Mouse(preciseX, searchPos.y + 13.0f, false).search.preciseToggled);
    CHECK(fixture.precise && std::strcmp(fixture.query, "kick") == 0);
    return true;
}

bool SearchLegibility()
{
    Fixture fixture;
    fixture.theme.toolbarBG = ImVec4(0.08f, 0.11f, 0.15f, 1.0f);
    fixture.theme.frameBgBase = ImVec4(0.09f, 0.12f, 0.16f, 1.0f);
    const auto dark = fixture.SearchPalette();
    CHECK(TransportContrast::Ratio(dark.searchFill, fixture.theme.toolbarBG) >= 1.49);
    CHECK(TransportContrast::Ratio(dark.text, dark.searchFill) >= 4.45);
    CHECK(TransportContrast::Ratio(dark.hint, dark.searchFill) >= 4.45);
    CHECK(TransportContrast::Ratio(dark.searchIcon, dark.searchFill) >= 4.45);
    fixture.theme.toolbarBG = ImVec4(0.98f, 0.93f, 0.83f, 1.0f);
    fixture.theme.frameBgBase = ImVec4(1.0f, 0.98f, 0.94f, 1.0f);
    CHECK(fixture.SearchPalette().searchFill.x == fixture.theme.frameBgBase.x);
    std::strcpy(fixture.query, "rain");
    const auto frame = fixture.Draw();
    CHECK(frame.clearInk.GetWidth() >= 13.0f && frame.clearInk.GetHeight() >= 13.0f);
    CHECK(frame.searchBounds.Max.x - frame.clearInk.Max.x >= 4.0f);
    CHECK(frame.clearInk.Min.y - frame.searchBounds.Min.y >= 4.0f);
    CHECK(frame.searchBounds.Max.y - frame.clearInk.Max.y >= 4.0f);
    CHECK(frame.crossInk.GetWidth() > 5.0f && frame.crossInk.GetHeight() > 5.0f);
    const ImVec2 clearCenter = frame.clearInk.GetCenter();
    const ImVec2 crossCenter = frame.crossInk.GetCenter();
    CHECK(std::abs(clearCenter.x - crossCenter.x) < 0.2f);
    CHECK(std::abs(clearCenter.y - crossCenter.y) < 0.2f);
    return true;
}

bool FilterLensFocusesWithoutChangingSearchMode()
{
    Fixture fixture;
    fixture.allowPrecise = false;
    fixture.Draw();
    const float lensX = searchPos.x + toolbar::kSearchTextOffset * 0.5f;
    const float middleY = searchPos.y + 13.0f;
    CHECK(!fixture.Mouse(lensX, middleY, true).search.preciseToggled);
    CHECK(!fixture.Mouse(lensX, middleY, false).search.preciseToggled);
    CHECK(fixture.Draw().nativeInputActive);
    ImGui::GetIO().AddInputCharactersUTF8("folder");
    CHECK(!fixture.Draw().search.preciseToggled && std::strcmp(fixture.query, "folder") == 0);
    const float clearX = searchPos.x + width - toolbar::kSearchClearWidth * 0.5f;
    CHECK(!fixture.Mouse(clearX, middleY, true).search.cleared);
    CHECK(fixture.Mouse(clearX, middleY, false).search.cleared);
    CHECK(fixture.query[0] == '\0');
    return true;
}

bool FocusRingFitsClippedField()
{
    Fixture fixture;
    fixture.allowPrecise = false;
    fixture.clipToSearchBounds = true;
    fixture.requestFocus = true;
    fixture.Draw();
    const auto focused = fixture.Draw();
    CHECK(focused.search.focused);
    CHECK(focused.focusInk.GetWidth() > width - 6.0f);
    CHECK(focused.focusInk.GetHeight() > focused.searchBounds.GetHeight() - 6.0f);
    CHECK(focused.focusInk.Min.x >= focused.searchBounds.Min.x);
    CHECK(focused.focusInk.Min.y >= focused.searchBounds.Min.y);
    CHECK(focused.focusInk.Max.x <= focused.searchBounds.Max.x);
    CHECK(focused.focusInk.Max.y <= focused.searchBounds.Max.y);
    return true;
}

bool NarrowModeLabelKeepsQueryAndClearSeparate()
{
    Fixture fixture;
    fixture.showModeLabel = true;
    fixture.requestedWidth = 102.0f;
    ImGui::GetIO().DisplaySize.x = 850.0f;
    std::memset(fixture.query, 'm', sizeof(fixture.query) - 1);
    fixture.requestFocus = true;
    fixture.Draw();
    fixture.Draw();
    ImGui::GetIO().AddKeyEvent(ImGuiKey_End, true);
    const auto live = fixture.Draw();
    ImGui::GetIO().AddKeyEvent(ImGuiKey_End, false);
    CHECK(live.nativeInputActive);
    const float clearLeft = live.searchBounds.Max.x - toolbar::kSearchClearWidth;
    const float editorRight = clearLeft - live.modeSlotWidth;
    const float editorWidth = editorRight - live.searchBounds.Min.x - toolbar::kSearchTextOffset;
    CHECK(editorWidth >= 80.0f && editorWidth <= 90.0f);
    CHECK(live.scrollX > 0.0f && live.queryInk.GetWidth() >= 60.0f);
    CHECK(live.queryInk.Max.x <= editorRight && live.modeInk.Min.x > editorRight);
    CHECK(live.modeInk.GetWidth() > 0.0f && live.modeInk.Max.x < clearLeft);
    CHECK(live.clearInk.Min.x > clearLeft);
    const float centerY = live.searchBounds.GetCenter().y;
    fixture.Draw();
    ImGui::GetIO().AddMousePosEvent(clearLeft - 1.0f, centerY);
    fixture.Draw();
    CHECK(!fixture.Mouse(clearLeft - 1.0f, centerY, true).search.cleared);
    CHECK(ImGui::GetIO().MouseDown[ImGuiMouseButton_Left]);
    CHECK(!fixture.Mouse(clearLeft - 1.0f, centerY, false).search.cleared);
    CHECK(std::strlen(fixture.query) == sizeof(fixture.query) - 1);

    ImGui::GetIO().AddMousePosEvent(clearLeft + 1.0f, centerY);
    fixture.Draw();
    CHECK(!fixture.Mouse(clearLeft + 1.0f, centerY, true).search.cleared);
    CHECK(ImGui::GetIO().MouseDown[ImGuiMouseButton_Left] && ImGui::GetIO().MousePos.x > clearLeft);
    const auto cleared = fixture.Mouse(clearLeft + 1.0f, centerY, false);
    CHECK(cleared.search.cleared && fixture.query[0] == '\0');
    CHECK(fixture.Draw().nativeInputActive);
    std::printf("850px geometry: pill %.0f, editor %.0f, label slot %.0f, clear %.0f, scroll %.0f\n",
        live.searchBounds.GetWidth(), editorWidth, live.modeSlotWidth, toolbar::kSearchClearWidth, live.scrollX);
    return true;
}

bool ModeLabelsShareAlignmentAndFollowFocusAndTheme()
{
    Fixture fixture;
    std::strcpy(fixture.query, "rain");
    fixture.requestFocus = true;
    fixture.Draw();
    const auto withoutLabel = fixture.Draw();
    CHECK(withoutLabel.nativeInputActive && withoutLabel.modeInk.IsInverted());
    fixture.showModeLabel = true;
    const auto live = fixture.Draw();
    CHECK(live.modeInk.GetWidth() > 0.0f && live.modeShapeVertices == withoutLabel.modeShapeVertices);
    CHECK(live.queryInk.Min.x == withoutLabel.queryInk.Min.x);
    CHECK(live.clearInk.GetCenter().x == withoutLabel.clearInk.GetCenter().x);
    CHECK(live.clearInk.GetCenter().y == withoutLabel.clearInk.GetCenter().y);
    ImGui::GetIO().AddInputCharactersUTF8("wind");
    CHECK(!fixture.Draw().search.submitted && std::strstr(fixture.query, "wind"));

    fixture.realtime = false;
    const auto enter = fixture.Draw();
    CHECK(enter.modeInk.GetWidth() > live.modeInk.GetWidth());
    CHECK(std::abs(enter.modeInk.Max.x - live.modeInk.Max.x) <= 1.0f);
    CHECK(enter.clearInk.GetCenter().x == live.clearInk.GetCenter().x);
    fixture.theme.toolbarBG = ImVec4(0.97f, 0.94f, 0.89f, 1.0f);
    fixture.theme.frameBgBase = ImVec4(0.99f, 0.96f, 0.91f, 1.0f);
    fixture.theme.interfaceText = ImVec4(0.18f, 0.09f, 0.24f, 1.0f);
    const auto light = fixture.Draw();
    CHECK(light.modeInk.GetWidth() > 0.0f && light.modeColor != enter.modeColor);
    const auto lightPalette = fixture.SearchPalette();
    CHECK(TransportContrast::Ratio(ImGui::ColorConvertU32ToFloat4(light.modeColor), lightPalette.searchFill) >= 4.45);
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter, true);
    CHECK(fixture.Draw().search.submitted);
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter, false);
    fixture.Draw();
    fixture.Mouse(searchPos.x + 12.0f, searchPos.y + 64.0f, true);
    fixture.Mouse(searchPos.x + 12.0f, searchPos.y + 64.0f, false);
    const auto unfocused = fixture.Draw();
    CHECK(!unfocused.search.focused && unfocused.modeInk.IsInverted());
    CHECK(unfocused.searchBounds.GetWidth() == light.searchBounds.GetWidth());
    return true;
}

}

int main()
{
    const struct {
        const char* name;
        bool (*run)();
    } tests[] = {
        {"native editing, Enter, precise toggle and focus after clear", SearchEditsSubmitAndClear},
        {"dark and light palette plus visible centered clear glyph", SearchLegibility},
        {"filter lens focuses without switching to precise search", FilterLensFocusesWithoutChangingSearchMode},
        {"focus ring remains inside a clipped field", FocusRingFitsClippedField},
        {"850px mode label preserves editing and clear hit areas", NarrowModeLabelKeepsQueryAndClearSeparate},
        {"mode labels align and follow focus and theme", ModeLabelsShareAlignmentAndFollowFocusAndTheme},
    };
    int failures = 0;
    for (const auto& test : tests) {
        const bool passed = test.run();
        std::printf("%s %s\n", passed ? "PASS" : "FAIL", test.name);
        if (!passed) ++failures;
    }
    return failures == 0 ? 0 : 1;
}
