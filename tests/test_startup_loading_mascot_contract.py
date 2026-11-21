"""REQ-20261117-SPLASH-LOGO-EXPRESSION / REQ-20261119-WELCOME-MASCOT / REQ-20261120-STARTUP-AVATAR:
both startup pages share one slot; it holds the brand mascot until the user sets a
personal avatar, then that avatar takes the slot."""

from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "src" / "WindowsMain.cpp").read_text(encoding="utf-8")
LOADING = SOURCE.split("static std::wstring BuildFlashUiLoadingHtml(", 1)[1].split("\n}\n", 1)[0]
WELCOME = SOURCE.split(
    "static std::wstring BuildFlashUiWelcomeHtml(const AppSettings& settings, const std::string& userNameUtf8) {",
    1,
)[1].split("\n}\n", 1)[0]
CSS = SOURCE.split("static void AppendStartupMascotCss(std::wstring& html) {", 1)[1].split("\n}\n", 1)[0]
AVATAR_CSS = SOURCE.split("static void AppendStartupAvatarCss(std::wstring& html) {", 1)[1].split("\n}\n", 1)[0]
GATE = SOURCE.split("static bool StartupHasPersonalAvatar(const AppSettings& settings) {", 1)[1].split("\n}\n", 1)[0]
AVATAR_MARKUP = SOURCE.split("static std::string BuildStartupAvatarMarkup(const AppSettings& settings) {", 1)[1].split("\n}\n", 1)[0]
BUILTIN_SVG = SOURCE.split("static std::string BuildStartupBuiltinAvatarSvg(int avatarIndex) {", 1)[1].split("\n}\n", 1)[0]
MASCOT = ".startup-loading-mascot"
SLOT = '<div class="startup-loading-mascot-slot" aria-hidden="true">'
ANIMATIONS = {
    f"{MASCOT}-antenna": "startupMascotAntenna",
    f"{MASCOT}-antenna-warmth": "startupMascotWarmth",
    f"{MASCOT}-eyes": "startupMascotEyes",
}


def mount(section: str) -> str:
    """The avatar slot expression for one startup surface."""
    match = re.search(
        re.escape(SLOT)
        + r'\n\)HTML_[LW]2";\n\s*const std::string (\w+) = BuildStartupAvatarMarkup\(settings\);'
        + r'\n\s*html \+= \1\.empty\(\) \? std::wstring\(HarkenSearch::kStartupLoadingMascotArtwork\)'
        + r'\n\s*: Utf8ToWide\(\1\);',
        section,
    )
    return match.group(1) if match else ""


def declarations(block: str) -> dict[str, str]:
    return {
        property_name: value.strip()
        for property_name, value in re.findall(r"([\w-]+)\s*:\s*([^;{}]+);", block)
    }


