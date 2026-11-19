from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[1]
STYLE_HEADER = ROOT / "src" / "ui" / "CoreWorkspaceStyle.h"
STYLE_SOURCE = ROOT / "src" / "ui" / "CoreWorkspaceStyle.cpp"
SHELL_HEADER = ROOT / "src" / "ui" / "ModuleWindowShell.h"
SHELL_SOURCE = ROOT / "src" / "ui" / "ModuleWindowShell.cpp"
CPP_TEST = ROOT / "tests" / "test_core_workspace_style.cpp"
CMAKE = ROOT / "CMakeLists.txt"
BEHAVIOR = ROOT / "cmake" / "harken_search_behavior_tests.cmake"
MAIN_CPP = ROOT / "src" / "main.cpp"
THEME_CPP = ROOT / "src" / "Theme.cpp"
BUILTIN_THEME_CPP = ROOT / "src" / "BuiltinThemeScheme.cpp"
FILE_INFO_CPP = ROOT / "src" / "FileInfoPanel.cpp"
PLAYLIST_QUICK_BAR_CPP = ROOT / "src" / "PlaylistQuickBar.cpp"
SCROLLBAR_HOST_CPP = ROOT / "src" / "TransientScrollbarHost.cpp"
AURO_WIDGETS_HEADER = ROOT / "src" / "HarkenSearchWidgets.h"
AURO_WIDGETS_SOURCE = ROOT / "src" / "HarkenSearchWidgets.cpp"
RUNTIME_CHROME = ROOT / "src" / "webui" / "src" / "components" / "runtime-twin" / "RuntimeChrome.tsx"
RUNTIME_TWIN_CSS = ROOT / "src" / "webui" / "src" / "components" / "runtime-twin" / "runtime-twin.css"


def read(path: Path) -> str:
    assert path.is_file(), f"required core-workspace file is missing: {path.relative_to(ROOT)}"
    return path.read_text(encoding="utf-8")


def between(text: str, start: str, end: str) -> str:
    start_index = text.index(start)
    end_index = text.index(end, start_index)
    return text[start_index:end_index]


def test_native_workspace_units_exist_and_are_registered_once():
    for path in (STYLE_HEADER, STYLE_SOURCE, SHELL_HEADER, SHELL_SOURCE, CPP_TEST):
        read(path)

    cmake = read(CMAKE)
    behavior = read(BEHAVIOR)
    app_sources = between(cmake, "set(APP_SOURCES", "# Platform abstraction layer")
    assert app_sources.count("src/ui/CoreWorkspaceStyle.cpp") == 1
    assert cmake.count("src/ui/ModuleWindowShell.cpp") == 1
    assert behavior.count("add_executable(TestCoreWorkspaceStyle ") == 1
    assert behavior.count("tests/test_core_workspace_style.cpp") == 1
    core_style_test = between(
        behavior,
        "add_executable(TestCoreWorkspaceStyle ",
        "add_test(NAME TestCoreWorkspaceStyle",
    )
    theme_test = between(
        behavior,
        "add_executable(TestThemeColors",
        "add_test(NAME TestThemeColors",
    )
    assert core_style_test.count("src/ui/CoreWorkspaceStyle.cpp") == 1
    assert theme_test.count("src/ui/CoreWorkspaceStyle.cpp") == 1
    assert behavior.count("src/ui/ModuleWindowShell.cpp") == 1
    assert behavior.count("NAME TestCoreWorkspaceStyleBoundary") == 1


def test_palette_is_supplied_without_a_second_global_theme_system():
    header = read(STYLE_HEADER)
    source = read(STYLE_SOURCE)
    combined = header + source

    assert "struct CoreWorkspacePaletteInput" in header
    assert "BuildCoreWorkspacePalette(const CoreWorkspacePaletteInput&" in header
    assert "functionalAccent" in combined
    assert "ThemeColors::current" not in combined
    assert "static CoreWorkspacePalette" not in combined
    assert "singleton" not in combined.lower()
    assert "registry" not in combined.lower()
    assert "AppState" not in combined
    assert "AudioEngine" not in combined
    assert "WebView2" not in combined
    assert "JSON" not in combined
    assert "IM_COL32" not in combined


def test_style_scope_is_raii_and_balances_only_its_own_pushes():
    header = read(STYLE_HEADER)
    source = read(STYLE_SOURCE)

    assert "class CoreWorkspaceStyleScope" in header
    assert "~CoreWorkspaceStyleScope" in header
    assert "CoreWorkspaceStyleScope(const CoreWorkspaceStyleScope&) = delete" in header
    assert "void reset()" in header
    assert "m_colorCount" in header and "m_varCount" in header
    assert "ImGui::PushStyleColor" in source
    assert "ImGui::PushStyleVar" in source
    assert "ImGui::PopStyleColor(m_colorCount)" in source
    assert "ImGui::PopStyleVar(m_varCount)" in source


def test_module_shell_is_visual_foundation_not_docking_engine():
    header = read(SHELL_HEADER)
    source = read(SHELL_SOURCE)
    combined = header + source

    for name in (
        "ModuleWindowMode",
        "Docked",
        "Floating",
        "Detached",
        "ModuleShellActivity",
        "Inactive",
        "Active",
        "Focused",
        "ModuleShellContentState",
        "Empty",
        "Loading",
        "Error",
        "Unavailable",
        "ModuleWindowShellScope",
    ):
        assert name in combined

    assert "CoreWorkspacePalette" in header
    assert "ImGui::BeginChild" in source
    assert "ImGui::EndChild" in source
    assert "DockBuilder" not in combined
    assert "ImGuiDockNode" not in combined
    assert "SaveIniSettings" not in combined
    assert "AppState" not in combined
    assert "AudioEngine" not in combined
    assert "Filter" not in combined
    assert "InfoPanel" not in combined


def test_foundation_keeps_one_line_and_compact_rhythm_in_cpp_contract():
    header = read(STYLE_HEADER)
    cpp_test = read(CPP_TEST)
    assert '#include "HarkenSearchMetrics.h"' in header
    for value in ("4.0f", "8.0f", "12.0f", "16.0f"):
        assert value in cpp_test
    assert "perimeter is one pixel" in cpp_test
    assert "splitter line is one pixel" in cpp_test
    assert "exactly three surface levels" in cpp_test
    assert "balances color stack" in cpp_test
    assert "balances BeginChild/EndChild" in cpp_test


def test_shipping_imgui_composes_the_workspace_palette_from_current_theme_tokens():
    main = read(MAIN_CPP)

    assert '#include "ui/CoreWorkspaceStyle.h"' in main
    assert '#include "ui/ModuleWindowShell.h"' in main
    assert "CoreWorkspacePaletteInput workspacePaletteInput" in main
    assert "BuildCoreWorkspacePalette(workspacePaletteInput)" in main
    assert "WorkspaceSurfaceBasis workspaceSurfaceBasis" in main
    assert "ResolveWorkspaceSurfaceLevels(workspaceSurfaceBasis)" in main
    for mapping in (
        "application = kCurrentTheme.mainWindowBG",
        "frame = kCurrentTheme.frameBgBase",
        "toolbar = kCurrentTheme.toolbarBG",
        "elevated = kCurrentTheme.dialogPanelBG",
        "lightChrome = kWorkspaceLightTheme",
        "applicationSurface = workspaceSurfaceLevels.application",
        "dockedSurface = workspaceSurfaceLevels.docked",
        "elevatedSurface = kUsePreWorkspaceSemanticColors",
        "perimeter = kCurrentTheme.chromeBorder",
        "splitter = kCurrentTheme.waveformSplitterLine",
        "primaryText = kCurrentTheme.interfaceText",
        "mutedText = kCurrentTheme.resultsSecondaryText",
        "functionalAccent = kCurrentTheme.controlAccent",
        "error = kCurrentTheme.wwiseConnectFail",
    ):
        assert mapping in main


