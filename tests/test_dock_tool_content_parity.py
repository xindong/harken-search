from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
MAIN = (ROOT / "src/main.cpp").read_text(encoding="utf-8")
RAIL = (ROOT / "src/SidebarActivityBar.cpp").read_text(encoding="utf-8")


def section(source, start, end):
    return source.split(start, 1)[1].split(end, 1)[0]


def test_filter_uses_the_same_body_style_and_rendering_in_both_hosts():
    floating = section(MAIN, "// Floating Sidebar with drag-to-dock", "// Floating File Info")
    docked = section(MAIN, 'if (leaf.id == "filter"', 'if (leaf.id == "info"')
    assert 'pushDockedToolAppearance("filter")' in floating
    assert 'pushDockedToolAppearance("filter")' in docked
    assert 'renderFloatingToolBody("filter", size, false, "FloatSidebar")' in floating
    assert 'renderDockedToolBody("filter", shell, size.y, false, "DockedFilter")' in docked


def test_tool_body_uses_inner_available_width_in_both_modes():
    docked = section(MAIN, "const auto renderDockedToolBody", "const auto renderFloatingToolBody")
    floating = section(MAIN, "const auto renderFloatingToolBody", "auto renderFunctionDockedModule")
    body = section(MAIN, "const auto renderDockToolBody", "const auto renderDockedToolBody")
    assert "renderDockToolBody(" in docked
    assert "renderDockToolBody(" in floating
    assert "beginDockContentViewport(" in body
    assert "renderToolContent(module, ImGui::GetContentRegionAvail().x, sidebarScope)" in body
    assert "renderFloatingToolBody(moduleId, size, false, nullptr)" in MAIN
    assert 'renderFloatingToolBody("info", size, false, nullptr)' in MAIN
    assert 'renderFloatingToolBody(selected, size, appearance.similarPerimeter, "GroupedFloatingSidebar")' in MAIN


def test_short_filter_rail_leaves_all_sections_and_the_menu_reachable():
    region = section(MAIN, 'BeginTransientScrollbarChild("##SidebarRail"', 'BeginTransientScrollbarChild("##SidebarContent"')
    assert "ImGuiWindowFlags_NoScrollbar" not in region
    assert "visibleSections * item_h" in RAIL
    assert "std::max(pos.y + sectionHeight," in RAIL


def test_folder_expansion_storage_does_not_depend_on_the_host_window():
    node = section(MAIN, "static void RenderFolderTreeNode", "// Helper: apply all active filters")
    tree = section(MAIN, "// Section 1: 文件夹树", "// Section 4: 标签云")
    assert "ImGui::SetNextItemStorageID(ImHashStr(folder.path.c_str(), 0, expansionSeed))" in node
    assert "true, locate, expansionSeed)" in node
    assert "ImGui::SetStateStorage(&folderExpansion)" in tree
    assert "ImGui::SetStateStorage(previousStorage)" in tree
    assert "folderExpansion.Clear()" in tree