class StartupLoadingMascotContract(unittest.TestCase):
    def css_rule(self, selector: str, section: str = CSS) -> dict[str, str]:
        match = re.search(re.escape(selector) + r"\s*\{([^{}]*)\}", section)
        self.assertIsNotNone(match, f"Missing CSS rule for {selector}")
        return declarations(match[1])

    def css_group(self, prelude: str, section: str = CSS) -> dict[str, dict[str, str]]:
        match = re.search(re.escape(prelude) + r"\s*\{((?:[^{}]+\{[^{}]*\})+)\s*\}", section)
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
        for section in (LOADING, CSS):
            literals = list(re.finditer(pattern, section, re.DOTALL))
            self.assertTrue(literals)
            for literal in literals:
                delimiter = literal["delimiter"]
                windows_body = literal["body"].replace("\n", "\r\n")
                self.assertLessEqual(len(windows_body.encode("utf-16-le")), 16_380, delimiter)
                self.assertTrue(section[literal.end():].lstrip().startswith(";"), delimiter)

    def test_mounts_shared_artwork_in_the_centered_unclipped_loading_slot(self) -> None:
        self.assertTrue('#include "StartupLoadingMascotArt.h"' in SOURCE, "Missing shared artwork header")
        self.assertTrue(mount(LOADING), "The loading slot must fall back to the shared inline SVG")
        slot = self.css_rule(f"{MASCOT}-slot")
        artwork = self.css_rule(MASCOT)
        expected_slot = {"width": "86px", "height": "86px", "margin": "0 0 22px", "place-items": "center"}
        expected_artwork = {"width": "74px", "height": "74px", "overflow": "visible"}
        self.assertEqual(expected_slot, {key: slot.get(key) for key in expected_slot})
        self.assertEqual(expected_artwork, {key: artwork.get(key) for key in expected_artwork})
        self.assertNotIn("animation", slot)
        self.assertNotIn("animation", artwork)

    def test_personal_avatar_requires_an_upload_or_an_explicit_pick(self) -> None:
        self.assertIn("settings.hasCustomAvatar", GATE)
        self.assertRegex(GATE, r"settings\.diceBearAvatarIndex >= 1 && settings\.diceBearAvatarIndex <= 36")
        for surface, name in ((LOADING, "loading"), (WELCOME, "welcome")):
            with self.subTest(surface=name):
                self.assertTrue(mount(surface), f"{name} does not route the slot through the avatar rule")

    def test_custom_avatar_mounts_the_saved_picture(self) -> None:
        self.assertIn("GetCustomAvatarPathUtf8()", AVATAR_MARKUP)
        self.assertIn('src=\\"data:image/png;base64,', AVATAR_MARKUP)
        self.assertIn("Base64EncodeStartupAsset(pngBytes)", AVATAR_MARKUP)

    def test_builtin_avatar_reuses_the_settings_preview_numbers(self) -> None:
        self.assertIn("ImGui::ColorConvertHSVtoRGB(hue, 0.50f, 0.56f", BUILTIN_SVG)
        self.assertIn("ImGui::ColorConvertHSVtoRGB(accentHue, 0.34f, 0.86f", BUILTIN_SVG)
        self.assertIn("const float hue = (float)(idx - 1) / 36.0f;", BUILTIN_SVG)
        for literal in ("0.62f", "0.58f", "0.70f", "0.055f", "0.10f"):
            self.assertIn(literal, BUILTIN_SVG, literal)
        self.assertIn("ResolveStartupAvatarIndex(settings)", AVATAR_MARKUP)

    def test_avatar_keeps_the_mascot_slot_metrics(self) -> None:
        avatar = self.css_rule(".startup-loading-avatar", AVATAR_CSS)
        slot = self.css_rule(f"{MASCOT}-slot")
        artwork = self.css_rule(MASCOT)
        self.assertEqual(artwork["width"], avatar["width"])
        self.assertEqual(artwork["height"], avatar["height"])
        self.assertEqual("block", avatar["display"])
        self.assertIn("AppendStartupAvatarCss(html)", LOADING)
        self.assertIn("AppendStartupAvatarCss(html)", WELCOME)

    def test_removes_dangling_loading_dom_ids(self) -> None:
        declared_ids = set(re.findall(r'\bid="([^"]+)"', LOADING))
        script_ids = set(re.findall(r'\bgetElementById\("([^"]+)"\)', LOADING))
        self.assertLessEqual({"progressBar", "progressDetail", "statusText"}, script_ids)
        self.assertLessEqual(script_ids, declared_ids)
        self.assertTrue("window.updateProgress = function" in LOADING)
        self.assertTrue("window.startExitTransition = function" in LOADING)
        self.assertNotIn("avatarInitials", LOADING + WELCOME)
        self.assertNotIn("pulseHalo", LOADING + WELCOME)

    def test_entry_veil_is_behind_the_loading_information(self) -> None:
        veil = self.css_rule("body::before", LOADING)
        overlay = self.css_rule(".loading-overlay", LOADING)
        self.assertLess(int(veil["z-index"]), int(overlay["z-index"]))

    def test_loading_card_is_legible_from_the_first_frame(self) -> None:
        overlay = self.css_rule(".loading-overlay", LOADING)
        with self.subTest(layer="overlay"):
            self.assertNotIn("animation", overlay)
            self.assertEqual("1", overlay.get("opacity", "1"))
        with self.subTest(layer="card"):
            fade = self.css_rule(".glass-card", LOADING)["animation"].split()[0]
            frames = self.css_group(f"@keyframes {fade}", LOADING)
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

    def test_welcome_surface_uses_the_same_slot_and_avatar_rule(self) -> None:
        self.assertTrue(mount(WELCOME), "The welcome slot must fall back to the shared inline SVG")
        self.assertIn("AppendStartupMascotCss(html)", WELCOME, "Welcome must reuse the shared mascot CSS")
        self.assertIn("AppendStartupMascotCss(html)", LOADING, "Loading must reuse the shared mascot CSS")
        self.assertNotIn("BuildLocalDiceBearAvatarDataUri", SOURCE, "DiceBear files must not come back")
        self.assertNotIn("avatarPulse", WELCOME, "The welcome page must not keep its old avatar halo")
        self.assertNotIn("avatar-wrap", WELCOME, "The welcome page must not keep its old avatar slot")

    def test_reduced_motion_disables_each_mascot_animation(self) -> None:
        reduced = self.css_group("@media (prefers-reduced-motion: reduce)")
        for selector in ANIMATIONS:
            self.assertEqual("none", reduced.get(selector, {}).get("animation"), selector)


if __name__ == "__main__":
    unittest.main()