def test_only_four_non_dark_builtins_restore_pre_workspace_semantic_surfaces():
    main = read(MAIN_CPP)
    composition = between(main, "const ThemeColorScheme& kCurrentTheme", "// Non-soft-chrome fallback")
    title_bar = between(main, "auto drawDockTitleBar", "struct DockModuleShellLayout")
    module_shell = between(main, "auto drawDockModuleShell", "// The Info title actions belong to the module shell")
    toolbar = between(main, 'BeginTransientScrollbarChild("Toolbar"', "// MAIN CONTENT AREA")
    waveform = between(main, 'BeginTransientScrollbarChild("WaveformPanel"', "// FLOATING PANELS")

    # The selective rollback is one policy branch: the four user-approved
    # built-ins return to the semantic tokens used immediately before 9830285.
    # Dark and custom themes keep the current depth resolver.
    for theme in ("Ocean", "LapisBlue", "JadeMist", "RoseCedar"):
        assert f"AppTheme::{theme}" in composition
    assert "AppTheme::Dark" not in composition.split("kUsePreWorkspaceSemanticColors", 1)[1].split(";", 1)[0]
    assert "workspacePaletteInput.elevatedSurface = kUsePreWorkspaceSemanticColors" in composition
    assert "kCurrentTheme.dialogPanelBG" in composition
    assert "workspaceToolbarSurface" in composition
    assert "kCurrentTheme.toolbarBG" in composition
    assert "workspaceDockTitleSurface" in composition
    assert "kLegacyDockTitleSurface" in composition

    # Current structure stays in place while only its surface inputs switch.
    assert "workspaceDockTitleSurface" in title_bar
    assert "kLegacyDockLaneSurface" in module_shell
    assert "kLegacyDockShellSurface" in module_shell
    assert "kUsePreWorkspaceSemanticColors" in module_shell
    assert "workspaceToolbarSurface" in toolbar
    assert "kLegacyWaveShellSurface" in waveform
    assert "ThemeColors::waveformPanelBG(waveformShellTheme)" in waveform
    assert "kLegacyTransportSurface" in waveform
    assert "tcsTransport.transportBarBG" in waveform

    # Splitter ownership and interaction colors are deliberately not rolled back.
    assert "workspacePalette.splitter" in module_shell
    assert "workspacePalette.functionalAccent" in main


def test_docked_panels_begin_below_the_rendered_toolbar():
    main = read(MAIN_CPP)
    toolbar = between(main, 'BeginTransientScrollbarChild("Toolbar"', "// MAIN CONTENT AREA")
    assert "toolbarBottomY = ImGui::GetWindowPos().y + ImGui::GetWindowSize().y" in toolbar
    assert "dockColumnTop = toolbarBottomY" in main
    for host in ("LeftDockColumn", "RightDockColumn"):
        child = between(main, f'"{host}",', ");")
        assert "ImGuiWindowFlags_NoBackground" in child


def test_shipping_dock_shell_has_one_perimeter_owner_and_no_decorative_layers():
    main = read(MAIN_CPP)
    title_bar = between(main, "auto drawDockTitleBar", "struct DockModuleShellLayout")
    module_shell = between(main, "auto drawDockModuleShell", "// The Info title actions belong to the module shell")

    assert "workspaceDockTitleSurface" in title_bar
    assert "AddRectFilledMultiColor" not in title_bar
    assert "AddLine" not in title_bar

    assert module_shell.count("dl->AddRect(") == 1
    assert module_shell.count("dl->AddLine(") == 1
    assert "kCoreWorkspaceMetrics.perimeterWidth" in module_shell
    assert "workspacePalette.splitter" in module_shell
    assert "ResolveModuleWindowShellVisual(workspacePalette, shellState)" in module_shell
    assert "shellVisual.borderColor" in module_shell
    for decorative_layer in (
        "AddRectFilledMultiColor",
        "shellInnerEdge",
        "contentAccentLine",
        "bodyAccent",
    ):
        assert decorative_layer not in module_shell


def test_filter_and_info_keep_their_internal_styles_and_receive_only_the_common_shell_perimeter():
    main = read(MAIN_CPP)
    sidebar_style = between(main, "static int PushSidebarShellStyle", "static int PushWaveformShellStyle")
    info_style = between(read(FILE_INFO_CPP), "int PushFileInfoShellStyle", "void RenderFileInfoPanelContent")
    module_shell = between(main, "auto drawDockModuleShell", "// The Info title actions belong to the module shell")

    assert "filterBrowserSelection" in sidebar_style
    assert "metadataPanelText" in info_style
    assert "CoreWorkspace" not in sidebar_style
    assert "CoreWorkspace" not in info_style
    assert "filterBrowserSelection" not in module_shell
    assert "infoPanelAccent" not in module_shell
    assert main.count("RenderSidebarPanelContent(state,") == 4
    assert main.count("RenderFileInfoPanelContent(state,") == 4
    for panel_id in ("##FloatingSidebar", "##FloatingFileInfo", "##DockedFilter", "##DockedFileInfo"):
        assert panel_id in main
    assert "beginSharedDockWindowHost(" in main
    assert main.count("ImGui::PushStyleColor(ImGuiCol_Border, workspacePalette.perimeter)") == 1


def test_filter_and_info_share_one_rounded_outer_shell_without_global_restyling():
    main = read(MAIN_CPP)
    theme = read(THEME_CPP)
    module_shell = between(main, "auto drawDockModuleShell", "// The Info title actions belong to the module shell")

    # Filter and Info are the two special module windows. Their outer shells
    # use the same canonical radius in both docked and floating/detached modes;
    # no one-sided join rule may square only the Info panel.
    assert "constexpr float kFilterInfoShellRadius" in main
    assert "aura::ui::workspace::kCoreWorkspaceMetrics.compactControlRadius" in main
    assert "dockJoinLeft" not in module_shell
    assert "RoundCornersRight" not in module_shell
    assert "RoundCornersTopRight" not in module_shell
    assert "RoundCornersBottomRight" not in module_shell
    assert "ImDrawFlags_RoundCornersAll" in module_shell
    assert "ImDrawFlags_RoundCornersTop" in module_shell
    assert "ImDrawFlags_RoundCornersBottom" in module_shell
    assert "layout.contentMin.x + 0.5f" in module_shell
    assert "layout.contentMax.x - 0.5f" in module_shell

    # One scoped Begin helper owns the outer radius/perimeter for the two
    # floating and two legacy-detached Filter/Info windows.
    assert "auto beginSharedDockWindowHost" in main
    assert main.count("beginSharedDockWindowHost(") >= 4
    assert "ImGuiStyleVar_WindowRounding, kFilterInfoShellRadius" in main

    # The targeted module fix must not flatten or globally normalize the rest
    # of Aris. The product baseline remains unchanged.
    baseline = between(
        theme,
        "void ThemeColors::applyStyleBaseline",
        "ThemeDialogStyleScope::ThemeDialogStyleScope",
    )
    assert "style.WindowRounding    = 0.0f" in baseline
    assert "style.ChildRounding     = 6.0f" in baseline
    assert "style.FrameRounding     = 4.0f" in baseline


