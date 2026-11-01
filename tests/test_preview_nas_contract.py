#!/usr/bin/env python3
from __future__ import annotations

import json
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HEADER = (ROOT / "src/platform/filesystem.h").read_text(encoding="utf-8")
DATABASE = (ROOT / "src/Database.cpp").read_text(encoding="utf-8")
MAIN = (ROOT / "src/main.cpp").read_text(encoding="utf-8")
WINDOWS_MAIN = (ROOT / "src/WindowsMain.cpp").read_text(encoding="utf-8")
METADATA = (ROOT / "src/MetadataWriter.cpp").read_text(encoding="utf-8")
AI_ENGINE = (ROOT / "src/AIEngine.cpp").read_text(encoding="utf-8")
AI_HEADER = (ROOT / "src/AIEngine.h").read_text(encoding="utf-8")
MODULE_UI = (ROOT / "src/ModuleUI.cpp").read_text(encoding="utf-8")
LOCALE = (ROOT / "locale/harken.ui.strings.json").read_text(encoding="utf-8")
CMAKE = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")

FMT_RE = re.compile(r"%[-+0-9\.]*[sdif]")
KEYS = [
    "Scan_NetworkLibraryUnreachable",
    "Main_FailedFmt",
    "ScanUi_FailedPath",
    "ScanUi_NetworkTitle",
    "ScanUi_NetworkAction",
    "Scan_NetworkLibraryReadonlyHint",
    "Scan_NetworkDualPathWarn",
    "Scan_MappedDriveHiddenHint",
    "Lib_ReadonlyNetwork",
    "Write_BlockedNetworkLibrary",
    "Db_RejectNetworkPath",
]

