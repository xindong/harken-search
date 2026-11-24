"""RQ-20260920-02 follow-up: results/waveform boundary splitter contract."""

from __future__ import annotations

import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MAIN = ROOT / "src" / "main.cpp"


def _read() -> str:
    return MAIN.read_text(encoding="utf-8")


class WaveformSplitterContract(unittest.TestCase):
    def test_waveform_boundary_has_dedicated_splitter_id(self) -> None:
        self.assertIn('HorizontalSplitter("##WaveformSplitter"', _read())

    def test_splitter_binds_only_waveform_docked_height(self) -> None:
        main = _read()
        block = main.split("// Three-dot results/waveform boundary handle.", 1)[1].split(
            "const float waveformDockHeight", 1)[0]
        self.assertIn("waveformSplitHeight = workspacePx(state.panelWaveform.dockedHeight);", block)
        self.assertIn('HorizontalSplitter("##WaveformSplitter", &waveformSplitHeight,', block)
        self.assertIn("state.panelWaveform.dockedHeight = waveformSplitHeight / workspaceScale;", block)
        self.assertIn(
            "state.settings.waveformPanelHeight = state.panelWaveform.dockedHeight;",
            block,
        )
        self.assertIn("MarkSettingsDirty(state);", block)

    def test_height_budget_consumes_waveform_docked_height(self) -> None:
        main = _read()
        self.assertIn("desiredWaveformH", main)
        budget = main.split("desiredWaveformH", 1)[1][:300]
        self.assertIn("state.panelWaveform.dockedHeight", budget)

    def test_docked_waveform_budgets_only_the_spacing_used_by_its_splitter(self) -> None:
        main = _read()
        start = main.index("const bool showDockedWaveform =")
        end = main.index("const float minimumWaveform", start)
        budget = main[start:end]
        self.assertIn("WorkspacePanelGapPx() - 2.0f * waveShellPad", budget)
        self.assertIn("waveformPanelGapReserve + statusBottomLayoutReserve", budget)
        self.assertIn("bottomLayoutReserve = showDockedWaveform ? 1.0f : 13.0f;", budget)
        self.assertIn("startupWaveformOffsetReserve + bottomLayoutReserve;", budget)
        self.assertNotIn("ItemSpacing.y", budget)
        placement = main.split("// BOTTOM - Waveform and Transport", 1)[1].split(
            'BeginTransientScrollbarChild("WaveformPanel"', 1)[0]
        self.assertIn("dockColumnBottom + waveformPanelGapReserve + startupWaveformOffsetReserve", placement)
        self.assertNotIn("ItemSpacing.y", placement)

    def test_main_footer_does_not_inherit_the_pitch_column_tail_height(self) -> None:
        main = _read()
        start = main.index("// Waveform display with region selection", main.index("const float pitchColumnTailHeight ="))
        block = main[start : start + 900]
        self.assertIn("panelAvailH - waveFileNameBarHeight - waveFooterGap - waveFooterEdge", block)
        self.assertNotIn("waveformHeight = availHeight", block)

    def test_drag_preview_bottom_is_shared_and_not_hardcoded_blue(self) -> None:
        main = _read()
        self.assertIn("ResolveWaveformSplitterDragBg", main)
        self.assertIn("waveformSplitterDragBg", main)
        self.assertIn("gripFillBase", main)
        self.assertNotIn(
            "IM_COL32(90, 170, 240, aura::ui::dock::DockSplitGripFillAlpha(hovered, dragging))",
            main,
        )
        self.assertIn(
            "splitterMix(transportSeamBg, splitterTheme.chromeBorder, 0.06f)", main
        )

    def test_idle_dark_base_active_drag_preview(self) -> None:
        main = _read()
        self.assertIn('HorizontalSplitter("##TopDockSplitter"', main)
        self.assertIn('HorizontalSplitter("##BottomDockSplitter"', main)
        self.assertIn("const ImVec4 idleSeamBg = transportSeamBg;", main)
        self.assertIn("? waveformSplitterDragBg", main)
        self.assertNotIn("(waveformSplitter || dockBandSplitter)", main)


if __name__ == "__main__":
    unittest.main()