def test_shipping_workspace_style_pushes_and_pops_stay_balanced():
    main = read(MAIN_CPP)

    names = re.findall(
        r"const int (\w+) = Push(?:Sidebar|FileInfo)ShellStyle\(ThemeColors::current\(\)\);",
        main,
    )
    assert names
    for name in set(names):
        assert main.count(f"PopStyleColor({name})") == names.count(name)
    assert main.count("const int dockGroupStyleCount = filterStyle ? PushSidebarShellStyle(theme) : PushFileInfoShellStyle(theme);") == 1
    assert main.count("PopStyleColor(dockGroupStyleCount)") == 1
    waveform = between(main, 'BeginTransientScrollbarChild("WaveformPanel"', "// FLOATING PANELS")
    assert "ThemeColors::waveformPanelBG(waveformShellTheme)" in waveform
    assert "const ImVec4 waveShellEdge = workspacePalette.perimeter" in waveform
    assert "const ImVec4 waveShelfLine = workspacePalette.splitter" in waveform
    assert main.count("ImGui::PushStyleColor(ImGuiCol_WindowBg, workspacePalette.applicationSurface)") == 1
    assert main.count("// workspace application surface") == 2


def test_top_toolbar_search_respects_reserved_width_for_navigation():
    main = read(MAIN_CPP)
    search = between(main, "const bool historyAfterSearch", "float navCapsuleScreenX")

    assert "toolbarResultsRightScreenX - resultsHomeLeftScreenX" in search
    assert "historyReserveW - presetDialReserveW - toolbarDialRightInset - toolbarNavMinGap" in search
    assert "spaceAfterAnchor - compactNavWidth, searchMinW, searchMaxW" in search
    assert "spaceAfterAnchor - navReserveW, searchMinW, searchFitMaxW" in search
    assert 'DrawSearchField("##SearchBar"' in search
    assert "searchRectMax.x + historyReserveW" in search


def test_results_content_edge_is_compact_and_tab_labels_reject_settings_payloads():
    main = read(MAIN_CPP)
    center = between(main, "// Center Panel - File List", "// Right Column - File Info")
    load_settings = between(main, "void LoadAppSettings", "// Tab presets (F-033)")
    save_settings = between(main, "bool TrySaveAppSettings", "// Panel states (new unified system)")

    # Tabs, breadcrumb/path, table header and rows share one compact content
    # origin instead of the previous 20 px left moat.
    layout = read(ROOT / "src" / "ui" / "ResultsPanelLayout.h")
    assert "kContentInsetX = aura::ui::workspace::kCoreWorkspaceMetrics.space8" in layout
    assert "ImGuiStyleVar_WindowPadding, ImVec2(harken::results::kContentInsetX, 0.0f)" in center
    assert "ImVec2(20.0f, 2.0f)" not in center
    assert "constexpr float kBreadcrumbLabelIndentX = 0.0f" in center

    # A damaged settings value such as
    # `tab0_name=<mojibake>tab0_viewMode=1` must never reach visible UI or be
    # persisted again. Visible prefixes stay valid UTF-8 independently of the
    # source file's legacy encoding history.
    assert "SanitizeTabNameFromSettings" in main
    assert "FindEmbeddedTabSettingsKey" in main
    assert '"_viewMode="' in main
    assert "SanitizeTabNameFromSettings(itName->second" in load_settings
    assert "SanitizeTabNameFromSettings(tab.name" in save_settings
    assert "visibleTabName" in center
    assert "L(TabPrefixDir)" in center
    assert "L(TabPrefixFilter)" in center
    assert "L(TabPrefixSearch)" in center
    assert "###Tab%d" in center
    assert "Utf8ToWidePermissive" in main
    assert "Misc_AllFiles" in center


def test_all_resize_handles_share_the_three_dot_grip_logic():
    main = read(MAIN_CPP)
    style_header = read(STYLE_HEADER)
    shell_source = read(SHELL_SOURCE)

    assert "DrawThreeDotSplitterGrip" in style_header
    assert "DrawThreeDotSplitterGrip" in shell_source
    assert "drawList->AddLine" not in between(shell_source, "void ModuleWindowShellScope::drawResizeAffordance()", "ModuleWindowShellScope::~ModuleWindowShellScope")
    assert "DrawThreeDotSplitterGrip" in main
    vertical = between(main, "bool VerticalSplitter(", "// Draw a horizontal splitter")
    horizontal_start = main.rindex("bool HorizontalSplitter(")
    horizontal_end = main.index("// Begin a dockable panel", horizontal_start)
    horizontal = main[horizontal_start:horizontal_end]
    assert "DrawThreeDotSplitterGrip" in vertical
    assert "DrawThreeDotSplitterGrip" in horizontal
    assert "ResolveThreeDotSplitterColor" in vertical
    assert "ResolveThreeDotSplitterColor" in horizontal
    assert "dl->AddLine(" not in vertical
    assert "dl->AddLine(" not in horizontal

    meter = between(main, "// Meter resize handle", "// AUDITION_NATIVE_EXCEPTION: waveform-derived-meter")
    assert "const float handleWidth = 12.0f" in meter
    assert "InvisibleButton(\"##handle\", ImVec2(handleWidth" in meter
    assert "DrawThreeDotSplitterGrip" in meter
    assert "ResolveThreeDotSplitterColor" in meter


def test_lapis_blue_chrome_avoids_white_search_and_black_transport_seams():
    main = read(MAIN_CPP)
    controls = read(MAIN_CPP.parent / "ui" / "ToolbarSearchControls.h")
    toolbar_palette = between(controls, "inline Palette ResolveSearchPalette", "struct SearchFieldResult")

    assert "toolbarTheme = ThemeColors::current()" in main
    assert "toolbarTheme, toolbarTheme.toolbarBG, toolbarTheme.frameBgBase, toolbarTheme.interfaceText" in main
    for token in ("toolbarAccent", "controlAccent", "searchButtonBG", "resultsSelection"):
        assert f"theme.{token}" in toolbar_palette
    assert "AppTheme::" not in toolbar_palette and "IM_COL32" not in toolbar_palette
    builtin = read(BUILTIN_THEME_CPP)
    assert "s.transportBarBG         = H(0xff223a52)" in builtin
    assert "s.transportBarBG         = H(0xff000000)" not in builtin
    assert "const ImVec2 tableMin(winPos.x, ImGui::GetCursorScreenPos().y);" in main
    assert "ImGui::GetWindowPos().x + ImGui::GetWindowSize().x" in main
    assert "splitterLaneSurface" not in between(main, "bool VerticalSplitter(", "// Draw a horizontal splitter")
    for theme in ("Dark", "Ocean", "RoseCedar", "JadeMist"):
        assert f"case AppTheme::{theme}:" in main


