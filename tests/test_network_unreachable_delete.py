#!/usr/bin/env python3
"""RQ-20260917-01 slice 1: Unreachable must never be deleted as Missing."""

from __future__ import annotations

import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


class NetworkUnreachableDeleteContract(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.filesystem_h = (ROOT / "src" / "platform" / "filesystem.h").read_text(
            encoding="utf-8"
        )
        cls.database = (ROOT / "src" / "Database.cpp").read_text(encoding="utf-8")
        cls.database_h = (ROOT / "src" / "Database.h").read_text(encoding="utf-8")
        cls.main_cpp = (ROOT / "src" / "main.cpp").read_text(encoding="utf-8")
        cls.windows_main = (ROOT / "src" / "WindowsMain.cpp").read_text(encoding="utf-8")

    def test_path_status_has_independent_unreachable(self) -> None:
        enum_body = self.filesystem_h.split("enum class PathStatus {", 1)[1].split(
            "};", 1
        )[0]
        self.assertIn("Unreachable", enum_body)
        self.assertIn("Missing", enum_body)
        self.assertIn("Present", enum_body)
        self.assertIn("Error", enum_body)
        self.assertNotEqual(
            enum_body.find("Unreachable"),
            enum_body.find("Missing"),
            "Unreachable must be a distinct enumerator, not an alias of Missing",
        )

    def test_full_cleanup_only_deletes_missing_and_mentions_unreachable(self) -> None:
        full_cleanup = self.database.split(
            "int Database::removeMissingFilesDatabaseWide(", 1
        )[1].split("// Map column names once", 1)[0]
        self.assertIn("Platform::FileSystem::ProbePath(", full_cleanup)
        self.assertIn("Platform::FileSystem::PathStatus::Missing", full_cleanup)
        self.assertIn("Platform::FileSystem::PathStatus::Unreachable", full_cleanup)
        self.assertNotIn("fs::exists", full_cleanup)
        missing_push = full_cleanup.split("missingPaths.push_back", 1)[0][-400:]
        self.assertIn("PathStatus::Missing", missing_push)
        self.assertNotIn("PathStatus::Unreachable", missing_push)
        self.assertNotIn("PathStatus::Error", missing_push)

    def test_scan_remove_missing_skips_unreachable_and_error(self) -> None:
        scan_cleanup = self.database.split(
            "if (removeMissing && !wasCancelled", 1
        )[1].split("int uniqueCoverChanged", 1)[0]
        self.assertIn("Platform::FileSystem::ProbePath(", scan_cleanup)
        self.assertIn("Platform::FileSystem::PathStatus::Missing", scan_cleanup)
        self.assertIn("Platform::FileSystem::PathStatus::Unreachable", scan_cleanup)
        push = scan_cleanup.split("missingUnderPrefix.push_back", 1)[0][-400:]
        self.assertIn("PathStatus::Missing", push)
        self.assertNotIn("PathStatus::Unreachable", push)
        self.assertNotIn("PathStatus::Error", push)

    def test_scan_directory_entry_short_circuits_unreachable_root(self) -> None:
        entry = self.database.split("ScanStats Database::scanDirectoryEx(", 1)[1].split(
            "const bool sessionOwnedByThisCall", 1
        )[0]
        self.assertIn("Platform::FileSystem::PathStatus::Unreachable", entry)
        self.assertIn("ProbePath(", entry)
        # Early return must happen before removeMissing collection.
        self.assertIn("return stats;", entry)

    def test_scan_stats_can_report_unreachable_count(self) -> None:
        stats = self.database_h.split("struct ScanStats {", 1)[1].split("\n};", 1)[0]
        self.assertIn("unreachable", stats)
        merge = stats.split("void mergeFrom", 1)[1]
        self.assertIn("unreachable", merge)

    def test_watcher_rescan_skips_delete_on_unreachable_root(self) -> None:
        worker = self.database.split(
            "void Database::watcherRescanWorkerThread()", 1
        )[1].split("void Database::startFileSystemWatcher(", 1)[0]
        self.assertIn("scanDirectoryEx(root, true, true,", worker)
        self.assertIn("Platform::FileSystem::PathStatus::Unreachable", worker)
        self.assertIn("ProbePath(", worker)

    def test_scan_ui_uses_unreachable_copy_not_cleaned_count(self) -> None:
        for source, label in (
            (self.main_cpp, "main.cpp"),
            (self.windows_main, "WindowsMain.cpp"),
        ):
            with self.subTest(file=label):
                self.assertIn(
                    "Scan_NetworkLibraryUnreachable",
                    source,
                    f"{label} must surface the unreachable library status",
                )


if __name__ == "__main__":
    unittest.main()
