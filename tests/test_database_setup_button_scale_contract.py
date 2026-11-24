# -*- coding: utf-8 -*-
"""首次建库弹窗：按钮高度跟着字体、窄窗分行、圆角跟着界面倍率，默认倍率不变。"""

from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]


class TestDatabaseSetupButtonScaleContract(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.main = (ROOT / "src" / "main.cpp").read_text(encoding="utf-8")
        cls.layout = (ROOT / "src" / "ui" / "DatabaseSetupButtonLayout.h").read_text(encoding="utf-8")
        cls.action = (ROOT / "src" / "ui" / "ActionButtonStyle.h").read_text(encoding="utf-8")
        cls.widgets = (ROOT / "src" / "HarkenSearchWidgets.cpp").read_text(encoding="utf-8")
        cls.app_state = (ROOT / "src" / "AppState.h").read_text(encoding="utf-8")

    def test_buttons_never_hardcode_a_physical_height(self) -> None:
        # 32 物理像素在应用内 125% 时比字还矮，字底会被裁。
        self.assertNotIn("ImVec2(setupButtonWidth, 32)", self.main)
        self.assertNotIn("ImVec2(72.0f, 32)", self.main)
        self.assertIn("aura::ui::setup::ResolveRow(", self.main)
        for name in ("setupRow.create", "setupRow.open", "setupRow.exit"):
            self.assertIn(name, self.main)

    def test_height_follows_the_line_box_with_the_design_floor(self) -> None:
        self.assertIn("ButtonHeight(scale, frameHeight)", self.layout)
        self.assertIn("scaled > frameHeight ? scaled : frameHeight", self.layout)
        self.assertIn("kButtonHeight = 32.0f", self.layout)

    def test_narrow_dialog_stacks_one_button_per_row(self) -> None:
        self.assertIn("needed <= availableWidth", self.layout)
        self.assertIn("row.stacked = true;", self.layout)
        # 一行时共用宽度只在不压到标签时才用，否则各取自己的最小宽度。
        self.assertIn("if (shared >= minCreate && shared >= minOpen)", self.layout)
        self.assertIn("row.create = ImVec2(minCreate, height);", self.layout)
        self.assertIn("requireDatabaseSetup && setupRow.stacked", self.main)
        self.assertIn("if (!stackSetupButtons) ImGui::SameLine();", self.main)

    def test_dialog_box_follows_the_interface_scale(self) -> None:
        self.assertIn("aura::ui::dpi::Px(560.0f)", self.main)
        self.assertIn("aura::ui::dpi::Px(106.0f)", self.main)
        self.assertIn("aura::ui::dpi::Px(32.0f)", self.main)

    def test_button_rounding_scales_with_the_same_factor(self) -> None:
        # 按钮 6 px 是绝对值，弹窗圆角却随 chromeScale 放大，4K 下会一个越方、一个越圆。
        self.assertIn("inline float ScaledRounding()", self.action)
        self.assertIn("ImGuiStyleVar_FrameRounding, ScaledRounding()", self.action)
        self.assertNotIn("ImGuiStyleVar_FrameRounding, kRounding", self.action)
        self.assertIn("ActionButtonStyle::ScaledRounding()", self.widgets)

    def test_dialog_rounding_uses_the_same_scale(self) -> None:
        self.assertIn("ImGuiStyleVar_WindowRounding, aura::ui::dpi::Px(kRadiusDialog)", self.widgets)
        self.assertIn("ImGuiStyleVar_PopupRounding, aura::ui::dpi::Px(kRadiusDialog)", self.widgets)
        self.assertIn("aura::ui::dpi::Px(kRadiusDialog) - kCaptionBorder", self.widgets)

    def test_interface_scale_still_defaults_to_100(self) -> None:
        # 修裁字不靠改默认倍率：显式与未设置的用户都应保持 100%。
        self.assertIn("int uiScalePercent = 100;", self.app_state)
        self.assertIn('ParseInt(kv["uiScalePercent"], 100)', self.main)


if __name__ == "__main__":
    unittest.main()