def test_browser_path_hover_uses_results_semantics_without_a_new_border():
    main = read(MAIN_CPP)
    path_row = between(
        main,
        "// --- Row 1: parent button + path bar + open button ---",
        "if (pathEnter)",
    )
    path_call_start = path_row.index('DrawUnifiedSearchInput(\n                "##BrowserPathInput"')
    path_call = path_row[path_call_start : path_call_start + 420]

    assert "browserPathHoverBg" in path_row
    assert "kCurrentTheme.resultsHeaderBG" in path_row
    assert "kCurrentTheme.resultsSelection" in path_row
    assert "kCurrentTheme.resultsText" in path_row
    assert "const ImVec4 browserPathFrameBg" in path_row
    assert "browserPathInputFocused" in path_row
    assert "browserPathHitHovered ? browserPathHoverBg" in path_row
    assert "ImGui::PushStyleColor(ImGuiCol_FrameBg, browserPathFrameBg)" in path_row
    assert "ImGuiMouseCursor_Hand" in path_row
    assert "true" in path_call
    assert "AddRect(" not in path_row


def test_browser_path_input_and_hit_area_share_explicit_remaining_row_width():
    main = read(MAIN_CPP)
    path_row = between(
        main,
        "// --- Row 1: parent button + path bar + open button ---",
        "if (pathEnter)",
    )

    assert "const float browserPathInputWidth" in path_row
    assert "ImGui::GetContentRegionAvail().x" in path_row
    assert "openButtonWidth" in path_row
    assert "ImGui::GetStyle().ItemSpacing.x" in path_row
    assert "browserPathHitMin.x + browserPathInputWidth" in path_row
    assert "ImGuiInputTextFlags_EnterReturnsTrue,\n                browserPathInputWidth," in path_row
    assert "ImGui::PushItemWidth(-parentButtonWidth" not in path_row


def test_transport_is_36px_and_pitch_group_shares_one_column_center():
    main = read(MAIN_CPP)
    waveform = between(
        main,
        "const float waveQuickBarH",
        "// Waveform display with region selection",
    )
    pitch = waveform[waveform.index("// PITCH WHEEL") :]

    assert "const float waveTransportH = 36.0f;" in waveform
    assert "const float kBarH = waveTransportH;" in waveform
    assert "const float waveTransportH = 40.0f;" not in waveform
    assert "const float kBarH    = 40.0f;" not in waveform

    assert "const float pitchColumnWidth =" in pitch
    assert "pitchColumnOriginX + (pitchColumnWidth - pitchWheelWidth) * 0.5f" in pitch
    assert "pitchColumnOriginX + pitchColumnWidth" in pitch
    assert "Dummy(ImVec2(pitchWheelPadding, 0))" not in pitch


def test_results_frame_has_one_complete_rounded_two_pixel_waveform_color_owner():
    main = read(MAIN_CPP)
    center_setup = between(main, "// Center list: results* tokens", "// Right Column - File Info")
    assert "const ImVec4 centerResultsOutline = centerResultsTheme.waveformColor;" in center_setup
    assert "const ImVec4 centerResultsOutline = workspacePalette.perimeter;" not in center_setup
    assert "ImGuiStyleVar_ChildBorderSize" not in center_setup
    assert 'BeginTransientScrollbarChild("CenterFileList", ImVec2(surfaceWidth, surfaceHeight), ImGuiChildFlags_AlwaysUseWindowPadding,' in center_setup
    assert center_setup.count("centerResultsDrawList->AddRect(") == 1
    assert "ImGui::GetForegroundDrawList(ImGui::GetMainViewport())" not in center_setup
    assert center_setup.index("centerResultsDrawList->PushClipRectFullScreen();") < center_setup.index("centerResultsDrawList->AddRect(")
    assert center_setup.index("centerResultsDrawList->AddRect(") < center_setup.index("centerResultsDrawList->PopClipRect();")
    assert "s_centerPanelMin.x + 0.5f" in center_setup
    assert "s_centerPanelMax.x - 0.5f" in center_setup
    # Results is the visual focus. The pre-normalization implementation used
    # a dedicated 2 px frame; it must not collapse back to the generic 1 px
    # dock-shell perimeter even though it shares the waveform shell color.
    assert "constexpr float kResultsPerimeterWidth = 2.0f;" in center_setup
    assert "constexpr float kResultsPerimeterRounding = 7.0f;" in center_setup
    assert "kResultsPerimeterWidth);" in center_setup
    assert "kResultsPerimeterRounding," in center_setup
    assert "ImDrawFlags_RoundCornersAll," in center_setup
    assert "kCoreWorkspaceMetrics.perimeterWidth);" not in center_setup
    assert "ImDrawFlags_None" not in center_setup


def test_results_frame_uses_the_table_layer_even_when_browser_has_no_rows():
    main = read(MAIN_CPP)
    center_setup = between(main, "// Center list: results* tokens", "// Right Column - File Info")

    assert "ImDrawList* centerResultsDrawList = nullptr;" in center_setup
    assert "centerResultsDrawList = ImGui::GetWindowDrawList();" in center_setup
    assert center_setup.count("centerResultsDrawList = ImGui::GetWindowDrawList();") >= 2
    assert center_setup.index("if (BeginTransientScrollbarTable(resultsTableName,") < center_setup.rindex("centerResultsDrawList = ImGui::GetWindowDrawList();")
    assert center_setup.index("ImGui::EndTable();") < center_setup.index("centerResultsDrawList->AddRect(")
    assert "ImGui::GetForegroundDrawList(ImGui::GetMainViewport())" not in center_setup
    assert "centerResultsDrawList->AddRect(" in center_setup


def test_default_docked_results_match_the_side_shell_vertical_insets():
    main = read(MAIN_CPP)
    home_results = between(main, "renderLeftColumn(renderDockTabsInShell);", "// Right Column - File Info")
    assert re.search(
        r"ImGui::SetCursorScreenPos\(ImVec2\(resultsHomePos\.x,\s*"
        r"resultsHomePos\.y \+ kDockShellVerticalInset\)\);\s*"
        r"renderResultsSurface\(centerWidth,\s*availHeight - 2\.0f \* kDockShellVerticalInset\);",
        home_results,
    ), "The default results child must inset both vertical edges, not just its drawn frame."

    shell_layout = between(main, "struct DockModuleShellLayout", "auto drawDockModuleShell")
    assert "constexpr float kDockShellVerticalInset = aura::ui::workspace::kCoreWorkspaceMetrics.space4;" in main
    assert "float insetT = kDockShellVerticalInset;" in shell_layout
    assert "float insetB = kDockShellVerticalInset;" in shell_layout
    home_slot = between(main, "const aura::ui::dock::DockRect resultsHomeRect{", "};")
    assert "resultsHomePos.x, resultsHomePos.y, centerWidth, availHeight" in home_slot


def test_folder_search_inherits_sidebar_semantic_colors_in_both_views():
    main = read(MAIN_CPP)
    toolbar = between(main, "static void RenderFolderTreeExplorerToolbar", "static void RenderFolderTreeExplorerList")
    assert "ResolveSearchPalette(ThemeColors::current()," in toolbar
    for color in ("ChildBg", "FrameBg", "Text"):
        assert f"ImGui::GetStyleColorVec4(ImGuiCol_{color})" in toolbar
    assert "harken::toolbar::DrawSearchField(" in toolbar
    assert "browserSource.searchId,\n                L(Filter_SearchFolder)" in main
    assert "databaseSource.searchId,\n                L(Filter_SearchFolder)" in main
    assert "ImGuiCol_FrameBg,              leftPanelInput" in main
    assert "ImGuiCol_Border,               leftPanelOutline" in main
    assert "ImGuiCol_Text,                 leftPanelText" in main


