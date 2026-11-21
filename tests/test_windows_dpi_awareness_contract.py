"""Focused contract for the Windows DPI awareness Slice A.

The source contract is intentionally platform-neutral so it can run before a
Windows build.  When ``ARIS_DPI_TEST_EXE`` is provided, the same test also
checks the final PE bytes for the embedded manifest tokens.
"""

from __future__ import annotations

import ctypes
import os
import shutil
import subprocess
import tempfile
import time
import unittest
import xml.etree.ElementTree as ET
from ctypes import wintypes
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
CMAKE = ROOT / "CMakeLists.txt"
MANIFEST = ROOT / "resources" / "HarkenSearch.manifest"


class WindowsDpiAwarenessContractTests(unittest.TestCase):
    def test_windows_chrome_uses_native_dpi_without_new_reflow(self) -> None:
        windows = (ROOT / "src" / "WindowsMain.cpp").read_text(encoding="utf-8")
        main = (ROOT / "src" / "main.cpp").read_text(encoding="utf-8")
        self.assertIn("io.ConfigDpiScaleFonts = true;", windows)
        shared_scale = (ROOT / "src" / "ui" / "UiScale.h").read_text(encoding="utf-8")
        self.assertIn("ImGui::GetStyle().FontScaleDpi", shared_scale)
        self.assertIn("const bool layoutTinyWidth = mainWorkW < 760.0f;", main)
        self.assertIn("const bool layoutVeryShortHeight = mainWorkH < 520.0f;", main)
        self.assertNotIn("compactTransportReflow", main)
        self.assertNotIn("scrollCompactToolbar", main)

    def test_aris_target_embeds_manifest_and_registers_focused_test(self) -> None:
        cmake = CMAKE.read_text(encoding="utf-8")
        normalized = cmake.replace("\\", "/")
        self.assertRegex(
            normalized,
            r'else\(\)\s+set\(_aris_manifest "\$\{CMAKE_CURRENT_SOURCE_DIR\}/resources/HarkenSearch\.manifest"\)',
            "Normal Harken Search builds must select the approved Windows manifest",
        )
        self.assertIn('"/MANIFESTINPUT:${_aris_manifest}"', normalized)
        self.assertIn("target_link_options(HarkenSearch PRIVATE", cmake)
        self.assertRegex(
            cmake,
            r"NAME\s+TestWindowsDpiAwarenessContract",
            "the DPI contract must be registered with CTest",
        )

    def test_manifest_declares_per_monitor_v2_awareness(self) -> None:
        manifest = MANIFEST.read_text(encoding="utf-8")
        root = ET.fromstring(manifest)
        values = {
            element.tag.rsplit("}", 1)[-1]: (element.text or "").strip()
            for element in root.iter()
        }
        self.assertEqual("true/pm", values.get("dpiAware"))
        self.assertEqual("PerMonitorV2, PerMonitor", values.get("dpiAwareness"))

    def test_optional_final_exe_contains_manifest_tokens(self) -> None:
        raw_exe = os.environ.get("ARIS_DPI_TEST_EXE", "").strip()
        if not raw_exe:
            self.skipTest("ARIS_DPI_TEST_EXE is not set")
        exe = Path(raw_exe)
        self.assertTrue(exe.is_file(), exe)
        # A PE resource is binary; latin-1 preserves every byte one-to-one and
        # is sufficient for a deterministic token check without a Windows-only
        # resource parser.
        payload = exe.read_bytes().decode("latin-1")
        self.assertIn("PerMonitorV2", payload)
        self.assertIn("dpiAware", payload)
        self.assertIn("true/pm", payload)

    @unittest.skipUnless(os.name == "nt", "Windows runtime probe")
    def test_optional_runtime_process_is_per_monitor_aware(self) -> None:
        if os.environ.get("ARIS_DPI_RUNTIME_PROBE") != "1":
            self.skipTest("ARIS_DPI_RUNTIME_PROBE is not enabled")
        raw_exe = os.environ.get("ARIS_DPI_TEST_EXE", "").strip()
        if not raw_exe:
            self.fail("ARIS_DPI_TEST_EXE is required for the runtime probe")
        exe = Path(raw_exe)
        self.assertTrue(exe.is_file(), exe)

        probe_root = tempfile.mkdtemp(prefix="aris-dpi-probe-")
        environment = os.environ.copy()
        environment.update(
            {
                "AURA_RUNTIME_ROOT": probe_root,
                "AURA_HARNESS_MULTI_INSTANCE": "1",
                "AURA_DISABLE_SINGLE_INSTANCE": "1",
                "AURA_HARNESS_LANE_KIND": "diagnostics",
            }
        )
        startup = subprocess.STARTUPINFO()
        startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
        startup.wShowWindow = 0
        process = subprocess.Popen(
            [str(exe)],
            cwd=str(exe.parent),
            env=environment,
            startupinfo=startup,
            creationflags=subprocess.CREATE_NO_WINDOW,
        )
        try:
            get_awareness = ctypes.WinDLL("shcore.dll").GetProcessDpiAwareness
            get_awareness.argtypes = [wintypes.HANDLE, ctypes.POINTER(ctypes.c_int)]
            get_awareness.restype = ctypes.c_long
            deadline = time.monotonic() + 5.0
            last_hresult = -1
            last_awareness = -1
            while process.poll() is None and time.monotonic() < deadline:
                value = ctypes.c_int(-1)
                last_hresult = get_awareness(
                    wintypes.HANDLE(process._handle), ctypes.byref(value)
                )
                last_awareness = value.value
                if last_hresult == 0 and last_awareness == 2:
                    break
                time.sleep(0.1)
            self.assertIsNone(process.poll(), "Aris exited before the DPI probe completed")
            self.assertEqual(0, last_hresult)
            self.assertEqual(2, last_awareness)
        finally:
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
            shutil.rmtree(probe_root, ignore_errors=True)


if __name__ == "__main__":
    unittest.main()
