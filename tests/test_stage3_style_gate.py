from __future__ import annotations

import re
import subprocess
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


class Stage3StyleGate(unittest.TestCase):
    def test_internal_execution_routing_artifacts_are_retired(self) -> None:
        retired = (
            ROOT / "docs/process/ARIS_AI_MODEL_POLICY.md",
            ROOT / "docs/plans/refactor/prompts/stage-5-launch.md",
        )
        remaining = [path.relative_to(ROOT).as_posix() for path in retired if path.exists()]
        self.assertEqual([], remaining, f"Obsolete internal routing artifacts remain: {remaining}")

    def test_product_ai_contract_and_disclosure_remain(self) -> None:
        required_files = (
            "AI_FEATURE_DISCLOSURE.md",
            "PRIVACY.md",
            "src/AIEngine.cpp",
            "src/AIEngine.h",
        )
        for relative_path in required_files:
            self.assertTrue((ROOT / relative_path).is_file(), relative_path)

        disclosure = (ROOT / "AI_FEATURE_DISCLOSURE.md").read_text(encoding="utf-8")
        privacy = (ROOT / "PRIVACY.md").read_text(encoding="utf-8")
        engine = (ROOT / "src/AIEngine.cpp").read_text(encoding="utf-8")
        header = (ROOT / "src/AIEngine.h").read_text(encoding="utf-8")
        combined = disclosure + privacy + engine + header
        self.assertIn("whisper.cpp", disclosure)
        self.assertIn("GenerateCoverAsync", header)
        for token in (
            "SiliconFlow",
            "siliconflow.cn",
            "cover-generation prompts",
            "FLUX.1-schnell",
        ):
            self.assertNotIn(token, combined, token)

    def test_legal_and_provenance_records_remain(self) -> None:
        required_paths = (
            "LICENSE",
            "THIRD_PARTY_NOTICES.md",
            "ASSET_PROVENANCE.md",
            "thirdparty/BINARY_PROVENANCE.md",
            "LICENSES/WebView2-LICENSE.txt",
            "LICENSES/whisper.cpp-MIT.txt",
            "assets/avatars/dicebear/lorelei-neutral/LICENSE.md",
        )
        missing = [path for path in required_paths if not (ROOT / path).is_file()]
        self.assertEqual([], missing, f"Required legal/provenance records are missing: {missing}")

    def test_legacy_compatibility_identifiers_remain(self) -> None:
        cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8", errors="replace")
        utils = (ROOT / "src/Utils.cpp").read_text(encoding="utf-8", errors="replace")
        windows = (ROOT / "src/WindowsMain.cpp").read_text(encoding="utf-8", errors="replace")
        host_send = (ROOT / "src/HostSendModel.cpp").read_text(encoding="utf-8", errors="replace")
        for token, source in (
            ("project(HarkenSearch VERSION", cmake),
            ("Auro.db", utils),
            ("Auro.AudioFile", windows),
            ('root /= "Auro"', host_send),
            ("namespace Aura", (ROOT / "src/AppState.h").read_text(encoding="utf-8")),
        ):
            self.assertIn(token, source, token)

    def test_named_delivery_tool_residue_is_absent_from_first_party_surfaces(self) -> None:
        named_tools = re.compile(
            r"\b(?:Claude|Codex|Gemini|ChatGPT|Copilot|Anthropic)\b",
            re.IGNORECASE,
        )
        excluded = {
            "tools/stage3_human_style_audit.py",
            "tests/test_stage3_human_style_audit.py",
            "tests/test_stage3_style_gate.py",
            "docs/plans/refactor/stage-3-style-register.json",
            "docs/plans/refactor/stage-3-style-register.md",
            "docs/plans/refactor/stage-3-report.md",
        }
        findings: list[str] = []
        tracked = subprocess.check_output(
            ["git", "ls-files", "-z"], cwd=ROOT
        ).decode("utf-8", errors="surrogateescape").split("\0")
        for relative_path in tracked:
            if not relative_path or relative_path in excluded:
                continue
            if relative_path.startswith((
                "docs/context/", "docs/superpowers/", "docs/plans/",
                "docs/product/cards/", "tests/"
            )):
                continue
            if relative_path.startswith(("thirdparty/", "LICENSES/")):
                continue
            if relative_path.startswith("docs/plans/refactor/cards/stage-3-"):
                continue
            path = ROOT / relative_path
            if not path.is_file() or path.suffix.lower() not in {
                ".c",
                ".cmake",
                ".cpp",
                ".h",
                ".hpp",
                ".iss",
                ".md",
                ".mm",
                ".ps1",
                ".py",
                ".txt",
            }:
                continue
            text = path.read_text(encoding="utf-8", errors="replace")
            findings.extend(
                f"{relative_path}:{line_number}"
                for line_number, line in enumerate(text.splitlines(), start=1)
                if named_tools.search(line)
            )
        self.assertEqual([], findings, f"Named delivery-tool residue remains: {findings[:80]}")


if __name__ == "__main__":
    unittest.main()