def test_folder_tree_search_preserves_refresh_and_locate_action_width():
    main = read(MAIN_CPP)
    toolbar = between(main, "static void RenderFolderTreeExplorerToolbar", "static void RenderFolderTreeExplorerList")
    sources = between(main, "static ExplorerSource BrowserExplorerSource", "static void RenderRelatedPathsExplorerPane")

    assert "2.0f * (kButtonGap + aura::folder_tree::controls::kButtonSize) + 2.0f" in toolbar
    assert "ImGui::GetContentRegionAvail().x - searchReserve" in toolbar
    assert "searchBufSize, searchWidth, palette);" in toolbar
    assert "search.cleared" in toolbar and "onSearchEscape();" in toolbar
    assert "DrawButton(refreshId, ICON_LC_REFRESH, true," in toolbar
    assert "DrawButton(locateId, ICON_LC_FOLDER_OPEN, canLocate," in toolbar
    assert "&refreshMotion, refreshBusy)" in toolbar
    assert "browserSource.locateId,\n                canLocateCurrent,\n                controls.refresh," in main
    assert "databaseSource.locateId,\n                locateTarget != nullptr,\n                controls.refresh," in main
    assert "ImGui::SetNextItemWidth(-searchReserve)" not in toolbar
    for action_id in (
        "##BrowserFolderSearch", "##BrowserSidebarRefreshBtn", "##BrowserLocateCurrentFolderBtn",
        "##FolderSearch", "##SidebarRefreshBtn", "##LocateBtn",
    ):
        assert action_id in sources
    assert main.count("RenderFolderTreeExplorerToolbar(") == 3


def test_three_metadata_pickers_use_the_sidebar_input_surface_in_every_theme():
    main = read(MAIN_CPP)
    metadata = between(main, "// Section 2: 元数据过滤", "// Section 3: 播放列表")

    assert "const bool useLapisBlueMetadataSearchSurface = state.settings.theme == AppTheme::LapisBlue;" in metadata
    for input_id in ("##ArtistFilter", "##AlbumFilter", "##CategoryFilter"):
        assert f'"{input_id}"' in metadata
    picker_input = between(main, "static void DrawSidebarMetadataPickerField(", "const ImVec2 inputMin")
    assert "PushSidebarMetadataInputColors();" in picker_input
    assert "inputOptions.inputFlags = ImGuiInputTextFlags_AutoSelectAll;" in picker_input
    assert "inputOptions.width = fieldWidth;" in picker_input
    assert "inputOptions.inheritContextColors = true;" in picker_input
    assert "aura::ui::TextInput(inputId, &visible, inputOptions)" in picker_input
    assert picker_input.index("PushSidebarMetadataInputColors();") < picker_input.index("aura::ui::TextInput(")
    assert "ImGui::PopStyleColor(6);" in picker_input

    key_call = metadata[metadata.index('DrawUnifiedSearchInput("##KeyFilter"') :][:320]
    assert "useLapisBlueMetadataSearchSurface" in key_call
    assert metadata.count("useLapisBlueMetadataSearchSurface") == 2
    assert "ImGuiCol_FrameBg,              leftPanelInput" in main
    assert "ImGuiCol_Border,               leftPanelOutline" in main
    assert "ImGuiCol_Text,                 leftPanelText" in main


def test_toolbar_has_one_theme_derived_top_highlight_owner():
    main = read(MAIN_CPP)
    toolbar_backdrop = between(
        main,
        "// One shallow command plane; its bottom edge owns this adjacency.",
        "const ImVec4 kToolbarChromeText")

    assert "kToolbarTopHighlightHeight = 1.5f" in toolbar_backdrop
    assert "toolbarTheme.toolbarBG" in toolbar_backdrop
    assert "toolbarTheme.controlAccent" in toolbar_backdrop
    assert "toolbarTheme.chromeBorder" in toolbar_backdrop
    assert toolbar_backdrop.count("dl->AddRectFilledMultiColor(") == 1
    assert toolbar_backdrop.count("dl->AddLine(") == 1
    assert "state.settings.theme" not in toolbar_backdrop
    assert "toolbarTopSheen" not in toolbar_backdrop
    assert "toolbarBottomShade" not in toolbar_backdrop


def test_lapis_secondary_results_text_stays_readable_on_navy_surface():
    main = read(MAIN_CPP)
    assert "case AppTheme::LapisBlue:" in main
    assert "s.resultsSecondaryText   = H(0xffd7e3ef);" in read(BUILTIN_THEME_CPP)


def test_system_menu_font_keeps_141_proportional_mapping():
    settings = read(ROOT / "src" / "AppState.h")
    windows = read(ROOT / "src" / "WindowsMain.cpp")
    mapping = " ".join(between(windows, "float GetMenuFontSizePx()", "ImFont* GetMenuUIFont()").split())
    assert "kChromeFontPx = 14;" in settings
    assert "kDefaultReadingFontPx = 18;" in settings
    assert "kMaxReadingFontPx = 32;" in settings
    assert "g_readingRegular ? g_readingRegular->LegacySize : (float)kDefaultReadingFontPx" in mapping
    assert "(float)kChromeFontPx * (readingPx / (float)kDefaultReadingFontPx)" in mapping
    assert "return std::clamp(std::round(scaled), (float)kChromeFontPx, (float)kMaxReadingFontPx);" in mapping


def test_top_menu_uses_the_system_font_scope():
    main = read(MAIN_CPP)
    theme = read(THEME_CPP)
    windows = read(ROOT / "src" / "WindowsMain.cpp")
    header = between(main, "// TOP MENU BAR", "// TOP TOOLBAR")
    host = between(header, "const float baseMenuFontPx", "ImGui::BeginMainMenuBar()")
    bar = between(header, "if (ImGui::BeginMainMenuBar())", "ImGui::EndMainMenuBar();")
    assert "ImGuiWindowFlags_MenuBar" not in header
    assert "const float baseMenuFontPx = GetMenuFontSizePx();" in host
    assert "baseMenuFontPx == kChromeFontPx ? minMenuBarFontPx : baseMenuFontPx" in host
    assert "std::max(static_cast<float>(kChromeFontPx), menuBarFontPx) * menuScale" in host
    assert "ImGui::PushFont(menuFont, GetMenuChromeFontPushSize(menuFont, hostFontPx));" in host
    assert "ThemeMenuBarStyleScope menuBarChrome(menuFont, menuBarFontPx);" in bar
    assert "ThemeMenuPopupShellScope menuPopupShell(true, baseMenuFontPx);" in bar

    for title in ("File", "Edit", "View", "Database", "Playlist", "Tools", "Settings", "Help"):
        popup = between(bar, f"ImGui::BeginMenu(L(Menu_{title}))", f"menuChrome{title}.reset();")
        assert f"ThemeMenuPopupStyleScope menuChrome{title}(GetUIFontRegular());" in popup
    assert re.search(r"\b(?:ReadingTextScope|MenuTextScope)\s+\w+", bar) is None
    chrome_scope = between(windows, "ChromeTextScope::ChromeTextScope()", "ChromeTextScope::~ChromeTextScope()")
    assert re.search(r"if \(!InMenuChromeScope\(\)\)\s*\{\s*ImGui::PushFont\(g_uiFontRegular\);", chrome_scope)

    bar_style = between(theme, "ThemeMenuBarStyleScope::ThemeMenuBarStyleScope", "void ThemeMenuBarStyleScope::reset")
    assert "const float desiredTextPx = textSizePx * scale;" in bar_style
    assert re.search(r"PushMenuTextFont\(textFont,[^;]*desiredTextPx[^;]*\);", bar_style)
    popup_style = between(theme, "ThemeMenuPopupStyleScope::ThemeMenuPopupStyleScope",
                          "ThemeMenuPopupShellScope::ThemeMenuPopupShellScope")
    assert "const float textSizePx = s_menuPopupTextSizePx;" in popup_style
    assert "PushMenuTextFont(textFont ? textFont : ImGui::GetFont(), textSizePx * scale, m_pushedFont);" in popup_style
    popup_shell = between(theme, "ThemeMenuPopupShellScope::ThemeMenuPopupShellScope",
                          "bool ThemeMenuPopupShellScope::beginContextItem")
    assert "m_textSizePx(std::max(kMenuPopupMinTextPx, textSizePx))" in popup_shell
    assert "s_menuPopupTextSizePx = m_textSizePx;" in popup_shell


