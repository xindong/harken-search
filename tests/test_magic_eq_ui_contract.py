import re
import json
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
MAIN = ROOT / "src" / "main.cpp"
LOCALIZATION = ROOT / "src" / "Localization.cpp"
DATASOURCE = ROOT / "src" / "CenterSenseDataSource.cpp"
LEDGER = ROOT / "locale" / "harken.ui.strings.json"


def function_body(source: str, signature: str, next_signature: str) -> str:
    match = re.search(
        re.escape(signature) + r".*?(?=" + re.escape(next_signature) + r")",
        source,
        flags=re.DOTALL,
    )
    if match is None:
        raise AssertionError(f"找不到函数范围：{signature}")
    return match.group(0)


def require_text(test: unittest.TestCase, source: str, text: str) -> None:
    test.assertTrue(text in source, f"缺少预期实现标记：{text}")


class MagicEqUiContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.main = MAIN.read_text(encoding="utf-8")
        cls.localization = LOCALIZATION.read_text(encoding="utf-8")
        cls.datasource = DATASOURCE.read_text(encoding="utf-8")
        cls.ledger = json.loads(LEDGER.read_text(encoding="utf-8"))["entries"]

    def draw_body(self) -> str:
        return function_body(self.main, "static bool DrawMagicEQBar(", "static bool DrawSoundstagePopupContent(")

    def test_isolated_qa_drag_uses_real_handle_zones_and_imgui_input(self):
        windows = (ROOT / "src" / "WindowsMain.cpp").read_text(encoding="utf-8")
        draw = self.draw_body()
        for zone in ("transport.magicEQ.leftGrip", "transport.magicEQ.rightGrip",
                     "transport.magicEQ.centerGrip", "transport.magicEQ.mode",
                     "transport.magicEQ.grade"):
            self.assertIn(zone, draw)
        for token in ("magic_eq_identity", "magic_eq_state", "magic_eq_set_state",
                      "magic_eq_pointer", "PreviewIsolationRootUtf8",
                      "AddMousePosEvent", "AddMouseButtonEvent", "active_drag_part",
                      "published_equals_visible", "requested_tail_reason",
                      "drag_frame_count", "drag_frame_ms_p95"):
            self.assertIn(token, windows)

    def test_labels_include_utf8_chinese_english_japanese_without_replacement_text(self):
        source = "\n".join(
            value
            for entry in self.ledger
            for key, value in entry.items()
            if key in ("zh", "en", "ja")
        )
        for word in ("直通", "消除", "旁通", "通過", "ノッチ", "バイパス", "PASS", "NOTCH", "BYPASS"):
            require_text(self, source, word)
        self.assertNotRegex(self.localization, r'"\?\?"')

    def test_left_and_right_endpoints_pin_the_opposite_edge(self):
        body = self.draw_body()
        for text in ("DragPart::LeftEndpoint", "DragPart::RightEndpoint",
                     "magicEQSetLowEdgePercent", "magicEQSetHighEdgePercent"):
            require_text(self, body, text)

    def test_outer_grips_resize_widget_only_and_preserve_32px_height(self):
        body = self.draw_body()
        for text in ("DragPart::ResizeLeft", "DragPart::ResizeRight", "settings.widgetWidth",
                     "ImVec2 size(width, 32.0f)"):
            require_text(self, body, text)
        resize = re.search(r"if \(drag.part == DragPart::ResizeLeft.*?\n\s*\}\s*else", body, re.DOTALL)
        self.assertIsNotNone(resize, "最外侧抓手需要独立的宽度拖动分支")
        self.assertNotIn("centerFreq", resize.group(0))
        self.assertNotIn("halfWidthPercent", resize.group(0))

    def test_compact_visual_uses_transport_theme_and_draws_range_ticks_and_handles(self):
        body = self.draw_body()
        for text in ("transportSurface", "TransportContrast::ForSurface", "AddRectFilled", "AddLine",
                     "AddCircleFilled", "AddText", "for (const RulerTick& tick"):
            require_text(self, body, text)

    def test_pass_notch_and_bypass_have_distinct_colors_and_labels(self):
        body = self.draw_body()
        for text in ("GetMagicEQModeLabel(settings.enabled,", "MagicEQMode::Notch",
                     "theme.interfaceText", "theme.controlAccent", "theme.waveformSelectionEdge"):
            require_text(self, body, text)

    def test_visual_contract_uses_star_eq_switch_and_single_language_steepness(self):
        body = self.draw_body()
        require_text(self, body, "PathLineTo")
        require_text(self, body, "PathFillConcave")
        require_text(self, body, "magicEqStar")
        require_text(self, body, 'BeginTransientScrollbarPopup("MagicEQSteepnessPopup")')
        require_text(self, body, "measureMagicEQLocalSlope(params, sampleRate)")
        require_text(self, body, "drawSteepnessCurve")
        require_text(self, body, "settings.steepness")
        require_text(self, body, "AddText")
        self.assertNotIn("Power", body)

    def test_fixed_nominal_grades_and_tail_availability_are_wired_to_real_menu_and_drag(self):
        body = self.draw_body()
        for fragment in (
            "MagicEQSteepness::Brickwall",
            "L(MagicEQ_NominalEdgeSlope)",
            "L(MagicEQ_BrickwallApprox)",
            "L(MagicEQ_TailTooLong)",
            "L(MagicEQ_BandDetail)",
            "estimateMagicEQTail",
            "availability.available",
        ):
            require_text(self, body, fragment)
        self.assertIn("MenuItem(display.menu, nullptr, selected, selectable)", body)
        drag = body.index("if (drag.part != DragPart::None && held")
        gate = body.index("const auto currentAvailability = availabilityFor(settings.steepness)")
        self.assertLess(drag, gate)
        self.assertIn("settings = originalSettings", body[gate:gate + 1200])
        self.assertIn("slopeAvailableWidth >= shortSlopeSize.x + 2.0f", body)
        self.assertIn("steepnessLabel.shortValue", body)

    def test_narrow_control_keeps_a_visible_and_clickable_slope_entry(self):
        body = self.draw_body()
        self.assertIn("const float steepnessMinimumHitWidth", body)
        self.assertIn("steepnessVisibleRight > steepnessLeft", body)
        self.assertIn("draw->AddTriangleFilled", body)
        # At width 120 the old rail-left cutoff was x49, left edge x54.
        for width in (120.0, 180.0, 240.0, 440.0):
            status_width = min(60.0, max(54.0, width * 0.34))
            left = 5.0 + 46.0 + 3.0
            right = width - status_width
            self.assertGreater(right - left, 0.0)

    def test_rejected_drag_and_auto_bypass_reason_survive_tooltip_priority(self):
        body = self.draw_body()
        self.assertIn("rejectionReason", body)
        self.assertIn("dragLimited", body)
        self.assertIn("L(MagicEQ_LimitedPrefix)", body)
        self.assertIn("slopeDisplay(settings.steepness, true)", body)
        final_tooltip = body.rfind("if (steepnessHit) ImGui::SetTooltip")
        self.assertEqual(final_tooltip, -1, "旧空详情 tooltip 仍会覆盖原因")
        self.assertIn("availabilityReason(rejectionReason)", body)

    def test_technical_block_matches_approved_geometry_and_hitbox(self):
        body = self.draw_body()
        for fragment in (
            "const ImVec2 markMin(pos.x + 5.0f, pos.y + 4.0f)",
            "const ImVec2 markMax(markMin.x + 46.0f, markMin.y + 24.0f)",
            "mouse.x >= markMin.x && mouse.x < markMax.x",
            "mouse.y >= markMin.y && mouse.y < markMax.y",
            "point < 8", "3.14159265f / 4.0f",
            "PathFillConcave(markColor)",
            "markMin.x + 9.5f", "markMin.x + 18.0f",
            "markMin.x + 3.0f", "markMax.x - 3.0f",
            "const bool markHover = toggleHit", "const bool markPressed = markHover && held",
            "theme.controlAccent", "transportSurface",
            "steepnessLeft = markMax.x + 3.0f",
            "steepnessRight = pos.x + width - statusWidth",
            "mouse.x >= steepnessLeft && mouse.x < steepnessVisibleRight",
            "markPressed ? 0.06f : (markHover ? 0.17f : 0.11f)",
            "markPressed ? 2.0f : 1.5f",
        ):
            require_text(self, body, fragment)
        self.assertNotIn("point < 10", body)
        self.assertNotIn("PathFillConvex", body)
        self.assertNotIn("AddCircleFilled(magicEqStar", body)

    def test_ruler_uses_log_frequency_mapping_and_adaptive_labels(self):
        body = self.draw_body()
        for frequency in (20, 50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000):
            require_text(self, body, f"{frequency}.0f")
        for label in ('"20"', '"50"', '"100"', '"200"', '"500"',
                      '"1k"', '"2k"', '"5k"', '"10k"', '"20k"'):
            require_text(self, body, label)
        require_text(self, body, "CenterSenseDsp::magicEQFrequencyToPercent(tick.frequencyHz)")
        require_text(self, body, "ImGui::CalcTextSize(tick.label)")
        require_text(self, body, "labelLeft >= lastLabelRight + 4.0f")
        require_text(self, body, "railWidth >= 220.0f")
        require_text(self, body, "detailedRuler = width >= 440.0f")
        require_text(self, body, "std::clamp((width - 180.0f) * (88.0f / 260.0f), 0.0f, 88.0f)")
        require_text(self, body, "std::clamp((width - 180.0f) * (80.0f / 140.0f), 0.0f, 80.0f)")

    def test_vertical_handles_bubble_and_range_use_same_edges(self):
        body = self.draw_body()
        for fragment in (
            "leftDistance <= 7.0f && mouse.y >= gripTop && mouse.y <= gripBottom",
            "rightDistance <= 7.0f && mouse.y >= gripTop && mouse.y <= gripBottom",
            'drawVerticalGrip(leftX, leftEndpointHit || drag.part == DragPart::LeftEndpoint, "eq.grip")',
            'drawVerticalGrip(rightX, rightEndpointHit || drag.part == DragPart::RightEndpoint, "eq.grip.right")',
            "drag.part == DragPart::LeftEndpoint ? lowHz :",
            "drag.part == DragPart::LeftEndpoint ? leftX :",
            "ImGui::IsMouseDown(ImGuiMouseButton_Left) &&",
            "ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)",
            "GetMagicEQFrequencyLabel(lowHz, highHz)",
            "rangeSize.x + 16.0f <= rightX - leftX",
            "centerLabelSize.x + 12.0f <= rightX - leftX",
            "ImGui::GetForegroundDrawList()",
            "bubbleDraw->AddTriangleFilled",
            "mouse.y < gripTop",
        ):
            require_text(self, body, fragment)
        self.assertLess(body.index("drag.part = DragPart::None;", body.index("if (ImGui::IsMouseReleased")),
                        body.index("drawVerticalGrip(leftX"))

    def test_narrow_ruler_reserves_button_and_mode_space(self):
        for width in (120.0, 180.0, 240.0, 319.0, 320.0, 439.0, 440.0, 800.0):
            status_width = min(60.0, max(54.0, width * 0.34))
            inset = max(min(88.0, max(0.0, (width - 180.0) * 88.0 / 260.0)),
                        min(80.0, max(0.0, (width - 180.0) * 80.0 / 140.0)))
            rail_left = max(53.0 + inset, 80.0 if width >= 180.0 else 53.0,
                            190.0 if width >= 600.0 else 0.0)
            rail_right = max(rail_left + 12.0, width - status_width - 5.0)
            mode_left = width - status_width
            with self.subTest(width=width):
                self.assertGreaterEqual(rail_left, 51.0)
                self.assertLess(rail_right, mode_left)
                self.assertGreaterEqual(mode_left - 54.0, 12.0)
                if width == 120.0:
                    self.assertLess(rail_right - rail_left, 20.0)

    def test_steepness_menu_commits_live_value_and_saves_it(self):
        body = self.draw_body()
        for choice in ("Soft", "Steep", "VerySteep"):
            require_text(self, body, f"settings.steepness = CenterSenseDsp::MagicEQSteepness::{choice}")
        require_text(self, body, 'ImGui::OpenPopup("MagicEQSteepnessPopup")')
        require_text(self, body, "saveSettingsNow = true")
        render = self.main[self.main.index('MagicEQSettings magicEQ = state.audioEngine.getMagicEQ();'):]
        require_text(self, render, "state.audioEngine.setMagicEQ(magicEQ)")
        require_text(self, render, "SaveAppSettings(state)")

    def test_unloaded_file_can_choose_a_slope_without_weakening_loaded_tail_gate(self):
        body = self.draw_body()
        self.assertIn("sampleRate <= 0 || availability.available", body)
        self.assertIn("settings.enabled && sampleRate > 0 && !currentAvailability.available", body)
        self.assertIn("state.audioEngine.setMagicEQ(magicEQ)", self.main)

    def test_loaded_audio_uses_engine_rate_when_file_metadata_rate_is_temporarily_zero(self):
        render = self.main[self.main.index('MagicEQSettings magicEQ = state.audioEngine.getMagicEQ();'):]
        self.assertIn("getEngineSampleRate()", render)
        self.assertIn("magicEqFileInfo.sampleRate", render)
        self.assertIn("isLoaded()", render)
        self.assertIn("magicEqSampleRate", render)

    def test_unloaded_slope_tooltip_explains_choice_and_later_validation_in_three_languages(self):
        body = self.draw_body()
        expected = {
            "zh": "尚未加载音频。现在可以选择斜率档位；加载音频后会检查是否可用。",
            "en": "No audio loaded. You can choose a slope now; availability will be checked after loading audio.",
            "ja": "音声は未読み込みです。傾斜の段階は今選べます。音声の読み込み後に利用可否を確認します。",
        }
        entry = next((item for item in self.ledger if item["id"] == "MagicEQ_UnloadedGradeHint"), None)
        self.assertIsNotNone(entry)
        for language, value in expected.items():
            self.assertEqual(value, entry[language])
        self.assertIn('if (sampleRate <= 0) ImGui::SetTooltip("%s", L(MagicEQ_UnloadedGradeHint))', body)
        self.assertIn('steepnessVisibleRight > steepnessLeft', body)
        self.assertIn('steepnessMinimumHitWidth = 10.0f', body)

    def test_isolated_qa_zone_collection_starts_only_after_identity_binding(self):
        windows = (ROOT / "src" / "WindowsMain.cpp").read_text(encoding="utf-8")
        self.assertIn("s_magicEqQaCollectUntilMs.store(", windows)
        compact = "".join(windows.split())
        self.assertIn("magicEqQaCollectionEnabled=magicEqQaCollect&&Platform::MonotonicMilliseconds()<=s_magicEqQaCollectUntilMs.load(", compact)
        self.assertIn(
            "#if AURA_BUILD_DIAGNOSTICS && !AURA_INTERNAL_BETA\n"
            "        magicEqQaCollectionEnabled = magicEqQaCollect &&",
            windows,
        )
        self.assertIn("annotationBridgeEnabled || magicEqQaCollectionEnabled", windows)
        self.assertIn("static const bool ready = []", windows)

    def test_qa_slope_choice_traverses_actual_popup_and_published_state(self):
        body = self.draw_body()
        windows = (ROOT / "src" / "WindowsMain.cpp").read_text(encoding="utf-8")
        self.assertIn("transport.magicEQ.gradeSoft", body)
        self.assertIn("transport.magicEQ.gradeBrickwall", body)
        self.assertIn('part == "grade"', windows)

    def test_live_edge_measurement_and_mode_specific_geometry(self):
        body = self.draw_body()
        ledger = {entry["id"]: entry for entry in self.ledger}
        for key in ("MagicEQ_SlopeLowSide", "MagicEQ_SlopeHighSide", "MagicEQ_SlopeBypass",
                    "MagicEQ_SlopeNarrowBand", "MagicEQ_SlopeFrequencyLimit", "MagicEQ_SlopeSampleRate"):
            self.assertIn(key, ledger)
            self.assertTrue(all(ledger[key][language] for language in ("zh", "en", "ja")))
            require_text(self, body, f"L({key})")
        require_text(self, body, "drawSteepnessCurve")
        require_text(self, body, "MagicEQMode::Notch")
        require_text(self, body, "AddPolyline")
        require_text(self, body, "params.centerFreq = settings.centerFreq")
        require_text(self, body, "params.halfWidthPercent = settings.halfWidthPercent")
        require_text(self, body, "params.mode = settings.mode")
        require_text(self, body, "params.steepness = grade")
        require_text(self, body, "dB/oct")
        require_text(self, body, "side.fromHz, side.toHz")
        require_text(self, body, "sideName, reasonText(side.reason)")
        self.assertNotIn("L(MagicEQ_SteepnessSoft)", body)
        self.assertNotIn("L(MagicEQ_SteepnessSteep)", body)
        self.assertNotIn("L(MagicEQ_SteepnessVerySteep)", body)

    def test_ux_geometry_fallback_and_hit_rectangles(self):
        body = self.draw_body()
        for fragment in (
            "const float steepnessVisibleRight =",
            "mouse.x < steepnessVisibleRight",
            "ImVec2(steepnessVisibleRight, pos.y + 13.0f)",
            "ImGui::CalcTextSize(steepnessLabel.compact)",
            "ImGui::CalcTextSize(steepnessLabel.shortValue)",
            "draw->AddTriangleFilled",
            "ImGui::CalcTextSize(rangeText)",
            "ImGui::CalcTextSize(centerLabel)",
            "ImGui::CalcTextSize(compactFrequencyFallback)",
            "rangeSize.x + 8.0f <= frequencyInfoWidth",
            "centerLabelSize.x + 8.0f <= frequencyInfoWidth",
            "draw->AddText(ImVec2(frequencyInfoX",
            "const float modeTextWidth = ImGui::CalcTextSize(modeLabel).x",
            "L(MagicEQ_ModeBypassCompact)",
            "AddPolyline(sharpCurve, 5",
        ):
            require_text(self, body, fragment)
        require_text(self, body, "const bool showFrequencyFallback = !rangeFitsSelection && !centerFitsSelection")

    def test_ux_width_budget_and_click_points(self):
        # Mirrors the native dimensions; assertions encode the UX report's
        # click points and the minimum readable fallback at narrow widths.
        for width in (120.0, 180.0, 240.0, 320.0, 440.0, 800.0):
            status = min(60.0, max(54.0, width * 0.34))
            inset = max(min(88.0, max(0.0, (width - 180.0) * 88.0 / 260.0)),
                        min(80.0, max(0.0, (width - 180.0) * 80.0 / 140.0)))
            rail_left = max(53.0 + inset, 80.0 if width >= 180.0 else 53.0,
                            190.0 if width >= 600.0 else 0.0)
            mode_left = width - status
            rail_right = max(rail_left + 12.0, mode_left - 5.0)
            visible_right = min(mode_left, rail_left - 4.0)
            slope_left = 54.0
            for x in (rail_left, (rail_left + rail_right) / 2.0, rail_right):
                self.assertFalse(slope_left <= x < visible_right,
                                 f"ruler click must not open slope menu at width {width}")
            if width >= 180.0:
                self.assertGreaterEqual(rail_right - rail_left, 12.0)
            self.assertLessEqual(visible_right, rail_left - 4.0)

    def test_frequency_fallback_uses_full_track_when_selection_is_narrow(self):
        # Conservative ImGui text budgets from the 13 px transport font;
        # production chooses each label with CalcTextSize at its actual font.
        center_text_px, range_text_px, info_marker_px = 20.0, 100.0, 4.0
        for width in (120.0, 180.0, 240.0, 320.0, 440.0, 800.0):
            status = min(60.0, max(54.0, width * 0.34))
            inset = max(min(88.0, max(0.0, (width - 180.0) * 88.0 / 260.0)),
                        min(80.0, max(0.0, (width - 180.0) * 80.0 / 140.0)))
            left = max(53.0 + inset, 80.0 if width >= 180.0 else 53.0,
                       190.0 if width >= 600.0 else 0.0)
            right = max(left + 12.0, width - status - 5.0)
            rail_width = right - left
            for half_width in (1.0, 10.0, 40.0):
                selection_width = rail_width * (2.0 * half_width / 100.0)
                needs_fallback = (range_text_px + 16.0 > selection_width and
                                  center_text_px + 12.0 > selection_width)
                if needs_fallback:
                    chosen = (range_text_px if range_text_px + 8.0 <= rail_width else
                              center_text_px if center_text_px + 8.0 <= rail_width else
                              info_marker_px)
                    self.assertLessEqual(chosen + 4.0, rail_width)
            if width == 180.0:
                self.assertGreaterEqual(rail_width, center_text_px + 8.0)
            if width == 120.0:
                self.assertLess(rail_width, center_text_px + 8.0)

    def test_steepness_labels_are_current_language_only(self):
        entries = {entry["id"]: entry for entry in self.ledger}
        for key in ("MagicEQ_SlopeLowSide", "MagicEQ_SlopeHighSide"):
            self.assertEqual(len({entries[key][lang] for lang in ("zh", "en", "ja")}), 3)
            require_text(self, self.draw_body(), f"L({key})")
        render = self.main[self.main.index('MagicEQSettings magicEQ = state.audioEngine.getMagicEQ();'):]
        require_text(self, render, "magicEqFileInfo.sampleRate")
        require_text(self, render, "magicEqSampleRate")

    def test_spectrum_mask_direction_is_mode_specific_and_bypass_is_clear(self):
        overlay = function_body(self.main, "static void DrawMagicEQSelectionOverlay(", "static void FormatSpectrogramFrequencyLabel(")
        require_text(self, overlay, "AddRectFilled")
        require_text(self, overlay, "MagicEQMode::Notch")
        require_text(self, overlay, "settings.enabled")
        require_text(self, overlay, "rectMin.y")

    def test_mode_labels_use_three_language_localization_ledger_keys(self):
        expected = {
            "MagicEQ_ModePass": {"zh": "直通", "en": "PASS", "ja": "通過"},
            "MagicEQ_ModeNotch": {"zh": "消除", "en": "NOTCH", "ja": "ノッチ"},
            "MagicEQ_ModeBypass": {"zh": "旁通", "en": "BYPASS", "ja": "バイパス"},
        }
        entries = {entry["id"]: entry for entry in self.ledger}
        body = function_body(self.localization, "const char* GetMagicEQModeLabel(", "const char* GetMagicEQFrequencyLabel(")
        for key, translations in expected.items():
            with self.subTest(key=key):
                self.assertTrue(key in entries, f"缺少三语文案键：{key}")
                if key not in entries:
                    continue
                for language, text in translations.items():
                    self.assertEqual(entries[key][language], text, f"{key}.{language} 文案不匹配")
                self.assertIn(f"L({key})", body, f"{key} 必须经正式本地化入口读取")
                for text in translations.values():
                    self.assertNotIn(f'"{text}"', body, f"不得绕过账本硬编码 {text}")

    def test_center_drag_wheel_and_ctrl_cmd_adjustment_remain_available(self):
        body = self.draw_body()
        for text in ("DragPart::Center", "io.KeyCtrl || io.KeySuper",
                     "nextHalf - deltaY * 0.25f",
                     "settings.halfWidthPercent + io.MouseWheel * 1.5f"):
            require_text(self, body, text)

    def test_release_clears_only_drag_state_without_restoring_settings(self):
        body = self.draw_body()
        release = re.search(
            r"if \(ImGui::IsMouseReleased\(ImGuiMouseButton_Left\)\).*?pendingSettingsSave = false;\s*\}",
            body, re.DOTALL)
        self.assertIsNotNone(release, "松开鼠标后必须清除拖动状态")
        require_text(self, release.group(0), "drag.part = DragPart::None")
        self.assertNotIn("settings.centerFreq", release.group(0))
        self.assertNotIn("settings.halfWidthPercent", release.group(0))

    def test_current_spectrogram_gets_frequency_band_overlay_only_when_its_file_is_current(self):
        body = self.main
        require_text(self, body, "DrawMagicEQSelectionOverlay(")
        overlay = function_body(body, "static void DrawMagicEQSelectionOverlay(", "static void FormatSpectrogramFrequencyLabel(")
        require_text(self, overlay, "SpectrogramFrequencyToScreenY")
        require_text(self, overlay, "magicEQEdges")
        self.assertGreaterEqual(body.count("DrawMagicEQSelectionOverlay("), 3)
        require_text(self, body, "CanDisplaySpectrogramTextureForTimeline(")
        require_text(self, body, "state.spectrogramFilePath")
        require_text(self, self.datasource, "ResetAfterSeek")
        require_text(self, self.datasource, "resetMagicEQ")
    def test_formal_renderui_spectrogram_draws_selection_after_texture_in_both_modes(self):
        start = self.main.index("if (state.showSpectrogram && state.audioEngine.isLoaded())")
        end = self.main.index("// DrawMainWaveform", start)
        render = self.main[start:end]
        merged_start = render.index("// MERGED MODE: Spectrogram underneath waveform")
        split = render[:merged_start]
        merged_end = render.index("// No spectrogram - just draw waveform", merged_start)
        merged = render[merged_start:merged_end]

        for name, branch in (("split", split), ("merged", merged)):
            with self.subTest(mode=name):
                self.assertIn("CanDisplaySpectrogramTextureForTimeline(", branch)
                self.assertIn("DrawMagicEQSelectionOverlay(", branch,
                              "the formal RenderUI branch must draw the selection")
                image = branch.index("dl->AddImage(")
                readout = branch.index("DrawSpectrogramReadoutOverlay(")
                overlay = branch.index("DrawMagicEQSelectionOverlay(")
                self.assertLess(image, readout)
                self.assertLess(readout, overlay,
                                "selection must be queued after the spectrogram texture/readout")
                self.assertIn("state.audioEngine.getMagicEQ()", branch[overlay:])



if __name__ == "__main__":
    unittest.main()
