import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MAIN = (ROOT / "src" / "main.cpp").read_text(encoding="utf-8")
HEADER = ROOT / "src" / "SettingsBodyScrollHeight.h"
CMAKE = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
PUBLIC_CHECKS = ROOT / ".github" / "workflows" / "public-checks.yml"
PUBLIC_CHECK_RUNNER = ROOT / "tools" / "run_public_checks.py"
CPP_TEST = ROOT / "tests" / "test_settings_body_scroll_height.cpp"


class SettingsBodyScrollHeightSourceContractTests(unittest.TestCase):
    def test_height_policy_is_extracted_header(self) -> None:
        self.assertTrue(HEADER.is_file())
        text = HEADER.read_text(encoding="utf-8")
        self.assertIn("ShouldUpdateSettingsBodyContentHeight", text)

    def test_settings_dialog_uses_native_scroll_ownership(self) -> None:
        # Native child scrolling replaces the old manually measured body.
        # Retain the pure legacy height-policy tests for historical callers.
        shell=(ROOT / "src/SettingsDialog.inl").read_text(encoding="utf-8")
        self.assertIn('ImGui::BeginChild("SettingsBody"', shell)
        self.assertIn('winSize.y-headerH-footerH', shell)
        self.assertNotIn('s_settingsBodyContentH', shell)


    def test_cpp_behavior_test_is_wired(self) -> None:
        self.assertTrue(CPP_TEST.is_file())
        self.assertIn("TestSettingsBodyScrollHeight", CMAKE)
        self.assertIn("tests/test_settings_body_scroll_height.cpp", CMAKE)

    def test_source_contract_is_wired(self) -> None:
        self.assertIn("SettingsBodyScrollHeightSourceContract", CMAKE)
        self.assertIn("tests/test_settings_body_scroll_height.py", CMAKE)

    def test_source_contract_runs_on_public_pull_requests(self) -> None:
        self.assertTrue(PUBLIC_CHECKS.is_file())
        workflow = PUBLIC_CHECKS.read_text(encoding="utf-8")
        self.assertIn("pull_request:", workflow)
        self.assertIn("python tools/run_public_checks.py", workflow)
        self.assertTrue(PUBLIC_CHECK_RUNNER.is_file())
        self.assertIn(
            '"tests/test_settings_body_scroll_height.py"',
            PUBLIC_CHECK_RUNNER.read_text(encoding="utf-8"),
        )

    def test_qa_push_runs_regression(self) -> None:
        workflow = ROOT / ".github" / "workflows" / "settings-scroll-regression.yml"
        self.assertTrue(workflow.is_file())
        text = workflow.read_text(encoding="utf-8")
        self.assertIn("qa/current", text)
        self.assertIn("SettingsBodyScrollHeight", text)


if __name__ == "__main__":
    unittest.main()
