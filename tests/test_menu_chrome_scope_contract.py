import re
from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]


class MenuChromeScopeContract(unittest.TestCase):
    def test_navigation_context_menu_uses_original_palette(self):
        main = (ROOT / "src/main.cpp").read_text(encoding="utf-8")
        helper = main.split("bool MenuItemWithTooltip(", 1)[1].split("return clicked;", 1)[0]
        for color in ("ImGuiCol_Header, COLOR_SELECTED", "ImGuiCol_HeaderHovered, COLOR_ACCENT"):
            self.assertIn(color, helper)
        self.assertIn("ImGui::PopStyleColor(2);", helper)

    def test_rounding_requires_menu_widget_and_opt_in(self):
        widgets = (ROOT / "thirdparty/imgui/imgui_widgets.cpp").read_text(encoding="utf-8")
        selectable = widgets.split("bool ImGui::Selectable(const char* label, bool selected", 1)[1]
        selectable = selectable.split("bool ImGui::Selectable(const char* label, bool* p_selected", 1)[0]
        self.assertIn("flags & ImGuiSelectableFlags_MenuHighlight", selectable)
        self.assertNotIn("mac_menu_highlight", selectable)
        self.assertIn("| MenuHighlightFlags(window)", widgets)
        self.assertIn("| menu_highlight_flags;", widgets)

    def test_menu_chrome_popups_stay_in_the_owner_viewport(self):
        widgets = (ROOT / "thirdparty/imgui/imgui_widgets.cpp").read_text(encoding="utf-8")
        begin = widgets.split("bool ImGui::BeginMenuEx(", 1)[1].split("bool ImGui::BeginMenu(", 1)[0]
        self.assertEqual(begin.count("BeginPopupMenuEx("), 2)
        self.assertEqual(begin.count("SetNextWindowViewport(window->ViewportId);"), 2)
        self.assertEqual(begin.count("if (menu_highlight_flags & ImGuiSelectableFlags_MenuHighlight)"), 2)

    def test_only_result_row_contexts_opt_into_menu_chrome(self):
        main = (ROOT / "src/main.cpp").read_text(encoding="utf-8")
        self.assertCountEqual(re.findall(r'\.beginContextItem\("([^"]+)"', main),
                              ["##BrowserRowCtx", "RowContextMenu"])
        self.assertRegex(main, r'beginContextItem\("##BrowserRowCtx", !entry\.isDirectory\)')
        self.assertIn('!g_playlistQuickDrag.active && rowMenuShell.beginContextItem("RowContextMenu")', main)
        self.assertIn('ImGui::BeginPopupContextItem("##PlaylistSidebarContext")', main)

    def test_shared_surface_is_a_flat_theme_color(self):
        theme = (ROOT / "src/Theme.cpp").read_text(encoding="utf-8")
        self.assertIn("ThemeColors::menuBarFillBG(scheme)", theme)
        for custom_paint in ("DrawMenuPopupSurface", "ShadeVertsLinearColorGradientKeepAlpha"):
            self.assertNotIn(custom_paint, theme)
        self.assertIn("MenuChromeGetPopupColors", theme)

    def test_popup_outer_border_softens_without_changing_separator(self):
        theme = (ROOT / "src/Theme.cpp").read_text(encoding="utf-8")
        colors = theme.split("void GetFlatMenuPopupColors(", 1)[1].split("\n}", 1)[0]
        popup = theme.split("ThemeMenuPopupStyleScope::ThemeMenuPopupStyleScope(", 1)[1]
        popup = popup.split("ThemeMenuPopupShellScope::ThemeMenuPopupShellScope(", 1)[0]
        self.assertIn("const ImVec4 hairline = ThemeColors::popupBorder(scheme);", popup)
        self.assertIn("pushColor(ImGuiCol_Separator, hairline);", popup)
        self.assertIn(
            "border = MixThemeColor(background, ClampThemeColor(ThemeColors::popupBorder(scheme)), 0.39f);",
            colors,
        )

    def test_bar_spacing_reserves_one_slot_per_visible_title(self):
        main = (ROOT / "src/main.cpp").read_text(encoding="utf-8")
        bar = main.split("if (ImGui::BeginMainMenuBar()) {", 1)[1].split("ImGui::BeginMenu(L(Menu_File))", 1)[0]
        self.assertIn("CalcTextSize(title, nullptr, true)", bar)
        self.assertIn("brandLeading", bar)
        self.assertIn("brandTrailing", bar)
        self.assertNotIn("2.0f * IM_ARRAYSIZE(menuTitles)", bar)
        self.assertRegex(
            bar,
            r"const float menuSpacing = std::clamp\(\s*"
            r"std::floor\(\(menuAvailable - labelWidth\) / spacingSlots\), 0\.0f, menuStyle\.ItemSpacing\.x\);",
        )

    def test_popup_row_spacing_is_derived_after_the_menu_font_is_pushed(self):
        theme = (ROOT / "src/Theme.cpp").read_text(encoding="utf-8")
        popup = theme.split("ThemeMenuPopupStyleScope::ThemeMenuPopupStyleScope(", 1)[1]
        popup = popup.split("ThemeMenuPopupShellScope::ThemeMenuPopupShellScope(", 1)[0]
        self.assertIn("std::max(27.0f, textSizePx + 8.0f) * scale", popup)
        self.assertIn("rowPitch - ImGui::GetTextLineHeight()", popup)
        self.assertLess(popup.index("PushMenuTextFont("), popup.index("ImGui::GetTextLineHeight()"))

    def test_all_bar_titles_share_one_spacing_scope(self):
        main = (ROOT / "src/main.cpp").read_text(encoding="utf-8")
        chrome = main.split("// TOP MENU BAR", 1)[1].split("// TOP TOOLBAR", 1)[0]
        push = "ImGui::PushStyleVarX(ImGuiStyleVar_ItemSpacing,"
        self.assertEqual(chrome.count(push), 1)
        markers = (
            f"{push} menuSpacing);",
            "ImGui::BeginMenu(L(Menu_File))",
            "RenderToolsMenu(state);",
            "ImGui::BeginMenu(L(Menu_Settings))",
            "ImGui::BeginMenu(L(Menu_Help))",
            "menuChromeHelp.reset();",
            "ImGui::EndMainMenuBar();",
        )
        positions = [chrome.index(marker) for marker in markers]
        self.assertEqual(positions, sorted(positions))
        before_settings = chrome.split("RenderToolsMenu(state);", 1)[1].split("ImGui::BeginMenu(L(Menu_Settings))", 1)[0]
        self.assertNotIn("ImGui::SetCursorPosX(", before_settings)
        self.assertRegex(
            chrome,
            r"menuChromeHelp\.reset\(\);\s*ImGui::EndMenu\(\);\s*\}\s*ImGui::PopStyleVar\(\);",
        )

    def test_bar_spacing_uses_the_legacy_scaled_title_gap(self):
        theme = (ROOT / "src/Theme.cpp").read_text(encoding="utf-8")
        bar = theme.split("ThemeMenuBarStyleScope::ThemeMenuBarStyleScope(", 1)[1].split("\n}", 1)[0]
        self.assertIn("const float legacyTitleSpacing = ImGui::GetStyle().ItemSpacing.x * 2.0f;", bar)
        self.assertIn("pushVar2(ImGuiStyleVar_ItemSpacing, ImVec2(legacyTitleSpacing,", bar)

    def test_settings_popup_uses_shared_row_spacing_and_keeps_commands(self):
        main = (ROOT / "src/main.cpp").read_text(encoding="utf-8")
        chrome = main.split("// TOP MENU BAR", 1)[1].split("// TOP TOOLBAR", 1)[0]
        settings = chrome.split("ImGui::BeginMenu(L(Menu_Settings))", 1)[1].split("ImGui::EndMenu();", 1)[0]
        self.assertNotIn("legacyRowPitch", settings)
        self.assertNotIn("ImGuiStyleVar_ItemSpacing", settings)
        self.assertNotIn("ImGui::PopStyleVar();", settings)
        markers = (
            "ThemeMenuPopupStyleScope menuChromeSettings(",
            "_readingMenuSettings;",
            "ImGui::MenuItem(L(Set_AppSettings))",
            "GatedMenuItem(L(Set_ShortcutSettings), ProductEdition::Edition::Ultimate)",
            "GatedMenuItem(L(Ctx_ConfigureDAW), ProductEdition::Edition::Pro)",
            "menuChromeSettings.reset();",
        )
        positions = [settings.index(marker) for marker in markers]
        self.assertEqual(positions, sorted(positions))

    def test_popup_styles_unwind_before_end_menu(self):
        main = (ROOT / "src/main.cpp").read_text(encoding="utf-8")
        chrome = main.split("// TOP MENU BAR", 1)[1].split("// TOP TOOLBAR", 1)[0]
        scopes = []
        for line in chrome.splitlines():
            if "ImGui::BeginMenu(" in line:
                scopes.append(None)
            scope = re.search(r"ThemeMenuPopupStyleScope (\w+)\(", line)
            if scope:
                scopes[-1] = scope.group(1)
            if "ImGui::EndMenu();" in line:
                expected = scopes.pop()
                self.assertIsNotNone(expected)
                self.assertIn(f"{expected}.reset();", previous)
            previous = line
        self.assertEqual(scopes, [])
        self.assertLess(chrome.index("menuPopupShell.reset();"), chrome.index("menuBarChrome.reset();"))
        self.assertLess(chrome.index("menuBarChrome.reset();"), chrome.index("ImGui::EndMainMenuBar();"))

    def test_menu_host_follows_the_fitted_regular_face_but_popup_does_not(self):
        main = (ROOT / "src/main.cpp").read_text(encoding="utf-8")
        theme = (ROOT / "src/Theme.cpp").read_text(encoding="utf-8")
        chrome = main.split("// TOP MENU BAR", 1)[1].split("// MAIN LAYOUT", 1)[0]
        self.assertIn("ImFont* menuFont = GetUIFontRegular();", chrome)
        self.assertIn("constexpr float minMenuBarFontPx = kChromeFontPx - 1.0f;", chrome)
        self.assertIn("baseMenuFontPx == kChromeFontPx ? minMenuBarFontPx : baseMenuFontPx", chrome)
        self.assertIn("menuBarFontPx -= 1.0f", chrome)
        self.assertIn("std::max(static_cast<float>(kChromeFontPx), menuBarFontPx) * menuScale", chrome)
        self.assertLess(chrome.index("GetMenuChromeFontPushSize(menuFont, hostFontPx)"),
                        chrome.index("ImGui::BeginMainMenuBar()"))
        self.assertIn("ThemeMenuPopupShellScope menuPopupShell(true, baseMenuFontPx);", chrome)
        self.assertIn("kMenuPopupMinTextPx = 15.0f;", theme)
        self.assertIn("std::max(kMenuPopupMinTextPx, textSizePx)", theme)
        self.assertIn("s_menuPopupTextSizePx = m_previousTextSizePx;", theme)
        self.assertIn("viewportBuild->BuildWorkInsetMin.y", main.split("// MAIN LAYOUT", 1)[1])

    def test_default_wordmark_uses_approved_theme_art(self):
        main = (ROOT / "src/main.cpp").read_text(encoding="utf-8")
        bar = main.split("if (ImGui::BeginMainMenuBar()) {", 1)[1].split("ImGui::BeginMenu(L(Menu_File))", 1)[0]
        default_wordmark = bar.split("if (baseMenuFontPx == kChromeFontPx) {", 1)[1].split("\n            }", 1)[0]
        self.assertIn("barLuma >= 0.5f", default_wordmark)
        for color in ("black", "white"):
            self.assertIn(f"harken-search-menu-wordmark-{color}.png", default_wordmark)
        self.assertIn("aura::ui::DrawMenuBarBrandWordmark(", bar)

    def test_isolated_menu_diagnostics_track_actual_font_and_all_eight_titles_once(self):
        main = (ROOT / "src/main.cpp").read_text(encoding="utf-8")
        module = (ROOT / "src/ModuleUI.cpp").read_text(encoding="utf-8")
        windows = (ROOT / "src/WindowsMain.cpp").read_text(encoding="utf-8")
        chrome = main.split("// TOP MENU BAR", 1)[1].split("// MAIN LAYOUT", 1)[0]
        self.assertCountEqual(re.findall(r"QaProbeMenuBarItem\(state, L\(Menu_(\w+)\)\)", chrome),
                              ["File", "Edit", "View", "Database", "Playlist", "Tools", "Settings", "Help"])
        self.assertNotIn("QaProbeMenuBarItem(", module.split("void RenderToolsMenu(", 1)[1].split("// 1)", 1)[0])
        self.assertIn("state.qaRegions.menuBarFontPixels = ImGui::GetFontSize();", chrome)
        self.assertIn("GetMenuPopupFontPixelsForQa(*w)", windows)

    def test_isolated_folder_pointer_accepts_hover_and_requires_matching_button_release(self):
        windows = (ROOT / "src/WindowsMain.cpp").read_text(encoding="utf-8")
        pointer = windows.split('cmd == "folder_search_pointer"', 1)[1].split('"invalid_folder_search_pointer', 1)[0]
        self.assertIn("MagicEqQaIsolationReady()", pointer)
        self.assertIn('((phase == "hover" || phase == "down") && s_magicEqQaPointer.held)', pointer)
        self.assertRegex(pointer, r'phase == "up" && [^\n]*!s_magicEqQaPointer\.active\s*\|\|\s*'
                                  r'!s_magicEqQaPointer\.held\s*\|\|\s*s_magicEqQaPointer\.button != button')

    def test_result_popup_body_styles_unwind_before_end_popup(self):
        main = (ROOT / "src/main.cpp").read_text(encoding="utf-8")
        for popup_id, chrome, shell in (("##BrowserRowCtx", "browserMenuChrome", "browserMenuShell"),
                                        ("RowContextMenu", "rowMenuChrome", "rowMenuShell")):
            with self.subTest(popup_id=popup_id):
                marker = f'{shell}.beginContextItem("{popup_id}"'
                self.assertTrue(marker in main, f"{popup_id} must use the shared menu shell")
                body = main.split(marker, 1)[1]
                body, end = body.split("ImGui::EndPopup();", 1)
                self.assertRegex(body, rf"ThemeMenuPopupStyleScope {chrome}\(")
                self.assertTrue(body.rstrip().endswith(f"{chrome}.reset();"))
                self.assertRegex(end, rf"^\s*}}\s*{shell}\.reset\(\);")

    def test_appended_tools_popup_unwinds_before_end_menu(self):
        module = (ROOT / "src/ModuleUI.cpp").read_text(encoding="utf-8")
        tools = module.split("void RenderToolsMenu(", 1)[1].split("// 1)", 1)[0]
        self.assertRegex(tools, r"ThemeMenuPopupStyleScope menuChromeTools\b")
        self.assertRegex(tools, r"menuChromeTools\.reset\(\);\s*ImGui::EndMenu\(\);",
                         "Tools popup style must unwind before ImGui ends the appended window")


if __name__ == "__main__":
    unittest.main()