def test_appended_tools_menu_keeps_the_system_popup_font_scope():
    main = read(MAIN_CPP)
    shared_shell = between(main, "ThemeMenuPopupShellScope menuPopupShell(true, baseMenuFontPx);", "menuPopupShell.reset();")
    assert "RenderToolsMenu(state);" in shared_shell
    module = read(ROOT / "src" / "ModuleUI.cpp")
    tools = between(module, "void RenderToolsMenu(AppState& state)", "// 1)")
    assert tools.index("ImGui::BeginMenu(L(Menu_Tools))") < tools.index("ThemeMenuPopupStyleScope menuChromeTools;")
    assert tools.index("ThemeMenuPopupStyleScope menuChromeTools;") < tools.index("ImGui::MenuItem(")
    assert re.search(r"menuChromeTools\.reset\(\);\s*ImGui::EndMenu\(\);", tools)
    assert re.search(r"\b(?:ReadingTextScope|MenuTextScope)\s+\w+", tools) is None


def test_results_file_path_column_fills_trailing_width():
    main = read(MAIN_CPP)
    assert "ImGuiTableFlags_SizingStretchProp" in between(main, "// File table with sortable columns", "// Table ID V5")
    assert "ImGui::TableSetupColumn(L(Col_FilePath), ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_DefaultHide" in main
    assert "DateAdded; FilePath stretches" in main


def test_open_source_preset_dial_is_not_edition_gated():
    main = read(MAIN_CPP)
    preset_surface = between(
        main,
        "// F-034: Column view preset dial",
        "// CMD_MENU popup",
    )

    assert 'ProductEdition::hasFeature("column_preset_dial")' not in preset_surface
    assert "RenderPresetDial(state);" in preset_surface


def test_open_source_preset_dial_seeds_defaults_before_settings_early_returns():
    main = read(MAIN_CPP)
    load_settings = between(main, "void LoadAppSettings", "// Tab presets (F-033)")
    seed = load_settings.index("InitDefaultColumnPresets(state)")
    empty_path_return = load_settings.index("if (g_appSettingsPathUtf8.empty()) return;")
    empty_file_return = load_settings.index("if (kv.empty()) return;")

    assert seed < empty_path_return < empty_file_return


def test_preset_dial_is_positioned_inside_the_compact_toolbar_clip_rect():
    main = read(MAIN_CPP)
    toolbar = between(main, "// TOP TOOLBAR - Search and Buttons", "// MAIN CONTENT AREA")
    preset_surface = between(
        main,
        "// F-034: Column view preset dial (single open-source surface)",
        "// CMD_MENU popup",
    )

    assert "toolbarWindowPos.x + ImGui::GetWindowContentRegionMax().x" in toolbar
    assert "toolbarWindowPos.y + presetDialY" in toolbar
    assert "ImGui::SetCursorScreenPos(presetDialRect.Min);" in preset_surface


def test_preset_dial_right_edge_follows_the_results_panel_right_edge():
    main = read(MAIN_CPP)
    toolbar = between(main, "// TOP TOOLBAR - Search and Buttons", "// MAIN CONTENT AREA")

    # The toolbar and docked workspace must consume one width calculation.
    # When Info is docked, subtract its real width and splitter lane so the
    # preset dial ends exactly at the Results panel's right edge.
    assert "const float toolbarResultsRightScreenX" in toolbar
    assert "rightDockSlotRequired ? (rightDockColumnWidth + splitterWidth) : 0.0f" in toolbar
    assert "toolbarFlowInput.toolbarMaxX = toolbarResultsRightScreenX;" in toolbar
    assert "toolbarFlowInput.toolbarMaxX = toolbarContentRightScreenX;" not in toolbar
    assert "const float toolbarDialRightInset = showRightColumn ? 0.0f : toolbarNavRightPad;" in toolbar
    assert "toolbarFlowInput.rightInset = toolbarDialRightInset;" in toolbar
    assert "toolbarFlowInput.rightInset = toolbarNavRightPad;" not in toolbar

    toolbar_begin = main.index('BeginTransientScrollbarChild("Toolbar"')
    right_width = main.index("float rightPanelWidth = showRightColumn")
    assert right_width < toolbar_begin
    assert main.count("float rightPanelWidth = showRightColumn") == 1


def test_preset_dial_owns_a_reserved_toolbar_lane_after_navigation():
    main = read(MAIN_CPP)
    toolbar = between(main, "// TOP TOOLBAR - Search and Buttons", "// MAIN CONTENT AREA")
    preset_surface = between(
        main,
        "// F-034: Column view preset dial (single open-source surface)",
        "// CMD_MENU popup",
    )

    # Production drawing consumes one resolved flow: every navigation item,
    # scan/refresh/more action, and the dial use the slots verified by the C++
    # geometry tests. Merely naming independent rectangles is not sufficient.
    assert "ToolbarFlowLayoutInput toolbarFlowInput" in toolbar
    assert "ResolveToolbarFlowLayout(toolbarFlowInput)" in toolbar
    assert "toolbarFlow.itemSlots[i]" in toolbar
    assert "toolbarFlow.itemSlots[5]" in toolbar
    assert "toolbarFlow.navCapsule" in toolbar
    assert "toolbarFlow.presetDial" in toolbar
    assert "ImGui::SetCursorScreenPos(presetDialRect.Min);" in preset_surface
    assert "toolbarNavRect.Max.x <= presetDialRect.Min.x - kPresetDialGap" not in preset_surface

    # The former independent placement used only the Info-panel width and had
    # no knowledge of the actual navigation rectangle.
    assert "contentMaxX - rPanelW - kPresetDialWidth" not in preset_surface


def test_primary_search_uses_the_scoped_toolbar_input_material():
    main = read(MAIN_CPP)
    toolbar = between(main, "// TOP TOOLBAR - Search and Buttons", "// MAIN CONTENT AREA")

    call = between(toolbar, 'DrawSearchField("##SearchBar"', "inputReturn = search.submitted")
    assert "toolbarSearchPalette, harken::toolbar::SearchFieldOptions{" in call
    assert "true, state.searchExactModeLocked, searchFocusRequested" in call
    controls = read(MAIN_CPP.parent / "ui" / "ToolbarSearchControls.h")
    material = between(controls, "inline SearchFieldResult DrawSearchField(", "const auto iconPressed")
    assert "AddRectFilled(start, end, ImGui::GetColorU32(palette.searchFill)" in material
    assert "ImGuiCol_FrameBg, transparent" in controls


