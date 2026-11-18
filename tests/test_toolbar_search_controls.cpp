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
    int focusVertices;
};

struct Fixture {
    ThemeColorScheme theme;
    char query[128]{};
    bool requestFocus = false;
    bool allowPrecise = true;
    bool clipToSearchBounds = false;
    bool precise = false;

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
        ImGui::SetCursorScreenPos(searchPos);
        auto* drawList = ImGui::GetWindowDrawList();
        if (clipToSearchBounds)
            drawList->PushClipRect(searchPos, ImVec2(searchPos.x + width,
                searchPos.y + toolbar::SearchFieldHeight()), true);
        const int firstVertex = drawList->VtxBuffer.Size;
        Frame frame{};
        frame.search = toolbar::DrawSearchField("##SearchBar", "Search", query, sizeof(query),
            width, palette, toolbar::SearchFieldOptions{allowPrecise, precise, requestFocus});
        requestFocus = false;
        if (frame.search.preciseToggled) precise = !precise;
        frame.searchBounds = ImRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
        frame.nativeInputActive = ImGui::GetActiveID() == nativeId;
        const ImVec2 clearCenter(frame.searchBounds.Max.x - toolbar::kSearchClearWidth * 0.5f,
            frame.searchBounds.GetCenter().y);
        for (int index = firstVertex; index < drawList->VtxBuffer.Size; ++index) {
            const auto& vertex = drawList->VtxBuffer[index];
            frame.themedFill |= vertex.col == ImGui::GetColorU32(palette.searchFill);
            if (query[0] && vertex.col == ImGui::GetColorU32(palette.searchIcon)
                && vertex.pos.x > searchPos.x + width - toolbar::kSearchClearWidth)
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
        }
        if (clipToSearchBounds)
            drawList->PopClipRect();
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
    };
    int failures = 0;
    for (const auto& test : tests) {
        const bool passed = test.run();
        std::printf("%s %s\n", passed ? "PASS" : "FAIL", test.name);
        if (!passed) ++failures;
    }
    return failures == 0 ? 0 : 1;
}