class NetworkPathContract(unittest.TestCase):
    def test_header_declares_status_and_is_network_path(self) -> None:
        self.assertIn("Unreachable", HEADER)
        self.assertIn("bool IsNetworkPath(const std::string& pathUtf8);", HEADER)

    def test_probe_timeout_uses_detach_not_async(self) -> None:
        body = DATABASE.split("ScanStats Database::scanDirectoryEx(", 1)[1].split("FastScanModeGuard", 1)[0]
        self.assertIn("std::thread", body)
        self.assertIn(".detach()", body)
        self.assertNotIn("std::async", body)
        self.assertNotIn("std::future", body)
        self.assertIn("pathUtf8, done, status", body)
        self.assertNotIn("this,", body.replace("this, sessionOwnedByThisCall", ""))

    def test_open_checks_isolation_before_network(self) -> None:
        body = DATABASE.split("bool Database::open(", 1)[1].split("sqlite3_open", 1)[0]
        iso = body.find("PreviewIsolationAllowsDataPathUtf8")
        net = body.find("IsNetworkPath")
        ensure = body.find("EnsureDirectoryExists")
        self.assertGreaterEqual(iso, 0)
        self.assertGreater(net, iso)
        self.assertGreater(ensure, net)
        self.assertIn("Db_RejectNetworkPath", body)

    def test_no_new_aicover_button(self) -> None:
        self.assertNotIn("ImGui::Button(L(AICover_Title)", MAIN)

    def test_no_new_dock(self) -> None:
        self.assertNotIn("DockController", MAIN)
        self.assertNotIn("DockingEnable", CMAKE)
        self.assertNotIn("ViewportsEnable", CMAKE)
        self.assertIn("BeginDockablePanel", MAIN)

    def test_write_gates_exist(self) -> None:
        self.assertIn("IsNetworkPath", METADATA)
        self.assertIn("Write_BlockedNetworkLibrary", METADATA)
        self.assertIn("IsNetworkPath", AI_ENGINE)
        self.assertIn("CreateCoverTaskWithoutQueue", AI_HEADER)
        self.assertIn("IsNetworkPath", MODULE_UI)
        self.assertIn("Write_BlockedNetworkLibrary", MODULE_UI)

    def test_locale_keys_trilingual(self) -> None:
        data = json.loads(LOCALE)
        entries = {item["id"]: item for item in data["entries"]}
        for key in KEYS:
            self.assertIn(key, entries)
            entry = entries[key]
            self.assertTrue(entry.get("zh"))
            self.assertTrue(entry.get("en"))
            self.assertTrue(entry.get("ja"))
            self.assertEqual(FMT_RE.findall(entry["zh"]), FMT_RE.findall(entry["en"]))
            self.assertEqual(FMT_RE.findall(entry["zh"]), FMT_RE.findall(entry["ja"]))

    def test_product_cmake_has_no_test_fi_root(self) -> None:
        harken = CMAKE.split("add_executable(HarkenSearch")[1].split("add_executable(")[0]
        self.assertIn("ARIS_PREVIEW_ISOLATION_ROOT", CMAKE)
        self.assertNotIn("preview_nas_isolation_root.h", harken)
        self.assertIn("TestPreviewNasDatabase", CMAKE)
        self.assertIn("AURO_FILESYSTEM_PROBE_PATH_TESTING", CMAKE)

    def test_scan_error_on_network_is_unreachable(self) -> None:
        one_dir = DATABASE.split("auto scanOneDirectory", 1)[1].split(
            "Begin explicit-stack traversal", 1
        )[0]
        error_body = one_dir.split("DirectoryReadStatus::Error", 1)[1].split(
            "if (readStatus != Platform::FileSystem::DirectoryReadStatus::Entry)", 1
        )[0]
        self.assertIn("IsNetworkPath", error_body)
        self.assertIn("PathStatus::Unreachable", error_body)
        self.assertIn("stats.unreachable", error_body)
        self.assertNotIn("missingUnderPrefix.push_back", error_body)

    def test_watcher_skips_remove_missing_when_unreachable(self) -> None:
        self.assertIn("NeedRescan: skip removeMissing", DATABASE)
        self.assertIn("removeMissing =", DATABASE)

    def test_main_helpers_and_db_reject(self) -> None:
        self.assertIn("ScanFoldersIncludeNetworkPath", MAIN)
        self.assertIn("Db_RejectNetworkPath", MAIN)
        self.assertIn("Write_BlockedNetworkLibrary", WINDOWS_MAIN)



    def test_scan_worker_does_not_call_l_for_nas_unreachable(self) -> None:
        body = DATABASE.split("ScanStats Database::scanDirectoryEx(", 1)[1]
        body = body.split("int uniqueCoverChanged", 1)[0]
        self.assertNotIn("L(Scan_NetworkLibraryUnreachable)", body)
        self.assertIn('"Scan_NetworkLibraryUnreachable"', body)
        self.assertGreaterEqual(body.count("stats.unreachable"), 1)

    def test_ui_maps_unreachable_key_not_localized_match(self) -> None:
        key = '"Scan_NetworkLibraryUnreachable"'
        self.assertIn("error.find(" + key + ")", MAIN)
        self.assertNotIn("error.find(L(Scan_NetworkLibraryUnreachable))", MAIN)
        self.assertIn("msg.find(" + key + ")", WINDOWS_MAIN)
        self.assertNotIn("unreachableCopy = L(Scan_NetworkLibraryUnreachable)", WINDOWS_MAIN)
        self.assertIn("L(Scan_NetworkLibraryUnreachable)", MAIN)
        result = MAIN.split("if (BeginTransientScrollbarPopupModal(scanResultPopup", 1)[1].split("// New playlist dialog", 1)[0]
        self.assertIn("L(Main_FailedFmt), scanView.failed", result)
        self.assertIn("BuildScanFailureSummary(scanErrorSnapshot, scanView.failed)", result)
        self.assertIn("L(ScanUi_FailedPath)", result)
        self.assertIn('error.find("Scan_NetworkLibraryUnreachable")', result)
        self.assertIn("L(Scan_NetworkLibraryUnreachable)", result)

    def test_scan_callback_does_not_treat_key_as_filename(self) -> None:
        for label, text in (("main", MAIN), ("windows", WINDOWS_MAIN)):
            cb = text.split("scanDb.scanDirectoryEx", 1)[1].split("scanCancelToken", 1)[0]
            self.assertIn("Scan_NetworkLibraryUnreachable", cb, label)
            self.assertIn("setScanFileKey", cb, label)

    def test_embed_ports_do_not_capture_this_or_blocked_network(self) -> None:
        body = MAIN.split("class AppEmbedMetadataBatchPorts", 1)[1]
        body = body.split("static void PumpEmbedMetadataBatch", 1)[0]
        launch = body.split("std::future<bool> launch", 1)[1]
        finish = body.split("void finish()", 1)[1].split("std::future<bool> launch", 1)[0]
        self.assertNotIn("blockedNetwork_", body)
        self.assertNotIn("[this", launch)
        self.assertLess(launch.find("IsNetworkPath"), launch.find("std::async"))
        self.assertIn("set_value(false)", launch)
        self.assertIn("embedBatchActive = false", finish)
        self.assertIn("embedBatchTargets.clear()", finish)
        self.assertNotIn("loadErrorToast", finish)


if __name__ == "__main__":
    unittest.main()
