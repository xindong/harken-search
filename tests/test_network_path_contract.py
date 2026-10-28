#!/usr/bin/env python3
"""RQ-20260917-01 slice 2: readonly NAS ingest contracts."""

from __future__ import annotations

import json
import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
HEADER = (ROOT / "src/platform/filesystem.h").read_text(encoding="utf-8")
DATABASE_H = (ROOT / "src/Database.h").read_text(encoding="utf-8")
DATABASE = (ROOT / "src/Database.cpp").read_text(encoding="utf-8")
MAIN = (ROOT / "src/main.cpp").read_text(encoding="utf-8")
WINDOWS_MAIN = (ROOT / "src/WindowsMain.cpp").read_text(encoding="utf-8")
METADATA = (ROOT / "src/MetadataWriter.cpp").read_text(encoding="utf-8")
AI_ENGINE = (ROOT / "src/AIEngine.cpp").read_text(encoding="utf-8")
MODULE_UI = (ROOT / "src/ModuleUI.cpp").read_text(encoding="utf-8")
LEDGER = json.loads((ROOT / "locale/harken.ui.strings.json").read_text(encoding="utf-8"))
BY_ID = {entry["id"]: entry for entry in LEDGER["entries"]}
CJK_RE = re.compile(r"[一-鿿]")
FMT_RE = re.compile(r"%(?:ll)?[sd]")