def test_preset_dial_has_shadow_without_a_perimeter_card():
    main = read(MAIN_CPP)
    preset = between(main, "static bool RenderPresetDial", "static void ApplyDefaultTransferSettingsToDialog")

    assert "dialShadow" in preset
    assert "AddRectFilledMultiColor" in preset
    assert "AddRect(" not in preset
    assert "AddCircle(" not in preset


def test_runtime_twin_maps_native_toolbar_slots_without_overlay_coordinates():
    chrome = read(RUNTIME_CHROME)
    css = read(RUNTIME_TWIN_CSS)

    assert 'className="toolbar-nav-flow"' in chrome
    assert 'className="toolbar-primary-nav top-modes"' in chrome
    assert 'className="preset-dial-slot"' in chrome
    assert 'className="toolbar-secondary-actions"' in chrome
    assert "modes.slice(0, 3)" in chrome
    assert "modes.slice(3)" in chrome
    assert ".runtime-twin .toolbar-nav-flow{" in css
    assert ".runtime-twin .preset-dial-slot{" in css
    assert ".runtime-twin .toolbar-secondary-actions{" in css
    assert ".runtime-twin .preset-dial{position:absolute;inset:0" in css
    assert ".runtime-twin .preset-dial{position:absolute;right:538px" not in css


def test_runtime_twin_exposes_the_four_native_visual_contracts():
    css = read(RUNTIME_TWIN_CSS)

    assert "--rt-results-outline:#43586f" in css
    assert ".runtime-twin .file-table{border:1px solid var(--rt-results-outline)" in css
    assert "--rt-filter-input:#e8edf3" in css
    assert "--rt-filter-input-text:#000" in css
    assert ".runtime-twin .folder-search>input{background:var(--rt-filter-input)" in css
    assert ".runtime-twin .runtime-toolbar:before{" in css
    assert "linear-gradient(90deg,var(--rt-toolbar-highlight-start),var(--rt-toolbar-highlight-end))" in css


def test_waveform_zoom_scrollbar_is_transient_independently_and_never_shown_at_one_x():
    header = read(STYLE_HEADER)
    source = read(STYLE_SOURCE)
    main = read(MAIN_CPP)
    scroll = between(
        main,
        "// Horizontal scroll bar directly below waveform",
        "// 🚀 UNIFIED WAVEFORM FOOTER",
    )

    assert "TransientScrollbarVisibilityInput" in header
    assert "kTransientWaveformScrollbarZoomThreshold" in header
    assert "kTransientScrollbarIdleSeconds" in header
    assert "ShouldShowTransientScrollbar" in source
    assert "kTransientWaveformScrollbarIdleSeconds" not in header
    assert "ShouldShowTransientWaveformScrollbar" not in source
    assert "s_waveformScrollBarLastInteractionAt" not in main
    assert "showTransientWaveformScrollbar" in scroll
    assert "if (showTransientWaveformScrollbar)" in scroll
    assert "waveformScrollbarRegionId" in scroll
    assert "transientScrollbarController.shouldShow(\n                    waveformScrollbarRegionId" in scroll
    assert "transientScrollbarController.reportInteraction(\n                waveformScrollbarRegionId" in main
    assert "waveformScrollBarEligible" in scroll
    assert 'ImGui::InvisibleButton("##WaveScrollBar"' in scroll
    assert "ImGuiButtonFlags_AllowOverlap" in scroll
    assert "const float pitchColumnTailHeight = pitchModeTopOffset + pitchModeHeight;" in main
    assert "const float reservedH = std::max(waveFileNameBarHeight, pitchColumnTailHeight);" in main
    assert "waveScrollBarHeight + waveFooterGap" not in main
    assert "waveformPos.y + waveformHeight - scrollBarHeight - scrollOverlayInset" in scroll
    assert "const ImVec2 footerPos(waveformPos.x, waveformPos.y + waveformHeight + 1.0f);" in main
    assert "Ctrl+Scroll Zoom" not in scroll


def test_native_scrollbars_use_independent_region_visibility_gates():
    header = read(STYLE_HEADER)
    source = read(STYLE_SOURCE)
    main = read(MAIN_CPP)
    theme = read(THEME_CPP)
    behavior = read(BEHAVIOR)

    assert "inline constexpr double kTransientScrollbarIdleSeconds = 3.0" in header
    assert "class TransientScrollbarController" in header
    assert "class TransientScrollbarStyleScope" in header
    assert "PushTransientScrollbarColors" in header
    assert "GetWindowScrollbarRect" in source
    assert "GetWindowScrollbarID" in source
    assert "MouseWheelH" in source

    assert read(SCROLLBAR_HOST_CPP).count("TransientScrollbarController transientScrollbarController") == 1
    assert "TransientScrollbarStyleScope transientScrollbarStyle" not in main
    assert "BeginTransientScrollbarChild" in main
    assert "BeginTransientScrollbarTable" in main
    # The Similar Sounds leaf explicitly disables native scrollbars; normal
    # content uses the wrapper. The QA narrator is a local packaging addition.
    assert main.count("ImGui::BeginChild(") == 1
    similar = between(main, 'ImGui::BeginChild(\n                        "##DockedSimilarSoundsContent"', 'ImGui::EndChild();')
    assert "ImGuiWindowFlags_NoScrollbar |" in similar
    assert "ImGui::BeginTable(" not in main
    assert "ImGui::Begin(" not in main
    assert "ImGui::BeginPopup(" not in main
    assert "ImGui::BeginPopupModal(" not in main
    assert "ImGui::BeginCombo(" not in main
    assert main.count("PushTransientScrollbarColors") >= 4
    assert "PushTransientScrollbarColors" in read(FILE_INFO_CPP)
    assert "PushTransientScrollbarColors" in theme

    for slot in (
        "ImGuiCol_ScrollbarBg",
        "ImGuiCol_ScrollbarGrab",
        "ImGuiCol_ScrollbarGrabHovered",
        "ImGuiCol_ScrollbarGrabActive",
    ):
        assert f"ImGui::PushStyleColor({slot}" not in main
        assert f"pushColor({slot}" not in theme

    theme_test = between(
        behavior,
        "add_executable(TestThemeColors",
        "add_test(NAME TestThemeColors",
    )
    assert "src/ui/CoreWorkspaceStyle.cpp" in theme_test

    assert '"##BrowserRelatedPathsBox"' in between(main, 'static ExplorerSource BrowserExplorerSource()', 'static ExplorerSource DatabaseExplorerSource()')
    assert '"##RelatedPathsBox"' in between(main, 'static ExplorerSource DatabaseExplorerSource()', 'static void RenderRelatedPathsExplorerPane(')
    related_pane = between(main, 'static void RenderRelatedPathsExplorerPane(', 'renderRows();')
    assert re.search(r'BeginTransientScrollbarChild\(\s*childId,', related_pane)
    assert "BeginTransientScrollbarTable(resultsTableName," in main
    assert "reportInteraction(ImGui::GetTime())" not in main

    vertical_splitter = between(
        main,
        "bool VerticalSplitter(",
        "// Draw a horizontal splitter and handle dragging to resize",
    )
    assert "TransientScrollbarController" not in vertical_splitter
    assert "reportInteraction" not in vertical_splitter
    assert "DrawThreeDotSplitterGrip" in vertical_splitter
    assert "ImGuiCol_SliderGrab" in main


