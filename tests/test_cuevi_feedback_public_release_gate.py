import pathlib
import re
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
CMAKE = ROOT / "CMakeLists.txt"
MAIN = ROOT / "src" / "main.cpp"
HEADER = ROOT / "src" / "CueviFeedbackClient.h"
CLIENT = ROOT / "src" / "CueviFeedbackClient.cpp"
QA_PACK = ROOT / "scripts" / "package_qa_exe.ps1"

PUBLIC_RELEASE_FEEDBACK_GATE = re.compile(
    r"if\s*\(\s*ARIS_PUBLIC_RELEASE\s*\)\s*(.*?)\s*endif\s*\(\s*\)",
    re.S,
)
ENDPOINT_FORCE_EMPTY = re.compile(
    r'set\s*\(\s*AIRS_CUEVI_FEEDBACK_ENDPOINT\s+""\s+CACHE\s+STRING\s+"[^"]*"\s+FORCE\s*\)'
)
KEY_FORCE_EMPTY = re.compile(
    r'set\s*\(\s*AIRS_CUEVI_FEEDBACK_KEY\s+""\s+CACHE\s+STRING\s+"[^"]*"\s+FORCE\s*\)'
)
UI_ENTRY_DECL = re.compile(
    r"bool\s+IsUiEntryEnabled\s*\(\s*const\s+Config\s*&\s*\w*\s*\)\s*;"
)
UI_ENTRY_CALL = re.compile(
    r"CueviFeedback::IsUiEntryEnabled\s*\(\s*BuildCueviFeedbackConfig\s*\(\s*\)\s*\)"
)


def _slice_after(text, marker):
    idx = text.index(marker)
    return text[idx:]


class CueviFeedbackPublicReleaseGateTests(unittest.TestCase):
    def test_public_release_force_clears_feedback_endpoint_and_key_before_header_generation(self):
        cmake = CMAKE.read_text(encoding="utf-8")
        gates = [
            match for match in PUBLIC_RELEASE_FEEDBACK_GATE.finditer(cmake)
            if "AIRS_CUEVI_FEEDBACK_ENDPOINT" in match.group(1)
            and "AIRS_CUEVI_FEEDBACK_KEY" in match.group(1)
        ]
        self.assertEqual(
            len(gates),
            1,
            "CMakeLists.txt must have exactly one ARIS_PUBLIC_RELEASE block that clears feedback credentials",
        )
        gate = gates[0]
        body = gate.group(1)
        self.assertRegex(body, ENDPOINT_FORCE_EMPTY)
        self.assertRegex(body, KEY_FORCE_EMPTY)
        self.assertNotIn("if(", body)
        self.assertNotRegex(body, r'set\s*\(\s*AIRS_CUEVI_FEEDBACK_ENDPOINT\s+"[^"]+"')
        self.assertNotRegex(body, r'set\s*\(\s*AIRS_CUEVI_FEEDBACK_KEY\s+"[^"]+"')
        configure_idx = cmake.index("src/CueviFeedbackBuildConfig.h.in")
        self.assertLess(
            gate.start(),
            configure_idx,
            "public-release credential wipe must run before CueviFeedbackBuildConfig.h generation",
        )
        self.assertIn(
            'QA builds must set ARIS_PUBLIC_RELEASE=OFF; public-release mode would clear Cuevi feedback credentials',
            cmake,
        )

    def test_internal_qa_packaging_still_injects_feedback_cache_without_public_release(self):
        script = QA_PACK.read_text(encoding="utf-8")
        self.assertIn(
            'set(AIRS_CUEVI_FEEDBACK_ENDPOINT "$ENV{AIRS_CUEVI_FEEDBACK_ENDPOINT}" CACHE STRING "Cuevi feedback endpoint" FORCE)',
            script,
        )
        key_line = (
            'set(AIRS_CUEVI_FEEDBACK_KEY "'
            + "${_airs_cuevi_feedback_key}"
            + '" CACHE STRING "Cuevi feedback key" FORCE)'
        )
        self.assertIn(key_line, script)
        self.assertIn(
            'set(ARIS_PUBLIC_RELEASE OFF CACHE BOOL "Build a public-release-safe surface" FORCE)',
            script,
        )
        self.assertRegex(script, r'-DARIS_PUBLIC_RELEASE=OFF')
        self.assertNotRegex(script, r"-DARIS_PUBLIC_RELEASE\s*=\s*ON")
        self.assertNotRegex(script, r"set\(\s*ARIS_PUBLIC_RELEASE\s+ON")

    def test_feedback_ui_entry_empty_config_gate_is_a_pure_function_used_by_help_and_crash_entries(self):
        header = HEADER.read_text(encoding="utf-8")
        client = CLIENT.read_text(encoding="utf-8")
        main = MAIN.read_text(encoding="utf-8")
        self.assertRegex(header, UI_ENTRY_DECL)
        self.assertIn("IsUiEntryEnabled", client)
        self.assertIn("endpointUrl.empty()", client)
        self.assertIn("restrictedKey.empty()", client)
        self.assertIn("projectId.empty()", client)
        self.assertIn("boardId.empty()", client)
        self.assertIn("milestoneId.empty()", client)
        self.assertIn("ParseEndpoint(config.endpointUrl)", client)

        help_region = _slice_after(main, "if (ImGui::BeginMenu(L(Menu_Help)))")
        help_region = help_region[: help_region.index("ImGui::EndMenu();")]
        self.assertRegex(help_region, UI_ENTRY_CALL)
        self.assertIn("ImGui::MenuItem(L(Menu_SendFeedback))", help_region)
        self.assertRegex(
            help_region,
            r"if\s*\(\s*(?:feedbackUiEnabled|CueviFeedback::IsUiEntryEnabled)",
        )

        crash_region = _slice_after(main, "// F-035: Crash Recovery Dialog")
        crash_region = crash_region[: crash_region.index("// F-035: Beta Feedback Dialog")]
        self.assertNotIn("OpenPopup", crash_region)
        self.assertNotIn("crashButtonLabel", crash_region)
        self.assertNotIn("showCrashReportDialog = true", crash_region)
        self.assertIn("crashDetectedOnStartup = false", crash_region)


if __name__ == "__main__":
    unittest.main()
