#!/usr/bin/env python3
"""L1 link-base contract (RQ-20260911-04, architecture ruling A).

Pure CMake-file graph parsing (no build): every target whose SOURCES list a
localization-engine TU (Database/AudioEngine/AIEngine/ReaperBridgeDoctor) must
receive ArisLocalization through the single auto-link pass; per-target manual
links are forbidden; the product must no longer compile Localization.cpp
directly; the beta macro must be forwarded for byte-equivalence.
"""
from __future__ import annotations

import re
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ENGINE_TUS = ("src/Database.cpp", "src/AudioEngine.cpp", "src/AIEngine.cpp",
              "src/ReaperBridgeDoctor.cpp")

root_txt = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
tests_txt = (ROOT / "cmake" / "harken_search_behavior_tests.cmake").read_text(encoding="utf-8")
ALL_TXT = root_txt + "\n" + tests_txt


def executable_blocks(text: str):
    for m in re.finditer(r"add_executable\((\w+)(.*?)\)\n", text, re.S):
        yield m.group(1), m.group(2)


class LocalizationLinkContractTests(unittest.TestCase):
    def test_no_shared_static_localization_lib_runtime_trap(self) -> None:
        # Product is MD, tests MDd: a shared STATIC/OBJECT lib would LNK2038.
        self.assertNotIn("add_library(ArisLocalization", root_txt)

    def test_product_no_longer_compiles_localization_unit(self) -> None:
        # Engine users use the auto pass; isolated localization consumers have
        # explicit per-target sources, never a shared runtime library.
        app = re.search(r"set\(APP_SOURCES(.*?)\n\)", root_txt, re.S)
        self.assertIsNotNone(app)
        self.assertNotIn("src/Localization.cpp", app.group(1))
        self.assertEqual(2, root_txt.count("src/Localization.cpp"),
                         "expected the auto-pass site and isolated preference-draft test")
        self.assertIn("add_executable(TestPreferenceDraft EXCLUDE_FROM_ALL tests/test_preference_draft.cpp src/Localization.cpp)", root_txt)
        self.assertEqual(5, ALL_TXT.count("src/Localization.cpp"),
                         "auto pass, preference-draft test, two localization tests and standalone consumer loop")
        for target in ("TestLocalization", "TestMagicEQLocalization"):
            self.assertIn(f"add_dependencies({target} ArisLocalizationGenerated)", tests_txt)
        self.assertIn("add_dependencies(${_standalone_localized_target} ArisLocalizationGenerated)", tests_txt)

    def test_auto_pass_exists_and_covers_all_four_tus(self) -> None:
        pass_m = re.search(r"set\(AURA_LOCALIZATION_ENGINE_TUS(.*?)\)\n"
                           r".*?get_directory_property\(AURA_L1_ALL_TARGETS BUILDSYSTEM_TARGETS\)"
                           r".*?target_sources\(\$\{_t\} PRIVATE\s*\"\$\{CMAKE_CURRENT_SOURCE_DIR\}/src/Localization\.cpp\"\)",
                           root_txt, re.S)
        self.assertIsNotNone(pass_m, "L1 auto-source pass missing or truncated")
        for tu in ENGINE_TUS:
            self.assertIn(tu, pass_m.group(1), f"{tu} not in pass TU list")
        self.assertIn("add_dependencies(${_t} ArisLocalizationGenerated)", root_txt)

    def test_engine_targets_have_no_manual_wiring(self) -> None:
        engine_targets = [name for name, body in executable_blocks(ALL_TXT)
                          if any(tu in body for tu in ENGINE_TUS)]
        self.assertGreaterEqual(len(engine_targets), 20,
                                "file-graph parse regression: too few engine targets found")
        for tgt in engine_targets:
            self.assertEqual([], re.findall(
                rf"target_link_libraries\(\s*{tgt}\s[^)]*ArisLocalization", ALL_TXT))
            self.assertEqual([], re.findall(
                rf"target_sources\(\s*{tgt}\s[^)]*Localization\.cpp", ALL_TXT))

    def test_no_target_loses_localization_by_predicate_drift(self) -> None:
        pass_tus = set(re.findall(r"(src/\w+\.cpp)", re.search(
            r"set\(AURA_LOCALIZATION_ENGINE_TUS(.*?)\)\n", root_txt, re.S).group(1)))
        self.assertEqual(set(ENGINE_TUS), pass_tus)

    def test_l1b_support_injection_present_and_guarded(self) -> None:
        # L1b: Utils.cpp consumers get the HarkenUserData closure (migrator+layout)
        # through the SAME auto pass, per-file guards, WIN32 only. system_win32 /
        # text_encoding_win32 deliberately absent: stub-bound db targets LNK2005.
        block = re.search(r"if\(_t_needs_loc\)(.*?)endif\(\)\nendforeach\(\)", root_txt, re.S)
        self.assertIsNotNone(block, "injection block not found")
        body = block.group(1)
        self.assertIn("src/platform/harken_legacy_data_migrator.cpp", body)
        self.assertIn("src/platform/harken_user_data_layout.cpp", body)
        self.assertIn("if(WIN32)", body)
        self.assertIn("src/Utils.cpp", body)
        # guards must gate before inject (NOT <flag> immediately precedes target_sources)
        self.assertEqual(2, len(re.findall(r"if\(NOT _t_has_", body)))

if __name__ == "__main__":
    unittest.main(verbosity=2)
