from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
MAIN = (ROOT / "src/main.cpp").read_text(encoding="utf-8")
SIMILAR = (ROOT / "src/SimilarSoundsWindow.cpp").read_text(encoding="utf-8")


def section(start, end):
    return MAIN.split(start, 1)[1].split(end, 1)[0]


def test_floating_tool_shell_consumes_the_docked_palette():
    floating = section("auto beginFilterStyleFloatingWindow", "auto renderFunctionDockedModule")
    docked = section("auto renderFunctionDockedModule", "// Left Column - Sidebar with title bar")
    assert "resolveDockToolPalette(moduleId)" in floating
    assert "resolveDockToolPalette(moduleId)" in docked
    assert "palette.bodyText" in floating
    assert "pushDockToolBodyStyle(palette)" in docked


def test_info_similar_and_docked_tab_groups_share_the_tool_palette():
    grouped = section("const auto pushDockedToolAppearance", "const auto popDockedToolAppearance")
    info = section('if (leaf.id == "info" && !state.panelFileInfo.floating',
                   'if (leaf.id == "similar-sounds" && !state.panelSimilarSounds.floating')
    similar = section('if (leaf.id == "similar-sounds" && !state.panelSimilarSounds.floating',
                      'renderFunctionDockedModule(leaf, "mod-ucs-browser"')
    assert "resolveDockToolPalette(module)" in grouped
    assert 'pushDockedToolAppearance("info")' in info
    assert 'resolveDockToolPalette("similar-sounds")' in similar


def test_dock_group_tabs_pair_the_title_ink_with_the_same_surface():
    strip = section("auto renderDockTabStrip", "auto renderDockTabsInShell")
    assert "resolveDockToolPalette(selected)" in strip
    assert "palette.titleText" in strip
    assert "ImGui::GetColorU32(palette.title)" in strip
    assert "TransportContrast::Ratio(ink, tint)" in strip


def test_detached_info_uses_shared_title_and_body_colors():
    detached = section("// FLOATING FILE INFO PANEL (when detached)",
                       "// FLOATING FOLDER BROWSER PANEL (when detached)")
    assert 'resolveDockToolPalette("info")' in detached
    assert "pushDockToolBodyStyle(palette)" in detached
    assert "ImGuiCol_TitleBgActive, palette.title" in detached


def test_custom_similarity_table_uses_ink_for_its_own_colors_not_the_tool_shell():
    table = SIMILAR.split("static void DrawSimilarSoundsResultsTable", 1)[1]
    assert "TransportContrast::ForSurface(theme.resultsText, tableHeader)" in table
    assert "TransportContrast::ForSurface(theme.resultsText, tableBody)" in table
    assert "TransportContrast::ForSurface(theme.resultsText, stripedBody)" in table


def test_similarity_reference_and_result_count_use_their_own_surfaces():
    actions = SIMILAR.split("static bool DrawSimilarSoundsActionRow", 1)[1].split(
        "static const char* SimilarSoundsBodyMessage", 1)[0]
    body = SIMILAR.split("static void DrawSimilarSoundsBody", 1)[1].split(
        "static void PollSimilarSoundsWorkflow", 1)[0]
    assert "TransportContrast::ForSurface(theme.metadataPanelText, actionSurface" in actions
    assert "TransportContrast::ForSurface(theme.resultsSecondaryText, headingSurface" in body
