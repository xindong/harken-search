# -*- coding: utf-8 -*-
"""首次建库弹窗：按钮高度跟着字体、窄窗分行、圆角跟着界面倍率，默认倍率不变。"""

from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MAIN = (ROOT / "src" / "main.cpp").read_text(encoding="utf-8")
LAYOUT = (ROOT / "src" / "ui" / "DatabaseSetupButtonLayout.h").read_text(encoding="utf-8")
ACTION = (ROOT / "src" / "ui" / "ActionButtonStyle.h").read_text(encoding="utf-8")
WIDGETS = (ROOT / "src" / "HarkenSearchWidgets.cpp").read_text(encoding="utf-8")
APP_STATE = (ROOT / "src" / "AppState.h").read_text(encoding="utf-8")


class DatabaseSetupButtonScaleContract:
    def test_buttons_never_hardcode_a_physical_height(self) -> None:
        # 32 物理像素在应用内 125% 时比字还矮，字底会被裁。
        assert "ImVec2(setupButtonWidth, 32)" not in MAIN
        assert "ImVec2(72.0f, 32)" not in MAIN
        assert "aura::ui::setup::ResolveRow(" in MAIN
        assert "setupRow.create" in MAIN
        assert "setupRow.open" in MAIN
        assert "setupRow.exit" in MAIN

    def test_height_follows_the_line_box_with_the_design_floor(self) -> None:
        assert "ButtonHeight(scale, frameHeight)" in LAYOUT
        assert "scaled > frameHeight ? scaled : frameHeight" in LAYOUT
        assert "kButtonHeight = 32.0f" in LAYOUT

    def test_narrow_dialog_stacks_one_button_per_row(self) -> None:
        assert "needed <= availableWidth" in LAYOUT
        assert "row.stacked = true;" in LAYOUT
        assert "createWidth = openDbWidth = exitWidth" not in MAIN
        assert "requireDatabaseSetup && setupRow.stacked" in MAIN
        assert "if (!stackSetupButtons) ImGui::SameLine();" in MAIN

    def test_dialog_box_follows_the_interface_scale(self) -> None:
        assert "aura::ui::dpi::Px(560.0f)" in MAIN
        assert "aura::ui::dpi::Px(106.0f)" in MAIN
        assert "aura::ui::dpi::Px(32.0f)" in MAIN

    def test_button_rounding_scales_with_the_same_factor(self) -> None:
        # 按钮 6 px 是绝对值，弹窗圆角却随 chromeScale 放大，4K 下会一个越方、一个越圆。
        assert "inline float ScaledRounding()" in ACTION
        assert "ImGuiStyleVar_FrameRounding, ScaledRounding()" in ACTION
        assert "ImGuiStyleVar_FrameRounding, kRounding" not in ACTION
        assert "ActionButtonStyle::ScaledRounding()" in WIDGETS

    def test_dialog_rounding_uses_the_same_scale(self) -> None:
        assert "ImGuiStyleVar_WindowRounding, aura::ui::dpi::Px(kRadiusDialog)" in WIDGETS
        assert "ImGuiStyleVar_PopupRounding, aura::ui::dpi::Px(kRadiusDialog)" in WIDGETS
        assert "aura::ui::dpi::Px(kRadiusDialog) - kCaptionBorder" in WIDGETS

    def test_interface_scale_still_defaults_to_100(self) -> None:
        # 修裁字不靠改默认倍率：显式与未设置的用户都应保持 100%。
        assert "int uiScalePercent = 100;" in APP_STATE
        assert 'ParseInt(kv["uiScalePercent"], 100)' in MAIN
