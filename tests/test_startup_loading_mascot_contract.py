"""REQ-20261117-SPLASH-LOGO-EXPRESSION: loading uses the shared mascot, not the user avatar."""

from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "src" / "WindowsMain.cpp").read_text(encoding="utf-8")
LOADING = SOURCE.split("static std::wstring BuildFlashUiLoadingHtml(", 1)[1].split("\n}\n", 1)[0]
MASCOT = ".startup-loading-mascot"
ANIMATIONS = {
    f"{MASCOT}-antenna": "startupMascotAntenna",
    f"{MASCOT}-antenna-warmth": "startupMascotWarmth",
    f"{MASCOT}-eyes": "startupMascotEyes",
}


def declarations(block: str) -> dict[str, str]:
    return {
        property_name: value.strip()
        for property_name, value in re.findall(r"([\w-]+)\s*:\s*([^;{}]+);", block)
    }


class StartupLoadingMascotContract(unittest.TestCase):
    def css_rule(self, selector: str) -> dict[str, str]:
        match = re.search(re.escape(selector) + r"\s*\{([^{}]*)\}", LOADING)
        self.assertIsNotNone(match, f"Missing CSS rule for {selector}")
        return declarations(match[1])

    def css_group(self, prelude: str) -> dict[str, dict[str, str]]:
        match = re.search(re.escape(prelude) + r"\s*\{((?:[^{}]+\{[^{}]*\})+)\s*\}", LOADING)
        self.assertIsNotNone(match, f"Missing CSS group {prelude}")
        return {
            selector.strip(): declarations(block)
            for selectors, block in re.findall(r"([^{}]+)\{([^{}]*)\}", match[1])
            for selector in selectors.split(",")
        }

    def keyframes(self, animation: str) -> dict[float, dict[str, str]]:
        return {
            round(float(percent.rstrip("%")) * 3.2 / 100, 2): values
            for percent, values in self.css_group(f"@keyframes {animation}").items()
        }

    def test_loading_wide_raw_literals_fit_independent_msvc_appends(self) -> None:
        pattern = r'LR"(?P<delimiter>\w*)\((?P<body>.*?)\)(?P=delimiter)"'
        literals = list(re.finditer(pattern, LOADING, re.DOTALL))
        self.assertTrue(literals)
        for literal in literals:
            delimiter = literal["delimiter"]
            windows_body = literal["body"].replace("\n", "\r\n")
            self.assertLessEqual(len(windows_body.encode("utf-16-le")), 16_380, delimiter)
            self.assertTrue(LOADING[literal.end():].lstrip().startswith(";"), delimiter)

    def test_mounts_shared_artwork_in_the_centered_unclipped_loading_slot(self) -> None:
        self.assertTrue('#include "StartupLoadingMascotArt.h"' in SOURCE, "Missing shared artwork header")
        mount = re.search(
            r'<div class="startup-loading-mascot-slot" aria-hidden="true">'
            r'[^<]*HarkenSearch::kStartupLoadingMascotArtwork[^<]*</div>',
            LOADING,
        )
        self.assertIsNotNone(mount, "The loading slot must render the shared inline SVG")
        slot = self.css_rule(f"{MASCOT}-slot")
        artwork = self.css_rule(MASCOT)
        expected_slot = {"width": "86px", "height": "86px", "margin": "0 0 22px", "place-items": "center"}
        expected_artwork = {"width": "74px", "height": "74px", "overflow": "visible"}
        self.assertEqual(expected_slot, {key: slot.get(key) for key in expected_slot})
        self.assertEqual(expected_artwork, {key: artwork.get(key) for key in expected_artwork})
        self.assertNotIn("animation", slot)
        self.assertNotIn("animation", artwork)

    def test_removes_personal_avatar_hooks_without_leaving_dangling_loading_dom_ids(self) -> None:
        removed = set(re.findall(r"(?i)[\w-]*(?:avatar|pulseHalo|displayNameUtf8|loadingNameChars|loadingDisplayName)[\w-]*", LOADING))
        self.assertFalse(removed, f"Loading still references personal avatar hooks: {sorted(removed)}")
        declared_ids = set(re.findall(r'\bid="([^"]+)"', LOADING))
        script_ids = set(re.findall(r'\bgetElementById\("([^"]+)"\)', LOADING))
        self.assertLessEqual({"progressBar", "progressDetail", "statusText"}, script_ids)
        self.assertLessEqual(script_ids, declared_ids)
        self.assertTrue("window.updateProgress = function" in LOADING)
        self.assertTrue("window.startExitTransition = function" in LOADING)

    def test_entry_veil_is_behind_the_loading_information(self) -> None:
        veil = self.css_rule("body::before")
        overlay = self.css_rule(".loading-overlay")
        self.assertLess(int(veil["z-index"]), int(overlay["z-index"]))

    def test_loading_card_is_legible_from_the_first_frame(self) -> None:
        overlay = self.css_rule(".loading-overlay")
        with self.subTest(layer="overlay"):
            self.assertNotIn("animation", overlay)
            self.assertEqual("1", overlay.get("opacity", "1"))
        with self.subTest(layer="card"):
            fade = self.css_rule(".glass-card")["animation"].split()[0]
            frames = self.css_group(f"@keyframes {fade}")
            self.assertGreaterEqual(float(frames["from"]["opacity"]), 0.85)
            self.assertEqual("1", frames["to"]["opacity"])

    def test_eyes_reach_the_left_pose_within_200_ms(self) -> None:
        eyes = self.keyframes("startupMascotEyes")
        left = {"transform": "translateX(-2.9px) scaleY(1)"}
        self.assertTrue(
            any(0 < seconds <= 0.2 and pose == left for seconds, pose in eyes.items()),
            "The first left look must finish within the short-startup window",
        )

    def test_parts_follow_the_3_point_2_second_timeline(self) -> None:
        for selector, animation in ANIMATIONS.items():
            self.assertEqual([animation, "3.2s"], self.css_rule(selector)["animation"].split()[:2])
        eyes_rule = self.css_rule(f"{MASCOT}-eyes")
        self.assertEqual("view-box", eyes_rule["transform-box"])
        self.assertEqual("50% 47.2%", eyes_rule["transform-origin"])

        antenna = self.keyframes("startupMascotAntenna")
        warmth = self.keyframes("startupMascotWarmth")
        for seconds in (0.0, 3.2):
            self.assertEqual({"transform": "translateY(0)"}, antenna[seconds])
            self.assertEqual({"opacity": "0"}, warmth[seconds])
        antenna_peaks = [
            seconds for seconds, pose in antenna.items() if pose == {"transform": "translateY(-1.2px)"}
        ]
        self.assertEqual(1, len(antenna_peaks))
        self.assertGreater(antenna_peaks[0], 0)
        self.assertLessEqual(antenna_peaks[0], 0.2)
        self.assertEqual({"opacity": ".93"}, warmth[antenna_peaks[0]])

        eyes = self.keyframes("startupMascotEyes")
        for seconds in (0.0, 1.70, 1.86, 2.08, 3.2):
            self.assertEqual({"transform": "translateX(0) scaleY(1)"}, eyes[seconds])
        self.assertEqual({"transform": "translateX(-2.9px) scaleY(1)"}, eyes[0.92])
        for seconds in (1.24, 1.47):
            self.assertEqual({"transform": "translateX(2.3px) scaleY(1)"}, eyes[seconds])
        self.assertEqual({"transform": "translateX(0) scaleY(.25)"}, eyes[1.94])

    def test_reduced_motion_disables_each_mascot_animation(self) -> None:
        reduced = self.css_group("@media (prefers-reduced-motion: reduce)")
        for selector in ANIMATIONS:
            self.assertEqual("none", reduced.get(selector, {}).get("animation"), selector)


if __name__ == "__main__":
    unittest.main()
