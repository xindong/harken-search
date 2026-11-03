from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
MAIN = (ROOT / "src/main.cpp").read_text(encoding="utf-8")
APP_STATE = (ROOT / "src/AppState.h").read_text(encoding="utf-8")


def test_metadata_picker_loads_complete_name_lists_and_clips_visible_rows():
    metadata = MAIN.split("// Section 2: 元数据过滤", 1)[1].split("// Section 3: 播放列表", 1)[0]
    for field in ("Artists", "Albums", "Categories"):
        assert f"getUnique{field}(std::numeric_limits<int>::max())" in metadata
    assert "ImGuiListClipper" in MAIN.split("static void DrawSidebarMetadataPickerField", 1)[1].split(
        "// Renders [optional search field]", 1)[0]


def test_metadata_picker_does_not_apply_draft_or_group_artists():
    metadata = MAIN.split("// Section 2: 元数据过滤", 1)[1].split("// Section 3: 播放列表", 1)[0]
    widget = MAIN.split("static void DrawSidebarMetadataPickerField", 1)[1].split(
        "// Renders [optional search field]", 1)[0]
    assert "DrawSidebarFilterPopupBodyGrouped(" not in metadata
    assert "artistFilterAlternates = g.members" not in metadata
    assert "ui.draft.Edit(visible.data())" in widget
    assert "ui.draft.Select(applied)" in widget
    assert "ui.draft.Cancel()" in widget


def test_metadata_picker_can_apply_full_long_names_from_library():
    for field in ("artist", "album", "category"):
        assert f"char {field}FilterBuffer[1024]" in APP_STATE
    widget = MAIN.split("static void DrawSidebarMetadataPickerField", 1)[1].split(
        "// Renders [optional search field]", 1)[0]
    assert "std::vector<char> visible(appliedSize)" in widget
    assert "ImVec2(ImGui::GetContentRegionAvail().x, 0.0f)" in widget


def test_metadata_picker_is_one_scrollable_surface_without_inner_frame():
    widget = MAIN.split("static void DrawSidebarMetadataPickerField", 1)[1].split(
        "// Renders [optional search field]", 1)[0]
    assert 'BeginTransientScrollbarChild("##MetadataNameList", ImVec2(0.0f, 0.0f), false)' in widget
    assert "ImGuiWindowFlags_NoScrollbar" in widget
    assert "ImGui::Separator()" not in widget
    assert "HoveredForHelp() && !ui.draft.IsOpen()" in widget


def test_metadata_picker_input_uses_clean_display_label():
    widget = MAIN.split("static void DrawSidebarMetadataPickerField", 1)[1].split(
        "// Renders [optional search field]", 1)[0]
    assert "ui.draft.InputDisplayText(L(Filter_DamagedMetadataName))" in widget


def test_metadata_filter_chips_use_clean_display_labels():
    for field in ("category", "artist", "album"):
        assert f"PickerDisplayName(state.{field}FilterBuffer" in MAIN
