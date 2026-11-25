from pathlib import Path
import re


MAIN = (Path(__file__).resolve().parents[1] / "src" / "main.cpp").read_text(
    encoding="utf-8"
)


def section(begin, end):
    return MAIN.split(begin, 1)[1].split(end, 1)[0]


def test_toolbar_uses_the_shared_widget_for_both_lucide_icons():
    toolbar = section("static void RenderFolderTreeExplorerToolbar(",
                      "static void RenderFolderTreeExplorerList(")
    assert toolbar.count("folder_tree::controls::DrawButton(") == 2
    assert "ICON_LC_REFRESH" in toolbar
    assert "ICON_LC_FOLDER_OPEN" in toolbar
    assert "ripple" not in toolbar.lower()


def test_database_locate_requires_a_selected_file_in_the_current_tree():
    toolbar = section("auto renderFolderTreeToolbar = [&]()",
                      "if (playlistScopedFolderTree) {\n            ImGui::Spacing();")
    assert "state.currentFileId" not in toolbar
    assert "state.selectedResultIndex" in toolbar
    assert "FindFolderTreeLocateTarget" in toolbar
    assert "folderSearchHits" in toolbar


def test_database_scroll_only_flashes_a_requested_row():
    tree = section("static void RenderFolderTreeNode(",
                   "void FilterResultsByFileType(")
    scroll = tree.split("ImGui::SetScrollHereY(0.5f);", 1)[1]
    assert "MarkScrolled(folder.path" in scroll
    assert "folderTreeHighlightUntilMs" not in tree


def test_browser_auto_scroll_does_not_start_a_flash():
    search = section("static void RenderBrowserSystemFolderSearchNode(",
                     "static void RenderBrowserSystemFolderSearchTree(")
    normal = section("static void RenderBrowserSystemFolderNode(",
                     "static void RenderBrowserSystemFolderTree(")
    navigation = section("static void LocateBrowserSystemFolderPath(",
                         "static void RenderBrowserSystemFolderContextMenu(")
    assert "MarkScrolled(canonicalFolder" in normal
    assert "ImGui::ScrollToItem()" in search
    assert "MarkScrolled(" not in search
    assert ".Request(" not in navigation


def test_browser_locate_disables_paths_outside_the_system_tree():
    panel = section("static void RenderBrowserFolderTreeSidebarPanel(",
                    "static void ClearSpecificCoverKeyFilter(")
    assert "BrowserSystemFolderRoots()" in panel
    assert "ContainsPath(" in panel


def test_sidebar_render_instances_own_independent_folder_tree_controls():
    renderer = section("static void RenderSidebarPanelContent(", "if (state.viewMode == ViewMode::Browser) {")
    assert "static aura::folder_tree::controls::Interaction controls;" not in renderer
    assert "controls = instance->controls;" in renderer
    assert 'ImGui::PushID(instance->imguiScope);' in renderer

    slots = dict(re.findall(r'\{"(\w+)", "(\w+)", \{\}\}', renderer))
    assert slots == {
        "LeftSidebar": "LeftSidebar",
        "RightSidebar": "LeftSidebar",
        "GroupedDockedSidebar": "LeftSidebar",
        "GroupedFloatingSidebar": "FloatSidebar",
        "FloatSidebar": "FloatSidebar",
        "DockedFilter": "DockedFilter",
        "DetachedSidebar": "DetachedSidebar",
    }
    assert 'RenderSidebarPanelContent(state, sidebarScope);' in MAIN
    assert 'renderToolContent(module, ImGui::GetContentRegionAvail().x, sidebarScope);' in MAIN
    docked = re.findall(r'renderDockedToolBody\([^;\n]+, "(\w+)"\);', MAIN)
    floating = re.findall(r'renderFloatingToolBody\([^;\n]+, "(\w+)"\);', MAIN)
    standalone = re.findall(r'RenderSidebarPanelContent\(state, "(\w+)"\);', MAIN)
    assert sorted(docked + floating + standalone) == sorted(slots)

    sidebar_tree = section("// Section 1: 文件夹树", "// Section 2:")
    assert "controls.BeginFrame(ImGui::GetFrameCount(), controlsPanel, state.currentDbPathUtf8, tabId, -1, true);" in sidebar_tree
    assert re.search(r"controls\.BeginFrame\([^;]*state\.currentDbPathUtf8, tabId,\s*effectivePlaylistFolderTreeId, false\)", sidebar_tree)


def test_existing_metadata_picker_keys_keep_the_original_sidebar_scope():
    assert "const char* metadataScope = instance->imguiScope;" in MAIN
    for picker in ("Artist", "Album", "Category"):
        assert f"{picker.lower()}Pickers[metadataScope]" in MAIN
        assert f'metadataScope, "{picker}", "##{picker}Filter"' in MAIN