def test_browser_cover_column_width_keeps_localized_header_visible():
    main = read(MAIN_CPP)
    cover_column = between(
        main,
        "if (state.settings.showCoverColumn)",
        'ImGui::TableSetupColumn(u8"LUFS"',
    )

    assert "const float coverHeaderMinW" in cover_column
    assert "ImGui::CalcTextSize(L(Col_Cover)).x" in cover_column
    assert "fileTableStyle.CellPadding.x * 2.0f" in cover_column
    assert "ImGui::GetFontSize()" in cover_column
    assert "const float coverColW = coverColumnCount ? (std::max)(" in cover_column
    assert "static_cast<float>(state.settings.rowHeight) + kCoverColumnExtra" in cover_column
    assert "coverHeaderMinW" in cover_column
    assert "ImGuiTableColumnFlags_NoResize" in cover_column


def test_browser_mini_waveform_texture_keeps_cpu_pixel_clarity():
    main = read(MAIN_CPP)
    texture_upload = between(
        main,
        "static bool EnsureMiniWaveTextureLoaded(",
        "static void ProcessPendingMiniWaveTextures(",
    )

    assert "GL_TEXTURE_MIN_FILTER, GL_NEAREST" in texture_upload
    assert "GL_TEXTURE_MAG_FILTER, GL_NEAREST" in texture_upload
    assert "GL_TEXTURE_MIN_FILTER, GL_LINEAR" not in texture_upload
    assert "GL_TEXTURE_MAG_FILTER, GL_LINEAR" not in texture_upload


def test_browser_mini_waveform_texture_replacement_keeps_old_frame_visible():
    main = read(MAIN_CPP)
    texture_upload = between(
        main,
        "static bool EnsureMiniWaveTextureLoaded(",
        "static void ProcessPendingMiniWaveTextures(",
    )

    mismatch = between(
        texture_upload,
        "auto cacheIt = g_miniWaveTextureCache.find(filePath);",
        "if (filePath.empty()",
    )
    assert "staleTexture" in mismatch
    assert "scheduleFrontCoalescedByPath" in mismatch
    assert "outTex = staleTexture" in mismatch
    assert "return true" in mismatch
    assert "DeleteMiniWaveTexture(cacheIt->second.textureId)" not in mismatch

    upload = texture_upload[texture_upload.index("glGenTextures"):]
    assert upload.index("glTexImage2D") < upload.index("DeleteMiniWaveTexture")


def test_playlist_quickbar_blank_space_is_not_an_invisible_hide_button():
    main = read(MAIN_CPP)
    quickbar = between(
        read(PLAYLIST_QUICK_BAR_CPP),
        "void RenderPlaylistQuickBar(AppState& state, float height)",
        "void RenderPlaylistQuickBarRestoreStrip(AppState& state, float height)",
    )

    assert 'InvisibleButton("##PlaylistQuickHide"' not in quickbar
    assert "hideP0(-10000.0f, -10000.0f)" not in quickbar
    assert 'MenuItem(playlistQuickBarMenuLabel)' in main
    assert "SetPlaylistQuickBarVisible(state, !state.settings.showPlaylistQuickBar)" in main


def test_primary_search_has_a_static_theme_focus_ring_across_the_capsule():
    controls = read(MAIN_CPP.parent / "ui" / "ToolbarSearchControls.h")
    ring = between(controls, "if (result.focused)", "const auto iconSurface")

    assert "draw->AddRect(" in ring
    assert "GetClipRectMin()" in ring and "GetClipRectMax()" in ring
    assert "start.x - outset" in ring and "end.x + outset" in ring
    assert "ImGui::GetColorU32(palette.focus)" in ring
    assert "GetTime" not in ring and "sinf" not in ring


def test_primary_search_shares_the_results_left_edge():
    main = read(MAIN_CPP)
    layout = between(main, "const float leftDockColumnWidth", "// Right Column - File Info")

    assert "const float resultsHomeLeftScreenX" in layout
    assert "ImVec2(resultsHomeLeftScreenX, searchY)" in layout
    assert "ImVec2(resultsHomeLeftScreenX, dockContentTop)" in layout
    assert "historySlotWidth" not in layout


def test_database_tabs_restore_scroll_after_table_extent_is_known():
    main = read(MAIN_CPP)
    table = between(
        main,
        "if (BeginTransientScrollbarTable(resultsTableName,",
        'ImGui::EndTable();',
    )

    restore_pos = table.index("if (!state.listTabs.empty() && state.currentTab().restoreScrollY)")
    assert restore_pos > table.index("// UX #109: Empty state")
    restore = table[restore_pos:]
    assert "const float requestedScrollY" in restore
    assert "ImGui::GetScrollMaxY()" in restore
    assert "ImGui::SetScrollY(targetScrollY)" in restore
    assert "std::abs(observedScrollY - targetScrollY) <= 0.5f" in restore
    assert "state.currentTab().restoreScrollY = false" in restore

    pre_rows = table[:table.index("ImGuiListClipper clipper;")]
    assert "restoreScrollY = false" not in pre_rows


def test_loop_and_normalize_are_low_priority_footer_mode_buttons():
    main = read(MAIN_CPP)

    transport = between(
        main,
        "// Professional Transport Controls (DAW-style)",
        "// 1. Prepare footer text + lane geometry.",
    )
    footer = between(
        main,
        "// 1. Prepare footer text + lane geometry.",
        "// Sync legacy L/R aggregates",
    )

    # The high-visibility transport row no longer owns duplicate controls.
    assert 'InvisibleButton("##NormBadge"' not in transport
    assert 'SquareLedButton("##RegionLoop"' not in transport

    # Both low-frequency modes share the existing footer mode-button language.
    assert 'MiniModeBtn("##MLoop"' in footer
    assert 'ImGui::InvisibleButton("##MNormalize"' in footer
    assert '"waveform.footer.loop"' in footer
    assert '"waveform.footer.normalize"' in footer
    assert "auditionTransportView.setLoopAndRearmRegion(!auditionSnapshot.regionLoopEnabled)" in footer
    assert "state.settings.loudnessNormEnabled = !state.settings.loudnessNormEnabled" in footer
    assert "MarkSettingsDirty(state)" in footer
    assert "loopOn ? L(Loop_On) : L(Loop_Off)" in footer
    assert "L(Main_LoudnessNormFmtTip)" in footer

    # The shared geometry reserves four mode slots: shuffle, listening, loop, normalize.
    assert "footerMiniModeW * 3.0f + footerLoudnessBtnW" in footer
    assert "footerMiniModeGap * 3.0f" in footer


def test_pitch_mode_toggle_reserves_its_full_bottom_tail():
    main = read(MAIN_CPP)
    pitch_column = between(
        main,
        "// PITCH WHEEL (left side of waveform, Premium MIDI style)",
        "// Waveform display with region selection",
    )

    assert "const float pitchModeTopOffset = 22.0f;" in pitch_column
    assert "const float pitchModeHeight = 14.0f;" in pitch_column
    assert (
        "const float pitchColumnTailHeight = pitchModeTopOffset + pitchModeHeight;"
        in pitch_column
    )
    assert (
        "const float reservedH = std::max(waveFileNameBarHeight, pitchColumnTailHeight);"
        in pitch_column
    )
    assert "const float segY = pwY + pwHeight + pitchModeTopOffset;" in pitch_column
    assert "const float segH = pitchModeHeight;" in pitch_column