class NetworkPathContract(unittest.TestCase):
    def test_header_declares_is_network_path(self) -> None:
        self.assertIn("bool IsNetworkPath(const std::string& pathUtf8);", HEADER)

    def test_database_tracks_network_library_roots(self) -> None:
        self.assertIn(
            "bool isNetworkLibraryRoot(const std::string& pathUtf8) const;",
            DATABASE_H,
        )
        self.assertIn(
            "bool isNetworkLibraryFile(const std::string& pathUtf8) const;",
            DATABASE_H,
        )
        merge = DATABASE.split("void Database::mergeLibraryRoots(", 1)[1].split(
            "std::vector<std::string> Database::getLibraryRoots()", 1
        )[0]
        self.assertIn("IsNetworkPath", merge)
        self.assertIn("network_library_roots", merge)
        self.assertIn("CREATE TABLE IF NOT EXISTS network_library_roots", DATABASE)

    def test_scan_dialog_marks_readonly_network_library(self) -> None:
        dialog = MAIN.split("ImGui::Checkbox(L(Main_IncludeSubfolders)", 1)[1].split(
            "auto startScanForFolders", 1
        )[0]
        self.assertIn("Scan_NetworkLibraryReadonlyHint", dialog)
        self.assertIn("Scan_NetworkDualPathWarn", dialog)
        self.assertIn("Scan_MappedDriveHiddenHint", dialog)
        self.assertIn("IsNetworkPath", dialog)
        self.assertIn("scanRemoveMissing = false", dialog)
        self.assertIn("BeginDisabled", dialog)
        start = MAIN.split("auto startScanForFolders", 1)[1].split(
            "DIAG_SYSTEM_FMT(\"Scan\", \"ScanStart:", 1
        )[0]
        self.assertIn("IsNetworkPath", start)
        self.assertIn("removeMissing = false", start)
        self.assertIn("IsNetworkPath", WINDOWS_MAIN)
        self.assertIn("removeMissing = false", WINDOWS_MAIN)
        self.assertIn("mergeLibraryRoots", WINDOWS_MAIN)
        self.assertIn("Lib_ReadonlyNetwork", MAIN)

    def test_slice2_locale_keys_are_trilingual(self) -> None:
        expected_zh = {
            "Scan_NetworkLibraryReadonlyHint": "这是只读网络库，不会修改网络共享上的文件。",
            "Scan_NetworkDualPathWarn": "不要同时用盘符和网络地址添加同一共享，否则会当成两套库。",
            "Scan_MappedDriveHiddenHint": "当前以管理员运行，映射盘可能不可见，请改用网络地址。",
            "Lib_ReadonlyNetwork": "只读库",
            "Write_BlockedNetworkLibrary": "只读网络库不能修改源文件。",
            "Db_RejectNetworkPath": "数据库不能放在网络路径上，否则可能损坏。",
        }
        for key, zh in expected_zh.items():
            entry = BY_ID.get(key)
            self.assertIsNotNone(entry, f"missing ledger key {key}")
            assert entry is not None
            self.assertEqual(entry["zh"], zh)
            self.assertTrue(entry["en"].strip())
            self.assertTrue(entry["ja"].strip())
            self.assertFalse(CJK_RE.search(entry["en"]), f"{key} en contains CJK")
            self.assertNotEqual(entry["ja"], entry["zh"], f"{key} ja must not be leftover Chinese")
            self.assertEqual(FMT_RE.findall(entry["zh"]), FMT_RE.findall(entry["en"]))
            self.assertEqual(FMT_RE.findall(entry["zh"]), FMT_RE.findall(entry["ja"]))

    def test_metadata_writer_checks_network_library(self) -> None:
        self.assertIn("IsNetworkPath", METADATA)
        self.assertIn("Write_BlockedNetworkLibrary", METADATA)
        write_art = METADATA.split("bool MetadataWriter::writeAlbumArt(", 1)[1].split(
            "bool MetadataWriter::writeAlbumArtFromFile(", 1
        )[0]
        self.assertIn("IsNetworkPath", write_art)
        write_fields = METADATA.split(
            "bool MetadataWriter::writeMetadataFields(", 1
        )[1].split("int MetadataWriter::writeMetadataFieldsBatch(", 1)[0]
        self.assertIn("IsNetworkPath", write_fields)

    def test_ai_cover_rejects_network_folder(self) -> None:
        self.assertIn("IsNetworkPath", AI_ENGINE)
        self.assertIn("Write_BlockedNetworkLibrary", AI_ENGINE)
        choose = AI_ENGINE.split("static bool ChooseUnusedCoverJpegPath", 1)[1].split(
            "int AIEngine::GenerateCoverAsync", 1
        )[0]
        self.assertIn("IsNetworkPath", choose)

    def test_transfer_rejects_network_destination(self) -> None:
        self.assertIn("IsNetworkPath", MODULE_UI)
        self.assertIn("Write_BlockedNetworkLibrary", MODULE_UI)
        start = MAIN.split("if (ImGui::Button(L(Export_Start)", 1)[1].split(
            "state.transferInProgress = true;", 1
        )[0]
        self.assertIn("IsNetworkPath", start)
        self.assertIn("Write_BlockedNetworkLibrary", start)

    def test_database_open_rejects_network_path(self) -> None:
        body = DATABASE.split("bool Database::open(", 1)[1].split("sqlite3_open", 1)[0]
        self.assertIn("IsNetworkPath", body)
        self.assertIn("Db_RejectNetworkPath", body)
        self.assertIn("IsNetworkPath", MAIN)
        new_db = MAIN.split("if (state.showNewDbDialog)", 1)[1].split(
            "DAW Target Configuration Dialog", 1
        )[0]
        self.assertIn("IsNetworkPath", new_db)
        self.assertIn("Db_RejectNetworkPath", new_db)

    def test_cover_writeback_callers_surface_blocked_copy(self) -> None:
        apply_pending = MAIN.split(
            "static void ApplyPendingOriginalAudioWriteUnlocked", 1
        )[1].split("static void ApplyPendingOriginalAudioWrite(", 1)[0]
        self.assertIn("Platform::FileSystem::IsNetworkPath(path)", apply_pending)
        self.assertIn("state.loadErrorToast = L(Write_BlockedNetworkLibrary)", apply_pending)
        self.assertIn("state.loadErrorToastUntil = ImGui::GetTime()", apply_pending)
        self.assertLess(apply_pending.index("IsNetworkPath(path)"),
                        apply_pending.index("MetadataWriter::writeAlbumArt("))
        embed_launch = MAIN.split(
            "std::future<bool> launch(aura::metadata::EmbedMetadataTask task)", 1
        )[1].split("private:", 1)[0]
        self.assertIn("Write_BlockedNetworkLibrary", embed_launch)
        cover_worker = WINDOWS_MAIN.split(
            "for (const auto& path : task.filePaths)", 1
        )[1].split("EnsureCoverTaskWorkerStarted", 1)[0]
        self.assertIn("Write_BlockedNetworkLibrary", cover_worker)
        self.assertIn("IsNetworkPath", cover_worker)

    def test_windows_main_start_export_rejects_network_destination(self) -> None:
        export = WINDOWS_MAIN.split('} else if (cmd == "start_export")', 1)[1].split(
            '} else if (cmd == "cancel_export")', 1
        )[0]
        self.assertIn("IsNetworkPath", export)
        self.assertIn("Write_BlockedNetworkLibrary", export)
        mkdir_idx = export.find("CreateDirectories")
        block_idx = export.find("IsNetworkPath")
        self.assertGreater(mkdir_idx, 0)
        self.assertGreater(block_idx, 0)
        self.assertLess(block_idx, mkdir_idx)

    def test_ai_cover_keeps_blocked_copy_without_retired_product_entry(self) -> None:
        generate = AI_ENGINE.split(
            "int AIEngine::GenerateCoverAsync", 1
        )[1].split("std::vector<int> AIEngine::BatchTranscribe", 1)[0]
        self.assertIn("IsNetworkPath", generate)
        self.assertIn("Write_BlockedNetworkLibrary", generate)
        self.assertNotIn("m_taskQueue.push", generate.split("if (Platform::FileSystem::IsNetworkPath", 1)[1])
        execute = AI_ENGINE.split("void AIEngine::ExecuteTask", 1)[1].split(
            "case AITaskType::CoverGeneration:", 1
        )[1].split("task.progress = 1.0f;", 1)[0]
        self.assertIn("Write_BlockedNetworkLibrary", execute)
        self.assertIn("IsNetworkPath", execute)
        # The SiliconFlow generation entry was retired; the dormant engine
        # boundary still has to reject network destinations if called.
        self.assertNotIn("GenerateCoverAsync", MAIN)
        self.assertNotIn("state.showAICoverStudio = true", MAIN)
        self.assertNotIn('ProductEdition::hasFeature("ai_cover_gen")', MAIN)

    def test_tag_edit_product_path_surfaces_blocked_copy(self) -> None:
        patch_batch = DATABASE.split(
            "BatchMetadataApplyResult Database::applyMetadataPatchBatch(", 1
        )[1].split("BatchMetadataApplyResult Database::applyMetadataPatchMany(", 1)[0]
        self.assertIn("IsNetworkPath", patch_batch)
        self.assertIn("Write_BlockedNetworkLibrary", patch_batch)
        set_tags = DATABASE.split("bool Database::setFileTags(", 1)[1].split(
            "std::vector<std::string> Database::getFileTags(", 1
        )[0]
        self.assertIn("IsNetworkPath", set_tags)
        self.assertIn("Write_BlockedNetworkLibrary", set_tags)
        inline = MAIN.split("static bool SaveInfoMetadataDraft(", 1)[1].split(
            "void RenderInfoMetadataLeavePrompt(", 1
        )[0]
        self.assertIn("Write_BlockedNetworkLibrary", inline)
        self.assertLess(inline.index("IsNetworkPath(original.filePath)"),
                        inline.index("Platform::FileSystem::Exists(original.filePath)"))
        prompt = MAIN.split("void RenderInfoMetadataLeavePrompt(", 1)[1].split(
            "void RenderFileInfoPanelTabContents(", 1
        )[0]
        self.assertIn("else if (state.infoEditError.empty())", prompt)

    def test_scan_directory_checks_cancel_around_network_root_probe(self) -> None:
        scan = DATABASE.split("ScanStats Database::scanDirectoryEx(", 1)[1].split(
            "const bool sessionOwnedByThisCall", 1
        )[0]
        self.assertIn("cancelFlag", scan)
        self.assertIn("ProbePath", scan)
        probe_idx = scan.find("ProbePath")
        cancel_idx = scan.find("cancelFlag")
        self.assertGreater(probe_idx, 0)
        self.assertGreater(cancel_idx, 0)
        self.assertLess(cancel_idx, probe_idx)
        after_probe = scan[probe_idx:]
        self.assertIn("cancelFlag", after_probe)
        self.assertIn("stats.cancelled = true", after_probe)

    def test_scan_next_error_on_network_path_is_unreachable_not_missing(self) -> None:
        one_dir = DATABASE.split("auto scanOneDirectory", 1)[1].split(
            "Begin explicit-stack traversal", 1
        )[0]
        self.assertIn("DirectoryReadStatus::Error", one_dir)
        self.assertIn("IsNetworkPath", one_dir)
        error_body = one_dir.split("DirectoryReadStatus::Error", 1)[1].split(
            "if (readStatus != Platform::FileSystem::DirectoryReadStatus::Entry)", 1
        )[0]
        self.assertIn("IsNetworkPath", error_body)
        self.assertIn("PathStatus::Unreachable", error_body)
        self.assertIn("Scan_NetworkLibraryUnreachable", error_body)
        self.assertIn("stats.unreachable", error_body)
        self.assertNotIn("missingUnderPrefix.push_back", error_body)

    def test_scan_cancel_clears_scanning_flag(self) -> None:
        self.assertIn("requestCancelScanTask", MAIN)
        self.assertIn("scanWasCancelled", MAIN)
        finish = MAIN.split("state.scanWasCancelled = rollbackCancelled", 1)[1]
        self.assertIn("isScanning = false", finish)

    def test_network_root_probe_timeout_does_not_destroy_unready_future(self) -> None:
        scan = DATABASE.split("ScanStats Database::scanDirectoryEx(", 1)[1].split(
            "const bool sessionOwnedByThisCall", 1
        )[0]
        self.assertNotIn("std::async", scan)
        self.assertNotIn("std::future", scan)
        self.assertIn("std::thread", scan)
        self.assertIn("detach()", scan)
        self.assertIn("std::atomic", scan)
        self.assertIn("ProbePath", scan)
        self.assertIn("chrono::seconds(8)", scan)


if __name__ == "__main__":
    unittest.main()
