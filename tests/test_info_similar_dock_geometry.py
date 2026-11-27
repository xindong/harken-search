"""Info dock content geometry: no overlap with a lower docked panel."""

from __future__ import annotations

import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MAIN = ROOT / "src" / "main.cpp"


def _read() -> str:
    return MAIN.read_text(encoding="utf-8")


class InfoSimilarDockGeometryContract(unittest.TestCase):
    def test_info_content_uses_info_leaf_height_not_full_column(self) -> None:
        main = _read()
        body_start = main.index("const auto renderDockToolBody")
        body = main[body_start : main.index("// Left Column", body_start)]
        self.assertIn('"FileInfoContent"', body)
        self.assertIn("shellHeight - shell.titleHeight - shell.insetT - shell.insetB - top - bottom", body)
        self.assertIn("beginDockContentViewport(module, childId, outerSize, bodySize, bodyFlags)", body)
        self.assertIn("rightRegionSplit ? std::max(1.0f, infoLeafHeight) : sideColumnHeight", main)
        self.assertIn("renderDockedToolBody(displayedTool, rightShell, size.y,", main)
        legacy = main[main.index('if (leaf.id == "info" && !state.panelFileInfo.floating') :]
        self.assertIn("ImVec2(leaf.rect.width, leaf.rect.height)", legacy)
        self.assertIn('renderDockedToolBody("info", shell, size.y, false, nullptr);', legacy)

    def test_similar_docked_content_is_constrained_to_the_inner_leaf(self) -> None:
        main = _read()
        idx = main.find('"##DockedSimilarSounds"')
        self.assertGreater(idx, 0)
        block = main[idx : main.index("if (!open)", idx)]
        self.assertRegex(
            block,
            r"renderIndependentSimilarSounds\(ImVec2\(leaf\.rect\.width, leaf\.rect\.height\),"
            r"\s*similarShell\.contentMin, similarShell\.contentMax\)",
        )

        start = main.index("const auto renderIndependentSimilarSounds")
        viewport = main[start : main.index("auto renderFunctionDockedModule", start)]
        self.assertIn("ImVec2(contentMin.x + padding, contentMin.y + padding)", viewport)
        self.assertIn("contentMax.x - contentMin.x - 2.0f * padding", viewport)
        self.assertIn("contentMax.y - contentMin.y - 2.0f * padding", viewport)
        self.assertIn(
            'beginDockContentViewport("similar-sounds", "##IndependentSimilarSoundsContent",',
            viewport,
        )
        self.assertIn("outerSize, bodySize, ImGuiWindowFlags_NoBackground", viewport)


if __name__ == "__main__":
    unittest.main()
